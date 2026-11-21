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
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm)" AND NOT APPLE)
  # plain char is unsigned in the Linux/Android ARM ABIs, signed on x86-64
  # and Apple arm64, where the reference dumps were verified (ANDROID_PORT.md)
  list(APPEND CVF_DETERMINISM_FLAGS -fsigned-char)
endif()

# Headless-only build without an SDL library (CVF_SDL_STUB=ON): SDL's
# headers only (CVF_SDL2_INCLUDE_DIR), the SDL functions stubbed
# (tests/stub/sdl_stub.c); cvf_headless, cvf_tests, cvf_nearby_test, no
# game. Used for the armv7 Linux build that runs under qemu-arm
# (tools/package/armv7-linux.cmake, VITA_PORT.md).
option(CVF_SDL_STUB "headless executables only, SDL stubbed" OFF)

set(CVF_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# Hardening (no effect on the simulation's results): probe large stack
# frames so one cannot step over a stack guard into the next mapping. GCC on
# Ubuntu and Apple clang do this already; the Android NDK and others may not.
# Goroutine and iOS engine stacks also have 64 KB guards (core/sched.c).
# Off on the PS Vita: its stacks (main thread, SceFiber goroutines) have no
# guard pages to step over, so the probes (three instructions in every
# function with a frame) protect nothing there.
include(CheckCCompilerFlag)
if(VITA)
  set(CVF_STACK_CLASH_DEFAULT OFF)
else()
  set(CVF_STACK_CLASH_DEFAULT ON)
endif()
option(CVF_STACK_CLASH_PROTECTION "-fstack-clash-protection where the compiler accepts it" ${CVF_STACK_CLASH_DEFAULT})
if(NOT WIN32 AND CVF_STACK_CLASH_PROTECTION)  # Windows probes every frame > 4 KB (___chkstk_ms) anyway
  check_c_compiler_flag(-fstack-clash-protection CVF_HAVE_STACK_CLASH_PROTECTION)
  if(CVF_HAVE_STACK_CLASH_PROTECTION)
    add_compile_options(-fstack-clash-protection)
  endif()
endif()

# GC stress mode (docs/DEVSETUP.md): CVF_GC_STRESS=N at run time collects on
# every Nth allocation and poisons freed memory. Off in normal builds.
option(CVF_GC_STRESS "Build the collector with the CVF_GC_STRESS run-time stress mode" OFF)

file(GLOB CVF_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.c
  src/formats/*.c)
add_library(cvf_core STATIC ${CVF_CORE_SOURCES})
target_include_directories(cvf_core PUBLIC src)
target_compile_options(cvf_core PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
if(CVF_GC_STRESS)
  target_compile_definitions(cvf_core PRIVATE CVF_GC_STRESS)
endif()
if(NOT MSVC)
  target_link_libraries(cvf_core PUBLIC m)
endif()
if(NOT WIN32 AND NOT VITA)
  # the collector's marker threads (core/gc.c)
  find_package(Threads REQUIRED)
  target_link_libraries(cvf_core PUBLIC Threads::Threads)
endif()

# iOS / iPadOS (CMAKE_SYSTEM_NAME=iOS): only the engine libraries, linked
# into CvfEngine.xcframework by tools/package/ios_build.sh (IOS_PORT.md).
# The app links SDL2.xcframework itself; CVF_SDL2_INCLUDE_DIR (the
# framework's Headers directory) is all the build needs.
if(IOS)
  if(NOT CVF_SDL2_INCLUDE_DIR)
    message(FATAL_ERROR "iOS: set CVF_SDL2_INCLUDE_DIR to SDL2.framework/Headers")
  endif()
  # 1 GB heap reservation: iOS gives an app far less address space (3.5)
  target_compile_definitions(cvf_core PRIVATE CVF_GC_DEFAULT_ARENA_MB=1024)
  enable_language(OBJC)
  file(GLOB CVF_IK_SOURCES CONFIGURE_DEPENDS src/ik/*.c src/platform/sdl/*.c
    src/platform/gles/*.c src/platform/ios/*.c src/platform/ios/*.m)
  set_source_files_properties(src/platform/ios/ios_view.m src/platform/ios/http_nsurl.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  add_library(cvf_ik STATIC ${CVF_IK_SOURCES})
  target_link_libraries(cvf_ik PUBLIC cvf_core)
  target_include_directories(cvf_ik PUBLIC "${CVF_SDL2_INCLUDE_DIR}")
  # framework headers include each other as <SDL2/...>: search the folder
  # that holds SDL2.framework too
  get_filename_component(CVF_SDL2_FW_DIR "${CVF_SDL2_INCLUDE_DIR}/../.." ABSOLUTE)
  target_compile_options(cvf_ik PUBLIC "-F${CVF_SDL2_FW_DIR}")
  target_compile_definitions(cvf_ik PRIVATE CVF_IOS=1)
  target_compile_options(cvf_ik PRIVATE ${CVF_DETERMINISM_FLAGS} -w)
  set_target_properties(cvf_ik PROPERTIES C_EXTENSIONS ON)
  foreach(t cvf_core cvf_ik)
    # -fno-common: tentative definitions become real (hidden) ones; the
    # master object's export list cannot hide common symbols
    target_compile_options(${t} PRIVATE -fvisibility=hidden -fno-common)
  endforeach()
  return()
endif()

# PS Vita (vitasdk: -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake):
# VITA_PORT.md milestones M1 and M2 as one ELF (cvf_vita) that
# tools/package/vita_vpk.sh turns into a VPK: the game (vitaGL renderer, SDL2
# input and audio, Lua menus) and, with CVF_VITA_MODE=headless in
# cvf_env.txt, M1's unit tests and reference fight. Needs the vitasdk
# packages sdl2, vitaGL, vitaShaRK, SceShaccCgExt, libmathneon, taihen
# (VITA_PORT.md section 8).
if(VITA)
  if(NOT CVF_SDL2_INCLUDE_DIR)
    set(CVF_SDL2_INCLUDE_DIR "${VITASDK}/arm-vita-eabi/include/SDL2")
  endif()
  file(GLOB CVF_IK_SOURCES CONFIGURE_DEPENDS src/ik/*.c src/platform/sdl/*.c src/platform/gles/*.c)
  add_library(cvf_ik STATIC ${CVF_IK_SOURCES} src/platform/vita/vita_ifaddrs.c)
  target_link_libraries(cvf_ik PUBLIC cvf_core)
  target_include_directories(cvf_ik PUBLIC "${CVF_SDL2_INCLUDE_DIR}")
  # the GLES binding with its render thread (gles_thread.h, CVF_VITA_RENDER_THREAD)
  target_compile_definitions(cvf_ik PRIVATE CVF_GL_THREAD=1)
  target_compile_options(cvf_ik PRIVATE ${CVF_DETERMINISM_FLAGS} -w
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration)
  set_target_properties(cvf_ik PROPERTIES C_EXTENSIONS ON)
  file(GLOB CVF_TEST_SOURCES CONFIGURE_DEPENDS tests/*.c)
  # the runtime's own Vita imports (core/sched.c: fibers; core/gc.c: memory
  # blocks, threads), on cvf_core so they follow it on the link line
  target_link_libraries(cvf_core PUBLIC SceFiber_stub SceSysmodule_stub SceLibKernel_stub
    SceKernelThreadMgr_stub SceSysmem_stub)
  set(CVF_VITA_SYS_LIBS SceCtrl_stub ScePower_stub SceDisplay_stub SceNetCtl_stub SceNet_stub SceAppMgr_stub
    SceRtc_stub pthread m SceFiber_stub SceSysmodule_stub SceLibKernel_stub SceKernelThreadMgr_stub SceSysmem_stub)
  add_executable(cvf_vita src/platform/vita/vita_main.c src/platform/vita/vita_game.c src/platform/vita/debugScreen.c
    src/app/headless_main.c ${CVF_TEST_SOURCES})
  target_compile_options(cvf_vita PRIVATE ${CVF_DETERMINISM_FLAGS})
  set_target_properties(cvf_vita PROPERTIES C_EXTENSIONS ON)
  set_source_files_properties(src/app/headless_main.c PROPERTIES COMPILE_DEFINITIONS main=cvf_headless_main)
  set_source_files_properties(tests/test_main.c PROPERTIES COMPILE_DEFINITIONS main=cvf_tests_main)
  # the game: SDL2 (audio, controllers, touch), vitaGL with the runtime shader
  # compiler (vitaShaRK + SceShaccCg: libshacccg.suprx must be on the device)
  target_link_libraries(cvf_vita PRIVATE cvf_ik
    SDL2 vitaGL vitashark SceShaccCgExt SceShaccCg_stub mathneon taihen_stub
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub SceAppUtil_stub SceAudio_stub SceAudioIn_stub
    SceSysmodule_stub SceIofilemgr_stub SceCommonDialog_stub SceTouch_stub SceHid_stub SceMotion_stub
    ScePower_stub SceProcessmgr_stub SceKernelDmacMgr_stub SceIme_stub SceRazorCapture_stub_weak
    stdc++ ${CVF_VITA_SYS_LIBS})
  return()
endif()

# Android (NDK toolchain file, ANDROID_ABI=arm64-v8a): libmain.so, the
# engine library the app's SDLActivity loads (ANDROID_PORT.md), plus static
# test executables (headless reference fight, unit tests) that run on any
# arm64 Linux kernel or under qemu-aarch64. Built by
# tools/package/android_build.sh. CVF_SDL2_INCLUDE_DIR / CVF_SDL2_LIBRARY:
# SDL2's headers and the libSDL2.so built for the app.
if(ANDROID)
  if(NOT CVF_SDL2_INCLUDE_DIR OR NOT CVF_SDL2_LIBRARY)
    message(FATAL_ERROR "Android: set CVF_SDL2_INCLUDE_DIR and CVF_SDL2_LIBRARY (libSDL2.so)")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  # 1 GB heap reservation (as on iOS): the live set is ~30 MB, and phones
  # with little RAM can refuse a 4 GB reservation under heuristic overcommit
  target_compile_definitions(cvf_core PRIVATE CVF_GC_DEFAULT_ARENA_MB=1024)
  file(GLOB CVF_IK_SOURCES CONFIGURE_DEPENDS src/ik/*.c src/platform/sdl/*.c src/platform/gles/*.c)
  add_library(cvf_ik STATIC ${CVF_IK_SOURCES})
  target_link_libraries(cvf_ik PUBLIC cvf_core)
  target_include_directories(cvf_ik PUBLIC "${CVF_SDL2_INCLUDE_DIR}")
  target_compile_definitions(cvf_ik PUBLIC CVF_ANDROID=1)
  target_compile_options(cvf_ik PRIVATE ${CVF_DETERMINISM_FLAGS} -w)
  set_target_properties(cvf_ik PROPERTIES C_EXTENSIONS ON)
  file(GLOB CVF_ANDROID_SOURCES CONFIGURE_DEPENDS src/platform/android/*.c)
  add_library(main SHARED ${CVF_ANDROID_SOURCES})
  target_link_libraries(main PRIVATE cvf_ik "${CVF_SDL2_LIBRARY}" log android ${CMAKE_DL_LIBS} m)
  target_compile_options(main PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
  set_target_properties(main PROPERTIES C_EXTENSIONS ON)
  foreach(t cvf_core cvf_ik main)
    target_compile_options(${t} PRIVATE -fvisibility=hidden)
  endforeach()
  # only the JNI entry points and SDL_main are exported; 16 KB pages
  # (Android 15 devices) need 16 KB segment alignment
  target_link_options(main PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/exports.map"
    -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL)
  # static test executables: the same objects as libmain.so (cvf_core,
  # cvf_ik), SDL stubbed out (headless runs never call it)
  add_executable(cvf_headless src/app/headless_main.c tests/android/sdl_stub.c tests/android/tls_align.c)
  target_link_libraries(cvf_headless PRIVATE cvf_ik)
  add_executable(cvf_nearby_test tests/ik/nearby_test.c tests/android/sdl_stub.c tests/android/tls_align.c)
  target_link_libraries(cvf_nearby_test PRIVATE cvf_ik)
  file(GLOB CVF_TEST_SOURCES CONFIGURE_DEPENDS tests/*.c)
  add_executable(cvf_tests ${CVF_TEST_SOURCES} tests/android/tls_align.c)
  target_link_libraries(cvf_tests PRIVATE cvf_core)
  foreach(t cvf_headless cvf_nearby_test cvf_tests)
    target_compile_options(${t} PRIVATE ${CVF_DETERMINISM_FLAGS})
    set_target_properties(${t} PROPERTIES C_EXTENSIONS ON)
    target_link_options(${t} PRIVATE -static)
  endforeach()
  set_source_files_properties(tests/android/tls_align.c PROPERTIES COMPILE_OPTIONS "-fno-emulated-tls")
  return()
endif()

file(GLOB CVF_TEST_SOURCES CONFIGURE_DEPENDS tests/*.c)
add_executable(cvf_tests ${CVF_TEST_SOURCES})
target_link_libraries(cvf_tests PRIVATE cvf_core)
target_compile_options(cvf_tests PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})

# The same tests against a core whose scheduler uses the ucontext fallback
# (-DCVF_SCHED_UCONTEXT), the context switch of 32-bit ARM, i686 and other
# POSIX targets, so that path is tested on x86-64 / arm64 machines too.
# Linux only (glibc's swapcontext; Darwin's is deprecated, Windows uses fibers).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(cvf_core_ucontext STATIC ${CVF_CORE_SOURCES})
  target_include_directories(cvf_core_ucontext PUBLIC src)
  target_compile_definitions(cvf_core_ucontext PRIVATE CVF_SCHED_UCONTEXT)
  target_compile_options(cvf_core_ucontext PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
  if(CVF_GC_STRESS)
    target_compile_definitions(cvf_core_ucontext PRIVATE CVF_GC_STRESS)
  endif()
  target_link_libraries(cvf_core_ucontext PUBLIC m)
  add_executable(cvf_tests_ucontext ${CVF_TEST_SOURCES})
  target_link_libraries(cvf_tests_ucontext PRIVATE cvf_core_ucontext)
  target_compile_options(cvf_tests_ucontext PRIVATE ${CVF_DETERMINISM_FLAGS} ${CVF_WARNINGS})
endif()

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
if(CVF_SDL_STUB)
  if(NOT CVF_SDL2_INCLUDE_DIR)
    message(FATAL_ERROR "CVF_SDL_STUB: set CVF_SDL2_INCLUDE_DIR (SDL2 headers)")
  endif()
  add_library(cvf_sdl_stub STATIC tests/stub/sdl_stub.c)
  add_library(SDL2::SDL2 ALIAS cvf_sdl_stub)
  target_include_directories(cvf_sdl_stub PUBLIC "${CVF_SDL2_INCLUDE_DIR}")
elseif(NOT (CVF_SDL2_INCLUDE_DIR AND CVF_SDL2_LIBRARY))
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

file(GLOB CVF_IK_SOURCES CONFIGURE_DEPENDS src/ik/*.c src/platform/sdl/*.c src/platform/gles/*.c)
# (src/platform/ios is the iOS app's glue: built only in the IOS branch above)
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
add_executable(cvf_nearby_test tests/ik/nearby_test.c)
target_link_libraries(cvf_nearby_test PRIVATE cvf_ik)
target_compile_options(cvf_nearby_test PRIVATE ${CVF_DETERMINISM_FLAGS})
add_executable(cvf_headless src/app/headless_main.c)
target_link_libraries(cvf_headless PRIVATE cvf_ik)
target_compile_options(cvf_headless PRIVATE ${CVF_DETERMINISM_FLAGS})

if(CVF_SDL_STUB)
  # the stub library refers back to nothing, but cvf_ik's SDL references
  # come after it on the link line: repeat it last
  foreach(t cvf_headless cvf_nearby_test)
    target_link_libraries(${t} PRIVATE cvf_sdl_stub)
  endforeach()
  find_package(Threads REQUIRED)
  target_link_libraries(cvf_ik PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  return()
endif()

# The game (SDL2 window, OpenGL renderer, Lua menus)
add_executable(cvf src/app/game_main.c)
target_link_libraries(cvf PRIVATE cvf_ik)
if(WIN32)
  target_link_libraries(cvf_ik PUBLIC ws2_32 winhttp iphlpapi bcrypt)
  target_link_libraries(cvf_core PUBLIC winmm)  # timeBeginPeriod (sched.c)
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

# Linux: test programs for the PS Vita renderer paths (VITA_PORT.md M2) on a
# desktop GL context: cvf_vitasim (the game with cvfVitaGL set) and
# gles_thread_test (the GLES render thread against direct calls). The GLES
# binding gets its render thread (inert unless a program starts it).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(cvf_ik PRIVATE CVF_GL_THREAD=1)
  add_executable(cvf_vitasim tests/vita_sim/vita_sim_main.c)
  add_executable(gles_thread_test tests/gles/gles_thread_test.c)
  foreach(t cvf_vitasim gles_thread_test)
    target_link_libraries(${t} PRIVATE cvf_ik)
    target_compile_options(${t} PRIVATE ${CVF_DETERMINISM_FLAGS})
    set_target_properties(${t} PROPERTIES C_EXTENSIONS ON)
  endforeach()
endif()
