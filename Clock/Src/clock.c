/**
  ******************************************************************************
  * @file    clock.c
  * @brief   Minimal SNTP client + local time - see clock.h.
  *
  * One request per attempt (RFC 4330 client mode), no sub-second slewing:
  * the reply's transmit timestamp plus half the round trip becomes the new
  * base, and time is counted from HAL_GetTick() from there. Plenty for a
  * wall clock showing seconds.
  ******************************************************************************
  */
#include "clock.h"
#include "clock_config.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#define NTP_PORT            123U
#define NTP_PACKET_LEN      48U
#define NTP_UNIX_OFFSET     2208988800UL  /* 1900-01-01 -> 1970-01-01, s */

typedef enum
{
  SNTP_IDLE = 0,    /* waiting for s_next_try */
  SNTP_DNS,         /* dns_gethostbyname() in progress */
  SNTP_WAIT_REPLY   /* request sent */
} SntpState;

static struct udp_pcb *s_pcb;
static SntpState s_state = SNTP_IDLE;
static ip_addr_t s_server;
static uint32_t  s_attempt_start;   /* HAL tick the attempt began */
static uint32_t  s_sent_tick;       /* HAL tick the request went out */
static uint32_t  s_next_try;        /* HAL tick of the next attempt */
static uint8_t   s_failed;

/* Time base: at HAL tick s_base_tick it was s_base_unix + s_base_ms/1000 */
static uint8_t   s_synced;
static uint32_t  s_base_unix;
static uint32_t  s_base_ms;
static uint32_t  s_base_tick;
static uint32_t  s_last_sync_utc;   /* UTC second of the last good reply */

/* ---- calendar helpers (proleptic Gregorian, days since 1970-01-01) ------ */

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
  y -= (m <= 2U) ? 1 : 0;
  {
    const int32_t  era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153U * (m + (m > 2U ? (uint32_t)-3 : 9U)) + 2U) / 5U + d - 1U;
    const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int32_t)doe - 719468;
  }
}

static void civil_from_days(int32_t z, uint16_t *y, uint8_t *m, uint8_t *d)
{
  z += 719468;
  {
    const int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const uint32_t mp  = (5U * doy + 2U) / 153U;
    const uint32_t dd  = doy - (153U * mp + 2U) / 5U + 1U;
    const uint32_t mm  = (mp < 10U) ? mp + 3U : mp - 9U;

    *y = (uint16_t)((int32_t)yoe + era * 400 + (mm <= 2U ? 1 : 0));
    *m = (uint8_t)mm;
    *d = (uint8_t)dd;
  }
}

/* 0 = Monday .. 6 = Sunday; 1970-01-01 was a Thursday */
static uint8_t weekday(int32_t days)
{
  return (uint8_t)(((days % 7) + 7 + 3) % 7);
}

#if CLOCK_EU_DST
/* Unix time of the last Sunday of `month` (31-day month), 01:00 UTC */
static uint32_t last_sunday_0100_utc(uint16_t year, uint8_t month)
{
  const int32_t d31 = days_from_civil(year, month, 31U);
  const int32_t sun = d31 - (int32_t)((weekday(d31) + 1U) % 7U);  /* back to Sunday (6) */

  return (uint32_t)sun * 86400U + 3600U;
}

static uint8_t eu_dst_active(uint32_t unix_utc)
{
  uint16_t y;
  uint8_t  m, d;

  civil_from_days((int32_t)(unix_utc / 86400U), &y, &m, &d);
  return (uint8_t)((unix_utc >= last_sunday_0100_utc(y, 3U)) &&
                   (unix_utc <  last_sunday_0100_utc(y, 10U)));
}
#endif

/* ---- SNTP ---------------------------------------------------------------- */

static void attempt_done(uint8_t ok)
{
  s_state  = SNTP_IDLE;
  s_failed = (uint8_t)(ok ? 0U : 1U);
  s_next_try = HAL_GetTick() + 1000U *
               (ok ? CLOCK_RESYNC_S : (s_synced ? CLOCK_RETRY_SET_S : CLOCK_RETRY_UNSET_S));
}

