/* ======================================================================== */
/*  IME.H — OS32 カーネルFEP (日本語入力システム)                            */
/*                                                                          */
/*  カーネル常駐型の日本語入力フロントエンドプロセッサ。                      */
/*  辞書検索エンジン・ローマ字かな変換・プリエディット表示を統合する。        */
/* ======================================================================== */

#ifndef __IME_H
#define __IME_H

#include "types.h"

/* ======================================================================== */
/*  FEP モード                                                               */
/* ======================================================================== */

#define IME_MODE_OFF       0    /* FEP無効 (直接入力) */
#define IME_MODE_HIRAGANA  1    /* ひらがな入力 */
#define IME_MODE_KATAKANA  2    /* カタカナ入力 */

/* ======================================================================== */
/*  辞書バイナリ構造体 (.dic フォーマット)                                    */
/* ======================================================================== */

#define IME_DICT_MAGIC      0x54434944UL  /* "DICT" LE */
#define IME_DICT_VERSION    2

/* 検索結果の最大数 */
#define IME_MAX_RESULTS     32

/* データブロックバッファサイズ (最大ブロック 48.6KB + 余裕) */
#define IME_BLOCK_BUF_SIZE  51200

/* ブロックキャッシュサイズ (16KB FIFO) */
#define IME_CACHE_SIZE      16384
#define IME_CACHE_HDR_SIZE  12

/* ファイルヘッダ (16B) */
typedef struct {
    u32 magic;
    u32 version;
    u32 total_words;
    u32 l1_count;
} IME_DictHeader;

/* Level-1 インデックスエントリ (12B) */
typedef struct {
    u8  key_char[4];    /* 先頭1文字 (UTF-8, ゼロパディング) */
    u32 l2_offset;      /* Level-2 開始位置 (ファイル絶対オフセット) */
    u32 l2_count;       /* Level-2 エントリ数 */
} IME_L1Entry;

/* Level-2 インデックスエントリ (12B) */
typedef struct {
    u8  key_chars[8];   /* 先頭2文字 (UTF-8, ゼロパディング) */
    u32 data_offset;    /* データブロック開始位置 */
} IME_L2Entry;

/* キャッシュエントリヘッダ (12B) */
typedef struct {
    u8  l2_key[8];      /* Level-2 キー (照合用) */
    u32 block_size;     /* データブロックサイズ */
} IME_CacheHdr;

/* WordMeta32 ビットフィールド操作マクロ */
#define WMETA_YOMI_LEN(m)   ((m) & 0x1F)
#define WMETA_KANJI_LEN(m)  (((m) >> 5) & 0x1F)
#define WMETA_POS_ID(m)     (((m) >> 10) & 0x7FF)
#define WMETA_COST(m)       (((m) >> 21) & 0x7FF)

/* 検索結果エントリ */
typedef struct {
    char yomi[32];      /* 読み (UTF-8, ヌル終端) */
    char kanji[32];     /* 漢字/表層形 (UTF-8, ヌル終端) */
    u16  pos_id;        /* 品詞ID */
    u16  cost;          /* コスト (小さいほど優先) */
} IME_Result;

/* ======================================================================== */
/*  辞書コンテキスト                                                         */
/* ======================================================================== */

typedef struct {
    IME_DictHeader header;
    IME_L1Entry   *l1_index;        /* メモリ常駐 Level-1 */
    IME_L2Entry   *l2_index;        /* メモリ常駐 Level-2 */
    u32            l2_total;         /* Level-2 総エントリ数 */
    char           dict_path[128];
    u8            *block_buf;        /* データブロック読み込みバッファ */
    u32            block_buf_size;
    u8            *cache_buf;        /* 16KB FIFOブロックキャッシュ */
    u32            cache_used;       /* キャッシュ使用済みバイト数 */
} IME_Dict;

/* ======================================================================== */
/*  IME 状態構造体                                                           */
/* ======================================================================== */

/* ローマ字かな変換バッファ */
typedef struct {
    char preedit[8];    /* 未確定ローマ字バッファ (例: "ky") */
    char output[32];    /* 確定したかな出力 */
    int  n_wait;        /* 'n' 待ち状態フラグ */
} IME_RomKana;

/* IME全体状態 */
typedef struct {
    int         mode;           /* IME_MODE_xxx */
    IME_RomKana rk;             /* ローマ字かな変換 */
    char        kana_buf[128];  /* 入力中のかな列 (変換対象) */
    int         kana_len;       /* kana_bufのバイト長 */
    /* 変換候補 */
    IME_Result  results[IME_MAX_RESULTS];
    int         result_count;   /* ヒット数 */
    int         candidate_idx;  /* 選択中の候補インデックス */
    int         converting;     /* 1=変換候補表示中 */
    int         convert_len;    /* 変換対象の読みバイト数(最長一致用) */
    /* 確定済み出力バッファ */
    char        commit_buf[256];
    int         commit_pos;     /* 次に返すバイト位置 */
    int         commit_len;     /* 確定文字列のバイト長 */
    /* 辞書 */
    IME_Dict    dict;
    int         dict_loaded;    /* 辞書ロード済みフラグ */
} IME_State;

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

/* 初期化 (ブート時に1回) */
void ime_init(void);

/* FEP ON/OFF トグル (初回呼び出し時に辞書ロード) */
void ime_toggle(void);

/* FEP 状態取得 */
int  ime_is_active(void);

/* FEP モード設定/取得 */
void ime_set_mode(int mode);
int  ime_get_mode(void);

/* FEP経由のブロッキング入力 (確定文字を1バイトずつ返す) */
int  ime_getchar(void);

/* FEP経由のノンブロッキング入力 */
int  ime_trygetchar(void);

/*
 * FEP経由のブロッキングキー入力 (kbd_getkey互換)
 * 戻り値: (scancode << 8) | ascii
 * IME確定文字は scancode=0x00 で1バイトずつ返す。
 * 制御キー (矢印, TAB等) はIMEを透過して生のkeydataを返す。
 */
int  ime_getkey(void);

/* ======================================================================== */
/*  内部関数 (ime_romkana.c, ime_dict.c から公開)                            */
/* ======================================================================== */

/* ローマ字かな変換 (ime_romkana.c) */
void ime_rk_init(IME_RomKana *rk);
int  ime_rk_append(IME_RomKana *rk, char c);
int  ime_rk_flush_n(IME_RomKana *rk);
void ime_hira_to_kata(char *utf8_str);

/* 辞書操作 (ime_dict.c) */
int  ime_dict_open(IME_Dict *dict, const char *path);
int  ime_dict_search(IME_Dict *dict, const char *yomi,
                     IME_Result *results, int max_results);

#endif /* __IME_H */
