/* ======================================================================== */
/*  FAT12.C — FAT12 ファイルシステムドライバ                                */
/*                                                                          */
/*  PC-98 2HD フロッピー (1024B/sector) のFAT12読み書き                     */
/*  参照: Microsoft FAT Specification, Linux fs/fat/                        */
/* ======================================================================== */

#include "fat12.h"
#include "disk.h"
#include "vfs.h"
#include "kstring.h"
#include "kmalloc.h"
#include "os_time.h"

/* ======== 内部状態 ======== */
static FAT12_Info finfo;

/* FATテーブルバッファ (最大8セクタ=8KB、PC-98 1024Bセクタ) */
static u8 fat_buf[FAT12_MAX_FAT_SIZE];
static int fat_buf_sectors;

/* セクタ読み出しバッファ (1セクタ分) */
static u8 sect_buf[1024];

/* ======== ユーティリティ ======== */
/* メモリ操作は kstring.h (kmemcpy, kmemset) に統一 */

static int fat_toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int fat_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* ======================================================================== */
/*  12bit FATエントリ操作                                                   */
/* ======================================================================== */

/* クラスタ番号からFATエントリ値を取得 */
static u16 fat12_get_entry(int cluster)
{
    int offset = cluster + (cluster / 2);  /* 1.5バイト/エントリ */
    u16 val;

    if (offset + 1 >= fat_buf_sectors * (int)finfo.bytes_per_sector)
        return FAT12_EOC;

    val = (u16)fat_buf[offset] | ((u16)fat_buf[offset + 1] << 8);

    if (cluster & 1)
        return (val >> 4) & 0x0FFF;
    else
        return val & 0x0FFF;
}

/* FATエントリに値を書き込み */
static void fat12_set_entry(int cluster, u16 value)
{
    int offset = cluster + (cluster / 2);
    u16 val;

    if (offset + 1 >= fat_buf_sectors * (int)finfo.bytes_per_sector)
        return;

    val = (u16)fat_buf[offset] | ((u16)fat_buf[offset + 1] << 8);

    if (cluster & 1) {
        val = (val & 0x000F) | ((value & 0x0FFF) << 4);
    } else {
        val = (val & 0xF000) | (value & 0x0FFF);
    }

    fat_buf[offset] = (u8)(val & 0xFF);
    fat_buf[offset + 1] = (u8)((val >> 8) & 0xFF);
}

/* ======================================================================== */
/*  クラスタ ↔ LBA変換                                                     */
/* ======================================================================== */

/* クラスタ番号の先頭LBAを返す (クラスタ2が最初のデータクラスタ) */
static u32 cluster_to_lba(int cluster)
{
    return finfo.data_start_lba +
           (u32)(cluster - 2) * finfo.sectors_per_cluster;
}

/* ======================================================================== */
/*  8.3ファイル名変換                                                       */
/* ======================================================================== */

/* ユーザー入力ファイル名を8.3形式(11バイト、スペースパディング)に変換 */
static void name_to_83(const char *input, char *out83)
{
    int i, j;
    const char *dot;

    kmemset(out83, ' ', FAT12_NAME_LEN);

    /* ドットの位置を探す */
    dot = 0;
    for (i = 0; input[i]; i++) {
        if (input[i] == '.') dot = &input[i];
    }

    /* ファイル名部分 (最大8文字) */
    j = 0;
    for (i = 0; input[i] && j < 8; i++) {
        if (&input[i] == dot) break;
        out83[j++] = (char)fat_toupper((u8)input[i]);
    }

    /* 拡張子部分 (最大3文字) */
    if (dot) {
        j = 8;
        for (i = 1; dot[i] && j < 11; i++) {
            out83[j++] = (char)fat_toupper((u8)dot[i]);
        }
    }
}

