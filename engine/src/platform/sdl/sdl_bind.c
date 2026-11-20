// go-sdl2 functions the translated game calls, on top of SDL2's C API.
// Types are the Go-layout structs generated from go-sdl2 (sdl_KeyboardEvent,
// sdl_Surface...); SDL handles (SDL_Window*, SDL_GameController*...) are
// passed through as pointers to opaque Go types.
#include "ik/ik_sim.h"

static GoIface sdl_err(void) { return go_errorf("%s", SDL_GetError()); }
static GoIface sdl_check(int rc) { return rc < 0 ? sdl_err() : (GoIface){0}; }

static GoString sdl_gostr(const char *s) {
    if (!s) return (GoString){"", 0};
    return go_cstr(gc_strdup(s));
}

// ---------------------------------------------------------------- init, timing
GoIface sdl_Init(u32 flags) { return sdl_check(SDL_Init(flags)); }
void sdl_Quit(void) { SDL_Quit(); }
u64 sdl_GetPerformanceCounter(void) { return SDL_GetPerformanceCounter(); }
u64 sdl_GetPerformanceFrequency(void) { return SDL_GetPerformanceFrequency(); }

Ret_GoString__GoIface sdl_GetClipboardText(void) {
    char *t = SDL_GetClipboardText();
    Ret_GoString__GoIface r = {sdl_gostr(t), {0}};
    SDL_free(t);
    return r;
}

Ret_int__GoIface sdl_ShowCursor(sdl_EventStateConstant toggle) {
    int rc = SDL_ShowCursor((int)toggle);
    return (Ret_int__GoIface){rc, rc < 0 ? sdl_err() : (GoIface){0}};
}

// ---------------------------------------------------------------- windows, GL
Ret_sdl_WindowPtr__GoIface sdl_CreateWindow(GoString title, i32 x, i32 y, i32 w, i32 h, sdl_WindowFlags flags) {
    SDL_Window *win = SDL_CreateWindow(go_strz(title), x, y, w, h, (Uint32)flags);
    return (Ret_sdl_WindowPtr__GoIface){(sdl_Window *)win, win ? (GoIface){0} : sdl_err()};
}
GoIface sdl_Window_Destroy(sdl_Window *w) {
    SDL_DestroyWindow((SDL_Window *)w);
    return (GoIface){0};
}
Ret_sdl_GLContext__GoIface sdl_Window_GLCreateContext(sdl_Window *w) {
    SDL_GLContext c = SDL_GL_CreateContext((SDL_Window *)w);
    return (Ret_sdl_GLContext__GoIface){(sdl_GLContext)(uintptr_t)c, c ? (GoIface){0} : sdl_err()};
}
GoIface sdl_Window_GLMakeCurrent(sdl_Window *w, sdl_GLContext c) {
    return sdl_check(SDL_GL_MakeCurrent((SDL_Window *)w, (SDL_GLContext)(uintptr_t)c));
}
#if CVF_IOS
#include "platform/ios/ios_sys.h"
#endif
void cvf_gl_check_point(const char *where);  // gles_bind.c (CVF_GL_CHECK)
void sdl_Window_GLSwap(sdl_Window *w) {
#if CVF_IOS
    cvf_ios_wait_foreground();  // never touch GL in the background (IOS_PORT.md 3.7)
#endif
    cvf_gl_check_point("before SDL_GL_SwapWindow");
    SDL_GL_SwapWindow((SDL_Window *)w);
    cvf_gl_check_point("SDL_GL_SwapWindow");
}
Ret_i32__i32 sdl_Window_GLGetDrawableSize(sdl_Window *w) {
    int x = 0, y = 0;
    SDL_GL_GetDrawableSize((SDL_Window *)w, &x, &y);
    return (Ret_i32__i32){x, y};
}
GoIface sdl_GLSetAttribute(sdl_GLattr attr, int value) { return sdl_check(SDL_GL_SetAttribute((SDL_GLattr)attr, value)); }
GoIface sdl_GLSetSwapInterval(int interval) { return sdl_check(SDL_GL_SetSwapInterval(interval)); }

