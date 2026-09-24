/**
  ******************************************************************************
  * @file    gfx.h
  * @brief   Small 2D drawing layer on top of the ST7796S driver's only real
  *          primitive, ST7796S_FillRect(): lines (with thickness), circles,
  *          arcs, rounded rectangles, centred text. Integer maths only - a
  *          quarter-wave sine table instead of sinf()/cosf(), so no libm and
  *          no float formatting is needed.
  *
  *          Cost model: every pixel/segment is one FillRect = one SPI
  *          address-window + RAMWR sequence (~11 bytes of overhead), so
  *          long horizontal spans are cheap and scattered single pixels are
  *          not. Everything here favours spans (filled circles, rects) and
  *          only uses per-point drawing for lines and arcs.
  ******************************************************************************
  */
#ifndef GFX_H
#define GFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* A few extra RGB565 colours for instrument faces (ST7796S_* has the basics). */
#define GFX_GREY        0x8410U
#define GFX_DARKGREY    0x39E7U
#define GFX_PANEL       0x18E3U   /* very dark grey - widget background */
#define GFX_ORANGE      0xFD20U
#define GFX_DARKGREEN   0x0320U
#define GFX_DARKRED     0x7800U

/* sin/cos of an angle in whole degrees, scaled by 16384 (Q14). */
int32_t  gfx_sin(int32_t deg);
int32_t  gfx_cos(int32_t deg);

void     gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t thick, uint16_t color);
void     gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);           /* 1 px outline */
void     gfx_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                        uint16_t fill, uint16_t border);
void     gfx_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);                  /* 1 px outline */
void     gfx_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);

/* Point on a circle: angle in degrees, maths convention (0 = right,
   90 = up, counter-clockwise), screen y grows downwards. */
void     gfx_polar(int16_t cx, int16_t cy, int16_t r, int32_t deg, int16_t *x, int16_t *y);

/* Ring segment between radii r_in..r_out from deg_from down to deg_to
   (deg_from > deg_to, i.e. drawn clockwise like a gauge scale). */
void     gfx_arc_band(int16_t cx, int16_t cy, int16_t r_in, int16_t r_out,
                      int32_t deg_from, int32_t deg_to, uint16_t color);

/* Text with the driver's 5x7 font. */
uint16_t gfx_text_width(const char *s, uint8_t scale);
void     gfx_text(int16_t x, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t scale);
void     gfx_text_centered(int16_t cx, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
