/**
  ******************************************************************************
  * @file    debug_uart.h
  * @brief   Non-blocking debug output on USART1: a ring buffer drained by DMA
  *          (DMA2 Stream7 / Channel 4 = USART1_TX). Debug_Print() (main.c)
  *          and every other log writer go through debug_uart_write(), which
  *          copies into the ring and returns in microseconds - the main loop
  *          no longer stalls ~1.1 ms per 100 characters at 921600 baud.
  *
  *          Before debug_uart_init() (very early boot) writes fall back to
  *          the old blocking HAL_UART_Transmit(). If the ring is full, a
  *          caller in thread mode waits for the DMA to make room; one in an
  *          interrupt (or with interrupts masked) drops the text instead and
  *          it is counted in debug_uart_dropped().
  ******************************************************************************
  */
#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEBUG_UART_RING_SIZE  4096U

/* Call right after MX_USART1_UART_Init(). */
void     debug_uart_init(void);
void     debug_uart_write(const void *data, uint32_t len);
/* Fault handlers: stop the DMA and print synchronously (the ring may be
   half sent; that part is lost, the panic text is not). */
void     debug_uart_panic(const char *s);
uint32_t debug_uart_dropped(void);

/* Called from DMA2_Stream7_IRQHandler (stm32f4xx_it.c) */
void     debug_uart_dma_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UART_H */
