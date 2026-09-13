# S3 — リカバリ (`install --recover-settings`) と `cfg import`

状態: **設計 第 2 版 (往復 1 の blocker 11 件 + non-blocker を反映、往復 2/3 待ち)**。ユーザー決裁 2026-09-13「3. リカバリ」。前提: S0 / S2 / S4 / S5 完了 (main `02cefcc`)。
正典: [DESIGN.md](DESIGN.md) §2 (初期値はインストーラだけが持つ、リカバリモード) / §6b (JSON バックアップと `cfg import`)、[S0_FOUNDATION.md](S0_FOUNDATION.md) §6 (**明示リカバリ契約**: 自動分岐なし、表示と承認、元 DB と journal を対で保存、別名へ完全コピー → 検証 → 切替、失敗で元を消さない、原子性は backend で確認できなければ名乗らない、system.cfg 等は触らない、復元後に schema / sync / reopen を記録)、[TASK_S2.md](TASK_S2.md) §1 (規則) / §2 (`cfg export` の JSON 形、import は S3)、[TASK_S0.md](TASK_S0.md) (配備保護: `settings.db*` を通常配備が触らない)。
規約: [C1] C89、コーダーは worktree + ホスト TDD のみ、[D2] (使い捨てイメージ / ini) はユーザー承認。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S3-I** | `userland/system/install.c` に **`--recover-settings [drive]`** と **`--revert-settings [drive]`** (§1)。要求 KAPI 7 → **50** (db_* v50 を使う)。通常インストール経路は不変 — ただし TASK_S0 §3 が S3 に送った前提不整合 (`install` が `/kernel.bin` を必須とするが FDD は `/VMKRNL.LZ4` + `/boot` を収録 → FDD からの新規インストールが `/etc` コピーまで到達しない) を**別の残ゲート S3-I2 として §7 に明記** (本票では直さない、ユーザー決裁) | C (system) | `userland/system/install.c` (+ `install_recover.inc` に分ける)、`tools/tests/install_recover_host.c` / `test_install_recover.py` / `s3_tdd.md` |
| **S3-C** | `cfg import <file> [--scope <scope>] [--merge]` (§2)。`libos32cfg` に最小 JSON reader (`cfg_json.c`) と `cfg_import_*` API、`cfg.c` のサブコマンド。`cfg export` は不変 | C | `userland/lib/cfg/{cfg_json.c, libos32cfg.h (追記のみ), cfg_import.c}`、`userland/cmds/cfg.c`、`tools/tests/cfg_host.c` / `test_cfg.py` / `s3_tdd.md` §C |
| PM | `build/app.conf` (install 50)、`build/image.mk` (FDD に `install.bin` は既に載る。`cfg.bin` を **FDD_MIN_CMDS に足す**: リカバリ後の `cfg status` 確認用)、`userland/deploy.yaml` 確認、受入 (§4、FDD ブートは NP21/W の起動引数 = ini 変更なし) | PM / テスター | — |

S3 に**含めない**: `--from <json|tar.lz4>` (**FDD の `cfg` は FDD 自身の `/etc/settings.db` を見るので HDD の復元には使えない**、往復 1 の B11。JSON からの復元の正式手順は「FDD で `--recover-settings` (マスタ復元) → HDD で再起動 → 通常の `cfg import`」= §2 の受入 C6)、S6 `tar`、F3a-c、自動バックアップ、通常インストールの `/kernel.bin` 不整合 (S3-I2、§7)。

## 1. `install --recover-settings [drive]` / `--revert-settings [drive]` (S3-I)

### 1a. 前提と起動媒体の検査 (往復 1 の B1)
- `drive` は `hd0` のみ (既定 `hd0`。他は `unsupported drive` で終了 1)。
- **FDD ブートの確認**: `kernel.c` の自動マウントは FDD ブートで root = `fd0` (fat)、HDD ブートで root = `hd0` (ext2)、`/hd0` は FDD ブートでも自動マウントされうる。判定は **root の実デバイス**: `sys_stat("/")` の `st_dev` が FDD 種別 (VFS の dev encoding `(dev_type << 8) | unit`、memory `os32-vfs-mount-dev-encoding`) であること。HDD 種別なら `recover-settings must run from the install floppy` で終了 1。`/hd0` の有無は判定に使わない。
- **対象のマウント**: `sys_is_mounted("/hd0")` が偽なら**回復専用に** `sys_mount("/hd0", "hd0", "ext2")` (現行 install の mount は partition 書込み / format の**後**にあるので、その経路は共用しない)。失敗は `cannot mount hd0` で終了 1。
- DB の利用者: FDD ブートでは gshell も FEP も起動しておらず、`install` 自身以外に接続は無い (FOUNDATION §6-1 の「利用者を停止」は起動媒体の規則で満たす。HDD ブートを拒否するのはそのため)。

