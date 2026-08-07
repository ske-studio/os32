# 対戦スゴロクRPG — OS32 移植・完成 計画書

*策定: 2026-06-05*

> この文書は、`sample/game/` にあるDOS時代の対戦スゴロクRPG（ドカポン風 / PC-9801 /
> Open Watcom 16bit large model）を、OS32の `programs/libos32*` ゲームエンジン群を用いて
> OS32外部プログラムとして移植・完成させるための方針・アーキテクチャ・段階別実装計画を定義する。
>
> 移植元: `sample/game/`　／　移植先: `programs/game/`（新規）+ 各 `libos32*`

---

## 0. エグゼクティブサマリー

**結論: この移植は「ゼロからの作り直し」ではなく「完成済みロジックの再プラットフォーム +
描画/音/入力の新規実装」である。**

調査の結果、2つの事実が判明した。

1. **DOSゲーム側は純ロジック・マスターデータ・全メッセージが既に完成している。**
   `battle.c`(32KB) / `state.c`(37KB) / `item.c`(19KB) / `magic.c`(10KB) / `player.c` /
   `cpu_ai.c` / `event.c` / `devil.c` / `enemy_data.c` / `village_data.c` / `map_data.c` /
   `msg_data.c`(38KB) は実装済み。一方 HAL層 (`gfx.c`/`ui.c`/`input.c`/`sound.c`/`map.c`/`shop.c`)
   は **45バイトのスタブ**のまま。つまり「ゲームの頭脳は完成、身体(描画・入力・音)が空」という状態。

2. **OS32のエンジンライブラリ群は、このゲームを動かす目的で設計されたとしか思えないほど一致している。**
   §1.3 のとおり、DOSゲームの各モジュールに対応するOS32ライブラリが既に存在し、
   データ構造・計算式までほぼ一致する(例: `EconEstate` 構造体は DOS の `Village` と
   フィールド単位で対応、戦闘式 `def*1.25` / 回避 `0-50%` は docs のテストケースと完全一致)。

したがって最短経路は、**完成済みのDOSロジックを「正解(ゴールデンリファレンス)」として保持しつつ、
ゲームをOS32エンジンlibの上に段階的に載せ替え、HAL層をOS32の gfx/microui/input/snd で新規実装する**こと。

---

## 1. 現状分析

### 1.1 移植元 — DOSゲーム「対戦スゴロクRPG」

ドカポン321(SFC)をインスピレーションとした **1〜4人対戦スゴロクRPG**。
マップを移動しモンスターと戦い、村を統治して資産王/制覇王/征服王を目指す。
職業5種・デビルマン変身・PvP・ランダムイベント・ターン終了時オートセーブ。

設計ドキュメントは `sample/game/docs/00〜20` の21本が完備(概要/フロー/マップ/村経済/戦闘/
計算式/プレイヤー/デビル/AI/アイテム/敵マスター/言霊/週次イベント/メッセージ/セーブ/
PC98実装/ライブラリ/アーキテクチャ)。

実装完成度:

| 層 | モジュール | 状態 | 規模 |
|----|-----------|------|------|
| 純ロジック | battle / state / player / item / magic / village / devil / event / cpu_ai / save / rng | **実装済** | 多数(32KB級含む) |
| マスターデータ | enemy_data / village_data / map_data / msg_data | **実装済** | 敵44+BM8 / 村59 / 全メッセージ |
| HAL(描画) | gfx.c | **スタブ(45B)** | 未実装 |
| HAL(UI) | ui.c | **スタブ** | 未実装 |
| HAL(入力) | input.c | **スタブ** | 未実装 |
| HAL(音) | sound.c | **スタブ** | 未実装 |

> アーキテクチャ(`docs/19_architecture.md`)は最初からHW分離設計で、
> 「テキストUIでロジック検証 → 実機実装」の段階移行を想定している。
> この設計が、そのままOS32移植の追い風になる。

未設計項目(`sample/game/docs/INDEX.md` 末尾):
- シナリオ・ストーリー(未着手)
- ボス配置(シナリオ依存・未確定)
- 赤宝箱マス(ハイリスク・リターン、後日設計)

### 1.2 移植先 — OS32 ゲームエンジン (`programs/libos32*`)

OS32外部プログラムは `void __cdecl main(int argc, char **argv, KernelAPI *kapi)` をエントリとし、
`-3s`(スタック規約)でビルド、`KernelAPI` テーブル経由でカーネル機能(gfx/入力/ファイル/
**SQLite DB**/FM音源/RTC)を呼ぶ。

