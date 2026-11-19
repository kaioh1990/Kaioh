// reflect for the translated simulation, over the type descriptors gotoc
// generates (ik_rt[], indexed from interface type ids by ik_rt_by_tid[]).
// Covers what Ikemen's INI-driven struct population (iniutils.go,
// config.go) uses: Value navigation and mutation, maps, slices, struct
// fields and tags, New/Zero/MakeMap/MakeSlice/Append, DeepEqual.
//
// A reflect.Value lives in the GoOpaque storage of the Go value as RVal:
// the type, a pointer to the value's memory, and flags.
#include "ik/ik_sim.h"

extern const RType ik_rt[];
extern const int ik_rt_count;
extern const RType *const ik_rt_by_tid[];
extern const int ik_rt_by_tid_count;

enum { K_INVALID, K_BOOL, K_INT, K_INT8, K_INT16, K_INT32, K_INT64, K_UINT, K_UINT8, K_UINT16, K_UINT32, K_UINT64,
       K_UINTPTR, K_FLOAT32, K_FLOAT64, K_COMPLEX64, K_COMPLEX128, K_ARRAY, K_CHAN, K_FUNC, K_INTERFACE, K_MAP,
       K_PTR, K_SLICE, K_STRING, K_STRUCT, K_UNSAFEPTR };

enum { F_ADDR = 1, F_RO = 2 };

typedef struct RVal {
    const RType *t;
    void *ptr;
    u32 flags;
} RVal;
_Static_assert(sizeof(RVal) <= sizeof(GoOpaque), "reflect.Value fits");

static GoOpaque pack(RVal v) {
    GoOpaque o = {{0}};
    memcpy(o._opaque, &v, sizeof v);
    return o;
}
static RVal unpack(GoOpaque o) {
    RVal v;
    memcpy(&v, o._opaque, sizeof v);
    return v;
}
static const RVal INVALID = {NULL, NULL, 0};

static const RType *by_tid(int tid) {
    if (tid > 0 && tid < ik_rt_by_tid_count) return ik_rt_by_tid[tid];
    return NULL;
}

static bool ptr_shaped(const RType *t) {
    return t->kind == K_PTR || t->kind == K_UNSAFEPTR;  // stored directly in interface data
}

// Descriptors the program never boxed (e.g. *T for reflect.New(T)) are
// synthesized on demand and cached.
static const RType *ptr_to(const RType *elem) {
    for (int i = 0; i < ik_rt_count; i++)
        if (ik_rt[i].kind == K_PTR && ik_rt[i].elem == elem) return &ik_rt[i];
    static RType *extra[256];
    static int nextra;
    for (int i = 0; i < nextra; i++)
        if (extra[i]->elem == elem) return extra[i];
    RType *t = (RType *)gc_calloc(1, sizeof *t);
    t->kind = K_PTR;
    t->size = sizeof(void *);
    t->elem = elem;
    t->name = "";
    t->pkgpath = "";
    t->str = "*?";
    if (nextra < 256) extra[nextra++] = t;
    return t;
}

static const RType *basic(int kind) {
    for (int i = 0; i < ik_rt_count; i++)
        if (ik_rt[i].kind == kind && ik_rt[i].pkgpath[0] == 0 && ik_rt[i].name[0]) return &ik_rt[i];
    return NULL;
}

static GoIface box(const RType *t, const void *p) {
    if (!t) return (GoIface){0};
    if (t->kind == K_INTERFACE) return *(const GoIface *)p;
    if (ptr_shaped(t)) return (GoIface){t->tid, *(void *const *)p};
    void *c = gc_alloc(t->size ? t->size : 1);
    memcpy(c, p, t->size);
    return (GoIface){t->tid, c};
}

static RVal value_of(GoIface i) {
    if (i.type == 0) return INVALID;
    const RType *t = by_tid(i.type);
    if (!t) return INVALID;
    if (ptr_shaped(t)) {
        void **cell = (void **)gc_alloc(sizeof(void *));
        *cell = i.data;
        return (RVal){t, cell, 0};
    }
    return (RVal){t, i.data, 0};
}

