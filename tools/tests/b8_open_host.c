/* ========================================================================
 *  b8_open_host.c — 読み取り失敗を「不存在」として扱う経路 (票 B8) を
 *  **実物の vfs_open() / vfs_open_sqlite() を通して**確かめる
 *
 *  票:   docs/tasks/shell/TASK_FS_TYPE.md §2 (B8)
 *  実行: python3 -B tools/tests/test_b8_open.py
 *  記録: tools/tests/b8_tdd.md
 *
 *  なぜ open まで通すのか
 *  ---------------------
 *  vfs_path_kind() の戻り値までしか見ない試験では、同じ形の欠陥 (B7) を
 *  往復 4 で**実際に取り逃している**。判定を**消費する側**まで動かさないと、
 *  「読めなかった」が「無い」に化けて何が起きるかは見えない。
 *
 *  2 つの土台を 1 本の実行ファイルに載せる。
 *
 *   段 A: **実物の ext2** (fs/ext2_*.c) を RAM ディスク上で実物の ext2_format()
 *         が作った 8MB のファイルシステムに載せ、**実物の fs/vfs.c と
 *         fs/vfs_fd.c** から vfs_open() で叩く。贋物は Device API と IDE だけ。
 *         - §2-2a: **間接ブロックを使う大きなディレクトリ** (直接 12 本を
 *           越えた /big) の間接ブロック読み取りを**一度だけ**失敗させ、
 *           ext2_bmap -> find_entry -> lookup -> stat -> open の全段で
 *           「未割当 = 検索終了」に化けないことを見る。
 *         - §2-2b: 種別は確定しているのに**サイズ取得だけ**が一度失敗する
 *           組み合わせ (最も重い無言のデータ消失)。O_CREAT / O_TRUNC の
 *           4 通りすべてで、FD が出ず・媒体に 1 セクタも書かれず・
 *           既存の中身がそのまま残ることを確かめる。
 *
 *   段 B: 合成 VfsOps (tools/tests/vfs_kind_host.c と同じ作法)。stat と
 *         get_file_size の戻り値を 1 つずつ指定でき、**write_file の呼び出し
 *         回数をそのまま数えられる**ので、「作成へ進んでいない」ことを
 *         回数 0 で押さえる。vfs_open_sqlite も同じ土台で通す。
 *
 *  エミュレータ・実デバイス・実イメージ・make には一切触れない。
 *
 *  [C1] C89 / GNU89。宣言はブロック先頭、`//` コメント無し。
 *  u32 は `unsigned long` (include/types.h) なので**必ず ILP32 で組む**。
 *  libc は使わない (-nostdlib、Linux の int 0x80 で write/exit するだけ)。
 * ======================================================================== */

#include "ext2_priv.h"
#include "ide.h"
#include "kmalloc.h"

/* ======================================================================== */
/*  libc の代わり (-nostdlib)                                               */
/* ======================================================================== */

static int g_exit_code;

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

static void report_i(int v)
{
    char buf[16];
    int i = 15;
    u32 u;
    buf[i] = '\0';
    if (v < 0) u = (u32)(-v); else u = (u32)v;
    if (u == 0) { buf[--i] = '0'; }
    while (u > 0) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) buf[--i] = '-';
    report(&buf[i]);
}

static int g_checks;
static int g_failures;

static void check_at(int cond, const char *what, int line)
{
    g_checks++;
    if (cond) return;
    g_failures++;
    report("  FAIL line ");
    report_i(line);
    report(": ");
    report(what);
    report("\n");
}

#define CHECK(x) check_at((x) ? 1 : 0, #x, __LINE__)

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

char *kstrncat(char *dst, const char *src, u32 n)
{
    u32 d = h_strlen(dst);
    if (d + 1 >= n) return dst;
    kstrncpy(dst + d, src, n - d);
    return dst;
}

/* gcc が構造体コピー等で呼ぶことがある */
void *memcpy(void *dst, const void *src, u32 n) { return kmemcpy(dst, src, n); }
void *memset(void *dst, int val, u32 n) { return kmemset(dst, val, n); }

void kprintf(u8 attr, const char *fmt, ...) { (void)attr; (void)fmt; }

/* ---- kzalloc / kfree: 固定スロットの贋物 (Ext2Ctx 専用) ---- */
#define HEAP_SLOTS  4
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

/* ======================================================================== */
/*  RAM ディスク + **一度だけの I/O 失敗**の注入                            */
/* ======================================================================== */

/* 8MB の ext2。base_lba はパーティションテーブルが空なので
 * ext2_find_partition() のフォールバック (1088) になる。 */
#define DISK_FS_SECTORS   16384u
#define DISK_BASE_LBA     1088u
#define DISK_SECTORS      (DISK_BASE_LBA + DISK_FS_SECTORS)

static u8 g_disk[DISK_SECTORS * 512u];
static u32 g_rd_sect;
static u32 g_wr_sect;

/* 失敗の注入: 「この LBA の n 回目の読み出しを**一度だけ**失敗させる」。
 * 一度だけにするのが肝 — 恒久的な故障なら誰が見ても異常だが、B8 が起きるのは
 * **一度読めなかっただけで次は読める**ときである。 */
static u32 g_fail_lba;
static int g_fail_armed;      /* 1 = 仕掛けてある */
static int g_fail_nth;        /* 何回目の一致を落とすか (1 起算) */
static int g_fail_seen;       /* 仕掛けてからの一致回数 */
static int g_fail_fired;      /* 実際に落とした回数 */

static void fail_arm(u32 lba, int nth)
{
    g_fail_lba = lba; g_fail_armed = 1; g_fail_nth = nth;
    g_fail_seen = 0; g_fail_fired = 0;
}

static void fail_arm_always(u32 lba) { fail_arm(lba, 0); }

static void fail_disarm(void) { g_fail_armed = 0; }

static void io_reset(void) { g_rd_sect = 0; g_wr_sect = 0; }

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
        u32 cur = lba + (u32)i;
        if (cur >= DISK_SECTORS) return -1;
        if (g_fail_armed && cur == g_fail_lba) {
            g_fail_seen++;
            /* nth == 0 は「armed のあいだずっと落とす」(本当の不良セクタ) */
            if (g_fail_nth == 0 || g_fail_seen == g_fail_nth) {
                g_fail_fired++;
                return -1;
            }
        }
        kmemcpy((u8 *)buf + i * 512, g_disk + cur * 512u, 512);
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

/* ---- fs/vfs_fd.c の境界 (コンソール / リダイレクト / 所有者タグ) ---- */
int fd_is_redirected(int fd) { (void)fd; return 0; }
int fd_redirect_read(int fd, void *buf, u32 size)
{ (void)fd; (void)buf; (void)size; return VFS_ERR_INVAL; }
int fd_redirect_write(int fd, const void *buf, u32 size)
{ (void)fd; (void)buf; (void)size; return VFS_ERR_INVAL; }
int kbd_getchar(void) { return '\n'; }
void console_write(const char *buf, u32 size, u8 color)
{ (void)buf; (void)size; (void)color; }
int res_owner_get(void) { return 0; }

/* HostDrv 側のサイズ取得の純規則も実物を取り込む (③、fs/hostdrvfs.c が使う)。
 * hostdrvfs.c 本体はハイパーコールを叩くのでホストでは組めないが、
 * 判定はこの .inc の純関数に切り出してある (票 H1 と同じ作法)。 */
#include "../../fs/hostdrv_stat_rules.inc"

#include "../../fs/ext2_super.c"
#include "../../fs/ext2_inode.c"
#include "../../fs/ext2_dir.c"
#include "../../fs/ext2_file.c"
#include "../../fs/ext2_fmt.c"
#include "../../fs/ext2_vfs.c"
#include "../../fs/vfs.c"
#include "../../fs/vfs_fd.c"

/* ======================================================================== */
/*  足場                                                                    */
/* ======================================================================== */

static Ext2Ctx *g_ec;

/* /big/keepme の中身。**open が失敗したあともここがそのまま残ること**が、
 * 段 A の一番大事な確認。 */
static const char KEEP_TEXT[] = "KEEPME-0123456789-do-not-erase";
#define KEEP_LEN  30                  /* NUL を含めない */

/* 直接ブロック 12 本 (12KB) を越えさせるための詰め物。
 * 名前 6 文字 -> rec_len = (8+6+3)&~3 = 16 バイト -> 1 ブロック 64 件。
 * 12 ブロックで 768 件なので、それを十分に越える数を入れる。 */
#define FILLER_COUNT  900

static void fd_table_reset(void)
{
    int i;
    for (i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        open_files[i].in_use = 0;
        open_files[i].generation = 0;
    }
}

static void vfs_tables_reset(void)
{
    int i;
    num_fs = 0;
    for (i = 0; i < VFS_MAX_FS; i++) kmemset(&mounts[i], 0, sizeof(mounts[i]));
    fd_table_reset();
    kstrncpy(cwd, "/", VFS_MAX_PATH);
}