/* 8.3形式を人間可読の "name.ext" 形式に変換 (小文字で出力) */
static void name_from_83(const FAT12_DirEntry *ent, char *out, int maxlen)
{
    int i, j;

    j = 0;
    /* ファイル名 (末尾スペースを除去、小文字化) */
    for (i = 0; i < 8 && j < maxlen - 1; i++) {
        if (ent->name[i] == ' ') break;
        out[j++] = (char)fat_tolower((u8)ent->name[i]);
    }
    /* 拡張子 (あれば、小文字化) */
    if (ent->ext[0] != ' ') {
        out[j++] = '.';
        for (i = 0; i < 3 && j < maxlen - 1; i++) {
            if (ent->ext[i] == ' ') break;
            out[j++] = (char)fat_tolower((u8)ent->ext[i]);
        }
    }
    out[j] = '\0';
}

/* 8.3形式の名前比較 (name[8]+ext[3]を分離比較) */
static int name_match_83(const FAT12_DirEntry *ent, const char *name83)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (ent->name[i] != name83[i]) return 0;
    }
    for (i = 0; i < 3; i++) {
        if (ent->ext[i] != name83[8 + i]) return 0;
    }
    return 1;
}

/* ======================================================================== */
/*  マウント / アンマウント                                                 */
/* ======================================================================== */

int fat12_mount(int dev_id)
{
    FAT12_BPB *bpb;
    int i;
    int old_drive = finfo.drive_num;

    finfo.drive_num = dev_id;

    /* ブートセクタ (LBA 0) を読み込み */
    if (disk_read_lba(dev_id, 0, 1, sect_buf) != 0) {
        finfo.drive_num = old_drive;
        return -1;
    }

    bpb = (FAT12_BPB *)sect_buf;

    /* 簡易バリデーション */
    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024) {
        finfo.drive_num = old_drive;
        return -2;  /* 非対応セクタサイズ */
    }
    if (bpb->num_fats == 0 || bpb->num_fats > 2) {
        finfo.drive_num = old_drive;
        return -3;
    }
    if (bpb->sectors_per_cluster == 0) {
        finfo.drive_num = old_drive;
        return -4;
    }

    /* BPB情報をコピー */
    finfo.bytes_per_sector    = bpb->bytes_per_sector;
    finfo.sectors_per_cluster = bpb->sectors_per_cluster;
    finfo.reserved_sectors    = bpb->reserved_sectors;
    finfo.num_fats            = bpb->num_fats;
    finfo.root_entry_count    = bpb->root_entry_count;
    finfo.fat_size            = bpb->fat_size_16;
    finfo.total_sectors       = bpb->total_sectors_16;
    if (finfo.total_sectors == 0)
        finfo.total_sectors = (u16)bpb->total_sectors_32;

    /* レイアウト計算 */
    finfo.fat_start_lba      = finfo.reserved_sectors;
    finfo.root_dir_start_lba = finfo.fat_start_lba +
                               (u32)finfo.num_fats * finfo.fat_size;
    finfo.root_dir_sectors   = ((u32)finfo.root_entry_count * 32 +
                               finfo.bytes_per_sector - 1) /
                               finfo.bytes_per_sector;
    finfo.data_start_lba     = finfo.root_dir_start_lba +
                               finfo.root_dir_sectors;
    finfo.total_data_clusters= (finfo.total_sectors - finfo.data_start_lba) /
                               finfo.sectors_per_cluster;

    /* FATテーブルを読み込み */
    fat_buf_sectors = finfo.fat_size;
    if (fat_buf_sectors > 8) fat_buf_sectors = 8;

    for (i = 0; i < fat_buf_sectors; i++) {
        if (disk_read_lba(dev_id, (int)(finfo.fat_start_lba + i), 1,
                          &fat_buf[i * finfo.bytes_per_sector]) != 0) {
            finfo.drive_num = old_drive;
            return -5;
        }
    }

    finfo.mounted = 1;
    return 0;
}

void fat12_unmount(void)
{
    finfo.mounted = 0;
}

int fat12_is_mounted(void)
{
    return finfo.mounted;
}

const FAT12_Info *fat12_get_info(void)
{
    return &finfo;
}

/* ======================================================================== */
/*  ディレクトリ操作                                                        */
/* ======================================================================== */

