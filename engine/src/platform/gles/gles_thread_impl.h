// Implementation of gles_thread.h, included once by gles_bind.c (it needs the
// binding's p_gl* entry points) when CVF_GL_THREAD is defined.
//
// How it works. gles_thread_start() starts the render thread, which runs the
// host's init (the GL context lives on that thread from then on).
// gles2_Init loads the entry points as usual and then gt_install() moves each
// p_glX into r_glX and points p_glX at a recorder q_glX, so the wrappers
// above are unchanged: every GL call of the engine thread appends a command
// (thunk + 64-bit argument slots + copied data) to the current buffer.
//
//   - Plain calls (state, binds, uniforms by value, draws from buffers) are
//     recorded with their arguments.
//   - Calls that pass data (buffer/texture uploads, uniform arrays, id
//     lists) copy it into the command, so the engine may reuse its memory at
//     once. Uploads larger than GT_COPY_MAX are executed synchronously
//     instead (no copy): the engine thread waits, so its memory stays valid.
//   - Client-side vertex arrays (the Vita sprite path): glVertexAttribPointer
//     with no array buffer bound only updates a shadow of the bound vertex
//     array object; glDrawArrays copies the vertices it will read (first ..
//     first+count of every enabled client attribute, overlapping ranges
//     merged) and the command re-points the attributes at the copy before
//     drawing.
//   - Calls that return something (glGen*, glGet*, glCreate*, glMapBufferRange,
//     glReadPixels, glFinish, shader sources) are synchronous: the current
//     buffer is submitted with a "call" command and the engine thread waits
//     until the render thread has run it. They happen while loading, not in
//     a fight. glGetError is not synchronous: it returns 0 (the renderer only
//     logs its result; CVF_GL_CHECK is not available with the thread).
//
// Frames. There are two buffers. The engine records into one; at the swap
// (gles_thread_swap) it waits until the render thread has finished the other
// (the previous frame: back-pressure, at most one frame in flight), hands
// this one over and continues with the next tick in the free buffer. A
// synchronous call is a hand-over followed by a wait. Semaphores (SDL)
// order all memory accesses between the two threads.
//
// Determinism: the simulation never reads anything the render thread writes
// (GL results reach the engine only through synchronous calls, which return
// exactly what a direct call returns, in the same order), so the game's
// state does not depend on the thread or its timing.
#include <string.h>
#include "core/sched.h"  // go_mono_ns
#include "platform/gles/gles_thread.h"

#define GT_COPY_MAX (1u << 20)     // larger uploads are synchronous
#define GT_SUBMIT_AT (8u << 20)    // hand over early when a buffer grows past this
#define GT_MAX_VAO 64
#define GT_MAX_ATTR 16

typedef void (*GtThunk)(const uint64_t *a, const uint8_t *d);
typedef struct {
    GtThunk fn;
    uint32_t nargs, ndata;
} __attribute__((aligned(8))) GtHdr;
typedef struct {
    uint8_t *p;
    size_t len, cap;
} GtBuf;
typedef struct {
    uint8_t enabled, client, norm;
    i32 size, stride;
    u32 type;
    const uint8_t *ptr;  // client pointer (or the buffer offset when !client)
} GtAttr;

static struct {
    int on;
    SDL_Thread *th;
    SDL_sem *work, *done, *inited;
    GtBuf buf[2];
    int cur;           // buffer being recorded
    int busy;          // a handed-over buffer is not finished yet
    GtBuf *sub;        // the handed-over buffer (render thread)
    int quit, init_ret;
    GlesThreadHost host;
    SDL_atomic_t render_us;
    int64_t t_frame;   // render thread: execution time of the current buffer
    // shadow state (engine thread)
    u32 vao, array_buffer;
    i32 unpack_row_length, unpack_alignment;
    GtAttr attr[GT_MAX_VAO][GT_MAX_ATTR];
    int vao_warned;
} gt;

int gles_thread_active(void) { return gt.on; }

