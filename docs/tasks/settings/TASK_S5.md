# S5 — 設定レジストリの実測と受入 (DESIGN §6)、S4 の残件

状態: **第 1 版 (実測票。コードは小さく 2 点、Codex レビューはその 2 点だけ)**。前提: S0 / S2 / S4 完了 (main `9d455e0`)。
正典: [DESIGN.md](DESIGN.md) §6 (実測項目)、[S0_PLAN_2026-09-13.md](S0_PLAN_2026-09-13.md) §2 (S5 = 実測と受入、PM / テスター)、[TASK_S2.md](TASK_S2.md) §8 (C7 の pool 復帰が未計測: `db_test` が継承バグで落ちる)、[TASK_S4.md](TASK_S4.md) 状態行の残件 4 件。
規約: コーダーは worktree + ホスト TDD のみ。PM は測定の観測と判定、テスターが make / 配備。ini は [D2] (§3 R2 参照)。

## 0. 分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S5-C** | (1) 計測プログラム `userland/tests/cfg_bench.c` (CPL=3、`libos32cfg` をリンク): `cfg_bench [n] [m]` = n 回 (既定 50) の「`cfg_open(RO)` → m 件 (既定 20) の `cfg_get_int/text` (存在する 3 キーを巡回、無いキーも混ぜる) → `cfg_close`」。各回の tick (`get_tick`) と `db_mem_used` を 10 回ごとに 1 行、最後に **最小 / 最大 / 平均 tick、`db_mem_used` の開始 / ピーク / 終了**、状態 (cfg_status) と失敗回数を出す。`cfg_bench -w` は「`cfg_open(RW)` → begin → `cfg_set_int` 1 件 → commit → close」を n 回 (書き戻しの tick)。(2) `cfg status` の出力に `pool <db_mem_used> B` を足す (`cfg status` = `OK schema_version 1 pool 12345 B`。既存の受入 C2 の文言はこの追記を許す) | C | `userland/tests/cfg_bench.c`、`userland/cmds/cfg.c` (status の 1 行)、`tools/tests/cfg_host.c` / `test_cfg.py` (status の整形)、`tools/tests/s5_tdd.md` |
| **S5-W** | S4 の残件 4 件 (non-blocker): S20(c) を 2 本目の set 失敗 (`[0, -1]`) に、S18 の mock を「2 本目の get で ERROR へ遷移」に、S09b / S14 のコメントを「実 park 遷移ではない」に限定、TAB で Cancel に焦点表示しても RETURN が OK になる不一致 (Input ダイアログと同様に TAB でボタン焦点を動かさない) | W | `userland/gshell/src/modal.rs`、`userland/gshell/host/{settings_tests.rs, mocks.rs}`、`tools/tests/s4_tdd.md` |
| PM | `build/programs.mk` (cfg_bench の明示規則、`$(LIBCFG_OBJ)`)、`build/app.conf` (`userland/tests/cfg_bench 50 0`)、`userland/deploy.yaml` (`userland/tests/*.bin` の glob に乗るか確認)、`tools/emu_agent/agent.py`、測定の実施と記録 (§2)、3 バックエンド回帰 (§3) | PM / テスター | — |

## 1. 静的確認 (PM、コード変更なし)

- **同期** (DESIGN §6「同期」): `os32Sync` (lib/sqlite3/os32_sqlite_vfs.c:254) は `vfs_sync()` を呼び、`vfs_sync` は全マウントの `ops->sync` (= `ext2_sync`) を呼ぶ。`ext2_sync` が dirty バッファをデバイスへ書き戻すなら、`cfg_commit` の後に別の sync は不要。書き戻さない (no-op) なら `cfg_commit` の末尾に `vfs_sync` KAPI を足す (S5-C に追加)。判定は §2 の M3 に書く。
- **ジャーナル** (DESIGN §6「ジャーナル」): DELETE journal の回復は SQLite 本体。S2 の契約では **hot journal は自動回復せず CORRUPT** (BUSY_RECOVERY → CORRUPT、リカバリは S3)。強制終了試験 (使い捨てイメージ) は S3 の領分にし、S5 では「journal が残った状態の検出」だけを M2 で確かめる。

