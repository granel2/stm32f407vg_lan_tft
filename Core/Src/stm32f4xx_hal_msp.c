/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file         stm32f4xx_hal_msp.c
  * @brief        This file provides code for the MSP Initialization
  *               and de-Initialization codes.
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
/* USER CODE BEGIN Includes */
#include "tft_app.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN Macro */

/* USER CODE END Macro */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* External functions --------------------------------------------------------*/
/* USER CODE BEGIN ExternalFunctions */

/* USER CODE END ExternalFunctions */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */
/**
  * Initializes the Global MSP.
  */
void HAL_MspInit(void)
{

  /* USER CODE BEGIN MspInit 0 */

  /* USER CODE END MspInit 0 */

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  /* System interrupt init*/

  /* USER CODE BEGIN MspInit 1 */

  /* USER CODE END MspInit 1 */
}

/**
  * @brief UART MSP Initialization
  * This function configures the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(huart->Instance==USART1)
  {
    /* USER CODE BEGIN USART1_MspInit 0 */

    /* USER CODE END USART1_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USER CODE BEGIN USART1_MspInit 1 */

    /* USER CODE END USART1_MspInit 1 */
  }
  else if(huart->Instance==USART3)
  {
    /* USER CODE BEGIN USART3_MspInit 0 */

    /* USER CODE END USART3_MspInit 0 */
    /* Peripheral clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    __HAL_RCC_GPIOD_CLK_ENABLE();
    /**USART3 GPIO Configuration
    PD8     ------> USART3_TX
    PD9     ------> USART3_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* USER CODE BEGIN USART3_MspInit 1 */

    /* USER CODE END USART3_MspInit 1 */
  }

}

/**
  * @brief UART MSP De-Initialization
  * This function freeze the hardware resources used in this example
  * @param huart: UART handle pointer
  * @retval None
  */
void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)
{
  if(huart->Instance==USART1)
  {
    /* USER CODE BEGIN USART1_MspDeInit 0 */

    /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9|GPIO_PIN_10);

    /* USER CODE BEGIN USART1_MspDeInit 1 */

    /* USER CODE END USART1_MspDeInit 1 */
  }
  else if(huart->Instance==USART3)
  {
    /* USER CODE BEGIN USART3_MspDeInit 0 */

    /* USER CODE END USART3_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    /**USART3 GPIO Configuration
    PD8     ------> USART3_TX
    PD9     ------> USART3_RX
    */
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8|GPIO_PIN_9);

    /* USER CODE BEGIN USART3_MspDeInit 1 */

    /* USER CODE END USART3_MspDeInit 1 */
  }

}

/* USER CODE BEGIN 1 */

/**
  * @brief ETH MSP Initialization
  * This function configures the RMII pins for the on-board LAN8720 (see
  * f407.sch - matches CubeMX's default ETH/RMII pin assignment exactly) and
  * issues a clean reset pulse to the PHY via its NRST line (PB10).
  *
  * NOTE: ETH is not declared in stm32f407vg_lan_tft.ioc (it was wired up by
  * hand, see PROJECT_GUIDE.md), so this lives inside the USER CODE block on
  * purpose - CubeMX code regeneration would otherwise not know to preserve it.
  * @param heth: ETH handle pointer
  * @retval None
  */
