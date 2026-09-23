# Плата f407 — выжимка из `f407.sch` / `f407.brd` (Eagle 9.6.2)

Двухслойная плата 85.4 × 46.1 мм, 2 крепёжных отверстия Ø3.2 (15.6, 6.5) и (69.6, 39.5).
Полигон только один — `+3V3` на нижнем слое; сплошного GND-полигона нет, земля разведена дорожками.
Правила: 6 mil дорожка/зазор, via 0.25 мм. Все SMD-компоненты силовой части (IC1, IC2, L1, D1, D2) — снизу; МК, PHY, CAN, разъёмы, светодиоды — сверху.

## Питание

```
J2 (+12V in) ─ D1 ─ IC1 MC34063 (buck, L1 220 µH, R14 0.27 Ω → Ipk ≈ 1.1 A, ОС R13 30k / R11 10k → 5.0 V) ─ +5V ─ IC2 AMS1117-3.3 ─ +3V3
                                                     C15 = предохранитель 500 mA на +5V
```
- `+5V` питает: AMS1117, TCAN1044 (VCC), выведен на SV3.10, SV4.5, SV5.2, J3.4.
- `+3V3` питает STM32, LAN8720 (VDD1A/VDD2A/VDDIO), TCAN VIO, магнетику J1; выведен только на SV2.1.
- **PB1 = ADC12_IN9** — делитель R33 7.5k / R34 1k с входа +12V (до D1): `Vin = Vadc × 8.5` (12 V → 1.41 V, шкала до ~28 V). Готовый монитор входного напряжения.
- Бюджет 5V-шины ограничен MC34063 (~0.5 A) и предохранителем 500 mA. Плата (МК 160 MHz + PHY + LED) ≈ 200–250 mA; подсветка 3.5" TFT ≈ 100–150 mA — укладывается, но с небольшим запасом.

## МК STM32F407VGT6 (U1, LQFP100)

HSE 25 MHz (XTAL1, C7/C8 27 pF), BOOT0 → R4 10k на GND, NRST → R5 10k. SWD на SV2. Задействованные выводы:

| Вывод | Сигнал | Куда |
|---|---|---|
| PA0, PA3 | ADC123_IN0/IN3 | SV1.1, SV1.2 (через R35/R36 1k) |
| PA1 | LAN_REFCLKO (50 MHz RMII REF_CLK) | U2 NINT/REFCLKO, R22 4.7k |
| PA2 / PC1 | LAN_MDIO / LAN_MDC | U2 |
| PA4, PA5 | DAC_OUT1/2 | SV1.5, SV1.6 |
| PA7 | LAN_CRS_DV | U2 (+R24 4.7k страп MODE2) |
| PA9 / PA10 | USART1 TX/RX | SV3.5 / SV3.4 |
| PA11 / PA12 | CAN1 RX/TX | U3 TCAN1044V → J3 |
| PA13 / PA14 / PB3 | SWDIO / SWCLK / SWO | SV2 |
| PA15, PC10, PC11, PC12 | SPI3 NSS / SCK / MISO / MOSI | SV4 |
| PB1 | ADC12_IN9 — монитор +12V | R33/R34 |
| PB4, PB5, PD0, PD1, PD4, PD7 | LED7, LED8, LED3, LED4, LED5, LED6 | 1k → LED |
| PB6 / PB7 | I2C1 SCL / SDA | SV5.4 / SV5.3 |
| PB8, PB9 | RPM1 / RPM2 (TIM4 CH3/CH4 или EXTI) | SV1.3, SV1.4 |
| PB10 | LAN_NRST | U2 NRST, R3 10k pull-up |
| PB11, PB12, PB13 | LAN TXEN / TXD0 / TXD1 | U2 |
| PC2, PC3 | TEMP1/TEMP2 — 1-Wire для DS18B20, pull-up 2.2k к +5V (FT-выводы) | SV3.9, SV3.7 |
| PC4, PC5 | LAN RXD0 / RXD1 | U2 (+R25/R26 страпы MODE0/1) |
| PC6 / PC7 | USART6 TX/RX | SV1.7 / SV1.8 |
| PD5 / PD6 | USART2 TX/RX | SV1.9 / SV1.10 |
| PD8 / PD9 | USART3 TX/RX | SV3.1 / SV3.3 |

