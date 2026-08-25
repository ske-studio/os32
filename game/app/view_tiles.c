/* ======================================================================== */
/*  VIEW_TILES.C — 盤面の地形タイルとパレット                                */
/*                                                                          */
/*  古地図 (羊皮紙に刷った日本地図) の色合いで 16x16 のタイルを組み立て、    */
/*  libos32tilemap へ登録する。タイルは外部素材ではなくコードで生成する      */
/*  ので、assets に画像を持たなくてよい。                                    */
/*                                                                          */
/*  タイルの色 0 は libos32tilemap では「透過」なので、地形タイルの中では    */
/*  使わないこと (背景が抜ける)。輪郭が要るものは gfx で上から描く。         */
/* ======================================================================== */

#include "view_tiles.h"
#include "libos32tilemap.h"
#include <string.h>

/* ====================================================================== */
/*  パレット — 古地図                                                      */
/*                                                                        */
/*  カーネル既定のパレットは各成分が 0/7/15 の原色しか使わないため派手に    */
/*  なる。起動時にこれを流し込んで退色させる。                              */
/* ====================================================================== */

static const u8 g_palette[16][3] = {
    {  2,  2,  1 },   /*  0 濃セピア。輪郭・文字 (タイル内では使えない) */
    {  4,  6,  6 },   /*  1 海 (深) スレートブルー */
    {  6,  8,  8 },   /*  2 海 (浅) */
    {  9,  8,  5 },   /*  3 陸 (陰) セピア。起伏のハッチング */
    { 13, 12,  9 },   /*  4 陸 (地) 羊皮紙 — 面積が一番広い */
    { 14, 14, 11 },   /*  5 陸 (明) 生成り */
    { 11,  9,  6 },   /*  6 道 (褪せた朱土) */
    {  8,  8,  7 },   /*  7 岩・石垣 */
    {  7,  8,  4 },   /*  8 森 (くすんだオリーブ) */
    {  4,  6, 11 },   /*  9 プレイヤー青 (藍) */
    { 12,  5,  3 },   /* 10 プレイヤー赤 (朱) */
    {  9,  6, 10 },   /* 11 魔法 (退色した藤) */
    {  5,  9,  4 },   /* 12 プレイヤー緑 (苔) */
    {  7, 10, 11 },   /* 13 神社 (褪せた浅葱) */
    { 14, 11,  4 },   /* 14 プレイヤー黄 (山吹) */
    { 15, 15, 13 }    /* 15 紙の白 */
};

void view_tiles_set_palette(KernelAPI *api)
{
    int i;
    for (i = 0; i < 16; i++) {
        api->gfx_set_palette(i, g_palette[i][0], g_palette[i][1],
                             g_palette[i][2]);
    }
}

/* ====================================================================== */
/*  タイル生成                                                             */
/*                                                                        */
/*  4bpp パックド (128バイト/タイル)。1バイトに2ピクセル、                  */
/*  上位ニブルが偶数列・下位ニブルが奇数列。                                */
/* ====================================================================== */

static u8 g_buf[TILE_TOTAL_SZ];

static void px(int x, int y, u8 c)
{
    int idx;
    if (x < 0 || x >= 16 || y < 0 || y >= 16) return;
    idx = (y * 16 + x) / 2;
    if ((x & 1) == 0) {
        g_buf[idx] = (u8)((g_buf[idx] & 0x0F) | (c << 4));
    } else {
        g_buf[idx] = (u8)((g_buf[idx] & 0xF0) | (c & 0x0F));
    }
}

static void clear_buf(u8 c)
{
    memset(g_buf, (int)((c << 4) | c), sizeof(g_buf));
}

static void hline(int x0, int x1, int y, u8 c)
{
    int x;
    for (x = x0; x <= x1; x++) px(x, y, c);
}

/* --- 陸 (羊皮紙)。紙のムラを散らす。variant ごとに位置を変える --- */
static void make_land(int variant)
{
    static const signed char spots[4][6] = {
        {  3,  2,  9,  7, 13, 11 },
        { 11,  3,  5,  9,  2, 13 },
        {  7,  1, 14,  6,  4, 12 },
        {  2,  8, 12,  2,  8, 14 }
    };
    int i;
    clear_buf(TC_LAND);
    for (i = 0; i < 3; i++) {
        int sx = spots[variant & 3][i * 2];
        int sy = spots[variant & 3][i * 2 + 1];
        px(sx, sy, TC_LAND_HI);
        px(sx + 1, sy, TC_LAND_HI);
        px(sx, sy + 1, TC_LAND_HI);
    }
}

