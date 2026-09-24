#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  CDINST.C — OS32 CDパッケージインストーラー v2.0                          */
/*                                                                          */
/*  CD-ROM (ISO 9660) 上の .PKG ファイルからHDDにインストールする。           */
/*  - インストールタイプ選択 (Minimal / Normal / Full)                       */
/*    1 = BOOT + MINIMAL (CUI のみ、起動 FD と同じ集合)、2 = + GUI + NORMAL、 */
/*    3 = + DEBUG。展開は依存の順 MINIMAL → GUI → NORMAL → DEBUG。中身は配備マニフェストの */
/*    タグから tools/mkpkg.py --plan が作る (構成は build/packages.yaml)     */
/*  - 128 項目を超えるパッケージは NAME.PKG, NAME2.PKG, … に分かれている。   */
/*    連番を欠けるまで順に展開する                                          */
/*  - BOOT.PKG はブートセクタ直接書込み (IPL/PT/Loader/Kernel)              */
/*  - 他のPKGは /hd0 マウントポイントにファイルシステム展開                  */
/* ======================================================================== */

#define OS32_DBG_SERIAL
#include "os32api.h"
#include "rt/dbgserial.h"
#include "rt/pkg.h"

#define CD_MOUNT "/cd0"
#define HDD_MOUNT "/hd0"

/* パッケージファイルパス。BOOT は分割しない (セクタへ直接書く) */
#define PKG_BOOT    "/cd0/BOOT.PKG"
/* 分割されうるパッケージのベース名 (build/packages.yaml と一致させる。
 * make check-packages-host が突き合わせる)。連番は 2..PKG_SERIES_MAX */
#define PKG_BASE_MINIMAL "MINIMAL"
#define PKG_BASE_GUI     "GUI"
#define PKG_BASE_NORMAL  "NORMAL"
#define PKG_BASE_DEBUG   "DEBUG"
#define PKG_SERIES_MAX   9
/* "/cd0/" + 8 文字 + ".PKG" + NUL */
#define PKG_PATH_BUF     32

/* PC-98ジオメトリ定数 */
#define PC98_HEADS   8
#define PC98_SECTORS 17
#define PC98_CYL0_SECTORS  (PC98_HEADS * PC98_SECTORS)  /* 136 */
/* ブート予約シリンダ数。LBA 0=IPL / 1=PT / 2..5=loader / 6..=kernel.bin /
 * 262..=sqlite.bin を収めるため 12 シリンダ (1632 セクタ) 必要。
 * tools/nhd_deploy.py の HDD_PARTITION_LBA と必ず一致させること。 */
#define PC98_BOOT_CYLS     12
#define HDD_PARTITION_LBA  (PC98_CYL0_SECTORS * PC98_BOOT_CYLS)  /* 1632 */

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
    int ch;
    for (;;) {
        ch = api->kbd_trygetchar();
        if (ch > 0) return ch;
        ch = api->serial_trygetchar();
        if (ch > 0) return ch;
    }
}

static int check_cd(void)
{
    OS32_Stat st;
    return (api->sys_stat(PKG_BOOT, &st) == 0 && st.st_size > 0) ? 1 : 0;
}

static int pkg_exists(const char *path)
{
    OS32_Stat st;
    return (api->sys_stat(path, &st) == 0) ? 1 : 0;
}

/* 分割の n 本目 (1 始まり) のパス: n = 1 は "/cd0/NORMAL.PKG"、
 * n >= 2 は "/cd0/NORMAL<n>.PKG" */
static void pkg_series_path(char *buf, const char *base, int n)
{
    const char *pre = CD_MOUNT "/";
    const char *ext = ".PKG";
    int i = 0;

    while (*pre && i < PKG_PATH_BUF - 1) buf[i++] = *pre++;
    while (*base && i < PKG_PATH_BUF - 1) buf[i++] = *base++;
    if (n >= 2 && i < PKG_PATH_BUF - 1) buf[i++] = (char)('0' + n);
    while (*ext && i < PKG_PATH_BUF - 1) buf[i++] = *ext++;
    buf[i] = '\0';
}

