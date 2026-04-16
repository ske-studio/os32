/* ======================================================================== */
/*  ATAPI.C — PC-98 IDE/ATAPI CD-ROM PIOドライバ実装                        */
/*                                                                          */
/*  IDEセカンダリバンクに接続されたATAPI CD-ROMデバイスに対して、PACKETコマ  */
/*  ンド (0xA0) を発行し、SCSIコマンド (READ(10)等) でデータ転送を行う。    */
/*                                                                          */
/*  プロトコル (PIOモード):                                                  */
/*    1. バンク切替 → セカンダリIDE選択                                     */
/*    2. コマンドレジスタに 0xA0 書込み                                     */
/*    3. DRQ + CD 待ち → 12バイトCDBを6ワード送出                          */
/*    4. データ転送: CylHi:Lo = バイト数 → データレジスタから読み出し      */
/*                                                                          */
/*  参照: NP21/W (atapicmd.c L206-428, ideio.c L932-948)                   */
/* ======================================================================== */

#include "atapi.h"
#include "io.h"
#include "pc98.h"

/* === 内部状態 === */
static int cdrom_present = 0;

/* ======================================================================== */
/*  内部ヘルパー                                                             */
/* ======================================================================== */

/* バンク選択 (0=プライマリ/HDD, 1=セカンダリ/CD-ROM) */
static void atapi_select_bank(int bank)
{
    outp(IDE_BANK1, (unsigned)(bank ? 0x01 : 0x00));
}

/* BSY=0 待ち */
static int atapi_wait_bsy(void)
{
    int timeout = IDE_TIMEOUT_LOOP;
    while (timeout-- > 0) {
        u8 st = (u8)inp(IDE_ALT_STATUS);
        if (!(st & IDE_ST_BSY)) return ATAPI_OK;
    }
    return ATAPI_ERR_TIMEOUT;
}

/* DRQ待ち (BSY=0 && DRQ=1) */
static int atapi_wait_drq(void)
{
    int timeout = IDE_TIMEOUT_LOOP;
    u8 st;

    while (timeout-- > 0) {
        st = (u8)inp(IDE_ALT_STATUS);
        if (!(st & IDE_ST_BSY)) break;
    }
    if (timeout <= 0) return ATAPI_ERR_TIMEOUT;

    st = (u8)inp(IDE_ALT_STATUS);
    if (st & IDE_ST_ERR) return ATAPI_ERR_IO;
    if (st & IDE_ST_DRQ) return ATAPI_OK;

    /* DRQ追加待ち */
    timeout = IDE_TIMEOUT_BSY;
    while (timeout-- > 0) {
        st = (u8)inp(IDE_ALT_STATUS);
        if (st & IDE_ST_DRQ) return ATAPI_OK;
        if (st & IDE_ST_ERR) return ATAPI_ERR_IO;
    }
    return ATAPI_ERR_TIMEOUT;
}

/* 12バイトCDBを0クリア */
static void atapi_clear_cdb(u8 *cdb)
{
    int i;
    for (i = 0; i < 12; i++) cdb[i] = 0;
}

/* ======================================================================== */
/*  PACKETコマンドプロトコル                                                  */
/*                                                                          */
/*  NP21/W ideio.c L932-948:                                                */
/*    case 0xa0: send packet                                                */
/*      → DRQセット + Interrupt Reason = CD|~IO                            */
/*      → ホストが12バイトCDB書込み → atapicmd_a0() 呼び出し              */
/* ======================================================================== */

/* PACKETコマンド発行: CDB送出 → 完了ステータス待ち (データなし)
 * 戻り値: ATAPI_OK=成功 */
