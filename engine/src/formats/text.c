#include "formats/text.h"

#include <stdlib.h>

i32 cvf_atoi(CvfStr str) {
    i64 n = 0;
    str = cvf_str_trim(str);
    if (str.n > 0) {
        CvfStr a = str;
        if (str.p[0] == '-' || str.p[0] == '+') a.p++, a.n--;
        i64 limit = INT32_MAX;
        if (str.p[0] == '-') limit++;
        for (size_t i = 0; i < a.n; i++) {
            if (a.p[i] < '0' || a.p[i] > '9') break;
            n = n * 10 + (i64)(a.p[i] - '0');
            if (n > limit) {
                cvf_log("Atoi conversion outside int32 range: %.*s", (int)(i + 1), a.p);
                return str.p[0] == '-' ? CVF_IERR : CVF_IMAX;
            }
        }
        if (str.p[0] == '-') n *= -1;
    }
    return (i32)n;
}

// Go math.Pow10 tables.
static const f64 pow10tab[32] = {
    1e00, 1e01, 1e02, 1e03, 1e04, 1e05, 1e06, 1e07, 1e08, 1e09,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29,
    1e30, 1e31,
};
static const f64 pow10postab32[] = {
    1e00, 1e32, 1e64, 1e96, 1e128, 1e160, 1e192, 1e224, 1e256, 1e288,
};
static const f64 pow10negtab32[] = {
    1e-00, 1e-32, 1e-64, 1e-96, 1e-128, 1e-160, 1e-192, 1e-224, 1e-256, 1e-288, 1e-320,
};

f64 cvf_pow10(int n) {
    if (0 <= n && n <= 308) return pow10postab32[(unsigned)n / 32] * pow10tab[(unsigned)n % 32];
    if (-323 <= n && n <= 0) return pow10negtab32[(unsigned)(-n) / 32] / pow10tab[(unsigned)(-n) % 32];
    if (n > 0) return 1.0 / 0.0;
    return 0;
}

f64 cvf_atof(CvfStr str) {
    f64 f = 0.0;
    str = cvf_str_trim(str);
    if (str.n > 0) {
        CvfStr a = str;
        if (str.p[0] == '-' || str.p[0] == '+') a.p++, a.n--;
        size_t i = 0;
        int p = 0;
        for (; i < a.n; i++) {
            if (a.p[i] == '.') {
                if (p != 0) break;
                p = (int)i + 1;
                continue;
            }
            if (a.p[i] < '0' || a.p[i] > '9') break;
            f = f * 10 + (f64)(a.p[i] - '0');
        }
        f64 e = 0.0;
        if (i + 1 < a.n && (a.p[i] == 'e' || a.p[i] == 'E')) {
            size_t j = i + 1;
            if (a.p[j] == '-' || a.p[j] == '+') j++;
            for (; j < a.n && a.p[j] >= '0' && a.p[j] <= '9'; j++) e = e * 10 + (f64)(a.p[j] - '0');
            if (e != 0) {
                // Ikemen indexes the untrimmed-sign string here (str, not a).
                if (str.p[i + 1] == '-') e *= -1;
                if (p == 0) p = (int)i;
            }
        }
        if (p > 0) f *= cvf_pow10(p - (int)i + (int)e);
        if (str.p[0] == '-') f *= -1;
    }
    return f;
}

void cvf_lines_split(CvfLines *out, char *text, size_t len, bool normalize) {
    if (len >= 3 && (u8)text[0] == 0xef && (u8)text[1] == 0xbb && (u8)text[2] == 0xbf) {
        memmove(text, text + 3, len - 3);
        len -= 3;
        text[len] = 0;
    }
    if (normalize) {
        size_t w = 0;
        for (size_t r = 0; r < len; r++) {
            if (text[r] == '\r') {
                text[w++] = '\n';
                if (r + 1 < len && text[r + 1] == '\n') r++;
            } else {
                text[w++] = text[r];
            }
        }
        len = w;
        text[len] = 0;
    }
    int n = 1;
    for (size_t i = 0; i < len; i++) n += text[i] == '\n';
    out->text = text;
    out->lines = (CvfStr *)malloc(sizeof(CvfStr) * (size_t)n);
    out->count = 0;
    size_t s = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            out->lines[out->count++] = cvf_strn(text + s, i - s);
            s = i + 1;
        }
    }
}

bool cvf_lines_load(CvfLines *out, const char *path) {
    size_t len;
    u8 *buf = cvf_read_file(path, &len);
    if (!buf) return false;
    // Ikemen's CNS compiler splits on "\n" only; a trailing "\r" stays in
    // the line and is removed by TrimSpace. Same here.
    cvf_lines_split(out, (char *)buf, len, false);
    return true;
}

void cvf_lines_free(CvfLines *l) {
    free(l->text);
    free(l->lines);
    *l = (CvfLines){0};
}

