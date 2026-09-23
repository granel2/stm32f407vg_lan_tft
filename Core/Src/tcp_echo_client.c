/**
  ******************************************************************************
  * @file    tcp_echo_client.c
  * @brief   Minimal TCP client (lwIP raw API).
  *
  *  TX_BUF_SIZE bytes buffered for sending; overflow → drop.
  *  RX_BUF_SIZE bytes buffered for UART print; overflow → drop.
  *  Neither buffer causes a disconnect on overflow.
  ******************************************************************************
  */
#include "tcp_echo_client.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "ethernetif.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
extern struct netif gnetif;

/* Consecutive failed connection attempts (SYN never ACKed, never RST'd)
   before assuming the ETH DMA ring is wedged and forcing a peripheral
   reset. Previously this state required a physical board reset to clear. */
#define ETH_RESET_AFTER_FAILS  3U
static uint8_t s_fail_count;

/* Consecutive ethernetif_reset() calls with no successful connection in
   between. Observed on real hardware: resetting every ~15s (3 fails *
   RECONNECT_INTERVAL_MS) can itself be the problem - repeated link
   down/up flaps in a short window trip flap-protection on some switches/
   routers, which then blackhole the port for a cooldown period, so
   resetting *faster* only makes recovery less likely. Back off
   exponentially (5s, 10s, 20s, 40s, capped at 60s) between resets instead
   of immediately retrying every RECONNECT_INTERVAL_MS. Reset to 0 on any
   successful connection. */
#define ETH_RESET_BACKOFF_CAP_MS  60000U
static uint8_t s_reset_count;

typedef enum
{
  ECHO_STATE_IDLE = 0,
  ECHO_STATE_CONNECTING,
  ECHO_STATE_CONNECTED
} echo_state_t;

static struct tcp_pcb *s_pcb;
static echo_state_t    s_state = ECHO_STATE_IDLE;
static uint32_t        s_next_action_tick;
static uint32_t        s_msg_counter;

/* ── TX buffer: data queued to send → server ───────────────────────────── */
#define TX_BUF_SIZE  1024U
static char     s_tx_buf[TX_BUF_SIZE];
static uint16_t s_tx_len;          /* valid bytes starting at s_tx_buf[0] */

/* ── RX buffer: data received from server → UART ───────────────────────── */
#define RX_BUF_SIZE  1024U
static char     s_rx_buf[RX_BUF_SIZE];
static uint16_t s_rx_len;

/* ── "last received" snapshot, for the TFT status screen ───────────────── */
/* Independent of s_rx_buf above: taken (and immediately truncated to this
   size) at the same point s_rx_buf is drained to UART, so a slow/blocked
   display update can never affect the debug log path or vice versa. */
#define LAST_RX_SNAPSHOT_SIZE  96U
static char     s_last_rx[LAST_RX_SNAPSHOT_SIZE];
static uint16_t s_last_rx_len;
static uint8_t  s_last_rx_fresh;   /* 1 = not yet consumed by tcp_echo_client_take_last_rx() */

/* ── UART RX line accumulator (RXNE polling, no HAL lock) ───────────────── */
#define UART_LINE_BUF  256U
static char     s_uart_line[UART_LINE_BUF];
static uint16_t s_uart_line_pos;
static uint8_t  s_uart_line_ready;

#define RECONNECT_INTERVAL_MS  5000U
#define SEND_INTERVAL_MS       2000U

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Direct UART write, used for short diagnostic strings only. */
static void uart_puts(const char *s)
{
  HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), 200);
}

/* Append bytes to TX buffer; silently drop what doesn't fit. */
static void tx_push(const char *data, uint16_t len)
{
  uint16_t space = TX_BUF_SIZE - s_tx_len;
  if (len > space) { len = space; }
  if (len) { memcpy(s_tx_buf + s_tx_len, data, len); s_tx_len += len; }
}

/* Send as many buffered bytes as the TCP send window allows. */
static void tx_flush(void)
{
  u16_t window, n;

  if (s_pcb == NULL || s_tx_len == 0) { return; }
  window = tcp_sndbuf(s_pcb);
  if (window == 0) { return; }

  n = (s_tx_len < window) ? s_tx_len : window;

  {
    err_t we = tcp_write(s_pcb, s_tx_buf, n, TCP_WRITE_FLAG_COPY);
    if (we == ERR_MEM)
    {
      /* Temporary heap shortage; leave data in TX buffer and retry. */
      return;
    }
    if (we != ERR_OK)
    {
      uart_puts("[tcp_echo] tcp_write error, reconnecting\r\n");
      tcp_abort(s_pcb);
      s_pcb    = NULL;
      s_tx_len = 0;
      s_state  = ECHO_STATE_IDLE;
      s_next_action_tick = HAL_GetTick() + RECONNECT_INTERVAL_MS;
      return;
    }
  }

  tcp_output(s_pcb);
  /* Compact: move unsent remainder to the front. */
  if (n < s_tx_len) { memmove(s_tx_buf, s_tx_buf + n, s_tx_len - n); }
  s_tx_len -= n;
}

