/**
  ******************************************************************************
  * @file    panel.c
  * @brief   Widget drawing engine - see panel.h. Page layouts and demo data
  *          live in panel_pages.c.
  *
  *          Each widget type has a static part (frame, label, scale - drawn
  *          once by Panel_DrawPage()) and a dynamic part (needle, number,
  *          knob, bar, lamp colour - redrawn when the channel changes). The
  *          dynamic part only ever repaints its own fixed area, erasing with
  *          the widget background, so nothing is cleared that isn't redrawn
  *          right after (no flicker, no full-screen refresh).
  ******************************************************************************
  */
#include "panel.h"
#include "panel_internal.h"
#include "gfx.h"
#include "st7796s.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#define PANEL_MAX_WIDGETS   24U
#define DEMO_PERIOD_MS      100U   /* demo values change 10x per second */

/* Gauge scale: 240 degrees, from lower-left (210) clockwise to lower-right (-30). */
#define GAUGE_DEG_MIN       210
#define GAUGE_SWEEP         240

static int32_t  s_channels[PANEL_CHANNELS];
static uint32_t s_external;                    /* bit n: channel n set by Panel_SetChannel() */

/* Per widget: what is on screen now (to skip redraws and to erase). */
static int32_t  s_shown[PANEL_PAGE_COUNT][PANEL_MAX_WIDGETS];
static int16_t  s_shown_pos[PANEL_PAGE_COUNT][PANEL_MAX_WIDGETS]; /* needle angle / knob x */
static uint8_t  s_valid[PANEL_PAGE_COUNT][PANEL_MAX_WIDGETS];
static uint8_t  s_next[PANEL_PAGE_COUNT];                         /* round-robin cursor */

/* ---------- helpers ------------------------------------------------------- */

static void fmt_value(int32_t v, uint8_t decimals, char *out, size_t size)
{
  if (decimals == 0U)
  {
    snprintf(out, size, "%ld", (long)((v >= 0) ? ((v + 5) / 10) : ((v - 5) / 10)));
  }
  else
  {
    const int32_t a = (v < 0) ? -v : v;
    snprintf(out, size, "%s%ld.%ld", (v < 0) ? "-" : "", (long)(a / 10), (long)(a % 10));
  }
}

/* Text right-padded/centred to exactly `chars` cells, so a shorter new value
   fully overwrites a longer old one. */
static void text_field(int16_t cx, int16_t y, uint16_t chars, const char *s,
                       uint16_t color, uint16_t bg, uint8_t scale)
{
  char buf[24];
  const size_t len = strlen(s);
  size_t pad_l;

  if (chars >= sizeof(buf)) { chars = (uint16_t)(sizeof(buf) - 1U); }
  if (len >= chars) { snprintf(buf, sizeof(buf), "%.*s", (int)chars, s); }
  else
  {
    pad_l = (chars - len) / 2U;
    memset(buf, ' ', chars);
    memcpy(buf + pad_l, s, len);
    buf[chars] = '\0';
  }
  gfx_text_centered(cx, y, buf, color, bg, scale);
}

static int32_t clamp_frac_q10(const Widget *w, int32_t v)   /* 0..1024 */
{
  /* int64: thresholds may be INT32_MAX ("disabled") */
  const int64_t span = (int64_t)w->max - w->min;
  int64_t f;

  if (span <= 0) { return 0; }
  f = (((int64_t)v - w->min) * 1024) / span;
  return (f < 0) ? 0 : ((f > 1024) ? 1024 : (int32_t)f);
}

static uint16_t level_color(const Widget *w, int32_t v, uint16_t normal)
{
  if (v >= w->alarm) { return ST7796S_RED; }
  if (v >= w->warn)  { return ST7796S_YELLOW; }
  return normal;
}

static void widget_frame(const Widget *w)
{
  gfx_round_rect(w->x, w->y, w->w, w->h, 8, GFX_PANEL, GFX_DARKGREY);
}

/* ---------- analog gauge --------------------------------------------------- */

typedef struct { int16_t cx, cy, r, needle; } GaugeGeo;

static GaugeGeo gauge_geo(const Widget *w)
{
  GaugeGeo g;
  int16_t r1 = (int16_t)(w->w / 2 - 12);
  int16_t r2 = (int16_t)(((w->h - 50) * 2) / 3);

  g.r      = (r1 < r2) ? r1 : r2;
  g.cx     = (int16_t)(w->x + w->w / 2);
  g.cy     = (int16_t)(w->y + 20 + g.r + 2);
  g.needle = (int16_t)(g.r - 18);
  return g;
}

static int32_t value_to_deg(const Widget *w, int32_t v)
{
  return GAUGE_DEG_MIN - (GAUGE_SWEEP * clamp_frac_q10(w, v)) / 1024;
}

