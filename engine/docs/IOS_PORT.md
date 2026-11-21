# iOS / iPadOS port of the native C engine (design)

Status: implemented up to a signed device build, 2026-11-20 (branch
`claude/native-ios`). M0, M1, M3 and M4 are done, M2 and M5 in part; nothing
has run on a device yet. See "Implementation status" at the end; the design
sections below are kept as written, with notes where the implementation
differs.

Goal: replace the Go engine (`libikemen.a` + MoltenVK) inside the current
"CvF Online" iOS app with the native C engine. (PM, round 2: for now the
native app installs *next to* the Go app, as "CvF Native",
`com.kaioh1990.cvfnative`; see Implementation status, M7.) The app must keep the
current bundle id (`com.kaioh1990.cvfonline`) so that a Sideloadly install
updates the installed app in place, and the player's `Documents/Game`,
`Documents/save` and settings survive.

## 1. Summary of decisions

| Topic | Decision | Main reason |
| --- | --- | --- |
| Build | CMake (built-in iOS support) builds one static library, pre-linked into a single object, shipped as `CvfEngine.xcframework` (device arm64 + simulator arm64). The Xcode project links it the way it links `libikemen.a` today. | One place for the determinism flags; Xcode never indexes the 9.8 MB `ik_sim.c`; same integration as today's Go archive. |
| Symbols | `-fvisibility=hidden` + `ld -r -exported_symbols_list`: only the host API (section 5) and `SDL_main` are exported. | The generated names (`os_Args`, `url_Parse`, `gl_*`, `sdl_*`...) must not clash with SDL2, ZipArchive, zlib or vInputKit. |
| Renderer | OpenGL ES 3.0 (Apple EAGL, through SDL2), by translating Ikemen's existing `render_gles32.go` / `font_gles32.go` (already used by Android and the Vita build) with a `#version 300 es` header. Not desktop GL 3.3 (iOS has none), not Vulkan/MoltenVK for now. | Smallest change; works in the iOS Simulator; GLES 3.0 on Apple GPUs already runs on Metal internally. Vulkan via MoltenVK stays the fallback (section 4). |
| Engine thread | Unchanged from today's app: the engine runs on the main thread, entered from a run-loop timer (`performSelector:afterDelay:0`), never from a main-queue GCD block. | Lesson learned with CoreBluetooth / `dispatch_after` starvation. SDL2's UIKit back end needs the main thread anyway. |
| Threads and GC | Only the engine (main) thread touches collector memory. Callbacks from other threads (Nearby relay queue, vInputKit, HTTP) go through a mutex-protected mailbox drained once per frame in `cvfGcSafepoint`. | The collector is single-threaded and scans only the engine thread's stack and its goroutine stacks. |
| Game data | Same folder logic as today (`Documents/Game` or `<app>/Game`, symlinked into `Application Support/CvF`, `save` -> `Documents/save`). Engine scripts come from the app's `DefaultAssets.zip`, built from the native engine's own Ikemen tree. | Saves and the Game folder survive; scripts always match the engine. |
| App shell | Keep the Jesuszilla/ikemen-ios wrapper and the CvF patches (overlay, gestures, vInputKit glue, Nearby relay, name box, icon, 120 Hz). Swap the engine library; delete the MoltenVK/Metal-layer code. | The Objective-C side is proven on device; only the engine underneath changes. |

## 2. Build

### 2.1 Static library through CMake, not sources in Xcode

`engine/CMakeLists.txt` gains an `if(IOS)` branch (`CMAKE_SYSTEM_NAME=iOS`):

- builds `cvf_core` + `cvf_ik` + the new `src/platform/ios/*.c`,
  `src/platform/gles/*.c` and `src/platform/apple/*.m` (`enable_language(OBJC)`,
  `-fobjc-arc`);
- does not build `cvf`, `cvf_headless`, `cvf_tests`, `cvftool` (no
  executables on iOS; the headless runner is reached through `SDL_main`
  arguments instead, see M3);
- needs only SDL2 headers: `CVF_SDL2_INCLUDE_DIR` =
  `SDL2.xcframework/ios-arm64/SDL2.framework/Headers` (an INTERFACE include,
  no `CVF_SDL2_LIBRARY`); the app links SDL2 as it does today;
- defines `CVF_IOS=1` and `CVF_GC_DEFAULT_ARENA_MB=1024` (section 3.5).

New script `engine/tools/package/ios_build.sh`:

```
for sdk in iphoneos iphonesimulator:
  cmake -S engine -B build-ios-$sdk -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCVF_SDL2_INCLUDE_DIR=...
  cmake --build build-ios-$sdk --target cvf_engine_objs
  ld -r -arch arm64 -exported_symbols_list engine/src/platform/ios/exports.txt \
     <all .o of cvf_core, cvf_ik, platform> -o cvf_engine.o      # master object
  libtool -static -o libcvf_engine.a cvf_engine.o
xcodebuild -create-xcframework \
  -library build-ios-iphoneos/libcvf_engine.a -headers engine/src/platform/ios/include \
  -library build-ios-iphonesimulator/libcvf_engine.a -headers engine/src/platform/ios/include \
  -output CvfEngine.xcframework
dsymutil / keep the unstripped .o for symbolication
```

Why not add the sources to the Xcode project:

- Xcode's per-target flags would have to reproduce `CVF_DETERMINISM_FLAGS`
  by hand, and nothing would notice when they drift (Xcode's defaults are
  not ours: strict aliasing is on, `GCC_C_LANGUAGE_STANDARD` is not gnu17).
- Indexing and re-compiling `ik_sim.c` (188k lines) inside the app project
  makes every Objective-C edit slow.
- The wrapper already consumes a prebuilt engine library (`libikemen.a`);
  replacing one library with another keeps the project diff small.

