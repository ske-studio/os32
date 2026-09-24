# HDD インストーラ (段 2) — RED→GREEN の記録

- 票: [docs/tasks/realhw/TASK_HDD_INSTALL.md](../../docs/tasks/realhw/TASK_HDD_INSTALL.md) 段 2 (9〜11) / §1-v3 (N4・N6・N8・R3-1)
- 実行: `python3 -B tools/tests/test_hdd_stage2.py --target --mutate` (`make check-hdd-stage2-host`)
- ハーネス: `tools/tests/hdd_stage2_host.c` (純粋関数、ホスト 64 ビット + libc)、
  `tools/tests/cdinst_host.c` (実物の cdinst.c を main から、ILP32 -nostdlib)、
  `tools/tests/install_fresh_host.c` (実物の install.c を main から、`test_install_fresh.py` と共有)、
  `tools/tests/ext2_mini_host.c` (ローダの ext2 読み手、ILP32 -nostdlib)

## 1. 対象と見るもの

| 対象 | ケース | 見るもの |
|---|---|---|
| `userland/system/inst_disk.c` | `classify817` / `classify1663` | 空・空で 55AA・再作成 (標準配置、項目が表のどこでも、名前の後ろが NUL でも)・開始が 1 シリンダ前後・名前違い (OS32X / os32)・sid 違い・2 項目 (mid だけの項目も数える)・ディスクの外・壊れ、旧配置 8/17 (シリンダ 12 = LBA 1632) は再作成、旧配置 16/63 と 8/17 の標準配置の表を 16/63 で読むと開始 12,096 → 断る |
| 同 | `bootfiles` / `blocks` / `space` | IPL 1〜512・ローダ 1〜8192・vmkernel 1〜508KiB の境界、間接ブロック (13 ブロック目・二重間接の境目)、ブロック / inode の不足と「ちょうど収まる」、配置が成り立たない大きさ |
| 同 + `hdprep_plan.c` | `iplpt` | 8/17 (1632, 407,864)・16/63 (2016, 524,160)・8/17 で 256MiB を 136 で切り下げ (524,280) の計画から作った区画表を共有部で読み戻すと同じ範囲、残りの項目は空、IPL の [8]/[9] = 区画表の幾何 |
| 容量の見積もり | room | 16,652 / 20,160 / 36,000 / 62,496 セクタの空き (ブロック・inode) が **実物の `ext2_format_at` の像の `dumpe2fs`** と一致 (変異のたびにも見る) |
| 定数 | consts | `INST_KERNEL_MAX` = `boot/boot_defs.h` MAX_IMAGE_SIZE、`INST_LOADER_MAX` = `nhd_deploy.py` LOADER_MAX_SECTORS × 512、`INST_EXT2_MAX_GROUPS` = `fs/ext2_ctx.h`、IPL の [8]/[9]/[10] に `boot_hdd.bin` の既定 8 / 17 / 80h |
| `userland/system/cdinst.c` | `ok817` / `ok1663` / `modes` / `preflight` / `incomplete` | 8/17 (ドライブは 16/63 を申告) と 16/63 で区画表・IPL の幾何・IPL とローダの中身 (BOOT.PKG の中身、端数は 0)・format の範囲・順序 `[U] F W1 M W2.. W0`、旧配置 8/17 の再作成 (マウント中は外してから)・未知・2 項目、事前検査 (ローダ 8193・IPL 513・IPL 無し・LZSS の BOOT.PKG・vmkernel 508KiB ちょうどは通り +1 は断る・vmkernel 無し・shell 無し・NORMAL が 300MB を名乗る (Minimal なら数えない)・前置で溢れる項目・BIOS 幾何なし・承認しない) で 1 セクタも書かない、format / 区画表の読み戻し / マウント / IPL / NORMAL の展開 / sync の失敗は INCOMPLETE で「完了」と出さない |
| `userland/system/install.c` | `geom817` / `geom1663` / `modes` / `preflight` / `incomplete` / `rerun` (+ 既存 12 本) | cdinst と同じ規則を FD 側で。加えてマウントの 4 通り (ルートの hd0・別の場所・umount_checked の失敗・外しても残る)、FD の列挙の失敗は書く前に分かる、途中で止まった hd0 (format 失敗 → 空 / マウント失敗 → 再作成) と完了した hd0 を入れ直せる |
| `boot/ext2_mini.c` | mini | 1,000 B と 508KiB ちょうどは読んで後ろに書かない、508KiB + 1 と 600KiB は `EXT2M_ERR_TOO_BIG` で**読み先に 1 バイトも書かない** |

## 2. RED

