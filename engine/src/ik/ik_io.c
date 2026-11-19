// os / io / bytes / path/filepath / encoding/binary subset over stdio.
// Archives (zip), PNG and gob are not supported by the headless simulation:
// those calls return errors and Ikemen's own fallbacks take over.
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

#include "core/detmath.h"
#include "ik/ik_sim.h"

static GoString eof_s = {"EOF", 3};
static GoString uneof_s = {"unexpected EOF", 14};
static GoString notexist_s = {"file does not exist", 19};
GoIface io_EOF = {GO_TID_ERRSTR, &eof_s};
static struct {
    FILE *f;
    GoString name;
    bool notexist;
} rt_stderr = {NULL, {"/dev/stderr", 11}, false}, rt_stdout = {NULL, {"/dev/stdout", 11}, false};
Slice_GoString os_Args;
GoString ini_DEFAULT_SECTION = {"DEFAULT", 7};
GoOpaque *os_Stderr = (GoOpaque *)&rt_stderr;
GoOpaque *os_Stdout = (GoOpaque *)&rt_stdout;
GoIface io_ErrUnexpectedEOF = {GO_TID_ERRSTR, &uneof_s};
GoIface os_ErrNotExist = {GO_TID_ERRSTR, &notexist_s};

static GoIface path_error(const char *op, GoString path, GoIface base) {
    GoString b = go_error_string(base);
    return go_errorf("%s %.*s: %.*s", op, path.len, path.p, b.len, b.p);
}

// ---------------------------------------------------------------- os.File
typedef struct RtFile {
    FILE *f;
    GoString name;
    bool notexist;
} RtFile;

// Errors from os.Open keep "not exist" detectable by os.IsNotExist.
typedef struct RtPathErr {
    GoString msg;  // first: go_error_string works on it
    bool notexist;
} RtPathErr;

static Ret_GoOpaque_os_FilePtrPtr__GoIface open_mode(GoString name, const char *mode) {
    Ret_GoOpaque_os_FilePtrPtr__GoIface r = {0};
    FILE *f = fopen(go_strz(name), mode);
    if (!f) {
        RtPathErr *e = (RtPathErr *)gc_alloc(sizeof *e);
        e->notexist = true;
        e->msg = go_error_string(path_error("open", name, os_ErrNotExist));
        r.r1 = (GoIface){GO_TID_ERRSTR, e};
        return r;
    }
    RtFile *rf = (RtFile *)gc_calloc(1, sizeof *rf);
    rf->f = f;
    rf->name = (GoString){gc_strdup(go_strz(name)), name.len};
    r.r0 = (GoOpaque *)rf;
    return r;
}

Ret_GoOpaque_os_FilePtrPtr__GoIface os_Open(GoString a0) { return open_mode(a0, "rb"); }
Ret_GoOpaque_os_FilePtrPtr__GoIface os_Create(GoString a0) { return open_mode(a0, "wb"); }
Ret_GoOpaque_os_FilePtrPtr__GoIface os_OpenFile(GoString a0, int flags, u32 perm) {
    (void)perm;
    // O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 0x400, O_CREATE 0x40, O_TRUNC 0x200 (Linux values, as Go uses them)
    const char *mode = "rb";
    if (flags & 0x400) mode = "ab";
    else if (flags & 0x200) mode = (flags & 2) ? "w+b" : "wb";
    else if (flags & 2) mode = "r+b";
    else if (flags & 1) mode = (flags & 0x40) ? "wb" : "r+b";
    return open_mode(a0, mode);
}

GoIface os_File_Close(GoOpaque *recv) {
    RtFile *f = (RtFile *)recv;
    if (f && f->f) fclose(f->f), f->f = NULL;
    return (GoIface){0};
}
GoString os_File_Name(GoOpaque *recv) { return ((RtFile *)recv)->name; }

Ret_int__GoIface os_File_Read(GoOpaque *recv, Slice_u8 b) {
    RtFile *f = (RtFile *)recv;
    Ret_int__GoIface r = {0};
    if (b.len == 0) return r;
    size_t n = fread(b.p, 1, (size_t)b.len, f->f);
    r.r0 = (int)n;
    if (n == 0) r.r1 = io_EOF;
    return r;
}

Ret_i64__GoIface os_File_Seek(GoOpaque *recv, i64 off, int whence) {
    RtFile *f = (RtFile *)recv;
    Ret_i64__GoIface r = {0};
    if (fseek(f->f, (long)off, whence)) {
        r.r1 = go_error(GOSTR("seek failed"));
        return r;
    }
    r.r0 = (i64)ftell(f->f);
    return r;
}

GoIface os_WriteFile(GoString name, Slice_u8 data, u32 perm) {
    (void)perm;
    FILE *f = fopen(go_strz(name), "wb");
    if (!f) return path_error("open", name, go_error(GOSTR("cannot create")));
    fwrite(data.p, 1, (size_t)data.len, f);
    fclose(f);
    return (GoIface){0};
}

// ---------------------------------------------------------------- os.Stat
typedef struct RtFileInfo {
    GoString name;
    i64 size;
    bool dir;
} RtFileInfo;

