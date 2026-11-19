// Further standard-library entry points of the translated game (byte order,
// UTF-8/16, math via core/detmath, atomics, time, sync.Cond, runtime...).
#include <stdio.h>
#include <string.h>

#include "core/detmath.h"
#include "ik/ik_sim.h"

static GoString mkgo(const char *p, int n) { return go_bytes2str((Slice_u8){(u8 *)p, n, n}); }

// ---------------------------------------------------------------- encoding/binary byte orders
u16 binary_littleEndian_Uint16(GoOpaque o, Slice_u8 b) { (void)o; return (u16)(b.p[0] | b.p[1] << 8); }
void binary_littleEndian_PutUint16(GoOpaque o, Slice_u8 b, u16 v) { (void)o; b.p[0] = (u8)v; b.p[1] = (u8)(v >> 8); }
void binary_littleEndian_PutUint32(GoOpaque o, Slice_u8 b, u32 v) {
    (void)o;
    for (int i = 0; i < 4; i++) b.p[i] = (u8)(v >> (8 * i));
}
u16 binary_bigEndian_Uint16(GoOpaque o, Slice_u8 b) { (void)o; return (u16)(b.p[0] << 8 | b.p[1]); }
u32 binary_bigEndian_Uint32(GoOpaque o, Slice_u8 b) { (void)o; return (u32)b.p[0] << 24 | (u32)b.p[1] << 16 | (u32)b.p[2] << 8 | b.p[3]; }
u64 binary_bigEndian_Uint64(GoOpaque o, Slice_u8 b) {
    (void)o;
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | b.p[i];
    return v;
}
void binary_bigEndian_PutUint16(GoOpaque o, Slice_u8 b, u16 v) { (void)o; b.p[0] = (u8)(v >> 8); b.p[1] = (u8)v; }
void binary_bigEndian_PutUint32(GoOpaque o, Slice_u8 b, u32 v) {
    (void)o;
    for (int i = 0; i < 4; i++) b.p[i] = (u8)(v >> (24 - 8 * i));
}
void binary_bigEndian_PutUint64(GoOpaque o, Slice_u8 b, u64 v) {
    (void)o;
    for (int i = 0; i < 8; i++) b.p[i] = (u8)(v >> (56 - 8 * i));
}
GoString binary_bigEndian_String(GoOpaque o) { (void)o; return GOSTR("BigEndian"); }
u32 bits_Reverse32(u32 x) {
    u32 r = 0;
    for (int i = 0; i < 32; i++, x >>= 1) r = r << 1 | (x & 1);
    return r;
}

// ---------------------------------------------------------------- unicode
static const u8 *U8(GoString s) { return (const u8 *)s.p; }
Ret_i32__int utf8_DecodeRuneInString(GoString s) {
    if (!s.len) return (Ret_i32__int){0xFFFD, 0};
    i32 r;
    int w = go_decode_rune(s, 0, &r);
    return (Ret_i32__int){r, w};
}
Ret_i32__int utf8_DecodeLastRuneInString(GoString s) {
    if (!s.len) return (Ret_i32__int){0xFFFD, 0};
    int st = s.len - 1;
    while (st > 0 && st > s.len - 4 && (U8(s)[st] & 0xC0) == 0x80) st--;
    i32 r;
    int w = go_decode_rune(s, st, &r);
    if (st + w != s.len) return (Ret_i32__int){0xFFFD, 1};
    return (Ret_i32__int){r, w};
}
int utf8_EncodeRune(Slice_u8 p, i32 r) {
    GoString e = go_rune2str(r);
    memcpy(p.p, e.p, (size_t)e.len);
    return e.len;
}
bool utf16_IsSurrogate(i32 r) { return r >= 0xD800 && r < 0xE000; }
i32 utf16_DecodeRune(i32 r1, i32 r2) {
    if (r1 >= 0xD800 && r1 < 0xDC00 && r2 >= 0xDC00 && r2 < 0xE000) return ((r1 - 0xD800) << 10 | (r2 - 0xDC00)) + 0x10000;
    return 0xFFFD;
}

