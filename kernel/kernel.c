/* ============================================================
 * kernel.c — PC-9801 32ビットOS カーネル
 * プロテクトモードで動作、テキストVRAMに直接書き込む
 * リニアアドレス 0x9000 に配置される
 *
 * Phase 2+: IDT/PIC/PIT + キーボード + ミニシェル
 * ============================================================ */

#include "idt.h"
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
#include "paging.h"
#include "exec.h"
#include "ide.h"
#include "vfs.h"
#include "fat12.h"
#include "ext2.h"
#include "serialfs.h"
#include "tvram.h"
#include "pc98.h"
#include "memmap.h"
#include "config.h"
#include "sys.h"
#include "lconsole.h"

#define SHELL_RELOAD_DELAY 10

/* tvram_clear は console.c 側に実装済みのため削除 */

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
    lcons_init();

    tvram_print(0, 0, "PC-9801 OS32 booting...", TATTR_CYAN);
    
    /* 簡易itoa (mb表示用) */
    tmp[0] = ' '; tmp[1] = '('; 
    tmp[2] = '0' + (mb / 10) % 10;
    tmp[3] = '0' + (mb % 10);
    tmp[4] = 'M'; tmp[5] = 'B'; tmp[6] = ')'; tmp[7] = '\0';
    if(tmp[2] == '0') tmp[2] = ' '; /* leading zero suppression */
    tvram_print(24, 0, tmp, TATTR_CYAN);

    /* IDT/PIC/PIT 初期化 */
    tvram_print(0, 1, "IDT...", TATTR_GREEN);
    idt_init();
    tvram_print(6, 1, "OK  ", TATTR_WHITE);

    tvram_print(12, 1, "PIC...", TATTR_GREEN);
    pic_init();
    tvram_print(18, 1, "OK  ", TATTR_WHITE);

    tvram_print(24, 1, "PIT...", TATTR_GREEN);
    pit_init(PIT_HZ);
    tvram_print(30, 1, "OK  ", TATTR_WHITE);

    /* タイマとカスケード有効化 */
    irq_enable(0);
    irq_enable(ICU_SLAVEID);

    /* キーボード初期化 */
    tvram_print(36, 1, "KBD...", TATTR_GREEN);
    kbd_init();
    tvram_print(42, 1, "OK", TATTR_WHITE);

    /* FDC初期化 (I/Oポート直接制御) */
    tvram_print(48, 1, "FDC...", TATTR_GREEN);
    /* IRQ11 (FDD) 有効化 */
    irq_enable(FDC_IRQ);
    _enable();  /* FDC初期化前にIRQを許可 */
    {
        int fdc_ret = fdc_init();
        if (fdc_ret == 0) {
            tvram_print(54, 1, "OK", TATTR_WHITE);
        } else {
            tvram_print(54, 1, "ER", TATTR_RED);
        }
    }

    /* デバイス・パスシステム初期化 */
    tvram_print(58, 1, "DEV...", TATTR_GREEN);
    dev_init();
    path_init();
    fat12_init();
    ext2_init();
    serialfs_init();
    path_set_device_validator(dev_find_validator);
    tvram_print(64, 1, "OK", TATTR_WHITE);

    /* パレット初期化 */
    palette_init();


    /* IDE/HDD 初期化と登録 */
    tvram_print(0, 3, "IDE...", TATTR_GREEN);
    {
        int n = ide_init();
        if (n > 0) {
            tvram_print(5, 3, "OK", TATTR_WHITE);
            if (ide_drive_present(0)) dev_register_hdd(0);
            if (ide_drive_present(1)) dev_register_hdd(1);
        } else {
            tvram_print(5, 3, "no drive", TATTR_CYAN);
        }
    }

    /* 自動マウント処理 */
    tvram_print(14, 3, "MOUNT...", TATTR_GREEN);
    {
        int i, rc;
        const char *names[8];
        int num = dev_get_names(names, 8);
        const char *root_dev;
        const char *root_fs;

        if (boot_drive == BOOT_DRIVE_FDD || boot_drive == BOOT_DRIVE_FDD_144) {
            root_dev = "fd0";
            root_fs = "fat12";
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
            mnt[1 + j] = '/'; mnt[2 + j] = '\0';

            /* フォールバックしながらマウント試行 */
            /* 注意: ext2はシングルトン — ルートがext2なら他デバイスでext2は試みない */
            {
                int mounted = 0;
                int root_is_ext2 = (root_fs[0] == 'e'); /* "ext2" */
                if (!root_is_ext2) {
                    if (vfs_mount(mnt, dname, "ext2") == VFS_OK) mounted = 1;
                }
                if (!mounted) {
                    vfs_mount(mnt, dname, "fat12"); /* 失敗しても無視 */
                }
            }
        }
    }

    /* RTC初期化 */
    tvram_print(26, 3, "RTC...", TATTR_GREEN);
    rtc_init();
    tvram_print(32, 3, "OK", TATTR_WHITE);

    /* ヒープ初期化 */
    tvram_print(24, 2, "HEAP...", TATTR_GREEN);
    /* kmalloc_init needs to be modified if it uses mem_kb dynamically, 
       but for now we pass existing KHEAP_SIZE limit, or update it later. */
    kmalloc_init((void *)KHEAP_BASE, KHEAP_SIZE);
    tvram_print(31, 2, "192K", TATTR_WHITE);

    /* ページング初期化 (メモリ保護有効化) */
    tvram_print(37, 2, "PAGE...", TATTR_GREEN);
    paging_init(mem_kb);
    tvram_print(44, 2, "OK", TATTR_WHITE);

    /* プログラムローダー初期化 (KernelAPIテーブル構築) */
    tvram_print(48, 2, "EXEC...", TATTR_GREEN);
    exec_init();
    tvram_print(56, 2, "OK", TATTR_WHITE);

    /* Unicodeテーブルロード */
    tvram_print(60, 2, "UNI...", TATTR_GREEN);
    {
        int bytes = vfs_read("/UNICODE.BIN", (void *)MEM_UNICODE_TABLE_BASE, 131072);
        if (bytes == 131072) {
            tvram_print(67, 2, "OK", TATTR_WHITE);
        } else {
            tvram_print(67, 2, "ER", TATTR_RED);
        }
    }

    /* 割り込み有効化 */
    tvram_print(71, 2, "IRQ_EN", TATTR_YELLOW);
    _enable();

    /* autoexec: HDDマウント成功時に実行 */
    {
        char probe[4];
        int sz = vfs_read(SYS_AUTOEXEC_BIN, probe, 4);
        if (sz > 0) {
            tvram_print(0, 5, "autoexec.bin...", TATTR_GREEN);
            exec_run(SYS_AUTOEXEC_BIN);
            tvram_print(15, 5, "done", TATTR_WHITE);
        }
    }


    /* 外部シェル起動 (終了したら再起動する無限ループ) */
    for (;;) {
        tvram_print(0, 0, "Loading shell...", TATTR_GRAY);
        if (exec_run(SYS_SHELL_BIN) < 0) {
            tvram_print(0, 0, "FATAL: Failed to load shell.bin", TATTR_RED);
            for (;;) asm volatile("hlt");
        }
        /* シェルが異常終了・一瞬で終了した場合のFDD負荷軽減用ウェイト (約100ms) */
        {
            u32 wait_end = tick_count + SHELL_RELOAD_DELAY;
            while (tick_count < wait_end) asm volatile("hlt");
        }
        /* シェルが終了した場合は画面クリアして再起動 */
        tvram_clear();
    }

    /* 失敗時フォールバック */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
