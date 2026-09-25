/* ========================================================================
 *  cd_read_host.c — CD の読み (drivers/atapi.c + fs/iso9660.c) のホスト試験
 *
 *  実行:  python3 -B tools/tests/test_cd_read.py [--target] [--mutate] [case ...]
 *  記録:  tools/tests/cd_read_tdd.md
 *
 *  実物の drivers/atapi.c と fs/iso9660.c を 1 行も写さずに #include する。
 *  atapi.c のポートは tools/tests/atapi_hostshim/io.h がここの ATAPI デバイスの
 *  模型へ回す。模型は PACKET (0xA0) → 12 バイトの CDB → DRQ ごとのデータ、を
 *  レジスタの粒度で写し、READ(10) の回数・セクタ数・(LBA, 数) の列を数える。
 *  iso9660.c の下の dev 層 (dev_find / dev_blk_read_lba) は cd0 だけの贋物で、
 *  そのまま atapi_read_sectors へ渡す (drivers/dev.c の cd0_read と同じ)。
 *  媒体は試験が組む ISO 9660 のイメージ (PVD・根・SUB・ファイル)。
 *
 *  [C1] C89 / GNU89。
 * ======================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfs.h"
#include "dev.h"
#include "kmalloc.h"
#include "lib/kstring.h"

/* ---- 境界の贋物 (kstring / kmalloc / tick) ---- */
volatile u32 tick_count;
static int g_kmalloc_fail;   /* 1 なら kmalloc (先読みの窓) を断る */

u32 kstrlen(const char *s) { return (u32)strlen(s); }
int kstrcmp(const char *a, const char *b) { return strcmp(a, b); }
char *kstrncpy(char *dst, const char *src, u32 n)
{
    /* n はバッファ全体の大きさ (lib/kstring.h の注記) */
    if (n == 0) return dst;
    strncpy(dst, src, (size_t)n - 1);
    dst[n - 1] = '\0';
    return dst;
}
void *kmemcpy(void *dst, const void *src, u32 n) { return memcpy(dst, src, (size_t)n); }
void *kmalloc(u32 size) { return g_kmalloc_fail ? (void *)0 : malloc((size_t)size); }
void *kzalloc(u32 size) { return calloc(1, (size_t)size); }
void kfree(void *p) { free(p); }

/* ---- 実物 ---- */
#include "../../drivers/atapi.c"
#include "../../fs/iso9660.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); \
} } while (0)

/* ======================================================================== */
/*  ISO 9660 のイメージ                                                      */
/* ======================================================================== */

#define SEC        2048u
#define ROOT_LBA   20u
#define SUB_LBA    21u        /* SUB は 2 セクタ (21, 22) */
#define SUB_LBA_B  24u        /* 媒体 B の SUB (24, 25)。B の 21, 22 には囮の SUB */
#define TINY_LBA   30u
#define DEEP_LBA   32u
#define BIG_LBA_A  40u        /* 媒体 A の BIG.PKG */
#define BIG_LBA_B  45u        /* 媒体 B (入れ替え後) の BIG.PKG */
#define TINY_SIZE  100u
#define DEEP_SIZE  5000u
#define DEEP_SIZE_B 6000u     /* 媒体 B の DEEP.BIN */
/* 媒体 B の LBA 21, 22 は囮の SUB で、2 セクタ目は空 (DEEP.BIN が無い)。
 * 2 セクタ目を読んでも cb が呼ばれないので、読んだ後の世代の確認だけが頼り */
#define BIG_SIZE_DEFAULT (600u * SEC + 777u)   /* 601 セクタ、末尾は半端 */

static u32 g_big_size = BIG_SIZE_DEFAULT;

typedef struct {
    u8  *img;
    u32  secs;
    u32  big_lba;
    u32  sub_lba;
    u32  deep_size;
    u8   salt;      /* 中身の模様を媒体ごとに変える */
} Media;

static Media g_media_a, g_media_b;

static u8 pattern(u8 salt, u32 lba, u32 off)
{
    u32 v = lba * 2654435761u + off * 40503u + (off >> 9) + salt * 97u;
    return (u8)(v ^ (v >> 13) ^ (v >> 24));
}

static void put_both32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 24); p[5] = (u8)(v >> 16); p[6] = (u8)(v >> 8); p[7] = (u8)v;
}

/* ディレクトリレコードを 1 つ書き、長さを返す */
static u32 put_rec(u8 *p, u32 lba, u32 size, u8 flags, const char *name, int nlen)
{
    u32 len = 33u + (u32)nlen;
    if (len & 1u) len++;
    memset(p, 0, len);
    p[0] = (u8)len;
    put_both32(p + 2, lba);
    put_both32(p + 10, size);
    p[25] = flags;
    p[28] = 1; p[31] = 1;          /* volume sequence number */
    p[32] = (u8)nlen;
    memcpy(p + 33, name, (size_t)nlen);
    return len;
}

/* 2 セクタの SUB を at に書く (記録上の自分の位置は self)。1 セクタ目は詰め物、
 * DEEP.BIN (deep_size) は 2 セクタ目。deep_size = 0 なら 2 セクタ目は空 */
static void put_sub(u8 *img, u32 at, u32 self, u32 deep_size)
{
    u8 *d = img + at * SEC;
    u32 o = 0, i;
    memset(d, 0, 2u * SEC);
    o += put_rec(d + o, self, 2u * SEC, ISO_FLAG_DIRECTORY, "\0", 1);
    o += put_rec(d + o, ROOT_LBA, SEC, ISO_FLAG_DIRECTORY, "\1", 1);
    for (i = 0; o + 48u <= SEC; i++) {
        char nm[16];
        sprintf(nm, "F%04u.TXT;1", (unsigned)i);
        o += put_rec(d + o, TINY_LBA, TINY_SIZE, 0, nm, 11);
    }
    if (deep_size) put_rec(d + SEC, DEEP_LBA, deep_size, 0, "DEEP.BIN;1", 10);
}

