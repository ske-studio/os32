/* ======================================================================== */
/*  MORE.C — ページャー (画面単位のテキスト表示)                              */
/*                                                                          */
/*  Usage: more [FILE]                                                       */
/*         cmd | more                                                        */
/*                                                                          */
/*  スペース: 次ページ  Enter: 次の1行  q: 終了                             */
/* ======================================================================== */
#include "os32api.h"
#include <string.h>
#include <stdio.h>

static KernelAPI *api;

/* 入力バッファ */
static char buf[65536];

/* 行ポインタテーブル (各行の先頭位置) */
#define MAX_LINES 4096
static const char *lines[MAX_LINES];
static int line_count = 0;

/* バッファを行に分割 */
static void split_lines(char *data, int len)
{
    int i;
    char *p = data;

    line_count = 0;
    if (len <= 0) return;

    lines[line_count++] = p;

    for (i = 0; i < len && line_count < MAX_LINES; i++) {
        if (data[i] == '\n') {
            data[i] = '\0';
            if (i + 1 < len) {
                lines[line_count++] = &data[i + 1];
            }
        } else if (data[i] == '\r') {
            data[i] = '\0';
        }
    }

    /* 末尾が空行の場合は除去 */
    if (line_count > 0 && lines[line_count - 1][0] == '\0') {
        line_count--;
    }
}

/* 1行を表示 (コンソール幅でクリップ) */
static void print_line(const char *line, int max_width)
{
    int len = strlen(line);
    if (len > max_width) len = max_width;
    if (len > 0) {
        api->sys_write(1, line, len);
    }
    api->sys_write(1, "\n", 1);
}

/* ステータス行を表示 */
static void show_prompt(int current_line, int total_lines)
{
    int pct;
    if (total_lines > 0) {
        pct = (current_line * 100) / total_lines;
        if (pct > 100) pct = 100;
    } else {
        pct = 100;
    }
    api->kprintf(ATTR_MAGENTA, "--More-- (%d%%)", pct);
}

/* ステータス行をクリア */
static void clear_prompt(void)
{
    api->sys_write(1, "\r", 1);
    api->sys_write(1, "                    ", 20);
    api->sys_write(1, "\r", 1);
}

/* キー入力待ち (stdinではなくKAPIの直接キーボードアクセス) */
static int wait_key(void)
{
    return api->kbd_getchar();
}

int main(int argc, char **argv, KernelAPI *kapi)
{
    int sz = 0;
    int con_w, con_h;
    int page_lines;
    int current_line;

    api = kapi;

    /* コンソールサイズ取得 */
    api->console_get_size(&con_w, &con_h);
    if (con_h < 5) con_h = 25;
    if (con_w < 20) con_w = 80;
    page_lines = con_h - 2; /* ステータス行 + 余白分 */

    if (argc >= 2) {
        /* ファイルから読み込み */
        int fd = api->sys_open(argv[1], KAPI_O_RDONLY);
        if (fd < 0) {
            printf("more: %s: No such file\n", argv[1]);
            return 1;
        }
        sz = api->sys_read(fd, buf, sizeof(buf) - 1);
        api->sys_close(fd);
    } else {
        /* stdin から読み込み (パイプ入力) */
        if (api->sys_isatty(0)) {
            printf("Usage: more [FILE]\n");
            printf("       cmd | more\n");
            return 1;
        }
        sz = api->sys_read(0, buf, sizeof(buf) - 1);
    }

    if (sz <= 0) return 0;
    buf[sz] = '\0';

    /* 行に分割 */
    split_lines(buf, sz);

    if (line_count == 0) return 0;

    /* stdoutが端末でない場合はそのまま全部出力 (more | grep 等) */
    if (!api->sys_isatty(1)) {
        int i;
        for (i = 0; i < line_count; i++) {
            printf("%s\n", lines[i]);
        }
        return 0;
    }

    /* ページ表示ループ */
    current_line = 0;

    while (current_line < line_count) {
        int i;
        int lines_to_show;
        int ch;

        /* 1ページ分の行を表示 */
        if (current_line == 0) {
            lines_to_show = page_lines;
        } else {
            lines_to_show = page_lines;
        }

        for (i = 0; i < lines_to_show && current_line < line_count; i++) {
            print_line(lines[current_line], con_w);
            current_line++;
        }

        /* 最終行まで表示済みなら終了 */
        if (current_line >= line_count) break;

        /* --More-- プロンプト表示 */
        show_prompt(current_line, line_count);

        /* キー入力待ち */
        ch = wait_key();

        /* プロンプトクリア */
        clear_prompt();

        if (ch == 'q' || ch == 'Q' || ch == 0x1B) {
            /* q または ESC で終了 */
            break;
        } else if (ch == '\r' || ch == '\n') {
            /* Enter: 1行進む */
            /* 次のループで1行だけ表示するため lines_to_show を調整 */
            /* → 実際にはループ先頭で page_lines 分表示するので、
               ここでは1行だけ表示して再プロンプト */
            if (current_line < line_count) {
                print_line(lines[current_line], con_w);
                current_line++;
                /* まだ残りがあればプロンプトを再表示 (ループに戻る) */
                if (current_line < line_count) {
                    show_prompt(current_line, line_count);
                    goto wait_again;
                }
            }
            continue;
        }
        /* スペースや他のキー: 次ページ */
        continue;

wait_again:
        ch = wait_key();
        clear_prompt();
        if (ch == 'q' || ch == 'Q' || ch == 0x1B) break;
        if (ch == '\r' || ch == '\n') {
            /* Enter: もう1行進む → ループ先頭に戻る代わりにここで処理 */
            if (current_line < line_count) {
                print_line(lines[current_line], con_w);
                current_line++;
                if (current_line < line_count) goto wait_again;
            }
            continue;
        }
        /* スペース等: 次ページへ (ループ先頭に戻る) */
    }

    return 0;
}
