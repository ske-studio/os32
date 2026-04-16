/* ======================================================================== */
/*  SERIALFS.C — シリアル・リモートファイルシステム実装                     */
/* ======================================================================== */

#include "config.h"

#ifdef CONFIG_SERIALFS

#include "serialfs.h"
#include "../drivers/serial.h"
#include "../lib/lzss.h"
#include "../kernel/kmalloc.h"

/* 外部のタイマー関数 (sys_clock 等) を利用 - get_tick() 相当 */
extern u32 sys_get_tick(void); /* timer.c あたりに存在するか... 存在しない場合は自作ループ */

static int s_mounted = 0;

/* --- CRC16 ストリーム計算用 --- */
static u16 s_rx_crc16;

/* パケット受信時の簡易タイムアウト読み込み */
static int sfs_getc_timeout(u32 timeout_ms)
{
    int ch;
    u32 loops = timeout_ms * 10000; /* エミュレータの高速実行対策としてループ数を10倍に増加 */
    while (loops > 0) {
        ch = serial_trygetchar();
        if (ch >= 0) return ch;
        loops--;
        if ((loops & 0xFF) == 0) {
            __asm__ volatile("hlt"); /* 割り込み待ち */
        }
    }
    return -1;
}

/* ストリームデコード・チェックサム計算用コールバック */
static int sfs_get_encoded_byte(void)
{
    int ch = sfs_getc_timeout(5000);
    if (ch >= 0) {
        int i;
        s_rx_crc16 ^= (u8)ch;
        for (i = 0; i < 8; i++) {
            if (s_rx_crc16 & 1) s_rx_crc16 = (s_rx_crc16 >> 1) ^ 0xA001;
            else s_rx_crc16 >>= 1;
        }
    }
    return ch;
}

/* RPC送信ヘルパ */
static u8 sfs_send_header(u8 cmd, u32 payload_len)
{
    u8 csum;
    serial_putchar(0x05);  /* ENQ */
    serial_putchar('S');
    serial_putchar('F');
    serial_putchar(cmd);

    serial_putchar((u8)(payload_len & 0xFF));
    serial_putchar((u8)((payload_len >> 8) & 0xFF));
    serial_putchar((u8)((payload_len >> 16) & 0xFF));
    serial_putchar((u8)((payload_len >> 24) & 0xFF));

    csum = cmd + (u8)(payload_len & 0xFF) + (u8)((payload_len >> 8) & 0xFF) + (u8)((payload_len >> 16) & 0xFF) + (u8)((payload_len >> 24) & 0xFF);
    return csum;
}

static int sfs_rpc_req(u8 cmd, const void *payload, u32 len)
{
    u8 csum;
    u32 i;
    const u8 *p = (const u8 *)payload;

    csum = sfs_send_header(cmd, len);
    for (i = 0; i < len; i++) {
        serial_putchar(p[i]);
        csum += p[i];
    }
    serial_putchar(csum);
    return 0;
}

/* RPC受信ヘルパ (ヘッダ待ち) */
static int sfs_rpc_resp(u8 *err, u32 *len)
{
    int ch;
    u8 csum_calc = 0, csum_rx;
    u32 rx_len = 0;

    /* ヘッダ待ち: ACK(0x06), 'S', 'F' */
    ch = sfs_getc_timeout(3000); if (ch != 0x06) return -1;
    ch = sfs_getc_timeout(3000); if (ch != 'S') return -1;
    ch = sfs_getc_timeout(3000); if (ch != 'F') return -1;

    ch = sfs_getc_timeout(1000); if (ch < 0) return -1;
    *err = (u8)ch; csum_calc += *err;

    ch = sfs_getc_timeout(1000); if (ch < 0) return -1; rx_len |= ((u32)ch); csum_calc += ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return -1; rx_len |= ((u32)ch << 8); csum_calc += ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return -1; rx_len |= ((u32)ch << 16); csum_calc += ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return -1; rx_len |= ((u32)ch << 24); csum_calc += ch;

    *len = rx_len;
    return 0;
}

