/* ======================================================================== */
/*  V86_MEM.C — V86 メモリ空間構築                                          */
/*                                                                          */
/*  V86タスク用のページテーブル設定、IVT・BDA初期構築、                      */
/*  I/Oビットマップ設定を行う。                                             */
/*                                                                          */
/*  メモリレイアウト:                                                        */
/*    仮想 0x00000-0x9FFFF  → 物理 backing+0 〜 backing+0x9FFFF            */
/*                             (バッキングRAM: pgallocで動的確保)            */
/*    仮想 0xA0000-0xA3FFF  → 物理 0xA0000  (TVRAM)                        */
/*    仮想 0xA4000-0xA7FFF  → 物理 0xA4000  (CGウィンドウ, R/O)            */
/*    仮想 0xA8000-0xBFFFF  → 物理 0xA8000  (GVRAM Plane0-2)              */
/*    仮想 0xC0000-0xDFFFF  → NOT PRESENT                                  */
/*    仮想 0xE0000-0xE7FFF  → 物理 0xE0000  (GVRAM Plane3)                */
/*    仮想 0xE8000-0xEFFFF  → NOT PRESENT                                  */
/*    仮想 0xF0000-0xFFFFF  → 物理 0xF0000  (BIOS ROM, R/O)               */
/*                                                                          */
/*  バッキングRAMは pgalloc_alloc_n() でプログラム空間 (0x400000+) から      */
/*  連続160ページ (640KB) を動的に確保する。シェル帯域 (0x300000-0x37FFFF)  */
/*  とは物理的に分離されており、退避・復元は不要。                           */
/* ======================================================================== */

#include "v86_mem.h"
#include "v86_bda.h"
#include "paging.h"
#include "tss.h"
#include "kstring.h"
#include "memmap.h"
#include "io.h"
#include "kprintf.h"
#include "pgalloc.h"

/* v86_mem.h で定義済みの定数を使用:
 *   v86_backing_phys   (動的: pgalloc_alloc_n で確保)
 *   V86_BACKING_SIZE   0x0A0000UL
 *   V86_BACKING_PAGES  160
 *   V86_REMAP_END      0x08F000UL
 */

/* バッキングRAM物理ベースアドレス (pgallocで動的確保、初期値0) */
u32 v86_backing_phys = 0;

/* バッキングRAM有効フラグ (デフォルト=0: アイデンティティマッピング) */
static int v86_backing_enabled = 0;

/* ======================================================================== */
/*  V86専用ページテーブル (PDE[0]用コピー)                                   */
/*                                                                          */
/*  PDE[0] は 0x00000-0x3FFFFF (4MB) をカバーする。この範囲には:            */
/*    - V86バッキングRAM (0x00000-0x9FFFF)                                  */
/*    - VRAM/ROM (0xA0000-0xFFFFF)                                          */
/*    - カーネルコード (0x100000-0x1FFFFF) ← A20ラップの対象                */
/*  が含まれる。                                                             */
/*                                                                          */
/*  マスターPT[0] の全エントリをコピーした上で、HMA領域 (PTE[0x100-0x10F])  */
/*  をバッキングRAMの先頭にマッピングすることで、A20 OFFのラップアラウンド   */
/*  をページングレベルで実現する。                                           */
/*                                                                          */
/*  カーネルコード (PTE[0x110-0x1FF]) はマスターPTからコピーされるため       */
/*  Ring 0 で実行される #GP/#PF ハンドラは正常にアクセスできる。            */
/* ======================================================================== */
static u8 v86_pt0_raw[4096 + 4095];  /* 4KB PT + アライメントパディング */
static u32 *v86_pt0 = (u32 *)0;      /* アライン済みポインタ */
static u32 v86_pde0_saved = 0;       /* 元の PDE[0] 値 (復元用) */
static int v86_a20_state = 0;        /* 0=OFF(ラップ), 1=ON */

/* 4096バイト境界に切り上げ (paging.c の align4096 と同等) */
static u32 *v86_align4096(void *p)
{
    u32 a = (u32)p;
    a = (a + 4095) & ~4095UL;
    return (u32 *)a;
}

/* 旧 bda_defaults[] テーブルは NP21/W方式のBDA初期化に移行したため削除。
 * BDA初期値は v86_mem_setup() 内で NP21/W bios_reinitbyswitch() 準拠で
 * 直接 backing[] に書き込む。 */


/* IVTダミーハンドラ (IRET命令のみ) — IVT領域内に配置
 *
 * 歴史:
 *   0x600: IPLが0x0060:0x0000 (リニア0x600)にロードされ上書きされた
 *   0x500: Ys IPLが OR BYTE [0x500], 0x20 でBDAフラグを書き込み、
 *          IRET(0xCF) が OUT(0xEF) に破壊された
 *
 * 現在: IVTの未使用ベクタ領域内に配置。
 * 0x3F0 = INT FCh のオフセット下位バイトにIRET(0xCF)を書き込み、
 * 全ダミーベクタを 0x003F:0x0000 (リニア 0x3F0) に向ける。
 * IVT自体はゲストが明示的に上書きしない限り安全。
 * INT FCh-FFh のベクタは壊れるが、ダミーIRETエントリとして使われる
 * だけなので問題ない。 */
