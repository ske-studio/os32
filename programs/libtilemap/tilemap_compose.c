#include <string.h>
#include "tilemap_internal.h"

/* ======================================================================== */
/*  ASMルーチン宣言                                                         */
/* ======================================================================== */

extern void __cdecl asm_tile_pair_opaque(
    u8 **bb_planes, int dst_off, int bb_pitch,
    const u8 *tile_l, const u8 *tile_r, int tile_psz);

extern void __cdecl asm_tile_pair_masked(
    u8 **bb_planes, int dst_off, int bb_pitch,
    const u8 *tile_l, const u8 *tile_r, int tile_psz,
    u8 *cov_pair);

extern const u8 _asm_byte_reverse_lut[256];

/* ======================================================================== */
/*  静的バッファ (BSS — heap確保を排除)                                     */
/* ======================================================================== */

/* カバレッジマスク: 2タイルペア単位 (DWORD/行)
 * 12ペア × 24行 × (4bytes × 16行) = 18,432 bytes */
#define COV_PAIRS       (TILEMAP_COLS / 2)  /* 12 */
#define COV_PAIR_BYTES  (4 * TILE_H)        /* 64 */

static u8 s_cov_mask[TILEMAP_ROWS][COV_PAIRS][COV_PAIR_BYTES];
static u8 s_cov_full[TILEMAP_ROWS][COV_PAIRS];
static u8 s_cell_dirty[TILEMAP_ROWS][TILEMAP_COLS];

/* ======================================================================== */
/*  ユーティリティ                                                          */
/* ======================================================================== */

/* TileDef から一時的な GFX_Surface を作成 */
static void tile_to_surface(GFX_Surface *s, const TileDef *t)
{
    s->w = TILE_W;
    s->h = TILE_H;
    s->pitch = TILE_PITCH;
    s->planes[0] = (u8 *)t->planes[0];
    s->planes[1] = (u8 *)t->planes[1];
    s->planes[2] = (u8 *)t->planes[2];
    s->planes[3] = (u8 *)t->planes[3];
    s->_pool_idx = -1;
}

/* H/Vフリップ済みタイルデータを一時バッファに生成 (LUT使用) */
static void tile_to_surface_flipped(GFX_Surface *s, const TileDef *t,
                                    int hflip, int vflip,
                                    u8 flip_buf[4][TILE_PLANE_SZ])
{
    int p, row;
    for (p = 0; p < 4; p++) {
        for (row = 0; row < TILE_H; row++) {
            int src_row = vflip ? (TILE_H - 1 - row) : row;
            u8 b0 = t->planes[p][src_row * TILE_PITCH];
            u8 b1 = t->planes[p][src_row * TILE_PITCH + 1];

            if (hflip) {
                flip_buf[p][row * TILE_PITCH]     = _asm_byte_reverse_lut[b1];
                flip_buf[p][row * TILE_PITCH + 1] = _asm_byte_reverse_lut[b0];
            } else {
                flip_buf[p][row * TILE_PITCH]     = b0;
                flip_buf[p][row * TILE_PITCH + 1] = b1;
            }
        }
        s->planes[p] = flip_buf[p];
    }
    s->w = TILE_W;
    s->h = TILE_H;
    s->pitch = TILE_PITCH;
    s->_pool_idx = -1;
}

