// image, image/color, image/draw and image/png for the translated game.
//
// Images are the Go layouts (ik_rt_types.h): *image.NRGBA, *image.RGBA,
// *image.Alpha, *image.Paletted, *image.Uniform. PNG is decoded here with
// the results Go's image/png gives for the cases the game relies on
// (*image.Paletted with raw indices for palette PNGs - SFF v2 format 10 -
// and *image.NRGBA otherwise); other formats go through stb_image.
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#include "third_party/stb_image.h"

#include "ik/ik_sim.h"

// ---------------------------------------------------------------- colors

#ifndef GO_TID_P_image_NRGBA
#define GO_TID_P_image_NRGBA (-10)
#endif
#ifndef GO_TID_P_image_RGBA
#define GO_TID_P_image_RGBA (-11)
#endif
#ifndef GO_TID_P_image_Alpha
#define GO_TID_P_image_Alpha (-12)
#endif
#ifndef GO_TID_P_image_Paletted
#define GO_TID_P_image_Paletted (-13)
#endif
#ifndef GO_TID_P_image_Uniform
#define GO_TID_P_image_Uniform (-14)
#endif
#ifndef GO_TID_color_NRGBA
#define GO_TID_color_NRGBA (-1)
#endif
#ifndef GO_TID_color_RGBA
#define GO_TID_color_RGBA (-2)
#endif
#ifndef GO_TID_color_Alpha
#define GO_TID_color_Alpha (-3)
#endif
#ifndef GO_TID_color_Gray
#define GO_TID_color_Gray (-4)
#endif
#ifndef GO_TID_color_Gray16
#define GO_TID_color_Gray16 (-5)
#endif

static GoIface box_nrgba(u8 r, u8 g, u8 b, u8 a) {
    GoOpaque o;
    memset(&o, 0, sizeof o);
    o._opaque[0] = r, o._opaque[1] = g, o._opaque[2] = b, o._opaque[3] = a;
    return (GoIface){GO_TID_color_NRGBA, go_box(GoOpaque, o)};
}

// premultiplied 16-bit components of a color.Color, as its RGBA() method
typedef struct { u32 r, g, b, a; } C16;
static C16 color16(GoIface c) {
    C16 o = {0, 0, 0, 0};
    if (!c.type || !c.data) return o;
    const u8 *v = ((GoOpaque *)c.data)->_opaque;
    if (c.type == GO_TID_color_NRGBA) {
        u32 a = v[3] * 0x101u;
        o.r = v[0] * 0x101u * a / 0xffff, o.g = v[1] * 0x101u * a / 0xffff, o.b = v[2] * 0x101u * a / 0xffff, o.a = a;
    } else if (c.type == GO_TID_color_Alpha) {
        o.r = o.g = o.b = o.a = v[0] * 0x101u;
    } else if (c.type == GO_TID_color_Gray) {
        o.r = o.g = o.b = v[0] * 0x101u, o.a = 0xffff;
    } else if (c.type == GO_TID_color_Gray16) {
        u16 y;
        memcpy(&y, v, 2);
        o.r = o.g = o.b = y, o.a = 0xffff;
    } else {  // color.RGBA and unknown opaque colors: premultiplied bytes
        o.r = v[0] * 0x101u, o.g = v[1] * 0x101u, o.b = v[2] * 0x101u, o.a = v[3] * 0x101u;
    }
    return o;
}

static GoImageUniform uni_white, uni_black, uni_transparent, uni_opaque;
GoImageUniform *image_White = &uni_white, *image_Black = &uni_black, *image_Transparent = &uni_transparent,
               *image_Opaque = &uni_opaque;
__attribute__((constructor)) static void image_vars_init(void) {
    // not GC memory: boxed with malloc so the collector need not see them
    static GoOpaque w, b, t, op;
    w._opaque[0] = w._opaque[1] = w._opaque[2] = w._opaque[3] = 255;
    b._opaque[3] = 255;
    op._opaque[0] = op._opaque[1] = op._opaque[2] = op._opaque[3] = 255;
    uni_white.C = (GoIface){GO_TID_color_RGBA, &w};
    uni_black.C = (GoIface){GO_TID_color_RGBA, &b};
    uni_transparent.C = (GoIface){GO_TID_color_RGBA, &t};
    uni_opaque.C = (GoIface){GO_TID_color_RGBA, &op};
}