Ret_int__GoIface sdl_Window_GetDisplayIndex(sdl_Window *w) {
    int i = SDL_GetWindowDisplayIndex((SDL_Window *)w);
    return (Ret_int__GoIface){i, i < 0 ? sdl_err() : (GoIface){0}};
}
Ret_i32__i32 sdl_Window_GetPosition(sdl_Window *w) {
    int x = 0, y = 0;
    SDL_GetWindowPosition((SDL_Window *)w, &x, &y);
    return (Ret_i32__i32){x, y};
}
Ret_i32__i32 sdl_Window_GetSize(sdl_Window *w) {
    int x = 0, y = 0;
    SDL_GetWindowSize((SDL_Window *)w, &x, &y);
    return (Ret_i32__i32){x, y};
}
void sdl_Window_SetBordered(sdl_Window *w, bool b) { SDL_SetWindowBordered((SDL_Window *)w, b ? SDL_TRUE : SDL_FALSE); }
GoIface sdl_Window_SetFullscreen(sdl_Window *w, u32 flags) { return sdl_check(SDL_SetWindowFullscreen((SDL_Window *)w, flags)); }
void sdl_Window_SetIcon(sdl_Window *w, sdl_Surface *icon) { SDL_SetWindowIcon((SDL_Window *)w, (SDL_Surface *)icon); }
void sdl_Window_SetPosition(sdl_Window *w, i32 x, i32 y) { SDL_SetWindowPosition((SDL_Window *)w, x, y); }
void sdl_Window_SetResizable(sdl_Window *w, bool b) { SDL_SetWindowResizable((SDL_Window *)w, b ? SDL_TRUE : SDL_FALSE); }
void sdl_Window_SetSize(sdl_Window *w, i32 x, i32 y) { SDL_SetWindowSize((SDL_Window *)w, x, y); }

Ret_sdl_DisplayMode__GoIface sdl_GetDesktopDisplayMode(int idx) {
    SDL_DisplayMode m;
    Ret_sdl_DisplayMode__GoIface r = {0};
    if (SDL_GetDesktopDisplayMode(idx, &m) < 0) {
        r.r1 = sdl_err();
        return r;
    }
    r.r0.Format = m.format;
    r.r0.W = m.w;
    r.r0.H = m.h;
    r.r0.RefreshRate = m.refresh_rate;
    r.r0.DriverData = m.driverdata;
    return r;
}

// ---------------------------------------------------------------- surfaces (same layout as SDL_Surface)
_Static_assert(sizeof(sdl_Surface) == sizeof(SDL_Surface), "go-sdl2 Surface layout");
Ret_sdl_SurfacePtr__GoIface sdl_CreateRGBSurfaceWithFormat(u32 flags, i32 w, i32 h, i32 depth, u32 format) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(flags, w, h, depth, format);
    return (Ret_sdl_SurfacePtr__GoIface){(sdl_Surface *)s, s ? (GoIface){0} : sdl_err()};
}
void sdl_Surface_Free(sdl_Surface *s) { SDL_FreeSurface((SDL_Surface *)s); }
GoIface sdl_Surface_Lock(sdl_Surface *s) { return sdl_check(SDL_LockSurface((SDL_Surface *)s)); }
void sdl_Surface_Unlock(sdl_Surface *s) { SDL_UnlockSurface((SDL_Surface *)s); }
Slice_u8 sdl_Surface_Pixels(sdl_Surface *s) {
    SDL_Surface *c = (SDL_Surface *)s;
    int n = c->pitch * c->h;
    return (Slice_u8){(u8 *)c->pixels, n, n};
}

// ---------------------------------------------------------------- events
#define BOX_EVENT(T, v) ((GoIface){GO_TID_##T, go_box(T, v)})

sdl_Event sdl_PollEvent(void) {
    SDL_Event e;
    if (!SDL_PollEvent(&e)) return (GoIface){0};
    switch (e.type) {
        case SDL_QUIT:
            return BOX_EVENT(sdl_QuitEvent, ((sdl_QuitEvent){e.quit.type, e.quit.timestamp}));
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            sdl_KeyboardEvent k = {0};
            k.Type = e.key.type;
            k.Timestamp = e.key.timestamp;
            k.WindowID = e.key.windowID;
            k.State = e.key.state;
            k.Repeat = e.key.repeat;
            k.Keysym.Scancode = e.key.keysym.scancode;
            k.Keysym.Sym = e.key.keysym.sym;
            k.Keysym.Mod = e.key.keysym.mod;
            return BOX_EVENT(sdl_KeyboardEvent, k);
        }
        case SDL_TEXTINPUT: {
            sdl_TextInputEvent t = {e.text.type, e.text.timestamp, e.text.windowID, sdl_gostr(e.text.text)};
            return BOX_EVENT(sdl_TextInputEvent, t);
        }
        case SDL_WINDOWEVENT: {
            sdl_WindowEvent w = {e.window.type, e.window.timestamp, e.window.windowID, e.window.event, e.window.data1,
                                 e.window.data2};
            return BOX_EVENT(sdl_WindowEvent, w);
        }
        case SDL_CONTROLLERAXISMOTION: {
            sdl_ControllerAxisEvent a = {e.caxis.type, e.caxis.timestamp, e.caxis.which, e.caxis.axis, e.caxis.value};
            return BOX_EVENT(sdl_ControllerAxisEvent, a);
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            sdl_ControllerButtonEvent b = {e.cbutton.type, e.cbutton.timestamp, e.cbutton.which, e.cbutton.button,
                                           e.cbutton.state};
            return BOX_EVENT(sdl_ControllerButtonEvent, b);
        }
        case SDL_JOYDEVICEADDED:
            return BOX_EVENT(sdl_JoyDeviceAddedEvent, ((sdl_JoyDeviceAddedEvent){e.jdevice.type, e.jdevice.timestamp, e.jdevice.which}));
        case SDL_JOYDEVICEREMOVED:
            return BOX_EVENT(sdl_JoyDeviceRemovedEvent,
                             ((sdl_JoyDeviceRemovedEvent){e.jdevice.type, e.jdevice.timestamp, e.jdevice.which}));
        default:
            return BOX_EVENT(sdl_CommonEvent, ((sdl_CommonEvent){e.common.type, e.common.timestamp}));
    }
}