static int atapi_packet_nodata(const u8 *cdb)
{
    int ret;
    int i;

    ret = atapi_wait_bsy();
    if (ret != ATAPI_OK) return ret;

    /* Features=0 (PIOモード), ByteCount=0 (データなし) */
    outp(IDE_FEATURES, 0x00);
    outp(IDE_CYL_LO, 0x00);
    outp(IDE_CYL_HI, 0x00);
    outp(IDE_DRV_HEAD, 0x00);

    /* PACKETコマンド発行 */
    outp(IDE_COMMAND, ATAPI_CMD_PACKET);

    /* DRQ待ち (デバイスがCDB受付準備完了) */
    ret = atapi_wait_drq();
    if (ret != ATAPI_OK) return ret;

    /* 12バイトCDBを6ワードで送出 */
    for (i = 0; i < 6; i++) {
        u16 w = (u16)cdb[i * 2] | ((u16)cdb[i * 2 + 1] << 8);
        outpw(IDE_DATA, (unsigned)w);
    }

    /* コマンド完了待ち (BSY=0) */
    ret = atapi_wait_bsy();
    if (ret != ATAPI_OK) return ret;

    /* エラーチェック */
    {
        u8 st = (u8)inp(IDE_STATUS);
        if (st & IDE_ST_ERR) return ATAPI_ERR_IO;
    }

    return ATAPI_OK;
}

/* PACKETコマンド発行: CDB送出 → データ読み出し
 *   cdb:      12バイトCDB
 *   buf:      データ受信バッファ
 *   buf_size: バッファサイズ
 *   actual:   実際の受信バイト数 (NULLなら無視)
 * 戻り値: ATAPI_OK=成功 */
