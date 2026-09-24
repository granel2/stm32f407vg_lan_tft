/**
  ******************************************************************************
  * @file    diag_config.h
  * @brief   On/off switches for every diagnostic / test feature in Diag/.
  *
  * DIAG_ENABLE is the master switch: 0 drops all of Diag/ from the build -
  * the hooks left in main.c/tft_app.c turn into empty inline stubs, the CPU
  * page disappears from the page cycle. Each feature can also be switched
  * off on its own below. Any value can be overridden from the compiler
  * command line instead (e.g. -DDIAG_ENABLE=0) - every define is #ifndef'd.
  *
  * New diagnostic features: add a DIAG_<NAME> switch here, gated by
  * DIAG_ENABLE like the ones below, and keep the code in Diag/.
  ******************************************************************************
  */
#ifndef DIAG_CONFIG_H
#define DIAG_CONFIG_H

#ifndef DIAG_ENABLE
#define DIAG_ENABLE             1
#endif

/* Main-loop load measurement (DWT cycle counter): time per section
   (ETH, lwIP, TCP, HTTP, TFT), loops per second, longest loop pass. */
#ifndef DIAG_CPU_STATS
#define DIAG_CPU_STATS          1
#endif

/* ... summary printed to USART1 every DIAG_CPU_UART_PERIOD_S seconds */
#ifndef DIAG_CPU_UART
#define DIAG_CPU_UART           1
#endif
#ifndef DIAG_CPU_UART_PERIOD_S
#define DIAG_CPU_UART_PERIOD_S  5
#endif

/* ... and shown on a "CPU" page in the TFT page cycle (1 s window) */
#ifndef DIAG_CPU_PAGE
#define DIAG_CPU_PAGE           1
#endif

/* ---- dependencies: nothing below this line to edit ------------------- */
#if !DIAG_ENABLE
#undef  DIAG_CPU_STATS
#define DIAG_CPU_STATS          0
#endif
#if !DIAG_CPU_STATS
#undef  DIAG_CPU_UART
#define DIAG_CPU_UART           0
#undef  DIAG_CPU_PAGE
#define DIAG_CPU_PAGE           0
#endif

#endif /* DIAG_CONFIG_H */
