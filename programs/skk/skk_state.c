#include "skk.h"

/* SKK ステートマシン (C89) */

/* 内部状態をクリアし、直接入力モードに戻す */
static void _clear_state(SKK_STATE *state) {
    state->mode = SKK_MODE_DIRECT;
    skk_rk_init(&state->rk);
    state->midasi_buf[0] = '\0';
    state->okuri_char = '\0';
    state->okuri_kana[0] = '\0';
    state->current_dict_line = NULL;
    state->kanji_buf[0] = '\0';
}

void skk_state_init(SKK_STATE *state) {
    state->ime_on = 0;
    state->katakana_mode = 0;
    state->candidate_index = 0;
    _clear_state(state);
}

/* 未確定文字列を確定出力する共通ヘルパー */
static void _commit_preedit(SKK_STATE *state, char *out_str) {
    strcpy(out_str, state->midasi_buf);
    if (state->okuri_kana[0] != '\0') {
        strcat(out_str, state->okuri_kana);
    }
    if (state->rk.preedit[0] != '\0') {
        strcat(out_str, state->rk.preedit);
    }
    _clear_state(state);
}

/* 辞書変換を開始する */
static void _enter_conversion(SKK_STATE *state) {
    state->current_dict_line = skk_dict_search(state->midasi_buf, state->okuri_char);
    if (state->current_dict_line) {
        state->mode = SKK_MODE_CONVERSION;
        state->candidate_index = 0;
        if (!skk_dict_get_candidate(state->current_dict_line, 0, state->kanji_buf)) {
            strcpy(state->kanji_buf, state->midasi_buf);
        }
    } else {
        /* 辞書に見つからない場合: 見出しをそのまま出力 */
        strcpy(state->kanji_buf, state->midasi_buf);
        if (state->okuri_char) {
            strcat(state->kanji_buf, state->okuri_kana);
        }
        state->mode = SKK_MODE_DIRECT;
    }
}

/* 送り仮名変換を試行し、結果を処理する共通ヘルパー */
static void _try_okuri_conversion(SKK_STATE *state, char *out_str) {
    _enter_conversion(state);
    if (state->mode == SKK_MODE_DIRECT) {
        strcpy(out_str, state->kanji_buf);
        _clear_state(state);
    }
}

/* カタカナモード時にローマ字変換結果をカタカナ化する */
static void _apply_katakana(SKK_STATE *state) {
    if (state->katakana_mode) {
        skk_hira_to_kata(state->rk.output);
    }
}

/* --- モード別ハンドラ --- */

/* 直接入力モード */
static int _handle_direct(u8 key, SKK_STATE *state, char *out_str) {
    if (key == 'q') {
        state->katakana_mode = !state->katakana_mode;
        return 1;
    }
    if (key >= 'A' && key <= 'Z') {
        state->mode = SKK_MODE_PREEDIT;
        skk_rk_init(&state->rk);
        if (skk_rk_append(&state->rk, key + ('a' - 'A'))) {
            _apply_katakana(state);
            strcat(state->midasi_buf, state->rk.output);
        }
        return 1;
    }
    if (key >= 0x20 && key <= 0x7E) {
        if (skk_rk_append(&state->rk, key)) {
            _apply_katakana(state);
            if (state->rk.output[0] != '\0') {
                strcpy(out_str, state->rk.output);
            }
        }
        return 1;
    }
    return 0;
}

/* 見出し語入力モード ▽ */
static int _handle_preedit(u8 key, u32 modifier, SKK_STATE *state, char *out_str) {
    if (key == 0x08 || key == 0x7F) { /* BS */
        if (state->rk.preedit[0] != '\0') {
            skk_rk_delete(&state->rk);
        } else if (state->midasi_buf[0] != '\0') {
            /* 見出し語末尾を削除 (UTF-8対応) */
            int len = strlen(state->midasi_buf);
            if (len > 0) state->midasi_buf[len - 1] = '\0';
        } else {
            _clear_state(state);
        }
        return 1;
    }
    if (key == 'q') { /* カタカナへ変換して確定 */
        _commit_preedit(state, out_str);
        skk_hira_to_kata(out_str);
        return 1;
    }
    if (key == ' ') {
        _enter_conversion(state);
        if (state->mode == SKK_MODE_DIRECT) {
            strcpy(out_str, state->kanji_buf);
            _clear_state(state);
        }
        return 1;
    }
    if (key == 0x07 || key == 0x1B) { /* Ctrl+G or ESC */
        _clear_state(state);
        return 1;
    }
    if (key >= 'A' && key <= 'Z') { /* 送り開始 (大文字) */
        state->mode = SKK_MODE_OKURI;
        state->okuri_char = key + ('a' - 'A');
        state->okuri_kana[0] = '\0';
        skk_rk_init(&state->rk);
        if (skk_rk_append(&state->rk, state->okuri_char)) {
            strcpy(state->okuri_kana, state->rk.output);
            _try_okuri_conversion(state, out_str);
        }
        return 1;
    }
    if (key >= 0x20 && key <= 0x7E) {
        if (skk_rk_append(&state->rk, key)) {
            strcat(state->midasi_buf, state->rk.output);
        }
        return 1;
    }
    if (key == 0x0D || key == 0x0A) { /* ENTERで確定 */
        _commit_preedit(state, out_str);
        return 1;
    }
    /* その他の制御文字等: 確定してパススルー */
    _commit_preedit(state, out_str);
    return 0;
}

