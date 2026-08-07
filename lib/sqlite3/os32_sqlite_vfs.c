/* ======================================================================== */
/*  OS32_SQLITE_VFS.C — OS32 カスタム SQLite VFS 実装                       */
/*                                                                          */
/*  OS32 の VFS (vfs.h) をバックエンドとする SQLite カスタム VFS。            */
/*  MEMSYS5 固定プールによるメモリ管理もここで初期化する。                    */
/*                                                                          */
/*  制約:                                                                   */
/*    - シングルタスク: ロック / ミューテックス は全て no-op                  */
/*    - xTruncate: ext2 truncate 未実装のため no-op                          */
/*    - ジャーナルモード: DELETE (デフォルト)                                 */
/* ======================================================================== */

#include "os32_sqlite_config.h"
#include "sqlite3.h"
#include "os32_sqlite_vfs.h"
#include "vfs.h"
#include "types.h"
#include "kstring.h"
#include "kprintf.h"

/* ======== MEMSYS5 固定プール (200KB) + canary ========
 * 一時的に 400KB へ拡張していたが、エンジンlib (board/battle/econ/inv) が
 * 起動時のRAMキャッシュ読込後もDB接続を握りっぱなしにしていたのが原因で、
 * 各libが init 完了時に db_close するよう修正して同時接続を1本に戻した。
 * よって拡張前の 200KB に復帰する。 */
#define SQLITE_MEMSYS5_SIZE  (200 * 1024)
#define CANARY_VALUE 0xDEADBEEFUL
static u32 canary_before[4] = {
    CANARY_VALUE, CANARY_VALUE, CANARY_VALUE, CANARY_VALUE
};
static char sqlite_mem_pool[SQLITE_MEMSYS5_SIZE];
static u32 canary_after[4] = {
    CANARY_VALUE, CANARY_VALUE, CANARY_VALUE, CANARY_VALUE
};

/* MEMSYS5 プールの canary 検証
 * 戻り値: 0=正常, -1=前方破壊, -2=後方破壊 */
int memsys5_check_canary(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (canary_before[i] != CANARY_VALUE) return -1;
    }
    for (i = 0; i < 4; i++) {
        if (canary_after[i] != CANARY_VALUE) return -2;
    }
    return 0;
}

/* ======== OMIT_ATTACH スタブ ======== */
/* sqlite3DbIsNamed は OMIT_ATTACH で定義除去されるが、             */
/* sqlite3FindIndex 等から参照が残る。シングルDB構成なので常に true。 */
int sqlite3DbIsNamed(void *db, int iDb, const char *zName)
{
    (void)db; (void)iDb; (void)zName;
    return (iDb == 0) ? 1 : 0;
}

/* ======== ファイルハンドル構造体 ======== */
typedef struct Os32File {
    sqlite3_file base;   /* SQLite 基底 (先頭に置く) */
    int fd;              /* OS32 VFS ファイルディスクリプタ */
} Os32File;

/* ======================================================================== */
/*  VFS ファイルメソッド                                                     */
/* ======================================================================== */

static int os32Close(sqlite3_file *pFile)
{
    Os32File *p = (Os32File *)pFile;
    if (p->fd >= 0) {
        vfs_close(p->fd);
        p->fd = -1;
    }
    return SQLITE_OK;
}

