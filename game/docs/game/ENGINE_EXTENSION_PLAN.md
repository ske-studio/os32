# OS32 ゲームエンジン拡張 実装計画書 — libos32rpg / libos32save / libos32turn

*策定: 2026-06-05*

> 本書は、対戦スゴロクRPGのOS32移植(`GAME_PORT_PLAN.md`)に先立って**先行実装する3本の
> 汎用エンジンライブラリ**の設計・公開API・DBスキーマ・実装手順・検証方法を定義する。
>
> 対象: `libos32turn`(手番/週スケジューラ) / `libos32rpg`(キャラ育成・状態・リボーン) /
> `libos32save`(セーブ状態管理)。
> いずれも **特定ゲーム非依存・gfx非依存・C89・SQLite or RAM駆動・`*_test.c` 付き**という
> 既存 `libos32*` の規約に従う。
>
> 本書は `GAME_PORT_PLAN.md` §5「ステージA」の詳細版である。

---

## 0. なぜ先行実装か

ゲーム移植(ステージB)の Phase 2 以降は、戦闘後の**経験値・成長**(`libos32rpg`)、
多人数の**手番進行と週境界**(`libos32turn`)、**セーブ**(`libos32save`)に依存する。
これらを欠いたままゲーム側に各機能を個別実装すると、後で必ずlibへ括り直すことになり二度手間。
よって**汎用基盤を先に完成・テストしてから**、その上にゲームを載せる。

3本はいずれも `player.c` / `state.c` / `save.c` に**既に実装済みのDOSロジックが存在する**ため、
それを「正解(ゴールデンリファレンス)」として移植・検証できる(新規発明ではなく汎用化)。

### 0.1 依存関係

```
libos32math (rng, fix16) ──┐
libos32db   (SQLite)     ──┤
                           ├─> libos32rpg   (EXP/成長/状態異常/リボーン)
libos32battle (status定義) ┘        │ bridge: RpgActor <-> BtlUnit
                                    │
libos32turn  (手番/週) ── 連携 ──> libos32event (evt_tick を毎ターン呼ぶ)
libos32save  (KAPIファイルI/O上位) … 独立
```

### 0.2 実装順序

| 順 | lib | 理由 |
|----|-----|------|
| **A-1** | `libos32turn` | 依存なし・最小・手番ループを最初に解禁 |
| **A-2** | `libos32rpg` | 最大。`libos32math`(rng)と`libos32battle`(status定義)に依存 |
| **A-3** | `libos32save` | 独立(A-1/A-2と並行可)。確定した構造体を保存するため最後に整合 |

---

## 1. libos32turn — 手番 / ラウンド / 週スケジューラ

### 1.1 目的

多人数(1〜4)の手番回し、ラウンド境界(= 週 = 7ターン)、最大ターン到達判定、
死亡/行動不能プレイヤーのスキップを管理する。**`state.c` の進行部分だけ**を汎用化したもの。
描画もゲームルールも持たない純状態機械。DBは不要(手動設定、`board`/`ai` と同じくNULL許容方針)。

### 1.2 データ構造

```c
#define TURN_MAX_PLAYERS  8

/* 手番順序ポリシー */
#define TURN_ORDER_RR      0   /* ラウンドロビン(既定) */
#define TURN_ORDER_REVERSE 1   /* 逆順 */
#define TURN_ORDER_RANDOM  2   /* 毎ラウンド・ランダム */

typedef struct {
    u8   num_players;                 /* 参加人数 */
    u8   current;                     /* 現在の手番プレイヤー */
    u8   round_len;                   /* 1ラウンドのターン数(週=7) */
    u8   order;                       /* TURN_ORDER_* */
    u16  turn_count;                  /* 累計ターン(1起点) */
    u16  round_count;                 /* 累計ラウンド(=週) */
    u16  max_turns;                   /* 0=無制限 */
    u8   active[TURN_MAX_PLAYERS];    /* 1=参加中, 0=脱落 */
    u8   skip[TURN_MAX_PLAYERS];      /* >0でNターン手番スキップ(死亡/麻痺) */
    u8   phase;                       /* ターン内サブフェーズ(ゲーム定義) */
    u8   _pad;
} TurnState;

/* turn_advance の結果 */
typedef struct {
    u8   current;          /* 新しい手番プレイヤー */
    u8   crossed_round;    /* 1=このadvanceでラウンド(週)境界を跨いだ */
    u8   is_over;          /* 1=最大ターン到達 or 生存1人以下 */
    u8   _pad;
    u16  turn_count;
    u16  round_count;
} TurnAdvance;
```

