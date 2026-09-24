/**
  ******************************************************************************
  * @file    clock.h
  * @brief   Network clock: time of day from an NTP server (SNTP over lwIP raw
  *          UDP), kept between syncs on the SysTick, converted to local time
  *          (clock_config.h: time zone + EU summer time).
  *
  *          Bare-metal like the rest of the network code: clock_poll() from
  *          the main loop drives everything, the UDP/DNS callbacks run inside
  *          ethernetif_input()/sys_check_timeouts() - same context.
  *
  *          The TFT page for it is Clock/Src/clock_page.c (clock_page.h).
  ******************************************************************************
  */
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint16_t year;
  uint8_t  month;   /* 1..12 */
  uint8_t  day;     /* 1..31 */
  uint8_t  hour;    /* 0..23 */
  uint8_t  min;
  uint8_t  sec;
  uint8_t  wday;    /* 0 = Monday .. 6 = Sunday */
  uint8_t  dst;     /* 1 = summer time in effect */
} ClockTime;

/* Create the UDP socket. Call once after MX_LWIP_Init(). */
void     clock_init(void);

/* Main loop, every pass. net_ready = the board has an IP address; while it
   is 0 no request is sent and an attempt in progress is dropped. */
void     clock_poll(uint8_t net_ready);

/* Current local time. Returns 0 (and leaves *t untouched) until the first
   successful sync. */
uint8_t  clock_get(ClockTime *t);

/* Local time of the last successful sync. Returns 0 if never synced. */
uint8_t  clock_last_sync(ClockTime *t);

/* Seconds since the last successful sync, or UINT32_MAX if never. */
uint32_t clock_sync_age_s(void);

/* 1 if the most recent attempt failed (no DNS answer / no NTP reply). */
uint8_t  clock_last_failed(void);

/* Local offset from UTC in minutes right now (winter or summer). */
int32_t  clock_utc_offset_min(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_H */
