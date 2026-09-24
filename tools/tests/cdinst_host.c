/* ========================================================================
 *  cdinst_host.c — 実物の cdinst.c を main から回す (票 TASK_HDD_INSTALL 段 2)
 *
 *  実行:   python3 -B tools/tests/test_hdd_stage2.py
 *  記録:   tools/tests/hdd_stage2_tdd.md
 *
 *  取り込む実物: userland/system/cdinst.c (main ごと)・inst_hdd.c・inst_disk.c・
 *  userland/lib/rt/pkg.c・userland/shell/hdprep_plan.c・drivers/pc98pt.c・
 *  fs/ext2_layout.c。贋物は KAPI だけ:
 *    - /cd0 の .PKG は試験がバイト列で組む (無圧縮、PKG1 形式)
 *    - hd0 は LBA 0〜31 のセクタの模型 (区画表・IPL・ローダの帯)。ext2 の
 *      中身は ext2_format_at の呼び出しを記録するだけ (実物の format_at と
 *      e2fsck は test_hdd_stage1.py が見る)
 *    - /hd0 のファイルは名前と大きさだけを持つ
 *  見るもの: 幾何 (8/17・16/63) と区画表・IPL の一致、モード、事前検査の失敗で
 *  1 セクタも書かないこと、書いた後の失敗で「完了」と言わないこと。
 *
 *  pkg.c の PkgHeader は u32 = unsigned long を詰めて並べるので ILP32 で組む
 *  (ext2_part_host.c と同じ様式 — ホスト -m32、-nostdlib)。
 * ======================================================================== */

#include "os32api.h"
#include <stdarg.h>

void *memcpy(void *dst, const void *src, u32 n);
void *memset(void *dst, int val, u32 n);
u32   strlen(const char *s);
char *strcpy(char *dst, const char *src);

/* ======================================================================== */
/*  libc の代わり                                                           */
/* ======================================================================== */

static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) { }
}

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src; u32 i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memset(void *dst, int val, u32 n)
{
    u8 *d = (u8 *)dst; u32 i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}
u32 strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}
static int h_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}
static int h_strncmp(const char *a, const char *b, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(u8)a[i] - (int)(u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static const char *h_strstr(const char *h, const char *n)
{
    u32 ln = strlen(n);
    for (; *h; h++) if (h_strncmp(h, n, ln) == 0) return h;
    return ln == 0 ? h : (const char *)0;
}

static void out_raw(const char *p, u32 len)
{
    while (len > 0) {
        int n;
        __asm__ volatile("int $0x80" : "=a"(n) : "a"(4), "b"(2), "c"(p), "d"(len)
                         : "memory");
        if (n <= 0) die(3);
        p += n;
        len -= (u32)n;
    }
}
static void report(const char *t) { out_raw(t, strlen(t)); }

/* ---- kprintf の捕捉 (必要な書式だけ: %s %d %u %x %c、幅と 0 埋め、l) ---- */
static char cap[131072];
static u32  cap_len;

static void cap_ch(char c)
{
    if (cap_len + 1 < sizeof(cap)) { cap[cap_len++] = c; cap[cap_len] = '\0'; }
}
static void cap_num(unsigned long v, int base, int width, char pad, int neg)
{
    char b[24];
    int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0) { b[i++] = "0123456789abcdef"[v % (unsigned long)base]; v /= (unsigned long)base; }
    if (neg) b[i++] = '-';
    while (i < width) b[i++] = pad;
    while (i > 0) cap_ch(b[--i]);
}
static void cap_vfmt(const char *f, va_list ap)
{
    for (; *f; f++) {
        int width = 0;
        char pad = ' ';
        if (*f != '%') { cap_ch(*f); continue; }
        f++;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'l') f++;
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); while (s && *s) cap_ch(*s++); break; }
        case 'c': cap_ch((char)va_arg(ap, int)); break;
        case 'd': { int v = va_arg(ap, int);
                    cap_num(v < 0 ? (unsigned long)(-(long)v) : (unsigned long)v, 10, width, pad, v < 0);
                    break; }
        case 'u': cap_num(va_arg(ap, unsigned long), 10, width, pad, 0); break;
        case 'x': cap_num(va_arg(ap, unsigned long), 16, width, pad, 0); break;
        case '%': cap_ch('%'); break;
        default:  cap_ch('?'); break;
        }
    }
}
static void __cdecl f_kprintf(u8 attr, const char *fmt, ...)
{
    va_list ap;
    (void)attr;
    va_start(ap, fmt);
    cap_vfmt(fmt, ap);
    va_end(ap);
}

