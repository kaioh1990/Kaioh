// C declarations the translated platform code refers to by name: SDL's
// constants and enumerators (go-sdl2 defines them as C values).
#ifndef CVF_PLATFORM_BIND_H
#define CVF_PLATFORM_BIND_H
#define SDL_MAIN_HANDLED
// SDL headers configured on another architecture (e.g. an x86_64 Homebrew
// SDL used for an arm64 build) would pull in x86 intrinsics headers
#if !defined(__x86_64__) && !defined(__i386__) && !defined(SDL_DISABLE_IMMINTRIN_H)
#define SDL_DISABLE_IMMINTRIN_H
#endif
#include <SDL.h>
#endif
