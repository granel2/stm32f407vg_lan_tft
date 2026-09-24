/**
  ******************************************************************************
  * @file    diag_cpu.c
  * @brief   Main-loop load measurement - see diag_cpu.h.
  ******************************************************************************
  */
#include "diag_cpu.h"

#if DIAG_CPU_STATS

#include <stdio.h>
#include <string.h>
#include "main.h"

typedef struct
{
  uint32_t cyc[DIAG_SECT_COUNT];  /* cycles spent per section */
  uint32_t max[DIAG_SECT_COUNT];  /* longest single run per section */
  uint32_t loops;                 /* main-loop passes */
  uint32_t loop_max;              /* longest single pass */
  uint32_t total;                 /* cycles covered (window length) */
} DiagAcc;

static const char *const sect_name[DIAG_SECT_COUNT] = { "ETH", "LWIP", "TCP", "HTTP", "TFT" };

static DiagAcc  s_acc;        /* window being filled */
static DiagAcc  s_last;       /* last complete 1 s window, for the TFT page */
static uint8_t  s_have_last;
static uint32_t s_t0[DIAG_SECT_COUNT];
static uint32_t s_loop_prev;
static uint32_t s_win_start;

#if DIAG_CPU_UART
static DiagAcc  s_uart;       /* windows since the last UART print */
static uint32_t s_uart_windows;
#endif

static inline uint32_t cycles(void) { return DWT->CYCCNT; }

/* Share of the window in tenths of a percent */
static uint32_t per_mille(uint32_t part, uint32_t total)
{
  return (total == 0U) ? 0U : (uint32_t)(((uint64_t)part * 1000U) / total);
}

/* Cycle count as a short duration: "850US", "4.1MS", "612MS" */
static void fmt_time(char *buf, size_t size, uint32_t cyc)
{
  const uint32_t us = cyc / (SystemCoreClock / 1000000U);

  if (us < 1000U)       { snprintf(buf, size, "%luUS", (unsigned long)us); }
  else if (us < 10000U) { snprintf(buf, size, "%lu.%luMS", (unsigned long)(us / 1000U), (unsigned long)((us % 1000U) / 100U)); }
  else                  { snprintf(buf, size, "%luMS", (unsigned long)(us / 1000U)); }
}

static uint32_t other_cycles(const DiagAcc *a)
{
  uint32_t busy = 0U;

  for (uint8_t i = 0; i < DIAG_SECT_COUNT; i++) { busy += a->cyc[i]; }
  return (busy < a->total) ? (a->total - busy) : 0U;
}

#if DIAG_CPU_UART
static void uart_report(void)
{
  char line[200];
  char t[12];
  int  n;
  uint32_t pm;

  fmt_time(t, sizeof(t), s_uart.loop_max);
  n = snprintf(line, sizeof(line), "[cpu] %lu loops/s, max pass %s |",
               (unsigned long)(s_uart.loops / DIAG_CPU_UART_PERIOD_S), t);
  for (uint8_t i = 0; (i < DIAG_SECT_COUNT) && (n > 0) && ((size_t)n < sizeof(line)); i++)
  {
    pm = per_mille(s_uart.cyc[i], s_uart.total);
    fmt_time(t, sizeof(t), s_uart.max[i]);
    n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %lu.%lu%% (max %s)",
                  sect_name[i], (unsigned long)(pm / 10U), (unsigned long)(pm % 10U), t);
  }
  if ((n > 0) && ((size_t)n < sizeof(line)))
  {
    pm = per_mille(other_cycles(&s_uart), s_uart.total);
    snprintf(line + n, sizeof(line) - (size_t)n, " OTHER %lu.%lu%%\r\n",
             (unsigned long)(pm / 10U), (unsigned long)(pm % 10U));
  }
  Debug_Print(line);
}

static void uart_fold(const DiagAcc *w)
{
  for (uint8_t i = 0; i < DIAG_SECT_COUNT; i++)
  {
    s_uart.cyc[i] += w->cyc[i];
    if (w->max[i] > s_uart.max[i]) { s_uart.max[i] = w->max[i]; }
  }
  s_uart.loops += w->loops;
  s_uart.total += w->total;
  if (w->loop_max > s_uart.loop_max) { s_uart.loop_max = w->loop_max; }
}
#endif /* DIAG_CPU_UART */

