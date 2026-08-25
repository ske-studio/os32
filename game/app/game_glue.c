#include "game_glue.h"
#include "os32api.h"
#include "libos32math.h"
#include "libos32db.h"
#include "libos32save.h"
#include "view_board.h"   /* MASS_* (マス種別定数) */
#include <stdio.h>
#include <string.h>

GluePlayer glue_players[GLUE_MAX_PLAYERS];
u8 glue_num_players = 4;

TurnState glue_turnstate;
u8 glue_current_player = 0;
u16 glue_week = 1;
u16 glue_turn = 0;
u32 glue_last_income[GLUE_MAX_PLAYERS];

static KernelAPI *api;

/* 敵マスタ (battle.db enemies) の RAM キャッシュ */
static GlueEnemy g_enemies[GLUE_MAX_ENEMIES];
static int g_enemy_count = 0;

/* battle.db から敵マスタを読み込む。
   SQLite の MEMSYS5 プールは 200KB 固定なので、接続はこの関数の中で
   開いて閉じきる (開きっぱなしにすると後続の *_init が -2 で落ちる)。 */
static int load_enemies(const char *path)
{
    db_handle_t h;
    int rc;
    int count = 0;

    memset(g_enemies, 0, sizeof(g_enemies));
    g_enemy_count = 0;

    h = db_open(path);
    if (h < 0) return -1;

    rc = db_query(h,
        "SELECT id, name, stage, kind, max_hp, atk, def, spd, mag, "
        "elements, exp, gold, class_id FROM enemies ORDER BY id");
    if (rc < 0) {
        /* enemies テーブルがない古い battle.db でも致命傷にしない。
           ただし原因調査のため実エラーは出しておく */
        api->kprintf(0x06, "[GLUE] enemies query: %s (mem=%u)\n",
                     db_last_error(h), (unsigned)db_mem_used());
        db_close(h);
        return 0;
    }

    while (rc == DB_STATUS_ROW && count < GLUE_MAX_ENEMIES) {
        const char *name = db_column_text(1);
        GlueEnemy *e = &g_enemies[count];

        e->id       = (u16)db_column_int(0);
        if (name) {
            strncpy(e->name, name, GLUE_ENEMY_NAME_LEN - 1);
            e->name[GLUE_ENEMY_NAME_LEN - 1] = '\0';
        }
        e->stage    = (u8)db_column_int(2);
        e->kind     = (u8)db_column_int(3);
        e->max_hp   = (i16)db_column_int(4);
        e->atk      = (i16)db_column_int(5);
        e->def      = (i16)db_column_int(6);
        e->spd      = (i16)db_column_int(7);
        e->mag      = (i16)db_column_int(8);
        e->elements = (u32)db_column_int(9);
        e->exp      = (u32)db_column_int(10);
        e->gold     = (u32)db_column_int(11);
        e->class_id = (u8)db_column_int(12);

        count++;
        rc = db_step(h);
    }

    db_finalize(h);
    db_close(h);

    g_enemy_count = count;
    return count;
}

/* CPU 3人ぶんの性格プロファイル。
   プレイヤー0は人間なので使わないが、添字を揃えるため4つ持つ。 */
static AiProfile g_ai[GLUE_MAX_PLAYERS];

static void load_ai_profiles(void)
{
    int i;
    for (i = 0; i < GLUE_MAX_PLAYERS; i++) {
        /* 1=Aggressive, 2=Random, 3=Balanced を順に割り当てる */
        if (ai_load_profile((u8)(1 + (i % 3)), &g_ai[i]) != 0) {
            memset(&g_ai[i], 0, sizeof(g_ai[i]));
            g_ai[i].params[AI_P_MISS]  = 10;
            g_ai[i].params[AI_P_NOISE] = 5;
            g_ai[i].param_count = 5;
        }
    }
}

/* TurnState から公開ミラー変数へ反映する。
   glue_current_player / glue_week / glue_turn を直接書く場所を
   この関数だけに閉じ込め、手番の情報源を TurnState 一本にする。 */
static void sync_turn_mirror(void)
{
    glue_current_player = turn_current(&glue_turnstate);
    glue_week           = turn_round(&glue_turnstate);
    glue_turn           = turn_count(&glue_turnstate);
}

int glue_init(KernelAPI *kapi)
{
    int i;
    GluePlayer *p;
    int ret;
    api = kapi;

    /* エンジンライブラリの初期化 */
    ret = btl_init("/db/battle.db");
    if (ret != 0) {
        api->kprintf(0x04, "[GLUE] btl_init failed: %d\n", ret);
        return -1;
    }
    ret = econ_init("/db/econ.db");
    if (ret != 0) {
        api->kprintf(0x04, "[GLUE] econ_init failed: %d\n", ret);
        return -2;
    }
    ret = inv_init("/db/items.db");
    if (ret != 0) {
        api->kprintf(0x04, "[GLUE] inv_init failed: %d\n", ret);
        return -3;
    }

    ret = rpg_init("/db/rpg.db");
    if (ret != 0) {
        api->kprintf(0x04, "[GLUE] rpg_init failed: %d\n", ret);
        return -4;
    }

    /* CPU の性格。読めなくても既定プロファイルで動く */
    ret = ai_init("/db/ai.db");
    if (ret != 0) {
        api->kprintf(0x06, "[GLUE] ai_init failed (%d), using defaults\n", ret);
    }
    load_ai_profiles();

    /* 週次イベント。読めなくてもイベントなしで遊べる */
    ret = evt_init("/db/events.db");
    if (ret != 0) {
        api->kprintf(0x06, "[GLUE] evt_init failed (%d), no weekly events\n",
                     ret);
    }

    /* 敵マスタ。読めなくても式ベースのフォールバックで動くので致命傷にしない */
    ret = load_enemies("/db/battle.db");
    if (ret <= 0) {
        api->kprintf(0x06, "[GLUE] enemy master not loaded (%d)\n", ret);
    }

    /* 手番スケジューラ: 1ラウンド=7ターン(=1週)。
       GLUE_MAX_WEEKS 週で打ち切り → 資産王判定 (glue_victory_check) */
    turn_init(&glue_turnstate, glue_num_players, 7,
              (u16)(GLUE_MAX_WEEKS * 7));

    /* 勝利条件の状態を初期化 (再スタート時のため) */
    glue_victory_type = GLUE_WIN_NONE;
    glue_victory_winner = -1;
    glue_boss_progress = 1;
    sync_turn_mirror();
    glue_shop_apply_day();
    memset(glue_last_income, 0, sizeof(glue_last_income));

    /* プレイヤー初期化 */
    for (i = 0; i < GLUE_MAX_PLAYERS; i++) {
        p = &glue_players[i];

        /* 氏神ID = 成長テーブルの class_id。rpg.db から初期パラメータを引く */
        p->ujigami = (u8)i;
        rpg_actor_init(&p->actor, p->ujigami);

        p->gold = 1000;
        p->pos = 0;
        p->is_cpu = (i == 0) ? 0 : 1;
        p->is_devil = 0;
        p->devil_turns = 0;
        p->devil_cooldown = 0;

        switch (i) {
            case 0:  strcpy(p->name, "スサノオ");   break;
            case 1:  strcpy(p->name, "ヤマトタケル"); break;
            case 2:  strcpy(p->name, "オオクニヌシ"); break;
            default: strcpy(p->name, "アマテラス");  break;
        }

        /* インベントリ初期化: 最大10枠、装備スロットは GLUE_EQUIP_SLOTS */
        inv_bag_init(&p->bag, GLUE_MAX_ITEMS, GLUE_EQUIP_SLOTS);
        
        /* 初期装備の付与 */
        inv_add(&p->bag, 10, 1);
        inv_add(&p->bag, 20, 1);
        inv_add(&p->bag, 30, 1);

        /* 装備スロットに装備 */
        inv_equip(&p->bag, 10, INV_ESLOT_WEAPON);
        inv_equip(&p->bag, 20, INV_ESLOT_SHIELD);
        inv_equip(&p->bag, 30, INV_ESLOT_ARMOR);
    }

    return 0;
}

