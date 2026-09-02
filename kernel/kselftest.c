/* ======================================================================== */
/*  KSELFTEST.C — カーネル内プリミティブの自己診断                          */
/*                                                                          */
/*  なぜカーネル内でやるのか:                                                */
/*    programs/tests/klibc_test.c は **newlib とリンクされる外部プログラム** */
/*    なので、そこで通る strlen/memcpy/malloc は newlib の実装であって       */
/*    カーネルの kstring_asm.asm / kmalloc.c ではない。つまりカーネルが      */
/*    実際に使うプリミティブはこれまで一度もテストされていなかった。         */
/*                                                                          */
/*  ここでは「境界ケースだけ」を見る。通常ケースはシェルが動いている時点で   */
/*  通っているので、n=0 / バッファぴったり / 重なりコピー / 二重解放 など、   */
/*  実際にバグが潜んでいた形だけを実機で毎回踏んで確認する。                 */
/*                                                                          */
/*  ブート時に 1 回走り、全部通れば 1 行だけ出す。落ちた項目は赤で名前を     */
/*  出す (実機で回帰した瞬間に画面で分かる)。                               */
/* ======================================================================== */

#include "kselftest.h"
#include "kstring.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "paging.h"

/* 結果はホストから読めるようにグローバルにする。
 * ブート時の出力はスプラッシュで流れてしまい、rshell も未起動なので
 * シリアルにも出ない。kernel.map 経由で emu_read_mem するのが確実。
 * (v86.c の観測値と同じ理由で static にしない) */
int kselftest_pass = 0;
int kselftest_fail = 0;
#define ksel_pass kselftest_pass
#define ksel_fail kselftest_fail

static void check(int cond, const char *name)
{
    if (cond) {
        ksel_pass++;
    } else {
        ksel_fail++;
        kprintf(0xC1, "[selftest] FAIL: %s\n", name);
    }
}

/* ------------------------------------------------------------------------ */
/*  kstring_asm.asm (kmemcpy/kmemset/kstrlen/kstrcmp/kstrncpy)              */
/*                                                                          */
/*  ASM 実装は 4 バイト単位の高速パスと 1 バイトずつの端数パスを持つので、  */
/*  n が 0 / 端数 / 境界ちょうど のときに崩れやすい。番兵を置いて           */
/*  「書きすぎていないこと」まで見る。                                       */
/* ------------------------------------------------------------------------ */
static void test_mem(void)
{
    static u8 buf[32];
    static u8 src[32];
    u32 i;
    int ok;

    for (i = 0; i < 32; i++) src[i] = (u8)(i + 1);

    /* kmemset: n=0 は 1 バイトも書いてはいけない */
    buf[0] = 0xAA;
    kmemset(buf, 0x55, 0);
    check(buf[0] == 0xAA, "kmemset n=0");

    /* kmemset: 端数長 (高速パスと端数パスの境目) */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemset(buf, 0x55, 5);
    ok = (buf[0] == 0x55 && buf[4] == 0x55 && buf[5] == 0xAA);
    check(ok, "kmemset n=5 no overrun");

    /* kmemcpy: n=0 */
    buf[0] = 0xAA;
    kmemcpy(buf, src, 0);
    check(buf[0] == 0xAA, "kmemcpy n=0");

    /* kmemcpy: 端数長 + 番兵 */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemcpy(buf, src, 7);
    ok = (buf[0] == 1 && buf[6] == 7 && buf[7] == 0xAA);
    check(ok, "kmemcpy n=7 no overrun");

    /* kmemcpy: 非アラインの dst/src */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemcpy(buf + 1, src + 1, 9);
    ok = (buf[0] == 0xAA && buf[1] == 2 && buf[9] == 10 && buf[10] == 0xAA);
    check(ok, "kmemcpy unaligned");

    /* memmove: 前方に重なるコピー (kmemcpy では壊れる形) */
    for (i = 0; i < 32; i++) buf[i] = (u8)(i + 1);
    memmove(buf, buf + 2, 8);
    ok = (buf[0] == 3 && buf[7] == 10);
    check(ok, "memmove overlap forward");

    /* memmove: 後方に重なるコピー */
    for (i = 0; i < 32; i++) buf[i] = (u8)(i + 1);
    memmove(buf + 2, buf, 8);
    ok = (buf[2] == 1 && buf[9] == 8);
    check(ok, "memmove overlap backward");

    /* memmove: n=0 と同一ポインタ */
    buf[0] = 0x42;
    memmove(buf, buf, 8);
    memmove(buf, buf + 1, 0);
    check(buf[0] == 0x42, "memmove n=0 / same ptr");
}

