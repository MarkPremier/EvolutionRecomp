# EvolutionDecomp

A static recompilation of **Turok: Evolution** (Xbox, USA) to native Windows,
built with [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

The game's x86 code is translated to C and compiled into a Windows `.exe`;
the Xbox kernel, GPU (NV2A) and audio (MCPX APU) are provided by the
xboxrecomp runtime. **No game code or data is in this repository.** You need
your own disc image, and the C code is generated from it on your machine.

## Status

- Boots, plays the Acclaim logo and the intro movie, reaches the title screen.
- Rendering runs on the toolkit's CPU rasterizer: about 9 fps, and the title
  screen's 3D backdrop flickers. Next step is a Direct3D 11 GPU backend.
- Controller input is untested.

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
  xboxrecomp/                      the toolkit, patched
  turok_evolution/                 this repository
```

1. **Extract the disc**

   ```
   xdvdfs unpack "Turok - Evolution (USA).iso" "turok/Turok - Evolution (USA)"
   ```

   The disc path is hard-coded in `src/main.c` (`YOUR_GAME_XBE_PATH`,
   `YOUR_GAME_DIR`); change it if yours differs.

2. **Get the toolkit at the tested commit and apply this project's fixes**

   ```
   git clone https://github.com/sp00nznet/xboxrecomp.git
   cd xboxrecomp
   git checkout f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b
   git apply ../turok_evolution/patches/xboxrecomp-turok.patch
   ```

   The patch fixes three toolkit bugs this game hits:
   the utility-drive cluster size (the game asks for 64 KB clusters and was
   rebooting to the dashboard), RunPushBuffer handling in the pushbuffer walker
   (desyncs, crashes and missing geometry), and garbage semaphore releases.

3. **Generate the C code** (from inside `xboxrecomp/`)

   ```powershell
   $xbe = "../turok/Turok - Evolution (USA)/default.xbe"
   py -3 -m tools.xbe_parser $xbe --json "../turok/Turok - Evolution (USA)/turok_analysis.json"
   py -3 -m tools.disasm $xbe --force --seed-functions ../turok_evolution/seeds/manual_seeds.json
   py -3 -m tools.func_id $xbe
   py -3 -m tools.abi_analysis $xbe
   py -3 -m tools.recomp $xbe --all --split 250 --gen-dir ../turok_evolution/src/recomp/gen --exclude-manual ../turok_evolution/src/recomp_manual.c
   ```

   Two things that matter:
   - Do **not** pass `--text-only` to `tools.disasm`. The Xbox libraries the game
     links (D3D, DSOUND, XPP, XMV) are barely swept without it.
   - `--exclude-manual` is required. Without it the hand-written overrides in
     `src/recomp_manual.c` conflict with the generated code.

4. **Build** (from `turok_evolution/`, in a Developer PowerShell for VS)

   ```
   cmake -S . -B build
   cmake --build build --config Release --parallel
   ```

5. **Run**

   ```
   build\Release\turok_evolution.exe 2> stderr.txt
   ```

## Project files

| Path | What it is |
|------|------------|
| `src/main.c` | Host entry point: loads the XBE, starts the runtime, calls the game's entry point |
| `src/recomp_manual.c` | Hand-written overrides of game/XDK functions (the D3D fence wait) |
| `seeds/manual_seeds.json` | Function addresses the disassembler cannot find on its own |
| `patches/xboxrecomp-turok.patch` | Fixes to the toolkit, against the commit above |

## Runtime switches

`main.c` turns these on by default; set one to `0` to override.

| Variable | Why |
|----------|-----|
| `RECOMP_PB_EXEC=1` | Executes the GPU pushbuffer; D3D's fence waits never return without it |
| `RECOMP_AC97_READY=1` | Emulated audio; the game's sound init spins forever without it |
| `RECOMP_FB_WINDOW=1` | Opens the game window |

Useful while debugging: `RECOMP_WATCHDOG_SECS=N` (dump state on a hang),
`RECOMP_FB_DUMP=<path>` with `RECOMP_FB_WINDOW_DUMP_EVERY=<frames>` (screenshots),
and `tools/symbolize.py build/Release/turok_evolution.map <RIP>` on a `[CRASH]` line.