bool rt_fileinfo_IsDir(void *fi) { return ((RtFileInfo *)fi)->dir; }
GoString rt_fileinfo_Name(void *fi) { return ((RtFileInfo *)fi)->name; }
i64 rt_fileinfo_Size(void *fi) { return ((RtFileInfo *)fi)->size; }

Ret_GoIface__GoIface os_Stat(GoString name) {
    Ret_GoIface__GoIface r = {0};
    struct stat st;
    if (stat(go_strz(name), &st) != 0) {
        RtPathErr *e = (RtPathErr *)gc_alloc(sizeof *e);
        e->notexist = true;
        e->msg = go_error_string(path_error("stat", name, os_ErrNotExist));
        r.r1 = (GoIface){GO_TID_ERRSTR, e};
        return r;
    }
    RtFileInfo *fi = (RtFileInfo *)gc_calloc(1, sizeof *fi);
    int k = name.len;
    while (k > 0 && name.p[k - 1] != '/') k--;
    fi->name = (GoString){name.p + k, name.len - k};
    fi->size = (i64)st.st_size;
    fi->dir = S_ISDIR(st.st_mode);
    r.r0 = (GoIface){GO_TID_FILEINFO, fi};
    return r;
}

bool os_IsNotExist(GoIface e) {
    if (e.type != GO_TID_ERRSTR) return false;
    if (e.data == os_ErrNotExist.data) return true;
    GoString s = go_error_string(e);
    return s.len >= notexist_s.len && memcmp(s.p + s.len - notexist_s.len, notexist_s.p, (size_t)notexist_s.len) == 0;
}

// ---------------------------------------------------------------- bytes.Reader
typedef struct RtBytesReader {
    Slice_u8 b;
    i64 pos;
} RtBytesReader;

GoOpaque *bytes_NewReader(Slice_u8 b) {
    RtBytesReader *r = (RtBytesReader *)gc_calloc(1, sizeof *r);
    r->b = b;
    return (GoOpaque *)r;
}
Ret_int__GoIface bytes_Reader_Read(GoOpaque *recv, Slice_u8 p) {
    RtBytesReader *r = (RtBytesReader *)recv;
    Ret_int__GoIface o = {0};
    if (r->pos >= r->b.len) {
        o.r1 = io_EOF;
        return o;
    }
    int n = r->b.len - (int)r->pos;
    if (n > p.len) n = p.len;
    memcpy(p.p, r->b.p + r->pos, (size_t)n);
    r->pos += n;
    o.r0 = n;
    return o;
}
Ret_i64__GoIface bytes_Reader_Seek(GoOpaque *recv, i64 off, int whence) {
    RtBytesReader *r = (RtBytesReader *)recv;
    Ret_i64__GoIface o = {0};
    i64 base = whence == 0 ? 0 : whence == 1 ? r->pos : (i64)r->b.len;
    if (base + off < 0) {
        o.r1 = go_error(GOSTR("bytes.Reader.Seek: negative position"));
        return o;
    }
    r->pos = base + off;
    o.r0 = r->pos;
    return o;
}

// ---------------------------------------------------------------- io
// Reads through the interface's Read method (generated dispatcher).
static Ret_int__GoIface iread(GoIface r, u8 *p, int n) {
    Slice_u8 s = {p, n, n};
#ifdef ik_io_Read
    return ik_io_Read(r, s);
#else
    (void)r, (void)s;
    return (Ret_int__GoIface){0, io_EOF};
#endif
}

Ret_Slice_u8__GoIface io_ReadAll(GoIface r) {
    Ret_Slice_u8__GoIface o = {0};
    Slice_u8 b = {(u8 *)gc_alloc_atomic(512), 0, 512};
    for (;;) {
        if (b.len == b.cap) {
            b.cap *= 2;
            b.p = (u8 *)gc_realloc(b.p, (size_t)b.cap);
        }
        Ret_int__GoIface rr = iread(r, b.p + b.len, b.cap - b.len);
        b.len += rr.r0;
        if (rr.r1.type) {
            if (!go_iface_eq(rr.r1, io_EOF)) o.r1 = rr.r1;
            break;
        }
    }
    o.r0 = b;
    return o;
}

// binary.Read(r, order, data) for `count` values of `size` bytes: io.ReadFull
// semantics (io.EOF when nothing was read, io.ErrUnexpectedEOF when partial).
GoIface go_binread(GoIface r, void *data, size_t size, int count, int big) {
    int total = (int)size * count, got = 0;
    u8 *p = (u8 *)data;
    while (got < total) {
        Ret_int__GoIface rr = iread(r, p + got, total - got);
        got += rr.r0;
        if (got < total && rr.r1.type) return got == 0 ? io_EOF : io_ErrUnexpectedEOF;
        if (rr.r0 == 0 && !rr.r1.type) return io_ErrUnexpectedEOF;
    }
    // host order -> requested order (all current targets are little-endian)
    const u16 one = 1;
    bool host_little = *(const u8 *)&one == 1;
    if (size > 1 && (big != 0) == host_little) {
        for (int i = 0; i < count; i++) {
            u8 *e = p + (size_t)i * size;
            for (size_t a = 0, b2 = size - 1; a < b2; a++, b2--) {
                u8 t = e[a];
                e[a] = e[b2];
                e[b2] = t;
            }
        }
    }
    if (size == 1 && count >= 1) {
        // bool: any non-zero byte is true (Go decodes x != 0)
    }
    return (GoIface){0};
}

