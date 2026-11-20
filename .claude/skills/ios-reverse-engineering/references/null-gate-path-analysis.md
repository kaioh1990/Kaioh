# NULL-Gate Path Enabling Analysis — Defect 2

## Task
Analyze what happens when the fill loop overflow converts NULL pointer fields
to non-NULL, enabling normally-disabled code paths in the AVE encoder.

## Critical Re-interpretation of the cbz Pattern

The earlier analysis described the pointer fields as "NULL-check gates" with
the pattern: `load ptr → cbz → load from STACK (discarding corrupted value)`.

This was **misinterpreted**. The actual pattern is a conditional branch:

```
ldr  x8, [x19, #0x4AC0]     ; load pointer from session context
cbz  x8, null_path           ; if NULL → skip to alternative
; --- FALL-THROUGH (non-NULL path) ---
ldr  x8, [x8, #0x2D0]       ; DEREFERENCE the pointer
; ... use the loaded value ...
b    merge
null_path:
ldr  x8, [sp, #0x...]       ; alternative: load from stack (safe default)
merge:
; continue
```

The "load from stack" is the NULL/safe path, NOT the fall-through. When the
pointer is non-NULL (after overflow), the fall-through **DOES dereference it**.

This means the earlier conclusion that "pointer fields are only gates" was
**incomplete**. They ARE gates, but the enabled path performs real memory
accesses through the corrupted pointer.

## Fill Loop Writes ALL Indices

The fill loop writes indices 0 through (NTL-1). So NTL=276 writes to
offsets +0x4678 through +0x4678 + 275*4 = +0x4678 + 0x44C = +0x4AC4.

This means NTL=276 writes BOTH halves of the pointer at +0x4AC0:
- Index 274 → +0x4AC0 (lower 32 bits) ← w24
- Index 275 → +0x4AC4 (upper 32 bits) ← w24

A NULL pointer (0x0000000000000000) becomes:
  `(w24 << 32) | w24` = `0x0000000500000005` (for w24=5)

This address is in userspace and CAN be mapped with mmap(MAP_FIXED).

## Question 1: +0x4AC0 Path (NTL >= 276, 1 reference)

When this pointer becomes 0x0000000500000005 and the fall-through executes:

The single reference performs computed-offset access. Without the exact
disassembly of the fall-through body (we only have summary from the
kernelcache scan), the possibilities are:

**Scenario A — Simple read**: The path loads a value from the corrupted
pointer at some offset (e.g., `ldr x8, [x8, #0x...]`). If we map memory
at 0x0000000500000005's page and plant controlled data, the encoder reads
our fake values. This gives us an **arbitrary read primitive** — we control
what the encoder thinks the object contains.

**Scenario B — Read then write-back**: The path reads a value and writes it
to another session context field. This would let us inject a controlled value
into the session context, potentially corrupting other state that affects
later encoder operations.

**Scenario C — Function pointer call**: If the loaded value is used as a
function pointer or vtable dispatch, we get **code execution** directly.
However, PAC (Pointer Authentication) on A20 makes this unlikely unless
the call site uses an unsigned branch.

**What we can determine without device testing**: Scenario A is near-certain
given the documented dereference chains. Scenario B is plausible. Scenario C
is unlikely due to PAC.

## Question 2: +0x51C8 Path (NTL >= 726, 3 references)

This is the most interesting pointer with THREE dereference chains:
1. `ldr x8, [ptr+0x2D0]` → `ldr w8, [that+0x18]`
2. `ldr w8, [ptr+0x20AC]`
3. `ldr x22, [ptr+0x98]`

For NTL=726, same logic: both halves corrupted, pointer becomes
0x0000000500000005. All three paths attempt real dereferences.

**Chain 1 (double deref)**: Reads ptr+0x2D0, then reads THAT+0x18. This is
a two-level indirection. If we control the fake object at our mapped address,
we set +0x2D0 to point to another location we control, giving us full
control of the final w8 value.

**Chain 2 (deep offset)**: Reads ptr+0x20AC directly. This is 8KB into the
fake object. We need to map enough memory to cover this — mmap at the page
boundary with size >= 0x20AC + 8 = ~8.2KB. One 16KB page (0x4000) suffices.

