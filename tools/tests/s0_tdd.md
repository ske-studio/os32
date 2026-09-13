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
- `hsync` の inode 比較 (実体規則) はホストでは踏んでいない。純関数に出せる
  「/etc 列挙の一致判定」(`hsp_is_protected_basename`) までが試験の範囲。
- `sudo` / ループマウントの実挙動は模擬。sudoers や ext2 の振る舞いは何も言わない。
- bind mount による `<root>/etc` の別名は原理的に検出できない (運用で禁止し、
  symlink / 別マウント / 通常ファイルだけを `check_root_etc` が止める)。

### D.6 実装レビュー 往復 1 の blocker 9 件 (2026-09-13、着地 `2c35f7c` に対して)

RED は着地版 (`feat/gui` の `tools/deploy_protect.py` / `nhd_deploy.py` /
`hostdrv_deploy.py` / `prune_stale.py`) に新しい試験だけを当てて取った。

```
RED  : Ran 86 tests — FAILED (failures=25, errors=4)     57 passed
GREEN: Ran 86 tests — OK
```

| # | 反例 (RED で通ってしまっていた道) | 試験 |
|---|---|---|
| B1 | 判定後に `cp` / `copy2` が basename を補う。`copy --dest / --rename etc <settings.db>`、manifest の `guest: /etc` (末尾 `/` 無し) が **`/etc/settings.db` を上書き**した | `ReviewB1DirDestination` 5 件。`resolve_dest` が**実ディレクトリを見て**最終ファイル名を確定し、`cp` にディレクトリを渡さないことも見る |
| B2 | `mkdir -p` / `makedirs` が未判定の祖先を作る (`/etc/settings.db/a` で DB がディレクトリ化) | `ReviewB2MkdirChain` 5 件。`mkdir_chain` が root から 1 段ずつ判定し `ProtectedPath` |
| B3 | clean の symlink 分岐が保護判定より前、`root/etc` が symlink でも消して成功、保護ディレクトリの中身を bottom-up が先に消す | `ReviewB3Clean` 4 件。top-down + 判定を最初に + `check_root_etc` で拒否 |
| B4 | 名前規則で即 return する経路が `check_root_etc` を通らない、`etc` が通常ファイルだと `ENOTDIR` を空集合に握り潰す | `ReviewB4EntryCheck` 4 件。前提検査を入口で 1 回、ENOENT 以外は失敗 |
| B5 | 小文字 5 名しか stat しないので `/etc/SETTINGS.DB` + hardlink を `hsync -f bin` が上書き、stat が EIO でも open して切り詰める | C 側 (D.4 の「/etc 列挙の一致判定」11 件)。`sys_ls` で列挙 → 大文字小文字無視で一致 → 全部 stat、`OS32_ERR_NOTFOUND` 以外は同期中止 |
| B6 | `mkdir -p` の ENOSPC / `os.walk` の onerror 未指定 / clean の `rmdir` 失敗が成功になる | `ReviewB6FailurePropagation` 4 件 |
| B7 | 保護対象ディレクトリへの `copy --dest` が exit 1 | `ReviewB2MkdirChain.test_copy_dest_under_protected_ancestor` (除外は成功) |
| B8 | remote 欠損の早期 return / マウント中拒否 / `do_mount()` 失敗で stamp が残る | `ReviewB8PullStamp` 4 件。入口で消し、**全部成功した最後**に書く |
| B9 | `--dest /bin/..` (= `/`) を空パスとして拒否 | `ReviewB9RootNormalization` 4 件。parts が空 = `/` は正当、root より上だけ拒否 |

non-blocker も同時に直した: `FakeRun` の `cp` を実物どおり「宛先が既存ディレクトリ
なら中へ」にし、`cp` / `rm` / `mkdir` の宛先が temp の外なら **AssertionError**
(隔離の保証)、`ManifestEntryPoints` で resolver を差し替えずに manifest の入口
(file / glob / tag) を通し、`do_copy` の `Done!` と prune の件数から保護除外を除いた。
- `hsync` の inode 比較 (実体規則) はホストでは踏んでいない。
- `sudo` / ループマウントの実挙動は模擬。sudoers や ext2 の振る舞いは何も言わない。
- bind mount による `<root>/etc` の別名は原理的に検出できない (運用で禁止し、
  symlink / 別マウントだけを `check_root_etc` が止める)。


