// regexp subset (RE2 syntax as used by Ikemen) with a backtracking matcher.
// Leftmost-first semantics, like Go's regexp for the APIs used here
// (MatchString, FindAllString, FindStringSubmatch, ReplaceAllString).
// Supported: literals, . [classes] \d\w\s\b (and negations), ^ $, groups
// (capturing, (?:...), (?flags) and (?flags:...) with i s m), | and the
// quantifiers * + ? {n} {n,} {n,m} with lazy variants. \p{L}/\p{N} are
// approximated (non-ASCII runes count as letters).
#include "ik/ik_sim.h"

enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_GROUP, N_GEND, N_ALT, N_REP, N_EMPTY };

typedef struct RxRange {
    i32 lo, hi;
} RxRange;

typedef struct RxNode RxNode;
typedef struct RxSeq {
    RxNode *v;
    int n, cap;
} RxSeq;

struct RxNode {
    int kind;
    i32 c;          // N_CHAR
    bool fold;      // case-insensitive (CHAR, CLASS)
    bool dotall;    // N_ANY
    bool multi;     // N_BOL / N_EOL
    RxRange *rg;      // N_CLASS
    int nrg;
    bool neg;
    int group;      // N_GROUP / N_GEND (index), -1 for non-capturing
    RxSeq *alts;      // N_ALT: alternatives; N_GROUP: alts[0] = body (ends with N_GEND)
    int nalts;
    int min, max;   // N_REP (max -1 = inf)
    bool greedy;
    RxSeq *body;      // N_REP
};

typedef struct RxRe {
    RxSeq top;
    int ncap;  // capture groups (excluding group 0)
    GoString src;
} RxRe;

// ---------------------------------------------------------------- parser
typedef struct RxP {
    const char *s;
    int i, n;
    bool fi, fs, fm;  // flags
    int ncap;
    const char *err;
} RxP;

static void seq_push(RxSeq *q, RxNode nd) {
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 4;
        q->v = (RxNode *)realloc(q->v, sizeof(RxNode) * (size_t)q->cap);
    }
    q->v[q->n++] = nd;
}

static int dec_rune(const char *p, int n, i32 *r) {
    u8 c = (u8)p[0];
    if (c < 0x80 || n < 2) {
        *r = c;
        return 1;
    }
    int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    if (len > n) {
        *r = c;
        return 1;
    }
    i32 v = c & (0x7F >> len);
    for (int k = 1; k < len; k++) v = (v << 6) | ((u8)p[k] & 0x3F);
    *r = v;
    return len;
}

static void add_range(RxRange **rg, int *n, i32 lo, i32 hi) {
    *rg = (RxRange *)realloc(*rg, sizeof(RxRange) * (size_t)(*n + 1));
    (*rg)[(*n)++] = (RxRange){lo, hi};
}

// \d \w \s classes as ranges
static bool add_perl(RxRange **rg, int *n, char c, bool *negated) {
    *negated = false;
    switch (c) {
    case 'D': *negated = true; /* fallthrough */
    case 'd': add_range(rg, n, '0', '9'); return true;
    case 'W': *negated = true; /* fallthrough */
    case 'w':
        add_range(rg, n, '0', '9');
        add_range(rg, n, 'A', 'Z');
        add_range(rg, n, 'a', 'z');
        add_range(rg, n, '_', '_');
        return true;
    case 'S': *negated = true; /* fallthrough */
    case 's':
        add_range(rg, n, '\t', '\n');
        add_range(rg, n, '\f', '\r');
        add_range(rg, n, ' ', ' ');
        return true;
    }
    return false;
}

static i32 esc_char(char c) {
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    }
    return (u8)c;
}

static RxRange *negate(RxRange *rg, int n, int *on) {
    // sort ranges (insertion) then complement over [0, 0x10FFFF]
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && rg[j].lo < rg[j - 1].lo; j--) {
            RxRange t = rg[j];
            rg[j] = rg[j - 1];
            rg[j - 1] = t;
        }
    RxRange *out = NULL;
    int k = 0;
    i32 next = 0;
    for (int i = 0; i < n; i++) {
        if (rg[i].lo > next) add_range(&out, &k, next, rg[i].lo - 1);
        if (rg[i].hi + 1 > next) next = rg[i].hi + 1;
    }
    if (next <= 0x10FFFF) add_range(&out, &k, next, 0x10FFFF);
    free(rg);
    *on = k;
    return out;
}

