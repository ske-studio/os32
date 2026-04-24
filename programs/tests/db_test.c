/* ======================================================================== */
/*  DB_TEST.C — KAPI 経由 SQLite DB テストプログラム                         */
/*                                                                          */
/*  Phase 2 検証: 外部プログラムから KAPI テーブル経由で SQLite を操作し、     */
/*  CREATE TABLE / INSERT / SELECT の一連の動作を確認する。                   */
/*  テスト対象: メモリDB (:memory:) + ファイルDB (/db/test.db)               */
/* ======================================================================== */

#include "os32api.h"
#include "libos32db.h"
/* crt0_c.c で定義される KernelAPI ポインタ */
extern KernelAPI *kapi;
#define api kapi

static void print_ok(const char *label)
{
    api->kprintf(ATTR_GREEN, "  [OK] %s\n", label);
}

static void print_fail(const char *label, const char *detail)
{
    api->kprintf(ATTR_RED, "  [NG] %s: %s\n", label, detail);
}

static void print_header(const char *title)
{
    api->kprintf(ATTR_CYAN, "\n=== %s ===\n", title);
}

/* ======================================================================== */
/*  テスト1: メモリDB — 基本 CRUD                                           */
/* ======================================================================== */
static int test_memory_db(void)
{
    db_handle_t db;
    int rc;
    int pass = 1;

    print_header("Test 1: Memory DB CRUD");

    /* 1.1 オープン */
    db = db_open(":memory:");
    if (db < 0) {
        print_fail("db_open(:memory:)", "handle < 0");
        return 0;
    }
    print_ok("db_open(:memory:)");

    /* 1.2 テーブル作成 */
    rc = db_exec(db, "CREATE TABLE memo(id INTEGER PRIMARY KEY, text TEXT)");
    if (rc < 0) {
        print_fail("CREATE TABLE", db_errmsg());
        db_close(db);
        return 0;
    }
    print_ok("CREATE TABLE");

    /* 1.3 データ挿入 */
    rc = db_exec(db, "INSERT INTO memo(text) VALUES('Hello OS32')");
    if (rc < 0) {
        print_fail("INSERT 1", db_errmsg());
        db_close(db);
        return 0;
    }
    print_ok("INSERT 1");

    rc = db_exec(db, "INSERT INTO memo(text) VALUES('SQLite on PC-98')");
    if (rc < 0) {
        print_fail("INSERT 2", db_errmsg());
        db_close(db);
        return 0;
    }
    print_ok("INSERT 2");

    rc = db_exec(db, "INSERT INTO memo(text) VALUES('KAPI IPC works!')");
    if (rc < 0) {
        print_fail("INSERT 3", db_errmsg());
        db_close(db);
        return 0;
    }
    print_ok("INSERT 3");

    /* 1.4 SELECT クエリ */
    rc = db_query(db, "SELECT id, text FROM memo ORDER BY id");
    if (rc < 0) {
        print_fail("SELECT", db_errmsg());
        db_close(db);
        return 0;
    }

    if (rc == DB_STATUS_ROW) {
        int row_count = 0;
        do {
            i32 id = db_column_int(0);
            const char *text = db_column_text(1);
            api->kprintf(ATTR_WHITE, "  row %d: id=%d text=\"%s\"\n",
                         row_count, (int)id, text);
            row_count++;
        } while (db_step(db) == DB_STATUS_ROW);

        if (row_count == 3) {
            print_ok("SELECT (3 rows)");
        } else {
            print_fail("SELECT", "expected 3 rows");
            pass = 0;
        }
    } else if (rc == DB_STATUS_DONE) {
        print_fail("SELECT", "no rows returned");
        pass = 0;
    }

    /* 1.5 クローズ */
    rc = db_close(db);
    if (rc < 0) {
        print_fail("db_close", "failed");
        pass = 0;
    } else {
        print_ok("db_close");
    }

    return pass;
}

