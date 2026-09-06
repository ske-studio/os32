/* ======================================================================== */
/*  PAGING.C — x86ページング (メモリ保護)                                   */
/*                                                                          */
/*  アイデンティティマッピング方式:                                         */
/*    仮想アドレス = 物理アドレス の1:1対応を維持し、                        */
/*    ページ属性(R/W, Present)でメモリ保護のみを追加する。                   */
/*    既存コード(VRAMアクセス等)を一切変更せずに保護が得られる。             */
/*                                                                          */
/*  構造:                                                                   */
/*    page_directory[1024]  — ページディレクトリ (4KB)                      */
/*    page_tables[8][1024]  — ページテーブル8枚 = 32MBカバー (32KB)         */
/*    合計BSS: ~44KB (アライメント用パディング含む)                         */
/*                                                                          */
/*  保護マップ (ブートアーキテクチャ改善後):                               */
/*                                                                          */
/*  [コンベンショナルメモリ]                                                */
/*    0x00000 - 0x00FFF : NP   (NULLポインタ検出, paging_reclaim後)          */
/*    0x01000 - 0x9FFFF : R/W  (フォント/Unicode/GFX, ブート後に再利用)      */
/*    0xA0000 - 0xEFFFF : R/W  (テキスト/グラフィックVRAM)                  */
/*    0xF0000 - 0xFFFFF : R/O  (BIOS ROM)                                   */
/*                                                                          */
/*  [拡張メモリ]                                                            */
/*    0x100000 - 0x1FAFFF : R/W  (カーネル帯域: code+heap+KAPI+SHM)       */
/*    0x1FB000 - 0x1FBFFF : NP   (カーネルスタックガード)                    */
/*    0x1FC000 - 0x1FFFFF : R/W  (カーネルスタック, 16KB)                    */
/*    0x200000 - 0x23FFFF : R/W  (SQLite帯域: code+BSS+代替スタック)      */
/*    0x240000 - 0x2FFFFF : NP   (カーネル予約)                              */
/*    0x300000 - 0x3FFFFF : R/W  (シェル常駐帯域, ガード付き)             */
/*    0x400000 - 0x4FFFFF : R/O+U(共有ライブラリ帯域: .text/.rodata。      */
/*                                .data/.bss はアプリ PD ごとに差し替え)   */
/*    0x500000 - mem_end  : R/W  (プログラム空間, ガードページ付き)         */
/*    mem_end  - 0xFFFFFF : NP   (未実装メモリ)                             */
/*                                                                          */
/*  [16MB 超のデバイス窓帯 (H3b)]                                            */
/*    0x1000000-0x1FFFFFF : NP   (実 RAM 無し。既定は全ページ Not-Present)   */
/*      paging_map_phys() でドライバが窓だけを張る。現在の利用者は Xe10      */
/*      内蔵 Cirrus のリニア窓 0x1000000〜0x11FFFFF (2MB)。                  */
/* ======================================================================== */

#include "paging.h"

#include "io.h"
#include "pc98.h"
#include "memmap.h"
#include "pgalloc.h"

/* カーネルスタック帯のレイアウト不変条件。
 * ガードページはスタック直下に隣接し、スタックは 2MB 境界 (SQLite 帯域)
 * の手前で終わる。ずれると paging_init の R/W 強制やガード設定が
 * 意図しないページに掛かる。 */
STATIC_ASSERT(MEM_STACK_GUARD_END + 1 == MEM_KSTACK_BASE,
              kstack_guard_adjacent);
STATIC_ASSERT(MEM_KSTACK_TOP < 0x200000UL, kstack_below_sqlite_band);
STATIC_ASSERT((MEM_STACK_GUARD & (PAGE_SIZE - 1)) == 0,
              kstack_guard_page_aligned);

/* リング3 アプリ帯 PDE (M1b) はアプリ帯域を覆う PDE と一致し、かつ静的
 * page_tables[] の範囲内でなければならない。ここがずれるとアプリ PD が
 * カーネル帯域を差し替えたり範囲外 PT を読んだりして黙って壊れる。
 * K3 以降、この 1 枚の PDE が「共有ライブラリ帯域 + プログラム帯 +
 * ユーザスタック」を丸ごと覆う。3 つとも同じ PDE の内側であること
 * (別 PDE に出ると、PD ごとの差し替えから外れて共有されてしまう)。 */
