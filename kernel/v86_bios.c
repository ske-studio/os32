/* ======================================================================== */
/*  V86_BIOS.C — ゲスト BIOS の HLE                                         */
/* ======================================================================== */

#include "v86_bios.h"
#include "v86.h"
#include "kstring.h"
#include "loop_dev.h"
#include "v86_mem.h"
#include "paging.h"

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
static int disk_attached = 0;

u32 v86_bios_call_count(void)  { return bios_calls; }
u32 v86_bios_last_vector(void) { return bios_last_vec; }
int v86_bios_has_disk(void)    { return disk_attached; }

int v86_bios_attach_disk(const char *path)
{
    if (disk_attached) {
        v86_bios_detach_disk();
    }
    if (path == (const char *)0) {
        return 0;               /* ディスク無しで動かす */
    }
    if (loop_dev_attach(path, V86_DISK_SLOT) != 0) {
        return -1;
    }
    disk_attached = 1;
    return 0;
}

void v86_bios_detach_disk(void)
{
    if (disk_attached) {
        loop_dev_detach(V86_DISK_SLOT);
        disk_attached = 0;
    }
}

/* HLE する割り込みベクタ。ここに無いものは実機の IVT の値
 * (ほとんどが BIOS ROM) がそのままゲストに渡る。 */
static const u8 hle_vectors[] = {
    0x1B        /* ディスク BIOS */
};

void v86_bios_save_real(void)
{
    /* ページ 0 の状態に依存しないこと。
     *
     * paging.c は NULL ガードを R/O にしている (Not-Present だと LZ4 展開中の
     * BDA 参照で落ちるため、と経緯が残っている) が、外部プログラムを一度でも
     * 実行すると Not-Present に戻ってしまう実測がある。原因は exec 経路の
     * どこかで未特定。IVT の退避をその状態に賭けるわけにはいかないので、
     * 読む直前に自分で読める状態を作る。
     *
     * この後 v86_mem_setup() がページ 0 をバッキング RAM に張り替え、
     * teardown が R/O に戻すので、ここで R/O にしておくのが一貫している。 */
    paging_set_page(0, 0, PAGE_RO);
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
/*  PC-98 のレジスタ規約。前回プロジェクトが取り違えて詰まった箇所なので     */
/*  ここは特に慎重に:                                                       */
/*                                                                          */
/*    AH = コマンド (上位ビットはリトライ等の修飾。判定は下位ニブル)         */
/*    AL = DA/UA (装置番号)。0x90 = 2HD FDD#1                                */
/*    CL = シリンダ    CH = セクタ長コード (0..3 → 128<<N バイト)           */
/*    DH = ヘッド      DL = セクタ (1 起算)                                 */
/*    BX = 転送バイト数                                                     */
/*    ES:BP = 転送バッファ                                                  */
/*                                                                          */
/*  戻り: CF=0 成功 / CF=1 失敗、AH にステータス。                          */
/* ------------------------------------------------------------------------ */

/* ゲストのリニアアドレスがバッキング RAM の中に収まっているか。
 *
 * ES:BP は 16bit の掛け算なので理屈上 1MB 近くを指せるが、
 * 実際に張ってあるのは 0x00000-0x8EFFF だけ。範囲外を黙って書くと
 * カーネルを壊すので、必ず弾いて CF=1 で返す。 */
static int guest_range_ok(u32 linear, u32 len)
{
    if (len == 0) {
        return 0;
    }
    if (linear >= V86_REMAP_END) {
        return 0;
    }
    if (linear + len > V86_REMAP_END) {
        return 0;
    }
    return 1;
}

/* AH=04h SENSE — 装置の状態を返す。
 * ゲストは起動時にこれで「ドライブがあるか」を確かめる。 */
static void disk_sense(u32 *frame)
{
    if (!disk_attached) {
        bios_set_ah(frame, 0x60);       /* 装置レディでない */
        bios_set_cf(frame, 1);
        return;
    }
    bios_set_ah(frame, 0x00);
    bios_set_cf(frame, 0);
}

/* AH=06h READ DATA — CHS を進めながら BX バイト転送する。 */
static void disk_read(u32 *frame)
{
    u32 cyl   = (frame[V86F_ECX] >> 0) & 0xFFU;     /* CL */
    u32 n     = (frame[V86F_ECX] >> 8) & 0xFFU;     /* CH: セクタ長コード */
    u32 head  = (frame[V86F_EDX] >> 8) & 0xFFU;     /* DH */
    u32 sect  = (frame[V86F_EDX] >> 0) & 0xFFU;     /* DL (1 起算) */
    u32 count = frame[V86F_EBX] & 0xFFFFU;          /* BX: バイト数 */
    u32 dst   = ((frame[V86F_ES] & 0xFFFFU) << 4) +
                (frame[V86F_EBP] & 0xFFFFU);
    u16 cyls; u8 heads, spt; u16 bps; u32 total;
    u32 seclen;

    if (!disk_attached) {
        bios_set_ah(frame, 0x60);
        bios_set_cf(frame, 1);
        return;
    }
    if (!guest_range_ok(dst, count)) {
        bios_set_ah(frame, 0x40);       /* パラメータ異常 */
        bios_set_cf(frame, 1);
        return;
    }
    if (loop_dev_get_geometry(V86_DISK_SLOT, &cyls, &heads, &spt,
                              &bps, &total) != 0) {
        bios_set_ah(frame, 0x60);
        bios_set_cf(frame, 1);
        return;
    }

    /* 転送単位はイメージの実セクタ長に従う。
     * ゲストの CH (セクタ長コード) を鵜呑みにすると、実セクタの方が大きい
     * ときに loop_dev が要求バイト数を超えて書き込み、ゲストのメモリを
     * 壊す。CH は参考にとどめ、食い違ったら弾く。 */
    seclen = bps;
    if ((128UL << (n & 3)) != seclen) {
        bios_set_ah(frame, 0x40);       /* パラメータ異常 */
        bios_set_cf(frame, 1);
        return;
    }

    while (count >= seclen) {
        if (loop_dev_read_chs(V86_DISK_SLOT, (u16)cyl, (u8)head,
                              (u8)sect, (void *)dst) != 0) {
            bios_set_ah(frame, 0x40);   /* セクタ不在 */
            bios_set_cf(frame, 1);
            return;
        }
        dst   += seclen;
        count -= seclen;

        /* セクタ → ヘッド → シリンダ の順に繰り上げる */
        sect++;
        if (sect > spt) {
            sect = 1;
            head++;
            if (head >= heads) {
                head = 0;
                cyl++;
            }
        }
    }

    bios_set_ah(frame, 0x00);
    bios_set_cf(frame, 0);
}

static void bios_int1b(u32 *frame)
{
    u32 cmd = (frame[V86F_EAX] >> 8) & 0x0FU;   /* AH の下位ニブル */

    switch (cmd) {
    case V86_DISK_SENSE:
        disk_sense(frame);
        break;

    case V86_DISK_READ:
        disk_read(frame);
        break;

    case V86_DISK_SEEK:
    case V86_DISK_INIT:
    case V86_DISK_RECAL:
        /* 位置決め系。イメージ相手には実体が無いので成功を返す。
         * (Ys のコピープロテクトが SEEK の物理位置に依存するため、
         *  D88 の ID 照合が要るようになったら loop_dev_seek_d88 を使う) */
        bios_set_ah(frame, 0x00);
        bios_set_cf(frame, 0);
        break;

    default:
        /* WRITE と VERIFY は Phase 3-3b の後半で足す */
        bios_set_ah(frame, 0x86);
        bios_set_cf(frame, 1);
        break;
    }
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
