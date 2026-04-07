/*
 * screen_ui.c - OS32 VZ Editor 画面UI (ステータスバー、プロンプト、ヘルプ)
 * C89 compatible
 */

#include "vz.h"

/* ステータスバー・レイアウト定数 */
#define STATUS_COORD_WIDTH       14 /* 行:列 表示エリアの幅 */
#define STATUS_FILE_OFFSET       20 /* ファイル名表示開始位置 (右端からのオフセット) */

/* PC-98 罫線・特殊文字 */
#define CH_VBAR      '|'           /* 縦線区切り */

/*
 * ステータスバー描画
 * 本物のVZ Editor形式: "P □  行:列                    |番号 ファイル名"
 */
void su_draw_statusbar(TEXT* w)
{
    int i;
    int line_num, col_num;
    int status_y = w->tw_py - 1;
    char buf[200];
    int bp = 0;
    
    if (status_y < 0) status_y = 0;

    buf[bp++] = vz.insert_mode ? 'P' : 'O';
    buf[bp++] = ' ';

    if (w->tchf == 0)      buf[bp++] = ' ';
    else if (w->tchf > 0)  buf[bp++] = '*';
    else                   buf[bp++] = 'R';
    buf[bp++] = ' ';

    line_num = w->lnumb;
    if (line_num < 1) line_num = 1;
    col_num = w->lx + 1;

    /* 行番号右寄せ5桁 */
    {
        char nb[16];
        int d = su_int_to_str(line_num, nb);
        for(i=0; i < 5 - d; i++) buf[bp++] = ' ';
        for(i=0; i < d; i++) buf[bp++] = nb[i];
    }
    buf[bp++] = ':';
    /* 列番号左寄せ */
    {
        char nb[16];
        int d = su_int_to_str(col_num, nb);
        int rem = STATUS_COORD_WIDTH - (5 + 1);
        for(i=0; i < d; i++) buf[bp++] = nb[i];
        for(i=d; i < rem; i++) buf[bp++] = ' ';
    }
    
    /* セパレータとウィンドウ番号 */
    int sep_x = SCREEN_W - STATUS_FILE_OFFSET;
    while(bp < sep_x) buf[bp++] = ' ';
    buf[bp++] = CH_VBAR;
    buf[bp++] = '1' + w->wnum;
    buf[bp++] = ' ';
    
    /* null terminate to use su_put_utf8_string for notification or file name */
    buf[bp] = '\0';
    
    int cx = su_put_string(0, status_y, buf, ATR_STATUS);
    
    if (vz.notification_timer > 0 && vz.notification[0] != '\0') {
        cx = su_put_utf8_string(cx, status_y, vz.notification, ATR_STATUS);
    } else if (w->namep && w->namep[0]) {
        cx = su_put_utf8_string(cx, status_y, w->namep, ATR_STATUS);
    } else {
        cx = su_put_utf8_string(cx, status_y, "(新規)", ATR_STATUS);
    }
    
    /* 残りをスペースで埋める */
    if (cx < SCREEN_W) {
        su_fill_rect(cx, status_y, SCREEN_W - cx, 1, ' ', ATR_STATUS);
    }
}

/*
 * ファンクションキーバー描画 (行24)
 * "ファイル 窓換  文換  窓割  記憶 | 検索  置換  カット インサート ブロック"
 */
void su_draw_funcbar(void)
{
    int x, i;
    /* UTF-8形式のファンクションキーラベル */
    static const char *fkey_labels[] = {
        "ファイル", "窓換", "文換", "窓割", "記憶",
        "", /* 区切り */
        "検索", "置換", "カット", "挿入", "ブロック"
    };
    static const int fkey_count = 11;

    /* 行全体を反転表示のスペースで埋める */
    char buf[200];
    int bp = 0;

    for (i = 0; i < fkey_count; i++) {
        if (fkey_labels[i][0] == '\0') {
            buf[bp++] = CH_VBAR;
        } else {
            int j;
            for(j=0; fkey_labels[i][j] != '\0'; j++) {
                buf[bp++] = fkey_labels[i][j];
            }
            if (i < fkey_count - 1) {
                buf[bp++] = ' ';
            }
        }
    }
    buf[bp] = '\0';
    
    x = su_put_utf8_string(0, FUNCBAR_Y, buf, ATR_FUNCBAR);
    if (x < SCREEN_W) {
        su_fill_rect(x, FUNCBAR_Y, SCREEN_W - x, 1, ' ', ATR_FUNCBAR);
    }
}