static void test_str(void)
{
    static char dst[16];
    int ok;

    check(kstrlen("") == 0, "kstrlen empty");
    check(kstrlen("abcd") == 4, "kstrlen 4");

    check(kstrcmp("", "") == 0, "kstrcmp empty");
    check(kstrcmp("abc", "abd") < 0, "kstrcmp lt");
    check(kstrcmp("abd", "abc") > 0, "kstrcmp gt");
    /* 0x80 以上のバイトを含む比較。ASM 側が符号付きで比べていると
     * 大小が逆転する (CP932 の 2 バイト目が該当する) */
    check(kstrcmp("\x80", "\x01") > 0, "kstrcmp high byte unsigned");

    check(kstrncmp("abc", "abd", 0) == 0, "kstrncmp n=0");
    check(kstrncmp("abc", "abd", 2) == 0, "kstrncmp n=2");
    check(kstrncmp("abc", "abd", 3) != 0, "kstrncmp n=3");

    /* kstrncpy は strlcpy セマンティクス: n はバッファ全体サイズ、
     * 必ず NUL 終端する */
    kmemset(dst, 0x7F, sizeof(dst));
    kstrncpy(dst, "abcdefgh", 4);
    ok = (dst[0] == 'a' && dst[2] == 'c' && dst[3] == '\0');
    check(ok, "kstrncpy truncates + NUL");

    /* kstrncat: n=0 は 1 バイトも触らない (n-1 のラップで暴走した形) */
    dst[0] = 'X'; dst[1] = '\0';
    kstrncat(dst, "yyyy", 0);
    check(dst[0] == 'X' && dst[1] == '\0', "kstrncat n=0");

    /* kstrncat: バッファぴったりで切り詰め + NUL */
    kstrcpy(dst, "abc");
    kstrncat(dst, "defgh", 6);
    ok = (kstrcmp(dst, "abcde") == 0);
    check(ok, "kstrncat exact fit");

    /* kstrncat: 既に満杯なら何もしない */
    kstrcpy(dst, "abcde");
    kstrncat(dst, "zzz", 6);
    check(kstrcmp(dst, "abcde") == 0, "kstrncat already full");

    /* strchr は NUL 自身も見つける契約 */
    check(strchr("abc", '\0') != (char *)0, "strchr finds NUL");
    check(strchr("abc", 'z') == (char *)0, "strchr not found");
}

static void test_utoa(void)
{
    char buf[16];

    check(kutoa_dec(0, buf, 12) == 1 && kstrcmp(buf, "0") == 0,
          "kutoa_dec 0");
    check(kutoa_dec(4294967295UL, buf, 12) == 10 &&
          kstrcmp(buf, "4294967295") == 0, "kutoa_dec u32 max");
    check(kutoa_hex(0, buf, 12, 0) == 1 && kstrcmp(buf, "0") == 0,
          "kutoa_hex 0");
    check(kutoa_hex(0xDEADBEEFUL, buf, 12, 1) == 8 &&
          kstrcmp(buf, "DEADBEEF") == 0, "kutoa_hex upper");
    check(kutoa_hex(0xDEADBEEFUL, buf, 12, 0) == 8 &&
          kstrcmp(buf, "deadbeef") == 0, "kutoa_hex lower");
}

/* ------------------------------------------------------------------------ */
/*  アロケータ                                                              */
/*                                                                          */
/*  カーネルヒープを壊さずに実装そのものを試すため、専用の KHeap             */
/*  インスタンスを静的バッファ上に作る (kheap_* をパラメータ化してある       */
/*  おかげでこれができる)。                                                  */
/* ------------------------------------------------------------------------ */
static u8 ksel_heap_buf[2048];

