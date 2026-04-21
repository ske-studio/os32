/* ======================================================================== */
/*  FLIP400_TEST.C — 400ラインモードでのA6Hページ切替検証                   */
/*                                                                          */
/*  テスト内容:                                                             */
/*    1. gfx_init() で400ラインモード初期化                                 */
/*    2. VRAM直接書き込みでページ0に白パターン                              */
/*    3. A6H=1 でアクセスページ切替後、ページ1にストライプ                  */
/*    4. A4H=1 で表示ページ切替 → ストライプが見えれば成功                 */
/*    5. A4H=0 で表示ページ復帰 → 白が見えれば成功                        */
/* ======================================================================== */

#include "os32api.h"

static KernelAPI *api;

static void io_out(int port, int val)
{
    __asm__ volatile("outb %%al, %%dx" :: "a"(val), "d"(port));
}

void main(int argc, char **argv, KernelAPI *kapi)
{
    volatile u8 *vram_b = (volatile u8 *)0xA8000;
    volatile u8 *vram_r = (volatile u8 *)0xB0000;
    int i;

    api = kapi;

    api->gfx_init();  /* 400ラインモード */

    api->kprintf(0x0E, "=== 400-line A6H page flip test ===\n");
    api->kprintf(0x0E, "Step 1: Writing SOLID pattern to page 0...\n");

    /* ページ0 (現アクセスページ) に白い矩形を VRAM 直接書き込み */
    /* Bプレーン上半分 (200ライン分) を 0xFF で埋める */
    for (i = 0; i < 16000; i++) {
        vram_b[i] = 0xFF;
    }

    api->kprintf(0x0E, "Step 2: Switching access page to 1 via A6H...\n");

    /* A6H でアクセスページを 1 に切替 */
    io_out(0xA6, 0x01);

    /* ページ1 にストライプパターン (0xAA) を書き込み */
    for (i = 0; i < 16000; i++) {
        vram_b[i] = 0xAA;
        vram_r[i] = 0xAA;
    }

    api->kprintf(0x0E, "Step 3: Switching display page to 1 via A4H...\n");
    api->kprintf(0x0E, "  -> If STRIPE visible = A6H works in 400-line!\n");
    api->kprintf(0x0E, "  -> Press any key...\n");

    /* A4H で表示ページを 1 に切替 */
    io_out(0xA4, 0x01);

    /* キー待ち (ストライプが見えるか確認) */
    api->kbd_getchar();

    api->kprintf(0x0E, "Step 4: Display page back to 0...\n");
    api->kprintf(0x0E, "  -> If SOLID visible = Both pages independent!\n");
    api->kprintf(0x0E, "  -> Press any key to exit...\n");

    /* 表示ページを 0 に戻す */
    io_out(0xA4, 0x00);

    api->kbd_getchar();

    /* 後始末: ページ0に復帰 */
    io_out(0xA6, 0x00);
    io_out(0xA4, 0x00);

    /* VRAM クリア */
    for (i = 0; i < 32000; i++) {
        vram_b[i] = 0x00;
        vram_r[i] = 0x00;
    }

    api->gfx_shutdown();
}
