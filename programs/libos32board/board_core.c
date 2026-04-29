/* ======================================================================== */
/*  BOARD_CORE.C — libos32board 初期化・終了・DBロード                      */
/*                                                                          */
/*  SQLiteからマス・接続・区画をRAMキャッシュにロードする。                  */
/* ======================================================================== */

#include "libos32board.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include <string.h>

/* ====================================================================== */
/*  内部状態                                                                */
/* ====================================================================== */

static BoardMass  g_masses[BOARD_MAX_MASSES];
static int        g_mass_count;
static BoardArea  g_areas[BOARD_MAX_AREAS];
static int        g_area_count;
static int        g_db_slot = -1;
static int        g_initialized;

/* ====================================================================== */
/*  内部アクセサ (他の .c ファイルから参照)                                  */
/* ====================================================================== */

BoardMass *board_get_masses(void)      { return g_masses; }
int        board_get_mass_count(void)  { return g_mass_count; }
void       board_set_mass_count(int n) { g_mass_count = n; }
BoardArea *board_get_areas(void)       { return g_areas; }
int        board_get_area_cnt(void)    { return g_area_count; }
void       board_set_area_cnt(int n)   { g_area_count = n; }

/* マスIDからインデックスを検索 (-1=未発見) */
int board_find_index(u16 id)
{
    return DB_FIND_BY_FIELD(g_masses, g_mass_count, id, id);
}

/* ====================================================================== */
/*  board_init — ライブラリ初期化                                          */
/* ====================================================================== */

int board_init(const char *db_path)
{
    int i;

    if (g_initialized) {
        return 0;  /* 二重初期化防止 */
    }

    memset(g_masses, 0, sizeof(g_masses));
    memset(g_areas, 0, sizeof(g_areas));
    g_mass_count = 0;
    g_area_count = 0;
    g_db_slot = -1;

    /* 接続先を全てセンチネルで初期化 */
    for (i = 0; i < BOARD_MAX_MASSES; i++) {
        int j;
        for (j = 0; j < BOARD_MAX_CONNECT; j++) {
            g_masses[i].connect[j] = BOARD_CONNECT_NONE;
        }
        g_masses[i].trap_owner = 0xFF;
    }

    if (db_path != NULL) {
        db_handle_t h;
        int rc;

        h = db_open(db_path);
        if (h < 0) {
            return -1;  /* DB接続失敗 */
        }
        g_db_slot = h;

        /* マスデータをロード */
        DB_LOAD_TABLE(h,
            "SELECT id, type, area, param, cost, flags, x, y"
            " FROM masses ORDER BY id",
            g_masses, BOARD_MAX_MASSES, g_mass_count,
            {
                row->id    = (u16)db_column_int(0);
                row->type  = (u8)db_column_int(1);
                row->area  = (u8)db_column_int(2);
                row->param = (u16)db_column_int(3);
                row->cost  = (u8)db_column_int(4);
                row->flags = (u8)db_column_int(5);
                row->x     = (i16)db_column_int(6);
                row->y     = (i16)db_column_int(7);
                row->connect_count = 0;
                row->trap_owner = 0xFF;
            });

        /* 接続データをロード */
        rc = db_query(h,
            "SELECT from_id, to_id, bidirectional"
            " FROM connections ORDER BY from_id, to_id");
        if (rc == DB_STATUS_ROW) {
            do {
                u16 from_id = (u16)db_column_int(0);
                u16 to_id   = (u16)db_column_int(1);
                int bidir   = (int)db_column_int(2);
                int fi = board_find_index(from_id);

                /* from -> to を追加 */
                if (fi >= 0 && g_masses[fi].connect_count < BOARD_MAX_CONNECT) {
                    g_masses[fi].connect[g_masses[fi].connect_count] = to_id;
                    g_masses[fi].connect_count++;
                }

                /* 双方向の場合 to -> from も追加 */
                if (bidir) {
                    int ti = board_find_index(to_id);
                    if (ti >= 0 && g_masses[ti].connect_count < BOARD_MAX_CONNECT) {
                        g_masses[ti].connect[g_masses[ti].connect_count] = from_id;
                        g_masses[ti].connect_count++;
                    }
                }
            } while (db_step(h) == DB_STATUS_ROW);
        }

        /* 区画データをロード */
        DB_LOAD_TABLE_OPT(h,
            "SELECT id, unlock_type, unlock_param"
            " FROM areas ORDER BY id",
            g_areas, BOARD_MAX_AREAS, g_area_count,
            {
                row->id = (u8)db_column_int(0);
                row->unlock_type  = (u8)db_column_int(1);
                row->unlock_param = (u8)db_column_int(2);
                /* unlock_type=0 は初期解放 */
                row->unlocked = (row->unlock_type == 0) ? 1 : 0;
            });
    }

    g_initialized = 1;
    return 0;
}

/* ====================================================================== */
/*  board_shutdown — ライブラリ終了                                        */
/* ====================================================================== */

void board_shutdown(void)
{
    int i;

    if (!g_initialized) return;

    if (g_db_slot >= 0) {
        db_close(g_db_slot);
        g_db_slot = -1;
    }

    memset(g_masses, 0, sizeof(g_masses));
    memset(g_areas, 0, sizeof(g_areas));

    /* 接続先を全てセンチネルに戻す */
    for (i = 0; i < BOARD_MAX_MASSES; i++) {
        int j;
        for (j = 0; j < BOARD_MAX_CONNECT; j++) {
            g_masses[i].connect[j] = BOARD_CONNECT_NONE;
        }
        g_masses[i].trap_owner = 0xFF;
    }

    g_mass_count = 0;
    g_area_count = 0;
    g_initialized = 0;
}

/* ====================================================================== */
/*  board_reset — ランタイム状態のみリセット                                */
/* ====================================================================== */

void board_reset(void)
{
    int i;
    if (!g_initialized) return;

    for (i = 0; i < g_mass_count; i++) {
        /* フラグをDB初期値に戻す (DB再ロードが理想だが、簡易的に0クリア) */
        g_masses[i].flags = BOARD_FLAG_NONE;
        g_masses[i].trap_owner = 0xFF;
    }

    /* 区画のロック状態をリセット */
    for (i = 0; i < g_area_count; i++) {
        g_areas[i].unlocked = (g_areas[i].unlock_type == 0) ? 1 : 0;
    }
}
