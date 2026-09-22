/* ======================================================================== */
/*  KSELFTEST.C — カーネル内プリミティブの自己診断                          */
/*                                                                          */
/*  なぜカーネル内でやるのか:                                                */
/*    programs/tests/klibc_test.c は **newlib とリンクされる外部プログラム** */
/*    なので、そこで通る strlen/memcpy/malloc は newlib の実装であって       */
/*    カーネルの kstring_asm.asm / kmalloc.c ではない。つまりカーネルが      */
/*    実際に使うプリミティブはこれまで一度もテストされていなかった。         */
/*                                                                          */
/*  ここでは「境界ケースだけ」を見る。通常ケースはシェルが動いている時点で   */
/*  通っているので、n=0 / バッファぴったり / 重なりコピー / 二重解放 など、   */
/*  実際にバグが潜んでいた形だけを実機で毎回踏んで確認する。                 */
/*                                                                          */
/*  ブート時に 1 回走り、全部通れば 1 行だけ出す。落ちた項目は赤で名前を     */
/*  出す (実機で回帰した瞬間に画面で分かる)。                               */
/* ======================================================================== */

#include "kselftest.h"
#include "kstring.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "paging.h"
#include "con_sink.h"
#include "pc98.h"
#include "tvram.h"
#include "console.h"
#include "kbd_inject.h"
#include "appslot.h"
#include "launch.h"
#include "exec.h"
#include "kapi_db.h"
#include "cpu_calibrate.h"
#include "cpu_calibrate_math.h"
#include "idt.h"          /* pit_get_setup / PIT_MODE_TIMER0 */
#include "sysclk.h"       /* sysclk_hz / sysclk_detected */
#include "memmap.h"       /* PIT_HZ / MEM_DMA_POOL_* */
#include "dma_pool.h"     /* DMA プール (票 TASK_HAL_WIRING §1-3) */
#include "dma8237.h"      /* 8237 の共通部 (同 §1-2) */

/* 結果はホストから読めるようにグローバルにする。
 * ブート時の出力はスプラッシュで流れてしまい、rshell も未起動なので
 * シリアルにも出ない。kernel.map 経由で emu_read_mem するのが確実。
 * (v86.c の観測値と同じ理由で static にしない) */
int kselftest_pass = 0;
int kselftest_fail = 0;
#define ksel_pass kselftest_pass
#define ksel_fail kselftest_fail

static void check(int cond, const char *name)
{
    if (cond) {
        ksel_pass++;
    } else {
        ksel_fail++;
        kprintf(0xC1, "[selftest] FAIL: %s\n", name);
    }
}

/* ------------------------------------------------------------------------ */
/*  kstring_asm.asm (kmemcpy/kmemset/kstrlen/kstrcmp/kstrncpy)              */
/*                                                                          */
/*  ASM 実装は 4 バイト単位の高速パスと 1 バイトずつの端数パスを持つので、  */
/*  n が 0 / 端数 / 境界ちょうど のときに崩れやすい。番兵を置いて           */
/*  「書きすぎていないこと」まで見る。                                       */
/* ------------------------------------------------------------------------ */
static void test_mem(void)
{
    static u8 buf[32];
    static u8 src[32];
    u32 i;
    int ok;

    for (i = 0; i < 32; i++) src[i] = (u8)(i + 1);

    /* kmemset: n=0 は 1 バイトも書いてはいけない */
    buf[0] = 0xAA;
    kmemset(buf, 0x55, 0);
    check(buf[0] == 0xAA, "kmemset n=0");

    /* kmemset: 端数長 (高速パスと端数パスの境目) */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemset(buf, 0x55, 5);
    ok = (buf[0] == 0x55 && buf[4] == 0x55 && buf[5] == 0xAA);
    check(ok, "kmemset n=5 no overrun");

    /* kmemcpy: n=0 */
    buf[0] = 0xAA;
    kmemcpy(buf, src, 0);
    check(buf[0] == 0xAA, "kmemcpy n=0");

    /* kmemcpy: 端数長 + 番兵 */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemcpy(buf, src, 7);
    ok = (buf[0] == 1 && buf[6] == 7 && buf[7] == 0xAA);
    check(ok, "kmemcpy n=7 no overrun");

    /* kmemcpy: 非アラインの dst/src */
    for (i = 0; i < 32; i++) buf[i] = 0xAA;
    kmemcpy(buf + 1, src + 1, 9);
    ok = (buf[0] == 0xAA && buf[1] == 2 && buf[9] == 10 && buf[10] == 0xAA);
    check(ok, "kmemcpy unaligned");

    /* memmove: 前方に重なるコピー (kmemcpy では壊れる形) */
    for (i = 0; i < 32; i++) buf[i] = (u8)(i + 1);
    memmove(buf, buf + 2, 8);
    ok = (buf[0] == 3 && buf[7] == 10);
    check(ok, "memmove overlap forward");

    /* memmove: 後方に重なるコピー */
    for (i = 0; i < 32; i++) buf[i] = (u8)(i + 1);
    memmove(buf + 2, buf, 8);
    ok = (buf[2] == 1 && buf[9] == 8);
    check(ok, "memmove overlap backward");

    /* memmove: n=0 と同一ポインタ */
    buf[0] = 0x42;
    memmove(buf, buf, 8);
    memmove(buf, buf + 1, 0);
    check(buf[0] == 0x42, "memmove n=0 / same ptr");
}

