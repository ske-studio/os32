/* ========================================================================
 *  ring3_str_host.c — KAPI が CPL=3 へ返す文字列の置き場を実物で確かめる
 *
 *  対象票: docs/tasks/gui/v13/TASK_T9_sh.md §12 R1 (Codex 網羅レビュー 往復 7)
 *  実行:   python3 -B tools/tests/test_ring3_str.py
 *  記録:   tools/tests/t9_tdd.md
 *
 *  exec/ring3_str.c を 1 行も写さずそのまま #include する (模型ではない)。
 *  カーネル帯の代わりに要るのは kstrncpy だけ。トランポリンページは
 *  ホストの 4KB 配列で代用し、**番地の式は実物のマクロ** を使う
 *  (RING3_USTR_STUB_OFF / RING3_USTR_OFF / RING3_USTR_CAP)。
 *
 *  ここで見るのは 3 つ:
 *    (1) CPL=3 経路 (in_syscall = 1) では戻り値が写し場の中を指す
 *    (2) CPL=0 経路 (in_syscall = 0) では static cwd がそのまま返る
 *    (3) 長さ VFS_MAX_PATH-1 の cwd でも切れない (NUL 終端も保つ)
 *  加えて、写し場がスタブの後ろで 1 ページに収まることを **型で** 固定する
 *  (KAPI が増えたらここでビルドが落ちる = 実機で気づく前に止まる)。
 *
 *  tools/tests/launch_host.c と同じ様式 — ホスト ILP32 GNU89 で走らせ、同じ
 *  ソースが i386-elf-gcc -Werror でも通ることを別に見る ([C1])。
 *  libc は使わない (-nostdlib、Linux の int 0x80 で write/exit するだけ)。
 * ======================================================================== */

#include "types.h"

/* ---- カーネル帯の代わり ------------------------------------------------ */
char *kstrncpy(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    if (n == 0) return dst;
    while (i + 1 < n && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

/* 実物 */
#include "ring3_str.c"

/* 写し場がスタブの後ろで 1 ページに収まる (exec/exec.c と同じ検査)。
 * PAGE_SIZE は kernel/paging.h だが、ここでは番地の式だけを見たいので
 * 値を持ち込まずに 4096 を使う (i386 の 4KB ページは動かない)。 */
#define HOST_PAGE_SIZE 4096u
STATIC_ASSERT(RING3_USTR_OFF >= RING3_USTR_STUB_OFF +
                                (u32)KAPI_FUNC_COUNT * 8u,
              host_ustr_after_stubs);
STATIC_ASSERT(RING3_USTR_OFF + RING3_USTR_CAP <= HOST_PAGE_SIZE,
              host_ustr_fits_in_page);

/* ---- 最小の報告系 (libc 無し) ------------------------------------------ */

static void die(int code)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code));
    for (;;) {}
}

static void report(const char *text)
{
    u32 len = 0;
    while (text[len]) len++;
    __asm__ volatile("int $0x80" : : "a"(4), "b"(1), "c"(text), "d"(len)
                     : "memory");
}

static int failures;

static void check(int cond, const char *name)
{
    report(cond ? "  ok   " : "  FAIL ");
    report(name);
    report("\n");
    if (!cond) failures++;
}

/* ---- 試験の道具 -------------------------------------------------------- */

/* トランポリンページの代わり (番地は実物のマクロで引く)。 */
static u8 host_page[HOST_PAGE_SIZE];
/* fs/vfs.c の static cwd の代わり (カーネル帯 = CPL=3 からは読めない番地)。 */
static char host_cwd[RING3_USTR_CAP];

static char *scratch(void)
{
    return (char *)(host_page + RING3_USTR_OFF);
}