A plain `.a` instead of the master object would also work, but then every
generated global symbol is visible to the final link. `-fvisibility=hidden`
alone does not stop duplicate-symbol errors between static objects; the
`ld -r` step turns hidden symbols into local ones. The exports list is short
(section 5.1).

### 2.2 Compiler flags

Same as desktop, from `CVF_DETERMINISM_FLAGS`:
`-ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing`, plus
`C_EXTENSIONS ON` (gnu17: statement expressions, `typeof`) for `cvf_ik`, and
`-w` for generated code. Apple clang contracts `a*b+c` into FMA on arm64 by
default, so `-ffp-contract=off` is the important one. iOS arm64 uses the
same compiler and the same Darwin arm64 ABI as the macOS arm64 build, which
already reproduces the Linux reference dump byte for byte. That build is the
best stand-in for the device.

Never enable for the engine: ASan without the existing
`detect_stack_use_after_return=0` default, `-ffast-math`, LTO across the
Objective-C app (keeps the flag boundary clean), Xcode's "Thread Sanitizer".

### 2.3 Go-side changes and regeneration (in the patched Ikemen tree)

The translation is generated once for all platforms (GOOS=linux,
`GOTOC_TAGS=cvf`, cgo off) and decides the OS at run time through
`ik_goos()`, which already returns `"ios"` on iOS. Files with `//go:build
ios` or with `import "C"` (`util_ios.go`, `cvf_p2p_ios.go`, the iOS exports)
are therefore not translated. Changes to `tools/ikemen_headless.patch` and
`tools/ikemen/`:

1. `render_gles32.go`, `render_gles32_batch.go`, `font_gles32.go`: build
   tag `android || vita || cvf`, so the translation contains both renderers
   (they share no type names). The shader header becomes `#version 300 es`
   plus default precisions when `runtime.GOOS == "ios"`.
2. `selectRenderer` (cvf build): accept `"OpenGL ES 3.0"` ->
   `Renderer_GLES32`.
3. `main.go` / `system_sdl.go` iOS branches, under `cvfEngine`: force
   `RenderMode = "OpenGL ES 3.0"` instead of `"Vulkan 1.3"` (an old
   `save/config.ini` may still say Vulkan), set the SDL GL attributes for
   an ES 3.0 context (as the Android branch does for 3.2, with depth 24 and
   no alpha), create the window with `SDL_WINDOW_OPENGL` instead of
   `SDL_WINDOW_VULKAN`, and disable model shadows (they need a geometry
   shader, which ES 3.0 lacks).
4. New `tools/ikemen/cvf_mobile.go` (`//go:build cvf`, no cgo): the touch,
   Nearby and name-box logic from the Go app's
   `patches/ikemen-cvf-online.patch` (`cvfStoreTap`, `cvfTouchMenus`,
   `cvfTouchUI` for Lua, `cvfInMatch`, `cvfNearby.setP2P`,
   `cvfNameAskDone`), written as plain Go. The functions that call the app
   (`cvfP2PAdvertise`, `cvfP2PAdvertiseStop`, `cvfP2PBrowse`, `cvfP2PJoin`,
   `cvfP2PClose`, `cvfNativeAskName`, `cvfP2PAvailable` as a function) have
   stub bodies and are listed in `GOTOC_NATIVE_FUNCS`, so C implements them
   (section 5.2).
5. `run_reach.sh`: add the entry points the app calls as reach roots
   (`reach:main,cvfHeadlessMain,cvfStoreTap,cvfInMatch,cvfNameAskDone,...`)
   and the native functions above. `pkgs.sh`: add
   `github.com/leonkasovan/gl/v3.2/gles2` to `GOTOC_BIND_PKGS`.

After regeneration the headless reference must still give the same md5
(54f8ae06... with the old rules, 4334350c... with cvf-online-5) on Linux and
macOS. This is the gate for M0.

The online parts these files call (lobby, Nearby list, `cvf_online.go`) need
the online port that PORTING.md lists as still to do (`net/http`
placeholders). Touch and the name box do not depend on it. Nearby (P2P) does.

## 3. Runtime on iOS

### 3.1 Conservative GC: roots

Current state (`core/gc.c`): roots = `__DATA,__data`, `__DATA,__bss`,
`__DATA,__common` of `_dyld_get_image_header(0)`; the engine thread's stack
from the current sp up to `pthread_get_stackaddr_np`; registers through
`setjmp`; suspended goroutine stacks through `gc_extra_roots_hook`.

Changes:

- **Find the engine's own image, not image 0**: use `dladdr(&gc_init)` to
  get the Mach-O header containing the collector. With the static
  library this is the app executable (same as image 0 today). It stays
  correct if the engine ever moves into a framework or the app gains an
  extension.
- **Scan every writable segment**: walk the `LC_SEGMENT_64` load commands
  and add each section of each segment with `VM_PROT_WRITE` in `initprot`
  (`__DATA`, `__DATA_DIRTY`, `__DATA_CONST`), adding
  `_dyld_get_image_vmaddr_slide`. Skip `S_THREAD_LOCAL_*` sections and
  skip zero-length ones. Newer Apple linkers can place writable data
  outside `__DATA,__data/__bss/__common` (e.g. a `__DATA_DIRTY` segment).
  A global that is missed would let a live object be freed, a bug that
  shows up late and at random. Scanning a few extra KB of
  Objective-C/SDL data costs nothing measurable.
- **Startup self-check** (all platforms, `gc_selftest()` in debug and on a
  `--selftest` launch): store a fresh object only in a global in each
  section kind (initialised data, zero-init, tentative), collect, and check
  that each survived (canary bytes intact, not on a free list). Run in M2
  (macOS) and M3 (Simulator).
- **Main stack**: `find_stack_base` on Apple ignores the hint and uses
  `pthread_get_stackaddr_np`. That is right for the main thread. Change the
  hint to win when given, so that 3.3's fallback can work.
