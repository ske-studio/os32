/* ======================================================================== */
/*  TESTRESULT.H — 試験プログラムの合否を機械が読める形にする約束事           */
/*                                                                          */
/*  票 docs/tasks/test/TASK_TEST_RESULT.md §2 の正典。**終了コードと集計行を */
/*  1 回の呼び出しで同時に作る**のがこのヘッダの目的で、片方だけ直して        */
/*  ランナーが 2 つの答えを持つ状態にならないようにしてある。                */
/*                                                                          */
/*  使い方 (これ以外の形を作らない):                                        */
/*                                                                          */
/*      #include "rt/testresult.h"                                          */
/*                                                                          */
/*      int main(int argc, char **argv, KernelAPI *api)                     */
/*      {                                                                   */
/*          char line[OS32_TEST_LINE_MAX];                                  */
/*          int  rc;                                                        */
/*          ...                                                             */
/*          rc = os32_test_summary(line, sizeof(line),                      */
/*                                 "math_test", g_passed, g_total);         */
/*          api->kprintf(rc ? ATTR_RED : ATTR_GREEN, "%s", line);           */
/*          return rc;                                                      */
/*      }                                                                   */
/*                                                                          */
/*  `<名前>` は**固定文字列**で渡す。argv[0] から作ると、リダイレクト先や    */
/*  呼び出し方 (`/usr/bin/math_test.bin` / `math_test`) で行が変わって       */
/*  ランナーが取りこぼす。                                                  */
/*                                                                          */
/*  出力先はこのヘッダでは決めない。kprintf を使う試験と newlib の printf を */
/*  使う試験が混在していて、両方に同じ 1 本を通すと後から出力が入れ替わる    */
/*  ためで、ここは**行を組み立てて終了コードを返す**ところまでを持つ。       */
/* ======================================================================== */

#ifndef __LIBOS32_TESTRESULT_H
#define __LIBOS32_TESTRESULT_H

/* ------------------------------------------------------------------------ */
/*  終了コード (票 §2-1)                                                     */
/* ------------------------------------------------------------------------ */

#define OS32_TEST_EXIT_PASS   0   /* 全項目合格                             */
#define OS32_TEST_EXIT_FAIL   1   /* 1 件以上不合格                         */
#define OS32_TEST_EXIT_SKIP   2   /* 実行しなかった (前提の欠如・引数不正)  */

/* 3〜125 は個別の票が定義してよい。定義しないなら使わない。 */
#define OS32_TEST_EXIT_LOCAL_MIN   3
#define OS32_TEST_EXIT_LOCAL_MAX   125

/* シェルの `$?` が使う予約値 (userland/shell/shell.h)。試験は返さない。
 * 返すと「落ちた」のか「自分で落ちた」のか区別できなくなる。 */
#define OS32_TEST_EXIT_RESV_SPAWN     126  /* 起こせなかった               */
#define OS32_TEST_EXIT_RESV_NOTFOUND  127  /* 実行ファイルが無い           */
#define OS32_TEST_EXIT_RESV_INTR      130  /* CTRL+STOP / ESC で中断       */
#define OS32_TEST_EXIT_RESV_FAULT     139  /* 例外 (#PF / #GP) で畳んだ    */

/* 集計行 1 行を組み立てるのに要るバイト数 (NUL 込み)。理由は付けない
 * 名前と SKIP の理由が長くなっても切り詰めで済むようにこの大きさにしてある。 */
#define OS32_TEST_LINE_MAX  128

/* ------------------------------------------------------------------------ */
/*  内部 — libc に依存しない小道具                                           */
/* ------------------------------------------------------------------------ */

/* `dst[at..cap-1]` へ `src` を NUL 手前まで詰める。返り値は新しい末尾。
 * cap は NUL のぶんを含む。入り切らないぶんは黙って捨てる (溢れない)。 */
