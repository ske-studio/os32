#include "types.h"
#include "io.h"

/* Ring0 #GP 発生時にフォルトEIPをTVRAMに直接表示して停止
 * kprintfはスタック消費が大きいため使用禁止 */
void print_debug_gp(u32 esp)
{
    /* TVRAM直接書き込み (最小スタック消費) */
    static const char hex[] = "0123456789ABCDEF";
    volatile u16 *tc = (volatile u16 *)0xA0000UL;
    volatile u16 *ta = (volatile u16 *)0xA2000UL;
    u32 fault_eip;
    int i;
    int pos = 24 * 80;
    const char *msg = "!!! RING0 #GP  EIP=";

    /* スタック上のフォルトEIPを取得:
     * isr_stub_13 から push esp; call print_debug_gp なので
     * esp引数はISRスタック上のポインタ
     * [esp+0]=error_code, [esp+4]=EIP(fault), [esp+8]=CS */
    fault_eip = *(u32 *)(esp + 4);

    while (*msg) {
        tc[pos] = (u16)(u8)*msg;
        ta[pos] = 0x41;
        msg++;
        pos++;
    }

    for (i = 7; i >= 0; i--) {
        tc[pos] = (u16)(u8)hex[(fault_eip >> (i * 4)) & 0xF];
        ta[pos] = 0x41;
        pos++;
    }

    _disable();
    for (;;) { __asm__ volatile("hlt"); }
}
