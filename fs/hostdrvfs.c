/* ======================================================================== */
/*  HOSTDRVFS.C — NP21/W HostDrv(NT) VFSドライバ v2                       */
/*                                                                          */
/*  v2改修: session_begin() / setup_xxx() 方式に全面書き換え。              */
/*  NTゲストドライバ (hostdrv.c) の動作を忠実に模倣し、                     */
/*  invoke_clear() によるFileObject/FsContext破壊を解消。                   */
/*                                                                          */
/*  原則:                                                                   */
/*    - session_begin() は CREATE前に1回だけ呼ぶ (全バッファ初期化)         */
/*    - 後続IRPでは g_stack のみ全クリア+再設定                             */
/*    - g_fobj, g_fsctx, g_invoke は CREATE以降触らない                     */
/*    - g_stack.fileObject は毎回再設定                                     */
/*                                                                          */
/*  Phase 1: 読み取り専用 (list_dir, read_file, stat, read_stream)          */
/*                                                                          */
/*  参考: NP21/W generic/hostdrvnt.c, generic/hostdrvntdef.h               */
/*        np2tool/hostdrvnt/hostdrv.c (NTゲストドライバ)                    */
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

/* ===================================================================== */
/*  コンテキスト構造体 (静的グローバル — ページ境界問題を回避)             */
/* ===================================================================== */

/* 通信バッファ群 — エミュレータが直接読み書きするため volatile 必須 */
static volatile Np2InvokeInfo      g_invoke  __attribute__((aligned(4)));
static volatile Np2IoStackLocation g_stack   __attribute__((aligned(4)));
static volatile Np2FileObject      g_fobj    __attribute__((aligned(4)));
static volatile Np2FsContext       g_fsctx   __attribute__((aligned(4)));
static volatile Np2IoStatusBlock   g_iostatus __attribute__((aligned(4)));
static volatile Np2IoSecurityContext g_secctx __attribute__((aligned(4)));
static volatile u16  g_namebuf[HOSTDRV_NAME_BUF_WORDS] __attribute__((aligned(4)));
static volatile u8   g_databuf[HOSTDRV_DATA_BUF_SIZE]  __attribute__((aligned(4)));

/* SectionObjectPointers ダミー (12バイト: DataSectionObject, SharedCacheMap, ImageSectionObject) */
static u32 g_sop[3] __attribute__((aligned(4)));

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
    if (a == HOSTDRV_DETECT_ADDR_VAL && c == HOSTDRV_DETECT_CMD_VAL) {
        /* エミュレータ側の状態をリセット (0x00000000 + HDR9801) */
        outp(HOSTDRV_IO_ADDR, 0);
        outp(HOSTDRV_IO_ADDR, 0);
        outp(HOSTDRV_IO_ADDR, 0);
        outp(HOSTDRV_IO_ADDR, 0);
        outp(HOSTDRV_IO_CMD, 'H');
        outp(HOSTDRV_IO_CMD, 'D');
        outp(HOSTDRV_IO_CMD, 'R');
        outp(HOSTDRV_IO_CMD, '9');
        outp(HOSTDRV_IO_CMD, '8');
        outp(HOSTDRV_IO_CMD, '0');
        outp(HOSTDRV_IO_CMD, '1');
        return 1;
    }
    return 0;
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
/*  v2: セッション管理 (invoke_clear() の代替)                             */
/* ===================================================================== */

/* セッション開始: CREATE前に1回だけ呼ぶ。全バッファを初期化する。
 *
 * NTドライバとの差異分析に基づき、以下を正しく設定:
 *   - deviceFlags = 0x30 (DO_BUFFERED_IO | FILE_DEVICE_IS_MOUNTED)
 *   - sectionObjectPointerAddr = ダミーSOPバッファ
 *   - g_fobj.SectionObjectPointer = ダミーSOPバッファ
 *   - g_fobj.Flags = NP2_FO_SYNCHRONOUS_IO
 */
static void session_begin(void)
{
    kmemset(&g_invoke, 0, sizeof(g_invoke));
    kmemset(&g_stack, 0, sizeof(g_stack));
    kmemset(&g_fobj, 0, sizeof(g_fobj));
    kmemset(&g_fsctx, 0, sizeof(g_fsctx));
    kmemset(&g_secctx, 0, sizeof(g_secctx));
    kmemset(&g_iostatus, 0, sizeof(g_iostatus));
    kmemset(g_sop, 0, sizeof(g_sop));

    /* InvokeInfo 固定フィールド */
    g_invoke.stackAddr = (u32)&g_stack;
    g_invoke.statusAddr = (u32)&g_iostatus;
    g_invoke.inBufferAddr = (u32)g_databuf;
    g_invoke.outBufferAddr = (u32)g_databuf;
    g_invoke.sectionObjectPointerAddr = (u32)g_sop;
    g_invoke.deviceFlags = 0x30; /* DO_BUFFERED_IO | FILE_DEVICE_IS_MOUNTED */
    g_invoke.version = 1;

    /* FILE_OBJECT 固定フィールド */
    g_fobj.FsContext = (u32)&g_fsctx;
    g_fobj.SectionObjectPointer = (u32)g_sop;
    g_fobj.Flags = NP2_FO_SYNCHRONOUS_IO;

    /* IO_STACK_LOCATION 固定フィールド */
    g_stack.fileObject = (u32)&g_fobj;
}

