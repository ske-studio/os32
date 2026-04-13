/* ======================================================================== */
/*  VBZVIEW.C — VBZベクターファイルビューア (OS32外部プログラム)              */
/*                                                                          */
/*  VBZ形式のベジェ曲線ベクターデータを読み込み、スキャンラインフィルで      */
/*  塗りつぶし描画する。                                                    */
/*                                                                          */
/*  描画方式:                                                               */
/*    1. ベジェ曲線を直線セグメントに平坦化 (de Casteljau)                  */
/*    2. 各パスのエッジテーブルを構築                                       */
/*    3. スキャンラインごとに交差X座標を求め、even-odd ruleで塗りつぶし     */
/*                                                                          */
/*  全て整数演算のみ (FPU不使用)。                                          */
/*                                                                          */
/*  使い方: exec vbzview IMAGE.VBZ [-k] [-o]                                */
/*    -k : 終了時にVRAMをクリアしない                                        */
/*    -o : アウトラインのみ (塗りつぶしなし)                                */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>
#include "os32api.h"
#include "libos32gfx.h"

/* ---- VBZ フォーマット定数 ---- */
#define VBZ_HEADER_SIZE   128
#define VBZ_MAX_PATHS     4096
#define VBZ_MAX_EDGES     16384
#define VBZ_MAX_INTERSECT 512

/* コマンドタイプ */
#define CMD_MOVETO    0x00
#define CMD_LINETO    0x01
#define CMD_BEZIER3   0x02
#define CMD_CLOSEPATH 0x03

/* フラグ */
#define FLAG_FILL     0x01
#define FLAG_STROKE   0x02

/* ベジェ平坦化の最大深度 */
#define FLATTEN_MAX_DEPTH 10

/* ---- データ構造 ---- */

/* エッジ (直線セグメント) */
typedef struct {
    i16 x0, y0, x1, y1;  /* 始点-終点 (y0 <= y1 になるよう正規化) */
} Edge;

/* パスヘッダ */
typedef struct {
    u8  color_idx;
    u8  flags;
    u16 num_cmds;
} PathHeader;

/* VBZヘッダ解析結果 */
typedef struct {
    u16 width;
    u16 height;
    u16 num_paths;
    u8  num_colors;
    u8  flags;
    u8  palette[48];  /* 16色 × RGB 各4bit */
} VbzInfo;

/* エッジテーブル (グローバル、動的確保) */
static Edge *g_edges;
static int g_num_edges;
static int g_max_edges;

/* 交差点バッファ */
static i16 g_intersect[VBZ_MAX_INTERSECT];

static KernelAPI *api;

/* ======================================================================== */
/*  VBZ ヘッダ解析                                                          */
/* ======================================================================== */
static int parse_vbz_header(const u8 *buf, VbzInfo *info)
{
    int i;

    /* マジック確認 */
    if (buf[0] != 'V' || buf[1] != 'B' || buf[2] != 'Z' || buf[3] != '1')
        return -1;

    info->width      = (u16)buf[4] | ((u16)buf[5] << 8);
    info->height     = (u16)buf[6] | ((u16)buf[7] << 8);
    info->num_paths  = (u16)buf[8] | ((u16)buf[9] << 8);
    info->num_colors = buf[0x0A];
    info->flags      = buf[0x0B];

    for (i = 0; i < 48; i++) {
        info->palette[i] = buf[0x0C + i];
    }

    return 0;
}

/* ======================================================================== */
/*  パレット適用                                                            */
/* ======================================================================== */
static void apply_palette(const VbzInfo *info)
{
    int i;
    for (i = 0; i < (int)info->num_colors && i < 16; i++) {
        u8 r = info->palette[i * 3 + 0];
        u8 g = info->palette[i * 3 + 1];
        u8 b = info->palette[i * 3 + 2];
        api->gfx_set_palette(i, r, g, b);
    }
}

