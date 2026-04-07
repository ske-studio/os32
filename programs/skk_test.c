#include "os32api.h"
#include "skk/skk.h"

/* プリエディット表示エリアのクリア */
static void _clear_preedit_area(KernelAPI *api, int x, int y) {
    api->console_set_cursor(x, y);
    api->kprintf(0xE1, "%s", "                              ");
    api->console_set_cursor(x, y);
}

void main(int argc, char **argv, KernelAPI *api) {
    SKK_STATE skk;
    char commited[256];
    char ui_str[256];
    u8 key;
    int run = 1;
    int cur_x = 0, cur_y = 0;
    int pe_active = 0;

    api->kprintf(0xE1, "%s", "Loading SKK Dict (LZSS 90KB) from disk to memory...\r\n");
    api->kprintf(0xE1, "%s", "Please wait a moment...\r\n");
    if (skk_init("SKK.LZS", api) != 0) {
        api->kprintf(0x41, "%s", "Failed to initialize SKK.\r\n");
        return;
    }
    api->kprintf(0xC1, "%s", "SKK Dict Loaded! (");
    api->kprintf(0xC1, "%s", "M size, UTF-8)\r\n");

    skk_state_init(&skk);

    api->kprintf(0xA1, "%s", "SKK Test Shell (Press ESC to exit, Ctrl+Space to toggle IME)\r\n");
    api->kprintf(0x81, "%s", "Current Mode: [SKK OFF]\r\n> ");

    while (run) {
        key = (u8)api->kbd_getchar(); /* Block until key */
        
        if (key == 0x1B) { /* ESC */
            break;
        }

        /* Ctrl+Space の判定 (PC-9801 kbd_get_modifiers() の 0x10 は SHIFT_CTRL) */
        if (key == ' ' && (api->kbd_get_modifiers() & 0x10)) {
            key = 0x00; /* 仮想の IME トグルコード */
        }

        if (skk_process_key(key, api->kbd_get_modifiers(), &skk, commited)) {
            if (pe_active) {
                _clear_preedit_area(api, cur_x, cur_y);
                pe_active = 0;
            }

            if (key == 0x00) {
                api->kprintf(0xE1, "%s", "\r\n");
                if (skk.ime_on) {
                    api->kprintf(0xA1, "%s", "[SKK ON]> ");
                } else {
                    api->kprintf(0x81, "%s", "[SKK OFF]> ");
                }
                cur_x = api->console_get_cursor_x();
                cur_y = api->console_get_cursor_y();
            }

            if (commited[0] != '\0') {
                api->kprintf(0xE1, "%s", commited);
                cur_x = api->console_get_cursor_x();
                cur_y = api->console_get_cursor_y();
            }
        }
        
        skk_get_ui_string(&skk, ui_str);
        if (ui_str[0] != '\0') {
            if (!pe_active) {
                cur_x = api->console_get_cursor_x();
                cur_y = api->console_get_cursor_y();
                pe_active = 1;
            }
            api->kprintf(0xC1, "%s", ui_str);
        } else {
            if (pe_active) {
                _clear_preedit_area(api, cur_x, cur_y);
                pe_active = 0;
            }
        }
    }

    skk_free();
    api->kprintf(0xE1, "%s", "\r\nSKK test done.\r\n");
}
