/* ======================================================================== */
/*  FONT_LOAD_TEST.C — kcg_load_font KAPI テスト                            */
/*                                                                          */
/*  .kcgfont ファイルをロードして KCG キャッシュを上書きし、                  */
/*  日本語テキスト描画で動作確認する。                                        */
/* ======================================================================== */

#include "os32api.h"
#include <stdio.h>

extern KernelAPI *kapi;

int main(int argc, char **argv)
{
    const char *path;
    int ret;

    if (argc > 1) {
        path = argv[1];
    } else {
        path = "/sys/font/default.kcgfont";
    }

    printf("kcg_load_font test\n");
    printf("path: %s\n", path);

    ret = kapi->kcg_load_font(path);
    printf("result: %d\n", ret);

    if (ret == 0) {
        printf("OK! font loaded.\n");
    } else {
        printf("FAILED: %d\n", ret);
    }

    return ret;
}
