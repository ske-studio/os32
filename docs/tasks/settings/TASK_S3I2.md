# S3-I2 — FDD からの新規インストールの修正 (lz4 カーネル + `/boot`) と使い捨て NHD の道具

状態: **設計 第 1 版 (Codex 設計レビュー 往復 1/3 待ち)**。ユーザー決裁 2026-09-14「a から」。前提: S3 完了 (main `24cfcf7`)。
経緯: TASK_S0 §3 B10 → TASK_S3 §7 (残ゲート)。現行の `install` (無印) は `/kernel.bin` を必須とし LBA 6 へ生書きするが、FDD イメージは `/VMKRNL.LZ4` + ローダ v3 (`/sys/loader_h.bin` = `boot/loader_hdd.bin`、ext2 の `/boot/vmkernel.lz4` を読む) を収録するので、**FDD からの新規インストールは Phase 1 の `Missing /kernel.bin` で止まり `/etc` コピー (settings.db の seed) まで到達しない**。`cdinst` (CD) は lz4 / `/boot` 対応済み (`cdinst.c:240` の注、`:510` の `mkdir /hd0/boot`)。
正典: [TASK_S0.md](TASK_S0.md) §3 B10、[TASK_S3.md](TASK_S3.md) §7、`docs/08_build.md` §8-4 (配備 3 経路)、`build/image.mk` (FDD の中身)、スキル `os32-emu-config` (ini は PM だけ、実装と適用の承認を分ける)、memory `os32-np21w-launch` (FDD は引数、HDD は ini のみ)。
規約: [C1] C89、[D2] (NHD 上書き / ini 変更はユーザー承認、使い捨てだけを対象にする)、コーダーは worktree + ホスト TDD のみ。

## 0. 範囲と分担

| 票 | 範囲 | レーン | 触るファイル |
|---|---|---|---|
| **S3I2-I** | `userland/system/install.c` の通常インストール経路 (§1)。`--recover-settings` / `--revert-settings` (S3) は不変 | C (system) | `userland/system/install.c`、`tools/tests/install_fresh_host.c` / `test_install_fresh.py` / `s3i2_tdd.md` §I |
| **S3I2-T** | (a) `tools/np21w_ini.py` / `np21w_trial.py` に **`HDD1FILE`** を足す (§2)。(b) `tools/mk_blank_nhd.py` = 使い捨て NHD (NHD ヘッダ 512B + 全ゼロ、大きさ指定) を作る (§2) | ツール | `tools/np21w_ini.py`、`tools/np21w_trial.py`、`tools/np21w_ini_live.py` (触るなら)、`tools/mk_blank_nhd.py` (新規)、`tools/tests/test_np21w_ini*.py` / `test_mk_blank_nhd.py`、`tools/tests/np21w_trial_tdd.md` |
| PM | 受入 (§3): 使い捨て NHD の作成 → ini trial ([D2]) → FDD ブート → `install` → trial ini で HDD ブート → 検証 → ini 復元。`docs/INSTALL.md` / `08_build.md` の手順更新 | PM / テスター | — |

含めない: `cdinst` (CD) の変更、FDD イメージの中身の変更 (現行のまま使う)、パーティション / ジオメトリの見直し、`install` の対話 UI の作り直し。

## 1. `install` (無印) の修正 (S3I2-I)

