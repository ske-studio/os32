/* ======================================================================== */
/*  EVT_ACTIVE.C — libos32event アクティブイベント管理                      */
/*                                                                          */
/*  持続イベントの状態管理: アクティブ判定・一覧取得・手動終了               */
/* ======================================================================== */

#include "libos32event.h"
#include <string.h>

/* evt_core.c で定義されたグローバル状態への参照 */
extern EvtActive g_evt_active[];

/* ====================================================================== */
/*  evt_is_active — 特定イベントがアクティブか判定                          */
/* ====================================================================== */

int evt_is_active(u16 event_id)
{
    int i;
    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id == event_id &&
            g_evt_active[i].remaining > 0) {
            return 1;
        }
    }
    return 0;
}

/* ====================================================================== */
/*  evt_active_list — アクティブイベント一覧取得                            */
/* ====================================================================== */

int evt_active_list(EvtActive *out, int max)
{
    int i;
    int count = 0;

    if (out == NULL || max <= 0) return 0;

    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id != 0 &&
            g_evt_active[i].remaining > 0) {
            if (count >= max) break;
            memcpy(&out[count], &g_evt_active[i], sizeof(EvtActive));
            count++;
        }
    }
    return count;
}

/* ====================================================================== */
/*  evt_active_count — アクティブイベント数                                 */
/* ====================================================================== */

int evt_active_count(void)
{
    int i;
    int count = 0;

    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id != 0 &&
            g_evt_active[i].remaining > 0) {
            count++;
        }
    }
    return count;
}

/* ====================================================================== */
/*  evt_cancel — アクティブイベントを強制終了                               */
/* ====================================================================== */

void evt_cancel(u16 event_id)
{
    int i;
    for (i = 0; i < EVT_MAX_ACTIVE; i++) {
        if (g_evt_active[i].event_id == event_id) {
            g_evt_active[i].event_id = 0;
            g_evt_active[i].remaining = 0;
            g_evt_active[i].target = 0;
        }
    }
}
