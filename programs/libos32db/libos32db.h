/* ======================================================================== */
/*  LIBOS32DB.H — OS32 SQLite ユーザー空間ラッパー                           */
/*                                                                          */
/*  外部プログラムが KAPI 経由で SQLite DB にアクセスするための                */
/*  シンプルなラッパーライブラリ。                                             */
/*                                                                          */
/*  使い方:                                                                  */
/*    db_handle_t db = db_open("/db/test.db");                               */
/*    db_exec(db, "CREATE TABLE ...");                                       */
/*    db_query(db, "SELECT ...");                                            */
/*    while (db_step(db) == DB_STATUS_ROW) {                                 */
/*        int val = db_column_int(0);                                        */
/*        const char *text = db_column_text(1);                              */
/*    }                                                                      */
/*    db_close(db);                                                          */
/* ======================================================================== */

#ifndef LIBOS32DB_H
#define LIBOS32DB_H

#include "os32api.h"

/* DB ハンドル (カーネルのスロット番号をラップ) */
typedef int db_handle_t;

/* 接続管理 */
db_handle_t db_open(const char *path);
int         db_close(db_handle_t h);

/* SQL 実行 (結果セットなしの DDL/DML) */
int db_exec(db_handle_t h, const char *sql);

/* クエリ実行 (結果セットあり — 最初の行を自動取得) */
/* 戻り値: DB_STATUS_ROW / DB_STATUS_DONE / DB_STATUS_ERROR */
int db_query(db_handle_t h, const char *sql);

/* イテレータ — 次の行を取得 */
/* 戻り値: DB_STATUS_ROW / DB_STATUS_DONE / DB_STATUS_ERROR */
int db_step(db_handle_t h);

/* カラムアクセサ (共有メモリ上のデータを直接参照) */
int         db_column_count(void);
int         db_column_type(int col);
i32         db_column_int(int col);
const char *db_column_text(int col);
int         db_column_bytes(int col);

/* エラー情報 (共有メモリ上のエラーメッセージを参照) */
const char *db_errmsg(void);

#endif /* LIBOS32DB_H */
