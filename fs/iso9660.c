/* ======================================================================== */
/*  ISO9660.C — ISO 9660 読み取り専用ファイルシステム VFSドライバ            */
/*                                                                          */
/*  CD-ROM上のISO 9660 Level 1 ファイルシステムをVFS経由で読み出す。         */
/*  書き込み操作は全て VFS_ERR_IO を返す (読み取り専用)。                    */
/*                                                                          */
/*  ファイル名はcase-insensitive比較 (ISO 9660は大文字格納)。               */
/*  バージョン番号 ";1" は表示・検索時に除去する。                           */
/* ======================================================================== */

#include "iso9660.h"
#include "dev.h"
#include "atapi.h"
#include "kmalloc.h"
#include "lib/kstring.h"

/* ======================================================================== */
/*  内部ヘルパー                                                             */
/* ======================================================================== */

/* セクタ読み出し (2048バイト/セクタ) */
static int iso_read_sector(Iso9660Ctx *ctx, u32 lba, void *buf)
{
    Device *dev;
    (void)ctx;
    dev = dev_find("cd0");
    if (!dev || !dev->blk_read) return VFS_ERR_IO;
    return dev->blk_read(dev, (int)lba, 1, buf);
}

/* 大文字変換 */
static char iso_toupper(char c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

/* case-insensitive文字列比較 */
static int iso_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = iso_toupper(*a);
        char cb = iso_toupper(*b);
        if (ca != cb) return (int)ca - (int)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* PVD/ディレクトリレコードからリトルエンディアンu32読み出し (both-endian) */
static u32 iso_read_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* PVD/ディレクトリレコードからリトルエンディアンu16読み出し */
static u16 iso_read_le16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

/* ファイル名からバージョン番号 ";1" を除去し、末尾の "." も除去
 *   入力: "README.TXT;1" → 出力: "README.TXT"
 *   入力: "MYDIR"        → 出力: "MYDIR"      (ディレクトリ)
 */
static void iso_clean_name(const u8 *raw, int raw_len, char *out, int out_max)
{
    int i;
    int len = raw_len;

    /* バージョン番号 ";N" を除去 */
    for (i = 0; i < len; i++) {
        if (raw[i] == ';') {
            len = i;
            break;
        }
    }
    /* 末尾の "." を除去 (ディレクトリ名) */
    if (len > 0 && raw[len - 1] == '.') len--;

    if (len >= out_max) len = out_max - 1;
    for (i = 0; i < len; i++) out[i] = (char)raw[i];
    out[len] = '\0';
}

/* ======================================================================== */
/*  ディレクトリ走査コア                                                      */
/*                                                                          */
/*  パスを "/" で分解し、各コンポーネントをディレクトリ内で検索。             */
/*  最終コンポーネントのエクステント位置とサイズを返す。                     */
/* ======================================================================== */

/* ディレクトリエクステント内で名前を検索
 *   dir_lba:  ディレクトリのLBA
 *   dir_size: ディレクトリのサイズ (バイト)
 *   name:     検索するファイル/ディレクトリ名
 *   out_lba:  見つかったエントリのエクステントLBA
 *   out_size: 見つかったエントリのデータサイズ
 *   out_flags: ファイルフラグ
 * 戻り値: 0=発見, VFS_ERR_NOTFOUND=未発見 */
static int iso_find_in_dir(Iso9660Ctx *ctx, u32 dir_lba, u32 dir_size,
                           const char *name,
                           u32 *out_lba, u32 *out_size, u8 *out_flags)
{
    u8 sector[ISO_SECTOR_SIZE];
    u32 offset = 0;

    while (offset < dir_size) {
        u32 sect_lba = dir_lba + (offset / ISO_SECTOR_SIZE);
        u32 sect_off = offset % ISO_SECTOR_SIZE;
        u8 *rec;
        u8 rec_len;
        u8 name_len;
        char entry_name[ISO_MAX_NAME];

        if (sect_off == 0) {
            if (iso_read_sector(ctx, sect_lba, sector) != 0)
                return VFS_ERR_IO;
        }

        rec = sector + sect_off;
        rec_len = rec[0];

        /* レコード長0 = セクタ末尾のパディング → 次のセクタへ */
        if (rec_len == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        /* レコードがセクタ境界をまたぐ場合は次のセクタで再読み込み */
        if (sect_off + rec_len > ISO_SECTOR_SIZE) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        name_len = rec[32];

        /* "." (0x00) と ".." (0x01) はスキップ */
        if (name_len == 1 && (rec[33] == 0x00 || rec[33] == 0x01)) {
            offset += rec_len;
            continue;
        }

        iso_clean_name(rec + 33, name_len, entry_name, ISO_MAX_NAME);

        if (iso_strcasecmp(entry_name, name) == 0) {
            *out_lba   = iso_read_le32(rec + 2);
            *out_size  = iso_read_le32(rec + 10);
            *out_flags = rec[25];
            return 0;
        }

        offset += rec_len;
    }

    return VFS_ERR_NOTFOUND;
}

/* パスを辿ってエントリを解決
 *   path: "/" 区切りのパス (例: "DIR1/SUBDIR/FILE.TXT")
 *   out_lba, out_size, out_flags: 最終エントリの情報
 * 戻り値: 0=成功 */
static int iso_resolve_path(Iso9660Ctx *ctx, const char *path,
                            u32 *out_lba, u32 *out_size, u8 *out_flags)
{
    u32 cur_lba  = ctx->root_lba;
    u32 cur_size = ctx->root_size;
    u8 cur_flags = ISO_FLAG_DIRECTORY;
    const char *p = path;
    char component[ISO_MAX_NAME];
    int ci;

    /* 先頭の "/" をスキップ */
    while (*p == '/') p++;

    /* 空パス = ルートディレクトリ */
    if (*p == '\0') {
        *out_lba   = cur_lba;
        *out_size  = cur_size;
        *out_flags = cur_flags;
        return 0;
    }

    while (*p) {
        int ret;

        /* 次のコンポーネント抽出 */
        ci = 0;
        while (*p && *p != '/' && ci < ISO_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';
        while (*p == '/') p++;

        /* 現在のディレクトリがディレクトリでなければエラー */
        if (!(cur_flags & ISO_FLAG_DIRECTORY))
            return VFS_ERR_NOTDIR;

        ret = iso_find_in_dir(ctx, cur_lba, cur_size, component,
                              &cur_lba, &cur_size, &cur_flags);
        if (ret != 0) return ret;
    }

    *out_lba   = cur_lba;
    *out_size  = cur_size;
    *out_flags = cur_flags;
    return 0;
}

/* ======================================================================== */
/*  VFS操作関数                                                              */
/* ======================================================================== */

/* --- mount --- */
static void *iso9660_mount(int dev_id)
{
    u8 pvd[ISO_SECTOR_SIZE];
    Iso9660Ctx *ctx;
    Device *dev;
    char devname[8];

    /* デバイス名を構築: "cd0", "cd1", ... */
    devname[0] = 'c';
    devname[1] = 'd';
    devname[2] = '0' + (char)dev_id;
    devname[3] = '\0';

    dev = dev_find(devname);
    if (!dev) return (void *)0;

    /* PVD読み出し (LBA 16) */
    if (dev->blk_read(dev, ISO_PVD_LBA, 1, pvd) != 0)
        return (void *)0;

    /* マジック確認: pvd[1..5] == "CD001" */
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D'
                     || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
        return (void *)0;

    ctx = (Iso9660Ctx *)kmalloc(sizeof(Iso9660Ctx));
    if (!ctx) return (void *)0;

    ctx->dev_id = dev_id;

    /* ボリュームサイズ (pvd[80..83] = LE u32) */
    ctx->volume_size = iso_read_le32(pvd + 80);

    /* 論理ブロックサイズ (pvd[128..129] = LE u16) */
    ctx->block_size = iso_read_le16(pvd + 128);
    if (ctx->block_size == 0) ctx->block_size = ISO_SECTOR_SIZE;

    /* ルートディレクトリレコード (pvd[156..189], 34バイト) */
    {
        u8 *root_rec = pvd + 156;
        ctx->root_lba  = iso_read_le32(root_rec + 2);
        ctx->root_size = iso_read_le32(root_rec + 10);
    }

    return (void *)ctx;
}

/* --- umount --- */
static void iso9660_umount(void *ctx_raw)
{
    if (ctx_raw) kfree(ctx_raw);
}

/* --- is_mounted --- */
static int iso9660_is_mounted(void *ctx_raw)
{
    return (ctx_raw != (void *)0) ? 1 : 0;
}

/* --- list_dir --- */
static int iso9660_list_dir(void *ctx_raw, const char *path,
                            vfs_dir_cb cb, void *user_ctx)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)ctx_raw;
    u32 dir_lba, dir_size;
    u8 dir_flags;
    u8 sector[ISO_SECTOR_SIZE];
    u32 offset;
    int ret;

    if (!ctx) return VFS_ERR_NOMOUNT;

    ret = iso_resolve_path(ctx, path, &dir_lba, &dir_size, &dir_flags);
    if (ret != 0) return ret;
    if (!(dir_flags & ISO_FLAG_DIRECTORY)) return VFS_ERR_NOTDIR;

    offset = 0;
    while (offset < dir_size) {
        u32 sect_lba = dir_lba + (offset / ISO_SECTOR_SIZE);
        u32 sect_off = offset % ISO_SECTOR_SIZE;
        u8 *rec;
        u8 rec_len, name_len;

        if (sect_off == 0) {
            if (iso_read_sector(ctx, sect_lba, sector) != 0)
                return VFS_ERR_IO;
        }

        rec = sector + sect_off;
        rec_len = rec[0];

        if (rec_len == 0) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }
        if (sect_off + rec_len > ISO_SECTOR_SIZE) {
            offset = ((offset / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
            continue;
        }

        name_len = rec[32];

        /* "." / ".." はスキップ */
        if (!(name_len == 1 && (rec[33] == 0x00 || rec[33] == 0x01))) {
            VfsDirEntry ent;
            iso_clean_name(rec + 33, name_len, ent.name, VFS_MAX_PATH);
            ent.size = iso_read_le32(rec + 10);
            ent.type = (rec[25] & ISO_FLAG_DIRECTORY)
                     ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            cb(&ent, user_ctx);
        }

        offset += rec_len;
    }

    return VFS_OK;
}

/* --- read_file --- */
static int iso9660_read_file(void *ctx_raw, const char *path,
                             void *buf, u32 max_size)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)ctx_raw;
    u32 file_lba, file_size;
    u8 flags;
    u32 to_read, sectors, remain;
    u8 *p = (u8 *)buf;
    u32 i;
    int ret;

    if (!ctx) return VFS_ERR_NOMOUNT;

    ret = iso_resolve_path(ctx, path, &file_lba, &file_size, &flags);
    if (ret != 0) return ret;
    if (flags & ISO_FLAG_DIRECTORY) return VFS_ERR_ISDIR;

    to_read = (file_size < max_size) ? file_size : max_size;
    sectors = (to_read + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    for (i = 0; i < sectors; i++) {
        u8 sector[ISO_SECTOR_SIZE];
        u32 chunk;

        if (iso_read_sector(ctx, file_lba + i, sector) != 0)
            return VFS_ERR_IO;

        remain = to_read - (i * ISO_SECTOR_SIZE);
        chunk = (remain < ISO_SECTOR_SIZE) ? remain : ISO_SECTOR_SIZE;
        {
            u32 j;
            for (j = 0; j < chunk; j++)
                p[i * ISO_SECTOR_SIZE + j] = sector[j];
        }
    }

    return (int)to_read;
}

/* --- read_stream (オフセット付き部分読み出し) --- */
static int iso9660_read_stream(void *ctx_raw, const char *path,
                               void *buf, u32 size, u32 offset)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)ctx_raw;
    u32 file_lba, file_size;
    u8 flags;
    u32 to_read, done;
    u8 *p = (u8 *)buf;
    int ret;

    if (!ctx) return VFS_ERR_NOMOUNT;

    ret = iso_resolve_path(ctx, path, &file_lba, &file_size, &flags);
    if (ret != 0) return ret;
    if (flags & ISO_FLAG_DIRECTORY) return VFS_ERR_ISDIR;
    if (offset >= file_size) return 0;

    to_read = file_size - offset;
    if (to_read > size) to_read = size;

    done = 0;
    while (done < to_read) {
        u8 sector[ISO_SECTOR_SIZE];
        u32 cur_off = offset + done;
        u32 sect_idx = cur_off / ISO_SECTOR_SIZE;
        u32 sect_off = cur_off % ISO_SECTOR_SIZE;
        u32 chunk = ISO_SECTOR_SIZE - sect_off;

        if (chunk > to_read - done) chunk = to_read - done;

        if (iso_read_sector(ctx, file_lba + sect_idx, sector) != 0)
            return VFS_ERR_IO;

        {
            u32 j;
            for (j = 0; j < chunk; j++) p[done + j] = sector[sect_off + j];
        }
        done += chunk;
    }

    return (int)done;
}

