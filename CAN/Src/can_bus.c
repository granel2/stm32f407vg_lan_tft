/**
  ******************************************************************************
  * @file    can_bus.c
  * @brief   CAN1 driver on bxCAN registers - see can_bus.h.
  *
  * Register level because the project has no HAL CAN module (and the part
  * of bxCAN used here is small): RM0090 chapter 32.
  ******************************************************************************
  */
#include "can_bus.h"
#include "can_config.h"

#include <stdio.h>
#include <string.h>

#include "main.h"

#define INIT_TIMEOUT_MS  100U

static CanRxHandler s_handler;
static uint8_t      s_ok;          /* init succeeded */

/* RX queue: written by the RX0 interrupt, read by can_bus_poll() */
static CanFrame          s_q[CAN_RX_QUEUE_LEN];
static volatile uint16_t s_q_head;  /* next slot the ISR writes */
static volatile uint16_t s_q_tail;  /* next slot the main loop reads */

static volatile uint32_t s_rx;
static volatile uint32_t s_rx_overrun;
static uint32_t          s_tx;
static uint32_t          s_tx_busy;

/* ---- init ---------------------------------------------------------------- */

/* Bit timing for CAN_BITRATE_KBPS from the actual APB1 clock: the first
   time-quanta count (preferring 16) that divides evenly, sample point near
   87.5 %. Returns the BTR value, or 0 if no integer prescaler fits. */
static uint32_t bit_timing(uint32_t pclk1, uint32_t *presc_out, uint32_t *ntq_out)
{
  static const uint8_t tq_pref[] = { 16U, 20U, 18U, 14U, 12U, 10U, 8U, 24U, 25U };
  const uint32_t bitrate = CAN_BITRATE_KBPS * 1000U;

  for (uint32_t i = 0; i < sizeof(tq_pref); i++)
  {
    const uint32_t ntq = tq_pref[i];
    const uint32_t bs2 = (ntq + 4U) / 8U;          /* 16 -> 2, 20 -> 3, 10 -> 1 */
    const uint32_t bs1 = ntq - 1U - bs2;

    if ((bitrate == 0U) || ((pclk1 % (bitrate * ntq)) != 0U) || (bs1 > 16U))
    {
      continue;
    }
    {
      const uint32_t presc = pclk1 / (bitrate * ntq);
      if ((presc == 0U) || (presc > 1024U))
      {
        continue;
      }
      *presc_out = presc;
      *ntq_out   = ntq;
      return (0U << 24)            /* SJW = 1 tq */
           | ((bs2 - 1U) << 20)
           | ((bs1 - 1U) << 16)
           | (presc - 1U);
    }
  }
  return 0U;
}

static uint8_t wait_msr(uint32_t mask, uint32_t want)
{
  const uint32_t t0 = HAL_GetTick();

  while ((CAN1->MSR & mask) != want)
  {
    if ((HAL_GetTick() - t0) > INIT_TIMEOUT_MS)
    {
      return 0U;
    }
  }
  return 1U;
}

uint8_t can_bus_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  const uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
  uint32_t presc = 0U, ntq = 0U, btr;
  char msg[112];

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_CAN1_CLK_ENABLE();

  /* PA11 = CAN1_RX, PA12 = CAN1_TX -> TCAN1044V (U3) */
  gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF9_CAN1;
  HAL_GPIO_Init(GPIOA, &gpio);

  btr = bit_timing(pclk1, &presc, &ntq);
  if (btr == 0U)
  {
    snprintf(msg, sizeof(msg), "[can] no bit timing for %u kbit/s from APB1 %lu Hz\r\n",
             (unsigned)CAN_BITRATE_KBPS, (unsigned long)pclk1);
    Debug_Print(msg);
    return 0U;
  }

  /* Leave sleep, enter initialisation mode */
  CAN1->MCR &= ~CAN_MCR_SLEEP;
  CAN1->MCR |= CAN_MCR_INRQ;
  if (!wait_msr(CAN_MSR_INAK | CAN_MSR_SLAK, CAN_MSR_INAK))
  {
    Debug_Print("[can] CAN1 did not enter init mode\r\n");
    return 0U;
  }

  /* ABOM: recover from bus-off by itself. TXFP: send in request order.
     Automatic retransmission stays on (NART = 0). */
  CAN1->MCR |= CAN_MCR_ABOM | CAN_MCR_TXFP;
  CAN1->MCR &= ~CAN_MCR_NART;
  CAN1->BTR = btr | (CAN_SILENT ? CAN_BTR_SILM : 0U);

  /* Filter bank 0: 32-bit mask mode, mask 0 = accept everything -> FIFO0.
     Banks 14+ belong to CAN2 (reset value of CAN2SB), unused here. */
  CAN1->FMR  |= CAN_FMR_FINIT;
  CAN1->FA1R &= ~1U;
  CAN1->FS1R |= 1U;
  CAN1->FM1R &= ~1U;
  CAN1->FFA1R &= ~1U;
  CAN1->sFilterRegister[0].FR1 = 0U;
  CAN1->sFilterRegister[0].FR2 = 0U;
  CAN1->FA1R |= 1U;
  CAN1->FMR  &= ~CAN_FMR_FINIT;

  /* RX: message pending + overrun on FIFO0 */
  CAN1->IER |= CAN_IER_FMPIE0 | CAN_IER_FOVIE0;
  HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 6, 0);  /* below ETH (5), like the TFT DMA */
  HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);

  /* Leave init: the controller joins once it has seen 11 recessive bits.
     Stuck here = RX held dominant (transceiver unpowered, shorted bus). */
  CAN1->MCR &= ~CAN_MCR_INRQ;
  if (!wait_msr(CAN_MSR_INAK, 0U))
  {
    Debug_Print("[can] CAN1 did not leave init mode - bus stuck dominant? (check J3 wiring/power)\r\n");
    return 0U;
  }

  s_ok = 1U;
  snprintf(msg, sizeof(msg), "[can] CAN1 up: %u kbit/s (APB1 %lu MHz, presc %lu, %lu tq), %s\r\n",
           (unsigned)CAN_BITRATE_KBPS, (unsigned long)(pclk1 / 1000000U), (unsigned long)presc,
           (unsigned long)ntq, CAN_SILENT ? "listen-only" : "normal mode");
  Debug_Print(msg);
  return 1U;
}