static void parse_alt(RxP *p, RxSeq **alts, int *nalts);

static RxNode parse_class(RxP *p) {
    RxNode nd = {0};
    nd.kind = N_CLASS;
    nd.fold = p->fi;
    p->i++;  // [
    if (p->i < p->n && p->s[p->i] == '^') {
        nd.neg = true;
        p->i++;
    }
    bool first = true;
    while (p->i < p->n && (p->s[p->i] != ']' || first)) {
        first = false;
        i32 lo;
        if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
            char e = p->s[p->i + 1];
            bool ng;
            RxRange *sub = NULL;
            int ns = 0;
            if (add_perl(&sub, &ns, e, &ng)) {
                if (ng) sub = negate(sub, ns, &ns);
                for (int k = 0; k < ns; k++) add_range(&nd.rg, &nd.nrg, sub[k].lo, sub[k].hi);
                free(sub);
                p->i += 2;
                continue;
            }
            if (e == 'p' || e == 'P') {  // \pL, \p{L}, \p{N}: approximate
                int j = p->i + 2;
                char cls = 'L';
                if (j < p->n && p->s[j] == '{') {
                    cls = p->s[j + 1];
                    while (j < p->n && p->s[j] != '}') j++;
                    j++;
                } else if (j < p->n) {
                    cls = p->s[j++];
                }
                if (cls == 'N') add_range(&nd.rg, &nd.nrg, '0', '9');
                else {
                    add_range(&nd.rg, &nd.nrg, 'A', 'Z');
                    add_range(&nd.rg, &nd.nrg, 'a', 'z');
                    add_range(&nd.rg, &nd.nrg, 0xC0, 0x10FFFF);
                }
                p->i = j;
                continue;
            }
            lo = esc_char(e);
            p->i += 2;
        } else {
            p->i += dec_rune(p->s + p->i, p->n - p->i, &lo);
        }
        i32 hi = lo;
        if (p->i + 1 < p->n && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {
            p->i++;
            if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
                hi = esc_char(p->s[p->i + 1]);
                p->i += 2;
            } else {
                p->i += dec_rune(p->s + p->i, p->n - p->i, &hi);
            }
        }
        add_range(&nd.rg, &nd.nrg, lo, hi);
    }
    if (p->i >= p->n) p->err = "missing ]";
    p->i++;  // ]
    if (nd.fold) {  // add the other case of ASCII letters
        int n0 = nd.nrg;
        for (int k = 0; k < n0; k++) {
            i32 lo = nd.rg[k].lo > 'A' ? nd.rg[k].lo : 'A', hi = nd.rg[k].hi < 'Z' ? nd.rg[k].hi : 'Z';
            if (lo <= hi) add_range(&nd.rg, &nd.nrg, lo + 32, hi + 32);
            lo = nd.rg[k].lo > 'a' ? nd.rg[k].lo : 'a', hi = nd.rg[k].hi < 'z' ? nd.rg[k].hi : 'z';
            if (lo <= hi) add_range(&nd.rg, &nd.nrg, lo - 32, hi - 32);
        }
    }
    if (nd.neg) {
        nd.rg = negate(nd.rg, nd.nrg, &nd.nrg);
        nd.neg = false;
    }
    return nd;
}

static bool parse_flags(RxP *p, int j, bool *fi, bool *fs, bool *fm, int *end, bool *colon) {
    // at "(?" + flags, j points after "(?"
    bool on = true;
    for (; j < p->n; j++) {
        char c = p->s[j];
        if (c == 'i') *fi = on;
        else if (c == 's') *fs = on;
        else if (c == 'm') *fm = on;
        else if (c == 'U') continue;
        else if (c == '-') on = false;
        else if (c == ')' || c == ':') {
            *colon = c == ':';
            *end = j + 1;
            return true;
        } else return false;
    }
    return false;
}

