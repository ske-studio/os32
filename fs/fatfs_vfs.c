/* ======================================================================== */
/*  FATFS_VFS.C — FatFs VFS統合ラッパー                                     */
/*                                                                          */
/*  FatFs (elm-chan.org) のAPIをOS32のVfsOpsインターフェースに接続する。      */
/*  ext2_vfs.c と同パターンのマルチインスタンス対応。                        */
/*                                                                          */
/*  mount() で FatFsCtx を kmalloc確保、umount() で kfree解放。             */
/* ======================================================================== */

#include "fatfs/ff.h"
#include "vfs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "kprintf.h"
#include "ide.h"    /* ide_get_info, ide_drive_present — ジオメトリ情報取得のみ */
#include "dev.h"
#include "os_time.h"

extern void diskio_set_fdd_drive(int drv);
extern void diskio_set_hdd_drive(int drv);
extern void diskio_set_hdd_partition(u32 offset);
extern void diskio_set_hdd_sector_size(u16 sz);
extern void diskio_set_hdd_ide_phys_size(u16 sz);

/* ======== FatFs VFSコンテキスト ======== */
typedef struct {
    FATFS  fatfs;     /* FatFsワークエリア */
    int    dev_id;    /* OS32デバイスID */
    BYTE   pdrv;      /* FatFs物理ドライブ番号 */
    char   vol[4];    /* ボリューム文字列 "0:" or "1:" */
} FatFsCtx;

/* 物理ドライブ占有フラグ (pdrv 0=FDD, 1=HDD)。
 * diskio.c のドライブ選択と FatFs[] ボリューム登録はグローバルなため、
 * 同一 pdrv への多重マウントは先行マウントを破壊する。ここで遮断する。 */
static int pdrv_busy[FF_VOLUMES];


/* ======== FRESULT → VFSエラー変換 ======== */
static int ff_to_vfs(FRESULT fr)
{
    switch (fr) {
    case FR_OK:             return VFS_OK;
    case FR_NO_FILE:        return VFS_ERR_NOTFOUND;
    case FR_NO_PATH:        return VFS_ERR_NOTFOUND;
    case FR_INVALID_NAME:   return VFS_ERR_INVAL;
    case FR_DENIED:         return VFS_ERR_IO;
    case FR_EXIST:          return VFS_ERR_EXIST;
    case FR_WRITE_PROTECTED: return VFS_ERR_IO;
    case FR_NOT_READY:      return VFS_ERR_IO;
    case FR_DISK_ERR:       return VFS_ERR_IO;
    case FR_INT_ERR:        return VFS_ERR_IO;
    case FR_NOT_ENABLED:    return VFS_ERR_IO;
    case FR_NO_FILESYSTEM:  return VFS_ERR_IO;
    default:                return VFS_ERR_IO;
    }
}


/* ======== パスにボリューム接頭辞を付加 ======== */
static void ff_make_path(const FatFsCtx *fc, const char *path, char *out, int max)
{
    int len;
    /* "0:/path..." のような FatFs パスを構築 */
    out[0] = fc->vol[0];
    out[1] = ':';
    out[2] = '\0';
    len = 2;

    if (path && path[0]) {
        if (path[0] != '/') {
            out[len++] = '/';
            out[len] = '\0';
        }
        /* kstrncat の n は「残り容量」ではなくバッファ全体サイズ
         * (strlcat セマンティクス)。max - len - 1 を渡すと数文字ぶん
         * 早く切り詰められていた */
        kstrncat(out, path, (u32)max);
    } else {
        out[len++] = '/';
        out[len] = '\0';
    }
}


/* ======== ディレクトリ一覧 ======== */
static int fatfs_vfs_list(void *ctx, const char *path, vfs_dir_cb cb, void *user_ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    VfsDirEntry ve;

    ff_make_path(fc, path, fpath, sizeof(fpath));

    fr = f_opendir(&dir, fpath);
    if (fr != FR_OK) return ff_to_vfs(fr);

    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') break;

        kstrncpy(ve.name, fno.fname, VFS_MAX_PATH);
        ve.type = (fno.fattrib & AM_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        ve.size = fno.fsize;
        cb(&ve, user_ctx);
    }

    f_closedir(&dir);
    return VFS_OK;
}


