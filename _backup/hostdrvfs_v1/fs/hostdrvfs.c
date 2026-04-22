/* ======================================================================== */
/*  HOSTDRVFS.C — NP21/W HostDrv(NT) VFSドライバ                          */
/*                                                                          */
/*  I/Oポート 0x7EC/0x7EE を通じてNP21/Wエミュレータのhostdrvnt機能と      */
/*  通信し、ホストPCのファイルシステムにアクセスする。                       */
/*                                                                          */
/*  Phase 1: 読み取り専用 (list_dir, read_file, stat, read_stream)          */
/*                                                                          */
/*  参考: NP21/W generic/hostdrvnt.c, generic/hostdrvntdef.h               */
/*        SimK IFS解説 https://simk98.github.io/np21w/docs/makeifs.html    */
/* ======================================================================== */

#include "hostdrvfs.h"
#include "hostdrvfs_proto.h"
#include "io.h"
#include "kmalloc.h"
#include "kstring.h"
#include "kutf16.h"
#include "kprintf.h"

/* ===================================================================== */
/*  内部定数                                                              */
/* ===================================================================== */
#define HOSTDRV_DATA_BUF_SIZE  4096
#define HOSTDRV_NAME_BUF_WORDS  260   /* MAX_PATH */
#define HOSTDRV_MAX_DIR_ENTRIES 1000  /* 列挙上限 (無限ループ防止) */

/* エミュレータ未応答検出用の番兵値 */
#define NP2_STATUS_SENTINEL  0xDEADBEEFUL

/* 前方宣言 */
static int hostdrv_read(void *buf, u32 size, u64 offset);

/* ===================================================================== */
/*  コンテキスト構造体 (静的グローバル — ページ境界問題を回避)             */
/* ===================================================================== */

/* 通信バッファ群 — エミュレータが直接読み書きする */
static Np2InvokeInfo      g_invoke  __attribute__((aligned(4)));
static Np2IoStackLocation g_stack   __attribute__((aligned(4)));
static Np2FileObject      g_fobj    __attribute__((aligned(4)));
static Np2FsContext       g_fsctx   __attribute__((aligned(4)));
static Np2IoStatusBlock   g_iostatus __attribute__((aligned(4)));
static Np2IoSecurityContext g_secctx __attribute__((aligned(4)));
static u16  g_namebuf[HOSTDRV_NAME_BUF_WORDS] __attribute__((aligned(4)));
static u8   g_databuf[HOSTDRV_DATA_BUF_SIZE]  __attribute__((aligned(4)));

/* マウント状態 */
static int g_mounted = 0;

/* ===================================================================== */
/*  低レベルI/Oポート通信                                                  */
/* ===================================================================== */

/* ホストドライブ検出: エミュレータ対応なら1 */
int hostdrvfs_detect(void)
{
    int a, c;
    a = inp(HOSTDRV_IO_ADDR);
    c = inp(HOSTDRV_IO_CMD);
    return (a == HOSTDRV_DETECT_ADDR_VAL && c == HOSTDRV_DETECT_CMD_VAL);
}

/* ハイパーバイザーコール: invoke_info のアドレスを送信し処理実行 */
static void hostdrv_hypercall(void)
{
    u32 addr = (u32)&g_invoke;
    outp(HOSTDRV_IO_ADDR, (u8)(addr));
    outp(HOSTDRV_IO_ADDR, (u8)(addr >> 8));
    outp(HOSTDRV_IO_ADDR, (u8)(addr >> 16));
    outp(HOSTDRV_IO_ADDR, (u8)(addr >> 24));
    outp(HOSTDRV_IO_CMD, 'H');
    outp(HOSTDRV_IO_CMD, 'D');
    outp(HOSTDRV_IO_CMD, 'R');
    outp(HOSTDRV_IO_CMD, '9');
    outp(HOSTDRV_IO_CMD, '8');
    outp(HOSTDRV_IO_CMD, '0');
    outp(HOSTDRV_IO_CMD, '1');  /* 最後のバイトで同期実行 */
}

/* ===================================================================== */
/*  通信バッファ初期化ヘルパー                                            */
/* ===================================================================== */

