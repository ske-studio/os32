/* ======================================================================== */
/*  INV_BAG.C — インベントリ操作                                             */
/*                                                                          */
/*  バッグの初期化、アイテム追加・除去・検索を担当。                          */
/*  スタック可能アイテムは自動合算する。                                      */
/* ======================================================================== */

#include "libos32inv.h"

extern KernelAPI *kapi;
#define api kapi

/* memset は libc から提供 */
extern void *memset(void *, int, unsigned int);

/* 外部参照 (inv_core.c) */
extern InvItemDef *inv__get_masters(void);
extern int         inv__master_count(void);

/* ====================================================================== */
/*  公開API: バッグ初期化                                                   */
/* ====================================================================== */

void inv_bag_init(InvBag *bag, u8 max_slots, u8 equip_count)
{
    if (!bag) return;
    memset(bag, 0, sizeof(InvBag));
    if (max_slots > INV_MAX_SLOTS) max_slots = INV_MAX_SLOTS;
    if (equip_count > INV_EQUIP_SLOTS) equip_count = INV_EQUIP_SLOTS;
    bag->max_slots = max_slots;
    bag->equip_count = equip_count;
}

/* ====================================================================== */
/*  公開API: アイテム追加                                                   */
/* ====================================================================== */

int inv_add(InvBag *bag, u16 item_id, u8 count)
{
    const InvItemDef *def;
    int i;
    u8 remain;

    if (!bag || item_id == 0 || count == 0) return -1;

    def = inv_get_def(item_id);
    if (!def) return -1;

    remain = count;

    /* スタック可能な場合: 既存スロットに合算 */
    if (def->stackable) {
        for (i = 0; i < bag->max_slots && remain > 0; i++) {
            if (bag->slots[i].item_id == item_id) {
                u8 can_add;
                if (bag->slots[i].count >= 99) continue;
                can_add = 99 - bag->slots[i].count;
                if (can_add > remain) can_add = remain;
                bag->slots[i].count += can_add;
                remain -= can_add;
            }
        }
    }

    /* 残りを空きスロットに配置 */
    while (remain > 0) {
        int found = -1;
        u8 place;

        /* 空きスロット探索 */
        for (i = 0; i < bag->max_slots; i++) {
            if (bag->slots[i].item_id == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) return -1;  /* 満杯 */

        if (def->stackable) {
            place = (remain > 99) ? 99 : remain;
        } else {
            place = 1;
        }

        bag->slots[found].item_id    = item_id;
        bag->slots[found].count      = place;
        bag->slots[found].durability = 0xFF;  /* デフォルト: 無限耐久 */

        remain -= place;
    }

    return 0;
}

/* ====================================================================== */
/*  公開API: アイテム除去                                                   */
/* ====================================================================== */

int inv_remove(InvBag *bag, u8 slot, u8 count)
{
    if (!bag) return -1;
    if (slot >= bag->max_slots) return -1;
    if (bag->slots[slot].item_id == 0) return -1;
    if (count == 0) return -1;

    if (count >= bag->slots[slot].count) {
        /* 全除去 → スロットクリア */
        bag->slots[slot].item_id    = 0;
        bag->slots[slot].count      = 0;
        bag->slots[slot].durability = 0;
    } else {
        bag->slots[slot].count -= count;
    }

    return 0;
}

/* ====================================================================== */
/*  公開API: 所持数カウント                                                 */
/* ====================================================================== */

int inv_count_item(const InvBag *bag, u16 item_id)
{
    int i;
    int total = 0;

    if (!bag || item_id == 0) return 0;

    for (i = 0; i < bag->max_slots; i++) {
        if (bag->slots[i].item_id == item_id) {
            total += bag->slots[i].count;
        }
    }

    return total;
}

/* ====================================================================== */
/*  公開API: 空きスロット数                                                 */
/* ====================================================================== */

int inv_free_slots(const InvBag *bag)
{
    int i;
    int free_count = 0;

    if (!bag) return 0;

    for (i = 0; i < bag->max_slots; i++) {
        if (bag->slots[i].item_id == 0) {
            free_count++;
        }
    }

    return free_count;
}

/* ====================================================================== */
/*  公開API: 所持判定                                                       */
/* ====================================================================== */

int inv_has(const InvBag *bag, u16 item_id)
{
    int i;

    if (!bag || item_id == 0) return 0;

    for (i = 0; i < bag->max_slots; i++) {
        if (bag->slots[i].item_id == item_id) return 1;
    }

    return 0;
}

/* ====================================================================== */
/*  公開API: スロット内容取得                                               */
/* ====================================================================== */

const InvSlot *inv_get_slot(const InvBag *bag, u8 slot)
{
    if (!bag) return (const InvSlot *)0;
    if (slot >= bag->max_slots) return (const InvSlot *)0;
    return &bag->slots[slot];
}
