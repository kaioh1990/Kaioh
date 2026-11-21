// Host API for the iOS app (include/cvf_host.h): the C functions the Go
// engine's c-archive exported, now calling the translated engine.
//
// Thread rules (IOS_PORT.md 5.3): the collector is single-threaded, so only
// the engine (main) thread touches translated code. CvfTouchTap/CvfInMatch
// come from UIKit touch handling, which runs inside the engine's event pump
// on that thread, and call the engine directly. Everything else may come
// from any thread (the Nearby relay queue, UIKit actions): the value is
// copied with malloc into a mutex-protected mailbox, and cvf_host_drain()
// applies it on the engine thread at the next frame (cvfGcSafepoint).
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ik/ik_sim.h"
#include "platform/ios/include/cvf_host.h"
#include "platform/ios/ios_sys.h"

static pthread_mutex_t mbox_mu = PTHREAD_MUTEX_INITIALIZER;
static char *mbox_peers;       // latest Nearby peer list (latest wins)
static char *mbox_name;        // pending name-box answer
static bool mbox_name_set;
static _Atomic int touch_menus = -1;  // -1: unchanged
static _Atomic int engine_up;          // translated code may be called

// The Nearby direct link's function table goes to src/ik/ik_native.c,
// which the engine's cvfP2P* functions call through.
void cvf_p2p_set_funcs(const void *funcs);

static char *dup_or_empty(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *d = (char *)malloc(n + 1);
    if (d) {
        if (n) memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

// ---------------------------------------------------------------- engine thread
int CvfInMatch(void) {
    if (!atomic_load(&engine_up) || !cvf_ios_on_main_thread()) return 0;
    return fn_cvfHostInMatch() ? 1 : 0;
}

int CvfTouchTap(float x, float y) {
    if (!atomic_load(&engine_up) || !cvf_ios_on_main_thread()) return 0;
    return fn_cvfHostTap(x, y) ? 1 : 0;
}

// ---------------------------------------------------------------- any thread
void CvfSetTouchMenus(int on) { atomic_store(&touch_menus, on ? 1 : 0); }

// Called by CvfP2PInstall at startup (viewDidLayoutSubviews), before or
// while the engine runs; the table is copied once and only read afterwards.
void CvfP2PRegister(void *funcs) {
    if (!funcs) return;
    cvf_p2p_set_funcs(funcs);
    fprintf(stderr, "cvf: Nearby direct link registered by the app\n");
}

void CvfP2PPeers(char *json) {
    char *c = dup_or_empty(json);
    pthread_mutex_lock(&mbox_mu);
    free(mbox_peers);
    mbox_peers = c;
    pthread_mutex_unlock(&mbox_mu);
}

void CvfNameEntered(char *name) {
    char *c = dup_or_empty(name);
    pthread_mutex_lock(&mbox_mu);
    free(mbox_name);
    mbox_name = c;
    mbox_name_set = true;
    pthread_mutex_unlock(&mbox_mu);
}

// ---------------------------------------------------------------- input self-test
// CVF_INPUT_TEST=1 (save/cvf_env.txt): drives the host API the way the app's
// overlay does, for the Simulator where nobody touches the screen. A virtual
// SDL game controller, created like the overlay's (SDL_JoystickAttachVirtual,
// 6 axes, 15 buttons, 1 hat), presses A on the title menu (Arcade) until
// character select accepts a tap through CvfTouchTap (the call the overlay
// makes); a second tap on the same cell selects the character; then A until
// the fight starts, walking right and punching. CvfInMatch changes are logged.
static int it_state, it_frames, it_inmatch = -1;
static SDL_Joystick *it_joy;
static void it_button(int b, int down) {
    if (it_joy) SDL_JoystickSetVirtualButton(it_joy, b, down ? SDL_PRESSED : SDL_RELEASED);
}
static void input_test_tick(void) {
    it_frames++;
    if (!it_joy && it_frames == 30) {
        // the overlay's virtual gamepad (player 1) when it exists, else a new one
        int idx = -1;
        for (int i = 0; i < SDL_NumJoysticks() && idx < 0; i++)
            if (SDL_JoystickIsVirtual(i)) idx = i;
        int attached = idx < 0;
        if (attached) idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);
        it_joy = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
        fprintf(stderr, "cvf: input test: virtual controller %d (%s) %s\n", idx, attached ? "attached" : "the overlay's",
                it_joy ? "open" : SDL_GetError());
    }
    const float tx = 0.5f, ty = 0.45f;  // a cell of the character grid
    switch (it_state) {
    case 0:  // title: A pulses until a screen takes taps (character select)
        it_button(SDL_CONTROLLER_BUTTON_A, it_frames > 60 && it_frames % 90 < 6);
        if (it_frames > 60 && CvfTouchTap(tx, ty)) {
            it_button(SDL_CONTROLLER_BUTTON_A, 0);
            fprintf(stderr, "cvf: input test: tap accepted (character select) at frame %d\n", it_frames);
            it_state = 1, it_frames = 0;
        }
        break;
    case 1:  // second tap on the same cell selects it
        if (it_frames == 40) {
            fprintf(stderr, "cvf: input test: second tap -> %d\n", CvfTouchTap(tx, ty));
            it_state = 2, it_frames = 0;
        }
        break;
    case 2:  // A for whatever else asks, until the fight is on
        it_button(SDL_CONTROLLER_BUTTON_A, it_frames > 120 && it_frames % 90 < 6);
        if (CvfInMatch() == 1) {
            it_button(SDL_CONTROLLER_BUTTON_A, 0);
            it_state = 3, it_frames = 0;
        }
        break;
    case 3:  // fight: walk right, punch now and then
        it_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, (it_frames / 90) % 2 == 0);
        it_button(SDL_CONTROLLER_BUTTON_X, it_frames % 40 < 4);
        break;
    }
    int m = CvfInMatch();
    if (m != it_inmatch) {
        fprintf(stderr, "cvf: input test: CvfInMatch() = %d (state %d)\n", m, it_state);
        it_inmatch = m;
    }
}

