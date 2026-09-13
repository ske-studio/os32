/* ======================================================================== */
/*  DB_V50_TEST.C — KAPI v50 (設定レジストリの基盤) の CPL=3 受入 (票 S0-K K2)*/
/*                                                                          */
/*  ここで踏むのは「CPL=3 から呼んだときだけ分かること」だけ。SQL の意味や    */
/*  診断の保持規則はホスト試験 (tools/tests/test_kapi_db_v50.py) が見る。     */
/*                                                                          */
/*    1. RO で存在しない DB を開いても **ファイルが作られない**              */
/*       (`db_open_existing` は CREATE を付けない)。                         */
/*    2. `db_error_code(-1)` に直前の open 失敗コードが残る。                 */
/*    3. prepare_only + bind + step で 1 行取れる (prepare だけでは進まない)。*/
/*    4. 4096B の blob が往復する。                                          */
/*    5. **不正範囲**のポインタ / 長さが -1 で断られ、この**試験プログラムが  */
/*       落ちない**。落ちたら (fault_kill_count が増えたら) 不合格 —         */
/*       カーネルが写す前に弾けていないということ。                          */
/*                                                                          */
/*  使い方: `db_v50_test [work.db]` (既定 /tmp/db_v50.db)。                   */
/*  合格は最後の "db_v50_test: PASS n/n" と、fault_kill_count が不変なこと。  */
/* ======================================================================== */

#include "os32api.h"

/* CPL=3 アプリの許可帯 (exec/exec.c の ring3_ptr_ok)。ヒープ帯とスタック帯の
 * 間にはガードページがあり、帯の上端の先は写像がない。番地は
 * userland/tests/ring3_guard.c と同じ「アプリ固有 PDE 1 枚」の既定配置 —
 * このプログラムは app.conf でヒープを要求しない (0) ので必ず 1 枚になる。 */
#define BAND_TOP        0x800000UL   /* MEM_APP_BAND_TOP (スタック帯の上端) */
#define HEAP_TOP        0x7BF000UL   /* ガードページの先頭 = ヒープ帯の上端 */
#define VRAM_END        0x0C0000UL   /* 許可帯 [0xA0000, 0xC0000) の末尾 */

static int passed;
static int failed;
static KernelAPI *g;

static void ok(int cond, const char *name)
{
    if (cond) {
        passed++;
    } else {
        failed++;
        g->kprintf(0x41, "  FAIL: %s\n", name);
    }
}

/* 使い捨ての作業 DB を legacy db_open (= CREATE 付き) で用意する。
 * v50 の open は既存 DB しか開けないので、土台はこちらで作る。 */
static int make_fixture(const char *path)
{
    int h = g->db_open(path);
    if (h < 0) return -1;
    if (g->db_exec(h, "DROP TABLE IF EXISTS v50") != 0) { g->db_close(h); return -1; }
    if (g->db_exec(h, "CREATE TABLE v50(k TEXT, n INT, b BLOB)") != 0) {
        g->db_close(h);
        return -1;
    }
    g->db_close(h);
    return 0;
}