/* --- 陸 (起伏のハッチング)。古地図の山の描法にならった短い斜線 --- */
static void make_land_hatch(void)
{
    int k, i;
    clear_buf(TC_LAND);
    for (k = 0; k < 3; k++) {
        int hy = 3 + k * 5;
        int hx = 2 + k * 4;
        for (i = 0; i < 5; i++) {
            px(hx + i, hy + (i & 1), TC_LAND_SH);
        }
    }
}

/* --- 海。刷りムラ程度の濃淡だけ --- */
static void make_sea(int variant)
{
    clear_buf(TC_SEA);
    if (variant & 1) {
        hline(2, 7, 4, TC_SEA_HI);
        hline(9, 13, 11, TC_SEA_HI);
    } else {
        hline(4, 10, 8, TC_SEA_HI);
    }
}

/* --- 海岸。dir: 0=陸が上, 1=陸が右, 2=陸が下, 3=陸が左 --- */
static void make_shore(int dir)
{
    int i, j;
    clear_buf(TC_SEA);
    for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
            int d;
            switch (dir) {
            case 0:  d = j; break;
            case 1:  d = 15 - i; break;
            case 2:  d = 15 - j; break;
            default: d = i; break;
            }
            /* 境界を少し波打たせる (直線的な段差を避ける) */
            if (d < 5 + ((i * 3 + j * 5) % 3)) {
                px(i, j, TC_LAND);
            } else if (d < 7) {
                px(i, j, TC_SEA_HI);
            }
        }
    }
}

/* --- 森。塗りつぶさず、記号的な木立を2つ --- */
static void make_forest(void)
{
    int i, j;
    make_land(1);
    for (j = 0; j < 5; j++) {
        for (i = 0; i < 7; i++) {
            int dx = i - 3, dy = j - 2;
            if (dx * dx + dy * dy * 2 <= 9) {
                px(2 + i, 2 + j, TC_FOREST);
                px(8 + i, 8 + j, TC_FOREST);
            }
        }
    }
    px(5, 7, TC_LAND_SH);
    px(11, 13, TC_LAND_SH);
}

/* --- 山。矢印 (三角形) と紛れないよう、幅広の稜線とハッチングで描く --- */
static void make_mountain(void)
{
    static const signed char ridge[16] = {
        14, 13, 11, 9, 8, 7, 6, 7, 6, 5, 6, 8, 10, 12, 13, 14
    };
    int i, j;
    make_land(2);
    for (i = 0; i < 16; i++) {
        int top = ridge[i];
        /* 稜線 */
        px(i, top, TC_ROCK);
        if (i > 0 && ridge[i - 1] < top - 1) {
            for (j = ridge[i - 1]; j < top; j++) px(i, j, TC_ROCK);
        }
        /* 山肌は塗らず、斜めのハッチングだけ (古地図の描法) */
        for (j = top + 1; j < 15; j++) {
            if (((i + j) & 3) == 0) px(i, j, TC_LAND_SH);
        }
    }
    hline(0, 15, 15, TC_LAND_SH);
}

/* --- 進行方向の矢印。地面に刷った印。dir: 0=上 1=右 2=下 3=左 --- */
static void make_arrow(int dir)
{
    int i, w, x0, y;
    make_land(0);
    for (i = 0; i < 7; i++) {
        w = 7 - i;
        switch (dir) {
        case 2:   /* 下向き ▽ */
            y = 4 + i;
            for (x0 = 8 - w; x0 <= 7 + w; x0++) px(x0, y, TC_ROAD);
            px(8 - w, y, TC_ROCK);
            px(7 + w, y, TC_ROCK);
            break;
        case 0:   /* 上向き △ */
            y = 11 - i;
            for (x0 = 8 - w; x0 <= 7 + w; x0++) px(x0, y, TC_ROAD);
            px(8 - w, y, TC_LAND_SH);
            px(7 + w, y, TC_LAND_SH);
            break;
        case 1:   /* 右向き ▷ */
            y = 4 + i;
            for (x0 = 8 - w; x0 <= 7 + w; x0++) px(y, x0, TC_ROAD);
            px(y, 8 - w, TC_LAND_SH);
            px(y, 7 + w, TC_LAND_SH);
            break;
        default:  /* 左向き ◁ */
            y = 11 - i;
            for (x0 = 8 - w; x0 <= 7 + w; x0++) px(y, x0, TC_ROAD);
            px(y, 8 - w, TC_LAND_SH);
            px(y, 7 + w, TC_LAND_SH);
            break;
        }
    }
}

