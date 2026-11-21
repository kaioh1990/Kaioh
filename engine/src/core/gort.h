// Runtime support for code translated from Ikemen GO (tools/gen/gotoc):
// strings, slices, maps, interface values, function values and integer /
// float helpers with Go semantics (amd64, the reference platform).
#ifndef CVF_GORT_H
#define CVF_GORT_H

#include <math.h>
#include <stdlib.h>

#include "core/base.h"
#include "core/gc.h"
#include "core/sched.h"

// ---------------------------------------------------------------- strings
typedef struct GoString {
    const char *p;
    int len;
} GoString;

#define GOSTR(lit) ((GoString){(lit), (int)(sizeof(lit) - 1)})
GoString go_cstr(const char *s);
GoString go_cstr_copy(const char *s);  // copied into the GC heap (go_cstr only wraps s)
bool go_streq(GoString a, GoString b);
int go_strcmp(GoString a, GoString b);
GoString go_strcat(GoString a, GoString b);
GoString go_substr(GoString s, int lo, int hi);  // hi < 0: to end
GoString go_rune2str(i32 r);
GoString go_runes2str(const i32 *r, int n);  // string([]rune)
const char *go_strz(GoString s);  // NUL-terminated copy (scratch, rotating)
int go_decode_rune(GoString s, int i, i32 *r);  // utf8.DecodeRuneInString at i: width (>= 1)

// ---------------------------------------------------------------- slices
// Every slice type is `struct { T *p; int len, cap; }` (generated).
void *go_slice_grow(void *p, int len, int *cap, int need, size_t elem);
void *go_slice_grow_atomic(void *p, int len, int *cap, int need, size_t elem);  // pointer-free elements

// (operands evaluated once; hi < 0: omitted)
#define GO_SLICE(T, s, lo, hi)                                                         \
    __extension__({                                                                    \
        __typeof__(s) _ss = (s);                                                       \
        int _sl = (int)(lo), _sh = (int)(hi);                                          \
        (T){_ss.p + _sl, (_sh < 0 ? _ss.len : _sh) - _sl, _ss.cap - _sl};              \
    })
#define GO_ARRSLICE(T, arr, n, lo, hi)                                                 \
    __extension__({                                                                    \
        __typeof__(&*(arr)) _sa = (arr);                                               \
        int _sl = (int)(lo), _sh = (int)(hi);                                          \
        (T){_sa + _sl, (_sh < 0 ? (n) : _sh) - _sl, (n) - _sl};                        \
    })
// (n and c are evaluated once: they may have side effects, e.g. make([]T, r.Read8(6)+1))
#define GO_MAKE_(T, E, n, c, ALLOC)                                                    \
    __extension__({                                                                    \
        int _mn = (int)(n), _mc = (int)(c);                                            \
        if (_mc < _mn) _mc = _mn;                                                      \
        (T){(E *)ALLOC((size_t)(_mc > 0 ? _mc : 1), sizeof(E)), _mn, _mc};             \
    })
#define GO_MAKE(T, E, n, c) GO_MAKE_(T, E, n, c, gc_calloc)
#define GO_MAKE_A(T, E, n, c) GO_MAKE_(T, E, n, c, gc_calloc_atomic)
// append(s, v): grows like Go (amortised doubling), shares storage when
// capacity allows (aliasing semantics preserved).
// (variadic so compound-literal arguments with commas pass through)
#define GO_APPEND(T, s, ...) go_append_##T((s), (__VA_ARGS__))
#define GO_APPEND_SLICE(T, s, ...) go_append_slice_##T((s), (__VA_ARGS__))
#define GO_COPY(dst, src)                                                              \
    __extension__({                                                                    \
        __typeof__(dst) _cd = (dst);                                                   \
        __typeof__(src) _cs = (src);                                                   \
        go_copy_impl(_cd.p, _cd.len, _cs.p, _cs.len, sizeof(*_cd.p));                   \
    })
