/* ======================================================================== */
/*  GAME_SOUND.H — BGM と効果音                                              */
/*                                                                          */
/*  libos32snd (KAPI の FM/SSG エンジン) の薄い上乗せ。                      */
/*  場面ごとの BGM 切り替えと、ゲーム固有の効果音をまとめる。                 */
/*                                                                          */
/*  BGM は「今どの曲を鳴らすべきか」を毎フレーム宣言する形にしてある          */
/*  (game_sound_scene)。同じ曲なら何もしないので、状態遷移のたびに           */
/*  停止/再生を書き分けなくてよい。                                          */
/* ======================================================================== */

#ifndef GAME_SOUND_H
#define GAME_SOUND_H

#include "os32api.h"

/* BGM の場面 */
#define GS_BGM_NONE     0
#define GS_BGM_TITLE    1
#define GS_BGM_FIELD    2
#define GS_BGM_BATTLE   3
#define GS_BGM_BOSS     4
#define GS_BGM_SHOP     5
#define GS_BGM_DUNGEON  6
#define GS_BGM_RESULT   7

/* ゲーム固有の効果音 */
#define GS_SE_DICE      0   /* サイコロ */
#define GS_SE_MOVE      1   /* 1マス進む */
#define GS_SE_HIT       2   /* 攻撃が当たる */
#define GS_SE_DAMAGE    3   /* 被弾 */
#define GS_SE_WIN       4   /* 戦闘勝利 */
#define GS_SE_LOSE      5   /* 戦闘敗北 */
#define GS_SE_COIN      6   /* 金銭の取得 */
#define GS_SE_ITEM      7   /* アイテム取得 */
#define GS_SE_SELECT    8   /* 決定 */
#define GS_SE_CANCEL    9   /* 取り消し */
#define GS_SE_KOTODAMA 10   /* 言霊の発動 */

void game_sound_init(KernelAPI *api);

/* いま鳴らすべき BGM を宣言する。前回と同じなら何もしない */
void game_sound_scene(int scene);

/* 効果音 */
void game_sound_se(int se);

/* 音の ON/OFF を切り替える。戻り値=切り替え後の状態 (1=ON) */
int  game_sound_toggle(void);
int  game_sound_enabled(void);

void game_sound_shutdown(void);

#endif /* GAME_SOUND_H */
