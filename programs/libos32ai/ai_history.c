/* ======================================================================== */
/*  AI_HISTORY.C — libos32ai 行動履歴・カウンター補正 (Phase 2)             */
/*                                                                          */
/*  プレイヤーの行動パターンを記録し、AIが対策を調整するための機能。          */
/*  - 8種類の行動カウンタ (飽和防止付き半減メカニズム)                       */
/*  - 最頻行動検出                                                          */
/*  - 行動比率に基づくスコア補正 (PUNISH / EXPLOIT)                         */
/* ======================================================================== */

#include "libos32ai.h"
#include <string.h>

/* ====================================================================== */
/*  ai_history_reset — 全カウンタをゼロクリア                               */
/* ====================================================================== */

void ai_history_reset(AiHistory *hist)
{
    if (hist == NULL) return;
    memset(hist, 0, sizeof(AiHistory));
}

/* ====================================================================== */
/*  ai_history_record — 行動を記録                                          */
/*                                                                          */
/*  action_id のカウンタを +1 する。                                        */
/*  total が 255 に達した場合は、全カウンタを半減(右シフト1)させて           */
/*  相対的な比率を維持しつつオーバーフローを防止する。                        */
/*  半減後は total を再計算する。                                            */
/* ====================================================================== */

void ai_history_record(AiHistory *hist, u8 action_id)
{
    int i;
    u8 new_total;

    if (hist == NULL || action_id >= AI_HISTORY_ACTIONS) return;

    /* 飽和防止: total が 254 以上なら全カウンタを半減 */
    if (hist->total >= 254) {
        new_total = 0;
        for (i = 0; i < AI_HISTORY_ACTIONS; i++) {
            hist->action_counts[i] >>= 1;
            new_total += hist->action_counts[i];
        }
        hist->total = new_total;
    }

    /* カウンタ加算 */
    hist->action_counts[action_id]++;
    hist->total++;
}

/* ====================================================================== */
/*  ai_history_most_common — 最頻行動IDを返す                               */
/*                                                                          */
/*  同数の場合は最小IDを優先する (安定性のため)。                            */
/*  履歴が空(total==0)の場合は 0 を返す。                                   */
/* ====================================================================== */

u8 ai_history_most_common(const AiHistory *hist)
{
    int i;
    u8 best_id;
    u8 best_count;

    if (hist == NULL || hist->total == 0) return 0;

    best_id = 0;
    best_count = hist->action_counts[0];

    for (i = 1; i < AI_HISTORY_ACTIONS; i++) {
        if (hist->action_counts[i] > best_count) {
            best_count = hist->action_counts[i];
            best_id = (u8)i;
        }
    }

    return best_id;
}

/* ====================================================================== */
/*  ai_history_ratio — 指定行動の選択率を返す (0-100%)                      */
/*                                                                          */
/*  total==0 の場合は 0 を返す。                                            */
/*  計算: action_counts[action_id] * 100 / total                            */
/* ====================================================================== */

u8 ai_history_ratio(const AiHistory *hist, u8 action_id)
{
    if (hist == NULL || action_id >= AI_HISTORY_ACTIONS) return 0;
    if (hist->total == 0) return 0;

    return (u8)((u16)hist->action_counts[action_id] * 100 / hist->total);
}

/* ====================================================================== */
/*  ai_counter_score — 履歴ベースのスコア補正                               */
/*                                                                          */
/*  相手のaction_idの選択率に基づいてbase_scoreを補正する。                  */
/*                                                                          */
/*  AI_COUNTER_PUNISH (防御的):                                             */
/*    相手が多用するaction_idほどスコアを下げる。                            */
/*    → 相手の得意パターンを避けるAIに使用。                               */
/*    補正値 = -(ratio * strength / 100)                                     */
/*                                                                          */
/*  AI_COUNTER_EXPLOIT (攻撃的):                                            */
/*    相手が多用するaction_idほどスコアを上げる。                            */
/*    → 相手の癖を狙い撃ちするAIに使用。                                   */
/*    補正値 = +(ratio * strength / 100)                                     */
/*                                                                          */
/*  strength=0 → 補正なし, strength=100 → ratio%がそのまま加減算           */
/* ====================================================================== */

i16 ai_counter_score(i16 base_score, u8 action_id,
                      const AiHistory *hist, u8 strength,
                      int mode)
{
    u8 ratio;
    i16 delta;

    if (hist == NULL || strength == 0) return base_score;

    ratio = ai_history_ratio(hist, action_id);
    if (ratio == 0) return base_score;

    /* delta = ratio * strength / 100 */
    delta = (i16)((u16)ratio * (u16)strength / 100);

    switch (mode) {
    case AI_COUNTER_EXPLOIT:
        return (i16)(base_score + delta);
    case AI_COUNTER_PUNISH:
    default:
        return (i16)(base_score - delta);
    }
}
