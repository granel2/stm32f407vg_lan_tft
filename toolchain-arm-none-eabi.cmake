set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Plain set(), NOT set(... CACHE STRING ... FORCE): after the first configure
# CMake stores the *resolved full path* of the compiler in the cache. With
# FORCE, every re-run of this file (i.e. every automatic regeneration after a
# CMakeLists.txt edit) put the bare name back, CMake saw "CMAKE_C_COMPILER
# changed", wiped the whole cache - CMAKE_TOOLCHAIN_FILE included - and
# reconfigured as a native Windows build that then failed to link.
set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy CACHE FILEPATH "ARM objcopy")
set(CMAKE_SIZE    arm-none-eabi-size    CACHE FILEPATH "ARM size")
set(CMAKE_AR      arm-none-eabi-ar      CACHE FILEPATH "ARM archiver")
set(CMAKE_RANLIB  arm-none-eabi-ranlib  CACHE FILEPATH "ARM ranlib")
set(CMAKE_NM      arm-none-eabi-nm      CACHE FILEPATH "ARM nm")
