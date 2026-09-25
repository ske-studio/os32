/* ======================================================================== */
/*  BOOTLOG.C — 最後の起動のログ (純粋な部分)                               */
/*                                                                          */
/*  方針と API は include/bootlog.h。ここは I/O も VFS も触らない:           */
/*  溜める・あふれを数える・書き出し用の 1 本を組む・書き出しの手順 (ops    */
/*  越し)。ホスト試験 tools/tests/bootlog_host.c がこのファイルをそのまま    */
/*  #include する。VFS と版情報へ結ぶのは kernel/bootlog_save.c。           */
/*                                                                          */
/*  「リング」と呼んでいるが、先頭を残す方針なので実体は前から詰める 1 本の  */
/*  配列で、折り返さない。本文の前に BOOTLOG_HDR_MAX、後ろに              */
/*  BOOTLOG_TAIL_MAX を空けておき、ヘッダと末尾行をそこへ置いて連続した     */
/*  1 本として書く (写し用の 2 本目を持たない)。                            */
/* ======================================================================== */

#include "os32_kapi_shared.h"   /* OS32_ERR_EXIST / OS32_ERR_NOTFOUND (types.h より先) */
#include "bootlog.h"

/* 割込み禁止区間。ホスト試験は CPL=3 で走り cli/popfl を実行できないので、
 * そこだけ空の錠に差し替える (kernel/con_sink.c と同じ)。 */
#ifdef BOOTLOG_NO_IRQ_LOCK
static unsigned int bootlog_lock(void)       { return 0; }
static void bootlog_unlock(unsigned int f)   { (void)f; }
#else
#include "io.h"
static unsigned int bootlog_lock(void)       { return irq_save(); }
static void bootlog_unlock(unsigned int f)   { irq_restore(f); }
#endif

#define BL_TEXT_OFF   ((u32)BOOTLOG_HDR_MAX)
#define BL_TEXT_MAX   ((u32)SYS_BOOTLOG_TEXT_MAX)

static char g_bl_buf[BOOTLOG_HDR_MAX + SYS_BOOTLOG_TEXT_MAX + BOOTLOG_TAIL_MAX];
static u32  g_bl_len;          /* 本文のバイト数 */
static u32  g_bl_dropped;      /* あふれで捨てたバイト数 */
static int  g_bl_full;         /* 一度あふれたら 1 (以後は全部捨てる) */
static int  g_bl_stopped;      /* bootlog_stop 後は 1。BSS なので最初から積む */

/* ------------------------------------------------------------------------ */
/*  積む                                                                     */
/* ------------------------------------------------------------------------ */

void bootlog_push(const char *buf, u32 len)
{
    unsigned int f;
    u32 room, n, i;

    if (g_bl_stopped || !buf || len == 0) return;   /* 止めた後の費用は 1 回の読み */
    f = bootlog_lock();
    if (g_bl_stopped) { bootlog_unlock(f); return; }
    if (g_bl_full) {
        g_bl_dropped += len;
        bootlog_unlock(f);
        return;
    }
    room = BL_TEXT_MAX - g_bl_len;
    n = len;
    if (n > room) {
        /* 入る分だけ写し、UTF-8 の文字の途中で切らない: 最初に捨てる
         * バイトが継続バイト (10xxxxxx) なら、その文字の頭まで戻す。 */
        n = room;
        while (n > 0 && ((u8)buf[n] & 0xC0u) == 0x80u) n--;
        g_bl_full = 1;
        g_bl_dropped += len - n;
    }
    for (i = 0; i < n; i++) g_bl_buf[BL_TEXT_OFF + g_bl_len + i] = buf[i];
    g_bl_len += n;
    bootlog_unlock(f);
}

void bootlog_stop(void)
{
    unsigned int f = bootlog_lock();
    g_bl_stopped = 1;
    bootlog_unlock(f);
}

int bootlog_is_active(void) { return !g_bl_stopped; }
u32 bootlog_len(void)       { return g_bl_len; }
u32 bootlog_dropped(void)   { return g_bl_dropped; }

