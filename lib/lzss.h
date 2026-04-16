#ifndef LZSS_H
#define LZSS_H

#include "types.h"

/* 
 * LZSS圧縮 ストリーム展開ルーチン
 * getc_cb: 1バイト読み込みコールバック (エラー時 -1を返すこと)
 * src_len: 圧縮データのサイズ
 * dst: 展開先バッファ
 * dst_size: 展開先バッファの最大サイズ (または展開後の元のサイズ)
 * 戻り値: 実際に展開されたバイト数 または エラー時 -1
 */
typedef int (*lzss_getc_cb)(void);
int lzss_decode_stream(lzss_getc_cb getc_cb, u32 src_len, u8 *dst, u32 dst_size);

/*
 * LZSS圧縮 エンコーダ
 * src:      入力データ
 * src_len:  入力データサイズ
 * dst:      圧縮先バッファ
 * dst_size: 圧縮先バッファの最大サイズ
 * 戻り値:   圧縮後のバイト数 または エラー時 -1
 */
int lzss_encode(const u8 *src, u32 src_len, u8 *dst, u32 dst_size);

#endif /* LZSS_H */
