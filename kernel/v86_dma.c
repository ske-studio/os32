/* ======================================================================== */
/*  V86_DMA.C — V86 DMA (µPD8237A) 全4チャネル仮想化                       */
/*                                                                          */
/*  BIOS ROM初期化が全チャネルを操作するため、ch0-3 を全て仮想化する。      */
/*  実 DMA には一切アクセスしない (OS32のDMA設定を保護する)。               */
/*                                                                          */
/*  PC-98 DMA ポートアドレス (NP21/W io/dmac.c 準拠):                       */
/*    ch0: 0x01(addr) 0x03(count)  bank: 0x27                               */
/*    ch1: 0x05(addr) 0x07(count)  bank: 0x21                               */
/*    ch2: 0x09(addr) 0x0B(count)  bank: 0x23                               */
/*    ch3: 0x0D(addr) 0x0F(count)  bank: 0x25                               */
/*    制御: 0x11(SWリクエスト) 0x13(ステータス/コマンド)                     */
/*          0x15(シングルマスク) 0x17(モード) 0x19(FF CLR)                   */
/*          0x1B(マスタクリア) 0x1D(全マスクリセット) 0x1F(全マスク)         */
/*                                                                          */
/*  出典: PC9800Bible §2-9, §1-5                                            */
/* ======================================================================== */

#include "v86_dma.h"
#include "v86_mem.h"

/* ====================================================================== */
/*  仮想 DMA レジスタ (全4チャネル)                                         */
/* ====================================================================== */
static struct {
    u8  flipflop;   /* Low/High 切替フラグ (0=Low次, 1=High次) */
    struct {
        u16 addr;   /* ベースアドレス (16bit) */
        u16 count;  /* ワードカウント */
        u8  bank;   /* バンクレジスタ (上位8bit) */
        u8  mode;   /* チャネルモード */
    } ch[4];
    u8  mask;       /* 全チャネル分マスクビット (bit0-3) */
    u8  status;     /* ステータスレジスタ */
    u8  cmd;        /* コマンドレジスタ */
} vdma;

/* ====================================================================== */
/*  v86_dma_init — 仮想DMA初期化                                           */
/* ====================================================================== */
void v86_dma_init(void)
{
    int i;
    vdma.flipflop = 0;
    vdma.mask     = 0x0F; /* 全チャネルマスク (起動直後) */
    vdma.status   = 0;
    vdma.cmd      = 0;
    for (i = 0; i < 4; i++) {
        vdma.ch[i].addr  = 0;
        vdma.ch[i].count = 0;
        vdma.ch[i].bank  = 0;
        vdma.ch[i].mode  = 0;
    }
}

/* ====================================================================== */
/*  ポート → チャネル番号変換                                               */
/*  PC-98 DMA: addr=port>>2 & 3 (0x01→ch0, 0x05→ch1, 0x09→ch2, 0x0D→ch3) */
/* ====================================================================== */
static int port_to_ch_addr(u16 port)
{
    switch (port) {
    case 0x01: return 0;
    case 0x05: return 1;
    case 0x09: return 2;
    case 0x0D: return 3;
    default:   return -1;
    }
}

static int port_to_ch_count(u16 port)
{
    switch (port) {
    case 0x03: return 0;
    case 0x07: return 1;
    case 0x0B: return 2;
    case 0x0F: return 3;
    default:   return -1;
    }
}

/* NP21/W dmac_o21: バンクレジスタのポート→ch変換
 * 0x21→ch1, 0x23→ch2, 0x25→ch3, 0x27→ch0 */
static int port_to_ch_bank(u16 port)
{
    switch (port) {
    case 0x27: return 0;
    case 0x21: return 1;
    case 0x23: return 2;
    case 0x25: return 3;
    default:   return -1;
    }
}

