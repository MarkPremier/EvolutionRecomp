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
  58 fps, without flicker. Played through to the pteranodon flying level.
- Controller works (any XInput pad, tested in play), as controller 1: move,
  look, jump, fire, weapon switching, pause and rumble.
- PlayStation controllers work natively too (DualSense, DualSense Edge,
  DualShock 4; USB or Bluetooth, with rumble), read through HID with no
  Steam Input or DS4Windows: cross is A, circle B, square X, triangle Y,
  options Start, create/share Back. So does the Switch 2 Pro Controller over
  USB (with Steam closed: Steam holds its USB interface), its buttons placed
  by position as on an Xbox pad (the bottom one is A), plus Start, minus
  Back. `RECOMP_HID_PADS=0` turns this off. Both the DualSense and the
  Switch 2 Pro Controller are tested.
- Up to four controllers for split-screen, any mix of kinds (two DualSenses
  as well as an Xbox pad and a Switch 2 Pro Controller): the game sees all
  four controller ports, and XInput pads take controllers 1, 2... in
  Windows' order, then PlayStation and Switch 2 pads in the order they were
  connected. A pad keeps its number while connected. Each shows it: the
  DualSense's player lights, the DualShock 4's light bar (blue, red, green,
  pink) and the Switch 2's player lights. `RECOMP_USB_PADS=1` offers the game
  only controller 1.
- Keyboard and mouse work too, merged onto controller 1 (see Controls).
- Dinosaurs and Turok cast shadows, with filtered (PCF) edges, the Xbox's
  own or enhanced (Shadows, in Video Options; `RECOMP_SHADOWS=0` turns them
  off). Xbox is the game's shadows as they were. Enhanced is every change
  the recompilation makes to them:
  - a 2048x2048 shadow map instead of the game's 512x512, drawn four times
    finer again on the GPU (8192 texels a side): sharp edges, no blocks;
  - exact (float) shadow depth instead of the Xbox's 16-bit: there one
    depth step is several units deep, deeper than a creature, so a wall or
    building just in front of a body took its shadow as streaks;
  - four times the reach: the map covers 64 units ahead of the camera, not
    16, and 12 around the player (32 in the indoor levels, pods 05, 06 and
    10-12, where the map's edge beside you showed as a straight lit line on
    shadowed walls), so creatures beside and behind you (and at the sides
    of a widescreen view) cast them too;
  - creatures out of view cast theirs; the game drew only those of
    creatures on screen;
  - the ambient birds (FlyingIndigenousAI) cast none once more than 10 units
    above the player's height: their blobs landed far below them. Birds
    taking off nearby keep theirs.

  The longer reach has a side effect the Xbox never shows: the game draws
  only creatures into the shadow map, not floors, so an enemy on a balcony
  or upper gallery (Halls of Battle) casts down through its floor onto the
  hall below. The map's size changes when the game next starts; the rest at
  once. `RECOMP_SHADOW_DISTANCE`, `RECOMP_SHADOW_AROUND`, `RECOMP_SHADOW_MAP`,
  `RECOMP_SHADOW_OFFSCREEN`, `RECOMP_SHADOW_EXACT`, `RECOMP_SHADOW_SCALE`,
  `RECOMP_SHADOW_BIRD_HEIGHT` and `RECOMP_SHADOW_BIRDS=1` tune Enhanced.
- The window shows frames straight from the GPU through a swap chain, so it
  resizes and maximises freely (the frame stays 4:3, letterboxed), opens at
  the largest multiple of 640x480 that fits the screen, and **Alt+Enter**
  toggles borderless fullscreen. `RECOMP_VSYNC=1` presents on the monitor's
  refresh; `RECOMP_PRESENT=gdi` goes back to the old 640x480 window.
- On a laptop with two GPUs the game runs on the faster one (the NVIDIA or
  AMD card, not the integrated Intel graphics); `turok_log.txt` names the
  GPU it used on its `[GPU-D3D11] adapter:` line.
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

If the game crashes, the details are in `turok_log.txt` beside the program
(the run before is kept as `turok_log.prev.txt`, so starting again does not
lose it), and a crash dump in the `CrashDumps` folder beside it
(`turok_evolution_<date>_<time>.dmp`, under a megabyte: every thread's
registers and call stack). Please include both when reporting a crash. A run
with stderr redirected writes the log there instead.

