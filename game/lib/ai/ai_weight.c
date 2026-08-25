/* ======================================================================== */
/*  AI_WEIGHT.C — libos32ai 重み付き選択・先読み評価                        */
/*                                                                          */
/*  重み付きランダム選択: アイテム抽選・イベント抽選に汎用的に使える。       */
/*  先読みスコア集計: N手先の評価を減衰付きで合算する。                      */
/* ======================================================================== */

#include "libos32ai.h"
#include "libos32math.h"

/* ====================================================================== */
/*  ai_weighted_pick — 重み付きランダム選択                                 */
/*                                                                          */
/*  重み配列に基づいて確率的に1つの候補を選択する。                          */
/*  重みが大きいほど選ばれやすい。重み0の候補は選ばれない。                  */
/*                                                                          */
/*  全候補の重みが0の場合は ids[0] を返す (フォールバック)。                 */
/* ====================================================================== */

u16 ai_weighted_pick(const u16 *ids, const u8 *weights,
                      int count)
{
    u32 total;
    u32 r;
    u32 cumulative;
    int i;

    if (ids == NULL || weights == NULL || count <= 0) {
        return 0;
    }
    if (count == 1) {
        return ids[0];
    }

    /* 重み合計を計算 */
    total = 0;
    for (i = 0; i < count; i++) {
        total += (u32)weights[i];
    }

    /* 全重み0ならフォールバック */
    if (total == 0) {
        return ids[0];
    }

    /* 乱数で位置決定 (0 ～ total-1) */
    r = rng_next() % total;

    /* 累積走査で該当候補を特定 */
    cumulative = 0;
    for (i = 0; i < count; i++) {
        cumulative += (u32)weights[i];
        if (r < cumulative) {
            return ids[i];
        }
    }

    /* 通常ここには到達しないが安全のため最後の候補を返す */
    return ids[count - 1];
}

/* ====================================================================== */
/*  ai_lookahead_score — 先読みスコア集計                                   */
/*                                                                          */
/*  N手先のスコアを減衰方式に応じた重み付きで合算する。                      */
/*                                                                          */
/*  decay=0 (AI_DECAY_EQUAL):                                               */
/*    全ステップ同一重み: scores[0] + scores[1] + ... + scores[n-1]         */
/*                                                                          */
/*  decay=1 (AI_DECAY_LINEAR):                                              */
/*    近いステップほど重い: scores[0]*n + scores[1]*(n-1) + ... + scores[n-1]*1 */
/*                                                                          */
/*  decay=2 (AI_DECAY_EXP):                                                 */
/*    指数減衰: scores[0] + scores[1]/2 + scores[2]/4 + ...                 */
/*    (右シフトで実装、ゼロフロア付き)                                      */
/* ====================================================================== */

i16 ai_lookahead_score(const i16 *scores, int count,
                        int decay)
{
    i32 total;
    int i;

    if (scores == NULL || count <= 0) {
        return 0;
    }

    total = 0;

    switch (decay) {
    case AI_DECAY_LINEAR:
        /* 線形減衰: 近いほど重い */
        for (i = 0; i < count; i++) {
            total += (i32)scores[i] * (count - i);
        }
        break;

    case AI_DECAY_EXP:
        /* 指数減衰: 各ステップで半減 */
        for (i = 0; i < count; i++) {
            i32 weighted = (i32)scores[i];
            /* i回右シフト (2^i で除算) */
            if (i < 16) {
                weighted >>= i;
            } else {
                weighted = 0;
            }
            total += weighted;
        }
        break;

    default:
        /* 均等: 単純合算 */
        for (i = 0; i < count; i++) {
            total += (i32)scores[i];
        }
        break;
    }

    /* i16 範囲にクランプ */
    if (total > 32767) total = 32767;
    if (total < -32768) total = -32768;

    return (i16)total;
}
