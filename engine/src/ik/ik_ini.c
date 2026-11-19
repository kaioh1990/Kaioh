// The subset of github.com/go-ini/ini (v1.67) used by Ikemen GO's config
// loader, ported from its parser so values come out identically:
// sections/keys in file order, case folding (Insensitive*), shadows
// (AllowShadows), inline comments, surrounding quotes, continuation lines,
// back-quoted and triple-quoted values. Writing files back (SaveTo) is a
// no-op: the headless engine never rewrites the user's configuration.
#include "ik/ik_sim.h"

typedef struct GiniKey GiniKey;
typedef struct GiniSection GiniSection;
typedef struct GiniFile GiniFile;

struct GiniKey {
    GiniSection *s;
    GoString name, value;
    bool isShadow, isAuto, isBool;
    GiniKey **shadows;
    int nshadows;
};

struct GiniSection {
    GiniFile *f;
    GoString name;
    GiniKey **keys;  // in keyList order
    int nkeys, cap;
    bool raw;
};

struct GiniFile {
    GoIniLoadOptions o;
    GoString kvdelims, childdelim;
    GiniSection **secs;  // sectionList order (unique names unless AllowNonUniqueSections)
    int nsecs, cap;
};

static const GoString DEFAULT_SECTION = {"DEFAULT", 7};

static GoString lower(GoString s) { return strings_ToLower(s); }

static GiniSection *find_section(GiniFile *f, GoString name) {
    for (int i = 0; i < f->nsecs; i++)
        if (go_streq(f->secs[i]->name, name)) return f->secs[i];
    return NULL;
}

static GiniSection *new_section(GiniFile *f, GoString name) {
    if (name.len == 0) return NULL;
    if ((f->o.Insensitive || f->o.InsensitiveSections) && !go_streq(name, DEFAULT_SECTION)) name = lower(name);
    if (!f->o.AllowNonUniqueSections) {
        GiniSection *s = find_section(f, name);
        if (s) return s;
    }
    GiniSection *s = (GiniSection *)calloc(1, sizeof *s);
    s->f = f;
    s->name = name;
    if (f->nsecs == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->secs = (GiniSection **)realloc(f->secs, sizeof(GiniSection *) * (size_t)f->cap);
    }
    f->secs[f->nsecs++] = s;
    return s;
}

static GiniKey *find_key(GiniSection *s, GoString name) {
    for (int i = 0; i < s->nkeys; i++)
        if (go_streq(s->keys[i]->name, name)) return s->keys[i];
    return NULL;
}

static GiniKey *alloc_key(GiniSection *s, GoString name, GoString val) {
    GiniKey *k = (GiniKey *)calloc(1, sizeof *k);
    k->s = s;
    k->name = name;
    k->value = val;
    return k;
}

static GiniKey *new_key(GiniSection *s, GoString name, GoString val) {
    if (name.len == 0) return NULL;
    if (s->f->o.Insensitive || s->f->o.InsensitiveKeys) name = lower(name);
    GiniKey *k = find_key(s, name);
    if (k) {
        if (s->f->o.AllowShadows) {
            // addShadow
            if (k->isShadow || k->isAuto || k->isBool) return k;
            if (!s->f->o.AllowDuplicateShadowValues) {
                if (go_streq(k->value, val)) return k;
                for (int i = 0; i < k->nshadows; i++)
                    if (go_streq(k->shadows[i]->value, val)) return k;
            }
            GiniKey *sh = alloc_key(s, name, val);
            sh->isShadow = true;
            k->shadows = (GiniKey **)realloc(k->shadows, sizeof(GiniKey *) * (size_t)(k->nshadows + 1));
            k->shadows[k->nshadows++] = sh;
        } else {
            k->value = val;
        }
        return k;
    }
    k = alloc_key(s, name, val);
    if (s->nkeys == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->keys = (GiniKey **)realloc(s->keys, sizeof(GiniKey *) * (size_t)s->cap);
    }
    s->keys[s->nkeys++] = k;
    return k;
}

// ---------------------------------------------------------------- parser
typedef struct Rd {
    const char *p;
    int n, pos;
    bool eof;
} Rd;

