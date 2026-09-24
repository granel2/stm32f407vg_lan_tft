/**
  ******************************************************************************
  * @file    clock_config.h
  * @brief   Settings for the network clock (Clock/): NTP server, time zone,
  *          sync intervals. Every value is #ifndef'd, so it can also be
  *          overridden from the compiler command line.
  ******************************************************************************
  */
#ifndef CLOCK_CONFIG_H
#define CLOCK_CONFIG_H

/* NTP server: host name (resolved through DNS - the server DHCP hands out,
   or the gateway in static-IP mode) or a dotted IPv4 address. */
#ifndef CLOCK_NTP_SERVER
#define CLOCK_NTP_SERVER        "pool.ntp.org"
#endif

/* Standard (winter) time offset from UTC in minutes: Estonia = +120. */
#ifndef CLOCK_TZ_OFFSET_MIN
#define CLOCK_TZ_OFFSET_MIN     120
#endif

/* 1 = EU summer time: +1 h from the last Sunday of March 01:00 UTC to the
   last Sunday of October 01:00 UTC. 0 = no daylight saving. */
#ifndef CLOCK_EU_DST
#define CLOCK_EU_DST            1
#endif

/* Re-sync period once the clock is set. Between syncs time runs on the
   SysTick (HSE crystal, drift well under a second per hour). */
#ifndef CLOCK_RESYNC_S
#define CLOCK_RESYNC_S          3600U
#endif

/* Retry after a failed attempt: before the first sync, and after it. */
#ifndef CLOCK_RETRY_UNSET_S
#define CLOCK_RETRY_UNSET_S     10U
#endif
#ifndef CLOCK_RETRY_SET_S
#define CLOCK_RETRY_SET_S       60U
#endif

/* Give up on one attempt (DNS + NTP reply) after this long. */
#ifndef CLOCK_ATTEMPT_TIMEOUT_MS
#define CLOCK_ATTEMPT_TIMEOUT_MS 5000U
#endif

#endif /* CLOCK_CONFIG_H */