## 2. 実測 (PM / テスター、ゲスト = NP21/W 15MB pc98、386 相当ではないので tick は相対値)

| ID | 項目 | 手順 | 合格 / 記録 |
|---|---|---|---|
| M1 | プール | `ime on` (FEP 辞書常駐) → `cfg_bench 50 20` | `db_mem_used` の終了値が開始値に戻る (差 0)、失敗 0、`-2` (pool 枯渇) が出ない。ピークを記録 |
| M2 | ジャーナル | `cfg status` OK → `cp /etc/settings.tsv /etc/settings.db-journal` (非ゼロの journal を偽装) → `cfg status` / `cfg get` / `cfg init` → `rm` → `cfg status` | 偽装中は `CORRUPT` (hot journal)、get は既定値、`cfg init` は `needs recovery: journal present` で拒否、rm 後に OK に戻る。gshell も起動時通知 CORRUPT を出す (G4 と同じ経路) |
| M3 | 同期 | §1 の静的確認 + `cfg set` の直後に NP21/W を強制終了せず**リセット** (`os32-cycle` の emu 再起動と同じ = 電源断相当) → `cfg get` | 値が残る (ext2 が書き戻していれば)。残らなければ `cfg_commit` に `vfs_sync` を足して再測 |
| M4 | 速度 | `cfg_bench 50 20` (FEP なし / あり) の平均 tick、`cfg_bench -w 20` の平均 tick、gshell の `load` / `save` tick (S4 G7: 24t / 59t) | 記録。gshell 起動の体感 (load ≤ 50t) を目安 |
| M5 | 大きさ | ホスト: 300 件の合成 tsv (`tools/tests/mk_size_fixture.py` 相当を PM が scratch で) → `mk_settings_db.py` → サイズ | 64KB 以内 (page 1KB)。記録 |
| M6 | メモリ | M1 の `db_mem_used` ピーク (FEP 常駐 + 設定 DB) と、`cfg_bench` を CUI / GUI 端末の両方で | 記録。OOM (`-2`) が出ない |

## 3. 回帰と受入

| ID | 内容 | 合格 |
|---|---|---|
| R1 | 9801 (現行 `GFX=pc98`): `gui_gate.py v11` / `v12g1` / `v12g4`、S4 の G2 (Settings で色変更) | 全台本が従来どおり通る |
| R2 | PEGC / Cirrus: ゲスト側 `gfxmode pegc` / `gfxmode cirrus` → リセット → `hal_test` で backend を確認 → R1 と同じ台本 | **NP21/W が 9821 モデルで起動していることが前提** (現在の `hal_test` は `pc98 (planar 4bpp)`)。ini の変更が要るなら [D2] でユーザー承認を取ってから (スキル `os32-emu-config`)。承認が無ければ R2 は未実施と記録 |
| R3 | regress 6 本 + kselftest | 87 / 0 |
| R4 | S4 残件 (S5-W) のホスト試験 | gshell host 95 本 + 追加分 GREEN、TAB の焦点表示が RETURN の動作と一致 |

## 4. レビュー

Codex (枯渇時は Fable 5.1 サブエージェント、ROLES §5) に S5-C (`cfg_bench.c`、`cfg status` の 1 行、必要なら `cfg_commit` の sync) と S5-W の差分だけを 1 往復で見てもらう (実測票なので設計レビューは無し)。観点: cfg_bench が open〜close の間に yield しない、計測の tick の取り方、`db_mem_used` の読み方、`cfg status` の後方互換、S4 残件の直し方。

## 5. 記録

