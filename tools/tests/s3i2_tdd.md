# S3I2 — FDD からの新規インストール (lz4 + `/boot`) と使い捨て NHD の道具のホスト TDD 記録

票: [`docs/tasks/settings/TASK_S3I2.md`](../../docs/tasks/settings/TASK_S3I2.md)。

3 票 (S3I2-I / S3I2-T / S3I2-K) が同じファイルに書くので、節を票ごとに分ける。
**自分の節だけ**を書き、他の節には触れないこと。

- `## I.` — `install` (無印) の通常インストール経路 (S3I2-I)
- `## T.` — ini ツールの `HDD1FILE` と `mk_blank_nhd.py` (S3I2-T)
- `## K.` — `fatfs_vfs_list()` の列挙エラー伝播 (S3I2-K)

---

## I. `install` (無印) の通常インストール経路 (S3I2-I) — 2026-09-14

### I.1 何を直したか、なぜ要るか

FDD イメージは `/VMKRNL.LZ4` + ローダ v3 (`/sys/loader_h.bin`、ext2 の
`/boot/vmkernel.lz4` を読む) を収録するのに、`install` は `/kernel.bin` を必須にして
LBA 6 へ生書きしていた。そのため FDD からの新規インストールは Phase 1 の
`Missing /kernel.bin` で止まり、`/etc` のコピー (settings.db の seed) まで到達しない。
あわせて票 §1 の既存欠陥 4 つ (IdeInfo の型、承認前の検査が無い、宛先が大文字のまま、
失敗が伝播しない) を直した。

| 直した点 | 中身 |
|---|---|
| IDE 情報の型 | `IdeInfoTemp` (92 B) → `IdeInfo` (96 B、末尾 `phys_sector_size`)。`ide_identify` はカーネル側の 96 B で書くので、92 B のままだと呼び手の領域を 2 B 踏む |
| Phase 0 (承認前) | `/VMKRNL.LZ4` `/sys/boot_hdd.bin` `/sys/loader_h.bin` の存在と大きさ (> 0) を `sys_stat` で検査。欠ければ **1 バイトも書かずに** 終了 1 |
| Phase 1 | `/kernel.bin` の読込と LBA 6 への生書きを削除 (IPL / PT / ローダは現行どおり) |
| Phase 3 | `mkdir /hd0/boot` → `/VMKRNL.LZ4` → `/hd0/boot/vmkernel.lz4` をストリームコピー (128KB バッファ) → 長さ一致を確認 |
| 宛先の小文字化 | FAT (`FF_USE_LFN 0`) の `sys_ls` は `SHELL.BIN` を返し ext2 は大小文字を区別するので、宛先パスの各成分を ASCII 小文字に正規化。ソースは列挙で得た名前のまま開く |
| `/etc/profile` | 写さない (FDD 用の PATH に `/usr/bin` が無い)。`/etc/settings.db` は写す (seed) |
| 失敗の伝播 | `copy_file` は read の負を EOF と区別して `-4`、short write は `-3`。`copy_directory` は `sys_ls` の負・mkdir・再帰・列挙の取りこぼしを**失敗件数**として返す。`main` は IPL / PT / ローダ / format / mount / `/boot` コピー / ディレクトリコピー / `vfs_sync` のどれか 1 つでも失敗したら `[FAIL]` + 終了 1 |
| 表示 | バナー `v4.1`、`[1/3]` `[2/3]` `[3/3]` 維持、`Written KERNEL (LBA 6)` → `vmkernel.lz4 -> /boot (<n> bytes)`、成功時だけ `Installation complete` |

回復モード (`--recover-settings` / `--revert-settings`、`install_recover.inc`) は
**1 行も触っていない**。

### I.2 試験の作り

`tools/tests/install_fresh_host.c` + `tools/tests/test_install_fresh.py`。

実物は `userland/system/install.c` の通常経路そのもの (`#define main install_main`
で取り込む)。模型は KAPI の贋物だけ:

- `ide_identify` は **96 B** を書く (`drivers/ide.h` の並びの写し `IdeIdentifyWire`)。
  書く前に `sizeof(IdeInfo) == sizeof(IdeIdentifyWire)` と全 offset の一致を見るので、
  install.c 側の型が小さければその場で落ちる。
- `sys_ls` は **FAT のように大文字** の名前を返す。名前引きは `/hd0` 配下 (ext2) を
  大小文字で区別し、媒体側 (FAT) は区別しない。