/* ---- receive ------------------------------------------------------------- */

void can_bus_rx0_irq(void)
{
  while ((CAN1->RF0R & CAN_RF0R_FMP0) != 0U)
  {
    const uint32_t rir  = CAN1->sFIFOMailBox[0].RIR;
    const uint32_t rdtr = CAN1->sFIFOMailBox[0].RDTR;
    const uint32_t lo   = CAN1->sFIFOMailBox[0].RDLR;
    const uint32_t hi   = CAN1->sFIFOMailBox[0].RDHR;
    const uint16_t next = (uint16_t)((s_q_head + 1U) % CAN_RX_QUEUE_LEN);

    CAN1->RF0R = CAN_RF0R_RFOM0;  /* release the FIFO slot */
    s_rx++;

    if (next == s_q_tail)
    {
      s_rx_overrun++;  /* main loop too slow - drop */
      continue;
    }
    {
      CanFrame *f = &s_q[s_q_head];
      uint8_t len = (uint8_t)(rdtr & CAN_RDT0R_DLC);

      f->ext = (uint8_t)((rir & CAN_RI0R_IDE) != 0U);
      f->rtr = (uint8_t)((rir & CAN_RI0R_RTR) != 0U);
      f->id  = f->ext ? (rir >> 3) : (rir >> 21);
      f->len = (len > 8U) ? 8U : len;
      memcpy(&f->data[0], &lo, 4);
      memcpy(&f->data[4], &hi, 4);
    }
    s_q_head = next;
  }

  if ((CAN1->RF0R & CAN_RF0R_FOVR0) != 0U)
  {
    s_rx_overrun++;               /* hardware FIFO (3 frames) overflowed */
    CAN1->RF0R = CAN_RF0R_FOVR0;
  }
}

void can_bus_set_rx_handler(CanRxHandler h)
{
  s_handler = h;
}

/* ---- transmit ------------------------------------------------------------ */

uint8_t can_bus_send(const CanFrame *f)
{
  static const uint32_t tme[3] = { CAN_TSR_TME0, CAN_TSR_TME1, CAN_TSR_TME2 };
  const uint8_t len = (f->len > 8U) ? 8U : f->len;
  uint32_t lo = 0U, hi = 0U;

  if (!s_ok || CAN_SILENT)
  {
    return 0U;
  }
  for (uint32_t mb = 0; mb < 3U; mb++)
  {
    if ((CAN1->TSR & tme[mb]) == 0U)
    {
      continue;
    }
    memcpy(&lo, &f->data[0], (len < 4U) ? len : 4U);
    if (len > 4U) { memcpy(&hi, &f->data[4], len - 4U); }

    CAN1->sTxMailBox[mb].TDTR = len;
    CAN1->sTxMailBox[mb].TDLR = lo;
    CAN1->sTxMailBox[mb].TDHR = hi;
    CAN1->sTxMailBox[mb].TIR  = (f->ext ? ((f->id & 0x1FFFFFFFU) << 3) | CAN_TI0R_IDE
                                        : ((f->id & 0x7FFU) << 21))
                              | (f->rtr ? CAN_TI0R_RTR : 0U)
                              | CAN_TI0R_TXRQ;
    s_tx++;
    return 1U;
  }
  s_tx_busy++;
  return 0U;
}

/* ---- state, log ---------------------------------------------------------- */