static void test_str(void)
{
    static char dst[16];
    int ok;

    check(kstrlen("") == 0, "kstrlen empty");
    check(kstrlen("abcd") == 4, "kstrlen 4");

    check(kstrcmp("", "") == 0, "kstrcmp empty");
    check(kstrcmp("abc", "abd") < 0, "kstrcmp lt");
    check(kstrcmp("abd", "abc") > 0, "kstrcmp gt");
    /* 0x80 以上のバイトを含む比較。ASM 側が符号付きで比べていると
     * 大小が逆転する (CP932 の 2 バイト目が該当する) */
    check(kstrcmp("\x80", "\x01") > 0, "kstrcmp high byte unsigned");

    check(kstrncmp("abc", "abd", 0) == 0, "kstrncmp n=0");
    check(kstrncmp("abc", "abd", 2) == 0, "kstrncmp n=2");
    check(kstrncmp("abc", "abd", 3) != 0, "kstrncmp n=3");

    /* kstrncpy は strlcpy セマンティクス: n はバッファ全体サイズ、
     * 必ず NUL 終端する */
    kmemset(dst, 0x7F, sizeof(dst));
    kstrncpy(dst, "abcdefgh", 4);
    ok = (dst[0] == 'a' && dst[2] == 'c' && dst[3] == '\0');
    check(ok, "kstrncpy truncates + NUL");

    /* kstrncat: n=0 は 1 バイトも触らない (n-1 のラップで暴走した形) */
    dst[0] = 'X'; dst[1] = '\0';
    kstrncat(dst, "yyyy", 0);
    check(dst[0] == 'X' && dst[1] == '\0', "kstrncat n=0");

    /* kstrncat: バッファぴったりで切り詰め + NUL */
    kstrcpy(dst, "abc");
    kstrncat(dst, "defgh", 6);
    ok = (kstrcmp(dst, "abcde") == 0);
    check(ok, "kstrncat exact fit");

    /* kstrncat: 既に満杯なら何もしない */
    kstrcpy(dst, "abcde");
    kstrncat(dst, "zzz", 6);
    check(kstrcmp(dst, "abcde") == 0, "kstrncat already full");

    /* strchr は NUL 自身も見つける契約 */
    check(strchr("abc", '\0') != (char *)0, "strchr finds NUL");
    check(strchr("abc", 'z') == (char *)0, "strchr not found");
}

static void test_utoa(void)
{
    char buf[16];

    check(kutoa_dec(0, buf, 12) == 1 && kstrcmp(buf, "0") == 0,
          "kutoa_dec 0");
    check(kutoa_dec(4294967295UL, buf, 12) == 10 &&
          kstrcmp(buf, "4294967295") == 0, "kutoa_dec u32 max");
    check(kutoa_hex(0, buf, 12, 0) == 1 && kstrcmp(buf, "0") == 0,
          "kutoa_hex 0");
    check(kutoa_hex(0xDEADBEEFUL, buf, 12, 1) == 8 &&
          kstrcmp(buf, "DEADBEEF") == 0, "kutoa_hex upper");
    check(kutoa_hex(0xDEADBEEFUL, buf, 12, 0) == 8 &&
          kstrcmp(buf, "deadbeef") == 0, "kutoa_hex lower");
}

/* ------------------------------------------------------------------------ */
/*  アロケータ                                                              */
/*                                                                          */
/*  カーネルヒープを壊さずに実装そのものを試すため、専用の KHeap             */
/*  インスタンスを静的バッファ上に作る (kheap_* をパラメータ化してある       */
/*  おかげでこれができる)。                                                  */
/* ------------------------------------------------------------------------ */
static u8 ksel_heap_buf[2048];

static void test_heap(void)
{
    KHeap h;
    void *a, *b, *c;
    u32 free_all;
    int i;

    kheap_init(&h, ksel_heap_buf, sizeof(ksel_heap_buf), "selftest");

    a = kheap_alloc(&h, 16);
    check(a != (void *)0, "kheap_alloc basic");
    /* SQLite が double を置くので 8 バイト境界が要る */
    check((((u32)a) & 7) == 0, "kheap_alloc 8-byte aligned");

    b = kheap_alloc(&h, 1);
    check(b != (void *)0 && (((u32)b) & 7) == 0, "kheap_alloc(1) aligned");
    check(kheap_block_size(&h, a) >= 16, "kheap_block_size");

    /* 0 バイト要求と、ヒープ全体を超える要求は NULL */
    check(kheap_alloc(&h, 0) == (void *)0, "kheap_alloc(0) = NULL");
    check(kheap_alloc(&h, sizeof(ksel_heap_buf) * 2) == (void *)0,
          "kheap_alloc too big = NULL");
    /* アライン時に 0 へ化ける値 (整数オーバーフロー) も弾けること */
    check(kheap_alloc(&h, 0xFFFFFFFFUL) == (void *)0,
          "kheap_alloc overflow = NULL");

    /* 解放 → 再確保でブロックが再利用される */
    kheap_free(&h, a);
    kheap_free(&h, b);
    c = kheap_alloc(&h, 16);
    check(c == a, "kheap reuse after free");
    kheap_free(&h, c);

    /* 全域が 1 個のフリーブロックに戻っている (前後の結合が効いている)。
     * 結合漏れがあると、この後の「ほぼ全域」確保が失敗する。 */
    free_all = sizeof(ksel_heap_buf) - 64;
    c = kheap_alloc(&h, free_all);
    check(c != (void *)0, "kheap coalesce to one block");
    if (c) kheap_free(&h, c);

    /* 断片化 → 全解放で必ず 1 個に戻る (結合の取りこぼし検出) */
    {
        void *p[16];
        for (i = 0; i < 16; i++) p[i] = kheap_alloc(&h, 32);
        /* 飛び飛びに解放してから残りを解放 */
        for (i = 0; i < 16; i += 2) kheap_free(&h, p[i]);
        for (i = 1; i < 16; i += 2) kheap_free(&h, p[i]);
        c = kheap_alloc(&h, free_all);
        check(c != (void *)0, "kheap coalesce after fragmentation");
        if (c) kheap_free(&h, c);
    }

    /* 不正な解放を検出して弾くこと (弾かないとヒープ管理が壊れる)。
     * 診断が 1 行出るのは想定内。 */
    a = kheap_alloc(&h, 16);
    kheap_free(&h, a);
    kheap_free(&h, a);                      /* 二重解放 */
    kheap_free(&h, (void *)0x1);            /* 範囲外 */
    kheap_free(&h, (u8 *)a + 1);            /* 非アライン */
    c = kheap_alloc(&h, free_all);
    check(c != (void *)0, "kheap survives invalid frees");
    if (c) kheap_free(&h, c);

    /* 極小サイズのヒープ: ヘッダも入らないなら空ヒープとして扱う
     * (size - BLK_HDR_SIZE のアンダーフローで巨大ブロックに化けた形) */
    {
        KHeap tiny;
        kheap_init(&tiny, ksel_heap_buf, 4, "tiny");
        check(kheap_alloc(&tiny, 1) == (void *)0, "kheap tiny = empty");
    }
}

