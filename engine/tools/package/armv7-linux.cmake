# armv7 Linux hard-float cross build (Ubuntu's gcc-arm-linux-gnueabihf) with
# the PS Vita's code generation: Cortex-A9, NEON/VFPv3, Thumb-2, the same
# flags vitasdk's arm-vita-eabi-gcc uses by default. Static executables that
# run under qemu-arm: a 32-bit check of the runtime and the simulation
# without a Vita (VITA_PORT.md). Use with -DCVF_SDL_STUB=ON.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm)