void glue_shutdown(void)
{
    btl_shutdown();
    econ_shutdown();
    inv_shutdown();
    rpg_shutdown();
    ai_shutdown();
    evt_shutdown();
}

/* ====================================================================== */
/*  手番・週進行                                                           */
/* ====================================================================== */

char glue_event_msg[GLUE_EVT_MSG_LEN];

int glue_event_monster_surge(void)
{
    return evt_is_active(GLUE_EVT_MONSTER_SURGE);
}

int glue_event_big_sale(void)
{
    return evt_is_active(GLUE_EVT_BIG_SALE);
}

/* 総資産が最下位のプレイヤー */
static int lowest_player(void)
{
    int i, worst = 0;
    u32 low = glue_player_total_assets(0);
    for (i = 1; i < (int)glue_num_players; i++) {
        u32 a = glue_player_total_assets(i);
        if (a < low) { low = a; worst = i; }
    }
    return worst;
}

/* イベント1件の効果を適用し、説明を glue_event_msg へ書く */
static void apply_event(u16 id)
{
    int i;

    switch (id) {
    case GLUE_EVT_HARVEST: {
        /* 豊作: 統治village数 x 120G が全員に入る */
        u32 total = 0;
        for (i = 0; i < (int)glue_num_players; i++) {
            u32 g = (u32)econ_estate_count((u8)i) * 120;
            glue_players[i].gold += g;
            total += g;
        }
        sprintf(glue_event_msg, "豊作！ 村から総額 %d G が納められた",
                (int)total);
        break;
    }

    case GLUE_EVT_TAX_LEVY:
        /* 徴税: 全員の所持金 10% を徴収 */
        for (i = 0; i < (int)glue_num_players; i++) {
            glue_players[i].gold -= glue_players[i].gold / 10;
        }
        strcpy(glue_event_msg, "徴税！ 全員の所持金が一割減った");
        break;

    case GLUE_EVT_BANDITS: {
        /* 山賊: 一番の金持ちが所持金の1/4を失う */
        int rich = 0;
        u32 high = glue_players[0].gold;
        u32 lost;
        for (i = 1; i < (int)glue_num_players; i++) {
            if (glue_players[i].gold > high) {
                high = glue_players[i].gold;
                rich = i;
            }
        }
        lost = glue_players[rich].gold / 4;
        glue_players[rich].gold -= lost;
        sprintf(glue_event_msg, "山賊！ %s が %d G を奪われた",
                glue_players[rich].name, (int)lost);
        break;
    }

    case GLUE_EVT_PLAGUE:
        /* 疫病: 全員が毒に */
        for (i = 0; i < (int)glue_num_players; i++) {
            rpg_status_apply(&glue_players[i].actor, GLUE_ST_POISON);
        }
        strcpy(glue_event_msg, "疫病！ 全員が毒に冒された");
        break;

    case GLUE_EVT_MONSTER_SURGE:
        strcpy(glue_event_msg, "魔物大量発生！ 敵が手強くなる");
        break;

    case GLUE_EVT_ORACLE: {
        /* 神託: 最下位が全回復し、状態異常も消える */
        int w = lowest_player();
        glue_players[w].actor.hp = glue_players[w].actor.max_hp;
        glue_players[w].actor.status = 0;
        sprintf(glue_event_msg, "神託！ %s の傷が癒えた",
                glue_players[w].name);
        break;
    }

    case GLUE_EVT_BIG_SALE:
        strcpy(glue_event_msg, "大売出し！ 店の品が半値になる");
        break;

    case GLUE_EVT_TREASURE_HUNT: {
        /* 宝探し: 全員がアイテムを1つ拾う */
        u16 item = inv_lottery(0, 1);
        const InvItemDef *d = inv_get_def(item);
        for (i = 0; i < (int)glue_num_players; i++) {
            inv_add(&glue_players[i].bag, item, 1);
        }
        sprintf(glue_event_msg, "宝探し！ 全員が %s を手にした",
                d ? d->name : "何か");
        break;
    }

    case GLUE_EVT_BOSS_HUNT:
        strcpy(glue_event_msg, "討伐指令！ 城の主を討てば賞金が出る");
        break;

    default:
        sprintf(glue_event_msg, "出来事 %d が起きた", (int)id);
        break;
    }
}

void glue_week_tick(void)
{
    int i;

    /* 統治村に上納金を蓄積 */
    econ_estate_accumulate();

    for (i = 0; i < (int)glue_num_players; i++) {
        /* 蓄積分を所持金へ回収。UI が「今週の収入」を出せるよう額を残す */
        glue_last_income[i] = glue_village_collect_taxes(i);

        /* デビル再変身クールダウンの減算 */
        if (glue_players[i].devil_cooldown > 0) {
            glue_players[i].devil_cooldown--;
        }
    }

}

/* イベント判定。毎ターン呼ぶ。
   libos32event の乱数発火は「rng%%256 < 無発生カウンタ」で、
   カウンタは tick ごとに +1 されるため、週1回の tick だと第6週でも
   2.3%% にしかならず実質発生しない。events.db の min_turn / cooldown /
   duration もターン単位で持たせてある。 */
