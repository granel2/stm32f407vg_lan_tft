/**
  ******************************************************************************
  * @file    tft_app.c
  * @brief   Application layer for the 3.5" ST7796S TFT - see tft_app.h for
  *          the layering note. Extracted out of main.c so the display side
  *          can be modernised (fonts, network-status screen, ...) without
  *          touching the Ethernet/lwIP code that lives there.
  ******************************************************************************
  */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tft_app.h"
#include "st7796s.h"
#include "main.h"

SPI_HandleTypeDef hspi3;

/* Debug_Print() is main.c's UART logger (USART1, 921600 baud); declared in
   main.h so this module and main.c share one implementation. */

/**
  * @brief  TFT control lines: CS idle high, RST held high (pulsed in
  *         ST7796S_Init), DC/BL idle low (backlight off until the panel is
  *         initialised). PA15 is JTDI after reset - SWD-only debugging is
  *         unaffected by repurposing it. Written per-pin (not grouped by
  *         port) since CS/DC/RST/BL don't all share one GPIO port - see the
  *         pin defines in main.h. Also configures the page button
  *         (TFT_PAGEBTN_Pin, PC6/SV1.7): input, internal pull-up, so a
  *         plain switch/button to GND is all the external wiring needs -
  *         no external pull-up resistor required.
  *
  *         Enables GPIOC's clock itself: MX_GPIO_Init() only turns on
  *         A/B/D/H (nothing here used C before BL moved to PC7), and this
  *         must run before any register access to that port, including the
  *         WritePin calls below. GPIOA/B/D are already on by the time this
  *         runs (called right after MX_GPIO_Init()), but enabling them again
  *         is harmless - kept here too so this module stays self-contained
  *         and doesn't depend on main.c's enable order.
  */