// ---------------------------------------------------------------- Value
GoOpaque reflect_ValueOf(GoIface a0) { return pack(value_of(a0)); }

uintptr_t reflect_Value_Kind(GoOpaque recv) {
    RVal v = unpack(recv);
    return v.t ? v.t->kind : K_INVALID;
}

bool reflect_Value_IsValid(GoOpaque recv) { return unpack(recv).t != NULL; }

GoIface reflect_Value_Type(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!v.t) go_panic_str("reflect: call of reflect.Value.Type on zero Value");
    return (GoIface){GO_TID_RTYPE, (void *)v.t};
}

GoOpaque reflect_Value_Elem(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!v.t) return pack(INVALID);
    if (v.t->kind == K_PTR) {
        void *p = *(void **)v.ptr;
        if (!p) return pack(INVALID);
        return pack((RVal){v.t->elem, p, F_ADDR | (v.flags & F_RO)});
    }
    if (v.t->kind == K_INTERFACE) {
        RVal e = value_of(*(GoIface *)v.ptr);
        e.flags |= v.flags & F_RO;
        return pack(e);
    }
    go_panic_str("reflect: call of reflect.Value.Elem on wrong kind");
}

GoOpaque reflect_Value_Field(GoOpaque recv, int i) {
    RVal v = unpack(recv);
    if (!v.t || v.t->kind != K_STRUCT || i < 0 || i >= v.t->nfield) go_panic_str("reflect: Field index out of range");
    const RField *f = &v.t->fields[i];
    u32 fl = v.flags & F_ADDR;
    if ((v.flags & F_RO) || (f->pkgpath && f->pkgpath[0])) fl |= F_RO;
    return pack((RVal){f->type, (char *)v.ptr + f->offset, fl});
}

int reflect_Value_NumField(GoOpaque recv) {
    RVal v = unpack(recv);
    return v.t ? v.t->nfield : 0;
}

// FieldByName: direct fields first, then promoted ones (breadth first).
static bool find_field(const RType *t, GoString name, int depth, size_t *off, const RType **ft, bool *ro) {
    for (int i = 0; i < t->nfield; i++) {
        const RField *f = &t->fields[i];
        if ((int)strlen(f->name) == name.len && memcmp(f->name, name.p, (size_t)name.len) == 0) {
            *off = f->offset;
            *ft = f->type;
            *ro = f->pkgpath && f->pkgpath[0];
            return true;
        }
    }
    if (depth > 8) return false;
    for (int i = 0; i < t->nfield; i++) {
        const RField *f = &t->fields[i];
        if (f->anonymous && f->type && f->type->kind == K_STRUCT) {
            size_t o;
            if (find_field(f->type, name, depth + 1, &o, ft, ro)) {
                *off = f->offset + o;
                return true;
            }
        }
    }
    return false;
}

GoOpaque reflect_Value_FieldByName(GoOpaque recv, GoString name) {
    RVal v = unpack(recv);
    if (!v.t || v.t->kind != K_STRUCT) go_panic_str("reflect: FieldByName on non-struct");
    size_t off;
    const RType *ft;
    bool ro;
    if (!find_field(v.t, name, 0, &off, &ft, &ro)) return pack(INVALID);
    return pack((RVal){ft, (char *)v.ptr + off, (v.flags & F_ADDR) | ((v.flags & F_RO) || ro ? F_RO : 0)});
}

GoOpaque reflect_Value_Index(GoOpaque recv, int i) {
    RVal v = unpack(recv);
    switch (v.t ? v.t->kind : 0) {
    case K_SLICE: {
        GoSliceHdr *h = (GoSliceHdr *)v.ptr;
        if (i < 0 || i >= h->len) go_panic_str("reflect: slice index out of range");
        return pack((RVal){v.t->elem, (char *)h->p + (size_t)i * v.t->elem->size, F_ADDR | (v.flags & F_RO)});
    }
    case K_ARRAY:
        if (i < 0 || i >= v.t->len) go_panic_str("reflect: array index out of range");
        return pack((RVal){v.t->elem, (char *)v.ptr + (size_t)i * v.t->elem->size, v.flags});
    case K_STRING: {
        GoString s = *(GoString *)v.ptr;
        if (i < 0 || i >= s.len) go_panic_str("reflect: string index out of range");
        u8 *c = (u8 *)gc_alloc(1);
        *c = (u8)s.p[i];
        return pack((RVal){basic(K_UINT8), c, 0});
    }
    }
    go_panic_str("reflect: call of reflect.Value.Index on wrong kind");
}