static void fail(const char *what, int line)
{
    char b[16];
    int i = 15, v = line;
    b[i] = '\0';
    while (v > 0) { b[--i] = (char)('0' + v % 10); v /= 10; }
    report("FAIL line ");
    report(&b[i]);
    report(": ");
    report(what);
    report("\n--- output ---\n");
    report(cap);
    report("\n");
    die(1);
}
#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)
#define CHECK_STR(s) do { if (!h_strstr(cap, (s))) fail("message missing: " s, __LINE__); } while (0)
#define CHECK_NOSTR(s) do { if (h_strstr(cap, (s))) fail("unexpected message: " s, __LINE__); } while (0)

/* ======================================================================== */
/*  記憶域 (単純な積み上げ。free は何もしない)                                */
/* ======================================================================== */

static u8  arena[8u * 1024u * 1024u];
static u32 arena_used;
static void *arena_get(u32 n)
{
    void *p;
    n = (n + 7u) & ~7u;
    if (arena_used + n > sizeof(arena)) fail("arena full", __LINE__);
    p = arena + arena_used;
    arena_used += n;
    return p;
}
static void *__cdecl f_mem_alloc(u32 n) { return arena_get(n); }
static void __cdecl f_mem_free(void *p) { (void)p; }

/* ======================================================================== */
/*  ファイル (CD の PKG は中身あり、/hd0 は名前と大きさだけ)                  */
/* ======================================================================== */

#define FX_MAX 256
typedef struct { int used; int is_dir; char path[132]; u8 *data; u32 size; } Fx;
static Fx fx[FX_MAX];

static int fx_find(const char *p)
{
    int i;
    for (i = 0; i < FX_MAX; i++) if (fx[i].used && h_strcmp(fx[i].path, p) == 0) return i;
    return -1;
}
static int fx_new(const char *p)
{
    int i;
    for (i = 0; i < FX_MAX; i++) {
        if (!fx[i].used) {
            memset(&fx[i], 0, sizeof(fx[i]));
            fx[i].used = 1;
            strcpy(fx[i].path, p);
            return i;
        }
    }
    fail("fx table full", __LINE__);
    return -1;
}

#define FD_MAX 8
static struct { int used; int fi; u32 pos; int wr; } fds[FD_MAX];

static const char *inj_open_fail;     /* この /hd0 の名前は作れない */
static int inj_sync_fail, inj_mount_fail, inj_format_fail, inj_readback_bad = -1;
static int hd0_mounts, hd0_at_hd0;
static const char *keys;

