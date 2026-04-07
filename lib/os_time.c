#include "os_time.h"

static int is_leap_year(int y) {
    if (y % 400 == 0) return 1;
    if (y % 100 == 0) return 0;
    if (y % 4 == 0) return 1;
    return 0;
}

static const int days_in_month[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

os_time_t datetime_to_epoch(int year, int month, int day, int hour, int min, int sec) {
    int y, m, i;
    u32 days = 0;
    os_time_t epoch;
    int leap;
    
    /* 1970年からの経過日数を計算 */
    for (y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    
    /* 年内の経過日数を計算 */
    m = (month < 1) ? 1 : ((month > 12) ? 12 : month);
    leap = is_leap_year(year);
    for (i = 0; i < m - 1; i++) {
        days += days_in_month[leap][i];
    }
    
    days += (day > 0 ? day - 1 : 0);
    
    /* 日数を秒に変換し、時間・分・秒を足す */
    epoch = days * 86400UL;
    epoch += hour * 3600UL;
    epoch += min * 60UL;
    epoch += sec;
    
    return epoch;
}

os_time_t dos_time_to_epoch(u16 dos_date, u16 dos_time) {
    /* dos_date: 0-4=day, 5-8=month, 9-15=year-1980 */
    int day   = (dos_date & 0x001F);
    int month = ((dos_date >> 5) & 0x000F);
    int year  = ((dos_date >> 9) & 0x007F) + 1980;
    
    /* dos_time: 0-4=sec/2, 5-10=min, 11-15=hour */
    int sec   = (dos_time & 0x001F) * 2;
    int min   = ((dos_time >> 5) & 0x003F);
    int hour  = ((dos_time >> 11) & 0x001F);
    
    return datetime_to_epoch(year, month, day, hour, min, sec);
}
