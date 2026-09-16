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

#define KAPI_VERSION      53   /* 排他的作成 (票 H2): sys_open の KAPI_O_EXCL (0x0400)。O_CREAT と組でだけ有効で、名前が何であれ既に あれば OS32_ERR_EXIST、判定できなければその負値。VfsOps.create_excl を 持つ FS (ext2) だけが受け、持たない FS は OS32_ERR_NOSYS。スロットは増えていない (フラグだけ) が sys_open の意味が広がるので 版数を上げる ([ABI3])。v52 = mtime の保存 (票 H3): sys_set_mtime (VfsOps の任意実装フック。ext2 のみ実装、他の FS は OS32_ERR_NOSYS)。v51 = Host Services の基盤 (票 N1): host_open / host_status / host_read / host_write / host_close の 5 本 (非ブロッキング、同時 2 ハンドル、プロトコルを進めるのは 100Hz の link_tick だけ)。v50 = 設定レジストリの基盤 (票 S0-K): db_open_existing (RO / RW、CREATE 無し) / db_prepare_only / db_bind_int / db_bind_text / db_bind_blob / db_bind_null / db_error_code の 7 本。v49 = T9: 起動要求表 launch_req / launch_pending / launch_take / launch_report / launch_poll / launch_cancel / launch_child と sys_yield。v48 = T8: gfx_screen_owner。v47 = K7: kbd_inject / kbd_inject_pending。v46 = con_sink_read / con_sink_stat */

/* ======================================================================== */
/*  SQLite DB API 共有定数・構造体                                           */
/* ======================================================================== */

/* DB 最大接続数 */
#define DB_MAX_CONNECTIONS    8

/* DB結果ステータス */
#define DB_STATUS_DONE     0    /* クエリ完了 (行なし or 最終行到達) */
#define DB_STATUS_ROW      1    /* 行データあり (db_step で次を取得) */
#define DB_STATUS_ERROR   (-1)  /* エラー発生 */

/* DB カラム型 */
#define DB_TYPE_INT        1
#define DB_TYPE_FLOAT      2
#define DB_TYPE_TEXT       3
#define DB_TYPE_BLOB       4
#define DB_TYPE_NULL       5

/* IPC 共有メモリブロックサイズ (DB結果用) */
#define DB_SHM_BLOCK_SIZE  (16 * 1024)

/* v50 (票 S0-K §1a) — db_prepare_only / db_bind_* の上限。カーネル側
 * (kapi/kapi_db.c) の検証用スクラッチもこの値で取るので、ここが唯一の
 * 管理元 ([C4])。超過は**切り捨てず拒否**する。 */
#define DB_SQL_MAX_BYTES   1024   /* SQL 文字列 (NUL 込み) */
#define DB_BIND_TEXT_MAX   255    /* db_bind_text の length (NUL を含まない) */
#define DB_BIND_BLOB_MAX   4096   /* db_bind_blob の length */

/* DB_ResultHeader — db_exec/db_step 結果の先頭に配置 */
typedef struct {
    i32 status;         /* DB_STATUS_xxx */
    i32 column_count;   /* 列数 (0 = 結果セットなし) */
    i32 error_offset;   /* エラーメッセージのオフセット (0 = エラーなし) */
} DB_ResultHeader;

/* DB_ColumnInfo — 各カラムの型・サイズ・データ位置 */
typedef struct {
    i32 type;           /* DB_TYPE_xxx */
    i32 length;         /* データサイズ (バイト) */
    i32 data_offset;    /* 共有メモリ先頭からのデータ位置 */
} DB_ColumnInfo;

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
/*  カーネルビルド時のみ有効。外部プログラムは main() 第3引数を使用。         */
/* ======================================================================== */
#ifdef __KERNEL_BUILD__
#include "memmap.h"
#define KAPI_ADDR         (KHEAP_BASE + KHEAP_SIZE)
#endif

/* ======================================================================== */
/*  OS32X バイナリヘッダ                                                     */
/* ======================================================================== */

#define OS32X_MAGIC       0x4F533332UL  /* 'OS32' リトルエンディアン */
#define OS32X_HDR_V1_SIZE 40            /* v1ヘッダのサイズ */
#define OS32X_HDR_V2_SIZE 44            /* v2ヘッダのサイズ (load_addr を末尾に追記) */
#define OS32X_HDR_VERSION 2             /* mkos32x.py が現在焼く version */

