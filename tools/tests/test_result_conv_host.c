/* =========================================================================
 *  TEST_RESULT_CONV_HOST.C — 試験プログラムの「合否の出し方」を固定する
 *
 *  実行: python3 -B tools/tests/test_result_conv.py [--target] [--mutate]
 *  記録: tools/tests/result_conv_tdd.md
 *  票  : docs/tasks/test/TASK_TEST_RESULT.md §2 / §6 の 1
 *
 *  ゲストの試験プログラムの合否を人が画面を読まずに判定できるようにするには、
 *  **終了コードと最終行の集計行が必ず一致している**ことが要る。片方だけ直すと
 *  ランナー (3 段目) が 2 つの答えを持ってしまう。
 *
 *  この試験は 2 段で見る。
 *
 *   §1〜§4  約束事そのもの — 実物の userland/lib/rt/testresult.h を #include し、
 *           集計行の書式・終了コードの値域・両者の一致・予約値 (126/127/130/139)
 *           を返さないこと・切り詰めても溢れないことを直に叩く。
 *
 *   §5      実物のプログラム — userland/tests/ の 5 本を**贋物の KernelAPI で
 *           実際に走らせ**、出た文字列と `main` の返り値の**両方**を観測する。
 *           grep では「一致」は確かめられない (集計行が PASS と言いながら 1 を
 *           返す版も、grep はどちらも通してしまう)。
 *           合格側・不合格側・SKIP 側の 3 通りを同じ 1 本で踏み、さらに
 *           **argv[0] を変えても集計行の名前が変わらない**ことを見る。
 *
 *  プログラム本体は 1 行も写さない。取り込みは tools/tests/result_conv/run_*.c が
 *  `#define main <名前>_main` + `#include` で行う (理由は shim.h)。
 *
 *  エミュレータ・実配備・make には一切触れない。
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "os32api.h"
#include "rt/testresult.h"

static int failures;
static int checks;

static void check(int cond, const char *name)
{
    checks++;
    printf("  %s %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) failures++;
}

static void checkf(int cond, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    check(cond, line);
}

/* ------------------------------------------------------------------------ */
/*  §1  集計行の書式と終了コード                                              */
/* ------------------------------------------------------------------------ */

static void t_format(void)
{
    char buf[OS32_TEST_LINE_MAX];
    int rc;

    printf("\n[1] 集計行の書式 (票 §2-2)\n");

    rc = os32_test_summary(buf, sizeof(buf), "math_test", 42, 42);
    check(strcmp(buf, "math_test: PASS 42/42\n") == 0, "全部合格なら PASS n/n");
    check(rc == OS32_TEST_EXIT_PASS, "  その終了コードは 0");

    rc = os32_test_summary(buf, sizeof(buf), "math_test", 41, 42);
    check(strcmp(buf, "math_test: FAIL 41/42\n") == 0, "1 件落ちたら FAIL n/m");
    check(rc == OS32_TEST_EXIT_FAIL, "  その終了コードは 1");

    rc = os32_test_summary(buf, sizeof(buf), "save_test", 0, 9);
    check(strcmp(buf, "save_test: FAIL 0/9\n") == 0, "全滅も FAIL 0/m");
    check(rc == OS32_TEST_EXIT_FAIL, "  その終了コードは 1");

    rc = os32_test_summary_skip(buf, sizeof(buf), "save_test",
                                "/host is not mounted");
    check(strcmp(buf, "save_test: SKIP /host is not mounted\n") == 0,
          "前提が無ければ SKIP <理由>");
    check(rc == OS32_TEST_EXIT_SKIP, "  その終了コードは 2");

    /* 理由を書き忘れても行が壊れない (ランナーが読み違えない)。 */
    rc = os32_test_summary_skip(buf, sizeof(buf), "x", "");
    check(strcmp(buf, "x: SKIP no reason given\n") == 0,
          "SKIP の理由が空でも語が 1 つは載る");
    check(rc == OS32_TEST_EXIT_SKIP, "  その終了コードは 2");

    /* 1 項目も走らなかったものを合格にしない。集計変数が初期値のまま
     * 早期 return した試験が「PASS 0/0」で緑になるのが一番危ない。 */
    rc = os32_test_summary(buf, sizeof(buf), "x", 0, 0);
    check(strcmp(buf, "x: FAIL 0/0\n") == 0, "総数 0 は PASS にしない");
    check(rc == OS32_TEST_EXIT_FAIL, "  その終了コードは 1");

    /* 集計が壊れている (合格数が総数を超える / 負) のも合格にしない。 */
    rc = os32_test_summary(buf, sizeof(buf), "x", 4, 3);
    check(rc == OS32_TEST_EXIT_FAIL, "合格数 > 総数 は FAIL");
    check(strcmp(buf, "x: FAIL 4/3\n") == 0, "  行も FAIL");
    rc = os32_test_summary(buf, sizeof(buf), "x", -1, 3);
    check(rc == OS32_TEST_EXIT_FAIL, "合格数が負も FAIL");
    check(strcmp(buf, "x: FAIL -1/3\n") == 0, "  負値も 10 進で出る");

    /* 名前がそのまま載る (ランナーはこれで束ねる)。 */
    rc = os32_test_summary(buf, sizeof(buf), "db_v50_test", 1, 1);
    check(strncmp(buf, "db_v50_test: ", 13) == 0, "名前が行の先頭に載る");
    (void)rc;
}

