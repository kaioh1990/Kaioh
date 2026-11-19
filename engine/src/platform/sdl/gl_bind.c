// go-gl (v3.3-core) functions the translated renderer calls. GL entry
// points are loaded with SDL_GL_GetProcAddress by gl.Init (core profiles on
// macOS, Linux and Windows have no static exports for most of them).
// Generated from the prototypes in ik_sim.h; the helpers at the end are
// hand-written.
#include "ik/ik_sim.h"

#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

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
static u32 (APIENTRY *p_glCheckFramebufferStatus)(u32);
static void (APIENTRY *p_glClear)(u32);
static void (APIENTRY *p_glCompileShader)(u32);
static void (APIENTRY *p_glCopyTexSubImage2D)(u32, i32, i32, i32, i32, i32, i32, i32);
static u32 (APIENTRY *p_glCreateProgram)(void);
static u32 (APIENTRY *p_glCreateShader)(u32);
static void (APIENTRY *p_glCullFace)(u32);
static void (APIENTRY *p_glDeleteFramebuffers)(i32, u32*);
static void (APIENTRY *p_glDeleteProgram)(u32);
static void (APIENTRY *p_glDeleteShader)(u32);
static void (APIENTRY *p_glDeleteTextures)(i32, u32*);
static void (APIENTRY *p_glDepthFunc)(u32);
static void (APIENTRY *p_glDepthMask)(unsigned char);
static void (APIENTRY *p_glDisable)(u32);
static void (APIENTRY *p_glDisableVertexAttribArray)(u32);
static void (APIENTRY *p_glDrawArrays)(u32, i32, i32);
static void (APIENTRY *p_glDrawBuffer)(u32);
static void (APIENTRY *p_glEnable)(u32);
static void (APIENTRY *p_glEnableVertexAttribArray)(u32);
static void (APIENTRY *p_glFinish)(void);
static void (APIENTRY *p_glFramebufferRenderbuffer)(u32, u32, u32, u32);
static void (APIENTRY *p_glFramebufferTexture)(u32, u32, u32, i32);
static void (APIENTRY *p_glFramebufferTexture2D)(u32, u32, u32, u32, i32);
static void (APIENTRY *p_glFrontFace)(u32);
static void (APIENTRY *p_glGenBuffers)(i32, u32*);
static void (APIENTRY *p_glGenFramebuffers)(i32, u32*);
static void (APIENTRY *p_glGenRenderbuffers)(i32, u32*);
static void (APIENTRY *p_glGenTextures)(i32, u32*);
static void (APIENTRY *p_glGenVertexArrays)(i32, u32*);
static void (APIENTRY *p_glGenerateMipmap)(u32);
static i32 (APIENTRY *p_glGetAttribLocation)(u32, u8*);
static void (APIENTRY *p_glGetBooleanv)(u32, bool*);
static void (APIENTRY *p_glGetIntegerv)(u32, i32*);
static void (APIENTRY *p_glGetProgramInfoLog)(u32, i32, i32*, u8*);
static void (APIENTRY *p_glGetProgramiv)(u32, u32, i32*);
static void (APIENTRY *p_glGetShaderInfoLog)(u32, i32, i32*, u8*);
static void (APIENTRY *p_glGetShaderiv)(u32, u32, i32*);
static u8* (APIENTRY *p_glGetString)(u32);
static i32 (APIENTRY *p_glGetUniformLocation)(u32, u8*);
static void (APIENTRY *p_glGetVertexAttribiv)(u32, u32, i32*);
static unsigned char (APIENTRY *p_glIsEnabled)(u32);
static void (APIENTRY *p_glLinkProgram)(u32);
static void* (APIENTRY *p_glMapBuffer)(u32, u32);
static void (APIENTRY *p_glPixelStorei)(u32, i32);
static void (APIENTRY *p_glReadBuffer)(u32);
static void (APIENTRY *p_glReadPixels)(i32, i32, i32, i32, u32, u32, void*);
static void (APIENTRY *p_glRenderbufferStorage)(u32, u32, i32, i32);
static void (APIENTRY *p_glRenderbufferStorageMultisample)(u32, i32, u32, i32, i32);
static void (APIENTRY *p_glScissor)(i32, i32, i32, i32);
static void (APIENTRY *p_glShaderSource)(u32, i32, u8**, i32*);
static void (APIENTRY *p_glTexImage2D)(u32, i32, i32, i32, i32, i32, u32, u32, void*);
static void (APIENTRY *p_glTexImage2DMultisample)(u32, i32, u32, i32, i32, unsigned char);
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
static void (APIENTRY *p_glUniformMatrix3fv)(i32, i32, unsigned char, f32*);
static void (APIENTRY *p_glUniformMatrix4fv)(i32, i32, unsigned char, f32*);
static unsigned char (APIENTRY *p_glUnmapBuffer)(u32);
static void (APIENTRY *p_glUseProgram)(u32);
static void (APIENTRY *p_glVertexAttrib2f)(u32, f32, f32);
static void (APIENTRY *p_glVertexAttrib3f)(u32, f32, f32, f32);
static void (APIENTRY *p_glVertexAttrib4f)(u32, f32, f32, f32, f32);
static void (APIENTRY *p_glVertexAttribPointer)(u32, i32, u32, unsigned char, i32, void*);
static void (APIENTRY *p_glViewport)(i32, i32, i32, i32);
static void (APIENTRY *p_glDrawElements)(u32, i32, u32, const void *);
static void (APIENTRY *p_glVertexAttribPointer2)(u32, i32, u32, unsigned char, i32, const void *);