/* フラグ定義 */
#define OS32X_FLAG_GFX    0x0001        /* GFXモードを使用 */
#define OS32X_FLAG_RING3  0x0002        /* CPL=3 (リング3) で実行する (v2 M1) */
#define OS32X_FLAG_FORCE_CPL0 0x0004    /* CPL=0 強制 (ring3 デフォルト化後のエスケープハッチ, v2 M3) */
#define OS32X_FLAG_SHLIB  0x0008        /* 共有ライブラリ (MEM_SHLIB_BASE 常駐、GUI v1.1 K3/C3) */
/* CUI 専用 (mkos32x --cui-only、app.conf 4 列目 `cui`、票 T8-2)。
 * V86 / VDM のように「CPL=3 のプログラムだが KAPI の向こうで画面と BIOS を
 * 丸ごと持っていく」ものに立てる。GUI (con_sink 有効) からの exec_start は
 * OS32_ERR_INVAL で断り、CUI からは従来どおり通す。FORCE_CPL0 とは独立 —
 * v86.bin は CPL=3 なので FORCE_CPL0 では捕まらなかった (受入 F5)。 */
#define OS32X_FLAG_CUI_ONLY 0x0010
/* 起動要求を出せる宣言 (mkos32x --launcher、app.conf 4 列目 `launcher`、票 T9 D3)。
 * GUI 中の CPL=3 アプリは入れ子 exec_run を使えないので、外部プログラムの起動は
 * カーネルの要求表 (launch_req) 経由で WM に頼む。その口を叩けるのはこの宣言を
 * 持つバイナリだけ — 認証ではなく協調的な宣言で、任意のアプリが端末経由の
 * 非同期起動を使えてしまう穴を塞ぐためのもの (票 §5 blocker 2)。 */
#define OS32X_FLAG_LAUNCHER 0x0020

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
    /* ---- v2 (header_size >= OS32X_HDR_V2_SIZE のときだけ有効) ---- */
    u32 load_addr;        /* 0x28: リンク時のロードアドレス (mkos32x が ELF の
                           * .text から焼く)。exec はここが自分の置こうとして
                           * いる番地と一致するかを確かめ、食い違えば
                           * OS32_ERR_INVAL で弾く。0 = 不明 (警告のみ)。
                           * GUI v1.1 K3 でロードアドレスが 0x400000 →
                           * 0x500000 に動いたため、旧バイナリ (version 1 /
                           * header_size 40) が黙って別番地へ飛ぶのを防ぐ。 */
} OS32Header;             /* 合計: 44バイト (0x2C)。version 1 のバイナリは
                           * 先頭 40 バイト (min_api_ver まで) のみ有効。 */

/* ======================================================================== */
/*  共有ライブラリ (OS32X_FLAG_SHLIB) のジャンプ表 — GUI v1.1 K3 / C3        */
/*                                                                          */
/*  ライブラリ本体 (OS32X) の先頭 4KB がこのヘッダ。カーネル (K3) が            */
/*  MEM_SHLIB_BASE に読み、.text/.rodata を read-only + USER で全 PD に共有、  */
/*  data_vaddr から data_pages ページをアプリごとの物理ページにする。          */
/*  アプリ側スタブ (C3) は magic / version を照合してから entry[i] を呼ぶ。    */
/*  entry[] は **末尾追記のみ** (KAPI と同じ作法)。ヘッダ 32B の直後から。      */
/* ======================================================================== */
#define OS32_SHLIB_MAGIC      0x42494C53UL  /* 'SLIB' リトルエンディアン */
#define OS32_SHLIB_HDR_SIZE   4096          /* ジャンプ表を含む先頭ページ */
#define OS32_SHLIB_ENTRY_OFF  32            /* entry[0] のヘッダ先頭からのオフセット */
#define OS32_SHLIB_MAX_FUNC   ((OS32_SHLIB_HDR_SIZE - OS32_SHLIB_ENTRY_OFF) / 4)

typedef struct {
    u32 magic;        /* 0x00: OS32_SHLIB_MAGIC */
    u32 version;      /* 0x04: ライブラリの版 (libos32gui は GUI_PROTO_VERSION) */
    u32 nfunc;        /* 0x08: entry[] の有効数 */
    u32 data_vaddr;   /* 0x0C: .data/.bss の先頭仮想アドレス (ページ境界) */
    u32 data_pages;   /* 0x10: アプリごとに複製する物理ページ数 */
    u32 text_pages;   /* 0x14: 共有する read-only ページ数 (先頭ページ含む) */
    u32 _rsvd[2];     /* 0x18: 0 */
    /* 0x20: u32 entry[nfunc] — 関数の絶対アドレス (C89 なので配列は持たない。
     *       ((const u32 *)hdr)[OS32_SHLIB_ENTRY_OFF / 4 + i] で読む) */
} OS32ShlibHeader;    /* 32B */

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

