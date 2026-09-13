# S0 — 設定レジストリの基盤 3 票 (S0-K / S0-D / S0-T)

状態: **設計 第 4 版 (往復 3 の blocker 5 件を反映。3 往復を使い切ったので、4 往復目を回すか実装に進むかはユーザー判断)**。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3 (2026-09-13、4 点承認 + 初期値はビルド毎に生成しインストーラに添付)。
契約の正典は [S0_FOUNDATION.md](S0_FOUNDATION.md) §2 (非破壊 / transaction) と §4 (ABI 追加案)、§5 (D0)。本票はそれを「いま実装する範囲」に切り、着地条件を決める。
規約: [C1] C89、[C2] kstr*、[ABI1〜3] kapi.json SSoT / 末尾追記 / 版上げ + `make clean`、[V4]。コーダーは worktree で実装 + ホスト TDD のみ (配備・コミット・エミュレータ・make は禁止)。

## 0. 3 票の関係

| 票 | 範囲 | 依存 | 触るファイル |
|---|---|---|---|
| **S0-K** | KAPI **v50** (7 本、末尾追記)、`shm_write_row` の境界検査、exec 回収順序 (DB → FD) | — | `sdk/kapi.json` + 生成物、`kapi/kapi_db.c/.h`、`exec/exec.c` (回収順の 1 か所)、`kernel/kselftest.c`、`userland/tests/db_v50_test.c` (ゲスト試験)、`docs/KAPI_SPEC.md`、`tools/tests/` (deploy.yaml / sdk.mk は PM) |
| **S0-D** | 通常配備の settings 保護 (D0) | — | `tools/nhd_deploy.py`、`tools/hostdrv_deploy.py`、`tools/prune_stale.py`、新規 `tools/deploy_protect.py`、**`userland/system/hsync.c`** (ゲスト側)、`tools/tests/` |
| **S0-T** | 初期値の正典 tsv、生成ツール、ビルド統合、インストーラ添付 | — | `assets/settings/defaults.tsv`、`tools/mk_settings_db.py`、**`tools/mkpkg.py`** (欠損をエラーに)、`build/assets.mk`、`build/image.mk`、`build/core_packages.yaml`、`tools/tests/` (deploy.yaml は PM) |

3 票のコード・試験ファイルは重ならない。**共有ファイルは PM が着地時に直列に編集する**: `userland/deploy.yaml` (S0-K の試験プログラム登録、S0-T の tsv 登録)、`build/sdk.mk` (3 票の `make check` 登録)、`tools/emu_agent/agent.py` (許可ターゲット)。コーダーは登録すべき行を報告するだけで、これらを編集しない。S2 (`libos32cfg` + `cfg`) は S0-K の v50 と S0-T の tsv に依存する。

## 1. S0-K — KAPI v50 と回収順序

### 1a. ABI (kapi.json の `api` 末尾に **この順で** 追記。現行末尾 = slot 200 `sys_yield` (0x328)。data_fields は 0x348 / 0x34C へ移る)

