/* ======================================================================== */
/*  RPG_QUERY.C — libos32rpg クエリ関数および BtlUnit ブリッジ            */
/* ======================================================================== */

#include "libos32rpg.h"

/* 外部参照 (rpg_reborn.c で定義) */
extern void rpg_set_dead(RpgActor *a, u8 fled);

/* ====================================================================== */
/*  rpg_total_power — パラメータの総合力（ステータス合計値）を取得         */
/* ====================================================================== */
int rpg_total_power(const RpgActor *a)
{
    if (a == (const RpgActor *)0) return 0;
    return (int)(a->atk + a->def + a->spd + a->mag);
}

/* ====================================================================== */
/*  rpg_rank — スコア配列における対象要素の順位（1〜N）を算出            */
/* ====================================================================== */
int rpg_rank(const u32 *scores, int n, int idx)
{
    int rank = 1;
    int i;
    u32 my_score;

    if (scores == (const u32 *)0 || idx < 0 || idx >= n) {
        return 1;
    }

    my_score = scores[idx];

    for (i = 0; i < n; i++) {
        if (scores[i] > my_score) {
            rank++;
        } else if (scores[i] == my_score && i < idx) {
            /* 同点時は配列インデックスの若いほうを上位とする */
            rank++;
        }
    }

    return rank;
}

/* ====================================================================== */
/*  rpg_to_btl_unit — 永続アクターから戦闘用BtlUnitへのパラメータ転送      */
/* ====================================================================== */
void rpg_to_btl_unit(const RpgActor *a, BtlUnit *u)
{
    if (a == (const RpgActor *)0 || u == (BtlUnit *)0) return;

    u->hp         = a->hp;
    u->max_hp     = a->max_hp;
    u->atk        = a->atk;
    u->def        = a->def;
    u->spd        = a->spd;
    u->mag        = a->mag;
    u->status     = a->status;
    u->class_id   = a->class_id;

    /* 初期バフ・ためカウント・属性等のクリア */
    u->tame_count = 0;
    u->elements   = 0;
    u->modifier_count = 0;
    u->_pad       = 0;
}

/* ====================================================================== */
/*  rpg_from_btl_unit — 戦闘用BtlUnitから永続アクターへのパラメータ反映     */
/* ====================================================================== */
void rpg_from_btl_unit(RpgActor *a, const BtlUnit *u)
{
    if (a == (RpgActor *)0 || u == (const BtlUnit *)0) return;

    a->hp     = u->hp;
    a->status = u->status;

    /* HPが0以下になった場合は死亡状態に移行する */
    if (a->hp <= 0) {
        rpg_set_dead(a, 0);
    }
}
