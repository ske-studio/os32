/* ======================================================================== */
/*  FONT_LOAD_TEST.C — kcg_load_font KAPI テスト                            */
/*                                                                          */
/*  .kcgfont ファイルをロードして KCG キャッシュを上書きし、                  */
/*  日本語テキスト描画で動作確認する。                                        */
/*                                                                          */
/*  合否の出し方は票 docs/tasks/test/TASK_TEST_RESULT.md §2 に従う。         */
/*  **kcg_load_font の戻り値をそのまま終了コードにしない** — 失敗は負値で、   */
/*  シェルの予約値 (126/127/130/139) と衝突しうる。フォントが無いのは        */
/*  「不合格」ではなく「実行しなかった」なので SKIP (終了コード 2)。          */
/* ======================================================================== */

#include "os32api.h"
#include "rt/testresult.h"
#include <stdio.h>

int main(int argc, char **argv, KernelAPI *api)
{
    const char *path;
    char        line[OS32_TEST_LINE_MAX];
    OS32_Stat   st;
    int         ret;
    int         rc;

    if (argc > 1) {
        path = argv[1];
    } else {
        path = "/sys/font/default.kcgfont";
    }

    printf("kcg_load_font test\n");
    printf("path: %s\n", path);

    if (api->sys_stat(path, &st) != 0) {
        rc = os32_test_summary_skip(line, sizeof(line), "font_load_test",
                                    "font file not found");
        printf("%s", line);
        return rc;
    }

    ret = api->kcg_load_font(path);
    printf("result: %d\n", ret);

    rc = os32_test_summary(line, sizeof(line), "font_load_test",
                           (ret == 0) ? 1 : 0, 1);
    printf("%s", line);
    return rc;
}
