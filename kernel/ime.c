/* ======================================================================== */
/*  IME.C — OS32 カーネルFEP メインロジック                                  */
/*                                                                          */
/*  IME状態管理・キー処理・プリエディット表示・公開API                        */
/*  ローマ字変換は ime_romkana.c、辞書検索は ime_dict.c に分離               */
/* ======================================================================== */

#include "ime.h"
#include "kbd.h"
#include "tvram.h"
#include "console.h"
#include "utf8.h"
#include "kprintf.h"
#include "kstring.h"
#include "os32_kapi_shared.h"

/* ======================================================================== */
/*  グローバル IME 状態                                                      */
/* ======================================================================== */

static IME_State g_ime;

/* 辞書ファイルのパス */
#define IME_DICT_PATH  "/sys/fep.dic"

/* ======================================================================== */
/*  プリエディット表示 (TVRAM 25行目 = y=24)                                 */
/* ======================================================================== */

#define IME_PREEDIT_ROW  24   /* TVRAM最終行 (0始まり) */

static void preedit_clear(void)
{
    int x;
    for (x = 0; x < TVRAM_COLS; x++) {
        tvram_putchar_at(x, IME_PREEDIT_ROW, ' ', ATTR_WHITE);
    }
}

static void preedit_draw(void)
{
    int x = 0;
    const u8 *p;
    u16 jis;
    utf8_decode_t dec;
    u8 ank;

    preedit_clear();

    /* モード表示 */
    if (g_ime.mode == IME_MODE_HIRAGANA) {
        tvram_putchar_at(x++, IME_PREEDIT_ROW, '[', ATTR_CYAN);
        jis = unicode_to_jis(0x3042); /* あ */
        if (jis) { tvram_putkanji_at(x, IME_PREEDIT_ROW, jis, ATTR_CYAN); x += 2; }
        tvram_putchar_at(x++, IME_PREEDIT_ROW, ']', ATTR_CYAN);
    } else if (g_ime.mode == IME_MODE_KATAKANA) {
        tvram_putchar_at(x++, IME_PREEDIT_ROW, '[', ATTR_CYAN);
        jis = unicode_to_jis(0x30A2); /* ア */
        if (jis) { tvram_putkanji_at(x, IME_PREEDIT_ROW, jis, ATTR_CYAN); x += 2; }
        tvram_putchar_at(x++, IME_PREEDIT_ROW, ']', ATTR_CYAN);
    }

    if (g_ime.converting) {
        /* 変換候補表示 */
        jis = unicode_to_jis(0x25BC); /* ▼ */
        if (jis) { tvram_putkanji_at(x, IME_PREEDIT_ROW, jis, ATTR_YELLOW); x += 2; }

        if (g_ime.candidate_idx < g_ime.result_count) {
            p = (const u8 *)g_ime.results[g_ime.candidate_idx].kanji;
            while (*p && x < TVRAM_COLS - 1) {
                dec = utf8_decode(p);
                ank = unicode_to_ank(dec.codepoint);
                if (ank) {
                    tvram_putchar_at(x, IME_PREEDIT_ROW, (char)ank, ATTR_WHITE);
                    x += 1;
                } else {
                    jis = unicode_to_jis(dec.codepoint);
                    if (jis) {
                        tvram_putkanji_at(x, IME_PREEDIT_ROW, jis, ATTR_WHITE);
                        x += 2;
                    }
                }
                p += dec.bytes_used;
            }
        }

        /* 候補番号表示 */
        if (g_ime.result_count > 1 && x < TVRAM_COLS - 6) {
            char nbuf[8];
            int ni;
            tvram_putchar_at(x++, IME_PREEDIT_ROW, '(', ATTR_CYAN);
            nbuf[0] = '0' + ((g_ime.candidate_idx + 1) / 10);
            nbuf[1] = '0' + ((g_ime.candidate_idx + 1) % 10);
            nbuf[2] = '/';
            nbuf[3] = '0' + (g_ime.result_count / 10);
            nbuf[4] = '0' + (g_ime.result_count % 10);
            nbuf[5] = ')';
            nbuf[6] = '\0';
            for (ni = 0; nbuf[ni] && x < TVRAM_COLS; ni++) {
                tvram_putchar_at(x++, IME_PREEDIT_ROW, nbuf[ni], ATTR_CYAN);
            }
        }
    } else if (g_ime.kana_len > 0 || g_ime.rk.preedit[0] != '\0') {
        /* かなバッファ + 未確定ローマ字 */
        p = (const u8 *)g_ime.kana_buf;
        while (*p && x < TVRAM_COLS - 1) {
            dec = utf8_decode(p);
            ank = unicode_to_ank(dec.codepoint);
            if (ank) {
                tvram_putchar_at(x, IME_PREEDIT_ROW, (char)ank, ATTR_GREEN);
                x += 1;
            } else {
                jis = unicode_to_jis(dec.codepoint);
                if (jis) {
                    tvram_putkanji_at(x, IME_PREEDIT_ROW, jis, ATTR_GREEN);
                    x += 2;
                }
            }
            p += dec.bytes_used;
        }
        /* 未確定ローマ字 */
        p = (const u8 *)g_ime.rk.preedit;
        while (*p && x < TVRAM_COLS) {
            tvram_putchar_at(x++, IME_PREEDIT_ROW, (char)*p, ATTR_YELLOW);
            p++;
        }
    }
}

