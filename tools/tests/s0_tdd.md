# S0 — 設定レジストリ基盤の検証記録

票ごとに節を分ける (S0-K = `## K.`、S0-D = `## D.`、S0-T = `## T.`)。

## T. 初期値 tsv / 生成ツール / ビルド統合 (S0-T、2026-09-13)

### 範囲と契約

`assets/settings/defaults.tsv` (初期値の正典)、`tools/mk_settings_db.py` (tsv → DB)、
`build/assets.mk` / `build/image.mk` / `build/core_packages.yaml` (ビルド統合と媒体添付)、
`tools/mkpkg.py` (登録ファイルの欠損をエラーに) だけ。生成 DB は媒体 (FDD / CD) だけが持ち、
既存システムには tsv を通常配備する (TASK_S0 §3)。カーネル・KAPI・配備スクリプトは触らない。

試験: `python3 -B tools/tests/test_mk_settings_db.py` (38 件)。ホストのみ。エミュレータ・配備・`make`
(dry-run `-n` を除く) は実行していない。

### 実行済み RED → GREEN

1. **RED (1 回目)** — tsv / 生成ツールを書いた直後に全 38 件を実行し、5 件が失敗:
   - `BuildWiring.test_assets_mk_rule` / `test_image_mk_wiring` /
     `test_core_packages_has_settings_db` — ビルド統合が未実装 (期待どおりの RED)。
   - `MkpkgMissingFile.test_missing_file_is_error` — `exit 0` で
     `MINIMAL.PKG: 0 files, 43 bytes`。欠損が warning で素通りしていた (期待どおりの RED)。
   - `Rejects.test_text_255_boundary` — 試験側の欠陥。1 メソッド内で出力名 `out.db` を使い回し、
     直前の成功ビルドの残骸を「失敗なのに DB を作っている」と誤判定していた。出力名を毎回変えて修正。
   決定性・スキーマ・規則違反の拒否 (33 件) は 1 回目から通った。**tsv と生成ツールは試験より先に書いた**
   ので、この 33 件については RED を経ていない (正直に記録する)。
2. **GREEN** — `tools/mkpkg.py` を「全パッケージのファイルを先に解決し、欠損があれば 1 つも書かずに
   非ゼロ終了」に変更、`build/assets.mk` に `$(BUILD_OUT)/settings.db` (FORCE 依存) を追加、
   `build/image.mk` の FDD / `packages` に結線、`build/core_packages.yaml` の `minimal` に
   `/etc/settings.db` を追加 → 38 件すべて PASS。

### 見たもの (合格の中身)

- **決定性**: mtime の違う同内容 2 ファイル + 同じ epoch → 同一 sha256。同じ入力を 2 回 → 同一。
  epoch 違い / `SOURCE_DATE_EPOCH` / 既定 0 の順序も確認。入力の mtime は読んでいない。
- **スキーマ**: `meta(schema_version=1, created)`、`settings(scope,key,type,ival,tval,bval)` +
  `PRIMARY KEY(scope,key)` + `WITHOUT ROWID`、`page_size=1024`、`user_version=1`、
  `journal_mode=delete`。`integrity_check=ok`、`freelist_count=0` (VACUUM 後)。実物は 3 KB / 3 行。
- **規則**: int32 超過 / int の字句 / 不正 UTF-8 / NUL (scope・text) / key 規則と 63B / scope 規則と
  63B / type / 重複 / 列数 / CR / text 255B 境界 (3B 文字を含む) / blob の奇数桁・空白・非 hex・
  8192 文字超 — すべて非ゼロ終了 + 理由 (`path:line:`) で、DB を作らない。境界値 (int32 端、key 63B、
  blob 4096B) は受理。末尾の空欄は NULL ではなく空の text として入る。
- **mkpkg**: 欠損 = 非ゼロ + `ERROR: <path> not found (package '<name>')`、途中まで書いた `.PKG` を
  残さない。glob が 0 件なのは欠損扱いにしない (parser は変えていない)。
- **dry-run** (`make -n all` / `-n iso` / `-n images/os32_boot.d88`): `mk_settings_db.py` は 1 回だけ
  走り、`mkfat12.py` / `mkpkg.py` より前。`packages` の依存に結んだ既存入力
  (`programs` / `boot` / `vmkernel.lz4` / `unicode_bin` / `assets/fep.db`) も mkpkg より前に生成される。
- `python3 -B tools/check_constraints.py` → OK (規則 16 件)。

### 見ていないもの

- ゲストでの受入 T1 (媒体の中身を mount / mkpkg 一覧で確認、実機で `sqlite3` 読み出し) — `make` と
  配備が禁止のため未実施。ホスト側では `tools/mkfat12.py` に `/etc/settings.db=` を渡した
  smoke ビルドで FAT12 に 3 KB / 3 クラスタとして載ることまで確認した。
- 新規インストールでの seed (FDD の `install.bin` が `/kernel.bin` を要求する既存不整合、B10) は
  S3 の受入。本票では「媒体に入っている」までしか言えない。
