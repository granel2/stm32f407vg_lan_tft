/**
  ******************************************************************************
  * @file    keypad.c
  * @brief   Keypad receiver on I2C1 slave registers - see keypad.h.
  *
  * Register level because the project has no HAL I2C module, and because the
  * one thing this driver needs - deciding ACK/NACK of the tail byte from the
  * CRC check - is not expressible through the HAL slave API. RM0090 ch. 27
  * (target receiver), ES0182 2.10.2 (the I2C cannot NACK the byte it has just
  * checked - hence the extra tail byte in the protocol).
  *
  * Timing (RM0090, target receiver): after each byte the interface sends the
  * ACK pulse (if CR1.ACK is set), THEN sets RXNE. Clearing ACK in the RXNE
  * interrupt of byte k therefore decides the answer to byte k+1. SCL is
  * stretched only after byte k+1 has been received AND acknowledged (BTF), so
  * the decision about the tail must be made while its 8 bits arrive - hence
  * the high IRQ priority and the CRC computed byte by byte along the way.
  *
  * After a NACK the interface sets no STOPF (RM0090 SR1.STOPF note), so ACK is
  * put back in the RXNE of the NACKed byte itself, and keypad_poll() also
  * restores it whenever the bus is idle - otherwise the module's next address
  * would be refused.
  ******************************************************************************
  */
#include "keypad.h"
#include "keypad_config.h"
#include "keypad_page.h"

#include <stdio.h>
#include <string.h>

#include "main.h"

#define I2C_SR1_ERR_MASK  (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF | I2C_SR1_OVR | \
                           I2C_SR1_PECERR | I2C_SR1_TIMEOUT | I2C_SR1_SMBALERT)

static KeypadHandler s_handler;
static uint8_t       s_ok;

/* ---- frame being received (interrupt only) ------------------------------ */

static uint8_t s_buf[KBD_MAX_FRAME];
static uint8_t s_idx;        /* bytes of the current frame received so far */
static uint8_t s_n;          /* N from byte 2; 0xFF until it arrives */
static uint8_t s_crc;        /* running CRC over address + bytes before CRC */
static uint8_t s_crc_ok;     /* CRC byte matched -> tail will be ACKed */
static uint8_t s_dead;       /* frame finished or refused: ignore the rest */
static uint8_t s_nacked;     /* ACK cleared: the next byte gets NACK */

/* ---- queue: written by the interrupt, read by keypad_poll() ------------- */

static KeypadPacket      s_q[KEYPAD_QUEUE_LEN];
static volatile uint16_t s_q_head;   /* next slot the ISR writes */
static volatile uint16_t s_q_tail;   /* next slot the main loop reads */

static volatile KeypadStats s_st;    /* interrupt-side counters */
static uint32_t s_duplicate;         /* main-loop-side counters (keypad_poll) */
static uint32_t s_gap;

/* ---- init ---------------------------------------------------------------- */

void keypad_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  const uint32_t pclk1_mhz = HAL_RCC_GetPCLK1Freq() / 1000000U;
  char msg[96];

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_I2C1_CLK_ENABLE();

  /* PB6 = I2C1_SCL, PB7 = I2C1_SDA (SV5). Open drain, no internal pull-ups:
     the bus is pulled to +5V on the keypad module (pins are FT). */
  gpio.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
  gpio.Mode      = GPIO_MODE_AF_OD;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(GPIOB, &gpio);

  /* Start from a clean peripheral */
  __HAL_RCC_I2C1_FORCE_RESET();
  __HAL_RCC_I2C1_RELEASE_RESET();

  /* A slave needs FREQ = APB1 in MHz (>= 2 for 100 kHz) for its timings;
     CCR/TRISE only matter for a master, set to 100 kHz values anyway. */
  I2C1->CR2   = pclk1_mhz & I2C_CR2_FREQ;
  I2C1->CCR   = (HAL_RCC_GetPCLK1Freq() / (2U * 100000U)) & I2C_CCR_CCR;
  I2C1->TRISE = pclk1_mhz + 1U;
  I2C1->OAR1  = (1U << 14) | ((uint32_t)KBD_HOST_ADDR << 1);   /* bit 14: keep at 1 (RM0090) */

  I2C1->CR1 = I2C_CR1_PE;
  I2C1->CR1 |= I2C_CR1_ACK;           /* ACK is cleared by hardware while PE = 0 */
  I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;

  HAL_NVIC_SetPriority(I2C1_EV_IRQn, KEYPAD_IRQ_PRIORITY, 0);
  HAL_NVIC_SetPriority(I2C1_ER_IRQn, KEYPAD_IRQ_PRIORITY, 0);
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

  s_ok = 1U;
  snprintf(msg, sizeof(msg), "[kbd] I2C1 slave 0x%02X on PB6/PB7 (SV5), APB1 %lu MHz\r\n",
           (unsigned)KBD_HOST_ADDR, (unsigned long)pclk1_mhz);
  Debug_Print(msg);
}

