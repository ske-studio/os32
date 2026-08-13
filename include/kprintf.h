/* ======================================================================== */
/*  KPRINTF.H — 書式付き出力ライブラリヘッダ                                */
/* ======================================================================== */

#ifndef __KPRINTF_H
#define __KPRINTF_H

#include "types.h"

void __cdecl kprintf(u8 attr, const char *fmt, ...);

/* 数値→文字列変換 (kprintf 内部でも使用)。
 * buf に NUL 終端で書き、桁数を返す。bufsz はバッファ全体サイズ。
 * 桁揃えはしない — 固定幅表示が要る場合は呼び出し側で行う。 */
int kutoa_dec(u32 val, char *buf, int bufsz);
int kutoa_hex(u32 val, char *buf, int bufsz, int upper);

#endif /* __KPRINTF_H */
