#include "core/gort.h"

#include <stdio.h>
#include <string.h>

GoString go_cstr(const char *s) { return (GoString){s, s ? (int)strlen(s) : 0}; }

GoString go_cstr_copy(const char *s) {
    int n = s ? (int)strlen(s) : 0;
    if (!n) return (GoString){0};
    char *p = (char *)gc_alloc_atomic((size_t)n + 1);
    memcpy(p, s, (size_t)n + 1);
    return (GoString){p, n};
}

bool go_streq(GoString a, GoString b) { return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0); }

int go_strcmp(GoString a, GoString b) {
    int n = a.len < b.len ? a.len : b.len;
    int c = n ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c) return c;
    return a.len < b.len ? -1 : a.len > b.len;
}

GoString go_strcat(GoString a, GoString b) {
    if (!a.len) return b;
    if (!b.len) return a;
    char *p = (char *)gc_alloc_atomic((size_t)(a.len + b.len + 1));
    memcpy(p, a.p, (size_t)a.len);
    memcpy(p + a.len, b.p, (size_t)b.len);
    p[a.len + b.len] = 0;
    return (GoString){p, a.len + b.len};
}

GoString go_substr(GoString s, int lo, int hi) {
    if (hi < 0) hi = s.len;
    return (GoString){s.p + lo, hi - lo};
}

GoString go_rune2str(i32 r) {
    char *p = (char *)gc_alloc_atomic(5);
    int n = 0;
    if (r < 0 || r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF)) r = 0xFFFD;
    if (r < 0x80) p[n++] = (char)r;
    else if (r < 0x800) { p[n++] = (char)(0xC0 | (r >> 6)); p[n++] = (char)(0x80 | (r & 0x3F)); }
    else if (r < 0x10000) { p[n++] = (char)(0xE0 | (r >> 12)); p[n++] = (char)(0x80 | ((r >> 6) & 0x3F)); p[n++] = (char)(0x80 | (r & 0x3F)); }
    else { p[n++] = (char)(0xF0 | (r >> 18)); p[n++] = (char)(0x80 | ((r >> 12) & 0x3F)); p[n++] = (char)(0x80 | ((r >> 6) & 0x3F)); p[n++] = (char)(0x80 | (r & 0x3F)); }
    p[n] = 0;
    return (GoString){p, n};
}

int go_decode_rune(GoString s, int i, i32 *r) {
    const u8 *p = (const u8 *)s.p + i;
    int n = s.len - i;
    u8 c = p[0];
    if (c < 0x80) {
        *r = c;
        return 1;
    }
    int len = 0;
    i32 v = 0, min = 0;
    if (c >= 0xC2 && c <= 0xDF) len = 2, v = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) len = 3, v = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) len = 4, v = c & 0x07, min = 0x10000;
    if (!len || len > n) {
        *r = 0xFFFD;
        return 1;
    }
    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *r = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (p[k] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *r = 0xFFFD;
        return 1;
    }
    *r = v;
    return len;
}

// Short strings use 8 rotating static buffers (no allocation); a string of
// 1024 bytes or more gets its own GC-allocated copy instead of being
// truncated (long paths opened the wrong file, ik_io.c read past the copy).
const char *go_strz(GoString s) {
    static char bufs[8][1024];
    static int k;
    int n = s.len > 0 ? s.len : 0;
    char *b = n < (int)sizeof bufs[0] ? bufs[k++ & 7] : (char *)gc_alloc_atomic((size_t)n + 1);
    if (n) memcpy(b, s.p, (size_t)n);
    b[n] = 0;
    return b;
}

