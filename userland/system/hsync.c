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
/*    hsync              — /host 配下全体を / に同期                        */
/*    hsync bin           — /host/bin/ → /bin/ のみ同期                    */
/*    hsync -f            — サイズ無関係に全上書き                          */
/*    hsync -f sys        — /host/sys/ を強制同期                          */
/* ======================================================================== */

#include "os32api.h"

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

        /* "." と ".." をスキップ */
        if (fl.names[i][0] == '.') {
            if (fl.names[i][1] == '\0') continue;
            if (fl.names[i][1] == '.' && fl.names[i][2] == '\0') continue;
        }

        /* パス構築 */
        str_cpy(src_path, src_dir);
        if (src_path[str_len(src_path) - 1] != '/') str_cat(src_path, "/");
        str_cat(src_path, fl.names[i]);

        str_cpy(dst_path, dst_dir);
        if (dst_path[str_len(dst_path) - 1] != '/') str_cat(dst_path, "/");
        str_cat(dst_path, fl.names[i]);

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
            return;
        } else {
            subdir = argv[i];
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

    /* 同期パス構築 */
    if (subdir) {
        str_cpy(src, "/host/");
        str_cat(src, subdir);
        str_cpy(dst, "/");
        str_cat(dst, subdir);
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

    /* ファイルシステム同期 */
    api->vfs_sync();

    /* 結果表示 */
    api->kprintf(ATTR_WHITE, "\nDone: %d copied, %d skipped, %d errors\n",
                 g_copied, g_skipped, g_errors);

    api->mem_free(file_buf);
}
