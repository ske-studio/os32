/* ======================================================================== */
/*  V86_DISK.C — V86 ディスクBIOS (INT 1Bh) 仮想化                         */
/*                                                                          */
/*  PC-98 INT 1Bh ディスクBIOSのエミュレーション。                           */
/*  メモリ上に保持したFDDイメージまたは実FDDからセクタを読み書きする。        */
/*                                                                          */
/*  PC-98 2HD FDD ジオメトリ:                                              */
/*    77シリンダ × 2ヘッド × 8セクタ/トラック × 1024バイト/セクタ           */
/*                                                                          */
/*  INT 1Bh レジスタ規約 (PC9800Bible準拠):                                 */
/*    AH = ファンクション番号 (上位ニブル=フラグ, 下位ニブル=機能)          */
/*    AL = DA/UA (90h = 1MB FDD UNIT#0)                                    */
/*    BX = 転送バイト数                                                     */
/*    CH = セクタ長コード (3 = 1024バイト)                                  */
/*    CL = 開始シリンダ番号 (0-76)                                          */
/*    DH = ヘッド番号 (0, 1)                                                */
/*    DL = 開始セクタ番号                                                   */
/*    ES:BP = 転送先/元バッファアドレス                                     */
/*                                                                          */
/*    戻り値: AH = ステータス (0=成功)                                     */
/*    CF = 0:成功, 1:エラー                                                */
/* ======================================================================== */

#include "v86_disk.h"
#include "v86.h"
#include "v86_mem.h"
#include "kstring.h"
#include "kprintf.h"
#include "io.h"

#include "vfs.h"
#include "fdc.h"
#include "os32_kapi_shared.h"  /* OS32_Stat, OS_S_IWUSR (§6 ライトプロテクト) */

/* FDDイメージファイル (外部から設定される) */
static int fdd_fd = -1;
static u32 fdd_image_offset = 0;
static u32 fdd_image_size = 0;

/* D88モード: トラックテーブルを保持 */
static int fdd_is_d88 = 0;
#define D88_MAX_TRACKS 164
static u32 fdd_d88_track_table[D88_MAX_TRACKS];  /* 各トラックのファイルオフセット */

/* 実FDDモード */
static int fdd_use_physical = 0;
static int fdd_phys_drv = 0;

/* 現在マウント中のジオメトリポインタ (デフォルト = 2HD) */
static const struct fdc_geom *fdd_geom = &fdc_geom_2hd;

/* ====================================================================== */
/*  デバッグ用 INT 1Bh 呼び出しログ (リングバッファ)                        */
/*  V86終了後に v86_disk_dump_log() でダンプする。                          */
/*  #GPハンドラ内でkprintfを呼ぶとスタック溢れの危険があるため、            */
/*  グローバルバッファに記録し、V86セッション終了後にダンプする方式。       */
/* ====================================================================== */
#define V86_DISK_LOG_SIZE 64
/* struct v86_disk_log_entry は v86_disk.h で定義済み */
static struct v86_disk_log_entry disk_log[V86_DISK_LOG_SIZE];
static u32 disk_log_idx = 0;
static u32 disk_log_count = 0;

/* ディスクログコミット + エラーリターンマクロ
 * 各エラーパスで重複していた「log記録 + regs更新 + return 0」パターンを集約 */
#define DISK_ERROR_RETURN(entry, st, res_off, r) do { \
    (entry)->status = (st); \
    (entry)->result_offset = (res_off); \
    disk_log_idx = (disk_log_idx + 1) % V86_DISK_LOG_SIZE; \
    disk_log_count++; \
    (r)[V86_REG_EAX] = ((r)[V86_REG_EAX] & 0xFFFF00FFUL) | ((u32)(st) << 8); \
    (r)[V86_REG_EFLAGS] |= 1; \
    return 0; \
} while (0)

/* ====================================================================== */
/*  v86_disk_set_file — FDDイメージファイルを設定                         */
/* ====================================================================== */
void v86_disk_set_file(int fd, u32 data_offset, u32 data_size,
                       fdc_media_t media)
{
    fdd_fd = fd;
    fdd_image_offset = data_offset;
    fdd_image_size = data_size;
    fdd_is_d88 = 0;  /* RAW/FDIモード */
    switch (media) {
    case FDC_MEDIA_2DD_640: fdd_geom = &fdc_geom_2dd_640; break;
    case FDC_MEDIA_2DD_720: fdd_geom = &fdc_geom_2dd_720; break;
    case FDC_MEDIA_2D_256:  fdd_geom = &fdc_geom_2d_256;  break;
    default:                fdd_geom = &fdc_geom_2hd;      break;
    }
}

