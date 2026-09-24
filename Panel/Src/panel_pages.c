/**
  ******************************************************************************
  * @file    panel_pages.c
  * @brief   The two instrument pages (layout tables) and the demo data that
  *          animates them until real values arrive - see panel.h.
  *
  *          Screen: portrait 320x480. tft_app.c owns y < 50 (title + page
  *          number) and the 40x40 heartbeat square at (20, 434); widgets
  *          stay inside y = 50..428, the footer text sits right of the
  *          heartbeat at y = 448.
  *
  *          Channel map (tenths: 237 = 23.7):
  *            REMOTE  0 PRESSURE bar   1 SPEED rpm   2 TEMP C   3 FLOW l/m
  *                    4 VOLT V         5 VALVE 1 %   6 VALVE 2 %
  *                    7 PUMP  8 FAN  9 HEAT  10 ALARM   (lamps, 0/1)
  *            LOCAL  16..18 AIN1..3 V  19 SUPPLY V   20 UPTIME s (real)
  *                   21 RELAY 1  22 RELAY 2  23 START  24 STOP  (buttons, 0/1)
  *                   25 SETPOINT %
  ******************************************************************************
  */
#include "panel_internal.h"
#include "gfx.h"
#include "st7796s.h"
#include "main.h"

#define OFF        INT32_MAX       /* warn/alarm threshold disabled */
#define BLUE_FILL  ST7796S_RGB(0, 120, 255)

/* ---------- REMOTE: values from the server --------------------------------- */

static const Widget k_remote[] =
{
  /* type       x    y    w    h   label       unit   min   max    warn  alarm dec ch color */
  { W_GAUGE,   10,  50, 145, 140, "PRESSURE", "BAR",    0,   100,   70,   85, 1,  0, 0 },
  { W_GAUGE,  165,  50, 145, 140, "SPEED",    "RPM",    0, 30000, 24000, 27000, 0, 1, 0 },
  { W_DIGITAL, 10, 196,  94,  60, "TEMP",     "C",   -200,  1200,  800,  950, 1,  2, 0 },
  { W_DIGITAL,113, 196,  94,  60, "FLOW",     "L/M",    0,   500,  OFF,  OFF, 1,  3, 0 },
  { W_DIGITAL,216, 196,  94,  60, "VOLT",     "V",      0,  2500, 2400, 2450, 0,  4, 0 },
  { W_SLIDER,  10, 262, 300,  50, "VALVE 1",  "%",      0,  1000,  OFF,  OFF, 0,  5, BLUE_FILL },
  { W_SLIDER,  10, 318, 300,  50, "VALVE 2",  "%",      0,  1000,  OFF,  OFF, 0,  6, BLUE_FILL },
  { W_LAMP,    10, 374,  70,  54, "PUMP",     "",       0,     1,  OFF,  OFF, 0,  7, ST7796S_GREEN },
  { W_LAMP,    87, 374,  70,  54, "FAN",      "",       0,     1,  OFF,  OFF, 0,  8, ST7796S_GREEN },
  { W_LAMP,   164, 374,  70,  54, "HEAT",     "",       0,     1,  OFF,  OFF, 0,  9, GFX_ORANGE },
  { W_LAMP,   241, 374,  70,  54, "ALARM",    "",       0,     1,  OFF,  OFF, 0, 10, ST7796S_RED },
};

/* ---------- LOCAL: the module's own inputs and controls -------------------- */

static const Widget k_local[] =
{
  /* type       x    y    w    h   label       unit   min   max     warn  alarm dec ch color */
  { W_VBAR,    10,  50,  44, 190, "AIN1",     "V",      0,    33,    30,   32, 1, 16, ST7796S_GREEN },
  { W_VBAR,    58,  50,  44, 190, "AIN2",     "V",      0,    33,    30,   32, 1, 17, ST7796S_GREEN },
  { W_VBAR,   106,  50,  44, 190, "AIN3",     "V",      0,    33,    30,   32, 1, 18, ST7796S_GREEN },
  { W_GAUGE,  160,  50, 150, 140, "SUPPLY",   "V",      0,   300,   260,  280, 1, 19, 0 },
  { W_DIGITAL,160, 196, 150,  44, "UPTIME",   "S",      0, 999990,  OFF,  OFF, 0, 20, 0 },
  { W_BUTTON,  10, 248, 145,  50, "RELAY 1",  "",       0,     1,   OFF,  OFF, 0, 21, ST7796S_GREEN },
  { W_BUTTON, 165, 248, 145,  50, "RELAY 2",  "",       0,     1,   OFF,  OFF, 0, 22, ST7796S_GREEN },
  { W_BUTTON,  10, 304, 145,  50, "START",    "",       0,     1,   OFF,  OFF, 0, 23, ST7796S_GREEN },
  { W_BUTTON, 165, 304, 145,  50, "STOP",     "",       0,     1,   OFF,  OFF, 0, 24, ST7796S_RED },
  { W_SLIDER,  10, 362, 300,  56, "SETPOINT", "%",      0,  1000,   OFF,  OFF, 1, 25, GFX_ORANGE },
};

