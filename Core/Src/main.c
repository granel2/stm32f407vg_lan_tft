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
#include "st7796s.h"
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
SPI_HandleTypeDef hspi3;
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
static void MX_SPI3_Init(void);
static void TFT_SmokeTest(void);
static void netif_status_callback(struct netif *netif);
static void Debug_Print(const char *msg);
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
  /* TFT bring-up runs before lwIP so its blocking HAL_Delay()s can't stall
     Ethernet RX/DHCP; the panel just shows a static test pattern afterwards. */
  MX_SPI3_Init();
  TFT_SmokeTest();
  MX_LWIP_Init();
  tcp_echo_client_init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    Test_Blink_LEDs();

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

    /* Only talk to the server once we actually have an IP (DHCP-assigned). */
    {
      uint8_t has_ip = (netif_is_up(&gnetif) && !ip4_addr_isany_val(*netif_ip4_addr(&gnetif))) ? 1U : 0U;

      if (has_ip)
      {
        tcp_echo_client_poll();
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
  /* TFT control lines: CS idle high, RST held high (pulsed in ST7796S_Init),
     DC don't-care, backlight off until the panel is initialised. PA15 is
     JTDI after reset - SWD-only debugging is unaffected by repurposing it. */
  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, TFT_DC_Pin|TFT_BL_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = TFT_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(TFT_CS_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TFT_DC_Pin|TFT_RST_Pin|TFT_BL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
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
  * @brief  Bring up lwIP and the Ethernet netif (DHCP, no static IP).
  */
static void MX_LWIP_Init(void)
{
  ip4_addr_t ipaddr, netmask, gw;

  g_eth_debug_marker = 50;
  lwip_init();
  g_eth_debug_marker = 51;

  /* All zero: ethernetif_init()/dhcp_start() will fill the real address in. */
  IP4_ADDR(&ipaddr, 0, 0, 0, 0);
  IP4_ADDR(&netmask, 0, 0, 0, 0);
  IP4_ADDR(&gw, 0, 0, 0, 0);

  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);
  g_eth_debug_marker = 52;
  netif_set_default(&gnetif);
  netif_set_status_callback(&gnetif, netif_status_callback);
  g_eth_debug_marker = 53;

  if (netif_is_link_up(&gnetif))
  {
    netif_set_up(&gnetif);
    dhcp_start(&gnetif);
    g_eth_debug_marker = 54;
    Debug_Print("[lwip] link up, DHCP started\r\n");
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

static void Debug_Print(const char *msg)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)strlen(msg), 100);
}

/**
  * @brief  SPI3 master for the ST7796S TFT. APB1 = 40 MHz, prescaler /4 ->
  *         10 MHz SCK: comfortably inside the controller's write timing and
  *         slow enough for RDID readback over the module's SDA-O line. Once
  *         the panel is confirmed working, /2 (20 MHz) is worth trying for
  *         faster full-screen fills. Software CS (PA15 GPIO), mode 0.
  */
static void MX_SPI3_Init(void)
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

/**
  * @brief  One-shot TFT hardware check at boot: init, read the controller ID
  *         to the debug UART, flash R/G/B full-screen, leave a test pattern.
  *         Everything here is blocking (~1.5 s) - runs before lwIP on purpose.
  */
static void TFT_SmokeTest(void)
{
  uint8_t id[4];
  char msg[96];

  ST7796S_Init(&hspi3);

  ST7796S_ReadID(id);
  snprintf(msg, sizeof(msg), "[tft] RDID4(0xD3) = %02X %02X %02X %02X (ST7796S expects xx 00 77 96)\r\n",
           id[0], id[1], id[2], id[3]);
  Debug_Print(msg);

  ST7796S_FillScreen(ST7796S_RED);
  HAL_Delay(300);
  ST7796S_FillScreen(ST7796S_GREEN);
  HAL_Delay(300);
  ST7796S_FillScreen(ST7796S_BLUE);
  HAL_Delay(300);
  ST7796S_DrawTestPattern();
  Debug_Print("[tft] test pattern drawn\r\n");
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
