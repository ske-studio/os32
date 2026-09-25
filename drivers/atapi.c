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
#include "kprintf.h"

/* === 内部状態 === */
static int cdrom_present = 0;
/* 使う装置の選択 (DRV_HEAD に書く値)。セカンダリのマスター = 0x00、
 * スレーブ = ATAPI_DRV_SLAVE。atapi_init が媒体の入っている方を選ぶ */
static u8 s_drvsel = 0x00;

/* 最後にエラーで終わったコマンドのセンスキー (エラーレジスタの bit7-4)。
 * 次のコマンドを出す前に読まないと消える */
static u8 s_last_sense = 0;
/* 媒体の世代 (atapi_media_gen)。UNIT ATTENTION / NOT READY で進む */
static u32 s_media_gen = 0;
static AtapiStats s_stats;

/* 最後の PACKET の終わり方 (診断の行のため)。st = 最後に読んだステータス、
 * err = エラーレジスタ (ERR のときだけ)、got = 受け取ったバイト数 */
static u8  s_diag_st = 0;
static u8  s_diag_err = 0;
static u32 s_diag_got = 0;
/* 読みの失敗の行を出した数 (ATAPI_DIAG_MAX で止める) */
static u32 s_diag_lines = 0;

/* ======================================================================== */
/*  内部ヘルパー                                                             */
/* ======================================================================== */

/* バンク選択 (0=プライマリ/HDD, 1=セカンダリ/CD-ROM)。セカンダリへ切り替えた
 * ときは使う装置 (s_drvsel) も選び直してから 400ns 置く — 最初の BSY 待ちが
 * 別の装置 (居なければ 0xFF = BSY のまま) を見ないように */
static void atapi_select_bank(int bank)
{
    outp(IDE_BANK1, (unsigned)(bank ? 0x01 : 0x00));
    if (bank) {
        int i;
        outp(IDE_DRV_HEAD, s_drvsel);
        for (i = 0; i < IDE_SEL_SETTLE; i++) (void)inp(IDE_ALT_STATUS);
    }
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

/* ERR を見たときに呼ぶ。センスキーを覚え、媒体が替わった/替わりつつある
 *印なら世代を進める。 */
static void atapi_note_error(void)
{
    s_diag_err = (u8)inp(IDE_ERROR);
    s_last_sense = (u8)(s_diag_err >> ATAPI_ERR_SENSE_SHIFT);
    if (s_last_sense == ATAPI_SK_UNIT_ATTENTION
        || s_last_sense == ATAPI_SK_NOT_READY) {
        s_media_gen++;
        s_stats.unit_attention++;
    }
}

/* DRQ のブロックを読み終えた直後、デバイスが DRQ を落として BSY を上げる
 * までの 400ns を置く (ここを置かないと、前のブロックの DRQ=1 を読んで
 * もう 1 ブロックあると取り違える)。ALT_STATUS の空読みで待つ */
static void atapi_settle(void)
{
    int i;
    for (i = 0; i < IDE_SEL_SETTLE; i++) (void)inp(IDE_ALT_STATUS);
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
    outp(IDE_DRV_HEAD, s_drvsel);

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
        if (st & IDE_ST_ERR) {
            atapi_note_error();
            return ATAPI_ERR_IO;
        }
    }

    return ATAPI_OK;
}

/* PACKETコマンド発行: CDB送出 → データ読み出し
 *   cdb:      12バイトCDB
 *   buf:      データ受信バッファ
 *   buf_size: バッファサイズ
 *   actual:   デバイスが渡したバイト数 (NULLなら無視)。buf_size を超えた分は
 *             捨てるが、数には入れる (呼び手が「過不足なし」を確かめるため)
 * 戻り値: ATAPI_OK=成功
 *
 * データは 1 回以上の DRQ で来る。各 DRQ のバイト数は Cylinder Low/High に
 * 出る (byte count limit 以下、デバイスが決める)。DRQ が落ちるまで繰り返す。 */