The game's shaders are translated to HLSL and compiled as each one first
appears. Compiled shaders are kept in `%LOCALAPPDATA%\xboxrecomp\shadercache`,
so each one is compiled once per machine, and new ones compile in the
background rather than in the frame that needs them: until a pixel shader is
ready its draws use a slower generic shader, and a draw whose vertex shader is
still compiling is skipped for those few frames. The first time a scene is
ever seen may show that briefly; after that it plays without compiling at all.
`RECOMP_SHADER_CACHE=<folder>` moves the cache (`0` turns it off) and
`RECOMP_SHADER_ASYNC=0` compiles in the frame instead, as before.
Vertex layouts are cached separately for each attribute mask. A shader
that fails compilation or creation is remembered for the rest of the run,
using the generic pixel shader where available, rather than retried on
every draw. Restart the program to retry after fixing the compiler or driver.

Audio voice updates synchronize with their own mixer slice: the mixer waits
while the game has a voice locked for a restart or data change. A one-shot
sound also drains its resampler before being reported finished, preserving
short effects and the ends of longer samples.
An idle notification that became stale after a voice restart is rejected
by the shared DirectSound completion handler, covering both interrupt and
synchronous servicing before either can unlink the new playback. This
matters for rapid repeated effects such as pistol shots. Set
`RECOMP_APU_EVENT_TRACE=1` to log starts, first mixed contributions and stale
idle notifications while diagnosing audio.

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
   the program and its window that release's icon, and the menus its mouse
   arrow (taken from your copy at configure time, the icon by
   `tools/extract_icon.py`; neither is in this repository).
   `py tools/strip_icon.py <exe>` removes the icon from a copy before it is
   shared; the arrow stays.

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
| A weapon directly | 1 to 0 (see below) |
| Duck | Ctrl |
| Zoom | Middle mouse |
| Pause | Esc |
| Menu select / back | Enter or Space / Esc or Backspace |
| Menu up / down / left / right | Arrow keys |

The number keys pick a weapon you have, with ammunition, the way the next
and previous weapon buttons would reach it. Press a number to select its
slot directly. Press it again to cycle only through that slot's weapons,
skipping any you do not have or cannot use. Holding the key does not cycle:

| Key | Weapon |
|-----|--------|
| 1 | War Club |
| 2 | Bow, Tek Bow |
| 3 | Pistol |
| 4 | Shotgun |
| 5 | Rocket Launcher |
| 6 | Plasma Cannon (Tek Weapon) |
| 7 | Minigun |
| 8 | Flamethrower |
| 9 | Gravity Disruptor |
| 0 | Grenades, Spider Mines, Dark Matter Cube |

The Crossbow also shares key 2 if available. Number keys work in
single-player and follow `RECOMP_KBM`; in split-screen, change weapon with
next and previous weapon.

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

