/* ======================================================================== */
/*  V86_BIOS.C — ゲスト BIOS の HLE                                         */
/* ======================================================================== */

#include "v86_bios.h"
#include "v86.h"
#include "kstring.h"
#include "loop_dev.h"
#include "v86_mem.h"
#include "paging.h"
#include "vfs.h"

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

/* 失敗した INT 1Bh の中身。推測でなく事実で切り分けるために残す。
 * static にすると kernel.map に出ずホストから読めないのでグローバル。 */
u32 v86_disk_fail_n   = 0;      /* 失敗回数 */
u32 v86_disk_fail_why = 0;      /* 1=範囲外 2=セクタ長 3=セクタ不在 4=装置なし */
u32 v86_disk_fail_chs = 0;      /* cyl<<16 | head<<8 | sect */
u32 v86_disk_fail_len = 0;      /* n<<16 | count */
u32 v86_disk_fail_dst = 0;      /* ES:BP のリニアアドレス */

/* SEEK と ID ずれの実測用。§「物理位置と論理 ID の分離」を参照。 */
u32 v86_disk_seek_n   = 0;      /* SEEK / RECALIBRATE の回数 */
u32 v86_disk_seek_cyl = 0;      /* 最後に SEEK した物理シリンダ */
u32 v86_disk_shift_n  = 0;      /* 物理≠論理のまま読めた回数 */
u32 v86_disk_ident_n  = 0;      /* 物理=論理 (フォールバック) で読めた回数 */

/* INT 1Bh 呼び出しログ。
 *
 * カウンタだけでは「何回失敗したか」は分かっても「どういう順で何を
 * 要求されたか」が分からず、推測が始まってしまう。1 コール 8 ワードの
 * リングにそのまま積んでおけば、ホストから emu_read_mem 一発で
 * 呼び出し列を復元できる。 */
#define V86_DISKLOG_N   32
u32 v86_disklog_w = 0;                          /* 積んだエントリ数 */
u32 v86_disklog[V86_DISKLOG_N * 8];
/*  [0] AX  [1] CX  [2] DX  [3] BX
 *  [4] ES:BP のリニアアドレス
 *  [5] 呼び出し時の物理位置 (head<<8 | cyl)
 *  [6] 転送できたバイト数
 *  [7] 連続回数<<8 | 結果 (0=成功 / v86_disk_fail_why と同じ値) */

/* 直近 4 エントリのどれかと一致したら積まずに回数だけ数える。
 *
 * **これが無いとログが役に立たない。** ゲストは失敗したコマンドを
 * 何万回でもリトライするので、32 エントリのリングが一瞬で埋まり、
 * 「そこへ至る直前に何をしていたか」が消えてしまう。
 * I/O トラップログ (kernel/v86_io.c) も同じ理由で同じ作りにしてある。
 *
 * **「直前と同じなら畳む」では足りない。** ゲストは失敗したコマンドを
 * SEEK と READ の**組で**何万回もリトライするので、直前だけ見ていると
 * 2 種類が交互に積まれてリングが一瞬で埋まり、そこへ至る履歴が消える。
 * 周期 2〜4 のループを畳めるようにしておけば、最初の失敗もその後の
 * 定常ループも同じログの中に残る。 */
#define DISKLOG_LOOKBACK    4

static void disklog(u32 *frame, u32 ax_in, u32 phys, u32 done, u32 result)
{
    u32 rec[7];
    u32 *e;
    u32 back;
    int i;

    /* AX は処理の中で AH (ステータス) を書き換えるので、
     * 呼び出し時点の値を別に受け取る。 */
    rec[0] = ax_in;
    rec[1] = frame[V86F_ECX] & 0xFFFFU;
    rec[2] = frame[V86F_EDX] & 0xFFFFU;
    rec[3] = frame[V86F_EBX] & 0xFFFFU;
    rec[4] = ((frame[V86F_ES] & 0xFFFFU) << 4) + (frame[V86F_EBP] & 0xFFFFU);
    rec[5] = phys;
    rec[6] = done;

    for (back = 1; back <= DISKLOG_LOOKBACK && back <= v86_disklog_w; back++) {
        e = &v86_disklog[((v86_disklog_w - back) % V86_DISKLOG_N) * 8];
        if ((e[7] & 0xFFU) != (result & 0xFFU)) {
            continue;
        }
        for (i = 0; i < 7; i++) {
            if (e[i] != rec[i]) {
                break;
            }
        }
        if (i == 7) {
            e[7] += 0x100U;             /* 回数だけ増やす */
            return;
        }
    }

    e = &v86_disklog[(v86_disklog_w % V86_DISKLOG_N) * 8];
    for (i = 0; i < 7; i++) {
        e[i] = rec[i];
    }
    e[7] = 0x100U | (result & 0xFFU);
    v86_disklog_w++;
}

