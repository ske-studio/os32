#include "view_board.h"
#include "libos32board.h"
#include "libos32gfx.h"
#include "libos32math.h"
#include "libos32econ.h"
#include "game_glue.h"   /* glue_village_estate() (マスID -> 村マスタ) */
#include "libos32tilemap.h"
#include "view_tiles.h"
#include <stdio.h>
#include <string.h>

static KernelAPI *api;
static BoardPlayer players_anim[4];

static void settle_arrival(BoardPlayer *p);
static int mass_screen_origin(int mass_id, int *ox, int *oy);
static const BoardMass *mass_at_grid(int gx, int gy);
static int is_linked(u16 a, u16 b);

/* ---- グリッド座標 -> マスの索引 ------------------------------------
   fill_bg() は毎フレーム数百タイル分マスを引くので、全マス線形走査だと
   低クロック機 (386/16MHz) では 1 フレームに秒単位かかる。
   盤面は不変なのでロード時に一度だけ表を組む。値は「マス配列の添字+1」
   (0 = マスなし)。 */
#define GRID_LUT_W 64
#define GRID_LUT_H 64
static u8 g_grid_lut[GRID_LUT_H][GRID_LUT_W];

static void build_grid_lut(void) {
    int n = board_mass_count();
    int i;
    memset(g_grid_lut, 0, sizeof(g_grid_lut));
    for (i = 0; i < n && i < 254; i++) {
        const BoardMass *m = board_get_mass_at(i);
        if (m && m->x < GRID_LUT_W && m->y < GRID_LUT_H) {
            g_grid_lut[m->y][m->x] = (u8)(i + 1);
        }
    }
}

void view_board_init(KernelAPI *kapi) {
    int i;
    api = kapi;
    for (i = 0; i < 4; i++) {
        players_anim[i].pos = 0;
        players_anim[i].target_pos = 0;
        players_anim[i].anim_x = 0;
        players_anim[i].anim_y = 0;
        players_anim[i].moving = 0;
        players_anim[i].path_len = 0;
        players_anim[i].path_idx = 0;
        players_anim[i].anim_timer = 0;
        players_anim[i].rem_steps = 0;
        players_anim[i].prev_mass = -1;
        players_anim[i].branching = 0;
        players_anim[i].branch_count = 0;
    }
}

int view_board_load(const char *db_path) {
    int ret = board_init(db_path);
    if (ret == 0) {
        int i;
        build_grid_lut();
        for (i = 0; i < 4; i++) {
            view_board_get_mass_pos(players_anim[i].pos, &players_anim[i].anim_x, &players_anim[i].anim_y);
        }
    }
    return ret;
}

/* ======================================================================== */
/*  カメラ (ピクセル単位のスクロールビュー)                                  */
/*                                                                          */
/*  盤面は 2D グリッド (masses.x/y がマス座標)。世界座標は 1マス=48px の      */
/*  ピクセル平面で、カメラ中心 (cam_wx, cam_wy) が画面中央に写る。           */
/*  手番プレイヤーの移動中はカメラがキャラの世界座標に追従するので、          */
/*  キャラは画面中央に固定され、地面のほうが流れて見える。                    */
/*  手番の切り替えとワープでは演出なしで即座にカメラが切り替わる。           */
/* ======================================================================== */

static int cam_center = 0;      /* 注目マスID (view_board_get_focus 用) */
static int cam_focus_pid = 0;   /* 手番プレイヤー (最前面に描く) */
static int cam_wx = 0;          /* カメラ中心の世界座標 (px) */
static int cam_wy = 0;

