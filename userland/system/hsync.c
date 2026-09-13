#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  HSYNC.C — HostDrv同期コマンド                                           */
/*                                                                          */
/*  /host (HostDrvマウントポイント) の内容を ext2 ルート (/) に同期する。     */
/*  ファイルサイズが異なるもののみコピーし、同一サイズはスキップする。        */
/*                                                                          */
/*  使い方:                                                                 */
/*    hsync              — /host 配下を / に同期 (**sys は除く**)           */
/*    hsync bin           — /host/bin/ → /bin/ のみ同期                    */
/*    hsync -f            — サイズ無関係に全上書き (sys は除く)             */
/*    hsync sys           — /sys を明示指定したときだけ同期する             */
/*    hsync -f sys        — /host/sys/ を強制同期                          */
/*                                                                          */
/*  **既定で sys を外す理由**: /sys には稼働中の常駐シェル (shell.bin)、     */
/*  共有ライブラリ (lib/)、unicode.bin、フォントが入っている。走っている     */
/*  ものを背後から差し替えると、次の exec まで実体と食い違う。入れ替えたい   */
/*  ときは `hsync sys` と明示する (2026-09-09、ホットデプロイ撤去に伴い)。   */
/* ======================================================================== */

#include "os32api.h"

/* コピー先が /etc/settings.db* かを字句で見る純関数 (票 S0-D / D0)。
 * HostDrv に古い settings.db が残っていても NHD の本体を切り詰めない。
 * 実体は userland/system/hsync_protect.inc、ホスト試験は
 * tools/tests/test_hsync_protect.py。 */
#include "hsync_protect.inc"

#define FILE_BUF_SIZE  (64 * 1024)  /* 64KB */
#define MAX_FILES      128
#define MAX_DEPTH      8

static KernelAPI *api;
static u8 *file_buf;

/* 統計 */
static int g_copied;
static int g_skipped;
static int g_errors;
static int g_force;
/* 保護対象として除外した件数 */
static int g_protected;
/* サブディレクトリに sys が明示されたか (既定の全体同期では外す) */
static int g_want_sys;

/* ======== 文字列ユーティリティ ======== */

