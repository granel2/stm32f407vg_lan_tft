# STM32F407VG LAN v1 — CMake setup

## Что сделано

- Проект переведен на `CMake` для работы в `VS Code`
- Добавлен `CMakeLists.txt` для сборки `Core/` + HAL + CMSIS
- Добавлен `toolchain-arm-none-eabi.cmake` для `arm-none-eabi`
- Добавлены задачи VS Code в `.vscode/tasks.json`
- Добавлен `launch.json` для `cortex-debug` + OpenOCD
- Добавлен `startup_stm32f407xx.s` и `linker_script.ld`
- Добавлен `.gitignore`

## Как собрать

1. Установите `arm-none-eabi-gcc`, `cmake`, `ninja`, `openocd`, `st-flash`.
2. Откройте проект в `VS Code`.
3. Выполните задачу `CMake: Configure`.
4. Выполните задачу `CMake: Build`.
5. Результат: `build/stm32f407vg_lan_v1.elf`, `build/stm32f407vg_lan_v1.bin`, `build/stm32f407vg_lan_v1.hex`.

## Как прошить

- `openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program build/stm32f407vg_lan_v1.bin verify reset exit"`

Если вы хотите, можно настроить `st-flash` вместо `openocd`, но на этой машине `openocd` уже доступен.

## Отладка

- Запустите `Cortex Debug (ST-Link)` в `VS Code`

## Замечания

- `EWARM/` можно не использовать, это только артефакт CubeMX/Keil.
- Если вы используете другой компилятор, измените `toolchain-arm-none-eabi.cmake`.
