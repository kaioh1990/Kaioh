// Host API of the CvF native engine for the iOS app (CvfEngine.xcframework).
// Same C names and signatures as the exports of the Go engine's c-archive
// (libikemen.h), so the app's Objective-C does not change.
#ifndef CVF_HOST_H
#define CVF_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

// the engine is built with -fvisibility=hidden; only these are exported
#define CVF_HOST_API __attribute__((visibility("default")))

// Engine (main) thread only: UIKit touch callbacks, which run inside the
// engine's event pump.
CVF_HOST_API int CvfInMatch(void);                // 1 while a fight is on screen
CVF_HOST_API int CvfTouchTap(float x, float y);   // 0 = the current screen ignores taps

// Any thread: queued and applied at the next frame.
CVF_HOST_API void CvfSetTouchMenus(int on);
CVF_HOST_API void CvfP2PRegister(void *funcs);    // CvfP2PFuncs table (copied)
CVF_HOST_API void CvfP2PPeers(char *json);        // Nearby peers reachable directly
CVF_HOST_API void CvfNameEntered(char *name);     // answer from the native name box

// Engine entry: SDL_main(argc, argv) with --assets DIR --screen WxH.
CVF_HOST_API int SDL_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