void diag_cpu_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

  memset(&s_acc, 0, sizeof(s_acc));
  s_have_last = 0U;
  s_loop_prev = cycles();
  s_win_start = s_loop_prev;
}

void diag_cpu_loop(void)
{
  const uint32_t now  = cycles();
  const uint32_t pass = now - s_loop_prev;

  s_loop_prev = now;
  s_acc.loops++;
  if (pass > s_acc.loop_max) { s_acc.loop_max = pass; }

  /* 1 s window. CYCCNT wraps every ~26 s at 160 MHz - unsigned deltas
     stay correct as long as one loop pass is shorter than that. */
  if ((now - s_win_start) < SystemCoreClock)
  {
    return;
  }
  s_acc.total = now - s_win_start;
  s_last      = s_acc;
  s_have_last = 1U;
  memset(&s_acc, 0, sizeof(s_acc));
  s_win_start = now;

#if DIAG_CPU_UART
  uart_fold(&s_last);
  if (++s_uart_windows >= DIAG_CPU_UART_PERIOD_S)
  {
    uart_report();
    memset(&s_uart, 0, sizeof(s_uart));
    s_uart_windows = 0U;
    /* Leave the ~1-2 ms blocking UART print out of the next pass and the
       next window, so the report doesn't measure itself. */
    s_loop_prev = cycles();
    s_win_start = s_loop_prev;
  }
#endif
}

void diag_cpu_begin(DiagSect s)
{
  s_t0[s] = cycles();
}

void diag_cpu_end(DiagSect s)
{
  const uint32_t d = cycles() - s_t0[s];

  s_acc.cyc[s] += d;
  if (d > s_acc.max[s]) { s_acc.max[s] = d; }
}

uint8_t diag_cpu_rows(DiagRow *rows, uint8_t max)
{
  uint8_t n = 0U;
  char t[12];
  uint32_t pm;

  if (max < DIAG_CPU_ROWS)
  {
    return 0U;
  }

  snprintf(rows[n].label, sizeof(rows[n].label), "LOOPS/S:");
  if (s_have_last)
  {
    /* window is ~1 s, scale anyway in case a slow pass stretched it */
    snprintf(rows[n].value, sizeof(rows[n].value), "%lu",
             (unsigned long)(((uint64_t)s_last.loops * SystemCoreClock) / s_last.total));
  }
  n++;

  snprintf(rows[n].label, sizeof(rows[n].label), "LOOPMAX:");
  if (s_have_last) { fmt_time(rows[n].value, sizeof(rows[n].value), s_last.loop_max); }
  n++;

  for (uint8_t i = 0; i < DIAG_SECT_COUNT; i++, n++)
  {
    snprintf(rows[n].label, sizeof(rows[n].label), "%s:", sect_name[i]);
    if (s_have_last)
    {
      pm = per_mille(s_last.cyc[i], s_last.total);
      fmt_time(t, sizeof(t), s_last.max[i]);
      /* Fits 16 chars for any real figure ("100.0% MAX 999MS"); a longer
         one is cut to the TFT field width, which is fine. */
      #pragma GCC diagnostic push
      #pragma GCC diagnostic ignored "-Wformat-truncation"
      snprintf(rows[n].value, sizeof(rows[n].value), "%lu.%lu%% MAX %s",
               (unsigned long)(pm / 10U), (unsigned long)(pm % 10U), t);
      #pragma GCC diagnostic pop
    }
  }

  snprintf(rows[n].label, sizeof(rows[n].label), "OTHER:");
  if (s_have_last)
  {
    pm = per_mille(other_cycles(&s_last), s_last.total);
    snprintf(rows[n].value, sizeof(rows[n].value), "%lu.%lu%%",
             (unsigned long)(pm / 10U), (unsigned long)(pm % 10U));
  }
  n++;

  if (!s_have_last)
  {
    for (uint8_t i = 0; i < n; i++) { snprintf(rows[i].value, sizeof(rows[i].value), "---"); }
  }
  return n;
}

#endif /* DIAG_CPU_STATS */
