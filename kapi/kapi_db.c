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
#include "os32_kapi_slots.h"   /* v50: 自己診断が slot 番号を名前で見る */
#include "os32_sqlite_config.h"
#include "sqlite3.h"
#include "kstring.h"
#include "kprintf.h"
#include "os32_sqlite_vfs.h"
#include "memmap.h"
#include "fd_redirect.h"
#include "vfs.h"          /* v50: open 前の vfs_stat (本体 size / journal) */
#include "exec.h"         /* v50: ring3_user_range_ok (ユーザポインタ検証) */

/* kapi_db.c はリンカスクリプトで EXCLUDE_FILE に含まれていないため、
 * 通常の .text に配置される。sqlite3_exec 等は .sqlite_text にあるが、
 * カーネルの .text から .sqlite_text を呼ぶのは問題ない。
 * ただし、SQL文字列は外部プログラム空間 (0x400000+) から渡される。
 * sqlite3_exec が SQL を読み取る際にページフォールトが発生する場合、
 * カーネル側にコピーする必要がある。
 */
#define SQL_COPY_BUF_SIZE DB_SQL_MAX_BYTES
static char sql_copy_buf[SQL_COPY_BUF_SIZE];

/* パス文字列コピー用バッファ (外部プログラム空間からの読み取り問題回避) */
#define PATH_COPY_BUF_SIZE OS32_MAX_PATH
static char path_copy_buf[PATH_COPY_BUF_SIZE];

/* v50 (票 S0-K §1a): 検証後のコピー先は **1 本ずつの静的スクラッチ**。
 * bind は SQLITE_TRANSIENT なので SQLite が自分で写す = 次の呼び出しで
 * 上書きしてよい。SQLite に呼び手のポインタを保持させないための置き場。 */
static char text_copy_buf[DB_BIND_TEXT_MAX + 1];
static u8   blob_copy_buf[DB_BIND_BLOB_MAX];

/* `<path>-journal` を組み立てる場所。path は NUL 込み OS32_MAX_PATH 以内
 * (= 本体 255 バイト) なので、末尾 8 バイト + NUL がちょうど収まる。 */
#define DB_JOURNAL_SUFFIX "-journal"
static char journal_buf[OS32_MAX_PATH + 8];

/* ======== DB接続スロット ======== */
#define DB_ERROR_SIZE 256
typedef struct {
    int in_use;               /* 1=使用中, 0=空き */
    int owner;                /* open 時の res_owner_get() */
    int isolated;             /* close failed: never touch this connection again */
    int cleanup_error;        /* first teardown failure, retained until reuse */
    char cleanup_message[DB_ERROR_SIZE];
    sqlite3 *db;              /* SQLite 接続ハンドル */
    sqlite3_stmt *active_stmt; /* 実行中のステートメント */
    /* ---- v50 (票 S0-K §1a) ---- */
    int last_error;           /* 「最後の失敗」拡張コード (診断専用) */
    int used_once;            /* 一度でも open されたか (未使用 slot = MISUSE) */
    int bindable;             /* prepare_only 済みで、まだ step していない */
} DbSlot;

static DbSlot db_slots[DB_MAX_CONNECTIONS];

/* v50: 呼び手 (owner) ごとの「直前の open 失敗」。db_error_code(-1) で読む。
 * open は slot を取れないまま失敗しうるので、slot とは別の欄が要る。 */
static int db_open_fail[DB_OWNER_SLOTS];

/* 共有メモリベースアドレス (IPC用) */
#define DB_SHM_PTR   ((u8 *)MEM_SHM_BASE)

/* ======================================================================== */
/*  ヘルパー: スロット検証                                                   */
/* ======================================================================== */
static DbSlot *slot_get(int handle)
{
    if (handle < 0 || handle >= DB_MAX_CONNECTIONS) return (DbSlot *)0;
    if (!db_slots[handle].in_use || db_slots[handle].isolated)
        return (DbSlot *)0;
    return &db_slots[handle];
}