/* ------------------------------------------------------------------------ */
/*  組む (kstring / kprintf を引かない — ホストでそのまま走らせるため)       */
/* ------------------------------------------------------------------------ */

/* dst[*pos] 以降へ s を足す。cap - 1 までで止める (NUL の分を残す)。 */
static void bl_put_str(char *dst, u32 cap, u32 *pos, const char *s)
{
    while (s && *s && *pos + 1 < cap) dst[(*pos)++] = *s++;
}

static void bl_put_dec(char *dst, u32 cap, u32 *pos, u32 v)
{
    char t[11];
    int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v && n < 10);
    while (n > 0 && *pos + 1 < cap) dst[(*pos)++] = t[--n];
}

static void bl_put_hex8(char *dst, u32 cap, u32 *pos, u32 v)
{
    static const char hx[] = "0123456789ABCDEF";
    int sh;
    for (sh = 28; sh >= 0 && *pos + 1 < cap; sh -= 4)
        dst[(*pos)++] = hx[(v >> sh) & 0xFu];
}

u32 bootlog_format_header(char *buf, u32 cap, const BootlogHeaderInfo *hi)
{
    u32 pos = 0;

    if (!buf || cap < 2) return 0;
    bl_put_str(buf, cap, &pos, "# OS32 boot log  Build ");
    bl_put_str(buf, cap, &pos, (hi && hi->build) ? hi->build : "?");
    bl_put_str(buf, cap, &pos, "  Commit ");
    bl_put_str(buf, cap, &pos, (hi && hi->commit) ? hi->commit : "?");
    bl_put_str(buf, cap, &pos, "  Image CRC ");
    if (hi && hi->crc_valid) {
        bl_put_hex8(buf, cap, &pos, hi->image_crc);
        bl_put_str(buf, cap, &pos, " (");
        bl_put_dec(buf, cap, &pos, hi->image_size);
        bl_put_str(buf, cap, &pos, " bytes)");
    } else {
        bl_put_str(buf, cap, &pos, "none");
    }
    bl_put_str(buf, cap, &pos, "  uptime-ticks ");
    bl_put_dec(buf, cap, &pos, hi ? hi->ticks : 0);
    /* 切り詰めても 1 行で終わらせる */
    if (pos + 1 >= cap) pos = cap - 2;
    buf[pos++] = '\n';
    buf[pos] = '\0';
    return pos;
}

const char *bootlog_compose(const char *header, u32 *out_len)
{
    unsigned int f;
    u32 hl = 0, start, end, i;

    while (header && header[hl] && hl < (u32)BOOTLOG_HDR_MAX) hl++;
    f = bootlog_lock();
    start = BL_TEXT_OFF - hl;
    for (i = 0; i < hl; i++) g_bl_buf[start + i] = header[i];

    /* 末尾行は本文の直後 (空きの BOOTLOG_TAIL_MAX に収まる)。本文が行の
     * 途中で切れていたら改行を足してから書く。 */
    end = BL_TEXT_OFF + g_bl_len;
    if (g_bl_len > 0 && g_bl_buf[end - 1] != '\n') g_bl_buf[end++] = '\n';
    {
        u32 pos = 0;
        char *t = &g_bl_buf[end];
        u32 cap = (u32)sizeof(g_bl_buf) - end;
        bl_put_str(t, cap, &pos, "# end  kept ");
        bl_put_dec(t, cap, &pos, g_bl_len);
        bl_put_str(t, cap, &pos, " bytes  dropped ");
        bl_put_dec(t, cap, &pos, g_bl_dropped);
        bl_put_str(t, cap, &pos, " bytes\n");
        end += pos;
    }
    bootlog_unlock(f);
    if (out_len) *out_len = end - start;
    return &g_bl_buf[start];
}

/* ------------------------------------------------------------------------ */
/*  書き出しの手順                                                           */
/* ------------------------------------------------------------------------ */

