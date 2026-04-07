/* ======================================================================== */
/*  IDE.C — PC-98 IDE/ATA PIOドライバ実装                                   */
/*                                                                          */
/*  PIOモードによるセクタ読み書き。LBA28 対応。                             */
/*  NP21/W + DOSBox-X + FreeBSD/pc98 wdc を参考に実装。                     */
/* ======================================================================== */

#include "ide.h"
#include "io.h"
#include "pc98.h"

/* ドライブ検出フラグ */
static int drive_present[2] = { 0, 0 };
static IdeInfo drive_info[2];

/* ---- 内部ヘルパー ---- */

/* バンク選択 (0=プライマリ, 無効化しない) */
static void ide_select_bank(int bank)
{
    /* NP21/Wではgetidedev()がセカンダリバンク(IDE_BANK1)を参照するため、
       明示的に書き込む必要がある。値0x00=プライマリ, 0x01=セカンダリ */
    outp(IDE_BANK1, (unsigned)(bank ? 0x01 : 0x00));
}

/* ステータス待ち (BSY=0になるまで) */
static int ide_wait_bsy(void)
{
    int timeout = IDE_TIMEOUT_BSY;
    while (timeout-- > 0) {
        u8 st = (u8)inp(IDE_ALT_STATUS);
        if (!(st & IDE_ST_BSY)) return IDE_OK;
    }
    return IDE_ERR_TIMEOUT;
}

/* DRQ待ち: BSYが落ちてDRQが立つまで */
static int ide_wait_drq(void)
{
    int timeout = IDE_TIMEOUT_LOOP;
    u8 st;

    /* まずBSYが落ちるのを待つ */
    while (timeout-- > 0) {
        st = (u8)inp(IDE_ALT_STATUS);
        if (!(st & IDE_ST_BSY)) break;
    }
    if (timeout <= 0) return IDE_ERR_TIMEOUT;

    /* DRQチェック */
    st = (u8)inp(IDE_ALT_STATUS);
    if (st & IDE_ST_ERR) return IDE_ERR_IO;
    if (st & IDE_ST_DRQ) return IDE_OK;

    /* DRQが来るまで追加待ち */
    timeout = IDE_TIMEOUT_BSY;
    while (timeout-- > 0) {
        st = (u8)inp(IDE_ALT_STATUS);
        if (st & IDE_ST_DRQ) return IDE_OK;
        if (st & IDE_ST_ERR) return IDE_ERR_IO;
    }
    return IDE_ERR_TIMEOUT;
}

/* DRDY待ち */
static int ide_wait_ready(void)
{
    int timeout = IDE_TIMEOUT_BSY;
    while (timeout-- > 0) {
        u8 st = (u8)inp(IDE_ALT_STATUS);
        if (!(st & IDE_ST_BSY) && (st & IDE_ST_DRDY)) return IDE_OK;
    }
    return IDE_ERR_TIMEOUT;
}

/* ドライブ選択 (0=マスター, 1=スレーブ) */
static void ide_select_drive(int drive)
{
    outp(IDE_DRV_HEAD, (unsigned)(IDE_DRV_SEL_BASE | ((drive & 1) << 4)));
    /* ドライブ選択後、400ns待ち（ダミーリード） */
    {
        int i;
        for (i = 0; i < IDE_SEL_SETTLE; i++) inp(IDE_ALT_STATUS);
    }
}

/* LBAアドレスセット */
static void ide_set_lba(int drive, u32 lba, u8 count)
{
    outp(IDE_SECT_CNT, (unsigned)count);
    outp(IDE_SECT_NUM, (unsigned)(lba & 0xFF));
    outp(IDE_CYL_LO,   (unsigned)((lba >> 8) & 0xFF));
    outp(IDE_CYL_HI,   (unsigned)((lba >> 16) & 0xFF));
    /* LBAモード: bit6=1, bit5=1(LBA), bit4=drive, bit3-0=LBA[27:24] */
    outp(IDE_DRV_HEAD, (unsigned)(IDE_LBA_MODE | ((drive & 1) << 4) | ((lba >> 24) & 0x0F)));
}

/* ============================================================ */
/*  公開API                                                      */
/* ============================================================ */

int ide_init(void)
{
    int found = 0;

    /* PC-98スレーブPICでIRQ9をマスク。
       IRQ9 = スレーブPIC bit1。未処理IRQによるシステム破壊を防止。 */
    {
        u8 mask = (u8)inp(PIC_SLAVE_DATA);
        outp(PIC_SLAVE_DATA, mask | 0x02);  /* bit1 = IRQ9をマスク */
    }

    /* 割り込み無効 (nIEN=1) — ポーリングモード */
    outp(IDE_DEV_CTRL, IDE_NIEN);

    /* マスタードライブ検出 */
    ide_select_drive(0);
    if (ide_wait_bsy() == IDE_OK) {
        u8 st = (u8)inp(IDE_STATUS);
        if (st != 0xFF && st != 0x00) {
            drive_present[0] = 1;
            found++;
            ide_identify(0, &drive_info[0]);
        }
    }

    /* マスタードライブを再選択（後続アクセスのため） */
    ide_select_drive(0);

    return found;
}

