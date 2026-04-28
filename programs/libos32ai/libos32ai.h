/* ======================================================================== */
/*  LIBOS32AI.H — OS32 汎用AI意思決定エンジン 公開ヘッダ                    */
/*                                                                          */
/*  スコアベースの意思決定・重み付き抽選・先読み評価を提供する                */
/*  ゲームロジック非依存の汎用AIライブラリ。                                  */
/*                                                                          */
/*  依存: libos32math (rng_*), libos32db (KAPI経由SQLite)                   */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32AI_H
#define LIBOS32AI_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 定数                                                                */
/* ====================================================================== */

#define AI_PARAM_MAX       16   /* 汎用パラメータスロット数 */
#define AI_MAX_PROFILES    32   /* 同時ロード可能なプロファイル数 */
#define AI_INVALID_ID      0xFF /* 無効な選択肢ID */

/* ライブラリが内部で使用する標準パラメータインデックス */
#define AI_P_MISS          0    /* ランダム行動確率 (0-100%) */
#define AI_P_NOISE         1    /* スコアノイズ幅 (±N) */

/* ゲーム側が自由に定義する拡張インデックス例:                              */
/* #define AI_P_AGGRO       2 */  /* 攻撃傾向 */
/* #define AI_P_CAUTION     3 */  /* 慎重さ */
/* #define AI_P_GREED       4 */  /* けちさ */

/* 先読み減衰方式 (ai_lookahead_score の decay 引数) */
#define AI_DECAY_EQUAL     0    /* 均等: 全ステップ同一重み */
#define AI_DECAY_LINEAR    1    /* 線形: 近いほど重い */
#define AI_DECAY_EXP       2    /* 指数: 急速に減衰 */

/* 履歴定数 (Phase 2) */
#define AI_HISTORY_ACTIONS 8    /* 追跡する行動種別の最大数 */

/* カウンター補正モード (ai_counter_score の mode 引数) */
#define AI_COUNTER_PUNISH  0    /* 頻出行動のスコアを下げる */
#define AI_COUNTER_EXPLOIT 1    /* 頻出行動を狙い撃ちする */

/* ====================================================================== */
/*  2. データ構造                                                          */
/* ====================================================================== */

/* 性格プロファイル */
typedef struct {
    u8   params[AI_PARAM_MAX];  /* 汎用パラメータ配列 */
    u8   param_count;           /* 使用中スロット数 */
    u8   _pad[3];
} AiProfile;                    /* 20B */

/* 評価済み選択肢 */
typedef struct {
    u8   id;             /* 選択肢ID (0-254, 0xFF=無効) */
    u8   _pad;
    i16  score;          /* 評価スコア (高いほど良い, 負値も可) */
} AiOption;              /* 4B */

/* 行動履歴 (Phase 2) — プレイヤーの行動パターン記録 */
typedef struct {
    u8   action_counts[AI_HISTORY_ACTIONS]; /* 各行動ID (0~7) の選択回数 */
    u8   total;                             /* 記録された総行動数 */
    u8   _pad[3];
} AiHistory;             /* 12B */

/* ====================================================================== */
/*  3. API — システム管理 (ai_core.c)                                      */
/* ====================================================================== */

/* 初期化: DBからプロファイルをRAMにキャッシュ
 * db_path: ai.db のパス (NULL=DBなし、手動プロファイル設定のみ)
 * 戻り値: 0=成功, 負値=エラー
 */
int  ai_init(const char *db_path);

/* 終了: DB接続クローズ、内部状態リセット */
void ai_shutdown(void);

/* プロファイル管理 */
int  ai_load_profile(u8 profile_id, AiProfile *out);
int  ai_profile_count(void);

/* パラメータアクセサ */
u8   ai_get_param(const AiProfile *prof, u8 idx);
void ai_set_param(AiProfile *prof, u8 idx, u8 value);

/* ランタイムプロファイル編集 (Phase 2)
 * RAMキャッシュ内のプロファイルを直接更新する。
 * ai_load_profile で取得したローカルコピーではなく、
 * グローバルキャッシュ自体を変更するため、以降の ai_load_profile に反映される。
 *
 * 戻り値: 0=成功, -1=範囲外
 */
int  ai_update_profile(u8 profile_id, const AiProfile *prof);

/* プロファイルDB永続化 (Phase 2)
 * RAMキャッシュ上の指定プロファイルをDBに書き戻す。
 * DB未接続時は -1 を返す。
 *
 * 戻り値: 0=成功, 負値=エラー
 */
