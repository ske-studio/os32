/* ======================================================================== */
/*  KSTRING.C — カーネル用 文字列/メモリ操作ユーティリティ                  */
/*                                                                          */
/*  高頻度で呼ばれるメモリ操作・文字列操作は kstring_asm.asm に移行済み:     */
/*    kmemcpy, memcpy, kmemset, memset (rep movsd/stosd)                    */
/*    kstrlen, strlen (repne scasb)                                        */
/*    kstrcmp (lodsb + 比較ループ)                                         */
/*    kstrncmp (同上 + カウンタ制限)                                        */
/*    kstrcpy (repne scasb + rep movsb)                                    */
/*    kstrncpy (repne scasb + rep movsb + NUL終端保証)                     */
/*                                                                          */
/*  以下はASM化の効果が限定的なため C 実装のまま残す:                       */
/*    kstrcat, kstrncat (NUL探索+連結の2段階処理, 呼出箇所少)               */
/*                                                                          */
/*  libc互換シンボル (memcpy, memset, strlen) も kstring_asm.asm で提供。   */
/* ======================================================================== */

#include "kstring.h"

/* ======================================================================== */
/*  kstrcat — 文字列連結                                                      */
/* ======================================================================== */
char *kstrcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dst;
}

/* ======================================================================== */
/*  kstrncat — バッファ全体サイズ n を考慮した安全な文字列連結               */
/*  dst のバッファサイズが n のとき、n-1 文字目まで書き込み NUL 終端する。    */
/* ======================================================================== */
char *kstrncat(char *dst, const char *src, u32 n)
{
    u32 dlen = kstrlen(dst);
    u32 i;
    if (dlen >= n - 1) return dst;
    for (i = 0; src[i] && (dlen + i) < n - 1; i++) {
        dst[dlen + i] = src[i];
    }
    dst[dlen + i] = '\0';
    return dst;
}
