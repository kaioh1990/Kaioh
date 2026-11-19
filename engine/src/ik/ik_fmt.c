// fmt / strconv formatting with Go's output rules (Sprintf, Sprint,
// FormatFloat, Quote). Floats: shortest round-trip digits for %v, exact
// rounding for fixed precision (C's printf is exact on all targets).
#include <stdarg.h>
#include <stdio.h>

#include "ik/ik_sim.h"

extern const unsigned char ik_tid_kind[];
bool ik_fmt_method(GoIface x, GoString *out);  // generated (gotoc)
extern const int ik_tid_count;

// ---------------------------------------------------------------- buffer
typedef struct Buf {
    char *p;
    int len, cap;
} Buf;

static void b_grow(Buf *b, int n) {
    if (b->len + n <= b->cap) return;
    int nc = b->cap ? b->cap * 2 : 64;
    while (nc < b->len + n) nc *= 2;
    b->p = (char *)gc_realloc(b->p, (size_t)nc);
    b->cap = nc;
}
static void b_put(Buf *b, const char *s, int n) {
    if (n <= 0) return;
    b_grow(b, n);
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}
static void b_puts(Buf *b, const char *s) { b_put(b, s, (int)strlen(s)); }
static void b_putc(Buf *b, char c) { b_put(b, &c, 1); }
static void b_pad(Buf *b, char c, int n) {
    for (int i = 0; i < n; i++) b_putc(b, c);
}
static GoString b_str(Buf *b) {
    if (!b->len) {
        gc_free(b->p);
        return (GoString){"", 0};
    }
    b_grow(b, 1);
    b->p[b->len] = 0;
    return (GoString){b->p, b->len};
}

// ---------------------------------------------------------------- floats
// Decimal digits of |v|: d[0..nd) with the decimal point after dp digits.
typedef struct Digits {
    char d[40];
    int nd, dp;
} Digits;

static void digits_from_e(const char *s, Digits *g) {
    // s = "d.ddddde[+-]xx" (no sign)
    g->nd = 0;
    const char *p = s;
    for (; *p && *p != 'e'; p++)
        if (*p >= '0' && *p <= '9') g->d[g->nd++] = *p;
    int ex = *p == 'e' ? atoi(p + 1) : 0;
    g->dp = ex + 1;
    while (g->nd > 1 && g->d[g->nd - 1] == '0') g->nd--;  // trailing zeros carry no information
    if (g->nd == 1 && g->d[0] == '0') g->dp = 0;
}

static bool roundtrips(const char *s, f64 v, int bits) {
    if (bits == 32) return strtof(s, NULL) == (f32)v;
    return strtod(s, NULL) == v;
}

static void shortest_digits(f64 v, int bits, Digits *g) {
    char s[64];
    if (v == 0) {
        g->nd = 1;
        g->d[0] = '0';
        g->dp = 1;
        return;
    }
    for (int p = 1; p <= 17; p++) {
        snprintf(s, sizeof s, "%.*e", p - 1, v);
        if (roundtrips(s, v, bits) || p == 17) break;
    }
    digits_from_e(s, g);
}

static void fixed_digits(f64 v, int prec, Digits *g) {  // prec significant digits
    char s[64];
    if (prec < 1) prec = 1;
    if (prec > 30) prec = 30;
    snprintf(s, sizeof s, "%.*e", prec - 1, v);
    digits_from_e(s, g);
    if (v == 0) g->dp = 1, g->nd = 1;
}

static void put_e(Buf *b, const Digits *g, int prec, char ec) {  // prec digits after the point; -1 = all
    b_putc(b, g->d[0]);
    int nfrac = prec < 0 ? g->nd - 1 : prec;
    if (nfrac > 0) {
        b_putc(b, '.');
        for (int i = 1; i <= nfrac; i++) b_putc(b, i < g->nd ? g->d[i] : '0');
    }
    int ex = (g->nd == 1 && g->d[0] == '0') ? 0 : g->dp - 1;
    b_putc(b, ec);
    b_putc(b, ex < 0 ? '-' : '+');
    if (ex < 0) ex = -ex;
    char t[8];
    snprintf(t, sizeof t, "%02d", ex);
    b_puts(b, t);
}

