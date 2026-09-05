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
/* ======================================================================== */

#include "gfx_internal.h"   /* gfx.h (PAL_*_PORT), pc98.h, memmap.h, _out/_in */
#include "gfx_hal.h"
#include "pegc.h"
#include "paging.h"
#include "pgalloc.h"
#include "sys.h"
#include "kstring.h"
#include "kprintf.h"
#include "os32_kapi_shared.h"

/* ------------------------------------------------------------------------ */
/*  内部状態                                                                */
/* ------------------------------------------------------------------------ */
static u32 s_bb_phys   = 0;   /* バックバッファ先頭 (物理 = 仮想, 4KB 境界) */
static int s_probed    = 0;   /* probe を 1 回でも走らせたか */
static int s_probe_ok  = 0;   /* probe の結果 (キャッシュ) */
static int s_active    = 0;   /* init 済み = 拡張グラフィックモード中か */
static int s_sys16m_ram = 0;  /* 043Bh bit2 の読み値 (1 = 通常 RAM 扱い) */

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

static u8 bios_flag(u32 addr)
{
    /* アドレスを volatile 経由にして定数畳み込みを止める。直に
     * *(volatile u8 *)0x045C と書くと GCC が「ヌルポインタ近傍の配列外」と
     * 誤診断する (-Warray-bounds)。ここは BIOS ワークエリアの実アドレス。 */
    volatile u32 a = addr;
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
/*  probe — 9821 の PEGC が使えるか (9801 では必ず 0)                        */
/*                                                                          */
/*  段取り (どれか 1 つでも落ちたら 0 を返し、H1 の 9801 実装に落ちる):      */
/*    1. BIOS ワークエリアの機種判別 2 バイト。9801 はここで確実に落ちる     */
/*       ので、以降のポート叩きは 9801 では一切走らない = 回帰ゼロ。         */
/*    2. リニア窓 F00000h が「張れる」か。OS32 の RAM が 16MB システム空間に */
/*       食い込んでいたら (= 043Bh bit2=1 の 16MB 構成) 使ってはいけない。   */
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
    /* OS32 の RAM が F00000h に届いているなら、そこは 16MB システム空間では
     * なく通常 RAM (043Bh bit2=1 の構成)。PEGC VRAM は F00000h には出ないし、
     * 張ったら自分の RAM を潰す。8MB 構成では届かないので通る。 */
    if (sys_get_mem_kb() * 1024UL > PEGC_LINEAR_BASE) return 0;
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
     * ここではまだ USER は付けない — 本採用は init() で行う。 */
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
/* ------------------------------------------------------------------------ */
static void pegc_write_palette(int idx, u8 r, u8 g, u8 b)
{
    _out(PAL_IDX_PORT, idx & PEGC_PALETTE_MAX);
    _out(PAL_G_PORT, g);
    _out(PAL_R_PORT, r);
    _out(PAL_B_PORT, b);
    gfx_counters.io_accesses += 4;
}

static void pegc_palette_init(void)
{
    static const u8 cube[6] = { 0, 51, 102, 153, 204, 255 };
    const PaletteEntry *sys = palette_get_all();
    int i, r, g, b;

    /* 0-15: 9801 の 16 色をそのまま (4bit 輝度 → 8bit) */
    for (i = 0; i < PALETTE_COUNT; i++) {
        pegc_write_palette(i,
                           (u8)(sys[i].r * 17), (u8)(sys[i].g * 17),
                           (u8)(sys[i].b * 17));
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
static void pegc_init(void)
{
    u32 npages = (u32)(PEGC_FB_SIZE_480 + PAGE_SIZE - 1) / PAGE_SIZE;
    volatile u16 *tvram_char = (volatile u16 *)TVRAM_CHAR_BASE;
    volatile u8  *tvram_attr = (volatile u8  *)TVRAM_ATTR_BASE;
    int i;

    if (!pegc_probe()) return;

    /* --- バックバッファ (300KB) を物理メモリ末尾から切り出す ---
     * 0x400000-0x7FFFFF は PD ごとに差し替わる帯なので共有面を置けず、
     * それ以外の 0x500000〜mem_end は CPL=0 の子プロセスが
     * sys_usable_mem_end() まで使い切る。上限そのものを下げるのが唯一の道。
     * ホットデプロイ窓 (さらに上) は sys_hotdeploy_base() 側なので侵さない。 */
    if (s_bb_phys == 0) {
        s_bb_phys = sys_reserve_top((u32)MEM_GFX_BB8_SIZE);
        if (s_bb_phys == 0) {
            kprintf(0xC1, "[pegc] backbuffer reserve failed (mem too small)\n");
            s_probe_ok = 0;
            return;
        }
        /* pgalloc の管理域の末尾でもあるので、動的確保に配られないよう押さえる。
         * (sys_usable_mem_end() は下がったが pgalloc_init は既に済んでいる) */
        pgalloc_mark_used(s_bb_phys,
                          (int)((u32)MEM_GFX_BB8_SIZE / PAGE_SIZE));
        gfx_backend_pegc.bb_base = (u8 *)s_bb_phys;
        gfx_backend_pegc.bb_size = (u32)MEM_GFX_BB8_SIZE;
    }

    /* --- 表示モード ---
     * 31kHz (640x480 は 31kHz のときだけ指定できる) → 800 ライン VRAM 構成
     * (「480 ラインモードのデフォルト」) → 拡張グラフィックモード。 */
    _out(PEGC_HSYNC_PORT, PEGC_HSYNC_31KHZ);
    gfx_counters.io_accesses++;
    ff2_locked_write(PEGC_FF2_VRAM_800L);
    ff2_locked_write(PEGC_FF2_EXT_GFX);

    /* --- VRAM の見せ方: パックトピクセル + F00000h にリニア窓 --- */
    mmio_w8(PEGC_MMIO_PIXFMT, PEGC_PIXFMT_PACKED);
    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_ON);

    /* リニア窓を master PD に張る (RW + USER + キャッシュ無効)。
     * paging_addrspace_create() は master の PDE を全部コピーするので、
     * 以後に作られるアプリ PD からも同じ物理が見える。
     * PCD: VRAM は書き込み専用に使うデバイス窓なのでキャッシュに載せない。 */
    paging_map_phys(PEGC_LINEAR_BASE, PEGC_LINEAR_BASE, npages,
                    PAGE_RW | PTE_USER | PTE_PCD);

    /* --- 画面をクリア --- */
    kmemset((u8 *)s_bb_phys, 0, (u32)MEM_GFX_BB8_SIZE);
    kmemset((u8 *)PEGC_LINEAR_BASE, 0, (u32)PEGC_FB_SIZE_480);

    /* テキスト VRAM もクリアする (9801 の _gfx_common_init と同じ扱い)。
     * 256 色モードでテキスト面が合成されるかは未確認 (DESIGN §5 / 票 7)。 */
    for (i = 0; i < TVRAM_COLS * TVRAM_ROWS; i++) {
        tvram_char[i] = 0x0000;
        tvram_attr[i * 2] = 0x00;
    }

    pegc_palette_init();

    /* GDC は 400 ライン用の CSRFORM (ライン 2 倍表示なし) のまま。
     * ⚠ 資料 ([U] I/O 09A8h) は「周波数を切り替えたら GDC の SYNC コマンド等
     * で同期信号を設定しなおさないと正常に表示が行われない」と言うが、
     * ミラーには **480 ライン用の GDC SYNC パラメータ表が無い** (400 ライン
     * の PITCH 80/40 ワードしか載っていない)。誤った SYNC 値を流すと画面が
     * 出なくなるので、v1 では 09A8h (31kHz) と 6Ah (800 ライン構成) の指定
     * だけに留め、実際に 480 ラインのラスタが出るかは NP21/W で確認する
     * (ゲート G5)。400 ラインしか出ないようなら SYNC の再設定が次の一手。 */
    _out(GDC_GFX_CMD, GDC_GFX_400LINE);
    _out(GDC_GFX_PARAM, 0x00);
    _out(GDC_GFX_CMD, GDC_CMD_START);
    gfx_counters.io_accesses += 3;

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
     *  TEXT_OVERLAY = 0。256 色モードでテキスト面が合成されるかは資料に
     *    明文が無く (DESIGN §5 の未確認事項)、**実機で確かめるまで立てない**。
     *    確認手順は票 H2 の報告と DESIGN §5 に書き戻す。
     *  PAGE_FLIP    = 0。800 ライン VRAM 構成では 00A4h (表示ページ選択) が
     *    使えないので、PEGC のページ切替は v1 では使わない。
     *  HW_FILL/HW_BLT = 0。PEGC は塗り/転送エンジンを持たない (CPU 直書き)。 */
    info->flags  = 0;

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
/* ------------------------------------------------------------------------ */
static void pegc_set_palette(int first, int count, const u8 *rgb)
{
    int k;
    if (!rgb) return;
    for (k = 0; k < count; k++) {
        int idx = first + k;
        if (idx < 0 || idx >= PEGC_PALETTE_COUNT) continue;
        pegc_write_palette(idx, rgb[k * 3 + 0], rgb[k * 3 + 1],
                           rgb[k * 3 + 2]);
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
/*  shutdown — 標準グラフィックモード / 24kHz テキストへ戻す                 */
/*                                                                          */
/*  リニア窓を閉じる → 拡張モードを抜ける → 400 ライン VRAM 構成へ →         */
/*  24kHz へ、の順。E0000h の意味が「制御レジスタ」から「プレーン 3」へ       */
/*  戻るのは拡張モードを抜けた瞬間なので、MMIO への最後の書き込みは          */
/*  そのに済ませておく。                                                     */
/*  ※ OS32 のテキスト画面は 640x400 24.83kHz 前提。31kHz のまま抜けると      */
/*  機種によっては表示が乱れるため 24kHz へ戻す (G5 で確認する項目)。        */
/* ------------------------------------------------------------------------ */
static void pegc_shutdown(void)
{
    if (!s_active) return;

    mmio_w16(PEGC_MMIO_LINEAR, PEGC_LINEAR_OFF);
    ff2_locked_write(PEGC_FF2_STD_GFX);
    ff2_locked_write(PEGC_FF2_VRAM_400L);
    _out(PEGC_HSYNC_PORT, PEGC_HSYNC_24KHZ);
    _out(GDC_GFX_CMD, GDC_CMD_STOP);
    gfx_counters.io_accesses += 2;

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
    0                         /* bb_size:  init() が埋める */
};
