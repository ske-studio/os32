/* ======================================================================== */
/*  VIEW_BOARD.H — 盤面描画・移動アニメーション インターフェース               */
/* ======================================================================== */

#ifndef VIEW_BOARD_H
#define VIEW_BOARD_H

#include "os32api.h"

/* プレイヤー描画情報 */
typedef struct {
    int pos;          /* 現在のマスID */
    int target_pos;   /* 移動目標のマスID */
    int anim_x, anim_y; /* アニメーション中の画面座標 */
    int moving;       /* 移動中フラグ */
    int path[32];     /* 移動経路のマスIDリスト */
    int path_len;     /* 経路長 */
    int path_idx;     /* 現在の経路インデックス */
    int anim_timer;   /* アニメーション用タイマー */
} BoardPlayer;

/* マス種別マクロ */
#define MASS_EMPTY       0
#define MASS_VILLAGE     1
#define MASS_BATTLE      2
#define MASS_TREASURE    3
#define MASS_EQUIP_SHOP  4
#define MASS_ITEM_SHOP   5
#define MASS_MAGIC_SHOP  6
#define MASS_CHURCH      7
#define MASS_CIRCLE      8
#define MASS_EVENT       9
#define MASS_GATE        10
#define MASS_CASTLE      11
#define MASS_MAGIC_CHEST 12

/* 盤面描画モジュールの初期化 */
void view_board_init(KernelAPI *kapi);

/* ボードDBのロード */
int view_board_load(const char *db_path);

/* 盤面の描画更新 */
void view_board_draw(void);

/* 移動アニメーションなどの更新 */
void view_board_update(void);

/* プレイヤーを指定されたサイコロの目だけ進める (移動アニメーション開始) */
void view_board_move_player(int pid, int steps);

/* プレイヤーが現在移動中かどうか */
int view_board_is_player_moving(int pid);

/* 指定マスの画面上座標を取得 */
void view_board_get_mass_pos(int mass_id, int *x, int *y);

/* プレイヤーの現在位置を取得 */
int view_board_get_player_pos(int pid);

/* プレイヤーの現在位置を設定（ワープや初期化用） */
void view_board_set_player_pos(int pid, int mass_id);

#endif /* VIEW_BOARD_H */
