/* ======================================================================== */
/*  FDC.H — PC-98 フロッピーディスクコントローラ直接制御ドライバ            */
/*                                                                          */
/*  µPD765A (1MB FDC) + µPD8237A (DMA ch2) を                               */
/*  32ビットプロテクトモードから I/Oポート経由で直接制御する。               */
/*  BIOS呼び出し (INT 1Bh) やリアルモード遷移は一切不要。                   */
/*                                                                          */
/*  出典: PC9800Bible §2-9, §1-5, §4-3 / OSDev Wiki FDC                    */
/* ======================================================================== */

#ifndef FDC_H
#define FDC_H

#include "types.h"       /* u8, u16, u32 型定義 */
#include "fdc_decide.h"  /* ST0 のビット定義と純粋な判定 (ホストで試験する) */

/* ======================================================================== */
/*  µPD765A FDC I/Oポート (PC-98 1MB FDD)                                  */
/* ======================================================================== */
#define FDC_MSR     0x90    /* R:  メインステータスレジスタ */
#define FDC_FIFO    0x92    /* RW: データレジスタ (コマンド/リザルト/データ) */
#define FDC_CTRL    0x94    /* W:  コントロールレジスタ */
                            /* R:  リードスイッチ (FDDタイプ) */

/* --- MSRビットフラグ --- */
#define MSR_RQM     0x80    /* Request for Master: FDC準備完了 */
#define MSR_DIO     0x40    /* Data I/O direction: 1=FDC→CPU, 0=CPU→FDC */
#define MSR_NDMA    0x20    /* Non-DMA実行フェーズ中 */
#define MSR_BUSY    0x10    /* FDCビジー (コマンド実行中) */
#define MSR_ACTD    0x08    /* ドライブD アクティブ */
#define MSR_ACTC    0x04    /* ドライブC アクティブ */
#define MSR_ACTB    0x02    /* ドライブB アクティブ */
#define MSR_ACTA    0x01    /* ドライブA アクティブ */

/* --- コントロールレジスタ (0x94) ビット --- */
/* PC-98固有: FDDコントローラのリセットやモーター制御 */
#define CTRL_MTON   0x08    /* モーターON */
#define CTRL_DMAE   0x10    /* DMA有効 */
#define CTRL_RST    0x80    /* FDCリセット */

/* ======================================================================== */
/*  µPD765A コマンドコード                                                  */
/* ======================================================================== */
#define FDC_CMD_SPECIFY         0x03
#define FDC_CMD_SENSE_DRIVE     0x04
#define FDC_CMD_WRITE_DATA      0x05    /* + MF + MT */
#define FDC_CMD_READ_DATA       0x06    /* + MF + MT */
#define FDC_CMD_RECALIBRATE     0x07
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_READ_ID         0x0A    /* + MF */
#define FDC_CMD_FORMAT_TRACK    0x0D    /* + MF */
#define FDC_CMD_SEEK            0x0F

/* コマンドオプションビット */
#define FDC_OPT_MT    0x80    /* マルチトラック */
#define FDC_OPT_MF    0x40    /* MFM (倍密度) — 常にセット */
#define FDC_OPT_SK    0x20    /* Skip deleted sectors */

/* ======================================================================== */
/*  SPECIFY のパラメータ (値は据え置き。2 か所から使うので名前を付ける)     */
/* ======================================================================== */
/*  SRT = 16 - (8ms × 500kHz / 500000) = 8、HUT = 0 (最大)                  */
#define FDC_SPECIFY_SRT_HUT   0x80
/*  HLT = 10ms × 500kHz / 1000000 = 5、NDMA = 0 (DMA モード)                */
#define FDC_SPECIFY_HLT_DMA   0x0A