/* ======== ファイル読み込み (一括) ======== */
static int fatfs_vfs_read(void *ctx, const char *path, void *buf, u32 max_size)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FIL fil;
    FRESULT fr;
    UINT br;

    ff_make_path(fc, path, fpath, sizeof(fpath));

    fr = f_open(&fil, fpath, FA_READ);
    if (fr != FR_OK) return ff_to_vfs(fr);

    fr = f_read(&fil, buf, max_size, &br);
    f_close(&fil);

    if (fr != FR_OK) return ff_to_vfs(fr);
    return (int)br;
}


/* ======== ファイル書き込み (一括) ======== */
static int fatfs_vfs_write(void *ctx, const char *path, const void *data, u32 size)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FIL fil;
    FRESULT fr;
    UINT bw;

    ff_make_path(fc, path, fpath, sizeof(fpath));

    fr = f_open(&fil, fpath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) return ff_to_vfs(fr);

    fr = f_write(&fil, data, size, &bw);
    f_close(&fil);

    if (fr != FR_OK) return ff_to_vfs(fr);
    return (int)bw;
}


/* ======== ファイル削除 ======== */
static int fatfs_vfs_unlink(void *ctx, const char *path)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];

    ff_make_path(fc, path, fpath, sizeof(fpath));
    return ff_to_vfs(f_unlink(fpath));
}


/* ======== ディレクトリ作成 ======== */
static int fatfs_vfs_mkdir(void *ctx, const char *path)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];

    ff_make_path(fc, path, fpath, sizeof(fpath));
    return ff_to_vfs(f_mkdir(fpath));
}


/* ======== ディレクトリ削除 ======== */
static int fatfs_vfs_rmdir(void *ctx, const char *path)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];

    /* FatFsでは f_unlink でディレクトリ削除も可能 (空の場合) */
    ff_make_path(fc, path, fpath, sizeof(fpath));
    return ff_to_vfs(f_unlink(fpath));
}


/* ======== リネーム ======== */
static int fatfs_vfs_rename(void *ctx, const char *old_path, const char *new_path)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath_old[VFS_MAX_PATH];
    char fpath_new[VFS_MAX_PATH];

    ff_make_path(fc, old_path, fpath_old, sizeof(fpath_old));
    ff_make_path(fc, new_path, fpath_new, sizeof(fpath_new));
    return ff_to_vfs(f_rename(fpath_old, fpath_new));
}


/* ======== ファイルサイズ取得 ======== */
static int fatfs_vfs_get_size(void *ctx, const char *path, u32 *size)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FILINFO fno;
    FRESULT fr;

    ff_make_path(fc, path, fpath, sizeof(fpath));
    fr = f_stat(fpath, &fno);
    if (fr != FR_OK) return ff_to_vfs(fr);
    *size = fno.fsize;
    return VFS_OK;
}


/* ======== ストリーム読み込み (オフセット指定) ======== */
static int fatfs_vfs_read_stream(void *ctx, const char *path, void *buf, u32 size, u32 offset)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FIL fil;
    FRESULT fr;
    UINT br;

    ff_make_path(fc, path, fpath, sizeof(fpath));

    fr = f_open(&fil, fpath, FA_READ);
    if (fr != FR_OK) return ff_to_vfs(fr);

    fr = f_lseek(&fil, (FSIZE_t)offset);
    if (fr != FR_OK) { f_close(&fil); return ff_to_vfs(fr); }

    fr = f_read(&fil, buf, size, &br);
    f_close(&fil);

    if (fr != FR_OK) return ff_to_vfs(fr);
    return (int)br;
}