// ---------------------------------------------------------------- filepath
GoString filepath_ToSlash(GoString s) { return s; }  // '/' is the separator on every target we build

GoString filepath_Clean(GoString s) {
    if (s.len == 0) return GOSTR(".");
    bool rooted = s.p[0] == '/';
    // split, drop "." and resolve ".."
    GoString parts[256];
    int np = 0;
    for (int i = 0; i < s.len;) {
        while (i < s.len && s.p[i] == '/') i++;
        int st = i;
        while (i < s.len && s.p[i] != '/') i++;
        if (i == st) continue;
        GoString seg = {s.p + st, i - st};
        if (seg.len == 1 && seg.p[0] == '.') continue;
        if (seg.len == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
            if (np > 0 && !(parts[np - 1].len == 2 && parts[np - 1].p[0] == '.' && parts[np - 1].p[1] == '.')) {
                np--;
                continue;
            }
            if (rooted) continue;
        }
        if (np < 256) parts[np++] = seg;
    }
    int n = rooted ? 1 : 0;
    for (int i = 0; i < np; i++) n += parts[i].len + (i ? 1 : 0);
    if (n == 0) return GOSTR(".");
    char *p = (char *)gc_alloc_atomic((size_t)n + 1);
    int w = 0;
    if (rooted) p[w++] = '/';
    for (int i = 0; i < np; i++) {
        if (i) p[w++] = '/';
        memcpy(p + w, parts[i].p, (size_t)parts[i].len);
        w += parts[i].len;
    }
    p[w] = 0;
    return (GoString){p, w};
}

GoString filepath_Join(Slice_GoString a) {
    Slice_GoString nz = {0};
    for (int i = 0; i < a.len; i++)
        if (a.p[i].len) nz = go_append_Slice_GoString(nz, a.p[i]);
    if (!nz.len) return GOSTR("");
    GoString j = strings_Join(nz, GOSTR("/"));
    return filepath_Clean(j);
}
GoString path_Join(Slice_GoString a) { return filepath_Join(a); }

GoString filepath_Dir(GoString s) {
    int k = s.len;
    while (k > 0 && s.p[k - 1] != '/') k--;
    return filepath_Clean((GoString){s.p, k});
}
GoString filepath_Ext(GoString s) {
    for (int i = s.len - 1; i >= 0 && s.p[i] != '/'; i--)
        if (s.p[i] == '.') return (GoString){s.p + i, s.len - i};
    return GOSTR("");
}
bool filepath_IsAbs(GoString s) { return s.len > 0 && s.p[0] == '/'; }

// filepath.Match for one path element: * ? [class] and \ escapes.
static bool match_elem(const char *pat, int pn, const char *s, int sn) {
    int pi = 0, si = 0, star_p = -1, star_s = 0;
    while (si < sn) {
        if (pi < pn && pat[pi] == '*') {
            star_p = pi++;
            star_s = si;
            continue;
        }
        if (pi < pn) {
            bool ok = false;
            int adv = 1;
            if (pat[pi] == '?') ok = true;
            else if (pat[pi] == '[') {
                int k = pi + 1;
                bool neg = k < pn && pat[k] == '^';
                if (neg) k++;
                bool hit = false;
                while (k < pn && pat[k] != ']') {
                    char lo = pat[k], hi = lo;
                    if (lo == '\\' && k + 1 < pn) lo = hi = pat[++k];
                    if (k + 2 < pn && pat[k + 1] == '-' && pat[k + 2] != ']') {
                        hi = pat[k + 2];
                        k += 2;
                    }
                    if (s[si] >= lo && s[si] <= hi) hit = true;
                    k++;
                }
                ok = hit != neg;
                adv = k - pi + 1;
            } else if (pat[pi] == '\\' && pi + 1 < pn) {
                ok = pat[pi + 1] == s[si];
                adv = 2;
            } else {
                ok = pat[pi] == s[si];
            }
            if (ok) {
                pi += adv;
                si++;
                continue;
            }
        }
        if (star_p < 0) return false;
        pi = star_p + 1;
        si = ++star_s;
    }
    while (pi < pn && pat[pi] == '*') pi++;
    return pi == pn;
}

static bool has_meta(GoString s) {
    for (int i = 0; i < s.len; i++)
        if (s.p[i] == '*' || s.p[i] == '?' || s.p[i] == '[' || s.p[i] == '\\') return true;
    return false;
}

