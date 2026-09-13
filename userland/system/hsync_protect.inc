/* ========================================================================
 *  hsync_protect.inc — hsync が /etc/settings.db* を壊さないための字句判定
 *
 *  票: docs/tasks/settings/TASK_S0.md §2 (S0-D)、契約は S0_FOUNDATION.md §5 (D0)
 *  ホスト試験: tools/tests/hsync_protect_host.c / tools/tests/test_hsync_protect.py
 *
 *  hsync は HostDrv (/host = C:\os32) の中身を NHD の / へ O_CREAT|O_TRUNC で
 *  写す。HostDrv に古い etc/settings.db が 1 つ残っているだけで、ゲストが書いた
 *  設定 DB が切り詰められる。ここは**コピー先の文字列**を判定するだけの純関数で、
 *  KernelAPI にも FS にも触らない (だからホストでそのまま走らせて試験できる)。
 *
 *  判定は 2 段のうちの「名前規則」。字句正規化 (`./` `..` 連続 `/`、-f の subdir
 *  連結) をしてから、親が /etc で basename が保護対象名なら真 (大文字小文字は
 *  区別しない)。実体規則 (inode 比較) は sys_stat が要るので hsync.c 側にある。
 *
 *  [C1] C89 / GNU89。宣言はブロック先頭、`//` コメント無し。
 * ======================================================================== */

#ifndef HSP_MAX_PATH
#define HSP_MAX_PATH 512          /* OS32_MAX_PATH (256) より広く取る */
#endif
#define HSP_MAX_DEPTH 32

/* 保護対象のベース名。末尾は番兵 */
static const char *hsp_protected_names[] = {
    "settings.db",
    "settings.db-journal",
    "settings.db-wal",
    "settings.db-shm",
    "settings.db.bak",
    0
};

static int hsp_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

/* 大文字小文字を無視した完全一致 */
static int hsp_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (hsp_lower((unsigned char)*a) != hsp_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* 字句正規化。out には先頭 '/' 付きの正規パスが入る。
 * 戻り値 1 = 成功、0 = 失敗 (溢れ / '..' が root を越えた)。 */
static int hsp_normalize(const char *in, char *out, int cap)
{
    int starts[HSP_MAX_DEPTH];
    int depth = 0;
    int len = 0;
    int i = 0;

    if (!in || !out || cap < 2) return 0;
    out[0] = '\0';

    while (in[i]) {
        int seg_begin;
        int seg_len;
        int k;

        while (in[i] == '/') i++;
        if (!in[i]) break;

        seg_begin = i;
        while (in[i] && in[i] != '/') i++;
        seg_len = i - seg_begin;

        if (seg_len == 1 && in[seg_begin] == '.') continue;
        if (seg_len == 2 && in[seg_begin] == '.' && in[seg_begin + 1] == '.') {
            if (depth == 0) return 0;       /* root の外へ出る */
            depth--;
            len = starts[depth];
            out[len] = '\0';
            continue;
        }
        if (depth >= HSP_MAX_DEPTH) return 0;
        if (len + 1 + seg_len + 1 > cap) return 0;

        starts[depth] = len;
        depth++;
        out[len++] = '/';
        for (k = 0; k < seg_len; k++) out[len++] = in[seg_begin + k];
        out[len] = '\0';
    }

    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 1;
}

/* ベース名 1 個が保護対象名かを大文字小文字を無視して見る。
 * `/etc` を列挙して実在名を拾うときの一致判定 (往復 1 の B5) — 小文字の表と
 * 突き合わせるだけでは `/etc/SETTINGS.DB` を取りこぼす。 */
static int hsp_is_protected_basename(const char *name)
{
    int i;

    if (!name) return 0;
    for (i = 0; hsp_protected_names[i]; i++)
        if (hsp_ieq(name, hsp_protected_names[i])) return 1;
    return 0;
}

/* 正規化済みパスに名前規則を当てる (親 /etc + 保護対象名) */
static int hsp_name_protected(const char *norm)
{
    char dir[HSP_MAX_PATH];
    const char *base;
    int last = -1;
    int i = 0;

    while (norm[i]) {
        if (norm[i] == '/') last = i;
        i++;
    }
    if (last < 0) return 0;
    base = norm + last + 1;

    if (last == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (last >= HSP_MAX_PATH) return 0;
        for (i = 0; i < last; i++) dir[i] = norm[i];
        dir[last] = '\0';
    }
    if (!hsp_ieq(dir, "/etc")) return 0;

    return hsp_is_protected_basename(base);
}

/* 最終行のラベル。失敗があるのに "Done:" と出すと、終了コードを見ない目には
 * 成功に読める ("失敗時に成功表示へ進めない"、FOUNDATION §5)。
 * 件数はそのまま出すので、変えるのは頭の 1 語だけ。 */
static const char *hsp_final_label(int errors)
{
    return errors ? "FAILED:" : "Done:";
}

/* 名前が cap バイト (NUL 込み) のバッファに収まるか。
 * 戻り値 1 = 収まる / 0 = 収まらない。
 * 切り詰めた名前で写すと**別のファイル**を作って「成功」と出るので、
 * 呼び手は 0 のときエラーにすること (往復 3 の D6)。 */
static int hsp_name_fits(const char *name, int cap)
{
    int i = 0;

    if (!name || cap < 1) return 0;
    while (name[i]) {
        i++;
        if (i > cap - 1) return 0;
    }
    return 1;
}

/* 正規化済みパスの**祖先**に保護対象が居るか (path 自身は見ない)。
 * `/etc/settings.db/sub` のような宛先は、最終要素だけ見ても素通りする
 * (親が `/etc/settings.db` なので名前規則に当たらない、往復 2 の 2)。 */
static int hsp_ancestor_protected(const char *norm)
{
    char prefix[HSP_MAX_PATH];
    int i;

    for (i = 1; norm[i]; i++) {
        if (norm[i] != '/') continue;
        if (i >= HSP_MAX_PATH) return 1;    /* 判定できない = 保護側 */
        {
            int k;
            for (k = 0; k < i; k++) prefix[k] = norm[k];
            prefix[i] = '\0';
        }
        if (hsp_name_protected(prefix)) return 1;
    }
    return 0;
}

/* 入口: 任意のコピー先 / mkdir 先の文字列が保護対象か (自身または祖先)。
 * 正規化できない (異常に長い / root を越える) ものは**保護側**に倒す。 */
static int hsp_path_protected(const char *path)
{
    char norm[HSP_MAX_PATH];

    if (!hsp_normalize(path, norm, (int)sizeof(norm))) return 1;
    if (hsp_name_protected(norm)) return 1;
    return hsp_ancestor_protected(norm);
}