## T. 初期値 tsv / 生成ツール / ビルド統合 (S0-T、2026-09-13)

### 範囲と契約

`assets/settings/defaults.tsv` (初期値の正典)、`tools/mk_settings_db.py` (tsv → DB)、
`build/assets.mk` / `build/image.mk` / `build/core_packages.yaml` (ビルド統合と媒体添付)、
`tools/mkpkg.py` (登録ファイルの欠損をエラーに) だけ。生成 DB は媒体 (FDD / CD) だけが持ち、
既存システムには tsv を通常配備する (TASK_S0 §3)。カーネル・KAPI・配備スクリプトは触らない。

試験: `python3 -B tools/tests/test_mk_settings_db.py` (38 件)。ホストのみ。エミュレータ・配備・`make`
試験: `python3 -B tools/tests/test_mk_settings_db.py` (43 件)。ホストのみ。エミュレータ・配備・`make`
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
**証拠を 2 つに分ける**: (a) 反例で落ちるところを実際に見た試験と、(b) 実装が先にあって
「今 通っている」だけの回帰試験。どちらなのかを書き分ける。

#### (a) 反例による RED (実装前、または実装を一時的に緩めて確認した)

| 試験 | RED の症状 | 直したもの |
|---|---|---|
| `BuildWiring` 3 件 | 結線が無い (assets.mk / image.mk / core_packages.yaml) | ビルド統合 |
| `MkpkgMissingFile.test_missing_file_is_error` | `exit 0` + `MINIMAL.PKG: 0 files, 43 bytes` | mkpkg: 欠損をエラーに |
| `test_glob_without_match_is_error` / `test_later_package_empty_glob_writes_nothing` | 0 件の glob を `no files, skipping` で飛ばし、正常側の `.PKG` を書いて `exit 0` | mkpkg: 展開 0 件も欠損 |
| `test_missing_defs_is_error` | 存在しない `--defs` を黙って無視して `exit 0` | mkpkg: 欠けた定義をエラーに |
| `RealPackageDefs.test_every_registered_glob_matches_something` | リポジトリ自身の登録に 0 件の glob が 3 つ (`assets/images/*.vbz` `*.vdp` `assets/manga/*.mgx`) → 厳格化した mkpkg では `make iso` が落ちる | `userland/package_defs.yaml` の死んだ 3 行をコメント化 |
| `Rejects.test_int32_overflow` | 範囲検査を外すと `2147483648` を**受理**する | int32 の範囲検査 |
| `Rejects.test_duplicate_key` | 重複検査を外すと `sqlite3.IntegrityError: UNIQUE constraint failed` の Traceback (理由が `path:line:` の形で出ない) | 重複検査 |
| `Rejects.test_cr_rejected` | CR 検査を外すと `text` 値の末尾 CR (`x\r`) がそのまま DB に入る。`int` 行は字句規則が拾ってしまうので、CR 規則だけが捕まえる反例 (text 行・コメント行) を試験に足した | CR 検査 |

- mkpkg の 4 件: 修正前の `tools/mkpkg.py` (feat/gui 6360618) に戻して `MkpkgMissingFile` を実行 →
  `Ran 7 tests, FAILED (failures=3)` と上表の症状を確認し、修正版へ戻した。
- tsv の 3 件: `tools/mk_settings_db.py` の当該検査を一時的に外して `Rejects` を実行 →
  `Ran 18 tests, FAILED (failures=3)`。確認後に元へ戻し (着地版との差分が空であることを確認)、
  全件 GREEN を再確認した。
- `RealPackageDefs` は `userland/package_defs.yaml` を戻して単独実行 → 0 件の 3 パターンを
  列挙して FAILED を確認し、コメント化した版へ戻した。
- `Rejects.test_text_255_boundary` は 1 回目に試験側の欠陥で落ちた (1 メソッド内で出力名 `out.db` を
  使い回し、直前の成功ビルドの残骸を「失敗なのに DB を作っている」と誤判定)。出力名を毎回変えて修正。

#### (b) 回帰試験 (実装を先に書いたので反例 RED を経ていない)

