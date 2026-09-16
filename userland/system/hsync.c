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
/*    hsync --verify      — 日時を見ず、全件の内容を必ず比較する            */
/*    hsync sys           — /sys を明示指定したときだけ同期する             */
/*    hsync -f sys        — /host/sys/ を強制同期                          */
/*                                                                          */
/*  **同一判定は「サイズ + 内容のバイト比較」** (票 H1、設計書              */
/*  docs/tasks/shell/HSYNC_IMPROVEMENT_PLAN.md §1 / §3.1)。                 */
/*  以前はサイズが同じなら中身を見ずにスキップしていたので、長さを変えずに   */
/*  ヘッダだけ変わった shlib (2026-09-14 の libos32gui.shlib) が更新されず、 */
/*  「配備したのに古いまま」が起きた。                                      */
/*                                                                          */
/*  **日時は「前置フィルタ」** (票 H3 §8、ユーザー決裁 2026-09-15)。        */
/*  H1 の全件バイト比較は変更 0 件でも `hsync sys` に 25.8 秒 / 全体同期に   */
/*  135.4 秒かかった (両側を読むので実 I/O は対象の 2 倍)。日時で候補を絞り、*/
/*  **候補だけをバイト比較**することで読む量を数十分の 1 にする。           */
/*                                                                          */
/*    | サイズか日時が違う   | **内容を比較**して、違えばコピー            | */
/*    | サイズも日時も同じ   | スキップ (unchanged)                       | */
/*    | 日時が不明 / 0       | **内容を比較する**                         | */
/*                                                                          */
/*  **証拠が無いことを同一の根拠にしない** — 日時が取れないときに黙って     */
/*  スキップしない。日時の一致だけでコピーも決めない (決めるのは内容比較)。  */
/*  失うのは「サイズが同じ、かつ日時も同じ、かつ中身が違う」場合だけで、     */
/*  そこが要るときは `--verify` で全件の内容を比較する (= H1 の挙動)。       */
/*                                                                          */
/*  **保存側 (sys_set_mtime、KAPI v52) が無いと成立しない**: 宛先の日時は    */
/*  「hsync がコピーした時刻」なので、コピー元の時刻を宛先へ書かない限り     */
/*  全ファイルが永遠に「変更あり」に見えて 1 件もスキップされない。          */
/*                                                                          */
/*  **既定で sys を外す理由**: /sys には稼働中の常駐シェル (shell.bin)、     */
/*  共有ライブラリ (lib/)、unicode.bin、フォントが入っている。走っている     */
/*  ものを背後から差し替えると、次の exec まで実体と食い違う。入れ替えたい   */
/*  ときは `hsync sys` と明示する (2026-09-09、ホットデプロイ撤去に伴い)。   */
/*  除外は `-f` でも解除しない。                                            */
/*                                                                          */
/*  **置き換えは一時ファイル経由** (票 H2、KAPI v53)。宛先と同じディレクトリ */
/*  の予約名 `.hs~<名前>` へ O_EXCL で作り、書き込み・sync・読戻し検証・      */
/*  mtime まで済ませてから sys_rename で本名に載せ替える。                    */
/*                                                                          */
/*    **公開の前** (書き込み・検証・mtime・置換の途中まで) の失敗            */
/*        … 旧宛先の名前と内容が残る。一時ファイルは片づけられれば片づける。 */
/*          メタデータの失敗でマウントが書き込み禁止 (ROFS) に落ちた後は      */
/*          unlink も通らないので `STALE` と表示し、次の実行が片づける。      */
/*    **公開の後** (宛先エントリの inode が新しい方を指した後) の失敗        */
/*        … 宛先には検証済みの新しい内容が現れる。後始末が落ちたら漏れが      */
/*          残り (e2fsck が回収)、`replace_partial` として報告する            */
/*          (成功には数えない)。                                             */
/*                                                                          */
/*  公開の有無は**宛先の `st_ino`** で判定する。サイズと CRC は証拠にしない   */
/*  (`-f` で新旧が同じ内容だと未公開を公開と誤る)。ジャーナルは無いので       */
/*  「原子的」とは書かない — 電源断では一時ファイルが残り得る。               */
/*                                                                          */
/*  `.hs~` は **hsync の予約接頭辞**。この名前のファイルは hsync が作り、     */
/*  訪れたディレクトリで消す。利用者はこの接頭辞を使わないこと。             */
/*                                                                          */
/*  **古いカーネル** (KAPI v53 未満) では一時ファイル方式が成立しない         */
/*  (O_EXCL が黙って無視され、ext2 の置き換えも旧順序)。既定は 1 件も書かずに */
/*  `kernel_too_old` で断る。`--unsafe-overwrite` を明示したときだけ、以前と  */
/*  同じ直接上書きで進む (**失敗すると旧内容は残らない**)。                   */
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
/* ---- 票 H3 (日時) ---- */
#define HR_MTIME_ONLY   "mtime_only"        /* 内容同じ・日時だけ違う */
#define HR_SAME_MTIME   "size_mtime_same"   /* サイズも日時も同じ = 読まずに省略 */
#define HR_SAME_CONTENT "content_same"      /* 読み比べて全内容一致 */
#define HR_META_FAILED  "metadata_failed"   /* 有効な時刻の保存に失敗した */
#define HR_MTIME_UNKNOWN "mtime_unknown"    /* 元の mtime が 0 / 不明 */
#define HR_MTIME_NOSYS  "mtime_unsupported" /* 宛先 FS が set_mtime を持たない */
#define HR_DEFAULT_SYS  "default_sys_exclusion"
#define HR_SETTINGS_DB  "settings_db"
#define HR_TOO_DEEP     "path_too_deep"     /* VFS の要素数上限を越える */
/* ---- 票 H2 (一時ファイル + 検証 + 置換) ---- */
#define HR_REPLACE_PARTIAL "replace_partial"  /* 公開済み・後始末が落ちた */
#define HR_REPLACE_FAILED  "replace_failed"   /* 未公開・旧内容のまま */
#define HR_REPLACE_UNKNOWN "replace_unknown"  /* 公開したか判定できない */
#define HR_TEMP_EXISTS     "temp_exists"      /* 予約名が在って消せない */
#define HR_NAME_TOO_LONG   "name_too_long"    /* 一時名が NAME_CAP に入らない */
#define HR_NO_SPACE        "no_space"         /* 空き不足 */
#define HR_HARDLINK        "hardlink"         /* 宛先の st_nlink > 1 */
#define HR_KERNEL_TOO_OLD  "kernel_too_old"   /* KAPI が v53 未満 */
#define HR_REPLACE_UNSUPPORTED "replace_unsupported" /* 宛先 FS に O_EXCL が無い */
#define HR_DEST_CHANGED    "dest_changed"     /* 判定後に宛先が変わった */
#define HR_SYNC_FAILED     "sync_failed"      /* 置換後の vfs_sync が落ちた */
#define HR_PROT_RESERVED   "protected"        /* 予約名だが保護対象の実体 */
#define HR_RESERVED_NAME   "reserved_name"    /* コピー元に予約名 .hs~ が在る */

