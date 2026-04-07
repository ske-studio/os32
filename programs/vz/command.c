/*
 * command.c - VZ Editor Commands
 * C89 compatible
 */
#include "vz.h"

int vz_quit_flag = 0;

void cmd_save_and_quit(TEXT* t) {
    if (!t) return;
    if (te_save_file(t)) {
        vz_quit_flag = 1; /* Signal main loop to quit */
    } else {
        set_notification("保存に失敗しました。");
    }
}

void cmd_save(TEXT* t) {
    if (!t) return;
    if (te_save_file(t)) {
        set_notification("保存しました。");
    } else {
        set_notification("保存に失敗しました。");
    }
}

void cmd_save_as(TEXT* t) {
    char file_buf[PATHSZ];
    int i;
    if (!t) return;
    for (i = 0; i < PATHSZ; i++) file_buf[i] = '\0';
    if (vz_prompt_input("名前を付けて保存: ", file_buf, PATHSZ)) {
        if (file_buf[0] != '\0') {
            for (i = 0; file_buf[i] != '\0' && i < PATHSZ - 1; i++) {
                t->path[i] = file_buf[i];
            }
            t->path[i] = '\0';
            t->namep = t->path;
            if (te_save_file(t)) {
                set_notification("新しいファイルとして保存しました。");
            } else {
                set_notification("保存に失敗しました。");
            }
        } else {
            set_notification("キャンセルしました。");
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

void cmd_open_file(TEXT* t) {
    char file_buf[PATHSZ];
    int i;
    for (i = 0; i < PATHSZ; i++) file_buf[i] = '\0';
    if (vz_prompt_input("開く: ", file_buf, PATHSZ)) {
        if (file_buf[0] != '\0') {
            vz_load_file(file_buf);
            set_notification("開きました。");
        } else {
            set_notification("キャンセルしました。");
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

extern void vz_open_filer_internal(const char* dir); /* Declare external to access from main.c if needed, or implement here */
/* In main.c it is vz_open_filer, let's keep it in main.c and just call it */
extern void vz_open_filer(const char* dir);

void cmd_filer(TEXT* t) {
    const char *cwd = kapi->sys_getcwd();
    vz_open_filer(cwd ? cwd : "/");
}

void cmd_swap_text(TEXT* t) {
    if (vz.text_count > 1) {
        int next_idx = (vz.window_text_idx[vz.active_window] + 1) % vz.text_count;
        vz.window_text_idx[vz.active_window] = next_idx;
        vz.current_text = vz.text_list[next_idx];
        set_notification("テキストを切り替えました。");
    } else {
        set_notification("テキストは1つだけです。");
    }
}

void cmd_swap_window(TEXT* t) {
    if (vz.split_mode) {
        vz.active_window = !vz.active_window;
        vz.current_text = vz.text_list[vz.window_text_idx[vz.active_window]];
        set_notification("ウインドウを切り替えました。");
    } else {
        set_notification("分割されていません。");
    }
}

void cmd_toggle_split(TEXT* t) {
    vz.split_mode = !vz.split_mode;
    vz_clear(); 
}

void cmd_block_start(TEXT* t) {
    if (!t) return;
    if (t->block_mode == 1 || t->block_mode == 2) {
        /* 選択中に再度Ctrl-K Bで矩形モードにトグル */
        if (t->block_mode == 3) {
            t->block_mode = 1;
            set_notification("通常選択に戻しました。");
        } else {
            t->block_mode = 3;
            set_notification("矩形選択モード。");
        }
        return;
    }
    te_mark_block_start(t);
    /* 列位置を記録 */
    te_count_position(t);
    t->block_start_col = t->lx;
    set_notification("ブロック開始。");
}

void cmd_block_end(TEXT* t) {
    if (!t) return;
    te_mark_block_end(t);
    /* 列位置を記録 */
    te_count_position(t);
    t->block_end_col = t->lx;
    if (t->block_mode == 3) {
        set_notification("矩形ブロック確定。");
    } else {
        set_notification("ブロック終了。");
    }
}

void cmd_block_hide(TEXT* t) {
    if (!t) return;
    te_unmark_block(t);
    set_notification("ブロック解除。");
}

void cmd_block_copy(TEXT* t) {
    if (!t) return;
    te_block_copy(t);
    te_unmark_block(t);
    te_block_paste(t);
    set_notification("コピーしました。");
}

void cmd_block_delete(TEXT* t) {
    if (!t) return;
    te_block_delete(t);
    set_notification("削除しました。");
}

void cmd_redo(TEXT* t) {
    if (!t) return;
    te_redo(t);
    set_notification("やり直し");
}

void cmd_top(TEXT* t) {
    if (!t) return;
    te_move_to_top(t);
}

void cmd_bottom(TEXT* t) {
    if (!t) return;
    te_move_to_bottom(t);
}

void cmd_search_forward(TEXT* t) {
    if (!t) return;
    if (vz_prompt_input("検索: ", vz.search_buf, sizeof(vz.search_buf))) {
        if (vz.search_buf[0] != '\0') {
            int found;
            vz.search_dir = 1;
            if (vz.search_ignore_case) {
                found = te_search_forward_ci(t, vz.search_buf);
            } else {
                found = te_search_forward(t, vz.search_buf);
            }
            if (found) {
                set_notification("見つかりました。");
            } else {
                set_notification("見つかりません。");
            }
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

void cmd_search_next(TEXT* t) {
    if (!t) return;
    if (vz.search_buf[0] != '\0') {
        int found = 0;
        if (vz.search_dir == 1) {
            te_move_cursor(t, 1, 0);
            if (vz.search_ignore_case) {
                found = te_search_forward_ci(t, vz.search_buf);
            } else {
                found = te_search_forward(t, vz.search_buf);
            }
            if (!found) te_move_cursor(t, -1, 0); 
        } else {
            if (vz.search_ignore_case) {
                found = te_search_backward_ci(t, vz.search_buf);
            } else {
                found = te_search_backward(t, vz.search_buf);
            }
        }
        if (found) {
            set_notification("見つかりました。");
        } else {
            set_notification("見つかりません。");
        }
    }
}

void cmd_search_backward(TEXT* t) {
    if (!t) return;
    if (vz_prompt_input("上方向検索: ", vz.search_buf, sizeof(vz.search_buf))) {
        if (vz.search_buf[0] != '\0') {
            int found;
            vz.search_dir = -1;
            if (vz.search_ignore_case) {
                found = te_search_backward_ci(t, vz.search_buf);
            } else {
                found = te_search_backward(t, vz.search_buf);
            }
            if (found) {
                set_notification("見つかりました。");
            } else {
                set_notification("見つかりません。");
            }
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

void cmd_replace(TEXT* t) {
    if (!t) return;
    if (vz_prompt_input("検索: ", vz.search_buf, sizeof(vz.search_buf))) {
        if (vz.search_buf[0] != '\0') {
            if (vz_prompt_input("置換: ", vz.replace_buf, sizeof(vz.replace_buf))) {
                int count = te_replace(t, vz.search_buf, vz.replace_buf, 0); 
                if (count > 0) {
                    set_notification("置換しました。");
                } else {
                    set_notification("見つかりません。");
                }
            } else {
                set_notification("キャンセルしました。");
            }
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

void cmd_replace_all(TEXT* t) {
    if (!t) return;
    if (vz_prompt_input("検索(全置換): ", vz.search_buf, sizeof(vz.search_buf))) {
        if (vz.search_buf[0] != '\0') {
            if (vz_prompt_input("置換: ", vz.replace_buf, sizeof(vz.replace_buf))) {
                int count;
                /* ファイル先頭から全置換 */
                te_move_to_top(t);
                count = te_replace(t, vz.search_buf, vz.replace_buf, 1);
                if (count > 0) {
                    /* 置換件数を通知 */
                    char msg[64];
                    int i, val, dc;
                    /* "N件置換しました。" を構築 */
                    val = count;
                    dc = 0;
                    { int tmp = val; do { dc++; tmp /= 10; } while (tmp > 0); }
                    for (i = dc - 1; i >= 0; i--) {
                        msg[i] = '0' + (val % 10);
                        val /= 10;
                    }
                    msg[dc] = '\0';
                    /* 後ろに文字列を連結 */
                    {
                        const char* suffix = "\xE4\xBB\xB6\xE7\xBD\xAE\xE6\x8F\x9B\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82"; /* "件置換しました。" UTF-8 */
                        int j = dc;
                        int k = 0;
                        while (suffix[k] != '\0' && j < 62) {
                            msg[j++] = suffix[k++];
                        }
                        msg[j] = '\0';
                    }
                    set_notification(msg);
                } else {
                    set_notification("見つかりません。");
                }
            } else {
                set_notification("キャンセルしました。");
            }
        }
    } else {
        set_notification("キャンセルしました。");
    }
}

void cmd_toggle_case(TEXT* t) {
    (void)t;
    vz.search_ignore_case = !vz.search_ignore_case;
    if (vz.search_ignore_case) {
        set_notification("大文字小文字を無視します。");
    } else {
        set_notification("大文字小文字を区別します。");
    }
}


void cmd_help(TEXT* t) {
    if (vz.help_active) {
        vz.help_active = 0;
        set_notification("ヘルプを閉じました。");
    } else {
        vz.help_active = 1;
        set_notification("ヘルプを表示します。");
    }
}

void cmd_goto_line(TEXT* t) {
    char line_buf[16];
    int i;
    int line_num;
    if (!t) return;
    for (i = 0; i < 16; i++) line_buf[i] = '\0';
    if (vz_prompt_input("行番号: ", line_buf, 16)) {
        if (line_buf[0] != '\0') {
            /* 文字列→数値変換 */
            line_num = 0;
            for (i = 0; line_buf[i] != '\0'; i++) {
                if (line_buf[i] >= '0' && line_buf[i] <= '9') {
                    line_num = line_num * 10 + (line_buf[i] - '0');
                }
            }
            if (line_num > 0) {
                te_goto_line(t, line_num);
                set_notification("ジャンプしました。");
            } else {
                set_notification("無効な行番号です。");
            }
        } else {
            set_notification("キャンセルしました。");
        }
    } else {
        set_notification("キャンセルしました。");
    }
}
