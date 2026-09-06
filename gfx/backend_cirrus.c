/* ======================================================================== */
/*  BACKEND_CIRRUS.C — Cirrus GD54xx アクセラレータバックエンド (票 H3)      */
/*                                                                          */
/*  640x480 / 8bpp パックドピクセル。**クライアント面はカード VRAM の        */
/*  非表示領域**に置き、commit (present_rect) で表示面へエンジン BLT する。   */
/*  これで契約 G4 の「commit 前の描画は表示面に出ない」をソフトウェア        */
/*  バックエンド (主記憶バックバッファ) と同じ強さで守る (DESIGN §8、        */
/*  API_CONTRACTS G4 の 2026-09-05 改訂)。                                   */
/*                                                                          */
/*  層は 3 枚 (DESIGN §6):                                                   */
/*    このファイル      HAL の契約 (probe/init/query/present/palette/…)      */
/*    drivers/wab_cirrus.c   チップ (SR/GR/CR、BitBLT、DAC)                  */
/*    drivers/wab_glue_xe10.c ボード (ポート翻訳、ID、リレー、VRAM 窓)       */
/*  ここには VGA レジスタ番号も PC-98 のポート番号も書かない。               */
/*                                                                          */
/*  [HW1] は 98 標準グラフィック用の規則なのでここには適用されない           */
/*  (DESIGN B3 / §7-5): 塗りと転送はチップの 2D エンジンで行う。             */
/*                                                                          */
/*  **bb_base はリニア窓の中の非表示面** (票 H3b, 2026-09-06)。               */
/*  H3 の時点では Xe10 内蔵の CPU 窓が 32KB のバンク窓しかなく               */
/*  (include/wab_xe10.h §3)、300KB のクライアント面を線形アドレスで一望       */
/*  できないため bb_base = NULL / bb_size = 0 にしていた。結果、libos32gfx の */
/*  CPU 描画 (gshell / gdi_test) は Cirrus では #PF していた。               */
/*  H3b で 0FABh レジスタ 02h の **2MB リニア窓** (§4) を採用し、            */
/*  カーネルのページテーブルを 32MB へ広げて (kernel/paging.h の             */
/*  PAGING_RAM_LIMIT / PAGING_MAP_SIZE) 01000000h に張れるようにした。       */
/*  以後 CPU 直書きはすべてこの窓越しで、バンク切替は使わない。              */
/*    01000000h + 000000h  表示面      (CPL=3 へは見せない)                  */
/*    01000000h + 04B000h  クライアント面 = bb_base、300KB                   */
/*  窓は master には **supervisor + PCD** で張り、exec が                     */
/*  gfx_bb_phys_range() を見てこの 300KB だけをアプリ PD で USER へ昇格させる */
/*  (paging_addrspace_map_user_keep、PCD は保つ)。表示面の PTE は決して USER  */
/*  にならない = 契約 G4 (レビュー #5 ②③、2026-09-06)。                      */
/* ======================================================================== */

#include "gfx_internal.h"   /* gfx.h, pc98.h, memmap.h */
#include "gfx_hal.h"
#include "paging.h"
#include "sys.h"
#include "kstring.h"
#include "kprintf.h"
#include "palette.h"
#include "os32_kapi_shared.h"
#include "wab_glue.h"       /* -Idrivers (INC_GFX) */
#include "wab_glue_xe10.h"
#include "wab_cirrus.h"

/* ------------------------------------------------------------------------ */
/*  画面ジオメトリと VRAM の割り付け ([C4] 三層定数)                         */
/*                                                                          */
/*  Xe10 内蔵 CL-GD5430 の VRAM は実機 1MB / NP21/W では 4MB                 */
/*  ([N] pc98_cirrus_vga_setvramsize: gd54xxtype<=0xff → CIRRUS_VRAM_SIZE)。 */
/*  1MB でも収まるように前から詰める:                                        */
/*    000000h  表示面      640x480x1B = 4B000h                               */
/*    04B000h  クライアント面 (非表示) 同じ大きさ                            */
/*    096000h  塗りつぶし用 8x8 モノクロパターン (8 バイト、8B 境界)         */
/* ------------------------------------------------------------------------ */
#define CIRRUS_WIDTH        640
#define CIRRUS_HEIGHT       480
#define CIRRUS_BPP          8
#define CIRRUS_PITCH        CIRRUS_WIDTH          /* 1 バイト/画素 */
#define CIRRUS_SURFACE_SIZE ((u32)CIRRUS_PITCH * CIRRUS_HEIGHT)