/* ====================================================================== */
/*  施設タイル                                                             */
/*                                                                        */
/*  施設は 32x32 の絵を 2x2 タイルに割って登録する。                        */
/*  背景を色0 (透過) にしておくと、地形の上に重ねられる。                   */
/* ====================================================================== */

static u8 g_big[32 * 32];      /* 32x32 の作業用キャンバス (1バイト1画素) */

static void bpx(int x, int y, u8 c)
{
    if (x < 0 || x >= 32 || y < 0 || y >= 32) return;
    g_big[y * 32 + x] = c;
}

static void brect(int x, int y, int w, int h, u8 c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) bpx(x + i, y + j, c);
}

static void bframe(int x, int y, int w, int h, u8 c)
{
    int i;
    for (i = 0; i < w; i++) { bpx(x + i, y, c); bpx(x + i, y + h - 1, c); }
    for (i = 0; i < h; i++) { bpx(x, y + i, c); bpx(x + w - 1, y + i, c); }
}

/* g_big の (qx,qy) 象限を 1 タイルとして登録する */
static void emit_quad(int id, int qx, int qy)
{
    int i, j;
    memset(g_buf, 0, sizeof(g_buf));
    for (j = 0; j < 16; j++) {
        for (i = 0; i < 16; i++) {
            px(i, j, g_big[(qy * 16 + j) * 32 + (qx * 16 + i)]);
        }
    }
    tilemap_define(id, g_buf);
}

static void emit_facility(int base_id)
{
    emit_quad(base_id + 0, 0, 0);
    emit_quad(base_id + 1, 1, 0);
    emit_quad(base_id + 2, 0, 1);
    emit_quad(base_id + 3, 1, 1);
}

static void big_clear(void)
{
    memset(g_big, 0, sizeof(g_big));   /* 0 = 透過 */
}

/* --- 家。roof で所有者を示す --- */
static void big_house(u8 roof)
{
    int i;
    int x = 4, y = 8, w = 24, h = 20, wall = 11;
    big_clear();
    brect(x + 2, y + h - wall, w - 4, wall, TC_LAND_HI);
    bframe(x + 2, y + h - wall, w - 4, wall, TC_OUTLINE);
    for (i = 0; i < h - wall; i++) {
        brect(x + i, y + i, w - i * 2, 1, roof);
    }
    brect(x, y + h - wall - 1, w, 1, TC_OUTLINE);
    brect(x + w / 2 - 2, y + h - wall + 3, 5, wall - 4, TC_OUTLINE);
}

static void big_shop(u8 accent)
{
    int i;
    int x = 3, y = 10, w = 26;
    big_clear();
    brect(x + 2, y + 8, w - 4, 12, TC_ROCK);
    bframe(x + 2, y + 8, w - 4, 12, TC_OUTLINE);
    for (i = 0; i < (w - 4) / 5; i++) {
        brect(x + 2 + i * 5, y + 2, 5, 6, (i & 1) ? TC_LAND_HI : accent);
    }
    brect(x, y, w, 2, accent);
}

static void big_torii(void)
{
    int x = 5, y = 8, w = 22, h = 22;
    big_clear();
    brect(x + 3, y + 6, 4, h - 6, TC_P_RED);
    brect(x + w - 7, y + 6, 4, h - 6, TC_P_RED);
    brect(x, y + 1, w, 5, TC_P_RED);
    brect(x + 3, y + 11, w - 6, 3, TC_P_RED);
}

static void big_castle(void)
{
    int i, x = 1, y = 4, w = 30;
    big_clear();
    brect(x + 3, y + 10, w - 6, 18, TC_ROCK);
    bframe(x + 3, y + 10, w - 6, 18, TC_OUTLINE);
    for (i = 0; i < 3; i++) {
        brect(x + 3 + i * 10, y, 7, 11, TC_LAND_HI);
        bframe(x + 3 + i * 10, y, 7, 11, TC_OUTLINE);
    }
    brect(x + w / 2 - 3, y + 19, 7, 9, TC_OUTLINE);
}

static void big_chest(u8 col)
{
    int i, x = 7, y = 12, w = 18;
    big_clear();
    brect(x, y + 6, w, 10, col);
    bframe(x, y + 6, w, 10, TC_OUTLINE);
    for (i = 0; i < 6; i++) brect(x + i, y + i, w - i * 2, 1, col);
    brect(x, y + 9, w, 1, TC_OUTLINE);
}

