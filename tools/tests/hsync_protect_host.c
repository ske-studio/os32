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
