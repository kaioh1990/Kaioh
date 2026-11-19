#include "formats/air.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "formats/text.h"

// ---------------------------------------------------------------- table
void cvf_animtable_init(CvfAnimTable *t) {
    memset(t, 0, sizeof *t);
    cvf_arena_init_growable(&t->arena, 1 << 20);
}

void cvf_animtable_free(CvfAnimTable *t) {
    // Definitions are malloc'd individually (frames can be large); free them.
    for (i32 i = 0; i < t->cap; i++) (void)i;
    free(t->keys);
    free(t->vals);
    cvf_arena_free(&t->arena);
    memset(t, 0, sizeof *t);
}

static u32 hash_i32(i32 k) {
    u32 x = (u32)k;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static i32 slot_of(const CvfAnimTable *t, i32 no) {
    if (!t->cap) return -1;
    u32 m = (u32)t->cap - 1;
    for (u32 i = hash_i32(no) & m;; i = (i + 1) & m) {
        if (!t->vals[i]) return -(i32)i - 2;  // empty slot (encoded)
        if (t->keys[i] == no) return (i32)i;
    }
}

CvfAnimDef *cvf_animtable_get(const CvfAnimTable *t, i32 no) {
    i32 s = slot_of(t, no);
    return s >= 0 ? t->vals[s] : NULL;
}

static void table_put(CvfAnimTable *t, i32 no, CvfAnimDef *a) {
    if ((t->count + 1) * 2 > t->cap) {
        i32 ncap = t->cap ? t->cap * 2 : 256;
        i32 *ok = t->keys;
        CvfAnimDef **ov = t->vals;
        i32 ocap = t->cap;
        t->keys = (i32 *)calloc((size_t)ncap, sizeof(i32));
        t->vals = (CvfAnimDef **)calloc((size_t)ncap, sizeof(CvfAnimDef *));
        t->cap = ncap;
        t->count = 0;
        for (i32 i = 0; i < ocap; i++)
            if (ov[i]) table_put(t, ok[i], ov[i]);
        free(ok);
        free(ov);
    }
    i32 s = slot_of(t, no);
    if (s >= 0) {
        t->vals[s] = a;
        return;
    }
    s = -s - 2;
    t->keys[s] = no;
    t->vals[s] = a;
    t->count++;
}

static void table_delete(CvfAnimTable *t, i32 no) {
    // Rebuild without the key (deletions only happen at finalize).
    i32 s = slot_of(t, no);
    if (s < 0) return;
    t->vals[s] = NULL;
    i32 ocap = t->cap;
    i32 *ok = t->keys;
    CvfAnimDef **ov = t->vals;
    t->keys = (i32 *)calloc((size_t)ocap, sizeof(i32));
    t->vals = (CvfAnimDef **)calloc((size_t)ocap, sizeof(CvfAnimDef *));
    t->count = 0;
    for (i32 i = 0; i < ocap; i++)
        if (ov[i]) table_put(t, ok[i], ov[i]);
    free(ok);
    free(ov);
}

// ---------------------------------------------------------------- numbers
bool cvf_is_numeric(CvfStr s) {
    s = cvf_str_trim(s);
    if (s.n == 0 || s.n > 255) return false;
    char buf[256];
    memcpy(buf, s.p, s.n);
    buf[s.n] = 0;
    // Go accepts: [+-] (digits [. digits] | . digits) [e[+-]digits], inf,
    // infinity, nan (any case), hex floats, and '_' separators only in
    // base-prefixed forms. Validate the decimal form strictly; let strtod
    // handle the rest.
    const char *p = buf;
    if (*p == '+' || *p == '-') p++;
    char low[16];
    size_t ln = strlen(p);
    if (ln < sizeof low) {
        for (size_t k = 0; k <= ln; k++) low[k] = cvf_lower(p[k]);
        if (!strcmp(low, "inf") || !strcmp(low, "infinity")) return true;
        if (!strcmp(low, "nan")) return buf[0] != '+' && buf[0] != '-';
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        char *end;
        errno = 0;
        double v = strtod(buf, &end);
        return *end == 0 && end != buf && !(errno == ERANGE && isinf(v));
    }
    bool digits = false;
    while (*p >= '0' && *p <= '9') p++, digits = true;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++, digits = true;
    }
    if (!digits) return false;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return false;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p) return false;
    errno = 0;
    double v = strtod(buf, NULL);
    return !(errno == ERANGE && isinf(v));
}

