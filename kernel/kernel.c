/* ============================================================
 * kernel.c — PC-9801 32ビットOS カーネル
 * プロテクトモードで動作、テキストVRAMに直接書き込む
 * リニアアドレス 0x100000 (1MB) に配置される
 *
 * Phase 2+: IDT/PIC/PIT + キーボード + ミニシェル
 * ============================================================ */

#include "idt.h"
#include "tss.h"
#include "v86.h"
#include "io.h"
#include "kbd.h"
#include "fdc.h"
#include "disk.h"
#include "dev.h"
#include "path.h"
/* fs.h 削除 — レガシーFS廃止 */
#include "rtc.h"
#include "kmalloc.h"
#include "palette.h"
#include "gfx.h"
#include "gfx_hal.h"   /* H2b: GFX_PREF_* / gfx_set_backend_pref (GFX= の反映先) */
#include "kcg.h"
#include "boot_splash.h"
#include "cpu_calibrate.h"
#include "paging.h"
#include "pgalloc.h"
#include "shlib.h"
#include "hotdeploy.h"
#include "shm.h"
#include "utf8.h"
#include "kselftest.h"
#include "exec.h"
#include "ide.h"
#include "atapi.h"
#include "vfs.h"
#include "ext2.h"
#include "iso9660.h"
#include "loop_dev.h"

/* FatFs VFSドライバ (fs/fatfs_vfs.c) */
extern void fatfs_init(void);

#include "hostdrvfs.h"
#include "tvram.h"
#include "pc98.h"
#include "memmap.h"
#include "config.h"
#include "sysconfig.h"
#include "kstring.h"
#include "sys.h"
#include "ime.h"
#include "fd_redirect.h"
#include "pipe_buffer.h"
#include "snd_engine.h"
#include "mouse.h"
#include "lgy98.h"
#include "os32_sqlite_vfs.h"
#include "kapi_db.h"

#define SHELL_RELOAD_DELAY 10

/* tvram_clear は console.c 側に実装済みのため削除 */
extern void console_hw_cursor_enable(void);   /* console.c */
extern void console_text_gdc_stop(void);      /* console.c (K4): GUI 時にカーソルを消す */
extern void console_text_gdc_start(void);     /* console.c (K4): CUI 復帰でカーソル再表示 */

/* 次に起動するシェルのパス (契約 T9)。実体は kernel/gui.c (K1)。
 * gui.h を丸ごと取り込むと os32_gui_shared.h まで引くので、
 * 起動ループが使う 1 本だけを extern 宣言する。 */
extern int gui_take_next_shell(char *out, int cap);

/* 文字列表示 */
static void tvram_print(int x, int y, const char *str, u8 color)
{
    u32 offset;
    while (*str) {
        if (x >= TVRAM_COLS) { x = 0; y++; }
        if (y >= TVRAM_ROWS) break;
        offset = (u32)y * TVRAM_BPR + (u32)x * 2;
        *(volatile u16 *)(TVRAM_BASE + offset) = (u16)(u8)*str;
        *(volatile u8 *)(TVRAM_ATTR + offset) = color;
        str++; x++;
    }
}

/* dev_findをPathDeviceValidatorとして使用するラッパー */
static int dev_find_validator(const char *name)
{
    return (dev_find(name) != (void *)0) ? 1 : 0;
}

/* ============================================================
 * カーネルメイン — エントリポイント
 * ============================================================ */
extern u32 sys_mem_kb;


