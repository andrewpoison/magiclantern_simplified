/*
 * R6.190 M16.0 (spec m16/m160 2, 3, 4.7..4.9, 5.5): the core part of the Lua / module groundwork (r6_modsup.c, lvprobe.c).
 * The module-visible part is r6_modsup_abi.h.
 */
#ifndef _r6_modsup_h_
#define _r6_modsup_h_

#include "r6_modsup_abi.h"

/* the lock of the check + create (also lp_snap_start, spec 3.4): 1 = taken (within ms), 0 = not */
int  r6tk_lock(int ms);
void r6tk_unlock(void);
/* review R1-04: 1 = the lock exists (r6tk_lock 0 then means a timeout, not a missing semaphore) */
int  r6tk_have_sem(void);

/* lvprobe.c (spec 3.2): the M14.1c sys_mem read for the wrapper; 1 = ok (read only, any task) */
int lp_sys_query(struct r6_sysq * q);

/* spec 3.3.1: src/module.c sets them while it reads R6_190.sym (lines named task_create; that line's address is
 * r6_task_try_create); without exactly one such line no module is linked. Kept here, so module.c's own data stays as it was. */
extern int r6_alias_seen, r6_alias_ok;

#ifdef CONFIG_R6_SCRIPT
/* M16.1 (spec m16/m161 11): the living wrapper tasks (LVP tsk.hw), the odd task_create returns (R2-06); names _: not exported */
int _r6tk_live(int i, uint32_t * tid, uint32_t * stack, char * name);
uint32_t _r6tk_odd(void);
#endif

#endif
