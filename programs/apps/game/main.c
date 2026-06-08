/* ======================================================================== */
/*  MAIN.C — 対戦スゴロクRPG メインエントリ                                  */
/* ======================================================================== */

#include "os32api.h"
#include "libos32gfx.h"
#include "libos32ui.h"
#include "libos32board.h"
#include "libos32math.h"
#include "view_board.h"
#include "view_battle.h"
#include "game_glue.h"
#include <stdio.h>
#include <string.h>

static int roll_dice_for_player(int pid);
static void change_state_to_field(void);

static KernelAPI *api;
static mu_Context ctx;

/* ゲーム状態 */
/* 0: タイトル */
/* 1: フィールド（サイコロ待ち） */
/* 2: プレイヤー移動中 */
/* 3: 戦闘中 */
/* 4: 村経済ポップアップ */
/* 5: ショップ画面 */
/* 6: 宝箱メッセージ表示中 */
static int game_state;

/* サイコロの値 */
static int dice_value;

/* メッセージ表示用のタイマーとバッファ */
static char action_msg[64];
static int action_timer;

/* 直前のプレイヤーの移動状態（移動終了検知用） */
static int was_moving;

static void game_ui(void)
{
    const BoardMass *m;
    const EconEstate *estate;
    char buf[32];
    char cost_buf[32];
    u32 cost;

    (void)m;

    if (mu_begin_window(&ctx, "OS32 Sugoroku RPG", mu_rect(20, 240, 260, 140))) {
        
        if (game_state == 0) {
            mu_label(&ctx, "Welcome to OS32 Sugoroku RPG!");
            if (mu_button(&ctx, "Start Game (M2)")) {
                game_state = 1;
                dice_value = 0;
                was_moving = 0;
                
                /* プレイヤーの位置をリセット */
                glue_players[0].pos = 0;
                view_board_set_player_pos(0, 0);
            }
        } 
        else if (game_state == 1) {
            /* サイコロ待ち */
            if (mu_button(&ctx, "Roll Dice")) {
                dice_value = roll_dice_for_player(0);
                view_board_move_player(0, dice_value);
                game_state = 2; /* 移動アニメーション中へ */
                was_moving = 1;
            }
            
            if (dice_value > 0) {
                sprintf(buf, "Last Dice: %d", dice_value);
                mu_label(&ctx, buf);
            }
            
            if (mu_button(&ctx, "Back to Title")) {
                game_state = 0;
                dice_value = 0;
            }
        } 
        else if (game_state == 7) {
            mu_label(&ctx, "Do you want to transform?");
            mu_label(&ctx, "Penalty: Lose gold, items, villages!");
            if (mu_button(&ctx, "Yes (Transform)")) {
                glue_devil_transform(0);
                strcpy(action_msg, "Transformed into DEVIL!");
                action_timer = 60;
                game_state = 6;
            }
            if (mu_button(&ctx, "No")) {
                change_state_to_field();
            }
        } 
        else if (game_state == 2) {
            mu_label(&ctx, "Moving...");
        }
        else if (game_state == 3) {
            mu_label(&ctx, "In Battle!");
        }
        else if (game_state == 4) {
            /* 村経済ポップアップ */
            m = board_get_mass((u16)glue_players[0].pos);
            estate = econ_estate_get((u16)glue_players[0].pos);
            
            if (estate) {
                if (estate->owner == ECON_OWNER_NONE) {
                    mu_label(&ctx, "Unclaimed Village!");
                    if (mu_button(&ctx, "Claim Village (Free)")) {
                        glue_village_claim(glue_players[0].pos, 0);
                        change_state_to_field();
                    }
                } else if (estate->owner == 0) {
                    cost = econ_estate_invest_cost((u16)glue_players[0].pos);
                    sprintf(cost_buf, "Invest cost: %d G", (int)cost);
                    mu_label(&ctx, cost_buf);
                    
                    if (glue_players[0].gold >= cost) {
                        if (mu_button(&ctx, "Invest (Upgrade)")) {
                            glue_village_invest(glue_players[0].pos, 0);
                            change_state_to_field();
                        }
                    } else {
                        mu_label(&ctx, "Not enough gold.");
                    }
                } else {
                    /* 他のプレイヤーの村 */
                    mu_label(&ctx, "Enemy Village!");
                    if (mu_button(&ctx, "Fight Guard (Capture)")) {
                        view_battle_start(0, 2); /* 守備モンスター(ID=2)と戦闘 */
                        game_state = 3;
                    }
                }
            }
            
            if (mu_button(&ctx, "Cancel (End Turn)")) {
                change_state_to_field();
            }
        }
        else if (game_state == 5) {
            /* ショップ画面 */
            mu_label(&ctx, "Welcome to Shop!");
            
            /* 簡易的にアイテムを買うボタン */
            if (glue_players[0].gold >= 10) {
                if (mu_button(&ctx, "Buy Herb (10 G)")) {
                    inv_shop_buy(&glue_players[0].bag, 1, &glue_players[0].gold);
                }
            }
            if (glue_players[0].gold >= 15) {
                if (mu_button(&ctx, "Buy Antidote (15 G)")) {
                    inv_shop_buy(&glue_players[0].bag, 2, &glue_players[0].gold);
                }
            }
            
            if (mu_button(&ctx, "Exit Shop")) {
                change_state_to_field();
            }
        }
        else if (game_state == 6) {
            mu_label(&ctx, action_msg);
        }
        
        mu_end_window(&ctx);
    }
}

