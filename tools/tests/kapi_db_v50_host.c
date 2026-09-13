/* ========================================================================= */
/*  KAPI_DB_V50_HOST.C — 票 S0-K §1d のホスト TDD                            */
/*                                                                           */
/*  実物だけを組む: 実 `kapi/kapi_db.c` + 実 `lib/sqlite3/sqlite3.c` +        */
/*  実 `lib/sqlite3/os32_sqlite_vfs.c` + 実 `fs/vfs_fd.c` + RAM バックエンド。 */
/*  模型にするのは exec 側のポインタ検証 (`ring3_user_range_ok`) と           */
/*  SHM の置き場だけ — どちらもカーネル番地に依存してホストでは動かせない。    */
/*  ホストのファイルシステムには 1 バイトも触らない。                          */
/*                                                                           */
/*  実行: python3 -B tools/tests/test_kapi_db_v50.py                         */
/* ========================================================================= */

#define main f2a_main
#include "vfs_fd_sqlite_host.c"
#undef main

/* ホストの size_t はカーネルの u32 と違うので libc の実体を使い回す。 */
#define __KSTRING_H
char *kstrncpy(char *dst, const char *src, u32 size)
{ str_cpy(dst, src, (int)size); return dst; }
char *kstrncat(char *dst, const char *src, u32 size)
{
    u32 used = (u32)strlen(dst);
    if (used + 1u < size) str_cpy(dst + used, src, (int)(size - used));
    return dst;
}
u32 kstrlen(const char *s) { return (u32)strlen(s); }
int kstrcmp(const char *a, const char *b) { return strcmp(a, b); }
int kstrncmp(const char *a, const char *b, u32 n) { return strncmp(a, b, n); }
void *kmemcpy(void *dst, const void *src, u32 n) { return memcpy(dst, src, n); }
void *kmemset(void *dst, int val, u32 n) { return memset(dst, val, n); }

#include "../../lib/sqlite3/sqlite3.h"
#include "../../lib/sqlite3/os32_sqlite_vfs.c"
#include "sqlite_groups_backend.h"

volatile u32 tick_count;
u32 sys_time(void) { return 0; }
void kprintf(u8 color, const char *fmt, ...) { (void)color; (void)fmt; }
int vfs_sync(void) { probes++; return 0; }
int vfs_rm(const char *path) { probes++; return fixture_rm(path); }
int vfs_stat(const char *path, OS32_Stat *st)
{
    FixtureFile *f = fixture_find(path, 0);
    probes++;
    if (!f) return OS32_ERR_NOTFOUND;
    if (st) { memset(st, 0, sizeof(*st)); st->st_size = f->size; }
    return 0;
}

/* ---- exec 側の模型 (kapi_db.c が唯一使う口) ---------------------------- */
/* 0 = CPL=0 の直呼び (素通し) / 1 = CPL=3 由来 (帯と PTE を見る)。
 * 帯は [BAND_LO, BAND_HI) の 1 本にして、そのうち GUARD_PAGE だけを
 * 「許可帯の中だが非 present」= 実機のガード / 未マップに見立てる。 */
#define HOST_PAGE      4096u
#define BAND_LO        0x00400000u
#define BAND_HI        0x00800000u
#define GUARD_PAGE     0x007BF000u
static int host_cpl3;
static u32 host_range_calls;

int ring3_user_range_ok(u32 p, u32 len)
{
    u32 page, last;
    host_range_calls++;
    if (!host_cpl3) return 1;
    if (p == 0) return 0;
    if (len == 0) return 1;
    if (p + len < p) return 0;
    last = (p + len - 1u) & ~(HOST_PAGE - 1u);
    for (page = p & ~(HOST_PAGE - 1u); ; page += HOST_PAGE) {
        if (page < BAND_LO || page >= BAND_HI) return 0;
        if (page == GUARD_PAGE) return 0;           /* 非 present */
        if (page >= last) break;
    }
    return 1;
}

/* SHM の置き場。末尾に番兵を置いて「16KB を 1 バイトも越えない」を見る。 */
#define SHM_CANARY 256
static unsigned char test_shm[DB_SHM_BLOCK_SIZE + SHM_CANARY];
#include "../../kapi/kapi_db.c"

