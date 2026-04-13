/* ======================================================================== */
/*  OS32_KAPI_SHARED.H — KernelAPI 共有定義                                  */
/*                                                                          */
/*  カーネル (exec.h) と外部プログラム (os32api.h) の両方がインクルードする   */
/*  唯一の情報源 (Single Source of Truth)。                                  */
/*                                                                          */
/*  ■ 制約:                                                                */
/*    - このヘッダはカーネル内部ヘッダに一切依存してはならない               */
/*    - 基本型 (u8/u16/u32等) はここで自己完結的に定義する                   */
/*    - KernelAPI構造体のレイアウト変更は必ずここで行い、                    */
/*      exec.h / os32api.h で重複定義しないこと                              */
/* ======================================================================== */

#ifndef OS32_KAPI_SHARED_H
#define OS32_KAPI_SHARED_H

/* ======================================================================== */
/*  基本型定義 (フリースタンディング環境用)                                   */
/* ======================================================================== */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef signed char    i8;
typedef signed short   i16;
typedef signed long    i32;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif

/* ======================================================================== */
/*  KernelAPI バージョン                                                     */
/* ======================================================================== */

#define KAPI_VERSION      24

/* ======================================================================== */
/*  システム共通制限値 (SSoT)                                                */
/* ======================================================================== */

#define OS32_MAX_PATH     256
#define OS32_MAX_ARGS     256
#define OS32_GFX_WIDTH    640
#define OS32_GFX_HEIGHT   400

/* ======================================================================== */
/*  システム共通ステータスコード                                             */
/* ======================================================================== */

/* 実行エンジンのステータスコード */
typedef enum {
    EXEC_SUCCESS = 0,
    EXEC_ERR_GENERAL = -1,
    EXEC_ERR_FAULT = -2,     /* 例外/フォールトによる強制終了 */
    EXEC_ERR_NOT_FOUND = -3, /* 実行ファイルが見つからない */
    EXEC_ERR_NOMEM = -4,     /* メモリ不足 */
    EXEC_ERR_INVALID = -5    /* OS32Xヘッダが不正 */
} exec_status_t;

/* ======================================================================== */
/*  KernelAPI テーブルの配置アドレス                                          */
/* ======================================================================== */

#define KAPI_ADDR         0x189000UL

/* ======================================================================== */
/*  OS32X バイナリヘッダ                                                     */
/* ======================================================================== */

#define OS32X_MAGIC       0x4F533332UL  /* 'OS32' リトルエンディアン */
#define OS32X_HDR_V1_SIZE 40            /* v1ヘッダのサイズ */

/* フラグ定義 */
#define OS32X_FLAG_GFX    0x0001        /* GFXモードを使用 */

typedef struct {
    u32 magic;            /* 0x00: OS32X_MAGIC */
    u32 header_size;      /* 0x04: ヘッダ全体のサイズ (バイト) */
    u32 version;          /* 0x08: ヘッダバージョン (現在: 1) */
    u32 flags;            /* 0x0C: フラグ */
    u32 entry_offset;     /* 0x10: エントリポイント */
    u32 text_size;        /* 0x14: コード + 初期化済みデータ */
    u32 bss_size;         /* 0x18: BSS領域サイズ */
    u32 heap_size;        /* 0x1C: 要求ヒープサイズ */
    u32 stack_size;       /* 0x20: 要求スタックサイズ */
    u32 min_api_ver;      /* 0x24: 必要な最低KernelAPIバージョン */
} OS32Header;             /* 合計: 40バイト (0x28) */

/* ======================================================================== */
/*  KernelAPI 構造体                                                         */
/*                                                                          */
/*  外部プログラムがカーネル関数を呼ぶためのテーブル。                        */
/*  固定アドレス KAPI_ADDR に配置される。                                    */
/*  新しい関数は末尾に追加すること（バイナリ互換維持）。                      */
/* ======================================================================== */



/* ======================================================================== */
/*  共有構造体定義 (外部プログラムで直接使用可能)                              */
/* ======================================================================== */

/* 矩形 (ダーティレクタングルのX/Wは32の倍数でアライメントされる) */
typedef struct {
    int x, y, w, h;
} GFX_Rect;

/* ハードウェアバックバッファ本体の構造体 (PC-98プレーン構造) */
typedef struct {
    int width;      /* 640 */
    int height;     /* 400 */
    int pitch;      /* 80 bytes per line */
    u8 *planes[4];  /* 0:B, 1:R, 2:G, 3:I */
} GFX_Framebuffer;

/* パレットエントリ (各0-15) */
typedef struct {
    u8 r, g, b;
} GFX_Color;

/* サーフェス — オフスクリーン描画バッファ */
typedef struct {
    int w, h;
    int pitch;          /* バイト/ライン (w+7)/8 */
    u8 *planes[4];      /* 4プレーン (パックドビット) */
    int _pool_idx;      /* 静的プール管理用 (-1=外部管理) */
} GFX_Surface;

/* スプライト — マスク付き事前コンパイル済み (透過描画用) */
typedef struct {
    int w, h;
    int pitch;
    u8 *planes[4];      /* スプライトデータ */
    u8 *mask;           /* ANDマスク (透過=0xFF, 不透過=0x00) */
    u8 *bg_buf;         /* 自動背景退避用バッファ (4プレーン連続) */
    int _pool_idx;
} GFX_Sprite;