その上に、**gfx非依存・SQLite駆動の汎用ゲームエンジンlib群**が揃っている:

| ライブラリ | 役割 | 主なAPI |
|-----------|------|---------|
| `libos32board` | ノードグラフ盤(スゴロク) | `board_walk`/分岐/`board_distance`(BFS)/`board_find_path`(Dijkstra)/罠/区画 |
| `libos32battle` | ターン戦闘解決 | `btl_calc_damage`/`btl_resolve_commands`(攻守マトリクス)/状態異常/`btl_transform`(変身) |
| `libos32econ` | 経済+不動産 | `econ_estate_*`(統治/投資/上納金/共同統治/種別ボーナス)/市場/通貨/外交 |
| `libos32inv` | 所持・装備・店 | `inv_bag`/`inv_equip`/`inv_shop_*`/`inv_lottery`(宝箱)/耐久/セット/合成 |
| `libos32ai` | 意思決定 | `ai_decide`(性格+ノイズ)/`ai_weighted_pick`/`ai_lookahead`/履歴+カウンター読み合い |
| `libos32event` | イベント駆動 | 周期/確率/条件発火/クールダウン/排他グループ/連鎖 |
| `libos32text` | メッセージ | `text.db` からのテキスト取得 |
| `libos32tilemap` | BG/マップチップ描画 | タイル合成・BGスクロール |
| `libos32gfx` | 描画基盤 | 640×400 16色 直接描画・スプライト |
| `libos32ui` | UI(microui) | `mu_window`/`mu_button`/メニュー/テキスト/即時モード |
| `libos32input` | 入力 | バインド/コンテキスト(4人分のコマンド入力に有用) |
| `libos32snd` | 音 | FM音源 |
| `libos32math` | 数値 | `rng_*`(乱数)/`fix16`(固定小数) |
| `libos32ecs` / `libos32map` | ECS / タイルマップview | 補助 |

各libには `programs/tests/*_test.c`(board_test / btl_test / econ_test / inv_test /
ai_test / evt_test …)があり、**そのまま使用例・APIリファレンスになる**。
また各libの設計書が `docs/tasks/lib*/` に存在する。

マスターデータは `assets/*.db`(board.db / battle.db / econ.db / items.db / events.db /
ai.db / map.db / text.db)で、`tools/gen_*_db.py` が SQLite で生成する。
**現状のDBは各libのテスト用最小データ(例: board.dbは12マス)であり、実ゲームのデータは未投入。**

### 1.3 モジュール対応表(DOS → OS32)

| DOS モジュール | OS32 ライブラリ | 一致度 | 備考 |
|----------------|-----------------|--------|------|
| `map.c` + `map_data.c`(255マス/接続/分岐/区画/罠/ゲート) | `libos32board` | ◎ | `BOARD_FLAG_TRAP/BLOCKED/HIDDEN/ONEWAY` まで対応。⚠ `MAX_AREA=9` vs `BOARD_MAX_AREAS=8` |
| `village.c` + `village_data.c`(59村/Lv1-6/上納金/投資/価値/共同統治) | `libos32econ` (estate) | ◎ | `EconEstate` が `Village` とほぼ同型。`co_owner`/`share_pct`/`stage`/`monster_id` 完備 |
| `battle.c`(ダメージ/攻守/ためる/状態異常/言返し) | `libos32battle` | ◎ | 計算式まで一致(`def*1.25`,回避`0-50%`)。`BTL_RES_REFLECT/COUNTER` あり |
| `devil.c`(デビルマン変身) | `libos32battle` (`btl_transform`) | ◎ | `BtlTransformDef`(倍率/ターン/専用装備)が変身仕様に対応 |
| `item.c` + 武器/盾/鎧/道具(手荷物8+袋2) | `libos32inv` | ○ | `InvBag`(16枠+装備8)/`inv_shop`/`inv_lottery`/`inv_durability`/`inv_setbonus` |
| `magic.c`(言霊 フィールド/バトル) | `libos32inv`(言霊屋) + `libos32battle`(効果) | △ | 専用libなし。アイテムとして扱い、効果はゲーム側switchで解釈 |
| `cpu_ai.c`(3性格: 勇猛/商人/策士) | `libos32ai` | ◎ | `AiProfile` パラメータで性格表現、PvP読み合いに `ai_history`/`ai_counter` |
| `event.c`(週次イベント9種/クールダウン10週/BM討伐指令) | `libos32event` | ◎ | `EVT_TYPE_PERIODIC/RANDOM/CONDITION`/cooldown/排他グループ/連鎖 |
| `msg_data.c` + `msg_id.h`(全メッセージ) | `libos32text` (`text.db`) | ○ | UTF-8メッセージをDB化 |
| `rng.c`(Fisher-Yates RNGテーブル) | `libos32math` (`rng_*`) | ○ | 乱数源を差し替え |
| `save.c`(SaveData) | KAPI ファイルI/O | ○ | `open/write/read`。DOSセーブ互換は不要 |
| `gfx.c`(スタブ) | `libos32gfx` + `libos32tilemap` | 新規 | マップチップ・キャラ・盤面描画 |
| `ui.c`(スタブ) | `libos32ui` (microui) | 新規 | ステータス窓/コマンドメニュー/メッセージ窓 |
| `input.c`(スタブ) | `libos32input` + KAPI kbd | 新規 | 4人分のコマンド入力 |
| `sound.c`(スタブ OPN/FMP) | `libos32snd` + KAPI FM | 新規 | BGMはMPI/OPI→OS32形式へ変換が必要 |