/* ルートディレクトリの全エントリを走査してコールバック呼び出し */
int fat12_list(void (*print_fn)(const char *, u32, u8))
{
    int s, i, count;
    int entries_per_sector;

    if (!finfo.mounted) return -1;

    entries_per_sector = finfo.bytes_per_sector / 32;
    count = 0;

    for (s = 0; s < (int)finfo.root_dir_sectors; s++) {
        FAT12_DirEntry *dir;

        if (disk_read_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + s), 1, sect_buf) != 0)
            return -1;

        dir = (FAT12_DirEntry *)sect_buf;

        for (i = 0; i < entries_per_sector; i++) {
            char fname[13];

            /* 終端 */
            if ((u8)dir[i].name[0] == 0x00) return count;
            /* 削除済み */
            if ((u8)dir[i].name[0] == 0xE5) continue;
            /* VFATロングネーム / ボリュームラベルはスキップ */
            if (dir[i].attr == FAT_ATTR_LFN) continue;
            if (dir[i].attr & FAT_ATTR_VOLUME) continue;

            name_from_83(&dir[i], fname, sizeof(fname));

            if (print_fn)
                print_fn(fname, dir[i].file_size, dir[i].attr);

            count++;
        }
    }

    return count;
}

/* ルートディレクトリからファイル名で検索 */
static FAT12_DirEntry *fat12_find(const char *name, int *out_sector, int *out_index)
{
    char name83[11];
    int s, i;
    int entries_per_sector;
    static FAT12_DirEntry found;

    if (!finfo.mounted) return 0;

    name_to_83(name, name83);
    entries_per_sector = finfo.bytes_per_sector / 32;

    for (s = 0; s < (int)finfo.root_dir_sectors; s++) {
        FAT12_DirEntry *dir;

        if (disk_read_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + s), 1, sect_buf) != 0)
            return 0;

        dir = (FAT12_DirEntry *)sect_buf;

        for (i = 0; i < entries_per_sector; i++) {
            if ((u8)dir[i].name[0] == 0x00) return 0;
            if ((u8)dir[i].name[0] == 0xE5) continue;
            if (dir[i].attr == FAT_ATTR_LFN) continue;

            if (name_match_83(&dir[i], name83)) {
                kmemcpy(&found, &dir[i], sizeof(found));
                if (out_sector) *out_sector = s;
                if (out_index) *out_index = i;
                return &found;
            }
        }
    }
    return 0;
}

/* ======================================================================== */
/*  ファイル読込                                                            */
/* ======================================================================== */

int fat12_read(const char *name, void *buf, int maxsz)
{
    FAT12_DirEntry *ent;
    u16 cluster;
    int bytes_read, remaining, chunk;
    u8 *dst;

    ent = fat12_find(name, 0, 0);
    if (!ent) return -1;

    dst = (u8 *)buf;
    remaining = (int)ent->file_size;
    if (remaining > maxsz) remaining = maxsz;
    bytes_read = 0;
    cluster = ent->start_cluster;

    while (remaining > 0 && cluster >= 2 && cluster < FAT12_EOC) {
        u32 lba = cluster_to_lba(cluster);
        int s;

        /* クラスタ内の全セクタを読み込み */
        for (s = 0; s < finfo.sectors_per_cluster && remaining > 0; s++) {
            if (disk_read_lba(finfo.drive_num,(int)(lba + s), 1, sect_buf) != 0)
                return bytes_read > 0 ? bytes_read : -1;

            chunk = finfo.bytes_per_sector;
            if (chunk > remaining) chunk = remaining;

            kmemcpy(dst, sect_buf, (u32)chunk);
            dst += chunk;
            bytes_read += chunk;
            remaining -= chunk;
        }

        /* 次のクラスタへ */
        cluster = fat12_get_entry(cluster);
    }

    return bytes_read;
}

