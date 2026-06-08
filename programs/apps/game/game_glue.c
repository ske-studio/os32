#include "game_glue.h"
#include "os32api.h"
#include "libos32math.h"
#include <string.h>

GluePlayer glue_players[GLUE_MAX_PLAYERS];
u8 glue_num_players = 4;
u8 glue_current_player = 0;
u16 glue_week = 1;
u16 glue_turn = 0;

static KernelAPI *api;

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

    /* 週数カウントのリセット */
    glue_week = 1;
    glue_turn = 0;

    /* プレイヤー初期化 */
    for (i = 0; i < GLUE_MAX_PLAYERS; i++) {
        p = &glue_players[i];
        
        p->level = 1;
        p->exp = 0;
        p->hp = 50;
        p->max_hp = 50;
        p->atk = 10;
        p->def = 10;
        p->spd = 10;
        p->mag = 10;
        p->gold = 1000;
        p->pos = 0;
        p->status = 0;
        p->is_cpu = (i == 0) ? 0 : 1;
        p->is_devil = 0;
        p->devil_turns = 0;
        p->devil_cooldown = 0;

        switch (i) {
            case 0:
                strcpy(p->name, "Susanoo");
                p->ujigami = 0;
                break;
            case 1:
                strcpy(p->name, "Y.Takeru");
                p->ujigami = 1;
                break;
            case 2:
                strcpy(p->name, "Okuninushi");
                p->ujigami = 2;
                break;
            case 3:
                strcpy(p->name, "Amaterasu");
                p->ujigami = 3;
                break;
        }

        /* インベントリ初期化: 最大10枠、装備スロット3つ */
        inv_bag_init(&p->bag, GLUE_MAX_ITEMS, 3);
        
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
}

int glue_player_effective_atk(int pid)
{
    GluePlayer *p = &glue_players[pid];
    int bonus;
    if (p->is_devil) {
        return (int)p->atk + 86; /* イザナミの武器ATK */
    }
    bonus = inv_total_bonus(&p->bag, INV_STAT_ATK);
    return (int)p->atk + bonus;
}

int glue_player_effective_def(int pid)
{
    GluePlayer *p = &glue_players[pid];
    int bonus;
    if (p->is_devil) {
        return (int)p->def + 105; /* イザナミの防具DEF (盾43+鎧62) */
    }
    bonus = inv_total_bonus(&p->bag, INV_STAT_DEF);
    return (int)p->def + bonus;
}

int glue_battle_start_pve(int pid, u8 monster_id, BtlUnit *player_unit, BtlUnit *enemy_unit)
{
    GluePlayer *p = &glue_players[pid];
    
    player_unit->hp = (i16)p->hp;
    player_unit->max_hp = (i16)p->max_hp;
    player_unit->atk = (i16)glue_player_effective_atk(pid);
    player_unit->def = (i16)glue_player_effective_def(pid);
    player_unit->spd = (i16)p->spd;
    player_unit->mag = (i16)p->mag;
    player_unit->status = p->status;
    player_unit->elements = 0;
    player_unit->tame_count = 0;
    player_unit->class_id = 0;
    player_unit->modifier_count = 0;

    enemy_unit->max_hp = 30 + (monster_id * 10);
    enemy_unit->hp = enemy_unit->max_hp;
    enemy_unit->atk = 8 + (monster_id * 2);
    enemy_unit->def = 6 + (monster_id * 2);
    enemy_unit->spd = 8 + monster_id;
    enemy_unit->mag = 5 + monster_id;
    enemy_unit->status = 0;
    enemy_unit->elements = 0;
    enemy_unit->tame_count = 0;
    enemy_unit->class_id = 1;
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

int glue_village_claim(u8 mass_id, int pid)
{
    return econ_estate_claim((u16)mass_id, (u8)pid);
}

int glue_village_invest(u8 mass_id, int pid)
{
    GluePlayer *p = &glue_players[pid];
    u32 wallet = p->gold;
    int ret = econ_estate_invest((u16)mass_id, &wallet);
    if (ret == 0) {
        p->gold = wallet;
    }
    return ret;
}

u32 glue_village_collect_taxes(int pid)
{
    return econ_estate_collect((u8)pid);
}

u32 glue_player_total_assets(int pid)
{
    GluePlayer *p = &glue_players[pid];
    u32 estate_val = econ_estate_total_value((u8)pid);
    return p->gold + estate_val;
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
    int i, m_count;

    if (p->is_devil) return;

    /* 所持金・アイテムの放棄 */
    p->gold = 0;
    inv_bag_init(&p->bag, GLUE_MAX_ITEMS, 3);

    /* 統治村の放棄 */
    m_count = board_mass_count();
    for (i = 0; i < m_count; i++) {
        const EconEstate *estate = econ_estate_get((u16)i);
        if (estate && estate->owner == (u8)pid) {
            econ_estate_release((u16)i);
        }
    }

    /* パラメータの強化 */
    p->atk *= 2;
    p->def *= 2;
    p->spd *= 2;
    p->mag *= 2;
    p->max_hp *= 4;
    p->hp = p->max_hp;

    p->is_devil = 1;

    /* 変身持続ターン数 */
    p->devil_turns = glue_week;
    if (p->devil_turns < 3) p->devil_turns = 3;
    if (p->devil_turns > 30) p->devil_turns = 30;
}

void glue_devil_release(int pid)
{
    GluePlayer *p = &glue_players[pid];
    u32 orig_max_hp;

    if (!p->is_devil) return;

    /* ステータスを元に戻す */
    p->atk /= 2;
    p->def /= 2;
    p->spd /= 2;
    p->mag /= 2;

    /* HP比率を維持して縮小復元 */
    orig_max_hp = p->max_hp / 4;
    if (p->max_hp > 0) {
        p->hp = (p->hp * orig_max_hp) / p->max_hp;
    }
    p->max_hp = orig_max_hp;

    /* 念のためクランプ */
    if (p->hp > p->max_hp) {
        p->hp = p->max_hp;
    }

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
