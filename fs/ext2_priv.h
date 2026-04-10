#ifndef EXT2_PRIV_H
#define EXT2_PRIV_H

#include "ext2.h"
#include "ide.h"
#include "vfs.h"

/* マルチグループ対応: 200MBディスクで最大25グループ */
#define EXT2_MAX_GROUPS  32

/* 1KBブロック時、1グループあたりの最大ブロック数 (ビットマップ1ブロック = 8192ビット) */
#define EXT2_BLOCKS_PER_GROUP_MAX  8192

/* マウント状態とグローバル情報 */
extern int ext2_mounted;
extern int ext2_drive_num;
extern Ext2Super ext2_sb_info;
extern Ext2GroupDesc ext2_gd_table[EXT2_MAX_GROUPS];
extern u32 ext2_num_groups;
extern u32 ext2_base_lba;

/* 共有バッファ */
extern u8 ext2_g_blk[EXT2_BLOCK_SIZE];
extern u8 ext2_g_aux[EXT2_BLOCK_SIZE];

/* ======================================================================== */
/* 内部関数プロトタイプ                                                      */
/* ======================================================================== */

/* -- ext2_super.c -- */
int ext2_read_block(u32 block_num, void *buf);
int ext2_write_block(u32 block_num, const void *buf);
u32 ext2_current_time(void);
void ext2_mem_copy(void *dst, const void *src, u32 len);
void ext2_mem_zero(void *dst, u32 len);
int ext2_str_len(const char *s);
int ext2_str_ncmp(const char *a, const char *b, int n);
int ext2_write_super_raw(void);
int ext2_write_gd_raw(void);

/* -- ext2_inode.c -- */
int ext2_read_inode(u32 ino, Ext2Inode *inode);
int ext2_write_inode(u32 ino, const Ext2Inode *inode);
int ext2_alloc_block(void);
void ext2_free_block(u32 block_num);
int ext2_alloc_inode(void);
void ext2_free_inode(u32 ino);
u32 ext2_bmap(const Ext2Inode *inode, u32 file_block);
int ext2_bmap_set(Ext2Inode *inode, u32 file_block, u32 phys_block);
void ext2_free_all_blocks(Ext2Inode *inode);

/* -- ext2_dir.c -- */
int ext2_list_dir(u32 dir_ino, ext2_dir_callback cb, void *ctx);
int ext2_find_entry(u32 dir_ino, const char *name, u32 *out_ino, u8 *out_type);
int ext2_add_entry(u32 dir_ino, const char *name, u32 ino, u8 file_type);
int ext2_delete_entry(u32 dir_ino, const char *name);
int ext2_lookup(const char *path, u32 *out_ino);
int ext2_mkdir(u32 parent_ino, const char *name);
int ext2_rmdir(u32 parent_ino, const char *name);

/* -- ext2_file.c -- */
int ext2_create(u32 dir_ino, const char *name, const void *data, u32 size);
int ext2_write(u32 ino, const void *data, u32 size);
int ext2_write_stream(u32 ino, const void *buf, u32 size, u32 offset);
int ext2_get_size_ino(u32 ino, u32 *size);
int ext2_unlink(u32 dir_ino, const char *name);
int ext2_read_file(u32 ino, void *buf, u32 max_size);
int ext2_read_stream(u32 ino, void *buf, u32 size, u32 offset);

#endif
