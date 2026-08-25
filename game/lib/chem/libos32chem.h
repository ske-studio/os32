/* ======================================================================== */
/*  LIBOS32CHEM.H — OS32 化学エンジンライブラリ 公開ヘッダ                  */
/*                                                                          */
/*  BotW型「Chemistry Engine」の OS32 実装。                                */
/*  オブジェクトに属性タグと物理パラメータを持たせ、                          */
/*  SQLiteルール辞書 + C89整数演算で創発的ゲームプレイを実現する。            */
/*                                                                          */
/*  依存: libos32math (整数演算), libos32db (KAPI経由DB)                    */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32CHEM_H
#define LIBOS32CHEM_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 属性 (Element) — ビットフラグ方式 (u32: 最大32種)                   */
/* ====================================================================== */

#define ELEM_NONE       0x00000000
#define ELEM_FIRE       0x00000001
#define ELEM_WATER      0x00000002
#define ELEM_WOOD       0x00000004
#define ELEM_ICE        0x00000008
#define ELEM_ELECTRIC   0x00000010
#define ELEM_STEAM      0x00000020
#define ELEM_GRASS      0x00000040
#define ELEM_METAL      0x00000080
#define ELEM_STONE      0x00000100
#define ELEM_WIND       0x00000200
/* 以下ゲームごとに拡張可能: bit 10~31 */

/* ====================================================================== */
/*  2. アクション種別 / ターゲット / 状態定数                               */
/* ====================================================================== */

/* アクション種別 (ChemReaction.action) */
#define CHEM_ACT_NONE        0
#define CHEM_ACT_IGNITE      1   /* 着火 */
#define CHEM_ACT_EXTINGUISH  2   /* 消火 */
#define CHEM_ACT_FREEZE      3   /* 凍結 */
#define CHEM_ACT_MELT        4   /* 融解 */
#define CHEM_ACT_EVAPORATE   5   /* 蒸発 */
#define CHEM_ACT_ELECTRIFY   6   /* 帯電 */
#define CHEM_ACT_SPREAD      7   /* 周囲に伝播 */
#define CHEM_ACT_DAMAGE      8   /* ダメージ */
#define CHEM_ACT_SPAWN       9   /* オブジェクト生成 */
#define CHEM_ACT_DESTROY     10  /* 消滅 */

/* ターゲット (ChemReaction.target) */
#define CHEM_TGT_A      0   /* elem_a側 */
#define CHEM_TGT_B      1   /* elem_b側 */
#define CHEM_TGT_BOTH   2   /* 両方 */
#define CHEM_TGT_AREA   3   /* 周囲全体 */

/* オブジェクト状態 (ChemObject.state) */
#define CHEM_STATE_IDLE      0
#define CHEM_STATE_BURNING   1
#define CHEM_STATE_FROZEN    2
#define CHEM_STATE_WET       3
#define CHEM_STATE_CHARGED   4
#define CHEM_STATE_DISSOLVING 5

/* オブジェクトフラグ (ChemObject.flags) */
#define CHEM_FLAG_NONE       0x00

/* ====================================================================== */
/*  3. 配列上限                                                            */
/* ====================================================================== */

#define CHEM_MAX_OBJECTS   128    /* 同時管理オブジェクト上限 */
#define CHEM_MAX_REACTIONS  64    /* 反応ルール RAMキャッシュ上限 */
#define CHEM_MAX_PHASES     32    /* 状態遷移ルール RAMキャッシュ上限 */
#define CHEM_SPREAD_DEPTH    3    /* SPREAD再帰深度制限 */

/* ====================================================================== */
/*  4. データ構造体                                                        */
/* ====================================================================== */

/* ゲームオブジェクト */
typedef struct {
    u16  id;                /* ユニークID (0=未使用) */
    u16  type_id;           /* マスターデータ上の型ID */
    i16  x, y;              /* タイル座標 */
    u32  elements;          /* 現在の属性フラグ (複数持ち可) */
    i16  temperature;       /* 温度 (整数, 単位は任意) */
    i16  hp;                /* 耐久力 (0以下で消滅) */
    u8   state;             /* 状態 (CHEM_STATE_*) */
    u8   flags;             /* 各種フラグ */
    u8   timer;             /* 汎用タイマー (フレームカウント) */
    u8   _pad;
} ChemObject;

