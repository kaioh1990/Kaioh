# iPhone Duo Hinge State — Attack Surface Analysis

Date: 2026-11-27
Target: iPhone Duo (A20 Pro), iOS 27.1
Context: Educational jailbreak research on user's own device

## Executive Summary

The iPhone Duo introduces a **completely new code path** from hinge sensor hardware
through IOHIDEventSystem → CoreMotion (private CMAngleManager) → UIKit
(UIHingeInteraction / onHingeChange) → app layer. This path is iOS 27.1-only
code, less than 3 months old, and already showing bugs (keyboard extensions
can't receive hinge events — Apple forum thread #847490). New code = new attack
surface.

## API Stack (Sensor → App)

```
┌─────────────────────────────────────────────┐
│ App Layer                                    │
│  SwiftUI: onHingeChange(isEnabled:_:)       │
│  UIKit:   UIHingeInteraction                │
│  Returns: DeviceHinge { angle: Angle,       │
│           status: .closed/.partiallyOpen/   │
│                   .fullyOpen }              │
├─────────────────────────────────────────────┤
│ UIKit Framework (public)                     │
│  UIHinge, UIHingeInteraction.Update         │
│  reservedRegions(kind: .division)           │
├─────────────────────────────────────────────┤
│ CoreMotion (PRIVATE)                         │
│  CMAngleManager — undocumented class        │
│  Reads hinge angle, feeds UIKit             │
│  (Confirmed by artemnovichkov/hinge CLI     │
│   reverse engineering)                       │
├─────────────────────────────────────────────┤
│ IOHIDEventSystem                             │
│  Hinge events dispatched as HID events      │
│  Vendor-specific payload (IOCF-serialized)  │
│  Device model check: iPhone19,4             │
├─────────────────────────────────────────────┤
│ IOKit / Kernel Driver                        │
│  Unknown driver name (no ioreg data public) │
│  Likely: IOHIDDevice subclass for hinge     │
│  Hall effect + accelerometer fusion         │
│  Variable torque profile (100+ components)  │
└─────────────────────────────────────────────┘
```

## Attack Vectors

### 1. IOHIDEventSystem Hinge Event Injection (HIGH PRIORITY)

**What:** Hinge state changes are delivered as HID events through IOHIDEventSystem.
The artemnovichkov/hinge simulator tool proves the protocol: it creates HID
events with vendor-specific payloads and dispatches them through the system.

**Attack:** A sandboxed app MAY be able to inject fake hinge events if the
IOHIDEventSystem client entitlement check is weak or missing for hinge events.
This is new event type code — entitlement enforcement is the most common
oversight in new IOKit paths.

**Primitive:** State confusion in any app/system service that trusts hinge state.
Camera pipeline reconfiguration on a fake fold event could trigger:
- AVCaptureSession teardown/setup race
- Display configuration change mid-frame
- IOSurface reallocation for new display geometry

**Test:** From our exploit app, try IOHIDEventSystemCreate() and dispatch a
vendor hinge event. If it's accepted without entitlement → injection works.

### 2. Camera Pipeline Fold Transition Race (HIGH PRIORITY)

**What:** The iPhone Duo camera has unique fold-aware features:
- Dual Capture (front + rear simultaneous, 4K/30fps)
- Tent mode video recording (phone stands on its own)
- Duo FaceTime (outer display camera)
- Mode switching on fold/unfold

**Attack:** When the hinge state changes during active recording, the camera
pipeline must reconfigure — potentially switching between:
- Single camera → dual camera
- Inner display viewfinder → outer display viewfinder
- Portrait orientation → landscape (tent mode)

Each transition involves:
- AVCaptureSession stop/reconfigure/start
- IOSurface reallocation for new resolution/format
- Display server reconfiguration
- Possibly MediaServerd XPC calls

**Race window:** If hinge events fire rapidly (or are injected), the camera
pipeline may not fully tear down before the new configuration starts → UAF,
double-free, or use-of-stale-IOSurface.

**Precedent:** CVE-2024-44068 (Samsung) — UAF in cameraserver process via
race condition, used for privilege escalation in the wild.

### 3. Display Configuration TOCTOU (MEDIUM PRIORITY)

**What:** Folding/unfolding changes the active display configuration:
- Folded: outer display only (small)
- Partially open: inner display only (large, with reserved hinge region)
- Tent mode: both displays potentially active

**Attack:** The display server must reconfigure IOSurface framebuffers when
the display geometry changes. If the hinge state is checked, then the display
is reconfigured, but the hinge state changes between check and use:
- Framebuffer allocated for wrong display size
- Reserved region computed for wrong fold angle
- OOB write into undersized framebuffer

**Precedent:** CVE-2026-23786 (Samsung Exynos) — TOCTOU race in DPU HDR
driver causing heap overflow. CWE-367. The DPU validates display data, then
uses it later without rechecking. EXACTLY the same bug class applies to
Apple's display pipeline during fold transitions.

**Related:** CVE-2026-23787 (Samsung Exynos DPU UAF) and CVE-2026-23790
(Samsung DPU double-free) — same display processing subsystem, same batch.
Display pipelines during fold transitions are a proven vulnerability class.

### 4. CMAngleManager Private API Abuse (MEDIUM PRIORITY)

**What:** CoreMotion's CMAngleManager is a private, undocumented class that
reads the raw hinge angle. UIKit's UIHinge reads through this.

**Attack:** If CMAngleManager has XPC or Mach port interfaces that are:
- Not entitlement-checked (new code, easy oversight)
- Not input-validated (angle values out of expected range)

Then a sandboxed app could:
- Feed bogus angle values to trigger edge-case code paths
- Send malformed serialized data to crash the service
- Race angle updates against camera/display reconfiguration

**Test:** class-dump CoreMotion.framework on iOS 27.1, look for
CMAngleManager methods. Try instantiating from sandboxed app.

### 5. Reserved Region Geometry Confusion (LOWER PRIORITY)

**What:** UIView.reservedRegions(kind: .division) reports the physical fold
area. Apps use this for layout. Forum posts show developers struggling with
the API — changes don't always fire, behavior differs between views.

**Attack:** If reservedRegion geometry is spoofable or inconsistent:
- App renders content into the reserved (hinge) area
- Touch events in reserved area may be misrouted
- Accessibility features may expose content that should be hidden

Not directly a kernel exploit vector, but could enable UI-layer attacks.

## Samsung Foldable CVE Precedents

| CVE | Component | Bug Class | Relevance to iPhone Duo |
|-----|-----------|-----------|------------------------|
| CVE-2026-23786 | Exynos DPU HDR | TOCTOU race → heap overflow | Display pipeline fold transitions |
| CVE-2026-23787 | Exynos DPU HDR | UAF | Same display pipeline |
| CVE-2026-23790 | Exynos DPU | Double-free | Display pipeline memory mgmt |
| CVE-2024-44068 | cameraserver | UAF race → code exec (ITW) | Camera pipeline on fold change |
| CVE-2025-21042 | Image processing | OOB write (zero-click, ITW) | Media processing generally |
| CVE-2026-20971 | Knox PROCA | UAF (8 years undetected!) | New security subsystems hide old bugs |
| CVE-2025-21029 | System UI | Cover display permission bypass | Fold-specific UI state confusion |

**Key pattern:** Every Samsung foldable generation has shipped with display
pipeline and camera pipeline bugs related to fold state transitions. Apple's
implementation is new and likely has similar classes of bugs.

## Additional Findings from Child Sessions

- **DeviceHinge.angle** is `Angle` type (SwiftUI), not CGFloat — confirmed from primary Apple doc
- **ArrangementView** (SwiftUI): manages primary/secondary views across the fold
- **ReservedRegion kinds**: `.division` (fold crease, active only when folded) vs `.occlusion` (cameras etc)
- **UIHingeInteraction** confirmed via Flutter `duo_flex` plugin: attaches to app's UIWindow
- **Pre-release beta strings**: `foldState`, `angleDegrees`/`mechanicalAngleDegrees` found in iOS 27 beta
- **Samsung hinge permission model**: Galaxy Z TriFold requires signature|privileged permission for raw hinge angle; public API reports only 0.0 or 180.0 (no fine-grained access without privilege)
- **"Calif" iPhone Duo jailbreak**: MacObserver + aggregators report launch-day jailbreak demo by security firm Calif. Single-sourced — needs independent verification. Exploit details undisclosed, no Secure Enclave break shown.
- **No Samsung hinge-specific CVEs exist** in public record. All Samsung foldable CVEs are in adjacent subsystems (DPU, camera, Knox), not hinge sensor drivers directly.

## Known iOS 27.1 Hinge Bugs (Already Reported)

1. **Keyboard extensions can't receive hinge events** (Apple Forum #847490)
   - UIHingeInteraction and onHingeChange both fail in keyboard extensions
   - Implies hinge event delivery has entitlement or process-type filtering
   - Filtering bugs = potential bypass vectors

2. **reservedRegions inconsistency** (Apple Forum #848048)
   - Developers report layout changes don't always fire
   - Implies the notification path has race conditions or dropped events

3. **Hinge angle units confusion** (some say radians, API uses CGFloat)
   - If kernel reports in one unit and userspace interprets as another,
     out-of-range values could reach code paths

## Recommended Test Rounds

### Round 521: IOHIDEventSystem Hinge Event Injection Probe
- Create IOHIDEventSystem client
- Try dispatching vendor hinge events (payload from artemnovichkov/hinge)
- Check if accepted without special entitlements
- If yes → we can trigger fold/unfold events at will

### Round 522: Camera Pipeline Fold Stress Test
- Start AVCaptureSession recording
- Rapidly inject hinge open/close events (if round 521 works)
- Or: rapidly call hinge-dependent APIs while recording
- Monitor for crashes, assertion failures, or stale pointers

### Round 523: Display Configuration Race
- Monitor display configuration changes during fold events
- Try to trigger IOSurface reallocation during active rendering
- Check for OOB or use-after-reallocation

### Round 524: CMAngleManager Enumeration
- class-dump CoreMotion on device
- Find CMAngleManager methods and XPC endpoints
- Test instantiation from sandboxed app
- Try out-of-range angle injection

## Sources

- [Apple: onHingeChange](https://developer.apple.com/documentation/swiftui/view/onhingechange(isenabled:_:).md)
- [Apple: UIHingeInteraction](https://developer.apple.com/documentation/uikit/uihingeinteraction.md)
- [Apple: DeviceHinge](https://developer.apple.com/documentation/swiftui/devicehinge.md)
- [Apple Forum: Hinge listeners in keyboard extensions](https://developer.apple.com/forums/thread/847490)
- [Apple Forum: reservedRegions from renderer](https://developer.apple.com/forums/thread/848048)
- [Apple Forum: Low-latency hinge gameplay](https://developer.apple.com/forums/thread/847866)
- [Apple: Leverage multiple displays (Tech Talk)](https://developer.apple.com/videos/play/tech-talks/111464/)
- [artemnovichkov/hinge CLI tool](https://github.com/artemnovichkov/hinge)
- [iPhone Duo dev guide (dev.to)](https://dev.to/arshtechpro/iphone-duo-for-ios-developers-what-actually-changes-in-your-swift-code-5gc5)
- [iOS 27.1 new APIs (classmethod)](https://dev.classmethod.jp/en/articles/ios27-1-iphone-duo-new-apis/)
- [6 foldable APIs (slekens.dev)](https://slekens.dev/en/blog/iphone-duo-xcode-27-1-six-foldable-apis/)
- [iPhone Duo camera (MacRumors)](https://www.macrumors.com/guide/iphone-duo-camera/)
- [CVE-2026-23786 Exynos DPU TOCTOU](https://stack.watch/vuln/CVE-2026-23786/)
- [CVE-2026-20971 Samsung Knox PROCA](https://www.jamf.com/blog/cve-2026-20971-samsung-knox-proca-vulnerability/)
- [CVE-2024-44068 cameraserver UAF](https://www.securityweek.com/google-warns-of-samsung-zero-day-exploited-in-the-wild/)
- [Apple unveils iPhone Duo](https://www.apple.com/newsroom/2026/09/apple-unveils-iphone-duo/)