static int __cdecl f_sys_open(const char *path, int mode)
{
    int i, fi = fx_find(path);
    if (mode & KAPI_O_CREAT) {
        CHECK(h_strncmp(path, "/hd0/", 5) == 0);       /* CD には書かない */
        if (inj_open_fail && h_strcmp(inj_open_fail, path) == 0) return -5;
        if (fi < 0) fi = fx_new(path);
        if (mode & KAPI_O_TRUNC) fx[fi].size = 0;
    } else if (fi < 0) {
        return -2;
    }
    if (fx[fi].is_dir) return -21;
    for (i = 0; i < FD_MAX; i++) {
        if (!fds[i].used) {
            fds[i].used = 1; fds[i].fi = fi; fds[i].pos = 0;
            fds[i].wr = (mode & (KAPI_O_WRONLY | KAPI_O_RDWR)) ? 1 : 0;
            return i + 3;
        }
    }
    return -24;
}
static void __cdecl f_sys_close(int fd) { if (fd >= 3 && fd < 3 + FD_MAX) fds[fd - 3].used = 0; }
static int __cdecl f_sys_read(int fd, void *buf, u32 n)
{
    Fx *f;
    u32 left;
    if (fd < 3 || fd >= 3 + FD_MAX || !fds[fd - 3].used) return -9;
    f = &fx[fds[fd - 3].fi];
    if (!f->data) return -5;
    left = f->size > fds[fd - 3].pos ? f->size - fds[fd - 3].pos : 0;
    if (n > left) n = left;
    memcpy(buf, f->data + fds[fd - 3].pos, n);
    fds[fd - 3].pos += n;
    return (int)n;
}
static int __cdecl f_sys_write(int fd, const void *buf, u32 n)
{
    (void)buf;
    if (fd < 3 || fd >= 3 + FD_MAX || !fds[fd - 3].used || !fds[fd - 3].wr) return -9;
    fds[fd - 3].pos += n;
    if (fds[fd - 3].pos > fx[fds[fd - 3].fi].size) fx[fds[fd - 3].fi].size = fds[fd - 3].pos;
    return (int)n;
}
static int __cdecl f_sys_lseek(int fd, int off, int whence)
{
    if (fd < 3 || fd >= 3 + FD_MAX || !fds[fd - 3].used) return -9;
    if (whence == SEEK_SET) fds[fd - 3].pos = (u32)off;
    else if (whence == SEEK_END) fds[fd - 3].pos = fx[fds[fd - 3].fi].size + (u32)off;
    else fds[fd - 3].pos += (u32)off;
    return (int)fds[fd - 3].pos;
}
static int __cdecl f_sys_stat(const char *path, OS32_Stat *st)
{
    int fi = fx_find(path);
    if (fi < 0) return -2;
    memset(st, 0, sizeof(*st));
    st->st_size = fx[fi].size;
    st->st_mode = (u16)(fx[fi].is_dir ? OS_S_IFDIR : OS_S_IFREG);
    return 0;
}
static int __cdecl f_sys_mkdir(const char *path)
{
    int fi;
    CHECK(h_strncmp(path, "/hd0", 4) == 0);
    fi = fx_find(path);
    if (fi >= 0) return -6;
    fi = fx_new(path);
    fx[fi].is_dir = 1;
    return 0;
}

/* ======================================================================== */
/*  hd0 (LBA 0〜31 の模型) と手順の記録                                      */
/* ======================================================================== */

#define DISK_SECTS 32
static u8 disk[DISK_SECTS][512];
static HddGeom geom;
static char ev[2048];
static u32 ev_len;
static int writes, fmt_calls;
static u32 fmt_start, fmt_len;

static void ev_add(const char *e)
{
    while (*e && ev_len + 2 < sizeof(ev)) ev[ev_len++] = *e++;
    ev[ev_len++] = ' ';
    ev[ev_len] = '\0';
}
static void ev_lba(u32 lba)
{
    char b[8];
    b[0] = 'W';
    if (lba >= 10) { b[1] = (char)('0' + lba / 10); b[2] = (char)('0' + lba % 10); b[3] = '\0'; }
    else { b[1] = (char)('0' + lba); b[2] = '\0'; }
    ev_add(b);
}

