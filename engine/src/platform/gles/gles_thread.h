// Render thread for the GLES binding (VITA_PORT.md section 5, "Drawing on a
// second core"). With it, every GL call the translated renderer makes is
// recorded into a command buffer on the engine thread and executed by one
// render thread, which owns the GL context (vitaGL on the PS Vita). The
// engine hands a finished frame over at the swap and goes on with the next
// game tick while the render thread submits the frame to vitaGL.
//
// Only gles_bind.c implements it (built with CVF_GL_THREAD: the Vita, and
// the desktop test in tests/gles/gles_thread_test.c).
#ifndef CVF_GLES_THREAD_H
#define CVF_GLES_THREAD_H
#include <stdint.h>

typedef struct GlesThreadHost {
    // on the render thread: create/bind the GL context (vglInitExtended);
    // returns 0 on success
    int (*init)(void *ud);
    // on the render thread, at the end of every frame (vglSwapBuffers)
    void (*swap)(void *ud);
    void *ud;
} GlesThreadHost;

// Starts the render thread and runs host.init there; 0 on success. Must be
// called before gl.Init (gles2_Init), which then installs the recorder.
int gles_thread_start(const GlesThreadHost *host);
// Engine thread: end of a frame. Waits until the render thread has finished
// the previous frame, hands this one over and returns.
void gles_thread_swap(void);
// Engine thread: waits until all recorded work is done, then stops the thread.
void gles_thread_stop(void);
// Microseconds the render thread spent executing GL calls since the last
// call (swap excluded), and frames it finished.
uint32_t gles_thread_take_render_us(void);
int gles_thread_active(void);
#endif
