/* ======================================================================== */
/*  MAIN.C — 対戦スゴロクRPG メインエントリ                                  */
/* ======================================================================== */

#include "os32api.h"
#include "libos32gfx.h"
#include "libos32board.h"
#include "libos32math.h"
#include "view_board.h"
#include "view_battle.h"
#include "view_tiles.h"
#include "view_panel.h"
#include "view_export.h"
#include "game_sound.h"
#include "libos32tilemap.h"
#include "game_glue.h"
#include <stdio.h>
#include <string.h>

static KernelAPI *api;

/* ゲーム状態 */
#define ST_TITLE     0
#define ST_DICE      1   /* サイコロ待ち */
#define ST_MOVING    2   /* 移動アニメーション中 */
#define ST_BATTLE    3
#define ST_VILLAGE   4   /* 村経済ポップアップ */
#define ST_SHOP      5
#define ST_MESSAGE   6   /* メッセージ表示中 */
#define ST_TRANSFORM 7   /* デビル変身の確認 */
#define ST_BRANCH    8   /* 分岐選択待ち */
#define ST_COLLECT   9   /* 集金所: 統治村を選んで集金/投資 */
#define ST_CASTLE   10   /* 城: ボス街道への挑戦確認 */
#define ST_RESULT   11   /* 決着: 勝者と最終順位の表示 */
#define ST_SELL     12   /* 店: 持ち物を売る */
#define ST_INVENTORY 13  /* 持ち物と装備の確認・付け替え */
#define ST_DUNGEON  14   /* ダンジョン: 降りるか撤退するかの選択 */

static int game_state;

/* 直前に開始した戦闘がダンジョン内なら 1
   (勝てば潜行を続け、負ければ戦利品を失う) */
static int pending_dungeon = 0;
/* ダンジョンで直前に起きた出来事の説明 */
static char dungeon_msg[160];

/* 店の品揃え (マス種別に応じて items.db から引く) */
#define SHOP_MAX_LINEUP 8
static u16 shop_ids[SHOP_MAX_LINEUP];
static int shop_count;
static int shop_sel;
static u8  shop_type;      /* 0=装備屋, 1=道具屋, 2=言霊屋 */

/* 持ち物一覧 (売却・装備で共用)。値はバッグのスロット番号 */
static u8  bag_slots[GLUE_MAX_ITEMS];
static int bag_count;
static int bag_sel;

/* ST_INVENTORY から戻る先 (店から開いたら店へ戻る) */
static int inv_return_state;

/* 直前に開始した戦闘が城ボス戦なら 1 (勝利時に glue_boss_defeated を呼ぶ) */
static int pending_boss = 0;

/* サイコロの値 */
static int dice_value;

/* メッセージ表示用のタイマーとバッファ。
   msg_return_state はメッセージを閉じた後に戻る状態 (-1 = ST_DICE)。
   店や持ち物画面から出したメッセージで手番が終わらないようにする */
static char action_msg[160];
static int action_timer;
static int msg_return_state = -1;

/* 集金所で選択中の統治村 */
static u16 collect_ids[GLUE_MAX_OWNED];
static int collect_count;
static int collect_sel;

/* CPU の思考待ちフレーム数 (人間の目に追える速度で進めるための間) */
#define CPU_THINK_FRAMES 24
static int cpu_timer;

static void end_turn(void);
static void show_message(const char *msg, int frames);
static int  roll_dice_for_player(int pid);
static void arrive_at_mass(int pid);

/* バッグの中身 (空きスロットを除く) を bag_slots に集める */
static void refresh_bag_list(int pid)
{
    const InvBag *bag = &glue_players[pid].bag;
    int i;

    bag_count = 0;
    for (i = 0; i < (int)bag->max_slots && bag_count < GLUE_MAX_ITEMS; i++) {
        const InvSlot *s = inv_get_slot(bag, (u8)i);
        if (s && s->item_id != 0) bag_slots[bag_count++] = (u8)i;
    }
    if (bag_sel >= bag_count) bag_sel = 0;
}

/* 止まったマスに応じた店の品揃えを引く。
   ステージはマスの area (= board.db の area) をそのまま使う */
static void open_shop(int pid)
{
    const BoardMass *m = board_get_mass(glue_players[pid].pos);
    u8 stage = m ? (u8)m->area : 1;

    if (!m) { shop_count = 0; return; }
    switch (m->type) {
    case MASS_EQUIP_SHOP: shop_type = 0; break;
    case MASS_MAGIC_SHOP: shop_type = 2; break;
    default:              shop_type = 1; break;
    }
    if (stage < 1) stage = 1;
    shop_count = inv_shop_list(shop_type, stage, shop_ids, SHOP_MAX_LINEUP);
    shop_sel = 0;
}

/* ダンジョンを1階降りる。戦闘になったら ST_BATTLE へ移る。
   毒などで倒れたら潜行は失敗し、振り出しへ戻る */
static void dungeon_step(int pid)
{
    u16 enemy = 0;
    int what = glue_dungeon_descend(pid, &enemy, dungeon_msg);

    if (what == GLUE_DFLOOR_BATTLE) {
        pending_dungeon = 1;
        view_battle_start(pid, enemy);
        game_state = ST_BATTLE;
        return;
    }

    if (what == GLUE_DFLOOR_EXIT) {
        u32 loot = glue_dungeon_escape(pid);
        char buf[80];
        sprintf(buf, "最奥から %d G を持ち帰った！", (int)loot);
        show_message(buf, 70);
        end_turn();   /* 迷宮を出たら手番を終える */
        return;
    }

    /* 罠で倒れたら戦利品を失って振り出しへ */
    if (glue_players[pid].actor.hp <= 0) {
        glue_dungeon_fail(pid);
        glue_players[pid].actor.hp = glue_players[pid].actor.max_hp;
        glue_players[pid].pos = 0;
        view_board_set_player_pos(pid, 0);
        show_message("闇の中で倒れた。戦利品を失った", 70);
        end_turn();
        return;
    }

    game_state = ST_DUNGEON;
    cpu_timer = 0;
}

/* 店の看板 */
static const char *shop_name(void)
{
    switch (shop_type) {
    case 0:  return "武具屋";
    case 2:  return "言霊屋";
    default: return "道具屋";
    }
}

/* 装備スロットの表示名 */
static const char *eslot_name(int slot)
{
    switch (slot) {
    case INV_ESLOT_WEAPON: return "武器";
    case INV_ESLOT_SHIELD: return "盾";
    case INV_ESLOT_ARMOR:  return "鎧";
    default:               return "装飾";
    }
}

/* ====================================================================== */
/*  漢字変換表の有効化                                                     */
/*                                                                        */
/*  カーネルは /sys/unicode.bin を MEM_UNICODE_TABLE_BASE へ読み込むが、    */
/*  「読み込み済み」フラグは lib/utf8.c の static 変数で、外部プログラムは  */
/*  自分の lib/utf8.c をリンクするため別の実体になる。こちらで有効化       */
/*  しないと unicode_to_jis() が常に 0 を返し、漢字が全部 □ になる。       */
/*                                                                        */
/*  ただし無条件に有効化すると、表のロードに失敗していた場合に             */
/*  0x4A000 に残っていた別のデータを変換表として読んでしまう。             */
/*  そこで既知の対応をいくつか確かめてから有効にする。                     */
/* ====================================================================== */
extern void utf8_set_jis_table_ready(int ready);
extern u16 unicode_to_jis(u32 cp);

static int enable_kanji_table(void)
{
    /* (Unicode, JIS X 0208) の既知の対応。EUC-JP から機械的に導いた値 */
    static const u32 probe_cp[4]  = { 0x4E9C, 0x4E00, 0x5BFE, 0x9078 };
    static const u16 probe_jis[4] = { 0x3021, 0x306C, 0x4250, 0x412A };
    int i;

    utf8_set_jis_table_ready(1);
    for (i = 0; i < 4; i++) {
        if (unicode_to_jis(probe_cp[i]) != probe_jis[i]) {
            utf8_set_jis_table_ready(0);
            return 0;
        }
    }
    return 1;
}

