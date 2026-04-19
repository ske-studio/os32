/* ======================================================================== */
/*  CMD_FS_SHARED.C — ファイル/ディレクトリ操作コマンド 共通ユーティリティ    */
/*                                                                          */
/*  cmd_dir.c / cmd_file.c で共有するヘルパー関数群。                        */
/*  以前は cmd_fs_shared.h 内に static 実装として置かれていたが、             */
/*  バイナリ重複を避けるため .c ファイルに移動した。                          */
/* ======================================================================== */
#include "shell.h"

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
    int rc = g_api->sys_ls(path, (void *)fs_dummy_ls_cb, 0);
    return (rc == 0);
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
    char temp[256];
    int i = 0, j = 0;
    const char *base = get_basename(src_path);

    while (dst_path[i] && i < 254) { temp[i] = dst_path[i]; i++; }
    if (i > 0 && temp[i-1] != '/' && i < 254) temp[i++] = '/';
    while (base[j] && i < 255) temp[i++] = base[j++];
    temp[i] = '\0';

    for (i = 0; temp[i]; i++) dst_path[i] = temp[i];
    dst_path[i] = '\0';
}

/* ======================================================================== */
/*  fs_join_path — dir_path と name を結合して dst_path に格納                */
/* ======================================================================== */
void fs_join_path(char *dst_path, const char *dir_path, const char *name)
{
    int i = 0, j = 0;
    while (dir_path[i] && i < 254) { dst_path[i] = dir_path[i]; i++; }
    if (i > 0 && dst_path[i-1] != '/' && i < 254) dst_path[i++] = '/';
    while (name[j] && i < 255) dst_path[i++] = name[j++];
    dst_path[i] = '\0';
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