// ---------------------------------------------------------------- math (Go's algorithms, core/detmath)
f64 math_Sin(f64 x) { return cvf_sin(x); }
f64 math_Cos(f64 x) { return cvf_cos(x); }
f64 math_Tan(f64 x) { return cvf_tan(x); }
f64 math_Atan(f64 x) { return cvf_atan(x); }
f64 math_Exp(f64 x) { return cvf_exp(x); }
f64 math_Exp2(f64 x) { return cvf_pow(2, x); }
f64 math_Hypot(f64 p, f64 q) { return cvf_hypot(p, q); }
f64 math_Log(f64 x) { return cvf_ln(x); }
f64 math_Asin(f64 x) { return cvf_asin(x); }
f64 math_Acos(f64 x) { return cvf_acos(x); }
f64 math_Atan2(f64 y, f64 x) { return cvf_atan2(y, x); }
f64 math_Sinh(f64 x) {  // Go's math.sinh
    const f64 P0 = -0.6307673640497716991184787251e+6, P1 = -0.8991272022039509355398013511e+5,
              P2 = -0.2894211355989563807284660366e+4, P3 = -0.2630563213397497062819489e+2,
              Q0 = -0.6307673640497716991212077277e+6, Q1 = 0.1521517378790019070696485176e+5,
              Q2 = -0.173678953558233699533450911e+3;
    bool sign = false;
    if (x < 0) x = -x, sign = true;
    f64 t;
    if (x > 21) {
        t = cvf_exp(x) * 0.5;
    } else if (x > 0.5) {
        f64 ex = cvf_exp(x);
        t = (ex - 1 / ex) * 0.5;
    } else {
        f64 sq = x * x;
        t = (((P3 * sq + P2) * sq + P1) * sq + P0) * x;
        t = t / (((sq + Q2) * sq + Q1) * sq + Q0);
    }
    return sign ? -t : t;
}
f64 math_Cosh(f64 x) {  // Go's math.cosh
    if (x < 0) x = -x;
    if (x > 21) return cvf_exp(x) * 0.5;
    f64 ex = cvf_exp(x);
    return (ex + 1 / ex) * 0.5;
}
f64 math_Tanh(f64 x) {  // Go's math.tanh
    static const f64 P[3] = {-9.64399179425052238628e-1, -9.92877231001918586564e1, -1.61468768441708447952e3};
    static const f64 Q[3] = {1.12811678491632931402e2, 2.23548839060100448583e3, 4.84406305325125486048e3};
    const f64 MAXLOG = 8.8029691931113054295988e+01;
    f64 z = x < 0 ? -x : x;
    if (z > 0.5 * MAXLOG) return x < 0 ? -1 : 1;
    if (z >= 0.625) {
        f64 s = cvf_exp(2 * z);
        z = 1 - 2 / (s + 1);
        return x < 0 ? -z : z;
    }
    if (x == 0) return x;
    f64 s = x * x;
    return x + x * s * ((P[0] * s + P[1]) * s + P[2]) / (((s + Q[0]) * s + Q[1]) * s + Q[2]);
}
f64 math_Log10(f64 x) { return cvf_log2(x) * (0.693147180559945309417232121458176568 / 2.30258509299404568401799145468436421); }

// ---------------------------------------------------------------- strconv, hex, base64
static GoString fmt_base(u64 u, bool neg, int base) {
    char tmp[72];
    int i = (int)sizeof tmp;
    if (base < 2 || base > 36) base = 10;
    do {
        tmp[--i] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % (u64)base];
        u /= (u64)base;
    } while (u);
    if (neg) tmp[--i] = '-';
    return mkgo(tmp + i, (int)sizeof tmp - i);
}
GoString strconv_FormatInt(i64 v, int base) { return fmt_base(v < 0 ? (u64)0 - (u64)v : (u64)v, v < 0, base); }
GoString strconv_FormatUint(u64 v, int base) { return fmt_base(v, false, base); }
GoString hex_EncodeToString(Slice_u8 b) {
    char *p = (char *)gc_alloc_atomic((size_t)b.len * 2 + 1);
    for (int i = 0; i < b.len; i++) {
        p[2 * i] = "0123456789abcdef"[b.p[i] >> 4];
        p[2 * i + 1] = "0123456789abcdef"[b.p[i] & 15];
    }
    return (GoString){p, b.len * 2};
}
Ret_Slice_u8__GoIface base64_Encoding_DecodeString(GoOpaque *enc, GoString s) {  // StdEncoding
    (void)enc;
    Slice_u8 out = {(u8 *)gc_alloc_atomic((size_t)s.len * 3 / 4 + 3), 0, 0};
    u32 acc = 0;
    int bits = 0;
    for (int i = 0; i < s.len; i++) {
        char c = s.p[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (c == '=') break;
        if (v < 0) return (Ret_Slice_u8__GoIface){out, go_errorf("illegal base64 data at input byte %d", i)};
        acc = acc << 6 | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.p[out.len++] = (u8)(acc >> bits);
        }
    }
    out.cap = out.len;
    return (Ret_Slice_u8__GoIface){out, {0}};
}