実測値は本票 §6 に PM が追記し、DESIGN §6 の表を「実測済み」に更新する。S3 (リカバリ) / S6 (tar) / F3a-c の着手判断は実測の後にユーザーへ。

## 6. 実測の記録 (追記)

- **M3 (静的、2026-09-13)**: `os32Sync` → `vfs_sync()` → `ext2_sync()` は superblock と group descriptor を書く (`fs/ext2_super.c:274`)。データ / inode / bitmap のブロックは `ext2_write_block` → `dev_blk_write_lba` で**書いた時点でデバイスへ出る** (write-through、`fs/ext2_super.c:43`。ext2 にダーティキャッシュは無い)。→ `cfg_commit` の後に別の sync は不要。動的確認 (2026-09-13): `cfg set gshell desktop/color int 9` → NP21/W を**ハードリセット** (MCP `emu_reset`、電源断相当) → 起動 18 秒後 `cfg get` = **9**。書いた時点で NHD に出ている。合格 (追加の sync は不要、`cfg_commit` は変更しない)。
- **M2 (ゲスト、2026-09-13)**: `cp /etc/settings.tsv /etc/settings.db-journal` (1406 B の偽 journal) → `cfg status` = **`CORRUPT sqlite=261`** (BUSY_RECOVERY)、`cfg get … 99` = 99 (既定値)、`cfg init` = `already exists` (本体が存在するので (a) の判定が先に効く — S2 §1-7 の順どおり。journal の拒否文言は本体が無いときに出る)、`rm` 後 `cfg status` = OK、get = 12。合格。
- **M1 / M4 / M6 (ゲスト、2026-09-13、配備 `5ccf6b8`、NP21/W 15MB pc98)**:
  - `cfg status` の pool: FEP 無し **24,768 B** (接続保持中)、`ime on` 後 **63,552 B**。
  - `cfg_bench 50 20` (FEP 無し): 1 回 (open + 20 get + close) = **114〜115 tick** (avg 114、total 5711)、pool start 0 → peak 24,832 → **end 0** (完全に戻る)、failures 0。
  - `cfg_bench 20 20` (FEP 常駐): **113〜114 tick**、pool start 38,784 → peak 63,616 → **end 38,784** (辞書ぶんを残して戻る)、failures 0。**M1 合格** (差 0、`-2` 無し)。
  - `cfg_bench -w 10` (FEP 常駐): 1 回 (open RW + begin + set + commit + close) = **58〜59 tick**、peak 64,640 B。
  - M4: get 1 件あたり約 4.5 tick、open + close で約 20 tick (gshell の起動時 load = 24t と整合)。gshell 起動の目安 (≤ 50t) 内。tick は NP21/W の実行速度 (386 相当ではない) での相対値。
  - M6: FEP 辞書 (約 38.8KB) + 設定 DB (約 25KB) の共存ピーク **64,640 B** / 384KB プール。OOM 無し。GUI 端末での実行は R1 の後に追記。
  - 注: `/api/cmd` は約 30 秒で EOT を待ち切るので、50 回の read (57 秒) は応答がずれる。実測は 20 回以下で回す。
- **R3 (ゲスト、2026-09-13)**: regress 6 / 6 (`s5reg`)、kselftest 87 / 0。
- **R1 (ゲスト、2026-09-13、9801 `GFX=pc98`)**: `gui_gate.py v11` / `v12g1` / `v12g4` すべて RESULT OK、各台本の `CUI back: ok` (6 項目の Start メニューで `ROW_CUI` が CUI mode に当たる)、`system.cfg: GUI=0`。S4 の G2 は配備 2 回目 (TASK_S4 §10d) で確認済み。
- **M5 (ホスト、2026-09-13)**: 合成 tsv 300 件 (int 100 / text 96B 100 / blob 64B 100、scope `app:bench`) → `mk_settings_db.py` → **29,696 B** (page 1KB)。64KB 以内で合格。