/* スクロールを考慮したタイル描画座標とsrc_rectを計算 */
static int calc_tile_draw(int col, int row,
                          i16 scroll_x, i16 scroll_y,
                          int origin_x, int origin_y,
                          int *out_dx, int *out_dy,
                          GFX_Rect *out_sr, int *use_sr)
{
    int map_x = col * TILE_W - scroll_x;
    int map_y = row * TILE_H - scroll_y;
    int dx, dy, sx, sy, sw, sh;

    while (map_x < -TILE_W) map_x += TILEMAP_COLS * TILE_W;
    while (map_x >= TILEMAP_COLS * TILE_W) map_x -= TILEMAP_COLS * TILE_W;
    while (map_y < -TILE_H) map_y += TILEMAP_ROWS * TILE_H;
    while (map_y >= TILEMAP_ROWS * TILE_H) map_y -= TILEMAP_ROWS * TILE_H;

    dx = origin_x + map_x;
    dy = origin_y + map_y;
    sx = 0; sy = 0; sw = TILE_W; sh = TILE_H;

    if (dx < origin_x) {
        int clip = origin_x - dx;
        sx += clip; sw -= clip; dx = origin_x;
    }
    if (dy < origin_y) {
        int clip = origin_y - dy;
        sy += clip; sh -= clip; dy = origin_y;
    }
    if (dx + sw > origin_x + TILEMAP_COLS * TILE_W) {
        sw = origin_x + TILEMAP_COLS * TILE_W - dx;
    }
    if (dy + sh > origin_y + TILEMAP_ROWS * TILE_H) {
        sh = origin_y + TILEMAP_ROWS * TILE_H - dy;
    }

    if (sw <= 0 || sh <= 0) return 0;

    *out_dx = dx;
    *out_dy = dy;
    if (sx != 0 || sy != 0 || sw != TILE_W || sh != TILE_H) {
        out_sr->x = sx;
        out_sr->y = sy;
        out_sr->w = sw;
        out_sr->h = sh;
        *use_sr = 1;
    } else {
        *use_sr = 0;
    }
    return 1;
}

/* タイルの GFX_Surface を準備する (反転を含む) */
static void prepare_tile_surface(GFX_Surface *surf, const TileDef *tile,
                                 u16 attr,
                                 u8 flip_buf[4][TILE_PLANE_SZ])
{
    int hflip = TILEMAP_HFLIP(attr);
    int vflip = TILEMAP_VFLIP(attr);

    if (hflip || vflip) {
        tile_to_surface_flipped(surf, tile, hflip, vflip, flip_buf);
    } else {
        tile_to_surface(surf, tile);
    }
}

/* カバレッジペア (64 bytes) が完全カバー済みか DWORD 判定 */
static int check_cov_full_64(const u8 *cov)
{
    const u32 *p = (const u32 *)cov;
    int i;
    for (i = 0; i < COV_PAIR_BYTES / 4; i++) {
        if (p[i] != 0xFFFFFFFF) return 0;
    }
    return 1;
}

/* カバレッジペア (64 bytes) が空か DWORD 判定 */
static int check_cov_empty_64(const u8 *cov)
{
    const u32 *p = (const u32 *)cov;
    int i;
    for (i = 0; i < COV_PAIR_BYTES / 4; i++) {
        if (p[i] != 0) return 0;
    }
    return 1;
}

/* ダーティセルの検出とクリア (共通) */
static void detect_dirty(void)
{
    int row, col, bg;
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            s_cell_dirty[row][col] = 0;
            for (bg = 0; bg < BG_COUNT; bg++) {
                if (_tilemap.bg_planes[bg].dirty[row][col]) {
                    s_cell_dirty[row][col] = 1;
                    _tilemap.bg_planes[bg].dirty[row][col] = 0;
                }
            }
        }
    }
}

/* ======================================================================== */
/*  Back-to-Front 合成                                                      */
/* ======================================================================== */

void tilemap_compose_btf(void)
{
    int row, col, bg;
    int any_drawn = 0;
    GFX_Surface tmp_surf;
    u8 flip_buf[4][TILE_PLANE_SZ];

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    detect_dirty();

    gfx_dirty_suppress = 1;

    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            if (!s_cell_dirty[row][col]) continue;

            for (bg = 0; bg < BG_COUNT; bg++) {
                int tile_id;
                u16 attr;
                TileDef *tile;
                int dx, dy, use_sr;
                GFX_Rect sr;

                if (!_tilemap.bg_planes[bg].visible) continue;

                attr = _tilemap.bg_planes[bg].map[row][col];
                tile_id = TILEMAP_ID(attr);
                tile = &_tilemap.tiles[tile_id];

                if (tile->opacity == TILE_TRANSPARENT) continue;

                if (!calc_tile_draw(col, row,
                                    _tilemap.bg_planes[bg].scroll_x,
                                    _tilemap.bg_planes[bg].scroll_y,
                                    _tilemap.origin_x, _tilemap.origin_y,
                                    &dx, &dy, &sr, &use_sr))
                    continue;

                prepare_tile_surface(&tmp_surf, tile, attr, flip_buf);

                if (tile->opacity == TILE_OPAQUE) {
                    gfx_blit(dx, dy, &tmp_surf, use_sr ? &sr : NULL);
                } else {
                    gfx_blit_transparent(dx, dy, &tmp_surf, use_sr ? &sr : NULL);
                }
                any_drawn = 1;
            }
        }
    }

    gfx_dirty_suppress = 0;

    /* dirty rect を1回だけ登録 */
    if (any_drawn) {
        _tilemap.kapi->gfx_add_dirty_rect(
            _tilemap.origin_x, _tilemap.origin_y,
            TILEMAP_COLS * TILE_W, TILEMAP_ROWS * TILE_H);
    }
}

