# Chain Walk Analysis — Which Sub-branch Fires?

## The 5-Level Chain Walk (value==2 path at 0x909b538)

```
LDR X8, [X20, #0x28]    // Level 1: encoder sub-object (~9.2KB, kalloc.12288)
LDR X8, [X8, #0x70]     // Level 2: nested pointer
LDR X8, [X8, #0x288]    // Level 3: nested pointer (deep config)
LDR W8, [X8, #0x58]     // Level 4: 32-bit value
CBZ W8, Sub-branch-B     // Zero → B (data ignored); Nonzero → A (data used!)
```

## Assessment: Sub-branch B (zero) is MORE LIKELY

**Reasoning:**

1. **Offset depth suggests optional/advanced feature state.**
   Level 3 at +0x288 (648 bytes into a sub-object) and Level 4 at +0x58 (88 bytes)
   indicates a deeply nested configuration field, likely related to an optional codec
   feature (B-frames, temporal scalability, rate control mode, etc.)

2. **MV-HEVC is a simple stereo encode.**
   Our test encodes use: 128x128, real-time, max keyframe=1, no B-frames, no reordering.
   This is the simplest possible MV-HEVC configuration. Advanced codec features that
   require deep config initialization are unlikely to be active.

3. **CBZ is the "default" branch.**
   In compiler-generated code, CBZ typically guards the special/optional path.
   The zero case (Sub-branch B) is the common/fast path; nonzero (Sub-branch A) is
   the special case that only fires under specific encoder configurations.

4. **Sub-branch B is a logging + cleanup path.**
   It calls os_log then makes a simple vtable call. This is consistent with a
   "this feature isn't active, log and skip" pattern.

## How to Force Sub-branch A

If Sub-branch B fires (data ignored), we may be able to force Sub-branch A by:
- Enabling B-frames or temporal scalability on the encoder session
- Using a more complex encoding configuration
- Setting specific VTSessionSetProperty values that activate the deep config

Alternatively, if the 32-bit value at the chain endpoint is a **frame counter** or
**output count**, it would become nonzero after the first successful encode, making
Sub-branch A fire for all subsequent dispatches including at frame 214.

## Conclusion

**Cannot determine statically.** On-device test with the type confusion probe in
"trigger" mode will give a definitive answer via crash log analysis:
- Crash in vtable[0x1F0] code → Sub-branch A fired (data used!)
- Crash at 0x90b4f40 (os_log/panic) → Sub-branch B fired (data ignored)
- No crash → either path was benign

**If Sub-branch B fires: not the end.** Value==1 path (vtable[0x188]) may still provide
indirect type confusion if its return value points into the HEVC buffer. And the
intra-buffer firmware parameter corruption (entries 256-640) is an entirely separate
exploitation avenue that doesn't depend on the dispatch site at all.
