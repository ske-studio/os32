/* ======================================================================== */
/*  LIBOS32TURN.H — OS32 手番・ラウンド・週スケジューラ 公開ヘッダ          */
/*                                                                          */
/*  多人数ゲームの手番進行、ラウンド境界（週）、最大ターン、スキップ、       */
/*  およびゲーム終了判定を管理する汎用ライブラリ。                          */
/* ======================================================================== */

#ifndef LIBOS32TURN_H
#define LIBOS32TURN_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i16, i32 */

/* 最大同時プレイ人数 */
#define TURN_MAX_PLAYERS  8

/* 手番順序ポリシー */
#define TURN_ORDER_RR      0   /* ラウンドロビン (既定値) */
#define TURN_ORDER_REVERSE 1   /* 逆順 */
#define TURN_ORDER_RANDOM  2   /* 毎ラウンド・ランダム */

/* スケジューラ状態構造体 */
typedef struct {
    u8   num_players;                 /* 参加人数 */
    u8   current;                     /* 現在の手番プレイヤー (0〜num_players-1) */
    u8   round_len;                   /* 1ラウンドのターン数（週=7） */
    u8   order;                       /* TURN_ORDER_* */
    u16  turn_count;                  /* 累計ターン数 (1起点) */
    u16  round_count;                 /* 累計ラウンド数 (=週, 1起点) */
    u16  max_turns;                   /* 最大ターン数制限 (0=無制限) */
    u8   active[TURN_MAX_PLAYERS];    /* 1=参加中, 0=脱落 */
    u8   skip[TURN_MAX_PLAYERS];      /* 手番スキップカウンタ (0=行動可能, >0=スキップするターン数) */
    u8   phase;                       /* ターン内サブフェーズ (ゲーム側任意定義) */
    u8   _pad;
} TurnState;

/* turn_advance の結果出力構造体 */
typedef struct {
    u8   current;          /* 新しい手番プレイヤー */
    u8   crossed_round;    /* 1=この進行でラウンド(週)境界を跨いだ, 0=跨いでいない */
    u8   is_over;          /* 1=最大ターン到達または生存者が1人以下 (ゲーム終了), 0=継続 */
    u8   _pad;
    u16  turn_count;       /* 進行後の累計ターン数 */
    u16  round_count;      /* 進行後の累計ラウンド数 */
} TurnAdvance;

/* ====================================================================== */
/*  API — システム管理 (turn_core.c)                                      */
/* ====================================================================== */

/* 初期化
 * t: 対象構造体
 * num_players: 参加プレイヤー数
 * round_len: 1ラウンドのターン数 (通常7)
 * max_turns: 最大ターン数 (0=無制限)
 */
void turn_init(TurnState *t, u8 num_players, u8 round_len, u16 max_turns);

/* 手番順序ポリシーの設定 */
void turn_set_order(TurnState *t, u8 order);

/* 進行: 次の有効なプレイヤーへ手番を進める
 * t: 対象構造体
 * out: 進行結果の格納先
 * 戻り値: 1=ゲーム終了, 0=ゲーム継続
 */
int  turn_advance(TurnState *t, TurnAdvance *out);

/* 参加状態・スキップ制御 */
void turn_skip(TurnState *t, u8 player, u8 n);     /* 指定プレイヤーの手番をNターン飛ばす */
void turn_set_active(TurnState *t, u8 player, int active); /* 参加状態(脱落)の変更 */

/* ====================================================================== */
/*  API — クエリ (turn_query.c)                                           */
/* ====================================================================== */

u8   turn_current(const TurnState *t);             /* 現在の手番プレイヤー */
int  turn_is_round_boundary(const TurnState *t);   /* 現在の手番がラウンドの最初か */
u16  turn_round(const TurnState *t);               /* 現在のラウンド数 */
u16  turn_count(const TurnState *t);               /* 現在の累計ターン数 */
int  turn_alive_count(const TurnState *t);         /* 生存(アクティブ)プレイヤー数 */
int  turn_is_over(const TurnState *t);             /* ゲーム終了条件を満たしているか */

/* サブフェーズ制御 */
void turn_set_phase(TurnState *t, u8 phase);       /* ターン内フェーズの設定 */
u8   turn_get_phase(const TurnState *t);           /* ターン内フェーズの取得 */

#endif /* LIBOS32TURN_H */
