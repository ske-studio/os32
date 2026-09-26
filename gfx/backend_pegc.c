/* ======================================================================== */
/*  BACKEND_PEGC.C — PC-9821 PEGC 256 色バックエンド (GfxBackend, 票 H2)     */
/*                                                                          */
/*  640x480 / 8bpp パックドピクセル。描画は主記憶のバックバッファに対して    */
/*  行い、present_rect で F00000h のリニア窓へ矩形転送する。バンク窓         */
/*  (A8000h / B0000h) は使わない — 51 ライン (32768/640) ごとに MMIO で      */
/*  バンクを切り替える必要があり、present する矩形が途中で割れる             */
/*  (DESIGN §8)。                                                            */
/*                                                                          */
/*  [HW1] EGC / GRCG / GDC 描画コマンドは一切使わない。画素は CPU から       */
/*  リニア窓へ直接書くだけ。                                                 */
/*                                                                          */
/*  ポート・MMIO・BIOS ワークエリアの番地と出典は **すべて include/pegc.h**。 */
/*  このファイルには生の番号を書かない ([C4] / 票 H2 の鉄則)。               */
/*                                                                          */
/*  ページテーブルは触らない。リニア窓は K の paging_map_phys() に張らせる。  */
/*  バックバッファは物理メモリ末尾から sys_reserve_top() で切り出す。        */
/*                                                                          */
/*  票 H2c (NP21/W 実機で出た 3 点を修正。根拠は NP21/W のソース):            */
/*    1. パレットがほぼ黒 → KAPI の輝度は 0〜15、PEGC の 256 色パレット      */
/*       レジスタは 0〜255。set_palette で ×17 して伸ばす。                   */
/*       (io/gdc.c gdc_oaa/oac/oae, vram/palettes.c pal_make9821)            */
/*    2. 480 ラインにならない → 09A8h と 6Ah だけでは足りず、GDC の SYNC     */
/*       パラメータが表示ライン数を決める。両 GDC へ 480 ライン用の SYNC を   */
/*       流す。(vram/dispsync.c dispsync_renewalvertical,                    */
/*        bios/bios18.c gdcmastersync/gdcslavesync)                          */
/*    3. grph_disp=0 → グラフィック GDC へ START (0Dh) を出していないと      */
/*       gdcs.grphdisp の GDCSCRN_ENABLE が立たず、そもそも合成されない。     */
/*       (io/gdc.c gdc_work, vram/scrndraw.c scrndraw_draw)                  */
/* ======================================================================== */

#include "gfx_internal.h"   /* gfx.h (PAL_*_PORT), pc98.h, memmap.h, _out/_in */
#include "gfx_hal.h"
#include "pegc.h"
#include "paging.h"
#include "pgalloc.h"
#include "sys.h"
#include "kstring.h"
#include "kprintf.h"
#include "palette.h"   /* palette_get_all / palette_shadow_set */
#include "os32_kapi_shared.h"

/* アプリ帯 (kernel/paging.c) がリニア窓を踏まないための照合 ([C4] 三層定数)。
 * memmap.h 側は pegc.h を読まない (9821 の文字を core に持ち込まない) ので、
 * 両方を読むここで値の一致を検査する。ここが落ちたら
 * MEM_APP_BAND_DEVICE_FLOOR を PEGC_LINEAR_BASE に合わせ直すこと。 */
STATIC_ASSERT(MEM_APP_BAND_DEVICE_FLOOR == PEGC_LINEAR_BASE,
              app_band_device_floor_is_pegc_window);

/* ------------------------------------------------------------------------ */
/*  GDC 表示タイミング表 (値と出典は include/pegc.h §9)                      */
/* ------------------------------------------------------------------------ */
static const u8 s_msync_480[PEGC_GDC_SYNC_LEN]   = PEGC_GDC_MSYNC_480;
static const u8 s_ssync_480[PEGC_GDC_SYNC_LEN]   = PEGC_GDC_SSYNC_480;
static const u8 s_msync_400[PEGC_GDC_SYNC_LEN]   = PEGC_GDC_MSYNC_400;
static const u8 s_ssync_400[PEGC_GDC_SYNC_LEN]   = PEGC_GDC_SSYNC_400;
static const u8 s_scroll_480[PEGC_GDC_SCROLL_LEN] = PEGC_GDC_SCROLL_480;
static const u8 s_scroll_400[PEGC_GDC_SCROLL_LEN] = PEGC_GDC_SCROLL_400;
static const u8 s_msync_400_31k[PEGC_GDC_SYNC_LEN] = PEGC_GDC_MSYNC_400_31K;
static const u8 s_ssync_400_31k[PEGC_GDC_SYNC_LEN] = PEGC_GDC_SSYNC_400_31K;

/* ------------------------------------------------------------------------ */
/*  内部状態                                                                */
/* ------------------------------------------------------------------------ */
static u32 s_bb_phys   = 0;   /* バックバッファ先頭 (物理 = 仮想, 4KB 境界) */
static int s_probed    = 0;   /* probe を 1 回でも走らせたか */
static int s_probe_ok  = 0;   /* probe の結果 (キャッシュ) */
static int s_active    = 0;   /* init 済み = 拡張グラフィックモード中か */
static int s_sys16m_ram = 0;  /* 043Bh bit2 の読み値 (1 = 通常 RAM 扱い) */

/* 起動時 (BIOS が作ったまま) の水平走査周波数。pegc_boot_sync_record() が
 * 1 回だけ埋め、480 ラインから戻る pegc_shutdown() がこの値へ戻す。
 * -1 = まだ記録していない (probe が通る前)。 */
static int s_boot_recorded = 0;
static int s_boot_hsync    = -1;  /* PEGC_HSYNC_24KHZ / PEGC_HSYNC_31KHZ / -1 */

/* ------------------------------------------------------------------------ */
/*  MMIO / BIOS ワークエリアアクセス                                         */
/*  E0000h も 0000:0400h 台も低位 1MB のアイデンティティマップなので、       */
/*  volatile ポインタで直接読み書きできる (ページングの追加作業は不要)。      */
/* ------------------------------------------------------------------------ */
static void mmio_w8(u32 addr, u8 val)
{
    *(volatile u8 *)addr = val;
    gfx_counters.io_accesses++;
}