### 1.4 エンジン側の不足 — 先行実装すべき新規ライブラリ3本

対応表のうち、**既存libでカバーされていない層が3つ**ある。これらは特定ゲーム非依存の汎用基盤であり、
ゲーム移植に着手する前に**エンジン拡張として先行実装する**(§5 ステージA)。

| 新規lib | 吸収するゲームロジック | 補完する既存lib |
|---------|------------------------|-----------------|
| `libos32rpg` | `player.c`: 経験値曲線/レベルアップ成長/ステ配分/フィールド状態異常tick/死亡・リボーン/順位 | `libos32battle`(戦闘専用)の永続キャラ層を補完 |
| `libos32save` | `save.c`: magic/version/チェックサム付きセーブ状態管理(領域登録→スロット書込) | KAPIファイルI/Oの上位汎用層 |
| `libos32turn` | `state.c`の進行部: 多人数手番回し/週境界(7ターン)/最大ターン/死亡者スキップ | `libos32event`(週次イベント)と対 |

> 詳細な設計・API・実装計画・検証は **別冊『エンジン拡張実装計画書』(`docs/tasks/game/ENGINE_EXTENSION_PLAN.md`)** に定義する。
> 言霊(magic)・確率式・総資産は新lib化せず既存libへ吸収する(§2.3 / 別冊参照)。

---

## 2. 移植方針の選択

### 2.1 3案の比較

| 案 | 内容 | 長所 | 短所 |
|----|------|------|------|
| **A. 直接ポート** | DOSロジック.c を `-3s` で再コンパイルし、HALだけOS32実装 | 最速で動く | エンジンlibを使わない/DB化されない/16→32bit前提差の手当て |
| **B. 全面再プラットフォーム** | ロジックをエンジンlib API で全書き換え、データを全DB化 | OS32設計に完全準拠/DB駆動/lib側のテスト資産活用 | 一括書き換えはリスク大・検証困難 |
| **C. ハイブリッド段階移行(推奨)** | 高レベルの状態機械(`state.c`)とゲーム固有グルーは残し、重い処理(盤移動/戦闘/経済/所持/AI/イベント)をモジュール単位で順次エンジンlibへ委譲。データも順次DB化 | リスク分散/各段階で動作確認可/DOSロジックを正解として差分検証 | 移行期は新旧が混在 |

### 2.2 推奨 = C(ハイブリッド段階移行)

理由:
- エンジンlibは**このゲーム向けに設計**されており、最終的にlib側へ寄せるのが自然(B方向)。
- ただし一括書き換え(B)は検証が難しい。**DOSの完成済みロジックを「正解」として1モジュールずつ
  置き換え、結果を突き合わせ**れば、回帰を機械的に検出できる(C)。
- 各フェーズが独立して「動く成果物」を生むため、途中でも遊べる/見せられる。

### 2.3 新lib化しない要素(既存libへ吸収)

汎用性が薄い/既存libで表現できる要素は新規lib化せず、吸収する。