/* ------------------------------------------------------------------------ */
/*  §2  終了コードと集計行が食い違わないこと                                  */
/* ------------------------------------------------------------------------ */

/* 行から動詞 (PASS / FAIL / SKIP) を取り出す。見つからなければ 0。 */
static char verb_of(const char *line)
{
    const char *p = strstr(line, ": ");

    if (!p) return 0;
    p += 2;
    if (strncmp(p, "PASS ", 5) == 0) return 'P';
    if (strncmp(p, "FAIL ", 5) == 0) return 'F';
    if (strncmp(p, "SKIP ", 5) == 0) return 'S';
    return 0;
}

static void t_agreement(void)
{
    char buf[OS32_TEST_LINE_MAX];
    int total, pass, rc;
    int bad_pair = 0;
    int bad_range = 0;
    int bad_resv = 0;
    int seen_pass = 0, seen_fail = 0;

    printf("\n[2] 終了コードと集計行の一致 (票 §2-2 の最後の 1 行)\n");

    for (total = 0; total <= 40; total++) {
        for (pass = -2; pass <= total + 2; pass++) {
            char v;

            rc = os32_test_summary(buf, sizeof(buf), "t", pass, total);
            v = verb_of(buf);
            if (v == 'P') seen_pass++;
            if (v == 'F') seen_fail++;
            if ((v == 'P') != (rc == OS32_TEST_EXIT_PASS)) bad_pair++;
            if ((v == 'F') != (rc == OS32_TEST_EXIT_FAIL)) bad_pair++;
            if (rc != OS32_TEST_EXIT_PASS && rc != OS32_TEST_EXIT_FAIL)
                bad_range++;
            if (os32_test_exit_reserved(rc)) bad_resv++;
        }
    }
    rc = os32_test_summary_skip(buf, sizeof(buf), "t", "why");
    if ((verb_of(buf) == 'S') != (rc == OS32_TEST_EXIT_SKIP)) bad_pair++;
    if (os32_test_exit_reserved(rc)) bad_resv++;

    checkf(bad_pair == 0,
           "PASS<->0 / FAIL<->1 / SKIP<->2 が全組み合わせで一致 (食い違い %d)",
           bad_pair);
    checkf(bad_range == 0, "終了コードは 0 か 1 だけ (外れ %d)", bad_range);
    checkf(bad_resv == 0, "予約値 126/127/130/139 を返さない (違反 %d)",
           bad_resv);
    checkf(seen_pass > 0 && seen_fail > 0,
           "PASS と FAIL の両方を実際に出した (PASS %d / FAIL %d)",
           seen_pass, seen_fail);
}

/* ------------------------------------------------------------------------ */
/*  §3  予約値の表                                                            */
/* ------------------------------------------------------------------------ */