int fat12_read_stream(const char *name, void *buf, u32 size, u32 offset)
{
    FAT12_DirEntry *ent;
    u16 cluster;
    int bytes_read, remaining, chunk;
    u32 cluster_size;
    u32 skip_clusters;
    u32 byte_in_cluster;
    u8 *dst;

    ent = fat12_find(name, 0, 0);
    if (!ent) return VFS_ERR_NOTFOUND;

    if (offset >= ent->file_size) return 0;
    
    dst = (u8 *)buf;
    remaining = (int)ent->file_size - offset;
    if (remaining > size) remaining = size;
    bytes_read = 0;
    cluster = ent->start_cluster;
    
    cluster_size = finfo.bytes_per_sector * finfo.sectors_per_cluster;
    skip_clusters = offset / cluster_size;
    byte_in_cluster = offset % cluster_size;

    while (skip_clusters > 0 && cluster >= 2 && cluster < FAT12_EOC) {
        cluster = fat12_get_entry(cluster);
        skip_clusters--;
    }

    while (remaining > 0 && cluster >= 2 && cluster < FAT12_EOC) {
        u32 lba = cluster_to_lba(cluster);
        int s;
        u32 offset_in_cluster = byte_in_cluster;
        
        for (s = 0; s < finfo.sectors_per_cluster && remaining > 0; s++) {
            if (offset_in_cluster >= finfo.bytes_per_sector) {
                offset_in_cluster -= finfo.bytes_per_sector;
                continue;
            }
            
            if (disk_read_lba(finfo.drive_num,(int)(lba + s), 1, sect_buf) != 0)
                return bytes_read > 0 ? bytes_read : VFS_ERR_IO;

            chunk = finfo.bytes_per_sector - offset_in_cluster;
            if (chunk > remaining) chunk = remaining;

            kmemcpy(dst, &sect_buf[offset_in_cluster], (u32)chunk);
            dst += chunk;
            bytes_read += chunk;
            remaining -= chunk;
            offset_in_cluster = 0;
        }

        cluster = fat12_get_entry(cluster);
        byte_in_cluster = 0;
    }

    return bytes_read;
}

/* ======================================================================== */
/*  ファイル書込                                                            */
/* ======================================================================== */

/* 空きクラスタを検索 */
static int fat12_alloc_cluster(void)
{
    int i;
    for (i = 2; i < (int)finfo.total_data_clusters + 2; i++) {
        if (fat12_get_entry(i) == FAT12_FREE)
            return i;
    }
    return -1;  /* 空きなし */
}

/* FATテーブルをディスクに書き戻す */
static int fat12_flush_fat(void)
{
    int i, f;
    for (f = 0; f < finfo.num_fats; f++) {
        u32 base = finfo.fat_start_lba + (u32)f * finfo.fat_size;
        for (i = 0; i < fat_buf_sectors; i++) {
            if (disk_write_lba(finfo.drive_num,(int)(base + i), 1,
                              &fat_buf[i * finfo.bytes_per_sector]) != 0)
                return -1;
        }
    }
    return 0;
}

/* ルートディレクトリの空きエントリを探す */
static int fat12_find_free_dir(int *out_sector, int *out_index)
{
    int s, i;
    int entries_per_sector = finfo.bytes_per_sector / 32;

    for (s = 0; s < (int)finfo.root_dir_sectors; s++) {
        FAT12_DirEntry *dir;

        if (disk_read_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + s), 1, sect_buf) != 0)
            return -1;

        dir = (FAT12_DirEntry *)sect_buf;

        for (i = 0; i < entries_per_sector; i++) {
            if ((u8)dir[i].name[0] == 0x00 || (u8)dir[i].name[0] == 0xE5) {
                *out_sector = s;
                *out_index = i;
                return 0;
            }
        }
    }
    return -1;  /* 空きなし */
}