static void filler_name(char *dst, int n)
{
    dst[0] = 'f';
    dst[1] = (char)('0' + (n / 1000) % 10);
    dst[2] = (char)('0' + (n / 100) % 10);
    dst[3] = (char)('0' + (n / 10) % 10);
    dst[4] = (char)('0' + n % 10);
    dst[5] = 'x';
    dst[6] = '\0';
}

/* /big/keepme の中身を読み直す。**open を通さず** ext2 の read_file で直に
 * 見るので、「open が壊したかどうか」を open 自身に聞かずに済む。 */
static int keep_intact(void)
{
    static u8 buf[128];
    int n;
    u32 ino;
    kmemset(buf, 0, sizeof(buf));
    if (ext2_lookup(g_ec, "/big/keepme", &ino) != EXT2_OK) return 0;
    n = ext2_read_file(g_ec, ino, buf, sizeof(buf));
    if (n != KEEP_LEN) return 0;
    return kstrncmp((const char *)buf, KEEP_TEXT, KEEP_LEN) == 0;
}

static u32 keep_ino(void)
{
    u32 ino = 0;
    ext2_lookup(g_ec, "/big/keepme", &ino);
    return ino;
}

/* inode 番号 -> その inode が載っている **1KB ブロックの先頭セクタ LBA**。
 * fs/ext2_inode.c の ext2_read_inode と同じ式。 */
static u32 lba_of_inode(u32 ino)
{
    u32 group = (ino - 1) / g_ec->sb_info.inodes_per_group;
    u32 index = (ino - 1) % g_ec->sb_info.inodes_per_group;
    u32 blk = g_ec->gd_table[group].inode_table
              + (index * g_ec->sb_info.inode_size) / EXT2_BLOCK_SIZE;
    return g_ec->base_lba + blk * 2;
}

/* ディレクトリ /big の**間接ブロック**の先頭セクタ LBA (§2-2a の経路) */
static u32 lba_of_big_indirect(void)
{
    Ext2Inode dir;
    u32 ino = 0;
    CHECK(ext2_lookup(g_ec, "/big", &ino) == EXT2_OK);
    CHECK(ext2_read_inode(g_ec, ino, &dir) == EXT2_OK);
    /* ここが 0 なら詰め物が足りず、そもそも間接ブロックを使っていない */
    CHECK(dir.block[EXT2_IND_BLOCK] != 0);
    return g_ec->base_lba + dir.block[EXT2_IND_BLOCK] * 2;
}

static void disk_setup(void)
{
    int i;
    char name[8];

    kmemset(g_disk, 0, sizeof(g_disk));
    kmemset(&g_hd0, 0, sizeof(g_hd0));
    g_hd0.name = "hd0";
    g_hd0.type = DEV_BLOCK;
    g_hd0.bus_type = DEV_BUS_IDE;
    g_hd0.sect_size = 512;
    g_hd0.total_sects = DISK_SECTORS;
    g_hd0.heads = 8;
    g_hd0.spt = 17;
    fail_disarm();

    CHECK(ext2_format(0, DISK_FS_SECTORS) == EXT2_OK);

    vfs_tables_reset();
    ext2_init();                               /* 実物の登録 */
    CHECK(vfs_mount("/", "hd0", "ext2") == VFS_OK);
    g_ec = (Ext2Ctx *)mounts[0].fs_ctx;
    CHECK(g_ec != (Ext2Ctx *)0);
    CHECK(g_ec->base_lba == DISK_BASE_LBA);

    /* 間接ブロックを使う大きなディレクトリ (§2-2a)。
     * 詰め物の 900 件は足場なので CHECK で数えず、失敗したらその場で止める。 */
    CHECK(ext2_vfs_mkdir(g_ec, "/big") == VFS_OK);
    {
        u32 dir_ino = 0;
        CHECK(ext2_lookup(g_ec, "/big", &dir_ino) == EXT2_OK);
        for (i = 0; i < FILLER_COUNT; i++) {
            filler_name(name, i);
            if (ext2_create(g_ec, dir_ino, name, "", 0) != EXT2_OK) {
                report("  (harness) filler create failed at ");
                report_i(i); report("\n");
                g_failures++;
                return;
            }
        }
    }
    /* 目当てのファイルは**最後**に入れる = 間接ブロック側のブロックに載る */
    CHECK(ext2_vfs_write(g_ec, "/big/keepme", KEEP_TEXT, KEEP_LEN) == VFS_OK);
    CHECK(keep_intact());

    /* 通常ファイルとディレクトリの回帰用 */
    CHECK(ext2_vfs_mkdir(g_ec, "/etc") == VFS_OK);
    CHECK(ext2_vfs_write(g_ec, "/etc/plain", "PLAIN", 5) == VFS_OK);
}

static void disk_teardown(void)
{
    fail_disarm();
    if (g_ec) { vfs_umount("/"); g_ec = (Ext2Ctx *)0; }
}

/* 記憶 (票 S6-P の解決済み経路) を捨てて、必ず実際に辿り直させる */
static void memo_cold(void) { ext2_path_memo_reset(g_ec); }

static int any_fd_open(void)
{
    int i, n = 0;
    for (i = 0; i < VFS_MAX_OPEN_FILES; i++) if (open_files[i].in_use) n++;
    return n;
}

/* ======================================================================== */
/*  段 A: 実物の ext2 + 実物の vfs_open                                     */
/* ======================================================================== */

/* §2-2a — 間接ブロックが一度読めなかっただけで「無い」に化けないこと。
 * 直す前: ext2_bmap が読み取り失敗を 0 (= 未割当) に潰す
 *         -> ext2_find_entry が「検索終了」と読んで NOTFOUND
 *         -> ext2_lookup / ext2_vfs_stat も NOTFOUND
 *         -> vfs_path_kind が NOTFOUND -> open が O_CREAT 経路へ進む。 */
static void case_indirect_read_failure(int mode, const char *label)
{
    int fd;
    u32 lba;

    report("  [A1] "); report(label); report("\n");

    memo_cold();
    lba = lba_of_big_indirect();
    io_reset();
    fail_arm(lba, 1);
    fd = vfs_open("/big/keepme", mode);
    fail_disarm();

    CHECK(g_fail_fired == 1);            /* 仕掛けが本当に効いたか */
    CHECK(fd < 0);                        /* FD を発行しない */
    CHECK(fd != VFS_ERR_NOTFOUND);        /* 「無い」と言わない */
    CHECK(any_fd_open() == 0);
    CHECK(g_wr_sect == 0);                /* 媒体に 1 セクタも書いていない */
    CHECK(keep_intact());                 /* 中身がそのまま残っている */
}

/* §2-2b — **最も重い経路**。種別は stat で「通常ファイル」と確定しているのに、
 * 直後のサイズ取得だけが一度 I/O に失敗する。
 * 直す前: vfs_open_internal が「サイズが取れない = 存在しない」と読み、
 *         O_CREAT が付いていれば **O_TRUNC が無くても** 0 バイトで上書き。
 *
 * 1 回の vfs_open のあいだ、目当ての inode が載るブロックは
 *   1 回目 = vfs_path_kind -> ext2_vfs_stat の ext2_read_inode
 *   2 回目 = ops->get_file_size -> ext2_get_size_ino の ext2_read_inode
 * の 2 回読まれる (記憶が温まっていればパス解決は辿り直さない)。
 * その **2 回目だけ**を落とす。 */
static void case_size_failure(int mode, const char *label)
{
    int fd;
    u32 lba;

    report("  [A2] "); report(label); report("\n");

    /* 記憶を温めて、パス解決が inode 表を読み直さない状態にする */
    memo_cold();
    CHECK(vfs_path_kind("/big/keepme") == VFS_KIND_FILE);

    lba = lba_of_inode(keep_ino());
    io_reset();
    fail_arm(lba, 2);
    fd = vfs_open("/big/keepme", mode);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(fd < 0);
    CHECK(fd != VFS_ERR_NOTFOUND);
    CHECK(any_fd_open() == 0);
    CHECK(g_wr_sect == 0);
    CHECK(keep_intact());
}

static void case_sqlite_size_failure(void)
{
    VfsSqliteCookie ck;
    VfsSqliteLease lease;
    int rc;
    u32 lba;

    report("  [A3] vfs_open_sqlite も同じ (サイズ取得の一度の失敗)\n");

    ck.group_index = 0;
    ck.generation = 1;
    kmemset(&lease, 0, sizeof(lease));

    memo_cold();
    CHECK(vfs_path_kind("/big/keepme") == VFS_KIND_FILE);

    lba = lba_of_inode(keep_ino());
    io_reset();
    fail_arm(lba, 2);
    rc = vfs_open_sqlite("/big/keepme", O_RDWR | O_CREAT, 0, &ck, 0, &lease);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc < 0);
    CHECK(rc != VFS_ERR_NOTFOUND);
    CHECK(any_fd_open() == 0);
    CHECK(g_wr_sect == 0);
    CHECK(keep_intact());
}

/* /big の中の "keepme" という名前のエントリ数。一括書き込みが「無い」と
 * 誤判断して ext2_create へ落ちると、同じ名前が 2 つ並ぶ。 */