static void build_media(Media *m, u32 big_lba, u32 sub_lba, u32 deep_size, u8 salt)
{
    u32 big_secs = (g_big_size + SEC - 1) / SEC;
    u8 *d;
    u32 o, i, k;

    m->big_lba = big_lba;
    m->sub_lba = sub_lba;
    m->deep_size = deep_size;
    m->salt = salt;
    m->secs = big_lba + big_secs;    /* BIG.PKG が媒体の最後 (その先は読めない) */
    m->img = (u8 *)calloc(m->secs, SEC);
    CHECK(m->img != NULL);

    /* データ (全セクタに模様。ファイルの中身もこの模様の一部) */
    for (i = 0; i < m->secs; i++)
        for (k = 0; k < SEC; k++) m->img[i * SEC + k] = pattern(salt, i, k);

    /* PVD (LBA 16) */
    d = m->img + 16u * SEC;
    memset(d, 0, SEC);
    d[0] = 1; memcpy(d + 1, "CD001", 5); d[6] = 1;
    put_both32(d + 80, m->secs);
    d[128] = (u8)(SEC & 0xFF); d[129] = (u8)(SEC >> 8);
    put_rec(d + 156, ROOT_LBA, SEC, ISO_FLAG_DIRECTORY, "\0", 1);

    /* 根 */
    d = m->img + ROOT_LBA * SEC;
    memset(d, 0, SEC);
    o = 0;
    o += put_rec(d + o, ROOT_LBA, SEC, ISO_FLAG_DIRECTORY, "\0", 1);
    o += put_rec(d + o, ROOT_LBA, SEC, ISO_FLAG_DIRECTORY, "\1", 1);
    o += put_rec(d + o, big_lba, g_big_size, 0, "BIG.PKG;1", 9);
    o += put_rec(d + o, sub_lba, 2u * SEC, ISO_FLAG_DIRECTORY, "SUB", 3);
    o += put_rec(d + o, TINY_LBA, TINY_SIZE, 0, "TINY.TXT;1", 10);

    /* SUB。SUB が 21 でない媒体では、21, 22 に中身の違う囮の SUB を置く
     * (旧媒体の根で得た LBA 21 を新媒体で読むと、別の答えになる) */
    put_sub(m->img, sub_lba, sub_lba, deep_size);
    if (sub_lba != SUB_LBA) put_sub(m->img, SUB_LBA, SUB_LBA, 0);
}

/* 媒体 m の LBA lba から始まるファイルの off バイト目 */
static u8 file_byte(const Media *m, u32 lba, u32 off)
{
    return m->img[lba * SEC + off];
}

/* ======================================================================== */
/*  ATAPI デバイスの模型                                                    */
/* ======================================================================== */

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01
#define LOG_MAX 8192

static struct {
    Media *m;          /* いま入っている媒体 */
    int bank;
    u8  status;
    u8  err;
    u16 bcl;           /* ホストが書いた byte count limit */
    int phase;         /* 0 = 待ち, 1 = CDB, 2 = データ */
    u8  cdb[12];
    int cdb_words;
    const u8 *data;
    u32 data_len;
    u32 pos;
    u32 blk_end;
    u32 blk_size;
    int lag;           /* ブロックの境目の後、古い状態を見せる回数 */
    u8  lag_status;
    u8  cap[8];
    /* 設定 */
    u32 drq_max;       /* 1 回の DRQ の上限 (0 = bcl どおり) */
    int fail_multi;    /* count > 1 の READ(10) を MEDIUM ERROR で断る */
    long fail_lba;     /* この LBA を含む READ(10) を断る (-1 = なし) */
    int short_multi;   /* count > 1 の READ(10) で 1 セクタ少なく渡す */
    int ua_next;       /* 次のコマンドで UNIT ATTENTION を返し、媒体を B へ */
    int stale;         /* 境目の後の古い状態の回数 */
    /* 数 */
    u32 n_packets;
    u32 n_read10;
    u32 n_read10_multi;
    u32 n_sectors;
    u32 max_count;
    u32 max_bcl;
    int bcl_odd;
    u32 log_lba[LOG_MAX];
    u32 log_cnt[LOG_MAX];
    u32 n_log;
} M;

static void model_reset_counts(void)
{
    M.n_packets = M.n_read10 = M.n_read10_multi = M.n_sectors = 0;
    M.max_count = M.max_bcl = 0;
    M.bcl_odd = 0;
    M.n_log = 0;
}

static u32 lba_reads(u32 lba)   /* lba を含む READ(10) の数 */
{
    u32 i, n = 0;
    for (i = 0; i < M.n_log; i++)
        if (lba >= M.log_lba[i] && lba - M.log_lba[i] < M.log_cnt[i]) n++;
    return n;
}

static void model_error(u8 sense)
{
    M.status = ST_DRDY | ST_ERR;
    M.err = (u8)(sense << 4);
    M.phase = 0;
}

static void model_start_block(void)
{
    u32 limit = M.bcl & ~1u;
    u32 left = M.data_len - M.pos;
    if (M.drq_max && M.drq_max < limit) limit = M.drq_max;
    if (limit == 0) limit = 2;
    M.blk_size = (left < limit) ? left : limit;
    M.blk_end = M.pos + M.blk_size;
    M.status = ST_DRDY | ST_DRQ;
    M.phase = 2;
}

