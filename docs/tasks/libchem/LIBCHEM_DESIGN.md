# libos32chem — 化学エンジンライブラリ設計書

## 1. 概要

BotW型「化学エンジン（Chemistry Engine）」の OS32 実装。
オブジェクトに属性タグと物理パラメータを持たせ、
**SQLiteルール辞書**と**C89整数演算**の組み合わせで
「創発的ゲームプレイ」を実現する。

### 設計思想

- **ルール定義はSQL**: ゲームデザイナーが C を書かずにルールを追加・変更可能
- **ランタイムはC + libos32math**: 毎フレームの計算はSQL不使用、純粋整数演算
- **起動時キャッシュ**: 頻繁に参照するルールは起動時にRAMにロード
- **KernelAPI非依存の分離**: libos32db のみに依存し、カーネル層は触らない

### 依存関係

```
libos32math  (依存なし)
     ↑
libos32chem  (math + db)
     ↑              ↑
libos32db         libos32gfx
(KAPI経由)        (描画は使わない)
```

`libos32chem` は **描画 (gfx) には依存しない**。
オブジェクトの物理シミュレーションとルール検索のみを担当し、
描画はゲーム側が担当する。

---

## 2. コアデータ構造

### 2.1 属性 (Element) — ビットフラグ方式

```c
/* 属性ビットフラグ (u32: 最大32種) */
#define ELEM_NONE       0x00000000
#define ELEM_FIRE       0x00000001
#define ELEM_WATER      0x00000002
#define ELEM_WOOD       0x00000004
#define ELEM_ICE        0x00000008
#define ELEM_ELECTRIC   0x00000010
#define ELEM_STEAM      0x00000020
#define ELEM_GRASS      0x00000040
#define ELEM_METAL      0x00000080
#define ELEM_STONE      0x00000100
#define ELEM_WIND       0x00000200
/* 以下ゲームごとに拡張可能 */
```

### 2.2 ゲームオブジェクト (ChemObject)

```c
#define CHEM_MAX_OBJECTS  128     /* 同時管理オブジェクト上限 */

typedef struct {
    u16  id;                /* ユニークID (0=未使用) */
    u16  type_id;           /* マスターデータ上の型ID */
    i16  x, y;              /* タイル座標 */
    u32  elements;          /* 現在の属性フラグ (複数持ち可) */
    i16  temperature;       /* 温度 (整数, 単位は任意) */
    i16  hp;                /* 耐久力 (0で消滅) */
    u8   state;             /* 状態 (CHEM_STATE_*) */
    u8   flags;             /* 各種フラグ (燃焼中/凍結中/帯電中等) */
    u8   timer;             /* 汎用タイマー (フレームカウント) */
    u8   _pad;
} ChemObject;

/* 状態定数 */
#define CHEM_STATE_IDLE      0
#define CHEM_STATE_BURNING   1
#define CHEM_STATE_FROZEN    2
#define CHEM_STATE_WET       3
#define CHEM_STATE_CHARGED   4
#define CHEM_STATE_DISSOLVING 5
```

### 2.3 反応ルール (Reaction) — RAM キャッシュ構造体

```c
#define CHEM_MAX_REACTIONS  64    /* キャッシュ上限 */

typedef struct {
    u32  elem_a;        /* 接触元の属性マスク */
    u32  elem_b;        /* 接触先の属性マスク */
    u8   action;        /* CHEM_ACT_* */
    u8   target;        /* CHEM_TGT_* */
    u16  spawn_elem;    /* 新規生成される属性 */
    i16  temp_delta;    /* 温度変化量 */
    i16  hp_delta;      /* HP変化量 */
    u8   priority;      /* 優先度 (大きいほど優先) */
    u8   _pad;
} ChemReaction;

/* アクション種別 */
#define CHEM_ACT_NONE        0
#define CHEM_ACT_IGNITE      1   /* 着火 */
#define CHEM_ACT_EXTINGUISH  2   /* 消火 */
#define CHEM_ACT_FREEZE      3   /* 凍結 */
#define CHEM_ACT_MELT        4   /* 融解 */
#define CHEM_ACT_EVAPORATE   5   /* 蒸発 */
#define CHEM_ACT_ELECTRIFY   6   /* 帯電 */
#define CHEM_ACT_SPREAD      7   /* 周囲に伝播 */
#define CHEM_ACT_DAMAGE      8   /* ダメージ */
#define CHEM_ACT_SPAWN       9   /* オブジェクト生成 */
#define CHEM_ACT_DESTROY     10  /* 消滅 */

/* ターゲット */
#define CHEM_TGT_A      0   /* elem_a側 */
#define CHEM_TGT_B      1   /* elem_b側 */
#define CHEM_TGT_BOTH   2   /* 両方 */
#define CHEM_TGT_AREA   3   /* 周囲全体 */
```

### 2.4 状態遷移ルール (PhaseRule) — 温度ベース

