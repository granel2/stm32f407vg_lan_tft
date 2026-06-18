/**
  ******************************************************************************
  * @file    sys_arch.c
  * @brief   The two functions a NO_SYS=1 (bare-metal) lwIP port must supply:
  *          a critical-section primitive used by mem.c/memp.c/pbuf.c to guard
  *          against the ETH ISR reentering them while the main loop is in the
  *          middle of an allocation/free.
  ******************************************************************************
  */
#include "lwip/arch.h"
#include "cmsis_compiler.h"

sys_prot_t sys_arch_protect(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return (sys_prot_t)primask;
}

void sys_arch_unprotect(sys_prot_t pval)
{
  __set_PRIMASK((uint32_t)pval);
}