static int cases_run;

static void reset_all(void)
{
    int i;
    memset(test_shm, 0, sizeof(test_shm));
    memset(db_slots, 0, sizeof(db_slots));
    memset(db_open_fail, 0, sizeof(db_open_fail));
    memset(fixture_files, 0, sizeof(fixture_files));
    for (i = 0; i < VFS_MAX_OPEN_FILES; i++) open_files[i].in_use = 0;
    host_cpl3 = 0;
    resolve_owner = current_owner = 2;
    fixture_init();
}

static void canary_check(const char *where)
{
    int i;
    for (i = 0; i < SHM_CANARY; i++) {
        if (test_shm[DB_SHM_BLOCK_SIZE + i] != 0) {
            fprintf(stderr, "FAIL %s: wrote %d bytes past the 16KB block\n",
                    where, i + 1);
            exit(1);
        }
    }
}

/* 使い捨ての DB を legacy db_open (CREATE 付き) で作る。 */
static void make_db(const char *path, const char *ddl)
{
    int h = kapi_db_open(path);
    CHECK(h >= 0);
    CHECK(kapi_db_exec(h, ddl) == 0);
    CHECK(kapi_db_close(h) == 0);
}

/* ---- 1. RO / RW の no-create と open 前の検査 -------------------------- */
static void open_existing(void)
{
    int h;

    /* 欠損 DB は RO / RW とも失敗し、**作られない** */
    CHECK(kapi_db_open_existing("/nosuch.db", 0) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_CANTOPEN);
    CHECK(fixture_find("/nosuch.db", 0) == NULL);
    CHECK(kapi_db_open_existing("/nosuch.db", 1) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_CANTOPEN);
    CHECK(fixture_find("/nosuch.db", 0) == NULL);
    CHECK(fixture_find("/nosuch.db-journal", 0) == NULL);

    /* 0 バイトのファイルは NOTADB。SQLite を呼ばないので中身は変わらない。 */
    CHECK(fixture_find("/empty.db", 1) != NULL);
    CHECK(kapi_db_open_existing("/empty.db", 0) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_NOTADB);
    CHECK(fixture_find("/empty.db", 0)->size == 0);

    /* hot journal があれば RO / RW とも BUSY_RECOVERY。journal は消えない。 */
    make_db("/real.db", "CREATE TABLE t(x)");
    CHECK(fixture_create(NULL, "/real.db-journal", "hot", 3) == VFS_OK);
    CHECK(kapi_db_open_existing("/real.db", 0) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_BUSY_RECOVERY);
    CHECK(kapi_db_open_existing("/real.db", 1) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_BUSY_RECOVERY);
    CHECK(fixture_find("/real.db-journal", 0) != NULL);
    CHECK(fixture_find("/real.db-journal", 0)->size == 3);
    CHECK(fixture_rm("/real.db-journal") == VFS_OK);

    /* 引数の拒否 */
    CHECK(kapi_db_open_existing("/real.db", 2) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_MISUSE);
    CHECK(kapi_db_open_existing("", 0) == -1);
    CHECK(kapi_db_open_existing(":memory:", 0) == -1);
    CHECK(kapi_db_open_existing(":MEMORY:", 0) == -1);
    CHECK(kapi_db_open_existing("file:/real.db", 0) == -1);
    CHECK(kapi_db_open_existing(NULL, 0) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_MISUSE);

    /* 正常な RO / RW open。成功で「直前 open 失敗」は 0 に戻る。 */
    h = kapi_db_open_existing("/real.db", 0);
    CHECK(h >= 0);
    CHECK(kapi_db_error_code(-1) == SQLITE_OK);
    CHECK(kapi_db_error_code(h) == SQLITE_OK);
    /* RO 接続では書けない */
    CHECK(kapi_db_exec(h, "INSERT INTO t VALUES(1)") == -1);
    CHECK(kapi_db_error_code(h) != SQLITE_OK);
    CHECK(kapi_db_close(h) == 0);

    h = kapi_db_open_existing("/real.db", 1);
    CHECK(h >= 0);
    CHECK(kapi_db_exec(h, "INSERT INTO t VALUES(1)") == 0);
    CHECK(kapi_db_error_code(h) == SQLITE_OK);
    CHECK(kapi_db_close(h) == 0);
}

