"""Host OpenGL backend for vemu (enabled with VEMU_GL=1).

Replays the game's vitaGL (GLES2-style) calls on the host's Mesa through a
headless EGL context, so the Vita build's frames can be saved as PNGs.

  VEMU_SHOTS=<dir>          save <dir>/frame_NNNNN.png at vglSwapBuffers
  VEMU_SHOT_EVERY=<n>       every n-th frame (default 60; 0 = off)
  VEMU_SHOT_FRAMES=a,b,c    also these specific frames
  VEMU_GL_API=gles          force the GLES2/3 fallback context
  VEMU_GL_TRACE=1           print every GL call (very verbose)
  VEMU_GL_STEP=<n>[-<m>]    debug: for frames n..m, log every draw (blend, textures,
                            first vertices, sprite uniforms) and save the bound render
                            target after it (<shots>/step_<frame>_<k>.png)
  VEMU_GL_STEP_PNG=0        ...log only, no per-draw PNGs

The guest is ARM EABI hard-float: float arguments arrive in s0, s1, ...
independently of the integer arguments (r0-r3, then the stack).
Object names (textures, buffers, programs, uniform locations...) are the
host's own names, passed through unchanged; only framebuffer 0 is mapped to a
host FBO that stands in for the Vita's 960x544 display.
"""
import os, ctypes, struct
from ctypes import c_int, c_uint, c_float, c_double, c_void_p, c_char_p, c_ubyte, c_ssize_t, c_size_t

PASS = object()  # "not handled here": vemu.py's stub behaviour applies

GL_TEXTURE_2D = 0x0DE1
GL_ARRAY_BUFFER = 0x8892
GL_ELEMENT_ARRAY_BUFFER = 0x8893
GL_PIXEL_PACK_BUFFER = 0x88EB
GL_PIXEL_UNPACK_BUFFER = 0x88EC
GL_FRAMEBUFFER = 0x8D40
GL_READ_FRAMEBUFFER = 0x8CA8
GL_DRAW_FRAMEBUFFER = 0x8CA9
GL_RENDERBUFFER = 0x8D41
GL_COLOR_ATTACHMENT0 = 0x8CE0
GL_DEPTH_STENCIL_ATTACHMENT = 0x821A
GL_RGBA = 0x1908
GL_UNSIGNED_BYTE = 0x1401
GL_STREAM_DRAW = 0x88E0
GL_FRAGMENT_SHADER = 0x8B30
GL_TEXTURE_MAX_LEVEL = 0x813D
GL_UNPACK_ROW_LENGTH = 0x0CF2
GL_UNPACK_ALIGNMENT = 0x0CF5
GL_PACK_ALIGNMENT = 0x0D05
GL_BACK = 0x0405
GL_FRONT = 0x0404

ERRNAMES = {0x500: 'INVALID_ENUM', 0x501: 'INVALID_VALUE', 0x502: 'INVALID_OPERATION', 0x503: 'STACK_OVERFLOW',
            0x504: 'STACK_UNDERFLOW', 0x505: 'OUT_OF_MEMORY', 0x506: 'INVALID_FRAMEBUFFER_OPERATION'}

# components per pixel for a format, bytes per component (or per pixel when packed) for a type
FMT_COMP = {0x1909: 1, 0x1906: 1, 0x190A: 2, 0x1907: 3, 0x1908: 4, 0x1903: 1, 0x8227: 2, 0x80E1: 4, 0x80E0: 3,
            0x1902: 1, 0x84F9: 1, 0x8D94: 1, 0x8228: 2, 0x8D98: 3, 0x8D99: 4}
TYPE_SIZE = {0x1400: 1, 0x1401: 1, 0x1402: 2, 0x1403: 2, 0x1404: 4, 0x1405: 4, 0x1406: 4, 0x140B: 2, 0x8D61: 2}
TYPE_PACKED = {0x8363: 2, 0x8033: 2, 0x8034: 2, 0x8366: 2, 0x8365: 2, 0x84FA: 4, 0x8368: 4, 0x8367: 4, 0x8035: 4, 0x8036: 4}
ATTR_TYPE_SIZE = {0x1400: 1, 0x1401: 1, 0x1402: 2, 0x1403: 2, 0x1404: 4, 0x1405: 4, 0x1406: 4, 0x140B: 2, 0x140C: 4, 0x8D61: 2}
INDEX_SIZE = {0x1401: 1, 0x1403: 2, 0x1405: 4}

# Simple pass-through functions: argument kinds ('i' int, 'f' float, 'd' double)
# and return kind (None, 'i', 'b').
AUTO = {
    'glActiveTexture': ('i', None), 'glAttachShader': ('ii', None), 'glBindRenderbuffer': ('ii', None),
    'glBlendColor': ('ffff', None), 'glBlendEquation': ('i', None), 'glBlendEquationSeparate': ('ii', None),
    'glBlendFunc': ('ii', None), 'glBlendFuncSeparate': ('iiii', None), 'glClear': ('i', None),
    'glClearColor': ('ffff', None), 'glClearDepthf': ('f', None), 'glClearDepth': ('d', None),
    'glClearStencil': ('i', None), 'glColorMask': ('iiii', None), 'glCullFace': ('i', None),
    'glDeleteProgram': ('i', None), 'glDeleteShader': ('i', None), 'glDepthFunc': ('i', None),
    'glDepthMask': ('i', None), 'glDepthRangef': ('ff', None), 'glDepthRange': ('dd', None),
    'glDetachShader': ('ii', None), 'glDisable': ('i', None), 'glEnable': ('i', None), 'glFinish': ('', None),
    'glFlush': ('', None), 'glFramebufferRenderbuffer': ('iiii', None), 'glFramebufferTexture2D': ('iiiii', None),
    'glFramebufferTexture': ('iiii', None), 'glFrontFace': ('i', None), 'glHint': ('ii', None),
    'glLineWidth': ('f', None), 'glPolygonOffset': ('ff', None), 'glRenderbufferStorage': ('iiii', None),
    'glRenderbufferStorageMultisample': ('iiiii', None), 'glScissor': ('iiii', None),
    'glStencilFunc': ('iii', None), 'glStencilFuncSeparate': ('iiii', None), 'glStencilMask': ('i', None),
    'glStencilMaskSeparate': ('ii', None), 'glStencilOp': ('iii', None), 'glStencilOpSeparate': ('iiii', None),
    'glTexParameteri': ('iii', None), 'glTexParameterf': ('iif', None), 'glUseProgram': ('i', None),
    'glUniform1f': ('if', None), 'glUniform2f': ('iff', None), 'glUniform3f': ('ifff', None),
    'glUniform4f': ('iffff', None), 'glUniform1i': ('ii', None), 'glUniform2i': ('iii', None),
    'glUniform3i': ('iiii', None), 'glUniform4i': ('iiiii', None),
    'glVertexAttrib1f': ('if', None), 'glVertexAttrib2f': ('iff', None), 'glVertexAttrib3f': ('ifff', None),
    'glVertexAttrib4f': ('iffff', None), 'glViewport': ('iiii', None),
    'glCopyTexImage2D': ('iiiiiiii', None), 'glCopyTexSubImage2D': ('iiiiiiii', None),
    'glBlitFramebuffer': ('iiiiiiiiii', None), 'glVertexAttribDivisor': ('ii', None),
    'glCheckFramebufferStatus': ('i', 'i'), 'glCreateProgram': ('', 'i'), 'glIsEnabled': ('i', 'b'),
    'glIsTexture': ('i', 'b'), 'glIsProgram': ('i', 'b'), 'glIsFramebuffer': ('i', 'b'),
    'glIsRenderbuffer': ('i', 'b'), 'glIsBuffer': ('i', 'b'), 'glIsShader': ('i', 'b'),
    'glLinkProgram': ('i', None), 'glValidateProgram': ('i', None), 'glReleaseShaderCompiler': ('', None),
    'glSampleCoverage': ('fi', None), 'glPolygonMode': ('ii', None),
}
# Left to vemu's fake implementation on purpose: glGetString keeps reporting the
# Vita-like strings the game expects.
KEEP_FAKE = {'glGetString'}
CT = {'i': c_int, 'f': c_float, 'd': c_double}
RT = {None: None, 'i': c_uint, 'b': c_ubyte}


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