決定性 6 件、スキーマ 5 件、その他の規則違反の拒否 (不正 UTF-8 / NUL / key・scope 規則 / 列数 /
type / int の字句 / text 255B / blob の hex) と境界値の受理、`RealDefaults` 2 件。
これらは「今 通っている」以上のことは言えない。

#### GREEN

`Ran 43 tests — OK`。

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
- **mkpkg**: 欠損 = 非ゼロ + `ERROR: <理由> (package '<name>')`。欠損は 3 種 — 登録ファイルが無い /
  glob の展開が 0 件 / `--defs` のファイルが無い。いずれも**全パッケージを先に解決してから**まとめて
  報告し、`.PKG` を 1 つも書かずに落ちる (後半のパッケージが欠損しても前半の `.PKG` を残さない)。
  簡易 parser は変えず、展開結果だけを見ている。
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

票 [docs/tasks/settings/TASK_S0.md](../../docs/tasks/settings/TASK_S0.md)。
本書は **S0-K (KAPI v50 / `shm_write_row` の境界 / exec 回収順序)** 分。
S0-D (配備保護) と S0-T (初期値 tsv) は別票が同じファイルに節を足す。

走らせ方 (ホストのみ。`make`・配備・エミュレータ・ローカル AI は使っていない):

```
python3 -B tools/tests/test_kapi_db_v50.py            # 9 件 + 回収順の本文検査
python3 -B tools/tests/test_kapi_db_v50.py --target   # 上に i386-elf 単体コンパイル
python3 -B -m unittest discover -s tools/tests -p 'test_kapi_db_owned.py'
python3 tools/tests/test_vfs_fd_sqlite.py
python3 tools/tests/test_sqlite_groups.py
```

## K. KAPI v50 / 境界検査 / 回収順序 (S0-K、2026-09-13)

### 1. 何を実物で組んだか (S0-K)

`tools/tests/kapi_db_v50_host.c` は **実 `kapi/kapi_db.c` + 実 `lib/sqlite3/sqlite3.c`
+ 実 `lib/sqlite3/os32_sqlite_vfs.c` + 実 `fs/vfs_fd.c` + RAM バックエンド**
(`sqlite_groups_backend.h`) を組む。ホストのファイルシステムには触らない。
模型にしたのは 2 つだけ:
模型は 4 つ:

- `ring3_user_range_ok` — 許可帯と PTE はカーネルの番地とページテーブルに依存する。
  ホストでは 1 本の帯 + 1 枚の「非 present なページ」に見立てた等価な判定を置く。
  実物の帯判定は `exec/exec.c` にあり、CPL=3 の受入 (`userland/tests/db_v50_test.c`) が踏む。
- `MEM_SHM_BASE` — 試験側の配列へ向け、16KB の **後ろに 256B の番兵**を置く。
- `vfs_stat` — RAM の fixture を見る試験側の実装。障害注入の口
  (`stat_fail_on` / `stat_fail_rc`) を持つ。**実物の `fs/vfs.c` ではない**ので、
  path 正規化の切り詰めそのものは再現しない (長さの判定だけを固定している)。
- `vfs_resolve_path` / `vfs_route` — `vfs_fd_sqlite_host.c` の入れ替えなしのコピー。
  正規化も mount 解決もしない。だから「255B の path で journal 名が切り詰められる」は
  **長さの規則**として試験し、切り詰めの実挙動は `fs/vfs.c` の読みに拠っている。

### 2. RED → GREEN

| # | 対象 | RED (実際に落としたもの) | GREEN |
|---|---|---|---|
| 1 | v50 の 7 本そのもの | 実装前は `kapi_db_open_existing` 等が存在せず、`kapi_db_v50_host.c` は **リンクできない** (undefined reference) | 9 ケース全通過 |
| 2 | `shm_write_row` の境界 (§1b) | 境界検査を `if (0 && ...)` で殺す → `FAIL shm_bound:359: kapi_db_step(h) == DB_STATUS_ERROR` (20000B の行が ROW を返す) | 検査を戻して `PASS shm_bound` + 番兵 256B が無傷 |
| 3 | exec 回収順序 (§1c) | `db_cleanup_owned` を元の (6) の位置へ戻す → `AssertionError: exec_reclaim_owned: db_cleanup_owned は vfs_close_owned より先` | 先頭へ移して PASS |
| 4 | 回収順序の**観測できる差** | `order_old` (FD を先に閉じる) で **後始末がバックエンドに届いた回数 = 2** | `order_new` (DB が先) で **21**。rollback の journal 読み戻しは生きた FD 越しにしか起きない |

