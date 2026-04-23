#include <string.h>
#include "tilemap_internal.h"

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
    s->_pool_idx = -1;  /* プール外 */
}

/* H/Vフリップ済みタイルデータを一時バッファに生成 */
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
                /* 16ビット全体のビットリバース */
                /* byte0 の bit7..0 → byte1 の bit0..7 (逆順) */
                /* byte1 の bit7..0 → byte0 の bit0..7 (逆順) */
                u8 r0 = 0, r1 = 0;
                int bit;
                for (bit = 0; bit < 8; bit++) {
                    if (b0 & (1 << bit)) r1 |= (0x80 >> bit);
                    if (b1 & (1 << bit)) r0 |= (0x80 >> bit);
                }
                flip_buf[p][row * TILE_PITCH]     = r0;
                flip_buf[p][row * TILE_PITCH + 1] = r1;
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

/* スクロールを考慮したタイル描画座標とsrc_rectを計算
 * 戻り値: 0=描画不要, 1=描画あり */
static int calc_tile_draw(int col, int row,
                          i16 scroll_x, i16 scroll_y,
                          int origin_x, int origin_y,
                          int *out_dx, int *out_dy,
                          GFX_Rect *out_sr, int *use_sr)
{
    int map_x = col * TILE_W - scroll_x;
    int map_y = row * TILE_H - scroll_y;
    int dx, dy, sx, sy, sw, sh;

    /* スクロール範囲ラップ */
    while (map_x < -TILE_W) map_x += TILEMAP_COLS * TILE_W;
    while (map_x >= TILEMAP_COLS * TILE_W) map_x -= TILEMAP_COLS * TILE_W;
    while (map_y < -TILE_H) map_y += TILEMAP_ROWS * TILE_H;
    while (map_y >= TILEMAP_ROWS * TILE_H) map_y -= TILEMAP_ROWS * TILE_H;

    dx = origin_x + map_x;
    dy = origin_y + map_y;
    sx = 0; sy = 0; sw = TILE_W; sh = TILE_H;

    /* 描画領域クリッピング (タイル部分表示) */
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

void tilemap_compose_btf(void)
{
    int row, col, bg;
    u8 cell_dirty[TILEMAP_ROWS][TILEMAP_COLS];
    GFX_Surface tmp_surf;
    u8 flip_buf[4][TILE_PLANE_SZ];

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    /* 1. 全BGを走査してダーティなセルを特定しつつ、フラグをクリア */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            cell_dirty[row][col] = 0;
            for (bg = 0; bg < BG_COUNT; bg++) {
                if (_tilemap.bg_planes[bg].dirty[row][col]) {
                    cell_dirty[row][col] = 1;
                    _tilemap.bg_planes[bg].dirty[row][col] = 0;
                }
            }
        }
    }

    /* 2. ダーティなセルのみ、BG0から順に再描画 */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            if (!cell_dirty[row][col]) continue;

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

                /* 最背面(BG0) または 不透明タイルなら上書きblit (高速パス) */
                if (tile->opacity == TILE_OPAQUE) {
                    gfx_blit(dx, dy, &tmp_surf, use_sr ? &sr : NULL);
                } else {
                    gfx_blit_transparent(dx, dy, &tmp_surf, use_sr ? &sr : NULL);
                }
            }
        }
    }
}

/* ======================================================================== */
/*  Front-to-Back 合成 (カバレッジマスク方式)                               */
/* ======================================================================== */

/*
 * カバレッジマスク: 各タイルセル(16x16px)のピクセルが「既に描画済みか」を
 * ビットマップで管理する。
 *
 * 1タイル = 16行 × 16ピクセル = 16行 × 2バイト = 32バイト/タイル
 * 全セル = 24×24 × 32 = 18,432 バイト ≈ 18KB
 */

#define COV_TILE_SZ  TILE_PLANE_SZ  /* 32 bytes */

/* カバレッジマスクをクリア */
static void cov_clear(u8 *cov_mask, u8 *cov_full)
{
    memset(cov_mask, 0, TILEMAP_ROWS * TILEMAP_COLS * COV_TILE_SZ);
    memset(cov_full, 0, TILEMAP_ROWS * TILEMAP_COLS);
}