static void parse_seq(RxP *p, RxSeq *q) {
    while (p->i < p->n && !p->err) {
        char c = p->s[p->i];
        if (c == '|' || c == ')') return;
        RxNode nd = {0};
        if (c == '(') {
            int start = p->i;
            p->i++;
            int group = -1;
            bool sfi = p->fi, sfs = p->fs, sfm = p->fm;
            bool scoped = false;
            if (p->i < p->n && p->s[p->i] == '?') {
                if (p->i + 1 < p->n && p->s[p->i + 1] == 'P' && p->i + 2 < p->n && p->s[p->i + 2] == '<') {
                    while (p->i < p->n && p->s[p->i] != '>') p->i++;
                    p->i++;
                    group = ++p->ncap;
                } else {
                    int end;
                    bool colon;
                    bool fi = p->fi, fs = p->fs, fm = p->fm;
                    if (!parse_flags(p, p->i + 1, &fi, &fs, &fm, &end, &colon)) {
                        p->err = "bad flags";
                        return;
                    }
                    p->fi = fi, p->fs = fs, p->fm = fm;
                    p->i = end;
                    if (!colon) continue;  // (?flags) applies to the rest of the group
                    scoped = true;
                }
            } else {
                group = ++p->ncap;
            }
            (void)start;
            nd.kind = N_GROUP;
            nd.group = group;
            parse_alt(p, &nd.alts, &nd.nalts);
            if (p->i >= p->n || p->s[p->i] != ')') {
                p->err = "missing )";
                return;
            }
            p->i++;
            if (scoped || group >= 0) p->fi = sfi, p->fs = sfs, p->fm = sfm;
        } else if (c == '[') {
            nd = parse_class(p);
        } else if (c == '.') {
            nd.kind = N_ANY;
            nd.dotall = p->fs;
            p->i++;
        } else if (c == '^') {
            nd.kind = N_BOL;
            nd.multi = p->fm;
            p->i++;
        } else if (c == '$') {
            nd.kind = N_EOL;
            nd.multi = p->fm;
            p->i++;
        } else if (c == '\\' && p->i + 1 < p->n) {
            char e = p->s[p->i + 1];
            bool ng;
            RxRange *rg = NULL;
            int nr = 0;
            if (add_perl(&rg, &nr, e, &ng)) {
                nd.kind = N_CLASS;
                nd.rg = ng ? negate(rg, nr, &nr) : rg;
                nd.nrg = nr;
                p->i += 2;
            } else if (e == 'b' || e == 'B') {
                nd.kind = e == 'b' ? N_WORDB : N_NWORDB;
                p->i += 2;
            } else if (e == 'p' || e == 'P') {
                int save = p->i;
                (void)save;
                // reuse the class parser on a synthetic "[\p..]"
                nd.kind = N_CLASS;
                int j = p->i + 2;
                char cls = 'L';
                if (j < p->n && p->s[j] == '{') {
                    cls = p->s[j + 1];
                    while (j < p->n && p->s[j] != '}') j++;
                    j++;
                } else if (j < p->n) {
                    cls = p->s[j++];
                }
                if (cls == 'N') add_range(&nd.rg, &nd.nrg, '0', '9');
                else {
                    add_range(&nd.rg, &nd.nrg, 'A', 'Z');
                    add_range(&nd.rg, &nd.nrg, 'a', 'z');
                    add_range(&nd.rg, &nd.nrg, 0xC0, 0x10FFFF);
                }
                if (e == 'P') nd.rg = negate(nd.rg, nd.nrg, &nd.nrg);
                p->i = j;
            } else {
                nd.kind = N_CHAR;
                nd.c = esc_char(e);
                nd.fold = p->fi;
                p->i += 2;
            }
        } else if (c == '*' || c == '+' || c == '?' || (c == '{' && q->n)) {
            if (!q->n) {
                p->err = "missing argument to repetition operator";
                return;
            }
            int mn = 0, mx = -1;
            int j = p->i + 1;
            if (c == '+') mn = 1;
            else if (c == '?') mx = 1;
            else if (c == '{') {
                int k = j;
                int a = 0, b = -1;
                bool any = false;
                while (k < p->n && p->s[k] >= '0' && p->s[k] <= '9') a = a * 10 + (p->s[k++] - '0'), any = true;
                if (!any) goto literal;
                if (k < p->n && p->s[k] == ',') {
                    k++;
                    if (k < p->n && p->s[k] >= '0' && p->s[k] <= '9') {
                        b = 0;
                        while (k < p->n && p->s[k] >= '0' && p->s[k] <= '9') b = b * 10 + (p->s[k++] - '0');
                    }
                } else {
                    b = a;
                }
                if (k >= p->n || p->s[k] != '}') goto literal;
                mn = a, mx = b;
                j = k + 1;
            }
            RxNode rep = {0};
            rep.kind = N_REP;
            rep.min = mn;
            rep.max = mx;
            rep.greedy = true;
            if (j < p->n && p->s[j] == '?') {
                rep.greedy = false;
                j++;
            }
            rep.body = (RxSeq *)calloc(1, sizeof(RxSeq));
            seq_push(rep.body, q->v[q->n - 1]);
            q->v[q->n - 1] = rep;
            p->i = j;
            continue;
        } else {
        literal:
            nd.kind = N_CHAR;
            nd.fold = p->fi;
            p->i += dec_rune(p->s + p->i, p->n - p->i, &nd.c);
        }
        seq_push(q, nd);
    }
}

