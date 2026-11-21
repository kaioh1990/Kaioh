//go:build cvf

package main

// Mobile glue of the native C engine. The iOS app calls these through the
// C host API (engine/src/platform/ios/cvf_host.c), always on the engine
// (main) thread: directly from UIKit touch callbacks during the event pump,
// or from the per-frame mailbox drain for values coming from other threads.
// The Android app's JNI calls (engine/src/platform/android) arrive on its UI
// thread and always go through the mailbox.

import (
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
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
// app's SDL_main export. The Android app's SDL_main passes the same
// arguments (the game folder from nativeOnSDLReady, the screen size from
// nativeCvfSetScreen).
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

// True if the current screen reads taps (the test cvfStoreTap makes before
// it stores one). Android answers its UI thread's tap calls from this,
// published once per frame, and queues the tap itself for the next frame.
func cvfHostTapListening() bool {
	return cvfTap.used && sys.frameCounter-cvfTap.polled <= 5
}

func cvfHostSetTouchMenus(on bool) { cvfTouchMenus = on }

// Nearby peers reachable over the app's direct link (JSON list).
func cvfHostPeers(json string) { cvfNearby.setP2P(json) }

// The player's answer from the app's name box ("" if cancelled).
func cvfHostNameEntered(name string) { cvfNameAskDone(name) }

// Lobby round trip through net/http (NSURLSession on iOS), for a local test
// lobby (lobby/main.go): GET /api/rooms, POST a room, GET again. Run by the
// iOS app with CVF_HTTP_TEST=<base URL> (src/platform/ios/ios_main.c).
func cvfHTTPSelfTest(base string) string {
	c := &http.Client{Timeout: 15 * time.Second}
	out := ""
	get := func() {
		resp, err := c.Get(base + "/api/rooms")
		if err != nil {
			out += "GET error: " + err.Error() + "\n"
			return
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		out += fmt.Sprintf("GET /api/rooms -> %d %s\n", resp.StatusCode, strings.TrimSpace(string(b)))
	}
	get()
	body := `{"name":"iOS Simulator","port":7500,"udp_port":7501,"version":"` + cvfOnline.buildTag + `"}`
	resp, err := c.Post(base+"/api/rooms", "application/json", strings.NewReader(body))
	if err != nil {
		out += "POST error: " + err.Error() + "\n"
	} else {
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		out += fmt.Sprintf("POST /api/rooms -> %d %s\n", resp.StatusCode, strings.TrimSpace(string(b)))
	}
	get()
	return out
}