/* ---- 2. prepare_only は step しない ------------------------------------ */
static void prepare_only(void)
{
    static char big[DB_SQL_MAX_BYTES + 64];
    int h;

    make_db("/p.db", "CREATE TABLE t(x)");
    h = kapi_db_open_existing("/p.db", 1);
    CHECK(h >= 0);
    CHECK(kapi_db_exec(h, "INSERT INTO t VALUES(7)") == 0);

    /* SELECT: prepare_only では先頭行が進まない (legacy prepare とは違う) */
    CHECK(kapi_db_prepare_only(h, "SELECT x FROM t") == 0);
    CHECK(((DB_ResultHeader *)test_shm)->status != DB_STATUS_ROW);
    CHECK(kapi_db_column_int(h, 0) == 0);      /* まだ行は無い */
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);
    CHECK(kapi_db_column_int(h, 0) == 7);
    CHECK(kapi_db_step(h) == DB_STATUS_DONE);

    /* DML: prepare_only では実行されない */
    CHECK(kapi_db_prepare_only(h, "INSERT INTO t VALUES(8)") == 0);
    CHECK(kapi_db_prepare_only(h, "SELECT count(*) FROM t") == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);
    CHECK(kapi_db_column_int(h, 0) == 1);      /* 8 は入っていない */
    CHECK(kapi_db_finalize(h) == 0);

    /* 複数 statement は拒否、末尾の空白 / コメント / ; は可 */
    CHECK(kapi_db_prepare_only(h, "SELECT 1; SELECT 2") == -1);
    CHECK(kapi_db_error_code(h) == SQLITE_MISUSE);
    CHECK(kapi_db_prepare_only(h, "SELECT 1;  ") == 0);
    CHECK(kapi_db_prepare_only(h, "SELECT 1 -- tail") == 0);
    CHECK(kapi_db_prepare_only(h, "SELECT 1 /* tail */") == 0);
    CHECK(kapi_db_prepare_only(h, "   ") == -1);
    CHECK(kapi_db_prepare_only(h, "") == -1);

    /* NUL 込み 1024B ちょうどは通り、1 バイト超は**切り捨てず**拒否 */
    memset(big, ' ', sizeof(big));
    memcpy(big, "SELECT 1", 8);
    big[DB_SQL_MAX_BYTES - 1] = '\0';
    CHECK(kapi_db_prepare_only(h, big) == 0);
    big[DB_SQL_MAX_BYTES - 1] = ' ';
    big[DB_SQL_MAX_BYTES] = '\0';
    CHECK(kapi_db_prepare_only(h, big) == -1);
    CHECK(kapi_db_error_code(h) == SQLITE_MISUSE);

    CHECK(kapi_db_close(h) == 0);
}

