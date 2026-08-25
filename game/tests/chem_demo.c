/* ======================================================================== */
/*  CHEM_DEMO.C — 化学エンジン ビジュアルデモ                                */
/*                                                                          */
/*  libos32gfx 直接使用 (Pyxel不使用)                                       */
/*  640x400 PC-98グラフィック画面にグリッドとUIを描画する。                   */
/*                                                                          */
/*  操作:                                                                    */
/*    矢印キー — カーソル移動                                                */
/*    1-5      — 属性付与 (Fire/Water/Ice/Wind/Elec)                        */
/*    R        — リセット   ESC — 終了                                       */
/* ======================================================================== */

#include "os32api.h"
#include "libos32gfx.h"
#include "gfx_font.h"
#include "libos32chem.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  レイアウト (PC-98 640x400)                                             */
/* ====================================================================== */

#define TILE   24       /* 1マス 24x24 ピクセル */
#define GW     16       /* 横16マス = 384px */
#define GH     14       /* 縦14マス = 336px */
#define GX_OFF 8        /* グリッド左端オフセット */
#define GY_OFF 8        /* グリッド上端オフセット */

/* 右UIパネル */
#define UI_X   (GX_OFF + GW * TILE + 16)  /* 408 */

/* 色 (PC-98 4bit パレット) */
#define C_BG     0
#define C_GRID   1    /* 紺 */
#define C_TEXT   7    /* 白 */
#define C_DIM    5    /* グレー */
#define C_FIRE   8    /* 赤 */
#define C_WATER  12   /* 青 */
#define C_ICE    6    /* シアン */
#define C_WOOD   4    /* 茶 */
#define C_GRASS  3    /* 緑 */
#define C_ELEC   10   /* 黄 */
#define C_STEAM  5    /* グレー */
#define C_STONE  13   /* ライトグレー */
#define C_METAL  15   /* 白に近い */
#define C_BURN   9    /* オレンジ */
#define C_HP_FG  11   /* ライトグリーン */
#define C_HP_BG  2    /* 暗い */
#define C_CURSOR 7    /* 白 */

/* スキャンコード */
#define SC_ESC    0x00
#define SC_1      0x01
#define SC_2      0x02
#define SC_3      0x03
#define SC_4      0x04
#define SC_5      0x05
#define SC_R      0x13
#define SC_UP     0x3A
#define SC_DOWN   0x3D
#define SC_LEFT   0x3B
#define SC_RIGHT  0x3C

/* 反応インターバル */
#define TICK_INTERVAL 4

/* ====================================================================== */
/*  グローバル                                                             */
/* ====================================================================== */

static int g_cx, g_cy, g_frame;
static int g_grid[GW * GH];
static int g_running;

/* 前フレームのキー状態 (トリガー検出用) */
static int g_prev[12]; /* SC_ESC..SC_RIGHT の押下状態 */
static const int g_sclist[] = {
    SC_ESC, SC_1, SC_2, SC_3, SC_4, SC_5, SC_R,
    SC_UP, SC_DOWN, SC_LEFT, SC_RIGHT
};
#define NUM_SC 11
static int g_cur[12];

/* ====================================================================== */
/*  キー入力                                                               */
/* ====================================================================== */

static void keys_poll(void)
{
    int i;
    for (i = 0; i < NUM_SC; i++)
        g_cur[i] = api->kbd_is_pressed(g_sclist[i]);
}

static int key_trigger(int idx)
{
    return g_cur[idx] && !g_prev[idx];
}

static int key_down(int idx)
{
    return g_cur[idx];
}

static void keys_save(void)
{
    int i;
    for (i = 0; i < NUM_SC; i++)
        g_prev[i] = g_cur[i];
}

/* インデックス定数 */
#define KI_ESC   0
#define KI_1     1
#define KI_2     2
#define KI_3     3
#define KI_4     4
#define KI_5     5
#define KI_R     6
#define KI_UP    7
#define KI_DOWN  8
#define KI_LEFT  9
#define KI_RIGHT 10

