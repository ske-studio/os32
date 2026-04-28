/* ======================================================================== */
/*  IME_DICT.C - FEP辞書検索エンジン (SQLite ベース)                         */
/*                                                                          */
/*  カーネルVFS上のSQLite DBファイルを辞書バックエンドとして使用する。        */
/*  sqlite3_*関数をカーネル空間から直接呼び出す (KAPI不使用)。               */
/*                                                                          */
/*  検索モード:                                                              */
/*    - 2文字以下: 完全一致 (変換キー押下時のみ呼ばれる前提)                 */
/*    - 3文字以上: 前方一致 (範囲検索でインデックス活用)                     */
/* ======================================================================== */

#include "ime.h"
#include "sqlite3.h"
#include "utf8.h"
#include "kprintf.h"
#include "kstring.h"
#include "os32_kapi_shared.h"

/* プリペアドステートメントのSQL */
static const char SQL_EXACT[] =
    "SELECT kanji, pos_id, cost FROM ("
    "  SELECT kanji, 0 as pos_id, (-10000 - freq) as cost FROM dict_user WHERE yomi = ?1"
    "  UNION ALL "
    "  SELECT kanji, pos_id, cost FROM dict WHERE yomi = ?1"
    ") ORDER BY cost LIMIT 32";

static const char SQL_PREFIX[] =
    "SELECT kanji, pos_id, cost FROM ("
    "  SELECT kanji, 0 as pos_id, (-10000 - freq) as cost"
    "    FROM dict_user WHERE yomi >= ?1 AND yomi < ?1 || X'EFBFBF'"
    "  UNION ALL "
    "  SELECT kanji, pos_id,"
    "    CASE WHEN yomi = ?1 THEN cost - 500 ELSE cost END as cost"
    "    FROM dict WHERE yomi >= ?1 AND yomi < ?1 || X'EFBFBF'"
    ") ORDER BY cost LIMIT 32";

static const char SQL_LEARN[] =
    "INSERT INTO dict_user (yomi, kanji, freq, last_ts) "
    "VALUES (?1, ?2, 1, ?3) "
    "ON CONFLICT(yomi, kanji) DO UPDATE SET "
    "freq = freq + 1, last_ts = ?3";

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

int ime_dict_open(IME_Dict *dict, const char *path)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    kmemset(dict, 0, sizeof(IME_Dict));

    /* DBファイルを読み書き可能で開く (ユーザー辞書のため) */
    rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, (const char *)0);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: sqlite3_open failed: %s (rc=%d)\r\n",
                path, rc);
        if (db) sqlite3_close(db);
        return -1;
    }
    dict->db = (void *)db;

    /* 学習用テーブルの作成 (存在しない場合) */
    rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS dict_user ("
        "  yomi TEXT NOT NULL,"
        "  kanji TEXT NOT NULL,"
        "  freq INTEGER DEFAULT 1,"
        "  last_ts INTEGER DEFAULT 0,"
        "  PRIMARY KEY (yomi, kanji)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_user_yomi ON dict_user(yomi);",
        0, 0, 0);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: create user dict failed (rc=%d)\r\n", rc);
        /* 学習はできないが検索は続行するためエラーにはしない */
    }

    /* 完全一致検索用プリペアドステートメント */
    rc = sqlite3_prepare_v2(db, SQL_EXACT, -1, &stmt, (const char **)0);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: prepare exact failed (rc=%d)\r\n", rc);
        sqlite3_close(db);
        dict->db = (void *)0;
        return -2;
    }
    dict->exact_stmt = (void *)stmt;

    /* 前方一致検索用プリペアドステートメント */
    rc = sqlite3_prepare_v2(db, SQL_PREFIX, -1, &stmt, (const char **)0);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: prepare prefix failed (rc=%d)\r\n", rc);
        sqlite3_finalize((sqlite3_stmt *)dict->exact_stmt);
        sqlite3_close(db);
        dict->db = (void *)0;
        dict->exact_stmt = (void *)0;
        return -3;
    }
    dict->prefix_stmt = (void *)stmt;

    /* 学習(UPSERT)用プリペアドステートメント */
    rc = sqlite3_prepare_v2(db, SQL_LEARN, -1, &stmt, (const char **)0);
    if (rc == SQLITE_OK) {
        dict->learn_stmt = (void *)stmt;
    } else {
        kprintf(ATTR_RED, "IME: prepare learn failed (rc=%d)\r\n", rc);
        dict->learn_stmt = (void *)0;
    }

    kprintf(ATTR_GREEN, "IME: Dict loaded (SQLite): %s\r\n", path);
    return 0;
}

int ime_dict_search(IME_Dict *dict, const char *yomi,
                    IME_Result *results, int max_results)
{
    sqlite3_stmt *stmt;
    int yomi_chars;
    int count;
    int yomi_len;
    const char *kanji_text;
    int copy_len;

    if (!dict || !dict->db || !yomi || !results || max_results <= 0)
        return 0;
    if (yomi[0] == '\0') return 0;

    /* 最大取得数を制限 */
    if (max_results > IME_MAX_RESULTS)
        max_results = IME_MAX_RESULTS;

    /* UTF-8 文字数に応じてクエリを切り替え */
    yomi_chars = utf8_strlen((const u8 *)yomi);
    yomi_len = (int)kstrlen(yomi);

    if (yomi_chars <= 2) {
        /* 2文字以下: 完全一致のみ */
        stmt = (sqlite3_stmt *)dict->exact_stmt;
    } else {
        /* 3文字以上: 前方一致 */
        stmt = (sqlite3_stmt *)dict->prefix_stmt;
    }

    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, yomi, yomi_len, SQLITE_STATIC);

    /* 結果取得ループ */
    count = 0;
    while (count < max_results && sqlite3_step(stmt) == SQLITE_ROW) {
        IME_Result *r = &results[count];

        /* 漢字 (カラム0) */
        kanji_text = (const char *)sqlite3_column_text(stmt, 0);
        if (!kanji_text) continue;
        copy_len = (int)kstrlen(kanji_text);
        if (copy_len > 31) copy_len = 31;
        kmemcpy(r->kanji, kanji_text, copy_len);
        r->kanji[copy_len] = '\0';

        /* 読みをコピー */
        copy_len = yomi_len < 31 ? yomi_len : 31;
        kmemcpy(r->yomi, yomi, copy_len);
        r->yomi[copy_len] = '\0';

        /* 品詞ID (カラム1) */
        r->pos_id = (u16)sqlite3_column_int(stmt, 1);

        /* コスト (カラム2) */
        r->cost = sqlite3_column_int(stmt, 2);

        /* ひらがな/カタカナそのまま表記に対する動的ペナルティ */
        if (r->cost >= 0 && kstrlen(r->kanji) >= (u32)yomi_len) {
            r->cost += 300;
        }

        count++;
    }

    return count;
}

void ime_dict_learn(IME_Dict *dict, const char *yomi, const char *kanji)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!dict || !dict->db || !dict->learn_stmt || !yomi || !kanji)
        return;

    stmt = (sqlite3_stmt *)dict->learn_stmt;
    sqlite3_reset(stmt);

    sqlite3_bind_text(stmt, 1, yomi, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kanji, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, 0 /* sys_time() is not easily available here, use 0 for now */);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        kprintf(ATTR_RED, "IME: Learn failed (rc=%d)\r\n", rc);
    }
}