/* ======================================================================== */
/*  Front-to-Back 合成 (DWORD ペア転送 + BSS カバレッジ)                    */
/* ======================================================================== */

void tilemap_compose_ftb(void)
{
    int row, col, bg;
    int any_drawn = 0;

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    detect_dirty();

    memset(s_cov_mask, 0, sizeof(s_cov_mask));
    memset(s_cov_full, 0, sizeof(s_cov_full));

    /* BG3(最前面) → BG0(最背面) */
    for (bg = BG_COUNT - 1; bg >= 0; bg--) {
        if (!_tilemap.bg_planes[bg].visible) continue;

        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col += 2) {
                int pair = col >> 1;
                int tile_id_l, tile_id_r;
                TileDef *tile_l, *tile_r;
                int dx, dy, dst_off;
                u8 *cov_pair;

                /* ペア内どちらかがdirtyなら処理 */
                if (!s_cell_dirty[row][col] &&
                    !s_cell_dirty[row][col + 1]) continue;
                if (s_cov_full[row][pair]) continue;

                tile_id_l = TILEMAP_ID(
                    _tilemap.bg_planes[bg].map[row][col]);
                tile_id_r = TILEMAP_ID(
                    _tilemap.bg_planes[bg].map[row][col + 1]);
                tile_l = &_tilemap.tiles[tile_id_l];
                tile_r = &_tilemap.tiles[tile_id_r];

                /* 両方透明 → スキップ */
                if (tile_l->opacity == TILE_TRANSPARENT &&
                    tile_r->opacity == TILE_TRANSPARENT) continue;

                dx = _tilemap.origin_x + col * TILE_W;
                dy = _tilemap.origin_y + row * TILE_H;
                dst_off = dy * gfx_fb.pitch + (dx >> 3);
                cov_pair = s_cov_mask[row][pair];

                if (tile_l->opacity == TILE_OPAQUE &&
                    tile_r->opacity == TILE_OPAQUE &&
                    check_cov_empty_64(cov_pair)) {
                    asm_tile_pair_opaque(
                        gfx_fb.planes, dst_off, gfx_fb.pitch,
                        tile_l->planes[0], tile_r->planes[0],
                        TILE_PLANE_SZ);
                    s_cov_full[row][pair] = 1;
                    memset(cov_pair, 0xFF, COV_PAIR_BYTES);
                } else {
                    asm_tile_pair_masked(
                        gfx_fb.planes, dst_off, gfx_fb.pitch,
                        tile_l->planes[0], tile_r->planes[0],
                        TILE_PLANE_SZ, cov_pair);
                    if (check_cov_full_64(cov_pair))
                        s_cov_full[row][pair] = 1;
                }
                any_drawn = 1;
            }
        }
    }

    /* dirty rect を1回だけ登録 */
    if (any_drawn) {
        _tilemap.kapi->gfx_add_dirty_rect(
            _tilemap.origin_x, _tilemap.origin_y,
            TILEMAP_COLS * TILE_W, TILEMAP_ROWS * TILE_H);
    }
}

/* ======================================================================== */
/*  2タイル同時 32bit 転送 (ASM使用)                                        */
/* ======================================================================== */

