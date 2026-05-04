/* ======================================================================== */
/*  V86_FDC.C — V86 FDC (µPD765A) ポートレベル仮想化                       */
/*                                                                          */
/*  V86ゲストが FDC ポートに直接アクセスするケース (FORMAT.COM 等) に       */
/*  対応する。INT 1Bh (v86_disk.c) のバックエンドを再利用することで         */
/*  重複実装を避ける。                                                      */
/*                                                                          */
/*  PC-98 FDC ポート:                                                       */
/*    0x90: MSR (メインステータスレジスタ, R)                               */
/*    0x92: FIFO (データレジスタ, R/W)                                      */
/*    0x94: CTRL (コントロールレジスタ, W) / リードスイッチ (R)            */
/*                                                                          */
/*  FDC ステートマシン (4フェーズ):                                         */
/*    IDLE    → MSR: RQM=1, DIO=0                                          */
/*    COMMAND → MSR: RQM=1, DIO=0, BUSY=1                                  */
/*    EXECUTE → MSR: RQM=0, BUSY=1  (DMA転送フェーズ)                      */
/*    RESULT  → MSR: RQM=1, DIO=1, BUSY=1                                  */
/*                                                                          */
/*  対応コマンド:                                                           */
/*    SPECIFY (0x03)          パラメータ記録のみ (ack)                      */
/*    SENSE DRIVE (0x04)      ドライブ状態を返す                            */
/*    WRITE DATA (0x05+MF)    DMAバッファ → ディスクイメージ書き込み        */
/*    READ DATA (0x06+MF)     ディスクイメージ → DMAバッファ読み込み        */
/*    RECALIBRATE (0x07)      仮想シリンダを0にリセット                     */
/*    SENSE INTERRUPT (0x08)  ST0+PCN を返す                               */
/*    READ ID (0x0A+MF)       現在トラックのセクタID返却                    */
/*    FORMAT TRACK (0x0D+MF)  フォーマット (実装済)                         */
/*    SEEK (0x0F)             仮想シリンダを更新                            */
/*                                                                          */
/*  出典: PC9800Bible §2-9, fdc.h ポート定義, v86_disk.c バックエンド      */
/* ======================================================================== */

#include "v86_fdc.h"
#include "v86.h"
#include "v86_dma.h"
#include "v86_disk.h"
#include "v86_mem.h"
#include "fdc.h"
#include "io.h"
#include "kstring.h"   /* kmemset */
#include "vfs.h"       /* vfs_seek / vfs_write_fd (FORMAT実装用) */

/* ====================================================================== */
/*  FDC フェーズ定数                                                       */
/* ====================================================================== */
#define FDC_PHASE_IDLE    0
#define FDC_PHASE_COMMAND 1
#define FDC_PHASE_EXECUTE 2
#define FDC_PHASE_RESULT  3

/* ====================================================================== */
/*  MSRビット (FDC状態を反映)                                              */
/* ====================================================================== */
#define VFDC_MSR_ACTA   0x01    /* ドライブA アクティブ */
#define VFDC_MSR_BUSY   0x10    /* FDCビジー (コマンド実行中) */
#define VFDC_MSR_NDMA   0x20    /* Non-DMA実行フェーズ中 */
#define VFDC_MSR_DIO    0x40    /* データ方向: 1=FDC→CPU (結果フェーズ) */
#define VFDC_MSR_RQM    0x80    /* Request for Master: FDC準備完了 */

/* ====================================================================== */
/*  コマンドテーブル: 各コマンドのパラメータ数と結果バイト数               */
/* ====================================================================== */
struct fdc_cmd_info {
    u8 cmd_code;    /* コマンドコード (下位5bit) */
    u8 param_count; /* パラメータバイト数 (コマンドバイトに続く分) */
    u8 result_count;/* リザルトバイト数 */
};

static const struct fdc_cmd_info fdc_cmd_table[] = {
    { 0x03, 2, 0 }, /* SPECIFY */
    { 0x04, 1, 1 }, /* SENSE DRIVE STATUS */
    { 0x05, 8, 7 }, /* WRITE DATA */
    { 0x06, 8, 7 }, /* READ DATA */
    { 0x07, 1, 0 }, /* RECALIBRATE */
    { 0x08, 0, 2 }, /* SENSE INTERRUPT STATUS */
    { 0x0A, 1, 7 }, /* READ ID */
    { 0x0D, 5, 7 }, /* FORMAT TRACK */
    { 0x0F, 2, 0 }, /* SEEK */
    { 0x00, 0, 0 }  /* テーブル終端 */
};