static void test_heap(void)
{
    KHeap h;
    void *a, *b, *c;
    u32 free_all;
    int i;

    kheap_init(&h, ksel_heap_buf, sizeof(ksel_heap_buf), "selftest");

    a = kheap_alloc(&h, 16);
    check(a != (void *)0, "kheap_alloc basic");
    /* SQLite が double を置くので 8 バイト境界が要る */
    check((((u32)a) & 7) == 0, "kheap_alloc 8-byte aligned");

    b = kheap_alloc(&h, 1);
    check(b != (void *)0 && (((u32)b) & 7) == 0, "kheap_alloc(1) aligned");
    check(kheap_block_size(&h, a) >= 16, "kheap_block_size");

    /* 0 バイト要求と、ヒープ全体を超える要求は NULL */
    check(kheap_alloc(&h, 0) == (void *)0, "kheap_alloc(0) = NULL");
    check(kheap_alloc(&h, sizeof(ksel_heap_buf) * 2) == (void *)0,
          "kheap_alloc too big = NULL");
    /* アライン時に 0 へ化ける値 (整数オーバーフロー) も弾けること */
    check(kheap_alloc(&h, 0xFFFFFFFFUL) == (void *)0,
          "kheap_alloc overflow = NULL");

    /* 解放 → 再確保でブロックが再利用される */
    kheap_free(&h, a);
    kheap_free(&h, b);
    c = kheap_alloc(&h, 16);
    check(c == a, "kheap reuse after free");
    kheap_free(&h, c);

    /* 全域が 1 個のフリーブロックに戻っている (前後の結合が効いている)。
     * 結合漏れがあると、この後の「ほぼ全域」確保が失敗する。 */
    free_all = sizeof(ksel_heap_buf) - 64;
    c = kheap_alloc(&h, free_all);
    check(c != (void *)0, "kheap coalesce to one block");
    if (c) kheap_free(&h, c);

    /* 断片化 → 全解放で必ず 1 個に戻る (結合の取りこぼし検出) */
    {
        void *p[16];
        for (i = 0; i < 16; i++) p[i] = kheap_alloc(&h, 32);
        /* 飛び飛びに解放してから残りを解放 */
        for (i = 0; i < 16; i += 2) kheap_free(&h, p[i]);
        for (i = 1; i < 16; i += 2) kheap_free(&h, p[i]);
        c = kheap_alloc(&h, free_all);
        check(c != (void *)0, "kheap coalesce after fragmentation");
        if (c) kheap_free(&h, c);
    }

    /* 不正な解放を検出して弾くこと (弾かないとヒープ管理が壊れる)。
     * 診断が 1 行出るのは想定内。 */
    a = kheap_alloc(&h, 16);
    kheap_free(&h, a);
    kheap_free(&h, a);                      /* 二重解放 */
    kheap_free(&h, (void *)0x1);            /* 範囲外 */
    kheap_free(&h, (u8 *)a + 1);            /* 非アライン */
    c = kheap_alloc(&h, free_all);
    check(c != (void *)0, "kheap survives invalid frees");
    if (c) kheap_free(&h, c);

    /* 極小サイズのヒープ: ヘッダも入らないなら空ヒープとして扱う
     * (size - BLK_HDR_SIZE のアンダーフローで巨大ブロックに化けた形) */
    {
        KHeap tiny;
        kheap_init(&tiny, ksel_heap_buf, 4, "tiny");
        check(kheap_alloc(&tiny, 1) == (void *)0, "kheap tiny = empty");
    }
}

/* ------------------------------------------------------------------------ */
/*  kprintf                                                                 */
/*                                                                          */
/*  出力内容は取れないので「戻ってくること」を確かめる。書式末尾が '%' の    */
/*  ときに無限ループしていた (KAPI 公開関数なので、ユーザプログラムの        */
/*  1 行でカーネルが固まった)。ここで固まればブートが止まるので分かる。      */
/* ------------------------------------------------------------------------ */
static void test_kprintf(void)
{
    kprintf(0x07, "");
    kprintf(0x07, "%");             /* 末尾 % — 旧実装はここで無限ループ */
    kprintf(0x07, "%%");
    kprintf(0x07, "%z", 1);         /* 未知指定子 (vararg を消費する) */
    kprintf(0x07, "\n");
    check(1, "kprintf returns (no hang)");
}

/* ======================================================================== */
/*  公開エントリ                                                            */
/* ======================================================================== */
/* ------------------------------------------------------------------------ */
/*  リング3 PD 複製 (v2 M1b): 新 PD を作って CR3 に載せてもカーネル帯域が    */
/*  同一物理で共有され続けるか (V1)。ここが壊れると CPL=3 化以降が全滅する    */
/*  ので、ブート時に毎回検証する (CLAUDE.md: プリミティブは selftest に載せる)。*/
/* ------------------------------------------------------------------------ */
static void test_ring3_pd(void)
{
    int rc = paging_pd_clone_selftest();
    check(rc == 0, "ring3 PD clone (kernel band shared across PDs)");
    if (rc != 0) {
        kprintf(0xC1, "[selftest]   paging_pd_clone_selftest rc=%d\n", rc);
    }
}

int kselftest_run(void)
{
    ksel_pass = 0;
    ksel_fail = 0;

    test_mem();
    test_str();
    test_utoa();
    test_heap();
    test_kprintf();
    test_ring3_pd();

    if (ksel_fail == 0) {
        kprintf(0xA1, "[selftest] %d/%d passed\n", ksel_pass, ksel_pass);
    } else {
        kprintf(0xC1, "[selftest] %d FAILED (%d passed)\n",
                ksel_fail, ksel_pass);
    }
    return ksel_fail;
}
