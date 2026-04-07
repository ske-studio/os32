/*
 * screen_util.c - OS32 VZ Editor 画面描画ユーティリティ
 * C89 compatible
 */

#include "vz.h"

/* 塗りつぶし共通ヘルパー (KernelAPI一括呼び出し) */
void su_fill_rect(int start_x, int start_y, int w, int h, char ch, unsigned char attr)
{
    if (kapi) {
        kapi->lcons_fill_rect(start_x, start_y, w, h, ch, attr);
    }
}

void su_clear_line(int y, unsigned char attr)
{
    su_fill_rect(0, y, SCREEN_W, 1, ' ', attr);
}

/*
 * 数値を文字列に変換 (簡易sprintf代替)
 * 戻り値: 桁数
 */
int su_int_to_str(int val, char *buf)
{
    int dc = 0;
    int tmp = val;
    int i;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    do { dc++; tmp /= 10; } while (tmp > 0);
    tmp = val;
    for (i = 0; i < dc; i++) {
        buf[dc - 1 - i] = '0' + (tmp % 10);
        tmp /= 10;
    }
    buf[dc] = '\0';
    return dc;
}

/*
 * フォーマット付き数値描画
 * right_align=1 の場合は指定widthの右寄せ、0の場合は左寄せ(後ろをpad_charで埋める)
 * 戻り値: 描画した文字幅
 */
int su_put_int(int x, int y, int val, int width, int right_align, char pad_char, unsigned char attr)
{
    char num_buf[16];
    int digit_count = su_int_to_str(val, num_buf);
    int i;
    int cur_x = x;
    char out_buf[32];
    int bp = 0;
    
    if (right_align) {
        for (i = 0; i < width - digit_count && bp < 31; i++) out_buf[bp++] = pad_char;
    }
    for (i = 0; i < digit_count && bp < 31; i++) out_buf[bp++] = num_buf[i];
    if (!right_align) {
        for (i = 0; i < width - digit_count && bp < 31; i++) out_buf[bp++] = pad_char;
    }
    out_buf[bp] = '\0';
    
    su_put_string(x, y, out_buf, attr);
    return bp;
}

/*
 * 1文字分のUTF-8デコード・描画処理
 * 戻り値: 消費したX座標の増分 (1 or 2)
 */
int su_put_utf8_char(int x, int y, utf8_decode_t *dec, unsigned char attr)
{
    if (dec->codepoint >= 0x20 && dec->codepoint <= 0x7E) {
        vz_putc(x, y, (char)dec->codepoint, attr);
        return 1;
    } else {
        /* ANKチェック(比較演算のみ)をJISテーブル(131KB)より先に判定 */
        u8 ank = unicode_to_ank(dec->codepoint);
        if (ank != 0) {
            vz_putc(x, y, (char)ank, attr);
            return 1;
        } else {
            u16 jis = unicode_to_jis(dec->codepoint);
            if (jis != 0) {
                if (x + 1 < SCREEN_W) {
                    vz_putkanji(x, y, jis, attr);
                    return 2;
                } else {
                    vz_putc(x, y, ' ', attr);
                    return 1;
                }
            }
            vz_putc(x, y, '.', attr);
            return 1;
        }
    }
}

static unsigned short sjis_to_jis(unsigned char hi, unsigned char lo)
{
    unsigned short jhi, jlo;
    hi -= (hi <= 0x9F) ? 0x71 : 0xB1;
    hi = (hi << 1) + 1;
    if (lo > 0x7F) lo--;
    if (lo >= 0x9E) {
        lo -= 0x7D;
        hi++;
    } else {
        lo -= 0x1F;
    }
    jhi = hi + 0x20;
    jlo = lo + 0x20;
    return (jhi << 8) | jlo;
}

/*
 * 文字列表示ヘルパー
 */
int su_put_string(int x, int y, const char *s, unsigned char attr)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p && x < SCREEN_W) {
        if ((*p >= 0x81 && *p <= 0x9F) || (*p >= 0xE0 && *p <= 0xFC)) {
            if (*(p+1) != '\0') {
                u16 jis = sjis_to_jis(*p, *(p+1));
                if (x + 1 < SCREEN_W) {
                    vz_putkanji(x, y, jis, attr);
                    x += 2;
                } else {
                    vz_putc(x, y, ' ', attr);
                    x += 1;
                }
                p += 2;
            } else break;
        } else {
            vz_putc(x, y, (char)*p, attr);
            x++;
            p++;
        }
    }
    return x;
}

/*
 * UTF-8文字列描画 (戻り値は描画後のX座標)
 */
int su_put_utf8_string(int x, int y, const char *utf8_str, unsigned char attr)
{
    const unsigned char *p = (const unsigned char *)utf8_str;
    while (*p && x < SCREEN_W) {
        utf8_decode_t dec = utf8_decode(p);
        x += su_put_utf8_char(x, y, &dec, attr);
        p += dec.bytes_used;
    }
    return x;
}