/* タイルの1行をカバレッジ付きでBBに直接書き込む */
static void draw_tile_row_masked(
    u8 *bb_planes[4], int bb_pitch,
    const TileDef *tile, int row,
    int dx, int dy_row,
    u8 *cov_row /* 2バイト: このタイル行のカバレッジ */)
{
    int p;
    int doff = dy_row * bb_pitch + (dx >> 3);
    u8 uncovered0 = ~cov_row[0];
    u8 uncovered1 = ~cov_row[1];
    u8 any_bit;

    if (uncovered0 == 0 && uncovered1 == 0) return; /* 既に全て埋まっている */

    for (p = 0; p < 4; p++) {
        u8 src0 = tile->planes[p][row * TILE_PITCH];
        u8 src1 = tile->planes[p][row * TILE_PITCH + 1];

        /* src のうち uncovered な部分だけ BB に OR する
         * 既存のBB値は先行レイヤーで書き込み済み → クリアせずOR */
        bb_planes[p][doff]     = (bb_planes[p][doff]     & cov_row[0]) | (src0 & uncovered0);
        bb_planes[p][doff + 1] = (bb_planes[p][doff + 1] & cov_row[1]) | (src1 & uncovered1);
    }

    /* カバレッジを更新: 色0でないビットを「描画済み」としてマーク */
    any_bit = 0;
    for (p = 0; p < 4; p++) {
        any_bit |= tile->planes[p][row * TILE_PITCH];
    }
    cov_row[0] |= (any_bit & uncovered0);

    any_bit = 0;
    for (p = 0; p < 4; p++) {
        any_bit |= tile->planes[p][row * TILE_PITCH + 1];
    }
    cov_row[1] |= (any_bit & uncovered1);
}

void tilemap_compose_ftb(void)
{
    int row, col, bg, r;
    u8 *cov_mask; /* [TILEMAP_ROWS * TILEMAP_COLS][COV_TILE_SZ] */
    u8 cov_full[TILEMAP_ROWS][TILEMAP_COLS];
    u8 cell_dirty[TILEMAP_ROWS][TILEMAP_COLS];

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    /* カバレッジマスクをヒープ確保 */
    cov_mask = (u8 *)_tilemap.kapi->mem_alloc(
        TILEMAP_ROWS * TILEMAP_COLS * COV_TILE_SZ);
    if (!cov_mask) {
        /* メモリ不足時はBtFにフォールバック */
        tilemap_compose_btf();
        return;
    }

    /* ダーティセルの検出とクリア */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            cell_dirty[row][col] = 0;
            for (bg = 0; bg < BG_COUNT; bg++) {
                if (_tilemap.bg_planes[bg].dirty[row][col]) {
                    cell_dirty[row][col] = 1;
                    _tilemap.bg_planes[bg].dirty[row][col] = 0;
                }
            }
        }
    }

    cov_clear(cov_mask, (u8 *)cov_full);

    /* BG3(最前面) → BG0(最背面) の順で描画 */
    for (bg = BG_COUNT - 1; bg >= 0; bg--) {
        if (!_tilemap.bg_planes[bg].visible) continue;

        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col++) {
                int tile_id;
                u16 attr;
                TileDef *tile;
                int dx, dy;
                u8 *cell_cov;

                if (!cell_dirty[row][col]) continue;

                /* L1高速パス: タイルセルが完全カバー済みならスキップ */
                if (cov_full[row][col]) continue;

                attr = _tilemap.bg_planes[bg].map[row][col];
                tile_id = TILEMAP_ID(attr);
                tile = &_tilemap.tiles[tile_id];

                /* L0高速パス: 透明タイルはスキップ */
                if (tile->opacity == TILE_TRANSPARENT) continue;

                dx = _tilemap.origin_x + col * TILE_W;
                dy = _tilemap.origin_y + row * TILE_H;
                cell_cov = cov_mask +
                    (row * TILEMAP_COLS + col) * COV_TILE_SZ;

                if (tile->opacity == TILE_OPAQUE && cov_full[row][col] == 0) {
                    /* 完全不透明 & カバレッジなし → BB直接書き込み */
                    int p;
                    for (p = 0; p < 4; p++) {
                        int tr;
                        for (tr = 0; tr < TILE_H; tr++) {
                            int off = (dy + tr) * gfx_fb.pitch + (dx >> 3);
                            gfx_fb.planes[p][off]     = tile->planes[p][tr * TILE_PITCH];
                            gfx_fb.planes[p][off + 1] = tile->planes[p][tr * TILE_PITCH + 1];
                        }
                    }
                    cov_full[row][col] = 1;
                    memset(cell_cov, 0xFF, COV_TILE_SZ);
                } else {
                    /* カバレッジマスク付き描画 */
                    for (r = 0; r < TILE_H; r++) {
                        draw_tile_row_masked(
                            gfx_fb.planes, gfx_fb.pitch,
                            tile, r,
                            dx, dy + r,
                            cell_cov + r * TILE_PITCH);
                    }
                    /* 全ビットが立ったか確認 */
                    {
                        int full = 1;
                        int ci;
                        for (ci = 0; ci < COV_TILE_SZ; ci++) {
                            if (cell_cov[ci] != 0xFF) { full = 0; break; }
                        }
                        if (full) cov_full[row][col] = 1;
                    }
                }

                _tilemap.kapi->gfx_add_dirty_rect(dx, dy, TILE_W, TILE_H);
            }
        }
    }

    _tilemap.kapi->mem_free(cov_mask);
}

