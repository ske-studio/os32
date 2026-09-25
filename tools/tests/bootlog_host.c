/* ======================================================================== */
/*  bootlog_host.c — kernel/bootlog.c のホスト試験                           */
/*                                                                          */
/*  kernel/bootlog.c を 1 行も写さずに #include する (模型ではない)。        */
/*  ホスト側だけ -DBOOTLOG_NO_IRQ_LOCK (CPL=3 では cli/popfl を実行できない) */
/*  書き出しの手順は偽の VFS (呼ばれた順を記録し、段ごとに失敗を注入する)。 */
/*                                                                          */
/*  使い方: bootlog_host <case>   (case は main() の表)                     */
/*  記録: tools/tests/bootlog_tdd.md                                        */
/* ======================================================================== */

#include <stdio.h>
#include <string.h>

#include "bootlog.c"

static int g_fail;

#define CHECK(cond, name) do { \
    if (cond) { printf("  ok   %s\n", name); } \
    else { printf("  FAIL %s\n", name); g_fail++; } \
} while (0)

static void reset(void)
{
    memset(g_bl_buf, 0, sizeof(g_bl_buf));
    g_bl_len = 0;
    g_bl_dropped = 0;
    g_bl_full = 0;
    g_bl_stopped = 0;
}

static const char *text(void) { return &g_bl_buf[BL_TEXT_OFF]; }

/* 本文を n バイトの 'x' で埋める (容量の端を作る) */
static void fill(u32 n)
{
    static char blk[256];
    memset(blk, 'x', sizeof(blk));
    while (n > 0) {
        u32 k = n > sizeof(blk) ? (u32)sizeof(blk) : n;
        bootlog_push(blk, k);
        n -= k;
    }
}

/* ------------------------------------------------------------------------ */
/*  1. 溜める                                                                */
/* ------------------------------------------------------------------------ */
static void case_collect(void)
{
    reset();
    CHECK(bootlog_is_active(), "1a 最初から積む (BSS のまま)");
    bootlog_push("abc", 3);
    bootlog_push("def\n", 4);
    bootlog_push(0, 5);
    bootlog_push("zzz", 0);
    CHECK(bootlog_len() == 7, "1b 長さは積んだ分だけ (NULL と長さ 0 は積まない)");
    CHECK(memcmp(text(), "abcdef\n", 7) == 0, "1c 順番どおり、属性は入らない");
    CHECK(bootlog_dropped() == 0, "1d 容量内では捨てない");
}

/* ------------------------------------------------------------------------ */
/*  2. あふれ — 後ろを捨てて先頭を残す                                       */
/* ------------------------------------------------------------------------ */
static void case_overflow(void)
{
    reset();
    fill(BL_TEXT_MAX - 5);
    bootlog_push("HEAD!", 5);
    CHECK(bootlog_len() == BL_TEXT_MAX && bootlog_dropped() == 0,
          "2a ちょうど満杯は捨てない");
    bootlog_push("tail", 4);
    CHECK(bootlog_len() == BL_TEXT_MAX && bootlog_dropped() == 4,
          "2b 満杯の後は新しい方を捨てて数える");
    CHECK(memcmp(text() + BL_TEXT_MAX - 5, "HEAD!", 5) == 0,
          "2c 先頭側 (先に来た方) が残る");

    reset();
    fill(BL_TEXT_MAX - 5);
    bootlog_push("0123456789", 10);
    CHECK(bootlog_len() == BL_TEXT_MAX, "2d 入る分だけ写す");
    CHECK(memcmp(text() + BL_TEXT_MAX - 5, "01234", 5) == 0, "2e 写したのは頭の 5 バイト");
    CHECK(bootlog_dropped() == 5, "2f 捨てたのは残りの 5 バイト");
    CHECK(g_bl_buf[BL_TEXT_OFF + BL_TEXT_MAX] == 0, "2g 本文の外 (末尾行の置き場) を踏まない");
}

