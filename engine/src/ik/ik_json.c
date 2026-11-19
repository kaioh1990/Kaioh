// encoding/json for the translated game, over the reflect type descriptors
// (ik_rt[]): Marshal/MarshalIndent/Indent, Unmarshal and Decoder (with
// UseNumber) into any type - interfaces get Go's generic values
// (map[string]any, []any, float64 or json.Number, string, bool, nil).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ik/ik_sim.h"

extern const RType *const ik_rt_by_tid[];
extern const int ik_rt_by_tid_count;

enum { K_INVALID, K_BOOL, K_INT, K_INT8, K_INT16, K_INT32, K_INT64, K_UINT, K_UINT8, K_UINT16, K_UINT32, K_UINT64,
       K_UINTPTR, K_FLOAT32, K_FLOAT64, K_COMPLEX64, K_COMPLEX128, K_ARRAY, K_CHAN, K_FUNC, K_INTERFACE, K_MAP,
       K_PTR, K_SLICE, K_STRING, K_STRUCT, K_UNSAFEPTR };

#ifndef GO_TID_S_any
#define GO_TID_S_any (-20)
#endif
#ifndef GO_TID_M_string_any
#define GO_TID_M_string_any (-21)
#endif
#ifndef GO_TID_json_Number
#define GO_TID_json_Number (-22)
#endif

static const RType *by_tid(int tid) {
    return tid > 0 && tid < ik_rt_by_tid_count ? ik_rt_by_tid[tid] : NULL;
}

// ---------------------------------------------------------------- output
typedef struct {
    char *p;
    int len, cap;
} JB;
static void jb_put(JB *b, const char *s, int n) {
    if (b->len + n > b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        b->p = (char *)gc_realloc(b->p, (size_t)nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}
static void jb_c(JB *b, char c) { jb_put(b, &c, 1); }
static void jb_s(JB *b, const char *s) { jb_put(b, s, (int)strlen(s)); }

static void enc_string(JB *b, const char *s, int n) {
    static const char hex[] = "0123456789abcdef";
    jb_c(b, '"');
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': jb_s(b, "\\\""); break;
        case '\\': jb_s(b, "\\\\"); break;
        case '\n': jb_s(b, "\\n"); break;
        case '\r': jb_s(b, "\\r"); break;
        case '\t': jb_s(b, "\\t"); break;
        case '<': case '>': case '&': {  // Go escapes HTML characters
            char u[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
            jb_s(b, u);
            break;
        }
        default:
            if (c < 0x20) {
                char u[7] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0};
                jb_s(b, u);
            } else {
                jb_c(b, (char)c);
            }
        }
    }
    jb_c(b, '"');
}

static void enc_float(JB *b, f64 f, int bits) {
    if (isnan(f) || isinf(f)) {
        jb_s(b, "null");
        return;
    }
    f64 a = fabs(f);
    char fmt = 'f';
    if (a != 0 && (a < 1e-6 || a >= 1e21)) fmt = 'e';
    GoString s = go_format_float(f, fmt, -1, bits);
    if (fmt == 'e') {  // Go: clean up e-09 to e-9
        char t[64];
        int n = s.len < 63 ? s.len : 63;
        memcpy(t, s.p, (size_t)n);
        t[n] = 0;
        char *e = strchr(t, 'e');
        if (e && (e[1] == '-' || e[1] == '+') && e[2] == '0' && e[3]) memmove(e + 2, e + 3, strlen(e + 3) + 1);
        jb_s(b, t);
        return;
    }
    jb_put(b, s.p, s.len);
}

