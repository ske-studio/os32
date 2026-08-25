/* ======================================================================== */
/*  LIBOS32BATTLE.H — OS32 ターンバトル解決エンジン 公開ヘッダ               */
/*                                                                          */
/*  ターンベースバトルの計算処理 (ダメージ計算・コマンド解決・                */
/*  状態異常・バフ/デバフ・属性相性) を提供する汎用ライブラリ。              */
/*                                                                          */
/*  依存: libos32math (rng_*, fix16_t)                                      */
/*        libos32db  (KAPI経由SQLite)                                       */
/*        libos32ai  (AI判断 — バトルコマンド選択委譲)                       */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32BATTLE_H
#define LIBOS32BATTLE_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 定数                                                                */
/* ====================================================================== */

/* コマンドマトリクス最大サイズ */
#define BTL_CMD_MAX         8

/* バフ/デバフ修飾子: 1ユニットあたりの上限 */
#define BTL_MOD_MAX         8

/* 属性相性ペア最大数 */
#define BTL_ELEM_PAIRS_MAX  32

/* 結果タイプ定数 */
#define BTL_RES_NORMAL      0   /* 通常攻撃 */
#define BTL_RES_GUARD       1   /* 防御 */
#define BTL_RES_NOGUARD     2   /* 防御無視 */
#define BTL_RES_COUNTER     3   /* カウンター */
#define BTL_RES_MISS        4   /* ミス/ためる */
#define BTL_RES_REFLECT     5   /* 反射 */
#define BTL_RES_VULN        6   /* 弱点 */
#define BTL_RES_YIELD       7   /* 逃走 */

/* 修飾対象ステータス */
#define BTL_STAT_ATK        0
#define BTL_STAT_DEF        1
#define BTL_STAT_SPD        2
#define BTL_STAT_MAG        3
#define BTL_STAT_COUNT      4   /* ステータス種別の数 */

/* 状態異常ビットフラグ (status_effects テーブルの bit_flag と対応) */
#define BTL_STATUS_POISON   (1u << 0)   /* 毒 */
#define BTL_STATUS_PARA     (1u << 1)   /* 麻痺 */
#define BTL_STATUS_SLEEP    (1u << 2)   /* 眠り */
#define BTL_STATUS_CONFUSE  (1u << 3)   /* 混乱 */
#define BTL_STATUS_BLIND    (1u << 4)   /* 盲目 */
/* ビット5-31: ゲーム側で自由に定義可能 */

/* 属性倍率の基準値 (等倍) */
#define BTL_ELEM_NEUTRAL    256

/* ====================================================================== */
/*  2. データ構造                                                          */
/* ====================================================================== */

/* バトルユニット */
typedef struct {
    i16  hp, max_hp;
    i16  atk, def, spd, mag;    /* i16: 負値(デバフ)・255超に対応 */
    u32  status;                /* 状態異常ビットフラグ (最大32種) */
    u32  elements;              /* 属性フラグ (libos32chemと同形式) */
    u8   tame_count;            /* ためるカウンタ */
    u8   class_id;              /* 職業/種族 (固有技分岐用) */
    u8   modifier_count;        /* アクティブ修飾子数 */
    u8   _pad;
} BtlUnit;                      /* 24B */

/* バフ/デバフ修飾子 */
typedef struct {
    u8   stat;        /* BTL_STAT_* */
    u8   turns;       /* 残りターン数 (0=永続) */
    i16  add_value;   /* 加算値 */
    i16  mul_pct;     /* 乗算% (100=等倍, 150=1.5倍, 50=半減) */
    u16  _pad;
} BtlModifier;        /* 8B */

/* バトル結果 */
typedef struct {
    i16  damage;          /* ダメージ量 (負=回復) */
    u8   result_type;     /* BTL_RES_* */
    u8   is_critical;     /* クリティカルフラグ */
    u8   is_dodge;        /* 回避フラグ */
    u8   _pad;
    u16  status_apply;    /* 付与された状態異常ビット */
    u16  status_clear;    /* 解除された状態異常ビット */
    i16  element_mul;     /* 属性倍率 (256=等倍, 512=2倍) */
} BtlResult;              /* 12B */