void HAL_ETH_MspInit(ETH_HandleTypeDef *heth)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (heth->Instance == ETH)
  {
    /* Peripheral clock enable */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /**ETH GPIO Configuration
    PA1     ------> ETH_RMII_REF_CLK
    PA2     ------> ETH_RMII_MDIO
    PA7     ------> ETH_RMII_CRS_DV
    PB11     ------> ETH_RMII_TX_EN
    PB12     ------> ETH_RMII_TXD0
    PB13     ------> ETH_RMII_TXD1
    PC1     ------> ETH_RMII_MDC
    PC4     ------> ETH_RMII_RXD0
    PC5     ------> ETH_RMII_RXD1
    */
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;

    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* LAN8720 NRST (PB10). On this board it's pulled up to 3V3 through R3
       (10k) so the PHY normally comes out of reset on its own as power
       ramps up, but PB10 is wired in parallel with that resistor so
       firmware can force a clean reset pulse on every boot instead of
       depending on the supply ramp timing. */
    GPIO_InitStruct.Pin = GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* Hold nRST low for 2 s, not the datasheet-minimum ~10ms. The earlier 5s
       hold was a leftover from a now-disproven theory (PROJECT_GUIDE.md:
       "stuck REFCLKO/PLL block needing a long discharge") - the actual
       root cause of the "DMABMR.SWR timeout / no RMII REF_CLK" failures was
       a too-weak pulldown on the LED2/nINTSEL strap (pin 2) losing the race
       against the PHY's internal pull-up at nRST release, fixed in hardware
       by swapping that resistor to 1k. 2 s is kept only as a generous,
       cheap margin over the datasheet minimum - not because a long hold is
       load-bearing. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);
    HAL_Delay(2000);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);
    /* LAN8720 crystal oscillator needs ~10-20ms to stabilize after nRST.
       Without a stable 50 MHz CLK_OUT on PA1 the ETH DMA SW reset
       (DMABMR.SWR) never completes. 100 ms is safe across all variants. */
    HAL_Delay(100);

    /* Peripheral interrupt init */
    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

    /* Peripheral clock enable (HAL_ETH_Init() itself takes care of selecting
       RMII vs MII in SYSCFG->PMC based on heth->Init.MediaInterface). */
    __HAL_RCC_ETH_CLK_ENABLE();

    /* MDIO probe + PHY soft-reset.
       The RMII 50 MHz REF_CLK on PA1 must be active before DMABMR.SWR can
       clear.  We scan MDIO, read key registers, and issue a BCR soft-reset to
       restart the LAN8720's crystal oscillator / PLL → enables CLKOUT.
       CR[4:2]=0x14 = Div102 for 160 MHz HCLK (MDC ≈ 1.57 MHz). */
    {
      volatile uint32_t *macmiiar = (volatile uint32_t *)0x40028010UL;
      volatile uint32_t *macmiidr = (volatile uint32_t *)0x40028014UL;
      char probe_buf[64];
      uint32_t probe_t;
      uint32_t found_addr = 0xFFUL;

/* Helper: raw MDIO read.  Returns 0xFFFF on MB timeout (100 ms). */
#define MDIO_RD(a, r, v) do {                                            \
  *macmiiar = ((uint32_t)(a)<<11)|((uint32_t)(r)<<6)|0x14UL|0x01UL;    \
  probe_t = HAL_GetTick();                                               \
  while ((*macmiiar & 0x01U) && ((HAL_GetTick()-probe_t) < 100U));      \
  (v) = (*macmiiar & 0x01U) ? 0xFFFFUL : (*macmiidr & 0xFFFFUL);       \
} while (0)

