/**
  ******************************************************************************
  * @file    diag_cpu.h
  * @brief   Main-loop load measurement on the DWT cycle counter (160 MHz).
  *
  * There is no RTOS and no idle task: the main loop spins all the time, so
  * "CPU load" here means where the loop's time goes. main.c brackets each
  * job with diag_cpu_begin()/diag_cpu_end(); whatever is not bracketed
  * (LED blink, DHCP/watchdog logic, the loop itself) counts as OTHER.
  * Interrupts are charged to whichever section they interrupted.
  *
  * Reported per 1 s window: loop passes per second, the longest single pass
  * (= worst-case delay before an Ethernet frame gets looked at), and per
  * section its share of time and its longest single run.
  *
  * Switched by DIAG_CPU_STATS in diag_config.h; when off every function
  * below is an empty inline stub, so the calls in main.c cost nothing.
  ******************************************************************************
  */
#ifndef DIAG_CPU_H
#define DIAG_CPU_H

#include <stdint.h>
#include "diag_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  DIAG_SECT_ETH = 0,  /* ethernetif_input() + ethernetif_poll_link() */
  DIAG_SECT_LWIP,     /* sys_check_timeouts() */
  DIAG_SECT_TCP,      /* tcp_echo_client_poll() */
  DIAG_SECT_HTTP,     /* config_http_poll() */
  DIAG_SECT_TFT,      /* TFT_App_AlivePoll() + TFT_App_ShowReceived() */
  DIAG_SECT_COUNT
} DiagSect;

/* One display row: label fits the TFT label column (8 chars), value the
   value column (16 chars). */
typedef struct
{
  char label[9];
  char value[17];
} DiagRow;

#define DIAG_CPU_ROWS  (DIAG_SECT_COUNT + 3U)  /* LOOPS/S, LOOPMAX, sections, OTHER */

#if DIAG_CPU_STATS

/* Start the cycle counter. Call once, right before the main loop. */
void     diag_cpu_init(void);
/* Top of every main-loop pass: closes the 1 s window when it is due and
   prints the UART summary (its own time is left out of the figures). */
void     diag_cpu_loop(void);
void     diag_cpu_begin(DiagSect s);
void     diag_cpu_end(DiagSect s);
/* Last complete 1 s window as label/value rows (DIAG_CPU_ROWS of them);
   values read "---" until the first window has closed. */
uint8_t  diag_cpu_rows(DiagRow *rows, uint8_t max);

#else

static inline void    diag_cpu_init(void) {}
static inline void    diag_cpu_loop(void) {}
static inline void    diag_cpu_begin(DiagSect s) { (void)s; }
static inline void    diag_cpu_end(DiagSect s)   { (void)s; }
static inline uint8_t diag_cpu_rows(DiagRow *rows, uint8_t max) { (void)rows; (void)max; return 0U; }

#endif /* DIAG_CPU_STATS */

#ifdef __cplusplus
}
#endif

#endif /* DIAG_CPU_H */
