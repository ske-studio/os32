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
extern db_handle_t inv__get_db(void);

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
    db_handle_t db;
    int rc;
    int count = 0;

    /* shop_lineupテーブルからクエリ */
    /* SQLクエリを手動構築 (C89: snprintfなし、sprintfは安全性の観点で避ける) */
    char sql[128];
    char *p = sql;
    const char *prefix = "SELECT item_id FROM shop_lineup WHERE shop_type=";
    const char *mid = " AND stage<=";
    char digits[4];
    int di;
    int val;

    if (!out_ids || max <= 0) return 0;

    db = inv__get_db();
    if (db < 0) return 0;

    /* クエリ文字列構築 */
    while (*prefix) { *p++ = *prefix++; }

    /* shop_type を文字列に変換 */
    val = shop_type;
    di = 0;
    if (val == 0) {
        digits[di++] = '0';
    } else {
        while (val > 0) { digits[di++] = '0' + (char)(val % 10); val /= 10; }
    }
    while (di > 0) { *p++ = digits[--di]; }

    while (*mid) { *p++ = *mid++; }

    /* stage を文字列に変換 */
    val = stage;
    di = 0;
    if (val == 0) {
        digits[di++] = '0';
    } else {
        while (val > 0) { digits[di++] = '0' + (char)(val % 10); val /= 10; }
    }
    while (di > 0) { *p++ = digits[--di]; }

    *p = '\0';

    rc = db_query(db, sql);
    if (rc < 0) return 0;

    while (rc == DB_STATUS_ROW && count < max) {
        out_ids[count] = (u16)db_column_int(0);
        count++;
        rc = db_step(db);
    }

    return count;
}

/* ====================================================================== */
/*  公開API: 購入                                                           */
/* ====================================================================== */

int inv_shop_buy(InvBag *bag, u16 item_id, u32 *wallet)
{
    const InvItemDef *def;
    int rc;

    if (!bag || !wallet) return -1;

    def = inv_get_def(item_id);
    if (!def) return -1;

    /* 資金チェック */
    if (*wallet < def->price) return -2;

    /* インベントリに追加 */
    rc = inv_add(bag, item_id, 1);
    if (rc < 0) return -3;  /* 満杯 */

    /* 資金減算 */
    *wallet -= def->price;

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
