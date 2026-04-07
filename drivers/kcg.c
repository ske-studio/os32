/* ======================================================================== */
/*  KCG.C — 漢字キャラクタジェネレータ (KCG) ドライバ                       */
/*                                                                          */
/*  PC-98内蔵フォントROMから文字パターンを読み出してグラフィック描画          */
/*                                                                          */
/*  I/Oポートアクセス方式:                                                   */
/*    1. A1h ← JISコード下位バイト                                          */
/*    2. A3h ← JISコード上位バイト - 0x20                                   */
/*    3. A5h ← ライン番号 (0-15) + L/Rビット                                */
/*    4. A9h → パターンデータ (1バイト = 8ピクセル分)                         */
/*                                                                          */
/*  ANK文字:                                                                 */
/*    PC-98ではANK文字はJIS行0x29に配置 (半角漢字扱い)                       */
/*    例: ASCII 'A' (0x41) → JISコード 0x2941                               */
/*    ポート A3h ← 0x29-0x20 = 0x09                                         */
/*    出典: PC9800Bible §2-6-1 「半角漢字の場合」                            */
/*                                                                          */
/*  漢字: JIS上位=0x21-0x7E, 16x16ドット (左右各8ドット)                    */
/*                                                                          */
/*  Shift-JIS → JIS変換も内蔵                                               */
/*                                                                          */
/*  出典: PC9800Bible §2-6-5                                                */
/* ======================================================================== */

#include "kcg.h"
#include "gfx.h"
#include "io.h"
#include "utf8.h"
#include "pc98.h"

/* スケール係数 (デフォルト=1, 最大4) */
int kcg_scale = 1;

/* ======== I/Oウェイト ======== */
static void kcg_wait(void)
{
    io_wait();
}

/* ======================================================================== */
/*  KCG初期化                                                               */
/* ======================================================================== */

/* キャッシュ用のポインタ定義 (memmap.h: MEM_FONT_CACHE_BASE = 0x200000) */
#include "memmap.h"
#define KANJI_CACHE_SIZE   (32 * 94 * 94)   /* 282,752 Bytes */
#define KANJI_FETCHED_SIZE (94 * 94)        /*   8,836 Bytes */
#define ANK_CACHE_SIZE     (16 * 256)       /*   4,096 Bytes */

static u8 *kanji_cache   = (u8 *)(MEM_FONT_CACHE_BASE);
static u8 *kanji_fetched = (u8 *)(MEM_FONT_CACHE_BASE + KANJI_CACHE_SIZE);
static u8 *ank_cache     = (u8 *)(MEM_FONT_CACHE_BASE + KANJI_CACHE_SIZE + KANJI_FETCHED_SIZE);
static u8 *ank_fetched   = (u8 *)(MEM_FONT_CACHE_BASE + KANJI_CACHE_SIZE + KANJI_FETCHED_SIZE + ANK_CACHE_SIZE);

void kcg_init(void)
{
    int i;
    /* コードアクセスモードに設定 (モードFF1 = KCGコードアクセス) */
    outp(MODE_FF1_PORT, MFF1_KCG_CODE);
    kcg_wait();
    kcg_scale = 1;

    /* キャッシュフラグの初期化 (ゼロクリア) */
    for (i = 0; i < KANJI_FETCHED_SIZE; i++) kanji_fetched[i] = 0;
    for (i = 0; i < 256; i++) ank_fetched[i] = 0;
}

/* ======================================================================== */
/*  スケール設定                                                             */
/* ======================================================================== */
void kcg_set_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    kcg_scale = scale;
}

/* ======================================================================== */
/*  ANKパターン読み出し (8x16, 16バイト)                                    */
/*  ch: ASCII/ANKコード (0x00-0xFF)                                         */
/*                                                                          */
/*  PC-98のANK文字はJIS行0x29に格納 (半角漢字コード)                        */
/*  ASCII 0x20-0x7F → JIS 0x2920-0x297F                                    */
/*  半角カナ 0xA1-0xDF → JIS 0x2A21-0x2A5F (推定)                          */
/* ======================================================================== */
void kcg_read_ank(u8 ch, u8 *buf)
{
    int line;
    u8 *cache_ptr;

    if (ank_fetched[ch]) {
        cache_ptr = ank_cache + (ch * 16);
        for (line = 0; line < 16; line++) buf[line] = cache_ptr[line];
        return;
    }

    /*
     * NP2kaiソースコード (cgrom.c) より判明:
     *   oa1: cr->code = (dat << 8) | (cr->code & 0xff)  → A1hは上位バイト
     *   oa3: cr->code = (cr->code & 0xff00) | dat        → A3hは下位バイト
     *   ANK条件: !(cr->code & 0xff00) → A1h = 0x00
     *   フォント: fontrom + 0x80000 + (cr->code << 4)
     *   ライン: ptr[cr->line]  (line = dat & 0x1f)
     */
    outp(KCG_CODE_LO, 0x00);        /* A1h ← 0x00 (上位=0でANK) */
    kcg_wait();
    outp(KCG_CODE_HI, ch);          /* A3h ← ANK文字コード (下位) */
    kcg_wait();

    cache_ptr = ank_cache + (ch * 16);
    for (line = 0; line < 16; line++) {
        outp(KCG_LINE_SEL, (u8)line);  /* A5h ← ライン (bit4=0でANK) */
        kcg_wait();
        buf[line] = (u8)inp(KCG_DATA); /* A9h → パターン */
        cache_ptr[line] = buf[line];
    }
    ank_fetched[ch] = 1;
}

