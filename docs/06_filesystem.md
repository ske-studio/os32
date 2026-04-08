## 第6部 ファイルシステム

### §6-1 VFS (仮想ファイルシステム) レイヤー

vfs.h/vfs.cが提供する統一的なファイル操作API。  
各FSドライバ(ext2, fat12等)をVfsOps関数ポインタテーブルで抽象化し、
Linuxライクなコマンド体系を実現する。動的マウントに対応し、最大同時マウント数は `VFS_MAX_FS` (8) となる。
FAT12やext2などのハイブリッドマウントをサポートする。

**VFS API**:

| 関数 | 説明 |
|------|------|
| `sys_mount(prefix, dev, fs)` | マウント ("/", "hd0", "ext2") |
| `sys_umount(prefix)` | アンマウント |
| `sys_is_mounted(prefix)`| マウント状態確認 |
| `sys_ls(path, cb, ctx)` | ディレクトリ一覧 (コールバック) |
| `sys_open(path, mode)` | ファイル/デバイスオープン (FD取得) |
| `sys_read(fd, buf, sz)` | FDから読込 |
| `sys_write(fd, buf, sz)` | FDへ書込 |
| `sys_close(fd)` | FDクローズ |
| `sys_lseek(fd, off, w)` | FDシーク |
| `sys_unlink(path)` | ファイル削除 |
| `sys_rename(old, new)` | ファイル名変更 |
| `sys_mkdir(path)` | ディレクトリ作成 |
| `sys_rmdir(path)` | ディレクトリ削除 |
| `sys_chdir(path)` | カレントディレクトリ変更 |
| `sys_getcwd()` | カレントディレクトリ取得 |
| `vfs_sync()` | メタデータ書き戻し |

パス解決: `vfs_resolve_path()` が相対パスをcwd基準で絶対パスに変換する。

### §6-2 ext2 ファイルシステム (ext2.c / ext2.h)

IDE HDD上のLinux ext2ファイルシステムを読み書きする。NHDヘッダに基づくPC-98固有のオフセット（NHD_HEADER_SECTORS = 136、シリンダ1開始）に対応している。

| 項目 | 値 |
|------|-----|
| ブロックサイズ | 1024バイト |
| inodeサイズ | 128バイト |
| inode数 | 動的 (ブロック数/4, 最大65536) |
| 対応ブロック | ダイレクト(12) + 間接(1) + 二重間接(1) |
| ルートinode | 2 (EXT2_ROOT_INO) |
| ブロックグループ | 単一グループ |

**API**:

| 関数 | 説明 |
|------|------|
| `ext2_mount(drive)` | マウント |
| `ext2_format(drive, total_sectors)` | ext2フォーマット (mkfs相当) |
| `ext2_read_file(ino, buf, max)` | ファイル読込 |
| `ext2_write(ino, data, sz)` | ファイル上書き |
| `ext2_create(dir, name, data, sz)` | ファイル新規作成 |
| `ext2_unlink(dir, name)` | ファイル削除 |
| `ext2_mkdir(dir, name)` | ディレクトリ作成 |
| `ext2_rmdir(dir, name)` | ディレクトリ削除 |
| `ext2_list_dir(ino, cb, ctx)` | ディレクトリ一覧 |
| `ext2_lookup(path, out_ino)` | パス→inode解決 |
| `ext2_sync()` | メタデータ書き戻し |

### §6-3 IDE ドライバ (ide.c / ide.h)

ATA PIOモードによるIDE HDD制御。

| 項目 | 仕様 |
|------|------|
| ベースI/O | 0x640 (プライマリ) |
| 転送モード | PIO (セクタ単位) |
| セクタサイズ | 512バイト |
| アドレッシング | LBA28 |

**API**:

| 関数 | 説明 |
|------|------|
| `ide_init()` | IDE検出・初期化 |
| `ide_identify(drv, info)` | ドライブ情報取得 (IdeInfo構造体) |
| `ide_read_sector(drv, lba, buf)` | 1セクタ読込 |
| `ide_write_sector(drv, lba, buf)` | 1セクタ書込 |
| `ide_write_sectors(drv, lba, cnt, buf)` | 複数セクタ連続書込 |
| `ide_drive_present(drv)` | ドライブ存在チェック |

---