- `ide_write_sectors` は (LBA, 本数) を記録するだけ。
- 注入: read の負 (何回目からでも) / short write / mkdir / `sys_ls` の負 / `ext2_format` /
  `sys_mount` / `vfs_sync` / `ide_write_sectors` の失敗。
- `kprintf` は捕捉、`getkey` は台本 (既定 `y` = 承認)。

ホストのファイルシステム・配備・エミュレータには 1 バイトも触らない。

**ホストは LP64 で `u32` = `unsigned long` = 8 B** なので「96 B」という数そのものは
ホストでは再現できない。ホスト試験は **実型との一致** (大きさと全 offset) を見て、
`sizeof(IdeInfo) == 96` / `offsetof(..., phys_sector_size) == 92` は `--target` の
i386-elf コンパイル時表明 (`ide_layout_assert.c` を生成して `-Werror` で通す) で固定する。

```
python3 -B tools/tests/test_install_fresh.py --target
python3 -B tools/tests/test_install_fresh.py --sanitize
python3 -B tools/tests/test_install_recover.py --target   # (8) 回復モードの回帰
```

### I.3 RED → GREEN

**RED-1** (install.c 未修整、`IdeInfo` がまだ `IdeInfoTemp`) — ホストのビルドが通らない。
これが票 (7) の RED:

```
install_fresh_host.c:254: error: 'IdeInfo' undeclared (first use in this function)
install_fresh_host.c:255: error: expected specifier-qualifier-list before 'IdeInfo'
...
（`-DIdeInfo=IdeInfoTemp` で旧型に当てると）
install_fresh_host.c:255: error: 'IdeInfoTemp' has no member named 'phys_sector_size'
```

**RED-2** (型だけ実型に直し、他は未修整) — 残り 8 件が各自の理由で落ちる:

```
HOST GNU89 -Werror compile PASS (real install.c normal path)
FAIL case_nokernel:490: !rec_has(rec_open, rec_open_n, "/kernel.bin")
FAIL case_vmkernel:513: rec_has(rec_mkdir, rec_mkdir_n, "/hd0/boot")
FAIL case_lower:528:    fx_exists("/hd0/sys/shell.bin")
FAIL case_precheck:549: run() == 1
PASS decline                       ← 承認しない経路は元から 0 (不変であることの確認)
FAIL case_boot_fail:598: run() == 1
FAIL case_copy_fail:619: run() == 1
FAIL case_sync_fail:669: run() == 1
PASS idetype                       ← 型だけ先に直した分
SUMMARY 2/9 PASS
```

**GREEN** (票 §1 をすべて反映):

```
HOST GNU89 -Werror compile PASS (real install.c normal path)
TARGET i386-elf GNU89 compile PASS (install.c)
TARGET IdeInfo layout PASS (sizeof == 96, phys_sector_size @ 92)
PASS nokernel / vmkernel / lower / precheck / decline / boot_fail / copy_fail / sync_fail / idetype
SUMMARY 9/9 PASS
```

`--sanitize` (ASan) でも 9/9 PASS。回復モードの回帰
`python3 -B tools/tests/test_install_recover.py --target` は **14/14 PASS** (分岐は不変)。

### I.4 票 §1 の (1)〜(8) と試験の対応

| 票 | 試験 | 何を固定したか |
|---|---|---|
| (1) `/kernel.bin` を読まない | `nokernel` | `sys_open` / `sys_stat` の記録に `/kernel.bin` が無い、出力に `kernel.bin` / `Written KERNEL` が出ない |
| (2) 欠損で LBA 0/1/2 に書かない | `precheck` | lz4 欠損 / lz4 が 0 バイト / `boot_hdd.bin` 欠損 / `loader_h.bin` 欠損の 4 通りで `ide_write_sectors` が 0 回、終了 1、`ERASED` の確認すら出ない |
| (3) 長さ一致 | `vmkernel` | `/hd0/boot/vmkernel.lz4` が 470000 B で中身も一致 (128KB × 4 塊)、`vmkernel.lz4 -> /boot (470000 bytes)` |
| (4) 宛先が小文字 | `lower` | `/hd0/etc/settings.db` `/hd0/sys/shell.bin` `/hd0/bin/ls.bin` `/hd0/sbin/init.bin` が在り、大文字版が無い。再帰先も `/hd0/etc/rc.d/boot.sh` |
| (5) profile が写らない | `lower` | `/hd0/etc/profile` も `/hd0/etc/PROFILE` も無い |
| (6) 各失敗で終了 1 | `boot_fail` `copy_fail` `sync_fail` | IPL / PT / ローダの ide_write、`ext2_format`、`sys_mount`、read の負 (先頭 / 途中)、short write (通常ファイル / lz4 の長さ不一致)、mkdir (`/hd0/boot` / 再帰先)、`sys_ls` の負、`vfs_sync` — すべて終了 1 + `[FAIL]` + `Installation complete` が出ない |
| (6b) 生カーネルの書込みが無い | `nokernel` | 生の書込みは IPL(0,1) / PT(1,1) / ローダ(2,n) の **3 回だけ**。LBA 6 はローダの 2〜17 に含まれるので「LBA 6 に書かない」とはしない |
| (7) `IdeInfo` 96 B | `idetype` + `--target` | ホストは実型との一致 (大きさ・全 offset)、i386-elf は `sizeof == 96` / 末尾 offset 92 をコンパイル時表明 |
| (8) 回復モードが不変 | `test_install_recover.py` | 14/14 PASS (`--target` 込み) |