static void glob_rec(GoString dir, Slice_GoString segs, int k, Slice_GoString *out) {
    if (k == segs.len) {
        *out = go_append_Slice_GoString(*out, dir);
        return;
    }
    GoString seg = segs.p[k];
    if (!has_meta(seg)) {
        GoString nd = dir.len ? go_strcat(go_strcat(dir, GOSTR("/")), seg) : seg;
        if (dir.len == 1 && dir.p[0] == '/') nd = go_strcat(dir, seg);
        struct stat st;
        if (stat(go_strz(nd), &st) == 0) glob_rec(nd, segs, k + 1, out);
        return;
    }
    DIR *d = opendir(dir.len ? go_strz(dir) : ".");
    if (!d) return;
    Slice_GoString names = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = (int)strlen(e->d_name);
        if (e->d_name[0] == '.' && seg.p[0] != '.') continue;
        if (match_elem(seg.p, seg.len, e->d_name, n)) names = go_append_Slice_GoString(names, go_cstr(gc_strdup(e->d_name)));
    }
    closedir(d);
    sort_Strings(names);  // Go's Glob returns lexically sorted matches
    for (int i = 0; i < names.len; i++) {
        GoString nd = dir.len == 0 ? names.p[i]
                      : (dir.len == 1 && dir.p[0] == '/') ? go_strcat(dir, names.p[i])
                                                          : go_strcat(go_strcat(dir, GOSTR("/")), names.p[i]);
        glob_rec(nd, segs, k + 1, out);
    }
}

Ret_Slice_GoString__GoIface filepath_Glob(GoString pattern) {
    Ret_Slice_GoString__GoIface r = {0};
    Slice_GoString segs = strings_Split(pattern, GOSTR("/"));
    GoString root = GOSTR("");
    int k = 0;
    if (pattern.len && pattern.p[0] == '/') {
        root = GOSTR("/");
        k = 1;
    }
    Slice_GoString rest = {segs.p + k, segs.len - k, segs.len - k};
    Slice_GoString clean = {0};
    for (int i = 0; i < rest.len; i++)
        if (rest.p[i].len) clean = go_append_Slice_GoString(clean, rest.p[i]);
    glob_rec(root, clean, 0, &r.r0);
    return r;
}

// ---------------------------------------------------------------- unsupported
Ret_GoZipReadCloserPtr__GoIface zip_OpenReader(GoString a0) {
    Ret_GoZipReadCloserPtr__GoIface r = {0};
    r.r1 = go_errorf("zip: archives are not supported (%.*s)", a0.len, a0.p);
    return r;
}
GoIface zip_ReadCloser_Close(GoZipReadCloser *recv) {
    (void)recv;
    return (GoIface){0};
}
Ret_GoIface__GoIface zip_File_Open(GoZipFile *recv) {
    (void)recv;
    return (Ret_GoIface__GoIface){{0}, go_error(GOSTR("zip: not supported"))};
}
GoOpaque *gob_NewDecoder(GoIface a0) {
    (void)a0;
    return (GoOpaque *)gc_calloc(1, sizeof(GoOpaque));
}
GoOpaque *gob_NewEncoder(GoIface a0) {
    (void)a0;
    return (GoOpaque *)gc_calloc(1, sizeof(GoOpaque));
}
GoIface gob_Decoder_Decode(GoOpaque *recv, GoIface a0) {
    (void)recv, (void)a0;
    return go_error(GOSTR("gob: not supported"));
}
GoIface gob_Encoder_Encode(GoOpaque *recv, GoIface a0) {
    (void)recv, (void)a0;
    return go_error(GOSTR("gob: not supported"));
}
// net: online play is implemented natively elsewhere (not by translation).
GoIface net_conn_LocalAddr(GoOpaque *recv) {
    (void)recv;
    return (GoIface){0};
}
GoIface net_conn_RemoteAddr(GoOpaque *recv) {
    (void)recv;
    return (GoIface){0};
}
GoString net_IP_String(Slice_u8 recv) {
    (void)recv;
    return GOSTR("");
}
GoString net_JoinHostPort(GoString h, GoString p) { return go_strcat(go_strcat(h, GOSTR(":")), p); }
GoString net_TCPAddr_String(GoOpaque *recv) {
    (void)recv;
    return GOSTR("");
}
Slice_u8 net_IP_To4(Slice_u8 recv) { return recv; }
bool net_IP_Equal(Slice_u8 a, Slice_u8 b) { return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0); }
bool net_IP_IsLoopback(Slice_u8 recv) { return recv.len >= 1 && recv.p[recv.len == 16 ? 12 : 0] == 127; }

// ---------------------------------------------------------------- bufio
// bufio.Writer over an io.Writer that is an *os.File (the state dump).
typedef struct RtBufWriter {
    GoIface w;
} RtBufWriter;

GoOpaque *bufio_NewWriterSize(GoIface w, int size) {
    (void)size;  // stdio buffers already
    RtBufWriter *b = (RtBufWriter *)gc_calloc(1, sizeof *b);
    b->w = w;
    return (GoOpaque *)b;
}

Ret_int__GoIface bufio_Writer_Write(GoOpaque *recv, Slice_u8 p) {
    RtBufWriter *b = (RtBufWriter *)recv;
    Ret_int__GoIface r = {p.len, {0}};
#ifdef GO_TID_P_os_File
    if (b->w.type == GO_TID_P_os_File) {
        RtFile *f = (RtFile *)b->w.data;
        if (f->f) fwrite(p.p, 1, (size_t)p.len, f->f);
    }
#endif
    return r;
}