/* invoke_info をクリアし共通フィールドを設定 */
static void invoke_clear(void)
{
    kmemset(&g_invoke, 0, sizeof(g_invoke));
    kmemset(&g_stack, 0, sizeof(g_stack));
    kmemset(&g_fobj, 0, sizeof(g_fobj));
    kmemset(&g_fsctx, 0, sizeof(g_fsctx));
    kmemset(&g_secctx, 0, sizeof(g_secctx));

    /* 番兵値: エミュレータが応答しなければこの値が残る */
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;

    /* INVOKE_INFO のポインタ設定 */
    g_invoke.stackAddr = (u32)&g_stack;

    g_invoke.statusAddr = (u32)&g_iostatus;
    g_invoke.outBufferAddr = (u32)g_databuf;
    g_invoke.inBufferAddr = (u32)g_databuf;
    g_invoke.version = 1;

    /* FILE_OBJECT の固定フィールド */
    g_fobj.FsContext = (u32)&g_fsctx;
    g_fobj.Flags = NP2_FO_SYNCHRONOUS_IO;

    /* IO_STACK_LOCATION.fileObject を設定 */
    g_stack.fileObject = (u32)&g_fobj;
}

/* パスをUTF-16LEに変換してg_namebufとg_fobjに設定 */
static void invoke_set_path(const char *path)
{
    int words;
    /* パスの先頭を \ に変換 (VFS相対パス → NT形式) */
    /* OS32の '/' を '\' に変換 */
    char ntpath[260];
    int i;
    kstrncpy(ntpath, path, sizeof(ntpath));
    for (i = 0; ntpath[i]; i++) {
        if (ntpath[i] == '/') ntpath[i] = '\\';
    }
    /* 空パスの場合はルート "\" */
    if (ntpath[0] == '\0') {
        ntpath[0] = '\\';
        ntpath[1] = '\0';
    }

    words = kutf8_to_utf16le(ntpath, g_namebuf, HOSTDRV_NAME_BUF_WORDS);
    /* UNICODE_STRING: Length はNULL終端を含まないバイト数 */
    g_fobj.FileName.Length = (u16)((words - 1) * 2);
    g_fobj.FileName.MaximumLength = (u16)(words * 2);
    g_fobj.FileName.Buffer = (u32)g_namebuf;
}

/* ===================================================================== */
/*  IRP操作ラッパー                                                       */
/* ===================================================================== */

/* IRP_MJ_CREATE: ファイル/ディレクトリを開く
 * disposition: NP2_FILE_OPEN 等
 * options_flags: NP2_FILE_DIRECTORY_FILE 等 (下位24bit)
 * 戻り値: 0=成功, <0=エラー
 */
static int hostdrv_create(const char *path, u32 disposition,
                          u32 options_flags, u32 desired_access)
{
    invoke_clear();
    invoke_set_path(path);

    g_stack.majorFunction = NP2_IRP_MJ_CREATE;
    /* options: 上位8bit = disposition, 下位24bit = options */
    g_stack.parameters.create.options =
        (disposition << 24) | (options_flags & 0x00FFFFFFUL);
    g_stack.parameters.create.shareAccess = 0x07; /* 全共有 */
    
    /* 
     * エミュレータ側は securityContext を IO_SECURITY_CONTEXT へのポインタとして
     * 解釈し、+8バイトオフセットから DesiredAccess を読み取る。
     */
    g_secctx.DesiredAccess = desired_access;
    g_stack.parameters.create.securityContext = (u32)&g_secctx;

    hostdrv_hypercall();

    /* エミュレータ未応答チェック */
    if (g_iostatus.Status == NP2_STATUS_SENTINEL) {
        return -1;
    }
    if (g_iostatus.Status != NP2_STATUS_SUCCESS) {
        kprintf(0xE1, "[HDRV] CREATE failed: %x path=%s\n", g_iostatus.Status, path);
        return -1;
    }
    /* 成功時、ファイルインデックスなどが g_fsctx 等に入っている */
    return 0;
}

/* IRP_MJ_CLOSE */
static void hostdrv_close(void)
{
    /* fileObjectは前回のcreateのものをそのまま使う */
    g_stack.majorFunction = NP2_IRP_MJ_CLOSE;
    g_stack.minorFunction = 0;
    g_stack.flags = 0;
    g_stack.parameters.others.argument1 = 0;
    g_stack.parameters.others.argument2 = 0;
    g_stack.parameters.others.argument3 = 0;
    g_stack.parameters.others.argument4 = 0;

    hostdrv_hypercall();
}

