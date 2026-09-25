/* ======================================================================== */
/*  PCI_VERBOSE.C — `lspci -v` の行づくり (純粋関数)                        */
/*                                                                          */
/*  設計と使い方は pci_verbose.h の冒頭。ここには副作用のある行を置かない   */
/*  (I/O も静的な可変状態も無い)。復号は drivers/pci_decode.c を使う —     */
/*  BAR の種別と番地、クラス名、Header Type の切り出しをカーネルと          */
/*  食い違わせないため。libc も使わない (常駐シェルに printf 系を持ち込ま   */
/*  ない)。                                                                 */
/* ======================================================================== */

#include "pci_verbose.h"
#include "drivers/pci_decode.h"

/* 行の番号 (idx)。BAR は PV_BAR0 + n。 */
#define PV_ID        0
#define PV_REV       1
#define PV_CLASS     2
#define PV_HEADER    3
#define PV_COMMAND   4
#define PV_STATUS    5
#define PV_SUBSYS    6   /* Type 0 = Subsystem、Type 1 = バス番号 */
#define PV_BAR0      7
#define PV_IRQ       (PV_BAR0 + PCI_CFG_BAR_COUNT)

/* Type 1 (PCI-PCI ブリッヂ) の BAR は 0x10 と 0x14 の 2 本だけ。0x18 から
 * 先はバス番号やウィンドウで、BAR として読むと嘘になる。 */
#define PCI_BRIDGE_BAR_COUNT  2

/* Interrupt Line の「未割り当て」(PC/AT 互換 BIOS の慣習)。 */
#define PCI_INT_LINE_NONE     0xFF

/* ------------------------------------------------------------------ */
/*  小さな追記器 (libc 無し)。溢れたら黙って切る — 常に NUL 終端。     */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buf;
    int   cap;
    int   len;
} PvOut;

static void pv_ch(PvOut *o, char c)
{
    if (o->len + 1 < o->cap) {
        o->buf[o->len++] = c;
        o->buf[o->len] = '\0';
    }
}

static void pv_str(PvOut *o, const char *s)
{
    while (*s) pv_ch(o, *s++);
}

/* 16 進を固定桁で (小文字、既存の `lspci` の %04x / %02x と同じ)。 */
static void pv_hex(PvOut *o, u32 v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--)
        pv_ch(o, hx[(v >> (i * 4)) & 0xF]);
}

static void pv_dec(PvOut *o, u32 v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < (int)sizeof(tmp));
    while (n > 0) pv_ch(o, tmp[--n]);
}

/* 名前付きのビット 1 つ: " I/O+" / " I/O-"。Linux の lspci -vv と同じ形。 */
static void pv_flag(PvOut *o, const char *name, u32 word, u32 bit)
{
    pv_ch(o, ' ');
    pv_str(o, name);
    pv_ch(o, (word & bit) ? '+' : '-');
}

/* ------------------------------------------------------------------ */
/*  BAR の欄の数と、各欄の役割                                         */
/* ------------------------------------------------------------------ */
static int pv_bar_count(u8 header)
{
    int layout = pci_header_layout(header);
    if (layout == PCI_HDR_LAYOUT_DEVICE) return PCI_CFG_BAR_COUNT;
    if (layout == PCI_HDR_LAYOUT_BRIDGE) return PCI_BRIDGE_BAR_COUNT;
    /* CardBus (0x10 はソケットの窓で BAR ではない) と未知のレイアウト。 */
    return 0;
}

/* bar[n] が 64 ビットメモリ BAR の**上位半分**か。先頭から歩いて決める
 * (前の欄の型を 1 つだけ見ると、MEM64 の上位がたまたま下位と同じ形の
 * 値のとき、さらに次の欄まで「上位」に化ける)。 */
static int pv_bar_is_mem64_hi(const u32 *bar, int count, int n)
{
    int j = 0;
    while (j < count) {
        if (pci_bar_kind(bar[j]) == PCI_BAR_MEM64 && j + 1 < count) {
            if (j + 1 == n) return 1;
            j += 2;
        } else {
            j += 1;
        }
    }
    return 0;
}

