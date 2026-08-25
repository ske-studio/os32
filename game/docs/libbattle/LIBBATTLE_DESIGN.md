# libos32battle — ターンバトル解決エンジン設計書

*策定: 2026-04-28*

> この文書は、ターンベースバトルの計算処理（ダメージ計算・コマンド解決・
> 状態異常・バフ/デバフ・属性相性）を提供する汎用ライブラリ
> `libos32battle` の設計思想・API仕様・実装計画を定義する。

---

## 1. 設計背景

### 1.1 なぜ libos32battle が必要か

1. **ダメージ計算の散在** — 各ゲームで独自の計算式をハードコード
2. **コマンドマトリクスの硬直** — `resolve_commands()` が固定配列で差し替え不可
3. **バフ/デバフの不在** — 一時的なステータス修飾を管理する仕組みがない
4. **属性相性の不在** — 火→氷で2倍等の相性計算が組み込まれていない

### 1.2 設計思想

- **純粋計算ユニット**: ステータス値+コマンドを入力し、結果を返すだけ
- **UI/演出はゲーム側**: バトルアニメーション・メッセージ表示はコールバックで通知
- **ダメージ計算はポリシー差し替え**: 関数ポインタで計算式を外部から注入可能
- **コマンドマトリクスはDB駆動**: NxMの結果テーブルをSQLiteから読み込み
- **属性相性はDB駆動**: 属性ペアごとの倍率をテーブルで定義

---

## 2. アーキテクチャ

### 2.1 依存関係

```
libos32db   (SQLiteアクセス)
     ^
libos32math (乱数, fix16_t)
     ^
libos32ai   (AI判断 — バトルコマンド選択を委ねる)
     ^
libos32battle (db + math + ai)
     ^
     └── ゲーム本体 (バトルフロー制御・演出)
```

### 2.2 ディレクトリ構成

```
programs/libos32battle/
    libos32battle.h       公開ヘッダ
    btl_core.c            初期化, 終了, DBロード
    btl_calc.c            ダメージ計算, 回避, 逃走
    btl_resolve.c         コマンドマトリクス解決, ターン解決
    btl_status.c          状態異常・バフ/デバフ管理
    btl_element.c         属性相性計算
```

---

## 3. コアデータ構造

### 3.1 バトルユニット

```c
typedef struct {
    i16  hp, max_hp;
    i16  atk, def, spd, mag;    /* i16: 負値(デバフ)・255超に対応 */
    u32  status;                /* 状態異常ビットフラグ (最大32種) */
    u32  elements;              /* 属性フラグ (libos32chemと同形式) */
    u8   tame_count;            /* ためるカウンタ */
    u8   class_id;              /* 職業/種族 (固有技分岐用) */
    u8   modifier_count;        /* アクティブ修飾子数 */
    u8   _pad;
} BtlUnit;
```

### 3.2 バフ/デバフ修飾子

```c
#define BTL_MOD_MAX  8  /* 1ユニットあたりの修飾子上限 */

/* 修飾対象ステータス */
#define BTL_STAT_ATK  0
#define BTL_STAT_DEF  1
#define BTL_STAT_SPD  2
#define BTL_STAT_MAG  3

typedef struct {
    u8   stat;        /* BTL_STAT_* */
    u8   turns;       /* 残りターン数 (0=永続) */
    i16  add_value;   /* 加算値 */
    i16  mul_pct;     /* 乗算% (100=等倍, 150=1.5倍, 50=半減) */
    u16  _pad;
} BtlModifier;
```

### 3.3 バトル結果

```c
/* 結果タイプ定数 */
#define BTL_RES_NORMAL   0
#define BTL_RES_GUARD    1
#define BTL_RES_NOGUARD  2
#define BTL_RES_COUNTER  3
#define BTL_RES_MISS     4
#define BTL_RES_REFLECT  5
#define BTL_RES_VULN     6
#define BTL_RES_YIELD    7

typedef struct {
    i16  damage;          /* ダメージ量 (負=回復) */
    u8   result_type;     /* BTL_RES_* */
    u8   is_critical;     /* クリティカルフラグ */
    u8   is_dodge;        /* 回避フラグ */
    u8   _pad;
    u16  status_apply;    /* 付与された状態異常ビット */
    u16  status_clear;    /* 解除された状態異常ビット */
    i16  element_mul;     /* 属性倍率 (256=等倍, 512=2倍) */
} BtlResult;
```

