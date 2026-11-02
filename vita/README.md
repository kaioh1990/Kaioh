# PS Vita port — status

Goal: run the patched Ikemen GO (rollback netplay, CvF content) natively on a
hacked PS Vita (HENkaku/Ensō), able to play online against PC/Mac/iPad.

## Phase 1 — feasibility spike (done, Nov 1 2026)

| Question | Result |
|---|---|
| Can the engine's Go code be compiled without the standard Go runtime (which has no Vita port)? | **Yes.** With TinyGo 0.44 (Go 1.27, LLVM 22) the whole engine — Lua, rollback, netplay, the CvF patch — compiles and links (`-gc=conservative -scheduler=tasks`, build tag `vita`). |
| Graphics | Uses the existing OpenGL ES renderer (Android's) instead of GL 3.3/Vulkan. On the Vita this maps to vitaGL. |
| Video playback (FFmpeg) | Dropped on the Vita build (`video_stub_vita.go`). |
| Memory | **Blocker to fix:** one CvF match holds ~700 MB live heap, almost all from `data/Xsound.snd` (687 MB of uncompressed WAVs) being loaded whole. Vita homebrew gets roughly 300–450 MB. Needs on-demand sound loading (helps every platform). |

Changes needed for TinyGo (patches in this folder):
- `engine-vita-tags.patch`: `vita` build tag selects the GLES renderer, stubs Vulkan/video.
- `go-sdl2-tinygo.patch`, `gl-tinygo.patch`: TinyGo's cgo has no `pkg-config`,
  no C bitfields/unions-as-arrays, no `_cgo_export.h`, and needs C23 prototypes
  (`-std=c2x`). Unused SDL pieces (syswm, hidapi, shape, AudioCVT, event filters'
  header) removed or given explicit prototypes. `declstrip.go` is the helper used.

## Next phases

1. **On-demand sound loading** (SND index + read WAV when first played, LRU cache).
2. **TinyGo `vita` target**: target JSON (cortex-a9, hard-float, thumb2), small
   runtime file (heap from `sceKernelAllocMemBlock`, ticks, sleep), link with
   vitasdk (`arm-vita-eabi-gcc -Wl,-q`), `vita-elf-create` → `.vpk`.
   Hello-world VPK first, then the engine.
3. **Renderer**: Ikemen's GLES shaders → vitaGL; 960×544 output.
4. **Netplay** over the Vita's sockets; controls mapped to the Vita's buttons.
5. Test in Vita3K, then on hardware.