static void put_f(Buf *b, const Digits *g, int nfrac) {
    if (g->dp > 0) {
        for (int i = 0; i < g->dp; i++) b_putc(b, i < g->nd ? g->d[i] : '0');
    } else {
        b_putc(b, '0');
    }
    if (nfrac > 0) {
        b_putc(b, '.');
        for (int i = 0; i < nfrac; i++) {
            int j = g->dp + i;
            b_putc(b, (j >= 0 && j < g->nd) ? g->d[j] : '0');
        }
    }
}

static void fmt_float(Buf *b, f64 v, char fmt, int prec, int bits) {
    if (v != v) {
        b_puts(b, "NaN");
        return;
    }
    if (v > 1.7976931348623157e308) {
        b_puts(b, "+Inf");
        return;
    }
    if (v < -1.7976931348623157e308) {
        b_puts(b, "-Inf");
        return;
    }
    if (signbit(v)) b_putc(b, '-');
    f64 a = fabs(v);
    if (bits == 32) a = (f64)(f32)a;
    char s[400];
    Digits g;
    switch (fmt) {
    case 'f':
    case 'F':
        if (prec < 0) {
            shortest_digits(a, bits, &g);
            put_f(b, &g, g.nd - g.dp > 0 ? g.nd - g.dp : 0);
        } else {
            snprintf(s, sizeof s, "%.*f", prec, a);
            b_puts(b, s);
        }
        return;
    case 'e':
    case 'E':
        if (prec < 0) {
            shortest_digits(a, bits, &g);
            put_e(b, &g, -1, fmt);
        } else {
            snprintf(s, sizeof s, "%.*e", prec, a);
            if (fmt == 'E')
                for (char *q = s; *q; q++)
                    if (*q == 'e') *q = 'E';
            b_puts(b, s);
        }
        return;
    default: {  // 'g' / 'G'
        int eprec;
        if (prec < 0) {
            shortest_digits(a, bits, &g);
            eprec = 6;
        } else {
            fixed_digits(a, prec == 0 ? 1 : prec, &g);
            eprec = prec == 0 ? 1 : prec;
            if (eprec > g.nd && g.nd >= g.dp) eprec = g.nd;
        }
        int ex = g.dp - 1;
        if (a == 0) ex = 0;
        if (ex < -4 || ex >= eprec) {
            put_e(b, &g, g.nd - 1, fmt == 'G' ? 'E' : 'e');
        } else {
            put_f(b, &g, g.nd - g.dp > 0 ? g.nd - g.dp : 0);
        }
        return;
    }
    }
}

GoString go_format_float(f64 f, char fmt, int prec, int bits) {
    Buf b = {0};
    fmt_float(&b, f, fmt, prec, bits);
    return b_str(&b);
}

// ---------------------------------------------------------------- values
static int kind_of(int type) {
    if (type > 0 && type < 20) return type;
    if (type >= 100 && type < ik_tid_count) return ik_tid_kind[type];
    return 0;
}

static bool load_int(GoIface x, int k, i64 *s, u64 *u, bool *isu) {
    *isu = false;
    switch (k) {
    case GO_TID_int: *s = *(int *)x.data; return true;
    case GO_TID_i8: *s = *(i8 *)x.data; return true;
    case GO_TID_i16: *s = *(i16 *)x.data; return true;
    case GO_TID_i32: *s = *(i32 *)x.data; return true;
    case GO_TID_i64: *s = *(i64 *)x.data; return true;
    case GO_TID_uint:
    case GO_TID_uintptr: *u = *(uintptr_t *)x.data; *isu = true; return true;
    case GO_TID_u8: *u = *(u8 *)x.data; *isu = true; return true;
    case GO_TID_u16: *u = *(u16 *)x.data; *isu = true; return true;
    case GO_TID_u32: *u = *(u32 *)x.data; *isu = true; return true;
    case GO_TID_u64: *u = *(u64 *)x.data; *isu = true; return true;
    }
    return false;
}

typedef struct Spec {
    bool minus, plus, space, zero, sharp;
    int width, prec;  // -1 = unset
} Spec;

static int utf8_count(const char *p, int n) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (((u8)p[i] & 0xC0) != 0x80) c++;
    return c;
}

