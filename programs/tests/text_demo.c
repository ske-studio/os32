/* ======================================================================== */
/*  TEXT_DEMO.C — libos32text ビジュアルデモ                                */
/*                                                                          */
/*  RPG風メッセージウィンドウをPC-98 640x400画面に描画する。                 */
/*  libos32text のタイプライター演出・ページ送り・グループ会話を              */
/*  libos32gfx で可視化するインタラクティブデモ。                            */
/*                                                                          */
/*  操作:                                                                    */
/*    SPACE  — ページ送り / 次メッセージ                                     */
/*    S      — スキップ (全文即時表示)                                       */
/*    1-5    — デモシナリオ選択                                               */
/*    +/-    — 表示速度変更                                                   */
/*    ESC    — 終了                                                           */
/* ======================================================================== */

#include "os32api.h"
#include "libos32gfx.h"
#include "gfx_font.h"
#include "libos32text.h"
#include "libos32db.h"

extern KernelAPI *kapi;
#define api kapi

/* ====================================================================== */
/*  レイアウト (PC-98 640x400)                                             */
/* ====================================================================== */

/* メッセージウィンドウ */
#define MSGWIN_X       32
#define MSGWIN_Y       280
#define MSGWIN_W       576
#define MSGWIN_H       100
#define MSGWIN_PAD     12

/* 話者名プレート */
#define SPEAKER_X      44
#define SPEAKER_Y      262
#define SPEAKER_W      120
#define SPEAKER_H      20

/* ステータスバー */
#define STATUS_Y       10

/* テキスト描画エリア */
#define TEXT_X         (MSGWIN_X + MSGWIN_PAD)
#define TEXT_Y         (MSGWIN_Y + MSGWIN_PAD + 4)
#define TEXT_LINE_H    18
#define TEXT_MAX_LINE_W (MSGWIN_W - MSGWIN_PAD * 2)

/* 色 (PC-98 4bit パレット) */
#define C_BG         0
#define C_WIN_BG     1     /* 紺: ウィンドウ背景 */
#define C_WIN_FRAME  7     /* 白: ウィンドウ枠 */
#define C_TEXT       7     /* 白: メッセージテキスト */
#define C_SPEAKER_BG 4     /* 茶: 話者名背景 */
#define C_SPEAKER_FG 15    /* 明白: 話者名テキスト */
#define C_STATUS     5     /* グレー: ステータス */
#define C_CURSOR     11    /* ライトグリーン: 入力待ちカーソル */
#define C_TITLE      6     /* シアン: タイトル */
#define C_HELP       3     /* 緑: ヘルプ */
#define C_SCENE_BG   2     /* 暗い: シーン背景 */
#define C_DIM        5     /* グレー */
#define C_HIGHLIGHT  14    /* ライトイエロー */

/* スキャンコード */
#define SC_ESC     0x00
#define SC_1       0x01
#define SC_2       0x02
#define SC_3       0x03
#define SC_4       0x04
#define SC_5       0x05
#define SC_S       0x1F
#define SC_SPACE   0x35
#define SC_PLUS    0x26    /* ; (+ のシフトなし) */
#define SC_MINUS   0x0C

/* ====================================================================== */
/*  グローバル                                                             */
/* ====================================================================== */

static int g_running;
static int g_frame;
static int g_demo_mode;         /* 1-5: 現在のデモシナリオ */

/* キー入力 */
#define NUM_KEYS 9
static const int g_keylist[] = {
    SC_ESC, SC_1, SC_2, SC_3, SC_4, SC_5, SC_S, SC_SPACE, SC_MINUS
};
enum { KI_ESC=0, KI_1, KI_2, KI_3, KI_4, KI_5, KI_S, KI_SPACE, KI_MINUS };
static int g_cur[NUM_KEYS];
static int g_prev[NUM_KEYS];

static void keys_poll(void)
{
    int i;
    for (i = 0; i < NUM_KEYS; i++)
        g_cur[i] = api->kbd_is_pressed(g_keylist[i]);
}