/* ====================================================================== */
/*  仮想FDCレジスタ                                                        */
/* ====================================================================== */
static struct {
    u8  phase;           /* FDC_PHASE_* */
    u8  cmd;             /* 現在のコマンドコード (下位5bit) */
    u8  cmd_buf[9];      /* コマンドパラメータバッファ (コマンドバイト含む) */
    u8  cmd_idx;         /* 受信済みバイト数 (コマンドバイト含む) */
    u8  cmd_total;       /* このコマンドの総バイト数 (1+param_count) */
    u8  result_buf[7];   /* リザルトバッファ */
    u8  result_idx;      /* 返却済みリザルト数 */
    u8  result_total;    /* このコマンドの総リザルト数 */
    u8  st0;             /* ST0 (最後の完了ステータス) */
    u8  pcn;             /* PCN (現在シリンダ番号) */
    u8  drv;             /* 現在ドライブ番号 (0-3) */
    u8  hd;              /* 現在ヘッド番号 (0-1) */
    u8  ctrl;            /* CTRL レジスタ (0x94) の最後の書き込み値 */
    int irq_after_seek;  /* RECALIBRATE/SEEK後に SENSE INTERRUPT が必要 */
} vfdc;

/* ====================================================================== */
/*  ヘルパー: コマンドテーブル検索                                         */
/* ====================================================================== */
static const struct fdc_cmd_info *fdc_find_cmd(u8 cmd_code)
{
    int i;
    u8 code = cmd_code & 0x1F; /* 上位3bit (MF/MT/SK) を除去 */
    for (i = 0; fdc_cmd_table[i].cmd_code != 0x00; i++) {
        if (fdc_cmd_table[i].cmd_code == code) {
            return &fdc_cmd_table[i];
        }
    }
    return 0;
}

/* ====================================================================== */
/*  ヘルパー: 成功リザルト (ST0=00, ST1=00, ST2=00 + CHS + N)            */
/* ====================================================================== */
static void fdc_set_result_ok(u8 cyl, u8 hd, u8 sect, u8 sec_n)
{
    vfdc.result_buf[0] = vfdc.st0;  /* ST0 */
    vfdc.result_buf[1] = 0x00;      /* ST1 */
    vfdc.result_buf[2] = 0x00;      /* ST2 */
    vfdc.result_buf[3] = cyl;       /* C */
    vfdc.result_buf[4] = hd;        /* H */
    vfdc.result_buf[5] = sect;      /* R */
    vfdc.result_buf[6] = sec_n;     /* N */
}

/* ====================================================================== */
/*  ヘルパー: エラーリザルト (ST0 bit6=01 = Abnormal termination)         */
/* ====================================================================== */
static void fdc_set_result_error(u8 st1, u8 st2)
{
    vfdc.result_buf[0] = (vfdc.st0 & 0x3FU) | 0x40U; /* IC=01 (異常終了) */
    vfdc.result_buf[1] = st1;
    vfdc.result_buf[2] = st2;
    vfdc.result_buf[3] = vfdc.pcn;
    vfdc.result_buf[4] = vfdc.hd;
    vfdc.result_buf[5] = 1;
    vfdc.result_buf[6] = 0;
}