現行 Phase 1〜3 のうち変えるのは **kernel.bin の段だけ + `/boot` へのコピー**:
- Phase 1 (ブートセクタ): `/sys/boot_hdd.bin` → LBA 0 (ジオメトリを patch)、PT → LBA 1、`/sys/loader_h.bin` → LBA 2+ は**そのまま**。**`/kernel.bin` の読込と LBA 6 への生書きを削除** (ローダ v3 は ext2 の `/boot/vmkernel.lz4` を読む。LBA 2..17 はローダ領域で LBA 6 と衝突する — cdinst の注と同じ)。Phase 1 の前に **`/VMKRNL.LZ4` の存在と大きさ** (`sys_stat`、> 0、`FILE_BUF_SIZE` 制限なし = ストリームコピー) を確認し、無ければ `Missing /VMKRNL.LZ4 on the install media` で中止 (**何も書かない**。承認の前に検査する)。
- Phase 2 (ext2 format): そのまま。
- Phase 3 (コピー): `mkdir /hd0/boot` を追加し、**`/VMKRNL.LZ4` → `/hd0/boot/vmkernel.lz4`** を `copy_file` (16KB バッファのストリーム、short write は失敗) でコピーして**長さ一致**を確認 (不一致は `[FAIL]` + 終了 1)。続けて既存の `copy_directory` で `/sys` `/bin` `/sbin` `/etc` (**`/etc/settings.db` = マスタの seed**、S0-T の T1 の完成) を写す。`/etc/profile` は FDD の `assets/profile_fdd` 由来なので**写さない** (HDD 用の profile は別: 現行どおり `/etc` を丸ごと写すと FDD 用 profile が入る — 現状の挙動を確認して票に書く。S3I2-I は「`profile` は写す (現行どおり)」を既定にし、問題があれば non-blocker として次段)。
- 終了コード: 成功 0、各段の失敗は 1 (現行は `goto end` で 0 を返す箇所がある → 失敗を非ゼロに統一。回復モードの規約と揃える)。
- 表示: `[1/3] Writing Boot Sectors` / `[2/3] Formatting` / `[3/3] Copying files` は維持し、`Written KERNEL (LBA 6)` の行を `vmkernel.lz4 -> /boot (<n> bytes)` に置換。バナーを `v4.1`。
- ホスト TDD (`install_fresh_host.c`): 通常経路の Phase 1〜3 を KAPI 贋物 (ide_write_sectors の記録、ext2_format、mount、mkdir、open/read/write、stat) で通し、(1) `/kernel.bin` を読まない・LBA 6 に書かない、(2) `/VMKRNL.LZ4` 欠損で Phase 1 の前に中止して LBA 0/1/2 に書かない、(3) `/boot/vmkernel.lz4` が長さ一致で置かれる、(4) short write / read 負で失敗 1、(5) `/etc/settings.db` が写る、(6) 回復モードの分岐が不変 (S3 の `install_recover_host` を再実行)。RED→GREEN を `s3i2_tdd.md` §I に。

## 2. 道具 (S3I2-T)

### 2a. ini ツールの `HDD1FILE`
- `tools/np21w_ini.py` の `ALLOWED` は現在「値の白リスト」(固定文字列)。`HDD1FILE` は**パス**なので規則を足す: 値は `NP21W_DIR` 直下のファイル名 (`[A-Za-z0-9_.-]+\.nhd`、ディレクトリ区切り無し) を **Windows の絶対パス** (`<NP21W_DIR の Windows 表記>\<name>`) に展開して書く。ツールは (i) 名前の規則、(ii) `wslpath` で対応するホスト側ファイルが**存在する**こと、(iii) 既存の `HDD1FILE=` 行が 1 本だけであることを検査し、他は fail closed (現行の `transform()` の作法: 対象 section の全キー存在、重複拒否、バイト単位の差し替え、CP932 / 混在改行を壊さない)。
- 適用経路は **`np21w_trial.py` (通常終了 → 新規 trial ini → 明示起動) だけ**。`np21w_ini_live.py` (ライブ変更) には足さない (作業 NHD を差し替える事故を作らない)。復元は trial の `restore` (原本は触っていないので不要 = 通常の ini で起動し直すだけ)。
- ホスト TDD: `transform()` の `HDD1FILE` 追加 (名前規則 / 存在 / 重複 / 別 section / CP932 保持)、`make_plan()` に `HDD1FILE` を含む計画の束縛、`np21w_trial_tdd.md` に追記。実 ini・実プロセスには触れない。
- **適用の承認は別** (スキル §0): PM が受入のたびに [D2] で承認を取る。