/* ---- interrupt ----------------------------------------------------------- */

static void nack_next(void)
{
  I2C1->CR1 &= ~I2C_CR1_ACK;
  s_nacked = 1U;
}

static void frame_start(void)
{
  s_idx    = 0U;
  s_n      = 0xFFU;
  s_crc    = kbd_crc8_update(0U, (uint8_t)(KBD_HOST_ADDR << 1));   /* write address */
  s_crc_ok = 0U;
  s_dead   = 0U;
}

static void queue_put(void)
{
  const uint16_t next = (uint16_t)((s_q_head + 1U) % KEYPAD_QUEUE_LEN);
  KeypadPacket *p;

  if (next == s_q_tail)
  {
    s_st.overrun++;              /* the module already has our ACK - packet lost */
    return;
  }
  p = &s_q[s_q_head];
  p->type = s_buf[0];
  p->seq  = s_buf[1];
  p->len  = s_n;
  memcpy(p->data, &s_buf[3], s_n);
  s_q_head = next;
  s_st.accepted++;
}

/* One received data byte. late = BTF was already set when we got here: the
   NEXT byte has been received and acknowledged already. */
static void rx_byte(uint8_t b, uint8_t late)
{
  const uint8_t i = s_idx;

  if (s_nacked)
  {
    /* This byte got our NACK; the master stops now. Re-arm ACK so the next
       transfer's address is answered (no STOPF comes after a NACK). */
    I2C1->CR1 |= I2C_CR1_ACK;
    s_nacked = 0U;
    s_dead = 1U;
  }
  if (s_dead)
  {
    return;
  }

  if (i < sizeof(s_buf))
  {
    s_buf[i] = b;
  }
  s_idx = (uint8_t)(i + 1U);

  if (i < 3U)
  {
    s_crc = kbd_crc8_update(s_crc, b);
    if ((i == 2U) && (b > KBD_MAX_DATA))
    {
      s_st.bad_frame++;
      nack_next();
    }
    else if (i == 2U)
    {
      s_n = b;
    }
  }
  else if (i < 3U + s_n)
  {
    s_crc = kbd_crc8_update(s_crc, b);            /* data */
  }
  else if (i == 3U + s_n)
  {
    s_crc_ok = (uint8_t)(b == s_crc);             /* CRC: decide the tail's answer */
    if (late)
    {
      s_st.late++;
    }
    if (!s_crc_ok)
    {
      s_st.crc_err++;
      if (late)
      {
        s_st.lost++;                              /* tail ACKed already: no repeat */
      }
      nack_next();
    }
  }
  else
  {
    if (s_crc_ok)                                 /* tail, ACKed: packet accepted */
    {
      queue_put();
    }
    s_dead = 1U;                                  /* ignore anything after it */
  }
}

void keypad_ev_irq(void)
{
  uint32_t sr1 = I2C1->SR1;

  if ((sr1 & I2C_SR1_ADDR) != 0U)
  {
    (void)I2C1->SR2;                              /* SR1 then SR2 clears ADDR */
    s_st.addr++;
    frame_start();
  }

  /* Drain DR. BTF before the read = the following byte is already in and
     acknowledged (we are late for it). */
  while (((sr1 = I2C1->SR1) & I2C_SR1_RXNE) != 0U)
  {
    const uint8_t late = (uint8_t)((sr1 & I2C_SR1_BTF) != 0U);
    rx_byte((uint8_t)I2C1->DR, late);
  }

  /* Master reading from us - not part of the protocol; answer 0xFF so the
     bus is not held (it ends with AF, cleared in keypad_er_irq()). */
  if ((sr1 & I2C_SR1_TXE) != 0U)
  {
    I2C1->DR = 0xFFU;
  }

  if ((sr1 & I2C_SR1_STOPF) != 0U)
  {
    if (!s_dead && (s_idx > 0U))
    {
      s_st.bad_frame++;                           /* stopped before the tail */
    }
    s_dead = 1U;
    s_nacked = 0U;
    I2C1->CR1 |= I2C_CR1_ACK;                     /* SR1 read + CR1 write clears STOPF */
  }
}

void keypad_er_irq(void)
{
  const uint32_t sr1 = I2C1->SR1;
  const uint32_t err = sr1 & I2C_SR1_ERR_MASK;

  if ((err & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)) != 0U)
  {
    s_st.bus_err++;
  }
  /* AF: end of a master read (see TXE above) - normal for that case */
  if (((err & ~I2C_SR1_AF) != 0U) && !s_dead && (s_idx > 0U))
  {
    s_st.bad_frame++;
  }
  I2C1->SR1 = ~err & 0xFFFFU;                     /* rc_w0: write 0 to clear */
  s_dead = 1U;
  s_nacked = 0U;
  I2C1->CR1 |= I2C_CR1_ACK;
}

/* ---- main loop ----------------------------------------------------------- */

void keypad_set_handler(KeypadHandler h)
{
  s_handler = h;
}