static void pad_put(Buf *b, const Spec *sp, const char *s, int n, bool numeric) {
    int w = utf8_count(s, n);
    int padn = sp->width > w ? sp->width - w : 0;
    if (sp->minus) {
        b_put(b, s, n);
        b_pad(b, ' ', padn);
    } else if (sp->zero && numeric && padn) {
        // zeros go after the sign
        if (n && (s[0] == '-' || s[0] == '+' || s[0] == ' ')) {
            b_putc(b, s[0]);
            b_pad(b, '0', padn);
            b_put(b, s + 1, n - 1);
        } else {
            b_pad(b, '0', padn);
            b_put(b, s, n);
        }
    } else {
        b_pad(b, ' ', padn);
        b_put(b, s, n);
    }
}

static void quote_into(Buf *b, GoString s) {
    b_putc(b, '"');
    for (int i = 0; i < s.len; i++) {
        u8 c = (u8)s.p[i];
        switch (c) {
        case '"': b_puts(b, "\\\""); break;
        case '\\': b_puts(b, "\\\\"); break;
        case '\n': b_puts(b, "\\n"); break;
        case '\r': b_puts(b, "\\r"); break;
        case '\t': b_puts(b, "\\t"); break;
        case '\a': b_puts(b, "\\a"); break;
        case '\b': b_puts(b, "\\b"); break;
        case '\f': b_puts(b, "\\f"); break;
        case '\v': b_puts(b, "\\v"); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char t[8];
                snprintf(t, sizeof t, "\\x%02x", c);
                b_puts(b, t);
            } else {
                b_putc(b, (char)c);  // UTF-8 sequences are kept (printable assumption)
            }
        }
    }
    b_putc(b, '"');
}

GoString strconv_Quote(GoString s) {
    Buf b = {0};
    quote_into(&b, s);
    return b_str(&b);
}

