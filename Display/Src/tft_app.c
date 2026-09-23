/**
  ******************************************************************************
  * @file    tft_app.c
  * @brief   Application layer for the 3.5" ST7796S TFT - see tft_app.h for
  *          the layering note. Extracted out of main.c so the display side
  *          can be modernised (fonts, network-status screen, ...) without
  *          touching the Ethernet/lwIP code that lives there.
  ******************************************************************************
  */
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
  *         pin defines in main.h.
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

/* Layout of the operational status screen (portrait, 320x480), shown after
   the one-shot bring-up test below finishes. Two-column form: a label at
   STATUS_LABEL_X (drawn once, never changes) and a value at STATUS_VALUE_X
   (redrawn by TFT_App_AlivePoll() every second). Font scale 2 -> each
   character cell is ST7796S_CharPitch(2) = 12 px wide, 14 px tall. */
#define STATUS_FONT_SCALE  2U
#define STATUS_LABEL_X     20U
#define STATUS_VALUE_X     (STATUS_LABEL_X + 8U * 12U)   /* 8 label-char columns */
#define STATUS_VALUE_CHARS 16U    /* fixed width so a shorter new value fully
                                     overwrites a longer old one - no stale
                                     leftover characters from the previous
                                     draw (see status_draw_value()) */
#define STATUS_Y_TITLE     20U
#define STATUS_Y_LINK      60U
#define STATUS_Y_DHCP      90U
#define STATUS_Y_IP        120U
#define STATUS_Y_SERVER    150U   /* tcp_echo_client's target address:port - static,
                                     set once from main.c's TCP_ECHO_SERVER_* macros */
#define STATUS_Y_TCP       180U
#define STATUS_Y_UPTIME    210U
#define STATUS_Y_FRAME     240U
#define STATUS_BLINK_X     20U   /* 40x40 heartbeat square, toggles every second */
#define STATUS_BLINK_Y     280U

static uint32_t tft_frame_ms;  /* last measured full-screen fill, for the log/screen */

/* Draws `text` right-padded to STATUS_VALUE_CHARS with spaces before
   handing it to ST7796S_DrawString(), so this always repaints the exact
   same pixel width regardless of how long the previous value was. */
static void status_draw_value(uint16_t y, const char *text)
{
  char padded[STATUS_VALUE_CHARS + 1U];

  /* GCC can't know at compile time that `text` (a plain const char *) fits
     the field, so -Wformat-truncation flags this as a possible overflow -
     but snprintf() truncating a too-long value to STATUS_VALUE_CHARS is
     exactly the intended, safe behaviour here, not a bug. */
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(padded, sizeof(padded), "%-*s", (int)STATUS_VALUE_CHARS, text);
  #pragma GCC diagnostic pop
  ST7796S_DrawString(STATUS_VALUE_X, y, padded, ST7796S_CYAN, ST7796S_BLACK, STATUS_FONT_SCALE);
}

/**
  * @brief  Draws the status screen's static parts once: title, all labels,
  *         the one-shot frame-fill-time measurement, and the tcp_echo_client
  *         target address:port (also static - it's a compile-time constant,
  *         see tcp_echo_client.h). The values next to LINK/DHCP/IP/TCP/UPTIME
  *         are left blank here - TFT_App_AlivePoll() fills them in on its
  *         first call and every second after.
  *
  * @param  server_str  "a.b.c.d:port" the TCP client is (re)connecting to,
  *                      formatted by main.c from TCP_ECHO_SERVER_* - kept out
  *                      of this module so it doesn't need tcp_echo_client.h
  *                      (and the lwIP headers that pulls in).
  */
static void status_draw_static(const char *server_str)
{
  ST7796S_FillScreen(ST7796S_BLACK);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_TITLE, "STM32F407 STATUS", ST7796S_YELLOW, ST7796S_BLACK, STATUS_FONT_SCALE);

  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_LINK,   "LINK:",   ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_DHCP,   "DHCP:",   ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_IP,     "IP:",     ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_SERVER, "SERVER:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_TCP,    "TCP:",    ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_UPTIME, "UPTIME:", ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);
  ST7796S_DrawString(STATUS_LABEL_X, STATUS_Y_FRAME,  "FRAME:",  ST7796S_WHITE, ST7796S_BLACK, STATUS_FONT_SCALE);

  status_draw_value(STATUS_Y_SERVER, server_str);
  {
    char msg[24];
    snprintf(msg, sizeof(msg), "%lu MS", (unsigned long)tft_frame_ms);
    status_draw_value(STATUS_Y_FRAME, msg);
  }
}

/**
  * @brief  One-shot TFT hardware check at boot. Everything here is blocking
  *         (~7 s) - runs before lwIP on purpose. What to look for:
  *         UART : "[tft] id ..." verdict, full-frame fill time in ms
  *         panel: solid red -> green -> blue -> white (0.4 s each), then the
  *                test pattern rotated through all 4 orientations (0.8 s
  *                each), finally the LINK/DHCP/IP/TCP/UPTIME status screen
  *                (see status_draw_static() and TFT_App_AlivePoll()).
  *
  * @param  server_str  "a.b.c.d:port" the TCP client will (re)connect to -
  *                      passed through to the static SERVER: row, see
  *                      status_draw_static().
  */
void TFT_App_SmokeTest(const char *server_str)
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

  /* 4. Set up the operational status screen (labels + one-shot frame-time
     value). TFT_App_AlivePoll() fills in LINK/DHCP/IP/TCP/UPTIME and keeps
     them current from here on - see main.c's while(1) loop. */
  status_draw_static(server_str);
  Debug_Print("[tft] smoke test done, status screen running\r\n");
}

/**
  * @brief  Proof the link stays alive after boot, and the actual network
  *         status screen: once a second, redraw LINK/DHCP/IP/TCP/UPTIME and
  *         toggle a heartbeat square. ~3 kB per update at STATUS_FONT_SCALE
  *         = a few ms at 10 MHz, so it never stalls the Ethernet main loop.
  *         Call every main-loop iteration; internally rate-limited to 1 Hz.
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
  static uint8_t  phase = 0;
  const uint8_t   has_ip = (strcmp(ip_str, "---") != 0) ? 1U : 0U;

  if ((int32_t)(HAL_GetTick() - next_tick) < 0)
  {
    return;
  }
  next_tick = HAL_GetTick() + 1000U;
  phase ^= 1U;

  ST7796S_FillRect(STATUS_BLINK_X, STATUS_BLINK_Y, 40U, 40U, phase ? ST7796S_GREEN : ST7796S_RED);

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
