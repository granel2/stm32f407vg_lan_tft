/**
  ******************************************************************************
  * @file    keypad.h
  * @brief   Receiver for the 4x4 keypad module (ATmega328P) on I2C1 / SV5.
  *          Register level - the project has no HAL I2C module. Protocol:
  *          kbd_protocol.h (a copy of atm328p_keyboard/include/protocol.h -
  *          keep the two identical).
  *
  *          The module is the I2C master and writes one packet per transfer:
  *            [type][seq][N][N data bytes][CRC-8][tail '$']
  *          This board is a slave. The I2C1 event interrupt takes the packet
  *          byte by byte, runs the CRC along, and on the CRC byte decides the
  *          answer to the tail: ACK = accepted, NACK = bad packet (the module
  *          then repeats it). Accepted packets go to a queue; keypad_poll()
  *          (main loop) drops repeats (same seq), notes lost packets (seq gap),
  *          logs to USART1 and hands each one to the handler set with
  *          keypad_set_handler(), in main-loop context.
  *
  *          Idle cost is zero: no polling, no timer - the CPU only runs when
  *          the module sends something.
  ******************************************************************************
  */
#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>
#include "kbd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t type;                    /* KBD_PKT_KEY / GROUP / CANCEL / START */
  uint8_t seq;
  uint8_t len;                     /* bytes in data[] */
  uint8_t data[KBD_MAX_DATA];      /* GROUP: [reason][chars...]; CANCEL: [reason];
                                      KEY: [bit7 = pressed | ASCII];
                                      START: [fw version][MCUSR] */
} KeypadPacket;

typedef struct
{
  uint32_t addr;        /* times our address was matched (any transfer) */
  uint32_t accepted;    /* packets ACKed and queued */
  uint32_t duplicate;   /* repeats of the last seq (module missed our ACK) */
  uint32_t lost;        /* seq gaps: module queue overflow or gave up */
  uint32_t crc_err;     /* CRC wrong -> tail NACKed, module repeats */
  uint32_t bad_frame;   /* N too big, missing tail, bus errors */
  uint32_t late;        /* ISR came after the tail's ACK pulse - the tail was
                           ACKed before the CRC check. Should stay 0; if not,
                           raise KEYPAD_IRQ_PRIORITY. */
  uint32_t overrun;     /* queue full - keypad_poll() too slow */
  uint32_t bus_err;     /* BERR / ARLO / OVR seen by the error interrupt */
} KeypadStats;

/* Pins PB6/PB7, I2C1 as slave at KBD_HOST_ADDR, interrupts. Call once at
   boot (after the clocks are set up). */
void keypad_init(void);

/* Main loop, every pass: takes packets from the queue, filters repeats,
   logs, calls the handler. Cheap when nothing arrived. */
void keypad_poll(void);

/* Application hook, called from keypad_poll() for every new packet
   (repeats filtered out). NULL = none (packets are only logged). */
typedef void (*KeypadHandler)(const KeypadPacket *p);
void keypad_set_handler(KeypadHandler h);

void keypad_get_stats(KeypadStats *s);

/* Called from I2C1_EV_IRQHandler / I2C1_ER_IRQHandler (stm32f4xx_it.c) */
void keypad_ev_irq(void);
void keypad_er_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_H */