static void pv_bar_line(PvOut *o, const u32 *cfg, int count, int n)
{
    const u32 *bar = &cfg[PCI_CFG_BAR0 / 4];
    u32 raw = bar[n];
    int kind;

    pv_str(o, "  bar");
    pv_dec(o, (u32)n);
    pv_ch(o, ' ');
    pv_hex(o, raw, 8);          /* 生の 32 ビット値は必ず出す */
    pv_ch(o, ' ');

    if (pv_bar_is_mem64_hi(bar, count, n)) {
        /* 上位 32 ビット。PC-98 (32 ビットの機械) では 0 のはず。 */
        pv_str(o, "mem64-hi (bar");
        pv_dec(o, (u32)(n - 1));
        pv_str(o, raw ? " above 4G)" : ")");
        return;
    }

    kind = pci_bar_kind(raw);
    switch (kind) {
    case PCI_BAR_NONE:
        pv_str(o, "none");
        return;
    case PCI_BAR_IO:
        pv_str(o, "io");
        break;
    case PCI_BAR_MEM32:
        pv_str(o, "mem32");
        break;
    case PCI_BAR_MEM1M:
        pv_str(o, "mem1m");
        break;
    case PCI_BAR_MEM64:
        /* 最後の欄の MEM64 は上位の欄が無い (壊れた/未知の装置)。 */
        pv_str(o, (n + 1 < count) ? "mem64-lo" : "mem64-lo (no hi)");
        break;
    default:
        pv_str(o, "mem-rsv");
        break;
    }
    pv_str(o, " base 0x");
    pv_hex(o, pci_bar_base(raw), 8);
    if (pci_bar_prefetchable(raw)) pv_str(o, " prefetch");
    /* 番地 0 = BIOS が割り当てていない (大きさは読まないので出さない)。 */
    if (pci_bar_base(raw) == 0) pv_str(o, " unassigned");
}