- **Owner-thread check**: record `pthread_self()` in `gc_init`; in debug
  builds `gc_alloc*`/`gc_collect` abort if called from another thread. This
  turns the "only the engine thread allocates" rule (section 1) into a
  crash you can see, instead of heap corruption.

Other threads in the process (SDL audio, CoreBluetooth, Network.framework
queues, vInputKit, NSURLSession) are not scanned. That is correct as long as
they never hold the only reference to a collector object. Strings handed to
Objective-C are always `malloc` copies (as cgo's `C.CString` did), and
strings coming back are copied into collector memory on the engine thread
(mailbox, section 5.3).

### 3.2 Goroutine stacks

`core/sched.c`: cooperative goroutines on one thread, hand-written arm64
`ctx_switch` (saves x19–x30, d8–d15; does not touch x18, the
platform-reserved register on Apple, and needs no pointer authentication:
third-party iOS apps are plain arm64, not arm64e). 2 MB stacks from
`malloc`.

Changes:

- Allocate goroutine stacks with `mmap` and put a `PROT_NONE` guard page
  below each one (`mprotect` is allowed on iOS; nothing is made
  executable). Today an overflow silently overwrites the neighbouring heap
  block; with a guard page it crashes at the faulting frame. Same change on
  all POSIX platforms.
- Keep 2 MB (the character compiler recurses deeply). Darwin hands out
  big `mmap` regions zero-fill on demand, so unused stack costs no memory.
- Do not call UIKit/SDL video functions from a goroutine other than the
  main one. In the `cvf`/`vitaPerf` build graphics tasks are synchronous on
  the main goroutine and the loader goroutine only reads files. Add a
  debug assert in the `sdl_PollEvent` and `sdl_Window_GLSwap` bindings that the
  current goroutine is the main one.

### 3.3 Main thread stack size (the one real unknown)

The main goroutine runs on the main thread's stack (`main_coro.stack ==
NULL`). On iOS that stack is 1 MB (8 MB on macOS), and UIKit's frames, the
run-loop timer callout and every UIKit callback during `SDL_PumpEvents` sit
on it too.

- **Measure first (M2, on the Mac):** link the macOS `cvf` with
  `-Wl,-stack_size,0x100000` (1 MB main stack). Also log the deepest sp seen
  at each `cvfGcSafepoint` and in the event-poll binding
  (`CVF_STACK_REPORT=1`). Then play intro -> menus -> arcade match ->
  training -> online lobby screens. If peak use stays under ~600 KB, keep
  the main stack (plan A).
- **Plan B if it does not fit:** in `SDL_main`, `mmap` an 8 MB stack with a
  guard page and switch the main goroutine onto it (a one-way
  `ctx_switch` to a trampoline that calls `fn_main`), registering its top
  with the collector via the hint from 3.1. UIKit callbacks during the
  event pump then run on that stack. That works in general, but code that
  compares sp with `pthread_get_stackaddr_np` can misbehave, so it is plan
  B only.

### 3.4 Panic frames (setjmp/longjmp)

81 functions with `defer`/`recover` push a `GoPanicFrame` with `setjmp`. On
Darwin `setjmp` also saves the signal mask (a system call). Use
`_setjmp`/`_longjmp` on `__APPLE__` (a two-line macro in `gort.h`/`gort.c`).
Same semantics (the engine uses no signals), much cheaper. This also speeds
up the macOS build. `longjmp` across Objective-C frames never happens:
panics are raised and caught inside translated code.

### 3.5 Memory

- The collector reserves a 4 GB arena (`MAP_NORESERVE`) on 64-bit, plus
  page and mark tables. iOS gives an app far less virtual address space than
  macOS. Without the `extended-virtual-addressing` entitlement (which a
  sideloaded build should not depend on), a 4 GB reservation can fail or
  crowd out the GL driver and dyld. Default on iOS:
  `CVF_GC_DEFAULT_ARENA_MB=1024` (compile-time default; `CVF_GC_ARENA_MB`
  still overrides it). Measured live set ~30 MB, peak RSS 78–127 MB per
  match, so 1 GB leaves plenty of room.
- Jetsam limits on current iPhones are well above ~200 MB. Keep
  `CVF_GC_FIGHT_BUDGET_MB` at the 64-bit default (256 MB) and lower it if M7
  shows memory warnings. Hook `UIApplicationDidReceiveMemoryWarning` to
  request a collection at the next safepoint outside a fight.
- `getenv` tuning knobs exist on iOS only when started from Xcode. For a
  sideloaded build, optionally read `save/cvf_env.txt` (`KEY=VALUE` lines)
  in `SDL_main` before `gc_init`.

### 3.6 W^X / JIT

None. Everything is compiled ahead of time; gopher-lua is an interpreter;
the collector maps only RW memory; `ctx_switch` is in `__TEXT`. No
`MAP_JIT`, no `get-task-allow` dependence. iOS's code-signing rules impose
nothing new.

### 3.7 Background / foreground

GL calls from a backgrounded app get the process killed
(`gpus_ReturnNotPermittedKillClient`). The Go app used Metal/MoltenVK, which
forgives this more, so the native app needs explicit handling:
`ios_main.c` installs an `SDL_AddEventWatch` for
`SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERFOREGROUND`. While in the
background the swap binding (`sdl_Window_GLSwap`) does no GL work. It loops
on `SDL_WaitEventTimeout` until the app is in the foreground again, so the
frame loop stalls instead of drawing. An online match would time out. That
is acceptable and matches what desktop does on window minimise. Audio is
paused/resumed by SDL.

### 3.8 Networking

- `ik_net.c` (BSD sockets, non-blocking): works on iOS unchanged. The local
  network prompt and Bonjour keys are already in Info.plist
  (`customize.sh`).
- `ik_http.c` loads libcurl with `dlopen` on Apple, and iOS has no libcurl.
  Add `src/platform/apple/http_nsurl.m`, an NSURLSession back end with the
  same job struct. It fills the job's `malloc` buffers from the session's
  delegate queue, sets `done`, and the waiting goroutine polls as today.
  Use it on macOS too, so the macOS app no longer needs libcurl.

## 4. Rendering

| Option | Pros | Cons |
| --- | --- | --- |
| **GLES 3.0 via EAGL (chosen)** | `render_gles32.go` already exists and has run on GLES 3.2 (Android) and a GLES2-class driver (vitaGL); ~96 GL entry points, almost all shared with `gl_bind.c`; runs in the iOS Simulator; on Apple GPUs GLES is implemented on top of Metal. | Deprecated since iOS 12 (still shipped in iOS 26); no geometry shaders, `glMapBuffer`, `glFramebufferTexture`, or multisample textures in ES 3.0. |
| Desktop GL 3.3 shaders | – | Not available on iOS at all. |
| ANGLE (Metal back end) | Real GLES 3.0 on Metal, maintained by Google/Apple (WebKit); same renderer code. | SDL2's UIKit back end only creates EAGL contexts. ANGLE would need EGL plumbing in SDL (SDL3 has it) and a ~10 MB framework. Worth it only if Apple removes OpenGL ES. |
| Vulkan via MoltenVK | What the shipping Go app uses; known to work on these devices; Metal underneath. | `render_vk.go` uses the cgo vulkan-go bindings: hundreds of entry points and structs to bind by hand, SPIR-V shader pipeline, no Simulator. Much more work than GLES. |
| Hand-written Metal renderer | Best long-term performance and control. | A third renderer to keep in sync with Ikemen's; too big for the first port. |

Recommendation: GLES 3.0 now. Write the bindings so the renderer does not
know which GL is underneath. If OpenGL ES is ever removed or performance
falls short, the next step is ANGLE (same renderer and bindings, new
context code), then MoltenVK.

Implementation (`src/platform/gles/gles_bind.c`, binding package
`github.com/leonkasovan/gl/v3.2/gles2`):

- On iOS, call the OpenGLES.framework symbols directly (`<OpenGLES/ES3/gl.h>`,
  static linking, no `GetProcAddress`); on other platforms keep the
  `SDL_GL_GetProcAddress` loader used by `gl_bind.c` (same file can serve
  Android later).
- ES 3.0 gaps: `MapBuffer` -> `glMapBufferRange(..., GL_MAP_WRITE_BIT |
  GL_MAP_INVALIDATE_BUFFER_BIT)`; `FramebufferTexture` ->
  `glFramebufferTexture2D(GL_TEXTURE_2D)`; `GEOMETRY_SHADER` compile ->
  error (model shadows disabled, section 2.3); `ProgramParameteri` exists in
  ES 3.0; multisampling via `glRenderbufferStorageMultisample` +
  `glBlitFramebuffer` (both ES 3.0) or `MSAA=0` on iOS.
- Shaders: compile every embedded GLES shader with `glslangValidator`
  (`--client opengl100`, ES 300 profile) in the Linux container as a CI
  check (M0). Fix any 3.1/3.2-only constructs in the Ikemen tree.
- SDL creates the `CAEAGLLayer` view inside its own UIWindow with
  `SDL_WINDOW_ALLOW_HIGHDPI`; the engine sizes its viewport from
  `SDL_GL_GetDrawableSize`. If 3x drawables cost too much GPU time at 120 Hz,
  drop `SDL_WINDOW_ALLOW_HIGHDPI` (1x-point drawable, upscaled by Core
  Animation) or render the final blit at 2x (decide in M7 by measuring).
- 120 Hz: keep `CADisableMinimumFrameDurationOnPhone`; the engine's
  `Framerate`/`VSync` settings pace the loop. Verify on a ProMotion device
  in M7 (frame-time log in `save/logs`).

## 5. Platform glue: what is reused, how it binds

### 5.1 Reused as-is or nearly

| File (ikemen-ios + CvF patch) | Native app | Notes |
| --- | --- | --- |
| `main.m`, `AppDelegate.m`, `SceneDelegate.m`, storyboards, icon assets, `Localizable.xcstrings` | unchanged | |
| `ViewController.m` | small edits | Game-folder detection, symlinks, `installEngineFilesAtRoot`, run-loop-timer start, SDL hints (`JOYSTICK_MFI` off with vInputKit, controller DB, background events) stay. `#import "libikemen.h"` becomes `#import "cvf_host.h"`; the `MetalContainerView` lookup and the `viewWillTransitionToSize` Metal-layer resize hack go away (SDL's GL view resizes itself; the engine handles `SDL_WINDOWEVENT_SIZE_CHANGED`). `SDL_main(5, argv)` with `--assets <root> --screen WxH` is kept. |
| `SDLWindowFix.m/.h`, `GoWindow.h` | trimmed | Keep `IkemenGetSDLWindow` and the orientation lock; delete `IkemenAttachMetalLayer` / `GetMoltenVKWindowID` (Vulkan only). |
| `TouchOverlayView.m/.h`, `TouchOverlayViewController.m`, `DPadView.m`, `AnalogJoystickView.m`, `VirtualGamepadButton.m` | unchanged | Swipe/tap/double-tap/three-finger gestures and the SDL virtual joystick (`SDL_JoystickAttachVirtual`, `SDL_JoystickSetVirtual*`) are SDL calls, independent of the engine. They call `CvfTouchTap`, `CvfSetTouchMenus`, `CvfInMatch`, now provided by `cvf_host.c` under the same names. |
| `CvfVInput.m/.h` (vInputKit glue) | unchanged | Talks only to the vInputKit API (`vik_ios_start`, `vik_ios_set_frame_callback`, `vik_ios_present_configurator`, ...) and the overlay's virtual joystick. vInputKit stays a private, separately built xcframework linked only when `VIK_XCFRAMEWORK` is set (as in `customize.sh`). Simulator builds go without it unless it has a simulator slice. |
| `CvfP2P.m` (Nearby relay, Network.framework) | unchanged | It declares `CvfP2PPeers`, `CvfP2PRegister`, `CvfNameEntered` as `extern`; `cvf_host.c` exports exactly these names and signatures. Its relay uses loopback sockets to the engine's normal netplay ports, which `ik_net.c` provides unchanged. |
| Native name box (in `CvfP2P.m`) | unchanged | Reached through the `askName` slot of the registered function table. |
| Info.plist keys (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`, `CADisableMinimumFrameDurationOnPhone`, local network, Bonjour) | unchanged | |

Removed from the project: `libikemen.a`, the ffmpeg/libvpx/libxmp archives
the Go engine needed, `MoltenVK.xcframework`, `Metal.framework` if nothing
else needs it (SDL2 may; keep it if the link complains). Added:
`CvfEngine.xcframework`, `OpenGLES.framework`.

### 5.2 Host API (`engine/src/platform/ios/include/cvf_host.h`)

Same C names and signatures as the Go `//export`s, so the Objective-C does
not change:

```c
int  CvfInMatch(void);                 // engine thread only (UIKit touch callbacks)
int  CvfTouchTap(float x, float y);    // engine thread only; 0 = screen ignores taps
void CvfSetTouchMenus(int on);         // any thread (atomic flag, read each frame)
void CvfP2PRegister(void *funcs);      // any thread; copies the CvfP2PFuncs table
void CvfP2PPeers(char *json);          // any thread; mailbox
void CvfNameEntered(char *name);       // any thread; mailbox
int  SDL_main(int argc, char **argv);  // engine entry (src/platform/ios/ios_main.c)
```

`CvfP2PFuncs` keeps the layout from `cvf_p2p_ios.go` (`advertise`,
`advertiseStop`, `browse`, `join`, `close`, `askName`). The native
functions from section 2.3 (`fn_cvfP2PJoin` etc.) call through this table
with `malloc` copies of their string arguments, freed after the call,
exactly like the cgo version.

`exports.txt` for the master object: the seven symbols above (with Darwin's
leading underscore).

### 5.3 Thread rules and the mailbox

- `CvfTouchTap` and `CvfInMatch` are called by UIKit touch handling, which
  only runs inside `SDL_PumpEvents` on the main thread, i.e. synchronously
  inside the engine loop and never during a collection. They call the
  translated `fn_cvfStoreTap` / `fn_cvfInMatch` directly (the tap needs a
  synchronous answer). A debug assert checks
  `pthread_main_np() && !collecting`.
- `CvfP2PPeers` is called from the `cvf.nearby.p2p` serial queue, and
  `CvfNameEntered` from a UIKit action. Both copy their string with
  `strdup` into a slot guarded by a `pthread_mutex` (latest value wins for
  peers; a single pending answer for the name).
  `fn_cvfGcSafepoint` (already called every frame from `System.await`)
  calls `cvf_host_drain()`, which moves pending values into collector
  strings and calls `NearbyState_setP2P` / `fn_cvfNameAskDone` on the
  engine thread.
- `CvfSetTouchMenus` writes an `_Atomic int` that the translated
  `cvfTouchMenus` read path checks (native getter).

### 5.4 `SDL_main` (`src/platform/ios/ios_main.c`)

```
SDL_main(argc, argv):
  freopen stdout/stderr -> <assets>/save/logs/cvf.log (readable in Files)
  read save/cvf_env.txt (optional)
  gc_init(&argc)
  ik_init_globals()
  parse --assets DIR  -> g_sys.baseDir   (realMain's iOS branch chdirs there)
        --screen WxH  -> g_cvfScreen     (same effect as Go's cvfParseScreen)
        --selftest    -> run gc_selftest + headless reference, write md5 to save/logs
  os_Args = ["ikemen-go", remaining args...]
  install background/foreground event watch (3.7)
  fn_main()
  return 0
```

Unlike `app/game_main.c`, it does not chdir to the executable or `.app`
parent; the iOS branch of `realMain` requires `baseDir` and changes
directory itself.

## 6. Lua scripts, DefaultAssets, config

- The engine loads `external/script/*.lua` relative to `baseDir` (the
  `Application Support/CvF` symlink farm). `installEngineFilesAtRoot`
  already replaces `external/` with the copy from the app's
  `DefaultAssets.zip` and links in anything extra from `Game/external`. That
  is exactly what the native engine needs: the scripts must match the
  engine's own Ikemen tree (6a09392 + `ikemen_headless.patch` + the CvF
  online/touch script changes), not the Go app's newer Ikemen.
- New `engine/tools/package/ios_assets.sh` builds `DefaultAssets.zip` from
  that tree: `external/script/`, `external/gamecontrollerdb.txt`,
  `save/config.ini`, `manifest.txt`, and `external/.cvf-engine` (engine git
  revision). The wrapper's existing CRC check
  (`scriptFileDiffersFromBundleAtDestinationURL`) re-extracts when the
  scripts change.
- Embedded defaults (`go:embed`: default motif, shaders) are inside the
  binary already.
- `save/config.ini`: the player's existing file is kept (never
  overwritten). Settings the native engine requires are forced in code, not
  trusted from the file: `RenderMode` (section 2.3), and the shared
  gameplay settings (`FightAspect` 0,0, see PORTING.md). Unknown keys from
  the Go app's newer Ikemen are ignored by the INI loader.