/* ---- VFS実装 ---- */
/* SerialFSは永続的にシングルインスタンス (物理シリアルポート1本)。        */
/* void *ctx 引数は受け取るが常に無視する。                                */

static void *sfs_mount(int dev_id)
{
    (void)dev_id;
    if (!serial_is_initialized()) return (void *)0;
    s_mounted = 1;
    return (void *)1;  /* 非NULLのダミーポインタ */
}

static void sfs_umount(void *ctx)
{
    (void)ctx;
    s_mounted = 0;
}

static int sfs_is_mounted(void *ctx)
{
    (void)ctx;
    return s_mounted;
}

static int sfs_read_file(void *ctx, const char *path, void *buf, u32 max_size)
{
    int path_len;
    u8 err;
    u32 rx_len, i, orig_size, encoded_len;
    u8 *pbuf = (u8 *)buf;
    int ch, rd = 0;
    u16 rx_crc;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    path_len = 0;
    while (path[path_len]) path_len++;

    /* リクエスト送信 */
    if (sfs_rpc_req(SF_CMD_READ, path, path_len) < 0) return VFS_ERR_IO;

    /* 応答ヘッダ受信 */
    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err != SF_ERR_OK) return VFS_ERR_NOTFOUND;

    if (rx_len == 0) return 0; /* サイズ0 */

    /* ヘッダは (err + rx_len bytes) で加算済み (os32_serverの仕様通り)
     * sfs_rpc_resp は RX_LEN までの処理を済ませ、次は Payload。
     * ヘッダ用の 1byte CSUM を読み飛ばす */
    ch = sfs_getc_timeout(1000);
    if (ch < 0) return VFS_ERR_IO;
    
    s_rx_crc16 = 0xFFFF;
    orig_size = 0;
    for (i = 0; i < 4; i++) {
        ch = sfs_get_encoded_byte();
        if (ch < 0) return VFS_ERR_IO;
        orig_size |= ((u32)ch << (i * 8));
    }
    
    encoded_len = (rx_len >= 4) ? (rx_len - 4) : 0;
    if (encoded_len > 0) {
        rd = lzss_decode_stream(sfs_get_encoded_byte, encoded_len, pbuf, max_size);
        if (rd < 0) return VFS_ERR_IO;
    }

    /* CRC16 の読み込みと検証 */
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc = (u16)ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc |= ((u16)ch << 8);

    if (s_rx_crc16 != rx_crc) return VFS_ERR_IO; /* CRC Mismatch! */
    return rd;
}

static int sfs_write_file(void *ctx, const char *path, const void *data, u32 size)
{
    int path_len, data_len, i;
    u8 err;
    u32 rx_len;
    u8 csum = 0;
    const u8 *p = (const u8 *)data;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    path_len = 0;
    while (path[path_len] && path_len < 255) path_len++;

    /* SF_CMD_WRITE: Payload = [path_len (1 byte)] + [path_str] + [data] */
    data_len = 1 + path_len + size;

    csum = sfs_send_header(SF_CMD_WRITE, data_len);
    
    serial_putchar((u8)path_len);
    csum += (u8)path_len;
    
    for (i = 0; i < path_len; i++) {
        serial_putchar(path[i]);
        csum += path[i];
    }
    for (i = 0; i < size; i++) {
        serial_putchar(p[i]);
        csum += p[i];
    }
    serial_putchar(csum);

    /* 応答待ち */
    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    return err == SF_ERR_OK ? (int)size : VFS_ERR_IO;
}

