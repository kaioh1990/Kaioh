//go:build vita

package gles2

/*
#include <stdlib.h>
void *vglGetProcAddress(const char *name);
// vitaGL lacks many GLES3 entry points; calling a NULL function pointer
// crashes, so unsupported functions become no-ops that return 0.
static unsigned long long vita_gl_missing(void) { return 0; }
static void *vita_gl_proc(const char *name) {
	void *p = vglGetProcAddress(name);
	return p ? p : (void *)vita_gl_missing;
}
*/
import "C"
import "unsafe"

func getProcAddress(namea string) unsafe.Pointer {
	cname := C.CString(namea)
	defer C.free(unsafe.Pointer(cname))
	return C.vita_gl_proc(cname)
}