/* ------------------------------------------------------------------------ */
/*  kprintf                                                                 */
/*                                                                          */
/*  出力内容は取れないので「戻ってくること」を確かめる。書式末尾が '%' の    */
/*  ときに無限ループしていた (KAPI 公開関数なので、ユーザプログラムの        */
/*  1 行でカーネルが固まった)。ここで固まればブートが止まるので分かる。      */
/* ------------------------------------------------------------------------ */
static void test_kprintf(void)
{
    kprintf(0x07, "");
    kprintf(0x07, "%");             /* 末尾 % — 旧実装はここで無限ループ */
    kprintf(0x07, "%%");
    kprintf(0x07, "%z", 1);         /* 未知指定子 (vararg を消費する) */
    kprintf(0x07, "\n");
    check(1, "kprintf returns (no hang)");
}

/* ------------------------------------------------------------------------ */
/*  kprintf の属性変換 (lib/kprintf_attr.c)                                 */
/*                                                                          */
/*  PC-98 のテキスト属性は PC/AT (CGA) とビットの意味が別物で、             */
/*  呼び出し側の大半が渡している 07h をそのまま属性 VRAM へ書くと           */
/*  **画面に 1 文字も出ない** (bit0 = 表示 / bit1 = ブリンク /              */
/*  bit2 = リバース / bit5,6,7 = 青,赤,緑)。実機 PC-9821Ra266 の FD 起動が  */
/*  失敗したとき、[fdc] / [ide] の診断行が 1 行も読めなかったのがこれ       */
/*  (2026-09-22)。kprintf は KAPI 公開関数でもあるので、ここが壊れると      */
/*  カーネルもアプリも同時に黙る — プリミティブとして毎回踏む。            */
/*  ホスト側の試験は tools/tests/test_kprintf_attr.py。                     */
/* ------------------------------------------------------------------------ */
static void test_kprintf_attr(void)
{
    /* PC/AT 流の値 → PC-98 の色 + 表示。反転も点滅も付けない。 */
    check(kprintf_attr_to_pc98(0x07) == 0xE1, "kprintf attr 07 -> E1 (white)");
    check(kprintf_attr_to_pc98(0x0A) == 0x81, "kprintf attr 0A -> 81 (green)");
    check(kprintf_attr_to_pc98(0x0C) == 0x41, "kprintf attr 0C -> 41 (red)");
    check(kprintf_attr_to_pc98(0x0E) == 0xC1, "kprintf attr 0E -> C1 (yellow)");
    check(kprintf_attr_to_pc98(0x02) == 0x81, "kprintf attr 02 -> 81 (green)");
    check(kprintf_attr_to_pc98(0x04) == 0x41, "kprintf attr 04 -> 41 (red)");
    check(kprintf_attr_to_pc98(0x0B) == 0xA1, "kprintf attr 0B -> A1 (cyan)");
    /* 色を持たない値は黒 = 不可視になるので白へ倒す。 */
    check(kprintf_attr_to_pc98(0x00) == 0xE1, "kprintf attr 00 -> E1 (black->white)");
    /* 既に PC-98 流の値はそのまま。bit0 が落ちていれば立てるだけ。 */
    check(kprintf_attr_to_pc98(0xC1) == 0xC1, "kprintf attr C1 kept");
    check(kprintf_attr_to_pc98(0xE0) == 0xE1, "kprintf attr E0 -> E1 (visible)");
}

/* ======================================================================== */
/*  公開エントリ                                                            */
/* ======================================================================== */
/* ------------------------------------------------------------------------ */
/*  リング3 PD 複製 (v2 M1b): 新 PD を作って CR3 に載せてもカーネル帯域が    */
/*  同一物理で共有され続けるか (V1)。ここが壊れると CPL=3 化以降が全滅する    */
/*  ので、ブート時に毎回検証する (CLAUDE.md: プリミティブは selftest に載せる)。*/
/* ------------------------------------------------------------------------ */
static void test_ring3_pd(void)
{
    int rc = paging_pd_clone_selftest();
    check(rc == 0, "ring3 PD clone (kernel band shared across PDs)");
    if (rc != 0) {
        kprintf(0xC1, "[selftest]   paging_pd_clone_selftest rc=%d\n", rc);
    }
}

/* ------------------------------------------------------------------------ */
/*  デバイス窓の貸し出し (レビュー #5 ②③): 表示面を CPL=3 に見せないまま     */
/*  クライアント面だけを USER にでき、そのときキャッシュ属性 (PCD) が        */
/*  消えないこと。ここが壊れると Cirrus で「commit 前の描画が表示面に出る」   */
/*  (契約 G4 違反) か「BLT が古い VRAM を読んでちらつく」になるが、どちらも   */
/*  画面を見るまで分からないので毎回ブート時に見る。                          */
/* ------------------------------------------------------------------------ */
static void test_map_user_keep(void)
{
    int rc = paging_map_user_keep_selftest();
    check(rc == 0, "device window lease (USER only on client page, PCD kept)");
    if (rc != 0) {
        kprintf(0xC1, "[selftest]   paging_map_user_keep_selftest rc=%d\n", rc);
    }
}

/* ------------------------------------------------------------------------ */
/*  アプリ帯の可変 PDE 化 (票 docs/tasks/memory/APP_BAND_PDE.md): 帯を 4MB    */
/*  単位で伸ばしたとき、増えた PDE がアプリ固有 PT に差し替わり、USER が      */
/*  master の PDE/PT へ漏れないこと。ここが壊れると CPL=3 アプリが master の  */
/*  ページテーブルを書き換えられる (= 任意物理への読み書き) が、動いている    */
/*  ように見えてしまうので毎回ブート時に見る。                                */
/* ------------------------------------------------------------------------ */
static void test_app_band_pde(void)
{
    int rc = paging_app_band_selftest();
    check(rc == 0, "app band PDEs (private PTs, USER never reaches master)");
    if (rc != 0) {
        kprintf(0xC1, "[selftest]   paging_app_band_selftest rc=%d\n", rc);
    }
}