/* disk_read() が実際に転送できたバイト数 (ログ用) */
static u32 xfer_done = 0;

/* ドライブの現在位置 (SEEK で決まる物理シリンダ)。
 *
 * µPD765A は「どの物理トラックに居るか」をドライブ側のトラックレジスタで
 * 持ち、READ DATA コマンドの C/H/R/N は**そのトラック上で照合する ID**
 * でしかない。両者が一致している必要はどこにもない。
 *
 * Ys.D88 は実際にずれている。物理 (C1,H0) のセクタ ID は C=0,H=1、
 * 物理 (C2,H0) は C=1,H=0 というように、ID が物理位置より 1 シリンダ
 * 遅れて振られている (トラック 0 だけは C=0,H=0 で一致)。
 * このため「CL を物理シリンダとみなす」実装ではトラック 0 以外
 * 一切読めない。実測で INT 1Bh が 1647 回失敗していたのはこれ。
 *
 * NP21/W も同じモデルで実装されている:
 *   src/diskimage/fd/fdd_d88.c  fdd_read_d88()
 *     trkseek(fdd, (fdc.treg[fdc.us] << 1) + fdc.hd)   ← 物理位置
 *     searchsector_d88()  … c/h/r/n を照合            ← 論理 ID
 *
 * 【実測】物理シリンダは SEEK の CL、**物理ヘッドは READ の AL bit2**。
 *
 * 呼び出しログを最後まで並べて分かった。決め手は隣り合う 2 コール:
 *
 *   SEEK C=1 → READ AL=94 ID(0,1,1) N=3 x2 → 0x15000
 *              READ AL=90 ID(0,1,1) N=3 x2 → 0x15800
 *
 * **同じ ID を連続する 2 つのバッファへ読んでいる。** ローダが同じ 2KB を
 * 二度読む道理は無いので、AL で行き先の物理トラックが変わっているはず。
 * 実際 `Ys.D88` の物理trk2 と trk3 は ID が同一で中身が違う
 * (先頭が "CHSRD10" と "BEPMU1")。AL bit2 をヘッドとすると
 * trk3 → 0x15000、trk2 → 0x15800 となり辻褄が合う。
 *
 * DH をヘッドとみなす読み方も試したが、SEEK に DH=3 や DH=34 といった
 * 値が飛んでくる (SEEK は CL しか設定していない) ため成立しない。
 * ヘッド 1 が要る場面はすべて AL=0x94 で来ていた。
 *
 * 同じ ID を持つ物理トラックが 2 本ずつあるこのディスクの書式は、
 * 「AL でヘッドを選ぶ」前提で初めて意味を持つ。
 */
static u8 cur_cyl  = 0;
static u8 cur_head = 0;      /* SEEK の DH。実際には使わないが観測用に残す */

/* DA/UA (AL) から物理ヘッドを取り出す。 */
#define V86_DAUA_HEAD(al)   (((al) >> 2) & 1U)

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

    /* 電源投入直後のドライブはトラック 0 に居る */
    cur_cyl  = 0;
    cur_head = 0;
    v86_disk_seek_n = 0;
    v86_disk_seek_cyl = 0;
    v86_disk_shift_n = 0;
    v86_disk_ident_n = 0;
    v86_disk_fail_n = 0;
    v86_disklog_w = 0;
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

/* D88 の 1 セクタ読み。
 *
 * 物理トラックを (phys_cyl, head) で選び、その上で ID (id_c, id_h, id_r) を
 * 照合する。見つかったセクタの実データ長ぶんを buf へ読む。
 *
 * loop_dev_read_chs() は使えない。あれは「物理=論理」を仮定するうえ、
 * 読み出し長をイメージ全体の bps (= トラック 0 の値。Ys.D88 では 256) で
 * 頭打ちにするため、1024 バイトセクタが 256 バイトしか転送されない。
 *
 * セクタ長 (= コマンドの N) も照合する。**ここを緩めてはいけない。**
 * 緩めた実装で実測したところ、ゲストが N=3 (1024B) を要求した読みに
 * トラック 0 の 256B セクタを 3 個返して「成功」と答えてしまい、
 * ロード先の 3/4 が未初期化のまま残ってゲストがそこへ飛んで #UD になった。
 * 見つからないなら見つからないと答えるほうがゲストは正しく振る舞える。
 * NP21/W も searchsector_d88() で c/h/r に加えて n を比較している。
 *
 * 戻り値: 読めたバイト数、-1 = セクタ不在 / 長さ不一致 / I/O エラー */