STATIC_ASSERT(APP_BAND_PDE == (MEM_APP_BAND_BASE >> 22), app_band_pde_matches);
STATIC_ASSERT(APP_BAND_PDE < PAGING_PT_COUNT, app_band_pde_in_range);
STATIC_ASSERT((MEM_SHLIB_BASE >> 22) == APP_BAND_PDE, shlib_base_in_app_band);
STATIC_ASSERT((MEM_EXEC_LOAD_ADDR >> 22) == APP_BAND_PDE, exec_load_in_app_band);
STATIC_ASSERT(((MEM_APP_BAND_TOP - 1) >> 22) == APP_BAND_PDE, app_band_top_in_pde);
STATIC_ASSERT(MEM_SHLIB_END <= MEM_EXEC_LOAD_ADDR, shlib_band_below_exec);

/* H3b: 「実 RAM の上限」と「ページテーブルの守備範囲」の関係。
 *   - 実 RAM 上限は守備範囲の内側 (でないと恒等マップの穴ができる)。
 *   - どちらも 4MB (= 1 PDE) の倍数。端数があると RAM 上限が PT の途中に
 *     落ち、pgalloc とページテーブルの境界がずれる。
 *   - 守備範囲が実 RAM 上限より広い = 16MB 超にデバイス窓を張る余地がある
 *     (これが H3b の目的そのもの)。 */
STATIC_ASSERT(PAGING_RAM_LIMIT <= PAGING_MAP_SIZE, ram_limit_within_map);
STATIC_ASSERT((PAGING_RAM_LIMIT % (PTE_COUNT * PAGE_SIZE)) == 0,
              ram_limit_pde_aligned);
STATIC_ASSERT((PAGING_MAP_SIZE % (PTE_COUNT * PAGE_SIZE)) == 0,
              map_size_pde_aligned);
/* ホットデプロイ窓は物理 RAM の末尾を削って作るので、必ず実 RAM 上限の
 * 内側に収まる (16MB 超のデバイス窓帯には出てこない)。 */
STATIC_ASSERT(MEM_HOTDEPLOY_SIZE * 2 < PAGING_RAM_LIMIT,
              hotdeploy_window_within_ram);

/* ======== ページテーブル (BSS配置, 4096バイトアライン必須) ======== */
/* Open Watcomでは __declspec(align(4096)) が使えないため、
 * 手動でアライメントを確保する。
 * 実際のテーブルサイズ + 4095バイトのパディングを確保し、
 * 4096境界に切り上げたアドレスを使用する。 */

/* ページテーブルは 1 本の連続バッファから切り出す。かつては 1 枚ごとに
 * 4095 バイトのパディングを持たせていたが (u8[N][4096+4095])、H3b で枚数が
 * 4 → 8 になると捨てるぶんも倍 (32KB) になる。先頭だけ 4096 境界に上げれば
 * 以降の 4KB 刻みは自動的に境界に乗るので、増分は表そのものの +16KB で済む。 */
static u8 pd_raw[4096 + 4095];      /* ページディレクトリ用生バッファ */
static u8 pt_raw[PAGING_PT_COUNT * 4096 + 4095];  /* ページテーブル用生バッファ */

static u32 *page_directory;          /* アライン済みポインタ */
static u32 *page_tables[PAGING_PT_COUNT];

static int pg_enabled = 0;

/* 4096バイト境界に切り上げ */
static u32 *align4096(void *p)
{
    u32 addr = (u32)p;
    addr = (addr + 4095) & ~4095UL;
    return (u32 *)addr;
}

/* TLBフラッシュ (i386互換: CR3リロード方式)
 * invlpgはi486+なので使えない */
static void tlb_flush_all(void)
{
    u32 cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
}

