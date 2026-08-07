/* ======================================================================== */
/*  RPG_CORE.C — libos32rpg 初期化・終了・データロード                      */
/* ======================================================================== */

#include "libos32rpg.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include <string.h>

#define RPG_MAX_CLASSES    16
#define RPG_MAX_STATUS_DEF 16
#define RPG_MAX_REBORN_DEF 8

/* 職業成長定義（キャッシュ） */
typedef struct {
    u8   class_id;
    i16  atk, def, spd, mag;
    i16  hp;
    u8   free_points;
} RpgLevelGrowth;

/* 状態定義（キャッシュ） */
typedef struct {
    u32  bit_flag;
    u8   prevents_action;
    u8   tick_kind;         /* 0=なし, 1=固定, 2=レベル比例, 3=最大HP% */
    i16  tick_value;
    u8   recovery_pct;
    u8   lethal;
} RpgStatusFieldDef;

/* リボーンターン定義（キャッシュ） */
typedef struct {
    u8   rank_bucket;
    u8   min_turns;
    u8   max_turns;
} RpgRebornDef;

RpgLevelGrowth    g_growths[RPG_MAX_CLASSES];
int               g_growth_count;
RpgStatusFieldDef g_statuses[RPG_MAX_STATUS_DEF];
int               g_status_count;
RpgRebornDef      g_reborns[RPG_MAX_REBORN_DEF];
int               g_reborn_count;

static int               g_db_slot = -1;
static int               g_initialized;

/* ====================================================================== */
/*  内部ヘルパー (クエリ・操作用)                                          */
/* ====================================================================== */
const RpgStatusFieldDef *rpg_find_status_def(u32 bit)
{
    int i;
    for (i = 0; i < g_status_count; i++) {
        if (g_statuses[i].bit_flag == bit) {
            return &g_statuses[i];
        }
    }
    return (const RpgStatusFieldDef *)0;
}

const RpgRebornDef *rpg_find_reborn_def(u8 rank_bucket)
{
    int i;
    for (i = 0; i < g_reborn_count; i++) {
        if (g_reborns[i].rank_bucket == rank_bucket) {
            return &g_reborns[i];
        }
    }
    return (const RpgRebornDef *)0;
}

/* ====================================================================== */
/*  rpg_init — ライブラリ初期化                                            */
/* ====================================================================== */
int rpg_init(const char *db_path)
{
    if (g_initialized) {
        return 0;
    }

    memset(g_growths, 0, sizeof(g_growths));
    memset(g_statuses, 0, sizeof(g_statuses));
    memset(g_reborns, 0, sizeof(g_reborns));
    g_growth_count = 0;
    g_status_count = 0;
    g_reborn_count = 0;
    g_db_slot = -1;

    if (db_path != (const char *)0) {
        db_handle_t h = db_open(db_path);
        if (h < 0) {
            return -1;
        }
        g_db_slot = h;

        /* 成長データのロード */
        DB_LOAD_TABLE(h,
            "SELECT class_id, atk, def, spd, mag, hp, free_points FROM level_growth ORDER BY class_id",
            g_growths, RPG_MAX_CLASSES, g_growth_count,
            {
                row->class_id    = (u8)db_column_int(0);
                row->atk         = (i16)db_column_int(1);
                row->def         = (i16)db_column_int(2);
                row->spd         = (i16)db_column_int(3);
                row->mag         = (i16)db_column_int(4);
                row->hp          = (i16)db_column_int(5);
                row->free_points = (u8)db_column_int(6);
            });

        /* 状態異常定義のロード */
        DB_LOAD_TABLE_OPT(h,
            "SELECT bit_flag, prevents_action, tick_kind, tick_value, recovery_pct, lethal FROM status_field ORDER BY bit_flag",
            g_statuses, RPG_MAX_STATUS_DEF, g_status_count,
            {
                row->bit_flag        = (u32)db_column_int(0);
                row->prevents_action = (u8)db_column_int(1);
                row->tick_kind       = (u8)db_column_int(2);
                row->tick_value      = (i16)db_column_int(3);
                row->recovery_pct    = (u8)db_column_int(4);
                row->lethal          = (u8)db_column_int(5);
            });

        /* リボーンテーブルのロード */
        DB_LOAD_TABLE_OPT(h,
            "SELECT rank_bucket, min_turns, max_turns FROM reborn_table ORDER BY rank_bucket",
            g_reborns, RPG_MAX_REBORN_DEF, g_reborn_count,
            {
                row->rank_bucket = (u8)db_column_int(0);
                row->min_turns   = (u8)db_column_int(1);
                row->max_turns   = (u8)db_column_int(2);
            });
    }

    g_initialized = 1;
    return 0;
}

/* ====================================================================== */
/*  rpg_shutdown — ライブラリ終了                                          */
/* ====================================================================== */
void rpg_shutdown(void)
{
    if (!g_initialized) return;

    if (g_db_slot >= 0) {
        db_close(g_db_slot);
        g_db_slot = -1;
    }

    memset(g_growths, 0, sizeof(g_growths));
    memset(g_statuses, 0, sizeof(g_statuses));
    memset(g_reborns, 0, sizeof(g_reborns));
    g_growth_count = 0;
    g_status_count = 0;
    g_reborn_count = 0;
    g_initialized = 0;
}

/* ====================================================================== */
/*  rpg_actor_init — アクター初期化                                        */
/* ====================================================================== */
void rpg_actor_init(RpgActor *a, u8 class_id)
{
    int i;
    int idx = -1;

    if (a == (RpgActor *)0) return;

    memset(a, 0, sizeof(RpgActor));

    /* 職業/氏神に応じた成長設定を検索 */
    for (i = 0; i < g_growth_count; i++) {
        if (g_growths[i].class_id == class_id) {
            idx = i;
            break;
        }
    }

    a->level = 1;
    a->class_id = class_id;
    a->exp = 0;

    /* 基本初期値 */
    a->atk = 10;
    a->def = 10;
    a->spd = 10;
    a->mag = 10;
    a->hp  = 50;

    if (idx >= 0) {
        a->atk += g_growths[idx].atk;
        a->def += g_growths[idx].def;
        a->spd += g_growths[idx].spd;
        a->mag += g_growths[idx].mag;
        a->hp  += g_growths[idx].hp;
    }
    a->max_hp = a->hp;

    a->status = 0;
    a->dead_turns = 0;
    a->fled = 0;
    a->pending_points = 0;
}