/* ====================================================================== */
/*  v86_disk_set_d88 — D88形式ディスクイメージを設定                       */
/*                                                                          */
/*  D88形式はRAWフラットではなく、各セクタに16バイトヘッダが付く。           */
/*  トラックテーブル (ファイルオフセット0x20-0x2AF) をキャッシュし、         */
/*  CHS→オフセット変換時にセクタヘッダを検索する。                          */
/* ====================================================================== */
void v86_disk_set_d88(int fd, u32 file_size, fdc_media_t media)
{
    int i;
    u8 tbl[D88_MAX_TRACKS * 4];  /* 164 x 4 = 656 bytes */

    fdd_fd = fd;
    fdd_image_offset = 0;  /* D88ではトラックテーブル経由でアクセス */
    fdd_image_size = file_size;
    fdd_is_d88 = 1;

    switch (media) {
    case FDC_MEDIA_2DD_640: fdd_geom = &fdc_geom_2dd_640; break;
    case FDC_MEDIA_2DD_720: fdd_geom = &fdc_geom_2dd_720; break;
    case FDC_MEDIA_2D_256:  fdd_geom = &fdc_geom_2d_256;  break;
    default:                fdd_geom = &fdc_geom_2hd;      break;
    }

    /* トラックテーブル読み出し (ファイルオフセット 0x20 から 164 x 4 bytes) */
    vfs_seek(fd, 0x20, 0);
    vfs_read_fd(fd, tbl, D88_MAX_TRACKS * 4);
    for (i = 0; i < D88_MAX_TRACKS; i++) {
        fdd_d88_track_table[i] = *(u32 *)(tbl + i * 4);
    }

    /* トラック0のセクタヘッダから実際のジオメトリを自動検出
     * D88のmedia_flagは信頼性が低い場合があるため、
     * セクタヘッダのN値とSPTから正確なジオメトリを決定する */
    if (fdd_d88_track_table[0] != 0 && fdd_d88_track_table[0] < file_size) {
        u8 sec0[16];
        u8 actual_n;
        u16 actual_spt;

        vfs_seek(fd, fdd_d88_track_table[0], 0);
        vfs_read_fd(fd, sec0, 16);
        actual_n   = sec0[3];       /* N: セクタ長コード */
        actual_spt = *(u16 *)(sec0 + 4);  /* SPT */

        /* N値からメディア種別を再判定 */
        if (actual_n == 1 && actual_spt == 16) {
            /* 2D: 256 bytes/sector, 16 sectors/track */
            fdd_geom = &fdc_geom_2d_256;
        } else if (actual_n == 2 && actual_spt == 8) {
            /* 2DD 640KB: 512 bytes/sector, 8 sectors/track */
            fdd_geom = &fdc_geom_2dd_640;
        } else if (actual_n == 2 && actual_spt == 9) {
            /* 2DD 720KB: 512 bytes/sector, 9 sectors/track */
            fdd_geom = &fdc_geom_2dd_720;
        } else if (actual_n == 3 && actual_spt == 8) {
            /* 2HD: 1024 bytes/sector, 8 sectors/track */
            fdd_geom = &fdc_geom_2hd;
        }
        /* その他: media引数で設定済みのジオメトリを使う */
    }
}

/* ====================================================================== */
/*  d88_seek_sector — D88形式でCHSに対応するセクタデータのオフセットを検索  */
/*                                                                          */
/*  トラックテーブルからトラック先頭を求め、セクタヘッダのR(セクタID)を     */
/*  走査して一致するセクタのデータ部オフセットを返す。                       */
/*                                                                          */
/*  D88はトラックごとに異なるセクタサイズ/SPTを持てる（混合フォーマット）。 */
/*  例: Track 0 = N=1 (256B), SPT=16 (IPL用)                               */
/*      Track 1+ = N=3 (1024B), SPT=5 (ゲームデータ)                       */
/*                                                                          */
/*  out_data_len: [OUT] 見つかったセクタの実データ長 (NULL可)               */
/*  out_spt:      [OUT] トラック内のセクタ数 (NULL可)                       */
/*  戻り値: データ部のファイルオフセット。見つからない場合は 0。             */
/* ====================================================================== */
static u32 d88_seek_sector(u8 cyl, u8 head, u8 sect_r,
                           u32 *out_data_len, u16 *out_spt)
{
    u32 track_idx;
    u32 trk_off;
    u8 sec_hdr[16];
    u32 pos;
    int i;
    int spt;

    track_idx = (u32)cyl * 2 + (u32)head;
    if (track_idx >= D88_MAX_TRACKS) return 0;

    trk_off = fdd_d88_track_table[track_idx];
    if (trk_off == 0 || trk_off >= fdd_image_size) return 0;

    /* トラック先頭のセクタヘッダを読んでSPTを取得 */
    vfs_seek(fdd_fd, trk_off, 0);
    vfs_read_fd(fdd_fd, sec_hdr, 16);
    spt = *(u16 *)(sec_hdr + 4);  /* セクタヘッダ offset 4: このトラックのセクタ数 */
    if (spt <= 0 || spt > 64) spt = 8;  /* 安全ガード */

    if (out_spt) *out_spt = (u16)spt;

    /* セクタを走査して R = sect_r を探す */
    pos = trk_off;
    for (i = 0; i < spt; i++) {
        u32 data_len;
        vfs_seek(fdd_fd, pos, 0);
        vfs_read_fd(fdd_fd, sec_hdr, 16);
        data_len = *(u16 *)(sec_hdr + 14);  /* offset 0x0E: 実データサイズ */
        if (data_len == 0) data_len = (u32)(128 << sec_hdr[3]);  /* Nから計算 */

        if (sec_hdr[2] == sect_r) {  /* sec_hdr[2] = R (セクタID) */
            if (out_data_len) *out_data_len = data_len;
            return pos + 16;  /* データ部の先頭 = ヘッダ直後 */
        }
        pos += 16 + data_len;  /* 次のセクタへ */
    }
    return 0;  /* 見つからなかった */
}

