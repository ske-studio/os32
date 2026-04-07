/* ======================================================================== */
/*  FAT12.H — FAT12 ファイルシステムドライバ                                */
/*                                                                          */
/*  PC-98 2HD フロッピー (1024B/sector) のFAT12を読み書きする               */
/* ======================================================================== */

#ifndef FAT12_H
#define FAT12_H

#include "types.h"

/* パッキング (GCC) */

/* ======== BPB (BIOS Parameter Block) ======== */
/* オフセット0からのブートセクタ構造 */
typedef struct {
    u8  jmp[3];             /* 0x00: ジャンプ命令 */
    u8  oem[8];             /* 0x03: OEM名 */
    u16 bytes_per_sector;   /* 0x0B: セクタサイズ (1024 for PC-98 2HD) */
    u8  sectors_per_cluster;/* 0x0D: クラスタあたりセクタ数 */
    u16 reserved_sectors;   /* 0x0E: 予約セクタ数 (ブートセクタ含む) */
    u8  num_fats;           /* 0x10: FAT数 (通常2) */
    u16 root_entry_count;   /* 0x11: ルートディレクトリエントリ数 */
    u16 total_sectors_16;   /* 0x13: 総セクタ数 (16bit) */
    u8  media_type;         /* 0x15: メディアタイプ */
    u16 fat_size_16;        /* 0x16: FATあたりセクタ数 */
    u16 sectors_per_track;  /* 0x18: トラックあたりセクタ数 */
    u16 num_heads;          /* 0x1A: ヘッド数 */
    u32 hidden_sectors;     /* 0x1C: 隠しセクタ数 */
    u32 total_sectors_32;   /* 0x20: 総セクタ数 (32bit) */
} __attribute__((packed)) FAT12_BPB;

/* ======== ディレクトリエントリ (32バイト) ======== */
typedef struct {
    char name[8];           /* 0x00: ファイル名 (スペースパディング) */
    char ext[3];            /* 0x08: 拡張子 (スペースパディング) */
    u8   attr;              /* 0x0B: 属性 */
    u8   reserved[10];      /* 0x0C: 予約 */
    u16  time;              /* 0x16: 最終更新時刻 */
    u16  date;              /* 0x18: 最終更新日付 */
    u16  start_cluster;     /* 0x1A: 開始クラスタ番号 */
    u32  file_size;         /* 0x1C: ファイルサイズ */
} __attribute__((packed)) FAT12_DirEntry;

/* ======== 属性フラグ ======== */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME    0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F   /* VFAT長いファイル名エントリ */

/* ======== FAT特殊値 ======== */
#define FAT12_FREE         0x000
#define FAT12_RESERVED     0x001
#define FAT12_EOC          0xFF8   /* End of Cluster chain (0xFF8-0xFFF) */
#define FAT12_BAD          0xFF7

/* ======== サイズ等の定数 ======== */
#define FAT12_MAX_FAT_SIZE (9 * 1024)
#define FAT12_NAME_LEN     11

/* ======== マウント情報 ======== */
typedef struct {
    int mounted;
    int drive_num;
    u16 bytes_per_sector;       /* セクタサイズ */
    u8  sectors_per_cluster;    /* クラスタあたりセクタ数 */
    u16 reserved_sectors;       /* 予約セクタ数 */
    u8  num_fats;               /* FAT数 */
    u16 root_entry_count;       /* ルートディレクトリエントリ数 */
    u16 total_sectors;          /* 総セクタ数 */
    u16 fat_size;               /* FATあたりセクタ数 */

    /* 計算値 */
    u32 fat_start_lba;          /* FAT開始LBA */
    u32 root_dir_start_lba;     /* ルートディレクトリ開始LBA */
    u32 root_dir_sectors;       /* ルートディレクトリセクタ数 */
    u32 data_start_lba;         /* データ領域開始LBA */
    u32 total_data_clusters;    /* データクラスタ総数 */
} FAT12_Info;

/* ======== API ======== */

/* マウント: ブートセクタ (LBA 0) からBPBを読み出してFAT情報を初期化 */
int fat12_mount(int dev_id);

/* アンマウント */
void fat12_unmount(void);

/* マウント状態確認 */
int fat12_is_mounted(void);

/* ディレクトリ一覧表示 (シェル用コールバック) */
/* print_fn: void callback(const char *name, u32 size, u8 attr) */
int fat12_list(void (*print_fn)(const char *, u32, u8));

/* ファイル読込: bufにデータを読込。戻り値: 読込バイト数, -1=エラー */
int fat12_read(const char *name, void *buf, int maxsz);

/* ファイル書込: 戻り値: 0=成功, -1=エラー */
int fat12_write(const char *name, const void *buf, int sz);

/* ファイル削除: 戻り値: 0=成功, -1=エラー */
int fat12_delete(const char *name);

/* マウント情報の取得 */
const FAT12_Info *fat12_get_info(void);

/* VFSへの登録初期化 */
void fat12_init(void);

#endif /* FAT12_H */
