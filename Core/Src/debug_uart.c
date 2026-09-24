/**
  ******************************************************************************
  * @file    debug_uart.c
  * @brief   Ring buffer + DMA for USART1 debug output - see debug_uart.h.
  *
  * Register level on the DMA side (one stream, memory -> USART1->DR): the
  * HAL UART DMA path would also need the USART1 interrupt for its
  * transmit-complete step, for no benefit here.
  ******************************************************************************
  */
#include "debug_uart.h"
#include "main.h"

#include <string.h>

extern UART_HandleTypeDef huart1;

#define TX_STREAM      DMA2_Stream7
#define TX_CHANNEL     4U   /* DMA2 Stream7 / Channel 4 = USART1_TX (RM0090 table 43) */
#define TX_FLAGS_CLR   (DMA_HIFCR_CTCIF7 | DMA_HIFCR_CHTIF7 | DMA_HIFCR_CTEIF7 | \
                        DMA_HIFCR_CDMEIF7 | DMA_HIFCR_CFEIF7)

static uint8_t           s_ring[DEBUG_UART_RING_SIZE];
static volatile uint32_t s_head;      /* next byte written by debug_uart_write() */
static volatile uint32_t s_tail;      /* next byte the DMA sends */
static volatile uint32_t s_dma_len;   /* bytes in the transfer running now, 0 = idle */
static volatile uint32_t s_dropped;
static uint8_t           s_ready;

/* Start the next contiguous chunk [tail .. head or ring end). Caller has
   interrupts masked, or is the DMA interrupt itself. */
static void kick(void)
{
  uint32_t n;

  if ((s_dma_len != 0U) || (s_head == s_tail))
  {
    return;
  }
  n = (s_head > s_tail) ? (s_head - s_tail) : (DEBUG_UART_RING_SIZE - s_tail);

  DMA2->HIFCR     = TX_FLAGS_CLR;
  TX_STREAM->M0AR = (uint32_t)&s_ring[s_tail];
  TX_STREAM->NDTR = n;
  s_dma_len       = n;
  TX_STREAM->CR  |= DMA_SxCR_EN;
}

void debug_uart_init(void)
{
  __HAL_RCC_DMA2_CLK_ENABLE();

  TX_STREAM->CR &= ~DMA_SxCR_EN;
  while ((TX_STREAM->CR & DMA_SxCR_EN) != 0U) { }

  TX_STREAM->PAR = (uint32_t)&USART1->DR;
  TX_STREAM->CR  = (TX_CHANNEL << DMA_SxCR_CHSEL_Pos)
                 | DMA_SxCR_MINC                 /* bytes from the ring, one by one */
                 | DMA_SxCR_DIR_0                /* memory -> peripheral */
                 | DMA_SxCR_TCIE | DMA_SxCR_TEIE;  /* PL = low, byte sizes, no FIFO */
  TX_STREAM->FCR = 0U;                           /* direct mode */
  DMA2->HIFCR    = TX_FLAGS_CLR;

  USART1->CR3 |= USART_CR3_DMAT;

  /* Lowest of the project's interrupts: a late chunk only delays text */
  HAL_NVIC_SetPriority(DMA2_Stream7_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream7_IRQn);

  s_head = s_tail = s_dma_len = 0U;
  s_ready = 1U;
}

void debug_uart_dma_irq(void)
{
  const uint32_t hisr = DMA2->HISR;

  if ((hisr & (DMA_HISR_TCIF7 | DMA_HISR_TEIF7)) == 0U)
  {
    return;
  }
  DMA2->HIFCR = TX_FLAGS_CLR;
  /* On a transfer error the chunk is given up rather than retried forever */
  s_tail    = (s_tail + s_dma_len) % DEBUG_UART_RING_SIZE;
  s_dma_len = 0U;
  kick();
}

void debug_uart_write(const void *data, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)data;

  if (!s_ready)
  {
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)(uintptr_t)p, (uint16_t)len, 100U);
    return;
  }

  while (len > 0U)
  {
    const uint32_t primask = __get_PRIMASK();
    uint32_t used, room, n, first;

    __disable_irq();
    used = (s_head + DEBUG_UART_RING_SIZE - s_tail) % DEBUG_UART_RING_SIZE;
    room = DEBUG_UART_RING_SIZE - 1U - used;

    if (room == 0U)
    {
      __set_PRIMASK(primask);
      /* Waiting needs the DMA interrupt to run: only possible from thread
         mode with interrupts enabled. Anywhere else, drop. */
      if ((primask != 0U) || ((SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) != 0U))
      {
        s_dropped += len;
        return;
      }
      continue;
    }

    n     = (len < room) ? len : room;
    first = DEBUG_UART_RING_SIZE - s_head;
    if (first > n) { first = n; }
    memcpy(&s_ring[s_head], p, first);
    memcpy(&s_ring[0], p + first, n - first);
    s_head = (s_head + n) % DEBUG_UART_RING_SIZE;
    kick();
    __set_PRIMASK(primask);

    p   += n;
    len -= n;
  }
}

void debug_uart_panic(const char *s)
{
  __disable_irq();
  TX_STREAM->CR &= ~DMA_SxCR_EN;
  USART1->CR3   &= ~USART_CR3_DMAT;
  s_ready = 0U;
  while ((USART1->SR & USART_SR_TC) == 0U) { }  /* let the byte in flight finish */
  (void)HAL_UART_Transmit(&huart1, (uint8_t *)(uintptr_t)s, (uint16_t)strlen(s), 1000U);
}

uint32_t debug_uart_dropped(void)
{
  return s_dropped;
}
