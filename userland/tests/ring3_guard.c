/* ========================================================================= */
/*  RING3_GUARD — v2 M3 ハードニング: ヒープ/スタック境界ガードの検証        */
/*                 + GUI v1.1 K3: 共有ライブラリ帯域 .text の書き込み保護     */
/*                                                                           */
/*  ring3_hello/ring3_fault と同じ crt0/KAPI 非依存の自己完結バイナリ。      */
/*  引数で 2 つのケースを選ぶ (どちらも「書いたら kill される」ことの確認)。  */
/*                                                                           */
/*    ring3_guard          ケース A: ヒープ/スタック間のガードページ          */
/*                         (0x7BF000, 非present) へ書き込む                   */
/*    ring3_guard shlib    ケース B: 共有ライブラリ帯域の .text (0x400000)    */
/*                         へ書き込む。K3 でここは read-only + USER になった  */
/*                         ので保護違反 (#PF err bit0=1,bit1=1,bit2=1)。       */
/*                         ライブラリ未ロードでも USER が立っていないので     */
/*                         同じく #PF になる (どちらでも kill が正解)。       */
/*                                                                           */
/*  動作:                                                                     */
/*    1. テキスト VRAM に "RG"                                               */
/*    2. 0xA8000 に "GRD?" / "SLB?" マーカー (書込試行直前)                    */
/*    3. 対象へ書き込む → #PF → kill されるはず                              */
/*    4. 生き残ったら 0xA8004 に "SURV" を書いて int 0x80 終了 (=保護無効)     */
/*                                                                           */
/*  PM 検証: fault_kill_count +1、0xA8000="GRD?"(または "SLB?") かつ          */
/*           0xA8004≠"SURV"、シリアルに [ring3] #PF ... addr=0x007BF000       */
/*           (ケース B は addr=0x00400000 と " [shlib band, WRITE]")、        */
/*           カーネル生存。                                                   */
/*                                                                           */
/*  引数は exec_run が CPL=3 のユーザスタックに call 互換で積む               */
/*  ([esp]=ダミー retaddr, [esp+4]=argc, ...) ので、cdecl の宣言でそのまま     */
/*  受け取れる。argv の中身は読まない (argc だけで分岐)。                     */
/* ========================================================================= */

void _start(int argc, char **argv) __attribute__((section(".text.startup"), used, noreturn));

void _start(int argc, char **argv)
{
    volatile unsigned short *tvram = (volatile unsigned short *)0x000A0000UL;
    volatile unsigned short *avram = (volatile unsigned short *)0x000A2000UL;
    volatile unsigned long  *mark  = (volatile unsigned long  *)0x000A8000UL;
    volatile unsigned int   *guard = (volatile unsigned int   *)0x007BF000UL;
    /* 共有ライブラリ帯域の先頭ページ = ジャンプ表 (.text/.rodata と同じ RO)。
     * include/memmap.h の MEM_SHLIB_BASE。カーネルヘッダは引けないので直値。 */
    volatile unsigned int   *shtext = (volatile unsigned int *)0x00400000UL;

    (void)argv;

    tvram[76] = (unsigned short)'R'; avram[76] = 0x00E5;
    tvram[77] = (unsigned short)'G'; avram[77] = 0x00E5;

    if (argc > 1) {
        /* ---- ケース B: 共有ライブラリ帯域 .text への書き込み (K3) ---- */
        /* 0x3F424C53 = LE 53 4C 42 3F = "SLB?" */
        mark[0] = 0x3F424C53UL;
        *shtext = 0xDEADBEEFUL;
    } else {
        /* ---- ケース A: ヒープ/スタック間のガードページ (v2 M3) ---- */
        /* 0x3F445247 = LE 47 52 44 3F = "GRD?" */
        mark[0] = 0x3F445247UL;
        *guard = 0xDEADBEEFUL;
    }

    /* ここに来た = 保護が効いていない。0x56525553 = LE "SURV" */
    mark[1] = 0x56525553UL;

    __asm__ __volatile__("int $0x80" : : "a"(0), "b"(1) : "memory");

    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}