/* ======================================================================== */
/*  ヘルパー: 診断専用の slot 引き (v50)                                      */
/*                                                                          */
/*  slot_get() は解放済み / 隔離 slot を弾くので、db_error_code には使えない。 */
/*  「閉じた handle でも slot が再利用されるまで最後の失敗を返す」ためには    */
/*  in_use / isolated を見ない引き方が要る。                                  */
/* ======================================================================== */
static DbSlot *slot_diag(int handle)
{
    if (handle < 0 || handle >= DB_MAX_CONNECTIONS) return (DbSlot *)0;
    return &db_slots[handle];
}

/* v50 の「最後の失敗」規則 (票 §1a)。データ操作 (open_existing / prepare_only /
 * bind_* / step / exec) は **成功で 0 に**、失敗でそのコードに更新する。 */
static void slot_note(DbSlot *slot, int rc)
{
    if (slot) slot->last_error = rc;
}

/* finalize / close は **失敗したときだけ** 更新する。後片付けが原因診断を
 * 消さないための規則 (FOUNDATION §4、Codex 往復 3 の 2)。 */
static void slot_note_teardown(DbSlot *slot, int rc)
{
    if (slot && rc != SQLITE_OK) slot->last_error = rc;
}

/* SQLite の拡張 result code。接続が無い / 拡張コードが無いときは rc のまま。 */
static int slot_ext(DbSlot *slot, int rc)
{
    int ext;
    if (!slot || !slot->db) return rc;
    ext = sqlite3_extended_errcode(slot->db);
    return ext != SQLITE_OK ? ext : rc;
}

/* v50: owner 別の「直前 open 失敗」欄。owner が池の外なら記録しない。 */
static void open_fail_set(int code)
{
    int owner = res_owner_get();
    if (owner < 0 || owner >= DB_OWNER_SLOTS) return;
    db_open_fail[owner] = code;
}

/* ======================================================================== */
/*  ヘルパー: ユーザポインタの検証とコピー (票 S0-K §1a)                      */
/*                                                                          */
/*  ディスパッチャの早期検証は先頭番地しか見ない。長さ付き / NUL 終端の       */
/*  引数はここで範囲を確かめてから **カーネル側のスクラッチへ写す**。         */
/*  CPL=0 の呼び手 (常駐シェル / gshell の直呼び) では ring3_user_range_ok が  */
/*  素通しになるので、NULL と長さと容量だけを見ることになる。                 */
/* ======================================================================== */
static int db_user_range_ok(const void *p, u32 len)
{
    u32 a = (u32)p;
    if (!p) return 0;
    if (a + len < a) return 0;            /* 加算 overflow */
    return ring3_user_range_ok(a, len);
}

/* 上限 cap (NUL 込み) の中で NUL を探しながら dst へ写す。
 * 1 バイトずつ検証するので、途中のページが非 present でもそこで止まる。
 * 戻り値: 1 = 写した / 0 = NULL・帯外・cap 内に NUL が無い (切り捨てない)。 */
static int db_user_str_copy(const char *src, char *dst, u32 cap)
{
    u32 i;
    if (!src) return 0;
    for (i = 0; i < cap; i++) {
        if (!db_user_range_ok(src + i, 1)) return 0;
        dst[i] = src[i];
        if (dst[i] == '\0') return 1;
    }
    return 0;
}

/* prepare_only の pzTail が「次の statement を含まない」か (票 §1a)。
 * 空白・行コメント (--)・ブロックコメント・空の区切り (;) だけなら真。 */
static int sql_tail_is_blank(const char *t)
{
    if (!t) return 1;
    while (*t) {
        if (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n' || *t == ';') {
            t++;
            continue;
        }
        if (t[0] == '-' && t[1] == '-') {
            t += 2;
            while (*t && *t != '\n') t++;
            continue;
        }
        if (t[0] == '/' && t[1] == '*') {
            t += 2;
            while (*t && !(t[0] == '*' && t[1] == '/')) t++;
            if (!*t) return 1;            /* 閉じていない = 以降は SQL ではない */
            t += 2;
            continue;
        }
        return 0;                         /* 次の statement がある */
    }
    return 1;
}

