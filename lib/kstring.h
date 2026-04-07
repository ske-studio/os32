/* ======================================================================== */
/*  KSTRING.H — カーネル用 文字列/メモリ操作ユーティリティ                  */
/*                                                                          */
/*  ベアメタル環境でlibc無しに使える基本関数群。                             */
/*  GCCが構造体代入等で暗黙に生成する memcpy/memset シンボルも提供する。     */
/* ======================================================================== */

#ifndef __KSTRING_H
#define __KSTRING_H

#include "types.h"

/* ======== メモリ操作 ======== */
void *kmemcpy(void *dst, const void *src, u32 n);
void *kmemset(void *dst, int val, u32 n);

/* ======== 文字列操作 ======== */
u32   kstrlen(const char *s);
char *kstrncpy(char *dst, const char *src, u32 n);
int   kstrcmp(const char *a, const char *b);
int   kstrncmp(const char *a, const char *b, u32 n);

/* ======== libc互換シンボル ======== */
/* GCC -ffreestanding でも構造体コピー等で memcpy/memset を暗黙に呼ぶ場合がある。 */
/* リンクエラー防止のため、標準名のシンボルも提供する。                           */
void *memcpy(void *dst, const void *src, u32 n);
void *memset(void *dst, int val, u32 n);
u32   strlen(const char *s);

#endif /* __KSTRING_H */