static int key_trigger(int idx)
{
    return g_cur[idx] && !g_prev[idx];
}

static void keys_save(void)
{
    int i;
    for (i = 0; i < NUM_KEYS; i++)
        g_prev[i] = g_cur[i];
}

/* ====================================================================== */
/*  完了コールバック                                                       */
/* ====================================================================== */

static int g_done_slot;
static u16 g_done_msg;
static int g_done_flag;

static void on_done(int slot, u16 msg_id)
{
    g_done_slot = slot;
    g_done_msg = msg_id;
    g_done_flag = 1;
}

/* ====================================================================== */
/*  デモシナリオ                                                           */
/* ====================================================================== */

static const char *demo_titles[] = {
    "",
    "1: ASCII (Hello, World!)",
    "2: Japanese Text",
    "3: Variable Expansion",
    "4: Multi-Page (\\p)",
    "5: Group Conversation"
};

static void start_demo(int mode)
{
    g_demo_mode = mode;
    g_done_flag = 0;
    text_close(0);

    switch (mode) {
    case 1:
        /* 単純ASCII */
        text_load(0, 1);
        break;
    case 2:
        /* 日本語テスト */
        text_load(0, 2);
        break;
    case 3:
        /* 変数展開 */
        text_set_var(0, "Hero");
        text_set_var(1, "Sword");
        text_load(0, 3);
        break;
    case 4:
        /* マルチページ */
        text_load(0, 4);
        break;
    case 5:
        /* グループ会話 (オープニング) */
        text_set_var(0, "Hero");
        text_load_group(0, 1);
        break;
    default:
        break;
    }
}

/* ====================================================================== */
/*  描画ヘルパー                                                           */
/* ====================================================================== */

/* UTF-8文字列をnバイトだけ描画 (KCGフォント使用) */
static void draw_text_n(int x, int y, const char *text, int nbytes,
                         u8 fg, u8 bg)
{
    int i, cx, cy;
    int line_w;

    if (!text || nbytes <= 0)
        return;

    cx = x;
    cy = y;
    line_w = 0;

    for (i = 0; i < nbytes; ) {
        u8 lead = (u8)text[i];
        int char_w;
        int char_bytes;

        /* UTF-8のバイト数判定 */
        if (lead < 0x80) {
            char_bytes = 1;
            char_w = 8;   /* ASCII: 8px幅 */
        } else if ((lead & 0xE0) == 0xC0) {
            char_bytes = 2;
            char_w = 16;
        } else if ((lead & 0xF0) == 0xE0) {
            char_bytes = 3;
            char_w = 16;  /* 日本語: 16px幅 */
        } else if ((lead & 0xF8) == 0xF0) {
            char_bytes = 4;
            char_w = 16;
        } else {
            char_bytes = 1;
            char_w = 8;
        }

        /* バッファオーバーラン防止 */
        if (i + char_bytes > nbytes)
            break;

        /* 行の折り返し */
        if (line_w + char_w > TEXT_MAX_LINE_W) {
            cx = x;
            cy += TEXT_LINE_H;
            line_w = 0;
        }

        /* 1文字描画用: 一時的にNUL終端文字列を作る */
        {
            char tmp[8];
            int j;
            for (j = 0; j < char_bytes && j < 7; j++)
                tmp[j] = text[i + j];
            tmp[j] = '\0';

            kcg_draw_utf8(cx, cy, tmp, fg, bg);
        }

        cx += char_w;
        line_w += char_w;
        i += char_bytes;
    }
}

/* 入力待ちカーソル (点滅する▼) */
static void draw_wait_cursor(void)
{
    if ((g_frame / 15) & 1) {
        gfx_putchar(MSGWIN_X + MSGWIN_W - 28,
                     MSGWIN_Y + MSGWIN_H - 16,
                     'v', C_CURSOR);
    }
}

/* ====================================================================== */
/*  メインUI描画                                                           */
/* ====================================================================== */

