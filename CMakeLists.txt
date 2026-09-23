cmake_minimum_required(VERSION 3.24)
project(stm32f407vg_lan_tft C ASM)

# Target MCU configuration
set(MCU_FAMILY STM32F4)
set(MCU STM32F407VG)
set(MCU_DEFINE STM32F407xx)
set(MCU_ARCH cortex-m4)
set(MCU_FLOAT_ABI hard)
set(MCU_FPU fpv4-sp-d16)
set(HSE_VALUE 25000000)
set(SYSCLOCK_FREQ 160000000)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ASM compiler flags (must match C compiler for linker compatibility)
set(CMAKE_ASM_FLAGS "-mcpu=${MCU_ARCH} -mthumb -mfpu=${MCU_FPU} -mfloat-abi=${MCU_FLOAT_ABI} -x assembler-with-cpp")

# Build types and flags
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
  -mcpu=${MCU_ARCH}
  -mthumb
  -mfpu=${MCU_FPU}
  -mfloat-abi=${MCU_FLOAT_ABI}
  -DUSE_HAL_DRIVER
  -D${MCU_DEFINE}
  -DHSE_VALUE=${HSE_VALUE}
  -Wall
  -Wextra
  -Wshadow
  -Wconversion
  -Wdouble-promotion
  -Wpointer-arith
  -Wstrict-prototypes
  -Wno-unused-parameter
  -ffunction-sections
  -fdata-sections
)

# Linker script
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker_script.ld)

# Source files
set(PROJECT_SOURCES
  Core/Startup/startup_stm32f407xx.s
  Core/Src/main.c
  Core/Src/stm32f4xx_hal_msp.c
  Core/Src/stm32f4xx_it.c
  Core/Src/system_stm32f4xx.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c
  Drivers/BSP/Components/lan8742/lan8742.c
  Core/Src/ethernetif.c
  Core/Src/sys_arch.c
  Core/Src/tcp_echo_client.c
  Display/Src/st7796s.c
  Display/Src/tft_app.c
  Middlewares/Third_Party/LwIP/src/core/init.c
  Middlewares/Third_Party/LwIP/src/core/def.c
  Middlewares/Third_Party/LwIP/src/core/dns.c
  Middlewares/Third_Party/LwIP/src/core/inet_chksum.c
  Middlewares/Third_Party/LwIP/src/core/ip.c
  Middlewares/Third_Party/LwIP/src/core/mem.c
  Middlewares/Third_Party/LwIP/src/core/memp.c
  Middlewares/Third_Party/LwIP/src/core/netif.c
  Middlewares/Third_Party/LwIP/src/core/pbuf.c
  Middlewares/Third_Party/LwIP/src/core/raw.c
  Middlewares/Third_Party/LwIP/src/core/stats.c
  Middlewares/Third_Party/LwIP/src/core/sys.c
  Middlewares/Third_Party/LwIP/src/core/tcp.c
  Middlewares/Third_Party/LwIP/src/core/tcp_in.c
  Middlewares/Third_Party/LwIP/src/core/tcp_out.c
  Middlewares/Third_Party/LwIP/src/core/timeouts.c
  Middlewares/Third_Party/LwIP/src/core/udp.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/autoip.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/igmp.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c
  Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c
  Middlewares/Third_Party/LwIP/src/netif/ethernet.c
)

# Include directories
include_directories(
  ${CMAKE_SOURCE_DIR}/Core/Inc
  ${CMAKE_SOURCE_DIR}/Display/Inc
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc
  ${CMAKE_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
  ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
  ${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Include
  ${CMAKE_SOURCE_DIR}/Drivers/BSP/Components/lan8742
  ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/LwIP/src/include
  ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/LwIP/system
)

# Create executable
add_executable(${PROJECT_NAME}.elf ${PROJECT_SOURCES})

# Third-party/vendored code (lwIP, HAL ETH driver, lan8742 PHY driver) wasn't
# written against this project's strict warning flags - don't make it noisy.
set_source_files_properties(
  Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c
  Drivers/BSP/Components/lan8742/lan8742.c
  PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-unused-parameter"
)
file(GLOB LWIP_SOURCES_FOR_WARNINGS
  ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/LwIP/src/core/*.c
  ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/LwIP/src/core/ipv4/*.c
  ${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/LwIP/src/netif/*.c
)
set_source_files_properties(
  ${LWIP_SOURCES_FOR_WARNINGS}
  PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-unused-parameter;-Wno-double-promotion"
)

# Linker flags
target_link_options(${PROJECT_NAME}.elf PRIVATE
  -mcpu=${MCU_ARCH}
  -mthumb
  -mfpu=${MCU_FPU}
  -mfloat-abi=${MCU_FLOAT_ABI}
  -T${LINKER_SCRIPT}
  -Wl,--gc-sections
  -Wl,--print-memory-usage
  -Wl,-Map=${PROJECT_NAME}.map
  -specs=nosys.specs
)

# Generate binary and hex
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary ${PROJECT_NAME}.elf ${PROJECT_NAME}.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex ${PROJECT_NAME}.elf ${PROJECT_NAME}.hex
  COMMENT "Generate BIN and HEX files"
)

# Print FLASH/RAM usage as a progress bar on every build invocation, not
# just when the linker actually re-links (a custom target with no OUTPUT has
# no up-to-date check, so Ninja/Make always re-runs it - unlike the linker's
# own -Wl,--print-memory-usage above, which only fires when main.elf itself
# gets rebuilt and stays silent on a no-op "nothing changed" build).
# These two sizes must match the MEMORY block in linker_script.ld.
set(FLASH_SIZE_BYTES 1048576)  # 1024K
set(RAM_SIZE_BYTES   131072)   # 128K
add_custom_target(print_size ALL
  COMMAND ${CMAKE_COMMAND}
          -DSIZE_TOOL=${CMAKE_SIZE}
          -DELF_FILE=$<TARGET_FILE:${PROJECT_NAME}.elf>
          -DFLASH_SIZE=${FLASH_SIZE_BYTES}
          -DRAM_SIZE=${RAM_SIZE_BYTES}
          -P ${CMAKE_SOURCE_DIR}/cmake/print_size.cmake
  DEPENDS ${PROJECT_NAME}.elf
  COMMENT "Memory usage"
  VERBATIM
)
