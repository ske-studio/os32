/* ======================================================================== */
/*  VIEW_BATTLE.H — 戦闘画面の描画および入力処理                             */
/* ======================================================================== */

#ifndef VIEW_BATTLE_H
#define VIEW_BATTLE_H

#include "os32api.h"
#include "libos32gfx.h"
#include "libos32battle.h"

/* 戦闘画面の初期化 */
void view_battle_init(KernelAPI *kapi);

/* 戦闘の開始 (プレイヤー vs モンスター)
   monster_id は battle.db enemies.id (1〜44=野生, 101〜108=ボス) */
void view_battle_start(int player_id, u16 monster_id);

/* 戦闘画面の更新 (アニメーションタイマー処理やステート更新) */
/* 戻り値: 1=戦闘継続中, 0=戦闘終了 */
int view_battle_update(void);

/* 戦闘画面の描画。前フレームから見た目が変わらなければ何もしない */
void view_battle_draw(void);

/* シーン切り替えで下地が消えた直後に呼び、次の draw を強制する */
void view_battle_force_redraw(void);

/* 下段固定パネル (view_panel) の中身を組み立てる。
   進行と CPU の自動選択は view_battle_update() が行う */
void view_battle_panel(void);

/* コマンド入力待ちかどうか */
int view_battle_is_input_wait(void);

/* 戦闘中のキー入力 (1=攻撃, 2=防御, 3=逃走)。
   キーはメインループが1か所で読むので、戦闘側では読まずに渡してもらう */
void view_battle_handle_key(int ch);

#endif /* VIEW_BATTLE_H */
