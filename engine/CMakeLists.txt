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
