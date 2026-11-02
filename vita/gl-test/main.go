//go:build vita

package main

/*
#cgo CFLAGS: -std=c2x
#cgo LDFLAGS: -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub
#cgo LDFLAGS: -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub -lSceAppMgr_stub -lSceAppUtil_stub
#cgo LDFLAGS: -lSceSysmodule_stub -lSceIofilemgr_stub -lSceCommonDialog_stub -lScePower_stub
#cgo LDFLAGS: -lSceKernelDmacMgr_stub -lSceRazorCapture_stub_weak -lstdc++ -lsupc++ -lm
#include <stdlib.h>
void vlog(const char *s);
void vita_clock444(void);
int vita_gl_init(void);
void vita_gl_swap(void);
int vita_start_pressed(void);
void *vglGetProcAddress(const char *name);
*/
import "C"

import (
	"fmt"
	"time"
	"unsafe"

	gl "github.com/leonkasovan/gl/v3.2/gles2"
)

func logf(format string, a ...interface{}) {
	s := C.CString(fmt.Sprintf(format, a...))
	C.vlog(s)
	C.free(unsafe.Pointer(s))
}

// Vita (vitaGL GLSL translator) version of the engine's sprite shaders.
const spriteVert = `
uniform mat4 modelview;
uniform mat4 projection;
attribute vec2 position;
attribute vec2 uv;
varying vec2 texcoord;
void main(void) {
	texcoord = uv;
	gl_Position = projection * (modelview * vec4(position, 0.0, 1.0));
}
`

const spriteFrag = `
uniform sampler2D tex;
uniform sampler2D pal;
uniform vec4 tint;
uniform vec3 add;
uniform vec3 mult;
uniform float alpha;
uniform float gray;
uniform int mask;
uniform int isFlat;
uniform int isRgba;
uniform int neg;
uniform vec4 spriteUV;
uniform vec4 palUV;
varying vec2 texcoord;
void main(void) {
	vec4 c;
	vec3 neg_base = vec3(1.0);
	vec3 final_add = add;
	vec4 final_mul = vec4(mult, alpha);
	if (isFlat != 0) {
		c = tint;
		neg_base *= c.a;
		final_add *= c.a;
		final_mul.rgb *= alpha;
	} else {
		vec2 atlasUV = clamp(texcoord, spriteUV.xy, spriteUV.zw - 0.0001);
		c = texture2D(tex, atlasUV);
		if (isRgba != 0) {
			if (mask == -1) c.a = 1.0;
			neg_base *= c.a;
			final_add *= c.a;
			final_mul.rgb *= alpha;
		} else {
			c = texture2D(pal, vec2(palUV.x + palUV.z * c.r * 0.9966, palUV.y));
			if (mask == -1) c.a = 1.0;
		}
	}
	if (neg != 0) c.rgb = neg_base - c.rgb;
	c.rgb = mix(vec3((c.r + c.g + c.b) / 3.0), c.rgb, 1.0 - gray);
	c.rgb += final_add;
	c *= final_mul;
	if (isFlat == 0) c.rgb = mix(c.rgb, tint.rgb * c.a, tint.a);
	gl_FragColor = c;
}
`

func compile(kind uint32, src string) (uint32, bool) {
	sh := gl.CreateShader(kind)
	cs, free := gl.Strs(src + "\x00")
	gl.ShaderSource(sh, 1, cs, nil)
	free()
	gl.CompileShader(sh)
	var ok int32
	gl.GetShaderiv(sh, gl.COMPILE_STATUS, &ok)
	if ok == 0 {
		var n int32
		gl.GetShaderiv(sh, gl.INFO_LOG_LENGTH, &n)
		buf := make([]byte, n+1)
		if n > 0 {
			gl.GetShaderInfoLog(sh, n, nil, &buf[0])
		}
		logf("  shader compile FAILED: %s", string(buf))
		return sh, false
	}
	return sh, true
}

func ortho(l, r, b, t float32) [16]float32 {
	return [16]float32{2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -1, 0, -(r + l) / (r - l), -(t + b) / (t - b), 0, 1}
}