**Options > PC Settings**, under PC Controls, holds the mouse's settings:
Mouse look, Mouse speed, Flight mouse, Flight speed and Flight invert (see
the settings table below).

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
`pc_game_dir` (see Button icons below), or else the copy a build made with
`RECOMP_PC_EXE` carries; without either it is Windows' own.

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
| Mouse look | raw, stick (raw falls back to stick in vehicles and turrets; on the PC Settings page, kept with these settings) | |
| Mouse speed | 1 to 10 (5 by default; each step 1.4x; on the PC Settings page, kept with these settings) | |
| Flight mouse | on, off (the mouse steers the pteranodon; on the PC Settings page, kept with these settings) | |
| Flight speed | 1 to 10 (5 by default; each step 1.4x; the pteranodon's mouse steering; on the PC Settings page, kept with these settings) | |
| Flight invert | off, on (the mouse up the screen dives instead of climbing; on the PC Settings page, kept with these settings) | |
| Pad aim | original, modern (original by default; on the PC Settings page) | |
| Draw distance | normal, 2x, 4x, 8x (2x by default) | |
| Heat haze | off, xbox, enhanced (enhanced by default; off removes distortion; xbox keeps the original effect; enhanced refracts the full-resolution scene with filtered offsets and feathered edges) | |
| Shadows | xbox, enhanced (enhanced by default; see Shadows above -- the shadow map's size changes when the game next starts) | `RECOMP_SHADOW_EXACT`, `RECOMP_SHADOW_SCALE` override parts |
| Keep bodies | off, on (off by default; also keeps decals; always off in the flying levels, and bodies are not kept in the stealth level) | |

The page shows ten rows at a time and scrolls: it follows the highlight as you move up and down, and the mouse wheel scrolls it (click a row, or press left and right, to change its value).

For high resolutions, use **Heat haze: Enhanced**. Its signed distortion
offsets are decoded before filtering, so small ripples do not turn into
abrupt jumps across texture cells. The filtered refraction movement is
boosted to keep explosions visible. Each particle fades out towards its edge
instead of pasting a square of a low-resolution screen copy over the scene.
Enhanced omits the dark matter cube's legacy dark overlay, including its
halo, because that layer exposes a large rectangular backdrop in the final
flash. Its heat distortion follows the game's original lifetime without an
extra early fade. The explosion's bright particles and lighting remain.
Xbox retains the full original effect. Weapon heat keeps its original animated warp and opacity,
including the rocket launcher's barrel haze.
**Off** removes the screen distortion while keeping the flames, smoke,
explosion particles and lighting. The setting applies immediately and is
saved between runs. `RECOMP_HEAT_TRACE=1` logs the first refraction batches
for diagnostics.
Older `original` settings load as Xbox; `smooth` and `sharp` load as Enhanced.

An environment variable set by hand still wins at start-up.

**Pad aim: Modern**, under **Options > PC Settings**, replaces the
on-foot camera's per-axis deadzone and stepped response with a 15% circular
deadzone and a linear response. It preserves the stick's direction and
removes the original per-axis easing. The game's look sensitivity, zoom
scaling, Invert Look and pitch limits still apply, including in split-screen.
The setting applies immediately and is saved as `controller_aim=modern` in
`turok_settings.ini`. Original retains the console behaviour and is the default
for existing and new settings. Movement, flying and raw mouse look keep their
existing controls; Mouse look: Stick uses the selected controller response.

- **Widescreen** widens the view rather than stretching it (Hor+): the game's
  3D programs project through c96 (rigid) or c187 (skinned), and that row is
  narrowed for draws to the screen. 2D draws stay at 4:3 in the middle unless
  they reach the screen's edge (fades, the damage flash), so the HUD keeps its
  shape. The front end, movies, loading screens and in-engine cutscenes (framed
  for 4:3, with their own bars; StartCinema/EndCinema, `sub_00164230`/
  `sub_001642A0`) stay 4:3. A level is 4:3 from its load until its loading
  screen comes down (`sub_000FFF20`), so that screen holds one size. The window
  opens at the chosen shape.
- **Loading screens** stay up at least 3 seconds (`RECOMP_LOADING_HOLD_MS`,
  0 for none): levels load here in under a second, so the chapter picture
  and its text otherwise flash past.
- **Frame limit**: the game steps by a measured clock (`sub_00028B40`), so it
  runs at the right speed at any rate. The front end stays at 60, as its
  movies are paced by the vertical blank. The aim wobble of scoped weapons
  and fully drawn bows was the exception: it stepped once a frame, so above
  60 fps it wandered faster and further the higher the rate (impossible to
  aim at 120 and up, the pistol scope worst). It now steps at most 60 times
  a second per weapon instance, including in split-screen; at 60 fps and
  below it is as before.
- **Button icons, pc**: the menus' A and B prompts become Enter and Esc
  keycaps. Built-in prompts work with an Xbox-only installation. The PC
  release's icons can optionally be read from a PC install, which you name in
  `turok_settings.ini`: `[paths]` then `pc_game_dir=<folder with data\Textures>`
  (or `RECOMP_PC_GAME_DIR`). They are swapped in the renderer, by a hash of
  the Xbox textures. In menus Escape is Back (and resumes from the pause
  menu); in play it pauses (`RECOMP_KBM_MENU_MAP`).

- **PC Controls**: saved bindings apply before the first input poll,
  including fire, weapon mode, jump, duck, weapon cycling, zoom and pause.
  Arrow keys navigate menus independently; in gameplay they only perform
  an action if you assign them to it in PC Controls.

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
- **Flight mouse** (on by default): on the pteranodon the mouse steers it,
  whichever Mouse look is chosen: moving it pushes a stick of its own that
  springs back to the centre in about a sixth of a second, added to the
  flight's stick axes (`sub_001424B0`). The mouse up the screen climbs, as it
  looks up on foot (**Flight invert** turns that over). **Flight speed** sets
  how far a move steers (at 5, about 500 counts push the stick all the way);
  Mouse speed does not apply.
- **MSAA** applies to the screen's targets (multisampled copies resolved
  whenever the game reads them); it combines with the render scale.
  **Anisotropy** applies to mipmapped textures.
- **Draw distance**: grass, plants and other actors with a fade distance
  (A_NDD where they start to fade, A_MDD where they are gone: 15 to 25 units
  for grass) are drawn that many times further. The fade (`sub_0003C9E0`)
  reads the two from a block at actor +0x1CC; they are scaled while it runs.
  Butterflies' update distance (AU_D, actor +0x1D0, tested in
  `sub_0003B110`), out to which an actor animates and moves, is scaled with
  it, so they are not drawn frozen. Visible weapon and item pickups keep
  updating at every draw-distance setting, so their rotation and bobbing do
  not freeze beyond the original short update radius. Off-screen pickups
  and other actors retain their original update limits.