/* ====================================================================== */
/*  ワールド                                                               */
/* ====================================================================== */

static void put(int x, int y, u32 elem, i16 temp)
{
    int id = chem_spawn(0, (i16)x, (i16)y);
    if (id >= 0) {
        chem_add_element(id, elem);
        if (temp != 20) chem_set_temperature(id, temp);
        g_grid[y * GW + x] = id;
    }
}

static void setup_world(void)
{
    int x, y;
    chem_reset();
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            g_grid[y * GW + x] = -1;

    /* 森 (木の帯 行2-3) */
    for (x = 1; x < 15; x++) put(x, 2, ELEM_WOOD, 20);
    for (x = 3; x < 10; x++) put(x, 3, ELEM_WOOD, 20);
    /* 草原 (行4) */
    for (x = 0; x < 16; x++) put(x, 4, ELEM_GRASS, 20);
    /* 水溜まり (行7) */
    for (x = 5; x < 11; x++) put(x, 7, ELEM_WATER, 15);
    /* 氷 (右上) */
    put(13, 5, ELEM_ICE, -10);
    put(14, 5, ELEM_ICE, -10);
    put(13, 6, ELEM_ICE, -10);
    /* 金属 (左下) */
    put(1, 10, ELEM_METAL, 20);
    put(2, 10, ELEM_METAL, 20);
    put(3, 10, ELEM_METAL, 20);
    /* 石 (行11) */
    for (x = 5; x < 13; x++) put(x, 11, ELEM_STONE, 20);

    g_cx = 5; g_cy = 2;
}

/* ====================================================================== */
/*  描画ヘルパー                                                           */
/* ====================================================================== */

static int ecolor(const ChemObject *o)
{
    if (o->state == CHEM_STATE_BURNING)
        return (g_frame & 4) ? C_FIRE : C_BURN;
    if (o->state == CHEM_STATE_FROZEN)  return C_ICE;
    if (o->state == CHEM_STATE_CHARGED)
        return (g_frame & 2) ? C_ELEC : C_TEXT;
    if (o->elements & ELEM_FIRE)     return C_FIRE;
    if (o->elements & ELEM_STEAM)    return C_STEAM;
    if (o->elements & ELEM_WATER)    return C_WATER;
    if (o->elements & ELEM_ICE)      return C_ICE;
    if (o->elements & ELEM_ELECTRIC) return C_ELEC;
    if (o->elements & ELEM_WOOD)     return C_WOOD;
    if (o->elements & ELEM_GRASS)    return C_GRASS;
    if (o->elements & ELEM_METAL)    return C_METAL;
    if (o->elements & ELEM_STONE)    return C_STONE;
    return C_GRID;
}

static const char *elabel(const ChemObject *o)
{
    if (o->state == CHEM_STATE_BURNING) return "**";
    if (o->state == CHEM_STATE_FROZEN)  return "##";
    if (o->state == CHEM_STATE_CHARGED) return "~~";
    if (o->elements & ELEM_FIRE)     return "Fi";
    if (o->elements & ELEM_STEAM)    return "Sm";
    if (o->elements & ELEM_WATER)    return "Wa";
    if (o->elements & ELEM_ICE)      return "Ic";
    if (o->elements & ELEM_ELECTRIC) return "El";
    if (o->elements & ELEM_WOOD)     return "Wo";
    if (o->elements & ELEM_GRASS)    return "Gr";
    if (o->elements & ELEM_METAL)    return "Mt";
    if (o->elements & ELEM_STONE)    return "Sn";
    if (o->elements & ELEM_WIND)     return "Wd";
    return "..";
}

static const char *state_name(int st)
{
    switch (st) {
    case CHEM_STATE_IDLE:    return "IDLE";
    case CHEM_STATE_BURNING: return "BURN";
    case CHEM_STATE_FROZEN:  return "FRZN";
    case CHEM_STATE_WET:     return "WET ";
    case CHEM_STATE_CHARGED: return "CHRG";
    default:                 return "??? ";
    }
}

