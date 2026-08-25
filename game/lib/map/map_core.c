/* ======================================================================== */
/*  MAP_CORE.C — マップ管理ライブラリ コア実装                               */
/*                                                                          */
/*  初期化・終了・DBからのマップロード・タイルプロパティキャッシュを担当。     */
/*  DB操作はロード時のみ行い、ランタイムではRAMキャッシュのみを参照する。     */
/* ======================================================================== */

#include "libos32map.h"
#include "libos32db.h"
#include "libos32db_util.h"

/* KernelAPI ポインタ (crt0_c.c で定義) */
extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  libc関数 (外部から提供)                                                */
/* ====================================================================== */

extern void *memset(void *, int, unsigned int);
extern void *memcpy(void *, const void *, unsigned int);

/* ====================================================================== */
/*  内部グローバル状態                                                     */
/* ====================================================================== */

/* DB接続ハンドル */
static db_handle_t g_db = -1;

/* 現在のマップ定義 */
static MapDef g_mapdef;

/* タイルデータ (動的確保, レイヤー別) */
static u16 *g_tiles[MAP_MAX_LAYERS];

/* タイルプロパティキャッシュ */
static TileProp g_tile_props[MAP_MAX_TILE_PROPS];
static int g_tile_prop_count;

/* イベントキャッシュ */
static MapEvent g_events[MAP_MAX_EVENTS];
static int g_event_count;

/* ワープキャッシュ */
static MapWarp g_warps[MAP_MAX_WARPS];
static int g_warp_count;

/* エンカウントキャッシュ */
static MapEncounter g_encounters[MAP_MAX_ENCOUNTERS];
static int g_encounter_count;

/* カメラ位置 */
static i16 g_camera_x;
static i16 g_camera_y;

/* イベントコールバック */
static map_event_callback g_event_cb;

/* エンカウント歩数カウンタ */
static u16 g_step_count;

/* ====================================================================== */
/*  ゲッター (他ソースファイルから参照用)                                   */
/* ====================================================================== */

MapDef *map__get_mapdef(void)           { return &g_mapdef; }
u16   **map__get_tiles(void)            { return g_tiles; }
TileProp *map__get_tile_props(void)     { return g_tile_props; }
int    map__tile_prop_count(void)       { return g_tile_prop_count; }
MapEvent *map__get_events(void)         { return g_events; }
int    map__event_count(void)           { return g_event_count; }
MapWarp *map__get_warps(void)           { return g_warps; }
int    map__warp_count(void)            { return g_warp_count; }
MapEncounter *map__get_encounters(void) { return g_encounters; }
int    map__encounter_count(void)       { return g_encounter_count; }
i16   *map__get_camera_x(void)          { return &g_camera_x; }
i16   *map__get_camera_y(void)          { return &g_camera_y; }
map_event_callback map__get_event_cb(void) { return g_event_cb; }
u16   *map__get_step_count(void)        { return &g_step_count; }
db_handle_t map__get_db(void)           { return g_db; }



/* ====================================================================== */
/*  DBキャッシュ読み込み                                                    */
/* ====================================================================== */

/* タイルプロパティをDBからRAMキャッシュに読み込む */
static int load_tile_props(u8 tileset_id)
{
    char sql[128];
    char *p;

    p = sql;
    db_sql_append(&p,
        "SELECT tile_id, passable, flags, "
        "chem_elem, chem_temp, damage "
        "FROM tile_props WHERE tileset_id=");
    db_sql_append_int(&p, (int)tileset_id);

    return DB_LOAD_TABLE_OPT(g_db, sql,
        g_tile_props, MAP_MAX_TILE_PROPS, g_tile_prop_count,
        {
            row->tile_id         = (u16)db_column_int(0);
            row->passable        = (u8)db_column_int(1);
            row->flags           = (u8)db_column_int(2);
            row->chem_elements   = (u32)db_column_int(3);
            row->chem_temperature = (i16)db_column_int(4);
            row->damage          = (u16)db_column_int(5);
        });
}

