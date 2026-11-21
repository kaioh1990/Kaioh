// Android platform layer: the JNI entry points of libmain.so, SDL_main and
// the engine thread (see android_sys.h, ANDROID_PORT.md).
//
// The app (Jesuszilla/ikemen-droid + the CvF patches) is unchanged from the
// Go build: SDLActivity loads libSDL2.so and libmain.so, calls
// nativeCvfSetScreen and nativeOnSDLReady(game folder) on its UI thread, and
// SDL's own SDLThread calls SDL_main. The gesture overlay calls
// nativeCvfInMatch / nativeCvfTouchTap / nativeCvfSetTouchMenus on the UI
// thread. The collector is single-threaded and only the engine thread may
// touch translated code, so those calls only read values the engine
// publishes once per frame, or leave a value in the mailbox for the next
// frame (cvf_host_drain in android_host.c).
#define SDL_MAIN_HANDLED
#include "platform/android/android_sys.h"

#include <SDL.h>
#include <android/log.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "cvf"
#define ENGINE_STACK (16u << 20)  // the main goroutine needs ~1 MB (IOS_PORT.md 3.3)

CvfAndroidMailbox cvf_android_mbox = {.touch_menus = -1};

void cvf_android_http_init(JavaVM *vm, JNIEnv *env);  // android_http.c

// ---------------------------------------------------------------- JNI (UI thread)
static pthread_mutex_t ready_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static char *game_dir;
static _Atomic int screen_w, screen_h;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;
    // runs from SDLActivity's static initializer: the app's class loader
    // is current here (not on threads the engine attaches later)
    cvf_android_http_init(vm, env);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeOnSDLReady(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    const char *p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    pthread_mutex_lock(&ready_mu);
    if (!game_dir && p) game_dir = strdup(p);
    pthread_cond_broadcast(&ready_cv);
    pthread_mutex_unlock(&ready_mu);
    __android_log_print(ANDROID_LOG_INFO, TAG, "game folder: %s", p ? p : "(none)");
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
}

// Physical screen size in pixels (long side first); called before nativeOnSDLReady.
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeCvfSetScreen(JNIEnv *env, jclass cls, jint w, jint h) {
    (void)env, (void)cls;
    atomic_store(&screen_w, w);
    atomic_store(&screen_h, h);
}

// True while a fight is on screen, so touches are left alone (one frame old).
JNIEXPORT jboolean JNICALL Java_org_libsdl_app_SDLActivity_nativeCvfInMatch(JNIEnv *env, jclass cls) {
    (void)env, (void)cls;
    return atomic_load(&cvf_android_mbox.engine_up) && atomic_load(&cvf_android_mbox.in_match) ? JNI_TRUE : JNI_FALSE;
}