int fat12_write(const char *name, const void *buf, int sz)
{
    char name83[11];
    int dir_sector, dir_index;
    int first_cluster, prev_cluster, cur_cluster;
    const u8 *src;
    int remaining, chunk, s;
    FAT12_DirEntry *dir;

    if (!finfo.mounted) return -1;

    /* 既存ファイルがあれば削除 */
    fat12_delete(name);

    /* 空きディレクトリエントリを探す */
    if (fat12_find_free_dir(&dir_sector, &dir_index) != 0)
        return -1;

    /* データを書き込み */
    name_to_83(name, name83);
    src = (const u8 *)buf;
    remaining = sz;
    first_cluster = -1;
    prev_cluster = -1;

    while (remaining > 0) {
        u32 lba;
        cur_cluster = fat12_alloc_cluster();
        if (cur_cluster < 0) return -1;

        /* チェーンに追加 */
        if (first_cluster < 0) first_cluster = cur_cluster;
        if (prev_cluster >= 0) fat12_set_entry(prev_cluster, (u16)cur_cluster);
        fat12_set_entry(cur_cluster, FAT12_EOC);

        /* クラスタにデータ書き込み */
        lba = cluster_to_lba(cur_cluster);
        for (s = 0; s < finfo.sectors_per_cluster && remaining > 0; s++) {
            chunk = finfo.bytes_per_sector;
            if (chunk > remaining) {
                /* 最終セクタはゼロクリアしてからコピー */
                kmemset(sect_buf, 0, (u32)finfo.bytes_per_sector);
                kmemcpy(sect_buf, src, (u32)remaining);
                chunk = remaining;
            } else {
                kmemcpy(sect_buf, src, (u32)chunk);
            }
            if (disk_write_lba(finfo.drive_num,(int)(lba + s), 1, sect_buf) != 0)
                return -1;
            src += chunk;
            remaining -= chunk;
        }

        prev_cluster = cur_cluster;
    }

    /* FATテーブルを書き戻す */
    if (fat12_flush_fat() != 0) return -1;

    /* ディレクトリエントリを書き込み */
    if (disk_read_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + dir_sector), 1, sect_buf) != 0)
        return -1;

    dir = (FAT12_DirEntry *)sect_buf;
    kmemset(&dir[dir_index], 0, 32);
    kmemcpy(dir[dir_index].name, name83, 11);
    dir[dir_index].attr = FAT_ATTR_ARCHIVE;
    dir[dir_index].start_cluster = (first_cluster >= 0) ? (u16)first_cluster : 0;
    dir[dir_index].file_size = (u32)sz;

    if (disk_write_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + dir_sector), 1, sect_buf) != 0)
        return -1;

    return 0;
}

/* ======================================================================== */
/*  ファイル削除                                                            */
/* ======================================================================== */

int fat12_delete(const char *name)
{
    FAT12_DirEntry *ent;
    int dir_sector, dir_index;
    u16 cluster, next;
    FAT12_DirEntry *dir;

    ent = fat12_find(name, &dir_sector, &dir_index);
    if (!ent) return -1;

    /* クラスタチェーンを解放 */
    cluster = ent->start_cluster;
    while (cluster >= 2 && cluster < FAT12_EOC) {
        next = fat12_get_entry(cluster);
        fat12_set_entry(cluster, FAT12_FREE);
        cluster = next;
    }

    /* FATを書き戻す */
    fat12_flush_fat();

    /* ディレクトリエントリを削除マーク */
    if (disk_read_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + dir_sector), 1, sect_buf) != 0)
        return -1;

    dir = (FAT12_DirEntry *)sect_buf;
    dir[dir_index].name[0] = (char)0xE5;

    if (disk_write_lba(finfo.drive_num,(int)(finfo.root_dir_start_lba + dir_sector), 1, sect_buf) != 0)
        return -1;

    return 0;
}

/* ======== FAT12 VFSラッパー ======== */
/* マルチインスタンス対応: mount()でFAT12_Infoをkmalloc確保しコンテキストとする。 */
/* 内部関数は引き続き静的finfo/fat_bufを使用 (フロッピーは物理的にシングル)。    */

/* パスからファイル名部分を抽出 (FAT12はルート直下のみ) */
static const char *fat12_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/') last = p + 1;
        p++;
    }
    /* "/" のみの場合は空文字列 */
    return last;
}

