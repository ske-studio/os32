/* ========================================================================= */
/*  CFG_HOST.C — 票 S2-C §4 のホスト TDD                                     */
/*                                                                           */
/*  実物だけを組む: 実 `userland/lib/cfg/` 一式 + 実 `userland/cmds/cfg.c` +  */
/*  実 `kapi/kapi_db.c` + 実 `lib/sqlite3/sqlite3.c` + 実 `os32_sqlite_vfs.c` */
/*  + 実 `fs/vfs_fd.c` + RAM バックエンド。模型は exec 側のポインタ検証、     */
/*  SHM の置き場、VFS の rename / open / read / stat と「KAPI の close が     */
/*  失敗する」の注入だけ — どれもカーネル番地か障害注入でしか作れない。       */
/*  ホストのファイルシステムには 1 バイトも触らない (tsv は stdin から)。     */
/*                                                                           */
/*  実行: python3 -B tools/tests/test_cfg.py                                 */
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

static const char *host_resolve(const char *path)
{
    static char resolved[VFS_MAX_PATH];
    vfs_resolve_path(path, resolved, (int)sizeof(resolved));
    return resolved;
}
int vfs_rm(const char *path) { probes++; return fixture_rm(host_resolve(path)); }

static const char *stat_fail_on;
static int stat_fail_rc = OS32_ERR_IO;
int vfs_stat(const char *path, OS32_Stat *st)
{
    FixtureFile *f;
    probes++;
    if (stat_fail_on && strstr(path, stat_fail_on)) return stat_fail_rc;
    f = fixture_find(host_resolve(path), 0);
    if (!f) return OS32_ERR_NOTFOUND;
    if (st) { memset(st, 0, sizeof(*st)); st->st_size = f->size; st->st_nlink = 1; }
    return 0;
}

/* ---- exec 側の模型 (kapi_db.c が唯一使う口) ---------------------------- */
int ring3_user_range_ok(u32 p, u32 len)
{
    (void)len;
    return p != 0;                   /* CPL=0 の直呼びと同じ扱い */
}

#define SHM_CANARY 256
static unsigned char test_shm[DB_SHM_BLOCK_SIZE + SHM_CANARY];
#include "../../kapi/kapi_db.c"

/* ========================================================================= */
/*  libos32cfg の KAPI 境界 (差し替え先)                                     */
/* ========================================================================= */

#include "../../userland/lib/cfg/cfg_internal.h"

/* 障害注入 */
static int inj_close_fail;        /* != 0 = db_close が失敗し、この値を返す */
static const char *inj_exec_fail; /* 部分一致した SQL の db_exec を失敗させる */
static const char *inj_prep_fail; /* 同じく db_prepare_only を失敗させる */
static int inj_rename_mode;       /* 0=普通 1=新名あり旧名残る 2=何も起きない
                                     3=旧名は消えたが失敗を返す */

static int be_open_existing(const char *p, int w) { return kapi_db_open_existing(p, w); }
static int be_prepare_only(int h, const char *s)
{
    if (inj_prep_fail && strstr(s, inj_prep_fail)) {
        /* 実在しない表を引かせて、本物の SQLite の失敗と診断を作る。 */
        return kapi_db_prepare_only(h, "SELECT 1 FROM no_such_table_for_tdd");
    }
    return kapi_db_prepare_only(h, s);
}
static int be_bind_int(int h, int i, int v)       { return kapi_db_bind_int(h, i, v); }
static int be_bind_text(int h, int i, const char *s, int n)
{ return kapi_db_bind_text(h, i, s, n); }
static int be_bind_blob(int h, int i, const void *p, int n)
{ return kapi_db_bind_blob(h, i, p, n); }
static int be_bind_null(int h, int i)             { return kapi_db_bind_null(h, i); }
static int be_error_code(int h)
{
    if (inj_close_fail && h >= 0 && h < DB_MAX_CONNECTIONS &&
        db_slots[h].last_error == SQLITE_OK)
        return inj_close_fail;
    return kapi_db_error_code(h);
}
static int be_open(const char *p)                 { return kapi_db_open(p); }
static int be_close(int h)
{
    int rc = kapi_db_close(h);        /* 実際には閉じる (資源を漏らさない) */
    if (inj_close_fail) return -1;    /* ライブラリからは「失敗」に見せる */
    return rc;
}
static int be_exec(int h, const char *s)
{
    if (inj_exec_fail && strstr(s, inj_exec_fail)) return -1;
    return kapi_db_exec(h, s);
}
static int be_step(int h)                         { return kapi_db_step(h); }
static int be_finalize(int h)                     { return kapi_db_finalize(h); }
static unsigned char *be_shm(void)                { return test_shm; }
static int be_stat(const char *p, OS32_Stat *st)  { return vfs_stat(p, st); }

static int be_rename(const char *o, const char *n)
{
    FixtureFile *src = fixture_find(host_resolve(o), 0);
    FixtureFile *dst;
    if (!src) return OS32_ERR_NOTFOUND;
    if (inj_rename_mode == 2) return OS32_ERR_IO;   /* 何も起きなかった */
    dst = fixture_find(host_resolve(n), 1);
    if (!dst) return OS32_ERR_NOSPC;
    memcpy(dst->data, src->data, src->size);
    dst->size = src->size;
    if (inj_rename_mode == 1) return OS32_ERR_IO;   /* 旧名削除も巻き戻しも失敗 */
    src->exists = 0;
    if (inj_rename_mode == 3) return OS32_ERR_IO;   /* 旧名は消えたが失敗を返す */
    return 0;
}

static int be_unlink(const char *p) { return vfs_rm(p); }

/* tsv 読み出し用の最小 FD 表 (fixture の上、読み取り専用)。 */
#define HOSTFD_MAX 4
static struct { int used; FixtureFile *f; u32 off; } hostfd[HOSTFD_MAX];

static int be_fopen(const char *p, int mode)
{
    int i;
    FixtureFile *f = fixture_find(host_resolve(p), 0);
    (void)mode;
    if (!f) return OS32_ERR_NOTFOUND;
    for (i = 0; i < HOSTFD_MAX; i++) {
        if (!hostfd[i].used) {
            hostfd[i].used = 1;
            hostfd[i].f = f;
            hostfd[i].off = 0;
            return 100 + i;
        }
    }
    return OS32_ERR_NOSPC;
}

static int be_fread(int fd, void *buf, u32 size)
{
    int i = fd - 100;
    u32 n;
    if (i < 0 || i >= HOSTFD_MAX || !hostfd[i].used) return OS32_ERR_INVAL;
    if (hostfd[i].off >= hostfd[i].f->size) return 0;
    n = hostfd[i].f->size - hostfd[i].off;
    if (n > size) n = size;
    memcpy(buf, hostfd[i].f->data + hostfd[i].off, n);
    hostfd[i].off += n;
    return (int)n;
}