/* 現在の手番プレイヤー */
static int cur(void)
{
    return (int)glue_current_player;
}

static int cur_is_cpu(void)
{
    return glue_players[cur()].is_cpu ? 1 : 0;
}

static void show_message(const char *msg, int frames)
{
    int n = (int)strlen(msg);

    /* あふれたら切るが、UTF-8 の途中では切らない。
       後続バイト (10xxxxxx) の上に落ちたら文字の頭まで戻す
       (view_panel.c の add_line と同じ考え方) */
    if (n > (int)sizeof(action_msg) - 1) {
        n = (int)sizeof(action_msg) - 1;
        while (n > 0 && ((unsigned char)msg[n] & 0xC0) == 0x80) n--;
    }
    memcpy(action_msg, msg, (size_t)n);
    action_msg[n] = '\0';
    action_timer = frames;
    game_state = ST_MESSAGE;
}

/* ====================================================================== */
/*  手番の終了                                                             */
/*                                                                        */
/*  1手番につき必ず1回だけ呼ぶこと。デビル変身の残ターン消化と、           */
/*  libos32turn による手番/週の進行をここに集約している。                  */
/* ====================================================================== */
static void end_turn(void)
{
    char msg[160];
    int crossed = 0;
    int c = cur();

    /* 状態異常の進行 (毒ダメージ・自然回復) */
    {
        int dmg = glue_status_tick(c);
        if (dmg > 0 && game_state != ST_MESSAGE) {
            sprintf(msg, "%s は毒で %d のダメージ",
                    glue_players[c].name, dmg);
            show_message(msg, 45);
        }
    }

    /* 毒で倒れたら振り出しへ戻す */
    if (glue_players[c].actor.hp <= 0) {
        if (glue_players[c].is_devil) glue_devil_release(c);
        glue_players[c].pos = 0;
        glue_players[c].actor.hp = glue_players[c].actor.max_hp;
        view_board_set_player_pos(c, 0);
    }

    /* デビル変身の残ターン処理 (解除されたらメッセージを出す) */
    if (glue_devil_update(c, msg) && game_state != ST_MESSAGE) {
        show_message(msg, 60);
    }

    glue_turn_advance(&crossed);

    /* イベントは毎ターン起こりうる。週境界では上納金も通知する。
       同時に起きたらイベントの方を優先する */
    if (game_state != ST_MESSAGE) {
        if (glue_event_msg[0] != '\0') {
            show_message(glue_event_msg, 90);
        } else if (crossed) {
            int i;
            for (i = 0; i < (int)glue_num_players; i++) {
                if (!glue_players[i].is_cpu && glue_last_income[i] > 0) {
                    sprintf(msg, "第%d週の収入 %d G",
                            (int)glue_week, (int)glue_last_income[i]);
                    show_message(msg, 60);
                    break;
                }
            }
        }
    }

    /* 勝利判定 (征服王は戦闘側で確定済み、ここで制覇王/資産王も見る)。
       決着したらメッセージ表示より優先してリザルトへ */
    if (glue_victory_check()) {
        game_state = ST_RESULT;
    } else if (game_state != ST_MESSAGE) {
        game_state = ST_DICE;
    }
    dice_value = 0;
    cpu_timer = 0;

    /* 村の所有権などがこの手番で変わっていても盤面 (BG1 の施設) に
       反映されるように、描画キャッシュを無効化する */
    view_board_invalidate();
}

