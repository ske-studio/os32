#include "gfx_internal.h"

int vram_scroll_y = 0;

void gfx_vsync_wait(void)
{
    while ((_in(GDC_PRM_PORT) & 0x20) != 0);
    while ((_in(GDC_PRM_PORT) & 0x20) == 0);
}

void gfx_scroll_init(void)
{
    vram_scroll_y = 0;
    gfx_hardware_scroll(0);
}

void gfx_hardware_scroll(int lines)
{
    int sad1, sl1, sad2, sl2;
    u8 params[8];
    int i;

    vram_scroll_y += lines;
    while (vram_scroll_y < 0) vram_scroll_y += GFX_HEIGHT;
    vram_scroll_y %= GFX_HEIGHT;

    sad1 = vram_scroll_y * GFX_WPL;
    sl1  = GFX_HEIGHT - vram_scroll_y;
    sad2 = 0;
    sl2  = vram_scroll_y;

    params[0] = sad1 & 0xFF;
    params[1] = (sad1 >> 8) & 0xFF;
    params[2] = (sl1 & 0x0F) << 4;
    params[3] = (sl1 >> 4) & 0x3F;

    params[4] = sad2 & 0xFF;
    params[5] = (sad2 >> 8) & 0xFF;
    params[6] = (sl2 & 0x0F) << 4;
    params[7] = (sl2 >> 4) & 0x3F;

    _out(GDC_CMD_PORT, GDC_CMD_SCROLL);
    gfx_vsync_wait();
    for (i = 0; i < 8; i++) {
        _out(GDC_PRM_PORT, params[i]);
    }
}