### 1b. 名前の集合と事前検査 (往復 1 の B2 / B3)
対象ディレクトリ `/hd0/etc/` の 6 名: `settings.db` (本体)、`settings.db-journal`、`settings.db.bak`、`settings.db.bak-journal`、`settings.db.new`、`settings.db.new-journal`。承認前に**全部** `sys_stat` し三値 (PRESENT / ABSENT / UNKNOWN = NOTFOUND 以外の失敗) で持つ。
- UNKNOWN が 1 つでもあれば `stat failed (<name>: <err>)` で終了 1 (判定不能では触らない)。
- **同一性**: PRESENT の名前同士で `(st_dev, st_ino)` が一致する組があれば (ext2 の rename 失敗で同じ inode を 2 名が指す状態、リンク数は増えていない) `ambiguous: <a> and <b> share an inode - needs manual recovery` で終了 1 (**何も消さない・書かない**)。`st_ino == 0` (inode を供給しない FS) も判定不能として終了 1。
- **`.new` / `.new-journal` が PRESENT** なら `stale settings.db.new present - inspect and remove it manually (recovery shell: rm) before retrying` で終了 1。自分が今回作ったファイルだけを後始末の対象にする (残骸は S2 の `cfg init` 失敗の可能性があり、本体と inode を共有しうる)。v50 の RO open は `<path>-journal` の存在だけで BUSY_RECOVERY を返すので、`.new-journal` が残っていると検証も通らない。
- 表示 (承認前): マスタの版と件数、本体 `present <size> B` / `missing`、journal の有無 (`hot journal present - will be kept as settings.db.bak-journal`)、旧 `.bak` / `.bak-journal` の有無 (`will replace .bak`)、`.new*` 無し。

### 1c. マスタの検査 (往復 1 の B5)
FDD の `/etc/settings.db` を `db_open_existing(path, 0)` (v50、RO) → meta 検査 (S2 §1-1 (b) と同じ SQL) → **`settings` 表の全行走査** (`SELECT scope, key, type, ival, tval, bval FROM settings ORDER BY scope, key` を prepare-only + step で最後まで読み、件数と各列の `length()` の総和 (SQL 側で `SUM(length(scope)+length(key)+coalesce(length(tval),0)+coalesce(length(bval),0))` として 1 行で取る) を控える) → close。`PRAGMA integrity_check` はカーネルの SQLite で **OMIT** (`os32_sqlite_config.h:51`) なので使えない — 「meta + 全行走査 + 総和」が検証の上限であることを票と表示に明記する (未走査の自由ページの破損は検出できない)。open / 検査 / close の失敗は `master unreadable` / `master close failed (<code>)` で終了 1。

### 1d. 承認
`Replace /hd0/etc/settings.db with master (schema <n>, <k> keys)? Existing db(+journal) -> settings.db.bak(+.bak-journal) [Y/N]`。`Y` 以外は何もせず終了 0。

### 1e. 退避 (対で、状態機械) (往復 1 の B2 / B4)
状態 `S0` (初期) から次の順。**各 rename の完了を保持**し、失敗時は「旧 DB + journal を一組として」戻す `restore_pair()` を通す:
1. 旧 `.bak` / `.bak-journal` が PRESENT なら unlink (承認済み 1 世代。1b の同一性検査で本体と別 inode であることは確認済み)。失敗は `cannot remove old backup` で終了 1 (元は無傷)。
2. 本体が PRESENT: `sys_rename(settings.db → settings.db.bak)` → 状態 `S1`。失敗: 両名を stat し、両方 PRESENT なら `needs manual recovery: settings.db and settings.db.bak both present` で終了 1 (消さない)、本体だけなら元のまま終了 1、`.bak` だけなら「rename は実質成功」として `S1` へ。
3. journal が PRESENT: `sys_rename(settings.db-journal → settings.db.bak-journal)` → `S2`。失敗: 両名検査は 2 と同じ。両方 / 本体側だけが残った場合は **`restore_pair()`** (`.bak → settings.db`。journal は元のまま) を試み、その結果 (戻った / 両名残存) を表示して終了 1。
4. 本体 ABSENT + journal PRESENT (孤立 journal) も 3 を行う (journal は対の一部として `.bak-journal` へ)。本体 ABSENT + journal ABSENT は退避なし (`S2` 相当)。

