/* ======================================================================== */
/*  BTL_STATUS.C — libos32battle 状態異常・バフ/デバフ管理                  */
/*                                                                          */
/*  状態異常のビットフラグ操作、毎ターンのtick処理、                        */
/*  および修飾子 (バフ/デバフ) の加算・ターン経過管理を行う。               */
/* ======================================================================== */

#include "libos32battle.h"
#include <string.h>

/* ====================================================================== */
/*  外部参照 (btl_core.c)                                                   */
/* ====================================================================== */

extern BtlStatusDef *btl_get_status_defs(void);
extern int           btl_get_status_def_count(void);
extern btl_result_cb g_result_callback;

/* ====================================================================== */
/*  btl_apply_status — 状態異常を付与                                      */
/* ====================================================================== */

void btl_apply_status(BtlUnit *unit, u32 status_bit)
{
    if (unit == NULL) return;
    unit->status |= status_bit;
}

/* ====================================================================== */
/*  btl_clear_status — 状態異常を解除                                      */
/* ====================================================================== */

void btl_clear_status(BtlUnit *unit, u32 status_bit)
{
    if (unit == NULL) return;
    unit->status &= ~status_bit;
}

/* ====================================================================== */
/*  btl_has_status — 状態異常の有無を確認                                  */
/* ====================================================================== */

int btl_has_status(const BtlUnit *unit, u32 status_bit)
{
    if (unit == NULL) return 0;
    return (unit->status & status_bit) ? 1 : 0;
}

/* ====================================================================== */
/*  btl_status_tick — 毎ターン処理                                         */
/*                                                                          */
/*  DB定義に基づき以下を行う:                                               */
/*    1. tick_damage > 0 の状態異常: HPを減少                               */
/*    2. prevents_action の状態異常: 行動不能判定                           */
/*    3. duration > 0 の状態異常: カウントダウンし、0になったら自然回復      */
/*                                                                          */
/*  戻り値: 1 = 行動不能 (麻痺, 眠り等), 0 = 行動可能                      */
/*                                                                          */
/*  注意: DB定義がない場合 (btl_init(NULL) の場合) は                        */
/*        フォールバックとして組み込みルールで処理する。                     */
/* ====================================================================== */

int btl_status_tick(BtlUnit *unit)
{
    int prevented;
    int i;
    int def_count;
    BtlStatusDef *defs;

    if (unit == NULL) return 0;
    if (unit->status == 0) return 0;

    prevented = 0;
    defs = btl_get_status_defs();
    def_count = btl_get_status_def_count();

    if (def_count > 0) {
        /* DB定義に基づく処理 */
        for (i = 0; i < def_count; i++) {
            if (unit->status & defs[i].bit_flag) {
                /* tick ダメージ */
                if (defs[i].tick_damage > 0) {
                    unit->hp -= defs[i].tick_damage;
                }
                /* 行動不能 */
                if (defs[i].prevents_action) {
                    prevented = 1;
                }
            }
        }
    } else {
        /* フォールバック: 組み込みルール */
        /* 毒: 毎ターン HP -1 */
        if (unit->status & BTL_STATUS_POISON) {
            unit->hp -= 1;
        }
        /* 麻痺: 行動不能 */
        if (unit->status & BTL_STATUS_PARA) {
            prevented = 1;
        }
        /* 眠り: 行動不能 */
        if (unit->status & BTL_STATUS_SLEEP) {
            prevented = 1;
        }
    }

    return prevented;
}

/* ====================================================================== */
/*  btl_add_modifier — 修飾子を追加                                        */
/*                                                                          */
/*  unit->modifier_count が BTL_MOD_MAX に達している場合は追加不可。         */
/*  戻り値: 0=成功, -1=スロット不足                                         */
/* ====================================================================== */

int btl_add_modifier(BtlUnit *unit, BtlModifier *mods, const BtlModifier *mod)
{
    if (unit == NULL || mods == NULL || mod == NULL) return -1;
    if (unit->modifier_count >= BTL_MOD_MAX) return -1;

    memcpy(&mods[unit->modifier_count], mod, sizeof(BtlModifier));
    unit->modifier_count++;
    return 0;
}

/* ====================================================================== */
/*  btl_tick_modifiers — 修飾子のターン経過処理                             */
/*                                                                          */
/*  turns > 0 の修飾子をデクリメントし、0 になった修飾子を除去。            */
/*  turns == 0 は永続修飾子として除去しない。                               */
/* ====================================================================== */

void btl_tick_modifiers(BtlUnit *unit, BtlModifier *mods)
{
    int i;
    u8 new_count;

    if (unit == NULL || mods == NULL) return;

    new_count = 0;
    for (i = 0; i < (int)unit->modifier_count; i++) {
        if (mods[i].turns > 0) {
            mods[i].turns--;
            if (mods[i].turns == 0) {
                /* 期限切れ → 除去 (コピーしない) */
                continue;
            }
        }
        /* 存続する修飾子を前に詰める */
        if ((int)new_count != i) {
            memcpy(&mods[new_count], &mods[i], sizeof(BtlModifier));
        }
        new_count++;
    }
    unit->modifier_count = new_count;
}

/* ====================================================================== */
/*  btl_clear_modifiers — 全修飾子をクリア                                 */
/* ====================================================================== */

void btl_clear_modifiers(BtlUnit *unit, BtlModifier *mods)
{
    if (unit == NULL || mods == NULL) return;
    memset(mods, 0, sizeof(BtlModifier) * BTL_MOD_MAX);
    unit->modifier_count = 0;
}

/* ====================================================================== */
/*  btl_effective_stat — 実効ステータス計算                                 */
/*                                                                          */
/*  基礎値に対して全修飾子の加算値・乗算%を適用する。                       */
/*  乗算は全修飾子の積: base_mul = 100                                      */
/*  base_mul = base_mul * mul_pct / 100  (各修飾子ごと)                     */
/*  result = (base + total_add) * base_mul / 100                            */
/* ====================================================================== */

i16 btl_effective_stat(const BtlUnit *unit, const BtlModifier *mods, u8 stat)
{
    i16 base;
    int total_add;
    int mul_pct;
    int i;
    int result;

    if (unit == NULL) return 0;

    /* 基礎値を取得 */
    switch (stat) {
        case BTL_STAT_ATK: base = unit->atk; break;
        case BTL_STAT_DEF: base = unit->def; break;
        case BTL_STAT_SPD: base = unit->spd; break;
        case BTL_STAT_MAG: base = unit->mag; break;
        default: return 0;
    }

    if (mods == NULL || unit->modifier_count == 0) {
        return base;
    }

    total_add = 0;
    mul_pct = 100;

    for (i = 0; i < (int)unit->modifier_count; i++) {
        if (mods[i].stat == stat) {
            total_add += (int)mods[i].add_value;
            if (mods[i].mul_pct != 0 && mods[i].mul_pct != 100) {
                mul_pct = mul_pct * (int)mods[i].mul_pct / 100;
            }
        }
    }

    result = ((int)base + total_add) * mul_pct / 100;
    return (i16)result;
}

/* ====================================================================== */
/*  btl_set_result_callback — 結果通知コールバック設定                      */
/* ====================================================================== */

void btl_set_result_callback(btl_result_cb cb)
{
    g_result_callback = cb;
}