static void mmio_w16(u32 addr, u16 val)
{
    *(volatile u16 *)addr = val;
    gfx_counters.io_accesses++;
}

/* BIOS ワークエリアは master PD にしか写像が無い (ページ 0 は R/O、アプリ
 * PD では not present)。CPL=3 のアプリ文脈から読むと #PF でアプリが死ぬ。
 * probe は起動時のカーネル文脈で 1 回だけ走らせて結果をキャッシュする設計
 * (kernel.c の gfx_probe_backends) なので、ここへ来るのは想定外。多重防御
 * として、master AS 以外では読まずに 0 を返す。 */
static int in_master_addrspace(void)
{
    return paging_current_cr3() == paging_kernel_pd_phys();
}

static u8 bios_flag(u32 addr)
{
    /* アドレスを volatile 経由にして定数畳み込みを止める。直に
     * *(volatile u8 *)0x045C と書くと GCC が「ヌルポインタ近傍の配列外」と
     * 誤診断する (-Warray-bounds)。ここは BIOS ワークエリアの実アドレス。 */
    volatile u32 a = addr;
    if (!in_master_addrspace()) return 0;
    return *(volatile u8 *)a;
}

/* モード F/F2 の読み戻し ([B] 表3-3): 番号を 09A0h に書いて同じ番号から読む。 */
static int pegc_stat(u8 sel)
{
    _out(PEGC_STAT_PORT, sel);
    return (_in(PEGC_STAT_PORT) & PEGC_STAT_BIT) ? 1 : 0;
}

/* 6Ah の解錠 → 値 → 施錠。20h/21h/68h/69h は解錠中しか効かない。 */
static void ff2_locked_write(u8 val)
{
    _out(MODE_FF2_PORT, PEGC_FF2_UNLOCK);
    _out(MODE_FF2_PORT, val);
    _out(MODE_FF2_PORT, PEGC_FF2_LOCK);
    gfx_counters.io_accesses += 3;
}

/* ------------------------------------------------------------------------ */
/*  GDC の表示制御 (票 H2c)                                                  */
/*                                                                          */
/*  [HW1] が禁じているのは GDC の **描画** コマンド (VECTW/VECTE/TEXTE/      */
/*  WDAT 等) であって、表示制御 (SYNC / SCROLL / START / STOP / PITCH) は    */
/*  9801 側の backend_pc98.c / gfx_core.c も従来から使っている。画素は        */
/*  引き続き CPU からリニア窓へ直接書くだけ。                                */
/*                                                                          */
/*  コマンドは 62h (マスタ = テキスト) / A2h (スレーブ = グラフィック)、      */
/*  パラメータは 60h / A0h。                                                 */
/* ------------------------------------------------------------------------ */
static void gdc_send(unsigned int cmd_port, unsigned int prm_port,
                     u8 cmd, const u8 *para, int n)
{
    int i;
    _out(cmd_port, cmd);
    for (i = 0; i < n; i++) _out(prm_port, para[i]);
    gfx_counters.io_accesses += (u32)(n + 1);
}

/* 両 GDC の SYNC を入れ直し、グラフィック GDC の表示区間を置いて表示を開始する。
 * SYNC (0Eh, DE=0) で同期パラメータを流し込んでから START (0Dh) で表示を許可
 * する — これが NP21/W 側の gdcs.textdisp / gdcs.grphdisp の GDCSCRN_ENABLE
 * (= /api/status の grph_disp) を立てる唯一の道 (io/gdc.c gdc_work())。 */
static void pegc_gdc_set_timing(const u8 *msync, const u8 *ssync,
                                const u8 *scroll)
{
    gdc_send(GDC_TEXT_CMD, GDC_TEXT_PARAM, GDC_CMD_SYNC,
             msync, PEGC_GDC_SYNC_LEN);
    gdc_send(GDC_GFX_CMD, GDC_GFX_PARAM, GDC_CMD_SYNC,
             ssync, PEGC_GDC_SYNC_LEN);
    gdc_send(GDC_GFX_CMD, GDC_GFX_PARAM, GDC_CMD_SCROLL,
             scroll, PEGC_GDC_SCROLL_LEN);

    /* 画面表示可 (モード F1 0Fh)。NP21/W の vram/scrndraw.c scrndraw_draw()
     * は gdc.mode1 bit7 が立っていないとテキストもグラフィックも合成しない。
     * 起動時から立っている値だが、モードを触った直後に念を押しておく。 */
    _out(MODE_FF1_PORT, MFF1_DISP_ON);
    _out(GDC_TEXT_CMD, GDC_CMD_START);
    _out(GDC_GFX_CMD, GDC_CMD_START);
    gfx_counters.io_accesses += 3;
}

/* テキスト VRAM の row0 行目から row1 行目の手前までをクリアする。
 * 480 ラインでは 30 行が見えるので、25 行モードのまま切り替えると
 * 26 行目以降に古い内容が残り、テキストがグラフィックを隠す
 * (NP21/W vram/sdrawex.mcr pex_2 はテキスト画素優先)。 */
static void pegc_tvram_clear(int row0, int row1)
{
    volatile u16 *tvram_char = (volatile u16 *)TVRAM_CHAR_BASE;
    volatile u8  *tvram_attr = (volatile u8  *)TVRAM_ATTR_BASE;
    int i;
    int from = TVRAM_COLS * row0;
    int to   = TVRAM_COLS * row1;

    for (i = from; i < to; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }
}