static int atapi_packet_read(const u8 *cdb, void *buf, u32 buf_size,
                             u32 *actual)
{
    int ret;
    int i;
    u32 total_read = 0;
    u32 bcl;
    u8 *p = (u8 *)buf;

    if (actual) *actual = 0;
    s_diag_st = 0;
    s_diag_err = 0;
    s_diag_got = 0;

    ret = atapi_wait_bsy();
    if (ret != ATAPI_OK) return ret;

    /* byte count limit: 1 回の DRQ の上限。偶数で、ATAPI_PIO_BCL_MAX 以下 */
    bcl = (buf_size < ATAPI_PIO_BCL_MAX) ? buf_size : ATAPI_PIO_BCL_MAX;
    bcl &= ~1UL;
    if (bcl == 0) bcl = 2;

    /* Features=0 (PIOモード) */
    outp(IDE_FEATURES, 0x00);
    outp(IDE_CYL_LO, (unsigned)(bcl & 0xFF));
    outp(IDE_CYL_HI, (unsigned)((bcl >> 8) & 0xFF));
    outp(IDE_DRV_HEAD, s_drvsel);

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
    atapi_settle();

    /* データ転送ループ (DRQ ごとに 1 回) */
    while (1) {
        u16 xfer_size;
        u32 words;
        u32 k;
        u8 st;

        /* BSY=0になるまで待つ */
        ret = atapi_wait_bsy();
        if (ret != ATAPI_OK) return ret;

        /* ステータス確認 */
        st = (u8)inp(IDE_STATUS);
        s_diag_st = st;
        if (st & IDE_ST_ERR) {
            s_diag_got = total_read;
            atapi_note_error();
            return ATAPI_ERR_IO;
        }

        /* DRQが立っていなければ転送完了 */
        if (!(st & IDE_ST_DRQ)) break;

        /* CylHi:CylLo から転送バイト数を取得 */
        xfer_size = (u16)inp(IDE_CYL_LO) | ((u16)inp(IDE_CYL_HI) << 8);
        if (xfer_size == 0) break;

        /* ワード単位で読み出し。buf_size を超えた分は読み捨てる */
        words = ((u32)xfer_size + 1) / 2;
        for (k = 0; k < words; k++) {
            u16 w = (u16)inpw(IDE_DATA);
            if (total_read < buf_size) {
                p[total_read] = (u8)(w & 0xFF);
                if (total_read + 1 < buf_size) {
                    p[total_read + 1] = (u8)(w >> 8);
                }
            }
            total_read += 2;
        }
        atapi_settle();
    }

    /* IRQクリア */
    { u8 st = (u8)inp(IDE_STATUS); (void)st; }

    s_diag_got = total_read;
    if (actual) *actual = total_read;
    return ATAPI_OK;
}

/* ======================================================================== */
/*  公開API                                                                  */
/* ======================================================================== */

/* sel (マスター / スレーブ) に ATAPI のシグネチャが出ているか。コマンドを
 * 出す前 (リセットの直後) にだけ意味がある — PACKET の後は CylLo/Hi が
 * byte count に変わる。居ない装置は 0xFF が返るので BSY 待ちをしない */
static int atapi_probe_sig(u8 sel)
{
    u8 cl, ch;
    int i;

    outp(IDE_DRV_HEAD, sel);
    for (i = 0; i < IDE_SEL_SETTLE; i++) (void)inp(IDE_ALT_STATUS);
    if ((u8)inp(IDE_ALT_STATUS) == 0xFF) return 0;
    if (atapi_wait_bsy() != ATAPI_OK) return 0;

    /* NP21/W ideio.c: ATAPI デバイスはリセット後 CylLo=0x14, CylHi=0xEB */
    cl = (u8)inp(IDE_CYL_LO);
    ch = (u8)inp(IDE_CYL_HI);
    return (cl == ATAPI_SIG_CYL_LO && ch == ATAPI_SIG_CYL_HI) ? 1 : 0;
}

