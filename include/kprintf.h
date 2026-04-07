/* ======================================================================== */
/*  KPRINTF.H — 書式付き出力ライブラリヘッダ                                */
/* ======================================================================== */

#ifndef __KPRINTF_H
#define __KPRINTF_H

#include "types.h"

void __cdecl kprintf(u8 attr, const char *fmt, ...);

#endif /* __KPRINTF_H */