/* 送り仮名入力モード * */
static int _handle_okuri(u8 key, SKK_STATE *state, char *out_str) {
    if (key >= 0x20 && key <= 0x7E) {
        if (skk_rk_append(&state->rk, key)) {
            _apply_katakana(state);
            strcpy(state->okuri_kana, state->rk.output);
            _try_okuri_conversion(state, out_str);
        }
        return 1;
    }
    if (key == 0x08 || key == 0x7F) {
        if (!skk_rk_delete(&state->rk)) {
            state->mode = SKK_MODE_PREEDIT;
            state->okuri_char = '\0';
            state->okuri_kana[0] = '\0';
        }
        return 1;
    }
    if (key == 0x07 || key == 0x1B) { /* Ctrl+G or ESC */
        state->mode = SKK_MODE_PREEDIT;
        state->okuri_char = '\0';
        skk_rk_init(&state->rk);
        return 1;
    }
    if (key == 'q') { /* カタカナへ変換して確定 */
        _commit_preedit(state, out_str);
        skk_hira_to_kata(out_str);
        return 1;
    }
    if (key == 0x0D || key == 0x0A) { /* ENTERで確定 */
        _commit_preedit(state, out_str);
        return 1;
    }
    /* その他の制御文字等 */
    _commit_preedit(state, out_str);
    return 0;
}

/* 辞書変換モード ▼ — 変換候補の確定 */
static void _commit_conversion(SKK_STATE *state, char *out_str) {
    strcpy(out_str, state->kanji_buf);
    if (state->okuri_char) {
        strcat(out_str, state->okuri_kana);
    }
    _clear_state(state);
}

static int _handle_conversion(u8 key, u32 modifier, SKK_STATE *state, char *out_str) {
    if (key == ' ') {
        state->candidate_index++;
        if (!skk_dict_get_candidate(state->current_dict_line, state->candidate_index, state->kanji_buf)) {
            state->candidate_index = 0;
            skk_dict_get_candidate(state->current_dict_line, 0, state->kanji_buf);
        }
        return 1;
    }
    if (key == 0x0D || key == 0x0A) { /* ENTERで確定 */
        _commit_conversion(state, out_str);
        return 1;
    }
    if (key == 0x07 || key == 0x1B) { /* ESC/Ctrl+G でキャンセル */
        state->mode = SKK_MODE_PREEDIT;
        state->okuri_char = '\0';
        state->okuri_kana[0] = '\0';
        state->current_dict_line = NULL;
        skk_rk_init(&state->rk);
        return 1;
    }
    /* その他の通常文字: 確定して続けて打つ */
    if (key >= 0x20 && key <= 0x7E) {
        _commit_conversion(state, out_str);
        skk_process_key(key, modifier, state, out_str + strlen(out_str));
        return 1;
    }
    return 0;
}

/* --- メインディスパッチャ --- */

int skk_process_key(u8 key, u32 modifier, SKK_STATE *state, char *out_str) {
    out_str[0] = '\0';

    if (key == 0x00) { /* Ctrl+Space トグル */
        state->ime_on = !state->ime_on;
        if (!state->ime_on) _clear_state(state);
        return 1;
    }

    if (!state->ime_on) {
        if (key >= 0x20 && key < 0x7F) {
            out_str[0] = key;
            out_str[1] = '\0';
            return 1;
        }
        return 0;
    }

    switch (state->mode) {
    case SKK_MODE_DIRECT:     return _handle_direct(key, state, out_str);
    case SKK_MODE_PREEDIT:    return _handle_preedit(key, modifier, state, out_str);
    case SKK_MODE_OKURI:      return _handle_okuri(key, state, out_str);
    case SKK_MODE_CONVERSION: return _handle_conversion(key, modifier, state, out_str);
    }

    return 0;
}

void skk_get_ui_string(SKK_STATE *state, char *out_ui_str) {
    out_ui_str[0] = '\0';
    if (!state->ime_on) return;
    
    switch (state->mode) {
    case SKK_MODE_PREEDIT:
        strcpy(out_ui_str, state->katakana_mode ? "▽カナ:" : "▽");
        strcat(out_ui_str, state->midasi_buf);
        strcat(out_ui_str, state->rk.preedit);
        break;
    case SKK_MODE_OKURI:
        strcpy(out_ui_str, "▽");
        strcat(out_ui_str, state->midasi_buf);
        strcat(out_ui_str, "*");
        strcat(out_ui_str, state->rk.preedit);
        break;
    case SKK_MODE_CONVERSION:
        strcpy(out_ui_str, "▼");
        strcat(out_ui_str, state->kanji_buf);
        strcat(out_ui_str, state->okuri_kana);
        break;
    case SKK_MODE_DIRECT:
        if (state->katakana_mode) strcpy(out_ui_str, "[カナ]");
        strcat(out_ui_str, state->rk.preedit);
        break;
    default:
        break;
    }
}
