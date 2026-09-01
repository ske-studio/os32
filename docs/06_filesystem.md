## 第6部 ファイルシステム

### §6-1 VFS (仮想ファイルシステム) レイヤー

vfs.h/vfs.cが提供する統一的なファイル操作API。  
各FSドライバ(ext2, fat (FatFs), iso9660, hostdrv等)をVfsOps関数ポインタテーブルで抽象化し、
Linuxライクなコマンド体系を実現する。動的マウントに対応し、最大同時マウント数は `VFS_MAX_FS` (8) となる。
各FSドライバはマルチインスタンス方式 (mount時にctxをkmallocし、umount時にkfree) で複数デバイスの同時マウントをサポートする。

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

### §6-2 ext2 ファイルシステム (ext2_super.c / ext2_inode.c / ext2_dir.c / ext2_file.c / ext2_fmt.c / ext2_vfs.c)

IDE HDD上のLinux ext2ファイルシステムを読み書きする。パーティション開始位置は PC-98 パーティションテーブル (LBA 1) を `ext2_find_partition()` で解釈して動的に決定する (現行 NHD イメージではシリンダ12 = LBA 1632 開始。詳細は [NHD_FORMAT.md](NHD_FORMAT.md))。マルチインスタンス方式 (Ext2Ctx) により、複数デバイスの同時ext2マウントが可能。ext2フォーマット (`ext2_format`) によるmkfs相当の機能も備える。

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

### §6-4 FDリダイレクト (fd_redirect.c / fd_redirect.h)

VFSのファイルディスクリプタ (FD 0=stdin, 1=stdout) に対して、出力先をファイルまたはメモリバッファに切り替える機構。シェルのリダイレクト (`>`, `>>`, `<`) およびパイプ (`|`) を実現する基盤。

| 関数 | 説明 |
|------|------|
| `fd_redirect_to_file(fd, path, mode)` | FDの入出力先をファイルにリダイレクト |
| `fd_redirect_to_buffer(fd, buf, size, len)` | FDの入出力先をメモリバッファにリダイレクト |
| `fd_redirect_reset(fd)` | リダイレクトを解除しコンソールに復帰 |
| `fd_is_redirected(fd)` | FDがリダイレクト中か判定 |
| `fd_redirect_get_buf_len(fd)` | バッファリダイレクト時の書き込み済みバイト数取得 |

### §6-5 パイプバッファ (pipe_buffer.c / pipe_buffer.h)

コマンド間のデータ受け渡しに使用するカーネル管理のメモリバッファプール。最大同時確保数は `PIPE_BUF_COUNT` (2)、各バッファサイズは `PIPE_BUF_SIZE` (64KB)。

| 関数 | 説明 |
|------|------|
| `pipe_alloc()` | パイプバッファを1個確保 (IDを返す) |
| `pipe_free(id)` | パイプバッファを解放 |
| `pipe_get_buf(id)` | パイプバッファのデータポインタ取得 |
| `pipe_get_len(id)` | 書き込み済みバイト数取得 |
| `pipe_clear(id)` | パイプバッファをクリア |

### §6-6 ISO 9660 ファイルシステム (iso9660.c / iso9660.h)

CD-ROM上のISO 9660 Level 1ファイルシステムを読み取り専用でVFS経由で提供する。ATAPIドライバ (§5-6) を介してセクタ読み出しを行う。

| 項目 | 値 |
|------|-----|
| セクタサイズ | 2048バイト |
| PVD位置 | LBA 16 |
| ファイル名 | Level 1 (8.3)、case-insensitive比較 |
| 拡張 | Rock Ridge / Joliet 非対応 |
| 書き込み | 非対応 (全write系操作はVFS_ERR_IO) |

**VFS操作**:

| 関数 | 説明 |
|------|------|
| `iso9660_mount(dev_id)` | PVD読み込み、Iso9660Ctx確保 |
| `iso9660_umount(ctx)` | コンテキスト解放 |
| `iso9660_list_dir(ctx, path, cb, user)` | ディレクトリ一覧 (コールバック) |
| `iso9660_read_file(ctx, path, buf, max)` | ファイル全体読み込み |
| `iso9660_read_stream(ctx, path, buf, sz, off)` | オフセット付き部分読み込み |
| `iso9660_get_file_size(ctx, path, size)` | ファイルサイズ取得 |
| `iso9660_stat(ctx, path, st)` | ファイル情報取得 |

### §6-7 HostDrvFS (hostdrvfs.c / hostdrvfs.h)

NP21/WエミュレータのHostDrv機能を利用し、ホストPC (Windows) のファイルシステムにゲストOSから直接アクセスする仮想ファイルシステム。セッションベースのhypercall I/Oモデルで動作する。

| 項目 | 値 |
|------|-----|
| 通信方式 | NP21/W HostDrv hypercall (共有メモリ + I/Oポート) |
| マウントポイント | `/host` (カーネル自動マウント) |
| 対応操作 | READ, WRITE, LIST (ディレクトリ一覧) |
| 書き込み | 対応 (`hdrv_write_file` / `hdrv_write_stream`) |
| セッション管理 | CREATE → READ/WRITE/LIST → CLOSE のIRPシーケンス |
| 同期要件 | 通信バッファは `volatile` 宣言必須 (GCC最適化対策) |

**VFS操作**:

| 関数 | 説明 |
|------|------|
| `hostdrvfs_init()` | HostDrv検出・自動マウント |
| `hostdrvfs_read_file(ctx, path, buf, max)` | ファイル読み込み |
| `hostdrvfs_read_stream(ctx, path, buf, sz, off)` | オフセット付き部分読み込み |
| `hdrv_write_file / hdrv_write_stream` | ファイル書き込み (全体/部分) |
| `hostdrvfs_list_dir(ctx, path, cb, user)` | ディレクトリ一覧 (コールバック) |
| `hostdrvfs_get_file_size(ctx, path, size)` | ファイルサイズ取得 |
| `hostdrvfs_stat(ctx, path, st)` | ファイル情報取得 |

### §6-8 FAT (FatFs)

- **fatfs/ + fatfs_vfs.c** — ELM FatFs (elm-chan.org) の移植 + VfsOps 統合ラッパー。ext2_vfs.c と同じマルチインスタンスパターン (FatFsCtx を kmalloc/kfree)。FDD ブート時のルートFSでもある (`root_fs = "fat"`)
- FatFs のボリュームは pdrv 0/1 (fd0/fd1) の2つ。`fatfs_vfs_mount` は pdrv 単位の busy フラグで二重マウントを弾く (fd1 の自動マウント試行が fd0 のマウントを壊す経路があったため)

> **自作 FAT12 ドライバ (`fs/fat12.c`, 1,152行) は廃止済み。** FatFs と二重実装で
> あるうえ、FAT を8セクタに切り詰める (1.44MB で読みはサイレント切断・書きは
> ボリューム破壊)、`fat12_vfs_write/unlink` が basename しか見ずサブディレクトリに
> 書けない、といった不具合を抱えていた。FatFs は LFN・FAT16/32・サブディレクトリ
> 書き込み・rename・mkdir に対応した上位互換であるため一本化した。
> ホスト側の `tools/mkfat12.py` はイメージ生成ツールとして残っている。

**用途**: `hsync` コマンドによる `/host` → `/` へのファイル同期 (HostDrvデプロイワークフロー) の基盤。

---