/* ------------------------------------------------------------------ */
/*  1 行ずつ                                                           */
/* ------------------------------------------------------------------ */
int pci_verbose_line(const u32 *cfg, u32 bus, u32 dev, u32 fn,
                     int idx, char *buf, int cap)
{
    PvOut o;
    u16 vendor, device, command, status;
    u8 hdr, cls, sub, progif, rev;
    int layout, nbar;

    if (cap < 1 || buf == 0) return PCI_VERBOSE_END;
    buf[0] = '\0';
    o.buf = buf; o.cap = cap; o.len = 0;

    vendor  = pci_extract16(cfg[PCI_CFG_VENDOR_ID / 4], PCI_CFG_VENDOR_ID);
    device  = pci_extract16(cfg[PCI_CFG_DEVICE_ID / 4], PCI_CFG_DEVICE_ID);
    command = pci_extract16(cfg[PCI_CFG_COMMAND / 4], PCI_CFG_COMMAND);
    status  = pci_extract16(cfg[PCI_CFG_STATUS / 4], PCI_CFG_STATUS);
    rev     = pci_extract8(cfg[PCI_CFG_REVISION_ID / 4], PCI_CFG_REVISION_ID);
    progif  = pci_extract8(cfg[PCI_CFG_PROG_IF / 4], PCI_CFG_PROG_IF);
    sub     = pci_extract8(cfg[PCI_CFG_SUBCLASS / 4], PCI_CFG_SUBCLASS);
    cls     = pci_extract8(cfg[PCI_CFG_CLASS / 4], PCI_CFG_CLASS);
    hdr     = pci_extract8(cfg[PCI_CFG_HEADER_TYPE / 4], PCI_CFG_HEADER_TYPE);
    layout  = pci_header_layout(hdr);
    nbar    = pv_bar_count(hdr);

    if (idx < 0) return PCI_VERBOSE_END;

    if (idx == PV_ID) {
        const char *vn = pci_vendor_name(vendor);
        pv_dec(&o, bus & PCI_BUS_MASK); pv_ch(&o, ':');
        pv_dec(&o, dev & PCI_DEV_MASK); pv_ch(&o, '.');
        pv_dec(&o, fn & PCI_FN_MASK);   pv_ch(&o, ' ');
        pv_hex(&o, vendor, 4); pv_ch(&o, ':'); pv_hex(&o, device, 4);
        if (vn[0] != '\0') { pv_ch(&o, ' '); pv_str(&o, vn); }
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_REV) {
        pv_str(&o, "  revision "); pv_hex(&o, rev, 2);
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_CLASS) {
        pv_str(&o, "  class "); pv_hex(&o, cls, 2);
        pv_ch(&o, '.'); pv_hex(&o, sub, 2);
        pv_ch(&o, '.'); pv_hex(&o, progif, 2);
        pv_ch(&o, ' '); pv_str(&o, pci_class_name(cls, sub));
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_HEADER) {
        pv_str(&o, "  header "); pv_hex(&o, hdr, 2);
        pv_str(&o, " type "); pv_dec(&o, (u32)layout);
        if (layout == PCI_HDR_LAYOUT_DEVICE) pv_str(&o, " (device)");
        else if (layout == PCI_HDR_LAYOUT_BRIDGE) pv_str(&o, " (pci bridge)");
        else if (layout == PCI_HDR_LAYOUT_CARDBUS) pv_str(&o, " (cardbus)");
        else pv_str(&o, " (unknown)");
        pv_str(&o, pci_is_multifunction(hdr) ? " multi-function"
                                             : " single-function");
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_COMMAND) {
        pv_str(&o, "  command "); pv_hex(&o, command, 4);
        pv_flag(&o, "I/O", command, PCI_CMD_IO_ENABLE);
        pv_flag(&o, "Mem", command, PCI_CMD_MEM_ENABLE);
        pv_flag(&o, "BusMaster", command, PCI_CMD_BUS_MASTER);
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_STATUS) {
        pv_str(&o, "  status "); pv_hex(&o, status, 4);
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_SUBSYS) {
        if (layout == PCI_HDR_LAYOUT_DEVICE) {
            u32 sw = cfg[PCI_CFG_SUBSYS_VENDOR / 4];
            pv_str(&o, "  subsystem ");
            pv_hex(&o, pci_extract16(sw, PCI_CFG_SUBSYS_VENDOR), 4);
            pv_ch(&o, ':');
            pv_hex(&o, pci_extract16(sw, PCI_CFG_SUBSYS_ID), 4);
            return PCI_VERBOSE_LINE;
        }
        if (layout == PCI_HDR_LAYOUT_BRIDGE) {
            /* ブリッヂの配下のバスがどこかを見るために出す (0x18 の DWORD)。
             * Subsystem は Type 1 には無い (0x2C はプリフェッチ窓の上位)。 */
            u32 bw = cfg[PCI_CFG_PRIMARY_BUS / 4];
            pv_str(&o, "  bus primary ");
            pv_dec(&o, pci_extract8(bw, PCI_CFG_PRIMARY_BUS));
            pv_str(&o, " secondary ");
            pv_dec(&o, pci_extract8(bw, PCI_CFG_SECONDARY_BUS));
            pv_str(&o, " subordinate ");
            pv_dec(&o, pci_extract8(bw, PCI_CFG_SUBORDINATE_BUS));
            return PCI_VERBOSE_LINE;
        }
        return PCI_VERBOSE_SKIP;
    }
    if (idx >= PV_BAR0 && idx < PV_BAR0 + PCI_CFG_BAR_COUNT) {
        if (idx - PV_BAR0 >= nbar) return PCI_VERBOSE_SKIP;
        pv_bar_line(&o, cfg, nbar, idx - PV_BAR0);
        return PCI_VERBOSE_LINE;
    }
    if (idx == PV_IRQ) {
        u32 iw = cfg[PCI_CFG_INT_LINE / 4];
        u8 line = pci_extract8(iw, PCI_CFG_INT_LINE);
        u8 pin  = pci_extract8(iw, PCI_CFG_INT_PIN);
        pv_str(&o, "  interrupt line ");
        pv_dec(&o, line);
        if (line == PCI_INT_LINE_NONE) pv_str(&o, " (unassigned)");
        pv_str(&o, " pin ");
        if (pin >= 1 && pin <= 4) pv_ch(&o, (char)('A' + (pin - 1)));
        else pv_str(&o, "- (none)");
        return PCI_VERBOSE_LINE;
    }
    return PCI_VERBOSE_END;
}

/* ------------------------------------------------------------------ */
/*  "B:D.F" の読み取り                                                 */
/* ------------------------------------------------------------------ */
static const char *pv_num(const char *s, u32 *out, u32 max)
{
    u32 v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s - '0');
        if (v > max) return 0;
        any = 1;
        s++;
    }
    if (!any) return 0;
    *out = v;
    return s;
}

int pci_parse_bdf(const char *s, u32 *bus, u32 *dev, u32 *fn)
{
    u32 b, d, f;
    if (s == 0) return -1;
    s = pv_num(s, &b, PCI_BUS_MASK);
    if (s == 0 || *s != ':') return -1;
    s = pv_num(s + 1, &d, PCI_DEV_MASK);
    if (s == 0 || *s != '.') return -1;
    s = pv_num(s + 1, &f, PCI_FN_MASK);
    if (s == 0 || *s != '\0') return -1;
    *bus = b; *dev = d; *fn = f;
    return 0;
}