/* Append bytes to RX buffer; silently drop what doesn't fit. */
static void rx_push(const char *data, uint16_t len)
{
  uint16_t space = RX_BUF_SIZE - s_rx_len;
  if (len > space) { len = space; }
  if (len) { memcpy(s_rx_buf + s_rx_len, data, len); s_rx_len += len; }
}

/* ── TCP callbacks ───────────────────────────────────────────────────────── */

static void reset_to_idle(void)
{
  s_pcb    = NULL;
  s_tx_len = 0;
  s_state  = ECHO_STATE_IDLE;
  s_next_action_tick = HAL_GetTick() + RECONNECT_INTERVAL_MS;
}

static err_t on_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(tpcb);

  if (err != ERR_OK) { if (p) { pbuf_free(p); } return err; }

  if (p == NULL)
  {
    rx_push("[tcp_echo] server closed\r\n", 26);
    tcp_close(tpcb);
    reset_to_idle();
    return ERR_OK;
  }

  /* pbuf_copy_partial handles chained pbufs (p->tot_len, not just p->len). */
  {
    uint16_t space = RX_BUF_SIZE - s_rx_len;
    uint16_t n = (p->tot_len < space) ? (uint16_t)p->tot_len : space;
    if (n) { pbuf_copy_partial(p, s_rx_buf + s_rx_len, n, 0); s_rx_len += n; }
  }

  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
  /* PCB already freed by lwIP when this callback fires — do NOT touch s_pcb. */
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(err);
  rx_push("[tcp_echo] connection error, will retry\r\n", 41);

  /* Only count failures of attempts to *establish* a connection. A drop
     of an already-working connection is normal life, not a wedged NIC. */
  if (s_state == ECHO_STATE_CONNECTING)
  {
    s_fail_count++;
    if (s_fail_count >= ETH_RESET_AFTER_FAILS)
    {
      uint32_t backoff;
      char msg[64];

      rx_push("[tcp_echo] too many failed connects, resetting ETH\r\n", 53);
      ethernetif_reset(&gnetif);
      s_fail_count = 0;

      backoff = RECONNECT_INTERVAL_MS << (s_reset_count < 4U ? s_reset_count : 4U);
      if (backoff > ETH_RESET_BACKOFF_CAP_MS) { backoff = ETH_RESET_BACKOFF_CAP_MS; }
      if (s_reset_count < 255U) { s_reset_count++; }

      snprintf(msg, sizeof(msg), "[tcp_echo] backoff %lums before next try\r\n",
               (unsigned long)backoff);
      rx_push(msg, (uint16_t)strlen(msg));

      reset_to_idle();
      s_next_action_tick = HAL_GetTick() + backoff;
      return;
    }
  }
  reset_to_idle();
}

static err_t on_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(tpcb);

  if (err != ERR_OK) { reset_to_idle(); return err; }

  rx_push("[tcp_echo] connected\r\n", 22);
  s_fail_count = 0;
  s_reset_count = 0;
  s_state = ECHO_STATE_CONNECTED;
  s_next_action_tick = HAL_GetTick() + SEND_INTERVAL_MS;
  return ERR_OK;
}

