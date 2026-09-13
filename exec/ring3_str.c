/* ======================================================================== */
/*  RING3_STR.C — KAPI が CPL=3 へ返す文字列の置き場 (票 T9 §12 R1)         */
/*                                                                          */
/*  設計と番地の根拠は exec/ring3_str.h の冒頭。ここに置かないもの:          */
/*    - トランポリンページを作る / 張る手順 → exec/exec.c                    */
/*    - 「いま CPL=3 のディスパッチ中か」の判定 → exec/exec.c の             */
/*      ring3_in_syscall (呼び出し側が値で渡す)                              */
/*  だからこの .c はホストでそのまま試験できる                               */
/*  (tools/tests/ring3_str_host.c)。                                        */
/* ======================================================================== */

#include "ring3_str.h"
#include "kstring.h"

const char *ring3_user_str(int in_syscall, char *scratch, u32 cap,
                           const char *src)
{
    /* CPL=0 の呼び手 (常駐シェル / カーネル自身) はカーネル帯をそのまま
     * 読めるので写さない — 写すと「CUI で cd した直後の pwd」まで
     * トランポリンページ経由になり、exec_init より前の呼び出しで落ちる。 */
    if (!in_syscall) return src;
    if (scratch == 0 || cap == 0) return src;   /* 写し場が無い (起動途中) */
    if (src == 0) return src;
    /* kstrncpy の n は**バッファ全体サイズ**なので、cap ちょうどを渡せば
     * 必ず NUL 終端する (cap - 1 文字まで写る)。 */
    kstrncpy(scratch, src, cap);
    return (const char *)scratch;
}
