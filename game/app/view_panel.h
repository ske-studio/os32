/* ======================================================================== */
/*  VIEW_PANEL.H — 下段の固定コマンドパネル                                  */
/*                                                                          */
/*  microUI のフローティングウィンドウをやめ、画面下端の固定矩形に            */
/*  行テキストを敷く方式。マウスは使わないゲームなのでボタンは持たず、        */
/*  すべての操作はキー表示 ("1: Roll Dice" など) で案内する。                */
/*                                                                          */
/*  毎フレーム panel_begin() -> panel_line() ... -> panel_end() を呼ぶ。     */
/*  内容が前フレームと同じなら描画も VRAM 転送も起きない (16MHz 対策)。      */
/* ======================================================================== */

#ifndef VIEW_PANEL_H
#define VIEW_PANEL_H

#include "os32api.h"

#define PANEL_X        0
#define PANEL_Y      320
#define PANEL_W      640
#define PANEL_H       80
#define PANEL_LINES    5    /* 14px x 5行 + 余白 */
/* 1行のバイト数。表示は半角78桁だが、日本語は UTF-8 で 1文字3バイト・
   幅16px (半角2桁) を食うので、バイト数は桁数の 1.5 倍を見ておく。
   途中で切ると UTF-8 が壊れるため、panel_line 側でも文字境界で切る。 */
#define PANEL_COLS   120

void panel_init(KernelAPI *kapi);

/* 行バッファをクリアして書き込みを始める */
void panel_begin(void);

/* 1行追加。あふれた行は捨てられる */
void panel_line(const char *text);

/* 強調行 (黄) を追加 */
void panel_line_hi(const char *text);

/* 前フレームと内容が違うときだけパネルを描く。
   force=1 で無条件描画 (シーン切り替え直後など下地が消えた時に使う)。
   戻り値: 1=描いた, 0=スキップ */
int panel_end(int force);

/* ---- 観測用アクセサ (view_export が状態メールボックスへ書き出すのに使う) */
int         panel_get_count(void);
const char *panel_get_line(int idx);   /* 範囲外は "" */
u8          panel_get_attr(int idx);   /* 範囲外は 0 */

#endif /* VIEW_PANEL_H */
