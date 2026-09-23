/* ========================================================================= */
/*  IME_DICT_HOST.C — 辞書の常駐接続の開き直し (票 TASK_VFS_FD_PATH v3 の 6)  */
/*                                                                           */
/*  実物だけを組む: 実 `kernel/ime_dict.c` + 実 `lib/sqlite3/sqlite3.c` +     */
/*  実 `lib/sqlite3/os32_sqlite_vfs.c` + 実 `fs/vfs_fd.c` + RAM バックエンド  */
/*  (tools/tests/sqlite_groups_backend.h)。ルーティングは                     */
/*  vfs_fd_sqlite_host.c の模型 (パスで動く FS)。                             */
/*                                                                           */
/*  失効は VFS (vfs_rm / 置き換え rename / umount) が FD に付ける印なので、   */
/*  ここでは辞書の実体を差し替えた後に**同じ印**を FD 表へ直に付ける。        */
/*                                                                           */
/*  実行: python3 -B tools/tests/test_vfs_fd_path.py ime                     */
/* ========================================================================= */

#define main f2a_main
#include "vfs_fd_sqlite_host.c"
#undef main

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
int vfs_sync(void) { return 0; }
int vfs_rm(const char *path) { return fixture_rm(path); }
int vfs_stat(const char *path, OS32_Stat *st)
{
    FixtureFile *f = fixture_find(path, 0);
    if (!f) return OS32_ERR_NOTFOUND;
    if (st) { memset(st, 0, sizeof(*st)); st->st_size = f->size; }
    return 0;
}

/* 画面 (kprintf) とシリアル (serial_puts) に出た行を数える */
static int n_kprintf, n_reopened;
static char last_kprintf[256];
/* 1 = 検索の失敗 (0hit の診断行) の時点で journal を置く。SQLite 自身の
 * hot journal 処理 (古い接続の読み始め) より**後**、開き直しの判断より**前**に
 * 残っている journal を作るため — 古い接続の失敗した書き込みが残した形 */
static int plant_journal;
static int fixture_create(void *ctx, const char *path, const void *buf, u32 size);
void kprintf(u8 color, const char *fmt, ...)
{
    (void)color; n_kprintf++; str_cpy(last_kprintf, fmt, sizeof(last_kprintf));
    if (plant_journal && strstr(fmt, "0hit")) {
        plant_journal = 0;
        (void)fixture_create(NULL, "/db/fep.db-journal", "JJJJ", 4);
    }
}
void serial_puts(const char *s)
{ if (strstr(s, "reopened")) n_reopened++; }
int utf8_strlen(const u8 *s)
{ int n = 0; while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; } return n; }

/* 読みの失敗の注入 (パスに部分一致する read_stream を n 回 IO にする) */
static int fail_reads;
static u32 fail_from;       /* この offset 以上の読みだけを失敗させる */
static int inj_read(void *ctx, const char *path, void *buf, u32 size, u32 offset)
{
    if (fail_reads > 0 && offset >= fail_from && strstr(path, "fep.db")) {
        fail_reads--;
        return VFS_ERR_IO;
    }
    return fixture_read(ctx, path, buf, size, offset);
}

#include "../../kernel/ime_dict.c"

/* ---- 足場 ---- */
static void make_dict(const char *path, const char *kanji)
{
    sqlite3 *db = 0;
    char sql[256];
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          (const char *)0) == SQLITE_OK);
    snprintf(sql, sizeof(sql),
             "CREATE TABLE dict(yomi TEXT, kanji TEXT, pos_id INT, cost INT);"
             "INSERT INTO dict VALUES('か','%s',1,10);", kanji);
    CHECK(sqlite3_exec(db, sql, 0, 0, 0) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}

/* 実体の差し替え (hsync の置き換え rename と同じ結果): 別名で作った DB の
 * 中身を本名へ移し、別名を消す */
static void replace_dict(const char *kanji)
{
    FixtureFile *src, *dst;
    make_dict("/db/fep.new", kanji);
    src = fixture_find("/db/fep.new", 0);
    dst = fixture_find("/db/fep.db", 0);
    CHECK(src && dst);
    memcpy(dst->data, src->data, src->size);
    dst->size = src->size;
    fixture_rm("/db/fep.new");
}

static int dict_fd(IME_Dict *d) { return d->db ? os32_sqlite_db_fd(d->db) : -1; }

static const char *first_kanji(IME_Dict *d, int *n)
{
    static IME_Result r[4];
    *n = ime_dict_search(d, "か", r, 4);
    return *n > 0 ? r[0].kanji : "";
}