/* ---- 3. bind の上限と往復 --------------------------------------------- */
static void binds(void)
{
    static unsigned char blob[DB_BIND_BLOB_MAX + 1];
    static char text[DB_BIND_TEXT_MAX + 2];
    const unsigned char *got;
    int h, i;

    for (i = 0; i < (int)sizeof(blob); i++) blob[i] = (unsigned char)(i & 0xFF);
    memset(text, 'x', sizeof(text));

    make_db("/b.db", "CREATE TABLE t(a,b,c)");
    h = kapi_db_open_existing("/b.db", 1);
    CHECK(h >= 0);

    /* prepare の前 / step の後は bind できない */
    CHECK(kapi_db_bind_int(h, 1, 1) == -1);
    CHECK(kapi_db_error_code(h) == SQLITE_MISUSE);

    CHECK(kapi_db_prepare_only(h, "INSERT INTO t VALUES(?,?,?)") == 0);
    CHECK(kapi_db_bind_int(h, 0, 1) == -1);             /* 1-based */
    CHECK(kapi_db_error_code(h) == SQLITE_RANGE);
    CHECK(kapi_db_bind_int(h, 4, 1) == -1);
    CHECK(kapi_db_error_code(h) == SQLITE_RANGE);
    CHECK(kapi_db_bind_text(h, 1, NULL, 0) == -1);      /* NULL は bind_null */
    CHECK(kapi_db_bind_text(h, 1, text, -1) == -1);     /* 負の長さ */
    CHECK(kapi_db_bind_text(h, 1, text, DB_BIND_TEXT_MAX + 1) == -1);
    CHECK(kapi_db_bind_blob(h, 3, blob, DB_BIND_BLOB_MAX + 1) == -1);
    CHECK(kapi_db_bind_blob(h, 3, NULL, 4) == -1);

    CHECK(kapi_db_bind_text(h, 1, text, DB_BIND_TEXT_MAX) == 0);
    CHECK(kapi_db_bind_int(h, 2, -12345) == 0);
    CHECK(kapi_db_bind_blob(h, 3, blob, DB_BIND_BLOB_MAX) == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_DONE);
    CHECK(kapi_db_bind_int(h, 2, 1) == -1);             /* step の後は不可 */

    /* 0B の text / blob は NULL ではない空値 */
    CHECK(kapi_db_prepare_only(h, "INSERT INTO t VALUES(?,?,?)") == 0);
    CHECK(kapi_db_bind_text(h, 1, text, 0) == 0);
    CHECK(kapi_db_bind_null(h, 2) == 0);
    CHECK(kapi_db_bind_blob(h, 3, blob, 0) == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_DONE);

    CHECK(kapi_db_prepare_only(h,
        "SELECT typeof(a), typeof(b), typeof(c) FROM t WHERE length(a)=0") == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);
    CHECK(!strcmp(kapi_db_column_text(h, 0), "text"));
    CHECK(!strcmp(kapi_db_column_text(h, 1), "null"));
    CHECK(!strcmp(kapi_db_column_text(h, 2), "blob"));
    CHECK(kapi_db_finalize(h) == 0);

    /* 4096B blob の往復 (SQLITE_TRANSIENT なのでスクラッチを上書きしてよい) */
    memset(blob_copy_buf, 0, sizeof(blob_copy_buf));
    CHECK(kapi_db_prepare_only(h,
        "SELECT c FROM t WHERE length(c)=?") == 0);
    CHECK(kapi_db_bind_int(h, 1, DB_BIND_BLOB_MAX) == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);
    got = (const unsigned char *)kapi_db_column_text(h, 0);
    for (i = 0; i < DB_BIND_BLOB_MAX; i++) CHECK(got[i] == (unsigned char)(i & 0xFF));
    CHECK(kapi_db_finalize(h) == 0);
    canary_check("binds");

    CHECK(kapi_db_close(h) == 0);
}

/* ---- 4. db_error_code の保持規則 --------------------------------------- */
static void error_code(void)
{
    int h, code;

    CHECK(kapi_db_error_code(-2) == SQLITE_MISUSE);
    CHECK(kapi_db_error_code(DB_MAX_CONNECTIONS) == SQLITE_MISUSE);
    CHECK(kapi_db_error_code(0) == SQLITE_MISUSE);   /* 一度も開いていない */

    make_db("/e.db", "CREATE TABLE t(x)");
    h = kapi_db_open_existing("/e.db", 1);
    CHECK(h >= 0 && kapi_db_error_code(h) == SQLITE_OK);

    /* データ操作の失敗はコードを残し、成功で 0 に戻る */
    CHECK(kapi_db_prepare_only(h, "SELECT * FROM missing") == -1);
    code = kapi_db_error_code(h);
    CHECK(code != SQLITE_OK);
    CHECK(kapi_db_error_code(h) == code);            /* 取得しても消えない */
    CHECK(kapi_db_prepare_only(h, "SELECT 1") == 0);
    CHECK(kapi_db_error_code(h) == SQLITE_OK);

    /* finalize / close は成功しても上書きしない */
    CHECK(kapi_db_prepare_only(h, "SELECT * FROM missing") == -1);
    code = kapi_db_error_code(h);
    CHECK(kapi_db_finalize(h) == 0);
    CHECK(kapi_db_error_code(h) == code);
    CHECK(kapi_db_close(h) == 0);
    CHECK(kapi_db_error_code(h) == code);            /* close 後も再利用まで */

    /* slot を再利用すると新しい接続の状態になる */
    CHECK(kapi_db_open_existing("/e.db", 0) == h);
    CHECK(kapi_db_error_code(h) == SQLITE_OK);
    CHECK(kapi_db_close(h) == 0);

    /* owner 別の欄。別の owner の失敗は混ざらない。 */
    resolve_owner = current_owner = 2;
    CHECK(kapi_db_open_existing("/nosuch.db", 0) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_CANTOPEN);
    resolve_owner = current_owner = 3;
    CHECK(kapi_db_error_code(-1) == SQLITE_OK);
    CHECK(kapi_db_open_existing("/e.db", 2) == -1);
    CHECK(kapi_db_error_code(-1) == SQLITE_MISUSE);
    resolve_owner = current_owner = 2;
    CHECK(kapi_db_error_code(-1) == SQLITE_CANTOPEN);
    current_owner = DB_OWNER_SLOTS;                  /* 池の外 */
    CHECK(kapi_db_error_code(-1) == SQLITE_MISUSE);
    resolve_owner = current_owner = 2;
}