typedef struct {
    GoString name;
    bool omitempty, skip, asString;
} Tag;
static Tag field_tag(const RField *f) {
    Tag t = {go_cstr(f->name), false, false, false};
    const char *tg = f->tag ? strstr(f->tag, "json:\"") : NULL;
    if (tg) {
        tg += 6;
        const char *end = strchr(tg, '"');
        if (!end) end = tg + strlen(tg);
        const char *comma = memchr(tg, ',', (size_t)(end - tg));
        const char *nend = comma ? comma : end;
        if (nend - tg == 1 && tg[0] == '-' && !comma) t.skip = true;
        if (nend > tg) t.name = (GoString){tg, (int)(nend - tg)};
        for (const char *o = comma; o && o < end;) {
            const char *nx = memchr(o + 1, ',', (size_t)(end - o - 1));
            const char *oe = nx ? nx : end;
            if (oe - o - 1 == 9 && !memcmp(o + 1, "omitempty", 9)) t.omitempty = true;
            if (oe - o - 1 == 6 && !memcmp(o + 1, "string", 6)) t.asString = true;
            o = nx;
        }
    }
    if (f->pkgpath && f->pkgpath[0] && !f->anonymous) t.skip = true;  // unexported
    return t;
}

static bool is_empty(const RType *t, const void *p) {
    switch (t->kind) {
    case K_BOOL: return !*(const bool *)p;
    case K_STRING: return ((const GoString *)p)->len == 0;
    case K_SLICE: return ((const GoSliceHdr *)p)->len == 0;
    case K_MAP: return gomap_len((const GoMap *)p) == 0;
    case K_PTR: return *(void *const *)p == NULL;
    case K_INTERFACE: return ((const GoIface *)p)->type == 0;
    case K_FLOAT32: return *(const f32 *)p == 0;
    case K_FLOAT64: return *(const f64 *)p == 0;
    case K_ARRAY: return t->len == 0;
    default: {
        if (t->kind >= K_INT && t->kind <= K_UINTPTR) {
            for (size_t i = 0; i < t->size; i++)
                if (((const u8 *)p)[i]) return false;
            return true;
        }
        return false;
    }
    }
}

static i64 load_i(const RType *t, const void *p) {
    switch (t->kind) {
    case K_INT8: return *(const i8 *)p;
    case K_INT16: return *(const i16 *)p;
    case K_INT32: return *(const i32 *)p;
    case K_INT64: return *(const i64 *)p;
    case K_INT: return *(const int *)p;
    case K_UINT8: return *(const u8 *)p;
    case K_UINT16: return *(const u16 *)p;
    case K_UINT32: return *(const u32 *)p;
    case K_UINT64: return (i64) * (const u64 *)p;
    case K_UINT: return (i64) * (const unsigned *)p;
    case K_UINTPTR: return (i64) * (const uintptr_t *)p;
    }
    return 0;
}

static int cmp_gostr(const void *a, const void *b) {
    const GoString *x = a, *y = b;
    int n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->p, y->p, (size_t)n);
    return c ? c : x->len - y->len;
}

static bool encode(JB *b, const RType *t, const void *p, int depth);

static bool encode_iface(JB *b, GoIface v, int depth) {
    if (v.type == 0) {
        jb_s(b, "null");
        return true;
    }
    if (v.type == GO_TID_json_Number) {
        GoString s = *(GoString *)v.data;
        jb_put(b, s.p, s.len ? s.len : 0);
        if (!s.len) jb_c(b, '0');
        return true;
    }
    const RType *t = by_tid(v.type);
    if (!t) return false;
    if (t->kind == K_PTR || t->kind == K_UNSAFEPTR) return encode(b, t, &v.data, depth);
    return encode(b, t, v.data, depth);
}