static int __cdecl f_ide_write_sector(int drv, u32 lba, const void *buf)
{
    CHECK(drv == 0);
    CHECK(lba < DISK_SECTS);
    memcpy(disk[lba], buf, 512);
    writes++;
    ev_lba(lba);
    return 0;
}
static int __cdecl f_ide_read_sector(int drv, u32 lba, void *buf)
{
    CHECK(drv == 0);
    CHECK(lba < DISK_SECTS);
    memcpy(buf, disk[lba], 512);
    if (inj_readback_bad >= 0 && (u32)inj_readback_bad == lba && writes > 0)
        ((u8 *)buf)[3] ^= 0x33;
    return 0;
}
static int __cdecl f_hdd_geom_info(int drv, HddGeom *out) { CHECK(drv == 0); *out = geom; return 0; }
static int __cdecl f_ext2_format_at(int drv, u32 start, u32 len)
{
    CHECK(drv == 0);
    fmt_calls++;
    fmt_start = start;
    fmt_len = len;
    ev_add("F");
    return inj_format_fail ? -5 : 0;
}
static int __cdecl f_sys_mount(const char *pre, const char *dev, const char *fs)
{
    (void)fs;
    if (h_strcmp(pre, "/cd0") == 0) return 0;
    CHECK(h_strcmp(pre, "/hd0") == 0 && h_strcmp(dev, "hd0") == 0);
    ev_add("M");
    if (inj_mount_fail) return -5;
    hd0_mounts++;
    hd0_at_hd0 = 1;
    return 0;
}
static int __cdecl f_sys_umount_checked(const char *pre)
{
    ev_add("U");
    if (h_strcmp(pre, "/hd0") == 0 && hd0_at_hd0) { hd0_at_hd0 = 0; hd0_mounts--; return 0; }
    return -2;
}
static int __cdecl f_sys_is_mounted(const char *pre)
{ return (h_strcmp(pre, "/hd0") == 0 && hd0_at_hd0) ? 1 : 0; }
static int __cdecl f_dev_mount_count(int drv) { CHECK(drv == 0); return hd0_mounts; }
static const char *__cdecl f_vfs_devname(const char *pre) { (void)pre; return "fd0"; }
static int __cdecl f_vfs_sync(void) { return inj_sync_fail ? -5 : 0; }
static int __cdecl f_kbd_trygetchar(void)
{
    if (!keys || !*keys) fail("key script exhausted (the installer asked more)", __LINE__);
    return (int)(u8)*keys++;
}
static int __cdecl f_serial_trygetchar(void) { return 0; }

/* ======================================================================== */
/*  実物                                                                     */
/* ======================================================================== */

/* cdinst.c の DBG を空にする (vfs_fd_path_host.c と同じ手) */
#include "rt/dbgserial.h"
#define main cdinst_main
#include "../../userland/system/cdinst.c"
#undef main
#include "../../userland/system/inst_hdd.c"
#include "../../userland/system/inst_disk.c"
#include "../../userland/lib/rt/pkg.c"
#include "../../userland/shell/hdprep_plan.c"
#include "../../drivers/pc98pt.c"
#include "../../fs/ext2_layout.c"

static KernelAPI g_api;

/* ======================================================================== */
/*  PKG を組む (無圧縮)                                                      */
/* ======================================================================== */

typedef struct { const char *path; u32 size; int dir; } PE;

static u8 pat(u32 i, u32 seed) { return (u8)((i * 13u + seed * 7u + 1u) & 0xFF); }

/* nodata = 1 なら表だけ (大きさを名乗るだけで中身を持たない。事前検査は
 * 表しか読まないので、300MB を名乗る項目も置ける) */
static void pkg_put(const char *cdpath, const PE *e, int n, int lzss, int nodata, u32 seed)
{
    u32 tbl = 0, data = 0, off, k;
    u8 *b;
    int i, fi;

    for (i = 0; i < n; i++) {
        tbl += 1u + strlen(e[i].path) + 5u;
        if (!e[i].dir) data += e[i].size;
    }
    tbl += 1u;
    b = (u8 *)arena_get(PKG_HEADER_SIZE + tbl + (nodata ? 0u : data));
    memset(b, 0, PKG_HEADER_SIZE);
    b[0] = 'P'; b[1] = 'K'; b[2] = 'G'; b[3] = '1';
    b[12] = 1;                                   /* version */
    b[13] = (u8)(lzss ? PKG_FLAG_LZSS : 0);
    b[14] = 64;                                  /* kapi_ver */
    b[16] = (u8)n;                               /* entry_count */
    b[18] = (u8)data; b[19] = (u8)(data >> 8); b[20] = (u8)(data >> 16); b[21] = (u8)(data >> 24);
    b[22] = b[18]; b[23] = b[19]; b[24] = b[20]; b[25] = b[21];
    off = PKG_HEADER_SIZE;
    for (i = 0; i < n; i++) {
        u32 pl = strlen(e[i].path);
        b[off++] = (u8)pl;
        memcpy(b + off, e[i].path, pl);
        off += pl;
        b[off++] = (u8)e[i].size; b[off++] = (u8)(e[i].size >> 8);
        b[off++] = (u8)(e[i].size >> 16); b[off++] = (u8)(e[i].size >> 24);
        b[off++] = (u8)(e[i].dir ? PKG_TYPE_DIR : PKG_TYPE_FILE);
    }
    b[off++] = 0;
    if (!nodata) {
        for (i = 0; i < n; i++) {
            if (e[i].dir) continue;
            for (k = 0; k < e[i].size; k++) b[off + k] = pat(k, seed + (u32)i);
            off += e[i].size;
        }
    }
    fi = fx_find(cdpath);
    if (fi < 0) fi = fx_new(cdpath);
    fx[fi].data = b;
    fx[fi].size = off;
}

