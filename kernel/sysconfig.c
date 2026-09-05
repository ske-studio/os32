/* ======================================================================== */
/*  SYSCONFIG.C — 最小の起動設定パーサ (K レーン K4)                         */
/*                                                                          */
/*  行指向 KEY=VALUE 設定 (/etc/system.cfg) を VFS から読み、整数値を返す。   */
/*  カーネルのシェル起動ループ (kernel.c) が GUI=0/1 を分岐するのに使う。     */
/*  設定ファイルは小さい前提 (数キー) なので固定バッファに一括で読む。        */
/*  仕様: docs/tasks/gui/TASK_K4_gui_boot.md、契約 T9。                       */
/* ======================================================================== */

#include "types.h"
#include "vfs.h"
#include "kstring.h"
#include "sysconfig.h"

/* 設定ファイルの読み込み上限 (十分に小さい前提)。 */
#define SYSCONFIG_MAX  1024

/* 符号付き十進を最低限だけ解釈する (先頭の +/- と連続する数字)。 */
static int sc_atoi(const char *s)
{
    int v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (int)(*s - '0');
        s++;
    }
    return neg ? -v : v;
}

int sysconfig_get_int(const char *path, const char *key, int def)
{
    static char buf[SYSCONFIG_MAX];   /* 起動ループ専用・単一スレッド */
    int n;
    int klen;
    int i;

    n = vfs_read(path, buf, (u32)(SYSCONFIG_MAX - 1));
    if (n <= 0) {
        return def;   /* ファイル無し / 空 / 読み取り失敗 */
    }
    buf[n] = '\0';
    klen = (int)kstrlen(key);

    i = 0;
    while (i < n) {
        int j;

        /* 行頭の空白を飛ばす */
        while (i < n && (buf[i] == ' ' || buf[i] == '\t')) {
            i++;
        }

        /* コメント行・空行はスキップ */
        if (i >= n || buf[i] == '#' || buf[i] == ';' ||
            buf[i] == '\n' || buf[i] == '\r') {
            while (i < n && buf[i] != '\n') {
                i++;
            }
            if (i < n) {
                i++;
            }
            continue;
        }

        /* KEY を先頭一致で照合 */
        if (kstrncmp(&buf[i], key, (u32)klen) == 0) {
            j = i + klen;
            /* KEY の直後は空白 or '=' のみ許可 (前方一致の誤検出を防ぐ) */
            while (j < n && (buf[j] == ' ' || buf[j] == '\t')) {
                j++;
            }
            if (j < n && buf[j] == '=') {
                j++;
                while (j < n && (buf[j] == ' ' || buf[j] == '\t')) {
                    j++;
                }
                return sc_atoi(&buf[j]);
            }
        }

        /* 次の行へ */
        while (i < n && buf[i] != '\n') {
            i++;
        }
        if (i < n) {
            i++;
        }
    }

    return def;
}