void tilemap_compose_btf_fast(void)
{
    int row, col, bg;
    int any_drawn = 0;

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    detect_dirty();

    for (bg = 0; bg < BG_COUNT; bg++) {
        if (!_tilemap.bg_planes[bg].visible) continue;

        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col += 2) {
                int dx, dy;
                int tile_id_l, tile_id_r;
                TileDef *tile_l, *tile_r;

                if (!s_cell_dirty[row][col] &&
                    !s_cell_dirty[row][col + 1]) continue;

                tile_id_l = TILEMAP_ID(
                    _tilemap.bg_planes[bg].map[row][col]);
                tile_id_r = TILEMAP_ID(
                    _tilemap.bg_planes[bg].map[row][col + 1]);
                tile_l = &_tilemap.tiles[tile_id_l];
                tile_r = &_tilemap.tiles[tile_id_r];

                dx = _tilemap.origin_x + col * TILE_W;
                dy = _tilemap.origin_y + row * TILE_H;

                if (tile_l->opacity == TILE_OPAQUE &&
                    tile_r->opacity == TILE_OPAQUE) {
                    int dst_off = dy * gfx_fb.pitch + (dx >> 3);
                    asm_tile_pair_opaque(
                        gfx_fb.planes, dst_off, gfx_fb.pitch,
                        tile_l->planes[0], tile_r->planes[0],
                        TILE_PLANE_SZ);
                } else {
                    GFX_Surface tmp;
                    if (tile_l->opacity != TILE_TRANSPARENT) {
                        tile_to_surface(&tmp, tile_l);
                        if (tile_l->opacity == TILE_OPAQUE)
                            gfx_blit(dx, dy, &tmp, NULL);
                        else
                            gfx_blit_transparent(dx, dy, &tmp, NULL);
                    }
                    if (tile_r->opacity != TILE_TRANSPARENT) {
                        tile_to_surface(&tmp, tile_r);
                        if (tile_r->opacity == TILE_OPAQUE)
                            gfx_blit(dx + TILE_W, dy, &tmp, NULL);
                        else
                            gfx_blit_transparent(
                                dx + TILE_W, dy, &tmp, NULL);
                    }
                }
                any_drawn = 1;
            }
        }
    }

    /* dirty rect を1回だけ登録 */
    if (any_drawn) {
        _tilemap.kapi->gfx_add_dirty_rect(
            _tilemap.origin_x, _tilemap.origin_y,
            TILEMAP_COLS * TILE_W, TILEMAP_ROWS * TILE_H);
    }
}

/* ======================================================================== */
/*  差分スクロール合成                                                      */
/*                                                                          */
/*  BB 上のタイル領域を memmove でバイト単位シフトし、                       */
/*  新規露出タイル列のみ再描画する。上層BG の非透明タイルも再描画。          */
/*  スクロールが 8px 非境界の場合や大ジャンプ時は全面再描画にフォールバック。 */
/* ======================================================================== */

/* BB のタイル領域を水平バイトシフト (4プレーン全行) */
static void bb_shift_horiz(int shift_bytes)
{
    int p, row;
    int base_byte = _tilemap.origin_x >> 3;
    int tile_bytes = (TILEMAP_COLS * TILE_W) >> 3;  /* 48 */
    int tile_h = TILEMAP_ROWS * TILE_H;             /* 384 */
    int abs_shift = (shift_bytes < 0) ? -shift_bytes : shift_bytes;

    if (abs_shift >= tile_bytes) return;  /* 全面シフト=意味なし */

    for (p = 0; p < 4; p++) {
        u8 *plane = gfx_fb.planes[p];
        for (row = 0; row < tile_h; row++) {
            u8 *line = plane + (_tilemap.origin_y + row) * gfx_fb.pitch
                       + base_byte;
            if (shift_bytes > 0) {
                /* 左シフト: 内容が左へずれる (カメラ右移動) */
                memmove(line, line + shift_bytes,
                        tile_bytes - shift_bytes);
                memset(line + tile_bytes - shift_bytes, 0, shift_bytes);
            } else {
                /* 右シフト: 内容が右へずれる (カメラ左移動) */
                memmove(line + abs_shift, line,
                        tile_bytes - abs_shift);
                memset(line, 0, abs_shift);
            }
        }
    }
}

/* 指定列のタイルを全BG分描画 (BtF順)
 * screen_col: BB上の描画位置 (0-23)
 * map_col:    タイルマップ上の列インデックス (0-23, ラップ済み)
 */