/* Helper: raw MDIO write. */
#define MDIO_WR(a, r, v) do {                                            \
  *macmiidr = (v);                                                       \
  *macmiiar = ((uint32_t)(a)<<11)|((uint32_t)(r)<<6)|0x14UL|0x03UL;    \
  probe_t = HAL_GetTick();                                               \
  while ((*macmiiar & 0x01U) && ((HAL_GetTick()-probe_t) < 100U));      \
} while (0)

      Debug_Print("[msp] MDIO scan\r\n");

      /* Scan 0-31 for a responding PHY */
      {
        uint32_t pa;
        for (pa = 0; pa < 32U; pa++)
        {
          uint32_t id1;
          MDIO_RD(pa, 2, id1);
          if (id1 != 0xFFFFUL) { found_addr = pa; break; }
        }
      }

      if (found_addr == 0xFFUL)
      {
        Debug_Print("[msp] no PHY (all 0xFFFF)\r\n");
      }
      else
      {
        uint32_t id2, bcr, bsr, mcsr, smr;
        MDIO_RD(found_addr, 3,    id2);
        MDIO_RD(found_addr, 0,    bcr);
        MDIO_RD(found_addr, 1,    bsr);
        MDIO_RD(found_addr, 0x11, mcsr); /* MCSR: bit1=ENERGYON */
        MDIO_RD(found_addr, 0x12, smr);  /* SMR:  bits[7:5]=MODE, bits[4:0]=PHYAD */

        snprintf(probe_buf, sizeof(probe_buf),
                 "[msp] PHY@%u ID2=%04X BCR=%04X BSR=%04X MCSR=%04X SMR=%04X\r\n",
                 (unsigned)found_addr, (unsigned)id2,
                 (unsigned)bcr, (unsigned)bsr, (unsigned)mcsr, (unsigned)smr);
        Debug_Print(probe_buf);

        /* If power-down is set (BCR bit 11), clear it first */
        if (bcr & 0x0800U)
        {
          Debug_Print("[msp] PHY power-down! Waking...\r\n");
          MDIO_WR(found_addr, 0, bcr & ~0x0800U);
          HAL_Delay(50);
        }

        /* PHY software reset (BCR bit 15) via MDIO - REMOVED 2026-06-19.
           Confirmed on real hardware that this extra step was the problem,
           not a fix: even after lengthening the hardware nRST pulse above
           to 5s (which alone, done manually with tweezers, reliably
           recovered a stuck REFCLKO block - see PROJECT_GUIDE.md), 9/9
           subsequent attempts STILL failed with "no RMII REF_CLK" as long
           as this MDIO soft-reset ran right after. Hypothesis: this
           register-triggered "restart crystal osc + PLL" can itself fail
           to relock, re-breaking the exact thing the long nRST pulse had
           just fixed. The manual tweezers test that proved 5s works never
           involved this MDIO step at all - only the hardware nRST. Left
           the diagnostic register dump above (read-only) since it's
           harmless and useful; only the actual reset WRITE + its wait +
           settle delay are removed. */
      }

#undef MDIO_RD
#undef MDIO_WR
    }
  }
}

/**
  * @brief ETH MSP De-Initialization
  * @param heth: ETH handle pointer
  * @retval None
  */
void HAL_ETH_MspDeInit(ETH_HandleTypeDef *heth)
{
  if (heth->Instance == ETH)
  {
    __HAL_RCC_ETH_CLK_DISABLE();

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);

    HAL_NVIC_DisableIRQ(ETH_IRQn);
  }
}

/**
  * @brief SPI3 MSP Initialization - TFT bus on the SV4 header
  * PC10 -> SPI3_SCK, PC11 -> SPI3_MISO, PC12 -> SPI3_MOSI (AF6).
  * NSS is software-driven (PA15 as plain GPIO, set up in MX_GPIO_Init).
  * TX DMA: DMA1 Stream5 / Channel 0, half-words, memory increment OFF -
  * ST7796S_FillRect() streams one colour word, SPI in 16-bit frames.
  * SPI3_IRQn is deliberately NOT enabled: in 2-line mode the unread RX side
  * raises OVR every other frame, and with the ERR interrupt HAL turns on
  * for DMA transfers that would be an IRQ every ~1.6 us for the whole fill.
  * Completion comes from the DMA interrupt; HAL clears OVR at the end.
  * Not declared in the .ioc for the same reason as ETH above.
  */
void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (hspi->Instance == SPI3)
  {
    __HAL_RCC_SPI3_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    __HAL_RCC_DMA1_CLK_ENABLE();
    hdma_spi3_tx.Instance                 = DMA1_Stream5;
    hdma_spi3_tx.Init.Channel             = DMA_CHANNEL_0;
    hdma_spi3_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_spi3_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi3_tx.Init.MemInc              = DMA_MINC_DISABLE;
    hdma_spi3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_spi3_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_spi3_tx.Init.Mode                = DMA_NORMAL;
    hdma_spi3_tx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_spi3_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_spi3_tx) != HAL_OK)
    {
      Error_Handler();
    }
    __HAL_LINKDMA(hspi, hdmatx, hdma_spi3_tx);

    /* Below ETH (5): a late fill-complete only delays CS by a few us */
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
  }
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef* hspi)
{
  if (hspi->Instance == SPI3)
  {
    __HAL_RCC_SPI3_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12);
    HAL_DMA_DeInit(hspi->hdmatx);
    HAL_NVIC_DisableIRQ(DMA1_Stream5_IRQn);
  }
}

/* USER CODE END 1 */