/* ======================================================================== */
/*  DMA (µPD8237A) チャネル2 I/Oポート (PC-98)                             */
/* ======================================================================== */
#define DMA_CH2_ADDR   0x09    /* チャネル2 アドレス (Low/High) */
#define DMA_CH2_COUNT  0x0B    /* チャネル2 ワードカウント (Low/High) */
#define DMA_CH2_BANK   0x23    /* チャネル2 バンクレジスタ */
#define DMA_MASK_REG   0x15    /* シングルマスクレジスタ */
#define DMA_MODE_REG   0x17    /* モードレジスタ */
#define DMA_FLIPFLOP   0x19    /* バイトポインタ・フリップフロップ・クリア */

/* DMAモード値 */
#define DMA_MODE_READ  0x46    /* ch2, single, addr++, read (FDC→メモリ) */
#define DMA_MODE_WRITE 0x4A    /* ch2, single, addr++, write (メモリ→FDC) */
#define DMA_MASK_CH2   0x06    /* ch2をマスク (無効化) */
#define DMA_UNMASK_CH2 0x02    /* ch2をアンマスク (有効化) */

/* ======================================================================== */
/*  I/O 0439h — DMA アクセス制御 (Undocumented)                             */
/*                                                                          */
/*  正典: docs/hw/undocumented/io_dma.md の「I/O 0439h DMAアクセス制御等」。 */
/*  対象は 80286 以上の機種 (PC-98XA を除く)。                              */
/*                                                                          */
/*    bit 2: DMA アドレスマスクレジスタ                                     */
/*           1 = 1M バイト以上のアドレスへの DMA アクセス**禁止**           */
/*           0 = 許可                                                       */
/*           **ノーマルモードの起動時設定は 1**(ハイレゾは 0)。             */
/*                                                                          */
/*  OS32 の dma_buffer はカーネル BSS (1MB 超) に置かれるので、立ったまま    */
/*  だと READ DATA が正常終了してもデータがバッファに届かない。             */
/*                                                                          */
/*  **必ず read-modify-write で他のビットを保つこと。** bit7 はプリンタ I/F  */
/*  選択で、0 を書くと本体内蔵プリンタインタフェースが切り離される機種が    */
/*  ある (PC-9821Ne/Bf/Bp/Bs/Be/Xt/Xa/Xn/Xp/Xs/Xe、PC-9801BA2/BS2/BX2 等)。 */
/* ======================================================================== */
#define SYSPORT_DMA_CTRL       0x0439
#define SYSPORT_DMA_MASK_1MB   0x04   /* bit2: 1 = 1MB 超への DMA 禁止 */

/*  **読みが FFh でも書く。** 一度は「ポートが浮いている印」として FFh を
 *  避ける案を採ったが、実機は bit7 = 1 (内蔵プリンタ)・bit2 = 1 (起動時
 *  設定)・「未使用(?)」の bit6/bit3 が 1 で読めれば **正当に FFh を返し得る**。
 *  そこで書かないと DMA 禁止が残ったままで、直したい root panic が直らない。
 *  RMW なら FFh → FBh で bit7 は 1 のまま保たれるので、プリンタ I/F を
 *  切り離す危険も無い。0439h を持たない機種 (8086/V30) は OS32 の対象外
 *  (386 以上が前提)。                                                      */

/* ======================================================================== */
/*  割り込み                                                                */
/* ======================================================================== */
/* PC-98: 2HD FDD = スレーブ IR11 → PIC_SLAVE_OFFSET + 3 = 0x2B */
#define FDC_IRQ     11    /* スレーブPIC IR11 */

/* ======================================================================== */
/*  ディスクパラメータ (PC-98 2HD 1MB MFM) — 互換マクロ                    */
/* ======================================================================== */
#define FDC_CYLINDERS    77     /* シリンダ数 (0-76) */
#define FDC_HEADS        2      /* ヘッド数 */
#define FDC_SPT          8      /* セクタ/トラック */
#define FDC_SECTOR_SIZE  1024   /* バイト/セクタ (N=3) */
#define FDC_SECTOR_N     3      /* セクタ長コード (3=1024) */
#define FDC_GAP3         0x74   /* Read/Write ギャップ長 (MFM 1024byte/sec) */
#define FDC_TOTAL_SECTORS (FDC_CYLINDERS * FDC_HEADS * FDC_SPT)  /* 1232 */
#define FDC_TIMEOUT_LOOP 10000  /* BSY等待ちのためのループカウンタ上限 */

