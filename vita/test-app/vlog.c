// Step log at ux0:data/cvftest.log, flushed after every line, so a crash
// still leaves a trail.
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
	vlog("0 program loaded (C constructor)");
}
