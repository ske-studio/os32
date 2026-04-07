#ifndef CMD_FS_SHARED_H
#define CMD_FS_SHARED_H
#include "shell.h"

__attribute__((unused)) static void dummy_ls_cb(const DirEntry_Ext *entry, void *ctx) {
    (void)entry;
    *(int*)ctx = 1;
}

__attribute__((unused)) static const char *get_basename(const char *path) {
    const char *p = path;
    const char *base = path;
    while (*p) {
        if (*p == '/' || *p == '\\') base = p + 1;
        p++;
    }
    return base;
}

__attribute__((unused)) static void format_size(u32 size, char *buf, int max_len) {
    int pos = 0, i; char temp[16]; u32 s = size;
    (void)max_len;
    if (s == 0) temp[pos++] = '0';
    while (s > 0) { temp[pos++] = '0' + (s % 10); s /= 10; }
    for (i = 0; i < pos / 2; i++) { char t = temp[i]; temp[i] = temp[pos - 1 - i]; temp[pos - 1 - i] = t; }
    for (i = 0; i < pos; i++) buf[i] = temp[i];
    buf[pos] = 0;
}
#endif
