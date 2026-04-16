/* ======================================================================== */
/*  DEV.H — デバイス抽象化層                                                */
/*                                                                          */
/*  DOS風ドライブ名方式 (fdd0:, hdd0:, con:, serial:)                      */
/*  キャラクタデバイス (read/write = バイト単位)                             */
/*  ブロックデバイス (read/write = セクタ単位)                               */
/* ======================================================================== */

#ifndef DEV_H
#define DEV_H

#include "types.h"  /* u8, u16, u32 */

/* ======== デバイスタイプ ======== */
typedef enum {
    DEV_CHAR,       /* キャラクタデバイス (con, serial, fm) */
    DEV_BLOCK       /* ブロックデバイス (fdd, hdd) */
} DevType;

/* ======== デバイス構造体 ======== */
typedef struct _Device Device;
struct _Device {
    const char *name;        /* デバイス名 ("fdd0", "con", "serial") */
    DevType     type;        /* DEV_CHAR or DEV_BLOCK */
    int         sect_size;   /* セクタサイズ (ブロックデバイスのみ) */
    u32         total_sects; /* 総セクタ数 (ブロックデバイスのみ) */

    /* ブロックデバイス用: LBA単位 */
    int       (*blk_read)(Device *self, int lba, int count, void *buf);
    int       (*blk_write)(Device *self, int lba, int count, const void *buf);

    /* キャラクタデバイス用: バイト単位 */
    int       (*chr_read)(Device *self, void *buf, int len);
    int       (*chr_write)(Device *self, const void *buf, int len);

    /* デバイス固有制御 */
    int       (*ioctl)(Device *self, int cmd, void *arg);

    void       *priv;        /* ドライバ固有データへのポインタ */
};

/* ======== 定数 ======== */
#define MAX_DEVICES  10

/* ======== API ======== */

/* デバイスシステム初期化 (全ドライバ登録) */
void dev_init(void);

/* デバイス登録 (戻り値: 0=成功, -1=満杯) */
int dev_register(Device *dev);

/* HDDデバイスの登録 (ユーティリティ) */
void dev_register_hdd(int drive);

/* CD-ROMデバイスの登録 (ATAPIドライバ検出済みの場合) */
void dev_register_cdrom(void);

/* デバイス検索 (名前で) */
Device *dev_find(const char *name);

/* デバイス取得 (インデックスで, 0〜dev_count()-1) */
Device *dev_get(int index);

/* 登録デバイス数 */
int dev_count(void);

/* デバイス名一覧取得 (Tab補完用: names配列にポインタを格納, 戻り値=個数) */
int dev_get_names(const char **names, int max);

/* API用デバイス情報取得 (構造体を隠蔽) */
int dev_api_get_info(int idx, char *name, int name_max, int *type, u32 *total_sects);

#endif /* DEV_H */
