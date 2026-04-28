/* ======================================================================== */
/*  BTL_TRANSFORM.C — libos32battle 変身システム [Phase 2]                  */
/*                                                                          */
/*  変身定義 (BtlTransformDef) に基づいてユニットのステータスを変更し、      */
/*  変身前の値を BtlTransformState に保存する。                              */
/*  ターン経過で自動解除・確率解除を行い、解除時に元のステータスを復元する。 */
/* ======================================================================== */

#include "libos32battle.h"
#include "libos32math.h"
#include <string.h>

/* ====================================================================== */
/*  btl_transform — 変身開始                                               */
/*                                                                          */
/*  処理:                                                                   */
/*    1. 変身前のステータスを state に保存                                   */
/*    2. ステータスを stat_mul_pct で倍率変更                               */
/*    3. HP/max_hp を hp_mul_pct で倍率変更                                 */
/*    4. 専用装備の加算 (weapon_atk, shield_def, armor_def)                  */
/*    5. ターン管理情報をセット                                              */
/*                                                                          */
/*  既に変身中 (state->active == 1) の場合は -1 を返す。                    */
/*  戻り値: 0=成功, -1=エラー                                               */
/* ====================================================================== */

int btl_transform(BtlUnit *unit, BtlTransformState *state,
                   const BtlTransformDef *def)
{
    i16 new_max_hp;

    if (unit == NULL || state == NULL || def == NULL) return -1;
    if (state->active) return -1;  /* 二重変身防止 */

    /* 変身前のステータスを保存 */
    state->orig_atk    = unit->atk;
    state->orig_def    = unit->def;
    state->orig_spd    = unit->spd;
    state->orig_mag    = unit->mag;
    state->orig_max_hp = unit->max_hp;

    /* ステータスを倍率変更 */
    unit->atk = (i16)((int)unit->atk * (int)def->stat_mul_pct / 100);
    unit->def = (i16)((int)unit->def * (int)def->stat_mul_pct / 100);
    unit->spd = (i16)((int)unit->spd * (int)def->stat_mul_pct / 100);
    unit->mag = (i16)((int)unit->mag * (int)def->stat_mul_pct / 100);

    /* 専用装備の加算 */
    unit->atk += def->weapon_atk;
    unit->def += def->shield_def + def->armor_def;

    /* HP倍率変更 */
    new_max_hp = (i16)((int)unit->max_hp * (int)def->hp_mul_pct / 100);
    /* HPも同じ比率で増加 (現在HPの割合を維持) */
    if (unit->max_hp > 0) {
        unit->hp = (i16)((int)unit->hp * (int)new_max_hp / (int)unit->max_hp);
    }
    unit->max_hp = new_max_hp;

    /* ターン管理 */
    state->turns_left   = def->max_turns;
    state->release_rate = def->release_rate;
    state->active       = 1;

    return 0;
}

/* ====================================================================== */
/*  btl_transform_tick — 変身ターン経過                                    */
/*                                                                          */
/*  毎ターン呼び出し。以下の順序で解除判定:                                  */
/*    1. turns_left > 0 の場合: デクリメントし、0になったら解除             */
/*    2. release_rate > 0 の場合: 確率で解除                                */
/*                                                                          */
/*  戻り値: 1=変身解除された, 0=変身継続                                     */
/* ====================================================================== */

int btl_transform_tick(BtlUnit *unit, BtlTransformState *state)
{
    if (unit == NULL || state == NULL) return 0;
    if (!state->active) return 0;

    /* ターン数による解除 */
    if (state->turns_left > 0) {
        state->turns_left--;
        if (state->turns_left == 0) {
            btl_transform_release(unit, state);
            return 1;
        }
    }

    /* 確率による解除 */
    if (state->release_rate > 0) {
        if (rng_range(0, 99) < (int)state->release_rate) {
            btl_transform_release(unit, state);
            return 1;
        }
    }

    return 0;
}

/* ====================================================================== */
/*  btl_transform_release — 変身強制解除                                   */
/*                                                                          */
/*  ステータスを変身前の値に復元する。                                       */
/*  HPは max_hp の縮小に合わせてクランプする (超過分は切り捨て)。            */
/* ====================================================================== */

void btl_transform_release(BtlUnit *unit, BtlTransformState *state)
{
    if (unit == NULL || state == NULL) return;
    if (!state->active) return;

    /* ステータスを復元 */
    unit->atk = state->orig_atk;
    unit->def = state->orig_def;
    unit->spd = state->orig_spd;
    unit->mag = state->orig_mag;

    /* max_hp を復元し、HPが超過していたらクランプ */
    unit->max_hp = state->orig_max_hp;
    if (unit->hp > unit->max_hp) {
        unit->hp = unit->max_hp;
    }

    /* 状態リセット */
    state->active      = 0;
    state->turns_left  = 0;
    state->release_rate = 0;
}
