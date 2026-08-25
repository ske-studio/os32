/* ======================================================================== */
/*  VIEW_BATTLE.C — 戦闘画面の描画および入力処理                             */
/* ======================================================================== */

#include "view_battle.h"
#include "game_glue.h"
#include "view_panel.h"
#include "view_tiles.h"
#include "game_sound.h"
#include "libos32gfx.h"
#include "libos32inv.h"
#include "libos32math.h"
#include <stdio.h>
#include <string.h>

/* 攻撃側と防御側が交互に入れ替わる。
   1ラウンド = 「先攻が攻撃 -> 後攻が攻撃」の2フェーズ。
   各フェーズで攻撃側は攻撃コマンド、防御側は防御コマンドを選ぶ。 */
enum BattleState {
    B_STATE_ATK_INPUT,   /* 攻撃側のコマンド待ち */
    B_STATE_KOTODAMA,    /* 唱える言霊の選択待ち */
    B_STATE_DEF_INPUT,   /* 防御側のコマンド待ち */
    B_STATE_RESOLVE,     /* 結果表示中 */
    B_STATE_FINISHED
};

static KernelAPI *api;

static int b_pid;
static u16 b_monster_id;
static const char *b_enemy_name;
static int b_rewarded;          /* 1=報酬精算済み (多重加算の防止) */
static RpgLevelResult b_lv;     /* 直近の戦闘でのレベルアップ結果 */
static int b_cpu_timer;         /* CPU の思考待ちフレーム */
static BtlUnit b_player;
static BtlUnit b_enemy;
static int b_state;

/* 演出用メッセージバッファ */
static char b_msg[192];
static int b_msg_timer;

/* コマンド選択結果 */
static u8 b_atk_cmd;
static u8 b_def_cmd;
static BtlResult b_res;

/* 1=プレイヤーが攻撃側, 0=敵が攻撃側 */
static int b_player_attacks;
/* このラウンドで残っているフェーズ数 (2 -> 1 -> 次ラウンド) */
static int b_phase_left;

/* 言霊の選択候補 (バッグのスロット番号) */
static u8  b_koto_slots[GLUE_MAX_KOTODAMA];
static int b_koto_count;

void view_battle_init(KernelAPI *kapi)
{
    api = kapi;
}

void view_battle_start(int player_id, u16 monster_id)
{
    b_pid = player_id;
    b_monster_id = monster_id;
    b_enemy_name = glue_enemy_name(monster_id);

    glue_battle_start_pve(player_id, monster_id, &b_player, &b_enemy);

    /* 先攻は素早さで決まる */
    b_player_attacks = btl_first_strike((int)b_player.spd, (int)b_enemy.spd);
    b_phase_left = 2;
    b_state = B_STATE_ATK_INPUT;
    b_msg[0] = '\0';
    b_msg_timer = 0;
    b_rewarded = 0;
    b_cpu_timer = 0;
    memset(&b_lv, 0, sizeof(b_lv));
}

/* 撃破メッセージ。報酬とレベルアップまで含めて 1 行にまとめる */
static void set_victory_msg(void)
{
    const GlueEnemy *e = glue_enemy_get(b_monster_id);

    if (e) {
        sprintf(b_msg, "%s を倒した！ 経験値 +%d ／ %d G",
                b_enemy_name, (int)e->exp, (int)e->gold);
    } else {
        sprintf(b_msg, "%s を倒した！", b_enemy_name);
    }
}

static void advance_phase(void);
static void cpu_choose(void);
static int  human_turn(void);

