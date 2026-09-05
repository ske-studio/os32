#include "gfx_internal.h"
#include "gfx_hal.h"
#include "os32_kapi_shared.h"
#include "kstring.h"

/* ======================================================================== */
/*  バックバッファ (拡張メモリ固定アドレス, 128KB)                          */
/* ======================================================================== */
u8 *bb_b = (u8 *)MEM_GFX_BB_BASE;
u8 *bb_r = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ);
u8 *bb_g = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 2);
u8 *bb_i = (u8 *)(MEM_GFX_BB_BASE + GFX_PLANE_SZ * 3);

int gfx_current_height = GFX_HEIGHT;  /* 200 or 400 */

u8 *bb[4];

DirtyRectQueue dirty_queue = {0};
DirtyRectQueue prev_dirty = {0};   /* 前フレームdirty (ステイルページ対策) */

int gfx_flip_enabled = 0;
int gfx_display_page = 0;

/* ======================================================================== */
/*  HAL バックエンド選択とカウンタ (GUI v1.1, レーン H1)                     */
/*  g_backend は probe 順で選ばれる。静的初期化子で 9801 を指すので、        */
/*  gfx_init 前でも NULL にならない (boot_splash 等が present を呼べる)。     */
/* ======================================================================== */
GfxCounters gfx_counters = { 0, 0, 0, 0 };

const GfxBackend *g_backend = &gfx_backend_pc98;

/* probe 順のバックエンド一覧。v1 は 9801 のみ。H2 (PEGC) / H3 (Cirrus) は
 * 実機に近い順で前へ足す (Cirrus → PEGC → 9801)。 */
static const GfxBackend *const g_backend_list[] = {
    &gfx_backend_pc98
};

static void gfx_select_backend(void)
{
    int i;
    int n = (int)(sizeof(g_backend_list) / sizeof(g_backend_list[0]));
    for (i = 0; i < n; i++) {
        if (g_backend_list[i]->probe && g_backend_list[i]->probe()) {
            g_backend = g_backend_list[i];
            return;
        }
    }
    g_backend = &gfx_backend_pc98;   /* フォールバック */
}

/* ======================================================================== */
/*  KAPI: フレームバッファ取得                                              */
/* ======================================================================== */
void __cdecl gfx_get_framebuffer(GFX_Framebuffer *fb)
{
    if (!fb) return;
    fb->width = GFX_WIDTH;
    fb->height = gfx_current_height;
    fb->pitch = GFX_BPL;
    fb->planes[0] = bb[0];
    fb->planes[1] = bb[1];
    fb->planes[2] = bb[2];
    fb->planes[3] = bb[3];
}

/* ======================================================================== */
/*  KAPI: 画面能力の問い合わせ (GUI HAL, docs/tasks/gui/API_CONTRACTS.md G5)  */
/*  現在の唯一のバックエンドは 9801 プレーン: CPU 直書き、HW 塗り/転送なし。   */
/* ======================================================================== */
void __cdecl gfx_screen_info(void *out)
{
    if (!out) return;
    /* 決め打ちせずバックエンドに問い合わせる (契約 G5)。9801 だけの現状でも
     * 400/200 ラインやフリップ有無はバックエンドが正直に申告する。 */
    if (g_backend && g_backend->init) {
        g_backend->init((GFX_ScreenInfo *)out);
    }
}

/* ======================================================================== */
/*  KAPI: ハードウェア塗り / 転送 (アクセラレータ系バックエンド用の枠)        */
/*  バックエンドが fill_rect / blit を持てばそれへ、無ければ OS32_ERR_NOSYS。 */
/*  呼ぶ側は GFX_CAP_HW_* を見て CPU 実装へフォールバックする。               */
/* ======================================================================== */
int __cdecl gfx_hw_fill_rect(int x, int y, int w, int h, u8 color)
{
    if (g_backend && g_backend->fill_rect)
        return g_backend->fill_rect(x, y, w, h, color);
    return OS32_ERR_NOSYS;
}

int __cdecl gfx_hw_blit(int dx, int dy, int sx, int sy, int w, int h)
{
    if (g_backend && g_backend->blit)
        return g_backend->blit(dx, dy, sx, sy, w, h);
    return OS32_ERR_NOSYS;
}

/* ======================================================================== */
/*  KAPI (v41): 描画カウンタの取得 (契約 G7 / DESIGN §8)。                   */
/*  GFX_Stats を埋める。K1 が kapi.json に載せる弱既定 (NOSYS 返し) を、この   */
/*  強シンボルがリンク時に上書きする。                                       */
/* ======================================================================== */
int __cdecl gfx_stats(void *out)
{
    GFX_Stats *s = (GFX_Stats *)out;
    if (!s) return OS32_ERR_INVAL;
    s->present_bytes = gfx_counters.present_bytes;
    s->hw_ops        = gfx_counters.hw_ops;
    s->io_accesses   = gfx_counters.io_accesses;
    s->commits       = gfx_counters.commits;
    return 0;
}