GoIface bufio_Writer_Flush(GoOpaque *recv) {
    RtBufWriter *b = (RtBufWriter *)recv;
#ifdef GO_TID_P_os_File
    if (b->w.type == GO_TID_P_os_File) {
        RtFile *f = (RtFile *)b->w.data;
        if (f->f) fflush(f->f);
    }
#endif
    return (GoIface){0};
}


int bytes_Reader_Len(GoOpaque *recv) {
    RtBytesReader *r = (RtBytesReader *)recv;
    return r->pos >= r->b.len ? 0 : r->b.len - (int)r->pos;
}

GoString filepath_Base(GoString s) {
    if (s.len == 0) return GOSTR(".");
    while (s.len > 0 && s.p[s.len - 1] == '/') s.len--;
    int k = s.len;
    while (k > 0 && s.p[k - 1] != '/') k--;
    s = (GoString){s.p + k, s.len - k};
    if (s.len == 0) return GOSTR("/");
    return s;
}

Ret_int__GoIface io_ReadFull(GoIface r, Slice_u8 buf) {
    Ret_int__GoIface o = {0};
    while (o.r0 < buf.len) {
        Ret_int__GoIface rr = iread(r, buf.p + o.r0, buf.len - o.r0);
        o.r0 += rr.r0;
        if (rr.r1.type) {
            if (o.r0 < buf.len) o.r1 = o.r0 == 0 ? io_EOF : io_ErrUnexpectedEOF;
            break;
        }
        if (rr.r0 == 0) {
            o.r1 = io_ErrUnexpectedEOF;
            break;
        }
    }
    return o;
}

Ret_int__GoIface fmt_Scanln(Slice_GoIface a0) {
    (void)a0;  // no console input in the headless engine
    return (Ret_int__GoIface){0, io_EOF};
}

_Noreturn void os_Exit(int code) { exit(code); }

GoImageRect image_Rectangle_Bounds(GoImageRect recv) { return recv; }


// bufio.Scanner (lines) over an io.Reader: reads everything up front.
typedef struct RtScanner {
    Slice_u8 data;
    int pos;
    GoString tok;
} RtScanner;

GoOpaque *bufio_NewScanner(GoIface r) {
    RtScanner *s = (RtScanner *)gc_calloc(1, sizeof *s);
    s->data = io_ReadAll(r).r0;
    return (GoOpaque *)s;
}

bool bufio_Scanner_Scan(GoOpaque *recv) {
    RtScanner *s = (RtScanner *)recv;
    if (s->pos >= s->data.len) return false;
    int st = s->pos;
    while (s->pos < s->data.len && s->data.p[s->pos] != '\n') s->pos++;
    int end = s->pos;
    if (s->pos < s->data.len) s->pos++;  // skip '\n'
    if (end > st && s->data.p[end - 1] == '\r') end--;  // ScanLines drops a trailing \r
    s->tok = (GoString){(const char *)s->data.p + st, end - st};
    return true;
}

GoString bufio_Scanner_Text(GoOpaque *recv) { return ((RtScanner *)recv)->tok; }

GoString os_Getenv(GoString key) {
    const char *v = getenv(go_strz(key));
    return v ? go_cstr(gc_strdup(v)) : (GoString){"", 0};
}

// ---------------------------------------------------------------- os.ReadFile
Ret_Slice_u8__GoIface os_ReadFile(GoString name) {
    Ret_Slice_u8__GoIface r = {0};
    Ret_GoOpaque_os_FilePtrPtr__GoIface o = open_mode(name, "rb");
    if (o.r1.type) {
        r.r1 = o.r1;
        return r;
    }
    RtFile *rf = (RtFile *)o.r0;
    fseek(rf->f, 0, SEEK_END);
    long n = ftell(rf->f);
    fseek(rf->f, 0, SEEK_SET);
    if (n < 0) n = 0;
    r.r0.p = (u8 *)gc_alloc_atomic((size_t)n + 1);
    r.r0.len = r.r0.cap = (int)fread(r.r0.p, 1, (size_t)n, rf->f);
    fclose(rf->f);
    gc_free(rf);
    return r;
}

// ---------------------------------------------------------------- inert JSON / matrix stand-ins
// gjson/sjson/encoding/json are only used by the stats and save-data code and
// none affect the simulation. The headless build treats every JSON
// document as empty.