static void draw_column_btf(int screen_col, int map_col)
{
    int row, bg;
    GFX_Surface tmp_surf;
    u8 flip_buf[4][TILE_PLANE_SZ];

    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (bg = 0; bg < BG_COUNT; bg++) {
            int tile_id;
            u16 attr;
            TileDef *tile;
            int dx, dy;

            if (!_tilemap.bg_planes[bg].visible) continue;

            attr = _tilemap.bg_planes[bg].map[row][map_col];
            tile_id = TILEMAP_ID(attr);
            tile = &_tilemap.tiles[tile_id];

            if (tile->opacity == TILE_TRANSPARENT) continue;

            /* BB上の固定位置に描画 */
            dx = _tilemap.origin_x + screen_col * TILE_W;
            dy = _tilemap.origin_y + row * TILE_H;

            prepare_tile_surface(&tmp_surf, tile, attr, flip_buf);

            if (tile->opacity == TILE_OPAQUE) {
                gfx_blit(dx, dy, &tmp_surf, NULL);
            } else {
                gfx_blit_transparent(dx, dy, &tmp_surf, NULL);
            }
        }
    }
}

/* 上層BG (bg >= 1) の非透明タイルを全列再描画 */
static void redraw_upper_bgs(void)
{
    int row, col, bg;
    GFX_Surface tmp_surf;
    u8 flip_buf[4][TILE_PLANE_SZ];

    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            for (bg = 1; bg < BG_COUNT; bg++) {
                int tile_id;
                u16 attr;
                TileDef *tile;
                int dx, dy;

                if (!_tilemap.bg_planes[bg].visible) continue;

                attr = _tilemap.bg_planes[bg].map[row][col];
                tile_id = TILEMAP_ID(attr);
                tile = &_tilemap.tiles[tile_id];

                if (tile->opacity == TILE_TRANSPARENT) continue;

                dx = _tilemap.origin_x + col * TILE_W;
                dy = _tilemap.origin_y + row * TILE_H;

                prepare_tile_surface(&tmp_surf, tile, attr, flip_buf);

                if (tile->opacity == TILE_OPAQUE) {
                    gfx_blit(dx, dy, &tmp_surf, NULL);
                } else {
                    gfx_blit_transparent(dx, dy, &tmp_surf, NULL);
                }
            }
        }
    }
}

/* 全タイルを強制ダーティ化 (内部フォールバック用) */
static void force_all_dirty_internal(void)
{
    int bg, row, col;
    for (bg = 0; bg < BG_COUNT; bg++) {
        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col++) {
                _tilemap.bg_planes[bg].dirty[row][col] = 1;
            }
        }
    }
}