static void be_fclose(int fd)
{
    int i = fd - 100;
    if (i >= 0 && i < HOSTFD_MAX) hostfd[i].used = 0;
}

static u32 host_tick = 7;
static u32 be_tick(void) { return host_tick; }

static const CfgBackend host_backend = {
    be_open_existing, be_prepare_only, be_bind_int, be_bind_text, be_bind_blob,
    be_bind_null, be_error_code, be_open, be_close, be_exec, be_step,
    be_finalize, be_shm, be_stat, be_rename, be_unlink, be_fopen, be_fread,
    be_fclose, be_tick
};

const CfgBackend *cfg_backend_platform(void) { return &host_backend; }

/* ========================================================================= */
/*  試験対象 (そのまま載せる)                                                */
/* ========================================================================= */

#include "../../userland/lib/cfg/libos32cfg.c"
#include "../../userland/lib/cfg/cfg_enum.c"
#include "../../userland/lib/cfg/cfg_tsv.c"
#include "../../userland/lib/cfg/cfg_init.c"

/* cfg コマンドは main を差し替えて丸ごと載せる (出力は捕まえる)。 */
static char cap_out[65536];
static int cap_len;
static int cap_yields;
static int host_write(int fd, const void *buf, u32 n)
{
    if (fd == 1 || fd == 2) {
        if (cap_len + (int)n < (int)sizeof(cap_out)) {
            memcpy(cap_out + cap_len, buf, n);
            cap_len += (int)n;
        }
        return (int)n;
    }
    {   /* export の書き出し先も fixture の中 */
        int i = fd - 200;
        FixtureFile *f;
        if (i < 0 || i >= HOSTFD_MAX) return -1;
        f = hostfd[i].f;
        if (!f || hostfd[i].off + n > FIXTURE_BYTES) return -1;
        memcpy(f->data + hostfd[i].off, buf, n);
        hostfd[i].off += n;
        if (f->size < hostfd[i].off) f->size = hostfd[i].off;
        return (int)n;
    }
}
static int host_open_w(const char *path, int mode)
{
    int i;
    FixtureFile *f;
    (void)mode;
    f = fixture_find(host_resolve(path), 1);
    if (!f) return OS32_ERR_NOSPC;
    f->size = 0;
    for (i = 0; i < HOSTFD_MAX; i++) {
        if (!hostfd[i].used) {
            hostfd[i].used = 1;
            hostfd[i].f = f;
            hostfd[i].off = 0;
            return 200 + i;
        }
    }
    return OS32_ERR_NOSPC;
}
static void host_close_w(int fd)
{
    int i = fd - 200;
    if (i >= 0 && i < HOSTFD_MAX) hostfd[i].used = 0;
}
static i32 host_yield(void) { cap_yields++; return 0; }
static u32 host_gettick(void) { return host_tick; }

static KernelAPI host_api;

#define main cfg_main
#include "../../userland/cmds/cfg.c"
#undef main

/* ========================================================================= */
/*  下ごしらえ                                                               */
/* ========================================================================= */

static int cases_run;

static void reset_all(void)
{
    int i;
    memset(test_shm, 0, sizeof(test_shm));
    memset(db_slots, 0, sizeof(db_slots));
    memset(db_open_fail, 0, sizeof(db_open_fail));
    memset(fixture_files, 0, sizeof(fixture_files));
    memset(hostfd, 0, sizeof(hostfd));
    for (i = 0; i < VFS_MAX_OPEN_FILES; i++) open_files[i].in_use = 0;
    stat_fail_on = NULL;
    stat_fail_rc = OS32_ERR_IO;
    inj_close_fail = 0;
    inj_exec_fail = NULL;
    inj_prep_fail = NULL;
    inj_rename_mode = 0;
    resolve_cwd = "";
    resolve_owner = current_owner = 2;
    cap_len = 0;
    cap_yields = 0;
    host_tick = 7;
    fixture_init();
    /* cfg 側の静的状態を毎回真っさらに戻す */
    memset(&g_db, 0, sizeof(g_db));
    memset(&g_new, 0, sizeof(g_new));
    g_close_error = 0;
    g_init_reason = 0;
    g_backend = NULL;
    memset(&host_api, 0, sizeof(host_api));
    host_api.sys_write = host_write;
    host_api.sys_yield = host_yield;
    host_api.sys_open = host_open_w;
    host_api.sys_close = host_close_w;
    host_api.get_tick = host_gettick;
}

static void canary_check(const char *where)
{
    int i;
    for (i = 0; i < SHM_CANARY; i++) {
        if (test_shm[DB_SHM_BLOCK_SIZE + i] != 0) {
            fprintf(stderr, "FAIL %s: wrote past the 16KB block\n", where);
            exit(1);
        }
    }
}

/* 使い捨ての DB を legacy db_open (CREATE 付き) で作る。 */
static void raw_db(const char *path, const char *const *stmts)
{
    int h = kapi_db_open(path);
    int i;
    CHECK(h >= 0);
    for (i = 0; stmts[i]; i++) CHECK(kapi_db_exec(h, stmts[i]) == 0);
    CHECK(kapi_db_close(h) == 0);
}

static const char *DDL[] = {
    "CREATE TABLE meta (schema_version INTEGER NOT NULL, created TEXT)",
    "CREATE TABLE settings (scope TEXT NOT NULL, key TEXT NOT NULL,"
    " type INTEGER NOT NULL, ival INTEGER, tval TEXT, bval BLOB,"
    " PRIMARY KEY (scope, key)) WITHOUT ROWID",
    "INSERT INTO meta VALUES (1, '0')",
    NULL
};

static void make_good_db(void) { raw_db(CFG_DB_PATH, DDL); }

static void put_file(const char *path, const char *text)
{
    CHECK(fixture_create(NULL, path, text, (u32)strlen(text)) == VFS_OK);
}

/* ========================================================================= */
/*  (1) MISSING                                                              */
/* ========================================================================= */
static void c_missing(void)
{
    CfgDb *db;
    char buf[32];

    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_status(db) == CFG_MISSING);
    CHECK(cfg_schema_version(db) == 0);
    CHECK(cfg_get_int(db, "gshell", "desktop/color", 42) == 42);
    CHECK(cfg_get_text(db, "gshell", "a", buf, 32) == OS32_ERR_NOTFOUND);
    CHECK(cfg_begin(db) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "a", 1) == OS32_ERR_INVAL);
    CHECK(cfg_close(db) == 0);
    /* DB も journal も作られていない */
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);
    CHECK(fixture_find(CFG_DB_JOURNAL_PATH, 0) == NULL);

    /* writable=1 でも同じ (RW へ切り替えない) */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_status(db) == CFG_MISSING);
    CHECK(cfg_begin(db) == OS32_ERR_INVAL);
    CHECK(cfg_close(db) == 0);
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);
}

