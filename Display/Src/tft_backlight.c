/**
  ******************************************************************************
  * @file    tft_backlight.c
  * @brief   PWM backlight on PC7 / TIM3_CH2 - see tft_backlight.h.
  *
  * Register level: the project has no HAL TIM module, and one PWM channel
  * is only a handful of registers (RM0090 chapter 18).
  ******************************************************************************
  */
#include "tft_backlight.h"
#include "main.h"

static uint32_t s_period;   /* timer ticks per PWM period (ARR + 1) */
static uint8_t  s_percent;

void TFT_Backlight_Init(uint8_t percent)
{
  GPIO_InitTypeDef gpio = {0};
  /* TIM3 sits on APB1; with an APB1 prescaler != 1 its clock is 2 x PCLK1
     (40 MHz -> 80 MHz here). */
  const uint32_t tim_clk = HAL_RCC_GetPCLK1Freq() *
                           (((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? 1U : 2U);

  __HAL_RCC_TIM3_CLK_ENABLE();
  s_period = tim_clk / TFT_BACKLIGHT_PWM_HZ;   /* 8000 at 80 MHz / 10 kHz */

  TIM3->CR1   = 0U;
  TIM3->PSC   = 0U;
  TIM3->ARR   = s_period - 1U;
  TIM3->CCR2  = 0U;
  TIM3->CCMR1 = (TIM3->CCMR1 & ~(TIM_CCMR1_OC2M | TIM_CCMR1_CC2S))
              | (6U << TIM_CCMR1_OC2M_Pos)     /* PWM mode 1: high while CNT < CCR2 */
              | TIM_CCMR1_OC2PE;               /* CCR2 preloaded - no glitch on change */
  TIM3->CCER  = (TIM3->CCER & ~TIM_CCER_CC2P) | TIM_CCER_CC2E;  /* active high = BL on */
  TIM3->EGR   = TIM_EGR_UG;
  TIM3->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;

  TFT_Backlight_Set(percent);

  /* Hand PC7 from GPIO output to the timer only now that CCR2 holds the
     wanted level, so the backlight doesn't flash off/on in between. */
  gpio.Pin       = TFT_BL_Pin;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(TFT_BL_GPIO_Port, &gpio);
}

void TFT_Backlight_Set(uint8_t percent)
{
  if (percent > 100U) { percent = 100U; }
  s_percent = percent;
  if (s_period == 0U)
  {
    return;  /* not initialised yet */
  }
  /* Perceived -> duty: square law. 100 % -> full on, 10 % -> 1 % duty. */
  TIM3->CCR2 = (s_period * percent * percent) / 10000U;
}

uint8_t TFT_Backlight_Get(void)
{
  return s_percent;
}
