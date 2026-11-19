// os / io / bytes / path/filepath / encoding/binary subset over stdio.
// Archives (zip), PNG and gob are not supported by the headless simulation:
// those calls return errors and Ikemen's own fallbacks take over.
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "ik/ik_sim.h"

static GoString eof_s = {"EOF", 3};
static GoString uneof_s = {"unexpected EOF", 14};
static GoString notexist_s = {"file does not exist", 19};
GoIface io_EOF = {GO_TID_ERRSTR, &eof_s};
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
        RtPathErr *e = (RtPathErr *)malloc(sizeof *e);
        e->notexist = true;
        e->msg = go_error_string(path_error("open", name, os_ErrNotExist));
        r.r1 = (GoIface){GO_TID_ERRSTR, e};
        return r;
    }
    RtFile *rf = (RtFile *)calloc(1, sizeof *rf);
    rf->f = f;
    rf->name = (GoString){strdup(go_strz(name)), name.len};
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
        RtPathErr *e = (RtPathErr *)malloc(sizeof *e);
        e->notexist = true;
        e->msg = go_error_string(path_error("stat", name, os_ErrNotExist));
        r.r1 = (GoIface){GO_TID_ERRSTR, e};
        return r;
    }
    RtFileInfo *fi = (RtFileInfo *)calloc(1, sizeof *fi);
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
    RtBytesReader *r = (RtBytesReader *)calloc(1, sizeof *r);
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
#ifdef ik_iface_Read
    return ik_iface_Read(r, s);
#else
    (void)r, (void)s;
    return (Ret_int__GoIface){0, io_EOF};
#endif
}

Ret_Slice_u8__GoIface io_ReadAll(GoIface r) {
    Ret_Slice_u8__GoIface o = {0};
    Slice_u8 b = {(u8 *)malloc(512), 0, 512};
    for (;;) {
        if (b.len == b.cap) {
            b.cap *= 2;
            b.p = (u8 *)realloc(b.p, (size_t)b.cap);
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
    char *p = (char *)malloc((size_t)n + 1);
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
        if (match_elem(seg.p, seg.len, e->d_name, n)) names = go_append_Slice_GoString(names, go_cstr(strdup(e->d_name)));
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
Ret_GoOpaque_zip_ReadCloserPtrPtr__GoIface zip_OpenReader(GoString a0) {
    Ret_GoOpaque_zip_ReadCloserPtrPtr__GoIface r = {0};
    r.r1 = go_errorf("zip: archives are not supported (%.*s)", a0.len, a0.p);
    return r;
}
GoIface zip_ReadCloser_Close(GoOpaque *recv) {
    (void)recv;
    return (GoIface){0};
}
Ret_GoIface__GoIface zip_File_Open(GoOpaque *recv) {
    (void)recv;
    return (Ret_GoIface__GoIface){{0}, go_error(GOSTR("zip: not supported"))};
}
Ret_GoIface__GoIface png_Decode(GoIface a0) {
    (void)a0;
    return (Ret_GoIface__GoIface){{0}, go_error(GOSTR("png: not supported in the headless simulation"))};
}
GoIface png_Encode(GoIface a0, GoIface a1) {
    (void)a0, (void)a1;
    return go_error(GOSTR("png: not supported"));
}
GoOpaque *gob_NewDecoder(GoIface a0) {
    (void)a0;
    return (GoOpaque *)calloc(1, sizeof(GoOpaque));
}
GoOpaque *gob_NewEncoder(GoIface a0) {
    (void)a0;
    return (GoOpaque *)calloc(1, sizeof(GoOpaque));
}
GoIface gob_Decoder_Decode(GoOpaque *recv, GoIface a0) {
    (void)recv, (void)a0;
    return go_error(GOSTR("gob: not supported"));
}
GoIface gob_Encoder_Encode(GoOpaque *recv, GoIface a0) {
    (void)recv, (void)a0;
    return go_error(GOSTR("gob: not supported"));
}
GoOpaque *image_NewNRGBA(GoOpaque a0) {
    (void)a0;
    return (GoOpaque *)calloc(1, sizeof(GoOpaque));
}
GoOpaque image_Rect(int a0, int a1, int a2, int a3) {
    GoOpaque r = {{0}};
    int v[4] = {a0, a1, a2, a3};
    memcpy(r._opaque, v, sizeof v);
    return r;
}
GoOpaque image_NRGBA_Bounds(GoOpaque *recv) {
    (void)recv;
    return (GoOpaque){{0}};
}
GoOpaque image_Paletted_Bounds(GoOpaque *recv) {
    (void)recv;
    return (GoOpaque){{0}};
}
void draw_Draw(GoIface a0, GoOpaque a1, GoIface a2, GoOpaque a3, int a4) { (void)a0, (void)a1, (void)a2, (void)a3, (void)a4; }

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
