/* ======================================================================== */
/*  PYXEL_GFX.C — libpyxel 描画プリミティブ                                 */
/*                                                                          */
/*  全座標はPyxel仮想座標系 (0,0)-(width-1,height-1)。                      */
/*  内部で PYXEL_SCALE 倍して libos32gfx に渡す。                            */
/*  カメラオフセットとクリッピングは自動適用される。                          */
/*                                                                          */
/*  描画方式: 2倍座標直接描画 (スケーリング工程なし)                         */
/*  VRAM転送: gfx_present_dirty() による dirty rect 方式                    */
/* ======================================================================== */

#include "pyxel_internal.h"

/* ======================================================================== */
/*  内部ヘルパー: 座標変換とdirty rect                                       */
/* ======================================================================== */

/* Pyxel座標 → 実座標 (カメラオフセット適用 + 2倍) */
static int _px(int x) { return (x - _pyxel.cam_x) * PYXEL_SCALE; }
static int _py(int y) { return (y - _pyxel.cam_y) * PYXEL_SCALE; }

/* dirty rect 登録 — libos32gfx の描画プリミティブが
 * 内部で gfx_add_dirty_rect() を呼ぶため、libpyxel 側では
 * 二重登録を避けて何もしない (dirty_rect_optimization.md 参照) */
static void _dirty(int rx, int ry, int rw, int rh)
{
    (void)rx; (void)ry; (void)rw; (void)rh;
}

/* ======================================================================== */
/*  pyxel_cls — 画面クリア                                                   */
/*                                                                          */
/*  警告: gfx_fill_rect で 512x384 を塗りつぶす。                           */
/*  毎フレーム呼出で 9fps 上限 (07_BENCH_RESULTS.md Test 6)。               */
/* ======================================================================== */

void pyxel_cls(int col)
{
    u8 c = PYXEL_COL(col);
    gfx_fill_rect(0, 0, PYXEL_DISP_WIDTH, PYXEL_DISP_HEIGHT, c);
    _dirty(0, 0, PYXEL_DISP_WIDTH, PYXEL_DISP_HEIGHT);
}

/* ======================================================================== */
/*  pyxel_pset / pyxel_pget — ピクセル操作                                   */
/* ======================================================================== */

void pyxel_pset(int x, int y, int col)
{
    int rx, ry;
    u8 c;

    if (x < 0 || x >= _pyxel.width || y < 0 || y >= _pyxel.height) return;

    c = PYXEL_COL(col);
    rx = _px(x);
    ry = _py(y);
    gfx_fill_rect(rx, ry, PYXEL_SCALE, PYXEL_SCALE, c);
    _dirty(rx, ry, PYXEL_SCALE, PYXEL_SCALE);
}

int pyxel_pget(int x, int y)
{
    if (x < 0 || x >= _pyxel.width || y < 0 || y >= _pyxel.height) return 0;
    return (int)gfx_get_pixel(_px(x), _py(y));
}

/* ======================================================================== */
/*  pyxel_line — 線描画                                                      */
/* ======================================================================== */

void pyxel_line(int x1, int y1, int x2, int y2, int col)
{
    int rx1, ry1, rx2, ry2;
    int min_x, min_y, max_x, max_y;
    u8 c = PYXEL_COL(col);

    rx1 = _px(x1); ry1 = _py(y1);
    rx2 = _px(x2); ry2 = _py(y2);

    gfx_line(rx1, ry1, rx2, ry2, c);

    /* dirty rect: バウンディングボックス */
    min_x = PYXEL_MIN(rx1, rx2);
    min_y = PYXEL_MIN(ry1, ry2);
    max_x = PYXEL_MAX(rx1, rx2);
    max_y = PYXEL_MAX(ry1, ry2);
    _dirty(min_x, min_y, max_x - min_x + PYXEL_SCALE,
                          max_y - min_y + PYXEL_SCALE);
}

/* ======================================================================== */
/*  pyxel_rect / pyxel_rectb — 矩形                                         */
/* ======================================================================== */

void pyxel_rect(int x, int y, int w, int h, int col)
{
    int rx, ry, rw, rh;
    u8 c = PYXEL_COL(col);

    rx = _px(x); ry = _py(y);
    rw = w * PYXEL_SCALE;
    rh = h * PYXEL_SCALE;
    gfx_fill_rect(rx, ry, rw, rh, c);
    _dirty(rx, ry, rw, rh);
}

void pyxel_rectb(int x, int y, int w, int h, int col)
{
    int rx, ry, rw, rh;
    u8 c = PYXEL_COL(col);

    rx = _px(x); ry = _py(y);
    rw = w * PYXEL_SCALE;
    rh = h * PYXEL_SCALE;
    gfx_rect(rx, ry, rw, rh, c);
    _dirty(rx, ry, rw, rh);
}

/* ======================================================================== */
/*  pyxel_circ / pyxel_circb — 円                                            */
/* ======================================================================== */

