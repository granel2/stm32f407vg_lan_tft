/**
  ******************************************************************************
  * @file    clock_page.c
  * @brief   TFT "CLOCK" page - see clock_page.h.
  *
  * Nothing is repainted on a timer: every item is compared with what is on
  * the glass and redrawn only when it differs - normally just the seconds
  * digit(s) once a second, minutes/hours/date as they roll over, the status
  * rows when a sync happens or fails.
  *
  * Layout (portrait 320 x 480, title bar and heartbeat drawn by tft_app.c):
  *   y  80  HH:MM:SS   7-segment, 34 x 64 px digits
  *   y 180  DD.MM.YYYY font scale 3, centred
  *   y 220  WEEKDAY    font scale 3, centred
  *   y 290  TZ:   / SYNC: / NTP:   status rows, font scale 2
  ******************************************************************************
  */
#include "clock_page.h"
#include "clock.h"
#include "clock_config.h"

#include <stdio.h>
#include <string.h>

#include "st7796s.h"

#define DIGIT_W       34U
#define DIGIT_H       64U
#define DIGIT_GAP     8U     /* between the two digits of a pair */
#define COLON_W       20U    /* slot for ':' between pairs */
#define TIME_Y        80U
#define TIME_X        ((320U - (6U * DIGIT_W + 3U * DIGIT_GAP + 2U * COLON_W)) / 2U)

#define BIG_SCALE     3U
#define BIG_CHARS     10U    /* "DD.MM.YYYY" and the longest weekday (9) */
#define DATE_Y        180U
#define WDAY_Y        220U

#define STAT_SCALE    2U
#define STAT_LABEL_X  20U
#define STAT_VALUE_X  (STAT_LABEL_X + 6U * 12U)   /* 6 label columns */
#define STAT_CHARS    19U
#define STAT_Y_TZ     290U
#define STAT_Y_SYNC   320U
#define STAT_Y_NTP    350U

#define COLOR_TIME    ST7796S_CYAN
#define COLOR_DATE    ST7796S_YELLOW
#define COLOR_WDAY    ST7796S_WHITE
#define COLOR_VALUE   ST7796S_CYAN
#define COLOR_BG      ST7796S_BLACK

#define NO_DIGIT      0xFFU  /* DrawDigit7: >9 = blank */

static const char *const wday_name[7] =
{
  "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY",
};

/* What is on the glass now - only differences get redrawn */
static uint8_t  s_digit[6];
static uint16_t s_date_key;   /* packed year%64/month/day, 0 = none drawn */
static uint8_t  s_wday;
static char     s_tz_text[STAT_CHARS + 1U];
static char     s_sync_text[STAT_CHARS + 1U];
static uint32_t s_status_sec; /* status texts are rebuilt (not redrawn) once a second */
static uint8_t  s_step;       /* which status row to check next */

static uint16_t digit_x(uint8_t i)
{
  const uint8_t pair = (uint8_t)(i / 2U);

  return (uint16_t)(TIME_X + pair * (2U * DIGIT_W + DIGIT_GAP + COLON_W) + (i % 2U) * (DIGIT_W + DIGIT_GAP));
}

static void draw_colons(void)
{
  for (uint8_t c = 0; c < 2U; c++)
  {
    const uint16_t x = (uint16_t)(digit_x((uint8_t)(c * 2U + 1U)) + DIGIT_W + (COLON_W - 6U) / 2U);

    ST7796S_FillRect(x, (uint16_t)(TIME_Y + DIGIT_H / 3U - 3U),      6U, 6U, COLOR_TIME);
    ST7796S_FillRect(x, (uint16_t)(TIME_Y + 2U * DIGIT_H / 3U - 3U), 6U, 6U, COLOR_TIME);
  }
}

/* Text centred in a fixed BIG_CHARS-wide field, padded with spaces on both
   sides so a shorter string fully covers a longer previous one. */
static void draw_big_centred(uint16_t y, const char *text, uint16_t color)
{
  char   buf[BIG_CHARS + 1U];
  size_t n   = strlen(text);
  size_t pad;

  if (n > BIG_CHARS) { n = BIG_CHARS; }
  pad = (BIG_CHARS - n) / 2U;
  memset(buf, ' ', BIG_CHARS);
  memcpy(buf + pad, text, n);
  buf[BIG_CHARS] = '\0';
  (void)ST7796S_DrawString((uint16_t)((320U - ST7796S_CharPitch(BIG_SCALE) * BIG_CHARS) / 2U), y,
                           buf, color, COLOR_BG, BIG_SCALE);
}

static void draw_value(uint16_t y, const char *text)
{
  char buf[STAT_CHARS + 1U];

  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(buf, sizeof(buf), "%-*s", (int)STAT_CHARS, text);
  #pragma GCC diagnostic pop
  (void)ST7796S_DrawString(STAT_VALUE_X, y, buf, COLOR_VALUE, COLOR_BG, STAT_SCALE);
}

/* Builds status row `which` (0 = TZ, 1 = SYNC) and draws it only if the
   text differs from what is on the glass. */