/* ======================================================================== */
/*  IRQ11 待ちタイムアウト (tick 単位。PIT_HZ = 100 なので 1 tick = 10ms)    */
/*                                                                          */
/*  ここは長いあいだ 20 tick (200ms) の一本値だった。あれは **NP21/W に      */
/*  合わせた値**。エミュレータはシークの所要を **シリンダ距離に依らず固定** */
/*  にしている — np21w-src/src/io/fdc.c の FDC_Seek / FDC_Recalibrate は    */
/*  fdc.int_timer[us] に FDC_INT_DELAY (= 6、pccore_exec のフレーム単位で    */
/*  約 100ms) を置くだけで、fdc_intdelay() がそれを 0 まで数えてから         */
/*  fdc_interrupt() を呼び、そこから 512 サイクル後に IRQ を予約する。       */
/*  **実機のシーク時間に比べれば即時**なので 200ms でも足りていた。         */
/*  **実機 PC-9821Ra266 では足りず**、FD 起動が MOUNT... の root panic に    */
/*  なっていた (2026-09-22)。                                               */
/*  用途ごとに実機の機構から引き直す ([C4]: 根拠を数字の隣に置く)。         */
/* ======================================================================== */

/* SEEK / RECALIBRATE。
 *   SPECIFY の SRT は 8ms (下の 0x80)。RECALIBRATE は 1 回で最大 77 ステップ、
 *   SEEK は 80 シリンダ媒体で最大 79 トラック分踏む。
 *     8ms × 80 トラック = 640ms  + ヘッドセトリング (約 15ms) ≒ 655ms
 *   余裕 2 倍で 1.5 秒。
 *   ローダが VMKRNL.LZ4 を読んだ直後のヘッドはシリンダ 20〜40 付近に居るので
 *   (20 × 8ms = 160ms、40 × 8ms = 320ms)、fdc_init() の RECALIBRATE は
 *   ここが 200ms だと頻繁にタイムアウトし、そのあと未回収の割り込みが残った。 */
#define FDC_SEEK_TIMEOUT_TICKS    150

/* READ DATA / WRITE DATA。
 *   目的セクタが直前に通過していると最大 2 回転待つ。300rpm (1.44MB) なら
 *   1 回転 200ms で最悪 400ms、360rpm (2HD 1232KB) なら 167ms で 334ms。
 *   これに HLT 10ms のヘッドロードと 1 セクタの転送を足して約 420ms。
 *   余裕 2 倍で 1 秒。 */
#define FDC_RW_TIMEOUT_TICKS      100

/* リセット完了の割り込み待ち。**ここは短くする**。
 *   - シークを伴わないので、実機ならリセット後の Ready-change 通知は
 *     µs〜ms のオーダーで来る。
 *   - 来なくても困らない。直後の SIS 排水が同じ通知を回収するので、
 *     待ちは「来れば早く進む」ための上乗せでしかない。
 *   - **NP21/W はこの経路で IRQ を出さない**。fdc_o94 のリセット処理は
 *     `dat & 0x08` (モーター ON) を同時に書いたときしか割り込みを積まず、
 *     こちらは 0x80 (RST だけ) を書くため。ここを長くすると、媒体の無い
 *     ドライブを読むたびに丸ごと空待ちする。
 * 100ms。 */
#define FDC_RESET_TIMEOUT_TICKS   10

/* 旧名。**エミュレータに合わせた 200ms の一本値**で、実機のシークには
 * 足りなかった。残してあるのは外部の参照を壊さないためだけで、新しい
 * コードは用途別の定数を使うこと。 */
#define FDC_IRQ_TIMEOUT_TICKS  FDC_RW_TIMEOUT_TICKS

/* ======================================================================== */
/*  回数の上限 (どれも無限ループを作らないための縛り)                       */
/* ======================================================================== */

