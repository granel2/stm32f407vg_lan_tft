/*
 * Minimal NO_SYS=1 (bare-metal) sys_arch port for this project.
 *
 * The sys_arch.h shipped by ST under Middlewares/Third_Party/LwIP/system/arch
 * is written for the FreeRTOS/CMSIS-RTOS port (it pulls in cmsis_os.h and
 * hard-errors unless NO_SYS==0) - this project runs bare-metal, so it gets
 * its own trivial replacement instead. See sys_arch.c for the only two
 * functions a NO_SYS port actually needs to provide: sys_arch_protect()/
 * sys_arch_unprotect() (critical sections) and sys_now() (see ethernetif.c).
 */
#ifndef __SYS_ARCH_H__
#define __SYS_ARCH_H__

#include "lwip/opt.h"

#if (NO_SYS == 0)
#error "This sys_arch.h only supports NO_SYS=1 builds - see sys_arch.c"
#endif

#endif /* __SYS_ARCH_H__ */
