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
    u8 imr;       /* IMR (Interrupt Mask Register) */
    u8 isr;       /* ISR (In-Service Register) */
    u8 irr;       /* IRR (Interrupt Request Register) */
    u8 read_isr;  /* OCW3で「次のINはISRを返す」フラグ */
    u8 icw_state; /* ICWシーケンス状態: 0=通常, 1=ICW2待ち, 2=ICW3待ち, 3=ICW4待ち */
    u8 icw4_needed; /* ICW1 bit0: ICW4が必要か */
} vpic[2];  /* [0]=マスタ, [1]=スレーブ */

/* EOIデバッグカウンタ */
static u32 v86_eoi_count[2] = {0, 0};    /* 非特殊EOI回数 */
static u32 v86_seoi_count[2] = {0, 0};   /* 特殊EOI回数 */

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
        vpic[i].imr = 0xFF;      /* 全マスク: FreeDOSのPIC初期化(OUT 02h)で更新される */
        vpic[i].isr = 0x00;
        vpic[i].irr = 0x00;
        vpic[i].read_isr = 0;
        vpic[i].icw_state = 0;
        vpic[i].icw4_needed = 0;
    }
    v86_eoi_count[0] = v86_eoi_count[1] = 0;
    v86_seoi_count[0] = v86_seoi_count[1] = 0;
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
    /* ICW1 判定: bit4=1 → 初期化シーケンス開始 */
    if (val & 0x10) {
        vpic[idx].icw_state = 1;  /* 次のデータポート書き込みは ICW2 */
        vpic[idx].icw4_needed = (val & 0x01) ? 1 : 0;  /* bit0 = ICW4必要 */
        vpic[idx].isr = 0x00;     /* ISRクリア */
        vpic[idx].irr = 0x00;     /* IRRクリア */
        vpic[idx].read_isr = 0;
        return;
    }

    /* OCW2 判定: bit5=1, bit4-3=00 → EOIコマンド */
    if ((val & 0x18) == 0x00 && (val & 0x20)) {
        if (val & 0x40) {
            /* 特殊EOI (0x60+n): ISR bit n を直接クリア
             * PC9800Bible §1-4: OCW2 R=0,S=1,E=1 → 指定レベルEOI */
            int level = val & 0x07;
            vpic[idx].isr &= ~(1 << level);
            v86_seoi_count[idx]++;
        } else {
            /* 非特殊EOI (0x20): ISRの最高優先度ビットをクリア */
            if (vpic[idx].isr) {
                int bit;
                for (bit = 0; bit < 8; bit++) {
                    if (vpic[idx].isr & (1 << bit)) {
                        vpic[idx].isr &= ~(1 << bit);
                        v86_eoi_count[idx]++;
                        break;
                    }
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

    /* その他のコマンド: 無視 */
}

/* ====================================================================== */
/*  pic_write_data — PICデータポート (02h/0Ah) への書き込み処理            */
/*                                                                          */
/*  ICWシーケンス中はICW2/3/4として処理し、IMRとして扱わない。             */
/*  ICWシーケンス完了後はIMRとして設定。                                   */
/* ====================================================================== */
static void pic_write_data(int idx, u8 val)
{
    switch (vpic[idx].icw_state) {
    case 1:  /* ICW2: ベクタベース (無視 — V86ではベクタ固定) */
        vpic[idx].icw_state = 2;
        break;
    case 2:  /* ICW3: カスケード接続 (無視) */
        if (vpic[idx].icw4_needed) {
            vpic[idx].icw_state = 3;
        } else {
            vpic[idx].icw_state = 0;  /* ICWシーケンス完了 */
        }
        break;
    case 3:  /* ICW4: 動作モード (無視) */
        vpic[idx].icw_state = 0;  /* ICWシーケンス完了 */
        break;
    default: /* ICWシーケンス外 → IMR設定 */
        vpic[idx].imr = val;
        break;
    }
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
            pic_write_data(0, *val);
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
            pic_write_data(1, *val);
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

/* ====================================================================== */
/*  仮想PICの状態への直接アクセス (タイマ割り込み注入等で使用)               */
/* ====================================================================== */
u8 v86_pic_get_imr(int idx)
{
    return vpic[idx & 1].imr;
}

u8 v86_pic_get_isr(int idx)
{
    return vpic[idx & 1].isr;
}

void v86_pic_set_isr(int idx, u8 val)
{
    vpic[idx & 1].isr = val;
}

u32 v86_pic_get_eoi_count(int idx)
{
    return v86_eoi_count[idx & 1] + v86_seoi_count[idx & 1];
}

/* ====================================================================== */
/*  §4 IRR 直接アクセス (v86_set_pending_irq から呼ばれる)                  */
/*  ゲストが OCW3=0x0A で IRR を読んだとき正しいビットが返るようにする。      */
/* ====================================================================== */
void v86_pic_set_irr(int idx, u8 val)
{
    vpic[idx & 1].irr = val;
}

u8 v86_pic_get_irr(int idx)
{
    return vpic[idx & 1].irr;
}