int main(void)
{
    static IME_Dict d;
    int n, fd;

    CHECK(os32_sqlite_init() == SQLITE_OK);
    fixture_init();
    mock_ops.read_stream = inj_read;
    make_dict("/db/fep.db", "蚊");
    CHECK(ime_dict_open(&d, "/db/fep.db") == 0);
    CHECK(!strcmp(first_kanji(&d, &n), "蚊") && n == 1);
    fd = dict_fd(&d);
    CHECK(fd >= 3);
    CHECK(open_files[fd].sqlite_db == 1);     /* rename は BUSY で断られる印 */
    CHECK(open_files[fd].protect == 1);       /* exec_exit の回収から外す印 */
    printf("PASS open\n");

    /* (1) 実体の差し替え → 失効 → 接続ごと開き直して新しい中身で答える */
    replace_dict("課");
    open_files[fd].stale = 1;                 /* vfs_rm / rename が付ける印 */
    CHECK(!strcmp(first_kanji(&d, &n), "課") && n == 1);
    CHECK(n_reopened == 1);
    CHECK(!open_files[fd].in_use || !open_files[fd].stale ||
          dict_fd(&d) != fd);                 /* 古い FD は閉じた */
    CHECK(open_files[dict_fd(&d)].protect == 1);
    CHECK(open_files[dict_fd(&d)].sqlite_db == 1);
    printf("PASS stale_reopen\n");

    /* (2) 失効 1 回につき 1 回。次の失効ではまた開き直す */
    replace_dict("可");
    open_files[dict_fd(&d)].stale = 1;
    CHECK(!strcmp(first_kanji(&d, &n), "可"));
    CHECK(n_reopened == 2);
    printf("PASS stale_each_event\n");

    /* (3) 失効を伴わない I/O エラーは 1 回だけ開き直す (成功で戻す) */
    fail_reads = 1;
    CHECK(!strcmp(first_kanji(&d, &n), "可"));
    CHECK(n_reopened == 3);
    CHECK(d.io_retried == 0);                 /* やり直しが成功したので戻した */
    fail_reads = 1000000;                     /* 読めないまま */
    CHECK(first_kanji(&d, &n)[0] == '\0' && n == 0);
    CHECK(n_reopened == 3);                   /* 開き直しても読めない = 開けない */
    CHECK(d.db == 0);                         /* 辞書無しで動く */
    fail_reads = 0;
    CHECK(first_kanji(&d, &n)[0] == '\0' && n == 0);
    /* 開き直せるが中身の頁だけ読めない: 1 回開き直したら、以後の検索では
     * 開き直さない (読めないまま開き直しを繰り返さない) */
    CHECK(ime_dict_open(&d, "/db/fep.db") == 0);
    fail_from = 1024;                          /* 1 頁目 (schema) は読める */
    fail_reads = 1000000;
    CHECK(first_kanji(&d, &n)[0] == '\0');
    CHECK(n_reopened == 4 && d.db != 0 && d.io_retried == 1);
    CHECK(first_kanji(&d, &n)[0] == '\0');
    CHECK(first_kanji(&d, &n)[0] == '\0');
    CHECK(n_reopened == 4);
    fail_reads = 0;
    fail_from = 0;
    CHECK(!strcmp(first_kanji(&d, &n), "可"));
    CHECK(d.io_retried == 0);                  /* 成功で戻す */
    printf("PASS io_once\n");

    /* (4) hot journal が残っていたら開かない (辞書無し、画面に出す) */
    CHECK(ime_dict_reopen(&d, "/db/fep.db") == 0);
    CHECK(!strcmp(first_kanji(&d, &n), "可"));
    replace_dict("化");
    open_files[dict_fd(&d)].stale = 1;
    n_kprintf = 0;
    plant_journal = 1;
    CHECK(first_kanji(&d, &n)[0] == '\0' && n == 0);
    CHECK(plant_journal == 0);
    CHECK(d.db == 0);
    CHECK(n_kprintf >= 1 && strstr(last_kprintf, "remains"));
    CHECK(fixture_find("/db/fep.db-journal", 0) != NULL);   /* 触らない */
    printf("PASS hot_journal\n");

    /* (5) 長さ 0 の journal は hot ではない — 開き直す */
    fixture_rm("/db/fep.db-journal");
    CHECK(fixture_create(NULL, "/db/fep.db-journal", "", 0) == VFS_OK);
    CHECK(ime_dict_open(&d, "/db/fep.db") == 0);
    replace_dict("火");
    open_files[dict_fd(&d)].stale = 1;
    CHECK(!strcmp(first_kanji(&d, &n), "火"));
    printf("PASS empty_journal\n");

    /* (6) 学習 (UPSERT) も失効で開き直して 1 回やり直す */
    replace_dict("科");
    open_files[dict_fd(&d)].stale = 1;
    n_kprintf = 0;
    ime_dict_learn(&d, "か", "科");
    CHECK(n_kprintf == 0);                    /* Learn failed を出していない */
    CHECK(!strcmp(first_kanji(&d, &n), "科"));
    printf("PASS learn_reopen\n");

    printf("SUMMARY ime_dict PASS\n");
    return 0;
}
