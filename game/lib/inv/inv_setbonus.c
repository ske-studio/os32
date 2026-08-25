/* ======================================================================== */
/*  INV_SETBONUS.C — セット装備ボーナス計算                                  */
/*                                                                          */
/*  set_bonus テーブルの定義に基づき、現在の装備からセットボーナスを計算する。 */
/*  同一set_idの装備が piece_count 以上揃っていればボーナスが発動する。       */
/* ======================================================================== */

#include "libos32inv.h"

extern KernelAPI *kapi;
#define api kapi

/* 外部参照 (inv_core.c) */
extern InvSetBonus *inv__get_set_bonuses(void);
extern int          inv__set_bonus_count(void);

/* ====================================================================== */
/*  内部ヘルパー: 装備中のセットID別ピース数カウント                        */
/* ====================================================================== */

static int count_set_pieces(const InvBag *bag, u8 set_id)
{
    int i;
    int count = 0;

    for (i = 0; i < bag->equip_count; i++) {
        const InvItemDef *def;
        if (bag->equip[i].item_id == 0) continue;

        def = inv_get_def(bag->equip[i].item_id);
        if (!def) continue;

        if (def->set_id == set_id) count++;
    }

    return count;
}

/* ====================================================================== */
/*  公開API: セットボーナス合計                                             */
/* ====================================================================== */

i16 inv_set_bonus(const InvBag *bag, u8 stat_type)
{
    InvSetBonus *bonuses;
    int bonus_count;
    int i;
    i16 total = 0;

    if (!bag) return 0;

    bonuses = inv__get_set_bonuses();
    bonus_count = inv__set_bonus_count();

    for (i = 0; i < bonus_count; i++) {
        int pieces;

        /* stat_type が一致しなければスキップ */
        if (bonuses[i].stat_type != stat_type) continue;

        /* 装備中のピース数チェック */
        pieces = count_set_pieces(bag, bonuses[i].set_id);
        if (pieces >= bonuses[i].piece_count) {
            total += bonuses[i].bonus;
        }
    }

    return total;
}

/* ====================================================================== */
/*  公開API: 装備ボーナス + セットボーナスの統合値                           */
/* ====================================================================== */

i16 inv_total_bonus(const InvBag *bag, u8 stat_type)
{
    i16 equip_b;
    i16 set_b;

    if (!bag) return 0;

    equip_b = inv_equip_bonus(bag, stat_type);
    set_b   = inv_set_bonus(bag, stat_type);

    return equip_b + set_b;
}
