/* ======================================================================== */
/*  gfx_fill.c — スキャンラインフィルエンジン (libos32gfx)                  */
/*                                                                          */
/*  エッジテーブル構築 + even-odd rule によるスキャンラインフィル。           */
/*  ベジェ曲線の平坦化 (de Casteljau) もこのモジュールに含む。              */
/*                                                                          */
/*  全て整数演算のみ (FPU不使用)。                                          */
/* ======================================================================== */

#include "libos32gfx.h"
#include "libgfx_internal.h"
#include "os32api.h"

/* ベジェ平坦化の最大深度 */
#define FLATTEN_MAX_DEPTH 10

/* ======================================================================== */
/*  エッジテーブル操作                                                      */
/* ======================================================================== */

GFX_EdgeTable *gfx_edge_table_create(int max_edges, int max_intersect)
{
    GFX_EdgeTable *et;

    et = (GFX_EdgeTable *)gfx_api->mem_alloc(sizeof(GFX_EdgeTable));
    if (!et) return 0;

    et->edges = (GFX_Edge *)gfx_api->mem_alloc(max_edges * sizeof(GFX_Edge));
    if (!et->edges) {
        gfx_api->mem_free(et);
        return 0;
    }

    et->intersect_buf = (int *)gfx_api->mem_alloc(max_intersect * sizeof(int));
    if (!et->intersect_buf) {
        gfx_api->mem_free(et->edges);
        gfx_api->mem_free(et);
        return 0;
    }

    et->num_edges = 0;
    et->max_edges = max_edges;
    et->max_intersect = max_intersect;
    return et;
}

void gfx_edge_table_free(GFX_EdgeTable *et)
{
    if (!et) return;
    if (et->intersect_buf) gfx_api->mem_free(et->intersect_buf);
    if (et->edges) gfx_api->mem_free(et->edges);
    gfx_api->mem_free(et);
}

void gfx_edges_clear(GFX_EdgeTable *et)
{
    if (et) et->num_edges = 0;
}

void gfx_edge_add(GFX_EdgeTable *et, int x0, int y0, int x1, int y1)
{
    GFX_Edge *e;

    /* 水平線は無視 (スキャンラインフィルでは不要) */
    if (y0 == y1) return;
    if (!et || et->num_edges >= et->max_edges) return;

    e = &et->edges[et->num_edges];

    /* y0 < y1 になるよう正規化 */
    if (y0 > y1) {
        e->x0 = x1; e->y0 = y1;
        e->x1 = x0; e->y1 = y0;
    } else {
        e->x0 = x0; e->y0 = y0;
        e->x1 = x1; e->y1 = y1;
    }
    et->num_edges++;
}

/* ======================================================================== */
/*  3次ベジェ曲線の平坦化 (エッジテーブルに直線セグメントを追加)            */
/* ======================================================================== */

void gfx_bezier3_to_edges(GFX_EdgeTable *et,
                           int x0, int y0, int x1, int y1,
                           int x2, int y2, int x3, int y3, int depth)
{
    int mx01, my01, mx12, my12, mx23, my23;
    int mx012, my012, mx123, my123;
    int mx, my;

    /* 平坦性テスト */
    if (depth >= FLATTEN_MAX_DEPTH) {
        gfx_edge_add(et, x0, y0, x3, y3);
        return;
    }

    {
        long dx = (long)(x3 - x0);
        long dy = (long)(y3 - y0);
        long len2 = dx * dx + dy * dy;
        long cross1, cross2;

        if (len2 == 0) {
            long d1 = (long)(x1 - x0) * (x1 - x0) + (long)(y1 - y0) * (y1 - y0);
            long d2 = (long)(x2 - x0) * (x2 - x0) + (long)(y2 - y0) * (y2 - y0);
            if (d1 <= 4 && d2 <= 4) {
                gfx_edge_add(et, x0, y0, x3, y3);
                return;
            }
        } else {
            cross1 = dx * (long)(y1 - y0) - dy * (long)(x1 - x0);
            cross2 = dx * (long)(y2 - y0) - dy * (long)(x2 - x0);
            if (cross1 * cross1 <= len2 && cross2 * cross2 <= len2) {
                gfx_edge_add(et, x0, y0, x3, y3);
                return;
            }
        }
    }

    /* de Casteljau 中点分割 */
    mx01 = (x0 + x1) / 2;   my01 = (y0 + y1) / 2;
    mx12 = (x1 + x2) / 2;   my12 = (y1 + y2) / 2;
    mx23 = (x2 + x3) / 2;   my23 = (y2 + y3) / 2;

    mx012 = (mx01 + mx12) / 2;  my012 = (my01 + my12) / 2;
    mx123 = (mx12 + mx23) / 2;  my123 = (my12 + my23) / 2;

    mx = (mx012 + mx123) / 2;   my = (my012 + my123) / 2;

    gfx_bezier3_to_edges(et, x0, y0, mx01, my01, mx012, my012, mx, my, depth + 1);
    gfx_bezier3_to_edges(et, mx, my, mx123, my123, mx23, my23, x3, y3, depth + 1);
}

/* ======================================================================== */
/*  スキャンラインフィル (even-odd rule)                                    */
/* ======================================================================== */

/* 交差点ソート (挿入ソート — 交差点数は通常少ない) */
static void sort_intersections(int *arr, int n)
{
    int i, j, key;

    for (i = 1; i < n; i++) {
        key = arr[i];
        j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

void gfx_scanline_fill(GFX_EdgeTable *et, u8 color, int min_y, int max_y)
{
    int y, i;

    if (!et || et->num_edges == 0) return;
    if (min_y < 0) min_y = 0;
    if (max_y >= 400) max_y = 399;

    for (y = min_y; y <= max_y; y++) {
        int n_intersect = 0;

        /* 全エッジとの交差を求める */
        for (i = 0; i < et->num_edges; i++) {
            GFX_Edge *e = &et->edges[i];
            int dy_e, dx_e, ix;

            /* このスキャンラインとエッジが交差するか */
            if (y < e->y0 || y >= e->y1) continue;

            /* 交差X座標を求める (整数演算) */
            dy_e = e->y1 - e->y0;
            dx_e = e->x1 - e->x0;
            if (dy_e == 0) continue;

            /* ix = x0 + dx * (y - y0) / dy */
            ix = e->x0 + (int)((long)dx_e * (y - e->y0) / dy_e);

            if (n_intersect < et->max_intersect) {
                et->intersect_buf[n_intersect++] = ix;
            }
        }

        if (n_intersect < 2) continue;

        /* ソート */
        sort_intersections(et->intersect_buf, n_intersect);

        /* even-odd rule で塗りつぶし */
        for (i = 0; i + 1 < n_intersect; i += 2) {
            int x_start = et->intersect_buf[i];
            int x_end = et->intersect_buf[i + 1];

            if (x_start < 0) x_start = 0;
            if (x_end >= 640) x_end = 639;
            if (x_start <= x_end) {
                gfx_hline(x_start, y, x_end - x_start + 1, color);
            }
        }
    }
}
