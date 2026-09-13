/* S3-K: FAT の stat が「そのボリュームに存在しえない名前」を NOTFOUND と
 * 言えるかの回帰試験 (2026-09-13)。
 *
 * 実物の fs/fatfs_vfs.c をそのまま取り込み、境界 (FatFs の f_*、Device /
 * IDE、kmalloc、kprintf) だけを贋物に差し替える。実デバイス・実イメージ・
 * 実 FatFs には一切触れない。
 *
 * 追う事象: FDD ブート (root = FAT、FF_USE_LFN 0) で KAPI v50 の
 * db_open_existing("/etc/settings.db", 0) が SQLITE_IOERR。v50 は hot
 * journal の有無を vfs_stat("<path>-journal") で見て NOTFOUND 以外を IOERR
 * とするが、"settings.db-journal" は 8.3 に収まらないので f_stat が
 * FR_INVALID_NAME を返していた。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs/ff.h"
#include "vfs.h"
#include "ide.h"
#include "dev.h"
#include "os_time.h"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); \
} } while (0)

/* ---- FatFs の境界 ----
 * f_stat / f_open は「次に返す FRESULT」を持ち、渡された FatFs パスを
 * 控える。他は呼ばれない前提で FR_INT_ERR を返す。 */
static FRESULT stub_stat_rc = FR_OK;
static FILINFO stub_stat_info;
static char    stub_last_path[FF_MAX_LFN + 32];
static int     stub_stat_calls;
static FRESULT stub_open_rc = FR_OK;

static void stub_record(const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof(stub_last_path)) n = sizeof(stub_last_path) - 1;
    memcpy(stub_last_path, path, n);
    stub_last_path[n] = '\0';
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    stub_stat_calls++;
    stub_record(path);
    if (stub_stat_rc != FR_OK) return stub_stat_rc;
    if (fno) *fno = stub_stat_info;
    return FR_OK;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    (void)mode;
    stub_record(path);
    if (fp) memset(fp, 0, sizeof(*fp));
    return stub_open_rc;
}

FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{ (void)fp; (void)buff; (void)btr; if (br) *br = 0; return FR_OK; }
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{ (void)fp; (void)buff; (void)btw; if (bw) *bw = btw; return FR_OK; }
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) { (void)fp; (void)ofs; return FR_OK; }
FRESULT f_unlink(const TCHAR *path) { stub_record(path); return stub_open_rc; }
FRESULT f_mkdir(const TCHAR *path) { stub_record(path); return stub_open_rc; }
FRESULT f_rename(const TCHAR *a, const TCHAR *b)
{ (void)b; stub_record(a); return stub_open_rc; }
FRESULT f_opendir(DIR *dp, const TCHAR *path)
{ (void)dp; stub_record(path); return stub_open_rc; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_readdir(DIR *dp, FILINFO *fno)
{ (void)dp; if (fno) fno->fname[0] = '\0'; return FR_OK; }
FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs)
{ (void)path; if (nclst) *nclst = 0; if (fatfs) *fatfs = 0; return FR_INT_ERR; }
FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{ (void)fs; (void)path; (void)opt; return FR_NOT_READY; }

/* ---- diskio / Device / IDE の境界 (mount 経路だけが使う) ---- */
void diskio_set_fdd_drive(int drv) { (void)drv; }
void diskio_set_hdd_drive(int drv) { (void)drv; }
void diskio_set_hdd_partition(u32 offset) { (void)offset; }
void diskio_set_hdd_sector_size(u16 sz) { (void)sz; }
void diskio_set_hdd_ide_phys_size(u16 sz) { (void)sz; }

int ide_get_info(int drive, IdeInfo *info)
{ (void)drive; (void)info; return -1; }
Device *dev_find(const char *name) { (void)name; return (Device *)0; }
int dev_blk_read_lba(Device *dev, u32 lba, int count, void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -1; }

os_time_t dos_time_to_epoch(u16 dos_date, u16 dos_time)
{ return (os_time_t)dos_date * 65536 + dos_time; }

void vfs_register_fs(VfsOps *ops) { (void)ops; }

