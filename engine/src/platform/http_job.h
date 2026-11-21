// One HTTP request, shared between the engine (src/ik/ik_http.c) and the
// thread or queue that performs it (libcurl/WinHTTP worker in ik_http.c,
// NSURLSession on iOS: src/platform/ios/http_nsurl.m). malloc memory only:
// the collector never sees it. The performer fills the results and sets
// `done` last; the waiting goroutine polls `done`.
#ifndef CVF_HTTP_JOB_H
#define CVF_HTTP_JOB_H
#include <stddef.h>

typedef struct HttpJob {
    char *method, *url, *headers;  // headers: "K: V\n" lines
    char *body;
    size_t body_len;
    long timeout_ms;
    // results (done: written last with release order, read with acquire;
    // see http_job_done / http_job_finish)
    volatile int done;
    long status;
    char *resp;
    size_t resp_len, resp_cap;
    char err[256];
} HttpJob;

// The performer's results must be visible before `done` (arm64 reorders
// plain stores and loads): publish and read it with release/acquire.
static inline void http_job_finish(HttpJob *j) { __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE); }
static inline int http_job_done(HttpJob *j) { return __atomic_load_n(&j->done, __ATOMIC_ACQUIRE); }

#endif

