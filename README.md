# stm32f407vg_lan_tft

STM32F407VGT6 + LAN8720 (Ethernet, lwIP, DHCP, TCP-клиент) + 3.5" TFT-дисплей.
Проект основан на `stm32f407vg_lan_v1` (Ethernet-часть отлажена и работает, см. `docs/`).

## Структура

| Путь | Что это |
|---|---|
| `Core/Inc`, `Core/Src`, `Core/Startup` | Ethernet/lwIP-часть: HAL MSP, прерывания, lwIP-порт (`ethernetif.c`), TCP-клиент, `main.c` |
| `Display/` | Весь дисплейный модуль, обособлен от Ethernet-кода — см. `Display/README.md` |
| `Config/` | Постоянные настройки (сервер/имя/статический IP) + веб-страница настройки `http://<IP модуля>/` (порт 80) + TCP-конфигуратор на порту 7000 — см. `Config/README.md` |
| `Drivers/` | STM32Cube HAL F4 V1.28.3, CMSIS, драйвер PHY lan8742 |
| `Middlewares/Third_Party/LwIP` | lwIP (NO_SYS=1) |
| `CMakeLists.txt`, `CMakePresets.json`, `toolchain-arm-none-eabi.cmake`, `linker_script.ld` | Сборка |
| `stm32f407vg_lan_tft.ioc`, `.mxproject` | Проект CubeMX — **только справочно**: ETH и часть периферии добавлены руками, перегенерировать код из CubeMX нельзя |
| `hardware/` | `BOARD.md` — выжимка по плате (разъёмы, питание, выводы); схема и плата (Eagle) `f407.sch`/`f407.brd`, распечатка схемы `f407_schematic_A3.pdf` |
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

- Задача VS Code `Flash STM32F407VG` — `scripts/flash-progress.ps1`, обёртка над той же
  последовательностью OpenOCD (`init` → `reset halt` → `mass_erase` → `write_image` →
  `verify_image` → `reset run`; ST-Link). Команда `program … verify` в текущей сборке
  OpenOCD глючит — подробности в `docs/PROJECT_GUIDE.md`, поэтому по-прежнему используется
  явная последовательность, а не `program`. Обёртка добавляет текстовую шкалу прогресса
  в терминал (OpenOCD сам не печатает прогресс во время записи, только итог) — она
  ориентируется на время (калибровка ~20 с под текущий размер прошивки), реальный вывод
  OpenOCD печатается полностью после завершения.
- Есть составная задача `Build + Flash` (сборка + прошивка одной командой,
  `.vscode/tasks.json`); запускать через `Ctrl+Shift+P` → `Tasks: Run Task`. `F5`
  (Run and Debug) тоже сначала пересобирает — см. `preLaunchTask` в `launch.json`.
  Личные горячие клавиши на конкретные задачи — дело вкуса, настраиваются в
  `keybindings.json` пользователя (VS Code не поддерживает keybindings, привязанные
  к конкретному проекту, поэтому в репозитории их нет).
- Отладка: конфигурация `Cortex Debug (ST-Link)` в `launch.json` (расширение `marus25.cortex-debug`).

## TFT 3.5" ST7796S (320×480), 4-wire SPI

Весь дисплейный код, схема подключения и фото платы вынесены в **[`Display/`](Display/)** —
отдельный, не зависящий от Ethernet-части модуль, чтобы его можно было дорабатывать
(шрифты, экран сетевого статуса) не трогая `main.c`. Подробности, распиновка и порядок
включения — в [`Display/README.md`](Display/README.md).

Коротко: SPI3 (PC10/PC11/PC12, 20 МГц) + CS/DC/RST/BL на PA15/PD6/PD5/PC7, всего 2 разъёма
(SV4 + SV1). При старте `TFT_App_SmokeTest()` заливает экран R/G/B (время кадра — в USART1) и рисует тест-паттерн, затем показывает
рабочий экран статуса сети (IP/DHCP/TCP/последнее сообщение от сервера).

## Что дальше

Актуальные чек-листы — в README каждого модуля: [`Display/README.md`](Display/README.md)
(дисплей, шрифт, многостраничный экран) и [`Config/README.md`](Config/README.md)
(TCP-конфигуратор, статический IP).