/* ------------------------------------------------------------------------ */
/*  3. あふれの境目は UTF-8 の切れ目、一度あふれたら以後は全部捨てる          */
/* ------------------------------------------------------------------------ */
static void case_utf8_latch(void)
{
    /* "a" + あ (E3 81 82) + い (E3 81 84) */
    static const char s[] = "a\xE3\x81\x82\xE3\x81\x84";

    reset();
    fill(BL_TEXT_MAX - 4);
    bootlog_push(s, 7);
    CHECK(bootlog_len() == BL_TEXT_MAX && bootlog_dropped() == 3,
          "3a 4 バイトの空きには a+あ が入る");
    reset();
    fill(BL_TEXT_MAX - 3);
    bootlog_push(s, 7);
    CHECK(bootlog_len() == BL_TEXT_MAX - 2, "3b 3 バイトの空きには a だけ (あ を割らない)");
    CHECK(bootlog_dropped() == 6, "3c 割らずに捨てた分も数える");
    CHECK(text()[BL_TEXT_MAX - 3] == 'a', "3d 残った a");
    bootlog_push("ok", 2);
    CHECK(bootlog_len() == BL_TEXT_MAX - 2 && bootlog_dropped() == 8,
          "3e 空きが残っていても一度あふれたら後から来た行は捨てる");
}

/* ------------------------------------------------------------------------ */
/*  4. 止める                                                                */
/* ------------------------------------------------------------------------ */
static void case_stop(void)
{
    reset();
    bootlog_push("before\n", 7);
    bootlog_stop();
    bootlog_push("after\n", 6);
    CHECK(!bootlog_is_active(), "4a 止めた後は active でない");
    CHECK(bootlog_len() == 7, "4b 止めた後は積まない");
    CHECK(bootlog_dropped() == 0, "4c 止めた後の出力は「捨てた」に数えない");
}

/* ------------------------------------------------------------------------ */
/*  5. ヘッダ                                                                */
/* ------------------------------------------------------------------------ */
static void case_header(void)
{
    char buf[BOOTLOG_HDR_MAX];
    char small[24];
    BootlogHeaderInfo hi;
    u32 n;

    memset(&hi, 0, sizeof(hi));
    hi.build = "Sep 25 2026 17:14:00";
    hi.commit = "1d4ac40";
    hi.crc_valid = 1;
    hi.image_crc = 0x0012ABCDUL;
    hi.image_size = 452491UL;
    hi.ticks = 1234UL;
    n = bootlog_format_header(buf, sizeof(buf), &hi);
    CHECK(strcmp(buf, "# OS32 boot log  Build Sep 25 2026 17:14:00  Commit 1d4ac40"
                      "  Image CRC 0012ABCD (452491 bytes)  uptime-ticks 1234\n") == 0,
          "5a ヘッダの全文 (CRC は 8 桁の 16 進)");
    CHECK(n == strlen(buf), "5b 戻りは長さ");

    hi.crc_valid = 0;
    hi.build = 0;
    hi.ticks = 4294967295UL;
    bootlog_format_header(buf, sizeof(buf), &hi);
    CHECK(strcmp(buf, "# OS32 boot log  Build ?  Commit 1d4ac40"
                      "  Image CRC none  uptime-ticks 4294967295\n") == 0,
          "5c CRC が無ければ none、Build が無ければ ?、tick は 10 桁まで");

    memset(small, 'Z', sizeof(small));
    n = bootlog_format_header(small, sizeof(small), &hi);
    CHECK(n == sizeof(small) - 1 && small[n - 1] == '\n' && small[n] == '\0',
          "5d 収まらなければ切って、それでも 1 行 (改行 + NUL) で終わる");
    CHECK(bootlog_format_header(small, 1, &hi) == 0, "5e cap < 2 は書かない");
}