static int roll_dice_for_player(int pid)
{
    int val = (int)rng_range(1, 6);
    int i, extra_dice, extra;

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

/* サイコロを振って移動を開始する */
static void start_move(int pid)
{
    game_sound_se(GS_SE_DICE);
    dice_value = roll_dice_for_player(pid);
    view_board_move_player(pid, dice_value);
    game_state = ST_MOVING;
    cpu_timer = 0;
}

/* ====================================================================== */
/*  マス到達時の処理                                                       */
/* ====================================================================== */
static void arrive_at_mass(int pid)
{
    const BoardMass *m;

    glue_players[pid].pos = (u16)view_board_get_player_pos(pid);
    m = board_get_mass(glue_players[pid].pos);

    if (!m) {
        end_turn();
        return;
    }

    /* 止まったマスの地形効果 (雪原=マヒ、毒沼=毒) */
    {
        u32 st = glue_apply_terrain(pid, m->terrain, 1);
        if (st == GLUE_ST_PARALYZE) {
            char buf[64];
            sprintf(buf, "%s は凍りついた！ 次の手番を飛ばす",
                    glue_players[pid].name);
            show_message(buf, 60);
            end_turn();
            return;
        }
    }

    switch (m->type) {
        case MASS_BATTLE:
            view_battle_start(pid, glue_enemy_pick_for_mass(glue_players[pid].pos));
            game_state = ST_BATTLE;
            break;


        case MASS_VILLAGE:
            game_state = ST_VILLAGE;
            cpu_timer = 0;
            break;

        case MASS_ITEM_SHOP:
        case MASS_EQUIP_SHOP:
        case MASS_MAGIC_SHOP:
            open_shop(pid);
            game_state = ST_SHOP;
            cpu_timer = 0;
            break;

        case MASS_CIRCLE:
            if (!glue_players[pid].is_devil && glue_devil_check(pid)) {
                game_state = ST_TRANSFORM;
                cpu_timer = 0;
            } else {
                end_turn();
            }
            break;

        case MASS_CHURCH:
            if (glue_players[pid].is_devil) {
                glue_devil_release(pid);
                show_message("デビルの力が祓われた", 60);
            }
            end_turn();
            break;

        case MASS_CASTLE:
            /* 城は祓いの場でもある (デビルはまず解除) */
            if (glue_players[pid].is_devil) {
                glue_devil_release(pid);
                show_message("デビルの力が祓われた", 60);
                end_turn();
                break;
            }
            if (glue_castle_boss() != 0) {
                /* ボス街道: 現在の段のボスへの挑戦を選べる */
                game_state = ST_CASTLE;
                cpu_timer = 0;
            } else {
                show_message("城は静まり返っている", 45);
                end_turn();
            }
            break;

        case MASS_DUNGEON:
            glue_dungeon_enter(pid);
            strcpy(dungeon_msg, "暗い入口が口を開けている");
            game_state = ST_DUNGEON;
            cpu_timer = 0;
            break;

        case MASS_COLLECT:
            collect_count = glue_collect_list(pid, collect_ids, GLUE_MAX_OWNED);
            collect_sel = 0;
            if (collect_count == 0) {
                show_message("集金できる村がない", 45);
                end_turn();
            } else {
                game_state = ST_COLLECT;
                cpu_timer = 0;
            }
            break;

        case MASS_TREASURE: {
            u16 item_id = inv_lottery(0, 1);
            const InvItemDef *idef = inv_get_def(item_id);
            char buf[64];
            if (idef) {
                inv_add(&glue_players[pid].bag, item_id, 1);
                game_sound_se(GS_SE_ITEM);
                sprintf(buf, "%s は %s を手に入れた！", glue_players[pid].name, idef->name);
            } else {
                strcpy(buf, "箱は空だった");
            }
            show_message(buf, 60);
            end_turn();
            break;
        }

        default:
            end_turn();
            break;
    }
}

/* ====================================================================== */
/*  村マスの処理                                                           */
/* ====================================================================== */

/* 村での行動を実行する。act: 0=統治/投資, 1=守備と戦闘, 2=何もしない */
static void do_village_action(int pid, int act)
{
    const EconEstate *estate = glue_village_estate(glue_players[pid].pos);
    u32 cost;

    if (!estate || act == 2) {
        end_turn();
        return;
    }

    if (estate->owner == ECON_OWNER_NONE) {
        glue_village_claim(glue_players[pid].pos, pid);
        end_turn();
    } else if (estate->owner == (u8)pid) {
        cost = glue_village_invest_cost(glue_players[pid].pos);
        if (glue_players[pid].gold >= cost) {
            glue_village_invest(glue_players[pid].pos, pid);
        }
        end_turn();
    } else {
        if (act == 1) {
            view_battle_start(pid, glue_enemy_village_guard(glue_players[pid].pos));
            game_state = ST_BATTLE;
        } else {
            end_turn();
        }
    }
}

/* CPU の村での判断 */
static void cpu_village_turn(int pid)
{
    const EconEstate *estate = glue_village_estate(glue_players[pid].pos);

    if (!estate) {
        end_turn();
        return;
    }

    if (estate->owner != ECON_OWNER_NONE && estate->owner != (u8)pid) {
        /* 他人の村。HP に余裕があれば攻め込む */
        if (glue_players[pid].actor.hp * 2 > glue_players[pid].actor.max_hp) {
            do_village_action(pid, 1);
        } else {
            do_village_action(pid, 2);
        }
    } else {
        do_village_action(pid, 0);
    }
}

/* ====================================================================== */
/*  下段パネルの構築 (固定ウィンドウ方式)                                   */
/*                                                                        */
/*  microUI のフローティングウィンドウは残像と再描画管理の温床だったので    */
/*  やめた。マウスも使わないので、操作は全てキー表示の行テキストで案内する。 */
/*  描画は view_panel が差分検知して必要なときだけ行う。                    */
/* ====================================================================== */
static void build_panel(void)
{
    const EconEstate *estate;
    char buf[192];
    int c = cur();

    if (game_state == ST_BATTLE) {
        view_battle_panel();
        return;
    }

    switch (game_state) {
    case ST_TITLE:
        panel_line_hi("OS32 対戦スゴロクRPG へようこそ");
        panel_line("1: はじめから    L: つづきから");
        break;

    case ST_DICE:
        sprintf(buf, "%d ターン ／ 第%d週 (%s曜)", (int)glue_turn,
                (int)glue_week, glue_day_name());
        panel_line(buf);
        sprintf(buf, "手番: %s%s", glue_players[c].name,
                glue_players[c].is_cpu ? " (CPU)" : "");
        panel_line_hi(buf);
        if (!glue_players[c].is_cpu) {
            sprintf(buf, "2: サイコロ  I: 持ち物  W: 記録  M: 音 %s",
                    game_sound_enabled() ? "ON" : "OFF");
            panel_line(buf);
        } else {
            panel_line("CPU が考えている……");
        }
        if (dice_value > 0) {
            sprintf(buf, "出目: %d", dice_value);
            panel_line(buf);
        }
        break;

    case ST_MOVING:
        panel_line("移動中……");
        break;

    case ST_BRANCH: {
        int opts[VIEW_BOARD_MAX_BRANCH];
        int n = view_board_branch_options(c, opts, VIEW_BOARD_MAX_BRANCH);
        int i;
        sprintf(buf, "進む道を選べ (残り %d マス)",
                view_board_remaining_steps(c));
        panel_line_hi(buf);
        if (!glue_players[c].is_cpu) {
            for (i = 0; i < n; i++) {
                const BoardMass *bm = board_get_mass((u16)opts[i]);
                sprintf(buf, "%d: %d 番のマスへ%s", i + 1, opts[i],
                        (bm && bm->type == MASS_VILLAGE) ? " (村)" : "");
                panel_line(buf);
            }
        } else {
            panel_line("CPU が選んでいる……");
        }
        break;
    }

    case ST_VILLAGE:
        estate = glue_village_estate(glue_players[c].pos);
        if (!estate) {
            panel_line("村の記録がない");
        } else if (estate->owner == ECON_OWNER_NONE) {
            panel_line_hi("どこにも属さぬ村だ");
            if (!glue_players[c].is_cpu) {
                panel_line("1: 統治する (無償)    3: 手番を終える");
            }
        } else if (estate->owner == (u8)c) {
            u32 cost = glue_village_invest_cost(glue_players[c].pos);
            sprintf(buf, "自分の村。投資に %d G", (int)cost);
            panel_line_hi(buf);
            if (!glue_players[c].is_cpu) {
                if (glue_players[c].gold >= cost) {
                    panel_line("1: 投資する    3: 手番を終える");
                } else {
                    panel_line("所持金が足りない    3: 手番を終える");
                }
            }
        } else {
            panel_line_hi("他人の村だ");
            if (!glue_players[c].is_cpu) {
                panel_line("1: 守備と戦う (奪う)    3: 手番を終える");
            }
        }
        if (glue_players[c].is_cpu) {
            panel_line("CPU が考えている……");
        }
        break;

    case ST_COLLECT: {
        int i;
        sprintf(buf, "集金所  (統治村 %d)", collect_count);
        panel_line_hi(buf);
        if (!glue_players[c].is_cpu) {
            char row[80];
            row[0] = '\0';
            for (i = 0; i < collect_count && i < 4; i++) {
                const EconEstate *e = econ_estate_get(collect_ids[i]);
                char one[24];
                if (!e) continue;
                sprintf(one, "%c%d番 Lv%d(%dG) ",
                        (i == collect_sel) ? '>' : ' ',
                        (int)collect_ids[i], (int)e->level, (int)e->tax);
                if (strlen(row) + strlen(one) < sizeof(row)) {
                    strcat(row, one);
                }
            }
            panel_line(row);
            if (collect_count > 0) {
                u32 cost = glue_invest_cost(collect_ids[collect_sel]);
                sprintf(buf,
                        "N: 次へ  1: 集金  2: 投資 (%d G)  3: 出る",
                        (int)cost);
                panel_line(buf);
            }
        } else {
            panel_line("CPU が集金している……");
        }
        break;
    }

    case ST_SHOP: {
        u16 scale = glue_shop_price_scale();

        if (scale == 0) {
            sprintf(buf, "%s は本日休業 (%s曜)",
                    shop_name(), glue_day_name());
            panel_line_hi(buf);
            if (!glue_players[c].is_cpu) panel_line("3: 出る");
            break;
        }

        if (scale < 100) {
            sprintf(buf, "%s ／ %s曜の大安売り %d%%引き ／ 所持金 %d G",
                    shop_name(), glue_day_name(), (int)(100 - scale),
                    (int)glue_players[c].gold);
        } else {
            sprintf(buf, "%s ／ %s曜 ／ 所持金 %d G", shop_name(),
                    glue_day_name(), (int)glue_players[c].gold);
        }
        panel_line_hi(buf);

        if (glue_players[c].is_cpu) {
            panel_line("CPU が買い物をしている……");
            break;
        }

        if (shop_count == 0) {
            panel_line("品切れのようだ");
        } else {
            int i;
            char row[80];
            const InvItemDef *d = inv_get_def(shop_ids[shop_sel]);
            row[0] = '\0';
            for (i = 0; i < shop_count; i++) {
                const InvItemDef *e = inv_get_def(shop_ids[i]);
                char one[28];
                if (!e) continue;
                sprintf(one, "%c%s ", (i == shop_sel) ? '>' : ' ', e->name);
                if (strlen(row) + strlen(one) < sizeof(row)) strcat(row, one);
                if ((i % 4) == 3) { panel_line(row); row[0] = '\0'; }
            }
            if (row[0]) panel_line(row);
            if (d) {
                if (d->type == INV_TYPE_CONSUMABLE) {
                    sprintf(buf, "%s ／ %d G  (効果 %d/%d)",
                            d->name, (int)inv_shop_buy_price(d->id),
                            (int)d->effect, (int)d->param);
                } else {
                    sprintf(buf, "%s ／ %d G  %s %+d  [%s]",
                            d->name, (int)inv_shop_buy_price(d->id),
                            (d->stat_type == INV_STAT_ATK) ? "ATK" :
                            (d->stat_type == INV_STAT_DEF) ? "DEF" :
                            (d->stat_type == INV_STAT_SPD) ? "SPD" : "MAG",
                            (int)d->stat_bonus, eslot_name(d->equip_slot));
                }
                panel_line(buf);
            }
        }
        panel_line("N: 次へ  1: 買う  2: 売る  4: 持ち物  3: 出る");
        break;
    }

    case ST_SELL: {
        int i;
        char row[80];
        sprintf(buf, "売却 ／ 所持金 %d G", (int)glue_players[c].gold);
        panel_line_hi(buf);
        if (bag_count == 0) {
            panel_line("売れる物がない");
        } else {
            const InvSlot *s;
            const InvItemDef *d;
            row[0] = '\0';
            for (i = 0; i < bag_count; i++) {
                const InvSlot *si = inv_get_slot(&glue_players[c].bag,
                                                 bag_slots[i]);
                const InvItemDef *di = si ? inv_get_def(si->item_id) : 0;
                char one[28];
                if (!di) continue;
                sprintf(one, "%c%s ", (i == bag_sel) ? '>' : ' ', di->name);
                if (strlen(row) + strlen(one) < sizeof(row)) strcat(row, one);
                if ((i % 4) == 3) { panel_line(row); row[0] = '\0'; }
            }
            if (row[0]) panel_line(row);
            s = inv_get_slot(&glue_players[c].bag, bag_slots[bag_sel]);
            d = s ? inv_get_def(s->item_id) : 0;
            if (d) {
                sprintf(buf, "%s x%d ／ %d G で売れる",
                        d->name, (int)s->count,
                        (int)inv_get_sell_price(d->id));
                panel_line(buf);
            }
        }
        panel_line("N: 次へ  1: 売る  3: 戻る");
        break;
    }

    case ST_INVENTORY: {
        const InvBag *bag = &glue_players[c].bag;
        int i;
        char row[80];

        sprintf(buf, "持ち物と装備 ／ %s", glue_players[c].name);
        panel_line_hi(buf);

        /* 装備中の一覧 */
        row[0] = '\0';
        for (i = 0; i < (int)bag->equip_count; i++) {
            const InvSlot *e = &bag->equip[i];
            const InvItemDef *d = (e->item_id) ? inv_get_def(e->item_id) : 0;
            char one[32];
            sprintf(one, "%s:%s ", eslot_name(i), d ? d->name : "-");
            if (strlen(row) + strlen(one) < sizeof(row)) strcat(row, one);
        }
        panel_line(row);

        if (bag_count == 0) {
            panel_line("持ち物は空だ");
        } else {
            const InvSlot *s;
            const InvItemDef *d;
            row[0] = '\0';
            for (i = 0; i < bag_count; i++) {
                const InvSlot *si = inv_get_slot(bag, bag_slots[i]);
                const InvItemDef *di = si ? inv_get_def(si->item_id) : 0;
                char one[28];
                if (!di) continue;
                sprintf(one, "%c%s ", (i == bag_sel) ? '>' : ' ', di->name);
                if (strlen(row) + strlen(one) < sizeof(row)) strcat(row, one);
                if ((i % 4) == 3) { panel_line(row); row[0] = '\0'; }
            }
            if (row[0]) panel_line(row);
            s = inv_get_slot(bag, bag_slots[bag_sel]);
            d = s ? inv_get_def(s->item_id) : 0;
            if (d) {
                if (inv_is_equipment(d->id)) {
                    sprintf(buf, "%s  %s %+d  [%s]   1: 装備する",
                            d->name,
                            (d->stat_type == INV_STAT_ATK) ? "ATK" :
                            (d->stat_type == INV_STAT_DEF) ? "DEF" :
                            (d->stat_type == INV_STAT_SPD) ? "SPD" : "MAG",
                            (int)d->stat_bonus, eslot_name(d->equip_slot));
                } else {
                    sprintf(buf, "%s x%d   1: 使う", d->name, (int)s->count);
                }
                panel_line(buf);
            }
        }
        panel_line("N: 次へ  1: 装備/使用  3: 戻る");
        break;
    }

    case ST_TRANSFORM:
        panel_line_hi("デビルに変身するか？");
        panel_line("代償: 所持金・持ち物・統治村を失う");
        if (!glue_players[c].is_cpu) {
            panel_line("1: 変身する    他のキー: やめる");
        }
        break;

    case ST_CASTLE: {
        const GlueEnemy *e = glue_enemy_get(glue_castle_boss());
        sprintf(buf, "城 ／ 魔王街道 %d の段 (全8段)", (int)glue_boss_progress);
        panel_line_hi(buf);
        if (e) {
            sprintf(buf, "%s が待ち構える (HP %d ／ 攻 %d)",
                    e->name, (int)e->max_hp, (int)e->atk);
            panel_line(buf);
        }
        if (!glue_players[c].is_cpu) {
            panel_line("1: 挑む    3: 立ち去る");
        } else {
            panel_line("CPU が考えている……");
        }
        break;
    }

    case ST_DUNGEON:
        sprintf(buf, "迷宮 地下%d階 ／ 戦利品 %d G ／ HP %d/%d",
                glue_dungeon_floor, (int)glue_dungeon_loot,
                (int)glue_players[c].actor.hp,
                (int)glue_players[c].actor.max_hp);
        panel_line_hi(buf);
        if (dungeon_msg[0]) panel_line(dungeon_msg);
        if (!glue_players[c].is_cpu) {
            panel_line("1: さらに潜る    3: 戦利品を持って戻る");
        } else {
            panel_line("CPU が探索している……");
        }
        break;

    case ST_RESULT:
        sprintf(buf, "決着！ %s が%sとなった",
                (glue_victory_winner >= 0)
                    ? glue_players[glue_victory_winner].name : "?",
                glue_victory_name(glue_victory_type));
        panel_line_hi(buf);
        panel_line("1: タイトルへ戻る");
        break;

    case ST_MESSAGE:
        panel_line(action_msg);
        break;

    default:
        break;
    }
}

/* ====================================================================== */
/*  キーボード操作                                                         */
/* ====================================================================== */
static void handle_key(int ch)
{
    int c = cur();

    if (ch <= 0) return;

    /* CPU の手番中は人間の入力を受け付けない。
       ただしタイトルとリザルトは手番と無関係な画面なので通す
       (CPU の手番中に決着するとリザルトから抜けられなくなる) */
    if (glue_players[c].is_cpu &&
        game_state != ST_TITLE && game_state != ST_RESULT) return;

    if (game_state == ST_BATTLE) {
        view_battle_handle_key(ch);
        return;
    }

    switch (game_state) {
        case ST_TITLE:
            if (ch == '1' || ch == 's' || ch == 'S') {
                int i;
                game_state = ST_DICE;
                dice_value = 0;
                for (i = 0; i < (int)glue_num_players; i++) {
                    glue_players[i].pos = 0;
                    view_board_set_player_pos(i, 0);
                }
                view_board_set_focus(0);
            } else if (ch == 'l' || ch == 'L') {
                if (glue_load(GLUE_SAVE_PATH) == 0) {
                    int i;
                    for (i = 0; i < (int)glue_num_players; i++) {
                        view_board_set_player_pos(i, glue_players[i].pos);
                    }
                    /* カメラを手番プレイヤーへ寄せ、村の所有権が変わった
                       盤面 (BG1 の施設) も描き直させる */
                    view_board_set_focus(glue_players[cur()].pos);
                    view_board_invalidate();
                    show_message("つづきから始める", 45);
                    msg_return_state = ST_DICE;
                } else {
                    show_message("記録がない", 45);
                    msg_return_state = ST_TITLE;
                }
            }
            break;

        case ST_DICE:
            if (ch == '2' || ch == 'r' || ch == 'R') {
                start_move(c);
            } else if (ch == 'w' || ch == 'W') {
                show_message(glue_save(GLUE_SAVE_PATH) == 0
                             ? "記録した" : "記録できなかった", 45);
            } else if (ch == 'i' || ch == 'I') {
                refresh_bag_list(c);
                inv_return_state = ST_DICE;
                game_state = ST_INVENTORY;
            } else if (ch == 'm' || ch == 'M') {
                show_message(game_sound_toggle() ? "音を出す" : "音を消す",
                             30);
                msg_return_state = ST_DICE;
            } else if (ch == 'p' || ch == 'P') {
                /* デバッグ: 次の店 (道具/装備/言霊) へワープする。
                   店は散らばっていて狙って止まるのが難しいため */
                int n = board_mass_count();
                int k;
                for (k = 1; k <= n; k++) {
                    const BoardMass *m =
                        board_get_mass_at((glue_players[c].pos + k) % n);
                    if (m && (m->type == MASS_ITEM_SHOP ||
                              m->type == MASS_EQUIP_SHOP ||
                              m->type == MASS_MAGIC_SHOP)) {
                        glue_players[c].pos = m->id;
                        view_board_set_player_pos(c, m->id);
                        view_board_set_focus(m->id);
                        arrive_at_mass(c);
                        break;
                    }
                }
            } else if (ch == 'c' || ch == 'C') {
                /* デバッグ: 輪マス (デビル変身) へワープする。
                   輪マスは盤上に1つ (北海道) しかなく、通常プレイでは
                   まず踏めないため変身まわりの検証に使う */
                int n = board_mass_count();
                int k;
                for (k = 0; k < n; k++) {
                    const BoardMass *m = board_get_mass_at(k);
                    if (m && m->type == MASS_CIRCLE) {
                        glue_players[c].pos = m->id;
                        view_board_set_player_pos(c, m->id);
                        view_board_set_focus(m->id);
                        arrive_at_mass(c);
                        break;
                    }
                }
            } else if (ch == 'd' || ch == 'D') {
                /* デバッグ: 次のダンジョン入口へワープする (全4ヶ所) */
                int n = board_mass_count();
                int k;
                for (k = 1; k <= n; k++) {
                    const BoardMass *m =
                        board_get_mass_at((glue_players[c].pos + k) % n);
                    if (m && m->type == MASS_DUNGEON) {
                        glue_players[c].pos = m->id;
                        view_board_set_player_pos(c, m->id);
                        view_board_set_focus(m->id);
                        arrive_at_mass(c);
                        break;
                    }
                }
            } else if (ch == 'b' || ch == 'B') {
                /* デバッグ: 城へワープ (ボス街道の検証用)。
                   城は1マスしかなく、狙って止まるのが難しいため */
                int n = board_mass_count();
                int k;
                for (k = 0; k < n; k++) {
                    const BoardMass *m = board_get_mass_at(k);
                    if (m && m->type == MASS_CASTLE) {
                        glue_players[c].pos = m->id;
                        view_board_set_player_pos(c, m->id);
                        view_board_set_focus(m->id);
                        arrive_at_mass(c);
                        break;
                    }
                }
            } else if (ch == 'v' || ch == 'V') {
                /* デバッグ: 強制決着 (リザルト画面の検証用)。
                   資産王判定を即時に走らせる */
                int i, best = 0;
                u32 top = glue_player_total_assets(0);
                for (i = 1; i < (int)glue_num_players; i++) {
                    u32 a = glue_player_total_assets(i);
                    if (a > top) { top = a; best = i; }
                }
                glue_victory_type = GLUE_WIN_ASSET;
                glue_victory_winner = best;
                game_state = ST_RESULT;
            } else if (ch == 'g' || ch == 'G') {
                /* デバッグ: 次の集金所へワープする。
                   集金所は 227マス中6マスしかなく、通常プレイでは
                   検証に到達しにくいため。 */
                int n = board_mass_count();
                int k;
                for (k = 1; k <= n; k++) {
                    const BoardMass *m =
                        board_get_mass_at((glue_players[c].pos + k) % n);
                    if (m && m->type == MASS_COLLECT) {
                        glue_players[c].pos = m->id;
                        view_board_set_player_pos(c, m->id);
                        view_board_set_focus(m->id);
                        arrive_at_mass(c);
                        break;
                    }
                }
            }
            break;

        case ST_BRANCH: {
            int opts[VIEW_BOARD_MAX_BRANCH];
            int n = view_board_branch_options(c, opts, VIEW_BOARD_MAX_BRANCH);
            int sel = ch - '1';
            if (sel >= 0 && sel < n) {
                view_board_choose_branch(c, opts[sel]);
                game_state = ST_MOVING;
            }
            break;
        }

        case ST_VILLAGE: {
            const EconEstate *estate = glue_village_estate(glue_players[c].pos);
            if (estate) {
                if (ch == '1') {
                    /* 無主なら統治、自分の村なら投資、他人の村なら攻め込む */
                    do_village_action(c,
                        (estate->owner != ECON_OWNER_NONE &&
                         estate->owner != (u8)c) ? 1 : 0);
                }
            }
            if (ch == '3' || ch == 'c' || ch == 'C') {
                end_turn();
            }
            break;
        }

        case ST_COLLECT:
            if (collect_count > 0) {
                if (ch == 'n' || ch == 'N') {
                    collect_sel = (collect_sel + 1) % collect_count;
                } else if (ch == '1') {
                    u32 got = glue_collect_one(c, collect_ids[collect_sel]);
                    char buf[64];
                    sprintf(buf, "%d G を集金した", (int)got);
                    show_message(buf, 45);
                    end_turn();
                } else if (ch == '2') {
                    if (glue_invest_estate(c, collect_ids[collect_sel]) == 0) {
                        show_message("村が発展した！", 45);
                    } else {
                        show_message("所持金が足りない", 45);
                    }
                    end_turn();
                }
            }
            if (ch == '3' || ch == 'e' || ch == 'E') {
                end_turn();
            }
            break;

        case ST_SHOP:
            /* 価格と休業は inv_shop_buy が曜日倍率を見て判断する */
            if ((ch == 'n' || ch == 'N') && shop_count > 0) {
                shop_sel = (shop_sel + 1) % shop_count;
            } else if (ch == '1' && shop_count > 0) {
                u16 id = shop_ids[shop_sel];
                int r = inv_shop_buy(&glue_players[c].bag, id,
                                     &glue_players[c].gold);
                const InvItemDef *d = inv_get_def(id);
                char buf[64];
                if (r == 0) {
                    game_sound_se(GS_SE_COIN);
                    sprintf(buf, "%s を買った", d ? d->name : "品物");
                } else if (r == -2) {
                    strcpy(buf, "所持金が足りない");
                } else if (r == -3) {
                    strcpy(buf, "持ち物がいっぱいだ");
                } else {
                    strcpy(buf, "それは買えない");
                }
                show_message(buf, 40);
                /* メッセージのあとは店へ戻る (手番は終わらせない) */
                msg_return_state = ST_SHOP;
            } else if (ch == '2') {
                refresh_bag_list(c);
                game_state = ST_SELL;
            } else if (ch == '4' || ch == 'i' || ch == 'I') {
                refresh_bag_list(c);
                inv_return_state = ST_SHOP;
                game_state = ST_INVENTORY;
            } else if (ch == '3' || ch == 'e' || ch == 'E') {
                end_turn();
            }
            break;

        case ST_DUNGEON:
            if (ch == '1') {
                dungeon_step(c);
            } else if (ch == '3' || ch == 'e' || ch == 'E') {
                u32 loot = glue_dungeon_escape(c);
                char buf[64];
                if (loot > 0) {
                    sprintf(buf, "%d G を持って迷宮を出た", (int)loot);
                } else {
                    strcpy(buf, "手ぶらで迷宮を出た");
                }
                show_message(buf, 50);
                end_turn();
            }
            break;

        case ST_SELL:
            if ((ch == 'n' || ch == 'N') && bag_count > 0) {
                bag_sel = (bag_sel + 1) % bag_count;
            } else if (ch == '1' && bag_count > 0) {
                char buf[64];
                const InvSlot *s = inv_get_slot(&glue_players[c].bag,
                                                bag_slots[bag_sel]);
                const InvItemDef *d = s ? inv_get_def(s->item_id) : 0;
                u32 price = d ? inv_get_sell_price(d->id) : 0;
                if (inv_shop_sell(&glue_players[c].bag, bag_slots[bag_sel],
                                  &glue_players[c].gold) == 0) {
                    sprintf(buf, "%d G で売った", (int)price);
                } else {
                    strcpy(buf, "それは売れない");
                }
                refresh_bag_list(c);
                show_message(buf, 40);
                msg_return_state = ST_SELL;
            } else if (ch == '3' || ch == 'e' || ch == 'E') {
                game_state = ST_SHOP;
            }
            break;

        case ST_INVENTORY:
            if ((ch == 'n' || ch == 'N') && bag_count > 0) {
                bag_sel = (bag_sel + 1) % bag_count;
            } else if (ch == '1' && bag_count > 0) {
                char buf[64];
                const InvSlot *s = inv_get_slot(&glue_players[c].bag,
                                                bag_slots[bag_sel]);
                const InvItemDef *d = s ? inv_get_def(s->item_id) : 0;
                if (d && inv_is_equipment(d->id)) {
                    if (inv_equip(&glue_players[c].bag, d->id,
                                  d->equip_slot) == 0) {
                        sprintf(buf, "%s を装備した", d->name);
                    } else {
                        strcpy(buf, "それは装備できない");
                    }
                } else if (d) {
                    /* 消耗品は盤上で使える (言霊は戦闘専用) */
                    if (glue_is_kotodama(d->id)) {
                        strcpy(buf, "言霊は戦いの場でしか使えない");
                    } else if (glue_use_item(c, bag_slots[bag_sel], buf) != 0) {
                        strcpy(buf, "何も起きなかった");
                    }
                } else {
                    strcpy(buf, "何も選ばれていない");
                }
                refresh_bag_list(c);
                show_message(buf, 40);
                msg_return_state = ST_INVENTORY;
            } else if (ch == '3' || ch == 'e' || ch == 'E') {
                game_state = inv_return_state;
            }
            break;

        case ST_TRANSFORM:
            if (ch == '1') {
                glue_devil_transform(c);
                show_message("デビルに変身した！", 60);
                end_turn();
            } else {
                end_turn();
            }
            break;

        case ST_CASTLE:
            if (ch == '1') {
                pending_boss = 1;
                view_battle_start(c, glue_castle_boss());
                game_state = ST_BATTLE;
            } else if (ch == '3' || ch == 'e' || ch == 'E') {
                end_turn();
            }
            break;

        case ST_RESULT:
            if (ch == '1') {
                /* 全状態を作り直して新しいゲームへ。
                   glue_init はエンジン一式を初期化し直すので、
                   村の所有権・手番・勝利状態がすべて素に戻る */
                int i;
                glue_shutdown();
                glue_init(api);
                for (i = 0; i < (int)glue_num_players; i++) {
                    view_board_set_player_pos(i, 0);
                }
                view_board_set_focus(0);
                game_state = ST_TITLE;
            }
            break;

        default:
            break;
    }
}

/* ====================================================================== */
/*  CPU の手番進行                                                         */
/* ====================================================================== */
static void update_cpu(void)
{
    int c = cur();

    if (!cur_is_cpu()) return;

    switch (game_state) {
        case ST_DICE:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                start_move(c);
            }
            break;

        case ST_BRANCH:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                int opts[VIEW_BOARD_MAX_BRANCH];
                int n = view_board_branch_options(c, opts, VIEW_BOARD_MAX_BRANCH);
                cpu_timer = 0;
                if (n > 0) {
                    view_board_choose_branch(c, opts[rng_range(0, n - 1)]);
                    game_state = ST_MOVING;
                }
            }
            break;

        case ST_VILLAGE:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                cpu_village_turn(c);
            }
            break;

        case ST_COLLECT:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                /* CPU: 一番育っていない村へ投資、金がなければ全部集金 */
                if (collect_count > 0) {
                    u32 cost = glue_invest_cost(collect_ids[0]);
                    if (glue_players[c].gold >= cost) {
                        glue_invest_estate(c, collect_ids[0]);
                    } else {
                        int i;
                        for (i = 0; i < collect_count; i++) {
                            glue_collect_one(c, collect_ids[i]);
                        }
                    }
                }
                end_turn();
            }
            break;

        case ST_SHOP:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                /* CPU: 品揃えのうち買える一番高いものを1つ買う。
                   所持金の半分までしか使わない (村の投資用に残す) */
                int i, best = -1;
                u32 best_price = 0;
                u32 budget = glue_players[c].gold / 2;
                for (i = 0; i < shop_count; i++) {
                    u32 price = inv_shop_buy_price(shop_ids[i]);
                    if (price > 0 && price <= budget && price > best_price) {
                        best_price = price;
                        best = i;
                    }
                }
                if (best >= 0) {
                    u16 id = shop_ids[best];
                    const InvItemDef *d = inv_get_def(id);
                    if (inv_shop_buy(&glue_players[c].bag, id,
                                     &glue_players[c].gold) == 0 &&
                        d && inv_is_equipment(id)) {
                        /* 装備品ならその場で着ける */
                        inv_equip(&glue_players[c].bag, id, d->equip_slot);
                    }
                }
                end_turn();
            }
            break;

        case ST_DUNGEON:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                /* CPU: HP が半分を切るか、3階まで潜ったら撤退する */
                GluePlayer *p = &glue_players[c];
                if (p->actor.hp * 2 <= p->actor.max_hp ||
                    glue_dungeon_floor >= 3) {
                    glue_dungeon_escape(c);
                    end_turn();
                } else {
                    dungeon_step(c);
                }
            }
            break;

        case ST_SELL:
        case ST_INVENTORY:
            /* CPU はこれらの画面に入らないが、保険として抜けておく */
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                end_turn();
            }
            break;

        case ST_TRANSFORM:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                /* 最下位まで落ちた CPU は変身する */
                glue_devil_transform(c);
                show_message("CPU がデビルに変身した！", 60);
                end_turn();
            }
            break;

        case ST_CASTLE:
            if (++cpu_timer >= CPU_THINK_FRAMES) {
                /* CPU: HP 満タンかつレベルがボス段の2倍以上なら挑む */
                const GlueEnemy *e = glue_enemy_get(glue_castle_boss());
                GluePlayer *p = &glue_players[c];
                if (e && p->actor.hp >= p->actor.max_hp &&
                    (int)p->actor.level >= (int)e->stage * 2) {
                    pending_boss = 1;
                    view_battle_start(c, glue_castle_boss());
                    game_state = ST_BATTLE;
                } else {
                    end_turn();
                }
            }
            break;

        default:
            break;
    }
}

