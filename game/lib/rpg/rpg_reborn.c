/* ======================================================================== */
/*  RPG_REBORN.C — libos32rpg 死亡・リボーン判定                          */
/* ======================================================================== */

#include "libos32rpg.h"
#include "libos32math.h"

/* 外部参照 (rpg_core.c で定義) */
typedef struct {
    u8   rank_bucket;
    u8   min_turns;
    u8   max_turns;
} RpgRebornDef;

extern const RpgRebornDef *rpg_find_reborn_def(u8 rank_bucket);

/* ====================================================================== */
/*  rpg_set_dead — アクターを死亡状態にする                                */
/* ====================================================================== */
void rpg_set_dead(RpgActor *a, u8 fled)
{
    if (a == (RpgActor *)0) return;

    a->hp = 0;
    a->dead_turns = 1; /* 死亡1ターン目 */
    a->fled = fled;
    a->status = 0;     /* 死亡時はフィールド状態異常を全解除 */
}

/* ====================================================================== */
/*  rpg_is_dead — 死亡判定                                                 */
/* ====================================================================== */
int rpg_is_dead(const RpgActor *a)
{
    if (a == (const RpgActor *)0) return 0;
    return (a->dead_turns > 0) ? 1 : 0;
}

/* ====================================================================== */
/*  rpg_reborn_check — 復活チェック                                         */
/* ====================================================================== */
int rpg_reborn_check(RpgActor *a, int rank, int num_players)
{
    u8 min_t = 2;
    u8 max_t = 4;
    const RpgRebornDef *def;

    if (a == (RpgActor *)0) return 0;
    if (!rpg_is_dead(a)) return 0;

    /* 順位に基づき復活規定テーブルを引く */
    def = rpg_find_reborn_def((u8)rank);
    if (def != (const RpgRebornDef *)0) {
        min_t = def->min_turns;
        max_t = def->max_turns;
    }

    /* 1. 最大ターン数以上経過していれば確定復活 */
    if (a->dead_turns >= max_t) {
        goto do_reborn;
    }

    /* 2. 最小ターン数未満なら復活しない */
    if (a->dead_turns < min_t) {
        goto do_skip;
    }

    /* 3. 最小〜最大の間の確率復活ランプ
     * 確率 = (dead_turns - min) * (256 / (max - min))
     */
    if (max_t > min_t) {
        int diff = (int)(max_t - min_t);
        int step = 256 / diff;
        int threshold = (int)(a->dead_turns - min_t) * step;

        if (rng_range(0, 255) < threshold) {
            goto do_reborn;
        }
    } else {
        /* min >= max の場合は即復活 */
        goto do_reborn;
    }

do_skip:
    a->dead_turns++;
    return 0; /* 復活ならず */

do_reborn:
    a->hp = a->max_hp; /* HP全回復 */
    a->dead_turns = 0; /* 生存状態へ */
    a->fled = 0;
    return 1; /* 復活成功 */
}
