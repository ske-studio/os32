/* ======================================================================== */
/*  INV_CRAFT.C — 合成/クラフト実装                                          */
/*                                                                          */
/*  recipes テーブルのレシピに基づき、素材の消費と完成品の生成を行う。         */
/* ======================================================================== */

#include "libos32inv.h"

extern KernelAPI *kapi;
#define api kapi

/* 外部参照 (inv_core.c) */
extern InvRecipe *inv__get_recipes(void);
extern int        inv__recipe_count(void);

/* ====================================================================== */
/*  内部ヘルパー: レシピ検索                                                */
/* ====================================================================== */

static const InvRecipe *find_recipe(u16 recipe_id)
{
    InvRecipe *recipes = inv__get_recipes();
    int count = inv__recipe_count();
    int i;

    for (i = 0; i < count; i++) {
        if (recipes[i].id == recipe_id) return &recipes[i];
    }
    return (const InvRecipe *)0;
}

/* ====================================================================== */
/*  内部ヘルパー: 素材のスロットインデックス検索                            */
/*  item_id のアイテムが入っている最初のスロットを返す                      */
/* ====================================================================== */

static int find_item_slot(const InvBag *bag, u16 item_id)
{
    int i;
    for (i = 0; i < bag->max_slots; i++) {
        if (bag->slots[i].item_id == item_id) return i;
    }
    return -1;
}

/* ====================================================================== */
/*  公開API: 合成可否チェック                                               */
/* ====================================================================== */

int inv_can_craft(const InvBag *bag, u16 recipe_id)
{
    const InvRecipe *r;

    if (!bag) return 0;

    r = find_recipe(recipe_id);
    if (!r) return 0;

    /* 素材Aのチェック */
    if (r->mat_a != 0) {
        if (inv_count_item(bag, r->mat_a) < r->mat_a_count) return 0;
    }

    /* 素材Bのチェック */
    if (r->mat_b != 0) {
        if (inv_count_item(bag, r->mat_b) < r->mat_b_count) return 0;
    }

    return 1;
}

/* ====================================================================== */
/*  公開API: 合成実行                                                       */
/* ====================================================================== */

int inv_craft(InvBag *bag, u16 recipe_id)
{
    const InvRecipe *r;

    if (!bag) return -3;

    r = find_recipe(recipe_id);
    if (!r) return -3;

    /* 素材チェック */
    if (!inv_can_craft(bag, recipe_id)) return -1;

    /* 素材A消費 */
    if (r->mat_a != 0) {
        int remaining = r->mat_a_count;
        while (remaining > 0) {
            int slot = find_item_slot(bag, r->mat_a);
            if (slot < 0) return -1;

            if (bag->slots[slot].count <= remaining) {
                remaining -= bag->slots[slot].count;
                inv_remove(bag, (u8)slot, bag->slots[slot].count);
            } else {
                inv_remove(bag, (u8)slot, (u8)remaining);
                remaining = 0;
            }
        }
    }

    /* 素材B消費 */
    if (r->mat_b != 0) {
        int remaining = r->mat_b_count;
        while (remaining > 0) {
            int slot = find_item_slot(bag, r->mat_b);
            if (slot < 0) return -1;

            if (bag->slots[slot].count <= remaining) {
                remaining -= bag->slots[slot].count;
                inv_remove(bag, (u8)slot, bag->slots[slot].count);
            } else {
                inv_remove(bag, (u8)slot, (u8)remaining);
                remaining = 0;
            }
        }
    }

    /* 完成品追加 */
    {
        int rc = inv_add(bag, r->result_id, r->result_count);
        if (rc < 0) return -2;
    }

    return 0;
}

/* ====================================================================== */
/*  公開API: レシピ参照                                                     */
/* ====================================================================== */

const InvRecipe *inv_get_recipe(u16 recipe_id)
{
    return find_recipe(recipe_id);
}

int inv_recipe_count(void)
{
    return inv__recipe_count();
}
