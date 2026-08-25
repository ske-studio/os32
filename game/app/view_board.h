/* ======================================================================== */
/*  VIEW_BOARD.H — 盤面描画・移動アニメーション インターフェース               */
/* ======================================================================== */

#ifndef VIEW_BOARD_H
#define VIEW_BOARD_H

#include "os32api.h"

/* 分岐の選択肢の最大数 */
#define VIEW_BOARD_MAX_BRANCH 4

/* スクロールビューのレイアウト
   地形は libos32tilemap の BG0 (16x16 タイル) に敷く。
   グリッド1マス = 3x3 タイル = 48x48px。
   盤面 400x304 = 25x19 タイル -> 8x6 マス分が視界に入る。
   道になっているセルにだけ矢印が出るので、実際に見えるマスは10〜20。 */
#define VIEW_FIELD_W   400
#define VIEW_FIELD_H   304
#define VIEW_CELL_W    48
#define VIEW_CELL_H    48
#define VIEW_ORIGIN_X  0
#define VIEW_ORIGIN_Y  0

/* プレイヤー描画情報 */
typedef struct {
    int pos;          /* 現在のマスID */
    int target_pos;   /* 移動目標のマスID */
    int anim_x, anim_y; /* アニメーション中の世界座標 (1マス=48px) */
    int moving;       /* 移動中フラグ */
    int path[32];     /* 移動経路のマスIDリスト */
    int path_len;     /* 経路長 */
    int path_idx;     /* 現在の経路インデックス */
    int anim_timer;   /* アニメーション用タイマー */

    int rem_steps;    /* 未消化の残り歩数 (分岐で停止した時に残る) */
    int prev_mass;    /* 直前にいたマス (来た道の除外用。-1=なし) */
    int branching;    /* 1=分岐待ち (選択されるまで移動を再開しない) */
    int branch_opts[VIEW_BOARD_MAX_BRANCH]; /* 選べる進行先マスID */
    int branch_count; /* 選択肢の数 */
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
#define MASS_COLLECT     13   /* 集金所: 統治村から選んで集金/投資 */
#define MASS_DUNGEON     14   /* ダンジョン入口 (未実装) */

/* 盤面描画モジュールの初期化 */
void view_board_init(KernelAPI *kapi);

/* ボードDBのロード */
int view_board_load(const char *db_path);

/* 盤面の描画更新。
   前フレームから見た目が変わらないときは何もせず 0 を返す。
   描画したら 1 を返す (呼び出し側は上に重ねる UI の再描画に使う)。 */
int view_board_draw(void);

/* カメラ・駒以外の理由 (村の所有権など) で盤面の見た目が変わったとき、
   次の view_board_draw() に再描画を強制する */
void view_board_invalidate(void);

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

/* 分岐選択待ちかどうか。1=選択待ち
   (移動が止まっていて、かつ残り歩数がある状態) */
int view_board_is_branching(int pid);

/* 分岐の選択肢を取得する。戻り値=選択肢の数、out にマスIDを格納 */
int view_board_branch_options(int pid, int *out, int max);

/* 分岐先を選んで移動を再開する。
   next_mass は view_board_branch_options が返した値のいずれか。
   戻り値: 0=成功, -1=分岐待ちでない/選択肢外 */
int view_board_choose_branch(int pid, int next_mass);

/* 残り歩数 (分岐待ち中に「あと何マス進めるか」を表示する用) */
int view_board_remaining_steps(int pid);

/* カメラ (3x3 ビューの中心マス)。駒が動いたら追従させる */
void view_board_set_focus(int mass_id);
int  view_board_get_focus(void);
/* 手番プレイヤー。頭上に▼を出し、重なったとき最前面に描く */
void view_board_set_focus_player(int pid);

/* 直前に組み立てた移動経路。path[0]=出発マス, 末尾=到達マス。
   通過したマスの地形効果 (毒の沼など) を適用するために使う。
   分岐で区切られるので、1手番の移動が複数区間に分かれることがある。 */
int view_board_path_len(int pid);
int view_board_path_at(int pid, int idx);

#endif /* VIEW_BOARD_H */
