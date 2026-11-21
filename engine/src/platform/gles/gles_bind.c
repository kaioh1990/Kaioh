// leonkasovan/gl v3.2/gles2 functions the translated GLES renderer
// (render_gles32.go, font_gles32.go) calls: OpenGL ES 3.0 on iOS (Apple's
// OpenGLES.framework through SDL's UIKit back end), ES 3.x elsewhere.
// Entry points come from SDL_GL_GetProcAddress (dlsym on UIKit, EGL/GLX on
// other platforms), so this file needs no GL headers and links nowhere.
// The wrappers were generated from the prototypes in ik_sim.h; the helpers
// at the end and the ES 3.0 fallbacks are hand-written.
#include <stdio.h>
#include <stdlib.h>

#include "ik/ik_sim.h"

#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

#define CVF_GL_TEXTURE_2D 0x0DE1

// CVF_GL_CHECK=1 (environment or save/cvf_env.txt): glGetError after every
// void GL call, logging the first errors with the call's name.
static int gl_check, gl_check_logged;
static u32 (APIENTRY *p_glGetError_ck)(void);
static void gl_check_fail(const char *fn, u32 e) {
    if (gl_check_logged++ < 40) fprintf(stderr, "gles: %s -> error 0x%x\n", fn, e);
}
#define CK(fn) do { if (gl_check) { u32 e_ = p_glGetError_ck(); if (e_) gl_check_fail(fn, e_); } } while (0)
void cvf_gl_check_point(const char *where) { CK(where); }  // e.g. around the SDL swap