/* ========================================================================= */
/*  (2) CORRUPT — 0 バイト / hot journal / meta 表が無い                     */
/* ========================================================================= */
static void c_corrupt(void)
{
    CfgDb *db;
    static const char *empty_ddl[] = { "CREATE TABLE t(x)", NULL };

    CHECK(fixture_find(CFG_DB_PATH, 1) != NULL);      /* 0 バイト */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_status(db) == CFG_CORRUPT);
    CHECK(cfg_begin(db) == OS32_ERR_INVAL);
    CHECK(cfg_get_int(db, "gshell", "a", 9) == 9);
    CHECK(cfg_close(db) == 0);
    CHECK(fixture_find(CFG_DB_PATH, 0)->size == 0);   /* 触っていない */

    reset_all();
    make_good_db();
    put_file(CFG_DB_JOURNAL_PATH, "hot");             /* hot journal */
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_status(db) == CFG_CORRUPT);
    CHECK(cfg_close(db) == 0);
    CHECK(fixture_find(CFG_DB_JOURNAL_PATH, 0) != NULL);  /* 消さない */

    reset_all();
    raw_db(CFG_DB_PATH, empty_ddl);                   /* meta 表が無い */
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_status(db) == CFG_CORRUPT);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (3)(12) VERSION — 読めるが書けない。RW 要求でも RO のまま。              */
/* ========================================================================= */
static void c_version(void)
{
    CfgDb *db;
    char buf[64];
    static const char *v2[] = {
        "CREATE TABLE meta (schema_version INTEGER NOT NULL, created TEXT)",
        "CREATE TABLE settings (scope TEXT NOT NULL, key TEXT NOT NULL,"
        " type INTEGER NOT NULL, ival INTEGER, tval TEXT, bval BLOB,"
        " PRIMARY KEY (scope, key)) WITHOUT ROWID",
        "INSERT INTO meta VALUES (2, '0')",
        "INSERT INTO settings VALUES ('gshell','desktop/color',0,5,NULL,NULL)",
        "INSERT INTO settings VALUES ('gshell','desktop/name',1,NULL,'hi',NULL)",
        NULL
    };
    raw_db(CFG_DB_PATH, v2);

    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_status(db) == CFG_VERSION);
    CHECK(cfg_schema_version(db) == 2);
    /* 認識できる列はそのまま読める */
    CHECK(cfg_get_int(db, "gshell", "desktop/color", 0) == 5);
    CHECK(cfg_get_text(db, "gshell", "desktop/name", buf, 64) == 2);
    CHECK(!strcmp(buf, "hi"));
    /* 書きは拒否 */
    CHECK(cfg_begin(db) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "x", 1) == OS32_ERR_INVAL);
    CHECK(cfg_delete(db, "gshell", "desktop/color") == OS32_ERR_INVAL);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (4)(19) 型違い・cap 不足・NULL と空値                                     */
/* ========================================================================= */
static void c_types(void)
{
    CfgDb *db;
    char buf[8];
    unsigned char bb[8];
    static const char *rows[] = {
        "CREATE TABLE meta (schema_version INTEGER NOT NULL, created TEXT)",
        "CREATE TABLE settings (scope TEXT NOT NULL, key TEXT NOT NULL,"
        " type INTEGER NOT NULL, ival INTEGER, tval TEXT, bval BLOB,"
        " PRIMARY KEY (scope, key)) WITHOUT ROWID",
        "INSERT INTO meta VALUES (1, '0')",
        "INSERT INTO settings VALUES ('gshell','n',0,7,NULL,NULL)",
        "INSERT INTO settings VALUES ('gshell','t',1,NULL,'abcdefgh',NULL)",
        "INSERT INTO settings VALUES ('gshell','e',1,NULL,'',NULL)",
        "INSERT INTO settings VALUES ('gshell','nul',1,NULL,NULL,NULL)",
        "INSERT INTO settings VALUES ('gshell','b',2,NULL,NULL,x'0102')",
        "INSERT INTO settings VALUES ('gshell','eb',2,NULL,NULL,x'')",
        NULL
    };
    raw_db(CFG_DB_PATH, rows);
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_status(db) == CFG_OK);

    /* 型違いは「無い」扱い */
    CHECK(cfg_get_int(db, "gshell", "t", 3) == 3);
    CHECK(cfg_get_text(db, "gshell", "n", buf, 8) == OS32_ERR_NOTFOUND);
    CHECK(cfg_get_blob(db, "gshell", "t", bb, 8) == OS32_ERR_NOTFOUND);

    /* cap 不足は NOSPC で out を書かない */
    memset(buf, 'Z', sizeof(buf));
    CHECK(cfg_get_text(db, "gshell", "t", buf, 8) == OS32_ERR_NOSPC);
    CHECK(buf[0] == 'Z' && buf[7] == 'Z');
    memset(bb, 'Z', sizeof(bb));
    CHECK(cfg_get_blob(db, "gshell", "b", bb, 1) == OS32_ERR_NOSPC);
    CHECK(bb[0] == 'Z');
    CHECK(cfg_get_blob(db, "gshell", "b", bb, 2) == 2);
    CHECK(bb[0] == 1 && bb[1] == 2);

    /* NULL の text は未設定、空 text は長さ 0 */
    CHECK(cfg_get_text(db, "gshell", "nul", buf, 8) == OS32_ERR_NOTFOUND);
    CHECK(cfg_get_text(db, "gshell", "e", buf, 8) == 0);
    CHECK(buf[0] == '\0');
    CHECK(cfg_get_blob(db, "gshell", "eb", bb, 8) == 0);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (5) begin → set → commit → close → reopen                               */