static int os32Read(sqlite3_file *pFile, void *buf, int iAmt,
                    sqlite3_int64 iOfst)
{
    Os32File *p = (Os32File *)pFile;
    int n;

    vfs_seek(p->fd, (int)iOfst, 0);  /* SEEK_SET */
    n = vfs_read_fd(p->fd, buf, (u32)iAmt);
    if (n < 0) return SQLITE_IOERR_READ;
    if (n < iAmt) {
        /* 不足分をゼロ埋め (SQLite 仕様) */
        memset((char *)buf + n, 0, (u32)(iAmt - n));
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int os32Write(sqlite3_file *pFile, const void *buf, int iAmt,
                     sqlite3_int64 iOfst)
{
    Os32File *p = (Os32File *)pFile;
    int n;

    vfs_seek(p->fd, (int)iOfst, 0);  /* SEEK_SET */
    n = vfs_write_fd(p->fd, buf, (u32)iAmt);
    if (n < iAmt) return SQLITE_IOERR_WRITE;
    return SQLITE_OK;
}

static int os32Truncate(sqlite3_file *pFile, sqlite3_int64 size)
{
    (void)pFile; (void)size;
    /* ext2 truncate 未実装 — no-op */
    /* 影響: VACUUM はファイルサイズを縮小できない。 */
    /*       journal_mode=DELETE ではジャーナル truncate が発生するが、 */
    /*       no-op でも DELETE モードのコミットフローに実害なし。 */
    return SQLITE_OK;
}

static int os32Sync(sqlite3_file *pFile, int flags)
{
    Os32File *p = (Os32File *)pFile;
    (void)flags;
    (void)p;
    vfs_sync();
    return SQLITE_OK;
}

static int os32FileSize(sqlite3_file *pFile, sqlite3_int64 *pSize)
{
    Os32File *p = (Os32File *)pFile;
    *pSize = (sqlite3_int64)vfs_get_size(p->fd);
    return SQLITE_OK;
}

/* ロック系: シングルタスクのため全て no-op */
static int os32Lock(sqlite3_file *f, int l)
    { (void)f; (void)l; return SQLITE_OK; }
static int os32Unlock(sqlite3_file *f, int l)
    { (void)f; (void)l; return SQLITE_OK; }
static int os32CheckReservedLock(sqlite3_file *f, int *pOut)
    { (void)f; *pOut = 0; return SQLITE_OK; }

static int os32FileControl(sqlite3_file *f, int op, void *pArg)
{
    (void)f; (void)op; (void)pArg;
    return SQLITE_NOTFOUND;
}

static int os32SectorSize(sqlite3_file *f)
    { (void)f; return 512; }
static int os32DeviceCharacteristics(sqlite3_file *f)
    { (void)f; return 0; }

/* ファイルメソッドテーブル */
static const sqlite3_io_methods os32_io_methods = {
    1,                          /* iVersion */
    os32Close,
    os32Read,
    os32Write,
    os32Truncate,
    os32Sync,
    os32FileSize,
    os32Lock,
    os32Unlock,
    os32CheckReservedLock,
    os32FileControl,
    os32SectorSize,
    os32DeviceCharacteristics,
    /* v2, v3 メソッドは NULL */
    0, 0, 0, 0, 0
};

/* ======================================================================== */
/*  VFS メソッド                                                             */
/* ======================================================================== */

static int os32VfsOpen(sqlite3_vfs *pVfs, const char *zName,
                       sqlite3_file *pFile, int flags, int *pOutFlags)
{
    Os32File *p = (Os32File *)pFile;
    int oflags = 0;

    (void)pVfs;

    p->base.pMethods = (sqlite3_io_methods *)0;
    p->fd = -1;

    /* 一時ファイル (zName==NULL) はメモリストアで処理されるはず */
    if (zName == (const char *)0) return SQLITE_CANTOPEN;

    if (flags & SQLITE_OPEN_CREATE)   oflags |= 0x0100; /* KAPI_O_CREAT */
    if (flags & SQLITE_OPEN_READWRITE) oflags |= 0x0002; /* KAPI_O_RDWR */
    else                               oflags |= 0x0000; /* KAPI_O_RDONLY */

    p->fd = vfs_open(zName, oflags);
    if (p->fd < 0) return SQLITE_CANTOPEN;

    p->base.pMethods = &os32_io_methods;
    if (pOutFlags) *pOutFlags = flags;
    return SQLITE_OK;
}

static int os32VfsDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync)
{
    int rc;
    (void)pVfs; (void)dirSync;
    rc = vfs_rm(zPath);
    /* SQLite 仕様: ファイルが存在しない場合も SQLITE_OK を返す */
    /* (ジャーナル削除時にファイルが既にないケースがある) */
    if (rc < 0) {
        /* 注意: .sqlite_text からの kprintf は PAGE FAULT を引き起こす */
        /* (既知の制約: 06_PHASES.md §1.6 参照) */
    }
    return SQLITE_OK;
}

static int os32VfsAccess(sqlite3_vfs *pVfs, const char *zPath,
                         int flags, int *pResOut)
{
    OS32_Stat st;
    int rc;
    (void)pVfs;

    rc = vfs_stat(zPath, &st);
    if (flags == SQLITE_ACCESS_EXISTS) {
        *pResOut = (rc == 0) ? 1 : 0;
    } else {
        *pResOut = (rc == 0) ? 1 : 0;
    }
    return SQLITE_OK;
}

static int os32VfsFullPathname(sqlite3_vfs *pVfs, const char *zName,
                               int nOut, char *zOut)
{
    (void)pVfs;
    kstrncpy(zOut, zName, (u32)nOut);
    return SQLITE_OK;
}

static int os32VfsRandomness(sqlite3_vfs *pVfs, int nByte, char *zOut)
{
    extern volatile u32 tick_count;
    u32 val;
    int i;
    (void)pVfs;

    val = tick_count;
    for (i = 0; i < nByte; i++) {
        val = val * 1103515245 + 12345;
        zOut[i] = (char)(val >> 16);
    }
    return nByte;
}

static int os32VfsSleep(sqlite3_vfs *pVfs, int microseconds)
{
    extern volatile u32 tick_count;
    u32 ticks;
    u32 start;
    (void)pVfs;

    ticks = (u32)microseconds / 10000; /* 10ms/tick (100Hz) */
    if (ticks == 0) ticks = 1;
    start = tick_count;
    while ((tick_count - start) < ticks) {
        /* ビジーウェイト */
    }
    return microseconds;
}

static int os32VfsCurrentTime(sqlite3_vfs *pVfs, double *pTime)
{
    extern u32 sys_time(void);
    u32 unix_time;
    (void)pVfs;

    unix_time = sys_time();
    /* Unix epoch → Julian day number 変換 */
    /* JD of 1970-01-01 = 2440587.5 */
    *pTime = 2440587.5 + (double)unix_time / 86400.0;
    return SQLITE_OK;
}

static int os32VfsGetLastError(sqlite3_vfs *pVfs, int nBuf, char *zBuf)
{
    (void)pVfs;
    if (nBuf > 0) zBuf[0] = '\0';
    return 0;
}

/* ======================================================================== */
/*  sqlite3_os_init / sqlite3_os_end — SQLite が呼び出す VFS 登録           */
/* ======================================================================== */

static sqlite3_vfs os32_vfs = {
    1,                      /* iVersion */
    sizeof(Os32File),       /* szOsFile */
    256,                    /* mxPathname */
    0,                      /* pNext */
    "os32",                 /* zName */
    0,                      /* pAppData */
    os32VfsOpen,
    os32VfsDelete,
    os32VfsAccess,
    os32VfsFullPathname,
    0, 0, 0, 0,            /* xDlOpen, xDlError, xDlSym, xDlClose */
    os32VfsRandomness,
    os32VfsSleep,
    os32VfsCurrentTime,
    os32VfsGetLastError,
    0,                      /* xCurrentTimeInt64 */
    0, 0, 0                 /* xSetSystemCall, xGetSystemCall, xNextSystemCall */
};

int sqlite3_os_init(void)
{
    return sqlite3_vfs_register(&os32_vfs, 1);
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}

/* ======================================================================== */
/*  os32_sqlite_init — カーネル初期化から呼ばれるエントリポイント              */
/* ======================================================================== */
int os32_sqlite_init(void)
{
    int rc;

    kprintf(0x07, "[SQT] init: pool=%x size=%x\n",
            (unsigned)sqlite_mem_pool, SQLITE_MEMSYS5_SIZE);

    /* MEMSYS5 固定プール設定 */
    rc = sqlite3_config(SQLITE_CONFIG_HEAP,
                        sqlite_mem_pool,
                        SQLITE_MEMSYS5_SIZE,
                        64);  /* 最小アロケーション粒度 */

    kprintf(0x07, "[SQT] sqlite3_config rc=%d\n", rc);
    if (rc != SQLITE_OK) return rc;

    /* SQLite 初期化 (内部で sqlite3_os_init が呼ばれる) */
    rc = sqlite3_initialize();

    kprintf(0x07, "[SQT] sqlite3_initialize rc=%d\n", rc);
    return rc;
}

/* ======================================================================== */
/*  os32_sqlite_dump_vfs — VFS メソッドテーブルのアドレスダンプ (デバッグ用)   */
/* ======================================================================== */
void os32_sqlite_dump_vfs(void)
{
    kprintf(0x07, "[VFS] io_methods=%x\n", (unsigned)&os32_io_methods);
    kprintf(0x07, "[VFS] xClose=%x xRead=%x xWrite=%x\n",
            (unsigned)os32_io_methods.xClose,
            (unsigned)os32_io_methods.xRead,
            (unsigned)os32_io_methods.xWrite);
    kprintf(0x07, "[VFS] xTruncate=%x xSync=%x xFileSize=%x\n",
            (unsigned)os32_io_methods.xTruncate,
            (unsigned)os32_io_methods.xSync,
            (unsigned)os32_io_methods.xFileSize);
}
