#ifndef SKK_H
#define SKK_H

#include "os32api.h"
#include <string.h>

/* ======================================================================== */
/*  ローマ字かな変換エンジン状態 (skk_rom_kana)                               */
/* ======================================================================== */

typedef struct {
    char preedit[8];       /* 未確定のローマ字バッファ (例: "tt", "ky") */
    char output[32];       /* 確定したかな出力 (複数文字になる場合がある) */
    int  n_wait;           /* 'n' (ん) が入力された直後かどうかのフラグ */
} skk_rom_kana_t;

/* ======================================================================== */
/*  SKK 全体ステートマシン状態                                              */
/* ======================================================================== */

typedef enum {
    SKK_MODE_DIRECT = 0,   /* 直接入力状態 (例: わたし) */
    SKK_MODE_PREEDIT,      /* 見出し語入力状態 ▽ (例: ▽わたし) */
    SKK_MODE_OKURI,        /* 送り仮名入力状態 * (例: ▽か*k) */
    SKK_MODE_CONVERSION    /* 辞書変換状態 ▼ (例: ▼私) */
} SKK_MODE;

typedef struct {
    int ime_on;                 /* 0: アルファベット直接入力, 1: SKK稼働 */
    int katakana_mode;          /* 0: ひらがな, 1: カタカナ */
    SKK_MODE mode;              /* 現在のモード */
    
    skk_rom_kana_t rk;          /* ローマ字かな変換インスタンス */
    
    char midasi_buf[128];       /* 確定済みの見出し語 (例: "か") */
    char okuri_char;            /* 送りの起点となるローマ字子音 (例: 'k') */
    char okuri_kana[16];        /* 送り仮名のひらがな (例: "く") */
    
    /* 変換に関する情報 */
    const char *current_dict_line; /* 辞書のヒットした行の先頭 */
    int candidate_index;        /* 何番目の変換候補を選択しているか (0〜) */
    char kanji_buf[128];        /* 現在選択中の漢字出力バッファ */
} SKK_STATE;

/* ======================================================================== */
/*  SKK 辞書モジュール (skk_dict.c)                                         */
/* ======================================================================== */
int skk_init(const char *dict_path, KernelAPI *api);
const char *skk_dict_search(const char *midasi, char okuri);
int skk_dict_get_candidate(const char *line, int index, char *out_buf);
void skk_free(void);

/* ======================================================================== */
/*  SKK ローマ字エンジン (skk_rom_kana.c)                                    */
/* ======================================================================== */
void skk_rk_init(skk_rom_kana_t *rk);
int skk_rk_append(skk_rom_kana_t *rk, char c);
int skk_rk_delete(skk_rom_kana_t *rk);
void skk_hira_to_kata(char *utf8_str);

/* ======================================================================== */
/*  SKK ステートマシン (skk_state.c)                                        */
/* ======================================================================== */
void skk_state_init(SKK_STATE *state);
int skk_process_key(u8 key, u32 modifier, SKK_STATE *state, char *out_str);

/* ステートマシン専用UI文字列ジェネレータ（skk_test.c等で表示用） */
void skk_get_ui_string(SKK_STATE *state, char *out_ui_str);

#endif