| slot | offset | name | args | ret | 規則 |
|---|---|---|---|---|---|
| 201 | 0x32C | `db_open_existing` | `const char *path, int writable` | handle ≥ 0 / -1 | `writable` 0 = `SQLITE_OPEN_READONLY`、1 = `SQLITE_OPEN_READWRITE`、他は拒否。**CREATE / URI を付けない** (`sqlite3_open_v2`、vfs は既定の `os32`)。空 path、`:memory:`、`file:` 接頭、`OS32_MAX_PATH` 超は拒否。RO では変更 PRAGMA を実行しない。RW は `journal_mode=DELETE` を確認し、不成立なら close して失敗。**open の前**に (1) `vfs_stat` で本体が存在し size > 0 であること、(2) `<path>-journal` が存在**しない**ことを検査し、どちらかに反すれば **SQLite を呼ばず**に失敗する (副作用ゼロ: SQLite の `hasHotJournal` は RO でも 0 ページの DB に付随する journal を削除するため、往復 2 の 1)。診断: 欠損 = `SQLITE_CANTOPEN`、0 バイト = `SQLITE_NOTADB`、journal あり = `SQLITE_BUSY_RECOVERY` (RO / RW とも。回復は S3 の明示操作)。失敗の拡張コードは **owner 別の「直前 open 失敗」欄**に保存 (`db_error_code(-1)` で読める) |
| 202 | 0x330 | `db_prepare_only` | `int handle, const char *sql` | 0 / -1 | SQL は NUL 込み 1024B 以内 (超過は**切り捨てず拒否**)。単一の非空 statement のみ (末尾の空白 / コメントは可、次の statement があれば拒否 = `sqlite3_prepare_v2` の `pzTail` を検査)。**step しない**。同じ handle の旧 stmt は finalize して置換 |
| 203 | 0x334 | `db_bind_int` | `int handle, int index, int value` | 0 / -1 | prepare 後・最初の step 前だけ。index は 1-based、範囲外は拒否 |
| 204 | 0x338 | `db_bind_text` | `int handle, int index, const char *text, int length` | 0 / -1 | `length` 0〜255、負・超過は拒否。カーネルへ**検証付きコピー**してから `SQLITE_TRANSIENT` で bind。0B でも非 NULL の空値。NULL ポインタは拒否 (NULL は `db_bind_null`) |
| 205 | 0x33C | `db_bind_blob` | `int handle, int index, const void *data, int length` | 0 / -1 | `length` 0〜4096。同上 |
| 206 | 0x340 | `db_bind_null` | `int handle, int index` | 0 / -1 | |
| 207 | 0x344 | `db_error_code` | `int handle` | code | slot の **「最後の失敗」コード** (SQLite 拡張 result code) を返す。**データ操作** (open_existing / prepare_only / bind_* / step / exec) は成功で 0 に、失敗でそのコードに更新する。**finalize / close は成功しても更新しない** (失敗したときだけ自分のコードで更新) — 「後片付けが原因診断を消さない」(FOUNDATION §4、往復 3 の 2)。close 後も slot が再利用されるまで `db_error_code(handle)` はその値を返す (再利用後は `SQLITE_MISUSE`)。引数不正は `SQLITE_MISUSE`。`handle = -1` は呼び手 owner の直前 open 失敗診断。取得しても消えない。F1 の隔離 (close 失敗 slot の再利用禁止) はこの欄とは別に維持。既存 step / exec の自動 finalize より前に拡張コードを保存する |

