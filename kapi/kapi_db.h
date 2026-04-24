/* ======================================================================== */
/*  KAPI_DB.H — SQLite DB KAPI ラッパー ヘッダ                               */
/*                                                                          */
/*  外部プログラムが KAPI テーブル経由で呼ぶ DB 操作関数のプロトタイプ。       */
/*  カーネル内に DB 接続スロット (最大 DB_MAX_CONNECTIONS) を管理する。        */
/* ======================================================================== */

#ifndef KAPI_DB_H
#define KAPI_DB_H

#include "os32_kapi_shared.h"

/* DB オープン — パスからファイル DB (メモリDB は ":memory:") を開く */
/* 戻り値: ハンドル (0以上) / -1=失敗 */
int __cdecl kapi_db_open(const char *path);

/* DB クローズ — ハンドルのステートメント + DB を解放 */
/* 戻り値: 0=成功, -1=失敗 */
int __cdecl kapi_db_close(int handle);

/* SQL 実行 (結果不要の DDL/DML) — 共有メモリにステータスを書き込む */
/* 戻り値: 0=成功, 負数=エラー */
int __cdecl kapi_db_exec(int handle, const char *sql);

/* ステートメント準備 — SELECT 等のクエリを解析 */
/* 共有メモリに最初の行またはステータスを書き込む */
/* 戻り値: 0=DONE, 1=ROW, 負数=エラー */
int __cdecl kapi_db_prepare(int handle, const char *sql);

/* 次の行を取得 — 共有メモリを更新 */
/* 戻り値: 0=DONE (完了), 1=ROW (行あり), 負数=エラー */
int __cdecl kapi_db_step(int handle);

/* カラム値取得 (整数) — 現在の行から col 番目の値を返す */
int __cdecl kapi_db_column_int(int handle, int col);

/* カラム値取得 (文字列) — 共有メモリのデータ領域へのポインタを返す */
const char * __cdecl kapi_db_column_text(int handle, int col);

/* 直前のエラーメッセージ取得 — 共有メモリにコピーして返す */
const char * __cdecl kapi_db_last_error(int handle);

/* プログラム終了時のリソースクリーンアップ (exec_exit から呼ばれる) */
void db_cleanup_all(void);

#endif /* KAPI_DB_H */