static void model_exec(void)
{
    u8 op = M.cdb[0];
    M.err = 0;
    if (M.ua_next) {
        M.ua_next = 0;
        M.m = &g_media_b;
        model_error(ATAPI_SK_UNIT_ATTENTION);
        if (op == SCSI_CMD_READ_10) M.n_read10++;
        return;
    }
    if (op == SCSI_CMD_READ_10) {
        u32 lba = ((u32)M.cdb[2] << 24) | ((u32)M.cdb[3] << 16)
                | ((u32)M.cdb[4] << 8) | M.cdb[5];
        u32 cnt = ((u32)M.cdb[7] << 8) | M.cdb[8];
        M.n_read10++;
        if (cnt > 1) M.n_read10_multi++;
        if (cnt > M.max_count) M.max_count = cnt;
        if (M.n_log < LOG_MAX) {
            M.log_lba[M.n_log] = lba;
            M.log_cnt[M.n_log] = cnt;
            M.n_log++;
        }
        if (cnt == 0 || lba + cnt > M.m->secs
            || (M.fail_multi && cnt > 1)
            || (M.fail_lba >= 0 && (u32)M.fail_lba >= lba
                && (u32)M.fail_lba - lba < cnt)) {
            model_error(0x03);   /* MEDIUM ERROR */
            return;
        }
        M.data = M.m->img + lba * SEC;
        M.data_len = cnt * SEC;
        if (M.short_multi && cnt > 1) M.data_len -= SEC;
        M.n_sectors += M.data_len / SEC;
        M.pos = 0;
        model_start_block();
        return;
    }
    if (op == SCSI_CMD_READ_CAPACITY) {
        u32 last = M.m->secs - 1;
        M.cap[0] = (u8)(last >> 24); M.cap[1] = (u8)(last >> 16);
        M.cap[2] = (u8)(last >> 8);  M.cap[3] = (u8)last;
        M.cap[4] = 0; M.cap[5] = 0; M.cap[6] = (u8)(SEC >> 8); M.cap[7] = 0;
        M.data = M.cap;
        M.data_len = 8;
        M.pos = 0;
        model_start_block();
        return;
    }
    if (op == SCSI_CMD_TEST_UNIT_READY) {
        M.status = ST_DRDY;
        M.phase = 0;
        return;
    }
    model_error(0x05);   /* ILLEGAL REQUEST */
}

static u8 model_status(void)
{
    if (M.lag > 0) { M.lag--; return M.lag_status; }
    return M.status;
}

unsigned int atapi_shim_inp(unsigned int port)
{
    switch (port) {
    case IDE_STATUS:
    case IDE_ALT_STATUS: return model_status();
    case IDE_ERROR:      return M.err;
    case IDE_CYL_LO:     return (M.phase == 2) ? (M.blk_size & 0xFF) : (M.bcl & 0xFF);
    case IDE_CYL_HI:     return (M.phase == 2) ? ((M.blk_size >> 8) & 0xFF) : (M.bcl >> 8);
    case IDE_SECT_CNT:   return (M.phase == 2) ? ATAPI_IR_IO : (ATAPI_IR_IO | ATAPI_IR_CD);
    default:             return 0;
    }
}

void atapi_shim_outp(unsigned int port, unsigned int value)
{
    switch (port) {
    case IDE_BANK1:  M.bank = (int)(value & 1); break;
    case IDE_CYL_LO: M.bcl = (u16)((M.bcl & 0xFF00) | (value & 0xFF)); break;
    case IDE_CYL_HI: M.bcl = (u16)((M.bcl & 0x00FF) | ((value & 0xFF) << 8)); break;
    case IDE_COMMAND:
        if (value == ATAPI_CMD_PACKET) {
            CHECK(M.bank == 1);            /* セカンダリを選んでから */
            if (M.bcl > M.max_bcl) M.max_bcl = M.bcl;
            if (M.bcl & 1) M.bcl_odd = 1;
            CHECK(M.bcl != 0);             /* 0 は規定外 (16 ビットに入らない値を書いた) */
            M.n_packets++;
            M.phase = 1;
            M.cdb_words = 0;
            M.status = ST_DRDY | ST_DRQ;
        }
        break;
    default: break;
    }
}

unsigned int atapi_shim_inpw(unsigned int port)
{
    unsigned int w;
    if (port != IDE_DATA) return 0xFFFF;
    /* 境目の直後 (デバイスがまだ切り替えている間) や、データの外は屑 */
    if (M.lag > 0 || M.phase != 2 || M.pos >= M.blk_end) return 0xFFFF;
    w = (unsigned int)M.data[M.pos];
    w |= (M.pos + 1 < M.data_len) ? ((unsigned int)M.data[M.pos + 1] << 8) : 0;
    M.pos += 2;
    if (M.pos > M.data_len) M.pos = M.data_len;
    if (M.pos >= M.blk_end) {
        /* ブロックの終わり。しばらく古い状態 (DRQ) を見せる */
        M.lag = M.stale;
        M.lag_status = M.status;
        if (M.pos < M.data_len) {
            model_start_block();
        } else {
            M.status = ST_DRDY;
            M.phase = 0;
        }
    }
    return w;
}

void atapi_shim_outpw(unsigned int port, unsigned int value)
{
    if (port != IDE_DATA || M.phase != 1) return;
    M.cdb[M.cdb_words * 2] = (u8)value;
    M.cdb[M.cdb_words * 2 + 1] = (u8)(value >> 8);
    if (++M.cdb_words == 6) model_exec();
}

/* ======================================================================== */
/*  dev 層の贋物 (cd0 だけ)                                                 */
/* ======================================================================== */

static Device g_cd0;
static u32 g_dev_calls;

Device *dev_find(const char *name)
{
    return (strcmp(name, "cd0") == 0) ? &g_cd0 : (Device *)0;
}

int dev_blk_read_lba(Device *dev, u32 lba, int count, void *buf)
{
    CHECK(dev == &g_cd0);
    g_dev_calls++;
    return atapi_read_sectors(lba, (u32)count, buf);
}

/* ======================================================================== */
/*  共通                                                                     */
/* ======================================================================== */