（`decline` = 承認しないとき何も書かず **終了 0** は票に明示が無いので、回復モードの
`case_approve` (承認しない = 0) に合わせた。利用者が断っただけで `[FAIL]` は出さない。）

### I.5 Codex 実装レビュー 往復 1 の反映 (2026-09-14)

**B1 (blocker)**: 初期ディレクトリ作成 (`/hd0/sys` … `/hd0/tmp`) の `sys_mkdir` の
戻り値を捨てていたので、`/hd0/tmp` だけ作れなくてもコピーと sync が通れば
`Installation complete` + 終了 0 になっていた。

RED (反例をホストで踏んだ):

```
FAIL case_mkdir_init: mkdir /hd0/sys failed but install returned 0
EXIT mkdir_init=1
SUMMARY 10/12 PASS
```

直し方: 作る 12 個を `init_dirs[]` (親が先) にまとめ、1 つでも失敗したら
`Error: Failed to create <path>` + 終了 1。`/hd0/boot` の個別検査はこの表に統合した。
`mkdir_init` は 12 個それぞれを個別に失敗させて 12 回とも終了 1 を確かめ、
正常系では 12 個すべてが作られることも見る。

non-blocker (3 件とも試験を追加、実装は変更なしで GREEN):

| 追加 | 中身 |
|---|---|
| 列挙途中の失敗 | `sys_ls` の贋物に「**何件か callback を呼んでから** 負を返す」形 (`inj_ls_fail_after`) を足し、頭で負 / 2 件渡してから負 / 全件渡してから負 の 3 通りで終了 1。FAT 単体ではなく `install` との接続を見る |
| 64 / 65 件、深さ 4 / 5 | `bounds`: `/bin` にちょうど 64 件 → 全部写って 0、65 件 → 取りこぼすので 1。`/etc` に 4 段ネスト → `d1/d2/d3/d4/deep.txt` まで写って 0、5 段 → 1 |
| 正常 EOF の長さ不一致 / 綴り保持 | `srcname`: `stat` だけが 4096 B 大きい値を名乗る (read は正常に EOF) → 長さ一致の検査だけで終了 1。開いた名前が媒体の綴りのまま (`/VMKRNL.LZ4` `/sys/SHELL.BIN` `/etc/SETTINGS.DB`) で小文字版は開いておらず、宛先だけが `/hd0/sys/shell.bin` |

往復 1 後: **12/12 PASS** (`--target` の表明 2 件込み、`--sanitize` でも 12/12)、
回復モードの回帰は **14/14 PASS** で不変。

### I.6 まだ見ていないこと ([V4])

- 実機 (NP21/W) での FDD ブート → `install` → HDD ブートは **未実施**。票 §3 の F1〜F6 は
  PM / テスターの受入。コーダーはホスト TDD と単体コンパイルまで。
- `make all` / `make check` / 配備は実行していない (コーダーの範囲外)。
- `sys_ls` の負が実際に FAT から返るのは S3I2-K (`fatfs_vfs_list`) の修正後。
  ここでは贋物で負を注入して `install` 側の受け口だけを固定した。

---

## §T — ini 道具の `ALLOWED_PATHS` と `mk_blank_nhd.py` (2026-09-14)