/* ======================================================================== */
/*  paging_init — ページテーブル構築 + ページング有効化                      */
/* ======================================================================== */
void paging_init(u32 mem_kb)
{
    int i, j;
    u32 phys;
    u32 pd_phys;
    u32 max_mem_bytes;
    u32 *pt_base;

    /* プローブされた実メモリ量を「OS32 が RAM として面倒を見る上限」で頭打ちに
     * する (H3b)。従来はページテーブルが 16MB ぶんしか無かったので自然に
     * 16MB 止まりだったが、守備範囲を 32MB に広げた今は明示的に切る必要がある。
     * 16MB 超はデバイス窓のための空き番地であって RAM ではない。
     * mem_kb * 1024 の桁あふれ (mem_kb > 4194303) もこれで防げる。 */
    if (mem_kb > PAGING_RAM_LIMIT / 1024) mem_kb = PAGING_RAM_LIMIT / 1024;
    max_mem_bytes = mem_kb * 1024;

    /* アライン済みポインタを取得 (先頭だけ上げれば以降は 4KB 刻みで乗る) */
    page_directory = align4096(pd_raw);
    pt_base = align4096(pt_raw);
    for (i = 0; i < PAGING_PT_COUNT; i++) {
        page_tables[i] = pt_base + (u32)i * PTE_COUNT;
    }

    /* ページディレクトリ初期化: 全エントリをNot-Presentに */
    for (i = 0; i < PDE_COUNT; i++) {
        page_directory[i] = 0;
    }

    /* ページテーブル構築: 実装されている範囲のみR/W、超えた範囲はNot-Present。
     * 16MB〜32MB を覆う PT (H3b で足した 4 枚) は全ページ Not-Present のまま
     * になるが、**PDE は present で登録しておく** — こうしておけば
     * paging_map_phys() がデバイス窓を張るときに PTE を書くだけで済み、
     * PDE の張り替え (= 他 PD との整合) を考えなくてよい。 */
    for (i = 0; i < PAGING_PT_COUNT; i++) {
        for (j = 0; j < PTE_COUNT; j++) {
            phys = (u32)(i * PTE_COUNT + j) * PAGE_SIZE;
            if (phys < max_mem_bytes || phys < MEM_1MB) {
                /* コンベンショナルメモリ(0-1MB)またはプローブ範囲内 */
                page_tables[i][j] = phys | PAGE_RW;
            } else {
                /* 未実装領域 */
                page_tables[i][j] = PAGE_NOT_PRESENT;
            }
        }
        /* ページディレクトリにテーブルを登録 */
        page_directory[i] = (u32)page_tables[i] | PAGE_RW;
    }

    /* ========================================================
     *  保護属性の設定
     * ======================================================== */

    /* カーネルスタック: R/W を強制する。
     *
     * 上のループはプローブしたメモリ量 (max_mem_bytes) を超える範囲を
     * Not-Present にするので、極端に小さいメモリ量が報告された場合でも
     * **自分が今立っているスタックだけは必ず生かしておく**。
     * ここを NP にした瞬間に次の push で三重フォルトになる。 */
    paging_map_range(MEM_KSTACK_BASE,
                     PAGE_ALIGN_DOWN(MEM_KSTACK_TOP) + PAGE_SIZE,
                     MEM_KSTACK_BASE, PAGE_RW);

    /* スタックガードページ: Not-Present */
    paging_set_not_present(MEM_STACK_GUARD, MEM_STACK_GUARD_END);

    /* カーネル帯域内SHM後方予約: Not-Present (スタックガードの手前まで) */
    paging_set_not_present(MEM_SHM_RESV_START, MEM_SHM_RESV_END);

    /* カーネル予約域 (SQLite帯域後 〜 シェル帯域前): Not-Present */
    paging_set_not_present(MEM_KERNEL_RESV_START, MEM_KERNEL_RESV_END);

    /* シェルスタックガード: Not-Present */
    paging_set_not_present(MEM_SHELL_GUARD, MEM_SHELL_GUARD + PAGE_SIZE - 1);

    /* シェル帯域後方 (MEM_SHELL_HEAP_BASE 0x380000 - 0x3FFFFF) はシェルの
     * exec_heap (KAPI mem_alloc) として present R/W のまま使う。かつては
     * Not-Present の空白帯だったが、シェルの exec_heap を newlib の sbrk
     * (BSS 直後) から分離するためにここへ移した (include/memmap.h 参照)。
     * PTE に USER は立てないので CPL=3 のアプリからは触れない */

    /* SQLite帯域 + 代替スタック (0x200000〜): 強制R/W
     * ブートローダーが sqlite.bin を 0x200000 にロード済み。
     * 代替スタックも含めメモリプローブ結果に関係なく R/W を保証する。 */
    {
        extern u32 __sqlite_start;
        u32 sq_start = PAGE_ALIGN_DOWN((u32)&__sqlite_start);
        u32 sq_end   = PAGE_ALIGN_DOWN(MEM_SQLITE_STACK_TOP + PAGE_SIZE);
        paging_map_range(sq_start, sq_end, sq_start, PAGE_RW);
    }

    /* BIOS ROM: Read-Only */
    paging_set_readonly(MEM_BIOS_ROM_START, MEM_BIOS_ROM_END);

    /* ========================================================
     *  CR3にページディレクトリをセット → CR0.PGビットを立てる
     * ======================================================== */
    pd_phys = (u32)page_directory;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
    {
        u32 cr0_val;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
        cr0_val |= CR0_PG;
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val) : "memory");
    }

    pg_enabled = 1;
}

