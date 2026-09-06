/* ======================================================================== */
/*  SYS.H — システム制御およびハードウェア制御API                           */
/* ======================================================================== */

#ifndef __SYS_H
#define __SYS_H

#include "types.h"

void sys_reboot(void);
void sys_halt(void);
void buz_on(void);
void buz_off(void);

u32 sys_get_mem_kb(void);
u32 sys_usable_mem_end(void);
u32 sys_hotdeploy_base(void);

/* 物理末尾側 (ホットデプロイ窓の直下) に bytes バイトを固定予約し、先頭物理を
 * 返す。以後 sys_usable_mem_end() はその分下がる。ブート中に 1 回だけ。
 * 戻り値 0 = 予約できなかった。→ kernel/sys.c の説明 */
u32 sys_reserve_top(u32 bytes);

#endif /* __SYS_H */