`restore_pair()` = `.bak → settings.db` と `.bak-journal → settings.db-journal` を**両方**試み、それぞれの結果を表示。どちらかで両名残存なら `needs manual recovery` (消さない)。**片方だけ戻して対を分離したまま終わらない** (両方の rename を試みてから停止する)。

### 1f. コピー・検証・切替 (往復 1 の B3 / B5 / B6)
5. マスタを `/hd0/etc/settings.db.new` へコピー (`O_CREAT | O_TRUNC`、1b で `.new` が無いことは確認済みなので自分の生成物)。short write / read 失敗: `.new` を unlink → `restore_pair()` → 終了 1。
6. **検証**: (i) 長さがマスタと一致、(ii) **バイト比較** (マスタと `.new` を 16KB ずつ読み比べ、読取り失敗も不一致扱い)、(iii) `db_open_existing("/hd0/etc/settings.db.new", 0)` → meta 検査 → 1c と同じ全行走査で件数と総和がマスタと一致 → close。不一致 / open 失敗: `.new` を unlink → `restore_pair()` → 終了 1。**close の失敗** (接続が隔離される): `.new` を消さず (隔離接続が inode を掴んでいる可能性、S2 §1-7 と同じ扱い) `verify close failed (<code>) - settings.db.new kept; run --revert-settings after reboot` で終了 1 (この時点で本体は `.bak` にあるので、利用者は次の起動で `--revert-settings` で戻せる)。
7. **切替**: `sys_rename(.new → settings.db)`。両名検査: 両方 PRESENT → `needs manual recovery` (消さない、終了 1)。`settings.db` だけ → 成功。`.new` だけ → `.new` を unlink → `restore_pair()` → 終了 1。stat が UNKNOWN → 消さず `needs manual recovery`。**「原子的復元」とは名乗らない** (FOUNDATION §6-3)。
8. **記録**: `vfs_sync()` の戻り値、`db_open_existing("/hd0/etc/settings.db", 0)` で reopen → 1c と同じ検査 → close。`recovered: schema_version <n>, <k> keys, sync=<rc>, reopen=<ok|code>, close=<ok|code>` を表示。sync / reopen / close のどれかが失敗なら終了 1 で、**旧対を自動では戻さない**が、次の手順を表示: `to revert: install --revert-settings hd0` (§1g)。
9. `system.cfg` / `profile` / boot 領域 / `.bak` (成功後も残す) には触らない (FOUNDATION §6-4)。

### 1g. `install --revert-settings [drive]` (往復 1 の B6: 切替後失敗の旧対復帰手順)
1a / 1b と同じ検査 (同一性、UNKNOWN、`.new*`) の後、表示 → 承認 → `settings.db → settings.db.failed` (PRESENT なら。既存 `.failed` は unlink、承認済み) → `restore_pair()` → `vfs_sync` → 表示。`.bak` が無ければ `nothing to revert` で終了 1。両名残存はすべて `needs manual recovery` で消さない。

### 1h. ホスト TDD (`install_recover_host.c`)
install.c の該当部 (`install_recover.inc`) を `#include` し、KAPI 表を贋物 (RAM backend の stat (三値・UNKNOWN 注入・st_ino 共有の注入)、rename (新名追加成功・旧名削除失敗の注入)、open / read / write (short write)、unlink 失敗、sync 失敗、db close 失敗) に差し替え、1b〜1g の**各段の各失敗**で「元 DB + journal が対で残る / 欠損なら欠損のまま / 自分の `.new` だけ消える / 両名残存で消さず停止 / 対が分離して終わらない」を固定。**再実行**の試験 (両名残存の後にもう一度 `Y` → 1b の同一性検査で停止、本体無傷) を含める (往復 1 の B2)。実 SQLite の検査部 (1c / 6-iii) は `kapi_db_v50_host` の作法で実 kapi_db.c に通す。