int go_copy_impl(void *dst, int dlen, const void *src, int slen, size_t elem);
#define GO_SLICE_LIT(T, E, n, ...) go_slice_lit_##T((E[]){__VA_ARGS__}, (n))

// Declares the append helpers for one slice type (instantiated in gen code);
// the _A variant is for element types that hold no heap pointers.
#define GO_SLICE_FUNCS_(T, E, GROW, ALLOC)                                             \
    static inline T go_append_##T(T s, E v) {                                          \
        if (s.len >= s.cap) s.p = (E *)GROW(s.p, s.len, &s.cap, s.len + 1, sizeof(E)); \
        s.p[s.len++] = v;                                                              \
        return s;                                                                      \
    }                                                                                  \
    static inline T go_append_slice_##T(T s, T s2) {                                   \
        if (s.len + s2.len > s.cap) s.p = (E *)GROW(s.p, s.len, &s.cap, s.len + s2.len, sizeof(E)); \
        if (s2.len) memmove(s.p + s.len, s2.p, sizeof(E) * (size_t)s2.len);            \
        s.len += s2.len;                                                               \
        return s;                                                                      \
    }                                                                                  \
    static inline T go_slice_lit_##T(const E *v, int n) {                              \
        T s = {(E *)ALLOC(sizeof(E) * (size_t)(n ? n : 1)), n, n};                     \
        memcpy(s.p, v, sizeof(E) * (size_t)n);                                         \
        return s;                                                                      \
    }
#define GO_SLICE_FUNCS(T, E) GO_SLICE_FUNCS_(T, E, go_slice_grow, gc_alloc)
#define GO_SLICE_FUNCS_A(T, E) GO_SLICE_FUNCS_(T, E, go_slice_grow_atomic, gc_alloc_atomic)

// ---------------------------------------------------------------- maps
// Insertion-ordered hash map (deterministic iteration, unlike Go's random
// order). Keys are compared bytewise, except string keys (contents).
typedef struct GoMap {
    struct GoMapImpl *m;
} GoMap;

typedef struct GoMapIter {
    struct GoMapImpl *m;
    int i;
    void *key, *val;
} GoMapIter;

// key kinds: 0 = compared as bytes, 1 = GoString, 2 = GoIface (Go equality)
GoMap gomap_make(size_t ksize, size_t vsize, int keykind);
void *gomap_find(GoMap *m, const void *key);            // NULL if absent
void *gomap_put(GoMap *m, const void *key, size_t ksize, size_t vsize, int keykind);  // inserts zero value
void gomap_del(GoMap *m, const void *key);
void gomap_clear(GoMap *m);  // maps.Clear: keeps the storage
int gomap_len(const GoMap *m);
GoMapIter gomap_iter(GoMap *m);
bool gomap_next(GoMapIter *it);

#define GO_MAP_FUNCS(M, K, V, STRKEY)                                                  \
    /* make() allocates: copies of a map value share its storage */                   \
    static inline M M##_make(int n) { (void)n; return (M)gomap_make(sizeof(K), sizeof(V), STRKEY); } \
    static inline V M##_get(M *m, K k, bool *ok) {                                      \
        V *v = (V *)gomap_find((GoMap *)m, &k);                                         \
        if (ok) *ok = v != NULL;                                                        \
        if (v) return *v;                                                               \
        V z;                                                                            \
        memset(&z, 0, sizeof z);                                                        \
        return z;                                                                       \
    }                                                                                   \
    static inline V *M##_ptr(M *m, K k) { return (V *)gomap_put((GoMap *)m, &k, sizeof(K), sizeof(V), STRKEY); } \
    static inline void M##_set(M *m, K k, V v) { *M##_ptr(m, k) = v; }                  \
    static inline void M##_delete(M *m, K k) { gomap_del((GoMap *)m, &k); }              \
    static inline int M##_len(M *m) { return gomap_len((GoMap *)m); }

// ---------------------------------------------------------------- interfaces
typedef struct GoIface {
    int type;  // 0 = nil
    void *data;
} GoIface;
bool go_iface_eq(GoIface a, GoIface b);  // defined with the type tables (ik_rt.c)
u64 go_iface_hash(GoIface a);           // consistent with go_iface_eq