static void event_tick(void)
{
    u16 fired[8];
    int nf, i;

    glue_event_msg[0] = '\0';
    if (evt_tick(glue_turn, (const void *)0) > 0) {
        nf = evt_get_fired(fired, 8);
        for (i = 0; i < nf; i++) {
            apply_event(fired[i]);   /* 複数出たら最後の説明が残る */
        }
    }

    /* 継続中のイベント (大売出し等) を店の価格へ反映する */
    glue_shop_apply_day();
}

int glue_turn_advance(int *out_crossed_week)
{
    TurnAdvance adv;
    int over;

    over = turn_advance(&glue_turnstate, &adv);
    sync_turn_mirror();
    glue_shop_apply_day();

    if (adv.crossed_round) {
        glue_week_tick();
    }

    /* イベント判定は毎ターン */
    event_tick();
    if (out_crossed_week) {
        *out_crossed_week = (int)adv.crossed_round;
    }

    return over;
}

int glue_player_effective_atk(int pid)
{
    GluePlayer *p = &glue_players[pid];
    /* デビルの強さも「イザナミ装備を着ている」ことで表現されるので、
       変身の有無で分岐しない (装備ボーナスの経路を一本化) */
    int bonus = inv_total_bonus(&p->bag, INV_STAT_ATK);
    return (int)p->actor.atk + bonus;
}

int glue_player_effective_def(int pid)
{
    GluePlayer *p = &glue_players[pid];
    int bonus = inv_total_bonus(&p->bag, INV_STAT_DEF);
    return (int)p->actor.def + bonus;
}

int glue_enemy_count(void)
{
    return g_enemy_count;
}

const GlueEnemy *glue_enemy_get(u16 enemy_id)
{
    int i;
    for (i = 0; i < g_enemy_count; i++) {
        if (g_enemies[i].id == enemy_id) return &g_enemies[i];
    }
    return (const GlueEnemy *)0;
}

const char *glue_enemy_name(u16 enemy_id)
{
    const GlueEnemy *e = glue_enemy_get(enemy_id);
    return e ? e->name : "魔物";
}

u16 glue_enemy_pick_wild(u8 stage)
{
    int i;
    int n = 0;
    u16 cand[GLUE_MAX_ENEMIES];

    for (i = 0; i < g_enemy_count; i++) {
        if (g_enemies[i].kind == 0 && g_enemies[i].stage == stage) {
            cand[n++] = g_enemies[i].id;
        }
    }
    if (n == 0) return 0;
    return cand[rng_range(0, n - 1)];
}

u16 glue_enemy_boss_of_stage(u8 stage)
{
    int i;
    for (i = 0; i < g_enemy_count; i++) {
        if (g_enemies[i].kind == 1 && g_enemies[i].stage == stage) {
            return g_enemies[i].id;
        }
    }
    return 0;
}

u16 glue_enemy_village_guard(u16 mass_id)
{
    const EconEstate *e = glue_village_estate(mass_id);
    u16 id;

    if (!e) return glue_enemy_pick_for_mass(mass_id);

    /* 村に守備モンスターが居座っていればそいつが相手 */
    if (e->monster_id != 0) return (u16)e->monster_id;

    /* 居なければ村主の私兵として、その村のステージの野生敵を出す */
    id = glue_enemy_pick_wild(e->stage);
    return id ? id : glue_enemy_pick_for_mass(mass_id);
}

u16 glue_enemy_pick_for_mass(u16 mass_id)
{
    const BoardMass *m = board_get_mass(mass_id);
    u8 stage;

    /* area 0 = オノコロ島 (ハブ)。ステージ1相当の敵を出す */
    stage = (m && m->area > 0) ? m->area : 1;
    if (stage > 8) stage = 8;

    return glue_enemy_pick_wild(stage);
}

int glue_battle_start_pve(int pid, u16 monster_id, BtlUnit *player_unit, BtlUnit *enemy_unit)
{
    GluePlayer *p = &glue_players[pid];
    const GlueEnemy *e = glue_enemy_get(monster_id);

    /* 基礎パラメータは RpgActor から取り、装備補正だけ上書きする */
    rpg_to_btl_unit(&p->actor, player_unit);
    player_unit->atk = (i16)glue_player_effective_atk(pid);
    player_unit->def = (i16)glue_player_effective_def(pid);

    if (e) {
        enemy_unit->max_hp   = e->max_hp;
        enemy_unit->atk      = e->atk;
        enemy_unit->def      = e->def;
        enemy_unit->spd      = e->spd;
        enemy_unit->mag      = e->mag;
        enemy_unit->elements = e->elements;
        enemy_unit->class_id = e->class_id;

        /* 魔物大量発生の週は敵が 1.5 倍になる */
        if (glue_event_monster_surge()) {
            enemy_unit->max_hp = (i16)(enemy_unit->max_hp * 3 / 2);
            enemy_unit->atk    = (i16)(enemy_unit->atk * 3 / 2);
            enemy_unit->def    = (i16)(enemy_unit->def * 3 / 2);
        }
    } else {
        /* 敵マスタ未登録 (battle.db が古い等) のフォールバック */
        enemy_unit->max_hp   = (i16)(30 + (monster_id * 10));
        enemy_unit->atk      = (i16)(8 + (monster_id * 2));
        enemy_unit->def      = (i16)(6 + (monster_id * 2));
        enemy_unit->spd      = (i16)(8 + monster_id);
        enemy_unit->mag      = (i16)(5 + monster_id);
        enemy_unit->elements = 0;
        enemy_unit->class_id = 1;
    }
    enemy_unit->hp = enemy_unit->max_hp;
    enemy_unit->status = 0;
    enemy_unit->tame_count = 0;
    enemy_unit->modifier_count = 0;

    return 0;
}

int glue_battle_resolve_turn(BtlUnit *player_unit, u8 player_cmd, BtlUnit *enemy_unit, u8 enemy_cmd, BtlResult *res_p, BtlResult *res_e)
{
    int p_first = btl_first_strike((int)player_unit->spd, (int)enemy_unit->spd);

    if (p_first) {
        *res_p = btl_resolve_turn(player_unit, player_cmd, enemy_unit, enemy_cmd);
        if (res_p->damage > 0) {
            enemy_unit->hp -= res_p->damage;
            if (enemy_unit->hp < 0) enemy_unit->hp = 0;
        }

        if (enemy_unit->hp > 0) {
            *res_e = btl_resolve_turn(enemy_unit, enemy_cmd, player_unit, player_cmd);
            if (res_e->damage > 0) {
                player_unit->hp -= res_e->damage;
                if (player_unit->hp < 0) player_unit->hp = 0;
            }
        } else {
            res_e->damage = 0;
            res_e->result_type = BTL_RES_MISS;
        }
    } else {
        *res_e = btl_resolve_turn(enemy_unit, enemy_cmd, player_unit, player_cmd);
        if (res_e->damage > 0) {
            player_unit->hp -= res_e->damage;
            if (player_unit->hp < 0) player_unit->hp = 0;
        }

        if (player_unit->hp > 0) {
            *res_p = btl_resolve_turn(player_unit, player_cmd, enemy_unit, enemy_cmd);
            if (res_p->damage > 0) {
                enemy_unit->hp -= res_p->damage;
                if (enemy_unit->hp < 0) enemy_unit->hp = 0;
            }
        } else {
            res_p->damage = 0;
            res_p->result_type = BTL_RES_MISS;
        }
    }

    return 0;
}

