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

int kselftest_run(void)
{
    ksel_pass = 0;
    ksel_fail = 0;

    test_mem();
    test_str();
    test_utoa();
    test_heap();
    test_kprintf();
    test_ring3_pd();
    test_map_user_keep();
    test_app_band_pde();
    test_con_sink();
    test_con_sink_render_gate();
    test_kbd_inject();
    test_resume_mark();
    test_gfx_owner();
    test_launch();

    if (ksel_fail == 0) {
        kprintf(0xA1, "[selftest] %d/%d passed\n", ksel_pass, ksel_pass);
    } else {
        kprintf(0xC1, "[selftest] %d FAILED (%d passed)\n",
                ksel_fail, ksel_pass);
    }
    return ksel_fail;
}
