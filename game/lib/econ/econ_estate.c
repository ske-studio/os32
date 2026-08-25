/* ======================================================================== */
/*  ECON_ESTATE.C — 不動産サブシステム実装                                   */
/*                                                                          */
/*  村統治・投資・上納金蓄積・資産計算を汎用化する。                          */
/*  収入計算式: income = base_value / 640 * income_mul[level] / 100          */
/*  640はPC-98横解像度に由来する歴史的定数。                                  */
/* ======================================================================== */

#include "econ_internal.h"

/* ====================================================================== */
/*  グローバル変数定義                                                     */
/* ====================================================================== */

EconEstate          g_estates[ECON_ESTATE_MAX];
int                 g_estate_count;
EconEstateLevelDef  g_estate_levels[ECON_ESTATE_LV_MAX];
int                 g_estate_level_count;
EconEstateTypeDef   g_estate_types[ECON_ESTATE_TYPE_MAX];
int                 g_estate_type_count;

/* ====================================================================== */
/*  内部ヘルパー                                                           */
/* ====================================================================== */

int econ__find_estate(u16 estate_id)
{
    int i;
    for (i = 0; i < g_estate_count; i++) {
        if (g_estates[i].id == estate_id) return i;
    }
    return -1;
}

/* レベルテーブルからエントリを取得 (見つからなければデフォルト値) */
static const EconEstateLevelDef *get_level_def(u8 level)
{
    int i;
    for (i = 0; i < g_estate_level_count; i++) {
        if (g_estate_levels[i].level == level)
            return &g_estate_levels[i];
    }
    /* デフォルト: 等倍 */
    return (const EconEstateLevelDef *)0;
}

/* 価値の再計算 */
static void recalc_value(EconEstate *e)
{
    const EconEstateLevelDef *ld = get_level_def(e->level);
    if (ld) {
        e->value = e->base_value * (u32)ld->value_mul / 100;
    } else {
        e->value = e->base_value;
    }
}

/* ====================================================================== */
/*  DB読み込み: 不動産マスタ                                               */
/* ====================================================================== */

static int load_estates(void)
{
    int rc;
    int count = 0;

    rc = db_query(g_econ_db,
        "SELECT id, type, stage, base_value "
        "FROM estates ORDER BY id");
    if (rc < 0) return 0; /* テーブルがなくてもエラーにしない */

    while (rc == DB_STATUS_ROW && count < ECON_ESTATE_MAX) {
        g_estates[count].id         = (u16)db_column_int(0);
        g_estates[count].type       = (u8)db_column_int(1);
        g_estates[count].stage      = (u8)db_column_int(2);
        g_estates[count].base_value = (u32)db_column_int(3);
        g_estates[count].owner      = ECON_OWNER_NONE;
        g_estates[count].level      = 1;
        g_estates[count].flags      = 0;
        g_estates[count].tax        = 0;
        g_estates[count].monster_id = 0;
        g_estates[count].co_owner   = ECON_OWNER_NONE;
        g_estates[count].share_pct  = ECON_SHARE_FULL;
        g_estates[count]._pad2      = 0;
        /* 初期価値 = base_value (レベル1) */
        g_estates[count].value      = g_estates[count].base_value;
        count++;
        rc = db_step(g_econ_db);
    }

    g_estate_count = count;
    return count;
}

/* ====================================================================== */
/*  DB読み込み: レベルテーブル                                             */
/* ====================================================================== */

static int load_estate_levels(void)
{
    int rc;
    int count = 0;

    rc = db_query(g_econ_db,
        "SELECT level, income_mul, value_mul, invest_div "
        "FROM estate_levels ORDER BY level");
    if (rc < 0) return 0; /* テーブルがなくてもエラーにしない */

    while (rc == DB_STATUS_ROW && count < ECON_ESTATE_LV_MAX) {
        g_estate_levels[count].level          = (u8)db_column_int(0);
        g_estate_levels[count].income_mul     = (u8)db_column_int(1);
        g_estate_levels[count].value_mul      = (u8)db_column_int(2);
        g_estate_levels[count].invest_cost_div = (u32)db_column_int(3);
        g_estate_levels[count]._pad           = 0;
        count++;
        rc = db_step(g_econ_db);
    }

    g_estate_level_count = count;

    /* レベルテーブル適用で各不動産の価値を再計算 */
    {
        int i;
        for (i = 0; i < g_estate_count; i++) {
            recalc_value(&g_estates[i]);
        }
    }

    return count;
}

/* ====================================================================== */
/*  DB読み込み: 種別定義テーブル                                           */
/* ====================================================================== */

static int load_estate_types(void)
{
    int rc;
    int count = 0;

    rc = db_query(g_econ_db,
        "SELECT type, bonus_pct, bonus_mode, bonus_per_route "
        "FROM estate_types ORDER BY type");
    if (rc < 0) return 0;

    while (rc == DB_STATUS_ROW && count < ECON_ESTATE_TYPE_MAX) {
        g_estate_types[count].type           = (u8)db_column_int(0);
        g_estate_types[count].bonus_pct      = (u8)db_column_int(1);
        g_estate_types[count].bonus_mode     = (u8)db_column_int(2);
        g_estate_types[count].bonus_per_route = (u8)db_column_int(3);
        count++;
        rc = db_step(g_econ_db);
    }

    g_estate_type_count = count;
    return count;
}