## 2. `cfg import <file> [--scope <scope>] [--merge]` (S3-C)

- 入力: `cfg export` が書く形だけ (1 行目ヘッダ `{"schema_version":N,"exported":"..."}`、以降 1 行 1 レコード `{"scope":"…","key":"…","type":0|1|2,"v":<int>|"<text>"|"<base64>"|null}`)。最小 reader (`cfg_json.c`、純関数、**この形しか読まない**: キー順固定、空白なし、エスケープは `\"` `\\` `\n` `\r` (往復 1 の B7: writer は CR を `\r` で出す) `\t` `\uXXXX` (BMP のみ。非 BMP は writer が生 UTF-8 で出すのでサロゲートは拒否) だけ、数値は int32、他は拒否)。汎用 JSON は持ち込まない (DESIGN §6b)。**行バッファは writer の最長行から決める**: text 255B が全部 `\u0001` の行 1,695B、blob 4096B の行 5,629B (LF + NUL 込み) → 6KB。復号後の上限 (63 / 255 / 4096、UTF-8、NUL 無し) は別に検査。
- 版: ヘッダの `schema_version` が **自分 (1) より新しければ拒否** (`newer backup: schema_version N`)、古い (0 は無い) は読める範囲で取り込む。
- 対象の抽出 (往復 1 の B10): `--scope <s>` があれば **読み取り・検証・適用のすべてを scope = s の行だけ**に限定 (他 scope の行は構文検証だけして無視)。scope 外の**値は不変**。
- 2 巡: **1 巡目で対象行を全部検証** (S2 の tsv reader と同じ規則: scope / key / type / 値の上限、UTF-8、`v:null` の型、base64 の妥当性、重複)。**件数上限は現行 export の最大 = 32 scope × 256 key = 8192 件** (往復 1 の B8)。重複検出は (scope, key) の 32bit hash (FNV-1a) 8192 個の表 (32KB) + 衝突時は先行行を `sys_lseek` で読み直して文字列比較 (誤検出も見逃しも無い)。**1 行でも不正なら何も書かない**。
- 2 巡目 (ファイルを先頭から読み直す。再 open / read / parse の失敗も rollback 対象): `cfg_open(&db, 1)` → `cfg_begin` → 置換 (`--merge` 無し): `cfg_delete_scope(db, scope)` (**新規 API**: `scope` が NULL なら `DELETE FROM settings`、非 NULL なら `WHERE scope=?`。txn 必須・失敗で txn failed・bind・再入禁止は既存規則を継承) → 対象行を `cfg_set_*` / **`cfg_set_null(db, scope, key, type)`** (**新規 API**、往復 1 の B9: `v:null` は「宣言型つきの NULL 行」として格納し、`cfg list` の `(unset)` 行と export の `v:null` が往復で保たれる。`INSERT OR REPLACE … VALUES(?,?,?,NULL,NULL,NULL)`) → `cfg_commit` → `cfg_close`。置換では削除後なので**素の INSERT** でよいが、実装は既存の `cfg_set_*` (INSERT OR REPLACE) を使い、重複は 1 巡目の検査に任せる (INSERT OR REPLACE に重複検出をさせない)。`--merge` は削除せず set だけ。**単一トランザクション** (DESIGN §6b、FOUNDATION §6-4)。
- 失敗の保証: commit 前の失敗 (set / 2 巡目の read / parse) は `cfg_close` の rollback で **1 行も残らない** (rollback が成功した場合。rollback 失敗は `cfg_last_close_error` に出る)。**commit 成功後の close 失敗は更新済み** (`imported … (close failed <code>)` と表示、終了 1)。
- 状態: MISSING / CORRUPT / VERSION では `cannot import: <status>` で終了 1 (S2 の規則 3: set は OK 状態でだけ)。
- 出力: `imported <n> records (<scope>|all scopes), replaced|merged`。
- `cfg export` との往復: export → import (置換) で `cfg list` (NULL 行の `(unset)` を含む) が一致することをホスト試験で固定。

