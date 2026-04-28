/* ======================================================================== */
/*  BOARD_AREA.C — 区画 (ステージ) 管理・動的マス操作                      */
/* ======================================================================== */

#include "libos32board.h"
#include <string.h>

/* 外部参照 (board_core.c で定義) */
extern BoardMass *board_get_masses(void);
extern int        board_get_mass_count(void);
extern void       board_set_mass_count(int n);
extern BoardArea *board_get_areas(void);
extern int        board_get_area_cnt(void);
extern int        board_find_index(u16 id);

/* ====================================================================== */
/*  区画管理                                                                */
/* ====================================================================== */

int board_area_count(void)
{
    return board_get_area_cnt();
}

int board_is_area_unlocked(u8 area_id)
{
    BoardArea *areas = board_get_areas();
    int count = board_get_area_cnt();
    int i;

    for (i = 0; i < count; i++) {
        if (areas[i].id == area_id) {
            return (int)areas[i].unlocked;
        }
    }
    return 0;
}

int board_unlock_area(u8 area_id)
{
    BoardArea *areas = board_get_areas();
    int count = board_get_area_cnt();
    int i;

    for (i = 0; i < count; i++) {
        if (areas[i].id == area_id) {
            if (areas[i].unlocked) return 0;  /* 既に解放済み */
            areas[i].unlocked = 1;
            return 1;  /* 新規解放 */
        }
    }
    return -1;  /* 区画が見つからない */
}

void board_lock_area(u8 area_id)
{
    BoardArea *areas = board_get_areas();
    int count = board_get_area_cnt();
    int i;

    for (i = 0; i < count; i++) {
        if (areas[i].id == area_id) {
            areas[i].unlocked = 0;
            return;
        }
    }
}

/* ====================================================================== */
/*  動的マス操作                                                            */
/* ====================================================================== */

int board_add_mass(const BoardMass *mass)
{
    BoardMass *masses;
    int count;
    int j;
    BoardMass *m;

    if (mass == (const BoardMass *)0) return -1;

    count = board_get_mass_count();
    if (count >= BOARD_MAX_MASSES) return -1;

    masses = board_get_masses();
    m = &masses[count];
    memcpy(m, mass, sizeof(BoardMass));

    /* 未使用の接続先をセンチネルで初期化 */
    for (j = m->connect_count; j < BOARD_MAX_CONNECT; j++) {
        m->connect[j] = BOARD_CONNECT_NONE;
    }
    if (!(m->flags & BOARD_FLAG_TRAP)) {
        m->trap_owner = 0xFF;
    }

    board_set_mass_count(count + 1);
    return (int)m->id;
}

int board_add_connection(u16 from, u16 to)
{
    int idx = board_find_index(from);
    BoardMass *m;

    if (idx < 0) return -1;

    m = &board_get_masses()[idx];
    if (m->connect_count >= BOARD_MAX_CONNECT) return -1;

    /* 重複チェック */
    {
        int i;
        for (i = 0; i < (int)m->connect_count; i++) {
            if (m->connect[i] == to) return 0;  /* 既に接続済み */
        }
    }

    m->connect[m->connect_count] = to;
    m->connect_count++;
    return 1;  /* 新規追加 */
}

void board_remove_connection(u16 from, u16 to)
{
    int idx = board_find_index(from);
    BoardMass *m;
    int i;

    if (idx < 0) return;

    m = &board_get_masses()[idx];

    for (i = 0; i < (int)m->connect_count; i++) {
        if (m->connect[i] == to) {
            /* 後続の接続を詰める */
            int j;
            for (j = i; j < (int)m->connect_count - 1; j++) {
                m->connect[j] = m->connect[j + 1];
            }
            m->connect_count--;
            m->connect[m->connect_count] = BOARD_CONNECT_NONE;
            return;
        }
    }
}