## 7. Install flow (unchanged for the player)

1. On the Mac: run `ios/native/customize-native.sh <ikemen-ios checkout>`
   (new, alongside `ios/customize.sh`). It applies the CvF patch set, then
   `native-app.patch` (5.1 edits), copies `CvfEngine.xcframework` and the
   native `DefaultAssets.zip`, and links vInputKit when `VIK_XCFRAMEWORK` is
   set. Bundle id `com.kaioh1990.cvfonline`, team 9QP5427NJJ, display name
   "CvF Online".
2. Bump `CFBundleVersion` / `CFBundleShortVersionString` (e.g. 2.0 "native")
   so the update is visible.
3. Xcode: Product > Archive (or build for "Any iOS Device"), wrap
   `Payload/<app>.app` into an IPA (or reuse `make-full-ipa.py` for the
   all-in-one variant).
4. Sideloadly: same Apple ID and the **same bundle-id settings as the last
   install**. The installed app's application-identifier has Sideloadly's
   suffix, so Xcode cannot install over it directly. Sideloadly replaces the
   app in place and iOS keeps the data container (`Documents/Game`, `save`,
   preferences) as long as the identifier matches.
5. Before the first native install: copy `Documents/save` off the device
   through Finder (backup). After install, check in Files that `Game` and
   `save` are still there before playing.