#define CIRRUS_VIS_OFF      0UL
#define CIRRUS_CLIENT_OFF   CIRRUS_SURFACE_SIZE
#define CIRRUS_PATTERN_OFF  (CIRRUS_SURFACE_SIZE * 2UL)
#define CIRRUS_PATTERN_LEN  8
#define CIRRUS_VRAM_MIN     (CIRRUS_PATTERN_OFF + CIRRUS_PATTERN_LEN)

/* 面の割り付けの整合 (票 H3b)。クライアント面の先頭と大きさはページ境界で
 * なければならない — exec は gfx_bb_phys_range() の範囲を 4KB 単位で
 * USER マップするので、ずれると表示面の一部まで CPL=3 に見えてしまう。
 * リニア窓の側 (先頭がページ境界か / VRAM が収まるか) はボードの値なので
 * コンパイル時には見えない。probe() が実行時に確かめる。 */
STATIC_ASSERT((CIRRUS_CLIENT_OFF & (PAGE_SIZE - 1)) == 0,
              cirrus_client_page_aligned);
STATIC_ASSERT((CIRRUS_SURFACE_SIZE & (PAGE_SIZE - 1)) == 0,
              cirrus_surface_page_multiple);

/* パレット (契約 G8)。0〜15 はシステム色、16〜255 を貸す。 */
#define CIRRUS_PAL_COUNT    256
#define CIRRUS_LEASE_FIRST  16
#define CIRRUS_LEASE_COUNT  (CIRRUS_PAL_COUNT - CIRRUS_LEASE_FIRST)

/* KAPI の輝度は 4bit (0〜15)、VGA DAC は 6bit (0〜63)。
 * (v<<2)|(v>>2) で両端がぴったり合う (0→0, 15→63)。
 * PEGC (8bit DAC) の ×17 と同じ趣旨のスケーリング (票 H2c)。 */
#define CIRRUS_PAL_KAPI_MASK 0x0F

/* CPU 直書きに切り替える閾値 (画素数)。
 * BLT 1 回はレジスタ書き込み 20 本前後 (blt_setup + モード + ROP + 起動) で、
 * 1 本の OUT は VRAM 書き込み数回分 (DESIGN §8)。十数ピクセル角 = 16x16 =
 * 256 画素あたりで釣り合うので、そこを境にする。カウンタ (hw_ops と
 * io_accesses) で実測してから動かすこと。 */
#define CIRRUS_CPU_DIRECT_PIXELS 256

/* ------------------------------------------------------------------------ */
/*  内部状態                                                                */
/* ------------------------------------------------------------------------ */
static WabGlue *s_glue     = (WabGlue *)0;
static int      s_probed   = 0;
static int      s_probe_ok = 0;
static int      s_active   = 0;   /* init 済み = 拡張モード中か */
static int      s_relay_on = 0;
static u32      s_io_mark  = 0;   /* glue->io_count の前回値 */
static u8      *s_lin      = (u8 *)0;  /* リニア窓の先頭 (= VRAM オフセット 0) */
static u32      s_lin_pages = 0;  /* 張ったページ数 (shutdown で剥がす分) */

/* グルーが出した I/O の本数を契約 G7 のカウンタへ移す。
 * 層をまたいで gfx_counters を触らせないための緩衝 (DESIGN §7-3)。 */
static void cirrus_sync_io(void)
{
    if (!s_glue) return;
    gfx_counters.io_accesses += (s_glue->io_count - s_io_mark);
    s_io_mark = s_glue->io_count;
}

