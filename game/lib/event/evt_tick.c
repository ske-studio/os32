/* ======================================================================== */
/*  EVT_TICK.C — libos32event 毎ターン更新・発火判定                        */
/*                                                                          */
/*  ターン進行に伴うアクティブ更新、クールダウンデクリメント、               */
/*  PERIODIC/RANDOM/CONDITION各タイプの発火判定を行う。                      */
/* ======================================================================== */

#include "libos32event.h"
#include "libos32ai.h"    /* ai_weighted_pick */
#include "libos32math.h"  /* rng_next */

/* evt_core.c で定義されたグローバル状態への参照 */
extern EvtDef    g_evt_defs[];
extern u8        g_evt_def_count;
extern EvtActive g_evt_active[];
extern u16       g_evt_no_event_counter;
extern u16       g_evt_cooldowns[];
extern u16       g_evt_fired[];
extern u8        g_evt_fired_count;
extern evt_condition_fn g_evt_condition_cb;

/* evt_trigger.c で定義された内部発火関数 */
extern int evt_fire_internal(u16 event_id, u8 target);

/* ====================================================================== */
/*  内部ヘルパー: アクティブイベント更新 (残りターンデクリメント)            */
/* ====================================================================== */

static void update_active(void)
{
    int i;
    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id != 0 &&
            g_evt_active[i].remaining > 0) {
            g_evt_active[i].remaining--;
            if (g_evt_active[i].remaining == 0) {
                /* 期間満了: スロット解放 */
                g_evt_active[i].event_id = 0;
                g_evt_active[i].target = 0;
            }
        }
    }
}

/* ====================================================================== */
/*  内部ヘルパー: 全クールダウンデクリメント                                */
/* ====================================================================== */

static void decrement_cooldowns(void)
{
    int i;
    for (i = 0; i < (int)g_evt_def_count; i++) {
        if (g_evt_cooldowns[i] > 0) {
            g_evt_cooldowns[i]--;
        }
    }
}

/* ====================================================================== */
/*  内部ヘルパー: 排他グループにアクティブイベントがあるか                  */
/* ====================================================================== */

static int group_has_active(u8 group)
{
    int i;
    int j;

    if (group == 0) return 0;

    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id == 0 ||
            g_evt_active[i].remaining == 0) continue;

        /* アクティブイベントの定義を探してグループ比較 */
        for (j = 0; j < (int)g_evt_def_count; j++) {
            if (g_evt_defs[j].id == g_evt_active[i].event_id) {
                if (g_evt_defs[j].group == group) {
                    return 1;
                }
                break;
            }
        }
    }
    return 0;
}

/* ====================================================================== */
/*  evt_tick — 毎ターン更新                                                */
/*                                                                          */
/*  呼び出し順序:                                                          */
/*    1. 発火バッファクリア                                                 */
/*    2. アクティブイベント更新 (残りターンデクリメント)                     */
/*    3. クールダウンデクリメント                                           */
/*    4. PERIODICイベント判定                                               */
/*    5. RANDOMイベント判定 (カウンタベース確率上昇)                        */
/*    6. CONDITIONイベント判定                                              */
/* ====================================================================== */

int evt_tick(u16 current_turn, const void *context)
{
    int i;
    int random_fired = 0;

    /* ステップ1: 発火バッファクリア */
    g_evt_fired_count = 0;

    /* ステップ2: アクティブイベント更新 */
    update_active();

    /* ステップ3: クールダウンデクリメント */
    decrement_cooldowns();

    /* ステップ4: PERIODICイベント判定 */
    for (i = 0; i < (int)g_evt_def_count; i++) {
        const EvtDef *def = &g_evt_defs[i];
        u8 target;

        if (def->type != EVT_TYPE_PERIODIC) continue;
        if (def->period == 0) continue;
        if (g_evt_cooldowns[i] > 0) continue;
        if (current_turn % def->period != 0) continue;
        if (group_has_active(def->group)) continue;

        target = (def->scope == EVT_SCOPE_GLOBAL) ? 0xFF : 0;
        evt_fire_internal(def->id, target);
    }

    /* ステップ5: RANDOMイベント判定 */
    /* カウンタをインクリメント */
    if (g_evt_no_event_counter < 0xFFFF) {
        g_evt_no_event_counter++;
    }

    /* 確率判定: rng_next() % 256 < g_no_event_counter */
    {
        u32 roll = rng_next() % 256;

        if (roll < (u32)g_evt_no_event_counter) {
            /* 候補リスト作成: クールダウン中でなく、min_turn を超え、排他OK */
            u16 cand_ids[EVT_MAX_DEFS];
            u8  cand_weights[EVT_MAX_DEFS];
            int cand_count = 0;

            for (i = 0; i < (int)g_evt_def_count; i++) {
                const EvtDef *def = &g_evt_defs[i];

                if (def->type != EVT_TYPE_RANDOM) continue;
                if (g_evt_cooldowns[i] > 0) continue;
                if (g_evt_no_event_counter <= def->min_turn) continue;
                if (group_has_active(def->group)) continue;
                if (def->weight == 0) continue;

                cand_ids[cand_count] = def->id;
                cand_weights[cand_count] = def->weight;
                cand_count++;
            }

            if (cand_count > 0) {
                u16 picked;
                u8 target;
                const EvtDef *picked_def;

                picked = ai_weighted_pick(cand_ids, cand_weights, cand_count);

                /* picked のスコープ判定 */
                picked_def = NULL;
                for (i = 0; i < (int)g_evt_def_count; i++) {
                    if (g_evt_defs[i].id == picked) {
                        picked_def = &g_evt_defs[i];
                        break;
                    }
                }

                target = 0xFF;
                if (picked_def != NULL && picked_def->scope == EVT_SCOPE_PLAYER) {
                    target = 0;
                }

                if (evt_fire_internal(picked, target) == 0) {
                    random_fired = 1;
                    g_evt_no_event_counter = 0;
                }
            }
        }
    }

    /* ステップ6: CONDITIONイベント判定 */
    if (g_evt_condition_cb != NULL) {
        for (i = 0; i < (int)g_evt_def_count; i++) {
            const EvtDef *def = &g_evt_defs[i];
            u8 target;

            if (def->type != EVT_TYPE_CONDITION) continue;
            if (g_evt_cooldowns[i] > 0) continue;
            if (group_has_active(def->group)) continue;

            if (g_evt_condition_cb(def->id, current_turn, context)) {
                target = (def->scope == EVT_SCOPE_GLOBAL) ? 0xFF : 0;
                evt_fire_internal(def->id, target);
            }
        }
    }

    (void)random_fired;
    return (int)g_evt_fired_count;
}