/* ---- kernel の境界 ---- */
void *kzalloc(u32 size) { return calloc(1, size); }
void kfree(void *p) { free(p); }
void kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }

void *kmemset(void *dst, int val, u32 n) { return memset(dst, val, n); }
u32 kstrlen(const char *s) { return (u32)strlen(s); }
char *kstrncpy(char *dst, const char *src, u32 n)
{
    u32 i;
    if (n == 0) return dst;
    for (i = 0; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}
char *kstrncat(char *dst, const char *src, u32 n)
{
    u32 d = (u32)strlen(dst);
    if (d + 1 >= n) return dst;
    kstrncpy(dst + d, src, n - d);
    return dst;
}

#include "../../fs/fatfs_vfs.c"

/* ---- 試験用コンテキスト (mount を通さず直接組む) ---- */
static FatFsCtx test_ctx;

static void reset(void)
{
    memset(&test_ctx, 0, sizeof(test_ctx));
    test_ctx.pdrv = 0;          /* FDD */
    test_ctx.vol[0] = '0';
    test_ctx.vol[1] = ':';
    test_ctx.vol[2] = '\0';

    memset(&stub_stat_info, 0, sizeof(stub_stat_info));
    stub_stat_rc = FR_OK;
    stub_open_rc = FR_OK;
    stub_stat_calls = 0;
    stub_last_path[0] = '\0';
}

/* ケース 1: 8.3 に収まらない名前の stat は「存在しない」。
 * FF_USE_LFN 0 の FatFs は f_stat に FR_INVALID_NAME を返す。 */
static int case_stat_invalid_name_is_notfound(void)
{
    OS32_Stat st;
    reset();
    stub_stat_rc = FR_INVALID_NAME;
    memset(&st, 0xAA, sizeof(st));
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db-journal", &st)
          == VFS_ERR_NOTFOUND);
    CHECK(stub_stat_calls == 1);
    CHECK(strcmp(stub_last_path, "0:/etc/settings.db-journal") == 0);
    return 0;
}

/* ケース 2: 素直な不存在も NOTFOUND (FR_NO_FILE / FR_NO_PATH)。 */
static int case_stat_missing_is_notfound(void)
{
    OS32_Stat st;
    reset();
    stub_stat_rc = FR_NO_FILE;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/nosuch", &st) == VFS_ERR_NOTFOUND);
    reset();
    stub_stat_rc = FR_NO_PATH;
    CHECK(fatfs_vfs_stat(&test_ctx, "/nodir/x", &st) == VFS_ERR_NOTFOUND);
    return 0;
}

/* ケース 3: I/O 障害は NOTFOUND に化けない (v50 が IOERR と言うべき側)。 */
static int case_stat_disk_err_is_io(void)
{
    OS32_Stat st;
    reset();
    stub_stat_rc = FR_DISK_ERR;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db", &st) == VFS_ERR_IO);
    reset();
    stub_stat_rc = FR_NOT_READY;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db", &st) == VFS_ERR_IO);
    reset();
    stub_stat_rc = FR_NO_FILESYSTEM;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db", &st) == VFS_ERR_IO);
    return 0;
}

/* ケース 4: 正常時の st_size / st_mode。 */
static int case_stat_ok_fills_size_mode(void)
{
    OS32_Stat st;

    reset();
    stub_stat_info.fsize = 4096;
    stub_stat_info.fattrib = 0;
    stub_stat_info.fdate = 0;
    stub_stat_info.ftime = 0;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db", &st) == VFS_OK);
    CHECK(st.st_size == 4096);
    CHECK(st.st_mode == 0100644);

    reset();
    stub_stat_info.fsize = 0;
    stub_stat_info.fattrib = AM_DIR;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc", &st) == VFS_OK);
    CHECK(st.st_size == 0);
    CHECK(st.st_mode == 0040755);

    reset();
    stub_stat_info.fsize = 12;
    stub_stat_info.fattrib = AM_RDO;
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/ro.txt", &st) == VFS_OK);
    CHECK(st.st_size == 12);
    CHECK(st.st_mode == 0100444);

    /* buf が NULL の呼びは呼び手の誤りなので INVAL のまま。 */
    reset();
    CHECK(fatfs_vfs_stat(&test_ctx, "/etc/settings.db", (OS32_Stat *)0)
          == VFS_ERR_INVAL);
    return 0;
}

