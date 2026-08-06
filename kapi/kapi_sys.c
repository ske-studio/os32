/* ======================================================================== */
/*  kapi_sys.c — KernelAPI システム情報関数                                  */
/* ======================================================================== */

#include "os32_kapi_shared.h"
#include "lib/kstring.h"

/* カーネルビルド時の日時文字列を返す */
void kapi_sys_get_build_info(char *buf, int size)
{
    kstrncpy(buf, __DATE__ " " __TIME__, size);
}
