# AVE DMA OOB → Kernel R/W Testing Plan

## When iPhone Duo Returns

### Phase 1: Per-Layer DMA Size (Critical)
Deploy AVEControlled.app, run graduated NTL values to determine overflow size.

```
# Test sequence (each may panic — reboot between tests)
AVEControlled safe       # NTL=2, baseline, should NOT crash
AVEControlled probe_8    # NTL=8, max normal, should NOT crash
AVEControlled probe_9    # NTL=9, minimum overflow — WILL IT CRASH?
AVEControlled probe_10   # NTL=10, 2 extra layers
AVEControlled probe_16   # NTL=16, small overflow
```

After each crash, pull panic log:
```
pymobiledevice3 crash pull --udid 00008160-000A4C840E400036 /tmp/panic_logs/
```

From the panic log, compute per-layer DMA size:
- Compare FAR values between NTL=9 and NTL=16
- OOB amount = FAR offset from expected buffer end
- Per-layer size = OOB amount / (NTL - 8)

### Phase 2: Silent Corruption Test
If NTL=9 does NOT crash (best case — silent corruption):

```
AVEKWrite spray         # Verify pipe spray works without trigger
AVEKWrite probe_9       # Spray + NTL=9 + check corruption
```

If pipes are corrupted without crashing:
- Read corrupted pipe data for kernel pointer leaks
- Determine which pipe index was hit (spray position → heap layout)

### Phase 3: Info Leak
Deploy AVELeak.app:
```
AVELeak baseline   # Normal encode, capture bitstream for reference
AVELeak ntl_low    # NTL=9,16,32 — compare bitstreams for extra data
AVELeak small      # 128x128 with/without NTL=9 — tiny buffer = more OOB
AVELeak compare    # Same fill, different NTL — diff the encoded output
```

Analyze encoded bitstream differences — any extra bytes from kernel heap.

### Phase 4: Controlled Write (If Silent Corruption Achieved)
1. Identify which pipe_buffer field was corrupted
2. If buffer pointer corrupted → arbitrary kernel write via write()
3. If size field corrupted → arbitrary kernel read via read()
4. Write to proc→ucred→cr_uid = 0 for root
5. Write to proc→p_csflags to clear CS_HARD|CS_KILL for code signing bypass

### Phase 5: Bootstrap
Once root + code signing bypass achieved:
1. Mount rootfs as r/w (or use rootless approach)
2. Install Procursus bootstrap
3. Install Sileo package manager

### Phase 1b: Property Scan (Determine Controllable OOB Fields)
Deploy AVEPropScan.app, vary one property at a time with NTL=9.
Compare FAR values in panic logs to find which property controls struct+0x534.

```
# Reference: original panic had FAR=0x59
AVEPropScan safe          # NTL=2, verify no crash
AVEPropScan baseline      # NTL=9, default props → reference FAR
# After each reboot:
AVEPropScan bitrate_500000
AVEPropScan bitrate_5000000
AVEPropScan gop_1
AVEPropScan gop_30
AVEPropScan profile_1     # Main profile
AVEPropScan profile_2     # Main10 profile  
AVEPropScan bframes_0
AVEPropScan bframes_2
AVEPropScan qp_10
AVEPropScan qp_40
AVEPropScan res_640x480
AVEPropScan res_320x240
AVEPropScan pixel_1       # NV12 instead of BGRA
AVEPropScan pixel_2       # P010 (10-bit)
```

Compare all FAR values:
- If FAR changes when a property changes → that property maps to struct+0x534
- If FAR is always the same → +0x534 is unrelated to these properties
  (may be a fixed offset like surface count or encoder state)

Then test deeper OOB:
```
AVEPropScan multi_10      # NTL=10, reads struct+0x538
AVEPropScan multi_11      # NTL=11, reads struct+0x53c
AVEPropScan multi_16      # NTL=16, reads deeper fields
```

Each additional layer reads 4 bytes deeper. Map the full OOB region by
comparing FAR values for increasing NTL, creating a struct field map.

### Phase 1c: Session Reuse (Control OOB Values)
Deploy AVEReuse.app. Theory: encode normally first to populate struct fields,
then change NTL mid-session. If values at +0x534 retain prefill data,
we control the DMA loop count.