static __inline__ unsigned int
os32_test__put(char *dst, unsigned int at, unsigned int cap, const char *src)
{
    unsigned int n = at;

    if (!dst || cap == 0) return at;
    if (!src) return n;
    while (*src && n + 1 < cap) {
        dst[n] = *src;
        n++;
        src++;
    }
    return n;
}

/* `dst[at..cap-1]` へ 10 進数を詰める。負値は先頭に '-' を置く。 */
static __inline__ unsigned int
os32_test__put_num(char *dst, unsigned int at, unsigned int cap, int v)
{
    char tmp[12];
    unsigned int n = at;
    unsigned int u;
    int k = 0;

    if (!dst || cap == 0) return at;
    if (v < 0) {
        n = os32_test__put(dst, n, cap, "-");
        u = (unsigned int)(-(long)v);
    } else {
        u = (unsigned int)v;
    }
    if (u == 0) {
        tmp[k] = '0';
        k++;
    }
    while (u > 0) {
        tmp[k] = (char)('0' + (int)(u % 10u));
        u /= 10u;
        k++;
    }
    while (k > 0) {
        k--;
        if (n + 1 < cap) {
            dst[n] = tmp[k];
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/*  公開                                                                     */
/* ------------------------------------------------------------------------ */

/* 予約値か (票 §2-1 の表)。ランナーと試験の両方がこの 1 本を見る。 */
static __inline__ int os32_test_exit_reserved(int status)
{
    return status == OS32_TEST_EXIT_RESV_SPAWN ||
           status == OS32_TEST_EXIT_RESV_NOTFOUND ||
           status == OS32_TEST_EXIT_RESV_INTR ||
           status == OS32_TEST_EXIT_RESV_FAULT;
}

/* 合否の集計から**終了コードと集計行を同時に**作る。
 *
 *   buf   … `<名前>: PASS <n>/<m>\n` / `<名前>: FAIL <n>/<m>\n` が入る。
 *           cap が足りなければ切り詰めるが、必ず NUL で終わる。
 *   name  … 固定文字列。argv[0] から作らない。
 *   pass  … 合格数、total … 総数。
 *
 * 返り値は終了コード。**total が 0 以下、または pass が総数と釣り合わない
 * ときは FAIL** — 1 項目も走らずに終わった試験を合格にしない。 */
static __inline__ int os32_test_summary(char *buf, unsigned int cap,
                                        const char *name, int pass, int total)
{
    unsigned int n = 0;
    int ok;

    ok = (total > 0) && (pass == total);

    if (buf && cap > 0) {
        n = os32_test__put(buf, n, cap, name ? name : "?");
        n = os32_test__put(buf, n, cap, ": ");
        n = os32_test__put(buf, n, cap, ok ? "PASS " : "FAIL ");
        n = os32_test__put_num(buf, n, cap, pass);
        n = os32_test__put(buf, n, cap, "/");
        n = os32_test__put_num(buf, n, cap, total);
        n = os32_test__put(buf, n, cap, "\n");
        buf[n] = '\0';
    }
    return ok ? OS32_TEST_EXIT_PASS : OS32_TEST_EXIT_FAIL;
}

/* 前提が無くて実行しなかったとき。`<名前>: SKIP <理由>\n` と終了コード 2。
 * 理由は空にしない (空だとランナーが行を読み違える)。 */
static __inline__ int os32_test_summary_skip(char *buf, unsigned int cap,
                                             const char *name,
                                             const char *reason)
{
    unsigned int n = 0;

    if (buf && cap > 0) {
        n = os32_test__put(buf, n, cap, name ? name : "?");
        n = os32_test__put(buf, n, cap, ": SKIP ");
        n = os32_test__put(buf, n, cap,
                           (reason && *reason) ? reason : "no reason given");
        n = os32_test__put(buf, n, cap, "\n");
        buf[n] = '\0';
    }
    return OS32_TEST_EXIT_SKIP;
}

#endif /* __LIBOS32_TESTRESULT_H */
