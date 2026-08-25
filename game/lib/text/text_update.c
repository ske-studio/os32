/* ======================================================================== */
/*  TEXT_UPDATE.C — テキスト演出制御                                        */
/*                                                                          */
/*  タイプライター進行、ページ制御、状態取得。                                */
/*  毎フレーム text_update() を呼び出して全スロットを更新する。               */
/* ======================================================================== */

#include "libos32text.h"

/* text_core.c の内部アクセサ */
extern TextSlot *text__get_slot(int slot);
extern void text__fire_done(int slot, u16 msg_id);

/* ====================================================================== */
/*  内部ヘルパー                                                            */
/* ====================================================================== */

/* UTF-8の1文字のバイト数を返す (先頭バイトから判定) */
static int utf8_char_bytes(u8 lead)
{
    if (lead < 0x80) return 1;       /* ASCII */
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;  /* 不正なバイトは1バイト扱い */
}

/* 一時停止制御記法 \wNN を解析
 * buf[pos] が '\' で buf[pos+1] が 'w' の場合、数値を読み取る
 * 戻り値: 消費バイト数 (0=マッチなし) */
static int parse_pause(const char *buf, int pos, int total_len, u16 *out_frames)
{
    int start;
    u16 val;

    if (pos + 2 > total_len)
        return 0;
    if (buf[pos] != '\\' || buf[pos + 1] != 'w')
        return 0;

    /* 数値読み取り */
    start = pos + 2;
    val = 0;
    pos = start;
    while (pos < total_len && buf[pos] >= '0' && buf[pos] <= '9') {
        val = val * 10 + (buf[pos] - '0');
        pos++;
    }

    if (pos == start)
        return 0;  /* 数値なし */

    *out_frames = val;
    return pos - (start - 2);
}

/* 1スロットのタイプライター進行 */
static void update_slot(int slot_id, TextSlot *s)
{
    u16 page_start;
    u16 page_end;

    if (!s->active || s->state != TEXT_STATE_TYPING)
        return;

    /* フレームカウンタ進行 */
    s->counter++;
    if (s->counter < s->speed)
        return;
    s->counter = 0;

    /* 現在ページの範囲 */
    page_start = s->page_offsets[s->current_page];
    page_end = s->page_ends[s->current_page];

    /* 現在位置がページ開始未満なら補正 */
    if (s->visible_len < page_start)
        s->visible_len = page_start;

    /* 一時停止チェック (\wNN) */
    {
        u16 pause_frames;
        int consumed = parse_pause(s->buf, s->visible_len, s->total_len, &pause_frames);
        if (consumed > 0) {
            s->visible_len += consumed;
            s->pause_remaining = pause_frames;
            s->state = TEXT_STATE_PAUSE;
            return;
        }
    }

    /* UTF-8 1文字分進行 */
    if (s->visible_len < page_end) {
        int char_len = utf8_char_bytes((u8)s->buf[s->visible_len]);
        s->visible_len += char_len;
        if (s->visible_len > page_end)
            s->visible_len = page_end;
    }

    /* ページ末尾到達チェック */
    if (s->visible_len >= page_end) {
        if (s->current_page + 1 < s->page_count) {
            /* まだ次ページあり → 入力待ち */
            s->state = TEXT_STATE_WAIT;
        } else {
            /* 最終ページ完了 */
            s->state = TEXT_STATE_DONE;
            text__fire_done(slot_id, s->msg_id);
        }
    }
}

/* 一時停止の進行 */
static void update_pause(TextSlot *s)
{
    if (!s->active || s->state != TEXT_STATE_PAUSE)
        return;

    if (s->pause_remaining > 0) {
        s->pause_remaining--;
        return;
    }

    /* 一時停止完了 → TYPING に復帰 */
    s->state = TEXT_STATE_TYPING;
    s->counter = 0;
}

/* ====================================================================== */
/*  API — テキスト演出制御                                                  */
/* ====================================================================== */

void text_update(void)
{
    int i;
    for (i = 0; i < TEXT_MAX_SLOTS; i++) {
        TextSlot *s = text__get_slot(i);
        if (!s) continue;

        if (s->state == TEXT_STATE_PAUSE)
            update_pause(s);
        else if (s->state == TEXT_STATE_TYPING)
            update_slot(i, s);
    }
}

void text_skip(int slot)
{
    TextSlot *s = text__get_slot(slot);
    u16 page_end;

    if (!s || !s->active)
        return;
    if (s->state != TEXT_STATE_TYPING && s->state != TEXT_STATE_PAUSE)
        return;

    /* 現在ページの全文を表示 */
    page_end = s->page_ends[s->current_page];
    s->visible_len = page_end;
    s->pause_remaining = 0;

    /* 状態遷移 */
    if (s->current_page + 1 < s->page_count) {
        s->state = TEXT_STATE_WAIT;
    } else {
        s->state = TEXT_STATE_DONE;
        text__fire_done(slot, s->msg_id);
    }
}

int text_advance(int slot)
{
    TextSlot *s = text__get_slot(slot);

    if (!s || !s->active)
        return TEXT_ERR_SLOT;
    if (s->state != TEXT_STATE_WAIT && s->state != TEXT_STATE_DONE)
        return TEXT_ERR_SLOT;

    /* 次ページへ */
    if (s->current_page + 1 < s->page_count) {
        s->current_page++;
        s->visible_len = s->page_offsets[s->current_page];
        s->state = TEXT_STATE_TYPING;
        s->counter = 0;
        return TEXT_OK;
    }

    /* 最終ページ → 完了 */
    return TEXT_ERR_END;
}

void text_set_speed(int slot, u8 speed)
{
    TextSlot *s = text__get_slot(slot);
    if (s && speed > 0)
        s->speed = speed;
}

/* ====================================================================== */
/*  API — 状態取得                                                          */
/* ====================================================================== */

u8 text_get_state(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s)
        return TEXT_STATE_IDLE;
    return s->state;
}

const char *text_get_visible(int slot, int *out_len)
{
    TextSlot *s = text__get_slot(slot);
    u16 page_start;
    int len;

    if (!s || !s->active) {
        if (out_len) *out_len = 0;
        return 0;
    }

    page_start = s->page_offsets[s->current_page];
    len = (int)s->visible_len - (int)page_start;
    if (len < 0) len = 0;

    if (out_len) *out_len = len;
    return s->buf + page_start;
}

const char *text_get_speaker(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s || !s->active)
        return 0;
    return s->speaker;
}

u8 text_get_page(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s)
        return 0;
    return s->current_page;
}

u8 text_get_page_count(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s)
        return 0;
    return s->page_count;
}

u16 text_get_msg_id(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s)
        return 0;
    return s->msg_id;
}

int text_is_active(int slot)
{
    TextSlot *s = text__get_slot(slot);
    if (!s)
        return 0;
    return s->active;
}
