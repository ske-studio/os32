/* ======================================================================== */
/*  V86_DISK.H — V86 ディスクBIOS (INT 1Bh) 仮想化ヘッダ                   */
/*                                                                          */
/*  FDDイメージをメモリ上に保持し、INT 1Bh のセクタ読み書きを                */
/*  イメージデータからサーブする。                                           */
/* ======================================================================== */

#ifndef V86_DISK_H
#define V86_DISK_H

#include "types.h"

/* PC-98 2HD FDD ジオメトリ */
#define V86_FDD_CYLINDERS   77
#define V86_FDD_HEADS       2
#define V86_FDD_SPT         8       /* セクタ/トラック */
#define V86_FDD_BPS         1024    /* バイト/セクタ */
#define V86_FDD_TOTAL_SEC   (V86_FDD_CYLINDERS * V86_FDD_HEADS * V86_FDD_SPT)
#define V86_FDD_IMAGE_SIZE  (V86_FDD_TOTAL_SEC * V86_FDD_BPS)

/* DA/UA: 1MB FDD UNIT#0 */
#define V86_FDD_DAUA        0x90

/* FDDイメージをセット (dataはイメージ全体のポインタ、sizeはバイト数)
 * 呼び出し後、V86からのINT 1Bhでこのイメージのセクタが返される。 */
void v86_disk_set_image(const u8 *data, u32 size);

/* FDDイメージをクリア */
void v86_disk_clear(void);

/* INT 1Bh (ディスクBIOS) を処理する。
 * regs: V86スタックフレーム内レジスタ配列
 * 戻り値: 0=処理済み(V86続行), -1=未実装ファンクション */
int v86_bios_int1b(u32 *regs);

#endif /* V86_DISK_H */
