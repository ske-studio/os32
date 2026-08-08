/* ======================================================================== */
/*  V86_BIOS.C — ゲスト BIOS の HLE                                         */
/* ======================================================================== */

#include "v86_bios.h"
#include "v86.h"
#include "kstring.h"

/* 実機の IVT (1KB) と BDA (0x400-0x5FF) の退避先。
 *
 * バッキング RAM を張るとゲストから見た低位メモリは真っさらになるので、
 * 実機の値をそのまま渡してやらないとゲストは何も判断できない。
 * ROM を指す割り込みベクタもここから引き継ぐ。 */
#define REAL_SNAPSHOT_SIZE  0x600
static u8 real_lowmem[REAL_SNAPSHOT_SIZE];
static int real_saved = 0;

static u32 bios_calls = 0;
static u32 bios_last_vec = 0;

u32 v86_bios_call_count(void)  { return bios_calls; }
u32 v86_bios_last_vector(void) { return bios_last_vec; }

/* HLE する割り込みベクタ。ここに無いものは実機の IVT の値
 * (ほとんどが BIOS ROM) がそのままゲストに渡る。 */
static const u8 hle_vectors[] = {
    0x1B        /* ディスク BIOS */
};

void v86_bios_save_real(void)
{
    kmemcpy(real_lowmem, (const void *)0, REAL_SNAPSHOT_SIZE);
    real_saved = 1;
}

void v86_bios_setup(void)
{
    u8 *guest = (u8 *)0;        /* リマップ済みのゲスト低位メモリ */

    /* 実機の IVT / BDA をゲストへ引き継ぐ。
     * ROM を指す割り込みベクタもここから受け継ぐので、HLE しないコールは
     * そのまま実 BIOS ROM が処理する。 */
    if (real_saved) {
        kmemcpy(guest, real_lowmem, REAL_SNAPSHOT_SIZE);
    }

    /* ゲストに見せるメモリ量をバッキング RAM の範囲に合わせる。
     *   0x0413  MEM_SIZE   コンベンショナルメモリ (KB)
     *   0x05AE  CONV_MEM   同 (4KB 単位)
     */
    *(u16 *)(guest + 0x0413) = (u16)V86_GUEST_MEM_KB;
    *(u8  *)(guest + 0x05AE) = (u8)(V86_GUEST_MEM_KB / 4);

    bios_calls = 0;
    bios_last_vec = 0;
}

int v86_bios_is_hle(u32 vector)
{
    u32 i;
    for (i = 0; i < sizeof(hle_vectors); i++) {
        if (hle_vectors[i] == vector) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  戻り値の受け渡し                                                        */
/*                                                                          */
/*  INT は IDT ゲートの DPL チェックで配送前に落ちているので、ゲスト        */
/*  スタックには何も積まれていない。したがって BIOS の成否 (CF) は          */
/*  フレームの EFLAGS に直接書く。#GP から復帰する iretd がそれを            */
/*  ゲストに戻す。                                                          */
/* ------------------------------------------------------------------------ */
static void bios_set_cf(u32 *frame, int cf)
{
    if (cf) {
        frame[V86F_EFLAGS] |= 0x0001UL;
    } else {
        frame[V86F_EFLAGS] &= ~0x0001UL;
    }
}

static void bios_set_ah(u32 *frame, u32 ah)
{
    frame[V86F_EAX] = (frame[V86F_EAX] & 0xFFFF00FFUL) | ((ah & 0xFFU) << 8);
}

/* ------------------------------------------------------------------------ */
/*  INT 1Bh — ディスク BIOS                                                 */
/*                                                                          */
/*  PC-98 のレジスタ規約 (前回プロジェクトが一度取り違えて詰まった箇所):     */
/*    AH = コマンド, AL = DA/UA (装置番号)                                   */
/*    CL = シリンダ, CH = セクタ長コード, DH = ヘッド, DL = セクタ           */
/*    BX = バイト数, ES:BP = 転送バッファ                                    */
/*                                                                          */
/*  実際の読み書きは Phase 3-3b で loop_dev に繋ぐ。今はまだ                */
/*  「装置なし」を返すだけ。                                                */
/* ------------------------------------------------------------------------ */
static void bios_int1b(u32 *frame)
{
    bios_set_ah(frame, 0x00);
    bios_set_cf(frame, 0);
}

void v86_bios_dispatch(u32 *frame, u32 vector)
{
    bios_calls++;
    bios_last_vec = vector;

    switch (vector) {
    case 0x1B:
        bios_int1b(frame);
        break;

    default:
        /* 未対応の BIOS コールは CF=1 / AH=0x86 (機能なし) を返す。
         * 落とさずに返せば、ゲストは自前のフォールバックに進める。 */
        bios_set_ah(frame, 0x86);
        bios_set_cf(frame, 1);
        break;
    }
}