/* ------------------------------------------------------------------------ */
/*  console シンクのリング (票 K6C の受入 C2): GUI モード中のカーネル出力を  */
/*  溜める 8KB の環。レコード境界で切ること・あふれで **古い方**を捨てる     */
/*  こと・CUI 復帰で捨てること・読み手が 1 本であることが崩れると、端末      */
/*  アプリには「出力が出ない」か「途中で化ける」としか見えず原因が遠い。      */
/*  ホスト試験 (tools/tests/test_con_sink.py) と同じ形をブート時にも踏む。    */
/* ------------------------------------------------------------------------ */
static void test_con_sink(void)
{
    u32 bad = con_sink_selftest();
    check((bad & (1u << 0)) == 0, "con_sink push/read (record round-trip)");
    check((bad & (1u << 1)) == 0, "con_sink CLEAR / CURSOR records");
    check((bad & (1u << 2)) == 0, "con_sink read cuts on a record boundary");
    check((bad & (1u << 3)) == 0, "con_sink overflow drops the oldest record");
    check((bad & (1u << 4)) == 0, "con_sink discards on return to CUI");
    check((bad & (1u << 5)) == 0, "con_sink single reader (owner reclaim)");
    check((bad & (1u << 6)) == 0, "con_sink EXIT record (child exit, T7 E1)");
}

/* ------------------------------------------------------------------------ */
/*  GUI モード中の描画抑止 (票 K6C-2)                                        */
/*                                                                          */
/*  シンクが有効なあいだ console.c が従来どおりテキスト VRAM にも描いていた  */
/*  ので、gshell の GFX 画面の上に CUI プログラムの出力が残像として重なって  */
/*  いた (PM 実測 2026-09-12、K7 受入 I2)。実機で見えるのは「左上に古い文字」 */
/*  だけで、シンク側は正常に見えるため原因が遠い。ここで毎回踏む。           */
/*  con_sink_enable/disable を直に使う (console_text_gdc_start はブート画面を */
/*  消してしまう)。 */
/* ------------------------------------------------------------------------ */
static void test_con_sink_render_gate(void)
{
    int sx = console_get_cursor_x();
    int sy = console_get_cursor_y();
    int x  = TVRAM_COLS - 1;
    int y  = TVRAM_ROWS - 1;
    u16 before = 0;
    u16 after = 0;
    u8  attr = 0;

    tvram_readchar_at(x, y, &before, &attr);
    console_set_cursor(x, y);
    con_sink_enable();
    shell_print("Z", TATTR_WHITE);
    tvram_readchar_at(x, y, &after, &attr);
    check(after == before, "console: GUI mode does not draw to text VRAM");
    check(console_get_cursor_x() == x && console_get_cursor_y() == y,
          "console: GUI mode does not advance the logical cursor");
    con_sink_disable();
    console_set_cursor(sx, sy);
}

/* ------------------------------------------------------------------------ */
/*  打鍵の注入リングと「印の無い resume は拒否」(票 K7 の受入 I5)            */
/*                                                                          */
/*  GUI 中の kbd_getchar は第 2 の park 点になった。壊れたときに実機で見える */
/*  のは「端末に打っても文字が出ない」か「GUI ごと固まる」だけで、原因が     */
/*  遠い。ブート時に踏むのは 2 つ:                                           */
/*    (1) 256B の環 — 積んだ順に 1 バイトずつ出る (UTF-8 の並びを変えない)、 */
/*        あふれは新しい方を捨てる、破棄で空、読み手未確立の注入は拒否。     */
/*    (2) 印の無いフレームは起こせない (C6 の規則が PARKED / WAIT_KEY /      */
/*        WAIT_POLL の 3 つに効く)。resume の切替点が緩むとフレームが宙に    */
/*        浮く。D8 の tick の間引きが表の検査より先に効くことも見る。        */
/* ------------------------------------------------------------------------ */
static void test_kbd_inject(void)
{
    u32 bad = kbd_inject_selftest();
    check((bad & (1u << 0)) == 0, "kbd_inject refuses with no con_sink reader");
    check((bad & (1u << 1)) == 0, "kbd_inject keeps UTF-8 byte order (FIFO)");
    check((bad & (1u << 2)) == 0, "kbd_inject take on empty ring returns 0");
    check((bad & (1u << 3)) == 0, "kbd_inject overflow drops the newest byte");
    check((bad & (1u << 4)) == 0, "kbd_inject discard empties the ring");
}

static void test_resume_mark(void)
{
    u32 bad = appslot_resume_mark_selftest();
    check((bad & (1u << 0)) == 0, "resume refuses a free slot");
    check((bad & (1u << 1)) == 0, "resume needs the OP_WAIT mark (PARKED)");
    check((bad & (1u << 2)) == 0, "resume needs the kbd mark (WAIT_KEY)");
    check((bad & (1u << 3)) == 0, "kill folds WAIT_KEY but not a running app");
    check((bad & (1u << 4)) == 0, "exec_app_state adds 3/4 without moving 0/1/2");
    check((bad & (1u << 5)) == 0, "refused resume never counts as a switch");
    /* 票 T8 §7 D8 (第 3 の park 点 = ポーリング型の 1 周だけの譲り) */
    check((bad & (1u << 6)) == 0, "resume needs the poll mark (WAIT_POLL)");
    check((bad & (1u << 7)) == 0, "poll yield is throttled to one PIT tick");
    /* 票 T9 D5 (第 4 の park 点 = 明示的な譲り)。印から「EAX に何を入れるか」
     * が導けないと、sh が譲っている間に子宛の打鍵を吸って捨てる。 */
    check((bad & (1u << 8)) == 0, "resume needs the yield mark, and reads no key");
}

