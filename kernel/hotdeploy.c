/* ======================================================================== */
/*  hotdeploy.c — 再起動なしのバイナリ差し替え (ゲスト側エージェント)        */
/*                                                                          */
/*  ホストは NP21/W 内蔵 aidebug の POST /api/deploy を呼ぶ。エミュレータは */
/*  ステージング領域へバイト列を書き、制御ブロックに要求を立てるだけで、     */
/*  ファイル化はこのエージェントが行う。稼働中のゲストの背後で ext2 を      */
/*  直接書くとブロックキャッシュと必ず食い違うため、書き込みは必ずゲスト側。 */
/*                                                                          */
/*  ポーリングは割り込み文脈では行わない。ディスク I/O を ISR から呼べない  */
/*  ので、CPU がカーネルに戻っていて他の VFS 操作が走っていない地点          */
/*  (sys_halt / exec_exit / シェル再ロード待ち) で hotdeploy_poll() を叩く。 */
/*                                                                          */
/*  設計: docs/tasks/hotdeploy/DESIGN.md                                    */
/* ======================================================================== */
#include "types.h"
#include "memmap.h"
#include "vfs.h"
#include "kprintf.h"
#include "kstring.h"
#include "crc32.h"
#include "sys.h"
#include "hotdeploy.h"

#define HD_MAGIC0 'O'
#define HD_MAGIC1 'S'
#define HD_MAGIC2 '3'
#define HD_MAGIC3 '2'
#define HD_MAGIC4 'D'
#define HD_MAGIC5 'P'
#define HD_MAGIC6 'L'
#define HD_MAGIC7 'Y'

#define HD_VERSION 1

#define HD_IDLE  0u
#define HD_REQ   1u
#define HD_BUSY  2u
#define HD_DONE  3u
#define HD_ERROR 4u

/* エラー種別 (ホストが読む) */
#define HD_ERR_NONE    0
#define HD_ERR_LENGTH  1
#define HD_ERR_CRC     2
#define HD_ERR_WRITE   3
#define HD_ERR_PATH    4
#define HD_ERR_DENIED  5   /* システム領域は受け付けない */

/* 制御ブロック。オフセットはホスト側 (aidebug_api.cpp / hotdeploy.py) と
 * 一対一で対応する。並びを変えたら両方直すこと。 */
typedef struct {
    char magic[8];      /* 0x00 "OS32DPLY" */
    u32  version;       /* 0x08 */
    u32  buf_phys;      /* 0x0C ステージング領域の物理先頭 */
    u32  buf_size;      /* 0x10 */
    u32  seq;           /* 0x14 ホストが要求ごとに増やす */
    u32  status;        /* 0x18 */
    u32  length;        /* 0x1C */
    u32  crc;           /* 0x20 */
    i32  err;           /* 0x24 */
    u32  reserved[6];   /* 0x28 */
    char path[256];     /* 0x40 */
} HotDeployDesc;

static volatile HotDeployDesc *hd(void)
{
    return (volatile HotDeployDesc *)MEM_HOTDEPLOY_DESC;
}

/* ------------------------------------------------------------------------ */
/*  hotdeploy_init — ブート時に制御ブロックを初期化                          */
/* ------------------------------------------------------------------------ */
void hotdeploy_init(void)
{
    volatile HotDeployDesc *d = hd();
    u32 i;

    for (i = 0; i < sizeof(HotDeployDesc); i++) {
        ((volatile u8 *)d)[i] = 0;
    }

    d->version  = HD_VERSION;
    /* 予約した末尾がそのまま窓。sys_usable_mem_end() は H2 以降 PEGC の
     * バックバッファ予約でさらに下がるので、窓の位置は専用の getter で取る
     * (両者は 9801 では同じ値)。 */
    d->buf_phys = sys_hotdeploy_base();
    d->buf_size = MEM_HOTDEPLOY_SIZE;
    d->status   = HD_IDLE;

    /* magic は最後に書く。ホストはこれを見て有効判定するので、
     * 中身が揃う前に magic が立っていると壊れた記述子を掴まれる。 */
    d->magic[0] = HD_MAGIC0; d->magic[1] = HD_MAGIC1;
    d->magic[2] = HD_MAGIC2; d->magic[3] = HD_MAGIC3;
    d->magic[4] = HD_MAGIC4; d->magic[5] = HD_MAGIC5;
    d->magic[6] = HD_MAGIC6; d->magic[7] = HD_MAGIC7;
}

