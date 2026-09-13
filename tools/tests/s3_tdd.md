# S3 — リカバリ (`install --recover-settings`) と `cfg import` のホスト TDD 記録

票: [`docs/tasks/settings/TASK_S3.md`](../../docs/tasks/settings/TASK_S3.md)、
契約の正典は [`docs/tasks/settings/S0_FOUNDATION.md`](../../docs/tasks/settings/S0_FOUNDATION.md) §6
(明示リカバリ契約) と [`docs/tasks/settings/TASK_S0.md`](../../docs/tasks/settings/TASK_S0.md) §D (配備保護)。

3 票 (S3-I / S3-C / S3-D) が同じファイルに書くので、節を票ごとに分ける。
**自分の節だけ**を書き、他の節には触れないこと。

- `## I.` — `install --recover-settings` / `--revert-settings` (S3-I)
- `## C.` — `cfg import` と最小 JSON reader (S3-C)
- `## D.` — 通常配備の保護にリカバリ生成物の名前を足す (S3-D)

---

## D. リカバリ生成物の配備保護 (S3-D) — 2026-09-13

### D.1 何を足したか、なぜ要るか

S0-D の配備保護は `/etc/settings.db` `-journal` `-wal` `-shm` `.bak` の 5 名しか
知らない。S3 のリカバリは同じ `/etc` に**残り 7 名**を作る (票 §1b の 9 名から
本体と `-journal` を除いたもの):

| 名前 | 中身 | 通常配備が掴むと何が起きるか |
|---|---|---|
| `settings.db.bak` | 切替前の本体の写し | (S0-D で既に保護済み) |
| `settings.db.bak-journal` | 切替前の journal の写し | 対が分離し、revert が「DB だけ・journal 無し」に戻す |
| `settings.db.failed` / `-journal` | revert が退避した「失敗した世代」 | 手動復旧の材料が消える |
| `settings.db.new` / `-journal` | 検証中のコピー (他人の生成物) | 隔離接続が掴む inode を配備が消す (票 §1f 手順 4) |
| `settings.db.recover-state` | phase の印 (`backup` / `switching` / `switched` / `reverting` / `failed` / `done`) | 門が効かなくなり、次の recover が壊れた本体を新しい `.bak` にして**元の唯一の写しを失う** |

つまり `.bak*` と `.recover-state` を 1 つでも失うと「元へ戻す」経路そのものが
消える。`.new*` / `.failed*` は他人の生成物なので、そもそも配備が作ることも
消すこともしてはいけない。

足した先は 2 か所だけ (新しい判定は増やさない。S0-D の 2 段判定にそのまま乗る):

- `tools/deploy_protect.py` の `PROTECTED_BASENAMES` (5 → **11 名**)。
  名前規則 (`name_is_protected`)、実体検査の収集対象 (`_protected_identities`)、
  試験側の `FakeRun.touched_protected` が同じ集合を参照しているので、
  nhd_deploy (manifest / HostDrv 丸写し / copy / copy-all / rm / remove_partial) /
  hostdrv_deploy (manifest / clean) / prune_stale (prune_nhd / prune_hostdrv) の
  **全経路**が 1 か所の変更で揃う。
- `userland/system/hsync_protect.inc` の `hsp_protected_names` (5 → **11 名**)。
  名前規則 (`hsp_name_protected` / `hsp_ancestor_protected`) と、hsync.c が
  `/etc` を `sys_ls` で列挙して実在名を拾うときの一致判定
  (`hsp_is_protected_basename` = 実体規則の入口) が同じ表を見る。

付随して `userland/system/hsync.c` の `HS_MAX_PROT` を **16 → 24**。これは
「`/etc` の実在名を何件まで集めるか」の上限で、越えると hsync は守れないとして
**同期を拒否する**。表が 5 名のときは余裕 11 だったが、リカバリ途中の `/etc` は
9 名 + `-wal` / `-shm` = 11 名になりうるので、16 のままだと大文字小文字違いの
別名が 5 つあるだけで通常同期が止まる。余裕を元と同じだけ保つ。

### D.2 試験の範囲と遮断

