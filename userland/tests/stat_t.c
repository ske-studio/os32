/* ======================================================================== */
/*  STAT_T.C — sys_stat / sys_fstat / sys_isatty テスト                     */
/*                                                                          */
/*  合否の出し方は票 docs/tasks/test/TASK_TEST_RESULT.md §2 に従う。         */
/*  以前は結果を画面に出すだけで、成否によらず sys_exit(0) していた。         */
/* ======================================================================== */

#include "os32api.h"
#include "rt/testresult.h"

static KernelAPI *g_api;
static int g_total;
static int g_passed;

static void check(int cond, const char *label)
{
    g_total++;
    if (cond) {
        g_passed++;
        g_api->kprintf(ATTR_GREEN, "[OK] %s\r\n", label);
    } else {
        g_api->kprintf(ATTR_RED, "[FAIL] %s\r\n", label);
    }
}

int main(int argc, char **argv, KernelAPI *api)
{
    OS32_Stat st;
    char      line[OS32_TEST_LINE_MAX];
    int       rc;

    (void)argc;
    (void)argv;

    g_api = api;
    g_total = 0;
    g_passed = 0;

    api->kprintf(0x07, "%s", "=== stat API test ===\r\n");

    /* テスト1: 存在するファイル (例: HELLO.BIN) の sys_stat */
    check(api->sys_stat("HELLO.BIN", &st) == 0, "stat HELLO.BIN success");

    /* テスト2: 標準出力の sys_fstat */
    rc = api->sys_fstat(1, &st);
    check(rc == 0, "fstat fd=1 (stdout) success");
    if (rc == 0) {
        /* 簡単なフラグ確認: S_IFCHRが含まれているか */
        check((st.st_mode & OS_S_IFCHR) != 0, "fd=1 is a character device");
    } else {
        check(0, "fd=1 is a character device");
    }

    /* テスト3: 無効なファイルの sys_stat */
    check(api->sys_stat("NONEXIST.TXT", &st) != 0,
          "stat NONEXIST.TXT correctly failed");

    /* テスト4: 先ほど実装した sys_isatty の確認 */
    check(api->sys_isatty(1) == 1, "isatty(1) == 1");

    rc = os32_test_summary(line, sizeof(line), "stat_t", g_passed, g_total);
    api->kprintf(rc ? ATTR_RED : ATTR_GREEN, "%s", line);
    return rc;
}