## 8. Milestones

Each is verifiable as stated; only M7 needs the device.

| # | Milestone | Where | Done when |
| --- | --- | --- | --- |
| M0 | Go-side changes (2.3) + regeneration | container | Headless md5 unchanged on Linux (54f8ae06 / 4334350c); `cvf` still runs on Linux; all GLES shaders pass `glslangValidator` for ES 300; `nm` shows `fn_cvfStoreTap`, `fn_cvfInMatch`, `Renderer_GLES32_*` in `ik_sim.o`. |
| M1 | iOS library build (`ios_build.sh`, CMake `IOS` branch, master object, xcframework) | Mac | Both slices build with Xcode's clang; `nm -gU` on the master object lists exactly the export list; a 20-line test app links it with SDL2 and no undefined symbols. |
| M2 | Runtime hardening (3.1–3.5) on desktop | Mac + container | Segment-walk roots + `gc_selftest` pass on Linux and macOS; guard-page stacks; `_setjmp`; owner-thread assert; macOS `cvf` linked with a 1 MB main stack plays intro -> menus -> match -> online menus with `CVF_STACK_REPORT` peak noted (decides plan A/B of 3.3); macOS headless md5 unchanged. |
| M3 | Determinism on the iOS toolchain | iOS Simulator | Minimal app (or the real one with `--selftest`) runs the headless reference against the game folder copied into the simulator container (`xcrun simctl get_app_container ... data`); md5 equals the Linux/macOS value. |
| M4 | App boots in the Simulator (GLES renderer, no host glue yet) | iOS Simulator | Intro, title, menus and an AI match render correctly; Simulator keyboard and on-screen overlay drive the game; rotation and background/foreground do not crash; `save/logs/cvf.log` written. |
| M5 | Host glue (5.2–5.3) | iOS Simulator | Tap-to-select menus, double-tap, three-finger gesture, name box round trip, `cvfTouchUI` toggles with a (simulated) controller; debug thread asserts silent over a long session. |
| M6 | Networking: NSURLSession HTTP, lobby, netplay | Simulator + desktop | Lobby list loads; Simulator vs desktop native netplay match on the LAN completes without desync; P2P register/browse calls reach `CvfP2P.m` (radio itself needs devices). |
| M7 | Device install and checks | iPhone/iPad via Sideloadly | Update over the existing app keeps `Game` and `save`; `--selftest` md5 matches; 120 Hz frame-time log; vInputKit controllers; Nearby match between two devices; no jetsam over a long session; native vs Go-app cross-play decision documented (see open questions). |
| M8 | (optional) Metal path | – | Only if GLES deprecation or performance demands it: ANGLE first, MoltenVK second (section 4). |