対象は `tools/np21w_ini.py`、`tools/np21w_trial.py`、`tools/mk_blank_nhd.py` (新規) と
その試験のみ。`tools/np21w_ini_live.py` は**変更していない** (作業 NHD を差し替える
事故を作らないため、票 §2a)。実 ini・実プロセス (NP21/W)・実 `NP21W_DIR`・
エミュレータ・ネットワーク・`.env` には一切触れていない ([D3]、スキル `os32-emu-config` §0:
ini の変更権限は PM だけ。ここで作ったのは道具と試験だけで、適用の承認は別)。

### 変更点

| 何 | 内容 |
|---|---|
| `np21w_ini.py` | 固定値の `ALLOWED` と別に `ALLOWED_PATHS = {'HDD1FILE': '.nhd', 'FDD1FILE': '', 'FDD2FILE': ''}`。`HDD1FILE` は `NP21W_DIR` 直下の `[A-Za-z0-9_.-]+\.nhd` の**名前**で受け、`wslpath -w` で得た Windows 表記 + `\<name>` へ展開して書く (ホスト側の存在も確認)。`FDD1/2FILE` は**変更後の値は空だけ** = 装着解除で、変更前の値は非空の CP932 パスでも検査せず差し替える。`transform()` の「変更キーがあれば全キーの存在を要求」は 2 表**別々**に適用する |
| `np21w_trial.py` | 計画に `hdd` (名前) / `fdd_eject` (bool) / `fdd_arg` (`.d88` の絶対パス、任意) を追加。変更集合は `HDD1FILE` (+ `FDD1/2FILE`) と `e_resume=false` **だけ**で、Cirrus 系キーには触れない (`action` も `disk-trial` に改名)。起動は `exe + "/i<trial ini>" [+ "<d88>"]` で、PowerShell 側と Python 側が同じ文字列を照合する |
| `mk_blank_nhd.py` | `--out` / `--size-mb` (既定 200) / `--force`。H=8 S=17 セクタ長 512 固定、`C = floor(size_mb*1MiB / (8*17*512))`、受付は 1 ≤ C ≤ 65535、512B ヘッダ + 本体 C×8×17×512 B の全ゼロ。表示は `C=<n> capacity=<bytes>` |

`NP21W_DIR` は**環境変数だけ**から読む (`.env` は読まない [D3])。WSL 専用ディレクトリは
`wslpath -w` が UNC (`\\wsl.localhost\...`) を返すので drive 絶対でないとして拒否される
(実 `NP21W_DIR` は `/mnt/c/...` = `C:\...`)。

### ソース根拠 (np21w-src、読み取りのみ)

| 根拠 | 意味 |
|---|---|
| `src/fdd/sxsihdd.h` NHDHDR | `sig[16] + comment[0x100] + headersize[4] + cylinders[4] + surfaces[2] + sectors[2] + sectorsize[2] + reserved[0xe2]` = 512B。`mk_blank_nhd.header()` のバイト位置はこれ |
| `src/fdd/sxsihdd.c:13` | `sig_nhd[15] = "T98HDDIMAGE.R0"`。open 時に 15B を `memcmp` する |
| `src/fdd/sxsihdd.c` (open) | `totals = C * S * H`。実ファイル長をこれに一致させる必要がある |
| `src/fdd/sxsihdd.c` (書式確認) | `(cylinders == 0) || (cylinders >= 65536)` などで拒否 → C の受付範囲 1..65535 |
| `src/fdd/newdisk.c` `newdisk_nhd_ex_CHS` | 同じヘッダの生成手順 (headersize = sizeof(nhd) = 512、C/H/S/SS をリトルエンディアン) |
| `src/win9x/np2arg.cpp` `Np2Arg::Parse` | `/i<ini>` を設定ファイル指定として読み、拡張子で判る `.d88` はディスクとして装着する → 起動コマンドの形 |
| `src/x11/ini.c:538` | `{"HDD1FILE", INITYPE_STR, np2cfg.sasihdd[0], MAX_PATH}` = IDE 第 1 スロットのパス文字列 |

### 実際の RED → GREEN

```bash
python3 -B -m unittest discover -s tools/tests -p 'test_np21w_ini.py'    # 2a
python3 -B -m unittest discover -s tools/tests -p 'test_np21w_trial.py'  # 2a (trial)
python3 -B tools/tests/test_mk_blank_nhd.py                              # 2b
```

