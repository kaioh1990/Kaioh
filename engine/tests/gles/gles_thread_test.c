// Desktop check of the GLES render thread (src/platform/gles/gles_thread.h,
// VITA_PORT.md section 5): draws the same frames through the GLES binding
// once directly and once through the render thread, and prints an md5 of
// every frame's pixels. The two runs must print the same lines.
//
//   gles_thread_test direct | thread      (needs a GLES 3 context: Mesa, Xvfb)
//
// The frames use what the Vita renderer uses: client-side vertex arrays
// (interleaved and separate, glDrawArrays with first > 0), uniforms by value
// and by pointer, texture uploads (RGBA, one-byte rows of odd width, a large
// synchronous one), a buffer object, a framebuffer object, and results read
// back in the middle of a frame (glGetUniformLocation, glReadPixels).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ik/ik_sim.h"
#include "platform/gles/gles_thread.h"

#define W 256
#define H 192

static SDL_Window *win;
static SDL_GLContext ctx;

static int host_init(void *ud) {
    (void)ud;
    return SDL_GL_MakeCurrent(win, ctx);
}
static void host_swap(void *ud) {
    (void)ud;
    SDL_GL_SwapWindow(win);
}

static u32 shader(u32 type, const char *src) {
    u32 s = gles2_CreateShader(type);
    u8 *p = (u8 *)src;
    i32 n = (i32)strlen(src);
    gles2_ShaderSource(s, 1, &p, &n);
    gles2_CompileShader(s);
    i32 ok = 0;
    gles2_GetShaderiv(s, 0x8B81, &ok);  // COMPILE_STATUS
    if (!ok) {
        char log[1024] = "";
        gles2_GetShaderInfoLog(s, sizeof log, NULL, (u8 *)log);
        fprintf(stderr, "shader: %s\n", log);
        exit(2);
    }
    return s;
}

// RFC 1321 MD5 (as vita_main.c)
typedef struct { uint32_t a, b, c, d; uint64_t n; uint8_t buf[64]; } Md5;
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static void md5_block(Md5 *m, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
        0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
        0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
        0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = d;
        d = c, c = b, b = b + rol(a + f + K[i] + w[g], R[i]), a = t;
    }
    m->a += a, m->b += b, m->c += c, m->d += d;
}
static void md5_hex(const uint8_t *p, size_t n, char out[33]) {
    Md5 m = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}};
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        m.buf[used++] = p[i];
        if (used == 64) md5_block(&m, m.buf), used = 0;
    }
    uint64_t bits = (uint64_t)n * 8;
    m.buf[used++] = 0x80;
    if (used > 56) {
        while (used < 64) m.buf[used++] = 0;
        md5_block(&m, m.buf), used = 0;
    }
    while (used < 56) m.buf[used++] = 0;
    for (int i = 0; i < 8; i++) m.buf[56 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(&m, m.buf);
    uint32_t v[4] = {m.a, m.b, m.c, m.d};
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", (v[i / 4] >> (8 * (i % 4))) & 0xff);
}

static const char *VS =
    "#version 300 es\n"
    "in vec2 position; in vec2 uv; in vec4 color;\n"
    "uniform mat4 mvp; out vec2 v_uv; out vec4 v_col;\n"
    "void main() { v_uv = uv; v_col = color; gl_Position = mvp * vec4(position, 0.0, 1.0); }\n";
static const char *FS =
    "#version 300 es\nprecision mediump float;\n"
    "in vec2 v_uv; in vec4 v_col; uniform sampler2D tex; uniform sampler2D lum;\n"
    "uniform vec4 tint; uniform float k[3]; out vec4 o;\n"
    "void main() { vec4 t = texture(tex, v_uv); float l = texture(lum, v_uv).r;\n"
    "  o = vec4(t.rgb * tint.rgb * (k[0] + k[1] * l) + v_col.rgb * k[2], t.a * tint.a); }\n";

