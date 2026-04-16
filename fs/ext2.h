/* ======================================================================== */
/*  EXT2.H — ext2ファイルシステムドライバ (読み書き対応)                      */
/*                                                                          */
/*  マルチインスタンス対応: 全関数が Ext2Ctx* を第1引数に取る。              */
/*  1KBブロック, 512Bセクタ対応。複数同時マウント可能。                      */
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

/* 前方宣言: Ext2Ctx (ext2_ctx.h で定義) */
struct Ext2Ctx_tag;
typedef struct Ext2Ctx_tag Ext2Ctx;

/* ---- ディレクトリコールバック ---- */
typedef void (*ext2_dir_callback)(const Ext2DirEntry *entry, void *ctx);

/* ---- マウント/アンマウント ---- */
int ext2_mount(Ext2Ctx *ctx, int ide_drive);
void ext2_unmount(Ext2Ctx *ctx);
int ext2_is_mounted_ctx(Ext2Ctx *ctx);
const Ext2Super *ext2_get_super_ctx(Ext2Ctx *ctx);

/* ---- 同期 ---- */
int ext2_sync(Ext2Ctx *ctx);

/* ---- フォーマット (コンテキスト不要: 一時CTXを内部で使用) ---- */
int ext2_format(int ide_drive, u32 total_sectors);

/* ---- VFS登録 ---- */
void ext2_init(void);

#endif /* EXT2_H */