| 段階 | 実出力 | 内容 |
|---|---|---|
| RED (2a ini) | `Ran 40 tests` / `FAILED (errors=11)`、`AttributeError: module 'np21w_ini' has no attribute 'subprocess'` | 旧 `np21w_ini.py` に戻して新試験 `PathFields` を実行。パス表も wslpath 解決も無い |
| RED (2a trial) | `Ran 19 tests` / `FAILED (errors=18, skipped=1)` | 旧 `np21w_ini.py` + 旧 `np21w_trial.py`。`make_plan()` に `hdd` / `fdd_eject` / `fdd_arg` が無い |
| RED (2a trial 第 2 段) | `Ran 19 tests` / `FAILED (failures=1, errors=31, skipped=1)` (errors は subTest 単位) | 新 `np21w_ini.py` + 旧 `np21w_trial.py`。`$arguments = '"/i' + $plan.trial + '"'` が PS 本文に無い |
| RED (2b) | `Ran 13 tests` / `FAILED (failures=1, skipped=12)`、`mk_blank_nhd.py must implement the tested contract` | `tools/mk_blank_nhd.py` 未作成 |
| GREEN (2a) | `Ran 114 tests` / `OK (skipped=2)` (`-p 'test_np21w_*.py'`) | ini 87 (新規 11) + trial 19 (新規 2) + transport 8。skip は live の Windows fixture と trial の Windows parser |
| GREEN (2b) | `Ran 13 tests` / `OK` | ヘッダのバイト列、C の計算 (1 / 100 / 200 / 4351 と範囲外)、本体長、`--force`、symlink 拒否、`nhd_deploy.py` の定数一致 |

その他の実施結果:

```bash
python3 -B tools/mk_blank_nhd.py --out <scratch>/demo.nhd --size-mb 200
# C=3011 capacity=209661952 / ファイル長 209662464 = 512 + 3011*8*17*512、2 回目は exit 2
python3 -B tools/np21w_trial.py --help            # exit 0 (--hdd / --fdd-eject / --fdd-arg)
py_compile.compile(..., doraise=True)             # py_compile: OK (7 files)
python3 -B -m unittest discover -s tools/tests -p 'test_*.py'
# Ran 435 tests / errors=1 — 既存の test_paging_bounds.py が import 時に argparse を
# 走らせるための失敗で、この票の変更とは無関係 (check-tools-host は当該ファイルを
# discovery しない)
```

### 未実施 ([V4])

- **実 ini・実プロセス・実 NP21/W では一切検証していない。** `/i<ini>` 起動、`HDD1FILE` を
  書いた ini での HDD ブート、FDD 装着解除の効き、生成した NHD のゲストからの見え方は
  すべて PM / テスターの受入 (§3 F1〜F6) で確認する。適用は [D2] の個別承認。
- `PowerShellParser` の構文検査 (`--windows-parser`) は opt-in のままで未実行。
- `mk_blank_nhd.py` は本体を `ftruncate` で伸ばす (疎ファイル)。NTFS / drvfs 上での
  実際の書き込みは未確認。

### PM が登録する行

`build/sdk.mk` の `check-tools-host` は `test_np21w_*.py` を discovery するので 2a は自動で
入る。2b は名前が合わないので 1 行足す:

```make
	python3 -B tools/tests/test_mk_blank_nhd.py
```

`.claude/skills/os32-emu-config/SKILL.md` の「承認済み Cirrus trial」節は、trial が扱うキーが
Cirrus 2 キー → `HDD1FILE` / `FDD1/2FILE` (+ `e_resume`) に変わり、起動引数も
`/i<ini>` + 任意の `.d88` になったので PM の更新対象 (ini の文書は PM の担当)。

## §T 往復 1 — Codex 実装レビューの blocker 4 件 (2026-09-14)

対象は `tools/np21w_ini.py` / `tools/np21w_trial.py` とその試験だけ。`np21w_ini_live.py`、
`mk_blank_nhd.py` は変更なし。実 ini・実プロセス・実 `NP21W_DIR` には触れていない。