static void send_request(void)
{
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_PACKET_LEN, PBUF_RAM);

  if (p == NULL)
  {
    attempt_done(0U);
    return;
  }
  memset(p->payload, 0, NTP_PACKET_LEN);
  ((uint8_t *)p->payload)[0] = 0x1BU;  /* LI 0, version 3, mode 3 (client) */

  s_sent_tick = HAL_GetTick();
  if (udp_sendto(s_pcb, p, &s_server, NTP_PORT) == ERR_OK)
  {
    s_state = SNTP_WAIT_REPLY;
  }
  else
  {
    attempt_done(0U);
  }
  pbuf_free(p);
}

static void dns_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
  (void)name;
  (void)arg;

  if (s_state != SNTP_DNS)
  {
    return;  /* attempt already timed out or network dropped */
  }
  if (ipaddr == NULL)
  {
    Debug_Print("[clock] DNS lookup of " CLOCK_NTP_SERVER " failed\r\n");
    attempt_done(0U);
    return;
  }
  ip_addr_copy(s_server, *ipaddr);
  send_request();
}

static uint32_t be32(const uint8_t *b)
{
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  uint8_t  pkt[NTP_PACKET_LEN];
  uint32_t now = HAL_GetTick();
  (void)arg;
  (void)pcb;

  if ((s_state != SNTP_WAIT_REPLY) || (port != NTP_PORT) || !ip_addr_cmp(addr, &s_server) ||
      (pbuf_copy_partial(p, pkt, NTP_PACKET_LEN, 0) != NTP_PACKET_LEN))
  {
    pbuf_free(p);
    return;
  }
  pbuf_free(p);

  /* mode 4 = server; stratum 0 = "kiss-o'-death" (rate limited etc.) */
  if (((pkt[0] & 0x07U) != 4U) || (pkt[1] == 0U))
  {
    Debug_Print("[clock] NTP server refused the request\r\n");
    attempt_done(0U);
    return;
  }

  {
    /* Transmit timestamp: seconds since 1900 + 32-bit fraction. Unsigned
       wrap keeps the Unix conversion right past the 2036 NTP era roll. */
    const uint32_t rtt  = now - s_sent_tick;
    const uint32_t frac = (uint32_t)(((uint64_t)be32(&pkt[44]) * 1000U) >> 32);
    char msg[96];

    s_base_unix = be32(&pkt[40]) - NTP_UNIX_OFFSET;
    s_base_ms   = frac + rtt / 2U;
    s_base_tick = now;
    s_synced    = 1U;
    s_last_sync_utc = s_base_unix + s_base_ms / 1000U;
    attempt_done(1U);

    {
      ClockTime t = {0};
      (void)clock_get(&t);  /* s_synced was just set, so this fills t */
      snprintf(msg, sizeof(msg), "[clock] synced %02u.%02u.%04u %02u:%02u:%02u local (%s, rtt %lu ms)\r\n",
               t.day, t.month, t.year, t.hour, t.min, t.sec,
               ipaddr_ntoa(&s_server), (unsigned long)rtt);
      Debug_Print(msg);
    }
  }
}

void clock_init(void)
{
  s_pcb = udp_new();
  if (s_pcb == NULL)
  {
    Debug_Print("[clock] udp_new failed - no free UDP pcb (MEMP_NUM_UDP_PCB)\r\n");
    return;
  }
  (void)udp_bind(s_pcb, IP_ADDR_ANY, 0U);
  udp_recv(s_pcb, udp_recv_cb, NULL);
  s_next_try = HAL_GetTick();
}