void keypad_get_stats(KeypadStats *s)
{
  HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
  *s = *(const KeypadStats *)&s_st;
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  s->duplicate = s_duplicate;
  s->lost += s_gap;
}

static const char *reason_name(uint8_t r)
{
  switch (r)
  {
    case KBD_END_ENTER:   return "enter";
    case KBD_END_MAXLEN:  return "max length";
    case KBD_END_TIMEOUT: return "timeout";
    case KBD_END_CLEAR:   return "clear";
    default:              return "?";
  }
}

static void log_packet(const KeypadPacket *p)
{
  char msg[96];

  switch (p->type)
  {
    case KBD_PKT_GROUP:
    {
      const uint8_t n = (p->len > 0U) ? (uint8_t)(p->len - 1U) : 0U;
#if KEYPAD_LOG_TEXT
      snprintf(msg, sizeof(msg), "[kbd] #%u GROUP %s: \"%.*s\"\r\n", (unsigned)p->seq,
               reason_name(p->data[0]), (int)n, (const char *)&p->data[1]);
#else
      snprintf(msg, sizeof(msg), "[kbd] #%u GROUP %s: %u chars\r\n", (unsigned)p->seq,
               reason_name(p->data[0]), (unsigned)n);
#endif
      break;
    }
    case KBD_PKT_CANCEL:
      snprintf(msg, sizeof(msg), "[kbd] #%u CANCEL %s\r\n", (unsigned)p->seq,
               reason_name(p->data[0]));
      break;
    case KBD_PKT_KEY:
      snprintf(msg, sizeof(msg), "[kbd] #%u KEY %c %s\r\n", (unsigned)p->seq,
               (char)(p->data[0] & 0x7FU), ((p->data[0] & 0x80U) != 0U) ? "down" : "up");
      break;
    case KBD_PKT_START:
      snprintf(msg, sizeof(msg), "[kbd] #%u START fw %u, reset cause MCUSR 0x%02X\r\n",
               (unsigned)p->seq, (unsigned)p->data[0], (unsigned)p->data[1]);
      break;
    default:
      snprintf(msg, sizeof(msg), "[kbd] #%u unknown type 0x%02X, %u bytes\r\n",
               (unsigned)p->seq, (unsigned)p->type, (unsigned)p->len);
      break;
  }
  Debug_Print(msg);
}

void keypad_poll(void)
{
  static uint8_t  last_seq;          /* 0 = nothing yet */
  static uint32_t last_crc_err, last_late;
  char msg[96];

  if (!s_ok)
  {
    return;
  }

  /* ACK left cleared with no NACKed byte to restore it (master stopped right
     after our NACK decision): put it back while the bus is idle. */
  if (((I2C1->CR1 & I2C_CR1_ACK) == 0U) && ((I2C1->SR2 & I2C_SR2_BUSY) == 0U))
  {
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
    if ((I2C1->SR2 & I2C_SR2_BUSY) == 0U)
    {
      s_nacked = 0U;
      I2C1->CR1 |= I2C_CR1_ACK;
    }
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  }

  while (s_q_tail != s_q_head)
  {
    KeypadPacket *p = &s_q[s_q_tail];

    if (p->type == KBD_PKT_START)
    {
      last_seq = 0U;                 /* module restarted: seq begins again */
    }
    if ((last_seq != 0U) && (p->seq == last_seq))
    {
      s_duplicate++;                 /* module repeated: our ACK did not reach it */
    }
    else
    {
      if (last_seq != 0U)
      {
        const uint8_t expect = (last_seq == 255U) ? 1U : (uint8_t)(last_seq + 1U);
        if (p->seq != expect)
        {
          const uint32_t gap = (p->seq > last_seq) ? (uint32_t)(p->seq - last_seq - 1U)
                                                   : (uint32_t)(p->seq + 254U - last_seq);
          s_gap += gap;
          snprintf(msg, sizeof(msg), "[kbd] %lu packet(s) lost before #%u\r\n",
                   (unsigned long)gap, (unsigned)p->seq);
          Debug_Print(msg);
          Keypad_PageLost(gap);
        }
      }
      last_seq = p->seq;
      log_packet(p);
      Keypad_PagePacket(p);
      if (s_handler != NULL)
      {
        s_handler(p);
      }
    }
    memset(p, 0, sizeof(*p));        /* do not keep typed passwords in RAM */
    s_q_tail = (uint16_t)((s_q_tail + 1U) % KEYPAD_QUEUE_LEN);
  }

  /* Errors the module recovers from by itself - report when they happen */
  if ((s_st.crc_err != last_crc_err) || (s_st.late != last_late))
  {
    snprintf(msg, sizeof(msg), "[kbd] errors: crc %lu (repeated by module), late %lu\r\n",
             (unsigned long)s_st.crc_err, (unsigned long)s_st.late);
    Debug_Print(msg);
    Keypad_PageErrors(s_st.crc_err, s_st.late);
    last_crc_err = s_st.crc_err;
    last_late = s_st.late;
  }
}
