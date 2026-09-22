/* ======================================================================== */
/*  PCI モジュール (cmd_pci.c) — `lspci` と `pcidump`                       */
/*                                                                          */
/*  実機 PC-9821Ra266 の内蔵 LAN (Intel 82557 = 8086:1229) を見つけ、BAR と */
/*  Interrupt Line を読み出すための口。**NP21/W には PCI が無い**ので、     */
/*  エミュレータでは「PCI 無し」を正しく報告することだけが仕事になる。      */
/*                                                                          */
/*  `pcidump` は config 256 バイトの生ダンプ — 票 §5-2 の R2 / R3 / R4      */
/*  (82557 の Command と Subsystem、C バスブリッヂの PIRQ 割り当て、        */
/*  PCMC の vendor:device) を実機の 1 回でまとめて持ち帰るための道具。      */
/*                                                                          */
/*  票  : docs/tasks/realhw/TASK_LAN_82557.md §2 L-A / §5-2                 */
/*  復号: drivers/pci_decode.c を**写さずに**リンクしている                  */
/*        (build/programs.mk の PCI_DECODE_USER_OBJ)。BAR の種別と番地・    */
/*        クラス名・Header Type の切り出しがカーネルと食い違わないため。    */
/* ======================================================================== */

#include "shell.h"
#include "drivers/pci_decode.h"   /* PROGRAM_FLAGS の -I. で引く */

/* P-1: drivers/pci.h の struct pci_dev と**同じ並び**でなければならない。
 * `pci_get` はカーネル側の定義 (i386 で 40 バイト) で書くので、ここが
 * 違うと呼び手のスタックを踏む (drivers/ide.h の IdeInfo が 92B/96B で
 * ずれて `ide 0` が壊れたのと同じ穴)。drivers/pci.h はカーネル内部
 * ヘッダなので写しをここに置く — 向こうを変えたら必ず一緒に直すこと。
 * カーネル側は drivers/pci.c の STATIC_ASSERT が 40 バイトを見張る。 */
typedef struct {
    u8  bus;            /*  0 */
    u8  dev;            /*  1 */
    u8  fn;             /*  2 */
    /*  3: padding */
    u16 vendor;         /*  4 */
    u16 device;         /*  6 */
    u8  cls;            /*  8  config 0x0B (カーネル側の名前は class) */
    u8  subclass;       /*  9  config 0x0A */
    u8  progif;         /* 10  config 0x09 */
    u8  header;         /* 11  config 0x0E */
    u32 bar[PCI_CFG_BAR_COUNT]; /* 12〜35 */
    u8  irq_line;       /* 36  config 0x3C */
    u8  irq_pin;        /* 37  config 0x3D */
    u16 command;        /* 38  config 0x04 */
} PciDev;               /* 40 バイト */

/* 写しが本物と同じ大きさであることをコンパイル時に確かめる
 * (カーネル側は drivers/pci.c の同名の検査)。ずれたらここで止まる。 */
STATIC_ASSERT(sizeof(PciDev) == 40, shell_pci_dev_is_40);

/* Interrupt Pin の 1〜4 を A〜D に。0 = 割り込みを使わない。
 * PC-98 ではこの Pin がスロットごとに違う PIRQ 線へ配線され
 * (io_pci.md 表3)、PnP BIOS が 8259 の入力へ落とした結果が
 * Interrupt Line に入る。だから**表示するのは両方**。 */
static char pci_pin_letter(u8 pin)
{
    if (pin >= 1 && pin <= 4) return (char)('A' + (pin - 1));
    return '-';
}

/* 10 進 (先頭 0x なら 16 進) を 1 つ読む。読めなければ -1。 */
static int pci_parse_num(const char *s)
{
    long v = 0;
    int any = 0;

    if (s == 0 || *s == '\0') return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else return -1;
            v = v * 16 + d;
            if (v > 0xFFFF) return -1;
            any = 1; s++;
        }
    } else {
        while (*s) {
            if (*s < '0' || *s > '9') return -1;
            v = v * 10 + (*s - '0');
            if (v > 0xFFFF) return -1;
            any = 1; s++;
        }
    }
    return any ? (int)v : -1;
}

