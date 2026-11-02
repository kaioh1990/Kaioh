//go:build scheduler.tasks && vita

package task

// PS Vita: goroutines run as SceFiber fibers.
//
// The Vita kernel only accepts system calls made from a stack it knows about,
// so TinyGo's usual "switch the stack pointer by hand" approach crashes as
// soon as a goroutine calls into the OS. SceFiber registers each fiber's stack
// with the kernel. The scheduler (on the main thread's stack) resumes a
// goroutine with sceFiberRun and the goroutine pauses with
// sceFiberReturnToThread, which matches TinyGo's cooperative model exactly.
//
// Memory layout of each goroutine's stack allocation (scanned conservatively
// by the GC, so saved registers inside the SceFiber block are found too):
//   +0    stack canary
//   +8    SceFiber control block (128 bytes)
//   +136  fn, args (passed to the fiber entry)
//   +256  fiber context (the stack itself)

import (
	"internal/gclayout"
	"unsafe"
)

const stackCanary = uintptr(uint64(0x670c1333b83bf575) & uint64(^uintptr(0)))

const vitaFiberHeader = 256

type state struct {
	// sp is the stack pointer saved when the goroutine paused (callee-saved
	// registers are pushed just below it).
	sp uintptr

	// canaryPtr points to the lowest word of the stack allocation.
	canaryPtr *uintptr

	fiber unsafe.Pointer
}

const hasReleasableStack = false

// systemStack is the main thread's stack pointer while a goroutine runs, so
// the GC can scan the scheduler's stack.
var systemStack uintptr

//export tinygo_task_exit
func taskExit() {
	exit(false, nil)
}

//export tinygo_vita_fiber_init
func vitaFiberInit(fiber, ctx unsafe.Pointer, ctxSize uintptr, entryArgs unsafe.Pointer) int32

//export tinygo_vita_fiber_run
func vitaFiberRun(fiber unsafe.Pointer, systemStack *uintptr)

//export tinygo_vita_fiber_pause
func vitaFiberPause(sp *uintptr)

func (s *state) initialize(fn uintptr, args unsafe.Pointer, stackSize uintptr) {
	if stackSize < 32*1024 {
		stackSize = 32 * 1024
	}
	stackSize = (stackSize + 7) &^ 7
	stack := runtime_alloc(stackSize, gclayout.Conservative.AsPtr())

	s.canaryPtr = (*uintptr)(stack)
	*s.canaryPtr = stackCanary

	s.fiber = unsafe.Add(stack, 8)
	entryArgs := unsafe.Add(stack, 136)
	*(*uintptr)(entryArgs) = fn
	*(*unsafe.Pointer)(unsafe.Add(entryArgs, unsafe.Sizeof(uintptr(0)))) = args

	ctx := unsafe.Add(stack, vitaFiberHeader)
	if vitaFiberInit(s.fiber, ctx, stackSize-vitaFiberHeader, entryArgs) != 0 {
		panic("could not create goroutine fiber")
	}
}

func (s *state) resume() {
	vitaFiberRun(s.fiber, &systemStack)
}

func (s *state) pause() {
	vitaFiberPause(&s.sp)
}

// SystemStack returns the system stack pointer when called from a task stack.
// When called from the system stack, it returns 0.
func SystemStack() uintptr {
	return systemStack
}

// start creates and starts a new goroutine with the given function and arguments.
// The new goroutine is scheduled to run later.
func start(fn uintptr, args unsafe.Pointer, stackSize uintptr) {
	t := &Task{}
	inheritSynctest(t)
	addLiveTask(t)
	t.state.initialize(fn, args, stackSize)
	scheduleTask(t)
}

// unlinkStack does nothing here, since only asyncify keeps a list of stacks.
func unlinkStack(t *Task) {}