static int hd_magic_ok(volatile HotDeployDesc *d)
{
    return d->magic[0] == HD_MAGIC0 && d->magic[1] == HD_MAGIC1 &&
           d->magic[2] == HD_MAGIC2 && d->magic[3] == HD_MAGIC3 &&
           d->magic[4] == HD_MAGIC4 && d->magic[5] == HD_MAGIC5 &&
           d->magic[6] == HD_MAGIC6 && d->magic[7] == HD_MAGIC7 &&
           d->version == HD_VERSION;
}

/* 先頭一致。prefix は NUL 終端。 */
static int hd_has_prefix(const char *path, const char *prefix)
{
    int i;
    for (i = 0; prefix[i] != '\0'; i++) {
        if (path[i] != prefix[i]) return 0;
    }
    return 1;
}

/* 書き換えを拒むシステム領域か。 */
static int hd_path_is_system(const char *path)
{
    return hd_has_prefix(path, "/boot/") || hd_has_prefix(path, "/sys/");
}

/* ------------------------------------------------------------------------ */
/*  hotdeploy_poll — 要求があればファイル化する                              */
/*                                                                          */
/*  安全地点からのみ呼ぶこと。再入しないよう status を先に BUSY にする。     */
/* ------------------------------------------------------------------------ */
void hotdeploy_poll(void)
{
    volatile HotDeployDesc *d = hd();
    char path[256];
    u32 len, want, got;
    const void *src;
    int rc;
    u32 i;

    if (!hd_magic_ok(d) || d->status != HD_REQ) {
        return;
    }
    d->status = HD_BUSY;

    len  = d->length;
    want = d->crc;

    if (len == 0 || len > d->buf_size) {
        d->err = HD_ERR_LENGTH;
        d->status = HD_ERROR;
        return;
    }

    /* パスは volatile 越しなのでローカルへ写す。NUL 終端も自分で保証する。 */
    for (i = 0; i < sizeof(path) - 1; i++) {
        path[i] = d->path[i];
        if (path[i] == '\0') break;
    }
    path[sizeof(path) - 1] = '\0';
    if (path[0] != '/') {
        d->err = HD_ERR_PATH;
        d->status = HD_ERROR;
        return;
    }

    /* ホストからの差し替えはユーザーランドに限る。カーネル本体
     * (/boot/vmkernel.lz4) とシステム常駐物 (/sys/shell.bin, sqlite.bin,
     * unicode.bin, フォント) は、稼働中に書き換えると走っている当人を
     * 壊すか、次回ブートを壊す。これらは NHD フル配備で入れ替えること。 */
    if (hd_path_is_system(path)) {
        d->err = HD_ERR_DENIED;
        d->status = HD_ERROR;
        return;
    }

    /* ホストが書き終える前に叩かれた場合や、前回の残骸を掴んだ場合を
     * ここで落とす。CRC を必須にしているのはそのため。 */
    src = (const void *)d->buf_phys;
    got = crc32_calc(src, len);
    if (got != want) {
        d->err = HD_ERR_CRC;
        d->status = HD_ERROR;
        return;
    }

    /* vfs_write の成功は 0 (ext2_write は EXT2_OK を返す)。バイト数を返す
     * バックエンドもあり得るので両方を成功とみなす。失敗時は生の戻り値を
     * err に載せる — 定数だけだと原因の切り分けが遠回りになる。 */
    rc = vfs_write(path, src, len);
    if (rc != 0 && (rc < 0 || (u32)rc != len)) {
        d->err = (rc < 0) ? rc : HD_ERR_WRITE;
        d->status = HD_ERROR;
        return;
    }

    d->err = HD_ERR_NONE;
    d->status = HD_DONE;
    kprintf(0x0A, "hotdeploy: %s (%u bytes)\n", path, len);
}
