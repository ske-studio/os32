/* ======================================================================== */
/*  V86_MEM.C — V86 メモリ空間構築 (Phase 1)                                */
/*                                                                          */
/*  V86タスク用のページテーブル設定、IVT・BDA初期構築、                      */
/*  I/Oビットマップ設定を行う。                                             */
/*                                                                          */
/*  メモリレイアウト (implementation_plan.md §3):                           */
/*    仮想 0x00000-0x9FFFF  → 物理 0x300000-0x39FFFF  (バッキングRAM)      */
/*    仮想 0xA0000-0xA3FFF  → 物理 0xA0000  (TVRAM)                        */
/*    仮想 0xA4000-0xA7FFF  → 物理 0xA4000  (CGウィンドウ, R/O)            */
/*    仮想 0xA8000-0xBFFFF  → 物理 0xA8000  (GVRAM Plane0-2)              */
/*    仮想 0xC0000-0xDFFFF  → NOT PRESENT                                  */
/*    仮想 0xE0000-0xE7FFF  → 物理 0xE0000  (GVRAM Plane3)                */
/*    仮想 0xE8000-0xEFFFF  → NOT PRESENT                                  */
/*    仮想 0xF0000-0xFFFFF  → 物理 0xF0000  (BIOS ROM, R/O)               */
/*                                                                          */
/*  注意: シェル帯域 (0x300000-0x3FFFFF) をバッキングRAMとして転用する。     */
/*  V86モード中はシェルは待機中のため、シェルのメモリ内容は                  */
/*  V86開始前に退避し、V86終了後に復元する必要がある。                       */
/*  → Phase 1 では v86_test から呼ばれるため、シェルは未使用。              */
/*    Phase 2 以降でシェルからの呼び出しに対応する。                        */
/* ======================================================================== */

#include "v86_mem.h"
#include "paging.h"
#include "tss.h"
#include "kstring.h"
#include "memmap.h"
#include "io.h"

/* v86_mem.h で定義済みの定数を使用:
 *   V86_BACKING_PHYS  0x300000UL
 *   V86_BACKING_SIZE  0x0A0000UL
 *   V86_REMAP_END     0x08F000UL
 */

/* バッキングRAM有効フラグ (デフォルト=0: アイデンティティマッピング) */
static int v86_backing_enabled = 0;

/* ======================================================================== */
/*  BDA (BIOS Data Area) 初期値                                             */
/*                                                                          */
/*  出典: UNDOCUMENTED memsys.md                                            */
/*  FreeDOS(98) 起動に最低限必要なシステム共通域の値。                      */
/* ======================================================================== */
struct bda_entry {
    u16 offset;
    u8  value;
};

static const struct bda_entry bda_defaults[] = {
    { 0x0400, 0x00 },   /* BIOS_FLAG2: 機種フラグ */
    { 0x0401, 0x00 },   /* EXPMMSZ: 拡張メモリサイズ (未使用) */
    { 0x0480, 0x00 },   /* CPU_FLAG: bit3=V33A=0 */
    { 0x0484, 0x03 },   /* CPU_TYPE: i386以上 */
    { 0x0495, 0x00 },   /* GRAPH_CHG: GRCG OFF */
    { 0x0496, 0x00 },   /* GRAPH_TAL[0]: タイルレジスタ0 */
    { 0x0497, 0x00 },   /* GRAPH_TAL[1]: タイルレジスタ1 */
    { 0x0498, 0x00 },   /* GRAPH_TAL[2]: タイルレジスタ2 */
    { 0x0499, 0x00 },   /* GRAPH_TAL[3]: タイルレジスタ3 */
    { 0x0501, 0x00 },   /* BIOS_FLAG5: bit7=10MHz系, bit2-0=RAM 640KB */
};

/* IVTダミーハンドラ (IRET命令のみ) — バッキングRAM内に配置 */
/* 配置先: 0x500 (BDA直後、IPL/DOSフリーエリア (0x600) の手前) 
 * 注意: 以前は0x600に配置していたが、IPLが0x0060:0x0000 (リニア0x600) に
 * ロードされるためIPLコードで上書きされてしまっていた。 */
#define IVT_HANDLER_BASE   0x0500