typedef struct GoFunc {
    void *fn;
    void *env;
} GoFunc;

typedef struct GoOpaque {
    u8 _opaque[32];
} GoOpaque;
typedef GoOpaque GoOpaqueVal;

// ---------------------------------------------------------------- memory
void *go_box_impl(const void *v, size_t n);
#define go_box(T, ...) ((T *)go_box_impl(&(T[1]){__VA_ARGS__}, sizeof(T)))
void *go_box_atomic_impl(const void *v, size_t n);
#define go_box_a(T, ...) ((T *)go_box_atomic_impl(&(T[1]){__VA_ARGS__}, sizeof(T)))  // pointer-free T
#define go_new(T) ((T *)gc_calloc(1, sizeof(T)))
#define go_new_atomic(T) ((T *)gc_alloc_atomic(sizeof(T)))
// panic/recover: functions with deferred calls push a GoPanicFrame
// (setjmp); a panic longjmps to the innermost one, which runs its deferred
// calls - recover() there stops the panic - and otherwise passes it on.
#include <setjmp.h>
typedef struct GoPanicFrame {
    jmp_buf jb;
    struct GoPanicFrame *prev;
} GoPanicFrame;
extern GoPanicFrame *go_panic_top;  // per goroutine (switched by sched.c)
extern GoIface go_panic_val;
extern bool go_panicking;
_Noreturn void go_panic_raise(GoIface v);
_Noreturn void go_panic_continue(void);  // after the deferred calls: still panicking
GoIface go_recover(void);
_Noreturn void go_panic_str(const char *msg);
_Noreturn void go_panic_gostr(GoString msg);
_Noreturn void go_panic_s(int, GoString msg);
_Noreturn void go_panic_iface(int, GoIface v);  // ik_rt.c: prints strings and errors
_Noreturn void go_panic_other(int, ...);
#define go_panic(x) _Generic((x), GoString: go_panic_s, GoIface: go_panic_iface, default: go_panic_other)(0, x)

// ---------------------------------------------------------------- numbers
f64 go_nan(void);
static inline bool math_IsNaN(f64 f) { return f != f; }
static inline bool math_IsInf(f64 f, int sign) {
    return (sign >= 0 && f > 1.7976931348623157e308) || (sign <= 0 && f < -1.7976931348623157e308);
}
static inline f64 math_Abs(f64 f) { return fabs(f); }
static inline f64 math_Floor(f64 f) { return floor(f); }
static inline f64 math_Ceil(f64 f) { return ceil(f); }
static inline f64 math_Sqrt(f64 f) { return sqrt(f); }
static inline f64 math_Trunc(f64 f) { return trunc(f); }
static inline f64 math_Round(f64 f) { return round(f); }  // Go: half away from zero, same as C round
static inline f64 math_Max(f64 a, f64 b) { return a > b ? a : b; }  // TODO: NaN/±0 cases
static inline f64 math_Min(f64 a, f64 b) { return a < b ? a : b; }
static inline f64 math_Copysign(f64 a, f64 b) { return copysign(a, b); }
static inline bool math_Signbit(f64 f) { return signbit(f) != 0; }
static inline f64 math_Inf(int s) { return s >= 0 ? 1.0 / 0.0 : -1.0 / 0.0; }
static inline f64 math_NaN(void) { return go_nan(); }
static inline u32 math_Float32bits(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }
static inline f32 math_Float32frombits(u32 u) { f32 f; memcpy(&f, &u, 4); return f; }
static inline u64 math_Float64bits(f64 f) { u64 u; memcpy(&u, &f, 8); return u; }
static inline f64 math_Float64frombits(u64 u) { f64 f; memcpy(&f, &u, 8); return f; }