/* ---- 5. SHM の境界 (票 §1b) -------------------------------------------- */
static void shm_bound(void)
{
    u32 hdr = (u32)sizeof(DB_ResultHeader);
    u32 desc = (u32)sizeof(DB_ColumnInfo);
    int h;

    /* 純関数の側 */
    CHECK(shm_row_fits_n(1, DB_SHM_BLOCK_SIZE - hdr - desc));
    CHECK(!shm_row_fits_n(1, DB_SHM_BLOCK_SIZE - hdr - desc + 1));
    CHECK(!shm_row_fits_n((int)((DB_SHM_BLOCK_SIZE - hdr) / desc) + 1, 0));
    CHECK(!shm_row_fits_n(-1, 0));

    /* 実接続: 16KB に収まらない 1 行は -1、SHM の外へ 1 バイトも書かない */
    make_db("/s.db", "CREATE TABLE t(x)");
    h = kapi_db_open_existing("/s.db", 1);
    CHECK(h >= 0);
    CHECK(kapi_db_exec(h, "INSERT INTO t VALUES(zeroblob(20000))") == 0);
    CHECK(kapi_db_prepare_only(h, "SELECT x FROM t") == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ERROR);
    CHECK(kapi_db_error_code(h) == SQLITE_TOOBIG);
    CHECK(((DB_ResultHeader *)test_shm)->status == DB_STATUS_ERROR);
    CHECK(((DB_ResultHeader *)test_shm)->column_count == 0);
    canary_check("shm_bound");
    CHECK(kapi_db_finalize(h) == 0);

    /* 列数だけで descriptor 領域を溢れさせても同じ (部分 ROW を返さない) */
    CHECK(kapi_db_prepare_only(h,
        "SELECT 1,2,3,4,5,6,7,8,9,10 FROM t") == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);     /* 10 列は収まる */
    canary_check("shm_bound cols");
    CHECK(kapi_db_finalize(h) == 0);
    CHECK(kapi_db_close(h) == 0);
}

