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
const InvLotteryEntry *inv__get_lotteries(void);
int                    inv__lottery_count(void);

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
    const InvLotteryEntry *lotteries;
    int total_count;
    u16 items[64];
    u16 weights[64];
    int count = 0;
    u32 total_weight = 0;
    u32 roll;
    u32 accum;
    int i;

    lotteries = inv__get_lotteries();
    total_count = inv__lottery_count();

    for (i = 0; i < total_count && count < 64; i++) {
        if (lotteries[i].table_type == table_type && lotteries[i].min_stage <= stage) {
            items[count]   = lotteries[i].item_id;
            weights[count] = lotteries[i].weight;
            total_weight += lotteries[i].weight;
            count++;
        }
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
