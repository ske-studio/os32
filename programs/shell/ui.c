#include "shell.h"

/* ======================================================================== */
/*  シェルメインループ・UI（履歴・補完）                                     */
/* ======================================================================== */

#define HIST_SIZE    16

static char hist_buf[HIST_SIZE][CMD_BUF_SIZE];
static int  hist_count = 0;
static int  hist_idx   = 0;
static int  prev_draw_len = 0;

static void hist_add(const char *s) {
    int i;
    if (str_len(s) == 0) return;
    if (hist_count > 0 && str_eq(hist_buf[(hist_count - 1) % HIST_SIZE], s)) return;
    i = hist_count % HIST_SIZE;
    {
        int j;
        for (j = 0; s[j] && j < CMD_BUF_SIZE - 1; j++) hist_buf[i][j] = s[j];
        hist_buf[i][j] = 0;
    }
    hist_count++;
}

static void show_prompt(void) {
    g_api->kprintf(ATTR_GREEN, "OS32");
    g_api->kprintf(ATTR_GREEN, ":");
    g_api->kprintf(ATTR_CYAN, "%s", g_api->sys_getcwd());
    g_api->kprintf(ATTR_WHITE, "$ ");
}

static void redraw_line(const char *buf, int len, int cursor) {
    int i, cl;
    g_api->console_set_cursor(0, g_api->console_get_cursor_y());
    show_prompt();
    for (i = 0; i < len; i++) g_api->shell_putchar(buf[i], ATTR_WHITE);
    cl = prev_draw_len - len;
    for (i = 0; i < cl; i++) g_api->shell_putchar(' ', ATTR_WHITE);
    prev_draw_len = len;

    g_api->console_set_cursor(0, g_api->console_get_cursor_y());
    show_prompt();
    for (i = 0; i < cursor; i++) g_api->console_set_cursor(g_api->console_get_cursor_x() + 1, g_api->console_get_cursor_y());
}

static int tab_complete(char *buf, int pos, int show_candidates) {
    const char *matches[20];
    int match_count = 0, i, j, common_len;

    if (pos == 0) return pos;
    for (i = 0; cmd_names[i]; i++) {
        int ok = 1;
        for (j = 0; j < pos; j++) if (cmd_names[i][j] == 0 || cmd_names[i][j] != buf[j]) { ok = 0; break; }
        if (ok && match_count < 20) matches[match_count++] = cmd_names[i];
    }
    if (match_count == 0) return pos;
    if (match_count == 1) {
        i = 0;
        while (matches[0][i] && i < CMD_BUF_SIZE - 2) { buf[i] = matches[0][i]; i++; }
        buf[i++] = ' '; buf[i] = 0;
        return i;
    }
    common_len = pos;
    for (;;) {
        char c = matches[0][common_len]; int ok = 1;
        if (c == 0) break;
        for (i = 1; i < match_count; i++) if (matches[i][common_len] != c) { ok = 0; break; }
        if (!ok) break;
        common_len++;
    }
    if (common_len > pos) {
        for (i = pos; i < common_len && i < CMD_BUF_SIZE - 1; i++) buf[i] = matches[0][i];
        buf[common_len] = 0;
        return common_len;
    }
    if (show_candidates) {
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
        for (i = 0; i < match_count; i++) g_api->kprintf(ATTR_CYAN, "  %s", matches[i]);
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
        return -1;
    }
    return pos;
}

