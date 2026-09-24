/**
  ******************************************************************************
  * @file    diag_can.h
  * @brief   CAN1 self-test in silent loopback (DIAG_CAN_LOOPBACK in
  *          diag_config.h) - checks the CAN/ driver without a second node.
  *
  *          Every second one numbered frame is sent, alternately with an
  *          11-bit (0x7E1) and a 29-bit (0x18DAF1E1) id; it must come back
  *          through the receive path (filter, FIFO, interrupt, queue,
  *          can_bus_poll()) with the same id, length and data within
  *          100 ms. Every DIAG_CAN_REPORT_S seconds a result line goes to
  *          USART1, e.g.
  *            [diag-can] loopback PASS: sent 5 ok 5 lost 0 bad 0 (total ok 25)
  *
  *          Not tested (can't be, inside the chip): TCAN1044V, J3, wiring.
  *          When off, both calls are empty inline stubs.
  ******************************************************************************
  */
#ifndef DIAG_CAN_H
#define DIAG_CAN_H

#include "diag_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if DIAG_CAN_LOOPBACK

/* Before can_bus_init(): switches the driver to loopback, takes the RX hook */
void diag_can_init(void);
/* Main loop, after can_bus_poll() */
void diag_can_poll(void);

#else

static inline void diag_can_init(void) {}
static inline void diag_can_poll(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif /* DIAG_CAN_H */
