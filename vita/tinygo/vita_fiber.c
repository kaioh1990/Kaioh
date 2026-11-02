// PS Vita goroutine fibers for TinyGo (see src/internal/task/task_stack_vita.go).
#include <psp2/fiber.h>
#include <psp2/sysmodule.h>
#include <stdint.h>

void tinygo_task_exit(void);
void tinygo_vita_ieee_fp(void);

// Fiber entry: args points at {fn, argsBundle}. fn is TinyGo's goroutine start
// wrapper, which takes the argument bundle as its only parameter.
static void vita_fiber_entry(SceUInt32 argOnInitialize, SceUInt32 argOnRun) {
	uintptr_t *p = (uintptr_t *)argOnInitialize;
	tinygo_vita_ieee_fp(); // each fiber may start with the default FPSCR
	void (*fn)(void *) = (void (*)(void *))p[0];
	fn((void *)p[1]);
	tinygo_task_exit(); // pauses forever; never returns here
}

#include <stdio.h>
__attribute__((weak)) void vlog(const char *s) {}
static void fiber_err(const char *what, int r, void *p) {
	char msg[96];
	snprintf(msg, sizeof msg, "fiber: %s failed 0x%08x (%p)", what, (unsigned)r, p);
	vlog(msg);
}

int tinygo_vita_fiber_init(SceFiber *fiber, void *ctx, unsigned ctxSize, void *entryArgs) {
	int r = _sceFiberInitializeImpl(fiber, "goroutine", vita_fiber_entry,
		(SceUInt32)(uintptr_t)entryArgs, ctx, ctxSize, NULL);
	if (r < 0) fiber_err("init", r, ctx);
	return r;
}

// Called by the assembly helpers after registers are saved.
void tinygo_vita_fiber_run_c(SceFiber *fiber) {
	SceUInt32 argOnRun = 0;
	int r = sceFiberRun(fiber, 0, &argOnRun);
	if (r < 0) fiber_err("run", r, fiber);
}
void tinygo_vita_fiber_pause_c(void) {
	SceUInt32 argOnRun = 0;
	int r = sceFiberReturnToThread(0, &argOnRun);
	if (r < 0) fiber_err("return", r, 0);
}

__attribute__((constructor(101))) static void vita_fiber_module(void) {
	sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER);
}
