// net/http requests on iOS through NSURLSession (iOS has no libcurl).
// ik_http.c's perform() builds the job and polls job->done from its
// goroutine; this file runs the request on NSURLSession's delegate queue and
// touches only the job's malloc memory (the collector is single-threaded).
//
// App Transport Security applies to NSURLSession: the lobby is HTTPS; plain
// HTTP to the router (UPnP) and to local test servers needs
// NSAllowsLocalNetworking in the app's Info.plist.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/http_job.h"

static NSURLSession *session(void) {
    static NSURLSession *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
      c.HTTPAdditionalHeaders = @{@"User-Agent" : @"Go-http-client/1.1"};
      c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
      c.waitsForConnectivity = NO;
      s = [NSURLSession sessionWithConfiguration:c];
    });
    return s;
}

static void finish(HttpJob *j) { http_job_finish(j); }

int cvf_nsurl_start(HttpJob *j) {
    @autoreleasepool {
        NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:j->url ? j->url : ""]];
        if (!url) {
            snprintf(j->err, sizeof j->err, "http: invalid URL");
            finish(j);
            return 1;
        }
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
        req.HTTPMethod = [NSString stringWithUTF8String:j->method ? j->method : "GET"];
        // Go's http.Client.Timeout covers the whole exchange; 0 means none
        req.timeoutInterval = j->timeout_ms > 0 ? j->timeout_ms / 1000.0 : 3600;
        for (char *line = j->headers; line && *line;) {
            char *nl = strchr(line, '\n');
            size_t n = nl ? (size_t)(nl - line) : strlen(line);
            NSString *h = [[NSString alloc] initWithBytes:line length:n encoding:NSUTF8StringEncoding];
            NSRange colon = [h rangeOfString:@":"];
            if (colon.location != NSNotFound) {
                NSString *k = [h substringToIndex:colon.location];
                NSString *v = [[h substringFromIndex:colon.location + 1]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                [req addValue:v forHTTPHeaderField:k];
            }
            line = nl ? nl + 1 : NULL;
        }
        if (j->body && j->body_len) req.HTTPBody = [NSData dataWithBytes:j->body length:j->body_len];
        NSURLSessionDataTask *task = [session()
            dataTaskWithRequest:req
              completionHandler:^(NSData *data, NSURLResponse *resp, NSError *error) {
                if (error) {
                    snprintf(j->err, sizeof j->err, "http: %s", error.localizedDescription.UTF8String ?: "request failed");
                } else {
                    j->status = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
                    size_t n = data.length;
                    j->resp = (char *)malloc(n ? n : 1);
                    if (n) memcpy(j->resp, data.bytes, n);
                    j->resp_len = j->resp_cap = n;
                }
                finish(j);
              }];
        [task resume];
    }
    return 1;
}
