#include <stdio.h>
#include <string.h>
#include <os32_kapi_shared.h>

int main(int argc, char **argv)
{
    const char *img_path = NULL;
    const char *cmdline = NULL;
    int use_2dd = 0;  /* 1=2DD(640KB), 2=2DD(720KB) */
    int native_mode = 0;  /* 1=ネイティブPC-98ソフトモード */
    int debug_mode = 0;   /* 1=デバッグログ出力 */
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] [image_path] [cmdline]\n", argv[0]);
            printf("       %s              (boot from physical FDD as 2HD)\n", argv[0]);
            printf("       %s -2dd         (boot from physical FDD as 2DD 640KB)\n", argv[0]);
            printf("       %s path.fdi     (boot FreeDOS from image)\n", argv[0]);
            printf("       %s -native Ys.D88 (boot native PC-98 software)\n", argv[0]);
            printf("       %s -d -native Ys.D88 (with debug log)\n", argv[0]);
            printf("Options:\n");
            printf("  -native   Native PC-98 mode (no DOS, no timeout)\n");
            printf("  -d        Enable debug logging to /host/debug/\n");
            printf("  -2dd      Physical FDD as 2DD 640KB\n");
            printf("  -2dd-9    Physical FDD as 2DD 720KB\n");
            printf("  -h        Show this help\n");
            printf("Exit: Ctrl+GRPH+DEL\n");
            return 0;
        }
        if (strcmp(argv[i], "-native") == 0) {
            native_mode = 1;
            continue;
        }
        if (strcmp(argv[i], "-2dd") == 0) {
            use_2dd = 1;
            continue;
        }
        if (strcmp(argv[i], "-2dd-9") == 0) {
            use_2dd = 2;
            continue;
        }
        if (strcmp(argv[i], "-d") == 0) {
            debug_mode = 1;
            continue;
        }
        if (img_path == NULL) {
            img_path = argv[i];
        } else if (cmdline == NULL) {
            cmdline = argv[i];
        }
    }

    extern KernelAPI *kapi;

    /* デバッグモード設定 */
    if (debug_mode) {
        kapi->sys_v86_set_debug(1);
        printf("[DEBUG] V86 debug logging enabled\n");
    }

    /* ネイティブモード: ディスクイメージ必須 */
    if (native_mode) {
        if (img_path == NULL) {
            printf("Error: -native requires an image file path\n");
            return 1;
        }
        printf("Starting native PC-98 software: %s\n", img_path);
        printf("Exit with Ctrl+GRPH+DEL\n");
        rc = kapi->sys_v86_boot_native(img_path);
    } else if (img_path == NULL) {
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
        /* イメージファイル指定: FreeDOSモード */
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

    /* デバッグモード終了 */
    if (debug_mode) {
        kapi->sys_v86_set_debug(0);
        printf("[DEBUG] Debug log saved to /host/debug/\n");
    }

    printf("VDOS exited normally.\n");
    return 0;
}
