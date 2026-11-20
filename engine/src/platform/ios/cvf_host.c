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
}