- `tools/tests/test_deploy_protect.py` — S0-D と同じ土台 (temp dir + mock)。
  `subprocess.run` は `FakeRun` に差し替わり、`mount` / `umount` / `losetup` /
  `mkfs.ext2` / `e2fsck` / `taskkill.exe` が呼ばれたら AssertionError で試験が
  落ちる。実配備・sudo・エミュレータ・`make` には一切触れない。
- `tools/tests/hsync_protect_host.c` (`test_hsync_protect.py` から実行) —
  `userland/system/hsync_protect.inc` を**実物のまま** `#include` してホストで
  走らせ、同じソースが `i386-elf-gcc` + PROGRAM_FLAGS でも通ることを別に見る
  ([C1] C89/GNU89、`-Werror`)。

新しい節は S3-D の 2 クラスだけで、既存 142 件 / 57 チェックには手を入れていない。

### D.3 RED → GREEN

RED は**試験だけ先に入れ**、`tools/deploy_protect.py` / `hsync_protect.inc` /
`hsync.c` を feat/gui `b1ceac6` の版 (5 名) に戻して取った。

```
RED  : test_deploy_protect.py   Ran 164 tests — FAILED (failures=21)   143 passed
       hsync_protect_host.c     114 checks — FAIL (39 failures)
GREEN: test_deploy_protect.py   Ran 164 tests — OK
       hsync_protect_host.c     114 checks — PASS (0 failures)
       + HOST GNU89 -Werror COMPILE PASS / TARGET i386-elf GNU89 COMPILE PASS
```

件数: Python 142 → **164** (+22)、C 57 → **114** (+57)。

RED で落ちた内訳 (Python 21 件):

| 試験 | RED の症状 |
|---|---|
| `S3RecoveryJudgement.test_table_has_all_recovery_names` | `PROTECTED_BASENAMES` に 6 名が無い (件数 5) |
| `…test_name_rule_covers_each_recovery_name` | `/etc/settings.db.recover-state` 等が名前規則に当たらない |
| `…test_uppercase_and_mixed_case_recovery_names` | 大文字 / 混在の別名も素通り |
| `…test_missing_recovery_name_is_still_protected` | 欠損した 7 名を通常配備が**作れて**しまう |
| `…test_symlink_alias_to_recovery_file` / `…dangling…` / `…hardlink…` | symlink / dangling / hardlink の別名から `.recover-state` 等に届く |
| `…test_protected_identities_collects_recovery_files` / `…uppercase_on_disk` | 実体検査 (inode 比較) の収集対象に入らない = 別名経由の上書きを拾えない |
| `…test_recovery_name_as_ancestor` | `/etc/settings.db.bak-journal/` が残骸ディレクトリのとき中へ書ける |
| `S3RecoveryDeployPaths.test_nhd_manifest_sync_keeps_recovery_generation` | manifest 配備が 7 名を別内容で上書き |
| `…test_sync_from_hostdrv_keeps_recovery_generation` | **通常同期**で HostDrv の別世代 (`.bak-journal` / `.recover-state` / `.failed`) が NHD の世代を潰した |
| `…test_sync_from_hostdrv_keeps_uppercase_recovery_names` | 宛先が大文字の実在名でも小文字の別世代で潰れた |
| `…test_sync_from_hostdrv_does_not_create_missing_recovery_files` | 宛先に無い 7 名を通常同期が**作った** |
| `…test_hostdrv_manifest_sync_keeps_recovery_generation` | HostDrv 配備が 7 名を上書き |
| `…test_hostdrv_clean_keeps_recovery_names` | `clean` が 7 名を消した |
| `…test_prune_nhd_keeps_recovery_names` / `…prune_hostdrv…` | 掃除が 7 名を stale として消した (実際に消した数が 8 件) |
| `…test_nhd_cli_copy_cannot_write_recovery_names` / `…rm…` | CLI から 7 名を書ける・消せる |
| `…test_recovery_directories_are_not_created` | `deploy.yaml` の `directories` に紛れると `etc/settings.db.new/` を mkdir した |

`S3RecoveryJudgement.test_near_miss_names_are_not_protected` は RED でも GREEN でも
通る (接頭一致で拾わないことの固定。`settings.db.bak2` / `.recover` /
`.recover-state.old` / `settings.dbbak` / `/etc/sub/settings.db.bak` /
`/settings.db.bak` は**通常配備の対象のまま**)。