/* READ CAPACITY (バンクは呼び手が選んでおく)。8 バイトちょうど来なければ失敗 */
static int atapi_capacity_raw(AtapiCapacity *cap)
{
    u8 cdb[12];
    u8 buf[8];
    u32 got = 0;
    int ret;

    atapi_clear_cdb(cdb);
    cdb[0] = SCSI_CMD_READ_CAPACITY;
    ret = atapi_packet_read(cdb, buf, 8, &got);
    /* 受け皿の残りを容量として読まない */
    if (ret == ATAPI_OK && got != 8) ret = ATAPI_ERR_IO;
    if (ret != ATAPI_OK) return ret;

    /* READ CAPACITY応答: ビッグエンディアン
     * bytes 0-3: 最終LBA (NP21/W は総数を返す。空のドライブは 0)
     * bytes 4-7: セクタサイズ */
    cap->total_sectors = ((u32)buf[0] << 24) | ((u32)buf[1] << 16)
                       | ((u32)buf[2] << 8)  |  (u32)buf[3];
    cap->total_sectors += 1;  /* 最終LBA → 総セクタ数 */

    cap->sector_size   = ((u32)buf[4] << 24) | ((u32)buf[5] << 16)
                       | ((u32)buf[6] << 8)  |  (u32)buf[7];
    return ATAPI_OK;
}

/* 選んでいる装置に媒体が入っているか (容量が読めて 1 セクタより多い) */
static int atapi_has_media(void)
{
    AtapiCapacity cap;
    return (atapi_capacity_raw(&cap) == ATAPI_OK && cap.total_sectors > 1) ? 1 : 0;
}

int atapi_init(void)
{
    static const u8 sels[2] = { 0x00, ATAPI_DRV_SLAVE };
    u8 found[2] = { 0, 0 };
    int n = 0;
    int i;

    /* セカンダリバンクに切替 */
    atapi_select_bank(1);

    /* 割り込み無効 (ポーリングモード) */
    outp(IDE_DEV_CTRL, IDE_NIEN);

    /* マスターとスレーブの両方を見る (2026-09-26: NP21/W の ide3 = セカンダリの
     * スレーブに ISO を付けると、マスターの空の CD ドライブだけを見ていて
     * 1 セクタも読めなかった) */
    for (i = 0; i < 2; i++) if (atapi_probe_sig(sels[i])) found[n++] = sels[i];

    if (n == 0) {
        /* シグネチャが出ない場合、ソフトリセット後に再確認 */
        outp(IDE_DEV_CTRL, IDE_NIEN | 0x04); /* SRST */
        for (i = 0; i < 50000; i++) inp(IDE_ALT_STATUS);
        outp(IDE_DEV_CTRL, IDE_NIEN);         /* SRST解除 */
        for (i = 0; i < 2; i++) if (atapi_probe_sig(sels[i])) found[n++] = sels[i];
    }

    if (n > 0) {
        cdrom_present = 1;
        s_drvsel = found[0];
        /* 2 台あれば、媒体の入っている方 (マスター優先)。どちらも空ならマスター。
         * 容量を読むのはシグネチャを全部見た後 (PACKET で CylLo/Hi が変わる) */
        if (n == 2) {
            for (i = 0; i < n; i++) {
                s_drvsel = found[i];
                if (atapi_has_media()) break;
            }
            if (i == n) s_drvsel = found[0];
        }
    }

    /* 最後に選んだのはスレーブかもしれないが、以後の操作は atapi_select_bank(1)
     * で s_drvsel を選び直すので、ここでは戻さない */
    /* プライマリバンクに戻す */
    atapi_select_bank(0);

    return cdrom_present;
}

/* 使っている装置 (0 = セカンダリのマスター、1 = スレーブ) */
int atapi_drive_index(void)
{
    return (s_drvsel == ATAPI_DRV_SLAVE) ? 1 : 0;
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
    int ret;

    if (!cdrom_present) return ATAPI_ERR_NO_DRIVE;
    if (!cap) return ATAPI_ERR_IO;

    atapi_select_bank(1);
    ret = atapi_capacity_raw(cap);
    atapi_select_bank(0);
    return ret;
}

/* READ(10) を 1 回出す (n セクタ、バンクは呼び手が選んでおく)。
 * デバイスが渡したバイト数がちょうど n セクタでなければ失敗。
 * UNIT ATTENTION なら媒体の世代を進めて 1 回だけ出し直す。 */