static void fmt_int(Buf *b, const Spec *sp, char verb, i64 s, u64 u, bool isu) {
    char t[80];
    bool neg = !isu && s < 0;
    u64 mag = isu ? u : (neg ? (u64)0 - (u64)s : (u64)s);
    int n = 0;
    char digs[70];
    int base = 10;
    bool upper = false;
    switch (verb) {
    case 'x': base = 16; break;
    case 'X': base = 16; upper = true; break;
    case 'o': base = 8; break;
    case 'b': base = 2; break;
    case 'c': {
        GoString r = go_rune2str((i32)(isu ? (i64)u : s));
        pad_put(b, sp, r.p, r.len, false);
        return;
    }
    case 'q': {
        GoString r = go_rune2str((i32)(isu ? (i64)u : s));
        Buf q = {0};
        b_putc(&q, '\'');
        b_put(&q, r.p, r.len);
        b_putc(&q, '\'');
        pad_put(b, sp, q.p, q.len, false);
        gc_free(q.p);
        return;
    }
    }
    do {
        int d = (int)(mag % (u64)base);
        digs[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        mag /= (u64)base;
    } while (mag);
    int k = 0;
    if (neg) t[k++] = '-';
    else if (sp->plus) t[k++] = '+';
    else if (sp->space) t[k++] = ' ';
    if (sp->sharp && base == 16) {
        t[k++] = '0';
        t[k++] = upper ? 'X' : 'x';
    }
    if (sp->sharp && base == 8) t[k++] = '0';
    // precision: minimum digits
    for (int i = n; i < sp->prec; i++) t[k++] = '0';
    while (n) t[k++] = digs[--n];
    Spec s2 = *sp;
    if (sp->prec >= 0) s2.zero = false;
    pad_put(b, &s2, t, k, true);
}

static void fmt_value(Buf *b, const Spec *sp, char verb, GoIface x) {
    if (x.type == 0) {
        pad_put(b, sp, "<nil>", 5, false);
        return;
    }
    if (x.type == GO_TID_ERRSTR) {
        GoString s = *(GoString *)x.data;
        if (verb == 'q') {
            Buf q = {0};
            quote_into(&q, s);
            pad_put(b, sp, q.p, q.len, false);
            gc_free(q.p);
        } else {
            pad_put(b, sp, s.p, s.len, false);
        }
        return;
    }
    GoString ms;
    if ((verb == 'v' || verb == 's' || verb == 'q') && ik_fmt_method(x, &ms)) {  // Error() / String()
        if (verb == 'q') {
            Buf q = {0};
            quote_into(&q, ms);
            pad_put(b, sp, q.p, q.len, false);
            gc_free(q.p);
        } else {
            pad_put(b, sp, ms.p, ms.len, false);
        }
        return;
    }
    int k = kind_of(x.type);
    i64 si = 0;
    u64 ui = 0;
    bool isu = false;
    if (k == GO_TID_bool) {
        const char *s = *(bool *)x.data ? "true" : "false";
        pad_put(b, sp, s, (int)strlen(s), false);
        return;
    }
    if (load_int(x, k, &si, &ui, &isu)) {
        char v = verb == 'v' ? 'd' : verb;
        if (v == 's') v = 'd';
        fmt_int(b, sp, v, si, ui, isu);
        return;
    }
    if (k == GO_TID_f32 || k == GO_TID_f64) {
        f64 f = k == GO_TID_f32 ? (f64) * (f32 *)x.data : *(f64 *)x.data;
        int bits = k == GO_TID_f32 ? 32 : 64;
        Buf t = {0};
        char v = verb;
        int prec = sp->prec;
        if (v == 'v') v = 'g';
        else if ((v == 'f' || v == 'F' || v == 'e' || v == 'E') && prec < 0) prec = 6;
        if (v == 's' || v == 'd') v = 'g';
        if (!signbit(f) && f == f) {
            if (sp->plus) b_putc(&t, '+');
            else if (sp->space) b_putc(&t, ' ');
        }
        fmt_float(&t, f, v, prec, bits);
        pad_put(b, sp, t.p, t.len, true);
        gc_free(t.p);
        return;
    }
    if (k == GO_TID_string) {
        GoString s = *(GoString *)x.data;
        if (verb == 'q') {
            Buf q = {0};
            quote_into(&q, s);
            pad_put(b, sp, q.p, q.len, false);
            gc_free(q.p);
        } else if (verb == 'x' || verb == 'X') {
            Buf q = {0};
            for (int i = 0; i < s.len; i++) {
                char t[4];
                snprintf(t, sizeof t, verb == 'x' ? "%02x" : "%02X", (u8)s.p[i]);
                b_puts(&q, t);
            }
            pad_put(b, sp, q.p, q.len, false);
            gc_free(q.p);
        } else {
            int n = s.len;
            if (sp->prec >= 0) {  // precision truncates to runes
                int r = 0, i = 0;
                for (; i < s.len; i++) {
                    if (((u8)s.p[i] & 0xC0) != 0x80) {
                        if (r == sp->prec) break;
                        r++;
                    }
                }
                n = i;
            }
            pad_put(b, sp, s.p, n, false);
        }
        return;
    }
    // pointers, structs, slices: only used in diagnostics
    char t[48];
    snprintf(t, sizeof t, "%%!%c(type%d)", verb, x.type);
    b_puts(b, t);
}

GoString go_sprintf(GoString f, const GoIface *args, int nargs) {
    Buf b = {0};
    int ai = 0;
    for (int i = 0; i < f.len;) {
        char c = f.p[i];
        if (c != '%') {
            int j = i;
            while (j < f.len && f.p[j] != '%') j++;
            b_put(&b, f.p + i, j - i);
            i = j;
            continue;
        }
        i++;
        if (i >= f.len) {
            b_puts(&b, "%!(NOVERB)");
            break;
        }
        if (f.p[i] == '%') {
            b_putc(&b, '%');
            i++;
            continue;
        }
        Spec sp = {0};
        sp.width = -1;
        sp.prec = -1;
        for (; i < f.len; i++) {
            char fl = f.p[i];
            if (fl == '-') sp.minus = true;
            else if (fl == '+') sp.plus = true;
            else if (fl == ' ') sp.space = true;
            else if (fl == '0') sp.zero = !sp.minus;
            else if (fl == '#') sp.sharp = true;
            else break;
        }
        if (i < f.len && f.p[i] == '*') {
            i++;
            if (ai < nargs) {
                i64 s = 0;
                u64 u = 0;
                bool isu;
                if (load_int(args[ai], kind_of(args[ai].type), &s, &u, &isu)) sp.width = (int)(isu ? (i64)u : s);
                ai++;
            }
            if (sp.width < 0) {
                sp.minus = true;
                sp.width = -sp.width;
            }
        } else {
            while (i < f.len && f.p[i] >= '0' && f.p[i] <= '9') {
                if (sp.width < 0) sp.width = 0;
                sp.width = sp.width * 10 + (f.p[i++] - '0');
            }
        }
        if (i < f.len && f.p[i] == '.') {
            i++;
            sp.prec = 0;
            if (i < f.len && f.p[i] == '*') {
                i++;
                if (ai < nargs) {
                    i64 s = 0;
                    u64 u = 0;
                    bool isu;
                    if (load_int(args[ai], kind_of(args[ai].type), &s, &u, &isu)) sp.prec = (int)(isu ? (i64)u : s);
                    ai++;
                }
            } else {
                while (i < f.len && f.p[i] >= '0' && f.p[i] <= '9') sp.prec = sp.prec * 10 + (f.p[i++] - '0');
            }
        }
        if (i >= f.len) {
            b_puts(&b, "%!(NOVERB)");
            break;
        }
        char verb = f.p[i++];
        if (ai >= nargs) {
            b_puts(&b, "%!");
            b_putc(&b, verb);
            b_puts(&b, "(MISSING)");
            continue;
        }
        if (verb == 'T') {
            b_puts(&b, "?");
            ai++;
            continue;
        }
        fmt_value(&b, &sp, verb, args[ai++]);
    }
    if (ai < nargs) b_puts(&b, "%!(EXTRA)");
    return b_str(&b);
}

GoString go_sprint(const GoIface *args, int nargs, bool spaces_always) {
    Buf b = {0};
    Spec sp = {0};
    sp.width = -1;
    sp.prec = -1;
    for (int i = 0; i < nargs; i++) {
        bool isstr = kind_of(args[i].type) == GO_TID_string;
        bool previsstr = i > 0 && kind_of(args[i - 1].type) == GO_TID_string;
        // Sprint adds spaces between operands when neither is a string
        if (i > 0 && (spaces_always || (!isstr && !previsstr))) b_putc(&b, ' ');
        fmt_value(&b, &sp, 'v', args[i]);
    }
    return b_str(&b);
}

GoString fmt_Sprintf(GoString a0, Slice_GoIface a1) { return go_sprintf(a0, a1.p, a1.len); }
GoString fmt_Sprint(Slice_GoIface a0) { return go_sprint(a0.p, a0.len, false); }
GoIface fmt_Errorf(GoString a0, Slice_GoIface a1) { return go_error(go_sprintf(a0, a1.p, a1.len)); }

Ret_int__GoIface fmt_Printf(GoString a0, Slice_GoIface a1) {
    GoString s = go_sprintf(a0, a1.p, a1.len);
    cvf_log("%.*s", s.len, s.p);
    return (Ret_int__GoIface){s.len, {0}};
}
Ret_int__GoIface fmt_Println(Slice_GoIface a0) {
    GoString s = go_sprint(a0.p, a0.len, true);
    cvf_log("%.*s", s.len, s.p);
    return (Ret_int__GoIface){s.len + 1, {0}};
}
Ret_int__GoIface fmt_Fprintf(GoIface w, GoString a1, Slice_GoIface a2) {
    (void)w;  // diagnostics only (stderr / log files)
    GoString s = go_sprintf(a1, a2.p, a2.len);
    cvf_log("%.*s", s.len, s.p);
    return (Ret_int__GoIface){s.len, {0}};
}
void log_Printf(GoString a0, Slice_GoIface a1) {
    GoString s = go_sprintf(a0, a1.p, a1.len);
    cvf_log("%.*s", s.len, s.p);
}

GoString strconv_Itoa(int a0) {
    char t[24];
    int n = snprintf(t, sizeof t, "%d", a0);
    char *p = (char *)gc_alloc_atomic((size_t)n + 1);
    memcpy(p, t, (size_t)n + 1);
    return (GoString){p, n};
}

GoString strconv_FormatFloat(f64 f, u8 fmt, int prec, int bits) { return go_format_float(f, (char)fmt, prec, bits); }

Slice_u8 strconv_AppendInt(Slice_u8 b, i64 v, int base) {
    char t[72];
    int n = 0;
    bool neg = v < 0;
    u64 m = neg ? (u64)0 - (u64)v : (u64)v;
    do {
        int d = (int)(m % (u64)base);
        t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        m /= (u64)base;
    } while (m);
    if (neg) b = go_append_Slice_u8(b, '-');
    while (n) b = go_append_Slice_u8(b, (u8)t[--n]);
    return b;
}

Slice_u8 strconv_AppendUint(Slice_u8 b, u64 m, int base) {
    char t[72];
    int n = 0;
    do {
        int d = (int)(m % (u64)base);
        t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        m /= (u64)base;
    } while (m);
    while (n) b = go_append_Slice_u8(b, (u8)t[--n]);
    return b;
}