**Chain 3 (pointer load)**: Reads ptr+0x98 into x22 (a pointer register).
x22 is then likely used for further operations. If we control +0x98 in
our fake object, we control x22, which could be used for subsequent
memory accesses or even (if lucky) a branch target.

**Key insight**: Chain 3 loads another POINTER from our controlled data into
x22. If the code later dereferences x22, we get a second-level fake object
primitive. This is a powerful building block for data-only exploitation.

## Question 3: Type Confusion Potential

**Yes, this is likely.** The normally-disabled paths were written assuming the
pointer points to a valid, type-correct kernel object. When we provide a
fake object via userspace mmap:

- The code expects specific fields at specific offsets (e.g., a refcount
  at +0x10, a size at +0x20, a pointer at +0x98).
- We provide arbitrary values that violate the type's invariants.
- The encoder uses these values in arithmetic, comparisons, and further
  pointer dereferences without additional type checks.

The stack-loaded alternative values (from the NULL path) are presumably
safe defaults. The non-NULL path skips these defaults entirely.

**However**, true type confusion in the classic sense (treating object A as
object B) requires the pointer to point to a real but wrong-type kernel
object. Our fake userspace object is more like a **fake object injection**
than type confusion — we control the layout completely.

## Question 4: Multi-Path Interaction (Large NTL)

With NTL >= 726, we corrupt ALL pointer fields from +0x4AC0 through +0x51CC
(and beyond). This enables multiple normally-disabled code paths simultaneously.

**Interaction effects:**

1. **State accumulation**: Path A writes a value to session_ctx+X. Path B reads
   session_ctx+X. If path A runs first and writes a value derived from our fake
   object, path B gets a doubly-corrupted input.

2. **Ordering sensitivity**: The encoder processes these paths in a specific
   order during frame encoding. Early paths can set up state that later paths
   consume, creating a cascade of controlled corruption.

3. **Coverage**: More enabled paths = more reads from our fake userspace memory
   = more chances one of those reads feeds into a useful write primitive.

**Risk**: More enabled paths also means more chances for an uncontrolled crash
before we reach the useful path. The fields between +0x4AC0 and +0x51C8 all
get corrupted too, and some of their enabled paths might crash.

**Mitigation**: The dead zone (+0x4698 to +0x4AC0) has no code references,
so corruption there is safe. The first dangerous field is +0x4AC0. We need
device testing to determine which fields between +0x4AC0 and +0x51C8 have
fall-through paths that crash vs. survive.

## Revised Exploitation Strategy

The original conclusion that "pointer fields are only NULL-check gates" was
too pessimistic. The corrected understanding:

1. **NTL=276**: Safest exploitation target. Corrupts only +0x4AC0's pointer.
   One fall-through path, one dereference. Map 16KB at 0x0000000500000000,
   plant fake object. If the read value feeds into a write → arbitrary write.

2. **NTL=726**: Aggressive but powerful. Many enabled paths. Chain 3 at
   +0x51C8 (loads a pointer into x22) is especially promising for building
   a multi-level fake object chain.

3. **Progressive scan needed**: Test NTL 276, 280, 284, ... incrementally
   to find which enabled paths crash and which survive. This maps out the
   safe exploitation window.

## Confidence Assessment

- **HIGH confidence**: The cbz fall-through DOES dereference the pointer
  (this follows directly from the documented dereference chains at +0x2D0,
  +0x20AC, +0x98 — these only make sense in the non-NULL path).
- **HIGH confidence**: NTL=276 corrupts both halves to create a userspace
  address (arithmetic is straightforward).
- **MEDIUM confidence**: mmap at 0x0000000500000000 will succeed on iOS
  (needs device confirmation — some iOS versions restrict low userspace mappings).
- **LOW confidence**: Which specific paths survive vs. crash before reaching
  the useful dereference (requires device testing).
- **UNKNOWN**: Whether the values read from the fake object feed into writes
  that give us a useful primitive (requires deeper disassembly or device testing).
