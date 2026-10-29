# EvolutionRecomp

A static recompilation of **Turok: Evolution** (Xbox, USA) to native Windows,
built with the xboxrecomp toolkit
([sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), with the
Direct3D 11 renderer from [aknavj's fork](https://github.com/aknavj/xboxrecomp)).

The game's x86 code is translated to C and compiled into a Windows `.exe`;
the Xbox kernel, GPU (NV2A) and audio (MCPX APU) are provided by the
xboxrecomp runtime. **No game code or data is in this repository.** You need
your own disc image, and the C code is generated from it on your machine.

This branch (`d3d11-fork-experiment`) renders on the GPU. `main` is the older
CPU-renderer build (about 9 fps on the title screen).

## Status

- Boots through the Acclaim logo and the intro movie (full rate, ~59 fps) to
  the title screen and menus.
- New game reaches Chapter 1: terrain, jungle, weapon and HUD render at about
  58 fps, without flicker.
- Controller works (any XInput pad), as controller 1: move, look, jump,
  fire, weapon switching and pause.
- Keyboard and mouse work too, merged onto controller 1 (see Controls).
- Dinosaurs and Turok cast shadows, with filtered (PCF) edges.
  `RECOMP_SHADOWS=0` turns them off. They reach further than on the Xbox:
  the shadow map covers 64 units ahead of the camera and 12 around the
  player, so creatures beside and behind you (and at the sides of a
  widescreen view) cast them too, at 2048x2048 (the game's own 16 units
  ahead at 512x512). A creature out of view still casts its shadow; the
  game drew only those of creatures on screen. `RECOMP_SHADOW_DISTANCE`,
  `RECOMP_SHADOW_AROUND`, `RECOMP_SHADOW_MAP` and `RECOMP_SHADOW_OFFSCREEN`
  change them; 16, 0, 512 and 0 give the original.
  The ambient birds (FlyingIndigenousAI) cast no shadow once more than 10
  units above the player's height (`RECOMP_SHADOW_BIRD_HEIGHT`): their blobs
  landed far below them. Birds taking off nearby keep theirs.
  `RECOMP_SHADOW_BIRDS=1` gives every bird one.
- The window shows frames straight from the GPU through a swap chain, so it
  resizes and maximises freely (the frame stays 4:3, letterboxed), opens at
  the largest multiple of 640x480 that fits the screen, and **Alt+Enter**
  toggles borderless fullscreen. `RECOMP_VSYNC=1` presents on the monitor's
  refresh; `RECOMP_PRESENT=gdi` goes back to the old 640x480 window.
- The game renders above its 640x480: by default at the whole multiple of
  480 lines that covers your monitor's height (3x, 1920x1440, on a 1440p
  screen). `RECOMP_RENDER_SCALE=<n>` sets it (1 to 8, fractions allowed; 1 is
  the Xbox's resolution). The shadow map keeps its own size.
- **Options > Video Options**, in the front end and in the pause menu, sets
  the resolution, widescreen (up to 21:9), fullscreen, VSync, a frame limit
  (up to 240 or off) and PC button icons while you play; see
  [Video Options](#video-options).
- The menus take the mouse as the PC release's did (with its arrow, if you
  name a PC install), and the main menu ends in **Quit**; see
  [Mouse in menus](#mouse-in-menus).
- Known issues: environment-map layers
  (water, Tek bow) and cave lighting are not yet compared with the console.

## Playing

You need your own copy of Turok: Evolution for the original Xbox, **USA**
release. Start `turok_evolution.exe`:

- Put your disc image (`.iso`, a full image of the disc, or a trimmed
  `.xiso`) beside the program, and it is extracted once into a `game` folder
  there (about 2.5 GB).
- Or, with no image beside it, the program asks for the image or for the
  `default.xbe` in a folder of already extracted files.

The folder is remembered in `turok_settings.ini` (`[paths] game_dir`), and the
game's `default.xbe` is checked to be the USA release's (SHA-1
`25e1d77b216a2e557d7a5de6266cda64ae62627b`): the recompiled code is that
executable's, so other releases are refused. `RECOMP_GAME_DIR` names the
folder by hand. Saves go in that folder's `udata`.

The game's shaders are translated to HLSL and compiled as each one first
appears. Compiled shaders are kept in `%LOCALAPPDATA%\xboxrecomp\shadercache`,
so each one is compiled once per machine, and new ones compile in the
background rather than in the frame that needs them: until a pixel shader is
ready its draws use a slower generic shader, and a draw whose vertex shader is
still compiling is skipped for those few frames. The first time a scene is
ever seen may show that briefly; after that it plays without compiling at all.
`RECOMP_SHADER_CACHE=<folder>` moves the cache (`0` turns it off) and
`RECOMP_SHADER_ASYNC=0` compiles in the frame instead, as before.

## What you need to build it

- An original Xbox disc image of Turok: Evolution (USA), XISO format
- Windows 10/11 with Visual Studio 2022 or later ("Desktop development with C++",
  which includes CMake)
- Python 3.10+ with `pip install capstone`
- [xdvdfs](https://github.com/antangelo/xdvdfs) or extract-xiso

## Setup

The layout this project expects, side by side:

```
Recomp/
  turok/Turok - Evolution (USA)/   extracted disc (default.xbe, supertree*.tre, Data/)
  xboxrecomp/                      sp00nznet/xboxrecomp: only its disassembler is used
  xboxrecomp_fork/                 MarkPremier/xboxrecomp, branch turok-evolution: the runtime
  turok_evolution/                 this repository, branch d3d11-fork-experiment
```

1. **Extract the disc**

   ```
   xdvdfs unpack "Turok - Evolution (USA).iso" "turok/Turok - Evolution (USA)"
   ```

   The code generation below reads `default.xbe` from this folder. At run
   time the program finds the files itself (see [Playing](#playing)); the
   folder in `src/main.c`'s `YOUR_GAME_DIR` is one it tries before asking.
   `RECOMP_GAME_SETUP=1` makes it look only beside the program, as a new
   install does.

2. **Get both toolkits**

   ```
   git clone https://github.com/sp00nznet/xboxrecomp.git xboxrecomp
   git -C xboxrecomp checkout f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b

   git clone -b turok-evolution https://github.com/MarkPremier/xboxrecomp.git xboxrecomp_fork
   ```

   `turok-evolution` is aknavj's `modded-software-aknavj-rebased` (11a8aa1)
   plus this game's fixes, one commit each: the utility-drive cluster size,
   RunPushBuffer handling in the DMA walker, USB address resolution, vblank-
   locked flips, anti-aliased surfaces and present, shadow-map stages, and
   unmapping dynamic buffers before drawing (the flicker fix).
   `patches/xboxrecomp-aknavj-turok.patch` holds the same changes as one diff
   against 11a8aa1, if you would rather apply them yourself.

3. **Generate the C code**

   The fork's own disassembler misses about a thousand functions, so the
   upstream one runs first and its output feeds the fork's later stages:

   ```powershell
   $xbe = "$PWD\turok\Turok - Evolution (USA)\default.xbe"
   $out = "$PWD\disasm_out"

   cd xboxrecomp
   py -3 -m tools.disasm $xbe --force -o $out `
       --seed-functions ..\turok_evolution\seeds\manual_seeds.json `
       --seed-functions ..\turok_evolution\seeds\recomp_extra_seeds.json
   Copy-Item "$out\*.json" ..\xboxrecomp_fork\tools\disasm\output\ -Force

   cd ..\xboxrecomp_fork
   py -3 -m tools.func_id $xbe
   py -3 -m tools.abi_analysis $xbe
   py -3 -m tools.recomp $xbe --all --split 250 `
       --gen-dir ..\turok_evolution\src\recomp\gen `
       --exclude-manual ..\turok_evolution\src\recomp_manual.c
   ```

   - Do **not** pass `--text-only` to `tools.disasm`. The Xbox libraries the
     game links (D3D, DSOUND, XPP, XMV) are barely swept without it.
   - `--exclude-manual` is required. Without it the hand-written overrides in
     `src/recomp_manual.c` conflict with the generated code.

4. **Build** (from `turok_evolution/`, in a Developer PowerShell for VS)

   ```
   cmake -S . -B build -DXBOXRECOMP_DIR=../xboxrecomp_fork
   cmake --build build --config Release --parallel
   ```

   With the PC release installed, `-DRECOMP_PC_EXE=<path>/Turok4.exe` gives
   the program and its window that release's icon (taken from your copy at
   configure time by `tools/extract_icon.py`; it is not in this repository).

   Release builds use link-time code generation (`-DRECOMP_LTCG=OFF` turns
   it off): it inlines the generated functions' small callees across files
   and is worth about a quarter more frame rate in busy scenes.

5. **Run**

   ```
   build\Release\turok_evolution.exe 2> stderr.txt
   ```

## Controls

A pad works as on the console. Keyboard and mouse (on by default,
`RECOMP_KBM=0` turns them off; Alt+Esc releases the mouse):

| Action | Default |
|--------|---------|
| Move / strafe | W A S D |
| Look | Mouse |
| Fire | Left mouse |
| Weapon mode | Right mouse |
| Jump | Space |
| Next / previous weapon | Wheel down / up |
| Duck | Ctrl |
| Zoom | Middle mouse |
| Pause | Esc |
| Menu select / back | Enter or Space / Esc or Backspace |
| Menu up / down / left / right | Arrow keys |

### Options are saved when you leave the menu

The game's own options (Control Setup's Invert Look, sound volumes, Game
Options) are written to its Options save whenever you back out of the
Options menu, from the front end or the pause menu. On the console they
were kept only if you chose SAVE OPTIONS, and anything left unsaved came
back as it was at the next start.

### PC Controls

**Options > PC Controls** (in the front end and the pause menu, under
Control Setup) binds each action in play to one key or mouse input. Press
Enter on a row, or click it, and it shows *Press a Key*: the next key, mouse
button (including the side buttons) or wheel notch is its new one. A key
another action had swaps over to the row's old key; six seconds with no
press keeps the old one. Left and right step through the keys, and a right
click on a row unbinds it. The choices are kept in `turok_settings.ini`
under `[controls]` (key names as the page shows them, e.g. `jump=Space`,
`fire=Mouse 1`).

Menus keep Enter/Space to select, Esc/Backspace to go back and the arrow
keys to move whatever the actions are bound to. In play the arrow keys are
the pad's d-pad unless an action uses one.

The game's own **Options > Control Setup > Advanced > Invert Look** turns the
mouse as it does the pad. The disc has it on; the patch makes it off for a
new player (ControlSetup.hst), and options saved before keep what they had,
so switch it off there once if the mouse feels upside down.
`RECOMP_KBM_INVERT_Y=1` inverts the mouse against it in "stick" mouse look.
`RECOMP_KBM_SENS` sets the mouse counts for a full turn rate in "stick" mouse
look (default 80, lower is faster). `RECOMP_KBM_MAP` sets the toolkit's
starting keys for the pad inputs (e.g. `B=Back,BLACK=G`); PC Controls'
bindings replace it for the actions they cover.

### Mouse in menus

In any menu (the front end and the pause menu) the mouse is let go and works
as a pointer, as in the PC release:

| Mouse | In a menu |
|-------|-----------|
| Move over an entry | focuses it |
| Left click | chooses it; on an option row, the next value (after the last, the first) |
| Right click | on an option row, the previous value (before the first, the last); elsewhere Back (resume on the pause menu, as Esc) |
| Wheel | over an option row, its value; elsewhere up / down |

Leaving the menu for play takes the mouse back for look. The arrow shows once
the mouse moves and hides again when the keys or a pad move the highlight. It
is the PC release's `cursor.dds`, read from the PC install named by
`pc_game_dir` (see Button icons below); without one it is Windows' own.

How it works: the screen loader's `this` (`sub_00054A60`) is the screen; its
actors are a tree from +0x9C (walked as `sub_0009C1C0` does), each widget's
rectangle on the 640x480 screen is at +0x2C4, and +0x244 bit 0 says it can
take the focus. Hovering hands the controller from the old entry to the new
one and calls the screen's SetFocus (`sub_000544D0`), as the menu's own up
and down (`sub_000A86A0`) do. Clicks are pad presses. Option rows are the
screen's SelectionMenuWidgets without an A-button link, read from its `.ati`.

The main menu gains the PC release's **Quit** below Cheats. It asks "are you
sure?" with the pause menu's own question, and yes leaves the game
(`XBOX:DASH`, the console's return to the dashboard).

## Video Options

A page in the game's own Options menu, in the slot the console version left
empty below SOUND OPTIONS (the pause menu's Options has it too). Changes take
effect at once and are kept in `turok_settings.ini` beside the exe:

| Row | Values | Same as |
|-----|--------|---------|
| Resolution | auto, 480p, 960p, 1440p, 1920p, 2160p (lines; the width follows the aspect; auto matches the monitor, 2160 on 4K) | `RECOMP_RENDER_SCALE` |
| Aspect ratio | 4:3, 16:10, 16:9, 21:9, auto (the monitor's) | |
| Display | windowed, fullscreen (borderless) | Alt+Enter |
| VSync | off, on | `RECOMP_VSYNC` |
| Frame limit | 30, 60, 120, 144, 240, display (its refresh rate), off | |
| Anisotropy | off, 2x, 4x, 8x, 16x (16x by default) | |
| MSAA | off, 2x, 4x, 8x | |
| Button icons | xbox, pc | |
| Mouse look | raw, stick | |
| Mouse speed | 1 to 10 (5 by default; each step 1.4x) | |
| Draw distance | normal, 2x, 4x, 8x (2x by default) | |
| Keep bodies | off, on (off by default) | |
| Keep decals | off, on (off by default) | |

An environment variable set by hand still wins at start-up.

- **Widescreen** widens the view rather than stretching it (Hor+): the game's
  3D programs project through c96 (rigid) or c187 (skinned), and that row is
  narrowed for draws to the screen. 2D draws stay at 4:3 in the middle unless
  they reach the screen's edge (fades, the damage flash), so the HUD keeps its
  shape. The front end, movies, loading screens and in-engine cutscenes (framed
  for 4:3, with their own bars; StartCinema/EndCinema, `sub_00164230`/
  `sub_001642A0`) stay 4:3. The window opens
  at the chosen shape.
- **Frame limit**: the game steps by a measured clock (`sub_00028B40`), so it
  runs at the right speed at any rate. The front end stays at 60, as its
  movies are paced by the vertical blank.
- **Button icons, pc**: the menus' A and B prompts become the PC release's
  Enter and Esc keys. They are read from a PC install, which you name in
  `turok_settings.ini`: `[paths]` then `pc_game_dir=<folder with data\Textures>`
  (or `RECOMP_PC_GAME_DIR`). They are swapped in the renderer, by a hash of
  the Xbox textures. In menus Escape is Back (and resumes from the pause
  menu); in play it pauses (`RECOMP_KBM_MENU_MAP`).

- **Mouse look, raw** (native mouselook): the view turns by the mouse's
  counts alone, at any frame rate, with no acceleration. The look stick goes
  through a 0.3 deadzone, a response curve (`sub_00142280`: x0.25 under half
  deflection, x0.3 under 0.9, squared above it, so turning jumps from 0.27 to
  0.81) and an easing towards larger values before the controller turns by
  it; the mouse skips all of that and is added in the turn functions
  themselves (`sub_00144430` yaw, `sub_00144560` pitch), so it keeps what the
  game does there: slower when zoomed, the pitch limits, and Invert Look
  (which turns the mouse and the pad alike). Up and across turn alike per
  count. "stick" is the old behaviour, tuned by `RECOMP_KBM_SENS`.
- **MSAA** applies to the screen's targets (multisampled copies resolved
  whenever the game reads them); it combines with the render scale.
  **Anisotropy** applies to mipmapped textures.
- **Draw distance**: grass, plants and other actors with a fade distance
  (A_NDD where they start to fade, A_MDD where they are gone: 15 to 25 units
  for grass) are drawn that many times further. The fade (`sub_0003C9E0`)
  reads the two from a block at actor +0x1CC; they are scaled while it runs.
- **Keep bodies**: the dead stay where they fell. A dead AI actor's update
  (`sub_0013D000`) counts time since death at +0x52C; past 3.5 s it fades
  the body and sinks it into the ground, and at 5 s removes it. With the
  setting on that clock is held at 2 s, except for actors that respawn
  (+0x5AA). `RECOMP_CORPSE_TRACE=1` logs deaths, fades and actor changes;
  `RECOMP_DEBUG_KILL=<s>` zeroes the dinosaurs' health that long into a level.
- **Keep decals**: blood splats, bullet holes, scorch marks and gibs last ten
  minutes instead of 3 to 20 seconds. They are particles, each kind a PARTDEF
  text the particle system parses as a level loads (`sub_000B0110`); its eighth
  value, the life in seconds, is stored by `sub_000AF2A0`. Definitions whose
  texture or mesh is a splat, bullet hole, scorch or gore piece get the longer
  life; switching the setting re-applies it at once, and off leaves every
  life as the game has it. `RECOMP_PARTICLE_TRACE=1` lists the definitions.
- **Widescreen culling**: the game culls against its 4:3 camera, so blood,
  bullet holes and other effects past the 4:3 edges vanished while on screen.
  While the camera's view planes (`sub_00020EE0`) and the in-view test run, the
  horizontal cotangent `sub_00020030` returns is divided by the widescreen
  factor; the projection keeps the camera's own.
- **Press Start** takes any button (the screen's link is given the button mask
  of its own any-button link).

The widescreen findings (c96/c187, the screen loader at USA `sub_00054A60`,
the timing at `sub_00028B40`) come from phobos665's
[turok-evolution-recomp](https://github.com/phobos665/turok-evolution-recomp)
(PAL), reimplemented here.

The page is game data, built at start-up from your own disc: the game reads
`patch0.tre` beside its `supertree*.tre` archives if there is one, and its
files replace the supertrees' copies. `src/video_settings.c` writes that file
into the game folder, with the Options screens gaining the entry and a
Video Options page derived from Game Options, the main menu its Quit entry
(from the pause menu's pieces); nothing from the disc is in
this repository. `RECOMP_VIDEO_OPTIONS=0` removes it and leaves the menus as
the disc has them. A `patch0.tre` that something else wrote is left alone.

## Project files

| Path | What it is |
|------|------------|
| `src/main.c` | Host entry point: loads the XBE, sets the runtime defaults below, calls the game's entry point |
| `src/game_files.c` | Finds the player's USA disc files, checks `default.xbe`, and extracts a disc image (XDVDFS) into `game` |
| `src/video_settings.c` | The Video Options page: builds `patch0.tre` from the disc's menus (its `.tre` and screen formats are described there), keeps `turok_settings.ini`, and applies the page's values through the history values the menu writes |
| `src/recomp_manual.c` | Hand-written overrides: D3D's GetVisibilityTestResult (sub_00194B40) completes unfinished tests as visible; DirectSound's idle-voice handler (sub_001B23DD) ignores voices nobody owns (the emulated APU could report one, and quitting a level crashed); the shadow manager's constructor (sub_0017D5C0) takes its shadow distance and map size from `RECOMP_SHADOW_DISTANCE`/`RECOMP_SHADOW_MAP`; the light camera's fit (sub_0017DAF0, through the camera FOV setter sub_0001FD20) widens to cover the area around the player; the shadow caster check (sub_000F21B0) passes creatures out of view; the box-in-view test (sub_00021A90) settles 95% of its calls natively, with the original's x87 roundings (`RECOMP_CULL_VERIFY=1` checks every answer against the original); D3D KickOff (sub_001984B0/sub_00198480) skips the write-combine flush handshake and the vblank handler (sub_0019D580) drops PMC_INTR's PCRTC bit before its acknowledge spin -- both waited on the toolkit's GPU thread |
| `seeds/manual_seeds.json` | Functions only reached through pointers (XPP init) |
| `seeds/recomp_extra_seeds.json` | Functions the disassembler misses, found by the upstream recompiler or at runtime (`tools.seed_from_log` on an `[ICALL] Failed to resolve` log). Includes the player-action thunks (`mov eax,[ecx]; mov [ecx+0x3A4],0; jmp [eax+N]`) reached only through runtime-built tables; without them fire, pause and previous weapon did nothing |
| `patches/xboxrecomp-aknavj-turok.patch` | The toolkit changes as one diff against aknavj 11a8aa1 |

## Runtime switches

`main.c` turns these on by default; set one yourself to override it.

| Variable | Why |
|----------|-----|
| `RECOMP_PB_EXEC=1` | Executes the GPU pushbuffer |
| `RECOMP_AC97_READY=1` | Emulated audio; the game's sound init spins forever without it |
| `RECOMP_FB_WINDOW=1` | Opens the game window |
| `RECOMP_NV2A_NATIVE_FENCES=1`, `RECOMP_VBLANK=1`, `RECOMP_TITLE_KEVENTS=1`, `RECOMP_HEAP_RECLAIM=1` | The fork's fence, vblank and event handling; D3D's waits hang without them |
| `RECOMP_PRESENT_SCANOUT=1` | Shows the display buffer the game registered, not whichever surface was drawn last |
| `RECOMP_FPS_CAP=vblank` | One flip per emulated vblank; the title overlay only draws on frames that saw one |
| `RECOMP_USB=1`, `RECOMP_USB_NDP=4`, `RECOMP_USB_PORT=2`, `RECOMP_XINPUT_SLOT=auto` | Emulated USB controller fed by an XInput pad, on the root port XAPI calls controller 1 |

Useful while debugging:

- `RECOMP_PAD_SCRIPT=62000:start,66000:start,...` presses buttons at set
  times (ms since the first controller poll)
- `RECOMP_FLIP_DUMP_DIR=<dir>` with `RECOMP_FLIP_DUMP_AFTER_SEC=<s>`,
  `_EVERY=<n>` and `_COUNT=<n>` saves consecutive frames as BMPs
- `py -3 ..\xboxrecomp_fork\tools\symbolize.py build\Release\turok_evolution.map <RIP>`
  on a `[CRASH]` line
- `RECOMP_HWWATCH=<guest address>` with `RECOMP_HWWATCH_SEC=<s>` sets a
  hardware watchpoint on one guest byte after that many seconds and prints
  each instruction that touches it (symbolize the RIPs as above)
- **F11** in the game window writes `capture_<n>.txt` (every GPU batch of
  the next frame: textures, combiners, depth/blend/fog state, screen extent)
  and `capture_<n>.bmp` beside the exe
- `RECOMP_HITCH_MS=20` logs every frame that took longer than 20 ms, with a
  timestamp and what the frame spent its time on: draw phases, shader
  compiles (count/ms), texture creation and uploads, GPU sync waits, new
  render targets, generic-shader and skipped draws, and the guest's file
  reads. `RECOMP_SHADER_GENERIC=1` draws everything with the generic pixel
  shader, to check it against the compiled ones. The periodic GPU report
  (`RECOMP_PB_REPORT_MS`) stalls ~0.5 s each time, so it is off by default
- `RECOMP_FPS_LOG=5` logs the frame rate and the worst frame every 5
  seconds. Use it to measure speed: a hitch threshold that most frames
  cross logs a line per frame, and those writes slow the game down
  considerably (a run that measured 40 fps that way ran at over 100)
- `RECOMP_DRAW_ONLY=<first>-<last>`, `RECOMP_BATCH_INFO=<s>:<first>-<last>`
  and `RECOMP_DEPTH_DUMP=<s>` isolate, describe and dump GPU batches