/* ====================================================================== */
/*  隣接反応                                                               */
/* ====================================================================== */

static void check_reactions(void)
{
    int x, y;
    for (y = 0; y < GH; y++) {
        for (x = 0; x < GW; x++) {
            int a = g_grid[y * GW + x];
            int b;
            if (a < 0 || !chem_get(a)) continue;
            if (x + 1 < GW) {
                b = g_grid[y * GW + x + 1];
                if (b >= 0 && chem_get(b)) chem_react(a, b);
            }
            if (y + 1 < GH) {
                b = g_grid[(y+1) * GW + x];
                if (b >= 0 && chem_get(b)) chem_react(a, b);
            }
        }
    }
}

/* ====================================================================== */
/*  描画                                                                   */
/* ====================================================================== */

static void draw_grid(void)
{
    int x, y;

    for (y = 0; y < GH; y++) {
        for (x = 0; x < GW; x++) {
            int px = GX_OFF + x * TILE;
            int py = GY_OFF + y * TILE;
            int idx = y * GW + x;
            int id = g_grid[idx];

            if (id >= 0) {
                const ChemObject *o = chem_get(id);
                if (o) {
                    int col = ecolor(o);
                    /* タイル塗りつぶし */
                    gfx_fill_rect(px + 1, py + 1,
                                  TILE - 2, TILE - 2, (u8)col);
                    /* 2文字ラベル (8x16フォントで中央寄せ) */
                    gfx_puts(px + 4, py + 4, elabel(o), C_BG);
                    /* HPバー */
                    if (o->hp < 100 && o->hp > 0) {
                        int bw = (TILE - 2) * (int)o->hp / 100;
                        if (bw < 1) bw = 1;
                        gfx_fill_rect(px+1, py+TILE-4,
                                      TILE-2, 3, C_HP_BG);
                        gfx_fill_rect(px+1, py+TILE-4,
                                      bw, 3, C_HP_FG);
                    }
                } else {
                    g_grid[idx] = -1;
                }
            }

            /* グリッド枠 */
            gfx_rect(px, py, TILE, TILE, C_GRID);
        }
    }

    /* カーソル (二重枠) */
    {
        int cx = GX_OFF + g_cx * TILE;
        int cy = GY_OFF + g_cy * TILE;
        gfx_rect(cx - 1, cy - 1, TILE + 2, TILE + 2, C_CURSOR);
        gfx_rect(cx - 2, cy - 2, TILE + 4, TILE + 4, C_CURSOR);
    }
}

static void draw_ui(void)
{
    int py = GY_OFF;
    int sid;
    const ChemObject *so;

    /* タイトル */
    gfx_puts(UI_X, py, "CHEM ENGINE", C_TEXT);
    py += 18;
    gfx_hline(UI_X, py, 200, C_DIM);
    py += 6;

    /* 選択タイル情報 */
    sid = g_grid[g_cy * GW + g_cx];
    so = (sid >= 0) ? chem_get(sid) : (const ChemObject *)0;

    if (so) {
        gfx_printf(UI_X, py, (u8)ecolor(so),
                   "%s", elabel(so));
        py += 18;
        gfx_printf(UI_X, py, C_TEXT,
                   "Temp: %d", (int)so->temperature);
        py += 18;
        gfx_printf(UI_X, py, C_TEXT,
                   "HP:   %d", (int)so->hp);
        py += 18;
        gfx_printf(UI_X, py, C_TEXT,
                   "State: %s", state_name(so->state));
        py += 18;
        gfx_printf(UI_X, py, C_DIM,
                   "Elem: %08X", (unsigned)so->elements);
    } else {
        gfx_puts(UI_X, py, "(empty)", C_DIM);
        py += 18 * 4;
    }

    /* 統計 */
    py += 24;
    gfx_hline(UI_X, py, 200, C_DIM);
    py += 6;
    gfx_printf(UI_X, py, C_TEXT,
               "Objects: %d", chem_active_count());
    py += 18;
    gfx_printf(UI_X, py, C_FIRE,
               "Burning: %d", chem_count_burning());
    py += 18;
    gfx_printf(UI_X, py, C_DIM,
               "Tick:    %d", g_frame / TICK_INTERVAL);

    /* キーヘルプ */
    py += 30;
    gfx_hline(UI_X, py, 200, C_DIM);
    py += 6;
    gfx_puts(UI_X, py,       "1:Fire  2:Water", C_TEXT);
    py += 16;
    gfx_puts(UI_X, py,       "3:Ice   4:Wind", C_TEXT);
    py += 16;
    gfx_puts(UI_X, py,       "5:Elec  R:Reset", C_TEXT);
    py += 16;
    gfx_puts(UI_X, py,       "Arrows:Move  ESC:Quit", C_DIM);
}