func main() {
	logf("1 Go main started")
	C.vita_clock444()
	logf("2 CPU 444 MHz requested")
	r := C.vita_gl_init()
	logf("3 vglInitExtended -> %d", int(r))
	if err := gl.Init(func(name string) unsafe.Pointer { n := C.CString(name); defer C.free(unsafe.Pointer(n)); return C.vglGetProcAddress(n) }); err != nil {
		logf("gl.Init error: %v", err)
	}
	logf("4 GL: %s / %s / %s", gl.GoStr(gl.GetString(gl.VERSION)), gl.GoStr(gl.GetString(gl.RENDERER)), gl.GoStr(gl.GetString(gl.SHADING_LANGUAGE_VERSION)))

	vs, ok1 := compile(gl.VERTEX_SHADER, spriteVert)
	fs, ok2 := compile(gl.FRAGMENT_SHADER, spriteFrag)
	logf("5 shaders compiled: vertex=%v fragment=%v", ok1, ok2)
	prog := gl.CreateProgram()
	gl.AttachShader(prog, vs)
	gl.AttachShader(prog, fs)
	gl.BindAttribLocation(prog, 0, gl.Str("position\x00"))
	gl.BindAttribLocation(prog, 1, gl.Str("uv\x00"))
	gl.LinkProgram(prog)
	var linked int32
	gl.GetProgramiv(prog, gl.LINK_STATUS, &linked)
	logf("6 program linked: %v", linked != 0)
	gl.UseProgram(prog)
	u := func(n string) int32 { return gl.GetUniformLocation(prog, gl.Str(n+"\x00")) }

	// Paletted sprite: 64x64 index texture (GL_RED / R8, like the engine) + 256-color palette.
	idx := make([]byte, 64*64)
	for y := 0; y < 64; y++ {
		for x := 0; x < 64; x++ {
			idx[y*64+x] = byte((x/8 + y/8) * 16 % 256)
			if (x-32)*(x-32)+(y-32)*(y-32) > 30*30 {
				idx[y*64+x] = 0 // transparent corners (palette entry 0)
			}
		}
	}
	pal := make([]byte, 256*4)
	for i := 1; i < 256; i++ {
		pal[i*4], pal[i*4+1], pal[i*4+2], pal[i*4+3] = byte(i), byte(255-i), byte((i*3)%256), 255
	}
	var tex [2]uint32
	gl.GenTextures(2, &tex[0])
	gl.ActiveTexture(gl.TEXTURE0)
	gl.BindTexture(gl.TEXTURE_2D, tex[0])
	gl.PixelStorei(gl.UNPACK_ALIGNMENT, 1)
	gl.TexImage2D(gl.TEXTURE_2D, 0, gl.R8, 64, 64, 0, gl.RED, gl.UNSIGNED_BYTE, unsafe.Pointer(&idx[0]))
	logf("7 R8 index texture upload: glGetError=0x%x", gl.GetError())
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
	gl.ActiveTexture(gl.TEXTURE1)
	gl.BindTexture(gl.TEXTURE_2D, tex[1])
	gl.TexImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, unsafe.Pointer(&pal[0]))
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
	gl.TexParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
	gl.Uniform1i(u("tex"), 0)
	gl.Uniform1i(u("pal"), 1)
	gl.Uniform4f(u("spriteUV"), 0, 0, 1, 1)
	gl.Uniform4f(u("palUV"), 0, 0.5, 1, 0)
	gl.Uniform3f(u("mult"), 1, 1, 1)
	gl.Uniform3f(u("add"), 0, 0, 0)
	gl.Uniform1f(u("alpha"), 1)
	gl.Uniform1f(u("gray"), 0)
	gl.Uniform4f(u("tint"), 0, 0, 0, 0)
	gl.Uniform1i(u("mask"), 0)
	gl.Uniform1i(u("isRgba"), 0)
	gl.Uniform1i(u("neg"), 0)
	proj := ortho(0, 960, 544, 0)
	gl.UniformMatrix4fv(u("projection"), 1, false, &proj[0])

	quad := []float32{0, 0, 0, 0, 64, 0, 1, 0, 0, 64, 0, 1, 64, 64, 1, 1}
	var vbo uint32
	gl.GenBuffers(1, &vbo)
	gl.BindBuffer(gl.ARRAY_BUFFER, vbo)
	gl.BufferData(gl.ARRAY_BUFFER, len(quad)*4, gl.Ptr(quad), gl.STATIC_DRAW)
	gl.EnableVertexAttribArray(0)
	gl.EnableVertexAttribArray(1)
	gl.VertexAttribPointerWithOffset(0, 2, gl.FLOAT, false, 16, 0)
	gl.VertexAttribPointerWithOffset(1, 2, gl.FLOAT, false, 16, 8)
	gl.Enable(gl.BLEND)
	gl.BlendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
	gl.Viewport(0, 0, 960, 544)
	logf("8 setup done, glGetError=0x%x", gl.GetError())

	// Draw N sprites per frame; measure frames per second at several N.
	for _, n := range []int{50, 200, 500, 1000} {
		frames := 0
		start := time.Now()
		for time.Since(start) < 3*time.Second {
			gl.ClearColor(0.1, 0.1, 0.25, 1)
			gl.Clear(gl.COLOR_BUFFER_BIT)
			for i := 0; i < n; i++ {
				x := float32((i*37+frames*3)%900) + 10
				y := float32((i*53)%480) + 10
				mv := [16]float32{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, 0, 1}
				gl.UniformMatrix4fv(u("modelview"), 1, false, &mv[0])
				gl.Uniform1i(u("isFlat"), 0)
				gl.DrawArrays(gl.TRIANGLE_STRIP, 0, 4)
			}
			C.vita_gl_swap()
			frames++
			if C.vita_start_pressed() != 0 {
				break
			}
		}
		fps := float64(frames) / time.Since(start).Seconds()
		logf("9 %4d sprites/frame: %.1f fps", n, fps)
	}
	logf("10 done (glGetError=0x%x). Press START to exit.", gl.GetError())
	for C.vita_start_pressed() == 0 {
		gl.ClearColor(0, 0.3, 0, 1)
		gl.Clear(gl.COLOR_BUFFER_BIT)
		C.vita_gl_swap()
	}
}