/* ======== ストリーム書き込み (オフセット指定) ======== */
static int fatfs_vfs_write_stream(void *ctx, const char *path, const void *data, u32 size, u32 offset)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FIL fil;
    FRESULT fr;
    UINT bw;

    ff_make_path(fc, path, fpath, sizeof(fpath));

    fr = f_open(&fil, fpath, FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) return ff_to_vfs(fr);

    fr = f_lseek(&fil, (FSIZE_t)offset);
    if (fr != FR_OK) { f_close(&fil); return ff_to_vfs(fr); }

    fr = f_write(&fil, data, size, &bw);
    f_close(&fil);

    if (fr != FR_OK) return ff_to_vfs(fr);
    return (int)bw;
}


/* ======== stat ======== */
static int fatfs_vfs_stat(void *ctx, const char *path, OS32_Stat *buf)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    char fpath[VFS_MAX_PATH];
    FILINFO fno;
    FRESULT fr;

    if (!buf) return VFS_ERR_INVAL;

    ff_make_path(fc, path, fpath, sizeof(fpath));
    fr = f_stat(fpath, &fno);
    if (fr != FR_OK) return ff_to_vfs(fr);

    kmemset(buf, 0, sizeof(OS32_Stat));
    buf->st_size = fno.fsize;
    /* FAT属性 → POSIXモードへの簡易変換 */
    if (fno.fattrib & AM_DIR) {
        buf->st_mode = 0040755; /* drwxr-xr-x */
    } else if (fno.fattrib & AM_RDO) {
        buf->st_mode = 0100444; /* -r--r--r-- */
    } else {
        buf->st_mode = 0100644; /* -rw-r--r-- */
    }
    buf->st_mtime = dos_time_to_epoch(fno.fdate, fno.ftime);
    buf->st_atime = buf->st_mtime;
    buf->st_ctime = buf->st_mtime;

    return VFS_OK;
}


/* ======== sync ======== */
static int fatfs_vfs_sync(void *ctx)
{
    (void)ctx;
    /* FatFsはf_close/f_syncでフラッシュ済み */
    return VFS_OK;
}


/* ======== ブロック情報 ======== */
static u32 fatfs_vfs_total_blocks(void *ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    return fc->fatfs.n_fatent - 2; /* 総クラスタ数 */
}

static u32 fatfs_vfs_free_blocks(void *ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    DWORD nclst;
    FATFS *fs;
    char vol[4];

    vol[0] = fc->vol[0];
    vol[1] = ':';
    vol[2] = '\0';

    if (f_getfree(vol, &nclst, &fs) != FR_OK) return 0;
    return (u32)nclst;
}

static u32 fatfs_vfs_block_size(void *ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    /* FF_MAX_SS != FF_MIN_SS の場合、ssize にランタイムセクタサイズが格納される */
#if FF_MAX_SS == FF_MIN_SS
    return (u32)fc->fatfs.csize * (u32)FF_MAX_SS;
#else
    return (u32)fc->fatfs.csize * (u32)fc->fatfs.ssize;
#endif
}


/* ======== PC-98パーティション検出 ======== */

/* PC-98パーティションテーブルエントリ (32バイト)
 * LBA 1 に最大16エントリ格納 (512バイトセクタの場合) */
typedef struct {
    u8  bootable;        /* 0x80 = アクティブ */
    u8  sys_id;          /* システムID */
    u8  reserved1[2];
    u8  ipl_sector;      /* IPL開始セクタ */
    u8  ipl_head;        /* IPL開始ヘッド */
    u16 ipl_cyl;         /* IPL開始シリンダ */
    u8  start_sector;    /* パーティション開始セクタ */
    u8  start_head;      /* パーティション開始ヘッド */
    u16 start_cyl;       /* パーティション開始シリンダ */
    u8  end_sector;      /* パーティション終了セクタ */
    u8  end_head;        /* パーティション終了ヘッド */
    u16 end_cyl;         /* パーティション終了シリンダ */
    char name[16];       /* パーティション名 (SJIS) */
} PC98PartEntry;