/* ------------------------------------------------------------------------ */
/*  6. 書き出し用の 1 本                                                     */
/* ------------------------------------------------------------------------ */
static int eq(const char *got, u32 n, const char *want)
{
    return n == (u32)strlen(want) && memcmp(got, want, n) == 0;
}

static void case_compose(void)
{
    const char *out, *out2;
    u32 n, n2;

    reset();
    bootlog_push("[selftest] 9/9 passed\n", 22);
    out = bootlog_compose("# H\n", &n);
    CHECK(eq(out, n, "# H\n[selftest] 9/9 passed\n"
                     "# end  kept 22 bytes  dropped 0 bytes\n"),
          "6a ヘッダ + 本文 + 末尾行が連続した 1 本");
    out2 = bootlog_compose("# H\n", &n2);
    CHECK(out2 == out && n2 == n && bootlog_len() == 22,
          "6b 何度組んでも同じ (本文は動かない)");

    reset();
    bootlog_push("no newline", 10);
    out = bootlog_compose("# H\n", &n);
    CHECK(memcmp(out, "# H\nno newline\n# end  kept 10 bytes", 35) == 0,
          "6c 本文が行の途中で切れていたら改行を足してから末尾行");
    bootlog_push("+more", 5);
    out = bootlog_compose("# H\n", &n);
    CHECK(memcmp(out + 4, "no newline+more\n", 16) == 0,
          "6d 組んだ後に積んでも本文は続きから (足した改行は本文ではない)");

    reset();
    out = bootlog_compose("# H\n", &n);
    CHECK(eq(out, n, "# H\n# end  kept 0 bytes  dropped 0 bytes\n"),
          "6e 空でもヘッダと末尾行は出る");

    reset();
    fill(BL_TEXT_MAX - 1);
    bootlog_push("ab", 2);
    out = bootlog_compose("# H\n", &n);
    CHECK(n > 4 + BL_TEXT_MAX &&
          eq(out + 4 + BL_TEXT_MAX, n - 4 - BL_TEXT_MAX,
             "\n# end  kept 16384 bytes  dropped 1 bytes\n"),
          "6f あふれたときも末尾行は置き場に収まり、捨てた数を書く");
    CHECK(out == &g_bl_buf[BL_TEXT_OFF - 4], "6g ヘッダは本文の直前に右詰め");
}

/* ------------------------------------------------------------------------ */
/*  7. 書き出し先の種別と FAT の 8.3                                         */
/* ------------------------------------------------------------------------ */

/* FatFs (FF_USE_LFN 0) の create_name と同じ判定: 本体 1〜8、ドットは 1 個
 * まで、拡張子 0〜3、禁止文字なし。 */
static int is_83_component(const char *s, u32 len)
{
    u32 i, body = 0, ext = 0;
    int dot = 0;
    if (len == 0) return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '.') { if (dot || body == 0) return 0; dot = 1; continue; }
        if (strchr("*+,:;<=>[]|\"?\x7F ", c)) return 0;
        if (dot) ext++; else body++;
    }
    return body <= 8 && ext <= 3;
}

static int is_83_path(const char *p)
{
    while (*p) {
        const char *q;
        while (*p == '/') p++;
        q = p;
        while (*q && *q != '/') q++;
        if (q > p && !is_83_component(p, (u32)(q - p))) return 0;
        p = q;
    }
    return 1;
}