// ---------------------------------------------------------------- geometry

GoImageRect image_Rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    return (GoImageRect){{x0, y0}, {x1, y1}};
}
GoImagePoint image_Point_Add(GoImagePoint p, GoImagePoint q) { return (GoImagePoint){p.X + q.X, p.Y + q.Y}; }
bool image_Point_In(GoImagePoint p, GoImageRect r) {
    return r.Min.X <= p.X && p.X < r.Max.X && r.Min.Y <= p.Y && p.Y < r.Max.Y;
}
int image_Rectangle_Dx(GoImageRect r) { return r.Max.X - r.Min.X; }
int image_Rectangle_Dy(GoImageRect r) { return r.Max.Y - r.Min.Y; }
bool image_Rectangle_Empty(GoImageRect r) { return r.Min.X >= r.Max.X || r.Min.Y >= r.Max.Y; }
GoImageRect image_Rectangle_Add(GoImageRect r, GoImagePoint p) {
    return (GoImageRect){{r.Min.X + p.X, r.Min.Y + p.Y}, {r.Max.X + p.X, r.Max.Y + p.Y}};
}
GoImageRect image_Rectangle_Intersect(GoImageRect r, GoImageRect s) {
    if (r.Min.X < s.Min.X) r.Min.X = s.Min.X;
    if (r.Min.Y < s.Min.Y) r.Min.Y = s.Min.Y;
    if (r.Max.X > s.Max.X) r.Max.X = s.Max.X;
    if (r.Max.Y > s.Max.Y) r.Max.Y = s.Max.Y;
    if (image_Rectangle_Empty(r)) return (GoImageRect){{0, 0}, {0, 0}};
    return r;
}

// ---------------------------------------------------------------- images

static Slice_u8 pix_alloc(int n) {
    if (n < 0) n = 0;
    return (Slice_u8){(u8 *)gc_calloc_atomic((size_t)(n > 0 ? n : 1), 1), n, n};
}
GoImageNRGBA *image_NewNRGBA(GoImageRect r) {
    GoImageNRGBA *m = go_new(GoImageNRGBA);
    int w = r.Max.X - r.Min.X, h = r.Max.Y - r.Min.Y;
    m->Rect = r, m->Stride = 4 * w, m->Pix = pix_alloc(4 * w * h);
    return m;
}
GoImageRGBA *image_NewRGBA(GoImageRect r) {
    GoImageRGBA *m = go_new(GoImageRGBA);
    int w = r.Max.X - r.Min.X, h = r.Max.Y - r.Min.Y;
    m->Rect = r, m->Stride = 4 * w, m->Pix = pix_alloc(4 * w * h);
    return m;
}
GoImageAlpha *image_NewAlpha(GoImageRect r) {
    GoImageAlpha *m = go_new(GoImageAlpha);
    int w = r.Max.X - r.Min.X, h = r.Max.Y - r.Min.Y;
    m->Rect = r, m->Stride = w, m->Pix = pix_alloc(w * h);
    return m;
}
GoImageRect image_NRGBA_Bounds(GoImageNRGBA *m) { return m->Rect; }
GoImageRect image_RGBA_Bounds(GoImageRGBA *m) { return m->Rect; }
GoImageRect image_Alpha_Bounds(GoImageAlpha *m) { return m->Rect; }
GoImageRect image_Paletted_Bounds(GoImagePaletted *m) { return m->Rect; }
GoImageRect image_Uniform_Bounds(GoImageUniform *u) {
    (void)u;
    return (GoImageRect){{-1000000000, -1000000000}, {1000000000, 1000000000}};
}
Ret_u32__u32__u32__u32 image_Uniform_RGBA(GoImageUniform *u) {
    C16 c = color16(u->C);
    return (Ret_u32__u32__u32__u32){c.r, c.g, c.b, c.a};
}

