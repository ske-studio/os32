/* ======================================================================== */
/*  KAPI_DB.C — SQLite DB KAPI ラッパー実装                                  */
/*                                                                          */
/*  外部プログラムが KAPI 関数テーブル経由で SQLite を操作するための            */
/*  カーネル側ブリッジ。DB 接続スロットで最大 DB_MAX_CONNECTIONS 個の           */
/*  同時接続を管理する。                                                      */
/*                                                                          */
/*  結果データは共有メモリ (MEM_SHM_BASE) に DB_ResultHeader +               */
/*  DB_ColumnInfo[] + データ の形式で書き込まれる。                            */
/* ======================================================================== */

#include "kapi_db.h"
#include "os32_sqlite_config.h"
#include "sqlite3.h"
#include "kstring.h"
#include "kprintf.h"
#include "os32_sqlite_vfs.h"
#include "memmap.h"

/* kapi_db.c はリンカスクリプトで EXCLUDE_FILE に含まれていないため、
 * 通常の .text に配置される。sqlite3_exec 等は .sqlite_text にあるが、
 * カーネルの .text から .sqlite_text を呼ぶのは問題ない。
 * ただし、SQL文字列は外部プログラム空間 (0x400000+) から渡される。
 * sqlite3_exec が SQL を読み取る際にページフォールトが発生する場合、
 * カーネル側にコピーする必要がある。
 */
#define SQL_COPY_BUF_SIZE 1024
static char sql_copy_buf[SQL_COPY_BUF_SIZE];

/* パス文字列コピー用バッファ (外部プログラム空間からの読み取り問題回避) */
#define PATH_COPY_BUF_SIZE 256
static char path_copy_buf[PATH_COPY_BUF_SIZE];

/* ======== DB接続スロット ======== */
typedef struct {
    int in_use;               /* 1=使用中, 0=空き */
    sqlite3 *db;              /* SQLite 接続ハンドル */
    sqlite3_stmt *active_stmt; /* 実行中のステートメント */
} DbSlot;

static DbSlot db_slots[DB_MAX_CONNECTIONS];

/* 共有メモリベースアドレス (IPC用) */
#define DB_SHM_PTR   ((u8 *)MEM_SHM_BASE)

/* ======================================================================== */
/*  ヘルパー: スロット検証                                                   */
/* ======================================================================== */
static DbSlot *slot_get(int handle)
{
    if (handle < 0 || handle >= DB_MAX_CONNECTIONS) return (DbSlot *)0;
    if (!db_slots[handle].in_use) return (DbSlot *)0;
    return &db_slots[handle];
}

/* ======================================================================== */
/*  ヘルパー: 共有メモリにエラー情報を書き込む                                */
/* ======================================================================== */
static void shm_write_error(DbSlot *slot)
{
    DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
    const char *errmsg;
    i32 data_start;
    i32 max_len;

    hdr->status = DB_STATUS_ERROR;
    hdr->column_count = 0;

    /* エラーメッセージをデータ領域に書き込む */
    data_start = (i32)sizeof(DB_ResultHeader);
    hdr->error_offset = data_start;

    if (slot && slot->db) {
        errmsg = sqlite3_errmsg(slot->db);
    } else {
        errmsg = "invalid handle";
    }

    max_len = DB_SHM_BLOCK_SIZE - data_start - 1;
    if (max_len > 0) {
        kstrncpy((char *)(DB_SHM_PTR + data_start), errmsg, (u32)max_len);
    }
}

