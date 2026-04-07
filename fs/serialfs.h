/* ======================================================================== */
/*  SERIALFS.H — シリアル・リモートファイルシステム定義                     */
/*                                                                          */
/*  VFSプラグイン: COM1ポート経由でホスト内のファイルを操作                 */
/* ======================================================================== */

#ifndef SERIALFS_H
#define SERIALFS_H

#include "vfs.h"

/* ---- SF RPC コマンド ---- */
#define SF_CMD_NONE   0x00
#define SF_CMD_OPEN   0x01
#define SF_CMD_READ   0x02
#define SF_CMD_WRITE  0x03
#define SF_CMD_CLOSE  0x04
#define SF_CMD_LS     0x05
#define SF_CMD_MKDIR  0x06
#define SF_CMD_RMDIR  0x07
#define SF_CMD_UNLINK 0x08
#define SF_CMD_RENAME 0x09
#define SF_CMD_GETSIZE 0x0A
#define SF_CMD_READ_STREAM 0x0B
#define SF_CMD_WRITE_STREAM 0x0C

/* ---- SF RPC エラー ---- */
#define SF_ERR_OK     0x00
#define SF_ERR_NOF    0x01
#define SF_ERR_IO     0x02
#define SF_ERR_ACC    0x03
#define SF_ERR_EXT    0x04

void serialfs_init(void);

#endif /* SERIALFS_H */