/* hsync の**予約接頭辞** (票 H2 §2-4、決裁 D3 (a'))。この接頭辞で始まる名前は
 * hsync が作り、hsync が消す。所有の根拠は「作った印」ではなく**予約された
 * 名前空間**に置いてある — `st_nlink` や作成時刻では所有を証明できないので
 * (Codex 往復 1 所見 2)、man ページ (docs/manpages/hsync.1) と
 * docs/06_filesystem.md に「利用者は使わない」と明記したうえで消す。 */
#define HS_TEMP_PREFIX     ".hs~"
/* 長さは**接頭辞の文字列から導く** ([C4]: 同じ値を 2 か所に書かない)。
 * sizeof は終端の '\0' を含むので 1 を引く。 */
#define HS_TEMP_PREFIX_LEN ((int)(sizeof(HS_TEMP_PREFIX) - 1))

/* 一時ファイル方式が成立する最小の KAPI 版 (票 H2 §2-1: O_EXCL)。
 * **build/app.conf の要求版は 52 のまま**なので、v52 のカーネルでも hsync は
 * 起動でき、ここで自分から断れる (§2-3 末尾)。 */
#define HS_MIN_KAPI_H2  53
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

/* 統計 (設計書 §7.2 の 6 区分) */
static int g_copied;                 /* dry-run では「予定件数」 */
static int g_unchanged;
static int g_excluded;
static int g_protected;
static int g_metadata_updated;       /* 内容は同じで mtime だけ直したもの */
static int g_cleaned;                /* 片づけた予約名 (.hs~) の数 (票 H2 §2-4) */
static int g_errors;

/* 集計には出さないが、**省略したことを必ず見せる**ための数 (票 H3) */
static int g_mtime_unknown;          /* 元の mtime が 0 = 保存を省略した */
static int g_mtime_nosys;            /* 宛先 FS が set_mtime を持たない */

/* オプション */
static int g_force;
static int g_dry_run;
static int g_verbose;
static int g_verify;                 /* 全件の内容を必ず比較する (票 H3 §8) */
static int g_unsafe;                 /* --unsafe-overwrite が指定された */
static int g_direct;                 /* 実際に直接上書きで進む (旧カーネル) */
static int g_direct_overwrite;       /* 直接上書きで書いた件数 */

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

/* hsync の予約名か (票 H2 §2-4)。**`st_nlink` や作成時刻は見ない** —
 * 途中で止まった媒体では nlink が 2 になり得るし、所有の根拠は名前空間の
 * 予約だけに置いてある。 */
static int hs_is_temp_name(const char *name)
{
    return str_has_prefix(name, HS_TEMP_PREFIX);
}

/* ======== ファイルリスト ======== */

/* 名前の保持幅。FileList はスタックに載る (MAX_FILES x NAME_CAP x 深さ) ので
 * 無闇に広げられない。収まらない名前は**切り詰めずにエラー**にする (下記)。 */
#define NAME_CAP 64

