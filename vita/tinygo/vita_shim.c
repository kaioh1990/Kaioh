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

// TinyGo grabs its heap with one big anonymous mmap (it halves the size until
// one succeeds). Take it straight from the kernel so newlib's malloc heap
// stays free for the C libraries (SDL, vitaGL, ...).
#include <psp2/kernel/sysmem.h>
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
	char msg[96];
	void *p = NULL;
	size_t sz = (len + 0xFFFFF) & ~(size_t)0xFFFFF; // 1 MB granularity
	SceUID b = sceKernelAllocMemBlock("go-heap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz, NULL);
	if (b >= 0 && sceKernelGetMemBlockBase(b, &p) < 0) p = NULL;
	snprintf(msg, sizeof msg, "  runtime heap: %u MB -> %s", (unsigned)(sz >> 20), p ? "ok" : "no");
	vlog(msg);
	if (!p) { errno = ENOMEM; return (void *)-1; }
	memset(p, 0, len);
	return p;
}
int munmap(void *addr, size_t len) { return 0; } // heap is never returned
int pause(void) { for (;;) sceKernelDelayThread(1000000); return 0; }
// Fatal-signal reporting isn't available on the Vita.
void tinygo_register_fatal_signals(void) { vlog("  runtime main entered"); }
void tinygo_signal_enable(unsigned s) {}
void tinygo_signal_ignore(unsigned s) {}
void tinygo_signal_disable(unsigned s) {}
// Go's runtime passes a time64 struct {int64 sec; int64 nsec}; the Vita's
// newlib uses 32-bit time_t, so fill Go's layout explicitly.
struct go_timespec { long long sec; long long nsec; };
int __clock_gettime64(int clk, void *out) {
	struct timespec ts;
	int r = clock_gettime(clk, &ts);
	struct go_timespec *g = out;
	g->sec = (long long)ts.tv_sec;
	g->nsec = (long long)ts.tv_nsec;
	return r;
}

// IEEE floating point (no flush-to-zero, no default-NaN), like a PC. The Vita
// starts threads in flush-to-zero mode, which made rollback math differ.
void tinygo_vita_ieee_fp(void) {
	unsigned v;
	__asm__ volatile("vmrs %0, fpscr" : "=r"(v));
	v &= ~((1u << 24) | (1u << 25) | (3u << 22));
	__asm__ volatile("vmsr fpscr, %0" : : "r"(v));
}
__attribute__((constructor(102))) static void vita_fp_init(void) { tinygo_vita_ieee_fp(); }

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
// If the app shows a debug screen, echo stdout/stderr there too.
__attribute__((weak)) int psvDebugScreenPuts(const char *text);
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n) {
	if (fd == 1 || fd == 2) {
		if (psvDebugScreenPuts) {
			char tmp[257];
			size_t k = n < 256 ? n : 256;
			memcpy(tmp, buf, k); tmp[k] = 0;
			psvDebugScreenPuts(tmp);
		}
		SceUID f = sceIoOpen("ux0:data/cvftest.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (f >= 0) { sceIoWrite(f, buf, n); sceIoClose(f); }
		return (ssize_t)n;
	}
	return __real_write(fd, buf, n);
}

// clock_gettime: newlib's CLOCK_MONOTONIC isn't usable on the Vita (Go saw
// garbage durations), so monotonic time comes from the process timer
// (microseconds since the app started). Realtime keeps newlib's version.
#include <psp2/kernel/processmgr.h>
int __real_clock_gettime(clockid_t clk, struct timespec *ts);
int __wrap_clock_gettime(clockid_t clk, struct timespec *ts) {
	if (clk != CLOCK_REALTIME) {
		SceUInt64 us = sceKernelGetProcessTimeWide();
		ts->tv_sec = (time_t)(us / 1000000);
		ts->tv_nsec = (long)(us % 1000000) * 1000;
		return 0;
	}
	return __real_clock_gettime(clk, ts);
}