/* ======================================================================== */
/*  ヘルパー: db_step 結果を共有メモリに書き込む                              */
/* ======================================================================== */
static int shm_write_row(DbSlot *slot)
{
    DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
    DB_ColumnInfo *cols;
    int ncol;
    i32 data_offset;
    int i;

    if (!slot->active_stmt) {
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    }

    ncol = sqlite3_column_count(slot->active_stmt);
    hdr->status = DB_STATUS_ROW;
    hdr->column_count = (i32)ncol;
    hdr->error_offset = 0;

    /* カラム情報配列の開始位置 */
    cols = (DB_ColumnInfo *)(DB_SHM_PTR + sizeof(DB_ResultHeader));

    /* データ領域の開始位置 */
    data_offset = (i32)(sizeof(DB_ResultHeader) + (u32)ncol * sizeof(DB_ColumnInfo));

    for (i = 0; i < ncol; i++) {
        int col_type = sqlite3_column_type(slot->active_stmt, i);
        i32 remaining = DB_SHM_BLOCK_SIZE - data_offset;

        cols[i].data_offset = data_offset;

        switch (col_type) {
        case SQLITE_INTEGER: {
            i32 val = (i32)sqlite3_column_int(slot->active_stmt, i);
            cols[i].type = DB_TYPE_INT;
            cols[i].length = 4;
            if (remaining >= 4) {
                *(i32 *)(DB_SHM_PTR + data_offset) = val;
                data_offset += 4;
            }
            break;
        }
        case SQLITE_TEXT: {
            const char *text = (const char *)sqlite3_column_text(slot->active_stmt, i);
            int len = sqlite3_column_bytes(slot->active_stmt, i);
            cols[i].type = DB_TYPE_TEXT;
            cols[i].length = len;
            if (text && len < remaining - 1) {
                memcpy(DB_SHM_PTR + data_offset, text, (u32)len);
                DB_SHM_PTR[data_offset + len] = '\0';
                data_offset += len + 1;
            } else {
                /* データが大きすぎる場合はオフセットを無効化 */
                cols[i].data_offset = 0;
            }
            break;
        }
        case SQLITE_FLOAT: {
            /* float は i32 に切り捨て (簡易対応) */
            double dval = sqlite3_column_double(slot->active_stmt, i);
            i32 ival = (i32)dval;
            cols[i].type = DB_TYPE_FLOAT;
            cols[i].length = 4;
            if (remaining >= 4) {
                *(i32 *)(DB_SHM_PTR + data_offset) = ival;
                data_offset += 4;
            }
            break;
        }
        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(slot->active_stmt, i);
            int len = sqlite3_column_bytes(slot->active_stmt, i);
            cols[i].type = DB_TYPE_BLOB;
            cols[i].length = len;
            if (blob && len < remaining) {
                memcpy(DB_SHM_PTR + data_offset, blob, (u32)len);
                data_offset += len;
            } else {
                cols[i].data_offset = 0;
            }
            break;
        }
        case SQLITE_NULL:
        default:
            cols[i].type = DB_TYPE_NULL;
            cols[i].length = 0;
            cols[i].data_offset = 0;
            break;
        }
    }

    return DB_STATUS_ROW;
}

/* ======================================================================== */
/*  KAPI 関数実装                                                            */
/* ======================================================================== */

int __cdecl kapi_db_open(const char *path)
{
    int i;
    int rc;

    /* 空きスロットを探す */
    for (i = 0; i < DB_MAX_CONNECTIONS; i++) {
        if (!db_slots[i].in_use) break;
    }
    if (i >= DB_MAX_CONNECTIONS) return -1;

    /* パス文字列をカーネルバッファにコピー (外部プログラム空間からの読み取り問題回避) */
    kstrncpy(path_copy_buf, path, PATH_COPY_BUF_SIZE - 1);
    path_copy_buf[PATH_COPY_BUF_SIZE - 1] = '\0';

    rc = sqlite3_open(path_copy_buf, &db_slots[i].db);
    if (rc != SQLITE_OK) {
        if (db_slots[i].db) {
            sqlite3_close(db_slots[i].db);
            db_slots[i].db = (sqlite3 *)0;
        }
        return -1;
    }

    /* ジャーナルモード設定 (ファイルDB のクラッシュリカバリ用) */
    sqlite3_exec(db_slots[i].db, "PRAGMA journal_mode=DELETE", 0, 0, 0);

    db_slots[i].in_use = 1;
    db_slots[i].active_stmt = (sqlite3_stmt *)0;
    return i;
}

int __cdecl kapi_db_close(int handle)
{
    DbSlot *slot = slot_get(handle);
    if (!slot) return -1;

    /* 実行中ステートメントの finalize */
    if (slot->active_stmt) {
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
    }

    sqlite3_close(slot->db);
    slot->db = (sqlite3 *)0;
    slot->in_use = 0;
    return 0;
}

int __cdecl kapi_db_exec(int handle, const char *sql)
{
    DbSlot *slot = slot_get(handle);
    DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
    sqlite3_stmt *stmt = (sqlite3_stmt *)0;
    int rc;

    if (!slot) {
        shm_write_error((DbSlot *)0);
        return -1;
    }

    /* 前のステートメントがあれば解放 */
    if (slot->active_stmt) {
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
    }

    /* SQL文字列をカーネルバッファにコピー (外部プログラム空間からの読み取り問題回避) */
    kstrncpy(sql_copy_buf, sql, SQL_COPY_BUF_SIZE - 1);
    sql_copy_buf[SQL_COPY_BUF_SIZE - 1] = '\0';



    /* prepare */
    rc = sqlite3_prepare_v2(slot->db, sql_copy_buf, -1, &stmt, 0);
    if (rc != SQLITE_OK || !stmt) {
        kprintf(0x04, "[DB] prepare fail rc=%d\n", rc);
        shm_write_error(slot);
        return -1;
    }

    /* step */
    rc = sqlite3_step(stmt);

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        shm_write_error(slot);
        return -1;
    }

    hdr->status = DB_STATUS_DONE;
    hdr->column_count = 0;
    hdr->error_offset = 0;
    return 0;
}