```c
#define CHEM_MAX_PHASES  32

typedef struct {
    u32  elem_from;     /* 元の属性 */
    i16  temp_min;      /* この温度範囲内で遷移発生 */
    i16  temp_max;
    u32  elem_to;       /* 変化先の属性 */
    u16  spawn_elem;    /* 副産物 */
    u16  _pad;
} ChemPhaseRule;
```

---

## 3. SQLテーブル設計

ファイル: `/db/chem.db` (マスターデータ、読み取り専用)

```sql
-- 属性マスター
CREATE TABLE elements (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    flag  INTEGER NOT NULL UNIQUE
);

-- 相互作用ルール
CREATE TABLE reactions (
    id          INTEGER PRIMARY KEY,
    elem_a      INTEGER NOT NULL,   -- ビットマスク
    elem_b      INTEGER NOT NULL,   -- ビットマスク
    action      INTEGER NOT NULL,   -- CHEM_ACT_*
    target      INTEGER NOT NULL,   -- CHEM_TGT_*
    spawn_elem  INTEGER DEFAULT 0,
    temp_delta  INTEGER DEFAULT 0,
    hp_delta    INTEGER DEFAULT 0,
    priority    INTEGER DEFAULT 5
);

-- 温度ベース状態遷移
CREATE TABLE phase_transitions (
    id          INTEGER PRIMARY KEY,
    elem_from   INTEGER NOT NULL,
    temp_min    INTEGER NOT NULL,
    temp_max    INTEGER NOT NULL,
    elem_to     INTEGER NOT NULL,
    spawn_elem  INTEGER DEFAULT 0
);

-- オブジェクト型テンプレート (ゲーム固有)
CREATE TABLE object_types (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    elements    INTEGER NOT NULL,   -- 初期属性フラグ
    temperature INTEGER DEFAULT 20, -- 初期温度
    hp          INTEGER DEFAULT 100,
    flammable   INTEGER DEFAULT 0   -- 可燃性 (0/1)
);
```

---

## 4. API設計

### 4.1 システム管理

```c
/* 初期化: DBを開いてルールをRAMにキャッシュ */
int  chem_init(const char *db_path);

/* 終了: DB接続クローズ、メモリ解放 */
void chem_shutdown(void);

/* ワールドリセット (全オブジェクト消去) */
void chem_reset(void);
```

### 4.2 オブジェクト管理

```c
/* オブジェクト生成 (type_id は object_types テーブルのID) */
int  chem_spawn(u16 type_id, i16 x, i16 y);

/* オブジェクト消滅 */
void chem_destroy(int obj_id);

/* IDでオブジェクト取得 (読み取り用) */
const ChemObject *chem_get(int obj_id);

/* 属性の追加・除去 */
void chem_add_element(int obj_id, u32 elem);
void chem_remove_element(int obj_id, u32 elem);

/* 温度操作 */
void chem_set_temperature(int obj_id, i16 temp);
void chem_add_temperature(int obj_id, i16 delta);

/* 範囲内オブジェクト検索 */
int  chem_find_nearby(i16 x, i16 y, int radius,
                      int *out_ids, int max_count);
```

### 4.3 シミュレーション

```c
/* メインループで毎フレーム呼ぶ — 温度更新 + 状態遷移 */
void chem_update(void);

/* 2オブジェクト間の反応をチェック・適用 */
/* (衝突検出はゲーム側が行い、接触ペアをこの関数に渡す) */
int  chem_react(int obj_a, int obj_b);

/* 指定座標に属性効果を与える (魔法攻撃、爆発等) */
int  chem_apply_area(i16 x, i16 y, int radius,
                     u32 elements, i16 temp_delta);
```

### 4.4 クエリ (ゲーム側のUI用)

```c
/* オブジェクトが特定属性を持つか */
int  chem_has_element(int obj_id, u32 elem);

/* 現在燃焼中のオブジェクト数 */
int  chem_count_burning(void);

/* コールバック: 反応発生時の通知 (ゲーム側がSE/エフェクト用に使う) */
typedef void (*chem_reaction_callback)(int obj_a, int obj_b,
                                       u8 action, u32 spawn_elem);
void chem_set_callback(chem_reaction_callback cb);
```

---

## 5. 処理フロー

### 5.1 初期化フロー

```
chem_init("/db/chem.db")
  ├── db_open("/db/chem.db")
  ├── SELECT * FROM reactions ORDER BY priority DESC
  │   └── ChemReaction[] にキャッシュ (最大64件)
  ├── SELECT * FROM phase_transitions
  │   └── ChemPhaseRule[] にキャッシュ (最大32件)
  └── 内部配列初期化 (ChemObject[128] ゼロクリア)
```

### 5.2 毎フレーム更新 (`chem_update`)

```
chem_update()  ← 毎フレーム呼び出し (ホットパス, SQL不使用)
  │
  ├── for each active object:
  │   ├── 燃焼中 → temperature += 燃焼速度
  │   ├── timer > 0 → timer--
  │   ├── hp <= 0 → 消滅処理
  │   │
  │   └── 温度ベース状態遷移チェック (RAMキャッシュ参照):
  │       ├── 水 + temp >= 100 → 蒸気に変化
  │       ├── 水 + temp <= 0   → 氷に変化
  │       └── 氷 + temp > 0    → 水に変化
  │
  └── (描画はゲーム側の責任)
```