/* ====================================================================== */
/*  v86_dma_io — DMA I/Oポートハンドラ (全4チャネル対応)                   */
/*                                                                          */
/*  戻り値: 1=処理済み, 0=非対象ポート                                     */
/* ====================================================================== */
int v86_dma_io(u16 port, u8 *val, int is_write)
{
    int ch;

    /* ---------------------------------------------------------------- */
    /*  チャネルアドレスレジスタ (0x01/0x05/0x09/0x0D)                  */
    /* ---------------------------------------------------------------- */
    ch = port_to_ch_addr(port);
    if (ch >= 0) {
        if (is_write) {
            if (vdma.flipflop == 0) {
                vdma.ch[ch].addr = (vdma.ch[ch].addr & 0xFF00U) | *val;
                vdma.flipflop = 1;
            } else {
                vdma.ch[ch].addr = (vdma.ch[ch].addr & 0x00FFU) | ((u16)*val << 8);
                vdma.flipflop = 0;
            }
        } else {
            if (vdma.flipflop == 0) {
                *val = (u8)(vdma.ch[ch].addr & 0xFF);
                vdma.flipflop = 1;
            } else {
                *val = (u8)(vdma.ch[ch].addr >> 8);
                vdma.flipflop = 0;
            }
        }
        return 1;
    }

    /* ---------------------------------------------------------------- */
    /*  チャネルワードカウントレジスタ (0x03/0x07/0x0B/0x0F)            */
    /* ---------------------------------------------------------------- */
    ch = port_to_ch_count(port);
    if (ch >= 0) {
        if (is_write) {
            if (vdma.flipflop == 0) {
                vdma.ch[ch].count = (vdma.ch[ch].count & 0xFF00U) | *val;
                vdma.flipflop = 1;
            } else {
                vdma.ch[ch].count = (vdma.ch[ch].count & 0x00FFU) | ((u16)*val << 8);
                vdma.flipflop = 0;
            }
        } else {
            if (vdma.flipflop == 0) {
                *val = (u8)(vdma.ch[ch].count & 0xFF);
                vdma.flipflop = 1;
            } else {
                *val = (u8)(vdma.ch[ch].count >> 8);
                vdma.flipflop = 0;
            }
        }
        return 1;
    }

    /* ---------------------------------------------------------------- */
    /*  バンクレジスタ (0x21/0x23/0x25/0x27)                            */
    /* ---------------------------------------------------------------- */
    ch = port_to_ch_bank(port);
    if (ch >= 0) {
        if (is_write) {
            vdma.ch[ch].bank = *val;
        } else {
            *val = vdma.ch[ch].bank;
        }
        return 1;
    }

    /* ---------------------------------------------------------------- */
    /*  制御レジスタ                                                    */
    /* ---------------------------------------------------------------- */
    switch (port) {

    /* 0x11: ソフトウェアリクエスト (NP21/W dmac_o13) */
    case 0x11:
        if (is_write) {
            /* NOP: HLE方式では不要 */
        } else {
            *val = 0;
        }
        return 1;

    /* 0x13: ステータス/コマンドレジスタ (NP21/W dmac_o13_) */
    case 0x13:
        if (is_write) {
            vdma.cmd = *val;
        } else {
            *val = vdma.status;
        }
        return 1;

    /* 0x15: シングルマスクレジスタ */
    case 0x15:
        if (is_write) {
            /* NP21/W dmac_o15: bit2=set/clear, bit1-0=ch */
            if (*val & 4) {
                vdma.mask |= (u8)(1 << (*val & 3));
            } else {
                vdma.mask &= (u8)~(1 << (*val & 3));
            }
        } else {
            *val = vdma.mask;
        }
        return 1;

    /* 0x17: モードレジスタ (書き込みのみ) */
    case 0x17:
        if (is_write) {
            vdma.ch[*val & 3].mode = *val;
        } else {
            *val = 0;
        }
        return 1;

    /* 0x19: フリップフロップクリア */
    case 0x19:
        vdma.flipflop = 0;
        if (!is_write) *val = 0;
        return 1;

    /* 0x1B: マスタクリア (NP21/W dmac_o1b) */
    case 0x1B:
        if (is_write) {
            int i;
            vdma.mask = 0x0F;
            vdma.flipflop = 0;
            for (i = 0; i < 4; i++) {
                vdma.ch[i].addr  = 0;
                vdma.ch[i].count = 0;
            }
        }
        if (!is_write) *val = 0;
        return 1;

    /* 0x1D: 全マスクリセット (NP21/W dmac_o1d) */
    case 0x1D:
        if (is_write) {
            vdma.mask = 0;
        }
        if (!is_write) *val = 0;
        return 1;

    /* 0x1F: 全マスク書込み (NP21/W dmac_o1f) */
    case 0x1F:
        if (is_write) {
            vdma.mask = *val & 0x0F;
        } else {
            *val = vdma.mask;
        }
        return 1;

    default:
        return 0;
    }
}

/* ====================================================================== */
/*  v86_dma_get_transfer — DMA ch2 転送パラメータ取得                     */
/*                                                                          */
/*  FDC HLE から呼ばれる。ch2 の仮想レジスタから転送先アドレスと           */
/*  バイト数を計算し、V86メモリ空間内のバッファポインタを返す。             */
/* ====================================================================== */
extern volatile u32 tick_count;

u8 *v86_dma_get_transfer(u32 *out_bytes)
{
    u32 linear;
    u16 seg;
    u16 off;
    u8 *result;

    /* カウント+1 がバイト数 (DMA は count-1 を格納する慣例) */
    *out_bytes = (u32)vdma.ch[2].count + 1;

    /* 24bitアドレス: bank << 16 | addr */
    linear = ((u32)vdma.ch[2].bank << 16) | vdma.ch[2].addr;
    linear &= 0xFFFFFUL; /* 1MB境界でクリップ */

    /* seg:off に分解して v86_phys_addr() でバッキングRAMを参照 */
    seg = (u16)(linear >> 4);
    off = (u16)(linear & 0x0F);

    result = v86_phys_addr(seg, off);

    /* T3.3: DMA 転送ログを記録 */
    {
        struct v86_dma_entry *e = &v86_dma_log[v86_dma_log_idx % V86_DMA_LOG_SIZE];
        e->tick = tick_count;
        e->ch = 2;
        e->mode = vdma.ch[2].mode;
        e->phys_addr = linear;
        e->count = vdma.ch[2].count;
        e->trigger = 1;
        v86_dma_log_idx++;
        v86_dma_log_count++;
    }

    return result;
}

/* ====================================================================== */

struct v86_dma_entry v86_dma_log[V86_DMA_LOG_SIZE];
u32 v86_dma_log_idx = 0;
u32 v86_dma_log_count = 0;
