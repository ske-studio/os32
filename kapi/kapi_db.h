/* ======================================================================== */
/*  KAPI_DB.H — SQLite DB KAPI ラッパー ヘッダ                               */
/*                                                                          */
/*  外部プログラムが KAPI テーブル経由で呼ぶ DB 操作関数のプロトタイプ。       */
/*  カーネル内に DB 接続スロット (最大 DB_MAX_CONNECTIONS) を管理する。        */
/*                                                                          */
/*  制約:                                                                   */
/*    - SQL 文字列は最大 1024 バイト (SQL_COPY_BUF_SIZE)。超過分は切り捨て。 */
/* ======================================================================== */

#ifndef KAPI_DB_H
#define KAPI_DB_H

#include "os32_kapi_shared.h"

/* DB オープン — パスからファイル DB (メモリDB は ":memory:") を開く */
/* 戻り値: ハンドル (0以上) / -1=失敗 */
int __cdecl kapi_db_open(const char *path);

/* DB クローズ — finalize -> 必要なら rollback -> close。
 * 戻り値: 0=全段成功, -1=失敗。close 成功時だけ slot を解放する。
 * close 失敗接続は再操作/再回収/再利用せず隔離 (FD 保護は F2 の前提作業)。 */
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

/* ステートメント手動 finalize — 結果セット途中放棄時に使用 */
/* db_step(DONE) 時は自動 finalize されるため、通常は不要 */
/* 戻り値: 0=成功, -1=失敗 */
int __cdecl kapi_db_finalize(int handle);

/* 通常は sqlite3_errmsg()。open/close 回収中の最初の失敗はコピーを保持。
 * close 後も slot 再利用までは診断を取得できる (世代付き handle ではない)。
 * open 失敗の即時診断は SHM。owner 別 handle=-1 診断 ABI は F4 の範囲。 */
const char * __cdecl kapi_db_last_error(int handle);

/* MEMSYS5 メモリ使用量取得 (デバッグ・モニタリング用) */
/* 戻り値: sqlite3_memory_used() の値 (バイト単位) */
u32 __cdecl kapi_db_mem_used(void);

/* 既存 exec_exit 用の全 active slot 回収を F2 まで維持。隔離 slot は除外。 */
void db_cleanup_all(void);

/* Internal only: not a public KAPI entry; exec integration belongs to F2. */
void db_cleanup_owned(int owner);

#endif /* KAPI_DB_H */