| # | 指摘 | 直し |
|---|---|---|
| B2 | 承認計画に HDD の解決済みパスが残らず、`NP21W_DIR` を A→B に変えて同じ JSON を dispatch すると B の同名ファイルで起動する | 計画に **`hdd_path` (Windows) / `hdd_host` (ホスト側)** と `fdd_arg` / **`fdd_arg_host`** を束縛し、`changes['HDD1FILE']` も解決済みパスにした。環境を読むのは `make_plan()` の `resolve_image()` だけで、`_validate_plan()` と `transform()` は**環境を一切見ない** (`_plan()` を両者で共有し、束縛済みの 3 つ組が同じ 1 つの通常ファイルを指すかだけ検査する) |
| B3 | `wslpath -w` の出力を `.strip()` していたので `NP21W_DIR` の末尾空白が消え、存在確認先と ini の書き先がずれる | `_wslpath()` は**終端の改行だけ**を除去 (空白は保持) し、`windows_path()` が末尾空白の成分を拒否。さらに `wslpath -u` で**往復させ**、同じディレクトリに戻らなければ拒否 |
| B4 | 新しいパス値に `;` / `#` を許していたので、自分が書いた ini を同じ変更で読み直せない | `windows_path()` の文字集合から `;` `#` を除外 (入口と出口の両方で fail closed)。「1 回変換した出力を同じ変更でもう一度変換しても不変」を試験で固定 |
| B5 | `os.path.exists()` だけなので `.nhd` / `.d88` という名前の**ディレクトリ**を受理する | `resolve_image()` は `os.path.isfile()` かつ symlink でないことを要求。PowerShell 側にも `CheckFile`(= `CheckPath` + `PSIsContainer` 拒否) を足し、`preflight` と `start` で `$plan.hdd_path` と `$plan.fdd_arg` を検査 |

副作用として `transform()` の `HDD1FILE` は**名前ではなく解決済みの絶対パス**を受け取るようになった
(`ini.resolve_image(name, ext) -> (host, windows)` が唯一の展開点)。`np21w_ini.py` の CLI
`--set HDD1FILE=` は名前でも絶対パスでも受け、名前のときだけ解決する。

### 反例 → 直し (ホストで実際に踏んだ)

`/tmp/.../scratchpad/counterexamples.py` (temp dir + 贋 `wslpath`、実 ini / 実プロセス無し):

| | 着地版 (`cd1e136`) | 直し後 |
|---|---|---|
| B2 | `plan keys: ['fdd_arg', 'fdd_eject', 'hdd']` / 書かれた値 `HDD1FILE=C:\B Dir\os32_fresh.nhd` (A で承認 → B で dispatch) | `plan keys: [... 'hdd_host', 'hdd_path']` / `HDD1FILE=C:\A Dir\os32_fresh.nhd` |
| B3 | `resolved: 'C:\Trial\os32_fresh.nhd'` (`C:\Trial ` の空白が消えた) | `IniError: unsupported absolute Windows path` |
| B4 | 1 回目 `HDD1FILE=C:\Trial#1\os32_fresh.nhd` を出力 → 2 回目 `REFUSED -> unsupported separator in path field` | `IniError: unsupported absolute Windows path` (出力自体を作らない) |
| B5 | ディレクトリ `os32_fresh.nhd` を受理し `C:\Trial\os32_fresh.nhd` を返す | `IniError: image is not a regular file under NP21W_DIR` |

| 段階 | 実出力 | 内容 |
|---|---|---|
| RED | `test_np21w_ini.py`: `Ran 47 tests` / `FAILED (failures=2, errors=41)`、`test_np21w_trial.py`: `Ran 21 tests` / `FAILED (failures=2, errors=3, skipped=1)` | 着地版の道具に新試験を当てた (`resolve_image` 無し、計画に束縛パス無し、`CheckFile` 無し) |
| GREEN | `python3 -B -m unittest discover -s tools/tests -p 'test_np21w*.py'` → `Ran 123 tests` / `OK (skipped=2)` | ini 47 (+11 → 新規 4: 束縛 / 往復 / 空白 / 通常ファイル)、trial 21 (+2)、transport 8。`test_mk_blank_nhd.py` は `Ran 13` / `OK` のまま |

`python3 -B tools/np21w_trial.py --help` は exit 0。贋 `wslpath` (`-w` / `-u`) を PATH に置いた
dry-run では計画に `hdd_host` / `hdd_path` / `fdd_arg_host` が載り exit 0、往復しない贋物では
`stage: approval` で exit 2 (fail closed) を確認した。

### 未実施 ([V4])

実 ini・実プロセス・実 NP21/W では相変わらず未検証。PowerShell の `CheckFile` (`PSIsContainer`)、
`/i<ini>` 起動、HDD ブートはすべて受入 (§3 F1〜F6) と `--windows-parser` での確認が要る。
