# Build Status & Recovery

## ✅ Successful Build State (2026-06-16)

**Build Output**: `build/stm32f407vg_lan_v1.*` (elf, bin, hex, map)
- Flash usage: 46.4 KB / 1024 KB (4.43%)
- RAM usage: 2.6 KB / 128 KB (1.99%)

## Key Fixes Applied

### 1. MCU Define Fix (Critical)
- **File**: `CMakeLists.txt`
- **Fix**: Changed `-D${MCU}xx` → `-D${MCU_DEFINE}` where `MCU_DEFINE = STM32F407xx`
- **Reason**: Device headers require exact `-DSTM32F407xx` (not `-DSTM32F407VGxx`)
- **Impact**: Enables correct peripheral structure definitions (USART_TypeDef, etc.)

### 2. ASM Compiler Flags (Critical)
- **File**: `CMakeLists.txt`
- **Fix**: Added `set(CMAKE_ASM_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp")`
- **Reason**: ASM must match C compiler flags for linker compatibility (VFP hard-float)
- **Impact**: Prevents linker "uses VFP register arguments" errors

### 3. Linker Flags Enhancement (Critical)
- **File**: `CMakeLists.txt`
- **Fix**: Added `-mcpu -mthumb -mfpu -mfloat-abi` to `target_link_options()`
- **Reason**: Ensures output ELF is marked with correct ABI attributes
- **Impact**: Links correct multilib variant (thumb/v7e-m+fp/hard)

### 4. Linker Script Update
- **File**: `linker_script.ld`
- **Fix**: Added `end = .;` symbol after `.bss` section
- **Reason**: Required by `libc.a` for dynamic memory allocation (sbrk)
- **Impact**: Eliminates "undefined reference to `end`" error

## Quick Start After Project Reopening

```bash
cd e:\MEGAsync_R91\Mega_Git_R91\VSCode\stm32f407_lan\stm32f407vg_lan_v1

# Clean configure
cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$(Get-Location)\toolchain-arm-none-eabi.cmake"

# Build
cmake --build build --config Release

# Output files created at:
# - build/stm32f407vg_lan_v1.elf
# - build/stm32f407vg_lan_v1.bin
# - build/stm32f407vg_lan_v1.hex
```

## VSCode Tasks Available

1. **CMake: Configure** — Reconfigures CMake
2. **CMake: Build** — Builds project
3. **Flash STM32F407VG** — Programs device via ST-Link
4. **Cortex Debug (ST-Link)** — Starts debugger

All configured in `.vscode/tasks.json` and `.vscode/launch.json`

## Warnings (Safe to Ignore)

The following linker warnings are expected with `-specs=nosys.specs`:
- `_close is not implemented`
- `_lseek is not implemented`
- `_read is not implemented`
- `_write is not implemented`

These functions are not used by this bare-metal firmware. If needed later, implement stubs in `Core/Src/syscalls.c`.

## Files Modified

1. ✅ `CMakeLists.txt` — MCU define, ASM flags, linker options
2. ✅ `Core/Startup/startup_stm32f407xx.s` — Vector table + Reset_Handler
3. ✅ `linker_script.ld` — Added `end` symbol
4. ✅ `.vscode/c_cpp_properties.json` — Correct tool paths
5. ✅ `.vscode/settings.json` — Correct tool paths

## Tool Paths (Windows Machine)

- CMake: `C:\Users\svgra\AppData\Local\stm32cube\bundles\cmake\4.0.1+st.3\bin\cmake.exe`
- Ninja: `C:\ninja\ninja.exe`
- ARM GCC: `C:\Program Files (x86)\Arm\GNU Toolchain mingw-w64-i686-arm-none-eabi\bin`
- OpenOCD: `C:\OpenOCD\bin\openocd.exe`

## Next Steps

1. Test flashing via ST-Link: `cmake --build build --target flash` or manual OpenOCD
2. Test debugging with Cortex-Debug extension
3. Add application code to `Core/Src/main.c`