/* ========================================================================= */
static void c_roundtrip(void)
{
    CfgDb *db;
    char buf[300];
    unsigned char big[CFG_BLOB_MAX], out[CFG_BLOB_MAX];
    int i;

    make_good_db();
    for (i = 0; i < CFG_BLOB_MAX; i++) big[i] = (unsigned char)(i & 0xFF);

    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_status(db) == CFG_OK);
    CHECK(cfg_schema_version(db) == 1);
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_set_int(db, "gshell", "desktop/color", 5) == 0);
    CHECK(cfg_set_text(db, "gshell", "desktop/name", "hello") == 0);
    CHECK(cfg_set_blob(db, "app:filer", "window/main", big, CFG_BLOB_MAX) == 0);
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);

    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_get_int(db, "gshell", "desktop/color", 0) == 5);
    CHECK(cfg_get_text(db, "gshell", "desktop/name", buf, 300) == 5);
    CHECK(!strcmp(buf, "hello"));
    CHECK(cfg_get_blob(db, "app:filer", "window/main", out, CFG_BLOB_MAX)
          == CFG_BLOB_MAX);
    CHECK(!memcmp(out, big, CFG_BLOB_MAX));
    CHECK(cfg_close(db) == 0);

    /* delete も往復する */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_delete(db, "gshell", "desktop/name") == 0);
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_get_text(db, "gshell", "desktop/name", buf, 300) == OS32_ERR_NOTFOUND);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (6)(7)(15) set 失敗 → commit 拒否 / close で rollback / 診断の保持       */
/* ========================================================================= */
static void c_txn(void)
{
    CfgDb *db;

    make_good_db();
    /* txn 外の set は拒否 */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_set_int(db, "gshell", "a", 1) == OS32_ERR_INVAL);

    /* set 失敗 → txn を failed にし、commit は拒否して rollback する */
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_set_int(db, "gshell", "a", 1) == 0);
    inj_prep_fail = "INSERT OR REPLACE";
    CHECK(cfg_set_int(db, "gshell", "c", 3) == OS32_ERR_IO);
    inj_prep_fail = NULL;
    CHECK(db->txn == 2);
    CHECK(cfg_last_sqlite(db) != 0);
    {
        int saved = cfg_last_sqlite(db);
        CHECK(cfg_commit(db) == OS32_ERR_IO);          /* commit は拒否 */
        /* 成功した ROLLBACK は診断を 0 に戻さない (票 §1-4) */
        CHECK(cfg_last_sqlite(db) == saved);
    }
    CHECK(cfg_close(db) == 0);
    /* rollback されたので何も残っていない */
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_get_int(db, "gshell", "a", -1) == -1);
    CHECK(cfg_close(db) == 0);

    /* 未 commit のまま close → rollback */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_set_int(db, "gshell", "b", 2) == 0);
    CHECK(cfg_close(db) == 0);
    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_get_int(db, "gshell", "b", -1) == -1);
    CHECK(cfg_close(db) == 0);

    /* close の失敗はコードを保持して負を返す */
    CHECK(cfg_open(&db, 1) == 0);
    inj_close_fail = SQLITE_IOERR;
    CHECK(cfg_close(db) == OS32_ERR_IO);
    CHECK(cfg_last_close_error() == SQLITE_IOERR);
    inj_close_fail = 0;
    /* 静的 1 本なので close 後も診断が読める */
    CHECK(cfg_last_sqlite(db) == SQLITE_IOERR);
}

/* ========================================================================= */
/*  (8) 上限と規則                                                           */
/* ========================================================================= */
static void c_limits(void)
{
    CfgDb *db;
    char key64[CFG_KEY_MAX + 2], txt256[CFG_TEXT_MAX + 2];
    unsigned char blob[CFG_BLOB_MAX + 1];
    char scope64[CFG_SCOPE_MAX + 2];
    int i;

    make_good_db();
    for (i = 0; i < CFG_KEY_MAX + 1; i++) key64[i] = 'a';
    key64[CFG_KEY_MAX + 1] = '\0';
    for (i = 0; i < CFG_TEXT_MAX + 1; i++) txt256[i] = 'x';
    txt256[CFG_TEXT_MAX + 1] = '\0';
    memset(blob, 0xAB, sizeof(blob));
    strcpy(scope64, "app:");
    for (i = 4; i < CFG_SCOPE_MAX + 1; i++) scope64[i] = 'n';
    scope64[CFG_SCOPE_MAX + 1] = '\0';

    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);

    /* key 63B は通り 64B は拒否 */
    key64[CFG_KEY_MAX] = '\0';
    CHECK(cfg_set_int(db, "gshell", key64, 1) == 0);
    key64[CFG_KEY_MAX] = 'a';
    CHECK(cfg_set_int(db, "gshell", key64, 1) == OS32_ERR_INVAL);

    /* text 255B は通り 256B は拒否 */
    txt256[CFG_TEXT_MAX] = '\0';
    CHECK(cfg_set_text(db, "gshell", "t", txt256) == 0);
    txt256[CFG_TEXT_MAX] = 'x';
    CHECK(cfg_set_text(db, "gshell", "t", txt256) == OS32_ERR_INVAL);

    /* blob 4096B は通り 4097B は拒否 */
    CHECK(cfg_set_blob(db, "gshell", "b", blob, CFG_BLOB_MAX) == 0);
    CHECK(cfg_set_blob(db, "gshell", "b", blob, CFG_BLOB_MAX + 1) == OS32_ERR_INVAL);

    /* scope / key の字句規則 */
    CHECK(cfg_set_int(db, "SYSTEM", "a", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "app:", "a", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "app:My App", "a", 1) == OS32_ERR_INVAL);
    scope64[CFG_SCOPE_MAX] = '\0';
    CHECK(cfg_set_int(db, scope64, "a", 1) == 0);
    scope64[CFG_SCOPE_MAX] = 'n';
    CHECK(cfg_set_int(db, scope64, "a", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "Desktop", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "desktop/", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "/desktop", 1) == OS32_ERR_INVAL);
    CHECK(cfg_set_int(db, "gshell", "", 1) == OS32_ERR_INVAL);

    /* 不正 UTF-8 の text は拒否 */
    CHECK(cfg_set_text(db, "gshell", "u", "\xff\xfe") == OS32_ERR_INVAL);
    CHECK(cfg_set_text(db, "gshell", "u", "\xc0\x80") == OS32_ERR_INVAL);
    CHECK(cfg_set_text(db, "gshell", "u", "\xed\xa0\x80") == OS32_ERR_INVAL);
    CHECK(cfg_set_text(db, "gshell", "u", "\xe3\x81\x82") == 0);   /* あ */
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (9)(14) enum — 件数・順序・再入・前方一致・257 件                        */
/* ========================================================================= */
static int en_n;
static char en_seen[CFG_ENUM_MAX][CFG_KEY_MAX + 1];
static int en_types[CFG_ENUM_MAX];
static CfgDb *en_db;
static int en_reenter_rc;

static int en_cb(const char *key, int type, void *ctx)
{
    (void)ctx;
    if (en_n < CFG_ENUM_MAX) {
        strcpy(en_seen[en_n], key);
        en_types[en_n] = type;
    }
    en_n++;
    return 0;
}

static int en_scope_n;
static char en_scopes[CFG_SCOPES_MAX][CFG_SCOPE_MAX + 1];

