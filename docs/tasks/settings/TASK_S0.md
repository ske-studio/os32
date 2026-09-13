# S0 — 設定レジストリの基盤 3 票 (S0-K / S0-D / S0-T)

状態: **設計 (PM 案、Codex 独立レビュー待ち)**。決裁: [S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §3 (2026-09-13、4 点承認 + 初期値はビルド毎に生成しインストーラに添付)。
契約の正典は [S0_FOUNDATION.md](S0_FOUNDATION.md) §2 (非破壊 / transaction) と §4 (ABI 追加案)、§5 (D0)。本票はそれを「いま実装する範囲」に切り、着地条件を決める。
規約: [C1] C89、[C2] kstr*、[ABI1〜3] kapi.json SSoT / 末尾追記 / 版上げ + `make clean`、[V4]。コーダーは worktree で実装 + ホスト TDD のみ (配備・コミット・エミュレータ・make は禁止)。

## 0. 3 票の関係

| 票 | 範囲 | 依存 | 触るファイル |
|---|---|---|---|
| **S0-K** | KAPI **v50** (7 本、末尾追記)、`shm_write_row` の境界検査、exec 回収順序 (DB → FD) | — | `sdk/kapi.json` + 生成物、`kapi/kapi_db.c/.h`、`exec/exec.c` (回収順の 1 か所)、`docs/KAPI_SPEC.md`、`tools/tests/` |
| **S0-D** | 通常配備の settings 保護 (D0) | — | `tools/nhd_deploy.py`、`tools/hostdrv_deploy.py`、`tools/prune_stale.py`、新規 `tools/deploy_protect.py`、`tools/tests/` |
| **S0-T** | 初期値の正典 tsv、生成ツール、ビルド統合、インストーラ添付、`cfg init` 用の seed 配備 | — | `assets/settings/defaults.tsv`、`tools/mk_settings_db.py`、`build/assets.mk`、`build/image.mk`、`userland/deploy.yaml`、`build/core_packages.yaml`、`tools/tests/` |

3 票は独立に実装できる (共有ファイル無し)。S2 (`libos32cfg` + `cfg`) は S0-K の v50 と S0-T の seed に依存する。

## 1. S0-K — KAPI v50 と回収順序

### 1a. ABI (kapi.json の `api` 末尾に **この順で** 追記。現行末尾 = slot 200 `sys_yield` (0x328)。data_fields は 0x348 / 0x34C へ移る)

| slot | offset | name | args | ret | 規則 |
|---|---|---|---|---|---|
| 201 | 0x32C | `db_open_existing` | `const char *path, int writable` | handle ≥ 0 / -1 | `writable` 0 = `SQLITE_OPEN_READONLY`、1 = `SQLITE_OPEN_READWRITE`、他は拒否。**CREATE / URI を付けない** (`sqlite3_open_v2`、vfs は既定の `os32`)。空 path、`:memory:`、`file:` 接頭、`OS32_MAX_PATH` 超は拒否。RO では変更 PRAGMA を実行しない。RW は `journal_mode=DELETE` を確認し、不成立なら close して失敗。失敗の SQLite 拡張コードは **owner 別の「直前 open 失敗」欄**に保存 (`db_error_code(-1)` で読める) |
| 202 | 0x330 | `db_prepare_only` | `int handle, const char *sql` | 0 / -1 | SQL は NUL 込み 1024B 以内 (超過は**切り捨てず拒否**)。単一の非空 statement のみ (末尾の空白 / コメントは可、次の statement があれば拒否 = `sqlite3_prepare_v2` の `pzTail` を検査)。**step しない**。同じ handle の旧 stmt は finalize して置換 |
| 203 | 0x334 | `db_bind_int` | `int handle, int index, int value` | 0 / -1 | prepare 後・最初の step 前だけ。index は 1-based、範囲外は拒否 |
| 204 | 0x338 | `db_bind_text` | `int handle, int index, const char *text, int length` | 0 / -1 | `length` 0〜255、負・超過は拒否。カーネルへ**検証付きコピー**してから `SQLITE_TRANSIENT` で bind。0B でも非 NULL の空値。NULL ポインタは拒否 (NULL は `db_bind_null`) |
| 205 | 0x33C | `db_bind_blob` | `int handle, int index, const void *data, int length` | 0 / -1 | `length` 0〜4096。同上 |
| 206 | 0x340 | `db_bind_null` | `int handle, int index` | 0 / -1 | |
| 207 | 0x344 | `db_error_code` | `int handle` | code | 直前の DB 操作の SQLite **拡張** result code (成功 0、引数不正 `SQLITE_MISUSE`)。`handle = -1` は呼び手 owner の直前 open 失敗診断。close / finalize は最初の失敗を上書きしない (slot 再利用まで保持)。取得しても診断は消えない |

- 新 handle には既存の `db_step` (ROW=1 / DONE=0 / 失敗=-1、DONE で自動 finalize) / `db_finalize` / `db_close` をそのまま使う。反復は再 prepare (reset API は足さない)。
- ポインタ引数 (`path` / `sql` / `text` / `data`) は既存ディスパッチャの `argptr` マスクと `ring3_ptr_ok` を通す。SQLite に呼び手のポインタを保持させない (すべてコピー)。
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

- 新規 `tools/deploy_protect.py`: `is_protected(guest_path) -> bool`。ゲスト側の**正規化パス** (`..` / 連続 `/` / 先頭 `./` を畳み、symlink は辿らず名前で判定) が `/etc/settings.db`、`/etc/settings.db-journal`、`/etc/settings.db-wal`、`/etc/settings.db-shm`、`/etc/settings.db.bak` に一致すれば真。大文字小文字は区別。
- `tools/nhd_deploy.py` の `do_sync` (manifest からの cp) と `do_sync_from_hostdrv` (再帰 cp) の**実コピー直前**、`tools/hostdrv_deploy.py` の `copy2` 直前、`tools/prune_stale.py` の削除直前に `is_protected` を挟み、対象なら**作らない・上書きしない・消さない**。除外は明示ログ (`protected: /etc/settings.db (skipped)`)。
- 保護対象を**除いた**コピーの失敗は従来どおり全体を失敗させる (2026-09-10 の非ゼロ終了を維持)。`NO_PRUNE` に依存しない。`/etc` を prune 対象に広げない。
- 例外: **明示 install** (FDD / CD の `install.bin` / `cdinst.bin` がゲスト内で行うコピー) はツールの対象外。`cfg init` (S2) もゲスト内。ホストのツールで settings.db を書く道は**作らない** (リカバリは S3)。
- ホスト TDD: temp dir と mock で、source / destination それぞれの DB / journal の存在・欠損の全組合せ、manifest 直指定 / glob / tag / 再帰コピーで前後の存在一覧と hash が不変、無関係な bin の更新は成功、失敗注入で非ゼロ、保護対象への write 系呼び出し 0。`sudo` / `mount` / 実配備は試験から遮断。`make check` に登録。

## 3. S0-T — 初期値の正典と生成、インストーラ添付、seed

- `assets/settings/defaults.tsv` (初期値の正典、UTF-8、`#` コメント可): 列 = `scope`、`key`、`type` (int/text/blob)、`value` (int は 10 進、text はそのまま、blob は hex)。初版の中身は S4 の数キー: `gshell` / `desktop/color` (int、システム 16 色の番号)、`gshell` / `desktop/wallpaper` (text、空 = 単色)、`gshell` / `taskbar/clock_24h` (int 0/1)、`system` / `schema_note` は置かない (meta で持つ)。
- `tools/mk_settings_db.py defaults.tsv out.db`: Python 標準の `sqlite3` で DESIGN §3 のスキーマ (`meta(schema_version, created)`、`settings(scope,key,type,ival,tval,bval) WITHOUT ROWID`) を作り、`schema_version = 1`、`page_size = 1024`、`journal_mode = DELETE`、`user_version = 1`。scope / key / text / blob の上限 (S0 §2-5) を検査して超過は失敗。出力は決定的 (`created` は tsv の mtime か `SOURCE_DATE_EPOCH`) にして、同じ tsv から同じバイト列。
- ビルド統合: `build/assets.mk` に `$(BUILD_OUT)/settings.db: assets/settings/defaults.tsv tools/mk_settings_db.py` を足し、`assets-all` / `all` の依存に入れる (**ビルド毎に生成**、ユーザー決裁)。`make clean` で消える。
- インストーラ添付: `build/image.mk` の FDD イメージに `/etc/settings.db=$(BUILD_OUT)/settings.db` を追加 (install.bin の `/etc/` 一括コピーで `/hd0/etc/settings.db` に入る = 新規インストールの seed)。CD (`packages` / `build/core_packages.yaml`) にも同じ経路で `/etc/settings.db` を含める。
- **seed の配備** (既存システムの `cfg init` 用): `userland/deploy.yaml` に `$(BUILD_OUT)/settings.db → /etc/settings.seed.db` (tags core) を登録する。seed は通常配備で上書きされてよい (S0-D の保護対象ではない)。`cfg init` (S2) は `/etc/settings.db` が**無いときだけ** seed をコピーし、あれば拒否する。
- ホスト TDD: tsv → db の決定性 (2 回生成して同一 hash)、スキーマ、上限超過の拒否、`sqlite3` で読み戻して行数一致。`make check` に登録。`check_manifests.py` の配備定義検査 (seed の登録) が通ること。

## 4. 受入 (ゲスト、S0 の段階では最小)

| ID | 試験 | 合格条件 |
|---|---|---|
| K1 | `make clean` → `all` → `external` → `check`、配備、kselftest | `ver` API v50、kselftest +N / 0、regress 6 本 |
| K2 | CPL=3 の小さな試験プログラム (`userland/tests/db_v50_test.c`、S0-K に含める) | RO で missing を開いても `/etc/nosuch.db` が**作られない**、prepare-only + bind + step で 1 行取れる、4096B blob roundtrip、`db_error_code(-1)` に open 失敗のコードが残る |
| D1 | seed を配備した状態で `make deploy` / `deploy-nhd` を 2 回 | ログに `protected:` が出て `/etc/settings.db` は作られない (存在しないまま)、`/etc/settings.seed.db` は更新される |
| T1 | FDD イメージ | `images/os32_boot.d88` に `/etc/settings.db` が入る (`mkfat12` の一覧か mount で確認)、`sqlite3` で読める |

## 5. レビューで見てほしい点

1. §1a の 7 本が S0_FOUNDATION §4 の契約と 1 対 1 か (RO/no-create、prepare-only、bind の上限、error_code の保持規則)。
2. §1c の順序変更で FEP (`kernel/ime_dict.c` の直接接続、protect 付き FD) や既存の `db_cleanup_all` 経路が壊れないか。
3. §2 の保護がすべてのコピー経路 (manifest / glob / tag / sync-from-hostdrv / prune) を覆うか、迂回 (`..`、別名、HostDrv 残骸) が残らないか。
4. §3 の seed 方式 (`/etc/settings.seed.db` は通常配備、`/etc/settings.db` はインストーラだけ + `cfg init`) が DESIGN の「自動生成しない」と矛盾しないか。
5. 3 票が本当に共有ファイル無しで並走できるか。