int __cdecl kapi_db_prepare(int handle, const char *sql)
{
    DbSlot *slot = slot_get(handle);
    int rc;
    int step_rc;

    if (!slot) {
        shm_write_error((DbSlot *)0);
        return -1;
    }

    /* 前のステートメントがあれば解放 */
    if (slot->active_stmt) {
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
    }

    /* SQL文字列をカーネルバッファにコピー */
    kstrncpy(sql_copy_buf, sql, SQL_COPY_BUF_SIZE - 1);
    sql_copy_buf[SQL_COPY_BUF_SIZE - 1] = '\0';

    rc = sqlite3_prepare_v2(slot->db, sql_copy_buf, -1, &slot->active_stmt, 0);
    if (rc != SQLITE_OK) {
        shm_write_error(slot);
        return -1;
    }

    /* 最初の行を自動取得 */
    step_rc = sqlite3_step(slot->active_stmt);
    if (step_rc == SQLITE_ROW) {
        return shm_write_row(slot);
    } else if (step_rc == SQLITE_DONE) {
        DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    } else {
        shm_write_error(slot);
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        return -1;
    }
}

int __cdecl kapi_db_step(int handle)
{
    DbSlot *slot = slot_get(handle);
    int rc;

    if (!slot) {
        shm_write_error((DbSlot *)0);
        return -1;
    }

    if (!slot->active_stmt) {
        DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    }

    rc = sqlite3_step(slot->active_stmt);
    if (rc == SQLITE_ROW) {
        return shm_write_row(slot);
    } else if (rc == SQLITE_DONE) {
        DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    } else {
        shm_write_error(slot);
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        return -1;
    }
}

int __cdecl kapi_db_column_int(int handle, int col)
{
    DbSlot *slot = slot_get(handle);
    if (!slot || !slot->active_stmt) return 0;
    return sqlite3_column_int(slot->active_stmt, col);
}

const char * __cdecl kapi_db_column_text(int handle, int col)
{
    DbSlot *slot = slot_get(handle);
    DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
    DB_ColumnInfo *info;

    if (!slot || !slot->active_stmt) return "";

    /* 共有メモリ上のカラム情報からデータ位置を参照 */
    if (col < 0 || col >= hdr->column_count) return "";
    info = (DB_ColumnInfo *)(DB_SHM_PTR + sizeof(DB_ResultHeader)
                             + (u32)col * sizeof(DB_ColumnInfo));
    if (info->data_offset == 0) return "";
    return (const char *)(DB_SHM_PTR + info->data_offset);
}

/* ステートメント手動 finalize — 結果セット途中放棄時に使用 */
/* db_step(DONE) 時は自動 finalize されるため、通常は不要 */
/* 戻り値: 0=成功, -1=失敗 */
int __cdecl kapi_db_finalize(int handle)
{
    DbSlot *slot = slot_get(handle);
    if (!slot) return -1;
    if (slot->active_stmt) {
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
    }
    return 0;
}

const char * __cdecl kapi_db_last_error(int handle)
{
    DbSlot *slot = slot_get(handle);
    if (!slot || !slot->db) return "invalid handle";
    return sqlite3_errmsg(slot->db);
}

u32 __cdecl kapi_db_mem_used(void)
{
    return (u32)sqlite3_memory_used();
}

/* ======================================================================== */
/*  db_cleanup_all — exec_exit() から呼ばれるリソースクリーンアップ           */
/* ======================================================================== */
void db_cleanup_all(void)
{
    int i;
    for (i = 0; i < DB_MAX_CONNECTIONS; i++) {
        if (db_slots[i].in_use) {
            if (db_slots[i].active_stmt) {
                sqlite3_finalize(db_slots[i].active_stmt);
                db_slots[i].active_stmt = (sqlite3_stmt *)0;
            }
            sqlite3_close(db_slots[i].db);
            db_slots[i].db = (sqlite3 *)0;
            db_slots[i].in_use = 0;
        }
    }
}