static CanState read_state(uint8_t *tec, uint8_t *rec)
{
  const uint32_t esr = CAN1->ESR;

  if (tec != NULL) { *tec = (uint8_t)((esr & CAN_ESR_TEC) >> 16); }
  if (rec != NULL) { *rec = (uint8_t)((esr & CAN_ESR_REC) >> 24); }
  if (!s_ok)                        { return CAN_STATE_OFF; }
  if ((esr & CAN_ESR_BOFF) != 0U)   { return CAN_STATE_BUS_OFF; }
  if ((esr & CAN_ESR_EPVF) != 0U)   { return CAN_STATE_PASSIVE; }
  if ((esr & CAN_ESR_EWGF) != 0U)   { return CAN_STATE_WARNING; }
  return CAN_STATE_ACTIVE;
}

const char *can_bus_state_name(CanState st)
{
  switch (st)
  {
    case CAN_STATE_ACTIVE:  return "ACTIVE";
    case CAN_STATE_WARNING: return "WARNING";
    case CAN_STATE_PASSIVE: return "PASSIVE";
    case CAN_STATE_BUS_OFF: return "BUS-OFF";
    case CAN_STATE_OFF:
    default:                return "OFF";
  }
}

void can_bus_get_stats(CanStats *s)
{
  s->rx         = s_rx;
  s->rx_overrun = s_rx_overrun;
  s->tx         = s_tx;
  s->tx_busy    = s_tx_busy;
  s->state      = read_state(&s->tec, &s->rec);
}

static void log_frame(const CanFrame *f)
{
  char msg[64];
  int  n;

  n = snprintf(msg, sizeof(msg), f->ext ? "[can] rx %08lX [%u]" : "[can] rx %03lX [%u]",
               (unsigned long)f->id, (unsigned)f->len);
  if (f->rtr)
  {
    n += snprintf(msg + n, sizeof(msg) - (size_t)n, " RTR");
  }
  else
  {
    for (uint8_t i = 0; (i < f->len) && (n > 0) && ((size_t)n < sizeof(msg)); i++)
    {
      n += snprintf(msg + n, sizeof(msg) - (size_t)n, " %02X", f->data[i]);
    }
  }
  if ((n > 0) && ((size_t)n < sizeof(msg) - 2U))
  {
    memcpy(msg + n, "\r\n", 3);
  }
  Debug_Print(msg);
}

void can_bus_poll(void)
{
  static uint32_t win_start, logged, skipped;
  static CanState last_state = CAN_STATE_OFF;
#if CAN_STATS_PERIOD_S
  static uint32_t stats_start, stats_rx, stats_tx, stats_ovr;
#endif
  const uint32_t now = HAL_GetTick();
  char msg[112];

  if (!s_ok)
  {
    return;
  }

  /* Drain the queue: log (rate-limited), then the application handler */
  while (s_q_tail != s_q_head)
  {
    const CanFrame *f = &s_q[s_q_tail];

    if ((now - win_start) >= 1000U)
    {
      if (skipped > 0U)
      {
        snprintf(msg, sizeof(msg), "[can] ... %lu more frames not logged\r\n", (unsigned long)skipped);
        Debug_Print(msg);
      }
      win_start = now;
      logged = 0U;
      skipped = 0U;
    }
    if (logged < CAN_LOG_MAX_PER_S) { log_frame(f); logged++; }
    else                            { skipped++; }

    if (s_handler != NULL)
    {
      s_handler(f);
    }
    s_q_tail = (uint16_t)((s_q_tail + 1U) % CAN_RX_QUEUE_LEN);
  }

  /* Error state changes right away (bus-off etc.) */
  {
    uint8_t tec, rec;
    const CanState st = read_state(&tec, &rec);

    if (st != last_state)
    {
      snprintf(msg, sizeof(msg), "[can] state %s -> %s (tec %u rec %u)\r\n",
               can_bus_state_name(last_state), can_bus_state_name(st), tec, rec);
      Debug_Print(msg);
      last_state = st;
    }
  }

#if CAN_STATS_PERIOD_S
  if ((now - stats_start) >= (CAN_STATS_PERIOD_S * 1000U))
  {
    const uint32_t rx = s_rx, ovr = s_rx_overrun;

    if ((rx != stats_rx) || (s_tx != stats_tx) || (ovr != stats_ovr))
    {
      uint8_t tec, rec;
      const CanState st = read_state(&tec, &rec);

      snprintf(msg, sizeof(msg), "[can] %u s: rx %lu tx %lu lost %lu | total rx %lu, %s tec %u rec %u\r\n",
               (unsigned)CAN_STATS_PERIOD_S, (unsigned long)(rx - stats_rx),
               (unsigned long)(s_tx - stats_tx), (unsigned long)(ovr - stats_ovr),
               (unsigned long)rx, can_bus_state_name(st), tec, rec);
      Debug_Print(msg);
    }
    stats_start = now;
    stats_rx = rx;
    stats_tx = s_tx;
    stats_ovr = ovr;
  }
#endif
}