/* マップイベントをDBからRAMキャッシュに読み込む */
static int load_events(u16 map_id)
{
    int rc;
    int count = 0;
    char sql[128];
    char *p;
    const char *script_str;
    int slen, i;

    p = sql;
    db_sql_append(&p,
        "SELECT id, map_id, x, y, trigger, type, param, script "
        "FROM map_events WHERE map_id=");
    db_sql_append_int(&p, (int)map_id);

    rc = db_query(g_db, sql);
    if (rc < 0) {
        return 0;
    }

    while (rc == DB_STATUS_ROW && count < MAP_MAX_EVENTS) {
        g_events[count].id      = (u16)db_column_int(0);
        g_events[count].map_id  = (u16)db_column_int(1);
        g_events[count].x       = (i16)db_column_int(2);
        g_events[count].y       = (i16)db_column_int(3);
        g_events[count].trigger = (u8)db_column_int(4);
        g_events[count].type    = (u8)db_column_int(5);
        g_events[count].param   = (u16)db_column_int(6);

        /* スクリプト名をコピー */
        script_str = db_column_text(7);
        memset(g_events[count].script, 0, 32);
        if (script_str) {
            slen = 0;
            while (script_str[slen] && slen < 31) slen++;
            for (i = 0; i < slen; i++) {
                g_events[count].script[i] = script_str[i];
            }
        }

        count++;
        rc = db_step(g_db);
    }

    g_event_count = count;
    return count;
}

/* ワープ定義をDBからRAMキャッシュに読み込む */
static int load_warps(u16 map_id)
{
    char sql[128];
    char *p;

    p = sql;
    db_sql_append(&p,
        "SELECT id, src_map, src_x, src_y, "
        "dst_map, dst_x, dst_y, direction "
        "FROM warps WHERE src_map=");
    db_sql_append_int(&p, (int)map_id);

    return DB_LOAD_TABLE_OPT(g_db, sql,
        g_warps, MAP_MAX_WARPS, g_warp_count,
        {
            row->id        = (u16)db_column_int(0);
            row->src_map   = (u16)db_column_int(1);
            row->src_x     = (i16)db_column_int(2);
            row->src_y     = (i16)db_column_int(3);
            row->dst_map   = (u16)db_column_int(4);
            row->dst_x     = (i16)db_column_int(5);
            row->dst_y     = (i16)db_column_int(6);
            row->direction = (u8)db_column_int(7);
            row->_pad      = 0;
        });
}

/* エンカウント設定をDBからRAMキャッシュに読み込む */
static int load_encounters(u16 map_id)
{
    char sql[128];
    char *p;

    p = sql;
    db_sql_append(&p,
        "SELECT map_id, enemy_id, rate, min_steps "
        "FROM encounters WHERE map_id=");
    db_sql_append_int(&p, (int)map_id);

    return DB_LOAD_TABLE_OPT(g_db, sql,
        g_encounters, MAP_MAX_ENCOUNTERS, g_encounter_count,
        {
            row->map_id    = (u16)db_column_int(0);
            row->enemy_id  = (u16)db_column_int(1);
            row->rate      = (u8)db_column_int(2);
            row->_pad      = 0;
            row->min_steps = (u16)db_column_int(3);
        });
}

/* ====================================================================== */
/*  公開API: システム管理                                                   */
/* ====================================================================== */

int map_init(const char *db_path)
{
    /* 既に初期化済みの場合はシャットダウンしてから再初期化 */
    if (g_db >= 0) {
        map_shutdown();
    }

    /* 内部状態クリア */
    memset(&g_mapdef, 0, sizeof(g_mapdef));
    memset(g_tile_props, 0, sizeof(g_tile_props));
    memset(g_events, 0, sizeof(g_events));
    memset(g_warps, 0, sizeof(g_warps));
    memset(g_encounters, 0, sizeof(g_encounters));
    g_tile_prop_count = 0;
    g_event_count = 0;
    g_warp_count = 0;
    g_encounter_count = 0;
    g_camera_x = 0;
    g_camera_y = 0;
    g_event_cb = (map_event_callback)0;
    g_step_count = 0;
    g_tiles[0] = (u16 *)0;
    g_tiles[1] = (u16 *)0;
    g_tiles[2] = (u16 *)0;

    /* DB接続 */
    g_db = db_open(db_path);
    if (g_db < 0) {
        return -1;
    }

    return 0;
}

