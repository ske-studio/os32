/*
 * core.c - OS32 VZ Editor Core Memory Management
 * Translated from CORE.ASM / EMS.ASM logic
 * C89 compatible
 */

#include "vz.h"

/* The global VZ state instance */
VZGLOBAL vz;

int vz_screen_width = 80;
int vz_screen_height = 25;

/*
 * vz_init
 * Initialize the editor's core memory and global state.
 */
int vz_init(void) {
    /* Get console size if possible */
    if (kapi && kapi->console_get_size) {
        kapi->console_get_size(&vz_screen_width, &vz_screen_height);
        if (vz_screen_width <= 0) vz_screen_width = 80;
        if (vz_screen_height <= 0) vz_screen_height = 25;
    }

    /* Initialize the global state */
    vz.current_text = NULL;
    vz.clipboard_buf = NULL;
    vz.clipboard_len = 0;
    vz.clipboard_max = 0;

    /* Initialize Undo */
    vz.undo_top = 0;
    vz.undo_count = 0;
    vz.undo_ptr = 0;
    vz.undo_in_progress = 0;

    /* Initialize Multi-Window / Multi-File */
    vz.text_count = 0;
    vz.split_mode = 0;
    vz.window_text_idx[0] = 0;
    vz.window_text_idx[1] = 0;
    vz.active_window = 0;

    /* In OS32 flat memory model, we don't need EMS probing or segment setups.
     * We just verify we have enough heap memory.
     */
    
    /* Pre-allocations or pool setups can go here if needed.
     * For now, we will just use raw malloc / free wrappers.
     */
    return TRUE;
}

/*
 * vz_alloc
 * Wrapper around standard libc malloc.
 */
void* vz_alloc(DWORD size) {
    return malloc(size);
}

/*
 * vz_free
 * Wrapper around standard libc free.
 */
void vz_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

/* 
 * EMS.ASM mock functions (since EMS is abolished)
 * Any calls to EMS operations from parsed assemblies will map here.
 */
int ems_init(void) {
    return FALSE; /* EMS is not used in OS32 translation */
}

void ems_term(void) {
    /* No operation needed */
}

/*
 * 1行入力プロンプト (ミニバッファ)
 * 戻り値: 1=OK(Enter), 0=Canceled(ESC)
 */
int vz_prompt_input(const char* prompt, char* buf, int buf_size)
{
    int pos = 0;
    while (buf[pos] != '\0') pos++;

    while (1) {
        su_prompt(prompt, buf, pos);

        if (vz_kbhit()) {
            int raw = vz_getch();  /* キーコードデータ(u16) */
            int kd = KEYDATA(raw); /* ASCII部 */
            int kc = KEYCODE(raw); /* スキャンコード部 */
            if (kd == 0x1B) { /* ESC */
                return 0;
            } else if (kd == 0x0D || kd == 0x0A) { /* Enter */
                return 1;
            } else if (kd == 0x08 || kc == VK_BS) { /* Backspace */
                if (pos > 0) {
                    pos--;
                    buf[pos] = '\0';
                }
            } else if (kd >= ' ' && kd <= '~') {
                if (pos < buf_size - 1) {
                    buf[pos++] = (char)kd;
                    buf[pos] = '\0';
                }
            }
        }
    }
}
