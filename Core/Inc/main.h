/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
/* Shared debug logger (USART1, 921600 baud) - defined in main.c, used from
   here and from Display/Src/tft_app.c. */
void Debug_Print(const char *msg);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED3_Pin GPIO_PIN_0
#define LED3_GPIO_Port GPIOD
#define LED4_Pin GPIO_PIN_1
#define LED4_GPIO_Port GPIOD
#define LED5_Pin GPIO_PIN_4
#define LED5_GPIO_Port GPIOD
#define LED6_Pin GPIO_PIN_7
#define LED6_GPIO_Port GPIOD
#define LED7_Pin GPIO_PIN_4
#define LED7_GPIO_Port GPIOB
#define LED8_Pin GPIO_PIN_5
#define LED8_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* 3.5" ST7796S TFT on SPI3 (PC10 SCK / PC11 MISO / PC12 MOSI, header SV4).
   CS is a plain GPIO on SV4; DC/RST/BL are plain GPIOs grouped on three
   adjacent SV1 pins (8/9/10) - see Display/README.md and
   Display/docs/TFT_WIRING.md. PC7/PD5/PD6 are silkscreened USART6/USART2
   TX-RX on this board (hardware/BOARD.md) but neither UART is used by
   this firmware, so they're free to repurpose as plain outputs - same
   precedent as PB8/BL. PC7 needs its GPIOC clock enabled explicitly
   (TFT_App_GPIO_Init() does this) since MX_GPIO_Init() doesn't otherwise
   turn it on before this runs. */
#define TFT_CS_Pin GPIO_PIN_15
#define TFT_CS_GPIO_Port GPIOA
#define TFT_DC_Pin GPIO_PIN_6
#define TFT_DC_GPIO_Port GPIOD
#define TFT_RST_Pin GPIO_PIN_5
#define TFT_RST_GPIO_Port GPIOD
#define TFT_BL_Pin GPIO_PIN_7
#define TFT_BL_GPIO_Port GPIOC
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