static bool encode(JB *b, const RType *t, const void *p, int depth) {
    if (depth > 1000) return false;
    switch (t->kind) {
    case K_BOOL: jb_s(b, *(const bool *)p ? "true" : "false"); return true;
    case K_STRING: enc_string(b, ((const GoString *)p)->p, ((const GoString *)p)->len); return true;
    case K_FLOAT32: enc_float(b, *(const f32 *)p, 32); return true;
    case K_FLOAT64: enc_float(b, *(const f64 *)p, 64); return true;
    case K_INTERFACE: return encode_iface(b, *(const GoIface *)p, depth + 1);
    case K_PTR: {
        void *q = *(void *const *)p;
        if (!q) {
            jb_s(b, "null");
            return true;
        }
        return encode(b, t->elem, q, depth + 1);
    }
    case K_SLICE:
    case K_ARRAY: {
        const u8 *base;
        int n;
        if (t->kind == K_SLICE) {
            const GoSliceHdr *s = p;
            if (!s->p && t->kind == K_SLICE) {
                jb_s(b, "null");
                return true;
            }
            base = s->p, n = s->len;
            if (t->elem->kind == K_UINT8) {  // []byte: base64
                static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                jb_c(b, '"');
                for (int i = 0; i < n; i += 3) {
                    u32 v = (u32)base[i] << 16 | (i + 1 < n ? (u32)base[i + 1] << 8 : 0) | (i + 2 < n ? base[i + 2] : 0);
                    char o[4] = {tb[v >> 18], tb[(v >> 12) & 63], i + 1 < n ? tb[(v >> 6) & 63] : '=', i + 2 < n ? tb[v & 63] : '='};
                    jb_put(b, o, 4);
                }
                jb_c(b, '"');
                return true;
            }
        } else {
            base = p, n = t->len;
        }
        jb_c(b, '[');
        for (int i = 0; i < n; i++) {
            if (i) jb_c(b, ',');
            if (!encode(b, t->elem, base + (size_t)i * t->elem->size, depth + 1)) return false;
        }
        jb_c(b, ']');
        return true;
    }
    case K_MAP: {
        GoMap *m = (GoMap *)p;
        if (!m->m) {
            jb_s(b, "null");
            return true;
        }
        int n = gomap_len(m), k = 0;
        GoString *keys = (GoString *)gc_alloc(sizeof(GoString) * (size_t)(n ? n : 1));
        void **vals = (void **)gc_alloc(sizeof(void *) * (size_t)(n ? n : 1));
        GoMapIter it = gomap_iter(m);
        while (gomap_next(&it) && k < n) {
            if (t->key->kind == K_STRING) {
                keys[k] = *(GoString *)it.key;
            } else if (t->key->kind >= K_INT && t->key->kind <= K_UINTPTR) {
                char tmp[32];
                snprintf(tmp, sizeof tmp, "%lld", (long long)load_i(t->key, it.key));
                keys[k] = go_cstr(tmp);
                keys[k].p = strcpy((char *)gc_alloc_atomic(strlen(tmp) + 1), tmp);
            } else {
                return false;
            }
            vals[k++] = it.val;
        }
        // sort keys (Go orders map keys), carrying the values along
        int *ord = (int *)malloc(sizeof(int) * (size_t)(k ? k : 1));
        for (int i = 0; i < k; i++) ord[i] = i;
        for (int i = 1; i < k; i++)  // insertion sort: maps here are small
            for (int j = i; j > 0 && cmp_gostr(&keys[ord[j - 1]], &keys[ord[j]]) > 0; j--) {
                int x = ord[j];
                ord[j] = ord[j - 1], ord[j - 1] = x;
            }
        jb_c(b, '{');
        bool ok = true;
        for (int i = 0; i < k && ok; i++) {
            if (i) jb_c(b, ',');
            enc_string(b, keys[ord[i]].p, keys[ord[i]].len);
            jb_c(b, ':');
            ok = encode(b, t->elem, vals[ord[i]], depth + 1);
        }
        free(ord);
        jb_c(b, '}');
        return ok;
    }
    case K_STRUCT: {
        jb_c(b, '{');
        bool first = true;
        for (int i = 0; i < t->nfield; i++) {
            const RField *f = &t->fields[i];
            Tag tg = field_tag(f);
            if (tg.skip) continue;
            const void *fp = (const u8 *)p + f->offset;
            if (tg.omitempty && is_empty(f->type, fp)) continue;
            if (!first) jb_c(b, ',');
            first = false;
            enc_string(b, tg.name.p, tg.name.len);
            jb_c(b, ':');
            if (tg.asString) jb_c(b, '"');
            if (!encode(b, f->type, fp, depth + 1)) return false;
            if (tg.asString) jb_c(b, '"');
        }
        jb_c(b, '}');
        return true;
    }
    default:
        if (t->kind >= K_INT && t->kind <= K_UINTPTR) {
            char tmp[32];
            if (t->kind >= K_UINT && t->kind != K_UINT64)
                snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)load_i(t, p));
            else if (t->kind == K_UINT64)
                snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)*(const u64 *)p);
            else
                snprintf(tmp, sizeof tmp, "%lld", (long long)load_i(t, p));
            jb_s(b, tmp);
            return true;
        }
        return false;
    }
}

