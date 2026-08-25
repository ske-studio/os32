/* ======================================================================== */
/*  LIBOS32TEXT.H — OS32 テキスト管理ライブラリ 公開ヘッダ                   */
/*                                                                          */
/*  RPG/ADV向けメッセージテキスト管理エンジン。                              */
/*  SQLiteデータベースからテキストを取得し、タイプライター演出・              */
/*  ページ送り・変数埋め込みをランタイムで処理する。                          */
/*                                                                          */
/*  依存: libos32db (KAPI経由SQLite)                                        */
/*  描画 (gfx/tilemap/ecs) には依存しない。                                  */
/* ======================================================================== */

#ifndef LIBOS32TEXT_H
#define LIBOS32TEXT_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 定数                                                                */
/* ====================================================================== */

#define TEXT_MAX_SLOTS      4    /* 同時管理スロット上限 */
#define TEXT_BUF_SIZE     256    /* 1メッセージの最大バイト長 (UTF-8) */
#define TEXT_MAX_PAGES      8    /* 1メッセージの最大ページ数 */
#define TEXT_MAX_VARS      16    /* 変数埋め込み上限 */
#define TEXT_VAR_SIZE      32    /* 変数値の最大バイト長 */
#define TEXT_SPEAKER_SIZE  32    /* 話者名の最大バイト長 */

/* テキスト表示状態 */
#define TEXT_STATE_IDLE     0    /* 非表示 (スロット未使用) */
#define TEXT_STATE_TYPING   1    /* タイプライター進行中 */
#define TEXT_STATE_PAUSE    2    /* 一時停止中 (\w による遅延) */
#define TEXT_STATE_WAIT     3    /* 入力待ち (ページ末尾) */
#define TEXT_STATE_DONE     4    /* 全ページ表示完了 */

/* エラーコード */
#define TEXT_OK             0
#define TEXT_ERR_SLOT      (-1)  /* スロット番号範囲外 */
#define TEXT_ERR_DB        (-2)  /* DB取得失敗 */
#define TEXT_ERR_NOTFOUND  (-3)  /* メッセージが見つからない */
#define TEXT_ERR_END       (-4)  /* グループ終端 (次メッセージなし) */

/* ====================================================================== */
/*  2. データ構造体                                                        */
/* ====================================================================== */

/* テキストスロット (1メッセージ分の表示状態) */
typedef struct {
    char buf[TEXT_BUF_SIZE];            /* テキストバッファ (UTF-8, 展開済み) */
    char speaker[TEXT_SPEAKER_SIZE];    /* 話者名 (空文字列=ナレーション) */
    u16  total_len;                     /* テキスト全体のバイト長 */
    u16  visible_len;                   /* 現在表示済みのバイト長 */
    u16  page_offsets[TEXT_MAX_PAGES];  /* 各ページの開始バイト位置 */
    u16  page_ends[TEXT_MAX_PAGES];     /* 各ページの終了バイト位置 */
    u8   page_count;                    /* 総ページ数 */
    u8   current_page;                  /* 現在ページ (0始まり) */
    u8   state;                         /* TEXT_STATE_* */
    u8   speed;                         /* 表示速度 (フレーム/文字, 1=最速) */
    u8   counter;                       /* フレームカウンタ */
    u8   active;                        /* 0=未使用, 1=使用中 */
    u16  pause_remaining;               /* 一時停止残りフレーム */
    /* グループ連続取得用 */
    u16  group_id;                      /* 現在のグループID (0=グループなし) */
    u16  group_seq;                     /* グループ内のシーケンス番号 */
    u16  msg_id;                        /* 現在表示中のメッセージID */
} TextSlot;

/* 変数テーブルエントリ */
typedef struct {
    char value[TEXT_VAR_SIZE];          /* 展開後の文字列 */
} TextVar;

/* メッセージ完了コールバック */
typedef void (*text_done_callback)(int slot, u16 msg_id);

