#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
/* ======================================================================== */
/*  HSYNC.C — HostDrv同期コマンド                                           */
/*                                                                          */
/*  /host (HostDrvマウントポイント) の内容を ext2 ルート (/) に同期する。     */
/*                                                                          */
/*  使い方:                                                                 */
/*    hsync              — /host 配下を / に同期 (**sys は除く**)           */
/*    hsync bin           — /host/bin/ → /bin/ のみ同期                    */
/*    hsync -n            — dry-run (読んで比べるだけ、1 バイトも書かない)  */
/*    hsync -v            — 判定理由まで出す                               */
/*    hsync -f            — 同一判定を省いて全上書き (sys は除く)           */
/*    hsync sys           — /sys を明示指定したときだけ同期する             */
/*    hsync -f sys        — /host/sys/ を強制同期                          */
/*                                                                          */
/*  **既定の同一判定は「サイズ + 内容のバイト比較」** (票 H1、設計書        */
/*  docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md §1 / §3.1)。                 */
/*  以前はサイズが同じなら中身を見ずにスキップしていたので、長さを変えずに   */
/*  ヘッダだけ変わった shlib (2026-09-14 の libos32gui.shlib) が更新されず、 */
/*  「配備したのに古いまま」が起きた。mtime は**同一の根拠に使わない** —     */
/*  同じ秒の再ビルド・touch・過去コミットへの checkout で簡単に嘘になる。    */
/*                                                                          */
/*  **既定で sys を外す理由**: /sys には稼働中の常駐シェル (shell.bin)、     */
/*  共有ライブラリ (lib/)、unicode.bin、フォントが入っている。走っている     */
/*  ものを背後から差し替えると、次の exec まで実体と食い違う。入れ替えたい   */
/*  ときは `hsync sys` と明示する (2026-09-09、ホットデプロイ撤去に伴い)。   */
/*  除外は `-f` でも解除しない。                                            */
/*                                                                          */
/*  ★ H1 の限界 (解消は票 H2): 宛先を O_CREAT|O_TRUNC で**直接**開いて      */
/*     上書きする。コピー途中の I/O 失敗や読戻し検証の失敗が起きたとき、     */
/*     **旧宛先を復元する保証は無い** (切り詰め・書きかけのまま残り得る)。   */
/*     一時ファイルへ書いて検証してから置換する方式は H2。                   */
/* ======================================================================== */

#include "os32api.h"

/* コピー先が /etc/settings.db* かを字句で見る純関数 (票 S0-D / D0)。
 * HostDrv に古い settings.db が残っていても NHD の本体を切り詰めない。
 * 実体は userland/system/hsync_protect.inc、ホスト試験は
 * tools/tests/test_hsync_protect.py。 */
#include "hsync_protect.inc"

/* CRC-32 のストリーム核 (カーネルと共用、票 H1 / 設計書 §4.1)。
 * KAPI は増やさない — カーネル内の crc32_calc を番地で呼ぶのでもなく、
 * 同じ .inc をこちら側でもコンパイルして同じ値を得る。 */
#include "lib/crc32_core.inc"

/* コピーは 64KB 単位。比較はその 64KB を 32KB x 2 に割って使う
 * (設計書 §4.3: ファイル全体を確保しない)。 */
#define FILE_BUF_SIZE  (64 * 1024)
#define CMP_BUF_SIZE   (32 * 1024)
#define MAX_FILES      128
#define MAX_DEPTH      8

/* sys_read / sys_write は int を返すので、2GiB 以上は 1 回の長さで表せない。
 * 黙って切り詰めず、扱えないと言って断る (設計書 §4.3)。 */
#define HS_MAX_FILE_SIZE 0x7FFFFFFFUL

/* ---- 理由コード。**固定文字列**で出す (設計書 §7.2) ------------------- */
/* テスターが文言の雰囲気で判定しないよう、ここ以外で組み立てないこと。 */
#define HR_NEW          "new_file"          /* 宛先が無い */
#define HR_SIZE         "size_changed"      /* サイズが違う */
#define HR_CONTENT      "content_changed"   /* サイズ同じ・内容が違う */
#define HR_FORCED       "forced"            /* -f で比較を省いた */
#define HR_VERIFY       "verify_failed"     /* 読戻しの長さ / CRC が合わない */
#define HR_SOURCE       "source_changed"    /* 読んだ総量が stat と違う */
#define HR_TYPE         "type_conflict"     /* 通常ファイルでない */
#define HR_TYPE_UNKNOWN "type_unknown"      /* 種別が取れない */
#define HR_IO           "io_error"          /* stat / read / write の失敗 */
#define HR_TOO_LARGE    "size_unsupported"  /* 32bit / int で扱えない長さ */
#define HR_DEFAULT_SYS  "default_sys_exclusion"
#define HR_SETTINGS_DB  "settings_db"
#define HR_TOO_DEEP     "path_too_deep"     /* VFS の要素数上限を越える */
#define HR_BAD_NAME     "bad_name"          /* 名前に '\' が混じっている */
#define HR_PATH_REJECT  "path_rejected"     /* 正規化できず判定もできない */

/* VFS が 1 パスで扱える要素数。**fs/vfs.h の VFS_MAX_PATH_DEPTH が正典**で、
 * 外部プログラムからはそのヘッダを引けないので写しを置く。ずれの検出は
 * tools/tests/test_hsync_h1.py が両方を読んで突き合わせる ([C4])。
 *
 * fs/vfs.c の正規化は上限を越えた要素を**黙って捨てる** (エラーを返さない)。
 * `/host` を前置すると同期元は指定より 1 要素深くなるので、32 要素ちょうどの
 * dir を渡すと 33 要素になり、VFS が末尾を落として**指定した親ディレクトリ**を
 * 列挙してしまう。宛先側も同じ理由で別のパスに化ける。何も同期していないのに
 * `errors=0` / 終了コード 0 になる経路だったので、越えたら明示エラーにする
 * (Codex 実装レビュー B2)。 */
#define HS_MAX_PATH_DEPTH 32