static void t_reserved(void)
{
    int i;
    int wrong = 0;

    printf("\n[3] 予約値 (票 §1 / userland/shell/shell.h)\n");

    check(os32_test_exit_reserved(126), "126 (起こせなかった) は予約");
    check(os32_test_exit_reserved(127), "127 (実行ファイルが無い) は予約");
    check(os32_test_exit_reserved(130), "130 (中断) は予約");
    check(os32_test_exit_reserved(139), "139 (例外) は予約");

    for (i = OS32_TEST_EXIT_PASS; i <= OS32_TEST_EXIT_LOCAL_MAX; i++) {
        if (os32_test_exit_reserved(i)) wrong++;
    }
    checkf(wrong == 0, "0〜125 は 1 つも予約でない (誤判定 %d)", wrong);
    check(OS32_TEST_EXIT_LOCAL_MIN == 3 && OS32_TEST_EXIT_LOCAL_MAX == 125,
          "個別の票が使ってよい帯は 3〜125");
}

/* ------------------------------------------------------------------------ */
/*  §4  切り詰め — 短いバッファでも溢れず NUL で終わる                        */
/* ------------------------------------------------------------------------ */

static void t_truncate(void)
{
    char pad[64];
    unsigned int cap;
    int overflow = 0;
    int unterminated = 0;
    int wrong_rc = 0;

    printf("\n[4] 切り詰め\n");

    for (cap = 1; cap <= 40; cap++) {
        unsigned int i;
        int rc;

        memset(pad, '@', sizeof(pad));
        rc = os32_test_summary(pad, cap, "a_rather_long_test_name", 3, 3);
        if (rc != OS32_TEST_EXIT_PASS) wrong_rc++;
        if (pad[cap - 1] != '\0' && strlen(pad) >= cap) unterminated++;
        for (i = cap; i < sizeof(pad); i++) {
            if (pad[i] != '@') overflow++;
        }
    }
    checkf(overflow == 0, "cap を 1 バイトも超えて書かない (踏み越え %d)",
           overflow);
    checkf(unterminated == 0, "必ず NUL で終わる (終端なし %d)", unterminated);
    checkf(wrong_rc == 0,
           "行が切れても終了コードは正しいまま (ずれ %d)", wrong_rc);

    /* buf を渡さなくても終了コードだけは返る (出力先を持たない呼び手用)。 */
    check(os32_test_summary((char *)0, 0, "x", 1, 1) == OS32_TEST_EXIT_PASS,
          "buf が無くても終了コードは返る");
}

/* ========================================================================= */
/*  §5  実物のプログラムを贋物の KernelAPI で走らせる                          */
/* ========================================================================= */

/* --- 捕捉バッファ -------------------------------------------------------- */

#define CAP_MAX 65536
static char cap_buf[CAP_MAX];
static unsigned int cap_len;

static void cap_reset(void) { cap_len = 0; cap_buf[0] = '\0'; }

static void cap_add(const char *s)
{
    unsigned int n = (unsigned int)strlen(s);

    if (cap_len + n >= CAP_MAX) n = CAP_MAX - 1 - cap_len;
    memcpy(cap_buf + cap_len, s, n);
    cap_len += n;
    cap_buf[cap_len] = '\0';
}

int rconv_printf(const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cap_add(line);
    return n;
}

/* --- 贋物の KernelAPI ---------------------------------------------------- */

/* 場面ごとの振る舞いはこの 4 つで決める。 */
static int fk_stat_ok;        /* sys_stat が 0 を返すか                    */
static int fk_isatty_ok;      /* sys_isatty(1) が 1 を返すか               */
static int fk_font_rc;        /* kcg_load_font の返り値                    */
static int fk_write_short;    /* sys_write が要求より 1 バイト少なく返す   */
static int fk_res_ok;         /* FD / パイプ / リダイレクトが回収されたか  */

static char fk_file[256];
static int  fk_file_len;
static int  fk_file_open;

static void fk_kprintf(u8 attr, const char *fmt, ...)
{
    char line[4096];
    va_list ap;

    (void)attr;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    cap_add(line);
}

static int fk_sys_stat(const char *path, OS32_Stat *st)
{
    /* 「無いはずのもの」は常に無い — stat_t の 3 つ目はこれを見ている。 */
    if (path && strstr(path, "NONEXIST")) return -1;
    if (!fk_stat_ok) return -1;
    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_size = 1;
    }
    return 0;
}

