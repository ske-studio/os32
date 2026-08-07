/* ======================================================================== */
/*  LIBOS32INV.H — OS32 インベントリ・装備・ショップエンジン 公開ヘッダ       */
/*                                                                          */
/*  アイテム所持・装備スロット・ショップ売買・抽選を汎用管理する。             */
/*  マスターデータはSQLiteからロードし、ランタイムはRAM配列で管理する。       */
/*                                                                          */
/*  依存: libos32db (KAPI経由SQLite)                                        */
/*  描画 (gfx) には依存しない。                                              */
/* ======================================================================== */

#ifndef LIBOS32INV_H
#define LIBOS32INV_H

#include "os32_kapi_shared.h"    /* u8, u16, u32, i8, i16, i32 */

/* ====================================================================== */
/*  1. 配列上限定数                                                        */
/* ====================================================================== */

#define INV_MAX_SLOTS       16   /* 所持枠物理上限 */
#define INV_EQUIP_SLOTS      8   /* 装備スロット種別上限 */
#define INV_MASTER_MAX     128   /* アイテムマスター上限 */
#define INV_RECIPE_MAX      64   /* 合成レシピ上限 */
#define INV_SET_BONUS_MAX   32   /* セットボーナス定義上限 */
#define INV_SHOP_LINEUP_MAX 128  /* ショップ品揃え定義上限 */
#define INV_LOTTERY_MAX     128  /* 抽選テーブル定義上限 */

/* ====================================================================== */
/*  2. 装備スロット種別                                                    */
/* ====================================================================== */

#define INV_ESLOT_WEAPON     0
#define INV_ESLOT_SHIELD     1
#define INV_ESLOT_ARMOR      2
#define INV_ESLOT_ACCESSORY  3
#define INV_ESLOT_HEAD       4
#define INV_ESLOT_BOOTS      5
#define INV_ESLOT_GLOVES     6
#define INV_ESLOT_RING       7

/* ====================================================================== */
/*  3. アイテム種別                                                        */
/* ====================================================================== */

#define INV_TYPE_CONSUMABLE  0   /* 消耗品 */
#define INV_TYPE_WEAPON      1   /* 武器 */
#define INV_TYPE_SHIELD      2   /* 盾 */
#define INV_TYPE_ARMOR       3   /* 鎧 */
#define INV_TYPE_ACCESSORY   4   /* アクセサリ */

/* ====================================================================== */
/*  4. ステータスボーナス種別                                              */
/* ====================================================================== */

#define INV_STAT_ATK         0
#define INV_STAT_DEF         1
#define INV_STAT_SPD         2
#define INV_STAT_MAG         3

/* ====================================================================== */
/*  5. データ構造体                                                        */
/* ====================================================================== */

/* インベントリスロット */
typedef struct {
    u16  item_id;         /* 0=空き */
    u8   count;           /* スタック数 (1=非スタック, 最大99) */
    u8   durability;      /* 耐久度 (0xFF=無限, 0=破損) */
} InvSlot;

/* インベントリバッグ (ゲーム側が管理) */
typedef struct {
    InvSlot slots[INV_MAX_SLOTS];
    InvSlot equip[INV_EQUIP_SLOTS];
    u8      max_slots;    /* 実際の所持上限 (ゲームが設定) */
    u8      equip_count;  /* 使用する装備スロット数 */
    u16     _pad;
} InvBag;

/* アイテムマスターデータ (RAMキャッシュ) */
typedef struct {
    u16  id;
    u8   type;            /* INV_TYPE_* */
    u8   effect;          /* 効果ID (ゲーム側で解釈) */
    u8   param;           /* 効果パラメータ */
    u8   rarity;          /* レアリティ (抽選の重み計算用) */
    u8   equip_slot;      /* 装備先スロット (INV_ESLOT_*) */
    u8   stackable;       /* 1=スタック可能 */
    i16  stat_bonus;      /* ステータスボーナス値 */
    u8   stat_type;       /* ボーナス対象 (INV_STAT_*) */
    u8   stage;           /* 登場ステージ (ショップ品揃え用) */
    u8   set_id;          /* セット装備ID (0=セットなし) */
    u8   max_durability;  /* 最大耐久度 (0=耐久度なし, 0xFF=無限) */
    u32  price;           /* 購入価格 (売値=price/2) */
    char name[24];        /* アイテム名 (UTF-8) */
} InvItemDef;             /* 44B × 128 = ~5.5KB */