/* 媒体にある分割の本数 (0 = 1 本も無い) */
static int pkg_series_count(const char *base)
{
    char path[PKG_PATH_BUF];
    int n;

    for (n = 1; n <= PKG_SERIES_MAX; n++) {
        pkg_series_path(path, base, n);
        if (!pkg_exists(path)) break;
    }
    return n - 1;
}

/* 大文字小文字を無視してサフィックス一致判定 */
static int str_endswith(const char *s, const char *suffix)
{
    int slen = 0, suflen = 0;
    const char *p;
    char c1, c2;

    for (p = s; *p; p++) slen++;
    for (p = suffix; *p; p++) suflen++;
    if (suflen > slen) return 0;

    p = s + slen - suflen;
    while (*suffix) {
        c1 = *p; c2 = *suffix;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        p++;
        suffix++;
    }
    return 1;
}

/* ======================================================================== */
/*  パーティションテーブル作成 (LBA 1)                                       */
/* ======================================================================== */

static int write_partition_table(int ide_drv, u32 total_sectors)
{
    u8 pt[512];
    u32 end_cyl;
    int i;

    for (i = 0; i < 512; i++) pt[i] = 0;

    pt[0] = 0x80;  /* bootable */
    pt[1] = 0xE2;  /* system type: ext2 */

    pt[6] = 0;               /* start sector */
    pt[7] = 0;               /* start head */
    pt[8] = PC98_BOOT_CYLS;  /* start cylinder low */
    pt[9] = 0;               /* start cylinder high */

    end_cyl = (total_sectors / PC98_CYL0_SECTORS) - 1;
    pt[10] = (u8)(PC98_SECTORS - 1);
    pt[11] = (u8)(PC98_HEADS - 1);
    pt[12] = (u8)(end_cyl & 0xFF);
    pt[13] = (u8)((end_cyl >> 8) & 0xFF);

    pt[16] = 'O'; pt[17] = 'S'; pt[18] = '3'; pt[19] = '2';
    for (i = 20; i < 32; i++) pt[i] = ' ';

    return api->ide_write_sector(ide_drv, 1, pt);
}

/* ======================================================================== */
/*  BOOT.PKG → IDEセクタ直接書き込み                                         */
/*  boot_hdd.bin→LBA0, loader_hdd.bin→LBA2+, kernel.bin→LBA6+             */
/* ======================================================================== */

