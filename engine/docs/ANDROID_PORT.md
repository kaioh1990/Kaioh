# Android port of the native C engine

Status 2026-11-20, branch `claude/native-android`: the engine builds for
arm64-v8a with NDK r27d, the APK `cvf-native-android.apk` is built and
signed by CI, both reference fights give the reference md5s with Android's
code generation. Nothing has run on an Android device yet (none available).

The model is the iOS port (IOS_PORT.md): the engine is a library built by
CMake with the determinism flags; the existing app wrapper is reused
unchanged in its logic, because the engine exports the same C functions the
Go library exported; GLES 3 renderer; the engine on its own thread with a
big stack; platform code only in `src/platform/android`; generated C only
regenerated, never edited.

## 1. The app: "CvF Native"

| | Go app (claude/android) | Native app |
| --- | --- | --- |
| Package | `com.kaioh1990.cvfonline` | `com.kaioh1990.cvfnative` (installs next to the Go app) |
| Label, icon | CvF Online, ikemen-droid icon | **CvF Native**, same icon |
| Game folder | Internal storage/CvF Online/Game, then Android/data/com.kaioh1990.cvfonline/files/Game | **Internal storage/CvF Native/Game**, then Android/data/com.kaioh1990.cvfnative/files/Game (same order; a folder picked by hand comes first) |
| Signing | `android/cvf-online.keystore` | the same key (apksigner: certificate SHA-256 `4d9a2123...c968c1`) |
| Wrapper | Jesuszilla/ikemen-droid dbc5cfb + `android/cvf-online.patch` | the same, then `android/native/customize-native.sh` |
| Native libs | libmain.so (Go), libSDL2.so, ffmpeg, libxmp | libmain.so (C engine, 5.5 MB), libSDL2.so 2.32.10 |
| Version | 0.8.5 | 2.0-native, versionCode = CI run number |

