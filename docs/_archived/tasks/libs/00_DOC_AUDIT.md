# libos32 設計書 整合性監査 — 調査結果

*調査日: 2026-06-05 / 対象: [`docs/libs/`](../../libs/) 配下 21 ライブラリ・40 ファイル*

> `docs/libs/` 配下の全ライブラリ設計書 (README.md / DESIGN.md / その他) を精読し、
> **整合性・機能重複・コンフリクト・依存関係**の観点で横断監査した結果を記録する。
> 修正方針は [`01_DOC_FIX_PLAN.md`](01_DOC_FIX_PLAN.md) を参照。
>
> **本監査はドキュメント本文のみを対象**とする (実装コードとの突合は別タスク)。
> 検出された問題はすべてドキュメント側の修正で解消できる。

---

## 0. 監査範囲と方法

- **対象**: `docs/libs/libos32*/` の README.md・DESIGN.md・ESTATE_DESIGN.md・INDEX.md・TODO.md・最適化メモ (02〜06)
- **方法**: 全文精読 → 依存関係図・API一覧・命名・相互参照リンク・実装状況を抽出し相互突合
- **確認済み (問題なし)**: 各 DESIGN.md から `../../KAPI_SPEC.md` 等 `docs/` 直下ファイルへの相対リンクは全て正しく解決する (`docs/KAPI_SPEC.md` 等に実在)

### 重要度の凡例

| 記号 | 意味 |
|------|------|
| 🔴 高 | 依存関係の相互矛盾・循環。設計の正しさに直結し、実装時に破綻する |
| 🟠 中 | リンク切れ・命名不統一・機能境界の未定義。読者の誤読・将来の仕様分岐を招く |
| 🟡 低 | テンプレート不統一・軽微な相互参照欠落 |

---

## 1. サマリ

| ID | 分類 | 重要度 | 概要 | 主な該当ファイル |
|----|------|--------|------|-----------------|
| DEP-1 | 依存 | 🔴 | ai ↔ board が相互に「相手は自分の消費者」と記載 (循環) | ai, board |
| DEP-2 | 依存 | 🔴 | ecs↔asset の依存方向が矛盾 (asset は基盤と自称、ecs は asset を下流扱い) | ecs, asset |
| DEP-3 | 依存 | 🟠 | 廃止済み libpyxel への現役依存記述が残存 | chem, math (vs ecs, input) |
| DEP-4 | 依存 | 🟠 | 依存図の矢印方向・凡例が文書間で不統一、一部は誤った依存方向を示唆 | ai, battle, event, chem, econ, ecs |
| DEP-5 | 依存 | 🟡 | ecs は input 依存を宣言するが input 側は ecs を消費者に挙げず・ECSコアの input ハード依存に疑問 | ecs, input |
| CON-1 | 整合 | 🟠 | tilemap/INDEX.md のアクティブ文書リンク 2 件が旧採番で切れ | tilemap/INDEX |
| CON-2 | 整合 | 🟠 | `libtilemap` と `libos32tilemap` の名称不統一 | asset, ecs, chem, input, math, tilemap |
| CON-3 | 整合 | 🟠 | `libos32gui` と実在の `libos32ui` の名称不一致 (別物か綴り違いか不明) | input |
| CON-4 | 整合 | 🟡 | 策定日行・文書テンプレートの不統一 | chem, econ (vs 他) |
| DUP-1 | 重複 | 🟠 | inv ↔ econ の売買・クラフトが二重実装、境界・相互参照が未定義 | inv, econ |
| DUP-2 | 重複 | 🟠 | inv_lottery が ai_weighted_pick を再実装 (event は再利用済み) | inv, ai |
| DUP-3 | 重複 | 🟠 | 属性 (Element) ビットフラグ定義が chem/battle で重複、共有依存なし | chem, battle |
| MIN-1 | 補足 | 🟡 | 未実装 (asset/ecs) を既成連携前提で参照する文書が複数 | ecs, econ, text |
| MIN-2 | 補足 | 🟡 | ECS カスタムビット bit10 (0x0400) を text/econ/ecs例 が各々独立確保し衝突 | text, econ, ecs |
| MIN-3 | 補足 | 🟡 | ファイルI/O・RAMキャッシュ重複は asset が認識済みだが集約 (Phase 3) 未着手 | asset, map, chem, snd, tilemap |

