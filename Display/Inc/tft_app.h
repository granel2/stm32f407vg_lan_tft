/**
  ******************************************************************************
  * @file    tft_app.h
  * @brief   Application layer for the 3.5" ST7796S TFT: SPI3 bring-up,
  *          boot-time hardware smoke test, and the live on-screen heartbeat.
  *
  * Layering:
  *   st7796s.c/.h   - low-level controller driver (init sequence, RAMWR,
  *                     fills, rotation, 7-segment digits). Panel-specific,
  *                     no knowledge of this board or the application.
  *   tft_app.c/.h   - THIS module: owns the SPI3 handle, decides what goes
  *                     on screen and when. Board- and app-specific; this is
  *                     the file to extend for new screens/widgets.
  *   main.c         - calls TFT_App_GPIO_Init() + TFT_App_SPI3_Init() +
  *                     TFT_App_SmokeTest() once at boot (before
  *                     MX_LWIP_Init(), see main.c comment), then
  *                     TFT_App_AlivePoll() every main-loop iteration.
  *
  * Wiring / jumpers: see Display/docs/TFT_WIRING.md and
  * Display/docs/TFT_3.5_board.md. Pin assignment (SPI3 + CS/DC/RST/BL): see
  * main.h "Private defines".
  ******************************************************************************
  */
#ifndef TFT_APP_H
#define TFT_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* SPI3 handle for the TFT bus (PC10/11/12, header SV4). Exposed so
   HAL_SPI_MspInit()/Error_Handler() call sites elsewhere can reference it if
   ever needed; ordinary callers only need the functions below. */
extern SPI_HandleTypeDef hspi3;
/* DMA1 Stream5 / Channel 0 = SPI3_TX, used by ST7796S_FillRect() for large
   fills. Configured in HAL_SPI_MspInit(), IRQ in stm32f4xx_it.c. */
extern DMA_HandleTypeDef hdma_spi3_tx;

/**
  * @brief  Configure the TFT control GPIOs: CS (PA15), DC/RST/BL
  *         (PD6/PD5/PC7 - see main.h for the current pin assignment),
  *         all push-pull, RST idle high, DC/BL idle low. Call once, right
  *         after MX_GPIO_Init() and before TFT_App_SPI3_Init()/
  *         TFT_App_SmokeTest().
  */
void TFT_App_GPIO_Init(void);

/**
  * @brief  Configure SPI3 as the TFT master (20 MHz, mode 0, software CS).
  *         Must run before TFT_App_SmokeTest(). See tft_app.c for the
  *         prescaler/throughput trade-off notes.
  */
void TFT_App_SPI3_Init(void);

/**
  * @brief  One-shot boot-time hardware check (~7 s, blocking): controller ID
  *         readback to the debug UART, R/G/B/W fill with timing, test
  *         pattern in all 4 rotations, then switches to the operational
  *         status screen (see TFT_App_AlivePoll()). Call once, before
  *         lwIP/Ethernet bring-up so its blocking delays can't stall DHCP/RX.
  *
  * @param  server_str   "a.b.c.d:port" the TCP echo client will try to
  *                       reach - shown once on the SERVER: row. Persisted/
  *                       runtime-configurable now (Config/Inc/device_config.h),
  *                       formatted by main.c and passed as a plain string so
  *                       this module still doesn't need any lwIP/
  *                       tcp_echo_client/device_config headers.
  * @param  device_name  Persisted device name/label - shown once on the
  *                       NAME: row, same reasoning as server_str above.
  */
void TFT_App_SmokeTest(const char *server_str, const char *device_name);

/**
  * @brief  Replace the SETUP page's SERVER:/NAME: values after boot (e.g.
  *         after a save from the web config page). Redraws them right away
  *         if SETUP is the page showing, otherwise just stores them for the
  *         next time it is. Same plain-string contract as
  *         TFT_App_SmokeTest().
  */
void TFT_App_UpdateInfo(const char *server_str, const char *device_name);

/**
  * @brief  Non-blocking: once a second, redraws the LINK/DHCP/IP/TCP/UPTIME
  *         status screen and toggles a heartbeat square; returns immediately
  *         the rest of the time. Safe (and required) to call every
  *         main-loop iteration - see tft_app.c for the parameter contract
  *         (in particular, ip_str == "---" means "no address yet").
  */
void TFT_App_AlivePoll(const char *ip_str, uint8_t link_up, char ip_src, char tcp_state);

/**
  * @brief  Shows `text` below the status screen's LAST MSG: label for 2 s
  *         (word-wrapped by character count across up to STATUS_RX_MAX_ROWS
  *         rows, splitting on the server's own line breaks first), then
  *         TFT_App_AlivePoll() blanks it again on its own. Non-blocking.
  *         Call from main.c only when tcp_echo_client_take_last_rx()
  *         actually returned new data.
  */
void TFT_App_ShowReceived(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TFT_APP_H */