typedef struct {
    const char *src_dir;  /* 列挙中のコピー元ディレクトリ (表示用。FS には触らない) */
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

    /* **予約名は同期対象にしない** (票 H2 §2-3 手順 1)。127 件の枠に入れる
     * より**前**に弾く — 途中で止まった実行が残した `.hs~` が 128 件の枠を
     * 食って通常ファイルを落とすのを防ぐ。掃除は別の枠で行う (§2-4)。
     *
     * **黙って落とさない**。ホスト側の配備元に誤って `.hs~x` が紛れると、
     * その 1 件は同期されないのに `excluded` にも `-v` の行にも出ず、
     * 気づく手がかりが無かった。除外として数えて -v で見せる。
     * 見せるのは**コピー元**の名前 — 直すのはそちらなので。
     * コールバックの中なので FS には触らず、パスも組み立てずに書式で繋ぐ
     * (POLICY_DEBUG §4-26)。 */
    if (hs_is_temp_name(entry->name)) {
        g_excluded++;
        if (g_verbose) {
            const char *dir = fl->src_dir ? fl->src_dir : "";
            int n = str_len(dir);
            api->kprintf(ATTR_YELLOW, "  EXCLUDE %s%s%s reason=%s\n",
                         dir, (n > 0 && dir[n - 1] == '/') ? "" : "/",
                         entry->name, HR_RESERVED_NAME);
        }
        return;
    }

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
 * 戻り値 len = 成功 / 負値 = 失敗。**失敗は sys_write が返した番号をそのまま
 * 返す** (票 H2 §2-3 手順 3: 空き不足を `no_space` と呼び分けるため。
 * 0 進捗と契約違反は番号が無いので OS32_ERR_IO にする)。 */
static int write_all(int fd, const u8 *buf, int len)
{
    int done = 0;

    while (done < len) {
        int wr = api->sys_write(fd, buf + done, (u32)(len - done));
        if (wr < 0) return wr;                  /* 番号を保つ */
        if (wr == 0) return OS32_ERR_IO;        /* 0 進捗も失敗にする */
        if (wr > len - done) return OS32_ERR_IO;/* 契約違反 */
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

/* コピー元を読みながら CRC-32 と総バイト数を作り、開いてある fd へ書く。
 * 戻り値 0 = 成功 / -1 = 失敗 (*reason に固定文字列)。
 * 空き不足は `no_space` と呼び分ける (票 H2 §2-3 手順 3)。 */
static int copy_body(const char *src, int fd, u32 *out_crc, u32 *out_total,
                     const char **reason)
{
    int fs;
    u32 crc = CRC32_INIT;
    u32 total = 0;
    int failed = 0;

    *reason = HR_IO;
    fs = api->sys_open(src, KAPI_O_RDONLY);
    if (fs < 0) return -1;

    while (1) {
        int got = read_fill(fs, file_buf, FILE_BUF_SIZE);
        int wr;
        if (got < 0) { failed = 1; break; }
        if (got == 0) break;                         /* EOF */
        wr = write_all(fd, file_buf, got);
        if (wr != got) {
            if (wr == OS32_ERR_NOSPC || wr == OS32_ERR_FULL)
                *reason = HR_NO_SPACE;
            failed = 1;
            break;
        }
        crc = crc32_core_update(crc, file_buf, (u32)got);
        total += (u32)got;
        if (got < FILE_BUF_SIZE) break;              /* read_fill は EOF でのみ短い */
    }

    api->sys_close(fs);
    if (failed) return -1;
    *out_crc = crc;
    *out_total = total;
    return 0;
}

/* 書いたファイルを開き直して長さと CRC を照合する (設計書 §4.2)。
 * キャッシュを経由するので媒体からの物理再読の保証ではない
 * (最終受入は再起動後の内容で見る)。戻り値 0 = 一致 / -1 = 不一致・失敗。 */
static int verify_readback(const char *path, u32 crc, u32 total)
{
    int fd;
    u32 crc2 = CRC32_INIT;
    u32 total2 = 0;
    int failed = 0;

    fd = api->sys_open(path, KAPI_O_RDONLY);
    if (fd < 0) return -1;
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
    if (failed) return -1;

    /* 最後の XOR は 1 ストリームにつき 1 回だけ (チャンクごとに畳まない) */
    if (total2 != total || crc32_core_final(crc2) != crc32_core_final(crc))
        return -1;
    return 0;
}

/* ---- 直接上書き (**古いカーネル向けの退避経路だけ**) -------------------
 *
 * 宛先を O_TRUNC で直接開く。ここで落ちたとき**旧宛先を復元する保証は無い**。
 * 票 H2 の既定は下の replace_file (一時ファイル + 検証 + 置換) で、この関数へ
 * 来るのは KAPI v53 未満のカーネル上で `--unsafe-overwrite` を明示したときだけ。
 *
 * 戻り値 0 = 成功 / -1 = 失敗 (*reason に固定文字列)。 */
static int copy_verify(const char *src, const char *dst, u32 expect,
                       const char **reason)
{
    int fd;
    u32 crc = 0;
    u32 total = 0;

    *reason = HR_IO;
    fd = api->sys_open(dst, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_TRUNC);
    if (fd < 0) return -1;
    if (copy_body(src, fd, &crc, &total, reason) != 0) {
        api->sys_close(fd);
        return -1;
    }
    api->sys_close(fd);

    if (total != expect) { *reason = HR_SOURCE; return -1; }

    /* 書いたものをディスクへ出す。ここが落ちたら「届いていない」 */
    if (api->vfs_sync() != 0) { *reason = HR_VERIFY; return -1; }
    if (verify_readback(dst, crc, total) != 0) { *reason = HR_VERIFY; return -1; }
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

/* ======== 一時ファイル方式の道具 (票 H2 §2-3 / §2-4) ======== */

/* 定義は下の「コピー元 mtime を宛先へ」節。置き換えは**一時ファイルへ**
 * 設定してから rename するので、置換本体より前に名前だけ要る。 */
static int apply_mtime(const char *target, const char *label, u32 src_mtime);

/* 宛先と同じディレクトリの予約名 `.hs~<名前>` を組み立てる。
 * 戻り値 1 = 組めた / 0 = 名前が長すぎる。
 *
 * 長さの上限は `EXT2_NAME_LEN` ではなく **hsync 自身の列挙幅 `NAME_CAP`**
 * (票 H2 §2-3 手順 1 / Codex 往復 2 所見 7)。一時名が NAME_CAP に収まらないと
 * §2-4 の掃除の列挙で拾えなくなり、消せない予約名が残り続ける。
 * **直接上書きへは落とさない** — 落とすと H2 の保証がその 1 件だけ消える。 */
static int build_temp_path(const char *dst_path, char *out, int cap)
{
    int i, last = -1;
    const char *base;

    for (i = 0; dst_path[i]; i++) if (dst_path[i] == '/') last = i;
    base = (last >= 0) ? dst_path + last + 1 : dst_path;

    if (str_len(base) + HS_TEMP_PREFIX_LEN >= NAME_CAP) return 0;

    out[0] = '\0';
    if (last >= 0) {
        if (last + 2 > cap) return 0;
        for (i = 0; i <= last; i++) out[i] = dst_path[i];
        out[last + 1] = '\0';
    }
    if (!str_ncat(out, HS_TEMP_PREFIX, cap)) return 0;
    if (!str_ncat(out, base, cap)) return 0;
    return 1;
}

/* この実行が作った一時ファイルを片づける。
 * 消せなければ `STALE` を表示する (票 H2 §2-3 末尾 / A14c)。
 * メタデータの失敗でマウントが書き込み禁止 (ROFS) に落ちた後は unlink も
 * 通らないので、「必ず消える」とは言わない — 次の実行が予約名として消す。
 *
 * **errors に数えるかは呼び手が決める** — 1 つの失敗は 1 と数えるため:
 *   - 手順 7 (一時ファイルへの mtime 設定が落ちた) は**数えない**。ext2 は
 *     メタデータの I/O 失敗でマウントを ROFS に落とすので、続く unlink の
 *     失敗は同じ 1 つの失敗の続きであって別件ではない。apply_mtime が
 *     既に 1 件数えている。
 *   - 公開の前の失敗と手順 8 の rename 失敗では**数える**。そちらは unlink が
 *     落ちる理由が元の失敗と独立に在り得る (A14c は実際に別々の注入)。
 *
 * 戻り値 0 = 消えた / もう無い、-1 = 残った (STALE を表示済み)。 */
static int drop_temp(const char *tmp)
{
    OS32_Stat st;
    int rc = api->sys_stat(tmp, &st);

    if (rc == OS32_ERR_NOTFOUND) return 0;    /* もう無い (置換で本名になった等) */
    if (rc == 0) {
        rc = api->sys_unlink(tmp);
        if (rc == 0) return 0;
    }
    api->kprintf(ATTR_RED,
                 "  STALE %s (一時ファイルを消せない err=%d。"
                 "次の実行が予約名として片づける)\n", tmp, rc);
    return -1;
}

/* **公開の前**の失敗。旧宛先は名前も内容もそのまま残っている。 */
static void fail_before_publish(const char *dst, const char *tmp,
                                const char *reason, int err)
{
    api->kprintf(ATTR_RED,
                 "  FAIL %s reason=%s err=%d (公開の前なので旧宛先はそのまま)\n",
                 dst, reason, err);
    g_errors++;
    if (drop_temp(tmp) != 0) g_errors++;   /* 後始末の失敗は独立した 1 件 */
}

/* 手順 2 で予約名が既に在ったとき。**`st_nlink` は見ない** (途中で止まった
 * 媒体では 2 になり得る、票 H2 §2-2-4)。通常ファイルで、保護対象の実体でない
 * ものだけ消す。戻り値 0 = 消した / -1 = 消さなかった。 */
static int remove_stale_temp(const char *tmp)
{
    OS32_Stat st;

    if (api->sys_stat(tmp, &st) != 0) return -1;
    if ((st.st_mode & OS_S_IFMT) != OS_S_IFREG) return -1;  /* ディレクトリ等 */
    if (dst_protected(tmp) != 0) return -1;                 /* 保護 > 予約 (R5) */
    return api->sys_unlink(tmp) == 0 ? 0 : -1;
}

/* ---- 置き換え本体 (票 H2 §2-3 の手順 1〜9) -----------------------------
 *
 * 戻り値 0 = 置き換えた (呼び手が copied に数える) /
 *       -1 = 失敗 (表示と errors はこの中で済ませてある)。
 *
 * `*published` は**媒体の上で宛先が新しい内容に入れ替わったか**を返す。
 * 手順 8 の rename が通った時点で 1 になり、手順 9 の `vfs_sync` が落ちて
 * -1 を返すときも 1 のまま残る。rename が非ゼロを返した回でも、宛先の
 * `st_ino` が一時ファイルのものと一致する (= `replace_partial`) なら 1。
 * `replace_failed` と `replace_unknown` では 0 のまま — 前者は旧内容のまま、
 * 後者は公開したか分からないので、案内を出す根拠がない。失敗なのに置換は済んでいる場面があるので、
 * 呼び手はこれを見て**再起動の案内だけは出す** — 置換が媒体に載っているのに
 * 「/sys を更新した -> シェル再起動が必要」が消えるのは誤報になる。
 * copied に数えないのは今までどおり (errors にも入っている)。 */
static int replace_file(const char *src_path, const char *dst_path, u32 size,
                        const OS32_Stat *ss0, const OS32_Stat *ds0,
                        int dst_exists, int *published)
{
    char tmp[OS32_MAX_PATH];
    OS32_Stat ss1, ds1, ts;
    const char *reason = HR_IO;
    u32 crc = 0, total = 0;
    u32 tmp_ino = 0, old_ino = 0;
    int fd, rc, mrc, prot;

    *published = 0;

    /* 手順 1: 一時名 */
    if (!build_temp_path(dst_path, tmp, (int)sizeof(tmp))) {
        fail_file(dst_path, HR_NAME_TOO_LONG, 0);
        return -1;
    }

    /* 手順 2: 排他的作成。EXIST は §2-4 の決裁どおり 1 回だけ作り直す */
    fd = api->sys_open(tmp, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_EXCL);
    if (fd == OS32_ERR_EXIST) {
        if (remove_stale_temp(tmp) != 0) {
            fail_file(dst_path, HR_TEMP_EXISTS, 0);
            return -1;
        }
        fd = api->sys_open(tmp, KAPI_O_WRONLY | KAPI_O_CREAT | KAPI_O_EXCL);
    }
    if (fd < 0) {
        /* **NOSYS で直接上書きへ黙って落ちない** (票 H2 §2-3 手順 2)。
         * 宛先 FS が排他的作成を持たないなら、この票の保証は出せない。 */
        if (fd == OS32_ERR_NOSYS)
            fail_file(dst_path, HR_REPLACE_UNSUPPORTED, fd);
        else if (fd == OS32_ERR_EXIST)
            fail_file(dst_path, HR_TEMP_EXISTS, fd);
        else if (fd == OS32_ERR_NOSPC || fd == OS32_ERR_FULL)
            fail_file(dst_path, HR_NO_SPACE, fd);
        else
            fail_file(dst_path, HR_IO, fd);
        return -1;
    }

    /* 手順 3: 書き込み + CRC */
    rc = copy_body(src_path, fd, &crc, &total, &reason);
    api->sys_close(fd);
    if (rc != 0) { fail_before_publish(dst_path, tmp, reason, 0); return -1; }
    if (total != size) {
        fail_before_publish(dst_path, tmp, HR_SOURCE, 0);
        return -1;
    }

    /* 手順 4: 同期 → 読戻し検証 */
    if (api->vfs_sync() != 0) {
        fail_before_publish(dst_path, tmp, HR_VERIFY, 0);
        return -1;
    }
    if (verify_readback(tmp, crc, total) != 0) {
        fail_before_publish(dst_path, tmp, HR_VERIFY, 0);
        return -1;
    }

    /* 手順 5: 再確認 (コピー元・宛先・保護判定) と ino の控え */
    rc = api->sys_stat(src_path, &ss1);
    if (rc != 0) { fail_before_publish(dst_path, tmp, HR_IO, rc); return -1; }
    if (ss1.st_size != ss0->st_size || ss1.st_mtime != ss0->st_mtime) {
        fail_before_publish(dst_path, tmp, HR_SOURCE, 0);
        return -1;
    }

    rc = api->sys_stat(dst_path, &ds1);
    if (dst_exists) {
        if (rc != 0) {
            fail_before_publish(dst_path, tmp, HR_DEST_CHANGED, rc);
            return -1;
        }
        if ((ds1.st_mode & OS_S_IFMT) != OS_S_IFREG ||
            ds1.st_size != ds0->st_size || ds1.st_mtime != ds0->st_mtime) {
            fail_before_publish(dst_path, tmp, HR_DEST_CHANGED, 0);
            return -1;
        }
        old_ino = ds1.st_ino;
    } else if (rc != OS32_ERR_NOTFOUND) {
        /* 判定時に無かったものが在る / 読めない。**置き換えない** */
        fail_before_publish(dst_path, tmp, HR_DEST_CHANGED, rc);
        return -1;
    }

    /* **保護対象の判定もここで取り直す** (票 H2 §2-3 手順 5 / A17b) */
    prot = dst_protected(dst_path);
    if (prot < 0) {
        fail_before_publish(dst_path, tmp, HR_PATH_REJECT, 0);
        return -1;
    }
    if (prot > 0) {
        fail_before_publish(dst_path, tmp, HR_SETTINGS_DB, 0);
        return -1;
    }
    /* `is_same_as_protected` は「不存在以外の stat 失敗」で g_abort を立てて
     * **0 を返す**。守れないと分かった状態で置き換えへ進まない (往復 1 の B5)。 */
    if (g_abort) {
        fail_before_publish(dst_path, tmp, HR_IO, 0);
        return -1;
    }

    rc = api->sys_stat(tmp, &ts);
    if (rc != 0) { fail_before_publish(dst_path, tmp, HR_IO, rc); return -1; }
    tmp_ino = ts.st_ino;

    /* 手順 6: hardlink (呼び手が一時ファイルを作る前にも見ている) */
    if (dst_exists && ds1.st_nlink > 1) {
        fail_before_publish(dst_path, tmp, HR_HARDLINK, (int)ds1.st_nlink);
        return -1;
    }

    /* 手順 7: mtime は**一時ファイルへ**。ここでの失敗は「公開の前の失敗」
     * なので中断し、**copied に数えない** (Codex 往復 1 所見 4: ext2 は
     * メタデータの I/O 失敗でマウントを書き込み禁止にするので、続く rename は
     * 必ず ROFS になり、宛先は旧内容のまま)。 */
    mrc = apply_mtime(tmp, dst_path, ss1.st_mtime);
    if (mrc < 0) {
        /* **数え直さない**。ext2 は mtime の I/O 失敗でマウントを ROFS に
         * 落とすので、ここで unlink が落ちるのは同じ 1 つの失敗の続きで、
         * apply_mtime が既に 1 件数えている (1 failure = 1 error)。
         * 表示 (metadata_failed と STALE) は両方出す。 */
        (void)drop_temp(tmp);
        return -1;
    }

    /* 手順 8: 置換 */
    rc = api->sys_rename(tmp, dst_path);
    if (rc != 0) {
        /* **公開の有無は宛先の st_ino で決める** (票 H2 §2-3 手順 8)。
         * サイズと CRC は証拠にしない — `-f` で新旧が同じ内容のときに
         * 未公開を公開と誤るし、CRC 不一致は「旧内容のまま」の証明にもならない。 */
        const char *why = HR_REPLACE_UNKNOWN;
        const char *note = " (公開したか判定できない)";
        OS32_Stat now;
        int srr = api->sys_stat(dst_path, &now);

        if (srr == 0) {
            if (now.st_ino == tmp_ino) {
                why = HR_REPLACE_PARTIAL;
                note = " (公開済み: 宛先は検証済みの新しい内容。後始末が落ちた)";
                /* **公開済みなので案内は出す** (PM 決裁 2026-09-16、手順 9 と
                 * 同じ理屈)。媒体の上で内容は入れ替わっているのに
                 * 「/sys を更新した -> シェル再起動が必要」が消えるのは誤報。
                 * `replace_failed` (旧内容のまま) と `replace_unknown`
                 * (どちらか分からない) では**立てない** — 案内を出す根拠がない。 */
                *published = 1;
            } else if (dst_exists && now.st_ino == old_ino) {
                why = HR_REPLACE_FAILED;
                note = " (未公開: 宛先は旧内容のまま)";
            }
        } else if (!dst_exists && srr == OS32_ERR_NOTFOUND) {
            why = HR_REPLACE_FAILED;
            note = " (未公開: 宛先は作られていない)";
        }
        api->kprintf(ATTR_RED, "  FAIL %s reason=%s err=%d%s\n",
                     dst_path, why, rc, note);
        g_errors++;
        if (drop_temp(tmp) != 0) g_errors++; /* 後始末の失敗は独立した 1 件 */
        return -1;
    }

    /* ここから先、宛先の名前は**検証済みの新しい実体**を指している。
     * 以降の失敗で旧内容へ戻そうとはしない (票 H2 の規則 2)。 */
    *published = 1;

    /* 手順 9: 同期 */
    if (api->vfs_sync() != 0) {
        api->kprintf(ATTR_RED,
                     "  FAIL %s reason=%s (置換は済んでいる可能性がある)\n",
                     dst_path, HR_SYNC_FAILED);
        g_errors++;
        return -1;
    }
    return 0;
}

/* ======== コピー元 mtime を宛先へ (票 H3 / 設計書 §5.2) ======== */

/* **データを書き終えてから**呼ぶこと。通常の書き込みは mtime を
 * ゲストの現在時刻で上書きするので、先に設定すると消える。
 * 宛先の ctime はゲスト側の変更時刻でよい (カーネルがそうする)。
 *
 *   戻り値  1 … 設定した
 *           0 … 設定しなかったが**失敗ではない**
 *                (src_mtime が 0 = 不明 / 宛先 FS が持っていない = NOSYS)
 *          -1 … 有効な時刻の保存を試みて失敗した
 *                (呼び手が metadata_failed + errors に数える)
 *
 * dry-run では 1 バイトも書かないので呼ばない (呼び手側で分岐する)。
 *
 * 票 H2: 置き換え経路では**一時ファイル**に設定するので、設定先 (target) と
 * 表示する名前 (label) を分ける。rename は inode の mtime を変えないので、
 * 本名に現れた時点で日時が揃っている。 */
static int apply_mtime(const char *target, const char *label, u32 src_mtime)
{
    int rc;

    if (src_mtime == 0) {
        /* 証拠が無いものを書かない。**省略したことは必ず見せる** */
        g_mtime_unknown++;
        if (g_verbose)
            api->kprintf(ATTR_YELLOW, "  NOTIME %s reason=%s\n",
                         label, HR_MTIME_UNKNOWN);
        return 0;
    }

    rc = api->sys_set_mtime(target, src_mtime);
    if (rc == 0) return 1;

    if (rc == OS32_ERR_NOSYS) {
        /* 「できなかった」ではなく「この FS には無い」。内容の同期は続ける */
        g_mtime_nosys++;
        if (g_verbose)
            api->kprintf(ATTR_YELLOW, "  NOTIME %s reason=%s\n",
                         label, HR_MTIME_NOSYS);
        return 0;
    }

    /* 有効な時刻を書こうとして落ちた。内容コピーの成功だけで
     * 全成功と表示しない (設計書 §5.2)。 */
    fail_file(label, HR_META_FAILED, rc);
    return -1;
}

static void sync_file(const char *src_path, const char *dst_path)
{
    OS32_Stat ss;
    OS32_Stat ds;
    const char *reason = HR_NEW;
    const char *vreason = HR_IO;
    u32 size;
    int rc;
    int cmp;
    int mrc;
    int need = 1;
    int meta_only = 0;       /* 内容は同じで mtime だけ違う (票 H3) */
    int dst_exists = 0;      /* 判定時に宛先が在ったか (票 H2 手順 5 / 8) */
    int published = 0;       /* 置換が媒体に載ったか (票 H2 手順 8 / 9) */

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
    if (rc == 0) dst_exists = 1;
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
        /* サイズが違う = 内容も必ず違う。読み比べる意味が無い */
        reason = HR_SIZE;
    } else {
        /* ---- ここからが票 H3 §8 の判定順 ----------------------------
         *
         * サイズは同じ。**日時を前置フィルタに使う**が、「証拠が無い」
         * ことを同一の根拠にはしない:
         *
         *   - どちらかの mtime が 0 (不明)        → 内容を比較する
         *   - mtime が違う                        → 内容を比較する
         *   - `--verify`                          → 内容を比較する
         *   - どちらも有効で一致                  → 読まずにスキップ
         *
         * **日時の一致だけでコピーを決めない。**候補に挙がったものは
         * 必ずバイト比較を通し、コピーするかは内容が決める (H1 の A08 の
         * 精神 — CRC が同値を返しても既定のバイト比較が違いを見つける)。 */
        int mtime_known = (ss.st_mtime != 0 && ds.st_mtime != 0);
        int mtime_same  = mtime_known && (ss.st_mtime == ds.st_mtime);

        if (!g_verify && mtime_same) {
            /* サイズも日時も同じ。**1 バイトも読まない** — ここが速さの源 */
            g_unchanged++;
            if (g_verbose)
                api->kprintf(ATTR_WHITE, "  SAME %s size=%d reason=%s\n",
                             dst_path, (int)size, HR_SAME_MTIME);
            return;
        }

        cmp = compare_files(src_path, dst_path, size);
        if (cmp < 0) { fail_file(dst_path, HR_IO, 0); return; }
        if (cmp > 0) {
            reason = HR_CONTENT;
        } else {
            need = 0;
            /* 内容は同じ。日時だけ違うなら**本体を書き直さず mtime だけ
             * 更新する** (設計書 §5.2)。
             *
             * 条件は「宛先の mtime がコピー元と**一致していると言えない**」。
             * コピー元が不明 (0) のときもここに入るが、apply_mtime が
             * 0 を書きに行かず「省略した」と数える — **省略したことを
             * 黙らせない**のが要点 (票 H3)。 */
            meta_only = !(ss.st_mtime != 0 && ss.st_mtime == ds.st_mtime);
        }
    }

    if (!need) {
        if (meta_only) {
            if (g_dry_run) {
                /* dry-run は mtime も書かない (設計書 §3.2) */
                g_metadata_updated++;
                api->kprintf(ATTR_CYAN, "  PLAN %s reason=%s size=%d\n",
                             dst_path, HR_MTIME_ONLY, (int)size);
                return;
            }
            mrc = apply_mtime(dst_path, dst_path, ss.st_mtime);
            if (mrc < 0) return;            /* metadata_failed (errors 済み) */
            if (mrc > 0) {
                /* **内容は同じ** = 稼働中の版とディスクの食い違いは
                 * 生まれない。note_target は呼ばない (再起動の案内を
                 * 出すと「入れ替わった」と読めてしまう)。 */
                api->kprintf(ATTR_GREEN, "  MTIME %s reason=%s size=%d\n",
                             dst_path, HR_MTIME_ONLY, (int)size);
                g_metadata_updated++;
                return;
            }
            /* 設定を省略した (NOSYS / 不明)。内容は同じなので unchanged */
        }
        g_unchanged++;
        if (g_verbose)
            api->kprintf(ATTR_WHITE, "  SAME %s size=%d reason=%s\n",
                         dst_path, (int)size, HR_SAME_CONTENT);
        return;
    }

    if (g_dry_run) {
        /* 読んで比べるだけ。mkdir・一時ファイル・明示 sync・mtime はしない */
        g_copied++;
        api->kprintf(ATTR_CYAN, "  PLAN %s reason=%s size=%d\n",
                     dst_path, reason, (int)size);
        return;
    }

    if (g_direct) {
        /* ---- 古いカーネル向けの直接上書き (票 H2 §2-3 末尾) ----
         * `--unsafe-overwrite` を明示したときだけここへ来る。
         * **失敗すると旧内容は残らない。** */
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
        g_direct_overwrite++;
        note_target(dst_path);
        /* **データを書き終えてから**コピー元の mtime を宛先へ (設計書 §5.2)。
         * ここで落ちてもコピー自体は成功しているので copied は戻さないが、
         * metadata_failed は errors に入る = 終了コードは非ゼロになる。 */
        (void)apply_mtime(dst_path, dst_path, ss.st_mtime);
        return;
    }

    /* ---- 票 H2 の既定: 一時ファイル + 検証 + 置換 ----
     * 手順 6 の hardlink 判定は**一時ファイルを作る前にも**行う
     * (無駄な書き込みを避ける)。別名まで更新する仕様は持ち込まない。 */
    if (dst_exists && ds.st_nlink > 1) {
        fail_file(dst_path, HR_HARDLINK, (int)ds.st_nlink);
        return;
    }

    if (replace_file(src_path, dst_path, size, &ss, &ds, dst_exists,
                     &published) != 0) {
        /* 表示と errors は replace_file の中で済んでいる。ただし
         * **置換だけは媒体に載っている**場合 (手順 9 の sync_failed と、
         * 手順 8 の replace_partial) は再起動の案内を出す — 出さないと
         * 「/sys は入れ替わっていない」と読める誤報になる。
         * copied には数えない。 */
        if (published) note_target(dst_path);
        return;
    }

    /* mtime は一時ファイルに設定済み (rename は inode の mtime を変えない) */
    api->kprintf(ATTR_GREEN, "  UPDATE %s reason=%s size=%d\n",
                 dst_path, reason, (int)size);
    g_copied++;
    note_target(dst_path);
}

/* ======== 予約名 `.hs~` の掃除 (票 H2 §2-4、決裁 D3 (a')) ======== */

/* 掃除の列挙は**同期の列挙と枠を分ける** (Codex 往復 2 所見 7)。候補は
 * `.hs~` で始まる名前だけなので小さくてよい。越えたら `truncated` と同じ扱いで
 * **「全部は見ていない」と表示する** — 掃除の完了を主張しない。 */
#define MAX_TEMPS 32

typedef struct {
    char names[MAX_TEMPS][NAME_CAP];
    int  count;
    int  dropped;      /* 掃除の枠 MAX_TEMPS を越えて見送った数 */
    int  too_long;     /* NAME_CAP に収まらず拾えなかった数 (枠とは別の理由) */
} TempList;

static void temp_cb(const DirEntry_Ext *entry, void *ctx)
{
    TempList *tl = (TempList *)ctx;
    int i;

    if (!hs_is_temp_name(entry->name)) return;
    /* ディレクトリは消さない (§2-4)。特殊ファイルは下の stat で弾く */
    if (entry->type == OS32_FILE_TYPE_DIR) return;
    /* **枠と長さは別の理由**。列挙幅に入らないだけのものを「掃除の枠を
     * 越えた」と呼ぶと、MAX_TEMPS を広げれば直ると読めてしまう (直らない)。
     * 結論の「全部は見ていない」はどちらも同じなので、そこは変えない。 */
    if (!hsp_name_fits(entry->name, NAME_CAP)) { tl->too_long++; return; }
    if (tl->count >= MAX_TEMPS) { tl->dropped++; return; }

    i = 0;
    while (entry->name[i]) { tl->names[tl->count][i] = entry->name[i]; i++; }
    tl->names[tl->count][i] = '\0';
    tl->count++;
}

/* hsync が**実際に訪れた**ディレクトリの予約名を片づける。
 * 既定除外の /sys へは再帰しないので触らない。保護対象の実体 (hardlink) は
 * **予約より保護を優先して消さない** (R5)。`st_nlink` は見ない。 */
static void clean_temps(const char *dst_dir)
{
    TempList tl;
    char dir[OS32_MAX_PATH];
    char path[OS32_MAX_PATH];
    OS32_Stat st;
    int i, rc;

    if (!str_ncpy(dir, dst_dir[0] ? dst_dir : "/", (int)sizeof(dir))) return;

    tl.count = 0;
    tl.dropped = 0;
    tl.too_long = 0;
    rc = api->sys_ls(dir, temp_cb, &tl);
    if (rc != 0) {
        /* 宛先ディレクトリがまだ無いのは普通 (新規階層)。それ以外は
         * 「掃除できなかった」ことだけ見せる — 同期は続ける。 */
        if (rc != OS32_ERR_NOTFOUND)
            api->kprintf(ATTR_YELLOW,
                         "  NOTE: %s の予約名を列挙できない (err=%d)。"
                         "掃除は行っていない\n", dir, rc);
        return;
    }
    if (tl.dropped)
        api->kprintf(ATTR_YELLOW,
                     "  NOTE: %s の予約名が掃除の枠 %d を越えた (+%d)。"
                     "**全部は見ていない**\n", dir, MAX_TEMPS, tl.dropped);
    if (tl.too_long)
        api->kprintf(ATTR_YELLOW,
                     "  NOTE: %s に %d 文字を越える予約名が %d 件 "
                     "(掃除の枠ではなく名前の長さ)。**全部は見ていない**\n",
                     dir, NAME_CAP - 1, tl.too_long);

    for (i = 0; i < tl.count; i++) {
        if (!str_ncpy(path, dir, (int)sizeof(path)) ||
            ((str_len(path) == 0 || path[str_len(path) - 1] != '/') &&
             !str_ncat(path, "/", (int)sizeof(path))) ||
            !str_ncat(path, tl.names[i], (int)sizeof(path))) {
            api->kprintf(ATTR_RED, "  FAIL: path too long: %s/%s\n",
                         dir, tl.names[i]);
            g_errors++;
            continue;
        }

        {
            int prot = dst_protected(path);
            if (prot < 0) {
                api->kprintf(ATTR_RED, "  FAIL %s reason=%s\n",
                             path, HR_PATH_REJECT);
                g_errors++;
                continue;
            }
            if (prot > 0) {
                /* 予約名だが保護対象の実体を指している。**消さない** (R5) */
                api->kprintf(ATTR_YELLOW, "  PROTECTED %s reason=%s\n",
                             path, HR_PROT_RESERVED);
                g_protected++;
                continue;
            }
        }
        if (g_abort) return;

        rc = api->sys_stat(path, &st);
        if (rc == OS32_ERR_NOTFOUND) continue;
        if (rc != 0) { fail_file(path, HR_IO, rc); continue; }
        /* 通常ファイルだけ。`st_nlink` は見ない (§2-2-4 の復旧表) */
        if ((st.st_mode & OS_S_IFMT) != OS_S_IFREG) continue;

        if (g_dry_run) {
            api->kprintf(ATTR_CYAN, "  PLAN-CLEAN %s\n", path);
            g_cleaned++;
            continue;
        }
        rc = api->sys_unlink(path);
        if (rc != 0) { fail_file(path, HR_IO, rc); continue; }
        api->kprintf(ATTR_GREEN, "  CLEAN %s\n", path);
        g_cleaned++;
    }
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

    fl.src_dir = src_dir;
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

    /* 予約名 `.hs~` の掃除は**この実行が一時ファイルを作る前**に行う
     * (票 H2 §2-4)。前の実行が電源断などで残したものを片づけてから同期に
     * 入るので、手順 2 の EXIST 経路に落ちる回数が減る。 */
    if (!g_direct) clean_temps(dst_dir);
    if (g_abort) return;

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
    api->kprintf(ATTR_WHITE, "Usage: hsync [-f] [-n] [-v] [--verify] [dir]\n");
    api->kprintf(ATTR_WHITE, "  -f, --force     同一判定を省いて上書き (保護・検証は省かない)\n");
    api->kprintf(ATTR_WHITE, "      --verify    日時を見ず、全件の内容を必ず比較する (遅い)\n");
    api->kprintf(ATTR_WHITE, "  -n, --dry-run   読んで比べるだけ。1 バイトも書かない (mtime も)\n");
    api->kprintf(ATTR_WHITE, "  -v, --verbose   スキップ理由と比較結果も出す\n");
    api->kprintf(ATTR_WHITE, "      --unsafe-overwrite  KAPI v53 未満のカーネルで**直接上書き**する\n");
    api->kprintf(ATTR_WHITE, "                  (旧内容は残らない。既定は kernel_too_old で断る)\n");
    api->kprintf(ATTR_WHITE, "  -h, --help      この表示\n");
    api->kprintf(ATTR_WHITE, "  dir             同期対象は 1 つだけ (例: bin, sys, usr/bin)\n");
    api->kprintf(ATTR_WHITE, "  既定: サイズか日時が違うものだけ内容を比較し、違えばコピーする\n");
    api->kprintf(ATTR_WHITE, "        日時が不明 (0) なら必ず内容を比較する\n");
    api->kprintf(ATTR_WHITE, "        見逃すのは「サイズも日時も同じで中身が違う」場合だけ\n");
    api->kprintf(ATTR_WHITE, "  全体同期ではルート直下の sys を除外する (-f でも解除しない)\n");
    api->kprintf(ATTR_WHITE, "  置き換えは一時ファイル `%s<名前>` 経由。この接頭辞は hsync の予約\n",
                 HS_TEMP_PREFIX);
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
    g_metadata_updated = 0;
    g_cleaned = 0;
    g_errors = 0;
    g_mtime_unknown = 0;
    g_mtime_nosys = 0;
    g_force = 0;
    g_dry_run = 0;
    g_verbose = 0;
    g_verify = 0;
    g_unsafe = 0;
    g_direct = 0;
    g_direct_overwrite = 0;
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
            } else if (str_cmp(a, "--verify") == 0) {
                /* 票 H3 §8: 日時によるスキップをしない = H1 の現挙動。
                 * 確実さが要るときだけ払う費用 (短い別名は付けない — 誤って
                 * -v と打ち間違えたときに黙って遅くなるのを避ける)。 */
                g_verify = 1;
            } else if (str_cmp(a, "--unsafe-overwrite") == 0) {
                /* 票 H2 §2-3 末尾: 古いカーネルでの直接上書きを明示する。
                 * **`-f` では解除されない** — 別の意味の旗なので短縮形も
                 * 用意しない。 */
                g_unsafe = 1;
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

    /* ---- カーネルの版の門 (票 H2 §2-3 末尾) ----------------------------
     *
     * 一時ファイル方式は v53 の O_EXCL と、ext2 の新しい置き換え順序が
     * そろって初めて成立する。v53 未満では O_EXCL が**黙って無視され**、
     * 置き換えも旧順序 (宛先を先に消す) なので、H2 の保証は何ひとつ出せない。
     * **既定は 1 件も書かずに断る。** 直接上書きが要るなら明示させる —
     * 「更新の道が無くなる」は成り立たない (カーネルは停止中の NHD 配備
     * 経路で入れ替えられる、Codex 往復 1 所見 5)。`-f` では解除しない。
     *
     * 判定は dry-run でも同じにする: 断る条件を実行の種類で変えると、
     * `-n` が通ったのに本番が止まる、という分かりにくい形になる。 */
    if (api->version < HS_MIN_KAPI_H2) {
        if (!g_unsafe) {
            api->kprintf(ATTR_RED,
                         "Error: kernel KAPI v%d < v%d reason=%s\n",
                         (int)api->version, HS_MIN_KAPI_H2, HR_KERNEL_TOO_OLD);
            api->kprintf(ATTR_RED,
                         "  一時ファイル方式 (票 H2) が成立しないので 1 件も書かない。\n"
                         "  新しいカーネルを配備するか、旧来の直接上書きでよければ\n"
                         "  --unsafe-overwrite を明示すること (**失敗すると旧内容は残らない**)。\n");
            return 1;
        }
        g_direct = 1;
        api->kprintf(ATTR_YELLOW,
                     "WARN kernel KAPI v%d < %d: direct overwrite (no H2)\n",
                     (int)api->version, HS_MIN_KAPI_H2);
    } else if (g_unsafe) {
        api->kprintf(ATTR_YELLOW,
                     "NOTE: kernel KAPI v%d >= %d なので --unsafe-overwrite は無視する "
                     "(一時ファイル方式で進む)\n",
                     (int)api->version, HS_MIN_KAPI_H2);
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
    if (g_verify) {
        api->kprintf(ATTR_YELLOW,
                     "  (verify mode: 日時で省略せず、全件の内容を比較する)\n");
    }
    if (g_dry_run) {
        api->kprintf(ATTR_YELLOW,
                     "  (dry-run: 読み取りと比較だけ。mkdir・書き込み・sync・mtime はしない)\n");
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
                 "\n%s copied=%d unchanged=%d excluded=%d protected=%d "
                 "metadata_updated=%d cleaned=%d errors=%d%s\n",
                 hsp_final_label(g_errors),
                 g_copied, g_unchanged, g_excluded, g_protected,
                 g_metadata_updated, g_cleaned, g_errors,
                 g_dry_run ? " (dry-run: copied は予定件数)" : "");
    if (g_direct_overwrite)
        api->kprintf(ATTR_YELLOW,
                     "direct_overwrite=%d "
                     "(KAPI v%d < %d: 一時ファイル方式を使っていない。"
                     "**失敗した回の旧内容は残らない**)\n",
                     g_direct_overwrite, (int)api->version, HS_MIN_KAPI_H2);
    if (g_dry_run && g_metadata_updated)
        api->kprintf(ATTR_CYAN,
                     "  (dry-run: metadata_updated も予定件数。"
                     "mtime は 1 件も書いていない)\n");

    /* **省略したことを必ず見せる** (票 H3)。集計の 6 区分とは別に数える —
     * 失敗ではないが、「時刻を保存したつもり」で終わらせない。 */
    if (g_mtime_unknown)
        api->kprintf(ATTR_YELLOW,
                     "NOTE: 元の mtime が不明 (0) で時刻の保存を省略: %d 件 "
                     "reason=%s (内容の同期は行った)\n",
                     g_mtime_unknown, HR_MTIME_UNKNOWN);
    if (g_mtime_nosys)
        api->kprintf(ATTR_YELLOW,
                     "NOTE: 宛先 FS が mtime の設定に対応していない: %d 件 "
                     "reason=%s (内容の同期は行った)\n",
                     g_mtime_nosys, HR_MTIME_NOSYS);

    /* 「ディスクへ同期した」と「稼働中の版が入れ替わった」は別のこと
     * (設計書 §7.2)。再起動はここでは行わない。 */
    /* `g_copied` だけを条件にすると、**置換は済んだのに sync が落ちた**回
     * (票 H2 手順 9) で案内が丸ごと消える。note_target は媒体の上で内容が
     * 入れ替わったときだけ立つので、そちらも条件に入れる。 */
    if (!g_dry_run && (g_copied > 0 || g_touched_sys || g_touched_boot)) {
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
