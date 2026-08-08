/* ======================================================================== */
/*  V86.C — 仮想8086モード サブシステムの操作コマンド                       */
/*                                                                          */
/*  Usage: v86 -t     セルフテストを実行する                                */
/*         v86        使い方を表示する                                      */
/*                                                                          */
/*  16bit ゲスト (PC-98 ネイティブゲーム / DOS) を OS32 上で走らせるための   */
/*  ランタイムの入口。現時点で提供しているのはセルフテストだけで、          */
/*  ディスクイメージからのブートは未実装。                                  */
/*                                                                          */
/*  設計と実測データ: docs/tasks/v86v2/                                     */
/* ======================================================================== */
#include "os32api.h"
#include <string.h>
#include <stdio.h>

static KernelAPI *api;

static void usage(void)
{
    printf("Usage: v86 -t\n");
    printf("\n");
    printf("  -t   run the V86 self test\n");
    printf("\n");
    printf("The self test runs a small 16bit program under virtual 8086\n");
    printf("mode and checks that it can write memory, that allowed I/O\n");
    printf("ports pass through untrapped, that denied ports are trapped\n");
    printf("and emulated, and that a real hardware IRQ reaches the guest's\n");
    printf("own interrupt handler.\n");
}

int main(int argc, char **argv, KernelAPI *kapi)
{
    int rc;

    api = kapi;

    if (argc < 2 || strcmp(argv[1], "-t") != 0) {
        usage();
        return 1;
    }

    printf("V86 self test...\n");
    rc = api->v86_selftest();

    if (rc == 0) {
        printf("  result : OK\n");
        printf("\n");
        printf("  guest ran under V86, wrote memory, passed through the\n");
        printf("  allowed ports, was trapped on the denied ones, and its\n");
        printf("  own ISR received the reflected timer IRQ.\n");
        return 0;
    }

    printf("  result : FAILED (rc=%d)\n", rc);
    printf("\n");
    printf("  Inspect the details from the host:\n");
    printf("    emu_read_mem addr=v86_smoke_reason len=4   (exit reason)\n");
    printf("    emu_read_mem addr=v86_smoke_iotrap len=4   (I/O traps)\n");
    printf("    emu_read_mem addr=v86_smoke_gcnt   len=4   (guest ISR count)\n");
    printf("    emu_fault                                  (if it crashed)\n");
    return 1;
}