/* ------------------------------------------------------------------------ */
/*  起動時の同期状態の記録 (実機 Ra266 + 液晶の桁ズレ、TASK_FDC_REALHW §9-1)  */
/*                                                                          */
/*  読めるものだけ読む:                                                      */
/*    09A8h D0      水平走査周波数 ([U] io_disp.md「I/O 09A8h」READ/WRITE、  */
/*                  [B] §3-2 表3-1 は読み出しの D0=HF だけを定義し、D7〜D1   */
/*                  は不定)。**読み値でポートの有無を判定しない** — FFh も    */
/*                  D0=1 の正常な読みでありうる (レビュー往復 2)。           */
/*                                                                          */
/*  「PEGC probe が通った機種には 09A8h がある」の裏付けと限界:              */
/*    - [B] §3-2 表3-1 は「H98・MATE 共通の拡張 I/O ポート」として 09A8H の  */
/*      リード (D0=HF) を載せている。MATE = PC-9821 系、PEGC もこの節の      */
/*      「MATE の 256 色表示」。                                             */
/*    - [U] io_disp.md「I/O 09A8h」の対象は PC-98GS, PC-H98,                  */
/*      PC-9821[Ts を除く], PC-9801BA2･BS2･BX2, PC-9801NS/A。READ/WRITE は    */
/*      NS/A 以外 (NS/A は bit7 不明・他は未使用で意味が違う)。              */
/*    - probe の段 4 が見る 09A0h の対象 ([U] 同 1585 行) は PC-98GS, PC-H98, */
/*      PC-9821[Ts を除く], PC-9801BA2･BS2･BX2･BX3･BA3･BX4･NS/A。            */
/*      つまり **09A0h があって 09A8h の対象に無いのは BX3･BA3･BX4 だけ**、   */
/*      読みの意味が違うのが NS/A。これらは 9801 で、probe の段 1            */
/*      (045Ch bit6 と 0597h bit2) と段 5 (F00000h の PEGC リニア窓の        */
/*      書き読み) まで通る機種だという記述は資料に無い — **通らないはずだが、 */
/*      資料で直接「PEGC 機 ⇒ 09A8h あり」と書いた箇所は無い** (推論)。       */
/*    - PC-H98 は対象に入るが、H98 の 256 色は PEGC ではなく probe の段 5 を */
/*      通らない想定 (未確認)。                                               */
/*  この前提が崩れた機種では hsync= の値が当てにならない。行の MISMATCH と、 */
/*  480 ラインから戻った後の表示で気づける。                                 */
/*    054Ch bit5    BIOS の記録する水平走査周波数 ([US] memsys.md PRXCRT)。  */
/*                  9821 初代は 31kHz でも 0 なので補助にだけ使う。          */
/*    0459h bit0    BIOS の 480 ラインフラグ ([US] memsys.md CRT_EXT_STS)。   */
/*  テキスト GDC の SYNC は読めない (uPD7220 の SYNC は書き込み専用。I/O     */
/*  0062h の READ 系は READ/LPEN/CSRR だけ — [U] io_disp.md)。BIOS ワーク    */
/*  エリアにも写しは無い。だから「480 ラインへ入らない限り触らない」が本線。  */
/* ------------------------------------------------------------------------ */
static void pegc_boot_sync_record(void)
{
    u8 raw, prxcrt, crtext;
    int hs_bios;

    if (s_boot_recorded) return;
    s_boot_recorded = 1;

    raw    = (u8)_in(PEGC_HSYNC_PORT);
    prxcrt = bios_flag(PEGC_BIOS_PRXCRT);
    crtext = bios_flag(PEGC_BIOS_CRT_EXT_STS);
    gfx_counters.io_accesses++;

    /* ここへ来るのは probe が通った後だけ (pegc_prepare / pegc_init)。
     * ポートはある前提 (上の注記) で、定義のある D0 だけを採る。 */
    s_boot_hsync = (raw & PEGC_HSYNC_READ_HF) ? PEGC_HSYNC_31KHZ
                                              : PEGC_HSYNC_24KHZ;
    hs_bios = (prxcrt & PEGC_BIOS_PRXCRT_31KHZ) ? 1 : 0;

    /* 09A8h と 054Ch bit5 の食い違いは行に出す (採るのは 09A8h)。
     * 054Ch bit5=0 は 9821 初代では 31kHz でも起きる ([US] memsys.md)。
     * 09a8= は生の読み値 (D7〜D1 は不定なので値そのものに意味は無い)。 */
    kprintf(0x07, "[pegc] hsync=%s 09a8=%02x bios054c.b5=%d bios0459.b0=%d%s\n",
            s_boot_hsync == PEGC_HSYNC_31KHZ ? "31k" : "24k",
            (unsigned int)raw, hs_bios,
            (crtext & PEGC_BIOS_CRT_480LINE) ? 1 : 0,
            ((s_boot_hsync == PEGC_HSYNC_31KHZ) != hs_bios)
                ? " MISMATCH(09a8 vs 054c)" : "");
}

/* 480 ラインから戻るときの周波数。起動時に記録した値。分からなかったとき
 * (-1 = まだ記録していない。init は必ず先に記録するので、ここへは来ない
 * はず) だけ従来の 24kHz (OS32 のテキストの前提、資料の標準 SYNC と対) に
 * 落とす — H2 以来の既定動作。 */
static u8 pegc_restore_hsync(void)
{
    if (s_boot_hsync == PEGC_HSYNC_31KHZ) return PEGC_HSYNC_31KHZ;
    return PEGC_HSYNC_24KHZ;
}

/* ------------------------------------------------------------------------ */
/*  probe — 9821 の PEGC が使えるか (9801 では必ず 0)                        */
/*                                                                          */
/*  段取り (どれか 1 つでも落ちたら 0 を返し、H1 の 9801 実装に落ちる):      */
/*    1. BIOS ワークエリアの機種判別 2 バイト。9801 はここで確実に落ちる     */
/*       ので、以降のポート叩きは 9801 では一切走らない = 回帰ゼロ。         */
/*    2. リニア窓 F00000h が「張れる」か。16MB システム空間に OS32 の RAM が */
/*       登録されていたら (= 043Bh bit2=1 の 16MB 構成) 使ってはいけない。   */
/*       ページングの管理上限 (16MB) に収まることも見る。                     */
/*    3. 043Bh bit2 を読む (診断用)。PC-9801-61 型 SIMM 機ではこのポートは   */
/*       SIMM ソケットステータスなので、これだけでは決めない。書き込みもしない。*/
/*    4. 09A0h の解錠フラグが 6Ah の施錠/解錠に追随するか。open bus で常に   */
/*       FFh を返す機種 (As2 の設定 / Ts はポート自体が無い) を弾くために、   */
/*       「1 が返る」ではなく「0→1 が切り替わる」ことを見る。                */
/*    5. 実際に拡張モードへ入り、リニア窓を張って書き込み読み戻し試験。       */
/*       終わったら **必ず標準グラフィックモードへ戻す** (probe は副作用を    */
/*       残さない)。本番のモード設定は init() が行う。                       */
/* ------------------------------------------------------------------------ */
static int pegc_linear_selftest(void);

