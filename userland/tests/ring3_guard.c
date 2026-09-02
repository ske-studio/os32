/* ========================================================================= */
/*  RING3_GUARD — v2 M3 ハードニング: ヒープ/スタック境界ガードの検証        */
/*                                                                           */
/*  ring3_hello/ring3_fault と同じ crt0/KAPI 非依存の自己完結バイナリ。      */
/*  ヒープとスタックの間のガードページ (0x7BF000, 非present) へわざと書き込み、 */
/*  CPL=3 の #PF でアプリだけが kill されることを確認する。                   */
/*                                                                           */
/*  動作:                                                                     */
/*    1. テキスト VRAM に "RG"                                               */
/*    2. 0xA8000 に "GRD?" マーカー (書込試行直前)                            */
/*    3. ガードページ 0x7BF000 へ書き込む → 非present で #PF → kill されるはず */
/*    4. 生き残ったら 0xA8004 に "SURV" を書いて int 0x80 終了 (=ガード無効)   */
/*                                                                           */
/*  PM 検証: fault_kill_count +1、0xA8000="GRD?" かつ 0xA8004≠"SURV"、        */
/*           シリアルに [ring3] #PF ... addr=0x007BF000、カーネル生存。       */
/* ========================================================================= */

void _start(void) __attribute__((section(".text.startup"), used, noreturn));

void _start(void)
{
    volatile unsigned short *tvram = (volatile unsigned short *)0x000A0000UL;
    volatile unsigned short *avram = (volatile unsigned short *)0x000A2000UL;
    volatile unsigned long  *mark  = (volatile unsigned long  *)0x000A8000UL;
    volatile unsigned int   *guard = (volatile unsigned int   *)0x007BF000UL;

    tvram[76] = (unsigned short)'R'; avram[76] = 0x00E5;
    tvram[77] = (unsigned short)'G'; avram[77] = 0x00E5;

    /* 0x3F445247 = LE 47 52 44 3F = "GRD?" */
    mark[0] = 0x3F445247UL;

    /* ガードページ (非present) へ書き込み → #PF で kill されるはず。 */
    *guard = 0xDEADBEEFUL;

    /* ここに来た = ガードが効いていない。0x56525553 = LE "SURV" */
    mark[1] = 0x56525553UL;

    __asm__ __volatile__("int $0x80" : : "a"(0), "b"(1) : "memory");

    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}
