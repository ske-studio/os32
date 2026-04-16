#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  CDINST.C — OS32 CDパッケージインストーラー v1.0                          */
/*                                                                          */
/*  CD-ROM (ISO 9660) 上の .PKG ファイルからHDDにインストールする。           */
/*  - インストールタイプ選択 (Minimal / Normal / Full)                       */
/*  - BOOT.PKG はブートセクタ直接書込み (特殊処理)                          */
/*  - バージョン管理: /etc/pkgdb で追跡                                     */
/* ======================================================================== */

#include "os32api.h"
#include "libos32/pkg.h"

#define CD_MOUNT "/cd0"

/* パッケージファイルパス */
#define PKG_BOOT    "/cd0/BOOT.PKG"
#define PKG_MINIMAL "/cd0/MINIMAL.PKG"
#define PKG_NORMAL  "/cd0/NORMAL.PKG"
#define PKG_FULL    "/cd0/FULL.PKG"
#define PKG_DEBUG   "/cd0/DEBUG.PKG"
#define PKG_APPEND  "/cd0/APPEND.PKG"

/* 色定数 */
#define COL_TITLE  (0xE1 | 0x40)
#define COL_NORMAL 0xE1
#define COL_GREEN  0x81
#define COL_CYAN   0xA1
#define COL_RED    (0x41)
#define COL_YELLOW 0xC1

static KernelAPI *api;

/* ---- ユーティリティ ---- */

static void print(u8 attr, const char *s)
{
    api->kprintf(attr, "%s", s);
}

static void println(u8 attr, const char *s)
{
    api->kprintf(attr, "%s\n", s);
}

static int getkey(void)
{
    return api->kbd_getchar();
}

/* CDマウント確認 */
static int check_cd(void)
{
    OS32_Stat st;
    int ret = api->sys_stat(PKG_BOOT, &st);
    return (ret == 0 && st.st_size > 0) ? 1 : 0;
}

/* PKGファイル存在チェック */
static int pkg_exists(const char *path)
{
    OS32_Stat st;
    return (api->sys_stat(path, &st) == 0) ? 1 : 0;
}

/* ---- インストール処理 ---- */

static int install_package(const char *path, const char *label)
{
    PkgInfo info;
    int ret;

    api->kprintf(COL_CYAN, "\n  Installing %s ...", label);

    ret = pkg_parse(api, path, &info);
    if (ret != PKG_OK) {
        println(COL_RED, " PARSE ERROR");
        return ret;
    }

    api->kprintf(COL_NORMAL, " (%d files, v%d)", info.entry_count,
                 info.header.version);

    ret = pkg_extract(api, path, &info);
    if (ret != PKG_OK) {
        api->kprintf(COL_RED, " EXTRACT ERROR (rc=%d cs=%u os=%u)",
                     ret, info.header.comp_size, info.header.orig_size);
        print(COL_NORMAL, "\n");
        return ret;
    }

    println(COL_GREEN, " OK");
    return PKG_OK;
}

/* ---- メイン ---- */

