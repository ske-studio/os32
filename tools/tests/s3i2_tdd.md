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

### I.5 まだ見ていないこと ([V4])

- 実機 (NP21/W) での FDD ブート → `install` → HDD ブートは **未実施**。票 §3 の F1〜F6 は
  PM / テスターの受入。コーダーはホスト TDD と単体コンパイルまで。
- `make all` / `make check` / 配備は実行していない (コーダーの範囲外)。
- `sys_ls` の負が実際に FAT から返るのは S3I2-K (`fatfs_vfs_list`) の修正後。
  ここでは贋物で負を注入して `install` 側の受け口だけを固定した。