void clock_poll(uint8_t net_ready)
{
  const uint32_t now = HAL_GetTick();

  /* Fold elapsed time into the base once a day, so the millisecond sum in
     utc_now() can't wrap (2^32 ms = 49 days) if syncs keep failing. */
  if (s_synced && ((now - s_base_tick) >= 86400000U))
  {
    const uint32_t ms = s_base_ms + (now - s_base_tick);
    s_base_unix += ms / 1000U;
    s_base_ms    = ms % 1000U;
    s_base_tick  = now;
  }

  if (s_pcb == NULL)
  {
    return;
  }
  if (!net_ready)
  {
    if (s_state != SNTP_IDLE)
    {
      s_state = SNTP_IDLE;       /* drop it; a late DNS answer is ignored */
      s_next_try = now + 1000U;  /* try soon after the IP is back */
    }
    return;
  }

  if (s_state != SNTP_IDLE)
  {
    if ((now - s_attempt_start) >= CLOCK_ATTEMPT_TIMEOUT_MS)
    {
      Debug_Print((s_state == SNTP_DNS) ? "[clock] DNS lookup timed out\r\n"
                                        : "[clock] no reply from NTP server\r\n");
      attempt_done(0U);
    }
    return;
  }
  if ((int32_t)(now - s_next_try) < 0)
  {
    return;
  }

  /* Static-IP mode gets no DNS server from DHCP - ask the gateway. (With
     DHCP, the server from the lease is already set, or also nothing.) */
  if (ip_addr_isany(dns_getserver(0U)) && (netif_default != NULL))
  {
    ip_addr_t gw;
    ip_addr_copy_from_ip4(gw, *netif_ip4_gw(netif_default));
    dns_setserver(0U, &gw);
  }

  s_attempt_start = now;
  s_state = SNTP_DNS;
  switch (dns_gethostbyname(CLOCK_NTP_SERVER, &s_server, dns_found, NULL))
  {
    case ERR_OK:          /* dotted IP, or already in the DNS cache */
      send_request();
      break;
    case ERR_INPROGRESS:  /* dns_found() continues */
      break;
    default:
      Debug_Print("[clock] DNS lookup could not start\r\n");
      attempt_done(0U);
      break;
  }
}

static uint32_t utc_to_local(uint32_t utc, uint8_t *dst)
{
  uint8_t on = 0U;

#if CLOCK_EU_DST
  on = eu_dst_active(utc);
#endif
  if (dst != NULL) { *dst = on; }
  return (uint32_t)((int32_t)utc + (CLOCK_TZ_OFFSET_MIN * 60) + (on ? 3600 : 0));
}

static uint32_t utc_now(void)
{
  return s_base_unix + (s_base_ms + (HAL_GetTick() - s_base_tick)) / 1000U;
}

static void split_local(uint32_t utc, ClockTime *t)
{
  const uint32_t lt   = utc_to_local(utc, &t->dst);
  const int32_t  days = (int32_t)(lt / 86400U);
  const uint32_t sod  = lt % 86400U;

  civil_from_days(days, &t->year, &t->month, &t->day);
  t->wday = weekday(days);
  t->hour = (uint8_t)(sod / 3600U);
  t->min  = (uint8_t)((sod / 60U) % 60U);
  t->sec  = (uint8_t)(sod % 60U);
}

uint8_t clock_get(ClockTime *t)
{
  if (!s_synced)
  {
    return 0U;
  }
  split_local(utc_now(), t);
  return 1U;
}

uint8_t clock_last_sync(ClockTime *t)
{
  if (!s_synced)
  {
    return 0U;
  }
  split_local(s_last_sync_utc, t);
  return 1U;
}

uint32_t clock_sync_age_s(void)
{
  return s_synced ? (HAL_GetTick() - s_base_tick) / 1000U : UINT32_MAX;
}

uint8_t clock_last_failed(void)
{
  return s_failed;
}

int32_t clock_utc_offset_min(void)
{
  uint8_t dst = 0U;

  if (s_synced)
  {
    (void)utc_to_local(utc_now(), &dst);
  }
  return CLOCK_TZ_OFFSET_MIN + (dst ? 60 : 0);
}
