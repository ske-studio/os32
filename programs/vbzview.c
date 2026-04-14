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

/* ---- データ構造 ---- */

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
    u8  palette[48];
} VbzInfo;

/* エッジテーブル (動的確保, libos32gfx API) */
static GFX_EdgeTable *g_et;

static KernelAPI *api;

/* 工程別プロファイリング累計 */
static u32 g_prof_edge;
static u32 g_prof_fill;
static u32 g_prof_present;
static u32 g_prof_total_edges;

/* ---- ビューポート (ズーム/パン) ---- */
static int g_view_x, g_view_y;  /* ビューポート左上 (VBZ座標) */
static int g_view_w, g_view_h;  /* ビューポートの幅高 (VBZ座標系) */
static int g_img_w, g_img_h;    /* 元画像サイズ */

/* VBZ座標 → スクリーン座標変換 */
#define VX(vx) ((int)((long)((vx) - g_view_x) * 640 / g_view_w))
#define VY(vy) ((int)((long)((vy) - g_view_y) * 400 / g_view_h))

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
            i16 rx = (i16)(data[0] | (data[1] << 8));
            i16 ry = (i16)(data[2] | (data[3] << 8));
            data += 4;
            cur_x = VX(rx); cur_y = VY(ry);
            start_x = cur_x; start_y = cur_y;
            break;
        }
        case CMD_LINETO: {
            i16 rx = (i16)(data[0] | (data[1] << 8));
            i16 ry = (i16)(data[2] | (data[3] << 8));
            int sx = VX(rx), sy = VY(ry);
            data += 4;
            gfx_line(cur_x, cur_y, sx, sy, color);
            cur_x = sx; cur_y = sy;
            break;
        }
        case CMD_BEZIER3: {
            int scx1 = VX((i16)(data[0] | (data[1] << 8)));
            int scy1 = VY((i16)(data[2] | (data[3] << 8)));
            int scx2 = VX((i16)(data[4] | (data[5] << 8)));
            int scy2 = VY((i16)(data[6] | (data[7] << 8)));
            int sx   = VX((i16)(data[8] | (data[9] << 8)));
            int sy   = VY((i16)(data[10] | (data[11] << 8)));
            data += 12;
            gfx_bezier3(cur_x, cur_y, scx1, scy1, scx2, scy2, sx, sy, color);
            cur_x = sx; cur_y = sy;
            break;
        }
        case CMD_CLOSEPATH:
            if (cur_x != start_x || cur_y != start_y) {
                gfx_line(cur_x, cur_y, start_x, start_y, color);
            }
            cur_x = start_x; cur_y = start_y;
            break;
        default:
            return;
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
    u32 tp;

    /* エッジテーブルをクリア */
    gfx_edges_clear(g_et);

    tp = api->get_tick();

    /* コマンドをパースしてエッジテーブルを構築 (ビューポート変換適用) */
    for (i = 0; i < num_cmds; i++) {
        u8 cmd = *data++;

        switch (cmd) {
        case CMD_MOVETO: {
            i16 rx = (i16)(data[0] | (data[1] << 8));
            i16 ry = (i16)(data[2] | (data[3] << 8));
            data += 4;
            cur_x = VX(rx); cur_y = VY(ry);
            start_x = cur_x; start_y = cur_y;
            break;
        }
        case CMD_LINETO: {
            i16 rx = (i16)(data[0] | (data[1] << 8));
            i16 ry = (i16)(data[2] | (data[3] << 8));
            int sx = VX(rx), sy = VY(ry);
            data += 4;
            gfx_edge_add(g_et, cur_x, cur_y, sx, sy);
            if (cur_y < min_y) min_y = cur_y;
            if (cur_y > max_y) max_y = cur_y;
            if (sy < min_y) min_y = sy;
            if (sy > max_y) max_y = sy;
            cur_x = sx; cur_y = sy;
            break;
        }
        case CMD_BEZIER3: {
            int scx1 = VX((i16)(data[0] | (data[1] << 8)));
            int scy1 = VY((i16)(data[2] | (data[3] << 8)));
            int scx2 = VX((i16)(data[4] | (data[5] << 8)));
            int scy2 = VY((i16)(data[6] | (data[7] << 8)));
            int sx   = VX((i16)(data[8] | (data[9] << 8)));
            int sy   = VY((i16)(data[10] | (data[11] << 8)));
            data += 12;
            gfx_bezier3_to_edges(g_et, cur_x, cur_y, scx1, scy1, scx2, scy2, sx, sy, 0);
            if (cur_y < min_y) min_y = cur_y;
            if (cur_y > max_y) max_y = cur_y;
            if (scy1 < min_y) min_y = scy1;
            if (scy1 > max_y) max_y = scy1;
            if (scy2 < min_y) min_y = scy2;
            if (scy2 > max_y) max_y = scy2;
            if (sy < min_y) min_y = sy;
            if (sy > max_y) max_y = sy;
            cur_x = sx; cur_y = sy;
            break;
        }
        case CMD_CLOSEPATH:
            if (cur_x != start_x || cur_y != start_y) {
                gfx_edge_add(g_et, cur_x, cur_y, start_x, start_y);
                if (start_y < min_y) min_y = start_y;
                if (start_y > max_y) max_y = start_y;
            }
            cur_x = start_x; cur_y = start_y;
            break;
        default:
            return;
        }
    }

    g_prof_edge += api->get_tick() - tp;
    g_prof_total_edges += g_et->num_edges;

    /* スキャンラインフィル */
    if (g_et->num_edges > 0) {
        tp = api->get_tick();
        gfx_scanline_fill(g_et, color, min_y, max_y);
        g_prof_fill += api->get_tick() - tp;
    }
}