static int g_name_hits;
static void count_cb(const Ext2DirEntry *e, void *ctx)
{
    (void)ctx;
    if (e->name_len == 6 && kstrncmp(e->name, "keepme", 6) == 0) g_name_hits++;
}

static int keep_entry_count(void)
{
    u32 ino = 0;
    g_name_hits = 0;
    if (ext2_lookup(g_ec, "/big", &ino) != EXT2_OK) return -1;
    if (ext2_list_dir(g_ec, ino, count_cb, (void *)0) != EXT2_OK) return -1;
    return g_name_hits;
}

/* ext2_vfs_write (一括書き込み) — 既存かどうかの判定が読めなかったときに
 * **新規作成へ落ちない**こと。落ちると同じ名前の二重エントリができ、
 * 元の inode が名前から辿れなくなる。 */
static void case_write_file_failure(void)
{
    int rc;
    u32 lba;

    report("  [A6] 一括書き込み: 既存判定が読めなければ作成しない\n");

    /* 親 "/big" だけ記憶を温める — パス解決は通り、find_entry が失敗する */
    memo_cold();
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/big", &st) == VFS_OK);
    }

    CHECK(keep_entry_count() == 1);
    lba = lba_of_big_indirect();
    io_reset();
    fail_arm(lba, 1);
    rc = ext2_vfs_write(g_ec, "/big/keepme", "XX", 2);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    /* **読めなかったことを読めなかったと言う**。ここを「無い」と読み替えると
     * ext2_create へ落ち、その中の存在確認が (一度きりの失敗なら) 今度は通って
     * EXIST になる — 呼び手には「既にある」と見え、I/O 障害が隠れる。
     * 恒久的な失敗なら二重エントリになり、元の inode が名前から辿れなくなる。 */
    CHECK(rc == VFS_ERR_IO);
    CHECK(keep_entry_count() == 1);      /* 二重エントリを作っていない */
    CHECK(keep_intact());                /* 中身もそのまま */

    /* 本当の不良セクタ (ずっと読めない) でも作成へ落ちない */
    memo_cold();
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/big", &st) == VFS_OK);
    }
    lba = lba_of_big_indirect();
    fail_arm_always(lba);
    rc = ext2_vfs_write(g_ec, "/big/keepme", "YY", 2);
    fail_disarm();
    CHECK(g_fail_fired > 0);
    CHECK(rc == VFS_ERR_IO);
    CHECK(keep_entry_count() == 1);
    CHECK(keep_intact());
}

/* ext2_write_stream — 間接ブロックが読めなかったのを「未割当」と見なして
 * **新しいブロックを割り当て直すと、元の中身を捨てる**。 */
static void case_write_stream_failure(void)
{
    static u8 pattern[16 * 1024];
    static u8 got[2 * 1024];
    Ext2Inode fi;
    u32 ino = 0, lba, i;
    int rc;

    report("  [A7] 追記書き込み: 読めない間接ブロックを割り当て直さない\n");

    for (i = 0; i < sizeof(pattern); i++) pattern[i] = (u8)(i * 7 + 1);
    /* 16KB = 16 ブロック -> 直接 12 本を越えて間接ブロックを使う */
    CHECK(ext2_vfs_write(g_ec, "/big/wide", pattern, sizeof(pattern)) == VFS_OK);

    memo_cold();
    CHECK(ext2_lookup(g_ec, "/big/wide", &ino) == EXT2_OK);
    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    CHECK(fi.block[EXT2_IND_BLOCK] != 0);
    lba = g_ec->base_lba + fi.block[EXT2_IND_BLOCK] * 2;

    /* 記憶を温めてから、間接ブロックの読み出しだけを一度落とす */
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/big/wide", &st) == VFS_OK);
    }
    io_reset();
    fail_arm(lba, 1);
    rc = ext2_vfs_write_stream(g_ec, "/big/wide", "ZZZZ", 4, 13 * 1024 + 100);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc < 0);                       /* 「0 バイト書けた」で済ませない */

    /* 論理ブロック 13 の中身が丸ごと残っていること。割り当て直していたら
     * ここはゼロ埋めの新しいブロックに化けている。 */
    kmemset(got, 0, sizeof(got));
    CHECK(ext2_read_stream(g_ec, ino, got, 1024, 13 * 1024) == 1024);
    CHECK(kstrncmp((const char *)got, (const char *)&pattern[13 * 1024], 64) == 0);
    {
        int same = 1;
        for (i = 0; i < 1024; i++) {
            if (got[i] != pattern[13 * 1024 + i]) { same = 0; break; }
        }
        CHECK(same);
    }

    /* 正常時は同じ書き込みがちゃんと通る (回帰) */
    CHECK(ext2_vfs_write_stream(g_ec, "/big/wide", "ZZZZ", 4,
                                13 * 1024 + 100) == 4);
    kmemset(got, 0, sizeof(got));
    CHECK(ext2_read_stream(g_ec, ino, got, 1024, 13 * 1024) == 1024);
    CHECK(kstrncmp((const char *)&got[100], "ZZZZ", 4) == 0);
    CHECK(got[0] == pattern[13 * 1024]);
}

/* ディレクトリ ino の**直接ブロック 0** の先頭セクタ LBA */
static u32 lba_of_block0(u32 ino)
{
    Ext2Inode n;
    CHECK(ext2_read_inode(g_ec, ino, &n) == EXT2_OK);
    CHECK(n.block[0] != 0);
    return g_ec->base_lba + n.block[0] * 2;
}

static int g_entry_total;
static void total_cb(const Ext2DirEntry *e, void *ctx)
{ (void)e; (void)ctx; g_entry_total++; }

/* VFS 層の一覧用 (NULL コールバックは ext2_to_vfs_cb が呼び出して落ちる) */
static int g_vfs_entries;
static void vfs_total_cb(const VfsDirEntry *e, void *ctx)
{ (void)e; (void)ctx; g_vfs_entries++; }

static int dir_entry_total(const char *path)
{
    u32 ino = 0;
    g_entry_total = 0;
    if (ext2_lookup(g_ec, path, &ino) != EXT2_OK) return -1;
    if (ext2_list_dir(g_ec, ino, total_cb, (void *)0) != EXT2_OK) return -1;
    return g_entry_total;
}

/* ext2_bmap の**残りの呼び手**が、新しい区別を正しく扱っているか。
 * 1 つでも「読めなかった」を「未割当 = ここで終わり」と読む呼び手が残ると
 * 修正の意味が無い。 */