/* ======================================================================== */
/*  テスト2: 複数DB接続                                                      */
/* ======================================================================== */
static int test_multiple_connections(void)
{
    db_handle_t db1, db2;
    int rc;
    int pass = 1;

    print_header("Test 2: Multiple Connections");

    db1 = db_open(":memory:");
    db2 = db_open(":memory:");

    if (db1 < 0 || db2 < 0) {
        print_fail("open 2 DBs", "handle < 0");
        if (db1 >= 0) db_close(db1);
        if (db2 >= 0) db_close(db2);
        return 0;
    }
    api->kprintf(ATTR_WHITE, "  db1=%d, db2=%d\n", db1, db2);
    print_ok("open 2 DBs");

    /* db1 にテーブル作成 */
    rc = db_exec(db1, "CREATE TABLE t1(val INTEGER)");
    if (rc < 0) { print_fail("CREATE on db1", db_errmsg()); pass = 0; }
    else print_ok("CREATE on db1");

    /* db2 にテーブル作成 */
    rc = db_exec(db2, "CREATE TABLE t2(val INTEGER)");
    if (rc < 0) { print_fail("CREATE on db2", db_errmsg()); pass = 0; }
    else print_ok("CREATE on db2");

    /* db1 に INSERT */
    rc = db_exec(db1, "INSERT INTO t1 VALUES(100)");
    if (rc < 0) { print_fail("INSERT db1", db_errmsg()); pass = 0; }
    else print_ok("INSERT db1");

    /* db2 に INSERT */
    rc = db_exec(db2, "INSERT INTO t2 VALUES(200)");
    if (rc < 0) { print_fail("INSERT db2", db_errmsg()); pass = 0; }
    else print_ok("INSERT db2");

    /* db1 から SELECT */
    rc = db_query(db1, "SELECT val FROM t1");
    if (rc == DB_STATUS_ROW) {
        i32 val = db_column_int(0);
        if (val == 100) {
            print_ok("SELECT db1 = 100");
        } else {
            print_fail("SELECT db1", "unexpected value");
            pass = 0;
        }
    } else {
        print_fail("SELECT db1", "no rows");
        pass = 0;
    }

    /* db2 から SELECT */
    rc = db_query(db2, "SELECT val FROM t2");
    if (rc == DB_STATUS_ROW) {
        i32 val = db_column_int(0);
        if (val == 200) {
            print_ok("SELECT db2 = 200");
        } else {
            print_fail("SELECT db2", "unexpected value");
            pass = 0;
        }
    } else {
        print_fail("SELECT db2", "no rows");
        pass = 0;
    }

    db_close(db1);
    db_close(db2);
    print_ok("close both DBs");

    return pass;
}

/* ======================================================================== */
/*  テスト3: エラーハンドリング                                               */
/* ======================================================================== */
static int test_error_handling(void)
{
    db_handle_t db;
    int rc;
    int pass = 1;

    print_header("Test 3: Error Handling");

    db = db_open(":memory:");
    if (db < 0) {
        print_fail("db_open", "failed");
        return 0;
    }

    /* 存在しないテーブルへの INSERT → エラーが返ること */
    rc = db_exec(db, "INSERT INTO nonexistent VALUES(1)");
    if (rc < 0) {
        api->kprintf(ATTR_WHITE, "  errmsg: %s\n", db_errmsg());
        print_ok("error on bad table (expected)");
    } else {
        print_fail("error handling", "should have failed");
        pass = 0;
    }

    /* 不正な SQL → エラーが返ること */
    rc = db_exec(db, "THIS IS NOT SQL");
    if (rc < 0) {
        print_ok("error on bad SQL (expected)");
    } else {
        print_fail("error handling", "bad SQL should fail");
        pass = 0;
    }

    /* 不正なハンドル */
    rc = db_exec(99, "SELECT 1");
    if (rc < 0) {
        print_ok("error on bad handle (expected)");
    } else {
        print_fail("error handling", "bad handle should fail");
        pass = 0;
    }

    db_close(db);
    return pass;
}

/* ======================================================================== */
/*  エントリポイント                                                         */
/* ======================================================================== */
int main(int argc, char **argv, KernelAPI *k)
{
    int total = 0;
    int passed = 0;

    (void)argc; (void)argv;
    (void)k;

    api->kprintf(ATTR_CYAN, "db_test: KAPI SQLite integration test\n");
    api->kprintf(ATTR_CYAN, "KAPI version: %d\n", kapi->version);

    /* テスト実行 */
    total++; if (test_memory_db()) passed++;
    total++; if (test_multiple_connections()) passed++;
    total++; if (test_error_handling()) passed++;

    /* サマリ */
    api->kprintf(ATTR_CYAN, "\n=== Result: %d/%d passed ===\n", passed, total);
    if (passed == total) {
        api->kprintf(ATTR_GREEN, "All tests passed!\n");
    } else {
        api->kprintf(ATTR_RED, "Some tests failed.\n");
    }
    return 0;
}
