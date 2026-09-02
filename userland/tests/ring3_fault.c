/* ========================================================================= */
/*  RING3_FAULT — v2 M1e の CPL=3 フォールト kill 検証用 (KAPI 非依存)        */
/*                                                                           */
/*  【重要】ring3_hello と同じく crt0 も KAPI も使わない自己完結バイナリ。    */
/*  M1 (M2 トランポリン前) は CPL=3 からカーネルコードページを呼べないので、  */
/*  KAPI を使う userland/tests/faultprobe/ は M1e では使えない。こちらは      */
/*  独自 _start + int 0x80 終了で、KAPI を一切呼ばずに「わざと不正アクセス」   */
/*  だけを行う。                                                              */
/*                                                                           */
/*  動作:                                                                     */
/*    1. テキスト VRAM に "RF" (CPL=3 到達の可視痕跡)                        */
/*    2. 0xA8000 に "FLT?" マーカー (フォールト試行の直前まで来た証拠)        */
/*    3. カーネル帯域 0x100000 へ書き込む → CPL=3 では PTE=supervisor なので   */
/*       #PF。M1e の #PF ハンドラがアプリを kill し fault_kill_count を        */
/*       インクリメントしてシェルへ戻す。ここから先へは進まないはず。         */
/*    4. もし kill されず生き残ったら 0xA8004 に "SURV" を書いて int 0x80 で   */
/*       終了する = 保護が効いていない ([ABI4] の穴が残っている) 証拠。        */
/*                                                                           */
/*  PM 検証 (os32-cycle fault-test):                                          */
/*    - fault_kill_count が +1 される                                        */
/*    - 0xA8000 = "FLT?" かつ 0xA8004 ≠ "SURV" (kill 済み)                   */
/*    - カーネル生存 (ver 応答) + シェル復帰                                  */
/* ========================================================================= */

/* app.ld の ENTRY(_start)。crt0 を link しないのでこれがエントリになる。 */
void _start(void) __attribute__((section(".text.startup"), used, noreturn));

void _start(void)
{
    volatile unsigned short *tvram = (volatile unsigned short *)0x000A0000UL;
    volatile unsigned short *avram = (volatile unsigned short *)0x000A2000UL;
    volatile unsigned long  *mark  = (volatile unsigned long  *)0x000A8000UL;
    volatile unsigned int   *kern  = (volatile unsigned int   *)0x00100000UL;

    /* 1. 画面右上 (行0, 桁76-77) に "RF" */
    tvram[76] = (unsigned short)'R'; avram[76] = 0x00E4;
    tvram[77] = (unsigned short)'F'; avram[77] = 0x00E4;

    /* 2. フォールト試行の直前マーカー。0x3F544C46 = LE 46 4C 54 3F = "FLT?" */
    mark[0] = 0x3F544C46UL;

    /* 3. カーネル帯域へ書き込み。CPL=3 では #PF して kill されるはず。
     *    ここで戻ってこない (M1e で保護が効いていれば)。 */
    *kern = 0xDEADBEEFUL;

    /* 4. ここに来た = kill されなかった = 保護が効いていない。
     *    0x56525553 = LE 53 55 52 56 = "SURV" */
    mark[1] = 0x56525553UL;

    /* int 0x80 (sys_exit)。status=1 で「生き残ってしまった」を示す。 */
    __asm__ __volatile__("int $0x80" : : "a"(0), "b"(1) : "memory");

    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}
