// net/http requests on Android through java.net.HttpURLConnection (Android
// has no libcurl). ik_http.c's perform() builds the job and polls job->done
// from its goroutine; each job runs on a worker thread of its own, attached
// to the JVM, that calls the app's CvfHttp.request (Java, added to the app by
// android/native/customize-native.sh) and fills the job's malloc memory. The
// collector never sees these threads or this memory.
//
// Why Java and not curl + mbedTLS built for Android: the system's TLS stack
// and certificate store (including user-installed CAs and their updates),
// proxies and cleartext policy come for free, the APK grows by a few KB
// instead of about 1 MB, and no TLS library has to be kept patched. The
// lobby, the public-IP check and UPnP are a handful of small requests, so
// the cost of a thread and a JNI call per request does not matter.
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/http_job.h"

static JavaVM *g_vm;
static jclass g_cls;      // org.libsdl.app.CvfHttp (global ref)
static jmethodID g_req;   // static byte[] request(String, String, String, byte[], int, int[], String[])

// From JNI_OnLoad (android_sys.c): the app's class loader is only current
// on the thread that loads the library, not on threads attached later.
void cvf_android_http_init(JavaVM *vm, JNIEnv *env) {
    g_vm = vm;
    jclass c = (*env)->FindClass(env, "org/libsdl/app/CvfHttp");
    if (!c) {
        (*env)->ExceptionClear(env);
        return;
    }
    g_req = (*env)->GetStaticMethodID(env, c, "request",
                                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BI[I[Ljava/lang/String;)[B");
    if (!g_req) {
        (*env)->ExceptionClear(env);
        return;
    }
    g_cls = (jclass)(*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);
}

static void finish(HttpJob *j) {
    atomic_thread_fence(memory_order_release);
    j->done = 1;
}

static void perform(JNIEnv *env, HttpJob *j) {
    jstring method = (*env)->NewStringUTF(env, j->method ? j->method : "GET");
    jstring url = (*env)->NewStringUTF(env, j->url ? j->url : "");
    jstring headers = (*env)->NewStringUTF(env, j->headers ? j->headers : "");
    jbyteArray body = NULL;
    if (j->body && j->body_len) {
        body = (*env)->NewByteArray(env, (jsize)j->body_len);
        if (body) (*env)->SetByteArrayRegion(env, body, 0, (jsize)j->body_len, (const jbyte *)j->body);
    }
    jintArray status = (*env)->NewIntArray(env, 1);
    jclass strc = (*env)->FindClass(env, "java/lang/String");  // a system class: found from any thread
    jobjectArray err = strc ? (*env)->NewObjectArray(env, 1, strc, NULL) : NULL;
    if (!method || !url || !headers || !status || !err) {
        (*env)->ExceptionClear(env);
        snprintf(j->err, sizeof j->err, "http: out of memory");
        return;
    }
    jlong timeout = j->timeout_ms > 0 ? j->timeout_ms : 0;
    if (timeout > 0x7fffffff) timeout = 0x7fffffff;
    jbyteArray resp = (jbyteArray)(*env)->CallStaticObjectMethod(env, g_cls, g_req, method, url, headers, body,
                                                                 (jint)timeout, status, err);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        snprintf(j->err, sizeof j->err, "http: request failed (Java exception)");
        return;
    }
    jstring e = (jstring)(*env)->GetObjectArrayElement(env, err, 0);
    if (e) {
        const char *s = (*env)->GetStringUTFChars(env, e, NULL);
        snprintf(j->err, sizeof j->err, "http: %s", s ? s : "request failed");
        if (s) (*env)->ReleaseStringUTFChars(env, e, s);
        return;
    }
    jint st = 0;
    (*env)->GetIntArrayRegion(env, status, 0, 1, &st);
    j->status = st;
    jsize n = resp ? (*env)->GetArrayLength(env, resp) : 0;
    j->resp = (char *)malloc(n ? (size_t)n : 1);
    if (n && j->resp) (*env)->GetByteArrayRegion(env, resp, 0, n, (jbyte *)j->resp);
    j->resp_len = j->resp_cap = (size_t)n;
}

static void *worker(void *arg) {
    HttpJob *j = (HttpJob *)arg;
    JNIEnv *env = NULL;
    JavaVMAttachArgs aa = {JNI_VERSION_1_6, "cvf-http", NULL};
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, &aa) != JNI_OK || !env) {
        snprintf(j->err, sizeof j->err, "http: cannot attach to the JVM");
    } else {
        (*env)->PushLocalFrame(env, 16);
        perform(env, j);
        (*env)->PopLocalFrame(env, NULL);
        (*g_vm)->DetachCurrentThread(g_vm);
    }
    finish(j);
    return NULL;
}

int cvf_android_http_start(HttpJob *j) {
    if (!g_vm || !g_cls) {
        snprintf(j->err, sizeof j->err, "http: not available (the app has no CvfHttp class)");
        finish(j);
        return 1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, worker, j) != 0) return 0;
    pthread_detach(t);
    return 1;
}