// A tap, 0-1 across the game view. False if the current screen does not
// read taps; otherwise the tap reaches it at the next frame.
JNIEXPORT jboolean JNICALL Java_org_libsdl_app_SDLActivity_nativeCvfTouchTap(JNIEnv *env, jclass cls, jfloat x, jfloat y) {
    (void)env, (void)cls;
    if (!atomic_load(&cvf_android_mbox.engine_up) || !atomic_load(&cvf_android_mbox.tap_listening)) return JNI_FALSE;
    atomic_store(&cvf_android_mbox.tap_x, x);
    atomic_store(&cvf_android_mbox.tap_y, y);
    atomic_store_explicit(&cvf_android_mbox.tap_pending, 1, memory_order_release);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_nativeCvfSetTouchMenus(JNIEnv *env, jclass cls, jboolean on) {
    (void)env, (void)cls;
    atomic_store(&cvf_android_mbox.touch_menus, on ? 1 : 0);
}

const char *cvf_android_game_dir(void) { return game_dir; }
void cvf_android_screen(int *w, int *h) {
    *w = atomic_load(&screen_w);
    *h = atomic_load(&screen_h);
}
int cvf_android_chdir(const char *dir) { return chdir(dir); }

// ---------------------------------------------------------------- log
// stdout/stderr -> <game>/save/logs/cvf.log (readable over USB with the rest
// of the game folder) and logcat.
static FILE *log_file;
static int log_pipe[2] = {-1, -1};

static void mkdirs(const char *path) {
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static void *log_pump(void *arg) {
    (void)arg;
    char buf[4096], line[1024];
    size_t ln = 0;
    for (;;) {
        ssize_t n = read(log_pipe[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (log_file) fwrite(buf, 1, (size_t)n, log_file), fflush(log_file);
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || ln == sizeof line - 1) {
                line[ln] = 0;
                __android_log_write(ANDROID_LOG_INFO, TAG, line);
                ln = 0;
                if (buf[i] == '\n') continue;
            }
            line[ln++] = buf[i];
        }
    }
    return NULL;
}

static void start_log(const char *dir) {
    char logs[4096], path[4200];
    snprintf(logs, sizeof logs, "%s/save/logs", dir);
    mkdirs(logs);
    snprintf(path, sizeof path, "%s/cvf.log", logs);
    log_file = fopen(path, "w");  // a fresh log per run
    if (pipe(log_pipe) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(log_pipe[1], 1);
    dup2(log_pipe[1], 2);
    pthread_t t;
    if (pthread_create(&t, NULL, log_pump, NULL) == 0) pthread_detach(t);
}

// save/cvf_env.txt: KEY=VALUE lines (tuning and test knobs; an installed
// app has no other way to get environment variables).
static void read_env_file(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/save/cvf_env.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "cvf: env %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

// ---------------------------------------------------------------- engine thread
// SDLThread is a Java thread with ART's default stack (about 1 MB); the
// main goroutine alone needs that much. The engine runs on a thread of its
// own with a 16 MB stack; SDL_main waits for it. All SDL and GL calls come
// from this one thread (SDL attaches it to the JVM when it first calls Java).
typedef struct EngineArgs {
    int argc;
    char **argv;
} EngineArgs;
static uintptr_t stk_lo, stk_hi, stk_peak_logged;
static unsigned stk_frames;

static void *engine_thread(void *p) {
    EngineArgs *a = (EngineArgs *)p;
    pthread_attr_t at;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        void *lo;
        size_t sz;
        if (pthread_attr_getstack(&at, &lo, &sz) == 0) stk_lo = (uintptr_t)lo, stk_hi = (uintptr_t)lo + sz;
        pthread_attr_destroy(&at);
    }
    cvf_android_engine_main(a->argc, a->argv);
    return NULL;
}

// High-water mark of the engine stack: the lowest word written (fresh
// pages read as zero).
static size_t stack_peak(void) {
    uintptr_t here = (uintptr_t)__builtin_frame_address(0), a = stk_lo;
    while (a + 4096 < here && *(volatile uint64_t *)a == 0) a += 4096;
    if (a > stk_lo + 4096) a -= 4096;
    while (a < here && *(volatile uint64_t *)a == 0) a += 8;
    return (size_t)(stk_hi - a);
}

void cvf_android_frame_tick(void) {
    if (!stk_hi || ++stk_frames % 120) return;
    size_t peak = stack_peak();
    if (peak > stk_peak_logged + 16 * 1024) {
        stk_peak_logged = peak;
        fprintf(stderr, "cvf: engine stack peak %zu KB (of %zu KB)\n", peak / 1024, (size_t)(stk_hi - stk_lo) / 1024);
    }
}

// Called by SDL's SDLThread (SDLActivity.nativeRunMain).
JNIEXPORT int SDL_main(int argc, char *argv[]) {
    (void)argc, (void)argv;
    pthread_mutex_lock(&ready_mu);
    while (!game_dir) pthread_cond_wait(&ready_cv, &ready_mu);  // the app looks for the game first
    pthread_mutex_unlock(&ready_mu);
    start_log(game_dir);
    fprintf(stderr, "cvf: CvF native engine (Android arm64), game folder %s\n", game_dir);
    read_env_file(game_dir);

    int w, h;
    cvf_android_screen(&w, &h);
    char screen[32];
    snprintf(screen, sizeof screen, "%dx%d", w, h);
    // the arguments cvfIOSMain reads, as the iOS app passes them
    char *args[] = {"cvf", "--assets", game_dir, "--screen", screen, NULL};
    EngineArgs ea = {w > 0 && h > 0 ? 5 : 3, args};

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, ENGINE_STACK);
    pthread_t t;
    if (pthread_create(&t, &at, engine_thread, &ea) != 0) {
        fprintf(stderr, "cvf: cannot start the engine thread with a %u MB stack\n", ENGINE_STACK >> 20);
        engine_thread(&ea);
    } else {
        pthread_join(t, NULL);
    }
    pthread_attr_destroy(&at);
    fprintf(stderr, "cvf: engine returned\n");
    fflush(NULL);
    // The engine's globals (collector, Lua state) are set up once per
    // process; a second SDL_main in the same process would find them used.
    exit(0);
}