| 要素 | 吸収先 |
|------|--------|
| 言霊(magic) | 呪文=アイテム→`libos32inv`(言霊屋/所持上限)、効果適用→`libos32battle`/`libos32rpg`の状態異常。効果IDの分岐はゲーム側グルー |
| 対抗確率式(命中 `caster/(caster+target)`・回避・リボーン確率ランプ) | `libos32math`(または`libos32ai`)の汎用確率ヘルパー |
| 総資産・順位 | `gold + econ_estate_total_value()` のグルー(`libos32econ`が資産を保持) |

ゲーム固有で残す(lib化しない): 状態機械本体・勝利条件(資産王/制覇王/征服王)・氏神の意味づけ・メッセージ分岐。

---

## 3. ターゲット・アーキテクチャ

```
programs/game/                     ← 新規: OS32プログラム本体
├── main.c          エントリ。KAPI受け取り・初期化・メインループ(~30fps)
├── game_state.c    状態機械(DOS state.c を移植・グルー層)
├── game_glue.c     ゲーム固有ルール(氏神補正/勝利条件/週フロー等)
├── view_board.c    盤面描画 (libos32tilemap + libos32gfx)
├── view_battle.c   戦闘画面 (libos32gfx + libos32ui)
├── view_ui.c       ステータス窓/メニュー/メッセージ窓 (libos32ui microui)
├── input_map.c     入力 (libos32input + KAPI kbd) — 4人分
├── audio.c         BGM/SE (libos32snd + KAPI FM)
└── save.c          セーブ/ロード (KAPI file I/O)

リンクするエンジンlib:
  libos32board / libos32battle / libos32econ / libos32inv /
  libos32ai / libos32event / libos32text / libos32tilemap /
  libos32gfx / libos32ui / libos32input / libos32snd / libos32math

データ (assets/*.db, gen_*_db.py で生成):
  board.db   ← map_data.c から59村・全マス・接続・区画
  econ.db    ← village_data.c から村(estate)マスター
  battle.db  ← 攻守マトリクス/状態異常/属性/変身定義
  items.db   ← 武器/盾/鎧/道具/言霊 マスター
  ai.db      ← 3性格プロファイル(勇猛/商人/策士)
  events.db  ← 週次イベント9種定義
  text.db    ← msg_data.c の全メッセージ
```

メインループ雛形(`programs/apps/ui_demo/ui_demo.c` 準拠):
```
init: libos32gfx_init / mui_init / 各 *_init(db_path) / load or new game
loop:
  入力 pump (input_map)
  state_update()          ← ゲーム進行(エンジンlibへ委譲)
  gfx_clear → view描画(盤/UI/戦闘) → mui_render
  gfx_add_dirty_rect / gfx_present_dirty
  ~30fps ウェイト (get_tick)
```

### 3.1 16bit→32bit 移行で注意する差分

- `unsigned int` が 16bit→32bit に変わる(`exp`/`hp`/各カウンタの上限が広がる。動作上は安全だが
  **構造体レイアウトとセーブ形式が変わる**ためDOSセーブ互換は破棄)。
- far ポインタ・large model 前提コードは存在しない想定だが、`gfx.c` 等の旧PC98直書きは破棄するので無関係。
- `gold_t = unsigned long`(約43億)はそのまま。エンジンの estate は `u32` で整合。
- C89厳守(`//`禁止/ブロック先頭宣言)は移植元も移植先も同じ。

---

## 4. データ移植計画

DOSのハードコード表を `tools/gen_*_db.py` 経由で `assets/*.db` に投入する。
**現状DBはテスト用最小データなので、各生成スクリプトを実ゲームデータで拡張する**のが中心作業。

| 生成スクリプト | 入力(DOS) | 出力テーブル(例) | 作業 |
|----------------|-----------|------------------|------|
| `gen_board_db.py` | `map_data.c`(全マス/接続) | `masses` / `connections` / `areas` | 12マス→実マップ全マスへ拡張 |
| `econ_db_init.py` | `village_data.c`(村59) | estate マスター(価値/ステージ) | 59村投入。⚠ `MAX_AREA=9`は区画設計の見直し要 |
| `gen_battle_db.py` | `battle.c`/`docs/06` | 攻守マトリクス/状態異常/変身 | 言返し/カウンター/ためる/デビル定義を投入 |
| `gen_items_db.py` | 武器/盾/鎧/道具/言霊 | items マスター | `docs/11`/`13` の全マスター投入 |
| `gen_ai_db.py` | `cpu_ai.c`/`docs/09` | AIプロファイル | 勇猛/商人/策士の重みパラメータ |
| (events) | `event.c`/`docs/14` | events 定義 | 週次9種(周期/確率/条件) |
| `text_db_init.py` | `msg_data.c` | text マスター | UTF-8メッセージ移植 |