static int en_scope_cb(const char *scope, void *ctx)
{
    (void)ctx;
    if (en_scope_n < CFG_SCOPES_MAX) strcpy(en_scopes[en_scope_n], scope);
    en_scope_n++;
    return 0;
}

static int en_cb_reenter(const char *key, int type, void *ctx)
{
    (void)key; (void)type; (void)ctx;
    en_reenter_rc = cfg_enum(en_db, "gshell", NULL, en_cb, NULL);
    en_n++;
    return 0;
}

static void c_enum(void)
{
    CfgDb *db;
    int i;
    char key[32];

    make_good_db();
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_set_int(db, "gshell", "a_b", 1) == 0);
    CHECK(cfg_set_int(db, "gshell", "axb", 2) == 0);
    CHECK(cfg_set_int(db, "gshell", "a_c", 3) == 0);
    CHECK(cfg_set_text(db, "gshell", "b_z", "t") == 0);
    CHECK(cfg_set_int(db, "system", "s1", 1) == 0);
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);

    CHECK(cfg_open(&db, 0) == 0);
    en_n = 0;
    CHECK(cfg_enum(db, "gshell", NULL, en_cb, NULL) == 4);
    CHECK(en_n == 4);
    CHECK(!strcmp(en_seen[0], "a_b"));      /* ORDER BY key */
    CHECK(!strcmp(en_seen[1], "a_c"));
    CHECK(!strcmp(en_seen[2], "axb"));
    CHECK(!strcmp(en_seen[3], "b_z"));
    CHECK(en_types[3] == CFG_TYPE_TEXT);

    /* prefix "a_" は LIKE ではなく substr の前方一致 — axb は入らない */
    en_n = 0;
    CHECK(cfg_enum(db, "gshell", "a_", en_cb, NULL) == 2);
    CHECK(!strcmp(en_seen[0], "a_b") && !strcmp(en_seen[1], "a_c"));

    /* scope 一覧 (DISTINCT + ORDER BY) */
    en_scope_n = 0;
    CHECK(cfg_enum_scopes(db, en_scope_cb, NULL) == 2);
    CHECK(!strcmp(en_scopes[0], "gshell") && !strcmp(en_scopes[1], "system"));
    CHECK(cfg_close(db) == 0);

    /* callback の中からの cfg_* は拒否 */
    CHECK(cfg_open(&db, 0) == 0);
    en_db = db;
    en_n = 0;
    en_reenter_rc = 0;
    CHECK(cfg_enum(db, "gshell", NULL, en_cb_reenter, NULL) >= 1);
    CHECK(en_reenter_rc == OS32_ERR_INVAL);
    CHECK(cfg_close(db) == 0);

    /* 257 件目は NOSPC で callback を 1 度も呼ばない */
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    for (i = 0; i < CFG_ENUM_MAX + 1; i++) {
        sprintf(key, "k%04d", i);
        CHECK(cfg_set_int(db, "user", key, i) == 0);
    }
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);
    CHECK(cfg_open(&db, 0) == 0);
    en_n = 0;
    CHECK(cfg_enum(db, "user", NULL, en_cb, NULL) == OS32_ERR_NOSPC);
    CHECK(en_n == 0);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (13)(18) meta の 0 行 / 2 行 / TEXT / 2^32+1 / 正常行との混在            */
/* ========================================================================= */
static void meta_case(const char *insert1, const char *insert2, int want)
{
    CfgDb *db;
    const char *stmts[6];
    int n = 0;

    reset_all();
    stmts[n++] = "CREATE TABLE meta (schema_version INTEGER NOT NULL, created TEXT)";
    stmts[n++] = "CREATE TABLE settings (scope TEXT, key TEXT, type INTEGER,"
                 " ival INTEGER, tval TEXT, bval BLOB, PRIMARY KEY (scope,key))"
                 " WITHOUT ROWID";
    if (insert1) stmts[n++] = insert1;
    if (insert2) stmts[n++] = insert2;
    stmts[n] = NULL;
    raw_db(CFG_DB_PATH, stmts);
    CHECK(cfg_open(&db, 0) == 0);
    if (cfg_status(db) != want) {
        fprintf(stderr, "FAIL meta: %s / %s -> %d (want %d)\n",
                insert1 ? insert1 : "(none)", insert2 ? insert2 : "(none)",
                cfg_status(db), want);
        exit(1);
    }
    CHECK(cfg_close(db) == 0);
}

static void c_meta(void)
{
    meta_case(NULL, NULL, CFG_CORRUPT);                        /* 0 行 */
    meta_case("INSERT INTO meta VALUES (1,'a')",
              "INSERT INTO meta VALUES (1,'b')", CFG_CORRUPT); /* 2 行 */
    /* TEXT — INTEGER 親和性で '1' は整数に化けるので、化けない値で見る。 */
    meta_case("INSERT INTO meta VALUES ('abc','a')", NULL, CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (x'01','a')", NULL, CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (1.5,'a')", NULL, CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (4294967297,'a')", NULL, CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (0,'a')", NULL, CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (-1,'a')", NULL, CFG_CORRUPT);
    /* 正常行との混在 */
    meta_case("INSERT INTO meta VALUES (1,'a')",
              "INSERT INTO meta VALUES (0,'b')", CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (1,'a')",
              "INSERT INTO meta VALUES (4294967297,'b')", CFG_CORRUPT);
    meta_case("INSERT INTO meta VALUES (1,'a')", NULL, CFG_OK);
    meta_case("INSERT INTO meta VALUES (2,'a')", NULL, CFG_VERSION);
}

/* ========================================================================= */
/*  (17) SHM の row は private へ即コピーされている                          */
/* ========================================================================= */
static void c_shm_copy(void)
{
    CfgDb *db;
    char buf[64];
    int other;

    make_good_db();
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    CHECK(cfg_set_text(db, "gshell", "t", "private") == 0);
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_get_text(db, "gshell", "t", buf, 64) == 7);
    CHECK(!strcmp(buf, "private"));
    /* 別接続が SHM を丸ごと上書きしても、取り出した値は壊れない */
    other = kapi_db_open_existing(CFG_DB_PATH, 0);
    CHECK(other >= 0);
    CHECK(kapi_db_prepare_only(other, "SELECT 'AAAAAAAAAAAAAAAA'") == 0);
    CHECK(kapi_db_step(other) == DB_STATUS_ROW);
    CHECK(!strcmp(buf, "private"));
    CHECK(kapi_db_finalize(other) == 0);
    CHECK(kapi_db_close(other) == 0);
    CHECK(cfg_close(db) == 0);
}