/* SENSE INTERRUPT STATUS の排水回数。µPD765A は 4 ドライブ分の完了通知を
 * 溜められるので 4 回で必ず尽きる。尽きたかどうかは ST0 = 80h で判る。 */
#define FDC_SIS_DRAIN_MAX      4

/* RECALIBRATE を出す回数。1 回で 77 ステップ踏めるので、80 シリンダ媒体の
 * いちばん奥 (シリンダ 79) からでも 2 回でトラック 0 に届く。 */
#define FDC_RECAL_ATTEMPTS     2

/* READ / WRITE DATA のリトライ回数。 */
#define FDC_RW_RETRIES         3

/* MSR が次のリザルトバイトを出すまで待つループ回数。1 周が fdc_delay()
 * 2 回 (数 µs) なので、200 周でも 1ms に満たない。リザルトフェーズの
 * バイト間隔は µs のオーダーなので十分。
 * fdc_read_results() の「CB=1 のまま RQM/DIO が揃わない」空回りにも
 * **同じ上限を掛ける** — 縛りが無いと壊れた FDC で無限に回る。 */
#define FDC_MSR_SETTLE_LOOP    200

/* ======================================================================== */
/*  DMA バッファの配置 ([HW2])                                              */
/* ======================================================================== */

/* µPD8237A はアドレスの下位 16 ビットしか回さない。64KB 境界をまたぐ転送は
 * バンクの先頭へ巻き戻って別の番地を壊す。
 * **1024B 境界に揃えた 1024B は定義上 64KB 境界をまたげない** ので、
 * 揃え指定で塞ぐ。番地がリンク順に依存しているのをやめるため。 */
#define FDC_DMA_ALIGN      1024
#define FDC_DMA_BANK_SIZE  0x10000   /* DMA バンク = 64KB */
#define FDC_DMA_BANK_MASK  0xFFFF    /* バンク内オフセットの取り出し */

/* ======================================================================== */
/*  メディア種別 (FDI/実FDDのジオメトリ選択に使用)                          */
/* ======================================================================== */
typedef enum {
    FDC_MEDIA_2HD_1232 = 0,  /* 1.2MB (PC-98標準, 77×2×8×1024) */
    FDC_MEDIA_2DD_640  = 1,  /* 640KB (80×2×8×512) */
    FDC_MEDIA_2DD_720  = 2,  /* 720KB (80×2×9×512) */
    FDC_MEDIA_2D_256   = 3,  /* 2D (77×2×16×256) — 古いPC-98ゲーム用 */
    FDC_MEDIA_2HD_1440 = 4   /* 1.44MB (80×2×18×512) — DA/UA 0x30 系 */
} fdc_media_t;

/* メディアジオメトリ構造体 */
struct fdc_geom {
    u8  cyls;       /* シリンダ数 */
    u8  heads;      /* ヘッド数 */
    u8  spt;        /* セクタ/トラック */
    u8  sec_n;      /* セクタ長コード (2=512B, 3=1024B) */
    u16 bps;        /* バイト/セクタ */
    u8  gap3;       /* GAP3長 (R/W用) */
    u8  daua_high;  /* DA/UA上位ニブル (0x90/0x10/0x70) */
};

/* 既知メディア定義 (drivers/fdc.c で実体化) */
extern const struct fdc_geom fdc_geom_2hd;
extern const struct fdc_geom fdc_geom_2dd_640;
extern const struct fdc_geom fdc_geom_2dd_720;
extern const struct fdc_geom fdc_geom_2d_256;
extern const struct fdc_geom fdc_geom_144;

/* ======================================================================== */
/*  ドライブごとの現在ジオメトリ                                            */
/*                                                                          */
/*  上の FDC_* マクロは 2HD 1232KB の値で、**互換のために残してある**。      */
/*  実際に読み書きする側は必ず fdc_get_geom() を通すこと — マクロを直に     */
/*  使うと 1.44MB のディスクを 1024B/8セクタとして読んでゴミを掴む          */
/*  (2026-09-18 に MOUNT root panic として実測)。                           */
/* ======================================================================== */

