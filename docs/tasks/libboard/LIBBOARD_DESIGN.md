# libos32board — ノードグラフ型ボードゲームエンジン設計書

*策定: 2026-04-28*

> この文書は、スゴロク・ヘックスマップ等のノードグラフ型ボードの状態管理
> （マスデータ・接続・移動・分岐・区画）を提供する汎用ライブラリ
> `libos32board` の設計思想・API仕様・実装計画を定義する。

---

## 1. 設計背景

### 1.1 なぜ libos32board が必要か

1. **libos32mapとの棲み分け** — 既存の `libos32map` はタイルグリッドRPG向け。
   ノードグラフ構造（マスと接続線）は根本的に異なるデータモデルが必要
2. **グラフ走査の汎用化** — 移動シミュレーション・分岐先読み・同マス判定は
   ボードゲーム共通の操作
3. **動的マップ変更** — ゲーム中のマス封鎖・一方通行・コスト変更への対応

### 1.2 設計思想

- **描画は一切しない**: 座標(x,y)は持つが、描画はゲーム側が `libos32gfx` 等で行う
- **グラフはDB駆動**: マスと接続をSQLiteからロードし、RAMキャッシュ
- **有向グラフ対応**: 一方通行マスを表現可能
- **ランタイム変更可能**: フラグ操作でマスの状態を動的に変更

---

## 2. アーキテクチャ

### 2.1 依存関係

```
libos32db   (SQLiteアクセス)
     ^
libos32board (db)
     ^
     ├── libos32ai   (方向選択スコアの入力元としてマス情報を提供)
     └── ゲーム本体  (移動・イベントディスパッチ)
```

### 2.2 ディレクトリ構成

```
programs/libos32board/
    libos32board.h        公開ヘッダ
    board_core.c          初期化, 終了, DBロード
    board_query.c         マス情報取得, 接続取得
    board_move.c          移動シミュレーション, 先読み
    board_area.c          区画(ステージ)管理
```

---

## 3. コアデータ構造

### 3.1 ボードマス

```c
#define BOARD_MAX_CONNECT  8   /* ヘックス6方向+予備2 */
#define BOARD_MAX_MASSES  256  /* 同時管理マス上限 */
#define BOARD_CONNECT_NONE 0xFFFF

/* マスフラグ */
#define BOARD_FLAG_NONE       0x00
#define BOARD_FLAG_BLOCKED    0x01  /* 封鎖 (通行不可) */
#define BOARD_FLAG_ONEWAY     0x02  /* 一方通行 (connect[0]方向のみ) */
#define BOARD_FLAG_HIDDEN     0x04  /* 非表示 (天の岩戸等) */
#define BOARD_FLAG_TRAP       0x08  /* 罠あり */

typedef struct {
    u16  id;
    u8   type;                             /* マス種別 */
    u8   connect_count;                    /* 実際の接続数 */
    u16  connect[BOARD_MAX_CONNECT];       /* 接続先ID */
    u8   area;                             /* 所属区画 */
    u8   flags;                            /* BOARD_FLAG_* */
    u16  param;                            /* マス固有パラメータ */
    u8   cost;                             /* 通行コスト (0=無料) */
    u8   trap_owner;                       /* 罠設置者ID (0xFF=なし) */
    i16  x, y;                             /* 表示座標 */
} BoardMass;
```

### 3.2 区画(ステージ)

```c
#define BOARD_MAX_AREAS  8

typedef struct {
    u8   id;
    u8   unlocked;          /* 0=ロック, 1=解放済み */
    u8   unlock_type;       /* 0=初期解放, 1=ボス撃破, 2=イベント */
    u8   unlock_param;      /* ボスID / イベントID */
} BoardArea;
```

---

## 4. API設計

### 4.1 システム管理 (board_core.c)

```c
int  board_init(const char *db_path);
void board_shutdown(void);
void board_reset(void);   /* ランタイム状態のみリセット (フラグ等) */
```

### 4.2 マス情報取得 (board_query.c)