static int fk_sys_fstat(int fd, OS32_Stat *st)
{
    (void)fd;
    if (!fk_stat_ok) return -1;
    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_mode = OS_S_IFCHR;
    }
    return 0;
}

static int fk_sys_isatty(int fd)
{
    (void)fd;
    return fk_isatty_ok ? 1 : 0;
}

static int fk_kcg_load_font(const char *path)
{
    (void)path;
    return fk_font_rc;
}

/* restest 用。回収されていれば取れる、いなければ取れない。 */
static int fk_pipes;

static int fk_sys_pipe_alloc(void)
{
    if (!fk_res_ok) return -1;
    if (fk_pipes >= 2) return -1;
    return fk_pipes++;
}

static void fk_sys_pipe_free(int id)
{
    (void)id;
    if (fk_pipes > 0) fk_pipes--;
}

static int fk_sys_redirect_fd(int fd, const char *path, int mode)
{
    (void)fd; (void)path; (void)mode;
    return fk_res_ok ? 0 : -1;
}

static u32 fk_tick;
static u32 fk_get_tick(void) { return fk_tick++; }

static void *fk_mem_alloc(u32 n) { return malloc(n ? n : 1); }
static void  fk_mem_free(void *p) { free(p); }

static int fk_sys_open(const char *path, int mode)
{
    (void)path;
    if (mode & KAPI_O_TRUNC) fk_file_len = 0;
    fk_file_open = 1;
    return 7;
}

static void fk_sys_close(int fd) { (void)fd; fk_file_open = 0; }

static int fk_sys_write(int fd, const void *buf, u32 size)
{
    u32 n = size;

    (void)fd;
    if (fk_write_short && n > 0) n--;
    if (fk_file_len + (int)n > (int)sizeof(fk_file))
        n = (u32)((int)sizeof(fk_file) - fk_file_len);
    memcpy(fk_file + fk_file_len, buf, n);
    fk_file_len += (int)n;
    return (int)n;
}

static int fk_sys_read(int fd, void *buf, u32 size)
{
    u32 n = (u32)fk_file_len;

    (void)fd;
    if (n > size) n = size;
    memcpy(buf, fk_file, n);
    return (int)n;
}

static KernelAPI g_fake;

static void fake_api_init(u32 version)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.version = version;
    g_fake.kprintf = fk_kprintf;
    g_fake.mem_alloc = fk_mem_alloc;
    g_fake.mem_free = fk_mem_free;
    g_fake.get_tick = fk_get_tick;
    g_fake.sys_open = fk_sys_open;
    g_fake.sys_close = fk_sys_close;
    g_fake.sys_read = fk_sys_read;
    g_fake.sys_write = fk_sys_write;
    g_fake.sys_stat = fk_sys_stat;
    g_fake.sys_fstat = fk_sys_fstat;
    g_fake.sys_isatty = fk_sys_isatty;
    g_fake.kcg_load_font = fk_kcg_load_font;
    g_fake.sys_pipe_alloc = fk_sys_pipe_alloc;
    g_fake.sys_pipe_free = fk_sys_pipe_free;
    g_fake.sys_redirect_fd = fk_sys_redirect_fd;
    fk_pipes = 0;
}

/* --- 実物の main (取り込みは tools/tests/result_conv/run_*.c) ------------- */
/* **わざと別の翻訳単位から宣言している** — `void main` に戻す変異を       */
/* コンパイルエラーではなく実行時の食い違いとして踏むため (shim.h)。        */
extern int stat_t_main(int argc, char **argv, KernelAPI *api);
extern int restest_main(int argc, char **argv, KernelAPI *api);
extern int test2_main(int argc, char **argv, KernelAPI *api);
extern int klibc_test_main(int argc, char **argv, KernelAPI *api);
extern int font_load_test_main(int argc, char **argv, KernelAPI *api);

typedef int (*prog_fn)(int argc, char **argv, KernelAPI *api);

/* --- 集計行の取り出しと判定 ---------------------------------------------- */

typedef struct {
    int  found;
    char name[64];
    char verb;          /* 'P' / 'F' / 'S' */
    int  pass;
    int  total;
    char line[256];
} Summary;