/* FAT系パーティションかどうかの判定 (sys_id) */
static int pc98_is_fat_sysid(u8 id)
{
    /* PC-98 DOS パーティションタイプ:
     * 0x01/0x11/0x21: FAT12
     * 0x04/0x14/0x24: FAT16 (<32MB)
     * 0x06/0x16/0x26: FAT16 (>32MB)
     * 0x0B/0x1B/0x2B: FAT32
     * 0x81/0xA1: アクティブ+FAT12
     * 上位ニブルのフラグ部分を無視してチェック */
    u8 base = id & 0x7F;
    if (base == 0x01 || base == 0x11 || base == 0x21) return 1; /* FAT12 */
    if (base == 0x04 || base == 0x14 || base == 0x24) return 1; /* FAT16 */
    if (base == 0x06 || base == 0x16 || base == 0x26) return 1; /* FAT16B */
    if (base == 0x0B || base == 0x1B || base == 0x2B) return 1; /* FAT32 */
    return 0;
}

/* CHS→LBA変換 (ジオメトリ情報使用) */
static u32 chs_to_lba(u16 cyl, u8 head, u8 sector, u16 nheads, u16 nspt)
{
    if (nheads == 0 || nspt == 0) return 0;
    return ((u32)cyl * nheads + head) * nspt + sector;
}

/* HDD のパーティションテーブルをスキャンしてFATパーティションのLBAを返す
 * 見つからなければ 0 を返す
 * phys_sec_size: IDE物理セクタサイズ (256=SASI, 512=IDE) */
static u32 pc98_find_fat_partition(int drv, u16 phys_sec_size)
{
    u8 buf[512];
    IdeInfo info;
    u16 heads, spt;
    int i;
    int pt_offset;  /* パーティションテーブルのbuf内オフセット */
    int pt_max;     /* 最大エントリ数 */
    char devname[8];
    Device *dev;

    /* ジオメトリ取得 */
    if (ide_get_info(drv, &info) != 0) return 0;
    heads = info.heads;
    spt   = info.sectors;

    /* Device API ポインタ取得 */
    devname[0] = 'h'; devname[1] = 'd';
    devname[2] = '0' + (char)drv; devname[3] = '\0';
    dev = dev_find(devname);
    if (!dev) return 0;

    kprintf(0x07, "[pc98pt] drv=%d geom: C=%d H=%d S=%d total=%lu phys=%d\n",
            drv, info.cylinders, heads, spt,
            (unsigned long)info.total_sectors, phys_sec_size);

    /* PC-98パーティションテーブルの読み取り。
     * SASI 256Bセクタの場合: パーティションテーブルはHDIセクタ1(256B)にあるが、
     * IDEは512B転送なのLBA 0を読むとHDI sec 0+1が結合される。
     * よってbufの後半256Bにパーティションテーブルがある。
     * IDE 512Bセクタの場合: 従来通りセクタ1を読む。 */
    if (phys_sec_size == 256) {
        /* SASI: LBA 0を読んで後半256Bからパース */
        if (dev_blk_read_lba(dev, 0, 1, buf) != 0) return 0;
        pt_offset = 256;
        pt_max = 8;  /* (512-256)/32 = 8エントリ */
    } else {
        /* IDE: セクタ1を読む */
        if (dev_blk_read_lba(dev, 1, 1, buf) != 0) return 0;
        pt_offset = 0;
        pt_max = 16;  /* 512/32 = 16エントリ */
    }

    /* パーティションエントリをスキャン */
    for (i = 0; i < pt_max; i++) {
        PC98PartEntry *pe = (PC98PartEntry *)(buf + pt_offset + i * 32);
        u32 lba;

        /* 空エントリの検出: sys_idとbootableが共に0なら終了 */
        if (pe->sys_id == 0 && pe->bootable == 0) break;

        kprintf(0x07, "[pc98pt] entry %d: sys=%02x boot=%02x "
                "chs=%d/%d/%d\n",
                i, pe->sys_id, pe->bootable,
                pe->start_cyl, pe->start_head, pe->start_sector);

        /* FAT系パーティションを探す */
        if (pc98_is_fat_sysid(pe->sys_id)) {
            lba = chs_to_lba(pe->start_cyl, pe->start_head,
                             pe->start_sector, heads, spt);
            kprintf(0x07, "[pc98pt] FAT partition at LBA %lu "
                    "(C=%d H=%d S=%d)\n",
                    (unsigned long)lba, pe->start_cyl,
                    pe->start_head, pe->start_sector);
            return lba;
        }
    }

    /* PC-98形式で見つからなかった場合、IBM PC MBRも試す */
    if (dev_blk_read_lba(dev, 0, 1, buf) != 0) return 0;
    if (buf[510] == 0x55 && buf[511] == 0xAA) {
        /* 標準MBRパーティションテーブル (offset 446) */
        for (i = 0; i < 4; i++) {
            u8 *pe = buf + 446 + i * 16;
            u8 type = pe[4];
            u32 lba;
            if (type == 0) continue;
            /* FAT系: 01,04,06,0B,0C,0E,0F */
            if (type == 0x01 || type == 0x04 || type == 0x06 ||
                type == 0x0B || type == 0x0C || type == 0x0E) {
                lba = (u32)pe[8] | ((u32)pe[9] << 8) |
                      ((u32)pe[10] << 16) | ((u32)pe[11] << 24);
                kprintf(0x07, "[mbr] FAT partition at LBA %lu "
                        "(type=%02x)\n", (unsigned long)lba, type);
                return lba;
            }
        }
    }

    return 0;
}