### 1.3 公開API

```c
/* システム */
void turn_init(TurnState *t, u8 num_players, u8 round_len, u16 max_turns);
void turn_set_order(TurnState *t, u8 order);

/* 進行 */
u8   turn_current(const TurnState *t);
int  turn_advance(TurnState *t, TurnAdvance *out); /* 次の有効プレイヤーへ。戻り値=is_over */

/* ラウンド(週)・ターン */
int  turn_is_round_boundary(const TurnState *t);   /* current==ラウンド先頭か */
u16  turn_round(const TurnState *t);
u16  turn_count(const TurnState *t);

/* 参加状態・スキップ */
void turn_skip(TurnState *t, u8 player, u8 n);     /* Nターン手番を飛ばす(死亡等) */
void turn_set_active(TurnState *t, u8 player, int active);
int  turn_alive_count(const TurnState *t);
int  turn_is_over(const TurnState *t);

/* ターン内フェーズ(任意) */
void turn_set_phase(TurnState *t, u8 phase);
u8   turn_get_phase(const TurnState *t);
```

`turn_advance` の挙動: 次のプレイヤーへ進める際、`skip[p]>0` のプレイヤーは
`skip[p]--` してスキップ。`active[p]==0` は飛ばす。ラウンド先頭へ戻ったら `round_count++`、
`crossed_round=1`。`max_turns` 到達 or 生存1人以下で `is_over=1`。

### 1.4 ファイル構成・実装

```
programs/libos32turn/
    libos32turn.h     公開ヘッダ
    turn_core.c       init/advance/skip/active
    turn_query.c      current/round/count/is_over/phase
```

`TURN_ORDER_RANDOM` は `libos32math` の `rng_*` を使用。

### 1.5 検証 (`programs/tests/turn_test.c`)

- 4人 RR で `turn_advance` ×8 → `turn_count` 進行・**7ターンで週境界**(`crossed_round`)。
- `turn_skip(p,2)` → 当該プレイヤーが2回飛ばされ、3巡目で復帰。
- `turn_set_active(p,0)` → 脱落者を完全に飛ばす。生存1人で `is_over`。
- `max_turns=693`(99週)到達で `is_over`。
- 期待値は DOS `state.c` の手番/週カウンタ進行と一致させる。

---

## 2. libos32rpg — キャラクター育成・状態・リボーン

### 2.1 目的

**永続キャラクター**の経験値・レベル・成長・フィールド状態異常・死亡/復活・順位を管理する。
`libos32battle` は `BtlUnit`(戦闘中の一時ステータス)止まりで永続層を持たないため、その空白を埋める。
`player.c` のロジックを汎用化したもの。データ(EXP曲線/成長/状態/リボーン)はSQLite駆動。

### 2.2 データ構造

```c
/* 永続アクター(プレイヤー/CPU/重要NPC) */
typedef struct {
    u8   level;          /* 1〜99 */
    u8   class_id;       /* 職業/氏神(成長テーブル選択) */
    u32  exp;            /* 累計経験値 */
    i16  atk, def, spd, mag;
    i16  hp, max_hp;
    u32  status;         /* 状態異常ビット(libos32battleと同形式) */
    u8   dead_turns;     /* 0=生存, >0=行動不能ターン数 */
    u8   fled;           /* 1=とうそう(その場リボーン分岐) */
    u8   pending_points; /* レベルアップ未配分の自由ポイント */
    u8   _pad;
} RpgActor;              /* 24B */

/* add_exp / levelup の結果 */
typedef struct {
    u8   levels_gained;
    u8   free_points;    /* 配分待ちポイント合計 */
    u8   _pad[2];
} RpgLevelResult;

/* 状態異常tickのログ(ゲーム側メッセージ用) */
typedef struct {
    i16  tick_damage;    /* このtickで受けたダメージ合計 */
    u16  cleared;        /* 自然回復したstatusビット */
    u8   action_blocked; /* 1=眠り/麻痺で行動不能 */
    u8   _pad[3];
} RpgTickLog;

/* 配分ポリシー(CPU自動配分) */
typedef void (*rpg_alloc_policy_fn)(RpgActor *a, u8 points);
```

### 2.3 公開API

