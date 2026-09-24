/**
  ******************************************************************************
  * @file    can_bus.h
  * @brief   CAN1 driver (bxCAN, register level - the project has no HAL CAN
  *          module): init, receive through an interrupt-fed queue, transmit,
  *          counters and bus state. Settings in can_config.h.
  *
  *          Receive: the CAN1_RX0 interrupt copies frames out of the hardware
  *          FIFO into a queue; can_bus_poll() (main loop) takes them from
  *          there, logs them to USART1 (rate-limited) and hands each one to
  *          the handler set with can_bus_set_rx_handler(), in main-loop
  *          context. Transmit: can_bus_send() puts a frame in a free
  *          hardware mailbox and returns at once.
  ******************************************************************************
  */
#ifndef CAN_BUS_H
#define CAN_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t id;       /* 11-bit (std) or 29-bit (ext) identifier */
  uint8_t  ext;      /* 1 = extended (29-bit) id */
  uint8_t  rtr;      /* 1 = remote frame (no data) */
  uint8_t  len;      /* 0..8 */
  uint8_t  data[8];
} CanFrame;

typedef enum
{
  CAN_STATE_OFF = 0,   /* init failed / not started */
  CAN_STATE_ACTIVE,    /* error active - normal */
  CAN_STATE_WARNING,   /* TEC or REC >= 96 */
  CAN_STATE_PASSIVE,   /* TEC or REC >= 128 */
  CAN_STATE_BUS_OFF    /* TEC > 255; hardware recovers by itself (ABOM) */
} CanState;

typedef struct
{
  uint32_t rx;          /* frames received */
  uint32_t tx;          /* frames handed to a mailbox */
  uint32_t tx_busy;     /* can_bus_send() refused: all 3 mailboxes full */
  uint32_t rx_overrun;  /* frames lost: hardware FIFO or queue full */
  uint8_t  tec;         /* transmit error counter */
  uint8_t  rec;         /* receive error counter */
  CanState state;
} CanStats;

/* Clock, pins PA11/PA12, bit timing, accept-all filter, RX interrupt.
   Call once at boot. Returns 1 on success. */
uint8_t  can_bus_init(void);

/* Main loop, every pass: drains the RX queue (log + handler) and prints
   the periodic summary. */
void     can_bus_poll(void);

/* Application hook for received frames, called from can_bus_poll().
   NULL = none (frames are only counted and logged). */
typedef void (*CanRxHandler)(const CanFrame *f);
void     can_bus_set_rx_handler(CanRxHandler h);

/* Queue one frame for transmission. Returns 1 if a mailbox took it, 0 if
   all three are busy (or in listen-only mode). Non-blocking. */
uint8_t  can_bus_send(const CanFrame *f);

void     can_bus_get_stats(CanStats *s);
const char *can_bus_state_name(CanState st);

/* Called from CAN1_RX0_IRQHandler (stm32f4xx_it.c) */
void     can_bus_rx0_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BUS_H */