int reflect_Value_Len(GoOpaque recv) {
    RVal v = unpack(recv);
    switch (v.t ? v.t->kind : 0) {
    case K_SLICE: return ((GoSliceHdr *)v.ptr)->len;
    case K_ARRAY: return v.t->len;
    case K_STRING: return ((GoString *)v.ptr)->len;
    case K_MAP: return gomap_len((GoMap *)v.ptr);
    }
    go_panic_str("reflect: call of reflect.Value.Len on wrong kind");
}

bool reflect_Value_IsNil(GoOpaque recv) {
    RVal v = unpack(recv);
    switch (v.t ? v.t->kind : 0) {
    case K_PTR:
    case K_UNSAFEPTR:
    case K_CHAN: return *(void **)v.ptr == NULL;
    case K_MAP: return ((GoMap *)v.ptr)->m == NULL;
    case K_SLICE: return ((GoSliceHdr *)v.ptr)->p == NULL;
    case K_FUNC: return ((GoFunc *)v.ptr)->fn == NULL;
    case K_INTERFACE: return ((GoIface *)v.ptr)->type == 0;
    }
    go_panic_str("reflect: call of reflect.Value.IsNil on wrong kind");
}

bool reflect_Value_CanSet(GoOpaque recv) {
    RVal v = unpack(recv);
    return v.t && (v.flags & F_ADDR) && !(v.flags & F_RO);
}

GoIface reflect_Value_Interface(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!v.t) go_panic_str("reflect: call of reflect.Value.Interface on zero Value");
    return box(v.t, v.ptr);
}

i64 reflect_Value_Int(GoOpaque recv) {
    RVal v = unpack(recv);
    switch (v.t ? v.t->kind : 0) {
    case K_INT: return *(int *)v.ptr;
    case K_INT8: return *(i8 *)v.ptr;
    case K_INT16: return *(i16 *)v.ptr;
    case K_INT32: return *(i32 *)v.ptr;
    case K_INT64: return *(i64 *)v.ptr;
    }
    go_panic_str("reflect: call of reflect.Value.Int on wrong kind");
}

static u64 get_uint(RVal v) {
    switch (v.t ? v.t->kind : 0) {
    case K_UINT:
    case K_UINTPTR: return *(uintptr_t *)v.ptr;
    case K_UINT8: return *(u8 *)v.ptr;
    case K_UINT16: return *(u16 *)v.ptr;
    case K_UINT32: return *(u32 *)v.ptr;
    case K_UINT64: return *(u64 *)v.ptr;
    }
    go_panic_str("reflect: call of reflect.Value.Uint on wrong kind");
}
u64 reflect_Value_Uint(GoOpaque recv) { return get_uint(unpack(recv)); }

f64 reflect_Value_Float(GoOpaque recv) {
    RVal v = unpack(recv);
    if (v.t && v.t->kind == K_FLOAT32) return *(f32 *)v.ptr;
    if (v.t && v.t->kind == K_FLOAT64) return *(f64 *)v.ptr;
    go_panic_str("reflect: call of reflect.Value.Float on wrong kind");
}

bool reflect_Value_Bool(GoOpaque recv) {
    RVal v = unpack(recv);
    if (v.t && v.t->kind == K_BOOL) return *(bool *)v.ptr;
    go_panic_str("reflect: call of reflect.Value.Bool on wrong kind");
}

GoString reflect_Value_String(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!v.t) return GOSTR("<invalid Value>");
    if (v.t->kind == K_STRING) return *(GoString *)v.ptr;
    return go_strcat(go_strcat(GOSTR("<"), go_cstr(v.t->str)), GOSTR(" Value>"));
}

