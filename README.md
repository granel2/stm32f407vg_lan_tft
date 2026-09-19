# stm32f407vg_lan_tft

STM32F407VGT6 + LAN8720 (Ethernet, lwIP, DHCP, TCP-клиент) + 3.5" TFT-дисплей.
Проект основан на `stm32f407vg_lan_v1` (Ethernet-часть отлажена и работает, см. `docs/`).

## Структура

| Путь | Что это |
|---|---|
| `Core/Inc`, `Core/Src`, `Core/Startup` | Прикладной код, HAL MSP, прерывания, lwIP-порт (`ethernetif.c`), TCP-клиент |
| `Drivers/` | STM32Cube HAL F4 V1.28.3, CMSIS, драйвер PHY lan8742 |
| `Middlewares/Third_Party/LwIP` | lwIP (NO_SYS=1) |
| `CMakeLists.txt`, `CMakePresets.json`, `toolchain-arm-none-eabi.cmake`, `linker_script.ld` | Сборка |
| `stm32f407vg_lan_tft.ioc`, `.mxproject` | Проект CubeMX — **только справочно**: ETH и часть периферии добавлены руками, перегенерировать код из CubeMX нельзя |
| `hardware/` | `BOARD.md` — выжимка по плате (разъёмы, питание, выводы); схема и плата (Eagle) `f407.sch`/`f407.brd`, распечатка схемы `f407_schematic_A3.pdf`, фото дисплея |
| `docs/` | История отладки Ethernet (`PROJECT_GUIDE.md`), аппаратный фикс LAN8720 (`CHANGES_LAN8720.md`), заметки по сборке, даташиты |
| `stm32f407vg_lan_tft.code-workspace` | Workspace VS Code |

## Целевая платформа

- MCU STM32F407VGT6 (LQFP100), HSE 25 MHz, SYSCLK 160 MHz, APB1 40 MHz, APB2 80 MHz
- USART1 (PA9/PA10) и USART3 (PD8/PD9), 921600 baud — отладочный лог
- ETH RMII → LAN8720 (REFCLK на PA1), см. `docs/CHANGES_LAN8720.md` про обязательный резистор 1 кОм на страпе nINTSEL
- LED: PB4, PB5, PD0, PD1, PD4, PD7

## Сборка

Требуется `arm-none-eabi-gcc`, `cmake ≥ 3.24`, `ninja`, `openocd` в `PATH`.

```powershell
cmake --preset default          # конфигурация (build/, Ninja, toolchain-файл)
cmake --build --preset default  # сборка → build/stm32f407vg_lan_tft.{elf,bin,hex,map}
```

В VS Code: `Terminal → Run Task → CMake: Configure`, затем `CMake: Build` (Ctrl+Shift+B).

## Прошивка и отладка

- Задача VS Code `Flash STM32F407VG` (OpenOCD, ST-Link, `write_image` + `verify_image`).
  Команда `program … verify` в текущей сборке OpenOCD глючит — подробности в `docs/PROJECT_GUIDE.md`.
- Отладка: конфигурация `Cortex Debug (ST-Link)` в `launch.json` (расширение `marus25.cortex-debug`).

## TFT 3.5" ST7796S (320×480), 4-wire SPI

Драйвер: `Core/Src/st7796s.c` (SPI3, 10 MHz, RGB565). На модуле перемычки IM0–IM2 запаять по столбцу **SPI** таблицы на плате.

| Модуль (9-pin SPI) | STM32 | Разъём платы |
|---|---|---|
| GND | GND | SV4.6 |
| VCC | +5V (если на модуле есть LDO) / +3V3 | SV4.5 / SV2.1 |
| SCL | PC10 SPI3_SCK | SV4.3 |
| SDA | PC12 SPI3_MOSI | SV4.1 |
| SDA-O | PC11 SPI3_MISO | SV4.2 (нужен только для чтения ID) |
| CS | PA15 (GPIO) | SV4.4 |
| DC | PB6 | SV5.4 |
| RST | PB7 | SV5.3 |
| BL | PB8 (1 = вкл) | SV1.3 |

При старте `TFT_SmokeTest()` печатает в USART1 `[tft] RDID4(0xD3) = …` (ожидается `xx 00 77 96`), заливает экран R/G/B и рисует тест-паттерн (цветные полосы, шахматка, рамка). Если картинка «негатив» — сменить `CMD_INVON` на `CMD_INVOFF` в `ST7796S_Init()`; если красный и синий перепутаны — убрать бит `MADCTL_BGR`.

## Что дальше

- [x] Аппаратный тест дисплея (init, ID, заливка, паттерн)
- [ ] Шрифт и вывод сетевого статуса (IP, link, DHCP, TCP) на экран
