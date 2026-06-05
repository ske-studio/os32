# libos32 設計書 整合性 — 修正計画

*策定: 2026-06-05 / 調査結果: [`00_DOC_AUDIT.md`](00_DOC_AUDIT.md)*

> [`00_DOC_AUDIT.md`](00_DOC_AUDIT.md) で検出した 15 件 (DEP-1〜5 / CON-1〜4 / DUP-1〜3 / MIN-1〜3) を
> ドキュメント側の修正で解消するための実行計画。**実装コードの変更は伴わない**。
> リスクの低い機械的修正から着手し、最後に設計判断を要する依存グラフ・機能境界の正規化を行う。

---

## 0. 方針

1. **ドキュメントのみ完結**: 全件 `docs/libs/` 配下の `.md` 編集で解消。ビルド・実機テスト不要。
2. **低リスク先行**: リンク切れ・名称統一 (機械的) → 依存グラフ正本化 (設計判断) → 機能境界・共通化 (要合意)。
3. **正本の一元化**: 散在する依存図を 1 枚の正本 ([依存グラフ正本](#22-依存グラフ正本案)) に集約し、各 DESIGN はそれを参照する形に簡素化する。これが DEP-1〜5・CON-2 の根本対策。
4. **要判断項目は本計画で選択肢を提示**し、未確定のまま編集しない (DUP-1 の責務分担、CON-3 の gui/ui 別物判定)。

---

## 1. フェーズ構成

| マイルストーン | 対象 | 性質 | 前提 |
|---------------|------|------|------|
| **M1** 機械的修正 | CON-1, CON-2, DEP-3 | 文字列置換中心・低リスク | なし |
| **M2** 依存グラフ正本化 | DEP-1, DEP-2, DEP-4, DEP-5, CON-2 | 設計判断あり | M1 完了 |
| **M3** 名称・テンプレ統一 | CON-3, CON-4, MIN-1 | 一部は要確認 | M2 |
| **M4** 機能境界・共通化 | DUP-1, DUP-2, DUP-3, MIN-2, MIN-3 | 要合意 | M2 |

M1 と M3 の一部 (CON-4) は独立着手可。M4 は M2 の依存正本が決まってから。

---

## 2. M1 — 機械的修正 (低リスク)

### CON-1: tilemap/INDEX.md リンク切れ修正
- [ ] [`libos32tilemap/INDEX.md:9`](../../libs/libos32tilemap/INDEX.md) … `01_TILEMAP_DESIGN.md` → `DESIGN.md`
- [ ] [`libos32tilemap/INDEX.md:10`](../../libs/libos32tilemap/INDEX.md) … `07_TODO.md` → `TODO.md`
- [ ] あわせてリンク表の No. 列 (01/07) を実態に合わせるか、注記を追加

### CON-2: `libtilemap` → `libos32tilemap` 一括統一
正式名称を `libos32tilemap` に確定し、全 `libtilemap` 出現を置換:
- [ ] [`asset/DESIGN.md`](../../libs/libos32asset/DESIGN.md) (`:18` 他、§1.1 表・§2.2・Phase 3)
- [ ] [`ecs/DESIGN.md`](../../libs/libos32ecs/DESIGN.md) (`:31-36`, `:362`, §5.4)
- [ ] [`chem/DESIGN.md`](../../libs/libos32chem/DESIGN.md) (`:371`)
- [ ] [`input/DESIGN.md`](../../libs/libos32input/DESIGN.md) (`:75`)
- [ ] [`math/DESIGN.md`](../../libs/libos32math/DESIGN.md) (`:8`, `:70`)
- [ ] [`tilemap/DESIGN.md`](../../libs/libos32tilemap/DESIGN.md) (`:377` 自身の Phase 1 記述)

> 注: `libos32tilemap/INDEX.md:20` の「libtilemap リファクタリング」など**過去タスク名**としての出現は、
> 歴史的記録として残すか統一するか M1 着手時に判断 (推奨: 本文は統一、過去ログは原文ママ+脚注)。

### DEP-3: 廃止済み libpyxel 記述の整理
- [ ] [`chem/DESIGN.md:371`](../../libs/libos32chem/DESIGN.md) … Phase 4「libpyxel / libtilemap との連携設計」から libpyxel を除去し `libos32tilemap` 連携に置換 (または「libpyxel は廃止済み」の脚注)
- [ ] [`math/DESIGN.md:8`](../../libs/libos32math/DESIGN.md), [`:70`](../../libs/libos32math/DESIGN.md) … 利用者リストの `libpyxel` を削除、または「(廃止)」と明記。後継の `libos32ecs/libtilemap` 利用に言及

---

## 3. M2 — 依存グラフの正本化 (設計判断)

### 2.1 アプローチ

各 DESIGN にバラバラの ASCII 依存図を置く現状が DEP-1/2/4/5・CON-2 の温床。
**全ライブラリの依存を 1 枚に集約した正本を新設**し、個別 DESIGN の図は「正本へのリンク + 自分の直接依存 1 行」に簡素化する。

- [ ] 正本を新設: `docs/libs/libos32/DEPENDENCY.md` (または `libos32/README.md` に「依存グラフ」節を追加)
- [ ] 矢印凡例を 1 つに固定: **`A → B = 「A は B に依存する」`** で全図を統一
- [ ] 各 DESIGN の §2「アーキテクチャ / 依存関係」を、正本リンク + `本ライブラリの直接依存: math, db` のような 1 行記述に置換

### 2.2 依存グラフ正本 (案)

各ライブラリの DESIGN が宣言する**直接依存**を、矛盾を解消した形で層別に整理した案。
(`A → B` = A が B に依存。`*` = DESIGN 上 Phase 未完のライブラリ)

```
L0 基盤 (依存なし / KAPI のみ)
    libos32math      (依存なし — 最基底)
    libos32db        (→ KAPI)
    libos32          (→ KAPI ; crt0 / syscalls)
    libos32input     (→ math, KAPI)
    libos32asset*    (→ KAPI)

L1 描画 / IO / データI-O
    libos32gfx       (→ math, KAPI)
    libos32snd       (→ math, KAPI)
    libos32tilemap   (→ math, gfx)
    libos32text      (→ db)
    libos32map       (→ db, tilemap)

L2 シミュレーション / ロジック
    libos32chem      (→ math, db)
    libos32econ      (→ math, db)
    libos32ai        (→ math, db)
    libos32inv       (→ db)
    libos32ecs*      (→ math, input)        ← input 依存は要再検討 (DEP-5)

L3 上位ゲームシステム
    libos32battle    (→ math, db, ai)
    libos32board     (→ db)                  ← ai 依存を撤去 (DEP-1)
    libos32event     (→ db, ai)

L4 UI / アプリ
    libos32ui        (→ gfx)
    libos32filer     (→ gfx, VFS/KAPI)
    libos32md        (→ gfx)
```

### 2.3 各コンフリクトの確定方針

#### DEP-1: ai ↔ board の循環を解消
- [ ] **board → ai 依存を撤去**: [`board/DESIGN.md:40`](../../libs/libos32board/DESIGN.md) の消費者リストから `libos32ai` を削除 (board は ai を知らない)
- [ ] [`ai/DESIGN.md:52`](../../libs/libos32ai/DESIGN.md) の `libos32board` は「**ゲーム本体が** board のマス情報を評価し ai に渡す」と注記し、直接依存ではないことを明示
- 確定: **ai と board の間に直接依存なし**。連携はゲーム本体が仲介。

#### DEP-2: ecs ↔ asset の方向を是正
- [ ] [`ecs/DESIGN.md:31-37`](../../libs/libos32ecs/DESIGN.md) の図を「ecs の下流」ではなく「**ゲーム層で ecs と連携する周辺ライブラリ**」と再定義。asset/chem/map/tilemap/snd が ecs に依存するわけではない旨を明記
- [ ] ecs の chem 連携は §5.1 の「ブリッジ方式 (chem 側無変更)」と図を一致させる
- 確定: **asset は L0 基盤** (asset/DESIGN の自己宣言を正とする)。ecs はそれらに依存しない協調レイヤ。

#### DEP-4: 依存図記法の統一
- [ ] 2.1 の凡例 `A → B = A が B に依存` に全図を統一
- [ ] A群図 (ai/battle/event) の「db を math の上に積む」表現を是正し、**math と db は対象ライブラリの並列な依存先**として描く (math は依存なし)
- [ ] [`chem/DESIGN.md:25-28`](../../libs/libos32chem/DESIGN.md) の依存図から `libos32gfx` を除去 (本文「gfx 非依存」と一致させる)

#### DEP-5: ecs の input 依存を再検討
- [ ] 方針を決定 — **(推奨)** ECS コアから input ハード依存を外し、入力連携は「ゲーム側 System が input を読んで Component に書く」設計に統一 ([`ecs/DESIGN.md §5.3`](../../libs/libos32ecs/DESIGN.md) と整合)。正本の ecs 依存を `(→ math)` に修正
- [ ] 代替案を採る場合は [`input/DESIGN.md:74-77`](../../libs/libos32input/DESIGN.md) の消費者リストに `libos32ecs` を追加し双方向で整合させる

---

## 4. M3 — 名称・テンプレート統一

### CON-3: `libos32gui` の扱い確定 (要確認)
- [ ] `libos32gui` が `libos32ui` (microUI 移植) と**同一**か、別の v1.1 GUI シェル構想か判定
  - 同一の場合: [`input/DESIGN.md:74`](../../libs/libos32input/DESIGN.md) を `libos32ui` に統一
  - 別物の場合: `libos32gui` を「将来構想 (未着手)」と明記し、`libos32ui` との関係を 1 行追記
- → 判断材料: `ROADMAP.md` の v1.1 GUI シェル記述、`libos32ui/README.md`

### CON-4: 策定日・テンプレート統一
- [ ] [`chem/DESIGN.md`](../../libs/libos32chem/DESIGN.md) / [`econ/DESIGN.md`](../../libs/libos32econ/DESIGN.md) に他と同形式の `*策定: YYYY-MM-DD*` 行を追加
- [ ] (任意) DESIGN テンプレート (概要 → 設計背景 → アーキテクチャ → … → 関連ドキュメント) の節構成を揃える

### MIN-1: 実装状況ギャップの注記
- [ ] asset / ecs を参照する文書 (ecs→asset、[`econ→ECS`](../../libs/libos32econ/DESIGN.md)、[`text→ECS`](../../libs/libos32text/DESIGN.md)) に「連携先は設計のみ・実装未完」の注記を追加

---

## 5. M4 — 機能境界・共通化 (要合意)

### DUP-1: inv ↔ econ の責務境界を明文化 (要判断)
- [ ] 境界方針を決定。**推奨案**: inv = 「プレイヤー個人の所持品・装備・店UIの売買」、econ = 「世界経済・市場価格・NPC商人・マクロ需給」。店頭価格は econ が算出し inv が参照する片方向連携
- [ ] 採用案を inv §1・econ §1 双方に 1 段落で追記し相互リンク
- [ ] クラフトのレシピテーブル二重化 (econ.db `recipes` ↔ items.db `recipes`) を一元化する方針を記載 (どちらを正とするか)
- [ ] [`econ_can_craft(const void *inventory)`](../../libs/libos32econ/DESIGN.md) (`:428`) が受け取る `inventory` を inv の `InvBag` と明記し、依存方向を確定

### DUP-2: inv_lottery を ai_weighted_pick に寄せる
- [ ] [`inv/DESIGN.md`](../../libs/libos32inv/DESIGN.md) に「抽選は `ai_weighted_pick` を利用」と設計変更を記載 (inv の依存に ai を追加)、または「ai 非依存を維持し意図的に再実装」する理由を明記
- 推奨: event と同様に ai_weighted_pick を再利用し重複を排除

### DUP-3: 属性 (Element) 定義の共有化
- [ ] `ELEM_*` ビットフラグの正本配置を決定 (推奨: 下層の共有ヘッダ — math か db 層、もしくは新規 `libos32elem.h`)
- [ ] [`chem/DESIGN.md`](../../libs/libos32chem/DESIGN.md) / [`battle/DESIGN.md`](../../libs/libos32battle/DESIGN.md) 双方に共有ヘッダ参照を記載し、battle の「chem と同形式」を「共有定義を参照」に改める

### MIN-2: ECS カスタムビット bit10 の整理
- [ ] ECS カスタムビット (bit10〜31) の**推奨割当表**を ecs/DESIGN に追加し、text=COMP_TEXT・inv=COMP_INVENTORY・econ=COMP_ECON 等が衝突しないモデル例を提示

### MIN-3: I/O・キャッシュ集約計画の相互リンク
- [ ] map/chem/snd/tilemap の各 DESIGN に「将来 asset Phase 3 で I/O 集約予定」の脚注を追加 ([`asset/DESIGN.md §1.1`](../../libs/libos32asset/DESIGN.md) へリンク)

---

## 6. 変更ファイル・マトリクス

| ファイル | M1 | M2 | M3 | M4 | 主な修正 |
|---------|----|----|----|----|---------|
| `libos32/DEPENDENCY.md` (新規) | | ● | | | 依存グラフ正本 |
| `libos32tilemap/INDEX.md` | ● | | | | リンク切れ (CON-1) |
| `libos32ai/DESIGN.md` | | ● | | | board 関係注記 (DEP-1) |
| `libos32board/DESIGN.md` | | ● | | | ai 依存撤去 (DEP-1) |
| `libos32ecs/DESIGN.md` | ● | ● | ● | ● | 名称/依存方向/input/bit割当 |
| `libos32asset/DESIGN.md` | ● | ● | | ● | 名称/依存正/I-O集約 |
| `libos32chem/DESIGN.md` | ● | ● | ● | ● | libpyxel/gfx図/策定日/Elem |
| `libos32math/DESIGN.md` | ● | ● | | | libpyxel/名称 |
| `libos32econ/DESIGN.md` | | ● | ● | ● | 依存図/策定日/inv境界/Elem |
| `libos32inv/DESIGN.md` | | | ● | ● | inv境界/抽選共通化 |
| `libos32battle/DESIGN.md` | | | | ● | Element 共有参照 |
| `libos32input/DESIGN.md` | ● | ● | ● | | 名称/gui判定/ecs整合 |
| `libos32text/DESIGN.md` | | | ● | ● | ECS未実装注記/bit |
| `libos32map`,`snd`/各 README | | | | ● | I-O 集約脚注 |
| `docs/tasks/libs/INDEX.md` | ● | | | | 本監査・計画へのリンク追加 |

---

## 7. 検証

ドキュメントのみのため自動テストは不要。完了基準:

- [ ] 全 `.md` 内の相対リンクが解決する (リンクチェック)
- [ ] `libtilemap` / `libpyxel` (現役記述) / `libos32gui` の grep ヒットが 0 か、すべて意図的な脚注付き
- [ ] 依存図が正本 1 枚に集約され、各 DESIGN は正本参照 + 直接依存 1 行のみ
- [ ] 循環依存 (ai↔board) が解消され、層 L0→L4 が一方向 DAG になっている
- [ ] inv↔econ・chem↔battle に責務境界/共有定義の相互リンクがある

### 推奨着手順

```
M1 (CON-1, CON-2, DEP-3)   → 機械的・即着手可・レビュー容易
M2 (依存グラフ正本 + DEP-1/2/4/5) → 設計の核。正本を先に作り個別図を簡素化
M3 (CON-3, CON-4, MIN-1)   → CON-3 のみ要確認、他は機械的
M4 (DUP-1/2/3, MIN-2/3)    → 要合意。境界方針の決定後に一括反映
```

---

*修正計画 — 2026-06-05 / 調査結果: [`00_DOC_AUDIT.md`](00_DOC_AUDIT.md)*