// float -> integer conversions as compiled by Go for amd64.
static inline i32 go_f2i32(f64 f) { return (f > -2147483649.0 && f < 2147483648.0) ? (i32)f : INT32_MIN; }
static inline i64 go_f2i64(f64 f) {
    return (f >= -9223372036854775808.0 && f < 9223372036854775808.0) ? (i64)f : INT64_MIN;
}
static inline i8 go_f2i8(f64 f) { return (i8)go_f2i32(f); }
static inline i16 go_f2i16(f64 f) { return (i16)go_f2i32(f); }
static inline u8 go_f2u8(f64 f) { return (u8)go_f2i32(f); }
static inline u16 go_f2u16(f64 f) { return (u16)go_f2i32(f); }
static inline u32 go_f2u32(f64 f) { return (u32)go_f2i64(f); }
static inline u64 go_f2u64(f64 f) {
    if (f < 9223372036854775808.0) return (u64)go_f2i64(f);
    return (u64)go_f2i64(f - 9223372036854775808.0) ^ 0x8000000000000000ull;
}

// Integer division/remainder: Go wraps MinInt/-1 instead of trapping.
#define GO_DIVREM(bits, T)                                                              \
    static inline T go_div##bits(T a, T b) { return b == -1 ? (T)(0 - (u##bits)a) : a / b; } \
    static inline T go_rem##bits(T a, T b) { return b == -1 ? 0 : a % b; }
GO_DIVREM(8, i8)
GO_DIVREM(16, i16)
GO_DIVREM(32, i32)
GO_DIVREM(64, i64)
static inline u8 go_divu8(u8 a, u8 b) { return a / b; }
static inline u8 go_remu8(u8 a, u8 b) { return a % b; }
static inline u16 go_divu16(u16 a, u16 b) { return a / b; }
static inline u16 go_remu16(u16 a, u16 b) { return a % b; }
static inline u32 go_divu32(u32 a, u32 b) { return a / b; }
static inline u32 go_remu32(u32 a, u32 b) { return a % b; }
static inline u64 go_divu64(u64 a, u64 b) { return a / b; }
static inline u64 go_remu64(u64 a, u64 b) { return a % b; }

// Shifts by >= width: 0 (or sign fill for signed right shifts), as in Go.
#define GO_SHIFTS(bits, T, U)                                                            \
    static inline T go_shl##bits(T x, i64 n) { return (n < 0 || n >= bits) ? 0 : (T)((U)x << n); } \
    static inline T go_shr##bits(T x, i64 n) { return (n < 0 || n >= bits) ? (x < 0 ? -1 : 0) : (T)(x >> n); } \
    static inline U go_shlu##bits(U x, i64 n) { return (n < 0 || n >= bits) ? 0 : (U)(x << n); } \
    static inline U go_shru##bits(U x, i64 n) { return (n < 0 || n >= bits) ? 0 : (U)(x >> n); }
GO_SHIFTS(8, i8, u8)
GO_SHIFTS(16, i16, u16)
GO_SHIFTS(32, i32, u32)
GO_SHIFTS(64, i64, u64)

#define GO_MINMAX(T)                                                                     \
    static inline T go_min_##T(T a, T b) { return b < a ? b : a; }                       \
    static inline T go_max_##T(T a, T b) { return b > a ? b : a; }
GO_MINMAX(i8)
GO_MINMAX(i16)
GO_MINMAX(i32)
GO_MINMAX(i64)
GO_MINMAX(int)
GO_MINMAX(u8)
GO_MINMAX(u16)
GO_MINMAX(u32)
GO_MINMAX(f32)
GO_MINMAX(f64)
// Ikemen's generic helpers (common.go) instantiated per type.
#define GO_IKHELPERS(T)                                                                  \
    static inline T fn_Abs_##T(T v) { return v < 0 ? -v : v; }                           \
    static inline T fn_Sign_##T(T v) { return v < 0 ? (T)-1 : (v > 0 ? (T)1 : (T)0); }   \
    static inline T fn_Clamp_##T(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
GO_IKHELPERS(i32)
GO_IKHELPERS(i64)
GO_IKHELPERS(int)
GO_IKHELPERS(f32)
GO_IKHELPERS(f64)

#endif