// ---------------------------------------------------------------- bytes.Buffer (value type, zero value usable)
typedef struct RtBuf {
    u8 *p;
    int len, cap, off;  // unread bytes are p[off:len]
} RtBuf;
_Static_assert(sizeof(RtBuf) <= sizeof(GoOpaque), "bytes.Buffer fits");
static void buf_put(RtBuf *b, const void *src, int n) {
    if (b->len + n > b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n) nc *= 2;
        b->p = (u8 *)gc_realloc(b->p, (size_t)nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, src, (size_t)n);
    b->len += n;
}
GoIface bytes_Buffer_WriteByte(GoOpaque *recv, u8 c) {
    buf_put((RtBuf *)recv, &c, 1);
    return (GoIface){0};
}
Ret_int__GoIface bytes_Buffer_WriteString(GoOpaque *recv, GoString s) {
    buf_put((RtBuf *)recv, s.p, s.len);
    return (Ret_int__GoIface){s.len, {0}};
}
Slice_u8 bytes_Buffer_Bytes(GoOpaque *recv) {
    RtBuf *b = (RtBuf *)recv;
    return (Slice_u8){b->p + b->off, b->len - b->off, b->cap - b->off};
}
GoString bytes_Buffer_String(GoOpaque *recv) {
    RtBuf *b = (RtBuf *)recv;
    return go_bytes2str((Slice_u8){b->p + b->off, b->len - b->off, b->len - b->off});
}

// ---------------------------------------------------------------- strings.Reader
typedef struct RtStrReader {
    GoString s;
    i64 i;
} RtStrReader;
GoOpaque *strings_NewReader(GoString s) {
    RtStrReader *r = (RtStrReader *)gc_alloc(sizeof *r);
    r->s = s;
    return (GoOpaque *)r;
}
Ret_int__GoIface strings_Reader_Read(GoOpaque *recv, Slice_u8 b) {
    RtStrReader *r = (RtStrReader *)recv;
    if (r->i >= r->s.len) return (Ret_int__GoIface){0, io_EOF};
    int n = (int)(r->s.len - r->i) < b.len ? (int)(r->s.len - r->i) : b.len;
    memcpy(b.p, r->s.p + r->i, (size_t)n);
    r->i += n;
    return (Ret_int__GoIface){n, {0}};
}
Ret_i64__GoIface strings_Reader_Seek(GoOpaque *recv, i64 off, int whence) {
    RtStrReader *r = (RtStrReader *)recv;
    i64 abs = whence == 0 ? off : whence == 1 ? r->i + off : r->s.len + off;
    if (abs < 0) return (Ret_i64__GoIface){0, go_errorf("strings.Reader.Seek: negative position")};
    r->i = abs;
    return (Ret_i64__GoIface){abs, {0}};
}
int strings_Reader_Len(GoOpaque *recv) {
    RtStrReader *r = (RtStrReader *)recv;
    return r->i >= r->s.len ? 0 : (int)(r->s.len - r->i);
}
GoString strings_Repeat(GoString s, int n) {
    if (n <= 0 || !s.len) return (GoString){"", 0};
    char *p = (char *)gc_alloc_atomic((size_t)s.len * (size_t)n + 1);
    for (int i = 0; i < n; i++) memcpy(p + (size_t)i * s.len, s.p, (size_t)s.len);
    return (GoString){p, s.len * n};
}

// ---------------------------------------------------------------- bufio.Reader (reads its source up front)
typedef struct RtBufReader {
    Slice_u8 data;
    int pos;
} RtBufReader;
GoOpaque *bufio_NewReaderSize(GoIface rd, int size) {
    (void)size;
    RtBufReader *r = (RtBufReader *)gc_alloc(sizeof *r);
    r->data = io_ReadAll(rd).r0;
    return (GoOpaque *)r;
}
Ret_u8__GoIface bufio_Reader_ReadByte(GoOpaque *recv) {
    RtBufReader *r = (RtBufReader *)recv;
    if (r->pos >= r->data.len) return (Ret_u8__GoIface){0, io_EOF};
    return (Ret_u8__GoIface){r->data.p[r->pos++], {0}};
}
GoIface bufio_Reader_UnreadByte(GoOpaque *recv) {
    RtBufReader *r = (RtBufReader *)recv;
    if (r->pos <= 0) return go_errorf("bufio: invalid use of UnreadByte");
    r->pos--;
    return (GoIface){0};
}
Ret_int__GoIface bufio_Reader_Read(GoOpaque *recv, Slice_u8 b) {
    RtBufReader *r = (RtBufReader *)recv;
    if (r->pos >= r->data.len) return (Ret_int__GoIface){0, io_EOF};
    int n = r->data.len - r->pos < b.len ? r->data.len - r->pos : b.len;
    memcpy(b.p, r->data.p + r->pos, (size_t)n);
    r->pos += n;
    return (Ret_int__GoIface){n, {0}};
}

