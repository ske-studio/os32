/* ======================================================================== */
/*  PCI_DECODE_HOST.C — drivers/pci_decode.c をそのままホストで回す         */
/*                                                                          */
/*  実物の復号を 1 行も写さずに #include する。pci_decode.c は I/O も        */
/*  静的配列も構造体の padding も触らないので、模型は 1 つも要らない。       */
/*                                                                          */
/*  なぜホストで試すのか: **NP21/W は PCI を実装していない** (`0CF8h` が     */
/*  無い)。走らせて確かめられるのは実機 PC-9821Ra266 だけで、実機の 1 回は   */
/*  高い (シリアル 115200 で会話する)。ビットの読み違いはここで全部潰す。    */
/*                                                                          */
/*  期待値の出どころ (記録: tools/tests/pci_decode_tdd.md):                 */
/*    - `docs/hw/undocumented/io_pci.md` 図2 (76〜109 行) と                */
/*      0CF8h のビット定義 (446〜462 行)                                    */
/*    - Intel 8255x SDM テキスト版 Table 1 (697〜713 行) の Type 0 ヘッダ    */
/* ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/pci_decode.c"

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); failed++; \
} } while (0)

static int failed;

/* ------------------------------------------------------------------ */
/*  (1) アドレス語の組み立て (io_pci.md 図2 / 446〜456 行)              */
/* ------------------------------------------------------------------ */
static void cfg_addr(void)
{
    /* bit31 (CONE) は常に立つ。立てないとメカニズム #1 にならず、
     * 0CF8h はマーキュリー互換の別レジスタとして振る舞う (458 行)。 */
    CHECK((pci_cfg_addr(0, 0, 0, 0) & PCI_CFG_ADDR_ENABLE) != 0);
    CHECK(pci_cfg_addr(0, 0, 0, 0) == 0x80000000UL);

    /* 欄の位置: bus bit23〜16 / dev bit15〜11 / fn bit10〜8 / reg bit7〜2。 */
    CHECK(pci_cfg_addr(0, 1, 0, 0x3C) == 0x8000083CUL);   /* C バスブリッヂ */
    CHECK(pci_cfg_addr(0, 8, 0, 0x10) == 0x80004010UL);   /* PCI スロット #0 */
    CHECK(pci_cfg_addr(1, 0, 0, 0x00) == 0x80010000UL);   /* bus 1 */
    CHECK(pci_cfg_addr(0, 31, 7, 0xFC) == 0x8000FFFCUL);  /* 右端 */

    /* reg の bit1〜0 は落ちる。PCI アドレスの下位 2 ビットは 00b 固定
     * (図2 の末尾)。0x3D (Interrupt Pin) を渡しても 0x3C の DWORD を指す。 */
    CHECK(pci_cfg_addr(0, 0, 0, 0x3D) == 0x8000003CUL);
    CHECK(pci_cfg_addr(0, 0, 0, 0x3F) == 0x8000003CUL);
    CHECK(pci_cfg_addr(0, 0, 0, 0x02) == 0x80000000UL);

    /* 範囲外は**隣の欄を侵さない**。dev は 5 ビットなので 32 は 0 に、
     * fn は 3 ビットなので 8 は 0 に落ちる。マスクを忘れると
     * dev=32 が bus 欄へ、fn=8 が dev 欄へ溢れて**別のデバイスの
     * config を読む** — 列挙の上限を書き間違えた日に静かに壊れる。 */
    CHECK(pci_cfg_addr(0, 32, 0, 0) == 0x80000000UL);
    CHECK(pci_cfg_addr(0, 0, 8, 0) == 0x80000000UL);
    CHECK(pci_cfg_addr(256, 0, 0, 0) == 0x80000000UL);
    /* 溢れが予約ビット (bit30〜24) を汚さないことも見る。 */
    CHECK((pci_cfg_addr(256, 32, 8, 0xFF) & 0x7F000000UL) == 0);
}