// premultiplied 16-bit color of pixel (x, y) of any supported image
static C16 image_at(GoIface img, int x, int y) {
    C16 o = {0, 0, 0, 0};
    if (img.type == GO_TID_P_image_NRGBA) {
        GoImageNRGBA *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return o;
        const u8 *p = m->Pix.p + (y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X) * 4;
        u32 a = p[3] * 0x101u;
        o.r = p[0] * 0x101u * a / 0xffff, o.g = p[1] * 0x101u * a / 0xffff, o.b = p[2] * 0x101u * a / 0xffff, o.a = a;
    } else if (img.type == GO_TID_P_image_RGBA) {
        GoImageRGBA *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return o;
        const u8 *p = m->Pix.p + (y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X) * 4;
        o.r = p[0] * 0x101u, o.g = p[1] * 0x101u, o.b = p[2] * 0x101u, o.a = p[3] * 0x101u;
    } else if (img.type == GO_TID_P_image_Alpha) {
        GoImageAlpha *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return o;
        o.r = o.g = o.b = o.a = m->Pix.p[(y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X)] * 0x101u;
    } else if (img.type == GO_TID_P_image_Paletted) {
        GoImagePaletted *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return o;
        int i = m->Pix.p[(y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X)];
        if (i < m->Palette.len) return color16(m->Palette.p[i]);
    } else if (img.type == GO_TID_P_image_Uniform) {
        return color16(((GoImageUniform *)img.data)->C);
    }
    return o;
}

static void image_set(GoIface img, int x, int y, C16 c) {
    if (img.type == GO_TID_P_image_NRGBA) {
        GoImageNRGBA *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return;
        u8 *p = m->Pix.p + (y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X) * 4;
        if (c.a == 0) {
            p[0] = p[1] = p[2] = p[3] = 0;
        } else {
            p[0] = (u8)((c.r * 0xffff / c.a) >> 8), p[1] = (u8)((c.g * 0xffff / c.a) >> 8);
            p[2] = (u8)((c.b * 0xffff / c.a) >> 8), p[3] = (u8)(c.a >> 8);
        }
    } else if (img.type == GO_TID_P_image_RGBA) {
        GoImageRGBA *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return;
        u8 *p = m->Pix.p + (y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X) * 4;
        p[0] = (u8)(c.r >> 8), p[1] = (u8)(c.g >> 8), p[2] = (u8)(c.b >> 8), p[3] = (u8)(c.a >> 8);
    } else if (img.type == GO_TID_P_image_Alpha) {
        GoImageAlpha *m = img.data;
        if (x < m->Rect.Min.X || y < m->Rect.Min.Y || x >= m->Rect.Max.X || y >= m->Rect.Max.Y) return;
        m->Pix.p[(y - m->Rect.Min.Y) * m->Stride + (x - m->Rect.Min.X)] = (u8)(c.a >> 8);
    }
}

static GoImageRect image_bounds(GoIface img) {
    if (img.type == GO_TID_P_image_NRGBA) return ((GoImageNRGBA *)img.data)->Rect;
    if (img.type == GO_TID_P_image_RGBA) return ((GoImageRGBA *)img.data)->Rect;
    if (img.type == GO_TID_P_image_Alpha) return ((GoImageAlpha *)img.data)->Rect;
    if (img.type == GO_TID_P_image_Paletted) return ((GoImagePaletted *)img.data)->Rect;
    return image_Uniform_Bounds(NULL);
}

// draw.DrawMask (op 0 = Over, 1 = Src), per pixel as image/draw's generic path
void draw_DrawMask(GoIface dst, GoImageRect r, GoIface src, GoImagePoint sp, GoIface mask, GoImagePoint mp, int op) {
    // clip as draw.clip: r to dst, and to src/mask bounds translated
    r = image_Rectangle_Intersect(r, image_bounds(dst));
    GoImagePoint d0 = r.Min;
    GoImageRect sb = image_Rectangle_Add(image_bounds(src), (GoImagePoint){r.Min.X - sp.X, r.Min.Y - sp.Y});
    r = image_Rectangle_Intersect(r, sb);
    if (mask.type) {
        GoImageRect mb = image_Rectangle_Add(image_bounds(mask), (GoImagePoint){d0.X - mp.X, d0.Y - mp.Y});
        r = image_Rectangle_Intersect(r, mb);
    }
    if (image_Rectangle_Empty(r)) return;
    int dx = r.Min.X - d0.X, dy = r.Min.Y - d0.Y;
    sp.X += dx, sp.Y += dy, mp.X += dx, mp.Y += dy;
    for (int y = r.Min.Y; y < r.Max.Y; y++) {
        for (int x = r.Min.X; x < r.Max.X; x++) {
            int ox = x - r.Min.X, oy = y - r.Min.Y;
            C16 s = image_at(src, sp.X + ox, sp.Y + oy);
            u32 ma = 0xffff;
            if (mask.type) ma = image_at(mask, mp.X + ox, mp.Y + oy).a;
            C16 out;
            if (op == 0) {  // Over
                if (ma == 0) continue;
                C16 d = image_at(dst, x, y);
                u32 a = 0xffff - (s.a * ma / 0xffff);
                out.r = (d.r * a + s.r * ma) / 0xffff, out.g = (d.g * a + s.g * ma) / 0xffff;
                out.b = (d.b * a + s.b * ma) / 0xffff, out.a = (d.a * a + s.a * ma) / 0xffff;
            } else {  // Src
                out.r = s.r * ma / 0xffff, out.g = s.g * ma / 0xffff, out.b = s.b * ma / 0xffff, out.a = s.a * ma / 0xffff;
            }
            image_set(dst, x, y, out);
        }
    }
}