static void must_set(RVal v) {
    if (!v.t || !(v.flags & F_ADDR) || (v.flags & F_RO)) go_panic_str("reflect: value is not settable");
}

void reflect_Value_SetInt(GoOpaque recv, i64 x) {
    RVal v = unpack(recv);
    must_set(v);
    switch (v.t->kind) {
    case K_INT: *(int *)v.ptr = (int)x; return;
    case K_INT8: *(i8 *)v.ptr = (i8)x; return;
    case K_INT16: *(i16 *)v.ptr = (i16)x; return;
    case K_INT32: *(i32 *)v.ptr = (i32)x; return;
    case K_INT64: *(i64 *)v.ptr = x; return;
    }
    go_panic_str("reflect: SetInt on wrong kind");
}

void reflect_Value_SetUint(GoOpaque recv, u64 x) {
    RVal v = unpack(recv);
    must_set(v);
    switch (v.t->kind) {
    case K_UINT:
    case K_UINTPTR: *(uintptr_t *)v.ptr = (uintptr_t)x; return;
    case K_UINT8: *(u8 *)v.ptr = (u8)x; return;
    case K_UINT16: *(u16 *)v.ptr = (u16)x; return;
    case K_UINT32: *(u32 *)v.ptr = (u32)x; return;
    case K_UINT64: *(u64 *)v.ptr = x; return;
    }
    go_panic_str("reflect: SetUint on wrong kind");
}

void reflect_Value_SetFloat(GoOpaque recv, f64 x) {
    RVal v = unpack(recv);
    must_set(v);
    if (v.t->kind == K_FLOAT32) *(f32 *)v.ptr = (f32)x;
    else if (v.t->kind == K_FLOAT64) *(f64 *)v.ptr = x;
    else go_panic_str("reflect: SetFloat on wrong kind");
}

void reflect_Value_SetBool(GoOpaque recv, bool x) {
    RVal v = unpack(recv);
    must_set(v);
    if (v.t->kind != K_BOOL) go_panic_str("reflect: SetBool on wrong kind");
    *(bool *)v.ptr = x;
}

void reflect_Value_SetString(GoOpaque recv, GoString x) {
    RVal v = unpack(recv);
    must_set(v);
    if (v.t->kind != K_STRING) go_panic_str("reflect: SetString on wrong kind");
    *(GoString *)v.ptr = x;
}

void reflect_Value_Set(GoOpaque recv, GoOpaque a0) {
    RVal v = unpack(recv), x = unpack(a0);
    must_set(v);
    if (!x.t) go_panic_str("reflect: Set with zero Value");
    if (v.t->kind == K_INTERFACE && x.t->kind != K_INTERFACE) {
        *(GoIface *)v.ptr = box(x.t, x.ptr);
        return;
    }
    memmove(v.ptr, x.ptr, v.t->size);
}

// ---------------------------------------------------------------- maps
GoOpaque reflect_Value_MapIndex(GoOpaque recv, GoOpaque key) {
    RVal v = unpack(recv), k = unpack(key);
    if (!v.t || v.t->kind != K_MAP) go_panic_str("reflect: MapIndex on non-map");
    void *e = gomap_find((GoMap *)v.ptr, k.ptr);
    if (!e) return pack(INVALID);
    void *c = gc_alloc(v.t->elem->size ? v.t->elem->size : 1);
    memcpy(c, e, v.t->elem->size);
    return pack((RVal){v.t->elem, c, v.flags & F_RO});
}

void reflect_Value_SetMapIndex(GoOpaque recv, GoOpaque key, GoOpaque elem) {
    RVal v = unpack(recv), k = unpack(key), e = unpack(elem);
    if (!v.t || v.t->kind != K_MAP) go_panic_str("reflect: SetMapIndex on non-map");
    if (!e.t) {
        gomap_del((GoMap *)v.ptr, k.ptr);
        return;
    }
    void *slot = gomap_put((GoMap *)v.ptr, k.ptr, v.t->key->size, v.t->elem->size, v.t->strkey);
    if (v.t->elem->kind == K_INTERFACE && e.t->kind != K_INTERFACE) *(GoIface *)slot = box(e.t, e.ptr);
    else memcpy(slot, e.ptr, v.t->elem->size);
}