/* ------------------------------------------------------------------ */
/*  lspci — 1 行 1 デバイス                                            */
/* ------------------------------------------------------------------ */
static int cmd_lspci(int argc, char **argv)
{
    int n, i, b;
    PciDev d;
    (void)argc; (void)argv;

    n = g_api->pci_count();
    if (n <= 0) {
        /* PCI のある機械には必ずホストブリッヂ (PCMC = デバイス 0、
         * io_pci.md 55 行) が居るので、0 件 = メカニズム #1 が無い。
         * **これは失敗ではない** — NP21/W の正しい姿 ([V4])。 */
        g_api->kprintf(ATTR_CYAN, "%s",
                       "lspci: no PCI (mechanism #1 not present)\n");
        return 0;
    }

    for (i = 0; i < n; i++) {
        const char *vn;

        if (g_api->pci_get((u32)i, &d) != 0) continue;

        vn = pci_vendor_name(d.vendor);
        /* **1 行を 2 回に分けて出す。** `sh.bin` (CPL=3) の KAPI は
         * int 0x80 でユーザスタックから引数を写すが、その窓は 64 バイト
         * (exec/exec.c の RING3_ARG_WINDOW)。1 回にまとめると 15 引数
         * = 60 バイトで、余白が 1 引数しか残らない。 */
        g_api->kprintf(ATTR_WHITE, "%u:%u.%u %04x:%04x %s%s%s",
                       (u32)d.bus, (u32)d.dev, (u32)d.fn,
                       (u32)d.vendor, (u32)d.device,
                       vn, (vn[0] != '\0') ? " " : "",
                       pci_class_name(d.cls, d.subclass));
        g_api->kprintf(ATTR_WHITE, " class %02x.%02x hdr %02x irq %u pin %c",
                       (u32)d.cls, (u32)d.subclass, (u32)d.header,
                       (u32)d.irq_line, pci_pin_letter(d.irq_pin));

        for (b = 0; b < PCI_CFG_BAR_COUNT; b++) {
            int kind = pci_bar_kind(d.bar[b]);
            if (kind == PCI_BAR_NONE) continue;
            g_api->kprintf(ATTR_WHITE, "  bar%d %s 0x%x", b,
                           (kind == PCI_BAR_IO) ? "io" : "mem",
                           pci_bar_base(d.bar[b]));
        }
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  pcidump — config 256 バイトの生ダンプ                              */
/*                                                                    */
/*  実機の 1 回で票 §5-2 の R2〜R4 を持ち帰るための道具。復号しない    */
/*  生のバイト列を出すので、後から手元で何度でも読み直せる。           */
/* ------------------------------------------------------------------ */
static int cmd_pcidump(int argc, char **argv)
{
    int bus, dev, fn, off;
    u32 idw;

    if (argc < 4) {
        shell_print_help(argv[0]);
        return SH_STATUS_USAGE;
    }
    bus = pci_parse_num(argv[1]);
    dev = pci_parse_num(argv[2]);
    fn  = pci_parse_num(argv[3]);
    /* 欄の幅そのもので断る ([C4]: 上限はマスクから出す)。 */
    if (bus < 0 || bus > (int)PCI_BUS_MASK ||
        dev < 0 || dev > (int)PCI_DEV_MASK ||
        fn  < 0 || fn  > (int)PCI_FN_MASK) {
        g_api->kprintf(ATTR_RED, "pcidump: bus 0-%u dev 0-%u fn 0-%u\n",
                       (u32)PCI_BUS_MASK, (u32)PCI_DEV_MASK, (u32)PCI_FN_MASK);
        return SH_STATUS_USAGE;
    }

    if (g_api->pci_count() <= 0) {
        g_api->kprintf(ATTR_CYAN, "%s",
                       "pcidump: no PCI (mechanism #1 not present)\n");
        return SH_STATUS_ERROR;
    }

    idw = g_api->pci_cfg_read32((u32)bus, (u32)dev, (u32)fn, PCI_CFG_VENDOR_ID);
    if (pci_extract16(idw, PCI_CFG_VENDOR_ID) == PCI_VENDOR_NONE) {
        g_api->kprintf(ATTR_YELLOW, "pcidump: %u:%u.%u not present\n",
                       (u32)bus, (u32)dev, (u32)fn);
        return SH_STATUS_ERROR;
    }

    g_api->kprintf(ATTR_CYAN, "pcidump %u:%u.%u\n", (u32)bus, (u32)dev, (u32)fn);
    for (off = 0; off < PCI_CFG_SPACE_SIZE; off += 16) {
        int k;
        g_api->kprintf(ATTR_WHITE, "%02x:", (u32)off);
        for (k = 0; k < 16; k += 4) {
            u32 w = g_api->pci_cfg_read32((u32)bus, (u32)dev, (u32)fn,
                                          (u32)(off + k));
            /* DWORD をリトルエンディアンのバイト列として並べる
             * (0CFCh から読んだ順と同じ = ホストで読み直せる形)。 */
            g_api->kprintf(ATTR_WHITE, " %02x %02x %02x %02x",
                           (u32)((w >> 0) & 0xFF), (u32)((w >> 8) & 0xFF),
                           (u32)((w >> 16) & 0xFF), (u32)((w >> 24) & 0xFF));
        }
        g_api->kprintf(ATTR_WHITE, "%s", "\n");
    }
    return 0;
}

/* 登録用テーブル */
static const ShellCmd pci_cmds[] = {
    { "lspci",   cmd_lspci,   "",          "List PCI devices (vendor, class, BAR, IRQ)" },
    { "pcidump", cmd_pcidump, "bus dev fn","Hex dump of a PCI config space (256 bytes)" },
    { (const char *)0, 0, 0, 0 }
};

void shell_cmd_pci_init(void)
{
    shell_register_cmds(pci_cmds);
}
