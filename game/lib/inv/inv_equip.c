/* ======================================================================== */
/*  INV_EQUIP.C — 装備操作・ボーナス計算                                     */
/*                                                                          */
/*  装備変更時に旧装備をインベントリに自動返却。                              */
/*  ポリシーコールバックによる装備制限チェックをサポート。                    */
/* ======================================================================== */

#include "libos32inv.h"

extern KernelAPI *kapi;
#define api kapi

/* 外部参照 (inv_core.c) */
extern inv_equip_check_fn inv__get_equip_policy(void);

/* ====================================================================== */
/*  公開API: 装備変更                                                       */
/* ====================================================================== */

int inv_equip(InvBag *bag, u16 equip_id, u8 slot_type)
{
    const InvItemDef *def;
    inv_equip_check_fn policy;

    if (!bag) return -1;
    if (slot_type >= bag->equip_count) return -1;

    def = inv_get_def(equip_id);
    if (!def) return -1;

    /* 装備品でないものは装備できない */
    if (!inv_is_equipment(equip_id)) return -1;

    /* ポリシーチェック */
    policy = inv__get_equip_policy();
    if (policy) {
        if (!policy(equip_id, 0, 0)) return -1;
    }

    /* 旧装備がある場合、インベントリに戻す */
    if (bag->equip[slot_type].item_id != 0) {
        int rc = inv_add(bag, bag->equip[slot_type].item_id, 1);
        if (rc < 0) return -2;  /* インベントリ満杯 */
    }

    /* インベントリから装備品を除去 (所持している場合) */
    {
        int i;
        for (i = 0; i < bag->max_slots; i++) {
            if (bag->slots[i].item_id == equip_id) {
                inv_remove(bag, (u8)i, 1);
                break;
            }
        }
    }

    /* 装備スロットに設定 */
    bag->equip[slot_type].item_id    = equip_id;
    bag->equip[slot_type].count      = 1;
    bag->equip[slot_type].durability = def->max_durability;

    return 0;
}

/* ====================================================================== */
/*  公開API: 装備解除                                                       */
/* ====================================================================== */

int inv_unequip(InvBag *bag, u8 slot_type)
{
    int rc;

    if (!bag) return -1;
    if (slot_type >= bag->equip_count) return -1;
    if (bag->equip[slot_type].item_id == 0) return 0;  /* 既に空 */

    /* インベントリに戻す */
    rc = inv_add(bag, bag->equip[slot_type].item_id, 1);
    if (rc < 0) return -2;  /* インベントリ満杯 */

    /* スロットクリア */
    bag->equip[slot_type].item_id    = 0;
    bag->equip[slot_type].count      = 0;
    bag->equip[slot_type].durability = 0;

    return 0;
}

/* ====================================================================== */
/*  公開API: 装備ボーナス合計                                               */
/* ====================================================================== */

i16 inv_equip_bonus(const InvBag *bag, u8 stat_type)
{
    int i;
    i16 total = 0;

    if (!bag) return 0;

    for (i = 0; i < bag->equip_count; i++) {
        const InvItemDef *def;
        if (bag->equip[i].item_id == 0) continue;

        def = inv_get_def(bag->equip[i].item_id);
        if (!def) continue;

        if (def->stat_type == stat_type) {
            total += def->stat_bonus;
        }
    }

    return total;
}