static Slice_u8 jb_slice(JB b) { return (Slice_u8){(u8 *)b.p, b.len, b.cap}; }

Ret_Slice_u8__GoIface json_Marshal(GoIface v) {
    JB b = {0};
    if (!encode_iface(&b, v, 0)) return (Ret_Slice_u8__GoIface){{0}, go_error(GOSTR("json: unsupported type"))};
    return (Ret_Slice_u8__GoIface){jb_slice(b), {0}};
}

// json.Indent semantics over compact or loose input
static bool indent_into(JB *o, const u8 *s, int n, GoString prefix, GoString ind) {
    int depth = 0;
    bool instr = false, needIndent = false;
    for (int i = 0; i < n; i++) {
        u8 c = s[i];
        if (instr) {
            jb_c(o, (char)c);
            if (c == '\\' && i + 1 < n) jb_c(o, (char)s[++i]);
            else if (c == '"') instr = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (needIndent && c != ']' && c != '}') {
            needIndent = false;
            depth++;
            jb_c(o, '\n');
            jb_put(o, prefix.p, prefix.len);
            for (int d = 0; d < depth; d++) jb_put(o, ind.p, ind.len);
        }
        switch (c) {
        case '"': instr = true; jb_c(o, '"'); break;
        case '{': case '[': jb_c(o, (char)c); needIndent = true; break;
        case ',':
            jb_c(o, ',');
            jb_c(o, '\n');
            jb_put(o, prefix.p, prefix.len);
            for (int d = 0; d < depth; d++) jb_put(o, ind.p, ind.len);
            break;
        case ':': jb_s(o, ": "); break;
        case '}': case ']':
            if (needIndent) {
                needIndent = false;  // empty {} or []
            } else {
                depth--;
                jb_c(o, '\n');
                jb_put(o, prefix.p, prefix.len);
                for (int d = 0; d < depth; d++) jb_put(o, ind.p, ind.len);
            }
            jb_c(o, (char)c);
            break;
        default: jb_c(o, (char)c);
        }
    }
    return !instr && depth == 0;
}

Ret_Slice_u8__GoIface json_MarshalIndent(GoIface v, GoString prefix, GoString ind) {
    Ret_Slice_u8__GoIface r = json_Marshal(v);
    if (r.r1.type) return r;
    JB o = {0};
    indent_into(&o, r.r0.p, r.r0.len, prefix, ind);
    return (Ret_Slice_u8__GoIface){jb_slice(o), {0}};
}

GoIface json_Indent(GoOpaque *dst, Slice_u8 src, GoString prefix, GoString ind) {
    JB o = {0};
    if (!indent_into(&o, src.p, src.len, prefix, ind)) return go_error(GOSTR("json: invalid input"));
    bytes_Buffer_Write(dst, jb_slice(o));
    return (GoIface){0};
}

GoString json_Number_String(GoString n) { return n; }
Ret_f64__GoIface json_Number_Float64(GoString n) { return strconv_ParseFloat(n, 64); }
Ret_i64__GoIface json_Number_Int64(GoString n) { return strconv_ParseInt(n, 10, 64); }

// ---------------------------------------------------------------- input
typedef struct {
    const u8 *s;
    int n, i;
    bool useNumber;
    const char *err;
} JP;

static void ws(JP *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}
static bool fail(JP *p, const char *e) {
    if (!p->err) p->err = e;
    return false;
}

static void utf8_put(JB *b, u32 r) {
    char t[4];
    if (r < 0x80) { t[0] = (char)r; jb_put(b, t, 1); }
    else if (r < 0x800) { t[0] = (char)(0xC0 | r >> 6); t[1] = (char)(0x80 | (r & 63)); jb_put(b, t, 2); }
    else if (r < 0x10000) { t[0] = (char)(0xE0 | r >> 12); t[1] = (char)(0x80 | ((r >> 6) & 63)); t[2] = (char)(0x80 | (r & 63)); jb_put(b, t, 3); }
    else { t[0] = (char)(0xF0 | r >> 18); t[1] = (char)(0x80 | ((r >> 12) & 63)); t[2] = (char)(0x80 | ((r >> 6) & 63)); t[3] = (char)(0x80 | (r & 63)); jb_put(b, t, 4); }
}
static int hexv(u8 c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_string(JP *p, GoString *out) {
    if (p->i >= p->n || p->s[p->i] != '"') return fail(p, "expected string");
    p->i++;
    JB b = {0};
    while (p->i < p->n && p->s[p->i] != '"') {
        u8 c = p->s[p->i++];
        if (c != '\\') {
            jb_c(&b, (char)c);
            continue;
        }
        if (p->i >= p->n) return fail(p, "unexpected end");
        c = p->s[p->i++];
        switch (c) {
        case 'n': jb_c(&b, '\n'); break;
        case 't': jb_c(&b, '\t'); break;
        case 'r': jb_c(&b, '\r'); break;
        case 'b': jb_c(&b, '\b'); break;
        case 'f': jb_c(&b, '\f'); break;
        case 'u': {
            if (p->i + 4 > p->n) return fail(p, "bad escape");
            u32 r = 0;
            for (int k = 0; k < 4; k++) {
                int h = hexv(p->s[p->i++]);
                if (h < 0) return fail(p, "bad escape");
                r = r << 4 | (u32)h;
            }
            if (r >= 0xD800 && r < 0xDC00 && p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                u32 lo = 0;
                bool ok = true;
                for (int k = 0; k < 4; k++) {
                    int h = hexv(p->s[p->i + 2 + k]);
                    if (h < 0) ok = false;
                    lo = lo << 4 | (u32)(h < 0 ? 0 : h);
                }
                if (ok && lo >= 0xDC00 && lo < 0xE000) {
                    r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
                    p->i += 6;
                }
            }
            utf8_put(&b, r);
            break;
        }
        default: jb_c(&b, (char)c);
        }
    }
    if (p->i >= p->n) return fail(p, "unterminated string");
    p->i++;
    *out = (GoString){b.p ? b.p : "", b.len};
    return true;
}

static bool scan_number(JP *p, GoString *lit) {
    int st = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    while (p->i < p->n && ((p->s[p->i] >= '0' && p->s[p->i] <= '9') || p->s[p->i] == '.' || p->s[p->i] == 'e' ||
                           p->s[p->i] == 'E' || p->s[p->i] == '+' || p->s[p->i] == '-'))
        p->i++;
    if (p->i == st) return fail(p, "invalid character");
    char *c = (char *)gc_alloc_atomic((size_t)(p->i - st) + 1);
    memcpy(c, p->s + st, (size_t)(p->i - st));
    c[p->i - st] = 0;
    *lit = (GoString){c, p->i - st};
    return true;
}

static bool skip_value(JP *p);
static bool decode(JP *p, const RType *t, void *dst, int depth);

// generic value for interface{} targets
static bool decode_any(JP *p, GoIface *out, int depth) {
    ws(p);
    if (p->i >= p->n) return fail(p, "unexpected end of JSON input");
    if (depth > 1000) return fail(p, "too deep");
    u8 c = p->s[p->i];
    if (c == '{') {
        GoMap m = gomap_make(sizeof(GoString), sizeof(GoIface), 1);
        p->i++;
        ws(p);
        if (p->i < p->n && p->s[p->i] == '}') {
            p->i++;
        } else {
            for (;;) {
                GoString k;
                ws(p);
                if (!parse_string(p, &k)) return false;
                ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return fail(p, "expected colon");
                p->i++;
                GoIface v;
                if (!decode_any(p, &v, depth + 1)) return false;
                *(GoIface *)gomap_put(&m, &k, sizeof(GoString), sizeof(GoIface), 1) = v;
                ws(p);
                if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
                if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
                return fail(p, "expected comma or }");
            }
        }
        *out = (GoIface){GO_TID_M_string_any, go_box(GoMap, m)};
        return true;
    }
    if (c == '[') {
        int n = 0, cap = 8;
        GoIface *a = (GoIface *)gc_calloc((size_t)cap, sizeof(GoIface));
        p->i++;
        ws(p);
        if (p->i < p->n && p->s[p->i] == ']') {
            p->i++;
        } else {
            for (;;) {
                GoIface v;
                if (!decode_any(p, &v, depth + 1)) return false;
                if (n == cap) a = (GoIface *)go_slice_grow(a, n, &cap, n + 1, sizeof(GoIface));
                a[n++] = v;
                ws(p);
                if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
                if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
                return fail(p, "expected comma or ]");
            }
        }
        *out = (GoIface){GO_TID_S_any, go_box(Slice_GoIface, ((Slice_GoIface){a, n, cap}))};
        return true;
    }
    if (c == '"') {
        GoString s;
        if (!parse_string(p, &s)) return false;
        *out = (GoIface){GO_TID_string, go_box(GoString, s)};
        return true;
    }
    if (p->n - p->i >= 4 && !memcmp(p->s + p->i, "true", 4)) { p->i += 4; *out = (GoIface){GO_TID_bool, go_box_a(bool, true)}; return true; }
    if (p->n - p->i >= 5 && !memcmp(p->s + p->i, "false", 5)) { p->i += 5; *out = (GoIface){GO_TID_bool, go_box_a(bool, false)}; return true; }
    if (p->n - p->i >= 4 && !memcmp(p->s + p->i, "null", 4)) { p->i += 4; *out = (GoIface){0}; return true; }
    GoString lit;
    if (!scan_number(p, &lit)) return false;
    if (p->useNumber) {
        *out = (GoIface){GO_TID_json_Number, go_box(GoString, lit)};
    } else {
        *out = (GoIface){GO_TID_f64, go_box_a(f64, strtod(lit.p, NULL))};
    }
    return true;
}

static bool skip_value(JP *p) {
    GoIface v;
    return decode_any(p, &v, 0);
}

static void store_i(const RType *t, void *dst, i64 v) {
    switch (t->kind) {
    case K_INT8: case K_UINT8: *(i8 *)dst = (i8)v; break;
    case K_INT16: case K_UINT16: *(i16 *)dst = (i16)v; break;
    case K_INT32: case K_UINT32: *(i32 *)dst = (i32)v; break;
    case K_INT64: case K_UINT64: *(i64 *)dst = v; break;
    case K_INT: *(int *)dst = (int)v; break;
    case K_UINT: *(unsigned *)dst = (unsigned)v; break;
    case K_UINTPTR: *(uintptr_t *)dst = (uintptr_t)v; break;
    }
}

static bool name_eq_fold(GoString a, GoString b) {
    if (a.len != b.len) return false;
    for (int i = 0; i < a.len; i++) {
        char x = a.p[i], y = b.p[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

static bool decode(JP *p, const RType *t, void *dst, int depth) {
    ws(p);
    if (p->i >= p->n) return fail(p, "unexpected end of JSON input");
    if (depth > 1000) return fail(p, "too deep");
    if (t->kind == K_INTERFACE) return decode_any(p, (GoIface *)dst, depth);
    if (p->n - p->i >= 4 && !memcmp(p->s + p->i, "null", 4)) {  // null leaves the value alone (pointers/maps/slices: nil)
        p->i += 4;
        if (t->kind == K_PTR || t->kind == K_MAP || t->kind == K_SLICE) memset(dst, 0, t->size);
        return true;
    }
    switch (t->kind) {
    case K_PTR: {
        void **pp = (void **)dst;
        if (!*pp) *pp = gc_calloc(1, t->elem->size ? t->elem->size : 1);
        return decode(p, t->elem, *pp, depth + 1);
    }
    case K_BOOL:
        if (p->n - p->i >= 4 && !memcmp(p->s + p->i, "true", 4)) { p->i += 4; *(bool *)dst = true; return true; }
        if (p->n - p->i >= 5 && !memcmp(p->s + p->i, "false", 5)) { p->i += 5; *(bool *)dst = false; return true; }
        return fail(p, "cannot unmarshal into bool");
    case K_STRING: {
        GoString s;
        if (p->s[p->i] != '"') return skip_value(p) && fail(p, "cannot unmarshal into string");
        if (!parse_string(p, &s)) return false;
        *(GoString *)dst = s;
        return true;
    }
    case K_FLOAT32: case K_FLOAT64: {
        GoString lit;
        if (!scan_number(p, &lit)) return false;
        f64 f = strtod(lit.p, NULL);
        if (t->kind == K_FLOAT32) *(f32 *)dst = (f32)f;
        else *(f64 *)dst = f;
        return true;
    }
    case K_SLICE: {
        if (t->elem->kind == K_UINT8 && p->s[p->i] == '"') {  // base64 []byte
            GoString s;
            if (!parse_string(p, &s)) return false;
            Ret_Slice_u8__GoIface d = base64_Encoding_DecodeString(base64_StdEncoding, s);
            *(Slice_u8 *)dst = d.r0;
            return true;
        }
        if (p->s[p->i] != '[') return fail(p, "cannot unmarshal into slice");
        p->i++;
        size_t es = t->elem->size;
        int n = 0, cap = 4;
        u8 *a = (u8 *)gc_calloc((size_t)cap, es ? es : 1);
        ws(p);
        if (p->i < p->n && p->s[p->i] == ']') {
            p->i++;
        } else {
            for (;;) {
                if (n == cap) {
                    int nc = cap * 2;
                    u8 *na = (u8 *)gc_calloc((size_t)nc, es ? es : 1);
                    memcpy(na, a, es * (size_t)n);
                    a = na, cap = nc;
                }
                if (!decode(p, t->elem, a + es * (size_t)n, depth + 1)) return false;
                n++;
                ws(p);
                if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
                if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
                return fail(p, "expected comma or ]");
            }
        }
        *(GoSliceHdr *)dst = (GoSliceHdr){a, n, cap};
        return true;
    }
    case K_ARRAY: {
        if (p->s[p->i] != '[') return fail(p, "cannot unmarshal into array");
        p->i++;
        int n = 0;
        ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; return true; }
        for (;;) {
            if (n < t->len) {
                if (!decode(p, t->elem, (u8 *)dst + t->elem->size * (size_t)n, depth + 1)) return false;
            } else if (!skip_value(p)) {
                return false;
            }
            n++;
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; return true; }
            return fail(p, "expected comma or ]");
        }
    }
    case K_MAP: {
        if (p->s[p->i] != '{') return fail(p, "cannot unmarshal into map");
        if (t->key->kind != K_STRING && !(t->key->kind >= K_INT && t->key->kind <= K_UINTPTR))
            return fail(p, "unsupported map key");
        int kk = t->key->kind == K_STRING ? 1 : 0;
        GoMap *m = (GoMap *)dst;
        if (!m->m) *m = gomap_make(t->key->size, t->elem->size, kk);
        p->i++;
        ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return true; }
        for (;;) {
            GoString k;
            ws(p);
            if (!parse_string(p, &k)) return false;
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return fail(p, "expected colon");
            p->i++;
            u8 key[16] = {0};
            void *kp = &k;
            if (!kk) {
                store_i(t->key, key, strtoll(k.p, NULL, 10));
                kp = key;
            }
            void *slot = gomap_put(m, kp, t->key->size, t->elem->size, kk);
            if (!decode(p, t->elem, slot, depth + 1)) return false;
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; return true; }
            return fail(p, "expected comma or }");
        }
    }
    case K_STRUCT: {
        if (p->s[p->i] != '{') return fail(p, "cannot unmarshal into struct");
        p->i++;
        ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return true; }
        for (;;) {
            GoString k;
            ws(p);
            if (!parse_string(p, &k)) return false;
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return fail(p, "expected colon");
            p->i++;
            const RField *hit = NULL;
            bool asString = false;
            for (int pass = 0; pass < 2 && !hit; pass++) {
                for (int i = 0; i < t->nfield; i++) {
                    Tag tg = field_tag(&t->fields[i]);
                    if (tg.skip) continue;
                    if (pass == 0 ? go_streq(tg.name, k) : name_eq_fold(tg.name, k)) {
                        hit = &t->fields[i];
                        asString = tg.asString;
                        break;
                    }
                }
            }
            if (hit) {
                void *fp = (u8 *)dst + hit->offset;
                if (asString && p->s[p->i] == '"') {  // `json:",string"`: the value is quoted
                    GoString inner;
                    if (!parse_string(p, &inner)) return false;
                    JP q = {(const u8 *)inner.p, inner.len, 0, p->useNumber, NULL};
                    if (!decode(&q, hit->type, fp, depth + 1)) return fail(p, q.err);
                } else if (!decode(p, hit->type, fp, depth + 1)) {
                    return false;
                }
            } else if (!skip_value(p)) {
                return false;
            }
            ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; return true; }
            return fail(p, "expected comma or }");
        }
    }
    default:
        if (t->kind >= K_INT && t->kind <= K_UINTPTR) {
            GoString lit;
            if (!scan_number(p, &lit)) return false;
            if (t->kind >= K_UINT && t->kind <= K_UINTPTR) store_i(t, dst, (i64)strtoull(lit.p, NULL, 10));
            else store_i(t, dst, strtoll(lit.p, NULL, 10));
            return true;
        }
        return fail(p, "unsupported type");
    }
}