---

## 2. 依存関係のコンフリクト

### DEP-1 🔴 ai ↔ board の循環依存

両者が互いを「下流の消費者」として記載しており、依存方向が真逆。

- [`libos32ai/DESIGN.md:52`](../../libs/libos32ai/DESIGN.md) … `├── libos32board (方向選択判断を委ねる)` → **board が ai に依存**
- [`libos32board/DESIGN.md:40`](../../libs/libos32board/DESIGN.md) … `├── libos32ai (…マス情報を提供)` → **ai が board に依存**

**影響**: 依存グラフが循環し、ビルド順序・リンク順序が決定不能。実体は「ゲーム本体が board からマス情報を取得し ai に渡す」構造で、ai/board 間に直接依存はないはず。どちらかの図が誤り。

### DEP-2 🔴 ecs ↔ asset の依存方向矛盾

- [`libos32ecs/DESIGN.md:31-37`](../../libs/libos32ecs/DESIGN.md) … ecs が `libos32asset / libos32chem / libos32map / libtilemap / libos32snd` を**ecs の下流 (ecs に依存)** として列挙
- [`libos32asset/DESIGN.md:70-79`](../../libs/libos32asset/DESIGN.md) … asset は **KAPI のみに依存する最下層ライブラリ**で、map/tilemap/snd は asset の消費者だと明記

asset 自身が「最下層・KAPIのみ依存」と宣言しているのに、ecs は asset を ecs の下流に置いており両立しない。chem も「math+db のみ」([`libos32chem/DESIGN.md:19-27`](../../libs/libos32chem/DESIGN.md)) で ecs 非依存。
さらに ecs §5.1 自身が chem 連携を「ブリッジ方式・chem側無変更」と記すのに、依存図では chem を下流扱いしており内部矛盾。

**影響**: ecs の「下流リスト」は実態 (各ライブラリが宣言する低レベル依存) と矛盾。ecs は本来「ゲーム層で各ライブラリを束ねる協調レイヤ」であり、それらが ecs に依存するわけではない。

### DEP-3 🟠 廃止済み libpyxel への依存記述の残存

- [`libos32ecs/DESIGN.md:438`](../../libs/libos32ecs/DESIGN.md) … 「**libpyxel は廃止**」
- [`libos32input/DESIGN.md:33`](../../libs/libos32input/DESIGN.md) … 「libpyxel は廃止方向」(整合)
- しかし [`libos32chem/DESIGN.md:371`](../../libs/libos32chem/DESIGN.md) … Phase 4 に「**libpyxel** / libtilemap との連携設計」が残存
- [`libos32math/DESIGN.md:8`](../../libs/libos32math/DESIGN.md), [`:70`](../../libs/libos32math/DESIGN.md) … libpyxel を**現役の利用者**として列挙

**影響**: chem と math が廃止済みライブラリを将来連携先・現役消費者として記述したまま。

### DEP-4 🟠 依存図の記法・矢印方向の不統一

凡例 (矢印が「依存する」か「される」か) が文書群で割れている。

- **A群** (ai/battle/board/inv/event): `^` で依存先を**上**、消費者を下に配置
- **B群** (chem/econ/text): `↑` で「依存なしの math を最上段」に置き、最下層の libos32db を被依存ライブラリの**下**に配置
  - 例 [`libos32econ/DESIGN.md:19-25`](../../libs/libos32econ/DESIGN.md) は libos32db が econ の下に矢印で繋がり「db が econ に依存」と読めてしまう (実際は逆)
- **C群** (ecs): `◄━━` 記法