/* 画面能力 (GUI HAL 用、docs/tasks/gui/API_CONTRACTS.md G5)。gfx_screen_info() が埋める。
 * バックエンド (9801 プレーン / PEGC 8bpp / アクセラレータ) の違いはここで問い合わせ、
 * GUI とアプリは 400 ライン・16 色を決め打ちしない。末尾追記のみ。 */
#define GFX_FMT_PLANAR4   0   /* 4 プレーン 16 色 (PC-9801 標準) */
#define GFX_FMT_PACKED8   1   /* 1 バイト 1 ピクセル 256 色 (PEGC / アクセラレータ) */
#define GFX_CAP_TEXT_OVERLAY  0x0001  /* テキスト VRAM がグラフィック上に合成される */
#define GFX_CAP_HW_FILL       0x0002  /* gfx_hw_fill_rect が使える */
#define GFX_CAP_HW_BLT        0x0004  /* gfx_hw_blit が使える */
#define GFX_CAP_PAGE_FLIP     0x0008  /* 表裏ページ切替あり */
typedef struct {
    u16 width, height;   /* 640 x 400/480 ... */
    u8  bpp;             /* 4 or 8 */
    u8  format;          /* GFX_FMT_* */
    u32 flags;           /* GFX_CAP_* */
    u16 lease_mask;      /* 16 色: フォーカスアプリに貸せるパレット index のビット集合 (G8) */
    u16 lease_first;     /* 256 色: 貸せる先頭 index */
    u16 lease_count;     /* 256 色: 貸せる項目数 */
    u16 reserved[5];
} GFX_ScreenInfo;

/* GUI カウンタ (gfx_stats() が埋める、docs/tasks/gui/API_CONTRACTS.md G7)。
 * 累積値。NP21/W ではウェイトを再現しないので、性能はこの回数・転送量で
 * 見積もる (契約 P2)。末尾追記のみ。 */
typedef struct {
    u32 present_bytes;   /* VRAM へ転送したバイト数の累計 */
    u32 hw_ops;          /* アクセラレータの塗り/転送回数 */
    u32 io_accesses;     /* I/O ポートアクセス回数 */
    u32 commits;         /* commit 回数 */
} GFX_Stats;

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

/* マウス情報構造体 (mouse_poll 用) */
typedef struct {
    i16  x;          /* 現在のX座標 (画面座標) */
    i16  y;          /* 現在のY座標 (画面座標) */
    i16  dx;         /* X差分 (前回poll以降) */
    i16  dy;         /* Y差分 (前回poll以降) */
    u8   buttons;    /* ボタンビットマスク (MOUSE_BTN_xxx) */
    u8   mode;       /* 0=なし, 1=バス, 2=シームレス */
} MouseInfo;

/* マウスボタンビットマスク */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* マウスカーソル表示モード */
#define MOUSE_CURSOR_NONE  0  /* カーソル非表示 (生ポーリング専用) */
#define MOUSE_CURSOR_TEXT  1  /* TVRAM属性反転カーソル */
#define MOUSE_CURSOR_GFX   2  /* GFXスプライトカーソル (将来用) */

/* RTC時刻構造体 */
typedef struct {
    u8 year, month, day, wday, hour, min, sec;
} RTC_Time_Ext;

/* VFS 系 KAPI (sys_open/sys_chdir/sys_unlink/sys_mkdir/sys_rmdir/sys_rename
 * /sys_stat 等) が返す負のエラーコード。カーネル内の VFS_ERR_* (fs/vfs.h)
 * はこの別名。各 FS ドライバの内部コード (EXT2_ERR_* 等) はドライバ境界で
 * 必ずこの体系に変換される */