static int bl_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int bootlog_plan(const char *fstype)
{
    if (bl_streq(fstype, SYS_BOOTLOG_FS_EXT2)) return BOOTLOG_FS_EXT2;
    if (bl_streq(fstype, SYS_BOOTLOG_FS_FAT))  return BOOTLOG_FS_FAT;
    return BOOTLOG_FS_SKIP;    /* hostdrv / iso9660 / serialfs / 未マウント */
}

const char *bootlog_old_path(int kind)
{
    if (kind == BOOTLOG_FS_EXT2) return SYS_BOOTLOG_OLD;
    if (kind == BOOTLOG_FS_FAT)  return SYS_BOOTLOG_OLD_83;
    return 0;
}

const char *bootlog_stage_name(int stage)
{
    switch (stage) {
    case BOOTLOG_ST_OK:        return "ok";
    case BOOTLOG_ST_MKDIR_VAR: return "mkdir " SYS_BOOTLOG_VAR_DIR;
    case BOOTLOG_ST_MKDIR_LOG: return "mkdir " SYS_BOOTLOG_DIR;
    case BOOTLOG_ST_RM_OLD:    return "rm old";
    case BOOTLOG_ST_ROTATE:    return "rotate";
    case BOOTLOG_ST_WRITE:     return "write";
    case BOOTLOG_ST_SYNC:      return "sync";
    default:                   return "?";
    }
}

/* 最初の失敗だけを覚える */
static void bl_fail(int *first, int *first_rc, int stage, int rc)
{
    if (*first == BOOTLOG_ST_OK) { *first = stage; *first_rc = rc; }
}

int bootlog_save_with(const BootlogFsOps *ops, int kind,
                      const char *data, u32 len, int *fail_rc)
{
    const char *old = bootlog_old_path(kind);
    int first = BOOTLOG_ST_OK, first_rc = 0, rc, rotate_ok = 1;

    if (fail_rc) *fail_rc = 0;
    if (!ops || !old) return BOOTLOG_ST_OK;     /* 書かない種別 */

    /* 1. ディレクトリ。無ければ書けないので止める */
    rc = ops->mkdir(SYS_BOOTLOG_VAR_DIR);
    if (rc != 0 && rc != OS32_ERR_EXIST) {
        if (fail_rc) *fail_rc = rc;
        return BOOTLOG_ST_MKDIR_VAR;
    }
    rc = ops->mkdir(SYS_BOOTLOG_DIR);
    if (rc != 0 && rc != OS32_ERR_EXIST) {
        if (fail_rc) *fail_rc = rc;
        return BOOTLOG_ST_MKDIR_LOG;
    }

    /* 2. 前回分を消す。FatFs の rename は宛先があると断るので先に消す */
    rc = ops->rm(old);
    if (rc != 0 && rc != OS32_ERR_NOTFOUND) {
        bl_fail(&first, &first_rc, BOOTLOG_ST_RM_OLD, rc);
        rotate_ok = 0;
    }

    /* 3. 今の boot.log を前回分へ。宛先が空いていると分かったときだけ */
    if (rotate_ok) {
        rc = ops->rename(SYS_BOOTLOG_FILE, old);
        if (rc != 0 && rc != OS32_ERR_NOTFOUND)
            bl_fail(&first, &first_rc, BOOTLOG_ST_ROTATE, rc);
    }

    /* 4. 今回のログ。付け替えに失敗しても書く */
    rc = ops->write(SYS_BOOTLOG_FILE, data, len);
    if (rc < 0 || (u32)rc != len) {
        bl_fail(&first, &first_rc, BOOTLOG_ST_WRITE, rc);
    } else {
        /* 5. 電源断に備えて書き戻す */
        rc = ops->sync();
        if (rc != 0) bl_fail(&first, &first_rc, BOOTLOG_ST_SYNC, rc);
    }

    if (fail_rc) *fail_rc = first_rc;
    return first;
}