// ---------------------------------------------------------------- engine side
void cvf_host_set_running(int up) { atomic_store(&engine_up, up); }

// Once per frame on the engine thread (cvfGcSafepoint hook).
void cvf_host_drain(bool fighting) {
    // iOS memory warning: collect at the next frame outside a fight
    static bool low_memory;
    if (cvf_ios_take_low_memory()) low_memory = true;
    if (low_memory && !fighting) {
        low_memory = false;
        GcStats before = gc_stats();
        gc_collect();
        fprintf(stderr, "cvf: memory warning: collected (heap %zu KB -> %zu KB live)\n", before.heap_bytes / 1024,
                gc_stats().live_bytes / 1024);
    }
    int tm = atomic_exchange(&touch_menus, -1);
    if (tm >= 0) fn_cvfHostSetTouchMenus(tm != 0);
    pthread_mutex_lock(&mbox_mu);
    char *peers = mbox_peers, *name = mbox_name;
    bool name_set = mbox_name_set;
    mbox_peers = NULL, mbox_name = NULL, mbox_name_set = false;
    pthread_mutex_unlock(&mbox_mu);
    // strings handed to the engine live in collector memory
    if (peers) {
        fn_cvfHostPeers(go_cstr(gc_strdup(peers)));
        free(peers);
    }
    if (name_set) {
        fn_cvfHostNameEntered(go_cstr(gc_strdup(name ? name : "")));
        free(name);
    }
    cvf_ios_frame_tick();
    static int input_test = -1;
    if (input_test < 0) input_test = getenv("CVF_INPUT_TEST") && atoi(getenv("CVF_INPUT_TEST")) > 0;
    if (input_test) input_test_tick();
}
