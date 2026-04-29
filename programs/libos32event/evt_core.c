/* ======================================================================== */
/*  EVT_CORE.C — libos32event 初期化・終了・DBロード                        */
/*                                                                          */
/*  SQLiteからイベント定義をRAMにロードし、ランタイム状態を管理する。        */
/* ======================================================================== */

#include "libos32event.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include <string.h>

/* ====================================================================== */
/*  内部状態 (他モジュールから extern 参照)                                  */
/* ====================================================================== */

EvtDef    g_evt_defs[EVT_MAX_DEFS];
u8        g_evt_def_count;
EvtActive g_evt_active[EVT_MAX_ACTIVE];

/* 確率発火用カウンタ (未発生連続ターン数) */
u16       g_evt_no_event_counter;

/* 各イベントのクールダウン残りターン */
u16       g_evt_cooldowns[EVT_MAX_DEFS];

/* 前回tickで発火したイベントID一時バッファ */
u16       g_evt_fired[EVT_FIRED_MAX];
u8        g_evt_fired_count;

/* 条件コールバック */
evt_condition_fn g_evt_condition_cb;

/* 内部管理 */
static int       g_db_slot = -1;
static int       g_initialized;

/* ====================================================================== */
/*  evt_init — ライブラリ初期化                                            */
/*                                                                          */
/*  db_path が NULL の場合は DB なしで動作 (テスト用手動定義専用)。          */
/*  DB が指定された場合は events テーブルから全定義をロードする。            */
/* ====================================================================== */

int evt_init(const char *db_path)
{
    if (g_initialized) {
        return 0;  /* 二重初期化防止 */
    }

    memset(g_evt_defs, 0, sizeof(g_evt_defs));
    memset(g_evt_active, 0, sizeof(g_evt_active));
    memset(g_evt_cooldowns, 0, sizeof(g_evt_cooldowns));
    memset(g_evt_fired, 0, sizeof(g_evt_fired));
    g_evt_def_count = 0;
    g_evt_no_event_counter = 0;
    g_evt_fired_count = 0;
    g_evt_condition_cb = NULL;
    g_db_slot = -1;

    if (db_path != NULL) {
        db_handle_t h;

        h = db_open(db_path);
        if (h < 0) {
            return -1;  /* DB接続失敗 */
        }
        g_db_slot = h;

        /* events テーブルから全行をロード */
        DB_LOAD_TABLE(h,
            "SELECT id, type, weight, min_turn, cooldown,"
            " period, duration, grp, chain_id, chain_chance, scope"
            " FROM events ORDER BY id",
            g_evt_defs, EVT_MAX_DEFS, g_evt_def_count,
            {
                row->id           = (u16)db_column_int(0);
                row->type         = (u8)db_column_int(1);
                row->weight       = (u8)db_column_int(2);
                row->min_turn     = (u16)db_column_int(3);
                row->cooldown     = (u16)db_column_int(4);
                row->period       = (u16)db_column_int(5);
                row->duration     = (u8)db_column_int(6);
                row->group        = (u8)db_column_int(7);
                row->chain_id     = (u16)db_column_int(8);
                row->chain_chance = (u8)db_column_int(9);
                row->scope        = (u8)db_column_int(10);
            });
    }

    g_initialized = 1;
    return 0;
}

/* ====================================================================== */
/*  evt_shutdown — ライブラリ終了                                          */
/* ====================================================================== */

void evt_shutdown(void)
{
    if (!g_initialized) return;

    if (g_db_slot >= 0) {
        db_close(g_db_slot);
        g_db_slot = -1;
    }

    memset(g_evt_defs, 0, sizeof(g_evt_defs));
    memset(g_evt_active, 0, sizeof(g_evt_active));
    memset(g_evt_cooldowns, 0, sizeof(g_evt_cooldowns));
    g_evt_def_count = 0;
    g_evt_no_event_counter = 0;
    g_evt_fired_count = 0;
    g_evt_condition_cb = NULL;
    g_initialized = 0;
}

/* ====================================================================== */
/*  evt_reset — 全ランタイム状態リセット (定義は保持)                       */
/* ====================================================================== */

void evt_reset(void)
{
    memset(g_evt_active, 0, sizeof(g_evt_active));
    memset(g_evt_cooldowns, 0, sizeof(g_evt_cooldowns));
    memset(g_evt_fired, 0, sizeof(g_evt_fired));
    g_evt_no_event_counter = 0;
    g_evt_fired_count = 0;
}

/* ====================================================================== */
/*  evt_set_condition_callback — 条件コールバック登録                       */
/* ====================================================================== */

void evt_set_condition_callback(evt_condition_fn fn)
{
    g_evt_condition_cb = fn;
}

/* ====================================================================== */
/*  evt_reset_counter — 未発生カウンタリセット                              */
/* ====================================================================== */

void evt_reset_counter(void)
{
    g_evt_no_event_counter = 0;
}

/* ====================================================================== */
/*  evt_get_counter — 未発生カウンタ取得                                   */
/* ====================================================================== */

u16 evt_get_counter(void)
{
    return g_evt_no_event_counter;
}

/* ====================================================================== */
/*  evt_get_fired — 前回tick発火イベント取得                               */
/* ====================================================================== */

int evt_get_fired(u16 *out_ids, int max)
{
    int i;
    int count;

    if (out_ids == NULL || max <= 0) return 0;

    count = (int)g_evt_fired_count;
    if (count > max) count = max;

    for (i = 0; i < count; i++) {
        out_ids[i] = g_evt_fired[i];
    }
    return count;
}
