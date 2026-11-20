# Panic Log Analysis — AVE Temporal Layer Trigger

## Event
- **Date:** 2026-11-20 18:06:21 EST
- **Device:** iPhone Duo (iPhone19,4), A20 Pro (T8160)
- **OS:** iOS 27.1 (24A94232)
- **Kernel:** xnu-13432.2.10~176/RELEASE_ARM64_T8160
- **Trigger:** AVEProbe trigger mode (120 frames, 16463 temporal layers, HEVC 1920x1080)

## Panic Details

```
panic(cpu 4 caller 0xfffffe0043d05570): Kernel data abort.
  pc 0xfffffe004102a548, lr 0xfffffe004113ec5c

  x0:  0x0000000000000059  x1:  0x0000000000000006
  x2:  0x000000000000d555  x3:  0x00000000000000c0
  FAR: 0x0000000000000059
  ESR: 0x96000046 (synchronous data abort, translation fault level 2)
```

## Panicked Process
- **PID 308: videocodecd** (video codec daemon, 19 threads)
- **Kernel Extension:** com.apple.driver.AppleAVE2 (913.43.1)

## Kext Address Mapping
- **AppleAVE2 __TEXT_EXEC base (unslid):** 0xfffffe0008780bb0
- **AppleAVE2 __TEXT_EXEC base (slid):** 0xfffffe004101cbb0
- **KernelCache slide:** 0x000000003889c000
- **Panic PC kext offset:** 0xD998

## Crashing Instruction

```
offset 0xD998:  0xa9000000  STP x0, x0, [x0, #0]
```

The code tried to store a pair of registers to the address in x0 (0x59),
which is in the NULL page (unmapped).

## Function Context (at kext offset 0xD988)

```
0xD980: dac1a7fe  XPACI x30            ; strip PAC from LR
0xD984: a9bf7bfd  STP x29, x30, [sp, -16]!  ; push frame
0xD988: 910003fd  ADD x29, sp, #0      ; set FP
0xD98C: bl <kalloc_or_lookup>          ; call function
0xD990: d503245f  CLREX                ; memory barrier
0xD994: b40000a0  CBZ x0, skip         ; if NULL, skip
0xD998: a9000000  STP x0, x0, [x0]     ; <<< PANIC (x0=0x59)
```

## Root Cause Analysis

1. The excessive temporal layer count (16463, max intended is 8) causes an OOB write
   in the AppleAVE2 kernel extension's per-layer structure array.

2. The OOB write corrupts adjacent kernel heap data, overwriting a pointer
   with a small value (0x59).

3. 0x59 = 89 decimal. In our 120-frame encode, frame 89 sets pixels[0] = 89 = 0x59.
   This strongly suggests the OOB write copies frame/encode data into kernel heap.

4. A later function call returns the corrupted pointer (0x59 instead of a valid address).

5. The CBZ x0 check catches NULL but not 0x59, so code uses it as a pointer.

6. Store to address 0x59 causes translation fault (NULL page is unmapped).

## MTE/MIE Analysis

**MTE did NOT catch the OOB write.** ESR 0x96000046 indicates a translation fault,
not a tag check failure (which would be ESR 0x11...). Possible reasons:

1. The OOB write happens within the same tag granule (unlikely for 16455 extra entries)
2. The write uses a tag-ignoring store instruction
3. The memory region doesn't have MTE tags assigned
4. The corrupted memory shares the same MTE tag by chance

## Backtrace (within AppleAVE2)

| Offset | Description |
|--------|------------|
| 0x00D998 | Panic PC — STP to corrupted pointer |
| 0x1220AC | LR — caller of panicking function |
| 0x1221F0 | Caller 1 |
| 0x073310 | Caller 2 |
| 0x07655C | Caller 3 |
| 0x077260 | Caller 4 |
| 0x1BB320 | Caller 5 — deep in encode path |
| 0x1B3E40 | Caller 6 |
| 0x1B3924 | Caller 7 |
| 0x1B2B1C | Caller 8 — entry point from IOKit |

## Exploitation Implications

1. **Heap corruption is CONFIRMED** — OOB write smashes adjacent kernel objects
2. **Data is partially controllable** — pixel buffer data appears in corrupted values
3. **MTE bypass is built-in** — the OOB write path doesn't trigger MTE
4. **Small OOB (9 layers = max+1) would give targeted corruption** — one entry past buffer
5. **Heap feng shui** — position target objects adjacent to temporal layer buffer
6. **Write primitive** — control content written past buffer to overwrite specific fields

## Files
- Panic log: `~/a20_research/panic_ave_temporal.ips` (1.8MB)
- Trigger source: `poc/ave/ave_temporal_trigger.c`
- Kernelcache: `~/a20_research/kernelcache_raw`
