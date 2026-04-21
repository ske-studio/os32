/* ======================================================================== */
/*  PYXEL.H — libpyxel 公開APIヘッダ                                        */
/*                                                                          */
/*  OS32上でPyxel互換のゲームエンジンを提供するライブラリ。                  */
/*  06_IMPLEMENTATION_DETAILS.md / 04_API_MAPPING.md 準拠。                 */
/*                                                                          */
/*  アーキテクチャ:                                                          */
/*    libpyxel (本ヘッダ)                                                   */
/*      └→ libos32gfx (描画プリミティブ)                                    */
/*          └→ KAPI v27 (カーネルサービス)                                  */
/* ======================================================================== */

#ifndef __PYXEL_H
#define __PYXEL_H

#include "os32api.h"

/* ======================================================================== */
/*  システム定数                                                             */
/* ======================================================================== */

/* 基本画面サイズ (Pyxel仮想解像度) */
#define PYXEL_WIDTH   256
#define PYXEL_HEIGHT  192

/* 内部カラーパレット数 */
#define PYXEL_COLORS  16

/* 表示倍率 */
#define PYXEL_SCALE   2

/* 表示解像度 (PC-98 VRAM上での実サイズ) */
#define PYXEL_DISP_WIDTH   (PYXEL_WIDTH * PYXEL_SCALE)    /* 512 */
#define PYXEL_DISP_HEIGHT  (PYXEL_HEIGHT * PYXEL_SCALE)   /* 384 */

/* UI領域 (ゲーム領域の右側) */
#define PYXEL_UI_X     512
#define PYXEL_UI_Y     0
#define PYXEL_UI_W     128
#define PYXEL_UI_H     384

/* ステータスバー (画面下部) */
#define PYXEL_STATUS_Y  384
#define PYXEL_STATUS_H  16

/* ======================================================================== */
/*  キーボード定数 (PC-98スキャンコード)                                     */
/* ======================================================================== */

#define PYXEL_KEY_ESCAPE  0x00
#define PYXEL_KEY_1       0x01
#define PYXEL_KEY_2       0x02
#define PYXEL_KEY_3       0x03
#define PYXEL_KEY_4       0x04
#define PYXEL_KEY_5       0x05
#define PYXEL_KEY_6       0x06
#define PYXEL_KEY_7       0x07
#define PYXEL_KEY_8       0x08
#define PYXEL_KEY_9       0x09
#define PYXEL_KEY_0       0x0A

#define PYXEL_KEY_Q       0x10
#define PYXEL_KEY_W       0x11
#define PYXEL_KEY_E       0x12
#define PYXEL_KEY_R       0x13
#define PYXEL_KEY_T       0x14
#define PYXEL_KEY_Y       0x15
#define PYXEL_KEY_U       0x16
#define PYXEL_KEY_I       0x17
#define PYXEL_KEY_O       0x18
#define PYXEL_KEY_P       0x19

#define PYXEL_KEY_A       0x1D
#define PYXEL_KEY_S       0x1E
#define PYXEL_KEY_D       0x1F
#define PYXEL_KEY_F       0x20
#define PYXEL_KEY_G       0x21
#define PYXEL_KEY_H       0x22
#define PYXEL_KEY_J       0x23
#define PYXEL_KEY_K       0x24
#define PYXEL_KEY_L       0x25

#define PYXEL_KEY_Z       0x29
#define PYXEL_KEY_X       0x2A
#define PYXEL_KEY_C       0x2B
#define PYXEL_KEY_V       0x2C
#define PYXEL_KEY_B       0x2D
#define PYXEL_KEY_N       0x2E

#define PYXEL_KEY_RETURN  0x1C
#define PYXEL_KEY_SPACE   0x34

#define PYXEL_KEY_UP      0x3A
#define PYXEL_KEY_LEFT    0x3B
#define PYXEL_KEY_RIGHT   0x3C
#define PYXEL_KEY_DOWN    0x3D

/* ======================================================================== */
/*  サウンド定数 (Phase 4 用に定義のみ)                                      */
/* ======================================================================== */

/* 波形種別 (tones) */
#define PYXEL_TONE_TRIANGLE  0
#define PYXEL_TONE_SQUARE    1
#define PYXEL_TONE_PULSE     2
#define PYXEL_TONE_NOISE     3

/* エフェクト種別 (effects) */
#define PYXEL_EFFECT_NONE           0
#define PYXEL_EFFECT_SLIDE          1
#define PYXEL_EFFECT_VIBRATO        2
#define PYXEL_EFFECT_FADEOUT        3
#define PYXEL_EFFECT_HALF_FADEOUT   4
#define PYXEL_EFFECT_QUARTER_FADEOUT 5

/* サウンドタイミング */
#define PYXEL_TICK_HZ  120   /* 1 tick = 1/120 秒 */