/* ======================================================================== */
/*  エッジテーブル操作                                                      */
/* ======================================================================== */
static void edges_clear(void)
{
    g_num_edges = 0;
}

static void edge_add(int x0, int y0, int x1, int y1)
{
    Edge *e;

    /* 水平線は無視 (スキャンラインフィルでは不要) */
    if (y0 == y1) return;

    if (g_num_edges >= g_max_edges) return;

    e = &g_edges[g_num_edges];

    /* y0 < y1 になるよう正規化 */
    if (y0 > y1) {
        e->x0 = (i16)x1; e->y0 = (i16)y1;
        e->x1 = (i16)x0; e->y1 = (i16)y0;
    } else {
        e->x0 = (i16)x0; e->y0 = (i16)y0;
        e->x1 = (i16)x1; e->y1 = (i16)y1;
    }
    g_num_edges++;
}

/* ======================================================================== */
/*  3次ベジェ曲線の平坦化 (エッジテーブルに直線セグメントを追加)            */
/* ======================================================================== */
static void flatten_bezier3(int x0, int y0, int x1, int y1,
                            int x2, int y2, int x3, int y3, int depth)
{
    int mx01, my01, mx12, my12, mx23, my23;
    int mx012, my012, mx123, my123;
    int mx, my;

    /* 平坦性テスト (簡易版: 制御点と直線の距離) */
    if (depth >= FLATTEN_MAX_DEPTH) {
        edge_add(x0, y0, x3, y3);
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
                edge_add(x0, y0, x3, y3);
                return;
            }
        } else {
            cross1 = dx * (long)(y1 - y0) - dy * (long)(x1 - x0);
            cross2 = dx * (long)(y2 - y0) - dy * (long)(x2 - x0);
            if (cross1 * cross1 <= len2 && cross2 * cross2 <= len2) {
                edge_add(x0, y0, x3, y3);
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

    flatten_bezier3(x0, y0, mx01, my01, mx012, my012, mx, my, depth + 1);
    flatten_bezier3(mx, my, mx123, my123, mx23, my23, x3, y3, depth + 1);
}

/* ======================================================================== */
/*  スキャンラインフィル (even-odd rule)                                    */
/* ======================================================================== */

/* 交差点ソート (挿入ソート — 交差点数は通常少ない) */
static void sort_intersections(i16 *arr, int n)
{
    int i, j;
    i16 key;

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

static void scanline_fill(u8 color, int min_y, int max_y)
{
    int y, i;

    if (min_y < 0) min_y = 0;
    if (max_y >= 400) max_y = 399;

    for (y = min_y; y <= max_y; y++) {
        int n_intersect = 0;

        /* 全エッジとの交差を求める */
        for (i = 0; i < g_num_edges; i++) {
            Edge *e = &g_edges[i];
            int dy_e, dx_e;
            int ix;

            /* このスキャンラインとエッジが交差するか */
            if (y < e->y0 || y >= e->y1) continue;

            /* 交差X座標を求める (整数演算) */
            dy_e = e->y1 - e->y0;
            dx_e = e->x1 - e->x0;

            if (dy_e == 0) continue;

            /* ix = x0 + dx * (y - y0) / dy */
            ix = e->x0 + (int)((long)dx_e * (y - e->y0) / dy_e);

            if (n_intersect < VBZ_MAX_INTERSECT) {
                g_intersect[n_intersect++] = (i16)ix;
            }
        }

        if (n_intersect < 2) continue;

        /* ソート */
        sort_intersections(g_intersect, n_intersect);

        /* even-odd rule で塗りつぶし */
        for (i = 0; i + 1 < n_intersect; i += 2) {
            int x_start = g_intersect[i];
            int x_end = g_intersect[i + 1];

            if (x_start < 0) x_start = 0;
            if (x_end >= 640) x_end = 639;
            if (x_start <= x_end) {
                gfx_hline(x_start, y, x_end - x_start + 1, color);
            }
        }
    }
}

/* ======================================================================== */
/*  パスの描画 (アウトライン)                                               */
/* ======================================================================== */
static void draw_path_outline(const u8 *data, int num_cmds, u8 color)
{
    int i;
    int cur_x = 0, cur_y = 0;
    int start_x = 0, start_y = 0;

    for (i = 0; i < num_cmds; i++) {
        u8 cmd = *data++;

        switch (cmd) {
        case CMD_MOVETO: {
            i16 x = (i16)(data[0] | (data[1] << 8));
            i16 y = (i16)(data[2] | (data[3] << 8));
            data += 4;
            cur_x = x; cur_y = y;
            start_x = x; start_y = y;
            break;
        }
        case CMD_LINETO: {
            i16 x = (i16)(data[0] | (data[1] << 8));
            i16 y = (i16)(data[2] | (data[3] << 8));
            data += 4;
            gfx_line(cur_x, cur_y, x, y, color);
            cur_x = x; cur_y = y;
            break;
        }
        case CMD_BEZIER3: {
            i16 cx1 = (i16)(data[0] | (data[1] << 8));
            i16 cy1 = (i16)(data[2] | (data[3] << 8));
            i16 cx2 = (i16)(data[4] | (data[5] << 8));
            i16 cy2 = (i16)(data[6] | (data[7] << 8));
            i16 x   = (i16)(data[8] | (data[9] << 8));
            i16 y   = (i16)(data[10] | (data[11] << 8));
            data += 12;
            gfx_bezier3(cur_x, cur_y, cx1, cy1, cx2, cy2, x, y, color);
            cur_x = x; cur_y = y;
            break;
        }
        case CMD_CLOSEPATH:
            if (cur_x != start_x || cur_y != start_y) {
                gfx_line(cur_x, cur_y, start_x, start_y, color);
            }
            cur_x = start_x; cur_y = start_y;
            break;
        default:
            return; /* 不明コマンド → 中断 */
        }
    }
}

/* ======================================================================== */
/*  パスの描画 (塗りつぶし)                                                 */
/* ======================================================================== */
static void draw_path_filled(const u8 *data, int num_cmds, u8 color)
{
    int i;
    int cur_x = 0, cur_y = 0;
    int start_x = 0, start_y = 0;
    int min_y = 400, max_y = 0;

    /* エッジテーブルをクリア */
    edges_clear();

    /* コマンドをパースしてエッジテーブルを構築 */
    for (i = 0; i < num_cmds; i++) {
        u8 cmd = *data++;

        switch (cmd) {
        case CMD_MOVETO: {
            i16 x = (i16)(data[0] | (data[1] << 8));
            i16 y = (i16)(data[2] | (data[3] << 8));
            data += 4;
            cur_x = x; cur_y = y;
            start_x = x; start_y = y;
            break;
        }
        case CMD_LINETO: {
            i16 x = (i16)(data[0] | (data[1] << 8));
            i16 y = (i16)(data[2] | (data[3] << 8));
            data += 4;
            edge_add(cur_x, cur_y, x, y);
            /* Y範囲更新 */
            if (cur_y < min_y) min_y = cur_y;
            if (cur_y > max_y) max_y = cur_y;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            cur_x = x; cur_y = y;
            break;
        }
        case CMD_BEZIER3: {
            i16 cx1 = (i16)(data[0] | (data[1] << 8));
            i16 cy1 = (i16)(data[2] | (data[3] << 8));
            i16 cx2 = (i16)(data[4] | (data[5] << 8));
            i16 cy2 = (i16)(data[6] | (data[7] << 8));
            i16 x   = (i16)(data[8] | (data[9] << 8));
            i16 y   = (i16)(data[10] | (data[11] << 8));
            data += 12;
            /* ベジェ→直線セグメントに平坦化してエッジ追加 */
            flatten_bezier3(cur_x, cur_y, cx1, cy1, cx2, cy2, x, y, 0);
            /* Y範囲更新 (制御点含む) */
            if (cur_y < min_y) min_y = cur_y;
            if (cur_y > max_y) max_y = cur_y;
            if (cy1 < min_y) min_y = cy1;
            if (cy1 > max_y) max_y = cy1;
            if (cy2 < min_y) min_y = cy2;
            if (cy2 > max_y) max_y = cy2;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            cur_x = x; cur_y = y;
            break;
        }
        case CMD_CLOSEPATH:
            if (cur_x != start_x || cur_y != start_y) {
                edge_add(cur_x, cur_y, start_x, start_y);
                if (start_y < min_y) min_y = start_y;
                if (start_y > max_y) max_y = start_y;
            }
            cur_x = start_x; cur_y = start_y;
            break;
        default:
            return;
        }
    }

    /* スキャンラインフィル */
    if (g_num_edges > 0) {
        scanline_fill(color, min_y, max_y);
    }
}

/* ======================================================================== */
/*  VBZ 描画メイン                                                          */
/* ======================================================================== */
static int display_vbz(int fd, const VbzInfo *info, int outline_only)
{
    u8 *file_data;
    int file_size;
    int data_size;
    int offset;
    int path_idx;

    /* ファイル全体を読み込む */
    /* まずファイルサイズを取得 (seekで末尾に移動) */
    file_size = api->sys_lseek(fd, 0, 2);  /* SEEK_END */
    if (file_size <= VBZ_HEADER_SIZE) return -1;

    api->sys_lseek(fd, 0, 0);  /* SEEK_SET */

    file_data = (u8 *)api->mem_alloc(file_size);
    if (!file_data) {
        api->kprintf(ATTR_WHITE, "%s", "Error: mem_alloc failed\n");
        return -1;
    }

    if (api->sys_read(fd, file_data, file_size) != file_size) {
        api->mem_free(file_data);
        return -1;
    }

    /* エッジテーブル確保 */
    g_max_edges = VBZ_MAX_EDGES;
    g_edges = (Edge *)api->mem_alloc(g_max_edges * sizeof(Edge));
    if (!g_edges) {
        api->mem_free(file_data);
        api->kprintf(ATTR_WHITE, "%s", "Error: edge table alloc failed\n");
        return -1;
    }

    /* パスデータの開始位置 */
    data_size = file_size - VBZ_HEADER_SIZE;
    offset = VBZ_HEADER_SIZE;

    /* 全パスを描画 */
    for (path_idx = 0; path_idx < info->num_paths; path_idx++) {
        PathHeader ph;
        const u8 *cmd_data;
        int cmd_data_len;
        int j;

        if (offset + 4 > file_size) break;

        /* パスヘッダ読み込み */
        ph.color_idx = file_data[offset];
        ph.flags     = file_data[offset + 1];
        ph.num_cmds  = (u16)file_data[offset + 2] | ((u16)file_data[offset + 3] << 8);
        offset += 4;

        /* コマンドデータの位置を記録 */
        cmd_data = file_data + offset;

        /* コマンドデータをスキップしてサイズを計算 */
        {
            const u8 *p = cmd_data;
            for (j = 0; j < (int)ph.num_cmds; j++) {
                u8 cmd;
                if (p >= file_data + file_size) break;
                cmd = *p++;
                switch (cmd) {
                case CMD_MOVETO:    p += 4; break;
                case CMD_LINETO:    p += 4; break;
                case CMD_BEZIER3:   p += 12; break;
                case CMD_CLOSEPATH: break;
                default: goto done_skip;
                }
            }
done_skip:
            cmd_data_len = (int)(p - cmd_data);
            offset += cmd_data_len;
        }

        /* 描画 */
        if (ph.color_idx < 16) {
            if (outline_only) {
                /* アウトラインモード: 全パスを線描画 */
                draw_path_outline(cmd_data, ph.num_cmds, ph.color_idx);
            } else if (ph.flags & FLAG_FILL) {
                /* 塗りつぶしパス */
                draw_path_filled(cmd_data, ph.num_cmds, ph.color_idx);
            } else if (ph.flags & FLAG_STROKE) {
                /* ストロークパス (1px線描画) */
                draw_path_outline(cmd_data, ph.num_cmds, ph.color_idx);
            }
        }

        /* 16パスごとに画面更新 (進捗表示) */
        if ((path_idx & 15) == 15) {
            api->gfx_present_dirty();
        }
    }

    api->mem_free(g_edges);
    api->mem_free(file_data);

    (void)data_size;
    return 0;
}

/* ======================================================================== */
/*  メインプログラム                                                        */
/* ======================================================================== */
void main(int argc, char **argv, KernelAPI *kapi)
{
    const char *filename = NULL;
    int keep_vram = 0;
    int outline_only = 0;
    int i, fd, ret;
    u8 hdr_buf[VBZ_HEADER_SIZE];
    VbzInfo info;
    int ch;

    api = kapi;

    /* 引数解析 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            keep_vram = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            outline_only = 1;
        } else {
            filename = argv[i];
        }
    }

    if (filename == NULL) {
        api->kprintf(ATTR_WHITE, "%s", "VBZView v1.0 - Bezier Vector Viewer for OS32\n");
        api->kprintf(ATTR_WHITE, "%s", "Usage: vbzview FILE.VBZ [-k] [-o]\n");
        api->kprintf(ATTR_WHITE, "%s", "  -k : keep VRAM on exit\n");
        api->kprintf(ATTR_WHITE, "%s", "  -o : outline only (no fill)\n");
        return;
    }

    /* ファイルオープン */
    fd = api->sys_open(filename, O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_WHITE, "Error: cannot open %s\n", filename);
        return;
    }

    /* ヘッダ読み込み */
    if (api->sys_read(fd, hdr_buf, VBZ_HEADER_SIZE) != VBZ_HEADER_SIZE) {
        api->kprintf(ATTR_WHITE, "%s", "Error: read header failed\n");
        api->sys_close(fd);
        return;
    }

    /* ヘッダ解析 */
    if (parse_vbz_header(hdr_buf, &info) < 0) {
        api->kprintf(ATTR_WHITE, "%s", "Error: not a VBZ file\n");
        api->sys_close(fd);
        return;
    }

    api->kprintf(ATTR_WHITE, "VBZ: %dx%d, %d colors, %d paths\n",
                 info.width, info.height, info.num_colors, info.num_paths);

    /* GFX初期化 */
    libos32gfx_init(api);

    /* 背景クリア (VBZの背景色を使用) */
    gfx_clear(info.flags);  /* flags にBGカラーインデックスを格納 */

    /* パレット設定 */
    apply_palette(&info);

    /* 描画 (ファイル先頭に戻す) */
    api->sys_lseek(fd, 0, 0);
    ret = display_vbz(fd, &info, outline_only);
    api->sys_close(fd);

    if (ret < 0) {
        libos32gfx_shutdown();
        api->tvram_clear();
        api->kprintf(ATTR_WHITE, "%s", "Error: render failed\n");
        return;
    }

    /* VRAM転送 */
    gfx_present();
    api->gfx_present_dirty();

    /* キーバッファをフラッシュ (HTTP API経由の残留文字を排出) */
    {
        int flush_count;
        for (flush_count = 0; flush_count < 32; flush_count++) {
            if (api->kbd_trygetchar() < 0) break;
        }
    }

    /* ESCキーで終了 (それ以外のキーは無視) */
    for (;;) {
        ch = api->kbd_getchar();
        if (ch == 0x1B) break;  /* ESC */
    }

    /* 後片付け */
    if (!keep_vram) {
        gfx_clear(0);
        gfx_present();
        api->gfx_present_dirty();
    }

    libos32gfx_shutdown();
    api->tvram_clear();
}