/* シーン背景 (簡易RPG風) */
static void draw_scene_bg(void)
{
    /* 空 */
    gfx_fill_rect(0, 0, 640, 200, C_BG);

    /* 地面 */
    gfx_fill_rect(0, 200, 640, 70, C_SCENE_BG);

    /* 地平線 */
    gfx_hline(0, 200, 640, C_DIM);

    /* 星 (簡易) */
    {
        int sx[] = {50, 150, 280, 400, 530, 90, 320, 580, 200, 470};
        int sy[] = {30, 60, 20, 50, 35, 90, 80, 70, 110, 100};
        int i;
        for (i = 0; i < 10; i++) {
            if ((g_frame / 30 + i) % 3 != 0) {
                gfx_pixel(sx[i], sy[i], C_TEXT);
            }
        }
    }
}

/* メッセージウィンドウ */
static void draw_msgwin(void)
{
    u8 state;
    const char *speaker;
    const char *vis;
    int vis_len;

    state = text_get_state(0);
    if (state == TEXT_STATE_IDLE)
        return;

    /* ウィンドウ背景 */
    gfx_fill_rect(MSGWIN_X, MSGWIN_Y, MSGWIN_W, MSGWIN_H, C_WIN_BG);

    /* 外枠 (二重線) */
    gfx_rect(MSGWIN_X, MSGWIN_Y, MSGWIN_W, MSGWIN_H, C_WIN_FRAME);
    gfx_rect(MSGWIN_X + 2, MSGWIN_Y + 2,
             MSGWIN_W - 4, MSGWIN_H - 4, C_WIN_FRAME);

    /* 話者名プレート */
    speaker = text_get_speaker(0);
    if (speaker && speaker[0] != '\0') {
        gfx_fill_rect(SPEAKER_X, SPEAKER_Y, SPEAKER_W, SPEAKER_H,
                       C_SPEAKER_BG);
        gfx_rect(SPEAKER_X, SPEAKER_Y, SPEAKER_W, SPEAKER_H, C_WIN_FRAME);
        kcg_draw_utf8(SPEAKER_X + 8, SPEAKER_Y + 2, speaker,
                       C_SPEAKER_FG, C_SPEAKER_BG);
    }

    /* テキスト本文 (表示済み部分のみ) */
    vis = text_get_visible(0, &vis_len);
    if (vis && vis_len > 0) {
        draw_text_n(TEXT_X, TEXT_Y, vis, vis_len, C_TEXT, C_WIN_BG);
    }

    /* ページ番号 */
    if (text_get_page_count(0) > 1) {
        gfx_printf(MSGWIN_X + MSGWIN_W - 60, MSGWIN_Y + 6, C_DIM,
                   "%d/%d", text_get_page(0) + 1, text_get_page_count(0));
    }

    /* 入力待ちカーソル */
    if (state == TEXT_STATE_WAIT) {
        draw_wait_cursor();
    }

    /* 完了表示 */
    if (state == TEXT_STATE_DONE) {
        gfx_puts(MSGWIN_X + MSGWIN_W - 48, MSGWIN_Y + MSGWIN_H - 14,
                 "[END]", C_CURSOR);
    }
}

/* ステータスバー */
static void draw_status(void)
{
    u8 state;
    const char *state_name;

    /* タイトル */
    gfx_puts(16, STATUS_Y, "libos32text DEMO", C_TITLE);

    /* 現在のデモモード */
    if (g_demo_mode >= 1 && g_demo_mode <= 5)
        gfx_puts(200, STATUS_Y, demo_titles[g_demo_mode], C_TEXT);

    /* 状態表示 */
    state = text_get_state(0);
    switch (state) {
    case TEXT_STATE_IDLE:   state_name = "IDLE";   break;
    case TEXT_STATE_TYPING: state_name = "TYPING"; break;
    case TEXT_STATE_PAUSE:  state_name = "PAUSE";  break;
    case TEXT_STATE_WAIT:   state_name = "WAIT";   break;
    case TEXT_STATE_DONE:   state_name = "DONE";   break;
    default:                state_name = "???";    break;
    }
    gfx_printf(16, STATUS_Y + 16, C_STATUS,
               "State: %s  Speed: %d", state_name,
               text_is_active(0) ? (int)text_get_state(0) : 0);

    /* メッセージID */
    if (text_is_active(0)) {
        gfx_printf(250, STATUS_Y + 16, C_STATUS,
                   "MSG: %d  Frame: %d", text_get_msg_id(0), g_frame);
    }
}

