/**
  ******************************************************************************
  * @file    keypad_page.h
  * @brief   TFT "I2C KBD" page: what arrives from the keypad module on I2C1,
  *          one line per packet (newest at the bottom, scrolls when full),
  *          plus a counter row - transfers to our address (RX), accepted
  *          packets (OK), errors (ERR); the address is in the title -
  *          so a silent bus can be told apart from a bus with bad frames.
  *
  *          Same contract as Clock/ and Panel/: Keypad_PageDraw() paints the
  *          page after tft_app.c has drawn the title bar; Keypad_PagePoll()
  *          (every main-loop pass while the page is showing) redraws one row
  *          per call. The Keypad_Page*() feeders are called by keypad_poll()
  *          whatever page is showing - lines are kept and appear on the next
  *          visit.
  *
  *          Line format (23 characters, scale 2):
  *            #12 E 1234567890    GROUP, ended by: E '#', M max length,
  *                                T pause (only with TIMEOUT_SEND), C '*'
  *            #13 CANCEL TIMEOUT  CANCEL with its reason
  *            #1 START FW 2 RST 01  module reset, MCUSR (01 power, 02 RESET pin,
  *                                04 brown-out, 08 watchdog)
  *            #7 KEY A DOWN       single keys (module built with LETTERS_KEY)
  *            LOST 2              seq gap (before the next line's packet)
  *            CRC 3 LATE 0        error counters changed
  *          With KEYPAD_LOG_TEXT 0 a GROUP shows only its length.
  ******************************************************************************
  */
#ifndef KEYPAD_PAGE_H
#define KEYPAD_PAGE_H

#include <stdint.h>
#include "keypad.h"

#ifdef __cplusplus
extern "C" {
#endif

void Keypad_PageDraw(void);
void Keypad_PagePoll(void);

/* "KBD HELP" page: legend for the lines above, static, one row per poll */
void Keypad_HelpDraw(void);
void Keypad_HelpPoll(void);

/* Feeders, from keypad_poll() (main loop) */
void Keypad_PagePacket(const KeypadPacket *p);
void Keypad_PageLost(uint32_t count);
void Keypad_PageErrors(uint32_t crc_err, uint32_t late);

#ifdef __cplusplus
}
#endif

#endif /* KEYPAD_PAGE_H */
