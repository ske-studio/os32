# libos32event — イベントスケジューラ設計書

*策定: 2026-04-28*

> この文書は、ターン/週ベースの周期イベント・確率イベント・条件イベントを
> 管理するスケジューラライブラリ `libos32event` の設計思想・API仕様・実装計画を定義する。

---

## 1. 設計背景

### 1.1 なぜ libos32event が必要か

1. **イベント判定のハードコード** — クールダウン・確率上昇・重み付き抽選が
   `if/switch` の羅列で再利用不可
2. **条件イベントの不在** — 「HP30%以下で発動」等の条件判定が場当たり的
3. **イベント排他の不在** — 同時発生を防ぐ排他グループの概念がない
4. **アクティブイベント管理の不在** — 持続イベント(物忌みの日=1週間ショップ閉鎖)の
   ターンカウントダウンが散在

### 1.2 設計思想

- **「何が発火するか」を計算して返すだけ**: イベント内容の実行はゲーム側
- **3種の発火条件**: 周期(Nターンごと)・確率(クールダウン+重み)・条件(コールバック)
- **排他グループ**: 同グループのイベントは同時アクティブにならない
- **DB駆動**: イベント定義はSQLiteからロード。ランタイム状態はRAM
- **連鎖対応**: イベントA発火→イベントBも発火するチェイン

---

## 2. アーキテクチャ

### 2.1 依存関係

```
libos32db   (SQLiteアクセス)
     ^
libos32ai   (重み付き抽選 — ai_weighted_pick を利用)
     ^
libos32event (db + ai)
     ^
     └── ゲーム本体 (ターン進行・イベント実行)
```

### 2.2 ディレクトリ構成

```
programs/libos32event/
    libos32event.h        公開ヘッダ
    evt_core.c            初期化, 終了, DBロード
    evt_tick.c            毎ターン更新, 発火判定
    evt_active.c          アクティブイベント管理
    evt_trigger.c         手動発火, 連鎖処理
```

---

## 3. コアデータ構造

### 3.1 イベント定義

```c
#define EVT_MAX_DEFS     64  /* イベント定義上限 */

/* 発火タイプ */
#define EVT_TYPE_PERIODIC   0  /* N ターンごとに自動発火 */
#define EVT_TYPE_RANDOM     1  /* 確率発火 (クールダウン+重み上昇) */
#define EVT_TYPE_CONDITION  2  /* 条件コールバックがtrueで発火 */

/* スコープ */
#define EVT_SCOPE_GLOBAL    0  /* 全体イベント */
#define EVT_SCOPE_PLAYER    1  /* 個別プレイヤーイベント */

typedef struct {
    u16  id;
    u8   type;            /* EVT_TYPE_* */
    u8   weight;          /* 発生重み (RANDOM時) */
    u16  min_turn;        /* 最小発生ターン */
    u16  cooldown;        /* 発生後クールダウン (ターン数) */
    u16  period;          /* 周期 (PERIODIC時: Nターンごと) */
    u8   duration;        /* 持続ターン (0=瞬時) */
    u8   group;           /* 排他グループ (同グループ同時不可, 0=制限なし) */
    u16  chain_id;        /* 連鎖先イベントID (0=なし) */
    u8   chain_chance;    /* 連鎖確率% (1-100) */
    u8   scope;           /* EVT_SCOPE_* */
} EvtDef;
```

### 3.2 アクティブイベント

```c
#define EVT_MAX_ACTIVE   16  /* 同時アクティブ上限 */

typedef struct {
    u16  event_id;        /* イベントID (0=空き) */
    u8   remaining;       /* 残りターン数 */
    u8   target;          /* スコーププレイヤーID (GLOBAL時は0xFF) */
} EvtActive;
```

### 3.3 ランタイム状態

```c
/* evt_core.c 内部 */
static EvtDef    g_defs[EVT_MAX_DEFS];
static u8        g_def_count;
static EvtActive g_active[EVT_MAX_ACTIVE];

/* 確率発火用カウンタ (未発生連続ターン数 → 確率上昇) */
static u16       g_no_event_counter;

/* 各イベントのクールダウン残りターン */
static u16       g_cooldowns[EVT_MAX_DEFS];

/* 前回tickで発火したイベントID一時バッファ */
#define EVT_FIRED_MAX  8
static u16       g_fired[EVT_FIRED_MAX];
static u8        g_fired_count;
```

---

## 4. API設計

### 4.1 システム管理 (evt_core.c)

```c
int  evt_init(const char *db_path);
void evt_shutdown(void);
void evt_reset(void);   /* 全ランタイム状態リセット */
```

### 4.2 毎ターン更新 (evt_tick.c)

