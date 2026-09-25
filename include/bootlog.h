/* ======================================================================== */
/*  BOOTLOG.H — 最後の起動のログ (/var/log/boot.log)                         */
/*                                                                          */
/*  実機では起動画面の [selftest] などがすぐ流れて読めず、rshell が立つ前の  */
/*  kprintf はシリアルにも出ない。そこで最初の kprintf から常駐シェルを      */
/*  exec する直前までの出力 (文字だけ、色は捨てる) をカーネルの静的配列に   */
/*  溜め、ルートのマウント後に 1 回だけファイルへ書く。                      */
/*                                                                          */
/*  方針:                                                                   */
/*    - あふれたら**後ろを捨てて先頭を残す** (起動の最初が大事)。一度あふれ  */
/*      たら以後は全部捨てる (後から来た短い行で途中が抜けた形にしない)。   */
/*      捨てたバイト数は末尾の行に書く。                                     */
/*    - 割込み文脈 (ISR の kprintf) からも積めるよう IF を退避する           */
/*      (kernel/con_sink.c と同じ書き方)。                                  */
/*    - 書き出しの手順は BootlogFsOps 越しで、偽の VFS で試験できる          */
/*      (tools/tests/bootlog_host.c)。                                     */
/*                                                                          */
/*  純粋な部分は kernel/bootlog.c、VFS と版情報へ結ぶのは                  */
/*  kernel/bootlog_save.c。記録: tools/tests/bootlog_tdd.md                 */
/* ======================================================================== */

#ifndef __BOOTLOG_H
#define __BOOTLOG_H

#include "types.h"
#include "config.h"

/* ヘッダ 1 行と末尾 1 行の置き場。本文の前後に確保しておき、書き出しの時に
 * 連続した 1 本の領域にする (写し用の 2 本目の 16KB を持たない)。 */
#define BOOTLOG_HDR_MAX    192
#define BOOTLOG_TAIL_MAX   80

/* 書き出し先の FS の種別 (bootlog_plan)。 */
#define BOOTLOG_FS_SKIP    0     /* 書かない (HostDrv / iso9660 / 未マウント) */
#define BOOTLOG_FS_EXT2    1
#define BOOTLOG_FS_FAT     2

/* 書き出しの段 (bootlog_save_with の戻り = 最初に失敗した段、0 = 全部通った) */
#define BOOTLOG_ST_OK         0
#define BOOTLOG_ST_MKDIR_VAR  1
#define BOOTLOG_ST_MKDIR_LOG  2
#define BOOTLOG_ST_RM_OLD     3
#define BOOTLOG_ST_ROTATE     4
#define BOOTLOG_ST_WRITE      5
#define BOOTLOG_ST_SYNC       6

typedef struct {
    int (*mkdir)(const char *path);
    int (*rm)(const char *path);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*write)(const char *path, const void *data, u32 size);  /* 書いたバイト数 */
    int (*sync)(void);
} BootlogFsOps;

typedef struct {
    const char *build;     /* __DATE__ __TIME__ (kapi_sys_get_build_info) */
    const char *commit;    /* os32_build_commit */
    int  crc_valid;
    u32  image_crc;
    u32  image_size;
    u32  ticks;            /* tick_count (10ms) */
} BootlogHeaderInfo;

/* --- 積む側 (kernel/console.c の入口から。割込み文脈からも呼ばれる) ---- */
void bootlog_push(const char *buf, u32 len);

/* --- 止める・読む ---------------------------------------------------- */
void bootlog_stop(void);          /* 以後は積まない (書き出しの直前) */
int  bootlog_is_active(void);
u32  bootlog_len(void);           /* 溜めた本文のバイト数 */
u32  bootlog_dropped(void);       /* あふれで捨てたバイト数 */

/* ヘッダ 1 行 ("# OS32 boot log  Build …\n") を buf に組む。戻りは長さ
 * (NUL を除く)。cap に収まらない分は切る (必ず NUL 終端、末尾は '\n')。 */
u32 bootlog_format_header(char *buf, u32 cap, const BootlogHeaderInfo *hi);

/* ヘッダ + 本文 + 末尾行 ("# end  kept N bytes  dropped M bytes\n") を
 * 連続した 1 本にして先頭を返す。*out_len にその長さ。本文は動かさない
 * ので、積んでいる最中に呼んでもよい (kselftest が使う)。 */
const char *bootlog_compose(const char *header, u32 *out_len);

/* ルートの FS の名前 (vfs_fstype("/")) から書き出し先の種別を決める */
int bootlog_plan(const char *fstype);
/* 種別ごとの前回分の名前 (FAT は 8.3)。SKIP なら 0 */
const char *bootlog_old_path(int kind);

/* 書き出しの手順:
 *   1. /var, /var/log を作る (EXIST は成功)。作れなければそこで止める
 *   2. 前回分 (.1) を消す (NOTFOUND は成功)
 *   3. boot.log → .1 に付け替える (NOTFOUND は成功)。2 が失敗したら飛ばす
 *   4. data を boot.log に書く (2・3 が失敗しても書く — 今回のログが優先)
 *   5. sync
 * 戻りは最初に失敗した段 (BOOTLOG_ST_*)、*fail_rc にその戻り値。 */
int bootlog_save_with(const BootlogFsOps *ops, int kind,
                      const char *data, u32 len, int *fail_rc);
const char *bootlog_stage_name(int stage);

/* --- カーネルの結線 (kernel/bootlog_save.c) --------------------------- */
/* 常駐シェルを exec する直前に 1 回だけ呼ぶ。失敗しても起動は続ける。 */
void bootlog_save(void);

#endif /* __BOOTLOG_H */