/* 負の値でも床方向へ丸める除算 (世界座標 -> タイル/マス座標用) */
static int floordiv(int a, int b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

/* マス中心の世界座標 */
static void mass_world_center(const BoardMass *m, int *wx, int *wy) {
    *wx = (int)m->x * VIEW_CELL_W + VIEW_CELL_W / 2;
    *wy = (int)m->y * VIEW_CELL_H + VIEW_CELL_H / 2;
}

/* 画面左上の世界座標。X は 8px (VRAMバイト境界) に丸める。
   PC-98 のプレーン VRAM は横 8px = 1 バイトなので、X が非整列だと
   全タイルの blit がビットシフト経路に落ち、水平スクロールだけ
   極端に遅くなる。丸めた値を地形もキャラも共通で使い、ズレを作らない。
   (負値でも &~7 は floor 方向に働く: 2の補数) */
static void view_origin(int *vx0, int *vy0) {
    *vx0 = (cam_wx - VIEW_FIELD_W / 2) & ~7;
    *vy0 = cam_wy - VIEW_FIELD_H / 2;
}

/* a と b が直接つながっているか */
static int is_linked(u16 a, u16 b) {
    const BoardMass *m = board_get_mass(a);
    int i;
    if (!m) return 0;
    for (i = 0; i < (int)m->connect_count; i++) {
        if (m->connect[i] == b) return 1;
    }
    return 0;
}

void view_board_set_focus(int mass_id) {
    cam_center = mass_id;
    /* 手番プレイヤーの移動中は view_board_update() の追従に任せる。
       停止中の呼び出し (手番切り替え・ワープ) は即スナップ。 */
    if (!players_anim[cam_focus_pid].moving) {
        const BoardMass *m = board_get_mass((u16)mass_id);
        if (m) mass_world_center(m, &cam_wx, &cam_wy);
    }
}

void view_board_set_focus_player(int pid) {
    cam_focus_pid = pid;
}

int view_board_get_focus(void) {
    return cam_center;
}

/* グリッド座標からマスを引く (ロード時に組んだ索引で O(1)) */
static const BoardMass *mass_at_grid(int gx, int gy) {
    u8 idx;
    if ((unsigned)gx >= GRID_LUT_W || (unsigned)gy >= GRID_LUT_H)
        return (const BoardMass *)0;
    idx = g_grid_lut[gy][gx];
    return idx ? board_get_mass_at((int)idx - 1) : (const BoardMass *)0;
}

/* マスの足元の「世界座標」を返す。移動アニメはこの座標系で補間する */
void view_board_get_mass_pos(int mass_id, int *x, int *y) {
    const BoardMass *m = board_get_mass((u16)mass_id);
    if (m) {
        mass_world_center(m, x, y);
        *y += VIEW_CELL_H / 2 - 6;   /* マスの足元 */
    } else {
        *x = -200;
        *y = -200;
    }
}

/* ---- 地形: libos32tilemap の BG0 に敷く ----------------------------- */

/* グリッド1マスは 3x3 タイル (48x48px)。
   盤面 400x304 は 25x19 タイルだが、ピクセルスクロールの端数 (0..15px) を
   埋めるために BG プレーンの糊しろを含めた 26x20 全体を敷き直す。 */
#define GT   3                       /* 1マスあたりのタイル数 */

/* そのグリッド座標にマスがあるか */
static int has_mass(int gx, int gy) {
    return mass_at_grid(gx, gy) != (const BoardMass *)0;
}

/* マスの周囲1セル以内に陸があるか (陸を少し広げて海との境を作る) */
static int is_land(int gx, int gy) {
    int dx, dy;
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            if (has_mass(gx + dx, gy + dy)) return 1;
        }
    }
    return 0;
}

/* 直前のマス (IDが減る側) から入ってくる向き。無ければ -1 */
static int incoming_dir(const BoardMass *m) {
    static const int dxs[4] = { 0, 1, 0, -1 };
    static const int dys[4] = { -1, 0, 1, 0 };
    int d;
    for (d = 0; d < 4; d++) {
        const BoardMass *n = mass_at_grid((int)m->x + dxs[d], (int)m->y + dys[d]);
        if (n && is_linked(m->id, n->id) && n->id < m->id) {
            /* n から m へ入ってくるので、向きは d の逆 */
            return (d + 2) & 3;
        }
    }
    return -1;
}