/* ====================================================================== */
/*  コマンド実行: READ DATA / WRITE DATA                                   */
/*                                                                          */
/*  コマンドバッファ (cmd_buf[0]=コマンドバイト以降):                       */
/*    [0] コマンド (0x06+MF 等)                                            */
/*    [1] HD/US (ヘッド << 2 | ドライブ)                                   */
/*    [2] C (シリンダ)                                                     */
/*    [3] H (ヘッド)                                                       */
/*    [4] R (開始セクタ, 1始まり)                                           */
/*    [5] N (セクタ長コード)                                               */
/*    [6] EOT (最終セクタ番号)                                             */
/*    [7] GPL (ギャップ長)                                                 */
/*    [8] DTL (データ長, N=0のとき有効)                                    */
/* ====================================================================== */
static void fdc_execute_rw(void)
{
    u8 cyl    = vfdc.cmd_buf[2];
    u8 hd_reg = vfdc.cmd_buf[3];
    u8 sect   = vfdc.cmd_buf[4]; /* 1始まり */
    u8 sec_n  = vfdc.cmd_buf[5];
    const struct fdc_geom *g = v86_disk_get_geom();
    int is_write;
    u32 xfer_bytes;
    u8 *buf;
    u8 sect0;
    u32 byte_offset;

    is_write = ((vfdc.cmd & 0x1F) == 0x05) ? 1 : 0;

    /* セクタ長コードチェック */
    if (sec_n != g->sec_n) {
        fdc_set_result_error(0x04, 0x00); /* ST1 bit2=0x04: No Data */
        vfdc.result_total = 7;
        return;
    }

    /* セクタ番号 0 は無効 */
    if (sect == 0) {
        fdc_set_result_error(0x04, 0x00);
        vfdc.result_total = 7;
        return;
    }
    sect0 = sect - 1; /* 1始まり → 0ベース */

    /* CHS範囲チェック */
    if (cyl >= g->cyls || (hd_reg & 1) >= g->heads || sect0 >= g->spt) {
        fdc_set_result_error(0x04, 0x00);
        vfdc.result_total = 7;
        return;
    }

    /* DMAから転送先バッファとバイト数を取得 */
    buf = v86_dma_get_transfer(&xfer_bytes);
    if (!buf) {
        fdc_set_result_error(0x50, 0x00); /* ST1 bit4=0x50: Overrun */
        vfdc.result_total = 7;
        return;
    }
    if (xfer_bytes > (u32)g->bps) {
        xfer_bytes = (u32)g->bps;
    }

    /* CHS → バイトオフセット */
    byte_offset = ((u32)cyl * g->heads + (u32)(hd_reg & 1))
                  * ((u32)g->spt * g->bps)
                  + (u32)sect0 * g->bps;

    /* 実FDDモード */
    if (v86_disk_is_physical()) {
        if (is_write) {
            if (fdc_write_sector_geom(v86_disk_get_phys_drv(),
                                      cyl, hd_reg & 1, (int)sect,
                                      g, buf) != 0) {
                fdc_set_result_error(0x20, 0x00);
                vfdc.result_total = 7;
                return;
            }
        } else {
            if (fdc_read_sector_geom(v86_disk_get_phys_drv(),
                                     cyl, hd_reg & 1, (int)sect,
                                     g, buf) != 0) {
                fdc_set_result_error(0x20, 0x00);
                vfdc.result_total = 7;
                return;
            }
        }
    } else {
        /* ファイルモード: VFS直接アクセス */
        int fd      = v86_disk_get_fd();
        u32 img_off = v86_disk_get_offset();
        u32 img_sz;

        if (fd < 0) {
            fdc_set_result_error(0xE0, 0x00);
            vfdc.result_total = 7;
            return;
        }
        /* イメージサイズ境界チェック */
        img_sz = (u32)g->cyls * g->heads * g->spt * g->bps;
        if (byte_offset + xfer_bytes > img_sz) {
            fdc_set_result_error(0xC0, 0x00);
            vfdc.result_total = 7;
            return;
        }

        vfs_seek(fd, img_off + byte_offset, 0);
        if (is_write) {
            vfs_write_fd(fd, buf, xfer_bytes);
        } else {
            vfs_read_fd(fd, buf, xfer_bytes);
        }
    }

    /* 成功 */
    {
        u8 next_sect = sect + 1; /* 次セクタ */
        vfdc.st0 = (u8)((hd_reg & 0x04U) | (vfdc.drv & 0x03U));
        fdc_set_result_ok(cyl, hd_reg & 1, next_sect, sec_n);
    }
    vfdc.result_total = 7;
}


/* ====================================================================== */
/*  コマンド実行: READ ID                                                  */
/*                                                                          */
/*  cmd_buf[1] = HD/US                                                     */
/*  現在シリンダ (vfdc.pcn) のセクタ1のIDを返す。                          */
/* ====================================================================== */
static void fdc_execute_read_id(void)
{
    const struct fdc_geom *g = v86_disk_get_geom();
    u8 hd_reg = vfdc.cmd_buf[1];

    vfdc.st0 = (u8)((hd_reg & 0x04U) | (vfdc.drv & 0x03U));
    fdc_set_result_ok(vfdc.pcn, hd_reg & 1, 1, g->sec_n);
    vfdc.result_total = 7;
}