RED で落ちた C のチェック (39 件) は、6 名 × 6 形 (名前規則 / `/etc` 列挙の一致 /
大文字の実在名 / 大文字の名前規則 / 祖先 / 正規化を挟んだ形) + `hsync -f etc`
連結 2 件 + 表の件数 1 件。`settings.db.bak` は S0-D で入っているので 6 形とも
RED から通る。

### D.4 「通常同期の前後で不変」の測り方

票が求めた形は `S3RecoveryDeployPaths` の
`test_sync_from_hostdrv_keeps_recovery_generation` に置いた。

1. 宛先 (NHD 側) の `etc/` に**ゲスト世代**の 7 名を置く (`GUEST_GEN`)。
2. HostDrv 側に**別世代**の `settings.db.bak-journal` / `.recover-state` /
   `.failed` を置く (`OTHER_GEN`、中身は別) + 通常のファイル
   (`etc/settings.tsv`、`bin/sh.bin`)。
3. `nhd_deploy.do_sync_from_hostdrv()` を stdout を捕まえて走らせる。
4. 判定: (a) 7 名の**存在一覧**が不変、(b) 7 名の **sha256 が不変**
   (`assertRecoveryIntact`)、(c) 出力に `protected:` が出る、
   (d) `FakeRun.touched_protected()` が空 = `cp` / `rm` / `mkdir` が
   保護対象名を宛先にしていない、(e) 通常のファイルは従来どおり写る。

大文字違いは `test_sync_from_hostdrv_keeps_uppercase_recovery_names` が
「宛先に `SETTINGS.DB.RECOVER-STATE` があり HostDrv に小文字の別世代がある」形で
見ている (上書きしない、かつ小文字の別名を**新規に作らない**)。
symlink / hardlink の別名は `S3RecoveryJudgement` の 3 件が判定そのものに当てて
いる (配備ツリーに symlink があれば `check_tree` が配備全体を拒否するので、
経路側ではなく判定側で固定する)。

### D.5 この試験が言えないこと ([V4])

- **実機 / エミュレータでの受入は未実施**。コーダーは配備・エミュレータ・`make`
  を実行しない。`make deploy` / `make deploy-nhd` / ゲストの `hsync` を実際に
  走らせた結果は何も言っていない。
- `hsync` の**実体規則** (`sys_stat` の inode 比較、`scan_protected_entities` /
  `is_same_as_protected`) はホストでは踏んでいない。純関数に出せる
  `hsp_is_protected_basename` までが範囲。`HS_MAX_PROT` を 24 にした効果
  (リカバリ途中の `/etc` で同期が止まらないこと) も**ゲストでは未確認**。
- `hsync.c` は `test_hsync_protect.py` が `i386-elf-gcc` でコンパイルが通ることを
  見るだけで、リンク・実行はしていない。`HS_MAX_PROT` 変更分の static 領域
  (+576 B) が外部プログラムの帯域に収まることは机上 (`make external` は PM)。
- S0-D から引き継いだ限界はそのまま: bind mount による `<root>/etc` の別名は
  検出できない。配備ツリーに symlink があれば配備全体を拒否するだけで、
  「symlink を含むツリーへ安全に配備できる」とは言っていない。
  `/etc/settings.db*/` と `/lost+found/` の**中**は走査していない。
- リカバリ本体 (S3-I) の各段が実際にこの 7 名を作る / 消すことは、この節では
  一切検査していない (§I の担当)。ここが固定したのは「**通常配備がその名前を
  掴まない**」ことだけ。

---

# S3 — リカバリのホスト TDD 記録

票: [`docs/tasks/settings/TASK_S3.md`](../../docs/tasks/settings/TASK_S3.md) 第 5 版。
契約の正典は [`S0_FOUNDATION.md`](../../docs/tasks/settings/S0_FOUNDATION.md) §6
(明示リカバリ契約)、[`TASK_S2.md`](../../docs/tasks/settings/TASK_S2.md) §1-1 (b) の meta 検査 SQL と
§1-7 (f) の rename 両名検査、[`TASK_S0.md`](../../docs/tasks/settings/TASK_S0.md) (配備保護)。

---

# 節 I — `install --recover-settings` / `--revert-settings` (票 S3-I §1)

走らせ方:

