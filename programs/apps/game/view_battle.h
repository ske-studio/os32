/* ======================================================================== */
/*  VIEW_BATTLE.H — 戦闘画面の描画および入力処理                             */
/* ======================================================================== */

#ifndef VIEW_BATTLE_H
#define VIEW_BATTLE_H

#include "os32api.h"
#include "libos32gfx.h"
#include "libos32ui.h"
#include "libos32battle.h"

/* 戦闘画面の初期化 */
void view_battle_init(KernelAPI *kapi, mu_Context *mu_ctx);

/* 戦闘の開始 (プレイヤー vs モンスター) */
void view_battle_start(int player_id, u8 monster_id);

/* 戦闘画面の更新 (アニメーションタイマー処理やステート更新) */
/* 戻り値: 1=戦闘継続中, 0=戦闘終了 */
int view_battle_update(void);

/* 戦闘画面の描画 */
void view_battle_draw(void);

/* microUIの戦闘中UI構築 */
void view_battle_ui(void);

#endif /* VIEW_BATTLE_H */
