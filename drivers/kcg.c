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
/* #include "gfx.h" removed */
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

/* キャッシュ用のポインタ定義 (memmap.h: MEM_FONT_CACHE_BASE = 0x100000) */
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

/* End of kcg.c */