/* ====================================================================== */
/*  集金所                                                                 */
/* ====================================================================== */

int glue_collect_list(int pid, u16 *out, int max)
{
    return econ_estate_list((u8)pid, out, max);
}

u32 glue_collect_one(int pid, u16 estate_id)
{
    const EconEstate *e = econ_estate_get(estate_id);
    u32 got;

    /* 自分の村でなければ何もしない */
    if (!e || e->owner != (u8)pid) return 0;

    got = econ_estate_collect_one(estate_id);
    glue_players[pid].gold += got;
    return got;
}

u32 glue_invest_cost(u16 estate_id)
{
    return econ_estate_invest_cost(estate_id);
}

int glue_invest_estate(int pid, u16 estate_id)
{
    GluePlayer *p = &glue_players[pid];
    const EconEstate *e = econ_estate_get(estate_id);
    u32 wallet;
    int ret;

    if (!e || e->owner != (u8)pid) return -1;

    wallet = p->gold;
    ret = econ_estate_invest(estate_id, &wallet);
    if (ret == 0) {
        p->gold = wallet;
    }
    return ret;
}

/* ====================================================================== */
/*  曜日                                                                   */
/* ====================================================================== */

int glue_day_of_week(void)
{
    /* turn_count は 1 起点。ターン1を日曜として数える */
    u16 t = turn_count(&glue_turnstate);
    if (t == 0) return GLUE_SUN;
    return (int)((t - 1) % 7);
}

const char *glue_day_name(void)
{
    static const char *names[7] = {
        "日", "月", "火", "水", "木", "金", "土"
    };
    return names[glue_day_of_week()];
}

u16 glue_shop_price_scale(void)
{
    u16 scale;

    switch (glue_day_of_week()) {
    case GLUE_SUN: return 0;    /* 定休日はイベント中でも休み */
    case GLUE_SAT: scale = 75;  break;   /* 25%引き */
    default:       scale = 100; break;
    }

    /* 大売出し中はさらに半額 (土曜と重なれば 37%) */
    if (glue_event_big_sale()) {
        scale = scale / 2;
    }
    return scale;
}

void glue_shop_apply_day(void)
{
    inv_shop_set_price_scale(glue_shop_price_scale());
}

/* ====================================================================== */
/*  地形効果                                                               */
/* ====================================================================== */

u32 glue_apply_terrain(int pid, u8 terrain, int stopped)
{
    GluePlayer *p = &glue_players[pid];

    switch (terrain) {
    case BOARD_TERR_SWAMP:
        /* 毒の沼: 通過しただけで毒 */
        if (!rpg_has_status(&p->actor, GLUE_ST_POISON)) {
            rpg_status_apply(&p->actor, GLUE_ST_POISON);
            return GLUE_ST_POISON;
        }
        break;

    case BOARD_TERR_SNOW:
        /* 雪原: 止まったときだけマヒ。次の手番を1回飛ばす */
        if (stopped && !rpg_has_status(&p->actor, GLUE_ST_PARALYZE)) {
            rpg_status_apply(&p->actor, GLUE_ST_PARALYZE);
            turn_skip(&glue_turnstate, (u8)pid, 1);
            return GLUE_ST_PARALYZE;
        }
        break;

    default:
        break;
    }
    return 0;
}

int glue_status_tick(int pid)
{
    GluePlayer *p = &glue_players[pid];
    RpgTickLog log;

    memset(&log, 0, sizeof(log));
    rpg_status_tick(&p->actor, &log);

    /* 状態異常で倒れても振り出しに戻すのは呼び出し側の責務 */
    if (p->actor.hp < 0) p->actor.hp = 0;
    return (int)log.tick_damage;
}

/* ====================================================================== */
/*  戦闘コマンド                                                           */
/* ====================================================================== */

const char *glue_atk_cmd_name(u8 cmd)
{
    switch (cmd) {
    case GLUE_ATK_NORMAL: return "攻撃";
    case GLUE_ATK_HEAVY:  return "強攻撃";
    case GLUE_ATK_MAGIC:  return "魔法";
    case GLUE_ATK_CHARGE: return "ためる";
    case GLUE_ATK_FLEE:   return "逃走";
    default:              return "?";
    }
}

const char *glue_def_cmd_name(u8 cmd)
{
    switch (cmd) {
    case GLUE_DEF_NONE:    return "無防備";
    case GLUE_DEF_GUARD:   return "防御";
    case GLUE_DEF_DODGE:   return "回避";
    case GLUE_DEF_REFLECT: return "反射";
    default:               return "?";
    }
}

int glue_battle_strike(BtlUnit *atk, u8 atk_cmd,
                       BtlUnit *def, u8 def_cmd, BtlResult *out)
{
    BtlResult res = btl_resolve_turn(atk, atk_cmd, def, def_cmd);

    if (out) *out = res;

    if (atk_cmd == GLUE_ATK_FLEE) {
        return (res.result_type == BTL_RES_YIELD) ? 1 : 0;
    }

    /* ためるは攻撃側にカウントが乗るだけ (ダメージなし) */
    if (atk_cmd == GLUE_ATK_CHARGE) {
        if (atk->tame_count < 255) atk->tame_count++;
        return 0;
    }

    if (res.damage > 0) {
        /* 反射は攻撃側が食らう */
        if (res.result_type == BTL_RES_REFLECT) {
            atk->hp -= res.damage;
            if (atk->hp < 0) atk->hp = 0;
        } else {
            def->hp -= res.damage;
            if (def->hp < 0) def->hp = 0;
        }
    }

    /* 攻撃したら溜めは解放される */
    if (atk_cmd != GLUE_ATK_CHARGE) atk->tame_count = 0;

    return 0;
}

/* ====================================================================== */
/*  言霊 (魔法アイテム)                                                    */
/* ====================================================================== */