void __cdecl main(int argc, char **argv, KernelAPI *_api)
{
    int choice;
    int install_debug;
    int install_append;

    (void)argc;
    (void)argv;
    api = _api;

    /* タイトル */
    println(COL_TITLE, "========================================");
    println(COL_TITLE, "      OS32 CD Installer v1.0");
    println(COL_TITLE, "========================================");
    print(COL_NORMAL, "\n");

    /* CD-ROMマウント試行 */
    print(COL_NORMAL, "Mounting CD-ROM... ");
    {
        int mr = api->sys_mount("/cd0", "cd0", "iso9660");
        if (mr == 0) {
            println(COL_GREEN, "OK");
        } else {
            /* 既にマウント済みならOK */
            println(COL_YELLOW, "already mounted or skipped");
        }
    }

    /* CD-ROM内容確認 */
    print(COL_NORMAL, "Checking CD-ROM... ");
    if (!check_cd()) {
        println(COL_RED, "NOT FOUND");
        println(COL_RED, "  BOOT.PKG not found on /cd0/");
        println(COL_NORMAL, "  Please insert OS32 install CD and retry.");
        return;
    }
    println(COL_GREEN, "OK");

    /* パッケージ一覧表示 */
    print(COL_NORMAL, "\n");
    println(COL_NORMAL, "Available packages on CD:");
    if (pkg_exists(PKG_BOOT))    println(COL_CYAN, "  [*] BOOT.PKG");
    if (pkg_exists(PKG_MINIMAL)) println(COL_CYAN, "  [*] MINIMAL.PKG");
    if (pkg_exists(PKG_NORMAL))  println(COL_CYAN, "  [*] NORMAL.PKG");
    if (pkg_exists(PKG_FULL))    println(COL_CYAN, "  [*] FULL.PKG");
    if (pkg_exists(PKG_DEBUG))   println(COL_CYAN, "  [*] DEBUG.PKG");
    if (pkg_exists(PKG_APPEND))  println(COL_CYAN, "  [*] APPEND.PKG");

    /* インストールタイプ選択 */
    print(COL_NORMAL, "\n");
    println(COL_NORMAL, "Install type:");
    println(COL_NORMAL, "  1. Minimal  (shell + basic commands)");
    println(COL_NORMAL, "  2. Normal   (+ editor, tools, manpages)");
    println(COL_NORMAL, "  3. Full     (+ graphics, sound, viewers)");
    println(COL_NORMAL, "  0. Cancel");
    print(COL_NORMAL, "\n");

    print(COL_YELLOW, "Select [0-3]: ");
    do {
        choice = getkey();
    } while (choice < '0' || choice > '3');
    api->kprintf(COL_NORMAL, "%c\n", choice);

    if (choice == '0') {
        println(COL_NORMAL, "Installation cancelled.");
        return;
    }

    /* オプション: Debug / Append */
    install_debug = 0;
    install_append = 0;

    if (pkg_exists(PKG_DEBUG)) {
        print(COL_YELLOW, "Include Debug package? [y/N]: ");
        {
            int k = getkey();
            api->kprintf(COL_NORMAL, "%c\n", k);
            if (k == 'y' || k == 'Y') install_debug = 1;
        }
    }

    if (pkg_exists(PKG_APPEND)) {
        print(COL_YELLOW, "Include Append data? [y/N]: ");
        {
            int k = getkey();
            api->kprintf(COL_NORMAL, "%c\n", k);
            if (k == 'y' || k == 'Y') install_append = 1;
        }
    }

    /* 確認 */
    print(COL_NORMAL, "\n");
    println(COL_RED, "WARNING: This will format the HDD and install OS32.");
    print(COL_YELLOW, "Continue? [y/N]: ");
    {
        int k = getkey();
        api->kprintf(COL_NORMAL, "%c\n", k);
        if (k != 'y' && k != 'Y') {
            println(COL_NORMAL, "Installation cancelled.");
            return;
        }
    }

    /* インストール実行 */
    print(COL_NORMAL, "\n");
    println(COL_GREEN, "=== Installing OS32 ===");

    /* 1. BOOT.PKG (必須) */
    if (install_package(PKG_BOOT, "BOOT") != PKG_OK) {
        println(COL_RED, "Boot installation failed!");
        return;
    }

    /* 2. MINIMAL.PKG (必須) */
    if (install_package(PKG_MINIMAL, "MINIMAL") != PKG_OK) {
        println(COL_RED, "Minimal installation failed!");
        return;
    }

    /* 3. NORMAL.PKG (タイプ2以上) */
    if (choice >= '2' && pkg_exists(PKG_NORMAL)) {
        install_package(PKG_NORMAL, "NORMAL");
    }

    /* 4. FULL.PKG (タイプ3) */
    if (choice >= '3' && pkg_exists(PKG_FULL)) {
        install_package(PKG_FULL, "FULL");
    }

    /* 5. オプション */
    if (install_debug && pkg_exists(PKG_DEBUG)) {
        install_package(PKG_DEBUG, "DEBUG");
    }
    if (install_append && pkg_exists(PKG_APPEND)) {
        install_package(PKG_APPEND, "APPEND");
    }

    /* 完了 */
    print(COL_NORMAL, "\n");
    println(COL_GREEN, "=== Installation Complete ===");
    println(COL_NORMAL, "Remove the floppy disk and reboot from HDD.");
}