static int sfs_list_dir(void *ctx, const char *path, vfs_dir_cb cb, void *user_ctx)
{
    int path_len;
    u8 err;
    u32 rx_len, i, orig_size, encoded_len, bytes_read = 0;
    int ch, rd = 0;
    u16 rx_crc;
    VfsDirEntry ent;
    u8 *cbuf;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    path_len = 0;
    while (path[path_len]) path_len++;

    if (sfs_rpc_req(SF_CMD_LS, path, path_len) < 0) return VFS_ERR_IO;
    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err != SF_ERR_OK) return VFS_ERR_NOTFOUND;

    if (rx_len == 0) return 0;

    ch = sfs_getc_timeout(1000); /* 1byte CSUM skip */
    if (ch < 0) return VFS_ERR_IO;

    s_rx_crc16 = 0xFFFF;
    orig_size = 0;
    for (i = 0; i < 4; i++) {
        ch = sfs_get_encoded_byte();
        if (ch < 0) return VFS_ERR_IO;
        orig_size |= ((u32)ch << (i * 8));
    }
    
    encoded_len = (rx_len >= 4) ? (rx_len - 4) : 0;
    
    cbuf = (u8 *)kmalloc(orig_size > 0 ? orig_size : 1);
    if (!cbuf) return VFS_ERR_IO;

    if (encoded_len > 0) {
        rd = lzss_decode_stream(sfs_get_encoded_byte, encoded_len, cbuf, orig_size);
        if (rd < 0) { kfree(cbuf); return VFS_ERR_IO; }
    }

    /* CRC16 の読み込みと検証 */
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc = (u16)ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc |= ((u16)ch << 8);

    if (s_rx_crc16 != rx_crc) { kfree(cbuf); return VFS_ERR_IO; }

    /* バッファからエントリをパース */
    while (bytes_read < rd) {
        u8 e_type, e_namelen;
        u32 e_size = 0;
        
        e_type = cbuf[bytes_read++];
        e_size |= ((u32)cbuf[bytes_read++]);
        e_size |= ((u32)cbuf[bytes_read++] << 8);
        e_size |= ((u32)cbuf[bytes_read++] << 16);
        e_size |= ((u32)cbuf[bytes_read++] << 24);
        e_namelen = cbuf[bytes_read++];

        for (i = 0; i < e_namelen; i++) {
            ent.name[i] = cbuf[bytes_read++];
        }
        ent.name[e_namelen] = '\0';
        ent.type = e_type;
        ent.size = e_size;

        if (cb) cb(&ent, user_ctx);
    }

    kfree(cbuf);
    return bytes_read > 0 ? VFS_OK : VFS_OK;
}

static int sfs_simple_cmd(u8 cmd, const char *path)
{
    int path_len = 0;
    u8 err;
    u32 rx_len;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    while (path[path_len]) path_len++;
    if (sfs_rpc_req(cmd, path, path_len) < 0) return VFS_ERR_IO;
    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err == SF_ERR_EXT) return VFS_ERR_EXIST;
    if (err == SF_ERR_NOF) return VFS_ERR_NOTFOUND;
    if (err != SF_ERR_OK) return VFS_ERR_IO;
    return VFS_OK;
}

static int sfs_mkdir(void *ctx, const char *path) { (void)ctx; return sfs_simple_cmd(SF_CMD_MKDIR, path); }
static int sfs_rmdir(void *ctx, const char *path) { (void)ctx; return sfs_simple_cmd(SF_CMD_RMDIR, path); }
static int sfs_unlink(void *ctx, const char *path) { (void)ctx; return sfs_simple_cmd(SF_CMD_UNLINK, path); }

static int sfs_rename(void *ctx, const char *oldpath, const char *newpath)
{
    int old_len = 0, new_len = 0;
    u32 data_len;
    u8 csum = 0;
    u8 err;
    u32 rx_len;
    int i;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    while (oldpath[old_len]) old_len++;
    while (newpath[new_len]) new_len++;

    /* Payload = [old_len (1)] + [oldpath] + [new_len (1)] + [newpath] */
    data_len = 1 + old_len + 1 + new_len;

    csum = sfs_send_header(SF_CMD_RENAME, data_len);

    serial_putchar((u8)old_len); csum += (u8)old_len;
    for (i = 0; i < old_len; i++) { serial_putchar(oldpath[i]); csum += oldpath[i]; }
    serial_putchar((u8)new_len); csum += (u8)new_len;
    for (i = 0; i < new_len; i++) { serial_putchar(newpath[i]); csum += newpath[i]; }

    serial_putchar(csum);

    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err == SF_ERR_NOF) return VFS_ERR_NOTFOUND;
    if (err == SF_ERR_EXT) return VFS_ERR_EXIST;
    if (err != SF_ERR_OK) return VFS_ERR_IO;
    return VFS_OK;
}