int glue_is_kotodama(u16 item_id)
{
    const InvItemDef *d = inv_get_def(item_id);
    if (!d) return 0;
    return (d->type == INV_TYPE_CONSUMABLE &&
            d->effect >= GLUE_KOTODAMA_ATTACK) ? 1 : 0;
}

int glue_kotodama_list(int pid, u8 *out_slots, int max)
{
    const InvBag *bag = &glue_players[pid].bag;
    int i, n = 0;

    for (i = 0; i < (int)bag->max_slots && n < max; i++) {
        const InvSlot *s = inv_get_slot(bag, (u8)i);
        if (s && s->item_id != 0 && glue_is_kotodama(s->item_id)) {
            out_slots[n++] = (u8)i;
        }
    }
    return n;
}

int glue_kotodama_cast(int pid, u8 slot, BtlUnit *self, BtlUnit *foe,
                       char *msg)
{
    GluePlayer *p = &glue_players[pid];
    const InvSlot *s = inv_get_slot(&p->bag, slot);
    const InvItemDef *d;

    if (!s || s->item_id == 0) return -1;
    d = inv_get_def(s->item_id);
    if (!d || !glue_is_kotodama(s->item_id)) return -1;

    switch (d->effect) {
    case GLUE_KOTODAMA_ATTACK: {
        /* 威力 + 術者の MAG。防御力では軽減されない (言霊は魔の理) */
        int dmg = (int)d->param + (int)self->mag;
        foe->hp -= (i16)dmg;
        if (foe->hp < 0) foe->hp = 0;
        sprintf(msg, "%s！ %d のダメージ", d->name, dmg);
        break;
    }

    case GLUE_KOTODAMA_HEAL: {
        int before = (int)self->hp;
        int hp = before + (int)d->param;
        if (hp > (int)self->max_hp) hp = (int)self->max_hp;
        self->hp = (i16)hp;
        sprintf(msg, "%s！ HP を %d 回復", d->name, hp - before);
        break;
    }

    case GLUE_KOTODAMA_CURSE:
        foe->status |= (u32)d->stat_bonus;
        sprintf(msg, "%s！ 敵を呪縛した", d->name);
        break;

    default:
        return -1;
    }

    inv_remove(&p->bag, slot, 1);
    return 0;
}

/* ====================================================================== */
/*  ダンジョン (潜行型)                                                    */
/* ====================================================================== */

int glue_dungeon_floor = 0;
u32 glue_dungeon_loot  = 0;

void glue_dungeon_enter(int pid)
{
    (void)pid;
    glue_dungeon_floor = 0;
    glue_dungeon_loot  = 0;
}

/* その階の基準額。深いほど旨い */
static u32 floor_value(int floor)
{
    return (u32)(60 * floor + 20);
}

int glue_dungeon_descend(int pid, u16 *out_enemy, char *msg)
{
    int roll;
    u8 stage;

    glue_dungeon_floor++;
    if (out_enemy) *out_enemy = 0;

    /* 最深部。ここまで来たら大物を置いて強制帰還にする */
    if (glue_dungeon_floor >= GLUE_DUNGEON_MAX_FLOOR) {
        glue_dungeon_loot += floor_value(glue_dungeon_floor) * 3;
        sprintf(msg, "最奥の広間！ 財宝 %d G",
                (int)(floor_value(glue_dungeon_floor) * 3));
        return GLUE_DFLOOR_EXIT;
    }

    /* 階が深いほど強い敵が出る (ステージ 1..8) */
    stage = (u8)(1 + glue_dungeon_floor * 8 / GLUE_DUNGEON_MAX_FLOOR);
    if (stage > 8) stage = 8;

    roll = (int)rng_range(0, 99);
    if (roll < 55) {
        u16 id = glue_enemy_pick_wild(stage);
        if (id == 0) id = 1;
        if (out_enemy) *out_enemy = id;
        sprintf(msg, "地下%d階 %s が立ちふさがる！",
                glue_dungeon_floor, glue_enemy_name(id));
        return GLUE_DFLOOR_BATTLE;
    }
    if (roll < 75) {
        u32 gain = floor_value(glue_dungeon_floor);
        u16 item = inv_lottery(0, stage);
        glue_dungeon_loot += gain;
        if (item != 0 && inv_add(&glue_players[pid].bag, item, 1) >= 0) {
            const InvItemDef *d = inv_get_def(item);
            sprintf(msg, "地下%d階 財宝！ %d G と %s",
                    glue_dungeon_floor, (int)gain, d ? d->name : "何か");
        } else {
            sprintf(msg, "地下%d階 財宝！ %d G", glue_dungeon_floor, (int)gain);
        }
        return GLUE_DFLOOR_TREASURE;
    }
    if (roll < 90) {
        int dmg = 5 + glue_dungeon_floor * 3;
        glue_players[pid].actor.hp -= (i16)dmg;
        if (glue_players[pid].actor.hp < 0) glue_players[pid].actor.hp = 0;
        sprintf(msg, "地下%d階 罠だ！ %d のダメージ", glue_dungeon_floor, dmg);
        return GLUE_DFLOOR_TRAP;
    }

    {
        RpgActor *a = &glue_players[pid].actor;
        int before = (int)a->hp;
        a->hp = a->max_hp;
        a->status = 0;
        sprintf(msg, "地下%d階 祠があった。HP を %d 回復",
                glue_dungeon_floor, (int)a->hp - before);
    }
    return GLUE_DFLOOR_SHRINE;
}

void glue_dungeon_reward_floor(int pid)
{
    (void)pid;
    glue_dungeon_loot += floor_value(glue_dungeon_floor);
}

u32 glue_dungeon_escape(int pid)
{
    u32 loot = glue_dungeon_loot;
    glue_players[pid].gold += loot;
    glue_dungeon_floor = 0;
    glue_dungeon_loot  = 0;
    return loot;
}

void glue_dungeon_fail(int pid)
{
    (void)pid;
    glue_dungeon_floor = 0;
    glue_dungeon_loot  = 0;
}

