/* ======================================================================== */
/*  V86_SESSION.C - V86 セッションマネージャ                                */
/*                                                                          */
/*  V86 (VDOS) セッションのライフサイクル管理を行う。                       */
/*  v86_test.c から v86_boot_freedos() を分離し、Auto-Typer、               */
/*  強制脱出ホットキー、終了理由管理を統合する。                            */
/* ======================================================================== */

#include "v86_session.h"
#include "v86.h"
#include "v86_mem.h"
#include "v86_bios.h"
#include "v86_pic.h"
#include "v86_pit.h"
#include "v86_fdc.h"
#include "v86_dma.h"
#include "v86_disk.h"
#include "v86_debug.h"
#include "v86_bda.h"
#include "tss.h"
#include "paging.h"
#include "memmap.h"
#include "kstring.h"
#include "vfs.h"
#include "kprintf.h"
#include "io.h"
#include "kbd.h"
#include "fdc.h"

extern void serial_puts(const char *s);

/* exec_setjmp / exec_longjmp (setjmp.asm) */
extern int exec_setjmp(u32 *buf);
extern void exec_longjmp(u32 *buf);

/* 外部TSSアクセス */
extern struct tss_entry kernel_tss;

/* ====================================================================== */
/*  グローバル変数 (v86_session.h で extern 宣言済み)                       */
/* ====================================================================== */
volatile int v86_exit_request = 0;
u32 v86_irq0_inject_count = 0;

/* ====================================================================== */
/*  セッションローカル変数                                                 */
/* ====================================================================== */
static V86Session current_session;
static u32 v86_session_jmpbuf[6];

/* V86テスト用カーネルスタック (16KB) */
static u8 v86_kstack[16384] __attribute__((aligned(16)));

/* TSS ESP0保存 (static — longjmp後にスタック上のローカル変数が壊れるため) */
static u32 v86_saved_esp0;

/* ====================================================================== */
/*  §7.4 デバイス状態退避バッファ (FM音源 / EGC)                         */
/*                                                                          */
/*  V86開始前に保存し、V86終了後に復元する。                       */
/*  FM音源 (OPN): アドレスポート 0x188/0x18A を 0クリアしてサイレント化    */
/*  EGC: アクセスアドレス値 (0x04A0-0x04AE) を保存                          */
/* ====================================================================== */
static u8  v86_saved_opn_addr;     /* OPNアドレスレジスタ (0x188) */
static u8  v86_saved_opn2_addr;    /* OPN2アドレスレジスタ (0x18A, PC-9801-86/118等) */
static u16 v86_saved_egc[8];       /* EGCレジスタ (0x04A0/02/04/06/08/0A/0C/0E) */

/* ====================================================================== */
/*  IPL 定数                                                               */
/* ====================================================================== */
#define IPL_SEG      0x1FC0
#define IPL_LINEAR   0x1FC00UL
#define IPL_SIZE     1024

/* ====================================================================== */
/*  v86_exit_reason_str - 終了理由を文字列に変換                           */
/* ====================================================================== */
const char *v86_exit_reason_str(enum v86_exit_reason reason)
{
    switch (reason) {
    case V86_EXIT_NONE:       return "none";
    case V86_EXIT_TRAP_PORT:  return "trap port (0xFE)";
    case V86_EXIT_DOS_TERM:   return "DOS terminate (INT 20h/21h)";
    case V86_EXIT_REBOOT:     return "reboot detected";
    case V86_EXIT_BIOS_ROM:   return "BIOS ROM (CS>=F000)";
    case V86_EXIT_TIMEOUT:    return "timeout";
    case V86_EXIT_UNKNOWN_OP: return "unknown opcode";
    case V86_EXIT_HOTKEY:     return "hotkey (F12)";
    }
    return "unknown";
}

/* ====================================================================== */
/*  v86_request_exit - V86終了を要求する                                   */
/* ====================================================================== */
void v86_request_exit(enum v86_exit_reason reason)
{
    if (current_session.exit_reason == V86_EXIT_NONE) {
        current_session.exit_reason = reason;
    }
    v86_exit_request = 1;
}

