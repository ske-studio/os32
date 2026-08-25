/* ======================================================================== */
/*  TEXT_VAR.C — 変数テーブル・変数展開処理                                 */
/*                                                                          */
/*  テキスト内の {0} {1} ... {15} を変数テーブルの値で置換する。             */
/* ======================================================================== */

#include "libos32text.h"

/* ====================================================================== */
/*  変数テーブル (グローバル — text_core.c から extern 参照)                */
/* ====================================================================== */

TextVar text_vars[TEXT_MAX_VARS];

/* ====================================================================== */
/*  API — 変数設定                                                          */
/* ====================================================================== */

void text_set_var(int var_id, const char *value)
{
    int i;

    if (var_id < 0 || var_id >= TEXT_MAX_VARS)
        return;
    if (!value) {
        text_vars[var_id].value[0] = '\0';
        return;
    }

    i = 0;
    while (value[i] && i < TEXT_VAR_SIZE - 1) {
        text_vars[var_id].value[i] = value[i];
        i++;
    }
    text_vars[var_id].value[i] = '\0';
}

void text_clear_vars(void)
{
    int i;
    for (i = 0; i < TEXT_MAX_VARS; i++)
        text_vars[i].value[0] = '\0';
}

/* ====================================================================== */
/*  内部: 変数展開                                                          */
/* ====================================================================== */

/* 文字列長を返す */
static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* バッファ内の {N} を text_vars[N].value で置換
 * buf: 入力兼出力バッファ (インプレース展開)
 * buf_size: バッファ最大サイズ
 * 戻り値: 展開後のテキスト長 */
int text__expand_vars(char *buf, int buf_size)
{
    char tmp[TEXT_BUF_SIZE];
    int src = 0;
    int dst = 0;
    int orig_len;

    orig_len = slen(buf);

    while (src < orig_len && dst < buf_size - 1) {
        /* {N} パターン検出 */
        if (buf[src] == '{') {
            int var_id = 0;
            int num_start = src + 1;
            int num_end = num_start;
            int valid = 0;

            /* 数値読み取り */
            while (num_end < orig_len && buf[num_end] >= '0' && buf[num_end] <= '9')
                num_end++;

            if (num_end > num_start && num_end < orig_len && buf[num_end] == '}') {
                /* 数値をパース */
                int j;
                var_id = 0;
                for (j = num_start; j < num_end; j++)
                    var_id = var_id * 10 + (buf[j] - '0');

                if (var_id >= 0 && var_id < TEXT_MAX_VARS && text_vars[var_id].value[0]) {
                    /* 変数値をコピー */
                    const char *val = text_vars[var_id].value;
                    int vi = 0;
                    while (val[vi] && dst < buf_size - 1)
                        tmp[dst++] = val[vi++];
                    src = num_end + 1; /* '}' の次 */
                    valid = 1;
                }
            }

            if (!valid) {
                /* マッチしなかった場合はそのまま '{' をコピー */
                tmp[dst++] = buf[src++];
            }
        } else {
            tmp[dst++] = buf[src++];
        }
    }
    tmp[dst] = '\0';

    /* 結果をバッファに書き戻し */
    {
        int i;
        for (i = 0; i <= dst; i++)
            buf[i] = tmp[i];
    }

    return dst;
}