// ---------------------------------------------------------------- frames
typedef struct Compiler {
    CvfAnimTable *t;
    CvfLines L;  // trimmed lines
    int i;
    i32 anim_no;
} Compiler;

// SplitN(line, ",", 10) + TrimSpace
static int split_commas(CvfStr line, CvfStr *out, int maxn) {
    int n = 0;
    size_t s = 0;
    for (size_t k = 0; k <= line.n; k++) {
        if (k == line.n || (line.p[k] == ',' && n < maxn - 1)) {
            out[n++] = cvf_str_trim(cvf_strn(line.p + s, k - s));
            s = k + 1;
        }
    }
    return n;
}

static void parse_alpha(CvfStr a, size_t start, u8 defsrc, u8 defdst, u8 *src, u8 *dst) {
    *src = defsrc;
    *dst = defdst;
    size_t i = start;
    int alp = 0;
    for (; i < a.n && a.p[i] >= '0' && a.p[i] <= '9'; i++) alp = alp * 10 + (a.p[i] - '0');
    alp &= 0x3fff;
    if (alp > 255) alp = 255;
    *src = (u8)alp;
    if (i < a.n && a.p[i] == 'd') {
        i++;
        if (i < a.n && a.p[i] >= '0' && a.p[i] <= '9') {
            alp = 0;
            for (; i < a.n && a.p[i] >= '0' && a.p[i] <= '9'; i++) alp = alp * 10 + (a.p[i] - '0');
            alp &= 0x3fff;
            if (alp > 255) alp = 255;
            *dst = (u8)alp;
        }
    }
}

static bool has_prefix(CvfStr s, const char *p) {
    size_t n = strlen(p);
    return s.n >= n && memcmp(s.p, p, n) == 0;
}

// ReadAnimFrame. The line is already lowercased. Returns false when the
// line is not a frame.
static bool read_frame(CvfStr line, CvfAnimFrame *af) {
    if (line.n == 0 || ((line.p[0] < '0' || line.p[0] > '9') && line.p[0] != '-')) return false;
    CvfStr ary[10];
    int n = split_commas(line, ary, 10);
    if (n < 5) return false;  // "Invalid animation element definition": no frame
    *af = (CvfAnimFrame){.time = -1, .group = -1, .trans = CVF_TT_NONE, .src_alpha = 255, .dst_alpha = 0,
                         .hscale = 1, .vscale = 1, .xscale = 1, .yscale = 1};
    af->group = cvf_atoi(ary[0]);
    af->number = cvf_atoi(ary[1]);
    af->xoffset = cvf_atoi(ary[2]);
    af->yoffset = cvf_atoi(ary[3]);
    af->time = cvf_atoi(ary[4]);
    if (n >= 6 && ary[5].n) {
        // Go ranges over runes; content is ASCII here.
        for (size_t k = 0; k < ary[5].n; k++) {
            char ch = ary[5].p[k];
            if (ch == 'h' || ch == 'H') { af->hscale = -1; af->xoffset *= -1; }
            else if (ch == 'v' || ch == 'V') { af->vscale = -1; af->yoffset *= -1; }
        }
    }
    if (n >= 7 && ary[6].n) {
        CvfStr a = ary[6];
        for (size_t k = 0; k < a.n; k++) {
            char c = a.p[k];
            if (c == 'a' || c == 's' || c == 'A' || c == 'S') { a = cvf_strn(a.p + k, a.n - k); break; }
        }
        if (a.n == 2 && a.p[0] == 'a' && a.p[1] == '1') {
            af->trans = CVF_TT_ADD; af->src_alpha = 255; af->dst_alpha = 128;
        } else if (has_prefix(a, "sas")) {
            af->trans = CVF_TT_SUBADD; parse_alpha(a, 3, 255, 255, &af->src_alpha, &af->dst_alpha);
        } else if (has_prefix(a, "sa")) {
            af->trans = CVF_TT_SUBADD; af->src_alpha = 255; af->dst_alpha = 255;
        } else if (has_prefix(a, "as")) {
            af->trans = CVF_TT_ADD; parse_alpha(a, 2, 255, 0, &af->src_alpha, &af->dst_alpha);
        } else if (has_prefix(a, "ss")) {
            af->trans = CVF_TT_SUB; parse_alpha(a, 2, 255, 255, &af->src_alpha, &af->dst_alpha);
        } else if (a.n >= 1 && a.p[0] == 'a') {
            af->trans = CVF_TT_ADD; af->src_alpha = 255; af->dst_alpha = 255;
        } else if (a.n >= 1 && a.p[0] == 's') {
            af->trans = CVF_TT_SUB; af->src_alpha = 255; af->dst_alpha = 255;
        }
    }
    if (n >= 8 && ary[7].n && cvf_is_numeric(ary[7])) af->xscale = (f32)cvf_atof(ary[7]);
    if (n >= 9 && ary[8].n && cvf_is_numeric(ary[8])) af->yscale = (f32)cvf_atof(ary[8]);
    if (n >= 10 && ary[9].n && cvf_is_numeric(ary[9])) af->angle = (f32)cvf_atof(ary[9]);
    return true;
}

