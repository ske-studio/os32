/* ======================================================================== */
/*  BTL_RESOLVE.C — libos32battle コマンドマトリクス解決・ターン解決         */
/*                                                                          */
/*  コマンドマトリクスから結果タイプを引き、1ターン分の攻防を                */
/*  一括解決して BtlResult を返す。                                          */
/* ======================================================================== */

#include "libos32battle.h"
#include "libos32math.h"
#include <string.h>

/* ====================================================================== */
/*  外部参照 (btl_core.c の内部アクセサ)                                    */
/* ====================================================================== */

extern BtlCommandMatrix *btl_get_matrix(void);
extern btl_result_cb     g_result_callback;

/* btl_calc.c のAPI */
extern int btl_calc_damage(int atk, int def);
extern int btl_effective_def_guard(int def);
extern int btl_calc_dodge_rate(int atk_spd, int def_spd);
extern int btl_calc_flee_rate(int runner_spd, int chaser_spd);
extern i16 btl_element_multiplier(u32 atk_elem, u32 def_elem);

/* btl_status.c のAPI */
extern i16 btl_effective_stat(const BtlUnit *unit, const BtlModifier *mods, u8 stat);

/* ====================================================================== */
/*  btl_resolve_commands — マトリクスから結果タイプを引く                   */
/*                                                                          */
/*  範囲外のコマンドは BTL_RES_NORMAL を返す。                              */
/* ====================================================================== */

u8 btl_resolve_commands(u8 atk_cmd, u8 def_cmd)
{
    BtlCommandMatrix *m = btl_get_matrix();

    if (atk_cmd >= BTL_CMD_MAX || def_cmd >= BTL_CMD_MAX) {
        return BTL_RES_NORMAL;
    }
    return m->matrix[atk_cmd][def_cmd];
}

/* ====================================================================== */
/*  btl_resolve_turn — 1ターン分の攻防を一括解決                           */
/*                                                                          */
/*  処理フロー:                                                             */
/*    1. マトリクスから result_type を決定                                   */
/*    2. MISS/ためる → damage=0                                            */
/*    3. YIELD → 逃走判定                                                  */
/*    4. COUNTER → 攻守逆転してダメージ計算                                 */
/*    5. REFLECT → damage=0                                                */
/*    6. 実効ステータス計算 (修飾子はNULLで基礎値のみ)                      */
/*    7. GUARD → 実効DEFを防御ボーナスで増加                                */
/*    8. ダメージ計算 + 属性倍率適用                                         */
/*    9. 回避判定                                                           */
/*   10. コールバック通知                                                   */
/* ====================================================================== */

BtlResult btl_resolve_turn(const BtlUnit *attacker, u8 atk_cmd,
                            const BtlUnit *defender, u8 def_cmd)
{
    BtlResult res;
    u8 rt;
    int eff_atk, eff_def;
    int eff_atk_spd, eff_def_spd;
    int damage;
    int dodge_rate;
    i16 elem_mul;

    memset(&res, 0, sizeof(res));

    /* 1. マトリクスから結果タイプを決定 */
    rt = btl_resolve_commands(atk_cmd, def_cmd);
    res.result_type = rt;

    /* 2. MISS/ためる → ダメージなし */
    if (rt == BTL_RES_MISS) {
        return res;
    }

    /* 3. YIELD → 逃走判定 */
    if (rt == BTL_RES_YIELD) {
        int flee_rate;
        flee_rate = btl_calc_flee_rate((int)attacker->spd, (int)defender->spd);
        if (rng_range(0, 99) < flee_rate) {
            res.is_dodge = 1;  /* 逃走成功 (is_dodge を流用) */
        }
        return res;
    }

    /* 5. REFLECT → ダメージなし */
    if (rt == BTL_RES_REFLECT) {
        return res;
    }

    /* 4. COUNTER → 攻守逆転 */
    if (rt == BTL_RES_COUNTER) {
        /* 攻守を入れ替え: 防御者が攻撃者にダメージ */
        eff_atk = (int)defender->atk;
        eff_def = (int)attacker->def;
        eff_atk_spd = (int)defender->spd;
        eff_def_spd = (int)attacker->spd;
        /* 属性も逆転 */
        elem_mul = btl_element_multiplier(defender->elements, attacker->elements);
    } else {
        /* 通常方向 */
        eff_atk = (int)attacker->atk;
        eff_def = (int)defender->def;
        eff_atk_spd = (int)attacker->spd;
        eff_def_spd = (int)defender->spd;
        elem_mul = btl_element_multiplier(attacker->elements, defender->elements);
    }

    /* 7. GUARD → 防御ボーナス */
    if (rt == BTL_RES_GUARD) {
        eff_def = btl_effective_def_guard(eff_def);
    }

    /* 8. ダメージ計算 */
    damage = btl_calc_damage(eff_atk, eff_def);

    /* 属性倍率適用 */
    res.element_mul = elem_mul;
    if (elem_mul != BTL_ELEM_NEUTRAL) {
        damage = damage * (int)elem_mul / BTL_ELEM_NEUTRAL;
    }

    /* VULN (弱点): ダメージ1.5倍 */
    if (rt == BTL_RES_VULN) {
        damage = damage + damage / 2;
    }

    /* NOGUARD (防御無視): 既にeff_defにGUARDボーナスなしで計算済み */

    /* 9. 回避判定 */
    dodge_rate = btl_calc_dodge_rate(eff_atk_spd, eff_def_spd);
    if (dodge_rate > 0 && rng_range(0, 99) < dodge_rate) {
        res.is_dodge = 1;
        damage = 0;
    }

    res.damage = (i16)damage;

    /* 10. コールバック通知 */
    if (g_result_callback != NULL) {
        g_result_callback(attacker, defender, &res);
    }

    return res;
}

/* ====================================================================== */
/*  btl_first_strike — 先攻判定                                            */
/*                                                                          */
/*  p1_value > p2_value なら 1 (P1先攻)、                                   */
/*  同値ならランダムで 1 or 2。                                              */
/*  それ以外は 2 (P2先攻)。                                                 */
/* ====================================================================== */

int btl_first_strike(int p1_value, int p2_value)
{
    if (p1_value > p2_value) return 1;
    if (p2_value > p1_value) return 2;
    /* 同値: ランダム */
    return (rng_range(0, 1) == 0) ? 1 : 2;
}

/* ====================================================================== */
/*  btl_resolve_multi — 全体攻撃 (Phase 2)                                */
/*                                                                          */
/*  1人の攻撃者が複数の防御者に対して同じ攻撃コマンドを適用する。           */
/*  各防御者ごとに btl_resolve_turn を呼び出し、個別の結果を返す。          */
/*                                                                          */
/*  戻り値: 処理した防御者数                                                */
/* ====================================================================== */

int btl_resolve_multi(const BtlUnit *attacker, u8 atk_cmd,
                       const BtlUnit *defenders, const u8 *def_cmds,
                       BtlResult *results, int count)
{
    int i;

    if (attacker == NULL || defenders == NULL || def_cmds == NULL ||
        results == NULL || count <= 0) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        results[i] = btl_resolve_turn(attacker, atk_cmd,
                                       &defenders[i], def_cmds[i]);
    }

    return count;
}