int glue_use_item(int pid, u8 slot, char *msg)
{
    GluePlayer *p = &glue_players[pid];
    const InvSlot *s = inv_get_slot(&p->bag, slot);
    const InvItemDef *d;

    if (!s || s->item_id == 0) return -1;
    d = inv_get_def(s->item_id);
    if (!d || d->type != INV_TYPE_CONSUMABLE) return -1;
    if (glue_is_kotodama(s->item_id)) return -1;   /* 言霊は戦闘専用 */

    switch (d->effect) {
    case 1: {   /* 回復薬 */
        int before = (int)p->actor.hp;
        int hp = before + (int)d->param;
        if (hp > (int)p->actor.max_hp) hp = (int)p->actor.max_hp;
        p->actor.hp = (i16)hp;
        sprintf(msg, "%s: HP を %d 回復", d->name, hp - before);
        break;
    }

    case 2:     /* 毒消し */
        p->actor.status &= ~(u32)BTL_STATUS_POISON;
        sprintf(msg, "%s: 毒が消えた", d->name);
        break;

    case 3:     /* 万能薬: 全回復 + 状態異常解除 */
        p->actor.hp = p->actor.max_hp;
        p->actor.status = 0;
        sprintf(msg, "%s: すっかり癒えた", d->name);
        break;

    default:
        /* 素材や戦闘専用の効果は盤上では使えない */
        return -1;
    }

    inv_remove(&p->bag, slot, 1);
    return 0;
}

u8 glue_ai_attack_cmd(int pid, const BtlUnit *self, const BtlUnit *foe)
{
    AiOption opts[GLUE_ATK_COUNT];
    const AiProfile *prof = &g_ai[pid];
    int aggro = (int)prof->params[2];
    int caution = (int)prof->params[3];
    int hp_pct = (self->max_hp > 0)
                 ? (int)self->hp * 100 / (int)self->max_hp : 100;
    int n = 0;

    /* 通常攻撃: いつでも無難 */
    opts[n].id = GLUE_ATK_NORMAL; opts[n].score = 50; opts[n]._pad = 0; n++;

    /* 強攻撃: 攻撃的な性格ほど好む。溜めていると更に good */
    opts[n].id = GLUE_ATK_HEAVY;
    opts[n].score = (i16)(20 + aggro / 2 + self->tame_count * 15);
    opts[n]._pad = 0; n++;

    /* 魔法: MAG が高いほど good */
    opts[n].id = GLUE_ATK_MAGIC;
    opts[n].score = (i16)(10 + (int)self->mag * 2);
    opts[n]._pad = 0; n++;

    /* ためる: 慎重かつ相手がまだ元気なとき */
    opts[n].id = GLUE_ATK_CHARGE;
    opts[n].score = (i16)(caution / 3 + (self->tame_count == 0 ? 15 : -30));
    opts[n]._pad = 0; n++;

    /* 逃走: 瀕死なら最優先 */
    opts[n].id = GLUE_ATK_FLEE;
    opts[n].score = (i16)(hp_pct < 25 ? 90 + caution / 2 : -50);
    opts[n]._pad = 0; n++;

    (void)foe;
    return ai_decide(prof, opts, n);
}

u8 glue_ai_defend_cmd(int pid, const BtlUnit *self, const BtlUnit *foe)
{
    AiOption opts[GLUE_DEF_COUNT];
    const AiProfile *prof = &g_ai[pid];
    int caution = (int)prof->params[3];
    int hp_pct = (self->max_hp > 0)
                 ? (int)self->hp * 100 / (int)self->max_hp : 100;
    int n = 0;

    /* 無防備: 基本は選ばない */
    opts[n].id = GLUE_DEF_NONE; opts[n].score = 5; opts[n]._pad = 0; n++;

    /* 防御: HPが減るほど堅くなる */
    opts[n].id = GLUE_DEF_GUARD;
    opts[n].score = (i16)(40 + caution / 2 + (100 - hp_pct) / 3);
    opts[n]._pad = 0; n++;

    /* 回避: 素早いほど期待できる */
    opts[n].id = GLUE_DEF_DODGE;
    opts[n].score = (i16)(15 + (int)self->spd);
    opts[n]._pad = 0; n++;

    /* 反射: 相手の攻撃力が高いほど旨い */
    opts[n].id = GLUE_DEF_REFLECT;
    opts[n].score = (i16)(10 + (int)foe->atk);
    opts[n]._pad = 0; n++;

    return ai_decide(prof, opts, n);
}

int glue_battle_reward(int pid, u16 monster_id, RpgLevelResult *out_lv)
{
    GluePlayer *p = &glue_players[pid];
    const GlueEnemy *e = glue_enemy_get(monster_id);
    RpgLevelResult lv;

    memset(&lv, 0, sizeof(lv));

    if (e) {
        p->gold += e->gold;
        rpg_add_exp(&p->actor, e->exp, &lv);
    }

    /* CPU は自分でパラメータを振る。人間プレイヤーは pending_points に
       溜めておき、配分UIから rpg_alloc_point() を呼ぶ */
    if (p->is_cpu && p->actor.pending_points > 0) {
        rpg_auto_alloc(&p->actor);
    }

    if (out_lv) *out_lv = lv;
    return (int)lv.levels_gained;
}

/* ====================================================================== */
/*  セーブ/ロード                                                          */
/* ====================================================================== */

/* 不動産の可変状態のスナップショット。
   libos32econ は内部キャッシュを外へ出さないので、セーブ時にここへ
   吸い出し、ロード時に econ 側へ書き戻す。 */
typedef struct {
    u16 id;
    u8  owner;
    u8  level;
    /* 未回収の上納金。glue_week_tick() が蓄積と回収を同じ週境界で行うため
       通常は常に 0 だが、将来「回収は城でのみ」等に変えた時のために保存する。
       econ 側に tax の setter がないので、現状ロード時には書き戻せない。 */
    u32 tax;
} GlueEstateSnap;

static GlueEstateSnap g_estate_snap[ECON_ESTATE_MAX];
static u16 g_estate_snap_count;

static void snapshot_estates(void)
{
    int i;
    int total = econ_estate_total();

    g_estate_snap_count = 0;
    for (i = 0; i < total && i < ECON_ESTATE_MAX; i++) {
        const EconEstate *e = econ_estate_at(i);
        if (!e) continue;
        g_estate_snap[g_estate_snap_count].id    = e->id;
        g_estate_snap[g_estate_snap_count].owner = e->owner;
        g_estate_snap[g_estate_snap_count].level = e->level;
        g_estate_snap[g_estate_snap_count].tax   = e->tax;
        g_estate_snap_count++;
    }
}

static void restore_estates(void)
{
    int i;
    u8 lv;

    for (i = 0; i < (int)g_estate_snap_count; i++) {
        GlueEstateSnap *s = &g_estate_snap[i];

        if (s->owner == ECON_OWNER_NONE) {
            econ_estate_release(s->id);
        } else {
            econ_estate_claim(s->id, s->owner);
        }

        /* レベルは投資API経由でしか上げられないので、財布無限で積み上げる */
        for (lv = econ_estate_level(s->id); lv < s->level; lv++) {
            u32 wallet = 0xFFFFFFFFu;
            if (econ_estate_invest(s->id, &wallet) != 0) break;
        }
    }
}

