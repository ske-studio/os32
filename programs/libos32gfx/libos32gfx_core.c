#include "libos32gfx.h"

KernelAPI *gfx_api;
GFX_Framebuffer gfx_fb;

void libos32gfx_init(KernelAPI *api)
{
    gfx_api = api;
    gfx_api->gfx_init();
    gfx_api->gfx_get_framebuffer(&gfx_fb);
    
    gfx_surface_init();
    gfx_sprite_init();
}

void libos32gfx_shutdown(void)
{
    gfx_api->gfx_shutdown();
}

void gfx_present(void)
{
    /* 画面全体が更新された場合 */
    gfx_api->gfx_add_dirty_rect(0, 0, gfx_fb.width, gfx_fb.height);
}
