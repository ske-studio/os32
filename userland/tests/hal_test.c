/* ======================================================================== */
/*  HAL_TEST.C — GUI HAL 枠 (KAPI v40) の実機確認                            */
/*                                                                          */
/*  gfx_screen_info の内容と、CPU バックエンドで gfx_hw_fill_rect /          */
/*  gfx_hw_blit が OS32_ERR_NOSYS を返すことを表示する。                      */
/* ======================================================================== */

#include "os32api.h"

void main(int argc, char **argv, KernelAPI *api)
{
    GFX_ScreenInfo si;
    int r1, r2, has_hw;

    (void)argc; (void)argv;

    if (api->version < 40) {
        api->kprintf(0x41, "KAPI v%d < 40: HAL entries absent\n", api->version);
        return;
    }
    api->gfx_screen_info(&si);
    /* 選ばれたバックエンド名。GFX_ScreenInfo には名前が無いので format と
     * 能力ビットから引く (票 H2b: gfxmode がどれを選んだかを 1 行で見る)。
     * 票 H3 で 3 枚目 (Cirrus) が入った: パックド 8bpp で HW_FILL/HW_BLT が
     * 立っているのはアクセラレータだけ。 */
    has_hw = (si.flags & (GFX_CAP_HW_FILL | GFX_CAP_HW_BLT)) ? 1 : 0;
    api->kprintf(0xE1, "backend %s (%s)\n",
                 (si.format != GFX_FMT_PACKED8) ? "pc98"
                                                : (has_hw ? "cirrus" : "pegc"),
                 (si.format == GFX_FMT_PACKED8) ? "packed 8bpp" : "planar 4bpp");
    api->kprintf(0xE1, "screen %dx%d bpp=%d fmt=%d flags=0x%x lease_mask=0x%x first=%d count=%d\n",
                 si.width, si.height, si.bpp, si.format, si.flags,
                 si.lease_mask, si.lease_first, si.lease_count);
    r1 = api->gfx_hw_fill_rect(0, 0, 8, 8, 1);
    r2 = api->gfx_hw_blit(0, 0, 8, 8, 8, 8);
    /* 能力ビットと戻り値が一致していることが合格条件 (契約 G5):
     *   HW_FILL/HW_BLT が立つ → 0 が返る (アクセラレータが受けた)
     *   立たない             → OS32_ERR_NOSYS (共有ライブラリが CPU 実装へ) */
    api->kprintf(0xE1, "hw_fill_rect=%d hw_blit=%d (expect %d)\n",
                 r1, r2, has_hw ? 0 : OS32_ERR_NOSYS);
    api->kprintf((has_hw ? (r1 == 0 && r2 == 0)
                         : (r1 == OS32_ERR_NOSYS && r2 == OS32_ERR_NOSYS))
                     ? 0xC1 : 0x41,
                 "%s", "hal_test done\n");
}