```bash
python3 -B tools/tests/test_install_recover.py             # 14 ケース
python3 -B tools/tests/test_install_recover.py --target    # + i386-elf の -Werror コンパイル
python3 -B tools/tests/test_install_recover.py --sanitize  # + ASan
python3 -B tools/tests/test_install_recover.py happy chain # ケース指定
```

コーダーの範囲はホスト TDD までなので、**ゲスト受入 (票 §4 の I1〜I7) は未実行** ([V4])。
`make all` / `make check` / 配備 / エミュレータも触っていない。`make check` への登録行は
報告に書くだけで、`build/sdk.mk` には PM が入れる。

## I-0. 組み方

`tools/tests/install_recover_host.c` は `cfg_host.c` / `kapi_db_v50_host.c` と同じ作法で
「実物だけ」を組む。

| 実物 | 模型 (その形でしか作れないものだけ) |
|---|---|
| `userland/system/install_recover.inc` (install.c が `#include` するのと同じソース) | `exec` のポインタ検証 `ring3_user_range_ok` |
| `kapi/kapi_db.c` (KAPI v50 の本物 — `db_open_existing` / `db_prepare_only` / `db_step` / `db_close` / `db_error_code`) | SHM の置き場 (`test_shm`) |
| `lib/sqlite3/sqlite3.c` + `lib/sqlite3/os32_sqlite_vfs.c` | `stat` の三値 / UNKNOWN / `st_ino` 共有 / `st_dev` |
| `fs/vfs_fd.c` (`vfs_open` / `vfs_read_fd` / `vfs_write_fd` / `vfs_close`) + RAM backend | `rename` の 2 つの中途半端、`read` の負 / short write / 中身違い |
| — | `unlink` / `sync` / `db_close` / `db_open_existing` の失敗 |

ホストのファイルシステムには 1 バイトも触らない。マスタ (`/etc/settings.db`) も RAM fixture の
上に実 SQLite で毎回作り直す (`meta` 1 行 + `settings` 12 行)。

回復モードが呼ぶ KAPI は 1 枚の関数ポインタ表 `RecoverOps`
(`userland/system/install_recover.inc` の先頭) に集めてある。ゲストの実体は `install.c` の
`rop_*` (`api->sys_*` / `api->db_*`)、ホスト試験は同じ表を贋物で埋める。
**本体は 1 行も分岐していない** (`#ifdef` も無い)。

`st_dev` の復号は `fs/vfs.c` の `vfs_mount_dev_of` と同じ式 `(dev_type << 8 | unit) + 1`
(種別は `fs/vfs.h` の `VFS_DEV_HD` = 0 / `VFS_DEV_FD` = 1)。外部プログラムからカーネルヘッダは
見えないので `.inc` 側に写しがあり、試験も同じ式で値を作って**両方が同じ規則で動くこと**を見る。

## I-1. ケース (14 本)