static int str_eq(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static u32 str_len(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void set_cwd(const char *s)
{
    u32 i = 0;
    while (s[i] && i + 1 < RING3_USTR_CAP) { host_cwd[i] = s[i]; i++; }
    host_cwd[i] = '\0';
}

/* ========================================================================
 *  1. 経路で行き先が変わる (これが blocker R1 そのもの)
 * ======================================================================== */
static void case_route(void)
{
    const char *got;

    report("1 CPL=3 は写し / CPL=0 は static cwd\n");
    set_cwd("/usr/bin");

    got = ring3_user_str(0, scratch(), RING3_USTR_CAP, host_cwd);
    check(got == host_cwd, "1a CPL=0 の呼び手には static cwd をそのまま返す");

    got = ring3_user_str(1, scratch(), RING3_USTR_CAP, host_cwd);
    check(got == (const char *)scratch(), "1b CPL=3 の呼び手には写しを返す");
    check(got != host_cwd, "1c カーネル帯のポインタは CPL=3 へ渡らない");
    check((const u8 *)got >= host_page &&
          (const u8 *)got + RING3_USTR_CAP <= host_page + HOST_PAGE_SIZE,
          "1d 写し先はトランポリンページの中に収まる");
    check((const u8 *)got >= host_page + RING3_USTR_STUB_OFF +
                             (u32)KAPI_FUNC_COUNT * 8u,
          "1e 写し先は int 0x80 スタブの後ろ (表もスタブも壊さない)");
    check(str_eq(got, "/usr/bin"), "1f 中身は cwd と同じ");
    check(str_eq(host_cwd, "/usr/bin"), "1g 元の cwd は書き換えない");
}

/* ========================================================================
 *  2. 上書きの約束 (呼ばれるたびに写す)
 * ======================================================================== */
static void case_overwrite(void)
{
    const char *a;
    const char *b;

    report("2 写しは呼ばれるたびに上書きされる\n");
    set_cwd("/a");
    a = ring3_user_str(1, scratch(), RING3_USTR_CAP, host_cwd);
    check(str_eq(a, "/a"), "2a 1 回目");
    set_cwd("/bb/cc");
    b = ring3_user_str(1, scratch(), RING3_USTR_CAP, host_cwd);
    check(b == a, "2b 置き場は同じ (1 本だけ)");
    check(str_eq(b, "/bb/cc"), "2c 2 回目で上書きされる");
    check(str_len(b) == 6, "2d 前の中身が尾に残らない");
}

/* ========================================================================
 *  3. 上限の長さ (VFS_MAX_PATH - 1) でも切れない
 * ======================================================================== */
static void case_max_len(void)
{
    static char longpath[RING3_USTR_CAP + 8];
    const char *got;
    u32 i;

    report("3 上限長の cwd (RING3_USTR_CAP - 1 文字) でも切れない\n");
    longpath[0] = '/';
    for (i = 1; i < RING3_USTR_CAP - 1; i++) longpath[i] = 'a';
    longpath[RING3_USTR_CAP - 1] = '\0';
    set_cwd(longpath);
    check(str_len(host_cwd) == RING3_USTR_CAP - 1, "3a 元が上限ちょうど");

    got = ring3_user_str(1, scratch(), RING3_USTR_CAP, host_cwd);
    check(str_len(got) == RING3_USTR_CAP - 1, "3b 写しも上限ちょうど (切れない)");
    check(str_eq(got, host_cwd), "3c 中身が一致する");
    check(got[RING3_USTR_CAP - 1] == '\0', "3d NUL 終端が入っている");
    check(host_page[RING3_USTR_OFF + RING3_USTR_CAP] == 0,
          "3e 写し場の 1 バイト先を汚さない");
}

/* ========================================================================
 *  4. 写し場が無い / src が無い (起動途中と保険)
 * ======================================================================== */
static void case_no_scratch(void)
{
    report("4 写し場が無いとき (exec_init より前) は素通し\n");
    set_cwd("/x");
    check(ring3_user_str(1, 0, RING3_USTR_CAP, host_cwd) == host_cwd,
          "4a scratch = 0 なら写さずに返す");
    check(ring3_user_str(1, scratch(), 0, host_cwd) == host_cwd,
          "4b cap = 0 でも写さずに返す");
    check(ring3_user_str(1, scratch(), RING3_USTR_CAP, 0) == 0,
          "4c src = NULL はそのまま NULL");
    check(ring3_user_str(0, 0, 0, host_cwd) == host_cwd,
          "4d CPL=0 経路は写し場を見ない");
}

int main(void)
{
    failures = 0;
    report("ring3 user string scratch (T9 R1)\n");
    case_route();
    case_overwrite();
    case_max_len();
    case_no_scratch();
    if (failures) {
        report("FAILURES\n");
        die(1);
    }
    report("ALL PASS\n");
    die(0);
    return 0;
}

/* -nostdlib の入口 */
void _start(void)
{
    main();
}