void pyxel_circ(int x, int y, int r, int col)
{
    int rx, ry, rr;
    u8 c = PYXEL_COL(col);

    rx = _px(x); ry = _py(y);
    rr = r * PYXEL_SCALE;
    gfx_fill_circle(rx, ry, rr, c);
    _dirty(rx - rr, ry - rr, rr * 2 + 1, rr * 2 + 1);
}

void pyxel_circb(int x, int y, int r, int col)
{
    int rx, ry, rr;
    u8 c = PYXEL_COL(col);

    rx = _px(x); ry = _py(y);
    rr = r * PYXEL_SCALE;
    gfx_circle(rx, ry, rr, c);
    _dirty(rx - rr, ry - rr, rr * 2 + 1, rr * 2 + 1);
}

/* ======================================================================== */
/*  pyxel_tri / pyxel_trib — 三角形                                          */
/* ======================================================================== */

/* 塗りつぶし三角形: libos32gfx の gfx_fill_tri に委譲 */
void pyxel_tri(int x1, int y1, int x2, int y2,
               int x3, int y3, int col)
{
    u8 c = PYXEL_COL(col);
    gfx_fill_tri(_px(x1), _py(y1), _px(x2), _py(y2), _px(x3), _py(y3), c);
}

/* 枠三角形: 3本の線で描画 */
void pyxel_trib(int x1, int y1, int x2, int y2,
                int x3, int y3, int col)
{
    pyxel_line(x1, y1, x2, y2, col);
    pyxel_line(x2, y2, x3, y3, col);
    pyxel_line(x3, y3, x1, y1, col);
}

/* ======================================================================== */
/*  pyxel_text — テキスト描画                                                */
/* ======================================================================== */

void pyxel_text(int x, int y, const char *s, int col)
{
    int rx, ry, len;
    u8 c = PYXEL_COL(col);

    rx = _px(x); ry = _py(y);
    kcg_draw_utf8(rx, ry, s, c, 0);

    /* dirty rect: 文字列の長さを概算 (ANKフォント8px幅) */
    len = 0;
    while (s[len]) len++;
    _dirty(rx, ry, len * 8, 16);
}

/* ======================================================================== */
/*  pyxel_pal / pyxel_pal_reset — パレットスワップ                           */
/* ======================================================================== */

void pyxel_pal(int col1, int col2)
{
    if (col1 >= 0 && col1 < PYXEL_COLORS &&
        col2 >= 0 && col2 < PYXEL_COLORS) {
        _pyxel.pal_map[col1] = (u8)col2;
    }
}

void pyxel_pal_reset(void)
{
    int i;
    for (i = 0; i < PYXEL_COLORS; i++) {
        _pyxel.pal_map[i] = (u8)i;
    }
}

/* ======================================================================== */
/*  pyxel_camera — カメラ (描画オフセット) 設定                               */
/* ======================================================================== */

void pyxel_camera(int x, int y)
{
    _pyxel.cam_x = x;
    _pyxel.cam_y = y;
}

/* ======================================================================== */
/*  pyxel_clip / pyxel_clip_reset — クリッピング                              */
/* ======================================================================== */

void pyxel_clip(int x, int y, int w, int h)
{
    _pyxel.clip_x = x;
    _pyxel.clip_y = y;
    _pyxel.clip_w = w;
    _pyxel.clip_h = h;
    _pyxel.clip_enabled = 1;
}

void pyxel_clip_reset(void)
{
    _pyxel.clip_x = 0;
    _pyxel.clip_y = 0;
    _pyxel.clip_w = _pyxel.width;
    _pyxel.clip_h = _pyxel.height;
    _pyxel.clip_enabled = 0;
}

/* ======================================================================== */
/*  Phase 3 スタブ (未実装)                                                  */
/* ======================================================================== */

void pyxel_load(const char *filename)
{
    (void)filename;
    /* Phase 3 で実装 */
}

void pyxel_blt(int x, int y, int img, int u, int v,
               int w, int h, int colkey)
{
    (void)x; (void)y; (void)img; (void)u; (void)v;
    (void)w; (void)h; (void)colkey;
    /* Phase 3 で実装 */
}

void pyxel_bltm(int x, int y, int tm, int u, int v,
                int w, int h, int colkey)
{
    (void)x; (void)y; (void)tm; (void)u; (void)v;
    (void)w; (void)h; (void)colkey;
    /* Phase 3 で実装 */
}

void pyxel_fill(int x, int y, int col)
{
    (void)x; (void)y; (void)col;
    /* Phase 3 で実装 */
}

/* ======================================================================== */
/*  Phase 4 スタブ (未実装)                                                  */
/* ======================================================================== */

void pyxel_play(int ch, int snd)
{
    (void)ch; (void)snd;
    /* Phase 4 で実装 */
}

void pyxel_playm(int msc)
{
    (void)msc;
    /* Phase 4 で実装 */
}

void pyxel_stop(int ch)
{
    (void)ch;
    /* Phase 4 で実装 */
}