/* ------------------------------------------------------------------------ */
/*  リニア窓越しの CPU 書き込み (票 H3b)                                     */
/*                                                                          */
/*  窓は VRAM オフセット 0 から 2MB が連続して見えるので、バンクを合わせる    */
/*  必要が無い ([N] cirrus_linear_writeb は addr &= cirrus_addr_mask する     */
/*  だけで GR09 を見ない。詳細は include/wab_xe10.h §4)。                     */
/*  範囲だけは必ず確かめる — 窓の外へ書くと、そこは Not-Present なので        */
/*  カーネルが #PF で落ちる。                                                */
/* ------------------------------------------------------------------------ */
static int cirrus_lin_ok(u32 vram_off, u32 len)
{
    if (!s_lin || !s_glue) return 0;
    if (vram_off > s_glue->lin_size) return 0;
    return (len <= s_glue->lin_size - vram_off);
}

static void cirrus_cpu_fill(u32 vram_off, u32 pitch, int w, int h, u8 color)
{
    int row;
    for (row = 0; row < h; row++) {
        u32 off = vram_off + pitch * (u32)row;
        if (!cirrus_lin_ok(off, (u32)w)) return;
        kmemset(s_lin + off, color, w);
    }
}

static void cirrus_cpu_write(u32 vram_off, const u8 *src, u32 len)
{
    if (!cirrus_lin_ok(vram_off, len)) return;
    kmemcpy(s_lin + vram_off, src, len);
}

/* リニア窓を畳む: ハードウェア側 (レジスタ 02h) を閉じ、ページを
 * Not-Present に戻す。NP21/W はレジスタへの 0 を捨てる (wab_xe10.h §4) ので
 * 実効的にはページを剥がす側が効く。窓の中を指したままの bb_base を
 * 残さないよう、記述子も同時に空にする。二度呼んでも無害。 */
static void cirrus_linear_unmap(void)
{
    if (s_glue && s_glue->linear_enable) s_glue->linear_enable(0);
    if (s_lin_pages) {
        paging_map_phys(s_glue->lin_base, s_glue->lin_base, s_lin_pages,
                        PAGE_NOT_PRESENT);
        s_lin_pages = 0;
    }
    s_lin = (u8 *)0;
    gfx_backend_cirrus.bb_base = (u8 *)0;
    gfx_backend_cirrus.bb_size = 0;
}

/* ------------------------------------------------------------------------ */
/*  probe — Xe10 内蔵 (ID 5Bh) + Cirrus チップが居るか                       */
/*                                                                          */
/*  段取り:                                                                 */
/*    1. 窓を張れるか。OS32 の RAM が窓まで届いていたら (実装メモリが多い    */
/*       機種) 張ってはいけないし、ページングの守備範囲 (PAGING_MAP_SIZE)    */
/*       にも収まっていること。PEGC の probe と同じ理屈                     */
/*       (backend_pegc.c 段 2)。バンク窓 (F60000h、グルーが reg 01h で        */
/*       既定に固定する) と、H3b で採用したリニア窓 (01000000h、CPU 描画の   */
/*       本命) の両方を見る。                                                */
/*    2. ボードグルーの ID 判定。9801 や WAB 非搭載機はここで確実に落ちる    */
/*       ので、以降の VGA ポート叩きは走らない = 回帰ゼロ。                  */
/*    3. チップの解錠キー往復 (SR6)。                                        */
/*  1 だけ先に見るのは、窓が張れないなら ID が合っても使えないため。         */
/* ------------------------------------------------------------------------ */
static int cirrus_win_usable(u32 base, u32 size)
{
    if (size == 0) return 0;
    /* 実 RAM がそこまで届いているなら窓を開いてはいけない (自分の RAM を
     * 隠してしまう)。sys_get_mem_kb() は頭打ちされていない生の申告値。 */
    if (sys_get_mem_kb() * 1024UL > base) return 0;
    /* ページテーブルの守備範囲に末尾まで収まること。 */
    if (base > PAGING_MAP_SIZE) return 0;
    return (size <= PAGING_MAP_SIZE - base);
}