## 9. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| 1 MB main-thread stack too small | Crash with no clear cause | Measured in M2 on macOS with a 1 MB stack; plan B big-stack trampoline (3.3). |
| Missed GC root section on newer linkers | Random use-after-free | Segment walk + startup self-test (3.1). |
| A callback allocates from another thread | Heap corruption | Mailbox design; owner-thread assert in debug builds (3.1, 5.3). |
| Large VA reservation refused on iOS | Crash at start | 1 GB default arena on iOS (3.5). |
| GL work while backgrounded | App killed by iOS | Event watch blocks swaps (3.7). |
| ES 3.0 shader/feature gaps | Wrong rendering | `glslangValidator` CI in M0; ES 3.0 fallbacks in `gles_bind.c`; model shadows off. |
| OpenGL ES removal by Apple | Future breakage | Renderer-agnostic bindings; ANGLE then MoltenVK (section 4). |
| Rollback resimulation cost on older devices (~1 ms/tick on M1; netplay can resimulate several ticks per frame) | Slowdown online | Measure in M6/M7; arena/memcpy rollback optimisation is already planned in PORTING.md. |
| Duplicate symbols with SDL2/ZipArchive/zlib/vInputKit | Link failure | Master object with export list (2.1). |
| Sideloadly identifier mismatch | Fresh data container: Game folder and saves "lost" (they stay in the old container) | Same Sideloadly settings; Finder backup of `save` first (7). |
| Online parts depend on the unfinished online port | Nearby/lobby late | Touch, name box and offline play ship first (M4/M5); P2P after the online port. |
| Native vs Go-app netplay (Ikemen versions differ) | Desyncs between users on different apps | Version string in lobby/Nearby adverts already filters peers; decide cross-play policy (open question 3). |

## 10. Open questions

1. Sideloadly: which options were used for the current install (bundle-id
   change, Apple ID)? They must be repeated exactly. Is the team
   9QP5427NJJ a paid account? We avoid entitlements either way.