/* ヘルプ表示 */
static void draw_help(void)
{
    int y = 140;
    gfx_puts(16, y, "=== Controls ===", C_HELP);
    y += 14;
    gfx_puts(16, y, "1-5: Select Demo", C_DIM);
    y += 12;
    gfx_puts(16, y, "SPACE: Advance", C_DIM);
    y += 12;
    gfx_puts(16, y, "S: Skip", C_DIM);
    y += 12;
    gfx_puts(16, y, "ESC: Quit", C_DIM);
}

/* ====================================================================== */
/*  メインループ                                                           */
/* ====================================================================== */

static void game_loop(void)
{
    u32 last_tick;

    g_running = 1;
    g_frame = 0;
    last_tick = api->get_tick();

    /* 初期デモ: グループ会話 */
    start_demo(5);

    while (g_running) {
        u8 state;
        g_frame++;

        /* --- 入力 --- */
        keys_poll();

        if (key_trigger(KI_ESC)) {
            g_running = 0;
            break;
        }

        /* デモ選択 */
        if (key_trigger(KI_1)) start_demo(1);
        if (key_trigger(KI_2)) start_demo(2);
        if (key_trigger(KI_3)) start_demo(3);
        if (key_trigger(KI_4)) start_demo(4);
        if (key_trigger(KI_5)) start_demo(5);

        /* テキスト操作 */
        state = text_get_state(0);

        if (key_trigger(KI_S)) {
            /* スキップ */
            if (state == TEXT_STATE_TYPING || state == TEXT_STATE_PAUSE) {
                text_skip(0);
            }
        }

        if (key_trigger(KI_SPACE)) {
            if (state == TEXT_STATE_TYPING || state == TEXT_STATE_PAUSE) {
                /* タイプ中ならスキップ */
                text_skip(0);
            } else if (state == TEXT_STATE_WAIT) {
                /* ページ送り */
                text_advance(0);
            } else if (state == TEXT_STATE_DONE) {
                /* 完了 → グループなら次メッセージ */
                if (g_demo_mode == 5) {
                    int rc = text_next_message(0);
                    if (rc == TEXT_ERR_END) {
                        /* グループ終端 → 閉じてヘルプ表示 */
                        text_close(0);
                    }
                } else {
                    text_close(0);
                }
            }
        }

        keys_save();

        /* --- テキストエンジン更新 --- */
        text_update();

        /* --- 描画 --- */
        gfx_clear(C_BG);
        draw_scene_bg();
        draw_status();
        draw_help();
        draw_msgwin();
        api->gfx_present_dirty();

        /* フレーム待ち (~30fps) */
        while (api->get_tick() - last_tick < 3) {
            api->sys_halt();
        }
        last_tick = api->get_tick();
    }
}

/* ====================================================================== */
/*  main                                                                   */
/* ====================================================================== */

int main(int argc, char **argv, KernelAPI *k)
{
    int rc;
    (void)argc; (void)argv; (void)k;

    api->kprintf(ATTR_WHITE, "text_demo: initializing...\\n");

    /* テキストエンジン初期化 */
    rc = text_init("/db/text.db");
    if (rc < 0) {
        api->kprintf(ATTR_RED, "text_demo: text_init failed (%d)\\n", rc);
        return 1;
    }

    /* 完了コールバック設定 */
    text_set_done_callback(on_done);

    /* GFX初期化 */
    libos32gfx_init(kapi);

    /* KCGフォントスケール (1x = 8x16ドット) */
    kcg_set_scale(1);

    /* メインループ */
    game_loop();

    /* 終了処理 */
    libos32gfx_shutdown();
    text_shutdown();

    api->kprintf(ATTR_WHITE, "text_demo: done.\\n");
    return 0;
}
