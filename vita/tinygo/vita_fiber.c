// PS Vita goroutine fibers for TinyGo (see src/internal/task/task_stack_vita.go).
#include <psp2/fiber.h>
#include <psp2/sysmodule.h>
#include <stdint.h>

void tinygo_task_exit(void);

// Fiber entry: args points at {fn, argsBundle}. fn is TinyGo's goroutine start
// wrapper, which takes the argument bundle as its only parameter.
static void vita_fiber_entry(SceUInt32 argOnInitialize, SceUInt32 argOnRun) {
	uintptr_t *p = (uintptr_t *)argOnInitialize;
	void (*fn)(void *) = (void (*)(void *))p[0];
	fn((void *)p[1]);
	tinygo_task_exit(); // pauses forever; never returns here
}

int tinygo_vita_fiber_init(SceFiber *fiber, void *ctx, unsigned ctxSize, void *entryArgs) {
	return _sceFiberInitializeImpl(fiber, "goroutine", vita_fiber_entry,
		(SceUInt32)(uintptr_t)entryArgs, ctx, ctxSize, NULL);
}

__attribute__((constructor(101))) static void vita_fiber_module(void) {
	sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER);
}
