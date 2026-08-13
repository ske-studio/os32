/* ======================================================================== */
/*  kapi_sys.c — KernelAPI システム情報関数                                  */
/* ======================================================================== */

#include "os32_kapi_shared.h"
#include "lib/kstring.h"

/* カーネルビルド時の日時文字列を返す */
void kapi_sys_get_build_info(char *buf, int size)
{
    /* int の size を無検証で u32 に渡すと負値が巨大長に化ける */
    if (!buf || size <= 0) return;
    kstrncpy(buf, __DATE__ " " __TIME__, (u32)size);
}