#define KERNEL_LEN 470000u
#define LOADER_LEN 8192u
#define IPL_LEN    512u

static u32 boot_ipl_len, boot_loader_len;

static void boot_pkg(u32 ipl_len, u32 loader_len, int with_ipl, int lzss)
{
    PE e[2];
    int n = 0;
    if (with_ipl) { e[n].path = "/boot/boot_hdd.bin"; e[n].size = ipl_len; e[n].dir = 0; n++; }
    e[n].path = "/boot/loader_hdd.bin"; e[n].size = loader_len; e[n].dir = 0; n++;
    pkg_put("/cd0/BOOT.PKG", e, n, lzss, 0, 100u);
    boot_ipl_len = ipl_len;
    boot_loader_len = loader_len;
}

static void minimal_pkg(u32 kernel_len, int with_kernel, int with_shell, int nodata)
{
    PE e[6];
    int n = 0;
    e[n].path = "/boot"; e[n].size = 0; e[n].dir = 1; n++;
    e[n].path = "/sys"; e[n].size = 0; e[n].dir = 1; n++;
    if (with_kernel) { e[n].path = "/boot/vmkernel.lz4"; e[n].size = kernel_len; e[n].dir = 0; n++; }
    if (with_shell) { e[n].path = "/sys/shell.bin"; e[n].size = 1000; e[n].dir = 0; n++; }
    e[n].path = "/bin/ls.bin"; e[n].size = 300; e[n].dir = 0; n++;
    pkg_put("/cd0/MINIMAL.PKG", e, n, 0, nodata, 200u);
}

static void small_pkg(const char *cd, const char *file, u32 size, int nodata)
{
    PE e[1];
    e[0].path = file; e[0].size = size; e[0].dir = 0;
    pkg_put(cd, e, 1, 0, nodata, 300u);
}

static void geom_817(void)
{
    memset(&geom, 0, sizeof(geom));
    geom.bios_queried = 1; geom.bios_valid = 1; geom.bios_heads = 8; geom.bios_spt = 17;
    geom.bios_cyl = 3011; geom.bios_seclen = 512;
    /* ドライブ自身は 16/63 を申告 (BIOS の 8/17 変換と違う)。区画表と IPL は
     * BIOS 幾何で書かれなければならない (F15) */
    geom.ata_def_cyl = 16383; geom.ata_def_heads = 16; geom.ata_def_spt = 63;
    geom.ata_cur_cyl = 16383; geom.ata_cur_heads = 16; geom.ata_cur_spt = 63;
    geom.ata_w49 = 0x0200; geom.ata_w53 = 1; geom.ata_total = 409600;
    geom.ata_present = 1; geom.addr_mode = HDD_AMODE_LBA28; geom.bios_da = 0x80;
}
static void geom_1663(void)
{
    geom_817();
    geom.bios_heads = 16; geom.bios_spt = 63; geom.bios_cyl = 16382;
    geom.ata_def_cyl = 16382; geom.ata_def_heads = 16; geom.ata_def_spt = 63;
    geom.ata_cur_cyl = 16382; geom.ata_cur_heads = 16; geom.ata_cur_spt = 63;
    geom.ata_total = 16514063;
}