/* ======================================================================== */
/*  漢字パターン読み出し (16x16, 32バイト)                                  */
/*  jis_code: JISコード (例: 0x2422 = 'あ')                                */
/*  buf: 32バイト出力 [line*2+0]=左8dot, [line*2+1]=右8dot                  */
/* ======================================================================== */
void kcg_read_kanji(u16 jis_code, u8 *buf)
{
    int line;
    u8 hi = (u8)(jis_code >> 8);
    u8 lo = (u8)(jis_code & 0xFF);
    int idx;
    u8 *cache_ptr;

    /* JISコードのバリデーション (0x2121 - 0x7E7E) */
    if (hi >= 0x21 && hi <= 0x7E && lo >= 0x21 && lo <= 0x7E) {
        idx = (hi - 0x21) * 94 + (lo - 0x21);
        if (kanji_fetched[idx]) {
            cache_ptr = kanji_cache + (idx * 32);
            for (line = 0; line < 32; line++) buf[line] = cache_ptr[line];
            return;
        }
    } else {
        idx = -1; /* invalid jis code range for cache */
    }

    /* 文字コード設定 */
    outp(KCG_CODE_LO, lo);          /* A1h ← JIS下位 */
    kcg_wait();
    outp(KCG_CODE_HI, hi - 0x20);   /* A3h ← JIS上位 - 0x20 */
    kcg_wait();

    /* 左半分 (8ドット×16ライン) — bit5=1 */
    for (line = 0; line < 16; line++) {
        outp(KCG_LINE_SEL, (u8)(line | 0x20));  /* bit5=1: 左 */
        kcg_wait();
        buf[line * 2] = (u8)inp(KCG_DATA);
    }

    /* 右半分 (8ドット×16ライン) — bit5=0 */
    for (line = 0; line < 16; line++) {
        outp(KCG_LINE_SEL, (u8)(line | 0x00));  /* bit5=0: 右 */
        kcg_wait();
        buf[line * 2 + 1] = (u8)inp(KCG_DATA);
    }

    /* キャッシュへの書き込み */
    if (idx >= 0) {
        cache_ptr = kanji_cache + (idx * 32);
        for (line = 0; line < 32; line++) cache_ptr[line] = buf[line];
        kanji_fetched[idx] = 1;
    }
}

/* ======================================================================== */
/*  スケール付きピクセル描画 (scale×scaleの正方形を描画)                    */
/* ======================================================================== */
static void draw_scaled_pixel(int x, int y, u8 color)
{
    int sx, sy;
    if (kcg_scale == 1) {
        gfx_pixel(x, y, color);
        return;
    }
    for (sy = 0; sy < kcg_scale; sy++) {
        for (sx = 0; sx < kcg_scale; sx++) {
            gfx_pixel(x + sx, y + sy, color);
        }
    }
}

/* ======================================================================== */
/*  ANK文字描画 (バックバッファ上, 8x16 × scale)                           */
/* ======================================================================== */
void kcg_draw_ank(int x, int y, u8 ch, u8 fg, u8 bg)
{
    u8 pat[16];
    int row, col;

    if (bg != 0xFF) {
        gfx_fill_rect(x, y, 8 * kcg_scale, 16 * kcg_scale, bg);
    }

    kcg_read_ank(ch, pat);

    if (kcg_scale == 1) {
        gfx_draw_font(x, y, pat, 1, 16, fg);
    } else {
        for (row = 0; row < 16; row++) {
            u8 bits = pat[row];
            for (col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    draw_scaled_pixel(x + col * kcg_scale,
                                      y + row * kcg_scale, fg);
                }
            }
        }
    }
}

/* ======================================================================== */
/*  漢字描画 (バックバッファ上, 16x16 × scale)                             */
/* ======================================================================== */
void kcg_draw_kanji(int x, int y, u16 jis_code, u8 fg, u8 bg)
{
    u8 pat[32];
    int row, col;

    if (bg != 0xFF) {
        gfx_fill_rect(x, y, 16 * kcg_scale, 16 * kcg_scale, bg);
    }

    kcg_read_kanji(jis_code, pat);

    if (kcg_scale == 1) {
        gfx_draw_font(x, y, pat, 2, 16, fg);
    } else {
        for (row = 0; row < 16; row++) {
            u8 left  = pat[row * 2];
            u8 right = pat[row * 2 + 1];

            /* 左8ドット */
            for (col = 0; col < 8; col++) {
                if (left & (0x80 >> col)) {
                    draw_scaled_pixel(x + col * kcg_scale,
                                      y + row * kcg_scale, fg);
                }
            }
            /* 右8ドット */
            for (col = 0; col < 8; col++) {
                if (right & (0x80 >> col)) {
                    draw_scaled_pixel(x + (8 + col) * kcg_scale,
                                      y + row * kcg_scale, fg);
                }
            }
        }
    }
}

