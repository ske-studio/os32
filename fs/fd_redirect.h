/* ======================================================================== */
/*  FD_REDIRECT.H -- FD 0/1/2 リダイレクト管理                               */
/*                                                                          */
/*  標準入出力 (stdin/stdout/stderr) をファイルやメモリバッファに             */
/*  リダイレクトするためのカーネルモジュール。                                */
/*  シェルからの > / >> / < / 2> 構文、およびパイプ (|) を支える基盤。        */
/* ======================================================================== */

#ifndef FD_REDIRECT_H
#define FD_REDIRECT_H

#include "types.h"

/* リダイレクトターゲットの種類 */
#define FD_TARGET_CONSOLE  0   /* デフォルト: コンソール (TTY) */
#define FD_TARGET_FILE     1   /* ファイルに接続 */
#define FD_TARGET_BUFFER   2   /* メモリバッファに接続 (パイプ用) */

/* リダイレクトモード */
#define FD_REDIR_READ      0   /* 読み込み (stdin用) */
#define FD_REDIR_WRITE     1   /* 書き込み・上書き */
#define FD_REDIR_APPEND    2   /* 書き込み・追記 */

/* リダイレクト状態管理構造体 */
typedef struct {
    int target_type;        /* FD_TARGET_* */
    int file_fd;            /* FD_TARGET_FILE の場合、実ファイルのFD番号 */
    u8 *buffer;             /* FD_TARGET_BUFFER の場合のバッファポインタ */
    u32 buf_capacity;       /* バッファ容量 */
    u32 buf_pos;            /* 現在の読み書き位置 */
    u32 buf_len;            /* バッファ内の有効データ長 */
} FdRedirect;

/* ======== API ======== */

/* 初期化 (全FDをコンソールモードにリセット) */
void fd_redirect_init(void);

/* FD 0/1/2 をファイルにリダイレクト
 *   fd:   対象FD (0=stdin, 1=stdout, 2=stderr)
 *   path: リダイレクト先ファイルパス
 *   mode: FD_REDIR_READ / FD_REDIR_WRITE / FD_REDIR_APPEND
 * 戻り値: 0=成功, 負=エラー */
int fd_redirect_to_file(int fd, const char *path, int mode);

/* FD 0/1/2 をメモリバッファにリダイレクト (パイプ用)
 *   fd:   対象FD
 *   buf:  バッファポインタ
 *   size: バッファ容量
 *   len:  初期データ長 (読み込み用: バッファに既にあるデータ長)
 * 戻り値: 0=成功, 負=エラー */
int fd_redirect_to_buffer(int fd, u8 *buf, u32 size, u32 len);

/* FD 0/1/2 のリダイレクトを解除 (コンソールモードに戻す)
 * ファイルリダイレクト中の場合、ファイルを自動でクローズする */
void fd_redirect_reset(int fd);

/* リダイレクト状態の問い合わせ
 * 戻り値: 1=リダイレクト中, 0=コンソールモード */
int fd_is_redirected(int fd);

/* リダイレクト先への読み書き (vfs_fd.c から呼び出される)
 * 戻り値: 読み書きバイト数, 負=エラー */
int fd_redirect_read(int fd, void *buf, u32 size);
int fd_redirect_write(int fd, const void *buf, u32 size);

/* パイプバッファの書き込み済みデータ長を取得 */
u32 fd_redirect_get_buf_len(int fd);

#endif /* FD_REDIRECT_H */