```
AVEReuse safe          # NTL=4, 5 frames (no OOB, verify encode works)
AVEReuse reuse_9       # 3 frames NTL=4, then 1 frame NTL=9
# After reboot:
AVEReuse reuse_10      # 3 frames NTL=4, then 1 frame NTL=10
AVEReuse multi_10_9    # 10 prefill frames, then NTL=9
AVEReuse prefill       # NTL=8 prefill (fills entire array), then NTL=9
```

Compare panic FAR with original (0x59). If FAR changes:
- The prefill values survived at +0x534
- We can control the DMA loop count
- Setting +0x534 = 1 gives a single controlled DMA write

If NTL change is rejected mid-session:
- Destroy and recreate session (struct may retain values from allocator)
- Try different session creation order
- Use heap spray to position controlled data at +0x534 offset

## Key Parameters
- Device: iPhone Duo, UDID 00008160-000A4C840E400036
- Signing: "Apple Development: Travis Tameirao (7TJQC38PMP)"
- Build: `cd poc/ave && bash build_and_deploy.sh all`
- Panic logs: `pymobiledevice3 crash pull --udid <UDID> /tmp/panic_logs/`

## Updated Understanding (Nov 21, 2026)

### OOB Value Interpretation (from kernelcache RE, updated Nov 21)
The DMA config function at AVE2+0x13d308 interprets the OOB region as:
- **struct+0x534** = Surface type A COUNT (loop count for DMA register writes)
- **struct+0x538[i]** = Surface type A array (indexed by count at +0x534)
- **struct+0x558** = Surface type B COUNT (cascading)
- **struct+0x55c[i]** = Surface type B array
- **struct+0x57c** = Surface type C COUNT (cascading)
- **struct+0x5a0[i]** = Surface type C array

**CRITICAL NEW FINDING**: +0x534 has ZERO direct STR instructions in all of AVE2.
It is NOT heap residual — it's the VALUE COMPUTED FOR LAYER INDEX 8 during
per-layer array overflow. When NTL>8, the array at +0x514 (8 entries * 4 bytes)
overflows, and the per-layer calculation writes its layer[8] result to +0x534.

The 0x59 (89) in the original panic = per-layer value for layer 8 with NTL=16463.
**Different encoder properties will produce different layer[8] values.**
This makes Phase 1b (property scanning) the MOST IMPORTANT immediate test.

For comparison, +0x558 (Type B count) IS explicitly initialized from a config
struct (x20+0xb70 → x19+0x558), while +0x534 is only written via OOB overflow.

### Key Exploitation Insight
To get a CONTROLLED kernel write instead of a crash:
1. We need +0x534 to be a SMALL number (1 or 2) — not 89
2. The DMA destination is configured by the HW register writes using x24/x21
3. x24/x21 are set before the loop from function arguments or struct fields
4. The pipe spray strategy should work if the DMA lands in a pipe buffer page

### Why Property Scanning Still Matters
Different encoder configurations may leave different residual values at +0x534:
- A value of 0 at +0x534 → NO DMA → no crash, no corruption (safe)
- A value of 1 → single DMA → clean, controlled corruption
- A value of 89 (0x59) → 89 DMA iterations → massive heap corruption → panic

Finding a property combo that sets +0x534 = 1 would be ideal for exploitation.

## Decision Tree

```
NTL=9 crashes?
├── YES → check FAR and panic details:
│   │
│   ├── FAR = small value (0x0..0xFF)?
│   │   → Residual at +0x534 is small. This is GOOD for exploitation.
│   │   → The DMA did few iterations. Try spray to catch the write.
│   │
│   ├── FAR = large value?
│   │   → Residual is large. Too many DMA iterations.
│   │   → Try different properties to get smaller residual.
│   │
│   └── NTL=9 + pipe spray crashes?
│       ├── YES → spray not dense enough, or DMA too broad
│       │   → Try smaller resolution (less heap usage)
│       │   → Try IOSurface spray instead of pipes
│       └── NO (silent corruption) → check pipes → Phase 4!
│
└── NO (no crash) → BEST CASE — silent corruption!
    → Check pipes immediately
    → If not corrupted, increase NTL or spray count
```