/* ====================================================================== */
/*  v86_disk_set_physical — 実FDDモードを有効化                           */
/* ====================================================================== */
void v86_disk_set_physical(int drv, fdc_media_t media)
{
    fdd_use_physical = 1;
    fdd_phys_drv = drv;
    fdd_fd = -1;
    fdd_image_offset = 0;
    fdd_is_d88 = 0;
    switch (media) {
    case FDC_MEDIA_2DD_640: fdd_geom = &fdc_geom_2dd_640; break;
    case FDC_MEDIA_2DD_720: fdd_geom = &fdc_geom_2dd_720; break;
    case FDC_MEDIA_2D_256:  fdd_geom = &fdc_geom_2d_256;  break;
    default:                fdd_geom = &fdc_geom_2hd;      break;
    }
    fdd_image_size = (u32)fdd_geom->cyls * fdd_geom->heads
                     * fdd_geom->spt * fdd_geom->bps;
}

/* ====================================================================== */
/*  v86_disk_clear — FDDイメージをクリア                                   */
/* ====================================================================== */
void v86_disk_clear(void)
{
    fdd_fd = -1;
    fdd_image_offset = 0;
    fdd_image_size = 0;
    fdd_use_physical = 0;
    fdd_phys_drv = 0;
    fdd_is_d88 = 0;
    fdd_geom = &fdc_geom_2hd;  /* デフォルトに戻す */
}

/* ====================================================================== */
/*  v86_disk_get_geom — 現在マウント中のジオメトリを返す                   */
/* ====================================================================== */
const struct fdc_geom *v86_disk_get_geom(void)
{
    return fdd_geom;
}

/* ====================================================================== */
/*  内部状態アクセサ (v86_fdc.c の FORMAT TRACK 実装用)                    */
/* ====================================================================== */
int v86_disk_is_physical(void)
{
    return fdd_use_physical;
}

int v86_disk_get_phys_drv(void)
{
    return fdd_phys_drv;
}

int v86_disk_get_fd(void)
{
    return fdd_fd;
}

u32 v86_disk_get_offset(void)
{
    return fdd_image_offset;
}