Свободные и не выведенные на разъёмы: PA6, PA8 (MCO1, см. main.c), PB0, PB2, PB14, PB15, PC0, PC8, PC9, PC13–15, PD2, PD3, PD10–15, PE0–PE15.
DS18B20 на SV3: питание +5V (SV3.10), GND (SV3.8/6/2), данные TEMP1/TEMP2 — open-drain 1-Wire, подтяжка к +5V допустима, т.к. PC2/PC3 5V-tolerant.

## Разъёмы (положение на плате, вид сверху)

**SV2** — SWD, 2×3, левый верх (5.1, 39.4)
`1 +3V3 | 2 SWDIO | 3 GND | 4 SWCLK | 5 GND | 6 SWO`

**SV4** — SPI3, 2×3, левый край (5.1, 26.7) — **разъём дисплея**
`1 MOSI PC12 | 2 MISO PC11 | 3 SCK PC10 | 4 NSS PA15 | 5 +5V | 6 GND`

**SV5** — I2C1, 1×4, нижний край (23.5, 3.8)
`1 GND | 2 +5V | 3 SDA PB7 | 4 SCL PB6`

**SV1** — 2×5, верхний край (39.7, 40.6)
`1 PA0 | 2 PA3 | 3 PB8 | 4 PB9 | 5 PA4 | 6 PA5 | 7 PC6 | 8 PC7 | 9 PD5 | 10 PD6` (питания нет)

**SV3** — UART, 2×5, верхний край (21.9, 40.6)
`1 TXD3 | 2 GND | 3 RXD3 | 4 RXD1 | 5 TXD1 | 6 GND | 7 TEMP2 | 8 GND | 9 TEMP1 | 10 +5V`

**J1** HR911105A RJ45 с магнетикой (правый низ), LED: зелёный ← R12 1k / U2 LED2, жёлтый ← U2 LED1 / R15 1k.
**J2** +12V вход (правый верх), **J3** CAN1 4-pin (левый низ: GND, CANL, CANH, +5V), D4 PESD1CAN, R2/R30 2×49.9 Ω — split-терминатор 120 Ω впаян постоянно (узел должен быть на конце шины). В прошивке CAN пока не используется.

## LAN8720 (U2, QFN24)

Кварц XTAL2 25 MHz, RMII, REF_CLK берётся с NINT/REFCLKO (PA1). Страпы: R22 (REFCLKO) 4.7k, R24/R25/R26 (MODE2..0) 4.7k, R37 10k на LED2/nINTSEL — про необходимость дополнительного 1k на GND см. `../docs/CHANGES_LAN8720.md`. RBIAS R20 10k на GND (по даташиту должно быть 12.1k ±1 % — стоит проверить номинал), PHYAD0: R23 4.7k на GND → адрес PHY = 0. VDDCR блокирован C22 100n + C23 22µ.
CAN: TCAN1044V STB → R1 10k на GND (нормальный режим постоянно), VIO = 3V3.

## Что это значит для TFT

Подробная распиновка и код — в [`../Display/`](../Display/README.md). Коротко:

- SPI-сигналы и CS — все на одном разъёме SV4 вместе с GND и +5V.
- DC/RST/BL — три соседних пина SV1 (PD6/PD5/PC7, SV1.10/SV1.9/SV1.8). SV5 в разводке TFT не
  используется. PC7/PD5/PD6 здесь подписаны как USART6/USART2 TX-RX, но в прошивке не заняты
  (используются только USART1 и USART3).
- VCC дисплея: **+5V с SV4.5**, если на модуле есть свой стабилизатор (на фото — U8 в SOT-23 рядом с C16–C20, скорее всего 3.3V LDO); если модуль чисто 3.3-вольтовый — брать +3V3 с SV2.1. Логика ST7796S 3.3 V, уровни от STM32 подходят.