static int atapi_read10(u32 lba, u32 n, u8 *dst)
{
    int attempt;
    int ret = ATAPI_ERR_IO;

    for (attempt = 0; attempt < 2; attempt++) {
        u8 cdb[12];
        u32 got = 0;

        atapi_clear_cdb(cdb);
        cdb[0] = SCSI_CMD_READ_10;
        /* LBA (ビッグエンディアン, bytes 2-5) */
        cdb[2] = (u8)(lba >> 24);
        cdb[3] = (u8)(lba >> 16);
        cdb[4] = (u8)(lba >> 8);
        cdb[5] = (u8)(lba & 0xFF);
        /* 転送セクタ数 (ビッグエンディアン, bytes 7-8) */
        cdb[7] = (u8)(n >> 8);
        cdb[8] = (u8)(n & 0xFF);

        s_last_sense = 0;
        s_stats.read10_cmds++;
        ret = atapi_packet_read(cdb, dst, n * ATAPI_SECTOR_SIZE, &got);
        if (ret == ATAPI_OK) {
            if (got != n * ATAPI_SECTOR_SIZE) return ATAPI_ERR_IO;
            s_stats.read10_sectors += n;
            return ATAPI_OK;
        }
        if (s_last_sense != ATAPI_SK_UNIT_ATTENTION) break;
    }
    return ret;
}

/* 読みが失敗したときの 1 行 (ATAPI_DIAG_MAX 行まで)。NP21/W と実機で
 * 「どこで落ちたか」を画面で分けるため:
 *   sense=5 (ILLEGAL REQUEST) = 範囲外か、ドライブに媒体が無い (NP21/W は
 *            空のドライブへの READ(10) を ILLEGAL REQUEST / asc 21h で返す)
 *   sense=2 = NOT READY、sense=6 = UNIT ATTENTION (出し直しても落ちた)
 *   ret=-1 = BSY / DRQ の待ちの期限切れ、got が n*2048 未満 = 転送が足りない */
static void atapi_diag_fail(u32 lba, u32 n, int ret)
{
    if (s_diag_lines >= ATAPI_DIAG_MAX) return;
    s_diag_lines++;
    kprintf(0x07, "[atapi] READ(10) drv=%d lba=%u n=%u ret=%d st=%02x err=%02x sense=%x got=%u\n",
            atapi_drive_index(), (unsigned)lba, (unsigned)n, ret, (unsigned)s_diag_st,
            (unsigned)s_diag_err, (unsigned)s_last_sense, (unsigned)s_diag_got);
}

int atapi_read_sectors(u32 lba, u32 count, void *buf)
{
    u8 *p = (u8 *)buf;

    if (!cdrom_present) return ATAPI_ERR_NO_DRIVE;

    atapi_select_bank(1);

    /* 連続する範囲を ATAPI_READ_MAX_SECTORS ずつの READ(10) で読む */
    while (count > 0) {
        u32 n = (count < ATAPI_READ_MAX_SECTORS) ? count : ATAPI_READ_MAX_SECTORS;
        int ret = atapi_read10(lba, n, p);

        /* 複数セクタが失敗したら、その範囲を 1 セクタずつ読み直す
         * (ドライブとの相性・範囲のどこかの読めないセクタ)。
         * 1 セクタでも落ちたらそこで失敗を返す */
        if (ret != ATAPI_OK && n > 1) {
            u32 i;
            s_stats.multi_fail++;
            for (i = 0; i < n; i++) {
                s_stats.single_retry++;
                ret = atapi_read10(lba + i, 1, p + i * ATAPI_SECTOR_SIZE);
                if (ret != ATAPI_OK) break;
            }
        }
        if (ret != ATAPI_OK) {
            atapi_select_bank(0);
            atapi_diag_fail(lba, n, ret);
            return ret;
        }
        lba   += n;
        count -= n;
        p     += n * ATAPI_SECTOR_SIZE;
    }

    atapi_select_bank(0);
    return ATAPI_OK;
}

u32 atapi_media_gen(void)
{
    return s_media_gen;
}

void atapi_get_stats(AtapiStats *out)
{
    if (out) *out = s_stats;
}
