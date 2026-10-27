# EvolutionDecomp

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
  `RECOMP_SHADOWS=0` turns them off. They reach further and are sharper
  than on the Xbox: the shadow map covers 48 units ahead of the camera at
  2048x2048 (the game's own 16 units at 512x512). `RECOMP_SHADOW_DISTANCE`
  and `RECOMP_SHADOW_MAP` change them; 16 and 512 give the original.
- Known issues: the Xbox version runs at 30 fps and this runs at ~58, which
  may speed up anything the game ties to frame rate; environment-map layers
  (water, Tek bow) and cave lighting are not yet compared with the console.

## What you need

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

   The disc path is hard-coded in `src/main.c` (`YOUR_GAME_XBE_PATH`,
   `YOUR_GAME_DIR`); change it if yours differs.

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

5. **Run**

   ```
   build\Release\turok_evolution.exe 2> stderr.txt
   ```

## Controls

A pad works as on the console. Keyboard and mouse (on by default,
`RECOMP_KBM=0` turns them off; Alt+Esc releases the mouse):

| Action | Keys |
|--------|------|
| Move / strafe | W A S D |
| Look | Mouse |
| Fire | Left mouse |
| Weapon mode | Right mouse |
| Jump / menu select | Space / Enter |
| Next / previous weapon | Wheel down / up, or E / Q |
| Duck | Ctrl or C |
| Zoom | Middle mouse or Z |
| Pause | Esc or Tab |
| Menu back | Backspace |
| D-pad | Arrow keys |

The game ships with Invert Look on; the mouse is inverted back so moving it
up looks up (`RECOMP_KBM_INVERT_Y=0` if you turn the game's option off).
`RECOMP_KBM_SENS` sets the mouse counts for a full turn rate (default 80,
lower is faster), and `RECOMP_KBM_MAP` rebinds keys, e.g.
`A=Space|Return,RT=MouseL,X=WheelDown|E`.

## Project files

| Path | What it is |
|------|------------|
| `src/main.c` | Host entry point: loads the XBE, sets the runtime defaults below, calls the game's entry point |
| `src/recomp_manual.c` | Hand-written overrides: D3D's GetVisibilityTestResult (sub_00194B40) completes unfinished tests as visible; DirectSound's idle-voice handler (sub_001B23DD) ignores voices nobody owns (the emulated APU could report one, and quitting a level crashed); the shadow manager's constructor (sub_0017D5C0) takes its shadow distance and map size from `RECOMP_SHADOW_DISTANCE`/`RECOMP_SHADOW_MAP` |
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
  timestamp, to find periodic stutters. The periodic GPU report
  (`RECOMP_PB_REPORT_MS`) stalls ~0.5 s each time, so it is off by default
- `RECOMP_DRAW_ONLY=<first>-<last>`, `RECOMP_BATCH_INFO=<s>:<first>-<last>`
  and `RECOMP_DEPTH_DUMP=<s>` isolate, describe and dump GPU batches