/* ====================================================================== */
/*  v86_session_on_tick - IRQ0 (100Hz) ごとのコールバック                   */
/*                                                                          */
/*  v86_inject_timer_irq() から呼ばれる。                                  */
/*  1. Auto-Typer: BDAキーバッファへのキー注入                             */
/*  2. 強制脱出: F12キー検知                                               */
/* ====================================================================== */
void v86_session_on_tick(void)
{

    /* ============================================================ */
    /*  強制脱出ホットキー検知                                      */
    /*  方法1: Ctrl+GRPH+DEL (PC-98の GRPH = PC/AT の Alt)         */
    /*  方法2: STOP キー (PC-98固有キー、DOSでは未使用)             */
    /*  kbd_is_pressed() は物理キー押下状態を直接参照するため、     */
    /*  DOS側がキーバッファを消費しても影響を受けない。             */
    /* ============================================================ */
    if (kbd_is_pressed(KEY_DEL)
        && (kbd_shift_state & (SHIFT_CTRL | SHIFT_GRPH))
           == (SHIFT_CTRL | SHIFT_GRPH)) {
        v86_request_exit(V86_EXIT_HOTKEY);
        return;
    }
    if (kbd_is_pressed(KEY_STOP)) {
        v86_request_exit(V86_EXIT_HOTKEY);
        return;
    }

    /* ============================================================ */
    /*  Auto-Typer                                                  */
    /* ============================================================ */
    if (current_session.auto_cmd == 0 || current_session.auto_done) {
        return;
    }

    /* 初期ディレイ */
    if (current_session.auto_delay_remaining > 0) {
        current_session.auto_delay_remaining--;
        return;
    }

    /* コマンド文字列の終端チェック */
    if (current_session.auto_cmd[current_session.auto_cmd_idx] == '\0') {
        current_session.auto_done = 1;
        return;
    }

    /* V86_AUTO_TYPE_INTERVAL ticks ごとに1文字注入 */
    if ((v86_irq0_call_count % V86_AUTO_TYPE_INTERVAL) == 0) {
        u8 *bda = v86_phys_addr(0, 0);
        u8 count = bda[BDA_KB_COUNT];

        if (count < 0x10) {
            /* §1.2 修正: 生産側は TAIL を進める (kbd.c の物理キー経路と同規約)
             * BDA 規約: HEAD=取出ポインタ (DOS が進める), TAIL=入力ポインタ (生産側が進める)
             * NP21/W bios09.c 準拠 */
            u16 tail = *(u16 *)&bda[BDA_KB_TAIL];
            char c = current_session.auto_cmd[current_session.auto_cmd_idx++];
            u8 scancode = 0;

            if (c == '\r' || c == '\n') {
                c = 0x0D;
                scancode = 0x1C; /* Enter key */
            }

            bda[tail] = c;
            bda[tail + 1] = scancode;
            tail += 2;
            if (tail >= BDA_KB_BUF_END) tail = BDA_KB_BUF_START;
            *(u16 *)&bda[BDA_KB_TAIL] = tail;
            bda[BDA_KB_COUNT] = count + 1;
        }
    }
}




/* ====================================================================== */
/*  デバッグカウンタリセット                                               */
/* ====================================================================== */
static void v86_reset_counters(void)
{
    extern volatile u32 tick_count;

    v86_int_count = 0;
    v86_gp_count = 0;
    v86_irq0_inject_count = 0;
    v86_irq0_call_count = 0;
    v86_irq0_nonvm_count = 0;
    v86_irq0_noif_count = 0;
    v86_irq0_isr_count = 0;
    v86_irq0_ivt_count = 0;
    v86_irq0_gp_inject_count = 0;
    v86_irq0_gp_skip_if = 0;
    v86_irq0_gp_skip_isr = 0;
    v86_irq0_gp_skip_ivt = 0;
    v86_start_tick = tick_count;
    v86_timeout_cs = 0;
    v86_timeout_ip = 0;
    v86_trace_reset();
    v86_disk_reset_log();
    v86_reset_io_stats();
}