// ---------------------------------------------------------------- misc
f64 math_Mod(f64 x, f64 y) { return fmod(x, y); }  // exact (IEEE), same as Go
f64 math_Pow(f64 x, f64 y) { return cvf_pow(x, y); }  // Go's algorithm (core/detmath)
i32 atomic_AddInt32(i32 *p, i32 d) { return *p += d; }  // single-threaded
GoIface os_Remove(GoString name) {
    return remove(go_strz(name)) == 0 ? (GoIface){0} : go_errorf("remove %.*s: failed", name.len, name.p);
}
// fmt.Stringer implementations of foreign types (only reached through %v)
GoString time_Time_String(GoOpaque t) { (void)t; return GOSTR("0001-01-01 00:00:00 +0000 UTC"); }
GoString time_Location_String(GoOpaque *l) { (void)l; return GOSTR("UTC"); }
GoString time_Duration_String(i64 d) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lldns", (long long)d);
    return go_bytes2str((Slice_u8){(u8 *)tmp, n, n});
}
GoString image_Point_String(GoImagePoint p) {
    char tmp[48];
    int n = snprintf(tmp, sizeof tmp, "(%d,%d)", p.X, p.Y);
    return go_bytes2str((Slice_u8){(u8 *)tmp, n, n});
}
GoString image_Rectangle_String(GoImageRect r) {
    char tmp[96];
    int n = snprintf(tmp, sizeof tmp, "(%d,%d)-(%d,%d)", r.Min.X, r.Min.Y, r.Max.X, r.Max.Y);
    return go_bytes2str((Slice_u8){(u8 *)tmp, n, n});
}
GoString binary_littleEndian_String(GoOpaque o) { (void)o; return GOSTR("LittleEndian"); }

GoOpaque *os_Stdin, *base64_StdEncoding;  // not used by the native engine's I/O
GoIface io_Discard, ioutil_Discard;
GoIface color_AlphaModel, color_YCbCrModel, color_RGBAModel, color_RGBA64Model, color_GrayModel, color_Gray16Model;  // image/color is not translated
static GoString err_closed_pipe = {"io: read/write on closed pipe", 29};
GoIface io_ErrClosedPipe = {GO_TID_ERRSTR, &err_closed_pipe};
GoImagePoint image_ZP;

// ---------------------------------------------------------------- more buffers and readers
int bytes_Buffer_Len(GoOpaque *recv) {
    RtBuf *b = (RtBuf *)recv;
    return b->len - b->off;
}
Ret_int__GoIface bytes_Buffer_Write(GoOpaque *recv, Slice_u8 p) {
    buf_put((RtBuf *)recv, p.p, p.len);
    return (Ret_int__GoIface){p.len, {0}};
}
Ret_int__GoIface bytes_Buffer_Read(GoOpaque *recv, Slice_u8 p) {
    RtBuf *b = (RtBuf *)recv;
    int n = b->len - b->off;
    if (n <= 0) return (Ret_int__GoIface){0, p.len ? io_EOF : (GoIface){0}};
    if (n > p.len) n = p.len;
    memcpy(p.p, b->p + b->off, (size_t)n);
    b->off += n;
    return (Ret_int__GoIface){n, {0}};
}
Ret_u8__GoIface bytes_Buffer_ReadByte(GoOpaque *recv) {
    RtBuf *b = (RtBuf *)recv;
    if (b->off >= b->len) return (Ret_u8__GoIface){0, io_EOF};
    return (Ret_u8__GoIface){b->p[b->off++], {0}};
}
Ret_u8__GoIface strings_Reader_ReadByte(GoOpaque *recv) {
    RtStrReader *r = (RtStrReader *)recv;
    if (r->i >= r->s.len) return (Ret_u8__GoIface){0, io_EOF};
    return (Ret_u8__GoIface){(u8)r->s.p[r->i++], {0}};
}
GoOpaque *bufio_NewReader(GoIface rd) { return bufio_NewReaderSize(rd, 4096); }
Ret_Slice_u8__GoIface bufio_Reader_Peek(GoOpaque *recv, int n) {
    RtBufReader *r = (RtBufReader *)recv;
    int avail = r->data.len - r->pos;
    Ret_Slice_u8__GoIface o = {{r->data.p + r->pos, n < avail ? n : avail, n < avail ? n : avail}, {0}};
    if (avail < n) o.r1 = io_EOF;
    return o;
}
Ret_int__GoIface bufio_Reader_Discard(GoOpaque *recv, int n) {
    RtBufReader *r = (RtBufReader *)recv;
    int avail = r->data.len - r->pos;
    if (n > avail) {
        r->pos = r->data.len;
        return (Ret_int__GoIface){avail, io_EOF};
    }
    r->pos += n;
    return (Ret_int__GoIface){n, {0}};
}
Ret_Slice_u8__bool__GoIface bufio_Reader_ReadLine(GoOpaque *recv) {
    RtBufReader *r = (RtBufReader *)recv;
    Ret_Slice_u8__bool__GoIface o = {0};
    if (r->pos >= r->data.len) {
        o.r2 = io_EOF;
        return o;
    }
    int st = r->pos, e = st;
    while (e < r->data.len && r->data.p[e] != '\n') e++;
    r->pos = e < r->data.len ? e + 1 : e;
    if (e > st && r->data.p[e - 1] == '\r') e--;
    o.r0 = (Slice_u8){r->data.p + st, e - st, e - st};
    return o;
}
GoIface bufio_Scanner_Err(GoOpaque *recv) { (void)recv; return (GoIface){0}; }
GoOpaque *bufio_NewWriter(GoIface w) { return bufio_NewWriterSize(w, 4096); }
GoIface bufio_Writer_WriteByte(GoOpaque *recv, u8 c) { return bufio_Writer_Write(recv, (Slice_u8){&c, 1, 1}).r1; }
Ret_Slice_u8__GoIface ioutil_ReadAll(GoIface r) { return io_ReadAll(r); }