/* ======================================================================== */
/*  ヘルパー: 共有メモリにエラー情報を書き込む                                */
/* ======================================================================== */
static void shm_write_error_text(const char *errmsg)
{
    DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
    i32 data_start;
    i32 max_len;

    hdr->status = DB_STATUS_ERROR;
    hdr->column_count = 0;

    /* エラーメッセージをデータ領域に書き込む */
    data_start = (i32)sizeof(DB_ResultHeader);
    hdr->error_offset = data_start;

    max_len = DB_SHM_BLOCK_SIZE - data_start - 1;
    if (max_len > 0) {
        kstrncpy((char *)(DB_SHM_PTR + data_start), errmsg, (u32)max_len);
    }
}

static void shm_write_error(DbSlot *slot)
{
    const char *errmsg;

    if (slot && slot->cleanup_error != SQLITE_OK) {
        errmsg = slot->cleanup_message;
    } else if (slot && slot->db) {
        errmsg = sqlite3_errmsg(slot->db);
    } else {
        errmsg = "invalid handle";
    }
    shm_write_error_text(errmsg);
}

/* ======================================================================== */
/*  ヘルパー: 1 行が 16KB の結果ブロックに収まるか (票 S0-K §1b、F5 の前倒し) */
/*                                                                          */
/*  header + **全列の descriptor** + payload の合計で見る。descriptor 領域だけ */
/*  でも 16KB を超えられる (列数の上限は SQLite 側の 2000) ので、1 列ずつ     */
/*  「入るなら書く」では遅い — 書き始める前に総量を数える。                   */
/*  戻り値: 1 = 収まる / 0 = 超過 (呼び手は -1 で返し、部分 ROW を返さない)。 */
/* ======================================================================== */
/* 純関数の側 (自己診断・ホスト試験が stmt 無しで踏める)。 */
static int shm_row_fits_n(int ncol, u32 payload)
{
    u32 need = (u32)sizeof(DB_ResultHeader);

    if (ncol < 0) return 0;
    if ((u32)ncol > ((u32)DB_SHM_BLOCK_SIZE - need) / (u32)sizeof(DB_ColumnInfo))
        return 0;
    need += (u32)ncol * (u32)sizeof(DB_ColumnInfo);
    if (payload > (u32)DB_SHM_BLOCK_SIZE - need) return 0;
    return 1;
}

static int shm_row_fits(sqlite3_stmt *stmt, int ncol)
{
    u32 payload = 0;
    int i;

    if (!shm_row_fits_n(ncol, 0)) return 0;   /* descriptor だけで溢れる列数 */
    for (i = 0; i < ncol; i++) {
        u32 add;
        int len;
        switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER:
        case SQLITE_FLOAT:
            add = 4u;
            break;
        case SQLITE_TEXT:
            len = sqlite3_column_bytes(stmt, i);
            if (len < 0) return 0;
            add = (u32)len + 1u;          /* 終端 NUL の分 */
            break;
        case SQLITE_BLOB:
            len = sqlite3_column_bytes(stmt, i);
            if (len < 0) return 0;
            add = (u32)len;
            break;
        default:
            add = 0u;                      /* NULL は payload を持たない */
            break;
        }
        if (add > (u32)DB_SHM_BLOCK_SIZE - payload) return 0;  /* 加算 overflow */
        payload += add;
    }
    return shm_row_fits_n(ncol, payload);
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
    /* v50 (票 §1b): 収まらない行は **1 バイトも書かず** に失敗させる。
     * 部分 ROW も返さない — 呼び手が「取れた」と誤解しないため。
     * stmt は生かしたまま返す (finalize は呼び手の db_finalize / db_close)。 */
    if (!shm_row_fits(slot->active_stmt, ncol)) {
        slot_note(slot, SQLITE_TOOBIG);
        shm_write_error_text("row exceeds the 16KB result block");
        return DB_STATUS_ERROR;
    }
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