/* ====================================================================== */
/*  ステータス行の描画                                                     */
/* ====================================================================== */
/* 決着画面: 勝者の王座と最終順位 (総資産順) を出す。
   静的な画面なのでシーン切り替え時に1回だけ描く */
static void draw_result_screen(void)
{
    char buf[192];
    int order[GLUE_MAX_PLAYERS];
    int i, k, n = (int)glue_num_players;
    int y;

    for (i = 0; i < n; i++) order[i] = i;
    for (i = 0; i < n - 1; i++) {
        for (k = 0; k < n - 1 - i; k++) {
            if (glue_player_total_assets(order[k]) <
                glue_player_total_assets(order[k + 1])) {
                int t = order[k];
                order[k] = order[k + 1];
                order[k + 1] = t;
            }
        }
    }

    kcg_draw_utf8(240, 40, "― 決 着 ―", TC_PAPER, 0xFF);
    sprintf(buf, "%s が%sとなった",
            (glue_victory_winner >= 0)
                ? glue_players[glue_victory_winner].name : "?",
            glue_victory_name(glue_victory_type));
    kcg_draw_utf8(200, 70, buf, TC_P_YELLOW, 0xFF);

    y = 120;
    for (i = 0; i < n; i++) {
        int pid = order[i];
        u8 col = (pid == glue_victory_winner) ? TC_P_YELLOW : TC_PAPER;
        sprintf(buf, "%d位 %s  Lv%d  総資産 %d G  村 %d",
                i + 1, glue_players[pid].name,
                (int)glue_players[pid].actor.level,
                (int)glue_player_total_assets(pid),
                econ_estate_count((u8)pid));
        kcg_draw_utf8(120, y, buf, col, 0xFF);
        y += 24;
    }

    sprintf(buf, "第%d週 ／ %d ターン ／ 魔王街道 %d/8",
            (int)glue_week, (int)glue_turn,
            (int)(glue_boss_progress - 1));
    kcg_draw_utf8(160, y + 24, buf, TC_LAND_HI, 0xFF);
}