```c
/* システム */
int  rpg_init(const char *db_path);   /* EXP曲線/成長/状態/リボーン表をロード */
void rpg_shutdown(void);

/* 初期化 */
void rpg_actor_init(RpgActor *a, u8 class_id);  /* 基礎値+職業ボーナス */

/* 経験値・レベル */
u32  rpg_exp_for_level(u8 level);                /* 累計必要EXP(既定: lv^2*10) */
u8   rpg_level_for_exp(u32 exp);
u32  rpg_exp_to_next(const RpgActor *a);
int  rpg_add_exp(RpgActor *a, u32 amount, RpgLevelResult *out); /* 自動レベルアップ */

/* ステータス配分 */
int  rpg_alloc_point(RpgActor *a, u8 stat);      /* BTL_STAT_* を1上げ pending_points-- */
void rpg_set_alloc_policy(rpg_alloc_policy_fn fn);/* CPU自動配分 */
void rpg_auto_alloc(RpgActor *a);                /* 登録ポリシーで pending を消費 */

/* フィールド状態異常 */
void rpg_status_apply(RpgActor *a, u32 bit);
void rpg_status_clear(RpgActor *a, u32 bit);
int  rpg_has_status(const RpgActor *a, u32 bit);
int  rpg_status_tick(RpgActor *a, RpgTickLog *out); /* 毎ターン: ダメージ+確率回復。戻り値=行動不能 */

/* 死亡・リボーン */
void rpg_set_dead(RpgActor *a, u8 fled);
int  rpg_is_dead(const RpgActor *a);
int  rpg_reborn_check(RpgActor *a, int rank, int num_players); /* 1=復活, 0=継続 */

/* クエリ */
int  rpg_total_power(const RpgActor *a);
int  rpg_rank(const u32 *scores, int n, int idx);  /* 総資産等のスコア配列から順位 */

/* libos32battle 連携 */
void rpg_to_btl_unit(const RpgActor *a, BtlUnit *u);   /* 戦闘開始時 */
void rpg_from_btl_unit(RpgActor *a, const BtlUnit *u); /* 戦闘後HP/status書き戻し */
```

### 2.4 DBスキーマ (`assets/rpg.db`, `tools/gen_rpg_db.py`)

```sql
-- EXP曲線(累計必要EXP)。式駆動 or 明示テーブルのいずれか
CREATE TABLE exp_curve (level INTEGER PRIMARY KEY, total_exp INTEGER);

-- 職業/氏神ごとの自動成長(レベルアップ時に加算)+ 自由配分ポイント
CREATE TABLE level_growth (
    class_id INTEGER, atk INTEGER, def INTEGER, spd INTEGER, mag INTEGER,
    hp INTEGER, free_points INTEGER, PRIMARY KEY(class_id));

-- フィールド状態異常(libos32battle の status_effects を拡張)
CREATE TABLE status_field (
    bit_flag INTEGER PRIMARY KEY,
    prevents_action INTEGER,   -- 1=行動不能
    tick_kind INTEGER,         -- 0=なし,1=固定,2=レベル比例,3=最大HP%
    tick_value INTEGER,
    recovery_pct INTEGER,      -- 毎ターン自然回復確率%
    lethal INTEGER);           -- 0=HP1で止める(毒/呪い), 1=死亡可

-- 順位別リボーン待ち(min/max ターン)
CREATE TABLE reborn_table (rank_bucket INTEGER PRIMARY KEY, min_turns INTEGER, max_turns INTEGER);
```

> `status_field` は `battle.db` の `status_effects` と**ビット定義を共有**する(同じ状態異常を
> 戦闘中tickとフィールドtickの双方で扱えるようにする)。これにより player.c と battle.c に
> 二重化していた状態異常定義が一本化される。

### 2.5 ファイル構成・実装

```
programs/libos32rpg/
    libos32rpg.h
    rpg_core.c      init/shutdown/actor_init/DBロード
    rpg_level.c     exp_for_level/level_for_exp/add_exp/alloc
    rpg_status.c    status_apply/clear/tick(確率回復)
    rpg_reborn.c    set_dead/reborn_check(順位別確率ランプ)
    rpg_query.c     total_power/rank/btl_unit bridge
```

DOSの式を既定ポリシーとして移植:
- EXP曲線 `total = lv*lv*10`(`player.c:exp_table`)
- リボーン確率ランプ `step = 256/(max-min); threshold = (dead_turns-min)*step`(`player.c:reborn_check`)
- 毒=レベル分(致死しない)・呪い=最大HP15%×1/4・眠り回復1/3・麻痺回復1/2(`player.c:status_update`)