/*
 * ミニバッファ (プロンプト) の描画
 * ステータスバー行を使ってプロンプトを描画する
 */
void su_prompt(const char* prompt_msg, const char* input_buf, int cursor_pos)
{
    int x;
    int i;
    int status_y;
    
    if (!vz.current_text) return;
    status_y = vz.current_text->tw_py - 1;
    if (status_y < 0) status_y = 0;

    /* 行全体を反転表示のスペースで埋める */
    su_clear_line(status_y, ATR_STATUS);

    /* プロンプト文字を描画 */
    x = su_put_utf8_string(0, status_y, prompt_msg, ATR_STATUS);

    /* 入力文字列の処理（カーソル位置のみ属性を反転する） */
    if (cursor_pos > 0) {
        char before_cursor[128];
        int len = cursor_pos;
        if (len > 127) len = 127;
        for (i = 0; i < len; i++) before_cursor[i] = input_buf[i];
        before_cursor[len] = '\0';
        x = su_put_utf8_string(x, status_y, before_cursor, ATR_STATUS);
    }
    
    if (input_buf[cursor_pos] != '\0') {
        vz_putc(x++, status_y, input_buf[cursor_pos], ATR_NORMAL);
        if (input_buf[cursor_pos+1] != '\0') {
            x = su_put_utf8_string(x, status_y, &input_buf[cursor_pos+1], ATR_STATUS);
        }
    } else {
        vz_putc(x++, status_y, ' ', ATR_NORMAL);
    }
    
    if (x < SCREEN_W) {
        su_fill_rect(x, status_y, SCREEN_W - x, 1, ' ', ATR_STATUS);
    }
}

/*
 * ヘルプ（ショートカット一覧）画面の描画
 */
void su_draw_help(void) {
    int x, y;
    int box_w = 40;
    int box_h = 20;
    int start_x = (SCREEN_W - box_w) / 2;
    int start_y = (SCREEN_H - box_h) / 2;
    
    const char* lines[] = {
        " --- VZ Editor Help --- ",
        "",
        " [Cursor Movement]",
        "  Arrow Keys : Move",
        "  Ctrl-A/F   : Word Left/Right",
        "  Home/End   : Line Start/End",
        "  PageUp/Dn  : Scroll Page",
        "",
        " [Editing]",
        "  Ctrl-Y     : Delete Line",
        "  Enter      : New Line",
        "",
        " [File & Window (Ctrl-K + Key)]",
        "  ^K S / W   : Save / Save As",
        "  ^K R       : Open File",
        "  ^K F       : Open Filer",
        "  ^K O       : Split Window",
        "  ^K E       : Switch Window",
        "  ^K D       : Save & Quit",
        "",
        " [Search & Replace]",
        "  Ctrl-Q F   : Search Forward",
        "  Ctrl-Q A   : Replace",
        "  Ctrl-L     : Next Match",
        NULL
    };
    int line_idx = 0;

    /* 枠と背景の描画 */
    for (y = 0; y < box_h; y++) {
        for (x = 0; x < box_w; x++) {
            unsigned char attr = ATR_STATUS; /* 反転表示でポップアップ感 */
            char ch = ' ';
            if (y == 0 || y == box_h - 1) ch = '-';
            else if (x == 0 || x == box_w - 1) ch = '|';
            
            vz_putc(start_x + x, start_y + y, ch, attr);
        }
    }
    
    /* テキストの描画 */
    while (lines[line_idx] != NULL && line_idx < box_h - 2) {
        su_put_string(start_x + 2, start_y + 1 + line_idx, lines[line_idx], ATR_STATUS);
        line_idx++;
    }
}
