/* ========================================================================
 *  ext2_write_io_host.c — ext2 の 1 回の書き込みが起こすセクタ I/O を数える
 *
 *  対象票: S6-P (ext2 の小さな書き込みが極端に遅い)
 *  実行:   python3 -B tools/tests/test_ext2_write_io.py
 *  記録:   tools/tests/s6p_tdd.md
 *
 *  実物の fs/ext2_super.c / ext2_inode.c / ext2_dir.c / ext2_file.c /
 *  ext2_fmt.c / ext2_vfs.c をそのまま取り込み、境界は
 *    - Device API (dev_find / dev_blk_read_lba / dev_blk_write_lba)
 *    - ide_drive_present / ide_get_info
 *    - kstring / kmalloc / kprintf / vfs_register_fs
 *  だけを贋物に差し替える。RAM 上の 8MB ディスクを実物の ext2_format() で
 *  作るので、レイアウトも実機と同じ経路で決まる。実デバイス・実イメージ・
 *  エミュレータには一切触れない。
 *
 *  数えるのは **512B セクタ単位の I/O 回数** (= ide_read_sector_chs /
 *  ide_write_sector_chs の呼び出し回数)。ext2 の 1KB ブロック 1 本は必ず
 *  セクタ 2 本になる (fs/ext2_super.c の ext2_read_block / ext2_write_block)。
 *
 *  tools/tests/con_sink_host.c と同じ様式 — ホスト ILP32 GNU89 で走らせ、
 *  同じソースが i386-elf-gcc -Werror でも通ることを別に見る ([C1])。
 *  libc は使わない (-nostdlib、Linux の int 0x80 で write/exit するだけ)。
 *  u32 は `unsigned long` なので、ホストも必ず ILP32 で組む。
 * ======================================================================== */

#include "ext2_priv.h"
#include "ide.h"
#include "kmalloc.h"

/* ======================================================================== */
/*  libc の代わり (-nostdlib)                                               */
/* ======================================================================== */

static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) { }
}

static u32 h_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void report(const char *text)
{
    u32 len = h_strlen(text);
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(text), "d"(len)
                     : "memory");
}

static void report_u(u32 v)
{
    char buf[16];
    int i = 15;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    report(&buf[i]);
}

static int g_failures;

static void fail(const char *what, int line)
{
    report("FAIL line ");
    report_u((u32)line);
    report(": ");
    report(what);
    report("\n");
    g_failures++;
    die(1);
}

#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)

/* ======================================================================== */
/*  kstring / kprintf / kmalloc の境界                                      */
/* ======================================================================== */