/* ======================================================================== */
/*  v86_mem_setup — V86メモリ空間を構築                                     */
/*                                                                          */
/*  1. バッキングRAMをゼロクリア                                            */
/*  2. IVT (割り込みベクタテーブル) 構築                                    */
/*  3. BDA (BIOSデータエリア) 初期値設定                                    */
/*  4. ページテーブル設定                                                   */
/*  5. I/Oビットマップ設定                                                  */
/* ======================================================================== */
void v86_mem_setup(void)
{
    u32 virt, phys, addr;
    int i;
    u8 *backing;
    u32 *ivt;
    u16 handler_seg, handler_off;

    /* バッキングRAM有効フラグをセット */
    v86_backing_enabled = 1;

    /* ================================================================== */
    /*  1. バッキングRAMをゼロクリア (640KB)                               */
    /*  ※ シェル帯域 (0x300000-0x3FFFFF) にはガードページ(NOT PRESENT)が  */
    /*  含まれるため、ゼロクリア前に全ページをPRESENT+RWに変更する。       */
    /* ================================================================== */
    {
        u32 pa;
        for (pa = V86_BACKING_PHYS; pa < V86_BACKING_PHYS + V86_BACKING_SIZE; pa += PAGE_SIZE) {
            paging_set_page(pa, pa, PAGE_RW);
        }
    }
    backing = (u8 *)V86_BACKING_PHYS;
    kmemset(backing, 0, V86_BACKING_SIZE);



    /* ================================================================== */
    /*  2. IVT構築                                                        */
    /*                                                                      */
    /*  0x0000:0x0000 - 0x0000:0x03FF に 256個のベクタ。                   */
    /*  全てのベクタを 0x0060:0x0000 のダミーIRETハンドラに向ける。        */
    /*  ダミーハンドラは 0x600 にCF命令(IRET)1バイトを配置。               */
    /*  PC-98 リアルモードのIVTフォーマット: [offset:16, segment:16]       */
    /* ================================================================== */

    /* ダミーIRETハンドラを配置 (物理 0x300600 = 仮想 0x600) */
    backing[IVT_HANDLER_BASE] = 0xCF;  /* IRET */

    /* IVT: バッキングRAMの先頭 (物理 0x300000 = 仮想 0x0000) */
    ivt = (u32 *)backing;
    handler_seg = (IVT_HANDLER_BASE >> 4);
    handler_off = (IVT_HANDLER_BASE & 0x0F);
    for (i = 0; i < 256; i++) {
        /* seg:off 形式で格納 (リトルエンディアン: [off_lo, off_hi, seg_lo, seg_hi]) */
        ivt[i] = ((u32)handler_seg << 16) | handler_off;
    }

    /* ================================================================== */
    /*  3. BDA初期値設定                                                   */
    /* ================================================================== */
    for (i = 0; i < (int)(sizeof(bda_defaults) / sizeof(bda_defaults[0])); i++) {
        backing[bda_defaults[i].offset] = bda_defaults[i].value;
    }

    /* メモリサイズ: 640KB (0000:0413h = WORD, 単位KB)
     * PC-98の公式BDAにはこのフィールドは存在しない (PC/ATの慣習)。
     * PC-98の正式なメモリサイズは BDA 0501h bit2-0 で管理される。
     * ただし FreeDOS(98) が INT 12h 経由で参照するため設定する。 */
    backing[0x0413] = (640) & 0xFF;
    backing[0x0414] = (640 >> 8) & 0xFF;

    /* DISK_EQUIP (0000:055C-055Dh): ディスク接続状態 */
    /* 055Ch bit 0 = 1MB FDD UNIT#0 接続 */
    backing[0x055C] = 0x01;
    backing[0x055D] = 0x00;

    /* キーボードバッファ初期化 (NP21/W bios09.c 準拠)
     * 0x0524 = HEAD (WORD) = 0x0502
     * 0x0526 = TAIL (WORD) = 0x0502
     * 0x0528 = COUNT (BYTE) = 0 */
    backing[0x0524] = 0x02; backing[0x0525] = 0x05;
    backing[0x0526] = 0x02; backing[0x0527] = 0x05;
    backing[0x0528] = 0x00;

    /* ブートデバイス情報 (0000:0584h): CPU_TYPE/DA/UA */
    /* FreeDOS IPL は BDA[0x584] をINT 1BhのALレジスタ (DA/UA) として使用する */
    /* 0x90 = 1MB FDD UNIT#0 (DA=0x90, UA=0x00) */
    backing[0x0584] = 0x90;

    /* BIOS_FLAG (0000:0501h):
     *   bit 7   = 0 (5/10MHzクロック)
     *   bit 6   = 0 (i386系CPU)
     *   bit 5   = 1 (PC-9801無印以外)
     *   bit 4   = 0 (その他)
     *   bit 3   = 0 (ノーマルモード)
     *   bit 2-0 = 100b (640KB) */
    backing[0x0501] = 0x24;

    /* CRT_STS_FLAG (0000:053Ch):
     *   bit 4 = 1 (16色モード)
     *   bit 1 = 1 (GRCG搭載)
     *   bit 0 = 0 (CRT接続あり)
     * FreeDOS(98) int29dc.c がこのフラグを参照して画面出力処理を分岐する */
    backing[0x053C] = 0x12;

    /* BIOS_FLAG5 (0000:0458h):
     *   bit 7 = 0 (非NESAアーキテクチャ)
     *   bit 0 = 0 (WAIT機能なし)
     * FreeDOS(98) init_oem が参照 */
    backing[0x0458] = 0x00;

    /* SCSI HD接続状態 (0000:0482h):
     *   0x00 = SCSI HDDなし
     * FreeDOS(98) dsk_init が参照 */
    backing[0x0482] = 0x00;

    /* SASI/IDE HDD接続情報 (0000:055Dh):
     *   0x00 = SASI/IDE HDDなし
     * FreeDOS(98) dsk_init が参照 */
    backing[0x055D] = 0x00;

    /* ブートパーティション スクラッチパッド (0000:03FEh):
     *   FreeDOS(98) dsk_init が参照 */
    backing[0x03FE] = 0x00;
    backing[0x03FF] = 0x00;

    /* SCSI パラメータテーブル (0000:0460-047Fh):
     *   ゼロクリア済み (バッキングRAM全体がゼロクリア)
     *   FreeDOS(98) int29dc.c がタイマ関連として参照する場合あり */

    /* コンベンショナルメモリサイズ (0000:05AEh):
     *   0xA0 = 640KB (0xA0 * 4 = 640)
     *   PC-98の正式なメモリサイズフィールド */
    backing[0x05AE] = 0xA0;

    /* TVRAM メモリスイッチ (0xA000:3FE2-3FF7) の初期化
     * FreeDOS(98) init_crt が 0xA000:3FE2-3FF7 を読み取り、
     * セグメント 0x0060 の作業領域にコピーする。
     * 値が全て0だとコンソール出力 (_int29_main) が異常動作する。
     *
     * MEMSW1 (3FE2): bit6=25行, bit3=RS232C割り込み
     * MEMSW2 (3FE4): 各種設定
     * MEMSW3 (3FE6): bit7=ディップスイッチ=ON (拡張メモリ), bit2-0=CRT周波数
     * MEMSW4 (3FE8): 予約
     * MEMSW5 (3FEA): 予約
     * MEMSW6 (3FEC): 予約
     *
     * 注意: PC-98のTVRAMメモリスイッチは WORD単位 (偶数アドレスに1バイト) */
    {
        volatile u16 *tvram16 = (volatile u16 *)0xA0000UL;
        /* MEMSW1: 0x48 = bit6(25行モード) | bit3(RS232C) */
        tvram16[0x3FE2 / 2] = 0x48;
        /* MEMSW2: 0x01 = 10MHzクロック系 */
        tvram16[0x3FE4 / 2] = 0x01;
        /* MEMSW3: 0x04 = bit2(31kHz CRT) */
        tvram16[0x3FE6 / 2] = 0x04;
        /* MEMSW4-6: 0 */
        tvram16[0x3FE8 / 2] = 0x00;
        tvram16[0x3FEA / 2] = 0x00;
        tvram16[0x3FEC / 2] = 0x00;
    }

    /* ================================================================== */
    /*  4. ページテーブル設定                                              */
    /*                                                                      */
    /*  x86 ページングは PDE と PTE の両方で U/S ビットをチェックする。     */
    /*  V86モード (CPL=3) では PTE_USER が必須。                           */
    /* ================================================================== */

    /* 仮想 0x00000-0x8EFFF → 物理 0x300000-0x38EFFF (バッキングRAM, R/W) */
    for (virt = 0x00000; virt < V86_REMAP_END; virt += PAGE_SIZE) {
        phys = V86_BACKING_PHYS + virt;
        paging_set_page(virt, phys, PTE_PRESENT | PTE_RW | PTE_USER);
    }

    /* 仮想 0x8F000-0x9FFFF → 物理 0x38F000-0x39FFFF (バッキングRAM) にリマップ */
    /* カーネルスタック (0x90000-0x9FFFF) をV86タスクから分離する。             */
    /*                                                                          */
    /* 手順:                                                                    */
    /*   1. 物理 0x8F000-0x9FFFF の内容を物理 0x38F000-0x39FFFF にコピー       */
    /*      (この時点では両方アイデンティティマッピング済み)                    */
    /*   2. 仮想 0x8F000-0x9FFFF を物理 0x38F000-0x39FFFF にリマップ           */
    /*      → カーネル (Ring 0) はコピーされたスタックデータを継続使用         */
    /*      → V86 (Ring 3) は物理スタックではなくバッキングRAMにアクセス       */
    /*                                                                          */
    /* teardown時はリマップを維持しPTE_USERのみ除去する (方法C)。               */
    /* 物理 0x38F000-0x39FFFF はシェル帯域の未使用領域のため安全。              */
    /*                                                                          */
    /* 割り込み安全性: コピーとリマップの間にスタック変更が入らないようCLI保護。 */
    {
        u32 eflags;
        __asm__ volatile("pushfl; popl %0" : "=r"(eflags));
        __asm__ volatile("cli");

        /* コピー元 0x8F000 はスタックガードページ (NOT PRESENT) のため、
         * コピー前にアイデンティティマッピングで一時的にPRESENTにする */
        for (addr = 0x8F000; addr < 0xA0000; addr += PAGE_SIZE) {
            paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
        }

        /* スタック内容をバッキングRAMにコピー (68KB: 0x11000) */
        kmemcpy((u8 *)(V86_BACKING_PHYS + 0x8F000UL),
                (u8 *)0x8F000UL,
                0x11000UL);

        /* リマップ: 仮想 0x8F000-0x9FFFF → 物理 0x38F000-0x39FFFF */
        for (addr = 0x8F000; addr < 0xA0000; addr += PAGE_SIZE) {
            paging_set_page(addr, V86_BACKING_PHYS + addr,
                            PTE_PRESENT | PTE_RW | PTE_USER);
        }

        /* 割り込み復元 */
        if (eflags & 0x200) {
            __asm__ volatile("sti");
        }
    }

    /* 仮想 0xA0000-0xA3FFF → 物理 0xA0000 (TVRAM, R/W) */
    for (addr = 0xA0000; addr < 0xA4000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW | PTE_USER);
    }

    /* 仮想 0xA4000-0xA7FFF → 物理 0xA4000 (CGウィンドウ, R/O) */
    for (addr = 0xA4000; addr < 0xA8000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_USER);  /* R/O */
    }

    /* 仮想 0xA8000-0xBFFFF → 物理 0xA8000 (GVRAM Plane0-2, R/W) */
    for (addr = 0xA8000; addr < 0xC0000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW | PTE_USER);
    }

    /* 仮想 0xC0000-0xDFFFF → 物理 0xC0000 (拡張ROM BIOS, R/O) */
    for (addr = 0xC0000; addr < 0xE0000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_USER);  /* R/O */
    }

    /* 仮想 0xE0000-0xE7FFF → 物理 0xE0000 (GVRAM Plane3, R/W) */
    for (addr = 0xE0000; addr < 0xE8000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW | PTE_USER);
    }

    /* 仮想 0xE8000-0xEFFFF → 物理 0xE8000 (拡張ROM/バンクメモリ, R/O) */
    for (addr = 0xE8000; addr < 0xF0000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_USER);  /* R/O */
    }

    /* 仮想 0xF0000-0xFFFFF → 物理 0xF0000 (BIOS ROM, R/O) */
    for (addr = 0xF0000; addr <= 0xFF000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_USER);  /* R/O */
    }

    /* PDE[0] (0x00000-0x3FFFFF) に PTE_USER を設定 */
    paging_pde_set_flags(0x00000, PTE_USER);

    /* ================================================================== */
    /*  5. I/Oビットマップ設定                                            */
    /*                                                                      */
    /*  デフォルトは全トラップ (tss_init で設定済み)。                      */
    /*  安全なポートのみパススルー許可。                                    */
    /*  PIC (00h,02h,08h,0Ah) / PIT (71h,73h,75h,77h) /                   */
    /*  FDC (BE/CC/CA) は仮想化のためトラップのまま。                      */
    /* ================================================================== */

    /* キーボード 8251 */
    tss_iomap_allow(0x41);
    tss_iomap_allow(0x43);

    /* テキストGDC + モードFF1 (60h-6Ah 偶数) */
    tss_iomap_allow(0x60);
    tss_iomap_allow(0x62);
    tss_iomap_allow(0x64);
    tss_iomap_allow(0x66);
    tss_iomap_allow(0x68);
    tss_iomap_allow(0x6A);

    /* CRTC (70h-7Ah 偶数) — 注意: PIT (71h,73h,75h,77h) はトラップ維持 */
    tss_iomap_allow(0x70);
    tss_iomap_allow(0x72);
    tss_iomap_allow(0x74);
    tss_iomap_allow(0x76);
    tss_iomap_allow(0x78);
    tss_iomap_allow(0x7A);

    /* GRCG (7Ch, 7Eh) */
    tss_iomap_allow(0x7C);
    tss_iomap_allow(0x7E);

    /* グラフィックGDC + パレット (A0h-AEh 偶数) */
    tss_iomap_allow(0xA0);
    tss_iomap_allow(0xA1);
    tss_iomap_allow(0xA2);
    tss_iomap_allow(0xA3);
    tss_iomap_allow(0xA4);
    tss_iomap_allow(0xA5);
    tss_iomap_allow(0xA6);
    tss_iomap_allow(0xA8);
    tss_iomap_allow(0xA9);
    tss_iomap_allow(0xAA);
    tss_iomap_allow(0xAC);
    tss_iomap_allow(0xAE);

    /* EGC (04A0h-04AEh 偶数) */
    tss_iomap_allow_range(0x04A0, 0x04AE);

    /* FM音源 (188h-18Eh 偶数) */
    tss_iomap_allow(0x0188);
    tss_iomap_allow(0x018A);
    tss_iomap_allow(0x018C);
    tss_iomap_allow(0x018E);

    /* カレンダBIOS用ポート (20h) */
    tss_iomap_allow(0x20);

    /* シリアルポート (30h-35h) はOS32が使用するためトラップのまま */
}