#define OS32_ERR_IO        -1   /* 入出力エラー */
#define OS32_ERR_NOTFOUND  -2   /* パスが存在しない */
#define OS32_ERR_NOMOUNT   -3   /* 該当マウントなし / FS 未対応操作 */
#define OS32_ERR_NOSPC     -4   /* 空き容量・FD・スロット不足 */
#define OS32_ERR_EXIST     -5   /* 既に存在する */
#define OS32_ERR_NOTDIR    -6   /* ディレクトリではない */
#define OS32_ERR_NOTEMPTY  -7   /* ディレクトリが空でない */
#define OS32_ERR_ISDIR     -8   /* ディレクトリである (open/unlink 不可) */
#define OS32_ERR_INVAL     -9   /* 引数不正 / FS をまたぐ rename 等 */
#define OS32_ERR_NOSYS     -10  /* このバックエンド / 機種では未対応 */
/* GUI (KAPI v42、KAPI_SPEC §3-2 の予約どおり)。-14 以降はネットワーク用に空ける。 */
#define OS32_ERR_STALE     -11  /* 破棄済み / generation 不一致のハンドル (契約 T4) */
#define OS32_ERR_VERSION   -12  /* proto_version が WM より新しい (契約 T5) */
#define OS32_ERR_FULL      -13  /* スロット / 資源が満杯 (契約 T2a) */
/* GUI v1.3 K7 (KAPI v47)。KAPI_SPEC §3-2 の予約を 1 つ進め、ネットワークは
 * -15 以降へずらした (ネットワーク側は番号を 1 つも使っていない)。 */
#define OS32_ERR_AGAIN     -14  /* いまは無い / 後でもう一度 (kbd 待ちの resume) */
/* 票 B8 往復 5 (ユーザー決裁 2、2026-09-15)。**番号の追加だけで構造体・スロットは
 * 変えない**ので KAPI 版数は据え置き。KAPI_SPEC §3-2 の予約を 1 つ進め、
 * ネットワークは -16 以降へずらした (ネットワーク側は番号を 1 つも使っていない)。 */
#define OS32_ERR_ROFS      -15  /* 書き込みを受け付けない (FS がエラー状態、再マウントまで) */

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

/* ======================================================================== */
/*  console シンク (KAPI v46、票 K6C)                                        */
/*                                                                          */
/*  GUI モード中、カーネル / CUI コマンドが console.c の入口へ書いた出力は    */
/*  テキスト VRAM (非表示) に消える。これをカーネル内のリングに **レコード**  */
/*  として溜め、端末アプリが con_sink_read() で吸って Paint する。            */
/*                                                                          */
/*  ワイヤ形式 (先頭 1 バイトが型、残りは型ごと。詰め物・整列は無い):        */
/*    PRINT  : [type=1][color u8][len u8][UTF-8 バイト列 len 個]             */
/*    CLEAR  : [type=2]                                                      */
/*    CURSOR : [type=3][x u8][y u8]                                          */
/*    EXIT   : [type=4][id u8]        — gshell 配下の子が畳まれた (票 T7 E1) */
/*                                                                          */
/*  改行 / CR / TAB は PRINT のバイトとして流れる (端末モデルが解釈する)。   */
/*  スクロールはレコードにしない (行の追加に畳む)。                          */
/* ======================================================================== */
#define CON_SINK_REC_PRINT   1
#define CON_SINK_REC_CLEAR   2
#define CON_SINK_REC_CURSOR  3
#define CON_SINK_REC_EXIT    4

#define CON_SINK_PRINT_MAX   200  /* PRINT 1 本が運ぶ UTF-8 バイト数の上限 */
#define CON_SINK_HDR_PRINT   3    /* type + color + len */
#define CON_SINK_HDR_CLEAR   1    /* type */
#define CON_SINK_HDR_CURSOR  3    /* type + x + y */
#define CON_SINK_HDR_EXIT    2    /* type + id */
/* レコード 1 本の最大バイト数。con_sink_read() の cap はこれ以上でなければ
 * ならない (小さいと先頭レコードが永久に取り出せず読み手が止まるため)。 */
#define CON_SINK_REC_MAX     (CON_SINK_HDR_PRINT + CON_SINK_PRINT_MAX)

/* ======================================================================== */
/*  起動要求表 (KAPI v49、票 T9 D3 / §1a)                                    */
/*                                                                          */
/*  GUI 中の CPL=3 アプリ (端末・sh) は入れ子 exec_run を使えない (子が park  */
/*  できず協調型全体が止まる) ので、外部プログラムの起動と kill を **カーネル  */
/*  の要求表** に載せ、owner 1 (WM) が top-level で取りに来る。               */
/*                                                                          */
/*  表は要求者 ID ごとに 1 本 (ID 2〜5 の 4 本)。欄は「配送状態」(phase /     */
/*  kind) と「子の所有」(child) を分けてあり、取消や要求者の退場の途中でも    */
/*  child は消えない。照合は **token** で行う (要求者 ID は再利用されうる)。  */
/*                                                                          */
/*    launch_req    要求者 (宣言 LAUNCHER) → token                           */
/*    launch_pending 誰でも → PENDING の本数 (WM の should_park の材料)       */
/*    launch_take   owner 1 → token + kind + cmdline / 畳む ID               */
/*    launch_report owner 1 → 起動結果 (rc) を表へ返す                        */
/*    launch_poll   要求者 → status (下の LAUNCH_ST_*)                        */
/*    launch_cancel 要求者 → RUNNING を KILL(child) の PENDING へ             */
/*    launch_child  誰でも → その ID の表が所有する子 (連鎖の次)             */
/* ======================================================================== */