void TFT_App_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TFT_BL_GPIO_Port, TFT_BL_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

  GPIO_InitStruct.Pin = TFT_CS_Pin;
  HAL_GPIO_Init(TFT_CS_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TFT_DC_Pin;
  HAL_GPIO_Init(TFT_DC_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TFT_RST_Pin;
  HAL_GPIO_Init(TFT_RST_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TFT_BL_Pin;
  HAL_GPIO_Init(TFT_BL_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TFT_PAGEBTN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(TFT_PAGEBTN_GPIO_Port, &GPIO_InitStruct);
}

/**
  * @brief  SPI3 master for the ST7796S TFT. APB1 = 40 MHz, prescaler /4 ->
  *         10 MHz SCK: comfortably inside the controller's write timing and
  *         slow enough for RDID readback over the module's SDA-O line. Once
  *         the panel is confirmed working, /2 (20 MHz) is worth trying for
  *         faster full-screen fills. Software CS (PA15 GPIO), mode 0.
  */
void TFT_App_SPI3_Init(void)
{
  hspi3.Instance = SPI3;
  hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES;
  hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT;
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ===========================================================================
 * Multi-page status screen (portrait, 320x480).
 *
 * Pages (TFT_Page): SETUP (link/DHCP/IP/server/TCP/uptime/frame-time),
 * RECEIVED (the full last TCP message, wrapped/multi-line), RESERVED (empty
 * placeholder - add real content here for a 4th page and bump
 * TFT_PAGE_COUNT). Cycled by a physical button on TFT_PAGEBTN_Pin
 * (PC6/SV1.7, active low, internal pull-up - see TFT_App_GPIO_Init()).
 *
 * Every page shares: a title row (name + "n/N" page indicator, top-right)
 * and a heartbeat square (bottom-left) that toggles every second regardless
 * of which page is showing - drawn by draw_page_chrome()/TFT_App_AlivePoll()
 * respectively. Font scale 2 -> each character cell is
 * ST7796S_CharPitch(2) = 12 px wide, 14 px tall.
 * ===========================================================================
 */
typedef enum
{
  TFT_PAGE_SETUP = 0,
  TFT_PAGE_RECEIVED,
  TFT_PAGE_RESERVED,
  TFT_PAGE_COUNT
} TFT_Page;

#define STATUS_FONT_SCALE  2U
#define STATUS_LABEL_X     20U
#define STATUS_VALUE_X     (STATUS_LABEL_X + 8U * 12U)   /* 8 label-char columns */
#define STATUS_VALUE_CHARS 16U    /* fixed width so a shorter new value fully
                                     overwrites a longer old one - no stale
                                     leftover characters from the previous
                                     draw (see status_draw_value()) */
#define STATUS_Y_TITLE     20U
#define STATUS_PAGE_IND_X  (320U - STATUS_LABEL_X - 5U * 12U)  /* right-aligned "n/N" */

/* SETUP page rows */
#define STATUS_Y_LINK      60U
#define STATUS_Y_DHCP      90U
#define STATUS_Y_IP        120U
#define STATUS_Y_SERVER    150U   /* tcp_echo_client's target address:port - static,
                                     set once from main.c's TCP_ECHO_SERVER_* macros */
#define STATUS_Y_TCP       180U
#define STATUS_Y_UPTIME    210U
#define STATUS_Y_FRAME     240U
#define STATUS_Y_CHIPID    270U   /* CRC32 of the 96-bit chip ID - see stm32_uid_crc32() */
#define STATUS_Y_NAME      300U   /* device_config's name field - static, set once from main.c */

/* STM32F4's 96-bit factory-programmed unique device ID: three consecutive
   32-bit words in OTP, memory-mapped read-only - no peripheral clock or
   init needed, see RM0090 section 39.1 ("Unique device ID register"). */
#define STM32_UID_BASE  0x1FFF7A10U
static inline uint32_t stm32_uid_word(uint8_t index)
{
  return *(const volatile uint32_t *)(uintptr_t)(STM32_UID_BASE + ((uint32_t)index * 4U));
}

/* Standard reflected CRC-32 (poly 0xEDB88320, init/final XOR 0xFFFFFFFF) -
   the same algorithm as zlib's crc32()/Python's binascii.crc32()/`cksum`,
   so the 8-hex-digit result on screen can be cross-checked with any of
   those against the full ID logged over UART. Bit-by-bit, no lookup
   table: this only ever runs once, over 12 bytes, so a 256-entry table
   would just spend FLASH for no measurable speed benefit here. */
static uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;

  for (uint32_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8U; bit++)
    {
      crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

/* CRC32 over the UID's 12 raw bytes, each 32-bit word taken big-endian
   (matches the byte order the hex log/other tools would see reading the
   three words as one 96-bit big-endian number) - a short, still
   collision-resistant stand-in for the unwieldy 24-hex-digit full ID
   (see Display/README.md for why: STM32's LOT_NUM field often looks like
   plain decimal text rather than "random" hex, per RM0090 39.1). */
static uint32_t stm32_uid_crc32(void)
{
  uint8_t bytes[12];

  for (uint8_t w = 0; w < 3U; w++)
  {
    uint32_t word = stm32_uid_word(w);
    bytes[(w * 4U) + 0U] = (uint8_t)(word >> 24);
    bytes[(w * 4U) + 1U] = (uint8_t)(word >> 16);
    bytes[(w * 4U) + 2U] = (uint8_t)(word >> 8);
    bytes[(w * 4U) + 3U] = (uint8_t)(word);
  }
  return crc32_compute(bytes, sizeof(bytes));
}

/* RECEIVED page: "LAST MSG:" label + up to STATUS_RX_MAX_ROWS wrapped rows.
   A whole page to itself now, so this can be much taller than when it had
   to share space with the SETUP fields above. */
#define STATUS_Y_RXLABEL   60U
#define STATUS_RX_CHARS    23U    /* (320 - 2*STATUS_LABEL_X) / ST7796S_CharPitch(STATUS_FONT_SCALE) */
#define STATUS_RX_ROW_H    18U    /* row pitch - a bit tighter than the glyph's own 14 px
                                     (STATUS_FONT_SCALE*7) so more rows fit */
#define STATUS_Y_RX0       90U
#define STATUS_RX_MAX_ROWS 18U    /* (STATUS_BLINK_Y - 20 - STATUS_Y_RX0) / STATUS_RX_ROW_H, rounded down */

/* RESERVED page */
#define STATUS_Y_RESERVED  60U

/* Shared by every page */
#define STATUS_BLINK_X     20U   /* 40x40 heartbeat square, toggles every second */
#define STATUS_BLINK_Y     434U

static uint32_t tft_frame_ms;      /* last measured full-screen fill, for the log/screen */
static char     s_server_str[32];  /* SERVER: value, stashed so re-entering the SETUP page
                                       after boot can redraw it without main.c's help */
static char     s_device_name[24]; /* NAME: value, same reasoning - see device_config.h */
static TFT_Page s_page = TFT_PAGE_SETUP;
static uint8_t  s_blink_phase;     /* file-scope (not local to TFT_App_AlivePoll) so
                                       draw_page_chrome() can repaint the heartbeat square
                                       in its current colour right after a page switch,
                                       instead of waiting for the next 1 Hz tick */

/* Last received TCP message: kept as raw text (not yet wrapped) so switching
   to the RECEIVED page redraws whatever arrived while some other page was
   showing. */
#define LAST_MSG_BUF_SIZE  220U
static char     s_last_msg[LAST_MSG_BUF_SIZE];
static uint8_t  s_has_msg;
static uint16_t s_rx_rows_used;    /* how many RX rows the current message actually drew -
                                       exactly this many get blanked if a shorter message
                                       replaces it */

/* Page button (PC6/SV1.7): active low, internal pull-up. Time-based
   debounce (BTN_DEBOUNCE_MS) instead of multi-sample filtering - simpler,
   and plenty for a manual push-button polled every main-loop iteration. */
#define BTN_DEBOUNCE_MS  50U
static uint8_t  s_btn_pressed;      /* debounced/latched last-known state */
static uint32_t s_btn_last_change;

/* Draws `text` right-padded to `chars` with spaces before handing it to
   ST7796S_DrawString(), so this always repaints the exact same pixel width
   regardless of how long the previous text there was - no stale leftover
   characters. `chars` must be <= sizeof(padded)-1 (32); all current callers
   (STATUS_VALUE_CHARS = 16, STATUS_RX_CHARS = 23, the 5-char page
   indicator) are well under that, but bump the buffer too if a wider field
   is ever added. */
static void status_draw_field(uint16_t x, uint16_t y, uint16_t chars, uint16_t color, const char *text)
{
  char padded[32 + 1U];

  /* GCC can't know at compile time that `text` (a plain const char *) fits
     the field, so -Wformat-truncation flags this as a possible overflow -
     but snprintf() truncating a too-long value to `chars` is exactly the
     intended, safe behaviour here, not a bug. */
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(padded, sizeof(padded), "%-*s", (int)chars, text);
  #pragma GCC diagnostic pop
  ST7796S_DrawString(x, y, padded, color, ST7796S_BLACK, STATUS_FONT_SCALE);
}

/* SETUP-page column value (STATUS_VALUE_X, STATUS_VALUE_CHARS wide, cyan) -
   what every LINK/DHCP/IP/SERVER/TCP/UPTIME/FRAME row uses. */
static void status_draw_value(uint16_t y, const char *text)
{
  status_draw_field(STATUS_VALUE_X, y, STATUS_VALUE_CHARS, ST7796S_CYAN, text);
}

/**
  * @brief  Clears the screen and draws what every page has in common: the
  *         title, a right-aligned "n/N" page indicator, and the heartbeat
  *         square in whatever colour it was last toggled to (so it doesn't
  *         flash to a default colour for up to a second after a page
  *         switch). Callers add their own page-specific content after this.
  */
static void draw_page_chrome(const char *title, TFT_Page page)
{
  char pgbuf[8];

  ST7796S_FillScreen(ST7796S_BLACK);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_TITLE, title, ST7796S_YELLOW, ST7796S_BLACK, STATUS_FONT_SCALE);
  snprintf(pgbuf, sizeof(pgbuf), "%u/%u", (unsigned)page + 1U, (unsigned)TFT_PAGE_COUNT);
  status_draw_field(STATUS_PAGE_IND_X, STATUS_Y_TITLE, 5U, ST7796S_YELLOW, pgbuf);
  ST7796S_FillRect(STATUS_BLINK_X, STATUS_BLINK_Y, 40U, 40U, s_blink_phase ? ST7796S_GREEN : ST7796S_RED);
}

/**
  * @brief  Wraps s_last_msg onto the RECEIVED page's rows: split on the
  *         server's own line breaks (CR, LF, or CRLF/LFCR treated as one),
  *         then each of those source lines is wrapped to STATUS_RX_CHARS-
  *         wide screen rows (a plain character-count wrap, not word-aware).
  *         Stops at STATUS_RX_MAX_ROWS - anything past that is silently
  *         dropped rather than overflowing into the heartbeat square. A
  *         message that fits in fewer rows than the *previous* one still
  *         blanks the leftover rows (s_rx_rows_used tracks how many to
  *         clear), so nothing stale lingers below shorter new text. Bytes
  *         outside the font's charset (see st7796s.c) render as blank
  *         cells rather than garbage, so e.g. Cyrillic text shows only its
  *         ASCII portions.
  */
static void redraw_received_body(void)
{
  uint16_t row = 0;
  const char *line_start = s_last_msg;

  while ((*line_start != '\0') && (row < STATUS_RX_MAX_ROWS))
  {
    const char *line_end = line_start;
    uint16_t pos;

    while ((*line_end != '\0') && (*line_end != '\r') && (*line_end != '\n')) { line_end++; }

    for (pos = 0; ((line_start + pos) < line_end) && (row < STATUS_RX_MAX_ROWS); pos += STATUS_RX_CHARS)
    {
      char chunk[STATUS_RX_CHARS + 1U];
      uint16_t remaining = (uint16_t)(line_end - (line_start + pos));
      uint16_t n = (remaining < STATUS_RX_CHARS) ? remaining : STATUS_RX_CHARS;

      memcpy(chunk, line_start + pos, n);
      chunk[n] = '\0';
      status_draw_field(STATUS_LABEL_X, (uint16_t)(STATUS_Y_RX0 + (row * STATUS_RX_ROW_H)),
                        STATUS_RX_CHARS, ST7796S_YELLOW, chunk);
      row++;
    }

    /* Skip the line break itself (CRLF/LFCR counts as one) before looking
       for the next source line. */
    line_start = line_end;
    if ((*line_start == '\r') || (*line_start == '\n'))
    {
      char first = *line_start++;
      if (((*line_start == '\r') || (*line_start == '\n')) && (*line_start != first)) { line_start++; }
    }
  }

  for (uint16_t r = row; r < s_rx_rows_used; r++)
  {
    status_draw_field(STATUS_LABEL_X, (uint16_t)(STATUS_Y_RX0 + (r * STATUS_RX_ROW_H)),
                      STATUS_RX_CHARS, ST7796S_YELLOW, "");
  }
  s_rx_rows_used = row;
}

static void draw_page_setup_static(void)
{
  draw_page_chrome("SETUP", TFT_PAGE_SETUP);

  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_LINK,   "LINK:",   ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_DHCP,   "DHCP:",   ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_IP,     "IP:",     ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_SERVER, "SERVER:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_TCP,    "TCP:",    ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_UPTIME, "UPTIME:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_FRAME,  "FRAME:",  ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_CHIPID, "CHIP ID:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_NAME,   "NAME:",    ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);

  status_draw_value(STATUS_Y_SERVER, s_server_str);
  status_draw_value(STATUS_Y_NAME,   s_device_name);
  {
    char msg[24];
    snprintf(msg, sizeof(msg), "%lu MS", (unsigned long)tft_frame_ms);
    status_draw_value(STATUS_Y_FRAME, msg);
  }
  {
    /* CRC32 of the 96-bit unique ID: 8 hex digits, fits one line (unlike
       the raw 24-digit ID - see stm32_uid_crc32()'s comment for why this
       is what's shown instead of the full value). Read-only/derived from
       OTP, set at the factory - never changes, so (like SERVER:/FRAME:
       above) this is drawn once here, not refreshed by
       TFT_App_AlivePoll(). */
    char msg[16];
    snprintf(msg, sizeof(msg), "%08lX", (unsigned long)stm32_uid_crc32());
    status_draw_value(STATUS_Y_CHIPID, msg);
  }
  /* LINK/DHCP/IP/TCP/UPTIME are left blank here - TFT_App_AlivePoll() fills
     them in on its very next call (forced immediately after a page switch,
     see there) and every second after, while this page stays active. */
}

static void draw_page_received_static(void)
{
  draw_page_chrome("RECEIVED", TFT_PAGE_RECEIVED);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_RXLABEL, "LAST MSG:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);

  s_rx_rows_used = 0U;
  if (s_has_msg)
  {
    redraw_received_body();
  }
  else
  {
    status_draw_field(STATUS_LABEL_X, STATUS_Y_RX0, STATUS_RX_CHARS, ST7796S_YELLOW, "(NOTHING YET)");
    s_rx_rows_used = 1U;
  }
}

static void draw_page_reserved_static(void)
{
  draw_page_chrome("PAGE 3", TFT_PAGE_RESERVED);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_RESERVED, "(RESERVED FOR", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, (uint16_t)(STATUS_Y_RESERVED + 30U), "FUTURE USE)", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
}

/* Add a case here (and bump TFT_PAGE_COUNT above) for a 4th+ page. */
static void draw_page_static(TFT_Page page)
{
  switch (page)
  {
    case TFT_PAGE_SETUP:    draw_page_setup_static();    break;
    case TFT_PAGE_RECEIVED: draw_page_received_static(); break;
    case TFT_PAGE_RESERVED:
    default:                draw_page_reserved_static(); break;
  }
}

/**
  * @brief  Records the most recent TCP receive and, if the RECEIVED page is
  *         currently showing, redraws it immediately; otherwise it's just
  *         stashed and appears next time the user switches to that page
  *         (draw_page_received_static() calls redraw_received_body() too).
  *         Non-blocking: no HAL_Delay(). Call from main.c only when
  *         tcp_echo_client_take_last_rx() actually returned new data.
  */
void TFT_App_ShowReceived(const char *text)
{
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(s_last_msg, sizeof(s_last_msg), "%s", text);
  #pragma GCC diagnostic pop
  s_has_msg = 1U;

  if (s_page == TFT_PAGE_RECEIVED)
  {
    redraw_received_body();
  }
}

void TFT_App_UpdateInfo(const char *server_str, const char *device_name)
{
  snprintf(s_server_str, sizeof(s_server_str), "%s", server_str);
  snprintf(s_device_name, sizeof(s_device_name), "%s", device_name);

  if (s_page == TFT_PAGE_SETUP)
  {
    status_draw_value(STATUS_Y_SERVER, s_server_str);
    status_draw_value(STATUS_Y_NAME,   s_device_name);
  }
}

/**
  * @brief  One-shot TFT hardware check at boot. Everything here is blocking
  *         (~7 s) - runs before lwIP on purpose. What to look for:
  *         UART : "[tft] id ..." verdict, full-frame fill time in ms
  *         panel: solid red -> green -> blue -> white (0.4 s each), then the
  *                test pattern rotated through all 4 orientations (0.8 s
  *                each), finally the multi-page status screen, starting on
  *                SETUP (see draw_page_static() and TFT_App_AlivePoll()).
  *
  * @param  server_str   "a.b.c.d:port" the TCP client will (re)connect to -
  *                       stashed and shown on the SETUP page's SERVER: row.
  * @param  device_name  device_config's name field - stashed and shown on
  *                       the SETUP page's NAME: row.
  */
void TFT_App_SmokeTest(const char *server_str, const char *device_name)
{
  static const uint16_t fills[4] = { ST7796S_RED, ST7796S_GREEN, ST7796S_BLUE, ST7796S_WHITE };
  uint8_t id[4];
  uint32_t t0, id24;
  char msg[128];

  ST7796S_Init(&hspi3);

  /* 1. Link check via RDID4. Needs SDA-O -> PC11; without it the bus reads
     back all-0 or all-1, which is reported as such rather than as a fault.
     A 1-bit left shift of the reply is normal for 4-wire SPI reads. */
  ST7796S_ReadID(id);
  id24 = ((uint32_t)id[1] << 16) | ((uint32_t)id[2] << 8) | id[3];
  snprintf(msg, sizeof(msg), "[tft] RDID4(0xD3) = %02X %02X %02X %02X -> %s\r\n",
           id[0], id[1], id[2], id[3],
           ((id[0] & id[1] & id[2] & id[3]) == 0xFFU) ? "bus idle high: SDA-O not wired or no power" :
           ((id[0] | id[1] | id[2] | id[3]) == 0x00U) ? "all zero: SDA-O not wired or panel not answering" :
           (id24 == 0x007796U)                       ? "ST7796S confirmed" :
           (((id24 >> 1) & 0xFFFFFFU) == 0x007796U)  ? "ST7796S confirmed (1-bit shifted reply)" :
                                                       "unexpected ID - other controller or bad wiring");
  Debug_Print(msg);

  /* 2. Colour check + throughput: time one full-screen fill (307 200 B).
     Expect ~250 ms at 10 MHz SCK, ~125 ms at 20 MHz. */
  for (uint8_t i = 0; i < 4U; i++)
  {
    t0 = HAL_GetTick();
    ST7796S_FillScreen(fills[i]);
    tft_frame_ms = HAL_GetTick() - t0;
    HAL_Delay(400);
  }
  snprintf(msg, sizeof(msg), "[tft] full-screen fill = %lu ms (%lu kB/s)\r\n",
           (unsigned long)tft_frame_ms,
           (unsigned long)((tft_frame_ms > 0U) ? (300U * 1000U / tft_frame_ms) : 0U));
  Debug_Print(msg);

  /* 3. MADCTL check: the pattern must appear upright in every orientation,
     white border on all four edges, red bar 6th from the left. */
  for (uint8_t r = 0; r < 4U; r++)
  {
    ST7796S_SetRotation(r);
    ST7796S_DrawTestPattern();
    HAL_Delay(800);
  }

  /* 4. Set up the multi-page status screen, starting on SETUP.
     MUST reset rotation to portrait first: the loop above leaves it at
     r=3, where tft_height is 320 (landscape), not the 480 every Y position
     below assumes - anything past y=320 would silently get clipped or
     vanish entirely (ST7796S_FillRect() drops draws once y >= tft_height). */
  ST7796S_SetRotation(0U);
  snprintf(s_server_str, sizeof(s_server_str), "%s", server_str);
  snprintf(s_device_name, sizeof(s_device_name), "%s", device_name);
  snprintf(msg, sizeof(msg), "[tft] chip UID = %08lX%08lX%08lX (CRC32 %08lX)\r\n",
           (unsigned long)stm32_uid_word(0), (unsigned long)stm32_uid_word(1), (unsigned long)stm32_uid_word(2),
           (unsigned long)stm32_uid_crc32());
  Debug_Print(msg);
  s_page = TFT_PAGE_SETUP;
  draw_page_static(s_page);
  Debug_Print("[tft] smoke test done, status screen running\r\n");
}

/**
  * @brief  Proof the link stays alive after boot, the page button, and the
  *         active page's live fields: once a second, redraw whatever the
  *         current page needs and toggle the heartbeat square. ~3 kB per
  *         update at STATUS_FONT_SCALE = a few ms at 10 MHz, so it never
  *         stalls the Ethernet main loop. Call every main-loop iteration -
  *         the button check and the 1 Hz gate are both internal.
  *
  * @param  ip_str    Current IPv4 address as text (e.g. "192.168.1.42"), or
  *                    the literal string "---" if none has been assigned
  *                    yet (link down, or DHCP still in progress).
  * @param  link_up    Non-zero if the PHY reports link up.
  * @param  tcp_state  tcp_echo_client_state_char() passthrough: 'I' = idle
  *                    (about to retry), 'C' = connecting, 'E' = connected.
  *                    Any other value is shown as IDLE, so a future state
  *                    code added to tcp_echo_client.c fails safe here
  *                    instead of printing a raw letter.
  */
void TFT_App_AlivePoll(const char *ip_str, uint8_t link_up, char tcp_state)
{
  static uint32_t next_tick = 0;
  const uint8_t   has_ip = (strcmp(ip_str, "---") != 0) ? 1U : 0U;

  /* Page button - checked every call (not gated by the 1 Hz limiter below)
     for a responsive UI. Debounced by time rather than multi-sampling:
     a change is only accepted once BTN_DEBOUNCE_MS has passed since the
     last accepted change, which is enough to ride out mechanical bounce
     on a manual push-button. Triggers on the press edge (pull-up released
     -> grounded), not the release. */
  {
    uint8_t raw_pressed = (HAL_GPIO_ReadPin(TFT_PAGEBTN_GPIO_Port, TFT_PAGEBTN_Pin) == GPIO_PIN_RESET) ? 1U : 0U;

    if ((raw_pressed != s_btn_pressed) && ((HAL_GetTick() - s_btn_last_change) >= BTN_DEBOUNCE_MS))
    {
      s_btn_last_change = HAL_GetTick();
      s_btn_pressed = raw_pressed;
      if (raw_pressed)
      {
        s_page = (TFT_Page)(((unsigned)s_page + 1U) % (unsigned)TFT_PAGE_COUNT);
        draw_page_static(s_page);
        next_tick = HAL_GetTick();  /* redraw this page's live fields below right
                                        away instead of waiting up to 1 s */
      }
    }
  }

  if ((int32_t)(HAL_GetTick() - next_tick) < 0)
  {
    return;
  }
  next_tick = HAL_GetTick() + 1000U;
  s_blink_phase ^= 1U;

  ST7796S_FillRect(STATUS_BLINK_X, STATUS_BLINK_Y, 40U, 40U, s_blink_phase ? ST7796S_GREEN : ST7796S_RED);

  if (s_page == TFT_PAGE_SETUP)
  {
    status_draw_value(STATUS_Y_LINK, link_up ? "UP" : "DOWN");
    status_draw_value(STATUS_Y_DHCP, !link_up ? "---" : has_ip ? "OK" : "WAITING");
    status_draw_value(STATUS_Y_IP,   ip_str);
    status_draw_value(STATUS_Y_TCP,  (tcp_state == 'C') ? "CONNECTING" :
                                      (tcp_state == 'E') ? "CONNECTED"  : "IDLE");
    {
      char msg[24];
      snprintf(msg, sizeof(msg), "%lu S", (unsigned long)(HAL_GetTick() / 1000U));
      status_draw_value(STATUS_Y_UPTIME, msg);
    }
  }
  /* RECEIVED and RESERVED have nothing that needs a 1 Hz refresh - RECEIVED
     is updated on arrival by TFT_App_ShowReceived(), RESERVED is static. */
}
