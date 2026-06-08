/* ======================================================================== */
/*  VIEW_BATTLE.C — 戦闘画面の描画および入力処理                             */
/* ======================================================================== */

#include "view_battle.h"
#include "game_glue.h"
#include "libos32gfx.h"
#include "libos32ui.h"
#include "libos32math.h"
#include <stdio.h>
#include <string.h>

enum BattleState {
    B_STATE_CMD_INPUT,
    B_STATE_RESOLVE_FIRST,
    B_STATE_RESOLVE_SECOND,
    B_STATE_FINISHED
};

static KernelAPI *api;
static mu_Context *ctx;

static int b_pid;
static u8 b_monster_id;
static BtlUnit b_player;
static BtlUnit b_enemy;
static int b_state;

/* 演出用メッセージバッファ */
static char b_msg[128];
static int b_msg_timer;

/* コマンド選択結果 */
static u8 b_player_cmd;
static u8 b_enemy_cmd;
static BtlResult b_res_p;
static BtlResult b_res_e;

void view_battle_init(KernelAPI *kapi, mu_Context *mu_ctx)
{
    api = kapi;
    ctx = mu_ctx;
}

void view_battle_start(int player_id, u8 monster_id)
{
    b_pid = player_id;
    b_monster_id = monster_id;
    
    glue_battle_start_pve(player_id, monster_id, &b_player, &b_enemy);
    
    b_state = B_STATE_CMD_INPUT;
    b_msg[0] = '\0';
    b_msg_timer = 0;
}

int view_battle_update(void)
{
    if (b_state == B_STATE_FINISHED && b_msg_timer == 0) {
        glue_players[b_pid].hp = b_player.hp;
        return 0;
    }
    
    if (b_msg_timer > 0) {
        b_msg_timer--;
    }
    
    return 1;
}

static void draw_gauge(int x, int y, int w, int h, int val, int max, u32 color_bar, u32 color_bg)
{
    int fill_w;
    if (max <= 0) return;
    if (val < 0) val = 0;
    if (val > max) val = max;
    
    fill_w = (w * val) / max;
    
    gfx_fill_rect(x, y, w, h, color_bg);
    if (fill_w > 0) {
        gfx_fill_rect(x, y, fill_w, h, color_bar);
    }
}

void view_battle_draw(void)
{
    char hp_buf[32];
    char enemy_name[32];
    char name_buf[64];

    gfx_fill_rect(10, 10, 620, 220, 4);
    
    if (glue_players[b_pid].is_devil) {
        sprintf(name_buf, "%s (DEVIL)", glue_players[b_pid].name);
    } else {
        strcpy(name_buf, glue_players[b_pid].name);
    }
    kcg_draw_utf8(30, 30, name_buf, 15, 0);
    draw_gauge(30, 50, 150, 10, b_player.hp, b_player.max_hp, 2, 8);
    
    sprintf(hp_buf, "HP: %d/%d", (int)b_player.hp, (int)b_player.max_hp);
    kcg_draw_utf8(30, 65, hp_buf, 15, 0);

    sprintf(enemy_name, "Monster %d", (int)b_monster_id);
    kcg_draw_utf8(450, 30, enemy_name, 15, 0);
    draw_gauge(450, 50, 150, 10, b_enemy.hp, b_enemy.max_hp, 2, 8);
    
    sprintf(hp_buf, "HP: %d/%d", (int)b_enemy.hp, (int)b_enemy.max_hp);
    kcg_draw_utf8(450, 65, hp_buf, 15, 0);

    if (b_msg[0] != '\0') {
        gfx_fill_rect(10, 180, 620, 40, 0);
        kcg_draw_utf8(30, 190, b_msg, 15, 0);
    }
}