### 2.6 検証 (`programs/tests/rpg_test.c`)

- `rpg_exp_for_level(2)==40`, `(10)==1000`。`rpg_add_exp` で正しいレベル数・pending を返す。
- 成長: ある職業でレベルアップ後の atk/def/hp が DOS `player_levelup` と一致。
- 状態異常tick: 毒ダメージ=レベル、HP1で下げ止まり。回復確率は **同一seed**で一致、
  または N回試行の分布で検証(§5 rng整合)。
- リボーン: 順位別 min/max・最大到達で確定復活・確率ランプが `player.c` と一致。
- bridge: `rpg_to_btl_unit`→`btl_resolve_turn`→`rpg_from_btl_unit` でHPが往復。

---

## 3. libos32save — セーブ状態管理

### 3.1 目的

任意のRAM領域を **magic/version/チェックサム付き**で永続化する汎用セーブ層。
`save.c` の生 `fopen/fwrite`+XOR を、領域登録・スロット管理・バージョン移行・破損検出を備えた
KAPIファイルI/Oベースの汎用libに引き上げる。どのゲームでも再利用できる。

### 3.2 データ構造

```c
#define SAVE_MAX_REGIONS  16

typedef struct {
    const void *ptr;   /* 保存対象(読込時は書込先) */
    u32   size;        /* バイト数 */
    u16   id;          /* 領域識別(移行コールバック用) */
    u16   _pad;
} SaveRegion;

typedef struct {
    char  magic[4];                       /* 例 "DKP2" */
    u32   version;
    u16   region_count;
    u16   _pad;
    SaveRegion regions[SAVE_MAX_REGIONS];
} SaveContext;

/* ヘッダのみ読む(スロット一覧表示用) */
typedef struct {
    char  magic[4];
    u32   version;
    u32   total_size;
    u32   mtime;       /* RTC(任意) */
    u32   user_meta;   /* ゲーム定義の小メタ(ターン数など) */
} SaveMeta;

/* バージョン移行: 旧版領域を新版へ変換 */
typedef int (*save_migrate_fn)(u32 old_ver, u32 new_ver,
                               void *region, u16 region_id, u32 size);
```

ディスク形式: `[ヘッダ(magic,version,region_count,user_meta)] [各region size] [payload連結] [CRC32]`。

### 3.3 公開API

```c
void save_begin(SaveContext *c, const char magic[4], u32 version);
int  save_add_region(SaveContext *c, const void *ptr, u32 size, u16 id);

int  save_write(SaveContext *c, const char *path, u32 user_meta);
int  save_read (SaveContext *c, const char *path);   /* 検証→登録領域へ復元 */

int  save_peek(const char *path, SaveMeta *out);     /* ヘッダのみ(読込なし) */

void save_set_migrate_cb(save_migrate_fn fn);
u32  save_crc32(const void *data, u32 size);
```

戻り値: `0`=成功, `-1`=I/O, `-2`=magic不一致, `-3`=チェックサム不一致, `-4`=未対応version。
書き込みは一時ファイル→リネームの**アトミック書込**を基本とする(FS非対応時は直接書込にフォールバック)。

### 3.4 ファイル構成・実装

```
programs/libos32save/
    libos32save.h
    save_core.c    begin/add_region/write/read
    save_meta.c    peek/migrate/crc32
```

KAPIファイルI/O(`open`/`read`/`write`/`seek`、`KAPI_O_CREAT|KAPI_O_TRUNC|KAPI_O_WRONLY`)を使用。
チェックサムは XOR(DOS互換)ではなく **CRC32** を採用(破損検出力向上)。

### 3.5 検証 (`programs/tests/save_test.c`)

- ラウンドトリップ: 構造体配列を登録→`save_write`→ゼロクリア→`save_read`→完全一致。
- 破損検出: ファイル1バイト改変→`save_read` が `-3`。
- magic不一致→`-2`。version差→migrate cb 呼出し、未対応で `-4`。
- `save_peek` がロードなしで magic/version/user_meta を返す。

---

## 4. ビルド統合

各libは既存 `libos32*` と同一規約で追加する。

1. **ソース配置**: `programs/libos32turn/` `programs/libos32rpg/` `programs/libos32save/` に
   `libos32*.h` + 分割 `.c` を作成。