static int pegc_probe(void)
{
    int unlocked, locked;

    if (s_probed) return s_probe_ok;
    s_probed = 1;
    s_probe_ok = 0;

    /* --- 1. 機種判別 (BIOS ワークエリア, [US] memsys.md) --- */
    if (!(bios_flag(PEGC_BIOS_ARCH_FLAG) & PEGC_BIOS_ARCH_EXTGFX)) return 0;
    if (!(bios_flag(PEGC_BIOS_MODE_FLAG) & PEGC_BIOS_MODE_EXTGFX)) return 0;

    /* --- 2. リニア窓が張れるか --- */
    /* 15-16MB のシステム空間に OS32 が RAM を登録しているなら、そこは通常
     * RAM (043Bh bit2=1 の構成)。PEGC VRAM は F00000h には出ないし、張ったら
     * 自分の RAM を潰す。
     * **RAM の上端 (sys_get_mem_kb) では決めない** — K6-RAM (2026-09-11) で
     * 上端の定義が「RAM の上端アドレス / 1024」になり、15MB 構成 (ExMemory
     * 16) でも高位 RAM 16-17MB のぶん 17408 になる。上端で見ると窓が空いて
     * いるのに 9801 へ落ちた。見るべきは上端ではなく「窓に RAM が登録されて
     * いるか」で、検出器は 15-16MB を RAM にしない (memory_boot.c の
     * MEMORY_BOOT_LEGACY_END クランプ + PHYSMEM_RESERVED)。
     * 8MB 構成は窓まで RAM が届かないので従来どおり通る。 */
    if (pgalloc_range_has_ram(MEM_SYSTEM_SPACE_BASE / PAGE_SIZE,
                              MEM_HIGH_RAM_BASE / PAGE_SIZE)) return 0;
    /* ページングは先頭 16MB しか PT を持たない。窓の末尾まで入ること。 */
    if (PEGC_LINEAR_BASE + (u32)PEGC_FB_SIZE_480 > PAGING_MAP_SIZE) return 0;

    /* --- 3. 16MB 空間の設定 (診断用。判定は 5 の実測で行う) --- */
    s_sys16m_ram = (_in(PEGC_SYS16M_PORT) & PEGC_SYS16M_NORMAL_RAM) ? 1 : 0;

    /* --- 4. 09A0h が 6Ah に追随するか (open bus 除け) --- */
    _out(MODE_FF2_PORT, PEGC_FF2_UNLOCK);
    unlocked = pegc_stat(PEGC_STAT_SEL_UNLOCK);
    _out(MODE_FF2_PORT, PEGC_FF2_LOCK);
    locked = pegc_stat(PEGC_STAT_SEL_UNLOCK);
    gfx_counters.io_accesses += 2;
    if (!(unlocked == 1 && locked == 0)) return 0;

    /* --- 5. リニア窓の書き込み読み戻し --- */
    s_probe_ok = pegc_linear_selftest();
    return s_probe_ok;
}

/* 拡張モードへ一時的に入り、リニア窓を張って読み書きできるか確かめる。
 * 戻り値 1 = 使える。終了時はハードもページングも呼び出し前の状態に戻す。 */
static int pegc_linear_selftest(void)
{
    volatile u8 *fb = (volatile u8 *)PEGC_LINEAR_BASE;
    u32 npages = (u32)(PEGC_FB_SIZE_480 + PAGE_SIZE - 1) / PAGE_SIZE;
    int ok = 0;
    u8 s0, s1;

    /* 拡張グラフィックモードへ。E0000h の意味が変わるのはこの瞬間から。 */
    ff2_locked_write(PEGC_FF2_EXT_GFX);
    if (!pegc_stat(PEGC_STAT_SEL_GFXMODE)) goto restore_mode;

    mmio_w8(PEGC_MMIO_PIXFMT, PEGC_PIXFMT_PACKED);
    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_ON);

    /* K のページング API で master PD に張る (H はページテーブルを触らない)。
     * USER は付けない — init() の本採用でも同じ supervisor + PCD (表示面を
     * CPL=3 に見せないため。レビュー #5 ②)。 */
    if (paging_map_phys(PEGC_LINEAR_BASE, PEGC_LINEAR_BASE, npages,
                        PAGE_RW | PTE_PCD) != 0)
        goto disable_linear;

    /* 2 か所に別の値を書いて読み戻す。同じ値が両方から返る (エイリアス) /
     * 何も返らない (open bus) を弾く。試験に使うのは表示されない末尾側。 */
    s0 = fb[0];
    s1 = fb[PEGC_FB_SIZE_480 - 1];
    fb[0] = 0x5A;
    fb[PEGC_FB_SIZE_480 - 1] = 0xA5;
    if (fb[0] == 0x5A && fb[PEGC_FB_SIZE_480 - 1] == 0xA5) ok = 1;
    fb[0] = s0;
    fb[PEGC_FB_SIZE_480 - 1] = s1;

    if (!ok) paging_map_phys(PEGC_LINEAR_BASE, PEGC_LINEAR_BASE, npages, 0);

disable_linear:
    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_OFF);
restore_mode:
    ff2_locked_write(PEGC_FF2_STD_GFX);
    return ok;
}