void tilemap_compose_scroll(void)
{
    int bg;
    int dx_total = 0;
    int need_full_redraw = 0;
    int has_upper_bg = 0;

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    /* スクロール変化がなければ通常の btf で処理 */
    if (!_tilemap.scroll_changed) {
        tilemap_compose_btf();
        return;
    }

    /* 水平スクロール差分を計算 (BG0のみ対応、他BGはフォールバック) */
    for (bg = 0; bg < BG_COUNT; bg++) {
        i16 sx = _tilemap.bg_planes[bg].scroll_x;
        i16 prev = _tilemap.prev_scroll_x[bg];

        if (bg == 0) {
            dx_total = sx - prev;
        } else if (sx != _tilemap.prev_scroll_x[bg] ||
                   _tilemap.bg_planes[bg].scroll_y !=
                   _tilemap.prev_scroll_y[bg]) {
            /* BG1以上がスクロールした場合はフォールバック */
            need_full_redraw = 1;
        }
        if (bg >= 1 && _tilemap.bg_planes[bg].visible) {
            has_upper_bg = 1;
        }
    }

    /* BG0の垂直スクロールが変化した場合もフォールバック */
    if (_tilemap.bg_planes[0].scroll_y != _tilemap.prev_scroll_y[0]) {
        need_full_redraw = 1;
    }

    /* 8px境界チェック: 非境界のスクロールはフォールバック */
    if ((dx_total & 7) != 0) {
        need_full_redraw = 1;
    }

    /* 大ジャンプ (全タイル幅の半分以上) もフォールバック */
    {
        int abs_dx = (dx_total < 0) ? -dx_total : dx_total;
        if (abs_dx >= (TILEMAP_COLS * TILE_W) / 2) {
            need_full_redraw = 1;
        }
    }

    if (need_full_redraw) {
        /* フォールバック: 全面再描画 */
        force_all_dirty_internal();
        /* prev_scroll を更新してから btf を呼ぶ */
        for (bg = 0; bg < BG_COUNT; bg++) {
            _tilemap.prev_scroll_x[bg] = _tilemap.bg_planes[bg].scroll_x;
            _tilemap.prev_scroll_y[bg] = _tilemap.bg_planes[bg].scroll_y;
        }
        _tilemap.scroll_changed = 0;
        tilemap_compose_btf();
        return;
    }

    /* ---- 差分スクロール処理 ---- */
    gfx_dirty_suppress = 1;

    if (dx_total != 0) {
        int shift_bytes = dx_total >> 3;   /* ピクセル → バイト */
        int abs_bytes = (shift_bytes < 0) ? -shift_bytes : shift_bytes;
        int cols_exposed;
        int start_col, c;

        /* 1. BB のタイル領域を水平シフト */
        bb_shift_horiz(shift_bytes);

        /* 2. 新たに露出した端列のタイルを描画 */
        cols_exposed = (abs_bytes * 8 + TILE_W - 1) / TILE_W;
        if (cols_exposed < 1) cols_exposed = 1;
        if (cols_exposed > TILEMAP_COLS) cols_exposed = TILEMAP_COLS;

        if (dx_total > 0) {
            /* カメラ右移動 → 右端に新タイル */
            start_col = TILEMAP_COLS - cols_exposed;
        } else {
            /* カメラ左移動 → 左端に新タイル */
            start_col = 0;
        }

        {
            /* スクロール位置からBB上の各列に対応するマップ列を計算 */
            i16 sx = _tilemap.bg_planes[0].scroll_x;
            int first_map_col = (sx / TILE_W) % TILEMAP_COLS;

            for (c = start_col; c < start_col + cols_exposed; c++) {
                int map_col = (first_map_col + c) % TILEMAP_COLS;
                draw_column_btf(c, map_col);
            }
        }

        /* 3. 上層BG (BG1+) の非透明タイルを再描画 (BBシフトで位置ずれを補正) */
        if (has_upper_bg) {
            redraw_upper_bgs();
        }
    }

    /* 通常の dirty タイルも処理 (tilemap_set による個別変更) */
    detect_dirty();
    {
        int row, col;
        GFX_Surface tmp_surf;
        u8 flip_buf[4][TILE_PLANE_SZ];

        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col++) {
                int bg2;
                if (!s_cell_dirty[row][col]) continue;
                for (bg2 = 0; bg2 < BG_COUNT; bg2++) {
                    int tile_id;
                    u16 attr;
                    TileDef *tile;
                    int ddx, ddy;

                    if (!_tilemap.bg_planes[bg2].visible) continue;
                    attr = _tilemap.bg_planes[bg2].map[row][col];
                    tile_id = TILEMAP_ID(attr);
                    tile = &_tilemap.tiles[tile_id];
                    if (tile->opacity == TILE_TRANSPARENT) continue;

                    ddx = _tilemap.origin_x + col * TILE_W;
                    ddy = _tilemap.origin_y + row * TILE_H;

                    prepare_tile_surface(&tmp_surf, tile, attr, flip_buf);
                    if (tile->opacity == TILE_OPAQUE) {
                        gfx_blit(ddx, ddy, &tmp_surf, NULL);
                    } else {
                        gfx_blit_transparent(ddx, ddy, &tmp_surf, NULL);
                    }
                }
            }
        }
    }

    gfx_dirty_suppress = 0;

    /* dirty rect 登録 */
    _tilemap.kapi->gfx_add_dirty_rect(
        _tilemap.origin_x, _tilemap.origin_y,
        TILEMAP_COLS * TILE_W, TILEMAP_ROWS * TILE_H);

    /* prev_scroll 更新 */
    for (bg = 0; bg < BG_COUNT; bg++) {
        _tilemap.prev_scroll_x[bg] = _tilemap.bg_planes[bg].scroll_x;
        _tilemap.prev_scroll_y[bg] = _tilemap.bg_planes[bg].scroll_y;
    }
    _tilemap.scroll_changed = 0;
}

void tilemap_present(void)
{
    if (_tilemap.kapi) {
        _tilemap.kapi->gfx_present_rect(
            _tilemap.origin_x, _tilemap.origin_y,
            TILEMAP_COLS * TILE_W, TILEMAP_ROWS * TILE_H);
    }
}