/* ======================================================================== */
/*  グローバル変数                                                           */
/* ======================================================================== */

/* 起動してからの総フレーム数 (pyxel_run内でインクリメントされる) */
extern unsigned int pyxel_frame_count;

/* 直近の実測FPS値 */
extern int pyxel_fps;

/* ======================================================================== */
/*  システム管理 (pyxel_core.c)                                              */
/* ======================================================================== */

/* 初期化 (GFX初期化 + Pyxel16色パレット設定)
 * width, height: Pyxel仮想解像度 (通常 PYXEL_WIDTH, PYXEL_HEIGHT)
 * api: KernelAPIポインタ
 */
void pyxel_init(int width, int height, KernelAPI *api);

/* メインループ (コールバック関数を登録)
 * update: 毎フレーム呼ばれるゲームロジック関数
 * draw:   毎フレーム呼ばれる描画関数
 *
 * 注意: ループ内部で dirty rect 転送と VSYNC 同期を行う。
 *       ESC キーで自動終了。
 */
void pyxel_run(void (*update)(void), void (*draw)(void));

/* 終了処理 (GFXシャットダウン + TVRAM復帰) */
void pyxel_quit(void);

/* ======================================================================== */
/*  グラフィック描画 (pyxel_gfx.c)                                           */
/*                                                                          */
/*  全座標はPyxel仮想座標系 (0,0)-(255,191)。                               */
/*  内部で2倍して libos32gfx に渡す。                                        */
/*  カメラオフセットとクリッピングは自動適用される。                          */
/* ======================================================================== */

/* 画面クリア
 * 警告: 毎フレーム呼出で 9fps 上限 (Test 6 実測値)。
 *       高FPSが必要な場合は cls() を避け、スプライトオーバーレイ方式を使う。
 */
void pyxel_cls(int col);

/* ピクセル描画/取得 */
void pyxel_pset(int x, int y, int col);
int  pyxel_pget(int x, int y);

/* プリミティブ描画 — 線 */
void pyxel_line(int x1, int y1, int x2, int y2, int col);

/* プリミティブ描画 — 矩形 */
void pyxel_rect(int x, int y, int w, int h, int col);   /* 塗りつぶし */
void pyxel_rectb(int x, int y, int w, int h, int col);  /* 枠のみ */

/* プリミティブ描画 — 円 */
void pyxel_circ(int x, int y, int r, int col);   /* 塗りつぶし */
void pyxel_circb(int x, int y, int r, int col);  /* 枠のみ */

/* プリミティブ描画 — 三角形 */
void pyxel_tri(int x1, int y1, int x2, int y2,
               int x3, int y3, int col);          /* 塗りつぶし */
void pyxel_trib(int x1, int y1, int x2, int y2,
                int x3, int y3, int col);         /* 枠のみ */

/* テキスト描画 (OS32組み込みANKフォント) */
void pyxel_text(int x, int y, const char *s, int col);

/* パレットスワップ設定 (描画時の色番号を差し替え) */
void pyxel_pal(int col1, int col2);

/* パレットをデフォルトにリセット */
void pyxel_pal_reset(void);

/* カメラ (描画オフセット) 設定 */
void pyxel_camera(int x, int y);

/* クリッピング領域設定 */
void pyxel_clip(int x, int y, int w, int h);

/* クリッピングをリセット (全画面) */
void pyxel_clip_reset(void);

/* ======================================================================== */
/*  入力管理 (pyxel_input.c)                                                 */
/* ======================================================================== */

/* キー押下状態 (押下中なら非0) */
int pyxel_btn(int key);

/* 押された瞬間のトリガー判定 (hold/repeatフレームでのリピート対応)
 * hold=0, repeat=0: 押した瞬間の1回だけ
 * hold>0, repeat>0: holdフレーム後にrepeatフレーム間隔でリピート
 */
int pyxel_btnp(int key, int hold, int repeat);

/* 離された瞬間のリリース判定 */
int pyxel_btnr(int key);

/* ======================================================================== */
/*  Phase 3-4 スタブ宣言 (未実装)                                            */
/* ======================================================================== */

/* リソース管理 (Phase 3: pyxel_res.c) */
void pyxel_load(const char *filename);

/* 画像転送 (Phase 3: pyxel_gfx.c) */
void pyxel_blt(int x, int y, int img, int u, int v,
               int w, int h, int colkey);
void pyxel_bltm(int x, int y, int tm, int u, int v,
                int w, int h, int colkey);

/* フラッドフィル (Phase 3: pyxel_gfx.c) */
void pyxel_fill(int x, int y, int col);

/* オーディオ (Phase 4: pyxel_snd.c) */
void pyxel_play(int ch, int snd);
void pyxel_playm(int msc);
void pyxel_stop(int ch);

#endif /* __PYXEL_H */
