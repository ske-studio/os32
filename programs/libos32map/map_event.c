/* ======================================================================== */
/*  MAP_EVENT.C — イベント検索・ワープ・エンカウント判定                      */
/*                                                                          */
/*  マップ上のイベント・ワープ・エンカウントに関する処理を担当。              */
/*  全てRAMキャッシュに基づくため、ホットパスで安全に使用可能。               */
/* ======================================================================== */

#include "libos32map.h"

/* KernelAPI ポインタ (crt0_c.c で定義) */
extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  内部状態アクセサ (map_core.c で定義)                                    */
/* ====================================================================== */

extern MapDef         *map__get_mapdef(void);
extern MapEvent       *map__get_events(void);
extern int             map__event_count(void);
extern MapWarp        *map__get_warps(void);
extern int             map__warp_count(void);
extern MapEncounter   *map__get_encounters(void);
extern int             map__encounter_count(void);
extern map_event_callback map__get_event_cb(void);
extern u16            *map__get_step_count(void);

/* ====================================================================== */
/*  公開API: イベント検索                                                   */
/* ====================================================================== */

const MapEvent *map_get_event(int col, int row, u8 trigger)
{
    MapEvent *events = map__get_events();
    int count = map__event_count();
    int i;

    for (i = 0; i < count; i++) {
        if ((int)events[i].x == col &&
            (int)events[i].y == row &&
            events[i].trigger == trigger) {
            return &events[i];
        }
    }
    return (const MapEvent *)0;
}

/* ====================================================================== */
/*  公開API: 歩行時イベントチェック                                         */
/* ====================================================================== */

int map_check_step(int col, int row)
{
    const MapEvent *evt;
    map_event_callback cb;
    const TileProp *prop;
    u16 *step_count;

    /* マップ未ロード */
    if (!map_current()) return 0;

    /* 歩数カウント */
    step_count = map__get_step_count();
    if (*step_count < 65535) {
        (*step_count)++;
    }

    /* STEP トリガーのイベントを検索 */
    evt = map_get_event(col, row, MAP_TRIG_STEP);
    if (evt) {
        cb = map__get_event_cb();
        if (cb) {
            cb(evt);
        }
        /* ワープイベントの場合は自動実行 */
        if (evt->type == MAP_EVT_WARP && evt->param > 0) {
            return (int)evt->param;  /* ワープ先マップIDを返す */
        }
        return 1;  /* イベントあり */
    }

    /* タイルプロパティによるワープチェック */
    prop = map_get_prop(col, row);
    if (prop && (prop->flags & MAP_TFLAG_WARP)) {
        /* ワープフラグ付きタイル — ワープ検索 */
        {
            MapWarp *warps = map__get_warps();
            int warp_count = map__warp_count();
            int i;
            for (i = 0; i < warp_count; i++) {
                if ((int)warps[i].src_x == col &&
                    (int)warps[i].src_y == row) {
                    return -(int)warps[i].id; /* 負値: ワープID */
                }
            }
        }
    }

    return 0;  /* 特に何もなし */
}

/* ====================================================================== */
/*  公開API: エンカウント判定                                               */
/* ====================================================================== */

u16 map_check_encounter(void)
{
    MapEncounter *encounters = map__get_encounters();
    int enc_count = map__encounter_count();
    u16 *step_count = map__get_step_count();
    int i;
    u32 rnd;

    if (enc_count == 0) return 0;

    for (i = 0; i < enc_count; i++) {
        /* 最小歩数に達していない場合はスキップ */
        if (*step_count < encounters[i].min_steps) continue;

        /* 簡易乱数判定 (tick値ベース) */
        rnd = api->get_tick();
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF;

        if ((rnd % 100) < (u32)encounters[i].rate) {
            return encounters[i].enemy_id;
        }
    }

    return 0;
}

void map_reset_steps(void)
{
    u16 *step_count = map__get_step_count();
    *step_count = 0;
}