static void setup(void)
{
    memset(&M, 0, sizeof(M));
    M.fail_lba = -1;
    M.stale = 3;         /* 400ns の整定を置かないと取り違える形 */
    M.bcl = 0xEB14;      /* 起動直後はシグネチャ */
    build_media(&g_media_a, BIG_LBA_A, SUB_LBA, DEEP_SIZE, 1);
    build_media(&g_media_b, BIG_LBA_B, SUB_LBA_B, DEEP_SIZE_B, 2);
    M.m = &g_media_a;
    CHECK(atapi_init() == 1);
}

static Iso9660Ctx *do_mount(void)
{
    Iso9660Ctx *c = (Iso9660Ctx *)iso9660_ops.mount(
        VFS_MOUNT_DEV_ENCODE(VFS_DEV_CD, 0));
    CHECK(c != NULL);
    model_reset_counts();
    return c;
}

static u32 big_secs(void) { return (g_big_size + SEC - 1) / SEC; }
static u32 ceil_div(u32 a, u32 b) { return (a + b - 1) / b; }

/* BIG.PKG を off から chunk ずつ末尾まで read_stream し、中身を確かめる */
static void stream_all(Iso9660Ctx *c, const char *path, const Media *m,
                       u32 start, u32 chunk)
{
    u8 *buf = (u8 *)malloc(chunk);
    u32 off = start;
    CHECK(buf != NULL);
    while (off < g_big_size) {
        int rc = iso9660_ops.read_stream(c, path, buf, chunk, off);
        u32 want = (g_big_size - off < chunk) ? g_big_size - off : chunk;
        u32 i;
        CHECK(rc == (int)want);
        for (i = 0; i < want; i++) {
            if (buf[i] != file_byte(m, m->big_lba, off + i)) {
                fprintf(stderr, "mismatch at %lu\n", (unsigned long)(off + i));
                CHECK(0);
            }
        }
        off += want;
    }
    CHECK(iso9660_ops.read_stream(c, path, buf, chunk, off) == 0);   /* EOF */
    free(buf);
}

static void report(const char *name, Iso9660Ctx *c)
{
    printf("%s: read10=%lu multi=%lu sectors=%lu walks=%lu hits=%lu ra_fill=%lu "
           "bulk=%lu sc_fill=%lu secs=%lu\n", name,
           (unsigned long)M.n_read10, (unsigned long)M.n_read10_multi,
           (unsigned long)M.n_sectors, (unsigned long)c->stats.path_walks,
           (unsigned long)c->stats.path_hits, (unsigned long)c->stats.ra_fills,
           (unsigned long)c->stats.bulk_reads, (unsigned long)c->stats.scache_fills,
           (unsigned long)big_secs());
}

/* 上限: データは N セクタにつき 1 回、メタデータ (根のセクタ 1) と
 * 区切りの端 (先頭と末尾の半端) で + 2 */
static u32 bound(void)
{
    return ceil_div(big_secs(), ATAPI_READ_MAX_SECTORS) + 3u;
}

/* ======================================================================== */
/*  試験                                                                     */
/* ======================================================================== */

/* 4KB ずつ (VFS の既定の小さな区切り) */
static void t_stream_4k(void)
{
    Iso9660Ctx *c;
    setup(); c = do_mount();
    stream_all(c, "/BIG.PKG", &g_media_a, 0, 4096);
    report("stream_4k", c);
    CHECK(c->stats.path_walks == 1);
    CHECK(M.n_read10 <= bound());
    CHECK(M.max_count == ATAPI_READ_MAX_SECTORS);
    CHECK(lba_reads(ROOT_LBA) == 1);
    CHECK(M.n_sectors <= big_secs() + 1u);   /* 同じセクタを読み直さない */
    iso9660_ops.umount(c);
}

/* 1000B ずつ、セクタに揃わない位置から */
static void t_stream_odd(void)
{
    Iso9660Ctx *c;
    setup(); c = do_mount();
    stream_all(c, "BIG.PKG", &g_media_a, 13, 1000);
    report("stream_odd", c);
    CHECK(c->stats.path_walks == 1);
    CHECK(M.n_read10 <= bound());
    CHECK(M.n_sectors <= big_secs() + 1u);
    iso9660_ops.umount(c);
}

/* cdinst (pkg.c) の形: 先頭をセクタ境界へ揃えてから 32KB ずつ */
static void t_stream_32k(void)
{
    Iso9660Ctx *c;
    u8 *buf = (u8 *)malloc(32768);
    u32 off = 1234, i;
    setup(); c = do_mount();
    while (off < g_big_size) {
        u32 room = 32768u - (off % SEC);
        u32 want = (g_big_size - off < room) ? g_big_size - off : room;
        CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, room, off) == (int)want);
        for (i = 0; i < want; i++)
            CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, off + i));
        off += want;
    }
    report("stream_32k", c);
    CHECK(c->stats.path_walks == 1);
    CHECK(M.n_read10 <= bound());
    if (ISO_RA_BYTES <= 32768u)
        CHECK(c->stats.bulk_reads > 0);        /* 窓を通さず直接 (窓 ≤ 32KB のとき) */
    free(buf);
    iso9660_ops.umount(c);
}

/* read_file で全体 */
static void t_read_file(void)
{
    Iso9660Ctx *c;
    u8 *buf = (u8 *)malloc(g_big_size + 64);
    u32 i;
    setup(); c = do_mount();
    CHECK(iso9660_ops.read_file(c, "/BIG.PKG", buf, g_big_size + 64) == (int)g_big_size);
    for (i = 0; i < g_big_size; i++)
        CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, i));
    report("read_file", c);
    CHECK(M.n_read10 <= bound());
    /* 切り詰め (max_size < size) */
    CHECK(iso9660_ops.read_file(c, "/BIG.PKG", buf, 5000) == 5000);
    for (i = 0; i < 5000; i++)
        CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, i));
    CHECK(iso9660_ops.read_file(c, "/SUB", buf, 100) == VFS_ERR_ISDIR);
    CHECK(iso9660_ops.read_file(c, "/NOPE", buf, 100) == VFS_ERR_NOTFOUND);
    free(buf);
    iso9660_ops.umount(c);
}