/* ケース 5: get_size も存在確認の入口なので同じ写像。 */
static int case_get_size_invalid_name_is_notfound(void)
{
    u32 size = 0xDEADBEEF;
    reset();
    stub_stat_rc = FR_INVALID_NAME;
    CHECK(fatfs_vfs_get_size(&test_ctx, "/etc/settings.db-journal", &size)
          == VFS_ERR_NOTFOUND);
    reset();
    stub_stat_rc = FR_DISK_ERR;
    CHECK(fatfs_vfs_get_size(&test_ctx, "/etc/settings.db", &size)
          == VFS_ERR_IO);
    return 0;
}

/* ケース 6: open / read / write / unlink 系の INVAL は変えない。
 * 「呼び手が不正な名前を渡した」診断はそのまま残す。 */
static int case_open_paths_keep_inval(void)
{
    char buf[16];
    reset();
    stub_open_rc = FR_INVALID_NAME;
    CHECK(fatfs_vfs_read(&test_ctx, "/etc/settings.db-journal", buf,
                         sizeof(buf)) == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_write(&test_ctx, "/etc/settings.db-journal", buf,
                          sizeof(buf)) == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_read_stream(&test_ctx, "/etc/settings.db-journal", buf,
                                sizeof(buf), 0) == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_write_stream(&test_ctx, "/etc/settings.db-journal", buf,
                                 sizeof(buf), 0) == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_unlink(&test_ctx, "/etc/settings.db-journal")
          == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_mkdir(&test_ctx, "/etc/verylongdirname")
          == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_rename(&test_ctx, "/a", "/etc/settings.db-journal")
          == VFS_ERR_INVAL);
    CHECK(fatfs_vfs_list(&test_ctx, "/etc/verylongdirname",
                         (vfs_dir_cb)0, (void *)0) == VFS_ERR_INVAL);
    return 0;
}

/* ケース 7: v50 の hot journal 検査そのものの再現。
 * 「本体は在る / journal は 8.3 に収まらない」で、v50 が IOERR ではなく
 * 「journal は無い」と判断できること。 */
static int case_v50_journal_probe_on_8_3(void)
{
    OS32_Stat st;
    int rc;

    reset();
    stub_stat_info.fsize = 8192;
    stub_stat_info.fattrib = 0;
    rc = fatfs_vfs_stat(&test_ctx, "/etc/settings.db", &st);
    CHECK(rc == VFS_OK);
    CHECK(st.st_size == 8192);

    /* 同じボリュームで journal 名を問う。8.3 に収まらないので FatFs は
     * FR_INVALID_NAME。v50 は NOTFOUND なら「無い」と読んで先へ進む。 */
    stub_stat_rc = FR_INVALID_NAME;
    rc = fatfs_vfs_stat(&test_ctx, "/etc/settings.db-journal", &st);
    CHECK(rc == VFS_ERR_NOTFOUND);
    CHECK(rc != VFS_ERR_INVAL);
    return 0;
}

struct case_ent { const char *name; int (*fn)(void); };
static const struct case_ent cases[] = {
    { "stat_invalid_name_is_notfound",   case_stat_invalid_name_is_notfound },
    { "stat_missing_is_notfound",        case_stat_missing_is_notfound },
    { "stat_disk_err_is_io",             case_stat_disk_err_is_io },
    { "stat_ok_fills_size_mode",         case_stat_ok_fills_size_mode },
    { "get_size_invalid_name_is_notfound", case_get_size_invalid_name_is_notfound },
    { "open_paths_keep_inval",           case_open_paths_keep_inval },
    { "v50_journal_probe_on_8_3",        case_v50_journal_probe_on_8_3 }
};

int main(int argc, char **argv)
{
    unsigned i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(cases[i].name, argv[1]) == 0) return cases[i].fn();
    }
    fprintf(stderr, "unknown case: %s\n", argv[1]);
    return 2;
}