static int d88_read_sector(u32 phys_cyl, u32 head,
                           u32 id_c, u32 id_h, u32 id_r, u32 seclen,
                           u8 *buf, u32 buflen, u16 *out_spt)
{
    u32 off, data_len = 0, to_read;
    u16 spt = 0;
    int fd;

    off = loop_dev_seek_d88(V86_DISK_SLOT,
                            (u8)phys_cyl, (u8)head,
                            (u8)id_c, (u8)id_h, (u8)id_r,
                            &data_len, &spt);
    if (off == 0 || data_len == 0 || data_len != seclen) {
        return -1;
    }
    fd = loop_dev_get_fd(V86_DISK_SLOT);
    if (fd < 0) {
        return -1;
    }
    to_read = data_len;
    if (to_read > buflen) {
        to_read = buflen;
    }
    vfs_seek(fd, (int)off, 0);
    if (vfs_read_fd(fd, buf, to_read) < (int)to_read) {
        return -1;
    }
    if (out_spt) {
        *out_spt = spt;
    }
    return (int)to_read;
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
    u32 phys_cyl  = cur_cyl;                                /* SEEK の CL */
    u32 phys_head = V86_DAUA_HEAD(frame[V86F_EAX] & 0xFFU); /* AL bit2 */
    int is_d88 = (loop_dev_get_format(V86_DISK_SLOT) == LOOP_FMT_D88);
    /* PC-98 のセクタ長は最大 1024 バイト (N=3)。 */
    static u8 secbuf[1024];

    v86_disk_fail_chs = (cyl << 16) | (head << 8) | sect;
    v86_disk_fail_len = (n << 16) | count;
    v86_disk_fail_dst = dst;
    xfer_done = 0;

    if (!disk_attached) {
        v86_disk_fail_n++; v86_disk_fail_why = 4;
        bios_set_ah(frame, 0x60);
        bios_set_cf(frame, 1);
        return;
    }
    if (!guest_range_ok(dst, count)) {
        v86_disk_fail_n++; v86_disk_fail_why = 1;
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

    /* 転送単位はゲストの CH (セクタ長コード) に従う。実機の FDC と同じで、
     * 何バイトのセクタを読むかは呼び出し側が指定する。
     *
     * loop_dev が報告する bps を使ってはいけない。D88 はトラックごとに
     * セクタ長が違い得るからで、実際 Ys.D88 はトラック 0 が 256B x 16、
     * 以降が 1024B x 5 になっている。bps (=先頭トラックの値) で弾くと、
     * IPL の次段が読めずに INT 1Bh を無限リトライする。
     *
     * ただし loop_dev はセクタの実長ぶんを書き込むので、ゲストのバッファへ
     * 直接読ませるとサイズが食い違ったときに溢れる。いったんカーネル側の
     * バウンスバッファへ受けてから、ゲストが要求した長さだけ渡す。 */
    seclen = 128UL << (n & 3);
    if (seclen > sizeof(secbuf)) {
        v86_disk_fail_n++; v86_disk_fail_why = 2;
        bios_set_ah(frame, 0x40);
        bios_set_cf(frame, 1);
        return;
    }
    (void)bps;

    while (count >= seclen) {
        u32 i;
        u32 got;
        u16 trk_spt = 0;

        if (is_d88) {
            /* まず SEEK で決まった物理トラックを見る (実機の FDC と同じ)。 */
            int r = d88_read_sector(phys_cyl, phys_head, cyl, head, sect,
                                    seclen, secbuf, seclen, &trk_spt);
            if (r < 0) {
                /* そこに無ければ「物理=論理」でもう一度探す。
                 * ゲストが SEEK を省いて BIOS の暗黙シークに頼る
                 * 場合があるため。どちらで読めたかは実測できるよう
                 * カウンタを分けてある。 */
                r = d88_read_sector(cyl, head, cyl, head, sect,
                                    seclen, secbuf, seclen, &trk_spt);
                if (r < 0) {
                    v86_disk_fail_n++; v86_disk_fail_why = 3;
                    v86_disk_fail_chs = (phys_cyl << 24) | (phys_head << 28) |
                                        (cyl << 16) | (head << 8) | sect;
                    bios_set_ah(frame, 0x40);   /* セクタ不在 */
                    bios_set_cf(frame, 1);
                    return;
                }
                phys_cyl  = cyl;
                phys_head = head;
                v86_disk_ident_n++;
            } else if (phys_cyl != cyl || phys_head != head) {
                v86_disk_shift_n++;
            }
            got = (u32)r;
            if (trk_spt != 0) {
                spt = (u8)trk_spt;      /* トラックごとに違い得る */
            }
        } else {
            if (loop_dev_read_chs(V86_DISK_SLOT, (u16)cyl, (u8)head,
                                  (u8)sect, secbuf) != 0) {
                v86_disk_fail_n++; v86_disk_fail_why = 3;
                v86_disk_fail_chs = (cyl << 16) | (head << 8) | sect;
                bios_set_ah(frame, 0x40);   /* セクタ不在 */
                bios_set_cf(frame, 1);
                return;
            }
            got = seclen;
        }

        for (i = 0; i < got; i++) {
            ((volatile u8 *)dst)[i] = secbuf[i];
        }
        xfer_done += got;
        dst   += seclen;
        count -= seclen;

        /* セクタ → ヘッド → シリンダ の順に繰り上げる。
         * 物理位置も同じ歩幅で進める。ID のずれは連続領域では一定なので、
         * 差を保ったまま動かせば転送を跨いでも整合が取れる。 */
        sect++;
        if (sect > spt) {
            sect = 1;
            head++;
            if (head >= heads) {
                head = 0;
                cyl++;
            }
            phys_head++;
            if (phys_head >= heads) {
                phys_head = 0;
                phys_cyl++;
            }
        }
    }

    bios_set_ah(frame, 0x00);
    bios_set_cf(frame, 0);
}

static void bios_int1b(u32 *frame)
{
    u32 cmd = (frame[V86F_EAX] >> 8) & 0x0FU;   /* AH の下位ニブル */
    u32 ax_in = frame[V86F_EAX] & 0xFFFFU;
    u32 phys_in = ((u32)V86_DAUA_HEAD(ax_in & 0xFFU) << 8) | cur_cyl;
    u32 fails_before = v86_disk_fail_n;

    xfer_done = 0;      /* SEEK 等でも「今回の転送量」が 0 になるように */

    switch (cmd) {
    case V86_DISK_SENSE:
        disk_sense(frame);
        break;

    case V86_DISK_READ:
        disk_read(frame);
        break;

    case V86_DISK_SEEK:
        /* CL/DH の物理位置へヘッドを動かす。イメージ相手なので実体は
         * 無いが、**位置は必ず憶えておくこと**。この後の READ は
         * 「ここに居る物理トラック上で ID を照合する」ためにこれを使う。
         * ヘッドも憶える — シリンダだけだと Ys の 3 番目のロードが
         * 物理トラック 0 を見に行って空振りする。 */
        cur_cyl  = (u8)(frame[V86F_ECX] & 0xFFU);
        /* DH はそのまま憶える。**1 ビットにマスクしてはいけない。**
         * 実測で DH=3 の SEEK が来ることがあり、マスクするとヘッド 1 の
         * 別トラックが「それらしく」読めてしまう。ID は合っているので
         * 成功として返ってしまい、中身だけが違う — いちばん質の悪い壊れ方。
         * マスクしなければ範囲外トラックとして読みが失敗し、ゲストは
         * 自分で DH=0 に直して読み直す。その結果が正しい。 */
        cur_head = (u8)((frame[V86F_EDX] >> 8) & 0xFFU);
        v86_disk_seek_n++;
        v86_disk_seek_cyl = ((u32)cur_head << 8) | cur_cyl;
        bios_set_ah(frame, 0x00);
        bios_set_cf(frame, 0);
        break;

    case V86_DISK_RECAL:
        /* トラック 0 へ戻す */
        cur_cyl  = 0;
        cur_head = 0;
        v86_disk_seek_n++;
        v86_disk_seek_cyl = 0;
        bios_set_ah(frame, 0x00);
        bios_set_cf(frame, 0);
        break;

    case V86_DISK_INIT:
        bios_set_ah(frame, 0x00);
        bios_set_cf(frame, 0);
        break;

    default:
        /* WRITE と VERIFY は Phase 3-3b の後半で足す */
        bios_set_ah(frame, 0x86);
        bios_set_cf(frame, 1);
        break;
    }

    disklog(frame, ax_in, phys_in, xfer_done,
            (v86_disk_fail_n != fails_before) ? v86_disk_fail_why
          : ((frame[V86F_EFLAGS] & 1UL) ? 0x80U : 0U));
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
