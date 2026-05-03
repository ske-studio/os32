#include "types.h"
#include "io.h"
extern void kprintf(u8 attr, const char *fmt, ...);

void print_debug_gp(u32 esp) {
    kprintf(0x4F, "!!! GP FAULT !!! ESP=%x\n", esp);
    for(;;) io_wait();
}