/* 進行方向 (IDが増える側の隣接マス)。見つからなければ -1 */
static int forward_dir(const BoardMass *m) {
    static const int dxs[4] = { 0, 1, 0, -1 };
    static const int dys[4] = { -1, 0, 1, 0 };
    int d;
    for (d = 0; d < 4; d++) {
        const BoardMass *n = mass_at_grid((int)m->x + dxs[d], (int)m->y + dys[d]);
        if (n && is_linked(m->id, n->id) && n->id > m->id) return d;
    }
    /* 前進先が無ければ、繋がっている方向を適当に指す */
    for (d = 0; d < 4; d++) {
        const BoardMass *n = mass_at_grid((int)m->x + dxs[d], (int)m->y + dys[d]);
        if (n && is_linked(m->id, n->id)) return d;
    }
    return 1;
}

/* このタイルが道の一部か。
   入ってくる向きと出ていく向きの2本を、マス中心で繋いだ形にする
   (曲がり角では L 字になる)。 */
static int on_path(const BoardMass *m, int lx, int ly) {
    int fwd = forward_dir(m);
    int inc = incoming_dir(m);
    int d;

    if (lx == 1 && ly == 1) return 1;    /* 中心は必ず道 */

    for (d = 0; d < 2; d++) {
        int dir = d ? inc : fwd;
        if (dir < 0) continue;
        /* dir 方向へ中心から1タイル伸ばす */
        if (dir == 0 && lx == 1 && ly == 0) return 1;   /* 上 */
        if (dir == 1 && lx == 2 && ly == 1) return 1;   /* 右 */
        if (dir == 2 && lx == 1 && ly == 2) return 1;   /* 下 */
        if (dir == 3 && lx == 0 && ly == 1) return 1;   /* 左 */
    }
    return 0;
}

/* そのタイルに置く矢印の向き。道は常に「進行方向」を指す */
static int path_dir(const BoardMass *m, int lx, int ly) {
    int fwd = forward_dir(m);
    (void)lx; (void)ly;
    return (fwd >= 0) ? fwd : 1;
}

/* カメラ位置から BG0 のタイルマップを組み立てる。
   タイル窓は世界タイル座標 t0 から始まり、16px 未満の端数は
   tilemap_scroll() のピクセルスクロールで吸収する。 */
static void fill_bg(void) {
    int vx0, vy0, t0x, t0y;
    int tr, tc;

    view_origin(&vx0, &vy0);
    t0x = floordiv(vx0, 16);
    t0y = floordiv(vy0, 16);

    for (tr = 0; tr < TILEMAP_ROWS; tr++) {
        for (tc = 0; tc < TILEMAP_COLS; tc++) {
            int wtx = t0x + tc;            /* 世界タイル座標 */
            int wty = t0y + tr;
            int gx = floordiv(wtx, GT);
            int gy = floordiv(wty, GT);
            int lx = wtx - gx * GT;
            int ly = wty - gy * GT;
            const BoardMass *m = mass_at_grid(gx, gy);
            u16 tile;

            if (m && on_path(m, lx, ly)) {
                /* 道は矢印で埋める。マスを貫く3タイル分を敷くことで、
                   隣のマスの矢印と繋がって一本の帯として読める。
                   中心に1つだけ置くと 48px 間隔が空いて道に見えない。 */
                tile = (u16)(TILE_ARROW + path_dir(m, lx, ly));
            } else if (is_land(gx, gy)) {
                if (m) {
                    /* 模様のハッシュは必ず世界座標から取る。
                       画面タイル座標を混ぜるとスクロールで模様が泳ぐ */
                    tile = (u16)(TILE_LAND + ((gx * 3 + gy * 5 + wtx + wty) & 3));
                } else {
                    /* 道の無い陸。地形はグリッドのマス単位でまとめる。
                       タイルごとに変えると散らばって地図に見えない。 */
                    int h = (gx * 7 + gy * 13) % 9;
                    if (h == 0) {
                        tile = TILE_FOREST;
                    } else if (h == 1) {
                        tile = TILE_MOUNTAIN;
                    } else if (h == 2 && ly == 1) {
                        tile = TILE_LAND_HATCH;
                    } else {
                        tile = (u16)(TILE_LAND + ((gx + gy + lx) & 3));
                    }
                }
            } else {
                /* 海。陸に接している「その辺のタイルだけ」を海岸にする。
                   セル全体を海岸タイルで埋めると横縞に見えてしまう。 */
                int d = -1;
                if (ly == 0 && is_land(gx, gy - 1))          d = 0;
                else if (lx == GT - 1 && is_land(gx + 1, gy)) d = 1;
                else if (ly == GT - 1 && is_land(gx, gy + 1)) d = 2;
                else if (lx == 0 && is_land(gx - 1, gy))      d = 3;
                if (d >= 0) tile = (u16)(TILE_SHORE + d);
                else        tile = (u16)(TILE_SEA + ((gx + gy + lx) & 1));
            }
            tilemap_set(0, tc, tr, tile);
        }
    }
    tilemap_scroll(0, vx0 - t0x * 16, vy0 - t0y * 16);
}

