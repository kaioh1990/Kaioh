// Link stubs for headless executables built without an SDL library
// (CVF_SDL_STUB: the armv7 Linux test build, the Vita headless build):
// headless runs never open a window, audio device or controller, so the
// SDL functions the engine library refers to only need to exist. Any call
// is a bug and aborts.
#include <stdio.h>
#include <stdlib.h>

static void stub(const char *fn) {
    fprintf(stderr, "sdl_stub: %s called in a headless executable\n", fn);
    abort();
}
#define STUB(fn) \
    void fn(void); \
    void fn(void) { stub(#fn); }

STUB(SDL_ClearQueuedAudio)
STUB(SDL_CloseAudioDevice)
STUB(SDL_CreateRGBSurfaceWithFormat)
STUB(SDL_CreateWindow)
STUB(SDL_DestroyWindow)
STUB(SDL_FreeSurface)
STUB(SDL_GL_CreateContext)
STUB(SDL_GL_GetDrawableSize)
STUB(SDL_GL_GetProcAddress)
STUB(SDL_GL_MakeCurrent)
STUB(SDL_GL_SetAttribute)
STUB(SDL_GL_SetSwapInterval)
STUB(SDL_GL_SwapWindow)
STUB(SDL_GameControllerAddMapping)
STUB(SDL_GameControllerClose)
STUB(SDL_GameControllerGetAttached)
STUB(SDL_GameControllerGetJoystick)
STUB(SDL_GameControllerGetProduct)
STUB(SDL_GameControllerGetProductVersion)
STUB(SDL_GameControllerGetVendor)
STUB(SDL_GameControllerHasRumble)
STUB(SDL_GameControllerName)
STUB(SDL_GameControllerOpen)
STUB(SDL_GameControllerRumble)
STUB(SDL_GetClipboardText)
STUB(SDL_GetDesktopDisplayMode)
STUB(SDL_GetError)
STUB(SDL_GetPerformanceCounter)
STUB(SDL_GetPerformanceFrequency)
STUB(SDL_GetQueuedAudioSize)
STUB(SDL_GetWindowDisplayIndex)
STUB(SDL_GetWindowPosition)
STUB(SDL_GetWindowSize)
STUB(SDL_Init)
STUB(SDL_InitSubSystem)
STUB(SDL_JoystickInstanceID)
STUB(SDL_LockSurface)
STUB(SDL_OpenAudioDevice)
STUB(SDL_PauseAudioDevice)
STUB(SDL_PollEvent)
STUB(SDL_QueueAudio)
STUB(SDL_Quit)
STUB(SDL_SetWindowBordered)
STUB(SDL_SetWindowFullscreen)
STUB(SDL_SetWindowIcon)
STUB(SDL_SetWindowPosition)
STUB(SDL_SetWindowResizable)
STUB(SDL_SetWindowSize)
STUB(SDL_ShowCursor)
STUB(SDL_ShowMessageBox)
STUB(SDL_ShowSimpleMessageBox)
STUB(SDL_UnlockSurface)
STUB(SDL_free)
