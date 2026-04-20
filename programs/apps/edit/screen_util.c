/*
 * screen_util.c - OS32 VZ Editor 画面描画ユーティリティ
 *
 * libos32gfx lconsole API への薄いラッパー。
 * VZ固有のロジックを含まない関数は、すべて lcons_* に委譲する。
 * C89 compatible
 */

#include "vz.h"

/* 塗りつぶし共通ヘルパー */
void su_fill_rect(int start_x, int start_y, int w, int h, char ch, unsigned char attr)
{
    if (kapi) {
        lcons_fill_rect(start_x, start_y, w, h, ch, attr);
    }
}

void su_clear_line(int y, unsigned char attr)
{
    if (kapi) {
        lcons_clear_line(y, attr);
    }
}

/*
 * 数値を文字列に変換 — lconsole委譲
 */
int su_int_to_str(int val, char *buf)
{
    return lcons_int_to_str(val, buf);
}

/*
 * フォーマット付き数値描画 — lconsole委譲
 */
int su_put_int(int x, int y, int val, int width, int right_align, char pad_char, unsigned char attr)
{
    return lcons_put_int(x, y, val, width, right_align, pad_char, attr);
}

/*
 * 1文字分のUTF-8デコード・描画処理 — lconsole委譲
 */
int su_put_utf8_char(int x, int y, utf8_decode_t *dec, unsigned char attr)
{
    return lcons_put_utf8_char(x, y, (void *)dec, attr);
}

/*
 * 文字列表示ヘルパー — lconsole委譲
 */
int su_put_string(int x, int y, const char *s, unsigned char attr)
{
    return lcons_put_string(x, y, s, attr);
}

/*
 * UTF-8文字列描画 — lconsole委譲
 */
int su_put_utf8_string(int x, int y, const char *utf8_str, unsigned char attr)
{
    return lcons_put_utf8_string(x, y, utf8_str, attr);
}
