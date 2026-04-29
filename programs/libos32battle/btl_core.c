/* ======================================================================== */
/*  BTL_CORE.C — libos32battle 初期化・終了・DBロード                       */
/*                                                                          */
/*  SQLiteからコマンドマトリクス・属性相性・状態異常定義を                   */
/*  RAMキャッシュにロードする。                                              */
/* ======================================================================== */

#include "libos32battle.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include "libos32math.h"
#include <string.h>

/* ====================================================================== */
/*  内部状態                                                                */
/* ====================================================================== */

static BtlCommandMatrix  g_matrix;
static BtlElementEntry   g_elements[BTL_ELEM_PAIRS_MAX];
static int               g_element_count;
static BtlStatusDef      g_status_defs[BTL_STATUS_DEF_MAX];
static int               g_status_def_count;
static int               g_db_slot = -1;
static int               g_initialized;

/* ダメージ計算ポリシー (btl_calc.c で参照する外部宣言) */
btl_damage_fn g_damage_policy = NULL;
btl_damage_fn g_magic_policy  = NULL;

/* 結果コールバック (btl_resolve.c で参照する外部宣言) */
btl_result_cb g_result_callback = NULL;

/* ====================================================================== */
/*  内部アクセサ (他の .c ファイルから参照)                                  */
/* ====================================================================== */

BtlCommandMatrix *btl_get_matrix(void)        { return &g_matrix; }
BtlElementEntry  *btl_get_elements(void)      { return g_elements; }
int               btl_get_element_count(void) { return g_element_count; }
BtlStatusDef     *btl_get_status_defs(void)   { return g_status_defs; }
int               btl_get_status_def_count(void) { return g_status_def_count; }

/* ====================================================================== */
/*  btl_init — ライブラリ初期化                                            */
/* ====================================================================== */

int btl_init(const char *db_path)
{
    if (g_initialized) {
        return 0;  /* 二重初期化防止 */
    }

    memset(&g_matrix, 0, sizeof(g_matrix));
    memset(g_elements, 0, sizeof(g_elements));
    memset(g_status_defs, 0, sizeof(g_status_defs));
    g_element_count = 0;
    g_status_def_count = 0;
    g_db_slot = -1;
    g_damage_policy = NULL;
    g_magic_policy  = NULL;
    g_result_callback = NULL;

    if (db_path != NULL) {
        db_handle_t h;
        int rc;

        h = db_open(db_path);
        if (h < 0) {
            return -1;  /* DB接続失敗 */
        }
        g_db_slot = h;

        /* コマンドマトリクスをロード */
        {
            int _rc;
            _rc = db_query(h,
                "SELECT atk_cmd, def_cmd, result_type"
                " FROM command_matrix ORDER BY atk_cmd, def_cmd");
            if (_rc == DB_STATUS_ROW) {
                do {
                    u8 ac, dc, rt;
                    ac = (u8)db_column_int(0);
                    dc = (u8)db_column_int(1);
                    rt = (u8)db_column_int(2);

                    if (ac < BTL_CMD_MAX && dc < BTL_CMD_MAX) {
                        g_matrix.matrix[ac][dc] = rt;
                        if (ac + 1 > g_matrix.atk_count) {
                            g_matrix.atk_count = ac + 1;
                        }
                        if (dc + 1 > g_matrix.def_count) {
                            g_matrix.def_count = dc + 1;
                        }
                    }
                } while (db_step(h) == DB_STATUS_ROW);
            }
        }

        /* 属性相性テーブルをロード */
        DB_LOAD_TABLE_OPT(h,
            "SELECT elem_atk, elem_def, multiplier"
            " FROM element_chart",
            g_elements, BTL_ELEM_PAIRS_MAX, g_element_count,
            {
                row->elem_atk   = (u32)db_column_int(0);
                row->elem_def   = (u32)db_column_int(1);
                row->multiplier = (i16)db_column_int(2);
            });

        /* 状態異常定義をロード */
        DB_LOAD_TABLE_OPT(h,
            "SELECT bit_flag, duration, tick_damage, prevents_action"
            " FROM status_effects ORDER BY id",
            g_status_defs, BTL_STATUS_DEF_MAX, g_status_def_count,
            {
                row->bit_flag = (u32)db_column_int(0);
                row->duration = (u8)db_column_int(1);
                row->tick_damage = (i16)db_column_int(2);
                row->prevents_action = (u8)db_column_int(3);
            });
    }

    g_initialized = 1;
    return 0;
}

/* ====================================================================== */
/*  btl_shutdown — ライブラリ終了                                          */
/* ====================================================================== */

void btl_shutdown(void)
{
    if (!g_initialized) return;

    if (g_db_slot >= 0) {
        db_close(g_db_slot);
        g_db_slot = -1;
    }

    memset(&g_matrix, 0, sizeof(g_matrix));
    memset(g_elements, 0, sizeof(g_elements));
    memset(g_status_defs, 0, sizeof(g_status_defs));
    g_element_count = 0;
    g_status_def_count = 0;
    g_damage_policy = NULL;
    g_magic_policy  = NULL;
    g_result_callback = NULL;
    g_initialized = 0;
}