/* ====================================================================== */
/*  内部ヘルパー: 交易ルート数カウント                                     */
/* ====================================================================== */

int econ__count_routes_for_stage(u8 stage)
{
    int i;
    int count = 0;
    for (i = 0; i < g_route_count; i++) {
        if (g_routes[i].from_id == stage ||
            g_routes[i].to_id == stage) {
            /* 封鎖中のルートは除外 */
            if (!(g_routes[i].flags & 0x01))
                count++;
        }
    }
    return count;
}

/* ====================================================================== */
/*  公開API: 初期化                                                        */
/* ====================================================================== */

int econ_estate_init(void)
{
    memset(g_estates, 0, sizeof(g_estates));
    memset(g_estate_levels, 0, sizeof(g_estate_levels));
    memset(g_estate_types, 0, sizeof(g_estate_types));
    g_estate_count = 0;
    g_estate_level_count = 0;
    g_estate_type_count = 0;

    if (g_econ_db < 0) return -1;

    load_estates();
    load_estate_levels();
    load_estate_types();

    return g_estate_count;
}

/* ====================================================================== */
/*  公開API: 統治操作                                                      */
/* ====================================================================== */

int econ_estate_claim(u16 id, u8 owner)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return -1;
    g_estates[idx].owner = owner;
    return 0;
}

void econ_estate_release(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].owner = ECON_OWNER_NONE;
    g_estates[idx].tax = 0;
}

void econ_estate_set_monster(u16 id, u8 monster_id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].monster_id = monster_id;
}

void econ_estate_clear_monster(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].monster_id = 0;
}

/* ====================================================================== */
/*  公開API: 投資                                                          */
/* ====================================================================== */

u32 econ_estate_invest_cost(u16 id)
{
    int idx;
    const EconEstateLevelDef *ld;

    idx = econ__find_estate(id);
    if (idx < 0) return 0;

    ld = get_level_def((u8)(g_estates[idx].level + 1));
    if (!ld) {
        /* 次レベルのテーブルがない → base_value をそのまま使用 */
        return g_estates[idx].base_value;
    }
    if (ld->invest_cost_div == 0) return g_estates[idx].base_value;
    return g_estates[idx].base_value / ld->invest_cost_div;
}

int econ_estate_invest(u16 id, u32 *wallet)
{
    int idx;
    u32 cost;

    idx = econ__find_estate(id);
    if (idx < 0) return -1;

    /* 最大レベルチェック */
    if (g_estates[idx].level >= ECON_ESTATE_LV_MAX) return -1;

    cost = econ_estate_invest_cost(id);
    if (*wallet < cost) return -2;

    *wallet -= cost;
    g_estates[idx].level++;
    recalc_value(&g_estates[idx]);
    return 0;
}

void econ_estate_damage(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    if (g_estates[idx].level > 1) {
        g_estates[idx].level--;
        recalc_value(&g_estates[idx]);
    }
}

/* ====================================================================== */
/*  公開API: 収入管理                                                      */
/* ====================================================================== */

u32 econ_estate_income(u16 id)
{
    int idx;
    const EconEstateLevelDef *ld;
    u32 base_income;
    u32 leveled;
    u32 bonus;
    u8 mul;

    idx = econ__find_estate(id);
    if (idx < 0) return 0;

    /* 無主/モンスター支配/封鎖 → 収入なし */
    if (g_estates[idx].owner == ECON_OWNER_NONE) return 0;
    if (g_estates[idx].monster_id != 0) return 0;
    if (g_estates[idx].flags & ESTATE_FLAG_BLOCKED) return 0;

    base_income = g_estates[idx].base_value / 640;
    if (base_income == 0) base_income = 1;  /* 最低1 */

    ld = get_level_def(g_estates[idx].level);
    mul = ld ? ld->income_mul : 100;
    leveled = base_income * (u32)mul / 100;

    /* 種別ボーナス適用 */
    bonus = econ_estate_type_bonus(id);
    if (bonus > 0) {
        leveled = leveled * (100 + bonus) / 100;
    }

    return leveled;
}

void econ_estate_accumulate(void)
{
    int i;
    for (i = 0; i < g_estate_count; i++) {
        u32 income;
        EconEstate *e = &g_estates[i];

        /* スキップ条件 */
        if (e->owner == ECON_OWNER_NONE) continue;
        if (e->monster_id != 0) continue;
        if (e->flags & ESTATE_FLAG_BLOCKED) continue;

        income = econ_estate_income(e->id);
        e->tax += income;
    }
}