/* ======================================================================== */
/*  paging_reclaim_conventional — ブート後のコンベンショナルメモリ属性変更    */
/*  ページ0: NOT PRESENT (NULLポインタ検出)                                 */
/*  0x1000-0x9FFFF: R/W (旧R/O/ローダー領域とブート時スタックを解放)         */
/* ======================================================================== */
void paging_reclaim_conventional(void)
{
    /* ページ0: Read-Only (BIOS DATA AREA アクセスを許可しつつ書き込み検出)
     * [DEBUG] NOT PRESENT → R/O に変更: LZ4展開中にBDA参照でクラッシュする
     * 問題を調査中。元は paging_set_not_present(0x0, MEM_NULL_GUARD_END); */
    paging_set_readonly(0x0, MEM_NULL_GUARD_END);

    /* 0x1000-0x9FFFF: R/W (フォント/Unicode/GFX用) */
    paging_map_range(MEM_CONV_RECLAIM_START, MEM_CONV_RECLAIM_END + 1,
                     MEM_CONV_RECLAIM_START, PAGE_RW);
}

/* ======================================================================== */
/*  paging_set_page — 1ページの属性を変更                                   */
/* ======================================================================== */
/* 1 ページ設定の共通部 (TLB フラッシュなし)。
 * paging_set_page と paging_map_range から使う。 */
static int set_page_noflush(u32 virt_addr, u32 phys_addr, u32 flags)
{
    u32 pdi = virt_addr >> 22;
    u32 pti = (virt_addr >> 12) & 0x3FF;

    /* マッピング範囲外。無言 no-op にすると「ガードページを置いたつもり」の
     * まま保護なしで走る (fail-open) ので、必ず失敗を返す。 */
    if (pdi >= PAGING_PT_COUNT) return -1;

    page_tables[pdi][pti] = (phys_addr & 0xFFFFF000UL) | flags;

    /* 実効権限は PDE と PTE の論理積になる (i386 の仕様)。PTE だけ USER に
     * しても、その PDE に USER が無ければユーザ (V86 ゲスト) からは
     * アクセスできず #PF になる。PTE に USER を付けたら PDE にも伝播させる。
     *
     * PDE を USER にしても、同じ PDE 配下の他のページは PTE 側が
     * supervisor のままなので保護は保たれる。カーネル帯を USER に
     * しないこと (docs/tasks/v86v2/02_np21w_paging_analysis.md)。 */
    if (flags & PTE_USER) {
        page_directory[pdi] |= PTE_USER;
    }
    return 0;
}

