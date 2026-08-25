/* ======================================================================== */
/*  VIEW_PANEL.C — 下段の固定コマンドパネル                                  */
/* ======================================================================== */

#include "view_panel.h"
#include "view_tiles.h"   /* TC_* パレット添字 */
#include "libos32gfx.h"
#include <string.h>

static KernelAPI *api;

/* 今フレームの行と、直前に描いた行 */
static char g_lines[PANEL_LINES][PANEL_COLS + 1];
static u8   g_attrs[PANEL_LINES];          /* 0=通常, 1=強調 */
static int  g_count;
static char g_prev[PANEL_LINES][PANEL_COLS + 1];
static u8   g_prev_attrs[PANEL_LINES];
static int  g_prev_count = -1;             /* -1 = まだ一度も描いていない */

void panel_init(KernelAPI *kapi)
{
    api = kapi;
    g_count = 0;
    g_prev_count = -1;
}

void panel_begin(void)
{
    g_count = 0;
}

static void add_line(const char *text, u8 attr)
{
    int n;

    if (g_count >= PANEL_LINES) return;

    /* あふれたら切るが、UTF-8 の途中では切らない。
       後続バイト (10xxxxxx) の上に落ちたら文字の頭まで戻す */
    n = (int)strlen(text);
    if (n > PANEL_COLS) {
        n = PANEL_COLS;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(g_lines[g_count], text, (size_t)n);
    g_lines[g_count][n] = '\0';
    g_attrs[g_count] = attr;
    g_count++;
}

void panel_line(const char *text)
{
    add_line(text, 0);
}

void panel_line_hi(const char *text)
{
    add_line(text, 1);
}

int panel_get_count(void)
{
    return g_count;
}

const char *panel_get_line(int idx)
{
    if (idx < 0 || idx >= g_count) return "";
    return g_lines[idx];
}

u8 panel_get_attr(int idx)
{
    if (idx < 0 || idx >= g_count) return 0;
    return g_attrs[idx];
}

int panel_end(int force)
{
    int i;
    int same;

    /* 前フレームと完全一致なら何もしない */
    same = (!force && g_count == g_prev_count);
    for (i = 0; same && i < g_count; i++) {
        if (g_attrs[i] != g_prev_attrs[i] ||
            strcmp(g_lines[i], g_prev[i]) != 0) {
            same = 0;
        }
    }
    if (same) return 0;

    for (i = 0; i < g_count; i++) {
        strcpy(g_prev[i], g_lines[i]);
        g_prev_attrs[i] = g_attrs[i];
    }
    g_prev_count = g_count;

    /* 下地: 上縁の飾り線 + 本体 */
    gfx_fill_rect(PANEL_X, PANEL_Y, PANEL_W, 2, TC_LAND_HI);
    gfx_fill_rect(PANEL_X, PANEL_Y + 2, PANEL_W, PANEL_H - 2, TC_SEA);

    for (i = 0; i < g_count; i++) {
        kcg_draw_utf8(PANEL_X + 8, PANEL_Y + 6 + i * 14, g_lines[i],
                      g_attrs[i] ? TC_P_YELLOW : TC_PAPER, 0xFF);
    }
    return 1;
}