static inline void *slice_grow(void *p, int len, int *cap, int need, size_t elem, bool atomic) {
    int nc = *cap;
    // Go's growth: double small slices, 1.25x large ones (exact capacity
    // only matters for aliasing, which follows from the same rule).
    if (nc < 256) nc = nc ? nc * 2 : 1;
    else nc += (nc + 3 * 256) / 4;
    if (nc < need) nc = need;
    void *np = atomic ? gc_alloc_atomic(elem * (size_t)nc) : gc_alloc(elem * (size_t)nc);  // zeroed
    if (len) memcpy(np, p, elem * (size_t)len);
    *cap = nc;
    return np;  // old storage stays valid for any alias until unreachable
}
void *go_slice_grow(void *p, int len, int *cap, int need, size_t elem) { return slice_grow(p, len, cap, need, elem, false); }
void *go_slice_grow_atomic(void *p, int len, int *cap, int need, size_t elem) {
    return slice_grow(p, len, cap, need, elem, true);
}

int go_copy_impl(void *dst, int dlen, const void *src, int slen, size_t elem) {
    int n = dlen < slen ? dlen : slen;
    if (n > 0) memmove(dst, src, elem * (size_t)n);
    return n;
}

// ---------------------------------------------------------------- maps
typedef struct GoMapImpl {
    size_t ksize, vsize;
    int keykind;  // 0 bytes, 1 string, 2 interface
    int n, cap;      // entries (including deleted)
    int live;
    int freelist;    // deleted entry to reuse + 1 (0: none); chained through the hash field
    u8 *ents;        // n entries of (u8 used, key, value), stride
    size_t stride;
    i32 *slots;      // hash -> entry index + 1
    int nslots;
} GoMapImpl;