/* ====================================================================== */
/*  v86_bios_int1b — INT 1Bh ディスクBIOS ハンドラ                         */
/* ====================================================================== */
int v86_bios_int1b(u32 *regs)
{
    u8 func = (u8)((regs[V86_REG_EAX] >> 8) & 0xFF);
    u8 daua = (u8)(regs[V86_REG_EAX] & 0xFF);
    struct v86_disk_log_entry *log_entry;

    /* デバッグ: リングバッファにパラメータを記録 */
    log_entry = &disk_log[disk_log_idx];
    log_entry->func = func;
    log_entry->daua = daua;
    log_entry->cylinder = (u8)(regs[V86_REG_ECX] & 0xFF);
    log_entry->sector_len = (u8)((regs[V86_REG_ECX] >> 8) & 0xFF);
    log_entry->head = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
    log_entry->sector = (u8)(regs[V86_REG_EDX] & 0xFF);
    log_entry->xfer_bytes = (u16)(regs[V86_REG_EBX] & 0xFFFF);
    log_entry->es = (u16)(regs[V86_REG_ES] & 0xFFFF);
    log_entry->bp = (u16)(regs[V86_REG_EBP] & 0xFFFF);
    log_entry->result_offset = -99;  /* 未計算マーカー */
    log_entry->status = 0xFF;        /* 未完了マーカー */

    /* 計装: 最初の64回のINT 1Bh呼び出しをシリアルに出力 */
    {
        static u32 _int1b_trace_n = 0;
        if (_int1b_trace_n < 64) {
            kprintf(0xA1, "[1B] AH=%02X C=%u H=%u R=%u N=%u BX=%u ES:BP=%04X:%04X\n",
                    (unsigned)func,
                    (unsigned)log_entry->cylinder,
                    (unsigned)log_entry->head,
                    (unsigned)log_entry->sector,
                    (unsigned)log_entry->sector_len,
                    (unsigned)log_entry->xfer_bytes,
                    (unsigned)log_entry->es,
                    (unsigned)log_entry->bp);
            _int1b_trace_n++;
        }
    }

    /* DA/UAチェック: 現在マウント中のメディアの DA/UA 上位ニブルと一致するか確認 */
    {
        const struct fdc_geom *g = v86_disk_get_geom();
        if ((daua & 0xF0) != (u8)g->daua_high) {
            /* 未サポートデバイス: ステータス 0x40 = DA/UAが不適当 */
            DISK_ERROR_RETURN(log_entry, 0x40, -1, regs);
        }
    }

    /* ================================================================ */
    /*  ファンクションコードのルーティング                               */
    /*  PC-98 INT 1Bh では AH の下位ニブルが機能コード:                  */
    /*    0x01, 0x06 = READ DATA                                        */
    /*    0x05       = WRITE DATA                                       */
    /*    0x03       = INITIALIZE                                       */
    /*    0x04       = SENSE                                            */
    /*    0x07       = RECALIBRATE / VERIFY                             */
    /*  上位ニブルはフラグ (SEEK/Retry/MFM/MultiTrack)                   */
    /*  例: AH=0x56 → 0x50|0x06 → READ with flags                     */
    /*      AH=0x76 → 0x70|0x06 → READ with SEEK+Retry+MFM            */
    /* ================================================================ */
    {
        u8 func_base = func & 0x0F;

        switch (func_base) {

        /* ============================================================ */
        /*  機能 03h: ドライブ初期化 (Initialize)                       */
        /* ============================================================ */
        case 0x03:
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;
            log_entry->status = 0x00;
            break;

        /* ============================================================ */
        /*  機能 04h: センス (メディア検知)                              */
        /*  84h (拡張センス) も下位ニブル 04h なのでここで処理           */
        /*                                                                */
        /*  NP21/W bios1b.c 準拠の戻り値:                               */
        /*    bit 0 (0x01): 2HD メディア (AL bit7=1 の場合セット)        */
        /*    bit 3 (0x08): 1MB/640KB互換ドライブ (AH=84hの場合)        */
        /*    bit 4 (0x10): ライトプロテクト中                          */
        /*  IO.SYSはこの値でディスクフォーマット(CH)を決定する!          */
        /* ============================================================ */
        case 0x04: {
            u8 ret_ah = 0x00;
            u8 al = (u8)(regs[V86_REG_EAX] & 0xFF);

            /* 2HD判定: AL bit7 = DA bit7 = 1MB FDD */
            if (al & 0x80) {
                ret_ah |= 0x01;
            }

            /* 拡張センス (AH=0x84): 1MB/640KB互換ドライブ */
            if ((regs[V86_REG_EAX] & 0x8F40) == 0x8400) {
                ret_ah |= 0x08;
            }

            /* ================================================================ */
            /*  §6 ライトプロテクト反映: AH bit4 = ライトプロテクト中          */
            /*  ファイルモード: vfs_fstat で書き込み権限を確認                  */
            /*  実FDDモード: 常に書き込み可能 (FDCが実際のWP状態を管理)         */
            /* ================================================================ */
            if (!fdd_use_physical && fdd_fd >= 0) {
                OS32_Stat st;
                if (vfs_fstat(fdd_fd, &st) == 0) {
                    if (!(st.st_mode & OS_S_IWUSR)) {
                        ret_ah |= 0x10;  /* bit4: ライトプロテクト中 */
                    }
                }
            }

            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL)
                               | ((u32)ret_ah << 8);
            regs[V86_REG_EFLAGS] &= ~1UL;
            log_entry->status = ret_ah;
            break;
        }

        /* ============================================================ */
        /*  機能 01h/06h: セクタ読み出し (READ DATA)                    */
        /*                                                                */
        /*  01h = READ DATA (1MB FDD)                                   */
        /*  06h = READ DATA (1MB FDD, 別バリアント)                     */
        /*                                                                */
        /*  AH=0x56 → func_base=0x06, flags=0x50                       */
        /*  AH=0x76 → func_base=0x06, flags=0x70                       */
        /*                                                                */
        /*  セクタ長コード (CH) 処理方針:
         *  実FDCは物理フォーマット(N=3, 1024B)のセクタ単位で読み取る。
         *  しかしIO.SYSはCH=0(128B)でセクタ番号を指定する場合がある。
         *  この場合、セクタ番号はCH単位のセクタ番号空間で解釈し、
         *  トラック内バイトオフセットに変換してから物理イメージの
         *  正しい位置からデータを返す。
         *  例: CH=0, S=14(1-based) → (14-1)*128=1664バイト目 */
        case 0x01:
        case 0x06: {
            u16 xfer_bytes = (u16)(regs[V86_REG_EBX] & 0xFFFF);
            u8 cylinder = (u8)(regs[V86_REG_ECX] & 0xFF);          /* CL */
            u8 sector_len = (u8)((regs[V86_REG_ECX] >> 8) & 0xFF); /* CH */
            u8 head_dh = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);    /* DH */
            u8 sector_dl = (u8)(regs[V86_REG_EDX] & 0xFF);         /* DL */
            u16 es_val = (u16)(regs[V86_REG_ES] & 0xFFFF);
            u16 bp_val = (u16)(regs[V86_REG_EBP] & 0xFFFF);
            u8 *dst;
            i32 img_offset;
            u32 remaining;
            u32 chunk;
            /* NP2kai互換: READ完了後のBDA結果バッファに最終C/H/Rを反映
             * ゲストIPLはBDA 0x0564 のR値から次のアクセス位置を決定する */
            u8 final_cyl;
            u8 final_head;
            u8 final_sect_r;
            {
                u32 byte_offset;

                /* セクタ長コード (CH) バリデーション — 動的ジオメトリ参照
                 * FDCは物理セクタIDのN値と要求N値を照合する。
                 * D88モード: セクタヘッダに実N値が格納されているため、
                 *   ゲスト要求CHとジオメトリN値の不一致を許容する。
                 *   Ys等の古いゲームはBIOSの返す結果バッファN値に
                 *   依存せず独自のCH値で要求する場合がある。
                 * RAW/FDIモード: CH != g->sec_n → セクタ未検出エラー(0xC0) */
                {
                    const struct fdc_geom *g = v86_disk_get_geom();
                    if (!fdd_is_d88 && sector_len != g->sec_n) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, -2, regs);
                    }

                    /* PC-98 INT 1Bh: DL(セクタ番号)は常に1ベース → 0ベースに変換 */
                    if (sector_dl > 0) sector_dl--;

                    /* イメージ未設定チェック (ファイルモードのみ) */
                    if (!fdd_use_physical && fdd_fd < 0) {
                        DISK_ERROR_RETURN(log_entry, 0xE0, -1, regs);
                    }

                    /* CHS範囲チェック */
                    if (cylinder >= g->cyls || head_dh >= g->heads) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, -1, regs);
                    }

                    /* CHS → バイトオフセット変換 */
                    byte_offset = ((u32)cylinder * g->heads + (u32)head_dh)
                                  * ((u32)g->spt * g->bps)
                                  + (u32)sector_dl * g->bps;

                    /* 範囲チェック (SPT境界外もここでキャッチされる) */
                    if (byte_offset >= fdd_image_size) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, (i32)byte_offset, regs);
                    }

                    log_entry->result_offset = (i32)byte_offset;
                    img_offset = (i32)byte_offset;
                }
            }

            /* 転送先バッファのリニアアドレス */
            dst = v86_phys_addr(es_val, bp_val);

            /* セクタ単位で転送 (複数セクタ対応) */
            remaining = (u32)xfer_bytes;
            final_cyl = cylinder;
            final_head = head_dh;
            final_sect_r = sector_dl + 1; /* 1ベースに戻す */
            if (fdd_use_physical) {
                /* 実FDDモード: fdc_read_sector_geom()で1セクタずつ読む */
                const struct fdc_geom *g = v86_disk_get_geom();
                u8 cur_sect = sector_dl;  /* 0ベース */
                u8 cur_head = head_dh;
                u8 cur_cyl = cylinder;
                while (remaining > 0) {
                    chunk = (u32)g->bps;
                    if (chunk > remaining) chunk = remaining;
                    if (fdc_read_sector_geom(fdd_phys_drv, cur_cyl, cur_head,
                                             cur_sect + 1, g, dst) != 0) {
                        /* FDCリードエラー */
                        DISK_ERROR_RETURN(log_entry, 0xD0, (i32)img_offset, regs);
                    }
                    dst += chunk;
                    remaining -= chunk;
                    img_offset += (i32)chunk;
                    /* 次セクタに進む */
                    cur_sect++;
                    if (cur_sect >= g->spt) {
                        cur_sect = 0;
                        cur_head++;
                        if (cur_head >= g->heads) {
                            cur_head = 0;
                            cur_cyl++;
                        }
                    }
                }
                final_cyl = cur_cyl;
                final_head = cur_head;
                final_sect_r = cur_sect + 1; /* 1ベース */
            } else if (fdd_is_d88) {
                /* D88モード: セクタヘッダ経由でアクセス
                 * D88はトラックごとに異なるセクタサイズ/SPTを持てる
                 * (混合フォーマット)。d88_seek_sector が返す実データ長と
                 * SPTを使用し、固定ジオメトリへの依存を排除する。 */
                u8 cur_sect_r = sector_dl + 1;  /* D88のRは1ベース */
                u8 cur_head = head_dh;
                u8 cur_cyl = cylinder;
                while (remaining > 0) {
                    u32 sec_data_len = 0;
                    u16 trk_spt = 0;
                    u32 data_off = d88_seek_sector(cur_cyl, cur_head,
                                                   cur_sect_r,
                                                   &sec_data_len, &trk_spt);
                    if (data_off == 0) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, (i32)cur_sect_r, regs);
                    }
                    /* セクタの実データ長を使用 (トラックごとに異なる) */
                    chunk = sec_data_len;
                    if (chunk > remaining) chunk = remaining;
                    vfs_seek(fdd_fd, data_off, 0);
                    vfs_read_fd(fdd_fd, dst, chunk);
                    dst += chunk;
                    remaining -= chunk;
                    img_offset += (i32)chunk;
                    /* 次セクタに進む (トラック実SPTで折り返し) */
                    cur_sect_r++;
                    if (trk_spt > 0 && cur_sect_r > (u8)trk_spt) {
                        cur_sect_r = 1;
                        cur_head++;
                        if (cur_head >= 2) {
                            cur_head = 0;
                            cur_cyl++;
                        }
                    }
                }
                final_cyl = cur_cyl;
                final_head = cur_head;
                final_sect_r = cur_sect_r; /* D88: 既に1ベース */
            } else {
                /* RAW/FDI ファイルモード: VFS seek+read */
                const struct fdc_geom *g = v86_disk_get_geom();
                if (img_offset >= 0 && (u32)img_offset < fdd_image_size) {
                    vfs_seek(fdd_fd, fdd_image_offset + (u32)img_offset, 0);
                }
                while (remaining > 0 && img_offset >= 0 && (u32)img_offset < fdd_image_size) {
                    chunk = remaining;
                    if (chunk > (u32)g->bps) chunk = (u32)g->bps;
                    if ((u32)img_offset + chunk > fdd_image_size) {
                        chunk = fdd_image_size - (u32)img_offset;
                    }
                    vfs_read_fd(fdd_fd, dst, chunk);
                    dst += chunk;
                    img_offset += (i32)chunk;
                    remaining -= chunk;
                }
            }

            /* 成功 */
            log_entry->status = 0x00;
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;

            /* BDA FDC結果バッファ更新 (0000:0564 + us*8)
             * NP21/W biosfd_resultout() と同等。
             * IO.SYSはここからN(セクタ長コード)を読んで
             * 次のディスクリードのCHパラメータを決定する。 */
            {
                u8 us = 0; /* FDD UNIT#0 固定 */
                u8 *result_ptr = v86_phys_addr(0x0000, 0x0564 + us * 8);
                result_ptr[0] = us;            /* ST0: US | HD<<2 */
                result_ptr[1] = 0x00;          /* ST1: 正常 */
                result_ptr[2] = 0x00;          /* ST2: 正常 */
                result_ptr[3] = final_cyl;     /* C: 最終シリンダ */
                result_ptr[4] = final_head;    /* H: 最終ヘッド */
                result_ptr[5] = final_sect_r;  /* R: 次に読むべきセクタ (1ベース) */
                result_ptr[6] = sector_len;    /* N: セクタ長コード */
                result_ptr[7] = final_cyl;     /* NCN: 現在シリンダ */
            }
            break;
        }

        /* ============================================================ */
        /*  機能 05h: セクタ書き込み (WRITE DATA)                       */
        /*  READ (01h/06h) と対称な完全実装。                          */
        /*  ファイルモード: vfs_seek + vfs_write_fd                    */
        /*  実FDDモード:   fdc_write_sector (複数セクタ対応)           */
        /* ============================================================ */
        case 0x05: {
            u16 wr_xfer = (u16)(regs[V86_REG_EBX] & 0xFFFF);
            u8 wr_cyl = (u8)(regs[V86_REG_ECX] & 0xFF);
            u8 wr_seclen = (u8)((regs[V86_REG_ECX] >> 8) & 0xFF);
            u8 wr_head = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
            u8 wr_sect = (u8)(regs[V86_REG_EDX] & 0xFF);
            u16 wr_es = (u16)(regs[V86_REG_ES] & 0xFFFF);
            u16 wr_bp = (u16)(regs[V86_REG_EBP] & 0xFFFF);
            u8 *wr_buf;
            i32 wr_off;
            u32 wr_rem, wr_chunk;
            u8 wfinal_cyl, wfinal_head, wfinal_sect_r;

            /* CH バリデーション (READと同一) */
            {
                const struct fdc_geom *g = v86_disk_get_geom();
                if (wr_seclen != g->sec_n) {
                    DISK_ERROR_RETURN(log_entry, 0xC0, -2, regs);
                }

                /* DL: 1ベース → 0ベース */
                if (wr_sect > 0) wr_sect--;

                /* イメージ未設定チェック */
                if (!fdd_use_physical && fdd_fd < 0) {
                    DISK_ERROR_RETURN(log_entry, 0xE0, -1, regs);
                }

                /* CHS範囲チェック */
                if (wr_cyl >= g->cyls || wr_head >= g->heads) {
                    DISK_ERROR_RETURN(log_entry, 0xC0, -1, regs);
                }

                /* CHS → バイトオフセット */
                {
                    u32 boff = ((u32)wr_cyl * g->heads + (u32)wr_head)
                               * ((u32)g->spt * g->bps)
                               + (u32)wr_sect * g->bps;
                    if (boff >= fdd_image_size) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, (i32)boff, regs);
                    }
                    log_entry->result_offset = (i32)boff;
                    wr_off = (i32)boff;
                }
            }

            /* 転送元バッファ */
            wr_buf = v86_phys_addr(wr_es, wr_bp);

            /* セクタ単位で書き込み (複数セクタ対応) */
            wr_rem = (u32)wr_xfer;
            wfinal_cyl = wr_cyl;
            wfinal_head = wr_head;
            wfinal_sect_r = wr_sect + 1;
            if (fdd_use_physical) {
                /* 実FDDモード: fdc_write_sector_geom() ループ */
                const struct fdc_geom *g = v86_disk_get_geom();
                u8 wc_s = wr_sect;
                u8 wc_h = wr_head;
                u8 wc_c = wr_cyl;
                while (wr_rem > 0) {
                    wr_chunk = (u32)g->bps;
                    if (wr_chunk > wr_rem) wr_chunk = wr_rem;
                    if (fdc_write_sector_geom(fdd_phys_drv, wc_c, wc_h,
                                              wc_s + 1, g, wr_buf) != 0) {
                        DISK_ERROR_RETURN(log_entry, 0xD0, wr_off, regs);
                    }
                    wr_buf += wr_chunk;
                    wr_rem -= wr_chunk;
                    wr_off += (i32)wr_chunk;
                    wc_s++;
                    if (wc_s >= g->spt) {
                        wc_s = 0;
                        wc_h++;
                        if (wc_h >= g->heads) {
                            wc_h = 0;
                            wc_c++;
                        }
                    }
                }
                wfinal_cyl = wc_c;
                wfinal_head = wc_h;
                wfinal_sect_r = wc_s + 1;
            } else if (fdd_is_d88) {
                /* D88モード: セクタヘッダ経由でアクセス (混合フォーマット対応) */
                u8 wc_r = wr_sect + 1;  /* D88のRは1ベース */
                u8 wc_h = wr_head;
                u8 wc_c = wr_cyl;
                while (wr_rem > 0) {
                    u32 sec_data_len = 0;
                    u16 trk_spt = 0;
                    u32 data_off = d88_seek_sector(wc_c, wc_h, wc_r,
                                                   &sec_data_len, &trk_spt);
                    if (data_off == 0) {
                        DISK_ERROR_RETURN(log_entry, 0xC0, (i32)wc_r, regs);
                    }
                    wr_chunk = sec_data_len;
                    if (wr_chunk > wr_rem) wr_chunk = wr_rem;
                    vfs_seek(fdd_fd, data_off, 0);
                    vfs_write_fd(fdd_fd, wr_buf, wr_chunk);
                    wr_buf += wr_chunk;
                    wr_rem -= wr_chunk;
                    wr_off += (i32)wr_chunk;
                    wc_r++;
                    if (trk_spt > 0 && wc_r > (u8)trk_spt) {
                        wc_r = 1;
                        wc_h++;
                        if (wc_h >= 2) {
                            wc_h = 0;
                            wc_c++;
                        }
                    }
                }
                wfinal_cyl = wc_c;
                wfinal_head = wc_h;
                wfinal_sect_r = wc_r;
            } else {
                /* RAW/FDI ファイルモード: vfs_seek + vfs_write_fd */
                const struct fdc_geom *g = v86_disk_get_geom();
                if (wr_off >= 0 && (u32)wr_off < fdd_image_size) {
                    vfs_seek(fdd_fd, fdd_image_offset + (u32)wr_off, 0);
                }
                while (wr_rem > 0 && wr_off >= 0
                       && (u32)wr_off < fdd_image_size) {
                    wr_chunk = wr_rem;
                    if (wr_chunk > (u32)g->bps) wr_chunk = (u32)g->bps;
                    if ((u32)wr_off + wr_chunk > fdd_image_size) {
                        wr_chunk = fdd_image_size - (u32)wr_off;
                    }
                    vfs_write_fd(fdd_fd, wr_buf, wr_chunk);
                    wr_buf += wr_chunk;
                    wr_off += (i32)wr_chunk;
                    wr_rem -= wr_chunk;
                }
            }

            /* 成功 */
            log_entry->status = 0x00;
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;

            /* BDA FDC結果バッファ更新 (READと同一) */
            {
                u8 us = 0;
                u8 *rp = v86_phys_addr(0x0000, 0x0564 + us * 8);
                rp[0] = us;
                rp[1] = 0x00;
                rp[2] = 0x00;
                rp[3] = wfinal_cyl;
                rp[4] = wfinal_head;
                rp[5] = wfinal_sect_r;
                rp[6] = wr_seclen;
                rp[7] = wfinal_cyl;
            }
            break;
        }

        /* ============================================================ */
        /*  機能 07h: リキャリブレート / ベリファイ — 常に成功           */
        /* ============================================================ */
        case 0x07:
            log_entry->status = 0x00;
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;
            break;

        /* ============================================================ */
        /*  機能 00h: ドライブ状態チェック / リセット                    */
        /*  AH=00h/10h/20h 等 — 常に成功応答を返す                     */
        /*  Ys等のゲームがディスクアクセス前にドライブ状態を確認する    */
        /* ============================================================ */
        case 0x00:
            log_entry->status = 0x00;
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;
            break;

        /* ============================================================ */
        /*  機能 02h: 診断読み出し — READと同じ扱い                    */
        /* ============================================================ */
        case 0x02:
            /* 0x01/0x06と同じREAD処理にフォールスルーさせたいが、       */
            /* switchの制約でここに来る。成功応答のみ返す。             */
            log_entry->status = 0x00;
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
            regs[V86_REG_EFLAGS] &= ~1UL;
            break;

        /* ============================================================ */
        /*  機能 0Ah: READ ID — 物理セクタIDの読み出し                  */
        /*  NP21/W bios1b.c 準拠。FDCがトラック上のセクタIDを返す。     */
        /*  IO.SYSはSENSE後にREAD IDでN値(セクタ長コード)を取得し、    */
        /*  以後のREADのCHパラメータを決定する。                        */
        /*                                                                */
        /*  出力: CL = C, DH = H, DL = R(=1), CH = N(=3)               */
        /* ============================================================ */
        case 0x0A: {
            u8 cylinder_cl = (u8)(regs[V86_REG_ECX] & 0xFF);
            const struct fdc_geom *g = v86_disk_get_geom();

            /* 物理セクタIDを返す (現在メディアのN値を使用) */
            regs[V86_REG_ECX] = (u32)cylinder_cl          /* CL = C */
                              | ((u32)g->sec_n << 8);      /* CH = N */
            regs[V86_REG_EDX] = (regs[V86_REG_EDX] & 0xFFFF0000UL)
                              | (0UL << 8)                 /* DH = H = 0 */
                              | 1UL;                       /* DL = R = 1 (先頭セクタ) */
            regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL; /* AH = 0 */
            regs[V86_REG_EFLAGS] &= ~1UL;
            log_entry->status = 0x00;
            break;
        }

        default:
            {
                static int disk_unhandled_count = 0;
                if (disk_unhandled_count < 10) {
                    extern void serial_puts(const char *s);
                    char buf[16];
                    const char *hexc = "0123456789ABCDEF";
                    buf[0] = hexc[func >> 4];
                    buf[1] = hexc[func & 0xF];
                    buf[2] = '\0';
                    serial_puts("\r\n[V86 BIOS] Unhandled INT 1Bh AH=");
                    serial_puts(buf);
                    serial_puts("\r\n");
                    disk_unhandled_count++;
                }
            }
            log_entry->status = 0xFE;
            disk_log_idx = (disk_log_idx + 1) % V86_DISK_LOG_SIZE;
            disk_log_count++;
            return -1;
        } /* switch (func_base) */
    }

    /* ログバッファ更新 */
    disk_log_idx = (disk_log_idx + 1) % V86_DISK_LOG_SIZE;
    disk_log_count++;
    return 0;
}

