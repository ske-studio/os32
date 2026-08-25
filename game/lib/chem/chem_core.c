/* ======================================================================== */
/*  CHEM_CORE.C — 化学エンジン コア実装                                     */
/*                                                                          */
/*  初期化・終了・DBからのルールキャッシュ・オブジェクト配列管理を担当。       */
/*  DB操作は起動/終了時のみ行い、ランタイムではRAMキャッシュのみを参照する。  */
/* ======================================================================== */

#include "libos32chem.h"
#include "libos32db.h"
#include "libos32db_util.h"

/* KernelAPI ポインタ (crt0_c.c で定義) */
extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  内部グローバル状態                                                     */
/* ====================================================================== */

/* オブジェクト配列 */
static ChemObject g_objects[CHEM_MAX_OBJECTS];

/* 反応ルールキャッシュ */
static ChemReaction g_reactions[CHEM_MAX_REACTIONS];
static int g_reaction_count;

/* 状態遷移ルールキャッシュ */
static ChemPhaseRule g_phases[CHEM_MAX_PHASES];
static int g_phase_count;

/* DB接続ハンドル */
static db_handle_t g_db = -1;

/* 反応コールバック */
static chem_reaction_callback g_callback;

/* 次のユニークID (0は未使用を表すため1から開始) */
static u16 g_next_id = 1;

/* ====================================================================== */
/*  ゲッター (他ソースファイルから参照用)                                   */
/* ====================================================================== */

ChemObject *chem__get_objects(void)  { return g_objects; }
ChemReaction *chem__get_reactions(void) { return g_reactions; }
int chem__get_reaction_count(void)   { return g_reaction_count; }
ChemPhaseRule *chem__get_phases(void) { return g_phases; }
int chem__get_phase_count(void)      { return g_phase_count; }
chem_reaction_callback chem__get_callback(void) { return g_callback; }

/* ====================================================================== */
/*  ヘルパー: memset / memcpy (libcから提供)                               */
/* ====================================================================== */

extern void *memset(void *, int, unsigned int);

/* ====================================================================== */
/*  DBルール読み込み                                                       */
/* ====================================================================== */

/* 反応ルールをDBからRAMキャッシュに読み込む */
static int load_reactions(void)
{
    return DB_LOAD_TABLE(g_db,
        "SELECT elem_a, elem_b, action, target, "
        "spawn_elem, temp_delta, hp_delta, priority "
        "FROM reactions ORDER BY priority DESC",
        g_reactions, CHEM_MAX_REACTIONS, g_reaction_count,
        {
            row->elem_a     = (u32)db_column_int(0);
            row->elem_b     = (u32)db_column_int(1);
            row->action     = (u8)db_column_int(2);
            row->target     = (u8)db_column_int(3);
            row->spawn_elem = (u16)db_column_int(4);
            row->temp_delta = (i16)db_column_int(5);
            row->hp_delta   = (i16)db_column_int(6);
            row->priority   = (u8)db_column_int(7);
            row->_pad       = 0;
        });
}

/* 状態遷移ルールをDBからRAMキャッシュに読み込む */
static int load_phases(void)
{
    return DB_LOAD_TABLE(g_db,
        "SELECT elem_from, temp_min, temp_max, "
        "elem_to, spawn_elem "
        "FROM phase_transitions",
        g_phases, CHEM_MAX_PHASES, g_phase_count,
        {
            row->elem_from   = (u32)db_column_int(0);
            row->temp_min    = (i16)db_column_int(1);
            row->temp_max    = (i16)db_column_int(2);
            row->elem_to     = (u32)db_column_int(3);
            row->spawn_elem  = (u16)db_column_int(4);
            row->_pad        = 0;
        });
}

/* ====================================================================== */
/*  公開API: システム管理                                                   */
/* ====================================================================== */

int chem_init(const char *db_path)
{
    int rc;

    /* 既に初期化済みの場合はシャットダウンしてから再初期化 */
    if (g_db >= 0) {
        chem_shutdown();
    }

    /* 内部状態クリア */
    memset(g_objects, 0, sizeof(g_objects));
    memset(g_reactions, 0, sizeof(g_reactions));
    memset(g_phases, 0, sizeof(g_phases));
    g_reaction_count = 0;
    g_phase_count = 0;
    g_callback = (chem_reaction_callback)0;
    g_next_id = 1;

    /* DB接続 */
    g_db = db_open(db_path);
    if (g_db < 0) {
        return -1;
    }

    /* ルールキャッシュ読み込み */
    rc = load_reactions();
    if (rc < 0) {
        db_close(g_db);
        g_db = -1;
        return -2;
    }

    rc = load_phases();
    if (rc < 0) {
        db_close(g_db);
        g_db = -1;
        return -3;
    }

    return 0;
}

