/* ======================================================================== */
/*  V86_DISK.H — V86 ディスクBIOS (INT 1Bh) 仮想化ヘッダ                   */
/*                                                                          */
/*  FDDイメージをメモリ上に保持し、INT 1Bh のセクタ読み書きを                */
/*  イメージデータからサーブする。                                           */
/* ======================================================================== */

#ifndef V86_DISK_H
#define V86_DISK_H

#include "types.h"

/* PC-98 2HD FDD ジオメトリ */
#define V86_FDD_CYLINDERS   77
#define V86_FDD_HEADS       2
#define V86_FDD_SPT         8       /* セクタ/トラック */
#define V86_FDD_BPS         1024    /* バイト/セクタ */
#define V86_FDD_TOTAL_SEC   (V86_FDD_CYLINDERS * V86_FDD_HEADS * V86_FDD_SPT)
#define V86_FDD_IMAGE_SIZE  (V86_FDD_TOTAL_SEC * V86_FDD_BPS)

/* DA/UA: 1MB FDD UNIT#0 */
#define V86_FDD_DAUA        0x90

/* FDDイメージファイル(FD)をセット (fd: VFSのファイルディスクリプタ、
 * data_offset: イメージデータ開始位置、size: バイト数)
 * 呼び出し後、V86からのINT 1Bhでこのファイルからセクタが読み出される。 */
void v86_disk_set_file(int fd, u32 data_offset, u32 data_size);

/* FDDイメージをクリア */
void v86_disk_clear(void);

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

/* デバッグ: INT 1Bh呼び出しログをkprintfでダンプ
 * V86セッション終了後に呼び出すこと */
void v86_disk_dump_log(void);

/* デバッグ: INT 1Bhログカウンタをリセット */
void v86_disk_reset_log(void);

/* デバッグ: ログバッファへのアクセサ */
struct v86_disk_log_entry *v86_disk_get_log(u32 *count, u32 *idx);

#endif /* V86_DISK_H */