/* マスID -> 画面上のセル左上。ビューに全くかからないなら 0 */
static int mass_screen_origin(int mass_id, int *ox, int *oy) {
    const BoardMass *m = board_get_mass((u16)mass_id);
    int vx0, vy0;

    if (!m) return 0;
    view_origin(&vx0, &vy0);
    *ox = (int)m->x * VIEW_CELL_W - vx0;
    *oy = (int)m->y * VIEW_CELL_H - vy0;
    if (*ox <= -VIEW_CELL_W || *ox >= VIEW_FIELD_W ||
        *oy <= -VIEW_CELL_H || *oy >= VIEW_FIELD_H)
        return 0;
    return 1;
}




/* マス種別 -> 施設タイルの基底ID。施設が無ければ 0 */
static u16 facility_tile_base(const BoardMass *m) {
    switch (m->type) {
    case MASS_VILLAGE: {
        const EconEstate *e = glue_village_estate(m->id);
        int slot = 0;   /* 0=無主, 1..4=プレイヤー */
        if (e && e->owner != ECON_OWNER_NONE && e->owner < 4) {
            slot = (int)e->owner + 1;
        }
        return (u16)(TILE_FAC_VILLAGE + slot * 4);
    }
    case MASS_BATTLE:      return TILE_FAC_BATTLE;
    case MASS_TREASURE:    return TILE_FAC_CHEST;
    case MASS_MAGIC_CHEST: return TILE_FAC_GOLDCHEST;
    case MASS_ITEM_SHOP:   return TILE_FAC_ITEM;
    case MASS_EQUIP_SHOP:  return TILE_FAC_EQUIP;
    case MASS_MAGIC_SHOP:  return TILE_FAC_MAGIC;
    case MASS_CHURCH:      return TILE_FAC_CHURCH;
    case MASS_CIRCLE:      return TILE_FAC_CIRCLE;
    case MASS_GATE:        return TILE_FAC_GATE;
    case MASS_CASTLE:      return TILE_FAC_CASTLE;
    case MASS_COLLECT:     return TILE_FAC_COLLECT;
    case MASS_DUNGEON:     return TILE_FAC_DUNGEON;
    default:               return 0;
    }
}

/* 施設を BG1 に置く。1マス3x3タイルのうち、上段2x2 に 32x32 の絵を載せる
   (下段は道と駒のために空けておく) */