/* ====================================================================== */
/*  v86_disk_dump_log — INT 1Bh 呼び出しログを kprintf でダンプ           */
/*  V86セッション終了後に呼び出すこと (Ring 0 で安全に実行可能)           */
/* ====================================================================== */
void v86_disk_dump_log(void)
{
    u32 n;
    u32 i;
    u32 start;

    n = (disk_log_count < V86_DISK_LOG_SIZE) ? disk_log_count : V86_DISK_LOG_SIZE;

    kprintf(0xA1, "[V86 DISK] INT 1Bh log: %d calls (showing last %d)\n",
            (int)disk_log_count, (int)n);

    if (n == 0) return;

    start = (disk_log_count <= V86_DISK_LOG_SIZE) ? 0 : disk_log_idx;

    for (i = 0; i < n; i++) {
        u32 idx = (start + i) % V86_DISK_LOG_SIZE;
        struct v86_disk_log_entry *e = &disk_log[idx];
        kprintf(0x07, "  [%d] AH=%x AL=%x C=%d CH=%d H=%d S=%d BX=%x ES:BP=%x:%x off=%d st=%x\n",
                (int)i,
                (unsigned)e->func,
                (unsigned)e->daua,
                (int)e->cylinder,
                (int)e->sector_len,
                (int)e->head,
                (int)e->sector,
                (unsigned)e->xfer_bytes,
                (unsigned)e->es,
                (unsigned)e->bp,
                (int)e->result_offset,
                (unsigned)e->status);
    }
}

/* ====================================================================== */
/*  v86_disk_reset_log — ログカウンタをリセット                            */
/* ====================================================================== */
void v86_disk_reset_log(void)
{
    disk_log_idx = 0;
    disk_log_count = 0;
}

/* ====================================================================== */
/*  v86_disk_get_log — ログバッファへのアクセサ                            */
/* ====================================================================== */
struct v86_disk_log_entry *v86_disk_get_log(u32 *count, u32 *idx)
{
    *count = disk_log_count;
    *idx = disk_log_idx;
    return disk_log;
}