/* セーブ領域を登録する。save_add_region はポインタを保持するだけなので
   セーブ/ロードのどちらでも同じ構成でよい。 */
/* libos32event のランタイム状態。ヘッダには出ていないが実体はグローバル
   なので、そのままセーブ領域に載せる。これを保存しないと、ロード後に
   継続中のイベント (大売出し等) とクールダウンが消える */
extern EvtActive g_evt_active[];
extern u16       g_evt_no_event_counter;
extern u16       g_evt_cooldowns[];

static void build_save_context(SaveContext *c)
{
    /* version 2: ボス街道・勝利状態・ダンジョン・イベントを追加。
       version 1 のセーブは region が足りず save_read が失敗する
       (中途半端に読むより、読めないと分かる方が安全) */
    save_begin(c, "DKP2", 2);
    save_add_region(c, glue_players, sizeof(glue_players), 1);
    save_add_region(c, &glue_num_players, sizeof(glue_num_players), 2);
    save_add_region(c, &glue_turnstate, sizeof(glue_turnstate), 3);
    save_add_region(c, &g_estate_snap_count, sizeof(g_estate_snap_count), 4);
    save_add_region(c, g_estate_snap, sizeof(g_estate_snap), 5);

    /* 城のボス街道と決着状態。これが無いとロードで街道が 1 段目に戻り、
       征服王ルートが実質セーブ不可になる */
    save_add_region(c, &glue_boss_progress, sizeof(glue_boss_progress), 6);
    save_add_region(c, &glue_victory_type, sizeof(glue_victory_type), 7);
    save_add_region(c, &glue_victory_winner, sizeof(glue_victory_winner), 8);

    /* ダンジョンの潜行途中。今は潜行中にセーブできないので常に 0 だが、
       セーブ地点を増やしたときに黙って壊れないよう最初から持たせる */
    save_add_region(c, &glue_dungeon_floor, sizeof(glue_dungeon_floor), 9);
    save_add_region(c, &glue_dungeon_loot, sizeof(glue_dungeon_loot), 10);

    /* 週次イベントの継続状態・クールダウン・未発生カウンタ */
    save_add_region(c, g_evt_active,
                    (u32)(sizeof(EvtActive) * EVT_MAX_ACTIVE), 11);
    save_add_region(c, g_evt_cooldowns,
                    (u32)(sizeof(u16) * EVT_MAX_DEFS), 12);
    save_add_region(c, &g_evt_no_event_counter,
                    sizeof(g_evt_no_event_counter), 13);
}

int glue_save(const char *path)
{
    SaveContext c;

    snapshot_estates();
    build_save_context(&c);

    /* user_meta には累計ターン数を入れ、save_peek でセーブ一覧に出せるようにする */
    return save_write(&c, path, (u32)turn_count(&glue_turnstate));
}

int glue_load(const char *path)
{
    SaveContext c;
    int rc;

    build_save_context(&c);

    rc = save_read(&c, path);
    if (rc != 0) return rc;

    restore_estates();
    sync_turn_mirror();

    /* 曜日と継続イベント (大売出し) に応じた店の価格倍率を復元する。
       これを忘れると、ロード直後だけ休業日に買えてしまう */
    glue_shop_apply_day();

    return 0;
}

u16 glue_village_id_of_mass(u16 mass_id)
{
    const BoardMass *m = board_get_mass(mass_id);
    if (!m) return 0;
    if (m->type != MASS_VILLAGE) return 0;
    return (u16)m->param;
}

const EconEstate *glue_village_estate(u16 mass_id)
{
    u16 vid = glue_village_id_of_mass(mass_id);
    if (vid == 0) return (const EconEstate *)0;
    return econ_estate_get(vid);
}

u32 glue_village_invest_cost(u16 mass_id)
{
    u16 vid = glue_village_id_of_mass(mass_id);
    if (vid == 0) return 0;
    return econ_estate_invest_cost(vid);
}

int glue_village_claim(u16 mass_id, int pid)
{
    u16 vid = glue_village_id_of_mass(mass_id);
    if (vid == 0) return -1;
    return econ_estate_claim(vid, (u8)pid);
}

int glue_village_invest(u16 mass_id, int pid)
{
    GluePlayer *p = &glue_players[pid];
    u32 wallet;
    int ret;
    u16 vid = glue_village_id_of_mass(mass_id);

    if (vid == 0) return -1;

    wallet = p->gold;
    ret = econ_estate_invest(vid, &wallet);
    if (ret == 0) {
        p->gold = wallet;
    }
    return ret;
}

u32 glue_village_collect_taxes(int pid)
{
    u32 income = econ_estate_collect((u8)pid);
    glue_players[pid].gold += income;
    return income;
}

u32 glue_player_total_assets(int pid)
{
    GluePlayer *p = &glue_players[pid];
    u32 estate_val = econ_estate_total_value((u8)pid);
    return p->gold + estate_val;
}

/* ====================================================================== */
/*  勝利条件                                                               */
/* ====================================================================== */

int glue_victory_type = GLUE_WIN_NONE;
int glue_victory_winner = -1;
u8  glue_boss_progress = 1;

const char *glue_victory_name(int type)
{
    switch (type) {
    case GLUE_WIN_ASSET:      return "資産王";
    case GLUE_WIN_DOMINATION: return "制覇王";
    case GLUE_WIN_CONQUEST:   return "征服王";
    default:                  return "";
    }
}

/* 盤面の村マスの総数。盤面は不変なので初回に数えて覚える */
static int total_villages(void)
{
    static int cached = -1;
    if (cached < 0) {
        int n = board_mass_count();
        int i, cnt = 0;
        for (i = 0; i < n; i++) {
            const BoardMass *m = board_get_mass_at(i);
            if (m && m->type == MASS_VILLAGE) cnt++;
        }
        cached = cnt;
    }
    return cached;
}

u16 glue_castle_boss(void)
{
    if (glue_boss_progress > 8) return 0;
    return glue_enemy_boss_of_stage(glue_boss_progress);
}

u32 glue_boss_defeated(int pid)
{
    u32 bounty = 0;

    /* 討伐指令の週なら賞金を上乗せする (基本報酬は戦闘側で獲得済み) */
    if (evt_is_active(GLUE_EVT_BOSS_HUNT)) {
        const GlueEnemy *e = glue_enemy_get(glue_castle_boss());
        bounty = e ? e->gold : (u32)(100 * glue_boss_progress);
        glue_players[pid].gold += bounty;
    }

    if (glue_boss_progress >= 8) {
        /* 最終ボス撃破 = 征服王 */
        glue_victory_type = GLUE_WIN_CONQUEST;
        glue_victory_winner = pid;
    }
    glue_boss_progress++;
    return bounty;
}