int main(int argc, char **argv, KernelAPI *api)
{
    static char blob_out[4096];
    static char blob_in[4096];
    static char no_nul[OS32_MAX_PATH + 64];
    const char *work = "/tmp/db_v50.db";
    const char *missing = "/tmp/db_v50_nosuch.db";
    int h, rc, i, code;

    g = api;
    passed = 0;
    failed = 0;
    if (argc >= 2) work = argv[1];

    api->kprintf(0xE1, "db_v50_test: KAPI v%d\n", (int)api->version);
    if (api->version < 50) {
        api->kprintf(0x41, "db_v50_test: kernel is older than v50\n");
        return 1;
    }

    /* ---- (1) RO で欠損 DB を開いても作られない ------------------------- */
    ok(api->db_open_existing(missing, 0) < 0, "RO open of a missing db fails");
    ok(api->db_open_existing(missing, 1) < 0, "RW open of a missing db fails");
    /* legacy db_open は CREATE 付きなので、作られていたらここで開けてしまう。
     * 「開けない」= ファイルが無い、という確かめ方はできないので stat を使う。*/
    {
        OS32_Stat st;
        ok(api->sys_stat(missing, &st) != 0, "the missing db was not created");
    }

    /* ---- (2) db_error_code(-1) に直前の open 失敗が残る ----------------- */
    code = api->db_error_code(-1);
    ok(code != 0, "db_error_code(-1) keeps the open failure");
    api->kprintf(0x07, "  open failure code = %d\n", code);
    ok(api->db_error_code(999) != 0, "an out-of-range handle is MISUSE");

    /* ---- 作業 DB を用意して RW で開く ---------------------------------- */
    if (make_fixture(work) != 0) {
        api->kprintf(0x41, "db_v50_test: cannot create %s\n", work);
        return 1;
    }
    h = api->db_open_existing(work, 1);
    ok(h >= 0, "RW open of an existing db");
    if (h < 0) {
        api->kprintf(0x41, "db_v50_test: %d/%d passed, aborted\n",
                     passed, passed + failed);
        return 1;
    }
    ok(api->db_error_code(h) == 0, "a fresh handle has no failure");

    /* ---- (5) 不正範囲: 先頭は許可帯の中、範囲が外へ出る ---------------- */
    ok(api->db_prepare_only(h, "INSERT INTO v50(k,n,b) VALUES(?,?,?)") == 0,
       "prepare_only of a single statement");
    ok(api->db_bind_text(h, 1, (const char *)(VRAM_END - 1), 2) < 0,
       "text range crossing the end of a permitted band is refused");
    ok(api->db_bind_text(h, 1, (const char *)(BAND_TOP - 1), 2) < 0,
       "text range crossing the top of the app band is refused");
    ok(api->db_bind_blob(h, 3, (const void *)(HEAP_TOP - 1), 2) < 0,
       "blob range crossing the guard page is refused");
    ok(api->db_bind_text(h, 1, "x", -1) < 0, "a negative length is refused");
    ok(api->db_bind_text(h, 1, (const char *)0, 0) < 0,
       "a NULL text pointer is refused (db_bind_null is the way)");
    ok(api->db_bind_blob(h, 3, blob_in, 4097) < 0, "4097B blob is refused");

    /* NUL の無い path (上限まで探して見つからない)。切り捨てて開かないこと。*/
    for (i = 0; i < (int)sizeof(no_nul); i++) no_nul[i] = 'a';
    ok(api->db_open_existing(no_nul, 0) < 0, "a path without a NUL is refused");

    /* ---- (3)(4) prepare_only + bind + step で 1 行 ---------------------- */
    for (i = 0; i < 4096; i++) blob_in[i] = (char)(i & 0xFF);
    ok(api->db_bind_text(h, 1, "color", 5) == 0, "bind_text");
    ok(api->db_bind_int(h, 2, 12345) == 0, "bind_int");
    ok(api->db_bind_blob(h, 3, blob_in, 4096) == 0, "bind_blob 4096B");
    ok(api->db_bind_int(h, 0, 1) < 0, "index 0 is refused (1-based)");
    ok(api->db_bind_int(h, 9, 1) < 0, "an out-of-range index is refused");
    ok(api->db_step(h) == DB_STATUS_DONE, "step runs the bound INSERT once");

    /* prepare_only は step しない: SELECT の先頭行が進まないこと。 */
    ok(api->db_prepare_only(h, "SELECT n FROM v50 WHERE k = ?") == 0,
       "prepare_only of a SELECT");
    ok(api->db_bind_text(h, 1, "color", 5) == 0, "bind after prepare_only");
    rc = api->db_step(h);
    ok(rc == DB_STATUS_ROW, "the first row arrives only after step");
    ok(api->db_column_int(h, 0) == 12345, "the bound int came back");
    ok(api->db_bind_int(h, 1, 1) < 0, "bind after the first step is refused");
    api->db_finalize(h);

    /* blob の往復 (SHM の 16KB ブロックに 4096B + descriptor が収まる)。 */
    ok(api->db_prepare_only(h, "SELECT b FROM v50 WHERE k = ?") == 0,
       "prepare_only for the blob");
    ok(api->db_bind_text(h, 1, "color", 5) == 0, "bind for the blob");
    if (api->db_step(h) == DB_STATUS_ROW) {
        const char *p = api->db_column_text(h, 0);
        int same = 1;
        for (i = 0; i < 4096; i++) blob_out[i] = p ? p[i] : 0;
        for (i = 0; i < 4096; i++) {
            if (blob_out[i] != blob_in[i]) { same = 0; break; }
        }
        ok(p != 0 && same, "4096B blob roundtrip");
    } else {
        ok(0, "4096B blob roundtrip (no row)");
    }
    api->db_finalize(h);

    /* ---- 複数 statement / 長すぎる SQL は拒否 -------------------------- */
    ok(api->db_prepare_only(h, "SELECT 1; SELECT 2") < 0,
       "a second statement is refused");
    ok(api->db_prepare_only(h, "SELECT 1 -- trailing comment") == 0,
       "a trailing comment is allowed");
    api->db_finalize(h);

    /* ---- 後片付けは診断を消さない -------------------------------------- */
    ok(api->db_prepare_only(h, "SELECT * FROM no_such_table") < 0,
       "prepare_only of a bad table fails");
    code = api->db_error_code(h);
    ok(code != 0, "the failure is kept");
    api->db_finalize(h);
    ok(api->db_error_code(h) == code, "finalize does not clear the diagnosis");
    api->db_close(h);
    ok(api->db_error_code(h) == code, "close does not clear the diagnosis");

    api->kprintf(failed ? 0x41 : 0xA1, "db_v50_test: %s %d/%d\n",
                 failed ? "FAIL" : "PASS", passed, passed + failed);
    return failed ? 1 : 0;
}
