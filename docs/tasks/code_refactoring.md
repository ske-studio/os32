# コードリファクタリング TODO

> 作成: 2026-04-23  
> 目的: 肥大化ファイルの分割・重複コードの解消

---

## 優先度: 高

### ✅ 完了: `apps/filer.c` 削除
- `programs/apps/filer.c` (687行) は `programs/shell/cmd_filer.c` と重複していたため削除。
- スタンドアロン `filer.bin` のデプロイ廃止。
- Makefile / deploy.yaml から関連エントリ除去。

### ✅ 完了: `_backup/` 削除
- `_backup/hostdrvfs_v1/` (旧HostDrvFS v1残骸) を削除。

---

## 優先度: 中

### ✅ 完了: `programs/apps/mdview.c` 分割 (1279行 → 517行)

**実施内容**:
- 描画ロジック (ワードラップ、レイアウト計算、ノード描画、ページ描画、ステータスバー) を `programs/libmd/md_render.c` (722行) に分離。
- `programs/libmd/md_render.h` (129行) にAPIを公開。
- `mdview.c` はUI/ページング/検索/目次/ファイラー連携のみを担当 (517行)。
- Makefile に `md_render.o` のビルドルールとリンク設定を追加。

---

### ✅ 完了: `programs/shell/cmd_filer.c` リファクタリング (813行 → 476行)

**実施内容** (方針案1を採用):
- 描画・レイアウト関数群 (`fl_draw_*`, `fl_clear_line` 等) を `programs/libfiler/filer_draw.c` (300行) に分離。
- `programs/libfiler/filer_draw.h` (117行) にAPIとデータ構造 (`FL_State`, `FL_Entry`) を定義。
- `cmd_filer.c` はイベントループ、ディレクトリ走査、ファイルタイプ関連付け、シェル統合のみを担当 (476行)。
- Makefile で `shell.elf` に `filer_draw.o` をリンクするよう更新。

---

## 優先度: 低

### [ ] `programs/shell/main.c` 実行エンジン分離 (723行)

**現状の問題**:
- `execute_command()` 周辺のパイプライン・リダイレクト処理が main.c に集中
- 実行エンジン本体が 300行超

**方針**:
- `programs/shell/cmd_exec.c` を新設し、パイプライン・リダイレクト処理を移管
- `main.c` はコマンド登録・PATH解決・グロブ展開のみを残す (目標: 400行以下)

---

## 放置 (現時点では対応しない)

### `programs/tests/pyxel_test.c` (1050行)
- アクティブなテストコードのため分割は後回し。
- libpyxelのAPIが安定してから分割を検討する。
