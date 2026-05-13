/* ======================================================================== */
/*  V86_DMA.H — V86 DMA (µPD8237A) ch2 仮想化                              */
/*                                                                          */
/*  FDC が DMA ch2 経由でデータ転送するため、DMA コントローラの             */
/*  仮想化が必要。実 DMA には一切アクセスしない。                           */
/*                                                                          */
/*  対象ポート:                                                             */
/*    0x09: ch2 アドレス (Low/High)                                         */
/*    0x0B: ch2 ワードカウント (Low/High)                                   */
/*    0x15: シングルマスクレジスタ                                          */
/*    0x17: モードレジスタ                                                  */
/*    0x19: フリップフロップクリア                                          */
/*    0x23: ch2 バンクレジスタ                                              */
/* ======================================================================== */

#ifndef V86_DMA_H
#define V86_DMA_H

#include "types.h"

/* v86_dma_init — 仮想DMAの初期化 */
void v86_dma_init(void);

/* v86_dma_io — DMA I/Oポートハンドラ
 * 戻り値: 1=処理済み, 0=非対象ポート */
int v86_dma_io(u16 port, u8 *val, int is_write);

/* v86_dma_get_transfer — DMA転送パラメータ取得
 * 戻り値: V86メモリ空間内の転送先/元バッファポインタ (失敗時 NULL)
 * out_bytes: 転送バイト数 */
u8 *v86_dma_get_transfer(u32 *out_bytes);

/* T3.3: DMA 転送ログ */
#define V86_DMA_LOG_SIZE 32

struct v86_dma_entry {
    u32 tick;
    u8  ch;        /* DMA チャネル (2=FDC) */
    u8  mode;      /* モードレジスタ */
    u32 phys_addr; /* 24bit DMA アドレス */
    u16 count;     /* ワードカウント */
    u8  trigger;   /* 1=FDC READ, 2=FDC WRITE, 3=HDD */
};

extern struct v86_dma_entry v86_dma_log[];
extern u32 v86_dma_log_idx;
extern u32 v86_dma_log_count;

#endif /* V86_DMA_H */