/* ========================================================================= */
/*  (11)(22) cfg_init                                                        */
/* ========================================================================= */
static const char TSV_OK_TEXT[] =
    "# comment\n"
    "\n"
    "gshell\tdesktop/color\tint\t1\n"
    "gshell\tdesktop/wallpaper\ttext\t\n"
    "gshell\ttaskbar/clock_24h\tint\t1\n";

static void c_init(void)
{
    CfgDb *db;
    char buf[64];

    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    CHECK(cfg_init(NULL) == 0);
    CHECK(cfg_last_init_reason() == CFG_INIT_NONE);
    CHECK(fixture_find(CFG_DB_PATH, 0) != NULL);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) == NULL);

    CHECK(cfg_open(&db, 0) == 0);
    CHECK(cfg_status(db) == CFG_OK);
    CHECK(cfg_schema_version(db) == 1);
    CHECK(cfg_get_int(db, "gshell", "desktop/color", 0) == 1);
    CHECK(cfg_get_int(db, "gshell", "taskbar/clock_24h", 0) == 1);
    /* 末尾の空欄は NULL ではなく空 text */
    CHECK(cfg_get_text(db, "gshell", "desktop/wallpaper", buf, 64) == 0);
    CHECK(cfg_close(db) == 0);

    /* 生成した DB の形が mk_settings_db.py と一致すること
     * (user_version / meta の版 / 行数 / settings の列)。 */
    {
        int h = kapi_db_open_existing(CFG_DB_PATH, 0);
        CHECK(h >= 0);
        /* meta は 1 行、版は 1 */
        CHECK(kapi_db_prepare_only(h,
              "SELECT COUNT(*), MIN(schema_version) FROM meta") == 0);
        CHECK(kapi_db_step(h) == DB_STATUS_ROW);
        CHECK((int)cfg_i_col_int(0) == 1 && (int)cfg_i_col_int(1) == CFG_SCHEMA_VERSION);
        CHECK(kapi_db_finalize(h) == 0);
        /* settings は tsv の 3 行、列は 6 本 */
        CHECK(kapi_db_prepare_only(h,
              "SELECT COUNT(*) FROM settings") == 0);
        CHECK(kapi_db_step(h) == DB_STATUS_ROW);
        CHECK((int)cfg_i_col_int(0) == 3);
        CHECK(kapi_db_finalize(h) == 0);
        CHECK(kapi_db_prepare_only(h,
              "SELECT scope,key,type,ival,tval,bval FROM settings") == 0);
        CHECK(kapi_db_finalize(h) == 0);
        /* WITHOUT ROWID — rowid が無いこと */
        CHECK(kapi_db_prepare_only(h, "SELECT rowid FROM settings") != 0);
        CHECK(kapi_db_finalize(h) == 0);
        /* PRAGMA user_version はカーネルの SQLite では省かれている
         * (SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS) — 読みも書きも無効。
         * mk_settings_db.py (ホストの CPython SQLite) が入れる user_version は
         * `cfg init` では 0 のままになる。版の正典は meta.schema_version で、
         * ライブラリは user_version を読まないので動作には影響しない。 */
        CHECK(kapi_db_prepare_only(h, "PRAGMA user_version") == 0);
        CHECK(kapi_db_step(h) == DB_STATUS_DONE);
        CHECK(kapi_db_finalize(h) == 0);
        CHECK(kapi_db_close(h) == 0);
    }

    /* 2 度目は already exists で何も変えない */
    CHECK(cfg_init(NULL) == OS32_ERR_EXIST);
    CHECK(cfg_last_init_reason() == CFG_INIT_EXISTS);

    /* 0 バイトの本体も「ある」 */
    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    CHECK(fixture_find(CFG_DB_PATH, 1) != NULL);
    CHECK(cfg_init(NULL) == OS32_ERR_EXIST);

    /* journal の残骸 */
    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    put_file(CFG_DB_JOURNAL_PATH, "x");
    CHECK(cfg_init(NULL) == OS32_ERR_NOTEMPTY);
    CHECK(cfg_last_init_reason() == CFG_INIT_JOURNAL);
    CHECK(fixture_find(CFG_DB_JOURNAL_PATH, 0) != NULL);   /* 消さない */
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);

    /* .new の残骸 — 拒否して消さない (22) */
    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    put_file(CFG_DB_NEW_PATH, "stale");
    CHECK(cfg_init(NULL) == OS32_ERR_NOTEMPTY);
    CHECK(cfg_last_init_reason() == CFG_INIT_STALE_NEW);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) != NULL);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0)->size == 5);
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);

    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    put_file(CFG_DB_NEW_JOURNAL_PATH, "stale");
    CHECK(cfg_init(NULL) == OS32_ERR_NOTEMPTY);
    CHECK(cfg_last_init_reason() == CFG_INIT_STALE_NEW);
    CHECK(fixture_find(CFG_DB_NEW_JOURNAL_PATH, 0) != NULL);

    /* tsv が無い / 規則違反なら何も作らない */
    reset_all();
    CHECK(cfg_init(NULL) == OS32_ERR_NOTFOUND);
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);
    reset_all();
    put_file(CFG_TSV_PATH, "gshell\tDesktop\tint\t1\n");
    CHECK(cfg_init(NULL) == OS32_ERR_INVAL);
    CHECK(cfg_last_init_reason() == CFG_INIT_TSV);
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) == NULL);
}

/* ========================================================================= */
/*  (16)(20) rename の失敗                                                   */
/* ========================================================================= */
static void c_rename(void)
{
    /* 新名は出来たが旧名の削除も巻き戻しも失敗 = 同じ inode を 2 名が指す。
     * `.new` を消すと本体が壊れるので、**どちらも消さない**。 */
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    inj_rename_mode = 1;
    CHECK(cfg_init(NULL) == OS32_ERR_NOTEMPTY);
    CHECK(cfg_last_init_reason() == CFG_INIT_AMBIGUOUS);
    CHECK(fixture_find(CFG_DB_PATH, 0) != NULL);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) != NULL);

    /* 何も起きなかった → `.new` を片付けて失敗 */
    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    inj_rename_mode = 2;
    CHECK(cfg_init(NULL) == OS32_ERR_IO);
    CHECK(cfg_last_init_reason() == CFG_INIT_RENAME);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) == NULL);
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);

    /* 旧名は消えたが rename が失敗を返した → 本体を検査して成功にする */
    reset_all();
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    inj_rename_mode = 3;
    CHECK(cfg_init(NULL) == 0);
    CHECK(fixture_find(CFG_DB_PATH, 0) != NULL);
    CHECK(fixture_find(CFG_DB_NEW_PATH, 0) == NULL);
}

