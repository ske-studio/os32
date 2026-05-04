# libos32econ estate拡張 — 不動産サブシステム設計書

*策定: 2026-04-28*

> この文書は、既存の `libos32econ` ライブラリに不動産(estate)サブシステムを
> 追加する拡張設計を定義する。村統治・投資・上納金蓄積・資産計算を汎用化する。

---

## 1. 設計背景

### 1.1 なぜ estate サブシステムが必要か

DOSゲームの `village.c` は「村レベル×基本価値で収入蓄積」という単純モデルだが、
これは `libos32econ` の「市場と商品取引」とは別次元の経済概念。

既存の `econ_buy()`/`econ_sell()` は商品の売買を扱うが、
「不動産を取得→毎ターン収入蓄積→投資でレベルアップ→資産価値上昇」
という統治モデルは新しいサブシステムとして追加する。

### 1.2 既存APIとの関係

```
libos32econ (既存)
  ├── econ_core.c       初期化, 終了 (既存)
  ├── econ_market.c     市場・商品取引 (既存)
  ├── econ_trade.c      交易・輸送 (既存)
  └── econ_estate.c     ★ 新規追加: 不動産サブシステム
```

---

## 2. コアデータ構造

```c
#define ECON_ESTATE_MAX      32   /* 同時管理不動産上限 */
#define ECON_ESTATE_LV_MAX    8   /* 最大レベル */
#define ECON_OWNER_NONE    0xFF

/* 不動産種別 */
#define ESTATE_TYPE_VILLAGE  0   /* 村 */
#define ESTATE_TYPE_PORT     1   /* 港 */
#define ESTATE_TYPE_FORT     2   /* 砦 */
#define ESTATE_TYPE_MINE     3   /* 鉱山 */

typedef struct {
    u16  id;
    u8   owner;           /* オーナーID (0xFF=無主) */
    u8   level;           /* 1〜ECON_ESTATE_LV_MAX */
    u8   type;            /* ESTATE_TYPE_* */
    u8   flags;           /* 封鎖/略奪中/モンスター支配 */
    u16  _pad;
    u32  base_value;      /* 基本価値 */
    u32  value;           /* 現在価値 (base_value × レベル倍率) */
    u32  tax;             /* 蓄積上納金 */
    u8   monster_id;      /* 支配モンスターID (0=なし) */
    u8   stage;           /* 所属ステージ */
    u16  _pad2;
} EconEstate;

/* レベルテーブル (RAMキャッシュ) */
typedef struct {
    u8   level;
    u8   income_mul;      /* 収入倍率% (100=等倍) */
    u8   value_mul;       /* 価値倍率% */
    u8   _pad;
    u32  invest_cost_div; /* 投資費用の除数 */
} EconEstateLevelDef;
```

---

## 3. API設計

### 3.1 初期化

```c
/* econ_init() の内部で自動呼出し、または明示的に呼ぶ */
int  econ_estate_init(void);
```

### 3.2 統治操作

```c
/* 不動産を取得 (owner を設定) */
int  econ_estate_claim(u16 id, u8 owner);

/* 不動産を放棄 (owner を NONE に戻す) */
void econ_estate_release(u16 id);

/* 不動産のモンスター支配を設定 */
void econ_estate_set_monster(u16 id, u8 monster_id);
void econ_estate_clear_monster(u16 id);
```

### 3.3 投資

```c
/* レベルアップ投資
 * wallet: 所持金ポインタ (成功時に減算される)
 * 戻り値: 0=成功, -1=最大レベル, -2=資金不足
 */
int  econ_estate_invest(u16 id, u32 *wallet);

/* レベルダウン (略奪、デビルマン攻撃等) */
void econ_estate_damage(u16 id);

/* 投資費用を取得 */
u32  econ_estate_invest_cost(u16 id);
```

### 3.4 収入管理

```c
/* 全不動産の毎ターン収入蓄積 (ゲームのターン開始時に呼ぶ) */
void econ_estate_accumulate(void);

/* 指定オーナーの全蓄積上納金を回収
 * 戻り値: 回収した合計金額
 */
u32  econ_estate_collect(u8 owner);

/* 指定1件の上納金を回収 */
u32  econ_estate_collect_one(u16 id);

/* 指定不動産の毎ターン収入額を取得 */
u32  econ_estate_income(u16 id);
```

### 3.5 資産計算

```c
/* 指定オーナーの全不動産価値合計 */
u32  econ_estate_total_value(u8 owner);

/* 指定オーナーの不動産数 */
int  econ_estate_count(u8 owner);

/* 指定オーナーの不動産IDリスト */
int  econ_estate_list(u8 owner, u16 *out, int max);
```

### 3.6 クエリ

```c
/* 不動産情報取得 */
const EconEstate *econ_estate_get(u16 id);

/* 不動産総数 */
int  econ_estate_total(void);

/* レベル取得 */
u8   econ_estate_level(u16 id);

/* 価値取得 */
u32  econ_estate_value(u16 id);

/* フラグ操作 */
void econ_estate_set_flag(u16 id, u8 flag);
void econ_estate_clear_flag(u16 id, u8 flag);
int  econ_estate_has_flag(u16 id, u8 flag);
```

---

## 4. DBスキーマ (econ.db に追加)

```sql
/* 不動産マスター */
CREATE TABLE estates (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    type        INTEGER NOT NULL DEFAULT 0,
    stage       INTEGER NOT NULL DEFAULT 1,
    base_value  INTEGER NOT NULL
);

/* レベル別パラメータ */
CREATE TABLE estate_levels (
    level       INTEGER PRIMARY KEY,
    income_mul  INTEGER NOT NULL DEFAULT 100,
    value_mul   INTEGER NOT NULL DEFAULT 100,
    invest_div  INTEGER NOT NULL DEFAULT 256
);
```

---

## 5. 収入計算フロー

```
econ_estate_accumulate()
  │
  for each estate (id = 0 .. count-1):
  │   ├── owner == NONE → skip
  │   ├── monster_id != 0 → skip (モンスター支配中は収入なし)
  │   ├── flags に BLOCKED → skip
  │   │
  │   ├── income = base_value / 640 * income_mul[level] / 100
  │   └── estate.tax += income
```

`base_value / 640` はDOSゲームの `calc_income()` と同じ計算式を維持。
640はPC-98の横解像度に由来する歴史的定数だが、互換性のため保持する。

---

## 6. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~800B |
| EconEstate[32] | 32 × 24B = 768B |
| EconEstateLevelDef[8] | 64B |
| 内部変数 | ~16B |
| **追加 RAM** | **~1.6KB** |

---

## 7. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32econ.h` にestate構造体・API宣言を追加
- [x] `econ_estate.c` 実装
- [x] `econ.db` にestatesテーブル追加
- [x] `econ_test.c` にestate テストケース追加
- [x] Makefile にオブジェクト追加 (wildcardで自動認識)

### Phase 2: 拡張

- [x] 不動産種別ごとの特殊収入 (港: 交易ボーナス等)
  - `EconEstateTypeDef` 構造体 + `estate_types` DBテーブル
  - 村: ボーナスなし、港: 基本10%+交易ルート1本ごと+10%、砦: 固定+20%、鉱山: 固定+50%
  - `econ_estate_type_bonus()` / `econ_estate_type_def()` API
- [x] 複数オーナー共同統治
  - `EconEstate` に `co_owner` + `share_pct` フィールド追加
  - `econ_estate_set_co_owner()` / `clear_co_owner()` / `collect_co()` API
  - `collect` / `collect_one` が `share_pct` に基づく分配を実行

---

*この設計書は libos32econ estate拡張の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
