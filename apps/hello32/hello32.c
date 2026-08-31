/* hello32 — OS32 デモアプリ */
#include "os32api.h"
#include "libos32gfx.h"

/* main() はソース中の最初の関数でなければならない (crt0 が先頭へ飛ぶ) */
void main(int argc, char **argv, KernelAPI *api)
{
    int i;
    u32 until;
    (void)argc; (void)argv;

    api->kprintf(0x07, "hello32: start\n");
    libos32gfx_init(api);

    gfx_clear(0);
    for (i = 0; i < 16; i++)
        gfx_fill_rect(40 + i * 34, 160, 30, 80, (u8)i);

    /* gfx_present() はダーティ矩形を積むだけ。転送は KAPI 側。 */
    gfx_present();
    api->gfx_present_dirty();

    /* PIT 100Hz。5 秒保持してから戻る。 */
    until = api->get_tick() + 500;
    while (api->get_tick() < until) { }

    libos32gfx_shutdown();
    api->kprintf(0x07, "hello32: done\n");
}