/* ------------------------------------------------------------------------ */
/*  パレット初期化                                                          */
/*                                                                          */
/*  0〜15  : システム色。9801 の 16 色パレットと同じ色相を 4bit → 8bit へ    */
/*           伸ばす (x * 17 で 0-15 → 0-255)。既存アプリの色番号がそのまま   */
/*           同じ見た目になる = gdi_test が無変更で同じ絵を出す条件。        */
/*  16〜231: 6×6×6 の色立方 (216 色)。                                       */
/*  232〜255: 24 段のグレースケール。                                        */
/*  16 以降が契約 G8 のリース範囲 (lease_first=16, lease_count=240)。         */
/*                                                                          */
/*  ⚠ **輝度のスケールに 2 つの世界がある** (票 H2c の障害):                  */
/*    - ハードウェア (AAh/ACh/AEh) は 256 色モードでは **0〜255**。           */
/*      NP21/W は書いた値をそのまま np2_pal32 の RGB 成分にする               */
/*      (io/gdc.c gdc_oaa/oac/oae → vram/palettes.c pal_make9821)。          */
/*    - KAPI (gfx_set_palette / gfx_lease_palette) は **0〜15**。             */
/*      契約 G6 の GUI_SYSTEM_PALETTE も 0〜15 で、gshell が起動時に          */
/*      gfx_set_palette で 16 色ぶん流し込む (userland/gshell wm.rs)。        */
/*  H2 では KAPI 値を素通ししていたため、gshell がシステム色を入れた瞬間に     */
/*  0〜15 が 8bit レジスタへ書かれ、白 (15,15,15) が輝度 15/255 ≒ ほぼ黒に    */
/*  なっていた (実測: 画面全体が暗く、文字がかろうじて見える)。               */
/*  → KAPI から来る値は必ず pegc_pal8() で 8bit へ伸ばす。                    */
/* ------------------------------------------------------------------------ */
static void pegc_write_palette(int idx, u8 r, u8 g, u8 b)
{
    _out(PAL_IDX_PORT, idx & PEGC_PALETTE_MAX);
    _out(PAL_G_PORT, g);
    _out(PAL_R_PORT, r);
    _out(PAL_B_PORT, b);
    gfx_counters.io_accesses += 4;
}

/* KAPI の 4bit 輝度 (0〜15) → PEGC の 8bit 輝度 (0〜255)。15*17 = 255。 */
static u8 pegc_pal8(u8 v)
{
    return (u8)((v & PEGC_PAL_KAPI_MASK) * PEGC_PAL_KAPI_SCALE);
}

static void pegc_palette_init(void)
{
    static const u8 cube[6] = { 0, 51, 102, 153, 204, 255 };
    const PaletteEntry *sys = palette_get_all();
    int i, r, g, b;

    /* 0-15: 9801 の 16 色をそのまま (4bit 輝度 → 8bit) */
    for (i = 0; i < PALETTE_COUNT; i++) {
        pegc_write_palette(i, pegc_pal8(sys[i].r), pegc_pal8(sys[i].g),
                           pegc_pal8(sys[i].b));
    }

    /* 16-231: 6x6x6 色立方 */
    i = PEGC_LEASE_FIRST;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                pegc_write_palette(i++, cube[r], cube[g], cube[b]);
            }
        }
    }

    /* 232-255: グレースケール 24 段 */
    for (b = 0; i < PEGC_PALETTE_COUNT; i++, b++) {
        u8 v = (u8)(8 + b * 10);
        pegc_write_palette(i, v, v, v);
    }
}

/* ------------------------------------------------------------------------ */
/*  init — 640x480 / 256 色を立ち上げる                                      */
/*                                                                          */
/*  gfx_init() が「probe で選ばれた直後」に 1 回だけ呼ぶ (H1 レビュー ⑤)。   */
/*  9801 の GDC / プレーン初期化は走らない (あちらは init が NULL)。         */
/* ------------------------------------------------------------------------ */
/* バックバッファ (300KB) を物理メモリ末尾から切り出す。済んでいれば何もしない。
 * 0x400000-0x7FFFFF は PD ごとに差し替わる帯なので共有面を置けず、
 * それ以外の 0x500000〜mem_end は CPL=0 の子プロセスが
 * sys_usable_mem_end() まで使い切る。上限そのものを下げるのが唯一の道。
 * ホットデプロイ窓 (さらに上) は sys_hotdeploy_base() 側なので侵さない。
 * 戻り値 1 = 使える。失敗したら probe を取り下げる (9801 へ落ちる)。 */
static int pegc_reserve_backbuffer(void)
{
    if (s_bb_phys != 0) return 1;
    s_bb_phys = sys_reserve_top((u32)MEM_GFX_BB8_SIZE);
    if (s_bb_phys == 0) {
        kprintf(0xC1, "[pegc] backbuffer reserve failed (mem too small)\n");
        s_probe_ok = 0;
        return 0;
    }
    /* pgalloc の管理域の末尾でもあるので、動的確保に配られないよう押さえる。
     * (sys_usable_mem_end() は下がったが pgalloc_init は既に済んでいる) */
    pgalloc_mark_used(s_bb_phys, (int)((u32)MEM_GFX_BB8_SIZE / PAGE_SIZE));
    /* **予約した直後に 0 で埋める**。この面は exec が CPL=3 のアプリへ USER
     * で写す (exec/exec.c、gfx_bb_phys_range)。prepare は init と違って画面を
     * クリアしないので、ここで埋めないと最初の gfx_init までの間、予約前の
     * 物理ページの残り (カーネルのデータを含みうる) がアプリから見える
     * (レビュー往復 1)。 */
    kmemset((u8 *)s_bb_phys, 0, (u32)MEM_GFX_BB8_SIZE);
    gfx_backend_pegc.bb_base = (u8 *)s_bb_phys;
    gfx_backend_pegc.bb_size = (u32)MEM_GFX_BB8_SIZE;
    return 1;
}

/* リニア窓を master PD に張る (**supervisor** + RW + キャッシュ無効)。
 * 何度呼んでも同じ写像になる。
 *
 * H2 の初版はここに PTE_USER を付けていた。理由は「paging_addrspace_create
 * が master の PDE を写すので全アプリ PD で共有される」— つまりアプリから
 * 見えるようにするため、だった。だが F00000h は **表示面そのもの** で、
 * PEGC のバックバッファは主記憶側 (sys_reserve_top の 300KB) にある。
 * アプリが要るのはそちらだけで、exec が gfx_bb_phys_range() 経由で
 * USER マップする。表示面を USER にすると CPL=3 が commit を経ずに画面へ
 * 書けてしまい契約 G4 が崩れるので外した (レビュー #5 ②、2026-09-06)。
 * PCD: VRAM は書き込み専用に使うデバイス窓なのでキャッシュに載せない。 */
static void pegc_map_linear(void)
{
    u32 npages = (u32)(PEGC_FB_SIZE_480 + PAGE_SIZE - 1) / PAGE_SIZE;
    paging_map_phys(PEGC_LINEAR_BASE, PEGC_LINEAR_BASE, npages,
                    PAGE_RW | PTE_PCD);
}

