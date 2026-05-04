/* ======================================================================== */
/*  V86_BDA.H - PC-98 BDA (BIOS Data Area) 定数定義                         */
/*                                                                          */
/*  V86サブシステム内で参照される BDA オフセット定数を集約する。              */
/*  出典: PC9800Bible, UNDOCUMENTED memsys.md                               */
/* ======================================================================== */

#ifndef V86_BDA_H
#define V86_BDA_H

/* ====================================================================== */
/*  キーボードバッファ (NP21/W bios09.c 準拠)                               */
/* ====================================================================== */
#define BDA_KB_BUF_START  0x0502   /* バッファ先頭 (16エントリ x 2バイト) */
#define BDA_KB_BUF_END    0x0522   /* バッファ末尾 */
#define BDA_KB_HEAD       0x0524   /* 取出ポインタ (消費側: DOS が進める, WORD) */
#define BDA_KB_TAIL       0x0526   /* 入力ポインタ (生産側: kbd/auto-typerが進める, WORD) */
#define BDA_KB_COUNT      0x0528   /* バッファ内キー数 (BYTE) */

/* ====================================================================== */
/*  メモリ・システム情報                                                    */
/* ====================================================================== */
#define BDA_BIOS_FLAG2    0x0400   /* 機種フラグ */
#define BDA_EXPMMSZ       0x0401   /* 拡張メモリサイズ (未使用) */
#define BDA_MEM_SIZE      0x0413   /* コンベンショナルメモリサイズ (WORD, KB) */
#define BDA_BIOS_FLAG5    0x0458   /* NESA/WAIT */
#define BDA_CPU_FLAG      0x0480   /* CPU種別フラグ */
#define BDA_CPU_TYPE      0x0484   /* CPUタイプ (03=i386以上) */
#define BDA_SCSI_HD       0x0482   /* SCSI HD接続状態 */
#define BDA_GRCG          0x0495   /* GRAPH_CHG: GRCG状態 */
#define BDA_TILE_REG      0x0496   /* GRAPH_TAL[0-3]: タイルレジスタ */
#define BDA_BIOS_FLAG     0x0501   /* BIOS_FLAG (クロック/CPU/メモリ) */
#define BDA_CRT_STS       0x053C   /* CRT_STS_FLAG */
#define BDA_DISK_EQUIP    0x055C   /* ディスク接続状態 (WORD) */
#define BDA_SASI_IDE      0x055D   /* SASI/IDE HDD接続情報 */
#define BDA_FDC_RESULT    0x0564   /* FDC結果バッファ (8バイト × ユニット) */
#define BDA_BOOT_DEV      0x0584   /* ブートデバイス DA/UA */
#define BDA_CONV_MEM      0x05AE   /* コンベンショナルメモリ (x4KB) */

/* ====================================================================== */
/*  ブートパーティション                                                    */
/* ====================================================================== */
#define BDA_BOOT_PART     0x03FE   /* ブートパーティション スクラッチパッド (WORD) */

#endif /* V86_BDA_H */