static void update_status(uint8_t which)
{
  char msg[STAT_CHARS + 1U];
  char *shown = (which == 0U) ? s_tz_text : s_sync_text;

  /* Every text below fits STAT_CHARS for real values; truncation would only
     cut the displayed field, which is fine. */
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  if (which == 0U)
  {
    const int32_t off = clock_utc_offset_min();
    snprintf(msg, sizeof(msg), "UTC%c%ld%s %s", (off < 0) ? '-' : '+',
             (long)((off < 0 ? -off : off) / 60), ((off % 60) != 0) ? ":30" : "",
             CLOCK_EU_DST ? ((off != CLOCK_TZ_OFFSET_MIN) ? "SUMMER" : "WINTER") : "");
  }
  else
  {
    ClockTime last;

    if (!clock_last_sync(&last))
    {
      snprintf(msg, sizeof(msg), clock_last_failed() ? "FAILED, RETRYING" : "WAITING");
    }
    else if (clock_last_failed())
    {
      snprintf(msg, sizeof(msg), "RETRY, LAST %02u:%02u", last.hour, last.min);
    }
    else
    {
      snprintf(msg, sizeof(msg), "OK AT %02u:%02u:%02u", last.hour, last.min, last.sec);
    }
  }
  #pragma GCC diagnostic pop

  if (strcmp(msg, shown) != 0)
  {
    draw_value((which == 0U) ? STAT_Y_TZ : STAT_Y_SYNC, msg);
    memcpy(shown, msg, sizeof(msg));
  }
}

void Clock_PageDraw(void)
{
  memset(s_digit, NO_DIGIT, sizeof(s_digit));
  for (uint8_t i = 0; i < 6U; i++)
  {
    ST7796S_DrawDigit7(digit_x(i), TIME_Y, DIGIT_W, DIGIT_H, NO_DIGIT, COLOR_TIME, COLOR_BG);
  }
  draw_colons();

  s_date_key = 0U;
  s_wday = 0xFFU;
  draw_big_centred(DATE_Y, "--.--.----", COLOR_DATE);

  (void)ST7796S_DrawString(STAT_LABEL_X, STAT_Y_TZ,   "TZ:",   ST7796S_WHITE, COLOR_BG, STAT_SCALE);
  (void)ST7796S_DrawString(STAT_LABEL_X, STAT_Y_SYNC, "SYNC:", ST7796S_WHITE, COLOR_BG, STAT_SCALE);
  (void)ST7796S_DrawString(STAT_LABEL_X, STAT_Y_NTP,  "NTP:",  ST7796S_WHITE, COLOR_BG, STAT_SCALE);
  draw_value(STAT_Y_NTP, CLOCK_NTP_SERVER);
  s_tz_text[0]   = '\0';
  s_sync_text[0] = '\0';
  update_status(0U);
  update_status(1U);
  s_status_sec = HAL_GetTick() / 1000U;
  s_step = 0U;
}

void Clock_PagePoll(void)
{
  ClockTime t;
  uint8_t   now_digit[6];
  uint8_t   drew = 0U;

  if (ST7796S_Busy())
  {
    return;  /* previous item still going out by DMA - next pass */
  }
  if (clock_get(&t))
  {
    now_digit[0] = (uint8_t)(t.hour / 10U); now_digit[1] = (uint8_t)(t.hour % 10U);
    now_digit[2] = (uint8_t)(t.min / 10U);  now_digit[3] = (uint8_t)(t.min % 10U);
    now_digit[4] = (uint8_t)(t.sec / 10U);  now_digit[5] = (uint8_t)(t.sec % 10U);

    /* Seconds first and every call: a changed digit is ~2 k px by DMA plus
       a few small segment fills, well under a millisecond each. */
    for (uint8_t i = 6U; i-- > 0U;)
    {
      if (now_digit[i] != s_digit[i])
      {
        ST7796S_DrawDigit7(digit_x(i), TIME_Y, DIGIT_W, DIGIT_H, now_digit[i], COLOR_TIME, COLOR_BG);
        s_digit[i] = now_digit[i];
        drew = 1U;
      }
    }
    if (drew)
    {
      return;  /* one kind of item per call */
    }

    /* Date and weekday: only when the day changes (or right after the
       first sync), one of them per call - each is a ~7 kB text line. */
    {
      const uint16_t key = (uint16_t)(((t.year % 64U) << 9) | ((uint16_t)t.month << 5) | t.day);

      if (key != s_date_key)
      {
        char date[16];
        snprintf(date, sizeof(date), "%02u.%02u.%04u", t.day, t.month, t.year);
        draw_big_centred(DATE_Y, date, COLOR_DATE);
        s_date_key = key;
        return;
      }
      if (t.wday != s_wday)
      {
        draw_big_centred(WDAY_Y, wday_name[t.wday % 7U], COLOR_WDAY);
        s_wday = t.wday;
        return;
      }
    }
  }

  /* Status rows: text rebuilt once a second (alternating TZ / SYNC), but
     drawn only when it actually changed. */
  if ((HAL_GetTick() / 1000U) != s_status_sec)
  {
    s_status_sec = HAL_GetTick() / 1000U;
    update_status(s_step);
    s_step ^= 1U;
  }
}