const PageDef k_panel_pages[PANEL_PAGE_COUNT] =
{
  [PANEL_PAGE_REMOTE] = { k_remote, (uint8_t)(sizeof(k_remote) / sizeof(k_remote[0])),
#if PANEL_DEMO
                          "SERVER: DEMO"
#else
                          "SERVER DATA"
#endif
                        },
  [PANEL_PAGE_LOCAL]  = { k_local,  (uint8_t)(sizeof(k_local)  / sizeof(k_local[0])),
#if PANEL_DEMO
                          "LOCAL: DEMO"
#else
                          "LOCAL I/O"
#endif
                        },
};

/* ---------- data -------------------------------------------------------------- */

/* Real local sources - runs with or without the demo. */
void panel_sources_update(int32_t *ch, uint32_t external_mask)
{
  if ((external_mask & (1UL << 20)) == 0U)
  {
    ch[20] = (int32_t)(HAL_GetTick() / 1000U) * 10;   /* uptime, s */
  }
}

/* mid +- amp, sine with the given period; t in ms */
static int32_t wave(uint32_t t, uint32_t period_ms, int32_t phase_deg, int32_t mid, int32_t amp)
{
  const int32_t deg = (int32_t)(((uint64_t)(t % period_ms) * 360U) / period_ms) + phase_deg;
  return mid + (amp * gfx_sin(deg)) / 16384;
}

/* 0..max..0 triangle */
static int32_t triangle(uint32_t t, uint32_t period_ms, int32_t max)
{
  const uint32_t p = t % period_ms;
  const uint32_t half = period_ms / 2U;
  return (int32_t)(((uint64_t)((p < half) ? p : (period_ms - p)) * (uint32_t)max) / half);
}

/* 1 for `on_ms` out of every `period_ms` */
static int32_t pulse(uint32_t t, uint32_t period_ms, uint32_t offset_ms, uint32_t on_ms)
{
  return (((t + offset_ms) % period_ms) < on_ms) ? 1 : 0;
}

void panel_demo_update(int32_t *ch, uint32_t external_mask)
{
  const uint32_t t = HAL_GetTick();
  int32_t v[PANEL_CHANNELS];

  for (uint8_t i = 0U; i < PANEL_CHANNELS; i++) { v[i] = ch[i]; }

  /* REMOTE */
  v[0]  = wave(t,  8000U,   0,   50,   42);   /* pressure, dips into warn/alarm */
  v[1]  = wave(t, 11000U,  40, 15000, 13000);
  v[2]  = wave(t, 15000U,  90,   600,  350);
  v[3]  = wave(t,  6000U, 200,   250,  200);
  v[4]  = wave(t,  4000U,   0,  2300,  120);
  v[5]  = triangle(t, 10000U, 1000);
  v[6]  = wave(t,  7000U, 300,   500,  450);
  v[7]  = (v[5] > 200) ? 1 : 0;              /* pump runs while valve 1 is open */
  v[8]  = (v[2] > 600) ? 1 : 0;              /* fan when hot */
  v[9]  = pulse(t, 3000U, 0U, 1500U);
  v[10] = (v[0] >= 85) ? pulse(t, 500U, 0U, 250U) : 0;   /* blinking alarm */

  /* LOCAL */
  v[16] = wave(t,  9000U,   0,   165,  150);
  v[17] = wave(t,  9000U, 120,   165,  150);
  v[18] = wave(t,  9000U, 240,   165,  150);
  v[19] = wave(t,  9000U,  60,   240,   45);
  v[21] = pulse(t, 8000U, 0U, 4000U);
  v[22] = pulse(t, 12000U, 3000U, 6000U);
  v[23] = pulse(t, 5000U, 0U, 1000U);
  v[24] = pulse(t, 5000U, 2500U, 1000U);
  v[25] = triangle(t, 16000U, 1000);

  /* never overwrite a channel that real data has already claimed */
  for (uint8_t i = 0U; i < PANEL_CHANNELS; i++)
  {
    if ((external_mask & (1UL << i)) == 0U) { ch[i] = v[i]; }
  }
}