static int install_boot_sectors(int ide_drv, u32 total_sectors)
{
    PkgInfo info;
    int ret, i;
    u8 *data_buf;
    u32 comp_size;
    int rd;

    print(COL_CYAN, "\n  Installing boot sectors...");

    ret = pkg_parse(api, PKG_BOOT, &info);
    if (ret != PKG_OK) {
        println(COL_RED, " BOOT.PKG parse failed");
        return ret;
    }

    DBGF("[cdinst] BOOT.PKG: %d files, flags=0x%02x",
         info.entry_count, info.header.flags);

    /* BOOT.PKGは非圧縮でなければならない */
    if (info.header.flags & PKG_FLAG_LZSS) {
        println(COL_RED, " BOOT.PKG must not be LZSS compressed");
        return PKG_ERR_CORRUPT;
    }

    comp_size = info.header.comp_size;
    data_buf = (u8 *)api->mem_alloc(comp_size);
    if (!data_buf) {
        println(COL_RED, " out of memory");
        return PKG_ERR_NOMEM;
    }

    /* データ部を読み込み */
    {
        int fd = api->sys_open(PKG_BOOT, KAPI_O_RDONLY);
        if (fd < 0) {
            api->mem_free(data_buf);
            return PKG_ERR_IO;
        }

        /* pkg_parseで計算済みのdata_offsetを使用 */
        api->sys_lseek(fd, (int)info.data_offset, SEEK_SET);
        rd = api->sys_read(fd, data_buf, (int)comp_size);
        api->sys_close(fd);

        if (rd != (int)comp_size) {
            api->mem_free(data_buf);
            return PKG_ERR_IO;
        }
    }

    /* 各ファイルをIDEセクタに書き込み */
    {
        u32 offset = 0;

        for (i = 0; i < info.entry_count; i++) {
            const PkgEntry *ent = &info.entries[i];
            u8 *fdata;
            int fsize, nsects;

            if (ent->type != PKG_TYPE_FILE) continue;
            if (offset + ent->size > (u32)rd) break;

            fdata = data_buf + offset;
            fsize = (int)ent->size;

            if (str_endswith(ent->path, "boot_hdd.bin")) {
                /* IPL → LBA 0 */
                u8 ipl[512];
                int j;
                for (j = 0; j < 512; j++) ipl[j] = 0;
                for (j = 0; j < fsize && j < 512; j++) ipl[j] = fdata[j];
                ipl[8] = PC98_HEADS;
                ipl[9] = PC98_SECTORS;
                ipl[510] = 0x55;
                ipl[511] = 0xAA;

                ret = api->ide_write_sector(ide_drv, 0, ipl);
                if (ret != 0) {
                    api->kprintf(COL_RED, " IPL write err=%d\n", ret);
                    api->mem_free(data_buf);
                    return -1;
                }
                DBGF("[cdinst] IPL written LBA0 (%d bytes)", fsize);
            }
            else if (str_endswith(ent->path, "loader_hdd.bin")) {
                /* ローダ → LBA 2+ */
                nsects = (fsize + 511) / 512;
                ret = api->ide_write_sectors(ide_drv, 2, nsects, fdata);
                if (ret != 0) {
                    api->kprintf(COL_RED, " Loader write err=%d\n", ret);
                    api->mem_free(data_buf);
                    return -1;
                }
                DBGF("[cdinst] Loader written LBA2 (%d bytes)", fsize);
            }
            /* 注: kernel.bin の LBA 6 への生書き込みは廃止した。
             * IPL (boot_hdd.asm) は LBA 2 から 16 セクタを読むため、
             * ローダ領域は LBA 2..17 を占有し LBA 6 と衝突する。
             * loader v3 はカーネルを ext2 上の /boot/vmkernel.lz4 から
             * 読むので、生カーネルの書き込み自体が不要。 */

            offset += ent->size;
        }
    }

    api->mem_free(data_buf);

    /* パーティションテーブル → LBA 1 */
    ret = write_partition_table(ide_drv, total_sectors);
    if (ret != 0) {
        api->kprintf(COL_RED, " PT write err=%d\n", ret);
        return -1;
    }
    DBG("[cdinst] PT written LBA1");

    println(COL_GREEN, " OK");
    return 0;
}

/* ======================================================================== */
/*  パッケージ展開 (パスを /hd0 配下にリマップ)                              */
/* ======================================================================== */

static int install_package_hd(const char *path, const char *label)
{
    PkgInfo info;
    int ret, i;

    api->kprintf(COL_CYAN, "\n  Installing %s ...", label);

    ret = pkg_parse(api, path, &info);
    if (ret != PKG_OK) {
        println(COL_RED, " PARSE ERROR");
        return ret;
    }

    api->kprintf(COL_NORMAL, " (%d files, v%d)", info.entry_count,
                 info.header.version);

    /* 前置 (/hd0) で溢れる項目があれば**展開を始める前に**断る (票
     * TASK_VFS_FD_PATH v3 の追記)。以前は溢れた分を切り詰め、後のファイルが
     * 前のファイルを上書きし得た。外部で作られた PKG への消費側の保護。 */
    i = pkg_first_overflow(&info, 4);   /* strlen("/hd0") */
    if (i >= 0) {
        api->kprintf(COL_RED, " PATH TOO LONG: %s\n", info.entries[i].path);
        return PKG_ERR_TOOLONG;
    }

    /* パスに /hd0 プレフィックスを追加 */
    for (i = 0; i < info.entry_count; i++) {
        char orig[PKG_MAX_PATH];
        int j;

        for (j = 0; j < PKG_MAX_PATH - 1 && info.entries[i].path[j]; j++)
            orig[j] = info.entries[i].path[j];
        orig[j] = '\0';

        info.entries[i].path[0] = '/';
        info.entries[i].path[1] = 'h';
        info.entries[i].path[2] = 'd';
        info.entries[i].path[3] = '0';
        for (j = 0; orig[j] && j + 4 < PKG_MAX_PATH - 1; j++)
            info.entries[i].path[4 + j] = orig[j];
        info.entries[i].path[4 + j] = '\0';
    }

    ret = pkg_extract(api, path, &info);
    if (ret != PKG_OK) {
        api->kprintf(COL_RED, " EXTRACT ERROR (rc=%d)", ret);
        print(COL_NORMAL, "\n");
        return ret;
    }

    println(COL_GREEN, " OK");
    return PKG_OK;
}

