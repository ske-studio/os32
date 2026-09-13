# S3 — リカバリ (`install --recover-settings`) と `cfg import`

状態: **設計 第 1 版 (Codex 設計レビュー 往復 1/3 待ち)**。ユーザー決裁 2026-09-13「3. リカバリ」。前提: S0 / S2 / S4 / S5 完了 (main `02cefcc`)。
正典: [DESIGN.md](DESIGN.md) §2 (初期値はインストーラだけが持つ、リカバリモード) / §6b (JSON バックアップと `cfg import`)、[S0_FOUNDATION.md](S0_FOUNDATION.md) §6 (**明示リカバリ契約**: 自動分岐なし、表示と承認、元 DB と journal を対で保存、別名へ完全コピー → 検証 → 切替、失敗で元を消さない、原子性は backend で確認できなければ名乗らない、system.cfg 等は触らない、復元後に schema / sync / reopen を記録)、[TASK_S2.md](TASK_S2.md) §1 (規則) / §2 (`cfg export` の JSON 形、import は S3)、[TASK_S0.md](TASK_S0.md) (配備保護: `settings.db*` を通常配備が触らない)。
規約: [C1] C89、コーダーは worktree + ホスト TDD のみ、[D2] (使い捨てイメージ / ini) はユーザー承認。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S3-I** | `userland/system/install.c` に **`--recover-settings [drive]`** モード (§1)。要求 KAPI 7 → **50** (db_* v50 を使う)。通常インストール経路は不変 | C (system) | `userland/system/install.c` (+ `install_recover.inc` に分ける)、`tools/tests/install_recover_host.c` / `test_install_recover.py` / `s3_tdd.md` |
| **S3-C** | `cfg import <file> [--scope <scope>] [--merge]` (§2)。`libos32cfg` に最小 JSON reader (`cfg_json.c`) と `cfg_import_*` API、`cfg.c` のサブコマンド。`cfg export` は不変 | C | `userland/lib/cfg/{cfg_json.c, libos32cfg.h (追記のみ), cfg_import.c}`、`userland/cmds/cfg.c`、`tools/tests/cfg_host.c` / `test_cfg.py` / `s3_tdd.md` §C |
| PM | `build/app.conf` (install 50)、`build/image.mk` (FDD に `install.bin` は既に載る。`cfg.bin` を **FDD_MIN_CMDS に足す**: リカバリ後の `cfg status` 確認用)、`userland/deploy.yaml` 確認、受入 (§4、FDD ブートは NP21/W の起動引数 = ini 変更なし) | PM / テスター | — |

S3 に**含めない**: `--from <json|tar.lz4>` (リカバリモードからの JSON 復元 = `cfg import` を FDD の `cfg.bin` で行えるので専用オプションは作らない)、S6 `tar`、F3a-c、自動バックアップ。

## 1. `install --recover-settings [drive]` (S3-I)

引数: `drive` は `hd0` のみ (既定 `hd0`。他は `unsupported drive` で終了 1)。**起動媒体の検査**: `/` が対象 drive (= HDD ブート) なら `recover-settings must run from the install floppy` で終了 1 (`sys_is_mounted("/hd0")` が真、または root が hd0 — 判定は `vfs_devname` / `sys_stat("/")` の `st_dev` と `/hd0` の比較。実装で確定し票に書く)。DB を使うプロセスは FDD ブートでは存在しない (gshell も FEP も起動していない) — それでも `db_*` の接続数 (`db_mem_used` ではなく `db_open_existing` が BUSY を返すか) は見ない = 排他は「専用起動媒体からだけ実行する」で担保 (FOUNDATION §6-1 の「利用者を停止」は起動媒体の規則で満たす)。

