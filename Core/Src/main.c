/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"
#include "ethernetif.h"
#include "tcp_echo_client.h"
#include "stm32f4xx_hal_eth.h"
#include "lwip/stats.h"
#include "tft_app.h"
#include "device_config.h"
#include "config_server.h"
#include "config_http.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */
struct netif gnetif;
extern ETH_HandleTypeDef EthHandle; /* defined in ethernetif.c */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART3_UART_Init(void);
static void Test_Blink_LEDs(void);
/* USER CODE BEGIN PFP */
static void MX_LWIP_Init(void);
static void netif_status_callback(struct netif *netif);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* Hold the LAN8720 in reset (PB10/nRST) as early as firmware can run, before
     R3's 10k pull-up can release it on its own while VDD/clocks are still
     ramping up. Previously nRST wasn't touched by firmware until deep inside
     HAL_ETH_MspInit() (called from MX_LWIP_Init() below), well after
     SystemClock_Config() and other peripheral inits - confirmed on real
     hardware (PROJECT_GUIDE.md) that manually grounding nRST starting from
     power-up (instead of relying on R3 + this late firmware assert) was
     needed to reliably get the LAN8720's REFCLKO block to start. Asserting
     nRST here, before anything else, removes most of that gap; the actual
     5 s hold + release still happens later in HAL_ETH_MspInit(). */
  {
    GPIO_InitTypeDef nrst_early = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    nrst_early.Pin = GPIO_PIN_10;
    nrst_early.Mode = GPIO_MODE_OUTPUT_PP;
    nrst_early.Pull = GPIO_NOPULL;
    nrst_early.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &nrst_early);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
  }
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  TFT_App_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  /* Print the firmware build date/time on every boot (cold-boot or
     NVIC_SystemReset() escalation alike) so a pasted log can always be
     matched to the exact binary that produced it - __DATE__/__TIME__ are
     baked in by the compiler at build time, so this can't go stale the way
     a manually-maintained version number could. */
  {
    char boot_msg[64];
    snprintf(boot_msg, sizeof(boot_msg), "\r\n[boot] firmware built %s %s\r\n", __DATE__, __TIME__);
    Debug_Print(boot_msg);
  }
  /* Load persisted settings (server address, device name, static-IP-vs-DHCP
     choice - see Config/Inc/device_config.h) before anything that needs
     them: the SETUP page's SERVER:/NAME: rows below, and MX_LWIP_Init()'s
     static/DHCP branch further down. Falls back to compiled-in defaults on
     first boot (blank Flash) or a version mismatch. */
  device_config_load();

  /* TFT bring-up runs before lwIP so its blocking HAL_Delay()s can't stall
     Ethernet RX/DHCP; the panel just shows a static test pattern afterwards. */
  TFT_App_SPI3_Init();
  {
    const DeviceConfig *cfg = device_config_get();
    char server_str[24];
    snprintf(server_str, sizeof(server_str), "%u.%u.%u.%u:%u",
             cfg->server_ip[0], cfg->server_ip[1], cfg->server_ip[2], cfg->server_ip[3], cfg->server_port);
    TFT_App_SmokeTest(server_str, cfg->name);
  }
  MX_LWIP_Init();
  tcp_echo_client_init();
  config_server_init();
  config_http_init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    Test_Blink_LEDs();
    config_http_poll();

    /* A config save (from the web page or the port-7000 text protocol) bumps
       device_config_revision(). React here, so neither config module needs
       to know about the display or the TCP client: refresh SERVER:/NAME: on
       the SETUP page, and if the server address itself changed, drop the
       current connection and reconnect to the new one right away. IP-mode
       changes are not handled here - they need a reboot (MX_LWIP_Init()). */
    {
      static uint32_t seen_rev = 0U;
      static uint8_t  seen_ip[4];
      static uint16_t seen_port;
      static uint8_t  seen_init = 0U;
      const DeviceConfig *cfg = device_config_get();

      if (seen_init == 0U)
      {
        memcpy(seen_ip, cfg->server_ip, sizeof(seen_ip));
        seen_port = cfg->server_port;
        seen_init = 1U;
      }
      if (device_config_revision() != seen_rev)
      {
        char server_str[24];

        seen_rev = device_config_revision();
        snprintf(server_str, sizeof(server_str), "%u.%u.%u.%u:%u",
                 cfg->server_ip[0], cfg->server_ip[1], cfg->server_ip[2], cfg->server_ip[3], cfg->server_port);
        TFT_App_UpdateInfo(server_str, cfg->name);

        if ((memcmp(seen_ip, cfg->server_ip, sizeof(seen_ip)) != 0) || (seen_port != cfg->server_port))
        {
          memcpy(seen_ip, cfg->server_ip, sizeof(seen_ip));
          seen_port = cfg->server_port;
          tcp_echo_client_restart();
        }
      }
    }

    /* Drain all pending ETH RX frames. Called unconditionally: the internal
       do-while exits immediately when nothing is pending, so the cost is one
       cheap HAL_ETH_ReadData call that returns NULL.  Avoids a race where
       EthRxPending was already cleared when the last ISR fired but a frame
       is still waiting in the DMA descriptor ring. */
    ethernetif_input(&gnetif);

    /* Poll PHY link state and lwIP's own timers (ARP, DHCP, TCP, ...) -
       NO_SYS=1 means nothing else is going to do this for us. */
    ethernetif_poll_link(&gnetif);
    sys_check_timeouts();

    /* DHCP fallback: in DHCP mode, if the link is up but no DHCP server has
       answered within DHCP_FALLBACK_MS, take the configured static IP ("IP
       по умолчанию", see device_config_fallback_addr()) so a PC can still
       reach the web page - e.g. module cabled straight to a PC, no router.
       While on the fallback address DHCP keeps trying: dhcp_start() is
       re-armed every DHCP_RETRY_MS (the client's own backoff alone was seen
       to never bind again), and a lease simply replaces the fallback
       address (dhcp_bind() -> netif_set_addr()).
       Reboot escalation: on this board DHCP sometimes can't succeed after a
       cold power-on until the MCU is rebooted (see the "no IP" watchdog
       below and docs/PROJECT_GUIDE.md) - seen again here: stuck on the
       default IP for 10+ minutes behind a working router, one soft reset
       and the lease came at once. So if there is still no lease after
       DHCP_FALLBACK_REBOOT_MS on the default IP and nobody is using that
       address (no web page / port 7000 request since the fallback, or none
       for CONFIG_IDLE_MS), reboot. With a PC cabled straight to the module
       the page is reachable in each window and opening it stops the reboots. */
    #define DHCP_FALLBACK_MS        10000U
    #define DHCP_RETRY_MS           30000U
    #define DHCP_FALLBACK_REBOOT_MS 60000U
    #define CONFIG_IDLE_MS          300000U
    {
      static uint8_t  dhcp_waiting = 0U;
      static uint32_t dhcp_wait_since = 0U;
      static uint32_t dhcp_last_retry = 0U;
      static uint8_t  on_fallback = 0U;
      static uint32_t fallback_since = 0U;

      if (device_config_get()->use_static_ip != 0U)
      {
        dhcp_waiting = 0U;
        on_fallback = 0U;
      }
      else if (!netif_is_link_up(&gnetif))
      {
        /* on_fallback deliberately kept: an ETH reset drops the link for a
           moment, and restarting the reboot timer on every one of those
           kept the module on the default IP for 5 minutes. */
        dhcp_waiting = 0U;
      }
      else if (ip4_addr_isany_val(*netif_ip4_addr(&gnetif)))
      {
        if (dhcp_waiting == 0U)
        {
          dhcp_waiting = 1U;
          dhcp_wait_since = HAL_GetTick();
        }
        else if ((HAL_GetTick() - dhcp_wait_since) >= DHCP_FALLBACK_MS)
        {
          uint8_t a[4], m[4], g[4];
          ip4_addr_t ipaddr, netmask, gw;

          device_config_fallback_addr(a, m, g);
          IP4_ADDR(&ipaddr,  a[0], a[1], a[2], a[3]);
          IP4_ADDR(&netmask, m[0], m[1], m[2], m[3]);
          IP4_ADDR(&gw,      g[0], g[1], g[2], g[3]);
          netif_set_addr(&gnetif, &ipaddr, &netmask, &gw);
          Debug_Print("[lwip] no DHCP answer, using default IP\r\n");
          dhcp_waiting = 0U;
          dhcp_last_retry = HAL_GetTick();
          on_fallback = 1U;
          fallback_since = HAL_GetTick();
        }
      }
      else
      {
        dhcp_waiting = 0U;
        if (dhcp_supplied_address(&gnetif))
        {
          on_fallback = 0U;
        }
        else if (netif_is_up(&gnetif) && ((HAL_GetTick() - dhcp_last_retry) >= DHCP_RETRY_MS))
        {
          dhcp_last_retry = HAL_GetTick();
          dhcp_start(&gnetif);
          Debug_Print("[lwip] on default IP, retrying DHCP\r\n");
        }

        if ((on_fallback != 0U) && ((HAL_GetTick() - fallback_since) >= DHCP_FALLBACK_REBOOT_MS))
        {
          const uint32_t acc = device_config_last_access();
          const uint8_t  in_use = (uint8_t)((acc != 0U) && ((int32_t)(acc - fallback_since) >= 0) &&
                                  ((HAL_GetTick() - acc) < CONFIG_IDLE_MS));
          if (in_use == 0U)
          {
            Debug_Print("[lwip] still no DHCP lease on default IP, rebooting MCU\r\n");
            HAL_Delay(50); /* let the UART finish transmitting before reset */
            NVIC_SystemReset();
          }
        }
      }
    }

    /* Only talk to the server once we actually have an IP (DHCP-assigned). */
    {
      uint8_t has_ip = (netif_is_up(&gnetif) && !ip4_addr_isany_val(*netif_ip4_addr(&gnetif))) ? 1U : 0U;
      /* Where the IP came from, for the DHCP: row: 'S' static mode,
         'D' DHCP lease, 'F' default IP after DHCP timeout, '-' none yet. */
      char ip_src = (device_config_get()->use_static_ip != 0U) ? 'S' :
                    dhcp_supplied_address(&gnetif) ? 'D' : has_ip ? 'F' : '-';

      /* Feed the TFT status screen the same state this block already needs
         anyway - see tft_app.h for the ip_str/link_up/tcp_state contract.
         TFT_App_AlivePoll() rate-limits itself to 1 Hz internally, so
         calling it every loop iteration here is cheap. */
      TFT_App_AlivePoll(has_ip ? ip4addr_ntoa(netif_ip4_addr(&gnetif)) : "---",
                        netif_is_link_up(&gnetif) ? 1U : 0U,
                        ip_src,
                        tcp_echo_client_state_char());

      /* On the default IP there is no route to the TCP server anyway, and the
         client's failed connects would only trigger ETH resets. */
      if (has_ip && (ip_src != 'F'))
      {
        tcp_echo_client_poll();

        /* Show whatever the server just sent back on the LAST MSG: row for
           2 s (TFT_App_ShowReceived() handles the timing/blanking itself,
           non-blocking) - only when tcp_echo_client_poll() above actually
           drained something new, so an unrelated call here can't restart
           the 2 s timer on stale text. */
        {
          char rx_snapshot[220];  /* match tcp_echo_client.c's LAST_RX_SNAPSHOT_SIZE */
          if (tcp_echo_client_take_last_rx(rx_snapshot, sizeof(rx_snapshot)) > 0U)
          {
            TFT_App_ShowReceived(rx_snapshot);
          }
        }
      }

      /* Watchdog: DHCP lease loss/renewal clears the netif's IP
         (dhcp_release_and_stop() -> netif_set_addr(ANY)) until a fresh
         lease completes - confirmed in lwIP source (dhcp.c). While the IP
         is missing, tcp_echo_client_poll() above is skipped entirely, so
         the existing "3 failed TCP connects -> ethernetif_reset()" recovery
         path never gets a chance to run either - there is otherwise no
         self-healing at all for a stuck DHCP renewal. Observed on real
         hardware: IP missing for an entire hour with no recovery (see
         PROJECT_GUIDE.md). DHCP on a healthy LAN completes in well under a
         second, so anything stuck this long needs the same hardware-level
         kick used for wedged TCP. Lowered from 90s: that long a wait only
         delayed the inevitable - logs show ethernetif_reset() alone often
         doesn't get DHCP working again even though link/hardware recover
         fine, while a full MCU reboot reliably does within ~5s (see
         PROJECT_GUIDE.md) - so waiting it out in place wastes minutes for
         nothing. Shorter timeout = fewer wasted seconds before falling
         through to the escalation reboot below. */
      #define DHCP_STUCK_TIMEOUT_MS 15000U
      /* When ETH hardware itself is known broken (g_eth_hw_failed, set by
         low_level_init() on a HAL_ETH_Init() failure and cleared on success
         - see ethernetif.c/.h), there is nothing to "wait out":
         eth_check_refclk_pa1() already gave a definitive answer in seconds.
         Retry almost immediately instead of waiting the full
         DHCP_STUCK_TIMEOUT_MS (that long timeout exists for a genuinely
         different case - ETH hardware fine, DHCP server just
         slow/unresponsive).
         NOTE: g_eth_debug_marker==100 was used here originally, but that is
         WRONG - MX_LWIP_Init() unconditionally overwrites
         g_eth_debug_marker right after the very first low_level_init()
         call (via netif_add()), clobbering the 100 a cold-boot failure had
         just set, so that condition never actually fired on real hardware
         (confirmed: heartbeat kept showing LINK_DOWN instead of INIT_FAIL,
         and the fast retry never kicked in - the 90s DHCP_STUCK_TIMEOUT_MS
         ran every time instead). g_eth_hw_failed is written nowhere else,
         so it can't be clobbered the same way. See PROJECT_GUIDE.md. */
      #define ETH_INIT_FAIL_RETRY_MS 2000U
      /* Escalation: ethernetif_reset() alone (nRST pulse + MDIO, board
         power left on) sometimes cannot get DHCP working again even once
         the hardware/link itself recover fine - confirmed on real hardware
         that only a full MCU reboot (through main(), where nRST is now
         asserted as the very first thing - see above) reliably recovers it
         in that case. If the watchdog has had to force a reset this many
         times in a row with no successful IP in between, stop retrying in
         place and reboot the whole MCU instead, so the next boot gets the
         benefit of that early nRST assert - same effect as the manual
         "ground nRST during a restart" fix, with no human needed. Lowered
         from 3 to 2: logs show soft resets don't reliably fix this specific
         failure mode anyway, so a 3rd in-place retry is just wasted time
         that a reboot already proven to work could have used instead. See
         PROJECT_GUIDE.md. */
      #define ETH_RESET_ESCALATE_AFTER 2U
      {
        static uint32_t ip_missing_since = 0;
        static uint32_t reset_escalation_count = 0;

        if (has_ip)
        {
          ip_missing_since = 0;
          reset_escalation_count = 0;
        }
        else if (ip_missing_since == 0)
        {
          ip_missing_since = HAL_GetTick();
        }
        else if ((HAL_GetTick() - ip_missing_since) >=
                 (g_eth_hw_failed ? ETH_INIT_FAIL_RETRY_MS : DHCP_STUCK_TIMEOUT_MS))
        {
          reset_escalation_count++;
          if (reset_escalation_count >= ETH_RESET_ESCALATE_AFTER)
          {
            Debug_Print("[lwip] ETH reset escalation exhausted, rebooting MCU\r\n");
            HAL_Delay(50); /* let the UART finish transmitting before reset */
            NVIC_SystemReset();
          }
          Debug_Print("[lwip] no IP for too long, forcing ETH reset\r\n");
          ethernetif_reset(&gnetif);
          ip_missing_since = 0;
        }
      }
    }

    /* Periodic heartbeat so UART output is visible even if the terminal was
       connected after boot.  Prints every 5 s regardless of ETH state. */
    {
      static uint32_t last_hb = 0;
      if ((HAL_GetTick() - last_hb) >= 5000U)
      {
        char hb[160];
        last_hb = HAL_GetTick();
        snprintf(hb, sizeof(hb), "[alive] t=%ums eth=%s phy=%d txbuf=%lu rxalloc=%u tcp=%c heap=%u/%u sndq=%u\r\n",
                 (unsigned)HAL_GetTick(),
                 g_eth_hw_failed            ? "INIT_FAIL" :
                 netif_is_link_up(&gnetif)    ? "LINK_UP"   : "LINK_DOWN",
                 (int)g_eth_last_phy_link_state,
                 (unsigned long)EthHandle.TxDescList.BuffersInUse,
                 (unsigned)g_eth_rx_alloc_status,
                 tcp_echo_client_state_char(),
                 (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.avail,
                 (unsigned)tcp_echo_client_sndqueuelen());
        Debug_Print(hb);
      }
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 15;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /* MCO1 (PA8) = HSE/1 = 25 MHz: backup reference for LAN8720 XTAL1/CLKIN.
     The onboard 25 MHz crystal (XTAL2, near LAN8720) drives U2 pad 5 via the
     XTAL.1 net.  If that crystal is absent or defective, LAN8720 has no PLL
     reference and NINT/REFCLKO stays silent → DMABMR.SWR never clears.
     HAL_RCC_MCOConfig configures PA8 GPIO automatically (AF0 push-pull VH).
     HARDWARE (only needed if crystal is bad/absent):
       connect a short wire from PA8 (STM32 pin 67) to XTAL2 crystal pad 1
       (the pad on the XTAL.1 net = LAN8720 XTAL1/CLKIN side, NOT the XTAL2 side). */
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 921600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 921600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, LED3_Pin|LED4_Pin|LED5_Pin|LED6_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LED7_Pin|LED8_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LED3_Pin LED4_Pin LED5_Pin LED6_Pin */
  GPIO_InitStruct.Pin = LED3_Pin|LED4_Pin|LED5_Pin|LED6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pins : LED7_Pin LED8_Pin */
  GPIO_InitStruct.Pin = LED7_Pin|LED8_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* TFT control lines (CS/DC/RST/BL) are set up by TFT_App_GPIO_Init(),
     called right after this from main() - kept out of here so the whole
     display module stays self-contained under Display/ (see tft_app.c). */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/**
  * @brief  Sequential LED chase, 200 ms per LED - same visual effect as
  *         before, but non-blocking (HAL_Delay() would stall the whole main
  *         loop, including Ethernet RX/TX and lwIP timers, for ~1.2 s per
  *         call). Safe to call every main-loop iteration.
  */
void Test_Blink_LEDs(void)
{
  static const struct
  {
    GPIO_TypeDef *port;
    uint16_t pin;
  } leds[6] =
  {
    {GPIOD, LED3_Pin}, {GPIOD, LED4_Pin}, {GPIOD, LED5_Pin},
    {GPIOD, LED6_Pin}, {GPIOB, LED7_Pin}, {GPIOB, LED8_Pin},
  };
  static uint8_t current = 0;
  static uint32_t next_tick = 0;
  static uint8_t started = 0;

  if (!started)
  {
    HAL_GPIO_WritePin(leds[current].port, leds[current].pin, GPIO_PIN_SET);
    next_tick = HAL_GetTick() + 200U;
    started = 1;
    return;
  }

  if ((int32_t)(HAL_GetTick() - next_tick) < 0)
  {
    return;
  }

  HAL_GPIO_WritePin(leds[current].port, leds[current].pin, GPIO_PIN_RESET);
  current = (uint8_t)((current + 1U) % 6U);
  HAL_GPIO_WritePin(leds[current].port, leds[current].pin, GPIO_PIN_SET);
  next_tick = HAL_GetTick() + 200U;
}

/**
  * @brief  Bring up lwIP and the Ethernet netif - static IP or DHCP,
  *         per device_config_get()->use_static_ip (device_config_load()
  *         must have already run - see main(), USER CODE 2). Switching
  *         between the two modes needs a reboot to take effect (this
  *         function only runs once, at boot) - config_server.c's "SET IP
  *         ..." commands say so in their reply.
  */
static void MX_LWIP_Init(void)
{
  const DeviceConfig *cfg = device_config_get();
  ip4_addr_t ipaddr, netmask, gw;

  g_eth_debug_marker = 50;
  lwip_init();
  g_eth_debug_marker = 51;

  if (cfg->use_static_ip != 0U)
  {
    IP4_ADDR(&ipaddr,  cfg->static_ip[0],      cfg->static_ip[1],      cfg->static_ip[2],      cfg->static_ip[3]);
    IP4_ADDR(&netmask, cfg->static_netmask[0], cfg->static_netmask[1], cfg->static_netmask[2], cfg->static_netmask[3]);
    IP4_ADDR(&gw,      cfg->static_gw[0],      cfg->static_gw[1],      cfg->static_gw[2],      cfg->static_gw[3]);
  }
  else
  {
    /* All zero: ethernetif_init()/dhcp_start() will fill the real address in. */
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);
  }

  ethernetif_set_use_dhcp((uint8_t)((cfg->use_static_ip != 0U) ? 0U : 1U));
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);
  g_eth_debug_marker = 52;
  netif_set_default(&gnetif);
  netif_set_status_callback(&gnetif, netif_status_callback);
  g_eth_debug_marker = 53;

  if (netif_is_link_up(&gnetif))
  {
    netif_set_up(&gnetif);
    if (cfg->use_static_ip != 0U)
    {
      /* Address is already fixed above - no DHCP transaction needed, and
         netif_set_up() alone is what fires netif_status_callback() so the
         IP shows up on the SETUP page / UART log same as the DHCP path. */
      Debug_Print("[lwip] link up, using static IP\r\n");
    }
    else
    {
      dhcp_start(&gnetif);
      Debug_Print("[lwip] link up, DHCP started\r\n");
    }
    g_eth_debug_marker = 54;
  }
  else
  {
    netif_set_down(&gnetif);
    Debug_Print("[lwip] link down at startup, will retry via ethernetif_poll_link()\r\n");
  }
}

/**
  * @brief  Fires whenever the netif's flags/IP change - in particular once
  *         DHCP hands us a lease. Prints the address so it shows up on the
  *         debug UART (USART1, 921600 baud) without needing a debugger.
  */
static void netif_status_callback(struct netif *netif)
{
  if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)))
  {
    char msg[64];
    snprintf(msg, sizeof(msg), "[lwip] IP: %s\r\n", ip4addr_ntoa(netif_ip4_addr(netif)));
    Debug_Print(msg);
  }
}

void Debug_Print(const char *msg)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)strlen(msg), 100);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
