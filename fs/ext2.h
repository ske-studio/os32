/* ======================================================================== */
/*  EXT2.H — ext2ファイルシステムドライバ (読み書き対応)                      */
/*                                                                          */
/*  Linux kernel 2.4 (Plamo Linux) ext2実装を参考にしたフルスペック版。      */
/*  1KBブロック, 512Bセクタ, 単一ブロックグループ前提。                      */
/* ======================================================================== */

#ifndef EXT2_H
#define EXT2_H

#include "types.h"

/* ext2定数 */
#define EXT2_SUPER_MAGIC  0xEF53
#define EXT2_ROOT_INO     2
#define EXT2_BLOCK_SIZE   1024
#define EXT2_NAME_LEN     255
#define EXT2_NDIR_BLOCKS  12     /* 直接ブロック数 */
#define EXT2_IND_BLOCK    12     /* 単一間接ブロック */
#define EXT2_DIND_BLOCK   13     /* 二重間接ブロック */
#define EXT2_TIND_BLOCK   14     /* 三重間接ブロック */
#define EXT2_N_BLOCKS     15     /* ブロックポインタ総数 */

/* 1KBブロックの場合、間接ブロック内のポインタ数 */
#define EXT2_ADDR_PER_BLOCK  (EXT2_BLOCK_SIZE / 4)  /* 256 */

/* ファイルタイプ (dirent) */
#define EXT2_FT_UNKNOWN   0
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_FIFO      5
#define EXT2_FT_SOCK      6
#define EXT2_FT_SYMLINK   7

/* iノードモード (POSIX) */
#define EXT2_S_IFMT     0170000
#define EXT2_S_IFSOCK   0140000
#define EXT2_S_IFLNK    0120000
#define EXT2_S_IFREG    0100000
#define EXT2_S_IFBLK    0060000
#define EXT2_S_IFDIR    0040000
#define EXT2_S_IFCHR    0020000
#define EXT2_S_IFIFO    0010000

/* パーミッション */
#define EXT2_S_IRWXU    0000700
#define EXT2_S_IRUSR    0000400
#define EXT2_S_IWUSR    0000200
#define EXT2_S_IXUSR    0000100
#define EXT2_S_IRWXG    0000070
#define EXT2_S_IRGRP    0000040
#define EXT2_S_IWGRP    0000020
#define EXT2_S_IXGRP    0000010
#define EXT2_S_IRWXO    0000007
#define EXT2_S_IROTH    0000004
#define EXT2_S_IWOTH    0000002
#define EXT2_S_IXOTH    0000001

/* スーパーブロック (メモリ上の要約) */
typedef struct {
    u32 total_inodes;
    u32 total_blocks;
    u32 block_size;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 first_data_block;
    u16 inode_size;
    u16 magic;
    u32 first_ino;
    u32 free_blocks_count;
    u32 free_inodes_count;
    char volume_name[17];
} Ext2Super;

/* グループディスクリプタ */
typedef struct {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u16 free_blocks;
    u16 free_inodes;
    u16 used_dirs;
} Ext2GroupDesc;

/* iノード (ディスク上) */
typedef struct {
    u16 mode;
    u16 uid;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks;     /* 512B単位 */
    u32 flags;
    u32 osd1;
    u32 block[EXT2_N_BLOCKS];
    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8  osd2[12];
} Ext2Inode;

/* ディレクトリエントリ */
typedef struct {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[EXT2_NAME_LEN + 1];
} Ext2DirEntry;

/* エラーコード */
#define EXT2_OK           0
#define EXT2_ERR_IO      -1
#define EXT2_ERR_MAGIC   -2
#define EXT2_ERR_NOTFOUND -3
#define EXT2_ERR_NOMOUNT -4
#define EXT2_ERR_NOSPC   -5
#define EXT2_ERR_EXIST   -6
#define EXT2_ERR_NOTDIR  -7
#define EXT2_ERR_NOTEMPTY -8
#define EXT2_ERR_ISDIR   -9

/* ---- マウント/アンマウント ---- */
int ext2_mount(int ide_drive);
void ext2_unmount(void);
int ext2_is_mounted(void);
const Ext2Super *ext2_get_super(void);

/* ---- iノード ---- */
int ext2_read_inode(u32 ino, Ext2Inode *inode);
int ext2_write_inode(u32 ino, const Ext2Inode *inode);

/* ---- ディレクトリ ---- */
typedef void (*ext2_dir_callback)(const Ext2DirEntry *entry, void *ctx);
int ext2_list_dir(u32 dir_ino, ext2_dir_callback cb, void *ctx);
int ext2_find_entry(u32 dir_ino, const char *name, u32 *out_ino, u8 *out_type);
int ext2_add_entry(u32 dir_ino, const char *name, u32 ino, u8 file_type);
int ext2_delete_entry(u32 dir_ino, const char *name);

/* ---- ファイル操作 ---- */
int ext2_read_file(u32 ino, void *buf, u32 max_size);
int ext2_create(u32 dir_ino, const char *name, const void *data, u32 size);
int ext2_write(u32 ino, const void *data, u32 size);
int ext2_unlink(u32 dir_ino, const char *name);

/* ---- ディレクトリ操作 ---- */
int ext2_mkdir(u32 parent_ino, const char *name);
int ext2_rmdir(u32 parent_ino, const char *name);

/* ---- パス検索 ---- */
int ext2_lookup(const char *path, u32 *out_ino);

/* ---- ブロック管理 ---- */
int ext2_alloc_block(void);
void ext2_free_block(u32 block_num);
int ext2_alloc_inode(void);
void ext2_free_inode(u32 ino);

/* ---- フォーマット ---- */
int ext2_format(int ide_drive, u32 total_sectors);

/* ---- 同期 ---- */
int ext2_sync(void);

/* ---- VFS登録 ---- */
void ext2_init(void);

#endif /* EXT2_H */