/* ========================================================================= */
/*  (10)(21) tsv reader — stdin の fixture を判定するだけ                    */
/* ========================================================================= */
static void c_tsv(void)
{
    static unsigned char blob_in[FIXTURE_BYTES];
    static CfgTsvRow row;
    static CfgTsvSeen seen;
    CfgTsvErr err;
    TsvFile f;
    int n = (int)fread(blob_in, 1, sizeof(blob_in), stdin);
    int rc;

    CHECK(fixture_create(NULL, CFG_TSV_PATH, blob_in, (u32)n) == VFS_OK);
    CHECK(tsv_open(&f, CFG_TSV_PATH) >= 0);
    rc = cfg_tsv_parse(tsv_getc, &f, &row, &seen,
                       (int (*)(const CfgTsvRow *, void *))0, NULL, &err);
    tsv_shut(&f);
    if (rc == 0) printf("TSV ACCEPT %d\n", seen.count);
    else printf("TSV REJECT %d %d\n", err.code, err.lineno);
}

/* ========================================================================= */
/*  cfg コマンド — 引数解釈と整形 (純関数) + 通し                            */
/* ========================================================================= */
static void c_args(void)
{
    CfgArgs a;
    char *av1[] = { "cfg", "status" };
    char *av2[] = { "cfg", "get", "gshell", "k" };
    char *av3[] = { "cfg", "get", "gshell", "k", "9" };
    char *av4[] = { "cfg", "set", "gshell", "k", "int", "5" };
    char *av5[] = { "cfg", "set", "gshell", "k", "bogus", "5" };
    char *av6[] = { "cfg", "list" };
    char *av7[] = { "cfg", "list", "gshell", "desk" };
    char *av8[] = { "cfg", "init", "--tsv", "/x.tsv" };
    char *av9[] = { "cfg", "export", "/out.json" };
    char *av10[] = { "cfg", "nope" };
    char buf[64];
    unsigned char bb[8];

    CHECK(cfg_cmd_parse(2, av1, &a) == 0 && a.cmd == CFG_CMD_STATUS);
    CHECK(cfg_cmd_parse(4, av2, &a) == 0 && a.cmd == CFG_CMD_GET && a.def == NULL);
    CHECK(cfg_cmd_parse(5, av3, &a) == 0 && !strcmp(a.def, "9"));
    CHECK(cfg_cmd_parse(6, av4, &a) == 0 && !strcmp(a.type, "int"));
    CHECK(cfg_cmd_parse(6, av5, &a) == -1);
    CHECK(cfg_cmd_parse(2, av6, &a) == 0 && a.scope == NULL);
    CHECK(cfg_cmd_parse(4, av7, &a) == 0 && !strcmp(a.prefix, "desk"));
    CHECK(cfg_cmd_parse(4, av8, &a) == 0 && !strcmp(a.path, "/x.tsv"));
    CHECK(cfg_cmd_parse(3, av9, &a) == 0 && a.cmd == CFG_CMD_EXPORT);
    CHECK(cfg_cmd_parse(2, av10, &a) == -1);
    CHECK(cfg_cmd_parse(1, av1, &a) == -1);

    CHECK(fmt_int(buf, 64, 0) == 1 && !strcmp(buf, "0"));
    CHECK(fmt_int(buf, 64, -2147483647 - 1) == 11 && !strcmp(buf, "-2147483648"));
    CHECK(fmt_int(buf, 64, 2147483647) == 10 && !strcmp(buf, "2147483647"));
    {
        int v;
        CHECK(parse_int("2147483647", &v) == 0 && v == 2147483647);
        CHECK(parse_int("-2147483648", &v) == 0 && v == -2147483647 - 1);
        CHECK(parse_int("2147483648", &v) == -1);
        CHECK(parse_int("0x10", &v) == -1);
        CHECK(parse_int("", &v) == -1);
        CHECK(parse_int("+1", &v) == -1);
    }
    CHECK(parse_hex("0102ff", bb, 8) == 3 && bb[0] == 1 && bb[2] == 0xFF);
    CHECK(parse_hex("0f0", bb, 8) == -1);
    CHECK(parse_hex("zz", bb, 8) == -1);
    CHECK(parse_hex("", bb, 8) == 0);
    bb[0] = 0xAB; bb[1] = 0xCD;
    CHECK(fmt_hex(buf, 64, bb, 2) == 4 && !strcmp(buf, "abcd"));
    CHECK(fmt_b64(buf, 64, (const unsigned char *)"f", 1) == 4 && !strcmp(buf, "Zg=="));
    CHECK(fmt_b64(buf, 64, (const unsigned char *)"fo", 2) == 4 && !strcmp(buf, "Zm8="));
    CHECK(fmt_b64(buf, 64, (const unsigned char *)"foo", 3) == 4 && !strcmp(buf, "Zm9v"));
    CHECK(fmt_json_str(buf, 64, "a\"b\\c\n", 6) == 9 && !strcmp(buf, "a\\\"b\\\\c\\n"));
    CHECK(!strcmp(status_name(CFG_MISSING), "MISSING"));
    CHECK(!strcmp(type_name(CFG_TYPE_BLOB), "blob"));
}

/* ========================================================================= */
/*  (1)(23) cfg コマンドの通し — status / init / set / get / list / export   */
/* ========================================================================= */
static int ran(const char *a1, const char *a2, const char *a3, const char *a4,
               const char *a5)
{
    char *av[6];
    int n = 1;
    av[0] = (char *)"cfg";
    if (a1) av[n++] = (char *)a1;
    if (a2) av[n++] = (char *)a2;
    if (a3) av[n++] = (char *)a3;
    if (a4) av[n++] = (char *)a4;
    if (a5) av[n++] = (char *)a5;
    cap_len = 0;
    memset(&g_db, 0, sizeof(g_db));
    return cfg_main(n, av, &host_api);
}

static int cap_has(const char *needle)
{
    cap_out[cap_len] = '\0';
    return strstr(cap_out, needle) != NULL;
}