static void gauge_static(const Widget *w)
{
  const GaugeGeo g = gauge_geo(w);
  const int32_t d_warn  = value_to_deg(w, w->warn);
  const int32_t d_alarm = value_to_deg(w, w->alarm);
  const int32_t d_end   = GAUGE_DEG_MIN - GAUGE_SWEEP;
  char txt[16];
  int16_t tx, ty;

  widget_frame(w);
  gfx_text_centered(g.cx, (int16_t)(w->y + 6), w->label, ST7796S_WHITE, GFX_PANEL, 2);

  /* colour zones: normal / warn / alarm */
  gfx_arc_band(g.cx, g.cy, (int16_t)(g.r - 6), g.r, GAUGE_DEG_MIN, (w->warn <= w->max) ? d_warn : d_end, GFX_DARKGREEN);
  if (w->warn <= w->max)
  {
    gfx_arc_band(g.cx, g.cy, (int16_t)(g.r - 6), g.r, d_warn, (w->alarm <= w->max) ? d_alarm : d_end, GFX_ORANGE);
  }
  if (w->alarm <= w->max)
  {
    gfx_arc_band(g.cx, g.cy, (int16_t)(g.r - 6), g.r, d_alarm, d_end, ST7796S_RED);
  }

  /* 10 divisions, long tick every second one */
  for (int32_t i = 0; i <= 10; i++)
  {
    const int32_t deg = GAUGE_DEG_MIN - (GAUGE_SWEEP * i) / 10;
    int16_t x0, y0, x1, y1;
    gfx_polar(g.cx, g.cy, (int16_t)(g.r - (((i % 2) == 0) ? 13 : 9)), deg, &x0, &y0);
    gfx_polar(g.cx, g.cy, g.r, deg, &x1, &y1);
    gfx_line(x0, y0, x1, y1, 2U, ST7796S_WHITE);
  }

  /* min / max at the ends of the scale, unit at the bottom */
  fmt_value(w->min, 0U, txt, sizeof(txt));
  gfx_polar(g.cx, g.cy, g.r, GAUGE_DEG_MIN, &tx, &ty);
  gfx_text_centered(tx, (int16_t)(ty + 6), txt, GFX_GREY, GFX_PANEL, 1);
  fmt_value(w->max, 0U, txt, sizeof(txt));
  gfx_polar(g.cx, g.cy, g.r, d_end, &tx, &ty);
  gfx_text_centered(tx, (int16_t)(ty + 6), txt, GFX_GREY, GFX_PANEL, 1);
  gfx_text_centered(g.cx, (int16_t)(w->y + w->h - 12), w->unit, GFX_GREY, GFX_PANEL, 1);
}

static void gauge_value(const Widget *w, int32_t v, uint8_t valid, int16_t *pos)
{
  const GaugeGeo g = gauge_geo(w);
  const int16_t deg = (int16_t)value_to_deg(w, v);
  int16_t x, y;
  char txt[16];

  if (!valid || (deg != *pos))
  {
    if (valid)
    {
      gfx_polar(g.cx, g.cy, g.needle, *pos, &x, &y);
      gfx_line(g.cx, g.cy, x, y, 3U, GFX_PANEL);               /* erase old needle */
    }
    gfx_polar(g.cx, g.cy, g.needle, deg, &x, &y);
    gfx_line(g.cx, g.cy, x, y, 3U, level_color(w, v, ST7796S_WHITE));
    gfx_fill_circle(g.cx, g.cy, 6, GFX_GREY);                   /* hub */
    *pos = deg;
  }

  fmt_value(v, w->decimals, txt, sizeof(txt));
  text_field(g.cx, (int16_t)(g.cy + (g.r / 2) - 2), 6U, txt, level_color(w, v, ST7796S_CYAN), GFX_PANEL, 2);
}

/* ---------- digital readout ------------------------------------------------ */

static uint8_t digital_scale(const Widget *w)
{
  char a[16], b[16];
  size_t n;

  fmt_value(w->max, w->decimals, a, sizeof(a));
  fmt_value(w->min, w->decimals, b, sizeof(b));
  n = (strlen(a) > strlen(b)) ? strlen(a) : strlen(b);
  if ((n * ST7796S_CharPitch(3)) <= (size_t)(w->w - 8)) { return 3U; }
  return 2U;
}

static void digital_static(const Widget *w)
{
  widget_frame(w);
  gfx_text((int16_t)(w->x + 7), (int16_t)(w->y + 6), w->label, ST7796S_WHITE, GFX_PANEL, 1);
  gfx_text((int16_t)(w->x + w->w - 7 - (int16_t)gfx_text_width(w->unit, 1)), (int16_t)(w->y + 6),
           w->unit, GFX_GREY, GFX_PANEL, 1);
}