> **検証の要**: 各DB投入後、DOSの該当テーブル(例 `enemy_master[]`)と
> DBロード結果を突き合わせる小テストを `programs/tests/` に置き、数値一致を機械確認する。

---

## 5. 実装計画(2ステージ構成)

本計画は **「ステージA: エンジン拡張(先行・必須)」→「ステージB: ゲーム移植」** の順で進める。
ゲーム移植(ステージB)は、戦闘でキャラ成長(`libos32rpg`)・手番進行(`libos32turn`)・
セーブ(`libos32save`)に依存するため、**これらを欠いたままでは Phase 2 以降が成立しない**。
よってエンジン拡張を先に完成・テストし、安定した土台の上にゲームを載せる。

各フェーズは「動く成果物」と「検証手段」を必ず持つ。DOSロジックを正解として差分検証する。

---

### ステージA: エンジン拡張(先行実装・必須)

`libos32rpg` / `libos32save` / `libos32turn` の3本を、既存libと同じ規約
(gfx非依存・C89・SQLite駆動・`*_test.c` 付き)で新規実装する。

| 順 | 新規lib | 主目的 | 完了条件(検証) |
|----|---------|--------|----------------|
| A-1 | `libos32turn` | 多人数手番/週境界/最大ターン/死亡スキップ | `turn_test.c`: 4人ローテーション・7ターンで週境界・スキップ動作 |
| A-2 | `libos32rpg` | EXP曲線/成長/状態異常tick/リボーン/順位 | `rpg_test.c`: DOS `player.c` のゴールデン値(EXP/成長/リボーン確率)と一致 |
| A-3 | `libos32save` | magic/version/checksum付きセーブ状態管理 | `save_test.c`: ラウンドトリップ/破損検出/バージョン不一致 |

> **このステージの詳細(データ構造・公開API・DBスキーマ・段階別実装・検証)は
> 別冊『エンジン拡張実装計画書』(`ENGINE_EXTENSION_PLAN.md`)に定義する。本書ではゲーム移植との接続のみ扱う。**

接続(どのゲームPhaseがどの新libに依存するか):

| 新lib | 依存するゲームPhase |
|-------|---------------------|
| `libos32turn` | Phase 1(手番ループ)以降すべて |
| `libos32rpg` | Phase 2(戦闘後の経験値/成長)・Phase 3(順位/資産)・Phase 5(状態異常/リボーン) |
| `libos32save` | Phase 8(セーブ/ロード) |

**ステージA完了をもって M0 とし、ステージBに進む。**

---

### ステージB: ゲーム移植(Phase 0〜9)

### Phase 0 — 足場づくり(基盤)
- `programs/game/` 雛形(main + KAPI + 空メインループ + microui窓1枚)を作成し `Makefile` に登録。
- `ui_demo` を雛形に「Hello, 盤面」レベルが起動・終了できることを確認。
- **成果物**: 起動して黒画面+UI窓が出て ESC で終了する `game.bin`。
- **検証**: NP21/W で起動確認。

### Phase 1 — 盤面とプレイヤー移動 (libos32board + tilemap)
- `gen_board_db.py` を実マップ(全マス・接続・区画)に拡張、`board.db` 生成。
- サイコロ→`board_walk`→分岐選択→移動アニメ。マップチップ描画(tilemap)。
- **成果物**: 1人でサイコロを振って盤上を移動できる。
- **検証**: `board_distance`/分岐挙動が DOS `map.c` と一致(board_test拡張)。

### Phase 2 — 戦闘 (libos32battle + libos32ai)
- `gen_battle_db.py` で攻守マトリクス/状態異常/属性を投入。
- マスのモンスターと遭遇→コマンド(攻撃/必殺/言霊/ためる × 防御/カウンター/言返し/とうそう)→
  `btl_resolve_turn`。CPU防御は `ai_defend` を `libos32ai` へ。
- **成果物**: フィールド戦闘が一通り回る。
- **検証**: `docs/06_battle_calc_spec.md` の全テストケースを btl_test で再現・一致。