| ケース | 票の段 | 何を固定するか |
|---|---|---|
| `media` | 1a | `drive` は `hd0` だけ / HDD ブートからの実行を拒否 (受入 I1、DB は不変) / `st_dev == 0` も拒否 / `/hd0` が hd1・FDD なら `target /hd0 is not hd0` / 未マウントなら回復専用に `sys_mount` / mount 失敗で何もしない |
| `scan` | 1b | 9 名の三値。UNKNOWN を「無い」に丸めない (`stat failed (<name>)`) / `(st_dev, st_ino)` の共有で停止 (何も消さない) / `st_ino == 0` は判定不能で停止 / `.new` `.new-journal` の残骸で停止 (消さない) |
| `gate` | 1b | 印の phase の白リスト。`backup` / `switching` / `switched` / `failed` / `reverting` は全部停止、`done` と印無しだけ通る / 壊れた印は `recover-state unreadable` で停止 (消さない) / 印の書式が往復する / 印の open・short write・読み戻し不一致はすべて失敗 |
| `master` | 1c | マスタ欠損・非 SQLite・`meta` 表欠落・`meta` 2 行はすべて `master unreadable` (HDD 側は不変) / 検査 close の失敗は `master close failed` / 保証の上限 (integrity_check は OMIT) を表示に出す |
| `approve` | 1d | `N` / ESC で 9 名が 1 バイトも変わらず終了 0 (受入 I5)。承認文はマスタの版と件数を出す |
| `happy` | 1e/1f | 受入 I2 / I3 / I4。本体 + hot journal → `.bak` と `.bak-journal` の**対**、元の journal は消える、本体はマスタと同一、マスタは不変、印は `phase=done orig=present journal=present size=1406` / 本体欠損なら `.bak` を作らない / 孤立 journal も対の一部として写す / 旧 `.bak*` は承認済み 1 世代として消える |
| `backup_fail` | 1e | 旧 `.bak` が消せない / `read` の負 / short write / 長さは合うが中身が違う / journal の退避失敗 / 印が書けない — **どれでも元の対は 1 バイトも変わらず、今回作ったものだけが消える** |
| `newfail` | 1f | `.new` の short write / 中身違い / DB として開けない → `.new` を消して元は無傷 (`.bak` と印は残る) / **検証 close の失敗は `.new` を消さず REBOOT を案内** / その次の recover は印の門で止まる |
| `switch` | 1f | hot journal が消せなければ破壊段に入らない (写しは対で残る) / rename の**両名残存**は消さずに停止 (元の内容は `.bak` に、phase は `switching`) / **置換先 unlink 後の新名追加失敗**は「旧本体が残っている」と仮定せず写しから対で復元 / その復元が失敗したら `phase=failed` で停止し退避対を保護 / rename 後の stat が UNKNOWN なら何も消さない |
| `record` | 1f(7) | `sync` 失敗 / reopen 失敗 / reopen の close 失敗 — どれでも `phase=switched` のままで `REBOOT … install --revert-settings hd0` を案内し、勝手には戻さない |
| `revert` | 1g | 印が無ければ `nothing to revert` / I2 の後は現在の対が `.failed*` へ、`.bak` が本体へ / I3 の後は `orig=missing` で本体を消して欠損に戻す / **切替後に生まれた新世代の journal を旧 DB に付けたまま戻さない** / `.new` 残骸があっても進む / `N` で何もしない / `.bak` が無ければ何も触らない / 旧 `.failed*` は 1 世代置換 |
| `revert_fail` | 1g | 本体の復元失敗 → `phase=reverting` のまま・`.bak` は不変・次の recover は門で止まる・再 revert で復帰 / `.failed` への退避失敗は作ったものだけ消す / `sync` 失敗も `done` にしない / journal の復元失敗で**対を分離したままにしない** |
| `chain` | 1h | (1) 失敗 → 再実行が門で止まる → revert → recover / (2) recover → revert → recover (1 世代前が `.bak` に) / (3) recover 成功 → revert 途中失敗 → recover が門で止まり `.bak` 不変 → 再 revert / (4) `.new` close 失敗 → 次回停止 → revert → `rm` → recover (票 §1f 4 の正式手順) |
| `pure` | — | 印の書式の受理範囲 (phase 不正 / orig 不正 / size 非数字 / 欄欠落 / 空) / 16KB 境界をまたぐコピーとバイト比較 / 1 バイト違い・長さ違いを検出 / `read` の負は EOF ではなく失敗 / short write は失敗 |

## I-2. RED → GREEN の確かめ方 (突然変異)

「全部 PASS」だけでは試験が何も掴んでいない可能性があるので、`install_recover.inc` に
**17 種の意図的な後退**を 1 つずつ入れて、どのケースが落ちるかを確かめた (全部が少なくとも
1 ケースで落ちる = MISSED 0)。

| 入れた後退 | 落ちたケース |
|---|---|
| `read` の負を EOF に丸める (現行 `copy_file` と同じ) | `pure` |
| short write を見逃す | `pure` `newfail` |
| 退避のバイト比較を省く | `backup_fail` `switch` `revert_fail` `chain` |
| 印の phase の門を外す | `gate` `newfail` `revert_fail` `chain` |
| 起動媒体の種別を見ない | `media` |
| `inode` 共有の検査を外す | `scan` |
| `stat` の UNKNOWN を「無い」に丸める | `scan` |
| 両名残存で `.new` を消す | `switch` |
| `.new` だけ残ったとき「旧本体が残っている」と仮定する | `switch` |
| `db_close` の失敗を無視する (`.new` を消す) | `newfail` `chain` |
| journal を退避せずに消す | `happy` `backup_fail` `switch` `revert` `revert_fail` `chain` |
| revert の `phase=reverting` を破壊段の後にする | `revert_fail` `chain` |
| revert の前提検査 (`.bak` の存在) を外す | `revert` |
| 退避失敗で元の対も消す | `backup_fail` |
| `sync` / reopen の失敗でも `phase=done` にする | `record` |
| `.new` の残骸でも進む | `scan` `chain` |
| revert が現在の journal を消さない (別世代と混ざる) | `revert` |

