cmake_minimum_required(VERSION 3.20)
project(cvf_engine C)

# CvF native engine. See docs/ARCHITECTURE.md.
#
# Targets:
#   cvf_core    - deterministic simulation and file formats (no platform code)
#   cvf_tests   - unit/reference tests for the core
#   cvf         - the game (platform layer + renderer), added per platform

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Determinism: every platform must compute bit-identical game state
# (rollback netplay between any two devices).
#   -ffp-contract=off  no fused multiply-add (arm64 compilers fuse by default)
#   -fno-fast-math     IEEE semantics, no reassociation
#   -fwrapv            signed overflow wraps, as in Go (the reference engine)
set(CVF_DETERMINISM_FLAGS -ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
  # x87 excess precision would break determinism; force SSE2 on 32-bit x86.
  list(APPEND CVF_DETERMINISM_FLAGS -msse2 -mfpmath=sse)
endif()

set(CVF_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

file(GLOB CVF_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.c
  src/formats/*.c)
add_library(cvf_core STATIC ${CVF_CORE_SOURCES})
target_include_directories(cvf_core PUBLIC src)
target_compile_options(cvf_core PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
if(NOT MSVC)
  target_link_libraries(cvf_core PUBLIC m)
endif()

file(GLOB CVF_TEST_SOURCES CONFIGURE_DEPENDS tests/*.c)
add_executable(cvf_tests ${CVF_TEST_SOURCES})
target_link_libraries(cvf_tests PRIVATE cvf_core)
target_compile_options(cvf_tests PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})

# Command-line tools (inspect/cook content, replay reference fights headless).
file(GLOB CVF_TOOL_SOURCES CONFIGURE_DEPENDS tools/cvftool.c)
if(CVF_TOOL_SOURCES)
  add_executable(cvftool ${CVF_TOOL_SOURCES})
  target_link_libraries(cvftool PRIVATE cvf_core)
  target_compile_options(cvftool PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
endif()

# Translated Ikemen simulation (tools/gen/gotoc) and the headless match runner
# used to verify it tick-for-tick against the reference state dumps.
# SDL2: a CMake package (SDL2::SDL2) or explicit CVF_SDL2_INCLUDE_DIR /
# CVF_SDL2_LIBRARY (headers directory containing SDL.h); the explicit paths
# win (e.g. an arm64 SDL next to an x86_64 Homebrew one).
if(NOT (CVF_SDL2_INCLUDE_DIR AND CVF_SDL2_LIBRARY))
  find_package(SDL2 CONFIG QUIET)
endif()
if(NOT TARGET SDL2::SDL2)
  if(NOT CVF_SDL2_INCLUDE_DIR OR NOT CVF_SDL2_LIBRARY)
    message(FATAL_ERROR "SDL2 not found: set CVF_SDL2_INCLUDE_DIR and CVF_SDL2_LIBRARY")
  endif()
  add_library(SDL2::SDL2 UNKNOWN IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES IMPORTED_LOCATION "${CVF_SDL2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CVF_SDL2_INCLUDE_DIR}")
endif()

file(GLOB CVF_IK_SOURCES CONFIGURE_DEPENDS src/ik/*.c src/platform/sdl/*.c)
add_library(cvf_ik STATIC ${CVF_IK_SOURCES})
target_link_libraries(cvf_ik PUBLIC cvf_core SDL2::SDL2)
# generated code: warnings are not actionable here
target_compile_options(cvf_ik PRIVATE ${CVF_DETERMINISM_FLAGS} -w)
# GCC 14 makes these permanent errors; the translation relies on C's
# lenient pointer conversions between equivalent generated types
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(cvf_ik PRIVATE -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion -Wno-error=implicit-function-declaration)
endif()
set_target_properties(cvf_ik PROPERTIES C_EXTENSIONS ON)  # GNU statement expressions, typeof
add_executable(cvf_headless src/app/headless_main.c)
target_link_libraries(cvf_headless PRIVATE cvf_ik)
target_compile_options(cvf_headless PRIVATE ${CVF_DETERMINISM_FLAGS})

# The game (SDL2 window, OpenGL renderer, Lua menus)
add_executable(cvf src/app/game_main.c)
target_link_libraries(cvf PRIVATE cvf_ik)
if(WIN32)
  target_link_libraries(cvf_ik PUBLIC ws2_32 winhttp iphlpapi)
else()
  target_link_libraries(cvf_ik PUBLIC ${CMAKE_DL_LIBS})
  find_package(Threads REQUIRED)
  target_link_libraries(cvf_ik PUBLIC Threads::Threads)
endif()
if(MINGW)
  # self-contained executables (no libgcc/libwinpthread DLLs to ship)
  target_link_options(cvf PRIVATE -static)
  target_link_options(cvf_headless PRIVATE -static)
endif()
target_compile_options(cvf PRIVATE ${CVF_DETERMINISM_FLAGS})
set_target_properties(cvf PROPERTIES C_EXTENSIONS ON)