static int cirrus_probe(void)
{
    if (s_probed) return s_probe_ok;
    s_probed = 1;
    s_probe_ok = 0;
    s_glue = &wab_glue_xe10;

    if (!cirrus_win_usable(s_glue->win_base, s_glue->win_size)) return 0;
    /* リニア窓が使えないと CPU 描画面 (bb_base) を出せない = gshell も
     * gdi_test も描けないので、ここで諦めて PEGC / 9801 へ譲る (H3b)。 */
    if (!cirrus_win_usable(s_glue->lin_base, s_glue->lin_size)) return 0;
    if (!s_glue->linear_enable) return 0;
    /* 窓の先頭がページ境界で、使う VRAM (表示面 + 非表示面 + パターン) が
     * 窓に収まること。ボードごとの値なので実行時に見る。 */
    if (s_glue->lin_base & (PAGE_SIZE - 1)) return 0;
    if (s_glue->lin_size < (u32)CIRRUS_VRAM_MIN) return 0;

    if (!s_glue->probe || !s_glue->probe()) { cirrus_sync_io(); return 0; }
    if (!wab_cirrus_probe(s_glue))          { cirrus_sync_io(); return 0; }

    cirrus_sync_io();
    s_probe_ok = 1;
    return 1;
}

/* ------------------------------------------------------------------------ */
/*  パレット初期化 (PEGC と同じ色並び — 同じ絵が出ることが条件)              */
/*    0〜15  : 9801 の 16 色をそのまま (4bit → 6bit)                         */
/*    16〜231: 6x6x6 の色立方                                                */
/*    232〜255: グレースケール 24 段                                         */
/* ------------------------------------------------------------------------ */
static u8 cirrus_pal6(u8 v)
{
    u8 x = (u8)(v & CIRRUS_PAL_KAPI_MASK);
    return (u8)((x << 2) | (x >> 2));
}

static void cirrus_palette_init(void)
{
    /* 6x6x6 立方の各段 (0〜63 の 6 段) と灰色 24 段。PEGC の 0〜255 表を
     * 6bit へ落としたもの (51/255 ≒ 12/63)。 */
    static const u8 cube[6] = { 0, 12, 25, 38, 50, 63 };
    const PaletteEntry *sys = palette_get_all();
    int i, r, g, b;

    for (i = 0; i < PALETTE_COUNT; i++) {
        wab_cirrus_dac_set(s_glue, i, cirrus_pal6(sys[i].r),
                           cirrus_pal6(sys[i].g), cirrus_pal6(sys[i].b));
    }

    i = CIRRUS_LEASE_FIRST;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                wab_cirrus_dac_set(s_glue, i++, cube[r], cube[g], cube[b]);
            }
        }
    }
    for (b = 0; i < CIRRUS_PAL_COUNT; i++, b++) {
        u8 v = (u8)(2 + b * 2);
        if (v > 63) v = 63;
        wab_cirrus_dac_set(s_glue, i, v, v, v);
    }
}