/* ------------------------------------------------------------------ */
/*  (2) PCI 有無の判定に使う値                                         */
/* ------------------------------------------------------------------ */
static void probe_values(void)
{
    /* 読み戻しで確かめる以上、**本物のメカニズム #1 が保持できる値**
     * でなければならない。bit1〜0 は 0 に落ちる (図2) ので、そこを
     * 立てた値で探ると PCI があっても「無い」と答える。 */
    CHECK((PCI_MECH1_PROBE_A & 0x3UL) == 0);
    CHECK((PCI_MECH1_PROBE_B & 0x3UL) == 0);
    /* 予約 bit30〜24 も 0 でなければ読み戻しが一致しない。 */
    CHECK((PCI_MECH1_PROBE_A & 0x7F000000UL) == 0);
    CHECK((PCI_MECH1_PROBE_B & 0x7F000000UL) == 0);
    /* どちらも CONE が立っている。 */
    CHECK((PCI_MECH1_PROBE_A & PCI_CFG_ADDR_ENABLE) != 0);
    CHECK((PCI_MECH1_PROBE_B & PCI_CFG_ADDR_ENABLE) != 0);
    /* 2 本が**違う値**であること。同じ値を 2 回書いても区別は増えない。 */
    CHECK(PCI_MECH1_PROBE_A != PCI_MECH1_PROBE_B);
    /* B は書き込める全ビットを使う = 素通しのラッチと本物を見分ける力。
     * A だけだと bit31 しか動かさないので、0CF8h が単なる 32 ビット
     * ラッチの機械 (マーキュリー互換領域) を PCI と誤認し得る。 */
    CHECK(PCI_MECH1_PROBE_B == pci_cfg_addr(0xFF, 31, 7, 0xFC));
}

/* ------------------------------------------------------------------ */
/*  (3) BAR の種別                                                     */
/* ------------------------------------------------------------------ */
static void bar_kind(void)
{
    /* 生値 0 = そのレジスタは実装されていない。 */
    CHECK(pci_bar_kind(0x00000000UL) == PCI_BAR_NONE);

    /* bit0 = 1 は I/O 空間。 */
    CHECK(pci_bar_kind(0x0000E801UL) == PCI_BAR_IO);
    CHECK(pci_bar_kind(0xFFFFFFFFUL) == PCI_BAR_IO);

    /* **番地未割り当ての I/O BAR (生値 1) は NONE ではない。**
     * 票 §5-2 R1 が実機で見たいのは「BIOS が I/O 窓を割り当てたか」で、
     * 0x00000001 は「I/O BAR はあるが番地が 0」= 自分で割り当てが要る。
     * ここを NONE に畳むと、実機の 1 回でその事実が消える。 */
    CHECK(pci_bar_kind(0x00000001UL) == PCI_BAR_IO);

    /* bit0 = 0 はメモリ。bit2〜1 が型。 */
    CHECK(pci_bar_kind(0xF8000000UL) == PCI_BAR_MEM32);  /* 00b */
    CHECK(pci_bar_kind(0x000F0002UL) == PCI_BAR_MEM1M);  /* 01b */
    CHECK(pci_bar_kind(0xF8000004UL) == PCI_BAR_MEM64);  /* 10b */
    CHECK(pci_bar_kind(0xF8000006UL) == PCI_BAR_MEMRSV); /* 11b */
    /* prefetchable (bit3) は型を変えない。 */
    CHECK(pci_bar_kind(0xF8000008UL) == PCI_BAR_MEM32);
    CHECK(pci_bar_kind(0xF800000CUL) == PCI_BAR_MEM64);
}

/* ------------------------------------------------------------------ */
/*  (4) BAR の番地と prefetchable                                      */
/* ------------------------------------------------------------------ */
static void bar_base(void)
{
    /* I/O は下位 2 ビットだけを落とす (~3)。**~0xF で落とすと
     * 0xE808 が 0xE800 になる** — 82557 の I/O 窓は 32 バイト境界に
     * 置かれ得るので、これは実機で外す形の間違い。 */
    CHECK(pci_bar_base(0x0000E801UL) == 0x0000E800UL);
    CHECK(pci_bar_base(0x0000E809UL) == 0x0000E808UL);
    CHECK(pci_bar_base(0x00000001UL) == 0x00000000UL);

    /* メモリは下位 4 ビットを落とす (~0xF)。型と prefetch が混ざる。 */
    CHECK(pci_bar_base(0xF8000000UL) == 0xF8000000UL);
    CHECK(pci_bar_base(0xF8000004UL) == 0xF8000000UL);
    CHECK(pci_bar_base(0xF800000CUL) == 0xF8000000UL);
    CHECK(pci_bar_base(0x000F0002UL) == 0x000F0000UL);

    /* 生値 0 は 0。 */
    CHECK(pci_bar_base(0x00000000UL) == 0);

    /* prefetchable は**メモリ BAR のときだけ** bit3。I/O では bit3 は
     * 番地の一部なので、無条件に bit3 を見ると 0xE808 が
     * 「prefetchable な I/O」という存在しないものに化ける。 */
    CHECK(pci_bar_prefetchable(0xF8000008UL) == 1);
    CHECK(pci_bar_prefetchable(0xF800000CUL) == 1);
    CHECK(pci_bar_prefetchable(0xF8000000UL) == 0);
    CHECK(pci_bar_prefetchable(0x0000E809UL) == 0);
    CHECK(pci_bar_prefetchable(0x0000E80DUL) == 0);
    CHECK(pci_bar_prefetchable(0x00000000UL) == 0);
}