void chem_shutdown(void)
{
    if (g_db >= 0) {
        db_close(g_db);
        g_db = -1;
    }
    g_reaction_count = 0;
    g_phase_count = 0;
    g_callback = (chem_reaction_callback)0;
}

void chem_reset(void)
{
    memset(g_objects, 0, sizeof(g_objects));
    g_next_id = 1;
}

/* ====================================================================== */
/*  公開API: オブジェクト管理                                               */
/* ====================================================================== */

int chem_spawn(u16 type_id, i16 x, i16 y)
{
    int i;
    int rc;
    ChemObject *obj;

    /* 空きスロット検索 */
    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        if (g_objects[i].id == 0) {
            break;
        }
    }
    if (i >= CHEM_MAX_OBJECTS) {
        return -1;  /* 満杯 */
    }

    obj = &g_objects[i];
    memset(obj, 0, sizeof(ChemObject));
    obj->id = g_next_id++;
    obj->type_id = type_id;
    obj->x = x;
    obj->y = y;

    /* type_id == 0 の場合はデフォルト値で生成 (テンプレートなし) */
    if (type_id == 0) {
        obj->temperature = 20;
        obj->hp = 100;
        return i;
    }

    /* DBからテンプレート読み込み */
    if (g_db >= 0) {
        /* snprintf の代わりに kprintf + 固定SQL を使用 */
        /* type_id は u16 なので最大5桁 */
        char sql[128];
        char *p;

        /* "SELECT elements, temperature, hp FROM object_types WHERE id=" + 数値 */
        p = sql;
        db_sql_append(&p,
            "SELECT elements, temperature, hp "
            "FROM object_types WHERE id=");
        db_sql_append_int(&p, (int)type_id);

        rc = db_query(g_db, sql);
        if (rc == DB_STATUS_ROW) {
            obj->elements    = (u32)db_column_int(0);
            obj->temperature = (i16)db_column_int(1);
            obj->hp          = (i16)db_column_int(2);
        } else {
            /* テンプレートが見つからない場合はデフォルト */
            obj->temperature = 20;
            obj->hp = 100;
        }
    } else {
        obj->temperature = 20;
        obj->hp = 100;
    }

    return i;
}

void chem_destroy(int obj_id)
{
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return;
    memset(&g_objects[obj_id], 0, sizeof(ChemObject));
}

const ChemObject *chem_get(int obj_id)
{
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) {
        return (const ChemObject *)0;
    }
    if (g_objects[obj_id].id == 0) {
        return (const ChemObject *)0;
    }
    return &g_objects[obj_id];
}

void chem_add_element(int obj_id, u32 elem)
{
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return;
    if (g_objects[obj_id].id == 0) return;
    g_objects[obj_id].elements |= elem;
}

void chem_remove_element(int obj_id, u32 elem)
{
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return;
    if (g_objects[obj_id].id == 0) return;
    g_objects[obj_id].elements &= ~elem;
}

void chem_set_temperature(int obj_id, i16 temp)
{
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return;
    if (g_objects[obj_id].id == 0) return;
    g_objects[obj_id].temperature = temp;
}

void chem_add_temperature(int obj_id, i16 delta)
{
    i32 t;
    if (obj_id < 0 || obj_id >= CHEM_MAX_OBJECTS) return;
    if (g_objects[obj_id].id == 0) return;
    /* オーバーフロー防止 */
    t = (i32)g_objects[obj_id].temperature + (i32)delta;
    if (t > 32767) t = 32767;
    if (t < -32768) t = -32768;
    g_objects[obj_id].temperature = (i16)t;
}

/* ====================================================================== */
/*  公開API: クエリ・コールバック                                           */
/* ====================================================================== */

void chem_set_callback(chem_reaction_callback cb)
{
    g_callback = cb;
}

int chem_reaction_count(void)
{
    return g_reaction_count;
}

int chem_phase_count(void)
{
    return g_phase_count;
}

int chem_active_count(void)
{
    int i;
    int count = 0;
    for (i = 0; i < CHEM_MAX_OBJECTS; i++) {
        if (g_objects[i].id != 0) count++;
    }
    return count;
}