/* ------------------------------------------------------------------------ */
/*  画面の所有者 (票 T8 D1 / D1a)                                            */
/*                                                                          */
/*  全画面 GFX の持ち主は 1 つで、gfx_init で移り、回収で WM へ戻る。ここが  */
/*  緩むと「プログラムが抜けたのに GUI が戻らない」「宣言していないプログラム */
/*  が黙って画面を壊す」の両方が起きる。                                     */
/* ------------------------------------------------------------------------ */
static void test_gfx_owner(void)
{
    u32 bad = appslot_gfx_owner_selftest();
    check((bad & (1u << 0)) == 0, "gfx owner moves on claim, returns on exit");
    check((bad & (1u << 1)) == 0, "gfx claim without OS32X_FLAG_GFX is refused");
    check((bad & (1u << 2)) == 0, "OS32X_FLAG_CUI_ONLY refused only from GUI");
}

/* ------------------------------------------------------------------------ */
/*  起動要求表 (票 T9 D3 の受入): GUI 中の外部プログラム起動は端末 / sh から */
/*  カーネルの表を通って WM へ渡る。ここが壊れたとき実機で見えるのは         */
/*  「sh> から何も起動しない」「プロンプトに戻らない」「2 回目以降が         */
/*  ERR_FULL」だけで原因が遠いので、遷移の骨だけをブート時に踏む。          */
/*  ホスト試験 (tools/tests/test_launch.py) と同じ形。                       */
/* ------------------------------------------------------------------------ */
static void test_launch(void)
{
    u32 bad = launch_selftest();
    check((bad & (1u << 0)) == 0, "launch: child exit marks DONE and clears child");
    check((bad & (1u << 1)) == 0, "launch: a finished row is handed over once");
    check((bad & (1u << 2)) == 0, "launch: requester exit becomes an orphan KILL");
}

/* ------------------------------------------------------------------------ */
/*  KAPI が CPL=3 へ返す文字列の置き場 (票 T9 §12 R1)                       */
/*                                                                          */
/*  sys_getcwd はカーネル帯の static cwd をそのまま返していた。カーネル帯は  */
/*  USER ビット無しで張られるので、CPL=3 の sh.bin が `cd` / `pwd` で戻り値  */
/*  を読んだ瞬間に #PF → fault kill になる (実機で見えるのは「cd したら      */
/*  シェルが落ちる」だけ)。写し先はトランポリンページ (RO+USER) の空き。     */
/*  ここで踏むのは「その番地がページに収まり、PTE に USER が立っていて、     */
/*  CPL=0 の呼び手には従来どおり static cwd が返る」の 3 つ。                */
/* ------------------------------------------------------------------------ */
static void test_tramp_user_str(void)
{
    u32 bad = exec_tramp_user_selftest();
    check((bad & (1u << 0)) == 0, "getcwd scratch fits after the KAPI stubs");
    check((bad & (1u << 1)) == 0, "getcwd scratch page is present and USER");
    check((bad & (1u << 2)) == 0, "sys_getcwd copies only for CPL=3 callers");
}

/* ------------------------------------------------------------------------ */
/*  地図と実物の照合 (票 TASK_KSTACK_USER §4 の 3)                           */
/*                                                                          */
/*  PDE 0 の PTE 1024 本を include/memmap.h から導いた期待値と比べる。       */
/*  exec_init の **後** に呼ぶ — KAPI 踏み台ページ (RO+USER) が張られるのが  */
/*  exec_init なので、前に呼ぶとそれを食い違いとして数えてしまう。           */
/*                                                                          */
/*  逆転した範囲を撥ねた回数も同時に見る。paging_init の                     */
/*  paging_set_not_present(MEM_SHM_RESV_START, MEM_SHM_RESV_END) は          */
/*  start > end なので空振りしているが、呼び側が戻り値を見ていないため       */
/*  今まで誰も気づかなかった (票 §3)。                                       */
/* ------------------------------------------------------------------------ */
static void test_memmap(void)
{
    int bad = paging_memmap_selftest(exec_tramp_page_addr());
    u32 i, n;

    check(paging_range_reject_count == 0,
          "paging: no reversed (start > end) range was rejected");
    if (paging_range_reject_count != 0) {
        kprintf(0xC1, "[memmap] reversed ranges rejected: %d\n",
                (int)paging_range_reject_count);
    }

    if (bad <= 0) {
        check(bad == 0, "memmap: PDE 0 matches include/memmap.h");
        return;
    }

    /* 食い違いは **件数ぶん** kselftest_fail に積む。1 件にまとめると
     * 「1 か所ずれている」のか「帯ごと食い違っている」のかが見えない。 */
    n = paging_memmap_bad_count;
    if (n > MM_BAD_MAX) n = MM_BAD_MAX;
    for (i = 0; i < n; i++) {
        kprintf(0xC1, "[memmap] %x-%x want=%d seen=%d\n",
                paging_memmap_bad[i * 3 + 0], paging_memmap_bad[i * 3 + 1],
                (int)(paging_memmap_bad[i * 3 + 2] >> 4),
                (int)(paging_memmap_bad[i * 3 + 2] & 0xF));
        check(0, "memmap: band above differs from the map");
    }
}