// ---------------------------------------------------------------- game controllers
int sdl_GameControllerAddMapping(GoString m) { return SDL_GameControllerAddMapping(go_strz(m)); }
sdl_GameController *sdl_GameControllerOpen(int i) { return (sdl_GameController *)SDL_GameControllerOpen(i); }
bool sdl_GameController_Attached(sdl_GameController *c) { return SDL_GameControllerGetAttached((SDL_GameController *)c); }
void sdl_GameController_Close(sdl_GameController *c) { SDL_GameControllerClose((SDL_GameController *)c); }
bool sdl_GameController_HasRumble(sdl_GameController *c) { return SDL_GameControllerHasRumble((SDL_GameController *)c); }
sdl_Joystick *sdl_GameController_Joystick(sdl_GameController *c) {
    return (sdl_Joystick *)SDL_GameControllerGetJoystick((SDL_GameController *)c);
}
GoString sdl_GameController_Name(sdl_GameController *c) { return sdl_gostr(SDL_GameControllerName((SDL_GameController *)c)); }
int sdl_GameController_Product(sdl_GameController *c) { return SDL_GameControllerGetProduct((SDL_GameController *)c); }
int sdl_GameController_ProductVersion(sdl_GameController *c) {
    return SDL_GameControllerGetProductVersion((SDL_GameController *)c);
}
int sdl_GameController_Vendor(sdl_GameController *c) { return SDL_GameControllerGetVendor((SDL_GameController *)c); }
GoIface sdl_GameController_Rumble(sdl_GameController *c, u16 lo, u16 hi, u32 ms) {
    return sdl_check(SDL_GameControllerRumble((SDL_GameController *)c, lo, hi, ms));
}
sdl_JoystickID sdl_Joystick_InstanceID(sdl_Joystick *j) { return SDL_JoystickInstanceID((SDL_Joystick *)j); }

// ---------------------------------------------------------------- audio (queue model)
Ret_sdl_AudioDeviceID__GoIface sdl_OpenAudioDevice(GoString device, bool capture, sdl_AudioSpec *want, sdl_AudioSpec *have,
                                                    sdl_AudioDeviceAllowFlags allow) {
    SDL_AudioSpec w = {0}, h = {0};
    w.freq = want->Freq;
    w.format = (SDL_AudioFormat)want->Format;
    w.channels = want->Channels;
    w.samples = want->Samples;
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(device.len ? go_strz(device) : NULL, capture, &w, &h, (int)allow);
    if (have) {
        have->Freq = h.freq;
        have->Format = h.format;
        have->Channels = h.channels;
        have->Silence = h.silence;
        have->Samples = h.samples;
        have->Size = h.size;
    }
    return (Ret_sdl_AudioDeviceID__GoIface){id, id ? (GoIface){0} : sdl_err()};
}
void sdl_CloseAudioDevice(sdl_AudioDeviceID d) { SDL_CloseAudioDevice(d); }
void sdl_PauseAudioDevice(sdl_AudioDeviceID d, bool pause) { SDL_PauseAudioDevice(d, pause); }
GoIface sdl_QueueAudio(sdl_AudioDeviceID d, Slice_u8 data) { return sdl_check(SDL_QueueAudio(d, data.p, (Uint32)data.len)); }
u32 sdl_GetQueuedAudioSize(sdl_AudioDeviceID d) { return SDL_GetQueuedAudioSize(d); }
void sdl_ClearQueuedAudio(sdl_AudioDeviceID d) { SDL_ClearQueuedAudio(d); }