手順 (すべて表示してから承認 `Y` を待つ。`N` / 他は何もせず終了 0):
1. **表示**: 媒体マスタ `/etc/settings.db` (FDD) を `db_open_existing(path, 0)` (v50、RO、CREATE なし) → S2 §1-1 (b) と同じ meta 検査 SQL → `master: schema_version <n>, <k> keys` を出す。開けない / 検査不合格なら `master unreadable` で終了 1 (**対象には触らない**)。
2. **表示**: 対象 `/hd0/etc/settings.db` の状態: `sys_stat` で `present <size> B` / `missing` / `stat failed (<err>)`、`/hd0/etc/settings.db-journal` の有無 (`hot journal present`)、既存 `.bak` / `.bak-journal` の有無 (`will replace .bak`)。`stat failed` (NOTFOUND 以外) は判定不能として **終了 1** (FOUNDATION §6-2「失敗で元を消さない」)。
3. **承認**: `Replace /hd0/etc/settings.db with master (schema <n>)? Existing db -> settings.db.bak [Y/N]`。
4. **退避 (対で)**: 既存 `.bak` / `.bak-journal` があれば **先に消す** (1 世代、承認済み)。`settings.db` → `settings.db.bak` を `sys_rename`、`-journal` があれば → `settings.db.bak-journal` を `sys_rename` (DB と journal を**対で**保存、FOUNDATION §6-1)。rename の失敗はそこで停止 (終了 1、元は残る)。ext2 の rename は「新名追加 → 旧名削除」で原子的でない (S2 §1-7 (f)) ので、失敗後は両名を `sys_stat` して**両方あれば消さず** `needs manual recovery: settings.db and settings.db.bak both present` で停止。
5. **コピー**: マスタを `/hd0/etc/settings.db.new` へ (install の `copy_file`、16KB バッファ、short write は失敗)。失敗したら `.new` を消し (自分の生成物)、**`.bak` を `settings.db` へ戻す** (rename、失敗すれば両名の状態を表示して停止)、終了 1。
6. **検証**: `.new` の長さがマスタと一致、`db_open_existing("/hd0/etc/settings.db.new", 0)` → meta 検査 → `keys` 数がマスタと一致 → close。不一致は 5 と同じ後始末。
7. **切替**: `sys_rename(".new" → "settings.db")`。失敗したら両名を `sys_stat` し、両方あれば消さず `needs manual recovery` (inode 共有の可能性、S2 §1-7 (f))、`settings.db` だけなら成功扱い、`.new` だけなら `.bak` を戻して終了 1。**「原子的復元」とは名乗らない** (FOUNDATION §6-3)。
8. **記録**: `vfs_sync()` の戻り値、`db_open_existing("/hd0/etc/settings.db", 0)` で **reopen して schema 検査** (close まで)、`recovered: schema_version <n>, sync=<rc>, reopen=<ok|code>` を表示。sync / reopen の失敗は表示して終了 1 (DB は切替済みなので戻さない — 状態を正直に出す)。
9. `system.cfg` / `profile` / boot 領域 / `.bak` (成功後も残す) には触らない (FOUNDATION §6-4)。

ホスト TDD (`install_recover_host.c`): install.c の該当部を `#include` し、KAPI 表を贋物 (RAM backend の stat / rename / open / read / write / sync に失敗を注入) に差し替えて (1)〜(9) の各段の失敗で「元 DB が残る / 欠損なら欠損のまま / `.new` の残骸は自分のものだけ消す / 両名残存で停止」を固定。実 SQLite 検証は `kapi_db_v50_host` の作法で meta 検査を実 kapi_db.c に通す。

## 2. `cfg import <file> [--scope <scope>] [--merge]` (S3-C)

- 入力: `cfg export` が書く形だけ (1 行目ヘッダ `{"schema_version":N,"exported":"..."}`、以降 1 行 1 レコード `{"scope":"…","key":"…","type":0|1|2,"v":<int>|"<text>"|"<base64>"|null}`)。最小 reader (`cfg_json.c`、純関数、**この形しか読まない**: キー順固定、空白なし、エスケープは `\"` `\\` `\n` `\t` `\uXXXX` (BMP のみ) だけ、数値は int32、他は拒否)。汎用 JSON は持ち込まない (DESIGN §6b)。
- 版: ヘッダの `schema_version` が **自分 (1) より新しければ拒否** (`newer backup: schema_version N`)、古い (0 は無い) は読める範囲で取り込む。
- 2 巡: **1 巡目で全行検証** (S2 の tsv reader と同じ規則: scope / key / type / 値の上限、UTF-8、重複、`v:null` は「未設定 = 行を書かない (`--merge` では既存を消さない、置換では入れない)」、base64 の妥当性)。行数上限 4096 (超は拒否)。**1 行でも不正なら何も書かない**。
- 2 巡目: `cfg_open(&db, 1)` → `cfg_begin` → 置換 (`--merge` 無し): `--scope` があればその scope の全行を `cfg_delete` 系の 1 statement (`DELETE FROM settings WHERE scope=?`、libos32cfg に `cfg_delete_scope` を追記) で消してから、無ければ **全 scope を消す** (`DELETE FROM settings`) → 全行 `cfg_set_*` → `cfg_commit` → `cfg_close`。**単一トランザクション** (DESIGN §6b、FOUNDATION §6-4)。`--merge` は削除せず `cfg_set_*` だけ (INSERT OR REPLACE)。失敗は rollback (`cfg_close` が行う) で **1 行も残らない**。
- 状態: MISSING / CORRUPT / VERSION では `cannot import: <status>` で終了 1 (S2 の規則 3: set は OK 状態でだけ)。
- 出力: `imported <n> records (<scope>|all scopes), replaced|merged`。close 失敗は `close failed (<code>)` で終了 1。
- `cfg export` との往復: export → import (置換) で `cfg list` が一致することをホスト試験で固定 (NULL 行は export で `v:null`、import では書かれない = 往復で「未設定」が保たれる)。

