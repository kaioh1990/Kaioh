// Android platform layer of the native engine (ANDROID_PORT.md).
//
// android_sys.c: JNI entry points the app's Java calls (the names the Go
// engine's libmain.so exported), SDL_main, the engine thread, the log file.
// It includes system and JNI headers, which clash with generated names, so
// it does not include ik_sim.h; android_host.c is the side that calls the
// translated engine. They share the state below.
#ifndef CVF_ANDROID_SYS_H
#define CVF_ANDROID_SYS_H
#include <stdatomic.h>
#include <stdbool.h>

// UI thread -> engine (applied at the next frame by cvf_host_drain)
typedef struct CvfAndroidMailbox {
    _Atomic int touch_menus;  // -1: unchanged, else 0/1
    _Atomic int tap_pending;  // 1: tap_x/tap_y hold a tap
    _Atomic float tap_x, tap_y;
    // engine -> UI thread, published every frame
    _Atomic int in_match;       // CvfInMatch
    _Atomic int tap_listening;  // the current screen reads taps
    _Atomic int engine_up;      // the translated engine is running
} CvfAndroidMailbox;
extern CvfAndroidMailbox cvf_android_mbox;

// android_sys.c
const char *cvf_android_game_dir(void);  // from nativeOnSDLReady
void cvf_android_screen(int *w, int *h);  // from nativeCvfSetScreen (0 if unknown)
void cvf_android_frame_tick(void);        // once per frame: engine stack high-water mark
int cvf_android_chdir(const char *dir);

// android_host.c (runs on the engine thread)
void cvf_android_engine_main(int argc, char **argv);

#endif