/* 合成レシピ (RAMキャッシュ) */
typedef struct {
    u16  id;              /* レシピID */
    u16  result_id;       /* 完成品アイテムID */
    u16  mat_a;           /* 素材A アイテムID */
    u16  mat_b;           /* 素材B アイテムID */
    u8   mat_a_count;     /* 素材A 必要数 */
    u8   mat_b_count;     /* 素材B 必要数 */
    u8   result_count;    /* 完成品個数 */
    u8   _pad;
} InvRecipe;              /* 12B × 64 = 768B */

/* セット装備ボーナス (RAMキャッシュ) */
typedef struct {
    u8   set_id;          /* セットID */
    u8   piece_count;     /* 必要装備数 */
    u8   stat_type;       /* ボーナス対象 (INV_STAT_*) */
    u8   _pad;
    i16  bonus;           /* ボーナス値 */
    u16  _pad2;
} InvSetBonus;            /* 8B × 32 = 256B */

/* ショップ品揃えキャッシュ用構造体 */
typedef struct {
    u8   shop_type;       /* ショップ種類 (0=装備屋, 1=道具屋など) */
    u8   stage;           /* 登場ステージ */
    u16  item_id;         /* アイテムID */
} InvShopLineup;          /* 4B × 128 = 512B */

/* 抽選テーブルキャッシュ用構造体 */
typedef struct {
    u8   table_type;      /* 抽選テーブル種類 (0=宝箱, 1=敵ドロップなど) */
    u8   min_stage;       /* 最低ステージ */
    u16  item_id;         /* アイテムID */
    u16  weight;          /* 抽選重み */
} InvLotteryEntry;        /* 6B × 128 = 768B */

/* ====================================================================== */
/*  6. ポリシーコールバック型                                              */
/* ====================================================================== */

/* 装備可否チェック */
typedef int (*inv_equip_check_fn)(u16 equip_id, u8 class_id, u8 level);

/* ====================================================================== */
/*  7. API — システム管理 (inv_core.c)                                     */
/* ====================================================================== */

int  inv_init(const char *db_path);
void inv_shutdown(void);

/* マスターデータ参照 */
const InvItemDef *inv_get_def(u16 item_id);
int  inv_master_count(void);

/* キャッシュ参照アクセサ */
const InvShopLineup *inv__get_shop_lineups(void);
int inv__shop_lineup_count(void);
const InvLotteryEntry *inv__get_lotteries(void);
int inv__lottery_count(void);

/* ====================================================================== */
/*  8. API — インベントリ操作 (inv_bag.c)                                  */
/* ====================================================================== */

/* バッグ初期化 */
void inv_bag_init(InvBag *bag, u8 max_slots, u8 equip_count);

/* 追加 (スタック可能アイテムは自動合算) */
int  inv_add(InvBag *bag, u16 item_id, u8 count);   /* 0=成功, -1=満杯 */

/* 指定スロットから除去 */
int  inv_remove(InvBag *bag, u8 slot, u8 count);

/* 所持数 (同一アイテムの合計) */
int  inv_count_item(const InvBag *bag, u16 item_id);

/* 空きスロット数 */
int  inv_free_slots(const InvBag *bag);

/* 所持判定 */
int  inv_has(const InvBag *bag, u16 item_id);

/* スロット内容取得 */
const InvSlot *inv_get_slot(const InvBag *bag, u8 slot);

/* ====================================================================== */
/*  9. API — 装備操作 (inv_equip.c)                                        */
/* ====================================================================== */

/* 装備変更
 * 旧装備は自動的にインベントリに戻す (満杯なら失敗)
 * 戻り値: 0=成功, -1=装備不可, -2=インベントリ満杯
 */