static void digital_value(const Widget *w, int32_t v)
{
  const uint8_t  s = digital_scale(w);
  const uint16_t chars = (uint16_t)((w->w - 8) / ST7796S_CharPitch(s));
  char txt[16];

  fmt_value(v, w->decimals, txt, sizeof(txt));
  text_field((int16_t)(w->x + w->w / 2), (int16_t)(w->y + w->h - 7 * s - 7), chars, txt,
             level_color(w, v, ST7796S_CYAN), GFX_PANEL, s);
}

/* ---------- horizontal slider ---------------------------------------------- */

#define SLIDER_KNOB_R  9

static void slider_track(const Widget *w, int16_t knob_x)
{
  const int16_t x0 = (int16_t)(w->x + 14);
  const int16_t x1 = (int16_t)(w->x + w->w - 14);
  const int16_t ty = (int16_t)(w->y + w->h - 16);

  ST7796S_FillRect((uint16_t)x0, (uint16_t)ty, (uint16_t)(knob_x - x0 + 1), 8U, w->color);
  ST7796S_FillRect((uint16_t)knob_x, (uint16_t)ty, (uint16_t)(x1 - knob_x + 1), 8U, GFX_DARKGREY);
}

static void slider_static(const Widget *w)
{
  widget_frame(w);
  gfx_text((int16_t)(w->x + 8), (int16_t)(w->y + 7), w->label, ST7796S_WHITE, GFX_PANEL, 2);
}

static void slider_value(const Widget *w, int32_t v, uint8_t valid, int16_t *pos)
{
  const int16_t x0 = (int16_t)(w->x + 14);
  const int16_t x1 = (int16_t)(w->x + w->w - 14);
  const int16_t ky = (int16_t)(w->y + w->h - 12);
  const int16_t kx = (int16_t)(x0 + ((x1 - x0) * clamp_frac_q10(w, v)) / 1024);
  char txt[24], num[16];

  if (!valid || (kx != *pos))
  {
    if (valid)   /* erase the old knob (track is redrawn right after) */
    {
      ST7796S_FillRect((uint16_t)(*pos - SLIDER_KNOB_R - 1), (uint16_t)(ky - SLIDER_KNOB_R - 1),
                       (uint16_t)(2 * SLIDER_KNOB_R + 3), (uint16_t)(2 * SLIDER_KNOB_R + 3), GFX_PANEL);
    }
    slider_track(w, kx);
    gfx_fill_circle(kx, ky, SLIDER_KNOB_R, ST7796S_WHITE);
    gfx_fill_circle(kx, ky, SLIDER_KNOB_R - 4, w->color);
    *pos = kx;
  }

  fmt_value(v, w->decimals, num, sizeof(num));
  snprintf(txt, sizeof(txt), "%7s %s", num, w->unit);
  gfx_text((int16_t)(w->x + w->w - 8 - (int16_t)gfx_text_width(txt, 2)), (int16_t)(w->y + 7),
           txt, level_color(w, v, ST7796S_CYAN), GFX_PANEL, 2);
}

/* ---------- vertical bar ---------------------------------------------------- */

static void vbar_static(const Widget *w)
{
  widget_frame(w);
  gfx_text_centered((int16_t)(w->x + w->w / 2), (int16_t)(w->y + 6), w->label, ST7796S_WHITE, GFX_PANEL, 1);
  gfx_rect((int16_t)(w->x + 9), (int16_t)(w->y + 18), (int16_t)(w->w - 18), (int16_t)(w->h - 44), GFX_GREY);
  gfx_text_centered((int16_t)(w->x + w->w / 2), (int16_t)(w->y + w->h - 12), w->unit, GFX_GREY, GFX_PANEL, 1);
}

static void vbar_value(const Widget *w, int32_t v)
{
  const int16_t bx = (int16_t)(w->x + 10);
  const int16_t by = (int16_t)(w->y + 19);
  const int16_t bw = (int16_t)(w->w - 20);
  const int16_t bh = (int16_t)(w->h - 46);
  const int16_t fh = (int16_t)((bh * clamp_frac_q10(w, v)) / 1024);
  char txt[16];

  ST7796S_FillRect((uint16_t)bx, (uint16_t)by, (uint16_t)bw, (uint16_t)(bh - fh), ST7796S_BLACK);
  ST7796S_FillRect((uint16_t)bx, (uint16_t)(by + bh - fh), (uint16_t)bw, (uint16_t)fh, level_color(w, v, w->color));

  fmt_value(v, w->decimals, txt, sizeof(txt));
  text_field((int16_t)(w->x + w->w / 2), (int16_t)(w->y + w->h - 23), (uint16_t)((w->w - 4) / 6), txt,
             ST7796S_CYAN, GFX_PANEL, 1);
}

/* ---------- lamp / button --------------------------------------------------- */