int  ai_save_profile(u8 profile_id);

/* ====================================================================== */
/*  4. API — 意思決定 (ai_decide.c)                                        */
/* ====================================================================== */

/* N個の選択肢からスコア+性格に基づいて1つ選ぶ
 *
 * 処理:
 *   1. ミス判定: params[AI_P_MISS]% でランダム選択を返す
 *   2. 各選択肢のスコアに ±params[AI_P_NOISE] のノイズを加算
 *   3. 最高スコアの選択肢IDを返す
 *
 * prof:  性格プロファイル
 * opts:  評価済み選択肢の配列
 * count: 選択肢数 (1以上)
 * 戻り値: 選ばれた選択肢の id フィールド
 */
u8   ai_decide(const AiProfile *prof, const AiOption *opts,
               int count);

/* ミス判定のみ
 * 戻り値: 1=ミス発生(呼出元でランダム行動を実行すべき), 0=通常判断
 */
int  ai_check_miss(const AiProfile *prof);

/* スコアにノイズを加算して返す
 * ノイズ範囲: score ± params[AI_P_NOISE]
 */
i16  ai_add_noise(i16 score, const AiProfile *prof);

/* ====================================================================== */
/*  5. API — 重み付き選択 (ai_weight.c)                                    */
/* ====================================================================== */

/* 重み付きランダム選択
 * アイテム抽選、イベント抽選、ドロップ判定等に使用。
 *
 * ids:     候補IDの配列
 * weights: 各候補の重み (0=候補外, 大きいほど選ばれやすい)
 * count:   候補数
 * 戻り値:  選ばれた候補の ID (全候補の重みが0の場合は ids[0])
 *
 * アルゴリズム: 重み合計を計算 → 乱数で位置決定 → 累積走査
 */
u16  ai_weighted_pick(const u16 *ids, const u8 *weights,
                       int count);

/* 先読みスコア集計
 * N手先のスコアを距離減衰付きで合算する。
 *
 * scores: 各ステップの評価スコア [0]=1手目, [1]=2手目, ...
 * count:  ステップ数
 * decay:  減衰方式 (AI_DECAY_*)
 * 戻り値: 集計スコア
 *
 * 線形減衰例 (count=3, decay=AI_DECAY_LINEAR):
 *   total = scores[0]*3 + scores[1]*2 + scores[2]*1
 */
i16  ai_lookahead_score(const i16 *scores, int count,
                         int decay);

/* ====================================================================== */
/*  6. API — 行動履歴・カウンター補正 (ai_history.c)  [Phase 2]            */
/* ====================================================================== */

/* 履歴リセット: 全カウンタをゼロクリア */
void ai_history_reset(AiHistory *hist);

/* 行動記録: action_id (0~AI_HISTORY_ACTIONS-1) の選択回数を+1
 * total が 255 に達した場合は全カウンタを半減させる (飽和防止)。
 */
void ai_history_record(AiHistory *hist, u8 action_id);

/* 最頻行動ID を返す (同数の場合は最小IDを優先)
 * 戻り値: 0~AI_HISTORY_ACTIONS-1, 履歴なし=0
 */
u8   ai_history_most_common(const AiHistory *hist);

/* 指定行動の選択率を返す (0-100%)
 * total==0 の場合は 0 を返す。
 */
u8   ai_history_ratio(const AiHistory *hist, u8 action_id);

/* カウンタースコア補正
 * 相手の行動パターン（履歴）に基づき、base_score を補正して返す。
 *
 * base_score: 元のスコア
 * action_id:  この行動のID (0~AI_HISTORY_ACTIONS-1)
 * hist:       対象プレイヤーの行動履歴
 * strength:   補正強度 (0=無補正, 大きいほど強く補正)
 * mode:       AI_COUNTER_PUNISH  — 相手が多用する行動のスコアを下げる
 *             AI_COUNTER_EXPLOIT — 相手が多用する行動を狙い撃ちする
 *
 * PUNISH:  ratio が高い行動ほどスコアが下がる
 *   補正値 = -(ratio * strength / 100)
 *
 * EXPLOIT: ratio が高い行動ほどスコアが上がる
 *   補正値 = +(ratio * strength / 100)
 *
 * 戻り値: 補正済みスコア
 */
i16  ai_counter_score(i16 base_score, u8 action_id,
                       const AiHistory *hist, u8 strength,
                       int mode);

#endif /* LIBOS32AI_H */
