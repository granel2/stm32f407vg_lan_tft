/**
  ******************************************************************************
  * @file    gfx.c
  * @brief   See gfx.h.
  ******************************************************************************
  */
#include "gfx.h"
#include "st7796s.h"

#include <string.h>

/* round(16384 * sin(d)), d = 0..90 degrees */
static const int16_t k_sin_q14[91] =
{
  0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406, 3686, 3964,
  4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402, 6664, 6924, 7182, 7438, 7692,
  7943, 8192, 8438, 8682, 8923, 9162, 9397, 9630, 9860, 10087, 10311, 10531, 10749,
  10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733, 12911, 13085,
  13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726, 14849,
  14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964,
  16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382, 16384
};

int32_t gfx_sin(int32_t deg)
{
  deg %= 360;
  if (deg < 0) { deg += 360; }
  if (deg <= 90)  { return  k_sin_q14[deg]; }
  if (deg <= 180) { return  k_sin_q14[180 - deg]; }
  if (deg <= 270) { return -k_sin_q14[deg - 180]; }
  return -k_sin_q14[360 - deg];
}

int32_t gfx_cos(int32_t deg)
{
  return gfx_sin(deg + 90);
}

void gfx_polar(int16_t cx, int16_t cy, int16_t r, int32_t deg, int16_t *x, int16_t *y)
{
  /* +8192 = round to nearest when dropping the Q14 scale */
  *x = (int16_t)(cx + (((int32_t)r * gfx_cos(deg) + 8192) >> 14));
  *y = (int16_t)(cy - (((int32_t)r * gfx_sin(deg) + 8192) >> 14));
}

/* Clipped FillRect with signed coordinates. */
static void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  if (x < 0) { w = (int16_t)(w + x); x = 0; }
  if (y < 0) { h = (int16_t)(h + y); y = 0; }
  if ((w <= 0) || (h <= 0)) { return; }
  ST7796S_FillRect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static int32_t isqrt(int32_t v)
{
  int32_t r = 0;
  int32_t bit = 1L << 30;

  if (v <= 0) { return 0; }
  while (bit > v) { bit >>= 2; }
  while (bit != 0)
  {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
    else              { r >>= 1; }
    bit >>= 2;
  }
  return r;
}

void gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t thick, uint16_t color)
{
  const int16_t half = (int16_t)(thick / 2U);
  int16_t dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
  int16_t dy = (int16_t)(-((y1 > y0) ? (y1 - y0) : (y0 - y1)));
  const int16_t sx = (x0 < x1) ? 1 : -1;
  const int16_t sy = (y0 < y1) ? 1 : -1;
  int32_t err = dx + dy;

  if (thick == 0U) { thick = 1U; }

  /* Pure horizontal / vertical: one span instead of one rect per point. */
  if (dy == 0)
  {
    fill((int16_t)(((x0 < x1) ? x0 : x1) - half), (int16_t)(y0 - half), (int16_t)(dx + thick), (int16_t)thick, color);
    return;
  }
  if (dx == 0)
  {
    fill((int16_t)(x0 - half), (int16_t)(((y0 < y1) ? y0 : y1) - half), (int16_t)thick, (int16_t)(-dy + thick), color);
    return;
  }

  for (;;)
  {
    fill((int16_t)(x0 - half), (int16_t)(y0 - half), (int16_t)thick, (int16_t)thick, color);
    if ((x0 == x1) && (y0 == y1)) { break; }
    {
      const int32_t e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 = (int16_t)(x0 + sx); }
      if (e2 <= dx) { err += dx; y0 = (int16_t)(y0 + sy); }
    }
  }
}

void gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  fill(x, y, w, 1, color);
  fill(x, (int16_t)(y + h - 1), w, 1, color);
  fill(x, y, 1, h, color);
  fill((int16_t)(x + w - 1), y, 1, h, color);
}

static void fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color)
{
  if (r * 2 > h) { r = (int16_t)(h / 2); }
  if (r * 2 > w) { r = (int16_t)(w / 2); }

  for (int16_t i = 0; i < r; i++)
  {
    const int32_t dy    = r - i;
    const int16_t inset = (int16_t)(r - isqrt((int32_t)r * r - dy * dy));
    fill((int16_t)(x + inset), (int16_t)(y + i),         (int16_t)(w - 2 * inset), 1, color);
    fill((int16_t)(x + inset), (int16_t)(y + h - 1 - i), (int16_t)(w - 2 * inset), 1, color);
  }
  fill(x, (int16_t)(y + r), w, (int16_t)(h - 2 * r), color);
}

void gfx_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t fill_color, uint16_t border)
{
  if (border != fill_color)
  {
    /* 2 px border without filling the whole area twice (SPI time is the
       cost here): full border-colour rows only where the corners are,
       plus two 2 px side strips, then the inside once. */
    const int16_t rr = (r * 2 > h) ? (int16_t)(h / 2) : r;
    fill_round_rect(x, y, w, (int16_t)(2 * rr), rr, border);                       /* top corners   */
    fill_round_rect(x, (int16_t)(y + h - 2 * rr), w, (int16_t)(2 * rr), rr, border); /* bottom corners */
    fill(x, (int16_t)(y + rr), 2, (int16_t)(h - 2 * rr), border);
    fill((int16_t)(x + w - 2), (int16_t)(y + rr), 2, (int16_t)(h - 2 * rr), border);
    fill_round_rect((int16_t)(x + 2), (int16_t)(y + 2), (int16_t)(w - 4), (int16_t)(h - 4),
                    (int16_t)((rr > 2) ? (rr - 2) : 0), fill_color);
  }
  else
  {
    fill_round_rect(x, y, w, h, r, fill_color);
  }
}