static int str_len(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

static void str_cpy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void str_cat(char *dst, const char *src)
{
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ======== ファイルリスト ======== */

typedef struct {
    char names[MAX_FILES][64];
    u8   types[MAX_FILES];
    u32  sizes[MAX_FILES];
    int  count;
} FileList;

static void ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    FileList *fl = (FileList *)ctx;
    int i;
    if (fl->count >= MAX_FILES) return;

    i = 0;
    while (entry->name[i] && i < 63) {
        fl->names[fl->count][i] = entry->name[i];
        i++;
    }
    fl->names[fl->count][i] = '\0';
    fl->types[fl->count] = entry->type;
    fl->sizes[fl->count] = entry->size;
    fl->count++;
}

/* ======== ファイルコピー (Stream I/O) ======== */

static int copy_file(const char *src, const char *dst)
{
    int fd_src, fd_dst;
    int rd;
    int total = 0;

    fd_src = api->sys_open(src, KAPI_O_RDONLY);
    if (fd_src < 0) return -1;

    fd_dst = api->sys_open(dst, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd_dst < 0) {
        api->sys_close(fd_src);
        return -2;
    }

    while (1) {
        rd = api->sys_read(fd_src, file_buf, FILE_BUF_SIZE);
        if (rd <= 0) break;
        if (api->sys_write(fd_dst, file_buf, rd) != rd) {
            api->sys_close(fd_src);
            api->sys_close(fd_dst);
            return -3;
        }
        total += rd;
    }

    api->sys_close(fd_src);
    api->sys_close(fd_dst);
    return total;
}

/* ======== 保護対象の判定 ======== */

/* 現に存在する /etc/settings.db* の実体。同期を始める前に 1 度だけ集める
 * (ファイルごとに何度も stat すると 16MHz の実機では効く)。
 *
 * 表の小文字 5 名を決め打ちで stat するだけでは足りない: ext2 は大文字小文字を
 * 区別するので `/etc/SETTINGS.DB` が本体でも拾えず、そこへの hardlink を
 * `hsync -f bin` が上書きしてしまう (往復 1 の B5)。/etc を sys_ls で列挙し、
 * 大文字小文字を無視して一致する**実在名**を全部 stat する。
 * コールバックの中では FS に触らない (private バッファに写すだけ、§4-26)。 */
#define HS_MAX_PROT 16
static u32 g_prot_dev[HS_MAX_PROT];
static u32 g_prot_ino[HS_MAX_PROT];
static int g_prot_count;
static char g_prot_name[HS_MAX_PROT][64];
static int g_prot_name_count;
/* 判定できない stat 失敗を踏んだ。同期を中止する印 */
static int g_abort;

static void prot_scan_cb(const DirEntry_Ext *entry, void *ctx)
{
    int i;

    (void)ctx;
    if (g_prot_name_count >= HS_MAX_PROT) return;
    if (!hsp_is_protected_basename(entry->name)) return;
    i = 0;
    while (entry->name[i] && i < 63) {
        g_prot_name[g_prot_name_count][i] = entry->name[i];
        i++;
    }
    g_prot_name[g_prot_name_count][i] = '\0';
    g_prot_name_count++;
}

/* 0 = ok / -1 = 判定できないので同期を中止 */
static int scan_protected_entities(void)
{
    OS32_Stat st;
    char buf[OS32_MAX_PATH];
    int i;
    int rc;

    g_prot_count = 0;
    g_prot_name_count = 0;

    rc = api->sys_ls("/etc", prot_scan_cb, 0);
    if (rc != 0 && rc != OS32_ERR_NOTFOUND) {
        api->kprintf(ATTR_RED, "Error: /etc を読めない (%d)。中止する\n", rc);
        return -1;
    }

    for (i = 0; i < g_prot_name_count; i++) {
        str_cpy(buf, "/etc/");
        str_cat(buf, g_prot_name[i]);
        rc = api->sys_stat(buf, &st);
        if (rc == 0) {
            g_prot_dev[g_prot_count] = st.st_dev;
            g_prot_ino[g_prot_count] = st.st_ino;
            g_prot_count++;
        } else if (rc != OS32_ERR_NOTFOUND) {
            /* 読めない = 守れない。書いてから気づくより中止する。 */
            api->kprintf(ATTR_RED, "Error: stat %s 失敗 (%d)。中止する\n",
                         buf, rc);
            return -1;
        }
    }
    return 0;
}

/* 実体規則: dst_path が現に /etc/settings.db* のどれかと同じ実体 (NHD 上の
 * hardlink) なら真。名前規則 (hsp_path_protected) をすり抜ける別名を塞ぐ。
 * 宛先の stat が「不存在」以外で失べば g_abort を立てる (往復 1 の B5) — 
 * 読めないまま open すると O_TRUNC で切り詰めてしまう。 */
static int is_same_as_protected(const char *dst_path)
{
    OS32_Stat here;
    int i;
    int rc;

    if (g_prot_count == 0) return 0;                  /* 守る実体が無い */
    rc = api->sys_stat(dst_path, &here);
    if (rc != 0) {
        if (rc != OS32_ERR_NOTFOUND) {
            api->kprintf(ATTR_RED, "Error: stat %s 失敗 (%d)。中止する\n",
                         dst_path, rc);
            g_abort = 1;
        }
        return 0;
    }
    for (i = 0; i < g_prot_count; i++) {
        if (here.st_dev == g_prot_dev[i] && here.st_ino == g_prot_ino[i])
            return 1;
    }
    return 0;
}

/* コピー / mkdir の**直前**に通す 1 か所の判定 */
static int dst_protected(const char *dst_path)
{
    if (hsp_path_protected(dst_path)) return 1;
    return is_same_as_protected(dst_path);
}

/* ======== ディレクトリ再帰同期 ======== */

static void sync_directory(const char *src_dir, const char *dst_dir, int depth)
{
    FileList fl;
    int i;

    if (depth > MAX_DEPTH) return;

    fl.count = 0;
    api->sys_ls(src_dir, ls_cb, &fl);

    for (i = 0; i < fl.count; i++) {
        char src_path[OS32_MAX_PATH];
        char dst_path[OS32_MAX_PATH];

        /* 判定できない stat 失敗を踏んだら、それ以上は書かない */
        if (g_abort) return;

        /* "." と ".." をスキップ */
        if (fl.names[i][0] == '.') {
            if (fl.names[i][1] == '\0') continue;
            if (fl.names[i][1] == '.' && fl.names[i][2] == '\0') continue;
        }

        /* ルート直下の sys は既定で飛ばす (稼働中のシェル・共有ライブラリ)。
         * depth==0 だけで見るので /host/usr/sys のような別階層は対象外。
         * 入れ替えたいときは `hsync sys` と明示する。 */
        if (depth == 0 && !g_want_sys &&
            fl.names[i][0] == 's' && fl.names[i][1] == 'y' &&
            fl.names[i][2] == 's' && fl.names[i][3] == '\0') {
            g_skipped++;
            continue;
        }

        /* パス構築。dst_dir は全体同期のとき "" なので、空文字列で
         * [-1] を読まないように長さを先に見る。 */
        str_cpy(src_path, src_dir);
        if (str_len(src_path) == 0 ||
            src_path[str_len(src_path) - 1] != '/') str_cat(src_path, "/");
        str_cat(src_path, fl.names[i]);

        str_cpy(dst_path, dst_dir);
        if (str_len(dst_path) == 0 ||
            dst_path[str_len(dst_path) - 1] != '/') str_cat(dst_path, "/");
        str_cat(dst_path, fl.names[i]);

        /* /etc/settings.db* は通常配備で作らない・上書きしない (票 S0-D)。
         * ディレクトリ経路も同じ規則で見る (etc/settings.db/ の残骸を作らない)。 */
        if (dst_protected(dst_path)) {
            api->kprintf(ATTR_YELLOW, "  protected: %s (skipped)\n", dst_path);
            g_protected++;
            continue;
        }
        if (g_abort) return;

        if (fl.types[i] == OS32_FILE_TYPE_DIR) {
            /* ディレクトリ: 作成して再帰 */
            api->sys_mkdir(dst_path);
            sync_directory(src_path, dst_path, depth + 1);
        } else {
            /* ファイル: サイズ比較してコピー */
            OS32_Stat dst_stat;
            int need_copy = 1;

            if (!g_force && api->sys_stat(dst_path, &dst_stat) == 0) {
                /* 宛先が存在しサイズが同じならスキップ */
                if (dst_stat.st_size == fl.sizes[i]) {
                    need_copy = 0;
                }
            }

            if (need_copy) {
                int bytes = copy_file(src_path, dst_path);
                if (bytes >= 0) {
                    api->kprintf(ATTR_GREEN, "  %s (%d bytes)\n",
                                 dst_path, bytes);
                    g_copied++;
                } else {
                    api->kprintf(ATTR_RED, "  FAIL: %s (err=%d)\n",
                                 dst_path, bytes);
                    g_errors++;
                }
            } else {
                g_skipped++;
            }
        }
    }
}

/* ======== メイン ======== */

void __cdecl main(int argc, char **argv, KernelAPI *_api)
{
    const char *subdir;
    char src[OS32_MAX_PATH];
    char dst[OS32_MAX_PATH];
    int i;

    api = _api;
    subdir = NULL;
    g_copied = 0;
    g_skipped = 0;
    g_errors = 0;
    g_force = 0;
    g_protected = 0;
    g_abort = 0;

    /* 引数パース */
    for (i = 1; i < argc; i++) {
        if (str_cmp(argv[i], "-f") == 0 ||
            str_cmp(argv[i], "--force") == 0) {
            g_force = 1;
        } else if (str_cmp(argv[i], "-h") == 0 ||
                   str_cmp(argv[i], "--help") == 0) {
            api->kprintf(ATTR_WHITE, "hsync — HostDrv sync\n");
            api->kprintf(ATTR_WHITE, "Usage: hsync [-f] [dir]\n");
            api->kprintf(ATTR_WHITE, "  -f     Force overwrite\n");
            api->kprintf(ATTR_WHITE, "  dir    Sync specific dir only\n");
            api->kprintf(ATTR_WHITE, "  (sys is skipped unless named: running shell/libs)\n");
            return;
        } else {
            subdir = argv[i];
            /* 明示指定なら sys でも同期する (既定の全体同期では外す) */
            if (subdir[0] == 's' && subdir[1] == 'y' &&
                subdir[2] == 's' && subdir[3] == '\0') g_want_sys = 1;
        }
    }

    /* バッファ確保 */
    file_buf = (u8 *)api->mem_alloc(FILE_BUF_SIZE);
    if (!file_buf) {
        api->kprintf(ATTR_RED, "Error: out of memory\n");
        return;
    }

    /* /host がマウントされているか確認 */
    if (!api->sys_is_mounted("/host")) {
        api->kprintf(ATTR_RED, "Error: /host is not mounted\n");
        api->mem_free(file_buf);
        return;
    }

    /* 守るべき実体を 1 度だけ集める (票 S0-D の実体規則)。
     * 集められなければ守れないので同期そのものを行わない。 */
    if (scan_protected_entities() != 0) {
        api->mem_free(file_buf);
        return;
    }

    /* 同期パス構築。`hsync -f etc` のように subdir で保護対象を直接指されても
     * 書かない (連結後の文字列を字句正規化して判定する)。 */
    if (subdir) {
        str_cpy(src, "/host/");
        str_cat(src, subdir);
        str_cpy(dst, "/");
        str_cat(dst, subdir);
        if (dst_protected(dst) || g_abort) {
            if (!g_abort)
                api->kprintf(ATTR_YELLOW, "  protected: %s (skipped)\n", dst);
            api->mem_free(file_buf);
            return;
        }
        api->kprintf(ATTR_CYAN, "hsync: %s -> %s\n", src, dst);
    } else {
        str_cpy(src, "/host");
        str_cpy(dst, "");
        api->kprintf(ATTR_CYAN, "hsync: /host -> /\n");
    }

    if (g_force) {
        api->kprintf(ATTR_YELLOW, "  (force mode)\n");
    }

    /* 同期実行 */
    sync_directory(src, dst, 0);

    if (g_abort) {
        api->kprintf(ATTR_RED,
                     "\nAborted: 保護判定に必要な stat が失敗した\n");
        api->mem_free(file_buf);
        return;
    }

    /* ファイルシステム同期 */
    api->vfs_sync();

    /* 結果表示 */
    api->kprintf(ATTR_WHITE,
                 "\nDone: %d copied, %d skipped, %d errors, %d protected\n",
                 g_copied, g_skipped, g_errors, g_protected);

    api->mem_free(file_buf);
}