/* 複数セクタの READ(10) が落ちたら 1 セクタずつ読み直す */
static void t_multi_fallback(void)
{
    Iso9660Ctx *c;
    AtapiStats a0, a1;
    setup(); c = do_mount();
    M.fail_multi = 1;
    atapi_get_stats(&a0);
    stream_all(c, "/BIG.PKG", &g_media_a, 0, 4096);
    atapi_get_stats(&a1);
    report("multi_fallback", c);
    CHECK(a1.multi_fail > a0.multi_fail);
    CHECK(a1.single_retry - a0.single_retry >= big_secs() - 1u);
    CHECK(c->stats.path_walks == 1);
    iso9660_ops.umount(c);
}

/* 渡されたバイト数が足りない複数セクタの READ(10) も失敗として読み直す */
static void t_short_transfer(void)
{
    Iso9660Ctx *c;
    AtapiStats a0, a1;
    setup(); c = do_mount();
    M.short_multi = 1;
    atapi_get_stats(&a0);
    stream_all(c, "/BIG.PKG", &g_media_a, 0, 8192);
    atapi_get_stats(&a1);
    CHECK(a1.multi_fail > a0.multi_fail);
    iso9660_ops.umount(c);
}

/* 窓の先読みが要求の外の不良セクタで落ちても、要求の範囲は読める
 * (Codex レビュー 1 の P2)。窓は読む前に捨てる (途中まで上書きされた窓を
 * 前の範囲として当てない) */
static void t_bad_outside(void)
{
    Iso9660Ctx *c;
    u8 buf[4096];
    AtapiStats a0, a1;
    u32 i;
    setup(); c = do_mount();
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, sizeof(buf), 0) == (int)sizeof(buf));
    /* 要求は窓 2 枚目の頭の 2 セクタ、不良は同じ窓の 6 本目 */
    M.fail_lba = (long)(BIG_LBA_A + ISO_RA_SECTORS + 5u);
    atapi_get_stats(&a0);
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, sizeof(buf),
                                  ISO_RA_SECTORS * SEC) == (int)sizeof(buf));
    for (i = 0; i < sizeof(buf); i++)
        CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, ISO_RA_SECTORS * SEC + i));
    atapi_get_stats(&a1);
    CHECK(a1.multi_fail - a0.multi_fail == 1);
    CHECK(c->ra_valid == 0);
    /* 半端な位置から、不良の手前までは読める */
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 3000,
                                  (ISO_RA_SECTORS + 3u) * SEC + 100u) == 3000);
    for (i = 0; i < 3000; i++)
        CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A,
                                  (ISO_RA_SECTORS + 3u) * SEC + 100u + i));
    M.fail_lba = -1;
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, sizeof(buf), 0) == (int)sizeof(buf));
    for (i = 0; i < sizeof(buf); i++)
        CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, i));
    iso9660_ops.umount(c);
}

/* 要求の範囲の中の不良セクタは失敗。1 セクタずつの読み直しは不良で止まる */
static void t_bad_inside(void)
{
    Iso9660Ctx *c;
    u8 buf[4096];
    AtapiStats a0, a1;
    setup(); c = do_mount();
    M.fail_lba = (long)(BIG_LBA_A + ISO_RA_SECTORS + 1u);
    atapi_get_stats(&a0);
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, sizeof(buf),
                                  ISO_RA_SECTORS * SEC) == VFS_ERR_IO);
    atapi_get_stats(&a1);
    CHECK(a1.multi_fail - a0.multi_fail >= 1);
    /* 窓の読み直し (RA, RA+1) と要るセクタの読み直し (RA, RA+1)。不良で止まる */
    CHECK(a1.single_retry - a0.single_retry <= 4u);
    /* 大きな揃った読み (窓を通さない) の中の不良も失敗 */
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", (u8 *)malloc(65536), 65536, 0)
          == VFS_ERR_IO);
    iso9660_ops.umount(c);
}

/* LRU: いちばん長く使っていないものを追い出す */
static void t_lru_order(void)
{
    Iso9660Ctx *c;
    const u32 L[5] = { 100, 101, 102, 103, 104 };
    const u8 *p;
    u32 i;
    setup(); c = do_mount();
    for (i = 0; i < 4; i++) CHECK(iso_get_sector(c, L[i]) != NULL);
    CHECK(iso_get_sector(c, L[0]) != NULL);        /* 当たり: L0 が新しくなる */
    CHECK(lba_reads(L[0]) == 1);
    CHECK(iso_get_sector(c, L[4]) != NULL);        /* L1 が出る */
    p = iso_get_sector(c, L[0]);                   /* まだいる */
    CHECK(p != NULL && lba_reads(L[0]) == 1);
    for (i = 0; i < SEC; i++) CHECK(p[i] == g_media_a.img[L[0] * SEC + i]);
    p = iso_get_sector(c, L[2]);
    CHECK(p != NULL && lba_reads(L[2]) == 1);
    for (i = 0; i < SEC; i++) CHECK(p[i] == g_media_a.img[L[2] * SEC + i]);
    CHECK(iso_get_sector(c, L[1]) != NULL);        /* 出ていた */
    CHECK(lba_reads(L[1]) == 2);
    iso9660_ops.umount(c);
}