```c
int  board_mass_count(void);
const BoardMass *board_get_mass(u16 id);
u8   board_get_type(u16 id);
int  board_has_branch(u16 id);    /* connect_count >= 2 */
int  board_get_connections(u16 id, u16 *out, int max);

/* 特定種別のマスを検索 */
int  board_find_by_type(u8 type, u16 *out, int max);

/* 同マス判定: positions配列の中でmass_idにいるインデックスを返す */
int  board_check_colocated(u16 mass_id, const u16 *positions,
                            int count, u8 *out_indices, int max);
```

### 4.3 移動シミュレーション (board_move.c)

```c
/* fromからsteps歩進んだ先のマスIDを返す (分岐なしの直線移動)
 * 分岐に到達した場合は分岐マスIDを返し、stepsが残っていることを示す
 * *remaining に未消化ステップ数を格納
 */
u16  board_walk(u16 from, int steps, int *remaining);

/* N手先のマス列を取得 (指定方向dir)
 * 分岐が発生したら停止
 * 戻り値: 実際に取得したマス数
 */
int  board_peek_path(u16 from, u8 dir, u16 *out, int max);

/* 2マス間の最短距離 (-1=到達不能) */
int  board_distance(u16 from, u16 to);
```

### 4.4 フラグ操作 (board_query.c)

```c
void board_set_flag(u16 mass_id, u8 flag);
void board_clear_flag(u16 mass_id, u8 flag);
int  board_has_flag(u16 mass_id, u8 flag);

/* 罠管理 */
void board_set_trap(u16 mass_id, u8 owner_id);
void board_clear_trap(u16 mass_id);
u8   board_get_trap_owner(u16 mass_id);
```

### 4.5 区画管理 (board_area.c)

```c
int  board_is_area_unlocked(u8 area_id);
int  board_unlock_area(u8 area_id);
void board_lock_area(u8 area_id);
int  board_area_count(void);
```

### 4.6 動的マス操作

```c
/* ランタイムでマスを追加 (イベントで一時マス生成等) */
int  board_add_mass(const BoardMass *mass);   /* 戻り値: マスID */

/* 接続の動的変更 */
int  board_add_connection(u16 from, u16 to);
void board_remove_connection(u16 from, u16 to);
```

---

## 5. DBスキーマ (game.db — board セクション)

```sql
CREATE TABLE masses (
    id      INTEGER PRIMARY KEY,
    type    INTEGER NOT NULL,
    area    INTEGER NOT NULL DEFAULT 0,
    param   INTEGER DEFAULT 0,
    cost    INTEGER DEFAULT 0,
    flags   INTEGER DEFAULT 0,
    x       INTEGER DEFAULT 0,
    y       INTEGER DEFAULT 0
);

CREATE TABLE connections (
    from_id       INTEGER NOT NULL,
    to_id         INTEGER NOT NULL,
    bidirectional INTEGER NOT NULL DEFAULT 1,
    PRIMARY KEY (from_id, to_id)
);

CREATE TABLE areas (
    id           INTEGER PRIMARY KEY,
    unlock_type  INTEGER NOT NULL DEFAULT 0,
    unlock_param INTEGER DEFAULT 0
);
```

---

## 6. リソース使用量の見積もり

| 項目 | サイズ |
|------|--------|
| コード (.text) | ~1.2KB |
| BoardMass[256] | 256 × 26B = 6.5KB |
| BoardArea[8] | 32B |
| 内部変数 | ~16B |
| **合計 RAM** | **~7.8KB** |

---

## 7. 実装フェーズ

### Phase 1: コア実装

- [x] `libos32board.h` ヘッダ作成
- [x] `board_core.c` 実装 (init, shutdown, DBロード)
- [x] `board_query.c` 実装 (マス情報取得, フラグ操作)
- [x] `board_move.c` 実装 (移動シミュレーション, 先読み)
- [x] `board_area.c` 実装 (区画管理)
- [x] Makefile 統合
- [x] テストプログラム `board_test.c` 作成

### Phase 2: 拡張

- [x] 動的マス追加/接続変更
- [x] 最短距離計算 (BFS)
- [ ] コスト付き経路探索

---

*この設計書は libos32board の実装に先立つ設計ドキュメントであり、*
*実装フェーズの進行に伴い更新される。*