// Lowercased, comment-stripped, trimmed copy of line i into buf.
static CvfStr clean_line(const Compiler *ac, int i, char *buf, size_t cap) {
    CvfStr ln = ac->L.lines[i];
    int sc = cvf_str_find(ln, ';');
    if (sc >= 0) ln.n = (size_t)sc;
    ln = cvf_str_trim(ln);
    size_t n = CVF_MIN(ln.n, cap - 1);
    for (size_t k = 0; k < n; k++) buf[k] = cvf_lower(ln.p[k]);
    buf[n] = 0;
    return cvf_strn(buf, n);
}

static void normalize_rect(f32 r[4]) {
    if (r[0] > r[2]) { f32 t = r[0]; r[0] = r[2]; r[2] = t; }
    if (r[1] > r[3]) { f32 t = r[1]; r[1] = r[3]; r[3] = t; }
}

static CvfClsn read_clsn(Compiler *ac, i32 size) {
    CvfClsn c = {0};
    c.count = size;
    c.box = (f32(*)[4])cvf_arena_zalloc(&ac->t->arena, sizeof(f32[4]) * (size_t)(size ? size : 1), 4);
    if (size == 0) return c;
    bool *filled = (bool *)calloc((size_t)size, 1);
    ac->i++;
    i32 count = 0;
    char buf[1024];
    while (count < size && ac->i < ac->L.count) {
        CvfStr r = clean_line(ac, ac->i, buf, sizeof buf);
        if (r.n == 0) { ac->i++; continue; }
        if (r.n < 4 || memcmp(r.p, "clsn", 4) != 0) break;
        int eq = cvf_str_find(r, '=');
        if (eq < 0) break;
        i32 box = -1;
        int br = cvf_str_find(r, '[');
        if (br >= 0 && br < eq) {
            int be = cvf_str_find(cvf_strn(r.p + br, r.n - (size_t)br), ']');
            if (be > 0) {
                CvfStr idx = cvf_str_trim(cvf_strn(r.p + br + 1, (size_t)be - 1));
                bool valid = idx.n > 0;
                for (size_t k = 0; valid && k < idx.n; k++)
                    if (idx.p[k] < '0' || idx.p[k] > '9') valid = false;
                if (valid) box = cvf_atoi(idx);
            }
        }
        if (box < 0 || box >= size || filled[box]) {
            box = -1;
            for (i32 k = 0; k < size; k++)
                if (!filled[k]) { box = k; break; }
            if (box < 0) { ac->i++; continue; }
        }
        CvfStr rest = cvf_strn(r.p + eq + 1, r.n - (size_t)eq - 1);
        CvfStr parts[64];
        int np = 0;
        size_t s = 0;
        for (size_t k = 0; k <= rest.n && np < 64; k++)
            if (k == rest.n || rest.p[k] == ',') { parts[np++] = cvf_strn(rest.p + s, k - s); s = k + 1; }
        if (np < 4) break;
        f32 rc[4] = {(f32)cvf_atoi(parts[0]), (f32)cvf_atoi(parts[1]), (f32)cvf_atoi(parts[2]), (f32)cvf_atoi(parts[3])};
        normalize_rect(rc);
        memcpy(c.box[box], rc, sizeof rc);
        filled[box] = true;
        count++;
        ac->i++;
    }
    ac->i--;
    free(filled);
    return c;
}

typedef struct Vec32 { i32 *p; i32 n, cap; } Vec32;
static void vpush(Vec32 *v, i32 x) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 8; v->p = (i32 *)realloc(v->p, sizeof(i32) * (size_t)v->cap); }
    v->p[v->n++] = x;
}

