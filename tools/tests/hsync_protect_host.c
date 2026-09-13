/* ========================================================================
 *  hsync_protect_host.c — hsync の「コピー先が /etc/settings.db* か」の
 *  字句判定を **実物のソースで** 確かめる (票 S0-D / D0)
 *
 *  対象票: docs/tasks/settings/TASK_S0.md §2
 *  実行:   python3 -B tools/tests/test_hsync_protect.py
 *  記録:   tools/tests/s0_tdd.md 節 D
 *
 *  userland/system/hsync_protect.inc を 1 行も写さずそのまま #include する
 *  (模型ではない)。判定は KernelAPI にも FS にも触らない純関数なので、ホストで
 *  そのまま走らせられる。実体規則 (inode 比較) は sys_stat が要るので
 *  hsync.c 側に残っており、ここでは見ない。
 * ======================================================================== */

#include <stdio.h>
#include <string.h>

#include "hsync_protect.inc"

static int failures;

static void check(int cond, const char *name)
{
    printf("  %s %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) failures++;
}

static void check_norm(const char *in, const char *want, const char *name)
{
    char out[HSP_MAX_PATH];
    int ok = hsp_normalize(in, out, (int)sizeof(out));
    if (!ok) {
        printf("  FAIL %s (正規化できない: %s)\n", name, in);
        failures++;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("  FAIL %s (%s -> %s, 期待 %s)\n", name, in, out, want);
        failures++;
        return;
    }
    printf("  ok   %s\n", name);
}

int main(void)
{
    char joined[HSP_MAX_PATH];

    printf("== 字句正規化 ==\n");
    check_norm("/etc/settings.db", "/etc/settings.db", "そのまま");
    check_norm("/etc/./settings.db", "/etc/settings.db", "'./' を畳む");
    check_norm("//etc///settings.db", "/etc/settings.db", "連続 '/' を畳む");
    check_norm("/etc/sub/../settings.db", "/etc/settings.db", "'..' を戻す");
    check_norm("etc/settings.db", "/etc/settings.db", "相対を絶対に");
    check_norm("/etc/settings.db/", "/etc/settings.db",
               "末尾 '/' (mkdir 経路)");
    check(hsp_normalize("/../etc/settings.db", joined,
                        (int)sizeof(joined)) == 0,
          "root を越える '..' は失敗");

    printf("== 名前規則 ==\n");
    check(hsp_path_protected("/etc/settings.db"), "/etc/settings.db");
    check(hsp_path_protected("/etc/settings.db-journal"), "journal");
    check(hsp_path_protected("/etc/settings.db-wal"), "wal");
    check(hsp_path_protected("/etc/settings.db-shm"), "shm");
    check(hsp_path_protected("/etc/settings.db.bak"), "bak");
    check(hsp_path_protected("/etc/./settings.db"), "'./' 経由でも守る");
    check(hsp_path_protected("/etc/sub/../settings.db"), "'..' 経由でも守る");
    check(hsp_path_protected("//etc//settings.db"), "連続 '/' でも守る");
    check(hsp_path_protected("/ETC/SETTINGS.DB"), "大文字でも守る");
    check(hsp_path_protected("/etc/Settings.Db-Journal"), "混在大文字");
    check(hsp_path_protected("/etc/settings.db/"),
          "保護対象名のディレクトリも作らせない");

    printf("== 通してよいもの ==\n");
    check(!hsp_path_protected("/etc/settings.tsv"), "初期値 tsv は通す");
    check(!hsp_path_protected("/etc/motd"), "/etc の他のファイル");
    check(!hsp_path_protected("/etc/sub/settings.db"),
          "親が /etc でなければ対象外");
    check(!hsp_path_protected("/settings.db"), "ルート直下は対象外");
    check(!hsp_path_protected("/bin/sh.bin"), "普通のバイナリ");
    check(!hsp_path_protected("/etc"), "/etc そのものは作ってよい");

    printf("== /etc 列挙の一致判定 (実体規則の入口、往復 1 の B5) ==\n");
    /* sys_ls が返す**実在名**を大文字小文字を無視して拾えること。
     * 小文字 5 名を決め打ちで stat するだけでは SETTINGS.DB を取りこぼし、
     * そこへの hardlink を hsync -f bin が上書きしていた。 */
    check(hsp_is_protected_basename("settings.db"), "小文字そのまま");
    check(hsp_is_protected_basename("SETTINGS.DB"), "全部大文字");
    check(hsp_is_protected_basename("Settings.Db"), "混在");
    check(hsp_is_protected_basename("SETTINGS.DB-JOURNAL"), "大文字 journal");
    check(hsp_is_protected_basename("settings.db.BAK"), "大文字 bak");
    check(hsp_is_protected_basename("settings.db-WAL"), "大文字 wal");
    check(hsp_is_protected_basename("settings.db-Shm"), "混在 shm");
    check(!hsp_is_protected_basename("settings.tsv"), "tsv は拾わない");
    check(!hsp_is_protected_basename("settings.db2"), "接頭一致では拾わない");
    check(!hsp_is_protected_basename("ettings.db"), "部分一致では拾わない");
    check(!hsp_is_protected_basename(""), "空文字");

    printf("== 祖先の保護 (往復 2 の 2) ==\n");
    /* `/etc/settings.db` がディレクトリのとき、その中への宛先は最終要素だけ
     * 見ても素通りする (親が /etc ではないので名前規則に当たらない)。 */
    check(hsp_path_protected("/etc/settings.db/sub"), "保護対象の直下");
    check(hsp_path_protected("/etc/settings.db/sub/file"), "保護対象の孫");
    check(hsp_path_protected("/etc/SETTINGS.DB/inner"), "大文字の祖先");
    check(hsp_path_protected("/etc/settings.db-journal/x"), "journal の直下");
    check(hsp_path_protected("/etc/./settings.db/../settings.db/x"),
          "正規化してから祖先を見る");
    check(!hsp_path_protected("/etc/settings.tsv/x"), "tsv は祖先でも対象外");
    check(!hsp_path_protected("/etc/sub/settings.db2/x"), "別名の祖先");

    printf("== 長いパスの連結 (往復 2 の 5) ==\n");
    {
        /* `hsync ./././...etc` 相当。連結が容量を越えたら**判定より前に**
         * 止まる必要がある。純関数側は「正規化できない = 保護側」に倒す。
         * バッファは "a/" x HSP_MAX_PATH + "x" + NUL = 2*HSP_MAX_PATH+2 必要
         * (2 倍では 2 バイト足りずに試験自身が溢れていた)。 */
        static char longp[HSP_MAX_PATH * 2 + 16];
        int n;
        longp[0] = '\0';
        for (n = 0; n < HSP_MAX_PATH; n++) strcat(longp, "a/");
        strcat(longp, "x");
        check(hsp_normalize(longp, joined, (int)sizeof(joined)) == 0,
              "容量を越える正規化は失敗する");
        check(hsp_path_protected(longp), "正規化できないものは保護側");
    }

    printf("== 名前の長さ (往復 3 の D6) ==\n");
    {
        /* ls_cb は名前を NAME_CAP(64) のバッファに写す。切り詰めて写すと
         * **別のファイル**を作って「成功」と出るので、収まらないものは
         * 取り込まずにエラーへ回す。 */
        char name[128];
        int n;

        check(hsp_name_fits("sh.bin", 64), "普通の名前");
        check(hsp_name_fits("", 64), "空文字");
        for (n = 0; n < 63; n++) name[n] = 'a';
        name[63] = '\0';
        check(hsp_name_fits(name, 64), "63 文字ちょうどは収まる");
        name[63] = 'a';
        name[64] = '\0';
        check(!hsp_name_fits(name, 64), "64 文字は収まらない");
        for (n = 0; n < 127; n++) name[n] = 'b';
        name[127] = '\0';
        check(!hsp_name_fits(name, 64), "127 文字は収まらない");
        check(!hsp_name_fits(0, 64), "NULL");
    }

    printf("== -f の subdir 連結 (hsync.c:254 相当) ==\n");
    /* `hsync -f etc` は dst = "/" + "etc"、その下に settings.db を作る */
    strcpy(joined, "/");
    strcat(joined, "etc");
    check(!hsp_path_protected(joined), "hsync -f etc の dst 自体は /etc");
    strcat(joined, "/");
    strcat(joined, "settings.db");
    check(hsp_path_protected(joined), "hsync -f etc の下の settings.db");

    /* `hsync -f etc/` のような指定でも連結後に守れること */
    strcpy(joined, "/");
    strcat(joined, "etc/");
    strcat(joined, "/settings.db-journal");
    check(hsp_path_protected(joined), "hsync -f etc/ の journal");

    /* `hsync ..` のような指定は正規化に失敗し、保護側 (= コピーしない) */
    strcpy(joined, "/");
    strcat(joined, "../etc/settings.db");
    check(hsp_path_protected(joined), "'..' 混じりは保護側へ倒す");

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