/* ======================================================================== */
/*  v86_mem_teardown — V86メモリ空間を解放・復元                            */
/*                                                                          */
/*  V86終了時に呼ばれる。ページテーブルとI/Oビットマップを元に戻す。        */
/* ======================================================================== */
void v86_mem_teardown(void)
{
    u32 addr;

    /* ページテーブル復元: 仮想 0x00000-0x8EFFF を元のマッピングに戻す */
    /* (カーネルページングの初期状態: アイデンティティマッピング) */
    for (addr = 0x00000; addr < V86_REMAP_END; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
    }

    /* 仮想 0x8F000-0x9FFFF: リマップは維持、PTE_USER のみ除去 (方法C)      */
    /* 物理 0x38F000-0x39FFFF にコピーしたスタックデータをそのまま使い続ける。*/
    /* アイデンティティマッピングには戻さない (逆コピー不可のため)。         */
    for (addr = 0x8F000; addr < 0xA0000; addr += PAGE_SIZE) {
        paging_set_page(addr, V86_BACKING_PHYS + addr, PTE_PRESENT | PTE_RW);
    }

    /* CGウィンドウをR/Wに戻す */
    for (addr = 0xA4000; addr < 0xA8000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
    }

    /* 0xC0000-0xDFFFF を元に戻す */
    for (addr = 0xC0000; addr < 0xE0000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
    }

    /* 0xE8000-0xEFFFF を元に戻す */
    for (addr = 0xE8000; addr < 0xF0000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
    }

    /* BIOS ROMを元に戻す */
    for (addr = 0xF0000; addr <= 0xFF000; addr += PAGE_SIZE) {
        paging_set_page(addr, addr, PTE_PRESENT);
    }

    /* PDE[0] から PTE_USER を除去 */
    paging_pde_clear_flags(0x00000, PTE_USER);

    /* I/Oビットマップを全トラップに戻す */
    tss_iomap_deny_all();

    /* バッキングRAM無効化 */
    v86_backing_enabled = 0;

    /* NULL保護ページを復元 */
    paging_set_page(0x00000, 0, PAGE_NOT_PRESENT);

    /* シェル帯域ガードページを復元                                        */
    /* ※ 0x38F000-0x39FFFF はカーネルスタックのリマップ先として使用中の     */
    /*   ためNOT PRESENTにしない。0x380000-0x38EFFF と 0x3A0000 以降のみ。 */
    paging_set_page(MEM_SHELL_GUARD, 0, PAGE_NOT_PRESENT);
    for (addr = 0x380000UL; addr < 0x38F000UL; addr += PAGE_SIZE) {
        paging_set_page(addr, 0, PAGE_NOT_PRESENT);
    }
    /* 0x38F000-0x39FFFF はスキップ (カーネルスタック用) */
    for (addr = 0x3A0000UL; addr <= MEM_SHELL_BAND_END; addr += PAGE_SIZE) {
        paging_set_page(addr, 0, PAGE_NOT_PRESENT);
    }
}