static void slot_save_error(DbSlot *slot, int rc);

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

    db_slots[i].cleanup_error = SQLITE_OK;
    db_slots[i].cleanup_message[0] = '\0';
    /* v50: slot を配り直したので診断欄も初期化する (前の持ち主の「最後の
     * 失敗」はここで消える = 再利用まで保持、という規則の実装)。 */
    db_slots[i].last_error = SQLITE_OK;
    db_slots[i].used_once = 1;
    db_slots[i].bindable = 0;
    db_slots[i].owner = res_owner_get();
    rc = sqlite3_open(path_copy_buf, &db_slots[i].db);
    if (rc != SQLITE_OK) {
        /* Open can fail with a live connection. Save before teardown. */
        slot_save_error(&db_slots[i], rc);
        slot_note(&db_slots[i], slot_ext(&db_slots[i], rc));
        open_fail_set(db_slots[i].last_error);
        shm_write_error(&db_slots[i]);
        if (db_slots[i].db) {
            db_slots[i].in_use = 1;
            kapi_db_close(i);
        }
        return -1;
    }

    /* ジャーナルモード設定 (ファイルDB のクラッシュリカバリ用) */
    sqlite3_exec(db_slots[i].db, "PRAGMA journal_mode=DELETE", 0, 0, 0);

    db_slots[i].in_use = 1;
    db_slots[i].active_stmt = (sqlite3_stmt *)0;
    open_fail_set(SQLITE_OK);
    return i;
}

/* Copy before any subsequent SQLite call can replace the diagnostic. */
static void slot_save_error(DbSlot *slot, int rc)
{
    if (rc == SQLITE_OK || slot->cleanup_error != SQLITE_OK) return;
    slot->cleanup_error = rc;
    kstrncpy(slot->cleanup_message,
             slot->db ? sqlite3_errmsg(slot->db) : sqlite3_errstr(rc),
             DB_ERROR_SIZE - 1);
    slot->cleanup_message[DB_ERROR_SIZE - 1] = '\0';
}

int __cdecl kapi_db_close(int handle)
{
    DbSlot *slot = slot_get(handle);
    int rc;
    if (!slot) return -1;

    /* 実行中ステートメントの finalize */
    if (slot->active_stmt) {
        int frc = sqlite3_finalize(slot->active_stmt);
        slot_save_error(slot, frc);
        slot_note_teardown(slot, frc);   /* v50: 失敗したときだけ診断へ */
        slot->active_stmt = (sqlite3_stmt *)0;
    }
    slot->bindable = 0;

    if (!sqlite3_get_autocommit(slot->db)) {
        int rrc = sqlite3_exec(slot->db, "ROLLBACK", 0, 0, 0);
        slot_save_error(slot, rrc);
        slot_note_teardown(slot, rrc);
    }
    rc = sqlite3_close(slot->db);
    if (rc != SQLITE_OK) {
        slot_save_error(slot, rc);
        slot_note_teardown(slot, rc);
        /* Never retry, even after this nest/owner is recycled. This does NOT
         * protect main/journal/temp FDs against generic FD cleanup: F2 must
         * establish complete VFS tracking/quarantine before integration. */
        slot->isolated = 1;
        return -1;
    }
    slot->db = (sqlite3 *)0;
    slot->in_use = 0;
    return slot->cleanup_error == SQLITE_OK ? 0 : -1;
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
    slot->bindable = 0;

    /* SQL文字列をカーネルバッファにコピー (外部プログラム空間からの読み取り問題回避) */
    kstrncpy(sql_copy_buf, sql, SQL_COPY_BUF_SIZE - 1);
    sql_copy_buf[SQL_COPY_BUF_SIZE - 1] = '\0';



    /* prepare */
    rc = sqlite3_prepare_v2(slot->db, sql_copy_buf, -1, &stmt, 0);
    if (rc != SQLITE_OK || !stmt) {
        kprintf(0x04, "[DB] prepare fail rc=%d\n", rc);
        slot_note(slot, slot_ext(slot, rc != SQLITE_OK ? rc : SQLITE_MISUSE));
        shm_write_error(slot);
        return -1;
    }

    /* step */
    rc = sqlite3_step(stmt);
    /* v50: 自動 finalize が診断を差し替える前に拡張コードを控える。 */
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) slot_note(slot, slot_ext(slot, rc));

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        shm_write_error(slot);
        return -1;
    }

    slot_note(slot, SQLITE_OK);
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
    slot->bindable = 0;

    /* SQL文字列をカーネルバッファにコピー */
    kstrncpy(sql_copy_buf, sql, SQL_COPY_BUF_SIZE - 1);
    sql_copy_buf[SQL_COPY_BUF_SIZE - 1] = '\0';

    rc = sqlite3_prepare_v2(slot->db, sql_copy_buf, -1, &slot->active_stmt, 0);
    if (rc != SQLITE_OK) {
        slot_note(slot, slot_ext(slot, rc));
        shm_write_error(slot);
        return -1;
    }

    /* 最初の行を自動取得 */
    step_rc = sqlite3_step(slot->active_stmt);
    if (step_rc == SQLITE_ROW) {
        slot_note(slot, SQLITE_OK);
        return shm_write_row(slot);
    } else if (step_rc == SQLITE_DONE) {
        DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
        slot_note(slot, SQLITE_OK);
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    } else {
        /* v50: 自動 finalize より前に拡張コードを控える。 */
        slot_note(slot, slot_ext(slot, step_rc));
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

    slot->bindable = 0;    /* v50: 最初の step 以降は bind を受け付けない */
    rc = sqlite3_step(slot->active_stmt);
    if (rc == SQLITE_ROW) {
        slot_note(slot, SQLITE_OK);
        return shm_write_row(slot);
    } else if (rc == SQLITE_DONE) {
        DB_ResultHeader *hdr = (DB_ResultHeader *)DB_SHM_PTR;
        slot_note(slot, SQLITE_OK);
        sqlite3_finalize(slot->active_stmt);
        slot->active_stmt = (sqlite3_stmt *)0;
        hdr->status = DB_STATUS_DONE;
        hdr->column_count = 0;
        hdr->error_offset = 0;
        return DB_STATUS_DONE;
    } else {
        /* v50: 自動 finalize より前に拡張コードを控える。 */
        slot_note(slot, slot_ext(slot, rc));
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
        /* v50: 戻り値は従来どおり 0 のまま (既存 10 本の挙動は不変)。
         * 失敗したときだけ診断欄を更新する。 */
        slot_note_teardown(slot, sqlite3_finalize(slot->active_stmt));
        slot->active_stmt = (sqlite3_stmt *)0;
    }
    slot->bindable = 0;
    return 0;
}

