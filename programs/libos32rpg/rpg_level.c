/* ======================================================================== */
/*  RPG_LEVEL.C — libos32rpg 経験値・レベルアップ・ステータス配分           */
/* ======================================================================== */

#include "libos32rpg.h"
#include <string.h>

/* 外部参照 (rpg_core.c で定義) */
typedef struct {
    u8   class_id;
    i16  atk, def, spd, mag;
    i16  hp;
    u8   free_points;
} RpgLevelGrowth;

extern const RpgLevelGrowth *rpg_find_growth(u8 class_id);
extern RpgLevelGrowth        g_growths[];
extern int                   g_growth_count;

static rpg_alloc_policy_fn   g_alloc_policy = (rpg_alloc_policy_fn)0;

/* ====================================================================== */
/*  rpg_exp_for_level — レベルに対する累計必要経験値を算出                */
/* ====================================================================== */
u32 rpg_exp_for_level(u8 level)
{
    /* 既定式: total = lv * lv * 10 */
    return (u32)level * (u32)level * 10;
}

/* ====================================================================== */
/*  rpg_level_for_exp — 経験値から到達レベルを算出                        */
/* ====================================================================== */
u8 rpg_level_for_exp(u32 exp)
{
    u8 level = 1;

    /* レベル上限 99 */
    while (level < 99 && exp >= rpg_exp_for_level(level + 1)) {
        level++;
    }
    return level;
}

/* ====================================================================== */
/*  rpg_exp_to_next — 次のレベルまでの必要経験値を取得                    */
/* ====================================================================== */
u32 rpg_exp_to_next(const RpgActor *a)
{
    u32 next_exp;

    if (a == (const RpgActor *)0) return 0;
    if (a->level >= 99) return 0;

    next_exp = rpg_exp_for_level(a->level + 1);
    return (next_exp > a->exp) ? (next_exp - a->exp) : 0;
}

/* ====================================================================== */
/*  rpg_add_exp — 経験値を獲得しレベルアップ処理を行う                  */
/* ====================================================================== */
int rpg_add_exp(RpgActor *a, u32 amount, RpgLevelResult *out)
{
    u8 new_lv;
    u8 diff = 0;

    if (a == (RpgActor *)0) return 0;

    a->exp += amount;
    new_lv = rpg_level_for_exp(a->exp);

    if (new_lv > a->level) {
        diff = new_lv - a->level;
    }

    if (diff > 0) {
        int i, j;
        int idx = -1;

        /* 職業に対応する成長データを検索 */
        for (j = 0; j < g_growth_count; j++) {
            if (g_growths[j].class_id == a->class_id) {
                idx = j;
                break;
            }
        }

        /* レベルアップごとのステータス加算 */
        for (i = 0; i < (int)diff; i++) {
            if (idx >= 0) {
                a->atk += g_growths[idx].atk;
                a->def += g_growths[idx].def;
                a->spd += g_growths[idx].spd;
                a->mag += g_growths[idx].mag;
                a->max_hp += g_growths[idx].hp;
                a->pending_points += g_growths[idx].free_points;
            } else {
                /* デフォルト成長 */
                a->atk += 2;
                a->def += 2;
                a->spd += 2;
                a->mag += 2;
                a->max_hp += 10;
                a->pending_points += 3;
            }

            /* ツクヨミ (class_id = 4) のランダム自動成長ボーナス (+2) */
            if (a->class_id == 4) {
                switch (rng_range(0, 3)) {
                    case 0: a->atk += 2; break;
                    case 1: a->def += 2; break;
                    case 2: a->spd += 2; break;
                    case 3: a->mag += 2; break;
                }
            }
        }

        a->level = new_lv;
        a->hp = a->max_hp; /* HP全回復 */
    }

    if (out != (RpgLevelResult *)0) {
        out->levels_gained = diff;
        out->free_points = a->pending_points;
    }

    return (diff > 0) ? 1 : 0;
}

/* ====================================================================== */
/*  rpg_alloc_point — 未配分ポイントをステータスに割り当てる             */
/* ====================================================================== */
int rpg_alloc_point(RpgActor *a, u8 stat)
{
    if (a == (RpgActor *)0 || a->pending_points == 0) {
        return -1;
    }

    switch (stat) {
        case BTL_STAT_ATK:
            a->atk++;
            break;
        case BTL_STAT_DEF:
            a->def++;
            break;
        case BTL_STAT_SPD:
            a->spd++;
            break;
        case BTL_STAT_MAG:
            a->mag++;
            break;
        default:
            return -1; /* 無効なステータスID */
    }

    a->pending_points--;
    return 0;
}

/* ====================================================================== */
/*  rpg_set_alloc_policy — CPU用自動配分ポリシーを登録                  */
/* ====================================================================== */
void rpg_set_alloc_policy(rpg_alloc_policy_fn fn)
{
    g_alloc_policy = fn;
}

/* ====================================================================== */
/*  rpg_auto_alloc — 登録ポリシーによる自動ポイント配分                 */
/* ====================================================================== */
void rpg_auto_alloc(RpgActor *a)
{
    if (a == (RpgActor *)0 || a->pending_points == 0) return;

    if (g_alloc_policy != (rpg_alloc_policy_fn)0) {
        g_alloc_policy(a, a->pending_points);
    }
}