/* ======== マウント ======== */
static void *fatfs_vfs_mount(int dev_id)
{
    FatFsCtx *fc;
    FRESULT fr;
    int dev_type;
    int drv_num;

    /* VFSからのエンコード: (dev_type << 8) | drv_num
     * dev_type=0 (VFS_DEV_HD) → HDD → pdrv=1
     * dev_type=1 (VFS_DEV_FD) → FDD → pdrv=0
     */
    dev_type = (dev_id >> 8) & 0xFF;
    drv_num  = dev_id & 0xFF;

    /* FDD/HDD 以外 (CD, hostdrv等) は対象外 */
    if (dev_type != 0 && dev_type != 1) return (void *)0;

    /* pdrv 占有チェック: diskio のドライブ選択を書き換える前に判定する */
    if (pdrv_busy[dev_type == 1 ? 0 : 1]) return (void *)0;

    fc = (FatFsCtx *)kzalloc(sizeof(FatFsCtx));
    if (!fc) return (void *)0;

    fc->dev_id = dev_id;

    if (dev_type == 1) {
        /* FDD */
        fc->pdrv = 0;
        diskio_set_fdd_drive(drv_num);
    } else {
        /* HDD */
        fc->pdrv = 1;
        diskio_set_hdd_drive(drv_num);
    }

    fc->vol[0] = '0' + fc->pdrv;
    fc->vol[1] = ':';
    fc->vol[2] = '\0';

    /* FatFsにマウント要求 */
    fr = f_mount(&fc->fatfs, fc->vol, 1); /* 1 = 即座にマウント */

    /* マウント失敗かつHDDの場合、パーティションテーブルを解析してリトライ */
    if (fr == FR_NO_FILESYSTEM && dev_type == 0) {
        IdeInfo ide_info;
        u16 phys_sec_size = 512;
        u32 part_lba;
        Device *blkdev = (Device *)0;

        /* IDE物理セクタサイズを取得 (SASI 256B判定) */
        if (ide_get_info(drv_num, &ide_info) == 0) {
            phys_sec_size = ide_info.phys_sector_size;
        }
        diskio_set_hdd_ide_phys_size(phys_sec_size);

        /* Device API ポインタ取得 */
        {
            char dname[8];
            dname[0] = 'h'; dname[1] = 'd';
            dname[2] = '0' + (char)drv_num; dname[3] = '\0';
            blkdev = dev_find(dname);
        }

        part_lba = pc98_find_fat_partition(drv_num, phys_sec_size);
        if (part_lba > 0) {
            u8 dbg[512];
            u16 bps = 512;
            kprintf(0x07, "[fatfs] retrying with partition offset LBA=%lu "
                    "(phys=%d)\n",
                    (unsigned long)part_lba, phys_sec_size);
            /* パーティション先頭セクタを直接読んでBPS取得。
             * part_lbaはphys_sec_size単位のLBA。
             * dev_blk_read_lbaは512B返すのでBPBの先頭は含まれる。 */
            if (blkdev && dev_blk_read_lba(blkdev, (u32)part_lba, 1, dbg) == 0) {
                bps = (u16)dbg[11] | ((u16)dbg[12] << 8);
                kprintf(0x07, "[fatfs] sector %lu: jmp=%02x BPS=%d\n",
                        (unsigned long)part_lba, dbg[0], bps);
            }
            diskio_set_hdd_partition(part_lba);
            /* BPBの論理セクタサイズをdiskioに反映 (PC-98 DOSでは1024) */
            if (bps == 1024 || bps == 512) {
                diskio_set_hdd_sector_size(bps);
            }
            /* unmount してからリトライ */
            f_mount(0, fc->vol, 0);
            fr = f_mount(&fc->fatfs, fc->vol, 1);
        }
    }

    if (fr != FR_OK) {
        kprintf(0x07, "[fatfs] mount failed: type=%d drv=%d pdrv=%d err=%d\n",
                dev_type, drv_num, fc->pdrv, (int)fr);
        /* FatFs[] に登録済みの fc->fatfs を解除してから解放する
         * (解除しないと解放済み領域への参照が残る) */
        f_mount((FATFS *)0, fc->vol, 0);
        kfree(fc);
        return (void *)0;
    }

    pdrv_busy[fc->pdrv] = 1;

    kprintf(0x07, "[fatfs] mounted: type=%d drv=%d pdrv=%d FAT%s\n",
            dev_type, drv_num, fc->pdrv,
            fc->fatfs.fs_type == 1 ? "12" :
            fc->fatfs.fs_type == 2 ? "16" :
            fc->fatfs.fs_type == 3 ? "32" : "??");

    return (void *)fc;
}