/* ======================================================================== */
/*  2タイル同時 32bit 転送                                                  */
/* ======================================================================== */

void tilemap_compose_btf_fast(void)
{
    int row, col, bg;
    u8 cell_dirty[TILEMAP_ROWS][TILEMAP_COLS];

    if (!_tilemap.kapi || !_tilemap.bg_planes) return;

    /* 1. ダーティセルの検出とクリア */
    for (row = 0; row < TILEMAP_ROWS; row++) {
        for (col = 0; col < TILEMAP_COLS; col++) {
            cell_dirty[row][col] = 0;
            for (bg = 0; bg < BG_COUNT; bg++) {
                if (_tilemap.bg_planes[bg].dirty[row][col]) {
                    cell_dirty[row][col] = 1;
                    _tilemap.bg_planes[bg].dirty[row][col] = 0;
                }
            }
        }
    }

    /* 2. BG0→BG3の順で描画 (2タイルペア単位でDWORD転送) */
    for (bg = 0; bg < BG_COUNT; bg++) {
        if (!_tilemap.bg_planes[bg].visible) continue;

        for (row = 0; row < TILEMAP_ROWS; row++) {
            for (col = 0; col < TILEMAP_COLS; col += 2) {
                int dx, dy, p, tr;
                int tile_id_l, tile_id_r;
                TileDef *tile_l, *tile_r;
                int dirty_l, dirty_r;

                dirty_l = cell_dirty[row][col];
                dirty_r = (col + 1 < TILEMAP_COLS) ? cell_dirty[row][col + 1] : 0;
                if (!dirty_l && !dirty_r) continue;

                tile_id_l = TILEMAP_ID(_tilemap.bg_planes[bg].map[row][col]);
                tile_l = &_tilemap.tiles[tile_id_l];

                if (col + 1 < TILEMAP_COLS) {
                    tile_id_r = TILEMAP_ID(_tilemap.bg_planes[bg].map[row][col + 1]);
                    tile_r = &_tilemap.tiles[tile_id_r];
                } else {
                    tile_id_r = 0;
                    tile_r = &_tilemap.tiles[0];
                }

                dx = _tilemap.origin_x + col * TILE_W;
                dy = _tilemap.origin_y + row * TILE_H;

                /* 両方OPAQUEなら32bitペア転送 */
                if (tile_l->opacity == TILE_OPAQUE &&
                    tile_r->opacity == TILE_OPAQUE &&
                    col + 1 < TILEMAP_COLS) {
                    for (p = 0; p < 4; p++) {
                        u8 *dst = gfx_fb.planes[p] + dy * gfx_fb.pitch + (dx >> 3);
                        for (tr = 0; tr < TILE_H; tr++) {
                            u32 val =
                                ((u32)tile_l->planes[p][tr * TILE_PITCH]     << 24) |
                                ((u32)tile_l->planes[p][tr * TILE_PITCH + 1] << 16) |
                                ((u32)tile_r->planes[p][tr * TILE_PITCH]     <<  8) |
                                ((u32)tile_r->planes[p][tr * TILE_PITCH + 1]);
                            *(u32 *)(dst + tr * gfx_fb.pitch) = val;
                        }
                    }
                    _tilemap.kapi->gfx_add_dirty_rect(dx, dy, TILE_W * 2, TILE_H);
                } else {
                    /* 個別描画フォールバック */
                    GFX_Surface tmp;
                    if (tile_l->opacity != TILE_TRANSPARENT) {
                        tile_to_surface(&tmp, tile_l);
                        if (tile_l->opacity == TILE_OPAQUE)
                            gfx_blit(dx, dy, &tmp, NULL);
                        else
                            gfx_blit_transparent(dx, dy, &tmp, NULL);
                    }
                    if (col + 1 < TILEMAP_COLS &&
                        tile_r->opacity != TILE_TRANSPARENT) {
                        tile_to_surface(&tmp, tile_r);
                        if (tile_r->opacity == TILE_OPAQUE)
                            gfx_blit(dx + TILE_W, dy, &tmp, NULL);
                        else
                            gfx_blit_transparent(dx + TILE_W, dy, &tmp, NULL);
                    }
                }
            }
        }
    }
}

void tilemap_present(void)
{
    if (_tilemap.kapi) {
        _tilemap.kapi->gfx_present_dirty();
    }
}
