/* ======================================================================== */
/*  LOOP_DEV.H — 汎用ループバックブロックデバイス                            */
/*                                                                          */
/*  ディスクイメージファイル (D88/FDI/RAW等) を lo0..lo3 ブロック            */
/*  デバイスとして公開する。拡張子またはヘッダ解析でフォーマットを自動判別。  */
/*                                                                          */
/*  CHS コア設計: PC-98 のハードウェアは全て CHS ネイティブであるため、      */
/*  CHS レベルの API をコアとして公開し、LBA は薄いラッパーで提供する。      */
/* ======================================================================== */

#ifndef LOOP_DEV_H
#define LOOP_DEV_H

#include "types.h"

/* ループバックデバイス初期化 (lo0..lo3 を dev_register) */
void loop_dev_init(void);

/* パスベースのアタッチ (拡張子でフォーマット判別)
 * 戻り値: 0=成功, -1=パス/スロット不正, -2=フォーマット不正,
 *         -3=スロット使用中, -4=I/Oエラー */
int  loop_dev_attach(const char *vfs_path, int slot);

/* fd ベースのアタッチ (ヘッダ解析でフォーマット自動判別)
 * 既に vfs_open 済みの fd を直接スロットにアタッチする。
 * fd の所有権は呼び出し側に残る (detach 時に close しない)。
 * 戻り値: 0=成功, -1=fd/スロット不正, -2=フォーマット不正,
 *         -3=スロット使用中, -4=I/Oエラー */
int  loop_dev_attach_fd(int fd, int slot);

/* スロットをデタッチ (owns_fd の場合のみ fd をクローズ) */
void loop_dev_detach(int slot);

/* スロット状態照会
 * 戻り値: 1=アタッチ中, 0=未使用
 * out_total: 総LBA数, out_bps: セクタサイズ(bytes) */
int  loop_dev_status(int slot, u32 *out_total, int *out_bps);

/* ================================================================== */
/*  CHS コア API                                                       */
/* ================================================================== */

/* CHS セクタ読み出し (全フォーマット共通)
 * D88: トラックテーブル走査 → セクタヘッダ照合 → データ読み出し
 * FDI/RAW: オフセット計算 → vfs_read
 * sect は 1-based (ATA/FDC 準拠)
 * 戻り値: 0=成功, -1=セクタ不在/エラー */
int loop_dev_read_chs(int slot, u8 cyl, u8 head, u8 sect, void *buf);

/* D88 専用: FDC エミュレーション用 CHS セクタ検索
 *
 * µPD765A の READ DATA コマンドをエミュレートする際に使用。
 * トラック選択 (trk_cyl/trk_head): SEEK で設定された物理位置
 * セクタID照合 (id_c/id_h/id_r): コマンドの論理 C/H/R
 *
 * Ys 等のコピープロテクションでは:
 *   SEEK cyl=1 → READ C=0, H=1, R=1
 * のように物理トラックと論理IDが意図的に異なる。
 *
 * 戻り値: セクタデータのファイルオフセット (0=未発見)
 * out_data_len: セクタデータ長 (128<<N)
 * out_spt: トラック内セクタ数 */
u32 loop_dev_seek_d88(int slot,
                      u8 trk_cyl, u8 trk_head,
                      u8 id_c, u8 id_h, u8 id_r,
                      u32 *out_data_len, u16 *out_spt);

/* スロットの VFS fd を取得 (-1 = 未アタッチ) */
int loop_dev_get_fd(int slot);

/* ジオメトリ取得
 * 戻り値: 0=成功, -1=未アタッチ */
int loop_dev_get_geometry(int slot, u8 *cyls, u8 *heads, u8 *spt,
                          u16 *bps, u32 *total_lba);

#endif /* LOOP_DEV_H */
