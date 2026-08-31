#ifndef LIB_CRC32_H
#define LIB_CRC32_H

#include "types.h"

/* 標準 CRC-32 (多項式 0xEDB88320, 初期値 0xFFFFFFFF, 最終 XOR 0xFFFFFFFF)。
 * Python の zlib.crc32 / PNG / zip と同じ値になる。
 *
 * 同じ計算がユーザーランド側の userland/lib/save/save_meta.c にもある
 * (libos32save の save_crc32)。あちらはセーブデータ形式に埋め込まれて
 * いるためビルドを分けてある。値は一致するので相互に検証してよい。 */
u32 crc32_calc(const void *data, u32 size);

#endif /* LIB_CRC32_H */