```c
/* 毎ターン呼ぶ: 発火判定 + アクティブ更新
 *
 * 処理:
 *   1. アクティブイベントの残りターンをデクリメント (0で自動終了)
 *   2. 全クールダウンをデクリメント
 *   3. PERIODIC イベント: current_turn % period == 0 なら発火
 *   4. RANDOM イベント: g_no_event_counter > min_turn なら確率判定
 *   5. CONDITION イベント: コールバックがtrue なら発火
 *   6. 排他グループチェック
 *   7. 連鎖処理
 *
 * current_turn: 現在のターン数
 * context:      条件コールバックに渡すゲーム側コンテキスト
 * 戻り値:       発火したイベント数
 */
int  evt_tick(u16 current_turn, const void *context);

/* 前回tickで発火したイベントIDを取得 */
int  evt_get_fired(u16 *out_ids, int max);
```

### 4.3 条件コールバック

```c
/* 条件判定関数 (ゲーム側が実装)
 * event_id: 判定対象のイベントID
 * turn:     現在ターン
 * context:  evt_tick() に渡された context ポインタ
 * 戻り値:   1=条件成立(発火), 0=不成立
 */
typedef int (*evt_condition_fn)(u16 event_id, u16 turn,
                                 const void *context);

/* コールバック登録 */
void evt_set_condition_callback(evt_condition_fn fn);
```

### 4.4 アクティブイベント管理 (evt_active.c)

```c
/* 特定イベントがアクティブか */
int  evt_is_active(u16 event_id);

/* アクティブイベント一覧取得 */
int  evt_active_list(EvtActive *out, int max);

/* アクティブイベント数 */
int  evt_active_count(void);

/* 手動終了 (持続イベントを強制停止) */
void evt_cancel(u16 event_id);
```

### 4.5 手動発火 (evt_trigger.c)

```c
/* 手動でイベントを発火 (ボス討伐報酬等)
 * target: 対象プレイヤーID (GLOBAL時は0xFF)
 * 戻り値: 0=成功, -1=排他制約違反, -2=アクティブ上限
 */
int  evt_trigger(u16 event_id, u8 target);
```

### 4.6 カウンタ操作

```c
void evt_reset_counter(void);
u16  evt_get_counter(void);
```

---

## 5. DBスキーマ (events.db)

```sql
CREATE TABLE events (
    id           INTEGER PRIMARY KEY,
    type         INTEGER NOT NULL DEFAULT 1,
    weight       INTEGER NOT NULL DEFAULT 5,
    min_turn     INTEGER DEFAULT 0,
    cooldown     INTEGER DEFAULT 10,
    period       INTEGER DEFAULT 0,
    duration     INTEGER DEFAULT 0,
    grp          INTEGER DEFAULT 0,
    chain_id     INTEGER DEFAULT 0,
    chain_chance INTEGER DEFAULT 0,
    scope        INTEGER DEFAULT 0
);
```

---

## 6. 発火判定フロー (RANDOM型)

DOSゲームの `check_weekly_event()` を汎用化した確率上昇モデル:

```
evt_tick(turn, ctx)  [RANDOM型]
  │
  ├── クールダウン中の全イベントを除外
  │
  ├── g_no_event_counter++
  │
  ├── g_no_event_counter <= min_turn ?
  │   └── YES → スキップ (クールダウン期間)
  │
  ├── rng_next() < g_no_event_counter ?
  │   ├── YES → 候補リスト作成 (weight, min_turn, 排他チェック)
  │   │         ai_weighted_pick() で1つ選出
  │   │         発火 → g_no_event_counter = 0
  │   │         cooldown[selected] = def.cooldown
  │   │         chain_id があれば chain_chance% で連鎖発火
  │   └── NO  → 不発生 (カウンタ継続上昇)
```

---

## 7. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~1.2KB |
| EvtDef[64] | 64 × 14B = 896B |
| EvtActive[16] | 64B |
| g_cooldowns[64] | 128B |
| 内部変数 | ~32B |
| **合計 RAM** | **~2.3KB** |

---

## 8. 拡張設計

### 8.1 イベント優先度 (P2)

複数イベントが同時に発火条件を満たした場合の優先順位:

```c
/* EvtDef に追加 */
u8   priority;  /* 優先度 (大きいほど優先) */
```

### 8.2 イベントログ (P2)

発火履歴を記録し、「N回目の発動で効果変化」等に対応:

```c
#define EVT_LOG_MAX  32
typedef struct {
    u16  event_id;
    u16  turn;
} EvtLogEntry;

int evt_get_fire_count(u16 event_id);
```

---

## 9. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32event.h` ヘッダ作成
- [x] `evt_core.c` 実装 (init, shutdown, DBロード)
- [x] `evt_tick.c` 実装 (毎ターン更新, 確率発火)
- [x] `evt_active.c` 実装 (アクティブ管理)
- [x] `evt_trigger.c` 実装 (手動発火, 連鎖)
- [x] Makefile 統合
- [x] テストプログラム `evt_test.c` 作成
- [x] `events.db` サンプルデータ作成

### Phase 2: 拡張

- [ ] イベント優先度
- [ ] イベントログ
- [ ] 複合条件 (AND/OR)

---

*この設計書は libos32event の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
