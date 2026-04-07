/* ======================================================================== */
/*  KPRINTF.C — 書式付き出力ライブラリ                                      */
/* ======================================================================== */

#include "kprintf.h"
#include "console.h"

/* kprintf用ヘルパー */
static void kprintf_put_dec(u32 val, u8 attr)
    { shell_print_dec(val, attr); }

static void kprintf_put_signed(int val, u8 attr)
{
    if (val < 0) {
        shell_print("-", attr);
        shell_print_dec((u32)(-val), attr);
    } else {
        shell_print_dec((u32)val, attr);
    }
}

static void kprintf_put_hex(u32 val, u8 attr)
{
    char buf[9];
    int i = 7;
    buf[8] = '\0';
    if (val == 0) {
        shell_print("0", attr);
        return;
    }
    while (i >= 0 && val > 0) {
        int d = val & 0xF;
        buf[i] = (d < 10) ? ('0' + d) : ('a' + d - 10);
        val >>= 4;
        i--;
    }
    shell_print(&buf[i + 1], attr);
}

void __cdecl kprintf(u8 attr, const char *fmt, ...)
{
    u32 *args = (u32 *)(&fmt) + 1;
    int ai = 0;
    const char *p = fmt;
    char ch_buf[2] = {0, 0};

    while (*p) {
        if (*p == '%' && p[1]) {
            p++;
            switch (*p) {
                case 'd':
                    kprintf_put_signed((int)args[ai++], attr);
                    break;
                case 'u':
                    kprintf_put_dec(args[ai++], attr);
                    break;
                case 'x':
                    kprintf_put_hex(args[ai++], attr);
                    break;
                case 's':
                    shell_print((const char *)args[ai++], attr);
                    break;
                case 'c':
                    ch_buf[0] = (char)args[ai++];
                    shell_print(ch_buf, attr);
                    break;
                case '%':
                    shell_print("%", attr);
                    break;
                default:
                    ch_buf[0] = *p;
                    shell_print(ch_buf, attr);
                    break;
            }
        } else {
            const char *start = p;
            while (*p && *p != '%') p++;
            {
                u32 len = (u32)(p - start);
                char tmp[80];
                u32 ti;
                if (len > 79) len = 79;
                for (ti = 0; ti < len; ti++) tmp[ti] = start[ti];
                tmp[len] = '\0';
                shell_print(tmp, attr);
            }
            continue;
        }
        p++;
    }
}
