// Hand-written replacements for Ikemen functions that depend on libraries
// the translation does not include (GOTOC_NATIVE_FUNCS in run_reach.sh).
#include "ik/ik_sim.h"

// decodeShiftJIS: Ikemen decodes non-UTF-8 strings from Shift_JIS
// (golang.org/x/text/encoding/japanese). Content is UTF-8 here; invalid
// strings are passed through unchanged.
// TODO(cvf): Shift_JIS table for legacy Japanese content.
GoString fn_decodeShiftJIS(GoString input) { return input; }

// cvfGcSafepoint: frame boundary for the collector (see core/gc.h).
// cvf_safepoint_hook: per-frame work of the platform layer on the engine
// thread (the iOS host mailbox, src/platform/ios/cvf_host.c).
void (*cvf_safepoint_hook)(bool fighting);
void fn_cvfGcSafepoint(bool fighting) {
    if (cvf_safepoint_hook) cvf_safepoint_hook(fighting);
    gc_safepoint(fighting);
}

// ---------------------------------------------------------------- Nearby direct link
// cvfP2P* and cvfNativeAskName (tools/ikemen/cvf_p2p_cvf.go): call the
// iOS app's function table (CvfP2P.m, layout of CvfP2PFuncs in
// cvf_p2p_ios.go), registered through the host API (CvfP2PRegister,
// src/platform/ios/cvf_host.c). Strings go out as malloc copies freed after
// the call, as cgo's C.CString did. Without a table every call is a no-op.
typedef struct CvfP2PFuncs {
    void (*advertise)(const char *instance, const char *name, const char *ver, int tcpPort, int udpPort);
    void (*advertiseStop)(void);
    void (*browse)(int on);
    int (*join)(const char *instance, int localTcp, int localUdp);
    void (*close)(void);
    void (*askName)(const char *suggestion);
} CvfP2PFuncs;
static CvfP2PFuncs p2p;

void cvf_p2p_set_funcs(const void *funcs) {
    if (funcs) memcpy(&p2p, funcs, sizeof p2p);
}

static char *p2p_cstr(GoString s) {
    char *c = (char *)malloc((size_t)s.len + 1);
    if (s.len) memcpy(c, s.p, (size_t)s.len);
    c[s.len] = 0;
    return c;
}

void fn_cvfP2PAdvertise(GoString id, GoString name, GoString ver, int tcpPort, int udpPort) {
    if (!p2p.advertise) return;
    char *i = p2p_cstr(id), *n = p2p_cstr(name), *v = p2p_cstr(ver);
    p2p.advertise(i, n, v, tcpPort, udpPort);
    free(i), free(n), free(v);
}
void fn_cvfP2PAdvertiseStop(void) {
    if (p2p.advertiseStop) p2p.advertiseStop();
}
void fn_cvfP2PBrowse(bool on) {
    if (p2p.browse) p2p.browse(on ? 1 : 0);
}
bool fn_cvfP2PJoin(GoString id, int localTcp, int localUdp) {
    if (!p2p.join) return false;
    char *i = p2p_cstr(id);
    int ok = p2p.join(i, localTcp, localUdp);
    free(i);
    return ok != 0;
}
void fn_cvfP2PClose(void) {
    if (p2p.close) p2p.close();
}
bool fn_cvfNativeAskName(GoString suggestion) {
    if (!p2p.askName) return false;
    char *s = p2p_cstr(suggestion);
    p2p.askName(s);
    free(s);
    return true;
}