void shell_run(void) {
    char cmd_buf[CMD_BUF_SIZE];
    int cmd_pos, cmd_len, key, last_tab = 0;

    g_api->kprintf(ATTR_CYAN, "%s", "================================\n");
    g_api->kprintf(ATTR_CYAN, "%s", " OS32 External Shell Started\n");
    g_api->kprintf(ATTR_CYAN, "%s", "================================\n");

    /* 初期化時に自動シリアル＆rshell開始 */
    g_api->serial_init(38400);
    execute_command("rshell");

    for (;;) {
        show_prompt();
        cmd_pos = cmd_len = cmd_buf[0] = prev_draw_len = 0;
        hist_idx = hist_count;

        for (;;) {
            key = g_api->kbd_getkey();

            if ((key & 0xFF) == 0x0D) { g_api->shell_putchar('\n', ATTR_WHITE); break; }
            if ((key & 0xFF) == 0x08) {
                if (cmd_pos > 0) {
                    if (cmd_pos == cmd_len) {
                        cmd_pos--; cmd_len--; cmd_buf[cmd_len] = 0;
                        g_api->shell_putchar(0x08, ATTR_WHITE); prev_draw_len = cmd_len;
                    } else {
                        int i; for (i = cmd_pos - 1; i < cmd_len - 1; i++) cmd_buf[i] = cmd_buf[i+1];
                        cmd_len--; cmd_pos--; cmd_buf[cmd_len] = 0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                    }
                }
                continue;
            }
            if ((key >> 8) == 0x3A) { /* UP */
                if (hist_idx > 0 && hist_idx > hist_count - HIST_SIZE) {
                    int i = (--hist_idx) % HIST_SIZE; cmd_len = 0;
                    while (hist_buf[i][cmd_len]) { cmd_buf[cmd_len] = hist_buf[i][cmd_len]; cmd_len++; }
                    cmd_buf[cmd_len] = 0; cmd_pos = cmd_len; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x3D) { /* DOWN */
                if (hist_idx < hist_count - 1) {
                    int i = (++hist_idx) % HIST_SIZE; cmd_len = 0;
                    while (hist_buf[i][cmd_len]) { cmd_buf[cmd_len] = hist_buf[i][cmd_len]; cmd_len++; }
                    cmd_buf[cmd_len] = 0; cmd_pos = cmd_len; redraw_line(cmd_buf, cmd_len, cmd_pos);
                } else {
                    hist_idx = hist_count; cmd_len = cmd_pos = cmd_buf[0] = 0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x3B) { /* LEFT */
                if (cmd_pos > 0) { cmd_pos--; g_api->shell_putchar(0x08, ATTR_WHITE); } continue;
            }
            if ((key >> 8) == 0x3C) { /* RIGHT */
                if (cmd_pos < cmd_len) { g_api->shell_putchar(cmd_buf[cmd_pos], ATTR_WHITE); cmd_pos++; } continue;
            }
            if ((key >> 8) == 0x3E) { /* HOME */
                while (cmd_pos > 0) { cmd_pos--; g_api->shell_putchar(0x08, ATTR_WHITE); } continue;
            }
            if ((key >> 8) == 0x39) { /* DEL */
                if (cmd_pos < cmd_len) {
                    int i; for(i=cmd_pos; i<cmd_len-1; i++) cmd_buf[i]=cmd_buf[i+1];
                    cmd_len--; cmd_buf[cmd_len]=0; redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
                continue;
            }
            if ((key >> 8) == 0x0F || (key & 0xFF) == 0x09) { /* TAB */
                int npos = tab_complete(cmd_buf, cmd_len, last_tab);
                if (npos == -1) { show_prompt(); redraw_line(cmd_buf, cmd_len, cmd_pos); }
                else if (npos != cmd_len) { cmd_len = cmd_pos = npos; redraw_line(cmd_buf, cmd_len, cmd_pos); last_tab = 0; }
                else last_tab = 1;
                continue;
            }
            last_tab = 0;
            if ((key & 0xFF) == 0x1B) { cmd_len=cmd_pos=cmd_buf[0]=0; redraw_line(cmd_buf, cmd_len, cmd_pos); continue; }
            if ((key & 0xFF) >= 0x20 && (key & 0xFF) < 0x7F && cmd_len < CMD_BUF_SIZE - 1) {
                char ch = (char)(key & 0xFF);
                if (cmd_pos == cmd_len) {
                    cmd_buf[cmd_pos++] = ch; cmd_len++; cmd_buf[cmd_len] = 0;
                    g_api->shell_putchar(ch, ATTR_WHITE); prev_draw_len = cmd_len;
                } else {
                    int i; for (i = cmd_len; i > cmd_pos; i--) cmd_buf[i] = cmd_buf[i-1];
                    cmd_buf[cmd_pos++] = ch; cmd_len++; cmd_buf[cmd_len] = 0;
                    redraw_line(cmd_buf, cmd_len, cmd_pos);
                }
            }
        }
        cmd_buf[cmd_len] = 0;
        if (cmd_len > 0) hist_add(cmd_buf);
        execute_command(cmd_buf);
    }
}
