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

/* 連結・コピーは**容量付きのものだけ**を置く。無検査の str_cpy / str_cat は
 * 引数から組み立てた長いパスでバッファを越えるので撤去した (往復 2 の 5)。
 *
 * 容量付き連結。cap は NUL 込みのバッファ長。
 * 戻り値 1 = 入った / 0 = 溢れた (dst は変えない)。
 * `hsync ./././...etc` のように引数から組み立てた文字列が OS32_MAX_PATH を
 * 越えると、正規化や保護判定にたどり着く前にスタックを壊していた
 * (往復 2 の 5)。連結は必ずこちらを通す。 */
static int str_ncat(char *dst, const char *src, int cap)
{
    int len = str_len(dst);
    int add = str_len(src);
    int i;

    if (len + add + 1 > cap) return 0;
    for (i = 0; i < add; i++) dst[len + i] = src[i];
    dst[len + add] = '\0';
    return 1;
}

/* 容量付きコピー。戻り値 1 = 入った / 0 = 溢れた */
static int str_ncpy(char *dst, const char *src, int cap)
{
    dst[0] = '\0';
    return str_ncat(dst, src, cap);
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ======== ファイルリスト ======== */

/* 名前の保持幅。FileList はスタックに載る (MAX_FILES x NAME_CAP x 深さ) ので
 * 無闇に広げられない。収まらない名前は**切り詰めずにエラー**にする (下記)。 */
#define NAME_CAP 64

typedef struct {
    char names[MAX_FILES][NAME_CAP];
    u8   types[MAX_FILES];
    u32  sizes[MAX_FILES];
    int  count;
    int  dropped;      /* MAX_FILES を越えて捨てたエントリ数 */
    int  truncated;    /* NAME_CAP に収まらず取り込めなかったエントリ数 */
} FileList;

static void ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    FileList *fl = (FileList *)ctx;
    int i;
    if (fl->count >= MAX_FILES) { fl->dropped++; return; }

    if (!hsp_name_fits(entry->name, NAME_CAP)) {
        /* 切り詰めた名前でコピーすると**別のファイル**を作って成功と出る
         * (往復 3 の D6)。取り込まずに数えて、呼び手がエラーにする。 */
        fl->truncated++;
        return;
    }
    i = 0;
    while (entry->name[i]) {
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
        if (rd == 0) break;                  /* EOF */
        if (rd < 0) {                        /* I/O エラー: 部分コピーを成功にしない */
            api->sys_close(fd_src);
            api->sys_close(fd_dst);
            return -4;
        }
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
/* 一覧が HS_MAX_PROT を越えた。守れないので同期を拒否する (往復 2 の 3) */
static int g_prot_overflow;
/* 判定できない stat 失敗を踏んだ。同期を中止する印 */
static int g_abort;

static void prot_scan_cb(const DirEntry_Ext *entry, void *ctx)
{
    int i;

    (void)ctx;
    if (!hsp_is_protected_basename(entry->name)) return;
    if (g_prot_name_count >= HS_MAX_PROT) {
        /* 黙って捨てると 17 件目の実体を hardlink 経由で上書きできてしまう */
        g_prot_overflow++;
        return;
    }
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
    g_prot_overflow = 0;

    rc = api->sys_ls("/etc", prot_scan_cb, 0);
    if (rc != 0 && rc != OS32_ERR_NOTFOUND) {
        api->kprintf(ATTR_RED, "Error: /etc を読めない (%d)。中止する\n", rc);
        return -1;
    }
    if (g_prot_overflow) {
        api->kprintf(ATTR_RED,
                     "Error: /etc の保護対象が %d 件を越えた (+%d)。中止する\n",
                     HS_MAX_PROT, g_prot_overflow);
        return -1;
    }

    for (i = 0; i < g_prot_name_count; i++) {
        if (!str_ncpy(buf, "/etc/", (int)sizeof(buf)) ||
            !str_ncat(buf, g_prot_name[i], (int)sizeof(buf))) {
            api->kprintf(ATTR_RED, "Error: path too long (/etc/%s)。中止する\n",
                         g_prot_name[i]);
            return -1;
        }
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
    int rc;

    if (depth > MAX_DEPTH) {
        /* 打ち切りを黙って成功にしない (未同期のまま「完了」と出ていた) */
        api->kprintf(ATTR_RED, "  FAIL: depth > %d: %s\n", MAX_DEPTH, src_dir);
        g_errors++;
        return;
    }

    fl.count = 0;
    fl.dropped = 0;
    fl.truncated = 0;
    rc = api->sys_ls(src_dir, ls_cb, &fl);
    if (rc != 0) {
        api->kprintf(ATTR_RED, "  FAIL: ls %s (err=%d)\n", src_dir, rc);
        g_errors++;
        return;
    }
    if (fl.dropped) {
        api->kprintf(ATTR_RED, "  FAIL: %s のエントリが %d 件を越えた (+%d)\n",
                     src_dir, MAX_FILES, fl.dropped);
        g_errors++;
    }
    if (fl.truncated) {
        api->kprintf(ATTR_RED,
                     "  FAIL: %s に %d 文字を越える名前が %d 件 (コピーしない)\n",
                     src_dir, NAME_CAP - 1, fl.truncated);
        g_errors++;
    }

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
         * [-1] を読まないように長さを先に見る。連結は必ず容量付きで行い、
         * 溢れたら**判定より前に**エラーにする (往復 2 の 5)。 */
        if (!str_ncpy(src_path, src_dir, (int)sizeof(src_path)) ||
            ((str_len(src_path) == 0 ||
              src_path[str_len(src_path) - 1] != '/') &&
             !str_ncat(src_path, "/", (int)sizeof(src_path))) ||
            !str_ncat(src_path, fl.names[i], (int)sizeof(src_path))) {
            api->kprintf(ATTR_RED, "  FAIL: path too long: %s/%s\n",
                         src_dir, fl.names[i]);
            g_errors++;
            continue;
        }

        if (!str_ncpy(dst_path, dst_dir, (int)sizeof(dst_path)) ||
            ((str_len(dst_path) == 0 ||
              dst_path[str_len(dst_path) - 1] != '/') &&
             !str_ncat(dst_path, "/", (int)sizeof(dst_path))) ||
            !str_ncat(dst_path, fl.names[i], (int)sizeof(dst_path))) {
            api->kprintf(ATTR_RED, "  FAIL: path too long: %s/%s\n",
                         dst_dir, fl.names[i]);
            g_errors++;
            continue;
        }

        /* /etc/settings.db* は通常配備で作らない・上書きしない (票 S0-D)。
         * ディレクトリ経路も同じ規則で見る (etc/settings.db/ の残骸を作らない)。 */
        if (dst_protected(dst_path)) {
            api->kprintf(ATTR_YELLOW, "  protected: %s (skipped)\n", dst_path);
            g_protected++;
            continue;
        }
        if (g_abort) return;

        if (fl.types[i] == OS32_FILE_TYPE_DIR) {
            /* ディレクトリ: 作成して再帰。mkdir の失敗を無視すると
             * 中身のコピーが全部落ちて「完了」と出る (往復 2 の 4)。 */
            int mrc = api->sys_mkdir(dst_path);
            if (mrc != 0 && mrc != OS32_ERR_EXIST) {
                api->kprintf(ATTR_RED, "  FAIL: mkdir %s (err=%d)\n",
                             dst_path, mrc);
                g_errors++;
                continue;
            }
            sync_directory(src_path, dst_path, depth + 1);
        } else {
            /* ファイル: サイズ比較してコピー */
            OS32_Stat dst_stat;
            int need_copy = 1;
            int src = 0;

            src = api->sys_stat(dst_path, &dst_stat);
            if (!g_force && src == 0) {
                /* 宛先が存在しサイズが同じならスキップ */
                if (dst_stat.st_size == fl.sizes[i]) {
                    need_copy = 0;
                }
            } else if (src != 0 && src != OS32_ERR_NOTFOUND) {
                api->kprintf(ATTR_RED, "  FAIL: stat %s (err=%d)\n",
                             dst_path, src);
                g_errors++;
                continue;
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

int __cdecl main(int argc, char **argv, KernelAPI *_api)
{
    const char *subdir;
    char src[OS32_MAX_PATH];
    char dst[OS32_MAX_PATH];
    int i;
    int rc;

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
            return 0;
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
        return 1;
    }

    /* /host がマウントされているか確認 */
    if (!api->sys_is_mounted("/host")) {
        api->kprintf(ATTR_RED, "Error: /host is not mounted\n");
        api->mem_free(file_buf);
        return 1;
    }

    /* 守るべき実体を 1 度だけ集める (票 S0-D の実体規則)。
     * 集められなければ守れないので同期そのものを行わない。 */
    if (scan_protected_entities() != 0) {
        api->mem_free(file_buf);
        return 1;
    }

    /* 同期パス構築。`hsync -f etc` のように subdir で保護対象 (やその子) を
     * 直接指されても書かない。連結は容量付きで行い、溢れたら判定より前に
     * エラーにする (往復 2 の 2 / 5)。 */
    if (subdir) {
        if (!str_ncpy(src, "/host/", (int)sizeof(src)) ||
            !str_ncat(src, subdir, (int)sizeof(src)) ||
            !str_ncpy(dst, "/", (int)sizeof(dst)) ||
            !str_ncat(dst, subdir, (int)sizeof(dst))) {
            api->kprintf(ATTR_RED, "Error: path too long: %s\n", subdir);
            api->mem_free(file_buf);
            return 1;
        }
        if (dst_protected(dst) || g_abort) {
            if (g_abort) {
                api->mem_free(file_buf);
                return 1;
            }
            api->kprintf(ATTR_YELLOW, "  protected: %s (skipped)\n", dst);
            api->mem_free(file_buf);
            return 0;                 /* 除外は失敗ではない */
        }
        api->kprintf(ATTR_CYAN, "hsync: %s -> %s\n", src, dst);
    } else {
        if (!str_ncpy(src, "/host", (int)sizeof(src)) ||
            !str_ncpy(dst, "", (int)sizeof(dst))) {
            api->kprintf(ATTR_RED, "Error: path too long\n");
            api->mem_free(file_buf);
            return 1;
        }
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
        return 1;
    }

    /* ファイルシステム同期。落ちたら書いたものが届いていない */
    rc = api->vfs_sync();
    if (rc != 0) {
        api->kprintf(ATTR_RED, "  FAIL: vfs_sync (err=%d)\n", rc);
        g_errors++;
    }

    /* 結果表示 */
    api->kprintf(ATTR_WHITE,
                 "\nDone: %d copied, %d skipped, %d errors, %d protected\n",
                 g_copied, g_skipped, g_errors, g_protected);

    api->mem_free(file_buf);
    /* 失敗は終了コードに載せる (crt0_c が main の戻り値を sys_exit へ渡す) */
    return g_errors ? 1 : 0;
}