### 3.4 コマンドマトリクス (RAMキャッシュ)

```c
#define BTL_CMD_MAX  8   /* 攻撃/防御コマンド最大数 */

typedef struct {
    u8   atk_count;                         /* 攻撃コマンド種数 */
    u8   def_count;                         /* 防御コマンド種数 */
    u8   matrix[BTL_CMD_MAX][BTL_CMD_MAX];  /* [atk][def] → BTL_RES_* */
} BtlCommandMatrix;
```

### 3.5 属性相性テーブル (RAMキャッシュ)

```c
#define BTL_ELEM_PAIRS_MAX  32

typedef struct {
    u32  elem_atk;      /* 攻撃属性 */
    u32  elem_def;      /* 防御属性 */
    i16  multiplier;    /* 倍率 (256=等倍, 512=2倍, 128=半減) */
    u16  _pad;
} BtlElementChart;
```

---

## 4. API設計

### 4.1 システム管理 (btl_core.c)

```c
int  btl_init(const char *db_path);
void btl_shutdown(void);
```

### 4.2 ダメージ計算 (btl_calc.c)

```c
/* 物理ダメージ: effective_atk*1.5 - def + 乱数±3 */
int  btl_calc_damage(int atk, int def);

/* 魔法ダメージ: mag*2 - def + 乱数±3 */
int  btl_calc_magic_damage(int mag, int def_mag);

/* 防御時の実効DEF: def * 1.25 */
int  btl_effective_def_guard(int def);

/* 回避率: 0-50% (def_spd > atk_spd 時に発生) */
int  btl_calc_dodge_rate(int atk_spd, int def_spd);

/* 逃走成功率: 20-80% (速度比に基づく線形マッピング) */
int  btl_calc_flee_rate(int runner_spd, int chaser_spd);

/* 属性相性倍率を取得 (256=等倍) */
i16  btl_element_multiplier(u32 atk_elem, u32 def_elem);

/* ポリシー差し替え */
typedef int (*btl_damage_fn)(int atk, int def);
void btl_set_damage_policy(btl_damage_fn fn);
void btl_set_magic_policy(btl_damage_fn fn);
```

### 4.3 コマンド解決 (btl_resolve.c)

```c
/* コマンドマトリクスから結果を引く */
u8   btl_resolve_commands(u8 atk_cmd, u8 def_cmd);

/* 1ターン分の攻防を一括解決 */
BtlResult btl_resolve_turn(const BtlUnit *attacker, u8 atk_cmd,
                            const BtlUnit *defender, u8 def_cmd);

/* 先攻判定 (値が大きい方が先攻, 同値はランダム) */
int  btl_first_strike(int p1_value, int p2_value);
```

### 4.4 状態異常・修飾子 (btl_status.c)

```c
/* 状態異常 */
void btl_apply_status(BtlUnit *unit, u32 status_bit);
void btl_clear_status(BtlUnit *unit, u32 status_bit);
int  btl_has_status(const BtlUnit *unit, u32 status_bit);
int  btl_status_tick(BtlUnit *unit);  /* 毎ターン処理, 戻り値=行動不能 */

/* 修飾子 (バフ/デバフ) */
int  btl_add_modifier(BtlUnit *unit, const BtlModifier *mod);
void btl_tick_modifiers(BtlUnit *unit);  /* ターン経過でデクリメント */
void btl_clear_modifiers(BtlUnit *unit);

/* 実効ステータス取得 (基礎値 + 全修飾子の合算) */
i16  btl_effective_stat(const BtlUnit *unit, u8 stat);

/* コールバック */
typedef void (*btl_result_cb)(const BtlUnit *atk, const BtlUnit *def,
                               const BtlResult *result);
void btl_set_result_callback(btl_result_cb cb);
```

---

## 5. DBスキーマ (battle.db)