u32 econ_estate_collect(u8 owner)
{
    int i;
    u32 total = 0;
    for (i = 0; i < g_estate_count; i++) {
        if (g_estates[i].owner == owner) {
            /* 共同統治時: share_pct 分のみ主オーナーが取得 */
            u32 share = g_estates[i].tax * (u32)g_estates[i].share_pct / 100;
            u32 remainder = g_estates[i].tax - share;
            total += share;
            /* co_owner 分は保持 (別途 collect_co で回収) */
            g_estates[i].tax = remainder;
        }
    }
    return total;
}

u32 econ_estate_collect_one(u16 id)
{
    int idx;
    u32 t;
    u32 share;
    idx = econ__find_estate(id);
    if (idx < 0) return 0;
    /* 共同統治時: share_pct 分のみ */
    share = g_estates[idx].tax * (u32)g_estates[idx].share_pct / 100;
    t = g_estates[idx].tax - share;
    g_estates[idx].tax = t;  /* co_owner分は残す */
    return share;
}

/* ====================================================================== */
/*  公開API: 資産計算                                                      */
/* ====================================================================== */

u32 econ_estate_total_value(u8 owner)
{
    int i;
    u32 total = 0;
    for (i = 0; i < g_estate_count; i++) {
        if (g_estates[i].owner == owner) {
            total += g_estates[i].value;
        }
    }
    return total;
}

int econ_estate_count(u8 owner)
{
    int i;
    int count = 0;
    for (i = 0; i < g_estate_count; i++) {
        if (g_estates[i].owner == owner) count++;
    }
    return count;
}

int econ_estate_list(u8 owner, u16 *out, int max)
{
    int i;
    int n = 0;
    for (i = 0; i < g_estate_count && n < max; i++) {
        if (g_estates[i].owner == owner) {
            out[n++] = g_estates[i].id;
        }
    }
    return n;
}

/* ====================================================================== */
/*  公開API: クエリ                                                        */
/* ====================================================================== */

const EconEstate *econ_estate_get(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return (const EconEstate *)0;
    return &g_estates[idx];
}

const EconEstate *econ_estate_at(int index)
{
    if (index < 0 || index >= g_estate_count) return (const EconEstate *)0;
    return &g_estates[index];
}

int econ_estate_total(void)
{
    return g_estate_count;
}

u8 econ_estate_level(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return 0;
    return g_estates[idx].level;
}

u32 econ_estate_value(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return 0;
    return g_estates[idx].value;
}

void econ_estate_set_flag(u16 id, u8 flag)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].flags |= flag;
}

void econ_estate_clear_flag(u16 id, u8 flag)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].flags &= ~flag;
}

int econ_estate_has_flag(u16 id, u8 flag)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return 0;
    return (g_estates[idx].flags & flag) ? 1 : 0;
}

/* ====================================================================== */
/*  公開API: 種別ボーナス                                                    */
/* ====================================================================== */

const EconEstateTypeDef *econ_estate_type_def(u8 type)
{
    int i;
    for (i = 0; i < g_estate_type_count; i++) {
        if (g_estate_types[i].type == type)
            return &g_estate_types[i];
    }
    return (const EconEstateTypeDef *)0;
}

u32 econ_estate_type_bonus(u16 id)
{
    int idx;
    const EconEstateTypeDef *td;
    u32 bonus;

    idx = econ__find_estate(id);
    if (idx < 0) return 0;

    td = econ_estate_type_def(g_estates[idx].type);
    if (!td) return 0;

    bonus = (u32)td->bonus_pct;

    /* モード別追加ボーナス */
    switch (td->bonus_mode) {
    case ESTATE_BONUS_TRADE: {
        /* 港: 交易ルート接続数 * bonus_per_route */
        int routes = econ__count_routes_for_stage(g_estates[idx].stage);
        bonus += (u32)routes * (u32)td->bonus_per_route;
        break;
    }
    case ESTATE_BONUS_MINERAL:
        /* 鉱山: 固定ボーナスのみ (bonus_pctに含まれる) */
        break;
    default:
        break;
    }

    return bonus;
}

/* ====================================================================== */
/*  公開API: 共同統治                                                      */
/* ====================================================================== */

int econ_estate_set_co_owner(u16 id, u8 co_owner, u8 share_pct)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return -1;
    /* share_pct は主オーナーの取り分 (1-99) */
    if (share_pct == 0 || share_pct >= 100) return -2;
    if (g_estates[idx].owner == ECON_OWNER_NONE) return -3;
    g_estates[idx].co_owner = co_owner;
    g_estates[idx].share_pct = share_pct;
    return 0;
}

void econ_estate_clear_co_owner(u16 id)
{
    int idx = econ__find_estate(id);
    if (idx < 0) return;
    g_estates[idx].co_owner = ECON_OWNER_NONE;
    g_estates[idx].share_pct = ECON_SHARE_FULL;
}

u32 econ_estate_collect_co(u8 co_owner)
{
    int i;
    u32 total = 0;
    for (i = 0; i < g_estate_count; i++) {
        if (g_estates[i].co_owner == co_owner && g_estates[i].tax > 0) {
            /* tax に残っているのは co_owner 分 */
            total += g_estates[i].tax;
            g_estates[i].tax = 0;
        }
    }
    return total;
}