/* --- get_file_size --- */
static int iso9660_get_file_size(void *ctx_raw, const char *path, u32 *size)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)ctx_raw;
    u32 lba, fsize;
    u8 flags;
    int ret;

    if (!ctx) return VFS_ERR_NOMOUNT;
    ret = iso_resolve_path(ctx, path, &lba, &fsize, &flags);
    if (ret != 0) return ret;
    if (size) *size = fsize;
    return VFS_OK;
}

/* --- stat --- */
static int iso9660_stat(void *ctx_raw, const char *path, OS32_Stat *st)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)ctx_raw;
    u32 lba, fsize;
    u8 flags;
    int ret;

    if (!ctx) return VFS_ERR_NOMOUNT;
    ret = iso_resolve_path(ctx, path, &lba, &fsize, &flags);
    if (ret != 0) return ret;

    if (st) {
        st->st_size = fsize;
        st->st_mode = (flags & ISO_FLAG_DIRECTORY)
                    ? (OS_S_IFDIR | OS_S_IRWXU)
                    : (OS_S_IFREG | OS_S_IRUSR);
        st->st_mtime = 0;
        st->st_atime = 0;
        st->st_ctime = 0;
        st->st_dev   = 0;
        st->st_ino   = lba;
        st->st_nlink = 1;
        st->st_uid   = 0;
        st->st_gid   = 0;
    }
    return VFS_OK;
}