/* コマンドマトリクス (RAMキャッシュ) */
typedef struct {
    u8   atk_count;                         /* 攻撃コマンド種数 */
    u8   def_count;                         /* 防御コマンド種数 */
    u8   matrix[BTL_CMD_MAX][BTL_CMD_MAX];  /* [atk][def] -> BTL_RES_* */
} BtlCommandMatrix;     /* 66B */

/* 属性相性テーブル (RAMキャッシュ) */
typedef struct {
    u32  elem_atk;      /* 攻撃属性 */
    u32  elem_def;      /* 防御属性 */
    i16  multiplier;    /* 倍率 (256=等倍, 512=2倍, 128=半減) */
    u16  _pad;
} BtlElementEntry;      /* 12B */

/* 状態異常定義 (RAMキャッシュ) */
#define BTL_STATUS_DEF_MAX  16

typedef struct {
    u32  bit_flag;      /* u32のビットマスク */
    u8   duration;      /* 自然回復ターン (0=永続) */
    u8   prevents_action; /* 1=行動不能 */
    i16  tick_damage;   /* 毎ターンダメージ */
} BtlStatusDef;         /* 8B */

/* ====================================================================== */
/*  3. API — システム管理 (btl_core.c)                                     */
/* ====================================================================== */

/* 初期化: DBからマトリクス・属性相性・状態異常定義をRAMにキャッシュ
 * db_path: battle.db のパス (NULL=DBなし, 手動設定のみ)
 * 戻り値: 0=成功, 負値=エラー
 */
int  btl_init(const char *db_path);

/* 終了: DB接続クローズ, 内部状態リセット */
void btl_shutdown(void);

/* ====================================================================== */
/*  4. API — ダメージ計算 (btl_calc.c)                                     */
/* ====================================================================== */

/* 物理ダメージ: effective_atk*1.5 - def + 乱数+-3 */
int  btl_calc_damage(int atk, int def);

/* 魔法ダメージ: mag*2 - def + 乱数+-3 */
int  btl_calc_magic_damage(int mag, int def_mag);

/* 防御時の実効DEF: def * 1.25 (= def + def/4) */
int  btl_effective_def_guard(int def);

/* 回避率: 0-50% (def_spd > atk_spd 時に発生) */
int  btl_calc_dodge_rate(int atk_spd, int def_spd);

/* 逃走成功率: 20-80% (速度比に基づく線形マッピング) */
int  btl_calc_flee_rate(int runner_spd, int chaser_spd);

/* 属性相性倍率を取得 (256=等倍) */
i16  btl_element_multiplier(u32 atk_elem, u32 def_elem);

/* ポリシー差し替え: ダメージ計算関数ポインタ */
typedef int (*btl_damage_fn)(int atk, int def);
void btl_set_damage_policy(btl_damage_fn fn);
void btl_set_magic_policy(btl_damage_fn fn);

/* ====================================================================== */
/*  5. API — コマンド解決 (btl_resolve.c)                                  */
/* ====================================================================== */

/* コマンドマトリクスから結果を引く */
u8   btl_resolve_commands(u8 atk_cmd, u8 def_cmd);

/* 1ターン分の攻防を一括解決 */
BtlResult btl_resolve_turn(const BtlUnit *attacker, u8 atk_cmd,
                            const BtlUnit *defender, u8 def_cmd);

/* 先攻判定 (値が大きい方が先攻, 同値はランダム) */
int  btl_first_strike(int p1_value, int p2_value);

/* ====================================================================== */
/*  6. API — 状態異常・修飾子 (btl_status.c)                               */
/* ====================================================================== */

