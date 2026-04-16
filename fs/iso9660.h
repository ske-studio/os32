/* ======================================================================== */
/*  ISO9660.H — ISO 9660 読み取り専用ファイルシステム                        */
/*                                                                          */
/*  CD-ROM (ISO 9660) のディレクトリ走査・ファイル読み出しをVFS経由で行う。   */
/*  Level 1 (8.3ファイル名) のみ対応。Rock Ridge / Joliet 拡張は非対応。    */
/*  ファイル名の大文字/小文字は区別しない (case-insensitive比較)。           */
/*                                                                          */
/*  セクタサイズ: 2048バイト (ATAPI_SECTOR_SIZE)                            */
/*  Primary Volume Descriptor: LBA 16                                       */
/* ======================================================================== */

#ifndef ISO9660_H
#define ISO9660_H

#include "vfs.h"

/* ISO 9660 定数 */
#define ISO_SECTOR_SIZE       2048
#define ISO_PVD_LBA           16      /* Primary Volume Descriptor の位置 */
#define ISO_PVD_MAGIC         "CD001" /* PVD識別文字列 (5バイト) */
#define ISO_MAX_NAME          128     /* ファイル名最大長 */

/* === Primary Volume Descriptor (LBA 16, 2048バイト) ===
 *
 * オフセットと主要フィールド:
 *   [0]       Type Code: 1 = Primary
 *   [1-5]     "CD001"
 *   [6]       Version: 1
 *   [8-39]    System Identifier
 *   [40-71]   Volume Identifier
 *   [80-87]   Volume Space Size (both-endian u32)
 *   [120-123] Logical Block Size (LE u16 @ 128)
 *   [128-131] Logical Block Size (both-endian u16)
 *   [156-189] Root Directory Record (34バイト)
 */

/* ディレクトリレコード (ISO 9660 §9.1)
 *
 * 可変長 (最小33バイト + ファイル名長):
 *   [0]       Record Length
 *   [1]       Extended Attribute Record Length
 *   [2-9]     Extent Location (both-endian u32)
 *   [10-17]   Data Length (both-endian u32)
 *   [18-24]   Recording Date/Time
 *   [25]      File Flags: bit1=ディレクトリ
 *   [26]      File Unit Size
 *   [27]      Interleave Gap Size
 *   [28-31]   Volume Sequence Number (both-endian u16)
 *   [32]      File Identifier Length
 *   [33+]     File Identifier
 */

/* ファイルフラグ (ディレクトリレコード [25]) */
#define ISO_FLAG_HIDDEN      0x01
#define ISO_FLAG_DIRECTORY   0x02
#define ISO_FLAG_ASSOCIATED  0x04
#define ISO_FLAG_RECORD      0x08
#define ISO_FLAG_PROTECTION  0x10
#define ISO_FLAG_MULTIEXTENT 0x80

/* マウントコンテキスト */
typedef struct {
    int    dev_id;            /* ブロックデバイスインデックス */
    u32    root_lba;          /* ルートディレクトリのLBA */
    u32    root_size;         /* ルートディレクトリのサイズ (バイト) */
    u32    volume_size;       /* ボリューム総セクタ数 */
    u16    block_size;        /* 論理ブロックサイズ (通常2048) */
} Iso9660Ctx;

/* VFS操作テーブル (外部公開) */
extern VfsOps iso9660_ops;

#endif /* ISO9660_H */