static void big_den(void)
{
    int i;
    big_clear();
    for (i = 0; i < 3; i++) {
        brect(6 + i * 8, 20 - (i & 1) * 3, 6, 11, TC_ROCK);
        bframe(6 + i * 8, 20 - (i & 1) * 3, 6, 11, TC_OUTLINE);
    }
    brect(15, 6, 2, 24, TC_LAND_SH);
    brect(17, 6, 10, 7, TC_P_RED);
}

static void big_cave(void)
{
    int i, j;
    big_clear();
    for (i = 0; i < 20; i++) {
        int ww = 28 * (i + 3) / 23;
        brect(16 - ww / 2, 10 + i, ww, 1, TC_ROCK);
    }
    for (j = 0; j < 9; j++) {
        int ww = 14 - (j * 3) / 4;
        brect(16 - ww / 2, 21 + j, ww, 1, TC_OUTLINE);
    }
}

static void big_office(void)
{
    big_clear();
    brect(4, 12, 24, 16, TC_LAND_HI);
    bframe(4, 12, 24, 16, TC_OUTLINE);
    brect(2, 7, 28, 5, TC_P_YELLOW);
    /* G の字を粗く */
    brect(11, 16, 10, 2, TC_OUTLINE);
    brect(11, 16, 2, 8, TC_OUTLINE);
    brect(11, 22, 10, 2, TC_OUTLINE);
    brect(19, 20, 2, 4, TC_OUTLINE);
    brect(16, 19, 5, 2, TC_OUTLINE);
}

static void big_circle(void)
{
    int r, a;
    big_clear();
    for (r = 5; r <= 13; r += 4) {
        for (a = 0; a < 64; a++) {
            /* 粗い円 (整数だけで済ませる) */
            int qx = (a % 16) - 8;
            int qy = (a / 16) * 8 - 12;
            (void)qx; (void)qy;
        }
    }
    for (r = 5; r <= 13; r += 4) {
        int x, y;
        for (x = -r; x <= r; x++) {
            for (y = -r; y <= r; y++) {
                int d = x * x + y * y - r * r;
                if (d > -r && d < r) bpx(16 + x, 16 + y, TC_MAGIC);
            }
        }
    }
}

static void big_gate(void)
{
    big_clear();
    brect(4, 10, 7, 20, TC_P_RED);
    brect(21, 10, 7, 20, TC_P_RED);
    brect(2, 5, 28, 6, TC_P_RED);
}

void view_tiles_define_facilities(void)
{
    static const u8 roofs[5] = { TC_ROCK, TC_P_RED, TC_P_BLUE,
                                 TC_P_GREEN, TC_P_YELLOW };
    int i;

    /* 村: 無主 + プレイヤー4色 */
    for (i = 0; i < 5; i++) {
        big_house(roofs[i]);
        emit_facility(TILE_FAC_VILLAGE + i * 4);
    }

    big_den();      emit_facility(TILE_FAC_BATTLE);
    big_chest(TC_P_YELLOW); emit_facility(TILE_FAC_CHEST);
    big_chest(TC_MAGIC);    emit_facility(TILE_FAC_GOLDCHEST);
    big_shop(TC_SHRINE);    emit_facility(TILE_FAC_ITEM);
    big_shop(TC_P_BLUE);    emit_facility(TILE_FAC_EQUIP);
    big_shop(TC_MAGIC);     emit_facility(TILE_FAC_MAGIC);
    big_torii();    emit_facility(TILE_FAC_CHURCH);
    big_circle();   emit_facility(TILE_FAC_CIRCLE);
    big_gate();     emit_facility(TILE_FAC_GATE);
    big_castle();   emit_facility(TILE_FAC_CASTLE);
    big_office();   emit_facility(TILE_FAC_COLLECT);
    big_cave();     emit_facility(TILE_FAC_DUNGEON);
}

void view_tiles_define(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        make_land(i);
        tilemap_define(TILE_LAND + i, g_buf);
    }
    make_land_hatch();
    tilemap_define(TILE_LAND_HATCH, g_buf);

    for (i = 0; i < 2; i++) {
        make_sea(i);
        tilemap_define(TILE_SEA + i, g_buf);
    }
    for (i = 0; i < 4; i++) {
        make_shore(i);
        tilemap_define(TILE_SHORE + i, g_buf);
    }

    make_forest();
    tilemap_define(TILE_FOREST, g_buf);
    make_mountain();
    tilemap_define(TILE_MOUNTAIN, g_buf);

    for (i = 0; i < 4; i++) {
        make_arrow(i);
        tilemap_define(TILE_ARROW + i, g_buf);
    }
}