/* 捕捉した出力の**最終行**を読む。空行は読み飛ばす (最後が改行で終わるので)。 */
static void summary_of(const char *text, Summary *out)
{
    const char *end;
    const char *start;
    unsigned int n;
    char *colon;
    char *p;

    memset(out, 0, sizeof(*out));
    end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r')) end--;
    start = end;
    while (start > text && start[-1] != '\n' && start[-1] != '\r') start--;
    n = (unsigned int)(end - start);
    if (n == 0 || n >= sizeof(out->line)) return;
    memcpy(out->line, start, n);
    out->line[n] = '\0';

    colon = strstr(out->line, ": ");
    if (!colon) return;
    n = (unsigned int)(colon - out->line);
    if (n == 0 || n >= sizeof(out->name)) return;
    memcpy(out->name, out->line, n);
    out->name[n] = '\0';

    p = colon + 2;
    if (strncmp(p, "SKIP ", 5) == 0) {
        if (p[5] == '\0') return;
        out->verb = 'S';
        out->found = 1;
        return;
    }
    if (strncmp(p, "PASS ", 5) == 0) out->verb = 'P';
    else if (strncmp(p, "FAIL ", 5) == 0) out->verb = 'F';
    else return;
    if (sscanf(p + 5, "%d/%d", &out->pass, &out->total) != 2) return;
    out->found = 1;
}

/* 1 回走らせて、出力と終了コードの**両方**を約束事に照らす。
 * want は期待する動詞 ('P'/'F'/'S')、sub は副引数 (無ければ 0)。 */
static void run_prog_arg(prog_fn fn, const char *name, const char *sub,
                         char want, const char *scenario)
{
    char  argv0_a[64];
    char  argv0_b[64];
    char  sub_buf[64];
    char *argv_a[3];
    char *argv_b[3];
    Summary sa, sb;
    int   rc_a, rc_b;
    int   want_rc;
    int   argc = sub ? 2 : 1;

    snprintf(argv0_a, sizeof(argv0_a), "%s", name);
    snprintf(argv0_b, sizeof(argv0_b), "/usr/bin/%s.bin", name);
    snprintf(sub_buf, sizeof(sub_buf), "%s", sub ? sub : "");
    argv_a[0] = argv0_a; argv_a[1] = sub ? sub_buf : (char *)0; argv_a[2] = (char *)0;
    argv_b[0] = argv0_b; argv_b[1] = sub ? sub_buf : (char *)0; argv_b[2] = (char *)0;

    printf("  -- %s (%s)\n", name, scenario);

    cap_reset();
    rc_a = fn(argc, argv_a, &g_fake);
    summary_of(cap_buf, &sa);

    checkf(sa.found, "%s/%s: 最終行が集計行になっている [%s]",
           name, scenario, sa.line);
    if (!sa.found) return;

    checkf(strcmp(sa.name, name) == 0,
           "%s/%s: 行の名前が \"%s\" (出たのは \"%s\")",
           name, scenario, name, sa.name);
    checkf(sa.verb == want, "%s/%s: 動詞が %c (出たのは %c)",
           name, scenario, want, sa.verb ? sa.verb : '?');

    want_rc = (want == 'P') ? OS32_TEST_EXIT_PASS
            : (want == 'F') ? OS32_TEST_EXIT_FAIL
                            : OS32_TEST_EXIT_SKIP;
    checkf(rc_a == want_rc, "%s/%s: 終了コードが %d (返ったのは %d)",
           name, scenario, want_rc, rc_a);
    checkf(!os32_test_exit_reserved(rc_a),
           "%s/%s: 予約値 (126/127/130/139) を返していない", name, scenario);
    checkf(rc_a >= 0 && rc_a <= OS32_TEST_EXIT_LOCAL_MAX,
           "%s/%s: 終了コードが 0〜125 の中", name, scenario);

    /* 行と終了コードが食い違っていないこと — これがこの試験の主眼。 */
    checkf((sa.verb == 'P') == (rc_a == OS32_TEST_EXIT_PASS) &&
           (sa.verb == 'F') == (rc_a == OS32_TEST_EXIT_FAIL) &&
           (sa.verb == 'S') == (rc_a == OS32_TEST_EXIT_SKIP),
           "%s/%s: 集計行と終了コードが一致 (%c / %d)",
           name, scenario, sa.verb, rc_a);

    if (sa.verb == 'P') {
        checkf(sa.total > 0 && sa.pass == sa.total,
               "%s/%s: PASS の行は n==m かつ m>0 (%d/%d)",
               name, scenario, sa.pass, sa.total);
    } else if (sa.verb == 'F') {
        checkf(!(sa.total > 0 && sa.pass == sa.total),
               "%s/%s: FAIL の行は n==m>0 にならない (%d/%d)",
               name, scenario, sa.pass, sa.total);
    }

    /* argv[0] を変えても行が変わらない (リダイレクトや呼び方で名前が動かない)。 */
    cap_reset();
    rc_b = fn(argc, argv_b, &g_fake);
    summary_of(cap_buf, &sb);
    checkf(sb.found && strcmp(sa.line, sb.line) == 0,
           "%s/%s: argv[0] を \"%s\" にしても集計行が同じ [%s]",
           name, scenario, argv0_b, sb.line);
    checkf(rc_a == rc_b, "%s/%s: argv[0] を変えても終了コードが同じ",
           name, scenario);
}