/* ======================================================================== */
/*  Shift-JIS → JIS変換                                                     */
/* ======================================================================== */
static u16 sjis_to_jis(u8 hi, u8 lo)
{
    u16 jh, jl;

    /* Shift-JIS → JIS変換アルゴリズム */
    if (hi <= 0x9F) {
        jh = (u16)(hi - 0x71) * 2 + 1;
    } else {
        jh = (u16)(hi - 0xB1) * 2 + 1;
    }

    if (lo >= 0x80) lo--;

    if (lo >= 0x9E) {
        jh++;
        jl = (u16)(lo - 0x7D);
    } else {
        jl = (u16)(lo - 0x1F);
    }

    return (u16)((jh + 0x20) << 8) | (jl + 0x20);
}

/* ======================================================================== */
/*  Shift-JIS文字列描画 (スケール対応)                                      */
/*  ANK(半角)と漢字(全角)を自動判別して描画                                  */
/* ======================================================================== */
int kcg_draw_sjis(int x, int y, const char *sjis_str, u8 fg, u8 bg)
{
    int cx = x;
    const u8 *p = (const u8 *)sjis_str;
    int ank_w = 8 * kcg_scale;
    int kanji_w = 16 * kcg_scale;
    int line_h = 18 * kcg_scale;

    /* KCGコードアクセスモードを有効化 */
    outp(MODE_FF1_PORT, MFF1_KCG_CODE);


    while (*p) {
        int line_width = 0;
        const u8 *lp = p;
        
        /* 1行分の描画幅を計算 */
        while (*lp && *lp != '\n') {
            u8 c1 = *lp;
            if ((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC)) {
                if (*(lp + 1) != 0) { line_width += kanji_w; lp += 2; }
                else break;
            } else {
                line_width += ank_w; lp++;
            }
        }
        
        /* 1コマンドで背景を一括ベタ塗り (最高速) */
        if (bg != 0xFF && line_width > 0) {
            gfx_fill_rect(cx, y, line_width, 16 * kcg_scale, bg);
        }
        
        /* 個々の文字描画では背景ベタ塗りを行わない (bg = 0xFF) */
        while (p < lp) {
            u8 c1 = *p;
            if ((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC)) {
                u8 c2 = *(p + 1);
                kcg_draw_kanji(cx, y, sjis_to_jis(c1, c2), fg, 0xFF);
                cx += kanji_w;
                p += 2;
            } else {
                kcg_draw_ank(cx, y, c1, fg, 0xFF);
                cx += ank_w;
                p++;
            }
        }
        
        /* 改行処理 */
        if (*p == '\n') {
            cx = x;
            y += line_h;
            p++;
        }
    }

    /* テキストVRAMへのアクセスモード復旧 */
    outp(MODE_FF1_PORT, MFF1_HIRES);

    return cx - x;
}

/* ======================================================================== */
/*  UTF-8文字列描画 (スケール対応)                                          */
/*  lib/utf8.c の変換関数 (utf8_decode, unicode_to_ank, unicode_to_jis)     */
/*  を使用してUTF-8文字列をKCGで描画する。                                  */
/*  戻り値: 描画した水平ピクセル数                                           */
/* ======================================================================== */
int kcg_draw_utf8(int x, int y, const char *utf8_str, u8 fg, u8 bg)
{
    int cx = x;
    const u8 *p = (const u8 *)utf8_str;
    int scale = kcg_scale;
    int ank_w = 8 * scale;
    int kanji_w = 16 * scale;
    int line_h = 18 * scale;

    /* KCGコードアクセスモードを有効化 */
    outp(MODE_FF1_PORT, MFF1_KCG_CODE);


    while (*p) {
        utf8_decode_t dec;
        u32 cp;
        u8 ank;
        u16 jis;

        if (*p == '\n') {
            cx = x;
            y += line_h;
            p++;
            continue;
        }

        dec = utf8_decode(p);
        cp = dec.codepoint;
        p += dec.bytes_used;

        if (cp == 0xFEFF || cp < 0x20) {
            if (cp == '\t') cx += ank_w * 4;
            continue;
        }

        /* ANKチェック(比較演算のみ)を先に判定 */
        ank = unicode_to_ank(cp);
        if (ank) {
            kcg_draw_ank(cx, y, ank, fg, bg);
            cx += ank_w;
        } else {
            jis = unicode_to_jis(cp);
            if (!jis) jis = 0x2222;
            kcg_draw_kanji(cx, y, jis, fg, bg);
            cx += kanji_w;
        }
    }

    /* KCG へのアクセスが終わったら、テキストVRAMの表示やアクセスが正常に行えるようモードを戻す */
    outp(MODE_FF1_PORT, MFF1_HIRES);

    return cx - x;
}