#define IVT_HANDLER_BASE   0x03F0

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

    /* バッキングRAMを pgalloc から動的確保 (連続 160ページ = 640KB) */
    v86_backing_phys = pgalloc_alloc_n(V86_BACKING_PAGES);
    if (v86_backing_phys == 0) {
        kprintf(0xE1, "[V86] ERROR: pgalloc_alloc_n(%d) failed\n",
                V86_BACKING_PAGES);
        return;
    }

    /* バッキングRAM有効フラグをセット */
    v86_backing_enabled = 1;

    /* ================================================================== */
    /*  1. バッキングRAMをゼロクリア (640KB)                               */
    /*  pgalloc 確保ページは元から PRESENT+RW のためページ属性変更不要。   */
    /* pgalloc 確保ページをアイデンティティマッピング (PRESENT+RW) に設定     */
    /* exec_run のガードページ等で NOT PRESENT になっている場合があるため必須   */
    {
        u32 pa;
        for (pa = v86_backing_phys;
             pa < v86_backing_phys + V86_BACKING_SIZE;
             pa += PAGE_SIZE) {
            paging_set_page(pa, pa, PTE_PRESENT | PTE_RW);
        }
    }
    backing = (u8 *)v86_backing_phys;
    kmemset(backing, 0, V86_BACKING_SIZE);



    /* ================================================================== */
    /*  2. IVT構築 (ハイブリッド方式)                                       */
    /*                                                                      */
    /*  0x0000:0x0000 - 0x0000:0x03FF に 256個のベクタ。                   */
    /*                                                                      */
    /*  BIOS ROM が物理アドレスに存在する場合:                              */
    /*    NP21/W の bios_vectorset() と同様に、BIOS ROM内のベクタテーブル   */
    /*    (FD80:0000 + BIOS_TABLE) からINT 00h-1Fh のエントリを読み取る。  */
    /*    これにより、INT 18h (CRT/KBD), INT 1Ch (カレンダ) 等が            */
    /*    BIOS ROM内の実ハンドラにV86モードで直接ジャンプする。             */
    /*    ROM内コードのI/O命令は#GP経由で仮想化ディスパッチャが処理。      */
    /*                                                                      */
    /*  BIOS ROM が存在しない場合:                                          */
    /*    ダミーIRETハンドラにフォールバック (従来動作)。                    */
    /*                                                                      */
    /*  例外: INT 1Bh (ディスクBIOS) はHLEスタブを維持する。               */
    /*    ディスクI/Oは loop_dev 経由が必須であり、ROM内のFDCアクセス      */
    /*    では動作しない (D88/FDIイメージは実FDCに存在しない)。            */
    /* ================================================================== */

    /* IVT: バッキングRAMの先頭 (物理 backing+0 = 仮想 0x0000) */
    ivt = (u32 *)backing;
    handler_seg = (IVT_HANDLER_BASE >> 4);
    handler_off = (IVT_HANDLER_BASE & 0x0F);
    for (i = 0; i < 256; i++) {
        ivt[i] = ((u32)handler_seg << 16) | handler_off;
    }

    /* ダミーIRETハンドラを配置 */
    backing[IVT_HANDLER_BASE] = 0xCF;  /* IRET */

    /* ================================================================== */
    /*  2b. BIOS ROM からIVTを設定 (ハイブリッド方式)                       */
    /*                                                                      */
    /*  NP21/W の BIOS ROM レイアウト:                                      */
    /*    物理 0xFD800 = セグメント FD80:0000 にBIOSコードが配置            */
    /*    BIOS_TABLE (オフセット 0x0100) にINT 00h-1Fh 用のオフセット表    */
    /*    各エントリは2バイト (WORD): BIOS_SEG 内のオフセット値             */
    /*    IVT[n] = BIOS_SEG:table[n] に設定                                */
    /*                                                                      */
    /*  BIOS ROM 存在判定:                                                  */
    /*    物理 0xE8DD8 付近のNECシグネチャ文字列 "Copyright" を確認         */
    /*    (NP21/W bios.c の neccheck[] 参照)                                */
    /* ================================================================== */
#define BIOS_ROM_BASE   0xFD800UL     /* BIOS ROMコードベース (物理) */
#define BIOS_SEG        0xFD80U       /* BIOS ROMセグメント */
#define BIOS_TABLE_OFF  0x0100U       /* ベクタテーブルオフセット */
#define BIOS_SIG_ADDR   0xE8DD8UL     /* NEC BIOSシグネチャ位置 */
    {
        volatile u8 *bios_sig = (volatile u8 *)BIOS_SIG_ADDR;
        int bios_rom_present;

        /* BIOS ROM存在チェック: "Copy" の4バイトを確認 */
        bios_rom_present = (bios_sig[0] == 'C' && bios_sig[1] == 'o' &&
                            bios_sig[2] == 'p' && bios_sig[3] == 'y');

        if (bios_rom_present) {
            /* BIOS ROMのベクタテーブルからINT 00h-1Fh のIVTを設定 */
            volatile u16 *bios_table =
                (volatile u16 *)(BIOS_ROM_BASE + BIOS_TABLE_OFF);

            kprintf(0xA1, "[V86] BIOS ROM detected, setting IVT from ROM\n");

            for (i = 0; i < 0x20; i++) {
                u16 off = bios_table[i];
                ivt[i] = ((u32)BIOS_SEG << 16) | off;
            }

            /* INT 1Eh: N88-BASIC エントリ (0xE8000000) — NP21/W互換 */
            ivt[0x1E] = 0xE8000000UL;
        } else {
            kprintf(0xE1, "[V86] BIOS ROM not found, using dummy IVT\n");
        }
    }

    /* ================================================================== */
    /*  2c. INT 1Bh HLE スタブ配置 (ディスクBIOS)                           */
    /*                                                                      */
    /*  INT 1Bh はHLEを維持する。ROM内のFDCアクセスルーチンは               */
    /*  実FDCを制御するコードであり、D88/FDIディスクイメージには            */
    /*  対応できない。loop_dev 経由の v86_bios_int1b() が必須。             */
    /*                                                                      */
    /*  DOS/IO.SYS が PUSHF+CLI+CALL FAR [IVT[1Bh]] でBIOSを呼ぶ          */
    /*  場合にもGPハンドラに入るよう、スタブ (INT 1Bh + IRET) を設定。     */
    /*                                                                      */
    /*  配置先: 0x3C0 (IVT[0xF0] のベクタ領域内)                           */
    /* ================================================================== */
