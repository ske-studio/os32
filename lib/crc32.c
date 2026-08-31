/* ======================================================================== */
/*  crc32.c — 標準 CRC-32                                                   */
/*                                                                          */
/*  テーブルを持たないビット単位版。ホットデプロイの検証で使う程度の頻度   */
/*  なので 256 エントリのテーブル (1KB) をカーネルに常駐させる価値はない。  */
/* ======================================================================== */
#include "crc32.h"

u32 crc32_calc(const void *data, u32 size)
{
    const u8 *bytes = (const u8 *)data;
    u32 crc = 0xFFFFFFFFUL;
    u32 i;

    for (i = 0; i < size; i++) {
        int j;
        crc ^= bytes[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
