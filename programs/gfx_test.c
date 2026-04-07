/* ======================================================================== */
/*  GFX_TEST.C — GFXテストパターン (外部OS32Xプログラム)                    */
/*                                                                          */
/*  KernelAPI経由でグラフィック描画テストを実行する。                        */
/*  ESCキーでテキストモードに復帰。                                        */
/* ======================================================================== */




/* KernelAPI構造体 (exec.hと同一レイアウト) */
#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    int i;

    api->kprintf(0xA1, "%s", "GFX Test Pattern (external program)\n");
    api->kprintf(0xE1, "%s", "  Press any key to exit...\n");

    /* GFXモード開始 */
    api->gfx_init();

    /* 16色グラデーションバー */
    for (i = 0; i < 16; i++) {
        api->gfx_fill_rect(i * 40, 0, 40, 50, (u8)i);
    }

    /* 矩形テスト */
    api->gfx_rect(20, 60, 200, 100, 15);     /* 白枠 */
    api->gfx_rect(22, 62, 196, 96, 9);       /* 青枠 */
    api->gfx_fill_rect(30, 70, 80, 40, 10);  /* 赤塗り */
    api->gfx_fill_rect(120, 70, 80, 40, 12); /* 緑塗り */

    /* 直線テスト */
    for (i = 0; i < 16; i++) {
        api->gfx_line(240, 60, 440, 60 + i * 6, (u8)(i % 16));
    }

    /* 対角線 */
    api->gfx_line(0, 200, 639, 399, 14);  /* 黄 */
    api->gfx_line(639, 200, 0, 399, 13);  /* 水色 */

    /* 画面更新 */
    api->gfx_present();

    /* キー入力待ち */
    while (api->kbd_trygetchar() < 0) {
        /* hlt相当 — ビジーウェイト */
    }

    /* テキストモード復帰 */
    api->gfx_shutdown();
    api->kprintf(0xC1, "%s", "Returned to text mode.\n");
}
