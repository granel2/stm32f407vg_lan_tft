/**
  ******************************************************************************
  * @file    st7796s.h
  * @brief   Minimal ST7796S driver (3.5" 320x480 TFT) over 4-wire SPI.
  *
  * Wiring (module "SPI" header <-> board headers, see hardware/f407.sch):
  *   SCL   -> PC10 SPI3_SCK   (SV4.3)
  *   SDA   -> PC12 SPI3_MOSI  (SV4.1)
  *   SDA-O -> PC11 SPI3_MISO  (SV4.2)   optional, only needed for ReadID
  *   CS    -> PA15            (SV4.4)   software-driven GPIO
  *   GND   -> GND             (SV4.6)
  *   VCC   -> +5V             (SV4.5)   module must have its own 3.3 V LDO;
  *                                        otherwise +3V3 from SV2.1
  *   BL    -> PC7             (SV1.8)   high = backlight on
  *   RST   -> PD5             (SV1.9)
  *   DC    -> PD6             (SV1.10)
  * All of CS/DC/RST/BL are plain push-pull GPIOs, chosen so the whole panel
  * fits on just two headers (SV4 + SV1) instead of three - see
  * Display/README.md and Display/docs/TFT_WIRING.md.
  * Module IM0..IM2 solder jumpers must be set to the "SPI" (4-wire) column.
  ******************************************************************************
  */

#ifndef ST7796S_H
#define ST7796S_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* Panel geometry in the default (portrait, MADCTL rotation 0) orientation */
#define ST7796S_WIDTH   320U
#define ST7796S_HEIGHT  480U

/* RGB565 helpers */
#define ST7796S_RGB(r, g, b) \
  ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | ((uint16_t)(b) >> 3)))
#define ST7796S_BLACK   0x0000U
#define ST7796S_WHITE   0xFFFFU
#define ST7796S_RED     0xF800U
#define ST7796S_GREEN   0x07E0U
#define ST7796S_BLUE    0x001FU
#define ST7796S_YELLOW  0xFFE0U
#define ST7796S_CYAN    0x07FFU
#define ST7796S_MAGENTA 0xF81FU

void     ST7796S_Init(SPI_HandleTypeDef *hspi);
void     ST7796S_Backlight(uint8_t on);
/* Rotation 0..3 = portrait, landscape, portrait flipped, landscape flipped */
void     ST7796S_SetRotation(uint8_t rotation);
uint16_t ST7796S_Width(void);
uint16_t ST7796S_Height(void);

/* Reads RDID4 (0xD3): 4 raw bytes, ST7796S answers xx 00 77 96. Needs SDA-O
   wired to PC11. Returns the raw bytes so a log can show exactly what came
   back (a 1-bit shift is normal for this controller in 4-wire SPI). */
void     ST7796S_ReadID(uint8_t out[4]);

/* Fills of 256+ px run by DMA and return before the pixels are out; any
   later driver call waits for them automatically. Call ST7796S_WaitIdle()
   only when the frame must be complete before going on (timing, reset). */
void     ST7796S_FillScreen(uint16_t color);
void     ST7796S_WaitIdle(void);
void     ST7796S_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
/* Hardware smoke test: colour bars, white border and a checkerboard corner.
   Wrong colours / mirrored layout tell which MADCTL bits the panel needs. */
void     ST7796S_DrawTestPattern(void);

/* 7-segment style digits drawn from rectangles - lets the bring-up show
   numbers (uptime, timings) before any real font exists. Cell is w x h px. */
void     ST7796S_DrawDigit7(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint8_t digit, uint16_t color, uint16_t bg);
/* Right-aligned unsigned number, `digits` cells wide, leading zeros blanked */
void     ST7796S_DrawNumber7(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint32_t value, uint8_t digits, uint16_t color, uint16_t bg);

/* Small bitmap font: space, '.', ':', '-', '/', digits 0-9, uppercase A-Z.
   Any other character is drawn as a blank (background-filled) cell rather
   than garbage, so an unsupported byte in a string just leaves a gap.
   `scale` is an integer pixel multiplier (1 = native 5x7 px). */
void     ST7796S_DrawChar(uint16_t x, uint16_t y, char c,
                          uint16_t color, uint16_t bg, uint8_t scale);
/* One line of text, left to right, 1 px (x scale) gap between characters.
   Returns the x position right after the last character (useful for
   appending more text on the same line). */
uint16_t ST7796S_DrawString(uint16_t x, uint16_t y, const char *s,
                            uint16_t color, uint16_t bg, uint8_t scale);
/* Advance in px for one character cell at the given scale (glyph + gap) -
   for laying out fixed-width fields without hardcoding the "6" everywhere. */
uint16_t ST7796S_CharPitch(uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif /* ST7796S_H */