static inline uint64_t f2w(f32 f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static inline f32 w2f(uint64_t w) {
    uint32_t u = (uint32_t)w;
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}

static void gt_submit(void);

// Reserves a command; returns its argument slots (data follows them).
static uint64_t *rec(GtThunk fn, uint32_t nargs, uint32_t ndata) {
    GtBuf *b = &gt.buf[gt.cur];
    size_t need = sizeof(GtHdr) + 8u * nargs + ((ndata + 7u) & ~7u);
    if (b->len + need > b->cap) {
        size_t c = b->cap ? b->cap * 2 : (256u << 10);
        while (c < b->len + need) c *= 2;
        uint8_t *p = (uint8_t *)realloc(b->p, c);
        if (!p) {
            fprintf(stderr, "gles thread: out of memory for %zu bytes of commands\n", c);
            abort();
        }
        b->p = p, b->cap = c;
    }
    GtHdr *h = (GtHdr *)(b->p + b->len);
    h->fn = fn, h->nargs = nargs, h->ndata = ndata;
    b->len += need;
    return (uint64_t *)(h + 1);
}
static inline uint8_t *rec_data(uint64_t *a, uint32_t nargs) { return (uint8_t *)(a + nargs); }

static void gt_replay(GtBuf *b) {
    size_t o = 0;
    while (o < b->len) {
        GtHdr *h = (GtHdr *)(b->p + o);
        const uint64_t *a = (const uint64_t *)(h + 1);
        h->fn(a, (const uint8_t *)(a + h->nargs));
        o += sizeof(GtHdr) + 8u * h->nargs + ((h->ndata + 7u) & ~7u);
    }
}

static int SDLCALL gt_main(void *ud) {
    (void)ud;
    gt.init_ret = gt.host.init ? gt.host.init(gt.host.ud) : 0;
    SDL_SemPost(gt.inited);
    if (gt.init_ret != 0) return 0;
    for (;;) {
        SDL_SemWait(gt.work);
        if (gt.quit) break;
        int64_t t0 = go_mono_ns();
        gt.t_frame = 0;
        gt_replay(gt.sub);
        // the swap command subtracts its own time from t_frame
        gt.t_frame += go_mono_ns() - t0;
        SDL_AtomicAdd(&gt.render_us, (int)(gt.t_frame / 1000));
        SDL_SemPost(gt.done);
    }
    return 0;
}

// Engine thread: hand the current buffer over (after the previous one is done).
static void gt_wait_idle(void) {
    if (gt.busy) {
        SDL_SemWait(gt.done);
        gt.busy = 0;
    }
}
static void gt_submit(void) {
    gt_wait_idle();
    gt.sub = &gt.buf[gt.cur];
    gt.busy = 1;
    SDL_SemPost(gt.work);
    gt.cur ^= 1;
    gt.buf[gt.cur].len = 0;
}
static void gt_sync(void) {
    gt_submit();
    gt_wait_idle();
}
static void gt_maybe_submit(void) {
    if (gt.buf[gt.cur].len > GT_SUBMIT_AT) gt_submit();
}

// synchronous call: fn(ctx) on the render thread
static void x_call(const uint64_t *a, const uint8_t *d) {
    (void)d;
    ((void (*)(void *))(uintptr_t)a[0])((void *)(uintptr_t)a[1]);
}
static void gt_call(void (*fn)(void *), void *ctx) {
    uint64_t *a = rec(x_call, 2, 0);
    a[0] = (uintptr_t)fn, a[1] = (uintptr_t)ctx;
    gt_sync();
}
typedef struct {
    uint64_t a[10];
    const void *p[4];
    uint64_t ret;
} GtSC;

static void x_swap(const uint64_t *a, const uint8_t *d) {
    (void)a, (void)d;
    int64_t t0 = go_mono_ns();
    if (gt.host.swap) gt.host.swap(gt.host.ud);
    gt.t_frame -= go_mono_ns() - t0;
}

int gles_thread_start(const GlesThreadHost *host) {
    if (gt.on) return 0;
    gt.host = *host;
    gt.work = SDL_CreateSemaphore(0);
    gt.done = SDL_CreateSemaphore(0);
    gt.inited = SDL_CreateSemaphore(0);
    gt.unpack_alignment = 4;
#if defined(__vita__)
    gt.unpack_alignment = 1;  // vitaGL reads rows tightly packed (no UNPACK_ALIGNMENT)
#endif
    if (!gt.work || !gt.done || !gt.inited) return -1;
    gt.th = SDL_CreateThreadWithStackSize(gt_main, "cvf render", 256 * 1024, NULL);
    if (!gt.th) return -1;
    SDL_SemWait(gt.inited);
    if (gt.init_ret != 0) {
        SDL_WaitThread(gt.th, NULL);
        gt.th = NULL;
        return gt.init_ret;
    }
    gt.on = 1;
    return 0;
}

void gles_thread_swap(void) {
    if (!gt.on) return;
    rec(x_swap, 0, 0);
    gt_submit();
}

void gles_thread_stop(void) {
    if (!gt.th) return;
    if (gt.buf[gt.cur].len) gt_submit();
    gt_wait_idle();
    gt.quit = 1;
    SDL_SemPost(gt.work);
    SDL_WaitThread(gt.th, NULL);
    gt.th = NULL;
}

uint32_t gles_thread_take_render_us(void) { return (uint32_t)SDL_AtomicSet(&gt.render_us, 0); }

#define GT_DECL_R(n) static __typeof__(p_##n) r_##n;
GT_DECL_R(glUnmapBuffer) GT_DECL_R(glGetAttribLocation) GT_DECL_R(glGetUniformLocation)
GT_DECL_R(glCheckFramebufferStatus) GT_DECL_R(glCreateProgram) GT_DECL_R(glCreateShader) GT_DECL_R(glGetError)
GT_DECL_R(glGetString) GT_DECL_R(glBufferData) GT_DECL_R(glBufferSubData) GT_DECL_R(glDeleteFramebuffers)
GT_DECL_R(glDeleteTextures) GT_DECL_R(glDrawArrays) GT_DECL_R(glDrawBuffers) GT_DECL_R(glEnableVertexAttribArray)
GT_DECL_R(glFinish) GT_DECL_R(glGenBuffers) GT_DECL_R(glGenFramebuffers) GT_DECL_R(glGenRenderbuffers)
GT_DECL_R(glGenTextures) GT_DECL_R(glGenVertexArrays) GT_DECL_R(glGetIntegerv) GT_DECL_R(glGetProgramInfoLog)
GT_DECL_R(glGetProgramiv) GT_DECL_R(glGetShaderInfoLog) GT_DECL_R(glGetShaderiv) GT_DECL_R(glReadPixels)
GT_DECL_R(glShaderSource) GT_DECL_R(glTexImage2D) GT_DECL_R(glTexSubImage2D) GT_DECL_R(glUniform1fv)
GT_DECL_R(glUniform4fv) GT_DECL_R(glUniformMatrix4fv) GT_DECL_R(glVertexAttribPointer) GT_DECL_R(glMapBufferRange)
GT_DECL_R(glBindVertexArray) GT_DECL_R(glBindBuffer) GT_DECL_R(glPixelStorei) GT_DECL_R(glVertexAttribPointer_es)
GT_DECL_R(glActiveTexture) GT_DECL_R(glAttachShader) GT_DECL_R(glBindFramebuffer) GT_DECL_R(glBindRenderbuffer) GT_DECL_R(glBindTexture) GT_DECL_R(glBlendEquation) GT_DECL_R(glBlendFunc) GT_DECL_R(glBlitFramebuffer) GT_DECL_R(glClear) GT_DECL_R(glCompileShader) GT_DECL_R(glCopyTexSubImage2D) GT_DECL_R(glDeleteProgram) GT_DECL_R(glDeleteShader) GT_DECL_R(glDepthFunc) GT_DECL_R(glDepthMask) GT_DECL_R(glDetachShader) GT_DECL_R(glDisable) GT_DECL_R(glEnable) GT_DECL_R(glFramebufferRenderbuffer) GT_DECL_R(glFramebufferTexture2D) GT_DECL_R(glFrontFace) GT_DECL_R(glLinkProgram) GT_DECL_R(glReadBuffer) GT_DECL_R(glRenderbufferStorage) GT_DECL_R(glRenderbufferStorageMultisample) GT_DECL_R(glScissor) GT_DECL_R(glTexParameteri) GT_DECL_R(glTexStorage3D) GT_DECL_R(glUniform1f) GT_DECL_R(glUniform1i) GT_DECL_R(glUniform2f) GT_DECL_R(glUniform3f) GT_DECL_R(glUniform4f) GT_DECL_R(glUseProgram) GT_DECL_R(glViewport) GT_DECL_R(glFramebufferTexture2D_es)

// ---------------------------------------------------------------- plain recorded calls
// (written by a script from the p_gl* declarations of gles_bind.c: the calls
// whose arguments are all values)
static void x_glActiveTexture(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glActiveTexture((u32)a[0]); }
static void APIENTRY q_glActiveTexture(u32 a0) { uint64_t *a = rec(x_glActiveTexture, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glAttachShader(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glAttachShader((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glAttachShader(u32 a0, u32 a1) { uint64_t *a = rec(x_glAttachShader, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glBindFramebuffer(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBindFramebuffer((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glBindFramebuffer(u32 a0, u32 a1) { uint64_t *a = rec(x_glBindFramebuffer, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glBindRenderbuffer(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBindRenderbuffer((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glBindRenderbuffer(u32 a0, u32 a1) { uint64_t *a = rec(x_glBindRenderbuffer, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glBindTexture(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBindTexture((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glBindTexture(u32 a0, u32 a1) { uint64_t *a = rec(x_glBindTexture, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glBlendEquation(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBlendEquation((u32)a[0]); }
static void APIENTRY q_glBlendEquation(u32 a0) { uint64_t *a = rec(x_glBlendEquation, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glBlendFunc(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBlendFunc((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glBlendFunc(u32 a0, u32 a1) { uint64_t *a = rec(x_glBlendFunc, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glBlitFramebuffer(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glBlitFramebuffer((i32)a[0], (i32)a[1], (i32)a[2], (i32)a[3], (i32)a[4], (i32)a[5], (i32)a[6], (i32)a[7], (u32)a[8], (u32)a[9]); }
static void APIENTRY q_glBlitFramebuffer(i32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7, u32 a8, u32 a9) { uint64_t *a = rec(x_glBlitFramebuffer, 10, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; a[5] = (uint64_t)(int64_t)a5; a[6] = (uint64_t)(int64_t)a6; a[7] = (uint64_t)(int64_t)a7; a[8] = (uint64_t)(int64_t)a8; a[9] = (uint64_t)(int64_t)a9; }
static void x_glClear(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glClear((u32)a[0]); }
static void APIENTRY q_glClear(u32 a0) { uint64_t *a = rec(x_glClear, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glCompileShader(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glCompileShader((u32)a[0]); }
static void APIENTRY q_glCompileShader(u32 a0) { uint64_t *a = rec(x_glCompileShader, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glCopyTexSubImage2D(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glCopyTexSubImage2D((u32)a[0], (i32)a[1], (i32)a[2], (i32)a[3], (i32)a[4], (i32)a[5], (i32)a[6], (i32)a[7]); }
static void APIENTRY q_glCopyTexSubImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7) { uint64_t *a = rec(x_glCopyTexSubImage2D, 8, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; a[5] = (uint64_t)(int64_t)a5; a[6] = (uint64_t)(int64_t)a6; a[7] = (uint64_t)(int64_t)a7; }
static void x_glDeleteProgram(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDeleteProgram((u32)a[0]); }
static void APIENTRY q_glDeleteProgram(u32 a0) { uint64_t *a = rec(x_glDeleteProgram, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glDeleteShader(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDeleteShader((u32)a[0]); }
static void APIENTRY q_glDeleteShader(u32 a0) { uint64_t *a = rec(x_glDeleteShader, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glDepthFunc(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDepthFunc((u32)a[0]); }
static void APIENTRY q_glDepthFunc(u32 a0) { uint64_t *a = rec(x_glDepthFunc, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glDepthMask(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDepthMask((unsigned char)a[0]); }
static void APIENTRY q_glDepthMask(unsigned char a0) { uint64_t *a = rec(x_glDepthMask, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glDetachShader(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDetachShader((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glDetachShader(u32 a0, u32 a1) { uint64_t *a = rec(x_glDetachShader, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glDisable(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glDisable((u32)a[0]); }
static void APIENTRY q_glDisable(u32 a0) { uint64_t *a = rec(x_glDisable, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glEnable(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glEnable((u32)a[0]); }
static void APIENTRY q_glEnable(u32 a0) { uint64_t *a = rec(x_glEnable, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glFramebufferRenderbuffer(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glFramebufferRenderbuffer((u32)a[0], (u32)a[1], (u32)a[2], (u32)a[3]); }
static void APIENTRY q_glFramebufferRenderbuffer(u32 a0, u32 a1, u32 a2, u32 a3) { uint64_t *a = rec(x_glFramebufferRenderbuffer, 4, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; }
static void x_glFramebufferTexture2D(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glFramebufferTexture2D((u32)a[0], (u32)a[1], (u32)a[2], (u32)a[3], (i32)a[4]); }
static void APIENTRY q_glFramebufferTexture2D(u32 a0, u32 a1, u32 a2, u32 a3, i32 a4) { uint64_t *a = rec(x_glFramebufferTexture2D, 5, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; }
static void x_glFrontFace(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glFrontFace((u32)a[0]); }
static void APIENTRY q_glFrontFace(u32 a0) { uint64_t *a = rec(x_glFrontFace, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glLinkProgram(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glLinkProgram((u32)a[0]); }
static void APIENTRY q_glLinkProgram(u32 a0) { uint64_t *a = rec(x_glLinkProgram, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glReadBuffer(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glReadBuffer((u32)a[0]); }
static void APIENTRY q_glReadBuffer(u32 a0) { uint64_t *a = rec(x_glReadBuffer, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glRenderbufferStorage(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glRenderbufferStorage((u32)a[0], (u32)a[1], (i32)a[2], (i32)a[3]); }
static void APIENTRY q_glRenderbufferStorage(u32 a0, u32 a1, i32 a2, i32 a3) { uint64_t *a = rec(x_glRenderbufferStorage, 4, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; }
static void x_glRenderbufferStorageMultisample(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glRenderbufferStorageMultisample((u32)a[0], (i32)a[1], (u32)a[2], (i32)a[3], (i32)a[4]); }
static void APIENTRY q_glRenderbufferStorageMultisample(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4) { uint64_t *a = rec(x_glRenderbufferStorageMultisample, 5, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; }
static void x_glScissor(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glScissor((i32)a[0], (i32)a[1], (i32)a[2], (i32)a[3]); }
static void APIENTRY q_glScissor(i32 a0, i32 a1, i32 a2, i32 a3) { uint64_t *a = rec(x_glScissor, 4, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; }
static void x_glTexParameteri(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glTexParameteri((u32)a[0], (u32)a[1], (i32)a[2]); }
static void APIENTRY q_glTexParameteri(u32 a0, u32 a1, i32 a2) { uint64_t *a = rec(x_glTexParameteri, 3, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; }
static void x_glTexStorage3D(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glTexStorage3D((u32)a[0], (i32)a[1], (u32)a[2], (i32)a[3], (i32)a[4], (i32)a[5]); }
static void APIENTRY q_glTexStorage3D(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4, i32 a5) { uint64_t *a = rec(x_glTexStorage3D, 6, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; a[5] = (uint64_t)(int64_t)a5; }
static void x_glUniform1f(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUniform1f((i32)a[0], w2f(a[1])); }
static void APIENTRY q_glUniform1f(i32 a0, f32 a1) { uint64_t *a = rec(x_glUniform1f, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = f2w(a1); }
static void x_glUniform1i(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUniform1i((i32)a[0], (i32)a[1]); }
static void APIENTRY q_glUniform1i(i32 a0, i32 a1) { uint64_t *a = rec(x_glUniform1i, 2, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; }
static void x_glUniform2f(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUniform2f((i32)a[0], w2f(a[1]), w2f(a[2])); }
static void APIENTRY q_glUniform2f(i32 a0, f32 a1, f32 a2) { uint64_t *a = rec(x_glUniform2f, 3, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = f2w(a1); a[2] = f2w(a2); }
static void x_glUniform3f(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUniform3f((i32)a[0], w2f(a[1]), w2f(a[2]), w2f(a[3])); }
static void APIENTRY q_glUniform3f(i32 a0, f32 a1, f32 a2, f32 a3) { uint64_t *a = rec(x_glUniform3f, 4, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = f2w(a1); a[2] = f2w(a2); a[3] = f2w(a3); }
static void x_glUniform4f(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUniform4f((i32)a[0], w2f(a[1]), w2f(a[2]), w2f(a[3]), w2f(a[4])); }
static void APIENTRY q_glUniform4f(i32 a0, f32 a1, f32 a2, f32 a3, f32 a4) { uint64_t *a = rec(x_glUniform4f, 5, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = f2w(a1); a[2] = f2w(a2); a[3] = f2w(a3); a[4] = f2w(a4); }
static void x_glUseProgram(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glUseProgram((u32)a[0]); }
static void APIENTRY q_glUseProgram(u32 a0) { uint64_t *a = rec(x_glUseProgram, 1, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; }
static void x_glViewport(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glViewport((i32)a[0], (i32)a[1], (i32)a[2], (i32)a[3]); }
static void APIENTRY q_glViewport(i32 a0, i32 a1, i32 a2, i32 a3) { uint64_t *a = rec(x_glViewport, 4, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; }
static void x_glFramebufferTexture2D_es(const uint64_t *a, const uint8_t *d) { (void)a, (void)d; r_glFramebufferTexture2D_es((u32)a[0], (u32)a[1], (u32)a[2], (u32)a[3], (i32)a[4]); }
static void APIENTRY q_glFramebufferTexture2D_es(u32 a0, u32 a1, u32 a2, u32 a3, i32 a4) { uint64_t *a = rec(x_glFramebufferTexture2D_es, 5, 0); (void)a; a[0] = (uint64_t)(int64_t)a0; a[1] = (uint64_t)(int64_t)a1; a[2] = (uint64_t)(int64_t)a2; a[3] = (uint64_t)(int64_t)a3; a[4] = (uint64_t)(int64_t)a4; }
#define GT_PLAIN(X) X(glActiveTexture) X(glAttachShader) X(glBindFramebuffer) X(glBindRenderbuffer) X(glBindTexture) X(glBlendEquation) X(glBlendFunc) X(glBlitFramebuffer) X(glClear) X(glCompileShader) X(glCopyTexSubImage2D) X(glDeleteProgram) X(glDeleteShader) X(glDepthFunc) X(glDepthMask) X(glDetachShader) X(glDisable) X(glEnable) X(glFramebufferRenderbuffer) X(glFramebufferTexture2D) X(glFrontFace) X(glLinkProgram) X(glReadBuffer) X(glRenderbufferStorage) X(glRenderbufferStorageMultisample) X(glScissor) X(glTexParameteri) X(glTexStorage3D) X(glUniform1f) X(glUniform1i) X(glUniform2f) X(glUniform3f) X(glUniform4f) X(glUseProgram) X(glViewport) X(glFramebufferTexture2D_es)

// ---------------------------------------------------------------- calls with data or shadow state
static GtAttr *gt_attrs(void) {
    u32 v = gt.vao;
    if (v >= GT_MAX_VAO) {
        if (!gt.vao_warned++) fprintf(stderr, "gles thread: vertex array %u beyond the shadow table\n", v);
        v = GT_MAX_VAO - 1;
    }
    return gt.attr[v];
}

static void x_glBindVertexArray(const uint64_t *a, const uint8_t *d) { (void)d; r_glBindVertexArray((u32)a[0]); }
static void APIENTRY q_glBindVertexArray(u32 v) {
    gt.vao = v;
    rec(x_glBindVertexArray, 1, 0)[0] = v;
}
#define CVF_GL_ARRAY_BUFFER 0x8892
static void x_glBindBuffer(const uint64_t *a, const uint8_t *d) { (void)d; r_glBindBuffer((u32)a[0], (u32)a[1]); }
static void APIENTRY q_glBindBuffer(u32 target, u32 b) {
    if (target == CVF_GL_ARRAY_BUFFER) gt.array_buffer = b;
    uint64_t *a = rec(x_glBindBuffer, 2, 0);
    a[0] = target, a[1] = b;
}
#define CVF_GL_UNPACK_ALIGNMENT 0x0CF5
static void x_glPixelStorei(const uint64_t *a, const uint8_t *d) { (void)d; r_glPixelStorei((u32)a[0], (i32)a[1]); }
static void APIENTRY q_glPixelStorei(u32 p, i32 v) {
    if (p == CVF_GL_UNPACK_ROW_LENGTH) gt.unpack_row_length = v;
#if !defined(__vita__)
    if (p == CVF_GL_UNPACK_ALIGNMENT && v > 0) gt.unpack_alignment = v;
#endif
    uint64_t *a = rec(x_glPixelStorei, 2, 0);
    a[0] = p, a[1] = (uint64_t)(int64_t)v;
}
static void x_glEnableVertexAttribArray(const uint64_t *a, const uint8_t *d) { (void)d; r_glEnableVertexAttribArray((u32)a[0]); }
static void APIENTRY q_glEnableVertexAttribArray(u32 i) {
    if (i < GT_MAX_ATTR) gt_attrs()[i].enabled = 1;
    rec(x_glEnableVertexAttribArray, 1, 0)[0] = i;
}
static void x_glVertexAttribPointer(const uint64_t *a, const uint8_t *d) {
    (void)d;
    r_glVertexAttribPointer((u32)a[0], (i32)a[1], (u32)a[2], (unsigned char)a[3], (i32)a[4], (void *)(uintptr_t)a[5]);
}
static void gt_attrib(u32 i, i32 size, u32 type, unsigned char norm, i32 stride, const void *ptr, int client) {
    if (i < GT_MAX_ATTR) {
        GtAttr *t = &gt_attrs()[i];
        t->client = (uint8_t)client, t->size = size, t->type = type, t->norm = norm, t->stride = stride;
        t->ptr = (const uint8_t *)ptr;
    }
    if (client) return;  // pointed at a copy of the vertices at the next draw
    uint64_t *a = rec(x_glVertexAttribPointer, 6, 0);
    a[0] = i, a[1] = (uint64_t)(int64_t)size, a[2] = type, a[3] = norm, a[4] = (uint64_t)(int64_t)stride,
    a[5] = (uintptr_t)ptr;
}
static void APIENTRY q_glVertexAttribPointer(u32 i, i32 size, u32 type, unsigned char norm, i32 stride, void *ptr) {
    gt_attrib(i, size, type, norm, stride, ptr, gt.array_buffer == 0 && ptr != NULL);
}
static void APIENTRY q_glVertexAttribPointer_es(u32 i, i32 size, u32 type, unsigned char norm, i32 stride, void *off) {
    gt_attrib(i, size, type, norm, stride, off, 0);  // WithOffset: always a buffer offset
}

static int gt_type_size(u32 type) {
    switch (type) {
    case 0x1400: case 0x1401: return 1;  // BYTE, UNSIGNED_BYTE
    case 0x1402: case 0x1403: case 0x140B: return 2;  // SHORT, UNSIGNED_SHORT, HALF_FLOAT
    case 0x1404: case 0x1405: case 0x1406: case 0x140C: return 4;  // INT, UNSIGNED_INT, FLOAT, FIXED
    }
    return 4;
}

// glDrawArrays: args mode, first, count, n, array buffer, then per client
// attribute index, size, type, norm, stride, offset of element `first` in
// the data; data = the merged vertex ranges.
static void x_glDrawArrays(const uint64_t *a, const uint8_t *d) {
    u32 mode = (u32)a[0];
    i32 first = (i32)a[1], count = (i32)a[2];
    int n = (int)a[3];
    u32 ab = (u32)a[4];
    if (n && ab) r_glBindBuffer(CVF_GL_ARRAY_BUFFER, 0);
    for (int k = 0; k < n; k++) {
        const uint64_t *t = a + 5 + 6 * k;
        i32 size = (i32)t[1], stride = (i32)t[4];
        u32 type = (u32)t[2];
        intptr_t es = stride ? stride : size * gt_type_size(type);
        // element `first` is at d + t[5]; GL indexes from element 0
        const uint8_t *p0 = (const uint8_t *)((uintptr_t)(d + t[5]) - (uintptr_t)(first * es));
        r_glVertexAttribPointer((u32)t[0], size, type, (unsigned char)t[3], stride, (void *)p0);
    }
    if (n && ab) r_glBindBuffer(CVF_GL_ARRAY_BUFFER, ab);
    r_glDrawArrays(mode, first, count);
}
static void APIENTRY q_glDrawArrays(u32 mode, i32 first, i32 count) {
    GtAttr *at = gt_attrs();
    int idx[GT_MAX_ATTR], n = 0;
    const uint8_t *lo[GT_MAX_ATTR], *hi[GT_MAX_ATTR];
    for (int i = 0; i < GT_MAX_ATTR; i++) {
        GtAttr *t = &at[i];
        if (!t->enabled || !t->client || count <= 0) continue;
        intptr_t es = t->stride ? t->stride : t->size * gt_type_size(t->type);
        lo[n] = t->ptr + first * es;
        hi[n] = t->ptr + (intptr_t)(first + count - 1) * es + t->size * gt_type_size(t->type);
        idx[n++] = i;
    }
    // merge overlapping ranges (interleaved attributes share one range)
    const uint8_t *mlo[GT_MAX_ATTR], *mhi[GT_MAX_ATTR];
    int m = 0;
    uint32_t total = 0;
    for (int k = 0; k < n; k++) {
        int j;
        for (j = 0; j < m; j++)
            if (lo[k] <= mhi[j] && hi[k] >= mlo[j]) {
                if (lo[k] < mlo[j]) mlo[j] = lo[k];
                if (hi[k] > mhi[j]) mhi[j] = hi[k];
                break;
            }
        if (j == m) mlo[m] = lo[k], mhi[m] = hi[k], m++;
    }
    // a merge can make two earlier ranges overlap: repeat until stable
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < m && !again; i++)
            for (int j = i + 1; j < m && !again; j++)
                if (mlo[i] <= mhi[j] && mhi[i] >= mlo[j]) {
                    if (mlo[j] < mlo[i]) mlo[i] = mlo[j];
                    if (mhi[j] > mhi[i]) mhi[i] = mhi[j];
                    mlo[j] = mlo[m - 1], mhi[j] = mhi[m - 1], m--, again = 1;
                }
    }
    uint32_t moff[GT_MAX_ATTR];
    for (int j = 0; j < m; j++) moff[j] = total, total += (uint32_t)(mhi[j] - mlo[j]), total = (total + 7u) & ~7u;
    uint32_t nargs = 5 + 6 * (uint32_t)n;
    uint64_t *a = rec(x_glDrawArrays, nargs, total);
    uint8_t *d = rec_data(a, nargs);
    a[0] = mode, a[1] = (uint64_t)(int64_t)first, a[2] = (uint64_t)(int64_t)count, a[3] = (uint64_t)n,
    a[4] = gt.array_buffer;
    for (int j = 0; j < m; j++) memcpy(d + moff[j], mlo[j], (size_t)(mhi[j] - mlo[j]));
    for (int k = 0; k < n; k++) {
        GtAttr *t = &at[idx[k]];
        uint64_t *s = a + 5 + 6 * k;
        int j = 0;
        while (j < m && !(lo[k] >= mlo[j] && hi[k] <= mhi[j])) j++;
        s[0] = (uint64_t)idx[k], s[1] = (uint64_t)(int64_t)t->size, s[2] = t->type, s[3] = t->norm,
        s[4] = (uint64_t)(int64_t)t->stride, s[5] = moff[j] + (uint64_t)(lo[k] - mlo[j]);
    }
    gt_maybe_submit();
}

// uploads: copy the data, or run synchronously when it is large
static void x_glBufferData(const uint64_t *a, const uint8_t *d) {
    r_glBufferData((u32)a[0], (ptrdiff_t)a[1], a[3] ? (void *)d : NULL, (u32)a[2]);
}
static void sc_glBufferData(void *c_) {
    GtSC *c = (GtSC *)c_;
    r_glBufferData((u32)c->a[0], (ptrdiff_t)c->a[1], (void *)c->p[0], (u32)c->a[2]);
}
static void APIENTRY q_glBufferData(u32 target, ptrdiff_t size, void *data, u32 usage) {
    if (data && (size_t)size > GT_COPY_MAX) {
        GtSC c = {{target, (uint64_t)size, usage}, {data}, 0};
        gt_call(sc_glBufferData, &c);
        return;
    }
    uint32_t nd = data && size > 0 ? (uint32_t)size : 0;
    uint64_t *a = rec(x_glBufferData, 4, nd);
    a[0] = target, a[1] = (uint64_t)size, a[2] = usage, a[3] = data != NULL;
    if (nd) memcpy(rec_data(a, 4), data, nd);
    gt_maybe_submit();
}
static void x_glBufferSubData(const uint64_t *a, const uint8_t *d) {
    r_glBufferSubData((u32)a[0], (ptrdiff_t)a[1], (ptrdiff_t)a[2], (void *)d);
}
static void sc_glBufferSubData(void *c_) {
    GtSC *c = (GtSC *)c_;
    r_glBufferSubData((u32)c->a[0], (ptrdiff_t)c->a[1], (ptrdiff_t)c->a[2], (void *)c->p[0]);
}
static void APIENTRY q_glBufferSubData(u32 target, ptrdiff_t off, ptrdiff_t size, void *data) {
    if (!data || size <= 0) return;
    if ((size_t)size > GT_COPY_MAX) {
        GtSC c = {{target, (uint64_t)off, (uint64_t)size}, {data}, 0};
        gt_call(sc_glBufferSubData, &c);
        return;
    }
    uint64_t *a = rec(x_glBufferSubData, 3, (uint32_t)size);
    a[0] = target, a[1] = (uint64_t)off, a[2] = (uint64_t)size;
    memcpy(rec_data(a, 3), data, (size_t)size);
    gt_maybe_submit();
}

// bytes glTex(Sub)Image2D reads for a w x h image (0: unknown format)
static size_t gt_image_bytes(i32 w, i32 h, u32 format, u32 type) {
    int comp;
    switch (format) {
    case 0x1903: case 0x1906: case 0x1909: comp = 1; break;  // RED, ALPHA, LUMINANCE
    case 0x8227: case 0x190A: comp = 2; break;              // RG, LUMINANCE_ALPHA
    case 0x1907: comp = 3; break;                           // RGB
    case 0x1908: comp = 4; break;                           // RGBA
    default: return 0;
    }
    size_t bpp;
    switch (type) {
    case 0x1401: case 0x1400: bpp = (size_t)comp; break;
    case 0x1406: case 0x1404: case 0x1405: bpp = 4u * comp; break;
    case 0x140B: case 0x1402: case 0x1403: bpp = 2u * comp; break;
    case 0x8363: case 0x8033: case 0x8034: bpp = 2; break;  // packed 16-bit
    default: return 0;
    }
    if (w <= 0 || h <= 0) return 0;
    size_t rowlen = (size_t)w;
#if !defined(__vita__)
    if (gt.unpack_row_length > 0) rowlen = (size_t)gt.unpack_row_length;
#endif
    size_t al = (size_t)(gt.unpack_alignment > 0 ? gt.unpack_alignment : 1);
    size_t stride = (rowlen * bpp + al - 1) / al * al;
    return stride * (size_t)(h - 1) + (size_t)w * bpp;
}
static void x_glTexImage2D(const uint64_t *a, const uint8_t *d) {
    r_glTexImage2D((u32)a[0], (i32)a[1], (i32)a[2], (i32)a[3], (i32)a[4], (i32)a[5], (u32)a[6], (u32)a[7],
                   a[8] ? (void *)d : NULL);
}
static void sc_glTexImage2D(void *c_) {
    GtSC *c = (GtSC *)c_;
    r_glTexImage2D((u32)c->a[0], (i32)c->a[1], (i32)c->a[2], (i32)c->a[3], (i32)c->a[4], (i32)c->a[5], (u32)c->a[6],
                   (u32)c->a[7], (void *)c->p[0]);
}
static void APIENTRY q_glTexImage2D(u32 t, i32 lvl, i32 ifmt, i32 w, i32 h, i32 border, u32 fmt, u32 type, void *data) {
    size_t n = data ? gt_image_bytes(w, h, fmt, type) : 0;
    if (data && (n == 0 || n > GT_COPY_MAX)) {
        GtSC c = {{t, (uint64_t)(int64_t)lvl, (uint64_t)(int64_t)ifmt, (uint64_t)(int64_t)w, (uint64_t)(int64_t)h,
                   (uint64_t)(int64_t)border, fmt, type}, {data}, 0};
        gt_call(sc_glTexImage2D, &c);
        return;
    }
    uint64_t *a = rec(x_glTexImage2D, 9, (uint32_t)n);
    a[0] = t, a[1] = (uint64_t)(int64_t)lvl, a[2] = (uint64_t)(int64_t)ifmt, a[3] = (uint64_t)(int64_t)w,
    a[4] = (uint64_t)(int64_t)h, a[5] = (uint64_t)(int64_t)border, a[6] = fmt, a[7] = type, a[8] = data != NULL;
    if (n) memcpy(rec_data(a, 9), data, n);
    gt_maybe_submit();
}
static void x_glTexSubImage2D(const uint64_t *a, const uint8_t *d) {
    r_glTexSubImage2D((u32)a[0], (i32)a[1], (i32)a[2], (i32)a[3], (i32)a[4], (i32)a[5], (u32)a[6], (u32)a[7],
                      a[8] ? (void *)d : NULL);
}
static void sc_glTexSubImage2D(void *c_) {
    GtSC *c = (GtSC *)c_;
    r_glTexSubImage2D((u32)c->a[0], (i32)c->a[1], (i32)c->a[2], (i32)c->a[3], (i32)c->a[4], (i32)c->a[5], (u32)c->a[6],
                      (u32)c->a[7], (void *)c->p[0]);
}
static void APIENTRY q_glTexSubImage2D(u32 t, i32 lvl, i32 x, i32 y, i32 w, i32 h, u32 fmt, u32 type, void *data) {
    size_t n = data ? gt_image_bytes(w, h, fmt, type) : 0;
    if (data && (n == 0 || n > GT_COPY_MAX)) {
        GtSC c = {{t, (uint64_t)(int64_t)lvl, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y, (uint64_t)(int64_t)w,
                   (uint64_t)(int64_t)h, fmt, type}, {data}, 0};
        gt_call(sc_glTexSubImage2D, &c);
        return;
    }
    uint64_t *a = rec(x_glTexSubImage2D, 9, (uint32_t)n);
    a[0] = t, a[1] = (uint64_t)(int64_t)lvl, a[2] = (uint64_t)(int64_t)x, a[3] = (uint64_t)(int64_t)y,
    a[4] = (uint64_t)(int64_t)w, a[5] = (uint64_t)(int64_t)h, a[6] = fmt, a[7] = type, a[8] = data != NULL;
    if (n) memcpy(rec_data(a, 9), data, n);
    gt_maybe_submit();
}

// arrays passed by pointer: uniforms, id lists
#define GT_UNIFORMV(name, per)                                                                                \
    static void x_##name(const uint64_t *a, const uint8_t *d) { r_##name((i32)a[0], (i32)a[1], (f32 *)d); } \
    static void APIENTRY q_##name(i32 loc, i32 count, f32 *v) {                                              \
        uint32_t nd = count > 0 && v ? (uint32_t)count * (per) * 4u : 0;                                     \
        uint64_t *a = rec(x_##name, 2, nd);                                                                  \
        a[0] = (uint64_t)(int64_t)loc, a[1] = (uint64_t)(int64_t)(nd ? count : 0);                           \
        if (nd) memcpy(rec_data(a, 2), v, nd);                                                               \
    }
GT_UNIFORMV(glUniform1fv, 1)
GT_UNIFORMV(glUniform4fv, 4)
static void x_glUniformMatrix4fv(const uint64_t *a, const uint8_t *d) {
    r_glUniformMatrix4fv((i32)a[0], (i32)a[1], (unsigned char)a[2], (f32 *)d);
}
static void APIENTRY q_glUniformMatrix4fv(i32 loc, i32 count, unsigned char tr, f32 *v) {
    uint32_t nd = count > 0 && v ? (uint32_t)count * 64u : 0;
    uint64_t *a = rec(x_glUniformMatrix4fv, 3, nd);
    a[0] = (uint64_t)(int64_t)loc, a[1] = (uint64_t)(int64_t)(nd ? count : 0), a[2] = tr;
    if (nd) memcpy(rec_data(a, 3), v, nd);
}
#define GT_IDS(name)                                                                                   \
    static void x_##name(const uint64_t *a, const uint8_t *d) { r_##name((i32)a[0], (u32 *)d); }     \
    static void APIENTRY q_##name(i32 n, u32 *ids) {                                                  \
        uint32_t nd = n > 0 && ids ? (uint32_t)n * 4u : 0;                                            \
        uint64_t *a = rec(x_##name, 1, nd);                                                           \
        a[0] = (uint64_t)(int64_t)(nd ? n : 0);                                                       \
        if (nd) memcpy(rec_data(a, 1), ids, nd);                                                      \
    }
GT_IDS(glDeleteFramebuffers)
GT_IDS(glDeleteTextures)
GT_IDS(glDrawBuffers)

// ---------------------------------------------------------------- synchronous calls
#define GT_SYNC_GEN(name)                                                                        \
    static void sc_##name(void *c_) {                                                            \
        GtSC *c = (GtSC *)c_;                                                                    \
        r_##name((i32)c->a[0], (u32 *)c->p[0]);                                                  \
    }                                                                                            \
    static void APIENTRY q_##name(i32 n, u32 *out) {                                             \
        GtSC c = {{(uint64_t)(int64_t)n}, {out}, 0};                                             \
        gt_call(sc_##name, &c);                                                                  \
    }
GT_SYNC_GEN(glGenBuffers)
GT_SYNC_GEN(glGenFramebuffers)
GT_SYNC_GEN(glGenRenderbuffers)
GT_SYNC_GEN(glGenTextures)
GT_SYNC_GEN(glGenVertexArrays)

static void sc_glUnmapBuffer(void *c_) { GtSC *c = (GtSC *)c_; c->ret = r_glUnmapBuffer((u32)c->a[0]); }
static unsigned char APIENTRY q_glUnmapBuffer(u32 t) { GtSC c = {{t}, {0}, 0}; gt_call(sc_glUnmapBuffer, &c); return (unsigned char)c.ret; }
static void sc_glGetAttribLocation(void *c_) { GtSC *c = (GtSC *)c_; c->ret = (uint64_t)(int64_t)r_glGetAttribLocation((u32)c->a[0], (u8 *)c->p[0]); }
static i32 APIENTRY q_glGetAttribLocation(u32 p, u8 *n) { GtSC c = {{p}, {n}, 0}; gt_call(sc_glGetAttribLocation, &c); return (i32)c.ret; }
static void sc_glGetUniformLocation(void *c_) { GtSC *c = (GtSC *)c_; c->ret = (uint64_t)(int64_t)r_glGetUniformLocation((u32)c->a[0], (u8 *)c->p[0]); }
static i32 APIENTRY q_glGetUniformLocation(u32 p, u8 *n) { GtSC c = {{p}, {n}, 0}; gt_call(sc_glGetUniformLocation, &c); return (i32)c.ret; }
static void sc_glCheckFramebufferStatus(void *c_) { GtSC *c = (GtSC *)c_; c->ret = r_glCheckFramebufferStatus((u32)c->a[0]); }
static u32 APIENTRY q_glCheckFramebufferStatus(u32 t) { GtSC c = {{t}, {0}, 0}; gt_call(sc_glCheckFramebufferStatus, &c); return (u32)c.ret; }
static void sc_glCreateProgram(void *c_) { GtSC *c = (GtSC *)c_; c->ret = r_glCreateProgram(); }
static u32 APIENTRY q_glCreateProgram(void) { GtSC c = {{0}, {0}, 0}; gt_call(sc_glCreateProgram, &c); return (u32)c.ret; }
static void sc_glCreateShader(void *c_) { GtSC *c = (GtSC *)c_; c->ret = r_glCreateShader((u32)c->a[0]); }
static u32 APIENTRY q_glCreateShader(u32 t) { GtSC c = {{t}, {0}, 0}; gt_call(sc_glCreateShader, &c); return (u32)c.ret; }
static u32 APIENTRY q_glGetError(void) { return 0; }  // see the top of this file
static void sc_glGetString(void *c_) { GtSC *c = (GtSC *)c_; c->ret = (uintptr_t)r_glGetString((u32)c->a[0]); }
static u8 *APIENTRY q_glGetString(u32 n) { GtSC c = {{n}, {0}, 0}; gt_call(sc_glGetString, &c); return (u8 *)(uintptr_t)c.ret; }
static void sc_glFinish(void *c_) { (void)c_; r_glFinish(); }
static void APIENTRY q_glFinish(void) { gt_call(sc_glFinish, NULL); }
static void sc_glGetIntegerv(void *c_) { GtSC *c = (GtSC *)c_; r_glGetIntegerv((u32)c->a[0], (i32 *)c->p[0]); }
static void APIENTRY q_glGetIntegerv(u32 p, i32 *out) { GtSC c = {{p}, {out}, 0}; gt_call(sc_glGetIntegerv, &c); }
static void sc_glGetProgramiv(void *c_) { GtSC *c = (GtSC *)c_; r_glGetProgramiv((u32)c->a[0], (u32)c->a[1], (i32 *)c->p[0]); }
static void APIENTRY q_glGetProgramiv(u32 p, u32 n, i32 *out) { GtSC c = {{p, n}, {out}, 0}; gt_call(sc_glGetProgramiv, &c); }
static void sc_glGetShaderiv(void *c_) { GtSC *c = (GtSC *)c_; r_glGetShaderiv((u32)c->a[0], (u32)c->a[1], (i32 *)c->p[0]); }
static void APIENTRY q_glGetShaderiv(u32 s, u32 n, i32 *out) { GtSC c = {{s, n}, {out}, 0}; gt_call(sc_glGetShaderiv, &c); }
static void sc_glGetProgramInfoLog(void *c_) { GtSC *c = (GtSC *)c_; r_glGetProgramInfoLog((u32)c->a[0], (i32)c->a[1], (i32 *)c->p[0], (u8 *)c->p[1]); }
static void APIENTRY q_glGetProgramInfoLog(u32 p, i32 n, i32 *len, u8 *log) { GtSC c = {{p, (uint64_t)(int64_t)n}, {len, log}, 0}; gt_call(sc_glGetProgramInfoLog, &c); }
static void sc_glGetShaderInfoLog(void *c_) { GtSC *c = (GtSC *)c_; r_glGetShaderInfoLog((u32)c->a[0], (i32)c->a[1], (i32 *)c->p[0], (u8 *)c->p[1]); }
static void APIENTRY q_glGetShaderInfoLog(u32 s, i32 n, i32 *len, u8 *log) { GtSC c = {{s, (uint64_t)(int64_t)n}, {len, log}, 0}; gt_call(sc_glGetShaderInfoLog, &c); }
static void sc_glShaderSource(void *c_) { GtSC *c = (GtSC *)c_; r_glShaderSource((u32)c->a[0], (i32)c->a[1], (u8 **)c->p[0], (i32 *)c->p[1]); }
static void APIENTRY q_glShaderSource(u32 s, i32 n, u8 **src, i32 *len) { GtSC c = {{s, (uint64_t)(int64_t)n}, {src, len}, 0}; gt_call(sc_glShaderSource, &c); }
static void sc_glReadPixels(void *c_) {
    GtSC *c = (GtSC *)c_;
    r_glReadPixels((i32)c->a[0], (i32)c->a[1], (i32)c->a[2], (i32)c->a[3], (u32)c->a[4], (u32)c->a[5], (void *)c->p[0]);
}
static void APIENTRY q_glReadPixels(i32 x, i32 y, i32 w, i32 h, u32 f, u32 t, void *out) {
    GtSC c = {{(uint64_t)(int64_t)x, (uint64_t)(int64_t)y, (uint64_t)(int64_t)w, (uint64_t)(int64_t)h, f, t}, {out}, 0};
    gt_call(sc_glReadPixels, &c);
}
static void sc_glMapBufferRange(void *c_) { GtSC *c = (GtSC *)c_; c->ret = (uintptr_t)r_glMapBufferRange((u32)c->a[0], (ptrdiff_t)c->a[1], (ptrdiff_t)c->a[2], (u32)c->a[3]); }
static void *APIENTRY q_glMapBufferRange(u32 t, ptrdiff_t off, ptrdiff_t len, u32 acc) {
    // the engine writes into the mapping before glUnmapBuffer, which is
    // synchronous too, so nothing runs on the render thread meanwhile
    GtSC c = {{t, (uint64_t)off, (uint64_t)len, acc}, {0}, 0};
    gt_call(sc_glMapBufferRange, &c);
    return (void *)(uintptr_t)c.ret;
}

// ---------------------------------------------------------------- install
#define GT_SWAP(n) r_##n = p_##n, p_##n = q_##n;
static void gt_install(void) {
    GT_PLAIN(GT_SWAP)
    GT_SWAP(glUnmapBuffer) GT_SWAP(glGetAttribLocation) GT_SWAP(glGetUniformLocation)
    GT_SWAP(glCheckFramebufferStatus) GT_SWAP(glCreateProgram) GT_SWAP(glCreateShader) GT_SWAP(glGetError)
    GT_SWAP(glGetString) GT_SWAP(glBufferData) GT_SWAP(glBufferSubData) GT_SWAP(glDeleteFramebuffers)
    GT_SWAP(glDeleteTextures) GT_SWAP(glDrawArrays) GT_SWAP(glDrawBuffers) GT_SWAP(glEnableVertexAttribArray)
    GT_SWAP(glFinish) GT_SWAP(glGenBuffers) GT_SWAP(glGenFramebuffers) GT_SWAP(glGenRenderbuffers)
    GT_SWAP(glGenTextures) GT_SWAP(glGenVertexArrays) GT_SWAP(glGetIntegerv) GT_SWAP(glGetProgramInfoLog)
    GT_SWAP(glGetProgramiv) GT_SWAP(glGetShaderInfoLog) GT_SWAP(glGetShaderiv) GT_SWAP(glReadPixels)
    GT_SWAP(glShaderSource) GT_SWAP(glTexImage2D) GT_SWAP(glTexSubImage2D) GT_SWAP(glUniform1fv)
    GT_SWAP(glUniform4fv) GT_SWAP(glUniformMatrix4fv) GT_SWAP(glVertexAttribPointer) GT_SWAP(glMapBufferRange)
    GT_SWAP(glBindVertexArray) GT_SWAP(glBindBuffer) GT_SWAP(glPixelStorei) GT_SWAP(glVertexAttribPointer_es)
    if (gl_check) fprintf(stderr, "gles thread: CVF_GL_CHECK is not available with the render thread\n");
    gl_check = 0;
    fprintf(stderr, "gles: GL calls go to the render thread\n");
}
