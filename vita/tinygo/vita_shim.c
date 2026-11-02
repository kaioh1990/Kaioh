// libc pieces TinyGo's Linux runtime expects but the Vita's newlib lacks.
#include <stdlib.h>
#include <malloc.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>

// Optional startup trace (vita_trace) for debugging; appends to the app's log.
__attribute__((weak)) void vlog(const char *s) {}
#include <stdio.h>

// TinyGo grabs its heap with one big anonymous mmap.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
	char msg[96];
	void *p = memalign(4096, len);
	snprintf(msg, sizeof msg, "  runtime heap: mmap %u MB -> %s", (unsigned)(len >> 20), p ? "ok" : "no");
	vlog(msg);
	if (!p) { errno = ENOMEM; return (void *)-1; }
	memset(p, 0, len);
	return p;
}
int munmap(void *addr, size_t len) { free(addr); return 0; }
int pause(void) { for (;;) sceKernelDelayThread(1000000); return 0; }
// Fatal-signal reporting isn't available on the Vita.
void tinygo_register_fatal_signals(void) { vlog("  runtime main entered"); }
void tinygo_signal_enable(unsigned s) {}
void tinygo_signal_ignore(unsigned s) {}
void tinygo_signal_disable(unsigned s) {}
int __clock_gettime64(int clk, void *ts) { return clock_gettime(clk, (struct timespec *)ts); }

// Futexes: TinyGo's cooperative scheduler runs every goroutine on one Vita
// thread, so "waiting" just means sleeping until the value changes or the
// timeout passes.
#include <stdint.h>
void tinygo_futex_wait(volatile uint32_t *addr, uint32_t cmp) {
	while (*addr == cmp) sceKernelDelayThread(100);
}
void tinygo_futex_wait_timeout(volatile uint32_t *addr, uint32_t cmp, uint64_t timeout) {
	uint64_t waited = 0, us = timeout / 1000;
	while (*addr == cmp && waited < us) {
		uint64_t step = us - waited > 1000 ? 1000 : us - waited;
		if (step == 0) step = 1;
		sceKernelDelayThread((SceUInt)step);
		waited += step;
	}
}
void tinygo_futex_wake(uint32_t *addr) {}
void tinygo_futex_wake_all(uint32_t *addr) {}

// Random numbers from the Vita's hardware RNG.
#include <psp2/kernel/rng.h>
long getrandom(void *buf, size_t len, unsigned flags) {
	unsigned char *p = buf; size_t left = len;
	while (left) {
		size_t n = left > 64 ? 64 : left;
		if (sceKernelGetRandomNumber(p, n) < 0) return -1;
		p += n; left -= n;
	}
	return (long)len;
}

// glibc/musl name for errno; newlib calls it __errno.
int *__errno_location(void) { return &errno; }

// stdout/stderr (Go's println, panic messages) go to ux0:data/cvftest.log,
// since the Vita has no console. Other descriptors use newlib.
#include <psp2/io/fcntl.h>
#include <reent.h>
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n) {
	if (fd == 1 || fd == 2) {
		SceUID f = sceIoOpen("ux0:data/cvftest.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (f >= 0) { sceIoWrite(f, buf, n); sceIoClose(f); }
		return (ssize_t)n;
	}
	return __real_write(fd, buf, n);
}