- **Keep bodies**: the dead stay where they fell. A dead AI actor's update
  (`sub_0013D000`) counts time since death at +0x52C; past 3.5 s it fades
  the body and sinks it into the ground, and at 5 s removes it. With the
  setting on that clock is held at 2 s, except for actors that respawn
  (+0x5AA). Bodies are not kept in the stealth level (pod 05), whose alarms
  were built around the dead disappearing: an alarm box stands down only 10 s
  after no alerted Sleg is left near it (`sub_00128380` counts them from the
  Sleg list, where a kept body stayed), and sends the nearest Sleg to raise
  it, so with bodies kept the alarm never ended, the security gates stayed
  shut, and reinforcements and lockdowns misbehaved. Its decals are still
  kept. `RECOMP_CORPSE_TRACE=1` logs deaths, fades and actor changes;
  `RECOMP_DEBUG_KILL=<s>` zeroes the dinosaurs' health that long into a level.
- The same setting keeps decals: blood splats, bullet holes, scorch marks and gibs last ten
  minutes instead of 3 to 20 seconds. They are particles, each kind a PARTDEF
  text the particle system parses as a level loads (`sub_000B0110`); its eighth
  value, the life in seconds, is stored by `sub_000AF2A0`. Definitions whose
  texture or mesh is a splat, bullet hole, scorch or gore piece get the longer
  life; switching the setting re-applies it at once, and off leaves every
  life as the game has it. `RECOMP_PARTICLE_TRACE=1` lists the definitions.
  Effect parts with attached emitters keep their original lifetime
  (`sub_000AF9A0`): some steam controllers reuse a bullet-hole texture, and
  extending them made pipe steam accumulate and overlap with Keep bodies on.
  Explosion fireballs (`1txt_fireB3_.tga`) are not kept: they play their
  texture over their life, so ten minutes of it hid the explosions and their
  build-up crowded out new effects such as rocket smoke trails. In the flying
  levels (the "flight" chapters, pod 02, 09, 13 and 17) neither bodies nor
  decals are kept.
- **Particle limit**: a level's particle manager holds at least
  `RECOMP_PARTICLES` particles (default 4000, 1000 to 20000; the game asks
  for 1000). Before an effect is emitted (`sub_000B9700`) the game drops it
  by chance once the particles out pass its priority's share (40 to 70% of
  the limit), so kept decals used to stop new effects, such as the blood from
  a severed neck. Now, as an effect nears its share, the oldest kept decals
  and gibs are removed first (`sub_000B98D0` stamps each particle as it is
  added). The candidates are scanned and sorted at most once per frame,
  including when none can be removed; further emission requests reuse that
  result. A new particle at a recycled address is left alone. Definition
  tracking is cleared when changing worlds, and a reparsed definition
  replaces its old record. The log reports removals and refusals at most
  every ten seconds.