/* ====================================================================== */
/*  コマンド実行: FORMAT TRACK                                             */
/*                                                                          */
/*  コマンドバッファ (µPD765A FORMAT TRACK):                               */
/*    [0] コマンド (0x4D = MF|0x0D)                                        */
/*    [1] HD/US (ヘッド << 2 | ドライブ)                                   */
/*    [2] N     (セクタ長コード)                                           */
/*    [3] SC    (トラック当たりセクタ数)                                   */
/*    [4] GPL   (ギャップ3長)                                              */
/*    [5] D     (データフィルパターン, 通常 0xE5)                          */
/*                                                                          */
/*  DMAバッファには CHRN × SC バイトのIDフィールド配列が格納される:        */
/*    [C, H, R, N] × SC                                                    */
/*                                                                          */
/*  実装:                                                                  */
/*    - DMAバッファからIDフィールドを取得し、各セクタのCHS位置を確認       */
/*    - 各セクタをフィルパターンD で埋めてv86_disk.cバックエンドに書き込む  */
/*    - ファイルモード: vfs_seek + vfs_write_fd                            */
/*    - 実FDDモード: fdc_write_sector_geom                                 */
/* ====================================================================== */
static void fdc_execute_format(void)
{
    u8 hd_reg = vfdc.cmd_buf[1];
    u8 sec_n  = vfdc.cmd_buf[2];
    u8 sc     = vfdc.cmd_buf[3]; /* トラック当たりセクタ数 */
    u8 fill   = vfdc.cmd_buf[5]; /* フィルパターン (通常 0xE5) */
    const struct fdc_geom *g = v86_disk_get_geom();
    u8 *id_buf;                  /* DMAバッファ: CHRN × SC */
    u32 dma_bytes;
    int i;
    /* セクタフィルバッファ (スタックに1セクタ分確保するのは大きすぎるため静的) */
    static u8 fmt_sector[1024];  /* 最大セクタサイズ 1024バイト */

    /* IDフィールドの長さ: 4バイト × SC */
    id_buf = v86_dma_get_transfer(&dma_bytes);

    if (!id_buf || sc == 0 || (u32)(sc * 4) > dma_bytes) {
        /* DMAバッファ不足: エラー */
        fdc_set_result_error(0x04, 0x00);
        vfdc.result_total = 7;
        return;
    }

    /* フィルバッファを準備 */
    kmemset(fmt_sector, (int)fill, (u32)g->bps);

    /* 各IDフィールドに対してセクタを書き込む */
    for (i = 0; i < (int)sc; i++) {
        u8 id_c = id_buf[i * 4 + 0]; /* C */
        u8 id_h = id_buf[i * 4 + 1]; /* H */
        u8 id_r = id_buf[i * 4 + 2]; /* R (1始まり) */
        u8 id_n = id_buf[i * 4 + 3]; /* N */
        u32 regs[16];
        int j;

        /* セクタ長コードチェック */
        if (id_n != sec_n) {
            fdc_set_result_error(0x04, 0x00);
            vfdc.result_total = 7;
            return;
        }

        /* v86_bios_int1b() WRITE DATA (AH=0x05) を呼ぶ */
        for (j = 0; j < 16; j++) regs[j] = 0;

        /* AH=0x05 (WRITE DATA) + フラグ 0x70 (SEEK+Retry+MFM), AL=DA/UA */
        regs[V86_REG_EAX] = ((u32)0x75 << 8) | (u32)g->daua_high;
        /* BX = 転送バイト数 */
        regs[V86_REG_EBX] = (u32)g->bps;
        /* CH=セクタ長コード, CL=シリンダ */
        regs[V86_REG_ECX] = ((u32)id_n << 8) | id_c;
        /* DH=ヘッド, DL=セクタ (1始まり) */
        regs[V86_REG_EDX] = ((u32)id_h << 8) | id_r;
        /* ES:BP → fmt_sector バッファのV86リニアアドレス
         * fmt_sector は BSS 領域にあり、カーネル物理アドレスはバッキングRAM範囲外。
         * ここでは v86_phys_addr() の逆変換のかわりに fmt_sector をそのまま渡せる
         * よう、一時的に v86_mem から実アドレスを用いる。
         * 実装簡略化: fmt_sector のリニアアドレスを seg:off に分解 */
        {
            u32 la = (u32)fmt_sector;
            regs[V86_REG_ES]  = (u32)(la >> 4) & 0xFFFF;
            regs[V86_REG_EBP] = (u32)(la & 0x0F);
        }

        /* v86_bios_int1b は v86_phys_addr(ES, BP) でバッファを解決する。
         * ES:BP がバッキングRAM範囲外の場合 NULL が返るためエラーになる。
         * 代わりに v86_disk の低レベル書き込みを直接呼ぶ: */
        {
            u32 byte_offset;
            u8 sect0;

            if (id_r == 0) {
                /* セクタ番号 0 は無効 */
                fdc_set_result_error(0x04, 0x00);
                vfdc.result_total = 7;
                return;
            }
            sect0 = id_r - 1; /* 0ベースに変換 */

            /* CHS → バイトオフセット */
            byte_offset = ((u32)id_c * g->heads + (u32)id_h)
                          * ((u32)g->spt * g->bps)
                          + (u32)sect0 * g->bps;

            /* 実FDDモード */
            if (v86_disk_is_physical()) {
                if (fdc_write_sector_geom(v86_disk_get_phys_drv(),
                                          id_c, id_h, (int)id_r,
                                          g, fmt_sector) != 0) {
                    fdc_set_result_error(0x50, 0x00);
                    vfdc.result_total = 7;
                    return;
                }
            } else {
                /* ファイルモード: VFSで直接書き込み */
                int fd = v86_disk_get_fd();
                u32 img_off = v86_disk_get_offset();
                if (fd < 0) {
                    fdc_set_result_error(0xE0, 0x00);
                    vfdc.result_total = 7;
                    return;
                }
                vfs_seek(fd, img_off + byte_offset, 0);
                vfs_write_fd(fd, fmt_sector, (u32)g->bps);
            }
        }
    } /* for each sector */

    /* 全セクタ書き込み成功 */
    vfdc.st0 = (u8)((hd_reg & 0x04U) | (vfdc.drv & 0x03U));
    fdc_set_result_ok(vfdc.pcn, hd_reg & 1, 1, sec_n);
    vfdc.result_total = 7;
}

