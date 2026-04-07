/*
 * macro_engine.c - OS32 VZ Editor Macro Interpreter
 * C89 compatible
 */

#include "vz.h"

/*
 * macro_init
 * 初期化処理
 */
void macro_init(void) {
    vz.macro_env.running = 0;
    vz.macro_env.call_depth = 0;
    vz.key_bind_count = 0;
}

/*
 * macro_start
 * マクロの実行を開始する
 */
void macro_start(const char* mac_str) {
    if (!mac_str || mac_str[0] == '\0') return;
    vz.macro_env.macro_text = mac_str;
    vz.macro_env.pc = 0;
    vz.macro_env.call_depth = 0;
    vz.macro_env.running = 1;
    vz.macro_env.result_flag = 1; /* 初期状態は真 */
}

/*
 * macro_step
 * マクロを1ステップ（1コマンド）実行する
 * 戻り値: 1=まだ続きがある, 0=終了した
 */
int macro_step(void) {
    char c;
    TEXT* t;

    if (!vz.macro_env.running) return 0;
    if (!vz.macro_env.macro_text) return 0;
    
    c = vz.macro_env.macro_text[vz.macro_env.pc++];
    if (c == '\0') {
        /* マクロ終了（スタックがあれば戻る） */
        if (vz.macro_env.call_depth > 0) {
            vz.macro_env.call_depth--;
            vz.macro_env.macro_text = vz.macro_env.call_stack[vz.macro_env.call_depth];
            vz.macro_env.pc = vz.macro_env.call_pc_stack[vz.macro_env.call_depth];
            return 1;
        }
        vz.macro_env.running = 0;
        return 0;
    }

    /* Phase 3: マクロ制御構文の解釈 */
    t = vz.current_text;

    /* 
     * コマンド解釈の基本ルール: 
     * - コントロールコード (c < 0x20 や 0x7F) は組み込みコマンドとして処理
     * - 表示可能文字 (c >= 0x20) はそのまま文字入力（ただしマクロ特殊記号は後で処理）
     */
     
    /* TODO: ?, >, & などのマクロ専用特殊記号処理を増やす */

    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
        macro_exec_builtin((unsigned char)c);
    } else {
        if (t) {
            te_insert_char(t, c);
        }
    }

    return 1;
}

typedef void (*BuiltinAction)(TEXT* w);

typedef struct {
    int key;
    BuiltinAction action;
} BuiltinMap;

static void action_backspace(TEXT* t)  { te_delete_char(t, 1); }
static void action_delete(TEXT* t)     { te_delete_char(t, 0); }
static void action_enter(TEXT* t)      { te_insert_char(t, '\n'); }
static void action_right(TEXT* t)      { te_move_cursor(t, 1, 0); }
static void action_left(TEXT* t)       { te_move_cursor(t, -1, 0); }
static void action_up(TEXT* t)         { te_move_cursor(t, 0, -1); }
static void action_down(TEXT* t)       { te_move_cursor(t, 0, 1); }
static void action_ctrl_f(TEXT* t)     { te_move_cursor(t, 1, 0); }
static void action_ctrl_b(TEXT* t)     { te_move_cursor(t, -1, 0); }
static void action_ctrl_a(TEXT* t)     { te_move_to_bol(t); }
static void action_ctrl_e(TEXT* t)     { te_move_to_eol(t); }
static void action_ctrl_u(TEXT* t)     { te_undo(t); set_notification("Undo"); }
static void action_ctrl_y(TEXT* t)     { te_delete_line(t); }
static void action_ctrl_r(TEXT* t)     { te_move_line(t, -t->tw_sy); }
static void action_ctrl_c(TEXT* t)     { te_move_line(t, t->tw_sy); }
static void action_ctrl_v(TEXT* t)     { 
    vz.insert_mode = !vz.insert_mode;
    set_notification(vz.insert_mode ? "挿入モード" : "上書きモード");
}

static BuiltinMap builtin_table[] = {
    { 0x08, action_backspace }, /* Backspace */
    { 0x7F, action_delete },    /* Delete */
    { '\r', action_enter },     /* Enter */
    { '\n', action_enter },     /* Enter */
    { 0x1C, action_right },     /* Right */
    { 0x1D, action_left },      /* Left */
    { 0x1E, action_up },        /* Up */
    { 0x1F, action_down },      /* Down */
    { 0x0E, action_down },      /* Ctrl-N */
    { 0x10, action_up },        /* Ctrl-P */
    { 0x06, action_ctrl_f },    /* Ctrl-F */
    { 0x02, action_ctrl_b },    /* Ctrl-B */
    { 0x01, action_ctrl_a },    /* Ctrl-A */
    { 0x05, action_ctrl_e },    /* Ctrl-E */
    { 0x15, action_ctrl_u },    /* Ctrl-U */
    { 0x19, action_ctrl_y },    /* Ctrl-Y */
    { 0x12, action_ctrl_r },    /* Ctrl-R */
    { 0x03, action_ctrl_c },    /* Ctrl-C */
    { 0x16, action_ctrl_v },    /* Ctrl-V */
    { 0x0C, cmd_search_next },  /* Ctrl-L */
    { 0, NULL }
};

/*
 * macro_exec_builtin
 * メインループから分離された、内部の編集機能・システム機能を呼び出す
 */
void macro_exec_builtin(int c) {
    TEXT* t = vz.current_text;
    int i;
    if (!t) return;

    for (i = 0; builtin_table[i].action != NULL; i++) {
        if (builtin_table[i].key == c) {
            builtin_table[i].action(t);
            break;
        }
    }
}
