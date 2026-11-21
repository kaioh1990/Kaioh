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
    // results
    volatile int done;
    long status;
    char *resp;
    size_t resp_len, resp_cap;
    char err[256];
} HttpJob;

#endif