class HostGL:
    def __init__(self, mu, arg, cstr, say, frame_ref, W=960, H=544):
        from unicorn.arm_const import UC_ARM_REG_S0, UC_ARM_REG_D0
        self.S0, self.D0 = UC_ARM_REG_S0, UC_ARM_REG_D0
        self.mu, self.arg, self.cstr, self.say, self.FRAME = mu, arg, cstr, say, frame_ref
        self.W, self.H = W, H
        self.trace = os.environ.get('VEMU_GL_TRACE') == '1'
        self.shots = os.environ.get('VEMU_SHOTS', '')
        self.shot_every = int(os.environ.get('VEMU_SHOT_EVERY', '60') or 0)
        self.shot_frames = set(int(x) for x in os.environ.get('VEMU_SHOT_FRAMES', '').split(',') if x.strip())
        st = (os.environ.get('VEMU_GL_STEP', '') or '0').split('-')
        self.step_frame, self.step_last = int(st[0]), int(st[-1])
        self.step_png = os.environ.get('VEMU_GL_STEP_PNG', '1') != '0'
        self.step_n = 0
        self.last_client = {}
        self.vbo_shadow = {}
        if self.shots: os.makedirs(self.shots, exist_ok=True)
        self.fns = {}
        self.logged = set()
        self.unimpl = {}
        self.errs = {}
        self.gles = False
        self._init_egl()
        self._init_state()
        self.say('[hgl] host GL: %s | %s | %s (gles fallback: %s)' % (self.gl_string(0x1F00), self.gl_string(0x1F01), self.gl_string(0x1F02), self.gles))

    # ------------------------------------------------------------------ EGL
    def _init_egl(self):
        os.environ.setdefault('EGL_PLATFORM', 'surfaceless')
        egl = ctypes.CDLL('libEGL.so.1')
        self.egl = egl
        egl.eglGetProcAddress.restype = c_void_p
        egl.eglGetProcAddress.argtypes = [c_char_p]
        egl.eglInitialize.argtypes = [c_void_p, c_void_p, c_void_p]
        egl.eglChooseConfig.argtypes = [c_void_p, c_void_p, c_void_p, c_int, c_void_p]
        egl.eglCreateContext.restype = c_void_p
        egl.eglCreateContext.argtypes = [c_void_p, c_void_p, c_void_p, c_void_p]
        egl.eglMakeCurrent.argtypes = [c_void_p, c_void_p, c_void_p, c_void_p]
        egl.eglGetError.restype = c_int
        dpy = None
        gpd = egl.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
        if gpd:
            gpd = ctypes.CFUNCTYPE(c_void_p, c_uint, c_void_p, c_void_p)(gpd)
            dpy = gpd(0x31DD, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
        if not dpy:
            egl.eglGetDisplay.restype = c_void_p
            egl.eglGetDisplay.argtypes = [c_void_p]
            dpy = egl.eglGetDisplay(None)
        ma, mi = c_int(), c_int()
        if not egl.eglInitialize(c_void_p(dpy), ctypes.byref(ma), ctypes.byref(mi)):
            raise RuntimeError('eglInitialize failed 0x%x' % egl.eglGetError())
        self.dpy = dpy
        ctx = None
        want_gles = os.environ.get('VEMU_GL_API', '') == 'gles'
        if not want_gles:
            ctx = self._make_ctx(0x30A2, 0x0008, [0x30FB, 2, 0x30FC, 1])  # desktop GL 2.1+ (compat)
        if not ctx:
            self.gles = True
            ctx = self._make_ctx(0x30A0, 0x0040, [0x3098, 3])  # GLES3 (VAOs)
            if not ctx:
                ctx = self._make_ctx(0x30A0, 0x0004, [0x3098, 2])
        if not ctx:
            raise RuntimeError('could not create an EGL context (0x%x)' % egl.eglGetError())
        if not egl.eglMakeCurrent(c_void_p(dpy), None, None, c_void_p(ctx)):
            raise RuntimeError('eglMakeCurrent failed 0x%x' % egl.eglGetError())
        self.ctx = ctx

    def _make_ctx(self, api, rbit, cattrs):
        egl = self.egl
        if not egl.eglBindAPI(api): return None
        cfg = c_void_p(); n = c_int()
        attrs = (c_int * 3)(0x3040, rbit, 0x3038)
        if not egl.eglChooseConfig(c_void_p(self.dpy), attrs, ctypes.byref(cfg), 1, ctypes.byref(n)) or n.value < 1:
            cfg = c_void_p(None)  # EGL_KHR_no_config_context
        ca = (c_int * (len(cattrs) + 1))(*(cattrs + [0x3038]))
        ctx = egl.eglCreateContext(c_void_p(self.dpy), cfg, None, ca)
        return ctx or None

    def fn(self, name, restype, argtypes):
        key = (name, restype, tuple(argtypes))
        f = self.fns.get(key)
        if f is None:
            p = self.egl.eglGetProcAddress(name.encode())
            if not p: raise AttributeError(name)
            f = ctypes.CFUNCTYPE(restype, *argtypes)(p)
            self.fns[key] = f
        return f

    def gl_string(self, e):
        try:
            r = self.fn('glGetString', c_char_p, [c_uint])(e)
            return r.decode() if r else ''
        except Exception:
            return ''

    # ------------------------------------------------------------------ state
    def _init_state(self):
        g = self.fn
        self.glGetError = g('glGetError', c_uint, [])
        # stand-in for the Vita display (framebuffer 0)
        rb = (c_uint * 2)()
        g('glGenRenderbuffers', None, [c_int, c_void_p])(2, rb)
        self.fb0_color, self.fb0_ds = rb[0], rb[1]
        brb = g('glBindRenderbuffer', None, [c_uint, c_uint])
        rbs = g('glRenderbufferStorage', None, [c_uint, c_uint, c_int, c_int])
        brb(GL_RENDERBUFFER, self.fb0_color); rbs(GL_RENDERBUFFER, 0x8058, self.W, self.H)  # RGBA8
        brb(GL_RENDERBUFFER, self.fb0_ds); rbs(GL_RENDERBUFFER, 0x88F0, self.W, self.H)    # DEPTH24_STENCIL8
        brb(GL_RENDERBUFFER, 0)
        fb = (c_uint * 1)()
        g('glGenFramebuffers', None, [c_int, c_void_p])(1, fb)
        self.fb0 = fb[0]
        bfb = g('glBindFramebuffer', None, [c_uint, c_uint])
        bfb(GL_FRAMEBUFFER, self.fb0)
        fbr = g('glFramebufferRenderbuffer', None, [c_uint, c_uint, c_uint, c_uint])
        fbr(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, self.fb0_color)
        fbr(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, self.fb0_ds)
        st = g('glCheckFramebufferStatus', c_uint, [c_uint])(GL_FRAMEBUFFER)
        if st != 0x8CD5: self.say('[hgl] WARNING: display FBO incomplete 0x%x' % st)
        g('glDrawBuffers', None, [c_int, c_void_p])(1, (c_uint * 1)(GL_COLOR_ATTACHMENT0))
        g('glReadBuffer', None, [c_uint])(GL_COLOR_ATTACHMENT0)
        g('glViewport', None, [c_int] * 4)(0, 0, self.W, self.H)
        g('glScissor', None, [c_int] * 4)(0, 0, self.W, self.H)
        g('glClearColor', None, [c_float] * 4)(0, 0, 0, 1)
        g('glClear', None, [c_uint])(0x4500)
        g('glPixelStorei', None, [c_uint, c_int])(GL_UNPACK_ALIGNMENT, 1)  # vitaGL: rows are tightly packed
        g('glPixelStorei', None, [c_uint, c_int])(GL_PACK_ALIGNMENT, 1)
        sb = (c_uint * 2)()
        g('glGenBuffers', None, [c_int, c_void_p])(2, sb)
        self.stream_vbo, self.stream_ibo = sb[0], sb[1]
        self.fb_draw = 0; self.fb_read = 0   # game's names
        self.array_buffer = 0
        self.bufbind = {}                    # other buffer targets
        self.vao = 0
        self.vaos = {0: {'elem': 0, 'attr': {}}}
        self.tex_unit = 0
        self.tex_bound = {}
        self.tex_known = set()
        self.tex_size = {}
        self.program = 0
        self.unpack_row = 0
        self.shader_type = {}
        self.drain_errors()

    def vstate(self):
        v = self.vaos.get(self.vao)
        if v is None: v = self.vaos[self.vao] = {'elem': 0, 'attr': {}}
        return v

    def drain_errors(self, name=None):
        for _ in range(8):
            e = self.glGetError()
            if not e: return
            if name is None: continue
            k = (name, e)
            self.errs[k] = self.errs.get(k, 0) + 1
            if self.errs[k] == 1:
                self.say('[hgl] host GL error %s (0x%x) in %s (frame %d; logged once)' % (ERRNAMES.get(e, '?'), e, name, self.FRAME[0]))

    # ------------------------------------------------------------------ guest access
    def rd(self, addr, n):
        return bytes(self.mu.mem_read(addr, n)) if n > 0 else b''

    def fargs(self, kinds, a):
        """Collect C arguments following the hard-float AAPCS."""
        out = []; ii = 0; si = 0
        for k in kinds:
            if k == 'i':
                out.append(s32(a(ii))); ii += 1
            elif k == 'f':
                bits = self.mu.reg_read(self.S0 + si) & 0xffffffff; si += 1
                out.append(struct.unpack('<f', struct.pack('<I', bits))[0])
            elif k == 'd':
                si = (si + 1) & ~1
                bits = self.mu.reg_read(self.D0 + si // 2) & 0xffffffffffffffff; si += 2
                out.append(struct.unpack('<d', struct.pack('<Q', bits))[0])
        return out

    # ------------------------------------------------------------------ dispatch
    def call(self, name, a6):
        arg = self.arg
        h = getattr(self, 'h_' + name, None)
        if name in KEEP_FAKE: return PASS
        if h is None and name not in AUTO:
            if name not in self.unimpl:
                self.say('[hgl] not implemented on host (stubbed): %s' % name)
            self.unimpl[name] = self.unimpl.get(name, 0) + 1
            return PASS
        try:
            if h is not None:
                if self.trace: self.say('[hgl] %s(%s)' % (name, ', '.join(hex(arg(i)) for i in range(4))))
                r = h(arg)
            else:
                kinds, ret = AUTO[name]
                vals = self.fargs(kinds, arg)
                f = self.fn(name, RT[ret], [CT[k] for k in kinds])
                r = f(*vals)
                if ret is None: r = 0
                if self.trace: self.say('[hgl] %s%s -> %s' % (name, tuple(vals), r))
        except AttributeError as e:
            if name not in self.unimpl:
                self.say('[hgl] host has no %s (stubbed)' % e)
            self.unimpl[name] = self.unimpl.get(name, 0) + 1
            return PASS
        except Exception as e:
            k = (name, 'exc')
            if k not in self.errs:
                import traceback
                self.say('[hgl] exception in %s: %r (logged once)' % (name, e))
                traceback.print_exc()
            self.errs[k] = self.errs.get(k, 0) + 1
            r = 0
        if r is not PASS:
            self.drain_errors(name)
        return r

    # ------------------------------------------------------------------ objects
    def _gen(self, glname, arg):
        n, p = s32(arg(0)), arg(1)
        if n <= 0: return 0
        buf = (c_uint * n)()
        self.fn(glname, None, [c_int, c_void_p])(n, buf)
        self.mu.mem_write(p, struct.pack('<%dI' % n, *buf))
        if self.trace: self.say('[hgl] %s -> %s' % (glname, list(buf)))
        return 0

    def h_glGenTextures(self, arg): return self._gen('glGenTextures', arg)
    def h_glGenBuffers(self, arg): return self._gen('glGenBuffers', arg)
    def h_glGenFramebuffers(self, arg): return self._gen('glGenFramebuffers', arg)
    def h_glGenRenderbuffers(self, arg): return self._gen('glGenRenderbuffers', arg)
    def h_glGenVertexArrays(self, arg): return self._gen('glGenVertexArrays', arg)

    def _del(self, glname, arg):
        n, p = s32(arg(0)), arg(1)
        if n <= 0 or not p: return 0
        ids = list(struct.unpack('<%dI' % n, self.rd(p, 4 * n)))
        if glname == 'glDeleteFramebuffers':
            ids = [i for i in ids if i != self.fb0]
            if self.fb_draw in ids: self.fb_draw = 0
            if self.fb_read in ids: self.fb_read = 0
        elif glname == 'glDeleteBuffers':
            if self.array_buffer in ids: self.array_buffer = 0
            for v in self.vaos.values():
                if v['elem'] in ids: v['elem'] = 0
            ids = [i for i in ids if i not in (self.stream_vbo, self.stream_ibo)]
        elif glname == 'glDeleteVertexArrays':
            for i in ids:
                if i: self.vaos.pop(i, None)
            if self.vao in ids: self.vao = 0
        elif glname == 'glDeleteTextures':
            for i in ids: self.tex_known.discard(i)
        elif glname == 'glDeleteRenderbuffers':
            ids = [i for i in ids if i not in (self.fb0_color, self.fb0_ds)]
        if ids:
            self.fn(glname, None, [c_int, c_void_p])(len(ids), (c_uint * len(ids))(*ids))
        return 0

    def h_glDeleteTextures(self, arg): return self._del('glDeleteTextures', arg)
    def h_glDeleteBuffers(self, arg): return self._del('glDeleteBuffers', arg)
    def h_glDeleteFramebuffers(self, arg): return self._del('glDeleteFramebuffers', arg)
    def h_glDeleteRenderbuffers(self, arg): return self._del('glDeleteRenderbuffers', arg)
    def h_glDeleteVertexArrays(self, arg): return self._del('glDeleteVertexArrays', arg)

    # ------------------------------------------------------------------ binding
    def h_glBindFramebuffer(self, arg):
        target, fb = arg(0), arg(1)
        host = self.fb0 if fb == 0 else fb
        self.fn('glBindFramebuffer', None, [c_uint, c_uint])(target, host)
        if target in (GL_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER): self.fb_draw = fb
        if target in (GL_FRAMEBUFFER, GL_READ_FRAMEBUFFER): self.fb_read = fb
        return 0

    def h_glBindBuffer(self, arg):
        target, b = arg(0), arg(1)
        self.fn('glBindBuffer', None, [c_uint, c_uint])(target, b)
        if target == GL_ARRAY_BUFFER: self.array_buffer = b
        elif target == GL_ELEMENT_ARRAY_BUFFER: self.vstate()['elem'] = b
        else: self.bufbind[target] = b
        return 0

    def h_glBindVertexArray(self, arg):
        v = arg(0)
        self.fn('glBindVertexArray', None, [c_uint])(v)
        self.vao = v; self.vstate()
        return 0

    def h_glBindTexture(self, arg):
        target, t = arg(0), arg(1)
        self.fn('glBindTexture', None, [c_uint, c_uint])(target, t)
        if target == GL_TEXTURE_2D:
            self.tex_bound[self.tex_unit] = t
            if t and t not in self.tex_known:
                # vitaGL/GXM samples level 0 of a texture without mipmaps whatever
                # the min filter; desktop GL would treat it as incomplete (black).
                self.tex_known.add(t)
                self.fn('glTexParameteri', None, [c_uint, c_uint, c_int])(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0)
        return 0

    def h_glActiveTexture(self, arg):
        self.tex_unit = arg(0) - 0x84C0
        self.fn('glActiveTexture', None, [c_uint])(arg(0))
        return 0

    def h_glGenerateMipmap(self, arg):
        target = arg(0)
        if target == GL_TEXTURE_2D:
            self.fn('glTexParameteri', None, [c_uint, c_uint, c_int])(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000)
        self.fn('glGenerateMipmap', None, [c_uint])(target)
        return 0

    def h_glDrawBuffers(self, arg):
        n, p = s32(arg(0)), arg(1)
        if n <= 0: return 0
        bufs = list(struct.unpack('<%dI' % n, self.rd(p, 4 * n)))
        if self.fb_draw == 0:
            bufs = [GL_COLOR_ATTACHMENT0 if b in (GL_BACK, GL_FRONT) else b for b in bufs]
        self.fn('glDrawBuffers', None, [c_int, c_void_p])(n, (c_uint * n)(*bufs))
        return 0

    def h_glReadBuffer(self, arg):
        b = arg(0)
        if self.fb_read == 0 and b in (GL_BACK, GL_FRONT): b = GL_COLOR_ATTACHMENT0
        self.fn('glReadBuffer', None, [c_uint])(b)
        return 0

    # ------------------------------------------------------------------ shaders
    def h_glCreateShader(self, arg):
        t = arg(0)
        s = self.fn('glCreateShader', c_uint, [c_uint])(t)
        self.shader_type[s] = t
        return s

    def h_glShaderSource(self, arg):
        sh, count, strs, lens = arg(0), s32(arg(1)), arg(2), arg(3)
        parts = []
        for i in range(max(0, count)):
            sp = struct.unpack('<I', self.rd(strs + 4 * i, 4))[0]
            ln = s32(struct.unpack('<I', self.rd(lens + 4 * i, 4))[0]) if lens else -1
            if ln < 0: parts.append(self.cstr(sp, 1 << 20).encode())
            else: parts.append(self.rd(sp, ln))
        src = b''.join(parts)
        if self.gles:
            pre = b'precision mediump float;\n' if self.shader_type.get(sh) == GL_FRAGMENT_SHADER else b''
            src = b'#version 100\n' + pre + src
        buf = ctypes.c_char_p(src)
        self.fn('glShaderSource', None, [c_uint, c_int, c_void_p, c_void_p])(sh, 1, ctypes.byref(buf), (c_int * 1)(len(src)))
        return 0

    def _infolog(self, getiv, getlog, obj):
        n = c_int()
        self.fn(getiv, None, [c_uint, c_uint, c_void_p])(obj, 0x8B84, ctypes.byref(n))
        if n.value <= 1: return ''
        b = ctypes.create_string_buffer(n.value)
        self.fn(getlog, None, [c_uint, c_int, c_void_p, c_void_p])(obj, n.value, None, b)
        return b.value.decode('utf-8', 'replace')

    def h_glCompileShader(self, arg):
        sh = arg(0)
        self.fn('glCompileShader', None, [c_uint])(sh)
        ok = c_int()
        self.fn('glGetShaderiv', None, [c_uint, c_uint, c_void_p])(sh, 0x8B81, ctypes.byref(ok))
        if not ok.value:
            self.say('[hgl] shader %d compile FAILED: %s' % (sh, self._infolog('glGetShaderiv', 'glGetShaderInfoLog', sh)))
        return 0

    def h_glLinkProgram(self, arg):
        p = arg(0)
        self.fn('glLinkProgram', None, [c_uint])(p)
        ok = c_int()
        self.fn('glGetProgramiv', None, [c_uint, c_uint, c_void_p])(p, 0x8B82, ctypes.byref(ok))
        if not ok.value:
            self.say('[hgl] program %d link FAILED: %s' % (p, self._infolog('glGetProgramiv', 'glGetProgramInfoLog', p)))
        return 0

    def h_glGetShaderiv(self, arg):
        v = c_int()
        self.fn('glGetShaderiv', None, [c_uint, c_uint, c_void_p])(arg(0), arg(1), ctypes.byref(v))
        if arg(2): self.mu.mem_write(arg(2), struct.pack('<i', v.value))
        return 0

    def h_glGetProgramiv(self, arg):
        v = c_int()
        self.fn('glGetProgramiv', None, [c_uint, c_uint, c_void_p])(arg(0), arg(1), ctypes.byref(v))
        if arg(2): self.mu.mem_write(arg(2), struct.pack('<i', v.value))
        return 0

    def _getlog(self, glname, arg):
        obj, size, plen, buf = arg(0), s32(arg(1)), arg(2), arg(3)
        n = c_int()
        b = ctypes.create_string_buffer(max(1, size))
        self.fn(glname, None, [c_uint, c_int, c_void_p, c_void_p])(obj, max(0, size), ctypes.byref(n), b)
        if plen: self.mu.mem_write(plen, struct.pack('<i', n.value))
        if buf and size > 0: self.mu.mem_write(buf, b.raw[:min(size, n.value + 1)])
        return 0

    def h_glGetShaderInfoLog(self, arg): return self._getlog('glGetShaderInfoLog', arg)
    def h_glGetProgramInfoLog(self, arg): return self._getlog('glGetProgramInfoLog', arg)

    def h_glGetUniformLocation(self, arg):
        return self.fn('glGetUniformLocation', c_int, [c_uint, c_char_p])(arg(0), self.cstr(arg(1)).encode()) & 0xffffffff

    def h_glGetAttribLocation(self, arg):
        return self.fn('glGetAttribLocation', c_int, [c_uint, c_char_p])(arg(0), self.cstr(arg(1)).encode()) & 0xffffffff

    def h_glBindAttribLocation(self, arg):
        self.fn('glBindAttribLocation', None, [c_uint, c_uint, c_char_p])(arg(0), arg(1), self.cstr(arg(2)).encode())
        return 0

    # ------------------------------------------------------------------ uniforms
    def _uv(self, glname, ncomp, ctype, arg):
        loc, count, p = s32(arg(0)), s32(arg(1)), arg(2)
        if count <= 0 or not p: return 0
        n = count * ncomp
        arr = (ctype * n).from_buffer_copy(self.rd(p, 4 * n))
        self.fn(glname, None, [c_int, c_int, c_void_p])(loc, count, arr)
        return 0

    def h_glUniform1fv(self, arg): return self._uv('glUniform1fv', 1, c_float, arg)
    def h_glUniform2fv(self, arg): return self._uv('glUniform2fv', 2, c_float, arg)
    def h_glUniform3fv(self, arg): return self._uv('glUniform3fv', 3, c_float, arg)
    def h_glUniform4fv(self, arg): return self._uv('glUniform4fv', 4, c_float, arg)
    def h_glUniform1iv(self, arg): return self._uv('glUniform1iv', 1, c_int, arg)
    def h_glUniform2iv(self, arg): return self._uv('glUniform2iv', 2, c_int, arg)
    def h_glUniform3iv(self, arg): return self._uv('glUniform3iv', 3, c_int, arg)
    def h_glUniform4iv(self, arg): return self._uv('glUniform4iv', 4, c_int, arg)

    def _um(self, glname, nn, arg):
        loc, count, tr, p = s32(arg(0)), s32(arg(1)), arg(2) & 0xff, arg(3)
        if count <= 0 or not p: return 0
        n = count * nn
        arr = (c_float * n).from_buffer_copy(self.rd(p, 4 * n))
        self.fn(glname, None, [c_int, c_int, c_ubyte, c_void_p])(loc, count, tr, arr)
        return 0

    def h_glUniformMatrix2fv(self, arg): return self._um('glUniformMatrix2fv', 4, arg)
    def h_glUniformMatrix3fv(self, arg): return self._um('glUniformMatrix3fv', 9, arg)
    def h_glUniformMatrix4fv(self, arg): return self._um('glUniformMatrix4fv', 16, arg)

    def _va(self, glname, n, arg):
        idx, p = arg(0), arg(1)
        arr = (c_float * n).from_buffer_copy(self.rd(p, 4 * n))
        self.fn(glname, None, [c_uint, c_void_p])(idx, arr)
        return 0

    def h_glVertexAttrib1fv(self, arg): return self._va('glVertexAttrib1fv', 1, arg)
    def h_glVertexAttrib2fv(self, arg): return self._va('glVertexAttrib2fv', 2, arg)
    def h_glVertexAttrib3fv(self, arg): return self._va('glVertexAttrib3fv', 3, arg)
    def h_glVertexAttrib4fv(self, arg): return self._va('glVertexAttrib4fv', 4, arg)

    # ------------------------------------------------------------------ buffers / vertex arrays
    def h_glBufferData(self, arg):
        target, size, data, usage = arg(0), s32(arg(1)), arg(2), arg(3)
        src = self.rd(data, size) if data and size > 0 else None
        self.fn('glBufferData', None, [c_uint, c_ssize_t, c_void_p, c_uint])(target, max(0, size), src, usage)
        return 0

    def h_glBufferSubData(self, arg):
        target, off, size, data = arg(0), s32(arg(1)), s32(arg(2)), arg(3)
        if size <= 0 or not data: return 0
        src = self.rd(data, size)
        self.fn('glBufferSubData', None, [c_uint, c_ssize_t, c_ssize_t, c_void_p])(target, off, size, src)
        if self.step_frame and target == GL_ARRAY_BUFFER: self.vbo_shadow[self.array_buffer] = src
        return 0

    def h_glVertexAttribPointer(self, arg):
        idx, size, typ, norm, stride, ptr = arg(0), s32(arg(1)), arg(2), arg(3) & 0xff, s32(arg(4)), arg(5)
        st = self.vstate()['attr'].setdefault(idx, {'en': False})
        if self.array_buffer:
            st.update(client=False)
            self.fn('glVertexAttribPointer', None, [c_uint, c_int, c_uint, c_ubyte, c_int, c_void_p])(idx, size, typ, norm, stride, ptr)
        else:
            # client-side array in guest memory: uploaded at draw time
            st.update(client=True, ptr=ptr, size=size, type=typ, norm=norm, stride=stride)
        return 0

    def h_glEnableVertexAttribArray(self, arg):
        self.vstate()['attr'].setdefault(arg(0), {'client': False})['en'] = True
        self.fn('glEnableVertexAttribArray', None, [c_uint])(arg(0))
        return 0

    def h_glDisableVertexAttribArray(self, arg):
        self.vstate()['attr'].setdefault(arg(0), {'client': False})['en'] = False
        self.fn('glDisableVertexAttribArray', None, [c_uint])(arg(0))
        return 0

    def _upload_client(self, first, count):
        """Copy enabled client-side attribute arrays (guest memory) into the stream VBO."""
        attrs = [(i, s) for i, s in self.vstate()['attr'].items() if s.get('en') and s.get('client')]
        if not attrs or count <= 0: return
        blob = bytearray(); offs = []
        for i, s in attrs:
            esz = ATTR_TYPE_SIZE.get(s['type'], 4) * (4 if s['size'] == 0x80E1 else s['size'])
            stride = s['stride'] or esz
            start = s['ptr'] + first * stride
            nbytes = (count - 1) * stride + esz
            pad = (-len(blob)) & 15
            blob += b'\0' * pad
            # the attribute pointer is relative to vertex 0, so offset by -first*stride
            offs.append((i, s, stride, len(blob) - first * stride))
            blob += self.rd(start, nbytes)
            if self.step_frame: self.last_client[i] = (s['type'], s['size'], stride, self.rd(s['ptr'], min(nbytes + first * stride, 4 * stride)))
        bb = self.fn('glBindBuffer', None, [c_uint, c_uint])
        bb(GL_ARRAY_BUFFER, self.stream_vbo)
        self.fn('glBufferData', None, [c_uint, c_ssize_t, c_void_p, c_uint])(GL_ARRAY_BUFFER, len(blob), bytes(blob), GL_STREAM_DRAW)
        vap = self.fn('glVertexAttribPointer', None, [c_uint, c_int, c_uint, c_ubyte, c_int, c_void_p])
        for i, s, stride, off in offs:
            if off < 0:
                # first > 0 and the offset would be negative: shift data instead
                self.say('[hgl] negative client offset (first=%d)' % first)
                off = 0
            vap(i, s['size'], s['type'], s['norm'], stride, off)
        bb(GL_ARRAY_BUFFER, self.array_buffer)

    def h_glDrawArrays(self, arg):
        mode, first, count = arg(0), s32(arg(1)), s32(arg(2))
        if count <= 0: return 0
        # first*stride offsets: upload from vertex 0 so the pointer offset stays >= 0
        self._upload_client(0, first + count)
        self.fn('glDrawArrays', None, [c_uint, c_int, c_int])(mode, first, count)
        if self.step_frame and self.step_frame <= self.FRAME[0] + 1 <= self.step_last: self._step('glDrawArrays %d verts' % count)
        return 0

    def h_glUseProgram(self, arg):
        self.program = arg(0)
        self.fn('glUseProgram', None, [c_uint])(arg(0))
        return 0

    def _step(self, what):
        """Debug: snapshot the current render target after a draw."""
        g = self.fn
        gi = g('glGetIntegerv', None, [c_uint, c_void_p])
        vp = (c_int * 4)(); gi(0x0BA2, vp)
        def q(e):
            v = c_int(); gi(e, ctypes.byref(v)); return v.value
        blend = g('glIsEnabled', c_ubyte, [c_uint])(0x0BE2)
        sc = g('glIsEnabled', c_ubyte, [c_uint])(0x0C11)
        t = self.tex_bound.get(0, 0)
        info = '%s prog %d fb %d vp %s blend %d (%x,%x eq %x) scissor %d tex0 %d %s tex1 %s' % (
            what, self.program, self.fb_draw, list(vp), blend, q(0x80C9), q(0x80C8), q(0x8009), sc,
            t, self.tex_size.get(t), self.tex_size.get(self.tex_bound.get(1, 0)))
        # vertex data (first 4 client vertices) and the sprite uniforms
        for i, (typ, sz, stride, data) in sorted(self.last_client.items()):
            if typ == 0x1406 and sz in (2, 3, 4):
                vs = [struct.unpack_from('<%df' % sz, data, k * stride) for k in range(len(data) // stride)]
                info += ' | attr%d %s' % (i, ' '.join('(%s)' % ','.join('%.4g' % x for x in v) for v in vs))
        if not self.last_client and self.array_buffer in self.vbo_shadow:
            v = self.vbo_shadow[self.array_buffer]
            n = min(len(v) // 16, 12)
            info += ' | vbo%d %s' % (self.array_buffer, ' '.join('(%.4g,%.4g)' % struct.unpack_from('<2f', v, 16 * k) for k in range(n)))
        self.last_client = {}
        gul = g('glGetUniformLocation', c_int, [c_uint, c_char_p])
        guf = g('glGetUniformfv', None, [c_uint, c_int, c_void_p])
        for un, n in ((b'modelview', 16), (b'spriteUV', 4), (b'projection', 16)):
            loc = gul(self.program, un) if self.program else -1
            if loc >= 0:
                fv = (c_float * 16)(); guf(self.program, loc, fv)
                v = list(fv[:n])
                if n == 16: v = [v[0], v[5], v[12], v[13]]  # sx, sy, tx, ty
                info += ' | %s %s' % (un.decode(), ','.join('%.4g' % x for x in v))
        if self.FRAME[0] + 1 != getattr(self, 'step_cur', None): self.step_cur = self.FRAME[0] + 1; self.step_n = 0
        self.step_n += 1
        name = 'step_%05d_%03d.png' % (self.step_cur, self.step_n)
        self.say('[hgl] step %s: %s' % (name, info))
        if not self.shots or not self.step_png: return
        W, H = vp[0] + vp[2], vp[1] + vp[3]
        g('glBindFramebuffer', None, [c_uint, c_uint])(GL_READ_FRAMEBUFFER, self.fb0 if self.fb_draw == 0 else self.fb_draw)
        buf = ctypes.create_string_buffer(W * H * 4)
        g('glReadPixels', None, [c_int, c_int, c_int, c_int, c_uint, c_uint, c_void_p])(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf)
        g('glBindFramebuffer', None, [c_uint, c_uint])(GL_READ_FRAMEBUFFER, self.fb0 if self.fb_read == 0 else self.fb_read)
        self.drain_errors('<step>')
        import numpy as np
        from PIL import Image
        a = np.frombuffer(buf.raw, dtype=np.uint8).reshape(H, W, 4)[::-1, :, :3]
        Image.fromarray(np.ascontiguousarray(a), 'RGB').save(os.path.join(self.shots, name))

    def h_glDrawArraysInstanced(self, arg):
        mode, first, count, inst = arg(0), s32(arg(1)), s32(arg(2)), s32(arg(3))
        if count <= 0: return 0
        self._upload_client(0, first + count)
        self.fn('glDrawArraysInstanced', None, [c_uint, c_int, c_int, c_int])(mode, first, count, inst)
        return 0

    def h_glDrawElements(self, arg):
        mode, count, typ, ind = arg(0), s32(arg(1)), arg(2), arg(3)
        if count <= 0: return 0
        isz = INDEX_SIZE.get(typ, 2)
        elem = self.vstate()['elem']
        fmt = {1: 'B', 2: 'H', 4: 'I'}[isz]
        if elem:
            need_client = any(s.get('en') and s.get('client') for s in self.vstate()['attr'].values())
            if need_client:
                buf = ctypes.create_string_buffer(count * isz)
                self.fn('glGetBufferSubData', None, [c_uint, c_ssize_t, c_ssize_t, c_void_p])(GL_ELEMENT_ARRAY_BUFFER, ind, count * isz, buf)
                mx = max(struct.unpack('<%d%s' % (count, fmt), buf.raw))
                self._upload_client(0, mx + 1)
            self.fn('glDrawElements', None, [c_uint, c_int, c_uint, c_void_p])(mode, count, typ, ind)
        else:
            data = self.rd(ind, count * isz)
            mx = max(struct.unpack('<%d%s' % (count, fmt), data))
            self._upload_client(0, mx + 1)
            self.fn('glBindBuffer', None, [c_uint, c_uint])(GL_ELEMENT_ARRAY_BUFFER, self.stream_ibo)
            self.fn('glBufferData', None, [c_uint, c_ssize_t, c_void_p, c_uint])(GL_ELEMENT_ARRAY_BUFFER, len(data), data, GL_STREAM_DRAW)
            self.fn('glDrawElements', None, [c_uint, c_int, c_uint, c_void_p])(mode, count, typ, None)
            self.fn('glBindBuffer', None, [c_uint, c_uint])(GL_ELEMENT_ARRAY_BUFFER, 0)
        return 0

    # ------------------------------------------------------------------ textures / pixels
    def _pix_size(self, w, h, fmt, typ, row_len=0):
        if typ in TYPE_PACKED: bpp = TYPE_PACKED[typ]
        else: bpp = FMT_COMP.get(fmt, 4) * TYPE_SIZE.get(typ, 1)
        if w <= 0 or h <= 0: return 0, bpp
        rw = row_len if row_len > 0 else w
        return (h - 1) * rw * bpp + w * bpp, bpp

    def h_glPixelStorei(self, arg):
        pname, v = arg(0), s32(arg(1))
        if pname == GL_UNPACK_ROW_LENGTH:  # the only one vitaGL accepts
            self.unpack_row = v
            self.fn('glPixelStorei', None, [c_uint, c_int])(pname, v)
        return 0

    def _unpack_src(self, ptr, nbytes):
        if self.bufbind.get(GL_PIXEL_UNPACK_BUFFER): return ptr  # offset into a PBO
        if not ptr or nbytes <= 0: return None
        return self.rd(ptr, nbytes)

    def h_glTexImage2D(self, arg):
        target, level, ifmt, w, h, border, fmt, typ, px = [arg(i) for i in range(9)]
        w, h, level = s32(w), s32(h), s32(level)
        nbytes, _ = self._pix_size(w, h, fmt, typ, self.unpack_row)
        if level > 0 and target == GL_TEXTURE_2D:
            self.fn('glTexParameteri', None, [c_uint, c_uint, c_int])(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000)
        if level == 0 and target == GL_TEXTURE_2D: self.tex_size[self.tex_bound.get(self.tex_unit, 0)] = (w, h, fmt)
        if self.trace: self.say('[hgl] glTexImage2D lvl %d ifmt %x %dx%d fmt %x type %x px %x' % (level, ifmt, w, h, fmt, typ, px))
        self.fn('glTexImage2D', None, [c_uint, c_int, c_int, c_int, c_int, c_int, c_uint, c_uint, c_void_p])(
            target, level, s32(ifmt), w, h, s32(border), fmt, typ, self._unpack_src(px, nbytes))
        return 0

    def h_glTexSubImage2D(self, arg):
        target, level, x, y, w, h, fmt, typ, px = [arg(i) for i in range(9)]
        w, h = s32(w), s32(h)
        nbytes, _ = self._pix_size(w, h, fmt, typ, self.unpack_row)
        src = self._unpack_src(px, nbytes)
        if src is None: return 0
        self.fn('glTexSubImage2D', None, [c_uint, c_int, c_int, c_int, c_int, c_int, c_uint, c_uint, c_void_p])(
            target, s32(level), s32(x), s32(y), w, h, fmt, typ, src)
        return 0

    def h_glTexParameteriv(self, arg):
        v = (c_int * 4).from_buffer_copy(self.rd(arg(2), 16))
        self.fn('glTexParameteriv', None, [c_uint, c_uint, c_void_p])(arg(0), arg(1), v)
        return 0

    def h_glReadPixels(self, arg):
        x, y, w, h, fmt, typ, ptr = [arg(i) for i in range(7)]
        w, h = s32(w), s32(h)
        rp = self.fn('glReadPixels', None, [c_int, c_int, c_int, c_int, c_uint, c_uint, c_void_p])
        if self.bufbind.get(GL_PIXEL_PACK_BUFFER):
            rp(s32(x), s32(y), w, h, fmt, typ, ptr); return 0
        nbytes, _ = self._pix_size(w, h, fmt, typ)
        if nbytes <= 0 or not ptr: return 0
        buf = ctypes.create_string_buffer(nbytes)
        rp(s32(x), s32(y), w, h, fmt, typ, buf)
        self.mu.mem_write(ptr, buf.raw)
        return 0

    # ------------------------------------------------------------------ queries
    NVALS = {0x0BA2: 4, 0x0C10: 4, 0x0C23: 4, 0x0D3A: 2, 0x0C22: 4, 0x0B70: 2, 0x8005: 4, 0x846D: 2, 0x846E: 2}
    INT_OVERRIDE = {0x0D33: 4096, 0x84E8: 4096}  # Vita limits (MAX_TEXTURE_SIZE, MAX_RENDERBUFFER_SIZE)

    def h_glGetIntegerv(self, arg):
        pname, p = arg(0), arg(1)
        if not p: return 0
        if pname == 0x8CA6: vals = [self.fb_draw]          # (DRAW_)FRAMEBUFFER_BINDING
        elif pname == 0x8CAA: vals = [self.fb_read]        # READ_FRAMEBUFFER_BINDING
        elif pname in self.INT_OVERRIDE: vals = [self.INT_OVERRIDE[pname]]
        else:
            buf = (c_int * 16)()
            self.fn('glGetIntegerv', None, [c_uint, c_void_p])(pname, buf)
            vals = list(buf[:self.NVALS.get(pname, 1)])
        self.mu.mem_write(p, struct.pack('<%di' % len(vals), *vals))
        return 0

    def h_glGetFloatv(self, arg):
        pname, p = arg(0), arg(1)
        if not p: return 0
        buf = (c_float * 16)()
        self.fn('glGetFloatv', None, [c_uint, c_void_p])(pname, buf)
        n = 16 if pname in (0x0BA6, 0x0BA7, 0x0BA8) else self.NVALS.get(pname, 1)
        self.mu.mem_write(p, struct.pack('<%df' % n, *buf[:n]))
        return 0

    def h_glGetBooleanv(self, arg):
        pname, p = arg(0), arg(1)
        if not p: return 0
        buf = (c_ubyte * 16)()
        self.fn('glGetBooleanv', None, [c_uint, c_void_p])(pname, buf)
        n = self.NVALS.get(pname, 1)
        self.mu.mem_write(p, bytes(buf[:n]))
        return 0

    def h_glGetError(self, arg):
        return 0  # host errors are consumed and logged after each call

    def h_glGetTexParameteriv(self, arg):
        v = c_int()
        self.fn('glGetTexParameteriv', None, [c_uint, c_uint, c_void_p])(arg(0), arg(1), ctypes.byref(v))
        if arg(2): self.mu.mem_write(arg(2), struct.pack('<i', v.value))
        return 0

    # ------------------------------------------------------------------ frame end
    def swap(self, frame):
        if not self.shots: return
        if not ((self.shot_every and frame % self.shot_every == 0) or frame in self.shot_frames): return
        try:
            self.save_png(os.path.join(self.shots, 'frame_%05d.png' % frame))
        except Exception as e:
            self.say('[hgl] screenshot failed: %r' % e)

    def read_display(self):
        g = self.fn
        bfb = g('glBindFramebuffer', None, [c_uint, c_uint])
        bfb(GL_READ_FRAMEBUFFER, self.fb0)
        pb = self.bufbind.get(GL_PIXEL_PACK_BUFFER)
        if pb: g('glBindBuffer', None, [c_uint, c_uint])(GL_PIXEL_PACK_BUFFER, 0)
        g('glReadBuffer', None, [c_uint])(GL_COLOR_ATTACHMENT0)  # per-FBO state: only touches fb0
        buf = ctypes.create_string_buffer(self.W * self.H * 4)
        g('glReadPixels', None, [c_int, c_int, c_int, c_int, c_uint, c_uint, c_void_p])(0, 0, self.W, self.H, GL_RGBA, GL_UNSIGNED_BYTE, buf)
        # restore the game's read binding
        bfb(GL_READ_FRAMEBUFFER, self.fb0 if self.fb_read == 0 else self.fb_read)
        if pb: g('glBindBuffer', None, [c_uint, c_uint])(GL_PIXEL_PACK_BUFFER, pb)
        self.drain_errors('<screenshot>')
        return buf.raw

    def save_png(self, path):
        import numpy as np
        from PIL import Image
        a = np.frombuffer(self.read_display(), dtype=np.uint8).reshape(self.H, self.W, 4)[::-1, :, :3]
        Image.fromarray(np.ascontiguousarray(a), 'RGB').save(path)
        self.say('[hgl] saved %s' % path)

    def report(self):
        if self.unimpl:
            self.say('[hgl] stubbed (not on host): ' + ', '.join('%s=%d' % kv for kv in sorted(self.unimpl.items(), key=lambda x: -x[1])))
        errs = [(k, v) for k, v in self.errs.items()]
        if errs:
            self.say('[hgl] host GL errors: ' + ', '.join('%s:%s=%d' % (k[0], ERRNAMES.get(k[1], k[1]), v) for k, v in errs))