int view_battle_update(void)
{
    if (b_state == B_STATE_FINISHED && b_msg_timer == 0) {
        /* 戦闘結果の HP を永続アクターへ書き戻す */
        glue_players[b_pid].actor.hp = b_player.hp;

        /* 勝利していれば経験値と金銭を精算する (二重取得を防ぐため1回だけ) */
        if (b_enemy.hp <= 0 && !b_rewarded) {
            b_rewarded = 1;
            glue_battle_reward(b_pid, b_monster_id, &b_lv);
        }
        return 0;
    }

    if (b_msg_timer > 0) {
        b_msg_timer--;
        return 1;
    }

    /* 結果表示が終わったら次のフェーズへ */
    if (b_state == B_STATE_RESOLVE) {
        advance_phase();
        return 1;
    }

    /* 言霊の選択待ちはプレイヤーの入力を待つだけ */
    if (b_state == B_STATE_KOTODAMA) return 1;

    /* CPU (または敵) の入力は自動で進める */
    if ((b_state == B_STATE_ATK_INPUT || b_state == B_STATE_DEF_INPUT) &&
        !human_turn()) {
        if (++b_cpu_timer >= 20) {
            b_cpu_timer = 0;
            cpu_choose();
        }
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

/* 戦闘シーンの見た目を決める要素のチェックサム。
   変化がないフレームは描き直さない (16MHz 対策) */
static u32 battle_draw_sig(void)
{
    u32 sig = 0;
    const char *s;
    sig = (u32)b_player.hp * 131u + (u32)b_enemy.hp * 137u;
    sig = sig * 31u + (u32)b_state;
    sig = sig * 31u + (u32)b_player_attacks;
    sig = sig * 31u + (u32)b_player.tame_count;
    sig = sig * 31u + (u32)b_enemy.tame_count;
    sig = sig * 31u + (u32)(b_msg_timer > 0);
    for (s = b_msg; *s; s++) sig = sig * 31u + (u32)*s;
    return sig;
}

/* シーン切り替え直後は下地が消えているので強制再描画する */
static int b_force_draw = 1;
void view_battle_force_redraw(void)
{
    b_force_draw = 1;
}

void view_battle_draw(void)
{
    char hp_buf[32];
    char name_buf[96];

    {
        static u32 last_sig = 0xFFFFFFFFu;
        u32 sig = battle_draw_sig();
        if (!b_force_draw && sig == last_sig) return;
        last_sig = sig;
        b_force_draw = 0;
    }

    gfx_fill_rect(10, 10, 620, 220, 4);
    
    if (glue_players[b_pid].is_devil) {
        sprintf(name_buf, "%s (デビル)", glue_players[b_pid].name);
    } else {
        strcpy(name_buf, glue_players[b_pid].name);
    }
    kcg_draw_utf8(30, 30, name_buf, 15, 0);
    draw_gauge(30, 50, 150, 10, b_player.hp, b_player.max_hp, 2, 8);
    
    sprintf(hp_buf, "HP: %d/%d", (int)b_player.hp, (int)b_player.max_hp);
    kcg_draw_utf8(30, 65, hp_buf, 15, 0);

    kcg_draw_utf8(450, 30, b_enemy_name, 15, 0);
    draw_gauge(450, 50, 150, 10, b_enemy.hp, b_enemy.max_hp, 2, 8);
    
    sprintf(hp_buf, "HP: %d/%d", (int)b_enemy.hp, (int)b_enemy.max_hp);
    kcg_draw_utf8(450, 65, hp_buf, 15, 0);

    /* いま誰が攻撃側かを矢印で示す */
    if (b_state != B_STATE_FINISHED) {
        if (b_player_attacks) {
            kcg_draw_utf8(200, 30, "==>", 14, 0);
        } else {
            kcg_draw_utf8(200, 30, "<==", 14, 0);
        }
    }

    /* ためたカウント */
    if (b_player.tame_count > 0) {
        char t[40];
        sprintf(t, "ためる x%d", (int)b_player.tame_count);
        kcg_draw_utf8(30, 85, t, 14, 0);
    }
    if (b_enemy.tame_count > 0) {
        char t[40];
        sprintf(t, "ためる x%d", (int)b_enemy.tame_count);
        kcg_draw_utf8(450, 85, t, 14, 0);
    }

    if (b_msg[0] != '\0') {
        gfx_fill_rect(10, 180, 620, 40, 0);
        kcg_draw_utf8(30, 190, b_msg, 15, 0);
    }
}

/* いま人間プレイヤーが操作する番か */
static int human_turn(void)
{
    if (glue_players[b_pid].is_cpu) return 0;
    /* 攻撃フェーズなら攻撃側が、防御フェーズなら防御側が人間かどうか */
    if (b_state == B_STATE_ATK_INPUT)  return b_player_attacks;
    if (b_state == B_STATE_DEF_INPUT)  return !b_player_attacks;
    return 0;
}

/* 攻撃側/防御側のユニットを返す */
static BtlUnit *atk_unit(void)
{
    return b_player_attacks ? &b_player : &b_enemy;
}

static BtlUnit *def_unit(void)
{
    return b_player_attacks ? &b_enemy : &b_player;
}

static const char *atk_name(void)
{
    return b_player_attacks ? glue_players[b_pid].name : b_enemy_name;
}

static const char *def_name(void)
{
    return b_player_attacks ? b_enemy_name : glue_players[b_pid].name;
}

/* 攻撃側・防御側のコマンドが揃ったので1撃を解決する */
static void resolve_phase(void)
{
    int fled;
    char cmd_buf[48];

    strncpy(cmd_buf, glue_atk_cmd_name(b_atk_cmd), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    fled = glue_battle_strike(atk_unit(), b_atk_cmd,
                              def_unit(), b_def_cmd, &b_res);

    /* 攻撃が通ったら、こちらの一撃か被弾かで鳴らし分ける */
    if (b_res.damage > 0 && b_atk_cmd != GLUE_ATK_FLEE &&
        b_atk_cmd != GLUE_ATK_CHARGE) {
        int to_player = (b_res.result_type == BTL_RES_REFLECT)
                        ? b_player_attacks : !b_player_attacks;
        game_sound_se(to_player ? GS_SE_DAMAGE : GS_SE_HIT);
    }

    if (b_atk_cmd == GLUE_ATK_FLEE) {
        if (fled) {
            sprintf(b_msg, "%s は逃げ出した！", atk_name());
            b_state = B_STATE_FINISHED;
        } else {
            sprintf(b_msg, "%s は逃げられなかった", atk_name());
            b_state = B_STATE_RESOLVE;
        }
    } else if (b_atk_cmd == GLUE_ATK_CHARGE) {
        sprintf(b_msg, "%s は力をためた！", atk_name());
        b_state = B_STATE_RESOLVE;
    } else if (b_res.result_type == BTL_RES_REFLECT) {
        sprintf(b_msg, "%s が反射！ %s に %d のダメージ",
                def_name(), atk_name(), (int)b_res.damage);
        b_state = B_STATE_RESOLVE;
    } else if (b_res.damage == 0) {
        sprintf(b_msg, "%s の%s ／ ダメージなし", atk_name(), cmd_buf);
        b_state = B_STATE_RESOLVE;
    } else {
        sprintf(b_msg, "%s の%s ／ %d のダメージ (%s)",
                atk_name(), cmd_buf, (int)b_res.damage,
                glue_def_cmd_name(b_def_cmd));
        b_state = B_STATE_RESOLVE;
    }
    b_msg_timer = 50;
}

/* 結果表示が終わったあとの進行 */
static void advance_phase(void)
{
    if (b_player.hp <= 0 || b_enemy.hp <= 0) {
        if (b_enemy.hp <= 0) {
            set_victory_msg();
        } else {
            strcpy(b_msg, "あなたは倒れた……");
        }
        b_msg_timer = 50;
        b_state = B_STATE_FINISHED;
        return;
    }

    b_phase_left--;
    if (b_phase_left > 0) {
        /* 同じラウンドの後攻フェーズ。攻守が入れ替わる */
        b_player_attacks = !b_player_attacks;
    } else {
        /* 次ラウンド。先攻を素早さで引き直す */
        b_phase_left = 2;
        b_player_attacks = btl_first_strike((int)b_player.spd,
                                            (int)b_enemy.spd);
    }
    b_msg[0] = '\0';
    b_state = B_STATE_ATK_INPUT;
    b_cpu_timer = 0;
}

int view_battle_is_input_wait(void)
{
    return ((b_state == B_STATE_ATK_INPUT || b_state == B_STATE_DEF_INPUT)
            && b_msg_timer == 0) ? 1 : 0;
}

/* 言霊を唱える。防御側の選択を待たず即座に解決する */
static void cast_kotodama(int idx)
{
    char msg[160];

    if (idx < 0 || idx >= b_koto_count) return;

    if (glue_kotodama_cast(b_pid, b_koto_slots[idx],
                           &b_player, &b_enemy, msg) == 0) {
        game_sound_se(GS_SE_KOTODAMA);
        strncpy(b_msg, msg, sizeof(b_msg) - 1);
        b_msg[sizeof(b_msg) - 1] = '\0';
    } else {
        strcpy(b_msg, "言霊は不発に終わった");
    }
    b_player.tame_count = 0;
    b_msg_timer = 50;
    b_state = B_STATE_RESOLVE;
}

/* 攻撃側コマンドを確定し、防御側の入力へ進む */
static void set_atk_cmd(u8 cmd)
{
    /* 魔法は言霊を持っていれば選択へ。持っていなければ従来の
       MAG 依存の魔法攻撃にフォールバックする */
    if (cmd == GLUE_ATK_MAGIC && b_player_attacks &&
        !glue_players[b_pid].is_cpu) {
        b_koto_count = glue_kotodama_list(b_pid, b_koto_slots,
                                          GLUE_MAX_KOTODAMA);
        if (b_koto_count > 0) {
            b_state = B_STATE_KOTODAMA;
            return;
        }
    }

    b_atk_cmd = cmd;
    /* 逃走とためるは防御側の選択を待たない */
    if (cmd == GLUE_ATK_FLEE || cmd == GLUE_ATK_CHARGE) {
        b_def_cmd = GLUE_DEF_NONE;
        resolve_phase();
    } else {
        b_state = B_STATE_DEF_INPUT;
        b_cpu_timer = 0;
    }
}

static void set_def_cmd(u8 cmd)
{
    b_def_cmd = cmd;
    resolve_phase();
}

void view_battle_handle_key(int ch)
{
    /* 言霊の選択はプレイヤー固有の画面なので input_wait とは別扱い */
    if (b_state == B_STATE_KOTODAMA) {
        if (ch >= '1' && ch <= '9') {
            cast_kotodama(ch - '1');
        } else if (ch == '0' || ch == 'c' || ch == 'C') {
            b_state = B_STATE_ATK_INPUT;   /* 取り消して攻撃選択へ戻る */
        }
        return;
    }

    if (!view_battle_is_input_wait()) return;
    if (!human_turn()) return;

    if (b_state == B_STATE_ATK_INPUT) {
        if (ch >= '1' && ch <= '5') set_atk_cmd((u8)(ch - '1'));
    } else {
        /* 防御は 1=防御 2=回避 3=反射 */
        if (ch >= '1' && ch <= '3') set_def_cmd((u8)(ch - '1' + 1));
    }
}

/* CPU 側の選択 (libos32ai の性格プロファイルで決める) */
static void cpu_choose(void)
{
    if (b_state == B_STATE_ATK_INPUT) {
        set_atk_cmd(glue_ai_attack_cmd(b_pid, atk_unit(), def_unit()));
    } else {
        set_def_cmd(glue_ai_defend_cmd(b_pid, def_unit(), atk_unit()));
    }
}

/* 下段パネルの中身を組み立てる (描画そのものは main 側の panel_end)。
   進行は view_battle_update() がやるので、ここは表示だけ */
void view_battle_panel(void)
{
    char buf[160];

    if (b_msg_timer > 0 && b_msg[0] != '\0') {
        panel_line(b_msg);
        return;
    }

    if (b_state == B_STATE_FINISHED) {
        panel_line(b_msg[0] ? b_msg : "戦闘終了");
        return;
    }

    /* 言霊の選択はプレイヤー固有の画面。human_turn() は攻撃/防御の
       入力フェーズしか見ないので、先に判定すること */
    if (b_state == B_STATE_KOTODAMA) {
        int i;
        char row[160];
        panel_line_hi("唱える言霊を選べ");
        row[0] = '\0';
        for (i = 0; i < b_koto_count; i++) {
            const InvSlot *s = inv_get_slot(&glue_players[b_pid].bag,
                                            b_koto_slots[i]);
            const InvItemDef *d = s ? inv_get_def(s->item_id) : 0;
            char one[48];
            if (!d) continue;
            sprintf(one, "%d:%s(x%d) ", i + 1, d->name, (int)s->count);
            if (strlen(row) + strlen(one) < sizeof(row)) strcat(row, one);
            /* 1行に3つずつ */
            if ((i % 3) == 2) { panel_line(row); row[0] = '\0'; }
        }
        if (row[0]) panel_line(row);
        panel_line("0: やめる");
        return;
    }

    if (!human_turn()) {
        sprintf(buf, "%s は考えている……", atk_name());
        panel_line(buf);
        return;
    }

    if (b_state == B_STATE_ATK_INPUT) {
        sprintf(buf, "攻撃を選べ  (ためる x%d)", (int)b_player.tame_count);
        panel_line_hi(buf);
        panel_line("1: 攻撃    2: 強攻撃    3: 魔法");
        panel_line("4: ためる  5: 逃走");
    } else {
        sprintf(buf, "%s の攻撃！ 受け方を選べ", b_enemy_name);
        panel_line_hi(buf);
        panel_line("1: 防御    2: 回避    3: 反射");
    }
}