static GoIface unmarshal(const u8 *s, int n, GoIface v, bool useNumber, int *consumed) {
    const RType *t = by_tid(v.type);
    if (!t || t->kind != K_PTR || !v.data) return go_error(GOSTR("json: Unmarshal(non-pointer)"));
    JP p = {s, n, 0, useNumber, NULL};
    ws(&p);
    if (p.i >= p.n) return io_EOF;
    if (!decode(&p, t->elem, v.data, 0)) return go_errorf("json: %s", p.err ? p.err : "syntax error");
    if (consumed) *consumed = p.i;
    return (GoIface){0};
}

GoIface json_Unmarshal(Slice_u8 data, GoIface v) {
    int used = 0;
    GoIface e = unmarshal(data.p, data.len, v, false, &used);
    if (e.type == io_EOF.type && e.data == io_EOF.data) return go_error(GOSTR("unexpected end of JSON input"));
    if (!e.type) {
        JP p = {data.p, data.len, used, false, NULL};
        ws(&p);
        if (p.i < p.n) return go_error(GOSTR("invalid character after top-level value"));
    }
    return e;
}

// Decoder: reads the whole stream on the first Decode and hands out values
typedef struct JDec {
    GoIface r;
    bool useNumber, loaded;
    Slice_u8 buf;
    int off;
} JDec;

GoOpaque *json_NewDecoder(GoIface r) {
    JDec *d = (JDec *)gc_calloc(1, sizeof *d);
    d->r = r;
    return (GoOpaque *)d;
}
void json_Decoder_UseNumber(GoOpaque *recv) { ((JDec *)recv)->useNumber = true; }
GoIface json_Decoder_Decode(GoOpaque *recv, GoIface v) {
    JDec *d = (JDec *)recv;
    if (!d->loaded) {
        Ret_Slice_u8__GoIface all = io_ReadAll(d->r);
        if (all.r1.type) return all.r1;
        d->buf = all.r0;
        d->loaded = true;
    }
    int used = 0;
    GoIface e = unmarshal(d->buf.p + d->off, d->buf.len - d->off, v, d->useNumber, &used);
    if (!e.type) d->off += used;
    return e;
}