Slice_GoOpaque_reflect_ValuePtr reflect_Value_MapKeys(GoOpaque recv) {
    RVal v = unpack(recv);
    Slice_GoOpaque_reflect_ValuePtr out = {0};
    if (!v.t || v.t->kind != K_MAP) go_panic_str("reflect: MapKeys on non-map");
    int n = gomap_len((GoMap *)v.ptr);
    out.p = (GoOpaque *)gc_alloc(sizeof(GoOpaque) * (size_t)(n ? n : 1));
    out.cap = n;
    GoMapIter it = gomap_iter((GoMap *)v.ptr);
    while (gomap_next(&it)) {
        void *c = gc_alloc(v.t->key->size ? v.t->key->size : 1);
        memcpy(c, it.key, v.t->key->size);
        out.p[out.len++] = pack((RVal){v.t->key, c, 0});
    }
    return out;
}

GoOpaque reflect_MakeMap(GoIface typ) {
    const RType *t = (const RType *)typ.data;
    GoMap *m = (GoMap *)gc_alloc(sizeof(GoMap));
    *m = gomap_make(t->key->size, t->elem->size, t->strkey);
    return pack((RVal){t, m, 0});
}

// ---------------------------------------------------------------- slices etc.
GoOpaque reflect_MakeSlice(GoIface typ, int len, int cap) {
    const RType *t = (const RType *)typ.data;
    GoSliceHdr *h = (GoSliceHdr *)gc_alloc(sizeof *h);
    h->p = gc_calloc((size_t)(cap > 0 ? cap : 1), t->elem->size ? t->elem->size : 1);
    h->len = len;
    h->cap = cap;
    return pack((RVal){t, h, 0});
}

GoOpaque reflect_Append(GoOpaque s, Slice_GoOpaque_reflect_ValuePtr xs) {
    RVal v = unpack(s);
    if (!v.t || v.t->kind != K_SLICE) go_panic_str("reflect: Append on non-slice");
    GoSliceHdr in = *(GoSliceHdr *)v.ptr;
    size_t es = v.t->elem->size;
    GoSliceHdr *h = (GoSliceHdr *)gc_alloc(sizeof *h);
    *h = in;
    if (h->len + xs.len > h->cap) h->p = go_slice_grow(h->p, h->len, &h->cap, h->len + xs.len, es);
    for (int i = 0; i < xs.len; i++) {
        RVal x = unpack(xs.p[i]);
        void *dst = (char *)h->p + (size_t)h->len * es;
        if (v.t->elem->kind == K_INTERFACE && x.t->kind != K_INTERFACE) *(GoIface *)dst = box(x.t, x.ptr);
        else memcpy(dst, x.ptr, es);
        h->len++;
    }
    return pack((RVal){v.t, h, 0});
}

GoOpaque reflect_New(GoIface typ) {
    const RType *t = (const RType *)typ.data;
    void **cell = (void **)gc_alloc(sizeof(void *));
    *cell = gc_calloc(1, t->size ? t->size : 1);
    return pack((RVal){ptr_to(t), cell, 0});
}

GoOpaque reflect_Zero(GoIface typ) {
    const RType *t = (const RType *)typ.data;
    return pack((RVal){t, gc_calloc(1, t->size ? t->size : 1), 0});
}