static unsigned char (APIENTRY *p_glUnmapBuffer)(u32);
static i32 (APIENTRY *p_glGetAttribLocation)(u32, u8*);
static i32 (APIENTRY *p_glGetUniformLocation)(u32, u8*);
static u32 (APIENTRY *p_glCheckFramebufferStatus)(u32);
static u32 (APIENTRY *p_glCreateProgram)(void);
static u32 (APIENTRY *p_glCreateShader)(u32);
static u32 (APIENTRY *p_glGetError)(void);
static u8* (APIENTRY *p_glGetString)(u32);
static void (APIENTRY *p_glActiveTexture)(u32);
static void (APIENTRY *p_glAttachShader)(u32, u32);
static void (APIENTRY *p_glBindBuffer)(u32, u32);
static void (APIENTRY *p_glBindFramebuffer)(u32, u32);
static void (APIENTRY *p_glBindRenderbuffer)(u32, u32);
static void (APIENTRY *p_glBindTexture)(u32, u32);
static void (APIENTRY *p_glBindVertexArray)(u32);
static void (APIENTRY *p_glBlendEquation)(u32);
static void (APIENTRY *p_glBlendFunc)(u32, u32);
static void (APIENTRY *p_glBlitFramebuffer)(i32, i32, i32, i32, i32, i32, i32, i32, u32, u32);
static void (APIENTRY *p_glBufferData)(u32, ptrdiff_t, void*, u32);
static void (APIENTRY *p_glBufferSubData)(u32, ptrdiff_t, ptrdiff_t, void*);
static void (APIENTRY *p_glClear)(u32);
static void (APIENTRY *p_glCompileShader)(u32);
static void (APIENTRY *p_glCopyTexSubImage2D)(u32, i32, i32, i32, i32, i32, i32, i32);
static void (APIENTRY *p_glDeleteFramebuffers)(i32, u32*);
static void (APIENTRY *p_glDeleteProgram)(u32);
static void (APIENTRY *p_glDeleteShader)(u32);
static void (APIENTRY *p_glDeleteTextures)(i32, u32*);
static void (APIENTRY *p_glDepthFunc)(u32);
static void (APIENTRY *p_glDepthMask)(unsigned char);
static void (APIENTRY *p_glDetachShader)(u32, u32);
static void (APIENTRY *p_glDisable)(u32);
static void (APIENTRY *p_glDrawArrays)(u32, i32, i32);
static void (APIENTRY *p_glDrawBuffers)(i32, u32*);
static void (APIENTRY *p_glEnable)(u32);
static void (APIENTRY *p_glEnableVertexAttribArray)(u32);
static void (APIENTRY *p_glFinish)(void);
static void (APIENTRY *p_glFramebufferRenderbuffer)(u32, u32, u32, u32);
static void (APIENTRY *p_glFramebufferTexture2D)(u32, u32, u32, u32, i32);
static void (APIENTRY *p_glFrontFace)(u32);
static void (APIENTRY *p_glGenBuffers)(i32, u32*);
static void (APIENTRY *p_glGenFramebuffers)(i32, u32*);
static void (APIENTRY *p_glGenRenderbuffers)(i32, u32*);
static void (APIENTRY *p_glGenTextures)(i32, u32*);
static void (APIENTRY *p_glGenVertexArrays)(i32, u32*);
static void (APIENTRY *p_glGetIntegerv)(u32, i32*);
static void (APIENTRY *p_glGetProgramInfoLog)(u32, i32, i32*, u8*);
static void (APIENTRY *p_glGetProgramiv)(u32, u32, i32*);
static void (APIENTRY *p_glGetShaderInfoLog)(u32, i32, i32*, u8*);
static void (APIENTRY *p_glGetShaderiv)(u32, u32, i32*);
static void (APIENTRY *p_glLinkProgram)(u32);
static void (APIENTRY *p_glPixelStorei)(u32, i32);
static void (APIENTRY *p_glReadBuffer)(u32);
static void (APIENTRY *p_glReadPixels)(i32, i32, i32, i32, u32, u32, void*);
static void (APIENTRY *p_glRenderbufferStorage)(u32, u32, i32, i32);
static void (APIENTRY *p_glRenderbufferStorageMultisample)(u32, i32, u32, i32, i32);
static void (APIENTRY *p_glScissor)(i32, i32, i32, i32);
static void (APIENTRY *p_glShaderSource)(u32, i32, u8**, i32*);
static void (APIENTRY *p_glTexImage2D)(u32, i32, i32, i32, i32, i32, u32, u32, void*);
static void (APIENTRY *p_glTexParameteri)(u32, u32, i32);
static void (APIENTRY *p_glTexStorage3D)(u32, i32, u32, i32, i32, i32);
static void (APIENTRY *p_glTexSubImage2D)(u32, i32, i32, i32, i32, i32, u32, u32, void*);
static void (APIENTRY *p_glUniform1f)(i32, f32);
static void (APIENTRY *p_glUniform1fv)(i32, i32, f32*);
static void (APIENTRY *p_glUniform1i)(i32, i32);
static void (APIENTRY *p_glUniform2f)(i32, f32, f32);
static void (APIENTRY *p_glUniform3f)(i32, f32, f32, f32);
static void (APIENTRY *p_glUniform4f)(i32, f32, f32, f32, f32);
static void (APIENTRY *p_glUniform4fv)(i32, i32, f32*);
static void (APIENTRY *p_glUniformMatrix4fv)(i32, i32, unsigned char, f32*);
static void (APIENTRY *p_glUseProgram)(u32);
static void (APIENTRY *p_glVertexAttribPointer)(u32, i32, u32, unsigned char, i32, void*);
static void (APIENTRY *p_glViewport)(i32, i32, i32, i32);
static void* (APIENTRY *p_glMapBufferRange)(u32, ptrdiff_t, ptrdiff_t, u32);