static int atapi_packet_read(const u8 *cdb, void *buf, u32 buf_size,
                             u32 *actual)
{
    int ret;
    int i;
    u32 total_read = 0;
    u8 *p = (u8 *)buf;

    ret = atapi_wait_bsy();
    if (ret != ATAPI_OK) return ret;

    /* Features=0 (PIOモード), ByteCountにバッファサイズ上限を設定 */
    outp(IDE_FEATURES, 0x00);
    outp(IDE_CYL_LO, (unsigned)(buf_size & 0xFF));
    outp(IDE_CYL_HI, (unsigned)((buf_size >> 8) & 0xFF));
    outp(IDE_DRV_HEAD, 0x00);

    /* PACKETコマンド発行 */
    outp(IDE_COMMAND, ATAPI_CMD_PACKET);

    /* DRQ待ち (CDB受付準備) */
    ret = atapi_wait_drq();
    if (ret != ATAPI_OK) return ret;

    /* 12バイトCDB送出 */
    for (i = 0; i < 6; i++) {
        u16 w = (u16)cdb[i * 2] | ((u16)cdb[i * 2 + 1] << 8);
        outpw(IDE_DATA, (unsigned)w);
    }

    /* データ転送ループ */
    while (1) {
        u16 xfer_size;
        u16 words;
        u8 st;

        /* BSY=0になるまで待つ */
        ret = atapi_wait_bsy();
        if (ret != ATAPI_OK) return ret;

        /* ステータス確認 */
        st = (u8)inp(IDE_STATUS);
        if (st & IDE_ST_ERR) return ATAPI_ERR_IO;

        /* DRQが立っていなければ転送完了 */
        if (!(st & IDE_ST_DRQ)) break;

        /* CylHi:CylLo から転送バイト数を取得 */
        xfer_size = (u16)inp(IDE_CYL_LO) | ((u16)inp(IDE_CYL_HI) << 8);
        if (xfer_size == 0) break;

        /* ワード単位で読み出し */
        words = (xfer_size + 1) / 2;
        for (i = 0; i < words; i++) {
            u16 w = (u16)inpw(IDE_DATA);
            if (total_read < buf_size) {
                p[total_read] = (u8)(w & 0xFF);
                if (total_read + 1 < buf_size) {
                    p[total_read + 1] = (u8)(w >> 8);
                }
            }
            total_read += 2;
        }
    }

    /* IRQクリア */
    { u8 st = (u8)inp(IDE_STATUS); (void)st; }

    if (actual) *actual = (total_read < buf_size) ? total_read : buf_size;
    return ATAPI_OK;
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

int atapi_init(void)
{
    u8 cl, ch;

    /* セカンダリバンクに切替 */
    atapi_select_bank(1);

    /* 割り込み無効 (ポーリングモード) */
    outp(IDE_DEV_CTRL, IDE_NIEN);

    /* ドライブ選択 (マスター位置) */
    outp(IDE_DRV_HEAD, 0x00);
    {
        int i;
        for (i = 0; i < IDE_SEL_SETTLE; i++) inp(IDE_ALT_STATUS);
    }

    /* BSY待ち */
    if (atapi_wait_bsy() != ATAPI_OK) {
        atapi_select_bank(0);
        return 0;
    }

    /* ATAPIシグネチャ確認
     * NP21/W ideio.c: ATAPIデバイスは IDENTIFY(0xEC) 時に
     *   CylLo=0x14, CylHi=0xEB を返す */
    cl = (u8)inp(IDE_CYL_LO);
    ch = (u8)inp(IDE_CYL_HI);

    if (cl == ATAPI_SIG_CYL_LO && ch == ATAPI_SIG_CYL_HI) {
        cdrom_present = 1;
    } else {
        /* シグネチャが出ない場合、ソフトリセット後に再確認 */
        outp(IDE_DEV_CTRL, IDE_NIEN | 0x04); /* SRST */
        {
            int i;
            for (i = 0; i < 50000; i++) inp(IDE_ALT_STATUS);
        }
        outp(IDE_DEV_CTRL, IDE_NIEN);         /* SRST解除 */
        atapi_wait_bsy();

        cl = (u8)inp(IDE_CYL_LO);
        ch = (u8)inp(IDE_CYL_HI);
        if (cl == ATAPI_SIG_CYL_LO && ch == ATAPI_SIG_CYL_HI) {
            cdrom_present = 1;
        }
    }

    /* プライマリバンクに戻す */
    atapi_select_bank(0);

    return cdrom_present;
}

int atapi_present(void)
{
    return cdrom_present;
}

int atapi_test_unit_ready(void)
{
    u8 cdb[12];
    int ret;

    if (!cdrom_present) return ATAPI_ERR_NO_DRIVE;

    atapi_select_bank(1);

    atapi_clear_cdb(cdb);
    cdb[0] = SCSI_CMD_TEST_UNIT_READY;

    ret = atapi_packet_nodata(cdb);

    atapi_select_bank(0);

    if (ret != ATAPI_OK) return ATAPI_ERR_NO_MEDIA;
    return ATAPI_OK;
}

int atapi_read_capacity(AtapiCapacity *cap)
{
    u8 cdb[12];
    u8 buf[8];
    int ret;

    if (!cdrom_present) return ATAPI_ERR_NO_DRIVE;
    if (!cap) return ATAPI_ERR_IO;

    atapi_select_bank(1);

    atapi_clear_cdb(cdb);
    cdb[0] = SCSI_CMD_READ_CAPACITY;

    ret = atapi_packet_read(cdb, buf, 8, 0);

    atapi_select_bank(0);

    if (ret != ATAPI_OK) return ret;

    /* READ CAPACITY応答: ビッグエンディアン
     * bytes 0-3: 最終LBA
     * bytes 4-7: セクタサイズ */
    cap->total_sectors = ((u32)buf[0] << 24) | ((u32)buf[1] << 16)
                       | ((u32)buf[2] << 8)  |  (u32)buf[3];
    cap->total_sectors += 1;  /* 最終LBA → 総セクタ数 */

    cap->sector_size   = ((u32)buf[4] << 24) | ((u32)buf[5] << 16)
                       | ((u32)buf[6] << 8)  |  (u32)buf[7];

    return ATAPI_OK;
}

int atapi_read_sectors(u32 lba, u32 count, void *buf)
{
    u8 *p = (u8 *)buf;
    u32 i;

    if (!cdrom_present) return ATAPI_ERR_NO_DRIVE;

    atapi_select_bank(1);

    /* 1セクタずつ読み出し (シンプル + 安全) */
    for (i = 0; i < count; i++) {
        u8 cdb[12];
        int ret;

        atapi_clear_cdb(cdb);
        cdb[0] = SCSI_CMD_READ_10;
        /* LBA (ビッグエンディアン, bytes 2-5) */
        cdb[2] = (u8)((lba + i) >> 24);
        cdb[3] = (u8)((lba + i) >> 16);
        cdb[4] = (u8)((lba + i) >> 8);
        cdb[5] = (u8)((lba + i) & 0xFF);
        /* 転送セクタ数 (bytes 7-8) = 1 */
        cdb[7] = 0;
        cdb[8] = 1;

        ret = atapi_packet_read(cdb, p + i * ATAPI_SECTOR_SIZE,
                                ATAPI_SECTOR_SIZE, 0);
        if (ret != ATAPI_OK) {
            atapi_select_bank(0);
            return ret;
        }
    }

    atapi_select_bank(0);
    return ATAPI_OK;
}