/* ======================================================================== */
/*  パッケージの展開 (MINIMAL → GUI → NORMAL → DEBUG) と同期                */
/*                                                                          */
/*  **どのパッケージの失敗でも止める** (Codex / Opus 実装レビュー ラリー 1)。 */
/*  以前は MINIMAL だけ戻り値を見ていたので、NORMAL 以降が PATH TOO LONG や  */
/*  容量不足で失敗しても後続へ進み、最後に "Installation Complete" を出して   */
/*  いた。失敗したら失敗したパッケージ名を出し、完了表示は出さない。         */
/*  同期 (vfs_sync) の失敗も完了扱いにしない。                               */
/*  選んだ型のパッケージが媒体に 1 本も無いのも失敗 (黙って飛ばすと「Full を */
/*  選んだのに試験が入っていない」HDD が完了扱いになる)。                    */
/*  戻り値: PKG_OK = 完了 / それ以外 = 失敗した段の値                        */
/* ======================================================================== */

static int install_step(const char *path, const char *label)
{
    int ret = install_package_hd(path, label);
    if (ret != PKG_OK) {
        api->kprintf(COL_RED, "\n%s installation failed (rc=%d)!\n", label, ret);
        println(COL_RED, "Installation aborted. The HDD is incomplete.");
    }
    return ret;
}

/* base の分割をすべて (NAME.PKG, NAME2.PKG, …) 順に展開する */
static int install_series(const char *base)
{
    char path[PKG_PATH_BUF];
    char label[PKG_PATH_BUF];
    int n, count, ret;

    count = pkg_series_count(base);
    if (count == 0) {
        api->kprintf(COL_RED, "\n%s.PKG not found on CD!\n", base);
        println(COL_RED, "Installation aborted. The HDD is incomplete.");
        return PKG_ERR_IO;
    }
    for (n = 1; n <= count; n++) {
        pkg_series_path(path, base, n);
        /* 表示名 = パスから "/cd0/" と ".PKG" を除いたもの */
        {
            int i = 0;
            const char *p = path + sizeof(CD_MOUNT);
            while (*p && *p != '.' && i < PKG_PATH_BUF - 1) label[i++] = *p++;
            label[i] = '\0';
        }
        ret = install_step(path, label);
        if (ret != PKG_OK) return ret;
    }
    return PKG_OK;
}

static int install_packages(int choice)
{
    int ret;

    /* 2. MINIMAL → /hd0 配下に展開 (全タイプ) */
    ret = install_series(PKG_BASE_MINIMAL);
    if (ret != PKG_OK) return ret;

    /* 3. GUI → NORMAL (タイプ2以上)。GUI (gshell / libos32gui.shlib) を先に
     * 置く — NORMAL は GUI に依存しないが、GUI の後に展開すれば途中で止まっても
     * GUI 一式は揃っている。Minimal だけなら GUI は無く、system.cfg が GUI=1
     * でもカーネルが "gshell load failed -> CUI shell" で CUI に落ちる */
    if (choice >= '2') {
        ret = install_series(PKG_BASE_GUI);
        if (ret != PKG_OK) return ret;
        ret = install_series(PKG_BASE_NORMAL);
        if (ret != PKG_OK) return ret;
    }

    /* 4. DEBUG (タイプ3 = Full) */
    if (choice >= '3') {
        ret = install_series(PKG_BASE_DEBUG);
        if (ret != PKG_OK) return ret;
    }

    /* ファイルシステムをディスクに同期 */
    print(COL_CYAN, "\n  Syncing filesystem...");
    {
        int sret = api->vfs_sync();
        DBGF("[cdinst] vfs_sync=%d", sret);
        if (sret != 0) {
            api->kprintf(COL_RED, " FAILED (rc=%d)\n", sret);
            println(COL_RED, "Installation aborted. The HDD is incomplete.");
            return sret;
        }
        println(COL_GREEN, " OK");
    }

    /* 完了 */
    print(COL_NORMAL, "\n");
    println(COL_GREEN, "=== Installation Complete ===");
    println(COL_NORMAL, "Remove the floppy disk and reboot from HDD.");
    return PKG_OK;
}