static void setup(void)
{
    int i;
    for (i = 0; i < FX_MAX; i++) fx[i].used = 0;
    for (i = 0; i < FD_MAX; i++) fds[i].used = 0;
    arena_used = 0;
    cap_len = 0; cap[0] = '\0';
    ev_len = 0; ev[0] = '\0';
    memset(disk, 0, sizeof(disk));
    writes = fmt_calls = 0;
    fmt_start = fmt_len = 0;
    inj_open_fail = 0;
    inj_sync_fail = inj_mount_fail = inj_format_fail = 0;
    inj_readback_bad = -1;
    hd0_mounts = hd0_at_hd0 = 0;
    keys = "1y";
    geom_817();
    boot_pkg(IPL_LEN, LOADER_LEN, 1, 0);
    minimal_pkg(KERNEL_LEN, 1, 1, 0);
    small_pkg("/cd0/GUI.PKG", "/bin/gshell.bin", 2000, 0);
    small_pkg("/cd0/NORMAL.PKG", "/usr/bin/edit.bin", 3000, 0);
    small_pkg("/cd0/DEBUG.PKG", "/usr/bin/t.bin", 100, 0);

    memset(&g_api, 0, sizeof(g_api));
    g_api.kprintf = f_kprintf;
    g_api.mem_alloc = f_mem_alloc;
    g_api.mem_free = f_mem_free;
    g_api.sys_open = f_sys_open;
    g_api.sys_close = f_sys_close;
    g_api.sys_read = f_sys_read;
    g_api.sys_write = f_sys_write;
    g_api.sys_lseek = f_sys_lseek;
    g_api.sys_stat = f_sys_stat;
    g_api.sys_mkdir = f_sys_mkdir;
    g_api.sys_mount = f_sys_mount;
    g_api.sys_umount_checked = f_sys_umount_checked;
    g_api.sys_is_mounted = f_sys_is_mounted;
    g_api.dev_mount_count = f_dev_mount_count;
    g_api.vfs_devname = f_vfs_devname;
    g_api.vfs_sync = f_vfs_sync;
    g_api.ide_read_sector = f_ide_read_sector;
    g_api.ide_write_sector = f_ide_write_sector;
    g_api.hdd_geom_info = f_hdd_geom_info;
    g_api.ext2_format_at = f_ext2_format_at;
    g_api.kbd_trygetchar = f_kbd_trygetchar;
    g_api.serial_trygetchar = f_serial_trygetchar;
}

static void run(void) { cdinst_main(1, (char **)0, &g_api); }

#define CHECK_NOTHING_WRITTEN() do { \
    CHECK(writes == 0); CHECK(fmt_calls == 0); CHECK(ev_len == 0); \
    CHECK_STR("Nothing was written"); CHECK_NOSTR("Installation Complete"); } while (0)

/* 区画表 (標準配置、項目 1 つ)・IPL の幾何・中身・ローダの中身・format の範囲 */
static void check_disk(u32 start, u32 len, u32 heads, u32 spt, u32 total)
{
    unsigned long st = 0, ln = 0;
    int idx = -1;
    u32 k;
    CHECK(pc98pt_count_used(disk[1]) == 1);
    CHECK(pc98pt_find_os32(disk[1], heads, spt, total, &idx, &st, &ln) == PC98PT_OK);
    CHECK(idx == 0 && st == start && ln == len);
    CHECK(disk[0][8] == heads && disk[0][9] == spt);
    CHECK(disk[0][510] == 0x55 && disk[0][511] == 0xAA);
    /* IPL = BOOT.PKG の 1 本目 (seed 100) の中身、[8]/[9] と 55AA 以外 */
    for (k = 0; k < 510; k++) {
        if (k == 8 || k == 9) continue;
        CHECK(disk[0][k] == (k < boot_ipl_len ? pat(k, 100u) : 0));
    }
    /* ローダ = 2 本目 (seed 101)、端数は 0 */
    for (k = 0; k < ((boot_loader_len + 511u) / 512u) * 512u; k++)
        CHECK(disk[2 + k / 512u][k % 512u] == (k < boot_loader_len ? pat(k, 101u) : 0));
    CHECK(fmt_calls == 1 && fmt_start == start && fmt_len == len);
}