static int sfs_get_file_size(void *ctx, const char *path, u32 *size)
{
    int path_len = 0;
    u8 err;
    u32 rx_len = 0;
    u32 orig_size, encoded_len;
    int ch, i, rd = 0;
    u16 rx_crc;
    u8 size_buf[4];

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    while (path[path_len]) path_len++;

    if (sfs_rpc_req(SF_CMD_GETSIZE, path, path_len) < 0) return VFS_ERR_IO;
    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err == SF_ERR_NOF) return VFS_ERR_NOTFOUND;
    if (err != SF_ERR_OK) return VFS_ERR_IO;

    if (rx_len == 0) return VFS_ERR_IO;

    ch = sfs_getc_timeout(1000); /* 1byte CSUM skip */
    if (ch < 0) return VFS_ERR_IO;

    s_rx_crc16 = 0xFFFF;
    orig_size = 0;
    for (i = 0; i < 4; i++) {
        ch = sfs_get_encoded_byte();
        if (ch < 0) return VFS_ERR_IO;
        orig_size |= ((u32)ch << (i * 8));
    }

    if (orig_size != 4) return VFS_ERR_IO;

    encoded_len = (rx_len >= 4) ? (rx_len - 4) : 0;
    if (encoded_len > 0) {
        rd = lzss_decode_stream(sfs_get_encoded_byte, encoded_len, size_buf, 4);
        if (rd < 0 || rd != 4) return VFS_ERR_IO;
    } else {
        return VFS_ERR_IO; /* サイズデータが空なのは異常 */
    }

    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc = (u16)ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc |= ((u16)ch << 8);

    if (s_rx_crc16 != rx_crc) return VFS_ERR_IO; /* CRC Mismatch! */

    *size = size_buf[0] | ((u32)size_buf[1] << 8) | ((u32)size_buf[2] << 16) | ((u32)size_buf[3] << 24);
    return VFS_OK;
}