int main(int argc, char **argv) {
    int threaded = argc > 1 && !strcmp(argv[1], "thread");
    int frames = argc > 2 ? atoi(argv[2]) : 6;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    win = SDL_CreateWindow("gles thread test", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win || !(ctx = SDL_GL_CreateContext(win))) {
        fprintf(stderr, "GL: %s\n", SDL_GetError());
        return 2;
    }
    if (threaded) {
        SDL_GL_MakeCurrent(win, NULL);
        GlesThreadHost h = {host_init, host_swap, NULL};
        if (gles_thread_start(&h) != 0) {
            fprintf(stderr, "render thread failed\n");
            return 2;
        }
    }
    GoIface err = gles2_Init((GoFunc){0});
    if (err.type) {
        fprintf(stderr, "gles2_Init failed\n");
        return 2;
    }
    printf("renderer %s\n", (const char *)gles2_GetString(0x1F01));

    u32 prog = gles2_CreateProgram();
    gles2_AttachShader(prog, shader(0x8B31, VS));
    gles2_AttachShader(prog, shader(0x8B30, FS));
    gles2_LinkProgram(prog);
    i32 linked = 0;
    gles2_GetProgramiv(prog, 0x8B82, &linked);
    if (!linked) return fprintf(stderr, "link failed\n"), 2;
    gles2_UseProgram(prog);
    i32 locPos = gles2_GetAttribLocation(prog, (u8 *)"position");
    i32 locUV = gles2_GetAttribLocation(prog, (u8 *)"uv");
    i32 locCol = gles2_GetAttribLocation(prog, (u8 *)"color");

    // textures: RGBA 64x64 (copied), one-byte rows of width 37 (UNPACK_ALIGNMENT 1),
    // and a 1024x512 RGBA upload (2 MB: synchronous path)
    u32 tex[3];
    gles2_GenTextures(3, tex);
    uint8_t *rgba = malloc(1024 * 512 * 4);
    for (int i = 0; i < 1024 * 512 * 4; i++) rgba[i] = (uint8_t)(i * 7 + (i >> 9));
    gles2_PixelStorei(0x0CF5, 1);  // UNPACK_ALIGNMENT
    gles2_ActiveTexture(0x84C0);
    gles2_BindTexture(0x0DE1, tex[0]);
    gles2_TexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, rgba);
    for (int p = 0x2800; p <= 0x2801; p++) gles2_TexParameteri(0x0DE1, (u32)p, 0x2600);  // NEAREST
    gles2_ActiveTexture(0x84C1);
    gles2_BindTexture(0x0DE1, tex[1]);
    uint8_t lum[37 * 29];
    for (int i = 0; i < (int)sizeof lum; i++) lum[i] = (uint8_t)(i * 13);
    gles2_TexImage2D(0x0DE1, 0, 0x8229, 37, 29, 0, 0x1903, 0x1401, NULL);  // R8 / RED, no data
    gles2_TexSubImage2D(0x0DE1, 0, 0, 0, 37, 29, 0x1903, 0x1401, lum);
    for (int p = 0x2800; p <= 0x2801; p++) gles2_TexParameteri(0x0DE1, (u32)p, 0x2600);
    gles2_ActiveTexture(0x84C2);
    gles2_BindTexture(0x0DE1, tex[2]);
    gles2_TexImage2D(0x0DE1, 0, 0x1908, 1024, 512, 0, 0x1908, 0x1401, rgba);
    memset(rgba, 0x55, 64 * 64 * 4);  // the copy must already have been taken
    gles2_Uniform1i(gles2_GetUniformLocation(prog, (u8 *)"tex"), 0);
    gles2_Uniform1i(gles2_GetUniformLocation(prog, (u8 *)"lum"), 1);
    i32 locMvp = gles2_GetUniformLocation(prog, (u8 *)"mvp");
    i32 locTint = gles2_GetUniformLocation(prog, (u8 *)"tint");
    i32 locK = gles2_GetUniformLocation(prog, (u8 *)"k");

    // an offscreen target, drawn into and copied (CopyTexSubImage2D) each frame
    u32 fbo, fbotex;
    gles2_GenFramebuffers(1, &fbo);
    gles2_GenTextures(1, &fbotex);
    gles2_ActiveTexture(0x84C3);
    gles2_BindTexture(0x0DE1, fbotex);
    gles2_TexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, NULL);
    gles2_BindFramebuffer(0x8D40, fbo);
    gles2_FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, fbotex, 0);
    printf("fbo status 0x%x\n", gles2_CheckFramebufferStatus(0x8D40));
    gles2_BindFramebuffer(0x8D40, 0);

    // a buffer object for one attribute of the last quad of each frame
    u32 vbo;
    gles2_GenBuffers(1, &vbo);

    gles2_Enable(0x0BE2);  // BLEND
    gles2_BlendFunc(0x0302, 0x0303);
    uint8_t *px = malloc(W * H * 4);
    float *verts = malloc(sizeof(float) * 4 * 4 * 64);
    float *cols = malloc(sizeof(float) * 4 * 4 * 64);
    for (int f = 0; f < frames; f++) {
        gles2_Viewport(0, 0, W, H);
        gles2_BindFramebuffer(0x8D40, 0);
        gles2_Clear(0x4000);
        for (int q = 0; q < 40; q++) {
            // interleaved position/uv, 4 vertices per quad, quads q*4.. of one array
            for (int v = 0; v < 4; v++) {
                float *p = verts + (q * 4 + v) * 4;
                p[0] = -0.9f + 0.045f * q + (v & 1) * 0.3f + 0.01f * f;
                p[1] = -0.8f + 0.03f * ((q * 7) % 40) + (v >> 1) * 0.35f;
                p[2] = (v & 1) ? 1.0f : 0.0f;
                p[3] = (v >> 1) ? 1.0f : 0.0f;
                float *c = cols + (q * 4 + v) * 4;
                c[0] = (q % 3) / 2.0f, c[1] = (v % 2) * 0.5f, c[2] = f / (float)frames, c[3] = 1;
            }
            float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.002f * q, 0, 0, 1};
            gles2_UniformMatrix4fv(locMvp, 1, false, mvp);
            gles2_Uniform4f(locTint, 1.0f - q / 80.0f, 0.5f + q / 80.0f, 1, 0.75f);
            float k[3] = {0.5f, 0.5f * (q & 1), 0.25f};
            gles2_Uniform1fv(locK, 3, k);
            gles2_BindBuffer(0x8892, 0);
            gles2_VertexAttribPointer((u32)locPos, 2, 0x1406, false, 16, verts);
            gles2_VertexAttribPointer((u32)locUV, 2, 0x1406, false, 16, verts + 2);
            gles2_VertexAttribPointer((u32)locCol, 4, 0x1406, false, 0, cols);
            gles2_EnableVertexAttribArray((u32)locPos);
            gles2_EnableVertexAttribArray((u32)locUV);
            gles2_EnableVertexAttribArray((u32)locCol);
            gles2_ActiveTexture(0x84C0);
            gles2_BindTexture(0x0DE1, (q % 5 == 4) ? tex[2] : tex[0]);
            gles2_DrawArrays(0x0005, q * 4, 4);  // TRIANGLE_STRIP, first = q*4
            // the engine reuses its vertex memory right away
            memset(verts + q * 16, 0, 16 * sizeof(float));
        }
        // mid-frame: palette-like row update and a value read back
        lum[f] = (uint8_t)(255 - f);
        gles2_ActiveTexture(0x84C1);
        gles2_TexSubImage2D(0x0DE1, 0, 0, f % 29, 37, 1, 0x1903, 0x1401, lum);
        if (gles2_GetUniformLocation(prog, (u8 *)"tint") != locTint) return fprintf(stderr, "uniform moved\n"), 2;
        // a quad from a buffer object (position) plus client uv/color
        float bq[8] = {0.2f, 0.2f, 0.9f, 0.2f, 0.2f, 0.9f, 0.9f, 0.9f};
        gles2_BindBuffer(0x8892, vbo);
        gles2_BufferData(0x8892, sizeof bq, bq, 0x88E8);
        gles2_VertexAttribPointerWithOffset((u32)locPos, 2, 0x1406, false, 0, 0);
        gles2_BindBuffer(0x8892, 0);
        for (int v = 0; v < 4; v++) {
            verts[v * 4 + 2] = (v & 1), verts[v * 4 + 3] = (v >> 1);
            cols[v * 4] = 1, cols[v * 4 + 1] = 0, cols[v * 4 + 2] = 0, cols[v * 4 + 3] = 1;
        }
        gles2_VertexAttribPointer((u32)locUV, 2, 0x1406, false, 16, verts + 2);
        gles2_VertexAttribPointer((u32)locCol, 4, 0x1406, false, 0, cols);
        gles2_DrawArrays(0x0005, 0, 4);
        // offscreen pass, then copy it into its texture
        gles2_BindFramebuffer(0x8D40, fbo);
        gles2_Viewport(0, 0, 64, 64);
        gles2_Clear(0x4000);
        gles2_DrawArrays(0x0005, 0, 4);
        gles2_ActiveTexture(0x84C3);
        gles2_CopyTexSubImage2D(0x0DE1, 0, 0, 0, 0, 0, 32, 32);
        gles2_BindFramebuffer(0x8D40, 0);
        gles2_Viewport(0, 0, W, H);
        gles2_ReadPixels(0, 0, W, H, 0x1908, 0x1401, px);
        char md5[33];
        md5_hex(px, W * H * 4, md5);
        printf("frame %d %s\n", f, md5);
        if (threaded) gles_thread_swap();
        else SDL_GL_SwapWindow(win);
    }
    if (threaded) gles_thread_stop();
    return 0;
}