static void case_plan(void)
{
    CHECK(bootlog_plan("ext2") == BOOTLOG_FS_EXT2, "7a ext2 は書く");
    CHECK(bootlog_plan("fat") == BOOTLOG_FS_FAT, "7b fat は書く (8.3)");
    CHECK(bootlog_plan("hostdrv") == BOOTLOG_FS_SKIP, "7c hostdrv は書かない");
    CHECK(bootlog_plan("iso9660") == BOOTLOG_FS_SKIP, "7d iso9660 は書かない");
    CHECK(bootlog_plan("serialfs") == BOOTLOG_FS_SKIP, "7e serialfs は書かない");
    CHECK(bootlog_plan(0) == BOOTLOG_FS_SKIP, "7f 未マウント (NULL) は書かない");
    CHECK(bootlog_plan("ext") == BOOTLOG_FS_SKIP && bootlog_plan("ext2x") == BOOTLOG_FS_SKIP,
          "7g 名前は完全一致");
    CHECK(strcmp(bootlog_old_path(BOOTLOG_FS_EXT2), "/var/log/boot.log.1") == 0,
          "7h ext2 の前回分は boot.log.1");
    CHECK(bootlog_old_path(BOOTLOG_FS_SKIP) == 0, "7i 書かない種別に名前は無い");
    CHECK(is_83_path(bootlog_old_path(BOOTLOG_FS_FAT)), "7j FAT の前回分は 8.3");
    CHECK(is_83_path(SYS_BOOTLOG_FILE) && is_83_path(SYS_BOOTLOG_DIR) &&
          is_83_path(SYS_BOOTLOG_VAR_DIR), "7k /var /var/log /var/log/boot.log は 8.3");
    CHECK(!is_83_path("/var/log/boot.log.1"), "7l (判定の対照) boot.log.1 は 8.3 でない");
}

/* ------------------------------------------------------------------------ */
/*  8. 書き出しの手順 (偽の VFS)                                             */
/* ------------------------------------------------------------------------ */

#define FK_MAX   8
#define FK_ERR_IO (-7)

typedef struct { char path[40]; int isdir; char data[64]; u32 len; int used; } FkNode;

static FkNode fk[FK_MAX];
static char  fk_log[512];
static int   fk_fat;          /* 1 = rename は宛先があると EXIST (FatFs) */
static const char *fk_fail_op;  /* この名前の操作で fk_fail_rc を返す */
static int   fk_fail_rc;
static int   fk_short;        /* write が 1 バイト少なく返す */

static void fk_reset(int fat)
{
    memset(fk, 0, sizeof(fk));
    fk_log[0] = '\0';
    fk_fat = fat;
    fk_fail_op = 0;
    fk_fail_rc = 0;
    fk_short = 0;
}

static FkNode *fk_find(const char *p)
{
    int i;
    for (i = 0; i < FK_MAX; i++) if (fk[i].used && strcmp(fk[i].path, p) == 0) return &fk[i];
    return 0;
}

static FkNode *fk_add(const char *p, int isdir, const char *data)
{
    int i;
    for (i = 0; i < FK_MAX; i++) {
        if (!fk[i].used) {
            fk[i].used = 1;
            strcpy(fk[i].path, p);
            fk[i].isdir = isdir;
            if (data) { strcpy(fk[i].data, data); fk[i].len = (u32)strlen(data); }
            return &fk[i];
        }
    }
    return 0;
}

static int fk_logop(const char *op, const char *p)
{
    strcat(fk_log, op);
    if (p) { strcat(fk_log, ":"); strcat(fk_log, p); }
    strcat(fk_log, ";");
    return fk_fail_op && strcmp(fk_fail_op, op) == 0;
}

static int fk_mkdir(const char *p)
{
    if (fk_logop("mkdir", p)) return fk_fail_rc;
    if (fk_find(p)) return OS32_ERR_EXIST;
    fk_add(p, 1, 0);
    return 0;
}

static int fk_rm(const char *p)
{
    FkNode *n;
    if (fk_logop("rm", p)) return fk_fail_rc;
    n = fk_find(p);
    if (!n) return OS32_ERR_NOTFOUND;
    n->used = 0;
    return 0;
}