加えて、A群の ai/battle/event 図では libos32db を libos32math の**上**に `^` で置き、「**math が db に依存**」と読めるが、math は [`「依存なし・最も基底」`](../../libs/libos32math/DESIGN.md) と明言 (`libos32math/DESIGN.md:64`)。db と math は対象ライブラリの独立した依存先 (兄弟) であって連鎖ではない。
また [`libos32chem/DESIGN.md:25-28`](../../libs/libos32chem/DESIGN.md) は依存図に `libos32gfx (描画は使わない)` を含めるが、本文「gfx には依存しない」と矛盾。

**影響**: 依存図が読者ごとに別解釈になり、一部は実際の依存方向を誤って示している。

### DEP-5 🟡 ecs の input 依存と input 側の消費者リストの食い違い

- [`libos32ecs/DESIGN.md:30`](../../libs/libos32ecs/DESIGN.md) … ecs は `(math + input + KAPI)` を宣言
- [`libos32input/DESIGN.md:74-77`](../../libs/libos32input/DESIGN.md) … 消費者は gui/tilemap/map/ゲーム本体のみ (**ecs 不在**)

加えて ecs §5.3 では入力連携は「ゲーム側の `sys_player_input` System が行う」とあり、ECS コア自体が input に**ハード依存**する必要性が疑問 (過結合の可能性)。

---

## 3. 整合性 (リンク・命名・メタ情報)

### CON-1 🟠 tilemap/INDEX.md のリンク切れ (2 件)

- [`libos32tilemap/INDEX.md:9`](../../libs/libos32tilemap/INDEX.md) … `01_TILEMAP_DESIGN.md` → 実体は `DESIGN.md`
- [`libos32tilemap/INDEX.md:10`](../../libs/libos32tilemap/INDEX.md) … `07_TODO.md` → 実体は `TODO.md`

アーカイブ側 (02〜06) のリンクは正しく、アクティブ 2 件だけが旧採番のまま。

### CON-2 🟠 名称不統一: `libtilemap` vs `libos32tilemap`

ディレクトリ・README・INDEX は `libos32tilemap` だが、多数の文書が短縮形 `libtilemap` を使用:
[`asset:18`](../../libs/libos32asset/DESIGN.md) / [`ecs:31-36`](../../libs/libos32ecs/DESIGN.md) / [`chem:371`](../../libs/libos32chem/DESIGN.md) / [`input:75`](../../libs/libos32input/DESIGN.md) / [`math:8,70`](../../libs/libos32math/DESIGN.md) / 自身の [`tilemap/DESIGN.md:377`](../../libs/libos32tilemap/DESIGN.md) Phase 1 も `libtilemap`。

### CON-3 🟠 名称不一致: `libos32gui` vs `libos32ui`

- [`libos32input/DESIGN.md:74`](../../libs/libos32input/DESIGN.md) … `libos32gui (v1.1 イベントループ基盤)`
- 実在の GUI ライブラリは microUI 移植版の **`libos32ui`** ([`libos32ui/README.md`](../../libs/libos32ui/README.md))

別物 (将来のシェル) なのか綴り違いなのか文書から判別不能。明示が必要。

### CON-4 🟡 策定日・文書テンプレートの不統一

大半の DESIGN は冒頭に `*策定: 2026-04-28*` (math のみ 04-27) を持つが、
[`chem/DESIGN.md`](../../libs/libos32chem/DESIGN.md) と [`econ/DESIGN.md`](../../libs/libos32econ/DESIGN.md) は策定日行が無く、構成も「## 1. 概要」始まりで他と異なる。

---

## 4. 機能の重複・冗長性

### DUP-1 🟠 inv ↔ econ の売買・クラフト二重実装 (境界未定義)

