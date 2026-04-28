/* ======================================================================== */
/*  LIBOS32BOARD.H — OS32 ノードグラフ型ボードゲームエンジン 公開ヘッダ      */
/*                                                                          */
/*  スゴロク・ヘックスマップ等のノードグラフ型ボードの状態管理               */
/*  (マスデータ・接続・移動・分岐・区画) を提供する汎用ライブラリ。          */
/*                                                                          */
/*  依存: libos32db  (SQLiteアクセス)                                       */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32BOARD_H
#define LIBOS32BOARD_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 定数                                                                */
/* ====================================================================== */

/* 1マスの最大接続数 (ヘックス6方向+予備2) */
#define BOARD_MAX_CONNECT   8

/* 同時管理マス上限 */
#define BOARD_MAX_MASSES    256

/* 接続先なしのセンチネル値 */
#define BOARD_CONNECT_NONE  0xFFFF

/* 区画上限 */
#define BOARD_MAX_AREAS     8

/* マスフラグ */
#define BOARD_FLAG_NONE     0x00
#define BOARD_FLAG_BLOCKED  0x01  /* 封鎖 (通行不可) */
#define BOARD_FLAG_ONEWAY   0x02  /* 一方通行 (connect[0]方向のみ) */
#define BOARD_FLAG_HIDDEN   0x04  /* 非表示 (天の岩戸等) */
#define BOARD_FLAG_TRAP     0x08  /* 罠あり */

/* BFS用内部キュー上限 */
#define BOARD_BFS_QUEUE_MAX 256

/* ====================================================================== */
/*  2. データ構造                                                          */
/* ====================================================================== */

/* ボードマス */
typedef struct {
    u16  id;
    u8   type;                             /* マス種別 */
    u8   connect_count;                    /* 実際の接続数 */
    u16  connect[BOARD_MAX_CONNECT];       /* 接続先ID */
    u8   area;                             /* 所属区画 */
    u8   flags;                            /* BOARD_FLAG_* */
    u16  param;                            /* マス固有パラメータ */
    u8   cost;                             /* 通行コスト (0=無料) */
    u8   trap_owner;                       /* 罠設置者ID (0xFF=なし) */
    i16  x, y;                             /* 表示座標 */
} BoardMass;                               /* 26B */

/* 区画 (ステージ) */
typedef struct {
    u8   id;
    u8   unlocked;          /* 0=ロック, 1=解放済み */
    u8   unlock_type;       /* 0=初期解放, 1=ボス撃破, 2=イベント */
    u8   unlock_param;      /* ボスID / イベントID */
} BoardArea;

/* ====================================================================== */
/*  3. API — システム管理 (board_core.c)                                   */
/* ====================================================================== */

/* 初期化: DBからマス・接続・区画をRAMにキャッシュ
 * db_path: board.db のパス (NULL=DBなし, 手動設定のみ)
 * 戻り値: 0=成功, 負値=エラー
 */
int  board_init(const char *db_path);

/* 終了: DB接続クローズ, 内部状態リセット */
void board_shutdown(void);

/* ランタイム状態のみリセット (フラグ・罠等。マスデータ・接続は保持) */
void board_reset(void);

/* ====================================================================== */
/*  4. API — マス情報取得 (board_query.c)                                  */
/* ====================================================================== */

/* 現在のマス数を返す */
int  board_mass_count(void);

/* IDからマスを取得 (NULL=見つからない) */
const BoardMass *board_get_mass(u16 id);

/* マス種別を取得 (0=見つからない) */
u8   board_get_type(u16 id);

/* 分岐マスかどうか (connect_count >= 2) */
int  board_has_branch(u16 id);

/* 接続先IDを取得 (戻り値: 実際に取得した数) */
int  board_get_connections(u16 id, u16 *out, int max);

/* 特定種別のマスを検索 (戻り値: 見つかったマス数) */
int  board_find_by_type(u8 type, u16 *out, int max);

/* 同マス判定: positions配列の中でmass_idにいるインデックスを返す
 * 戻り値: 見つかった数
 */
int  board_check_colocated(u16 mass_id, const u16 *positions,
                            int count, u8 *out_indices, int max);

/* ====================================================================== */
/*  5. API — フラグ操作 (board_query.c)                                    */
/* ====================================================================== */

void board_set_flag(u16 mass_id, u8 flag);
void board_clear_flag(u16 mass_id, u8 flag);
int  board_has_flag(u16 mass_id, u8 flag);

/* 罠管理 */
void board_set_trap(u16 mass_id, u8 owner_id);
void board_clear_trap(u16 mass_id);
u8   board_get_trap_owner(u16 mass_id);

/* ====================================================================== */
/*  6. API — 移動シミュレーション (board_move.c)                           */
/* ====================================================================== */

/* fromからsteps歩進んだ先のマスIDを返す (分岐なしの直線移動)
 * 分岐に到達した場合は分岐マスIDを返し、stepsが残っていることを示す
 * *remaining に未消化ステップ数を格納
 */
u16  board_walk(u16 from, int steps, int *remaining);

/* N手先のマス列を取得 (指定方向dir — connect[dir] を辿る)
 * 分岐が発生したら停止
 * 戻り値: 実際に取得したマス数
 */
int  board_peek_path(u16 from, u8 dir, u16 *out, int max);

/* 2マス間の最短距離 (-1=到達不能) — BFS実装 */
int  board_distance(u16 from, u16 to);

/* 2マス間のコスト付き最短距離 (-1=到達不能) — Dijkstra法
 * 各マスの cost フィールドを考慮する (cost=0 は通行コスト1として扱う)
 */
int  board_distance_cost(u16 from, u16 to);

/* コスト付き最短経路を取得 (-1=到達不能) — Dijkstra法
 * from → to の最短経路をマスID列として out に格納する
 * (from自身は含まず、toは含む)
 * 戻り値: 経路のマス数 (-1=到達不能)
 */
int  board_find_path(u16 from, u16 to, u16 *out, int max);

/* ====================================================================== */
/*  7. API — 区画管理 (board_area.c)                                       */
/* ====================================================================== */

int  board_is_area_unlocked(u8 area_id);
int  board_unlock_area(u8 area_id);
void board_lock_area(u8 area_id);
int  board_area_count(void);

/* ====================================================================== */
/*  8. API — 動的マス操作                                                  */
/* ====================================================================== */

/* ランタイムでマスを追加 (イベントで一時マス生成等)
 * 戻り値: 割り当てられたマスID (-1=上限超過)
 */
int  board_add_mass(const BoardMass *mass);

/* 接続の動的変更 */
int  board_add_connection(u16 from, u16 to);
void board_remove_connection(u16 from, u16 to);

#endif /* LIBOS32BOARD_H */