void __cdecl kernel_main(u32 mem_kb, u32 boot_drive)
{
    char tmp[16];
    int mb = mem_kb / 1024;
    sys_mem_kb = mem_kb;
    
    tvram_clear();
    console_hw_cursor_enable();   /* GDC カーソル表示 ON (以後は出力に追従) */

    tvram_print(0, 0, "PC-9801 OS32 booting...", TATTR_CYAN);
    
    /* メモリ量表示 (例: " (16MB)") */
    {
        int d10 = (mb / 10) % 10;
        int d1  = mb % 10;
        tmp[0] = ' '; tmp[1] = '(';
        tmp[2] = d10 ? ('0' + d10) : ' '; /* 先行ゼロ抑制 */
        tmp[3] = '0' + d1;
        tmp[4] = 'M'; tmp[5] = 'B'; tmp[6] = ')'; tmp[7] = '\0';
    }
    tvram_print(24, 0, tmp, TATTR_CYAN);

    /* GDT 再構築 (ブートローダーのGDTから安全な場所へ移行) */
    tvram_print(0, 1, "GDT...", TATTR_GREEN);
    extern void gdt_init(void);
    gdt_init();
    tvram_print(6, 1, "OK  ", TATTR_WHITE);

    /* TSS (V86 モードに必須: #GP 時の Ring0 スタックと I/O 許可ビットマップ)
     * GDT のエントリ3を使うので gdt_init の直後に呼ぶ。
     * ESP0 は V86 突入直前に v86_enter が実際の ESP へ差し替える。 */
    tss_init(MEM_KSTACK_TOP);

    /* IDT/PIC/PIT 初期化 */
    tvram_print(12, 1, "IDT...", TATTR_GREEN);
    idt_init();
    tvram_print(18, 1, "OK  ", TATTR_WHITE);



    tvram_print(24, 1, "PIC...", TATTR_GREEN);
    pic_init();
    tvram_print(30, 1, "OK  ", TATTR_WHITE);

    tvram_print(36, 1, "PIT...", TATTR_GREEN);
    pit_init(PIT_HZ);
    tvram_print(42, 1, "OK  ", TATTR_WHITE);

    /* タイマとカスケード有効化 */
    irq_enable(0);
    irq_enable(ICU_SLAVEID);

    /* CPU速度キャリブレーション (PITベース) */
    tvram_print(0, 2, "CPU...", TATTR_GREEN);
    _enable();  /* タイマー割り込み許可 (キャリブレーションに必要) */
    cpu_calibrate();
    {
        u32 lpt = cpu_loops_per_tick();
        int k = (int)(lpt / 1000);
        char cpubuf[12];
        int pos = 0;
        /* 簡易数値→文字列変換 (0-999K) */
        if (k >= 100) { cpubuf[pos++] = '0' + (k / 100) % 10; }
        if (k >= 10)  { cpubuf[pos++] = '0' + (k / 10) % 10; }
        cpubuf[pos++] = '0' + k % 10;
        cpubuf[pos++] = 'K';
        cpubuf[pos] = '\0';
        tvram_print(6, 2, cpubuf, TATTR_WHITE);
    }

    /* キーボード初期化 */
    tvram_print(48, 1, "KBD...", TATTR_GREEN);
    kbd_init();
    tvram_print(54, 1, "OK", TATTR_WHITE);

    /* マウスドライバ初期化 (NP21/W検出→モード自動選択) */
    mouse_init();

    /* FDC初期化 (I/Oポート直接制御) */
    tvram_print(60, 1, "FDC...", TATTR_GREEN);
    /* IRQ11 (FDD) 有効化 */
    irq_enable(FDC_IRQ);
    _enable();  /* FDC初期化前にIRQを許可 */
    {
        int fdc_ret = fdc_init();
        if (fdc_ret == 0) {
            tvram_print(66, 1, "OK", TATTR_WHITE);
        } else {
            tvram_print(66, 1, "ER", TATTR_RED);
        }
    }

    /* デバイス・パスシステム初期化 */
    tvram_print(58, 1, "DEV...", TATTR_GREEN);
    dev_init();
    loop_dev_init();  /* lo0..lo3 ループバックデバイス登録 */
    path_init();
    ext2_init();
    fatfs_init();
    vfs_register_fs(&iso9660_ops);

    hostdrvfs_init();
    path_set_device_validator(dev_find_validator);
    tvram_print(64, 1, "OK", TATTR_WHITE);

    /* パレット初期化 */
    palette_init();


    /* IDE/HDD 初期化と登録 (4ドライブ: IDE#0-#3) */
    tvram_print(0, 3, "IDE...", TATTR_GREEN);
    {
        int n = ide_init();
        if (n > 0) {
            int d;
            tvram_print(5, 3, "OK", TATTR_WHITE);
            for (d = 0; d < 4; d++) {
                if (ide_drive_present(d)) dev_register_hdd(d);
            }
        } else {
            tvram_print(5, 3, "no drive", TATTR_CYAN);
        }
    }

    /* ATAPI CD-ROM 検出 */
    {
        int cd = atapi_init();
        if (cd > 0) {
            tvram_print(14, 3, "CD", TATTR_WHITE);
            dev_register_cdrom();
        }
    }

    /* ヒープ初期化 (VFSマウントでkmalloc使用のため、マウント前に必要) */
    tvram_print(24, 2, "HEAP...", TATTR_GREEN);
    kmalloc_init((void *)KHEAP_BASE, KHEAP_SIZE);
    tvram_print(31, 2, "320K", TATTR_WHITE);

    /* 自動マウント処理 */
    tvram_print(14, 3, "MOUNT...", TATTR_GREEN);
    {
        int i, rc;
        /* MAX_DEVICES 分確保する。lo0-lo3 が hd0/cd0 より先に登録されるため、
         * 固定8個だと後続デバイスが自動マウント対象から漏れる。 */
        const char *names[MAX_DEVICES];
        int num = dev_get_names(names, MAX_DEVICES);
        const char *root_dev;
        const char *root_fs;

        if (boot_drive == BOOT_DRIVE_FDD || boot_drive == BOOT_DRIVE_FDD_144) {
            root_dev = "fd0";
            root_fs = "fat";
        } else {
            root_dev = "hd0";
            root_fs = "ext2";
        }

        /* 1. ルートマウント */
        rc = vfs_mount("/", root_dev, root_fs);
        if (rc != VFS_OK) {
            tvram_print(23, 3, "root panic", TATTR_RED);
            for (;;) asm volatile("hlt");
        }
        tvram_print(23, 3, "root OK", TATTR_WHITE);

        /* 2. サブマウント (ルート以外) */
        for (i = 0; i < num; i++) {
            const char *dname = names[i];
            char mnt[VFS_MNTPATH_MAX];
            int j;
            Device *d = dev_find(dname);

            if (!d || d->type != DEV_BLOCK) continue;

            /* ルートと一致する場合はスキップ */
            {
                int match = 1;
                for (j = 0; root_dev[j] || dname[j]; j++) {
                    if (root_dev[j] != dname[j]) { match = 0; break; }
                }
                if (match) continue;
            }

            /* パス生成: "/dname/" */
            mnt[0] = '/';
            for (j = 0; dname[j] && j < VFS_MNTPATH_MAX - 4; j++) {
                mnt[1 + j] = dname[j];
            }
            mnt[1 + j] = '\0';

            /* フォールバックしながらマウント試行 */
            {
                int mounted = 0;
                /* CDデバイスにはiso9660のみ試行 */
                if (dname[0] == 'c' && dname[1] == 'd') {
                    if (vfs_mount(mnt, dname, "iso9660") == VFS_OK) mounted = 1;
                } else {
                    if (vfs_mount(mnt, dname, "ext2") == VFS_OK) mounted = 1;
                    if (!mounted) {
                        if (vfs_mount(mnt, dname, "iso9660") == VFS_OK) mounted = 1;
                    }
                    if (!mounted) {
                        vfs_mount(mnt, dname, "fat");
                    }
                }
            }
        }

        /* 3. HostDrv(NT) 自動マウント (/host) */
        if (hostdrvfs_detect()) {
            int hrc = vfs_mount("/host", "hostdrv", "hostdrv");
            if (hrc == VFS_OK) {
                tvram_print(32, 3, "HDRV OK", TATTR_WHITE);
            } else {
                tvram_print(32, 3, "HDRV NG", TATTR_RED);
            }
        } else {
            tvram_print(32, 3, "HDRV --", TATTR_CYAN);
        }
    }

    /* RTC初期化 */
    tvram_print(26, 3, "RTC...", TATTR_GREEN);
    rtc_init();
    tvram_print(32, 3, "OK", TATTR_WHITE);


    /* ページング初期化 (メモリ保護有効化) */
    tvram_print(37, 2, "PAGE...", TATTR_GREEN);
    paging_init(mem_kb);
    tvram_print(44, 2, "OK", TATTR_WHITE);

    /* コンベンショナルメモリ再利用 (ブート後のローダー領域等を解放) */
    paging_reclaim_conventional();

    /* KCGフォントキャッシュ初期化
     * コンベンショナルメモリ (0x01000〜) にキャッシュを配置しているため、
     * paging_reclaim 後にフラグをゼロクリアする必要がある。
     * これがないとBIOSデータの残骸がキャッシュ済みと誤認される。 */
    kcg_init();

    /* [DEBUG] カーネル初期化中にフォントロードをテスト。
     * 外部プログラム経由のクラッシュが、スタック/コンテキストの問題か
     * lz4_decode 自体の問題かを切り分けるための一時的なテスト。
     * 注意: Unicode テーブル (0x4A000) はまだロードされていないので
     *        LZ4 一時バッファとして安全に使用可能。 */
    tvram_print(0, 3, "FONT..", TATTR_GREEN);
    {
        int fret;
        fret = kcg_load_font("/sys/font/default.kcgfont");
        if (fret == 0) {
            tvram_print(6, 3, "OK ", TATTR_WHITE);
        } else {
            tvram_print(6, 3, "NG ", TATTR_RED);
            kprintf(0x07, "[KCG] kernel-init load failed: %d\n", fret);
        }
    }

    /* TTF由来フォント: 外部プログラムから kcg_load_font() (KAPI) で呼ぶ */

    /* FPU 初期化 (paging_init の後で CR0 に対して設定)
     * CR0.EM=0 (ネイティブFPU使用), CR0.TS=0 (タスクスイッチ不要)
     * SQLite が double 演算を使用するため必須 */
    {
        u32 cr0_val;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
        cr0_val &= ~((u32)0x0C);  /* EM(bit2) と TS(bit3) をクリア */
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val) : "memory");
        __asm__ volatile("fninit");
    }

    /* 物理ページフレームアロケータ初期化 (paging_initの後) */
    pgalloc_init(mem_kb);

    /* 共有メモリ初期化 (ガードページ設定 + R/W設定) */
    shm_init();

    /* ホットデプロイ制御ブロック初期化 (ホストが番地を読むので早めに) */
    hotdeploy_init();

    /* カーネル内プリミティブの自己診断。
     * 外部プログラムの klibc_test は newlib 側を試すだけで、カーネルが
     * 実際に使う kstring_asm / kmalloc / kprintf は一度も踏んでいない。
     * ここで境界ケースだけを実機で毎回確認する (全通過なら 1 行)。 */
    kselftest_run();

    /* FDリダイレクト初期化 (プログラムローダーより前に) */
    fd_redirect_init();

    /* パイプバッファ初期化 */
    pipe_buffer_init();

    /* プログラムローダー初期化 (KernelAPIテーブル構築) */
    tvram_print(48, 2, "EXEC...", TATTR_GREEN);
    exec_init();
    tvram_print(56, 2, "OK", TATTR_WHITE);

    /* Unicodeテーブルロード */
    tvram_print(60, 2, "UNI...", TATTR_GREEN);
    kprintf(0x07, "[BOOT] UNI load: dst=%x size=%x\n",
            (unsigned)MEM_UNICODE_TABLE_BASE, (unsigned)MEM_UNICODE_TABLE_SIZE);
    {
        int bytes = vfs_read(SYS_UNICODE_BIN, (void *)MEM_UNICODE_TABLE_BASE, MEM_UNICODE_TABLE_SIZE);
        kprintf(0x07, "[BOOT] UNI load: bytes=%d\n", bytes);
        if (bytes == (int)MEM_UNICODE_TABLE_SIZE) {
            utf8_set_jis_table_ready(1);
            tvram_print(67, 2, "OK", TATTR_WHITE);
        } else {
            /* 表を有効化しない。ロード失敗のまま引くと、0x4A000 に
             * 残っていた別のデータを変換表として読んでしまう。 */
            utf8_set_jis_table_ready(0);
            tvram_print(67, 2, "ER", TATTR_RED);
        }
    }

    /* IME (FEP) 初期化 — 辞書は初回使用時に遅延ロード */
    tvram_print(0, 4, "IME..", TATTR_GREEN);
    kprintf(0x07, "[BOOT] ime_init...\n");
    ime_init();
    tvram_print(5, 4, "OK ", TATTR_WHITE);
    kprintf(0x07, "[BOOT] ime_init OK\n");

    /* サウンドエンジン初期化 */
    tvram_print(9, 4, "SND..", TATTR_GREEN);
    kprintf(0x07, "[BOOT] snd_init...\n");
    snd_init();
    tvram_print(14, 4, "OK ", TATTR_WHITE);
    kprintf(0x07, "[BOOT] snd_init OK\n");

    /* LAN (LGY-98)。CONFIG_LGY98_BASE=0 (既定) なら何もしない。
     * カード未装着・応答不良でも NIC だけ無効にして起動を続ける。 */
    lgy98_init();

    tvram_print(18, 4, "SQ..", TATTR_GREEN);
    kprintf(0x07, "[BOOT] sqlite_init...\n");

    /* SQLite エンジン初期化 (ブートローダーが 0x200000 にロード済み、
     * kentry.asm が .sqlite_bss をゼロクリア済み) */
    tvram_print(0, 4, "SQLite...", TATTR_GREEN);
    {
        int sq_rc;
        sq_rc = os32_sqlite_init();
        kprintf(0x07, "[SQ] init rc=%d\n", sq_rc);
        if (sq_rc == 0) {
            tvram_print(9, 4, "OK", TATTR_WHITE);
            /* 代替スタック(128KB)でSQLiteテスト実行 */
            {
                extern int os32_sqlite_test(void);
                extern int sqlite_call_on_alt_stack(
                    int (*func)(void), u32 stack_top);
                int trc = sqlite_call_on_alt_stack(
                    os32_sqlite_test, MEM_SQLITE_STACK_TOP);
                kprintf(0x07, "[SQ] kernel test rc=%d\n", trc);
            }
        } else {
            tvram_print(9, 4, "ER", TATTR_RED);
        }
    }

    /* 割り込み有効化 */
    tvram_print(71, 2, "IRQ_EN", TATTR_YELLOW);
    _enable();

    /* autoexec: シェルスクリプト(/etc/autoexec.bat)に移行済み。
     * シェル起動後に ui.c から script_source_file() で実行される。 */

    /* グラフィクスバックエンドの強制指定 (票 H2b、契約 G5)。
     * /etc/system.cfg の GFX= を **最初の gfx_init より前** に読んで HAL へ
     * 渡す (この直後の boot_splash が gfx_init を呼ぶ)。
     *   GFX=pc98  9801 プレーン強制 (NP21/W でプレーン経路を回帰試験する)
     *   GFX=pegc  PEGC 強制 (probe が通らなければ 9801)
     *   未指定    auto = probe 順 (PEGC → 9801)
     * K4 の GUI= と同じ流儀: ここで読み、サブシステム側の変数へ渡す。 */
    {
        char gfxmode[16];
        if (sysconfig_get_str(SYS_SYSTEM_CFG, "GFX", gfxmode, (int)sizeof(gfxmode)) > 0) {
            if (kstrcmp(gfxmode, "pc98") == 0) {
                gfx_set_backend_pref(GFX_PREF_PC98);
            } else if (kstrcmp(gfxmode, "pegc") == 0) {
                gfx_set_backend_pref(GFX_PREF_PEGC);
            } else {
                gfx_set_backend_pref(GFX_PREF_AUTO);   /* auto / 未知の値 */
            }
        }
    }

    /* ブートスプラッシュ表示 (カーネル内蔵) */
    boot_splash();

    /* 共有ライブラリ (0x400000 帯) を常駐させる — シェルを載せる **前** に
     * 1 回だけ (票 K3)。ここより後だと pgalloc が帯域のページを配ってしまう。
     * ライブラリが無ければ静かに未ロードで続行し、CUI は従来どおり動く。 */
    shlib_init();

    /* 外部シェル起動 — CUI(shell.bin) と GUI(gshell.bin) を同じシェル帯
     * (0x300000, Level 1) で入れ替えながら回す (契約 T9)。両者は同時に
     * 載らない。終了/クラッシュ時のフォールバックとして再起動ループにする。
     *
     * 起動シェルの決定順:
     *   1. シェルが sys_switch_shell() で残した切替要求 (gui_take_next_shell
     *      で取り出して消す。consume 方式、レビュー #3 ④)。
     *   2. 記録が無ければ、起動時に読んだ /etc/system.cfg の GUI= に従う
     *      (GUI=1 → gshell、既定 → shell)。 */
    {
        const char *cur_shell = SYS_SHELL_BIN;
        char consumed[OS32_MAX_PATH];   /* 取り出した切替パス (cur_shell が指す) */
        int gui_fault_streak = 0;       /* gshell 連続クラッシュ回数 */
        consumed[0] = '\0';

        /* 起動時: system.cfg の GUI=1 なら gshell を既定にする */
        if (sysconfig_get_int(SYS_SYSTEM_CFG, "GUI", 0) == 1) {
            cur_shell = SYS_GSHELL_BIN;
        }

        for (;;) {
            int rc;
            int is_gui;

            /* 切替要求を取り出して消す (consume 方式、レビュー #3 ④)。
             * 取り出した後は要求が空になるので、gshell のロード失敗や連続
             * fault で CUI に落ちた後でも、次の os32gui が普通に採用される。 */
            if (gui_take_next_shell(consumed, OS32_MAX_PATH)) {
                cur_shell = consumed;
                gui_fault_streak = 0;   /* 別シェルへ切替 → 連続失敗をリセット */
            }

            is_gui = (kstrcmp(cur_shell, SYS_GSHELL_BIN) == 0);

            /* GUI は全画面 GFX を握るのでテキストカーソルを消す。CUI は出す。 */
            if (is_gui) {
                console_text_gdc_stop();
                tvram_print(0, 0, "Loading gshell...", TATTR_GRAY);
            } else {
                console_text_gdc_start();
                tvram_print(0, 0, "Loading shell...", TATTR_GRAY);
            }

            rc = exec_run(cur_shell);

            /* gshell がロードできない (未存在、不正バイナリ等) → 警告して CUI に
             * 落ちる。fault (実行後のクラッシュ) は別扱い。要求は取り出し済みで
             * 空なので、修復後の os32gui は普通に採用される。 */
            if (is_gui && rc < 0 && rc != EXEC_ERR_FAULT) {
                console_text_gdc_start();
                tvram_clear();
                tvram_print(0, 0, "gshell load failed -> CUI shell", TATTR_RED);
                cur_shell = SYS_SHELL_BIN;
                gui_fault_streak = 0;
                continue;   /* ウェイト無しで即 CUI を起動 */
            }

            /* gshell がクラッシュ (fault_kill) を繰り返すなら CUI に落とす保険。
             * 連続 3 回で CUI へ (完了条件: gshell 再起動の無限ループ回避)。 */
            if (is_gui) {
                if (rc == EXEC_ERR_FAULT) {
                    gui_fault_streak++;
                    if (gui_fault_streak >= 3) {
                        tvram_print(0, 0, "gshell crashed 3x -> CUI shell", TATTR_RED);
                        cur_shell = SYS_SHELL_BIN;
                        gui_fault_streak = 0;
                    }
                } else {
                    gui_fault_streak = 0;   /* 正常終了なら連続失敗をリセット */
                }
            } else if (rc < 0 && rc != EXEC_ERR_FAULT) {
                /* CUI シェルのロード自体が失敗 — FDD フォールバックを試す */
                rc = exec_run(SYS_SHELL_BIN_FDD);
                if (rc < 0 && rc != EXEC_ERR_FAULT) {
                    /* シェルバイナリのロード自体が失敗 — 致命的エラー */
                    tvram_print(0, 0, "FATAL: shell.bin load failed", TATTR_RED);
                    for (;;) asm volatile("hlt");
                }
            }

            /* シェルが終了 (切替 or exit) またはクラッシュ復帰。
             * FDD負荷軽減用ウェイト (約100ms) の後、画面クリアして再起動。
             * tick_count は 100Hz で回る u32 で 497 日で一周する。差分を
             * 符号付きで見れば一周しても正しい (待ち << 2^31 tick である限り)。 */
            {
                u32 start = tick_count;
                while ((i32)(tick_count - start) < (i32)SHELL_RELOAD_DELAY) {
                    asm volatile("hlt");
                }
            }
            tvram_clear();
        }
    }

    /* 失敗時フォールバック */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