/* ------------------------------------------------------------------------ */
/*  prepare — 起動時の下ごしらえ (gfx_prepare_backend が 1 回だけ呼ぶ)       */
/*                                                                          */
/*  **表示のモードも同期も変えない**。やるのは                               */
/*    1. 起動時の同期状態の記録 (と診断の 1 行)                              */
/*    2. バックバッファの予約 (アプリが走る前でないと取れない)               */
/*    3. リニア窓の写像 (master PD。以後に作るアプリ PD へ PDE が写る)       */
/*  だけ。かつては init → shutdown で済ませていたため、CUI しか使わない      */
/*  起動でも 31kHz/480 ラインへ入って 24kHz/400 ラインの標準 SYNC で戻して   */
/*  いた。実機 (PC-9821Ra266 + 液晶 LCD172VXM) ではテキストが 1 行ごとに     */
/*  1 文字ずつ右へずれ、GFX=pc98 (PEGC を選ばない) で消えた                  */
/*  (TASK_FDC_REALHW §9-1)。NP21/W は同期を模擬しないので再現しない。         */
/*                                                                          */
/*  ⚠ **原因は仮説のまま** (この変更での実機確認はまだ):                     */
/*   - 見立て: 起動時の送り直し (09A8h + GDC SYNC) が BIOS の作った同期と    */
/*     合わず、液晶の自動調整が合わなくなった。                              */
/*   - ただし **24kHz で起動していた場合、旧経路でも最終状態は同じ**         */
/*     (24kHz + 資料の標準 SYNC = BIOS の値)。違いは途中で 31kHz/480 を      */
/*     一瞬通ることと、同じ値の SYNC を入れ直すことだけ。起動行の          */
/*     `[pegc] hsync=` が 24k なら、最終値の食い違いでは説明できない。       */
/*   - それでも直らなければ次に疑う候補 (GFX=pc98 では両方とも走らない):     */
/*       a. PEGC probe (pegc_linear_selftest) の 6Ah 21h → 20h の出入り      */
/*          (拡張グラフィックモードへ入って戻る)。prepare でも走る。          */
/*       b. gdc_send が GDC の FIFO (ステータスの FIFO FULL/EMPTY) を待たずに  */
/*          コマンドとパラメータを流すこと。実機の GDC で SYNC の 8 バイトが   */
/*          取りこぼされうる (今回は注記のみ、直していない)。              */
/* ------------------------------------------------------------------------ */
static void pegc_prepare(void)
{
    if (!pegc_probe()) return;
    pegc_boot_sync_record();
    if (!pegc_reserve_backbuffer()) return;
    pegc_map_linear();
}

static void pegc_init(void)
{
    if (!pegc_probe()) return;

    /* prepare を経ずに来た場合 (今の経路には無いが) も起動時の状態を先に
     * 記録しておく — 31kHz を書いた後では読み戻しが意味を失う。 */
    pegc_boot_sync_record();

    if (!pegc_reserve_backbuffer()) return;

    /* --- 表示モード ---
     * 31kHz (640x480 は 31kHz のときだけ指定できる) → GDC の同期信号を
     * 480 ライン用に入れ直す → 800 ライン VRAM 構成 (「480 ラインモードの
     * デフォルト」) → 拡張グラフィックモード。
     *
     * SYNC を 6Ah の 69h/21h より **前** に置くのが肝 (票 H2c)。NP21/W は
     * 表示サイズを毎フレーム再計算するが、それを走らせるのは
     * gdcs.textdisp / gdcs.grphdisp の GDCSCRN_EXT で、09A8h への書き込み
     * (io/gdc.c gdc_o9a8) と gdc_analogext() (6Ah 21h) がそれを立てる
     * (pccore.c → vram/dispsync.c dispsync_renewalvertical)。SYNC を先に
     * 入れておけば、その再計算が新しい 480 ラインの値を読む。 */
    _out(PEGC_HSYNC_PORT, PEGC_HSYNC_31KHZ);
    gfx_counters.io_accesses++;
    pegc_gdc_set_timing(s_msync_480, s_ssync_480, s_scroll_480);
    ff2_locked_write(PEGC_FF2_VRAM_800L);
    ff2_locked_write(PEGC_FF2_EXT_GFX);

    /* --- VRAM の見せ方: パックトピクセル + F00000h にリニア窓 --- */
    mmio_w8(PEGC_MMIO_PIXFMT, PEGC_PIXFMT_PACKED);
    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_ON);

    /* リニア窓を master PD に張る (属性と理由は pegc_map_linear)。
     * probe の pegc_linear_selftest() と同じ属性 = 本採用でも USER は付けない。 */
    pegc_map_linear();

    /* --- 画面をクリア --- */
    kmemset((u8 *)s_bb_phys, 0, (u32)MEM_GFX_BB8_SIZE);
    kmemset((u8 *)PEGC_LINEAR_BASE, 0, (u32)PEGC_FB_SIZE_480);

    /* テキスト VRAM もクリアする (9801 の _gfx_common_init と同じ扱い)。
     * 480 ラインでは 30 行ぶんが見えるので 25 行では足りない (票 H2c)。
     * テキスト面が非 0 の画素はグラフィックより優先されるため、消し残しは
     * そのまま窓を隠す (NP21/W vram/sdrawex.mcr pex_2)。
     * 文字セルは 400/25 行と 480/30 行で同じ 16 ラスタなので、CSRFORM も
     * テキスト CRTC (70h〜) も触る必要はない
     * (NP21/W bios/bios18.c crtdata の "400-25" と "480-30" が同値)。 */
    pegc_tvram_clear(0, PEGC_TEXT_ROWS_480);

    pegc_palette_init();

    /* GDC の CSRFORM は触らない: 256 色パックド表示 (NP21/W
     * vram/makegrex.c makegrphex) はライン 2 倍表示を見ないし、GDC_PITCH も
     * 起動時のまま (BIOS 既定 40 ワード → 実効 80 = 640 バイト/ライン) で
     * 480 ラインでも変わらない。表示の開始と区間長は上の SCROLL で確定済み。 */

    /* HAL 共通の状態。PEGC ではハードウェアページ切替を使わない
     * (800 ライン構成では 00A4h が使えない — pegc.h の PEGC_FF2_VRAM_800L)。 */
    gfx_current_height = PEGC_HEIGHT_480;
    gfx_flip_enabled = 0;
    gfx_display_page = 0;

    s_active = 1;
}