int paging_set_page(u32 virt_addr, u32 phys_addr, u32 flags)
{
    int rc = set_page_noflush(virt_addr, phys_addr, flags);
    if (rc == 0 && pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_map_range — 範囲マップ (end は exclusive)                        */
/*                                                                          */
/*  [virt_start, virt_end) を phys_start からの連続物理へ flags でマップし、 */
/*  TLB フラッシュを最後に 1 回だけ行う。かつてはページごとに               */
/*  paging_set_page → CR3 全リロードが走り、V86 setup だけで 300 回超の      */
/*  フラッシュが発生していた (R9)。                                          */
/* ======================================================================== */
int paging_map_range(u32 virt_start, u32 virt_end, u32 phys_start, u32 flags)
{
    u32 v;
    u32 off = 0;
    int rc = 0;

    virt_start = PAGE_ALIGN_DOWN(virt_start);
    phys_start = PAGE_ALIGN_DOWN(phys_start);

    for (v = virt_start; v < virt_end; v += PAGE_SIZE, off += PAGE_SIZE) {
        if (set_page_noflush(v, phys_start + off, flags) != 0) {
            rc = -1;
            break;
        }
    }

    if (pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_map_phys — デバイス窓マップ (ページ数指定)                        */
/*                                                                          */
/*  paging_map_range のページ数版。ドライバ (gfx/ など) が「この物理窓を     */
/*  この仮想番地へ npages 分」と書けるようにするための入口で、               */
/*  ページテーブルはカーネル側 (本ファイル) だけが触る、という分担を保つ。   */
/* ======================================================================== */
int paging_map_phys(u32 virt_addr, u32 phys_addr, u32 npages, u32 flags)
{
    if (npages == 0) return 0;
    return paging_map_range(virt_addr, virt_addr + npages * PAGE_SIZE,
                            phys_addr, flags);
}

/* 指定範囲を覆う PDE から USER を落とす。
 * paging_set_page() は USER を立てる方向にしか伝播させないので、
 * V86 セッション終了時にカーネル側が明示的に戻すために使う。 */
int paging_pde_clear_user(u32 start, u32 end)
{
    u32 pdi;
    u32 first = start >> 22;
    u32 last  = end >> 22;

    if (first >= PAGING_PT_COUNT) return -1;

    for (pdi = first; pdi <= last && pdi < PAGING_PT_COUNT; pdi++) {
        page_directory[pdi] &= ~(u32)PTE_USER;
    }

    if (pg_enabled) tlb_flush_all();
    return 0;
}

/* ======================================================================== */
/*  paging_set_readonly — 範囲内の全ページをRead-Onlyに                     */
/* ======================================================================== */
int paging_set_readonly(u32 start, u32 end)
{
    u32 addr;
    int rc = 0;
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_DOWN(end) + PAGE_SIZE;      /* end は inclusive 指定 */

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        u32 pte;
        if (pdi >= PAGING_PT_COUNT) { rc = -1; break; }
        /* 既存 PTE の物理フレームを保持する。identity で上書きすると、
         * V86 リマップ中のページを R/O 化した時にマッピング自体が
         * 壊れてしまう (フラグ変更のつもりが張り替えになる)。 */
        pte = page_tables[pdi][pti];
        if (pte & PTE_PRESENT) {
            page_tables[pdi][pti] = (pte & 0xFFFFF000UL) | PAGE_RO;
        } else {
            page_tables[pdi][pti] = (addr & 0xFFFFF000UL) | PAGE_RO;
        }
    }

    if (pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_set_not_present — 範囲内の全ページをアクセス不可に               */
/* ======================================================================== */
int paging_set_not_present(u32 start, u32 end)
{
    u32 addr;
    int rc = 0;
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_DOWN(end) + PAGE_SIZE;      /* end は inclusive 指定 */

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        u32 pdi = addr >> 22;
        u32 pti = (addr >> 12) & 0x3FF;
        if (pdi >= PAGING_PT_COUNT) { rc = -1; break; }
        /* フレーム/属性は保持して P ビットだけ落とす。全消去 (=0) だと
         * 解除時に identity 以外のマッピングを復元できない。 */
        page_tables[pdi][pti] &= ~(u32)PTE_PRESENT;
    }

    if (pg_enabled) tlb_flush_all();
    return rc;
}

/* ======================================================================== */
/*  paging_enabled — ページングが有効かどうか                               */
/* ======================================================================== */
int paging_enabled(void) { return pg_enabled; }

/* ======================================================================== */
/*  paging_is_present — 指定アドレスのページがPresentかどうか                */
/*                                                                          */
/*  メモリダンプの安全チェック用。ページング無効時は常に1を返す。            */
/* ======================================================================== */
int paging_is_present(u32 virt_addr)
{
    u32 pdi, pti;
    if (!pg_enabled) return 1;
    pdi = virt_addr >> 22;
    if (pdi >= PAGING_PT_COUNT) return 0; /* PAGING_MAP_SIZE 超: 範囲外 */
    pti = (virt_addr >> 12) & 0x3FF;
    return (page_tables[pdi][pti] & PTE_PRESENT) ? 1 : 0;
}

/* ======================================================================== */
/*  リング3 アドレス空間 (M1b: PD 複製)                                     */
/*                                                                          */
/*  カーネル全体は identity マッピング (仮想=物理) なので、pgalloc が返す     */
/*  物理アドレスはそのまま仮想アドレスとして読み書きできる。新 PD / アプリ    */
/*  PT のバッキングは 0x400000 帯 (APP_BAND_PDE の範囲) から取られるが、      */
/*  アプリ PT を master と同一 identity で初期化するため、CR3 を新 PD に      */
/*  載せた後もそれらのページは自分自身を identity で見られる。               */
/* ======================================================================== */

u32 paging_kernel_pd_phys(void)
{
    /* identity マッピングなので page_directory の仮想アドレス = 物理。 */
    return (u32)page_directory;
}

u32 paging_current_cr3(void)
{
    u32 cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    return cr3_val;
}

void paging_load_cr3(u32 pd_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

int paging_addrspace_create(struct addrspace *as)
{
    u32 pd_phys;
    u32 pt_phys;
    u32 *new_pd;
    u32 *app_pt;
    int i;

    if (!as) return -1;
    as->pd_phys = 0;
    as->app_pt_phys = 0;
    as->app_pde = 0;

    /* PD 用に 1 ページ、0x400000 帯アプリ PT 用に 1 ページ確保する。
     * どちらも 0x400000 帯 (identity) から取られるので、そのまま
     * 物理=仮想で書き込める。 */
    pd_phys = pgalloc_alloc_page();
    if (!pd_phys) return -1;
    pt_phys = pgalloc_alloc_page();
    if (!pt_phys) {
        pgalloc_free_page(pd_phys);
        return -1;
    }

    new_pd = (u32 *)pd_phys;
    app_pt = (u32 *)pt_phys;

    /* 全 PDE を master からコピー = カーネル帯域・SHM・VRAM・ホットデプロイ窓
     * を含む全域を共有する。共有 PDE は master と同じ PT (同一物理) を指す。
     * ループは PDE_COUNT (1024 本) なので、H3b で守備範囲が 32MB に広がって
     * 増えた PDE 4〜7 (16MB 超のデバイス窓帯) も自動的に写る。master に
     * paging_map_phys() で張った Cirrus のリニア窓は、以後に作られる
     * CPL=3 アプリの PD からも同じ物理を指す。
     * ただしデバイス窓は master では **supervisor + PCD** で張られている
     * (レビュー #5 ②)。CPL=3 に見せてよいのはクライアント面だけなので、
     * 昇格は exec が paging_addrspace_map_user_keep() で 1 枚ずつ行う。 */
    for (i = 0; i < PDE_COUNT; i++) {
        new_pd[i] = page_directory[i];
    }

    /* 0x400000 帯アプリ PT を master の同帯 PT と同一の identity で初期化。
     * これで CPL=0 のまま CR3 を新 PD に載せてもカーネルから見た 0x400000 帯
     * は変わらない (V1)。CPL=3 用の USER overlay は M1c で行う。 */
    for (i = 0; i < PTE_COUNT; i++) {
        app_pt[i] = page_tables[APP_BAND_PDE][i];
    }

    /* 新 PD の 0x400000 帯 PDE だけアプリ PT に差し替える (PRESENT|RW)。
     * USER はまだ立てない — M1c で USER ページを張った時に伝播させる。 */
    new_pd[APP_BAND_PDE] = (pt_phys & 0xFFFFF000UL) | PAGE_RW;

    as->pd_phys = pd_phys;
    as->app_pt_phys = pt_phys;
    as->app_pde = APP_BAND_PDE;
    return 0;
}

void paging_addrspace_destroy(struct addrspace *as)
{
    if (!as || !as->pd_phys) return;
    /* アクティブな PD を破棄してはならない (呼び出し側が master へ戻す責任)。
     * ここでは確認だけして、万一アクティブでも解放は続行しない。 */
    if (paging_current_cr3() == as->pd_phys) {
        return;
    }
    if (as->app_pt_phys) pgalloc_free_page(as->app_pt_phys);
    pgalloc_free_page(as->pd_phys);
    as->pd_phys = 0;
    as->app_pt_phys = 0;
    as->app_pde = 0;
}

/* 1 ページ分の実体。keep_cache=1 なら既存 PTE のキャッシュ属性 (PCD/PWT) を
 * 引き継ぐ。デバイス窓の中を USER へ昇格させるとき、flags に PCD を書き忘れる
 * と「CPU が書いた画素がキャッシュに残り、BLT エンジンが古い VRAM を読む」に
 * なる (Cirrus のクライアント面)。呼び出し側に属性を復唱させるのではなく、
 * 張ってあるものを保つ方が壊れにくい (レビュー #5 ③)。 */
static int addrspace_map_user_page(struct addrspace *as, u32 virt, u32 phys,
                                   u32 flags, int keep_cache)
{
    u32 pdi = virt >> 22;
    u32 pti = (virt >> 12) & 0x3FF;
    u32 *pd;
    u32 *pt;

    if (!as || !as->pd_phys) return -1;
    pd = (u32 *)as->pd_phys;

    if (pdi == as->app_pde) {
        /* アプリ固有 PT (このアプリの PD からしか見えない) */
        pt = (u32 *)as->app_pt_phys;
    } else {
        /* 共有 PT (master と同一)。VRAM/SHM 等 C2 で共有 + USER の領域用。 */
        if (pdi >= PAGING_PT_COUNT) return -1;
        pt = page_tables[pdi];
    }

    if (keep_cache) flags |= (pt[pti] & (u32)(PTE_PCD | PTE_PWT));
    pt[pti] = (phys & 0xFFFFF000UL) | flags;

    /* このアプリ PD の PDE にだけ USER を伝播 (master の PDE は触らない)。 */
    if (flags & PTE_USER) {
        pd[pdi] |= PTE_USER;
    }
    return 0;
}

int paging_addrspace_map_user(struct addrspace *as, u32 virt, u32 phys,
                              u32 flags)
{
    return addrspace_map_user_page(as, virt, phys, flags, 0);
}

/* 範囲版の共通部。末尾の TLB 処理まで含めて 1 か所にまとめる。 */
static int addrspace_map_user_range(struct addrspace *as, u32 vstart,
                                    u32 vend, u32 flags, int keep_cache)
{
    u32 v;
    int rc = 0;

    vstart = PAGE_ALIGN_DOWN(vstart);
    for (v = vstart; v < vend; v += PAGE_SIZE) {
        if (addrspace_map_user_page(as, v, v, flags, keep_cache) != 0) {
            rc = -1;
            break;
        }
    }
    /* この PD が既にアクティブなら TLB を捨てる。通常は CR3 に載せる前に
     * 呼ぶので不要だが、載せた後の追加マップにも備える。 */
    if (as && as->pd_phys && paging_current_cr3() == as->pd_phys) {
        paging_load_cr3(as->pd_phys);
    }
    return rc;
}

int paging_addrspace_map_user_range(struct addrspace *as, u32 vstart,
                                    u32 vend, u32 flags)
{
    return addrspace_map_user_range(as, vstart, vend, flags, 0);
}

int paging_addrspace_map_user_keep(struct addrspace *as, u32 vstart,
                                   u32 vend, u32 flags)
{
    return addrspace_map_user_range(as, vstart, vend, flags, 1);
}

/* V1 自己診断用のプローブ。カーネル .bss (0x100000-0x1FFFFF, PDE 0) に置かれ、
 * 全 PD で共有される領域。SHM 等の実データを触らずに共有を検証できる。 */
static volatile u32 pd_selftest_probe;

int paging_pd_clone_selftest(void)
{
    struct addrspace as;
    u32 saved_cr3;
    u32 seen;
    int rc = 0;

    if (!pg_enabled) return 0; /* ページング無効なら検証対象外 */

    if (paging_addrspace_create(&as) != 0) return 1;

    saved_cr3 = paging_current_cr3();

    /* master 経由で既知値を書く。 */
    pd_selftest_probe = 0x12345678UL;

    /* CR3 を新 PD に載せる。この行以降が実行できている時点で、カーネルの
     * コード (PDE 0) とスタック (PDE 0) が新 PD でも共有されている証拠。 */
    paging_load_cr3(as.pd_phys);

    /* 新 PD からカーネル帯域の同じ番地を読む — master が書いた値が見えるなら
     * 同一物理を指している (共有 OK)。 */
    seen = pd_selftest_probe;

    /* 新 PD 側から書き換える。 */
    pd_selftest_probe = 0xA5A5F00DUL;

    /* master に戻す。 */
    paging_load_cr3(saved_cr3);

    if (seen != 0x12345678UL) rc |= 2;                 /* 新 PD から共有が見えない */
    if (pd_selftest_probe != 0xA5A5F00DUL) rc |= 4;    /* 新 PD の書込が master に反映されない */

    paging_addrspace_destroy(&as);
    return rc;
}

/* ======================================================================== */
/*  paging_map_user_keep_selftest — デバイス窓を CPL=3 へ貸すときの不変条件   */
/*                                                                          */
/*  レビュー #5 ②③ の回帰止め。Cirrus のクライアント面のように「共有 PT の   */
/*  中の 1 枚だけを USER にする」操作で、次の 4 つが同時に成り立つこと:      */
/*    1. 貸したページに USER が立つ                                          */
/*    2. そのページの PCD (キャッシュ無効) が消えない                        */
/*       — 消えると CPU が書いた画素がキャッシュに残り、BLT エンジンが       */
/*         古い VRAM を読む                                                  */
/*    3. 同じ PT の隣のページ (= 表示面に相当) は supervisor のまま無傷       */
/*    4. master の PDE には USER が伝播しない (アプリ PD 側にだけ立つ)        */
/*                                                                          */
/*  ハードウェアには一切依存しない: 使うのは守備範囲 (32MB) の末尾 2 ページ   */
/*  で、実 RAM も無く既定 Not-Present、どのドライバの窓とも重ならない         */
/*  (Cirrus の窓は 01000000h〜011FFFFFh)。PTE は試験前の値へ戻す。            */
/* ======================================================================== */
int paging_map_user_keep_selftest(void)
{
    struct addrspace as;
    u32 vclient = PAGING_MAP_SIZE - PAGE_SIZE;        /* 貸す側 (クライアント面役) */
    u32 vvisible = PAGING_MAP_SIZE - 2 * PAGE_SIZE;   /* 貸さない側 (表示面役) */
    u32 pdi = vclient >> 22;
    u32 pti_c = (vclient >> 12) & 0x3FF;
    u32 pti_v = (vvisible >> 12) & 0x3FF;
    u32 saved_c, saved_v, saved_pde;
    u32 pte_c, pte_v;
    u32 *app_pd;
    int rc = 0;

    if (!pg_enabled) return 0;              /* ページング無効なら検証対象外 */
    if (pdi >= PAGING_PT_COUNT) return 1;   /* 定数がずれた (起こらないはず) */

    saved_c = page_tables[pdi][pti_c];
    saved_v = page_tables[pdi][pti_v];
    saved_pde = page_directory[pdi];

    /* バックエンド init を模す: どちらも supervisor + PCD のデバイス窓。 */
    page_tables[pdi][pti_c] = (vclient & 0xFFFFF000UL) | PAGE_RW | PTE_PCD;
    page_tables[pdi][pti_v] = (vvisible & 0xFFFFF000UL) | PAGE_RW | PTE_PCD;

    if (paging_addrspace_create(&as) != 0) {
        page_tables[pdi][pti_c] = saved_c;
        page_tables[pdi][pti_v] = saved_v;
        page_directory[pdi] = saved_pde;
        tlb_flush_all();
        return 2;
    }
    app_pd = (u32 *)as.pd_phys;

    /* exec が bb 範囲に対して行う操作そのもの。 */
    if (paging_addrspace_map_user_keep(&as, vclient, vclient + PAGE_SIZE,
                                       PAGE_RW | PTE_USER) != 0) rc |= 4;

    pte_c = page_tables[pdi][pti_c];
    pte_v = page_tables[pdi][pti_v];

    if (!(pte_c & PTE_USER)) rc |= 8;                       /* 1 */
    if (!(pte_c & PTE_PCD))  rc |= 16;                      /* 2 */
    if (pte_v & PTE_USER)    rc |= 32;                      /* 3 */
    if (!(pte_v & PTE_PCD))  rc |= 64;                      /* 3 */
    if (page_directory[pdi] & PTE_USER) rc |= 128;          /* 4 (master) */
    if (!(app_pd[pdi] & PTE_USER))      rc |= 256;          /* 4 (アプリ PD) */

    paging_addrspace_destroy(&as);

    page_tables[pdi][pti_c] = saved_c;
    page_tables[pdi][pti_v] = saved_v;
    page_directory[pdi] = saved_pde;
    tlb_flush_all();
    return rc;
}

