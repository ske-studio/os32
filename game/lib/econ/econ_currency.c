/* ======================================================================== */
/*  ECON_CURRENCY.C — 通貨・為替・リフレーション                              */
/*                                                                          */
/*  通貨間の為替レート計算、通貨変換、通貨発行、税率設定を担当。               */
/* ======================================================================== */

#include "econ_internal.h"

/* ====================================================================== */
/*  公開API: 為替レート                                                    */
/* ====================================================================== */

u16 econ_exchange_rate(u8 currency_a, u8 currency_b)
{
    int ia;
    int ib;
    u32 rate;

    if (currency_a == currency_b) return 100;

    ia = econ__find_currency(currency_a);
    ib = econ__find_currency(currency_b);
    if (ia < 0 || ib < 0) return 100;

    /* レート = (base_value_a / supply_a) / (base_value_b / supply_b) × 100 */
    /* = (base_value_a * supply_b * 100) / (base_value_b * supply_a) */
    if (g_currencies[ib].base_value == 0 ||
        g_currencies[ia].supply == 0) return 100;

    rate = (u32)g_currencies[ia].base_value *
           (u32)g_currencies[ib].supply * 100 /
           ((u32)g_currencies[ib].base_value *
            (u32)g_currencies[ia].supply);

    if (rate > 65535) rate = 65535;
    if (rate == 0) rate = 1;

    return (u16)rate;
}

/* ====================================================================== */
/*  公開API: 通貨変換                                                      */
/* ====================================================================== */

u32 econ_convert(u32 amount, u8 from_currency, u8 to_currency)
{
    u16 rate;

    if (from_currency == to_currency) return amount;

    rate = econ_exchange_rate(from_currency, to_currency);
    return amount * rate / 100;
}

/* ====================================================================== */
/*  公開API: 通貨発行                                                      */
/* ====================================================================== */

void econ_mint(u8 nation_id, u16 amount)
{
    int i;
    for (i = 0; i < g_currency_count; i++) {
        if (g_currencies[i].nation_id == nation_id) {
            u32 new_supply = (u32)g_currencies[i].supply + amount;
            if (new_supply > 65535) new_supply = 65535;
            g_currencies[i].supply = (u16)new_supply;
            return;
        }
    }
}

/* ====================================================================== */
/*  公開API: 税率設定                                                      */
/* ====================================================================== */

void econ_set_tax(u16 market_id, u8 rate)
{
    int mi = econ__find_market(market_id);
    if (mi < 0) return;
    g_markets[mi].tax_rate = rate;
}

/* ====================================================================== */
/*  公開API: 外交                                                          */
/* ====================================================================== */

u16 econ_diplomacy_modifier(u8 player_nation, u16 market_id)
{
    int mi;
    int di;
    i32 mod;

    mi = econ__find_market(market_id);
    if (mi < 0) return 100;

    di = econ__find_diplomacy(player_nation, g_markets[mi].nation_id);
    if (di < 0) return 100; /* 外交関係なし = 中立 */

    /* relation (-100~+100) → 倍率 (80~120%) */
    mod = 100 + (i32)g_diplomacy[di].relation / 5;
    if (mod < 50) mod = 50;
    if (mod > 200) mod = 200;

    /* 禁輸フラグ */
    if (g_diplomacy[di].flags & 0x02) return 200; /* 禁輸 = 倍額 */

    /* 通商条約フラグ */
    if (g_diplomacy[di].flags & 0x01) {
        mod = mod * 90 / 100;  /* 10% 追加割引 */
    }

    return (u16)mod;
}

void econ_change_relation(u8 nation_a, u8 nation_b, i8 delta)
{
    int di = econ__find_diplomacy(nation_a, nation_b);
    i32 new_rel;

    if (di < 0) {
        /* 新しい外交関係を作成 */
        if (g_diplomacy_count >= ECON_MAX_DIPLOMACY) return;
        di = g_diplomacy_count++;
        g_diplomacy[di].nation_a = nation_a;
        g_diplomacy[di].nation_b = nation_b;
        g_diplomacy[di].relation = 0;
        g_diplomacy[di].flags = 0;
    }

    new_rel = (i32)g_diplomacy[di].relation + (i32)delta;
    if (new_rel > 100) new_rel = 100;
    if (new_rel < -100) new_rel = -100;
    g_diplomacy[di].relation = (i8)new_rel;
}