/* ------------------------------------------------------------------------ */
/*  DMA プールの写像 (票 TASK_HAL_WIRING §1-3 / 受入 W5)                     */
/*                                                                          */
/*  プールは予約域 (NP) の中に開けた 64KB の穴で、present / supervisor /     */
/*  R/W でなければならない。**USER が立ってはいけない** — 立つと CPL=3 の   */
/*  アプリが装置の記述子 (CB/RFD、PCM リング) を書き換えられる。            */
/*                                                                          */
/*  この検査が意味を持つのは `memmap_seen_at` が MM_RW と MM_RWU を         */
/*  分けてからで、それより前は RW を見た時点で MM_RW を返していた。         */
/*  **見分けられることを毎回確かめる**ために、PTE 1 本に USER を立てて       */
/*  検査が落ちるところまで見て、戻す (往復 3 B6)。                          */
/* ------------------------------------------------------------------------ */
static void test_memmap_pool_user(void)
{
    u32 tramp = exec_tramp_page_addr();
    int clean, poked;

    /* 変異の前。ここが 0 でなければ test_memmap が既に報告している。 */
    clean = paging_memmap_selftest(tramp);
    check(clean == 0, "pool: map ok before mutation");
    if (clean != 0) return;   /* 既に壊れている。変異しても意味が無い */

    if (paging_poke_user_bit(MEM_DMA_POOL_BASE, 1) != 0) {
        check(0, "pool: PTE poke failed");
        return;
    }
    poked = paging_memmap_selftest(tramp);
    /* **必ず戻す** — 落ちたかどうかを見る前に戻しておく。 */
    (void)paging_poke_user_bit(MEM_DMA_POOL_BASE, 0);

    check(poked > 0, "pool: USER bit fails the map check");

    /* 戻したので、もう一度通ること。TLB は poke の中で無効化している。 */
    check(paging_memmap_selftest(tramp) == 0,
          "pool: map ok after restore");
}

int kselftest_run_post_exec(void)
{
    int before = ksel_fail;

    test_tramp_user_str();
    test_memmap();
    test_memmap_pool_user();

    if (ksel_fail != before) {
        kprintf(0xC1, "[selftest] %d FAILED after exec_init\n",
                ksel_fail - before);
    }
    return ksel_fail - before;
}

/* ------------------------------------------------------------------------ */
/*  GUI 中の CTRL+STOP は WM が宛先を決める (票 T9 §12 S6)                   */
/*                                                                          */
/*  IRQ1 は「そのとき走っていた slot」しか知らないが、GUI 配下の宛先は       */
/*  フォーカス窓の連鎖の末尾 (D8) で、それを解決できるのは WM だけ。         */
/*  ここが崩れると実機では「CTRL+STOP で端末まで消える」(立て過ぎ) か        */
/*  「暴走したアプリを畳めない」(立て無さ過ぎ) としか見えない。              */
/* ------------------------------------------------------------------------ */
static void test_abort_admit(void)
{
    u32 bad = appslot_abort_admit_selftest();
    check((bad & (1u << 0)) == 0, "CUI keeps the CTRL+STOP escape hatch (K2)");
    check((bad & (1u << 1)) == 0, "GUI running app: target is left to the WM");
    check((bad & (1u << 2)) == 0, "GUI still kills a runaway app (2s no syscall)");
    check((bad & (1u << 3)) == 0, "CTRL+STOP never lands on the shell band");
    check((bad & (1u << 4)) == 0, "GUI keeps the K5c path (inside gui_call OP_WAIT)");
}

/* ------------------------------------------------------------------------ */
/*  設定レジストリの基盤 (票 S0-K、KAPI v50)                                 */
/*                                                                          */
/*  ここで踏むのは 2 つだけ。(a) KAPI の表が v50 の形か — 末尾追記の 7 本が   */
/*  201..207 に居て既存 db_* が動いていないこと。ずれると外部プログラムは     */
/*  「別の関数を呼ぶ」という最も静かな壊れ方をする。(b) 16KB の結果ブロックの */
/*  境界検査 — header + 全列 descriptor + payload が溢れる行で範囲外へ書かず  */
/*  部分 ROW も返さないこと (票 §1b)。                                       */
/*  ホスト試験 (tools/tests/test_kapi_db_v50.py) と同じ判定を使う。          */
/* ------------------------------------------------------------------------ */
static void test_db_v50(void)
{
    u32 bad = db_v50_selftest();
    check((bad & (1u << 0)) == 0, "KAPI v50: 7 new db slots appended at 201..207");
    check((bad & (1u << 1)) == 0, "db row: 16KB block bound counts descriptors");
    check((bad & (1u << 2)) == 0, "db ptr: NULL and length overflow refused");
    check((bad & (1u << 3)) == 0, "db path: journal name fits the VFS capacity");
    check((bad & (1u << 4)) == 0, "db diag: one open-failure slot per owner ID");
    /* 票 TASK_DB_ERRSTR: db_last_error() の返り先 (SHM 末尾の診断領域) が
     * 結果データと重なっていないこと。重なると結果が診断文を踏み潰す。 */
    check((bad & (1u << 5)) == 0, "db diag: error string area is outside the result area");
    /* owner 別の欄が ID の池を覆っているか (kapi_db.h の DB_OWNER_SLOTS)。 */
    check(DB_OWNER_SLOTS >= APP_SLOT_COUNT,
          "db diag: DB_OWNER_SLOTS covers the whole app ID pool");
}

/* CPU 校正が**丸めに負けていない**こと (票 TASK_SERIAL_VFAST 往復 3)。
 *
 * 直す前は校正ループを 1 周だけ回して tick で割っていた。実機の
 * PC-9821Ra266 (266MHz) では 1 周が 1 tick に満たず `elapsed = 0 → 1` に
 * 丸められ、`s_loops_per_tick` が実際の 1/7〜1/13 になっていた。すると
 * `cpu_delay_us(5)` が 0.5µs しか待たず、シリアルの送信ループが TxRDY を
 * 待てずに `_halt()` へ落ちて **1 バイト約 2ms の固定費**になる
 * (実機実測: 9600 で 389B/s、38400 でも 437B/s)。
 *
 * **NP21/W でも丸めは起きる** (実測 rounds = 16 / ticks = 5 = 1 周 ≒ 0.31
 * tick。旧コードの生の elapsed はそこでも 0 で、補正が 200,000 を入れていた)。
 * 実機 266MHz は 1 周 0.1 tick 未満でもっと深く落ちる。症状が出るかどうかの
 * 境目は「予算が 1 文字時間を割り込むか」だけなので、ここで起動時に見る。 */
