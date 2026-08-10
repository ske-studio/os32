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
    printf("       v86 -d <image>\n");
    printf("       v86 -b <image>\n");
    printf("\n");
    printf("  -t   run the V86 self test\n");
    printf("  -b   boot <image>: load its IPL at 1FC0:0000 and run it\n");
    printf("  -d   read sector C0/H0/S1 from <image> through the guest's\n");
    printf("       INT 1Bh and compare it with a direct read\n");
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

    if (argc >= 3 && strcmp(argv[1], "-b") == 0) {
        static const char *reason[] = {
            "none", "guest executed HLT", "escape port",
            "unsupported instruction", "session timeout", "hotkey",
            "fault", "runaway guest (#GP watchdog)"
        };
        printf("V86 boot: %s\n", argv[2]);
        rc = api->v86_boot(argv[2]);
        if (rc < 0) {
            printf("  could not start (rc=%d)\n", rc);
            printf("    -1 = image not found / attach failed\n");
            printf("    -2 = no memory for the guest\n");
            printf("    -3 = could not read the IPL (C0/H0/S1)\n");
            return 1;
        }
        printf("  exit   : %s\n",
               (rc >= 0 && rc <= 7) ? reason[rc] : "?");
        printf("\n");
        printf("  Read the counters from the host:\n");
        printf("    emu_read_mem addr=v86_boot_gp     len=4  (#GP count)\n");
        printf("    emu_read_mem addr=v86_boot_bios   len=4  (BIOS calls)\n");
        printf("    emu_read_mem addr=v86_boot_iotrap len=4  (I/O traps)\n");
        printf("    emu_read_mem addr=v86_boot_ioport len=4  (last port)\n");
        printf("    emu_read_mem addr=v86_boot_gpcs   len=4  (last #GP CS)\n");
        printf("    emu_read_mem addr=v86_boot_gpip   len=4  (last #GP IP)\n");
        printf("    emu_read_mem addr=v86_boot_gpop   len=4  (opcode there)\n");
        printf("    emu_read_mem addr=v86_flt_vec     len=24 (non-#GP fault)\n");
        printf("    emu_read_mem addr=v86_disk_ident_n len=32 (disk counters)\n");
        printf("    emu_read_mem addr=v86_kbd_n_push  len=12 (kbd push/read/drop)\n");
        printf("  Exit the guest with CTRL+GRPH+DEL.\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
        printf("V86 disk test: %s\n", argv[2]);
        rc = api->v86_disktest(argv[2]);
        if (rc == 0) {
            printf("  result : OK\n");
            printf("\n");
            printf("  the guest issued INT 1Bh READ DATA and the bytes it\n");
            printf("  received match a direct read of the image.\n");
            return 0;
        }
        printf("  result : FAILED (rc=%d)\n", rc);
        printf("\n");
        printf("  Inspect the details from the host:\n");
        printf("    emu_read_mem addr=v86_disk_marker len=4  (guest marker)\n");
        printf("    emu_read_mem addr=v86_disk_cmp    len=4  (bytes matched)\n");
        printf("    emu_read_mem addr=v86_disk_reason len=4  (exit reason)\n");
        return 1;
    }

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