/* ------------------------------------------------------------------------ */
/*  init — 640x480 / 256 色を立ち上げる                                      */
/*                                                                          */
/*  gfx_init() が probe で選ばれた直後に 1 回だけ呼ぶ (H1 レビュー ⑤)。      */
/*  リレーは触らない — 画面を奪うのは enter() の仕事 (DESIGN §8: リレーの    */
/*  切替はミリ秒単位 + モニタ再同期なので enter/leave のときだけ)。          */
/* ------------------------------------------------------------------------ */
static void cirrus_init(void)
{
    u8 pattern[CIRRUS_PATTERN_LEN];
    u32 npages;
    int i;

    if (!cirrus_probe()) return;

    /* ボードを起こす。glue->init() の中で 0FABh レジスタ 02h が出て
     * リニア窓が開く (番地はボードの領分。include/wab_xe10.h §4)。 */
    if (s_glue->init) s_glue->init();

    /* リニア窓を master PD に張る (H はページテーブルを触らない: K の API
     * 経由)。**supervisor + PCD** — USER は付けない (レビュー #5 ②)。
     *   USER 無し: この窓のオフセット 0 は **表示面**。master に USER で張ると
     *         paging_addrspace_create() が PDE を丸ごと写す先で CPL=3 アプリが
     *         表示 VRAM へ直接書けてしまい、契約 G4 (commit 前の描画は表示面に
     *         出ない) が崩れる。アプリに見せるのはクライアント面だけで、その
     *         300KB は exec が gfx_bb_phys_range() を見て
     *         paging_addrspace_map_user_keep() でアプリ PD ごとに昇格させる。
     *   PCD : 書き込み専用に使うデバイス窓なのでキャッシュに載せない。CPU が
     *         書いた画素を BLT エンジンが読むので、クライアント面を USER へ
     *         昇格させたあとも PCD は残らなければならない (…map_user_keep が
     *         既存 PTE の PCD/PWT を引き継ぐのはそのため)。
     * paging_addrspace_create() は master の PDE を全部コピーするので、
     * 物理そのものは以後に作られるアプリ PD からも見える (CPL=0 のみ、H3b)。 */
    npages = (s_glue->lin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    /* 失敗しても「範囲内の分は適用済み」で返ってくる (paging_map_range の
     * 契約) ので、剥がす枚数は呼ぶ前に控えておく。 */
    s_lin_pages = npages;
    if (paging_map_phys(s_glue->lin_base, s_glue->lin_base, npages,
                        PAGE_RW | PTE_PCD) != 0) {
        kprintf(0xC1, "[cirrus] linear window map failed\n");
        cirrus_linear_unmap();
        s_glue->relay(0);
        s_probe_ok = 0;
        cirrus_sync_io();
        return;
    }
    s_lin = (u8 *)s_glue->lin_base;

    if (wab_cirrus_setup_8bpp(s_glue, CIRRUS_WIDTH, CIRRUS_HEIGHT,
                              (u32)CIRRUS_PITCH, CIRRUS_VIS_OFF) != 0) {
        kprintf(0xC1, "[cirrus] mode setup failed\n");
        /* ここで諦めると gfx_core は 9801 へ落ち、以後 leave() も
         * shutdown() も呼ばれない。リレーは自分で 98 側へ戻しておく
         * (glue->init の FF82h が NP21/W ではリレーを倒しているため)。
         * リニア窓も畳む — 使わない番地を present のまま残さない。 */
        cirrus_linear_unmap();
        s_glue->relay(0);
        s_probe_ok = 0;
        cirrus_sync_io();
        return;
    }

    /* 塗りつぶし用パターン: 全ビット 1 の 8x8 モノクロ。
     * CL-GD5430 には専用の塗りつぶし機能が無く、色展開でこれを敷き詰める
     * のが唯一の道 (drivers/wab_cirrus.c 冒頭の注記)。 */
    for (i = 0; i < CIRRUS_PATTERN_LEN; i++) pattern[i] = 0xFF;
    wab_cirrus_set_fill_pattern(s_glue, CIRRUS_PATTERN_OFF);
    cirrus_cpu_write(CIRRUS_PATTERN_OFF, pattern, (u32)CIRRUS_PATTERN_LEN);

    /* 両面をクリアする。NP21/W はリセット時に VRAM を **FFh** で埋めるので
     * ([N] cirrus_reset の memset(vram_ptr, 0xff, …))、消さないと真っ白から
     * 始まる。リニア窓越しに CPU で消すこともできるが 600KB のバス転送に
     * なるので、エンジンの塗りで消す (アクセラレータの正しい使い方)。 */
    wab_cirrus_fill(s_glue, CIRRUS_VIS_OFF, (u32)CIRRUS_PITCH,
                    CIRRUS_WIDTH, CIRRUS_HEIGHT, 0);
    wab_cirrus_fill(s_glue, CIRRUS_CLIENT_OFF, (u32)CIRRUS_PITCH,
                    CIRRUS_WIDTH, CIRRUS_HEIGHT, 0);
    gfx_counters.hw_ops += 2;

    cirrus_palette_init();

    /* --- バックバッファ記述子 (票 H3b) ---
     * クライアント面はリニア窓の中の非表示面。libos32gfx はここへ CPU で
     * 直接描き、commit (present_rect) がエンジン BLT で表示面へ運ぶ。
     * exec は gfx_bb_phys_range() でこの 300KB だけをアプリ PD で USER へ
     * 昇格させる (表示面の PTE は supervisor のまま = 契約 G4)。 */
    gfx_backend_cirrus.bb_base = s_lin + CIRRUS_CLIENT_OFF;
    gfx_backend_cirrus.bb_size = (u32)CIRRUS_SURFACE_SIZE;

    /* HAL 共通の状態。ハードウェアページ切替は使わない
     * (表と裏の入れ替えではなく、非表示面から表示面への BLT で commit する)。 */
    gfx_current_height = CIRRUS_HEIGHT;
    gfx_flip_enabled = 0;
    gfx_display_page = 0;

    cirrus_sync_io();
    s_active = 1;
}

/* ------------------------------------------------------------------------ */
/*  query — 能力ビットと画面情報 (契約 G5)。副作用なし・冪等。               */
/* ------------------------------------------------------------------------ */
static int cirrus_query(GFX_ScreenInfo *info)
{
    int i;
    if (!info) return OS32_ERR_INVAL;

    info->width  = (u16)CIRRUS_WIDTH;
    info->height = (u16)CIRRUS_HEIGHT;
    info->bpp    = CIRRUS_BPP;
    info->format = GFX_FMT_PACKED8;

    /* 能力ビット (票 H3 作業 4):
     *  HW_FILL / HW_BLT = 1。初めて立つ。塗りと矩形転送を 2D エンジンが行う。
     *  TEXT_OVERLAY = 0。リレーがアクセラレータ側に倒れている間、98 の
     *    テキスト VRAM は画面に出ない (別系統の映像出力なので合成されない)。
     *  PAGE_FLIP = 0。表示面は 1 枚で、commit は非表示面からの BLT。 */
    info->flags  = GFX_CAP_HW_FILL | GFX_CAP_HW_BLT;

    info->lease_mask  = 0;
    info->lease_first = (u16)CIRRUS_LEASE_FIRST;
    info->lease_count = (u16)CIRRUS_LEASE_COUNT;
    for (i = 0; i < 5; i++) info->reserved[i] = 0;
    return 0;
}

/* 画面矩形へのクリップ。戻り値 0 = 空になった。 */
static int cirrus_clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > CIRRUS_WIDTH)  *w = CIRRUS_WIDTH - *x;
    if (*y + *h > CIRRUS_HEIGHT) *h = CIRRUS_HEIGHT - *y;
    return (*w > 0 && *h > 0);
}