static void draw_status_bar(int force)
{
    char buf[192];
    char name_buf[96];
    int c = cur();
    GluePlayer *p = &glue_players[c];
    (void)name_buf;

    /* 表示内容のチェックサム。変化がないフレームはパネル一式の再描画と
       VRAM 転送を丸ごと省く (16MHz 対策) */
    {
        static u32 last_sig = 0xFFFFFFFFu;
        u32 sig;
        int i;
        sig = (u32)glue_turn * 131u + (u32)glue_week * 7u + (u32)c;
        for (i = 0; i < (int)glue_num_players; i++) {
            GluePlayer *q = &glue_players[i];
            sig = sig * 31u + (u32)q->actor.level;
            sig = sig * 31u + (u32)q->actor.hp;
            sig = sig * 31u + (u32)q->actor.max_hp;
            sig = sig * 31u + (u32)q->actor.exp;
            sig = sig * 31u + (u32)q->gold;
            sig = sig * 31u + (u32)q->pos;
            sig = sig * 31u + (u32)q->is_devil;
            sig = sig * 31u + (u32)econ_estate_count((u8)i);
        }
        sig = sig * 31u + (glue_event_monster_surge() ? 1u : 0u);
        sig = sig * 31u + (glue_event_big_sale() ? 2u : 0u);
        if (!force && sig == last_sig) return;
        last_sig = sig;
    }

    if (p->is_devil) {
        sprintf(name_buf, "%s (デビル)", p->name);
    } else {
        strcpy(name_buf, p->name);
    }

    /* 盤面は左 400px を占めるので、ステータスは右パネル側に出す */
    gfx_fill_rect(VIEW_FIELD_W, 0, 640 - VIEW_FIELD_W, VIEW_FIELD_H,
                  TC_SEA);
    gfx_fill_rect(VIEW_FIELD_W, 0, 2, VIEW_FIELD_H, TC_LAND_HI);

    sprintf(buf, "第%d週 %s曜  %d ターン", (int)glue_week, glue_day_name(),
            (int)glue_turn);
    gfx_fill_rect(VIEW_FIELD_W + 4, 4, 640 - VIEW_FIELD_W - 8, 14,
                  TC_SEA_HI);
    kcg_draw_utf8(VIEW_FIELD_W + 8, 3, buf, TC_LAND_HI, 0xFF);

    {
        int i;
        int y = 24;
        for (i = 0; i < (int)glue_num_players; i++) {
            GluePlayer *q = &glue_players[i];
            u8 pc = (i == 0) ? 10 : (i == 1) ? 9 : (i == 2) ? 12 : 14;
            if (i == c) {
                gfx_fill_rect(VIEW_FIELD_W + 3, y - 2, 640 - VIEW_FIELD_W - 6,
                              66, TC_SEA_HI);
            }
            gfx_fill_rect(VIEW_FIELD_W + 8, y, 8, 12, pc);
            gfx_rect(VIEW_FIELD_W + 8, y, 8, 12, TC_LAND_HI);
            kcg_draw_utf8(VIEW_FIELD_W + 22, y - 1,
                          q->is_devil ? "デビル" : q->name, TC_LAND_HI, 0xFF);

            sprintf(buf, "Lv%-2d  HP %3d/%3d", (int)q->actor.level,
                    (int)q->actor.hp, (int)q->actor.max_hp);
            kcg_draw_utf8(VIEW_FIELD_W + 8, y + 16, buf, TC_LAND_HI, 0xFF);
            sprintf(buf, "所持 %-6d G   村 %d", (int)q->gold,
                    econ_estate_count((u8)i));
            kcg_draw_utf8(VIEW_FIELD_W + 8, y + 32, buf, TC_P_YELLOW, 0xFF);
            gfx_fill_rect(VIEW_FIELD_W + 4, y + 60, 640 - VIEW_FIELD_W - 8,
                          1, TC_SEA_HI);
            y += 66;
        }
    }

    /* 継続中の週次イベントをパネル下端に出す */
    {
        const char *ev = (const char *)0;
        if (glue_event_monster_surge()) ev = "魔物大量発生";
        else if (glue_event_big_sale()) ev = "大売出し";
        if (ev) {
            gfx_fill_rect(VIEW_FIELD_W + 4, VIEW_FIELD_H - 20,
                          640 - VIEW_FIELD_W - 8, 16, 2);
            kcg_draw_utf8(VIEW_FIELD_W + 8, VIEW_FIELD_H - 20, ev, 14, 0xFF);
        }
    }

    /* 手番プレイヤーの詳細は下段の左に。
       全画面クリアは毎フレームしないので、書く前に帯の背景を塗る */
    sprintf(buf, "%s  Lv%d  HP %d/%d  経験 %d  所持 %d G  %d番",
            name_buf, (int)p->actor.level,
            (int)p->actor.hp, (int)p->actor.max_hp,
            (int)p->actor.exp, (int)p->gold, (int)p->pos);
    /* 消去は画面幅いっぱいで行う。日本語化で行が長くなり、盤面幅
       (VIEW_FIELD_W) までしか消していないと右端に前の行が residue として
       残る (「0番」の右に古い文字が見えていた) */
    gfx_fill_rect(0, VIEW_FIELD_H, 640, PANEL_Y - VIEW_FIELD_H, TC_OUTLINE);
    kcg_draw_utf8(8, VIEW_FIELD_H + 4, buf, TC_LAND_HI, 0xFF);
}