static void fill_bg_facilities(void) {
    int vx0, vy0, t0x, t0y, g0x, g0y;
    int gx, gy;

    view_origin(&vx0, &vy0);
    t0x = floordiv(vx0, 16);
    t0y = floordiv(vy0, 16);
    g0x = floordiv(t0x, GT);
    g0y = floordiv(t0y, GT);

    tilemap_fill(1, 0);   /* 0 = 透過タイル */

    for (gy = g0y; gy * GT < t0y + TILEMAP_ROWS; gy++) {
        for (gx = g0x; gx * GT < t0x + TILEMAP_COLS; gx++) {
            const BoardMass *m = mass_at_grid(gx, gy);
            u16 base;
            int bc, br, q;
            if (!m) continue;
            base = facility_tile_base(m);
            if (!base) continue;
            bc = gx * GT - t0x;   /* セル左上の BG タイル座標 */
            br = gy * GT - t0y;
            /* 窓の端では 2x2 のうち窓内のタイルだけを置く */
            for (q = 0; q < 4; q++) {
                int qc = bc + (q & 1);
                int qr = br + (q >> 1);
                if (qc < 0 || qr < 0 ||
                    qc >= TILEMAP_COLS || qr >= TILEMAP_ROWS) continue;
                tilemap_set(1, qc, qr, (u16)(base + q));
            }
        }
    }
    tilemap_scroll(1, vx0 - t0x * 16, vy0 - t0y * 16);
}

/* キャラクタ。下端中央が (cx, by) */
static void draw_character(int cx, int by, u8 col, int marker) {
    const int w = 18, h = 26;
    int x = cx - w / 2;
    int y = by - h;
    int head = 10;
    int i;

    gfx_fill_rect(x + 3, by - 2, w - 6, 2, TC_OUTLINE);          /* 影 */
    gfx_fill_rect(x + 2, y + head, w - 4, h - head - 3, col);    /* 体 */
    gfx_rect(x + 2, y + head, w - 4, h - head - 3, TC_OUTLINE);
    gfx_fill_rect(x, y + head + 2, 3, 7, col);                   /* 腕 */
    gfx_fill_rect(x + w - 3, y + head + 2, 3, 7, col);
    gfx_fill_rect(x + 3, y + 2, w - 6, head - 2, TC_LAND_HI);    /* 顔 */
    gfx_rect(x + 3, y + 2, w - 6, head - 2, TC_OUTLINE);
    gfx_fill_rect(x + 2, y, w - 4, 4, TC_OUTLINE);               /* 髪 */
    gfx_fill_rect(x + 1, y + 3, 3, 4, TC_OUTLINE);
    gfx_fill_rect(x + w - 4, y + 3, 3, 4, TC_OUTLINE);
    gfx_fill_rect(x + 6, y + head - 4, 2, 3, TC_OUTLINE);        /* 目 */
    gfx_fill_rect(x + w - 8, y + head - 4, 2, 3, TC_OUTLINE);
    gfx_fill_rect(x + 3, by - 3, 4, 3, TC_LAND_SH);              /* 足 */
    gfx_fill_rect(x + w - 7, by - 3, 4, 3, TC_LAND_SH);

    if (marker) {
        for (i = 0; i < 8; i++) {
            gfx_fill_rect(cx - 7 + i, y - 14 + i, 15 - i * 2, 1, TC_OUTLINE);
        }
        for (i = 0; i < 6; i++) {
            gfx_fill_rect(cx - 5 + i, y - 13 + i, 11 - i * 2, 1, TC_P_YELLOW);
        }
    }
}

/* 施設の所有権など、カメラ以外の理由で盤面の見た目が変わったときに
   呼んで再描画を要求する */
static int board_invalid = 1;
void view_board_invalidate(void) {
    board_invalid = 1;
}

/* 盤面の見た目を決める要素のチェックサム。
   前フレームと一致するかぎり描画そのものをスキップする (16MHz 対策)。 */
static u32 draw_signature(void) {
    u32 sig = 0;
    int i;
    sig = (u32)cam_wx * 131u + (u32)cam_wy * 137u + (u32)cam_focus_pid;
    for (i = 0; i < 4; i++) {
        const BoardPlayer *p = &players_anim[i];
        sig = sig * 31u + (u32)p->pos;
        sig = sig * 31u + (u32)p->anim_x;
        sig = sig * 31u + (u32)p->anim_y;
        sig = sig * 31u + (u32)p->moving;
    }
    return sig;
}

