# vfs mount デバイス種別 — TDD 記録 (2026-09-10)

対象: `fs/vfs.h` / `fs/vfs.c` / `fs/ext2_vfs.c` / `fs/iso9660.c` / `fs/fatfs_vfs.c`
試験: `tools/tests/test_vfs_mount_dev.py` + `tools/tests/vfs_mount_dev_host.c`
登録: `make check` → `check-vfs-mount-dev-host`

## 症状

きれいな ext2 に 106KB のファイルを 1 個 `cp` して `sync` するだけで、
ホスト側 `e2fsck -fn` が必ずこう言う:

```
Free blocks count wrong for group #0 (4097, counted=3991).
Free inodes count wrong for group #0 (1761, counted=1760).
```

ずれはちょうどファイル 1 個分 (106 ブロック / 1 inode)。強制終了とは無関係で、
`CloseMainWindow` による通常終了でも同じ。ビットマップ側は常に正しい。

## 切り分け (実機 NP21/W)

1. `mounts[0].fs_ctx = 0x00156008` の `free_blocks_count` (ctx+52) を実測。
   cp 前 91386 → cp 後 91280 (−106)、inode 50496 → 50495 (−1)。**メモリ上は正しい**。
2. `sync` 後にホスト側イメージの `836096+1024+12` を直読。91386 / 50496 のまま。
   **ディスク上のスーパーブロックだけが巻き戻っている**。
3. `ext2_write_super_raw` にブレークポイントを置いたら 0 ヒット。
   → これは誤読で、`ext2_write_super_raw` は `ext2_sync` に**インライン展開**
   されていた (`i386-elf-objdump -d ext2_sync` で確認)。実際には走っている。
4. `vfs_sync` から順に止めると `ext2_vfs_sync` が **2 回**呼ばれていた。
   VFS のマウント表を実機メモリから読むと:

```
mounts[0] prefix=/     ctx=0x00156008 dev=hd0(t0,i0) base_lba=1632 dev_ptr=0x0012C800
mounts[1] prefix=/fd0  ctx=0x001562E0 dev=fd0(t1,i0) base_lba=1632 dev_ptr=0x0012C800
```

`/fd0` が **hd0 の同じパーティションを二重マウント**していた。

## 原因

`vfs_mount()` は `ops->mount((dev_type << 8) | dev_id)` と種別を上位バイトに
載せて渡すが、`fs/vfs.c` のコメントは「既存FSドライバ (ext2等) は
`dev_id & 0xFF` のみ参照するので互換」と書いていた。この前提が誤り。

`fd0` は `dev_type=VFS_DEV_FD(1), dev_id=0` なので `0x100` が渡り、ext2 側では

- `ide_drive_present(0x100)` → `drive_present[0x100 & 3]` = `drive_present[0]` (hd0)
- `ext2_dev_for(0x100)` → `'h','d','0'+(0x100 & 0xFF)` = `"hd0"`
- `ext2_find_partition(0x100)` → `ide_get_info(0x100)` → `drive_info[0]` → base_lba 1632

と**すべて種別が消えて hd0 に化ける**。ブート時の自動マウントループ
(`kernel/kernel.c`) が root 以外の全ブロックデバイスに ext2 を試すので、
`/fd0` として 2 つ目の `Ext2Ctx` が必ずできる。

`vfs_sync()` は全マウントを順に回すため:

1. `mounts[0]` が正しい空き数をスーパーブロック (LBA 1634/1635) に書く
2. `mounts[1]` が**マウント時点のスナップショット**を同じセクタに書き戻す

ビットマップはこの経路では触らないので真値が残り、e2fsck の
「counted=」不一致になる。`iso9660_mount()` も `'0' + (char)dev_id` で
同じ取りこぼしをしていた (読み取り専用なので破壊はしないが同じ欠陥)。

## RED (修正前)

```
COMPILE GNU89 -Werror PASS
EXIT encode=0
FAIL ext2_rejects_non_hd:122: ext2_vfs_mount(VFS_MOUNT_DEV_ENCODE(VFS_DEV_FD, 0)) == (void *)0
EXIT ext2_rejects_non_hd=1
FAIL boot_sequence_has_one_ext2:145: vfs_mount("/fd0", "fd0", "ext2") != VFS_OK
EXIT boot_sequence_has_one_ext2=1
FAIL duplicate_device_refused:160: vfs_mount("/again", "hd0", "ext2") == VFS_ERR_EXIST
EXIT duplicate_device_refused=1
SUMMARY 1/4 PASS
```

## GREEN (修正後)

```
COMPILE GNU89 -Werror PASS
EXIT encode=0
EXIT ext2_rejects_non_hd=0
EXIT boot_sequence_has_one_ext2=0
EXIT duplicate_device_refused=0
SUMMARY 4/4 PASS
```

## 修正

- `fs/vfs.h` — `VFS_DEV_*` と `VFS_MOUNT_DEV_ENCODE/TYPE/ID` を公開 (FS ドライバも見る)。
  `fs/vfs.c` の private な定義は撤去。
- `fs/ext2_vfs.c` — `VFS_MOUNT_DEV_TYPE != VFS_DEV_HD` を ext2 本体へ届く**前**に拒否。
- `fs/iso9660.c` — 同じく `VFS_DEV_CD` 以外を拒否。
- `fs/fatfs_vfs.c` — 手書きのシフトを共通マクロに置換 (挙動は不変)。
- `fs/vfs.c` — 同じ (ops, dev_type, dev_id) の二重マウントを `VFS_ERR_EXIST` で断る。
  ext2 に限らずクラスごと止める二重の網。

ついでに `fs/vfs_resolve_path()` の [C1] 違反 (宣言がブロック途中) を解消した。
ホスト試験を `-Wdeclaration-after-statement -Werror` で通すため。

## この試験がやらないこと

- 実デバイス・実イメージ・エミュレータに触れない。`ext2_mount()` 本体と
  `kzalloc`/`kmemset`/`kstr*` は境界としてホスト側の同義実装に差し替えている。
- ゲスト上での `e2fsck -fn` クリーンは別途 [V1]/[V4] に従って実機で確認する。