/* --- 読み取り専用: 書き込み系は全て拒否 --- */
static int iso9660_write_file(void *c, const char *p, const void *d, u32 s)
{ (void)c; (void)p; (void)d; (void)s; return VFS_ERR_IO; }

static int iso9660_unlink(void *c, const char *p)
{ (void)c; (void)p; return VFS_ERR_IO; }

static int iso9660_rename(void *c, const char *o, const char *n)
{ (void)c; (void)o; (void)n; return VFS_ERR_IO; }

static int iso9660_mkdir(void *c, const char *p)
{ (void)c; (void)p; return VFS_ERR_IO; }

static int iso9660_rmdir(void *c, const char *p)
{ (void)c; (void)p; return VFS_ERR_IO; }

static int iso9660_write_stream(void *c, const char *p, const void *b,
                                u32 s, u32 o)
{ (void)c; (void)p; (void)b; (void)s; (void)o; return VFS_ERR_IO; }

static int iso9660_sync(void *c) { (void)c; return VFS_OK; }

/* --- FS情報 --- */
static u32 iso9660_total_blocks(void *c)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)c;
    return ctx ? ctx->volume_size : 0;
}

static u32 iso9660_free_blocks(void *c)
{
    (void)c;
    return 0; /* 読み取り専用 → 空きなし */
}

static u32 iso9660_block_size_fn(void *c)
{
    Iso9660Ctx *ctx = (Iso9660Ctx *)c;
    return ctx ? (u32)ctx->block_size : ISO_SECTOR_SIZE;
}

/* ======================================================================== */
/*  VfsOps テーブル                                                          */
/* ======================================================================== */

VfsOps iso9660_ops = {
    "iso9660",
    iso9660_mount, iso9660_umount, iso9660_is_mounted,
    iso9660_list_dir, iso9660_mkdir, iso9660_rmdir,
    iso9660_read_file, iso9660_write_file, iso9660_unlink,
    iso9660_rename,
    iso9660_get_file_size, iso9660_read_stream, iso9660_write_stream,
    iso9660_sync,
    iso9660_total_blocks, iso9660_free_blocks, iso9660_block_size_fn,
    iso9660_stat
};
