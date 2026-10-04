/**
  ******************************************************************************
  * @file    keypad_page.c
  * @brief   TFT "I2C KBD" page - see keypad_page.h.
  *
  * Lines live in a ring of KP_ROWS entries; s_redraw_from marks the first
  * screen row that no longer matches the ring. Appending to a page that is
  * not yet full dirties one row; once full, every append scrolls and
  * dirties all of them. Keypad_PagePoll() redraws one row per call (~3 ms
  * of DMA at scale 2), so a scroll never holds the main loop for long.
  ******************************************************************************
  */
#include "keypad_page.h"
#include "keypad_config.h"

#include <stdio.h>
#include <string.h>

#include "st7796s.h"
#include "tft_app.h"
#include "main.h"

#define KP_X           20U
#define KP_SCALE       2U
#define KP_CHARS       23U    /* (320 - 2*KP_X) / ST7796S_CharPitch(KP_SCALE) */
#define KP_Y_STATS     60U
#define KP_Y_ROW0      90U
#define KP_ROW_H       18U
#define KP_ROWS        18U    /* up to the heartbeat square at y = 434 */
#define KP_STATS_MS    250U   /* counter row: checked this often */

#define REDRAW_NONE    0xFFU

typedef struct
{
  char     text[KP_CHARS + 1U];
  uint16_t color;
} KpLine;

static KpLine   s_lines[KP_ROWS];
static uint8_t  s_head;                    /* ring index of the top (oldest) row */
static uint8_t  s_count;
static uint8_t  s_redraw_from = REDRAW_NONE;
static char     s_stats_drawn[KP_CHARS + 1U];
static uint32_t s_stats_next;

static void draw_row(uint16_t y, uint16_t color, const char *text)
{
  char padded[KP_CHARS + 1U];

  /* Padded to the full width so a shorter line wipes a longer old one */
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(padded, sizeof(padded), "%-*s", (int)KP_CHARS, text);
  #pragma GCC diagnostic pop
  ST7796S_DrawString(KP_X, y, padded, color, ST7796S_BLACK, KP_SCALE);
}

static void add_line(uint16_t color, const char *text)
{
  KpLine *l;

  if (s_count < KP_ROWS)
  {
    l = &s_lines[(s_head + s_count) % KP_ROWS];
    if (s_count < s_redraw_from)
    {
      s_redraw_from = s_count;
    }
    s_count++;
  }
  else
  {
    l = &s_lines[s_head];                  /* oldest goes, the rest move up */
    s_head = (uint8_t)((s_head + 1U) % KP_ROWS);
    s_redraw_from = 0U;
  }
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(l->text, sizeof(l->text), "%s", text);
  #pragma GCC diagnostic pop
  l->color = color;
}

static char reason_tag(uint8_t r)
{
  switch (r)
  {
    case KBD_END_ENTER:   return 'E';
    case KBD_END_MAXLEN:  return 'M';
    case KBD_END_TIMEOUT: return 'T';
    case KBD_END_CLEAR:   return 'C';
    default:              return '?';
  }
}

static const char *reason_word(uint8_t r)
{
  switch (r)
  {
    case KBD_END_ENTER:   return "ENTER";
    case KBD_END_MAXLEN:  return "MAX LEN";
    case KBD_END_TIMEOUT: return "TIMEOUT";
    case KBD_END_CLEAR:   return "CLEAR";
    default:              return "?";
  }
}

void Keypad_PagePacket(const KeypadPacket *p)
{
  char line[48];

  /* Someone is typing: wake the backlight. START is a module reset, not a
     person, so it leaves the dimming alone. */
  if (p->type != KBD_PKT_START)
  {
    TFT_App_UserActivity();
  }

  switch (p->type)
  {
    case KBD_PKT_GROUP:
    {
      const uint8_t n = (p->len > 0U) ? (uint8_t)(p->len - 1U) : 0U;
#if KEYPAD_LOG_TEXT
      snprintf(line, sizeof(line), "#%u %c %.*s", (unsigned)p->seq, reason_tag(p->data[0]),
               (int)n, (const char *)&p->data[1]);
#else
      snprintf(line, sizeof(line), "#%u %c %u CHARS", (unsigned)p->seq, reason_tag(p->data[0]),
               (unsigned)n);
#endif
      add_line(ST7796S_GREEN, line);
      memset(line, 0, sizeof(line));       /* do not keep typed passwords on the stack */
      return;
    }
    case KBD_PKT_CANCEL:
      snprintf(line, sizeof(line), "#%u CANCEL %s", (unsigned)p->seq, reason_word(p->data[0]));
      add_line(ST7796S_YELLOW, line);
      return;
    case KBD_PKT_KEY:
      snprintf(line, sizeof(line), "#%u KEY %c %s", (unsigned)p->seq,
               (char)(p->data[0] & 0x7FU), ((p->data[0] & 0x80U) != 0U) ? "DOWN" : "UP");
      add_line(ST7796S_WHITE, line);
      return;
    case KBD_PKT_START:
      snprintf(line, sizeof(line), "#%u START FW %u RST %02X", (unsigned)p->seq,
               (unsigned)p->data[0], (unsigned)p->data[1]);
      add_line(ST7796S_CYAN, line);
      return;
    default:
      snprintf(line, sizeof(line), "#%u TYPE %02X %u BYTES", (unsigned)p->seq,
               (unsigned)p->type, (unsigned)p->len);
      add_line(ST7796S_MAGENTA, line);
      return;
  }
}