int glue_victory_check(void)
{
    int i;

    if (glue_victory_type != GLUE_WIN_NONE) return 1;   /* 征服王が確定済み */

    /* 制覇王: 全村の過半数を統治した瞬間 */
    for (i = 0; i < (int)glue_num_players; i++) {
        if (econ_estate_count((u8)i) * 2 > total_villages()) {
            glue_victory_type = GLUE_WIN_DOMINATION;
            glue_victory_winner = i;
            return 1;
        }
    }

    /* 資産王: ターン上限に達したら総資産が最大のプレイヤー */
    if (turn_is_over(&glue_turnstate)) {
        int best = 0;
        u32 top = glue_player_total_assets(0);
        for (i = 1; i < (int)glue_num_players; i++) {
            u32 a = glue_player_total_assets(i);
            if (a > top) { top = a; best = i; }
        }
        glue_victory_type = GLUE_WIN_ASSET;
        glue_victory_winner = best;
        return 1;
    }

    return 0;
}

int glue_devil_check(int pid)
{
    int i;
    u32 my_assets;

    if (glue_players[pid].is_devil) return 0; /* 既に変身中 */
    if (glue_players[pid].is_cpu && glue_players[pid].devil_cooldown > 0) return 0;

    my_assets = glue_player_total_assets(pid);

    /* 自分より総資産が厳密に低いプレイヤーがいたら最下位ではない（同率最下位は変身可能） */
    for (i = 0; i < (int)glue_num_players; i++) {
        if (i == pid) continue;
        if (glue_player_total_assets(i) < my_assets) {
            return 0; /* 最下位ではない */
        }
    }
    return 1; /* 最下位 */
}

void glue_devil_transform(int pid)
{
    GluePlayer *p = &glue_players[pid];
    int i, owned_count;
    u16 owned[ECON_ESTATE_MAX];

    if (p->is_devil) return;

    /* 所持金・アイテムの放棄 */
    p->gold = 0;
    inv_bag_init(&p->bag, GLUE_MAX_ITEMS, GLUE_EQUIP_SLOTS);

    /* イザナミの装備を身につける。inv_bag_init で空にした直後なので
       全スロットが空いている */
    inv_add(&p->bag, GLUE_DEVIL_WEAPON, 1);
    inv_add(&p->bag, GLUE_DEVIL_SHIELD, 1);
    inv_add(&p->bag, GLUE_DEVIL_ARMOR, 1);
    inv_equip(&p->bag, GLUE_DEVIL_WEAPON, INV_ESLOT_WEAPON);
    inv_equip(&p->bag, GLUE_DEVIL_SHIELD, INV_ESLOT_SHIELD);
    inv_equip(&p->bag, GLUE_DEVIL_ARMOR, INV_ESLOT_ARMOR);

    /* 統治村の放棄 (不動産IDはマスIDとは別物なので econ 側に列挙させる) */
    owned_count = econ_estate_list((u8)pid, owned, ECON_ESTATE_MAX);
    for (i = 0; i < owned_count; i++) {
        econ_estate_release(owned[i]);
    }

    /* 変身前のパラメータを控えておく (解除時に割り算の丸めで目減りしないよう、
       除算で戻さず控えた値をそのまま復元する) */
    p->base_atk    = p->actor.atk;
    p->base_def    = p->actor.def;
    p->base_spd    = p->actor.spd;
    p->base_mag    = p->actor.mag;
    p->base_max_hp = p->actor.max_hp;

    /* パラメータの強化 */
    p->actor.atk    = (i16)(p->base_atk * 2);
    p->actor.def    = (i16)(p->base_def * 2);
    p->actor.spd    = (i16)(p->base_spd * 2);
    p->actor.mag    = (i16)(p->base_mag * 2);
    p->actor.max_hp = (i16)(p->base_max_hp * 4);
    p->actor.hp     = p->actor.max_hp;

    p->is_devil = 1;

    /* 変身持続ターン数 */
    p->devil_turns = glue_week;
    if (p->devil_turns < 3) p->devil_turns = 3;
    if (p->devil_turns > 30) p->devil_turns = 30;
}

void glue_devil_release(int pid)
{
    GluePlayer *p = &glue_players[pid];
    i32 hp;

    if (!p->is_devil) return;

    /* ステータスを変身前の控えから復元 */
    p->actor.atk = p->base_atk;
    p->actor.def = p->base_def;
    p->actor.spd = p->base_spd;
    p->actor.mag = p->base_mag;

    /* HP比率を維持して縮小復元 */
    hp = p->actor.hp;
    if (p->actor.max_hp > 0) {
        hp = ((i32)p->actor.hp * (i32)p->base_max_hp) / (i32)p->actor.max_hp;
    }
    p->actor.max_hp = p->base_max_hp;

    /* 縮小の丸めで 0 に落ちて即死しないよう最低 1 は残す */
    if (hp < 1) hp = 1;
    if (hp > (i32)p->actor.max_hp) hp = (i32)p->actor.max_hp;
    p->actor.hp = (i16)hp;

    /* イザナミの装備は変身が解けたら消える (持ち逃げさせない)。
       バッグごと空にする — 変身中に拾ったものも道連れという仕様 */
    inv_bag_init(&p->bag, GLUE_MAX_ITEMS, GLUE_EQUIP_SLOTS);

    p->is_devil = 0;
    p->devil_turns = 0;

    if (p->is_cpu) {
        p->devil_cooldown = 10; /* CPUクールダウン10週間 */
    }
}

int glue_devil_update(int pid, char *out_msg)
{
    GluePlayer *p = &glue_players[pid];
    u16 max_turns;
    u16 early_start;
    u16 elapsed;
    u16 counter;

    if (!p->is_devil) return 0;

    p->devil_turns--;

    /* 確定解除: ターン0 */
    if (p->devil_turns <= 0) {
        glue_devil_release(pid);
        strcpy(out_msg, "Devil transformation expired.");
        return 1;
    }

    /* 確率解除判定 */
    max_turns = glue_week;
    if (max_turns < 3) max_turns = 3;
    if (max_turns > 30) max_turns = 30;

    early_start = max_turns - (max_turns / 10);
    if (early_start < 3) early_start = 3;

    elapsed = max_turns - p->devil_turns;
    if (elapsed >= early_start) {
        counter = elapsed - early_start + 1;
        /* カウンタ式確率: counter * 40 / 256 の確率で解除 */
        if ((int)rng_range(0, 255) < (int)counter * 40) {
            glue_devil_release(pid);
            strcpy(out_msg, "Devil transformation released.");
            return 1;
        }
    }

    return 0;
}