/* ====================================================================== */
/*  v86_session_run_core — V86実行コア (共通ランタイム)                     */
/*                                                                          */
/*  V86メモリ空間・PIC/PIT・ディスク仮想化の初期化後に呼ばれ、             */
/*  V86コンテキストのセットアップ → 実行 → 後始末 を一括で行う。           */
/* ====================================================================== */
static void v86_session_run_core(void)
{
    struct v86_context ctx;

    /* §1.7 TVRAM 退避 (V86開始前に OS32 テキスト画面を保存) */
    v86_tvram_save();

    /* V86コンテキスト: IPLエントリ */
    ctx.eip    = 0x0000;
    ctx.cs     = IPL_SEG;
    ctx.eflags = EFLAGS_VM | EFLAGS_IF;
    ctx.esp    = 0xFFFE;
    ctx.ss     = 0x0000;
    ctx.es     = IPL_SEG;
    ctx.ds     = IPL_SEG;
    ctx.fs     = 0x0000;
    ctx.gs     = 0x0000;

    /* TSS ESP0 切り替え (§1.5: static変数に実値を保存 — longjmp後スタックが壊れるため)
     * ハードコード 0x9FFF0UL 廃止 → tss_get_esp0() で現在値を取得して保存 */
    v86_saved_esp0 = tss_get_esp0();
    tss_set_esp0((u32)&v86_kstack[sizeof(v86_kstack) - 16]);

    /* ================================================================ */
    /*  §7.4 FM音源・ EGC 状態退避                                    */
    /* ================================================================ */
    {
        int i;
        v86_saved_opn_addr  = (u8)inp(0x188);
        v86_saved_opn2_addr = (u8)inp(0x18A);
        for (i = 0; i < 8; i++) {
            v86_saved_egc[i] = (u16)inpw((unsigned int)(0x04A0 + i * 2));
        }
    }

    /* V86モード遷移準備 */
    v86_active = 1;
    v86_exit_request = 0;
    current_session.exit_reason = V86_EXIT_NONE;

    /* デバッグカウンタリセット */
    v86_reset_counters();

    /* ジャンプバッファをアクティブに設定 */
    v86_current_jmpbuf = v86_session_jmpbuf;

    if (exec_setjmp(v86_session_jmpbuf) == 0) {
        v86_enter(&ctx);
    }

    /* ============================================================ */
    /*  V86終了後の後始末                                           */
    /*                                                              */
    /*  ★重要: longjmpでカーネルスタック(0x9Fxxx)に戻った時点では  */
    /*  ページテーブルがまだV86バッキングRAMを指しており、           */
    /*  スタック内容はDOSに上書きされて壊れている。                 */
    /*  ローカル変数もスタックも使えないため:                       */
    /*  1. 一時的にv86_kstackにスタックを切り替え                   */
    /*  2. v86_mem_teardown()でページテーブル復元                   */
    /*  3. カーネルスタックの実体が復活してから通常のスタックに復帰 */
    /* ============================================================ */
    _disable();

    /* 一時スタックに切り替え → teardown → 元のスタックに戻す */
    {
        u32 tmp_stack = (u32)&v86_kstack[sizeof(v86_kstack) - 64];
        __asm__ volatile (
            "mov %%esp, %%esi\n\t"   /* 現在のESPを保存 */
            "mov %%ebp, %%edi\n\t"   /* 現在のEBPを保存 */
            "mov %0, %%esp\n\t"      /* 一時スタックに切り替え */
            "call v86_mem_teardown\n\t" /* ページテーブル復元 */
            "mov %%esi, %%esp\n\t"   /* ESPを元に戻す (実体が復活) */
            "mov %%edi, %%ebp\n\t"   /* EBPも復元 */
            : : "r"(tmp_stack)
            : "esi", "edi", "eax", "ecx", "edx", "memory"
        );
    }

    /* TSS ESP0 復元 (static変数から読む) */
    tss_set_esp0(v86_saved_esp0);

    /* V86状態フラグクリア */
    v86_current_jmpbuf = 0;
    v86_active = 0;
    v86_exit_request = 0;
    v86_pending_irq = 0;

    _enable();

    /* デバッグダンプ (有効時のみ) */
    v86_debug_dump_session();

    /* リソース解放 */
    v86_disk_clear();

    /* 画面リストア */
    v86_restore_screen();

    /* §1.7 TVRAM 復元 (DOS が描いた文字を消去して OS32 画面を戻す) */
    v86_tvram_restore();

    /* ================================================================ */
    /*  §7.4 FM音源・ EGC 状態復元                                    */
    /*                                                                  */
    /*  DOSが変更した OPNアドレスラッチと EGCアクセスアドレスを復元する。   */
    /*  OPNは Key-Off (全チャンネルサイレンス) してからアドレス復元。     */
    /* ================================================================ */
    {
        int i;
        /* OPNサイレンス: 全チャンネル Key-Off (0x28レジスタ) */
        outp(0x188, 0x28); outp(0x18A, 0);
        outp(0x188, 0x29); outp(0x18A, 0);
        outp(0x188, 0x2A); outp(0x18A, 0);
        outp(0x188, 0x2C); outp(0x18A, 0);
        outp(0x188, 0x2D); outp(0x18A, 0);
        outp(0x188, 0x2E); outp(0x18A, 0);
        /* OPNアドレスラッチ復元 */
        outp(0x188, v86_saved_opn_addr);
        outp(0x18A, v86_saved_opn2_addr);
        /* EGCアクセスアドレス復元 */
        for (i = 0; i < 8; i++) {
            outpw((unsigned int)(0x04A0 + i * 2), v86_saved_egc[i]);
        }
    }

    /* 終了メッセージ */
    kprintf(0xA1, "[V86] Session ended: %s\n",
            v86_exit_reason_str(current_session.exit_reason));
}