static u64 hash_bytes(const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static u64 key_hash(GoMapImpl *m, const void *k) {
    if (m->keykind == 1) {
        const GoString *s = (const GoString *)k;
        return hash_bytes(s->p, (size_t)s->len);
    }
    if (m->keykind == 2) return go_iface_hash(*(const GoIface *)k);
    return hash_bytes(k, m->ksize);
}

static bool key_eq(GoMapImpl *m, const void *a, const void *b) {
    if (m->keykind == 1) return go_streq(*(const GoString *)a, *(const GoString *)b);
    if (m->keykind == 2) return go_iface_eq(*(const GoIface *)a, *(const GoIface *)b);
    return memcmp(a, b, m->ksize) == 0;
}

#define ENT(m, i) ((m)->ents + (size_t)(i) * (m)->stride)
#define ENT_HASH(m, i) (*(u32 *)(ENT(m, i) + 4))  // header: [0] used, [4..8) hash
#define ENT_KEY(m, i) (ENT(m, i) + 8)
#define ENT_VAL(m, i) (ENT(m, i) + 8 + (((m)->ksize + 7) & ~(size_t)7))

static void rehash(GoMapImpl *m, int nslots) {
    if (nslots != m->nslots) {
        m->nslots = nslots;
        m->slots = (i32 *)gc_calloc_atomic((size_t)nslots, sizeof(i32));
    } else {
        memset(m->slots, 0, sizeof(i32) * (size_t)nslots);
    }
    for (int i = 0; i < m->n; i++) {
        if (!ENT(m, i)[0]) continue;
        int s = (int)(ENT_HASH(m, i) & (u32)(nslots - 1));
        while (m->slots[s]) s = (s + 1) & (nslots - 1);
        m->slots[s] = i + 1;
    }
}

GoMap gomap_make(size_t ksize, size_t vsize, int keykind) {
    GoMapImpl *m = (GoMapImpl *)gc_calloc(1, sizeof *m);
    m->ksize = ksize;
    m->vsize = vsize;
    m->keykind = keykind;
    m->stride = 8 + ((ksize + 7) & ~(size_t)7) + ((vsize + 7) & ~(size_t)7);
    return (GoMap){m};
}

static int find_slot_h(GoMapImpl *m, const void *key, u32 h) {
    int s = (int)(h & (u32)(m->nslots - 1));
    while (m->slots[s]) {
        int e = m->slots[s] - 1;
        if (ENT_HASH(m, e) == h && key_eq(m, ENT_KEY(m, e), key)) return s;
        s = (s + 1) & (m->nslots - 1);
    }
    return -1;
}

static int find_slot(GoMapImpl *m, const void *key) {
    if (!m || !m->nslots) return -1;
    return find_slot_h(m, key, (u32)key_hash(m, key));
}

void *gomap_find(GoMap *gm, const void *key) {
    int s = find_slot(gm->m, key);
    return s < 0 ? NULL : ENT_VAL(gm->m, gm->m->slots[s] - 1);
}

void *gomap_put(GoMap *gm, const void *key, size_t ksize, size_t vsize, int keykind) {
    if (!gm->m) *gm = gomap_make(ksize, vsize, keykind);
    GoMapImpl *m = gm->m;
    u32 h = (u32)key_hash(m, key);
    if (m->nslots) {
        int s = find_slot_h(m, key, h);
        if (s >= 0) return ENT_VAL(m, m->slots[s] - 1);
    }
    if (m->live == 0 && m->n) {
        // emptied map: start the entry array over (live entries keep
        // insertion order; no entry exists to be visited by an iterator)
        m->n = 0;
        m->freelist = 0;
        memset(m->slots, 0, sizeof(i32) * (size_t)m->nslots);
    }
    int e;
    if (m->freelist) {
        // reuse a deleted entry (most recently deleted first): the entry
        // array stays as large as the most entries the map ever held, so a
        // map with rotating keys (rollback save states) does not grow. Live
        // entries never move, so an iteration in progress is unaffected; a
        // new entry may or may not be visited by it, as in Go.
        e = m->freelist - 1;
        m->freelist = (int)ENT_HASH(m, e);
    } else {
        if (m->n == m->cap) {
            m->cap = m->cap ? m->cap * 2 : 8;
            m->ents = (u8 *)gc_realloc(m->ents, m->stride * (size_t)m->cap);
        }
        e = m->n++;
    }
    memset(ENT(m, e), 0, m->stride);
    ENT(m, e)[0] = 1;
    ENT_HASH(m, e) = h;
    memcpy(ENT_KEY(m, e), key, m->ksize);
    m->live++;
    if (m->n * 2 >= m->nslots) rehash(m, m->nslots ? m->nslots * 2 : 16);
    else {
        int ss = (int)(h & (u32)(m->nslots - 1));
        while (m->slots[ss]) ss = (ss + 1) & (m->nslots - 1);
        m->slots[ss] = e + 1;
    }
    return ENT_VAL(m, e);
}

void gomap_del(GoMap *gm, const void *key) {
    GoMapImpl *m = gm->m;
    int i = find_slot(m, key);
    if (i < 0) return;
    int de = m->slots[i] - 1;
    // the deleted key and value must not keep their referents alive (the
    // collector scans the entry array); the entry joins the free list
    memset(ENT(m, de), 0, m->stride);
    m->live--;
    // backward-shift deletion: keep every probe chain unbroken, no tombstones
    int mask = m->nslots - 1;
    m->slots[i] = 0;
    for (int j = (i + 1) & mask; m->slots[j]; j = (j + 1) & mask) {
        int k = (int)(ENT_HASH(m, m->slots[j] - 1) & (u32)mask);
        // the entry at j may move to the hole at i unless its home k lies cyclically in (i, j]
        bool stay = i <= j ? (i < k && k <= j) : (i < k || k <= j);
        if (!stay) {
            m->slots[i] = m->slots[j];
            m->slots[j] = 0;
            i = j;
        }
    }
    ENT_HASH(m, de) = (u32)m->freelist;
    m->freelist = de + 1;
}

void gomap_clear(GoMap *gm) {
    GoMapImpl *m = gm->m;
    if (!m) return;
    m->n = 0;
    m->live = 0;
    m->freelist = 0;
    if (m->slots) memset(m->slots, 0, sizeof(i32) * (size_t)m->nslots);
}

int gomap_len(const GoMap *gm) { return gm->m ? gm->m->live : 0; }

GoMapIter gomap_iter(GoMap *gm) { return (GoMapIter){gm->m, -1, NULL, NULL}; }

bool gomap_next(GoMapIter *it) {
    if (!it->m) return false;
    for (it->i++; it->i < it->m->n; it->i++) {
        if (ENT(it->m, it->i)[0]) {
            it->key = ENT_KEY(it->m, it->i);
            it->val = ENT_VAL(it->m, it->i);
            return true;
        }
    }
    return false;
}

void *go_box_atomic_impl(const void *v, size_t n) {
    void *p = gc_alloc_atomic(n ? n : 1);
    memcpy(p, v, n);
    return p;
}

void *go_box_impl(const void *v, size_t n) {
    void *p = gc_alloc(n ? n : 1);
    memcpy(p, v, n);
    return p;
}

GoPanicFrame *go_panic_top;
GoIface go_panic_val;
bool go_panicking;
static GoString panic_desc(GoIface v);

void go_panic_raise(GoIface v) {
    go_panic_val = v;
    go_panicking = true;
    go_panic_continue();
}
void go_panic_continue(void) {
    GoPanicFrame *f = go_panic_top;
    if (f) {
        go_panic_top = f->prev;
        longjmp(f->jb, 1);
    }
    GoString d = panic_desc(go_panic_val);
    cvf_fatal("go panic: %.*s", (int)d.len, d.p);
}
GoIface go_recover(void) {
    if (!go_panicking) return (GoIface){0};
    go_panicking = false;
    GoIface v = go_panic_val;
    go_panic_val = (GoIface){0};
    return v;
}
// GO_TID_string = 17, runtime errors = 99 (ik_rt.h)
static GoString panic_desc(GoIface v) {
    if (v.type == 17 && v.data) return *(GoString *)v.data;
    if (v.type == 99 && v.data) return *(GoString *)v.data;
    return (GoString){"(non-string panic value)", 24};
}
static GoIface panic_box_str(GoString m) {
    GoString *p = (GoString *)gc_alloc(sizeof *p);
    *p = m;
    return (GoIface){17, p};
}
void go_panic_str(const char *msg) { go_panic_raise(panic_box_str((GoString){msg, (int)strlen(msg)})); }
void go_panic_gostr(GoString m) { go_panic_raise(panic_box_str(m)); }
void go_panic_s(int u, GoString m) { (void)u; go_panic_gostr(m); }
void go_panic_other(int u, ...) { (void)u; go_panic_str("panic"); }

f64 go_nan(void) {
    u64 u = 0x7FF8000000000001ull;
    f64 f;
    memcpy(&f, &u, 8);
    return f;
}

GoMap gomap_clone(GoMap *gm) {
    GoMapImpl *m = gm->m;
    if (!m) return (GoMap){0};
    GoMapImpl *c = (GoMapImpl *)gc_alloc(sizeof *c);
    *c = *m;
    c->ents = (u8 *)gc_alloc(m->stride * (size_t)(m->cap ? m->cap : 1));
    memcpy(c->ents, m->ents, m->stride * (size_t)m->n);
    if (m->nslots) {
        c->slots = (i32 *)gc_alloc_atomic(sizeof(i32) * (size_t)m->nslots);
        memcpy(c->slots, m->slots, sizeof(i32) * (size_t)m->nslots);
    }
    return (GoMap){c};
}

GoString go_runes2str(const i32 *r, int n) {  // string([]rune)
    char *p = (char *)gc_alloc_atomic((size_t)n * 4 + 1);
    int w = 0;
    for (int i = 0; i < n; i++) {
        GoString e = go_rune2str(r[i]);
        memcpy(p + w, e.p, (size_t)e.len);
        w += e.len;
    }
    return (GoString){p, w};
}

// Programs without the generated type tables (unit tests, tools) get
// byte-wise fallbacks; ik_rt.c provides the real definitions.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) bool go_iface_eq(GoIface a, GoIface b) { return a.type == b.type && a.data == b.data; }
__attribute__((weak)) u64 go_iface_hash(GoIface a) { return hash_bytes(&a, sizeof a); }
#endif