static void test_cpu_calibrate(void)
{
    u32 lpt = cpu_loops_per_tick();

    /* 測れていれば必ず下限を超える。フォールバックに倒れたら測れていない。 */
    check(lpt >= CALIBRATE_MIN_LPT,
          "cpu calib: loops_per_tick is above the floor");
    check(lpt != 0, "cpu calib: loops_per_tick was actually set");

    /* **必要な tick 数を本当に測れたか。** ここが 5 未満なら、周回を
     * 打ち切ってしまったか PIT が止まっている = 値は当てにならない。 */
    check(cpu_calib_ticks >= CALIBRATE_MIN_TICKS,
          "cpu calib: measured at least CALIBRATE_MIN_TICKS ticks");
    check(cpu_calib_rounds >= 1, "cpu calib: ran at least one round");
    /* 打ち切りに当たっていない (当たっていたら PIT を疑う)。 */
    check(cpu_calib_rounds < CALIBRATE_MAX_ROUNDS,
          "cpu calib: did not hit the round cap");

    /* **結果が測った値そのものか。** `lpt == 合計ループ / 経過 tick` を
     * 周回数に関わらず照合する。直す前のここは `|| cpu_calib_rounds > 1` が
     * 付いていて、**複数周回ったら何であれ通る**ザルだった (往復 4 の非
     * blocker)。合計は rounds × CALIBRATE_LOOPS = 最大 4000 万で u32 に収まる。
     * フォールバックに倒れた場合だけ式から外れるので、それは別に許す
     * (倒れたこと自体は下の check が落とす)。 */
    check(cpu_calib_ticks == 0
          || lpt == (cpu_calib_rounds * CALIBRATE_LOOPS) / cpu_calib_ticks
          || lpt == CALIBRATE_FALLBACK_LPT,
          "cpu calib: loops_per_tick == total loops / ticks");

    /* **打ち切りに当たったら測れていない** (PIT が止まっている疑い)。
     * 上の `< CALIBRATE_MAX_ROUNDS` と同じことを「失敗」として言い直す —
     * 当たったときに何が起きたかを名前で残すため。 */
    check(cpu_calib_rounds != CALIBRATE_MAX_ROUNDS,
          "cpu calib: did NOT give up at the round cap");

    /* **フォールバック値そのものだったら測れていない。**
     * cpu_calibrate_compute() は ticks == 0 か極端に小さい結果のときだけ
     * この値を返すので、一致したら測定が成立していない
     * (8MHz 実機でたまたま一致する確率は無視する — その場合も
     *  「測れたかどうか分からない」ので落ちてよい)。 */
    check(lpt != CALIBRATE_FALLBACK_LPT,
          "cpu calib: result is a real measurement, not the fallback");
}

/* PIT の分周が**判定したクロックに従っている**こと (票 TASK_HAL_WIRING §1-0)。
 *
 * 直す前の `pit_init()` は 1.9968MHz 決め打ちで割っていた。2.4576MHz 系
 * (実機 PC-9821Ra266、0000:0501h bit7 = 0) では 24576 を積むべきところに
 * 19968 が入り、100Hz のつもりの tick が **123Hz = 8.125ms** になる。
 * tick を数える待ち・番犬・CPU 校正がまとめて 23% 速くなるが、
 * **NP21/W は 1.9968MHz 設定なので一度も踏めない**。だからここで起動時に
 * 実機の値そのものを見る (ホスト試験 tools/tests/test_pit_clock.py は
 * 同じ算数を両クロックで見る)。 */
static void test_pit_setup(void)
{
    const struct pit_setup *p = pit_get_setup();

    /* 積んでいない (= pit_init が値を記録していない) なら以後は無意味。 */
    check(p->valid != 0, "pit: setup recorded by pit_init");

    /* **クロックを読んでいること。** 未判定のまま既定値で走ると、
     * 2.4576MHz 機が直す前と同じ分周のままになる。 */
    check(sysclk_detected() != 0, "sysclk: 0000:0501h read before pit_init");

    /* 分周に使ったのが判定値そのものか (既定へ倒れていないか)。 */
    check(p->clk_hz == sysclk_hz(), "pit: divided the detected clock");
    check(p->hz == (unsigned int)PIT_HZ, "pit: programmed PIT_HZ");
    check(p->reload == (unsigned int)(sysclk_hz() / (unsigned long)PIT_HZ),
          "pit: reload == sysclk / PIT_HZ");

    /* **どちらのクロックでもちょうど 10ms。** ここが 8125 なら 19968 を
     * 2.4576MHz 機に積んでいる (直す前の姿)。 */
    check(p->period_us == (unsigned int)(1000000UL / (unsigned long)PIT_HZ),
          "pit: tick period is exactly 1/PIT_HZ second");

    /* カウンタ#0 / LSB-MSB / モード2 (レートジェネレータ) / バイナリ。
     * モードが変わると周期そのものの意味が変わる。 */
    check(p->mode == (u8)PIT_MODE_TIMER0,
          "pit: counter 0 in mode 2 (rate generator)");
}

