/* ======================================================================== */
/*  CFG_INTERNAL.H — libos32cfg の内部境界 (SDK には配らない)                */
/*                                                                          */
/*  KAPI 呼び出しは 1 枚の関数ポインタ表 (CfgBackend) に集める。ホスト TDD は */
/*  ここを実 kapi_db.c + RAM backend に差し替えて、libos32cfg.c を**その      */
/*  まま**載せる (票 §4)。ゲストでは cfg_backend.c が kapi から作る。         */
/* ======================================================================== */

#ifndef CFG_INTERNAL_H
#define CFG_INTERNAL_H

#include "libos32cfg.h"

/* ---- SQLite のコード (libos32cfg が写像に使う分だけ、[C4]) -------------- */
#define CFG_SQLITE_OK             0
#define CFG_SQLITE_ERROR          1    /* no such table 等 */
#define CFG_SQLITE_BUSY           5
#define CFG_SQLITE_CORRUPT       11
#define CFG_SQLITE_CANTOPEN      14
#define CFG_SQLITE_CONSTRAINT    19    /* PRIMARY KEY 重複など */
#define CFG_SQLITE_NOTADB        26
#define CFG_SQLITE_BUSY_RECOVERY (5 | (1 << 8))   /* hot journal */
#define CFG_SQLITE_PRIMARY(c)    ((c) & 0xFF)

/* ---- KAPI 境界 --------------------------------------------------------- */
typedef struct CfgBackend {
    /* v50 */
    int (*db_open_existing)(const char *path, int writable);
    int (*db_prepare_only)(int handle, const char *sql);
    int (*db_bind_int)(int handle, int index, int value);
    int (*db_bind_text)(int handle, int index, const char *text, int length);
    int (*db_bind_blob)(int handle, int index, const void *data, int length);
    int (*db_bind_null)(int handle, int index);
    int (*db_error_code)(int handle);
    /* v42 */
    int (*db_open)(const char *path);          /* CREATE 付き (cfg_init) */
    int (*db_close)(int handle);
    int (*db_exec)(int handle, const char *sql);
    int (*db_step)(int handle);
    int (*db_finalize)(int handle);
    /* 結果ブロック (DB_ResultHeader + DB_ColumnInfo[] + データ) の先頭 */
    unsigned char *(*shm)(void);
    /* VFS */
    int (*sys_stat)(const char *path, OS32_Stat *st);
    int (*sys_rename)(const char *oldpath, const char *newpath);
    int (*sys_unlink)(const char *path);
    int (*sys_open)(const char *path, int mode);
    int (*sys_read)(int fd, void *buf, u32 size);
    void (*sys_close)(int fd);
    u32 (*get_tick)(void);
} CfgBackend;

/* ゲスト側の実体は cfg_backend.c。ホスト TDD は自前の定義で置き換える。 */
const CfgBackend *cfg_backend_platform(void);
/* 試験・特殊用途の差し替え (NULL で既定へ戻す)。 */
void cfg_set_backend(const CfgBackend *b);
const CfgBackend *cfg_backend(void);

/* ---- CfgDb の中身 ------------------------------------------------------ */
struct CfgDb {
    int in_use;
    int handle;          /* KAPI のスロット。-1 = 掴んでいない */
    int want_write;      /* cfg_open(writable) の要求 */
    int rw;              /* 実際に RW で開けたか */
    int status;          /* CFG_* */
    int schema_version;  /* 読めた版 (MISSING / CORRUPT は 0) */
    int last_sqlite;     /* 直前の SQLite 拡張コード */
    int txn;             /* 0 = 無し / 1 = 実行中 / 2 = failed */
    int in_enum;         /* 再入フラグ */
    int orphan_close;    /* 内部で捨てた接続の close 失敗 (0 = 無し) */
    int cleanup_sqlite;  /* 後片付け (ROLLBACK) の失敗。操作失敗とは別に持つ */
};