static GoString read_line(Rd *r) {  // readUntil('\n'), keeps the '\n'
    int st = r->pos;
    while (r->pos < r->n && r->p[r->pos] != '\n') r->pos++;
    if (r->pos < r->n) r->pos++;
    else r->eof = true;
    return (GoString){r->p + st, r->pos - st};
}

static bool is_space_c(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

static GoString trim_left(GoString s) {
    while (s.len && is_space_c(s.p[0])) s.p++, s.len--;
    return s;
}
static GoString trim(GoString s) {
    s = trim_left(s);
    while (s.len && is_space_c(s.p[s.len - 1])) s.len--;
    return s;
}

static int index_any(GoString s, const char *chars, int nchars) {
    for (int i = 0; i < s.len; i++)
        for (int k = 0; k < nchars; k++)
            if (s.p[i] == chars[k]) return i;
    return -1;
}

static int index_str(GoString s, const char *sub, int n) {
    for (int i = 0; i + n <= s.len; i++)
        if (memcmp(s.p + i, sub, (size_t)n) == 0) return i;
    return -1;
}

static int last_index_str(GoString s, const char *sub, int n) {
    for (int i = s.len - n; i >= 0; i--)
        if (memcmp(s.p + i, sub, (size_t)n) == 0) return i;
    return -1;
}

static bool has_surrounded_quote(GoString in, char q) {
    if (in.len < 2 || in.p[0] != q || in.p[in.len - 1] != q) return false;
    for (int i = 1; i < in.len; i++)
        if (in.p[i] == q) return i == in.len - 1;
    return false;
}

static GoString cat(GoString a, GoString b) { return go_strcat(a, b); }

// returns key name, offset of the value; err: 0 ok, 1 delimiter not found,
// 2 empty key name, 3 other
static int read_key_name(GoString delims, GoString line, GoString *name, int *offset) {
    const char *q = NULL;
    int ql = 0;
    if (line.p[0] == '"') {
        if (line.len > 6 && memcmp(line.p, "\"\"\"", 3) == 0) q = "\"\"\"", ql = 3;
        else q = "\"", ql = 1;
    } else if (line.p[0] == '`') {
        q = "`", ql = 1;
    }
    if (ql) {
        GoString rest = {line.p + ql, line.len - ql};
        int pos = index_str(rest, q, ql);
        if (pos < 0) return 3;
        pos += ql;
        GoString after = {line.p + pos + ql, line.len - pos - ql};
        int i = index_any(after, delims.p, delims.len);
        if (i < 0) return 1;
        int endIdx = pos + i;
        *name = trim((GoString){line.p + ql, pos - ql});
        *offset = endIdx + ql + 1;
        return 0;
    }
    int endIdx = index_any(line, delims.p, delims.len);
    if (endIdx < 0) return 1;
    if (endIdx == 0) return 2;
    *name = trim((GoString){line.p, endIdx});
    *offset = endIdx + 1;
    return 0;
}

static GoString read_multilines(Rd *r, GoString val, const char *q, int ql, bool *err) {
    for (;;) {
        GoString next = read_line(r);
        int pos = last_index_str(next, q, ql);
        if (pos > -1) {
            val = cat(val, (GoString){next.p, pos});
            break;
        }
        val = cat(val, next);
        if (r->eof) {
            *err = true;
            return val;
        }
    }
    return val;
}

static GoString read_continuation(Rd *r, GoString val) {
    for (;;) {
        GoString next = trim(read_line(r));
        if (next.len == 0) break;
        val = cat(val, next);
        if (val.p[val.len - 1] != '\\') break;
        val.len--;
    }
    return val;
}

static GoString read_value(GiniFile *f, Rd *r, GoString in, bool *err) {
    GoString line = trim_left(in);
    if (line.len == 0) return (GoString){"", 0};
    const char *q = NULL;
    int ql = 0;
    if (line.len > 3 && memcmp(line.p, "\"\"\"", 3) == 0) q = "\"\"\"", ql = 3;
    else if (line.p[0] == '`') q = "`", ql = 1;
    else if (f->o.UnescapeValueDoubleQuotes && line.p[0] == '"') q = "\"", ql = 1;
    if (ql) {
        GoString rest = {line.p + ql, line.len - ql};
        int pos = last_index_str(rest, q, ql);
        if (pos == -1) return read_multilines(r, rest, q, ql, err);
        GoString v = {line.p + ql, pos};
        if (f->o.UnescapeValueDoubleQuotes && ql == 1 && q[0] == '"') return strings_ReplaceAll(v, GOSTR("\\\""), GOSTR("\""));
        return v;
    }
    line = trim(line);
    if (!f->o.IgnoreContinuation && line.p[line.len - 1] == '\\') return read_continuation(r, (GoString){line.p, line.len - 1});
    if (!f->o.IgnoreInlineComment) {
        int i;
        if (f->o.SpaceBeforeInlineComment) {
            i = index_str(line, " #", 2);
            if (i == -1) i = index_str(line, " ;", 2);
        } else {
            i = index_any(line, "#;", 2);
        }
        if (i > -1) line = trim((GoString){line.p, i});
    }
    if ((has_surrounded_quote(line, '\'') || has_surrounded_quote(line, '"')) && !f->o.PreserveSurroundedQuote) {
        line = (GoString){line.p + 1, line.len - 2};
    } else if (!ql && f->o.UnescapeValueCommentSymbols) {
        line = strings_ReplaceAll(line, GOSTR("\\;"), GOSTR(";"));
        line = strings_ReplaceAll(line, GOSTR("\\#"), GOSTR("#"));
    }
    return line;
}

static GoIface parse(GiniFile *f, GoString data) {
    Rd r = {data.p, data.len, 0, false};
    // BOM
    if (r.n >= 2 && (((u8)r.p[0] == 254 && (u8)r.p[1] == 255) || ((u8)r.p[0] == 255 && (u8)r.p[1] == 254))) r.pos = 2;
    else if (r.n >= 3 && (u8)r.p[0] == 239 && (u8)r.p[1] == 187 && (u8)r.p[2] == 191) r.pos = 3;
    GoString defname = DEFAULT_SECTION;
    if (f->o.Insensitive || f->o.InsensitiveSections) defname = lower(defname);
    GiniSection *sec = new_section(f, defname);
    bool unparseable = false;
    int count = 1;
    while (!r.eof) {
        GoString line = read_line(&r);
        line = trim_left(line);
        if (line.len == 0) continue;
        if (line.p[0] == '#' || line.p[0] == ';') continue;
        if (line.p[0] == '[') {
            int close = -1;
            for (int i = line.len - 1; i >= 0; i--)
                if (line.p[i] == ']') {
                    close = i;
                    break;
                }
            if (close == -1) return go_errorf("unclosed section: %.*s", line.len, line.p);
            GoString name = {line.p + 1, close - 1};
            name = go_cstr(strdup(go_strz(name)));
            sec = new_section(f, name);
            if (!sec) return go_error(GOSTR("empty section name"));
            count = 1;
            unparseable = false;
            for (int i = 0; i < f->o.UnparseableSections.len; i++) {
                GoString u = f->o.UnparseableSections.p[i];
                if (go_streq(u, name) || ((f->o.Insensitive || f->o.InsensitiveSections) && strings_EqualFold(u, name))) unparseable = true;
            }
            continue;
        }
        if (unparseable) {
            sec->raw = true;
            continue;
        }
        GoString kname;
        int offset = 0;
        int e = read_key_name(f->kvdelims, line, &kname, &offset);
        if (e) {
            if (e == 1 && f->o.AllowBooleanKeys) {
                bool err = false;
                GoString kn = read_value(f, &r, line, &err);
                GiniKey *k = new_key(sec, go_cstr(strdup(go_strz(kn))), GOSTR("true"));
                if (k) k->isBool = true;
                continue;
            }
            if ((e == 1 || e == 2) && f->o.SkipUnrecognizableLines) continue;
            return go_errorf("key-value delimiter not found: %.*s", line.len, line.p);
        }
        bool autoincr = false;
        if (kname.len == 1 && kname.p[0] == '-') {
            autoincr = true;
            char t[24];
            snprintf(t, sizeof t, "#%d", count++);
            kname = go_cstr(strdup(t));
        }
        bool err = false;
        GoString value = read_value(f, &r, (GoString){line.p + offset, line.len - offset}, &err);
        if (err) return go_error(GOSTR("missing closing key quote"));
        // own the strings (the source buffer belongs to the caller)
        kname = go_cstr(strdup(go_strz(kname)));
        value = (GoString){(const char *)memcpy(malloc((size_t)value.len + 1), value.p, (size_t)value.len), value.len};
        GiniKey *k = new_key(sec, kname, value);
        if (k) k->isAuto = autoincr;
    }
    return (GoIface){0};
}

// ---------------------------------------------------------------- API
Ret_GoOpaque_ini_FilePtrPtr__GoIface ini_LoadSources(GoIniLoadOptions opts, GoIface source, Slice_GoIface others) {
    Ret_GoOpaque_ini_FilePtrPtr__GoIface r = {0};
    GiniFile *f = (GiniFile *)calloc(1, sizeof *f);
    f->o = opts;
    f->kvdelims = opts.KeyValueDelimiters.len ? opts.KeyValueDelimiters : GOSTR("=:");
    f->childdelim = opts.ChildSectionDelimiter.len ? opts.ChildSectionDelimiter : GOSTR(".");
    for (int i = -1; i < others.len; i++) {
        GoIface src = i < 0 ? source : others.p[i];
        GoString data;
        if (src.type == GO_TID_string) {
            // a file name
            Ret_GoOpaque_os_FilePtrPtr__GoIface o = os_Open(*(GoString *)src.data);
            if (o.r1.type) {
                if (opts.Loose) continue;
                r.r1 = o.r1;
                return r;
            }
            Ret_Slice_u8__GoIface all = io_ReadAll((GoIface){GO_TID_P_os_File, o.r0});
            os_File_Close(o.r0);
            data = (GoString){(const char *)all.r0.p, all.r0.len};
        }
#ifdef GO_TID_S_uint8
        else if (src.type == GO_TID_S_uint8) {
            Slice_u8 b = *(Slice_u8 *)src.data;
            data = (GoString){(const char *)b.p, b.len};
        }
#endif
        else {
            r.r1 = go_error(GOSTR("ini: unsupported data source"));
            return r;
        }
        GoIface e = parse(f, data);
        if (e.type) {
            r.r1 = e;
            return r;
        }
        if (opts.ShortCircuit) break;
    }
    r.r0 = (GoOpaque *)f;
    return r;
}

Slice_GoOpaque_ini_SectionPtrPtr ini_File_Sections(GoOpaque *recv) {
    GiniFile *f = (GiniFile *)recv;
    Slice_GoOpaque_ini_SectionPtrPtr s = {(GoOpaque **)malloc(sizeof(void *) * (size_t)(f->nsecs ? f->nsecs : 1)), f->nsecs, f->nsecs};
    for (int i = 0; i < f->nsecs; i++) s.p[i] = (GoOpaque *)f->secs[i];
    return s;
}

Ret_GoOpaque_ini_SectionPtrPtr__GoIface ini_File_GetSection(GoOpaque *recv, GoString name) {
    GiniFile *f = (GiniFile *)recv;
    Ret_GoOpaque_ini_SectionPtrPtr__GoIface r = {0};
    if (name.len == 0) name = DEFAULT_SECTION;
    if (f->o.Insensitive || f->o.InsensitiveSections) name = lower(name);
    GiniSection *s = find_section(f, name);
    if (!s) r.r1 = go_errorf("section %.*s does not exist", name.len, name.p);
    r.r0 = (GoOpaque *)s;
    return r;
}

GoOpaque *ini_File_Section(GoOpaque *recv, GoString name) {
    Ret_GoOpaque_ini_SectionPtrPtr__GoIface r = ini_File_GetSection(recv, name);
    if (r.r1.type) return (GoOpaque *)new_section((GiniFile *)recv, name.len ? name : DEFAULT_SECTION);
    return r.r0;
}

Ret_GoOpaque_ini_SectionPtrPtr__GoIface ini_File_NewSection(GoOpaque *recv, GoString name) {
    Ret_GoOpaque_ini_SectionPtrPtr__GoIface r = {0};
    GiniSection *s = new_section((GiniFile *)recv, name);
    if (!s) r.r1 = go_error(GOSTR("empty section name"));
    r.r0 = (GoOpaque *)s;
    return r;
}

GoIface ini_File_SaveTo(GoOpaque *recv, GoString path) {
    (void)recv, (void)path;
    return (GoIface){0};
}

GoString ini_Section_Name(GoOpaque *recv) { return ((GiniSection *)recv)->name; }

Slice_GoOpaque_ini_KeyPtrPtr ini_Section_Keys(GoOpaque *recv) {
    GiniSection *s = (GiniSection *)recv;
    Slice_GoOpaque_ini_KeyPtrPtr out = {(GoOpaque **)malloc(sizeof(void *) * (size_t)(s->nkeys ? s->nkeys : 1)), s->nkeys, s->nkeys};
    for (int i = 0; i < s->nkeys; i++) out.p[i] = (GoOpaque *)s->keys[i];
    return out;
}

Ret_GoOpaque_ini_KeyPtrPtr__GoIface ini_Section_GetKey(GoOpaque *recv, GoString name) {
    GiniSection *s = (GiniSection *)recv;
    Ret_GoOpaque_ini_KeyPtrPtr__GoIface r = {0};
    if (s->f->o.Insensitive || s->f->o.InsensitiveKeys) name = lower(name);
    GiniKey *k = find_key(s, name);
    if (!k) {
        // parent sections through the child delimiter ("a.b" -> "a")
        GoString sname = s->name;
        GoString d = s->f->childdelim;
        for (;;) {
            int i = strings_LastIndex(sname, d);
            if (i < 0) break;
            sname.len = i;
            GiniSection *p = find_section(s->f, sname);
            if (!p) continue;
            return ini_Section_GetKey((GoOpaque *)p, name);
        }
        r.r1 = go_errorf("error when getting key of section %.*s: key %.*s not exists", s->name.len, s->name.p, name.len, name.p);
        return r;
    }
    r.r0 = (GoOpaque *)k;
    return r;
}

Ret_GoOpaque_ini_KeyPtrPtr__GoIface ini_Section_NewKey(GoOpaque *recv, GoString name, GoString val) {
    Ret_GoOpaque_ini_KeyPtrPtr__GoIface r = {0};
    GiniKey *k = new_key((GiniSection *)recv, name, val);
    if (!k) r.r1 = go_error(GOSTR("error creating new key: empty key name"));
    r.r0 = (GoOpaque *)k;
    return r;
}

void ini_Section_DeleteKey(GoOpaque *recv, GoString name) {
    GiniSection *s = (GiniSection *)recv;
    for (int i = 0; i < s->nkeys; i++)
        if (go_streq(s->keys[i]->name, name)) {
            memmove(s->keys + i, s->keys + i + 1, sizeof(GiniKey *) * (size_t)(s->nkeys - i - 1));
            s->nkeys--;
            return;
        }
}

GoString ini_Key_Name(GoOpaque *recv) { return ((GiniKey *)recv)->name; }
GoString ini_Key_Value(GoOpaque *recv) { return ((GiniKey *)recv)->value; }
GoString ini_Key_String(GoOpaque *recv) { return ((GiniKey *)recv)->value; }
void ini_Key_SetValue(GoOpaque *recv, GoString v) { ((GiniKey *)recv)->value = v; }

Slice_GoString ini_Key_ValueWithShadows(GoOpaque *recv) {
    GiniKey *k = (GiniKey *)recv;
    Slice_GoString out = {0};
    if (k->value.len) out = go_append_Slice_GoString(out, k->value);
    for (int i = 0; i < k->nshadows; i++)
        if (k->shadows[i]->value.len) out = go_append_Slice_GoString(out, k->shadows[i]->value);
    if (!out.p) out = (Slice_GoString){(GoString *)malloc(sizeof(GoString)), 0, 1};  // []string{} is not nil
    return out;
}

GoOpaque *ini_Section_Key(GoOpaque *recv, GoString name) {
    Ret_GoOpaque_ini_KeyPtrPtr__GoIface r = ini_Section_GetKey(recv, name);
    if (r.r1.type) return (GoOpaque *)new_key((GiniSection *)recv, name, GOSTR(""));
    return r.r0;
}