/* ======================================================================== */
/*  v86_phys_addr — V86仮想アドレスからカーネル用リニアアドレスに変換       */
/*                                                                          */
/*  V86の seg:off (リニア = seg<<4 + off) を、カーネル (Ring0) から          */
/*  アクセスできる物理アドレスに変換する。                                  */
/*                                                                          */
/*  0x00000-0x9FFFF → 物理 0x300000 + offset  (バッキングRAM)             */
/*  0xA0000-0xFFFFF → 物理 = 仮想  (実機ハードウェア)                     */
/*                                                                          */
/*  ※ 方法Cリマップ対応: 0x8F000-0x9FFFF もバッキングRAM (0x38F000+) に   */
/*    リマップされているため、0xA0000 未満を全てバッキングRAM経由にする。   */
/* ======================================================================== */
u8 *v86_phys_addr(u32 seg, u32 off)
{
    u32 linear = (seg << 4) + off;
    linear &= 0xFFFFF;  /* 1MB境界でラップ */

    /* バッキングRAMが有効で、コンベンショナルメモリ (0-9FFFF) はバッキングRAM */
    if (v86_backing_enabled && linear < 0xA0000UL) {
        return (u8 *)(V86_BACKING_PHYS + linear);
    }
    return (u8 *)linear;
}

