/* ======================================================================== */
/*  TURN_CORE.C — libos32turn 初期化および進行制御                         */
/* ======================================================================== */

#include "libos32turn.h"
#include "libos32math.h"    /* rng_range */
#include <string.h>

/* ====================================================================== */
/*  turn_init — スケジューラの初期化                                       */
/* ====================================================================== */
void turn_init(TurnState *t, u8 num_players, u8 round_len, u16 max_turns)
{
    int i;

    if (t == (TurnState *)0) return;

    memset(t, 0, sizeof(TurnState));

    t->num_players = (num_players > TURN_MAX_PLAYERS) ? TURN_MAX_PLAYERS : num_players;
    t->current = 0;
    t->round_len = round_len;
    t->order = TURN_ORDER_RR;
    t->turn_count = 1;
    t->round_count = 1;
    t->max_turns = max_turns;
    t->phase = 0;

    /* 全プレイヤーをアクティブにし、スキップ状態をクリア */
    for (i = 0; i < TURN_MAX_PLAYERS; i++) {
        t->active[i] = (i < t->num_players) ? 1 : 0;
        t->skip[i] = 0;
    }
}

/* ====================================================================== */
/*  turn_set_order — 手番順ポリシーの設定                                  */
/* ====================================================================== */
void turn_set_order(TurnState *t, u8 order)
{
    if (t == (TurnState *)0) return;
    t->order = order;
}

/* ====================================================================== */
/*  turn_skip — 指定プレイヤーの手番をNターンスキップ登録                  */
/* ====================================================================== */
void turn_skip(TurnState *t, u8 player, u8 n)
{
    if (t == (TurnState *)0 || player >= t->num_players) return;
    t->skip[player] += n;
}

/* ====================================================================== */
/*  turn_set_active — プレイヤーの参加/脱落設定                            */
/* ====================================================================== */
void turn_set_active(TurnState *t, u8 player, int active)
{
    if (t == (TurnState *)0 || player >= t->num_players) return;
    t->active[player] = active ? 1 : 0;
}

/* ====================================================================== */
/*  turn_advance — 次の有効なプレイヤーへ手番を進める                       */
/* ====================================================================== */
int turn_advance(TurnState *t, TurnAdvance *out)
{
    int next_p;
    int visited[TURN_MAX_PLAYERS];
    int found = 0;
    int i;

    if (t == (TurnState *)0) return 1;

    next_p = t->current;

    /* ゲームがすでに終了している場合 */
    if (turn_is_over(t)) {
        if (out != (TurnAdvance *)0) {
            out->current = t->current;
            out->crossed_round = 0;
            out->is_over = 1;
            out->turn_count = t->turn_count;
            out->round_count = t->round_count;
        }
        return 1;
    }

    memset(visited, 0, sizeof(visited));

    while (1) {
        /* 次の候補プレイヤーを決定 */
        if (t->order == TURN_ORDER_RANDOM) {
            /* アクティブなプレイヤーからランダムに選定 */
            int candidates[TURN_MAX_PLAYERS];
            int cand_count = 0;

            for (i = 0; i < (int)t->num_players; i++) {
                if (t->active[i]) {
                    candidates[cand_count++] = i;
                }
            }

            if (cand_count == 0) {
                break; /* 有効な候補者なし */
            }

            next_p = candidates[rng_range(0, cand_count - 1)];
        } else if (t->order == TURN_ORDER_REVERSE) {
            next_p = (next_p + t->num_players - 1) % t->num_players;
        } else {
            /* TURN_ORDER_RR (ラウンドロビン) */
            next_p = (next_p + 1) % t->num_players;
        }

        /* 探索済みフラグによる無限ループ防止 (RR/REVERSE用) */
        if (t->order != TURN_ORDER_RANDOM) {
            if (visited[next_p]) {
                /* 一周したが誰も見つからなかった場合、スキップを1減らして強制決定 */
                int skipped_any = 0;
                for (i = 0; i < (int)t->num_players; i++) {
                    if (t->active[i] && t->skip[i] > 0) {
                        t->skip[i]--;
                        skipped_any = 1;
                    }
                }
                if (!skipped_any) {
                    break; /* スキップ対象すらいない */
                }
                memset(visited, 0, sizeof(visited)); /* 再探索 */
                continue;
            }
            visited[next_p] = 1;
        } else {
            /* RANDOMの場合、全員がスキップ対象ならスキップカウントを減らす */
            int playable_exists = 0;
            for (i = 0; i < (int)t->num_players; i++) {
                if (t->active[i] && t->skip[i] == 0) {
                    playable_exists = 1;
                    break;
                }
            }
            if (!playable_exists) {
                for (i = 0; i < (int)t->num_players; i++) {
                    if (t->active[i] && t->skip[i] > 0) {
                        t->skip[i]--;
                    }
                }
                continue; /* スキップが減った状態で再抽選 */
            }
        }

        /* アクティブチェック */
        if (!t->active[next_p]) {
            continue;
        }

        /* スキップチェック */
        if (t->skip[next_p] > 0) {
            t->skip[next_p]--;
            continue;
        }

        found = 1;
        break;
    }

    if (found) {
        t->current = (u8)next_p;
        t->turn_count++;

        if (out != (TurnAdvance *)0) {
            out->crossed_round = 0;
        }

        /* ラウンド(週)の境界チェック */
        if (t->round_len > 0 && (t->turn_count - 1) % t->round_len == 0) {
            t->round_count++;
            if (out != (TurnAdvance *)0) {
                out->crossed_round = 1;
            }
        }
    }

    {
        int over = turn_is_over(t);
        if (out != (TurnAdvance *)0) {
            out->current = t->current;
            out->is_over = (u8)over;
            out->turn_count = t->turn_count;
            out->round_count = t->round_count;
        }
        return over;
    }
}
