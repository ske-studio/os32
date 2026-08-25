/* ======================================================================== */
/*  INV_SHOP.C — ショップ品揃え・売買                                        */
/*                                                                          */
/*  DBの shop_lineup テーブルからステージ別品揃えを取得。                     */
/*  購入・売却操作を提供する。                                               */
/* ======================================================================== */

#include "libos32inv.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* 外部参照 (inv_core.c) */
const InvShopLineup *inv__get_shop_lineups(void);
int                 inv__shop_lineup_count(void);

/* ====================================================================== */
/*  公開API: 価格取得                                                       */
/* ====================================================================== */

u32 inv_get_price(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? def->price : 0;
}

u32 inv_get_sell_price(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? (def->price / 2) : 0;
}

/* ====================================================================== */
/*  公開API: ステージ別品揃え取得                                           */
/* ====================================================================== */

int inv_shop_list(u8 shop_type, u8 stage, u16 *out_ids, int max)
{
    const InvShopLineup *lineups;
    int total_count;
    int i;
    int count = 0;

    if (!out_ids || max <= 0) return 0;

    lineups = inv__get_shop_lineups();
    total_count = inv__shop_lineup_count();

    for (i = 0; i < total_count && count < max; i++) {
        if (lineups[i].shop_type == shop_type && lineups[i].stage <= stage) {
            out_ids[count] = lineups[i].item_id;
            count++;
        }
    }

    return count;
}

/* ====================================================================== */
/*  公開API: 購入                                                           */
/* ====================================================================== */

/* 購入価格の倍率 (百分率)。既定は定価 */
static u16 g_price_scale = 100;

void inv_shop_set_price_scale(u16 percent)
{
    g_price_scale = percent;
}

u16 inv_shop_get_price_scale(void)
{
    return g_price_scale;
}

u32 inv_shop_buy_price(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    if (!def) return 0;
    return (u32)def->price * (u32)g_price_scale / 100;
}

int inv_shop_buy(InvBag *bag, u16 item_id, u32 *wallet)
{
    const InvItemDef *def;
    u32 price;
    int rc;

    if (!bag || !wallet) return -1;

    /* 倍率0 = 休業日。買えない */
    if (g_price_scale == 0) return -4;

    def = inv_get_def(item_id);
    if (!def) return -1;

    price = (u32)def->price * (u32)g_price_scale / 100;

    /* 資金チェック */
    if (*wallet < price) return -2;

    /* インベントリに追加 */
    rc = inv_add(bag, item_id, 1);
    if (rc < 0) return -3;  /* 満杯 */

    /* 資金減算 */
    *wallet -= price;

    return 0;
}

/* ====================================================================== */
/*  公開API: 売却                                                           */
/* ====================================================================== */

int inv_shop_sell(InvBag *bag, u8 slot, u32 *wallet)
{
    u16 item_id;
    u32 sell_price;
    int rc;

    if (!bag || !wallet) return -1;
    if (slot >= bag->max_slots) return -1;

    item_id = bag->slots[slot].item_id;
    if (item_id == 0) return -1;

    sell_price = inv_get_sell_price(item_id);

    /* インベントリから除去 (1個) */
    rc = inv_remove(bag, slot, 1);
    if (rc < 0) return -1;

    /* 売却代金加算 */
    *wallet += sell_price;

    return 0;
}