2. Minimum iOS version and oldest device to support (sets the CPU budget
   for rollback and the GLES/ANGLE decision). The CvF patch sets 17.0.
3. Must the native app play online against the current Go iOS/PC builds
   during the transition, or does a new version string split the player
   pool? PORTING.md still lists the cross-check against a Go build with the
   cvf-online-5 rules as open.
4. Does vInputKit have (or can it get) a simulator slice? Otherwise
   controller remapping is tested only on device (M7).
5. Is `Documents/Game` the only supported layout from now on, or should the
   all-in-one IPA (`make-full-ipa.py`) stay supported for the native app?
   The design keeps both.

## Answers to the open questions (PM, 2026-11-20)

1. **Sideloadly install:** Apple ID sign mode with the user's Apple ID (Unlimited App IDs), automatic bundle ID, "Enable File Sharing" on, no plugins dropped. Final bundle id on both phones: `com.kaioh1990.cvfonline` (Sideloadly records, installations 17 and 21). The native app must keep this bundle id and be installed the same way (Sideloadly), so Documents (Game, save) survive.
2. **Devices:** iPhone Duo (iOS 27.1), iPhone 15 Pro Max (iOS 27.2.1), iPad mini (iPadOS 27.0.1). Current deployment target is iOS 17; keep it unless the engine needs newer APIs.
3. **Cross-play with the Go builds during the switch:** assume yes (friends will run mixed builds). Native must match the Go builds' simulation and handshake; see GAPS.md item 2 (fight width reset) and item 9.
4. **vInputKit Simulator build:** yes, the xcframework has an arm64 Simulator slice (vInputKit README/INTEGRATION.md). Controllers themselves are only testable on device.
5. **All-in-one IPA:** not needed. The small IPA + Documents/Game layout is what the user uses.

## Implementation status (2026-11-20, branch claude/native-ios)

| # | Status | Evidence |
| --- | --- | --- |
| M0 | done | Regenerated with the GLES renderer and the iOS entry points: 4842 functions, 0 TODO, 0 syntax errors, 31 placeholders (none new). Linux headless md5 unchanged: `54f8ae06d74648af2182587b82a390d6` (cvf-online-2), `4334350cdebd48dc0caa04cf78e0d9e3` (cvf-online-5); `cvf_tests` 93/0; `cvf` links. `Renderer_GLES32_*`, `fn_cvfStoreTap`, `fn_cvfIOSMain`, `fn_cvfHost*` in `ik_sim.o`. glslangValidator check not done (not available in the container); the shaders compile and link on Apple's ES 3.0 in the Simulator. |
| M1 | done | `tools/package/ios_build.sh` builds `CvfEngine.xcframework` (ios-arm64, ios-arm64-simulator, 14 MB) with Xcode 27.2's clang; `nm -gU` on each master object lists exactly the 7 exports (the script fails otherwise). The real app links it (no separate test app). |
| M2 | done except macOS rerun | Segment-walk roots (`dladdr` + every writable segment), `gc_selftest()` (in `cvf_tests` on Linux, and on every Simulator headless run: "gc selftest ok"), guard-page goroutine stacks, `_setjmp` on Apple, 1 GB arena on iOS. Owner-thread check: collections abort off the engine thread (allocations too in debug builds). Memory warning: `SDL_APP_LOWMEMORY` -> collection at the next frame outside a fight. Stack measurement done in the Simulator instead of a 1 MB macOS build: the main goroutine peaks at **~1 MB** (998 KB on the main thread, 983-985 KB on the engine stack) loading the title screen, so **plan B is implemented**: `SDL_main` runs the engine on an mmap'd 8 MB stack with a guard page (`cvf_call_on_stack`, `gc_set_main_stack`). Not done: macOS rerun of the reference with these changes (Linux and the Simulator are verified). |
| M3 | done | Simulator (iPhone 15 Pro Max, iOS 27.2) runs the reference fight inside the app (`CVF_HEADLESS_DIR`/`CVF_HEADLESS_ARGS` via `SIMCTL_CHILD_`, ~10 s): cvf-online-5 winner 2, 224,158 lines, md5 `4334350cdebd48dc0caa04cf78e0d9e3`; cvf-online-2 winner 1, 287,752 lines, md5 `54f8ae06d74648af2182587b82a390d6`. Both before and after the plan B stack switch. |
| M4 | done (Simulator) | Intro, title and an Arcade fight render (`docs/ios_sim_intro.png`, `docs/ios_sim_title.png`, `docs/ios_sim_match.png`); `save/logs/cvf.log` written. Rotation and background/foreground not exercised (no UI automation for the Simulator here). The Simulator GL is Apple's software renderer: slow (~2 min from launch to title), not a performance measure. |
| M5 | part | `cvf_host.c` with the Go names; mailbox drained at `cvfGcSafepoint`. `CVF_INPUT_TEST=1` (save/cvf_env.txt) drives the host API as the overlay does: buttons on the overlay's virtual gamepad (SDL virtual joystick 0) pick Arcade, `CvfTouchTap` is accepted in character select, `CvfInMatch()` turns 1, and presses in the fight act (the hearts counter drops from 100 to 089). Not tested: real UIKit touches/gestures, name box round trip, `cvfTouchUI` with a controller. |
| M6 | part | **HTTP**: `src/platform/ios/http_nsurl.m` (NSURLSession) replaces libcurl on iOS. `CVF_HTTP_TEST=<url>` runs `cvfHTTPSelfTest` (translated net/http: GET, POST JSON, GET `/api/rooms`) against a local `lobby/main.go` on the Mac: `200 []`, POST `200 {"id":...}`, `200 [{"name":"iOS Simulator",...}]`; the lobby logged the room. The lobby *screen* was not driven. Info.plist gets `NSAllowsLocalNetworking` (ATS) for UPnP and local servers; the real lobby is HTTPS. **Nearby**: `cvfP2PAvailable` is true on iOS; `cvfP2PAdvertise/AdvertiseStop/Browse/Join/Close` and `cvfNativeAskName` are C (`ik_native.c`) and call the table from `CvfP2PRegister`, as `cvf_p2p_ios.go` did. Simulator run of `tests/smoke/smoke.lua` (title -> Versus select -> Online > Host Nearby -> Join Nearby): `SMOKE OK`; while it hosts, the Mac's `cvf_nearby_test scan` lists it over the LAN, and `dns-sd -B _cvfnearby._tcp` on the Mac shows the app's Network.framework advertisement (engine -> table -> CvfP2P.m). Not tested: a Nearby join and match between two iOS instances (one Simulator only, for disk space; needs two devices), the name box, netplay. |
| M7 | IPA only | Signed Release build: `CvF Native Engine/dev-ios/CvF-Native.ipa`. **Own identity, installs next to the Go app**: bundle id `com.kaioh1990.cvfnative`, display name "CvF Native", same AppIcon; team 9QP5427NJJ, "Apple Development" identity, wildcard team profile (no new App ID or profile was needed); version 2.0 (2), iOS 17.0+, vInputKit compiled in. Its Documents folder is its own: the game folder must be copied into "CvF Native" (`Documents/Game`). Not installed. |

