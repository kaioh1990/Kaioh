#include "core/cooked.h"

#include <stdlib.h>

typedef struct Rd {
    const u8 *p, *end;
    bool err;
    CvfArena *a;
} Rd;

static u32 r_u32(Rd *r) {
    if (r->end - r->p < 4) { r->err = true; return 0; }
    u32 v = (u32)r->p[0] | (u32)r->p[1] << 8 | (u32)r->p[2] << 16 | (u32)r->p[3] << 24;
    r->p += 4;
    return v;
}
static i32 r_i32(Rd *r) { return (i32)r_u32(r); }
static u8 r_u8(Rd *r) {
    if (r->p >= r->end) { r->err = true; return 0; }
    return *r->p++;
}
static bool r_bool(Rd *r) { return r_u8(r) != 0; }
static f64 r_f64(Rd *r) {
    u64 lo = r_u32(r), hi = r_u32(r);
    u64 u = lo | hi << 32;
    f64 f;
    memcpy(&f, &u, 8);
    return f;
}
static f32 r_f32(Rd *r) {
    u32 u = r_u32(r);
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}
static const char *r_str(Rd *r) {
    u32 n = r_u32(r);
    if ((u32)(r->end - r->p) < n) { r->err = true; return ""; }
    char *s = cvf_arena_strdup(r->a, (const char *)r->p, n);
    r->p += n;
    return s;
}
static CvfExp r_exp(Rd *r) {
    u32 n = r_u32(r);
    if ((u32)(r->end - r->p) < n) { r->err = true; return (CvfExp){0}; }
    CvfExp e = {n ? r->p : NULL, n};
    r->p += n;
    return e;
}

static void r_params(Rd *r, u32 *np, CvfParam **ps) {
    *np = r_u32(r);
    if (r->err || *np > 100000) { r->err = true; *np = 0; return; }
    *ps = CVF_NEW_N(r->a, CvfParam, *np ? *np : 1);
    for (u32 i = 0; i < *np && !r->err; i++) {
        CvfParam *p = &(*ps)[i];
        p->id = r_u8(r);
        p->nexp = r_u32(r);
        if (p->nexp > 100000) { r->err = true; return; }
        p->exp = CVF_NEW_N(r->a, CvfExp, p->nexp ? p->nexp : 1);
        for (u32 k = 0; k < p->nexp; k++) p->exp[k] = r_exp(r);
    }
}

static void r_block(Rd *r, CvfBlock *b);

static void r_ctrl(Rd *r, CvfCtrl *c) {
    c->kind = r_u8(r);
    c->sctype = -1;
    switch (c->kind) {
    case CVF_CTL_BLOCK:
        c->block = CVF_NEW(r->a, CvfBlock);
        r_block(r, c->block);
        break;
    case CVF_CTL_CALLFUNC: {
        c->fname = r_str(r);
        c->exp = r_exp(r);
        c->nret = r_u32(r);
        if ((u32)(r->end - r->p) < c->nret) { r->err = true; return; }
        c->ret = r->p;
        r->p += c->nret;
        c->nargs = r_i32(r);
        break;
    }
    case CVF_CTL_EXPR: c->exp = r_exp(r); break;
    case CVF_CTL_VARASSIGN:
        c->vari = r_u8(r);
        c->exp = r_exp(r);
        break;
    case CVF_CTL_BREAK:
    case CVF_CTL_CONTINUE:
    case CVF_CTL_NULL: break;
    case CVF_CTL_SCB: {
        const char *name = r_str(r);
        c->sctype = (i16)cvf_sctype_index(name);
        if (c->sctype < 0) c->unknown_type = name;
        r_params(r, &c->nparams, &c->params);
        break;
    }
    default:
        c->unknown_type = r_str(r);
        c->kind = CVF_CTL_UNKNOWN;
        break;
    }
}

static void r_ctrls(Rd *r, u32 *n, CvfCtrl **cs) {
    *n = r_u32(r);
    if (r->err || *n > 1000000) { r->err = true; *n = 0; return; }
    *cs = CVF_NEW_N(r->a, CvfCtrl, *n ? *n : 1);
    for (u32 i = 0; i < *n && !r->err; i++) r_ctrl(r, &(*cs)[i]);
}

static void r_block(Rd *r, CvfBlock *b) {
    b->persistent = r_i32(r);
    b->persistent_index = r_i32(r);
    b->ignorehitpause = r_bool(r);
    b->ctrls_ignorehitpause = r_bool(r);
    b->trigger = r_exp(r);
    b->loop_block = r_bool(r);
    b->nested_in_loop = r_bool(r);
    b->for_loop = r_bool(r);
    b->for_assign = r_bool(r);
    b->for_var = r_u8(r);
    b->for_var_exp = r_exp(r);
    for (int i = 0; i < 3; i++) b->for_exp[i] = r_exp(r);
    b->for_begin = r_i32(r);
    b->for_end = r_i32(r);
    b->for_increment = r_i32(r);
    r_ctrls(r, &b->nctrls, &b->ctrls);
    if (r_bool(r)) {
        b->else_block = CVF_NEW(r->a, CvfBlock);
        r_block(r, b->else_block);
    }
}

