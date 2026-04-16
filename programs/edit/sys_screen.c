/*
 * sys_screen.c - OS32 Screen Wrapper for VZ Editor
 *
 * lconsole API への委譲レイヤー。
 * C89 compatible
 */
#include "vz.h"

void su_invalidate_vram(void) {
    if (kapi) lcons_clear();
}

void vz_clear(void) {
    if (kapi) lcons_clear();
}

void vz_set_cursor(int x, int y) {
    if (!kapi) return;
    lcons_set_cursor(x, y);
    lcons_show_cursor(1);
}

void vz_putc(int x, int y, char ch, unsigned char attr) {
    if (kapi) lcons_putc(x, y, ch, attr);
}

void vz_putkanji(int x, int y, unsigned short jis, unsigned char attr) {
    if (kapi) lcons_putkanji(x, y, jis, attr);
}

void su_sync_vram(void) {
    if (kapi) lcons_sync_vram();
}