/* ====================================================================== */
/*  v86_boot_freedos - FreeDOS(98) FDDイメージからブート                   */
/* ====================================================================== */
int v86_boot_freedos(const char *path, const char *cmdline)
{
    int fd;
    u8 *ipl_dst;

    /* §1.4 再入禁止ガード: V86セッションが既にアクティブなら即座に返る */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_freedos called while already active\n");
        return -2;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;

    /* イメージファイルオープン */
    fd = vfs_open(path, 0);
    if (fd < 0) {
        kprintf(0xE1, "[V86] FDD image open failed: %s (rc=%d)\n", path, fd);
        return -1;
    }
    current_session.fd = fd;
    current_session.img_data_size = vfs_get_size(fd);

    /* V86メモリ空間を構築 (バッキングRAM PTE_USER マッピング + IOPM設定) */
    v86_mem_setup();

    /* PIC/PIT/FDC/DMA仮想化初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_fdc_virt_init();
    v86_dma_init();

    /* ==================================================================

     * イメージ形式判定とジオメトリ解析
     *
     * 対応形式:
     *   FDI  (Anex86): 4096byte ヘッダ
     *                  offset 0x04: FDDType (0x10=2DD, 0x90=2HD)
     *                  offset 0x08: HeaderSize (通常 0x1000)
     *                  offset 0x0C: DataSize
     *   D88  (NP21/W): 可変ヘッダ
     *                  offset 0x1B: Media flag (0x10=2DD, 0x20=2HD)
     *                  offset 0x1C: ディスクサイズ (LE 4bytes, ヘッダ込み)
     *   RAW/IMG       : ヘッダなし — ファイルサイズで推定
     * ================================================================== */
    {
        u8 hdr[36];   /* D88: track[0] offset は 0x20-0x23 (36B必要) */
        u32 file_size = current_session.img_data_size;
        fdc_media_t media = FDC_MEDIA_2HD_1232; /* デフォルト */
        int media_detected = 0;

        /* ヘッダを最大36バイト読み込む */
        vfs_seek(fd, 0, 0);
        vfs_read_fd(fd, hdr, 36);

        /* ---- D88 判定 ----
         * offset 0x1B のメディアフラグが D88 の既知値 (0x00/0x10/0x20)
         * かつ offset 0x1C の「ディスクサイズ」がファイルサイズと一致する場合
         * D88 とみなす */
        {
            u8 media_flag = hdr[0x1B];
            u32 d88_size  = *(u32 *)(hdr + 0x1C);

            if (d88_size == file_size &&
                (media_flag == 0x00 || media_flag == 0x10 || media_flag == 0x20)) {
                /* D88 形式 */
                current_session.img_offset    = 0; /* D88はオフセットなし (セクタ単位アクセス) */
                /* ただし V86では D88をRAW相当として読むため
                 * 実際の転送はファイル先頭から行う。
                 * TODO: D88パーサ実装まではメディア種別のみ利用する */
                switch (media_flag) {
                case 0x10: /* 2DD */
                    /* D88の2DDはSPT=8(640KB)かSPT=9(720KB)か不明だが
                     * offset 0x20(track table[0])が指すセクタヘッダのN値で判定できる。
                     * 簡易判定: ディスクサイズで推定 */
                    if (file_size <= 700000UL) {
                        media = FDC_MEDIA_2DD_640;
                        kprintf(0xA1, "[V86] D88: 2DD 640KB (media_flag=0x10)\n");
                    } else {
                        media = FDC_MEDIA_2DD_720;
                        kprintf(0xA1, "[V86] D88: 2DD 720KB (media_flag=0x10)\n");
                    }
                    break;
                case 0x20: /* 2HD */
                    media = FDC_MEDIA_2HD_1232;
                    kprintf(0xA1, "[V86] D88: 2HD 1.2MB (media_flag=0x20)\n");
                    break;
                default: /* 0x00 = 2D — 現状は2DDとして扱う */
                    media = FDC_MEDIA_2DD_640;
                    kprintf(0xA1, "[V86] D88: 2D (media_flag=0x00), treating as 2DD\n");
                    break;
                }
                /* D88はデータオフセット=0 (セクタ単位アクセス)
                 * IPLはトラック0, セクタ0のデータ部分から読む。
                 * D88ファイル構造: トラックテーブル [0x20..0x2AF] の [0] が
                 * トラック0の開始オフセットを示す。その直後にセクタヘッダ (16B)、
                 * 続いてセクタデータが並ぶ。 */
                {
                    u32 trk0_off = *(u32 *)(hdr + 0x20); /* track[0] オフセット */
                    /* セクタヘッダ(16B)を読んでセクタサイズを確認 */
                    if (trk0_off >= 0x2B0 && trk0_off < file_size) {
                        u8 sec_hdr[16];
                        u8 sec_n;
                        vfs_seek(fd, trk0_off, 0);
                        vfs_read_fd(fd, sec_hdr, 16);
                        sec_n = sec_hdr[3]; /* N値: 0=128B,1=256B,2=512B,3=1024B */
                        /* IPLデータ開始位置 = トラック0先頭 + セクタヘッダ16B */
                        current_session.img_offset = trk0_off + 16;
                        kprintf(0xA1, "[V86] D88: trk0=0x%x secN=%d ipl_off=0x%x\n",
                                (unsigned)trk0_off, (int)sec_n,
                                (unsigned)current_session.img_offset);
                        /* IPL サイズをセクタサイズで上書き (読み過ぎ防止) */
                        (void)sec_n; /* 現状は1024B固定読み込み — 問題なし */
                    } else {
                        current_session.img_offset = 0x2C0; /* フォールバック */
                    }
                }
                /* D88のfdd_image_sizeはジオメトリ(2HD=1261568)から算出
                 * (ファイルサイズではなく論理ディスクサイズを使う) */
                {
                    const struct fdc_geom *g;
                    /* 一旦セット → geom を取得 → サイズ算出 */
                    v86_disk_set_file(fd, current_session.img_offset,
                                      (u32)77 * 2 * 8 * 1024, /* 2HD仮サイズ */
                                      media);
                    g = v86_disk_get_geom();
                    current_session.img_data_size = (u32)g->cyls * g->heads
                                                     * g->spt * g->bps;
                    /* img_data_size を更新して再セット */
                    v86_disk_set_file(fd, current_session.img_offset,
                                      current_session.img_data_size, media);
                }
                media_detected = 1;
            }
        }

        /* ---- FDI 判定 ----
         * offset 0x08 に HeaderSize が入っており 0x1000 or 0x2000 の場合 FDI */
        if (!media_detected) {
            u32 hdr_size  = *(u32 *)(hdr + 0x08);
            u32 fdi_type  = *(u32 *)(hdr + 0x04);
            u32 data_size = *(u32 *)(hdr + 0x0C);

            if ((hdr_size == 0x1000 || hdr_size == 0x2000) &&
                file_size > hdr_size &&
                (data_size == 0 || data_size == file_size - hdr_size)) {
                /* FDI 形式 */
                current_session.img_offset    = hdr_size;
                current_session.img_data_size = file_size - hdr_size;

                /* FDDType (offset 0x04) からメディア種別を決定 */
                switch (fdi_type) {
                case 0x10: /* 2DD (640KB or 720KB) */
                    if (current_session.img_data_size <= 700000UL) {
                        media = FDC_MEDIA_2DD_640;
                        kprintf(0xA1, "[V86] FDI: 2DD 640KB (FDDType=0x10, hdr=0x%x)\n",
                                (unsigned)hdr_size);
                    } else {
                        media = FDC_MEDIA_2DD_720;
                        kprintf(0xA1, "[V86] FDI: 2DD 720KB (FDDType=0x10, hdr=0x%x)\n",
                                (unsigned)hdr_size);
                    }
                    break;
                case 0x90: /* 2HD 1.2MB */
                    media = FDC_MEDIA_2HD_1232;
                    kprintf(0xA1, "[V86] FDI: 2HD 1.2MB (FDDType=0x90, hdr=0x%x)\n",
                            (unsigned)hdr_size);
                    break;
                case 0x30: /* 1.44MB — 2DDとして近似 */
                    media = FDC_MEDIA_2DD_720;
                    kprintf(0xA1, "[V86] FDI: 1.44MB (FDDType=0x30) → 2DD 720KB\n");
                    break;
                default:
                    /* FDDType不明: データサイズで推定 */
                    if (current_session.img_data_size == 655360UL) {
                        media = FDC_MEDIA_2DD_640;
                    } else if (current_session.img_data_size == 737280UL) {
                        media = FDC_MEDIA_2DD_720;
                    } else {
                        media = FDC_MEDIA_2HD_1232;
                    }
                    kprintf(0xA1, "[V86] FDI: unknown FDDType=0x%x, size=%u\n",
                            (unsigned)fdi_type, (unsigned)current_session.img_data_size);
                    break;
                }
                media_detected = 1;
            }
        }

        /* ---- RAW/IMG フォールバック ----
         * ヘッダなし — ファイルサイズのみで判定 */
        if (!media_detected) {
            if (file_size == 1261568UL) {
                media = FDC_MEDIA_2HD_1232;
                kprintf(0xA1, "[V86] RAW: 2HD 1.2MB (%u bytes)\n", (unsigned)file_size);
            } else if (file_size == 655360UL) {
                media = FDC_MEDIA_2DD_640;
                kprintf(0xA1, "[V86] RAW: 2DD 640KB (%u bytes)\n", (unsigned)file_size);
            } else if (file_size == 737280UL) {
                media = FDC_MEDIA_2DD_720;
                kprintf(0xA1, "[V86] RAW: 2DD 720KB (%u bytes)\n", (unsigned)file_size);
            } else {
                media = FDC_MEDIA_2HD_1232;
                kprintf(0xA1, "[V86] RAW: unknown size %u bytes, assuming 2HD\n",
                        (unsigned)file_size);
            }
            current_session.img_offset    = 0;
            current_session.img_data_size = file_size;
        }

        /* FDI / RAW のみここで set_file を呼ぶ。
         * D88 はブランチ内で呼び済みなのでスキップ。 */
        if (!media_detected) {
            v86_disk_set_file(fd, current_session.img_offset,
                              current_session.img_data_size, media);
        }
    }


    /* IPLをV86メモリにコピー */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    vfs_seek(fd, current_session.img_offset, 0);
    vfs_read_fd(fd, ipl_dst, IPL_SIZE);

    kprintf(0xA1, "[V86] Booting FreeDOS(98) IPL...\n");

    /* V86実行コア */
    v86_session_run_core();

    /* ファイルリソース解放 */
    vfs_close(fd);
    current_session.fd = -1;

    return 0;
}

