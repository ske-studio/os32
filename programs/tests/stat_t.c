#include "os32api.h"

int main(int argc, char **argv, KernelAPI *api) {
    OS32_Stat st;
    int rc;

    api->kprintf(0x07, "%s", "=== stat API test ===\r\n");

    /* テスト1: 存在するファイル (例: HELLO.BIN) の sys_stat */
    rc = api->sys_stat("HELLO.BIN", &st);
    if (rc == 0) {
        api->kprintf(0x0A, "%s", "[OK] stat HELLO.BIN success\r\n");
    } else {
        api->kprintf(0x0C, "%s", "[FAIL] stat HELLO.BIN failed\r\n");
    }

    /* テスト2: 標準出力の sys_fstat */
    rc = api->sys_fstat(1, &st);
    if (rc == 0) {
        api->kprintf(0x0A, "%s", "[OK] fstat fd=1 (stdout) success\r\n");
        /* 簡単なフラグ確認: S_IFCHRが含まれているか */
        if (st.st_mode & OS_S_IFCHR) {
            api->kprintf(0x07, "%s", "  -> Is character device\r\n");
        }
    } else {
        api->kprintf(0x0C, "%s", "[FAIL] fstat fd=1 failed\r\n");
    }

    /* テスト3: 無効なファイルの sys_stat */
    rc = api->sys_stat("NONEXIST.TXT", &st);
    if (rc != 0) {
        api->kprintf(0x0A, "%s", "[OK] stat NONEXIST.TXT correctly failed\r\n");
    } else {
        api->kprintf(0x0C, "%s", "[FAIL] stat NONEXIST.TXT unexpectedly succeeded\r\n");
    }

    /* テスト4: 先ほど実装した sys_isatty の確認 */
    rc = api->sys_isatty(1);
    if (rc == 1) {
        api->kprintf(0x0A, "%s", "[OK] isatty(1) == 1\r\n");
    } else {
        api->kprintf(0x0C, "%s", "[FAIL] isatty(1) != 1\r\n");
    }

    api->sys_exit(0);
    return 0;
}
