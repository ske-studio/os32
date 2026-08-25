/* ======================================================================== */
/*  RPG_STATUS.C — libos32rpg フィールド状態異常制御                       */
/* ======================================================================== */

#include "libos32rpg.h"
#include "libos32math.h"
#include <string.h>

/* 外部参照 (rpg_core.c で定義) */
typedef struct {
    u32  bit_flag;
    u8   prevents_action;
    u8   tick_kind;         /* 0=なし, 1=固定, 2=レベル比例, 3=最大HP% */
    i16  tick_value;
    u8   recovery_pct;
    u8   lethal;
} RpgStatusFieldDef;

extern const RpgStatusFieldDef *rpg_find_status_def(u32 bit);
extern void                     rpg_set_dead(RpgActor *a, u8 fled);

/* ====================================================================== */
/*  rpg_status_apply — 状態異常の付与                                      */
/* ====================================================================== */
void rpg_status_apply(RpgActor *a, u32 bit)
{
    if (a == (RpgActor *)0) return;
    a->status |= bit;
}

/* ====================================================================== */
/*  rpg_status_clear — 状態異常の解除                                      */
/* ====================================================================== */
void rpg_status_clear(RpgActor *a, u32 bit)
{
    if (a == (RpgActor *)0) return;
    a->status &= ~bit;
}

/* ====================================================================== */
/*  rpg_has_status — 状態異常の所持確認                                    */
/* ====================================================================== */
int rpg_has_status(const RpgActor *a, u32 bit)
{
    if (a == (const RpgActor *)0) return 0;
    return (a->status & bit) ? 1 : 0;
}

/* ====================================================================== */
/*  rpg_status_tick — 毎ターンの状態異常の更新 (ダメージ＋自然回復)        */
/* ====================================================================== */
int rpg_status_tick(RpgActor *a, RpgTickLog *out)
{
    int action_blocked = 0;
    u32 active_status;
    int i;

    if (a == (RpgActor *)0) return 0;

    if (out != (RpgTickLog *)0) {
        memset(out, 0, sizeof(RpgTickLog));
    }

    if (a->status == 0) {
        return 0;
    }

    active_status = a->status;

    /* 32ビット分の状態異常フラグを走査 */
    for (i = 0; i < 32; i++) {
        u32 bit = (1u << i);
        if (active_status & bit) {
            const RpgStatusFieldDef *def = rpg_find_status_def(bit);
            if (def == (const RpgStatusFieldDef *)0) {
                continue; /* 定義なし */
            }

            /* 自然回復判定 (0〜99の乱数を取得し、回復率未満なら回復) */
            if (def->recovery_pct > 0 && rng_range(0, 99) < (int)def->recovery_pct) {
                a->status &= ~bit;
                if (out != (RpgTickLog *)0) {
                    out->cleared |= bit;
                }
            } else {
                /* 回復しなかった場合 */
                
                /* 行動不能チェック */
                if (def->prevents_action) {
                    action_blocked = 1;
                    if (out != (RpgTickLog *)0) {
                        out->action_blocked = 1;
                    }
                }

                /* ダメージ計算 */
                if (def->tick_kind > 0) {
                    i16 dmg = 0;
                    
                    /* 呪い (bit=32) の場合は25%の確率でダメージ適用 */
                    if (bit == 32 && rng_range(0, 3) != 0) {
                        /* 75%の確率でダメージをパスする */
                    } else {
                        switch (def->tick_kind) {
                            case 1: /* 固定値 */
                                dmg = def->tick_value;
                                break;
                            case 2: /* レベル比例 */
                                dmg = (i16)a->level * def->tick_value;
                                break;
                            case 3: /* 最大HP比例 (%) */
                                dmg = a->max_hp * def->tick_value / 100;
                                break;
                            default:
                                break;
                        }
                    }

                    if (dmg > 0) {
                        if (out != (RpgTickLog *)0) {
                            out->tick_damage += dmg;
                        }
                        a->hp -= dmg;

                        /* 死亡判定 */
                        if (a->hp <= 0) {
                            if (def->lethal) {
                                a->hp = 0;
                                rpg_set_dead(a, 0); /* 毒などの通常フィールド死 */
                            } else {
                                a->hp = 1; /* 非致死（HP1で止まる） */
                            }
                        }
                    }
                }
            }
        }
    }

    return action_blocked;
}
