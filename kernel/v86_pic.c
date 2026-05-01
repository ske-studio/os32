/* ======================================================================== */
/*  V86_PIC.C — V86 PIC (8259A) 仮想化                                     */
/*                                                                          */
/*  PC-98のPICレイアウト (UNDOCUMENTED io_pic.md):                          */
/*    マスタPIC: ポート 0x00 (CMD), 0x02 (IMR/DATA)                        */
/*    スレーブPIC: ポート 0x08 (CMD), 0x0A (IMR/DATA)                      */
/*                                                                          */
/*  仮想化方針:                                                            */
/*    - DOS (V86) からの PIC 操作は全て仮想レジスタに保存                  */
/*    - 実PICには一切アクセスしない (OS32のPIC設定を保護)                   */
/*    - EOI: 仮想ISRからサービスビットをクリア                              */
/*    - IMR: 仮想IMRに保存し、INで返す                                     */
/*    - IRR/ISR読み出し: OCW3コマンドで切り替えて仮想レジスタを返す         */
/*                                                                          */
/*  リセットポート (0xF0):                                                  */
/*    OUT F0h への書き込みはCPUリセット命令。                               */
/*    V86モニタがこれを検知して V86→OS32 復帰トリガーとする。               */
/* ======================================================================== */

#include "v86_pic.h"
#include "io.h"

/* ====================================================================== */
/*  仮想PICレジスタ                                                        */
/* ====================================================================== */
static struct {
    u8 imr;     /* IMR (Interrupt Mask Register) */
    u8 isr;     /* ISR (In-Service Register) */
    u8 irr;     /* IRR (Interrupt Request Register) */
    u8 read_isr; /* OCW3で「次のINはISRを返す」フラグ */
} vpic[2];  /* [0]=マスタ, [1]=スレーブ */

/* PICポートアドレス */
#define MPIC_CMD   0x00   /* マスタ コマンドポート */
#define MPIC_DATA  0x02   /* マスタ データ/IMRポート */
#define SPIC_CMD   0x08   /* スレーブ コマンドポート */
#define SPIC_DATA  0x0A   /* スレーブ データ/IMRポート */

/* リセットポート */
#define RESET_PORT 0xF0

/* ====================================================================== */
/*  v86_pic_init — 仮想PICの初期化                                        */
/* ====================================================================== */
void v86_pic_init(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        vpic[i].imr = 0xFF;      /* 全マスク (DOS初期値) */
        vpic[i].isr = 0x00;
        vpic[i].irr = 0x00;
        vpic[i].read_isr = 0;
    }
}

/* ====================================================================== */
/*  pic_write_cmd — PICコマンドポート (00h/08h) への書き込み処理           */
/*                                                                          */
/*  OCW2 (0x20 = 非特殊EOI):                                              */
/*    ISRの最上位ビットをクリア                                            */
/*  OCW3 (0x0B = ISR読み出しモード, 0x0A = IRR読み出しモード):             */
/*    次のINコマンドで返すレジスタを切り替え                                */
/* ====================================================================== */
static void pic_write_cmd(int idx, u8 val)
{
    /* OCW2 判定: bit5=1, bit4-3=00 → EOIコマンド */
    if ((val & 0x18) == 0x00 && (val & 0x20)) {
        /* 非特殊EOI (0x20): ISRの最上位ビットをクリア */
        if (vpic[idx].isr) {
            int bit;
            for (bit = 0; bit < 8; bit++) {
                if (vpic[idx].isr & (1 << bit)) {
                    vpic[idx].isr &= ~(1 << bit);
                    break;
                }
            }
        }
        return;
    }

    /* OCW3 判定: bit4=0, bit3=1 */
    if ((val & 0x18) == 0x08) {
        if (val & 0x02) {
            /* bit1=1: 読み出しレジスタ選択 */
            vpic[idx].read_isr = (val & 0x01) ? 1 : 0;
        }
        return;
    }

    /* ICW1 (bit4=1): 初期化シーケンス開始 — ackのみ */
    /* その他のコマンド: 無視 */
}

/* ====================================================================== */
/*  pic_read_cmd — PICコマンドポート (00h/08h) からの読み出し              */
/* ====================================================================== */
static u8 pic_read_cmd(int idx)
{
    if (vpic[idx].read_isr) {
        return vpic[idx].isr;
    }
    return vpic[idx].irr;
}

/* ====================================================================== */
/*  v86_pic_io — PIC I/Oポートハンドラ                                    */
/*                                                                          */
/*  戻り値: 1=処理済み, 0=非対象ポート                                     */
/* ====================================================================== */
int v86_pic_io(u16 port, u8 *val, int is_write)
{
    switch (port) {
    /* ---- マスタPIC コマンドポート (0x00) ---- */
    case MPIC_CMD:
        if (is_write) {
            pic_write_cmd(0, *val);
        } else {
            *val = pic_read_cmd(0);
        }
        return 1;

    /* ---- マスタPIC データ/IMRポート (0x02) ---- */
    case MPIC_DATA:
        if (is_write) {
            vpic[0].imr = *val;
        } else {
            *val = vpic[0].imr;
        }
        return 1;

    /* ---- スレーブPIC コマンドポート (0x08) ---- */
    case SPIC_CMD:
        if (is_write) {
            pic_write_cmd(1, *val);
        } else {
            *val = pic_read_cmd(1);
        }
        return 1;

    /* ---- スレーブPIC データ/IMRポート (0x0A) ---- */
    case SPIC_DATA:
        if (is_write) {
            vpic[1].imr = *val;
        } else {
            *val = vpic[1].imr;
        }
        return 1;

    default:
        return 0;
    }
}

/* ====================================================================== */
/*  v86_pic_is_reboot — リセットポート検知                                 */
/*                                                                          */
/*  PC-98: OUT F0h への書き込みでCPUリセット                               */
/*  出典: PC9800Bible §4-3 #31                                            */
/* ====================================================================== */
int v86_pic_is_reboot(u16 port, u8 val)
{
    (void)val;
    return (port == RESET_PORT) ? 1 : 0;
}
