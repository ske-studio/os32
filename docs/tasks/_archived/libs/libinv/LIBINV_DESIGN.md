# libos32inv — インベントリ・装備・ショップエンジン設計書

*策定: 2026-04-28*

> この文書は、アイテム所持・装備スロット・ショップ売買・抽選を
> 汎用管理するライブラリ `libos32inv` の設計思想・API仕様・実装計画を定義する。

---

## 1. 設計背景

### 1.1 なぜ libos32inv が必要か

1. **装備/アイテム管理の重複** — ゲームごとにインベントリ操作を再実装している
2. **装備ボーナス計算の散在** — 実効ATK/DEFの計算がゲームロジックに埋没
3. **ショップ品揃えのハードコード** — ステージ別品揃えが `switch` 文で固定
4. **抽選テーブルの不在** — 宝箱・ドロップの確率計算が場当たり的

### 1.2 設計思想

- **マスターデータはDB駆動**: 武器/盾/鎧/アイテム定義をSQLiteからロード
- **ランタイムはRAM配列**: `InvBag` 構造体をゲーム側が管理し、APIに渡す
- **装備制限はポリシーコールバック**: レベル/職業制限の判定はゲーム側が注入
- **アイテム効果の実行はゲーム側**: ライブラリは効果ID/パラメータを返すだけ

---

## 2. アーキテクチャ

### 2.1 依存関係

```
libos32db   (SQLiteアクセス)
     ^
libos32inv  (db)
     ^
     ├── libos32battle  (装備ボーナスをステータスに反映)
     └── ゲーム本体     (ショップUI・アイテム使用)
```

### 2.2 ディレクトリ構成

```
programs/libos32inv/
    libos32inv.h          公開ヘッダ
    inv_core.c            初期化, 終了, DBロード, マスターキャッシュ
    inv_bag.c             インベントリ操作 (add/remove/count)
    inv_equip.c           装備操作・ボーナス計算
    inv_shop.c            ショップ品揃え・売買
    inv_lottery.c          抽選 (宝箱/ドロップ)
```

---

## 3. コアデータ構造

### 3.1 インベントリスロット

```c
typedef struct {
    u16  item_id;         /* 0=空き */
    u8   count;           /* スタック数 (1=非スタック, 最大99) */
    u8   durability;      /* 耐久度 (0xFF=無限, 0=破損) */
} InvSlot;
```

### 3.2 インベントリバッグ (ゲーム側が管理)

```c
#define INV_MAX_SLOTS    16  /* 所持枠物理上限 */
#define INV_EQUIP_SLOTS   8  /* 装備スロット種別上限 */

/* 装備スロット種別 */
#define INV_ESLOT_WEAPON    0
#define INV_ESLOT_SHIELD    1
#define INV_ESLOT_ARMOR     2
#define INV_ESLOT_ACCESSORY 3
#define INV_ESLOT_HEAD      4
#define INV_ESLOT_BOOTS     5
#define INV_ESLOT_GLOVES    6
#define INV_ESLOT_RING      7

typedef struct {
    InvSlot slots[INV_MAX_SLOTS];
    InvSlot equip[INV_EQUIP_SLOTS];
    u8      max_slots;    /* 実際の所持上限 (ゲームが設定) */
    u8      equip_count;  /* 使用する装備スロット数 */
    u16     _pad;
} InvBag;
```

### 3.3 マスターデータ (RAMキャッシュ)

```c
#define INV_MASTER_MAX  128  /* アイテムマスター上限 */

typedef struct {
    u16  id;
    u8   type;            /* 0=消耗品, 1=武器, 2=盾, 3=鎧, 4=アクセサリ等 */
    u8   effect;          /* 効果ID (ゲーム側で解釈) */
    u8   param;           /* 効果パラメータ */
    u8   rarity;          /* レアリティ (抽選の重み計算用) */
    u8   equip_slot;      /* 装備先スロット (INV_ESLOT_*) */
    u8   stackable;       /* 1=スタック可能 */
    i16  stat_bonus;      /* ステータスボーナス値 */
    u8   stat_type;       /* ボーナス対象 (ATK/DEF/SPD/MAG) */
    u8   stage;           /* 登場ステージ (ショップ品揃え用) */
    u32  price;           /* 購入価格 (売値=price/2) */
    char name[24];        /* アイテム名 (UTF-8) */
} InvItemDef;
```

---

## 4. API設計

### 4.1 システム管理 (inv_core.c)

```c
int  inv_init(const char *db_path);
void inv_shutdown(void);

/* マスターデータ参照 */
const InvItemDef *inv_get_def(u16 item_id);
int  inv_master_count(void);
```

### 4.2 インベントリ操作 (inv_bag.c)

```c
/* バッグ初期化 */
void inv_bag_init(InvBag *bag, u8 max_slots, u8 equip_count);

/* 追加 (スタック可能アイテムは自動合算) */
int  inv_add(InvBag *bag, u16 item_id, u8 count);  /* 0=成功, -1=満杯 */

/* 指定スロットから除去 */
int  inv_remove(InvBag *bag, u8 slot, u8 count);

/* 所持数 (同一アイテムの合計) */
int  inv_count_item(const InvBag *bag, u16 item_id);

/* 空きスロット数 */
int  inv_free_slots(const InvBag *bag);

/* 所持判定 */
int  inv_has(const InvBag *bag, u16 item_id);

/* スロット内容取得 */
const InvSlot *inv_get_slot(const InvBag *bag, u8 slot);
```