ホスト TDD (`cfg_host.c` §C): reader の受理 / 拒否 (エスケープ 6 種、`\u` サロゲート拒否、数値境界、順序違い、空白入り、余分なキー、最長行 5,629B、ヘッダ無し、版 2 拒否)、CR を含む text の往復、NULL 行の往復、8192 件の受理と 8193 件目の拒否、hash 衝突の非重複 2 件の受理、`--scope` の削除範囲と **scope 外の値の不変**、`--merge`、途中失敗 (set / 2 巡目の read) で 1 行も残らない (RAM backend に注入)、commit 後 close 失敗の表示、MISSING / VERSION の拒否。

## 3. 共有ファイルと FDD

- `build/app.conf`: `userland/system/install 50 262144` (PM)。`cfg` は 50 で登録済み。
- `build/image.mk`: `FDD_MIN_CMDS` に `cfg` を足す (PM) — FDD の `/bin/cfg.bin` で **FDD 自身のマスタ**の `cfg status` を確認する用途 (HDD の DB には使えない、B11)。容量は D88 のファイルサイズではなく **生成の成功とクラスタ使用量 / 空き** (`tools/mkfat12.py` の出力) で判定。
- 配備: `install.bin` は `/sbin` (HostDrv / NHD) と FDD の両方に載る (既存)。

## 4. 受入 (ゲスト、PM / テスター。FDD ブートは NP21/W を `os32_boot.d88` 引数付きで起動 = ini 変更なし、memory `os32-np21w-launch`)

| ID | 試験 | 合格 |
|---|---|---|
| I1 | HDD ブートで `install --recover-settings` | `must run from the install floppy` で終了 1、DB 不変 (root の st_dev で判定) |
| I2 | `cp /etc/settings.tsv /etc/settings.db` (壊す) → `os32gui` → 起動時通知 CORRUPT (S4) → FDD ブート → `install --recover-settings` → 表示 → `Y` | `recovered: schema_version 1, sync=0, reopen=ok`。`/hd0/etc/settings.db.bak` = 壊れた元 (1406 B)、`settings.db` = マスタ 3072 B。HDD ブート → `cfg status` OK → GUI 通知なし |
| I3 | DB 欠損 (`rm`) → FDD ブート → recover | `missing` 表示 → `Y` → 復元。`.bak` は作られない。派生: 欠損 + 孤立 journal → journal が `.bak-journal` へ (対の一部)、欠損 + 旧 `.bak` → 旧 `.bak` は消える (承認済み)、欠損 + `.new` 残骸 → `stale settings.db.new present` で停止 |
| I4 | 偽 journal あり (`cp` で `settings.db-journal`) → recover | 表示に `hot journal present` → `Y` → `.bak` と `.bak-journal` の**対**が残り、`settings.db-journal` は無い |
| I5 | `N` で承認しない | 何も変わらない (hash 一致)、終了 0 |
| I6 | 障害注入 (ホスト TDD のみ、ゲストでは踏めない): コピー失敗 / 検証不一致 / 各 rename 失敗の両名残存 / **両名残存の後の再実行** / close 失敗 | 元が対で残る、自分の `.new` だけ消える、両名残存で消さず停止、再実行は 1b の同一性検査で本体無傷のまま停止 |
| I7 | `--revert-settings` (I2 の後) | `settings.db` → `.failed`、`.bak` → `settings.db` (+ `.bak-journal` → `-journal`)、`cfg status` = 元の状態 (I2 で壊した DB なら CORRUPT) |
| C1 | `cfg export /tmp/a.json` → `cfg set` で 2 件変更 → `cfg import /tmp/a.json` | `cfg list` が export 時点に戻る (置換)、`imported <n> records` |
| C2 | `cfg import` で `--scope gshell` / `--merge` | scope 外が残る / 既存が消えない |
| C3 | 版 2 のヘッダ (手で書く) / 壊れた行 | 拒否、DB 不変 (hash) |
| C4 | GUI 端末から `cfg import` | CUI と同じ |
| C5 | FDD ブートの `cfg.bin` で `cfg status` (FDD 自身の `/etc/settings.db` = マスタ) | `OK schema_version 1`。**マスタの検査に限定** (FDD から HDD の DB は読めない・直せない) |
| C6 | JSON からの復元の正式手順: `cfg export /tmp/b.json` → DB を壊す → FDD で `--recover-settings` → HDD 再起動 → `cfg import /tmp/b.json` | export 時点の `cfg list` に一致 |
| C7 | CR を含む text (`cfg set … text` ではシェル行に入れられないので `cfg_bench` 系ではなくホスト試験で担保) と NULL 行 (`cfg list` の `(unset)`) の往復 | ホスト試験、ゲストでは NULL 行の往復だけ (`cfg del` では作れないので tsv fixture の `text` 空欄は空値 = NULL ではない。**NULL 行はゲストでは作れない** → ホストのみと記録) |

