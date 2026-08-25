/* ======================================================================== */
/*  INV_DURABILITY.C — 耐久度管理                                            */
/*                                                                          */
/*  装備品の耐久度消耗・修復・破損チェックを提供する。                        */
/*  durability = 0xFF は無限耐久（消耗しない）。                              */
/*  durability = 0 は破損状態。                                               */
/* ======================================================================== */

#include "libos32inv.h"

extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  公開API: 耐久度消耗                                                     */
/* ====================================================================== */

int inv_wear(InvBag *bag, u8 equip_slot)
{
    if (!bag) return -1;
    if (equip_slot >= bag->equip_count) return -1;
    if (bag->equip[equip_slot].item_id == 0) return -1;

    /* 無限耐久 → 変化なし */
    if (bag->equip[equip_slot].durability == 0xFF) return 0xFF;

    /* 既に破損 → 0のまま */
    if (bag->equip[equip_slot].durability == 0) return 0;

    /* 消耗 */
    bag->equip[equip_slot].durability--;

    return (int)bag->equip[equip_slot].durability;
}

/* ====================================================================== */
/*  公開API: 耐久度回復                                                     */
/* ====================================================================== */

int inv_repair(InvBag *bag, u8 equip_slot)
{
    const InvItemDef *def;

    if (!bag) return -1;
    if (equip_slot >= bag->equip_count) return -1;
    if (bag->equip[equip_slot].item_id == 0) return -1;

    /* 無限耐久 → 変化なし */
    if (bag->equip[equip_slot].durability == 0xFF) return 0xFF;

    /* max_durability を取得して復元 */
    def = inv_get_def(bag->equip[equip_slot].item_id);
    if (!def) return -1;

    /* max_durability が 0xFF (無限) の場合もそのまま設定 */
    bag->equip[equip_slot].durability = def->max_durability;

    return (int)bag->equip[equip_slot].durability;
}

/* ====================================================================== */
/*  公開API: 破損チェック                                                   */
/* ====================================================================== */

int inv_is_broken(const InvBag *bag, u8 equip_slot)
{
    if (!bag) return 0;
    if (equip_slot >= bag->equip_count) return 0;
    if (bag->equip[equip_slot].item_id == 0) return 0;

    /* 無限耐久は破損しない */
    if (bag->equip[equip_slot].durability == 0xFF) return 0;

    return (bag->equip[equip_slot].durability == 0) ? 1 : 0;
}