static void run_prog(prog_fn fn, const char *name, char want,
                     const char *scenario)
{
    run_prog_arg(fn, name, (const char *)0, want, scenario);
}

static void t_programs(void)
{
    printf("\n[5] 実物のプログラムを贋物の KAPI で走らせる\n");

    /* --- stat_t: 4 項目。FS が応えるかどうかで合否が動く ----------------- */
    fake_api_init(60);
    fk_stat_ok = 1; fk_isatty_ok = 1;
    run_prog(stat_t_main, "stat_t", 'P', "stat/fstat/isatty が全部応える");
    fk_stat_ok = 0; fk_isatty_ok = 0;
    run_prog(stat_t_main, "stat_t", 'F', "FS が応えない");

    /* --- restest: 引数で場面が変わる。引数不正は SKIP (票 §2-1) ---------- */
    fake_api_init(60);
    fk_res_ok = 1; fk_isatty_ok = 1;
    run_prog_arg(restest_main, "restest", "verify", 'P', "回収できている");
    fake_api_init(60);
    fk_res_ok = 0; fk_isatty_ok = 0;
    run_prog_arg(restest_main, "restest", "verify", 'F', "回収されていない");
    fake_api_init(60);
    run_prog(restest_main, "restest", 'S', "引数なし");
    fake_api_init(60);
    run_prog_arg(restest_main, "restest", "nosuch", 'S', "知らない引数");

    /* --- test2: 版が古ければ SKIP、短い書き込みは合格にしない ------------ */
    fake_api_init(60);
    fk_write_short = 0;
    run_prog(test2_main, "test2", 'P', "KAPI v60");
    fake_api_init(1);
    run_prog(test2_main, "test2", 'S', "KAPI v1 (前提が無い)");
    fake_api_init(60);
    fk_write_short = 1;
    run_prog(test2_main, "test2", 'F', "sys_write が 1 バイト短い");
    fk_write_short = 0;

    /* --- klibc_test: KAPI を使わない newlib 側 --------------------------- */
    fake_api_init(60);
    run_prog(klibc_test_main, "klibc_test", 'P', "ホストの libc");

    /* --- font_load_test: PASS / FAIL / SKIP の 3 通り -------------------- */
    fake_api_init(60);
    fk_stat_ok = 1; fk_font_rc = 0;
    run_prog(font_load_test_main, "font_load_test", 'P', "読み込み成功");
    fk_stat_ok = 1; fk_font_rc = -3;
    run_prog(font_load_test_main, "font_load_test", 'F',
             "読み込み失敗 (負値を終了コードにしない)");
    fk_stat_ok = 0;
    run_prog(font_load_test_main, "font_load_test", 'S', "フォントが無い");
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    printf("=== test_result_conv_host — 合否の出し方 (票 §2) ===\n");

    t_format();
    t_agreement();
    t_reserved();
    t_truncate();
    t_programs();

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
