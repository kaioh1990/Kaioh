// Go standard-library subset for the translated simulation: errors, sort,
// strings, strconv parsing, unicode, math helpers, and inert stand-ins for
// the concurrency/time/reflection APIs the single-threaded headless
// simulation does not depend on.
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "formats/text.h"
#include "ik/ik_sim.h"

// ---------------------------------------------------------------- errors
GoIface go_error(GoString msg) { return (GoIface){GO_TID_ERRSTR, go_box(GoString, msg)}; }

GoIface go_errorf(const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof t) n = (int)sizeof t - 1;
    char *p = (char *)gc_alloc_atomic((size_t)n + 1);
    memcpy(p, t, (size_t)n + 1);
    return go_error((GoString){p, n});
}

GoString go_error_string(GoIface e) {
    if (e.type == GO_TID_ERRSTR) return *(GoString *)e.data;
    return (GoString){"", 0};
}

extern const unsigned char ik_tid_kind[];
extern const unsigned ik_tid_size[];
extern const int ik_tid_count;

// Go interface equality: same dynamic type and equal values (pointer
// identity for pointer types and runtime errors).
bool go_iface_eq(GoIface a, GoIface b) {
    if (a.type != b.type) return false;
    if (a.type == 0 || a.data == b.data) return true;
    if (a.type == GO_TID_ERRSTR || a.type == GO_TID_FILEINFO) return false;
    int kind = a.type < 20 ? a.type : (a.type < ik_tid_count ? ik_tid_kind[a.type] : 0);
    if (kind == GO_TID_string) return go_streq(*(GoString *)a.data, *(GoString *)b.data);
    unsigned size = 0;
    switch (kind) {
    case GO_TID_bool: case GO_TID_i8: case GO_TID_u8: size = 1; break;
    case GO_TID_i16: case GO_TID_u16: size = 2; break;
    case GO_TID_i32: case GO_TID_u32: size = 4; break;
    case GO_TID_i64: case GO_TID_u64: size = 8; break;
    case GO_TID_int: size = sizeof(int); break;
    case GO_TID_uint: case GO_TID_uintptr: size = sizeof(uintptr_t); break;
    case GO_TID_f32: return *(f32 *)a.data == *(f32 *)b.data;
    case GO_TID_f64: return *(f64 *)a.data == *(f64 *)b.data;
    default: size = a.type < ik_tid_count ? ik_tid_size[a.type] : 0;
    }
    if (size == 0) return false;  // pointer types: identity (checked above)
    return memcmp(a.data, b.data, size) == 0;
}

GoIface errors_New(GoString a0) { return go_error(a0); }
bool errors_Is(GoIface a0, GoIface a1) {
    if (a0.type == GO_TID_ERRSTR && a1.type == GO_TID_ERRSTR) return a0.data == a1.data;
    return go_iface_eq(a0, a1);
}

// ---------------------------------------------------------------- bytes
GoString go_bytes2str(Slice_u8 b) {
    char *p = (char *)gc_alloc_atomic((size_t)b.len + 1);
    if (b.len) memcpy(p, b.p, (size_t)b.len);
    p[b.len] = 0;
    return (GoString){p, b.len};
}

Slice_u8 go_str2bytes(GoString s) {
    Slice_u8 b = {(u8 *)gc_alloc_atomic((size_t)(s.len ? s.len : 1)), s.len, s.len};
    if (s.len) memcpy(b.p, s.p, (size_t)s.len);
    return b;
}

// ---------------------------------------------------------------- sort
// Go's pdqsort_func (sort/zsortfunc.go), so unstable orderings match.
typedef struct LessSwap {
    GoFunc less;
    u8 *base;
    size_t elem;
    u8 tmp[256];
    u8 *big;
} LessSwap;

static inline bool ls_less(LessSwap *d, int i, int j) {
    return ((bool (*)(void *, int, int))d->less.fn)(d->less.env, i, j);
}
static inline void ls_swap(LessSwap *d, int i, int j) {
    u8 *a = d->base + (size_t)i * d->elem, *b = d->base + (size_t)j * d->elem;
    u8 *t = d->elem <= sizeof d->tmp ? d->tmp : d->big;
    memcpy(t, a, d->elem);
    memcpy(a, b, d->elem);
    memcpy(b, t, d->elem);
}

static void insertion_sort(LessSwap *d, int a, int b) {
    for (int i = a + 1; i < b; i++)
        for (int j = i; j > a && ls_less(d, j, j - 1); j--) ls_swap(d, j, j - 1);
}

static void sift_down(LessSwap *d, int lo, int hi, int first) {
    int root = lo;
    for (;;) {
        int child = 2 * root + 1;
        if (child >= hi) break;
        if (child + 1 < hi && ls_less(d, first + child, first + child + 1)) child++;
        if (!ls_less(d, first + root, first + child)) return;
        ls_swap(d, first + root, first + child);
        root = child;
    }
}

static void heap_sort(LessSwap *d, int a, int b) {
    int first = a, lo = 0, hi = b - a;
    for (int i = (hi - 1) / 2; i >= 0; i--) sift_down(d, i, hi, first);
    for (int i = hi - 1; i >= 0; i--) {
        ls_swap(d, first, first + i);
        sift_down(d, lo, i, first);
    }
}

