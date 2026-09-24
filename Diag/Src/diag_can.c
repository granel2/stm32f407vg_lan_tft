/**
  ******************************************************************************
  * @file    diag_can.c
  * @brief   CAN1 silent-loopback self-test - see diag_can.h.
  ******************************************************************************
  */
#include "diag_can.h"

#if DIAG_CAN_LOOPBACK

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "can_bus.h"

#define TEST_ID_STD      0x7E1U
#define TEST_ID_EXT      0x18DAF1E1U
#define TEST_PERIOD_MS   1000U
#define TEST_TIMEOUT_MS  100U

static CanFrame s_sent;          /* frame waiting to come back */
static uint8_t  s_waiting;
static uint32_t s_sent_at;
static uint32_t s_next_send;
static uint32_t s_seq;
static uint32_t s_next_report;

/* This report period / since boot */
static uint32_t s_n_sent, s_n_ok, s_n_lost, s_n_bad;
static uint32_t s_total_ok, s_total_fail;

static void on_rx(const CanFrame *f)
{
  if (!s_waiting)
  {
    s_n_bad++;  /* nothing outstanding - a stray or duplicate frame */
    return;
  }
  if ((f->id == s_sent.id) && (f->ext == s_sent.ext) && (f->rtr == s_sent.rtr) &&
      (f->len == s_sent.len) && (memcmp(f->data, s_sent.data, f->len) == 0))
  {
    s_n_ok++;
    s_total_ok++;
  }
  else
  {
    s_n_bad++;
    s_total_fail++;
  }
  s_waiting = 0U;
}

void diag_can_init(void)
{
  can_bus_set_loopback(1U);
  can_bus_set_rx_handler(on_rx);
  s_next_send   = HAL_GetTick() + TEST_PERIOD_MS;
  /* Half a send period off the send times: at report time no frame is
     in flight, so the counts of each period add up. */
  s_next_report = HAL_GetTick() + DIAG_CAN_REPORT_S * 1000U + TEST_PERIOD_MS / 2U;
}

void diag_can_poll(void)
{
  const uint32_t now = HAL_GetTick();

  if (s_waiting && ((now - s_sent_at) >= TEST_TIMEOUT_MS))
  {
    s_waiting = 0U;
    s_n_lost++;
    s_total_fail++;
  }

  if (!s_waiting && ((int32_t)(now - s_next_send) >= 0))
  {
    s_next_send += TEST_PERIOD_MS;  /* fixed cadence - stays half a period off the reports */
    s_seq++;

    memset(&s_sent, 0, sizeof(s_sent));
    s_sent.ext = (uint8_t)(s_seq & 1U);        /* alternate 11-bit / 29-bit */
    s_sent.id  = s_sent.ext ? TEST_ID_EXT : TEST_ID_STD;
    s_sent.len = 8U;
    s_sent.data[0] = (uint8_t)(s_seq >> 24);
    s_sent.data[1] = (uint8_t)(s_seq >> 16);
    s_sent.data[2] = (uint8_t)(s_seq >> 8);
    s_sent.data[3] = (uint8_t)s_seq;
    s_sent.data[4] = 0xCAU;
    s_sent.data[5] = 0xFEU;
    s_sent.data[6] = 0x55U;                     /* 0101... / 1010... bit */
    s_sent.data[7] = 0xAAU;                     /* patterns for stuffing */

    s_n_sent++;
    if (can_bus_send(&s_sent))
    {
      s_waiting = 1U;
      s_sent_at = now;
    }
    else
    {
      s_n_lost++;  /* driver down or no free mailbox */
      s_total_fail++;
    }
  }

  if ((int32_t)(now - s_next_report) >= 0)
  {
    char msg[128];

    s_next_report += DIAG_CAN_REPORT_S * 1000U;  /* keep the half-period offset */
    snprintf(msg, sizeof(msg),
             "[diag-can] loopback %s: sent %lu ok %lu lost %lu bad %lu (total ok %lu fail %lu)\r\n",
             ((s_n_ok == s_n_sent) && (s_n_sent > 0U) && (s_n_bad == 0U)) ? "PASS" : "FAIL",
             (unsigned long)s_n_sent, (unsigned long)s_n_ok, (unsigned long)s_n_lost,
             (unsigned long)s_n_bad, (unsigned long)s_total_ok, (unsigned long)s_total_fail);
    Debug_Print(msg);
    s_n_sent = s_n_ok = s_n_lost = s_n_bad = 0U;
  }
}

#endif /* DIAG_CAN_LOOPBACK */