### 2b. 実装レビュー 往復 1 (Codex、`08b4879`) の blocker 6 件 — RED → GREEN

反例はすべて `test_kapi_db_v50.py` に常設した。RED は **実装を 1 件ずつ元へ戻して**
採ったもの (実行した `FAIL` 行をそのまま写す)。

| # | blocker | ケース | RED |
|---|---|---|---|
| 1 | SHM にちょうど収まる TEXT/BLOB が欠落 ROW になる | `shm_exact` | writer を `len < remaining - 1` / `len < remaining` に戻す → `FAIL shm_exact:563: info->data_offset != 0` (16359B の TEXT が payload 無しの ROW) |
| 2 | journal の stat 失敗を不存在として SQLite に進む | `stat_faults` | stat の戻り値を NOTFOUND と区別しない実装に戻す → `FAIL stat_faults:610: kapi_db_open_existing("/f.db", 0) == -1` (I/O 障害なのに open が通る) |
| 3 | RW open の schema / I/O 障害が一律 CANTOPEN | `journal_mode` | `db_journal_mode_check` の戻りを `SQLITE_CANTOPEN` に潰す → `FAIL journal_mode:642: kapi_db_error_code(-1) == SQLITE_NOTADB` |
| 4 | stmt が無い `db_step` の DONE で診断が 0 に戻らない | `step_no_stmt` | 早期 DONE の `slot_note` を外す → `FAIL step_no_stmt:665: kapi_db_error_code(h) == SQLITE_OK` |
| 5 | 255B の path で本体を journal と誤認 | `path_len` | `journal_buf` を `VFS_MAX_PATH + 8` に戻す → `FAIL path_len:694: kapi_db_open_existing(longp, 0) == -1` (248B の path が通ってしまう) |
| 6 | 末尾の `\f` を複数 statement と誤判定 | `sql_tail` | `sql_is_space` から `\f` を外す → `FAIL sql_tail:711: sql_is_space('\f') && sql_is_space('\r')`、続けて `"SELECT 1;\f "` が拒否される |

**6 の訂正**: SQLite の空白は「6 文字」ではなく、トークナイザ (`aiClass` の
`CC_SPACE`) では **5 文字** — space / `\t` / `\n` / `\f` / `\r`。`0x0B` (`\v`) は
`sqlite3Isspace` では空白だが `aiClass` では `CC_ILLEGAL` で、`"SELECT 1\v"` は
prepare 自体が落ちる。したがって `\v` は末尾でも空白に数えない (数えると
「SQLite が読めない末尾」を通してしまう)。`sql_tail` がこの両方を固定している。

### 3. ケース一覧 (`test_kapi_db_v50.py`)

| ケース | 見るもの |
|---|---|
| `open_existing` | RO / RW の欠損 DB が**作られない**、0 バイト = `NOTADB`、hot journal = `BUSY_RECOVERY` かつ **journal が消えない**、`:memory:` / `file:` / 空 / `writable=2` / NULL の拒否、RO 接続で書けない、成功で「直前 open 失敗」が 0 に戻る |
| `prepare_only` | SELECT の先頭行が進まない / DML が実行されない、複数 statement の拒否、末尾の空白・`--`・`/* */`・`;` は可、NUL 込み 1024B ちょうどは可・1 バイト超は**切り捨てず**拒否 |
| `binds` | prepare 前 / step 後は不可、1-based と範囲外、負長、text 256B・blob 4097B の拒否、0B の text/blob が `typeof` で `text`/`blob` (NULL ではない)、**4096B blob の往復** |
| `error_code` | 範囲外 / 未使用 slot = `MISUSE`、失敗の保持と取得で消えないこと、成功で 0 に戻ること、**finalize / close が上書きしない**こと、close 後も再利用まで残ること、owner 別の `-1` 欄が混ざらないこと |
| `shm_bound` | 純関数 `shm_row_fits_n` のちょうど / 1 バイト超 / descriptor だけで溢れる列数、実接続で 20000B の行が `-1` + `SQLITE_TOOBIG` + 番兵無傷 |
| `user_range` | CPL=0 は素通し、CPL=3 は帯外 / 帯末尾またぎ / **ガードまたぎ** / overflow を拒否、NUL 無し path の拒否、bind 後にユーザ側を書き換えても値が変わらない (スクラッチへ写っている) |
| `owner_isolation` | 子 owner の回収で親の接続と実行中 stmt が無事 |
| `order_new` / `order_old` | 上の RED → GREEN の 4 |

