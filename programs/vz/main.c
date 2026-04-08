/*
 * main.c - OS32 VZ Editor メインエントリ
 * C89 compatible
 */

#include "vz.h"

/* グローバルKernelAPIポインタ */
KernelAPI* kapi;

/* 前方宣言 */
int vz_init(void);
static void vz_run_loop(void);
void vz_load_file(const char* path);
int vz_prompt_input(const char* prompt, char* buf, int buf_size);

/* 通知メッセージ表示ヘルパー */
void set_notification(const char* msg) {
    int i;
    for (i = 0; msg[i] != '\0' && i < 63; i++) {
        vz.notification[i] = msg[i];
    }
    vz.notification[i] = '\0';
    vz.notification_timer = 2; /* 2操作分保持 */
}

/*
 * main
 * OS32 VZ Editor のエントリポイント
 */
int main(int argc, char **argv, KernelAPI *api)
{
    int i;
    TEXT* t;

    /* KernelAPIポインタを初期化 */
    kapi = api;

    /* VZエディタ初期化 */
    if (!vz_init()) {
        return 1;
    }
    
    /* gfxエンジンの初期化 (VZ向け最小構成: スプライト不要) */
    {
        extern KernelAPI *gfx_api;
        extern GFX_Framebuffer gfx_fb;
        gfx_api = api;
        kapi->gfx_init();
        kapi->gfx_get_framebuffer(&gfx_fb);
        kapi->kcg_init();
        lcons_init();
    }

    macro_init(); /* マクロエンジン初期化 */

    t = (TEXT*)os32_malloc(sizeof(TEXT));
    te_init_buffer(t);
    vz.current_text = t;

    /* コマンドライン引数からファイルを読み込む */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            vz_load_file(argv[i]);
            break; /* 最初のファイルのみ */
        }
    }

    /* メインループへ */
    vz.insert_mode = 1; /* デフォルトは挿入モード */
    vz_run_loop();

    return 0;
}

/* vz_prompt_input moved to core.c */

/*
 * ファイルをテキストバッファに読み込む
 */
void vz_load_file(const char* path)
{
    int fd;
    unsigned int size;
    TEXT* t;

    fd = vz_open(path, 0);
    if (fd < 0) return;

    size = vz_get_size(fd);

    t = (TEXT*)os32_malloc(sizeof(TEXT));
    if (!t) {
        vz_close(fd);
        return;
    }

    /* TEXT構造体をゼロ初期化 */
    {
        unsigned char *p = (unsigned char *)t;
        int j;
        for (j = 0; j < (int)sizeof(TEXT); j++) p[j] = 0;
    }

    te_init_buffer(t);

    if (t->ttop && size > 0) {
        if (size >= t->tbmax) {
            /* tbmaxより大きい場合はバッファを拡張 */
            if (!te_resize_buffer(t, size + TE_INITIAL_BUFFER_SIZE)) {
                vz_close(fd);
                os32_free(t);
                return;
            }
        }
        
        /* ギャップバッファの末尾側にファイルデータを読み込む */
        {
            char* data_start = (char*)t->tmax - size;
            vz_read(fd, data_start, size);
            t->tend = (void*)data_start;
        }
    }
    vz_close(fd);

    /* ファイル名を保存 */
    {
        int k;
        for (k = 0; path[k] != '\0' && k < PATHSZ - 1; k++) {
            t->path[k] = path[k];
        }
        t->path[k] = '\0';
        t->namep = t->path;
    }

    if (vz.text_count < MAX_TEXTS) {
        t->wnum = vz.text_count;
        vz.text_list[vz.text_count] = t;
        vz.window_text_idx[vz.active_window] = vz.text_count;
        vz.text_count++;
        vz.current_text = t;
    } else {
        /* すでに上限なら現在のウインドウを上書き (ここは後で改善) */
        vz.current_text = t;
        vz.text_list[vz.window_text_idx[vz.active_window]] = t;
    }
}

/* vz_open_filer is in filer.c */

/* ディスパッチテーブル */
static KeyMap command_table[] = {
    {1, 'd', cmd_save_and_quit},
    {1, 's', cmd_save},
    {1, 'w', cmd_save_as},
    {1, 'r', cmd_open_file},
    {1, 'f', cmd_filer},
    {1, 't', cmd_swap_text},
    {1, 'e', cmd_swap_window},
    {1, 'o', cmd_toggle_split},
    {1, 'b', cmd_block_start},
    {1, 'k', cmd_block_end},
    {1, 'h', cmd_help},
    {1, 'c', cmd_block_copy},
    {1, 'y', cmd_block_delete},
    {1, 'u', cmd_redo},
    {2, 'r', cmd_top},
    {2, 'c', cmd_bottom},
    {2, 'f', cmd_search_forward},
    {2, 'b', cmd_search_backward},
    {2, 'a', cmd_replace},
    {2, 'g', cmd_replace_all},
    {2, 'i', cmd_toggle_case},
    {2, 'j', cmd_goto_line},
    {0, 0, NULL}
};

/*
 * 高レベルコマンド実行 (Ctrl-K / Ctrl-Q 系)
 */
static void execute_high_level_command(int prefix, int c, int* need_redraw) {
    int i;
    int key = c;
    if (key >= 'A' && key <= 'Z') key = key - 'A' + 'a';
    *need_redraw = 1;

    for (i = 0; command_table[i].func != NULL; i++) {
        if (command_table[i].prefix == prefix && command_table[i].key == key) {
            command_table[i].func(vz.current_text);
            return;
        }
    }
    set_notification("キャンセルしました。");
}

/*
 * エディタメインループ
 */
