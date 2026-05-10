/* v86.c — V86 ディスクイメージブートコマンド */
#include <stdio.h>
#include <string.h>
#include <os32_kapi_shared.h>

int main(int argc, char **argv)
{
    const char *img_path = NULL;
    const char *auto_cmd = NULL;
    int use_fdd = 0;
    int use_2dd = 0;  /* 1=2DD(640KB), 2=2DD(720KB) */
    int debug_mode = 0;
    int rc;
    int i;

    extern KernelAPI *kapi;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: v86 [options] <image_path>\n");
            printf("       v86 -fdd [-2dd|-2dd-9]\n");
            printf("Options:\n");
            printf("  -fdd      Boot from physical FDD\n");
            printf("  -2dd      Physical FDD as 2DD 640KB\n");
            printf("  -2dd-9    Physical FDD as 2DD 720KB\n");
            printf("  -c \"cmd\"  Auto-type command after boot\n");
            printf("  -d        Enable debug logging\n");
            printf("  -h        Show this help\n");
            printf("Exit: Ctrl+GRPH+DEL\n");
            return 0;
        }
        if (strcmp(argv[i], "-fdd") == 0)   { use_fdd = 1; continue; }
        if (strcmp(argv[i], "-d") == 0)     { debug_mode = 1; continue; }
        if (strcmp(argv[i], "-2dd") == 0)   { use_2dd = 1; continue; }
        if (strcmp(argv[i], "-2dd-9") == 0) { use_2dd = 2; continue; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            auto_cmd = argv[++i]; continue;
        }
        if (img_path == NULL) {
            img_path = argv[i];
        }
    }

    /* 引数なし + FDDなし -> エラー */
    if (img_path == NULL && !use_fdd) {
        printf("Error: image path required. Use 'v86 -h' for help.\n");
        return 1;
    }

    if (debug_mode) {
        kapi->sys_v86_set_debug(1);
    }

    if (use_fdd) {
        /* 実FDDブート */
        if (use_2dd) {
            rc = kapi->sys_v86_boot_physical_ex(0, use_2dd, auto_cmd);
        } else {
            rc = kapi->sys_v86_boot_physical(0, auto_cmd);
        }
    } else {
        /* イメージブート */
        printf("Booting: %s\n", img_path);
        if (auto_cmd) {
            printf("Auto-type: %s\n", auto_cmd);
        }
        printf("Exit: Ctrl+GRPH+DEL\n");
        rc = kapi->sys_v86_boot_image(img_path, auto_cmd);
    }

    if (debug_mode) {
        kapi->sys_v86_set_debug(0);
    }

    if (rc < 0) {
        printf("v86 failed (rc=%d)\n", rc);
        return 1;
    }
    return 0;
}