/* 反応ルール (RAMキャッシュ) */
typedef struct {
    u32  elem_a;        /* 接触元の属性マスク */
    u32  elem_b;        /* 接触先の属性マスク */
    u8   action;        /* CHEM_ACT_* */
    u8   target;        /* CHEM_TGT_* */
    u16  spawn_elem;    /* 新規生成される属性 */
    i16  temp_delta;    /* 温度変化量 */
    i16  hp_delta;      /* HP変化量 */
    u8   priority;      /* 優先度 (大きいほど優先) */
    u8   _pad;
} ChemReaction;

/* 温度ベース状態遷移ルール (RAMキャッシュ) */
typedef struct {
    u32  elem_from;     /* 元の属性 */
    i16  temp_min;      /* この温度範囲内で遷移発生 */
    i16  temp_max;
    u32  elem_to;       /* 変化先の属性 */
    u16  spawn_elem;    /* 副産物 */
    u16  _pad;
} ChemPhaseRule;

/* 反応コールバック (ゲーム側がSE/エフェクト用に使う) */
typedef void (*chem_reaction_callback)(int obj_a, int obj_b,
                                       u8 action, u32 spawn_elem);

/* ====================================================================== */
/*  5. API — システム管理                                                  */
/* ====================================================================== */

/* 初期化: DBを開いてルールをRAMにキャッシュ */
int  chem_init(const char *db_path);

/* 終了: DB接続クローズ、メモリ解放 */
void chem_shutdown(void);

/* ワールドリセット (全オブジェクト消去) */
void chem_reset(void);

/* ====================================================================== */
/*  6. API — オブジェクト管理                                              */
/* ====================================================================== */

/* オブジェクト生成 (type_id は object_types テーブルのID) */
/* 戻り値: オブジェクトID (0以上), 失敗時 -1 */
int  chem_spawn(u16 type_id, i16 x, i16 y);

/* オブジェクト消滅 */
void chem_destroy(int obj_id);

/* IDでオブジェクト取得 (読み取り用) */
const ChemObject *chem_get(int obj_id);

/* 属性の追加・除去 */
void chem_add_element(int obj_id, u32 elem);
void chem_remove_element(int obj_id, u32 elem);

/* 温度操作 */
void chem_set_temperature(int obj_id, i16 temp);
void chem_add_temperature(int obj_id, i16 delta);

/* ====================================================================== */
/*  7. API — シミュレーション                                              */
/* ====================================================================== */

/* 毎フレーム更新 — 温度更新 + 状態遷移 (ホットパス, SQL不使用) */
void chem_update(void);

/* 2オブジェクト間の反応をチェック・適用 */
/* 戻り値: 適用されたルール数 */
int  chem_react(int obj_a, int obj_b);

/* 指定座標に属性効果を与える (魔法攻撃、爆発等) */
/* 戻り値: 影響を受けたオブジェクト数 */
int  chem_apply_area(i16 x, i16 y, int radius,
                     u32 elements, i16 temp_delta);

/* ====================================================================== */
/*  8. API — クエリ                                                        */
/* ====================================================================== */

/* オブジェクトが特定属性を持つか */
int  chem_has_element(int obj_id, u32 elem);

/* 現在燃焼中のオブジェクト数 */
int  chem_count_burning(void);

/* 範囲内オブジェクト検索 */
int  chem_find_nearby(i16 x, i16 y, int radius,
                      int *out_ids, int max_count);

/* コールバック設定 */
void chem_set_callback(chem_reaction_callback cb);

/* ====================================================================== */
/*  9. 内部情報取得 (デバッグ用)                                           */
/* ====================================================================== */

/* キャッシュ済みルール数 */
int  chem_reaction_count(void);
int  chem_phase_count(void);

/* アクティブオブジェクト数 */
int  chem_active_count(void);

#endif /* LIBOS32CHEM_H */