static i32 *arena_copy_i32(CvfArena *a, const Vec32 *v) {
    if (!v->n) return NULL;
    i32 *d = CVF_NEW_N(a, i32, v->n);
    memcpy(d, v->p, sizeof(i32) * (size_t)v->n);
    return d;
}

// strconv.ParseInt(numStr, 10, 32)
static bool parse_int32(CvfStr s, i32 *out) {
    if (s.n == 0) return false;
    size_t i = 0;
    bool neg = false;
    if (s.p[0] == '+' || s.p[0] == '-') { neg = s.p[0] == '-'; i = 1; }
    if (i == s.n) return false;
    i64 v = 0;
    for (; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') return false;
        v = v * 10 + (s.p[i] - '0');
        if (v > (i64)INT32_MAX + 1) return false;
    }
    if (neg) v = -v;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    *out = (i32)v;
    return true;
}

static CvfAnimDef *read_animation(Compiler *ac) {
    CvfAnimDef *a = CVF_NEW(&ac->t->arena, CvfAnimDef);
    a->action_no = ac->anim_no;
    a->copy_action = -1;
    a->mask = 0;
    i32 ols = 0;
    CvfClsn clsn1 = {0}, clsn1d = {0}, clsn2 = {0}, clsn2d = {0};
    bool def1 = true, def2 = true;
    struct { CvfAnimFrame *p; i32 n, cap; } fr = {0};
    Vec32 io = {0}, isc = {0}, ian = {0}, ibl = {0};
    char buf[2048];
    for (; ac->i < ac->L.count; ac->i++) {
        CvfStr raw = ac->L.lines[ac->i];
        if (raw.n > 0 && raw.p[0] == '[') break;
        CvfStr line = clean_line(ac, ac->i, buf, sizeof buf);
        if (line.n >= 12 && memcmp(line.p, "copy action ", 12) == 0) {
            CvfStr num = cvf_str_trim(cvf_strn(line.p + 12, line.n - 12));
            i32 id;
            a->copy_action = parse_int32(num, &id) ? id : -1;
            goto done;
        }
        CvfAnimFrame af;
        if (read_frame(line, &af)) {
            ols = a->loopstart;
            if (def1) clsn1 = clsn1d;
            if (def2) clsn2 = clsn2d;
            af.clsn1 = clsn1;
            af.clsn2 = clsn2;
            if (fr.n == fr.cap) { fr.cap = fr.cap ? fr.cap * 2 : 8; fr.p = (CvfAnimFrame *)realloc(fr.p, sizeof(CvfAnimFrame) * (size_t)fr.cap); }
            fr.p[fr.n++] = af;
            def1 = def2 = true;
        } else if (line.n >= 9 && memcmp(line.p, "loopstart", 9) == 0) {
            a->loopstart = fr.n;
        } else if (line.n >= 18 && memcmp(line.p, "interpolate offset", 18) == 0) {
            vpush(&io, fr.n);
        } else if (line.n >= 17 && memcmp(line.p, "interpolate scale", 17) == 0) {
            vpush(&isc, fr.n);
        } else if (line.n >= 17 && memcmp(line.p, "interpolate angle", 17) == 0) {
            vpush(&ian, fr.n);
        } else if (line.n >= 17 && memcmp(line.p, "interpolate blend", 17) == 0) {
            vpush(&ibl, fr.n);
        } else if (line.n >= 5 && memcmp(line.p, "clsn", 4) == 0) {
            int ii = cvf_str_find(line, ':');
            if (ii < 0) continue;
            i32 size = cvf_atoi(cvf_strn(line.p + ii + 1, line.n - (size_t)ii - 1));
            if (size < 0) continue;
            char ct = line.p[4];
            bool isdef = line.n >= 12 && memcmp(line.p + 5, "default", 7) == 0;
            if (ct == '1') {
                clsn1 = read_clsn(ac, size);
                if (isdef) clsn1d = clsn1;
                def1 = false;
            } else if (ct == '2') {
                clsn2 = read_clsn(ac, size);
                if (isdef) clsn2d = clsn2;
                def2 = false;
            }
        }
    }
    if (a->loopstart >= fr.n) a->loopstart = ols;
    if (fr.n == 0) {
    } else if (fr.p[fr.n - 1].time == -1) {
        a->totaltime = -1;
    } else {
        i32 tmp = 0;
        for (i32 i = 0; i < fr.n; i++) {
            const CvfAnimFrame *f = &fr.p[i];
            if (f->time == -1) {
                a->totaltime = 0;
                a->looptime = -tmp;
                a->prelooptime = 0;
            }
            a->totaltime += f->time;
            if (i < a->loopstart) {
                a->prelooptime += f->time;
                tmp += f->time;
            } else {
                a->looptime += f->time;
            }
        }
        if (a->totaltime == -1) a->prelooptime = 0;
    }
done:
    if (fr.n) {
        a->frames = CVF_NEW_N(&ac->t->arena, CvfAnimFrame, fr.n);
        memcpy(a->frames, fr.p, sizeof(CvfAnimFrame) * (size_t)fr.n);
    }
    a->nframes = fr.n;
    a->interp_offset = arena_copy_i32(&ac->t->arena, &io); a->n_interp_offset = io.n;
    a->interp_scale = arena_copy_i32(&ac->t->arena, &isc); a->n_interp_scale = isc.n;
    a->interp_angle = arena_copy_i32(&ac->t->arena, &ian); a->n_interp_angle = ian.n;
    a->interp_blend = arena_copy_i32(&ac->t->arena, &ibl); a->n_interp_blend = ibl.n;
    free(fr.p); free(io.p); free(isc.p); free(ian.p); free(ibl.p);
    return a;
}

