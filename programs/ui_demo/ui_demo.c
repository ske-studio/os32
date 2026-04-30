/* ======================================================================== */
/*  UI_DEMO — microUI デモプログラム                                        */
/*                                                                          */
/*  GFXモードで microUI のウィンドウ・ボタン・テキスト描画をテストする       */
/* ======================================================================== */

#include "os32api.h"
#include "libos32gfx.h"
#include "libos32ui.h"

static KernelAPI *api;

/* microui コンテキスト (巨大構造体なので static 配置) */
static mu_Context ctx;

/* デモ用状態 */
static int click_count;
static int checkbox_val;
static int slider_val;
static char textbox_buf[64];

static void demo_ui(void)
{
    if (mu_begin_window(&ctx, "UI Demo", mu_rect(40, 40, 280, 300))) {

        /* ラベル */
        mu_label(&ctx, "microUI on OS32!");

        /* ボタン */
        if (mu_button(&ctx, "Click Me")) {
            click_count++;
        }
        {
            char buf[32];
            int n;
            n = click_count;
            /* C89: snprintf で表示 */
            buf[0] = 'C'; buf[1] = 'l'; buf[2] = 'i'; buf[3] = 'c';
            buf[4] = 'k'; buf[5] = 's'; buf[6] = ':'; buf[7] = ' ';
            /* 簡易 itoa */
            {
                int pos = 8;
                int tmp = n;
                int digits = 0;
                int d;
                if (tmp == 0) { buf[pos++] = '0'; }
                else {
                    char rev[10];
                    while (tmp > 0 && digits < 10) {
                        rev[digits++] = '0' + (tmp % 10);
                        tmp /= 10;
                    }
                    for (d = digits - 1; d >= 0; d--) {
                        buf[pos++] = rev[d];
                    }
                }
                buf[pos] = '\0';
            }
            mu_label(&ctx, buf);
        }

        /* チェックボックス */
        mu_checkbox(&ctx, "Enable feature", &checkbox_val);

        /* スライダー */
        {
            int widths[1];
            widths[0] = -1;
            mu_layout_row(&ctx, 1, widths, 20);
        }
        mu_slider(&ctx, &slider_val, 0, 100);

        /* テキスト入力 */
        mu_textbox(&ctx, textbox_buf, sizeof(textbox_buf));

        /* 閉じるボタン */
        if (mu_button(&ctx, "Quit")) {
            mu_end_window(&ctx);
            return;
        }

        mu_end_window(&ctx);
    }
}

void __cdecl main(int argc, char **argv, KernelAPI *kapi)
{
    int running;
    (void)argc; (void)argv;
    api = kapi;

    /* GFX初期化 */
    libos32gfx_init(api);
    api->mouse_cursor_set_mode(MOUSE_CURSOR_NONE);

    /* microUI初期化 */
    mui_init(&ctx, api, 8, 8);

    /* 初期値 */
    click_count = 0;
    checkbox_val = 0;
    slider_val = 50;
    textbox_buf[0] = '\0';

    running = 1;
    while (running) {
        /* 入力 */
        mui_pump_input(&ctx);

        /* ESCで終了 */
        {
            int ch = api->kbd_trygetchar();
            if (ch == 0x1B) running = 0;
        }

        /* UI構築 */
        mu_begin(&ctx);
        demo_ui();
        mu_end(&ctx);

        /* 描画 */
        gfx_clear(0);
        mui_render(&ctx);

        /* VRAM転送 */
        api->gfx_add_dirty_rect(0, 0, 640, 400);
        api->gfx_present_dirty();

        /* ~30fps: get_tick はビジーウェイト */
        {
            u32 t0 = api->get_tick();
            while (api->get_tick() - t0 < 3) { /* 3 tick = 30ms */ }
        }
    }

    api->gfx_shutdown();
    api->tvram_clear();
}