// ---------------------------------------------------------------- paths
GoString path_Base(GoString p) {
    while (p.len > 1 && p.p[p.len - 1] == '/') p.len--;
    int i = p.len;
    while (i > 0 && p.p[i - 1] != '/') i--;
    if (!p.len) return GOSTR(".");
    return (GoString){p.p + i, p.len - i};
}
GoString filepath_FromSlash(GoString p) { return p; }  // '/' is the separator on every supported platform's C library
Slice_GoString filepath_SplitList(GoString p) {
    Slice_GoString out = {0};
    if (!p.len) return out;
    int st = 0;
    for (int i = 0; i <= p.len; i++)
        if (i == p.len || p.p[i] == ':') {
            out = go_append_Slice_GoString(out, (GoString){p.p + st, i - st});
            st = i + 1;
        }
    return out;
}

// ---------------------------------------------------------------- atomics (single OS thread)
bool atomic_Bool_Load(GoOpaque *b) { return *(u32 *)b != 0; }
u64 atomic_Uint64_Load(GoOpaque *u) { return *(u64 *)u; }
void atomic_Uint64_Store(GoOpaque *u, u64 v) { *(u64 *)u = v; }

// ---------------------------------------------------------------- sync.Cond (cooperative goroutines)
typedef struct RtCond {
    u64 gen;
} RtCond;
GoOpaque *sync_NewCond(GoIface l) {
    (void)l;
    return (GoOpaque *)gc_alloc(sizeof(GoOpaque));
}
void sync_Cond_Broadcast(GoOpaque *c) { ((RtCond *)c)->gen++; }
void sync_Cond_Wait(GoOpaque *c) {  // the Locker is a no-op on one thread
    u64 g = ((RtCond *)c)->gen;
    while (((RtCond *)c)->gen == g) go_sleep_ns(1000000);
}

// ---------------------------------------------------------------- time
f64 time_Duration_Seconds(i64 d) { return (f64)(d / 1000000000) + (f64)(d % 1000000000) / 1e9; }
typedef struct AfterEnv {
    GoChan *c;
    i64 d;
} AfterEnv;
static void after_fire(void *e) {
    AfterEnv *a = (AfterEnv *)e;
    go_sleep_ns(a->d);
    GoOpaque now = time_Now();
    go_chan_send(a->c, &now);
}
GoChan *time_After(i64 d) {
    AfterEnv *a = (AfterEnv *)gc_alloc(sizeof *a);
    a->c = go_chan_make(sizeof(GoOpaque), 1);
    a->d = d;
    go_spawn(after_fire, a);
    return a->c;
}
GoOpaque *time_NewTicker(i64 d) { (void)d; return (GoOpaque *)gc_alloc(sizeof(GoOpaque)); }  // its C field stays nil (never fires)
void time_Ticker_Stop(GoOpaque *t) { (void)t; }

// ---------------------------------------------------------------- math/rand (global source)
static u64 rnd_state = 0x9E3779B97F4A7C15ull;
static u64 rnd_next(void) {
    if (rnd_state == 0x9E3779B97F4A7C15ull) rnd_state ^= (u64)go_mono_ns();
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}
i64 rand_Int63(void) { return (i64)(rnd_next() >> 1); }
int rand_Int(void) { return (int)(rnd_next() >> 33); }

// ---------------------------------------------------------------- runtime, debug, log, color
void runtime_GC(void) { gc_collect(); }
int runtime_NumGoroutine(void) { return go_num_goroutines(); }
void runtime_ReadMemStats(GoOpaque *m) { (void)m; }
void runtime_SetFinalizer(GoIface o, GoIface f) { (void)o, (void)f; }
Ret_uintptr_t__GoString__int__bool runtime_Caller(int skip) { (void)skip; return (Ret_uintptr_t__GoString__int__bool){0}; }
int runtime_Callers(int skip, Slice_uintptr_t pc) { (void)skip, (void)pc; return 0; }
GoOpaque *runtime_FuncForPC(uintptr_t pc) { (void)pc; return NULL; }
GoString runtime_Func_Name(GoOpaque *f) { (void)f; return (GoString){"", 0}; }
Slice_u8 debug_Stack(void) { return (Slice_u8){0}; }
void log_Logger_Printf(GoOpaque *l, GoString f, Slice_GoIface a) {
    (void)l;
    GoString s = go_sprintf(f, a.p, a.len);
    fprintf(stderr, "%.*s\n", s.len, s.p);
}
void log_Logger_Println(GoOpaque *l, Slice_GoIface a) {
    (void)l;
    for (int i = 0; i < a.len; i++) {
        GoString s = go_sprintf(GOSTR("%v"), &a.p[i], 1);
        fprintf(stderr, i ? " %.*s" : "%.*s", s.len, s.p);
    }
    fputc('\n', stderr);
}
Ret_int__GoIface fmt_Fprintln(GoIface w, Slice_GoIface a) {  // to stdout/stderr only
    (void)w;
    log_Logger_Println(NULL, a);
    return (Ret_int__GoIface){0, {0}};
}
Ret_u32__u32__u32__u32 color_NRGBA_RGBA(GoOpaque c) {  // premultiplied 16-bit, as Go
    u32 r = c._opaque[0], g = c._opaque[1], b = c._opaque[2], a = c._opaque[3];
    r |= r << 8, g |= g << 8, b |= b << 8, a |= a << 8;
    return (Ret_u32__u32__u32__u32){r * a / 0xffff, g * a / 0xffff, b * a / 0xffff, a};
}