- **Close-range shots (issue #11)**: the original
  emitter scheduler can suppress pellets and casings when their source fails
  the camera visibility check, even when the parent effect keeps updating.
  Emitters referencing the player-weapon resource namespace (`marc\\weapons\\`)
  now use the original `NoCull` emission flag. A generated-code regression
  reproduces delayed creation and verifies immediate creation with this flag.
  Emission rates, distance limits, particle budgets and render clipping remain
  in effect. Gameplay testing confirmed that shots and casings now work at
  point-blank range. That emission fix leaves collision and impact placement
  unchanged; the shotgun alignment correction is described below.
- **Shotgun impact alignment**: the original
  pellet effect inherits the held gun's offset from the aim camera. This can
  put point-blank hits below and beside the crosshair. Shotgun ballistic
  effects now start at their player's camera position, retaining their
  original orientation and pellet spread. The single shot and two-, three-
  and four-shell attacks share this correction. Shells, muzzle flashes and
  impact effects keep their original positions. A regression runs the real
  actor spawn routine and verifies the origin and guest calling convention.
  Gameplay testing against the reported wall confirmed the impact pattern
  is now centred around the crosshair.
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

Run `py -3 tools/test_stability.py` on Windows with MSVC installed for the
decal lifetime, split-screen wobble, particle cleanup, extraction failure,
shader-cache, heat-haze, controller aiming, pickup animation distance, settings migration and audio-voice
regression checks. Run after building the
Release runtime (the audio test links its resampler library). They need no
disc files; the renderer checks use D3D11 WARP, Microsoft's software device,
and the audio checks use generated PCM. When generated game code is present,
the controller checks also run its on-foot update around the native override,
and the pickup checks run the original visibility and update-distance gates.

| Path | What it is |
|------|------------|
| `src/main.c` | Host entry point: loads the XBE, sets the runtime defaults below, calls the game's entry point |
| `src/game_files.c` | Finds the player's USA disc files, checks `default.xbe`, and extracts a disc image (XDVDFS) into `game` |
| `src/video_settings.c` | The Video Options page: builds `patch0.tre` from the disc's menus (its `.tre` and screen formats are described there), keeps `turok_settings.ini`, and applies the page's values through the history values the menu writes |
| `src/recomp_manual.c` | Hand-written overrides: D3D's GetVisibilityTestResult (sub_00194B40) completes unfinished tests as visible; DirectSound's idle-voice handler (sub_001B23DD) ignores voices nobody owns (the emulated APU could report one, and quitting a level crashed); with enhanced shadows, the shadow manager's constructor (sub_0017D5C0) takes its map size from `RECOMP_SHADOW_MAP`, the light camera's fit (sub_0017DAF0, through the camera FOV setter sub_0001FD20) its distance and a wider area around the player, and the shadow caster check (sub_000F21B0) passes creatures out of view; the box-in-view test (sub_00021A90) settles 95% of its calls natively, with the original's x87 roundings (`RECOMP_CULL_VERIFY=1` checks every answer against the original); D3D KickOff (sub_001984B0/sub_00198480) skips the write-combine flush handshake and the vblank handler (sub_0019D580) drops PMC_INTR's PCRTC bit before its acknowledge spin -- both waited on the toolkit's GPU thread; the movement integrator (sub_00042450) takes a flight path's orientation without rotation collision (Escape Flight's ships tumbled at the ceiling at 120 fps); the aim wobble of scoped guns (sub_0011B220) and drawn bows (sub_0011CD30, sub_0011E9E0, and the update sub_00122CD0 through the view turn sub_001421B0 and aim setter sub_00038E10) steps at most 60 times a second |
| `seeds/manual_seeds.json` | Functions only reached through pointers (XPP init) |
| `seeds/recomp_extra_seeds.json` | Functions the disassembler misses, found by the upstream recompiler or at runtime (`tools.seed_from_log` on an `[ICALL] Failed to resolve` log). Includes the player-action thunks (`mov eax,[ecx]; mov [ecx+0x3A4],0; jmp [eax+N]`) reached only through runtime-built tables; without them fire, pause and previous weapon did nothing. `py -3 tools/scan_missed_functions.py <default.xbe> src/recomp/gen` lists code still missing (after switch tables, in gaps) to seed; its remaining five hits are data or interior addresses |
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