// ---------------------------------------------------------------- DeepEqual
static bool deep_eq(const RType *t, const void *a, const void *b, int depth) {
    if (depth > 100) return true;
    switch (t->kind) {
    case K_FLOAT32: return *(const f32 *)a == *(const f32 *)b;
    case K_FLOAT64: return *(const f64 *)a == *(const f64 *)b;
    case K_STRING: return go_streq(*(const GoString *)a, *(const GoString *)b);
    case K_PTR:
    case K_UNSAFEPTR: {
        const void *pa = *(void *const *)a, *pb = *(void *const *)b;
        if (pa == pb) return true;
        if (!pa || !pb || t->kind == K_UNSAFEPTR) return false;
        return deep_eq(t->elem, pa, pb, depth + 1);
    }
    case K_ARRAY:
        for (int i = 0; i < t->len; i++)
            if (!deep_eq(t->elem, (const char *)a + (size_t)i * t->elem->size, (const char *)b + (size_t)i * t->elem->size, depth + 1))
                return false;
        return true;
    case K_SLICE: {
        const GoSliceHdr *x = (const GoSliceHdr *)a, *y = (const GoSliceHdr *)b;
        if ((x->p == NULL) != (y->p == NULL) || x->len != y->len) return false;
        if (x->p == y->p) return true;
        for (int i = 0; i < x->len; i++)
            if (!deep_eq(t->elem, (const char *)x->p + (size_t)i * t->elem->size, (const char *)y->p + (size_t)i * t->elem->size, depth + 1))
                return false;
        return true;
    }
    case K_STRUCT:
        for (int i = 0; i < t->nfield; i++)
            if (!deep_eq(t->fields[i].type, (const char *)a + t->fields[i].offset, (const char *)b + t->fields[i].offset, depth + 1))
                return false;
        return true;
    case K_INTERFACE: {
        GoIface x = *(const GoIface *)a, y = *(const GoIface *)b;
        if (x.type != y.type) return false;
        if (!x.type) return true;
        RVal vx = value_of(x), vy = value_of(y);
        if (!vx.t) return x.data == y.data;
        return deep_eq(vx.t, vx.ptr, vy.ptr, depth + 1);
    }
    case K_MAP: {
        GoMap *x = (GoMap *)a, *y = (GoMap *)b;
        if ((x->m == NULL) != (y->m == NULL) || gomap_len(x) != gomap_len(y)) return false;
        if (x->m == y->m) return true;
        GoMapIter it = gomap_iter(x);
        while (gomap_next(&it)) {
            void *e = gomap_find(y, it.key);
            if (!e || !deep_eq(t->elem, it.val, e, depth + 1)) return false;
        }
        return true;
    }
    case K_FUNC: return ((const GoFunc *)a)->fn == NULL && ((const GoFunc *)b)->fn == NULL;
    }
    return memcmp(a, b, t->size) == 0;
}

bool reflect_DeepEqual(GoIface a, GoIface b) {
    if (a.type == 0 || b.type == 0) return a.type == b.type;
    if (a.type != b.type) return false;
    RVal x = value_of(a), y = value_of(b);
    if (!x.t) return go_iface_eq(a, b);
    return deep_eq(x.t, x.ptr, y.ptr, 0);
}

// ---------------------------------------------------------------- StructTag
GoString reflect_StructTag_Get(GoString tag, GoString key) {
    // Go's StructTag.Lookup
    int i = 0;
    while (i < tag.len) {
        while (i < tag.len && tag.p[i] == ' ') i++;
        if (i >= tag.len) break;
        int st = i;
        while (i < tag.len && tag.p[i] > ' ' && tag.p[i] != ':' && tag.p[i] != '"' && tag.p[i] != 0x7f) i++;
        if (i == st || i + 1 >= tag.len || tag.p[i] != ':' || tag.p[i + 1] != '"') break;
        GoString name = {tag.p + st, i - st};
        i++;  // ':'
        int qs = i;
        i++;  // '"'
        while (i < tag.len && tag.p[i] != '"') {
            if (tag.p[i] == '\\') i++;
            i++;
        }
        if (i >= tag.len) break;
        GoString qv = {tag.p + qs, i + 1 - qs};
        i++;
        if (go_streq(name, key)) {
            Ret_GoString__GoIface u = strconv_Unquote(qv);
            if (u.r1.type) break;
            return u.r0;
        }
    }
    return GOSTR("");
}

// ---------------------------------------------------------------- reflect.Type
static GoIface type_iface(const RType *t) { return t ? (GoIface){GO_TID_RTYPE, (void *)t} : (GoIface){0}; }

