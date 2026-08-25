/* ======================================================================== */
/*  AI_CORE.C — libos32ai 初期化・終了・プロファイル管理                    */
/*                                                                          */
/*  SQLiteからプロファイルをRAMキャッシュにロードし、                        */
/*  ai_decide / ai_weighted_pick に性格パラメータを供給する。                */
/* ======================================================================== */

#include "libos32ai.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include "libos32math.h"
#include <string.h>
#include <stdio.h>

/* ====================================================================== */
/*  内部状態                                                                */
/* ====================================================================== */

static AiProfile g_profiles[AI_MAX_PROFILES];
static u8        g_profile_count;
static int       g_db_slot = -1;   /* libos32db のハンドル (-1=未接続) */
static int       g_initialized;

/* ====================================================================== */
/*  ai_init — ライブラリ初期化                                             */
/*                                                                          */
/*  db_path が NULL の場合は DB なしで動作 (手動プロファイル設定専用)。       */
/*  DB が指定された場合は profiles テーブルから全プロファイルをロードする。   */
/* ====================================================================== */

int ai_init(const char *db_path)
{
    int rc;

    if (g_initialized) {
        return 0;  /* 二重初期化防止 */
    }

    memset(g_profiles, 0, sizeof(g_profiles));
    g_profile_count = 0;
    g_db_slot = -1;

    if (db_path != NULL) {
        db_handle_t h;
        h = db_open(db_path);
        if (h < 0) {
            return -1;  /* DB接続失敗 */
        }
        g_db_slot = h;

        /* profiles テーブルから全行をロード */
        {
            int _rc;
            _rc = db_query(h,
                "SELECT id, p0_miss, p1_noise, p2, p3, p4, p5, p6, p7,"
                " p8, p9, p10, p11, p12, p13, p14, p15"
                " FROM profiles ORDER BY id");
            if (_rc == DB_STATUS_ROW) {
                do {
                    u8 pid;
                    int i;
                    AiProfile *prof;

                    pid = (u8)db_column_int(0);
                    if (pid >= AI_MAX_PROFILES) continue;

                    prof = &g_profiles[pid];
                    for (i = 0; i < AI_PARAM_MAX; i++) {
                        prof->params[i] = (u8)db_column_int(1 + i);
                    }
                    prof->param_count = AI_PARAM_MAX;

                    if (pid >= g_profile_count) {
                        g_profile_count = pid + 1;
                    }
                } while (db_step(h) == DB_STATUS_ROW);
            }
        }
    }

    g_initialized = 1;
    return 0;
}

/* ====================================================================== */
/*  ai_shutdown — ライブラリ終了                                           */
/* ====================================================================== */

void ai_shutdown(void)
{
    if (!g_initialized) return;

    if (g_db_slot >= 0) {
        db_close(g_db_slot);
        g_db_slot = -1;
    }

    memset(g_profiles, 0, sizeof(g_profiles));
    g_profile_count = 0;
    g_initialized = 0;
}

/* ====================================================================== */
/*  ai_load_profile — 指定IDのプロファイルをキャッシュから取得               */
/*                                                                          */
/*  戻り値: 0=成功, -1=範囲外またはプロファイル未登録                        */
/* ====================================================================== */

int ai_load_profile(u8 profile_id, AiProfile *out)
{
    if (profile_id >= AI_MAX_PROFILES || out == NULL) {
        return -1;
    }
    memcpy(out, &g_profiles[profile_id], sizeof(AiProfile));
    return 0;
}

/* ====================================================================== */
/*  ai_profile_count — 登録済みプロファイル数を返す                         */
/* ====================================================================== */

int ai_profile_count(void)
{
    return (int)g_profile_count;
}

/* ====================================================================== */
/*  ai_get_param — プロファイルからパラメータを取得                         */
/*                                                                          */
/*  範囲外のインデックスは 0 を返す。                                       */
/* ====================================================================== */

u8 ai_get_param(const AiProfile *prof, u8 idx)
{
    if (prof == NULL || idx >= AI_PARAM_MAX) {
        return 0;
    }
    return prof->params[idx];
}

/* ====================================================================== */
/*  ai_set_param — プロファイルのパラメータを設定                           */
/*                                                                          */
/*  ランタイムでの性格調整に使用。DB には反映しない (RAM上の値のみ変更)。    */
/* ====================================================================== */

void ai_set_param(AiProfile *prof, u8 idx, u8 value)
{
    if (prof == NULL || idx >= AI_PARAM_MAX) {
        return;
    }
    prof->params[idx] = value;
}

/* ====================================================================== */
/*  ai_update_profile — RAMキャッシュ内のプロファイルを直接更新              */
/*                                                                          */
/*  ゲーム中にAIの性格を動的に変更する場合に使用。                          */
/*  以降の ai_load_profile 呼び出しに反映される。                           */
/*  DBには反映しない (永続化には ai_save_profile を使用)。                   */
/*                                                                          */
/*  戻り値: 0=成功, -1=範囲外                                               */
/* ====================================================================== */

int ai_update_profile(u8 profile_id, const AiProfile *prof)
{
    if (profile_id >= AI_MAX_PROFILES || prof == NULL) {
        return -1;
    }
    memcpy(&g_profiles[profile_id], prof, sizeof(AiProfile));

    /* profile_count を必要に応じて拡張 */
    if (profile_id >= g_profile_count) {
        g_profile_count = profile_id + 1;
    }
    return 0;
}

/* ====================================================================== */
/*  ai_save_profile — RAMキャッシュのプロファイルをDBに書き戻す              */
/*                                                                          */
/*  INSERT OR REPLACE で upsert する。                                      */
/*  DB未接続 (ai_init(NULL) で初期化) の場合は -1 を返す。                  */
/*                                                                          */
/*  戻り値: 0=成功, -1=DB未接続, -2=SQL実行エラー                           */
/* ====================================================================== */

int ai_save_profile(u8 profile_id)
{
    char sql[512];
    const AiProfile *prof;
    int rc;

    if (g_db_slot < 0) return -1;
    if (profile_id >= AI_MAX_PROFILES) return -1;

    prof = &g_profiles[profile_id];

    /* snprintf で UPDATE文を構築
     * INSERT OR REPLACE で存在しない場合も対応
     */
    rc = snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO profiles"
        " (id, name, p0_miss, p1_noise,"
        " p2, p3, p4, p5, p6, p7, p8, p9,"
        " p10, p11, p12, p13, p14, p15)"
        " VALUES (%d, 'profile_%d',"
        " %d, %d, %d, %d, %d, %d, %d, %d, %d, %d,"
        " %d, %d, %d, %d, %d, %d)",
        (int)profile_id, (int)profile_id,
        (int)prof->params[0],  (int)prof->params[1],
        (int)prof->params[2],  (int)prof->params[3],
        (int)prof->params[4],  (int)prof->params[5],
        (int)prof->params[6],  (int)prof->params[7],
        (int)prof->params[8],  (int)prof->params[9],
        (int)prof->params[10], (int)prof->params[11],
        (int)prof->params[12], (int)prof->params[13],
        (int)prof->params[14], (int)prof->params[15]);

    if (rc < 0 || rc >= (int)sizeof(sql)) return -2;

    rc = db_exec(g_db_slot, sql);
    return (rc < 0) ? -2 : 0;
}