/* ====================================================================== */
/*  v86_boot_physical_fdd - 実FDDからV86セッションを起動                   */
/*                                                                          */
/*  NP21/Wにマウント中のFDDから直接IPLを読み、FreeDOSを起動する。         */
/*  ファイルオープン不要。fdc_read_sector() でセクタを直接読む。           */
/* ====================================================================== */
int v86_boot_physical_fdd(int drv, const char *cmdline)
{
    u8 *ipl_dst;
    int retry;
    int ipl_rc = -1;

    /* §1.4 再入禁止ガード */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_physical_fdd called while already active\n");
        return -2;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;
    current_session.img_data_size = V86_FDD_IMAGE_SIZE;

    /* Phase 1A: FDCを再初期化 (ブート時に失敗していたケースの救済)
     * メディア挿入後にユーザーが vdos を起動するため、毎回
     * reset + specify + recalibrate を再実行する必要がある。 */
    {
        int init_rc = fdc_init();
        if (init_rc != 0) {
            kprintf(0xE1, "[V86] fdc_init failed (rc=%d). Insert FDD media and retry.\n",
                    init_rc);
            return -1;
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT/FDC/DMA初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_fdc_virt_init();
    v86_dma_init();

    /* 実FDDモードを設定 (デフォルト 2HD) */
    v86_disk_set_physical(drv, FDC_MEDIA_2HD_1232);

    /* Phase 1B: IPLを実FDCから読み込み (3回リトライ + 各リトライ前に再recalibrate) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    for (retry = 0; retry < 3; retry++) {
        ipl_rc = fdc_read_sector(drv, 0, 0, 1, ipl_dst);
        if (ipl_rc == 0) break;
        kprintf(0xA1, "[V86] IPL read retry %d/3 (rc=%d)\n", retry + 1, ipl_rc);
        /* 失敗時: FDCを再初期化して次のリトライに備える */
        if (fdc_init() != 0) {
            break; /* 再初期化も失敗なら諦める */
        }
    }
    if (ipl_rc != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed after 3 retries (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }

    kprintf(0xA1, "[V86] Booting from physical FDD (drv=%d)...\n", drv);

    /* V86実行コア */
    v86_session_run_core();

    return 0;
}

/* ====================================================================== */
/*  v86_boot_physical_fdd_ex - 実FDDからV86セッションを起動 (メディア指定版) */
/*                                                                          */
/*  media: 0=2HD(1.2MB), 1=2DD(640KB), 2=2DD(720KB)                       */
/*  kapi.json の sys_v86_boot_physical_ex から呼び出される。               */
/* ====================================================================== */
int v86_boot_physical_fdd_ex(int drv, int media, const char *cmdline)
{
    fdc_media_t fdc_media;
    u8 *ipl_dst;
    int retry;
    int ipl_rc = -1;
    const struct fdc_geom *g;

    /* §1.4 再入禁止ガード */
    if (v86_active) {
        kprintf(0xE1, "[V86] ERROR: v86_boot_physical_fdd_ex called while already active\n");
        return -2;
    }

    /* メディア種別を enum に変換 */
    switch (media) {
    case 1:  fdc_media = FDC_MEDIA_2DD_640;  break;
    case 2:  fdc_media = FDC_MEDIA_2DD_720;  break;
    default: fdc_media = FDC_MEDIA_2HD_1232; break;
    }

    /* セッション初期化 */
    kmemset(&current_session, 0, sizeof(current_session));
    current_session.auto_cmd = cmdline;
    current_session.auto_delay_remaining = V86_AUTO_TYPE_DELAY;
    current_session.fd = -1;

    /* FDC再初期化 */
    {
        int init_rc = fdc_init();
        if (init_rc != 0) {
            kprintf(0xE1, "[V86] fdc_init failed (rc=%d).\n", init_rc);
            return -1;
        }
    }

    /* V86メモリ空間を構築 */
    v86_mem_setup();

    /* PIC/PIT/FDC/DMA初期化 */
    v86_pic_init();
    v86_pit_init();
    v86_fdc_virt_init();
    v86_dma_init();

    /* 実FDDモードを設定 (指定メディア) */
    v86_disk_set_physical(drv, fdc_media);
    g = v86_disk_get_geom();
    current_session.img_data_size = (u32)g->cyls * g->heads
                                     * g->spt * g->bps;

    /* IPL読み込み (3回リトライ) */
    ipl_dst = v86_phys_addr(IPL_SEG, 0);
    for (retry = 0; retry < 3; retry++) {
        ipl_rc = fdc_read_sector_geom(drv, 0, 0, 1, g, ipl_dst);
        if (ipl_rc == 0) break;
        kprintf(0xA1, "[V86] IPL read retry %d/3 (rc=%d)\n", retry + 1, ipl_rc);
        if (fdc_init() != 0) break;
    }
    if (ipl_rc != 0) {
        kprintf(0xE1, "[V86] FDC read IPL failed after 3 retries (drv=%d)\n", drv);
        v86_disk_clear();
        v86_mem_teardown();
        return -1;
    }

    kprintf(0xA1, "[V86] Booting from physical FDD (drv=%d, media=%d)...\n",
            drv, media);

    /* V86実行コア */
    v86_session_run_core();

    return 0;
}