int view_board_draw(void) {
    int i, k;
    int order[4];
    int n = 0;

    /* 変化がないフレームは描かない。全タイル合成 + VRAM 転送が
       まるごと消えるので、待機中のフレームがほぼ無コストになる */
    {
        static u32 last_sig = 0xFFFFFFFFu;
        u32 sig = draw_signature();
        if (!board_invalid && sig == last_sig) return 0;
        last_sig = sig;
        board_invalid = 0;
    }

    /* 1. 地形: BG0 を組み立てて合成する。
       compose はダーティ矩形方式なので、キャラを gfx で上書きする
       この描画フローでは全タイルを強制的に描き直す必要がある
       (これをしないと2フレーム目以降、盤面が真っ黒になる)。 */
    fill_bg();
    fill_bg_facilities();
    tilemap_force_redraw();
    tilemap_compose_btf();

    /* 2. 施設は BG1 (透過タイル) に載せる。
       静的な絵なのでタイル化して問題ない。
       キャラクタは移動アニメでピクセル単位に動かす必要があるため、
       タイル化せず gfx で描く。 */

    /* 3. キャラクタ
       描画順: 奥 (グリッド y が小さい) から手前へ。
       同じマスにいる場合は手番プレイヤーを最後に描いて最前面に出す。 */
    for (i = 0; i < 4; i++) order[n++] = i;
    for (i = 0; i < n - 1; i++) {
        for (k = 0; k < n - 1 - i; k++) {
            const BoardMass *a = board_get_mass((u16)players_anim[order[k]].pos);
            const BoardMass *b =
                board_get_mass((u16)players_anim[order[k + 1]].pos);
            int ya = a ? (int)a->y : 0;
            int yb = b ? (int)b->y : 0;
            int swap = 0;
            if (ya > yb) {
                swap = 1;
            } else if (ya == yb) {
                /* 手番プレイヤーは後ろへ回して最前面にする */
                if (order[k] == cam_focus_pid) swap = 1;
            }
            if (swap) {
                int t = order[k];
                order[k] = order[k + 1];
                order[k + 1] = t;
            }
        }
    }

    for (i = 0; i < n; i++) {
        int pid = order[i];
        BoardPlayer *p = &players_anim[pid];
        int px, py, ox, oy, stack = 0;
        u8 col_p;

        switch (pid) {
        case 0:  col_p = TC_P_RED;    break;
        case 1:  col_p = TC_P_BLUE;   break;
        case 2:  col_p = TC_P_GREEN;  break;
        default: col_p = TC_P_YELLOW; break;
        }

        if (p->moving) {
            /* anim_x/y は世界座標。画面座標へ変換する */
            int vx0, vy0;
            view_origin(&vx0, &vy0);
            px = p->anim_x - vx0;
            py = p->anim_y - vy0;
        } else {
            if (!mass_screen_origin(p->pos, &ox, &oy)) continue;
            for (k = 0; k < pid; k++) {
                if (!players_anim[k].moving && players_anim[k].pos == p->pos)
                    stack++;
            }
            px = ox + VIEW_CELL_W / 2 - 6 + stack * 5;
            py = oy + VIEW_CELL_H - 6;
        }

        /* 盤面の外へはみ出す駒は描かない。
           右パネル・下段は差分描画なので、1px でもはみ出すと
           そちらが再描画されるまで残像になる (キャラ幅18 + 重なりズレ) */
        if (px < 12 || px > VIEW_FIELD_W - 12 ||
            py < 0 || py > VIEW_FIELD_H)
            continue;

        draw_character(px, py, col_p, pid == cam_focus_pid);
    }

    gfx_rect(0, 0, VIEW_FIELD_W, VIEW_FIELD_H, TC_PAPER);
    return 1;
}