static void *fat12_vfs_mount(int dev_id)
{
    FAT12_Info *fi;
    if (fat12_mount(dev_id) != 0) return (void *)0;
    /* コンテキストとしてfinfo のコピーをkmalloc確保 */
    fi = (FAT12_Info *)kmalloc(sizeof(FAT12_Info));
    if (!fi) { fat12_unmount(); return (void *)0; }
    kmemcpy(fi, &finfo, sizeof(FAT12_Info));
    return (void *)fi;
}

static void fat12_vfs_umount(void *ctx)
{
    fat12_unmount();
    if (ctx) kfree(ctx);
}

static int fat12_vfs_is_mounted(void *ctx)
{
    (void)ctx;
    return fat12_is_mounted();
}

/* fat12_list用コールバック変換コンテキスト */
typedef struct {
    vfs_dir_cb  user_cb;
    void       *user_ctx;
} Fat12ListCtx;

/* 注意: fat12_list()がコールバックにコンテキストを渡せないため、
 * 暫定でstaticを使用。シングルタスクOSなので問題なし。 */
static Fat12ListCtx fat12_list_ctx;

/* fat12_list用コールバック: FAT12 print_fn → VfsDirEntry変換 */
static void fat12_to_vfs_cb(const char *name, u32 size, u8 attr)
{
    VfsDirEntry ve;
    int i;
    for (i = 0; name[i] && i < 255; i++) ve.name[i] = name[i];
    ve.name[i] = '\0';
    ve.size = size;
    ve.type = (attr & 0x10) ? VFS_TYPE_DIR : VFS_TYPE_FILE; /* FAT_ATTR_DIRECTORY */
    fat12_list_ctx.user_cb(&ve, fat12_list_ctx.user_ctx);
}

static int fat12_vfs_list(void *ctx, const char *path, vfs_dir_cb cb, void *user_ctx)
{
    int rc;
    (void)ctx;
    (void)path;  /* FAT12はルートディレクトリのみ */
    fat12_list_ctx.user_cb = cb;
    fat12_list_ctx.user_ctx = user_ctx;
    rc = fat12_list(fat12_to_vfs_cb);
    return (rc >= 0) ? VFS_OK : VFS_ERR_IO;
}

static int fat12_vfs_read(void *ctx, const char *path, void *buf, u32 max_size)
{
    const char *fname;
    (void)ctx;
    fname = fat12_basename(path);
    if (!fname[0]) return VFS_ERR_NOTFOUND;
    return fat12_read(fname, buf, (int)max_size);
}

static int fat12_vfs_write(void *ctx, const char *path, const void *data, u32 size)
{
    const char *fname;
    (void)ctx;
    fname = fat12_basename(path);
    if (!fname[0]) return VFS_ERR_INVAL;
    return fat12_write(fname, data, (int)size);
}

static int fat12_vfs_unlink(void *ctx, const char *path)
{
    const char *fname;
    (void)ctx;
    fname = fat12_basename(path);
    if (!fname[0]) return VFS_ERR_INVAL;
    return fat12_delete(fname);
}

static int fat12_vfs_read_stream(void *ctx, const char *path, void *buf, u32 size, u32 offset)
{
    const char *fname;
    (void)ctx;
    fname = fat12_basename(path);
    if (!fname[0]) return VFS_ERR_NOTFOUND;
    return fat12_read_stream(fname, buf, size, offset);
}

static int fat12_vfs_write_stream(void *ctx, const char *path, const void *buf, u32 size, u32 offset)
{
    /* FAT12でのシーク書き込みは未実装 (主に読出し専用として使われるため) */
    (void)ctx; (void)path; (void)buf; (void)size; (void)offset;
    return VFS_ERR_INVAL;
}

static int fat12_vfs_get_size(void *ctx, const char *path, u32 *size)
{
    const char *fname;
    FAT12_DirEntry *ent;
    (void)ctx;
    fname = fat12_basename(path);
    if (!fname[0]) return VFS_ERR_NOTFOUND;
    ent = fat12_find(fname, 0, 0);
    if (!ent) return VFS_ERR_NOTFOUND;
    if (size) *size = ent->file_size;
    return VFS_OK;
}