/* ------------------------------------------------------------------ */
/*  (5) Header Type                                                    */
/* ------------------------------------------------------------------ */
static void header_type(void)
{
    /* bit7 = マルチファンクション。fn0 だけを見て 1〜7 を飛ばす判定に使う。 */
    CHECK(pci_is_multifunction(0x00) == 0);
    CHECK(pci_is_multifunction(0x01) == 0);
    CHECK(pci_is_multifunction(0x80) == 1);
    CHECK(pci_is_multifunction(0x81) == 1);

    /* bit6〜0 = レイアウト。**bit7 を落とさずに比較すると、
     * マルチファンクションのブリッヂ (0x81) が Type 0 扱いになり、
     * secondary bus を読まずに配下のバスを丸ごと見落とす。** */
    CHECK(pci_header_layout(0x00) == PCI_HDR_LAYOUT_DEVICE);
    CHECK(pci_header_layout(0x80) == PCI_HDR_LAYOUT_DEVICE);
    CHECK(pci_header_layout(0x01) == PCI_HDR_LAYOUT_BRIDGE);
    CHECK(pci_header_layout(0x81) == PCI_HDR_LAYOUT_BRIDGE);
    CHECK(pci_header_layout(0x82) == PCI_HDR_LAYOUT_CARDBUS);
    /* 不在の読み (0xFF) がブリッヂに見えないこと — 0xFF & 0x7F = 0x7F。 */
    CHECK(pci_header_layout(0xFF) != PCI_HDR_LAYOUT_BRIDGE);
}

/* ------------------------------------------------------------------ */
/*  (6) DWORD からの切り出し                                           */
/* ------------------------------------------------------------------ */
static void extract(void)
{
    /* 0CF8h は DWORD 必須 (io_pci.md 456 行) なので、8/16 ビットの
     * 読みも DWORD 1 回から切り出す。レーンは reg の下位 2 ビット。 */
    u32 d = 0x12345678UL;
    CHECK(pci_extract8(d, 0x00) == 0x78);
    CHECK(pci_extract8(d, 0x01) == 0x56);
    CHECK(pci_extract8(d, 0x02) == 0x34);
    CHECK(pci_extract8(d, 0x03) == 0x12);
    /* オフセットがそろっていなくても同じレーンを指す (0x3D → bit15〜8)。 */
    CHECK(pci_extract8(d, 0x3D) == 0x56);
    CHECK(pci_extract8(d, 0x3C) == 0x78);
    CHECK(pci_extract8(d, 0x0B) == 0x12);

    CHECK(pci_extract16(d, 0x00) == 0x5678);
    CHECK(pci_extract16(d, 0x02) == 0x1234);
    /* u16 の欄は PCI では必ず偶数オフセットに置かれ、**バイト境界を
     * またがない**。奇数を渡されても 16 ビットのレーンへ丸める
     * (bit ずれた値を返すより、隣の欄を返す方が表示で気づける)。 */
    CHECK(pci_extract16(d, 0x01) == 0x5678);
    CHECK(pci_extract16(d, 0x03) == 0x1234);
    /* Vendor ID (0x00) と Device ID (0x02) が同じ DWORD から取れる。 */
    CHECK(pci_extract16(0x12298086UL, PCI_CFG_VENDOR_ID) == PCI_VENDOR_INTEL);
    CHECK(pci_extract16(0x12298086UL, PCI_CFG_DEVICE_ID) == PCI_DEVICE_82557);
}

