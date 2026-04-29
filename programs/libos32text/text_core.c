/* ======================================================================== */
/*  TEXT_CORE.C — テキスト管理ライブラリ コア実装                           */
/*                                                                          */
/*  初期化・終了、DB接続管理、メッセージ取得、スロット管理、                  */
/*  グループ連続取得、コールバック、デバッグ。                                */
/* ======================================================================== */

#include "libos32text.h"
#include "libos32db.h"
#include "libos32db_util.h"
#include "os32api.h"

/* ====================================================================== */
/*  内部変数                                                                */
/* ====================================================================== */

static db_handle_t text_db = -1;       /* DB接続ハンドル */
static TextSlot    text_slots[TEXT_MAX_SLOTS];
static text_done_callback text_done_cb = 0;

/* 変数テーブル (text_var.c で定義) */
extern TextVar text_vars[TEXT_MAX_VARS];

/* 変数展開 (text_var.c) */
extern int text__expand_vars(char *buf, int buf_size);

/* ====================================================================== */
/*  内部ヘルパー                                                            */
/* ====================================================================== */

/* スロット番号の検証 */
static int slot_valid(int slot)
{
    return (slot >= 0 && slot < TEXT_MAX_SLOTS);
}

/* スロットをゼロクリア */
static void slot_reset(TextSlot *s)
{
    int i;
    for (i = 0; i < (int)sizeof(TextSlot); i++)
        ((u8 *)s)[i] = 0;
}

/* バッファ内の制御記法 \p を解析してページ分割 */
static void parse_pages(TextSlot *s)
{
    int i;
    int page = 0;
    int text_out = 0;

    s->page_offsets[0] = 0;
    s->page_count = 1;

    for (i = 0; i < (int)s->total_len && page < TEXT_MAX_PAGES; i++) {
        if (s->buf[i] == '\\' && (i + 1) < (int)s->total_len) {
            if (s->buf[i + 1] == 'p') {
                /* ページ区切り: \p を除去してページ境界をマーク */
                s->page_ends[page] = text_out;
                page++;
                if (page < TEXT_MAX_PAGES) {
                    s->page_offsets[page] = text_out;
                }
                i++; /* 'p' をスキップ */
                continue;
            }
        }
        /* 制御記法以外はそのままコピー (インプレース) */
        s->buf[text_out++] = s->buf[i];
    }

    /* 最終ページの終端 */
    s->page_ends[page] = text_out;
    s->page_count = page + 1;
    s->total_len = text_out;
}

/* ====================================================================== */
/*  API — システム管理                                                      */
/* ====================================================================== */

int text_init(const char *db_path)
{
    int i;

    /* 全スロットリセット */
    for (i = 0; i < TEXT_MAX_SLOTS; i++)
        slot_reset(&text_slots[i]);

    /* DB接続 */
    text_db = db_open(db_path);
    if (text_db < 0)
        return TEXT_ERR_DB;

    return TEXT_OK;
}

void text_shutdown(void)
{
    int i;

    for (i = 0; i < TEXT_MAX_SLOTS; i++)
        slot_reset(&text_slots[i]);

    if (text_db >= 0) {
        db_close(text_db);
        text_db = -1;
    }

    text_done_cb = 0;
}

/* ====================================================================== */
/*  API — メッセージ操作                                                    */
/* ====================================================================== */

int text_load(int slot, u16 msg_id)
{
    TextSlot *s;
    char sql[128];
    int rc;
    const char *text_str;
    const char *speaker_str;
    int text_len;
    int speed_val;
    int i;

    if (!slot_valid(slot))
        return TEXT_ERR_SLOT;

    s = &text_slots[slot];
    slot_reset(s);

    if (text_db < 0)
        return TEXT_ERR_DB;

    /* SQL組み立て */
    {
        char *p = sql;
        db_sql_append(&p, "SELECT text, speaker, speed FROM messages WHERE id=");
        db_sql_append_int(&p, (int)msg_id);
    }

    rc = db_query(text_db, sql);
    if (rc != DB_STATUS_ROW)
        return TEXT_ERR_NOTFOUND;

    /* テキスト取得 */
    text_str = db_column_text(0);
    speaker_str = db_column_text(1);
    speed_val = db_column_int(2);

    /* テキストバッファにコピー */
    text_len = 0;
    if (text_str) {
        while (text_str[text_len] && text_len < TEXT_BUF_SIZE - 1)
            text_len++;
        for (i = 0; i < text_len; i++)
            s->buf[i] = text_str[i];
    }
    s->buf[text_len] = '\0';
    s->total_len = text_len;

    /* 話者名コピー */
    if (speaker_str && speaker_str[0]) {
        i = 0;
        while (speaker_str[i] && i < TEXT_SPEAKER_SIZE - 1) {
            s->speaker[i] = speaker_str[i];
            i++;
        }
        s->speaker[i] = '\0';
    } else {
        s->speaker[0] = '\0';
    }

    /* ステートメントを完了させる */
    db_finalize(text_db);

    /* 速度設定 */
    s->speed = (speed_val > 0) ? (u8)speed_val : 2;

    /* 変数展開 */
    s->total_len = text__expand_vars(s->buf, TEXT_BUF_SIZE);

    /* ページ分割 */
    parse_pages(s);

    /* 状態を TYPING に */
    s->state = TEXT_STATE_TYPING;
    s->active = 1;
    s->visible_len = 0;
    s->current_page = 0;
    s->counter = 0;
    s->msg_id = msg_id;

    return TEXT_OK;
}

