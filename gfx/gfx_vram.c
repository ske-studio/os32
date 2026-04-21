#include "gfx_internal.h"
#include "os32_kapi_shared.h"
#include "cpu_calibrate.h"

static void _flush_dirty_queue(void);
static void _merge_prev_dirty(void);
static void _flip_page(void);

/* ======================================================================== */
/*  KAPI: ダーティレクタングルの追加 (OS管理)                              */
/* ======================================================================== */
void __cdecl gfx_add_dirty_rect(int x, int y, int w, int h)
{
    int aligned_x, aligned_w;
    int right = x + w;
    int bottom = y + h;
    int i;
    GFX_Rect *r;

    /* マージ判定用のギャップ閾値 (px)。
     * この距離以内の矩形は統合する (32pxアライメントに合わせる) */
    int gap = 32;

    int new_x, new_y, new_r, new_b;

    if (w <= 0 || h <= 0) return;

    /* 32ピクセル境界 (4バイト) にアライメント:
     * xは切り捨て、幅は切り上げ
     */
    aligned_x = x & ~31;
    aligned_w = ((right + 31) & ~31) - aligned_x;

    /* 画面クリッピング */
    if (aligned_x < 0) {
        aligned_w += aligned_x;
        aligned_x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (aligned_x + aligned_w > GFX_WIDTH) aligned_w = GFX_WIDTH - aligned_x;
    if (y + h > gfx_current_height) h = gfx_current_height - y;

    if (aligned_w <= 0 || h <= 0) return;

    /* --- マージ判定 --- */
    new_x = aligned_x;
    new_y = y;
    new_r = aligned_x + aligned_w;
    new_b = y + h;

    for (i = 0; i < dirty_queue.count; i++) {
        int ex, ey, er, eb;

        r = &dirty_queue.rects[i];
        ex = r->x;
        ey = r->y;
        er = ex + r->w;
        eb = ey + r->h;

        /* 完全包含チェック: 新rectが既存rectに包含されるなら何もしない */
        if (new_x >= ex && new_y >= ey && new_r <= er && new_b <= eb) {
            return;
        }

        /* オーバーラップ or 隣接(gap px以内)なら統合 */
        if (new_x <= er + gap && new_r >= ex - gap &&
            new_y <= eb + gap && new_b >= ey - gap) {
            /* バウンディングボックスに統合 */
            if (new_x < ex) r->x = new_x; else new_x = ex;
            if (new_y < ey) r->y = new_y; else new_y = ey;
            if (new_r > er) er = new_r;
            if (new_b > eb) eb = new_b;
            r->w = er - r->x;
            r->h = eb - r->y;
            return;
        }
    }

    /* マージ対象なし → 新規追加 */

    /* キューがいっぱいの場合はバウンディングボックスに圧縮 */
    if (dirty_queue.count >= MAX_DIRTY_RECTS) {
        int min_x = GFX_WIDTH, min_y = gfx_current_height, max_x = 0, max_y = 0;
        for (i = 0; i < dirty_queue.count; i++) {
            if (dirty_queue.rects[i].x < min_x) min_x = dirty_queue.rects[i].x;
            if (dirty_queue.rects[i].y < min_y) min_y = dirty_queue.rects[i].y;
            if (dirty_queue.rects[i].x + dirty_queue.rects[i].w > max_x) max_x = dirty_queue.rects[i].x + dirty_queue.rects[i].w;
            if (dirty_queue.rects[i].y + dirty_queue.rects[i].h > max_y) max_y = dirty_queue.rects[i].y + dirty_queue.rects[i].h;
        }
        dirty_queue.count = 1;
        dirty_queue.rects[0].x = min_x;
        dirty_queue.rects[0].y = min_y;
        dirty_queue.rects[0].w = max_x - min_x;
        dirty_queue.rects[0].h = max_y - min_y;
    }

    r = &dirty_queue.rects[dirty_queue.count++];
    r->x = new_x;
    r->y = new_y;
    r->w = new_r - new_x;
    r->h = new_b - new_y;
}

/* ======================================================================== */
/*  ダーティレクタングルのVRAM転送 (VSYNC同期内等で呼ばれる)              */
/* ======================================================================== */
static void _flush_dirty_queue(void)
{
    int i, row;
    u8 *vb_base = (u8 *)VRAM_PLANE_B;
    u8 *vr_base = (u8 *)VRAM_PLANE_R;
    u8 *vg_base = (u8 *)VRAM_PLANE_G;
    u8 *vi_base = (u8 *)VRAM_PLANE_I;

    for (i = 0; i < dirty_queue.count; i++) {
        GFX_Rect *r = &dirty_queue.rects[i];
        int byte_x = r->x >> 3;           /* 横は何バイト目か */
        int byte_w = r->w >> 3;           /* 横幅のバイト数 */
        int words  = byte_w >> 1;         /* 16bit転送するワード数 */
        int physical_y;
        unsigned long phys_off, base_off;

        if (words <= 0) continue;

        physical_y = (r->y + vram_scroll_y) % gfx_current_height;
        phys_off = (unsigned long)physical_y * GFX_BPL + byte_x;
        base_off = (unsigned long)r->y * GFX_BPL + byte_x;

        for (row = 0; row < r->h; row++) {
            _memcpy_w(vb_base + phys_off, bb_b + base_off, words);
            _memcpy_w(vr_base + phys_off, bb_r + base_off, words);
            _memcpy_w(vg_base + phys_off, bb_g + base_off, words);
            _memcpy_w(vi_base + phys_off, bb_i + base_off, words);

            base_off += GFX_BPL;
            phys_off += GFX_BPL;
            physical_y++;
            if (physical_y >= gfx_current_height) {
                physical_y = 0;
                phys_off = byte_x;
            }
        }
    }
    dirty_queue.count = 0;
}

/* ======================================================================== */
/*  ステイルページ対策: 前フレームのdirty rectを現フレームにマージ            */
/* ======================================================================== */
static void _merge_prev_dirty(void)
{
    int i;
    for (i = 0; i < prev_dirty.count; i++) {
        GFX_Rect *r = &prev_dirty.rects[i];
        gfx_add_dirty_rect(r->x, r->y, r->w, r->h);
    }
}

/* ======================================================================== */
/*  ページ切替: 表示ページと描画ページを入れ替える (~3µs)              */
/* ======================================================================== */
static void _flip_page(void)
{
    gfx_display_page ^= 1;
    _out(GDC_DISP_PAGE, gfx_display_page);
    _out(GDC_ACCESS_PAGE, gfx_display_page ^ 1);
}

/* ======================================================================== */
/*  KAPI: 強制でダーティ領域をVSYNC同期転送する                           */
/*                                                                          */
/*  フリップモード時: VSYNC待ちなしで非表示ページに転送→ページ切替。       */
/*  従来モード時: VSYNC待ち→転送 (従来動作維持)。                    */
/* ======================================================================== */
void __cdecl gfx_present_dirty(void)
{
    if (gfx_flip_enabled) {
        DirtyRectQueue snapshot;
        if (dirty_queue.count == 0 && prev_dirty.count == 0) return;
        /* 今フレームのオリジナルdirtyを保存 (マージ前) */
        snapshot = dirty_queue;
        _merge_prev_dirty();
        _flush_dirty_queue();
        prev_dirty = snapshot;
        _flip_page();
    } else {
        if (dirty_queue.count == 0) return;
        /* VSYNC期間になるまで待つ */
        while ((_in(0x60) & 0x20) == 0) { }
        _flush_dirty_queue();
    }
}

/* ======================================================================== */
/*  KAPI: VSYNC待ちなしでダーティ領域を即座にVRAM転送する                  */
/*                                                                          */
/*  フリップモード時: gfx_present_dirty()と同じ動作 (元々VSYNC不要)。    */
/*  従来モード時: VSYNC待ちなしで即座に転送 (従来動作維持)。           */
/* ======================================================================== */
void __cdecl gfx_present_nosync(void)
{
    if (gfx_flip_enabled) {
        DirtyRectQueue snapshot;
        if (dirty_queue.count == 0 && prev_dirty.count == 0) return;
        snapshot = dirty_queue;
        _merge_prev_dirty();
        _flush_dirty_queue();
        prev_dirty = snapshot;
        _flip_page();
    } else {
        if (dirty_queue.count == 0) return;
        _flush_dirty_queue();
    }
}

/* ======================================================================== */
/*  互換KAPI実装 (OS管理に寄せる)                                          */
/* ======================================================================== */
void __cdecl gfx_present(void)
{
    gfx_add_dirty_rect(0, 0, GFX_WIDTH, GFX_HEIGHT);
    gfx_present_dirty();
}

void __cdecl gfx_present_rect(int rx, int ry, int rw, int rh)
{
    gfx_add_dirty_rect(rx, ry, rw, rh);
    gfx_present_dirty();
}

/* ======================================================================== */
/*  1ライン分だけVRAM転送 (全dirty rectの該当ラインのみ)                   */
/* ======================================================================== */
static void _flush_dirty_line(int line)
{
    int i;
    u8 *vb_base = (u8 *)VRAM_PLANE_B;
    u8 *vr_base = (u8 *)VRAM_PLANE_R;
    u8 *vg_base = (u8 *)VRAM_PLANE_G;
    u8 *vi_base = (u8 *)VRAM_PLANE_I;

    for (i = 0; i < dirty_queue.count; i++) {
        GFX_Rect *r = &dirty_queue.rects[i];
        int byte_x, byte_w, words;
        int physical_y;
        unsigned long phys_off, base_off;

        /* このdirty rectにこのラインが含まれるかチェック */
        if (line < r->y || line >= r->y + r->h) continue;

        byte_x = r->x >> 3;
        byte_w = r->w >> 3;
        words  = byte_w >> 1;
        if (words <= 0) continue;

        physical_y = (line + vram_scroll_y) % gfx_current_height;
        phys_off = (unsigned long)physical_y * GFX_BPL + byte_x;
        base_off = (unsigned long)line * GFX_BPL + byte_x;

        _memcpy_w(vb_base + phys_off, bb_b + base_off, words);
        _memcpy_w(vr_base + phys_off, bb_r + base_off, words);
        _memcpy_w(vg_base + phys_off, bb_g + base_off, words);
        _memcpy_w(vi_base + phys_off, bb_i + base_off, words);
    }
}

/* ======================================================================== */
/*  KAPI: ラスタパレット付きVRAM転送                                        */
/*                                                                          */
/*  VSYNC待機後、ラスタパレットテーブルに従ってパレットI/Oを書き換える。      */
/*  NP21/Wのrasterdraw機構はCPUクロック値でイベントをラスタラインに            */
/*  対応付けるため、HBLANK I/Oポーリングではなく固定CPUサイクルの              */
/*  ディレイで書き込み間隔を制御する。                                        */
/*                                                                          */
/*  2ライン ≈ gdc.rasterclock*2 CPUサイクル。                                */
/*  実機8MHz時: 1ライン ≈ 199サイクル → 2ライン ≈ 398サイクル。              */
/*  NOPディレイでこれに近い間隔を確保する。                                  */
/* ======================================================================== */

/* ラスタパレット書き換え間ディレイ
 * NP21/W 実測: 約120µs で全400ラインカバー (100エントリ × 4ライン間隔)。
 * cpu_delay_us() により CPU 速度に自動適応する。 */
#define RASTER_LINE_US  120

static inline void _raster_delay(void)
{
    cpu_delay_us(RASTER_LINE_US);
}

void __cdecl gfx_present_raster(GFX_RasterPalTable *table)
{
    int entry_idx, line;
    int has_dirty;

    if (!table || table->count == 0) return;

    /* フリップモードではラスタパレット非対応:
     * 走査中のパレットI/O操作は表示ページを前提とするため、
     * dirty転送のみ行いパレット書き換えはスキップする */
    if (gfx_flip_enabled) {
        gfx_present_dirty();
        return;
    }

    has_dirty = (dirty_queue.count > 0);

    /* VSYNC期間になるまで待つ */
    while ((_in(0x60) & 0x20) == 0) { }

    /* 割り込み禁止 (全ラスタ期間を通じて安定させる) */
    __asm__ volatile("cli");

    /* VSYNC終了を待つ → アクティブ表示領域が始まる */
    while (_in(GDC_STATUS_PORT) & 0x20) { }

    entry_idx = 0;

    if (has_dirty) {
        /* VRAM転送 + パレット書き換え */
        for (line = 0; line < gfx_current_height; line++) {
            _flush_dirty_line(line);

            while (entry_idx < table->count &&
                   table->entries[entry_idx].line <= (u16)line) {
                GFX_RasterPalEntry *e = &table->entries[entry_idx];
                _out(PAL_IDX_PORT, e->pal_idx);
                _out(PAL_G_PORT, e->g & 0x0F);
                _out(PAL_R_PORT, e->r & 0x0F);
                _out(PAL_B_PORT, e->b & 0x0F);
                entry_idx++;
            }
            _raster_delay();
        }
    } else {
        /* パレット書き換えのみ (VRAM転送なし) */
        for (entry_idx = 0; entry_idx < table->count; entry_idx++) {
            GFX_RasterPalEntry *e = &table->entries[entry_idx];
            _out(PAL_IDX_PORT, e->pal_idx);
            _out(PAL_G_PORT, e->g & 0x0F);
            _out(PAL_R_PORT, e->r & 0x0F);
            _out(PAL_B_PORT, e->b & 0x0F);
            _raster_delay();
        }
    }

    /* 割り込み復帰 */
    __asm__ volatile("sti");

    dirty_queue.count = 0;
}
