/* ======================================================================== */
/*  FEP_ENGINE.H - OS32 FEP 辞書検索エンジン                                 */
/*                                                                          */
/*  .dic バイナリ辞書のインデックス読み込み・検索を行う。                     */
/*  外部プログラム (fep_test等) から利用する。                                */
/* ======================================================================== */

#ifndef FEP_ENGINE_H
#define FEP_ENGINE_H

#include "os32api.h"

/* ======================================================================== */
/*  定数                                                                     */
/* ======================================================================== */

#define FEP_DICT_MAGIC    0x54434944UL  /* "DICT" リトルエンディアン */
#define FEP_DICT_VERSION  2

/* 検索結果の最大エントリ数 */
#define FEP_MAX_RESULTS   32

/* データブロック読み込みバッファサイズ (最大ブロック「しょ」32.8KB + 余裕) */
#define FEP_BLOCK_BUF_SIZE  34816

/* ======================================================================== */
/*  バイナリ構造体 (ディスク上のレイアウトと一致)                              */
/* ======================================================================== */

/* ファイルヘッダ (16 bytes) */
typedef struct {
    u32 magic;          /* 0x00: FEP_DICT_MAGIC */
    u32 version;        /* 0x04: 辞書バージョン */
    u32 total_words;    /* 0x08: 総単語数 */
    u32 l1_count;       /* 0x0C: Level-1 エントリ数 */
} FEP_DictHeader;

/* Level-1 インデックスエントリ (12 bytes) */
typedef struct {
    u8  key_char[4];    /* 読みの先頭1文字 (UTF-8, パディング 0x00) */
    u32 l2_offset;      /* Level-2 開始位置 (ファイル絶対オフセット) */
    u32 l2_count;       /* このL1に属するLevel-2エントリ数 */
} FEP_L1Entry;

/* Level-2 インデックスエントリ (12 bytes) */
typedef struct {
    u8  key_chars[8];   /* 読みの先頭2文字 (UTF-8, パディング 0x00) */
    u32 data_offset;    /* データブロック開始位置 (ファイル絶対オフセット) */
} FEP_L2Entry;

/* WordMeta32 ビットフィールド操作マクロ */
#define WMETA_YOMI_LEN(m)   ((m) & 0x1F)
#define WMETA_KANJI_LEN(m)  (((m) >> 5) & 0x1F)
#define WMETA_POS_ID(m)     (((m) >> 10) & 0x7FF)
#define WMETA_COST(m)       (((m) >> 21) & 0x7FF)

/* ======================================================================== */
/*  検索結果構造体                                                           */
/* ======================================================================== */

typedef struct {
    char yomi[32];      /* 読み (UTF-8, ヌル終端) */
    char kanji[32];     /* 漢字/表層形 (UTF-8, ヌル終端) */
    u16  pos_id;        /* 品詞ID */
    u16  cost;          /* コスト (小さいほど優先) */
} FEP_Result;

/* ======================================================================== */
/*  辞書コンテキスト                                                         */
/* ======================================================================== */

typedef struct {
    FEP_DictHeader header;
    FEP_L1Entry   *l1_index;        /* オンメモリ常駐 Level-1 */
    FEP_L2Entry   *l2_index;        /* オンメモリ常駐 Level-2 */
    u32            l2_total;         /* Level-2 総エントリ数 */
    char           dict_path[OS32_MAX_PATH];
    u8            *block_buf;        /* データブロック読み込みバッファ */
    u32            block_buf_size;   /* バッファサイズ */
    KernelAPI     *api;
} FEP_Dict;

/* ======================================================================== */
/*  API                                                                      */
/* ======================================================================== */

/*
 * fep_dict_open - 辞書ファイルを開きインデックスをメモリにロードする
 *   dict: 初期化される辞書コンテキスト
 *   path: .dic ファイルのパス
 *   api:  KernelAPI ポインタ
 *   戻り値: 0=成功, 負=エラー
 */
int  fep_dict_open(FEP_Dict *dict, const char *path, KernelAPI *api);

/*
 * fep_dict_close - 辞書のメモリを解放する
 */
void fep_dict_close(FEP_Dict *dict);

/*
 * fep_dict_search - 読み(ひらがな, UTF-8)で辞書を検索する (前方一致)
 *   dict:        初期化済みの辞書コンテキスト
 *   yomi:        検索する読み (UTF-8, ヌル終端)
 *   results:     結果を格納する配列
 *   max_results: 結果配列の最大エントリ数
 *   戻り値: ヒットした結果数 (0以上), 負=エラー
 */
int  fep_dict_search(FEP_Dict *dict, const char *yomi,
                     FEP_Result *results, int max_results);

#endif /* FEP_ENGINE_H */