/* UNIT ATTENTION の直後の mount (PVD を読む 1 セクタ) も通る */
static void t_ua_mount(void)
{
    Iso9660Ctx *c;
    u8 buf[16];
    u32 gen0;
    setup();
    gen0 = atapi_media_gen();
    M.ua_next = 1;
    c = do_mount();
    CHECK(atapi_media_gen() == gen0 + 1u);
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 16, 0) == 16);
    CHECK(buf[0] == file_byte(&g_media_b, BIG_LBA_B, 0));
    iso9660_ops.umount(c);
}

/* DRQ の区切り方: 2048 ずつ (NP21/W)、bcl どおり、セクタの途中で切れる大きさ */
static void t_multi_drq(void)
{
    static const u32 sizes[] = { 2048, 0, 6144, 1000, 512 };
    unsigned i;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        Iso9660Ctx *c;
        setup(); c = do_mount();
        M.drq_max = sizes[i];
        stream_all(c, "/BIG.PKG", &g_media_a, 0, 65536);
        CHECK(M.n_read10 <= bound());
        CHECK(!M.bcl_odd);
        CHECK(M.max_bcl <= ATAPI_PIO_BCL_MAX);
        iso9660_ops.umount(c);
    }
}

/* 覚えたパス: 違うパスは引き直す、同じなら引かない、マウントし直せば引く */
static void t_path_cache(void)
{
    Iso9660Ctx *c;
    u8 buf[8192];
    u32 i;
    OS32_Stat st;
    setup(); c = do_mount();
    CHECK(iso9660_ops.read_stream(c, "/SUB/DEEP.BIN", buf, 8192, 0) == (int)DEEP_SIZE);
    for (i = 0; i < DEEP_SIZE; i++) CHECK(buf[i] == file_byte(&g_media_a, DEEP_LBA, i));
    CHECK(c->stats.path_walks == 1);
    CHECK(iso9660_ops.read_stream(c, "/SUB/DEEP.BIN", buf, 10, 4990) == 10);
    CHECK(c->stats.path_walks == 1);
    CHECK(iso9660_ops.read_stream(c, "/TINY.TXT", buf, 8192, 0) == (int)TINY_SIZE);
    CHECK(c->stats.path_walks == 2);
    for (i = 0; i < TINY_SIZE; i++) CHECK(buf[i] == file_byte(&g_media_a, TINY_LBA, i));
    CHECK(iso9660_ops.stat(c, "/TINY.TXT", &st) == VFS_OK && st.st_size == TINY_SIZE);
    CHECK(c->stats.path_walks == 2);
    /* 見つからないパスは覚えない (覚えたパスはそのまま) */
    CHECK(iso9660_ops.read_stream(c, "/NOPE.TXT", buf, 10, 0) == VFS_ERR_NOTFOUND);
    CHECK(iso9660_ops.read_stream(c, "/NOPE.TXT", buf, 10, 0) == VFS_ERR_NOTFOUND);
    CHECK(c->stats.path_walks == 4);
    /* ファイルの下はディレクトリではない */
    CHECK(iso9660_ops.read_stream(c, "/TINY.TXT/X", buf, 10, 0) == VFS_ERR_NOTDIR);
    iso9660_ops.umount(c);
    c = do_mount();
    CHECK(iso9660_ops.read_stream(c, "/TINY.TXT", buf, 8192, 0) == (int)TINY_SIZE);
    CHECK(c->stats.path_walks == 1);
    iso9660_ops.umount(c);
}

/* 2 本のパスを交互に小さく読む: パスは引き直すが、ディレクトリのセクタは
 * LRU から出るので、媒体からは 1 回ずつしか読まない */
static void t_dir_lru(void)
{
    Iso9660Ctx *c;
    u8 buf[64];
    u32 k;
    setup(); c = do_mount();
    for (k = 0; k < 40; k++) {
        CHECK(iso9660_ops.read_stream(c, "/SUB/DEEP.BIN", buf, 64, k * 64) == 64);
        CHECK(buf[0] == file_byte(&g_media_a, DEEP_LBA, k * 64));
        CHECK(iso9660_ops.read_stream(c, "/TINY.TXT", buf, 1, k) == 1);
        CHECK(buf[0] == file_byte(&g_media_a, TINY_LBA, k));
    }
    report("dir_lru", c);
    CHECK(lba_reads(ROOT_LBA) == 1);
    CHECK(lba_reads(SUB_LBA) == 1);
    CHECK(lba_reads(SUB_LBA + 1) == 1);
    iso9660_ops.umount(c);
}

/* list_dir の cb が同じ FS を読んでも一覧が崩れない */
static Iso9660Ctx *g_ls_ctx;
static int g_ls_count, g_ls_big, g_ls_sub, g_ls_tiny;
static void ls_cb(const VfsDirEntry *e, void *u)
{
    u8 buf[200];
    (void)u;
    g_ls_count++;
    if (strcmp(e->name, "BIG.PKG") == 0) g_ls_big = (e->size == g_big_size);
    if (strcmp(e->name, "SUB") == 0) g_ls_sub = (e->type == VFS_TYPE_DIR);
    if (strcmp(e->name, "TINY.TXT") == 0) g_ls_tiny = 1;
    /* cb の中で同じ FS を読む。窓なしなので端のセクタも LRU に入り、根の
     * セクタより後に 4 本以上の別のセクタを使う = 根のスロットが追い出される */
    CHECK(iso9660_ops.read_stream(g_ls_ctx, "/SUB/DEEP.BIN", buf, 200, 100) == 200);
    CHECK(iso9660_ops.read_stream(g_ls_ctx, "/BIG.PKG", buf, 200, 70000) == 200);
    CHECK(iso9660_ops.read_stream(g_ls_ctx, "/BIG.PKG", buf, 200, 90000) == 200);
    CHECK(iso9660_ops.read_stream(g_ls_ctx, "/BIG.PKG", buf, 200, 110000) == 200);
    CHECK(iso9660_ops.read_stream(g_ls_ctx, "/BIG.PKG", buf, 200, 130000) == 200);
    CHECK(buf[0] == file_byte(&g_media_a, BIG_LBA_A, 130000));
}