/* ======================================================================== */
/*  かなバッファへのローマ字出力追記 (共通処理)                              */
/* ======================================================================== */

static void append_rk_output(void)
{
    int olen = (int)kstrlen(g_ime.rk.output);
    if (g_ime.kana_len + olen < 127) {
        kmemcpy(g_ime.kana_buf + g_ime.kana_len, g_ime.rk.output, olen);
        g_ime.kana_len += olen;
        g_ime.kana_buf[g_ime.kana_len] = '\0';
    }
}

/* ======================================================================== */
/*  確定処理                                                                 */
/* ======================================================================== */

/* かなバッファの内容を確定出力バッファにコミット (無変換確定) */
static void commit_kana_direct(void)
{
    if (ime_rk_flush_n(&g_ime.rk)) {
        append_rk_output();
    }

    if (g_ime.kana_len > 0) {
        if (g_ime.mode == IME_MODE_KATAKANA) {
            ime_hira_to_kata(g_ime.kana_buf);
        }
        kmemcpy(g_ime.commit_buf, g_ime.kana_buf, g_ime.kana_len);
        g_ime.commit_len = g_ime.kana_len;
        g_ime.commit_pos = 0;
        g_ime.commit_buf[g_ime.commit_len] = '\0';
        g_ime.kana_buf[0] = '\0';
        g_ime.kana_len = 0;
    }
}

/* 変換候補を確定する (残りかなはバッファに保持) */
static void commit_candidate(void)
{
    if (g_ime.candidate_idx < g_ime.result_count) {
        int len = (int)kstrlen(g_ime.results[g_ime.candidate_idx].kanji);
        if (len > 255) len = 255;
        kmemcpy(g_ime.commit_buf, g_ime.results[g_ime.candidate_idx].kanji, len);
        g_ime.commit_len = len;
        g_ime.commit_pos = 0;
        g_ime.commit_buf[g_ime.commit_len] = '\0';
    }

    /* 最長一致: 変換対象部分だけ消費し、残りをバッファ先頭に移動 */
    if (g_ime.convert_len > 0 && g_ime.convert_len < g_ime.kana_len) {
        int remaining = g_ime.kana_len - g_ime.convert_len;
        int i;
        for (i = 0; i < remaining; i++) {
            g_ime.kana_buf[i] = g_ime.kana_buf[g_ime.convert_len + i];
        }
        g_ime.kana_len = remaining;
        g_ime.kana_buf[remaining] = '\0';
    } else {
        g_ime.kana_buf[0] = '\0';
        g_ime.kana_len = 0;
    }

    g_ime.converting = 0;
    g_ime.candidate_idx = 0;
    g_ime.result_count = 0;
    g_ime.convert_len = 0;
}

/* ======================================================================== */
/*  IME コアロジック: キー処理                                               */
/* ======================================================================== */

/*
 * 戻り値: 1=commit_bufに確定データあり, 0=未確定, -1=キーを消費せず透過
 */
