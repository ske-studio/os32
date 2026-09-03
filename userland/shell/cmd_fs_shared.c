/* ======================================================================== */
/*  CMD_FS_SHARED.C — ファイル/ディレクトリ操作コマンド 共通ユーティリティ    */
/*                                                                          */
/*  cmd_dir.c / cmd_file.c で共有するヘルパー関数群。                        */
/*  以前は cmd_fs_shared.h 内に static 実装として置かれていたが、             */
/*  バイナリ重複を避けるため .c ファイルに移動した。                          */
/* ======================================================================== */
#include "shell.h"
#include "cmd_fs_shared.h"

/* ======================================================================== */
/*  skip_space — 先頭の空白文字をスキップしてポインタを返す                   */
/* ======================================================================== */
const char *fs_skip_space(const char *s)
{
    while (*s == ' ') s++;
    return s;
}

/* ======================================================================== */
/*  dummy_ls_cb — is_dir() 判定用のダミーコールバック                        */
/* ======================================================================== */
void fs_dummy_ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    (void)entry;
    *(int *)ctx = 1;
}

/* ======================================================================== */
/*  fs_is_dir — 指定パスがディレクトリかどうかを判定                         */
/*                                                                          */
/*  sys_ls が成功 (0) を返せばディレクトリとみなす。                          */
/* ======================================================================== */
int fs_is_dir(const char *path)
{
    int found = 0;
    int rc = g_api->sys_ls(path, (void *)fs_dummy_ls_cb, &found);
    return (rc == 0);
}

/* ======================================================================== */
/*  fs_path_kind — stat でパス種別を判定 (stat 非対応 FS は sys_ls で代替)    */
/* ======================================================================== */
int fs_path_kind(const char *path)
{
    OS32_Stat st;
    int rc = g_api->sys_stat(path, &st);
    if (rc == 0) {
        return ((st.st_mode & OS_S_IFMT) == OS_S_IFDIR) ? FS_KIND_DIR : FS_KIND_FILE;
    }
    if (rc == OS32_ERR_NOTFOUND) return rc;
    if (fs_is_dir(path)) return FS_KIND_DIR;
    return rc;
}

/* ======================================================================== */
/*  fs_same_file — cp/mv の自己上書き防止                                     */
/*                                                                          */
/*  `cp a a` は src を開いた後に dst を O_TRUNC で開くので a が 0 バイトに、  */
/*  `mv a a` はコピー後に unlink するので a が消えていた。                    */
/* ======================================================================== */
int fs_same_file(const char *a, const char *b)
{
    OS32_Stat sa, sb;

    if (strcmp(a, b) == 0) return 1;
    if (g_api->sys_stat(a, &sa) != 0) return 0;
    if (g_api->sys_stat(b, &sb) != 0) return 0;
    /* hostdrv 等 inode を返さない FS (st_ino=0) は比較できないので文字列一致のみ */
    if (sa.st_ino == 0 || sb.st_ino == 0) return 0;
    return (sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);
}

/* ======================================================================== */
/*  fs_strerror — OS32_ERR_* → メッセージ                                    */
/* ======================================================================== */
const char *fs_strerror(int rc)
{
    switch (rc) {
    case 0:                 return "Success";
    case OS32_ERR_IO:       return "I/O error";
    case OS32_ERR_NOTFOUND: return "No such file or directory";
    case OS32_ERR_NOMOUNT:  return "Not mounted";
    case OS32_ERR_NOSPC:    return "No space left on device";
    case OS32_ERR_EXIST:    return "File exists";
    case OS32_ERR_NOTDIR:   return "Not a directory";
    case OS32_ERR_NOTEMPTY: return "Directory not empty";
    case OS32_ERR_ISDIR:    return "Is a directory";
    case OS32_ERR_INVAL:    return "Invalid argument";
    default:                return "Unknown error";
    }
}

/* ======================================================================== */
/*  get_basename — パス文字列から末尾のファイル名部分を取得                   */
/* ======================================================================== */
const char *get_basename(const char *path)
{
    const char *p = path;
    const char *base = path;
    while (*p) {
        if (*p == '/' || *p == '\\') base = p + 1;
        p++;
    }
    return base;
}

/* ======================================================================== */
/*  fs_append_basename — dst_path の末尾に src_path のベース名を追加          */
/* ======================================================================== */
void fs_append_basename(char *dst_path, const char *src_path)
{
    int dlen;
    const char *base;

    base = get_basename(src_path);
    dlen = strlen(dst_path);
    if (dlen > 0 && dst_path[dlen - 1] != '/' && dlen < PATH_MAX_LEN - 1) {
        strncat(dst_path, "/", PATH_MAX_LEN - dlen - 1);
    }
    strncat(dst_path, base, PATH_MAX_LEN - strlen(dst_path) - 1);
}

/* ======================================================================== */
/*  fs_join_path — dir_path と name を結合して dst_path に格納                */
/* ======================================================================== */
void fs_join_path(char *dst_path, const char *dir_path, const char *name)
{
    int dlen;

    strncpy(dst_path, dir_path, PATH_MAX_LEN - 1);
    dst_path[PATH_MAX_LEN - 1] = '\0';
    dlen = strlen(dst_path);
    if (dlen > 0 && dst_path[dlen - 1] != '/' && dlen < PATH_MAX_LEN - 1) {
        dst_path[dlen] = '/';
        dst_path[dlen + 1] = '\0';
    }
    strncat(dst_path, name, PATH_MAX_LEN - strlen(dst_path) - 1);
}

/* ======================================================================== */
/*  fs_parse_two_args — コマンド文字列から2つの引数を取り出す                 */
/*                                                                          */
/*  cmd + skip 位置から空白区切りで arg1, arg2 を切り出す。                   */
/*  両方取れたら 1、片方でも空なら 0 を返す。                                */
/* ======================================================================== */
int fs_parse_two_args(const char *cmd, int skip, char *arg1, char *arg2)
{
    const char *p = fs_skip_space(cmd + skip);
    int i = 0;
    while (*p && *p != ' ' && i < 255) arg1[i++] = *p++;
    arg1[i] = '\0';
    p = fs_skip_space(p);
    i = 0;
    while (*p && *p != ' ' && i < 255) arg2[i++] = *p++;
    arg2[i] = '\0';
    return (arg1[0] && arg2[0]);
}

/* ======================================================================== */
/*  format_size — 数値をサイズ文字列に変換 (右寄せなし、素の数字)             */
/* ======================================================================== */
void format_size(u32 size, char *buf, int max_len)
{
    int pos = 0, i;
    char temp[16];
    u32 s = size;
    (void)max_len;
    if (s == 0) temp[pos++] = '0';
    while (s > 0) { temp[pos++] = '0' + (s % 10); s /= 10; }
    for (i = 0; i < pos / 2; i++) {
        char t = temp[i];
        temp[i] = temp[pos - 1 - i];
        temp[pos - 1 - i] = t;
    }
    for (i = 0; i < pos; i++) buf[i] = temp[i];
    buf[pos] = '\0';
}