static void check_order(const char *prefix)
{
    char want[256];
    u32 k, n = 0;
    const char *p;
    for (p = prefix; *p; p++) want[n++] = *p;
    for (p = "F W1 M "; *p; p++) want[n++] = *p;
    for (k = 2; k < 2u + (boot_loader_len + 511u) / 512u; k++) {
        want[n++] = 'W';
        if (k >= 10) want[n++] = (char)('0' + k / 10);
        want[n++] = (char)('0' + k % 10);
        want[n++] = ' ';
    }
    want[n++] = 'W'; want[n++] = '0'; want[n++] = ' '; want[n] = '\0';
    if (h_strcmp(ev, want) != 0) {
        report("order got: "); report(ev); report("\nwant:      "); report(want); report("\n");
        fail("order", __LINE__);
    }
}

static int hd0_size(const char *p)
{
    int fi = fx_find(p);
    return fi < 0 ? -1 : (int)fx[fi].size;
}

/* ---- 8/17 の NHD、Minimal ---- */
static void case_ok817(void)
{
    setup();
    run();
    check_disk(1632u, 407864u, 8, 17, 409600u);
    check_order("");
    CHECK(hd0_size("/hd0/boot/vmkernel.lz4") == (int)KERNEL_LEN);
    CHECK(hd0_size("/hd0/sys/shell.bin") == 1000);
    CHECK(hd0_size("/hd0/bin/gshell.bin") < 0);            /* Minimal は GUI を入れない */
    CHECK_STR("empty disk");
    CHECK_STR("Installation Complete");
    CHECK_NOSTR("INCOMPLETE");
}

/* ---- 16/63 (実機)、Full、ローダは 1 セクタの端数あり ---- */
static void case_ok1663(void)
{
    setup();
    geom_1663();
    boot_pkg(IPL_LEN - 12u, 5000u, 1, 0);
    keys = "3y";
    run();
    check_disk(2016u, 524160u, 16, 63, 16514063u);
    check_order("");
    CHECK(hd0_size("/hd0/bin/gshell.bin") == 2000);
    CHECK(hd0_size("/hd0/usr/bin/t.bin") == 100);
    CHECK_STR("Installation Complete");
}

/* ---- モード: 旧配置 8/17 は作り直す (マウント中なら外してから)、他は断る ---- */
static void case_modes(void)
{
    setup();
    disk[1][0] = 0x80; disk[1][1] = 0xE2; disk[1][8] = 12;      /* 旧 cdinst の表 */
    disk[1][10] = 16; disk[1][11] = 7;
    disk[1][12] = (u8)((409600u / 136u - 1u) & 0xFF); disk[1][13] = (u8)((409600u / 136u - 1u) >> 8);
    memcpy(disk[1] + 16, "OS32            ", 16);
    disk[0][510] = 0x55; disk[0][511] = 0xAA;
    hd0_mounts = 1; hd0_at_hd0 = 1;
    run();
    CHECK_STR("old table layout");
    CHECK_STR("ALL FILES IN IT WILL BE LOST");
    check_disk(1632u, 407864u, 8, 17, 409600u);
    check_order("U ");
    CHECK_STR("Installation Complete");

    /* 未知の区画 */
    setup();
    disk[1][0] = 0x80; disk[1][1] = 0xA1; disk[1][10] = 12; disk[1][14] = 100;
    memcpy(disk[1] + 16, "MS-DOS          ", 16);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("OS32 did not create");

    /* 2 項目 */
    setup();
    disk[1][1] = 0xE2; disk[1][32 + 1] = 0xE2;
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("two or more");
}