static void lamp_static(const Widget *w)
{
  widget_frame(w);
  gfx_text_centered((int16_t)(w->x + w->w / 2), (int16_t)(w->y + w->h - 13), w->label, ST7796S_WHITE, GFX_PANEL, 1);
}

static void lamp_value(const Widget *w, int32_t v)
{
  const int16_t cx = (int16_t)(w->x + w->w / 2);
  const int16_t cy = (int16_t)(w->y + 20);

  gfx_fill_circle(cx, cy, 12, GFX_GREY);
  gfx_fill_circle(cx, cy, 10, (v != 0) ? w->color : GFX_DARKGREY);
  if (v != 0) { gfx_fill_circle((int16_t)(cx - 3), (int16_t)(cy - 3), 3, ST7796S_WHITE); }  /* glint */
}

static void button_value(const Widget *w, int32_t v)
{
  const uint8_t on = (v != 0) ? 1U : 0U;

  gfx_round_rect(w->x, w->y, w->w, w->h, 10, on ? w->color : GFX_DARKGREY, on ? ST7796S_WHITE : GFX_GREY);
  gfx_text_centered((int16_t)(w->x + w->w / 2), (int16_t)(w->y + w->h / 2 - 7), w->label,
                    on ? ST7796S_BLACK : ST7796S_WHITE, on ? w->color : GFX_DARKGREY, 2);
}

/* ---------- dispatch -------------------------------------------------------- */

static void draw_static(const Widget *w)
{
  switch (w->type)
  {
    case W_GAUGE:   gauge_static(w);   break;
    case W_DIGITAL: digital_static(w); break;
    case W_SLIDER:  slider_static(w);  break;
    case W_VBAR:    vbar_static(w);    break;
    case W_LAMP:    lamp_static(w);    break;
    case W_BUTTON:  /* fully drawn by button_value() */ break;
    default:        break;
  }
}

static void draw_value(PanelPage page, uint8_t i)
{
  const Widget *w = &k_panel_pages[page].widgets[i];
  const int32_t v = s_channels[w->channel];

  switch (w->type)
  {
    case W_GAUGE:   gauge_value(w, v, s_valid[page][i], &s_shown_pos[page][i]);  break;
    case W_DIGITAL: digital_value(w, v); break;
    case W_SLIDER:  slider_value(w, v, s_valid[page][i], &s_shown_pos[page][i]); break;
    case W_VBAR:    vbar_value(w, v);    break;
    case W_LAMP:    lamp_value(w, v);    break;
    case W_BUTTON:  button_value(w, v);  break;
    default:        break;
  }
  s_shown[page][i] = v;
  s_valid[page][i] = 1U;
}

/* ---------- public API ------------------------------------------------------ */

void Panel_DrawPage(PanelPage page)
{
  const PageDef *pd;

  if (page >= PANEL_PAGE_COUNT) { return; }
  pd = &k_panel_pages[page];
  panel_sources_update(s_channels, s_external);
#if PANEL_DEMO
  panel_demo_update(s_channels, s_external);
#endif
  for (uint8_t i = 0U; (i < pd->count) && (i < PANEL_MAX_WIDGETS); i++)
  {
    s_valid[page][i] = 0U;
    draw_static(&pd->widgets[i]);
    draw_value(page, i);
  }
  if (pd->footer != NULL)
  {
    gfx_text(80, 448, pd->footer, GFX_GREY, ST7796S_BLACK, 2);
  }
}

void Panel_Poll(PanelPage page)
{
  static uint32_t next_demo;
  const PageDef *pd;

  if (page >= PANEL_PAGE_COUNT) { return; }
  pd = &k_panel_pages[page];

#if PANEL_DEMO
  if ((int32_t)(HAL_GetTick() - next_demo) >= 0)
  {
    next_demo = HAL_GetTick() + DEMO_PERIOD_MS;
    panel_demo_update(s_channels, s_external);
  }
#endif
  panel_sources_update(s_channels, s_external);

  /* Previous widget's pixels still going out by DMA: come back next pass
     rather than wait for them inside the next draw call. */
  if (ST7796S_Busy()) { return; }

  /* Redraw at most one changed widget per call (round robin). */
  for (uint8_t n = 0U; n < pd->count; n++)
  {
    uint8_t i = s_next[page];
    s_next[page] = (uint8_t)((i + 1U) % pd->count);
    if (!s_valid[page][i] || (s_shown[page][i] != s_channels[pd->widgets[i].channel]))
    {
      draw_value(page, i);
      break;
    }
  }
}

void Panel_SetChannel(uint8_t channel, int32_t value)
{
  if (channel >= PANEL_CHANNELS) { return; }
  s_channels[channel] = value;
  s_external |= (1UL << channel);
}

int32_t Panel_GetChannel(uint8_t channel)
{
  return (channel < PANEL_CHANNELS) ? s_channels[channel] : 0;
}