## I-3. 実装で票からずらした点 / 票が決めていなかった点

1. **`RecoverOps` の表に `db_column_*` は入れない**。列は `libos32cfg` と同じく SHM を直接読む
   (`shm()` が表の 1 本)。KAPI の口は `db_open_existing` / `db_prepare_only` / `db_step` /
   `db_finalize` / `db_close` / `db_error_code` + `shm`。
2. `main()` を `void` から `int` に変えた。票の「終了 1 / 終了 0」を返す口が他に無い
   (`cfg.c` と同じ形)。通常インストール経路は最後に `return 0`、メモリ確保失敗だけ `return 1`
   で、**それ以外の振る舞いは変えていない** (それまでは戻り値が未定義だった)。
3. 票が文言を決めていなかった失敗に付けた文言:
   `stat failed (/)` / `stat failed (/hd0)` / `stat failed (/hd0/etc)` / `cannot mount /hd0` /
   `ambiguous: <name> has no inode - needs manual recovery` (`st_ino == 0`) /
   `cannot remove old recover-state` / `recover-state write failed` /
   `backup failed (<name>)` / `copy failed (settings.db.new)` / `verify failed (settings.db.new)` /
   `needs manual recovery: cannot stat after rename` / `cannot remove current journal` /
   `cannot remove old failed copy` / `save failed (<name>)` /
   `rename left settings.db.new only - restored from settings.db.bak; nothing was replaced`。
   票が決めている文言 (受入が読むもの) はそのまま。
4. 票 §1f の 6 で「`.new` だけ残って**復元に成功した**とき」の印を決めていなかった。
   元の対がそのまま戻っているので `phase=done` にして終了 1 とした (次の recover は通常に進める)。
   復元に失敗した側は票どおり `phase=failed`。
5. 票 §1f の 6 は「`settings.db` だけ / `.new` だけ / 両方」の 3 通りしか書いていない。
   **両方 ABSENT** も「`.new` だけ」と同じ復元経路に入れた (写しから戻す。`.new` の unlink は
   存在するときだけ)。
6. 票 §1g の 6 は失敗の文言を `revert failed at <段> (phase stays reverting)` と決めているので、
   段ごとの具体的な文言 (`restore failed at <name> - …` など) の**後に**この 1 行も出す。
7. `--revert-settings` を HDD ブートで実行したときの文言も
   `recover-settings must run from the install floppy` のまま (票 §1g が 1a の検査をそのまま
   使うと書いているため)。
8. 印 (`settings.db.recover-state`) は 1 行 + LF。書式は
   `phase=<p> orig=<present|missing> journal=<present|absent> size=<n>\n` で、読み側は
   **厳密** (欄の欠落・未知の phase・非数字の size はすべて「壊れている」)。

## I-4. 踏めなかったもの ([V4])

- ゲスト受入 I1〜I7 (NP21/W、FDD ブート) は**未実行**。コーダーの範囲外。
- 隔離された SQLite 接続 (`kapi_db.c` の F1) が `.new` の inode を掴んだままかどうかは
  ユーザーランドから検出できない。ホスト試験が固定しているのは「`db_close` が失敗したら
  `.new` を消さず、再起動を前提にした文言を出す」ところまでで、**再起動しなければ本当に
  危ないこと自体は試験していない** (票 §1f 4 の限界そのまま)。
- `PRAGMA integrity_check` はカーネルの SQLite で OMIT (`lib/sqlite3/os32_sqlite_config.h:51`)。
  検査は meta + 全行走査 + バイト一致までで、索引や自由ページの健全性は見ていない。
- ext2 の rename が実際にどの中途半端で止まるかは注入した 2 形 (両名残存 / 置換先 unlink 後の
  新名追加失敗) だけ。実 FS での再現は受入 I6 の領分。