/* ======================================================================== */
/*  v86_restore_screen — DOS→OS32復帰時の画面リストア                      */
/*                                                                          */
/*  V86モード終了後、DOSが変更した可能性のあるハードウェア状態を             */
/*  OS32のデフォルトに復帰する。                                            */
/*                                                                          */
/*  1. GRCG OFF                                                             */
/*  2. EGC → GRCG互換モード復帰                                           */
/*  3. 16色モード設定                                                       */
/*  4. デフォルトパレット復帰                                               */
/*  5. テキストGDC: 80桁×25行モード                                        */
/*  6. テキスト画面表示開始                                                 */
/* ======================================================================== */

/* デフォルト16色パレット (G,R,B 各4bit) */
static const u8 default_palette[16][3] = {
    { 0,  0,  0}, /* 0: 黒 */
    { 0,  0,  7}, /* 1: 青 */
    { 0,  7,  0}, /* 2: 赤 */
    { 0,  7,  7}, /* 3: マゼンタ */
    { 7,  0,  0}, /* 4: 緑 */
    { 7,  0,  7}, /* 5: シアン */
    { 7,  7,  0}, /* 6: 黄 */
    { 7,  7,  7}, /* 7: 白 */
    { 4,  4,  4}, /* 8: 暗灰 */
    { 0,  0, 15}, /* 9: 明青 */
    { 0, 15,  0}, /*10: 明赤 */
    { 0, 15, 15}, /*11: 明マゼンタ */
    {15,  0,  0}, /*12: 明緑 */
    {15,  0, 15}, /*13: 明シアン */
    {15, 15,  0}, /*14: 明黄 */
    {15, 15, 15}, /*15: 明白 */
};

void v86_restore_screen(void)
{
    int i;

    /* 1. GRCG OFF */
    outp(0x7C, 0x00);

    /* 2. EGC → GRCG互換モードに復帰 */
    outp(0x6A, 0x07);  /* 拡張モード変更可 */
    outp(0x6A, 0x04);  /* GRCG互換モード */
    outp(0x6A, 0x06);  /* 拡張モード変更不可 */

    /* 3. 16色モード */
    outp(0x6A, 0x01);

    /* 4. デフォルトパレット復帰 */
    for (i = 0; i < 16; i++) {
        outp(0xA8, (u8)i);              /* パレット番号 */
        outp(0xAA, default_palette[i][0]); /* 緑 */
        outp(0xAC, default_palette[i][1]); /* 赤 */
        outp(0xAE, default_palette[i][2]); /* 青 */
    }

    /* 5. テキストGDC: 80桁モード + 表示設定 */
    outp(0x68, 0x08);  /* モードF/F1: 400ラインモード */

    /* 6. テキスト画面表示開始 (GDC START) */
    outp(0x62, 0x0D);

    /* 7. グラフィック画面表示停止 (GDC STOP) */
    outp(0xA2, 0x0C);
}