void view_battle_ui(void)
{
    int ch;
    static int auto_cmd_timer = 0;

    if (b_state == B_STATE_CMD_INPUT && b_msg_timer == 0) {
        /* 自動デバッグ: 30フレーム経過で自動攻撃 */
        auto_cmd_timer++;
        if (auto_cmd_timer >= 30) {
            auto_cmd_timer = 0;
            b_player_cmd = 0;
            b_enemy_cmd = (u8)rng_range(0, 3);
            glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
            sprintf(b_msg, "%s attacks! Deals %d dmg.", glue_players[b_pid].name, (int)b_res_p.damage);
            b_msg_timer = 45;
            b_state = B_STATE_RESOLVE_FIRST;
            return;
        }

        ch = api->kbd_trygetchar();
        if (ch == '1') {
            b_player_cmd = 0;
            b_enemy_cmd = (u8)rng_range(0, 3);
            glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
            sprintf(b_msg, "%s attacks! Deals %d dmg.", glue_players[b_pid].name, (int)b_res_p.damage);
            b_msg_timer = 45;
            b_state = B_STATE_RESOLVE_FIRST;
            return;
        }
        else if (ch == '2') {
            b_player_cmd = 3;
            b_enemy_cmd = 0;
            glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
            sprintf(b_msg, "%s defends.", glue_players[b_pid].name);
            b_msg_timer = 45;
            b_state = B_STATE_RESOLVE_FIRST;
            return;
        }
        else if (ch == '3') {
            b_player_cmd = 4;
            b_enemy_cmd = 0;
            glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
            if (b_res_p.result_type == BTL_RES_YIELD) {
                strcpy(b_msg, "Fled successfully!");
                b_state = B_STATE_FINISHED;
            } else {
                strcpy(b_msg, "Failed to flee!");
                b_state = B_STATE_RESOLVE_FIRST;
            }
            b_msg_timer = 45;
            return;
        }

        if (mu_begin_window(ctx, "Battle Commands", mu_rect(20, 240, 260, 140))) {
            
            if (mu_button(ctx, "Attack (Normal)")) {
                b_player_cmd = 0;
                b_enemy_cmd = (u8)rng_range(0, 3);
                
                glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
                
                sprintf(b_msg, "%s attacks! Deals %d dmg.", glue_players[b_pid].name, (int)b_res_p.damage);
                b_msg_timer = 45;
                b_state = B_STATE_RESOLVE_FIRST;
            }
            
            if (mu_button(ctx, "Defend")) {
                b_player_cmd = 3;
                b_enemy_cmd = 0;
                
                glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
                
                sprintf(b_msg, "%s defends.", glue_players[b_pid].name);
                b_msg_timer = 45;
                b_state = B_STATE_RESOLVE_FIRST;
            }
            
            if (mu_button(ctx, "Flee (Yield)")) {
                b_player_cmd = 4;
                b_enemy_cmd = 0;
                
                glue_battle_resolve_turn(&b_player, b_player_cmd, &b_enemy, b_enemy_cmd, &b_res_p, &b_res_e);
                
                if (b_res_p.result_type == BTL_RES_YIELD) {
                    strcpy(b_msg, "Fled successfully!");
                    b_state = B_STATE_FINISHED;
                } else {
                    strcpy(b_msg, "Failed to flee!");
                    b_state = B_STATE_RESOLVE_FIRST;
                }
                b_msg_timer = 45;
            }
            
            mu_end_window(ctx);
        }
    } else {
        if (b_msg_timer == 0) {
            if (b_state == B_STATE_RESOLVE_FIRST) {
                if (b_enemy.hp > 0 && b_player.hp > 0) {
                    sprintf(b_msg, "Monster counter-attacks! Deals %d dmg.", (int)b_res_e.damage);
                    b_msg_timer = 45;
                    b_state = B_STATE_RESOLVE_SECOND;
                } else {
                    if (b_enemy.hp <= 0) {
                        strcpy(b_msg, "Monster defeated!");
                    } else {
                        strcpy(b_msg, "You were defeated...");
                    }
                    b_msg_timer = 45;
                    b_state = B_STATE_FINISHED;
                }
            } else if (b_state == B_STATE_RESOLVE_SECOND) {
                if (b_player.hp <= 0) {
                    strcpy(b_msg, "You were defeated...");
                    b_state = B_STATE_FINISHED;
                } else if (b_enemy.hp <= 0) {
                    strcpy(b_msg, "Monster defeated!");
                    b_state = B_STATE_FINISHED;
                } else {
                    b_msg[0] = '\0';
                    b_state = B_STATE_CMD_INPUT;
                }
                b_msg_timer = 30;
            }
        }
    }
}
