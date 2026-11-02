Files added to / changed in TinyGo 0.44's source tree for the Vita target:

- `src/internal/task/task_stack_vita.go` (new): goroutines as SceFiber fibers.
- `src/internal/task/task_stack.go`: build tag gets `&& !vita`.
- `src/internal/task/task_stack_arm.go`: build tag gets `&& !vita`.
- `targets/`: copy `vita_shim.c`, `vita_pad.c`, `vita_fiber.c`, `vita_fiber.S` there.