/* パスをUTF-16LEに変換してg_namebufとg_fobjに設定 */
static void session_set_path(const char *path)
{
    int words;
    char ntpath[260];
    int i;

    kstrncpy(ntpath, path, sizeof(ntpath));
    for (i = 0; ntpath[i]; i++) {
        if (ntpath[i] == '/') ntpath[i] = '\\';
    }
    /* 空パスの場合はルート "\\" */
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
/*  v2: IRP別セットアップ関数                                              */
/*  各関数は g_stack を全クリアして必要フィールドのみ再設定する。           */
/*  g_fobj / g_fsctx / g_invoke には一切触れない。                         */
/* ===================================================================== */

/* g_stack を全クリアして CREATE 用に設定 */
static void setup_create(const char *path, u32 disposition,
                         u32 options_flags, u32 desired_access)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_CREATE;
    g_stack.fileObject = (u32)&g_fobj;  /* 再設定必須 */
    /* options: 上位8bit = disposition, 下位24bit = options */
    g_stack.parameters.create.options =
        (disposition << 24) | (options_flags & 0x00FFFFFFUL);
    g_stack.parameters.create.shareAccess = 0x07; /* 全共有 */

    /* SecurityContext: エミュレータは +8 から DesiredAccess を読む */
    kmemset(&g_secctx, 0, sizeof(g_secctx));
    g_secctx.DesiredAccess = desired_access;
    g_stack.parameters.create.securityContext = (u32)&g_secctx;

    /* パス設定 */
    session_set_path(path);

    /* IOステータス: 番兵値 */
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして READ 用に再設定 */
static void setup_read(u32 length, u64 offset)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_READ;
    g_stack.fileObject = (u32)&g_fobj;  /* ★ 再設定必須 */
    g_stack.parameters.read.length = length;
    g_stack.parameters.read.byteOffset = offset;

    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして QUERY_INFORMATION 用に再設定 */
static void setup_query_info(u32 info_class, u32 buf_len)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_QUERY_INFORMATION;
    g_stack.fileObject = (u32)&g_fobj;
    g_stack.parameters.queryFile.Length = buf_len;
    g_stack.parameters.queryFile.FileInformationClass = info_class;

    g_invoke.outBufferAddr = (u32)g_databuf;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして DIRECTORY_CONTROL 用に再設定 */
static void setup_query_dir(int first)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_DIRECTORY_CONTROL;
    g_stack.minorFunction = NP2_IRP_MN_QUERY_DIRECTORY;
    g_stack.flags = NP2_SL_RETURN_SINGLE_ENTRY;
    if (first) {
        g_stack.flags |= NP2_SL_RESTART_SCAN;
    }
    g_stack.fileObject = (u32)&g_fobj;
    g_stack.parameters.queryDirectory.Length = HOSTDRV_DATA_BUF_SIZE;
    g_stack.parameters.queryDirectory.FileName = 0;
    g_stack.parameters.queryDirectory.FileInformationClass =
        NP2_FileBothDirectoryInformation;
    g_stack.parameters.queryDirectory.FileIndex = 0;

    g_invoke.outBufferAddr = (u32)g_databuf;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして CLEANUP 用に再設定 */
static void setup_cleanup(void)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_CLEANUP;
    g_stack.fileObject = (u32)&g_fobj;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* g_stack を全クリアして CLOSE 用に再設定 */
static void setup_close(void)
{
    kmemset(&g_stack, 0, sizeof(g_stack));
    g_stack.majorFunction = NP2_IRP_MJ_CLOSE;
    g_stack.fileObject = (u32)&g_fobj;
    g_iostatus.Status = NP2_STATUS_SENTINEL;
    g_iostatus.Information = 0;
}

/* ===================================================================== */
/*  IRP操作ラッパー                                                       */
/* ===================================================================== */

/* IRP_MJ_CREATE: ファイル/ディレクトリを開く
 * 戻り値: 0=成功, <0=エラー
 *
 * 注意: session_begin() を呼んだ後に使用すること。
 */
static int hostdrv_create(const char *path, u32 disposition,
                          u32 options_flags, u32 desired_access)
{
    setup_create(path, disposition, options_flags, desired_access);
    hostdrv_hypercall();

    if (g_iostatus.Status == NP2_STATUS_SENTINEL) {
        return -1;
    }
    if (g_iostatus.Status != NP2_STATUS_SUCCESS) {
        return -1;
    }
    return 0;
}

/* IRP_MJ_READ: ファイル読み込み (チャンク分割)
 * 戻り値: 読み込んだバイト数, <0=エラー
 *
 * g_fobj / g_fsctx は session_begin+CREATE で確立済み。
 * g_stack のみ全クリア+再設定する (v2方式)。
 */
static int hostdrv_read(void *buf, u32 size, u64 offset)
{
    u32 chunk;
    u32 total = 0;

    while (total < size) {
        chunk = size - total;
        if (chunk > HOSTDRV_DATA_BUF_SIZE)
            chunk = HOSTDRV_DATA_BUF_SIZE;

        setup_read(chunk, offset + total);
        g_invoke.outBufferAddr = (u32)g_databuf;

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

/* IRP_MJ_QUERY_INFORMATION
 * 戻り値: 0=成功, <0=エラー (結果はg_databufに格納)
 */
static int hostdrv_query_info(u32 info_class, u32 buf_len)
{
    setup_query_info(info_class, buf_len);
    kmemset(g_databuf, 0, buf_len);

    hostdrv_hypercall();

    if (g_iostatus.Status == NP2_STATUS_SENTINEL) return -1;
    if (g_iostatus.Status != NP2_STATUS_SUCCESS &&
        g_iostatus.Status != NP2_STATUS_BUFFER_OVERFLOW) {
        return -1;
    }
    return 0;
}

/* IRP_MJ_DIRECTORY_CONTROL (IRP_MN_QUERY_DIRECTORY)
 * 戻り値: 0=成功, 1=終了, <0=エラー
 */
static int hostdrv_query_dir(int first)
{
    setup_query_dir(first);
    kmemset(g_databuf, 0, HOSTDRV_DATA_BUF_SIZE);

    hostdrv_hypercall();

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

/* IRP_MJ_CLEANUP + IRP_MJ_CLOSE のペア */
static void hostdrv_cleanup_close(void)
{
    setup_cleanup();
    hostdrv_hypercall();
    setup_close();
    hostdrv_hypercall();
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
    kprintf(0x0A, "[HDRV] HostDrv(NT) mounted on /host\n");
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

    /* セッション開始 + ディレクトリを開く */
    session_begin();
    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) {
        return VFS_ERR_NOTFOUND;
    }

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

            /* ファイル名をUTF-8に変換 */
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
    }

    /* ディレクトリを閉じる */
    hostdrv_cleanup_close();

    return VFS_OK;
}

/* read_file: 一括ファイル読み込み */
static int hdrv_read_file(void *ctx, const char *path,
                          void *buf, u32 max_size)
{
    int rc;
    int bytes;
    (void)ctx;

    session_begin();
    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_NON_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) return VFS_ERR_NOTFOUND;

    bytes = hostdrv_read(buf, max_size, 0);

    hostdrv_cleanup_close();

    return bytes;
}

/* get_file_size */
static int hdrv_get_file_size(void *ctx, const char *path, u32 *size)
{
    int rc;
    Np2FileStandardInfo *info;
    (void)ctx;

    session_begin();
    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) {
        return VFS_ERR_NOTFOUND;
    }

    rc = hostdrv_query_info(NP2_FileStandardInformation,
                            sizeof(Np2FileStandardInfo));

    if (rc == 0) {
        info = (Np2FileStandardInfo *)g_databuf;
        *size = (u32)info->EndOfFile;
    }

    hostdrv_cleanup_close();

    return (rc == 0) ? VFS_OK : VFS_ERR_IO;
}

/* read_stream: シーク対応読み込み */
static int hdrv_read_stream(void *ctx, const char *path,
                            void *buf, u32 size, u32 offset)
{
    int rc;
    int bytes;
    (void)ctx;

    session_begin();
    rc = hostdrv_create(path, NP2_FILE_OPEN,
                        NP2_FILE_NON_DIRECTORY_FILE |
                        NP2_FILE_SYNCHRONOUS_IO_NONALERT,
                        NP2_FILE_READ_DATA);
    if (rc < 0) {
        return VFS_ERR_NOTFOUND;
    }

    bytes = hostdrv_read(buf, size, (u64)offset);

    hostdrv_cleanup_close();

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

    session_begin();
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

    hostdrv_cleanup_close();

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