static int sfs_read_stream(void *ctx, const char *path, void *buf, u32 size, u32 offset)
{
    int path_len = 0;
    u32 data_len;
    u8 csum;
    u8 err;
    u32 rx_len, orig_size, encoded_len;
    int i, ch, rd = 0;
    u8 *pbuf = (u8 *)buf;
    u16 rx_crc;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    while (path[path_len] && path_len < 255) path_len++;

    /* Payload = [offset (4)] + [size (4)] + [path] */
    data_len = 4 + 4 + path_len;
    csum = sfs_send_header(SF_CMD_READ_STREAM, data_len);

    serial_putchar((u8)(offset & 0xFF)); csum += (u8)(offset & 0xFF);
    serial_putchar((u8)((offset >> 8) & 0xFF)); csum += (u8)((offset >> 8) & 0xFF);
    serial_putchar((u8)((offset >> 16) & 0xFF)); csum += (u8)((offset >> 16) & 0xFF);
    serial_putchar((u8)((offset >> 24) & 0xFF)); csum += (u8)((offset >> 24) & 0xFF);

    serial_putchar((u8)(size & 0xFF)); csum += (u8)(size & 0xFF);
    serial_putchar((u8)((size >> 8) & 0xFF)); csum += (u8)((size >> 8) & 0xFF);
    serial_putchar((u8)((size >> 16) & 0xFF)); csum += (u8)((size >> 16) & 0xFF);
    serial_putchar((u8)((size >> 24) & 0xFF)); csum += (u8)((size >> 24) & 0xFF);

    for (i = 0; i < path_len; i++) {
        serial_putchar(path[i]);
        csum += path[i];
    }
    serial_putchar(csum);

    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err == SF_ERR_NOF) return VFS_ERR_NOTFOUND;
    if (err != SF_ERR_OK) return VFS_ERR_IO;

    if (rx_len == 0) return 0;

    ch = sfs_getc_timeout(1000); /* 1byte CSUM skip */
    if (ch < 0) return VFS_ERR_IO;

    s_rx_crc16 = 0xFFFF;
    orig_size = 0;
    for (i = 0; i < 4; i++) {
        ch = sfs_get_encoded_byte();
        if (ch < 0) return VFS_ERR_IO;
        orig_size |= ((u32)ch << (i * 8));
    }

    encoded_len = (rx_len >= 4) ? (rx_len - 4) : 0;
    if (encoded_len > 0) {
        rd = lzss_decode_stream(sfs_get_encoded_byte, encoded_len, pbuf, size);
        if (rd < 0) return VFS_ERR_IO;
    }

    /* CRC16 の読み込みと検証 */
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc = (u16)ch;
    ch = sfs_getc_timeout(1000); if (ch < 0) return VFS_ERR_IO; rx_crc |= ((u16)ch << 8);

    if (s_rx_crc16 != rx_crc) return VFS_ERR_IO; /* CRC Mismatch! */
    return rd;
}

static int sfs_write_stream(void *ctx, const char *path, const void *data, u32 size, u32 offset)
{
    int path_len = 0;
    u32 data_len;
    u8 csum;
    u8 err;
    u32 rx_len;
    int i;
    const u8 *p = (const u8 *)data;

    (void)ctx;
    if (!s_mounted) return VFS_ERR_NOMOUNT;
    while (path[path_len] && path_len < 255) path_len++;

    /* Payload = [offset (4)] + [path_len (1)] + [path] + [data] */
    data_len = 4 + 1 + path_len + size;
    csum = sfs_send_header(SF_CMD_WRITE_STREAM, data_len);

    serial_putchar((u8)(offset & 0xFF)); csum += (u8)(offset & 0xFF);
    serial_putchar((u8)((offset >> 8) & 0xFF)); csum += (u8)((offset >> 8) & 0xFF);
    serial_putchar((u8)((offset >> 16) & 0xFF)); csum += (u8)((offset >> 16) & 0xFF);
    serial_putchar((u8)((offset >> 24) & 0xFF)); csum += (u8)((offset >> 24) & 0xFF);

    serial_putchar((u8)path_len); csum += (u8)path_len;

    for (i = 0; i < path_len; i++) {
        serial_putchar(path[i]);
        csum += path[i];
    }
    for (i = 0; i < size; i++) {
        serial_putchar(p[i]);
        csum += p[i];
    }
    serial_putchar(csum);

    if (sfs_rpc_resp(&err, &rx_len) < 0) return VFS_ERR_IO;
    if (err == SF_ERR_NOF) return VFS_ERR_NOTFOUND;
    if (err != SF_ERR_OK) return VFS_ERR_IO;

    return (int)size;
}

VfsOps g_serialfs_ops = {
    "serialfs",
    sfs_mount,
    sfs_umount,
    sfs_is_mounted,
    sfs_list_dir,
    sfs_mkdir,
    sfs_rmdir,
    sfs_read_file,
    sfs_write_file,
    sfs_unlink,
    sfs_rename,
    sfs_get_file_size,
    sfs_read_stream,
    sfs_write_stream,
    0, /* sync */
    0, /* total_blocks */
    0, /* free_blocks */
    0, /* block_size */
    0  /* stat */
};

void serialfs_init(void)
{
    vfs_register_fs(&g_serialfs_ops);
}

#endif /* CONFIG_SERIALFS */