void Keypad_PageLost(uint32_t count)
{
  char line[24];

  snprintf(line, sizeof(line), "LOST %lu", (unsigned long)count);
  add_line(ST7796S_RED, line);
}

void Keypad_PageErrors(uint32_t crc_err, uint32_t late)
{
  char line[32];

  snprintf(line, sizeof(line), "CRC %lu LATE %lu", (unsigned long)crc_err, (unsigned long)late);
  add_line(ST7796S_RED, line);
}

void Keypad_PageDraw(void)
{
  /* The title bar left the rest of the screen black */
  s_stats_drawn[0] = '\0';
  s_stats_next = HAL_GetTick();
  s_redraw_from = 0U;
}

void Keypad_PagePoll(void)
{
  if (ST7796S_Busy())
  {
    return;  /* previous row still going out by DMA - next pass */
  }

  /* Counter row: RX = transfers to our address (0 while the module sends =
     wiring or wrong module firmware), OK = accepted packets, ERR = CRC + bad
     frames + bus errors. RX = OK with ERR 0 is a clean bus. The address
     itself is in the title. */
  if ((int32_t)(HAL_GetTick() - s_stats_next) >= 0)
  {
    KeypadStats st;
    char        line[48];

    s_stats_next = HAL_GetTick() + KP_STATS_MS;
    keypad_get_stats(&st);
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(line, sizeof(line), "RX %lu OK %lu ERR %lu", (unsigned long)st.addr,
             (unsigned long)st.accepted,
             (unsigned long)(st.crc_err + st.bad_frame + st.bus_err));
    #pragma GCC diagnostic pop
    line[KP_CHARS] = '\0';
    if (strcmp(line, s_stats_drawn) != 0)
    {
      draw_row(KP_Y_STATS, ST7796S_WHITE, line);
      memcpy(s_stats_drawn, line, sizeof(s_stats_drawn));
      return;
    }
  }

  if (s_redraw_from == REDRAW_NONE)
  {
    return;
  }
  if (s_count == 0U)
  {
    draw_row(KP_Y_ROW0, ST7796S_WHITE, "WAITING FOR MODULE");  /* first line overwrites it */
    s_redraw_from = REDRAW_NONE;
    return;
  }

  {
    const uint8_t r = s_redraw_from;
    const KpLine *l = &s_lines[(s_head + r) % KP_ROWS];

    draw_row((uint16_t)(KP_Y_ROW0 + (uint16_t)r * KP_ROW_H), l->color, l->text);
    s_redraw_from = (r + 1U < s_count) ? (uint8_t)(r + 1U) : REDRAW_NONE;
  }
}

/* ---- "KBD HELP" page: what the I2C KBD lines mean ------------------------ */

typedef struct
{
  const char *text;
  uint16_t    color;
} HelpRow;

#define HELP_Y0     60U
#define HELP_ROW_H  18U

/* Max KP_CHARS (23) per row, 20 rows fit above the heartbeat square. Colours
   match the I2C KBD lines they explain. */
static const HelpRow k_help[] =
{
  { "LINE: #SEQ TYPE DATA",    ST7796S_WHITE  },
  { "GROUP, ENDED BY:",        ST7796S_WHITE  },
  { " E  KEY #",               ST7796S_GREEN  },
  { " M  16 CHARS - AUTO",     ST7796S_GREEN  },
  { " T  PAUSE 10 S",          ST7796S_GREEN  },
  { " C  KEY *",               ST7796S_GREEN  },
  { "CANCEL:",                 ST7796S_WHITE  },
  { " CLEAR    KEY *",         ST7796S_YELLOW },
  { " TIMEOUT  NO KEY 10 S",   ST7796S_YELLOW },
  { "START FW N RST XX:",      ST7796S_WHITE  },
  { " 01 POWER ON",            ST7796S_CYAN   },
  { " 02 RESET BUTTON",        ST7796S_CYAN   },
  { " 04 BROWN-OUT",           ST7796S_CYAN   },
  { " 08 WATCHDOG",            ST7796S_CYAN   },
  { "COUNTERS:",               ST7796S_WHITE  },
  { " RX  TRANSFERS IN",       ST7796S_WHITE  },
  { " OK  PACKETS ACCEPTED",   ST7796S_WHITE  },
  { " ERR CRC/FRAME/BUS",      ST7796S_RED    },
  { " LOST N  SEQ GAP",        ST7796S_RED    },
  { "# NEXT PAGE  N# PAGE N",  ST7796S_YELLOW },
};
#define HELP_ROWS  (sizeof(k_help) / sizeof(k_help[0]))

static uint8_t s_help_next = REDRAW_NONE;  /* next row to draw */

void Keypad_HelpDraw(void)
{
  s_help_next = 0U;
}

void Keypad_HelpPoll(void)
{
  if ((s_help_next == REDRAW_NONE) || ST7796S_Busy())
  {
    return;
  }
  ST7796S_DrawString(KP_X, (uint16_t)(HELP_Y0 + (uint16_t)s_help_next * HELP_ROW_H),
                     k_help[s_help_next].text, k_help[s_help_next].color, ST7796S_BLACK, KP_SCALE);
  s_help_next = (s_help_next + 1U < HELP_ROWS) ? (uint8_t)(s_help_next + 1U) : REDRAW_NONE;
}
