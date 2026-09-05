/* ======================================================================== */
/*  GFX_HAL.H — グラフィクス HAL バックエンド表 (GUI v1.1, レーン H)         */
/*                                                                          */
/*  GUI (libos32gui / gshell) と WM はこの表と gfx_screen_info() だけを見て   */
/*  動き、機種 (9801 プレーン / PEGC 8bpp / Cirrus アクセラレータ) を知らない。 */
/*  各バックエンドは probe / init / present などを 1 枚の GfxBackend として   */
/*  実装し、GUI からはポート番号・VRAM アドレス・機種名を隠す。               */
/*                                                                          */
/*  設計: docs/tasks/gui/DESIGN.md §2 (B1〜B3), §6, §7 /                     */
/*        契約 docs/tasks/gui/API_CONTRACTS.md G5, G7, G8                    */
/* ======================================================================== */

#ifndef __GFX_HAL_H
#define __GFX_HAL_H

#include "types.h"             /* u8, u16, u32 */
#include "os32_kapi_shared.h"  /* GFX_ScreenInfo, GFX_FMT_*, GFX_CAP_* */

/* ------------------------------------------------------------------------ */
/*  バックバッファの画素形式 (ソフトウェア系バックエンドの記述子 bb_format)    */
/*  ソフトウェアバックエンドは描画プリミティブを共通の CPU 実装へ委ね、       */
/*  「先頭・ピッチ・画素形式」だけを渡す (DESIGN §7-1)。                       */
/* ------------------------------------------------------------------------ */
#define GFX_BB_PLANAR4  0   /* 4 プレーン。bb_base=プレーン0, 各面 height*pitch */
#define GFX_BB_PACKED8  1   /* 1 バイト 1 ピクセル (PEGC / アクセラレータ CPU 転送) */

/* ------------------------------------------------------------------------ */
/*  バックエンド関数表 (DESIGN §7-1 の契約)。                                */
/*                                                                          */
/*  描画プリミティブ (fill_rect / blit) は「アクセラレータがあれば」の枠で、   */
/*  NULL のときは呼び出し側 (共有ライブラリ = libos32gfx) が CPU 共通実装へ    */
/*  フォールバックする。文字とビットマップは v1 では CPU 共通実装に任せるため  */
/*  この表には置かない (契約 G2 の対応表)。                                   */
/* ------------------------------------------------------------------------ */
typedef struct GfxBackend {
    const char *name;

    int  (*probe)(void);                /* 機種検出。1 = このバックエンドが使える */
    int  (*init)(GFX_ScreenInfo *info); /* 能力ビットと画面情報を埋める。0 = 成功 */
    void (*shutdown)(void);             /* バックエンドのハードウェア終了処理 */

    void (*present_rect)(int x, int y, int w, int h); /* バックバッファ→表示面 */
    void (*set_palette)(int first, int count, const u8 *rgb); /* rgb は 3B/項目 */

    void (*enter)(void);  /* 表示出力の切替 (フルスクリーン GFX プログラム前後) */
    void (*leave)(void);

    /* 描画プリミティブ。NULL = CPU 共通実装へフォールバック (アクセラレータ用の枠) */
    int  (*fill_rect)(int x, int y, int w, int h, u8 color);
    int  (*blit)(int dx, int dy, int sx, int sy, int w, int h);

    /* バックバッファ記述子 (ソフトウェア系のみ。アクセラレータ系は bb_base=NULL) */
    u8  *bb_base;
    u32  bb_pitch;
    u8   bb_format;   /* GFX_BB_* */
} GfxBackend;

/* ------------------------------------------------------------------------ */
/*  カウンタ (契約 G7 / DESIGN §8)。                                        */
/*  NP21/W はバスウェイトを再現しないため性能は回数で見積もる。バックエンドが  */
/*  present / fill / blit / パレット設定の中で累積し、gfx_stats() が          */
/*  GFX_Stats へ写す。VRAM は読まない — 書き込みバイト数だけを数える。         */
/* ------------------------------------------------------------------------ */
typedef struct GfxCounters {
    u32 present_bytes;  /* present で表示面へ書いたバイト数 (機種に依らず正確) */
    u32 hw_ops;         /* 発行したエンジン操作数 (CPU バックエンドは 0) */
    u32 io_accesses;    /* I/O ポートアクセス数 (パレット OUT / ページ切替 等) */
    u32 commits;        /* present (commit) 回数 */
} GfxCounters;

/* gfx_core.c が実体を持つ (バックエンドは加算するだけ) */
extern GfxCounters gfx_counters;

/* 現在選択されているバックエンド。gfx_init() が probe 順に最初の 1 枚を選ぶ。
 * 静的初期化子で 9801 を指すので、gfx_init 前でも NULL にはならない。 */
extern const GfxBackend *g_backend;

/* 9801 プレーンバックエンド (gfx/backend_pc98.c)。
 * H2 (PEGC) / H3 (Cirrus) はこの表にもう 1 枚ずつ足す。 */
extern const GfxBackend gfx_backend_pc98;

#endif /* __GFX_HAL_H */
