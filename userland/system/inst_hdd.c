/* ======================================================================== */
/*  INST_HDD.C — インストーラの hd0 の検査と書き込み (cdinst / install 共通)  */
/*                                                                          */
/*  手順と規則は inst_hdd.h / inst_disk.h。「全検査 → 書く」の手本は         */
/*  userland/shell/cmd_hdprep.c。                                            */
/* ======================================================================== */

#include "inst_hdd.h"
#include "drivers/pc98pt.h"

#define IH_SECT 512

static u8 ih_lba0[IH_SECT];
static u8 ih_lba1[IH_SECT];
static u8 ih_sect[IH_SECT];
static u8 ih_back[IH_SECT];

static int ih_memeq(const u8 *a, const u8 *b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int ih_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void ih_refuse(KernelAPI *api, int code)
{
    api->kprintf(ATTR_RED, "Refused: %s (code %d). Nothing was written.\n",
                 inst_reason(code), code);
}

void inst_hdd_incomplete(KernelAPI *api, const char *what, int rc)
{
    api->kprintf(ATTR_RED, "INCOMPLETE: %s (rc=%d)\n", what, rc);
    api->kprintf(ATTR_RED, "%s",
                 "  hd0 is not a usable OS32 disk. Run the installer again "
                 "(it re-creates the OS32 area).\n");
}

static void ih_geom_from_kapi(const HddGeom *hg, HdprepGeom *g)
{
    g->ata_present   = hg->ata_present;
    g->addr_mode     = hg->addr_mode;
    g->ata_total     = hg->ata_total;
    g->ata_cur_cyl   = hg->ata_cur_cyl;
    g->ata_cur_heads = hg->ata_cur_heads;
    g->ata_cur_spt   = hg->ata_cur_spt;
    g->bios_queried  = hg->bios_queried;
    g->bios_valid    = hg->bios_valid;
    g->bios_cyl      = hg->bios_cyl;
    g->bios_heads    = hg->bios_heads;
    g->bios_spt      = hg->bios_spt;
    g->bios_seclen   = hg->bios_seclen;
}

int inst_hdd_check(KernelAPI *api, InstTarget *t)
{
    const char *rootdev;
    int rc, root_hd0;

    if (api->hdd_geom_info(INST_DRIVE, &t->hg) != 0) {
        ih_refuse(api, INST_E_ARG);
        return INST_E_ARG;
    }
    ih_geom_from_kapi(&t->hg, &t->g);
    api->kprintf(ATTR_WHITE,
                 "  hd0 BIOS (AH=84h): valid=%d C/H/S=%u/%u/%u len=%u\n",
                 (int)t->hg.bios_valid, (u32)t->hg.bios_cyl, (u32)t->hg.bios_heads,
                 (u32)t->hg.bios_spt, (u32)t->hg.bios_seclen);
    api->kprintf(ATTR_WHITE, "  hd0 ATA: present=%d total=%u sectors (%u MB) io=%d\n",
                 (int)t->hg.ata_present, t->hg.ata_total,
                 t->hg.ata_total / HDPREP_SECTORS_PER_MB, (int)t->hg.addr_mode);

    rc = hdprep_check_geom(&t->g);
    if (rc != HDPREP_OK) { ih_refuse(api, rc); return rc; }
    rc = hdprep_plan(&t->g, INST_PART_MB, &t->plan);
    if (rc != HDPREP_OK) { ih_refuse(api, rc); return rc; }

    rc = api->ide_read_sector(INST_DRIVE, 0, ih_lba0);
    if (rc == 0) rc = api->ide_read_sector(INST_DRIVE, PC98PT_LBA, ih_lba1);
    if (rc != 0) {
        api->kprintf(ATTR_RED,
                     "Refused: cannot read LBA 0/1 of hd0 (rc=%d). Nothing was written.\n", rc);
        return rc < 0 ? rc : INST_E_ARG;
    }
    rc = inst_classify(ih_lba0, ih_lba1, t->plan.heads, t->plan.spt,
                       t->g.ata_total, t->plan.start, &t->mode);
    if (rc != 0) { ih_refuse(api, rc); return rc; }

    rootdev = api->vfs_devname("/");
    root_hd0 = (rootdev && ih_streq(rootdev, INST_DEV)) ? 1 : 0;
    t->mounts = api->dev_mount_count(INST_DRIVE);
    rc = hdprep_check_mounts(t->mounts, root_hd0);
    if (rc < 0) { ih_refuse(api, rc); return rc; }
    return 0;
}

int inst_hdd_check_media(KernelAPI *api, const InstTarget *t,
                         u32 ipl_len, u32 loader_len, u32 kernel_len,
                         const InstNeed *need)
{
    InstRoom room;
    int rc;

    rc = inst_check_boot_files(ipl_len, loader_len, kernel_len);
    if (rc != 0) {
        api->kprintf(ATTR_WHITE, "  sizes: IPL %u, loader %u, vmkernel.lz4 %u bytes\n",
                     ipl_len, loader_len, kernel_len);
        ih_refuse(api, rc);
        return rc;
    }
    rc = inst_check_space(t->plan.len, need, &room);
    api->kprintf(ATTR_WHITE,
                 "  space: need %u KiB / %u inodes, the area has %u KiB / %u inodes free\n",
                 room.need_blocks, room.need_inodes, room.free_blocks, room.free_inodes);
    if (rc != 0) { ih_refuse(api, rc); return rc; }
    return 0;
}

void inst_hdd_describe(KernelAPI *api, const InstTarget *t)
{
    const HdprepPlan *p = &t->plan;

    api->kprintf(ATTR_CYAN, "Target: hd0 (IDE drive 0 = BIOS DA 80h), %s\n",
                 inst_mode_name(t->mode));
    api->kprintf(ATTR_WHITE,
                 "  OS32 area: LBA %u..%u (%u MB), cyl %u..%u at %u heads x %u sectors\n",
                 p->start, p->start + p->len - 1UL, p->len / HDPREP_SECTORS_PER_MB,
                 p->start / p->cyl_sectors, p->end_cyl, p->heads, p->spt);
    api->kprintf(ATTR_WHITE, "%s",
                 "  Writes: ext2 in the area, partition table (LBA 1), "
                 "loader (LBA 2..17), IPL (LBA 0)\n");
    if (t->mode != INST_MODE_EMPTY)
        api->kprintf(ATTR_RED, "%s",
                     "  The existing OS32 area (the temporary storage, /hd0) is "
                     "re-created: ALL FILES IN IT WILL BE LOST.\n");
    if (t->mounts > 0)
        api->kprintf(ATTR_YELLOW, "  hd0 is mounted (%d) and will be unmounted first.\n",
                     t->mounts);
}

int inst_hdd_release(KernelAPI *api, const InstTarget *t)
{
    int rc;

    /* ---- 使用中の検査 (N6): 書く前の最後の検査。失敗なら何も書かない ---- */
    if (t->mounts > 0 && api->sys_is_mounted(INST_MOUNT)) {
        rc = api->sys_umount_checked(INST_MOUNT);
        if (rc < 0) {
            api->kprintf(ATTR_RED,
                         "Refused: umount /hd0 failed (rc=%d). Nothing was written.\n", rc);
            return rc;
        }
    }
    if (api->dev_mount_count(INST_DRIVE) != 0) {
        ih_refuse(api, HDPREP_E_STILL_MOUNTED);
        return HDPREP_E_STILL_MOUNTED;
    }
    return 0;
}

int inst_hdd_prepare(KernelAPI *api, const InstTarget *t)
{
    int rc;

    /* ---- ext2 (区画表を読まずに範囲だけ) ---- */
    api->kprintf(ATTR_CYAN, "  Formatting ext2 (LBA %u, %u sectors)...\n",
                 t->plan.start, t->plan.len);
    rc = api->ext2_format_at(INST_DRIVE, t->plan.start, t->plan.len);
    if (rc != 0) {
        inst_hdd_incomplete(api, "ext2_format_at failed (partition table not written)", rc);
        return rc < 0 ? rc : -1;
    }

    /* ---- 区画表 → 読み戻し比較 ---- */
    rc = inst_build_pt(ih_sect, &t->plan);
    if (rc != 0) {
        inst_hdd_incomplete(api, "cannot build the partition entry", rc);
        return rc < 0 ? rc : -1;
    }
    rc = api->ide_write_sector(INST_DRIVE, PC98PT_LBA, ih_sect);
    if (rc == 0) rc = api->ide_read_sector(INST_DRIVE, PC98PT_LBA, ih_back);
    if (rc != 0 || !ih_memeq(ih_sect, ih_back, IH_SECT)) {
        inst_hdd_incomplete(api, "partition table write/readback failed", rc);
        return rc < 0 ? rc : -1;
    }
    api->kprintf(ATTR_GREEN, "%s", "  Partition table written and verified.\n");

    /* ---- 通常のマウントで確認 (区画表 → ext2_find_partition → ext2) ---- */
    rc = api->sys_mount(INST_MOUNT, INST_DEV, "ext2");
    if (rc != 0) {
        inst_hdd_incomplete(api, "mount /hd0 through the new partition table failed", rc);
        return rc < 0 ? rc : -1;
    }
    api->kprintf(ATTR_GREEN, "%s", "  Mounted /hd0.\n");
    return 0;
}

/* 1 セクタを書いて読み戻して比べる */
static int ih_write_verify(KernelAPI *api, u32 lba, const u8 *buf)
{
    int rc = api->ide_write_sector(INST_DRIVE, lba, buf);
    if (rc == 0) rc = api->ide_read_sector(INST_DRIVE, lba, ih_back);
    if (rc != 0) return rc;
    return ih_memeq(buf, ih_back, IH_SECT) ? 0 : -1;
}

int inst_hdd_write_boot(KernelAPI *api, const InstTarget *t,
                        const u8 *ipl, u32 ipl_len,
                        const u8 *loader, u32 loader_len)
{
    u32 off, n, i;
    int rc;

    /* 事前検査を通っていること (ここでも範囲を守る) */
    if (inst_check_boot_files(ipl_len, loader_len, 1) != 0 || !ipl || !loader) {
        inst_hdd_incomplete(api, "boot files are not the checked ones", INST_E_ARG);
        return INST_E_ARG;
    }

    /* ---- ローダ → LBA 2〜 (512 B ずつ、端数は 0 で埋める) ---- */
    for (off = 0; off < loader_len; off += IH_SECT) {
        n = loader_len - off;
        if (n > IH_SECT) n = IH_SECT;
        for (i = 0; i < IH_SECT; i++) ih_sect[i] = (i < n) ? loader[off + i] : 0;
        rc = ih_write_verify(api, INST_LOADER_LBA + off / IH_SECT, ih_sect);
        if (rc != 0) {
            inst_hdd_incomplete(api, "loader write/readback failed", rc);
            return rc < 0 ? rc : -1;
        }
    }

    /* ---- IPL → LBA 0 (BIOS 幾何を [8]/[9] に、末尾 55AA) ---- */
    for (i = 0; i < IH_SECT; i++) ih_sect[i] = (i < ipl_len) ? ipl[i] : 0;
    inst_patch_ipl(ih_sect, t->plan.heads, t->plan.spt);
    rc = ih_write_verify(api, 0, ih_sect);
    if (rc != 0) {
        inst_hdd_incomplete(api, "IPL write/readback failed", rc);
        return rc < 0 ? rc : -1;
    }
    api->kprintf(ATTR_GREEN, "  IPL (geometry %u/%u) and loader (%u bytes) written and verified.\n",
                 t->plan.heads, t->plan.spt, loader_len);
    return 0;
}
