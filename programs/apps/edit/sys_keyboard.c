/*
 * sys_keyboard.c - OS32 Keyboard Wrapper for VZ Editor
 *
 * kbd_trygetkey()を使用してキーコードデータ(u16)を取得。
 * 戻り値: 上位バイト=PC-98スキャンコード、下位バイト=ASCIIキーデータ
 * PC9800Bible 表2-14 のキーコードデータ形式に準拠。
 */
#include "vz.h"

static int kb_buf = -1;

int vz_kbhit(void) {
    int c;
    if (!kapi) return 0;
    if (kb_buf != -1) return 1;

    /* 1. シリアル入力をチェック（リモートデバッグ用） */
    if (kapi->serial_is_initialized()) {
        c = kapi->serial_trygetchar();
        if (c != -1) {
            kb_buf = c; /* シリアルはASCIIのみ（下位バイト） */
            return 1;
        }
    }

    /* 2. 物理キーボード（キーコードデータ形式で取得） */
    c = kapi->kbd_trygetkey();
    if (c != -1) {
        kb_buf = c;
        return 1;
    }

    return 0;
}

int vz_getch(void) {
    int c;
    if (kb_buf != -1) {
        c = kb_buf;
        kb_buf = -1;
        return c;
    }

    /* キー入力待ち */
    while (!vz_kbhit()) {
        /* wait */
    }

    c = kb_buf;
    kb_buf = -1;
    return c;
}

unsigned int vz_get_modifiers(void) {
    if (!kapi) return 0;
    return kapi->kbd_get_modifiers();
}