/* ====================================================================== */
/*  メインループ                                                           */
/* ====================================================================== */

static void game_loop(void)
{
    u32 last_tick;
    g_running = 1;
    last_tick = api->get_tick();

    /* 初回描画 */
    gfx_clear(C_BG);
    draw_grid();
    draw_ui();
    api->gfx_present_dirty();

    while (g_running) {
        int idx, id;
        g_frame++;

        /* --- 入力 --- */
        keys_poll();

        if (key_down(KI_ESC)) { g_running = 0; break; }

        if (key_trigger(KI_UP)    && g_cy > 0)    g_cy--;
        if (key_trigger(KI_DOWN)  && g_cy < GH-1) g_cy++;
        if (key_trigger(KI_LEFT)  && g_cx > 0)    g_cx--;
        if (key_trigger(KI_RIGHT) && g_cx < GW-1) g_cx++;

        idx = g_cy * GW + g_cx;
        id = g_grid[idx];

        if (key_trigger(KI_1) && id >= 0 && chem_get(id))
            chem_add_element(id, ELEM_FIRE);
        if (key_trigger(KI_2) && id >= 0 && chem_get(id))
            chem_add_element(id, ELEM_WATER);
        if (key_trigger(KI_3) && id >= 0 && chem_get(id)) {
            chem_add_element(id, ELEM_ICE);
            chem_set_temperature(id, -10);
        }
        if (key_trigger(KI_4) && id >= 0 && chem_get(id))
            chem_add_element(id, ELEM_WIND);
        if (key_trigger(KI_5) && id >= 0 && chem_get(id))
            chem_add_element(id, ELEM_ELECTRIC);
        if (key_trigger(KI_R)) setup_world();

        keys_save();

        /* --- シミュレーション (Nフレーム毎) --- */
        if ((g_frame % TICK_INTERVAL) == 0) {
            check_reactions();
            chem_update();
        }

        /* --- 描画 → 全画面VRAM転送 --- */
        gfx_clear(C_BG);
        draw_grid();
        draw_ui();
        api->gfx_present_dirty();

        /* フレーム待ち (tick=10ms, 3tick ~30fps) */
        while (api->get_tick() - last_tick < 3) {
            api->sys_halt();
        }
        last_tick = api->get_tick();
    }
}

/* ====================================================================== */
/*  main                                                                   */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    int rc;
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_WHITE, "chem_demo: initializing...\n");

    /* 化学エンジン初期化 */
    rc = chem_init("/db/chem.db");
    if (rc < 0) {
        api->kprintf(ATTR_RED, "chem_demo: chem_init failed (%d)\n", rc);
        return 1;
    }

    /* GFX初期化 */
    libos32gfx_init(kapi);

    /* ワールド初期化 */
    setup_world();

    /* メインループ */
    game_loop();

    /* 終了処理 */
    libos32gfx_shutdown();
    chem_shutdown();

    return 0;
}
