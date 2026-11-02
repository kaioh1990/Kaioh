// Stack switches for PS Vita goroutine fibers. Callee-saved registers are
// pushed onto the stack (and the stack pointer recorded) before switching, so
// TinyGo's conservative GC sees every live pointer. r3 is pushed only to keep
// the stack 8-byte aligned for the call.
.syntax unified
.arm

.section .text.tinygo_vita_fiber_run
.global tinygo_vita_fiber_run
.type tinygo_vita_fiber_run, %function
// r0 = SceFiber*, r1 = &systemStack. Runs on the main thread's stack.
tinygo_vita_fiber_run:
    push {r3-r11, lr}
    str  sp, [r1]
    mov  r4, r1
    bl   tinygo_vita_fiber_run_c
    mov  r1, #0
    str  r1, [r4]
    pop  {r3-r11, pc}
.size tinygo_vita_fiber_run, .-tinygo_vita_fiber_run

.section .text.tinygo_vita_fiber_pause
.global tinygo_vita_fiber_pause
.type tinygo_vita_fiber_pause, %function
// r0 = &task.sp. Runs on a goroutine (fiber) stack.
tinygo_vita_fiber_pause:
    push {r3-r11, lr}
    str  sp, [r0]
    bl   tinygo_vita_fiber_pause_c
    pop  {r3-r11, pc}
.size tinygo_vita_fiber_pause, .-tinygo_vita_fiber_pause