/* ------------------------------------------------------------------------ */
/*  query — 能力ビットと画面情報 (契約 G5)。副作用なし・冪等。               */
/* ------------------------------------------------------------------------ */
static int pegc_query(GFX_ScreenInfo *info)
{
    int i;
    if (!info) return OS32_ERR_INVAL;

    info->width  = (u16)PEGC_WIDTH;
    info->height = (u16)PEGC_HEIGHT_480;
    info->bpp    = PEGC_BPP;
    info->format = GFX_FMT_PACKED8;

    /* 能力ビット (票 7):
     *  TEXT_OVERLAY = 1。NP21/W で実測 (2026-09-06): 256 色モード中に TVRAM へ
     *    書いた文字 (gui_busy の kprintf) が 256 色の絵の上に合成されて見えた
     *    (DESIGN §5 に書き戻し済み)。R4 の TVRAM クロームは PEGC でも使える。
     *  PAGE_FLIP    = 0。800 ライン VRAM 構成では 00A4h (表示ページ選択) が
     *    使えないので、PEGC のページ切替は v1 では使わない。
     *  HW_FILL/HW_BLT = 0。PEGC は塗り/転送エンジンを持たない (CPU 直書き)。 */
    info->flags  = GFX_CAP_TEXT_OVERLAY;

    /* パレットのリース (契約 G8): 256 色機は連続範囲で申告する。
     * lease_mask は 16 色機用なので 0。 */
    info->lease_mask  = 0;
    info->lease_first = (u16)PEGC_LEASE_FIRST;
    info->lease_count = (u16)PEGC_LEASE_COUNT;
    for (i = 0; i < 5; i++) info->reserved[i] = 0;
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  present_rect — バックバッファの矩形をリニア窓へ転送                      */
/*                                                                          */
/*  1 ライン 640 バイトの単純な矩形コピー。プレーンに分解しないので 9801 の   */
/*  ダーティキュー (gfx_vram.c) は通さず、ここで直接転送する。               */
/*  ⚠ 資料 ([B] §2-7「VRAM アクセスの注意点」):「32 ビットマシンであっても    */
/*  VRAM は 16 ビットバス接続であるため、32 ビットアクセスをしても 16 ビット  */
/*  2 回分の時間がかかります」。rep movsd はバス時間を縮めないが、発行する    */
/*  命令数は半分になる (票 5)。                                              */
/*  カウンタは 1 バイト/画素で加算する = 全画面で 640*480 = 307200 (完了条件)。*/
/* ------------------------------------------------------------------------ */
static void pegc_present_rect(int x, int y, int w, int h)
{
    u8 *src;
    u8 *dst;
    int row;
    u32 bytes;

    if (!s_active || s_bb_phys == 0) return;

    /* 画面クリップ */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PEGC_WIDTH)      w = PEGC_WIDTH - x;
    if (y + h > PEGC_HEIGHT_480) h = PEGC_HEIGHT_480 - y;
    if (w <= 0 || h <= 0) return;

    src = (u8 *)s_bb_phys + (u32)y * PEGC_PITCH + (u32)x;
    dst = (u8 *)PEGC_LINEAR_BASE + (u32)y * PEGC_PITCH + (u32)x;
    bytes = (u32)w;

    for (row = 0; row < h; row++) {
        /* 先頭の端数 → dword 単位 → 末尾の端数。x と w が 4 の倍数なら
         * 端数ループは 0 回で、丸ごと rep movsd になる。 */
        u32 head = ((4u - ((u32)dst & 3u)) & 3u);
        u32 mid;
        u32 tail;
        if (head > bytes) head = bytes;
        mid  = (bytes - head) >> 2;
        tail = (bytes - head) - (mid << 2);

        if (head) kmemcpy(dst, src, head);
        if (mid)  _memcpy_d(dst + head, src + head, mid);
        if (tail) kmemcpy(dst + head + (mid << 2), src + head + (mid << 2),
                          tail);

        src += PEGC_PITCH;
        dst += PEGC_PITCH;
    }

    gfx_counters.present_bytes += bytes * (u32)h;   /* 1 バイト/画素 */
    gfx_counters.commits++;
}

/* ------------------------------------------------------------------------ */
/*  set_palette — 連続する count 項目を差し替える (rgb は 3B/項目)           */
/*  ハードウェアの並びは G, R, B ([U] パレットレジスタ) だが、HAL の引数は    */
/*  9801 と同じ R, G, B 順。                                                 */
/*                                                                          */
/*  引数の輝度は **KAPI のスケール = 0〜15** (契約 G6/G8 の GuiRgb)。         */
/*  gfx_set_palette (9801 互換の 1 色差し替え) も gfx_lease_palette (G8) も   */
/*  この単位で来るので、ここで一括して 8bit へ伸ばす (票 H2c)。               */
/*  0〜15 のシステム色はシャドウ台帳にも書き戻し、gfx_get_palette            */
/*  (= palette_get) が実際の色を返せるようにしておく。                        */
/* ------------------------------------------------------------------------ */
static void pegc_set_palette(int first, int count, const u8 *rgb)
{
    int k;
    if (!rgb) return;
    for (k = 0; k < count; k++) {
        int idx = first + k;
        u8 r = rgb[k * 3 + 0];
        u8 g = rgb[k * 3 + 1];
        u8 b = rgb[k * 3 + 2];
        if (idx < 0 || idx >= PEGC_PALETTE_COUNT) continue;
        pegc_write_palette(idx, pegc_pal8(r), pegc_pal8(g), pegc_pal8(b));
        if (idx < PALETTE_COUNT) palette_shadow_set(idx, r, g, b);
    }
}

/* ------------------------------------------------------------------------ */
/*  enter / leave — 表示出力の切替。PEGC は本体のグラフィック出力そのもの    */
/*  なのでリレーが無く、両方とも空 (9801 と同じ)。表示リレーを持つのは       */
/*  H3 のアクセラレータだけ (DESIGN §8)。                                    */
/* ------------------------------------------------------------------------ */
static void pegc_enter(void) { }
static void pegc_leave(void) { }

