# Panic Log Analysis — CVE-2026-112705 Confirmed on iOS 27.1

## Trigger
- **App:** AVEExact.app (ave_exact_trigger.c, mode "exact")
- **Parameters:** HEVC 1280x720, NumberOfTemporalLayers=16463, EncoderUsage=1, RealTime=false
- **Date:** November 20, 2026, 18:06:21 EST

## Device
- iPhone Duo (iPhone19,4), iOS 27.1 (24A94232)
- A20 Pro (T8160), arm64e
- SPTM-820.0.22, TXM loaded

## Panic Details
```
panic(cpu 4 caller 0xfffffe0043d05570): Kernel data abort.
  pc:  0xfffffe004102a548
  lr:  0xfffffe004113ec5c
  far: 0x0000000000000059
  esr: 0x0000000096000046  (sync data abort, translation fault L2)
```

## Register State
```
x0:  0x0000000000000059  ← faulting value (near-NULL from OOB corruption)
x1:  0x0000000000000006
x2:  0x000000000000d555
x3:  0x00000000000000c0
x4:  0xf8fffe2da492edc0  ← GEN2 zone pointer (tagged)
x5:  0x00000000002e7df5
x8:  0x0000000000000006
x11: 0xfffffe12d07499e0  ← metadata zone
x15: 0xfafffe1c52db86d8  ← RO zone pointer (tagged)
x16: 0x2badfe0044028660  ← PAC-signed pointer
x17: 0xfffffe0044028600  ← kext code pointer
x20: 0xfffffe0040872b50  ← kernel data
x23: 0xf4fffe8a8a5d47b0  ← GEN1 zone pointer (tagged)
x27: 0x000000002ba60044
```

## Faulting Kext
```
com.apple.driver.AppleAVE2 (913.43.1)
  UUID: C5A5A187-EC3D-3F4A-AAF8-F385E23B6218
  Load range: 0xfffffe004101cbb0 → 0xfffffe00411eb1d7
  Size: 0x1CE627 (~1.8MB)
```

## Crash Offsets (from AppleAVE2 base 0xfffffe004101cbb0)
- **PC offset: 0xD998** (crash instruction)
- **LR offset: 0x122_0AC** (return address)

## KASLR / Slide Information
```
KernelCache slide:      0x000000003889c000
KernelCache base:       0xfffffe003f8a0000
Kernel slide:           0x00000000388a4000
Kernel text base:       0xfffffe003f8a8000
Kernel text exec slide: 0x000000003c418000
Kernel text exec base:  0xfffffe004341c000
```

## Zone Map Layout
```
Zone map: 0xfffffe13d6000000 - 0xfffffe39d6000000
  VM    : 0xfffffe13d6000000 - 0xfffffe19a2000000
  RO    : 0xfffffe19a2000000 - 0xfffffe1c3c000000
  GEN0  : 0xfffffe1c3c000000 - 0xfffffe2208000000
  GEN1  : 0xfffffe2208000000 - 0xfffffe27d4000000
  GEN2  : 0xfffffe27d4000000 - 0xfffffe2da0000000
  GEN3  : 0xfffffe2da0000000 - 0xfffffe336c000000
  DATA  : 0xfffffe336c000000 - 0xfffffe39d6000000
  Metadata: 0xfffffe12ca010000 - 0xfffffe12d3810000
  Bitmaps : 0xfffffe12d3810000 - 0xfffffe12d5940000
```

## Panicked Process
- **Process:** videocodecd (pid 308)
- **Pages:** 1469
- **Threads:** 19
- **Thread tid:** 3046901

## Analysis

The crash occurs because:
1. NumberOfTemporalLayers=16463 is passed unchecked to the AVE coprocessor
2. The AVE coprocessor performs DMA writes based on this count
3. The DMA write overflows the allocated output buffer
4. The overflow corrupts adjacent kernel memory
5. When the corrupted memory is later dereferenced (x0=0x59), it causes a data abort

The key insight is that **x0=0x59=far** — the kernel tried to load from address 0x59, 
which is the corrupted value. This means the OOB write overwrote a pointer that was 
subsequently dereferenced. The original pointer was likely a valid kernel heap address 
that got partially overwritten by the DMA spillover.

## Implications for Exploitation

1. **DMA bypasses MIE** — confirmed by the crash occurring on A20 Pro with MTE2 active
2. **The OOB corrupts kernel heap pointers** — x0=0x59 shows pointer corruption
3. **To get a controlled write:** Need to position a known kernel object adjacent to the 
   AVE DMA output buffer, then control the OOB data to write a specific value
4. **Heap shaping:** Use IOSurface or other kernel object allocations to place targets 
   adjacent to the AVE buffer in physical memory
5. **Info leak needed:** CVE-2026-112466/112465 (AVE OOB reads) could provide KASLR 
   bypass and heap layout information

## Dependencies (from panic log)
- com.apple.driver.AppleARMPlatform (1.0.2)
- com.apple.driver.FairPlayIOKit (72.19.0)
- com.apple.iokit.CoreAnalyticsFamily (1)
- com.apple.iokit.IOSurface (402.8)