void gl_ActiveTexture(u32 a0) { p_glActiveTexture(a0); }
void gl_AttachShader(u32 a0, u32 a1) { p_glAttachShader(a0, a1); }
void gl_BindBuffer(u32 a0, u32 a1) { p_glBindBuffer(a0, a1); }
void gl_BindFramebuffer(u32 a0, u32 a1) { p_glBindFramebuffer(a0, a1); }
void gl_BindRenderbuffer(u32 a0, u32 a1) { p_glBindRenderbuffer(a0, a1); }
void gl_BindTexture(u32 a0, u32 a1) { p_glBindTexture(a0, a1); }
void gl_BindVertexArray(u32 a0) { p_glBindVertexArray(a0); }
void gl_BlendEquation(u32 a0) { p_glBlendEquation(a0); }
void gl_BlendFunc(u32 a0, u32 a1) { p_glBlendFunc(a0, a1); }
void gl_BlitFramebuffer(i32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7, u32 a8, u32 a9) { p_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }
void gl_BufferData(u32 a0, int a1, void* a2, u32 a3) { p_glBufferData(a0, (ptrdiff_t)a1, a2, a3); }
void gl_BufferSubData(u32 a0, int a1, int a2, void* a3) { p_glBufferSubData(a0, (ptrdiff_t)a1, (ptrdiff_t)a2, a3); }
u32 gl_CheckFramebufferStatus(u32 a0) { return p_glCheckFramebufferStatus(a0); }
void gl_Clear(u32 a0) { p_glClear(a0); }
void gl_CompileShader(u32 a0) { p_glCompileShader(a0); }
void gl_CopyTexSubImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, i32 a6, i32 a7) { p_glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7); }
u32 gl_CreateProgram(void) { return p_glCreateProgram(); }
u32 gl_CreateShader(u32 a0) { return p_glCreateShader(a0); }
void gl_CullFace(u32 a0) { p_glCullFace(a0); }
void gl_DeleteFramebuffers(i32 a0, u32* a1) { p_glDeleteFramebuffers(a0, a1); }
void gl_DeleteProgram(u32 a0) { p_glDeleteProgram(a0); }
void gl_DeleteShader(u32 a0) { p_glDeleteShader(a0); }
void gl_DeleteTextures(i32 a0, u32* a1) { p_glDeleteTextures(a0, a1); }
void gl_DepthFunc(u32 a0) { p_glDepthFunc(a0); }
void gl_DepthMask(bool a0) { p_glDepthMask((unsigned char)a0); }
void gl_Disable(u32 a0) { p_glDisable(a0); }
void gl_DisableVertexAttribArray(u32 a0) { p_glDisableVertexAttribArray(a0); }
void gl_DrawArrays(u32 a0, i32 a1, i32 a2) { p_glDrawArrays(a0, a1, a2); }
void gl_DrawBuffer(u32 a0) { p_glDrawBuffer(a0); }
void gl_Enable(u32 a0) { p_glEnable(a0); }
void gl_EnableVertexAttribArray(u32 a0) { p_glEnableVertexAttribArray(a0); }
void gl_Finish(void) { p_glFinish(); }
void gl_FramebufferRenderbuffer(u32 a0, u32 a1, u32 a2, u32 a3) { p_glFramebufferRenderbuffer(a0, a1, a2, a3); }
void gl_FramebufferTexture(u32 a0, u32 a1, u32 a2, i32 a3) { p_glFramebufferTexture(a0, a1, a2, a3); }
void gl_FramebufferTexture2D(u32 a0, u32 a1, u32 a2, u32 a3, i32 a4) { p_glFramebufferTexture2D(a0, a1, a2, a3, a4); }
void gl_FrontFace(u32 a0) { p_glFrontFace(a0); }
void gl_GenBuffers(i32 a0, u32* a1) { p_glGenBuffers(a0, a1); }
void gl_GenFramebuffers(i32 a0, u32* a1) { p_glGenFramebuffers(a0, a1); }
void gl_GenRenderbuffers(i32 a0, u32* a1) { p_glGenRenderbuffers(a0, a1); }
void gl_GenTextures(i32 a0, u32* a1) { p_glGenTextures(a0, a1); }
void gl_GenVertexArrays(i32 a0, u32* a1) { p_glGenVertexArrays(a0, a1); }
void gl_GenerateMipmap(u32 a0) { p_glGenerateMipmap(a0); }
i32 gl_GetAttribLocation(u32 a0, u8* a1) { return p_glGetAttribLocation(a0, a1); }
void gl_GetBooleanv(u32 a0, bool* a1) { p_glGetBooleanv(a0, a1); }
void gl_GetIntegerv(u32 a0, i32* a1) { p_glGetIntegerv(a0, a1); }
void gl_GetProgramInfoLog(u32 a0, i32 a1, i32* a2, u8* a3) { p_glGetProgramInfoLog(a0, a1, a2, a3); }
void gl_GetProgramiv(u32 a0, u32 a1, i32* a2) { p_glGetProgramiv(a0, a1, a2); }
void gl_GetShaderInfoLog(u32 a0, i32 a1, i32* a2, u8* a3) { p_glGetShaderInfoLog(a0, a1, a2, a3); }
void gl_GetShaderiv(u32 a0, u32 a1, i32* a2) { p_glGetShaderiv(a0, a1, a2); }
u8* gl_GetString(u32 a0) { return p_glGetString(a0); }
i32 gl_GetUniformLocation(u32 a0, u8* a1) { return p_glGetUniformLocation(a0, a1); }
void gl_GetVertexAttribiv(u32 a0, u32 a1, i32* a2) { p_glGetVertexAttribiv(a0, a1, a2); }
bool gl_IsEnabled(u32 a0) { return p_glIsEnabled(a0) != 0; }
void gl_LinkProgram(u32 a0) { p_glLinkProgram(a0); }
void* gl_MapBuffer(u32 a0, u32 a1) { return p_glMapBuffer(a0, a1); }
void gl_PixelStorei(u32 a0, i32 a1) { p_glPixelStorei(a0, a1); }
void gl_ReadBuffer(u32 a0) { p_glReadBuffer(a0); }
void gl_ReadPixels(i32 a0, i32 a1, i32 a2, i32 a3, u32 a4, u32 a5, void* a6) { p_glReadPixels(a0, a1, a2, a3, a4, a5, a6); }
void gl_RenderbufferStorage(u32 a0, u32 a1, i32 a2, i32 a3) { p_glRenderbufferStorage(a0, a1, a2, a3); }
void gl_RenderbufferStorageMultisample(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4) { p_glRenderbufferStorageMultisample(a0, a1, a2, a3, a4); }
void gl_Scissor(i32 a0, i32 a1, i32 a2, i32 a3) { p_glScissor(a0, a1, a2, a3); }
void gl_ShaderSource(u32 a0, i32 a1, u8** a2, i32* a3) { p_glShaderSource(a0, a1, a2, a3); }
void gl_TexImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, u32 a6, u32 a7, void* a8) { p_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
void gl_TexImage2DMultisample(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4, bool a5) { p_glTexImage2DMultisample(a0, a1, a2, a3, a4, (unsigned char)a5); }
void gl_TexParameteri(u32 a0, u32 a1, i32 a2) { p_glTexParameteri(a0, a1, a2); }
void gl_TexStorage3D(u32 a0, i32 a1, u32 a2, i32 a3, i32 a4, i32 a5) { p_glTexStorage3D(a0, a1, a2, a3, a4, a5); }
void gl_TexSubImage2D(u32 a0, i32 a1, i32 a2, i32 a3, i32 a4, i32 a5, u32 a6, u32 a7, void* a8) { p_glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
void gl_Uniform1f(i32 a0, f32 a1) { p_glUniform1f(a0, a1); }
void gl_Uniform1fv(i32 a0, i32 a1, f32* a2) { p_glUniform1fv(a0, a1, a2); }
void gl_Uniform1i(i32 a0, i32 a1) { p_glUniform1i(a0, a1); }
void gl_Uniform2f(i32 a0, f32 a1, f32 a2) { p_glUniform2f(a0, a1, a2); }
void gl_Uniform3f(i32 a0, f32 a1, f32 a2, f32 a3) { p_glUniform3f(a0, a1, a2, a3); }
void gl_Uniform4f(i32 a0, f32 a1, f32 a2, f32 a3, f32 a4) { p_glUniform4f(a0, a1, a2, a3, a4); }
void gl_Uniform4fv(i32 a0, i32 a1, f32* a2) { p_glUniform4fv(a0, a1, a2); }
void gl_UniformMatrix3fv(i32 a0, i32 a1, bool a2, f32* a3) { p_glUniformMatrix3fv(a0, a1, (unsigned char)a2, a3); }
void gl_UniformMatrix4fv(i32 a0, i32 a1, bool a2, f32* a3) { p_glUniformMatrix4fv(a0, a1, (unsigned char)a2, a3); }
bool gl_UnmapBuffer(u32 a0) { return p_glUnmapBuffer(a0) != 0; }
void gl_UseProgram(u32 a0) { p_glUseProgram(a0); }
void gl_VertexAttrib2f(u32 a0, f32 a1, f32 a2) { p_glVertexAttrib2f(a0, a1, a2); }
void gl_VertexAttrib3f(u32 a0, f32 a1, f32 a2, f32 a3) { p_glVertexAttrib3f(a0, a1, a2, a3); }
void gl_VertexAttrib4f(u32 a0, f32 a1, f32 a2, f32 a3, f32 a4) { p_glVertexAttrib4f(a0, a1, a2, a3, a4); }
void gl_VertexAttribPointer(u32 a0, i32 a1, u32 a2, bool a3, i32 a4, void* a5) { p_glVertexAttribPointer(a0, a1, a2, (unsigned char)a3, a4, a5); }
void gl_Viewport(i32 a0, i32 a1, i32 a2, i32 a3) { p_glViewport(a0, a1, a2, a3); }

void gl_DrawElementsWithOffset(u32 mode, i32 count, u32 type, uintptr_t off) { p_glDrawElements(mode, count, type, (const void *)off); }
void gl_VertexAttribPointerWithOffset(u32 i, i32 size, u32 type, bool norm, i32 stride, uintptr_t off) {
    p_glVertexAttribPointer2(i, size, type, norm, stride, (const void *)off);
}

#define LOAD(n) (*(void **)&p_##n = SDL_GL_GetProcAddress(#n))
static int gl_missing;
GoIface gl_Init(void) {
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
    LOAD(glCheckFramebufferStatus);
    LOAD(glClear);
    LOAD(glCompileShader);
    LOAD(glCopyTexSubImage2D);
    LOAD(glCreateProgram);
    LOAD(glCreateShader);
    LOAD(glCullFace);
    LOAD(glDeleteFramebuffers);
    LOAD(glDeleteProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteTextures);
    LOAD(glDepthFunc);
    LOAD(glDepthMask);
    LOAD(glDisable);
    LOAD(glDisableVertexAttribArray);
    LOAD(glDrawArrays);
    LOAD(glDrawBuffer);
    LOAD(glEnable);
    LOAD(glEnableVertexAttribArray);
    LOAD(glFinish);
    LOAD(glFramebufferRenderbuffer);
    LOAD(glFramebufferTexture);
    LOAD(glFramebufferTexture2D);
    LOAD(glFrontFace);
    LOAD(glGenBuffers);
    LOAD(glGenFramebuffers);
    LOAD(glGenRenderbuffers);
    LOAD(glGenTextures);
    LOAD(glGenVertexArrays);
    LOAD(glGenerateMipmap);
    LOAD(glGetAttribLocation);
    LOAD(glGetBooleanv);
    LOAD(glGetIntegerv);
    LOAD(glGetProgramInfoLog);
    LOAD(glGetProgramiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glGetShaderiv);
    LOAD(glGetString);
    LOAD(glGetUniformLocation);
    LOAD(glGetVertexAttribiv);
    LOAD(glIsEnabled);
    LOAD(glLinkProgram);
    LOAD(glMapBuffer);
    LOAD(glPixelStorei);
    LOAD(glReadBuffer);
    LOAD(glReadPixels);
    LOAD(glRenderbufferStorage);
    LOAD(glRenderbufferStorageMultisample);
    LOAD(glScissor);
    LOAD(glShaderSource);
    LOAD(glTexImage2D);
    LOAD(glTexImage2DMultisample);
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
    LOAD(glUniformMatrix3fv);
    LOAD(glUniformMatrix4fv);
    LOAD(glUnmapBuffer);
    LOAD(glUseProgram);
    LOAD(glVertexAttrib2f);
    LOAD(glVertexAttrib3f);
    LOAD(glVertexAttrib4f);
    LOAD(glVertexAttribPointer);
    LOAD(glViewport);
    *(void **)&p_glDrawElements = SDL_GL_GetProcAddress("glDrawElements");
    *(void **)&p_glVertexAttribPointer2 = SDL_GL_GetProcAddress("glVertexAttribPointer");
    if (!p_glClear || !p_glCreateShader || !p_glDrawElements) return go_errorf("OpenGL: entry points not found (%s)", SDL_GetError());
    (void)gl_missing;
    return (GoIface){0};
}

// ---------------------------------------------------------------- go-gl helpers
// gl.Str: Go strings passed to GL end in "\x00"; the pointer is the data.
u8 *gl_Str(GoString s) { return (u8 *)s.p; }
GoString gl_GoStr(u8 *p) { return p ? go_cstr(gc_strdup((const char *)p)) : (GoString){"", 0}; }
static void strs_free(void *env) { (void)env; }  // the copies are collected
Ret_u8PtrPtr__GoFunc gl_Strs(Slice_GoString s) {
    u8 **v = (u8 **)gc_alloc(sizeof(u8 *) * (size_t)(s.len ? s.len : 1));
    for (int i = 0; i < s.len; i++) {
        char *c = (char *)gc_alloc_atomic((size_t)s.p[i].len + 1);
        memcpy(c, s.p[i].p, (size_t)s.p[i].len);
        v[i] = (u8 *)c;
    }
    return (Ret_u8PtrPtr__GoFunc){v, (GoFunc){(void *)strs_free, NULL}};
}
// gl.Ptr: address of a pointer's target or of a slice's first element.
void *gl_Ptr(GoIface x) {
    if (!x.type) return NULL;
    if (x.type < ik_tid_count && ik_tid_kind[x.type] == IK_KIND_SLICE) return ((GoSliceHdr *)x.data)->p;
    return x.data;  // pointers and unsafe.Pointer are stored unboxed
}
void *gl_PtrOffset(int off) { return (void *)(intptr_t)off; }
void gl_DebugMessageCallback(gl_DebugProc cb, void *user) { (void)cb, (void)user; }  // debug output: not wired