const char * __cdecl kapi_db_last_error(int handle)
{
    DbSlot *slot;
    if (handle < 0 || handle >= DB_MAX_CONNECTIONS) return "invalid handle";
    slot = &db_slots[handle];
    if (slot->cleanup_error != SQLITE_OK) return slot->cleanup_message;
    if (!slot->in_use || !slot->db) return "invalid handle";
    return sqlite3_errmsg(slot->db);
}

u32 __cdecl kapi_db_mem_used(void)
{
    return (u32)sqlite3_memory_used();
}

/* ======================================================================== */
/*  v50 (票 S0-K §1a) — 設定レジストリの基盤 7 本                            */
/* ======================================================================== */

/* ASCII の大文字小文字を無視した比較 (PRAGMA の戻り値の照合にだけ使う)。 */
static int db_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* RW 要求のとき DELETE journal が成立しているかを **問い合わせだけ** で見る。
 * `PRAGMA journal_mode` (= を付けない形) は照会で、モードを 1 ビットも変えない。 */
static int db_journal_mode_is_delete(sqlite3 *db)
{
    sqlite3_stmt *stmt = (sqlite3_stmt *)0;
    const char *mode;
    int ok = 0;

    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, 0) != SQLITE_OK ||
        !stmt) {
        if (stmt) sqlite3_finalize(stmt);
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        mode = (const char *)sqlite3_column_text(stmt, 0);
        ok = mode != (const char *)0 && db_ieq(mode, "delete");
    }
    sqlite3_finalize(stmt);
    return ok;
}

/* `<path>-journal` を journal_buf に組み立てる。1 = 組み立てた / 0 = 入らない。 */
static int db_journal_name(const char *path)
{
    u32 want = kstrlen(path) + (u32)sizeof(DB_JOURNAL_SUFFIX) - 1u;

    if (want + 1u > (u32)sizeof(journal_buf)) return 0;
    kstrncpy(journal_buf, path, (u32)sizeof(journal_buf));
    journal_buf[sizeof(journal_buf) - 1] = '\0';
    kstrncat(journal_buf, DB_JOURNAL_SUFFIX, (u32)sizeof(journal_buf));
    return kstrlen(journal_buf) == want;
}