uintptr_t rt_type_Kind(const RType *t) { return t->kind; }
GoIface rt_type_Elem(const RType *t) {
    if (!t->elem) go_panic_str("reflect: Elem of invalid type");
    return type_iface(t->elem);
}
GoIface rt_type_Key(const RType *t) {
    if (t->kind != K_MAP) go_panic_str("reflect: Key of non-map type");
    return type_iface(t->key);
}
int rt_type_NumField(const RType *t) {
    if (t->kind != K_STRUCT) go_panic_str("reflect: NumField of non-struct type");
    return t->nfield;
}
GoReflectStructField rt_type_Field(const RType *t, int i) {
    if (t->kind != K_STRUCT || i < 0 || i >= t->nfield) go_panic_str("reflect: Field index out of bounds");
    const RField *f = &t->fields[i];
    GoReflectStructField sf;
    memset(&sf, 0, sizeof sf);
    sf.Name = go_cstr(f->name);
    sf.PkgPath = go_cstr(f->pkgpath);
    sf.Type = type_iface(f->type);
    sf.Tag = go_cstr(f->tag);
    sf.Offset = f->offset;
    sf.Index.p = (int *)gc_alloc(sizeof(int));
    sf.Index.p[0] = i;
    sf.Index.len = sf.Index.cap = 1;
    sf.Anonymous = f->anonymous;
    return sf;
}
GoString rt_type_Name(const RType *t) { return go_cstr(t->name); }
GoString rt_type_String(const RType *t) { return go_cstr(t->str); }
GoString rt_type_PkgPath(const RType *t) { return go_cstr(t->pkgpath); }
int rt_type_Len(const RType *t) { return t->len; }
uintptr_t rt_type_Size(const RType *t) { return t->size; }
bool rt_type_AssignableTo(const RType *t, GoIface u) {
    const RType *x = u.type == GO_TID_RTYPE ? (const RType *)u.data : NULL;
    if (!x) return false;
    if (t == x) return true;
    return x->kind == K_INTERFACE;  // approximation: only empty interfaces are assigned to here
}
bool rt_type_Comparable(const RType *t) {
    return t->kind != K_SLICE && t->kind != K_MAP && t->kind != K_FUNC;
}

// ---------------------------------------------------------------- more
GoIface reflect_TypeOf(GoIface a0) { return type_iface(value_of(a0).t); }

bool reflect_Value_CanAddr(GoOpaque recv) { return (unpack(recv).flags & F_ADDR) != 0; }

GoOpaque reflect_Value_Addr(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!(v.flags & F_ADDR)) go_panic_str("reflect.Value.Addr of unaddressable value");
    void **cell = (void **)gc_alloc(sizeof(void *));
    *cell = v.ptr;
    return pack((RVal){ptr_to(v.t), cell, v.flags & F_RO});
}

uintptr_t reflect_Value_Pointer(GoOpaque recv) {
    RVal v = unpack(recv);
    if (!v.t) return 0;
    switch (v.t->kind) {
    case K_PTR: case K_UNSAFEPTR: case K_MAP: case K_CHAN: case K_FUNC: return (uintptr_t) * (void **)v.ptr;
    case K_SLICE: return (uintptr_t)((GoSliceHdr *)v.ptr)->p;
    default: go_panic_str("reflect.Value.Pointer of non-pointer kind");
    }
}
void *reflect_Value_UnsafePointer(GoOpaque recv) { return (void *)reflect_Value_Pointer(recv); }

GoString reflect_Kind_String(uintptr_t k) {
    static const char *const names[] = {"invalid", "bool", "int", "int8", "int16", "int32", "int64", "uint",
                                        "uint8", "uint16", "uint32", "uint64", "uintptr", "float32", "float64",
                                        "complex64", "complex128", "array", "chan", "func", "interface", "map",
                                        "ptr", "slice", "string", "struct", "unsafe.Pointer"};
    if (k < sizeof names / sizeof names[0]) return go_cstr(names[k]);
    return go_cstr("kind?");
}