bool gles2_UnmapBuffer(u32 a0) { unsigned char r_ = p_glUnmapBuffer(a0); CK("glUnmapBuffer"); return r_ != 0; }
i32 gles2_GetAttribLocation(u32 a0, u8* a1) { i32 r_ = p_glGetAttribLocation(a0, a1); CK("glGetAttribLocation"); return r_; }
i32 gles2_GetUniformLocation(u32 a0, u8* a1) { i32 r_ = p_glGetUniformLocation(a0, a1); CK("glGetUniformLocation"); return r_; }
u32 gles2_CheckFramebufferStatus(u32 a0) { u32 r_ = p_glCheckFramebufferStatus(a0); CK("glCheckFramebufferStatus"); return r_; }
u32 gles2_CreateProgram(void) { u32 r_ = p_glCreateProgram(); CK("glCreateProgram"); return r_; }
u32 gles2_CreateShader(u32 a0) { u32 r_ = p_glCreateShader(a0); CK("glCreateShader"); return r_; }
u32 gles2_GetError(void) { return p_glGetError(); }
u8* gles2_GetString(u32 a0) { u8* r_ = p_glGetString(a0); CK("glGetString"); return r_; }
void gles2_ActiveTexture(u32 a0) { p_glActiveTexture(a0); CK("glActiveTexture"); }
void gles2_AttachShader(u32 a0, u32 a1) { p_glAttachShader(a0, a1); CK("glAttachShader"); }
void gles2_BindBuffer(u32 a0, u32 a1) { p_glBindBuffer(a0, a1); CK("glBindBuffer"); }
void gles2_BindTexture(u32 a0, u32 a1) { p_glBindTexture(a0, a1); CK("glBindTexture"); }
void gles2_BindVertexArray(u32 a0) { p_glBindVertexArray(a0); CK("glBindVertexArray"); }
void gles2_BlendEquation(u32 a0) { p_glBlendEquation(a0); CK("glBlendEquation"); }
void gles2_BlendFunc(u32 a0, u32 a1) { p_glBlendFunc(a0, a1); CK("glBlendFunc"); }
void gles2_BlitFramebuffer(i32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7, u32 a8, u32 a9) { p_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); CK("glBlitFramebuffer"); }
void gles2_BufferData(u32 a0, int a1, void* a2, u32 a3) { p_glBufferData(a0, a1, a2, a3); CK("glBufferData"); }
void gles2_BufferSubData(u32 a0, int a1, int a2, void* a3) { p_glBufferSubData(a0, a1, a2, a3); CK("glBufferSubData"); }
void gles2_Clear(u32 a0) { p_glClear(a0); CK("glClear"); }
void gles2_CompileShader(u32 a0) { p_glCompileShader(a0); CK("glCompileShader"); }
void gles2_CopyTexSubImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7) { p_glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7); CK("glCopyTexSubImage2D"); }
void gles2_DeleteFramebuffers(i32 a0, u32* a1) { p_glDeleteFramebuffers(a0, a1); CK("glDeleteFramebuffers"); }
void gles2_DeleteProgram(u32 a0) { p_glDeleteProgram(a0); CK("glDeleteProgram"); }
void gles2_DeleteShader(u32 a0) { p_glDeleteShader(a0); CK("glDeleteShader"); }
void gles2_DeleteTextures(i32 a0, u32* a1) { p_glDeleteTextures(a0, a1); CK("glDeleteTextures"); }
void gles2_DepthFunc(u32 a0) { p_glDepthFunc(a0); CK("glDepthFunc"); }
void gles2_DepthMask(bool a0) { p_glDepthMask(a0); CK("glDepthMask"); }
void gles2_DetachShader(u32 a0, u32 a1) { p_glDetachShader(a0, a1); CK("glDetachShader"); }
void gles2_Disable(u32 a0) { p_glDisable(a0); CK("glDisable"); }
void gles2_DrawArrays(u32 a0, i32 a1, i32 a2) { p_glDrawArrays(a0, a1, a2); CK("glDrawArrays"); }
void gles2_DrawBuffers(i32 a0, u32* a1) { p_glDrawBuffers(a0, a1); CK("glDrawBuffers"); }
void gles2_Enable(u32 a0) { p_glEnable(a0); CK("glEnable"); }
void gles2_EnableVertexAttribArray(u32 a0) { p_glEnableVertexAttribArray(a0); CK("glEnableVertexAttribArray"); }
void gles2_Finish(void) { p_glFinish(); CK("glFinish"); }
void gles2_FramebufferRenderbuffer(u32 a0, u32 a1, u32 a2, u32 a3) { p_glFramebufferRenderbuffer(a0, a1, a2, a3); CK("glFramebufferRenderbuffer"); }
void gles2_FramebufferTexture2D(u32 a0, u32 a1, u32 a2, u32 a3, i32 a4) { p_glFramebufferTexture2D(a0, a1, a2, a3, a4); CK("glFramebufferTexture2D"); }
void gles2_FrontFace(u32 a0) { p_glFrontFace(a0); CK("glFrontFace"); }
void gles2_GenBuffers(i32 a0, u32* a1) { p_glGenBuffers(a0, a1); CK("glGenBuffers"); }
void gles2_GenFramebuffers(i32 a0, u32* a1) { p_glGenFramebuffers(a0, a1); CK("glGenFramebuffers"); }
void gles2_GenRenderbuffers(i32 a0, u32* a1) { p_glGenRenderbuffers(a0, a1); CK("glGenRenderbuffers"); }
void gles2_GenTextures(i32 a0, u32* a1) { p_glGenTextures(a0, a1); CK("glGenTextures"); }
void gles2_GenVertexArrays(i32 a0, u32* a1) { p_glGenVertexArrays(a0, a1); CK("glGenVertexArrays"); }
void gles2_GetIntegerv(u32 a0, i32* a1) { p_glGetIntegerv(a0, a1); CK("glGetIntegerv"); }
void gles2_GetProgramInfoLog(u32 a0, i32 a1, i32* a2, u8* a3) { p_glGetProgramInfoLog(a0, a1, a2, a3); CK("glGetProgramInfoLog"); }
void gles2_GetProgramiv(u32 a0, u32 a1, i32* a2) { p_glGetProgramiv(a0, a1, a2); CK("glGetProgramiv"); }
void gles2_GetShaderInfoLog(u32 a0, i32 a1, i32* a2, u8* a3) { p_glGetShaderInfoLog(a0, a1, a2, a3); CK("glGetShaderInfoLog"); }
void gles2_GetShaderiv(u32 a0, u32 a1, i32* a2) { p_glGetShaderiv(a0, a1, a2); CK("glGetShaderiv"); }
void gles2_LinkProgram(u32 a0) { p_glLinkProgram(a0); CK("glLinkProgram"); }
void gles2_PixelStorei(u32 a0, i32 a1) { p_glPixelStorei(a0, a1); CK("glPixelStorei"); }
void gles2_ReadBuffer(u32 a0) { p_glReadBuffer(a0); CK("glReadBuffer"); }
void gles2_ReadPixels(i32 a0, i32 a1, i32 a2, i32 a3, u32 a4, u32 a5, void* a6) { p_glReadPixels(a0, a1, a2, a3, a4, a5, a6); CK("glReadPixels"); }
void gles2_RenderbufferStorage(u32 a0, u32 a1, i32 a2, i32 a3) { p_glRenderbufferStorage(a0, a1, a2, a3); CK("glRenderbufferStorage"); }
void gles2_RenderbufferStorageMultisample(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4) { p_glRenderbufferStorageMultisample(a0, a1, a2, a3, a4); CK("glRenderbufferStorageMultisample"); }
void gles2_Scissor(i32 a0, i32 a1, i32 a2, i32 a3) { p_glScissor(a0, a1, a2, a3); CK("glScissor"); }
void gles2_ShaderSource(u32 a0, i32 a1, u8** a2, i32* a3) { p_glShaderSource(a0, a1, a2, a3); CK("glShaderSource"); }
void gles2_TexImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, u32 a6, u32 a7, void* a8) { p_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); CK("glTexImage2D"); }
void gles2_TexParameteri(u32 a0, u32 a1, i32 a2) { p_glTexParameteri(a0, a1, a2); CK("glTexParameteri"); }
void gles2_TexStorage3D(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4, i32 a5) { p_glTexStorage3D(a0, a1, a2, a3, a4, a5); CK("glTexStorage3D"); }
void gles2_TexSubImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, u32 a6, u32 a7, void* a8) { p_glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); CK("glTexSubImage2D"); }
void gles2_Uniform1f(i32 a0, f32 a1) { p_glUniform1f(a0, a1); CK("glUniform1f"); }
void gles2_Uniform1fv(i32 a0, i32 a1, f32* a2) { p_glUniform1fv(a0, a1, a2); CK("glUniform1fv"); }
void gles2_Uniform1i(i32 a0, i32 a1) { p_glUniform1i(a0, a1); CK("glUniform1i"); }
void gles2_Uniform2f(i32 a0, f32 a1, f32 a2) { p_glUniform2f(a0, a1, a2); CK("glUniform2f"); }
void gles2_Uniform3f(i32 a0, f32 a1, f32 a2, f32 a3) { p_glUniform3f(a0, a1, a2, a3); CK("glUniform3f"); }
void gles2_Uniform4f(i32 a0, f32 a1, f32 a2, f32 a3, f32 a4) { p_glUniform4f(a0, a1, a2, a3, a4); CK("glUniform4f"); }
void gles2_Uniform4fv(i32 a0, i32 a1, f32* a2) { p_glUniform4fv(a0, a1, a2); CK("glUniform4fv"); }
void gles2_UniformMatrix4fv(i32 a0, i32 a1, bool a2, f32* a3) { p_glUniformMatrix4fv(a0, a1, a2, a3); CK("glUniformMatrix4fv"); }
void gles2_UseProgram(u32 a0) { p_glUseProgram(a0); CK("glUseProgram"); }
void gles2_VertexAttribPointer(u32 a0, i32 a1, u32 a2, bool a3, i32 a4, void* a5) { p_glVertexAttribPointer(a0, a1, a2, a3, a4, a5); CK("glVertexAttribPointer"); }
void gles2_Viewport(i32 a0, i32 a1, i32 a2, i32 a3) { p_glViewport(a0, a1, a2, a3); CK("glViewport"); }
void* gles2_MapBufferRange(u32 a0, int a1, int a2, u32 a3) { void* r_ = p_glMapBufferRange(a0, a1, a2, a3); CK("glMapBufferRange"); return r_; }
static void (APIENTRY *p_glFramebufferTexture2D_es)(u32, u32, u32, u32, i32);
static void (APIENTRY *p_glVertexAttribPointer_es)(u32, i32, u32, unsigned char, i32, void *);

