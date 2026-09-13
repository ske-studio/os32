/* ======================================================================== */
/*  LIBOS32CFG.H — 設定レジストリ (/etc/settings.db) のクライアント          */
/*                                                                          */
/*  票 docs/tasks/settings/TASK_S2.md §1 / 契約の正典は S0_FOUNDATION.md §2  */
/*  と DESIGN.md §3〜§5。C89 [C1]、静的リンク、KAPI v50 の                    */
/*  db_open_existing / db_prepare_only / db_bind_* / db_error_code を使う。  */
/*                                                                          */
/*  使い方 (読み)                                                            */
/*      CfgDb *db;                                                          */
/*      if (cfg_open(&db, 0) == 0) {                                        */
/*          int c = cfg_get_int(db, "gshell", "desktop/color", 1);          */
/*          cfg_close(db);                                                  */
/*      }                                                                   */
/*  使い方 (書き — OS だけ。gshell の設定 UI と cfg コマンド、票 §7)          */
/*      cfg_open(&db, 1); cfg_begin(db); cfg_set_int(...); cfg_commit(db);   */
/*      cfg_close(db);                                                      */
/*                                                                          */
/*  規則                                                                     */
/*    - open 〜 close は 1 回の実行 / 1 イベントの中で閉じ、間で yield しない。*/
/*      協調型なので接続は構造的に同時 1 本になる (票 §7 の決裁)。            */
/*    - X4 / IRQ / 描画 callback からは呼ばない (票 §1-6)。                   */
/*    - 1 プロセス 1 接続。CfgDb は静的 1 本で malloc しない。                */
/*    - DB が無い / 壊れている / 版が新しい場合も cfg_open は 0 を返す。       */
/*      呼び手を分岐させないため — 状態は cfg_status() で見る。               */
/* ======================================================================== */

#ifndef LIBOS32CFG_H
#define LIBOS32CFG_H

#include "os32api.h"

/* ---- 状態 (cfg_status) ------------------------------------------------- */
#define CFG_OK       0   /* 開けて schema も一致した */
#define CFG_MISSING  1   /* DB が無い (fallback で動作) */
#define CFG_CORRUPT  2   /* 0 バイト / NOTADB / CORRUPT / hot journal */
#define CFG_VERSION  3   /* schema_version が自分より新しい (読みだけ) */
#define CFG_ERROR    4   /* I/O 等 (詳細は cfg_last_sqlite()) */

/* ---- 値の型 (settings.type、DESIGN §3) --------------------------------- */
#define CFG_TYPE_INT   0
#define CFG_TYPE_TEXT  1
#define CFG_TYPE_BLOB  2

/* ---- 場所と版 ([C4] 定数はここが管理元) -------------------------------- */
#define CFG_DB_PATH             "/etc/settings.db"
#define CFG_DB_JOURNAL_PATH     "/etc/settings.db-journal"
#define CFG_DB_NEW_PATH         "/etc/settings.db.new"
#define CFG_DB_NEW_JOURNAL_PATH "/etc/settings.db.new-journal"
#define CFG_TSV_PATH            "/etc/settings.tsv"
#define CFG_SCHEMA_VERSION      1

/* ---- 値の上限 (S0_FOUNDATION §2-5) ------------------------------------- */
#define CFG_SCOPE_MAX    63     /* UTF-8 バイト数 (NUL を含まない) */
#define CFG_KEY_MAX      63
#define CFG_TEXT_MAX     255
#define CFG_BLOB_MAX     4096
#define CFG_ENUM_MAX     256    /* cfg_enum が一度に集められる件数 */
#define CFG_SCOPES_MAX   32     /* cfg_enum_scopes の上限 */

/* ---- cfg_init の失敗理由 (cfg_last_init_reason) ------------------------ */
#define CFG_INIT_NONE        0
#define CFG_INIT_EXISTS      1   /* /etc/settings.db が既にある */
#define CFG_INIT_JOURNAL     2   /* /etc/settings.db-journal が残っている */
#define CFG_INIT_STALE_NEW   3   /* /etc/settings.db.new* の残骸 */
#define CFG_INIT_TSV         4   /* tsv が読めない / 規則違反 */
#define CFG_INIT_BUILD       5   /* .new の生成に失敗 */
#define CFG_INIT_CLOSE       6   /* .new の db_close に失敗 (隔離、消さない) */
#define CFG_INIT_RENAME      7   /* rename に失敗し .new だけが残った */
#define CFG_INIT_AMBIGUOUS   8   /* rename 後に本体と .new の両方がある */
#define CFG_INIT_VERIFY      9   /* 生成した DB の自己確認に失敗 */
#define CFG_INIT_STAT       10   /* /etc/settings.db の stat が I/O で失敗 */

typedef struct CfgDb CfgDb;

/* ---- 接続 -------------------------------------------------------------- */
/* /etc/settings.db を開く。0 = 開けた (状態は cfg_status)。
 * 負 = OS32_ERR_* (引数不正・二重 open)。 */
int   cfg_open(CfgDb **out, int writable);
/* 未 commit は rollback してから close。0 / 負。
 * 失敗したときは最初の失敗コードを cfg_last_close_error() に残す
 * (CfgDb は静的 1 本なので close 後も cfg_last_sqlite 等は読める)。 */
int   cfg_close(CfgDb *db);
int   cfg_last_close_error(void);
int   cfg_schema_version(const CfgDb *db);
int   cfg_status(const CfgDb *db);
int   cfg_last_sqlite(const CfgDb *db);

/* ---- 読み -------------------------------------------------------------- */
int   cfg_get_int (CfgDb *db, const char *scope, const char *key, int def);
/* 戻り: 長さ (NUL 除く) / 負: OS32_ERR_NOTFOUND (無い) /
 *       OS32_ERR_NOSPC (cap 不足: out は書かない) / OS32_ERR_INVAL */
int   cfg_get_text(CfgDb *db, const char *scope, const char *key,
                   char *out, int cap);
int   cfg_get_blob(CfgDb *db, const char *scope, const char *key,
                   void *out, int cap);

/* ---- 書き (writable かつ CFG_OK のときだけ) ---------------------------- */
int   cfg_begin(CfgDb *db);
int   cfg_set_int (CfgDb *db, const char *scope, const char *key, int v);
int   cfg_set_text(CfgDb *db, const char *scope, const char *key, const char *s);
int   cfg_set_blob(CfgDb *db, const char *scope, const char *key,
                   const void *p, int n);
int   cfg_delete  (CfgDb *db, const char *scope, const char *key);
int   cfg_commit(CfgDb *db);
int   cfg_rollback(CfgDb *db);

/* ---- 列挙 -------------------------------------------------------------- */
/* 戻り: callback を呼んだ件数 / 負 (CFG_ENUM_MAX を超えたら OS32_ERR_NOSPC で
 * callback は 1 度も呼ばない、callback 内の cfg_* は OS32_ERR_INVAL)。
 * callback の中では値を取らない — key を集めてから外で get する。 */
int   cfg_enum(CfgDb *db, const char *scope, const char *prefix,
               int (*fn)(const char *key, int type, void *ctx), void *ctx);
int   cfg_enum_scopes(CfgDb *db, int (*fn)(const char *scope, void *ctx),
                      void *ctx);

/* ---- 生成 (`cfg init` の実体) ------------------------------------------ */
/* DB の場所は固定 CFG_DB_PATH。tsv_path が NULL なら CFG_TSV_PATH。
 * 0 / 負 (理由は cfg_last_init_reason())。 */
int   cfg_init(const char *tsv_path);
int   cfg_last_init_reason(void);

#endif /* LIBOS32CFG_H */