/* ------------------------------------------------------------------------ */
/*  DMA プールの配り方 (票 TASK_HAL_WIRING §1-3)                            */
/*                                                                          */
/*  表の算数はホスト試験 (tools/tests/test_dma_pool.py) が見る。ここが       */
/*  見るのは **実物の池**: 固定番地が memmap.h のとおりで、物理 = 仮想で、   */
/*  枯渇と解放が起動のたびに一度踏まれること。                              */
/*                                                                          */
/*  **後片付けまでが試験。** 途中で return すると池が埋まったまま残り、      */
/*  82557 の probe が取れなくなる。                                          */
/* ------------------------------------------------------------------------ */
static void test_dma_pool(void)
{
    void *a, *b, *c, *d;
    u32 pa = 0, pb = 0, pc = 0;
    u32 free_before = dma_pool_free_pages();
    u32 bad_before = dma_pool_bad_free();
    u32 leak_before = dma_pool_leaked();

    check(free_before == DMA_POOL_PAGES, "pool: starts empty");

    a = dma_pool_alloc(16 * 1024, 0, &pa);
    b = dma_pool_alloc(16 * 1024, 0, &pb);
    c = dma_pool_alloc(16 * 1024, 0, &pc);
    check(a != (void *)0 && b != (void *)0 && c != (void *)0,
          "pool: 3x16KB fit");
    /* 恒等写像。装置へ渡すのがどちらか迷わせないための約束。 */
    check(pa == (u32)a && pb == (u32)b && pc == (u32)c,
          "pool: phys == virt");
    check(pa == (u32)MEM_DMA_POOL_BASE, "pool: first span at base");
    /* **64KB 境界をまたがない。** またぐ候補は飛ばしている。 */
    check(!dma_crosses_64k(pa, 16 * 1024) && !dma_crosses_64k(pb, 16 * 1024) &&
          !dma_crosses_64k(pc, 16 * 1024),
          "pool: no 64KB straddle");

    /* 真ん中を返すと、そこに 8KB が入る。 */
    check(dma_pool_free(b) == 0, "pool: free middle");
    d = dma_pool_alloc(8 * 1024, 0, (u32 *)0);
    check(d == b, "pool: 8KB reuses it");

    /* 途中ポインタの解放は数える (装置がまだ書いているかもしれない)。 */
    check(dma_pool_free((void *)((u32)a + 4096)) < 0,
          "pool: mid pointer refused");
    check(dma_pool_bad_free() == bad_before + 1,
          "pool: bad free counted");

    /* LEAKED は二度と配らない。 */
    check(dma_pool_mark_leaked(c) == 0, "pool: mark_leaked ok");
    check(dma_pool_leaked() == leak_before + 1, "pool: leak counted");
    check(dma_pool_free(c) < 0, "pool: leaked not freeable");

    /* 枯渇。32KB より大きい要求は**空でも**通らない。 */
    check(dma_pool_alloc(33 * 1024, 0, (u32 *)0) == (void *)0,
          "pool: 33KB refused");
    check(dma_pool_alloc(0, 0, (u32 *)0) == (void *)0,
          "pool: 0 bytes refused");

    /* 後片付け。LEAKED の c は**戻せない**ので、その 16KB は使えないまま。
     * 起動ごとの自己診断で池を削るわけにはいかないので、
     * **プールを作り直す**。そのため kernel.c は `pci_bind_all` を
     * **この自己診断より後**で呼ぶ (先に呼ぶと driver の span が消える)。 */
    (void)dma_pool_free(a);
    (void)dma_pool_free(d);
    dma_pool_init();
    check(dma_pool_free_pages() == DMA_POOL_PAGES,
          "pool: empty again");
}

/* ------------------------------------------------------------------------ */
/*  8237 の共通部 (票 TASK_HAL_WIRING §1-2)                                  */
/*                                                                          */
/*  **悪い引数でハードウェアに触らないこと**だけを見る。良い引数の転送は     */
/*  FDC が起動のたびに踏んでいる (W2)。ここで出せない out が 1 つでも        */
/*  出ると、他チャネル (CS4231) の設定を壊す。                              */
/* ------------------------------------------------------------------------ */
static void test_dma8237(void)
{
    int done = -1, tc = -1;

    check(dma8237_ready() != 0, "dma: init before fdc");
    check(dma_above_1mb_state() != DMA_A20_UNKNOWN,
          "dma: 0439h recorded");

    /* ch は 0〜3。範囲外は表も引かない。 */
    check(dma_chan_setup(DMA_CHAN_COUNT, MEM_DMA_POOL_BASE, 512,
                         DMA_DIR_TO_MEM, DMA_MODE_SINGLE) < 0,
          "dma: bad channel refused");
    /* 0 バイトは積めない (カウントに -1 を積むので 65536 になる)。 */
    check(dma_chan_setup(2, MEM_DMA_POOL_BASE, 0, DMA_DIR_TO_MEM,
                         DMA_MODE_SINGLE) < 0,
          "dma: 0 bytes refused");
    /* 64KB バンクまたぎ ([HW2])。プールの境界 0x2F0000 の手前から。 */
    check(dma_chan_setup(3, MEM_DMA_POOL_BASE + 0x7000UL, 0x4000UL,
                         DMA_DIR_TO_MEM, DMA_MODE_SINGLE) < 0,
          "dma: 64KB straddle refused");
    /* 16MB 以上 */
    check(dma_chan_setup(3, 0x1000000UL, 512, DMA_DIR_TO_MEM,
                         DMA_MODE_SINGLE) < 0,
          "dma: >=16MB refused");

    /* 引数が通っても **マスクしていなければ積まない**。ch3 は誰も使って
     * いないので、ここで触っても他の装置に当たらない。 */
    check(dma_chan_setup(3, MEM_DMA_POOL_BASE, 512, DMA_DIR_TO_MEM,
                         DMA_MODE_SINGLE) < 0,
          "dma: unmasked setup refused");

    /* マスクしてから積むと通り、done / tc_event が落ちている。
     * **積んだだけでマスクは外れない**ので、ch3 は閉じたまま。 */
    dma_chan_mask(3);
    check(dma_chan_setup(3, MEM_DMA_POOL_BASE, 512, DMA_DIR_TO_MEM,
                         DMA_MODE_SINGLE) == 0,
          "dma: masked setup ok");
    check(dma_chan_state(3, &done, &tc) == 0, "dma: state readable");
    check(done == 0 && tc == 0, "dma: setup cleared TC");
    dma_chan_mask(3);   /* 念のため閉じたままにしておく */
}

int kselftest_run(void)
{
    ksel_pass = 0;
    ksel_fail = 0;

    test_mem();
    test_str();
    test_utoa();
    test_heap();
    test_kprintf();
    test_kprintf_attr();
    test_ring3_pd();
    test_map_user_keep();
    test_app_band_pde();
    test_con_sink();
    test_con_sink_render_gate();
    test_kbd_inject();
    test_resume_mark();
    test_gfx_owner();
    test_abort_admit();
    test_launch();
    test_db_v50();
    test_cpu_calibrate();
    test_pit_setup();
    test_dma8237();
    test_dma_pool();

    if (ksel_fail == 0) {
        kprintf(0xA1, "[selftest] %d/%d passed\n", ksel_pass, ksel_pass);
    } else {
        kprintf(0xC1, "[selftest] %d FAILED (%d passed)\n",
                ksel_fail, ksel_pass);
    }
    return ksel_fail;
}
