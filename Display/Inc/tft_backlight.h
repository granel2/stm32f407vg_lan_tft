/**
  ******************************************************************************
  * @file    tft_backlight.h
  * @brief   TFT backlight brightness: PWM on BL = PC7 (TIM3_CH2, AF2) instead
  *          of the plain on/off GPIO ST7796S_Backlight() drives.
  *
  *          TFT_Backlight_Init() takes the pin over from the GPIO setup in
  *          TFT_App_GPIO_Init(); call it once ST7796S_Init() has cleared the
  *          panel and switched the backlight on. Level in percent of
  *          *perceived* brightness: the duty cycle is level^2 / 100 %, so
  *          equal steps look equal (a linear duty makes 50 % look almost as
  *          bright as 100 %).
  ******************************************************************************
  */
#ifndef TFT_BACKLIGHT_H
#define TFT_BACKLIGHT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PWM frequency: well above visible flicker, low enough for the module's
   backlight transistor to switch cleanly. */
#define TFT_BACKLIGHT_PWM_HZ  10000U

void    TFT_Backlight_Init(uint8_t percent);
/* 0..100; 0 = off (not used by the idle dimming, which keeps >= 1). */
void    TFT_Backlight_Set(uint8_t percent);
uint8_t TFT_Backlight_Get(void);

#ifdef __cplusplus
}
#endif

#endif /* TFT_BACKLIGHT_H */