int text_load_group(int slot, u16 group_id)
{
    TextSlot *s;
    char sql[160];
    int rc;
    u16 first_msg_id;

    if (!slot_valid(slot))
        return TEXT_ERR_SLOT;
    if (text_db < 0)
        return TEXT_ERR_DB;

    /* グループ内の最初のメッセージIDを取得 */
    {
        char *p = sql;
        db_sql_append(&p, "SELECT id FROM messages WHERE group_id=");
        db_sql_append_int(&p, (int)group_id);
        db_sql_append(&p, " ORDER BY seq ASC LIMIT 1");
    }

    rc = db_query(text_db, sql);
    if (rc != DB_STATUS_ROW) {
        db_finalize(text_db);
        return TEXT_ERR_NOTFOUND;
    }

    first_msg_id = (u16)db_column_int(0);
    db_finalize(text_db);

    /* 先頭メッセージをロード */
    rc = text_load(slot, first_msg_id);
    if (rc != TEXT_OK)
        return rc;

    /* グループ情報を設定 */
    s = &text_slots[slot];
    s->group_id = group_id;
    s->group_seq = 0;

    return TEXT_OK;
}

int text_next_message(int slot)
{
    TextSlot *s;
    char sql[160];
    int rc;
    u16 next_msg_id;

    if (!slot_valid(slot))
        return TEXT_ERR_SLOT;

    s = &text_slots[slot];
    if (s->group_id == 0)
        return TEXT_ERR_END;

    /* 次のseqのメッセージを検索 */
    {
        char *p = sql;
        db_sql_append(&p, "SELECT id FROM messages WHERE group_id=");
        db_sql_append_int(&p, (int)s->group_id);
        db_sql_append(&p, " AND seq=");
        db_sql_append_int(&p, (int)(s->group_seq + 1));
        db_sql_append(&p, " LIMIT 1");
    }

    rc = db_query(text_db, sql);
    if (rc != DB_STATUS_ROW) {
        db_finalize(text_db);
        return TEXT_ERR_END;
    }

    next_msg_id = (u16)db_column_int(0);
    db_finalize(text_db);

    /* 次メッセージをロード */
    {
        u16 saved_group = s->group_id;
        u16 saved_seq = s->group_seq + 1;

        rc = text_load(slot, next_msg_id);
        if (rc != TEXT_OK)
            return rc;

        /* グループ情報を復元 */
        s = &text_slots[slot];
        s->group_id = saved_group;
        s->group_seq = saved_seq;
    }

    return TEXT_OK;
}

void text_close(int slot)
{
    if (slot_valid(slot))
        slot_reset(&text_slots[slot]);
}

/* ====================================================================== */
/*  API — コールバック                                                      */
/* ====================================================================== */

void text_set_done_callback(text_done_callback cb)
{
    text_done_cb = cb;
}

/* コールバック発火 (text_update.c から呼ばれる) */
void text__fire_done(int slot, u16 msg_id)
{
    if (text_done_cb)
        text_done_cb(slot, msg_id);
}

/* ====================================================================== */
/*  API — デバッグ                                                          */
/* ====================================================================== */

void text_debug_dump(int slot)
{
    extern KernelAPI *kapi;
    KernelAPI *api;
    TextSlot *s;

    if (!slot_valid(slot))
        return;

    api = kapi;
    s = &text_slots[slot];

    api->kprintf(ATTR_WHITE, "[TEXT] Slot %d: active=%d state=%d page=%d/%d\n",
                 slot, s->active, s->state, s->current_page, s->page_count);
    api->kprintf(ATTR_WHITE, "  msg_id=%d group=%d seq=%d speed=%d\n",
                 s->msg_id, s->group_id, s->group_seq, s->speed);
    api->kprintf(ATTR_WHITE, "  visible=%d/%d speaker='%s'\n",
                 s->visible_len, s->total_len, s->speaker);
    if (s->active) {
        int vis_len;
        int page_start = s->page_offsets[s->current_page];
        vis_len = s->visible_len - page_start;
        if (vis_len < 0) vis_len = 0;
        api->kprintf(ATTR_WHITE, "  text(page %d): '%.60s'\n",
                     s->current_page, s->buf + page_start);
    }
}

int text__slot_active_count(void)
{
    int i, count = 0;
    for (i = 0; i < TEXT_MAX_SLOTS; i++) {
        if (text_slots[i].active)
            count++;
    }
    return count;
}

/* ====================================================================== */
/*  内部アクセサ (text_update.c / text_var.c から参照)                      */
/* ====================================================================== */

TextSlot *text__get_slot(int slot)
{
    if (!slot_valid(slot))
        return 0;
    return &text_slots[slot];
}