void map_shutdown(void)
{
    map_unload();

    if (g_db >= 0) {
        db_close(g_db);
        g_db = -1;
    }

    g_tile_prop_count = 0;
    g_event_count = 0;
    g_warp_count = 0;
    g_encounter_count = 0;
    g_event_cb = (map_event_callback)0;
}

/* ====================================================================== */
/*  公開API: マップロード・切替                                             */
/* ====================================================================== */

void map_unload(void)
{
    int i;

    /* タイルデータ解放 */
    for (i = 0; i < MAP_MAX_LAYERS; i++) {
        if (g_tiles[i]) {
            api->mem_free(g_tiles[i]);
            g_tiles[i] = (u16 *)0;
        }
    }

    /* マップ定義クリア */
    memset(&g_mapdef, 0, sizeof(g_mapdef));
    g_event_count = 0;
    g_warp_count = 0;
    g_encounter_count = 0;
    g_camera_x = 0;
    g_camera_y = 0;
    g_step_count = 0;
}

int map_load(u16 map_id)
{
    int rc;
    int layer;
    char sql[128];
    char *p;
    const char *name_str;
    int nlen, i;
    u32 tile_bytes;

    if (g_db < 0) {
        return -1;
    }

    /* 既存マップ解放 */
    map_unload();

    /* --- マップ定義読み込み --- */
    p = sql;
    db_sql_append(&p,
        "SELECT id, name, width, height, layer_count, "
        "tileset_id, bgm_id FROM maps WHERE id=");
    db_sql_append_int(&p, (int)map_id);

    rc = db_query(g_db, sql);
    if (rc != DB_STATUS_ROW) {
        return -2;  /* マップが見つからない */
    }

    g_mapdef.id          = (u16)db_column_int(0);
    /* name はテキストカラム */
    name_str = db_column_text(1);
    memset(g_mapdef.name, 0, 16);
    if (name_str) {
        nlen = 0;
        while (name_str[nlen] && nlen < 15) nlen++;
        for (i = 0; i < nlen; i++) {
            g_mapdef.name[i] = name_str[i];
        }
    }
    g_mapdef.width       = (u16)db_column_int(2);
    g_mapdef.height      = (u16)db_column_int(3);
    g_mapdef.layer_count = (u8)db_column_int(4);
    g_mapdef.tileset_id  = (u8)db_column_int(5);
    g_mapdef.bgm_id      = (u16)db_column_int(6);
    g_mapdef._pad        = 0;

    /* サイズバリデーション */
    if (!MAP_SIZE_VALID(g_mapdef.width) ||
        !MAP_SIZE_VALID(g_mapdef.height)) {
        memset(&g_mapdef, 0, sizeof(g_mapdef));
        return -3;  /* 不正なマップサイズ */
    }
    if (g_mapdef.layer_count < 1 || g_mapdef.layer_count > MAP_MAX_LAYERS) {
        memset(&g_mapdef, 0, sizeof(g_mapdef));
        return -4;  /* 不正なレイヤー数 */
    }

    /* --- タイルデータ用バッファ確保 --- */
    tile_bytes = (u32)g_mapdef.width * (u32)g_mapdef.height * 2;
    for (layer = 0; layer < (int)g_mapdef.layer_count; layer++) {
        g_tiles[layer] = (u16 *)api->mem_alloc(tile_bytes);
        if (!g_tiles[layer]) {
            map_unload();
            return -5;  /* メモリ確保失敗 */
        }
        memset(g_tiles[layer], 0, tile_bytes);
    }

    /* --- タイルデータ (BLOB) 読み込み --- */
    for (layer = 0; layer < (int)g_mapdef.layer_count; layer++) {
        const void *blob;
        int blob_size;

        p = sql;
        db_sql_append(&p,
            "SELECT tile_data FROM map_tiles WHERE map_id=");
        db_sql_append_int(&p, (int)map_id);
        db_sql_append(&p, " AND layer=");
        db_sql_append_int(&p, layer);

        rc = db_query(g_db, sql);
        if (rc == DB_STATUS_ROW) {
            blob = db_column_blob(0);
            blob_size = db_column_bytes(0);

            if (blob && blob_size > 0) {
                /* BLOBサイズチェック */
                if ((u32)blob_size > tile_bytes) {
                    blob_size = (int)tile_bytes;
                }
                memcpy(g_tiles[layer], blob, (unsigned int)blob_size);
            }
        }
        /* BLOB が見つからなくてもエラーにしない (空レイヤー) */
    }

    /* --- タイルプロパティキャッシュ --- */
    load_tile_props(g_mapdef.tileset_id);

    /* --- イベント・ワープ・エンカウント読み込み --- */
    load_events(map_id);
    load_warps(map_id);
    load_encounters(map_id);

    return 0;
}

