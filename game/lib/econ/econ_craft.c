/* ======================================================================== */
/*  ECON_CRAFT.C — クラフトシステム                                          */
/*                                                                          */
/*  レシピ判定とクラフト実行を担当。                                          */
/*  inventory は u16 配列 [ECON_MAX_ITEMS] を想定 (各商品の所持数)。          */
/* ======================================================================== */

#include "econ_internal.h"

/* ====================================================================== */
/*  公開API: クラフト可能判定                                              */
/* ====================================================================== */

int econ_can_craft(u16 recipe_id, const void *inventory)
{
    int ri;
    int i;
    const u16 *inv;

    if (!inventory) return 0;

    ri = econ__find_recipe(recipe_id);
    if (ri < 0) return 0;

    inv = (const u16 *)inventory;

    /* 全素材のチェック */
    for (i = 0; i < g_recipes[ri].mat_count; i++) {
        int ii = econ__find_item(g_recipes[ri].mat_ids[i]);
        if (ii < 0) return 0;

        /* inv[item_id] ≧ 必要数 */
        if (inv[g_recipes[ri].mat_ids[i]] < g_recipes[ri].mat_qtys[i]) {
            return 0;
        }
    }

    return 1;
}

/* ====================================================================== */
/*  公開API: クラフト実行                                                  */
/* ====================================================================== */

int econ_craft(u16 recipe_id, void *inventory)
{
    int ri;
    int i;
    u16 *inv;

    if (!inventory) return -1;

    ri = econ__find_recipe(recipe_id);
    if (ri < 0) return -1;

    inv = (u16 *)inventory;

    /* 素材の消費可能か確認 */
    if (!econ_can_craft(recipe_id, inventory)) return -1;

    /* 素材消費 */
    for (i = 0; i < g_recipes[ri].mat_count; i++) {
        inv[g_recipes[ri].mat_ids[i]] -= g_recipes[ri].mat_qtys[i];
    }

    /* 成果物の追加 */
    inv[g_recipes[ri].result_id] += g_recipes[ri].result_qty;

    return 0;
}

/* ====================================================================== */
/*  公開API: レシピ一覧取得                                                */
/* ====================================================================== */

int econ_get_recipes(u16 category, u16 *out_ids, int max_count)
{
    int i;
    int count = 0;

    if (!out_ids) return 0;

    for (i = 0; i < g_recipe_count && count < max_count; i++) {
        /* カテゴリフィルタ: 生成物のカテゴリで絞り込み */
        int ii = econ__find_item(g_recipes[i].result_id);
        if (ii >= 0 && g_items[ii].category == (u8)category) {
            out_ids[count++] = g_recipes[i].id;
        }
    }

    return count;
}
