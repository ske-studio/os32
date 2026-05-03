#include <stdio.h>
#include <string.h>
#include <os32_kapi_shared.h>

int main(int argc, char **argv)
{
    const char *img_path = NULL;
    const char *cmdline = NULL;
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [fdi_image_path] [cmdline]\n", argv[0]);
            printf("       %s                  (boot from FDD)\n", argv[0]);
            printf("Boot FreeDOS in V86 environment.\n");
            printf("  No args: boot from physical FDD (NP21/W mounted disk)\n");
            printf("  path:    boot from FDI/IMG image file\n");
            return 0;
        }
        if (img_path == NULL) {
            img_path = argv[i];
        } else if (cmdline == NULL) {
            cmdline = argv[i];
        }
    }

    extern KernelAPI *kapi;

    if (img_path == NULL) {
        /* 引数なし: 実FDDからブート */
        printf("Starting VDOS from physical FDD...\n");
        if (cmdline) {
            printf("Auto-typing command: %s\n", cmdline);
        }
        rc = kapi->sys_v86_boot_physical(0, cmdline);
    } else {
        /* イメージファイル指定 */
        printf("Starting VDOS with image: %s\n", img_path);
        if (cmdline) {
            printf("Auto-typing command: %s\n", cmdline);
        }
        rc = kapi->sys_v86_boot_freedos(img_path, cmdline);
    }

    if (rc < 0) {
        printf("VDOS failed to start (rc=%d)\n", rc);
        return 1;
    }

    printf("VDOS exited normally.\n");
    return 0;
}