/* ======================================================================== */
/*  メイン                                                                   */
/* ======================================================================== */

void __cdecl main(int argc, char **argv, KernelAPI *_api)
{
    int choice;
    int ide_drv;
    u32 total_sects;

    (void)argc;
    (void)argv;
    api = _api;
    dbg_init(api);
    DBG("[cdinst] started");

    ide_drv = 0;
    total_sects = 0;

    println(COL_TITLE, "========================================");
    println(COL_TITLE, "      OS32 CD Installer v2.0");
    println(COL_TITLE, "========================================");
    print(COL_NORMAL, "\n");

    /* CD-ROMマウント */
    print(COL_NORMAL, "Mounting CD-ROM... ");
    {
        int mr = api->sys_mount("/cd0", "cd0", "iso9660");
        if (mr == 0)
            println(COL_GREEN, "OK");
        else
            println(COL_YELLOW, "already mounted or skipped");
    }

    /* CD-ROM確認 */
    print(COL_NORMAL, "Checking CD-ROM... ");
    if (!check_cd()) {
        println(COL_RED, "NOT FOUND");
        println(COL_RED, "  BOOT.PKG not found on /cd0/");
        return;
    }
    println(COL_GREEN, "OK");

    /* パッケージ一覧 */
    print(COL_NORMAL, "\n");
    println(COL_NORMAL, "Available packages on CD:");
    if (pkg_exists(PKG_BOOT))    println(COL_CYAN, "  [*] BOOT.PKG");
    {
        static const char *const bases[4] = {
            PKG_BASE_MINIMAL, PKG_BASE_GUI, PKG_BASE_NORMAL, PKG_BASE_DEBUG
        };
        int b, n, count;
        for (b = 0; b < 4; b++) {
            count = pkg_series_count(bases[b]);
            for (n = 1; n <= count; n++) {
                char path[PKG_PATH_BUF];
                pkg_series_path(path, bases[b], n);
                api->kprintf(COL_CYAN, "  [*] %s\n", path + sizeof(CD_MOUNT));
            }
        }
    }

    /* インストールタイプ選択 (中身は build/packages.yaml の振り分け) */
    print(COL_NORMAL, "\n");
    println(COL_NORMAL, "Install type:");
    println(COL_NORMAL, "  1. Minimal  (CUI only: shell + basic commands, same as the boot FD)");
    println(COL_NORMAL, "  2. Normal   (+ GUI shell, commands, apps, manpages, IME dictionary, data)");
    println(COL_NORMAL, "  3. Full     (+ test programs and test data)");
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

    /* 選んだ型に要るパッケージが媒体に揃っているか、HDD を消す前に見る */
    if (pkg_series_count(PKG_BASE_MINIMAL) == 0
        || (choice >= '2' && pkg_series_count(PKG_BASE_GUI) == 0)
        || (choice >= '2' && pkg_series_count(PKG_BASE_NORMAL) == 0)
        || (choice >= '3' && pkg_series_count(PKG_BASE_DEBUG) == 0)) {
        println(COL_RED, "ERROR: the CD lacks a package for this install type.");
        return;
    }

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

    /* === インストール実行 === */
    print(COL_NORMAL, "\n");
    println(COL_GREEN, "=== Installing OS32 ===");

    /* HDD自動検出 */
    {
        int drv, n;
        int found = -1;

        n = api->dev_count();
        for (drv = 0; drv < n; drv++) {
            char name[32];
            int type;
            u32 s;
            if (api->dev_get_info(drv, name, 32, &type, &s) == 0) {
                if (type == 1 && s > 0
                    && name[0] == 'h' && name[1] == 'd') {
                    found = drv;
                    ide_drv = name[2] - '0';
                    total_sects = s;
                    break;
                }
            }
        }

        if (found < 0) {
            println(COL_RED, "ERROR: No HDD device found!");
            return;
        }

        DBGF("[cdinst] HDD: ide_drv=%d sects=%lu",
             ide_drv, (unsigned long)total_sects);

        /* 起動時の自動マウントで /hd0 が既に掴まれていることがある。
         * その ctx はフォーマット前のスーパーブロック/GDT を保持したままなので、
         * 外さずに進めると mkdir が旧FSのエントリを見つけて EXIST(-6) になる。 */
        api->sys_umount("/hd0");

        /* パーティションテーブル書き込み (フォーマットより先に行うこと)
         * ext2_format は内部で ext2_find_partition() が読む LBA 1 の
         * パーティションテーブルから base_lba を決める。後から書くと
         * 「旧テーブルの位置にフォーマットし、新テーブルは別の位置を指す」
         * 状態になり、ローダが ext2 をマウントできなくなる。 */
        print(COL_CYAN, "  Writing partition table...");
        if (write_partition_table(ide_drv, total_sects) != 0) {
            println(COL_RED, " FAILED");
            return;
        }
        println(COL_GREEN, " OK");

        /* ext2フォーマット */
        {
            u32 part_sects = total_sects - HDD_PARTITION_LBA;
            int fret;

            api->kprintf(COL_CYAN,
                         "\n  Formatting HDD (ide%d, %u part sects)...",
                         ide_drv, part_sects);

            fret = api->ext2_format(ide_drv, part_sects);
            DBGF("[cdinst] ext2_format=%d", fret);
            if (fret != 0) {
                api->kprintf(COL_RED, " FAILED (rc=%d)\n", fret);
                return;
            }
        }
        println(COL_GREEN, " OK");

        /* HDDを /hd0 にマウント (FDDの / と共存) */
        print(COL_CYAN, "  Mounting HDD at /hd0...");
        {
            int mret = api->sys_mount("/hd0", "hd0", "ext2");
            DBGF("[cdinst] mount /hd0=%d", mret);
            if (mret != 0) {
                api->kprintf(COL_RED, " FAILED (rc=%d)\n", mret);
                return;
            }
        }
        println(COL_GREEN, " OK");

        /* ディレクトリ構造を作成 */
        print(COL_CYAN, "  Creating directories...");
        {
            int mr;
            mr = api->sys_mkdir("/hd0/sys");
            if (mr != 0) {
                api->kprintf(COL_RED, "\n    mkdir failed (rc=%d)\n", mr);
                return;
            }
            api->sys_mkdir("/hd0/boot");   /* loader v3 が読む vmkernel.lz4 の置き場 */
            api->sys_mkdir("/hd0/bin");
            api->sys_mkdir("/hd0/sbin");
            api->sys_mkdir("/hd0/usr");
            api->sys_mkdir("/hd0/usr/bin");
            api->sys_mkdir("/hd0/usr/man");
            api->sys_mkdir("/hd0/etc");
            api->sys_mkdir("/hd0/data");
            api->sys_mkdir("/hd0/home");
            api->sys_mkdir("/hd0/home/user");
            api->sys_mkdir("/hd0/tmp");
        }
        println(COL_GREEN, " OK");
    }

    /* 1. BOOT.PKG → IDEセクタ直接書き込み */
    if (install_boot_sectors(ide_drv, total_sects) != 0) {
        println(COL_RED, "Boot sector installation failed!");
        return;
    }

    /* 2〜5. パッケージの展開・同期・完了表示 */
    (void)install_packages(choice);
}
