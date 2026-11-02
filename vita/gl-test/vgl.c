#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <string.h>

void vlog(const char *s) {
	SceUID fd = sceIoOpen("ux0:data/cvftest.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd < 0) return;
	sceIoWrite(fd, s, strlen(s));
	sceIoWrite(fd, "\n", 1);
	sceIoClose(fd);
}
__attribute__((constructor)) static void vlog_start(void) {
	sceIoRemove("ux0:data/cvftest.log");
	vlog("0 program loaded");
}
void vita_clock444(void) {
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
}
int vita_gl_init(void) {
	return vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
}
void vita_gl_swap(void) { vglSwapBuffers(GL_FALSE); }
int vita_start_pressed(void) {
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	return (pad.buttons & SCE_CTRL_START) != 0;
}
