/* ======================================================================== */
/*  GREP.C -- パターンマッチフィルタ                                          */
/*                                                                          */
/*  Usage: grep PATTERN [FILE...]                                            */
/*  stdin からも読み取り可能 (パイプ対応)                                     */
/* ======================================================================== */
#include "os32api.h"
#include <string.h>
#include <stdio.h>

static KernelAPI *api;
static int match_count = 0;

/* 簡易部分文字列検索 */
static int str_contains(const char *haystack, const char *needle)
{
    int hlen, nlen, i, j;
    if (!needle[0]) return 1;
    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (i = 0; i <= hlen - nlen; i++) {
        int ok = 1;
        for (j = 0; j < nlen; j++) {
            if (haystack[i + j] != needle[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* バッファ内の各行を検索してマッチした行を出力 */
static void grep_buffer(const char *buf, int len, const char *pattern, const char *filename, int show_filename)
{
    char line[1024];
    int li = 0;
    int i;

    for (i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            line[li] = '\0';
            if (str_contains(line, pattern)) {
                if (show_filename) {
                    printf("%s:", filename);
                }
                printf("%s\n", line);
                match_count++;
            }
            li = 0;
        } else if (li < 1022) {
            line[li++] = buf[i];
        }
    }
}

/* ファイルを読み取って grep */
static void grep_file(const char *path, const char *pattern, int show_filename)
{
    static char buf[65536];
    int fd, sz;

    fd = api->sys_open(path, KAPI_O_RDONLY);
    if (fd < 0) {
        printf("grep: %s: No such file\n", path);
        return;
    }
    sz = api->sys_read(fd, buf, sizeof(buf) - 1);
    api->sys_close(fd);
    if (sz <= 0) return;
    buf[sz] = '\0';
    grep_buffer(buf, sz, pattern, path, show_filename);
}

/* stdin から読み取って grep */
static void grep_stdin(const char *pattern)
{
    static char buf[65536];
    int sz;

    sz = api->sys_read(0, buf, sizeof(buf) - 1);
    if (sz <= 0) return;
    buf[sz] = '\0';
    grep_buffer(buf, sz, pattern, "(stdin)", 0);
}

int main(int argc, char **argv, KernelAPI *kapi)
{
    int i;
    api = kapi;

    if (argc < 2) {
        printf("Usage: grep PATTERN [FILE...]\n");
        return 1;
    }

    if (argc == 2) {
        /* パターンのみ: stdin から読む */
        if (api->sys_isatty(0)) {
            printf("Usage: grep PATTERN [FILE...]\n");
            return 1;
        }
        grep_stdin(argv[1]);
    } else {
        int show_fn = (argc > 3);
        for (i = 2; i < argc; i++) {
            grep_file(argv[i], argv[1], show_fn);
        }
    }

    return (match_count > 0) ? 0 : 1;
}