/* ------------------------------------------------------------------------ */
/*  present_rect — クライアント面 (非表示) → 表示面 へエンジン BLT           */
/*                                                                          */
/*  契約 G4: commit したぶんだけが表示面に現れる。描画中の半端な状態は       */
/*  非表示面にしか無いので、途中で screenshot を撮っても前フレームのまま。   */
/*  CPU は 1 バイトも運ばないので present_bytes は増やさない (契約 G7 は     */
/*  「present で表示面へ書いたバイト数」= CPU が運んだ量)。                  */
/*  commits は gfx_vram.c の _present_dirty_packed が 1 周につき 1 に        */
/*  そろえるので、ここでは矩形ごとに 1 ずつ足しておく。                      */
/* ------------------------------------------------------------------------ */
static void cirrus_present_rect(int x, int y, int w, int h)
{
    u32 off;

    if (!s_active) return;
    if (!cirrus_clip(&x, &y, &w, &h)) return;

    off = (u32)y * CIRRUS_PITCH + (u32)x;
    wab_cirrus_copy(s_glue, CIRRUS_VIS_OFF + off, CIRRUS_CLIENT_OFF + off,
                    (u32)CIRRUS_PITCH, (u32)CIRRUS_PITCH, w, h);
    gfx_counters.hw_ops++;
    gfx_counters.commits++;
    cirrus_sync_io();
}