static int bits_len(unsigned long long x) {
    int n = 0;
    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

enum { UNKNOWN_HINT, INCREASING_HINT, DECREASING_HINT };

static void order2(LessSwap *d, int *a, int *b, int *swaps) {
    if (ls_less(d, *b, *a)) {
        (*swaps)++;
        int t = *a;
        *a = *b;
        *b = t;
    }
}
static int median(LessSwap *d, int a, int b, int c, int *swaps) {
    order2(d, &a, &b, swaps);
    order2(d, &b, &c, swaps);
    order2(d, &a, &b, swaps);
    return b;
}
static int median_adjacent(LessSwap *d, int a, int *swaps) { return median(d, a - 1, a, a + 1, swaps); }

static int choose_pivot(LessSwap *d, int a, int b, int *hint) {
    int l = b - a, swaps = 0;
    int i = a + l / 4 * 1, j = a + l / 4 * 2, k = a + l / 4 * 3;
    if (l >= 8) {
        if (l >= 50) {
            i = median_adjacent(d, i, &swaps);
            j = median_adjacent(d, j, &swaps);
            k = median_adjacent(d, k, &swaps);
        }
        j = median(d, i, j, k, &swaps);
    }
    *hint = swaps == 0 ? INCREASING_HINT : swaps == 12 ? DECREASING_HINT : UNKNOWN_HINT;
    return j;
}

static void reverse_range(LessSwap *d, int a, int b) {
    for (int i = a, j = b - 1; i < j; i++, j--) ls_swap(d, i, j);
}

static bool partial_insertion_sort(LessSwap *d, int a, int b) {
    int i = a + 1;
    for (int j = 0; j < 5; j++) {
        while (i < b && !ls_less(d, i, i - 1)) i++;
        if (i == b) return true;
        if (b - a < 50) return false;
        ls_swap(d, i, i - 1);
        if (i - a >= 2)
            for (int k = i - 1; k >= 1; k--) {
                if (!ls_less(d, k, k - 1)) break;
                ls_swap(d, k, k - 1);
            }
        if (b - i >= 2)
            for (int k = i + 1; k < b; k++) {
                if (!ls_less(d, k, k - 1)) break;
                ls_swap(d, k, k - 1);
            }
    }
    return false;
}

static void break_patterns(LessSwap *d, int a, int b) {
    int length = b - a;
    if (length >= 8) {
        u64 r = (u64)length;
        unsigned long long modulus = 1ull << bits_len((unsigned long long)length);
        for (int idx = a + (length / 4) * 2 - 1; idx <= a + (length / 4) * 2 + 1; idx++) {
            r ^= r << 13;
            r ^= r >> 7;
            r ^= r << 17;
            int other = (int)((unsigned long long)r & (modulus - 1));  // uint is 64-bit on the reference
            if (other >= length) other -= length;
            ls_swap(d, idx, a + other);
        }
    }
}

static int partition_equal(LessSwap *d, int a, int b, int pivot) {
    ls_swap(d, a, pivot);
    int i = a + 1, j = b - 1;
    for (;;) {
        while (i <= j && !ls_less(d, a, i)) i++;
        while (i <= j && ls_less(d, a, j)) j--;
        if (i > j) break;
        ls_swap(d, i, j);
        i++;
        j--;
    }
    return i;
}

static int partition(LessSwap *d, int a, int b, int pivot, bool *already) {
    ls_swap(d, a, pivot);
    int i = a + 1, j = b - 1;
    while (i <= j && ls_less(d, i, a)) i++;
    while (i <= j && !ls_less(d, j, a)) j--;
    if (i > j) {
        ls_swap(d, j, a);
        *already = true;
        return j;
    }
    ls_swap(d, i, j);
    i++;
    j--;
    for (;;) {
        while (i <= j && ls_less(d, i, a)) i++;
        while (i <= j && !ls_less(d, j, a)) j--;
        if (i > j) break;
        ls_swap(d, i, j);
        i++;
        j--;
    }
    ls_swap(d, j, a);
    *already = false;
    return j;
}

static void pdqsort(LessSwap *d, int a, int b, int limit) {
    bool was_balanced = true, was_partitioned = true;
    for (;;) {
        int length = b - a;
        if (length <= 12) {
            insertion_sort(d, a, b);
            return;
        }
        if (limit == 0) {
            heap_sort(d, a, b);
            return;
        }
        if (!was_balanced) {
            break_patterns(d, a, b);
            limit--;
        }
        int hint;
        int pivot = choose_pivot(d, a, b, &hint);
        if (hint == DECREASING_HINT) {
            reverse_range(d, a, b);
            pivot = (b - 1) - (pivot - a);
            hint = INCREASING_HINT;
        }
        if (was_balanced && was_partitioned && hint == INCREASING_HINT)
            if (partial_insertion_sort(d, a, b)) return;
        if (a > 0 && !ls_less(d, a - 1, pivot)) {
            a = partition_equal(d, a, b, pivot);
            continue;
        }
        bool already;
        int mid = partition(d, a, b, pivot, &already);
        was_partitioned = already;
        int left = mid - a, right = b - mid, thr = length / 8;
        if (left < right) {
            was_balanced = left >= thr;
            pdqsort(d, a, mid, limit);
            a = mid + 1;
        } else {
            was_balanced = right >= thr;
            pdqsort(d, mid + 1, b, limit);
            b = mid;
        }
    }
}

static void ls_init(LessSwap *d, void *hdr, size_t elem, GoFunc less) {
    GoSliceHdr *h = (GoSliceHdr *)hdr;
    d->less = less;
    d->base = (u8 *)h->p;
    d->elem = elem;
    d->big = elem > sizeof d->tmp ? (u8 *)gc_alloc(elem) : NULL;
}

void go_sort_slice(void *hdr, size_t elem, GoFunc less) {
    LessSwap d;
    ls_init(&d, hdr, elem, less);
    int n = ((GoSliceHdr *)hdr)->len;
    pdqsort(&d, 0, n, bits_len((unsigned long long)n));
    gc_free(d.big);
}

void go_sort_slice_stable(void *hdr, size_t elem, GoFunc less) {
    // any stable sort yields the same result; binary insertion keeps it simple
    LessSwap d;
    ls_init(&d, hdr, elem, less);
    insertion_sort(&d, 0, ((GoSliceHdr *)hdr)->len);
    gc_free(d.big);
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_str(const void *a, const void *b) { return go_strcmp(*(const GoString *)a, *(const GoString *)b); }
void sort_Ints(Slice_int a0) {
    if (a0.len > 1) qsort(a0.p, (size_t)a0.len, sizeof(int), cmp_int);  // equal ints are indistinguishable
}
void sort_Strings(Slice_GoString a0) {
    if (a0.len > 1) qsort(a0.p, (size_t)a0.len, sizeof(GoString), cmp_str);
}

// ---------------------------------------------------------------- strings
static GoString mkstr(const char *p, int n) {
    char *q = (char *)gc_alloc_atomic((size_t)n + 1);
    if (n) memcpy(q, p, (size_t)n);
    q[n] = 0;
    return (GoString){q, n};
}

static bool is_space(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
// Unicode white space beyond ASCII (U+0085, U+00A0, U+1680, U+2000-200A,
// U+2028/9, U+202F, U+205F, U+3000) - decoded from UTF-8.
static int space_len_at(GoString s, int i) {
    u8 c = (u8)s.p[i];
    if (c < 0x80) return is_space(c) ? 1 : 0;
    if (c == 0xC2 && i + 1 < s.len && ((u8)s.p[i + 1] == 0x85 || (u8)s.p[i + 1] == 0xA0)) return 2;
    if (c == 0xE1 && i + 2 < s.len && (u8)s.p[i + 1] == 0x9A && (u8)s.p[i + 2] == 0x80) return 3;
    if (c == 0xE2 && i + 2 < s.len) {
        u8 c1 = (u8)s.p[i + 1], c2 = (u8)s.p[i + 2];
        if (c1 == 0x80 && (c2 <= 0x8A || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF)) return 3;
        if (c1 == 0x81 && c2 == 0x9F) return 3;
    }
    if (c == 0xE3 && i + 2 < s.len && (u8)s.p[i + 1] == 0x80 && (u8)s.p[i + 2] == 0x80) return 3;
    return 0;
}
static int space_len_before(GoString s, int end) {  // trailing space ending at end
    for (int k = 1; k <= 3 && end - k >= 0; k++) {
        u8 c = (u8)s.p[end - k];
        if (k == 1 && c < 0x80) return is_space(c) ? 1 : 0;
        if ((c & 0xC0) != 0x80) return space_len_at(s, end - k) == k ? k : 0;
    }
    return 0;
}

GoString strings_TrimSpace(GoString s) {
    int a = 0, b = s.len, n;
    while (a < b && (n = space_len_at(s, a)) > 0) a += n;
    while (b > a && (n = space_len_before(s, b)) > 0) b -= n;
    return (GoString){s.p + a, b - a};
}

static bool in_cutset(u8 c, GoString cut) {
    for (int i = 0; i < cut.len; i++)
        if ((u8)cut.p[i] == c) return true;
    return false;
}
GoString strings_Trim(GoString s, GoString cut) {  // ASCII cutsets (all Ikemen uses)
    int a = 0, b = s.len;
    while (a < b && in_cutset((u8)s.p[a], cut)) a++;
    while (b > a && in_cutset((u8)s.p[b - 1], cut)) b--;
    return (GoString){s.p + a, b - a};
}
GoString strings_TrimLeft(GoString s, GoString cut) {
    int a = 0;
    while (a < s.len && in_cutset((u8)s.p[a], cut)) a++;
    return (GoString){s.p + a, s.len - a};
}
GoString strings_TrimRight(GoString s, GoString cut) {
    int b = s.len;
    while (b > 0 && in_cutset((u8)s.p[b - 1], cut)) b--;
    return (GoString){s.p, b};
}
bool strings_HasPrefix(GoString s, GoString p) { return s.len >= p.len && memcmp(s.p, p.p, (size_t)p.len) == 0; }
bool strings_HasSuffix(GoString s, GoString p) {
    return s.len >= p.len && memcmp(s.p + s.len - p.len, p.p, (size_t)p.len) == 0;
}
GoString strings_TrimPrefix(GoString s, GoString p) { return strings_HasPrefix(s, p) ? (GoString){s.p + p.len, s.len - p.len} : s; }
GoString strings_TrimSuffix(GoString s, GoString p) { return strings_HasSuffix(s, p) ? (GoString){s.p, s.len - p.len} : s; }

int strings_Index(GoString s, GoString sub) {
    if (sub.len == 0) return 0;
    for (int i = 0; i + sub.len <= s.len; i++)
        if (s.p[i] == sub.p[0] && memcmp(s.p + i, sub.p, (size_t)sub.len) == 0) return i;
    return -1;
}
int strings_LastIndex(GoString s, GoString sub) {
    if (sub.len == 0) return s.len;
    for (int i = s.len - sub.len; i >= 0; i--)
        if (memcmp(s.p + i, sub.p, (size_t)sub.len) == 0) return i;
    return -1;
}
int strings_IndexByte(GoString s, u8 c) {
    for (int i = 0; i < s.len; i++)
        if ((u8)s.p[i] == c) return i;
    return -1;
}
int strings_IndexAny(GoString s, GoString chars) {
    for (int i = 0; i < s.len; i++)
        if (in_cutset((u8)s.p[i], chars)) return i;
    return -1;
}
bool strings_Contains(GoString s, GoString sub) { return strings_Index(s, sub) >= 0; }

static int utf8_decode(const char *p, int n, i32 *r) {
    u8 c = (u8)p[0];
    if (c < 0x80) {
        *r = c;
        return 1;
    }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (len == 1 || len > n) {
        *r = 0xFFFD;
        return 1;
    }
    i32 v = c & (0x7F >> len);
    for (int i = 1; i < len; i++) {
        if (((u8)p[i] & 0xC0) != 0x80) {
            *r = 0xFFFD;
            return 1;
        }
        v = (v << 6) | ((u8)p[i] & 0x3F);
    }
    *r = v;
    return len;
}

bool strings_ContainsRune(GoString s, i32 r) {
    for (int i = 0; i < s.len;) {
        i32 c;
        i += utf8_decode(s.p + i, s.len - i, &c);
        if (c == r) return true;
    }
    return false;
}

bool unicode_IsSpace(i32 r) {
    switch (r) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ': case 0x85: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    }
    return r >= 0x2000 && r <= 0x200A;
}

static inline bool rune_pred(GoFunc f, i32 r) { return ((bool (*)(void *, i32))f.fn)(f.env, r); }
int strings_IndexFunc(GoString s, GoFunc f) {
    for (int i = 0; i < s.len;) {
        i32 c;
        int n = utf8_decode(s.p + i, s.len - i, &c);
        if (rune_pred(f, c)) return i;
        i += n;
    }
    return -1;
}
GoString strings_TrimLeftFunc(GoString s, GoFunc f) {
    int i = 0;
    while (i < s.len) {
        i32 c;
        int n = utf8_decode(s.p + i, s.len - i, &c);
        if (!rune_pred(f, c)) break;
        i += n;
    }
    return (GoString){s.p + i, s.len - i};
}

i32 unicode_ToLower(i32 r) {
    if (r >= 'A' && r <= 'Z') return r + 32;
    if (r < 0x80) return r;
    if ((r >= 0xC0 && r <= 0xDE && r != 0xD7) || (r >= 0x391 && r <= 0x3AB && r != 0x3A2)) return r + 32;
    if (r >= 0x410 && r <= 0x42F) return r + 32;
    if (r >= 0x400 && r <= 0x40F) return r + 80;
    if (r >= 0x100 && r <= 0x17F && !(r & 1) && r != 0x130 && r != 0x138) return r + 1;
    if (r >= 0xFF21 && r <= 0xFF3A) return r + 32;
    return r;
}
i32 unicode_ToUpper(i32 r) {
    if (r >= 'a' && r <= 'z') return r - 32;
    if (r < 0x80) return r;
    if ((r >= 0xE0 && r <= 0xFE && r != 0xF7) || (r >= 0x3B1 && r <= 0x3CB && r != 0x3C2)) return r - 32;
    if (r >= 0x430 && r <= 0x44F) return r - 32;
    if (r >= 0x450 && r <= 0x45F) return r - 80;
    if (r >= 0x101 && r <= 0x17F && (r & 1) && r != 0x131 && r != 0x149) return r - 1;
    if (r == 0xFF) return 0x178;
    if (r >= 0xFF41 && r <= 0xFF5A) return r - 32;
    return r;
}

static GoString map_case(GoString s, bool upper) {
    bool ascii = true;
    for (int i = 0; i < s.len; i++)
        if ((u8)s.p[i] >= 0x80) ascii = false;
    if (ascii) {
        int i = 0;
        for (; i < s.len; i++) {
            char c = s.p[i];
            if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) break;
        }
        if (i == s.len) return s;  // Go returns the input unchanged
        char *p = (char *)gc_alloc_atomic((size_t)s.len + 1);
        for (int k = 0; k < s.len; k++) {
            char c = s.p[k];
            if (upper && c >= 'a' && c <= 'z') c -= 32;
            if (!upper && c >= 'A' && c <= 'Z') c += 32;
            p[k] = c;
        }
        p[s.len] = 0;
        return (GoString){p, s.len};
    }
    char *p = (char *)gc_alloc_atomic((size_t)s.len * 2 + 4);
    int n = 0;
    for (int i = 0; i < s.len;) {
        i32 r;
        i += utf8_decode(s.p + i, s.len - i, &r);
        r = upper ? unicode_ToUpper(r) : unicode_ToLower(r);
        GoString e = go_rune2str(r);
        memcpy(p + n, e.p, (size_t)e.len);
        n += e.len;
        gc_free((void *)e.p);
    }
    p[n] = 0;
    return (GoString){p, n};
}
GoString strings_ToLower(GoString s) { return map_case(s, false); }
GoString strings_ToUpper(GoString s) { return map_case(s, true); }

bool strings_EqualFold(GoString a, GoString b) {
    int i = 0, j = 0;
    while (i < a.len && j < b.len) {
        i32 r1, r2;
        i += utf8_decode(a.p + i, a.len - i, &r1);
        j += utf8_decode(b.p + j, b.len - j, &r2);
        if (r1 != r2 && unicode_ToLower(r1) != unicode_ToLower(r2)) return false;
    }
    return i == a.len && j == b.len;
}

GoString strings_Clone(GoString s) { return mkstr(s.p, s.len); }

GoString strings_Replace(GoString s, GoString old, GoString nw, int n) {
    if (n == 0 || (old.len == 0 && nw.len == 0)) return s;
    // count matches
    int m = 0;
    if (old.len == 0) {
        m = 1;
        for (int i = 0; i < s.len;) {
            i32 r;
            i += utf8_decode(s.p + i, s.len - i, &r);
            m++;
        }
    } else {
        for (int i = 0; i + old.len <= s.len;) {
            int k = strings_Index((GoString){s.p + i, s.len - i}, old);
            if (k < 0) break;
            m++;
            i += k + old.len;
        }
    }
    if (m == 0) return s;
    if (n < 0 || m < n) n = m;
    char *p = (char *)gc_alloc_atomic((size_t)(s.len + n * (nw.len - old.len) + 1 > 0 ? s.len + n * (nw.len - old.len) + 1 : 1));
    int w = 0, start = 0;
    for (int i = 0; i < n; i++) {
        int j = start;
        if (old.len == 0) {
            if (i > 0) {
                i32 r;
                j += utf8_decode(s.p + start, s.len - start, &r);
            }
        } else {
            j += strings_Index((GoString){s.p + start, s.len - start}, old);
        }
        memcpy(p + w, s.p + start, (size_t)(j - start));
        w += j - start;
        memcpy(p + w, nw.p, (size_t)nw.len);
        w += nw.len;
        start = j + old.len;
    }
    memcpy(p + w, s.p + start, (size_t)(s.len - start));
    w += s.len - start;
    p[w] = 0;
    return (GoString){p, w};
}
GoString strings_ReplaceAll(GoString s, GoString old, GoString nw) { return strings_Replace(s, old, nw, -1); }

static Slice_GoString split_n(GoString s, GoString sep, int sepSave, int n) {
    Slice_GoString out = {0};
    if (n == 0) return out;
    if (sep.len == 0) {  // explode into UTF-8 sequences
        for (int i = 0; i < s.len;) {
            i32 r;
            int k = utf8_decode(s.p + i, s.len - i, &r);
            if (n > 0 && out.len == n - 1) k = s.len - i;
            out = go_append_Slice_GoString(out, (GoString){s.p + i, k});
            i += k;
        }
        return out;
    }
    int start = 0;
    while (n < 0 || out.len < n - 1) {
        int k = strings_Index((GoString){s.p + start, s.len - start}, sep);
        if (k < 0) break;
        out = go_append_Slice_GoString(out, (GoString){s.p + start, k + sepSave});
        start += k + sep.len;
    }
    out = go_append_Slice_GoString(out, (GoString){s.p + start, s.len - start});
    return out;
}
Slice_GoString strings_Split(GoString s, GoString sep) { return split_n(s, sep, 0, -1); }
Slice_GoString strings_SplitN(GoString s, GoString sep, int n) { return split_n(s, sep, 0, n); }

Slice_GoString strings_Fields(GoString s) {
    Slice_GoString out = {0};
    int i = 0, n;
    while (i < s.len) {
        while (i < s.len && (n = space_len_at(s, i)) > 0) i += n;
        if (i >= s.len) break;
        int st = i;
        while (i < s.len && space_len_at(s, i) == 0) i++;
        out = go_append_Slice_GoString(out, (GoString){s.p + st, i - st});
    }
    return out;
}

GoString strings_Join(Slice_GoString a, GoString sep) {
    if (a.len == 0) return (GoString){"", 0};
    if (a.len == 1) return a.p[0];
    int n = sep.len * (a.len - 1);
    for (int i = 0; i < a.len; i++) n += a.p[i].len;
    char *p = (char *)gc_alloc_atomic((size_t)n + 1);
    int w = 0;
    for (int i = 0; i < a.len; i++) {
        if (i) {
            memcpy(p + w, sep.p, (size_t)sep.len);
            w += sep.len;
        }
        memcpy(p + w, a.p[i].p, (size_t)a.p[i].len);
        w += a.p[i].len;
    }
    p[w] = 0;
    return (GoString){p, w};
}

// strings.Builder lives in the GoOpaque storage of the Go value.
typedef struct SB {
    char *p;
    int len, cap;
} SB;
_Static_assert(sizeof(SB) <= sizeof(GoOpaque), "Builder fits");
Ret_int__GoIface strings_Builder_WriteString(GoOpaque *recv, GoString s) {
    SB *b = (SB *)recv;
    if (b->len + s.len + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 32;
        while (nc < b->len + s.len + 1) nc *= 2;
        b->p = (char *)gc_realloc(b->p, (size_t)nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, s.p, (size_t)s.len);
    b->len += s.len;
    return (Ret_int__GoIface){s.len, {0}};
}
GoString strings_Builder_String(GoOpaque *recv) {
    SB *b = (SB *)recv;
    return mkstr(b->p ? b->p : "", b->len);
}
int strings_Builder_Len(GoOpaque *recv) { return ((SB *)recv)->len; }
void strings_Builder_Reset(GoOpaque *recv) { memset(recv, 0, sizeof(SB)); }

bool utf8_Valid(Slice_u8 b) {
    for (int i = 0; i < b.len;) {
        i32 r;
        int n = utf8_decode((const char *)b.p + i, b.len - i, &r);
        if (r == 0xFFFD && n == 1 && !(b.len - i >= 3 && b.p[i] == 0xEF && b.p[i + 1] == 0xBF && b.p[i + 2] == 0xBD))
            return false;
        i += n;
    }
    return true;
}

// ---------------------------------------------------------------- strconv
static GoIface num_error(const char *fn, GoString s, const char *what) {
    return go_errorf("strconv.%s: parsing \"%.*s\": %s", fn, s.len, s.p, what);
}

// Go float literal syntax check (decimal and hex mantissas, "_" only
// between digits after a base prefix, inf/infinity/nan, optional sign).
static bool float_syntax(GoString s, bool *special) {
    int i = 0, n = s.len;
    *special = false;
    if (n == 0) return false;
    if (s.p[0] == '+' || s.p[0] == '-') i++;
    GoString rest = {s.p + i, n - i};
    const char *sp[] = {"inf", "infinity", "nan"};
    for (int k = 0; k < 3; k++)
        if (rest.len == (int)strlen(sp[k]) && strings_EqualFold(rest, go_cstr(sp[k]))) {
            *special = true;
            return k < 2 || i == 0;  // signed NaN is invalid
        }
    bool hex = false;
    if (i + 1 < n && s.p[i] == '0' && (s.p[i + 1] == 'x' || s.p[i + 1] == 'X')) {
        hex = true;
        i += 2;
    }
    bool digits = false, dot = false;
    for (; i < n; i++) {
        char c = s.p[i];
        if (c == '_' && hex) continue;
        if (c == '.') {
            if (dot) return false;
            dot = true;
        } else if ((c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))) {
            digits = true;
        } else {
            break;
        }
    }
    if (!digits) return false;
    if (i < n && ((!hex && (s.p[i] == 'e' || s.p[i] == 'E')) || (hex && (s.p[i] == 'p' || s.p[i] == 'P')))) {
        i++;
        if (i < n && (s.p[i] == '+' || s.p[i] == '-')) i++;
        bool ed = false;
        for (; i < n && s.p[i] >= '0' && s.p[i] <= '9'; i++) ed = true;
        if (!ed) return false;
    } else if (hex) {
        return false;  // hex mantissa requires a p exponent
    }
    return i == n;
}

Ret_f64__GoIface strconv_ParseFloat(GoString s, int bits) {
    Ret_f64__GoIface r = {0};
    bool special;
    if (!float_syntax(s, &special)) {
        r.r1 = num_error("ParseFloat", s, "invalid syntax");
        return r;
    }
    char buf[512];
    int n = 0;
    for (int i = 0; i < s.len && n < (int)sizeof buf - 1; i++)
        if (s.p[i] != '_') buf[n++] = s.p[i];
    buf[n] = 0;
    errno = 0;
    if (bits == 32) {
        f32 f = strtof(buf, NULL);
        r.r0 = f;
    } else {
        r.r0 = strtod(buf, NULL);
    }
    if (!special && errno == ERANGE && (r.r0 > 1e300 || r.r0 < -1e300))
        r.r1 = num_error("ParseFloat", s, "value out of range");
    return r;
}

static bool parse_uint(GoString s, int base, int bits, u64 *out, GoIface *err, const char *fn, GoString orig) {
    int i = 0;
    bool underscores = false;
    if (s.len == 0) {
        *err = num_error(fn, orig, "invalid syntax");
        return false;
    }
    if (base == 0) {
        base = 10;
        if (s.p[0] == '0') {
            if (s.len >= 3 && (s.p[1] == 'x' || s.p[1] == 'X')) base = 16, i = 2;
            else if (s.len >= 3 && (s.p[1] == 'b' || s.p[1] == 'B')) base = 2, i = 2;
            else if (s.len >= 3 && (s.p[1] == 'o' || s.p[1] == 'O')) base = 8, i = 2;
            else base = 8, i = 1;
            underscores = true;
        }
    }
    u64 maxv = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    u64 v = 0;
    bool range = false, any = i > 0 && base == 8 && s.len == 1;
    if (i == s.len && i > 0 && !(base == 8 && i == 1)) {
        *err = num_error(fn, orig, "invalid syntax");
        return false;
    }
    for (; i < s.len; i++) {
        char c = s.p[i];
        int d;
        if (c == '_' && underscores) continue;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else {
            *err = num_error(fn, orig, "invalid syntax");
            return false;
        }
        if (d >= base) {
            *err = num_error(fn, orig, "invalid syntax");
            return false;
        }
        any = true;
        if (v > (maxv - (u64)d) / (u64)base) range = true;
        if (!range) v = v * (u64)base + (u64)d;
    }
    (void)any;
    if (range) {
        *out = maxv;
        *err = num_error(fn, orig, "value out of range");
        return false;
    }
    *out = v;
    return true;
}

Ret_u64__GoIface strconv_ParseUint(GoString s, int base, int bits) {
    Ret_u64__GoIface r = {0};
    if (bits == 0) bits = 64;
    parse_uint(s, base, bits, &r.r0, &r.r1, "ParseUint", s);
    return r;
}

Ret_i64__GoIface strconv_ParseInt(GoString s, int base, int bits) {
    Ret_i64__GoIface r = {0};
    if (bits == 0) bits = 64;
    if (s.len == 0) {
        r.r1 = num_error("ParseInt", s, "invalid syntax");
        return r;
    }
    bool neg = false;
    GoString t = s;
    if (t.p[0] == '+' || t.p[0] == '-') {
        neg = t.p[0] == '-';
        t.p++;
        t.len--;
    }
    u64 u = 0;
    GoIface err = {0};
    bool ok = parse_uint(t, base, 64, &u, &err, "ParseInt", s);
    u64 cutoff = 1ull << (bits - 1);
    if (!ok && go_error_string(err).len && strstr(go_strz(go_error_string(err)), "range") == NULL) {
        r.r1 = err;
        return r;
    }
    if (!ok || (!neg && u >= cutoff) || (neg && u > cutoff)) {
        r.r0 = neg ? -(i64)(cutoff - 1) - 1 : (i64)(cutoff - 1);
        r.r1 = num_error("ParseInt", s, "value out of range");
        return r;
    }
    r.r0 = neg ? (i64)(0 - u) : (i64)u;
    return r;
}

Ret_int__GoIface strconv_Atoi(GoString s) {
    Ret_i64__GoIface p = strconv_ParseInt(s, 10, 64);
    Ret_int__GoIface r = {(int)p.r0, {0}};
    if (p.r1.type) r.r1 = go_errorf("strconv.Atoi: parsing \"%.*s\": %s", s.len, s.p,
                                    strstr(go_strz(go_error_string(p.r1)), "range") ? "value out of range" : "invalid syntax");
    return r;
}

Ret_bool__GoIface strconv_ParseBool(GoString s) {
    static const char *t[] = {"1", "t", "T", "true", "TRUE", "True"};
    static const char *f[] = {"0", "f", "F", "false", "FALSE", "False"};
    for (int i = 0; i < 6; i++) {
        if (go_streq(s, go_cstr(t[i]))) return (Ret_bool__GoIface){true, {0}};
        if (go_streq(s, go_cstr(f[i]))) return (Ret_bool__GoIface){false, {0}};
    }
    return (Ret_bool__GoIface){false, num_error("ParseBool", s, "invalid syntax")};
}

Ret_GoString__GoIface strconv_Unquote(GoString s) {
    Ret_GoString__GoIface r = {0};
    if (s.len < 2 || s.p[0] != s.p[s.len - 1] || (s.p[0] != '"' && s.p[0] != '`' && s.p[0] != '\'')) {
        r.r1 = go_error(GOSTR("invalid syntax"));
        return r;
    }
    if (s.p[0] == '`') {
        r.r0 = (GoString){s.p + 1, s.len - 2};
        return r;
    }
    char *p = (char *)gc_alloc_atomic((size_t)s.len);
    int w = 0;
    for (int i = 1; i < s.len - 1; i++) {
        char c = s.p[i];
        if (c != '\\') {
            p[w++] = c;
            continue;
        }
        if (++i >= s.len - 1) break;
        switch (s.p[i]) {
        case 'n': p[w++] = '\n'; break;
        case 't': p[w++] = '\t'; break;
        case 'r': p[w++] = '\r'; break;
        case 'a': p[w++] = '\a'; break;
        case 'b': p[w++] = '\b'; break;
        case 'f': p[w++] = '\f'; break;
        case 'v': p[w++] = '\v'; break;
        case '\\': p[w++] = '\\'; break;
        case '"': p[w++] = '"'; break;
        case '\'': p[w++] = '\''; break;
        case 'x':
            if (i + 2 < s.len) {
                char h[3] = {s.p[i + 1], s.p[i + 2], 0};
                p[w++] = (char)strtol(h, NULL, 16);
                i += 2;
            }
            break;
        default:
            gc_free(p);
            r.r1 = go_error(GOSTR("invalid syntax"));
            return r;
        }
    }
    p[w] = 0;
    r.r0 = (GoString){p, w};
    return r;
}

// ---------------------------------------------------------------- math
f64 math_Pow10(int n) { return cvf_pow10(n); }
f64 math_Ldexp(f64 f, int e) { return ldexp(f, e); }  // exact (scaling by a power of two)
Ret_f64__int math_Frexp(f64 f) {
    int e;
    f64 m = frexp(f, &e);
    return (Ret_f64__int){m, e};
}
Ret_f64__f64 math_Modf(f64 f) {
    f64 ip;
    f64 fr = modf(f, &ip);
    return (Ret_f64__f64){ip, fr};
}
int bits_LeadingZeros64(u64 x) { return 64 - bits_len(x); }
Ret_u64__u64 bits_Add64(u64 x, u64 y, u64 carry) {
    u64 s = x + y + carry;
    u64 c = ((x & y) | ((x | y) & ~s)) >> 63;
    return (Ret_u64__u64){s, c};
}
Ret_u64__u64 bits_Mul64(u64 x, u64 y) {
    u64 x0 = x & 0xffffffff, x1 = x >> 32, y0 = y & 0xffffffff, y1 = y >> 32;
    u64 w0 = x0 * y0;
    u64 t = x1 * y0 + (w0 >> 32);
    u64 w1 = t & 0xffffffff, w2 = t >> 32;
    w1 += x0 * y1;
    u64 hi = x1 * y1 + w2 + (w1 >> 32);
    return (Ret_u64__u64){hi, x * y};
}

u32 crc32_ChecksumIEEE(Slice_u8 b) {
    static u32 table[256];
    static bool init;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    u32 c = 0xFFFFFFFFu;
    for (int i = 0; i < b.len; i++) c = table[(c ^ b.p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

GoOpaque binary_LittleEndian, binary_BigEndian;  // empty structs in Go

u32 binary_littleEndian_Uint32(GoOpaque recv, Slice_u8 b) {
    (void)recv;
    return (u32)b.p[0] | ((u32)b.p[1] << 8) | ((u32)b.p[2] << 16) | ((u32)b.p[3] << 24);
}
Slice_u8 binary_bigEndian_AppendUint32(GoOpaque recv, Slice_u8 b, u32 v) {
    (void)recv;
    b = go_append_Slice_u8(b, (u8)(v >> 24));
    b = go_append_Slice_u8(b, (u8)(v >> 16));
    b = go_append_Slice_u8(b, (u8)(v >> 8));
    return go_append_Slice_u8(b, (u8)v);
}

// ---------------------------------------------------------------- inert
// Single-threaded headless simulation: locks and pools are no-ops, time is
// frozen at zero (the simulation never reads the wall clock for state).
void sync_Mutex_Lock(GoOpaque *m) { (void)m; }
void sync_Mutex_Unlock(GoOpaque *m) { (void)m; }
void sync_RWMutex_RLock(GoOpaque *m) { (void)m; }
void sync_RWMutex_RUnlock(GoOpaque *m) { (void)m; }
void sync_Once_Do(GoOpaque *o, GoFunc f) {
    if (o->_opaque[0]) return;
    o->_opaque[0] = 1;
    ((void (*)(void *))f.fn)(f.env);
}
// sync.Pool keeps only its New function (no reuse: allocation reuse is
// not observable by the simulation).
GoOpaque go_sync_pool(GoFunc new_fn) {
    GoOpaque o = {{0}};
    memcpy(o._opaque, &new_fn, sizeof new_fn);
    return o;
}
GoIface sync_Pool_Get(GoOpaque *p) {
    GoFunc f;
    memcpy(&f, p->_opaque, sizeof f);
    if (!f.fn) return (GoIface){0};
    return ((GoIface(*)(void *))f.fn)(f.env);
}
void sync_Pool_Put(GoOpaque *p, GoIface x) { (void)p, (void)x; }
void runtime_Gosched(void) {}
GoOpaque time_Now(void) { return (GoOpaque){{0}}; }
GoOpaque time_Time_Add(GoOpaque t, i64 d) {
    (void)d;
    return t;
}
bool time_Time_After(GoOpaque a, GoOpaque b) {
    (void)a, (void)b;
    return false;
}
i64 time_Since(GoOpaque t) {
    (void)t;
    return 0;
}
i64 time_Time_Sub(GoOpaque a, GoOpaque b) {
    (void)a, (void)b;
    return 0;
}
i64 time_Time_UnixMilli(GoOpaque t) {
    (void)t;
    return 0;
}
i64 time_Time_UnixNano(GoOpaque t) {
    (void)t;
    return 0;
}
GoString time_Time_Format(GoOpaque t, GoString f) {
    (void)t, (void)f;
    return GOSTR("0000-00-00");
}
void time_Sleep(i64 d) { (void)d; }
GoOpaque *log_New(GoIface w, GoString prefix, int flags) {
    (void)w, (void)prefix, (void)flags;
    return (GoOpaque *)gc_calloc(1, sizeof(GoOpaque));
}

// Fields of opaque platform-library structs read and write a zeroed
// scratch slot (sound/video state the headless simulation does not model).
void *go_scratch(size_t n) {
    static _Alignas(16) u8 buf[4096];
    if (n > sizeof buf) n = sizeof buf;
    memset(buf, 0, n);
    return buf;
}

bool strings_ContainsAny(GoString s, GoString chars) {
    for (int i = 0; i < s.len;) {
        i32 r;
        i += utf8_decode(s.p + i, s.len - i, &r);
        if (strings_ContainsRune(chars, r)) return true;
    }
    return false;
}
void sync_RWMutex_Lock(GoOpaque *m) { (void)m; }
void sync_RWMutex_Unlock(GoOpaque *m) { (void)m; }

Ret_GoString__GoString__bool strings_Cut(GoString s, GoString sep) {
    int i = strings_Index(s, sep);
    if (i >= 0) return (Ret_GoString__GoString__bool){{s.p, i}, {s.p + i + sep.len, s.len - i - sep.len}, true};
    return (Ret_GoString__GoString__bool){s, {"", 0}, false};
}

// fmt.Sscan into pointers to numbers/strings (space separated operands).
extern const RType *const ik_rt_by_tid[];
extern const int ik_rt_by_tid_count;

static bool scan_store(GoIface dst, GoString tok) {
    if (dst.type <= 0 || dst.type >= ik_rt_by_tid_count || !ik_rt_by_tid[dst.type]) return false;
    const RType *t = ik_rt_by_tid[dst.type];
    if (t->kind != 22 || !t->elem) return false;  // pointer
    void *p = dst.data;
    switch (t->elem->kind) {
    case 2: case 3: case 4: case 5: case 6: {
        Ret_i64__GoIface r = strconv_ParseInt(tok, 10, 64);
        if (r.r1.type) return false;
        switch (t->elem->kind) {
        case 2: *(int *)p = (int)r.r0; break;
        case 3: *(i8 *)p = (i8)r.r0; break;
        case 4: *(i16 *)p = (i16)r.r0; break;
        case 5: *(i32 *)p = (i32)r.r0; break;
        default: *(i64 *)p = r.r0; break;
        }
        return true;
    }
    case 13: case 14: {
        Ret_f64__GoIface r = strconv_ParseFloat(tok, t->elem->kind == 13 ? 32 : 64);
        if (r.r1.type) return false;
        if (t->elem->kind == 13) *(f32 *)p = (f32)r.r0;
        else *(f64 *)p = r.r0;
        return true;
    }
    case 24: *(GoString *)p = tok; return true;
    case 1: {
        Ret_bool__GoIface r = strconv_ParseBool(tok);
        if (r.r1.type) return false;
        *(bool *)p = r.r0;
        return true;
    }
    }
    return false;
}

Ret_int__GoIface fmt_Sscan(GoString str, Slice_GoIface args) {
    Ret_int__GoIface r = {0};
    Slice_GoString f = strings_Fields(str);
    for (int i = 0; i < args.len; i++) {
        if (i >= f.len) {
            r.r1 = go_error(GOSTR("unexpected EOF"));
            return r;
        }
        if (!scan_store(args.p[i], f.p[i])) {
            r.r1 = go_error(GOSTR("expected integer"));
            return r;
        }
        r.r0++;
    }
    return r;
}

// fmt.Sscanf for verb-only formats with literal separators ("%d-%d").
Ret_int__GoIface fmt_Sscanf(GoString str, GoString format, Slice_GoIface args) {
    Ret_int__GoIface r = {0};
    int si = 0, ai = 0;
    for (int fi = 0; fi < format.len; fi++) {
        if (format.p[fi] != '%' || fi + 1 >= format.len) {
            if (si >= str.len || str.p[si] != format.p[fi]) {
                r.r1 = go_error(GOSTR("input does not match format"));
                return r;
            }
            si++;
            continue;
        }
        fi++;
        int st = si;
        if (si < str.len && (str.p[si] == '-' || str.p[si] == '+')) si++;
        while (si < str.len && ((str.p[si] >= '0' && str.p[si] <= '9') || str.p[si] == '.' || str.p[si] == 'e' || str.p[si] == 'E'))
            si++;
        if (ai >= args.len || !scan_store(args.p[ai], (GoString){str.p + st, si - st})) {
            r.r1 = go_error(GOSTR("bad input"));
            return r;
        }
        ai++;
        r.r0++;
    }
    return r;
}

GoIface strings_Builder_WriteByte(GoOpaque *recv, u8 c) {
    char ch = (char)c;
    strings_Builder_WriteString(recv, (GoString){&ch, 1});
    return (GoIface){0};
}

int utf8_RuneCountInString(GoString s) {
    int n = 0;
    for (int i = 0; i < s.len; n++) {
        i32 r;
        i += go_decode_rune(s, i, &r);
    }
    return n;
}

// ---------------------------------------------------------------- sort.Sort
// Only sort.StringSlice (optionally wrapped in sort.Reverse) reaches sort.Sort
// in the translated code; other Interface implementations panic loudly.
enum { GO_TID_SORT_REVERSE = 96 };
GoIface sort_Reverse(GoIface data) { return (GoIface){GO_TID_SORT_REVERSE, go_box(GoIface, data)}; }
int sort_StringSlice_Len(Slice_GoString recv) { return recv.len; }

typedef struct { Slice_GoString *s; bool rev; } StrLessEnv;
static bool str_less(void *env, int i, int j) {
    StrLessEnv *e = (StrLessEnv *)env;
    GoString a = e->s->p[i], b = e->s->p[j];
    if (e->rev) { GoString t = a; a = b; b = t; }
    int n = a.len < b.len ? a.len : b.len;
    int c = memcmp(a.p, b.p, (size_t)n);
    return c < 0 || (c == 0 && a.len < b.len);
}
void sort_Sort(GoIface data) {
    bool rev = false;
    if (data.type == GO_TID_SORT_REVERSE) {
        rev = true;
        data = *(GoIface *)data.data;
    }
    if (data.type != GO_TID_sort_StringSlice) go_panic_str("sort.Sort: unsupported sort.Interface implementation");
    StrLessEnv env = {(Slice_GoString *)data.data, rev};
    go_sort_slice(env.s, sizeof(GoString), (GoFunc){(void *)str_less, &env});
}
