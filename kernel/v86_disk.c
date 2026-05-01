/* ======================================================================== */
/*  V86_DISK.C — V86 ディスクBIOS (INT 1Bh) 仮想化                         */
/*                                                                          */
/*  PC-98 INT 1Bh ディスクBIOSのエミュレーション。                           */
/*  メモリ上に保持したFDDイメージからセクタを読み出す。                      */
/*                                                                          */
/*  PC-98 2HD FDD ジオメトリ:                                              */
/*    77シリンダ × 2ヘッド × 8セクタ/トラック × 1024バイト/セクタ           */
/*                                                                          */
/*  INT 1Bh レジスタ規約 (1MB FDD):                                        */
/*    AH = ファンクション番号                                               */
/*    AL = DA/UA (90h = 1MB FDD UNIT#0)                                    */
/*    BX = 転送バイト数                                                     */
/*    CL = セクタ長コード (3 = 1024バイト)                                  */
/*    DH = シリンダ番号 (0-76)                                              */
/*    DL = セクタ番号 (0ベース: 0-7=ヘッド0, 8-15=ヘッド1)                 */
/*    ES:BP = 転送先/元バッファアドレス                                     */
/*                                                                          */
/*    戻り値: AH = ステータス (0=成功)                                     */
/*    CF = 0:成功, 1:エラー                                                */
/* ======================================================================== */

#include "v86_disk.h"
#include "v86.h"
#include "v86_mem.h"
#include "kstring.h"
#include "io.h"

/* FDDイメージデータ (外部から設定される) */
static const u8 *fdd_image = 0;
static u32 fdd_image_size = 0;

/* ====================================================================== */
/*  v86_disk_set_image — FDDイメージを設定                                 */
/* ====================================================================== */
void v86_disk_set_image(const u8 *data, u32 size)
{
    fdd_image = data;
    fdd_image_size = size;
}

/* ====================================================================== */
/*  v86_disk_clear — FDDイメージをクリア                                   */
/* ====================================================================== */
void v86_disk_clear(void)
{
    fdd_image = 0;
    fdd_image_size = 0;
}

/* ====================================================================== */
/*  CHS → イメージオフセット変換                                           */
/*                                                                          */
/*  PC-98 FDD の INT 1Bh では:                                            */
/*    DH = シリンダ (C)                                                    */
/*    DL = ヘッド×SPT + セクタ                                             */
/*      → DL 0-7  = ヘッド0, セクタ0-7                                    */
/*      → DL 8-15 = ヘッド1, セクタ0-7                                    */
/*                                                                          */
/*  LBA = C × (Heads × SPT) + DL                                          */
/*  オフセット = LBA × BPS                                                 */
/* ====================================================================== */
static i32 chs_to_offset(u8 cylinder, u8 sector_dl)
{
    u32 lba;
    u32 offset;

    /* 範囲チェック */
    if (cylinder >= V86_FDD_CYLINDERS) return -1;
    if (sector_dl >= (V86_FDD_HEADS * V86_FDD_SPT)) return -1;

    lba = (u32)cylinder * (V86_FDD_HEADS * V86_FDD_SPT) + (u32)sector_dl;
    offset = lba * V86_FDD_BPS;

    if (offset >= fdd_image_size) return -1;

    return (i32)offset;
}

