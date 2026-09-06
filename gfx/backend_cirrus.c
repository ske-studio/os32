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
/*  ⚠ **bb_base は NULL** (gfx_hal.h の「アクセラレータ系は bb_base=NULL」)。 */
/*  Xe10 内蔵の CPU 窓は 32KB しかなく (include/wab_xe10.h §3)、300KB の      */
/*  クライアント面を線形アドレスで一望することはできない。CPU 直書きは       */
/*  小さい矩形だけ、バンク窓越しに行う。                                     */
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

/* グルーが出した I/O の本数を契約 G7 のカウンタへ移す。
 * 層をまたいで gfx_counters を触らせないための緩衝 (DESIGN §7-3)。 */
static void cirrus_sync_io(void)
{
    if (!s_glue) return;
    gfx_counters.io_accesses += (s_glue->io_count - s_io_mark);
    s_io_mark = s_glue->io_count;
}

/* ------------------------------------------------------------------------ */
/*  CPU 窓 (バンク窓) 越しの書き込み                                         */
/*                                                                          */
/*  窓は 32KB でバンクは 4KB 粒度。1 行ぶんずつバンクを合わせて書く。        */
/*  小さい矩形専用なので行の長さは必ず窓に収まる (念のため確認する)。        */
/* ------------------------------------------------------------------------ */
static void cirrus_cpu_fill(u32 vram_off, u32 pitch, int w, int h, u8 color)
{
    int row;
    for (row = 0; row < h; row++) {
        u32 off = vram_off + pitch * (u32)row;
        u32 win = wab_cirrus_set_bank(s_glue, off);
        if (win + (u32)w > s_glue->win_size) return;   /* 窓からはみ出す */
        kmemset((u8 *)(s_glue->win_base + win), color, w);
    }
}

static void cirrus_cpu_write(u32 vram_off, const u8 *src, u32 len)
{
    u32 win = wab_cirrus_set_bank(s_glue, vram_off);
    if (win + len > s_glue->win_size) return;
    kmemcpy((u8 *)(s_glue->win_base + win), src, len);
}

/* ------------------------------------------------------------------------ */
/*  probe — Xe10 内蔵 (ID 5Bh) + Cirrus チップが居るか                       */
/*                                                                          */
/*  段取り:                                                                 */
/*    1. CPU 窓を張れるか。OS32 の RAM が窓まで届いていたら (実装メモリが     */
/*       多い機種) 張ってはいけないし、ページングの管理上限 (16MB) にも      */
/*       収まっていること。PEGC の probe と同じ理屈 (backend_pegc.c 段 2)。  */
/*    2. ボードグルーの ID 判定。9801 や WAB 非搭載機はここで確実に落ちる    */
/*       ので、以降の VGA ポート叩きは走らない = 回帰ゼロ。                  */
/*    3. チップの解錠キー往復 (SR6)。                                        */
/*  1 だけ先に見るのは、窓が張れないなら ID が合っても使えないため。         */
/* ------------------------------------------------------------------------ */
static int cirrus_probe(void)
{
    if (s_probed) return s_probe_ok;
    s_probed = 1;
    s_probe_ok = 0;
    s_glue = &wab_glue_xe10;

    if (sys_get_mem_kb() * 1024UL > s_glue->win_base) return 0;
    if (s_glue->win_base + s_glue->win_size > PAGING_MAP_SIZE) return 0;

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

    /* CPU 窓を master PD に張る (H はページテーブルを触らない: K の API 経由)。
     * USER は付けない — クライアント面はカード VRAM にあり、CPL=3 アプリへ
     * 見せる主記憶バックバッファは無い (bb_base = NULL)。 */
    npages = (s_glue->win_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (paging_map_phys(s_glue->win_base, s_glue->win_base, npages,
                        PAGE_RW | PTE_PCD) != 0) {
        kprintf(0xC1, "[cirrus] VRAM window map failed\n");
        s_probe_ok = 0;
        return;
    }

    if (s_glue->init) s_glue->init();

    if (wab_cirrus_setup_8bpp(s_glue, CIRRUS_WIDTH, CIRRUS_HEIGHT,
                              (u32)CIRRUS_PITCH, CIRRUS_VIS_OFF) != 0) {
        kprintf(0xC1, "[cirrus] mode setup failed\n");
        /* ここで諦めると gfx_core は 9801 へ落ち、以後 leave() も
         * shutdown() も呼ばれない。リレーは自分で 98 側へ戻しておく
         * (glue->init の FF82h が NP21/W ではリレーを倒しているため)。 */
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
     * 始まる。300KB×2 を CPU で消すとバンク切替 150 回になるため、
     * エンジンの塗りで消す (アクセラレータの正しい使い方)。 */
    wab_cirrus_fill(s_glue, CIRRUS_VIS_OFF, (u32)CIRRUS_PITCH,
                    CIRRUS_WIDTH, CIRRUS_HEIGHT, 0);
    wab_cirrus_fill(s_glue, CIRRUS_CLIENT_OFF, (u32)CIRRUS_PITCH,
                    CIRRUS_WIDTH, CIRRUS_HEIGHT, 0);
    gfx_counters.hw_ops += 2;

    cirrus_palette_init();

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
/* ------------------------------------------------------------------------ */
static void cirrus_shutdown(void)
{
    if (!s_active) return;
    cirrus_leave();
    wab_cirrus_shutdown(s_glue);
    cirrus_sync_io();
    gfx_current_height = GFX_HEIGHT;
    s_active = 0;
}

/* ------------------------------------------------------------------------ */
/*  バックエンド表。                                                        */
/*  bb_base = NULL / bb_size = 0 — クライアント面はカード VRAM にあり、      */
/*  主記憶のバックバッファを持たない (gfx_hal.h の記述子の規約)。            */
/*  bb_pitch だけは gfx_get_framebuffer が返すピッチとして意味を持つ。       */
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
    (u8 *)0,                  /* bb_base: 主記憶バックバッファ無し */
    (u32)CIRRUS_PITCH,
    GFX_BB_PACKED8,
    0                         /* bb_size: 0 = CPL=3 へマップする面が無い */
};