/* IRP_MJ_CLEANUP */
static void hostdrv_cleanup(void)
{
    g_stack.majorFunction = NP2_IRP_MJ_CLEANUP;
    g_stack.minorFunction = 0;
    g_stack.flags = 0;

    hostdrv_hypercall();
}

/* IRP_MJ_READ: ファイル読み込み
 * buf:     出力バッファ
 * size:    読み込みサイズ
 * offset:  ファイルオフセット
 * 戻り値: 読み込んだバイト数, <0=エラー
 *
 * CREATEで初期化した g_fobj / g_fsctx はそのまま保持し、
 * g_stack のみクリア+再設定する。
 * FSドライバの設計上、IRP_MJ_READ は CREATE で確保した
 * FileObject / FsContext を再利用すべきであり、
 * invoke_clear() で全構造体をゼロクリアしてはいけない。
 */
static int hostdrv_read(void *buf, u32 size, u64 offset)
{
    u32 chunk;
    u32 total = 0;

    while (total < size) {
        chunk = size - total;
        if (chunk > HOSTDRV_DATA_BUF_SIZE)
            chunk = HOSTDRV_DATA_BUF_SIZE;

        /* hostdrv_query_info と同じ方式: g_stack をクリアせず上書きのみ */
        g_stack.majorFunction = NP2_IRP_MJ_READ;
        g_stack.minorFunction = 0;
        g_stack.flags = 0;
        g_stack.parameters.read.length = chunk;
        g_stack.parameters.read.key = 0;
        g_stack.parameters.read.byteOffset = offset + total;

        g_invoke.outBufferAddr = (u32)g_databuf;

        g_iostatus.Status = NP2_STATUS_SENTINEL;
        g_iostatus.Information = 0;

        hostdrv_hypercall();

        if (g_iostatus.Status == NP2_STATUS_SENTINEL) {
            if (total > 0) break;
            return -1;
        }
        if (g_iostatus.Status == NP2_STATUS_END_OF_FILE) {
            break;
        }
        if (g_iostatus.Status != NP2_STATUS_SUCCESS) {
            if (total > 0) break;
            return -1;
        }
        if (g_iostatus.Information == 0) break;

        kmemcpy((u8 *)buf + total, g_databuf, g_iostatus.Information);
        total += g_iostatus.Information;

        if (g_iostatus.Information < chunk) break;
    }
    return (int)total;
}

/* IRP_MJ_QUERY_INFORMATION: ファイル情報取得
 * info_class: NP2_FileBasicInformation 等
 * 戻り値: 0=成功, <0=エラー (結果はg_databufに書き込まれる)
 */
static int hostdrv_query_info(u32 info_class, u32 buf_len)
{
    g_stack.majorFunction = NP2_IRP_MJ_QUERY_INFORMATION;
    g_stack.minorFunction = 0;
    g_stack.flags = 0;
    g_stack.parameters.queryFile.Length = buf_len;
    g_stack.parameters.queryFile.FileInformationClass = info_class;
    g_invoke.outBufferAddr = (u32)g_databuf;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;

    kmemset(g_databuf, 0, buf_len);

    hostdrv_hypercall();

    if (g_iostatus.Status == NP2_STATUS_SENTINEL) return -1;

    if (g_iostatus.Status != NP2_STATUS_SUCCESS &&
        g_iostatus.Status != NP2_STATUS_BUFFER_OVERFLOW) {
        return -1;
    }
    return 0;
}

/* IRP_MJ_DIRECTORY_CONTROL (IRP_MN_QUERY_DIRECTORY):
 * ディレクトリ列挙 1エントリ取得
 * first: 1=最初のエントリ, 0=次のエントリ
 * 戻り値: 0=成功(g_databufにエントリ), 1=終了, <0=エラー
 */
