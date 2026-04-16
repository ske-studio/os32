#ifndef EXT2_PRIV_H
#define EXT2_PRIV_H

#include "ext2.h"
#include "ext2_ctx.h"
#include "ide.h"
#include "vfs.h"

/* 1KBブロック時、1グループあたりの最大ブロック数 (ビットマップ1ブロック = 8192ビット) */
#define EXT2_BLOCKS_PER_GROUP_MAX  8192

/* 共有バッファ (シングルタスクのため全インスタンスで共有)
 * 注意: 将来マルチタスク化する場合はインスタンスごとに分離が必要 */
extern u8 ext2_g_blk[EXT2_BLOCK_SIZE];
extern u8 ext2_g_aux[EXT2_BLOCK_SIZE];

/* ======================================================================== */
/* 内部関数プロトタイプ (全関数が Ext2Ctx* を第1引数に取る)                  */
/* ======================================================================== */

/* -- ext2_super.c -- */
int ext2_read_block(Ext2Ctx *ctx, u32 block_num, void *buf);
int ext2_write_block(Ext2Ctx *ctx, u32 block_num, const void *buf);
u32 ext2_current_time(void);
void ext2_mem_copy(void *dst, const void *src, u32 len);
void ext2_mem_zero(void *dst, u32 len);
int ext2_str_len(const char *s);
int ext2_str_ncmp(const char *a, const char *b, int n);
int ext2_write_super_raw(Ext2Ctx *ctx);
int ext2_write_gd_raw(Ext2Ctx *ctx);

/* -- ext2_inode.c -- */
int ext2_read_inode(Ext2Ctx *ctx, u32 ino, Ext2Inode *inode);
int ext2_write_inode(Ext2Ctx *ctx, u32 ino, const Ext2Inode *inode);
int ext2_alloc_block(Ext2Ctx *ctx);
void ext2_free_block(Ext2Ctx *ctx, u32 block_num);
int ext2_alloc_inode(Ext2Ctx *ctx);
void ext2_free_inode(Ext2Ctx *ctx, u32 ino);
u32 ext2_bmap(Ext2Ctx *ctx, const Ext2Inode *inode, u32 file_block);
int ext2_bmap_set(Ext2Ctx *ctx, Ext2Inode *inode, u32 file_block, u32 phys_block);
void ext2_free_all_blocks(Ext2Ctx *ctx, Ext2Inode *inode);

/* -- ext2_dir.c -- */
int ext2_list_dir(Ext2Ctx *ctx, u32 dir_ino, ext2_dir_callback cb, void *user_ctx);
int ext2_find_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 *out_ino, u8 *out_type);
int ext2_add_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name, u32 ino, u8 file_type);
int ext2_delete_entry(Ext2Ctx *ctx, u32 dir_ino, const char *name);
int ext2_lookup(Ext2Ctx *ctx, const char *path, u32 *out_ino);
int ext2_mkdir(Ext2Ctx *ctx, u32 parent_ino, const char *name);
int ext2_rmdir(Ext2Ctx *ctx, u32 parent_ino, const char *name);

/* -- ext2_file.c -- */
int ext2_create(Ext2Ctx *ctx, u32 dir_ino, const char *name, const void *data, u32 size);
int ext2_write(Ext2Ctx *ctx, u32 ino, const void *data, u32 size);
int ext2_write_stream(Ext2Ctx *ctx, u32 ino, const void *buf, u32 size, u32 offset);
int ext2_get_size_ino(Ext2Ctx *ctx, u32 ino, u32 *size);
int ext2_unlink(Ext2Ctx *ctx, u32 dir_ino, const char *name);
int ext2_read_file(Ext2Ctx *ctx, u32 ino, void *buf, u32 max_size);
int ext2_read_stream(Ext2Ctx *ctx, u32 ino, void *buf, u32 size, u32 offset);

#endif
