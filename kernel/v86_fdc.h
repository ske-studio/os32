/* ======================================================================== */
/*  V86_FDC.H — V86 FDC (µPD765A) ポートレベル仮想化                       */
/*                                                                          */
/*  PC-98 FDC ポート:                                                       */
/*    0x90: MSR (メインステータスレジスタ, R)                               */
/*    0x92: FIFO (データレジスタ, R/W)                                      */
/*    0x94: CTRL (コントロールレジスタ, W) / リードスイッチ (R)            */
/* ======================================================================== */

#ifndef V86_FDC_H
#define V86_FDC_H

#include "types.h"

/* v86_fdc_virt_init — 仮想FDCの初期化 */
void v86_fdc_virt_init(void);

/* v86_fdc_io — FDC I/Oポートハンドラ
 * 戻り値: 1=処理済み, 0=非対象ポート */
int v86_fdc_io(u16 port, u8 *val, int is_write);

/* v86_fdc_sync_rw — INT 1Bh READ/WRITE HLE後のFDC同期
 * NP21/W fdcsend_success7() 相当。
 * FDCを BUFSEND (MSR=RQM|DIO|CB) に設定し、リザルト7バイトを
 * バッファに格納。ゲストが0x92ポートで読み取ると自動でIDLEに遷移。
 * IRQ11もペンディングする。 */
void v86_fdc_sync_rw(u8 cyl, u8 head, u8 sect_r, u8 sec_n);

/* v86_fdc_sync_seek — INT 1Bh SEEK/RECALIBRATE HLE後のFDC同期
 * NP21/W bios_fdresult(FDCBIOS_SEEKSUCCESS) 相当。
 * FDCを NEUTRAL (MSR=RQM) に設定し、SENSE INTERRUPTで
 * ST0(SE)+PCNを返す準備をする。IRQ11もペンディングする。 */
void v86_fdc_sync_seek(u8 cyl);

/* v86_fdc_get_msr_log — diagダンプ用: MSRリードログを取得 */
void v86_fdc_get_msr_log(u8 *out_status, u8 *out_phase,
                          u32 *out_total, u32 *out_idx);

/* v86_fdc_get_state — 現在のFDC状態を返す */
void v86_fdc_get_state(u8 *out_status, u8 *out_phase, u8 *out_drv);

#endif /* V86_FDC_H */