#define BIOS_STUB_BASE     0x03C0
#define BIOS_STUB_SEG      0x003C
#define BIOS_STUB_OFF      0x0000
    /* INT 1Bh スタブ (offset +0) */
    backing[BIOS_STUB_BASE + 0] = 0xCD; /* INT */
    backing[BIOS_STUB_BASE + 1] = 0x1B; /* 1Bh */
    backing[BIOS_STUB_BASE + 2] = 0xCF; /* IRET */

    /* IVT[0x1B] = HLEスタブ (003C:0000) — 常にHLEを使う */
    ivt[0x1B] = ((u32)BIOS_STUB_SEG << 16) | BIOS_STUB_OFF;

    /* ================================================================== */
    /*  3. BDA初期値設定 (NP21/W bios_reinitbyswitch 準拠)                  */
    /*                                                                      */
    /*  NP21/W の biosmem.h + bios.c を参考に、全てのブートクリティカルな   */
    /*  BDAフィールドを初期化する。                                         */
    /*                                                                      */
    /*  計算ロジック:                                                        */
    /*    PRXCRT/PRXDUPD/BIOS_FLAG1 はメモリスイッチ (TVRAM 0xA3FE2-) と   */
    /*    ハードウェア状態から NP21/W と同じアルゴリズムで計算する。         */
    /* ================================================================== */

    /* --- メモリスイッチ初期化 (BDA計算前に設定する必要あり) --- */
    {
        volatile u16 *tvram16 = (volatile u16 *)0xA0000UL;
        /* MEMSW1: 0x48 = bit6(25行モード) | bit3(RS232C) */
        tvram16[0x3FE2 / 2] = 0x48;
        /* MEMSW2: 0x05 = 10MHzクロック系 + bit2(RS-232C 9600bps) */
        tvram16[0x3FE4 / 2] = 0x05;
        /* MEMSW3: 0x04 = bit2(31kHz CRT) */
        tvram16[0x3FE6 / 2] = 0x04;
        /* MEMSW4: 0x00 */
        tvram16[0x3FE8 / 2] = 0x00;
        /* MEMSW5: 0x10 = bit7-4=ブート (0001=1MB FDD)
         *   NP21/W: boot = mem[MEMB_MSW5] & 0xF0
         *   0x10 = 1MB FDD, 0x20 = 640KB FDD */
        tvram16[0x3FEA / 2] = 0x10;
        /* MEMSW6: 0x00 */
        tvram16[0x3FEC / 2] = 0x00;
    }

    /* --- CPU / システムタイプ --- */
    /* MEMB_SYS_TYPE (0x0480): CPUタイプ (NP21/W: 0x03 = i386以上) */
    backing[BDA_CPU_FLAG] = 0x03;
    /* CPU_TYPE (0x0484): レガシー互換 */
    backing[BDA_CPU_TYPE] = 0x03;
    /* BIOS_FLAG3 (0x0481): 0x00 (PC-9801-119未搭載) */
    backing[BDA_BIOS_FLAG3] = 0x00;
    /* DISK_EQUIPS (0x0482): 0x00 (SCSI HDDなし) */
    backing[BDA_DISK_EQUIPS] = 0x00;

    /* --- BIOS_FLAG0 (0x0500): 初期化済みフラグ + FDD種別 ---
     * NP21/W: mem[MEMB_BIOS_FLAG0] = 0x01;
     *         boot != 0x20 → |= 0x02 (1MB FDD)
     * OS32: 1MB FDD前提 → 0x03 */
    backing[BDA_BIOS_FLAG0] = 0x03;

    /* --- BIOS_FLAG1 (0x0501): クロック/CPU/メモリ ---
     * NP21/W 計算ロジック:
     *   biosflag = 0x20 (PC-9801無印以外)
     *   8MHz → |= 0x80
     *   MEMSW3 下位3bit → |= (mem[0xA3FEA] & 7)
     *   V30モード → |= 0x40 */
    {
        volatile u16 *tvram16 = (volatile u16 *)0xA0000UL;
        u8 memsw3 = (u8)(tvram16[0x3FEA / 2] & 0xFF);
        u8 biosflag = 0x20;   /* bit5=1: PC-9801無印以外 */
        biosflag |= 0x80;     /* bit7=1: 8MHz系以上 */
        biosflag |= memsw3 & 7; /* bit2-0: CRT周波数 */
        backing[BDA_BIOS_FLAG] = biosflag;
    }

    /* --- 拡張メモリサイズ (0x0401) ---
     * NP21/W: extmem = min(pccore.extmem, 14); mem[MEMB_EXPMMSZ] = extmem<<3
     * OS32: 拡張メモリ未提供 → 0 */
    backing[BDA_EXPMMSZ] = 0x00;

    /* --- PRXCRT (0x054C): CRT制御状態 ---
     * NP21/W 計算ロジック:
     *   prxcrt = 0x08                              (bit3=常時1)
     *   dipsw1-1 off → |= 0x40                    (25行モード)
     *   アナログ表示 → |= 0x04                    (16色)
     *   dipsw1-8 off → |= 0x01
     *   GRCG搭載 → |= 0x02
     * OS32ターゲット: NP21/W PC-9821 (GRCG+EGC+アナログ+25行) */
    {
        volatile u16 *tvram16 = (volatile u16 *)0xA0000UL;
        u8 memsw1 = (u8)(tvram16[0x3FE2 / 2] & 0xFF);
        u8 prxcrt = 0x08;           /* bit3=1: 常時ON */
        if (memsw1 & 0x40) {
            prxcrt |= 0x40;         /* bit6=1: 25行モード */
        }
        prxcrt |= 0x04;             /* bit2=1: アナログ16色 */
        prxcrt |= 0x01;             /* bit0=1: dipsw1-8 */
        prxcrt |= 0x02;             /* bit1=1: GRCG搭載 */
        backing[BDA_PRXCRT] = prxcrt;
    }

    /* --- PRXDUPD (0x054D): 表示更新状態 ---
     * NP21/W 計算ロジック:
     *   prxdupd = 0x18                             (bit4,3=1)
     *   GRCG chip >= 3 → |= 0x40                  (EGC搭載)
     *   dipsw2-8 off → |= 0x20
     * OS32: PC-9821 (EGC搭載) */
    backing[BDA_PRXDUPD] = 0x78;  /* 0x18 | 0x40(EGC) | 0x20(dipsw2-8) */

    /* --- CRT関連 --- */
    backing[BDA_CRT_RASTER] = 0x0F;   /* CRTラスタ (NP21/W固定値) */
    backing[BDA_CRT_STS]    = 0x12;   /* bit4=16色, bit1=GRCG */
    backing[BDA_CRT_CNT]    = 0x00;   /* CRTカウンタ初期値 */
    backing[BDA_CRT_BIOS]   = 0x84;   /* bit7=31kHz, bit2=PC-9821 */

    /* CRT VRAMアドレスとラスタ (WORD) */
    backing[BDA_CRT_W_VRAMADR]     = 0x00;
    backing[BDA_CRT_W_VRAMADR + 1] = 0x00;
    backing[BDA_CRT_W_RASTER]      = 0x00;
    backing[BDA_CRT_W_RASTER + 1]  = 0x00;
    /* PRXGLS (WORD) */
    backing[BDA_PRXGLS]     = 0x00;
    backing[BDA_PRXGLS + 1] = 0x00;

    /* --- キーボード --- */
    backing[BDA_KB_HEAD]     = (u8)(BDA_KB_BUF_START & 0xFF);
    backing[BDA_KB_HEAD + 1] = (u8)(BDA_KB_BUF_START >> 8);
    backing[BDA_KB_TAIL]     = (u8)(BDA_KB_BUF_START & 0xFF);
    backing[BDA_KB_TAIL + 1] = (u8)(BDA_KB_BUF_START >> 8);
    backing[BDA_KB_COUNT]    = 0x00;
    backing[BDA_KB_RETRY]    = 0x00;
    backing[BDA_SHIFT_STS]   = 0x00;

    /* --- メモリサイズ --- */
    backing[BDA_MEM_SIZE]     = (640) & 0xFF;
    backing[BDA_MEM_SIZE + 1] = (640 >> 8) & 0xFF;
    backing[BDA_CONV_MEM]     = 0xA0;   /* 640KB (0xA0 * 4KB) */

    /* --- ディスク関連 --- */
    backing[BDA_DISK_EQUIP]     = 0x01;  /* UNIT#0 のみ接続 */
    backing[BDA_DISK_EQUIP + 1] = 0x00;
    backing[BDA_SASI_IDE]       = 0x00;  /* HDDなし */
    backing[BDA_DISK_INTL]      = 0x00;
    backing[BDA_DISK_INTH]      = 0x00;
    backing[BDA_BOOT_DEV]       = 0x90;  /* 1MB FDD UNIT#0 */

    /* --- FDDモード/パラメータポインタ ---
     * NP21/W: F2HD_MODE=0xFF, F2DD_MODE=0xFF
     *         F2DD_POINTER=0xFD801AD7 (BIOS ROM内)
     *         F2HD_POINTER=0xFD801AAF (BIOS ROM内)
     * これらはINT 1BhのFDDパラメータ参照に使われる可能性がある */
    backing[BDA_F2HD_MODE] = 0xFF;
    backing[BDA_F2DD_MODE] = 0xFF;
    /* F2DD_POINTER (DWORD, リトルエンディアン) */
    backing[BDA_F2DD_POINTER + 0] = 0xD7;
    backing[BDA_F2DD_POINTER + 1] = 0x1A;
    backing[BDA_F2DD_POINTER + 2] = 0x80;
    backing[BDA_F2DD_POINTER + 3] = 0xFD;
    /* F2HD_POINTER (DWORD, リトルエンディアン) */
    backing[BDA_F2HD_POINTER + 0] = 0xAF;
    backing[BDA_F2HD_POINTER + 1] = 0x1A;
    backing[BDA_F2HD_POINTER + 2] = 0x80;
    backing[BDA_F2HD_POINTER + 3] = 0xFD;

    /* --- その他 NP21/W固定値 --- */
    backing[BDA_BIOS_FLAG5]  = 0x00;  /* 非NESA, WAITなし */
    backing[BDA_WAIT_FLAG]   = 0x80;  /* bit7=1: OUT 5Fh wait有効 */
    backing[BDA_PC9821_FLAG] = 0x40;  /* bit6=1: PC-9821 */
    backing[BDA_RS_S_FLAG]   = 0x00;  /* RS-232Cフラグ */
    /* GRCG関連 */
    backing[BDA_GRCG]     = 0x00;     /* GRCG OFF */
    backing[BDA_TILE_REG]     = 0x00; /* タイルレジスタ0 */
    backing[BDA_TILE_REG + 1] = 0x00; /* タイルレジスタ1 */
    backing[BDA_TILE_REG + 2] = 0x00; /* タイルレジスタ2 */
    backing[BDA_TILE_REG + 3] = 0x00; /* タイルレジスタ3 */
    /* ブートパーティション */
    backing[BDA_BOOT_PART]     = 0x00;
    backing[BDA_BOOT_PART + 1] = 0x00;

    /* ================================================================== */
    /*  4. ページテーブル設定                                              */
    /*                                                                      */
    /*  x86 ページングは PDE と PTE の両方で U/S ビットをチェックする。     */
    /*  V86モード (CPL=3) では PTE_USER が必須。                           */
    /* ================================================================== */

    /* 仮想 0x00000-0x8EFFF → 物理 backing+0 〜 backing+0x8EFFF (バッキングRAM, R/W) */
    for (virt = 0x00000; virt < V86_REMAP_END; virt += PAGE_SIZE) {
        phys = v86_backing_phys + virt;
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
        kmemcpy((u8 *)(v86_backing_phys + 0x8F000UL),
                (u8 *)0x8F000UL,
                0x11000UL);

        /* リマップ: 仮想 0x8F000-0x9FFFF → 物理 backing+0x8F000 〜 */
        for (addr = 0x8F000; addr < 0xA0000; addr += PAGE_SIZE) {
            paging_set_page(addr, v86_backing_phys + addr,
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

    /* ================================================================== */
    /*  A20ラップアラウンド — V86専用ページテーブル方式                      */
    /*                                                                      */
    /*  リアルモードでは A20=OFF の場合 linear 0xFFFFF を超えるアドレスが   */
    /*  0x00000 にラップアラウンドする。V86モードではA20が常に有効なため     */
    /*  ラップが発生しない。                                                */
    /*                                                                      */
    /*  解決: PDE[0]用のPTをコピーし、HMA部分 (PTE[0x100-0x10F]) を         */
    /*  バッキングRAMの先頭にマッピングする。カーネルコード                 */
    /*  (PTE[0x110-0x1FF]) はコピーから継承されるため、Ring 0での             */
    /*  #GP/#PFハンドラ実行には影響しない。                                 */
    /*                                                                      */
    /*  NP21/W HAXM の i386hax_vm_sethmemory() と同等のアプローチ。         */
    /* ================================================================== */
    {
        u32 *master_pt0 = paging_get_pt0();
        int pi;

        /* V86専用PTをアライン */
        v86_pt0 = v86_align4096(v86_pt0_raw);

        /* マスターPT[0] の全エントリをコピー */
        for (pi = 0; pi < PTE_COUNT; pi++) {
            v86_pt0[pi] = master_pt0[pi];
        }

        /* HMA領域 (PTE[0x100-0x10F]) をバッキングRAMの先頭にマッピング
         * → A20 OFF のラップアラウンドを実現
         * 仮想 0x100000+i*4096 → 物理 backing+i*4096 */
        for (pi = 0; pi < 16; pi++) {
            v86_pt0[0x100 + pi] =
                (v86_backing_phys + (u32)pi * PAGE_SIZE)
                | PTE_PRESENT | PTE_RW | PTE_USER;
        }

        /* A20初期状態: OFF (ラップ有効) */
        v86_a20_state = 0;

        /* PDE[0] の元の値を退避 */
        v86_pde0_saved = paging_get_pde(0);

        /* PDE[0] を V86専用PTに差し替え + PTE_USER を付与 */
        paging_set_pde(0,
            (u32)v86_pt0 | PTE_PRESENT | PTE_RW | PTE_USER);
    }

    /* ================================================================== */
    /*  5. I/Oビットマップ設定                                            */
    /*                                                                      */
    /*  デフォルトは全トラップ (tss_init で設定済み)。                      */
    /*  安全なポートのみパススルー許可。                                    */
    /*  PIC (00h,02h,08h,0Ah) / PIT (71h,73h,75h,77h) /                   */
    /*  FDC (BE/CC/CA) は仮想化のためトラップのまま。                      */
    /* ================================================================== */

    /* キーボード 8251 — トラップして仮想化
     * ゲストのINT 09hハンドラがポート0x41を読む時、
     * v86_kbd_bufからスキャンコードを供給する。
     * tss_iomap_allow(0x41); — トラップ維持
     * tss_iomap_allow(0x43); — トラップ維持 */

    /* テキストGDC + モードFF1 (60h-6Ah 偶数)
     * 0x60: ステータス読み出し (bit5=VSYNC) → トラップして仮想化
     * 0x64: VSYNC割り込みトリガ → トラップして仮想化 */
    /* tss_iomap_allow(0x60); — VSYNCポーリング仮想化のためトラップ維持 */
    tss_iomap_allow(0x62);
    /* tss_iomap_allow(0x64); — VSYNC割り込みアーム仮想化のためトラップ維持 */
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

    /* グラフィックGDC + パレット (A0h-AEh 偶数)
     * 0xA0: ステータス読み出し (bit5=VSYNC) → トラップして仮想化 */
    /* tss_iomap_allow(0xA0); — VSYNCポーリング仮想化のためトラップ維持 */
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

    /* FM音源 (188h-18Eh 偶数) — 直接パススルー */
    tss_iomap_allow(0x0188);
    tss_iomap_allow(0x018A);
    tss_iomap_allow(0x018C);
    tss_iomap_allow(0x018E);

    /* カレンダBIOS用ポート (20h) */
    tss_iomap_allow(0x20);

    /* ビープON/OFFポート (37h) — Ys等がBEEP音源BGMで使用 */
    tss_iomap_allow(0x37);

    /* ウェイト用ダミーI/Oポート (5Fh) — 直接パススルー
     * PC-9800Bible §2-13: FM音源レジスタアクセス間のウェイトとして
     * OUT 5Fh を20回以上繰り返すのが標準手順。
     * トラップ状態だとGPフォルトのオーバーヘッドが大きすぎて
     * 音楽再生のリアルタイム性が失われる。 */
    tss_iomap_allow(0x5F);

    /* ================================================================== */
    /*  6. 画面初期化 (NP21/W pccore_reset + bios0x18_16 準拠)             */
    /*                                                                      */
    /*  V86ゲスト起動前にGVRAM/TVRAMをクリアし、GDCとパレットを             */
    /*  デフォルト状態に戻す。ネイティブゲーム(Ys等)はGDC初期化を           */
    /*  自前で行わず、BIOSが設定済みであることを前提とするため必須。         */
    /* ================================================================== */
    {
        volatile u8 *gvram_b = (volatile u8 *)0xA8000UL;
        volatile u8 *gvram_e = (volatile u8 *)0xE0000UL;
        volatile u16 *tvram  = (volatile u16 *)0xA0000UL;
        int i;

        /* GVRAM全面クリア (Plane B/R/G: 0xA8000-0xBFFFF, Plane E: 0xE0000-0xE7FFF) */
        kmemset((u8 *)gvram_b, 0, 0x18000);  /* 96KB: B+R+G */
        kmemset((u8 *)gvram_e, 0, 0x08000);  /* 32KB: E */

        /* TVRAMクリア (0xA0000-0xA1FFF: 文字コード, 0xA2000-0xA3FFF: アトリビュート)
         * WORD単位でアクセス (PC-98 TVRAMはWORDアドレッシング) */
        for (i = 0; i < 0x2000; i++) {
            tvram[i] = 0x0000;          /* 文字コード: 空白 */
        }
        for (i = 0x1000; i < 0x2000; i++) {
            tvram[i] = 0x00E1;          /* アトリビュート: 白文字、表示ON */
        }

        /* GRCG OFF (ポート 0x7C に 0 を出力) */
        outp(0x7C, 0x00);

        /* アナログパレット初期化 (PC-98 デフォルト16色)
         * ポート: 0xA8=パレット番号, 0xAA=G, 0xAC=R, 0xAE=B (各4bit) */
        {
            /* NP21/W bios0x18_16 のデフォルトパレット (PC-98標準) */
            static const u8 def_pal[16][3] = {
                /* G,   R,   B */
                { 0x0, 0x0, 0x0 },  /* 0: 黒 */
                { 0x0, 0x0, 0x7 },  /* 1: 青 */
                { 0x0, 0x7, 0x0 },  /* 2: 赤 */
                { 0x0, 0x7, 0x7 },  /* 3: マゼンタ */
                { 0x7, 0x0, 0x0 },  /* 4: 緑 */
                { 0x7, 0x0, 0x7 },  /* 5: シアン */
                { 0x7, 0x7, 0x0 },  /* 6: 黄 */
                { 0x7, 0x7, 0x7 },  /* 7: 白 */
                { 0x4, 0x4, 0x4 },  /* 8: 灰 */
                { 0x0, 0x0, 0xF },  /* 9: 明青 */
                { 0x0, 0xF, 0x0 },  /* A: 明赤 */
                { 0x0, 0xF, 0xF },  /* B: 明マゼンタ */
                { 0xF, 0x0, 0x0 },  /* C: 明緑 */
                { 0xF, 0x0, 0xF },  /* D: 明シアン */
                { 0xF, 0xF, 0x0 },  /* E: 明黄 */
                { 0xF, 0xF, 0xF },  /* F: 明白 */
            };
            for (i = 0; i < 16; i++) {
                outp(0xA8, (u8)i);         /* パレット番号 */
                outp(0xAA, def_pal[i][0]);  /* G */
                outp(0xAC, def_pal[i][1]);  /* R */
                outp(0xAE, def_pal[i][2]);  /* B */
            }
        }

        /* テキスト画面表示OFF → ゲストが自前で設定する
         * GDCコマンドSTOP1: ポート0x62に0x0Dを出力 (テキスト表示停止) */
        outp(0x62, 0x0D);

        /* グラフィック画面表示ON:
         * GDCコマンドSTART: ポート0xA2に0x0Dを出力 (グラフィック表示開始) */
        outp(0xA2, 0x0D);

        /* グラフィックGDC SCROLL コマンド (0x70): 表示開始アドレス初期化
         * GDC I/O: コマンド→0xA2, パラメータ→0xA0
         * SAD1=0x0000 (VRAM先頭), SL=0 (全画面1分割)
         * NP21/W gdc.c リセット: ZeroMemory(gdc.s.para + GDC_SCROLL, 4)
         * デフォルト 2.5MHz GDC: IM=0 */
        outp(0xA2, 0x70);       /* SCROLL コマンド, RA=0 */
        outp(0xA0, 0x00);       /* SAD1 下位バイト = 0x00 */
        outp(0xA0, 0x00);       /* SAD1 上位バイト = 0x00 */
        outp(0xA0, 0x00);       /* SL1 下位バイト = 0x00 */
        outp(0xA0, 0x00);       /* SL1 上位 + IM=0 (2.5MHz デフォルト) */

        /* グラフィックGDC PITCH コマンド (0x47): VRAM横幅
         * NP21/W デフォルト: gdc.s.para[GDC_PITCH] = 40
         * 2.5MHzモード: 40ワード (0x28) — 200ラインゲームの標準値 */
        outp(0xA2, 0x47);       /* PITCH コマンド */
        outp(0xA0, 0x28);       /* 40ワード (2.5MHz デフォルト) */

        /* グラフィックGDC CSRFORM (0x4B): L/R=1 (200ラインモード)
         * 200ラインモードでは各VRAMラインを2倍表示する。
         * L/R=1 → 2倍表示。L/R=0 → 1倍表示(400ライン)。
         * OS32のgfx_init()がL/R=0に設定しているため、ここで戻す。
         * NP21/W: gdc.s.para[GDC_CSRFORM] = 1 (200ラインモード) */
        outp(0xA2, 0x4B);       /* CSRFORM コマンド */
        outp(0xA0, 0x01);       /* L/R = 1 (200ラインモード, 各ライン2倍表示) */

        /* モードフリップフロップ1 (0x68): 200ラインモード
         * MFF1 GRP Mode = 0x09: 奇数ラスタ非表示
         *   → CRT400ライン + グラフィック200ラインの組み合わせ
         * OS32のgfx_init()が0x08(400ライン)に設定しているため戻す */
        outp(0x68, 0x09);       /* 200ラインモード (奇数ラスタ非表示) */

        /* テキストGDC SCROLL 初期化 (全画面表示)
         * GDC I/O: コマンド→0x62, パラメータ→0x60 */
        outp(0x62, 0x70);       /* SCROLL コマンド, RA=0 */
        outp(0x60, 0x00);       /* SAD1 下位 = 0 */
        outp(0x60, 0x00);       /* SAD1 上位 = 0 */
        outp(0x60, 0x00);       /* SL1 下位 = 0 */
        outp(0x60, 0x00);       /* SL1 上位 = 0 */

        /* テキストGDC PITCH */
        outp(0x62, 0x47);       /* PITCH コマンド */
        outp(0x60, 0x50);       /* 80ワード */

        /* グラフィックGDC START: 表示開始 */
        outp(0xA2, 0x0D);       /* START コマンド */

        /* ページフリッピング解除: ページ0に復帰
         * OS32のgfx_init()がページ1を描画ページにしている場合がある */
        outp(0xA4, 0x00);       /* 表示ページ = 0 */
        outp(0xA6, 0x00);       /* 描画ページ = 0 */

        /* モードフリップフロップ2 (0x6A): 16色モード + GDCクロック初期化
         * NP21/W gdc.c: gdc.clock = 0 (2.5MHz デフォルト) */
        outp(0x6A, 0x07);       /* 拡張モード変更可 */
        outp(0x6A, 0x04);       /* GRCG互換モード */
        outp(0x6A, 0x06);       /* 拡張モード変更不可 */
        outp(0x6A, 0x01);       /* 16色モード */
        outp(0x6A, 0x82);       /* GDC CLOCK-1 = 2.5MHz */
        outp(0x6A, 0x84);       /* GDC CLOCK-2 = 2.5MHz */
    }
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

    /* 仮想 0x8F000-0x9FFFF: バッキングRAMから物理ページに内容を書き戻し、   */
    /* アイデンティティマッピングに復元する。                                 */
    /*                                                                        */
    /* ★順序が重要: 先に remap → 後に kmemcpy                               */
    /*                                                                        */
    /* setup 時に VA 0x8F000 → PA 0x38F000 にリマップされている。           */
    /* この状態で kmemcpy(VA 0x8F000, VA 0x38F000, ...) を行うと、            */
    /* 両方とも PA 0x38F000 を指すため no-op になり、PA 0x8F000 が             */
    /* 更新されない。longjmp 復帰後に ESP が VA 0x9Fxxx を指すが、            */
    /* PA 0x9Fxxx は v86_mem_setup 以前の古いデータのままで #PF になる。      */
    /*                                                                        */
    /* 修正: 先に identity remap (VA 0x8F000 → PA 0x8F000) してから          */
    /* kmemcpy(VA 0x8F000, VA 0x38F000, ...) を行う。                         */
    /* これで src=PA 0x38F000, dst=PA 0x8F000 となり正しくコピーされる。     */
    /*                                                                        */
    /* スタック安全性: この関数は v86_session_run_core() の inline asm で     */
    /* v86_kstack 上で呼ばれるため、0x8F000-0x9FFFF のリマップ変更は          */
    /* 現在のスタックに影響しない。                                           */
    {
        u32 eflags;
        __asm__ volatile("pushfl; popl %0" : "=r"(eflags));
        __asm__ volatile("cli");

        /* 1. 先にアイデンティティマッピングに復元                            */
        /*    VA 0x8F000 → PA 0x8F000 に戻す                                */
        for (addr = 0x8F000; addr < 0xA0000; addr += PAGE_SIZE) {
            paging_set_page(addr, addr, PTE_PRESENT | PTE_RW);
        }

        /* 2. バッキングRAM (backing+0x8F000) の内容を                        */
        /*    元の物理ページ (VA=PA 0x8F000) に書き戻す                       */
        kmemcpy((u8 *)0x8F000UL,
                (u8 *)(v86_backing_phys + 0x8F000UL),
                0x11000UL);

        if (eflags & 0x200) {
            __asm__ volatile("sti");
        }
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

    /* PDE[0] をマスターPTに復元 (V86専用PTから元に戻す) */
    if (v86_pde0_saved != 0) {
        paging_set_pde(0, v86_pde0_saved);
        v86_pde0_saved = 0;
    } else {
        /* フォールバック: PTE_USER のみ除去 */
        paging_pde_clear_flags(0x00000, PTE_USER);
    }
    v86_a20_state = 0;

    /* I/Oビットマップを全トラップに戻す */
    tss_iomap_deny_all();

    /* バッキングRAM無効化 */
    v86_backing_enabled = 0;

    /* NULL保護ページを復元 */
    paging_set_page(0x00000, 0, PAGE_NOT_PRESENT);

    /* バッキングRAM解放 (pgalloc にページを返却) */
    if (v86_backing_phys != 0) {
        pgalloc_free_n(v86_backing_phys, V86_BACKING_PAGES);
        v86_backing_phys = 0;
    }
}

/* ======================================================================== */
/*  v86_a20_set — A20ライン状態を変更 (ページテーブル更新)                   */
/*                                                                          */
/*  NP21/W cpuio.c の CPU_A20EN() + haxcore.c の i386hax_vm_sethmemory()    */
/*  に相当する処理。V86専用PT[0] のHMAエントリ (PTE[0x100-0x10F]) を         */
/*  書き換えることでA20のON/OFFをエミュレートする。                          */
/*                                                                          */
/*  enable=0 (A20 OFF): 0x100000-0x10FFFF → backing+0x00000 (ラップ)       */
/*  enable=1 (A20 ON):  Phase 1 では未サポート → ラップを維持              */
/* ======================================================================== */
void v86_a20_set(int enable)
{
    int pi;

    if (!v86_pt0) return;  /* V86未初期化 */

    if (enable) {
        /* Phase 1: A20 ON は未サポート (バッキングRAMが640KBしかない)
         * HMA用の物理RAMが無いためラップを維持する。
         * Phase 2 でバッキングRAMを拡張後に対応予定。 */
        kprintf(0xA1, "[V86] A20 ON requested (not yet supported, keeping wrap)\n");
    } else {
        /* A20 OFF: HMAをバッキングRAMの先頭にマッピング (ラップ) */
        for (pi = 0; pi < 16; pi++) {
            v86_pt0[0x100 + pi] =
                (v86_backing_phys + (u32)pi * PAGE_SIZE)
                | PTE_PRESENT | PTE_RW | PTE_USER;
        }
    }

    v86_a20_state = enable;

    /* TLBフラッシュ (CR3リロード方式) */
    {
        u32 cr3_val;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
    }
}

/* ======================================================================== */
/*  v86_a20_get — A20ライン状態を取得                                       */
/*  戻り値: 0=OFF(ラップ有効), 1=ON                                         */
/* ======================================================================== */
int v86_a20_get(void)
{
    return v86_a20_state;
}

/* ======================================================================== */
/*  v86_phys_addr — V86仮想アドレスからカーネル用リニアアドレスに変換       */
/*                                                                          */
/*  V86の seg:off (リニア = seg<<4 + off) を、カーネル (Ring0) から          */
/*  アクセスできる物理アドレスに変換する。                                  */
/*                                                                          */
/*  0x00000-0x9FFFF → 物理 backing + offset  (バッキングRAM)              */
/*  0xA0000-0xFFFFF → 物理 = 仮想  (実機ハードウェア)                     */
/*                                                                          */
/*  ※ 方法Cリマップ対応: 0x8F000-0x9FFFF もバッキングRAMにリマップ       */
/*    されているため、0xA0000 未満を全てバッキングRAM経由にする。           */
/* ======================================================================== */
u8 *v86_phys_addr(u32 seg, u32 off)
{
    u32 linear = (seg << 4) + off;
    linear &= 0xFFFFF;  /* 1MB境界でラップ */

    /* バッキングRAMが有効で、コンベンショナルメモリ (0-9FFFF) はバッキングRAM */
    if (v86_backing_enabled && linear < 0xA0000UL) {
        return (u8 *)(v86_backing_phys + linear);
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

/* ======================================================================== */
/*  v86_tvram_save / v86_tvram_restore — TVRAM 退避・復元 (§1.7)            */
/*                                                                          */
/*  PC-98 TVRAM: 物理 0xA0000-0xA1FFF (8KB)                                */
/*    0xA0000-0xA0FFF: テキストコード (2KB, 80桁×25行×2byte)               */
/*    0xA2000-0xA3FFF: テキスト属性  (2KB, 同)                             */
/*  V86開始前後では物理アドレスへの直接書き込みが必要。                       */
/*  V86 バッキングRAMは 0xA0000 より上 (pgalloc管理域) にあるため、          */
/*  物理 0xA0000 への TVRAM アクセスはバッキングRAMと衝突しない。           */
/* ======================================================================== */
#define TVRAM_PHYS_BASE  0xA0000UL
#define TVRAM_SIZE       0x02000UL  /* テキストコード 4KB + 属性 4KB = 合計 8KB で安全マージン */

/* 退避バッファ (static — スタックに置くには大きすぎる) */
static u8 tvram_save_buf[TVRAM_SIZE];
static int tvram_saved = 0;

void v86_tvram_save(void)
{
    u8 *tvram = (u8 *)TVRAM_PHYS_BASE;
    u32 i;
    for (i = 0; i < TVRAM_SIZE; i++) {
        tvram_save_buf[i] = tvram[i];
    }
    tvram_saved = 1;
}

void v86_tvram_restore(void)
{
    u8 *tvram;
    u32 i;
    if (!tvram_saved) return;
    tvram = (u8 *)TVRAM_PHYS_BASE;
    for (i = 0; i < TVRAM_SIZE; i++) {
        tvram[i] = tvram_save_buf[i];
    }
    tvram_saved = 0;
}
