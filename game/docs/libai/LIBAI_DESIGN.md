# libos32ai — 汎用AI意思決定エンジン設計書

*策定: 2026-04-28*

> この文書は、OS32のゲーム開発基盤として、スコアベースの意思決定・
> 重み付き抽選・先読み評価を提供する汎用AIライブラリ
> `libos32ai` の設計思想・API仕様・実装計画を定義する。

---

## 1. 設計背景

### 1.1 なぜ libos32ai が必要か

1. **CPU AIのハードコーディング** — 既存ゲームのCPU判断は `if/switch` の羅列で
   性格変更にコード修正が必要
2. **判断フレームワークの不在** — ミス率・ノイズ・重み付き選択といった
   共通パターンがゲームごとに再実装されている
3. **テスト不能** — UIや状態遷移と密結合しているため、AI判断の単体テストができない

### 1.2 設計思想

- **純粋計算ユニット**: 入力(選択肢+スコア+性格)を受け取り、選ばれたIDを返すだけ
- **状態を持たない**: ライブラリ自体は判断履歴を保持しない。記憶が必要ならゲーム側で管理
- **性格はデータ駆動**: SQLite DBからプロファイルをロードし、パラメータ配列で表現
- **ゲームロジック非依存**: ボードゲーム・RPG・カードゲーム等、ジャンルを問わない

### 1.3 核心概念

```
[ゲーム側]                    [libos32ai]
  選択肢A: score=13  ──┐
  選択肢B: score=6   ──┤──→ ai_decide() ──→ 選択肢AのID
  性格: noise=4      ──┘     (ノイズ加算+ミス判定)
```

---

## 2. アーキテクチャ

### 2.1 依存関係

```
libos32db    (SQLiteアクセス)
     ^
libos32math  (乱数, fix16_t)
     ^
libos32ai    (db + math)
     ^
     ├── libos32battle  (バトルAI判断を委ねる)
     ├── libos32board   (方向選択判断を委ねる)
     ├── libos32event   (重み付きイベント抽選)
     └── ゲーム本体     (CPU行動全般)
```

**重要な制約**:
- libos32ai は **他のゲームライブラリに依存しない**
- GFX/描画には一切依存しない
- KernelAPI には libos32db 経由でのみ間接依存

### 2.2 ディレクトリ構成

```
programs/libos32ai/
    libos32ai.h         公開ヘッダ (全API宣言 + 型定義 + 定数)
    ai_core.c           初期化, 終了, プロファイルロード
    ai_decide.c         意思決定コア (decide, miss, noise)
    ai_weight.c         重み付き選択, 先読み評価
```

---

## 3. コアデータ構造

### 3.1 性格プロファイル

```c
#define AI_PARAM_MAX  16   /* 汎用パラメータスロット数 */

typedef struct {
    u8   params[AI_PARAM_MAX];  /* 汎用パラメータ配列 */
    u8   param_count;           /* 使用中スロット数 */
    u8   _pad[3];
} AiProfile;
```

ライブラリが参照する標準パラメータインデックス:

```c
/* libos32ai が内部で使用する標準インデックス */
#define AI_P_MISS      0   /* ランダム行動確率 (0-100%) */
#define AI_P_NOISE     1   /* スコアノイズ幅 (±N) */

/* ゲーム側が自由に定義する拡張インデックス */
/* #define AI_P_AGGRO     2 */  /* 攻撃傾向 */
/* #define AI_P_CAUTION   3 */  /* 慎重さ */
/* #define AI_P_GREED     4 */  /* けちさ */
```

**拡張性ポイント**: ライブラリコアは `params[0]` (ミス率) と `params[1]` (ノイズ)
のみを参照する。残りの14スロットはゲーム側がスコア計算時に自由に利用する。
新しい性格軸の追加にライブラリ側のコード変更は不要。

### 3.2 評価済み選択肢

```c
typedef struct {
    u8   id;             /* 選択肢ID (0-254, 0xFF=無効) */
    u8   _pad;
    i16  score;          /* 評価スコア (高いほど良い, 負値も可) */
} AiOption;
```

### 3.3 内部状態

```c
/* ai_core.c 内部 — グローバル状態 */
#define AI_MAX_PROFILES  32

static AiProfile g_profiles[AI_MAX_PROFILES];
static u8        g_profile_count;
static int       g_db_slot;   /* libos32db のスロット番号 (-1=未接続) */
```

---

## 4. API設計

### 4.1 システム管理 (ai_core.c)

```c
/* 初期化: DBからプロファイルをRAMにキャッシュ
 * db_path: ai.db のパス (NULL=DBなし、手動プロファイル設定のみ)
 * 戻り値: 0=成功, 負値=エラー
 */
int  ai_init(const char *db_path);

/* 終了: DB接続クローズ */
void ai_shutdown(void);

/* プロファイル管理 */
int  ai_load_profile(u8 profile_id, AiProfile *out);
int  ai_profile_count(void);

/* パラメータアクセサ */
u8   ai_get_param(const AiProfile *prof, u8 idx);
void ai_set_param(AiProfile *prof, u8 idx, u8 value);
```

### 4.2 意思決定 (ai_decide.c)

```c
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
```

### 4.3 重み付き選択 (ai_weight.c)

