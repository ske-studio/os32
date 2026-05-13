/* ======================================================================== */
/*  V86_BDA.H - PC-98 BDA (BIOS Data Area) 定数定義                         */
/*                                                                          */
/*  V86サブシステム内で参照される BDA オフセット定数を集約する。              */
/*  出典: NP21/W biosmem.h, PC9800Bible, UNDOCUMENTED memsys.md             */
/* ======================================================================== */

#ifndef V86_BDA_H
#define V86_BDA_H

/* ====================================================================== */
/*  機種・BIOS フラグ (0x0400-0x04FF)                                       */
/* ====================================================================== */
#define BDA_BIOS_FLAG2    0x0400   /* 機種フラグ */
#define BDA_EXPMMSZ       0x0401   /* 拡張メモリサイズ (MEMB_EXPMMSZ) */
#define BDA_MEM_SIZE      0x0413   /* コンベンショナルメモリサイズ (WORD, KB) */
#define BDA_WAIT_FLAG     0x045B   /* ウェイトフラグ: bit7=OUT 5Fh wait有効 */
#define BDA_PC9821_FLAG   0x045C   /* PC-9821拡張フラグ: bit6=PC-9821 */
#define BDA_BIOS_FLAG5    0x0458   /* NESA/WAIT */
#define BDA_DISK_XROM     0x04B0   /* ディスクXROM (16バイト) */
#define BDA_CPU_FLAG      0x0480   /* CPU種別フラグ (MEMB_SYS_TYPE) */
#define BDA_BIOS_FLAG3    0x0481   /* BIOS拡張フラグ (MEMB_BIOS_FLAG3) */
#define BDA_DISK_EQUIPS   0x0482   /* ディスク装備数 (MEMB_DISK_EQUIPS) */
#define BDA_CPU_TYPE      0x0484   /* CPUタイプ (03=i386以上) */
#define BDA_F2HD_MODE     0x0493   /* 2HDモード (MEMB_F2HD_MODE, 0xFF=未使用) */
#define BDA_GRCG          0x0495   /* GRAPH_CHG: GRCG状態 */
#define BDA_TILE_REG      0x0496   /* GRAPH_TAL[0-3]: タイルレジスタ */
#define BDA_HRT_TIME      0x04F1   /* 高精度タイマ (DWORD, SUPPORT_HRTIMER) */

/* ====================================================================== */
/*  BIOS フラグ / キーボード (0x0500-0x054F)                                */
/* ====================================================================== */
#define BDA_BIOS_FLAG0    0x0500   /* BIOS基本フラグ (MEMB_BIOS_FLAG0)
                                    * bit0=1: 初期化済み
                                    * bit1=1: 1MB FDD接続 */
#define BDA_BIOS_FLAG     0x0501   /* BIOS_FLAG (MEMB_BIOS_FLAG1)
                                    * bit7=8MHz系, bit6=V30, bit5=PC-9801以外
                                    * bit2-0=MEMSW3下位3bit (CRT周波数) */
#define BDA_KB_BUF_START  0x0502   /* バッファ先頭 (16エントリ x 2バイト) */
#define BDA_KB_BUF_END    0x0522   /* バッファ末尾 */
#define BDA_KB_HEAD       0x0524   /* 取出ポインタ (WORD) */
#define BDA_KB_TAIL       0x0526   /* 入力ポインタ (WORD) */
#define BDA_KB_COUNT      0x0528   /* バッファ内キー数 (BYTE) */
#define BDA_KB_RETRY      0x0529   /* KBリトライ (MEMB_KB_RETRY) */
#define BDA_KB_KEY_STS    0x052A   /* キー押下状態テーブル (16バイト) */
#define BDA_SHIFT_STS     0x053A   /* シフトキー状態 (MEMB_SHIFT_STS) */

/* ====================================================================== */
/*  CRT / 画面制御 (0x053B-0x054F)                                          */
/* ====================================================================== */
#define BDA_CRT_RASTER    0x053B   /* CRTラスタ (MEMB_CRT_RASTER, 0x0F) */
#define BDA_CRT_STS       0x053C   /* CRT_STS_FLAG (MEMB_CRT_STS_FLAG) */
#define BDA_CRT_CNT       0x053D   /* CRTカウンタ (MEMB_CRT_CNT) */
#define BDA_CRT_W_VRAMADR 0x0548   /* CRT VRAMアドレス (WORD) */
#define BDA_CRT_W_RASTER  0x054A   /* CRTラスタW (WORD) */
#define BDA_PRXCRT        0x054C   /* CRT制御状態 (MEMB_PRXCRT)
                                    * bit6=25行, bit3=常時1
                                    * bit2=アナログ16色, bit1=GRCG
                                    * bit0=dipsw1-8 */
#define BDA_PRXDUPD       0x054D   /* 表示更新状態 (MEMB_PRXDUPD)
                                    * bit6=EGC搭載, bit5=dipsw2-8
                                    * bit4=1, bit3=1 */
#define BDA_PRXGLS        0x054E   /* GVRAMパレット状態 (WORD) */

/* ====================================================================== */
/*  RS-232C / ディスク (0x0550-0x05FF)                                      */
/* ====================================================================== */
#define BDA_RS_S_FLAG     0x055B   /* RS-232Cフラグ */
#define BDA_DISK_EQUIP    0x055C   /* ディスク接続状態 (WORD) */
#define BDA_SASI_IDE      0x055D   /* SASI/IDE HDD接続情報 */
#define BDA_DISK_INTL     0x055E   /* ディスク割り込みL */
#define BDA_DISK_INTH     0x055F   /* ディスク割り込みH */
#define BDA_FDC_RESULT    0x0564   /* FDC結果バッファ (8バイト × ユニット) */
#define BDA_BOOT_DEV      0x0584   /* ブートデバイス DA/UA */
#define BDA_CA_TIM_CNT    0x058A   /* カレンダタイマカウンタ (WORD) */
#define BDA_CRT_BIOS      0x0597   /* CRT BIOS状態 (MEMB_CRT_BIOS)
                                    * bit7=31kHz対応, bit2=PC-9821 */
#define BDA_CONV_MEM      0x05AE   /* コンベンショナルメモリ (x4KB) */
#define BDA_F2DD_MODE     0x05CA   /* 2DDモード (MEMB_F2DD_MODE, 0xFF=未使用) */
#define BDA_F2DD_POINTER  0x05CC   /* 2DDパラメータポインタ (DWORD, FAR PTR) */
#define BDA_F2HD_POINTER  0x05F8   /* 2HDパラメータポインタ (DWORD, FAR PTR) */

/* ====================================================================== */
/*  メモリスイッチ (TVRAM 0xA3FE2-0xA3FFE)                                  */
/*  PC-98のTVRAMメモリスイッチは WORD単位 (偶数アドレスに1バイト)            */
/* ====================================================================== */
#define BDA_MEMSW1        0xA3FE2  /* bit6=25行, bit3=RS232C割り込み */
#define BDA_MEMSW2        0xA3FE6  /* クロック設定 */
#define BDA_MEMSW3        0xA3FEA  /* bit7=DIPスイッチ, bit2-0=CRT周波数 */
#define BDA_MEMSW4        0xA3FEE  /* 予約 */
#define BDA_MEMSW5        0xA3FF2  /* bit7-4=ブートデバイス */
#define BDA_MEMSW6        0xA3FF6  /* 予約 */

/* ====================================================================== */
/*  ブートパーティション                                                    */
/* ====================================================================== */
#define BDA_BOOT_PART     0x03FE   /* ブートパーティション スクラッチパッド (WORD) */

#endif /* V86_BDA_H */