static void t_list_reentrant(void)
{
    Iso9660Ctx *c;
    int i;
    setup();
    g_kmalloc_fail = 1;      /* 窓なし: データの端も LRU を通る */
    c = do_mount();
    g_kmalloc_fail = 0;
    g_ls_ctx = c;
    for (i = 0; i < 2; i++) {
        g_ls_count = g_ls_big = g_ls_sub = g_ls_tiny = 0;
        CHECK(iso9660_ops.list_dir(c, "/", ls_cb, NULL) == VFS_OK);
        CHECK(g_ls_count == 3 && g_ls_big && g_ls_sub && g_ls_tiny);
    }
    iso9660_ops.umount(c);
}

/* UNIT ATTENTION: 読みの途中で媒体が替わった。atapi は世代を進めて出し直し、
 * iso9660 は覚えていたものを捨てて新しい媒体で引き直す */
static void t_unit_attention(void)
{
    Iso9660Ctx *c;
    u8 buf[4096];
    u32 i, gen0;
    setup(); c = do_mount();
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 4096, 0) == 4096);
    CHECK(buf[0] == file_byte(&g_media_a, BIG_LBA_A, 0));
    gen0 = atapi_media_gen();
    M.ua_next = 1;
    /* 窓の外 (次の READ(10) が要る位置) を読む */
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 4096, 40u * SEC) == 4096);
    CHECK(atapi_media_gen() == gen0 + 1u);
    for (i = 0; i < 4096; i++)
        CHECK(buf[i] == file_byte(&g_media_b, BIG_LBA_B, 40u * SEC + i));
    CHECK(c->stats.path_walks == 2);
    /* 以後は新しい媒体の窓から */
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 4096, 0) == 4096);
    for (i = 0; i < 4096; i++)
        CHECK(buf[i] == file_byte(&g_media_b, BIG_LBA_B, i));
    iso9660_ops.umount(c);
}

/* 解決の途中で媒体が替わる (Codex レビュー 1 の P2)。根は A のものがキャッシュに
 * あり SUB = 21 と答える。SUB を読む READ(10) が UNIT ATTENTION → 出し直しで
 * B の 21 (囮) を読む。世代を見て B の根から引き直し、B の本物 (24) の答えを返す */
static void t_stat_swap(void)
{
    Iso9660Ctx *c;
    OS32_Stat st;
    u32 sz = 0;
    setup(); c = do_mount();
    CHECK(iso9660_ops.stat(c, "/TINY.TXT", &st) == VFS_OK);   /* A の根を LRU へ */
    M.ua_next = 1;
    CHECK(iso9660_ops.stat(c, "/SUB/DEEP.BIN", &st) == VFS_OK);
    CHECK(st.st_size == DEEP_SIZE_B);
    CHECK(lba_reads(SUB_LBA_B + 1u) == 1);

    /* get_file_size も同じ */
    iso9660_ops.umount(c);
    setup(); c = do_mount();
    CHECK(iso9660_ops.get_file_size(c, "/TINY.TXT", &sz) == VFS_OK && sz == TINY_SIZE);
    M.ua_next = 1;
    CHECK(iso9660_ops.get_file_size(c, "/SUB/DEEP.BIN", &sz) == VFS_OK);
    CHECK(sz == DEEP_SIZE_B);
    iso9660_ops.umount(c);
}

/* list_dir: 解決のあいだの交換は引き直す、一覧の途中 (cb の後) の交換は中断 */
static Iso9660Ctx *g_sw_ctx;
static int g_sw_calls, g_sw_arm, g_sw_read, g_sw_deep_b;
static void sw_cb(const VfsDirEntry *e, void *u)
{
    (void)u;
    g_sw_calls++;
    if (strcmp(e->name, "DEEP.BIN") == 0 && e->size == DEEP_SIZE_B) g_sw_deep_b = 1;
    if (g_sw_calls == 1 && g_sw_arm) {
        M.ua_next = 1;
        if (g_sw_read) {
            u8 b[16];
            /* cb の中で FS を読み、その読みが交換を踏む */
            (void)iso9660_ops.read_stream(g_sw_ctx, "/BIG.PKG", b, 16, 300000);
        }
    }
}

static void t_list_swap(void)
{
    Iso9660Ctx *c;
    OS32_Stat st;
    /* (1) 解決の途中 (根の読み) で交換 → 引き直して B の SUB (24) を一覧する */
    setup(); c = do_mount();
    M.ua_next = 1;
    g_sw_calls = g_sw_arm = g_sw_read = g_sw_deep_b = 0;
    g_sw_ctx = c;
    CHECK(iso9660_ops.list_dir(c, "/SUB", sw_cb, NULL) == VFS_OK);
    CHECK(g_sw_deep_b);
    iso9660_ops.umount(c);

    /* (2) 2 セクタの SUB の 1 セクタ目を読んだ後、次のセクタの読みで交換 → 中断 */
    setup(); c = do_mount();
    g_sw_calls = g_sw_deep_b = 0; g_sw_arm = 1; g_sw_read = 0;
    g_sw_ctx = c;
    CHECK(iso9660_ops.list_dir(c, "/SUB", sw_cb, NULL) == VFS_ERR_IO);
    CHECK(!g_sw_deep_b);
    iso9660_ops.umount(c);

    /* (3) 1 セクタの根: cb の中の読みが交換を踏む → 残りを読まずとも中断 */
    setup(); c = do_mount();
    g_sw_calls = 0; g_sw_arm = 1; g_sw_read = 1;
    g_sw_ctx = c;
    CHECK(iso9660_ops.list_dir(c, "/", sw_cb, NULL) == VFS_ERR_IO);
    CHECK(g_sw_calls == 1);
    /* 次の操作は新しい媒体で引き直す */
    CHECK(iso9660_ops.stat(c, "/SUB/DEEP.BIN", &st) == VFS_OK && st.st_size == DEEP_SIZE_B);
    iso9660_ops.umount(c);
}

