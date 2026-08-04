/* ======================================================================== */
/*  LIBOS32RPG.H — OS32 キャラクター育成・状態・リボーン 公開ヘッダ          */
/*                                                                          */
/*  永続アクターのステータス管理、経験値計算、レベルアップ成長、            */
/*  フィールド上での状態異常進行、死亡および復活処理を提供するライブラリ。  */
/* ======================================================================== */

#ifndef LIBOS32RPG_H
#define LIBOS32RPG_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i16, i32 */
#include "libos32battle.h"      /* BtlUnit, BTL_STAT_* */

/* 永続アクター構造体 */
typedef struct {
    u8   level;          /* 1〜99 */
    u8   class_id;       /* 職業/氏神ID (成長テーブル選択用) */
    u16  _pad0;          /* アライメント調整用 */
    u32  exp;            /* 累計経験値 */
    i16  atk, def, spd, mag; /* 基礎パラメータ */
    i16  hp, max_hp;     /* パラメータHP */
    u32  status;         /* 状態異常ビット (libos32battleと同形式) */
    u8   dead_turns;     /* 0=生存, >0=死亡から経過したターン数 */
    u8   fled;           /* 1=戦闘から逃走して死亡 (その場リボーン分岐用) */
    u8   pending_points; /* レベルアップ未配分の自由ポイント */
    u8   _pad1;          /* アライメント調整用 */
} RpgActor;              /* 28B */

/* レベルアップ結果 */
typedef struct {
    u8   levels_gained;  /* 今回上昇したレベル数 */
    u8   free_points;    /* 得られた未配分ポイント */
    u16  _pad;
} RpgLevelResult;

/* 状態異常Tickログ */
typedef struct {
    i16  tick_damage;    /* このターンで受けた状態異常ダメージ合計 */
    u32  cleared;        /* 自然回復したstatusのビットマスク */
    u8   action_blocked; /* 1=行動不能 (眠り/麻痺など), 0=行動可能 */
    u8   _pad[3];
} RpgTickLog;

/* パラメータ自動配分ポリシー関数ポインタ */
typedef void (*rpg_alloc_policy_fn)(RpgActor *a, u8 points);

/* ====================================================================== */
/*  API — システム管理 (rpg_core.c)                                       */
/* ====================================================================== */

/* ライブラリ初期化
 * db_path: rpg.db のパス (NULL=DBなし, 手動設定のみ)
 * 戻り値: 0=成功, 負値=エラー
 */
int  rpg_init(const char *db_path);

/* 終了 */
void rpg_shutdown(void);

/* アクターの初期化 (class_idに基づき成長表の初期パラメータ・HPをロード) */
void rpg_actor_init(RpgActor *a, u8 class_id);

/* ====================================================================== */
/*  API — 経験値・レベル (rpg_level.c)                                     */
/* ====================================================================== */

u32  rpg_exp_for_level(u8 level);                /* レベルに対する累計必要EXP */
u8   rpg_level_for_exp(u32 exp);                 /* 累計EXPに対するレベルを算出 */
u32  rpg_exp_to_next(const RpgActor *a);         /* 次のレベルに必要な残りEXP */
int  rpg_add_exp(RpgActor *a, u32 amount, RpgLevelResult *out); /* 経験値追加・レベルアップ進行 */

/* パラメータ配分 */
int  rpg_alloc_point(RpgActor *a, u8 stat);      /* 指定ステータス(BTL_STAT_*)を1上昇し、pending_pointsを1減らす */
void rpg_set_alloc_policy(rpg_alloc_policy_fn fn); /* CPU自動配分ポリシーの登録 */
void rpg_auto_alloc(RpgActor *a);                /* 未配分ポイントをポリシーに従い自動配分 */

/* ====================================================================== */
/*  API — フィールド状態異常 (rpg_status.c)                                 */
/* ====================================================================== */

void rpg_status_apply(RpgActor *a, u32 bit);
void rpg_status_clear(RpgActor *a, u32 bit);
int  rpg_has_status(const RpgActor *a, u32 bit);
int  rpg_status_tick(RpgActor *a, RpgTickLog *out); /* 毎ターン進行: ダメージ適用・確率回復。戻り値=行動不能フラグ */

/* ====================================================================== */
/*  API — 死亡・リボーン (rpg_reborn.c)                                     */
/* ====================================================================== */

void rpg_set_dead(RpgActor *a, u8 fled);           /* アクターを死亡状態に変更 */
int  rpg_is_dead(const RpgActor *a);               /* 死亡しているか */
int  rpg_reborn_check(RpgActor *a, int rank, int num_players); /* 復活判定。戻り値: 1=復活, 0=継続 */

/* ====================================================================== */
/*  API — クエリ (rpg_query.c)                                            */
/* ====================================================================== */

int  rpg_total_power(const RpgActor *a);         /* パラメータの単純合計値 */
int  rpg_rank(const u32 *scores, int n, int idx); /* 総資産などのスコア配列から指定要素の順位を取得 (1〜N) */

/* libos32battle との連携 */
void rpg_to_btl_unit(const RpgActor *a, BtlUnit *u);
void rpg_from_btl_unit(RpgActor *a, const BtlUnit *u);

#endif /* LIBOS32RPG_H */