void view_board_update(void) {
    int i;
    for (i = 0; i < 4; i++) {
        BoardPlayer *p = &players_anim[i];
        if (!p->moving) continue;

        p->anim_timer++;
        if (p->anim_timer >= 8) {
            p->anim_timer = 0;
            p->path_idx++;
            if (p->path_idx >= p->path_len - 1) {
                p->pos = p->target_pos;
                p->moving = 0;
                view_board_get_mass_pos(p->pos, &p->anim_x, &p->anim_y);
                /* 残り歩数があれば分岐待ちへ */
                settle_arrival(p);
            }
        }

        if (p->moving) {
            int curr_mass = p->path[p->path_idx];
            int next_mass = p->path[p->path_idx + 1];
            int x0, y0, x1, y1;
            view_board_get_mass_pos(curr_mass, &x0, &y0);
            view_board_get_mass_pos(next_mass, &x1, &y1);

            p->anim_x = lerp_int(x0, x1, p->anim_timer, 8);
            p->anim_y = lerp_int(y0, y1, p->anim_timer, 8);
        }
    }

    /* 手番プレイヤーの移動中はカメラが足元を追う。
       キャラは画面中央に固定され、地面のほうがスクロールする */
    {
        BoardPlayer *f = &players_anim[cam_focus_pid];
        if (f->moving) {
            cam_wx = f->anim_x;
            cam_wy = f->anim_y - (VIEW_CELL_H / 2 - 6);
        }
    }
}

/* 現在地から進める接続先を列挙する。
   prev >= 0 : 来た道 prev を除外する (移動途中)
   prev <  0 : 手番の開始時。来た道がないので「前進方向」= 自分より大きい
               マスIDの接続先に限定する。これをしないと、直線の途中でも
               常に前後どちらへ進むか聞かれてしまう。
   戻り値=候補数、out にマスIDを格納 */
static int collect_exits(int mass_id, int prev, int *out, int max) {
    const BoardMass *m = board_get_mass((u16)mass_id);
    int i;
    int n = 0;

    if (!m) return 0;

    for (i = 0; i < (int)m->connect_count && n < max; i++) {
        int to = (int)m->connect[i];
        if (m->connect[i] == BOARD_CONNECT_NONE) continue;
        if (prev >= 0) {
            if (to == prev) continue;      /* 来た道は戻らない */
        } else {
            if (to <= mass_id) continue;   /* 手番開始時は前進方向のみ */
        }
        out[n++] = to;
    }

    /* 前進方向がない (盤面の終端・逆走区間) 場合は全接続先から選ぶ */
    if (n == 0 && prev < 0) {
        for (i = 0; i < (int)m->connect_count && n < max; i++) {
            if (m->connect[i] == BOARD_CONNECT_NONE) continue;
            out[n++] = (int)m->connect[i];
        }
    }

    /* 行き止まり (来た道しかない) なら、引き返すことを許可する */
    if (n == 0 && prev >= 0 && m->connect_count > 0 && max > 0) {
        out[n++] = prev;
    }

    return n;
}

/* p->pos から p->rem_steps 歩ぶんの経路を組み立てる。
   分岐 (進行先が2つ以上) に当たったらそこで打ち切り、残り歩数を rem_steps に残す。
   最初の 1 歩を forced_first (>=0) で強制できる (分岐選択後の再開用)。 */