static KernelAPI *api;
static u8 *file_buf;                 /* 64KB。コピーと読戻しで使う */
static u8 *cmp_a;                    /* file_buf[0 .. 32KB) */
static u8 *cmp_b;                    /* file_buf[32KB .. 64KB) */

/* 統計 (設計書 §7.2 の 5 本) */
static int g_copied;                 /* dry-run では「予定件数」 */
static int g_unchanged;
static int g_excluded;
static int g_protected;
static int g_errors;

/* オプション */
static int g_force;
static int g_dry_run;
static int g_verbose;

/* 既定の /sys 除外は「全体同期のルート直下」だけに効かせる。
 * `hsync usr` の usr/sys を巻き添えにしない (設計書 §3.2)。 */
static int g_root_sync;

/* 更新した先に応じた再起動の案内 (設計書 §7.2)。自動では再起動しない */
static int g_touched_sys;
static int g_touched_boot;

/* ======== 文字列ユーティリティ ======== */

static int str_len(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

/* 連結・コピーは**容量付きのものだけ**を置く。無検査の str_cpy / str_cat は
 * 引数から組み立てた長いパスでバッファを越えるので撤去した (往復 2 の 5)。
 *
 * 容量付き連結。cap は NUL 込みのバッファ長。
 * 戻り値 1 = 入った / 0 = 溢れた (dst は変えない)。
 * `hsync ./././...etc` のように引数から組み立てた文字列が OS32_MAX_PATH を
 * 越えると、正規化や保護判定にたどり着く前にスタックを壊していた
 * (往復 2 の 5)。連結は必ずこちらを通す。 */
static int str_ncat(char *dst, const char *src, int cap)
{
    int len = str_len(dst);
    int add = str_len(src);
    int i;

    if (len + add + 1 > cap) return 0;
    for (i = 0; i < add; i++) dst[len + i] = src[i];
    dst[len + add] = '\0';
    return 1;
}

/* 容量付きコピー。戻り値 1 = 入った / 0 = 溢れた */
static int str_ncpy(char *dst, const char *src, int cap)
{
    dst[0] = '\0';
    return str_ncat(dst, src, cap);
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int str_has_prefix(const char *s, const char *pre)
{
    int i = 0;
    while (pre[i]) {
        if (s[i] != pre[i]) return 0;
        i++;
    }
    return 1;
}

/* ======== ファイルリスト ======== */

/* 名前の保持幅。FileList はスタックに載る (MAX_FILES x NAME_CAP x 深さ) ので
 * 無闇に広げられない。収まらない名前は**切り詰めずにエラー**にする (下記)。 */
#define NAME_CAP 64

typedef struct {
    char names[MAX_FILES][NAME_CAP];
    u8   types[MAX_FILES];
    int  count;
    int  dropped;      /* MAX_FILES を越えて捨てたエントリ数 */
    int  truncated;    /* NAME_CAP に収まらず取り込めなかったエントリ数 */
    int  bad_name;     /* '\' を含むので取り込まなかったエントリ数 (B1) */
} FileList;

/* 列挙時のサイズは**持ち回さない**。同一判定に使うサイズは比較の直前に
 * stat で取り直す (設計書 §3.1)。ここでは名前と種別だけ私有バッファへ写す
 * (コールバックの中では FS に触らない、POLICY_DEBUG §4-26)。 */
static void ls_cb(const DirEntry_Ext *entry, void *ctx)
{
    FileList *fl = (FileList *)ctx;
    int i;
    if (fl->count >= MAX_FILES) { fl->dropped++; return; }

    if (!hsp_name_fits(entry->name, NAME_CAP)) {
        /* 切り詰めた名前でコピーすると**別のファイル**を作って成功と出る
         * (往復 3 の D6)。取り込まずに数えて、呼び手がエラーにする。 */
        fl->truncated++;
        return;
    }
    if (hsp_has_backslash(entry->name)) {
        /* ホスト側のファイル名に '\' は入らないはずだが、そこを信用しない。
         * '\' は OS32 では普通の 1 文字なのに HostDrv の先では区切りに化け、
         * `..\other` のような名前が同期元の外を指す (B1)。組み立てる前に断る。 */
        fl->bad_name++;
        return;
    }
    i = 0;
    while (entry->name[i]) {
        fl->names[fl->count][i] = entry->name[i];
        i++;
    }
    fl->names[fl->count][i] = '\0';
    fl->types[fl->count] = entry->type;
    fl->count++;
}

/* ======== 低レベル I/O (短い read / write を詰める) ======== */

/* want バイト読めるまで sys_read を繰り返す。
 * 戻り値 >= 0 … 実際に読めたバイト数 (want 未満は EOF)
 *        <  0 … I/O エラー
 * 両側の short read の**分割が違っても**、これを通せば比較のオフセットと
 * 有効長が揃う (設計書 §3.1)。 */
static int read_fill(int fd, u8 *buf, int want)
{
    int got = 0;

    while (got < want) {
        int rd = api->sys_read(fd, buf + got, (u32)(want - got));
        if (rd < 0) return rd;              /* I/O エラー */
        if (rd == 0) break;                 /* EOF */
        if (rd > want - got) return -1;     /* 契約違反 (溢れている) */
        got += rd;
    }
    return got;
}

/* len バイト書けるまで sys_write を繰り返す。
 * 0 進捗・負値・要求超過は失敗 (設計書 §6 の規則を H1 でも守る)。
 * 戻り値 len = 成功 / -1 = 失敗。 */
static int write_all(int fd, const u8 *buf, int len)
{
    int done = 0;

    while (done < len) {
        int wr = api->sys_write(fd, buf + done, (u32)(len - done));
        if (wr <= 0) return -1;             /* 0 進捗も失敗にする */
        if (wr > len - done) return -1;     /* 契約違反 */
        done += wr;
    }
    return done;
}

static int buf_equal(const u8 *a, const u8 *b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ======== 同一判定: サイズ + 内容のバイト比較 ======== */

/* 戻り値 0 = 全内容一致 / 1 = 不一致 / -1 = I/O 失敗・予定サイズに届かない EOF
 *
 * **エラーを「同一」扱いにしない** (設計書 §3.1)。読めなかった部分を
 * 「同じだった」と見なすと、まさに今回の「更新されない」が戻る。
 * CRC ではなくバイト比較なのは、どちらにせよ両方を読む必要があるなら
 * 衝突が無く最初の相違で打ち切れるから (設計書 §1)。 */
static int compare_files(const char *pa, const char *pb, u32 size)
{
    int fa, fb;
    int result = 0;
    u32 remaining = size;

    fa = api->sys_open(pa, KAPI_O_RDONLY);
    if (fa < 0) return -1;
    fb = api->sys_open(pb, KAPI_O_RDONLY);
    if (fb < 0) { api->sys_close(fa); return -1; }

    while (remaining > 0) {
        int want = (remaining > (u32)CMP_BUF_SIZE)
                       ? CMP_BUF_SIZE : (int)remaining;
        int ga, gb;

        ga = read_fill(fa, cmp_a, want);
        if (ga < 0) { result = -1; break; }
        gb = read_fill(fb, cmp_b, want);
        if (gb < 0) { result = -1; break; }
        if (ga != want || gb != want) { result = -1; break; }  /* 早期 EOF */
        if (!buf_equal(cmp_a, cmp_b, want)) { result = 1; break; }
        remaining -= (u32)want;
    }

    if (result == 0) {
        /* 予定より長い = stat のあとで変わった。「同一」とは言えないので
         * コピー側へ回す (握りつぶして unchanged にしない)。 */
        int ea = read_fill(fa, cmp_a, 1);
        int eb = read_fill(fb, cmp_b, 1);
        if (ea < 0 || eb < 0) result = -1;
        else if (ea != 0 || eb != 0) result = 1;
    }

    api->sys_close(fa);
    api->sys_close(fb);
    return result;
}

/* ======== コピー + 読戻し検証 ======== */

/* コピー元を読みながら CRC-32 と総バイト数を作り、書き終えたら宛先を
 * **再オープンして読み**、バイト数と CRC の**両方**を照合する (設計書 §4.2)。
 * I/O 失敗・予定長不一致・CRC 不一致は成功件数に入れない。
 *
 * CRC は偶発的破損の検出用。衝突があるので同一内容の厳密な証明には使わない
 * (コピー前の同一判定はバイト比較のまま)。
 *
 * ★ H1 の限界: 宛先を O_TRUNC で直接開く。ここで落ちたとき**旧宛先を
 *    復元する保証は無い**。一時ファイル + 検証 + 置換は票 H2。
 *
 * 戻り値 0 = 成功 / -1 = 失敗 (*reason に固定文字列)。 */
static int copy_verify(const char *src, const char *dst, u32 expect,
                       const char **reason)
{
    int fs, fd;
    u32 crc = CRC32_INIT;
    u32 crc2 = CRC32_INIT;
    u32 total = 0;
    u32 total2 = 0;
    int failed = 0;

    *reason = HR_IO;

    fs = api->sys_open(src, KAPI_O_RDONLY);
    if (fs < 0) return -1;
    fd = api->sys_open(dst, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd < 0) { api->sys_close(fs); return -1; }

    while (1) {
        int got = read_fill(fs, file_buf, FILE_BUF_SIZE);
        if (got < 0) { failed = 1; break; }
        if (got == 0) break;                         /* EOF */
        if (write_all(fd, file_buf, got) != got) { failed = 1; break; }
        crc = crc32_core_update(crc, file_buf, (u32)got);
        total += (u32)got;
        if (got < FILE_BUF_SIZE) break;              /* read_fill は EOF でのみ短い */
    }

    api->sys_close(fs);
    api->sys_close(fd);
    if (failed) return -1;

    if (total != expect) { *reason = HR_SOURCE; return -1; }

    /* 書いたものをディスクへ出す。ここが落ちたら「届いていない」 */
    if (api->vfs_sync() != 0) { *reason = HR_VERIFY; return -1; }

    /* 読戻し。キャッシュを経由するので媒体からの物理再読の保証ではない
     * (最終受入は再起動後の内容で見る、設計書 §4.2)。 */
    fd = api->sys_open(dst, KAPI_O_RDONLY);
    if (fd < 0) { *reason = HR_VERIFY; return -1; }
    while (1) {
        int got = read_fill(fd, file_buf, FILE_BUF_SIZE);
        if (got < 0) { failed = 1; break; }
        if (got == 0) break;
        crc2 = crc32_core_update(crc2, file_buf, (u32)got);
        total2 += (u32)got;
        if (total2 > total) break;                   /* 余分 = 不一致 */
        if (got < FILE_BUF_SIZE) break;
    }
    api->sys_close(fd);
    if (failed) { *reason = HR_VERIFY; return -1; }

    /* 最後の XOR は 1 ストリームにつき 1 回だけ (チャンクごとに畳まない) */
    if (total2 != total ||
        crc32_core_final(crc2) != crc32_core_final(crc)) {
        *reason = HR_VERIFY;
        return -1;
    }
    return 0;
}

/* ======== 保護対象の判定 ======== */

/* 現に存在する /etc/settings.db* の実体。同期を始める前に 1 度だけ集める
 * (ファイルごとに何度も stat すると 16MHz の実機では効く)。
 *
 * 表の小文字名を決め打ちで stat するだけでは足りない: ext2 は大文字小文字を
 * 区別するので `/etc/SETTINGS.DB` が本体でも拾えず、そこへの hardlink を
 * `hsync -f bin` が上書きしてしまう (往復 1 の B5)。/etc を sys_ls で列挙し、
 * 大文字小文字を無視して一致する**実在名**を全部 stat する。
 * コールバックの中では FS に触らない (private バッファに写すだけ、§4-26)。
 *
 * 上限は**表の名前数 + 大文字小文字違いの別名の余裕**。越えたら守れないので
 * 同期を拒否する (往復 2 の 3) = 上限が表より詰まっていると、リカバリ途中の
 * /etc (票 TASK_S3 §1b の 9 名 + wal/shm = 11 名) で通常同期が止まる。
 * 表が 5 名 → 11 名になったので (S3-D)、余裕を同じだけ保つよう 16 → 24。
 * `hsp_protected_names` に名前を足すときはここも見直すこと。 */
#define HS_MAX_PROT 24
static u32 g_prot_dev[HS_MAX_PROT];
static u32 g_prot_ino[HS_MAX_PROT];
static int g_prot_count;
static char g_prot_name[HS_MAX_PROT][64];
static int g_prot_name_count;
/* 一覧が HS_MAX_PROT を越えた。守れないので同期を拒否する (往復 2 の 3) */
static int g_prot_overflow;
/* 判定できない stat 失敗を踏んだ。同期を中止する印 */
static int g_abort;

static void prot_scan_cb(const DirEntry_Ext *entry, void *ctx)
{
    int i;

    (void)ctx;
    if (!hsp_is_protected_basename(entry->name)) return;
    if (g_prot_name_count >= HS_MAX_PROT) {
        /* 黙って捨てると 17 件目の実体を hardlink 経由で上書きできてしまう */
        g_prot_overflow++;
        return;
    }
    i = 0;
    while (entry->name[i] && i < 63) {
        g_prot_name[g_prot_name_count][i] = entry->name[i];
        i++;
    }
    g_prot_name[g_prot_name_count][i] = '\0';
    g_prot_name_count++;
}

/* 0 = ok / -1 = 判定できないので同期を中止 */
static int scan_protected_entities(void)
{
    OS32_Stat st;
    char buf[OS32_MAX_PATH];
    int i;
    int rc;

    g_prot_count = 0;
    g_prot_name_count = 0;
    g_prot_overflow = 0;

    rc = api->sys_ls("/etc", prot_scan_cb, 0);
    if (rc != 0 && rc != OS32_ERR_NOTFOUND) {
        api->kprintf(ATTR_RED, "Error: /etc を読めない (%d)。中止する\n", rc);
        return -1;
    }
    if (g_prot_overflow) {
        api->kprintf(ATTR_RED,
                     "Error: /etc の保護対象が %d 件を越えた (+%d)。中止する\n",
                     HS_MAX_PROT, g_prot_overflow);
        return -1;
    }

    for (i = 0; i < g_prot_name_count; i++) {
        if (!str_ncpy(buf, "/etc/", (int)sizeof(buf)) ||
            !str_ncat(buf, g_prot_name[i], (int)sizeof(buf))) {
            api->kprintf(ATTR_RED, "Error: path too long (/etc/%s)。中止する\n",
                         g_prot_name[i]);
            return -1;
        }
        rc = api->sys_stat(buf, &st);
        if (rc == 0) {
            g_prot_dev[g_prot_count] = st.st_dev;
            g_prot_ino[g_prot_count] = st.st_ino;
            g_prot_count++;
        } else if (rc != OS32_ERR_NOTFOUND) {
            /* 読めない = 守れない。書いてから気づくより中止する。 */
            api->kprintf(ATTR_RED, "Error: stat %s 失敗 (%d)。中止する\n",
                         buf, rc);
            return -1;
        }
    }
    return 0;
}

/* 実体規則: dst_path が現に /etc/settings.db* のどれかと同じ実体 (NHD 上の
 * hardlink) なら真。名前規則 (hsp_path_protected) をすり抜ける別名を塞ぐ。
 * 宛先の stat が「不存在」以外で失べば g_abort を立てる (往復 1 の B5) —
 * 読めないまま open すると O_TRUNC で切り詰めてしまう。 */
static int is_same_as_protected(const char *dst_path)
{
    OS32_Stat here;
    int i;
    int rc;

    if (g_prot_count == 0) return 0;                  /* 守る実体が無い */
    rc = api->sys_stat(dst_path, &here);
    if (rc != 0) {
        if (rc != OS32_ERR_NOTFOUND) {
            api->kprintf(ATTR_RED, "Error: stat %s 失敗 (%d)。中止する\n",
                         dst_path, rc);
            g_abort = 1;
        }
        return 0;
    }
    for (i = 0; i < g_prot_count; i++) {
        if (here.st_dev == g_prot_dev[i] && here.st_ino == g_prot_ino[i])
            return 1;
    }
    return 0;
}

/* コピー / mkdir の**直前**に通す 1 か所の判定。
 *    1 = 保護対象 (書かない。失敗ではない)
 *    0 = 対象外 (進んでよい)
 *   -1 = 判定できない (正規化に失敗。書かないが**エラーとして数える**)
 *
 * 判定できないものを「保護」に畳むと、`PROTECTED` が「守った」と「読めなかった」の
 * 両方を指すことになり、何も同期していないのに errors=0 で終わる
 * (Codex 実装レビュー B2)。語を分けるためにここで 3 値にする。 */
static int dst_protected(const char *dst_path)
{
    int cls = hsp_path_classify(dst_path);

    if (cls != 0) return cls;                  /* 1 = 保護 / -1 = 判定不能 */
    return is_same_as_protected(dst_path);
}

/* ======== 1 ファイルの判定と処理 (設計書 §3.1 の判定順) ======== */

static void note_target(const char *dst)
{
    if (str_has_prefix(dst, "/sys/"))  g_touched_sys = 1;
    if (str_has_prefix(dst, "/boot/")) g_touched_boot = 1;
}

static void fail_file(const char *dst, const char *reason, int err)
{
    api->kprintf(ATTR_RED, "  FAIL %s reason=%s err=%d\n", dst, reason, err);
    g_errors++;
}

static void sync_file(const char *src_path, const char *dst_path)
{
    OS32_Stat ss;
    OS32_Stat ds;
    const char *reason = HR_NEW;
    const char *vreason = HR_IO;
    u32 size;
    int rc;
    int need = 1;

    /* コピー元: 列挙結果を信用せず**直前に取り直す** (設計書 §3.1)。
     * 列挙とコピーの間にホスト側が差し替えているかもしれない。 */
    rc = api->sys_stat(src_path, &ss);
    if (rc != 0) { fail_file(dst_path, HR_IO, rc); return; }
    if ((ss.st_mode & OS_S_IFMT) == 0) {
        /* 種別が取れない。特殊ファイルを通常ファイルとして読まない */
        fail_file(dst_path, HR_TYPE_UNKNOWN, 0);
        return;
    }
    if ((ss.st_mode & OS_S_IFMT) != OS_S_IFREG) {
        fail_file(dst_path, HR_TYPE, 0);
        return;
    }
    if (ss.st_size > HS_MAX_FILE_SIZE) {
        fail_file(dst_path, HR_TOO_LARGE, 0);
        return;
    }
    size = ss.st_size;

    /* 宛先 */
    rc = api->sys_stat(dst_path, &ds);
    if (rc == OS32_ERR_NOTFOUND) {
        reason = HR_NEW;
    } else if (rc != 0) {
        fail_file(dst_path, HR_IO, rc);
        return;
    } else if ((ds.st_mode & OS_S_IFMT) != OS_S_IFREG) {
        /* ディレクトリ等。勝手に削除・切り詰めしない */
        fail_file(dst_path, HR_TYPE, 0);
        return;
    } else if (g_force) {
        /* 比較だけ省く。保護・型検査・コピー後検証は省略しない */
        reason = HR_FORCED;
    } else if (ds.st_size != size) {
        reason = HR_SIZE;
    } else {
        int cmp = compare_files(src_path, dst_path, size);
        if (cmp < 0) { fail_file(dst_path, HR_IO, 0); return; }
        if (cmp > 0) {
            reason = HR_CONTENT;
        } else {
            need = 0;
        }
    }

    if (!need) {
        g_unchanged++;
        if (g_verbose)
            api->kprintf(ATTR_WHITE, "  SAME %s size=%d\n",
                         dst_path, (int)size);
        return;
    }

    if (g_dry_run) {
        /* 読んで比べるだけ。mkdir・一時ファイル・明示 sync はしない */
        g_copied++;
        api->kprintf(ATTR_CYAN, "  PLAN %s reason=%s size=%d\n",
                     dst_path, reason, (int)size);
        return;
    }

    if (copy_verify(src_path, dst_path, size, &vreason) != 0) {
        api->kprintf(ATTR_RED,
                     "  FAIL %s reason=%s (直接上書きなので旧内容は残らない)\n",
                     dst_path, vreason);
        g_errors++;
        return;
    }

    api->kprintf(ATTR_GREEN, "  UPDATE %s reason=%s size=%d\n",
                 dst_path, reason, (int)size);
    g_copied++;
    note_target(dst_path);
}

/* 宛先をディレクトリとして使えるかを確かめる (Codex 実装レビュー B3)。
 *   0 = ディレクトリ、または不存在 (進んでよい)
 *  -1 = 通常ファイル等の型衝突 / stat 不能 (errors に数え済み)
 *
 * fs/ext2_dir.c の ext2_find_entry は**種別を問わず**名前があれば EXIST を
 * 返すので、sys_mkdir の OS32_ERR_EXIST だけでは「同じ名前の通常ファイル」を
 * 見分けられない。無条件に受理して再帰すると、`/host/usr/empty` が
 * ディレクトリ・`/usr/empty` が通常ファイルのまま errors=0 / 終了コード 0 で
 * 終わっていた。通常ファイル側には type_conflict を入れたのに、
 * ディレクトリ側に無かった。 */
static int dst_dir_type_ok(const char *dst_path)
{
    OS32_Stat ds;
    int rc = api->sys_stat(dst_path, &ds);

    if (rc == OS32_ERR_NOTFOUND) return 0;
    if (rc != 0) { fail_file(dst_path, HR_IO, rc); return -1; }
    if ((ds.st_mode & OS_S_IFMT) == OS_S_IFDIR) return 0;
    /* 勝手に消さない。型が食い違ったまま「完了」と言わない。 */
    fail_file(dst_path, HR_TYPE, 0);
    return -1;
}

/* 明示 dir の同期を**始める前**に、起点の型を 1 度だけ確かめる
 * (Codex 実装レビュー 往復 2 の B3 残件)。
 *
 * 列挙ループの中の型検査は**子項目**にしか掛からない。コピー元が空の
 * ディレクトリだと子が 1 つも無いので一度も呼ばれず、`/host/usr/empty` が
 * ディレクトリ・`/usr/empty` が通常ファイルのまま errors=0 / 終了コード 0 で
 * 終わっていた。-n でも -f でも同じ。語と集計は子項目側とそろえる。
 *
 * 戻り値 0 = 始めてよい / -1 = 型衝突等 (errors に数え済み)。 */
static int start_point_ok(const char *src_path, const char *dst_path)
{
    OS32_Stat ss;
    int rc;

    /* コピー元: dir として渡された以上ディレクトリであること。
     * sys_ls 任せにしない — HostDrv の hdrv_list_dir は DIRECTORY_FILE で
     * 開くので通常ファイルなら失敗するが、返るのは VFS_ERR_NOTFOUND で
     * 「存在しない」と区別が付かないし、通常ファイルに空の列挙を成功として
     * 返す FS が 1 つでもあれば同じ穴がそのまま開く。 */
    rc = api->sys_stat(src_path, &ss);
    if (rc != 0) { fail_file(src_path, HR_IO, rc); return -1; }
    if ((ss.st_mode & OS_S_IFMT) == 0) {
        fail_file(src_path, HR_TYPE_UNKNOWN, 0);
        return -1;
    }
    if ((ss.st_mode & OS_S_IFMT) != OS_S_IFDIR) {
        fail_file(src_path, HR_TYPE, 0);
        return -1;
    }

    /* 宛先: 在るならディレクトリであること (子項目と同じ検査) */
    return dst_dir_type_ok(dst_path);
}

/* ======== ディレクトリ再帰同期 ======== */

static void sync_directory(const char *src_dir, const char *dst_dir, int depth)
{
    FileList fl;
    int i;
    int rc;

    if (depth > MAX_DEPTH) {
        /* 打ち切りを黙って成功にしない (未同期のまま「完了」と出ていた) */
        api->kprintf(ATTR_RED, "  FAIL: depth > %d: %s\n", MAX_DEPTH, src_dir);
        g_errors++;
        return;
    }

    fl.count = 0;
    fl.dropped = 0;
    fl.truncated = 0;
    fl.bad_name = 0;
    rc = api->sys_ls(src_dir, ls_cb, &fl);
    if (rc != 0) {
        api->kprintf(ATTR_RED, "  FAIL: ls %s (err=%d)\n", src_dir, rc);
        g_errors++;
        return;
    }
    if (fl.dropped) {
        api->kprintf(ATTR_RED, "  FAIL: %s のエントリが %d 件を越えた (+%d)\n",
                     src_dir, MAX_FILES, fl.dropped);
        g_errors++;
    }
    if (fl.truncated) {
        api->kprintf(ATTR_RED,
                     "  FAIL: %s に %d 文字を越える名前が %d 件 (コピーしない)\n",
                     src_dir, NAME_CAP - 1, fl.truncated);
        g_errors++;
    }
    if (fl.bad_name) {
        /* '\' を含む名前。OS32 側の '..' 検査を素通りしてホスト側で
         * 同期元の外を指し得る (B1)。組み立てずに数えてエラーにする。 */
        api->kprintf(ATTR_RED,
                     "  FAIL: %s に '\\' を含む名前が %d 件 reason=%s\n",
                     src_dir, fl.bad_name, HR_BAD_NAME);
        g_errors++;
    }

    for (i = 0; i < fl.count; i++) {
        char src_path[OS32_MAX_PATH];
        char dst_path[OS32_MAX_PATH];

        /* 判定できない stat 失敗を踏んだら、それ以上は書かない */
        if (g_abort) return;

        /* "." と ".." をスキップ */
        if (fl.names[i][0] == '.') {
            if (fl.names[i][1] == '\0') continue;
            if (fl.names[i][1] == '.' && fl.names[i][2] == '\0') continue;
        }

        /* パス構築。dst_dir は全体同期のとき "" なので、空文字列で
         * [-1] を読まないように長さを先に見る。連結は必ず容量付きで行い、
         * 溢れたら**判定より前に**エラーにする (往復 2 の 5)。
         * ここは純粋な文字列操作で、FS には触らない。 */
        if (!str_ncpy(src_path, src_dir, (int)sizeof(src_path)) ||
            ((str_len(src_path) == 0 ||
              src_path[str_len(src_path) - 1] != '/') &&
             !str_ncat(src_path, "/", (int)sizeof(src_path))) ||
            !str_ncat(src_path, fl.names[i], (int)sizeof(src_path))) {
            api->kprintf(ATTR_RED, "  FAIL: path too long: %s/%s\n",
                         src_dir, fl.names[i]);
            g_errors++;
            continue;
        }

        if (!str_ncpy(dst_path, dst_dir, (int)sizeof(dst_path)) ||
            ((str_len(dst_path) == 0 ||
              dst_path[str_len(dst_path) - 1] != '/') &&
             !str_ncat(dst_path, "/", (int)sizeof(dst_path))) ||
            !str_ncat(dst_path, fl.names[i], (int)sizeof(dst_path))) {
            api->kprintf(ATTR_RED, "  FAIL: path too long: %s/%s\n",
                         dst_dir, fl.names[i]);
            g_errors++;
            continue;
        }

        /* ---- 要素数の検査 (B2) ----
         * fs/vfs.c は上限を越えた要素を黙って捨てるので、越えたパスは
         * 「1 つ上のディレクトリ」を指す**別のパス**として成立してしまう。
         * 内容を開く前に断ち切る。 */
        if (hsp_depth(src_path) > HS_MAX_PATH_DEPTH ||
            hsp_depth(dst_path) > HS_MAX_PATH_DEPTH) {
            api->kprintf(ATTR_RED,
                         "  FAIL %s reason=%s (VFS の上限 %d 要素)\n",
                         dst_path, HR_TOO_DEEP, HS_MAX_PATH_DEPTH);
            g_errors++;
            continue;
        }

        /* ---- 範囲判定 (内容を開く処理や mkdir より**先**、設計書 §3.1) ----
         * ルート直下の sys は既定で飛ばす (稼働中のシェル・共有ライブラリ)。
         * **全体同期のときだけ**。`hsync usr` の usr/sys は対象に含める。
         * 入れ替えたいときは `hsync sys` と明示する。-f でも解除しない。 */
        if (depth == 0 && g_root_sync && str_cmp(fl.names[i], "sys") == 0) {
            g_excluded++;
            if (g_verbose)
                api->kprintf(ATTR_YELLOW, "  EXCLUDE %s reason=%s\n",
                             dst_path, HR_DEFAULT_SYS);
            continue;
        }

        /* ---- 保護判定 (同じく内容を開く前) ----
         * /etc/settings.db* は通常配備で作らない・上書きしない (票 S0-D)。
         * ディレクトリ経路も同じ規則で見る (etc/settings.db/ の残骸を作らない)。 */
        {
            int prot = dst_protected(dst_path);
            if (prot < 0) {
                /* 判定できないものを PROTECTED と呼ばない (B2)。
                 * 書かないのは同じだが errors に数えて非ゼロ終了させる。 */
                api->kprintf(ATTR_RED, "  FAIL %s reason=%s\n",
                             dst_path, HR_PATH_REJECT);
                g_errors++;
                continue;
            }
            if (prot > 0) {
                api->kprintf(ATTR_YELLOW, "  PROTECTED %s reason=%s\n",
                             dst_path, HR_SETTINGS_DB);
                g_protected++;
                continue;
            }
        }
        if (g_abort) return;

        if (fl.types[i] == OS32_FILE_TYPE_DIR) {
            /* ディレクトリ: 作成して再帰。mkdir の失敗を無視すると
             * 中身のコピーが全部落ちて「完了」と出る (往復 2 の 4)。
             * dry-run では mkdir しない — 無い宛先の下は「全部新規」として
             * 読み比べだけ続ける。ただし**型検査だけは dry-run でも行う**
             * (書き込みはしない、B3)。 */
            if (g_dry_run) {
                if (dst_dir_type_ok(dst_path) != 0) continue;
            } else {
                int mrc = api->sys_mkdir(dst_path);
                if (mrc == OS32_ERR_EXIST) {
                    /* EXIST は「同名の何か」がある印でしかない。
                     * ディレクトリであることを確かめてから入る (B3)。 */
                    if (dst_dir_type_ok(dst_path) != 0) continue;
                } else if (mrc != 0) {
                    api->kprintf(ATTR_RED, "  FAIL: mkdir %s (err=%d)\n",
                                 dst_path, mrc);
                    g_errors++;
                    continue;
                }
            }
            sync_directory(src_path, dst_path, depth + 1);
        } else {
            sync_file(src_path, dst_path);
        }
    }
}

/* ======== メイン ======== */

static void usage(void)
{
    api->kprintf(ATTR_WHITE, "hsync — HostDrv sync (/host -> /)\n");
    api->kprintf(ATTR_WHITE, "Usage: hsync [-f] [-n] [-v] [dir]\n");
    api->kprintf(ATTR_WHITE, "  -f, --force     同一判定を省いて上書き (保護・検証は省かない)\n");
    api->kprintf(ATTR_WHITE, "  -n, --dry-run   読んで比べるだけ。1 バイトも書かない\n");
    api->kprintf(ATTR_WHITE, "  -v, --verbose   スキップ理由と比較結果も出す\n");
    api->kprintf(ATTR_WHITE, "  -h, --help      この表示\n");
    api->kprintf(ATTR_WHITE, "  dir             同期対象は 1 つだけ (例: bin, sys, usr/bin)\n");
    api->kprintf(ATTR_WHITE, "  既定の同一判定は「サイズ + 内容のバイト比較」。mtime は見ない\n");
    api->kprintf(ATTR_WHITE, "  全体同期ではルート直下の sys を除外する (-f でも解除しない)\n");
}

int __cdecl main(int argc, char **argv, KernelAPI *_api)
{
    const char *subdir;
    char norm[HSP_MAX_PATH];
    char src[OS32_MAX_PATH];
    char dst[OS32_MAX_PATH];
    int i;
    int rc;

    api = _api;
    subdir = NULL;
    g_copied = 0;
    g_unchanged = 0;
    g_excluded = 0;
    g_protected = 0;
    g_errors = 0;
    g_force = 0;
    g_dry_run = 0;
    g_verbose = 0;
    g_root_sync = 1;
    g_touched_sys = 0;
    g_touched_boot = 0;
    g_abort = 0;
    file_buf = 0;

    /* 引数パース。未知オプションと複数 dir は**エラー**にする。
     * 以前は「最後の引数で上書き」だったので `hsync bin sys` が黙って
     * sys だけを同期し、`hsync --dry-run` が dir 名として通っていた。 */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] == '-') {
            if (str_cmp(a, "-f") == 0 || str_cmp(a, "--force") == 0) {
                g_force = 1;
            } else if (str_cmp(a, "-n") == 0 ||
                       str_cmp(a, "--dry-run") == 0) {
                g_dry_run = 1;
            } else if (str_cmp(a, "-v") == 0 ||
                       str_cmp(a, "--verbose") == 0) {
                g_verbose = 1;
            } else if (str_cmp(a, "-h") == 0 || str_cmp(a, "--help") == 0) {
                usage();
                return 0;
            } else {
                api->kprintf(ATTR_RED, "Error: unknown option: %s\n", a);
                usage();
                return 1;
            }
        } else {
            if (subdir) {
                api->kprintf(ATTR_RED,
                             "Error: dir は 1 つだけ (%s と %s)\n", subdir, a);
                return 1;
            }
            subdir = a;
        }
    }

    /* 対象パスを**正規化してから** /host 配下と宛先を決める。
     * '..' による同期元脱出、切り詰め、自己コピーをここで断る。 */
    if (subdir) {
        if (hsp_has_backslash(subdir)) {
            /* '\' は OS32 の区切りではないので `..\other` が 1 要素として
             * '..' 検査を素通りするが、HostDrv の先では区切りに化けて
             * 同期元の外を指す (B1)。専用の文言で断る。 */
            api->kprintf(ATTR_RED,
                         "Error: dir に '\\' は使えない (reason=%s): %s\n",
                         HR_BAD_NAME, subdir);
            return 1;
        }
        if (!hsp_normalize(subdir, norm, (int)sizeof(norm))) {
            api->kprintf(ATTR_RED,
                         "Error: dir が不正 (長すぎる / root の外へ出る): %s\n",
                         subdir);
            return 1;
        }
        if (str_cmp(norm, "/") == 0) {
            subdir = NULL;                     /* `hsync .` は全体同期と同じ */
        } else if (str_cmp(norm, "/host") == 0 ||
                   str_has_prefix(norm, "/host/")) {
            /* 同期元そのもの = 同一実体への自己コピー */
            api->kprintf(ATTR_RED,
                         "Error: %s は同期元 (/host) 自身。自己コピーは行わない\n",
                         norm);
            return 1;
        }
    }
    g_root_sync = (subdir == NULL);

    /* バッファ確保。64KB を比較用 32KB x 2 に割って使う (設計書 §4.3) */
    file_buf = (u8 *)api->mem_alloc(FILE_BUF_SIZE);
    if (!file_buf) {
        api->kprintf(ATTR_RED, "Error: out of memory\n");
        return 1;
    }
    cmp_a = file_buf;
    cmp_b = file_buf + CMP_BUF_SIZE;

    /* /host がマウントされているか確認 */
    if (!api->sys_is_mounted("/host")) {
        api->kprintf(ATTR_RED, "Error: /host is not mounted\n");
        api->mem_free(file_buf);
        return 1;
    }

    /* 守るべき実体を 1 度だけ集める (票 S0-D の実体規則)。
     * 集められなければ守れないので同期そのものを行わない。 */
    if (scan_protected_entities() != 0) {
        api->mem_free(file_buf);
        return 1;
    }

    /* 同期パス構築。`hsync -f etc` のように subdir で保護対象 (やその子) を
     * 直接指されても書かない。連結は容量付きで行い、溢れたら判定より前に
     * エラーにする (往復 2 の 2 / 5)。 */
    if (subdir) {
        if (!str_ncpy(src, "/host", (int)sizeof(src)) ||
            !str_ncat(src, norm, (int)sizeof(src)) ||
            !str_ncpy(dst, norm, (int)sizeof(dst))) {
            api->kprintf(ATTR_RED, "Error: path too long: %s\n", subdir);
            api->mem_free(file_buf);
            return 1;
        }
        /* **`/host` を前置したあとの**要素数で上限を見る (B2)。
         * hsp_normalize は入力側にしか上限を掛けないので、32 要素ちょうどの
         * dir はここまで通ってくる。宛先側も同じ理由で見る。 */
        if (hsp_depth(src) > HS_MAX_PATH_DEPTH ||
            hsp_depth(dst) > HS_MAX_PATH_DEPTH) {
            api->kprintf(ATTR_RED,
                         "Error: dir が深すぎる reason=%s (VFS の上限 %d 要素、"
                         "/host を足すと %d 要素): %s\n",
                         HR_TOO_DEEP, HS_MAX_PATH_DEPTH, hsp_depth(src), norm);
            api->mem_free(file_buf);
            return 1;
        }
        {
            int prot = dst_protected(dst);
            if (g_abort) {
                api->mem_free(file_buf);
                return 1;
            }
            if (prot < 0) {
                /* 判定できないものを PROTECTED と呼ばない (B2) */
                api->kprintf(ATTR_RED, "Error: %s reason=%s\n",
                             dst, HR_PATH_REJECT);
                api->mem_free(file_buf);
                return 1;
            }
            if (prot > 0) {
                api->kprintf(ATTR_YELLOW, "  PROTECTED %s reason=%s\n",
                             dst, HR_SETTINGS_DB);
                api->mem_free(file_buf);
                return 0;             /* 除外は失敗ではない */
            }
        }
        api->kprintf(ATTR_CYAN, "hsync: %s -> %s\n", src, dst);
    } else {
        if (!str_ncpy(src, "/host", (int)sizeof(src)) ||
            !str_ncpy(dst, "", (int)sizeof(dst))) {
            api->kprintf(ATTR_RED, "Error: path too long\n");
            api->mem_free(file_buf);
            return 1;
        }
        api->kprintf(ATTR_CYAN, "hsync: /host -> /\n");
        /* 既定の除外を**先頭で明示する** (設計書 §7.2) */
        api->kprintf(ATTR_YELLOW,
                     "  note: ルート直下の sys は既定で除外 "
                     "(入れ替えるなら `hsync sys`)。-f でも解除しない\n");
    }

    if (g_force) {
        api->kprintf(ATTR_YELLOW,
                     "  (force mode: 同一判定のみ省略。保護と読戻し検証は行う)\n");
    }
    if (g_dry_run) {
        api->kprintf(ATTR_YELLOW,
                     "  (dry-run: 読み取りと比較だけ。mkdir・書き込み・sync はしない)\n");
    }

    /* 同期実行。明示 dir は**起点の型を先に確かめてから**始める (B3)。
     * 全体同期の起点 (/host -> /) はマウントの前提としてディレクトリ。
     * 始めなかった場合も下の集計行と終了コードはそのまま通る
     * (errors に入っているので FAILED: / 非ゼロになる)。 */
    if (!subdir || start_point_ok(src, dst) == 0) {
        sync_directory(src, dst, 0);
    }

    if (g_abort) {
        api->kprintf(ATTR_RED,
                     "\nAborted: 保護判定に必要な stat が失敗した\n");
        api->mem_free(file_buf);
        return 1;
    }

    /* ファイルシステム同期。落ちたら書いたものが届いていない。
     * dry-run は明示 sync をしない (設計書 §3.2)。 */
    if (!g_dry_run) {
        rc = api->vfs_sync();
        if (rc != 0) {
            api->kprintf(ATTR_RED, "  FAIL: vfs_sync (err=%d)\n", rc);
            g_errors++;
        }
    }

    /* 結果表示。失敗があれば頭を FAILED: にする (成功表示へ進めない) */
    api->kprintf(g_errors ? ATTR_RED : ATTR_WHITE,
                 "\n%s copied=%d unchanged=%d excluded=%d protected=%d errors=%d%s\n",
                 hsp_final_label(g_errors),
                 g_copied, g_unchanged, g_excluded, g_protected, g_errors,
                 g_dry_run ? " (dry-run: copied は予定件数)" : "");

    /* 「ディスクへ同期した」と「稼働中の版が入れ替わった」は別のこと
     * (設計書 §7.2)。再起動はここでは行わない。 */
    if (!g_dry_run && g_copied > 0) {
        api->kprintf(ATTR_YELLOW,
                     "NOTE: ディスク上を更新しただけ。稼働中の版は切り替わっていない\n");
        if (g_touched_sys)
            api->kprintf(ATTR_YELLOW,
                         "NOTE: /sys を更新した -> シェル再起動が必要 "
                         "(shlib は起動時ロード)\n");
        if (g_touched_boot)
            api->kprintf(ATTR_YELLOW,
                         "NOTE: /boot を更新した -> 再起動が必要 "
                         "(カーネルはブート時ロード)\n");
    }

    api->mem_free(file_buf);
    /* 失敗は終了コードに載せる (crt0_c が main の戻り値を sys_exit へ渡す) */
    return g_errors ? 1 : 0;
}