/* cmdline の欄 (NUL 込み)。launch_take の cap はこれ以上でなければならない。 */
#define LAUNCH_CMDLINE_MAX   256

/* launch_take が書く kind */
#define LAUNCH_KIND_NONE     0
#define LAUNCH_KIND_LAUNCH   1    /* buf に cmdline */
#define LAUNCH_KIND_KILL     2    /* arg = 畳む ID */

/* launch_poll が書く status。RUNNING は下位に子 ID、FAILED は下位に -rc。 */
#define LAUNCH_ST_PENDING    0x000
#define LAUNCH_ST_TAKEN      0x001
#define LAUNCH_ST_RUNNING    0x100   /* + child */
#define LAUNCH_ST_DONE       0x200
#define LAUNCH_ST_FAILED     0x300   /* + (-rc) */

/* token は 32bit の全体単調増加カウンタ。0 と負は使わない。ここに達したら
 * launch_req は OS32_ERR_FULL を返す (1 要求 1 token なので事実上到達しない)。 */
#define LAUNCH_TOKEN_MAX     0x7FFFFFFF

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
/* 排他的作成 (票 H2 §2-1、KAPI v53)。**O_CREAT と組でだけ有効** —
 * 単独で渡すと OS32_ERR_INVAL。名前が既に在れば種別を問わず OS32_ERR_EXIST
 * (ディレクトリでも ISDIR ではない)。在るかどうかを判定できなかったときは
 * その負値をそのまま返す (「読めなかった」を「無い」と読み替えない、票 B8)。
 * 排他性を持てるのは VfsOps.create_excl を実装した FS だけで、持たない FS は
 * OS32_ERR_NOSYS を返す (呼び手は黙って通常の作成へ落ちないこと)。
 * ホスト側が同時に書ける FS (HostDrv) では排他性は成り立たない。 */
#define KAPI_O_EXCL      0x0400

#ifndef O_RDONLY
#define O_RDONLY    KAPI_O_RDONLY
#define O_WRONLY    KAPI_O_WRONLY
#define O_RDWR      KAPI_O_RDWR
#define O_CREAT     KAPI_O_CREAT
#define O_TRUNC     KAPI_O_TRUNC
#define O_EXCL      KAPI_O_EXCL
#endif

/* シーク起点 */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* FDリダイレクトモード (sys_redirect_fd()用) */
#define FD_REDIR_READ      0   /* 読み込み (stdin用) */
#define FD_REDIR_WRITE     1   /* 書き込み・上書き */
#define FD_REDIR_APPEND    2   /* 書き込み・追記 */

/* パイプバッファの容量 */
#define PIPE_BUF_SIZE   (64 * 1024)

/* ======================================================================== */
/*  FEP モード定数と構造体                                                   */
/*                                                                          */
/*  ユーザ空間 (ime コマンド等) とカーネルの双方が参照するため、             */
/*  kernel/ime.h ではなく共有ヘッダに置く。                                  */
/* ======================================================================== */
#define IME_MODE_OFF       0   /* FEP無効 (直接入力) */
#define IME_MODE_HIRAGANA  1   /* ひらがな入力 */
#define IME_MODE_KATAKANA  2   /* カタカナ入力 */

/* ユーザ辞書エントリ (kapi ime_user_list が void* で返す実体) */
typedef struct {
    char yomi[32];      /* 読み (UTF-8, ヌル終端) */
    char kanji[32];     /* 漢字/表層形 (UTF-8, ヌル終端) */
    int  freq;          /* 変換頻度 */
} IME_UserEntry;

/* 自動生成された APIテーブルを、全ての構造体が定義された後でインクルード */
#include "os32_kapi_generated.h"

/* 外部プログラムのエントリポイント型 */
typedef void (__cdecl *ExecEntry)(int argc, char **argv, KernelAPI *api);

#endif /* OS32_KAPI_SHARED_H */