/* ---- 内部共有 (libos32cfg.c が実装) ------------------------------------ */
int  cfg_i_valid_scope(const char *s);        /* 1 = 規則どおり */
int  cfg_i_valid_key(const char *s);
int  cfg_i_len_ok(const char *s, int max);    /* 1..max バイトの C 文字列 */
int  cfg_i_utf8_check(const void *p, int n);  /* 0 = 妥当 / -1 */

/* 増分 UTF-8 検査 (tsv の長いコメントを溜めずに見るため) */
typedef struct { int need; u32 cp; u32 lo; } CfgU8;
void cfg_i_u8_reset(CfgU8 *s);
int  cfg_i_u8_byte(CfgU8 *s, int b);          /* 0 = ここまで妥当 / -1 */
int  cfg_i_u8_done(const CfgU8 *s);           /* 1 = 途中の列が無い */

/* SHM の列アクセサ (libos32db と同じ読み方) */
int          cfg_i_col_count(void);
int          cfg_i_col_type(int col);
int          cfg_i_col_len(int col);
i32          cfg_i_col_int(int col);
const void  *cfg_i_col_ptr(int col);

/* 単一 statement の実行 (BEGIN / COMMIT / ROLLBACK / DDL)。0 / -1。 */
int  cfg_i_exec(CfgDb *db, const char *sql);
/* prepare_only だけ (bind と step は呼び手)。0 / -1。 */
int  cfg_i_prepare(CfgDb *db, const char *sql);
void cfg_i_note(CfgDb *db);                   /* last_sqlite を取り込む */
/* RO / RW どちらの接続でも同じ schema 検査 (票 §1-1b)。CFG_* を返す。 */
int  cfg_i_schema_check(CfgDb *db, int *version_out);
/* cfg_init から close 失敗コードを共有欄に残す。 */
void cfg_i_set_close_error(int code);

/* ---- tsv reader (cfg_tsv.c、純関数) ------------------------------------ */
#define CFG_TSV_OK        0
#define CFG_TSV_E_CR      1
#define CFG_TSV_E_UTF8    2
#define CFG_TSV_E_COLS    3
#define CFG_TSV_E_SCOPE   4
#define CFG_TSV_E_KEY     5
#define CFG_TSV_E_TYPE    6
#define CFG_TSV_E_VALUE   7
#define CFG_TSV_E_RANGE   8
#define CFG_TSV_E_NUL     9
#define CFG_TSV_E_IO      12
#define CFG_TSV_E_EMIT    13

/* 重複 (scope, key) はここでは見ない。`cfg_init` が流し込む先の
 * `settings` 表が PRIMARY KEY (scope, key) なので、素の INSERT が
 * SQLITE_CONSTRAINT で弾く — 控えを持たないので行数にも名前の長さにも
 * 上限が要らない (往復 1 の ⑯)。 */

typedef struct {
    char          scope[CFG_SCOPE_MAX + 1];
    char          key[CFG_KEY_MAX + 1];
    int           type;                       /* CFG_TYPE_* */
    int           ival;
    char          tval[CFG_TEXT_MAX + 1];
    int           tlen;
    unsigned char bval[CFG_BLOB_MAX];
    int           blen;
    int           lineno;
} CfgTsvRow;

typedef struct { int code; int lineno; } CfgTsvErr;

/* get は 1 バイト (0..255) か EOF の -1、読み取り障害は -2 を返す。
 * row は呼び手が用意する作業領域。emit が非 0 を返したら中断。
 * 0 = 全行受理 / -1 = 規則違反 (err に理由と行番号)。 */
int cfg_tsv_parse(int (*get)(void *ctx), void *gctx, CfgTsvRow *row,
                  int (*emit)(const CfgTsvRow *r, void *ctx), void *ectx,
                  CfgTsvErr *err);

#endif /* CFG_INTERNAL_H */