/* 2 秒規則: 最後に媒体を読んでから ISO_IDLE_TICKS を超えたら捨てる (ちょうどは捨てない)。
 * 捨てるのはパス・LRU・先読みの窓の全部 */
static void t_idle_rule(void)
{
    Iso9660Ctx *c;
    u8 buf[16];
    u32 i;
    setup(); c = do_mount();
    tick_count = 1000;
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 16, 0) == 16);
    CHECK(c->stats.path_walks == 1);
    tick_count = 1000 + ISO_IDLE_TICKS;
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 16, 16) == 16);
    CHECK(c->stats.path_walks == 1);
    CHECK(lba_reads(ROOT_LBA) == 1);
    /* 当たりは時刻を進めない: 最後に媒体を読んだのは 1000 */
    tick_count = 1000 + ISO_IDLE_TICKS + 1;
    /* 入れ替えた媒体 (UNIT ATTENTION を出さない形)。B の BIG.PKG (LBA 45) は
     * A の窓 (LBA 40..) の中にある */
    M.m = &g_media_b;
    CHECK(iso9660_ops.read_stream(c, "/BIG.PKG", buf, 16, 0) == 16);
    for (i = 0; i < 16; i++) CHECK(buf[i] == file_byte(&g_media_b, BIG_LBA_B, i));
    CHECK(c->stats.path_walks == 2);
    CHECK(lba_reads(ROOT_LBA) == 2);
    iso9660_ops.umount(c);
}

/* 先読みの窓が取れなくても正しく読める (遅いだけ) */
static void t_no_window(void)
{
    Iso9660Ctx *c;
    setup();
    g_kmalloc_fail = 1;
    c = do_mount();
    g_kmalloc_fail = 0;
    CHECK(c->ra_buf == NULL);
    stream_all(c, "/BIG.PKG", &g_media_a, 7, 3000);
    CHECK(c->stats.path_walks == 1);
    iso9660_ops.umount(c);
}

/* 読みの列を再生する (tools/tests/test_cd_read.py が pkg.c の読み方から作る)。
 * 行は "offset length"。CD_READ_TRACE_MAX があれば READ(10) の数の上限 */
static void t_replay(void)
{
    const char *path = getenv("CD_READ_TRACE");
    const char *mx = getenv("CD_READ_TRACE_MAX");
    FILE *f;
    Iso9660Ctx *c;
    unsigned long off, len;
    u8 *buf = (u8 *)malloc(1u << 20);
    u32 reads = 0, bytes = 0, i;
    if (!path) { printf("replay: SKIP (CD_READ_TRACE なし)\n"); free(buf); return; }
    f = fopen(path, "r");
    CHECK(f != NULL);
    setup(); c = do_mount();
    while (fscanf(f, "%lu %lu", &off, &len) == 2) {
        int rc;
        CHECK(len <= (1u << 20));
        rc = iso9660_ops.read_stream(c, "/BIG.PKG", buf, (u32)len, (u32)off);
        CHECK(rc >= 0);
        for (i = 0; i < (u32)rc; i++)
            CHECK(buf[i] == file_byte(&g_media_a, BIG_LBA_A, (u32)off + i));
        reads++;
        bytes += (u32)rc;
    }
    fclose(f);
    printf("replay: reads=%lu bytes=%lu read10=%lu sectors=%lu walks=%lu\n",
           (unsigned long)reads, (unsigned long)bytes, (unsigned long)M.n_read10,
           (unsigned long)M.n_sectors, (unsigned long)c->stats.path_walks);
    CHECK(c->stats.path_walks == 1);
    if (mx) CHECK(M.n_read10 <= (u32)strtoul(mx, NULL, 10));
    free(buf);
    iso9660_ops.umount(c);
}

int main(int argc, char **argv)
{
    const char *cs = (argc > 1) ? argv[1] : "";
    const char *bs = getenv("CD_READ_BIGSIZE");
    if (bs) g_big_size = (u32)strtoul(bs, NULL, 10);
    if (!strcmp(cs, "stream_4k"))            t_stream_4k();
    else if (!strcmp(cs, "stream_odd"))      t_stream_odd();
    else if (!strcmp(cs, "stream_32k"))      t_stream_32k();
    else if (!strcmp(cs, "read_file"))       t_read_file();
    else if (!strcmp(cs, "multi_fallback"))  t_multi_fallback();
    else if (!strcmp(cs, "short_transfer"))  t_short_transfer();
    else if (!strcmp(cs, "bad_outside"))     t_bad_outside();
    else if (!strcmp(cs, "bad_inside"))      t_bad_inside();
    else if (!strcmp(cs, "stat_swap"))       t_stat_swap();
    else if (!strcmp(cs, "list_swap"))       t_list_swap();
    else if (!strcmp(cs, "lru_order"))       t_lru_order();
    else if (!strcmp(cs, "ua_mount"))        t_ua_mount();
    else if (!strcmp(cs, "multi_drq"))       t_multi_drq();
    else if (!strcmp(cs, "path_cache"))      t_path_cache();
    else if (!strcmp(cs, "dir_lru"))         t_dir_lru();
    else if (!strcmp(cs, "list_reentrant"))  t_list_reentrant();
    else if (!strcmp(cs, "unit_attention"))  t_unit_attention();
    else if (!strcmp(cs, "idle_rule"))       t_idle_rule();
    else if (!strcmp(cs, "no_window"))       t_no_window();
    else if (!strcmp(cs, "replay"))          t_replay();
    else { fprintf(stderr, "unknown case '%s'\n", cs); return 2; }
    return 0;
}