### Phase 3 — 村統治と経済 (libos32econ estate)
- `econ_db_init.py` に59村を投入。`econ_estate_claim`/`invest`/`accumulate`/`collect`、
  共同統治(`co_owner`)、総資産計算。
- **成果物**: 村に止まる→統治→投資→上納金徴収→総資産が動く。
- **検証**: DOS `village.c` の上納金/発展/総資産テスト(`docs/19`)と一致。

### Phase 4 — アイテム・装備・ショップ (libos32inv)
- `gen_items_db.py` に全マスター投入。`InvBag`(手荷物8+袋2=実装上16枠), 装備3スロット(武器/盾/鎧),
  ショップ3種(装備/道具/言霊屋), 宝箱抽選(`inv_lottery`)。
- **成果物**: 買い物・装備変更・宝箱取得が機能。
- **検証**: 装備による実効ATK/DEF が DOS `item.c` の `player_effective_*` と一致。

### Phase 5 — デビルマン変身 (libos32battle transform)
- `BtlTransformDef`(倍率/持続ターン/専用装備/解除率) と ゲージ(0-20)を実装。
- **成果物**: 条件成立で変身→強化→解除のサイクル。
- **検証**: DOS `devil.c` の変身判定・効果と一致。

### Phase 6 — 週次イベント (libos32event)
- 週次9種(豊穣/日照り/疫病/物忌み/神前試合/ストライキ/神託(BM討伐)/天の岩戸)を
  周期・確率・条件で登録。クールダウン10週・排他グループ・連鎖。
- **成果物**: 週の切替でイベントが発火し盤面/経済に影響。
- **検証**: 発火頻度・クールダウンが `docs/14` 仕様どおり。

### Phase 7 — CPU 思考の統合 (libos32ai)
- 方向選択/投資判断/デビル判断/PvP判断を `ai_decide`+性格プロファイルへ。
- PvPの読み合いに `ai_history`/`ai_counter`(相手の頻出行動を咎める/狙い撃つ)を活用。
- **成果物**: 1〜4人(人間+CPU混在)でターンが自動進行。
- **検証**: 各性格の行動傾向が `docs/09` の設計と整合。

### Phase 8 — メッセージ・音・セーブ・タイトル
- `text.db` 経由のメッセージ表示(`libos32text`)。
- BGM/SE(`libos32snd`+FM、MPI/OPI→OS32形式変換)。
- セーブ/ロード(KAPI file I/O)。タイトル/コンフィグ/リザルト画面。
- **成果物**: タイトル→対戦→決着→セーブの一周が通る。

### Phase 9 — 統合・未設計項目・調整
- 勝利条件(資産王/制覇王/征服王)判定の通し確認。
- 未設計項目を確定: **シナリオ/ストーリー**, **ボス配置**, **赤宝箱マス**(`docs/INDEX` 末尾)。
- バランス調整・演出強化・最適化(描画dirty rect/30fps維持)。
- **成果物**: 「完成版」候補。1ゲーム(数十週)を通しでプレイ可能。

---

## 6. 検証戦略 — DOSロジックを「正解」に

DOSの純ロジックは標準C89で、**ホスト(gcc)でもそのままビルドできる**(`docs/19` のテスト設計)。
これを利用し、各フェーズで:

1. DOS版ロジック(または既存のホストテスト)を**期待値生成器**として使う。
2. 同じ入力をOS32エンジンlibに与え、`programs/tests/*_test.c` 形式で**数値一致を機械確認**。
3. 仕様書(`docs/06`/`09`/`19` 等)のテストケースを移植してレグレッションスイート化。

これにより「載せ替えてもゲームの挙動が変わっていない」ことを各段階で保証する。

---

## 7. リスクと対策

| リスク | 影響 | 対策 |
|--------|------|------|
| 区画数 `MAX_AREA=9` > `BOARD_MAX_AREAS=8` | マップ区画設計に不整合 | 区画を8に再設計 or boardのarea上限を拡張(libos32board改修) |
| 言霊(magic)に専用libが無い | 効果実装がゲーム側に残る | アイテム(言霊屋)+battle効果で表現、効果IDをゲーム側switchで解釈(設計済の方針) |
| BGM(MPI/OPI)がOS32非対応形式 | 音が出ない | Phase 8 で変換ツール作成、または暫定SEのみで先行 |
| 16→32bit でのデータ範囲/レイアウト差 | セーブ非互換・想定外オーバーフロー | DOSセーブ互換は破棄。`unsigned int` 依存箇所を見直し |
| 現状DBがテストデータのみ | 実ゲームが動かない | データ移植(§4)を各フェーズの先頭に組み込む |
| 未設計項目(シナリオ/ボス/赤宝箱) | 「完成」の定義が曖昧 | Phase 9 で確定。MVP(対戦が一周回る)と完成版を分けて合意 |
| 4人同時のUI/入力設計 | 操作が破綻 | `libos32input` のコンテキスト機能で手番ごとに入力主体を切替 |