void __cdecl main(int argc, char **argv, KernelAPI *kapi)
{
    int running;
    int ch;
    u32 t0;
    int glue_ret;
    int c;

    (void)argc; (void)argv;
    api = kapi;

    /* テキストVRAM を消す。
       PC-98 はテキスト面がグラフィック面の上に重なるので、
       シェルが出力した行が残っていると黒帯になって盤面を隠す
       (テキスト1行 = 16px の帯として出る)。 */
    api->tvram_clear();

    /* GFX初期化 */
    libos32gfx_init(api);
    api->mouse_cursor_set_mode(MOUSE_CURSOR_NONE);

    /* 地形はタイルマップの BG0 に敷く。盤面は画面左上に置く */
    tilemap_init(api);
    tilemap_set_origin(0, 0);
    /* 糊しろタイルが右パネル・下段帯にはみ出さないようにクリップする。
       これで盤面の再描画が UI 側の再描画を巻き込まない */
    tilemap_set_clip(0, 0, VIEW_FIELD_W, VIEW_FIELD_H);
    view_tiles_define();
    view_tiles_define_facilities();
    tilemap_fill(0, TILE_SEA);
    tilemap_set_visible(0, 1);
    tilemap_fill(1, 0);              /* 施設プレーン (0=透過) */
    tilemap_set_visible(1, 1);

    /* 古地図パレットをハードへ流し込む。
       カーネル既定は原色なので、これをやらないと派手なままになる。
       gfx / tilemap の初期化がパレットを触るので、必ずそのあとに呼ぶ。 */
    view_tiles_set_palette(api);

    /* 漢字変換表を有効化 (失敗しても仮名だけで動く) */
    if (!enable_kanji_table()) {
        api->kprintf(0x06, "[GAME] kanji table unavailable\n");
    }

    /* 下段固定パネル初期化 */
    panel_init(api);

    /* サウンド初期化 (BGM は場面ごとに game_sound_scene で切り替える) */
    game_sound_init(api);

    /* 乱数初期化 (RTCベース) */
    rng_seed((unsigned int)api->get_tick());

    /* 盤面描画・移動の初期化 */
    view_board_init(api);

    /* board.db をロード */
    if (view_board_load("/db/board.db") != 0) {
        api->kprintf(12, "[GAME] ERROR: failed to load board.db\n");
        api->gfx_shutdown();
        return;
    }

    /* ゲームエンジン（グルー）初期化 */
    glue_ret = glue_init(api);
    if (glue_ret != 0) {
        api->kprintf(12, "[GAME] ERROR: failed to init game glue (%d)\n", glue_ret);
        api->gfx_shutdown();
        return;
    }

    /* 戦闘表示の初期化 */
    view_battle_init(api);

    /* 初期状態 */
    game_state = ST_TITLE;
    dice_value = 0;
    cpu_timer = 0;

    /* 初期化中の kprintf (GLUE の警告など) がテキストVRAMに残ると
       黒帯になって盤面に重なるので、ゲーム画面に入る前に消す */
    api->tvram_clear();

    running = 1;

    while (running) {
        ch = api->kbd_trygetchar();

        if (ch == 0x1B) {
            running = 0;
        } else {
            handle_key(ch);
        }

        /* CPU の自動進行 */
        update_cpu();

        /* プレイヤー移動アニメーションの更新 */
        view_board_update();

        c = cur();
        /* カメラは手番プレイヤーを追う (移動中は現在地、停止時は到達マス) */
        view_board_set_focus(view_board_get_player_pos(c));
        view_board_set_focus_player(c);

        /* 状態ごとの進行 */
        if (game_state == ST_MOVING) {
            if (!view_board_is_player_moving(c)) {
                /* 通過したマスの地形効果 (毒の沼は通っただけで毒)。
                   末尾は「止まったマス」なので arrive 側で見る */
                {
                    int n = view_board_path_len(c);
                    int k;
                    for (k = 1; k < n - 1; k++) {
                        const BoardMass *pm =
                            board_get_mass((u16)view_board_path_at(c, k));
                        if (pm) glue_apply_terrain(c, pm->terrain, 0);
                    }
                }
                if (view_board_is_branching(c)) {
                    /* 分岐に差しかかった。進行方向の選択を待つ */
                    game_state = ST_BRANCH;
                    cpu_timer = 0;
                } else {
                    arrive_at_mass(c);
                }
            }
        }
        else if (game_state == ST_BATTLE) {
            if (view_battle_update() == 0) {
                const BoardMass *m = board_get_mass(glue_players[c].pos);
                int stay_in_dungeon = 0;

                game_sound_se((glue_players[c].actor.hp > 0)
                              ? GS_SE_WIN : GS_SE_LOSE);

                if (glue_players[c].actor.hp > 0) {
                    /* 村の守備を倒したらその村を奪う */
                    if (m && m->type == MASS_VILLAGE) {
                        glue_village_claim(glue_players[c].pos, c);
                    }
                    /* 城ボスを倒したら街道が1段進む (最終段なら征服王) */
                    if (pending_boss) {
                        char buf[64];
                        u32 bounty = glue_boss_defeated(c);
                        if (bounty > 0) {
                            sprintf(buf, "魔王を討った！ 賞金 %d G", (int)bounty);
                        } else {
                            strcpy(buf, "魔王を討った！ 街道はまだ続く");
                        }
                        show_message(buf, 60);
                    }
                    /* ダンジョン内の戦闘に勝ったら、その階の戦利品を得て
                       潜行を続ける (手番はまだ終わらせない) */
                    if (pending_dungeon) {
                        glue_dungeon_reward_floor(c);
                        game_sound_se(GS_SE_COIN);
                        sprintf(dungeon_msg, "地下%d階を突破 ／ 戦利品 %d G",
                                glue_dungeon_floor, (int)glue_dungeon_loot);
                        game_state = ST_DUNGEON;
                        cpu_timer = 0;
                        stay_in_dungeon = 1;
                    }
                } else {
                    /* 敗北: 変身解除のうえ振り出しに戻る */
                    if (glue_players[c].is_devil) {
                        glue_devil_release(c);
                    }
                    glue_players[c].pos = 0;
                    glue_players[c].actor.hp = glue_players[c].actor.max_hp;
                    view_board_set_player_pos(c, 0);
                    /* ダンジョンで倒れたら戦利品は全部失う */
                    if (pending_dungeon) {
                        glue_dungeon_fail(c);
                        show_message("迷宮で倒れた。戦利品を失った",
                                     70);
                    }
                }
                pending_dungeon = 0;
                pending_boss = 0;
                if (!stay_in_dungeon) end_turn();
            }
        }
        else if (game_state == ST_MESSAGE) {
            if (action_timer > 0) {
                action_timer--;
            } else {
                /* 店・持ち物から出したメッセージは元の画面へ戻す。
                   それ以外は従来どおりサイコロ待ちへ */
                game_state = (msg_return_state >= 0)
                             ? msg_return_state : ST_DICE;
                msg_return_state = -1;
            }
        }

        /* --- BGM ---
           「いま鳴らすべき曲」を毎フレーム宣言する。同じ曲なら
           game_sound_scene() が何もしないので、状態遷移のたびに
           停止/再生を書き分けなくてよい */
        switch (game_state) {
        case ST_TITLE:
            game_sound_scene(GS_BGM_TITLE);
            break;
        case ST_BATTLE:
            game_sound_scene(pending_boss ? GS_BGM_BOSS : GS_BGM_BATTLE);
            break;
        case ST_SHOP:
        case ST_SELL:
            game_sound_scene(GS_BGM_SHOP);
            break;
        case ST_DUNGEON:
            game_sound_scene(GS_BGM_DUNGEON);
            break;
        case ST_RESULT:
            game_sound_scene(GS_BGM_RESULT);
            break;
        default:
            /* 持ち物画面は開いた元の場面の曲を続ける */
            if (game_state != ST_INVENTORY) {
                game_sound_scene(GS_BGM_FIELD);
            }
            break;
        }

        /* --- 描画 ---
           16MHz 級の実機では「毎フレーム全画面を描いて転送する」コストが
           支配的なので、変化した部分しか描かない。gfx の描画プリミティブは
           ダーティ矩形を自動登録するため、描かなければ present は何も
           転送しない。全画面クリアは画面構成の切り替え時だけ。 */
        {
            static int prev_group = -1;
            int group = (game_state == ST_BATTLE) ? 2
                      : (game_state == ST_TITLE)  ? 0
                      : (game_state == ST_RESULT) ? 3 : 1;
            int scene_reset = (group != prev_group);
            prev_group = group;

            if (scene_reset) {
                gfx_clear(TC_OUTLINE);
                view_board_invalidate();
                view_battle_force_redraw();
            }

            if (game_state == ST_BATTLE) {
                /* シーン描画は view_battle_draw が差分検知する */
                view_battle_draw();
            } else if (game_state == ST_RESULT) {
                if (scene_reset) draw_result_screen();
            } else if (game_state != ST_TITLE) {
                view_board_draw();
                draw_status_bar(scene_reset);
            } else if (scene_reset) {
                kcg_draw_utf8(160, 40, "OS32 対戦スゴロクRPG", 15, 0);
                kcg_draw_utf8(160, 160, "下のパネルから選んでください", 12, 0);
            }

            /* 下段の固定パネル。内容が変わったときだけ描かれる */
            panel_begin();
            build_panel();
            panel_end(scene_reset);
        }

        /* VRAM転送 (ダーティ矩形のみ) */
        api->gfx_present_dirty();

        /* 自動プレイ観測用: 現在状態をメールボックス (0x90000) へ書き出す */
        view_export_tick(game_state, dice_value);

        /* ~30fps制御 */
        t0 = api->get_tick();
        while (api->get_tick() - t0 < 3) {
            /* 空ループ */
        }
    }

    /* シャットダウン */
    game_sound_shutdown();
    tilemap_shutdown();
    glue_shutdown();
    board_shutdown();
    api->gfx_shutdown();
    api->tvram_clear();
}