int  inv_equip(InvBag *bag, u16 equip_id, u8 slot_type);

/* 装備解除 */
int  inv_unequip(InvBag *bag, u8 slot_type);

/* 装備ボーナス合計 (全装備スロットの stat_bonus を合算) */
i16  inv_equip_bonus(const InvBag *bag, u8 stat_type);

/* 装備可否チェックポリシー設定 */
void inv_set_equip_policy(inv_equip_check_fn fn);

/* ====================================================================== */
/*  10. API — ショップ (inv_shop.c)                                        */
/* ====================================================================== */

/* ステージ別品揃え取得
 * shop_type: 0=装備屋, 1=道具屋, 2=言霊屋
 * stage: 現在ステージ
 * 戻り値: 取得した品数
 */
int  inv_shop_list(u8 shop_type, u8 stage,
                    u16 *out_ids, int max);

/* 価格取得 */
u32  inv_get_price(u16 item_id);
u32  inv_get_sell_price(u16 item_id);   /* 売値 = price / 2 */

/* 購入 (wallet から減算, バッグに追加) */
int  inv_shop_buy(InvBag *bag, u16 item_id, u32 *wallet);

/* 売却 (バッグから除去, wallet に加算) */
int  inv_shop_sell(InvBag *bag, u8 slot, u32 *wallet);

/* ====================================================================== */
/*  11. API — 抽選 (inv_lottery.c)                                         */
/* ====================================================================== */

/* 抽選テーブルからランダムにアイテムIDを取得
 * table_type: 0=宝箱, 1=敵ドロップ, 2=イベント報酬
 * stage: 現在ステージ (対象外のアイテムを除外)
 */
u16  inv_lottery(u8 table_type, u8 stage);

/* ====================================================================== */
/*  12. API — アイテム効果クエリ                                           */
/* ====================================================================== */

/* アイテムの効果ID/パラメータを取得 (ゲーム側でswitch分岐) */
u8   inv_get_effect(u16 item_id);
u8   inv_get_param(u16 item_id);
int  inv_is_consumable(u16 item_id);   /* 消耗品か */
int  inv_is_equipment(u16 item_id);    /* 装備品か */

/* ====================================================================== */
/*  13. API — 合成/クラフト (inv_craft.c)                                  */
/* ====================================================================== */

/* レシピの合成可否チェック (素材が揃っているか) */
int  inv_can_craft(const InvBag *bag, u16 recipe_id);

/* 合成実行 (素材消費 → 完成品追加)
 * 戻り値: 0=成功, -1=素材不足, -2=インベントリ満杯, -3=レシピ不明
 */
int  inv_craft(InvBag *bag, u16 recipe_id);

/* レシピ参照 */
const InvRecipe *inv_get_recipe(u16 recipe_id);
int  inv_recipe_count(void);

/* ====================================================================== */
/*  14. API — セット装備ボーナス (inv_setbonus.c)                          */
/* ====================================================================== */

/* 現在の装備で発動しているセットボーナスの合計値を取得
 * 複数セットの同一stat_typeボーナスは加算される
 */
i16  inv_set_bonus(const InvBag *bag, u8 stat_type);

/* 装備ボーナス + セットボーナスの統合値 */
i16  inv_total_bonus(const InvBag *bag, u8 stat_type);

/* ====================================================================== */
/*  15. API — 耐久度 (inv_durability.c)                                    */
/* ====================================================================== */

/* 装備の耐久度を1減らす (0xFF=無限なら変化なし)
 * 戻り値: 残り耐久度 (0=破損, -1=エラー)
 */
int  inv_wear(InvBag *bag, u8 equip_slot);

/* 装備の耐久度を回復 (鍛冶屋等)
 * 戻り値: 回復後の耐久度, -1=エラー
 */
int  inv_repair(InvBag *bag, u8 equip_slot);

/* 装備が破損しているか (durability==0) */
int  inv_is_broken(const InvBag *bag, u8 equip_slot);

#endif /* LIBOS32INV_H */