static void case_other_bmap_callers(void)
{
    u32 big_ino = 0, ind_lba, blk0_lba;
    int n, rc;

    report("  [A8] ext2_bmap の残りの呼び手\n");

    memo_cold();
    CHECK(ext2_lookup(g_ec, "/big", &big_ino) == EXT2_OK);
    ind_lba = lba_of_big_indirect();

    /* (1) ext2_list_dir — **打ち切った一覧を成功として返さない** */
    n = dir_entry_total("/big");
    CHECK(n > FILLER_COUNT);                  /* 全件見えている */
    fail_arm(ind_lba, 1);
    g_entry_total = 0;
    rc = ext2_list_dir(g_ec, big_ino, total_cb, (void *)0);
    fail_disarm();
    CHECK(g_fail_fired == 1);
    CHECK(rc == EXT2_ERR_IO);                 /* 途中までを成功と言わない */

    /* VFS 層 (ext2_vfs_list) でも同じ */
    memo_cold();
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/big", &st) == VFS_OK);
    }
    fail_arm(ind_lba, 1);
    g_vfs_entries = 0;
    rc = ext2_vfs_list(g_ec, "/big", vfs_total_cb, (void *)0);
    fail_disarm();
    CHECK(g_fail_fired == 1);
    CHECK(rc == VFS_ERR_IO);

    /* (2) ext2_add_entry — 読めないまま抜けて新ブロックを継ぎ足さない。
     * 継ぎ足すと bmap_set(12) が**既存の間接ブロックの割り当てを上書き**し、
     * 間接側のエントリが丸ごと行方不明になる。 */
    n = dir_entry_total("/big");
    fail_arm(ind_lba, 1);
    rc = ext2_add_entry(g_ec, big_ino, "zzz", 2, EXT2_FT_REG_FILE);
    fail_disarm();
    CHECK(g_fail_fired == 1);
    CHECK(rc == EXT2_ERR_IO);
    CHECK(dir_entry_total("/big") == n);      /* 1 件も失っていない */
    CHECK(keep_intact());

    /* (3) ext2_delete_entry */
    fail_arm(ind_lba, 1);
    rc = ext2_delete_entry(g_ec, big_ino, "keepme");
    fail_disarm();
    CHECK(g_fail_fired == 1);
    CHECK(rc == EXT2_ERR_IO);                 /* NOTFOUND と言わない */
    CHECK(keep_intact());

    /* (4) ext2_read_file / ext2_read_stream — **短いファイル**に化けない */
    {
        static u8 got[20 * 1024];
        u32 wide_ino = 0;
        u32 wide_ind;
        Ext2Inode wi;

        memo_cold();
        CHECK(ext2_lookup(g_ec, "/big/wide", &wide_ino) == EXT2_OK);
        CHECK(ext2_read_inode(g_ec, wide_ino, &wi) == EXT2_OK);
        wide_ind = g_ec->base_lba + wi.block[EXT2_IND_BLOCK] * 2;

        CHECK(ext2_read_file(g_ec, wide_ino, got, sizeof(got)) == 16 * 1024);

        fail_arm(wide_ind, 1);
        rc = ext2_read_file(g_ec, wide_ino, got, sizeof(got));
        fail_disarm();
        CHECK(g_fail_fired == 1);
        CHECK(rc == EXT2_ERR_IO);             /* 12KB の「成功」にしない */

        fail_arm(wide_ind, 1);
        rc = ext2_read_stream(g_ec, wide_ino, got, 16 * 1024, 0);
        fail_disarm();
        CHECK(g_fail_fired == 1);
        CHECK(rc == EXT2_ERR_IO);
    }

    /* (5) ext2_is_dir_empty (rmdir 経由) — 読めなかったのを NOTEMPTY と偽らない */
    {
        u32 parent = 0, victim = 0;
        CHECK(ext2_vfs_mkdir(g_ec, "/rmtest") == VFS_OK);
        memo_cold();
        CHECK(ext2_lookup(g_ec, "/", &parent) == EXT2_OK);
        CHECK(ext2_lookup(g_ec, "/rmtest", &victim) == EXT2_OK);
        blk0_lba = lba_of_block0(victim);

        fail_arm(blk0_lba, 1);
        rc = ext2_rmdir(g_ec, parent, "rmtest");
        fail_disarm();
        CHECK(g_fail_fired == 1);
        CHECK(rc == EXT2_ERR_IO);
        CHECK(rc != EXT2_ERR_NOTEMPTY);

        /* 読めるなら従来どおり消せる (回帰) */
        CHECK(ext2_rmdir(g_ec, parent, "rmtest") == EXT2_OK);
    }

    /* (6) ext2_parent_of (rename の循環検査) — 読めなかったのを
     * 「祖先ではない」と言うと、ディレクトリを自分の配下へ移せてしまう。 */
    {
        u32 root = 0, d1 = 0, d2 = 0;
        CHECK(ext2_vfs_mkdir(g_ec, "/d1") == VFS_OK);
        CHECK(ext2_vfs_mkdir(g_ec, "/d1/d2") == VFS_OK);
        memo_cold();
        CHECK(ext2_lookup(g_ec, "/", &root) == EXT2_OK);
        CHECK(ext2_lookup(g_ec, "/d1", &d1) == EXT2_OK);
        CHECK(ext2_lookup(g_ec, "/d1/d2", &d2) == EXT2_OK);

        /* 読めれば従来どおり断る */
        CHECK(ext2_rename(g_ec, root, "d1", d2, "moved") == EXT2_ERR_INVAL);

        /* ".." が読めないときは **INVAL とも OK とも言わず** I/O エラー。
         * d2 の先頭ブロックは 1 回の rename のあいだに
         *   1 回目 = 宛先の重複確認 (ext2_find_entry(d2, "moved"))
         *   2 回目 = 循環検査の ext2_parent_of(d2) の ".." 引き
         * と読まれるので、**2 回目だけ**落とす。ずっと落とすと後段の
         * ext2_add_entry も止まってしまい、循環検査を素通りした場合の
         * 被害 (木が輪になる) が見えない。 */
        blk0_lba = lba_of_block0(d2);
        fail_arm(blk0_lba, 2);
        rc = ext2_rename(g_ec, root, "d1", d2, "moved");
        fail_disarm();
        CHECK(g_fail_fired == 1);
        CHECK(rc == EXT2_ERR_IO);
        CHECK(rc != EXT2_OK);
        /* 木が輪になっていない — /d1 は root の下のまま */
        {
            u32 chk = 0;
            memo_cold();
            CHECK(ext2_lookup(g_ec, "/d1/d2", &chk) == EXT2_OK);
            CHECK(chk == d2);
        }
    }
}

/* ======================================================================== */
/*  段 A2: Codex 実装レビュー P1-1 / P1-2 / P1-3 / P1-5 の反例              */
/*                                                                          */
/*  どれも「検索や更新の**失敗を二値に潰した**次の段」で、前回の修正が       */
/*  届いていなかったところ。Codex は 32bit バイナリを Unicorn で実行して     */
/*  実際に再現している。同じ形をここに入れる。                              */
/* ======================================================================== */

/* "/big" だけ記憶を温める — パス解決は通り、その先の検索が失敗する状態 */
static void warm_big(void)
{
    OS32_Stat st;
    memo_cold();
    CHECK(ext2_vfs_stat(g_ec, "/big", &st) == VFS_OK);
}

/* P1-1: mkdir が「存在確認が読めなかった」まま作らない。
 * 直す前: `if (find_entry(...) == EXT2_OK) return EXIST;` なので I/O エラーは
 * すり抜け、既にある /big/keepme と**同名のディレクトリを作っていた**
 * (Codex 実測: 成功を返し 18 セクタ書き込み、同名エントリ 2 件)。 */
static void case_mkdir_existence_failure(void)
{
    int rc, n;
    u32 lba;

    report("  [P1-1] mkdir: 存在確認が読めなければ作らない\n");

    warm_big();
    n = keep_entry_count();
    CHECK(n == 1);
    lba = lba_of_big_indirect();
    io_reset();
    fail_arm(lba, 1);
    rc = vfs_mkdir("/big/keepme");
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc < 0);
    CHECK(rc != VFS_OK);
    CHECK(rc != VFS_ERR_EXIST);         /* 「既にある」とも言わない */
    CHECK(g_wr_sect == 0);              /* **1 セクタも書いていない** */
    CHECK(keep_entry_count() == 1);     /* 同名エントリが増えていない */
    CHECK(keep_intact());

    /* 回帰: 読めるなら従来どおり EXIST / 作成ができる */
    memo_cold();
    CHECK(vfs_mkdir("/big/keepme") == VFS_ERR_EXIST);
    memo_cold();
    CHECK(vfs_mkdir("/big/newdir") == VFS_OK);
    CHECK(vfs_path_kind("/big/newdir") == VFS_KIND_DIR);
}

/* P1-2: ext2_create が同じ形。
 * (VFS 経由の ext2_vfs_write は前回直したので、ここは関数の契約そのものを
 *  直接叩く。Codex 実測: 成功を返して同名ファイルを二重作成、16 セクタ。) */
static void case_create_existence_failure(void)
{
    int rc;
    u32 big_ino = 0, lba;

    report("  [P1-2] ext2_create: 存在確認が読めなければ作らない\n");

    warm_big();
    CHECK(ext2_lookup(g_ec, "/big", &big_ino) == EXT2_OK);
    lba = lba_of_big_indirect();
    io_reset();
    fail_arm(lba, 1);
    rc = ext2_create(g_ec, big_ino, "keepme", "XX", 2);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc == EXT2_ERR_IO);
    CHECK(rc != EXT2_OK);
    CHECK(rc != EXT2_ERR_EXIST);
    CHECK(g_wr_sect == 0);
    CHECK(keep_entry_count() == 1);
    CHECK(keep_intact());

    /* 回帰 */
    CHECK(ext2_create(g_ec, big_ino, "keepme", "XX", 2) == EXT2_ERR_EXIST);
    CHECK(keep_intact());
    CHECK(ext2_create(g_ec, big_ino, "fresh1", "YY", 2) == EXT2_OK);
}

/* P1-3: rename の**宛先**存在確認。
 * 直す前: 置き換えの分岐を丸ごと飛ばし、add_entry が宛先に同名エントリを
 * 二重に作ったうえで delete_entry が移動元の名前を消していた
 * (Codex 実測: 成功を返し 10 セクタ書き込み、**名前が片方だけ消えて二重**)。 */
static void case_rename_dest_failure(void)
{
    int rc;
    u32 lba, tmp = 0;

    report("  [P1-3] rename: 宛先の確認が読めなければ何もしない\n");

    memo_cold();
    CHECK(vfs_path_kind("/etc/plain") == VFS_KIND_FILE);
    warm_big();

    CHECK(keep_entry_count() == 1);
    lba = lba_of_big_indirect();
    io_reset();
    fail_arm(lba, 1);
    rc = vfs_rename("/etc/plain", "/big/keepme");
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc < 0);
    CHECK(rc != VFS_OK);
    CHECK(g_wr_sect == 0);                       /* 何も書いていない */
    CHECK(keep_entry_count() == 1);              /* 宛先が二重になっていない */
    CHECK(keep_intact());                        /* 宛先の中身もそのまま */
    /* **移動元の名前が残っている** */
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/plain", &tmp) == EXT2_OK);
    CHECK(vfs_path_kind("/etc/plain") == VFS_KIND_FILE);

    /* 回帰: 読めるなら従来どおり (ファイル同士は置き換え) */
    memo_cold();
    CHECK(vfs_rename("/etc/plain", "/etc/plain2") == VFS_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/plain", &tmp) == EXT2_ERR_NOTFOUND);
    CHECK(vfs_path_kind("/etc/plain2") == VFS_KIND_FILE);
    memo_cold();
    CHECK(vfs_rename("/etc/plain2", "/etc/plain") == VFS_OK);
}