ホスト TDD (`cfg_host.c` §C): reader の受理 / 拒否 (エスケープ、`\u` サロゲート拒否、数値境界、順序違い、空白入り、余分なキー、行数上限、ヘッダ無し、版 2 拒否)、往復一致、`--scope` の削除範囲、`--merge`、途中失敗で 1 行も残らない (RAM backend に set 失敗を注入)、MISSING / VERSION の拒否。

## 3. 共有ファイルと FDD

- `build/app.conf`: `userland/system/install 50 262144` (PM)。`cfg` は 50 で登録済み。
- `build/image.mk`: `FDD_MIN_CMDS` に `cfg` を足す (PM) — FDD の `/bin/cfg.bin` で `cfg status` / `cfg import` がリカバリモードから使える (DESIGN §6b の `--from` はこれで代替)。FDD の容量 (1.2MB) に `cfg.bin` 29.7KB + `libos32cfg` 込み → 確認は `make images/os32_boot.d88` のサイズで。
- 配備: `install.bin` は `/sbin` (HostDrv / NHD) と FDD の両方に載る (既存)。

## 4. 受入 (ゲスト、PM / テスター。FDD ブートは NP21/W を `os32_boot.d88` 引数付きで起動 = ini 変更なし、memory `os32-np21w-launch`)

| ID | 試験 | 合格 |
|---|---|---|
| I1 | HDD ブートで `install --recover-settings` | `must run from the install floppy` で終了 1、DB 不変 |
| I2 | `cp /etc/settings.tsv /etc/settings.db` (壊す) → `os32gui` → 起動時通知 CORRUPT (S4) → FDD ブート → `install --recover-settings` → 表示 → `Y` | `recovered: schema_version 1, sync=0, reopen=ok`。`/hd0/etc/settings.db.bak` = 壊れた元 (1406 B)、`settings.db` = マスタ 3072 B。HDD ブート → `cfg status` OK → GUI 通知なし |
| I3 | DB 欠損 (`rm`) → FDD ブート → recover | `missing` 表示 → `Y` → 復元。`.bak` は作られない |
| I4 | 偽 journal あり (`cp` で `settings.db-journal`) → recover | 表示に `hot journal present` → `Y` → `.bak` と `.bak-journal` の**対**が残り、`settings.db-journal` は無い |
| I5 | `N` で承認しない | 何も変わらない (hash 一致)、終了 0 |
| I6 | 障害注入 (ホスト TDD のみ、ゲストでは踏めない): コピー失敗 / 検証不一致 / rename 失敗の両名残存 | 元が残る、`.new` の残骸だけ消える、両名残存で停止 |
| C1 | `cfg export /tmp/a.json` → `cfg set` で 2 件変更 → `cfg import /tmp/a.json` | `cfg list` が export 時点に戻る (置換)、`imported <n> records` |
| C2 | `cfg import` で `--scope gshell` / `--merge` | scope 外が残る / 既存が消えない |
| C3 | 版 2 のヘッダ (手で書く) / 壊れた行 | 拒否、DB 不変 (hash) |
| C4 | GUI 端末から `cfg import` | CUI と同じ |
| C5 | FDD ブートの `cfg.bin` で `cfg status` (対象は `/hd0/etc/…` ではなく FDD 自身の `/etc/settings.db` = マスタを読む) | `OK schema_version 1` (FDD のマスタ)。注: FDD ブート中の `cfg` は FDD の DB を見る — HDD の DB を `cfg` で直す用途には使わない (票に明記) |

## 5. レビューで見てほしい点

1. §1 の各段が FOUNDATION §6 の 4 契約を満たすか (特に「失敗で元を消さない」「両名残存で停止」「原子的と名乗らない」「journal を対で」)。
2. 起動媒体の検査 (HDD ブートからの実行拒否) が確実か、FDD ブートで DB の利用者が本当にいないか (FEP は `ime on` するまで開かない、gshell は起動していない)。
3. `install.c` の要求 KAPI を 50 に上げることの影響 (FDD の古いカーネルでは動かない = 同じ媒体に載るので問題ないか)。
4. `cfg import` の 2 巡と単一トランザクション、`v:null` の扱い、置換の削除範囲、版の判定、JSON reader の受理範囲の狭さが export の出力を**すべて**読めるか (blob base64、`\u` エスケープ、255B text の最長行)。
5. FDD の容量と `cfg.bin` の追加、`install.bin` のサイズ増 (db_* v50 の meta 検査を持つ)。
6. ホスト TDD の障害注入が §1 の全段を覆うか。

## 6. ユーザー判断が要る点

- FDD ブートの実走 (I2〜I5) は現行の NHD (作業イメージ) の `/etc/settings.db` を実際に置き換える。**受入の前に NHD をバックアップ** (既存の運用どおり、[D2])。使い捨てイメージは作らない (置換対象は settings.db だけで、`.bak` に元が残る)。
