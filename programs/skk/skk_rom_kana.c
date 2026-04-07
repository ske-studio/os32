#include "skk.h"

/* 
 * SKK専用ローマ字かな変換エンジン
 * C89の制約内で、ステートを持った高度な変換を実現する。
 */

typedef struct {
    const char *roma;
    const char *kana;
} RomaKana;

/* SKKローマ字テーブル（よく使うもの） */
static const RomaKana romaji_table[] = {
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"sa", "さ"}, {"shi", "し"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"chi", "ち"}, {"ti", "ち"}, {"tsu", "つ"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"fu", "ふ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"}, {"nn", "ん"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"za", "ざ"}, {"ji", "じ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"da", "だ"}, {"di", "ぢ"}, {"du", "づ"}, {"de", "で"}, {"do", "ど"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"xtu", "っ"}, {"xtsu", "っ"},
    {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"}, {"xo", "ぉ"},
    {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"", ""}
};

/* ローマ字テーブルに対する前方一致チェック */
static int _has_prefix_match(const char *temp, int temp_len) {
    int i, j, match;
    for (i = 0; romaji_table[i].roma[0] != '\0'; i++) {
        if (strlen(romaji_table[i].roma) > temp_len) {
            match = 1;
            for (j = 0; j < temp_len; j++) {
                if (romaji_table[i].roma[j] != temp[j]) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    return 0;
}

void skk_rk_init(skk_rom_kana_t *rk) {
    rk->preedit[0] = '\0';
    rk->output[0] = '\0';
    rk->n_wait = 0;
}

int skk_rk_append(skk_rom_kana_t *rk, char c) {
    int i, len;
    char temp[8];
    
    rk->output[0] = '\0';
    
    /* 英文字以外が来たらそのまま出す（SKKの仕様上、直接記号が入ることもある） */
    if (c < 'a' || c > 'z') {
        if (c == '-' || c == ',' || c == '.' || c == '[' || c == ']') {
            /* 記号変換 */
            if (c == '-') { strcpy(rk->output, "ー"); }
            else if (c == ',') { strcpy(rk->output, "、"); }
            else if (c == '.') { strcpy(rk->output, "。"); }
            else if (c == '[') { strcpy(rk->output, "「"); }
            else if (c == ']') { strcpy(rk->output, "」"); }
            rk->preedit[0] = '\0';
            rk->n_wait = 0;
            return 1;
        } else {
            /* その他の記号はそのまま */
            rk->output[0] = c;
            rk->output[1] = '\0';
            rk->preedit[0] = '\0';
            rk->n_wait = 0;
            return 1;
        }
    }

    /* 継続 n 処理 (nn で "ん"、na で "な"、nk で "ん" + "k"等) */
    if (rk->n_wait) {
        if (c == 'n') {
            /* nn -> ん */
            strcpy(rk->output, "ん");
            rk->preedit[0] = '\0';
            rk->n_wait = 0;
            return 1;
        } else if (c == 'y' || c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o') {
            /* 母音または 'y' は n とくっついてな行になるので n_wait を解除して普通に続ける */
            rk->n_wait = 0;
            /* 継続されるので下へ落ちる */
        } else {
            /* ん が確定し、さらに新しい子音が来る */
            strcpy(rk->output, "ん");
            rk->preedit[0] = c;
            rk->preedit[1] = '\0';
            rk->n_wait = 0;
            return 1;
        }
    }

    /* 'n' 単独押下の検知 (preeditが空で n) */
    if (rk->preedit[0] == '\0' && c == 'n') {
        rk->preedit[0] = 'n';
        rk->preedit[1] = '\0';
        rk->n_wait = 1;
        return 0; /* まだ未確定 */
    }

    len = strlen(rk->preedit);
    
    /* 連続子音 (tt 等) の促音化処理 */
    if (len == 1 && rk->preedit[0] == c && c != 'n' && c != 'a' && c != 'i' && c != 'u' && c != 'e' && c != 'o') {
        strcpy(rk->output, "っ");
        /* preedit は1文字 (c) を残す */
        rk->preedit[0] = c;
        rk->preedit[1] = '\0';
        return 1;
    }

    /* 結合してテーブルと照合 */
    strcpy(temp, rk->preedit);
    temp[len] = c;
    temp[len + 1] = '\0';

    for (i = 0; romaji_table[i].roma[0] != '\0'; i++) {
        if (strcmp(romaji_table[i].roma, temp) == 0) {
            strcpy(rk->output, romaji_table[i].kana);
            rk->preedit[0] = '\0';
            rk->n_wait = 0;
            return 1;
        }
    }

    /* テーブルに完全一致しなかった場合。
       プレフィックスとして前方一致するものがあるか？ (例: "k" -> "ka" など) */
    if (_has_prefix_match(temp, strlen(temp))) {
        /* まだ続きがある */
        strcpy(rk->preedit, temp);
        return 0;
    }

    /* 前方一致もしないような不正な綴りの場合はバッファをクリアして打ち直し */
    rk->preedit[0] = c;
    rk->preedit[1] = '\0';
    rk->n_wait = 0;
    
    return 0;
}

int skk_rk_delete(skk_rom_kana_t *rk) {
    int len = strlen(rk->preedit);
    if (len > 0) {
        rk->preedit[len - 1] = '\0';
        if (len - 1 == 0) rk->n_wait = 0;
        return 1;
    }
    return 0;
}

/* UTF-8の文字列中のひらがなをカタカナに変換する */
void skk_hira_to_kata(char *utf8_str) {
    u8 *p = (u8 *)utf8_str;
    while (*p) {
        if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            u16 code = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (code >= 0x3041 && code <= 0x3096) {
                code += 0x0060;
                p[0] = 0xE0 | ((code >> 12) & 0x0F);
                p[1] = 0x80 | ((code >> 6) & 0x3F);
                p[2] = 0x80 | (code & 0x3F);
            }
            p += 3;
        } else if ((p[0] & 0xE0) == 0xC0) {
            p += 2;
        } else if ((p[0] & 0x80) == 0x00) {
            p += 1;
        } else {
            p += 1;
        }
    }
}