/* ラスタパレットエントリ (1分割 = 2ライン単位) */
typedef struct {
    u16 line;           /* 開始ライン (0-398, 2ライン単位推奨) */
    u8  pal_idx;        /* パレット番号 (0-15) */
    u8  r, g, b;        /* RGB値 (0-15) */
    u8  _pad[2];        /* アラインメント用 */
} GFX_RasterPalEntry;

#define GFX_RASTER_MAX_ENTRIES 200  /* 最大200エントリ (NP21/W: 1024イベント制限) */

/* ラスタパレットテーブル (外部プログラムが構築→カーネルに渡す) */
typedef struct {
    int count;                                    /* 有効エントリ数 */
    GFX_RasterPalEntry entries[GFX_RASTER_MAX_ENTRIES];
} GFX_RasterPalTable;

/* RTC時刻構造体 */
typedef struct {
    u8 year, month, day, wday, hour, min, sec;
} RTC_Time_Ext;

/* ファイル種別 (OS32_FILE_TYPE_*) */
#define OS32_FILE_TYPE_FILE 1
#define OS32_FILE_TYPE_DIR  2

/* ディレクトリエントリ (コールバック用) */
typedef struct {
    char name[OS32_MAX_PATH];
    u32  size;
    u8   type;  /* OS32_FILE_TYPE_FILE / OS32_FILE_TYPE_DIR */
} DirEntry_Ext;

/* DirEntry_Ext コールバック型 */
typedef void (*DirCallback)(const DirEntry_Ext *entry, void *ctx);

/* コンソール属性色 */
#define ATTR_WHITE   0xE1
#define ATTR_CYAN    0xA1
#define ATTR_GREEN   0x81
#define ATTR_YELLOW  0xC1
#define ATTR_RED     0x41
#define ATTR_MAGENTA 0x61

/* ======================================================================== */
/*  ファイル属性と時間 (Stat)                                               */
/* ======================================================================== */

/* UNIX時間に準拠した 32-bit (符号なし) エポック秒 (1970年1月1日〜) */
typedef u32 os_time_t;

/* ファイル種別 (st_mode の S_IFMT ビットマスク) */
#define OS_S_IFMT   0xF000
#define OS_S_IFCHR  0x2000 /* キャラクタデバイス */
#define OS_S_IFDIR  0x4000 /* ディレクトリ */
#define OS_S_IFREG  0x8000 /* 通常ファイル */

/* パーミッションフラグ */
#define OS_S_IRUSR  00400  /* User (システムではエンドユーザー) Read */
#define OS_S_IWUSR  00200  /* User Write */
#define OS_S_IXUSR  00100  /* User eXecute */
#define OS_S_IRWXU  00700  /* User R/W/X mask */

#define OS_S_IRGRP  00040  /* Group (プログラム) Read */
#define OS_S_IWGRP  00020  /* Group Write */
#define OS_S_IXGRP  00010  /* Group eXecute */
#define OS_S_IRWXG  00070  /* Group R/W/X mask */

#define OS_S_IROTH  00004  /* Other (OS/システム) Read */
#define OS_S_IWOTH  00002  /* Other Write */
#define OS_S_IXOTH  00001  /* Other eXecute */
#define OS_S_IRWXO  00007  /* Other R/W/X mask */

typedef struct {
    u32       st_dev;     /* デバイスID (マウントポイント等) */
    u32       st_ino;     /* inode番号 (FS一意の識別子) */
    u16       st_mode;    /* ファイル種別 + パーミッション (16bit) */
    u16       st_nlink;   /* ハードリンク数 (FATでは常に 1) */
    u16       st_uid;     /* 所有ユーザー ID (OS32では固定化) */
    u16       st_gid;     /* 所有プログラム(グループ) ID */
    u32       st_size;    /* ファイルサイズ (バイト) */
    os_time_t st_atime;   /* 最終アクセス日時 (UNIX Epoch) */
    os_time_t st_mtime;   /* 最終更新日時 (UNIX Epoch) */
    os_time_t st_ctime;   /* 状態変更日時・作成日時 (UNIX Epoch) */
} OS32_Stat;

/* ======================================================================== */
/*  ファイル I/O 定数 (Stream API)                                           */
/* ======================================================================== */

/* オープンモード API定数 */
#define KAPI_O_RDONLY    0x00
#define KAPI_O_WRONLY    0x01
#define KAPI_O_RDWR      0x02
#define KAPI_O_CREAT     0x0100
#define KAPI_O_TRUNC     0x0200

#ifndef O_RDONLY
#define O_RDONLY    KAPI_O_RDONLY
#define O_WRONLY    KAPI_O_WRONLY
#define O_RDWR      KAPI_O_RDWR
#define O_CREAT     KAPI_O_CREAT
#define O_TRUNC     KAPI_O_TRUNC
#endif

/* シーク起点 */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* 自動生成された APIテーブルを、全ての構造体が定義された後でインクルード */
#include "os32_kapi_generated.h"

/* 外部プログラムのエントリポイント型 */
typedef void (__cdecl *ExecEntry)(int argc, char **argv, KernelAPI *api);

#endif /* OS32_KAPI_SHARED_H */