2. **Makefile登録**: ルート `Makefile` の programs ビルドに各libを追加(外部プログラム規約 `-3s`、
   既存 `libos32board` 等のエントリに倣う)。`libos32rpg` は `libos32math`/`libos32db`/`libos32battle` をリンク。
3. **ヘッダ参照**: 共有型は `os32_kapi_shared.h`(`u8`/`u16`/`u32`/`i16`)。
   `libos32rpg` は `libos32battle.h`(`BtlUnit`/`BTL_STAT_*`/status ビット)を取り込む。
4. **DB生成**: `tools/gen_rpg_db.py` 追加(`assets/rpg.db`)。turn/save はDB不要。
5. **テスト**: `programs/tests/turn_test.c` `rpg_test.c` `save_test.c` を `board_test.c` 形式
   (`int main(int,char**,KernelAPI*)` + `check()`)で作成。
6. **設計書**: `docs/tasks/libturn/` `docs/tasks/librpg/` `docs/tasks/libsave/` に
   `LIB*_DESIGN.md` を配置(本書から各libの該当節を展開)。

---

## 5. 横断課題・リスク

| 課題 | 内容 | 対策 |
|------|------|------|
| **RNG整合** | DOS `rng.c`(Fisher-Yates表)と `libos32math` の乱数列が異なると確率ロジックの厳密一致が取れない | (a)DOS RNG表を `libos32math` に移植して seed 一致、または (b)状態異常回復/リボーンは**N回試行の分布**で検証 |
| status定義の共有 | フィールドtick(rpg)と戦闘tick(battle)で状態異常ビットを統一 | `rpg.db.status_field` と `battle.db.status_effects` のビット定義を同一に保つ。生成スクリプトで共有 |
| 16→32bit レイアウト | DOS `unsigned int`(16bit)前提のセーブ構造体がOS32で拡大 | `libos32save` は領域をそのままblit。DOSセーブ互換は破棄(magic="DKP2"/version=2) |
| 成長の氏神依存 | 自動成長が氏神(ゲーム固有)に依存 | rpgは `class_id`+DB成長表で汎用化。氏神→class_idの対応はゲーム側グルー |
| `pending_points` の人手配分 | レベルアップ自由配分はUIが要る | rpgは pending を返すだけ。配分UI/CPUポリシーはゲーム側 or `rpg_set_alloc_policy` |

---

## 6. 完了条件 (M0)

- [x] `libos32turn` 実装 + `turn_test.c` 全合格(4人ローテ・週境界・スキップ・最大ターン)
      — 実機 35/35 PASS (2026-08-07)
- [x] `libos32rpg` 実装 + `rpg_test.c` 全合格(EXP/成長/状態tick/リボーンが DOS `player.c` と一致 or 分布一致)
      — 実機 60/60 PASS (2026-08-07)
- [x] `libos32save` 実装 + `save_test.c` 全合格(往復/破損/version)
      — 実機 15/15 PASS (2026-08-07)
- [x] 3libが `Makefile` でビルドされ、`gen_rpg_db.py` が `assets/rpg.db` を生成
      — `build/libs.mk` の `DEFINE_LIB` / `build/programs.mk` の `DEFINE_TEST` に登録済み
- [ ] `docs/tasks/lib{turn,rpg,save}/` 設計書を配置
      — 未実施。本書 §1〜§3 が実質の設計書として機能しているため優先度は低い

> **M0 は達成済み (2026-08-07)。** 実装は `feat/vdm` 系の作業ツリーで行われ、
> 本リポジトリ `main` へ移植・実機検証した (移植計画:
> [`../wintree_port/PORT_PLAN.md`](../../../docs/tasks/wintree_port/PORT_PLAN.md) フェーズ4)。

M0達成後、`GAME_PORT_PLAN.md` ステージB Phase 0 へ進む。

---

## 7. 参照

- 移植計画(親文書): `docs/tasks/game/GAME_PORT_PLAN.md`
- 移植元ロジック(ゴールデン): `sample/game/src/player.c`(rpg) / `state.c`(turn) / `save.c`(save)
- 既存lib規約の手本: `programs/libos32board/`(構成) / `programs/tests/board_test.c`(テスト形式)
- 連携lib: `libos32battle.h`(BtlUnit/status) / `libos32event.h`(evt_tick) / `libos32math`(rng) / `libos32db`
- KAPI: `include/os32_kapi_shared.h`(ファイルI/O定数 `KAPI_O_*`、共有型)
