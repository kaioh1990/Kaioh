# CvF Native for PS Vita — M2 test build (`cvf-native-m2.vpk`)

The native C engine running the game on the Vita: menus, character select and
fights. This is a test build: it has never run on a real Vita. Please send back
the log lines listed at the end.

## Install

1. Copy `cvf-native-m2.vpk` to the Vita (for example `ux0:downloads/`) with
   VitaShell's FTP or USB mode.
2. In VitaShell, select the VPK, press X and confirm. It installs as
   **"CvF Native"** (title ID `CVFN00001`). If you installed the M1 test
   build, this replaces it. The TinyGo "CvF Online" app (`ux0:data/CvF`)
   stays separate.
3. `ur0:data/libshacccg.suprx` must be on the Vita. It is already there if
   the TinyGo CvF Online build runs, because vitaGL compiles shaders with it.

## Copy the game

The VPK holds no game files and no game artwork. Copy your own Castlevania
Fighter / CvF Online game folder (the one that holds `data/`, `chars/`,
`stages/`, `font/`, `sound/`, `external/` and `save/`) to

    ux0:data/CvFNative/game/

so that `ux0:data/CvFNative/game/data/` exists. A copy of the folder you
use for the TinyGo build (`ux0:data/CvF`) works. Copy it rather than moving
it, so that both builds keep working. The game writes `save/` inside that
folder. If the folder is missing, the app shows a message saying so and
exits.

## Settings: `ux0:data/CvFNative/cvf_env.txt`

Optional. Use one `KEY=VALUE` per line. Lines that start with `#` are
ignored.

| Setting | Default | What it does |
| --- | --- | --- |
| `CVF_VITA_FPS` | `30` | Drawing rate in fights: `30` draws every second game tick (the TinyGo build's mode), `60` tries to draw every tick and skips a draw when it falls behind. The game logic always runs at 60 Hz. Menus always try for 60. |
| `CVF_VITA_RENDER_THREAD` | `0` | `1`: the vitaGL drawing calls run on a second CPU core while the next game tick runs (experimental) |
| `CVF_ARM_MHZ` | `444` | CPU clock. Use `500` if an overclock plugin unlocks it. |
| `CVF_FRAME_LOG` | `1` | `0` turns `frames.csv` off |
| `CVF_GAME_DIR` | `ux0:data/CvFNative/game` | Another game folder |
| `CVF_VITA_VSYNC` | `1` | `0`: vitaGL does not wait for the vertical blank |
| `CVF_VITA_FBO` | `0` | `1`: draws the scene offscreen first (only for comparison if the picture is wrong) |
| `CVF_VGL_RAM_THRESHOLD_MB` | `16` | Main RAM that vitaGL leaves free |
| `CVF_GC_ARENA_MB` | `192` | The engine's heap |
| `CVF_SOUND_CACHE_MB` | `24` | Cache for sounds loaded on demand |
| `CVF_VITA_MODE` | `game` | `headless` runs the M1 test instead: unit tests and the reference fight, with the result on screen (see VITA_PORT.md section 8) |

Example for a 60 fps test:

    CVF_VITA_FPS=60
    CVF_ARM_MHZ=500

`ux0:data/CvFNative/cvf_args.txt` (optional, one line) adds Ikemen
command-line arguments. For example, a quick AI-vs-AI match that skips the
menus:

    -p1 Alucard -p2 Richter -p1.ai 8 -p2.ai 8 -s stages/ParanoiaBG.def -rounds 1

## Controls

The Vita buttons act as controller 1 with the game's `[Joystick_P1]`
mapping from `save/config.ini`: Cross = A, Circle = B, Square = X,
Triangle = Y, L = LB, R = RB, START = start, SELECT = back. Tapping the
front touch screen works in the menus and the character select, as on
iPhone and iPad. Remap the buttons in the game's options or in
`config.ini` if a button you need sits on a trigger, because the Vita has
no RT or LT.

## What to send back

All of these are in `ux0:data/CvFNative/`:

1. **`perf.log`**, the whole file. It is small. It contains:
   - the first line: CPU, bus and GPU clocks and the draw mode;
   - `mem before vitaGL` and `mem after vitaGL`;
   - every 5 s, a `perf t=...` line: game ticks, ticks during a fight,
     frames drawn and fps, **logic ms per tick** (average, 95th percentile,
     maximum), **draw ms** and **swap ms** per drawn frame, frame time, and
     the number of ticks over 17.5 ms. With the render thread there is also
     a `perf render thread` line;
   - after each `perf` line, a `mem 5s` line: engine heap (live, pages,
     high-water mark, collections, longest pause), newlib heap, free kernel
     memory (user, CDRAM, phycont) and free vitaGL memory.
2. **`cvf.log`**: the end of the file, or the whole file if something went
   wrong (a Lua error, `gles:` lines, a crash).
3. Optional: **`frames.csv`**. It has one line per game tick:
   `tick,t_ms,logic_us,draw_us,swap_us,render_us,frame_us,drawn,fight`.

The most useful test is one fight with `CVF_VITA_FPS=30` (the default),
then one fight with `CVF_VITA_FPS=60`, and if you like one more with
`CVF_VITA_FPS=60` and `CVF_VITA_RENDER_THREAD=1`. Send `perf.log` after
each run, because every start overwrites it. Also say whether the picture,
the sound and the controls looked right.