static int roll_dice_for_player(int pid)
{
    int val = (int)rng_range(1, 6);
    int i, extra_dice, extra;

    /* ターンと週数のカウントアップ */
    glue_turn++;
    if (glue_turn % 7 == 0) {
        glue_week++;
        /* デビルクールダウン減算 */
        for (i = 0; i < (int)glue_num_players; i++) {
            if (glue_players[i].devil_cooldown > 0) {
                glue_players[i].devil_cooldown--;
            }
        }
    }

    /* デビルマン移動ボーナス */
    if (glue_players[pid].is_devil) {
        extra_dice = (int)rng_range(1, 5); /* 1〜5個 */
        extra = 0;
        for (i = 0; i < extra_dice; i++) {
            extra += (int)rng_range(1, 6);
        }
        val = val * 2 + extra;
    }

    return val;
}

static void change_state_to_field(void)
{
    char msg[64];
    int released = glue_devil_update(0, msg);
    if (released) {
        strcpy(action_msg, msg);
        action_timer = 60; /* 60フレーム表示 */
        game_state = 6;
    } else {
        game_state = 1;
    }
}

void __cdecl main(int argc, char **argv, KernelAPI *kapi)
{
    int running;
    int ch;
    u32 t0;
    const BoardMass *m;
    int glue_ret;
    const EconEstate *estate;
    u32 cost;
    static int auto_title_timer = 0;
    static int auto_dice_timer = 0;
    static int auto_village_timer = 0;
    static int auto_shop_timer = 0;
    static int auto_transform_timer = 0;
    static int debug_log_timer = 0;
    (void)argc; (void)argv;
    api = kapi;

    api->kprintf(15, "[GAME] Initializing gfx...\n");
    /* GFX初期化 */
    libos32gfx_init(api);
    api->mouse_cursor_set_mode(MOUSE_CURSOR_NONE);

    api->kprintf(15, "[GAME] Initializing microUI...\n");
    /* microUI初期化 */
    mui_init(&ctx, api, 8, 8);

    api->kprintf(15, "[GAME] Initializing RNG...\n");
    /* 乱数初期化 (RTCベース) */
    rng_seed((unsigned int)api->get_tick());

    api->kprintf(15, "[GAME] Initializing view_board...\n");
    /* 盤面描画・移動の初期化 */
    view_board_init(api);
    
    api->kprintf(15, "[GAME] Loading board.db...\n");
    /* board.db をロード */
    if (view_board_load("/db/board.db") != 0) {
        api->kprintf(12, "[GAME] ERROR: failed to load board.db\n");
        api->gfx_shutdown();
        return;
    }

    api->kprintf(15, "[GAME] Initializing game glue...\n");
    /* ゲームエンジン（グルー）初期化 */
    glue_ret = glue_init(api);
    if (glue_ret != 0) {
        api->kprintf(12, "[GAME] ERROR: failed to init game glue (%d)\n", glue_ret);
        api->gfx_shutdown();
        return;
    }

    api->kprintf(15, "[GAME] Initializing view_battle...\n");
    /* 戦闘表示の初期化 */
    view_battle_init(api, &ctx);

    /* 初期状態 */
    game_state = 0;
    dice_value = 0;
    was_moving = 0;

    running = 1;
    
    while (running) {
        /* microUIの入力処理 */
        mui_pump_input(&ctx);

        /* キーボード直接入力とデバッグショートカット */
        ch = api->kbd_trygetchar();

        /* 自動進行処理 */
        debug_log_timer++;
        if (debug_log_timer >= 30) {
            debug_log_timer = 0;
            api->kprintf(15, "[GAME] Loop active. state=%d, pos=%d, hp=%d\n", game_state, (int)glue_players[0].pos, (int)glue_players[0].hp);
        }

        if (game_state == 0) {
            auto_title_timer++;
            if (auto_title_timer >= 30) {
                api->kprintf(15, "[GAME] Auto start game from title.\n");
                auto_title_timer = 0;
                game_state = 1;
                dice_value = 0;
                was_moving = 0;
                glue_players[0].pos = 0;
                view_board_set_player_pos(0, 0);
            }
        } else {
            auto_title_timer = 0;
        }

        if (game_state == 1) {
            auto_dice_timer++;
            if (auto_dice_timer >= 45) {
                auto_dice_timer = 0;
                dice_value = roll_dice_for_player(0);
                api->kprintf(15, "[GAME] Auto roll dice: %d\n", dice_value);
                api->kprintf(15, "[GAME] Calling view_board_move_player...\n");
                view_board_move_player(0, dice_value);
                api->kprintf(15, "[GAME] view_board_move_player returned.\n");
                game_state = 2;
                was_moving = 1;
            }
        } else {
            auto_dice_timer = 0;
        }

        if (game_state == 4) {
            auto_village_timer++;
            if (auto_village_timer >= 45) {
                auto_village_timer = 0;
                estate = econ_estate_get((u16)glue_players[0].pos);
                if (estate) {
                    if (estate->owner == ECON_OWNER_NONE) {
                        glue_village_claim(glue_players[0].pos, 0);
                        change_state_to_field();
                    } else if (estate->owner == 0) {
                        cost = econ_estate_invest_cost((u16)glue_players[0].pos);
                        if (glue_players[0].gold >= cost) {
                            glue_village_invest(glue_players[0].pos, 0);
                        }
                        change_state_to_field();
                    } else {
                        view_battle_start(0, 2);
                        game_state = 3;
                    }
                } else {
                    change_state_to_field();
                }
            }
        } else {
            auto_village_timer = 0;
        }

        if (game_state == 5) {
            auto_shop_timer++;
            if (auto_shop_timer >= 45) {
                auto_shop_timer = 0;
                change_state_to_field();
            }
        } else {
            auto_shop_timer = 0;
        }

        if (game_state == 7) {
            auto_transform_timer++;
            if (auto_transform_timer >= 45) {
                auto_transform_timer = 0;
                glue_devil_transform(0);
                strcpy(action_msg, "Transformed into DEVIL!");
                action_timer = 60;
                game_state = 6;
            }
        } else {
            auto_transform_timer = 0;
        }

        if (ch == 0x1B) {
            running = 0;
        }
        else if (ch > 0) {
            if (game_state == 0) {
                if (ch == '1' || ch == 's' || ch == 'S') {
                    game_state = 1;
                    dice_value = 0;
                    was_moving = 0;
                    glue_players[0].pos = 0;
                    view_board_set_player_pos(0, 0);
                }
            }
            else if (game_state == 1) {
                if (ch == '2' || ch == 'r' || ch == 'R') {
                    dice_value = roll_dice_for_player(0);
                    view_board_move_player(0, dice_value);
                    game_state = 2;
                    was_moving = 1;
                }
            }
            else if (game_state == 4) {
                estate = econ_estate_get((u16)glue_players[0].pos);
                if (estate) {
                    if (estate->owner == ECON_OWNER_NONE) {
                        if (ch == '1') {
                            glue_village_claim(glue_players[0].pos, 0);
                            change_state_to_field();
                        }
                    } else if (estate->owner == 0) {
                        cost = econ_estate_invest_cost((u16)glue_players[0].pos);
                        if (ch == '1' && glue_players[0].gold >= cost) {
                            glue_village_invest(glue_players[0].pos, 0);
                            change_state_to_field();
                        }
                    } else {
                        if (ch == '1') {
                            view_battle_start(0, 2);
                            game_state = 3;
                        }
                    }
                }
                if (ch == '3' || ch == 'c' || ch == 'C') {
                    change_state_to_field();
                }
            }
            else if (game_state == 5) {
                if (ch == '1' && glue_players[0].gold >= 10) {
                    inv_shop_buy(&glue_players[0].bag, 1, &glue_players[0].gold);
                }
                if (ch == '2' && glue_players[0].gold >= 15) {
                    inv_shop_buy(&glue_players[0].bag, 2, &glue_players[0].gold);
                }
                if (ch == '3' || ch == 'e' || ch == 'E') {
                    change_state_to_field();
                }
            }
            else if (game_state == 7) {
                if (ch == '1') {
                    glue_devil_transform(0);
                    strcpy(action_msg, "Transformed into DEVIL!");
                    action_timer = 60;
                    game_state = 6;
                } else if (ch > 0) {
                    change_state_to_field();
                }
            }
        }

        /* プレイヤー移動アニメーションの更新 */
        view_board_update();

        /* 移動完了検知とアクション開始 */
        if (game_state == 2) {
            if (!view_board_is_player_moving(0)) {
                /* 移動完了！ */
                glue_players[0].pos = (u8)view_board_get_player_pos(0);
                
                m = board_get_mass((u16)glue_players[0].pos);
                if (m) {
                    switch (m->type) {
                        case MASS_BATTLE:
                            view_battle_start(0, 1);
                            game_state = 3;
                            break;
                        case MASS_VILLAGE:
                            game_state = 4;
                            break;
                        case MASS_ITEM_SHOP:
                        case MASS_EQUIP_SHOP:
                        case MASS_MAGIC_SHOP:
                            game_state = 5;
                            break;
                        case MASS_CIRCLE:
                            if (!glue_players[0].is_devil && glue_devil_check(0)) {
                                game_state = 7;
                            } else {
                                change_state_to_field();
                            }
                            break;
                        case MASS_CHURCH:
                        case MASS_CASTLE:
                            if (glue_players[0].is_devil) {
                                glue_devil_release(0);
                                strcpy(action_msg, "Devil transformation purified!");
                                action_timer = 60;
                                game_state = 6;
                            } else {
                                change_state_to_field();
                            }
                            break;
                        case MASS_TREASURE:
                            {
                                u16 item_id = inv_lottery(0, 1);
                                const InvItemDef *idef = inv_get_def(item_id);
                                if (idef) {
                                    inv_add(&glue_players[0].bag, item_id, 1);
                                    sprintf(action_msg, "Got item: %s!", idef->name);
                                } else {
                                    strcpy(action_msg, "Found empty chest.");
                                }
                                action_timer = 60;
                                game_state = 6;
                            }
                            break;
                        default:
                            change_state_to_field();
                            break;
                    }
                } else {
                    change_state_to_field();
                }
                was_moving = 0;
            }
        }
        else if (game_state == 3) {
            if (view_battle_update() == 0) {
                m = board_get_mass((u16)glue_players[0].pos);
                if (glue_players[0].hp > 0) {
                    if (m && m->type == MASS_VILLAGE) {
                        glue_village_claim(glue_players[0].pos, 0);
                    }
                } else {
                    if (glue_players[0].is_devil) {
                        glue_devil_release(0);
                    }
                    glue_players[0].pos = 0;
                    glue_players[0].hp = glue_players[0].max_hp;
                    view_board_set_player_pos(0, 0);
                }
                change_state_to_field();
            }
        }
        else if (game_state == 6) {
            if (action_timer > 0) {
                action_timer--;
            } else {
                change_state_to_field();
            }
        }

        /* microUI 構築 */
        mu_begin(&ctx);
        game_ui();
        if (game_state == 3) {
            view_battle_ui();
        }
        mu_end(&ctx);

        /* 描画クリア (黒) */
        gfx_clear(0);

        /* 描画 */
        if (game_state == 3) {
            view_battle_draw();
        } else {
            if (game_state != 0) {
                view_board_draw();
                
                {
                    char stat_buf[128];
                    char name_buf[64];
                    if (glue_players[0].is_devil) {
                        sprintf(name_buf, "%s (DEVIL)", glue_players[0].name);
                    } else {
                        strcpy(name_buf, glue_players[0].name);
                    }
                    sprintf(stat_buf, "Player: %s | HP: %d/%d | Gold: %d G | Pos: %d | Week: %d", 
                                 name_buf, (int)glue_players[0].hp, (int)glue_players[0].max_hp, 
                                 (int)glue_players[0].gold, glue_players[0].pos, glue_week);
                    kcg_draw_utf8(20, 15, stat_buf, 15, 0);
                }
            } else {
                kcg_draw_utf8(160, 40, "OS32 SUGOROKU RPG - Milestone 2", 15, 0);
                kcg_draw_utf8(160, 160, "Press [Start Game (M2)] to play", 12, 0);
            }
        }

        /* microUIの描画 */
        mui_render(&ctx);

        /* VRAM転送 */
        api->gfx_add_dirty_rect(0, 0, 640, 400);
        api->gfx_present_dirty();

        /* ~30fps制御 */
        t0 = api->get_tick();
        while (api->get_tick() - t0 < 3) {
            /* 空ループ */
        }
    }

    /* シャットダウン */
    glue_shutdown();
    board_shutdown();
    api->gfx_shutdown();
    api->tvram_clear();
}