static void parse_alt(RxP *p, RxSeq **alts, int *nalts) {
    *alts = NULL;
    *nalts = 0;
    for (;;) {
        *alts = (RxSeq *)realloc(*alts, sizeof(RxSeq) * (size_t)(*nalts + 1));
        RxSeq *q = &(*alts)[(*nalts)++];
        memset(q, 0, sizeof *q);
        parse_seq(p, q);
        if (p->err || p->i >= p->n || p->s[p->i] != '|') break;
        p->i++;
    }
}

static RxRe *re_compile(GoString src, const char **err) {
    RxP p = {src.p, 0, src.len, false, false, false, 0, NULL};
    RxRe *re = (RxRe *)calloc(1, sizeof *re);
    RxNode top = {0};
    top.kind = N_GROUP;
    top.group = 0;
    parse_alt(&p, &top.alts, &top.nalts);
    if (!p.err && p.i < p.n) p.err = "unexpected )";
    if (p.err) {
        *err = p.err;
        free(re);
        return NULL;
    }
    seq_push(&re->top, top);
    re->ncap = p.ncap;
    re->src = src;
    return re;
}

// ---------------------------------------------------------------- matcher
typedef struct RxCont {
    int kind;  // 0 = sequence, 1 = repetition, 2 = group end
    const RxSeq *seq;
    int idx;
    const RxNode *rep;
    int cnt, start;
    int group;
    const struct RxCont *next;
} RxCont;

typedef struct RxM {
    const char *s;
    int n;
    int *cap;  // 2 * (ncap + 1)
    int steps;
} RxM;

static bool run(RxM *m, const RxSeq *q, int idx, int i, const RxCont *k);
static bool rep(RxM *m, const RxNode *r, int cnt, int i, const RxCont *after);

static bool cont(RxM *m, const RxCont *k, int i) {
    if (!k) return true;
    switch (k->kind) {
    case 0: return run(m, k->seq, k->idx, i, k->next);
    case 1:
        if (i == k->start) return cont(m, k->next, i);  // empty iteration: stop repeating
        return rep(m, k->rep, k->cnt + 1, i, k->next);
    default: {
        int g = k->group, old = g >= 0 ? m->cap[2 * g + 1] : 0;
        if (g >= 0) m->cap[2 * g + 1] = i;
        if (cont(m, k->next, i)) return true;
        if (g >= 0) m->cap[2 * g + 1] = old;
        return false;
    }
    }
}

static bool rep(RxM *m, const RxNode *r, int cnt, int i, const RxCont *after) {
    if (r->max >= 0 && cnt >= r->max) return cont(m, after, i);
    RxCont c = {1, NULL, 0, r, cnt, i, 0, after};
    if (cnt < r->min) return run(m, r->body, 0, i, &c);
    if (r->greedy) {
        if (run(m, r->body, 0, i, &c)) return true;
        return cont(m, after, i);
    }
    if (cont(m, after, i)) return true;
    return run(m, r->body, 0, i, &c);
}

