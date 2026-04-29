/* ======================================================================== */
/*  INV_LOTTERY.C — 抽選 (宝箱・ドロップ・イベント報酬)                      */
/*                                                                          */
/*  DBの lottery_tables テーブルから重み付き抽選を行う。                      */
/*  ステージフィルタにより、現在以下のステージのアイテムのみを対象とする。    */
/* ======================================================================== */

#include "libos32inv.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* 外部参照 (inv_core.c) */
extern db_handle_t inv__get_db(void);

/* ====================================================================== */
/*  内部: 簡易乱数 (LFSR16)                                                */
/* ====================================================================== */

static u16 g_lottery_seed = 12345;

static u16 lottery_rand(void)
{
    u16 bit;
    bit = ((g_lottery_seed >> 0) ^ (g_lottery_seed >> 2) ^
           (g_lottery_seed >> 3) ^ (g_lottery_seed >> 5)) & 1;
    g_lottery_seed = (g_lottery_seed >> 1) | (bit << 15);
    return g_lottery_seed;
}

/* ====================================================================== */
/*  公開API: 抽選                                                           */
/* ====================================================================== */

u16 inv_lottery(u8 table_type, u8 stage)
{
    db_handle_t db;
    int rc;
    u16 items[64];
    u16 weights[64];
    int count = 0;
    u32 total_weight = 0;
    u32 roll;
    u32 accum;
    int i;

    /* SQLクエリ構築 */
    char sql[160];
    char *p = sql;
    const char *prefix =
        "SELECT item_id, weight FROM lottery_tables "
        "WHERE table_type=";
    const char *mid = " AND min_stage<=";
    char digits[4];
    int di;
    int val;

    db = inv__get_db();
    if (db < 0) return 0;

    while (*prefix) { *p++ = *prefix++; }

    val = table_type;
    di = 0;
    if (val == 0) {
        digits[di++] = '0';
    } else {
        while (val > 0) { digits[di++] = '0' + (char)(val % 10); val /= 10; }
    }
    while (di > 0) { *p++ = digits[--di]; }

    while (*mid) { *p++ = *mid++; }

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

    while (rc == DB_STATUS_ROW && count < 64) {
        items[count]   = (u16)db_column_int(0);
        weights[count] = (u16)db_column_int(1);
        total_weight += weights[count];
        count++;
        rc = db_step(db);
    }

    if (count == 0 || total_weight == 0) return 0;

    /* 重み付き抽選 */
    roll = lottery_rand() % total_weight;
    accum = 0;
    for (i = 0; i < count; i++) {
        accum += weights[i];
        if (roll < accum) {
            return items[i];
        }
    }

    /* フォールバック: 最後のアイテム */
    return items[count - 1];
}