static void start_connect(void)
{
  ip_addr_t ip;
  err_t err;

  IP4_ADDR(&ip, TCP_ECHO_SERVER_IP0, TCP_ECHO_SERVER_IP1,
           TCP_ECHO_SERVER_IP2, TCP_ECHO_SERVER_IP3);

  s_pcb = tcp_new();
  if (s_pcb == NULL)
  {
    rx_push("[tcp_echo] tcp_new() failed (PCB pool exhausted?)\r\n", 51);
    reset_to_idle();
    return;
  }

  tcp_arg(s_pcb, NULL);
  tcp_err(s_pcb, on_err);
  tcp_recv(s_pcb, on_recv);
  /* Enable keepalive so lwIP probes the server after 10 s of idle,
     and calls on_err → reconnect if the server disappears silently. */
  s_pcb->so_options |= SOF_KEEPALIVE;

  s_state = ECHO_STATE_CONNECTING;
  err = tcp_connect(s_pcb, &ip, TCP_ECHO_SERVER_PORT, on_connected);
  if (err != ERR_OK)
  {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "[tcp_echo] tcp_connect() failed err=%d heap=%u/%u tcp_pcb=%u/%u tcp_seg=%u/%u pbuf=%u/%u\r\n",
             (int)err,
             (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.avail,
             (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used, (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->avail,
             (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->used, (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->avail,
             (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->used, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->avail);
    rx_push(msg, (uint16_t)strlen(msg));
    tcp_abort(s_pcb);
    reset_to_idle();
    return;
  }
  s_next_action_tick = HAL_GetTick() + RECONNECT_INTERVAL_MS;
}

/* ── UART RX polling ─────────────────────────────────────────────────────── */

static void poll_uart_rx(void)
{
  while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) && !s_uart_line_ready)
  {
    uint8_t b = (uint8_t)(huart1.Instance->DR & 0xFFU);
    if (b == '\r' || b == '\n')
    {
      if (s_uart_line_pos > 0)
      {
        s_uart_line[s_uart_line_pos++] = '\n';
        s_uart_line[s_uart_line_pos]   = '\0';
        s_uart_line_ready = 1;
      }
    }
    else if (s_uart_line_pos < UART_LINE_BUF - 2U)
    {
      s_uart_line[s_uart_line_pos++] = (char)b;
    }
    /* Byte beyond UART_LINE_BUF silently dropped. */
  }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

uint16_t tcp_echo_client_sndqueuelen(void)
{
  return (s_pcb != NULL) ? tcp_sndqueuelen(s_pcb) : 0U;
}

char tcp_echo_client_state_char(void)
{
  switch (s_state)
  {
    case ECHO_STATE_CONNECTING: return 'C';
    case ECHO_STATE_CONNECTED:  return 'E';
    default:                    return 'I';
  }
}

uint16_t tcp_echo_client_take_last_rx(char *buf, uint16_t buf_size)
{
  uint16_t n;

  if ((s_last_rx_fresh == 0U) || (buf_size == 0U)) { return 0U; }

  n = (s_last_rx_len < (buf_size - 1U)) ? s_last_rx_len : (buf_size - 1U);
  memcpy(buf, s_last_rx, n);
  buf[n] = '\0';
  s_last_rx_fresh = 0U;
  return n;
}

void tcp_echo_client_init(void)
{
  s_pcb          = NULL;
  s_state        = ECHO_STATE_IDLE;
  s_tx_len       = 0;
  s_rx_len       = 0;
  s_msg_counter  = 0;
  s_fail_count   = 0;
  s_reset_count  = 0;
  s_last_rx_len   = 0;
  s_last_rx_fresh = 0;
  s_uart_line_pos   = 0;
  s_uart_line_ready = 0;
  s_next_action_tick = HAL_GetTick();
  uart_puts("[tcp_echo] ready\r\n");
}

void tcp_echo_client_poll(void)
{
  /* 1. Drain RX buffer to debug UART (never blocks TCP callbacks). */
  if (s_rx_len > 0)
  {
    HAL_UART_Transmit(&huart1, (uint8_t *)s_rx_buf, s_rx_len, 50);

    {
      uint16_t n = (s_rx_len < LAST_RX_SNAPSHOT_SIZE - 1U) ? s_rx_len : (LAST_RX_SNAPSHOT_SIZE - 1U);
      memcpy(s_last_rx, s_rx_buf, n);
      s_last_rx[n]   = '\0';
      s_last_rx_len  = n;
      s_last_rx_fresh = 1U;
    }

    s_rx_len = 0;
  }

  /* 2. Read UART input; queue complete lines into TX buffer. */
  poll_uart_rx();
  if (s_uart_line_ready)
  {
    if (s_state == ECHO_STATE_CONNECTED)
    {
      tx_push(s_uart_line, (uint16_t)strlen(s_uart_line));
    }
    s_uart_line_pos   = 0;
    s_uart_line_ready = 0;
  }

  /* 3. Push buffered TX data over TCP. */
  if (s_state == ECHO_STATE_CONNECTED) { tx_flush(); }

  /* 4. Periodic: connect / send ping. */
  if ((int32_t)(HAL_GetTick() - s_next_action_tick) < 0) { return; }

  switch (s_state)
  {
    case ECHO_STATE_IDLE:
      start_connect();
      break;

    case ECHO_STATE_CONNECTED:
    {
      char msg[64];
      snprintf(msg, sizeof(msg), "ping #%lu from stm32f407\r\n",
               (unsigned long)s_msg_counter++);
      tx_push(msg, (uint16_t)strlen(msg));
      s_next_action_tick = HAL_GetTick() + SEND_INTERVAL_MS;
      break;
    }

    case ECHO_STATE_CONNECTING:
    default:
      /* SYN sent but no ACK within RECONNECT_INTERVAL_MS → give up.
         tcp_abort() synchronously invokes on_err() (registered via
         tcp_err()), which already does all cleanup - including
         reset_to_idle() and, every 3rd failure, the exponential backoff
         override of s_next_action_tick. Calling reset_to_idle() again
         here unconditionally was clobbering that backoff back down to
         the plain 5 s RECONNECT_INTERVAL_MS every single time, which is
         why the logged "backoff Nms" never actually held for N ms. */
      if (s_pcb != NULL)
      {
        tcp_abort(s_pcb);
      }
      else
      {
        rx_push("[tcp_echo] connect timed out, retrying\r\n", 40);
        reset_to_idle();
      }
      break;
  }
}