/* ------------------------------------------------------------------------ */
/*  shutdown — 標準グラフィックモード / 起動時の周波数のテキストへ戻す        */
/*                                                                          */
/*  リニア窓を閉じる → 拡張モードを抜ける → 400 ライン VRAM 構成へ →         */
/*  24kHz へ、の順。E0000h の意味が「制御レジスタ」から「プレーン 3」へ       */
/*  戻るのは拡張モードを抜けた瞬間なので、MMIO への最後の書き込みは          */
/*  そのに済ませておく。                                                     */
/*  ※ 周波数は **起動時に BIOS が作っていた値** へ戻す (pegc_boot_sync_record */
/*  の記録)。以前は 24kHz 決め打ちで、31kHz で起動する機種では戻った後の     */
/*  同期が起動時と変わっていた (桁ずれとの関係は仮説、TASK_FDC_REALHW §9-1)。 */
/*  s_active が 0 の                                                         */
/*  とき (480 ラインへ入っていない) は何もしない = 同期に一切触らない。      */
/*  ※ 480 ライン化で GDC の SYNC を書き換えているので、周波数だけでなく      */
/*  同期パラメータと表示区間も 400 ライン用へ確実に戻す (票 H2c)。           */
/*  戻したあとグラフィック GDC は STOP にする — 9801 経路の gfx_init() が     */
/*  改めて START を出すし、テキストだけの状態では表示していてはいけない       */
/*  (従来の挙動を維持)。テキスト GDC は START のままにしてコンソールを残す。  */
/* ------------------------------------------------------------------------ */
/* 09A8h を hs に → それに合う 400 ラインの SYNC・表示区間 → グラフィック GDC を
 * 止める。pegc_shutdown と pegc_restore_text_sync が共有する後半。 */
static void pegc_text_sync_400(u8 hs)
{
    _out(PEGC_HSYNC_PORT, hs);
    gfx_counters.io_accesses++;
    if (hs == PEGC_HSYNC_31KHZ) {
        pegc_gdc_set_timing(s_msync_400_31k, s_ssync_400_31k, s_scroll_400);
    } else {
        pegc_gdc_set_timing(s_msync_400, s_ssync_400, s_scroll_400);
    }

    _out(GDC_GFX_CMD, GDC_CMD_STOP);
    gfx_counters.io_accesses++;
}

static void pegc_shutdown(void)
{
    u8 hs;

    if (!s_active) return;

    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_OFF);
    ff2_locked_write(PEGC_FF2_STD_GFX);
    ff2_locked_write(PEGC_FF2_VRAM_400L);

    /* 480 ラインでだけ見えていた 26〜30 行目を消す。25 行までは
     * コンソールの内容なので触らない (init 側で一度消してある)。 */
    pegc_tvram_clear(TVRAM_ROWS, PEGC_TEXT_ROWS_480);

    /* 起動時に記録した周波数へ戻し、それに合う 400 ラインの SYNC を入れる
     * (24kHz 決め打ちをやめた。TASK_FDC_REALHW §9-1)。起動時の 09A8h が
     * 読めなかった機種は従来どおり 24kHz。 */
    hs = pegc_restore_hsync();
    pegc_text_sync_400(hs);

    gfx_current_height = GFX_HEIGHT;
    s_active = 0;
}

/* ------------------------------------------------------------------------ */
/*  pegc_restore_text_sync — ROM で戻れなかったときの OS32 側の戻し          */
/*                                                                          */
/*  `v86 -g` (kernel/v86_gcap.c、票 TASK_PEGC480_REALHW §3 段 1) は実機の    */
/*  ROM の INT 18h AH=30h で 480 ラインへ入り、同じ AH=30h で元のモードへ     */
/*  戻す。その戻しが失敗した (AH≠05h・打ち切り・暴走) ときだけここへ来る。    */
/*  やることは pegc_shutdown の後半と同じ考え方 — 拡張モードを抜け、        */
/*  **採取の前に AH=31h で読んだ周波数** (hsync31) の 400 ラインの SYNC を   */
/*  入れ、グラフィック GDC を止める。24kHz 固定にはしない (CUI の桁ずれが     */
/*  再発する、TASK_FDC_REALHW §9-1)。                                        */
/*  リニア窓は拡張モードのときだけ閉じる (標準モードの E0000h はプレーン 3 で、*/
/*  書くと画素を 1 つ壊すだけだが意味が無い)。09A0h は PEGC の probe が通った */
/*  機種にしか無いので、読むのもそのときだけ。                              */
/* ------------------------------------------------------------------------ */
void pegc_restore_text_sync(int hsync31)
{
    /* 6Ah の 07h/20h/68h/06h と 09A0h は PEGC の probe が通った機種の値。
     * 9801 で ROM の戻しに失敗してここへ来ても書かない (代行レビュー P3-2)。 */
    if (s_probe_ok) {
        if (pegc_stat(PEGC_STAT_SEL_GFXMODE)) {
            mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_OFF);
        }
        ff2_locked_write(PEGC_FF2_STD_GFX);
        ff2_locked_write(PEGC_FF2_VRAM_400L);
    }
    pegc_text_sync_400(hsync31 ? PEGC_HSYNC_31KHZ : PEGC_HSYNC_24KHZ);

    gfx_current_height = GFX_HEIGHT;
    s_active = 0;
}

/* ------------------------------------------------------------------------ */
/*  バックエンド表。                                                        */
/*  const ではない: bb_base / bb_size を init() が実行時に埋める             */
/*  (バックバッファは物理メモリ末尾からの動的な切り出し)。                   */
/* ------------------------------------------------------------------------ */
GfxBackend gfx_backend_pegc = {
    "pegc-256",
    pegc_probe,
    pegc_init,
    pegc_query,
    pegc_shutdown,
    pegc_present_rect,
    pegc_set_palette,
    pegc_enter,
    pegc_leave,
    (int (*)(int, int, int, int, u8))0,        /* fill_rect: CPU 実装へ */
    (int (*)(int, int, int, int, int, int))0,  /* blit:      CPU 実装へ */
    (u8 *)0,                  /* bb_base:  init() が埋める */
    (u32)PEGC_PITCH,          /* bb_pitch: 640 バイト/ライン */
    GFX_BB_PACKED8,
    0,                        /* bb_size:  init() が埋める */
    pegc_prepare              /* 起動時: 予約・写像・同期の記録だけ */
};
