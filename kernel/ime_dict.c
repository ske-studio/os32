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
#include "vfs.h"
#include "utf8.h"
#include "kprintf.h"
#include "serial.h"
#include "kstring.h"
#include "os32_kapi_shared.h"
#include "os32_sqlite_vfs.h"

/* FEP辞書はカーネル常駐の接続なので、基底の VFS fd を exec_exit の
 * FD一括クローズから保護する。これを欠くと外部プログラム (ime.bin 等)
 * の終了時に fd が回収され、以後の検索が全て SQLITE_IOERR になる。 */
static void dict_fd_protect(IME_Dict *dict, int on)
{
    int fd;
    if (!dict || !dict->db) return;
    fd = os32_sqlite_db_fd(dict->db);
    if (fd >= 0) vfs_fd_set_protect(fd, on);
}

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

    /* 常駐接続の fd を exec クリーンアップから保護 */
    dict_fd_protect(dict, 1);

    /* 成功の知らせはシリアルだけに出す。GUI (gshell) 中に SHIFT+SPACE で
     * 初めて辞書を開くと、この行が TVRAM に残って GFX 画面に透けるため
     * (2026-09-06 G3 で実測)。失敗は今までどおり画面にも出す。 */
    serial_puts("IME: Dict loaded (SQLite): ");
    serial_puts(path);
    serial_puts("\r\n");
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
    {
        int step_rc;
        while (count < max_results &&
               (step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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

        /* 診断: step がエラーで終わった場合のみ表示 (SQLITE_DONE は正常) */
        if (count == 0 && step_rc != SQLITE_DONE) {
            kprintf(ATTR_RED,
                    "IME: search 0hit len=%d chars=%d step=%d err=%d ext=%d %s\r\n",
                    yomi_len, yomi_chars, step_rc,
                    sqlite3_errcode((sqlite3 *)dict->db),
                    sqlite3_extended_errcode((sqlite3 *)dict->db),
                    sqlite3_errmsg((sqlite3 *)dict->db));
        }
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

/* 辞書クローズ内部関数 */
static void ime_dict_close(IME_Dict *dict)
{
    if (!dict || !dict->db) return;
    /* 保護を解除してから閉じる (fd スロット再利用に備える) */
    dict_fd_protect(dict, 0);
    if (dict->exact_stmt) {
        sqlite3_finalize((sqlite3_stmt *)dict->exact_stmt);
        dict->exact_stmt = NULL;
    }
    if (dict->prefix_stmt) {
        sqlite3_finalize((sqlite3_stmt *)dict->prefix_stmt);
        dict->prefix_stmt = NULL;
    }
    if (dict->learn_stmt) {
        sqlite3_finalize((sqlite3_stmt *)dict->learn_stmt);
        dict->learn_stmt = NULL;
    }
    sqlite3_close((sqlite3 *)dict->db);
    dict->db = NULL;
}

/* 辞書再オープン */
int ime_dict_reopen(IME_Dict *dict, const char *path)
{
    ime_dict_close(dict);
    return ime_dict_open(dict, path);
}

/* ユーザー学習辞書列挙 */
int ime_user_list(IME_Dict *dict, const char *yomi_prefix,
                  IME_UserEntry *out, int max)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;
    const char *sql;

    if (!dict || !dict->db || !out || max <= 0) return 0;
    db = (sqlite3 *)dict->db;

    if (yomi_prefix && yomi_prefix[0] != '\0') {
        sql = "SELECT yomi, kanji, freq FROM dict_user WHERE yomi >= ?1 AND yomi < ?1 || X'EFBFBF' ORDER BY freq DESC, yomi ASC LIMIT ?2";
    } else {
        sql = "SELECT yomi, kanji, freq FROM dict_user ORDER BY freq DESC, yomi ASC LIMIT ?2";
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: user_list prepare failed (rc=%d)\r\n", rc);
        return 0;
    }

    if (yomi_prefix && yomi_prefix[0] != '\0') {
        sqlite3_bind_text(stmt, 1, yomi_prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, max);
    } else {
        sqlite3_bind_int(stmt, 2, max);
    }

    while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
        IME_UserEntry *e = &out[count];
        const char *y_text = (const char *)sqlite3_column_text(stmt, 0);
        const char *k_text = (const char *)sqlite3_column_text(stmt, 1);
        int freq = sqlite3_column_int(stmt, 2);

        if (y_text) {
            kstrncpy(e->yomi, y_text, sizeof(e->yomi));
        } else {
            e->yomi[0] = '\0';
        }

        if (k_text) {
            kstrncpy(e->kanji, k_text, sizeof(e->kanji));
        } else {
            e->kanji[0] = '\0';
        }

        e->freq = freq;
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ユーザー学習辞書エントリ削除 */
int ime_user_delete(IME_Dict *dict, const char *yomi, const char *kanji)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *sql;

    if (!dict || !dict->db || !yomi || yomi[0] == '\0') return -1;
    db = (sqlite3 *)dict->db;

    if (kanji && kanji[0] != '\0') {
        sql = "DELETE FROM dict_user WHERE yomi = ?1 AND kanji = ?2";
    } else {
        sql = "DELETE FROM dict_user WHERE yomi = ?1";
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: user_delete prepare failed (rc=%d)\r\n", rc);
        return -2;
    }

    sqlite3_bind_text(stmt, 1, yomi, -1, SQLITE_STATIC);
    if (kanji && kanji[0] != '\0') {
        sqlite3_bind_text(stmt, 2, kanji, -1, SQLITE_STATIC);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        kprintf(ATTR_RED, "IME: user_delete execute failed (rc=%d)\r\n", rc);
        return -3;
    }

    return 0;
}

/* ユーザー学習辞書CSVエクスポート */
int ime_user_export(IME_Dict *dict, const char *path)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int fd;
    const char *sql;
    char line[128];

    if (!dict || !dict->db || !path || path[0] == '\0') return -1;
    db = (sqlite3 *)dict->db;

    fd = vfs_open(path, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd < 0) {
        kprintf(ATTR_RED, "IME: export open failed: %s (fd=%d)\r\n", path, fd);
        return -2;
    }

    sql = "SELECT yomi, kanji, freq FROM dict_user ORDER BY freq DESC, yomi ASC";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: export prepare failed (rc=%d)\r\n", rc);
        vfs_close(fd);
        return -3;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *y_text = (const char *)sqlite3_column_text(stmt, 0);
        const char *k_text = (const char *)sqlite3_column_text(stmt, 1);
        int freq = sqlite3_column_int(stmt, 2);

        if (!y_text || !k_text) continue;

        sqlite3_snprintf(sizeof(line), line, "%s,%s,%d\n", y_text, k_text, freq);
        vfs_write_fd(fd, line, (u32)kstrlen(line));
    }

    sqlite3_finalize(stmt);
    vfs_close(fd);
    return 0;
}

/* ユーザー学習辞書全消去 */
int ime_user_clear(IME_Dict *dict)
{
    sqlite3 *db;
    int rc;

    if (!dict || !dict->db) return -1;
    db = (sqlite3 *)dict->db;

    rc = sqlite3_exec(db, "DELETE FROM dict_user;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        kprintf(ATTR_RED, "IME: user_clear execute failed (rc=%d)\r\n", rc);
        return -2;
    }

    return 0;
}
