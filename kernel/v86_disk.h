/* ======================================================================== */
/*  V86_DISK.H — V86 ディスクBIOS (INT 1Bh) 仮想化ヘッダ                   */
/*                                                                          */
/*  FDDイメージをメモリ上に保持し、INT 1Bh のセクタ読み書きを                */
/*  イメージデータからサーブする。                                           */
/*                                                                          */
/*  Phase B: loop_dev の薄いブリッジとして再設計。                           */
/*  新 API は loop_dev 経由でジオメトリ/I/Oを取得する。                      */
/*  旧 API は互換ラッパーとして残し、Phase B-4 完了後に削除予定。            */
/* ======================================================================== */

#ifndef V86_DISK_H
#define V86_DISK_H

#include "types.h"
#include "fdc.h"  /* fdc_media_t, struct fdc_geom */

/* PC-98 2HD FDD イメージサイズ (互換用) */
#define V86_FDD_IMAGE_SIZE  (77 * 2 * 8 * 1024)

/* ====================================================================== */
/*  新 API (Phase B: loop_dev ブリッジ)                                    */
/* ====================================================================== */

/* loop_dev スロットをアタッチ */
void v86_disk_attach_loop(int slot);

/* 実FDDモード (据え置き) */
void v86_disk_set_physical(int drv, fdc_media_t media);

/* クリア (loop_dev detach + 状態リセット) */
void v86_disk_clear(void);

/* loop_dev スロット番号取得 (-1=未設定) */
int  v86_disk_get_loop_slot(void);

/* ジオメトリ取得 (loop_dev 経由) */
int  v86_disk_get_geometry(u16 *cyls, u8 *heads, u8 *spt,
                           u16 *bps, u8 *sec_n, u8 *daua_high);

/* モード判定 */
int  v86_disk_is_loop(void);     /* 1=loop_dev 経由 */
int  v86_disk_is_phys(void);     /* 1=物理FDD */
int  v86_disk_get_phys_drv_num(void);  /* 物理ドライブ番号 */

/* ====================================================================== */
/*  旧 API (互換ラッパー — Phase B-4 完了後に削除予定)                     */
/* ====================================================================== */

/* FDDイメージファイル(FD)をセット */
void v86_disk_set_file(int fd, u32 data_offset, u32 data_size,
                       fdc_media_t media);

/* D88形式ディスクイメージをセット */
void v86_disk_set_d88(int fd, u32 file_size, fdc_media_t media);

/* 現在マウント中のジオメトリを返す (旧API) */
const struct fdc_geom *v86_disk_get_geom(void);

/* 内部状態アクセサ (旧API — v86_fdc.c の FORMAT TRACK 実装用) */
int v86_disk_is_physical(void);
int v86_disk_get_phys_drv(void);
int v86_disk_get_fd(void);
u32 v86_disk_get_offset(void);

/* ====================================================================== */
/*  INT 1Bh ハンドラ                                                       */
/* ====================================================================== */

/* INT 1Bh (ディスクBIOS) を処理する。
 * regs: V86スタックフレーム内レジスタ配列
 * 戻り値: 0=処理済み(V86続行), -1=未実装ファンクション */
int v86_bios_int1b(u32 *regs);

/* ====================================================================== */
/*  ディスクログエントリ (デバッグ用リングバッファ)                         */
/* ====================================================================== */
struct v86_disk_log_entry {
    u8  func;           /* AH (ファンクションコード) */
    u8  daua;           /* AL (DA/UA) */
    u8  cylinder;       /* CL (シリンダ番号, 0-76) */
    u8  sector_len;     /* CH (セクタ長コード, 3=1024B) */
    u8  head;           /* DH (ヘッド番号, 0-1) */
    u8  sector;         /* DL (セクタ番号, 1ベース) */
    u16 xfer_bytes;     /* BX (転送バイト数) */
    u16 es;             /* ES */
    u16 bp;             /* BP */
    i32 result_offset;  /* chs_to_offset の結果 (-1=エラー) */
    u8  status;         /* 応答 AH (0=成功, それ以外=エラー) */
    u8  pad;
};

/* デバッグ: INT 1Bh呼び出しログをkprintfでダンプ */
void v86_disk_dump_log(void);

/* デバッグ: INT 1Bhログカウンタをリセット */
void v86_disk_reset_log(void);

/* デバッグ: ログバッファへのアクセサ */
struct v86_disk_log_entry *v86_disk_get_log(u32 *count, u32 *idx);

#endif /* V86_DISK_H */