void draw_Draw(GoIface dst, GoImageRect r, GoIface src, GoImagePoint sp, int op) {
    // fast path: same-format copies (decoded images into NRGBA/RGBA buffers)
    if (op == 1 && dst.type == src.type &&
        (dst.type == GO_TID_P_image_NRGBA || dst.type == GO_TID_P_image_RGBA)) {
        GoImageNRGBA *d = dst.data, *s = src.data;  // same layout
        GoImageRect rr = image_Rectangle_Intersect(r, d->Rect);
        GoImagePoint d0 = r.Min;
        rr = image_Rectangle_Intersect(rr, image_Rectangle_Add(s->Rect, (GoImagePoint){d0.X - sp.X, d0.Y - sp.Y}));
        if (image_Rectangle_Empty(rr)) return;
        int sx = sp.X + rr.Min.X - d0.X, sy = sp.Y + rr.Min.Y - d0.Y, w = (rr.Max.X - rr.Min.X) * 4;
        for (int y = rr.Min.Y; y < rr.Max.Y; y++) {
            memmove(d->Pix.p + (y - d->Rect.Min.Y) * d->Stride + (rr.Min.X - d->Rect.Min.X) * 4,
                    s->Pix.p + (sy + y - rr.Min.Y - s->Rect.Min.Y) * s->Stride + (sx - s->Rect.Min.X) * 4, (size_t)w);
        }
        return;
    }
    draw_DrawMask(dst, r, src, sp, (GoIface){0}, (GoImagePoint){0, 0}, op);
}

// ---------------------------------------------------------------- PNG

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

typedef struct {
    u8 *p;
    int len, cap;
} Buf;
static void buf_add(Buf *b, const u8 *p, int n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = realloc(b->p, (size_t)b->cap);
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// unfilters one (sub)image of w x h, rowbytes per line, bpp filter stride
static int unfilter(u8 *data, int avail, int w, int h, int bits_pp, u8 *out) {
    int rowbytes = (w * bits_pp + 7) / 8, bpp = bits_pp >= 8 ? bits_pp / 8 : 1;
    if (w == 0 || h == 0) return 0;
    if ((rowbytes + 1) * h > avail) return -1;
    for (int y = 0; y < h; y++) {
        int ft = data[y * (rowbytes + 1)];
        const u8 *in = data + y * (rowbytes + 1) + 1;
        u8 *row = out + y * rowbytes, *prev = y ? out + (y - 1) * rowbytes : NULL;
        for (int i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = in[i];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: return -1;
            }
            row[i] = (u8)v;
        }
    }
    return (rowbytes + 1) * h;
}

