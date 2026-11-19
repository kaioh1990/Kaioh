#include "core/base.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

void cvf_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void cvf_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

void cvf_arena_init(CvfArena *a, size_t cap) {
    a->base = (u8 *)malloc(cap);
    if (!a->base) cvf_fatal("arena: cannot allocate %zu bytes", cap);
    a->used = 0;
    a->cap = cap;
    a->owns = true;
}

void cvf_arena_free(CvfArena *a) {
    if (a->owns) free(a->base);
    *a = (CvfArena){0};
}

void *cvf_arena_alloc(CvfArena *a, size_t size, size_t align) {
    size_t off = (a->used + (align - 1)) & ~(align - 1);
    if (off + size > a->cap) cvf_fatal("arena full (%zu + %zu > %zu)", off, size, a->cap);
    a->used = off + size;
    return a->base + off;
}

void *cvf_arena_zalloc(CvfArena *a, size_t size, size_t align) {
    void *p = cvf_arena_alloc(a, size, align);
    memset(p, 0, size);
    return p;
}

char *cvf_arena_strdup(CvfArena *a, const char *s, size_t n) {
    char *d = (char *)cvf_arena_alloc(a, n + 1, 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

CvfStr cvf_str_trim(CvfStr s) {
    while (s.n && is_space(s.p[0])) s.p++, s.n--;
    while (s.n && is_space(s.p[s.n - 1])) s.n--;
    return s;
}

bool cvf_str_ieq(CvfStr a, const char *b) {
    size_t n = strlen(b);
    if (n != a.n) return false;
    for (size_t i = 0; i < n; i++)
        if (cvf_lower(a.p[i]) != cvf_lower(b[i])) return false;
    return true;
}

bool cvf_str_ieq2(CvfStr a, CvfStr b) {
    if (a.n != b.n) return false;
    for (size_t i = 0; i < a.n; i++)
        if (cvf_lower(a.p[i]) != cvf_lower(b.p[i])) return false;
    return true;
}

bool cvf_str_eq(CvfStr a, const char *b) {
    size_t n = strlen(b);
    return n == a.n && memcmp(a.p, b, n) == 0;
}

int cvf_str_find(CvfStr s, char c) {
    for (size_t i = 0; i < s.n; i++)
        if (s.p[i] == c) return (int)i;
    return -1;
}

void cvf_str_lower_into(CvfStr s, char *out, size_t cap) {
    size_t n = CVF_MIN(s.n, cap - 1);
    for (size_t i = 0; i < n; i++) out[i] = cvf_lower(s.p[i]);
    out[n] = 0;
}

u8 *cvf_read_file(const char *path, size_t *len) {
    char *rp = cvf_resolve_path(path);
    if (!rp) return NULL;
    FILE *f = fopen(rp, "rb");
    free(rp);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)n + 1);
    if (!buf) cvf_fatal("out of memory reading %s", path);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

char *cvf_resolve_path(const char *path) {
    // Normalise separators.
    size_t n = strlen(path);
    char *p = (char *)malloc(n + 1);
    for (size_t i = 0; i <= n; i++) p[i] = path[i] == '\\' ? '/' : path[i];
    if (file_exists(p)) return p;
    // Walk components, matching each case-insensitively.
    char *out = (char *)malloc(n + 2);
    out[0] = 0;
    size_t on = 0;
    const char *s = p;
    if (*s == '/') { out[on++] = '/'; out[on] = 0; s++; }
    while (*s) {
        const char *e = strchr(s, '/');
        size_t cn = e ? (size_t)(e - s) : strlen(s);
        char comp[512];
        if (cn >= sizeof comp) { free(p); free(out); return NULL; }
        memcpy(comp, s, cn);
        comp[cn] = 0;
        DIR *d = opendir(on ? out : ".");
        bool found = false;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (strcasecmp(de->d_name, comp) == 0) {
                    memcpy(comp, de->d_name, cn);
                    found = true;
                    break;
                }
            }
            closedir(d);
        }
        if (!found && strcmp(comp, ".") && strcmp(comp, "..")) { free(p); free(out); return NULL; }
        memcpy(out + on, comp, cn);
        on += cn;
        if (e) out[on++] = '/';
        out[on] = 0;
        s = e ? e + 1 : s + cn;
    }
    free(p);
    if (!file_exists(out)) { free(out); return NULL; }
    return out;
}

void cvf_path_join_dir(char *out, size_t cap, const char *base, const char *rel) {
    // rel may use backslashes; absolute rel is returned as-is.
    if (rel[0] == '/') { snprintf(out, cap, "%s", rel); return; }
    const char *slash = strrchr(base, '/');
    int dn = slash ? (int)(slash - base + 1) : 0;
    snprintf(out, cap, "%.*s%s", dn, base, rel);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}