/* ======================================================================== */
/*  KAPI (v41): パレットのリース (契約 G8)。                                 */
/*  フォーカスのあるアプリ / WM が、バックエンドが許す範囲だけパレットを      */
/*  差し替える。範囲は gfx_screen_info() の lease_mask (16 色) /              */
/*  lease_first, lease_count (256 色) が正典。範囲外は OS32_ERR_INVAL で弾く。 */
/*  H1 は状態を持たない — システム色へ戻すのは WM が同じ関数で行う。          */
/* ======================================================================== */
int __cdecl gfx_lease_palette(int first, int count, const u8 *rgb)
{
    GFX_ScreenInfo si;
    int i;

    if (count <= 0 || first < 0 || !rgb) return OS32_ERR_INVAL;
    if (!g_backend || !g_backend->init || !g_backend->set_palette)
        return OS32_ERR_NOSYS;

    g_backend->init(&si);

    if (si.lease_count > 0) {
        /* 256 色機: 連続範囲 [lease_first, lease_first + lease_count) */
        if (first < (int)si.lease_first ||
            first + count > (int)si.lease_first + (int)si.lease_count)
            return OS32_ERR_INVAL;
    } else {
        /* 16 色機: lease_mask のビットが立つ index だけ貸せる */
        if (first + count > 16) return OS32_ERR_INVAL;
        for (i = first; i < first + count; i++) {
            if (!(si.lease_mask & (u16)(1u << i))) return OS32_ERR_INVAL;
        }
    }

    g_backend->set_palette(first, count, rgb);
    return 0;
}

int gfx_get_height(void)
{
    return gfx_current_height;
}

/* ======================================================================== */
/*  初期化・終了                                                            */
/* ======================================================================== */
/* 共通初期化処理 (テキストVRAMクリア + バックバッファゼロクリア) */
static void _gfx_common_init(int plane_sz)
{
    int i;
    volatile u16 *tvram_char = (volatile u16 *)TVRAM_CHAR_BASE;
    volatile u8  *tvram_attr = (volatile u8  *)TVRAM_ATTR_BASE;

    bb[0] = bb_b; bb[1] = bb_r; bb[2] = bb_g; bb[3] = bb_i;
    dirty_queue.count = 0;

    /* テキストVRAMクリア */
    for (i = 0; i < TVRAM_COLS * TVRAM_ROWS; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }

    /* ゼロクリア (バックバッファ) — kmemset (rep stosd) で高速化 */
    kmemset(bb_b, 0, plane_sz);
    kmemset(bb_r, 0, plane_sz);
    kmemset(bb_g, 0, plane_sz);
    kmemset(bb_i, 0, plane_sz);

    _out(MODE_FF2_PORT, MFF2_16COLOR);
}

void gfx_init(void)
{
    gfx_current_height = GFX_HEIGHT;  /* 400ラインモード */
    gfx_display_page = 0;
    prev_dirty.count = 0;

    _gfx_common_init(GFX_PLANE_SZ);

    /* GDC CSRFORM: L/R=0 (400ライン) */
    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x00);
    _out(MODE_FF1_PORT, MFF1_HIRES);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    /* ページフリッピング有効化: 両ページのVRAMをゼロクリア */
    _out(GDC_ACCESS_PAGE, 0x00);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ);

    _out(GDC_ACCESS_PAGE, 0x01);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ);

    /* ページ0を表示、ページ1に描画 */
    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x01);
    gfx_flip_enabled = 1;

    palette_init();
    gfx_scroll_init();

    /* HAL バックエンドを選び、表示出力を有効化 (9801 の enter は空) */
    gfx_select_backend();
    if (g_backend && g_backend->enter) g_backend->enter();
}

void gfx_init_200(void)
{
    gfx_current_height = GFX_HEIGHT_200;  /* 200ラインモード */

    _gfx_common_init(GFX_PLANE_SZ_200);

    /* GDC CSRFORM: L/R=1 (各ライン2倍表示 → 200ライン) */
    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x01);
    _out(MODE_FF1_PORT, MFF1_200LINE);

    _out(GDC_GFX_CMD, GDC_CMD_START);

    /* ページフリッピング有効化: 両ページのVRAMをゼロクリア */
    _out(GDC_ACCESS_PAGE, 0x00);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ_200);

    _out(GDC_ACCESS_PAGE, 0x01);
    kmemset((u8 *)VRAM_PLANE_B, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_R, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_G, 0, GFX_PLANE_SZ_200);
    kmemset((u8 *)VRAM_PLANE_I, 0, GFX_PLANE_SZ_200);

    /* ページ0を表示、ページ1に描画 */
    _out(GDC_DISP_PAGE, 0x00);
    _out(GDC_ACCESS_PAGE, 0x01);
    gfx_flip_enabled = 1;
    gfx_display_page = 0;
    prev_dirty.count = 0;

    palette_init();
    gfx_scroll_init();

    /* HAL バックエンドを選び、表示出力を有効化 (9801 の enter は空) */
    gfx_select_backend();
    if (g_backend && g_backend->enter) g_backend->enter();
}

void gfx_shutdown(void)
{
    /* 表示出力を戻し (leave)、バックエンドのハードウェア終了処理へ。
     * 9801 では leave は空、shutdown がフリップ解除 + GDC 表示停止を行う
     * (旧 gfx_shutdown の本体は backend_pc98.c の pc98_shutdown に移設)。 */
    if (g_backend && g_backend->leave) g_backend->leave();
    if (g_backend && g_backend->shutdown) g_backend->shutdown();
}