static int fat12_vfs_stat(void *ctx, const char *path, OS32_Stat *buf)
{
    const char *fname;
    FAT12_DirEntry *ent;
    u16 mode = 0;
    (void)ctx;
    fname = fat12_basename(path);
    
    if (!buf) return VFS_ERR_INVAL;

    kmemset(buf, 0, sizeof(OS32_Stat));

    /* ROOT ディレクトリの場合 */
    if (!fname[0] || (fname[0] == '.' && fname[1] == '\0')) {
        buf->st_dev = 0;
        buf->st_ino = 2; /* root クラスタ */
        buf->st_mode = OS_S_IFDIR | OS_S_IRWXU | OS_S_IRWXG | OS_S_IRWXO; /* 0777 */
        buf->st_nlink = 2;
        buf->st_size = 0;
        return VFS_OK;
    }

    ent = fat12_find(fname, 0, 0);
    if (!ent) return VFS_ERR_NOTFOUND;

    buf->st_dev = 0;
    buf->st_ino = ent->start_cluster;
    
    if (ent->attr & FAT_ATTR_DIRECTORY) {
        mode = OS_S_IFDIR | OS_S_IRWXU | OS_S_IRWXG | OS_S_IROTH | OS_S_IXOTH; /* 0775 */
    } else {
        mode = OS_S_IFREG;
        if (ent->attr & FAT_ATTR_READONLY) {
            mode |= OS_S_IRUSR | OS_S_IXUSR | OS_S_IRGRP | OS_S_IXGRP | OS_S_IROTH | OS_S_IXOTH; /* 0555 */
        } else {
            mode |= OS_S_IRWXU | OS_S_IRWXG | OS_S_IROTH | OS_S_IWOTH | OS_S_IXOTH; /* 0777 */
        }
    }
    buf->st_mode = mode;
    buf->st_nlink = 1;
    buf->st_uid = 0;
    buf->st_gid = 0;
    buf->st_size = ent->file_size;
    
    buf->st_mtime = dos_time_to_epoch(ent->date, ent->time);
    buf->st_atime = buf->st_mtime;
    buf->st_ctime = buf->st_mtime;
    
    return VFS_OK;
}

/* FAT12はフラットFS: サブディレクトリ作成/削除は非対応 */
static int fat12_vfs_mkdir(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return VFS_ERR_INVAL;
}

static int fat12_vfs_rmdir(void *ctx, const char *path)
{
    (void)ctx; (void)path;
    return VFS_ERR_INVAL;
}

/* FAT12は書き込み時に即フラッシュ済み */
static int fat12_vfs_sync(void *ctx) { (void)ctx; return 0; }

static u32 fat12_vfs_total_blocks(void *ctx)
{
    const FAT12_Info *fi = fat12_get_info();
    (void)ctx;
    return (u32)fi->total_data_clusters;
}

static u32 fat12_vfs_free_blocks(void *ctx)
{
    (void)ctx;
    /* FAT12には空きクラスタ数のカウンタがないので0を返す */
    return 0;
}

static u32 fat12_vfs_block_size(void *ctx)
{
    const FAT12_Info *fi = fat12_get_info();
    (void)ctx;
    return (u32)fi->sectors_per_cluster * fi->bytes_per_sector;
}

static VfsOps fat12_ops = {
    "fat12",
    fat12_vfs_mount, fat12_vfs_umount, fat12_vfs_is_mounted,
    fat12_vfs_list, fat12_vfs_mkdir, fat12_vfs_rmdir,
    fat12_vfs_read, fat12_vfs_write, fat12_vfs_unlink,
    (void *)0, /* rename */
    fat12_vfs_get_size, fat12_vfs_read_stream, fat12_vfs_write_stream,
    fat12_vfs_sync,
    fat12_vfs_total_blocks, fat12_vfs_free_blocks, fat12_vfs_block_size,
    fat12_vfs_stat
};


/* ======== 初期化・登録 ======== */
void fat12_init(void)
{
    vfs_register_fs(&fat12_ops);
}
