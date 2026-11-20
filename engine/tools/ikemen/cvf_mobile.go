//go:build cvf

package main

// Mobile glue of the native C engine. The iOS app calls these through the
// C host API (engine/src/platform/ios/cvf_host.c), always on the engine
// (main) thread: directly from UIKit touch callbacks during the event pump,
// or from the per-frame mailbox drain for values coming from other threads.

import (
	"strings"
	"unsafe"
)

// "OpenGL ES 3.x" selects the GLES renderer (iOS forces "OpenGL ES 3.0").
func cvfSelectRendererGLES(cfgVal string) (Renderer, FontRenderer, bool) {
	if strings.HasPrefix(cfgVal, "OpenGL ES") {
		return &Renderer_GLES32{}, &FontRenderer_GLES32{}, true
	}
	return nil, nil, false
}

// The GLES entry points are bound by the platform layer (gles_bind.c);
// gl.Init ignores this loader.
func eglGetProcAddress(name string) unsafe.Pointer { return nil }

// Entry of the iOS app (SDL_main): --assets DIR --screen WxH, as the Go
// app's SDL_main export.
func cvfIOSMain(args []string) {
	for i := 0; i+1 < len(args); i++ {
		switch args[i] {
		case "--assets":
			sys.baseDir = args[i+1]
		case "--screen":
			cvfParseScreen(args[i+1])
		}
	}
	realMain()
}

// True while a fight is on screen, so touches stay game controls.
func cvfHostInMatch() bool { return sys.middleOfMatch() && !sys.motif.me.active }

// Hands a tap to the current screen; false if that screen ignores taps.
func cvfHostTap(x, y float32) bool { return cvfStoreTap(x, y) }

func cvfHostSetTouchMenus(on bool) { cvfTouchMenus = on }

// Nearby peers reachable over the app's direct link (JSON list).
func cvfHostPeers(json string) { cvfNearby.setP2P(json) }

// The player's answer from the app's name box ("" if cancelled).
func cvfHostNameEntered(name string) { cvfNameAskDone(name) }
