# Phase E: ドキュメント整理 & アーカイブ

## 目的

- VDOS 名称の痕跡をドキュメントから除去
- v86 統合の決定事項を記録
- 不要になった FreeDOS カスタムカーネルをアーカイブ
- KAPI_SPEC.md を再生成

---

## E-1. ディレクトリリネーム

```
docs/tasks/vdos/ → docs/tasks/v86/
```

### 注意事項

- 既存の `docs/tasks/vdos/` 内の文書は歴史的記録として内容を保持
- ファイル名/パス内の `vdos` を `v86` に置換
- 文書の冒頭に「VDM 方針撤回ノート」を追記

### VDM 方針撤回ノート (各文書の冒頭に追記)

```markdown
> **注記 (2026-05)**: VDOS (Virtual DOS Machine) 方針は撤回されました。
> OS32 の V86 サブシステムは DOS API エミュレータを持たず、
> ゲスト OS (FreeDOS 等) が DOS API を処理する設計です。
> 名称は `v86` に統一されました。
> 詳細は `docs/tasks/v86_integration/README.md` を参照してください。
```

---

## E-2. FreeDOS カスタムカーネルのアーカイブ

### 移動

```
tools/fdkernel/nec98/ → tools/_archive/fdkernel/nec98/
```

### 残すもの

- `tools/freedos98/` — FreeDOS ディスクイメージとして使えるため残す
- `screenshot_vdos_2hd.png` 等 — 履歴保存のためリネーム不要

### `_archive` ディレクトリの README

```markdown
# _archive

アーカイブされたコンポーネント。
Git 履歴に残るため削除ではなく移動で対応。

## fdkernel/nec98/

FreeDOS(98) カスタムカーネル。
Phase 2 デバッグ中にハングで詰まり、
v86 統合リファクタリング (2026-05) で方針撤回に伴いアーカイブ。
```

---

## E-3. INDEX.md 更新

### 対象ファイル

- `docs/INDEX.md`

### 変更内容

- `vdos` への参照を `v86` に更新
- `docs/tasks/v86_integration/` へのリンクを追加
- `docs/tasks/v86/` (旧 vdos タスク) へのリンクを更新

---

## E-4. KAPI_SPEC.md 再生成

### 対象ファイル

- `docs/KAPI_SPEC.md`

### 作業

1. `kapi.json` の version (38) と関数表を反映
2. 新規 API (`sys_v86_boot_image`) の仕様を追記
3. 削除予定 API にマーク付与 (Phase 9 で削除するもの)

### KAPI 変更履歴セクション追記

```markdown
## 変更履歴

| Version | 変更内容 |
|---------|----------|
| 38 | `sys_v86_boot_image` 追加 |
| 37 | IDE KAPI削除, `dev_blk_write` 追加, `ide_get_info` 追加 |
| ... | ... |
```

---

## E-5. 新規ドキュメント

### `docs/tasks/v86_integration/` (本ディレクトリ)

Phase A〜E の決定事項を記録した正式ドキュメント群:

| ファイル | 内容 |
|---------|------|
| `README.md` | 概要・フェーズ構成・実施順序 |
| `PHASE_0_PREREQ.md` | 前提条件 (kapi.json 同期) |
| `PHASE_A_LOOP_DEV.md` | loop_dev フォーマット拡張 |
| `PHASE_B_V86_DISK.md` | v86_disk ブリッジ化 |
| `PHASE_C_SESSION.md` | セッション API 統合 |
| `PHASE_D_RENAME.md` | コマンド改名 |
| `PHASE_E_DOCS.md` | ドキュメント整理 (本ファイル) |

---

## E 全体の検証

| 項目 | 手順 | 期待結果 |
|------|------|----------|
| リンク整合性 | `docs/tasks/v86_integration/README.md` から全文書到達可能 | リンク切れなし |
| INDEX.md | `docs/INDEX.md` の全リンクが有効 | リンク切れなし |
| KAPI_SPEC | version=38 の関数表が正確 | 全関数が記載 |
| fdkernel | `tools/_archive/fdkernel/nec98/` にファイルが移動 | 元の場所に残っていない |
| freedos98 | `tools/freedos98/` が存在 | 残っている |
| ビルド | `make all` | エラー 0 (fdkernel 撤去の影響なし) |
