/* ======================================================================== */
/*  EVT_TRIGGER.C — libos32event 手動発火・連鎖処理                         */
/*                                                                          */
/*  ゲーム側からのイベント手動発火と、発火後の連鎖イベント処理を提供する。   */
/* ======================================================================== */

#include "libos32event.h"
#include "libos32math.h"  /* rng_next */

/* evt_core.c で定義されたグローバル状態への参照 */
extern EvtDef    g_evt_defs[];
extern u8        g_evt_def_count;
extern EvtActive g_evt_active[];
extern u16       g_evt_cooldowns[];
extern u16       g_evt_fired[];
extern u8        g_evt_fired_count;

/* ====================================================================== */
/*  内部ヘルパー: イベント定義をIDで検索                                    */
/* ====================================================================== */

static const EvtDef *find_def(u16 event_id)
{
    int i;
    for (i = 0; i < (int)g_evt_def_count; i++) {
        if (g_evt_defs[i].id == event_id) {
            return &g_evt_defs[i];
        }
    }
    return NULL;
}

/* ====================================================================== */
/*  内部ヘルパー: 排他グループチェック                                      */
/*                                                                          */
/*  指定グループにアクティブなイベントがあれば 1 を返す                      */
/* ====================================================================== */

static int group_conflict(u8 group)
{
    int i;
    if (group == 0) return 0;  /* グループ0は制限なし */

    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        const EvtDef *adef;
        if (g_evt_active[i].event_id == 0 ||
            g_evt_active[i].remaining == 0) continue;

        adef = find_def(g_evt_active[i].event_id);
        if (adef != NULL && adef->group == group) {
            return 1;
        }
    }
    return 0;
}

/* ====================================================================== */
/*  内部ヘルパー: アクティブスロットにイベントを追加                         */
/*                                                                          */
/*  戻り値: 0=成功, -2=アクティブ上限                                       */
/* ====================================================================== */

static int activate_event(const EvtDef *def, u8 target)
{
    int i;

    /* 瞬時イベント (duration=0) はアクティブスロットを使わない */
    if (def->duration == 0) {
        return 0;
    }

    /* 空きスロットを検索 */
    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id == 0 ||
            g_evt_active[i].remaining == 0) {
            g_evt_active[i].event_id = def->id;
            g_evt_active[i].remaining = def->duration;
            g_evt_active[i].target = target;
            return 0;
        }
    }

    return -2;  /* アクティブ上限 */
}

/* ====================================================================== */
/*  内部ヘルパー: 発火バッファにIDを追加                                    */
/* ====================================================================== */

static void record_fired(u16 event_id)
{
    if (g_evt_fired_count < EVT_FIRED_MAX) {
        g_evt_fired[g_evt_fired_count] = event_id;
        g_evt_fired_count++;
    }
}

/* ====================================================================== */
/*  内部ヘルパー: クールダウンインデックス取得                              */
/* ====================================================================== */

static int def_index(u16 event_id)
{
    int i;
    for (i = 0; i < (int)g_evt_def_count; i++) {
        if (g_evt_defs[i].id == event_id) {
            return i;
        }
    }
    return -1;
}

/* ====================================================================== */
/*  evt_fire_internal — 内部発火処理 (連鎖含む)                             */
/*                                                                          */
/*  手動発火 / tick からの共通処理パス。                                    */
/*  排他チェック → アクティブ登録 → クールダウン設定 → 連鎖判定            */
/*                                                                          */
/*  戻り値: 0=成功, -1=排他制約, -2=アクティブ上限                          */
/* ====================================================================== */

int evt_fire_internal(u16 event_id, u8 target)
{
    const EvtDef *def;
    int idx;
    int rc;

    def = find_def(event_id);
    if (def == NULL) return -1;

    /* 排他グループチェック */
    if (group_conflict(def->group)) {
        return -1;
    }

    /* アクティブ登録 */
    rc = activate_event(def, target);
    if (rc < 0) return rc;

    /* クールダウン設定 */
    idx = def_index(event_id);
    if (idx >= 0) {
        g_evt_cooldowns[idx] = def->cooldown;
    }

    /* 発火バッファに記録 */
    record_fired(event_id);

    /* 連鎖処理 */
    if (def->chain_id != 0 && def->chain_chance > 0) {
        u32 roll = rng_next() % 100;
        if (roll < (u32)def->chain_chance) {
            /* 再帰的に連鎖先を発火 (連鎖の連鎖もサポート) */
            evt_fire_internal(def->chain_id, target);
        }
    }

    return 0;
}

/* ====================================================================== */
/*  evt_trigger — 手動でイベントを発火                                      */
/*                                                                          */
/*  ボス討伐報酬・ストーリーイベント等、ゲーム側から直接呼び出す。          */
/* ====================================================================== */

int evt_trigger(u16 event_id, u8 target)
{
    return evt_fire_internal(event_id, target);
}