int __cdecl kapi_db_open_existing(const char *path, int writable)
{
    int i, rc, flags;
    OS32_Stat st;
    sqlite3 *db = (sqlite3 *)0;

    if (writable != 0 && writable != 1) {
        open_fail_set(SQLITE_MISUSE);
        return -1;
    }
    /* 上限内で NUL を探しながら写す。超過は切り捨てず拒否。 */
    if (!db_user_str_copy(path, path_copy_buf, PATH_COPY_BUF_SIZE) ||
        path_copy_buf[0] == '\0' ||
        db_ieq(path_copy_buf, ":memory:") ||
        kstrncmp(path_copy_buf, "file:", 5) == 0) {
        open_fail_set(SQLITE_MISUSE);
        return -1;
    }

    /* (1) 本体が存在し size > 0 か。**SQLite を呼ぶ前**に見る (Codex 往復 2 の 1:
     * SQLite の hasHotJournal は RO でも 0 ページの DB に付随する journal を
     * 消してしまうので、その手前で止める = 副作用ゼロ)。 */
    if (vfs_stat(path_copy_buf, &st) != 0) {
        open_fail_set(SQLITE_CANTOPEN);
        return -1;
    }
    if (st.st_size == 0) {
        open_fail_set(SQLITE_NOTADB);
        return -1;
    }
    /* (2) hot journal があれば RO / RW とも失敗。回復は S3 の明示操作。 */
    if (!db_journal_name(path_copy_buf)) {
        open_fail_set(SQLITE_CANTOPEN);
        return -1;
    }
    if (vfs_stat(journal_buf, &st) == 0) {
        open_fail_set(SQLITE_BUSY_RECOVERY);
        return -1;
    }

    /* 空きスロット */
    for (i = 0; i < DB_MAX_CONNECTIONS; i++) {
        if (!db_slots[i].in_use) break;
    }
    if (i >= DB_MAX_CONNECTIONS) {
        open_fail_set(SQLITE_FULL);
        return -1;
    }

    db_slots[i].cleanup_error = SQLITE_OK;
    db_slots[i].cleanup_message[0] = '\0';
    db_slots[i].last_error = SQLITE_OK;
    db_slots[i].used_once = 1;
    db_slots[i].bindable = 0;
    db_slots[i].active_stmt = (sqlite3_stmt *)0;
    db_slots[i].owner = res_owner_get();

    /* CREATE も URI も付けない。vfs は既定の "os32"。 */
    flags = writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
    rc = sqlite3_open_v2(path_copy_buf, &db, flags, (const char *)0);
    db_slots[i].db = db;
    if (rc != SQLITE_OK) {
        slot_save_error(&db_slots[i], rc);
        slot_note(&db_slots[i], slot_ext(&db_slots[i], rc));
        open_fail_set(db_slots[i].last_error);
        shm_write_error(&db_slots[i]);
        if (db) {
            db_slots[i].in_use = 1;
            kapi_db_close(i);
        }
        return -1;
    }

    /* RW は DELETE journal の成立を確認する。RO では変更 PRAGMA も照会も
     * 実行しない (FOUNDATION §2-2: RO は元ファイルと journal に触らない)。 */
    if (writable && !db_journal_mode_is_delete(db)) {
        db_slots[i].in_use = 1;
        slot_note(&db_slots[i], SQLITE_CANTOPEN);
        open_fail_set(SQLITE_CANTOPEN);
        shm_write_error(&db_slots[i]);
        kapi_db_close(i);
        return -1;
    }

    db_slots[i].in_use = 1;
    open_fail_set(SQLITE_OK);
    return i;
}