/* P1-5: 追記の inode 更新が失敗したのに成功 (バイト数) を返していた。
 * Codex 実測: 5 バイトのファイルに 4 バイト追記 -> 戻り値 4、
 * 媒体上のサイズは 5 のまま = 追記が見えない。
 *
 * 1 回の ext2_write_stream で、目当ての inode が載るブロックは
 *   1 回目 = 先頭の ext2_read_inode
 *   2 回目 = 最後の ext2_write_inode の read-modify-write
 * と読まれる。その **2 回目だけ**を落とす。 */
static void case_write_stream_inode_failure(void)
{
    u32 ino = 0, lba, sz = 0;
    int rc;

    report("  [P1-5] 追記: inode を書けなければ成功と言わない\n");

    CHECK(ext2_vfs_write(g_ec, "/etc/small", "01234", 5) == VFS_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/small", &ino) == EXT2_OK);
    /* 記憶を温めてパス解決が inode 表を読み直さないようにする */
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/etc/small", &st) == VFS_OK);
    }

    lba = lba_of_inode(ino);
    io_reset();
    fail_arm(lba, 2);
    rc = ext2_vfs_write_stream(g_ec, "/etc/small", "ABCD", 4, 5);
    fail_disarm();

    CHECK(g_fail_fired == 1);
    CHECK(rc < 0);                       /* 4 (成功) と言わない */
    CHECK(rc != 4);
    /* **戻り値が成功でないなら、媒体上のサイズと食い違っていてよい**。
     * 逆に「成功なら一致する」ことを次で押さえる。 */
    CHECK(ext2_vfs_get_size(g_ec, "/etc/small", &sz) == VFS_OK);
    CHECK(sz == 5);

    /* 回帰: 同じ追記をやり直すと通り、**戻り値と媒体上のサイズが一致する** */
    rc = ext2_vfs_write_stream(g_ec, "/etc/small", "ABCD", 4, 5);
    CHECK(rc == 4);
    CHECK(ext2_vfs_get_size(g_ec, "/etc/small", &sz) == VFS_OK);
    CHECK(sz == 9);
    {
        static u8 got[32];
        kmemset(got, 0, sizeof(got));
        CHECK(ext2_read_file(g_ec, ino, got, sizeof(got)) == 9);
        CHECK(kstrncmp((const char *)got, "01234ABCD", 9) == 0);
    }
}

/* ブロックがまだ「使用中」かをビットマップで直に見る
 * (fs/ext2_inode.c の ext2_alloc_block と同じ式)。
 * **解放したかどうかは inode を見ても分からない** — 呼び手が失敗して
 * inode を書き戻さなければ媒体上の inode は元のままだからである。
 * 漏れ (解放したのに誰も指していない) を捕まえるにはここを見るしかない。 */
static int block_in_use(u32 blk)
{
    static u8 bm[EXT2_BLOCK_SIZE];
    u32 rel, g, bit, byte_idx, bit_idx;

    if (blk < g_ec->sb_info.first_data_block) return 1;
    rel = blk - g_ec->sb_info.first_data_block;
    g = rel / g_ec->sb_info.blocks_per_group;
    bit = rel % g_ec->sb_info.blocks_per_group;
    if (g >= g_ec->num_groups) return 1;
    if (ext2_read_block(g_ec, g_ec->gd_table[g].block_bitmap, bm) != 0) return 1;
    byte_idx = bit / 8;
    bit_idx = bit % 8;
    return (bm[byte_idx] & (1 << bit_idx)) ? 1 : 0;
}

/* 別票候補 -> 本票で対応: ext2_free_all_blocks が間接表を読めないとき、
 * **何も解放しない** (以前は表だけ解放して配下を行方不明にしていた)。 */
static void case_free_all_blocks_failure(void)
{
    static u8 pattern[16 * 1024];
    static u8 got[16 * 1024];
    Ext2Inode fi;
    u32 ino = 0, lba, i;
    u32 ind_before, dir0_before, child_before;
    int rc;

    report("  [BONUS] free_all_blocks: 表が読めなければ何も解放しない\n");

    for (i = 0; i < sizeof(pattern); i++) pattern[i] = (u8)(i * 11 + 3);
    CHECK(ext2_vfs_write(g_ec, "/etc/leak", pattern, sizeof(pattern)) == VFS_OK);

    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/leak", &ino) == EXT2_OK);
    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    CHECK(fi.block[EXT2_IND_BLOCK] != 0);
    ind_before = fi.block[EXT2_IND_BLOCK];
    dir0_before = fi.block[0];
    /* 間接表の 1 本目が指す実データブロック (表ごと消えると行方不明になる) */
    CHECK(ext2_bmap(g_ec, &fi, EXT2_NDIR_BLOCKS, &child_before) == EXT2_OK);
    CHECK(child_before != 0);
    lba = g_ec->base_lba + ind_before * 2;

    /* 上書き (= 切り詰めてから書き直す) の途中で間接表が読めなくなる */
    {
        OS32_Stat st;
        CHECK(ext2_vfs_stat(g_ec, "/etc/leak", &st) == VFS_OK);
    }
    io_reset();
    fail_arm_always(lba);
    rc = ext2_vfs_write(g_ec, "/etc/leak", "small", 5);
    fail_disarm();

    CHECK(g_fail_fired > 0);
    CHECK(rc < 0);                       /* 上書きは通らない */

    /* **何も解放していない**。媒体上の inode は書き戻されていないので
     * inode を見ても分からない — **ビットマップを直に見る**。
     * 直す前はここで直接ブロックが「空き」に戻り (下見が無いので先に解放
     * してしまう)、さらに間接表も解放されて配下が行方不明になっていた。 */
    CHECK(block_in_use(dir0_before) == 1);
    CHECK(block_in_use(ind_before) == 1);
    CHECK(block_in_use(child_before) == 1);   /* 間接表の先の実データ */

    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    CHECK(fi.block[EXT2_IND_BLOCK] == ind_before);
    CHECK(fi.block[0] == dir0_before);
    CHECK(fi.size == 16 * 1024);

    /* 解放されていたら、次の割り当てが同じブロックを別ファイルへ配る
     * (2026-09-06 の相互リンクと同じ壊れ方)。そうならないことを見る。 */
    {
        Ext2Inode ni;
        u32 nino = 0;
        int k;
        CHECK(ext2_vfs_write(g_ec, "/etc/after", pattern, 4096) == VFS_OK);
        memo_cold();
        CHECK(ext2_lookup(g_ec, "/etc/after", &nino) == EXT2_OK);
        CHECK(ext2_read_inode(g_ec, nino, &ni) == EXT2_OK);
        for (k = 0; k < EXT2_NDIR_BLOCKS; k++) {
            CHECK(ni.block[k] != dir0_before);
            CHECK(ni.block[k] != ind_before);
            CHECK(ni.block[k] != child_before);
        }
    }

    /* 中身も無事 */
    kmemset(got, 0, sizeof(got));
    CHECK(ext2_read_file(g_ec, ino, got, sizeof(got)) == 16 * 1024);
    {
        int same = 1;
        for (i = 0; i < sizeof(pattern); i++) {
            if (got[i] != pattern[i]) { same = 0; break; }
        }
        CHECK(same);
    }

    /* 回帰: 読めるなら従来どおり上書きできて、表が解放される */
    memo_cold();
    CHECK(ext2_vfs_write(g_ec, "/etc/leak", "small", 5) == VFS_OK);
    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    CHECK(fi.block[EXT2_IND_BLOCK] == 0);
    CHECK(fi.size == 5);
}

/* inode がまだ「使用中」かを inode ビットマップで直に見る
 * (fs/ext2_inode.c の ext2_free_inode と同じ式)。 */
static int inode_in_use(u32 ino)
{
    static u8 bm[EXT2_BLOCK_SIZE];
    u32 g, rel;
    g = (ino - 1) / g_ec->sb_info.inodes_per_group;
    rel = (ino - 1) % g_ec->sb_info.inodes_per_group;
    if (g >= g_ec->num_groups) return 1;
    if (ext2_read_block(g_ec, g_ec->gd_table[g].inode_bitmap, bm) != 0) return 1;
    return (bm[rel / 8] & (1 << (rel % 8))) ? 1 : 0;
}

/* 削除系: ブロックを返しきれなかったとき **inode まで空きに戻さない**。
 *
 * ext2_free_all_blocks は失敗時に 1 ブロックも解放せず指し先を inode に
 * 残す。そこで inode のビットだけ空きに戻すと、次の ext2_alloc_inode が
 * 同じ番号を配って ext2_create が inode を上書きし、残したブロックを
 * 指すものが誰も居なくなる (本当の漏れ)。再開時の読み直しで見つけた。 */