// time.Local / time.UTC: locations are opaque markers (the runtime keeps
// wall-clock times in local time)
static GoOpaque time_loc_local, time_loc_utc;
GoOpaque *time_Local = &time_loc_local, *time_UTC = &time_loc_utc;

// ---------------------------------------------------------------- context
// Background and WithCancel (the BGM loader cancels superseded loads).
// Every context is a *context.cancelCtx made here; Background's is never
// cancelled.
#ifndef GO_TID_P_context_cancelCtx
#define GO_TID_P_context_cancelCtx (-40)
#endif
typedef struct RtCtx {
    GoChan *done;
    bool cancelled;
    struct RtCtx **kids;
    int nkids, capkids;
} RtCtx;
static RtCtx *ctx_new(void) {
    RtCtx *c = (RtCtx *)gc_calloc(1, sizeof *c);
    c->done = go_chan_make(1, 0);
    return c;
}
static void ctx_cancel(RtCtx *c) {
    if (c->cancelled) return;
    c->cancelled = true;
    go_chan_close(c->done);
    for (int i = 0; i < c->nkids; i++) ctx_cancel(c->kids[i]);
}
static void ctx_cancel_fn(void *env) { ctx_cancel((RtCtx *)env); }
GoIface context_Background(void) {
    static RtCtx *bg;
    if (!bg) bg = ctx_new();  // reachable from the data segment: a GC root
    return (GoIface){GO_TID_P_context_cancelCtx, bg};
}
Ret_GoIface__GoFunc context_WithCancel(GoIface parent) {
    RtCtx *c = ctx_new();
    if (parent.type == GO_TID_P_context_cancelCtx && parent.data) {
        RtCtx *p = (RtCtx *)parent.data;
        if (p->cancelled) {
            ctx_cancel(c);
        } else {
            if (p->nkids == p->capkids) {
                int n = p->capkids ? p->capkids * 2 : 4;
                RtCtx **k = (RtCtx **)gc_calloc((size_t)n, sizeof *k);
                if (p->nkids) memcpy(k, p->kids, sizeof *k * (size_t)p->nkids);
                p->kids = k, p->capkids = n;
            }
            p->kids[p->nkids++] = c;
        }
    }
    return (Ret_GoIface__GoFunc){{GO_TID_P_context_cancelCtx, c}, {(void *)ctx_cancel_fn, c}};
}
GoChan *context_cancelCtx_Done(GoOpaque *c) { return ((RtCtx *)c)->done; }
GoIface context_cancelCtx_Err(GoOpaque *c) { return ((RtCtx *)c)->cancelled ? go_error(GOSTR("context canceled")) : (GoIface){0}; }
GoString context_cancelCtx_String(GoOpaque *c) { (void)c; return GOSTR("context.WithCancel"); }

// ---------------------------------------------------------------- io.LimitReader, bytes.TrimSpace
#ifndef GO_TID_P_io_LimitedReader
#define GO_TID_P_io_LimitedReader (-41)
#endif
GoIface io_LimitReader(GoIface r, i64 n) {
    GoIoLimitedReader *l = go_new(GoIoLimitedReader);
    l->R = r, l->N = n;
    return (GoIface){GO_TID_P_io_LimitedReader, l};
}
Ret_int__GoIface io_LimitedReader_Read(GoIoLimitedReader *l, Slice_u8 p) {
    if (l->N <= 0) return (Ret_int__GoIface){0, io_EOF};
    if ((i64)p.len > l->N) p.len = (int)l->N;
    Ret_int__GoIface r = ik_io_Read(l->R, p);
    l->N -= r.r0;
    return r;
}
Slice_u8 bytes_TrimSpace(Slice_u8 s) {
    int a = 0, b = s.len;
    while (a < b && (s.p[a] == ' ' || s.p[a] == '\t' || s.p[a] == '\n' || s.p[a] == '\r' || s.p[a] == '\v' || s.p[a] == '\f')) a++;
    while (b > a && (s.p[b - 1] == ' ' || s.p[b - 1] == '\t' || s.p[b - 1] == '\n' || s.p[b - 1] == '\r' || s.p[b - 1] == '\v' || s.p[b - 1] == '\f')) b--;
    return (Slice_u8){s.p + a, b - a, s.cap - a};
}