int __cdecl kapi_db_prepare_only(int handle, const char *sql)
{
    DbSlot *slot = slot_get(handle);
    sqlite3_stmt *stmt = (sqlite3_stmt *)0;
    const char *tail = (const char *)0;
    int rc;

    if (!slot) {
        shm_write_error((DbSlot *)0);
        return -1;
    }
    if (!db_user_str_copy(sql, sql_copy_buf, SQL_COPY_BUF_SIZE) ||
        sql_copy_buf[0] == '\0') {
        slot_note(slot, SQLITE_MISUSE);
        shm_write_error_text("sql is empty, unterminated or over the limit");
        return -1;
    }

    /* 旧 stmt は finalize して置換する。 */
    if (slot->active_stmt) {
        slot_note_teardown(slot, sqlite3_finalize(slot->active_stmt));
        slot->active_stmt = (sqlite3_stmt *)0;
    }
    slot->bindable = 0;

    rc = sqlite3_prepare_v2(slot->db, sql_copy_buf, -1, &stmt, &tail);
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) sqlite3_finalize(stmt);
        slot_note(slot, slot_ext(slot, rc != SQLITE_OK ? rc : SQLITE_MISUSE));
        shm_write_error(slot);
        return -1;
    }
    /* 次の statement が続いていれば拒否 (末尾の空白 / コメント / ; は可)。 */
    if (!sql_tail_is_blank(tail)) {
        sqlite3_finalize(stmt);
        slot_note(slot, SQLITE_MISUSE);
        shm_write_error_text("only a single statement is allowed");
        return -1;
    }

    slot->active_stmt = stmt;     /* step はしない */
    slot->bindable = 1;
    slot_note(slot, SQLITE_OK);
    return 0;
}

/* bind の共通前提: prepare_only 済み・最初の step の前・index は 1-based。 */
static DbSlot *bind_slot(int handle, int index)
{
    DbSlot *slot = slot_get(handle);
    if (!slot) return (DbSlot *)0;
    if (!slot->active_stmt || !slot->bindable) {
        slot_note(slot, SQLITE_MISUSE);
        return (DbSlot *)0;
    }
    if (index < 1 || index > sqlite3_bind_parameter_count(slot->active_stmt)) {
        slot_note(slot, SQLITE_RANGE);
        return (DbSlot *)0;
    }
    return slot;
}

static int bind_done(DbSlot *slot, int rc)
{
    slot_note(slot, rc == SQLITE_OK ? SQLITE_OK : slot_ext(slot, rc));
    return rc == SQLITE_OK ? 0 : -1;
}

int __cdecl kapi_db_bind_int(int handle, int index, int value)
{
    DbSlot *slot = bind_slot(handle, index);
    if (!slot) return -1;
    return bind_done(slot, sqlite3_bind_int(slot->active_stmt, index, value));
}

int __cdecl kapi_db_bind_text(int handle, int index, const char *text, int length)
{
    DbSlot *slot = bind_slot(handle, index);
    if (!slot) return -1;
    if (length < 0 || length > DB_BIND_TEXT_MAX ||
        !db_user_range_ok(text, (u32)length)) {
        slot_note(slot, SQLITE_MISUSE);
        return -1;
    }
    if (length > 0) kmemcpy(text_copy_buf, text, (u32)length);
    text_copy_buf[length] = '\0';
    /* 0B でも非 NULL の空値。SQLITE_TRANSIENT なので SQLite が写す。 */
    return bind_done(slot, sqlite3_bind_text(slot->active_stmt, index,
                                             text_copy_buf, length,
                                             SQLITE_TRANSIENT));
}

int __cdecl kapi_db_bind_blob(int handle, int index, const void *data, int length)
{
    DbSlot *slot = bind_slot(handle, index);
    if (!slot) return -1;
    if (length < 0 || length > DB_BIND_BLOB_MAX ||
        !db_user_range_ok(data, (u32)length)) {
        slot_note(slot, SQLITE_MISUSE);
        return -1;
    }
    if (length > 0) kmemcpy(blob_copy_buf, data, (u32)length);
    return bind_done(slot, sqlite3_bind_blob(slot->active_stmt, index,
                                             blob_copy_buf, length,
                                             SQLITE_TRANSIENT));
}