static int ime_process_key(int keydata)
{
    u8 scancode = (u8)((keydata >> 8) & 0x7F);
    u8 ascii = (u8)(keydata & 0xFF);
    int count;

    /* === 変換候補表示中の操作 === */
    if (g_ime.converting) {
        if (scancode == KEY_SPACE) {
            g_ime.candidate_idx++;
            if (g_ime.candidate_idx >= g_ime.result_count) {
                g_ime.candidate_idx = 0;
            }
            preedit_draw();
            return 0;
        }
        if (scancode == KEY_RETURN) {
            commit_candidate();
            if (g_ime.kana_len > 0) {
                preedit_draw();  /* 残りかなを表示 */
            } else {
                preedit_clear();
            }
            return 1;
        }
        if (ascii == 0x1B || scancode == KEY_BS) {
            g_ime.converting = 0;
            g_ime.candidate_idx = 0;
            g_ime.result_count = 0;
            g_ime.convert_len = 0;
            preedit_draw();
            return 0;
        }
        /* その他のキーは候補確定後にフォールスルー */
        commit_candidate();
        if (g_ime.kana_len > 0) {
            preedit_draw();
        } else {
            preedit_clear();
        }
        return -1;
    }

    /* === 通常入力中 === */

    /* スペースキー: 変換開始 (最長一致法) */
    if (scancode == KEY_SPACE && g_ime.kana_len > 0) {
        int try_len;
        char saved_char;

        if (ime_rk_flush_n(&g_ime.rk)) {
            append_rk_output();
        }

        /* 完全一致 → 最長前方一致の順で検索 */
        try_len = g_ime.kana_len;
        while (try_len > 0) {
            saved_char = g_ime.kana_buf[try_len];
            g_ime.kana_buf[try_len] = '\0';

            count = ime_dict_search(&g_ime.dict, g_ime.kana_buf,
                                    g_ime.results, IME_MAX_RESULTS);

            g_ime.kana_buf[try_len] = saved_char;

            if (count > 0) {
                g_ime.result_count = count;
                g_ime.candidate_idx = 0;
                g_ime.converting = 1;
                g_ime.convert_len = try_len;
                preedit_draw();
                return 0;
            }

            /* 1文字分戻る (UTF-8後続バイトをスキップ) */
            try_len--;
            while (try_len > 0 && ((u8)g_ime.kana_buf[try_len] & 0xC0) == 0x80) {
                try_len--;
            }
        }

        /* 全く一致なし: かなのまま確定 */
        commit_kana_direct();
        preedit_clear();
        return 1;
    }

    /* Enter: かなバッファを直接確定 */
    if (scancode == KEY_RETURN) {
        if (g_ime.kana_len > 0 || g_ime.rk.preedit[0] != '\0') {
            commit_kana_direct();
            preedit_clear();
            return 1;
        }
        return -1;
    }

    /* ESC: かなバッファをクリア */
    if (ascii == 0x1B) {
        if (g_ime.kana_len > 0 || g_ime.rk.preedit[0] != '\0') {
            g_ime.kana_buf[0] = '\0';
            g_ime.kana_len = 0;
            ime_rk_init(&g_ime.rk);
            preedit_draw();
            return 0;
        }
        return -1;
    }

    /* Backspace: かなバッファ末尾削除 */
    if (scancode == KEY_BS) {
        if (g_ime.rk.preedit[0] != '\0') {
            int plen = (int)kstrlen(g_ime.rk.preedit);
            if (plen > 0) {
                g_ime.rk.preedit[plen - 1] = '\0';
                if (plen - 1 == 0) g_ime.rk.n_wait = 0;
            }
            preedit_draw();
            return 0;
        }
        if (g_ime.kana_len > 0) {
            utf8_delete_last((u8 *)g_ime.kana_buf);
            g_ime.kana_len = (int)kstrlen(g_ime.kana_buf);
            preedit_draw();
            return 0;
        }
        return -1;
    }

    /* 英字/記号: ローマ字変換 */
    if (ascii >= 0x20 && ascii <= 0x7E && scancode != KEY_SPACE) {
        if (ime_rk_append(&g_ime.rk, (char)ascii)) {
            append_rk_output();
        }
        preedit_draw();
        return 0;
    }

    /* その他のキー: 透過 */
    return -1;
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

void ime_init(void)
{
    kmemset(&g_ime, 0, sizeof(IME_State));
    g_ime.mode = IME_MODE_OFF;
    g_ime.dict_loaded = 0;
    ime_rk_init(&g_ime.rk);
}

void ime_toggle(void)
{
    if (g_ime.mode == IME_MODE_OFF) {
        if (!g_ime.dict_loaded) {
            if (ime_dict_open(&g_ime.dict, IME_DICT_PATH) == 0) {
                g_ime.dict_loaded = 1;
            } else {
                kprintf(ATTR_RED, "IME: Dict load failed, FEP disabled\r\n");
                return;
            }
        }
        g_ime.mode = IME_MODE_HIRAGANA;
        g_ime.kana_buf[0] = '\0';
        g_ime.kana_len = 0;
        g_ime.converting = 0;
        ime_rk_init(&g_ime.rk);
        preedit_draw();
    } else {
        /* かなバッファに残りがあれば確定 */
        if (g_ime.kana_len > 0 || g_ime.rk.preedit[0] != '\0') {
            commit_kana_direct();
        }
        g_ime.mode = IME_MODE_OFF;
        g_ime.converting = 0;
        preedit_clear();
    }
}

int ime_is_active(void)
{
    return g_ime.mode != IME_MODE_OFF;
}

void ime_set_mode(int mode)
{
    g_ime.mode = mode;
    if (mode != IME_MODE_OFF) {
        preedit_draw();
    } else {
        preedit_clear();
    }
}

int ime_get_mode(void)
{
    return g_ime.mode;
}

int ime_getchar(void)
{
    int keydata;
    int result;

    /* バッファに確定済みがあれば返す */
    if (g_ime.commit_pos < g_ime.commit_len) {
        return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
    }

    for (;;) {
        keydata = kbd_getkey();

        /* Shift+Space: ON/OFF問わず常に検出 */
        if (((keydata >> 8) & 0x7F) == KEY_SPACE &&
            (kbd_shift_state & SHIFT_SHIFT)) {
            ime_toggle();
            if (g_ime.commit_pos < g_ime.commit_len) {
                return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
            }
            continue;
        }

        /* FEP OFF: ASCII値をそのまま返す */
        if (g_ime.mode == IME_MODE_OFF) {
            u8 a = (u8)(keydata & 0xFF);
            if (a != 0) return (int)a;
            continue;
        }

        /* FEP ON: IME処理 */
        result = ime_process_key(keydata);
        if (result == 1 && g_ime.commit_pos < g_ime.commit_len) {
            return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
        }
        if (result == -1) {
            u8 a = (u8)(keydata & 0xFF);
            if (a != 0) return (int)a;
        }
    }
}

int ime_trygetchar(void)
{
    int keydata;
    int result;

    if (g_ime.commit_pos < g_ime.commit_len) {
        return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
    }

    keydata = kbd_trygetkey();
    if (keydata < 0) return -1;

    if (((keydata >> 8) & 0x7F) == KEY_SPACE &&
        (kbd_shift_state & SHIFT_SHIFT)) {
        ime_toggle();
        if (g_ime.commit_pos < g_ime.commit_len) {
            return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
        }
        return -1;
    }

    if (g_ime.mode == IME_MODE_OFF) {
        u8 a = (u8)(keydata & 0xFF);
        if (a != 0) return (int)a;
        return -1;
    }

    result = ime_process_key(keydata);
    if (result == 1 && g_ime.commit_pos < g_ime.commit_len) {
        return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
    }
    if (result == -1) {
        u8 a = (u8)(keydata & 0xFF);
        if (a != 0) return (int)a;
    }
    return -1;
}

int ime_getkey(void)
{
    int keydata;
    int result;

    /* バッファに確定済みがあれば 1バイトずつ返す (scancode=0) */
    if (g_ime.commit_pos < g_ime.commit_len) {
        return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
    }

    for (;;) {
        keydata = kbd_getkey();

        /* Shift+Space: ON/OFF問わず常に検出 */
        if (((keydata >> 8) & 0x7F) == KEY_SPACE &&
            (kbd_shift_state & SHIFT_SHIFT)) {
            ime_toggle();
            if (g_ime.commit_pos < g_ime.commit_len) {
                return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
            }
            continue;
        }

        /* FEP OFF: キーをそのまま返す */
        if (g_ime.mode == IME_MODE_OFF) {
            return keydata;
        }

        /* FEP ON: IME処理 */
        result = ime_process_key(keydata);
        if (result == 1 && g_ime.commit_pos < g_ime.commit_len) {
            return (int)(u8)g_ime.commit_buf[g_ime.commit_pos++];
        }
        if (result == -1) {
            return keydata;
        }
        /* result == 0: 未確定、次のキーを待つ */
    }
}