```sql
/* コマンドマトリクス */
CREATE TABLE command_matrix (
    atk_cmd     INTEGER NOT NULL,
    def_cmd     INTEGER NOT NULL,
    result_type INTEGER NOT NULL,
    PRIMARY KEY (atk_cmd, def_cmd)
);

/* 状態異常定義 */
CREATE TABLE status_effects (
    id           INTEGER PRIMARY KEY,
    name         TEXT NOT NULL,
    bit_flag     INTEGER NOT NULL,   /* u32のビット位置 (0-31) */
    duration     INTEGER DEFAULT 0,  /* 自然回復ターン (0=永続) */
    tick_damage  INTEGER DEFAULT 0,  /* 毎ターンダメージ */
    prevents_action INTEGER DEFAULT 0 /* 1=行動不能 */
);

/* 属性相性テーブル */
CREATE TABLE element_chart (
    elem_atk    INTEGER NOT NULL,
    elem_def    INTEGER NOT NULL,
    multiplier  INTEGER NOT NULL DEFAULT 256,  /* 256=等倍 */
    PRIMARY KEY (elem_atk, elem_def)
);
```

---

## 6. btl_resolve_turn 処理フロー

```
btl_resolve_turn(attacker, atk_cmd, defender, def_cmd)
  │
  ├── result_type = btl_resolve_commands(atk_cmd, def_cmd)
  │
  ├── [MISS/ためる] → damage=0, return
  ├── [YIELD/逃走] → btl_calc_flee_rate() で判定
  ├── [COUNTER]    → 攻守逆転してダメージ計算
  ├── [REFLECT]    → damage=0, return
  │
  ├── 実効ステータス計算:
  │   ├── eff_atk = btl_effective_stat(attacker, ATK)
  │   ├── eff_def = btl_effective_stat(defender, DEF)
  │   └── [GUARD] → eff_def = btl_effective_def_guard(eff_def)
  │
  ├── ダメージ計算:
  │   ├── damage_policy(eff_atk, eff_def)
  │   └── 属性倍率: damage = damage * element_mul / 256
  │
  ├── 回避判定: btl_calc_dodge_rate()
  │
  └── return BtlResult { damage, result_type, flags... }
```

---

## 7. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~1.5KB |
| BtlCommandMatrix | 66B |
| BtlElementChart[32] | 320B |
| 内部変数 | ~32B |
| **合計 RAM (ライブラリ自身)** | **~420B** |
| BtlUnit (呼出側が管理) | 24B/ユニット |
| BtlModifier[8] (呼出側が管理) | 64B/ユニット |

---

## 8. 拡張設計

### 8.1 パーティバトル (P2)

1対1前提の現設計を複数ユニット対応に拡張する場合:

```c
/* 全体攻撃: 同じコマンドで複数防御者に適用 */
int btl_resolve_multi(const BtlUnit *attacker, u8 atk_cmd,
                       BtlUnit *defenders, u8 *def_cmds,
                       BtlResult *results, int count);
```

### 8.2 変身システム (P2)

```c
typedef struct {
    i16  stat_mul_pct;   /* ステータス倍率% (200=2倍) */
    i16  hp_mul_pct;     /* HP倍率% (400=4倍) */
    u8   max_turns;      /* 最大持続ターン */
    u8   release_rate;   /* 毎ターン解除確率% */
    i16  weapon_atk;     /* 専用装備ATK */
    i16  shield_def;     /* 専用装備DEF */
    i16  armor_def;      /* 専用装備DEF */
    u16  _pad;
} BtlTransformDef;

int  btl_transform(BtlUnit *unit, const BtlTransformDef *def);
int  btl_transform_tick(BtlUnit *unit);  /* 戻り値: 1=解除 */
void btl_transform_release(BtlUnit *unit);
```

---

## 9. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32battle.h` ヘッダ作成
- [x] `btl_core.c` 実装 (init, shutdown, DBロード)
- [x] `btl_calc.c` 実装 (ダメージ計算, 回避, 逃走)
- [x] `btl_resolve.c` 実装 (コマンドマトリクス, ターン解決)
- [x] `btl_status.c` 実装 (状態異常, 修飾子)
- [x] `btl_element.c` 実装 (属性相性)
- [x] Makefile 統合
- [x] テストプログラム `btl_test.c` 作成
- [x] `battle.db` サンプルデータ作成

### Phase 2: 拡張

- [x] パーティバトル (btl_resolve_multi)
- [x] 変身システム (btl_transform)

---

*この設計書は libos32battle の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