- `db_error_code` は **診断専用の lookup** を使う (`slot_get` は解放済み / 隔離 slot を拒否するので使わない)。閉じた handle は slot 再利用まで最後の失敗を返し、再利用後 / 範囲外 handle は `SQLITE_MISUSE`。
- 新 handle には既存の `db_step` (ROW=1 / DONE=0 / 失敗=-1、DONE で自動 finalize) / `db_finalize` / `db_close` をそのまま使う。反復は再 prepare (reset API は足さない)。
- ポインタ引数の検証 (往復 1 の B1、往復 2 の 2〜4): 既存の `argptr` マスク + `ring3_ptr_ok` は**先頭番地だけ**を見る (先頭が許可帯外なら wrap に入る前に kill — これは既存挙動で変えない)。v50 の wrap は先頭が通った後に自前で範囲を検査する: `kapi/kapi_db.c` の `db_user_range_ok(const void *p, u32 len)` = `p != NULL`、`p + len` が overflow しない、**呼び手が CPL=3 由来 (`ring3_in_syscall`) のときだけ** `[p, p+len)` の全ページが `ring3_ptr_ok` の許可帯 **かつ呼び手のアドレス空間 (syscall 中も CR3 はアプリの PD のまま。`AppSlot.as` の PD/PT を引く `paging_addrspace_pte_flags(as, page)` を新設 — master `page_tables` を見る `paging_pte_flags` はアプリ帯の USER 写像を持たないので使わない、往復 3 の 1) で present + USER** (許可帯内でも guard / 未マップ (sbrk 上限〜guard) は非 present なので、ここで弾かないとカーネルのコピーで #PF → 呼び手が kill される)。CPL=0 の呼び手 (常駐シェル / gshell = シェル帯 0x300000、ディスパッチャを通らない直呼び) は帯検査を**しない** (NULL / 長さ / 容量の検査だけ)。`path` / `sql` は上限バイトまでの範囲内で NUL を探す (NUL が見つかるまでの各ページを同じ規則で検査、上限内に NUL が無ければ拒否)。検証後にカーネル側の **1 本の静的スクラッチ** (sql 1024B、path 256B (NUL 込み)、text 256B (255 + NUL)、blob 4096B) へコピーし、bind は `SQLITE_TRANSIENT` (SQLite が写す) — 次の呼び出しで上書きされてよい。SQLite に呼び手のポインタを保持させない。受入 K2 の不正ケースは**先頭が許可帯内**のものに限る: 許可帯末尾 -1 から 2B、guard ページをまたぐ範囲、負の `length`、NUL 無しの path — いずれも -1 が返り試験プログラムは落ちない。`p + length` の overflow は許可帯と 4096B では構成できないので**ホスト側の helper 試験**で検査する。先頭が許可帯外 (`0xFFFFFFFE` 等) は既存どおり kill なので K2 に含めない。
- 既存 10 本の順序・型・挙動 (prepare が先頭行まで進む、exec が先頭 statement だけ) は**不変**。
- 版: `KAPI_VERSION` 49 → **50**。`docs/KAPI_SPEC.md` §3-2 に v50 の行、関数表を更新。`build/app.conf` の要求版は S2 で上げる (S0-K では上げない)。

### 1b. `shm_write_row` の境界検査 (F5 の前倒し)

- header + 全列 descriptor + payload の合計が 16KB を超えるとき、**範囲外書き込みも部分 ROW も返さず**失敗 (`-1`、`db_error_code` に `SQLITE_TOOBIG` 相当を保持)。既存の SHM レイアウト・型番号は不変。列数・列幅の上限は現行の定数を管理元 ([C4]) から引く。

### 1c. exec 回収順序 (F2 の順序修正のみ)

- `exec_reclaim_owned(id)` で **`db_cleanup_owned(id)` を `vfs_close_owned(id)` より先**に呼ぶ (SQLite が rollback / close で journal を書ける間に FD が生きている)。`shm_free_owned` / `con_sink_owner_exit` / `launch_owner_exit` の相対順は変えない (先頭に DB を持ってくるだけ)。
- FD lease の完全統合 (F2c/F2d) は後回し。**close 失敗 slot の再利用禁止 (F1) は維持**。

### 1d. ホスト TDD (tools/tests、`make check` に登録)

- 実 SQLite + 実 `os32_sqlite_vfs.c` + RAM backend (既存 `sqlite_groups_host.c` / `vfs_fd_sqlite_host.c` の作法) で: RO で missing が**作られない**、RO で書き込み系 SQL が失敗、RW+no-create で missing が失敗、`journal_mode` 不成立で失敗、prepare-only が step しない (SELECT の先頭行が進まない / DML が実行されない)、複数 statement 拒否、1024B 超拒否、bind の 1-based / 範囲外 / 0B / NULL / 4096B roundtrip / 4097B 拒否、text 256B 拒否、`db_error_code` の保持 (close で上書きしない、-1 の open 失敗)、SHM 境界超過で範囲外書き込み 0、回収順序 (mock で DB cleanup が FD close より先、rollback 時に FD 生存)。
- kselftest に 1〜2 項 (v50 の表の件数、境界検査)。
- `check_kapi_version.py` / `check_constraints.py` / i386-elf-gcc 単体コンパイル。

## 2. S0-D — 通常配備の settings 保護 (D0)

保護対象名 (`PROTECTED_BASENAMES`、大文字小文字を**区別しない**): `settings.db`、`settings.db-journal`、`settings.db-wal`、`settings.db-shm`、`settings.db.bak`。保護対象ディレクトリ: ゲストの `/etc`。

- 新規 `tools/deploy_protect.py`:
  - `resolve_dest(root, guest_path, host_src) -> str`: **実際の最終コピー先**を確定する (ディレクトリ指定 `guest: /etc/` は basename を補う、`..` / 連続 `/` / `./` を畳む = ゲストの正規化名)。root (マウントした NHD のツリー / HostDrv のルート) の外へ出る結果は例外 (拒否)。
  - `is_protected(root, dest) -> bool` は **2 段** (往復 2 の 6): (1) **名前規則 — realpath の前に**、正規化したゲスト名の親が `/etc` で basename が保護対象名 (大文字小文字無視) なら真 (dangling symlink や `etc → conf` の別名で実体が欠損していても守る = 「欠損は欠損のまま」)。(2) **実体規則** — `dest` の各要素を OS の `realpath` で解決し、root 内でなければ拒否、解決先の `st_dev/st_ino` が `<root>/etc/settings.db*` の**存在する**いずれかと一致すれば真 (hardlink / symlink の別名対策)。`stat` の失敗は **ENOENT (確定した不存在) だけ「保護対象ではない」**とし (新規の通常ファイル、例えば初回の `/etc/settings.tsv` を配備できる)、それ以外 (EACCES / EIO / ELOOP / 解決失敗) は保護側 = 配備を失敗させる (往復 3 の 3)。`<root>/etc` 自体が symlink または別マウント (realpath が字句と異なる) なら**配備全体を拒否** (bind mount の別名はツールで検出できないので運用で禁止し、この検査で最低限止める)。
  - 判定は「作成 / 切り詰め / 削除 / rename の**直前**」に、確定した最終パスで行う。HostDrv の内容比較 (同一なら copy しない) より**前**に判定し、保護対象は比較もせずに `protected:` を出す。
- 適用点 (**すべて**、B3、往復 3 の 4): `tools/nhd_deploy.py` の `do_sync` (cp、:670)、`do_sync_from_hostdrv` (:748、**そのファイルコピー前の mkdir (:733) にも**: 保護対象名のディレクトリ (`etc/settings.db/` の残骸) は作らない)、`do_copy` / `do_copy_all` (:254)、`do_rm` (:310)、`remove_partial` (:585); `tools/hostdrv_deploy.py` の `do_sync` (copy2 と mkdir)、`do_clean` (:218〜223 — `rmtree(root/etc)` をやめ、**エントリごとに削除して保護対象とその祖先ディレクトリは残す**); `tools/prune_stale.py` の `prune_hostdrv` (:107) と `prune_nhd` (:124)。`hsync` のディレクトリ作成 (`hsync.c:168`) も同じ名前規則で保護対象名のディレクトリを作らない。除外は明示ログ `protected: <path> (skipped)`。`nhd_deploy.py copy` / `copy-all` / `rm` の CLI からも保護対象は書けない (ホストのツールで settings.db を書く道を**作らない**)。
- **ゲスト側の `hsync`** (`userland/system/hsync.c`、HostDrv → NHD の再帰コピー、`O_CREAT|O_TRUNC`、往復 2 の 7): コピー先の文字列を**ゲスト内で字句正規化** (`./` `..` 連続 `/`、`-f` の subdir 連結 `hsync.c:254` を含む) してから名前規則 (親 `/etc` + 保護対象名、大文字小文字無視) で判定し、さらに `/etc/settings.db*` が存在すれば `sys_stat` の inode と比較して同一実体 (NHD 上の hardlink) も**コピーしない** (ログに skipped)。HostDrv に古い `etc/settings.db` が残っていても本 DB を切り詰めない。この 1 ファイルは S0-D の C 側の範囲。
- **NHD イメージ全体の配備** (B4、往復 2 の 8): `do_deploy` は local → remote の全体コピーなので、ファイル単位の除外では稼働後の設定 (remote) が古い local で戻る。→ **stamp** (`<local>.pulled`、JSON): `pull` (明示 / `ensure_local_nhd` の自動 pull とも、**コピー成功後にだけ**書く。失敗した pull は stamp を消す) が `remote_path`、`remote_size`、`remote_mtime`、**`remote_sha256`** (pull 時の remote の内容 hash)、`local_path` を記録する。`deploy` は (1) `OS32_NHD_LOCAL` が stamp の `local_path` と一致、(2) remote の path / size が一致、(3) remote を**再ハッシュ**して `remote_sha256` と一致 (mtime を保った別内容の置換も検出) のすべてを満たすときだけ全体を書き、どれかが崩れれば失敗 (`--force` でだけ通す)。local 側は deploy 自身が sync で書き換えるので local の hash は記録しない。
- **失敗の伝播** (B5、往復 2 の 9): `do_sync` / `do_sync_from_hostdrv` の `sync` 戻り値 (:687 / :763)、`do_copy` / `do_copy_all` の cp 失敗 (:257、continue して成功を返している)、`do_rm` の rm / sync 失敗、`do_umount` の sync 失敗 (:178)、`prune_stale.py` の rm / sync 失敗 (:124) を**すべて**非ゼロにし、`main` (:905) が各サブコマンドの戻り値を終了コードにする。後続 (NHD deploy) へ進めない。保護対象の**除外**は失敗ではない。
- 通常 deploy はDB や journal を作成・上書き・削除・rename しない。`NO_PRUNE` に依存しない。`/etc` を prune 対象に広げない。`do_init` / `do_format` は明示の全消去操作として通常更新から分離し、本票では触らない (呼ぶ側の承認 [D2] に委ねる)。`do_clean` (HostDrv) は上の適用点どおり**保護対象を残す**。
- 例外: 明示 install (FDD / CD の `install.bin` / `cdinst.bin` がゲスト内で行うコピー) と `cfg init` (S2、ゲスト内) はツールの対象外。
- ホスト TDD (B12): temp dir + mock で (1) **保護対象の source を意図的に置く** fixture (HostDrv / manifest に `etc/settings.db` と `-journal` を置き、宛先に別内容の DB がある / 無い) で、前後の存在一覧と hash が不変、`protected:` が出る; (2) tsv だけの通常配備では `protected:` が出ず tsv が更新される; (3) `guest: /etc/` のディレクトリ指定、symlink / hardlink の別名、大文字名、`..`、sync-from-hostdrv、copy / copy-all / rm CLI、prune の各経路; (4) 失敗注入 (cp / sync / rm) で非ゼロ、保護対象への write 系呼び出し 0; (5) stamp: local あり + stamp 無し / `local_path` 不一致 / remote hash 不一致で deploy が失敗、`--force` で通る; local 無し → 自動 pull が stamp を書いて deploy が通る; pull 失敗で stamp が残らない。`sudo` / `mount` / 実配備は遮断。`make check` に登録 (PM)。

## 3. S0-T — 初期値の正典 (tsv)、生成ツール、ビルド統合、インストーラ添付

決裁との整合 (B6): **生成 DB は媒体 (FDD / CD) だけが持つ**。既存システムへの seed は生成 DB ではなく **tsv を通常配備** (`assets/settings/defaults.tsv` → `/etc/settings.tsv`、通常配備で上書きされてよい) し、`cfg init` (S2) が `/etc/settings.db` が**無いときだけ** tsv から生成する (明示コマンド、自動生成しない)。`libos32cfg` は tsv の最小 reader を持つ (ホストの生成ツールと同じ規則)。

- `assets/settings/defaults.tsv` (B7): UTF-8、行 = `scope\tkey\ttype\tvalue` の**厳密 4 列** (タブで split、行全体を strip しない — 末尾の空欄 = 空 text を保つ)、`#` 始まりと空行はコメント。規則: `scope` は `system` / `gshell` / `user` / `app:<name>` (63B 以内)、`key` は `[a-z0-9_]+(/[a-z0-9_]+)*` (63B 以内)、`type` は `int` / `text` / `blob`、`int` は 10 進で **int32 の範囲** (`-2147483648`〜`2147483647`、超過は拒否)、`text` は UTF-8 妥当・埋め込み NUL 無し・255B 以内、`blob` は hex (偶数桁、空白なし、大文字小文字可、4096B = 8192 文字以内; C 側 reader (S2) はこの行長を扱う)。`scope` にも埋め込み NUL 無し、`app:<name>` の name は `[a-z0-9_]+`。改行は LF (CR は拒否)、`int` の字句は `-?[0-9]+`。重複 (scope, key) は拒否。初版の中身: `gshell\tdesktop/color\tint\t1`、`gshell\tdesktop/wallpaper\ttext\t` (空)、`gshell\ttaskbar/clock_24h\tint\t1`。
- `tools/mk_settings_db.py --tsv defaults.tsv --out out.db [--epoch N]` (B8): Python `sqlite3` で DESIGN §3 のスキーマ、`schema_version = 1`、`page_size = 1024`、`journal_mode = DELETE`、`user_version = 1`。**決定性は内容だけで決める**: `meta.created` は `--epoch` (省略時は `SOURCE_DATE_EPOCH`、それも無ければ **0**) の値。mtime は使わない。同じ tsv + 同じ epoch → 同じバイト列 (試験は「mtime の違う同内容の 2 ファイル」で確認)。上の規則の違反は非ゼロ終了。
- ビルド統合 (B9): `build/assets.mk` の `$(BUILD_OUT)/settings.db` は **`FORCE` 依存** (ビルド毎に必ず生成、ユーザー決裁)。`all` / `assets-all` に加え、**`images/os32_boot.d88` と `packages` (`build/image.mk`) から直接依存**させる。`packages` の依存は現行 `programs` だけなので、core パッケージが要求する既存入力 (`$(BUILD_OUT)/vmkernel.lz4`、`unicode_bin`、boot 成果物) も **`packages` の依存に結ぶ** (clean 後の単独 `make iso` / `make -j` でも欠損エラーにならない、往復 3 の 5)。`clean-assets` / `clean` で消す。`tools/mkpkg.py` は**すべての**登録ファイルの欠損を warning で飛ばさずエラー (非ゼロ) にする (属性の追加ではなく一律。現行の簡易 parser を変えない)。`make -j` の競合は依存で解決する (生成ターゲット 1 本に集約)。
- インストーラ添付: `build/image.mk` の FDD イメージに `/etc/settings.db=$(BUILD_OUT)/settings.db`、CD は `build/core_packages.yaml` の core パッケージに `/etc/settings.db`。
- **既知の前提不整合** (B10): 現行の `install.bin` は `/kernel.bin` を必須とし (`userland/system/install.c:330`)、FDD イメージは `/VMKRNL.LZ4` を収録するので、**FDD からの新規インストール自体が今は `/etc` コピーまで到達しない**。これは本票より前からの不整合で、修正 (install を lz4 カーネル + `/boot` レイアウトに合わせる) は **S3 (リカバリ / インストーラ更新) の票**で扱う。したがって本票の T1 は「媒体に入っている」までで、「新規インストールで seed される」は S3 の受入に送る (票に明記、成功と言わない)。
- `userland/deploy.yaml` に `assets/settings/defaults.tsv → /etc/settings.tsv` (tags core) を PM が登録 (YAML は `$(BUILD_OUT)` を展開しないので、DB ではなく tsv を載せる)。
- ホスト TDD: tsv → db の決定性 (mtime が違う同内容 2 ファイル + 同じ epoch で同一 hash、epoch 違いで異なる)、スキーマ / `user_version` / `page_size`、int32 超過・不正 UTF-8・NUL・key 規則違反・重複・列数違反の拒否、末尾空欄の text が空で入る、`sqlite3` で読み戻して行数一致、mkpkg の欠損エラー。`make check` に登録 (PM)。

## 4. 受入 (ゲスト、S0 の段階では最小)

| ID | 試験 | 合格条件 |
|---|---|---|
| K1 | `make clean` + `make clean-external` → `all` → `external` → `check`、配備 (停止 → pull → バックアップ → deploy-nhd → `make deploy`)、kselftest | `ver` API v50、kselftest +N / 0、regress 6 本 (`tools/emu_agent/tasks/regress.txt` の 6 件) |
| K2 | CPL=3 の小さな試験プログラム (`userland/tests/db_v50_test.c`、S0-K に含める。deploy.yaml 登録は PM) | RO で missing を開いても `/etc/nosuch.db` が**作られない**、prepare-only + bind + step で 1 行取れる、4096B blob roundtrip、`db_error_code(-1)` に open 失敗のコードが残る、**不正範囲** (許可帯末尾 -1 から 2B、負の length、overflow、NUL 無し path) が拒否され試験プログラムが落ちない |
| D1 | (a) HostDrv に**意図的に** `etc/settings.db` (別内容) を置き `make deploy` → ゲストで `hsync -f etc` (同サイズでも上書き経路を踏む) → NP21/W 停止 → `pull` → `make deploy-nhd` の順 | ログに `protected:` が出て、ゲストの `/etc/settings.db` の有無と内容 (hash) が前後で不変; (b) tsv だけの通常配備で `protected:` が出ず `/etc/settings.tsv` が更新される; (c1) local あり + stamp を消して `deploy-nhd` → 失敗; (c2) local を消して `deploy-nhd` → 自動 pull が stamp を書いて成功 |
| T1 | FDD / CD の媒体 | `images/os32_boot.d88` と core パッケージに `/etc/settings.db` が入る (mount / mkpkg の一覧で確認)、`sqlite3` で読める、2 回ビルドして同一 hash。**新規インストールでの seed は S3 の受入** (B10) |

## 5. レビューで見てほしい点

1. §1a の 7 本が S0_FOUNDATION §4 の契約と 1 対 1 か (RO/no-create、prepare-only、bind の上限、error_code の保持規則)。
2. §1c の順序変更で FEP (`kernel/ime_dict.c` の直接接続、protect 付き FD) や既存の `db_cleanup_all` 経路が壊れないか。
3. §2 の保護がすべてのコピー経路 (manifest / glob / tag / sync-from-hostdrv / prune) を覆うか、迂回 (`..`、別名、HostDrv 残骸) が残らないか。
4. §3 の方式 (生成 DB は媒体だけ、既存システムには tsv を通常配備し `cfg init` が明示生成) が決裁と DESIGN に一致するか。
5. 共有ファイル (deploy.yaml / sdk.mk / agent.py) を PM が直列編集する前提で、3 票のコード側が並走できるか。
6. 往復 2 の 10 件の反映: RO/RW open 前の stat + journal 検査 (副作用ゼロ)、present+USER の PTE 検査と CPL=0 の除外、K2 の範囲、診断は直前操作、名前規則を realpath の前に、hsync の正規化と inode、stamp の来歴 (remote hash + local_path)、失敗伝播の全経路、D1 の fixture 分離。

## 6. Codex 設計レビューの記録 (2026-09-13、`codex exec -s read-only`、網羅指示)

| 往復 | 判定 | blocker |
|---|---|---|
| 1 (`8597957`) | Request changes | 12 件: 範囲検証、D0 の最終パス / 別名 / 未保護経路 / 全体配備 / 失敗伝播、seed DB の通常配備が決裁と不一致、tsv 規則、決定性、Make 依存、FDD インストーラの既存不整合、共有ファイル、D1 fixture |
| 2 (`0212b9a`) | Request changes | 10 件: RO でも 0 ページ DB の journal を消す、許可帯内の非 present ページ、先頭が帯外は kill、CPL=0 の呼び手、診断の意味、欠損保護は名前規則を先に、hsync の正規化、stamp の来歴、CLI の失敗伝播、D1 と自動 pull |
| 3 (`cffa137`) | Request changes | 5 件: PTE 検査は呼び手の PD、finalize / close は診断を消さない、stat の ENOENT は不存在、mkdir と rmtree、packages の既存入力依存 |
| 4 版 | — | 5 件を反映 (本版)。3 往復を使い切ったので次はユーザー判断 |
