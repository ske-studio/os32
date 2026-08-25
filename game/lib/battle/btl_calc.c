/* ======================================================================== */
/*  BTL_CALC.C — libos32battle ダメージ計算・回避・逃走                     */
/*                                                                          */
/*  全てのダメージ計算は整数演算のみで行い、FPUに依存しない。                */
/*  ダメージ計算式はポリシー関数ポインタで差し替え可能。                     */
/* ======================================================================== */

#include "libos32battle.h"
#include "libos32math.h"

/* ====================================================================== */
/*  外部参照 (btl_core.c の内部状態)                                        */
/* ====================================================================== */

extern btl_damage_fn g_damage_policy;
extern btl_damage_fn g_magic_policy;

/* ====================================================================== */
/*  btl_calc_damage — 物理ダメージ計算                                     */
/*                                                                          */
/*  デフォルト計算式: atk * 3/2 - def + rng(-3,+3)                         */
/*  ポリシーが設定されている場合はそちらを使用。                             */
/*  最小値は 0 (ネガティブダメージ = 回復はデフォルトでは発生しない)。       */
/* ====================================================================== */

int btl_calc_damage(int atk, int def)
{
    int dmg;

    if (g_damage_policy != NULL) {
        return g_damage_policy(atk, def);
    }

    /* atk * 1.5 = atk + atk/2 */
    dmg = atk + atk / 2 - def + rng_range(-3, 3);
    if (dmg < 0) dmg = 0;
    return dmg;
}

/* ====================================================================== */
/*  btl_calc_magic_damage — 魔法ダメージ計算                               */
/*                                                                          */
/*  デフォルト計算式: mag * 2 - def + rng(-3,+3)                           */
/* ====================================================================== */

int btl_calc_magic_damage(int mag, int def_mag)
{
    int dmg;

    if (g_magic_policy != NULL) {
        return g_magic_policy(mag, def_mag);
    }

    dmg = mag * 2 - def_mag + rng_range(-3, 3);
    if (dmg < 0) dmg = 0;
    return dmg;
}

/* ====================================================================== */
/*  btl_effective_def_guard — 防御時の実効DEF                              */
/*                                                                          */
/*  def * 1.25 = def + def/4                                               */
/* ====================================================================== */

int btl_effective_def_guard(int def)
{
    return def + def / 4;
}

/* ====================================================================== */
/*  btl_calc_dodge_rate — 回避率計算                                        */
/*                                                                          */
/*  def_spd > atk_spd の場合に回避率が発生。                               */
/*  差分を 0-50% にマッピング。同速以下では 0%。                            */
/*                                                                          */
/*  計算式: rate = min(50, (def_spd - atk_spd) * 2)                        */
/* ====================================================================== */

int btl_calc_dodge_rate(int atk_spd, int def_spd)
{
    int diff, rate;

    diff = def_spd - atk_spd;
    if (diff <= 0) return 0;

    rate = diff * 2;
    if (rate > 50) rate = 50;
    return rate;
}

/* ====================================================================== */
/*  btl_calc_flee_rate — 逃走成功率計算                                     */
/*                                                                          */
/*  速度比に基づく線形マッピング: 20% (同速以下) ~ 80% (2倍速以上)。        */
/*                                                                          */
/*  計算式:                                                                 */
/*    ratio = runner_spd * 256 / chaser_spd  (Q8.0 固定小数点)              */
/*    rate = 20 + (ratio - 256) * 60 / 256                                  */
/*    結果を [20, 80] にクランプ                                            */
/* ====================================================================== */

int btl_calc_flee_rate(int runner_spd, int chaser_spd)
{
    int ratio, rate;

    if (chaser_spd <= 0) return 80;  /* 追跡者速度0 → 確実に逃げられる */
    if (runner_spd <= 0) return 20;  /* 逃走者速度0 → 最低確率 */

    ratio = runner_spd * 256 / chaser_spd;
    rate = 20 + (ratio - 256) * 60 / 256;

    if (rate < 20) rate = 20;
    if (rate > 80) rate = 80;
    return rate;
}

/* ====================================================================== */
/*  btl_set_damage_policy / btl_set_magic_policy                           */
/* ====================================================================== */

void btl_set_damage_policy(btl_damage_fn fn)
{
    g_damage_policy = fn;
}

void btl_set_magic_policy(btl_damage_fn fn)
{
    g_magic_policy = fn;
}
