# S0 — 設定レジストリ基盤のホスト TDD 記録

票: [`docs/tasks/settings/TASK_S0.md`](../../docs/tasks/settings/TASK_S0.md)、
契約の正典は [`docs/tasks/settings/S0_FOUNDATION.md`](../../docs/tasks/settings/S0_FOUNDATION.md)。

3 票 (S0-K / S0-D / S0-T) が同じファイルに書くので、節を票ごとに分ける。
**自分の節だけ**を書き、他の節には触れないこと。

- `## K.` — KAPI v50 と回収順序 (S0-K)
- `## D.` — 通常配備の settings 保護 (S0-D)
- `## T.` — 初期値 tsv と生成ツール (S0-T)

---

## D. 通常配備の settings 保護 (D0) — 2026-09-13

### D.1 何を止めたいか

`/etc/settings.db` は**ゲストが書く**もので、ホストのビルド成果物ではない。
ところが配備は 4 系統あり、どれか 1 つでも掴むとユーザーの設定が消える。

| 系統 | 入口 | 消し方 |
|---|---|---|
| マニフェスト配備 | `nhd_deploy.do_sync` / `hostdrv_deploy.do_sync` | `cp` / `copy2` が `O_TRUNC` 相当で上書き |
| HostDrv 丸写し | `nhd_deploy.do_sync_from_hostdrv`、ゲストの `hsync` | HostDrv に残った古い `etc/settings.db` が本体を切り詰める |
| 掃除 | `prune_stale`、`hostdrv_deploy.do_clean` | `rmtree(root/etc)` は判定の余地なく木ごと落とす |
| イメージ全体 | `nhd_deploy.do_deploy` | 古い local NHD で remote を丸ごと上書き = 稼働中に書かれた設定が消える |

ファイル単位の除外だけでは 4 番目が残る。そこで pull の**来歴** (`<local>.pulled`) を
足し、`deploy` は来歴が崩れていないときだけ全体を書く。

### D.2 試験の範囲と遮断

- `tools/tests/test_deploy_protect.py` — temp dir + mock。`subprocess.run` を
  `FakeRun` に差し替え、`mount` / `umount` / `losetup` / `mkfs.ext2` / `e2fsck` /
  `taskkill.exe` が呼ばれたら **AssertionError で試験を落とす**。`cp` / `rm` /
  `mkdir` / `sync` だけを temp dir の中で模擬する。import 時に `.env` を読ませない
  よう、`NP21W_DIR` / `HOSTDRV_DIR` / `OS32_NHD_LOCAL` を temp へ向けてから import する。
- `tools/tests/test_hsync_protect.py` + `hsync_protect_host.c` — ゲスト側の字句判定
  (`userland/system/hsync_protect.inc`) を**実物のソースのまま** ホストで走らせ、
  同じソースが `i386-elf-gcc` + PROGRAM_FLAGS でも通ることを別に見る ([C1])。
- エミュレータ・実配備・`make` には一切触れていない ([V4])。

### D.3 RED → GREEN

RED は実装前の `tools/nhd_deploy.py` / `hostdrv_deploy.py` / `prune_stale.py`
(feat/gui `775fc4a` の版) に対して、新しい試験だけを当てて取った。
`tools/deploy_protect.py` は新規モジュールなので RED 時も置いてある
(だから「判定そのもの」の 13 件は RED でも通る = 差分は**適用点**にある)。

```
RED  : Ran 53 tests — FAILED (failures=26, errors=9)     18 passed
GREEN: Ran 53 tests — OK
```

RED で落ちた代表 (票 §2 の項目との対応):

| 試験 | RED の症状 | 票 |
|---|---|---|
| `NhdSync.test_protected_source_is_skipped_and_db_unchanged` | manifest の `etc/settings.db` が別内容で上書きされた | (1) |
| `NhdSync.test_missing_db_stays_missing` | 欠損していた DB を通常配備が**作った** | (1) |
| `NhdSync.test_directory_style_guest_path` / `test_dotdot_detour_is_blocked` | `guest: /etc/` と `..` 経由で素通り | (3) |
| `SyncFromHostdrv.test_stale_hostdrv_db_does_not_truncate_nhd_db` | HostDrv の残骸が NHD の本体を潰した | (3) |
| `SyncFromHostdrv.test_protected_directory_in_hostdrv_is_not_created` | `etc/settings.db-wal/` を mkdir した | (3)、往復 3 の 4 |
| `NhdCli.test_copy_*` / `test_rm_cannot_remove_protected` | CLI から DB を書ける・消せる | (3) |
| `HostdrvSync.test_clean_keeps_protected_and_its_ancestors` | `rmtree` が `/etc` ごと落とした | (3)、往復 3 の 4 |
| `HostdrvSync.test_protected_is_judged_before_content_comparison` | 保護対象を内容比較のために開いていた | (1) |
| `Prune.test_prune_*_keeps_protected` | 掃除が保護対象を消した | (3) |
| `NhdSync.test_sync_failure_is_not_success` ほか | `sync` / `rm` / `cp` の失敗が成功として返った | (4) |
| `MainExit.*` | `copy` / `rm` / `umount` の失敗が exit 0 のまま後続へ進んだ | (4) |
| `Stamp.*` (7 件) | 来歴が無い / local が別物 / remote が書き換わっていても全体上書きした | (5) |

「判定そのもの」(`ProtectJudgement`、13 件) は RED でも GREEN でも通る。名前規則を
realpath の**前**に置くこと (dangling symlink / 欠損でも守る)、`stat` の失敗は
**ENOENT だけ不存在**として扱い他は `ProtectError` で配備を止めること、
`<root>/etc` が symlink / 別マウントなら**配備全体を拒否**すること、hardlink の
別名を `st_dev`/`st_ino` で拾うことを、この 13 件が固定している (往復 2 の 6、
往復 3 の 3)。

### D.4 hsync (ゲスト側)

```
HOST GNU89 -Werror COMPILE PASS
  28 checks / 0 failures        (正規化 7、名前規則 11、通してよいもの 6、-f 連結 4)
TARGET i386-elf GNU89 COMPILE PASS   (hsync.c を PROGRAM_FLAGS で)
```

`./` / `..` / 連続 `/` / 相対 / 末尾 `/` (mkdir 経路) / 大文字 / `hsync -f etc` の
subdir 連結の各形で `/etc/settings.db*` に届くことを確かめてある。正規化に失敗する
入力 (root を越える `..`) は**保護側**へ倒す。実体規則 (同一 inode の hardlink を
コピーしない) は `sys_stat` が要るので `hsync.c` 側にあり、ここでは見ていない
(ゲスト受入 D1 で見る)。

### D.5 この試験が言えないこと ([V4])

- 実機 / エミュレータでの D1 受入 (HostDrv に意図的に `etc/settings.db` を置いて
  `make deploy` → `hsync -f etc` → 停止 → `pull` → `make deploy-nhd`) は**未実施**。
  コーダーは配備・エミュレータ・`make` を実行しない。
- `hsync` の inode 比較 (実体規則) はホストでは踏んでいない。
- `sudo` / ループマウントの実挙動は模擬。sudoers や ext2 の振る舞いは何も言わない。
- bind mount による `<root>/etc` の別名は原理的に検出できない (運用で禁止し、
  symlink / 別マウントだけを `check_root_etc` が止める)。

## T.`)。

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