### Differences from the design

- **GL view placement**: SDL's UIKit back end creates its own UIWindow; the
  app's window (overlay) stays in front. `src/platform/ios/ios_view.m` moves
  SDL's GL view into the app's root view (below the overlay) right after GL
  context creation, as the Go engine's `IkemenAttachMetalLayer` did. The app
  needs no change for this.
- **Screen framebuffer**: on iOS framebuffer and renderbuffer 0 are not the
  screen. `gles_bind.c` maps 0 to SDL's view FBO/renderbuffer (read at
  `gl.Init`) and rebinds the view renderbuffer before each swap (SDL's swap
  presents the bound renderbuffer).
- **Post-processing targets**: `RGBA8_SNORM` is not renderable in ES 3.0;
  plain RGBA on iOS (as on the Vita).
- **App changes** (copy of the Go Xcode project in `dev-ios/app`, made by
  `tools/package/ios_native_project.py`): `libikemen.a`, ffmpeg/vpx/xmp and MoltenVK
  out; `CvfEngine.xcframework` and `-framework OpenGLES` in; `#import
  "libikemen.h"` -> `"cvf_host.h"` in ViewController.m and
  TouchOverlayView.m; version 2.0 (2). The Metal container code is left in
  place (unused). SDL2.xcframework gained an arm64 Simulator slice built from
  SDL 2.32.10 sources (device slice unchanged).
- **DefaultAssets.zip**: `tools/package/ios_assets.sh` (6a09392 scripts +
  `patches/ikemen-cvf-online.patch`, gamecontrollerdb/config from the Go
  app's zip, `external/.cvf-engine`).
- **Diagnostics**: `CVF_GL_CHECK=1` logs the first GL errors with the call
  name; the log reports the engine stack high-water mark.

### Device run (iPhone 15 Pro Max, iOS 27.2.1, 2026-11-21)

Installed with `xcrun devicectl device install app` next to the Go app
(game data in `Documents/Game`). Boots to the title and loads the roster;
engine stack peak 983 KB of 8192 KB.

- **AGX assertion (fixed)**: the first device log had
  `AGX: Texture read/write assertion failed: bytes_per_row >= used_bytes_per_row`
  twice (after "Check D", and after the first screen framebuffer bind).
  Cause: `Texture_GLES32.SetSubData` passes the pixel size (`depth/8`) as
  the row stride, so `SetSubData2` sets `GL_UNPACK_ROW_LENGTH` to 1 for
  each 256x1 palette row it uploads into the palette atlas (the white
  palette in `System.init`, then the motif's palettes). GL ignores the row
  length for a one-row image and other drivers say nothing, but Apple's
  GLES-on-Metal validates it. `gles_bind.c` now tracks the unpack row
  length and uploads single-row images whose row length is below the
  width with row length 0 (the same bytes), then restores it; multi-row
  cases are passed through and logged. With the fix the log shows
  `gles: glTexSubImage2D 256x1: UNPACK_ROW_LENGTH 1 ignored for a single row`
  at exactly the two places, and no AGX line. The Go iOS app does not hit
  this: it renders through Vulkan/MoltenVK (`render_vk.go` passes stride
  0). The Go Android build uses the same GLES code and sets the same row
  length; Android drivers accept it. The native Android build shares
  `gles_bind.c`, so it gets the fix too. The headless reference does not
  use GL: Simulator md5s unchanged (`54f8ae06...`, `4334350c...`).
- **Expected log lines** (same in Go builds of this Ikemen version):
  `Warning: Failed to assign key [...]` (12 lines: CvF's `system.def` has
  keys from a newer Ikemen; Ikemen 6a09392's `motif.go`/`config.go` print
  them; the Go Vita build's log has the identical 12 lines) and
  `Failed to add char: <br>` / `<vacant> (DEF not found)` (CvF's select
  layout tokens; 6a09392's `AddChar` prints them; only the Vita build skips
  them silently).

### Open / next

1. Device run (M7): boot, title, roster and the GL log done on the 15 Pro
   Max (above); still to do: a match, 120 Hz, vInputKit, touch.
2. Real touch, gestures, name box and rotation in the Simulator or on device.
3. M6 on devices: Nearby join + match between two phones (direct link),
   lobby screen against the real lobby, netplay vs desktop native.
4. macOS reference rerun with the runtime changes.
5. Trim the Metal-only code from the app.
