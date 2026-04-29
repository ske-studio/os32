/* ======================================================================== */
/*  INV_CORE.C — インベントリエンジン コア実装                                */
/*                                                                          */
/*  初期化・終了・DBからのマスタデータキャッシュを担当。                       */
/*  DB操作は起動/終了時のみ行い、ランタイムではRAMキャッシュのみを参照する。  */
/* ======================================================================== */

#include "libos32inv.h"
#include "libos32db.h"
#include "libos32db_util.h"

extern KernelAPI *kapi;
#define api kapi

/* memset は libc から提供 */
extern void *memset(void *, int, unsigned int);

/* ====================================================================== */
/*  グローバル変数                                                         */
/* ====================================================================== */

static InvItemDef  g_masters[INV_MASTER_MAX];
static int         g_master_count;
static InvRecipe   g_recipes[INV_RECIPE_MAX];
static int         g_recipe_count;
static InvSetBonus g_set_bonuses[INV_SET_BONUS_MAX];
static int         g_set_bonus_count;
static db_handle_t g_inv_db = -1;

/* 装備ポリシーコールバック */
static inv_equip_check_fn g_equip_policy;

/* ====================================================================== */
/*  内部参照用アクセサ (他モジュールから使用)                               */
/* ====================================================================== */

InvItemDef *inv__get_masters(void)    { return g_masters; }
int         inv__master_count(void)   { return g_master_count; }
InvRecipe  *inv__get_recipes(void)    { return g_recipes; }
int         inv__recipe_count(void)   { return g_recipe_count; }
InvSetBonus *inv__get_set_bonuses(void) { return g_set_bonuses; }
int         inv__set_bonus_count(void){ return g_set_bonus_count; }
db_handle_t inv__get_db(void)         { return g_inv_db; }

inv_equip_check_fn inv__get_equip_policy(void) { return g_equip_policy; }

/* ====================================================================== */
/*  DB読み込み: アイテムマスタ                                             */
/* ====================================================================== */

static int load_items(void)
{
    int rc;
    int count = 0;

    rc = db_query(g_inv_db,
        "SELECT id, type, effect, param, rarity, equip_slot, "
        "stackable, stat_bonus, stat_type, stage, price, name, "
        "COALESCE(set_id,0), COALESCE(max_durability,255) "
        "FROM items ORDER BY id");
    if (rc < 0) return -1;

    while (rc == DB_STATUS_ROW && count < INV_MASTER_MAX) {
        const char *nm;
        int i;

        g_masters[count].id             = (u16)db_column_int(0);
        g_masters[count].type           = (u8)db_column_int(1);
        g_masters[count].effect         = (u8)db_column_int(2);
        g_masters[count].param          = (u8)db_column_int(3);
        g_masters[count].rarity         = (u8)db_column_int(4);
        g_masters[count].equip_slot     = (u8)db_column_int(5);
        g_masters[count].stackable      = (u8)db_column_int(6);
        g_masters[count].stat_bonus     = (i16)db_column_int(7);
        g_masters[count].stat_type      = (u8)db_column_int(8);
        g_masters[count].stage          = (u8)db_column_int(9);
        g_masters[count].price          = (u32)db_column_int(10);
        g_masters[count].set_id         = (u8)db_column_int(12);
        g_masters[count].max_durability = (u8)db_column_int(13);

        /* 名前コピー (最傇23文字 + NUL) */
        nm = db_column_text(11);
        for (i = 0; i < 23 && nm && nm[i]; i++) {
            g_masters[count].name[i] = nm[i];
        }
        g_masters[count].name[i] = '\0';

        count++;
        rc = db_step(g_inv_db);
    }

    g_master_count = count;
    return count;
}

/* ====================================================================== */
/*  DB読み込み: レシピ                                                      */
/* ====================================================================== */