/* ------------------------------------------------------------------ */
/*  (7) 名前引き                                                       */
/* ------------------------------------------------------------------ */
static void names(void)
{
    CHECK(!strcmp(pci_class_name(0x02, 0x00), "Network/Ethernet"));
    CHECK(!strcmp(pci_class_name(0x06, 0x00), "Bridge/Host"));
    CHECK(!strcmp(pci_class_name(0x06, 0x01), "Bridge/ISA"));
    CHECK(!strcmp(pci_class_name(0x06, 0x04), "Bridge/PCI"));
    CHECK(!strcmp(pci_class_name(0x03, 0x00), "Display/VGA"));
    CHECK(!strcmp(pci_class_name(0x01, 0x01), "Storage/IDE"));
    /* サブクラスが違っても親クラスは分かる。 */
    CHECK(!strcmp(pci_class_name(0x02, 0x80), "Network"));
    CHECK(!strcmp(pci_class_name(0x06, 0x80), "Bridge"));
    /* 知らない組み合わせは空欄にしない (欄がずれる)。 */
    CHECK(!strcmp(pci_class_name(0x7F, 0x00), "Unknown"));
    CHECK(pci_class_name(0xFF, 0xFF)[0] != '\0');

    /* io_pci.md 271〜280 行 / 336〜345 行の表にあるベンダ。 */
    CHECK(!strcmp(pci_vendor_name(0x8086), "Intel"));
    CHECK(!strcmp(pci_vendor_name(0x1033), "NEC"));
    CHECK(!strcmp(pci_vendor_name(0x102B), "Matrox"));
    CHECK(!strcmp(pci_vendor_name(0x9004), "Adaptec"));
    CHECK(!strcmp(pci_vendor_name(0x1023), "Trident"));
    /* 知らないベンダは空文字 — 行には vendor:device の 16 進が既に出る。 */
    CHECK(pci_vendor_name(0x1234)[0] == '\0');
    /* 不在 (0xFFFF) に名前を付けない。 */
    CHECK(pci_vendor_name(PCI_VENDOR_NONE)[0] == '\0');
}

/* ------------------------------------------------------------------ */
/*  (8) 実機で見るはずの姿 — 82557 の config を丸ごと復号する           */
/*                                                                    */
/*  8255x SDM Table 1 (697〜713 行) の並び。BAR は 0x10 がメモリ、      */
/*  0x14 が I/O、0x18 がフラッシュ (票の例示とは順が逆 — **SDM が正**)。 */
/*  値は「BIOS が割り当て済み」の想定。実機で違ったらここを直す。       */
/* ------------------------------------------------------------------ */
static void story_82557(void)
{
    u32 cfg[16];
    u16 vendor, device, command;
    u8 cls, sub, progif, hdr, line, pin;
    int i;

    for (i = 0; i < 16; i++) cfg[i] = 0;
    cfg[0x00 / 4] = 0x12298086UL;   /* Device ID : Vendor ID */
    cfg[0x04 / 4] = 0x02800007UL;   /* Status : Command (IO+MEM+BusMaster) */
    cfg[0x08 / 4] = 0x02000001UL;   /* Class 200000h : Revision 01 */
    cfg[0x0C / 4] = 0x00002008UL;   /* BIST:Header:Latency:CacheLine */
    cfg[0x10 / 4] = 0xF8000000UL;   /* CSR memory mapped */
    cfg[0x14 / 4] = 0x0000E801UL;   /* CSR I/O mapped */
    cfg[0x18 / 4] = 0xF8100000UL;   /* Flash */
    cfg[0x3C / 4] = 0x2814010BUL;   /* MaxLat:MinGnt:IntPin(01h):IntLine(0Bh) */

    vendor = pci_extract16(cfg[PCI_CFG_VENDOR_ID / 4], PCI_CFG_VENDOR_ID);
    device = pci_extract16(cfg[PCI_CFG_DEVICE_ID / 4], PCI_CFG_DEVICE_ID);
    CHECK(vendor == PCI_VENDOR_INTEL);
    CHECK(device == PCI_DEVICE_82557);
    CHECK(!strcmp(pci_vendor_name(vendor), "Intel"));

    command = pci_extract16(cfg[PCI_CFG_COMMAND / 4], PCI_CFG_COMMAND);
    /* 票 §5-2 R2: I/O Enable と Bus Master が立っているか。 */
    CHECK((command & PCI_CMD_IO_ENABLE) != 0);
    CHECK((command & PCI_CMD_BUS_MASTER) != 0);

    progif = pci_extract8(cfg[PCI_CFG_PROG_IF / 4], PCI_CFG_PROG_IF);
    sub    = pci_extract8(cfg[PCI_CFG_SUBCLASS / 4], PCI_CFG_SUBCLASS);
    cls    = pci_extract8(cfg[PCI_CFG_CLASS / 4], PCI_CFG_CLASS);
    CHECK(cls == PCI_CLASS_NETWORK);
    CHECK(sub == PCI_SUB_NET_ETHERNET);
    CHECK(progif == 0x00);
    CHECK(!strcmp(pci_class_name(cls, sub), "Network/Ethernet"));

    hdr = pci_extract8(cfg[PCI_CFG_HEADER_TYPE / 4], PCI_CFG_HEADER_TYPE);
    CHECK(pci_is_multifunction(hdr) == 0);
    CHECK(pci_header_layout(hdr) == PCI_HDR_LAYOUT_DEVICE);

    /* BAR: 0x10 メモリ / 0x14 I/O / 0x18 メモリ、残りは未実装。 */
    CHECK(pci_bar_kind(cfg[0x10 / 4]) == PCI_BAR_MEM32);
    CHECK(pci_bar_base(cfg[0x10 / 4]) == 0xF8000000UL);
    CHECK(pci_bar_kind(cfg[0x14 / 4]) == PCI_BAR_IO);
    CHECK(pci_bar_base(cfg[0x14 / 4]) == 0x0000E800UL);
    CHECK(pci_bar_kind(cfg[0x18 / 4]) == PCI_BAR_MEM32);
    CHECK(pci_bar_kind(cfg[0x1C / 4]) == PCI_BAR_NONE);
    CHECK(pci_bar_kind(cfg[0x24 / 4]) == PCI_BAR_NONE);

    /* Interrupt Line / Pin (SDM 4.1.15 / 4.1.16)。PC-98 では PIRQ0〜3 が
     * C バスブリッヂ経由で 8259 に配られ、その結果がここに入る
     * (io_pci.md 315〜320 行)。82557 の Pin は常に 1 = INTA#。 */
    line = pci_extract8(cfg[PCI_CFG_INT_LINE / 4], PCI_CFG_INT_LINE);
    pin  = pci_extract8(cfg[PCI_CFG_INT_PIN / 4], PCI_CFG_INT_PIN);
    CHECK(line == 11);
    CHECK(pin == 1);

    /* この 1 行がアドレス語になって 0CF8h へ出る。 */
    CHECK(pci_cfg_addr(0, 11, 0, PCI_CFG_INT_LINE) == 0x8000583CUL);
}