// ---------------------------------------------------------------- files
Ret_int__GoIface os_File_Write(GoOpaque *recv, Slice_u8 b) {
    RtFile *f = (RtFile *)recv;
    size_t n = fwrite(b.p, 1, (size_t)b.len, f->f);
    return (Ret_int__GoIface){(int)n, n == (size_t)b.len ? (GoIface){0} : go_errorf("write %.*s: failed", f->name.len, f->name.p)};
}
GoIface os_File_SetReadDeadline(GoOpaque *recv, GoOpaque t) { (void)recv, (void)t; return (GoIface){0}; }
GoIface os_MkdirAll(GoString path, u32 perm) {
    char *p = (char *)go_strz(path);
    char buf[4096];
    size_t n = strlen(p);
    if (n >= sizeof buf) return go_errorf("mkdir: path too long");
    memcpy(buf, p, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == 0) {
            char c = buf[i];
            buf[i] = 0;
            struct stat st;
            if (stat(buf, &st) != 0 && mkdir(buf, (mode_t)(perm & 0777)) != 0) return go_errorf("mkdir %s: failed", buf);
            buf[i] = c;
        }
    }
    return (GoIface){0};
}
Ret_GoString__GoIface os_Executable(void) { return (Ret_GoString__GoIface){{"", 0}, go_errorf("os.Executable: not supported")}; }

// filepath.Walk / WalkDir: lexical order, directories before their contents.
static GoIface walk(GoString root, GoFunc fn, bool dirEntry) {
    Ret_GoIface__GoIface st = os_Stat(root);
    GoIface (*cb)(void *, GoString, GoIface, GoIface) = (GoIface(*)(void *, GoString, GoIface, GoIface))fn.fn;
    if (st.r1.type) return cb(fn.env, root, (GoIface){0}, st.r1);
    GoIface e = cb(fn.env, root, st.r0, (GoIface){0});
    if (e.type) return e;  // includes filepath.SkipDir: not distinguished
    if (!((RtFileInfo *)st.r0.data)->dir) return (GoIface){0};
    Slice_GoString names = {0};
    DIR *d = opendir(go_strz(root));
    if (!d) return (GoIface){0};
    struct dirent *de;
    while ((de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            names = go_append_Slice_GoString(names, go_cstr(gc_strdup(de->d_name)));
    closedir(d);
    for (int i = 1; i < names.len; i++)  // sort names
        for (int j = i; j > 0 && strcmp(go_strz(names.p[j - 1]), go_strz(names.p[j])) > 0; j--) {
            GoString t = names.p[j];
            names.p[j] = names.p[j - 1];
            names.p[j - 1] = t;
        }
    for (int i = 0; i < names.len; i++) {
        GoString sub = filepath_Join(GO_SLICE_LIT(Slice_GoString, GoString, 2, root, names.p[i]));
        e = walk(sub, fn, dirEntry);
        if (e.type) return e;
    }
    return (GoIface){0};
}
GoIface filepath_Walk(GoString root, GoFunc fn) { return walk(root, fn, false); }
GoIface filepath_WalkDir(GoString root, GoFunc fn) { return walk(root, fn, true); }

// ---------------------------------------------------------------- io copying (generic Read/Write dispatch)
Ret_int__GoIface io_ReadAtLeast(GoIface r, Slice_u8 buf, int min) {
    int n = 0;
    GoIface err = {0};
    while (n < min && !err.type) {
        Ret_int__GoIface rr = iread(r, buf.p + n, buf.len - n);
        n += rr.r0;
        err = rr.r1;
    }
    if (n >= min) err = (GoIface){0};
    else if (n > 0 && go_iface_eq(err, io_EOF)) err = io_ErrUnexpectedEOF;
    return (Ret_int__GoIface){n, err};
}
Ret_i64__GoIface io_CopyN(GoIface dst, GoIface src, i64 limit) {
    u8 tmp[16384];
    i64 total = 0;
    for (;;) {
        int want = (int)sizeof tmp;
        if (limit >= 0 && limit - total < want) want = (int)(limit - total);
        if (want <= 0) break;
        Ret_int__GoIface rr = iread(src, tmp, want);
        if (rr.r0 > 0) {
#ifdef ik_io_Write
            Ret_int__GoIface wr = ik_io_Write(dst, (Slice_u8){tmp, rr.r0, rr.r0});
            if (wr.r1.type) return (Ret_i64__GoIface){total, wr.r1};
#endif
            total += rr.r0;
        }
        if (rr.r1.type) {
            if (go_iface_eq(rr.r1, io_EOF)) {
                if (limit >= 0 && total < limit) return (Ret_i64__GoIface){total, io_EOF};
                break;
            }
            return (Ret_i64__GoIface){total, rr.r1};
        }
    }
    return (Ret_i64__GoIface){total, {0}};
}
Ret_i64__GoIface io_Copy(GoIface dst, GoIface src) { return io_CopyN(dst, src, -1); }