### 2b. `tools/mk_blank_nhd.py`
- `--out <path> --size-mb <n>` (既定 200、`os32.nhd` と同じ)。NHD ヘッダ (512B: `T98HDDIMAGE.R0` 署名 + `dwHeadSize` 512 + `dwCylinder` / `wHead` / `wSect` / `wSectLen` を `os32.nhd` のヘッダから写す = `tools/nhd_deploy.py` の `NHD_HEADER_SECTORS` / ジオメトリ定数と同じ) + 全ゼロ。既存ファイルは `--force` 無しでは上書きしない。**書き込み先は `NP21W_DIR` 直下に限定**しない (ホストのどこでも作れるが、ini に載せるのは 2a の規則で `NP21W_DIR` 直下だけ)。
- ホスト TDD: ヘッダのバイト列、大きさ、`--force` 無しの拒否、ジオメトリが `nhd_deploy.py` の定数と一致。

## 3. 受入 (PM / テスター)

| ID | 手順 | 合格 |
|---|---|---|
| F1 | ホスト: `mk_blank_nhd.py --out <NP21W_DIR>/os32_fresh.nhd` → `np21w_trial.py` で `HDD1FILE=…\os32_fresh.nhd` の trial ini を作る ([D2]) → trial ini + `os32_boot.d88` 引数で NP21/W を起動 (FDD ブート、HDD = 空) | FDD ブートが上がる、`ls /hd0` は未マウント (ext2 無し) |
| F2 | `install` → `WARNING … ERASED` に `Y` | `[1/3]` IPL / PT / LOADER、`[2/3] Format OK`、`[3/3]` に `vmkernel.lz4 -> /boot (470761 bytes)` と `/sys` `/bin` `/sbin` `/etc` の `[OK]`、終了 0 |
| F3 | NP21/W を trial ini だけで起動 (FDD 引数なし = HDD ブート) | `ver` が応答、kselftest 87 / 0、`ls -l /boot/vmkernel.lz4` = 470,761 B、`cfg status` = `OK schema_version 1` (**seed 済み** = S0-T の T1 完了)、`cfg list` に tsv の 3 行 |
| F4 | F3 の NHD に対し通常の配備 (`nhd_deploy.py` を trial の NHD へ向ける手段が無いので**やらない**。配備保護の確認は S0-D 済み) | — (記録のみ) |
| F5 | 後始末: NP21/W を止め、通常の ini で起動 (作業 NHD)、`os32_fresh.nhd` は残す (次回の使い捨てに再利用、または削除) | 作業 NHD が無傷 (`cfg status` OK、`ls /boot`) |
| F6 | 回帰: S3 の I2 (FDD ブートで `--recover-settings`) が変わらず通る (同じ install.bin) | 合格 |

## 4. レビューで見てほしい点
1. Phase 1 で `/kernel.bin` を外しても IPL / ローダ v3 が `/boot/vmkernel.lz4` を見つける前提 (`boot/loader_hdd.bin` = v3、`HDD_PARTITION_LBA`、ext2 の位置) が FDD の同梱物と一致するか。
2. `/VMKRNL.LZ4` の事前検査が承認前に置かれ、失敗で 1 バイトも書かないか。lz4 の大きさ (470KB) がストリームコピーで扱えるか (現行 `copy_file` は 16KB バッファのループ = 可)。
3. `/etc` 丸写し (FDD 用 `profile` が HDD に入る) の是非。
4. ini ツールの `HDD1FILE` 規則 (パス値の fail closed、trial 限定、ライブ変更に足さない) がスキル §0 / §1 と矛盾しないか。CP932 パスの扱い (`NP21W_DIR` が ASCII のみでよいか)。
5. `mk_blank_nhd.py` のヘッダとジオメトリ、NP21/W が空イメージを IDE として認識するか (`ide_identify` の total_sectors)。
6. 受入 F1〜F6 で作業 NHD を触らないことが保証されるか。

## 5. ユーザー判断が要る点
- なし (使い捨て NHD の作成と trial ini の適用は受入時に [D2] で個別に承認を取る)。
