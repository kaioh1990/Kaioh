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

## Key Parameters
- Device: iPhone Duo, UDID 00008160-000A4C840E400036
- Signing: "Apple Development: Travis Tameirao (7TJQC38PMP)"
- Build: `cd poc/ave && bash build_and_deploy.sh all`
- Panic logs: `pymobiledevice3 crash pull --udid <UDID> /tmp/panic_logs/`

## Decision Tree

```
NTL=9 crashes?
├── YES → per-layer size known from panic, increase spray density
│   NTL=9 + spray crashes?
│   ├── YES → try NTL=10 (more spray), or switch to IOSurface target
│   └── NO (silent corruption) → Phase 4
└── NO (no crash) → silent corruption! → check pipes immediately
    Pipes corrupted?
    ├── YES → identify corrupted field → kernel R/W
    └── NO → increase spray count, retry with NTL=10, 16
```
