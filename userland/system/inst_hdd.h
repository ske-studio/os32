/* ======================================================================== */
/*  INST_HDD.H — インストーラの hd0 の検査と書き込みの手順 (cdinst / install) */
/*                                                                          */
/*  票 docs/tasks/realhw/TASK_HDD_INSTALL.md 段 2 / §1-v3 N4・N6・R3-1。     */
/*  順序 (R3-1、ユーザー決裁 B):                                             */
/*                                                                          */
/*   検査 (1 セクタも書かない)                                               */
/*     inst_hdd_check        幾何 (BIOS / ATA)・計画・LBA 0/1 のモード・     */
/*                           マウント (ルートの hd0 は断る)                  */
/*     inst_hdd_check_media  IPL / ローダ / vmkernel.lz4 の大きさ・容量      */
/*     inst_hdd_describe     確認画面 (作り直すと一時置き場のデータが消える) */
/*   書く (呼び手が承認を取った後)                                           */
/*     inst_hdd_release      hd0 のマウントを umount_checked で外す (N6)。   */
/*                           外れなければ断る (まだ何も書いていない)         */
/*     inst_hdd_prepare      ext2_format_at → 区画表 → 読み戻し比較 →        */
/*                           通常のマウント (/hd0)                           */
/*     inst_hdd_write_boot   ローダ (LBA 2〜) → IPL (LBA 0、BIOS 幾何) を     */
/*                           それぞれ読み戻して比較                          */
/*   この後で呼び手が展開 → sync。                                           */
/*                                                                          */
/*  書いた後の失敗は "INCOMPLETE" と出して負を返す。書く前の失敗は           */
/*  "Nothing was written" と出す。どちらの状態でも次の実行は通る: format の  */
/*  失敗なら区画表はまだ空 (空のディスク)、区画表を書いた後なら OS32 の項目  */
/*  1 つ (再作成) — ローダと IPL はマウントの確認の**後**に書くので、途中で   */
/*  止まっても LBA 0 の 55AA だけが残って「空でない」になることは無い。       */
/* ======================================================================== */

#ifndef INST_HDD_H
#define INST_HDD_H

#include "os32api.h"
#include "inst_disk.h"

#define INST_DRIVE        0          /* hd0 = IDE 0 = DA 80h だけ */
#define INST_DEV          "hd0"
#define INST_MOUNT        "/hd0"

typedef struct {
    HddGeom    hg;
    HdprepGeom g;
    HdprepPlan plan;
    int        mode;          /* INST_MODE_* */
    int        mounts;        /* 検査の時点の dev_mount_count(0) */
    int        umount_hd0;    /* hd0 は /hd0 に 1 つだけマウントされている (外す) */
} InstTarget;

int  inst_hdd_check(KernelAPI *api, InstTarget *t);
int  inst_hdd_check_media(KernelAPI *api, const InstTarget *t,
                          u32 ipl_len, u32 loader_len, u32 kernel_len,
                          const InstNeed *need);
void inst_hdd_describe(KernelAPI *api, const InstTarget *t);
/* hd0 のマウントを外す (N6)。失敗・外れないなら負 (何も書いていない) */
int  inst_hdd_release(KernelAPI *api, const InstTarget *t);
/* ext2_format_at → 区画表 → 読み戻し → マウント。失敗は INCOMPLETE */
int  inst_hdd_prepare(KernelAPI *api, const InstTarget *t);
int  inst_hdd_write_boot(KernelAPI *api, const InstTarget *t,
                         const u8 *ipl, u32 ipl_len,
                         const u8 *loader, u32 loader_len);
/* 書いた後の失敗の表示 (展開・sync の失敗にも呼び手が使う) */
void inst_hdd_incomplete(KernelAPI *api, const char *what, int rc);

#endif /* INST_HDD_H */