static int fk_rename(const char *a, const char *b)
{
    FkNode *n, *d;
    if (fk_logop("rename", a)) return fk_fail_rc;
    n = fk_find(a);
    if (!n) return OS32_ERR_NOTFOUND;
    d = fk_find(b);
    if (d) {
        if (fk_fat) return OS32_ERR_EXIST;   /* FatFs f_rename は置き換えない */
        d->used = 0;
    }
    strcpy(n->path, b);
    return 0;
}

static int fk_write(const char *p, const void *data, u32 size)
{
    FkNode *n;
    if (fk_logop("write", p)) return fk_fail_rc;
    n = fk_find(p);
    if (!n) n = fk_add(p, 0, 0);
    if (size >= sizeof(n->data)) size = sizeof(n->data) - 1;
    memcpy(n->data, data, size);
    n->data[size] = '\0';
    n->len = size;
    return fk_short ? (int)size - 1 : (int)size;
}

static int fk_sync(void)
{
    if (fk_logop("sync", 0)) return fk_fail_rc;
    return 0;
}

static const BootlogFsOps fk_ops = { fk_mkdir, fk_rm, fk_rename, fk_write, fk_sync };

static int fk_has(const char *p, const char *content)
{
    FkNode *n = fk_find(p);
    return n && strcmp(n->data, content) == 0;
}

static void case_save(void)
{
    int st, rc;

    /* a. 何も無い ext2 */
    fk_reset(0);
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_OK && rc == 0, "8a 初回は全段通る (EXIST / NOTFOUND は成功)");
    CHECK(strcmp(fk_log, "mkdir:/var;mkdir:/var/log;rm:/var/log/boot.log.1;"
                         "rename:/var/log/boot.log;write:/var/log/boot.log;sync;") == 0,
          "8b 順序: mkdir → mkdir → rm .1 → rename → write → sync");
    CHECK(fk_has("/var/log/boot.log", "NEW"), "8c boot.log に今回の中身");

    /* b. 前々回と前回がある ext2 */
    fk_reset(0);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    fk_add("/var/log/boot.log", 0, "PREV");
    fk_add("/var/log/boot.log.1", 0, "OLDER");
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_OK, "8d 2 回目以降も通る");
    CHECK(fk_has("/var/log/boot.log", "NEW") && fk_has("/var/log/boot.log.1", "PREV"),
          "8e 前回分は .1 へ、前々回は上書きされて消える");

    /* c. FAT: rename は宛先があると断るので、先に消していないと回らない */
    fk_reset(1);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    fk_add("/var/log/boot.log", 0, "PREV");
    fk_add("/var/log/bootlog.1", 0, "OLDER");
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_FAT, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_OK && fk_has("/var/log/bootlog.1", "PREV") &&
          fk_has("/var/log/boot.log", "NEW"), "8f FAT は bootlog.1 へ回して書く");

    /* d. /var が作れない → 書かずに止める */
    fk_reset(0);
    fk_fail_op = "mkdir"; fk_fail_rc = FK_ERR_IO;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_MKDIR_VAR && rc == FK_ERR_IO && strstr(fk_log, "write") == 0,
          "8g /var が作れなければ MKDIR_VAR で止め、書かない");

    /* f. 前回分を消せない → 付け替えを飛ばして今回分は書く */
    fk_reset(0);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    fk_add("/var/log/boot.log", 0, "PREV");
    fk_fail_op = "rm"; fk_fail_rc = FK_ERR_IO;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_RM_OLD && rc == FK_ERR_IO, "8i rm の失敗は RM_OLD");
    CHECK(strstr(fk_log, "rename") == 0, "8j rm が落ちたら付け替えない");
    CHECK(fk_has("/var/log/boot.log", "NEW") && strstr(fk_log, "sync;") != 0,
          "8k それでも今回分は書いて sync する");

    /* g. 付け替えが落ちる → 書く */
    fk_reset(0);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    fk_add("/var/log/boot.log", 0, "PREV");
    fk_fail_op = "rename"; fk_fail_rc = FK_ERR_IO;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_ROTATE && rc == FK_ERR_IO && fk_has("/var/log/boot.log", "NEW"),
          "8l rename の失敗は ROTATE、今回分は書く");

    /* h. 書けない → sync しない */
    fk_reset(0);
    fk_fail_op = "write"; fk_fail_rc = FK_ERR_IO;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_WRITE && rc == FK_ERR_IO && strstr(fk_log, "sync") == 0,
          "8m write の失敗は WRITE、sync しない");

    /* i. 書いた量が足りない */
    fk_reset(0);
    fk_short = 1;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_WRITE && rc == 2, "8n 書いた量が足りなければ WRITE (rc は書けた量)");

    /* j. sync が落ちる */
    fk_reset(0);
    fk_fail_op = "sync"; fk_fail_rc = FK_ERR_IO;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_SYNC && rc == FK_ERR_IO, "8o sync の失敗は SYNC");

    /* k. 最初の失敗を報告する (rename と write の両方が落ちる) */
    fk_reset(0);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    fk_add("/var/log/boot.log", 0, "PREV");
    fk_add("/var/log/boot.log.1", 0, "X");
    fk_fail_op = "rm"; fk_fail_rc = -9;
    fk_short = 1;
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_RM_OLD && rc == -9, "8p 失敗が重なっても最初の段と rc を返す");

    /* l. 書かない種別 */
    fk_reset(0);
    st = bootlog_save_with(&fk_ops, BOOTLOG_FS_SKIP, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_OK && fk_log[0] == '\0', "8q 書かない種別は何も呼ばない");

    /* m. 段の名前 */
    CHECK(strcmp(bootlog_stage_name(BOOTLOG_ST_ROTATE), "rotate") == 0 &&
          strcmp(bootlog_stage_name(BOOTLOG_ST_MKDIR_LOG), "mkdir /var/log") == 0 &&
          strcmp(bootlog_stage_name(99), "?") == 0, "8r 段の名前");
}