/* ====================================================================== */
/*  v86_bios_int1b — INT 1Bh ディスクBIOS ハンドラ                         */
/* ====================================================================== */
int v86_bios_int1b(u32 *regs)
{
    u8 func = (u8)((regs[V86_REG_EAX] >> 8) & 0xFF);
    u8 daua = (u8)(regs[V86_REG_EAX] & 0xFF);

    /* DA/UAチェック: 1MB FDD UNIT#0 のみサポート */
    if ((daua & 0xF0) == 0x90) {
        /* 1MB FDD */
    } else {
        /* 未サポートデバイス: エラー応答 */
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL) | 0x6000UL;
        /* CF セット: EFLAGS bit 0 */
        regs[V86_REG_EFLAGS] |= 1;
        return 0;
    }

    switch (func) {

    /* ================================================================ */
    /*  AH=03h: ドライブ初期化 (Initialize)                             */
    /* ================================================================ */
    case 0x03:
        /* 成功応答: AH=0, CF=0 */
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        regs[V86_REG_EFLAGS] &= ~1UL;
        break;

    /* ================================================================ */
    /*  AH=04h: センス (メディア検知)                                    */
    /*  戻り値: AH=0(成功), CF=0                                        */
    /* ================================================================ */
    case 0x04:
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        regs[V86_REG_EFLAGS] &= ~1UL;
        break;

    /* ================================================================ */
    /*  AH=84h: センス拡張                                               */
    /* ================================================================ */
    case 0x84:
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        regs[V86_REG_EFLAGS] &= ~1UL;
        break;

    /* ================================================================ */
    /*  AH=05h: セクタ読み出し (Read)                                    */
    /*                                                                    */
    /*  入力:                                                            */
    /*    BX = 転送バイト数                                              */
    /*    CL = セクタ長コード (3=1024)                                   */
    /*    DH = シリンダ                                                  */
    /*    DL = セクタ (0-based, ヘッド含む)                              */
    /*    ES:BP = 転送先バッファ                                         */
    /*                                                                    */
    /*  出力:                                                            */
    /*    AH = 0 (成功), CF=0                                            */
    /* ================================================================ */
    case 0x05: {
        u16 xfer_bytes = (u16)(regs[V86_REG_EBX] & 0xFFFF);
        u8 cylinder = (u8)((regs[V86_REG_EDX] >> 8) & 0xFF);
        u8 sector_dl = (u8)(regs[V86_REG_EDX] & 0xFF);
        u16 es_val = (u16)(regs[V86_REG_ES] & 0xFFFF);
        u16 bp_val = (u16)(regs[V86_REG_EBP] & 0xFFFF);
        u8 *dst;
        i32 img_offset;
        u32 remaining;
        u32 chunk;

        /* イメージ未設定チェック */
        if (!fdd_image) {
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL) | 0xE000UL;
            regs[V86_REG_EFLAGS] |= 1;
            return 0;
        }

        /* CHS → オフセット変換 */
        img_offset = chs_to_offset(cylinder, sector_dl);
        if (img_offset < 0) {
            /* 範囲外: AH=60h (レコードノットファウンド) */
            regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL) | 0x6000UL;
            regs[V86_REG_EFLAGS] |= 1;
            return 0;
        }

        /* 転送先バッファのリニアアドレス */
        dst = v86_phys_addr(es_val, bp_val);

        /* セクタ単位で転送 (複数セクタ対応) */
        remaining = (u32)xfer_bytes;
        while (remaining > 0 && img_offset >= 0 && (u32)img_offset < fdd_image_size) {
            chunk = remaining;
            if (chunk > V86_FDD_BPS) chunk = V86_FDD_BPS;
            if ((u32)img_offset + chunk > fdd_image_size) {
                chunk = fdd_image_size - (u32)img_offset;
            }
            kmemcpy(dst, fdd_image + img_offset, chunk);
            dst += chunk;
            img_offset += (i32)chunk;
            remaining -= chunk;
        }

        /* 成功 */
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        regs[V86_REG_EFLAGS] &= ~1UL;
        break;
    }

    /* ================================================================ */
    /*  AH=06h: セクタ書き込み (Write)                                   */
    /*  Phase 2: 読み取り専用 — エラー応答                              */
    /* ================================================================ */
    case 0x06:
        /* ライトプロテクト: AH=70h */
        regs[V86_REG_EAX] = (regs[V86_REG_EAX] & 0xFFFF00FFUL) | 0x7000UL;
        regs[V86_REG_EFLAGS] |= 1;
        break;

    /* ================================================================ */
    /*  AH=07h: ベリファイ — 常に成功                                   */
    /* ================================================================ */
    case 0x07:
        regs[V86_REG_EAX] = regs[V86_REG_EAX] & 0xFFFF00FFUL;
        regs[V86_REG_EFLAGS] &= ~1UL;
        break;

    default:
        return -1;
    }

    return 0;
}