/* ====================================================================== */
/*  コマンド実行: SENSE DRIVE STATUS (0x04)                               */
/*                                                                          */
/*  cmd_buf[1] = HD/US                                                     */
/*  ST3 を返す: RY=1 (準備完了), WP=0 (書き込み可)                        */
/* ====================================================================== */
static void fdc_execute_sense_drive(void)
{
    u8 hd_reg = vfdc.cmd_buf[1];
    u8 st3;

    /* ST3: bit5=RY(Ready)=1, bit4=T0(Track0)=1 if PCN=0 */
    st3  = (u8)((hd_reg & 0x07U));     /* HD/US */
    st3 |= 0x20U;                       /* RY=1 (常にReady) */
    if (vfdc.pcn == 0) st3 |= 0x10U;   /* T0=1 (シリンダ0にいる) */

    vfdc.result_buf[0] = st3;
    vfdc.result_total  = 1;
}

/* ====================================================================== */
/*  FDCコマンド実行メイン (EXECUTE → RESULT フェーズ遷移)                 */
/* ====================================================================== */
static void fdc_execute_command(void)
{
    u8 cmd_code = vfdc.cmd & 0x1F;

    vfdc.result_idx   = 0;
    vfdc.result_total = 0;

    switch (cmd_code) {

    /* SPECIFY (0x03): SRT/HLT/HUT記録のみ */
    case 0x03:
        /* パラメータは捨てる。リザルトなし。 */
        break;

    /* SENSE DRIVE STATUS (0x04) */
    case 0x04:
        fdc_execute_sense_drive();
        break;

    /* WRITE DATA (0x05) */
    case 0x05:
        fdc_execute_rw();
        break;

    /* READ DATA (0x06) */
    case 0x06:
        fdc_execute_rw();
        break;

    /* RECALIBRATE (0x07): 仮想シリンダ=0 */
    case 0x07:
        vfdc.pcn = 0;
        vfdc.drv = vfdc.cmd_buf[1] & 0x03U;
        vfdc.st0 = (u8)(vfdc.drv & 0x03U) | 0x20U; /* SE=1 (Seek End) */
        vfdc.irq_after_seek = 1;
        /* リザルトなし: SENSE INTERRUPT で読み出す */
        break;

    /* SENSE INTERRUPT STATUS (0x08): ST0 + PCN を返す */
    case 0x08:
        if (vfdc.irq_after_seek) {
            vfdc.irq_after_seek = 0;
        } else {
            /* 無効 SENSE INTERRUPT: ST0=0x80 (Invalid Command) */
            vfdc.st0 = 0x80U;
        }
        vfdc.result_buf[0] = vfdc.st0;
        vfdc.result_buf[1] = vfdc.pcn;
        vfdc.result_total  = 2;
        break;

    /* READ ID (0x0A) */
    case 0x0A:
        fdc_execute_read_id();
        break;

    /* FORMAT TRACK (0x0D) */
    case 0x0D:
        fdc_execute_format();
        break;

    /* SEEK (0x0F): 仮想シリンダ更新 */
    case 0x0F:
        vfdc.drv = vfdc.cmd_buf[1] & 0x03U;
        vfdc.hd  = (vfdc.cmd_buf[1] >> 2) & 0x01U;
        vfdc.pcn = vfdc.cmd_buf[2];
        vfdc.st0 = (u8)((vfdc.hd << 2) | vfdc.drv) | 0x20U; /* SE=1 */
        vfdc.irq_after_seek = 1;
        /* リザルトなし */
        break;

    default:
        /* 未知コマンド: ST0=0x80 (Invalid Command) */
        vfdc.result_buf[0] = 0x80U;
        vfdc.result_total  = 1;
        break;
    }

    /* リザルトがあれば RESULT フェーズへ、なければ IDLE に戻る */
    if (vfdc.result_total > 0) {
        vfdc.phase = FDC_PHASE_RESULT;
    } else {
        vfdc.phase = FDC_PHASE_IDLE;
    }
}