static void vz_run_loop(void)
{
    int loop_run = 1;
    int prefix_ctrl_k = 0;
    int prefix_ctrl_q = 0;
    TEXT* t = vz.current_text;

    /* ファイル未読み込み時は空バッファを作成 */
    if (!t) {
        t = (TEXT*)os32_malloc(sizeof(TEXT));
        if (!t) {
            kapi->kprintf(0x41, "%s", "VZ: alloc failed\n");
            return;
        }
        {
            unsigned char *p = (unsigned char *)t;
            int j;
            for (j = 0; j < (int)sizeof(TEXT); j++) p[j] = 0;
        }
        te_init_buffer(t);
        if (!t->ttop) {
            kapi->kprintf(0x41, "%s", "VZ: buf alloc failed\n");
            return;
        }
        t->wnum = 0; /* ウインドウ番号初期化 */
        vz.current_text = t;
    }

    if (vz.text_count == 0) {
        vz.text_list[0] = t;
        vz.text_count = 1;
        vz.window_text_idx[0] = 0;
        vz.window_text_idx[1] = 0;
        t->wnum = 0;
    }

    /* 初期画面描画 */
    vz_clear();
    su_redraw_screen();

    while (loop_run) {
        if (vz.macro_env.running) {
            /* マクロ実行中 */
            macro_step();
            su_redraw_screen();
            continue;
        }

        if (vz_kbhit()) {
            int c = vz_getch();   /* キーコードデータ: 上位=スキャンコード, 下位=ASCII */
            int kc = KEYCODE(c);  /* PC-98スキャンコード */
            int kd = KEYDATA(c);  /* ASCIIキーデータ */
            int redraw = 0;

            if (vz.notification_timer > 0) {
                vz.notification_timer--;
                if (vz.notification_timer == 0) {
                    vz.notification[0] = '\0';
                    redraw = 1;
                }
            }

            if (vz.help_active) {
                vz.help_active = 0;
                set_notification("\x83\x77\x83\x8B\x83\x76\x82\xF0\x95\xC2\x82\xB6\x82\xDC\x82\xB5\x82\xBD\x81\x42"); /* ヘルプを閉じました。 */
                su_redraw_screen();
                continue;
            }

            if (vz.filer.active) {
                if (vz_filer_handle_key(c)) {
                    su_redraw_screen();
                }
                continue;
            }

            if (prefix_ctrl_k || prefix_ctrl_q) {
                int pfx = prefix_ctrl_k ? 1 : 2;
                prefix_ctrl_k = 0;
                prefix_ctrl_q = 0;
                execute_high_level_command(pfx, kd, &redraw);
                if (vz_quit_flag) {
                    loop_run = 0; 
                }
                if (redraw) {
                    su_redraw_screen();
                }
                continue;
            }

            /* --- ファンクションキー処理 (スキャンコードで判定) --- */
            if (kc == VK_F1) {
                cmd_help(vz.current_text);
                su_redraw_screen();
                continue;
            }
            if (kc == VK_ROLLUP) {
                te_move_line(vz.current_text, -(vz.current_text->tw_sy));
                su_redraw_screen();
                continue;
            }
            if (kc == VK_ROLLDOWN) {
                te_move_line(vz.current_text, vz.current_text->tw_sy);
                su_redraw_screen();
                continue;
            }
            if (kc == VK_HOME) {
                te_move_to_top(vz.current_text);
                su_redraw_screen();
                continue;
            }
            if (kc == VK_HELP) {
                cmd_help(vz.current_text);
                su_redraw_screen();
                continue;
            }
            if (kc == VK_INS) {
                vz.insert_mode = !vz.insert_mode;
                set_notification(vz.insert_mode ? "INS" : "OVR");
                su_redraw_screen();
                continue;
            }

            /* --- ASCII/コントロールコードで判定 --- */
            if (kd == 0x1B) {
                /* ESC — エディタ終了 */
                loop_run = 0;
            } else if (kd == 0x0B) {
                /* Ctrl-K — プレフィックス */
                prefix_ctrl_k = 1;
                set_notification("^K");
                redraw = 1;
            } else if (kd == 0x11) {
                /* Ctrl-Q — プレフィックス */
                prefix_ctrl_q = 1;
                set_notification("^Q");
                redraw = 1;
            } else {
                /* マクロエンジンへ: ASCII部だけを渡す */
                /* 特殊キー(矢印キー等)はスキャンコード→仮想ASCII変換 */
                int mac_key = kd;
                if (kd == 0) {
                    /* ASCII=0の特殊キーはスキャンコードから仮想キーコードに変換 */
                    switch (kc) {
                    case VK_UP:    mac_key = 0x1E; break; /* ↑ */
                    case VK_DOWN:  mac_key = 0x1F; break; /* ↓ */
                    case VK_LEFT:  mac_key = 0x1D; break; /* ← */
                    case VK_RIGHT: mac_key = 0x1C; break; /* → */
                    case VK_DEL:   mac_key = 0x7F; break; /* DEL */
                    case VK_BS:    mac_key = 0x08; break; /* BS */
                    default:       mac_key = 0;    break; /* 未対応キーは無視 */
                    }
                }
                if (mac_key != 0) {
                    static char temp_macro[2];
                    temp_macro[0] = (char)mac_key;
                    temp_macro[1] = '\0';
                    macro_start(temp_macro);
                }
            }

            if (redraw) {
                su_redraw_screen();
            }
        }
    }

    /* 終了時にクリアおよびVRAMの初期化(シャットダウン) */
    vz_clear();
    if (kapi) {
        kapi->gfx_shutdown();
    }
}