/* ====================================================================== */
/*  3. API — システム管理 (text_core.c)                                    */
/* ====================================================================== */

/* 初期化: DBを開く
 * db_path: テキストDBのパス (例: "/db/text.db")
 * 戻り値: 0=成功, 負=エラー */
int  text_init(const char *db_path);

/* 終了: DB接続クローズ、全スロットリセット */
void text_shutdown(void);

/* ====================================================================== */
/*  4. API — メッセージ操作 (text_core.c)                                  */
/* ====================================================================== */

/* メッセージをDBから取得してスロットにロード
 * slot: スロット番号 (0~3)
 * msg_id: messages テーブルの id
 * 戻り値: TEXT_OK / TEXT_ERR_SLOT / TEXT_ERR_DB / TEXT_ERR_NOTFOUND */
int  text_load(int slot, u16 msg_id);

/* グループの先頭メッセージをロード (連続会話用)
 * 戻り値: TEXT_OK / TEXT_ERR_* */
int  text_load_group(int slot, u16 group_id);

/* 次のメッセージをロード (同一グループ内の seq+1)
 * 戻り値: TEXT_OK / TEXT_ERR_END (グループ終端) */
int  text_next_message(int slot);

/* スロットを閉じる (状態リセット) */
void text_close(int slot);

/* ====================================================================== */
/*  5. API — テキスト演出制御 (text_update.c)                              */
/* ====================================================================== */

/* 毎フレーム呼び出し — 全スロットのTYPING状態を更新
 * UTF-8の1文字単位でvisible_lenを進行 */
void text_update(void);

/* 全文即時表示 (Bボタンスキップ)
 * TYPING → 現在ページの全文を表示 → WAIT or DONE */
void text_skip(int slot);

/* 次ページ送り (WAIT状態で呼ぶ)
 * 戻り値: TEXT_OK=次ページ開始, TEXT_ERR_END=最終ページ完了 */
int  text_advance(int slot);

/* 表示速度変更 (フレーム/文字, 1=最速) */
void text_set_speed(int slot, u8 speed);

/* ====================================================================== */
/*  6. API — 状態取得 (text_update.c)                                      */
/* ====================================================================== */

/* 現在の状態を取得 */
u8   text_get_state(int slot);

/* 表示すべきテキストのポインタと長さ
 * 現在ページの先頭から visible_len まで
 * out_len: 出力バイト長
 * 戻り値: テキストポインタ (NULL=無効スロット) */
const char *text_get_visible(int slot, int *out_len);

/* 話者名を取得 (空文字列=ナレーション, NULL=無効スロット) */
const char *text_get_speaker(int slot);

/* 現在ページ番号 (0始まり) */
u8   text_get_page(int slot);

/* 総ページ数 */
u8   text_get_page_count(int slot);

/* 現在のメッセージID */
u16  text_get_msg_id(int slot);

/* スロットがアクティブか */
int  text_is_active(int slot);

/* ====================================================================== */
/*  7. API — 変数設定 (text_var.c)                                         */
/* ====================================================================== */

/* 変数テーブルに値を設定
 * var_id: 0~15
 * value: 展開文字列 (例: プレイヤー名)
 * text_load 前に設定しておく必要がある */
void text_set_var(int var_id, const char *value);

/* 変数テーブルをクリア */
void text_clear_vars(void);

/* ====================================================================== */
/*  8. API — コールバック (text_core.c)                                    */
/* ====================================================================== */

/* メッセージ完了時コールバック設定 (全ページ表示 + advance 後) */
void text_set_done_callback(text_done_callback cb);

/* ====================================================================== */
/*  9. API — デバッグ (text_core.c)                                        */
/* ====================================================================== */

/* スロット状態ダンプ (kprintf経由) */
void text_debug_dump(int slot);

/* 内部情報取得 (テスト用) */
int  text__slot_active_count(void);

#endif /* LIBOS32TEXT_H */
