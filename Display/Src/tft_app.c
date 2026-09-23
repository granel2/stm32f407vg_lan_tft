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

/* Layout of the bring-up screen (portrait, 320x480). The test pattern's
   bottom-right quarter is a plain grey block - the live counters go there. */
#define TFT_CNT_X       166U   /* left edge of the 5-digit counters */
#define TFT_CNT_DIGIT_W  24U
#define TFT_CNT_DIGIT_H  40U
#define TFT_UPTIME_Y    300U   /* seconds since boot */
#define TFT_FRAME_MS_Y  360U   /* measured full-screen fill time, ms */
#define TFT_BLINK_X     170U   /* 40x40 square toggling every second (= pattern's red square) */
#define TFT_BLINK_Y     250U
#define TFT_GREY        ST7796S_RGB(64, 64, 64)

static uint32_t tft_frame_ms;  /* last measured full-screen fill, for the log/screen */

/**
  * @brief  One-shot TFT hardware check at boot. Everything here is blocking
  *         (~7 s) - runs before lwIP on purpose. What to look for:
  *         UART : "[tft] id ..." verdict, full-frame fill time in ms
  *         panel: solid red -> green -> blue -> white (0.4 s each), then the
  *                test pattern rotated through all 4 orientations (0.8 s
  *                each), finally portrait with two counters bottom-right.
  */
void TFT_App_SmokeTest(void)
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

  /* 4. Final screen: portrait test pattern + live counters (TFT_App_AlivePoll) */
  ST7796S_SetRotation(0U);
  ST7796S_DrawTestPattern();
  ST7796S_DrawNumber7(TFT_CNT_X, TFT_FRAME_MS_Y, TFT_CNT_DIGIT_W, TFT_CNT_DIGIT_H,
                      tft_frame_ms, 5U, ST7796S_CYAN, TFT_GREY);
  Debug_Print("[tft] smoke test done, live counters running\r\n");
}

/**
  * @brief  Proof the link stays alive after boot: once a second redraw the
  *         uptime (seconds, cyan digits) and toggle a red/green square in the
  *         grey block. ~2.5 kB per update = ~2 ms at 10 MHz, so it never
  *         stalls the Ethernet main loop. Call every main-loop iteration.
  */
void TFT_App_AlivePoll(void)
{
  static uint32_t next_tick = 0;
  static uint8_t  phase = 0;

  if ((int32_t)(HAL_GetTick() - next_tick) < 0)
  {
    return;
  }
  next_tick = HAL_GetTick() + 1000U;
  phase ^= 1U;

  ST7796S_FillRect(TFT_BLINK_X, TFT_BLINK_Y, 40U, 40U, phase ? ST7796S_GREEN : ST7796S_RED);
  ST7796S_DrawNumber7(TFT_CNT_X, TFT_UPTIME_Y, TFT_CNT_DIGIT_W, TFT_CNT_DIGIT_H,
                      HAL_GetTick() / 1000U, 5U, ST7796S_CYAN, TFT_GREY);
}
