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

### [ ] `programs/apps/mdview.c` 分割 (1278行)

**現状の問題**:
- UI/ページング (ページスクロール・キー操作): 約350行
- 検索機能: 約150行
- 目次(TOC)ジャンプ機能: 約100行
- MD描画ロジック: 約400行

**方針**:
- `programs/libmd/md_render.c` を新設し、描画ロジックを移管
- `mdview.c` を 600行以下に削減
- `Makefile` の `mdview.elf` ターゲットに `md_render.o` を追加

**依存関係**:
- `programs/libmd/libmd.h` へのインタフェース追加が必要
- `libmd/md_parse.c` とは独立した描画レイヤーとして実装

---

### [ ] `programs/shell/cmd_filer.c` リファクタリング (812行)

**現状の問題**:
- `fl_draw_*` 系の描画関数群がモノリシックに集中 (300行超)
- `libfiler/filer_core.c` (343行) が既に抽出されているが、cmd_filerは独自実装を維持

**方針案1** (推奨): 描画・レイアウト部分を `libfiler/` に移管
- `libfiler/filer_draw.c` を新設
- `cmd_filer.c` はイベントループと shell統合部のみを残す (目標: 400行以下)

**方針案2**: 現状維持 (コードが安定しているため)

**確認事項**: libfilerへの移管はシェルのビルドに `libfiler` リンクが必要になるため、`Makefile` の `shell.elf` ターゲット更新が必要。

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
