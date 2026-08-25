/* ======================================================================== */
/*  BOARD_QUERY.C — マス情報取得・フラグ操作                                */
/* ======================================================================== */

#include "libos32board.h"

/* 外部参照 (board_core.c で定義) */
extern BoardMass *board_get_masses(void);
extern int        board_get_mass_count(void);
extern int        board_find_index(u16 id);

/* ====================================================================== */
/*  マス情報取得                                                            */
/* ====================================================================== */

int board_mass_count(void)
{
    return board_get_mass_count();
}

const BoardMass *board_get_mass(u16 id)
{
    int idx = board_find_index(id);
    if (idx < 0) return (const BoardMass *)0;
    return &board_get_masses()[idx];
}

const BoardMass *board_get_mass_at(int index)
{
    if (index < 0 || index >= board_get_mass_count())
        return (const BoardMass *)0;
    return &board_get_masses()[index];
}

u8 board_get_type(u16 id)
{
    int idx = board_find_index(id);
    if (idx < 0) return 0;
    return board_get_masses()[idx].type;
}

int board_has_branch(u16 id)
{
    int idx = board_find_index(id);
    if (idx < 0) return 0;
    return (board_get_masses()[idx].connect_count >= 2) ? 1 : 0;
}

int board_get_connections(u16 id, u16 *out, int max)
{
    int idx, count, i;
    const BoardMass *m;

    idx = board_find_index(id);
    if (idx < 0 || out == (u16 *)0 || max <= 0) return 0;

    m = &board_get_masses()[idx];
    count = (int)m->connect_count;
    if (count > max) count = max;

    for (i = 0; i < count; i++) {
        out[i] = m->connect[i];
    }
    return count;
}

int board_find_by_type(u8 type, u16 *out, int max)
{
    BoardMass *masses = board_get_masses();
    int total = board_get_mass_count();
    int found = 0;
    int i;

    if (out == (u16 *)0 || max <= 0) return 0;

    for (i = 0; i < total && found < max; i++) {
        if (masses[i].type == type) {
            out[found] = masses[i].id;
            found++;
        }
    }
    return found;
}

int board_check_colocated(u16 mass_id, const u16 *positions,
                           int count, u8 *out_indices, int max)
{
    int found = 0;
    int i;

    if (positions == (const u16 *)0 || out_indices == (u8 *)0 ||
        count <= 0 || max <= 0) {
        return 0;
    }

    for (i = 0; i < count && found < max; i++) {
        if (positions[i] == mass_id) {
            out_indices[found] = (u8)i;
            found++;
        }
    }
    return found;
}

/* ====================================================================== */
/*  フラグ操作                                                              */
/* ====================================================================== */

void board_set_flag(u16 mass_id, u8 flag)
{
    int idx = board_find_index(mass_id);
    if (idx >= 0) {
        board_get_masses()[idx].flags |= flag;
    }
}

void board_clear_flag(u16 mass_id, u8 flag)
{
    int idx = board_find_index(mass_id);
    if (idx >= 0) {
        board_get_masses()[idx].flags &= (u8)~flag;
    }
}

int board_has_flag(u16 mass_id, u8 flag)
{
    int idx = board_find_index(mass_id);
    if (idx < 0) return 0;
    return (board_get_masses()[idx].flags & flag) ? 1 : 0;
}

/* ====================================================================== */
/*  罠管理                                                                  */
/* ====================================================================== */

void board_set_trap(u16 mass_id, u8 owner_id)
{
    int idx = board_find_index(mass_id);
    if (idx >= 0) {
        board_get_masses()[idx].flags |= BOARD_FLAG_TRAP;
        board_get_masses()[idx].trap_owner = owner_id;
    }
}

void board_clear_trap(u16 mass_id)
{
    int idx = board_find_index(mass_id);
    if (idx >= 0) {
        board_get_masses()[idx].flags &= (u8)~BOARD_FLAG_TRAP;
        board_get_masses()[idx].trap_owner = 0xFF;
    }
}

u8 board_get_trap_owner(u16 mass_id)
{
    int idx = board_find_index(mass_id);
    if (idx < 0) return 0xFF;
    return board_get_masses()[idx].trap_owner;
}
