/* ======================================================================== */
/*  V86_DOS_REF.H - MS-DOS BDA 期待値テンプレート (T2.5)                    */
/*                                                                          */
/*  NP21/W で MS-DOS 6.2 を直接ブートして採取した BDA リファレンスデータと  */
/*  比較マスクを提供する。                                                  */
/* ======================================================================== */

#ifndef V86_DOS_REF_H
#define V86_DOS_REF_H

#include "types.h"

/* リファレンス BDA データ (512バイト: 0x0400-0x05FF) */
extern const u8 v86_dos_ref_bda[512];

/* 比較マスク (0xFF=比較対象, 0x00=無視) */
extern const u8 v86_dos_ref_bda_mask[512];

/* 現在の BDA とリファレンスを比較し、差分レポートを出力する */
void v86_debug_compare_bda_to_ref(void);

#endif /* V86_DOS_REF_H */