static int load_recipes(void)
{
    return DB_LOAD_TABLE_OPT(g_inv_db,
        "SELECT id, result_id, mat_a, mat_b, "
        "COALESCE(mat_a_count,1), COALESCE(mat_b_count,1), "
        "COALESCE(result_count,1) "
        "FROM recipes ORDER BY id",
        g_recipes, INV_RECIPE_MAX, g_recipe_count,
        {
            row->id           = (u16)db_column_int(0);
            row->result_id    = (u16)db_column_int(1);
            row->mat_a        = (u16)db_column_int(2);
            row->mat_b        = (u16)db_column_int(3);
            row->mat_a_count  = (u8)db_column_int(4);
            row->mat_b_count  = (u8)db_column_int(5);
            row->result_count = (u8)db_column_int(6);
            row->_pad         = 0;
        });
}

/* ====================================================================== */
/*  DB読み込み: セットボーナス                                                */
/* ====================================================================== */

static int load_set_bonuses(void)
{
    return DB_LOAD_TABLE_OPT(g_inv_db,
        "SELECT set_id, piece_count, stat_type, bonus "
        "FROM set_bonus ORDER BY set_id, piece_count",
        g_set_bonuses, INV_SET_BONUS_MAX, g_set_bonus_count,
        {
            row->set_id      = (u8)db_column_int(0);
            row->piece_count = (u8)db_column_int(1);
            row->stat_type   = (u8)db_column_int(2);
            row->_pad        = 0;
            row->bonus       = (i16)db_column_int(3);
            row->_pad2       = 0;
        });
}

/* ====================================================================== */
/*  公開API: システム管理                                                   */
/* ====================================================================== */

int inv_init(const char *db_path)
{
    int rc;

    /* 既に初期化済みの場合はシャットダウンしてから再初期化 */
    if (g_inv_db >= 0) {
        inv_shutdown();
    }

    /* 内部状態クリア */
    memset(g_masters, 0, sizeof(g_masters));
    memset(g_recipes, 0, sizeof(g_recipes));
    memset(g_set_bonuses, 0, sizeof(g_set_bonuses));
    g_master_count = 0;
    g_recipe_count = 0;
    g_set_bonus_count = 0;
    g_equip_policy = (inv_equip_check_fn)0;

    /* DB接続 */
    g_inv_db = db_open(db_path);
    if (g_inv_db < 0) return -1;

    /* マスタデータ読み込み */
    rc = load_items();
    if (rc < 0) { db_close(g_inv_db); g_inv_db = -1; return -2; }

    /* レシピ読み込み (テーブルがなくてもエラーではない) */
    load_recipes();

    /* セットボーナス読み込み (テーブルがなくてもエラーではない) */
    load_set_bonuses();

    return 0;
}

void inv_shutdown(void)
{
    if (g_inv_db >= 0) {
        db_close(g_inv_db);
        g_inv_db = -1;
    }
    g_master_count = 0;
    g_recipe_count = 0;
    g_set_bonus_count = 0;
    g_equip_policy = (inv_equip_check_fn)0;
}

/* ====================================================================== */
/*  公開API: マスターデータ参照                                             */
/* ====================================================================== */

const InvItemDef *inv_get_def(u16 item_id)
{
    int i = DB_FIND_BY_FIELD(g_masters, g_master_count, id, item_id);
    if (i < 0) return (const InvItemDef *)0;
    return &g_masters[i];
}

int inv_master_count(void)
{
    return g_master_count;
}

/* ====================================================================== */
/*  公開API: ポリシー設定                                                   */
/* ====================================================================== */

void inv_set_equip_policy(inv_equip_check_fn fn)
{
    g_equip_policy = fn;
}

/* ====================================================================== */
/*  公開API: アイテム効果クエリ                                             */
/* ====================================================================== */

u8 inv_get_effect(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? def->effect : 0;
}

u8 inv_get_param(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? def->param : 0;
}

int inv_is_consumable(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? (def->type == INV_TYPE_CONSUMABLE) : 0;
}

int inv_is_equipment(u16 item_id)
{
    const InvItemDef *def = inv_get_def(item_id);
    return def ? (def->type >= INV_TYPE_WEAPON) : 0;
}