### 4.3 装備操作 (inv_equip.c)

```c
/* 装備変更
 * 旧装備は自動的にインベントリに戻す (満杯なら失敗)
 * 戻り値: 0=成功, -1=装備不可, -2=インベントリ満杯
 */
int  inv_equip(InvBag *bag, u16 equip_id, u8 slot_type);

/* 装備解除 */
int  inv_unequip(InvBag *bag, u8 slot_type);

/* 装備ボーナス合計 (全装備スロットの stat_bonus を合算) */
i16  inv_equip_bonus(const InvBag *bag, u8 stat_type);

/* 装備可否チェック (ポリシーコールバック) */
typedef int (*inv_equip_check_fn)(u16 equip_id, u8 class_id,
                                   u8 level);
void inv_set_equip_policy(inv_equip_check_fn fn);
```

### 4.4 ショップ (inv_shop.c)

```c
/* ステージ別品揃え取得
 * shop_type: 0=装備屋, 1=道具屋, 2=言霊屋
 * stage: 現在ステージ
 * 戻り値: 取得した品数
 */
int  inv_shop_list(u8 shop_type, u8 stage,
                    u16 *out_ids, int max);

/* 価格取得 */
u32  inv_get_price(u16 item_id);
u32  inv_get_sell_price(u16 item_id);   /* 売値 = price / 2 */

/* 購入 (wallet から減算, バッグに追加) */
int  inv_shop_buy(InvBag *bag, u16 item_id, u32 *wallet);

/* 売却 (バッグから除去, wallet に加算) */
int  inv_shop_sell(InvBag *bag, u8 slot, u32 *wallet);
```

### 4.5 抽選 (inv_lottery.c)

```c
/* 抽選テーブルからランダムにアイテムIDを取得
 * table_type: 0=宝箱, 1=敵ドロップ, 2=イベント報酬
 * stage: 現在ステージ (対象外のアイテムを除外)
 */
u16  inv_lottery(u8 table_type, u8 stage);
```

### 4.6 アイテム効果クエリ

```c
/* アイテムの効果ID/パラメータを取得 (ゲーム側でswitch分岐) */
u8   inv_get_effect(u16 item_id);
u8   inv_get_param(u16 item_id);
int  inv_is_consumable(u16 item_id);   /* 消耗品か */
int  inv_is_equipment(u16 item_id);    /* 装備品か */
```

---

## 5. DBスキーマ (items.db)

```sql
CREATE TABLE items (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    type        INTEGER NOT NULL DEFAULT 0,
    effect      INTEGER DEFAULT 0,
    param       INTEGER DEFAULT 0,
    rarity      INTEGER DEFAULT 1,
    equip_slot  INTEGER DEFAULT 0,
    stackable   INTEGER DEFAULT 0,
    stat_bonus  INTEGER DEFAULT 0,
    stat_type   INTEGER DEFAULT 0,
    stage       INTEGER DEFAULT 1,
    price       INTEGER DEFAULT 0
);

CREATE TABLE shop_lineup (
    shop_type   INTEGER NOT NULL,
    stage       INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    PRIMARY KEY (shop_type, stage, item_id)
);

CREATE TABLE lottery_tables (
    table_type  INTEGER NOT NULL,
    item_id     INTEGER NOT NULL,
    weight      INTEGER NOT NULL DEFAULT 1,
    min_stage   INTEGER DEFAULT 1,
    PRIMARY KEY (table_type, item_id)
);
```

---

## 6. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~1.5KB |
| InvItemDef[128] | 128 × 40B = 5KB |
| 内部変数 | ~32B |
| **合計 RAM (ライブラリ自身)** | **~6.5KB** |
| InvBag (呼出側が管理) | ~140B/バッグ |

---

## 7. 拡張設計

### 7.1 合成/クラフト (P2)

```c
/* items.db に追加: */
/* CREATE TABLE recipes (
       id INTEGER PRIMARY KEY,
       result_id INTEGER, mat_a INTEGER, mat_b INTEGER,
       mat_a_count INTEGER DEFAULT 1, mat_b_count INTEGER DEFAULT 1
   ); */

int inv_can_craft(const InvBag *bag, u16 recipe_id);
int inv_craft(InvBag *bag, u16 recipe_id);
```

### 7.2 セット装備ボーナス (P2)

```c
/* CREATE TABLE set_bonus (
       set_id INTEGER, piece_count INTEGER,
       stat_type INTEGER, bonus INTEGER
   ); */

i16 inv_set_bonus(const InvBag *bag, u8 stat_type);
```

---

## 8. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32inv.h` ヘッダ作成
- [x] `inv_core.c` 実装 (init, shutdown, マスターロード)
- [x] `inv_bag.c` 実装 (add, remove, count, has)
- [x] `inv_equip.c` 実装 (equip, unequip, bonus)
- [x] `inv_shop.c` 実装 (shop_list, buy, sell)
- [x] `inv_lottery.c` 実装 (lottery)
- [x] Makefile 統合
- [x] テストプログラム `inv_test.c` 作成
- [x] `items.db` サンプルデータ作成

### Phase 2: 拡張

- [x] 合成/クラフト
- [x] セット装備ボーナス
- [x] 耐久度消耗 (使用ごとに -1)

---

*この設計書は libos32inv の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
