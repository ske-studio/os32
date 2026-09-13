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