- **inv**: [`inv_shop_buy/sell`](../../libs/libos32inv/DESIGN.md) (`:190`), [`inv_get_price/sell_price`](../../libs/libos32inv/DESIGN.md) (`:186`), [`inv_can_craft/inv_craft`](../../libs/libos32inv/DESIGN.md) (`:278`, items.db に `recipes`), `inv_lottery`
- **econ**: [`econ_buy/sell`](../../libs/libos32econ/DESIGN.md) (`:386`), [`econ_haggle`](../../libs/libos32econ/DESIGN.md) (`:390`), [`econ_can_craft/econ_craft`](../../libs/libos32econ/DESIGN.md) (`:428`, econ.db に `recipes`+`recipe_materials`)

買売・クラフトが両ライブラリに別レシピテーブル付きで存在。さらに [`econ_can_craft(const void *inventory)`](../../libs/libos32econ/DESIGN.md) (`:428`) は明らかに inv の `InvBag` 連携を想定しているのに依存・相互参照の宣言なし。
board が map との棲み分けを、econ が chem との対比を明示しているのに対し、**inv↔econ の境界だけ無言**で将来の仕様分岐リスクが高い。

### DUP-2 🟠 inv_lottery が ai_weighted_pick を再実装

- [`libos32ai/DESIGN.md:192`](../../libs/libos32ai/DESIGN.md) … `ai_weighted_pick(ids, weights, count)` が汎用の重み付き抽選を提供 (用途に「ドロップ判定」と明記)
- [`libos32inv/DESIGN.md:203`](../../libs/libos32inv/DESIGN.md) … `inv_lottery` は `lottery_tables.weight` で同じ重み付き抽選を独自実装
- [`libos32event/DESIGN.md`](../../libs/libos32event/DESIGN.md) は ai_weighted_pick を正しく再利用しているのに inv は db のみ依存で重複実装。再利用方針が不統一。

### DUP-3 🟠 属性 (Element) ビットフラグ定義の重複

- [`libos32chem/DESIGN.md:40-50`](../../libs/libos32chem/DESIGN.md) … `ELEM_FIRE…ELEM_WIND` (u32) を定義
- [`libos32battle/DESIGN.md:70`](../../libs/libos32battle/DESIGN.md) … `elements`「**libos32chemと同形式**」+ 属性相性表を持つが、battle の依存は db+math+ai で **chem 非依存**

「同形式」を謳いながら共有ヘッダの依存関係がなく、`ELEM_*` 定数が二重定義になるか暗黙結合になる。

---

## 5. 補足 (軽微)

### MIN-1 🟡 未実装ライブラリを既成連携前提で参照

asset ([Phase 1-4 全て未チェック](../../libs/libos32asset/DESIGN.md), `:459-486`) と ecs ([全Phase未チェック](../../libs/libos32ecs/DESIGN.md), `:389-415`) はまだ実装 0 だが、他文書 (ecs→asset、[`econ→ECS`](../../libs/libos32econ/DESIGN.md) `:45`、[`text→ECS`](../../libs/libos32text/DESIGN.md) `:294`) が既成連携前提で記述。実装状況とのギャップ注記が望ましい。

### MIN-2 🟡 ECS カスタムビット bit10 (0x0400) の衝突

[`text/DESIGN.md:304`](../../libs/libos32text/DESIGN.md) の `COMP_TEXT=0x0400` と、ecs の [`カスタムビット例 COMP_INVENTORY=0x0400`](../../libs/libos32ecs/DESIGN.md) (`:133`)、econ の COMP_ECON 連携が、いずれも独立に bit10 を確保。ecs の登録制で実害は回避されるが、複数アダプタ併用時に要調整。

### MIN-3 🟡 ファイルI/O・RAMキャッシュ重複 (asset が認識済み・集約未着手)

[`asset/DESIGN.md:14-30`](../../libs/libos32asset/DESIGN.md) が map/chem/tilemap/snd の各自実装を集約する Phase 3 を計画しているが未着手。当面は重複が残る旨を各ライブラリ側にも注記すると良い。

---

*調査結果 — 2026-06-05 / 続き: [`01_DOC_FIX_PLAN.md`](01_DOC_FIX_PLAN.md)*