int __cdecl kapi_db_bind_null(int handle, int index)
{
    DbSlot *slot = bind_slot(handle, index);
    if (!slot) return -1;
    return bind_done(slot, sqlite3_bind_null(slot->active_stmt, index));
}

int __cdecl kapi_db_error_code(int handle)
{
    DbSlot *slot;
    int owner;

    if (handle == -1) {
        owner = res_owner_get();
        if (owner < 0 || owner >= DB_OWNER_SLOTS) return SQLITE_MISUSE;
        return db_open_fail[owner];
    }
    slot = slot_diag(handle);           /* in_use / isolated を見ない診断引き */
    if (!slot || !slot->used_once) return SQLITE_MISUSE;
    return slot->last_error;
}

/* ======================================================================== */
/*  db_v50_selftest — ブート時に踏む v50 の骨 (kernel/kselftest.c から)      */
/* ======================================================================== */
u32 db_v50_selftest(void)
{
    u32 bad = 0;
    u32 hdr = (u32)sizeof(DB_ResultHeader);
    u32 desc = (u32)sizeof(DB_ColumnInfo);

    /* (0) 表の件数と slot 番号 (末尾追記で 201..207、data_fields はその後ろ)。*/
    if (KAPI_SLOT_COUNT != 208) bad |= 1u << 0;
    if (KAPI_SLOT_DB_OPEN_EXISTING != 201) bad |= 1u << 0;
    if (KAPI_SLOT_DB_ERROR_CODE != KAPI_SLOT_COUNT - 1) bad |= 1u << 0;
    if (KAPI_SLOT_DB_ERROR_CODE - KAPI_SLOT_DB_OPEN_EXISTING != 6) bad |= 1u << 0;
    if (KAPI_SLOT_DB_OPEN != 140) bad |= 1u << 0;   /* 既存 10 本は動かない */

    /* (1) SHM の境界: ちょうど収まる / 1 バイト超過 / descriptor だけで溢れる */
    if (!shm_row_fits_n(1, (u32)DB_SHM_BLOCK_SIZE - hdr - desc)) bad |= 1u << 1;
    if (shm_row_fits_n(1, (u32)DB_SHM_BLOCK_SIZE - hdr - desc + 1u)) bad |= 1u << 1;
    if (shm_row_fits_n((int)(((u32)DB_SHM_BLOCK_SIZE - hdr) / desc) + 1, 0))
        bad |= 1u << 1;
    if (shm_row_fits_n(-1, 0)) bad |= 1u << 1;

    /* (2) ポインタ検証の NULL / overflow (帯と PTE は CPL=3 側でしか踏めない) */
    if (db_user_range_ok((const void *)0, 1u)) bad |= 1u << 2;
    if (db_user_range_ok((const void *)0xFFFFFF00UL, 0x200u)) bad |= 1u << 2;

    /* (3) 単一 statement の判定 (末尾の空白 / コメント / ; は可) */
    if (!sql_tail_is_blank("")) bad |= 1u << 3;
    if (!sql_tail_is_blank("  \t\r\n")) bad |= 1u << 3;
    if (!sql_tail_is_blank(" -- trailing")) bad |= 1u << 3;
    if (!sql_tail_is_blank(" /* c */ ;")) bad |= 1u << 3;
    if (sql_tail_is_blank(" SELECT 2")) bad |= 1u << 3;

    /* (4) owner 別の欄が ID の池 (1 = シェル帯 .. 5) を覆っている */
    if (DB_OWNER_SLOTS < 6) bad |= 1u << 4;

    return bad;
}

/* ======================================================================== */
/*  db_cleanup_all — exec_exit() から呼ばれるリソースクリーンアップ           */
/* ======================================================================== */
void db_cleanup_owned(int owner)
{
    int i;
    for (i = 0; i < DB_MAX_CONNECTIONS; i++) {
        if (db_slots[i].in_use && !db_slots[i].isolated &&
            db_slots[i].owner == owner)
            kapi_db_close(i);
    }
}

void db_cleanup_all(void)
{
    int i;
    for (i = 0; i < DB_MAX_CONNECTIONS; i++) {
        if (db_slots[i].in_use && !db_slots[i].isolated)
            kapi_db_close(i);
    }
}