static void case_unlink_keeps_inode(void)
{
    static u8 pattern[16 * 1024];
    Ext2Inode fi;
    u32 ino = 0, etc_ino = 0, lba, ind, child, tmp = 0, i;
    int rc;

    report("  [BONUS-3] unlink: 返しきれなければ inode を解放しない\n");

    for (i = 0; i < sizeof(pattern); i++) pattern[i] = (u8)(i * 5 + 7);
    CHECK(ext2_vfs_write(g_ec, "/etc/orphan", pattern, sizeof(pattern)) == VFS_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc", &etc_ino) == EXT2_OK);
    CHECK(ext2_lookup(g_ec, "/etc/orphan", &ino) == EXT2_OK);
    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    ind = fi.block[EXT2_IND_BLOCK];
    CHECK(ind != 0);
    CHECK(ext2_bmap(g_ec, &fi, EXT2_NDIR_BLOCKS, &child) == EXT2_OK);
    lba = g_ec->base_lba + ind * 2;

    fail_arm_always(lba);
    rc = ext2_unlink(g_ec, etc_ino, "orphan");
    fail_disarm();

    CHECK(g_fail_fired > 0);
    CHECK(rc == EXT2_ERR_IO);                 /* 返しきれなかったと報告 */
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/orphan", &tmp) == EXT2_ERR_NOTFOUND);
    /* **inode もブロックも使用中のまま** = 孤児として辿れる */
    CHECK(inode_in_use(ino) == 1);
    CHECK(block_in_use(ind) == 1);
    CHECK(block_in_use(child) == 1);
    CHECK(block_in_use(fi.block[0]) == 1);
    /* 次に作るファイルが同じ inode 番号を受け取らない */
    CHECK(ext2_vfs_write(g_ec, "/etc/next", "N", 1) == VFS_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/next", &tmp) == EXT2_OK);
    CHECK(tmp != ino);
    /* 残した inode は依然としてブロックを指している */
    CHECK(ext2_read_inode(g_ec, ino, &fi) == EXT2_OK);
    CHECK(fi.block[EXT2_IND_BLOCK] == ind);
    CHECK(fi.links_count == 0);
    CHECK(fi.dtime != 0);
}

static void case_rmdir_keeps_inode(void)
{
    static char name[256];
    Ext2Inode di;
    u32 root = 0, dino = 0, ind, lba, nblocks, tmp = 0;
    int i, rc, nth;

    report("  [BONUS-4] rmdir: 返しきれなければ inode を解放しない\n");

    /* 間接ブロックを持つ**空の**ディレクトリを作る: 長い名前で
     * ブロックを埋めてから全部消す (ext2 はディレクトリを縮めない)。
     * 名前 240 文字 -> rec_len 248 -> 1 ブロック 4 件 -> 60 件で 15 ブロック。 */
    CHECK(ext2_vfs_mkdir(g_ec, "/rmbig") == VFS_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/rmbig", &dino) == EXT2_OK);
    for (i = 0; i < 240; i++) name[i] = 'n';
    name[240] = '\0';
    for (i = 0; i < 60; i++) {
        name[0] = (char)('A' + i / 26);
        name[1] = (char)('a' + i % 26);
        if (ext2_create(g_ec, dino, name, "", 0) != EXT2_OK) {
            report("  (harness) rmbig create failed\n");
            g_failures++;
            return;
        }
    }
    for (i = 0; i < 60; i++) {
        name[0] = (char)('A' + i / 26);
        name[1] = (char)('a' + i % 26);
        CHECK(ext2_unlink(g_ec, dino, name) == EXT2_OK);
    }
    CHECK(ext2_read_inode(g_ec, dino, &di) == EXT2_OK);
    ind = di.block[EXT2_IND_BLOCK];
    CHECK(ind != 0);
    nblocks = di.size / EXT2_BLOCK_SIZE;
    CHECK(nblocks > EXT2_NDIR_BLOCKS);
    lba = g_ec->base_lba + ind * 2;
    CHECK(ext2_lookup(g_ec, "/", &root) == EXT2_OK);

    /* 空判定 (ext2_is_dir_empty) は bi = 12 .. nblocks の各回で間接表を
     * 読む (最後の 1 回は「未割当 = 終わり」を知るため)。その次の 1 回が
     * ext2_free_all_blocks の下見。**下見だけ**を落とす。 */
    nth = (int)(nblocks - EXT2_NDIR_BLOCKS) + 2;
    fail_arm(lba, nth);
    rc = ext2_rmdir(g_ec, root, "rmbig");
    fail_disarm();

    CHECK(g_fail_fired == 1);                 /* 狙った 1 回に当たった */
    CHECK(rc == EXT2_ERR_IO);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/rmbig", &tmp) == EXT2_ERR_NOTFOUND);
    CHECK(inode_in_use(dino) == 1);
    CHECK(block_in_use(ind) == 1);
    CHECK(block_in_use(di.block[0]) == 1);
}

/* 書き込み系の最後の ext2_sync の失敗を捨てない (P1-5 と同じ形の洗い出し)。
 *
 * ext2_sync は空き数・グループ記述子を書き戻す。この FS の約束は
 * **「戻った時点でディスクが正しい」(write-through)** なので、書き戻せ
 * なかったのに成功と言うのはその約束の嘘になる。
 *
 * ext2_write_super_raw はスーパーブロック (ブロック 1) を read-modify-write
 * するので、その**読み出し**を落とせば sync を失敗させられる。 */
static void case_trailing_sync_failure(void)
{
    u32 sb_lba, etc_ino = 0, tmp = 0;
    int rc;

    report("  [SYNC] create / write / mkdir / rename / 追記 が sync 失敗を返す\n");

    sb_lba = g_ec->base_lba + 1 * 2;
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc", &etc_ino) == EXT2_OK);

    /* create (新しい inode とブロックを割り当てる = 書き戻すものがある) */
    fail_arm_always(sb_lba);
    rc = ext2_create(g_ec, etc_ino, "sync1", "abc", 3);
    fail_disarm();
    CHECK(g_fail_fired > 0);
    CHECK(rc == EXT2_ERR_IO);

    /* write (切り詰めて書き直す = 解放と割り当て) */
    fail_arm_always(sb_lba);
    rc = ext2_vfs_write(g_ec, "/etc/sync1", "defgh", 5);
    fail_disarm();
    CHECK(g_fail_fired > 0);
    CHECK(rc == VFS_ERR_IO);

    /* mkdir */
    fail_arm_always(sb_lba);
    rc = ext2_mkdir(g_ec, etc_ino, "syncdir");
    fail_disarm();
    CHECK(g_fail_fired > 0);
    CHECK(rc == EXT2_ERR_IO);

    /* 追記でブロックを新しく割り当てる (P1-5 の sync 側) */
    {
        static u8 blk[1024];
        kmemset(blk, 'Z', sizeof(blk));
        fail_arm_always(sb_lba);
        rc = ext2_vfs_write_stream(g_ec, "/etc/sync1", blk, sizeof(blk), 5);
        fail_disarm();
        CHECK(g_fail_fired > 0);
        CHECK(rc == VFS_ERR_IO);
    }

    /* rename はそれ自体では空き数を動かさないので、書き戻し待ちが
     * 残っている状態 (前の操作の書き戻しが失敗した直後と同じ) を作る */
    ext2_meta_touch(g_ec);
    fail_arm_always(sb_lba);
    rc = ext2_rename(g_ec, etc_ino, "sync1", etc_ino, "sync2");
    fail_disarm();
    CHECK(g_fail_fired > 0);
    CHECK(rc == EXT2_ERR_IO);

    /* 回帰: 読めるなら全部通る */
    CHECK(ext2_sync(g_ec) == EXT2_OK);
    memo_cold();
    CHECK(ext2_lookup(g_ec, "/etc/sync2", &tmp) == EXT2_OK);
    CHECK(ext2_create(g_ec, etc_ino, "sync3", "x", 1) == EXT2_OK);
    CHECK(ext2_mkdir(g_ec, etc_ino, "syncdir2") == EXT2_OK);
    CHECK(ext2_rename(g_ec, etc_ino, "sync3", etc_ino, "sync4") == EXT2_OK);
}

/* 正常系の回帰 — 直した結果ふつうの使い方が壊れていないこと */
static void case_normal_paths(void)
{
    int fd;
    u32 sz = 0;

    report("  [A4] 正常系の回帰\n");

    memo_cold();
    fd = vfs_open("/big/keepme", O_RDONLY);
    CHECK(fd >= 3);                                    /* 通常ファイルは開ける */
    if (fd >= 0) vfs_close(fd);
    CHECK(keep_intact());

    CHECK(vfs_open("/etc", O_RDONLY) == VFS_ERR_ISDIR);        /* ディレクトリ */
    CHECK(vfs_open("/big", O_WRONLY | O_CREAT) == VFS_ERR_ISDIR);

    /* 本当に不存在なら O_CREAT で作れる */
    CHECK(vfs_open("/etc/brandnew", O_RDONLY) == VFS_ERR_NOTFOUND);
    fd = vfs_open("/etc/brandnew", O_WRONLY | O_CREAT);
    CHECK(fd >= 3);
    if (fd >= 0) vfs_close(fd);
    CHECK(ext2_vfs_get_size(g_ec, "/etc/brandnew", &sz) == VFS_OK);
    CHECK(sz == 0);

    /* O_TRUNC は本当に切り詰める */
    CHECK(ext2_vfs_write(g_ec, "/etc/trunc", "0123456789", 10) == VFS_OK);
    CHECK(ext2_vfs_get_size(g_ec, "/etc/trunc", &sz) == VFS_OK);
    CHECK(sz == 10);
    fd = vfs_open("/etc/trunc", O_WRONLY | O_TRUNC);
    CHECK(fd >= 3);
    if (fd >= 0) vfs_close(fd);
    CHECK(ext2_vfs_get_size(g_ec, "/etc/trunc", &sz) == VFS_OK);
    CHECK(sz == 0);

    /* O_CREAT ありでも、既存ファイルは中身を保ったまま開ける */
    CHECK(keep_intact());
    fd = vfs_open("/big/keepme", O_WRONLY | O_CREAT);
    CHECK(fd >= 3);
    if (fd >= 0) vfs_close(fd);
    CHECK(keep_intact());

    /* ディレクトリはサイズ取得にも答えない (③) */
    CHECK(ext2_vfs_get_size(g_ec, "/etc", &sz) == VFS_ERR_ISDIR);
    CHECK(vfs_path_kind("/etc") == VFS_KIND_DIR);
}

/* ext2_bmap の区別そのもの — 未割当は EXT2_OK + 0、読めなければ EXT2_ERR_IO */
static void case_bmap_contract(void)
{
    Ext2Inode dir;
    u32 ino = 0, phys = 12345;
    u32 lba;

    report("  [A5] ext2_bmap の約束 (未割当と I/O エラーを分ける)\n");

    memo_cold();
    CHECK(ext2_lookup(g_ec, "/big", &ino) == EXT2_OK);
    CHECK(ext2_read_inode(g_ec, ino, &dir) == EXT2_OK);

    /* 未割当: ずっと先の論理ブロックは EXT2_OK で 0 */
    phys = 12345;
    CHECK(ext2_bmap(g_ec, &dir, 60000, &phys) == EXT2_OK);
    CHECK(phys == 0);

    /* 割当済み: 直接ブロック 0 は非 0 */
    phys = 0;
    CHECK(ext2_bmap(g_ec, &dir, 0, &phys) == EXT2_OK);
    CHECK(phys != 0);

    /* 間接ブロックが読めない: EXT2_ERR_IO で、0 (未割当) と区別できる */
    lba = g_ec->base_lba + dir.block[EXT2_IND_BLOCK] * 2;
    phys = 12345;
    fail_arm(lba, 1);
    CHECK(ext2_bmap(g_ec, &dir, EXT2_NDIR_BLOCKS, &phys) == EXT2_ERR_IO);
    fail_disarm();
    CHECK(g_fail_fired == 1);

    /* 同じ経路が find_entry / lookup / stat / path_kind まで畳まれずに届く */
    {
        OS32_Stat st;
        u32 out = 0;
        Ext2Inode big;
        CHECK(ext2_read_inode(g_ec, ino, &big) == EXT2_OK);

        memo_cold();
        fail_arm(lba, 1);
        CHECK(ext2_find_entry(g_ec, ino, "keepme", &out, (u8 *)0) == EXT2_ERR_IO);
        fail_disarm();

        memo_cold();
        fail_arm(lba, 1);
        CHECK(ext2_lookup(g_ec, "/big/keepme", &out) == EXT2_ERR_IO);
        fail_disarm();

        memo_cold();
        fail_arm(lba, 1);
        CHECK(ext2_vfs_stat(g_ec, "/big/keepme", &st) == VFS_ERR_IO);
        fail_disarm();

        memo_cold();
        fail_arm(lba, 1);
        CHECK(vfs_path_kind("/big/keepme") == VFS_ERR_IO);
        fail_disarm();

        /* ext2_vfs_get_size も NOTFOUND に畳まない */
        memo_cold();
        fail_arm(lba, 1);
        CHECK(ext2_vfs_get_size(g_ec, "/big/keepme", &out) == VFS_ERR_IO);
        fail_disarm();
    }
}

static void stage_a(void)
{
    report("== 段 A: 実物の ext2 (RAM ディスク) + 実物の vfs_open ==\n");
    disk_setup();
    if (g_failures) { disk_teardown(); return; }

    case_bmap_contract();

    /* O_CREAT あり / なし x O_TRUNC あり / なし の 4 通り */
    case_indirect_read_failure(O_RDONLY, "間接ブロック失敗 / O_CREAT 無 O_TRUNC 無");
    case_indirect_read_failure(O_WRONLY | O_CREAT,
                               "間接ブロック失敗 / O_CREAT 有 O_TRUNC 無");
    case_indirect_read_failure(O_WRONLY | O_TRUNC,
                               "間接ブロック失敗 / O_CREAT 無 O_TRUNC 有");
    case_indirect_read_failure(O_WRONLY | O_CREAT | O_TRUNC,
                               "間接ブロック失敗 / O_CREAT 有 O_TRUNC 有");

    case_size_failure(O_RDONLY, "サイズ取得失敗 / O_CREAT 無 O_TRUNC 無");
    case_size_failure(O_WRONLY | O_CREAT,
                      "サイズ取得失敗 / O_CREAT 有 O_TRUNC 無 (**無言のデータ消失**)");
    case_size_failure(O_WRONLY | O_TRUNC,
                      "サイズ取得失敗 / O_CREAT 無 O_TRUNC 有");
    case_size_failure(O_WRONLY | O_CREAT | O_TRUNC,
                      "サイズ取得失敗 / O_CREAT 有 O_TRUNC 有");

    case_sqlite_size_failure();
    case_write_file_failure();
    case_write_stream_failure();
    case_other_bmap_callers();
    case_mkdir_existence_failure();
    case_create_existence_failure();
    case_rename_dest_failure();
    case_write_stream_inode_failure();
    case_free_all_blocks_failure();
    case_unlink_keeps_inode();
    case_rmdir_keeps_inode();
    case_trailing_sync_failure();
    case_normal_paths();

    disk_teardown();
}

/* ======================================================================== */
/*  段 B: 合成 VfsOps — write_file の**呼び出し回数**で押さえる             */
/* ======================================================================== */

static int  s_stat_rc;
static int  s_stat_is_dir;
static int  s_size_rc;
static int  s_size_calls;
static int  s_write_rc;
static int  s_write_calls;
static u32  s_size_value;
/* 合成 FS が持つ「中身」。O_TRUNC / 作成が本当に走ったかを長さで見る。 */
static u32  s_content_len;

static int s_stat(void *ctx, const char *path, OS32_Stat *buf)
{
    (void)ctx; (void)path;
    kmemset(buf, 0, sizeof(OS32_Stat));
    if (s_stat_rc != VFS_OK) return s_stat_rc;
    buf->st_mode = (u16)(s_stat_is_dir ? (OS_S_IFDIR | 0755) : (OS_S_IFREG | 0644));
    buf->st_size = s_content_len;
    return VFS_OK;
}

static int s_get_file_size(void *ctx, const char *path, u32 *size)
{
    (void)ctx; (void)path;
    s_size_calls++;
    if (s_size_rc != VFS_OK) return s_size_rc;
    *size = s_size_value;
    return VFS_OK;
}

static int s_write_file(void *ctx, const char *path, const void *data, u32 size)
{
    (void)ctx; (void)path; (void)data;
    s_write_calls++;
    if (s_write_rc != VFS_OK) return s_write_rc;
    s_content_len = size;                 /* 0 バイト書き込み = 中身が消える */
    return VFS_OK;
}

static int s_ctx_tag = 1;
static void *s_mount(int dev) { (void)dev; return &s_ctx_tag; }
static void s_umount(void *ctx) { (void)ctx; }
static int s_is_mounted(void *ctx) { (void)ctx; return 1; }

static VfsOps s_ops;

static void synth_reset(void)
{
    vfs_tables_reset();

    s_stat_rc = VFS_OK;
    s_stat_is_dir = 0;
    s_size_rc = VFS_OK;
    s_size_calls = 0;
    s_write_rc = VFS_OK;
    s_write_calls = 0;
    s_size_value = 16;
    s_content_len = 16;

    kmemset(&s_ops, 0, sizeof(s_ops));
    s_ops.name = "synth";
    s_ops.mount = s_mount;
    s_ops.umount = s_umount;
    s_ops.is_mounted = s_is_mounted;
    s_ops.get_file_size = s_get_file_size;
    s_ops.write_file = s_write_file;
    s_ops.stat = s_stat;

    vfs_register_fs(&s_ops);
    if (vfs_mount("/synth", "hostdrv", "synth") != VFS_OK) {
        report("  (harness) synth vfs_mount failed\n");
        g_failures++;
    }
}

/* 「通常ファイルと確認済み → サイズ取得だけが一度失敗」の 4 通り。
 * ここでは write_file の**呼び出し回数**をそのまま数えられる。 */
static void synth_size_failure(int mode, int size_rc, const char *label)
{
    int fd;

    report("  [B1] "); report(label); report("\n");

    synth_reset();
    s_stat_rc = VFS_OK;
    s_stat_is_dir = 0;                    /* 種別は「通常ファイル」で確定 */
    s_size_rc = size_rc;                  /* サイズ取得だけが失敗する */

    fd = vfs_open("/synth/f", mode);

    CHECK(fd < 0);                        /* FD を発行しない */
    CHECK(fd == size_rc);                 /* そのエラーをそのまま返す */
    CHECK(s_write_calls == 0);            /* **write_file を呼ばない** */
    CHECK(s_content_len == 16);           /* 中身が残っている */
    CHECK(any_fd_open() == 0);
}

static void synth_trunc_write_failure(void)
{
    int fd;

    report("  [B2] O_TRUNC の write_file が失敗したら open しない\n");

    synth_reset();
    s_write_rc = VFS_ERR_ISDIR;           /* ext2 の一括書き込みが持つ拒否 */
    fd = vfs_open("/synth/f", O_WRONLY | O_TRUNC);
    CHECK(fd == VFS_ERR_ISDIR);           /* 失敗をそのまま返す */
    CHECK(s_write_calls == 1);
    CHECK(any_fd_open() == 0);

    report("  [B3] O_CREAT の write_file が失敗したら open しない\n");
    synth_reset();
    s_size_rc = VFS_ERR_NOTFOUND;
    s_stat_rc = VFS_ERR_NOTFOUND;
    s_write_rc = VFS_ERR_NOSPC;
    fd = vfs_open("/synth/new", O_WRONLY | O_CREAT);
    CHECK(fd == VFS_ERR_NOSPC);
    CHECK(s_write_calls == 1);
    CHECK(any_fd_open() == 0);
}

static void synth_sqlite(void)
{
    VfsSqliteCookie ck;
    VfsSqliteLease lease;
    int rc;

    report("  [B4] vfs_open_sqlite も作成へ進まない\n");

    ck.group_index = 0;
    ck.generation = 1;

    synth_reset();
    s_size_rc = VFS_ERR_IO;
    kmemset(&lease, 0, sizeof(lease));
    rc = vfs_open_sqlite("/synth/f", O_RDWR | O_CREAT, 0, &ck, 0, &lease);
    CHECK(rc == VFS_ERR_IO);
    CHECK(s_write_calls == 0);
    CHECK(s_content_len == 16);
    CHECK(any_fd_open() == 0);

    /* 本当に不存在なら従来どおり作れる */
    synth_reset();
    s_stat_rc = VFS_ERR_NOTFOUND;
    s_size_rc = VFS_ERR_NOTFOUND;
    kmemset(&lease, 0, sizeof(lease));
    rc = vfs_open_sqlite("/synth/new", O_RDWR | O_CREAT, 0, &ck, 0, &lease);
    CHECK(rc == VFS_OK);
    CHECK(s_write_calls == 1);
    CHECK(lease.fd >= 3);
}

static void synth_normal(void)
{
    int fd;

    report("  [B5] 合成ドライバでの正常系\n");

    synth_reset();
    fd = vfs_open("/synth/f", O_RDONLY);
    CHECK(fd >= 3);
    CHECK(s_write_calls == 0);

    synth_reset();
    s_stat_rc = VFS_ERR_NOTFOUND;
    s_size_rc = VFS_ERR_NOTFOUND;
    fd = vfs_open("/synth/new", O_WRONLY | O_CREAT);
    CHECK(fd >= 3);
    CHECK(s_write_calls == 1);            /* 空ファイルを 1 度書いて作る */

    synth_reset();
    s_stat_rc = VFS_ERR_NOTFOUND;
    s_size_rc = VFS_ERR_NOTFOUND;
    CHECK(vfs_open("/synth/new", O_RDONLY) == VFS_ERR_NOTFOUND);
    CHECK(s_write_calls == 0);

    synth_reset();
    fd = vfs_open("/synth/f", O_WRONLY | O_TRUNC);
    CHECK(fd >= 3);
    CHECK(s_write_calls == 1);
    CHECK(s_content_len == 0);            /* 本当に切り詰まる */

    synth_reset();
    s_stat_is_dir = 1;
    CHECK(vfs_open("/synth/d", O_WRONLY | O_CREAT | O_TRUNC) == VFS_ERR_ISDIR);
    CHECK(s_write_calls == 0);

    /* サイズ取得がディレクトリを断ったら、それは「無い」ではなく DIR
     * (stat を持たないドライバのプローブ、③ の受け手) */
    synth_reset();
    s_ops.stat = 0;
    vfs_tables_reset();
    vfs_register_fs(&s_ops);
    CHECK(vfs_mount("/synth", "hostdrv", "synth") == VFS_OK);
    s_size_rc = VFS_ERR_ISDIR;
    CHECK(vfs_path_kind("/synth/d") == VFS_KIND_DIR);
    CHECK(vfs_open("/synth/d", O_WRONLY | O_CREAT) == VFS_ERR_ISDIR);
    CHECK(s_write_calls == 0);
}

/* HostDrv の get_file_size が「サイズと名乗ってよいか」(③)。
 * NP21/W の HostDrv は NON_DIRECTORY_FILE を付けずに開くので**ディレクトリでも
 * 成功し、NT はディレクトリにもサイズを返す**。 */
static void stage_c(void)
{
    report("== 段 C: HostDrv のサイズ取得の純規則 (fs/hostdrv_stat_rules.inc) ==\n");
    report("  [C1] hdrv_size_result\n");

    /* 通常ファイル: 問い合わせが通っていればサイズを返してよい */
    CHECK(hdrv_size_result(0, 0) == 0);

    /* **ディレクトリには答えない** */
    CHECK(hdrv_size_result(0, 1) == OS32_ERR_ISDIR);
    CHECK(hdrv_size_result(0, 1) != 0);

    /* 問い合わせが通っていない = 種別もサイズも分からない。
     * 「サイズ 0 の通常ファイル」と名乗らない。 */
    CHECK(hdrv_size_result(-1, 0) == OS32_ERR_IO);
    CHECK(hdrv_size_result(-1, 1) == OS32_ERR_IO);
    CHECK(hdrv_size_result(1, 0) == OS32_ERR_IO);

    /* NP21/W は Directory を FileStandardInformation に埋める
     * (np21w-src/src/generic/hostdrvnt.c)。値は 0 / 1 だが、
     * 非 0 ならディレクトリとして扱う。 */
    CHECK(hdrv_size_result(0, 2) == OS32_ERR_ISDIR);
}

static void stage_b(void)
{
    report("== 段 B: 合成 VfsOps (write_file の呼び出し回数で押さえる) ==\n");

    synth_size_failure(O_RDONLY, VFS_ERR_IO,
                       "O_CREAT 無 O_TRUNC 無 / サイズ取得 IO");
    synth_size_failure(O_WRONLY | O_CREAT, VFS_ERR_IO,
                       "O_CREAT 有 O_TRUNC 無 / サイズ取得 IO (**最重要**)");
    synth_size_failure(O_WRONLY | O_TRUNC, VFS_ERR_IO,
                       "O_CREAT 無 O_TRUNC 有 / サイズ取得 IO");
    synth_size_failure(O_WRONLY | O_CREAT | O_TRUNC, VFS_ERR_IO,
                       "O_CREAT 有 O_TRUNC 有 / サイズ取得 IO");
    /* NOTFOUND 以外なら何でも同じ — 「無い」以外は作らない */
    synth_size_failure(O_WRONLY | O_CREAT, VFS_ERR_NOSPC,
                       "O_CREAT 有 / サイズ取得 NOSPC");
    synth_size_failure(O_WRONLY | O_CREAT, VFS_ERR_NOTDIR,
                       "O_CREAT 有 / サイズ取得 NOTDIR");
    synth_size_failure(O_WRONLY | O_CREAT, VFS_ERR_INVAL,
                       "O_CREAT 有 / サイズ取得 INVAL");

    synth_trunc_write_failure();
    synth_sqlite();
    synth_normal();
}

/* ======================================================================== */
/*  入口                                                                    */
/* ======================================================================== */

static void run(void)
{
    report("=== 票 B8: 読み取り失敗を「不存在」にしない (vfs_open まで) ===\n");
    stage_a();
    stage_b();
    stage_c();

    report("\n");
    report_i(g_checks);
    report(" checks, ");
    report_i(g_failures);
    report(" failures\n");
    g_exit_code = g_failures ? 1 : 0;
}

/* -nostdlib の入口。ext2_write_io_host.c と同じ様式 (スタックを整えて run へ) */
__asm__(
    ".globl _start\n"
    "_start:\n"
    "    xor %ebp, %ebp\n"
    "    and $-16, %esp\n"
    "    call b8_main\n"
    "    hlt\n");

void b8_main(void);
void b8_main(void)
{
    run();
    die(g_exit_code);
}
