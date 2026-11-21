// net/http client and net/url for the translated game (online lobby, public
// IP lookup, UPnP). Requests run on an OS thread - libcurl, loaded at run
// time, on Linux/macOS; WinHTTP on Windows - while the calling goroutine
// sleeps in 2 ms steps, so the game keeps running (goroutines are
// cooperative). The worker thread touches only malloc memory: the
// collector is not thread-safe.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#elif defined(__vita__)
#include <pthread.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#include "ik/ik_sim.h"

GoIface rt_mem_file(const u8 *p, int n);  // ik_io.c

// ---------------------------------------------------------------- url
static GoString sub(GoString s, int a, int b) { return (GoString){s.p + a, b - a}; }
static int find(GoString s, int from, char c) {
    for (int i = from; i < s.len; i++)
        if (s.p[i] == c) return i;
    return -1;
}

Ret_GoUrlURLPtr__GoIface url_Parse(GoString raw) {
    GoUrlURL *u = go_new(GoUrlURL);
    GoString s = raw;
    int h = find(s, 0, '#');
    if (h >= 0) u->Fragment = sub(s, h + 1, s.len), s = sub(s, 0, h);
    int q = find(s, 0, '?');
    if (q >= 0) u->RawQuery = sub(s, q + 1, s.len), s = sub(s, 0, q);
    int c = find(s, 0, ':');
    if (c > 0) {
        bool ok = true;
        for (int i = 0; i < c; i++) {
            char x = s.p[i];
            if (!((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (i && ((x >= '0' && x <= '9') || x == '+' || x == '-' || x == '.')))) ok = false;
        }
        if (ok) u->Scheme = sub(s, 0, c), s = sub(s, c + 1, s.len);
    }
    if (s.len >= 2 && s.p[0] == '/' && s.p[1] == '/') {
        s = sub(s, 2, s.len);
        int sl = find(s, 0, '/');
        if (sl < 0) sl = s.len;
        u->Host = sub(s, 0, sl);
        s = sub(s, sl, s.len);
    } else if (u->Scheme.len && (s.len == 0 || s.p[0] != '/')) {
        u->Opaque = s;
        s = GOSTR("");
    }
    u->Path = s;
    return (Ret_GoUrlURLPtr__GoIface){u, {0}};
}

GoString url_URL_String(GoUrlURL *u) {
    GoString r = GOSTR("");
    if (u->Scheme.len) r = go_strcat(go_strcat(r, u->Scheme), GOSTR(":"));
    if (u->Opaque.len) {
        r = go_strcat(r, u->Opaque);
    } else {
        if (u->Host.len || u->Scheme.len) r = go_strcat(go_strcat(r, GOSTR("//")), u->Host);
        r = go_strcat(r, u->Path);
    }
    if (u->RawQuery.len) r = go_strcat(go_strcat(r, GOSTR("?")), u->RawQuery);
    if (u->Fragment.len) r = go_strcat(go_strcat(r, GOSTR("#")), u->Fragment);
    return r;
}

GoString url_URL_Hostname(GoUrlURL *u) {
    GoString h = u->Host;
    if (h.len && h.p[0] == '[') {
        int e = find(h, 0, ']');
        return e > 0 ? sub(h, 1, e) : h;
    }
    int c = find(h, 0, ':');
    return c >= 0 ? sub(h, 0, c) : h;
}

// reference resolution (RFC 3986, enough for UPnP's relative control URLs)
Ret_GoUrlURLPtr__GoIface url_URL_Parse(GoUrlURL *base, GoString ref) {
    Ret_GoUrlURLPtr__GoIface r = url_Parse(ref);
    GoUrlURL *u = r.r0;
    if (u->Scheme.len) return r;
    u->Scheme = base->Scheme;
    if (u->Host.len) return r;
    u->Host = base->Host;
    if (u->Path.len == 0) {
        u->Path = base->Path;
        if (!u->RawQuery.len) u->RawQuery = base->RawQuery;
    } else if (u->Path.p[0] != '/') {
        int last = -1;
        for (int i = 0; i < base->Path.len; i++)
            if (base->Path.p[i] == '/') last = i;
        u->Path = go_strcat(last >= 0 ? sub(base->Path, 0, last + 1) : GOSTR("/"), u->Path);
    }
    return r;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
Ret_GoString__GoIface url_PathUnescape(GoString s) {
    char *o = (char *)gc_alloc_atomic((size_t)s.len + 1);
    int n = 0;
    for (int i = 0; i < s.len; i++) {
        if (s.p[i] == '%') {
            if (i + 2 >= s.len || hexval(s.p[i + 1]) < 0 || hexval(s.p[i + 2]) < 0)
                return (Ret_GoString__GoIface){{"", 0}, go_errorf("invalid URL escape")};
            o[n++] = (char)(hexval(s.p[i + 1]) << 4 | hexval(s.p[i + 2]));
            i += 2;
        } else {
            o[n++] = s.p[i];
        }
    }
    return (Ret_GoString__GoIface){{o, n}, {0}};
}

// ---------------------------------------------------------------- headers
static GoString canonical_key(GoString k) {
    char *c = (char *)gc_alloc_atomic((size_t)k.len + 1);
    bool up = true;
    for (int i = 0; i < k.len; i++) {
        char x = k.p[i];
        if (up && x >= 'a' && x <= 'z') x -= 32;
        else if (!up && x >= 'A' && x <= 'Z') x += 32;
        up = x == '-';
        c[i] = x;
    }
    return (GoString){c, k.len};
}
void http_Header_Set(Map_GoString_Slice_GoString h, GoString k, GoString v) {
    if (!h.m) return;  // writing to a nil map panics in Go; requests here always have one
    Slice_GoString vals = {(GoString *)gc_alloc(sizeof(GoString)), 1, 1};
    vals.p[0] = v;
    GoString ck = canonical_key(k);
    *(Slice_GoString *)gomap_put((GoMap *)&h, &ck, sizeof(GoString), sizeof(Slice_GoString), 1) = vals;
}

// ---------------------------------------------------------------- requests
#include "platform/http_job.h"

static char *dupn(const char *p, size_t n) {
    char *c = (char *)malloc(n + 1);
    memcpy(c, p, n);
    c[n] = 0;
    return c;
}
static void job_append(HttpJob *j, const void *p, size_t n) {
    if (j->resp_len + n > j->resp_cap) {
        size_t c = j->resp_cap ? j->resp_cap * 2 : 4096;
        while (c < j->resp_len + n) c *= 2;
        j->resp = (char *)realloc(j->resp, c);
        j->resp_cap = c;
    }
    memcpy(j->resp + j->resp_len, p, n);
    j->resp_len += n;
}

#if CVF_IOS
// iOS has no libcurl: NSURLSession (src/platform/ios/http_nsurl.m)
int cvf_nsurl_start(HttpJob *j);
static bool start_worker(HttpJob *j) { return cvf_nsurl_start(j) != 0; }
#elif defined(__vita__)
// PS Vita: no HTTP client yet (SceHttp or vitasdk's curl package, VITA_PORT.md
// milestone M3): requests fail with an error, as without libcurl elsewhere
static bool start_worker(HttpJob *j) {
    snprintf(j->err, sizeof j->err, "http: not available on the PS Vita yet");
    j->done = 1;
    return true;
}
#elif CVF_ANDROID
// Android has no libcurl: java.net.HttpURLConnection through JNI
// (src/platform/android/android_http.c, the app's CvfHttp class)
int cvf_android_http_start(HttpJob *j);
static bool start_worker(HttpJob *j) { return cvf_android_http_start(j) != 0; }
#elif !defined(_WIN32)
// libcurl, loaded at run time (no build dependency)
typedef void CURL;
struct curl_slist;
static struct {
    bool tried, ok;
    int (*global_init)(long);
    CURL *(*easy_init)(void);
    int (*easy_setopt)(CURL *, int, ...);
    int (*easy_perform)(CURL *);
    int (*easy_getinfo)(CURL *, int, ...);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} curl;
enum {
    OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018, OPT_HTTPHEADER = 10023,
    OPT_CUSTOMREQUEST = 10036, OPT_WRITEFUNCTION = 20011, OPT_FOLLOWLOCATION = 52, OPT_POSTFIELDSIZE = 60,
    OPT_NOSIGNAL = 99, OPT_TIMEOUT_MS = 155, OPT_CONNECTTIMEOUT_MS = 156, INFO_RESPONSE_CODE = 0x200002,
};
static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;
static bool curl_load(void) {
    pthread_mutex_lock(&curl_lock);
    if (!curl.tried) {
        curl.tried = true;
        static const char *const names[] = {
#if defined(__APPLE__)
            "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
            "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so",
#endif
        };
        void *h = NULL;
        for (size_t i = 0; i < sizeof names / sizeof names[0] && !h; i++) h = dlopen(names[i], RTLD_NOW);
        if (h) {
            *(void **)&curl.global_init = dlsym(h, "curl_global_init");
            *(void **)&curl.easy_init = dlsym(h, "curl_easy_init");
            *(void **)&curl.easy_setopt = dlsym(h, "curl_easy_setopt");
            *(void **)&curl.easy_perform = dlsym(h, "curl_easy_perform");
            *(void **)&curl.easy_getinfo = dlsym(h, "curl_easy_getinfo");
            *(void **)&curl.easy_cleanup = dlsym(h, "curl_easy_cleanup");
            *(void **)&curl.easy_strerror = dlsym(h, "curl_easy_strerror");
            *(void **)&curl.slist_append = dlsym(h, "curl_slist_append");
            *(void **)&curl.slist_free_all = dlsym(h, "curl_slist_free_all");
            curl.ok = curl.global_init && curl.easy_init && curl.easy_setopt && curl.easy_perform && curl.easy_getinfo &&
                      curl.easy_cleanup && curl.slist_append && curl.slist_free_all;
            if (curl.ok) curl.global_init(3);  // CURL_GLOBAL_DEFAULT
        }
    }
    pthread_mutex_unlock(&curl_lock);
    return curl.ok;
}
static size_t curl_write(char *p, size_t sz, size_t n, void *ud) {
    job_append((HttpJob *)ud, p, sz * n);
    return sz * n;
}
static void *http_worker(void *arg) {
    HttpJob *j = (HttpJob *)arg;
    if (!curl_load()) {
        snprintf(j->err, sizeof j->err, "http: libcurl not available");
        http_job_finish(j);
        return NULL;
    }
    CURL *c = curl.easy_init();
    struct curl_slist *hl = NULL;
    for (char *line = j->headers; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        hl = curl.slist_append(hl, line);
        line = nl ? nl + 1 : NULL;
    }
    curl.easy_setopt(c, OPT_URL, j->url);
    curl.easy_setopt(c, OPT_NOSIGNAL, 1L);
    curl.easy_setopt(c, OPT_FOLLOWLOCATION, 1L);
    curl.easy_setopt(c, OPT_USERAGENT, "Go-http-client/1.1");
    curl.easy_setopt(c, OPT_WRITEFUNCTION, curl_write);
    curl.easy_setopt(c, OPT_WRITEDATA, j);
    if (j->timeout_ms > 0) curl.easy_setopt(c, OPT_TIMEOUT_MS, j->timeout_ms);
    if (hl) curl.easy_setopt(c, OPT_HTTPHEADER, hl);
    if (strcmp(j->method, "GET")) {
        if (j->body) {
            curl.easy_setopt(c, OPT_POSTFIELDSIZE, (long)j->body_len);
            curl.easy_setopt(c, OPT_POSTFIELDS, j->body);
        }
        curl.easy_setopt(c, OPT_CUSTOMREQUEST, j->method);
    }
    int rc = curl.easy_perform(c);
    if (rc != 0) {
        snprintf(j->err, sizeof j->err, "http: %s", curl.easy_strerror ? curl.easy_strerror(rc) : "request failed");
    } else {
        long st = 0;
        curl.easy_getinfo(c, INFO_RESPONSE_CODE, &st);
        j->status = st;
    }
    if (hl) curl.slist_free_all(hl);
    curl.easy_cleanup(c);
    http_job_finish(j);
    return NULL;
}
static bool start_worker(HttpJob *j) {
    pthread_t t;
    if (pthread_create(&t, NULL, http_worker, j) != 0) return false;
    pthread_detach(t);
    return true;
}
#else
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static DWORD WINAPI http_worker(LPVOID arg) {
    HttpJob *j = (HttpJob *)arg;
    wchar_t *wurl = widen(j->url), *wmethod = widen(j->method), *wheaders = j->headers ? widen(j->headers) : NULL;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096], extra[2048];
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 2048;
    HINTERNET s = NULL, c = NULL, r = NULL;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(j->err, sizeof j->err, "http: invalid URL");
        goto out;
    }
    s = WinHttpOpen(L"Go-http-client/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"Go-http-client/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (j->timeout_ms > 0) WinHttpSetTimeouts(s, (int)j->timeout_ms, (int)j->timeout_ms, (int)j->timeout_ms, (int)j->timeout_ms);
    c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    if (uc.dwExtraInfoLength) wcsncat(path, extra, 2047);  // ?query
    // path plus query (lpszExtraInfo follows lpszUrlPath in wurl)
    r = c ? WinHttpOpenRequest(c, wmethod, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
          : NULL;
    if (!r || !WinHttpSendRequest(r, wheaders ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS, wheaders ? (DWORD)-1L : 0,
                                  j->body ? j->body : WINHTTP_NO_REQUEST_DATA, (DWORD)j->body_len, (DWORD)j->body_len, 0) ||
        !WinHttpReceiveResponse(r, NULL)) {
        snprintf(j->err, sizeof j->err, "http: request failed (error %lu)", (unsigned long)GetLastError());
        goto out;
    }
    DWORD st = 0, sl = sizeof st;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
    j->status = (long)st;
    for (;;) {
        char buf[8192];
        DWORD got = 0;
        if (!WinHttpReadData(r, buf, sizeof buf, &got) || got == 0) break;
        job_append(j, buf, got);
    }
out:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    free(wurl), free(wmethod), free(wheaders);
    http_job_finish(j);
    return 0;
}
static bool start_worker(HttpJob *j) {
    HANDLE t = CreateThread(NULL, 0, http_worker, j, 0, NULL);
    if (!t) return false;
    CloseHandle(t);
    return true;
}
#endif

static Ret_GoHttpResponsePtr__GoIface perform(GoHttpClient *cl, GoString method, GoString url, GoMap *headers,
                                               GoString contentType, const u8 *body, int body_len) {
    HttpJob *j = (HttpJob *)calloc(1, sizeof *j);
    j->method = dupn(method.p, (size_t)method.len);
    j->url = dupn(url.p, (size_t)url.len);
    if (body) j->body = dupn((const char *)body, (size_t)body_len), j->body_len = (size_t)body_len;
    j->timeout_ms = cl && cl->Timeout > 0 ? (long)(cl->Timeout / 1000000) : 0;
    // headers as "K: V" lines
    size_t hcap = 256, hlen = 0;
    char *hs = (char *)malloc(hcap);
    hs[0] = 0;
#define ADDH(k, kl, v, vl)                                      \
    do {                                                        \
        size_t need = hlen + (size_t)(kl) + (size_t)(vl) + 5;   \
        if (need > hcap) hs = (char *)realloc(hs, hcap = need * 2); \
        memcpy(hs + hlen, k, (size_t)(kl)), hlen += (size_t)(kl); \
        memcpy(hs + hlen, ": ", 2), hlen += 2;                   \
        memcpy(hs + hlen, v, (size_t)(vl)), hlen += (size_t)(vl); \
        hs[hlen++] = '\n', hs[hlen] = 0;                         \
    } while (0)
    if (contentType.len) ADDH("Content-Type", 12, contentType.p, contentType.len);
    if (headers && headers->m) {
        GoMapIter it = gomap_iter(headers);
        while (gomap_next(&it)) {
            GoString k = *(GoString *)it.key;
            Slice_GoString v = *(Slice_GoString *)it.val;
            for (int i = 0; i < v.len; i++) ADDH(k.p, k.len, v.p[i].p, v.p[i].len);
        }
    }
#undef ADDH
#if defined(_WIN32)
    // WinHTTP takes "K: V\r\n" lines
    {
        size_t n = 0;
        for (size_t i = 0; i < hlen; i++) n += hs[i] == '\n' ? 2 : 1;
        char *w = (char *)malloc(n + 1);
        size_t o = 0;
        for (size_t i = 0; i < hlen; i++) {
            if (hs[i] == '\n') w[o++] = '\r';
            w[o++] = hs[i];
        }
        w[o] = 0;
        free(hs);
        hs = w;
        hlen = o;
    }
#endif
    j->headers = hlen ? hs : (free(hs), (char *)NULL);
    if (!start_worker(j)) {
        // leaks the job's strings deliberately small; report the failure
        return (Ret_GoHttpResponsePtr__GoIface){NULL, go_errorf("http: cannot start request thread")};
    }
    while (!http_job_done(j)) go_sleep_ns(2000000);  // the game and other goroutines run meanwhile
    Ret_GoHttpResponsePtr__GoIface res = {NULL, {0}};
    if (j->err[0]) {
        res.r1 = go_errorf("%s %.*s: %s", go_strz(method), url.len, url.p, j->err);
    } else {
        GoHttpResponse *r = go_new(GoHttpResponse);
        r->StatusCode = (int)j->status;
        char st[16];
        snprintf(st, sizeof st, "%ld", j->status);
        r->Status = go_cstr_copy(st);
        u8 *b = (u8 *)gc_alloc_atomic(j->resp_len + 1);
        if (j->resp_len) memcpy(b, j->resp, j->resp_len);
        r->Body = rt_mem_file(b, (int)j->resp_len);
        res.r0 = r;
    }
    free(j->method), free(j->url), free(j->headers), free(j->body), free(j->resp), free(j);
    return res;
}

Ret_GoHttpRequestPtr__GoIface http_NewRequest(GoString method, GoString url, GoIface body) {
    GoHttpRequest *r = go_new(GoHttpRequest);
    r->Method = method.len ? method : GOSTR("GET");
    r->url_ = url;
    r->body_ = body;
    r->Header = gomap_make(sizeof(GoString), sizeof(Slice_GoString), 1);
    return (Ret_GoHttpRequestPtr__GoIface){r, {0}};
}
Ret_GoHttpResponsePtr__GoIface http_Client_Do(GoHttpClient *c, GoHttpRequest *req) {
    Slice_u8 body = {0};
    if (req->body_.type) {
        Ret_Slice_u8__GoIface all = io_ReadAll(req->body_);
        if (all.r1.type) return (Ret_GoHttpResponsePtr__GoIface){NULL, all.r1};
        body = all.r0;
    }
    return perform(c, req->Method, req->url_, &req->Header, GOSTR(""), req->body_.type ? body.p : NULL, body.len);
}
Ret_GoHttpResponsePtr__GoIface http_Client_Get(GoHttpClient *c, GoString url) {
    return perform(c, GOSTR("GET"), url, NULL, GOSTR(""), NULL, 0);
}
Ret_GoHttpResponsePtr__GoIface http_Client_Post(GoHttpClient *c, GoString url, GoString contentType, GoIface body) {
    Slice_u8 b = {0};
    if (body.type) {
        Ret_Slice_u8__GoIface all = io_ReadAll(body);
        if (all.r1.type) return (Ret_GoHttpResponsePtr__GoIface){NULL, all.r1};
        b = all.r0;
    }
    return perform(c, GOSTR("POST"), url, NULL, contentType, b.p ? b.p : (const u8 *)"", b.len);
}