### 5.3 衝突時の反応処理 (`chem_react`)

```
chem_react(obj_a, obj_b)  ← ゲーム側が衝突検出後に呼ぶ
  │
  ├── a->elements と b->elements でキャッシュ検索
  │   (O(n) でルール配列を走査, n=最大64)
  │
  ├── マッチしたルールを優先度順に適用:
  │   ├── IGNITE → target の state = BURNING
  │   ├── EXTINGUISH → state = IDLE, 燃焼停止
  │   ├── FREEZE → state = FROZEN, 属性変更
  │   ├── SPAWN → chem_spawn() で新オブジェクト生成
  │   ├── SPREAD → chem_find_nearby() + chem_react() 再帰
  │   └── DAMAGE → hp -= delta
  │
  └── callback 発火 (ゲーム側にSE/エフェクトを通知)
```

---

## 6. ディレクトリ構造

```
programs/libos32chem/
  ├── libos32chem.h       公開ヘッダ (型定義, 定数, 全API)
  ├── chem_core.c         初期化, DBキャッシュ, オブジェクト配列管理
  ├── chem_react.c        反応処理, ルール検索, アクション適用
  ├── chem_update.c       毎フレーム更新 (温度, 状態遷移, タイマー)
  └── chem_query.c        ゲーム向けクエリ (nearby検索等)

tools/
  └── chem_db_init.py     マスターデータ生成 (→ /db/chem.db)

programs/tests/
  └── chem_test.c         テストプログラム
```

---

## 7. 使用リソース見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~2KB |
| ChemObject[128] | 2,048B |
| ChemReaction[64] | 1,024B |
| ChemPhaseRule[32] | 384B |
| **合計 RAM** | **~5.5KB** |
| chem.db (ディスク) | ~4KB |

PC-98 の限られたメモリ環境でも十分に収まるサイズ。

---

## 8. 実装フェーズ

### Phase 1: コアエンジン

- [ ] `libos32chem.h` ヘッダ作成 (型定義, 定数, 全API宣言)
- [ ] `chem_core.c` 実装 (init/shutdown, DBキャッシュ, オブジェクト配列)
- [ ] `chem_react.c` 実装 (反応ルール検索, アクション適用)
- [ ] `chem_update.c` 実装 (温度更新, 状態遷移)
- [ ] Makefile 統合 (`LIBCHEM_OBJ`, リンク順序)
- [ ] ビルド確認

### Phase 2: マスターデータ + テスト

- [ ] `tools/chem_db_init.py` 実装 (サンプルルール投入)
- [ ] `chem_test.c` 実装 (ルールキャッシュ, 反応テスト, 温度遷移テスト)
- [ ] deploy.yaml にDB/テスト登録
- [ ] NP21/W実機テスト

### Phase 3: 高度な機能

- [ ] `chem_query.c` 実装 (nearby検索, コールバック)
- [ ] `chem_apply_area()` 実装 (範囲攻撃/爆発)
- [ ] 伝播制御 (SPREAD アクションの再帰深度制限)
- [ ] デモプログラム (ビジュアル付き化学エンジンデモ)

### Phase 4: ゲーム統合

- [ ] libpyxel / libtilemap との連携設計
- [ ] ゲームプロトタイプ (フィールド上で火/水/氷の相互作用)
- [ ] パフォーマンス計測・最適化

---

## 9. 設計上の判断ポイント

### Q: ルール検索は毎回SQLか、RAMキャッシュか？

**RAMキャッシュを採用**。理由:
- ルール数は数十件程度 (64件上限)
- 毎フレームの衝突判定は高頻度で発生
- 線形探索 O(64) は i386 16MHz でも十分高速
- SQLはルールの「定義と編集」に使い、ランタイムでは使わない

### Q: オブジェクト上限 128 は少なすぎないか？

PC-98 (640x400, 16色) の画面に同時に表示・管理できるゲームオブジェクトは
実用的に100前後が上限。描画コスト・メモリ制約を考慮すると128は妥当。
ゲームの規模に応じて `CHEM_MAX_OBJECTS` を調整可能。

### Q: コールバックの設計意図は？

化学エンジンは**物理シミュレーションのみ**を担当する。
「火がついたときの炎エフェクト」「水が蒸発したときのSE」は
ゲーム側がコールバックで受け取って自前で処理する。
これにより libos32chem は gfx/snd に一切依存しない。

---

## 10. 関連ドキュメント

| ドキュメント | 内容 |
|-------------|------|
| [LIBMATH_DESIGN.md](../libmath/LIBMATH_DESIGN.md) | libos32math 設計書 (依存先) |
| [KAPI_SPEC.md](../../KAPI_SPEC.md) | KernelAPI 仕様書 (DB接続) |
| [05_drivers.md](../../05_drivers.md) | デバイスドライバ・ライブラリ仕様 |
