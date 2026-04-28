/* ======================================================================== */
/*  AI_DECIDE.C — libos32ai 意思決定コア                                   */
/*                                                                          */
/*  スコアベースの意思決定エンジン。                                         */
/*  ミス判定 → ノイズ加算 → 最高スコア選択の3段階処理。                    */
/* ======================================================================== */

#include "libos32ai.h"
#include "libos32math.h"

/* ====================================================================== */
/*  ai_check_miss — ミス判定                                               */
/*                                                                          */
/*  params[AI_P_MISS] の値(0-100)をランダム行動の発生確率として判定する。    */
/*  0 なら絶対にミスしない、100 なら常にランダム行動。                      */
/*                                                                          */
/*  戻り値: 1=ミス発生, 0=通常判断                                          */
/* ====================================================================== */

int ai_check_miss(const AiProfile *prof)
{
    u8 miss_rate;

    if (prof == NULL) return 0;

    miss_rate = prof->params[AI_P_MISS];
    if (miss_rate == 0) return 0;
    if (miss_rate >= 100) return 1;

    /* rng_range は [min, max] の閉区間で乱数を返す */
    return (rng_range(0, 99) < (int)miss_rate) ? 1 : 0;
}

/* ====================================================================== */
/*  ai_add_noise — スコアにノイズを加算                                    */
/*                                                                          */
/*  params[AI_P_NOISE] の値 N に対し、score ± N の範囲でランダム加算。       */
/*  ノイズ 0 ならそのまま返す。                                              */
/* ====================================================================== */

i16 ai_add_noise(i16 score, const AiProfile *prof)
{
    int noise;
    int delta;

    if (prof == NULL) return score;

    noise = (int)prof->params[AI_P_NOISE];
    if (noise == 0) return score;

    /* -noise ～ +noise の範囲でランダム加算 */
    delta = rng_range(-noise, noise);
    return (i16)(score + delta);
}

/* ====================================================================== */
/*  ai_decide — N個の選択肢から1つ選ぶ (メイン判断関数)                    */
/*                                                                          */
/*  処理フロー:                                                             */
/*    1. count <= 0 → AI_INVALID_ID を返す                                 */
/*    2. count == 1 → 唯一の選択肢を返す                                   */
/*    3. ミス判定 → ミスならランダム選択                                    */
/*    4. 各選択肢のスコアにノイズを加算                                     */
/*    5. 最高スコアの選択肢IDを返す (同スコアは先頭を優先)                  */
/* ====================================================================== */

u8 ai_decide(const AiProfile *prof, const AiOption *opts,
             int count)
{
    int i;
    i16 best_score;
    u8  best_id;

    /* 基本チェック */
    if (opts == NULL || count <= 0) {
        return AI_INVALID_ID;
    }
    if (count == 1) {
        return opts[0].id;
    }

    /* ステップ1: ミス判定 */
    if (ai_check_miss(prof)) {
        /* ランダムに1つ選ぶ */
        return opts[rng_range(0, count - 1)].id;
    }

    /* ステップ2: ノイズ加算 + 最高スコア探索 */
    best_score = ai_add_noise(opts[0].score, prof);
    best_id = opts[0].id;

    for (i = 1; i < count; i++) {
        i16 ns = ai_add_noise(opts[i].score, prof);
        if (ns > best_score) {
            best_score = ns;
            best_id = opts[i].id;
        }
    }

    return best_id;
}