static bool is_word(RxM *m, int i) {
    if (i < 0 || i >= m->n) return false;
    char c = m->s[i];
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static i32 fold1(i32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool run(RxM *m, const RxSeq *q, int idx, int i, const RxCont *k) {
    if (++m->steps > 50000000) return false;  // runaway guard
    if (idx == q->n) return cont(m, k, i);
    const RxNode *nd = &q->v[idx];
    switch (nd->kind) {
    case N_CHAR: {
        if (i >= m->n) return false;
        i32 r;
        int w = dec_rune(m->s + i, m->n - i, &r);
        if (r != nd->c && !(nd->fold && fold1(r) == fold1(nd->c))) return false;
        return run(m, q, idx + 1, i + w, k);
    }
    case N_ANY: {
        if (i >= m->n) return false;
        i32 r;
        int w = dec_rune(m->s + i, m->n - i, &r);
        if (r == '\n' && !nd->dotall) return false;
        return run(m, q, idx + 1, i + w, k);
    }
    case N_CLASS: {
        if (i >= m->n) return false;
        i32 r;
        int w = dec_rune(m->s + i, m->n - i, &r);
        bool hit = false;
        for (int j = 0; j < nd->nrg && !hit; j++) hit = r >= nd->rg[j].lo && r <= nd->rg[j].hi;
        if (!hit) return false;
        return run(m, q, idx + 1, i + w, k);
    }
    case N_BOL:
        if (!(i == 0 || (nd->multi && m->s[i - 1] == '\n'))) return false;
        return run(m, q, idx + 1, i, k);
    case N_EOL:
        if (!(i == m->n || (nd->multi && m->s[i] == '\n'))) return false;
        return run(m, q, idx + 1, i, k);
    case N_WORDB:
    case N_NWORDB: {
        bool b = is_word(m, i - 1) != is_word(m, i);
        if (b != (nd->kind == N_WORDB)) return false;
        return run(m, q, idx + 1, i, k);
    }
    case N_GROUP: {
        RxCont after = {0, q, idx + 1, NULL, 0, 0, 0, k};
        RxCont gend = {2, NULL, 0, NULL, 0, 0, nd->group, &after};
        int g = nd->group, os = 0, oe = 0;
        if (g >= 0) {
            os = m->cap[2 * g];
            oe = m->cap[2 * g + 1];
            m->cap[2 * g] = i;
        }
        for (int a = 0; a < nd->nalts; a++)
            if (run(m, &nd->alts[a], 0, i, &gend)) return true;
        if (g >= 0) {
            m->cap[2 * g] = os;
            m->cap[2 * g + 1] = oe;
        }
        return false;
    }
    case N_REP: {
        RxCont after = {0, q, idx + 1, NULL, 0, 0, 0, k};
        return rep(m, nd, 0, i, &after);
    }
    }
    return run(m, q, idx + 1, i, k);
}

// Leftmost match at or after `from`; caps filled (-1 for unset groups).
static bool re_exec(const RxRe *re, GoString s, int from, int *cap) {
    for (int st = from; st <= s.len; st++) {
        RxM m = {s.p, s.len, cap, 0};
        for (int g = 0; g < 2 * (re->ncap + 1); g++) cap[g] = -1;
        if (run(&m, &re->top, 0, st, NULL)) return true;
        if (st < s.len && ((u8)s.p[st] & 0xC0) == 0xC0) {
            while (st + 1 < s.len && ((u8)s.p[st + 1] & 0xC0) == 0x80) st++;
        }
    }
    return false;
}

static int rune_width(GoString s, int i) {
    if (i >= s.len) return 1;
    i32 r;
    return dec_rune(s.p + i, s.len - i, &r);
}

// ---------------------------------------------------------------- API
GoOpaque *regexp_MustCompile(GoString a0) {
    const char *err = NULL;
    RxRe *re = re_compile(a0, &err);
    if (!re) go_panic_str(err);
    return (GoOpaque *)re;
}

Ret_GoOpaque_regexp_RegexpPtrPtr__GoIface regexp_Compile(GoString a0) {
    Ret_GoOpaque_regexp_RegexpPtrPtr__GoIface r = {0};
    const char *err = NULL;
    RxRe *re = re_compile(a0, &err);
    if (!re) r.r1 = go_errorf("error parsing regexp: %s: `%.*s`", err, a0.len, a0.p);
    r.r0 = (GoOpaque *)re;
    return r;
}

bool regexp_Regexp_MatchString(GoOpaque *recv, GoString s) {
    RxRe *re = (RxRe *)recv;
    int cap[2 * 64];
    if (re->ncap >= 63) return false;
    return re_exec(re, s, 0, cap);
}

Ret_bool__GoIface regexp_MatchString(GoString pattern, GoString s) {
    Ret_bool__GoIface r = {0};
    Ret_GoOpaque_regexp_RegexpPtrPtr__GoIface c = regexp_Compile(pattern);
    if (c.r1.type) {
        r.r1 = c.r1;
        return r;
    }
    r.r0 = regexp_Regexp_MatchString(c.r0, s);
    return r;
}

Slice_GoString regexp_Regexp_FindStringSubmatch(GoOpaque *recv, GoString s) {
    RxRe *re = (RxRe *)recv;
    int cap[2 * 64];
    Slice_GoString out = {0};
    if (re->ncap >= 63 || !re_exec(re, s, 0, cap)) return out;
    for (int g = 0; g <= re->ncap; g++) {
        GoString v = {"", 0};
        if (cap[2 * g] >= 0 && cap[2 * g + 1] >= 0) v = (GoString){s.p + cap[2 * g], cap[2 * g + 1] - cap[2 * g]};
        out = go_append_Slice_GoString(out, v);
    }
    return out;
}

Slice_GoString regexp_Regexp_FindAllString(GoOpaque *recv, GoString s, int n) {
    RxRe *re = (RxRe *)recv;
    int cap[2 * 64];
    Slice_GoString out = {0};
    if (n < 0) n = s.len + 1;
    for (int pos = 0, i = 0, prev = -1; i < n && pos <= s.len;) {
        if (!re_exec(re, s, pos, cap)) break;
        bool accept = true;
        if (cap[1] == pos) {
            if (cap[0] == prev) accept = false;
            pos += rune_width(s, pos);
        } else {
            pos = cap[1];
        }
        prev = cap[1];
        if (accept) {
            out = go_append_Slice_GoString(out, (GoString){s.p + cap[0], cap[1] - cap[0]});
            i++;
        }
    }
    return out;
}

static void expand(Slice_u8 *buf, GoString tmpl, GoString src, const int *cap, int ncap) {
    for (int i = 0; i < tmpl.len; i++) {
        char c = tmpl.p[i];
        if (c != '$' || i + 1 >= tmpl.len) {
            *buf = go_append_Slice_u8(*buf, (u8)c);
            continue;
        }
        i++;
        if (tmpl.p[i] == '$') {
            *buf = go_append_Slice_u8(*buf, '$');
            continue;
        }
        int st, en;
        if (tmpl.p[i] == '{') {
            st = i + 1;
            en = st;
            while (en < tmpl.len && tmpl.p[en] != '}') en++;
            if (en >= tmpl.len) {  // malformed: emit literally
                *buf = go_append_Slice_u8(*buf, '$');
                i--;
                continue;
            }
            i = en;
        } else {
            st = i;
            en = st;
            while (en < tmpl.len && ((tmpl.p[en] >= '0' && tmpl.p[en] <= '9') || (tmpl.p[en] >= 'a' && tmpl.p[en] <= 'z') ||
                                     (tmpl.p[en] >= 'A' && tmpl.p[en] <= 'Z') || tmpl.p[en] == '_'))
                en++;
            if (en == st) {
                *buf = go_append_Slice_u8(*buf, '$');
                i--;
                continue;
            }
            i = en - 1;
        }
        int g = 0;
        bool num = en > st;
        for (int k = st; k < en; k++) {
            if (tmpl.p[k] < '0' || tmpl.p[k] > '9') num = false;
            else g = g * 10 + (tmpl.p[k] - '0');
        }
        if (num && g <= ncap && cap[2 * g] >= 0)
            for (int k = cap[2 * g]; k < cap[2 * g + 1]; k++) *buf = go_append_Slice_u8(*buf, (u8)src.p[k]);
    }
}

GoString regexp_Regexp_ReplaceAllString(GoOpaque *recv, GoString src, GoString repl) {
    RxRe *re = (RxRe *)recv;
    int cap[2 * 64];
    Slice_u8 buf = {0};
    int last = 0, pos = 0;
    while (pos <= src.len) {
        if (!re_exec(re, src, pos, cap)) break;
        for (int k = last; k < cap[0]; k++) buf = go_append_Slice_u8(buf, (u8)src.p[k]);
        if (cap[1] > last || cap[0] == 0) expand(&buf, repl, src, cap, re->ncap);
        last = cap[1];
        int w = rune_width(src, pos);
        if (pos + w > cap[1]) pos += w;
        else if (pos + 1 > cap[1]) pos++;
        else pos = cap[1];
    }
    for (int k = last; k < src.len; k++) buf = go_append_Slice_u8(buf, (u8)src.p[k]);
    return go_bytes2str(buf);
}