`customize-native.sh` runs `android/customize.sh` (gestures overlay, game
folder lookup, never overwriting `save/`, signing) and then: own id and
label; "CvF Online" -> "CvF Native" in SDLActivity's folder lookup and
dialogs; no `GODEBUG`; adds `CvfHttp.java`; `usesCleartextTraffic` (UPnP to
the router is plain HTTP, as the Go engine's client allows); copies the
engine libraries and assets. The two apps must not share one game folder:
each extracts its own `external/script` (the native engine runs the 6a09392
scripts + the CvF patch, the Go app newer ones).

APK assets (`tools/package/android_assets.sh`): the engine's Lua scripts,
SDL_GameControllerDB, `save/config.ini` from cvf-files with
`RenderMode = OpenGL ES 3.2`, `external/.cvf-engine` (engine revision).

## 2. Engine library (`CMakeLists.txt`, `if(ANDROID)`)

- `cvf_core` + `cvf_ik` (translation, SDL and GLES bindings) as static
  libraries with `-fvisibility=hidden -fPIC`, linked into **libmain.so** with
  `src/platform/android/*.c`. A version script exports exactly 7 symbols:
  `JNI_OnLoad`, `SDL_main` and the five `Java_org_libsdl_app_SDLActivity_*`
  functions the Java side declares (`nativeOnSDLReady`, `nativeCvfInMatch`,
  `nativeCvfTouchTap`, `nativeCvfSetTouchMenus`, `nativeCvfSetScreen`).
  `android_build.sh` fails if the count differs. `-z max-page-size=16384`
  (16 KB-page devices).
- Minimum API 26 for the native code (`API=` to change). getifaddrs (Nearby)
  needs 24; timespec_get needs 29, so the engine uses clock_gettime on
  Android. The Go build targeted 34, which would break on Android 13
  (API 33) devices if any 34-only symbol were used; the Retroid Pocket 6 runs
  Android 13.
- Heap reservation 1 GB (`CVF_GC_DEFAULT_ARENA_MB=1024`, as iOS): live set
  ~30 MB; a 4 GB reservation can be refused on phones with little RAM.
- Static test executables from the same objects: `cvf_headless`,
  `cvf_nearby_test`, `cvf_tests` (`-static`, SDL stubbed in
  `tests/android/sdl_stub.c`; `tests/android/tls_align.c` gives the 64-byte
  TLS alignment bionic requires of static arm64 executables). They run on any
  arm64 Linux kernel and under qemu-aarch64.

### Determinism

- `CVF_DETERMINISM_FLAGS` as everywhere: `-ffp-contract=off -fno-fast-math
  -fwrapv -fno-strict-aliasing`. NDK clang fuses `a*b+c` into `fmadd` without
  it; with it libmain.so contains no fused multiply-add instruction
  (`llvm-objdump | grep fmadd` = 0).
- New: **`-fsigned-char`** on non-Apple ARM targets. Plain `char` is
  unsigned in the Linux/Android ARM ABIs and signed on x86-64 and Apple
  arm64, where the reference dumps were verified. The reference fight gives
  the same md5 with unsigned char too (tested), so this is a safeguard for
  code paths the reference fight does not reach, not a fix of a seen
  difference.
- No libm transcendental functions in the game (all through detmath); the
  libm functions used (fmod, ldexp, frexp, modf) and strtod are exact or
  correctly rounded in bionic as in glibc/Apple.

### Runtime changes (all platforms where noted)

- `gc.c`: data roots of the engine module from `dl_iterate_phdr` (every
  writable PT_LOAD segment of the module containing the collector; the
  Linux `__data_start`/`_end` symbols are an executable's, not a shared
  library's). Stack base from `pthread_getattr_np` on bionic too.
- `gc.c`, **a real collector bug on Android**: bionic's arm64 `setjmp` stores
  x19-x30 XORed with a per-process cookie (checked in the NDK's libc.a), so
  the collector's register spill through `setjmp` hid any pointer that lived
  only in a callee-saved register. `mark_stack` now also stores those
  registers in clear (arm64), and rbx/rbp/r12-r15 on x86-64 (glibc mangles
  rbp in setjmp).
- `gort.h`: `_setjmp`/`_longjmp` on Android as on Apple (bionic's `setjmp`
  saves the signal mask: one system call per function with `defer`).

## 3. Platform glue (`src/platform/android`)

- `android_sys.c` (JNI, system headers; no `ik_sim.h`):
  - `JNI_OnLoad`: keeps the JavaVM and looks up `CvfHttp` (only possible
    there: threads attached later have the system class loader).
  - `nativeOnSDLReady(path)`: the game folder chosen by the Java lookup;
    `SDL_main` (on SDL's SDLThread) waits for it, as the Go library did.
  - `nativeCvfSetScreen(w, h)` -> `--screen WxH`.
  - **Engine thread**: SDLThread is a Java thread with ART's ~1 MB stack;
    the main goroutine needs ~1 MB alone (IOS_PORT.md 3.3). `SDL_main` starts
    a pthread with a **16 MB** stack and joins it; all SDL/GL calls happen on
    that thread (SDL attaches it to the JVM itself). A real thread rather
    than iOS's stack switch, because ART checks stack bounds on JNI calls.
    The engine stack high-water mark is logged.
  - Log: stdout/stderr -> `<game>/save/logs/cvf.log` (readable over USB) and
    logcat (tag `cvf`).
  - `save/cvf_env.txt`: `KEY=VALUE` lines become environment variables
    (`CVF_GC_*`, `CVF_GL_CHECK=1`, and the test modes below).
  - When the engine returns, the process exits (the engine's globals are set
    up once per process).
- `android_host.c` (engine thread): collector, `os.Args`, `fn_cvfIOSMain`
  with `--assets <game> --screen WxH` (the same entry as iOS; realMain's
  Android branch chdirs and initialises SDL). Before it,
  `SDL_InitSubSystem(JOYSTICK | GAMECONTROLLER | HAPTIC)`: Ikemen's Android
  branch calls `sdl.InitSubSystem`, which is a placeholder in the
  translation (GAPS.md), so controllers would otherwise never open.
- **Touch hand-off.** The gesture overlay calls `nativeCvfInMatch` and
  `nativeCvfTouchTap` on the UI thread and needs an answer at once. The
  collector is single-threaded, so the UI thread never enters translated
  code: once per frame (`cvfGcSafepoint` hook) the engine publishes "in a
  fight" (`cvfHostInMatch`) and "this screen reads taps" (new
  `cvfHostTapListening`, the test `cvfStoreTap` makes) in atomics; a tap is
  accepted against that value and stored in the mailbox, and the engine
  passes it to `cvfHostTap` at the next frame. `nativeCvfSetTouchMenus` goes
  through the mailbox too.
- **Controllers**: SDL's own Android path (SDLControllerManager.java,
  HIDAPI for USB/Bluetooth pads), the engine's SDL GameController code and
  `external/gamecontrollerdb.txt`. Built-in handheld controls (Retroid
  Pocket 6) are an Android gamepad input device, which SDL maps
  automatically. With a controller connected the overlay only passes direct
  taps (`cvfTouchMenus` off), as in the Go app.
- **Renderer**: the translated `render_gles32.go` (`#version 320 es`
  shaders), entry points through `SDL_GL_GetProcAddress` (SDL falls back to
  `libGLESv2.so`'s symbols). The Go side now forces `RenderMode = OpenGL ES
  3.2` on Android (a config.ini copied from a PC says OpenGL 3.3). The window
  and context attributes come from Ikemen's Android branch (ES 3.2, depth 24,
  no alpha). The manifest requires GLES 3.2, as the Go app's.
- **Pause/resume**: SDL blocks the event pump while the activity is paused
  (`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, default) and recreates the EGL surface
  on resume, so no GL work happens in the background. An online match times
  out meanwhile, as with the Go app.

### Go-side changes (`tools/ikemen_headless.patch`, regenerated)

- `main.go`: `RenderMode = "OpenGL ES 3.2"` on Android under `cvfEngine`.
- `tools/ikemen/cvf_mobile.go`: `cvfHostTapListening`; root in
  `run_reach.sh`. Regenerated: 4838 functions, 0 TODO, 0 syntax errors, 31
  placeholders (none new). Desktop reference fight unchanged
  (54f8ae06..., Linux x86-64).

## 4. Online

- **HTTP** (lobby, public IP, UPnP): `ik_http.c` -> `android_http.c` -> the
  app's `org.libsdl.app.CvfHttp.request` (HttpURLConnection) on a worker
  thread attached to the JVM, one per request; results in the job's malloc
  memory, the waiting goroutine polls `done` as with libcurl/NSURLSession.
  Chosen over building curl + mbedTLS from source: Android's own TLS stack,
  certificate store (updates, user CAs), proxy settings and cleartext policy
  for free; a few KB instead of ~1 MB; nothing to keep patched. The requests
  are few and small. Not exercised yet (needs a device; the iOS-style
  `CVF_HTTP_TEST=<lobby URL>` in `save/cvf_env.txt` runs the round trip and
  logs it).
- **Nearby (LAN)**: the translated cvf_nearby.go over `ik_net.c` (BSD
  sockets, getifaddrs), UDP 7520 probe/reply and subnet sweep, unchanged
  from desktop. `cvf_nearby_test` built for Android passes host, scan,
  "host listed" and the periodic sweep (qemu and the CI arm64 runner); its
  two "desktop answers" checks fail by design on Android (`cvfTouchUI` is
  true and the host hint uses touch wording, as on the phones).
  Permissions INTERNET and ACCESS_WIFI_STATE are in the wrapper's manifest.
  bionic's getifaddrs works for apps targeting API 30+, where Go's netlink
  interface listing fails.
- **P2P direct link**: unavailable, as in the Go Android build
  (`cvfP2PAvailable` is true only on iOS; the C functions do nothing
  without a registered table).

## 5. Build

Local (Linux x86-64; NDK r27d):

    tools/package/android_build.sh NDK_DIR OUT     # libSDL2.so, libmain.so, test binaries
    tools/package/android_assets.sh IKEMEN_CLONE gamecontrollerdb.txt OUT/assets
    android/native/customize-native.sh IKEMEN_DROID OUT/jniLibs OUT/assets
    (cd IKEMEN_DROID && ./gradlew assembleDebug)  # needs the Android SDK

CI: `.github/workflows/native-android.yml` (push to this branch or
`claude/native-integration`, or by hand): job `apk` installs NDK
27.3.13750724 with sdkmanager, builds, assembles with Gradle (AGP 8.1, JDK
17), checks package/label/ABI/signature/CvfHttp and uploads the artifacts
`cvf-native-android` (the APK) and `cvf-native-android-tests`; job
`arm64-tests` runs the static test binaries on `ubuntu-24.04-arm`.

The container used for this port has no access to dl.google.com: the NDK
came from a GitHub mirror (Lonemachinist/android-ndk, r27d). Its clang-18,
libc.a and API-26 libc.so have the same SHA-256 as the NDK that CI installs
from Google.

## 6. Determinism evidence

| Check | Where | Result |
| --- | --- | --- |
| Reference fight, cvf-online-2 rules, static bionic `cvf_headless` (NDK r27d, the objects of libmain.so) | qemu-aarch64 7.2 user mode, container | winner 1, 287,752 lines, **54f8ae06d74648af2182587b82a390d6** |
| Reference fight, cvf-online-5 rules, same binary | qemu-aarch64 | winner 2, 224,158 lines, **4334350cdebd48dc0caa04cf78e0d9e3** |
| Same, built without `-fsigned-char` | qemu-aarch64 | 54f8ae06... (unchanged) |
| `cvf_tests` (detmath golden values, RNG, Atof, formats, scheduler, GC selftest) | qemu and CI `ubuntu-24.04-arm` (real arm64) | 93 checks, 0 failures |
| Fused multiply-add instructions in libmain.so | llvm-objdump | 0 |

qemu emulates the arm64 FP instructions with IEEE semantics; the binary is
the same machine code as libmain.so's simulation (same objects, same
flags), only linked statically. A run on a device stays to do: put
`CVF_HEADLESS_DIR=/storage/emulated/0/CvF Native/refgame` and
`CVF_HEADLESS_ARGS=-p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1`
in `CvF Native/Game/save/cvf_env.txt` with the reference data in that
folder; the app then writes `statedump.txt` there and "winner: N" to the
log, and exits. (Remove the lines afterwards.) The netplay dump
(`a350bc2d...`, tests/netplay on claude/native-netplay) needs the same
reference game and two engines; not run for Android.

## 7. Install (player)

1. Get `cvf-native-android.apk` (CI artifact `cvf-native-android`, or the
   copy in `CvF Native Engine/dev-android/` on the Mac).
2. Copy the game to the device over USB (MTP): connect, choose "File
   transfer", open Internal storage, create the folder `CvF Native` and copy
   the game folder into it as `Game`, so that `Internal storage/CvF
   Native/Game/chars`, `data`, `external`, `font`, `sound`, `stages` exist.
   Copy `save/` too to keep settings (the app never overwrites files in
   `save/`). A copy of the Go app's `CvF Online/Game` works; do not point
   both apps at the same folder.
3. Install the APK (open it on the device, allow installing from that
   source). It installs next to CvF Online.
4. First start: allow "All files access" when asked (the folder is in shared
   storage), go back to the app. It copies its scripts into the game folder
   and starts.
5. Problems: `CvF Native/Game/save/logs/cvf.log` (also in logcat, tag
   `cvf`).

## 8. Open

1. Device run: start-up, rendering on Adreno/Mali (ES 3.2 shaders,
   RGBA8_SNORM post-processing targets as in the Go app), the engine stack
   peak in the log, controllers (Retroid Pocket 6 built-in pad, Bluetooth
   pads), touch gestures and taps, pause/resume, the device reference fight
   (section 6).
2. HTTP on a device (lobby list/host, public IP, UPnP) and a Nearby match
   Android <-> desktop native; rollback cost on phone CPUs.
3. The name box (`cvfNativeAskName`) has no Android implementation, as in
   the Go Android build: the Lua prompt is used.
4. Integration with the all-platform fixes on other branches (netplay leak,
   defer close, goroutine stack scanning, go_cstr_copy): see the branch
   notes in the final report.
