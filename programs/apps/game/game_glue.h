/* ======================================================================== */
/*  GAME_GLUE.H — ゲームエンジン連携（グルー）インターフェース                */
/* ======================================================================== */

#ifndef GAME_GLUE_H
#define GAME_GLUE_H

#include "os32api.h"
#include "libos32battle.h"
#include "libos32econ.h"
#include "libos32inv.h"
#include "libos32ai.h"
#include "libos32board.h"

/* 定数定義 */
#define GLUE_MAX_PLAYERS 4
#define GLUE_MAX_ITEMS 10
#define GLUE_MAX_FMAGIC 8

/* プレイヤー構造体 (C89) */
typedef struct {
    char name[32];
    u8 ujigami;
    u8 level;
    u32 exp;
    
    u32 hp;
    u32 max_hp;
    u8 atk;
    u8 def;
    u8 spd;
    u8 mag;
    
    u32 gold;
    u8 pos;            /* 盤面上の位置 (マスID) */
    u32 status;        /* 状態異常フラグ */
    
    InvBag bag;        /* libos32inv のインベントリバッグ */
    u8 is_cpu;
    
    u8 is_devil;       /* 1=デビルマン変身中, 0=通常 */
    u16 devil_turns;   /* 残り変身ターン数 */
    u8 devil_cooldown; /* 再変身可能になるまでのクールダウン週数 */
} GluePlayer;

/* グローバル変数の宣言 */
extern GluePlayer glue_players[GLUE_MAX_PLAYERS];
extern u8 glue_num_players;
extern u8 glue_current_player;
extern u16 glue_week;
extern u16 glue_turn;

/* グルーインターフェース関数 */
int glue_init(KernelAPI *kapi);
void glue_shutdown(void);

/* プレイヤーの実効ステータス（装備品補正込み） */
int glue_player_effective_atk(int pid);
int glue_player_effective_def(int pid);

/* バトル開始処理: プレイヤー vs モンスター */
int glue_battle_start_pve(int pid, u8 monster_id, BtlUnit *player_unit, BtlUnit *enemy_unit);
/* バトルターン解決: コマンドを入力して1ターン処理 */
int glue_battle_resolve_turn(BtlUnit *player_unit, u8 player_cmd, BtlUnit *enemy_unit, u8 enemy_cmd, BtlResult *res_p, BtlResult *res_e);

/* 村経済処理 */
int glue_village_claim(u8 mass_id, int pid);
int glue_village_invest(u8 mass_id, int pid);
u32 glue_village_collect_taxes(int pid);
u32 glue_player_total_assets(int pid);

/* デビルマン変身システム関連 */
int glue_devil_check(int pid);
void glue_devil_transform(int pid);
void glue_devil_release(int pid);
int glue_devil_update(int pid, char *out_msg);

#endif /* GAME_GLUE_H */