// Framebuffer 0 is the screen for the renderer. On iOS the screen is an FBO
// that SDL's UIKit GL view creates and binds when the context is made
// current; binding 0 there would draw nowhere (GL_INVALID_FRAMEBUFFER_OPERATION).
// gl.Init runs right after GLMakeCurrent, so the binding seen there is the
// screen (0 on platforms with a real default framebuffer).
#define CVF_GL_FRAMEBUFFER_BINDING 0x8CA6
#define CVF_GL_RENDERBUFFER_BINDING 0x8CA7
#define CVF_GL_RENDERBUFFER 0x8D41
static u32 screen_fbo, screen_rbo;
static int gles_ready;
// Same for renderbuffer 0: SDL's UIKit swap presents the bound renderbuffer,
// which must be the view's color renderbuffer.
void gles2_BindRenderbuffer(u32 target, u32 rb) {
    p_glBindRenderbuffer(target, rb ? rb : screen_rbo);
    CK("glBindRenderbuffer");
}
// Before every swap (sdl_bind.c): the view's renderbuffer bound, whatever
// the renderer left bound.
void cvf_gles_before_swap(void) {
    if (gles_ready && screen_rbo) p_glBindRenderbuffer(CVF_GL_RENDERBUFFER, screen_rbo);
}
static int screen_binds;
void gles2_BindFramebuffer(u32 target, u32 fb) {
    p_glBindFramebuffer(target, fb ? fb : screen_fbo);
    CK("glBindFramebuffer");
    if (!fb && screen_binds < 3) {  // diagnostics: the first screen binds
        screen_binds++;
        fprintf(stderr, "gles: screen framebuffer %u status 0x%x error 0x%x\n", screen_fbo,
                p_glCheckFramebufferStatus(0x8D40), p_glGetError());
    }
}