static int sample(const u8 *row, int x, int depth) {  // x-th sample of a row
    switch (depth) {
    case 1: return (row[x >> 3] >> (7 - (x & 7))) & 1;
    case 2: return (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
    case 4: return (row[x >> 1] >> (4 - 4 * (x & 1))) & 15;
    case 8: return row[x];
    default: return row[2 * x] << 8 | row[2 * x + 1];
    }
}

static GoIface png_err(const char *m) { return go_errorf("png: %s", m); }

static Ret_GoIface__GoIface png_decode_bytes(const u8 *d, int n) {
    Ret_GoIface__GoIface res = {{0}, {0}};
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 8 || memcmp(d, sig, 8) != 0) return res.r1 = png_err("not a PNG file"), res;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0, npal = 0, has_trns = 0;
    u8 pal[256][4];
    u16 trns_key[3] = {0, 0, 0};
    Buf idat = {0};
    int pos = 8, seen_ihdr = 0;
    while (pos + 8 <= n) {
        u32 len = be32(d + pos);
        const u8 *type = d + pos + 4, *cd = d + pos + 8;
        if (len > (u32)(n - pos - 8)) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)be32(cd), h = (int)be32(cd + 4), depth = cd[8], ctype = cd[9], interlace = cd[12];
            seen_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = (int)len / 3;
            if (npal > 256) npal = 256;
            for (int i = 0; i < npal; i++) pal[i][0] = cd[3 * i], pal[i][1] = cd[3 * i + 1], pal[i][2] = cd[3 * i + 2], pal[i][3] = 255;
        } else if (!memcmp(type, "tRNS", 4)) {
            has_trns = 1;
            if (ctype == 3) {
                for (u32 i = 0; i < len && i < 256; i++) pal[i][3] = cd[i];
            } else if (ctype == 0 && len >= 2) {
                trns_key[0] = (u16)(cd[0] << 8 | cd[1]);
            } else if (ctype == 2 && len >= 6) {
                for (int i = 0; i < 3; i++) trns_key[i] = (u16)(cd[2 * i] << 8 | cd[2 * i + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            buf_add(&idat, cd, (int)len);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (int)len;
    }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!seen_ihdr || !ch || w <= 0 || h <= 0 || w > 32768 || h > 32768 || !idat.len ||
        !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) {
        free(idat.p);
        return res.r1 = png_err("invalid format"), res;
    }
    int raw_len = 0;
    u8 *raw = (u8 *)stbi_zlib_decode_malloc_guesssize_headerflag((const char *)idat.p, idat.len, w * h * ch + h, &raw_len, 1);
    free(idat.p);
    if (!raw) return res.r1 = png_err("invalid zlib data"), res;

    int bits_pp = ch * depth;
    GoImagePaletted *pm = NULL;
    GoImageNRGBA *nm = NULL;
    if (ctype == 3) {
        pm = go_new(GoImagePaletted);
        pm->Rect = (GoImageRect){{0, 0}, {w, h}}, pm->Stride = w, pm->Pix = pix_alloc(w * h);
        pm->Palette = (Slice_GoIface){(GoIface *)gc_calloc((size_t)(npal ? npal : 1), sizeof(GoIface)), npal, npal};
        for (int i = 0; i < npal; i++) pm->Palette.p[i] = box_nrgba(pal[i][0], pal[i][1], pal[i][2], pal[i][3]);
    } else {
        nm = image_NewNRGBA((GoImageRect){{0, 0}, {w, h}});
    }
    static const int ax[7] = {0, 4, 0, 2, 0, 1, 0}, ay[7] = {0, 0, 4, 0, 2, 0, 1}, sx[7] = {8, 8, 4, 4, 2, 2, 1},
                     sy[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = interlace ? 7 : 1, off = 0;
    u8 *rows = malloc((size_t)((w * bits_pp + 7) / 8) * (size_t)h + 1);
    for (int ps = 0; ps < passes; ps++) {
        int x0 = interlace ? ax[ps] : 0, y0 = interlace ? ay[ps] : 0, dx = interlace ? sx[ps] : 1, dy = interlace ? sy[ps] : 1;
        int pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        int used = unfilter(raw + off, raw_len - off, pw, ph, bits_pp, rows);
        if (used < 0) {
            free(rows), free(raw);
            return res.r1 = png_err("invalid filter"), res;
        }
        off += used;
        int rowbytes = (pw * bits_pp + 7) / 8;
        for (int y = 0; y < ph; y++) {
            const u8 *row = rows + y * rowbytes;
            int oy = y0 + y * dy;
            for (int x = 0; x < pw; x++) {
                int ox = x0 + x * dx;
                if (pm) {
                    pm->Pix.p[oy * w + ox] = (u8)sample(row, x, depth);
                    continue;
                }
                u8 *o = nm->Pix.p + oy * nm->Stride + ox * 4;
                int s0 = sample(row, x * ch, depth), maxv = (1 << depth) - 1;
#define TO8(v) (u8)(depth == 16 ? (v) >> 8 : (v) * 255 / maxv)
                if (ctype == 0) {
                    o[0] = o[1] = o[2] = TO8(s0), o[3] = (has_trns && s0 == trns_key[0]) ? 0 : 255;
                } else if (ctype == 4) {
                    o[0] = o[1] = o[2] = TO8(s0), o[3] = TO8(sample(row, x * ch + 1, depth));
                } else {
                    int s1 = sample(row, x * ch + 1, depth), s2 = sample(row, x * ch + 2, depth);
                    o[0] = TO8(s0), o[1] = TO8(s1), o[2] = TO8(s2);
                    if (ctype == 6) o[3] = TO8(sample(row, x * ch + 3, depth));
                    else o[3] = (has_trns && s0 == trns_key[0] && s1 == trns_key[1] && s2 == trns_key[2]) ? 0 : 255;
                }
#undef TO8
            }
        }
    }
    free(rows), free(raw);
    res.r0 = pm ? (GoIface){GO_TID_P_image_Paletted, pm} : (GoIface){GO_TID_P_image_NRGBA, nm};
    return res;
}

// reads one PNG from r chunk by chunk, consuming exactly its bytes (SFF
// files hold PNG sprites back to back)
static Ret_GoIface__GoIface png_read(GoIface r) {
    Buf b = {0};
    u8 hdr[8];
    Ret_GoIface__GoIface res = {{0}, {0}};
    Ret_int__GoIface rr = io_ReadFull(r, (Slice_u8){hdr, 8, 8});
    if (rr.r1.type) return res.r1 = rr.r1, res;
    buf_add(&b, hdr, 8);
    for (;;) {
        rr = io_ReadFull(r, (Slice_u8){hdr, 8, 8});
        if (rr.r1.type) break;
        buf_add(&b, hdr, 8);
        u32 len = be32(hdr);
        if (len > 0x7fffffff - 4) break;
        if (b.len + (int)len + 4 > b.cap) {
            b.cap = (b.len + (int)len + 4) * 2;
            b.p = realloc(b.p, (size_t)b.cap);
        }
        rr = io_ReadFull(r, (Slice_u8){b.p + b.len, (int)len + 4, (int)len + 4});
        if (rr.r1.type) break;
        b.len += (int)len + 4;
        if (!memcmp(hdr + 4, "IEND", 4)) break;
    }
    res = png_decode_bytes(b.p, b.len);
    free(b.p);
    return res;
}

Ret_GoIface__GoIface png_Decode(GoIface r) { return png_read(r); }

GoIface png_Encode(GoIface w, GoIface m) {
    (void)w, (void)m;
    return go_error(GOSTR("png: encoding is not supported by the native engine"));
}

// image.Decode: PNG natively; JPEG, BMP, TGA and GIF through stb_image
Ret_GoIface__GoString__GoIface image_Decode(GoIface r) {
    Ret_GoIface__GoString__GoIface res = {{0}, {0}, {0}};
    Ret_Slice_u8__GoIface all = io_ReadAll(r);
    if (all.r1.type) return res.r2 = all.r1, res;
    if (all.r0.len >= 8 && !memcmp(all.r0.p, "\x89PNG", 4)) {
        Ret_GoIface__GoIface p = png_decode_bytes(all.r0.p, all.r0.len);
        res.r0 = p.r0, res.r1 = GOSTR("png"), res.r2 = p.r1;
        return res;
    }
    int w, h, n;
    u8 *px = stbi_load_from_memory(all.r0.p, all.r0.len, &w, &h, &n, 4);
    if (!px) return res.r2 = go_error(GOSTR("image: unknown format")), res;
    GoImageNRGBA *m = image_NewNRGBA((GoImageRect){{0, 0}, {w, h}});
    memcpy(m->Pix.p, px, (size_t)w * (size_t)h * 4);
    stbi_image_free(px);
    res.r0 = (GoIface){GO_TID_P_image_NRGBA, m};
    res.r1 = all.r0.len >= 2 && all.r0.p[0] == 0xff && all.r0.p[1] == 0xd8 ? GOSTR("jpeg") : GOSTR("image");
    return res;
}

void image_RegisterFormat(GoString name, GoString magic, GoFunc dec, GoFunc cfg) {
    (void)name, (void)magic, (void)dec, (void)cfg;  // decoders are built in (image_Decode)
}