/* ------------------------------------------------------------------------ */
/*  set_palette — 連続する count 項目を差し替える (rgb は 3B/項目、0〜15)     */
/* ------------------------------------------------------------------------ */
static void cirrus_set_palette(int first, int count, const u8 *rgb)
{
    int k;
    if (!s_active || !rgb) return;
    for (k = 0; k < count; k++) {
        int idx = first + k;
        u8 r = rgb[k * 3 + 0];
        u8 g = rgb[k * 3 + 1];
        u8 b = rgb[k * 3 + 2];
        if (idx < 0 || idx >= CIRRUS_PAL_COUNT) continue;
        wab_cirrus_dac_set(s_glue, idx, cirrus_pal6(r), cirrus_pal6(g),
                           cirrus_pal6(b));
        if (idx < PALETTE_COUNT) palette_shadow_set(idx, r, g, b);
    }
    cirrus_sync_io();
}

/* ------------------------------------------------------------------------ */
/*  fill_rect / blit — 描画プリミティブ (契約 G5 の HW_FILL / HW_BLT)        */
/*                                                                          */
/*  どちらも**クライアント面**に対して行う (表示面には commit でしか触らない)。*/
/*  小さい矩形はエンジンの設定コストのほうが高いので CPU 直書きへ回す        */
/*  (DESIGN §8)。閾値は CIRRUS_CPU_DIRECT_PIXELS。                           */
/* ------------------------------------------------------------------------ */
static int cirrus_fill_rect(int x, int y, int w, int h, u8 color)
{
    u32 off;

    if (!s_active) return OS32_ERR_NOSYS;
    if (!cirrus_clip(&x, &y, &w, &h)) return 0;

    off = CIRRUS_CLIENT_OFF + (u32)y * CIRRUS_PITCH + (u32)x;

    if (w * h <= CIRRUS_CPU_DIRECT_PIXELS) {
        if (wab_cirrus_wait_idle(s_glue) != 0) return OS32_ERR_IO;
        cirrus_cpu_fill(off, (u32)CIRRUS_PITCH, w, h, color);
    } else {
        if (wab_cirrus_fill(s_glue, off, (u32)CIRRUS_PITCH, w, h, color) != 0) {
            cirrus_sync_io();
            return OS32_ERR_IO;
        }
        gfx_counters.hw_ops++;
    }
    cirrus_sync_io();
    return 0;
}