/* ---- 事前検査の各失敗: 1 セクタも書かない ---- */
static void case_preflight(void)
{
    setup();                                        /* ローダ 8193 */
    boot_pkg(IPL_LEN, 8193u, 1, 0);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than 8192");

    setup();                                        /* IPL 513 */
    boot_pkg(513u, LOADER_LEN, 1, 0);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than 512");

    setup();                                        /* IPL が無い */
    boot_pkg(IPL_LEN, LOADER_LEN, 0, 0);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("BOOT.PKG lacks boot_hdd.bin");

    setup();                                        /* BOOT.PKG が LZSS */
    boot_pkg(IPL_LEN, LOADER_LEN, 1, 1);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("must not be LZSS");

    setup();                                        /* vmkernel 508KiB ちょうどは通る */
    minimal_pkg(508u * 1024u, 1, 1, 0);
    run();
    CHECK_STR("Installation Complete");
    CHECK(hd0_size("/hd0/boot/vmkernel.lz4") == 508 * 1024);

    setup();                                        /* vmkernel 508KiB + 1 */
    minimal_pkg(508u * 1024u + 1u, 1, 1, 1);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("larger than 508 KiB");

    setup();                                        /* vmkernel が無い */
    minimal_pkg(KERNEL_LEN, 0, 1, 0);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("MINIMAL.PKG lacks /boot/vmkernel.lz4");

    setup();                                        /* shell が無い */
    minimal_pkg(KERNEL_LEN, 1, 0, 0);
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("MINIMAL.PKG lacks /sys/shell.bin");

    setup();                                        /* 容量: NORMAL が 300MB を名乗る */
    small_pkg("/cd0/NORMAL.PKG", "/usr/share/big.dat", 300u * 1024u * 1024u, 1);
    keys = "2y";
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("do not fit");

    setup();                                        /* Minimal なら NORMAL は数えない */
    small_pkg("/cd0/NORMAL.PKG", "/usr/share/big.dat", 300u * 1024u * 1024u, 1);
    run();
    CHECK_STR("Installation Complete");

    setup();                                        /* 前置で溢れる項目 (展開の途中ではなく書く前に) */
    {
        static char longp[125];
        u32 k;
        longp[0] = '/';
        for (k = 1; k < 124u; k++) longp[k] = 'a';
        longp[124] = '\0';
        small_pkg("/cd0/NORMAL.PKG", longp, 10, 0);
    }
    keys = "2y";
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("PATH TOO LONG");

    setup();                                        /* 16/63 の幾何が無い */
    geom.bios_valid = 0;
    run();
    CHECK_NOTHING_WRITTEN();
    CHECK_STR("no BIOS geometry");

    setup();                                        /* 断れば何も書かない */
    keys = "1n";
    run();
    CHECK_NOTHING_WRITTEN();
}

/* ---- 書いた後の失敗: INCOMPLETE、完了とは言わない ---- */
static void case_incomplete(void)
{
    setup();
    inj_format_fail = 1;
    run();
    CHECK(writes == 0);
    CHECK_STR("INCOMPLETE");
    CHECK_NOSTR("Installation Complete");

    setup();
    inj_readback_bad = 1;
    run();
    CHECK(writes == 1);
    CHECK(!h_strstr(ev, "M"));
    CHECK_STR("INCOMPLETE: partition table");
    CHECK_NOSTR("Installation Complete");

    setup();
    inj_mount_fail = 1;
    run();
    CHECK(writes == 1 && disk[0][510] == 0);
    CHECK_STR("INCOMPLETE: mount /hd0");
    CHECK_NOSTR("Installation Complete");

    setup();
    inj_readback_bad = 0;
    run();
    CHECK(hd0_size("/hd0/boot/vmkernel.lz4") < 0);
    CHECK_STR("INCOMPLETE: IPL");
    CHECK_NOSTR("Installation Complete");

    setup();                                        /* 追加パッケージ (NORMAL) の展開の失敗 */
    inj_open_fail = "/hd0/usr/bin/edit.bin";
    keys = "2y";
    run();
    CHECK_STR("INCOMPLETE: package installation failed");
    CHECK_NOSTR("Installation Complete");

    setup();                                        /* sync の失敗 */
    inj_sync_fail = 1;
    run();
    CHECK_STR("INCOMPLETE");
    CHECK_NOSTR("Installation Complete");
}

int os32_main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (h_strcmp(c, "ok817") == 0) case_ok817();
    else if (h_strcmp(c, "ok1663") == 0) case_ok1663();
    else if (h_strcmp(c, "modes") == 0) case_modes();
    else if (h_strcmp(c, "preflight") == 0) case_preflight();
    else if (h_strcmp(c, "incomplete") == 0) case_incomplete();
    else { report("unknown case\n"); return 2; }
    report("cdinst: PASS (");
    report(c);
    report(")\n");
    return 0;
}

void start_c(long *sp);

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  movl %esp, %eax\n"
        "  andl $-16, %esp\n"
        "  pushl %eax\n"
        "  call start_c\n"
        "  hlt\n");

void start_c(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)&sp[1];
    die(os32_main(argc, argv));
}