/* ====================================================================== */
/*  v86_fdc_virt_init — 仮想FDCの初期化                                   */
/* ====================================================================== */
void v86_fdc_virt_init(void)
{
    int i;
    vfdc.phase        = FDC_PHASE_IDLE;
    vfdc.cmd          = 0;
    vfdc.cmd_idx      = 0;
    vfdc.cmd_total    = 0;
    vfdc.result_idx   = 0;
    vfdc.result_total = 0;
    vfdc.st0          = 0;
    vfdc.pcn          = 0;
    vfdc.drv          = 0;
    vfdc.hd           = 0;
    vfdc.ctrl         = 0;
    vfdc.irq_after_seek = 0;
    for (i = 0; i < 9; i++) vfdc.cmd_buf[i] = 0;
    for (i = 0; i < 7; i++) vfdc.result_buf[i] = 0;
}

/* ====================================================================== */
/*  v86_fdc_io — FDC I/Oポートハンドラ                                    */
/*                                                                          */
/*  戻り値: 1=処理済み, 0=非対象ポート                                     */
/* ====================================================================== */
int v86_fdc_io(u16 port, u8 *val, int is_write)
{
    switch (port) {

    /* ---------------------------------------------------------------- */
    /*  0x90: MSR (メインステータスレジスタ, 読み出しのみ)               */
    /*                                                                    */
    /*  フェーズに応じた RQM/DIO/BUSY/ACTx フラグを返す。               */
    /* ---------------------------------------------------------------- */
    case 0x90:
        if (!is_write) {
            u8 msr = 0;
            switch (vfdc.phase) {
            case FDC_PHASE_IDLE:
                msr = VFDC_MSR_RQM; /* RQM=1, DIO=0 (CPU→FDC方向) */
                break;
            case FDC_PHASE_COMMAND:
                /* コマンドパラメータ受信中: RQM=1, BUSY=1 */
                msr = VFDC_MSR_RQM | VFDC_MSR_BUSY;
                break;
            case FDC_PHASE_EXECUTE:
                /* データ転送中: RQM=0, BUSY=1 */
                msr = VFDC_MSR_BUSY;
                break;
            case FDC_PHASE_RESULT:
                /* リザルト読み出し中: RQM=1, DIO=1, BUSY=1 */
                msr = VFDC_MSR_RQM | VFDC_MSR_DIO | VFDC_MSR_BUSY;
                break;
            default:
                msr = VFDC_MSR_RQM;
                break;
            }
            *val = msr;
        }
        return 1;

    /* ---------------------------------------------------------------- */
    /*  0x92: FIFO (コマンド送信 / リザルト受信)                        */
    /* ---------------------------------------------------------------- */
    case 0x92:
        if (is_write) {
            /* ゲストがコマンドまたはパラメータを書き込む */
            switch (vfdc.phase) {

            case FDC_PHASE_IDLE:
            case FDC_PHASE_RESULT: {
                /* 新しいコマンドバイト受信: コマンドフェーズ開始 */
                const struct fdc_cmd_info *info;

                vfdc.cmd        = *val;
                vfdc.cmd_idx    = 0;
                vfdc.cmd_buf[0] = *val;
                vfdc.cmd_idx    = 1;

                info = fdc_find_cmd(*val);
                if (info) {
                    /* コマンドバイト(1) + パラメータ数 */
                    vfdc.cmd_total = (u8)(1 + info->param_count);
                } else {
                    /* 未知コマンド: コマンドバイトのみ */
                    vfdc.cmd_total = 1;
                }

                if (vfdc.cmd_total == 1) {
                    /* パラメータなし: 即実行 */
                    vfdc.phase = FDC_PHASE_EXECUTE;
                    fdc_execute_command();
                } else {
                    vfdc.phase = FDC_PHASE_COMMAND;
                }
                break;
            }

            case FDC_PHASE_COMMAND:
                /* パラメータバイト受信 */
                if (vfdc.cmd_idx < 9) {
                    vfdc.cmd_buf[vfdc.cmd_idx++] = *val;
                }
                if (vfdc.cmd_idx >= vfdc.cmd_total) {
                    /* 全パラメータ受信完了 → 実行フェーズ */
                    vfdc.phase = FDC_PHASE_EXECUTE;
                    fdc_execute_command();
                }
                break;

            default:
                /* EXECUTE中の書き込みは無視 */
                break;
            }
        } else {
            /* ゲストがリザルトを読み出す */
            if (vfdc.phase == FDC_PHASE_RESULT && vfdc.result_idx < vfdc.result_total) {
                *val = vfdc.result_buf[vfdc.result_idx++];
                if (vfdc.result_idx >= vfdc.result_total) {
                    /* 全リザルト返却完了 → IDLEに戻る */
                    vfdc.phase = FDC_PHASE_IDLE;
                }
            } else {
                /* データなし */
                *val = 0xFF;
            }
        }
        return 1;

    /* ---------------------------------------------------------------- */
    /*  0x94: CTRL (書き込み) / リードスイッチ (読み出し)               */
    /*                                                                    */
    /*  書き込み: モーター/リセット/DMA有効ビットを仮想レジスタに保存    */
    /*  読み出し: リードスイッチ = FDDタイプ (0x04 = 2HD両面)           */
    /* ---------------------------------------------------------------- */
    case 0x94:
        if (is_write) {
            u8 prev_ctrl = vfdc.ctrl;
            vfdc.ctrl = *val;

            /* リセット検出: CTRL bit7 (0x80) が 1→0 の遷移 */
            if ((prev_ctrl & 0x80U) && !(*val & 0x80U)) {
                /* FDCリセット: ステートを初期化 */
                vfdc.phase        = FDC_PHASE_IDLE;
                vfdc.cmd_idx      = 0;
                vfdc.result_idx   = 0;
                vfdc.result_total = 0;
                vfdc.st0          = 0xC0U; /* リセット後の ST0: IC=11 */
                /* SENSE INTERRUPT が4回必要 (4ドライブ分) */
                vfdc.irq_after_seek = 4;
            }
        } else {
            /* リードスイッチ: bit2=1 → 2HD 両面ドライブ搭載 */
            *val = 0x04U;
        }
        return 1;

    default:
        return 0;
    }
}