/* 状態異常 */
void btl_apply_status(BtlUnit *unit, u32 status_bit);
void btl_clear_status(BtlUnit *unit, u32 status_bit);
int  btl_has_status(const BtlUnit *unit, u32 status_bit);
int  btl_status_tick(BtlUnit *unit);  /* 毎ターン処理, 戻り値=行動不能 */

/* 修飾子 (バフ/デバフ) — 外部から BtlModifier 配列を管理 */
int  btl_add_modifier(BtlUnit *unit, BtlModifier *mods, const BtlModifier *mod);
void btl_tick_modifiers(BtlUnit *unit, BtlModifier *mods);
void btl_clear_modifiers(BtlUnit *unit, BtlModifier *mods);

/* 実効ステータス取得 (基礎値 + 全修飾子の合算) */
i16  btl_effective_stat(const BtlUnit *unit, const BtlModifier *mods, u8 stat);

/* コールバック */
typedef void (*btl_result_cb)(const BtlUnit *atk, const BtlUnit *def,
                               const BtlResult *result);
void btl_set_result_callback(btl_result_cb cb);

/* ====================================================================== */
/*  7. API — パーティバトル (btl_resolve.c)  [Phase 2]                     */
/* ====================================================================== */

/* 全体攻撃: 同じコマンドで複数防御者に適用
 *
 * attacker:  攻撃者
 * atk_cmd:   攻撃コマンド
 * defenders: 防御者配列 (const — HP変更は呼出側が行う)
 * def_cmds:  各防御者の防御コマンド配列
 * results:   結果出力先配列 (count個以上のサイズが必要)
 * count:     防御者数 (1以上)
 * 戻り値:    処理した防御者数
 */
int btl_resolve_multi(const BtlUnit *attacker, u8 atk_cmd,
                       const BtlUnit *defenders, const u8 *def_cmds,
                       BtlResult *results, int count);

/* ====================================================================== */
/*  8. API — 変身システム (btl_transform.c)  [Phase 2]                     */
/* ====================================================================== */

/* 変身定義 — ゲーム側がconst/DBで用意する */
typedef struct {
    i16  stat_mul_pct;   /* ステータス倍率% (200=2倍) */
    i16  hp_mul_pct;     /* HP倍率% (400=4倍) */
    u8   max_turns;      /* 最大持続ターン (0=無制限) */
    u8   release_rate;   /* 毎ターン解除確率% (0=確率解除なし) */
    i16  weapon_atk;     /* 専用装備ATK加算 */
    i16  shield_def;     /* 専用装備DEF加算 (盾) */
    i16  armor_def;      /* 専用装備DEF加算 (鎧) */
    u16  _pad;
} BtlTransformDef;       /* 16B */

/* 変身状態 — ユニットごとに呼出側が保持する */
typedef struct {
    i16  orig_atk;       /* 変身前のATK */
    i16  orig_def;       /* 変身前のDEF */
    i16  orig_spd;       /* 変身前のSPD */
    i16  orig_mag;       /* 変身前のMAG */
    i16  orig_max_hp;    /* 変身前のmax_hp */
    u8   turns_left;     /* 残りターン (0=無制限/変身中でない) */
    u8   release_rate;   /* 毎ターン解除確率% */
    u8   active;         /* 1=変身中, 0=通常 */
    u8   _pad;
} BtlTransformState;     /* 14B */

/* 変身開始: ユニットのステータスを変身定義に従って変更
 * 変身前の値を state に保存する。
 * 既に変身中の場合は -1 を返す。
 * 戻り値: 0=成功, -1=エラー
 */
int  btl_transform(BtlUnit *unit, BtlTransformState *state,
                    const BtlTransformDef *def);

/* 変身ターン経過: 残りターン数をデクリメントし解除判定
 * 戻り値: 1=変身解除された, 0=変身継続
 */
int  btl_transform_tick(BtlUnit *unit, BtlTransformState *state);

/* 変身強制解除: ステータスを変身前の値に復元 */
void btl_transform_release(BtlUnit *unit, BtlTransformState *state);

#endif /* LIBOS32BATTLE_H */