// ES 3.0 has no glFramebufferTexture (3.2): attach level 0 of a 2D texture.
// Only the model-shadow path uses it, and models are off on iOS.
void gles2_FramebufferTexture(u32 target, u32 attachment, u32 texture, i32 level) {
    p_glFramebufferTexture2D_es(target, attachment, CVF_GL_TEXTURE_2D, texture, level);
    CK("glFramebufferTexture");
}
void gles2_VertexAttribPointerWithOffset(u32 index, i32 size, u32 type, bool normalized, i32 stride, uintptr_t offset) {
    p_glVertexAttribPointer_es(index, size, type, normalized, stride, (void *)offset);
    CK("glVertexAttribPointerWithOffset");
}

#define LOAD(n) (*(void **)&p_##n = SDL_GL_GetProcAddress(#n))
// gl.Init: the Go loader argument (eglGetProcAddress) is ignored.
GoIface gles2_Init(GoFunc getProcAddr) {
    (void)getProcAddr;
    LOAD(glUnmapBuffer);
    LOAD(glGetAttribLocation);
    LOAD(glGetUniformLocation);
    LOAD(glCheckFramebufferStatus);
    LOAD(glCreateProgram);
    LOAD(glCreateShader);
    LOAD(glGetError);
    LOAD(glGetString);
    LOAD(glActiveTexture);
    LOAD(glAttachShader);
    LOAD(glBindBuffer);
    LOAD(glBindFramebuffer);
    LOAD(glBindRenderbuffer);
    LOAD(glBindTexture);
    LOAD(glBindVertexArray);
    LOAD(glBlendEquation);
    LOAD(glBlendFunc);
    LOAD(glBlitFramebuffer);
    LOAD(glBufferData);
    LOAD(glBufferSubData);
    LOAD(glClear);
    LOAD(glCompileShader);
    LOAD(glCopyTexSubImage2D);
    LOAD(glDeleteFramebuffers);
    LOAD(glDeleteProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteTextures);
    LOAD(glDepthFunc);
    LOAD(glDepthMask);
    LOAD(glDetachShader);
    LOAD(glDisable);
    LOAD(glDrawArrays);
    LOAD(glDrawBuffers);
    LOAD(glEnable);
    LOAD(glEnableVertexAttribArray);
    LOAD(glFinish);
    LOAD(glFramebufferRenderbuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glFrontFace);
    LOAD(glGenBuffers);
    LOAD(glGenFramebuffers);
    LOAD(glGenRenderbuffers);
    LOAD(glGenTextures);
    LOAD(glGenVertexArrays);
    LOAD(glGetIntegerv);
    LOAD(glGetProgramInfoLog);
    LOAD(glGetProgramiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetShaderiv);
    LOAD(glLinkProgram);
    LOAD(glPixelStorei);
    LOAD(glReadBuffer);
    LOAD(glReadPixels);
    LOAD(glRenderbufferStorage);
    LOAD(glRenderbufferStorageMultisample);
    LOAD(glScissor);
    LOAD(glShaderSource);
    LOAD(glTexImage2D);
    LOAD(glTexParameteri);
    LOAD(glTexStorage3D);
    LOAD(glTexSubImage2D);
    LOAD(glUniform1f);
    LOAD(glUniform1fv);
    LOAD(glUniform1i);
    LOAD(glUniform2f);
    LOAD(glUniform3f);
    LOAD(glUniform4f);
    LOAD(glUniform4fv);
    LOAD(glUniformMatrix4fv);
    LOAD(glUseProgram);
    LOAD(glVertexAttribPointer);
    LOAD(glViewport);
    LOAD(glMapBufferRange);
    *(void **)&p_glFramebufferTexture2D_es = SDL_GL_GetProcAddress("glFramebufferTexture2D");
    *(void **)&p_glVertexAttribPointer_es = SDL_GL_GetProcAddress("glVertexAttribPointer");
    if (!p_glClear || !p_glCreateShader || !p_glDrawArrays || !p_glBindVertexArray)
        return go_errorf("OpenGL ES: entry points not found (%s)", SDL_GetError());
    *(void **)&p_glGetError_ck = SDL_GL_GetProcAddress("glGetError");
    gl_check = getenv("CVF_GL_CHECK") && atoi(getenv("CVF_GL_CHECK")) > 0;
    i32 fb = 0;
    p_glGetIntegerv(CVF_GL_FRAMEBUFFER_BINDING, &fb);
    screen_fbo = (u32)fb;
    i32 rb = 0;
    p_glGetIntegerv(CVF_GL_RENDERBUFFER_BINDING, &rb);
    screen_rbo = (u32)rb;
    gles_ready = 1;
    fprintf(stderr, "gles: init: framebuffer %d, renderbuffer %d, status 0x%x\n", fb, rb, p_glCheckFramebufferStatus(0x8D40));
    return (GoIface){0};
}