/* ======== アンマウント ======== */
static void fatfs_vfs_umount(void *ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;

    if (fc) {
        f_mount((FATFS *)0, fc->vol, 0); /* アンマウント */
        pdrv_busy[fc->pdrv] = 0;
        kfree(fc);
    }
}


/* ======== マウント状態確認 ======== */
static int fatfs_vfs_is_mounted(void *ctx)
{
    FatFsCtx *fc = (FatFsCtx *)ctx;
    return (fc != (void *)0) ? 1 : 0;
}


/* ======== VfsOps テーブル ======== */
static VfsOps fatfs_ops = {
    "fat",
    fatfs_vfs_mount,       fatfs_vfs_umount,      fatfs_vfs_is_mounted,
    fatfs_vfs_list,        fatfs_vfs_mkdir,        fatfs_vfs_rmdir,
    fatfs_vfs_read,        fatfs_vfs_write,        fatfs_vfs_unlink,
    fatfs_vfs_rename,
    fatfs_vfs_get_size,    fatfs_vfs_read_stream,  fatfs_vfs_write_stream,
    fatfs_vfs_sync,
    fatfs_vfs_total_blocks, fatfs_vfs_free_blocks, fatfs_vfs_block_size,
    fatfs_vfs_stat
};


/* ======== 初期化・登録 ======== */
void fatfs_init(void)
{
    vfs_register_fs(&fatfs_ops);
}
