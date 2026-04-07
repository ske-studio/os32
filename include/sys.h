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

#endif /* __SYS_H */