- `inst_disk.c` / `inst_hdd.c` と cdinst / install の新しい手順はこの票で作った。**実装と試験は同じ作業の
  中で書いた (試験先行ではない)**。旧動作 (IPL のヘッド 8 固定・IDENTIFY の幾何を IPL に書く・ディスク全体の
  format・区画表を読み戻さない・ext2_mini の切り詰め) は §3 の変異で再現して RED を確かめた。
- `boot/ext2_mini.c` の上限は段 0 で直っていた。この段で初めて試験を付けた (変異 2 本が RED)。

## 3. 変異 (`--mutate`)

41 本を写しの上で 1 本ずつ当て、4 本のハーネスを全部組み直して回す。**ビルドが通って試験が落ちた**ものだけを
RED、ビルドが通らないものは ERROR (何も確かめていない) と数える。変異させる 5 ファイルそれぞれに恒等変異
(末尾に注釈 1 行) の対照を当て、SURVIVED になることを確かめる。

結果 (2026-09-24): **41/41 RED (ERROR 0、SURVIVED 0)、対照 5/5 SURVIVED**。

- 最初の回は ERROR 7 (変数・関数が未使用になり `-Werror` で落ちる形) と SURVIVED 2 だった。ERROR は
  値を使ったまま判定だけ外す形に書き直した。SURVIVED の 1 本 (空きの見積もりがルートの 1 ブロックを数えない) は
  dumpe2fs との突き合わせを変異のたびにも回すようにして RED にした。もう 1 本 (`inst_hdd_check` の
  `hdprep_check_geom` を外す) は `hdprep_plan` が同じ検査を中でもう一度するので**等価変異**。外して、
  「検査でマウントを数えない」に置き換えた。

## 4. 実装レビュー往復 1 (2026-09-24、Codex P1-1〜3・P2-4〜6 / Fable minor) と NP21/W の落ち

足したケース:

- `cdinst_host.c` `preflight`: データ部が 10 バイト足りない NORMAL.PKG / 1 バイト足りない BOOT.PKG /
  orig_size = 0 の MINIMAL.PKG と BOOT.PKG / 空の shell.bin / 1 ファイルの上限 + 1 (67,383,297 B) /
  13MB の区画に 30MB (Minimal なら数えない) / ルートが hd0 / umount_checked の失敗 / 別の prefix にも
  マウント — どれも 1 セクタも書かない。`paths`: `/../hd1/evil.bin`・`/usr/../../hd1/x`・`/usr/./x`・
  `//x`・`/x/`・`x/y`・`/.`・`/..` と MINIMAL の中の `..` は断り、`/hd0` の外に 1 つも作らない。
  深さ 31 要素 (`/hd0` と合わせて 32) は入り、32 要素は断る。`incomplete`: REBOOT の案内。
- `install_fresh_host.c`: `/sys/shell.bin` が無い・空・ディレクトリ、ローダがディレクトリ、hdd_geom_info の
  失敗、別の prefix / 2 か所のマウント、INCOMPLETE は 1 回だけ、区画表が媒体の上で壊れた場合
  (INCOMPLETE + ホスト側の手当て → 次の実行は 1 セクタも書かずに断り、同じ案内) を足した。
- `hdd_stage2_host.c` `paths` / `space`: パスの規則の表、inode の余白の境界、失敗でも room が 0 で埋まる、
  上限ちょうど / +1。
- 実物の `packages/*.PKG` の全 209 項目のパスが `inst_check_path` を通る (real PKG paths)。
- `boot_hdd.asm` の `mov cx, 16` × 512 = `INST_LOADER_MAX` (consts)。
- **str-return guard**: インストーラ (CPL=3) が呼ぶ `const char *` の KAPI は、どれも CPL=3 に写しを返す実体に
  つながる (`vfs_devname` → `vfs_devname_user`)。628c61f は NP21/W で cdinst が `vfs_devname("/")` の返り値
  (カーネルのマウント表) を読んで fault kill された。ホスト試験の贋物は利用者の文字列を返し、ページの保護も
  無いので見えなかった — この種類はホストのハーネスでは再現できないので、kapi.json の target と生成物を見る。

変異は 21 本を足して **62/62 RED (ERROR 0、SURVIVED 0)、対照 5/5 SURVIVED**。足した直後は「BOOT.PKG の
データ部を見ない」が SURVIVED だった (切れた BOOT.PKG は comp_size 分の読みで別に断れるので同じ結果になる)。
orig_size が表と食い違う BOOT.PKG のケースを足して RED にした。展開の直前のパスの検査 (事前検査と同じ規則の
2 回目) だけを外す変異は、事前検査が先に断るので区別できない — 変異の表には入れていない。