static int hostdrv_query_dir(int first)
{
    g_stack.majorFunction = NP2_IRP_MJ_DIRECTORY_CONTROL;
    g_stack.minorFunction = NP2_IRP_MN_QUERY_DIRECTORY;
    g_stack.flags = NP2_SL_RETURN_SINGLE_ENTRY;
    if (first) {
        g_stack.flags |= NP2_SL_RESTART_SCAN;
    }
    g_stack.parameters.queryDirectory.Length = HOSTDRV_DATA_BUF_SIZE;
    g_stack.parameters.queryDirectory.FileName = 0;
    g_stack.parameters.queryDirectory.FileInformationClass =
        NP2_FileBothDirectoryInformation;
    g_stack.parameters.queryDirectory.FileIndex = 0;
    g_invoke.outBufferAddr = (u32)g_databuf;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;

    kmemset(g_databuf, 0, HOSTDRV_DATA_BUF_SIZE);

    hostdrv_hypercall();

    /* エミュレータ未応答 */
    if (g_iostatus.Status == NP2_STATUS_SENTINEL) {
        return -1;
    }
    if (g_iostatus.Status == NP2_STATUS_NO_MORE_FILES ||
        g_iostatus.Status == NP2_STATUS_OBJECT_NAME_NOT_FOUND) {
        return 1;  /* 列挙終了 */
    }
    if (g_iostatus.Status != NP2_STATUS_SUCCESS) {
        return -1;
    }
    return 0;
}

/* ===================================================================== */
/*  VfsOps 実装                                                           */
/* ===================================================================== */

/* mount: ホストドライブ検出・マウント */
static void *hdrv_mount(int dev_id)
{
    (void)dev_id;

    if (!hostdrvfs_detect()) {
        return (void *)0;
    }
    g_mounted = 1;
    return (void *)1;  /* コンテキストは静的グローバル */
}

/* umount */
static void hdrv_umount(void *ctx)
{
    (void)ctx;
    g_mounted = 0;
}

/* is_mounted */
static int hdrv_is_mounted(void *ctx)
{
    (void)ctx;
    return g_mounted;
}

/* list_dir: ディレクトリ列挙 */
static int hdrv_list_dir(void *ctx, const char *path,
                         vfs_dir_cb cb, void *user_ctx)
{
    int rc;
    int first = 1;
    (void)ctx;

    /* ディレクトリを開く */
    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) return VFS_ERR_NOTFOUND;

    /* エントリを1つずつ列挙 (上限付き) */
    {
        int count = 0;
        for (;;) {
            Np2FileBothDirInfo *info;
            VfsDirEntry entry;
            char namebuf[260];
            int namelen;

            if (count++ >= HOSTDRV_MAX_DIR_ENTRIES) break;

            rc = hostdrv_query_dir(first);
            first = 0;

            if (rc > 0) break;       /* 列挙終了 */
            if (rc < 0) break;       /* エラー */

        info = (Np2FileBothDirInfo *)g_databuf;

        /* ファイル名をUTF-8に変換 (packed構造体のアドレス取得回避) */
        {
            const u16 *fname_ptr = info->FileName;
            namelen = kutf16le_to_utf8(fname_ptr,
                                       info->FileNameLength,
                                       namebuf, sizeof(namebuf));
        }
        if (namelen <= 0) continue;

        /* "." と ".." はスキップ */
        if (namebuf[0] == '.' &&
            (namebuf[1] == '\0' ||
             (namebuf[1] == '.' && namebuf[2] == '\0'))) {
            continue;
        }

        /* VfsDirEntryに変換 */
        kstrncpy(entry.name, namebuf, VFS_MAX_PATH);
        entry.size = (u32)info->EndOfFile;  /* 下位32bitのみ */
        entry.type = (info->FileAttributes & NP2_FILE_ATTRIBUTE_DIRECTORY)
                     ? VFS_TYPE_DIR : VFS_TYPE_FILE;

        cb(&entry, user_ctx);
    }

    } /* end for loop block */

    /* ディレクトリを閉じる */
    hostdrv_cleanup();
    hostdrv_close();

    return VFS_OK;
}

/* read_file: 一括ファイル読み込み */
static int hdrv_read_file(void *ctx, const char *path,
                          void *buf, u32 max_size)
{
    int rc;
    int bytes;
    (void)ctx;

    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_NON_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) return VFS_ERR_NOTFOUND;

    bytes = hostdrv_read(buf, max_size, 0);

    hostdrv_cleanup();
    hostdrv_close();

    return bytes;
}

/* get_file_size */
static int hdrv_get_file_size(void *ctx, const char *path, u32 *size)
{
    int rc;
    Np2FileStandardInfo *info;
    (void)ctx;

    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) {
        kprintf(0xE1, "[HDRV] get_size: CREATE failed path=%s\n", path);
        return VFS_ERR_NOTFOUND;
    }

    rc = hostdrv_query_info(NP2_FileStandardInformation,
                            sizeof(Np2FileStandardInfo));

    if (rc == 0) {
        info = (Np2FileStandardInfo *)g_databuf;
        *size = (u32)info->EndOfFile;
    }

    hostdrv_cleanup();
    hostdrv_close();

    return (rc == 0) ? VFS_OK : VFS_ERR_IO;
}