/* ---- 6. CPL=3 のポインタ検証 ------------------------------------------- */
static void user_range(void)
{
    int h;
    char *inband = (char *)malloc(64);

    CHECK(inband != NULL);
    memset(inband, 'q', 64);

    /* CPL=0 の呼び手は帯を見ない (NULL と長さだけ) */
    host_cpl3 = 0;
    CHECK(db_user_range_ok(inband, 8));
    CHECK(!db_user_range_ok(NULL, 1));
    /* 加算 overflow。include/types.h の u32 はホストでは 64bit (unsigned long)
     * なので、ホスト幅の端で同じ経路を踏む。 */
    CHECK(!db_user_range_ok((const void *)~(u32)0xFF, 0x200u));

    /* CPL=3: 帯の外、帯の末尾をまたぐ、ガードをまたぐ、overflow */
    host_cpl3 = 1;
    CHECK(db_user_range_ok((const void *)BAND_LO, 16));
    CHECK(!db_user_range_ok((const void *)(BAND_LO - 1), 2));
    CHECK(!db_user_range_ok((const void *)(BAND_HI - 1), 2));
    CHECK(db_user_range_ok((const void *)(BAND_HI - 2), 2));
    CHECK(!db_user_range_ok((const void *)(GUARD_PAGE - 1), 2));
    CHECK(!db_user_range_ok((const void *)(GUARD_PAGE + 8), 2));
    CHECK(!db_user_range_ok((const void *)~(u32)0x0F, 0x20u));
    CHECK(!db_user_range_ok(NULL, 0));
    host_cpl3 = 0;

    /* NUL の無い path は上限まで探して拒否 (切り捨てて開かない) */
    {
        static char no_nul[OS32_MAX_PATH + 64];
        memset(no_nul, 'a', sizeof(no_nul));
        CHECK(kapi_db_open_existing(no_nul, 0) == -1);
        CHECK(kapi_db_error_code(-1) == SQLITE_MISUSE);
    }

    /* 検証を通った text はカーネルのスクラッチへ写る (SQLite は呼び手の
     * ポインタを持たない) — 写した後にユーザ側を書き換えても値は変わらない。*/
    make_db("/u.db", "CREATE TABLE t(x)");
    h = kapi_db_open_existing("/u.db", 1);
    CHECK(h >= 0);
    CHECK(kapi_db_prepare_only(h, "INSERT INTO t VALUES(?)") == 0);
    memcpy(inband, "keep", 5);
    CHECK(kapi_db_bind_text(h, 1, inband, 4) == 0);
    memcpy(inband, "GONE", 4);
    CHECK(kapi_db_step(h) == DB_STATUS_DONE);
    CHECK(kapi_db_prepare_only(h, "SELECT x FROM t") == 0);
    CHECK(kapi_db_step(h) == DB_STATUS_ROW);
    CHECK(!strcmp(kapi_db_column_text(h, 0), "keep"));
    CHECK(kapi_db_finalize(h) == 0);
    CHECK(kapi_db_close(h) == 0);
    free(inband);
}

/* ---- 7. 回収順序 (票 §1c) ---------------------------------------------- */
/*  DB 接続の main / journal FD は汎用 FD (kapi_db.c は既定 VFS を使う) なので、
 *  vfs_close_owned が先に走ると SQLite は死んだ FD 越しに rollback / close を
 *  投げることになる。新しい並び (DB → FD) と古い並び (FD → DB) を両方走らせ、
 *  観測できる差を assert する。                                             */