### 4. ホストでは踏めなかったもの ([V4])

- **1364 列を超える行**での descriptor 領域のはみ出し。SQL は NUL 込み 1024B が上限なので
  そこまで列を並べた statement を作れない。純関数 `shm_row_fits_n` の算術だけで覆ってある。
| `shm_exact` | 1 列の行で TEXT `room - 1` / BLOB `room` が**書ける** (`data_offset != 0`、長さ一致)、+1 は `-1` + `TOOBIG`、番兵は 4 とも無傷 |
| `stat_faults` | journal / 本体の stat が NOTFOUND 以外で落ちたら `IOERR` で断り **SQLite を呼ばない** (バックエンド呼び出し回数で確認)、NOTFOUND だけが `CANTOPEN` |
| `journal_mode` | 非空の非 DB を RW で開くと `NOTADB` (CANTOPEN に潰れない)、正常な DB では `db_journal_mode_check` が `SQLITE_OK` |
| `step_no_stmt` | prepare_only 失敗の後の `db_step` が DONE を返したら診断が 0 に戻る |
| `path_len` | `<path>-journal` が `VFS_MAX_PATH` に収まる 247B は開ける、248B は本体があっても `CANTOPEN` |
| `sql_tail` | `sql_is_space` が SQLite のトークナイザと同じ 5 文字、`\v` は不可 (prepare も落ちる)、`\f` 混じりの末尾は可・`;` の後に statement があれば不可 |
| `transient` | 同じスクラッチを 2 本目の bind で上書きしてから step しても 1 本目の値が残る (`SQLITE_TRANSIENT`) |

### 4. ホストでは踏めなかったもの ([V4])

- **descriptor 領域だけで 16KB を溢れさせる列数の行**。溢れるには
  `(16384 - 12) / 12 = 1364` 列より多くが要るが、この build の
  `SQLITE_MAX_COLUMN` は **100** (`lib/sqlite3/os32_sqlite_config.h`) なので
  実接続では到達できない (SQL の 1024B 上限より前にこちらで止まる)。
  純関数 `shm_row_fits_n` の算術だけで覆ってある。
- `p + len` の **32bit** overflow。`include/types.h` の `u32` はホストでは `unsigned long`
  (64bit) なので、ホスト幅の端で同じ経路を踏むように書き換えてある。
- **rollback が本体ファイルを縮めること**。`os32 SQLite VFS` の `xTruncate` はまだ
  no-op 成功 (票 F3a が未実施) なので、どちらの回収順でもサイズは戻らない。
  だから順序の判定は「戻り値が成功か」ではなく「後始末がバックエンドに届いたか」で行う。
- 許可帯と PTE の**実物**の判定 (`ring3_ptr_ok` / `paging_addrspace_pte_flags`)。
  ホストにページテーブルが無い。CPL=3 の受入 `userland/tests/db_v50_test.c` (K2) と
  ブート時の `kselftest` が実機側の担当で、**どちらもまだ実行していない**
  (コーダーは `make`・配備・エミュレータを行わない)。
  K2 の PTE 検査ケースは実装レビュー 往復 1 の指摘で、**許可帯の外**を指す番地から
  **許可帯の中の未マップページ** (`kapi->sbrk_heap_limit` = guard_a の先頭) へ
  置き換えた — 前者は `ring3_ptr_ok` だけで落ちるので PTE 検査を消しても通ってしまう。
- `PDE.PS` (4MB ページ) の経路。この OS は一度も 4MB ページを張らないので
  ホストでも実機でも作れない。`paging_addrspace_pte_flags` は**明示的に**
  非 present 扱いで断る (安全側) というコードとコメントだけがある。