static void r_numblock(Rd *r, CvfNumBlock *b) {
    b->tag = r_str(r);
    b->n = r_u32(r);
    if (b->n > 100000) { r->err = true; b->n = 0; return; }
    b->v = CVF_NEW_N(r->a, CvfNamedNum, b->n ? b->n : 1);
    for (u32 i = 0; i < b->n; i++) {
        b->v[i].name = r_str(r);
        b->v[i].val = r_f64(r);
    }
}

bool cvf_cooked_load(CvfCookedChar *c, const char *path) {
    memset(c, 0, sizeof *c);
    size_t len;
    c->file = cvf_read_file(path, &len);
    if (!c->file) return false;
    cvf_arena_init_growable(&c->arena, 1 << 20);
    Rd r = {c->file, c->file + len, false, &c->arena};
    if (len < 8 || memcmp(c->file, "CVFC", 4) != 0) goto bad;
    r.p += 4;
    if (r_u32(&r) != 1) goto bad;
    c->def = r_str(&r);
    c->name = r_str(&r);
    c->displayname = r_str(&r);
    c->author = r_str(&r);
    u32 v = r_u32(&r);
    c->ikemenver[0] = (u16)v;
    c->ikemenver[1] = (u16)(v >> 16);
    c->ikemenver[2] = (u16)r_u32(&r);
    v = r_u32(&r);
    c->mugenver[0] = (u16)v;
    c->mugenver[1] = (u16)(v >> 16);
    c->localcoord[0] = r_i32(&r);
    c->localcoord[1] = r_i32(&r);
    c->rawlocalscl = r_f32(&r);
    c->attack_base = r_i32(&r);
    c->defence_base = r_i32(&r);
    c->npool = r_u32(&r);
    if (c->npool > 1000000) goto bad;
    c->pool = CVF_NEW_N(&c->arena, const char *, c->npool ? c->npool : 1);
    for (u32 i = 0; i < c->npool; i++) c->pool[i] = r_str(&r);
    c->nconst = r_u32(&r);
    if (c->nconst > 1000000) goto bad;
    c->consts = CVF_NEW_N(&c->arena, CvfConstant, c->nconst ? c->nconst : 1);
    for (u32 i = 0; i < c->nconst; i++) {
        c->consts[i].name = r_str(&r);
        c->consts[i].type = r_u8(&r);
        c->consts[i].num = r_f64(&r);
        c->consts[i].str = r_str(&r);
    }
    r_numblock(&r, &c->data);
    r_numblock(&r, &c->velocity);
    r_numblock(&r, &c->movement);
    c->nfuncs = r_u32(&r);
    if (c->nfuncs > 100000) goto bad;
    c->funcs = CVF_NEW_N(&c->arena, CvfFunction, c->nfuncs ? c->nfuncs : 1);
    for (u32 i = 0; i < c->nfuncs && !r.err; i++) {
        CvfFunction *f = &c->funcs[i];
        f->name = r_str(&r);
        f->num_vars = r_i32(&r);
        f->num_args = r_i32(&r);
        f->num_rets = r_i32(&r);
        r_ctrls(&r, &f->nctrls, &f->ctrls);
    }
    c->nstates = r_u32(&r);
    if (c->nstates > 1000000) goto bad;
    c->states = CVF_NEW_N(&c->arena, CvfStateDef, c->nstates ? c->nstates : 1);
    for (u32 i = 0; i < c->nstates && !r.err; i++) {
        CvfStateDef *s = &c->states[i];
        s->no = r_i32(&r);
        s->state_type = r_i32(&r);
        s->state_type_exp = r_exp(&r);
        s->move_type = r_i32(&r);
        s->move_type_exp = r_exp(&r);
        s->physics = r_i32(&r);
        s->physics_exp = r_exp(&r);
        s->player_no = r_i32(&r);
        r_params(&r, &s->nsdparams, &s->sdparams);
        s->nctrlsps = r_u32(&r);
        if (s->nctrlsps > 100000) goto bad;
        s->ctrlsps = CVF_NEW_N(&c->arena, i32, s->nctrlsps ? s->nctrlsps : 1);
        for (u32 k = 0; k < s->nctrlsps; k++) s->ctrlsps[k] = r_i32(&r);
        s->num_vars = r_i32(&r);
        r_block(&r, &s->block);
    }
    if (r.err || r.end - r.p != 4 || memcmp(r.p, "END!", 4) != 0) goto bad;
    return true;
bad:
    cvf_log("cooked: %s is malformed", path);
    cvf_cooked_free(c);
    return false;
}

void cvf_cooked_free(CvfCookedChar *c) {
    free(c->file);
    cvf_arena_free(&c->arena);
    memset(c, 0, sizeof *c);
}

const CvfStateDef *cvf_cooked_state(const CvfCookedChar *c, i32 no) {
    u32 lo = 0, hi = c->nstates;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (c->states[mid].no < no) lo = mid + 1;
        else hi = mid;
    }
    return lo < c->nstates && c->states[lo].no == no ? &c->states[lo] : NULL;
}

bool cvf_cooked_num(const CvfNumBlock *b, const char *name, f64 *out) {
    for (u32 i = 0; i < b->n; i++)
        if (!strcmp(b->v[i].name, name)) { *out = b->v[i].val; return true; }
    return false;
}

int cvf_sctype_index(const char *name) {
    int lo = 0, hi = cvf_sctype_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(cvf_sctype_names[mid], name);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}