// ---------------------------------------------------------------- go-gl helpers (as gl_bind.c)
u8 *gles2_Str(GoString s) { return (u8 *)s.p; }
GoString gles2_GoStr(u8 *p) { return p ? go_cstr(gc_strdup((const char *)p)) : (GoString){"", 0}; }
static void strs_free(void *env) { (void)env; }  // the copies are collected
Ret_u8PtrPtr__GoFunc gles2_Strs(Slice_GoString s) {
    u8 **v = (u8 **)gc_alloc(sizeof(u8 *) * (size_t)(s.len ? s.len : 1));
    for (int i = 0; i < s.len; i++) {
        char *c = (char *)gc_alloc_atomic((size_t)s.p[i].len + 1);
        memcpy(c, s.p[i].p, (size_t)s.p[i].len);
        v[i] = (u8 *)c;
    }
    return (Ret_u8PtrPtr__GoFunc){v, (GoFunc){(void *)strs_free, NULL}};
}
void *gles2_Ptr(GoIface x) {
    if (!x.type) return NULL;
    if (x.type < ik_tid_count && ik_tid_kind[x.type] == IK_KIND_SLICE) return ((GoSliceHdr *)x.data)->p;
    return x.data;
}
void *gles2_PtrOffset(int off) { return (void *)(intptr_t)off; }
