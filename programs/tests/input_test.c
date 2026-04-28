/* ======================================================================== */
/*  INPUT_TEST.C — libos32input テストプログラム                              */
/*                                                                          */
/*  キーボード/マウスのバインディングを登録し、                               */
/*  入力状態をCUIでリアルタイム表示する。ESCで終了。                          */
/* ======================================================================== */

#include "os32api.h"
#include "libos32input.h"

/* アクションID */
#define ACT_MOVE_UP     0
#define ACT_MOVE_DOWN   1
#define ACT_MOVE_LEFT   2
#define ACT_MOVE_RIGHT  3
#define ACT_CONFIRM     4
#define ACT_CANCEL      5
#define ACT_MENU        6

static const char *act_names[] = {
    "UP   ", "DOWN ", "LEFT ", "RIGHT",
    "OK   ", "ESC  ", "MENU "
};

static KernelAPI *api;

static void setup_bindings(void)
{
    /* 方向キー */
    input_bind(ACT_MOVE_UP,    INP_DEV_KEYBOARD, 0x3A, FIX16_ONE);
    input_bind(ACT_MOVE_DOWN,  INP_DEV_KEYBOARD, 0x3D, FIX16_ONE);
    input_bind(ACT_MOVE_LEFT,  INP_DEV_KEYBOARD, 0x3B, FIX16_ONE);
    input_bind(ACT_MOVE_RIGHT, INP_DEV_KEYBOARD, 0x3C, FIX16_ONE);

    /* 決定 = Space or マウス左 */
    input_bind(ACT_CONFIRM, INP_DEV_KEYBOARD, 0x34, FIX16_ONE);
    input_bind(ACT_CONFIRM, INP_DEV_MOUSE, MOUSE_BTN_LEFT, FIX16_ONE);

    /* キャンセル = Escape or マウス右 */
    input_bind(ACT_CANCEL, INP_DEV_KEYBOARD, 0x00, FIX16_ONE);
    input_bind(ACT_CANCEL, INP_DEV_MOUSE, MOUSE_BTN_RIGHT, FIX16_ONE);

    /* メニュー = F1 */
    input_bind(ACT_MENU, INP_DEV_KEYBOARD, 0x62, FIX16_ONE);
}

int main(int argc, char **argv, KernelAPI *_api)
{
    int i;
    i16 mx, my;

    api = _api;
    (void)argc; (void)argv;

    api->kprintf(ATTR_WHITE, "=== libos32input test ===\n");
    api->kprintf(ATTR_CYAN,  "Arrow/Space/Esc/F1/MouseL/MouseR\n");
    api->kprintf(ATTR_CYAN,  "Press ESC to quit.\n\n");

    input_init(api);
    setup_bindings();

    for (;;) {
        input_update();

        /* ESC (ACT_CANCEL) のトリガーで終了 */
        if (input_triggered(ACT_CANCEL)) {
            break;
        }

        /* 各アクションの状態を表示 */
        for (i = 0; i <= ACT_MENU; i++) {
            if (input_triggered(i)) {
                api->kprintf(ATTR_GREEN, "[TRIG] %s\n", act_names[i]);
            }
            if (input_released(i)) {
                api->kprintf(ATTR_YELLOW, "[REL]  %s\n", act_names[i]);
            }
            /* リピート判定テスト (hold=30, repeat=10) */
            if (input_held(i, 30, 10) && !input_triggered(i)) {
                api->kprintf(ATTR_MAGENTA, "[RPT]  %s\n", act_names[i]);
            }
        }

        /* マウス座標表示 (ボタン押下時のみ) */
        if (input_mouse_btn(MOUSE_BTN_LEFT) ||
            input_mouse_btn(MOUSE_BTN_RIGHT)) {
            input_get_mouse(&mx, &my);
            api->kprintf(ATTR_WHITE, "Mouse: (%d, %d)\n",
                         (int)mx, (int)my);
        }

        /* 少し待つ (約16ms ≒ 60fps) */
        {
            u32 start = api->get_tick();
            while (api->get_tick() - start < 16) {
                /* busy wait */
            }
        }
    }

    input_shutdown();
    api->kprintf(ATTR_WHITE, "\nDone.\n");
    return 0;
}