/* ======================================================================== */
/*  VBZ 描画 (ファイルデータからレンダリング)                                */
/* ======================================================================== */
static void render_paths(const u8 *file_data, int file_size,
                         const VbzInfo *info, int outline_only, u8 bg_color)
{
    int offset;
    int path_idx;
    u32 t0, t1;

    t0 = api->get_tick();
    g_prof_edge = 0;
    g_prof_fill = 0;
    g_prof_present = 0;
    g_prof_total_edges = 0;

    /* 背景クリア */
    gfx_clear(bg_color);

    offset = VBZ_HEADER_SIZE;

    for (path_idx = 0; path_idx < info->num_paths; path_idx++) {
        PathHeader ph;
        const u8 *cmd_data;
        int cmd_data_len;
        int j;

        if (offset + 4 > file_size) break;

        ph.color_idx = file_data[offset];
        ph.flags     = file_data[offset + 1];
        ph.num_cmds  = (u16)file_data[offset + 2] | ((u16)file_data[offset + 3] << 8);
        offset += 4;

        cmd_data = file_data + offset;

        /* コマンドデータをスキップ */
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
                draw_path_outline(cmd_data, ph.num_cmds, ph.color_idx);
            } else if (ph.flags & FLAG_FILL) {
                draw_path_filled(cmd_data, ph.num_cmds, ph.color_idx);
            } else if (ph.flags & FLAG_STROKE) {
                draw_path_outline(cmd_data, ph.num_cmds, ph.color_idx);
            }
        }

        /* 64パスごとに画面更新 (進捗表示) */
        if ((path_idx & 63) == 63) {
            u32 tp_pr = api->get_tick();
            gfx_present();
            api->gfx_present_dirty();
            g_prof_present += api->get_tick() - tp_pr;
        }
    }

    {
        u32 tp_pr = api->get_tick();
        gfx_present();
        api->gfx_present_dirty();
        g_prof_present += api->get_tick() - tp_pr;
    }

    t1 = api->get_tick();
    api->kprintf(ATTR_WHITE, "Render: %d paths, %d ticks (%d ms)\n",
                 info->num_paths, t1 - t0, (t1 - t0) * 10);
    api->kprintf(ATTR_WHITE, "  Edge: %d, Fill: %d, Present: %d, Other: %d\n",
                 g_prof_edge, g_prof_fill, g_prof_present,
                 (t1 - t0) - g_prof_edge - g_prof_fill - g_prof_present);
    api->kprintf(ATTR_WHITE, "  Total edges: %d (avg %d/path)\n",
                 g_prof_total_edges,
                 info->num_paths ? (int)(g_prof_total_edges / info->num_paths) : 0);
}