/* ======================================================================== */
/*  3モードFD I/F 制御 (I/O 04BEh、Undocumented)                            */
/*                                                                          */
/*  正典: docs/hw/undocumented/io_fdd.md の「I/O 04BEh 3モードFD I/F制御」。 */
/*  1.44MB アクセスは **1MB I/F モードのときだけ**可能で、640KB I/F モード   */
/*  ではどのドライブも不可を示す。                                          */
/*                                                                          */
/*  資料の注意 2 つ:                                                        */
/*   - **リードの前に必ずライトしてドライブを指定する。**                    */
/*   - **読んだ値が FFh かどうかで搭載を判断してはいけない** —               */
/*     I/O 00BEh のデコードイメージが 04BEh に出る機種がある。              */
/* ======================================================================== */
#define FDC_IO_3MODE        0x04BE

/* WRITE */
#define FDC_3M_DRV_SHIFT    5       /* bit6,5: ドライブ指定 (00b=1台目) */
#define FDC_3M_APPLY        0x10    /* bit4=1: bit0 のモード指定を有効にする */
#define FDC_3M_MODE_144     0x01    /* bit0=1: 1.44MB アクセスモード */

/* READ */
#define FDC_3M_CAP_144      0x10    /* bit4: 1 = 1.44MB アクセス可能 */
#define FDC_3M_CUR_144      0x01    /* bit0: 1 = いま 1.44MB アクセスモード */

/* drv のアクセスモードを切り替える。on!=0 で 1.44MB、0 で 1MB/640KB。
 * 0 = 読み戻しで要求どおりになった / -1 = ならなかった。
 *
 * **搭載判定には使えない** (上の注意)。呼ぶのは「1.44MB の I/F が居ると
 * 分かっているとき」だけにする — 起動した DA/UA が 0x30 系なら BIOS が
 * その I/F を使った証拠になる。 */
int fdc_set_3mode(int drv, int on);

/* drv のいま選ばれているジオメトリ。既定は 2HD 1232KB。 */
const struct fdc_geom *fdc_get_geom(int drv);

/* drv のメディアを選ぶ。0 で成功、-1 で未知の種別。
 * **dev_init() より前に呼ぶこと** — デバイス記述子がここから値を取る。 */
int fdc_set_media(int drv, fdc_media_t media);

/* ブートローダから渡された DA/UA でメディアを選ぶ (0x30 系 = 1.44MB)。
 * 対応する種別が無ければ 2HD のまま 0 を返す (既存構成を壊さない)。 */
int fdc_set_media_by_daua(int drv, u32 daua);

/* ======================================================================== */
/*  FDCドライバAPI                                                          */
/* ======================================================================== */

/* FDC初期化: リセット → Specify → Recalibrate */
int fdc_init(void);

/* セクタ読み込み (2HD固定ラッパ)
 *   drv:   ドライブ (0-3)
 *   cyl:   シリンダ (0-76)
 *   head:  ヘッド (0-1)
 *   sect:  セクタ (1ベース)
 *   buf:   データバッファ (>= FDC_SECTOR_SIZE バイト)
 * 戻り値: 0=成功
 */
int fdc_read_sector(int drv, int cyl, int head, int sect, void *buf);

/* セクタ書き込み (2HD固定ラッパ) */
int fdc_write_sector(int drv, int cyl, int head, int sect, const void *buf);

/* セクタ読み込み (ジオメトリ指定版) */
int fdc_read_sector_geom(int drv, int cyl, int head, int sect,
                         const struct fdc_geom *g, void *buf);

/* セクタ書き込み (ジオメトリ指定版) */
int fdc_write_sector_geom(int drv, int cyl, int head, int sect,
                          const struct fdc_geom *g, const void *buf);

/* IRQ11完了フラグ (isr_handlers.cからセット) */
extern volatile u32 fdc_irq_fired;

#endif /* FDC_H */