static void build_path(BoardPlayer *p, int forced_first) {
    int idx = 0;
    int current = p->pos;
    int prev = p->prev_mass;
    int exits[VIEW_BOARD_MAX_BRANCH];
    int n;
    const BoardMass *m;

    p->path[idx++] = current;

    while (p->rem_steps > 0 && idx < 32) {
        m = board_get_mass((u16)current);
        if (!m || m->connect_count == 0) break;   /* 行き止まり */

        /* 封鎖マスに乗り上げたら、残り歩数を捨てて強制停止 */
        if ((m->flags & BOARD_FLAG_BLOCKED) && idx > 1) {
            p->rem_steps = 0;
            break;
        }

        if (forced_first >= 0) {
            /* 分岐で選ばれた向きへ 1 歩 */
            n = 1;
            exits[0] = forced_first;
            forced_first = -1;
        } else {
            n = collect_exits(current, prev, exits, VIEW_BOARD_MAX_BRANCH);
            if (n == 0) break;
            /* 進行先が複数あるなら分岐。歩数を残して停止し、選択を待つ */
            if (n >= 2) break;
        }

        prev = current;
        current = exits[0];
        p->path[idx++] = current;
        p->rem_steps--;
    }

    p->prev_mass  = prev;
    p->path_len   = idx;
    p->path_idx   = 0;
    p->target_pos = current;
    p->moving     = (idx > 1) ? 1 : 0;
    p->anim_timer = 0;
    p->branching  = 0;

    view_board_get_mass_pos(p->pos, &p->anim_x, &p->anim_y);

    /* 1 歩も動けなかった場合は、この場で分岐判定まで済ませておく */
    if (!p->moving) {
        settle_arrival(p);
    }
}

void view_board_move_player(int pid, int steps) {
    BoardPlayer *p = &players_anim[pid];

    if (p->moving || p->branching) return;

    p->rem_steps = steps;
    p->prev_mass = -1;   /* 手番開始時は「来た道」なし */
    build_path(p, -1);
}

int view_board_is_branching(int pid) {
    return players_anim[pid].branching;
}

int view_board_branch_options(int pid, int *out, int max) {
    BoardPlayer *p = &players_anim[pid];
    int n = p->branch_count;
    int i;

    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = p->branch_opts[i];
    return n;
}

int view_board_remaining_steps(int pid) {
    return players_anim[pid].rem_steps;
}

int view_board_path_len(int pid) {
    return players_anim[pid].path_len;
}

int view_board_path_at(int pid, int idx) {
    BoardPlayer *p = &players_anim[pid];
    if (idx < 0 || idx >= p->path_len) return -1;
    return p->path[idx];
}

/* 移動が止まった時点で、分岐待ちに入るかどうかを確定させる */
static void settle_arrival(BoardPlayer *p) {
    p->branch_count = 0;
    p->branching = 0;

    if (p->rem_steps <= 0) return;

    p->branch_count = collect_exits(p->pos, p->prev_mass,
                                    p->branch_opts, VIEW_BOARD_MAX_BRANCH);
    if (p->branch_count >= 2) {
        p->branching = 1;
    } else {
        /* 分岐ではないのに止まった = 行き止まり。残り歩数は消化できない */
        p->rem_steps = 0;
        p->branch_count = 0;
    }
}

int view_board_choose_branch(int pid, int next_mass) {
    BoardPlayer *p = &players_anim[pid];
    int i;
    int ok = 0;

    if (!p->branching) return -1;

    for (i = 0; i < p->branch_count; i++) {
        if (p->branch_opts[i] == next_mass) { ok = 1; break; }
    }
    if (!ok) return -1;

    p->branching = 0;
    build_path(p, next_mass);
    return 0;
}

int view_board_is_player_moving(int pid) {
    return players_anim[pid].moving;
}

int view_board_get_player_pos(int pid) {
    return players_anim[pid].pos;
}

void view_board_set_player_pos(int pid, int mass_id) {
    players_anim[pid].pos = mass_id;
    players_anim[pid].target_pos = mass_id;
    players_anim[pid].moving = 0;
    /* ワープ扱いなので移動途中の状態は全て捨てる */
    players_anim[pid].rem_steps = 0;
    players_anim[pid].prev_mass = -1;
    players_anim[pid].branching = 0;
    players_anim[pid].branch_count = 0;
    view_board_get_mass_pos(mass_id, &players_anim[pid].anim_x, &players_anim[pid].anim_y);
}
