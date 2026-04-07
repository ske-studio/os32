#ifndef OS_TIME_H
#define OS_TIME_H

#include "os32_kapi_shared.h"

/* 日時から UNIX Epoch への変換 */
os_time_t datetime_to_epoch(int year, int month, int day, int hour, int min, int sec);

/* MS-DOS フォーマットの Date と Time から UNIX Epoch への変換 */
os_time_t dos_time_to_epoch(u16 dos_date, u16 dos_time);

#endif /* OS_TIME_H */