## 5. レビューで見てほしい点

1. §1 の各段が FOUNDATION §6 の 4 契約を満たすか (特に「失敗で元を消さない」「両名残存で停止」「原子的と名乗らない」「journal を対で」「再実行で壊さない」「切替後失敗の復帰手順 = `--revert-settings`」)。
2. 起動媒体の検査 (HDD ブートからの実行拒否) が確実か、FDD ブートで DB の利用者が本当にいないか (FEP は `ime on` するまで開かない、gshell は起動していない)。
3. `install.c` の要求 KAPI を 50 に上げることの影響 (FDD の古いカーネルでは動かない = 同じ媒体に載るので問題ないか)。
4. `cfg import` の 2 巡と単一トランザクション、`v:null` の扱い、置換の削除範囲、版の判定、JSON reader の受理範囲の狭さが export の出力を**すべて**読めるか (blob base64、`\u` エスケープ、255B text の最長行)。
5. FDD の容量と `cfg.bin` の追加、`install.bin` のサイズ増 (db_* v50 の meta 検査を持つ)。
6. ホスト TDD の障害注入が §1 の全段を覆うか。

## 6. ユーザー判断が要る点

- **受入イメージ**: FOUNDATION §6 は「承認済み使い捨てイメージ」での受入を求める。本票の提案は「現行の作業 NHD をバックアップしたうえで実走 (置換対象は settings.db だけで `.bak` に元が残る)」。使い捨てイメージ (NHD の複製を NP21/W に別名で刺す = ini 変更 [D2]) にするか、バックアップ済み作業 NHD で行うかを決裁。決裁までは受入を始めない。

## 7. 残ゲート (本票で直さない)

- **S3-I2**: 通常インストール (`install` 無印) は `/kernel.bin` を必須とし FDD は `/VMKRNL.LZ4` + `/boot` を収録するので、FDD からの新規インストールが `/etc` コピー (settings.db の seed) まで到達しない (TASK_S0 §3 B10、S0-T の T1 は「媒体に入っている」まで)。修正は install の lz4 カーネル + `/boot` レイアウト対応で、受入には**使い捨て NHD** ([D2]) が要る。本票の後に別票で。
- ジャーナル: DELETE journal の回復・電源断の crash durability (FOUNDATION §6「保証上限」) は未検証のまま (S5 で「set → ハードリセット → 保持」までは確認)。

## 8. レビュー記録

| 版 | 判定 | 要旨 |
|---|---|---|
| 第 1 版 | Request changes | 11 件: B1 `/hd0` マウント有無の判定が逆 → root の st_dev で FDD を判定し `/hd0` は別に mount、B2 両名残存の後の再実行で `.bak` unlink が本体を壊す → 事前に 6 名の同一性検査、B3 既存 `.new` を自分の生成物扱い + `.new-journal` で RO open が BUSY → 残骸があれば停止、B4 失敗時に DB と journal が分離 → 状態機械 + `restore_pair()`、B5 長さ + meta + 件数では内容一致を保証しない → バイト比較 + 全行走査 (integrity_check は OMIT)、B6 切替後失敗の復帰手順と close 失敗 → `--revert-settings` と close 記録、B7 reader が `\r` を拒否 → 許可、B8 4096 件上限 → 8192 + hash 重複検出、B9 NULL 行を捨てると list が一致しない → `cfg_set_null`、B10 `--scope` が削除だけ限定 → 抽出も限定、B11 FDD の cfg import は HDD 復元の代替にならない → 正式手順 C6。non-blocker: 最長行 5,629B、`cfg_delete_scope` の契約、2 巡の失敗保証の文言、三値 stat、容量の判定法、受入イメージの決裁、S3-I2 の残ゲート |
