/* ======================================================================== */
/*  EXT2_CTX.H — ext2ファイルシステム インスタンスコンテキスト               */
/*                                                                          */
/*  マルチインスタンス対応: 従来のグローバル変数を1構造体に集約。            */
/*  各マウントポイントごとに Ext2Ctx を kmalloc で確保する。                 */
/* ======================================================================== */

#ifndef EXT2_CTX_H
#define EXT2_CTX_H

#include "ext2.h"

/* 前方宣言 (dev.h の循環 include 回避) */
struct _Device;

/* マルチグループ対応: 200MBディスクで最大25グループ */
#define EXT2_MAX_GROUPS  32

/* ext2インスタンスコンテキスト
 * 1インスタンスあたり約541+4バイト。kmalloc で動的確保する。 */
typedef struct Ext2Ctx_tag {
    int mounted;
    int drive_num;           /* 後方互換 (format等で使用) */
    u32 base_lba;
    u32 num_groups;
    struct _Device *dev;     /* Device API ポインタ */
    Ext2Super sb_info;
    Ext2GroupDesc gd_table[EXT2_MAX_GROUPS];
} Ext2Ctx;

#endif /* EXT2_CTX_H */