static int cirrus_blit(int dx, int dy, int sx, int sy, int w, int h)
{
    u32 doff, soff;
    int cw, ch;

    if (!s_active) return OS32_ERR_NOSYS;
    /* 負の座標は受け付けない (原点をずらすと転送元と転送先の対応が崩れる)。
     * 幅と高さは「両方の矩形が画面に収まる」ところまで縮める。 */
    if (dx < 0 || dy < 0 || sx < 0 || sy < 0) return OS32_ERR_INVAL;
    if (dx >= CIRRUS_WIDTH || sx >= CIRRUS_WIDTH) return 0;
    if (dy >= CIRRUS_HEIGHT || sy >= CIRRUS_HEIGHT) return 0;

    cw = w;
    ch = h;
    if (cw > CIRRUS_WIDTH - dx)  cw = CIRRUS_WIDTH - dx;
    if (cw > CIRRUS_WIDTH - sx)  cw = CIRRUS_WIDTH - sx;
    if (ch > CIRRUS_HEIGHT - dy) ch = CIRRUS_HEIGHT - dy;
    if (ch > CIRRUS_HEIGHT - sy) ch = CIRRUS_HEIGHT - sy;
    if (cw <= 0 || ch <= 0) return 0;

    doff = CIRRUS_CLIENT_OFF + (u32)dy * CIRRUS_PITCH + (u32)dx;
    soff = CIRRUS_CLIENT_OFF + (u32)sy * CIRRUS_PITCH + (u32)sx;

    if (wab_cirrus_copy(s_glue, doff, soff, (u32)CIRRUS_PITCH,
                        (u32)CIRRUS_PITCH, cw, ch) != 0) {
        cirrus_sync_io();
        return OS32_ERR_IO;
    }
    gfx_counters.hw_ops++;
    cirrus_sync_io();
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  enter / leave — 映像出力リレーの切替                                     */
/*                                                                          */
/*  リレーはメカニカル (またはアナログスイッチ) + モニタ再同期でミリ秒級な   */
/*  ので、フルスクリーン GFX の前後 = enter/leave でしか動かさない           */
/*  (DESIGN §8)。                                                           */
/*  ⚠ **leave() で必ず 98 側スルーへ戻す**。gfx_shutdown() が leave() →      */
/*  shutdown() の順で呼ぶので、`gfxmode pc98` や CUI 復帰でもここを通る。    */
/* ------------------------------------------------------------------------ */
static void cirrus_enter(void)
{
    if (!s_active || s_relay_on) return;
    s_glue->relay(1);
    s_relay_on = 1;
    cirrus_sync_io();
}

/* leave は **状態を見ずに必ず書く**。init() の Video Subsystem Enable
 * (FF82h) も NP21/W ではリレーを倒すので、enter() を一度も通らずに
 * shutdown へ来た場合でも 98 側へ戻す必要がある。同じ値を二度書いても
 * 無害 (リレーは変化があったときだけ動く)。 */
static void cirrus_leave(void)
{
    if (!s_glue || !s_glue->relay) return;
    s_glue->relay(0);
    s_relay_on = 0;
    cirrus_sync_io();
}

/* ------------------------------------------------------------------------ */
/*  shutdown — 98 側の表示へ戻す                                             */
/*  leave() が呼ばれていなくてもリレーは必ず戻す (二重に呼んでも無害)。      */
/*                                                                          */
/*  リニア窓を畳むのは **ここだけ** (leave() ではない)。leave() は映像出力の  */
/*  切替であって、enter() で戻ってくることがある — そこで窓を剥がすと        */
/*  enter() 後の描画が Not-Present ページへの書き込みになる。                */
/*  畳んだ時点で bb_base も NULL に戻るので、`gfxmode pc98` のあとに          */
/*  gfx_get_framebuffer() が死んだ窓を指すことはない。                       */
/* ------------------------------------------------------------------------ */
static void cirrus_shutdown(void)
{
    if (!s_active) return;
    cirrus_leave();
    wab_cirrus_shutdown(s_glue);
    cirrus_linear_unmap();
    cirrus_sync_io();
    gfx_current_height = GFX_HEIGHT;
    s_active = 0;
}

/* ------------------------------------------------------------------------ */
/*  バックエンド表。                                                        */
/*  bb_base / bb_size は init() が埋める (票 H3b): リニア窓の中の非表示面     */
/*  = 01000000h + 04B000h の 300KB。窓が張れるまでは値が決まらないので、      */
/*  静的初期化子では NULL / 0 のまま置く (PEGC と同じ流儀)。                 */
/*  shutdown() が窓を畳むときに NULL / 0 へ戻す。                            */
/* ------------------------------------------------------------------------ */
GfxBackend gfx_backend_cirrus = {
    "cirrus-gd54xx",
    cirrus_probe,
    cirrus_init,
    cirrus_query,
    cirrus_shutdown,
    cirrus_present_rect,
    cirrus_set_palette,
    cirrus_enter,
    cirrus_leave,
    cirrus_fill_rect,
    cirrus_blit,
    (u8 *)0,                  /* bb_base: init() が埋める (リニア窓の非表示面) */
    (u32)CIRRUS_PITCH,
    GFX_BB_PACKED8,
    0                         /* bb_size: init() が埋める (300KB) */
};