void gfx_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
  int16_t x = r;
  int16_t y = 0;
  int32_t err = 1 - r;

  while (x >= y)
  {
    fill((int16_t)(cx + x), (int16_t)(cy + y), 1, 1, color);
    fill((int16_t)(cx - x), (int16_t)(cy + y), 1, 1, color);
    fill((int16_t)(cx + x), (int16_t)(cy - y), 1, 1, color);
    fill((int16_t)(cx - x), (int16_t)(cy - y), 1, 1, color);
    fill((int16_t)(cx + y), (int16_t)(cy + x), 1, 1, color);
    fill((int16_t)(cx - y), (int16_t)(cy + x), 1, 1, color);
    fill((int16_t)(cx + y), (int16_t)(cy - x), 1, 1, color);
    fill((int16_t)(cx - y), (int16_t)(cy - x), 1, 1, color);
    y++;
    if (err < 0) { err += 2 * y + 1; }
    else         { x--; err += 2 * (y - x) + 1; }
  }
}

void gfx_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
  for (int16_t dy = (int16_t)(-r); dy <= r; dy++)
  {
    const int16_t dx = (int16_t)isqrt((int32_t)r * r - (int32_t)dy * dy);
    fill((int16_t)(cx - dx), (int16_t)(cy + dy), (int16_t)(2 * dx + 1), 1, color);
  }
}

/* Angle of (dx, -dy) in whole degrees 0..359 (maths convention, screen y
   down). Octant reduction + a linear search over the first 45 degrees:
   only used while drawing static scale bands, so speed doesn't matter. */
static int32_t angle_deg(int32_t dx, int32_t dy)
{
  const int32_t ax = (dx < 0) ? -dx : dx;
  const int32_t ay = (dy < 0) ? -dy : dy;
  int32_t a = 0;

  if ((ax == 0) && (ay == 0)) { return 0; }
  if (ax >= ay)
  {
    /* smallest a in 0..45 with tan(a) >= ay/ax */
    while ((a < 45) && ((ay * gfx_cos(a)) > (ax * gfx_sin(a)))) { a++; }
  }
  else
  {
    while ((a < 45) && ((ax * gfx_cos(a)) > (ay * gfx_sin(a)))) { a++; }
    a = 90 - a;
  }
  /* screen y grows down: "up" means dy < 0 */
  if (dx >= 0) { return (dy <= 0) ? a : (360 - a) % 360; }
  return (dy <= 0) ? (180 - a) : (180 + a);
}

static uint8_t angle_in_range(int32_t a, int32_t from, int32_t to)
{
  /* from > to, the arc runs clockwise from `from` down to `to` */
  int32_t span = from - to;
  int32_t d = from - a;

  while (d < 0)    { d += 360; }
  while (d >= 360) { d -= 360; }
  return (d <= span) ? 1U : 0U;
}

void gfx_arc_band(int16_t cx, int16_t cy, int16_t r_in, int16_t r_out,
                  int32_t deg_from, int32_t deg_to, uint16_t color)
{
  const int32_t ro2 = (int32_t)r_out * r_out;
  const int32_t ri2 = (int32_t)r_in * r_in;

  /* Row scan, pixels grouped into horizontal runs -> one FillRect per run. */
  for (int16_t dy = (int16_t)(-r_out); dy <= r_out; dy++)
  {
    int16_t run_start = 0;
    uint8_t in_run = 0U;

    for (int16_t dx = (int16_t)(-r_out); dx <= (int16_t)(r_out + 1); dx++)
    {
      const int32_t d2 = (int32_t)dx * dx + (int32_t)dy * dy;
      const uint8_t inside = (uint8_t)((dx <= r_out) && (d2 <= ro2) && (d2 >= ri2) &&
                                       angle_in_range(angle_deg(dx, dy), deg_from, deg_to));
      if (inside && !in_run)      { run_start = dx; in_run = 1U; }
      else if (!inside && in_run)
      {
        fill((int16_t)(cx + run_start), (int16_t)(cy + dy), (int16_t)(dx - run_start), 1, color);
        in_run = 0U;
      }
    }
  }
}

uint16_t gfx_text_width(const char *s, uint8_t scale)
{
  const size_t n = strlen(s);
  if (n == 0U) { return 0U; }
  /* last character has no trailing 1-px gap */
  return (uint16_t)(n * ST7796S_CharPitch(scale) - ((scale == 0U) ? 1U : scale));
}

void gfx_text(int16_t x, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t scale)
{
  if ((x < 0) || (y < 0)) { return; }
  (void)ST7796S_DrawString((uint16_t)x, (uint16_t)y, s, color, bg, scale);
}

void gfx_text_centered(int16_t cx, int16_t y, const char *s, uint16_t color, uint16_t bg, uint8_t scale)
{
  gfx_text((int16_t)(cx - (int16_t)(gfx_text_width(s, scale) / 2U)), y, s, color, bg, scale);
}
