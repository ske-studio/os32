/* ======================================================================== */
/*  TURN_QUERY.C — libos32turn 状態クエリおよびフェーズ制御               */
/* ======================================================================== */

#include "libos32turn.h"

/* ====================================================================== */
/*  turn_current — 現在の手番プレイヤーIDを取得                           */
/* ====================================================================== */
u8 turn_current(const TurnState *t)
{
    if (t == (const TurnState *)0) return 0;
    return t->current;
}

/* ====================================================================== */
/*  turn_is_round_boundary — 現在の手番がラウンド(週)の先頭か判定         */
/* ====================================================================== */
int turn_is_round_boundary(const TurnState *t)
{
    if (t == (const TurnState *)0) return 0;
    if (t->round_len == 0) return 0;

    /* 1起点カウントで、(turn - 1) % len == 0 のとき先頭 (1, 8, 15...) */
    return ((t->turn_count - 1) % t->round_len == 0) ? 1 : 0;
}

/* ====================================================================== */
/*  turn_round — 現在の累計ラウンド(週)数を取得                           */
/* ====================================================================== */
u16 turn_round(const TurnState *t)
{
    if (t == (const TurnState *)0) return 1;
    return t->round_count;
}

/* ====================================================================== */
/*  turn_count — 現在の累計ターン数を取得                                 */
/* ====================================================================== */
u16 turn_count(const TurnState *t)
{
    if (t == (const TurnState *)0) return 1;
    return t->turn_count;
}

/* ====================================================================== */
/*  turn_alive_count — アクティブな生存プレイヤー数を取得                 */
/* ====================================================================== */
int turn_alive_count(const TurnState *t)
{
    int count = 0;
    int i;

    if (t == (const TurnState *)0) return 0;

    for (i = 0; i < (int)t->num_players; i++) {
        if (t->active[i]) {
            count++;
        }
    }
    return count;
}

/* ====================================================================== */
/*  turn_is_over — ゲーム終了判定                                         */
/* ====================================================================== */
int turn_is_over(const TurnState *t)
{
    int alive;

    if (t == (const TurnState *)0) return 1;

    alive = turn_alive_count(t);

    /* 参加人数が2名以上の時、生存者が1名以下になれば終了 */
    if (t->num_players > 1 && alive <= 1) {
        return 1;
    }
    /* 1人プレイの時は、生存者が0名（脱落）になれば終了 */
    if (t->num_players == 1 && alive == 0) {
        return 1;
    }

    /* 最大ターン数制限到達時 */
    if (t->max_turns > 0 && t->turn_count > t->max_turns) {
        return 1;
    }

    return 0;
}

/* ====================================================================== */
/*  ターン内フェーズの制御                                                 */
/* ====================================================================== */
void turn_set_phase(TurnState *t, u8 phase)
{
    if (t == (TurnState *)0) return;
    t->phase = phase;
}

u8 turn_get_phase(const TurnState *t)
{
    if (t == (const TurnState *)0) return 0;
    return t->phase;
}