/* ------------------------------------------------------------------ */
/*  (9) 不在の読み — PCI が居ない / そのスロットが空                    */
/* ------------------------------------------------------------------ */
static void absent(void)
{
    /* 応答の無いバスサイクルは全ビット 1。vendor が 0xFFFF なら飛ばす。 */
    CHECK(pci_extract16(0xFFFFFFFFUL, PCI_CFG_VENDOR_ID) == PCI_VENDOR_NONE);
    CHECK(pci_extract16(0xFFFFFFFFUL, PCI_CFG_DEVICE_ID) == PCI_VENDOR_NONE);
    /* 0x0000FFFF (vendor だけ 0xFFFF) も不在。**DWORD 全体を
     * 0xFFFFFFFF と比べる判定は、device 側にゴミが乗った機械で
     * 空スロットを拾う。** */
    CHECK(pci_extract16(0x0000FFFFUL, PCI_CFG_VENDOR_ID) == PCI_VENDOR_NONE);
    /* vendor 0 も実在しない (PCI 規格)。ただし列挙は 0xFFFF だけを見る
     * ので、ここでは「0xFFFF と 0 は別物」であることだけ確かめる。 */
    CHECK(pci_extract16(0x00000000UL, PCI_CFG_VENDOR_ID) != PCI_VENDOR_NONE);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    failed = 0;
    if (!strcmp(argv[1], "cfg_addr")) cfg_addr();
    else if (!strcmp(argv[1], "probe_values")) probe_values();
    else if (!strcmp(argv[1], "bar_kind")) bar_kind();
    else if (!strcmp(argv[1], "bar_base")) bar_base();
    else if (!strcmp(argv[1], "header_type")) header_type();
    else if (!strcmp(argv[1], "extract")) extract();
    else if (!strcmp(argv[1], "names")) names();
    else if (!strcmp(argv[1], "story_82557")) story_82557();
    else if (!strcmp(argv[1], "absent")) absent();
    else return 2;
    if (failed) return 1;
    printf("PASS %s\n", argv[1]);
    return 0;
}