const MapDef *map_current(void)
{
    if (g_mapdef.id == 0) {
        return (const MapDef *)0;
    }
    return &g_mapdef;
}

int map_warp(u16 warp_id)
{
    int i;
    const MapWarp *w = (const MapWarp *)0;
    int rc;

    /* ワープID検索 */
    for (i = 0; i < g_warp_count; i++) {
        if (g_warps[i].id == warp_id) {
            w = &g_warps[i];
            break;
        }
    }
    if (!w) {
        return -1;  /* ワープが見つからない */
    }

    /* 遷移先マップのロード */
    rc = map_load(w->dst_map);
    if (rc < 0) {
        return rc;
    }

    /* カメラ位置を到着座標に設定 */
    g_camera_x = w->dst_x;
    g_camera_y = w->dst_y;

    return 0;
}

/* ====================================================================== */
/*  公開API: カメラ管理                                                     */
/* ====================================================================== */

void map_set_camera(i16 tile_x, i16 tile_y)
{
    g_camera_x = tile_x;
    g_camera_y = tile_y;
}

void map_get_camera(i16 *out_x, i16 *out_y)
{
    if (out_x) *out_x = g_camera_x;
    if (out_y) *out_y = g_camera_y;
}

/* ====================================================================== */
/*  公開API: コールバック設定                                               */
/* ====================================================================== */

void map_set_event_callback(map_event_callback cb)
{
    g_event_cb = cb;
}

/* ====================================================================== */
/*  公開API: デバッグ                                                       */
/* ====================================================================== */

void map_debug_dump(void)
{
    if (g_mapdef.id == 0) {
        api->kprintf(ATTR_YELLOW, "map: no map loaded\n");
        return;
    }

    api->kprintf(ATTR_WHITE, "--- Map Debug Dump ---\n");
    api->kprintf(ATTR_WHITE, "  id=%d name=%s\n",
                 (int)g_mapdef.id, g_mapdef.name);
    api->kprintf(ATTR_WHITE, "  size=%dx%d layers=%d tileset=%d bgm=%d\n",
                 (int)g_mapdef.width, (int)g_mapdef.height,
                 (int)g_mapdef.layer_count, (int)g_mapdef.tileset_id,
                 (int)g_mapdef.bgm_id);
    api->kprintf(ATTR_WHITE, "  tile_props=%d events=%d warps=%d enc=%d\n",
                 g_tile_prop_count, g_event_count,
                 g_warp_count, g_encounter_count);
    api->kprintf(ATTR_WHITE, "  camera=(%d,%d) steps=%d\n",
                 (int)g_camera_x, (int)g_camera_y, (int)g_step_count);
}