int ide_identify(int drive, IdeInfo *info)
{
    u16 buf[256];
    int i, ret;

    ide_select_drive(drive & 1);
    ret = ide_wait_bsy();
    if (ret != IDE_OK) return ret;

    /* IDENTIFYコマンド発行 */
    outp(IDE_COMMAND, IDE_CMD_IDENTIFY);

    /* DRQ待ち */
    ret = ide_wait_drq();
    if (ret != IDE_OK) return IDE_ERR_NO_DRIVE;

    /* 256ワード読み出し (個別inpwループ) */
    {
        int w;
        for (w = 0; w < 256; w++) {
            buf[w] = (u16)inpw(IDE_DATA);
        }
    }

    /* IRQクリア: Statusレジスタ読み出し */
    { u8 st = (u8)inp(IDE_STATUS); (void)st; }
    ide_wait_bsy();

    /* 情報抽出 */
    info->cylinders = buf[1];
    info->heads = buf[3];
    info->sectors = buf[6];

    /* LBAサポート確認 (word 49 bit 9) */
    info->lba_supported = (buf[49] & 0x0200) ? 1 : 0;

    /* LBA総セクタ数 (word 60-61) */
    info->total_sectors = (u32)buf[60] | ((u32)buf[61] << 16);

    /* サイズ(MB) = セクタ数 * 512 / 1048576 */
    info->size_mb = info->total_sectors / 2048;

    /* モデル名 (word 27-46): ATA文字列はバイトスワップ */
    for (i = 0; i < 20; i++) {
        info->model[i * 2]     = (char)(buf[27 + i] >> 8);
        info->model[i * 2 + 1] = (char)(buf[27 + i] & 0xFF);
    }
    info->model[40] = '\0';
    /* 末尾空白除去 */
    for (i = 39; i >= 0 && info->model[i] == ' '; i--) {
        info->model[i] = '\0';
    }

    /* シリアル番号 (word 10-19) */
    for (i = 0; i < 10; i++) {
        info->serial[i * 2]     = (char)(buf[10 + i] >> 8);
        info->serial[i * 2 + 1] = (char)(buf[10 + i] & 0xFF);
    }
    info->serial[20] = '\0';
    for (i = 19; i >= 0 && info->serial[i] == ' '; i--) {
        info->serial[i] = '\0';
    }

    /* ファームウェア (word 23-26) */
    for (i = 0; i < 4; i++) {
        info->firmware[i * 2]     = (char)(buf[23 + i] >> 8);
        info->firmware[i * 2 + 1] = (char)(buf[23 + i] & 0xFF);
    }
    info->firmware[8] = '\0';
    for (i = 7; i >= 0 && info->firmware[i] == ' '; i--) {
        info->firmware[i] = '\0';
    }

    return IDE_OK;
}

int ide_read_sector(int drive, u32 lba, void *buf)
{
    int ret;

    if (!drive_present[drive & 1]) return IDE_ERR_NO_DRIVE;

    ret = ide_wait_bsy();
    if (ret != IDE_OK) return ret;

    ide_set_lba(drive, lba, 1);

    outp(IDE_COMMAND, IDE_CMD_READ);

    ret = ide_wait_drq();
    if (ret != IDE_OK) return ret;

    /* 256ワード = 512バイト読み出し (個別inpwループ) */
    {
        u16 *dst = (u16 *)buf;
        int w;
        for (w = 0; w < 256; w++) {
            dst[w] = (u16)inpw(IDE_DATA);
        }
    }

    /* データ読み出し後: Statusレジスタを読んでIRQクリア */
    {
        u8 st = (u8)inp(IDE_STATUS);
        (void)st;
    }

    /* 転送完了待ち */
    ide_wait_bsy();

    return IDE_OK;
}

int ide_read_sectors(int drive, u32 lba, u32 count, void *buf)
{
    u32 i;
    u8 *p = (u8 *)buf;

    for (i = 0; i < count; i++) {
        int ret = ide_read_sector(drive, lba + i, p + i * 512);
        if (ret != IDE_OK) return ret;
    }
    return IDE_OK;
}

int ide_write_sector(int drive, u32 lba, const void *buf)
{
    int ret;
    const u16 *data = (const u16 *)buf;
    int i;

    if (!drive_present[drive & 1]) return IDE_ERR_NO_DRIVE;

    ret = ide_wait_bsy();
    if (ret != IDE_OK) return ret;

    ide_set_lba(drive, lba, 1);

    outp(IDE_COMMAND, IDE_CMD_WRITE);

    ret = ide_wait_drq();
    if (ret != IDE_OK) return ret;

    /* 256ワード = 512バイト書き込み */
    for (i = 0; i < 256; i++) {
        outpw(IDE_DATA, (unsigned)data[i]);
    }

    /* 書き込み完了待ち */
    ret = ide_wait_bsy();
    if (ret != IDE_OK) return ret;

    /* エラーチェック */
    {
        u8 st = (u8)inp(IDE_STATUS);
        if (st & IDE_ST_ERR) return IDE_ERR_IO;
    }

    return IDE_OK;
}

int ide_write_sectors(int drive, u32 lba, u32 count, const void *buf)
{
    u32 i;
    const u8 *p = (const u8 *)buf;

    for (i = 0; i < count; i++) {
        int ret = ide_write_sector(drive, lba + i, p + i * 512);
        if (ret != IDE_OK) return ret;
    }
    return IDE_OK;
}

int ide_drive_present(int drive)
{
    return drive_present[drive & 1];
}

int ide_get_info(int drive, IdeInfo *info)
{
    if (!drive_present[drive & 1]) return IDE_ERR_NO_DRIVE;
    if (info) {
        int i;
        u8 *dst = (u8 *)info;
        u8 *src = (u8 *)&drive_info[drive & 1];
        for (i = 0; i < sizeof(IdeInfo); i++) {
            dst[i] = src[i];
        }
    }
    return IDE_OK;
}