```c
/* 重み付きランダム選択
 * アイテム抽選、イベント抽選、ドロップ判定等に使用。
 *
 * ids:     候補IDの配列
 * weights: 各候補の重み (0=候補外, 大きいほど選ばれやすい)
 * count:   候補数
 * 戻り値:  選ばれた候補の ID
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
 * decay:  減衰方式 (0=均等, 1=線形減衰, 2=指数減衰)
 * 戻り値: 集計スコア
 *
 * 線形減衰例 (count=3, decay=1):
 *   total = scores[0]*3 + scores[1]*2 + scores[2]*1
 */
i16  ai_lookahead_score(const i16 *scores, int count,
                         int decay);
```

---

## 5. DBスキーマ (ai.db)

```sql
CREATE TABLE profiles (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    p0_miss     INTEGER NOT NULL DEFAULT 10,
    p1_noise    INTEGER NOT NULL DEFAULT 3,
    p2          INTEGER NOT NULL DEFAULT 0,
    p3          INTEGER NOT NULL DEFAULT 0,
    p4          INTEGER NOT NULL DEFAULT 0,
    p5          INTEGER NOT NULL DEFAULT 0,
    p6          INTEGER NOT NULL DEFAULT 0,
    p7          INTEGER NOT NULL DEFAULT 0,
    p8          INTEGER NOT NULL DEFAULT 0,
    p9          INTEGER NOT NULL DEFAULT 0,
    p10         INTEGER NOT NULL DEFAULT 0,
    p11         INTEGER NOT NULL DEFAULT 0,
    p12         INTEGER NOT NULL DEFAULT 0,
    p13         INTEGER NOT NULL DEFAULT 0,
    p14         INTEGER NOT NULL DEFAULT 0,
    p15         INTEGER NOT NULL DEFAULT 0
);
```

p0/p1 はライブラリが使用する標準パラメータ。p2〜p15 はゲーム側が自由に定義。

---

## 6. 処理フロー

### 6.1 ai_decide 内部処理

```
ai_decide(prof, opts, count)
  │
  ├── ミス判定: rng_range(100) < prof->params[AI_P_MISS] ?
  │   └── YES → opts[rng_range(count)].id を返す (ランダム)
  │
  ├── for each opt in opts:
  │   └── noisy_score = opt.score + rng_range(noise*2+1) - noise
  │
  ├── 最高 noisy_score の opt を探す
  │   └── 同スコアの場合: 先頭を採用 (安定ソート相当)
  │
  └── return best.id
```

### 6.2 ゲーム側での使用例

```c
/* ドカポンのCPU方向選択 */
void cpu_choose_direction(u8 pid, u8 *dirs, int dir_count)
{
    AiOption opts[4];
    AiProfile prof;
    int i;
    u8 chosen;

    ai_load_profile(players[pid].ai_type, &prof);

    /* ゲーム固有: 各方向のスコアを評価 */
    for (i = 0; i < dir_count; i++) {
        opts[i].id = dirs[i];
        opts[i].score = evaluate_direction(pid, dirs[i]);

        /* ゲーム固有パラメータの活用 */
        if (mass_is_shop(dirs[i]) && ai_get_param(&prof, AI_P_GREED) > 50) {
            opts[i].score += 5;  /* けちなAIはショップを好む */
        }
    }

    /* AI基盤に判断を委ねる */
    chosen = ai_decide(&prof, opts, dir_count);
    player_move_to(pid, chosen);
}
```

---

## 7. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~600B |
| AiProfile[32] | 640B |
| 内部変数 | ~16B |
| **合計 RAM** | **~1.3KB** |

---

## 8. 拡張設計

### 8.1 学習機能 (P2)

プレイヤーの行動パターンを記録し、対策を調整する:

```c
#define AI_HISTORY_MAX  16

typedef struct {
    u8   action_counts[8];   /* 各行動の選択回数 */
    u8   total;
} AiHistory;

/* 履歴ベースのスコア補正 */
i16  ai_counter_score(i16 base_score, u8 action_id,
                       const AiHistory *hist);
```

### 8.2 複数段階判断 (P1)

「攻撃/防御/逃走」→「対象選択」→「技選択」のような多段階判断は
`ai_decide()` を複数回呼ぶことで実現:

```c
/* 第1段階: 行動種別 */
u8 action = ai_decide(&prof, action_opts, 3);
/* 第2段階: 対象選択 */
u8 target = ai_decide(&prof, target_opts, enemy_count);
```

---

## 9. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32ai.h` ヘッダ作成
- [x] `ai_core.c` 実装 (init, shutdown, プロファイルロード)
- [x] `ai_decide.c` 実装 (decide, check_miss, add_noise)
- [x] `ai_weight.c` 実装 (weighted_pick, lookahead_score)
- [x] Makefile 統合
- [x] テストプログラム `ai_test.c` 作成
- [x] `ai.db` サンプルデータ作成

### Phase 2: 拡張

- [x] 学習機能 (AiHistory)
- [x] カウンタースコア補正
- [x] プロファイルのランタイム編集API

---

## 10. 関連ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [LIBMATH_DESIGN.md](../../../docs/tasks/libmath/LIBMATH_DESIGN.md) | libos32math 設計書 (乱数依存先) |
| [LIBBATTLE_DESIGN.md](../libbattle/LIBBATTLE_DESIGN.md) | バトルエンジン (AI判断の消費者) |
| [LIBBOARD_DESIGN.md](../libboard/LIBBOARD_DESIGN.md) | ボードエンジン (方向選択の消費者) |

---

*この設計書は libos32ai の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
