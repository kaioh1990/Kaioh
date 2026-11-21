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

// Function table the app hands over for the Nearby direct link
// (layout of CvfP2PFuncs in cvf_p2p_ios.go). Kept for the native P2P
// functions; the engine does not call it yet (IOS_PORT.md M6).
typedef struct CvfP2PFuncs {
    void (*advertise)(const char *id, const char *name, const char *ver, int tcpPort, int udpPort);
    void (*advertiseStop)(void);
    void (*browse)(int on);
    int (*join)(const char *id, int localTcp, int localUdp);
    void (*close)(void);
    int (*askName)(const char *suggestion);
} CvfP2PFuncs;
static CvfP2PFuncs p2p_funcs;
static _Atomic int p2p_registered;

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

void CvfP2PRegister(void *funcs) {
    if (!funcs) return;
    pthread_mutex_lock(&mbox_mu);
    memcpy(&p2p_funcs, funcs, sizeof p2p_funcs);
    pthread_mutex_unlock(&mbox_mu);
    atomic_store(&p2p_registered, 1);
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
        int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);
        it_joy = idx >= 0 ? SDL_JoystickOpen(idx) : NULL;
        fprintf(stderr, "cvf: input test: virtual controller %d %s\n", idx, it_joy ? "open" : SDL_GetError());
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
void cvf_host_drain(void) {
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