void *kmemcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src; u32 i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *kmemset(void *dst, int val, u32 n)
{
    u8 *d = (u8 *)dst; u32 i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

u32 kstrlen(const char *s) { return h_strlen(s); }

int kstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int kstrncmp(const char *a, const char *b, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(u8)a[i] - (int)(u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *kstrncpy(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    if (n == 0) return dst;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

/* gcc が構造体コピー等で呼ぶことがある */
void *memcpy(void *dst, const void *src, u32 n) { return kmemcpy(dst, src, n); }
void *memset(void *dst, int val, u32 n) { return kmemset(dst, val, n); }

void kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }

/* ---- kzalloc / kfree: 固定スロットの贋物 (Ext2Ctx 専用) ---- */
#define HEAP_SLOTS  8
static struct { int in_use; Ext2Ctx ctx; } g_slots[HEAP_SLOTS];

void *kzalloc(u32 size)
{
    int i;
    if (size > sizeof(Ext2Ctx)) return (void *)0;
    for (i = 0; i < HEAP_SLOTS; i++) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use = 1;
            kmemset(&g_slots[i].ctx, 0, sizeof(Ext2Ctx));
            return (void *)&g_slots[i].ctx;
        }
    }
    return (void *)0;
}

void kfree(void *p)
{
    int i;
    for (i = 0; i < HEAP_SLOTS; i++) {
        if (p == (void *)&g_slots[i].ctx) { g_slots[i].in_use = 0; return; }
    }
}

/* ---- VFS 登録の境界 (ext2_init から呼ばれるだけ) ---- */
void vfs_register_fs(VfsOps *ops) { (void)ops; }

/* ======================================================================== */
/*  RAM ディスク + セクタ I/O 計数                                          */
/* ======================================================================== */

/* 8MB の ext2。base_lba はパーティションテーブルが空なので
 * ext2_find_partition() のフォールバック (1088) になる。 */
#define DISK_FS_SECTORS   16384u
#define DISK_BASE_LBA     1088u
#define DISK_SECTORS      (DISK_BASE_LBA + DISK_FS_SECTORS)

static u8 g_disk[DISK_SECTORS * 512u];
static u32 g_rd_sect;     /* 512B セクタ読み出し回数 */
static u32 g_wr_sect;     /* 512B セクタ書き込み回数 */

static void io_reset(void) { g_rd_sect = 0; g_wr_sect = 0; }
static u32 io_total(void) { return g_rd_sect + g_wr_sect; }

static Device g_hd0;

Device *dev_find(const char *name)
{
    if (name && name[0] == 'h' && name[1] == 'd' && name[2] == '0' && !name[3])
        return &g_hd0;
    return (Device *)0;
}

int dev_blk_read_lba(Device *dev, u32 lba, int count, void *buf)
{
    int i;
    if (!dev) return -1;
    for (i = 0; i < count; i++) {
        if (lba + (u32)i >= DISK_SECTORS) return -1;
        kmemcpy((u8 *)buf + i * 512, g_disk + (lba + (u32)i) * 512u, 512);
        g_rd_sect++;
    }
    return 0;
}

int dev_blk_write_lba(Device *dev, u32 lba, int count, const void *buf)
{
    int i;
    if (!dev) return -1;
    for (i = 0; i < count; i++) {
        if (lba + (u32)i >= DISK_SECTORS) return -1;
        kmemcpy(g_disk + (lba + (u32)i) * 512u, (const u8 *)buf + i * 512, 512);
        g_wr_sect++;
    }
    return 0;
}

/* ---- IDE の境界 (ext2_super.c / ext2_fmt.c が存在確認とジオメトリに使う) */
int ide_drive_present(int drive) { return (drive & 3) == 0; }

int ide_get_info(int drive, IdeInfo *info)
{
    if ((drive & 3) != 0) return IDE_ERR_NO_DRIVE;
    if (info) {
        kmemset(info, 0, sizeof(*info));
        info->cylinders = 1024;
        info->heads = 8;
        info->sectors = 17;
        info->phys_sector_size = 512;
        info->total_sectors = DISK_SECTORS;
    }
    return IDE_OK;
}

/* ======================================================================== */
/*  実物のソース                                                            */
/* ======================================================================== */
#include "../../fs/ext2_super.c"
#include "../../fs/ext2_inode.c"
#include "../../fs/ext2_dir.c"
#include "../../fs/ext2_file.c"
#include "../../fs/ext2_fmt.c"
#include "../../fs/ext2_vfs.c"

/* ======================================================================== */
/*  足場                                                                    */
/* ======================================================================== */

static Ext2Ctx *g_ec;
static const char *ARCHIVE = "/tmp/e8.tar";
static const char CFG_TEXT[16] = "gui.autostart\n";   /* 14B + NUL + 1 = 15B */

static void line(const char *tag, u32 rd, u32 wr)
{
    report("  ");
    report(tag);
    report(": rd=");   report_u(rd);
    report(" wr=");    report_u(wr);
    report(" total="); report_u(rd + wr);
    report(" sectors\n");
}

static void disk_setup(void)
{
    kmemset(g_disk, 0, sizeof(g_disk));
    kmemset(&g_hd0, 0, sizeof(g_hd0));
    g_hd0.name = "hd0";
    g_hd0.type = DEV_BLOCK;
    g_hd0.bus_type = DEV_BUS_IDE;
    g_hd0.sect_size = 512;
    g_hd0.total_sects = DISK_SECTORS;
    g_hd0.heads = 8;
    g_hd0.spt = 17;

    CHECK(ext2_format(0, DISK_FS_SECTORS) == EXT2_OK);

    g_ec = (Ext2Ctx *)ext2_vfs_mount(VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
    CHECK(g_ec != (Ext2Ctx *)0);
    CHECK(g_ec->base_lba == DISK_BASE_LBA);
}

static void disk_teardown(void)
{
    if (g_ec) { ext2_vfs_umount(g_ec); g_ec = (Ext2Ctx *)0; }
}

/* tar と同じ道具立て: /tmp と 15B の /etc/system.cfg を用意する */
static void tree_setup(void)
{
    CHECK(ext2_vfs_mkdir(g_ec, "/tmp") == VFS_OK);
    CHECK(ext2_vfs_mkdir(g_ec, "/etc") == VFS_OK);
    CHECK(ext2_vfs_write(g_ec, "/etc/system.cfg", CFG_TEXT, 15) == VFS_OK);
}

/* vfs_open(O_CREAT|O_TRUNC) と同じ「0 バイトで作る」 */
static void create_empty(const char *path)
{
    CHECK(ext2_vfs_write(g_ec, path, "", 0) == VFS_OK);
}

static int wr(const char *path, const void *buf, u32 size, u32 off)
{
    return ext2_vfs_write_stream(g_ec, path, buf, size, off);
}

static u8 g_blk512[512];
static void blk_fill(u8 v) { kmemset(g_blk512, v, sizeof(g_blk512)); }

/* ======================================================================== */
/*  ケース                                                                  */
/* ======================================================================== */

/* P1: 新しいブロックを起こす 512B 書き込み 1 回 */
static void case_w512_new(void)
{
    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');

    io_reset();
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    line("P1 write(512B @0, new block)", g_rd_sect, g_wr_sect);
    disk_teardown();
}

/* P2: 既存ブロックの中への 512B 書き込み 1 回 */
static void case_w512_existing(void)
{
    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    CHECK(wr(ARCHIVE, g_blk512, 512, 512) == 512);

    io_reset();
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    line("P2 write(512B @0, existing) ", g_rd_sect, g_wr_sect);
    disk_teardown();
}

/* P3: 1 バイト追記 1 回 (microtar の write_null_bytes が出す形) */
static void case_w1_append(void)
{
    u8 nul = 0;

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    CHECK(wr(ARCHIVE, CFG_TEXT, 15, 512) == 15);

    io_reset();
    CHECK(wr(ARCHIVE, &nul, 1, 527) == 1);
    line("P3 write(1B @527, existing) ", g_rd_sect, g_wr_sect);
    disk_teardown();
}

/* P4: 後方 lseek は FS を触らない (fs/vfs_fd.c の vfs_seek は
 *     VfsFile.offset を書き換えるだけ) + 戻った先への 512B 書き直し */
static void case_seek_back(void)
{
    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    CHECK(wr(ARCHIVE, g_blk512, 512, 512) == 512);

    io_reset();
    line("P4 lseek(back)              ", g_rd_sect, g_wr_sect);
    CHECK(io_total() == 0);

    io_reset();
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    line("P4 rewrite header(512B @0)  ", g_rd_sect, g_wr_sect);
    disk_teardown();
}

/* P5: `tar c /tmp/e8.tar /etc/system.cfg` の呼び出し列をそのまま再生する。
 *
 *   lib/microtar/microtar.c:
 *     mtar_write_file_header -> twrite(512)           … 1 回
 *     mtar_write_data(15)    -> twrite(15)            … 1 回
 *                            -> write_null_bytes(497) … **1 バイトずつ 497 回**
 *     mtar_finalize          -> write_null_bytes(1024)… **1 バイトずつ 1024 回**
 *   合計 1523 回の sys_write。 */
#define TAR_HDR   512u
#define TAR_DATA  15u
#define TAR_PAD1  497u                 /* round_up(527,512) - 527 */
#define TAR_PAD2  1024u                /* 終端の 2 レコード */
#define TAR_SIZE  2048u

static void tar_header(u8 *out)
{
    kmemset(out, 0, TAR_HDR);
    kmemcpy(out, "etc/system.cfg", 14);
}

static u32 tar_replay_bytewise(void)
{
    static u8 hdr[TAR_HDR];
    u8 nul = 0;
    u32 off = 0;
    u32 i;

    tar_header(hdr);
    io_reset();
    CHECK(wr(ARCHIVE, hdr, TAR_HDR, off) == (int)TAR_HDR); off += TAR_HDR;
    CHECK(wr(ARCHIVE, CFG_TEXT, TAR_DATA, off) == (int)TAR_DATA); off += TAR_DATA;
    for (i = 0; i < TAR_PAD1; i++) { CHECK(wr(ARCHIVE, &nul, 1, off) == 1); off++; }
    for (i = 0; i < TAR_PAD2; i++) { CHECK(wr(ARCHIVE, &nul, 1, off) == 1); off++; }
    CHECK(off == TAR_SIZE);
    return io_total();
}

static u32 tar_replay_blockwise(void)
{
    static u8 blk[512];
    u32 off = 0;

    io_reset();
    tar_header(blk);
    CHECK(wr(ARCHIVE, blk, 512, off) == 512); off += 512;
    kmemset(blk, 0, sizeof(blk));
    kmemcpy(blk, CFG_TEXT, TAR_DATA);
    CHECK(wr(ARCHIVE, blk, 512, off) == 512); off += 512;
    kmemset(blk, 0, sizeof(blk));
    CHECK(wr(ARCHIVE, blk, 512, off) == 512); off += 512;
    CHECK(wr(ARCHIVE, blk, 512, off) == 512); off += 512;
    CHECK(off == TAR_SIZE);
    return io_total();
}

static void archive_verify(void)
{
    static u8 got[TAR_SIZE];
    u32 i;

    CHECK(ext2_vfs_read(g_ec, ARCHIVE, got, sizeof(got)) == (int)TAR_SIZE);
    CHECK(kstrncmp((const char *)got, "etc/system.cfg", 14) == 0);
    CHECK(kstrncmp((const char *)got + 512, CFG_TEXT, 14) == 0);
    for (i = 527; i < TAR_SIZE; i++) CHECK(got[i] == 0);
}

static void case_tar_sequence(void)
{
    u32 bytewise, blockwise;

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    bytewise = tar_replay_bytewise();
    archive_verify();
    disk_teardown();

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blockwise = tar_replay_blockwise();
    archive_verify();
    disk_teardown();

    report("  P5 tar c via microtar (1523 writes): total=");
    report_u(bytewise); report(" sectors\n");
    report("  P5 tar c via 4 x 512B writes      : total=");
    report_u(blockwise); report(" sectors\n");
    report("  P5 caller-side multiplier         : x");
    report_u(bytewise / blockwise); report("\n");
}

/* P6: 読み側 — 512B 読み 1 回 */
static void case_read512(void)
{
    static u8 buf[512];

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    CHECK(wr(ARCHIVE, g_blk512, 512, 512) == 512);

    io_reset();
    CHECK(ext2_vfs_read_stream(g_ec, ARCHIVE, buf, 512, 512) == 512);
    line("P6 read(512B @512)          ", g_rd_sect, g_wr_sect);
    disk_teardown();
}

/* P7: 正しさの回帰 — 書き込みが戻った時点でディスクに反映されている
 *     (write-through の契約)。別インスタンスでマウントし直して確かめる。 */
static void case_write_through(void)
{
    static u8 src[1500];
    static u8 got[4096];
    u32 i, sz = 0;

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    for (i = 0; i < sizeof(src); i++) src[i] = (u8)(i * 31u + 7u);

    CHECK(wr(ARCHIVE, src, 1500, 0) == 1500);

    {
        Ext2Ctx *fresh = (Ext2Ctx *)ext2_vfs_mount(
            VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
        CHECK(fresh != (Ext2Ctx *)0);
        CHECK(ext2_vfs_get_size(fresh, ARCHIVE, &sz) == VFS_OK);
        CHECK(sz == 1500);
        CHECK(ext2_vfs_read(fresh, ARCHIVE, got, sizeof(got)) == 1500);
        for (i = 0; i < 1500; i++) CHECK(got[i] == src[i]);
        /* 空きブロック数もディスク側と一致する (sync が落ちていない) */
        CHECK(fresh->sb_info.free_blocks_count == g_ec->sb_info.free_blocks_count);
        CHECK(fresh->sb_info.free_inodes_count == g_ec->sb_info.free_inodes_count);
        CHECK(fresh->gd_table[0].free_blocks == g_ec->gd_table[0].free_blocks);
        ext2_vfs_umount(fresh);
    }

    /* 追記・途中上書きの後も同じ */
    CHECK(wr(ARCHIVE, src, 600, 1500) == 600);
    CHECK(wr(ARCHIVE, src + 3, 40, 10) == 40);
    {
        Ext2Ctx *fresh = (Ext2Ctx *)ext2_vfs_mount(
            VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
        CHECK(fresh != (Ext2Ctx *)0);
        CHECK(ext2_vfs_read(fresh, ARCHIVE, got, sizeof(got)) == 2100);
        for (i = 0; i < 40; i++) CHECK(got[10 + i] == src[3 + i]);
        for (i = 50; i < 1500; i++) CHECK(got[i] == src[i]);
        for (i = 0; i < 600; i++) CHECK(got[1500 + i] == src[i]);
        CHECK(fresh->sb_info.free_blocks_count == g_ec->sb_info.free_blocks_count);
        ext2_vfs_umount(fresh);
    }
    report("  P7 write-through contract kept\n");
    disk_teardown();
}

/* P8: 名前空間が変わったら経路の記憶は必ず捨てられる (キャッシュ導入の番犬)。
 *     同じパスが別の inode を指すようになる並びを踏む。 */
static void case_ns_invalidation(void)
{
    static u8 got[64];
    u32 sz = 0;

    disk_setup(); tree_setup();

    CHECK(ext2_vfs_write(g_ec, "/tmp/a", "AAAA", 4) == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/a", got, sizeof(got)) == 4);
    CHECK(kstrncmp((const char *)got, "AAAA", 4) == 0);

    /* 消して作り直す → inode が変わっても読めるのは新しい方 */
    CHECK(ext2_vfs_unlink(g_ec, "/tmp/a") == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/a", got, sizeof(got)) == VFS_ERR_NOTFOUND);
    CHECK(ext2_vfs_write(g_ec, "/tmp/a", "BBBBBB", 6) == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/a", got, sizeof(got)) == 6);
    CHECK(kstrncmp((const char *)got, "BBBBBB", 6) == 0);

    /* rename で同じ名前が別の中身を指す */
    CHECK(ext2_vfs_write(g_ec, "/tmp/b", "CCCCCCCC", 8) == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/b", got, sizeof(got)) == 8);
    CHECK(ext2_vfs_unlink(g_ec, "/tmp/a") == VFS_OK);
    CHECK(ext2_vfs_rename(g_ec, "/tmp/b", "/tmp/a") == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/b", got, sizeof(got)) == VFS_ERR_NOTFOUND);
    CHECK(ext2_vfs_read(g_ec, "/tmp/a", got, sizeof(got)) == 8);
    CHECK(kstrncmp((const char *)got, "CCCCCCCC", 8) == 0);
    CHECK(ext2_vfs_get_size(g_ec, "/tmp/a", &sz) == VFS_OK);
    CHECK(sz == 8);

    /* ディレクトリを消して同名のファイルにする */
    CHECK(ext2_vfs_mkdir(g_ec, "/tmp/d") == VFS_OK);
    CHECK(ext2_vfs_get_size(g_ec, "/tmp/d", &sz) == VFS_OK);
    CHECK(ext2_vfs_rmdir(g_ec, "/tmp/d") == VFS_OK);
    CHECK(ext2_vfs_write(g_ec, "/tmp/d", "DD", 2) == VFS_OK);
    CHECK(ext2_vfs_read(g_ec, "/tmp/d", got, sizeof(got)) == 2);
    CHECK(kstrncmp((const char *)got, "DD", 2) == 0);

    /* 2 インスタンスが同時にマウントされていても取り違えない */
    {
        Ext2Ctx *other = (Ext2Ctx *)ext2_vfs_mount(
            VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
        CHECK(other != (Ext2Ctx *)0);
        CHECK(ext2_vfs_read(other, "/tmp/d", got, sizeof(got)) == 2);
        CHECK(ext2_vfs_read(g_ec, "/tmp/a", got, sizeof(got)) == 8);
        CHECK(ext2_vfs_read(other, "/tmp/a", got, sizeof(got)) == 8);
        ext2_vfs_umount(other);
    }

    report("  P8 path cache follows the namespace\n");
    disk_teardown();
}

/* P9: 空き数を動かす書き込み (新ブロック確保) では sync が必ず出る */
static void case_sync_on_alloc(void)
{
    u32 alloc_io, plain_io;

    disk_setup(); tree_setup();
    create_empty(ARCHIVE);
    blk_fill('A');

    io_reset();
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    alloc_io = io_total();

    io_reset();
    CHECK(wr(ARCHIVE, g_blk512, 512, 0) == 512);
    plain_io = io_total();

    report("  P9 alloc="); report_u(alloc_io);
    report(" vs no-alloc="); report_u(plain_io); report(" sectors\n");
    CHECK(alloc_io > plain_io);

    {
        Ext2Ctx *fresh = (Ext2Ctx *)ext2_vfs_mount(
            VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
        CHECK(fresh != (Ext2Ctx *)0);
        CHECK(fresh->sb_info.free_blocks_count == g_ec->sb_info.free_blocks_count);
        ext2_vfs_umount(fresh);
    }
    disk_teardown();
}

/* P10: メタデータ (空きブロック / 空き inode / used_dirs / ビットマップ番地)
 *      を動かす操作のあと、必ずディスク側と一致していること。
 *      ext2_meta_touch() の付け忘れはここで落ちる。 */
static void meta_matches_disk(const char *what)
{
    Ext2Ctx *fresh;
    u32 g;

    fresh = (Ext2Ctx *)ext2_vfs_mount(VFS_MOUNT_DEV_ENCODE(VFS_DEV_HD, 0));
    CHECK(fresh != (Ext2Ctx *)0);
    if (fresh->sb_info.free_blocks_count != g_ec->sb_info.free_blocks_count ||
        fresh->sb_info.free_inodes_count != g_ec->sb_info.free_inodes_count) {
        report("    after "); report(what); report(": SB mismatch mem=");
        report_u(g_ec->sb_info.free_blocks_count); report("/");
        report_u(g_ec->sb_info.free_inodes_count); report(" disk=");
        report_u(fresh->sb_info.free_blocks_count); report("/");
        report_u(fresh->sb_info.free_inodes_count); report("\n");
        fail("superblock did not reach the disk", __LINE__);
    }
    CHECK(fresh->num_groups == g_ec->num_groups);
    for (g = 0; g < fresh->num_groups; g++) {
        if (fresh->gd_table[g].free_blocks != g_ec->gd_table[g].free_blocks ||
            fresh->gd_table[g].free_inodes != g_ec->gd_table[g].free_inodes ||
            fresh->gd_table[g].used_dirs   != g_ec->gd_table[g].used_dirs) {
            report("    after "); report(what); report(": GD mismatch group ");
            report_u(g); report("\n");
            fail("group descriptor did not reach the disk", __LINE__);
        }
    }
    ext2_vfs_umount(fresh);
}

static void case_meta_reaches_disk(void)
{
    static u8 big[5000];
    u32 i;

    disk_setup();
    for (i = 0; i < sizeof(big); i++) big[i] = (u8)(i * 17u + 3u);

    CHECK(ext2_vfs_mkdir(g_ec, "/tmp") == VFS_OK);
    meta_matches_disk("mkdir");

    CHECK(ext2_vfs_write(g_ec, "/tmp/f", big, sizeof(big)) == VFS_OK);
    meta_matches_disk("create");

    CHECK(wr("/tmp/f", big, 512, 5000) == 512);
    meta_matches_disk("append (new block)");

    CHECK(wr("/tmp/f", big, 16, 8) == 16);
    meta_matches_disk("overwrite (no alloc)");

    /* 12 直接ブロックを越えて間接ブロックを起こす */
    CHECK(wr("/tmp/f", big, 2000, 13u * 1024u) == 2000);
    meta_matches_disk("indirect block");

    CHECK(ext2_vfs_write(g_ec, "/tmp/f", big, 100) == VFS_OK);  /* 全体上書き */
    meta_matches_disk("truncate + rewrite");

    CHECK(ext2_vfs_rename(g_ec, "/tmp/f", "/tmp/g") == VFS_OK);
    meta_matches_disk("rename");

    CHECK(ext2_vfs_unlink(g_ec, "/tmp/g") == VFS_OK);
    meta_matches_disk("unlink");

    CHECK(ext2_vfs_rmdir(g_ec, "/tmp") == VFS_OK);
    meta_matches_disk("rmdir");

    report("  P10 metadata always reaches the disk\n");
    disk_teardown();
}

/* ======================================================================== */

static int str_eq(const char *a, const char *b) { return kstrcmp(a, b) == 0; }

int ext2_write_io_main(const char *sel)
{
    g_failures = 0;

    if (str_eq(sel, "all") || str_eq(sel, "w512_new"))        case_w512_new();
    if (str_eq(sel, "all") || str_eq(sel, "w512_existing"))   case_w512_existing();
    if (str_eq(sel, "all") || str_eq(sel, "w1_append"))       case_w1_append();
    if (str_eq(sel, "all") || str_eq(sel, "seek_back"))       case_seek_back();
    if (str_eq(sel, "all") || str_eq(sel, "tar_sequence"))    case_tar_sequence();
    if (str_eq(sel, "all") || str_eq(sel, "read512"))         case_read512();
    if (str_eq(sel, "all") || str_eq(sel, "write_through"))   case_write_through();
    if (str_eq(sel, "all") || str_eq(sel, "ns_invalidation")) case_ns_invalidation();
    if (str_eq(sel, "all") || str_eq(sel, "sync_on_alloc"))   case_sync_on_alloc();
    if (str_eq(sel, "all") || str_eq(sel, "meta_reaches_disk")) case_meta_reaches_disk();

    report("ext2_write_io: PASS (");
    report(sel);
    report(")\n");
    return g_failures ? 1 : 0;
}

/* -nostdlib の入口。プロセス開始時の esp は [argc][argv0][argv1]... を
 * 指しているので、そのまま C へ渡す (フレームポインタの有無に依存しない)。 */
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
    long argc = sp[0];
    char **argv = (char **)&sp[1];
    const char *sel = (argc >= 2 && argv[1]) ? argv[1] : "all";
    die(ext2_write_io_main(sel));
}
