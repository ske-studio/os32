#include <stdio.h>
#include <string.h>
#include <os32_kapi_shared.h>

int main(int argc, char **argv)
{
    const char *img_path = NULL;
    const char *cmdline = NULL;
    int use_2dd = 0;  /* 1=2DD(640KB), 2=2DD(720KB) */
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] [fdi_image_path] [cmdline]\n", argv[0]);
            printf("       %s              (boot from physical FDD as 2HD)\n", argv[0]);
            printf("       %s -2dd         (boot from physical FDD as 2DD 640KB)\n", argv[0]);
            printf("       %s path.fdi     (boot from FDI/IMG image file)\n", argv[0]);
            printf("Boot FreeDOS in V86 environment.\n");
            return 0;
        }
        if (strcmp(argv[i], "-2dd") == 0) {
            use_2dd = 1;
            continue;
        }
        if (strcmp(argv[i], "-2dd-9") == 0) {
            use_2dd = 2;
            continue;
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
        if (use_2dd) {
            printf("Starting VDOS from physical FDD (2DD %s)...\n",
                   use_2dd == 2 ? "720KB" : "640KB");
            if (cmdline) {
                printf("Auto-typing command: %s\n", cmdline);
            }
            rc = kapi->sys_v86_boot_physical_ex(0, use_2dd, cmdline);
        } else {
            printf("Starting VDOS from physical FDD (2HD)...\n");
            if (cmdline) {
                printf("Auto-typing command: %s\n", cmdline);
            }
            rc = kapi->sys_v86_boot_physical(0, cmdline);
        }
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