/* /var/log だけが作れない (1 本目の mkdir は通し、2 本目で落とす) */
static int g_mk_n;
static int fk_mkdir_2nd_fails(const char *p)
{
    g_mk_n++;
    if (g_mk_n == 2) { fk_logop("mkdir", p); return FK_ERR_IO; }
    return fk_mkdir(p);
}

static void case_save_mkdir_log(void)
{
    BootlogFsOps ops = fk_ops;
    int st, rc;
    ops.mkdir = fk_mkdir_2nd_fails;
    fk_reset(0);
    g_mk_n = 0;
    st = bootlog_save_with(&ops, BOOTLOG_FS_EXT2, "NEW", 3, &rc);
    CHECK(st == BOOTLOG_ST_MKDIR_LOG && rc == FK_ERR_IO && strstr(fk_log, "write") == 0,
          "8s /var/log が作れなければ MKDIR_LOG で止め、書かない");
    fk_reset(0);
    fk_add("/var", 1, 0); fk_add("/var/log", 1, 0);
    g_mk_n = 0;
    ops.mkdir = fk_mkdir;
    st = bootlog_save_with(&ops, BOOTLOG_FS_EXT2, "NEW", 3, 0);
    CHECK(st == BOOTLOG_ST_OK, "8t fail_rc が NULL でも落ちない");
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "collect",      case_collect },
        { "overflow",     case_overflow },
        { "utf8_latch",   case_utf8_latch },
        { "stop",         case_stop },
        { "header",       case_header },
        { "compose",      case_compose },
        { "plan",         case_plan },
        { "save",         case_save },
        { "save_mkdir",   case_save_mkdir_log },
    };
    unsigned i;
    int ran = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc < 2 || strcmp(argv[1], cases[i].name) == 0) {
            printf("[%s]\n", cases[i].name);
            cases[i].fn();
            ran++;
        }
    }
    if (ran == 0) { printf("unknown case\n"); return 2; }
    return g_fail ? 1 : 0;
}
