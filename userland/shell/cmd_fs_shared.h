/* ======================================================================== */
/*  CMD_FS_SHARED.H — ファイル/ディレクトリ操作コマンド 共通ヘッダ            */
/*                                                                          */
/*  cmd_dir.c / cmd_file.c が共有するユーティリティ関数のプロトタイプ宣言。   */
/*  実体は cmd_fs_shared.c に定義。                                          */
/* ======================================================================== */
#ifndef CMD_FS_SHARED_H
#define CMD_FS_SHARED_H

#include "shell.h"

/* 空白スキップ */
const char *fs_skip_space(const char *s);

/* ディレクトリ判定 (sys_ls が成功すればディレクトリ) */
int fs_is_dir(const char *path);

/* パス種別: FS_KIND_DIR / FS_KIND_FILE、負値は OS32_ERR_* (存在しない等) */
#define FS_KIND_FILE 0
#define FS_KIND_DIR  1
int fs_path_kind(const char *path);

/* 2 つのパスが同じファイルを指すか (文字列一致 or stat の dev/ino 一致) */
int fs_same_file(const char *a, const char *b);

/* OS32_ERR_* → 人間向けメッセージ */
const char *fs_strerror(int rc);

/* sys_ls 用ダミーコールバック (is_dir 内部で使用) */
void fs_dummy_ls_cb(const DirEntry_Ext *entry, void *ctx);

/* パスのベース名 (ファイル名部分) を返す */
const char *get_basename(const char *path);

/* dst_path の末尾に src_path のベース名を追加 */
void fs_append_basename(char *dst_path, const char *src_path);

/* dir_path と name を結合 */
void fs_join_path(char *dst_path, const char *dir_path, const char *name);

/* コマンド文字列から2引数を取り出す */
int fs_parse_two_args(const char *cmd, int skip, char *arg1, char *arg2);

/* 数値をサイズ文字列に変換 */
void format_size(u32 size, char *buf, int max_len);

#endif /* CMD_FS_SHARED_H */