/* read_stream: シーク対応読み込み */
static int hdrv_read_stream(void *ctx, const char *path,
                            void *buf, u32 size, u32 offset)
{
    int rc;
    int bytes;
    (void)ctx;

    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_NON_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) {
        return VFS_ERR_NOTFOUND;
    }

    bytes = hostdrv_read(buf, size, (u64)offset);

    hostdrv_cleanup();
    hostdrv_close();

    return bytes;
}

/* stat: ファイル情報取得 */
static int hdrv_stat(void *ctx, const char *path, OS32_Stat *buf)
{
    int rc;
    Np2FileBasicInfo *basic;
    Np2FileStandardInfo *std_info;
    (void)ctx;

    kmemset(buf, 0, sizeof(OS32_Stat));

    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) return VFS_ERR_NOTFOUND;

    /* FileBasicInformation 取得 */
    rc = hostdrv_query_info(NP2_FileBasicInformation,
                            sizeof(Np2FileBasicInfo));
    if (rc == 0) {
        basic = (Np2FileBasicInfo *)g_databuf;
        if (basic->FileAttributes & NP2_FILE_ATTRIBUTE_DIRECTORY) {
            buf->st_mode = OS_S_IFDIR | 0755;
        } else {
            buf->st_mode = OS_S_IFREG | 0644;
        }
    }

    /* FileStandardInformation 取得 */
    rc = hostdrv_query_info(NP2_FileStandardInformation,
                            sizeof(Np2FileStandardInfo));
    if (rc == 0) {
        std_info = (Np2FileStandardInfo *)g_databuf;
        buf->st_size = (u32)std_info->EndOfFile;
    }

    hostdrv_cleanup();
    hostdrv_close();

    return VFS_OK;
}

/* ===================================================================== */
/*  未実装のスタブ (Phase 2で実装)                                        */
/* ===================================================================== */
static int hdrv_write_file(void *ctx, const char *path,
                           const void *data, u32 size)
{ (void)ctx; (void)path; (void)data; (void)size; return VFS_ERR_IO; }

static int hdrv_mkdir(void *ctx, const char *path)
{ (void)ctx; (void)path; return VFS_ERR_IO; }

static int hdrv_rmdir(void *ctx, const char *path)
{ (void)ctx; (void)path; return VFS_ERR_IO; }

static int hdrv_unlink(void *ctx, const char *path)
{ (void)ctx; (void)path; return VFS_ERR_IO; }

static int hdrv_rename(void *ctx, const char *old, const char *new_path)
{ (void)ctx; (void)old; (void)new_path; return VFS_ERR_IO; }

static int hdrv_write_stream(void *ctx, const char *path,
                             const void *buf, u32 size, u32 offset)
{ (void)ctx; (void)path; (void)buf; (void)size; (void)offset;
  return VFS_ERR_IO; }

static int hdrv_sync(void *ctx)
{ (void)ctx; return VFS_OK; }

static u32 hdrv_total_blocks(void *ctx)
{ (void)ctx; return 0; }

static u32 hdrv_free_blocks(void *ctx)
{ (void)ctx; return 0; }

static u32 hdrv_block_size(void *ctx)
{ (void)ctx; return 512; }

/* ===================================================================== */
/*  VfsOps テーブル                                                       */
/* ===================================================================== */
static VfsOps g_hostdrvfs_ops = {
    "hostdrv",
    hdrv_mount,
    hdrv_umount,
    hdrv_is_mounted,
    hdrv_list_dir,
    hdrv_mkdir,
    hdrv_rmdir,
    hdrv_read_file,
    hdrv_write_file,
    hdrv_unlink,
    hdrv_rename,
    hdrv_get_file_size,
    hdrv_read_stream,
    hdrv_write_stream,
    hdrv_sync,
    hdrv_total_blocks,
    hdrv_free_blocks,
    hdrv_block_size,
    hdrv_stat
};

VfsOps *hostdrvfs_get_ops(void)
{
    return &g_hostdrvfs_ops;
}

void hostdrvfs_init(void)
{
    vfs_register_fs(&g_hostdrvfs_ops);
}