/* ======================================================================== */
/*  メインプログラム                                                        */
/* ======================================================================== */
void main(int argc, char **argv, KernelAPI *kapi)
{
    const char *filename = NULL;
    int keep_vram = 0;
    int outline_only = 0;
    int i, fd;
    VbzInfo info;
    int ch;
    u8 *file_data;
    int file_size;
    int need_render;
    int zoom_level;  /* 0=1x, 1=2x, 2=4x, 3=8x */
    int pan_step;

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
        api->kprintf(ATTR_WHITE, "%s", "VBZView v2.0 - Bezier Vector Viewer for OS32\n");
        api->kprintf(ATTR_WHITE, "%s", "Usage: vbzview FILE.VBZ [-k] [-o]\n");
        api->kprintf(ATTR_WHITE, "%s", "  -k : keep VRAM on exit\n");
        api->kprintf(ATTR_WHITE, "%s", "  -o : outline only (no fill)\n");
        api->kprintf(ATTR_WHITE, "%s", "Keys: Z=zoom in, X=zoom out, Arrows=pan, 1=reset, ESC=quit\n");
        return;
    }

    /* ファイルオープン & 全体読み込み */
    fd = api->sys_open(filename, O_RDONLY);
    if (fd < 0) {
        api->kprintf(ATTR_WHITE, "Error: cannot open %s\n", filename);
        return;
    }

    file_size = api->sys_lseek(fd, 0, 2);
    if (file_size <= VBZ_HEADER_SIZE) {
        api->kprintf(ATTR_WHITE, "%s", "Error: file too small\n");
        api->sys_close(fd);
        return;
    }
    api->sys_lseek(fd, 0, 0);

    file_data = (u8 *)api->mem_alloc(file_size);
    if (!file_data) {
        api->kprintf(ATTR_WHITE, "%s", "Error: mem_alloc failed\n");
        api->sys_close(fd);
        return;
    }
    if (api->sys_read(fd, file_data, file_size) != file_size) {
        api->mem_free(file_data);
        api->sys_close(fd);
        return;
    }
    api->sys_close(fd);

    /* ヘッダ解析 */
    if (parse_vbz_header(file_data, &info) < 0) {
        api->kprintf(ATTR_WHITE, "%s", "Error: not a VBZ file\n");
        api->mem_free(file_data);
        return;
    }

    api->kprintf(ATTR_WHITE, "VBZ: %dx%d, %d colors, %d paths\n",
                 info.width, info.height, info.num_colors, info.num_paths);

    /* GFX初期化 (gfx_apiを設定するため、先に呼ぶ) */
    libos32gfx_init(api);
    apply_palette(&info);

    /* エッジテーブル確保 (gfx_api->mem_allocを使用するため、init後) */
    g_et = gfx_edge_table_create(VBZ_MAX_EDGES, VBZ_MAX_INTERSECT);
    if (!g_et) {
        api->mem_free(file_data);
        api->kprintf(ATTR_WHITE, "%s", "Error: edge table alloc failed\n");
        return;
    }

    /* ビューポート初期化 (1x, 全体表示) */
    g_img_w = info.width;
    g_img_h = info.height;
    g_view_x = 0;
    g_view_y = 0;
    g_view_w = g_img_w;
    g_view_h = g_img_h;
    zoom_level = 0;

    /* 初回描画 */
    render_paths(file_data, file_size, &info, outline_only, info.flags);

    /* キーバッファをフラッシュ */
    {
        int flush_count;
        for (flush_count = 0; flush_count < 32; flush_count++) {
            if (api->kbd_trygetchar() < 0) break;
        }
    }

    /* ======== インタラクティブ ズーム/パン ループ ======== */
    for (;;) {
        ch = api->kbd_getchar();
        need_render = 0;
        pan_step = g_view_w / 4;
        if (pan_step < 8) pan_step = 8;

        if (ch == 0x1B) {
            break;  /* ESC: 終了 */

        } else if (ch == 'z' || ch == 'Z' || ch == '+') {
            /* ズームイン */
            if (zoom_level < 4) {
                int cx = g_view_x + g_view_w / 2;
                int cy = g_view_y + g_view_h / 2;
                zoom_level++;
                g_view_w = g_img_w >> zoom_level;
                g_view_h = g_img_h >> zoom_level;
                if (g_view_w < 16) g_view_w = 16;
                if (g_view_h < 16) g_view_h = 16;
                g_view_x = cx - g_view_w / 2;
                g_view_y = cy - g_view_h / 2;
                need_render = 1;
            }

        } else if (ch == 'x' || ch == 'X' || ch == '-') {
            /* ズームアウト */
            if (zoom_level > 0) {
                int cx = g_view_x + g_view_w / 2;
                int cy = g_view_y + g_view_h / 2;
                zoom_level--;
                g_view_w = g_img_w >> zoom_level;
                g_view_h = g_img_h >> zoom_level;
                g_view_x = cx - g_view_w / 2;
                g_view_y = cy - g_view_h / 2;
                need_render = 1;
            }

        } else if (ch == '1') {
            /* リセット (1x) */
            zoom_level = 0;
            g_view_x = 0;
            g_view_y = 0;
            g_view_w = g_img_w;
            g_view_h = g_img_h;
            need_render = 1;

        } else if (ch == 0x1C) {
            /* 右矢印 */
            g_view_x += pan_step;
            need_render = 1;
        } else if (ch == 0x1D) {
            /* 左矢印 */
            g_view_x -= pan_step;
            need_render = 1;
        } else if (ch == 0x1E) {
            /* 上矢印 */
            g_view_y -= pan_step;
            need_render = 1;
        } else if (ch == 0x1F) {
            /* 下矢印 */
            g_view_y += pan_step;
            need_render = 1;
        }

        if (need_render) {
            render_paths(file_data, file_size, &info, outline_only, info.flags);
        }
    }

    /* 後片付け */
    gfx_edge_table_free(g_et);
    api->mem_free(file_data);

    if (!keep_vram) {
        gfx_clear(0);
        gfx_present();
        api->gfx_present_dirty();
    }

    libos32gfx_shutdown();
    api->tvram_clear();
}
