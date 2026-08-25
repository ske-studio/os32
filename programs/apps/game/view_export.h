/* ======================================================================== */
/*  VIEW_EXPORT.H — 自動プレイ観測用の状態メールボックス                     */
/*                                                                          */
/*  ゲームの現在状態を毎フレーム物理 0x90000 (空き領域 0x8C000-0x9EFFF 内)   */
/*  へ書き出す。ホスト側 (tools/autoplay/driver.py) はエミュレータの         */
/*  GET /api/mem?addr=0x90000&space=phys でこれを読み、ローカルAIに          */
/*  次の操作を選ばせる。OS32 は全識別マッピングかつ ring0 なので、           */
/*  ユーザプログラムから低位物理メモリへ直接書ける。                         */
/*                                                                          */
/*  注意: V86 セッションはコンベンショナルメモリ全体をゲストに渡すため、     */
/*  この領域は V86 実行中に壊れる。ゲームは V86 を使わないので実害はない。   */
/* ======================================================================== */

#ifndef VIEW_EXPORT_H
#define VIEW_EXPORT_H

#include "os32api.h"

#define EXPORT_MAILBOX_ADDR  0x90000UL
#define EXPORT_MAGIC         0x31545347UL   /* 'GST1' (リトルエンディアン) */
#define EXPORT_VERSION       1

/* 毎フレーム1回呼ぶ。game_state / dice は main.c の static なので引数で渡す */
void view_export_tick(int game_state, int dice_value);

#endif /* VIEW_EXPORT_H */