static void reclaim_order(const char *mode)
{
    static unsigned char before[FIXTURE_BYTES];
    u32 before_size;
    int h, live, i, db_probes;

    make_db("/r.db", "CREATE TABLE t(x)");
    resolve_owner = current_owner = 4;
    h = kapi_db_open_existing("/r.db", 1);
    CHECK(h >= 0);
    /* ページキャッシュを小さくして **本体ファイルへの spill** を起こす。
     * spill すると rollback は journal を読み戻さなければならない = 後始末に
     * 生きた FD が要る。キャッシュに収まる小さな transaction では I/O が
     * 起きず、順序の違いが観測できない。 */
    CHECK(kapi_db_exec(h, "PRAGMA cache_size = 2") == 0);
    before_size = fixture_find("/r.db", 0)->size;
    memcpy(before, fixture_find("/r.db", 0)->data, before_size);
    CHECK(kapi_db_exec(h, "BEGIN") == 0);
    for (i = 0; i < 60; i++)
        CHECK(kapi_db_exec(h, "INSERT INTO t VALUES(zeroblob(512))") == 0);
    /* 未 commit なので hot journal が出来ている = rollback に FD が要る */
    CHECK(fixture_find("/r.db-journal", 0) != NULL);
    live = 0;
    for (i = 3; i < VFS_MAX_OPEN_FILES; i++) if (open_files[i].in_use) live++;
    CHECK(live >= 2);

    if (!strcmp(mode, "new")) {
        db_probes = probes;
        db_cleanup_owned(4);            /* 票 §1c の並び: DB が先 */
        db_probes = probes - db_probes;
        vfs_close_owned(4);
    } else {
        vfs_close_owned(4);             /* 旧: FD が先 */
        live = 0;
        for (i = 3; i < VFS_MAX_OPEN_FILES; i++) if (open_files[i].in_use) live++;
        CHECK(live == 0);               /* 後始末の前に FD が消えている */
        db_probes = probes;
        db_cleanup_owned(4);
        db_probes = probes - db_probes;
    }
    printf("ORDER %s: backend calls during db teardown = %d\n", mode, db_probes);

    printf("ORDER %s: slot in_use=%d isolated=%d last_error=%d journal=%s\n",
           mode, db_slots[h].in_use, db_slots[h].isolated,
           db_slots[h].last_error,
           fixture_find("/r.db-journal", 0) ? "left" : "gone");
    /* 参考値: 本体サイズは **どちらの並びでも縮まない**。os32 SQLite VFS の
     * xTruncate がまだ no-op 成功だから (票 F3a)。ここでは判定に使わない。 */
    printf("ORDER %s: db size before=%u after=%u head_same=%d\n", mode,
           (unsigned)before_size, (unsigned)fixture_find("/r.db", 0)->size,
           memcmp(before, fixture_find("/r.db", 0)->data, before_size) == 0);
    live = 0;
    for (i = 3; i < VFS_MAX_OPEN_FILES; i++) if (open_files[i].in_use) live++;
    printf("ORDER %s: fds left=%d\n", mode, live);

    if (!strcmp(mode, "new")) {
        /* DB が先なら rollback も close も **生きた FD の上**で終わる。
         * 観測点は「後始末がバックエンドに届いた回数」— journal の読み戻しは
         * FD 越しにしか起きない。 */
        CHECK(!db_slots[h].in_use && !db_slots[h].isolated);
        CHECK(fixture_find("/r.db-journal", 0) == NULL);
        CHECK(live == 0);
        CHECK(db_probes >= 10);
    } else {
        /* 旧い並びでは FD が先に消えるので、rollback の I/O は
         * バックエンドに 1 度も届かない (path で引く journal の削除だけ)。
         * **戻り値は成功のまま**なのがこの並びの怖さ — os32 SQLite VFS は
         * まだ I/O 失敗を握り潰す (票 F3a は未実施)。だから「成功したか」では
         * なく「後始末が届いたか」で見る。 */
        CHECK(db_probes <= 3);
    }
    resolve_owner = current_owner = 2;
}

/* ---- 8. FEP のような protect 付き FD / 他 owner を巻き込まない ---------- */
static void owner_isolation(void)
{
    int parent, child;

    make_db("/p1.db", "CREATE TABLE t(x)");
    make_db("/c1.db", "CREATE TABLE t(x)");
    resolve_owner = current_owner = 1;
    parent = kapi_db_open_existing("/p1.db", 1);
    CHECK(parent >= 0);
    CHECK(kapi_db_prepare_only(parent, "SELECT 1") == 0);
    resolve_owner = current_owner = 3;
    child = kapi_db_open_existing("/c1.db", 1);
    CHECK(child >= 0 && child != parent);

    db_cleanup_owned(3);
    CHECK(!db_slots[child].in_use);
    CHECK(db_slots[parent].in_use);
    CHECK(kapi_db_step(parent) == DB_STATUS_ROW);   /* 親は無事 */
    resolve_owner = current_owner = 1;
    CHECK(kapi_db_close(parent) == 0);
    resolve_owner = current_owner = 2;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(os32_sqlite_init() == SQLITE_OK);
    reset_all();
    cases_run++;

    if (!strcmp(argv[1], "open_existing")) open_existing();
    else if (!strcmp(argv[1], "prepare_only")) prepare_only();
    else if (!strcmp(argv[1], "binds")) binds();
    else if (!strcmp(argv[1], "error_code")) error_code();
    else if (!strcmp(argv[1], "shm_bound")) shm_bound();
    else if (!strcmp(argv[1], "user_range")) user_range();
    else if (!strcmp(argv[1], "owner_isolation")) owner_isolation();
    else if (!strncmp(argv[1], "order_", 6)) reclaim_order(argv[1] + 6);
    else CHECK(0);

    canary_check(argv[1]);
    CHECK(memsys5_check_canary() == 0);
    printf("PASS %s\n", argv[1]);
    return 0;
}
