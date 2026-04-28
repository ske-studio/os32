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

/* ======== FatFs VFSコンテキスト ======== */
typedef struct {
    FATFS  fatfs;     /* FatFsワークエリア */
    int    dev_id;    /* OS32デバイスID */
    BYTE   pdrv;      /* FatFs物理ドライブ番号 */
    char   vol[4];    /* ボリューム文字列 "0:" or "1:" */
} FatFsCtx;


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
        kstrncat(out, path, max - len - 1);
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
    /* FAT時刻 → Unix時刻変換は簡略化 (RTC epochが不定のため0) */
    buf->st_mtime = 0;
    buf->st_atime = 0;
    buf->st_ctime = 0;

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


/* ======== マウント ======== */
static void *fatfs_vfs_mount(int dev_id)
{
    FatFsCtx *fc;
    FRESULT fr;

    fc = (FatFsCtx *)kmalloc(sizeof(FatFsCtx));
    if (!fc) return (void *)0;
    kmemset(fc, 0, sizeof(FatFsCtx));

    fc->dev_id = dev_id;

    /* dev_id → 物理ドライブ番号マッピング
     * dev_id 0-3 : FDD → pdrv=0
     * dev_id 4+  : HDD → pdrv=1
     */
    if (dev_id < 4) {
        fc->pdrv = 0;
    } else {
        fc->pdrv = 1;
    }

    fc->vol[0] = '0' + fc->pdrv;
    fc->vol[1] = ':';
    fc->vol[2] = '\0';

    /* FatFsにマウント要求 */
    fr = f_mount(&fc->fatfs, fc->vol, 1); /* 1 = 即座にマウント */
    if (fr != FR_OK) {
        kprintf(0x07, "[fatfs] mount failed: dev=%d pdrv=%d err=%d\n",
                dev_id, fc->pdrv, (int)fr);
        kfree(fc);
        return (void *)0;
    }

    kprintf(0x07, "[fatfs] mounted: dev=%d pdrv=%d type=FAT%s\n",
            dev_id, fc->pdrv,
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