static void c_cmd(void)
{
    /* C1: DB が無ければ MISSING / 終了 1 / ファイルは作られない */
    CHECK(ran("status", NULL, NULL, NULL, NULL) == 1);
    CHECK(cap_has("MISSING"));
    CHECK(fixture_find(CFG_DB_PATH, 0) == NULL);

    /* C2: init → status → list */
    put_file(CFG_TSV_PATH, TSV_OK_TEXT);
    CHECK(ran("init", NULL, NULL, NULL, NULL) == 0);
    CHECK(ran("status", NULL, NULL, NULL, NULL) == 0);
    CHECK(cap_has("OK schema_version 1"));
    CHECK(ran("list", NULL, NULL, NULL, NULL) == 0);
    CHECK(cap_has("gshell\tdesktop/color\tint\t1"));
    CHECK(cap_has("gshell\ttaskbar/clock_24h\tint\t1"));
    CHECK(ran("init", NULL, NULL, NULL, NULL) == 1);
    CHECK(cap_has("already exists"));

    /* C3: set / get */
    CHECK(ran("set", "gshell", "desktop/color", "int", "5") == 0);
    CHECK(ran("get", "gshell", "desktop/color", NULL, NULL) == 0);
    CHECK(cap_has("5\n"));
    CHECK(ran("get", "gshell", "nosuch", NULL, NULL) == 0);
    CHECK(cap_has("(not set)"));
    CHECK(ran("get", "gshell", "nosuch", "42", NULL) == 0);
    CHECK(cap_has("42"));
    CHECK(ran("set", "gshell", "b", "blob", "0102ff") == 0);
    CHECK(ran("get", "gshell", "b", NULL, NULL) == 0);
    CHECK(cap_has("0102ff"));
    CHECK(ran("set", "gshell", "Bad", "int", "1") == 1);
    CHECK(cap_has("set rejected"));
    CHECK(ran("del", "gshell", "b", NULL, NULL) == 0);
    CHECK(ran("get", "gshell", "b", NULL, NULL) == 0);
    CHECK(cap_has("(not set)"));

    /* C4: export — 行数 = 件数 + 1、ヘッダの版が status と一致 */
    CHECK(ran("export", "/s.json", NULL, NULL, NULL) == 0);
    {
        FixtureFile *f = fixture_find("/s.json", 0);
        int lines = 0;
        u32 i;
        CHECK(f != NULL);
        for (i = 0; i < f->size; i++) if (f->data[i] == '\n') lines++;
        CHECK(lines == 4);                 /* ヘッダ + 3 件 */
        f->data[f->size] = '\0';
        CHECK(strstr((char *)f->data, "{\"schema_version\":1,\"exported\":\"7\"}"));
        CHECK(strstr((char *)f->data,
                     "{\"scope\":\"gshell\",\"key\":\"desktop/color\","
                     "\"type\":0,\"v\":5}"));
        CHECK(strstr((char *)f->data,
                     "\"key\":\"desktop/wallpaper\",\"type\":1,\"v\":\"\""));
    }

    /* (23) meta が 2 の DB の export ヘッダは実値 2 */
    reset_all();
    {
        static const char *v2[] = {
            "CREATE TABLE meta (schema_version INTEGER NOT NULL, created TEXT)",
            "CREATE TABLE settings (scope TEXT NOT NULL, key TEXT NOT NULL,"
            " type INTEGER NOT NULL, ival INTEGER, tval TEXT, bval BLOB,"
            " PRIMARY KEY (scope, key)) WITHOUT ROWID",
            "INSERT INTO meta VALUES (2, '0')",
            "INSERT INTO settings VALUES ('gshell','k',0,3,NULL,NULL)",
            NULL
        };
        FixtureFile *f;
        raw_db(CFG_DB_PATH, v2);
        CHECK(ran("export", "/s2.json", NULL, NULL, NULL) == 0);
        f = fixture_find("/s2.json", 0);
        CHECK(f != NULL);
        f->data[f->size] = '\0';
        CHECK(strstr((char *)f->data, "{\"schema_version\":2,"));
        /* VERSION では書けない */
        CHECK(ran("set", "gshell", "k", "int", "4") == 1);
        CHECK(cap_has("cannot write: VERSION"));
    }

    /* MISSING の export は失敗し、案内を出す */
    reset_all();
    CHECK(ran("export", "/s3.json", NULL, NULL, NULL) == 1);
    CHECK(cap_has("cannot export: MISSING"));
    CHECK(cap_has("settings.db missing: run 'cfg init'"));
}

/* ========================================================================= */
/*  list が出力バッファを越える — 閉じて吐いて開き直し、続きから出す          */
/* ========================================================================= */
static void c_list_big(void)
{
    CfgDb *db;
    char key[32], val[201];
    int i, lines = 0, n;

    make_good_db();
    memset(val, 'v', 200);
    val[200] = '\0';
    CHECK(cfg_open(&db, 1) == 0);
    CHECK(cfg_begin(db) == 0);
    for (i = 0; i < 100; i++) {
        sprintf(key, "row/%03d", i);
        CHECK(cfg_set_text(db, "user", key, val) == 0);
    }
    CHECK(cfg_commit(db) == 0);
    CHECK(cfg_close(db) == 0);

    /* 1 行 ~220B x 100 = 22KB > CFG_OUT_MAX (8KB) なので必ず複数回に割れる */
    CHECK(ran("list", NULL, NULL, NULL, NULL) == 0);
    CHECK(cap_len > CFG_OUT_MAX);
    for (i = 0; i < cap_len; i++) if (cap_out[i] == '\n') lines++;
    CHECK(lines == 100);                       /* 落ちも重複もしない */
    cap_out[cap_len] = '\0';
    for (i = 0; i < 100; i++) {
        sprintf(key, "user\trow/%03d\ttext\t", i);
        CHECK(strstr(cap_out, key) != NULL);
        /* 同じ key が 2 度出ていないこと */
        n = 0;
        {
            const char *p = cap_out;
            while ((p = strstr(p, key)) != NULL) { n++; p++; }
        }
        CHECK(n == 1);
    }
    CHECK(cap_yields > 0);                     /* 1KB ごとに間を作っている */
}

/* ========================================================================= */

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(os32_sqlite_init() == SQLITE_OK);
    reset_all();
    cases_run++;

    if (!strcmp(argv[1], "missing")) c_missing();
    else if (!strcmp(argv[1], "corrupt")) c_corrupt();
    else if (!strcmp(argv[1], "version")) c_version();
    else if (!strcmp(argv[1], "types")) c_types();
    else if (!strcmp(argv[1], "roundtrip")) c_roundtrip();
    else if (!strcmp(argv[1], "txn")) c_txn();
    else if (!strcmp(argv[1], "limits")) c_limits();
    else if (!strcmp(argv[1], "enum")) c_enum();
    else if (!strcmp(argv[1], "meta")) c_meta();
    else if (!strcmp(argv[1], "shm_copy")) c_shm_copy();
    else if (!strcmp(argv[1], "init")) c_init();
    else if (!strcmp(argv[1], "rename")) c_rename();
    else if (!strcmp(argv[1], "tsv")) c_tsv();
    else if (!strcmp(argv[1], "args")) c_args();
    else if (!strcmp(argv[1], "cmd")) c_cmd();
    else if (!strcmp(argv[1], "list_big")) c_list_big();
    else CHECK(0);

    canary_check(argv[1]);
    CHECK(memsys5_check_canary() == 0);
    if (strcmp(argv[1], "tsv")) printf("PASS %s\n", argv[1]);
    return 0;
}