---

## 8. マイルストーン

| マイルストーン | 含むフェーズ | 達成状態 |
|----------------|--------------|----------|
| **M0: エンジン拡張完了** | ステージA(A-1〜A-3) | `libos32turn`/`libos32rpg`/`libos32save` がテスト合格(DOSゴールデン一致) |
| **M1: 盤面が動く** | 0–1 | サイコロで盤上を移動できる(描画あり) |
| **M2: 戦って稼げる** | 2–4 | 戦闘・村統治・買い物が機能(1人プレイで一周の骨格) |
| **M3: 対戦が成立** | 5–7 | 変身・週次イベント・CPU思考込みで多人数対戦が回る(=遊べるMVP) |
| **M4: 完成版** | 8–9 | 音/セーブ/タイトル/勝利条件/未設計項目を確定し通しプレイ可能 |

---

## 9. 次のアクション

### 進捗 (2026-08-07 時点)

本計画は `feat/vdm` 系の作業ツリーで着手され、成果を本リポジトリ `main` へ
移植・実機検証した。移植の経緯は
[`../wintree_port/PORT_PLAN.md`](../wintree_port/PORT_PLAN.md) を参照。

| 項目 | 状態 | 備考 |
|------|------|------|
| **M0: ステージA (エンジン拡張)** | **達成** | turn 35/35・rpg 60/60・save 15/15 が実機 PASS |
| ステージB Phase 0 (足場) | **達成** | `programs/apps/game/` が起動し microUI 窓とメインループが回る |
| ステージB Phase 1 (盤面と移動) | **一部** | 実マップ170マスを `board.db` に投入済み、サイコロ→移動→盤面描画まで動作。分岐選択・移動アニメは未実装 |
| Phase 2 以降 | 未着手 | — |

補足: 移植先ディレクトリは本文の `programs/game/` ではなく
**`programs/apps/game/`** になっている (`apps/` 配下へ集約する規約に合わせたもの)。
また `gen_board_db.py` は実マップ用 (`board.db`) とテスト用 (`board_test.db`) を
`--game` / `--test` で出し分ける。`libos32board` のテストが12マスの固定
トポロジを前提としており、実マップと同居できないため。

### 次のアクション

1. **ステージB Phase 1 の残り**: 分岐選択UIと移動アニメーション。
2. **ホスト側ゴールデン値生成の整備**: DOSロジック(または `docs/19` のテスト)を gcc でビルドし、`player.c`/`battle.c`/`village.c` の期待値を出力 → ステージBの照合に使う。
3. **ステージB Phase 2 (戦闘)** へ着手。

すでに `libos32rpg` / `libos32turn` / `libos32save` が揃っているため、
Phase 2 以降の依存 (戦闘後の成長・手番進行・セーブ) は解消済み。

---

## 付録A. 参照

- **エンジン拡張実装計画書(別冊): `docs/tasks/game/ENGINE_EXTENSION_PLAN.md`**(ステージAの詳細)
- ゲーム設計: `sample/game/docs/00〜20`(`docs/INDEX.md` が索引)
- エンジンlib設計書: `docs/tasks/libboard/` `docs/tasks/libbattle/` `docs/tasks/libecon/` ほか
- エンジンlibヘッダ: `programs/libos32board/libos32board.h` ほか各 `libos32*/*.h`
- 使用例(=APIリファレンス): `programs/tests/board_test.c` / `btl_test.c` / `econ_test.c` /
  `inv_test.c` / `ai_test.c` / `evt_test.c`
- プログラム雛形: `programs/apps/ui_demo/ui_demo.c`
- KAPI定義: `include/os32_kapi_shared.h`(関数表は自動生成 `os32_kapi_generated.h`)
- DB生成: `tools/gen_*_db.py`, 出力先 `assets/*.db`
- ビルド: ルート `Makefile`(外部プログラムは `-3s`)、KAPI拡張手順は `KAPI_SPEC.md`