bool cvf_section_name(CvfStr sec, char *name, size_t namecap, CvfStr *sub) {
    name[0] = 0;
    *sub = cvf_strn(sec.p, 0);
    if (sec.n == 0 || sec.p[0] != '[') return false;
    int sc = cvf_str_find(sec, ';');
    if (sc >= 0) sec.n = (size_t)sc;
    sec = cvf_str_trim(sec);
    if (sec.n == 0 || sec.p[sec.n - 1] != ']') return false;
    int rb = cvf_str_find(sec, ']');
    sec = cvf_strn(sec.p + 1, (size_t)rb - 1);
    int i = cvf_str_find(sec, ' ');
    CvfStr nm;
    if (i >= 0) {
        nm = cvf_strn(sec.p, (size_t)i + 1);
        *sub = cvf_strn(sec.p + i + 1, sec.n - (size_t)i - 1);
    } else {
        nm = sec;
        *sub = cvf_strn(sec.p + sec.n, 0);
    }
    cvf_str_lower_into(nm, name, namecap);
    return nm.n > 0;
}

static void ini_put(CvfIni *is, CvfStr key, CvfStr val) {
    char k[128];
    cvf_str_lower_into(key, k, sizeof k);
    for (int j = 0; j < is->count; j++)
        if (strcmp(is->kv[j].key, k) == 0) return;  // first occurrence wins
    if (is->count == is->cap) {
        is->cap = is->cap ? is->cap * 2 : 16;
        is->kv = (CvfIniKV *)realloc(is->kv, sizeof(CvfIniKV) * (size_t)is->cap);
    }
    size_t kn = strlen(k);
    char *kd = (char *)malloc(kn + 1);
    memcpy(kd, k, kn + 1);
    is->kv[is->count++] = (CvfIniKV){kd, val};
}

void cvf_ini_parse(CvfIni *is, const CvfLines *L, int *i) {
    for (; *i < L->count; (*i)++) {
        CvfStr ln = L->lines[*i];
        if (ln.n > 0 && ln.p[0] == '[') break;
        int sc = cvf_str_find(ln, ';');
        if (sc >= 0) ln.n = (size_t)sc;
        ln = cvf_str_trim(ln);
        int ia = -1;
        for (size_t k = 0; k < ln.n; k++)
            if (ln.p[k] == '=' || ln.p[k] == ' ' || ln.p[k] == '\t') { ia = (int)k; break; }
        if (ia > 0) {
            CvfStr key = cvf_strn(ln.p, (size_t)ia);
            CvfStr data = cvf_strn(ln.p, 0);
            int eq = cvf_str_find(ln, '=');
            if (eq >= 0) data = cvf_str_trim(cvf_strn(ln.p + eq + 1, ln.n - (size_t)eq - 1));
            ini_put(is, key, data);
        }
    }
}

bool cvf_ini_read_section(const CvfLines *L, int *i, CvfIni *is, char *name, size_t namecap, CvfStr *sub) {
    name[0] = 0;
    for (; *i < L->count; (*i)++) {
        if (cvf_section_name(L->lines[*i], name, namecap, sub)) {
            (*i)++;
            break;
        }
    }
    if (!name[0]) return false;
    *is = (CvfIni){0};
    cvf_ini_parse(is, L, i);
    return true;
}

void cvf_ini_free(CvfIni *is) {
    for (int j = 0; j < is->count; j++) free(is->kv[j].key);
    free(is->kv);
    *is = (CvfIni){0};
}

bool cvf_ini_get(const CvfIni *is, const char *key, CvfStr *val) {
    for (int j = 0; j < is->count; j++)
        if (strcmp(is->kv[j].key, key) == 0) {
            *val = is->kv[j].val;
            return true;
        }
    return false;
}

typedef void (*csv_fn)(CvfStr s, void *out, int idx);

static bool read_csv(const CvfIni *is, const char *key, void *out, int n, csv_fn fn) {
    CvfStr str;
    if (!cvf_ini_get(is, key, &str) || str.n == 0) return false;
    int idx = 0;
    size_t s = 0;
    for (size_t k = 0; k <= str.n && idx < n; k++) {
        if (k == str.n || str.p[k] == ',') {
            CvfStr part = cvf_str_trim(cvf_strn(str.p + s, k - s));
            if (part.n > 0) fn(part, out, idx);
            idx++;
            s = k + 1;
        }
    }
    return true;
}

static void put_i32(CvfStr s, void *o, int i) { ((i32 *)o)[i] = cvf_atoi(s); }
static void put_f32(CvfStr s, void *o, int i) { ((f32 *)o)[i] = (f32)cvf_atof(s); }
static void put_bool(CvfStr s, void *o, int i) { ((bool *)o)[i] = cvf_atoi(s) != 0; }

bool cvf_ini_read_i32(const CvfIni *is, const char *key, i32 *out, int n) { return read_csv(is, key, out, n, put_i32); }
bool cvf_ini_read_f32(const CvfIni *is, const char *key, f32 *out, int n) { return read_csv(is, key, out, n, put_f32); }
bool cvf_ini_read_bool(const CvfIni *is, const char *key, bool *out, int n) { return read_csv(is, key, out, n, put_bool); }