static CvfAnimDef *read_action(Compiler *ac, i32 *no) {
    char name[64];
    CvfStr sub = {0};
    name[0] = 0;
    for (; ac->i < ac->L.count; ac->i++)
        if (cvf_section_name(ac->L.lines[ac->i], name, sizeof name, &sub)) break;
    if (strcmp(name, "begin ") != 0) return NULL;
    int spi = cvf_str_find(sub, ' ');
    if (spi < 0) return NULL;
    if (!cvf_str_ieq(cvf_strn(sub.p, (size_t)spi + 1), "action ")) return NULL;
    ac->i++;
    *no = cvf_atoi(cvf_strn(sub.p + spi + 1, sub.n - (size_t)spi - 1));
    ac->anim_no = *no;
    return read_animation(ac);
}

static CvfAnimDef *compile_action(Compiler *ac) {
    while (ac->i < ac->L.count) {
        i32 outer = ac->anim_no;
        i32 no = 0;
        CvfAnimDef *a = read_action(ac, &no);
        ac->anim_no = outer;
        if (a) {
            CvfAnimDef *existing = cvf_animtable_get(ac->t, no);
            if (existing) return existing;
            while (a->nframes == 0 && ac->i < ac->L.count && a->copy_action < 0) {
                CvfAnimDef *a2 = compile_action(ac);
                if (a2) {
                    *a = *a2;
                    break;
                }
                ac->i++;
            }
            table_put(ac->t, no, a);
            return a;
        }
        ac->i++;
    }
    return NULL;
}

void cvf_air_compile_text(CvfAnimTable *t, char *text, size_t len) {
    Compiler ac = {.t = t};
    cvf_lines_split(&ac.L, text, len, false);
    for (int k = 0; k < ac.L.count; k++) ac.L.lines[k] = cvf_str_trim(ac.L.lines[k]);
    while (compile_action(&ac)) {
    }
    free(ac.L.lines);  // text is owned by the caller
}

bool cvf_air_compile_file(CvfAnimTable *t, const char *path) {
    size_t len;
    u8 *buf = cvf_read_file(path, &len);
    if (!buf) return false;
    // Text stays referenced by nothing after compilation (all data copied).
    cvf_air_compile_text(t, (char *)buf, len);
    free(buf);
    return true;
}

void cvf_air_finalize(CvfAnimTable *t) {
    i32 *del = NULL;
    i32 nd = 0;
    for (i32 s = 0; s < t->cap; s++) {
        CvfAnimDef *a = t->vals[s];
        if (!a) continue;
        for (int loops = 0; a->copy_action >= 0 && loops < 8; loops++) {
            CvfAnimDef *target = cvf_animtable_get(t, a->copy_action);
            if (!target || target == a) break;
            if (target->copy_action >= 0) {
                a->copy_action = target->copy_action;
                continue;
            }
            *a = *target;
            a->copy_action = -1;
            break;
        }
        if (a->copy_action >= 0) {
            del = (i32 *)realloc(del, sizeof(i32) * (size_t)(nd + 1));
            del[nd++] = t->keys[s];
        }
    }
    for (i32 k = 0; k < nd; k++) table_delete(t, del[k]);
    free(del);
}
