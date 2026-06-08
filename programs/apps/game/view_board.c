#include "view_board.h"
#include "libos32board.h"
#include "libos32gfx.h"
#include "libos32math.h"
#include "libos32econ.h"
#include <stdio.h>

static KernelAPI *api;
static BoardPlayer players_anim[4];

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
    }
}

int view_board_load(const char *db_path) {
    int ret = board_init(db_path);
    if (ret == 0) {
        int i;
        for (i = 0; i < 4; i++) {
            view_board_get_mass_pos(players_anim[i].pos, &players_anim[i].anim_x, &players_anim[i].anim_y);
        }
    }
    return ret;
}

void view_board_get_mass_pos(int mass_id, int *x, int *y) {
    const BoardMass *m = board_get_mass((u16)mass_id);
    if (m) {
        /* グリッド: xは0~19なので * 26 + 45 => 45~539 */
        /* yは0~8なので * 36 + 40 => 40~328 */
        *x = (int)(m->x * 26 + 45);
        *y = (int)(m->y * 36 + 40);
    } else {
        *x = 0;
        *y = 0;
    }
}

void view_board_draw(void) {
    int i, j;
    int count = board_mass_count();
    int x0, y0, x1, y1;
    int x, y;
    u8 color;

    /* 1. 接続線を描画 */
    for (i = 0; i < count; i++) {
        const BoardMass *m = board_get_mass((u16)i);
        if (!m) continue;
        
        view_board_get_mass_pos(i, &x0, &y0);

        for (j = 0; j < m->connect_count; j++) {
            u16 to_id = m->connect[j];
            if (to_id != BOARD_CONNECT_NONE && to_id < BOARD_MAX_MASSES) {
                if (to_id > (u16)i) {
                    view_board_get_mass_pos(to_id, &x1, &y1);
                    gfx_line(x0, y0, x1, y1, 7); /* グレー */
                }
            }
        }
    }

    /* 2. マスを描画 */
    for (i = 0; i < count; i++) {
        const BoardMass *m = board_get_mass((u16)i);
        if (!m) continue;

        view_board_get_mass_pos(i, &x, &y);

        color = 8;
        switch (m->type) {
            case MASS_EMPTY:        color = 7;  break; /* 白 */
            case MASS_VILLAGE: {
                const EconEstate *estate = econ_estate_get((u16)i);
                if (estate && estate->owner != ECON_OWNER_NONE) {
                    switch (estate->owner) {
                        case 0: color = 10; break; /* 赤 */
                        case 1: color = 9;  break; /* 青 */
                        case 2: color = 12; break; /* 緑 */
                        case 3: color = 14; break; /* 黄 */
                        default: color = 12; break;
                    }
                } else {
                    color = 8; /* 所有者なし: グレー */
                }
                break;
            }
            case MASS_TREASURE:     color = 14; break; /* 明るい黄 */
            case MASS_EQUIP_SHOP:   color = 9;  break; /* 明るい青 */
            case MASS_ITEM_SHOP:    color = 9;  break;
            case MASS_MAGIC_SHOP:   color = 9;  break;
            case MASS_CHURCH:       color = 11; break; /* 明るい紫 */
            case MASS_CIRCLE:       color = 13; break; /* 水色 */
            case MASS_GATE:         color = 10; break; /* 明るい赤 */
            case MASS_CASTLE:       color = 15; break; /* 高輝度白 */
            case MASS_MAGIC_CHEST:  color = 6;  break; /* 暗い黄 */
            default:                color = 8;  break;
        }

        gfx_fill_circle(x, y, 6, color);
        gfx_circle(x, y, 6, 15); /* 白い外枠 */

        /* 村マスのレベル描画 */
        if (m->type == MASS_VILLAGE) {
            const EconEstate *estate = econ_estate_get((u16)i);
            if (estate) {
                char lvl_buf[8];
                sprintf(lvl_buf, "%d", (int)estate->level);
                kcg_draw_utf8(x - 3, y - 4, lvl_buf, 15, 0); /* マスの中心にレベルを描画 */
            }
        }
    }

    /* 3. プレイヤーを描画 */
    for (i = 0; i < 4; i++) {
        BoardPlayer *p = &players_anim[i];
        u8 p_color = 10;
        switch (i) {
            case 0: p_color = 10; break; /* 赤 */
            case 1: p_color = 9;  break; /* 青 */
            case 2: p_color = 12; break; /* 緑 */
            case 3: p_color = 14; break; /* 黄 */
        }
        gfx_fill_circle(p->anim_x, p->anim_y, 4, p_color);
        gfx_circle(p->anim_x, p->anim_y, 4, 15);      /* 白い外枠 */
    }
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
}

void view_board_move_player(int pid, int steps) {
    BoardPlayer *p = &players_anim[pid];
    int current;
    int rem;
    int idx = 0;
    const BoardMass *m;
    u16 next_pos;
    u16 prev_pos;
    int i;

    if (p->moving) return;

    current = p->pos;
    rem = steps;

    p->path[idx++] = current;

    while (rem > 0 && idx < 32) {
        m = board_get_mass((u16)current);
        next_pos = BOARD_CONNECT_NONE;

        if (!m || m->connect_count == 0) {
            break; /* 行き止まり */
        }

        /* 封鎖マスに到達したら停止 */
        if (m->flags & BOARD_FLAG_BLOCKED) {
            if (idx > 1) {
                break;
            }
        }

        /* 分岐判定：
           2歩目以降(idx >= 2)の場合：接続数が3以上なら分岐（そこで停止）。
           起点(idx == 1)の場合は出発点なので分岐判定を行わない。 */
        if (idx >= 2) {
            if (m->connect_count >= 3) {
                break;
            }
        }

        /* 次に進むマスを決定する */
        if (idx == 1) {
            /* 1歩目の場合、進行可能な接続先の中から、IDが現在地より大きい方向（順方向）を優先する */
            next_pos = BOARD_CONNECT_NONE;
            for (i = 0; i < (int)m->connect_count; i++) {
                if (m->connect[i] != BOARD_CONNECT_NONE && m->connect[i] > current) {
                    next_pos = m->connect[i];
                    break;
                }
            }
            /* もし順方向の接続先がなければ、最初の接続先（逆方向など）を選択する */
            if (next_pos == BOARD_CONNECT_NONE && m->connect_count > 0) {
                next_pos = m->connect[0];
            }
        } else {
            /* 来た道（p->path[idx - 2]）以外の接続先を探す */
            prev_pos = (u16)p->path[idx - 2];
            for (i = 0; i < (int)m->connect_count; i++) {
                if (m->connect[i] != BOARD_CONNECT_NONE && m->connect[i] != prev_pos) {
                    next_pos = m->connect[i];
                    break;
                }
            }
        }

        if (next_pos == BOARD_CONNECT_NONE) {
            break; /* 次に進む場所がない */
        }

        /* 1歩進む */
        current = next_pos;
        p->path[idx++] = current;
        rem--;
    }

    p->path_len = idx;
    p->path_idx = 0;
    p->target_pos = current;
    p->moving = (idx > 1) ? 1 : 0;
    p->anim_timer = 0;

    view_board_get_mass_pos(p->pos, &p->anim_x, &p->anim_y);
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
    view_board_get_mass_pos(mass_id, &players_anim[pid].anim_x, &players_anim[pid].anim_y);
}
