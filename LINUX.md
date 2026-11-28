# Native Linux port

This `linux-native` branch builds Turok: Evolution as a native Linux program,
separate from the Windows release (whose instructions and figures are in
README.md; Windows code paths are unchanged here). It uses the fork's D3D11
GPU renderer on Vulkan through DXVK-native, vkd3d-shader to compile its
shaders, SDL2 for the window, sound and input, and the DSP56300 audio engine.

Status: plays through menus, saves and levels. On the development laptop
(Ryzen 5 with integrated Radeon graphics) the Chapter 1 jungle holds the
60 fps cap at render scale 1 (about 170 fps uncapped). Not yet available on
Linux: the enhanced Options menu (Video, PC Controls), native ISO import
(use an extracted disc), and Codex's noted sky gap in some views.

## Playing a release

Linux releases are tagged `linux-v*` on GitHub, as pre-releases beside the
Windows ones. Needs a 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04,
Fedora 39, Debian 13 or later), a Vulkan driver (Mesa RADV/ANV, or NVIDIA),
SDL2 and OpenSSL 3. On Ubuntu or Debian:

```sh
sudo apt install libsdl2-2.0-0 libssl3 mesa-vulkan-drivers python3
```

Extract the release and point it at an extracted USA disc (the folder with
`default.xbe`):

```sh
tar xf EvolutionRecomp-linux-x86_64-*.tar.gz
cd EvolutionRecomp-linux-x86_64-*/
./play.sh "/path/to/Turok - Evolution (USA)"
```

Saves, the shader cache and a disc view of symlinks go to
`~/.local/share/evolutionrecomp/linux-native`, apart from any Windows saves.
The first visit to an area may hitch briefly while shaders compile; later
runs use the cache.

## Building from source

Use the matching `linux-native` branch of the toolkit
([MarkPremier/xboxrecomp](https://github.com/MarkPremier/xboxrecomp)) in a
folder named `xboxrecomp_fork` beside this one. The generated guest C
sources must already exist in `src/recomp/gen` (made from your own copy of
the XBE, not kept in the repository). Packages (Ubuntu 24.04):

```sh
sudo apt install build-essential cmake pkg-config libsdl2-dev libssl-dev \
    libvulkan-dev meson ninja-build glslang-tools bison flex libjson-perl python3
```

Build DXVK-native (v3.2) and vkd3d 1.19 into a local prefix, then the game:

```sh
PREFIX=$PWD/../deps/native
meson setup ../deps/build-dxvk ../deps/dxvk --buildtype release --prefix "$PREFIX"
ninja -C ../deps/build-dxvk install
(mkdir -p ../deps/build-vkd3d && cd ../deps/build-vkd3d && \
 ../vkd3d-release-1.19/configure --prefix="$PREFIX" --disable-tests --disable-demos \
     --without-opengl --without-xcb --without-spirv-tools && make -j6 install)
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:$PREFIX/lib/x86_64-linux-gnu/pkgconfig
cmake -S . -B build/linux-native -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_GPU_NATIVE=ON
cmake --build build/linux-native --parallel 6
python3 tools/run_linux.py "/path/to/Turok - Evolution (USA)"
```

`tools/package_linux.sh <version>` builds a release tarball (binary, the
DXVK and vkd3d libraries beside it in `lib/`, launcher and licenses).

The launcher creates a Linux-only disc view under
`$XDG_DATA_HOME/evolutionrecomp/linux-native` (normally
`~/.local/share/evolutionrecomp/linux-native`). It links the original disc
assets and keeps its own TDATA/UDATA saves and runtime cache there. The
Windows settings patch and Windows saves are excluded. Override the location
with `--state-dir`, or use `--executable` for a binary built elsewhere.

## Controls

Keyboard and mouse play as in the Windows build (`RECOMP_KBM`, on by
default), merged onto player one, so a controller keeps working beside them.
An SDL-compatible controller can be connected before or during play.

Click in the window to capture the mouse (or touchpad): the pointer hides and
turns the view. Alt+Esc, or switching to another window, releases it; click
again to recapture. Keys are read only while the window has the focus.

| Keys / mouse | Action (Xbox control) |
|--------------|-----------------------|
| W A S D | Move (left stick) |
| Mouse / touchpad | Look (right stick) |
| Left mouse | Fire (right trigger) |
| Right mouse | Weapon mode (left trigger) |
| Space / Enter | Jump, menu select (A) |
| Backspace | Menu back (B) |
| E / wheel down, Q / wheel up | Next / previous weapon (X / Y) |
| Ctrl or C | Duck (white) |
| Middle mouse or Z | Zoom (right stick click) |
| Esc or Tab | Pause (Start); Esc is Back in menus |
| Arrow keys | Menu navigation |

Mouse speed: `RECOMP_KBM_SENS` is the mouse counts for a full turn rate
(default 80, lower is faster; a touchpad may want 30-40).
`RECOMP_KBM_INVERT_Y=1` inverts looking up and down. `RECOMP_KBM_MAP`
rebinds keys (see the toolkit's `src/input/xinput_device.c`). On GNOME,
Settings > Mouse & Touchpad > "Disable while typing" stops a touchpad from
turning while a movement key is held. `RECOMP_KBM=0` returns to the simple
keyboard pad (`RECOMP_KEYBOARD`): keypad 8/2/4/6 move, I/J/K/L look.

The enhanced Windows settings menu (Options > PC Controls, Video) is not yet
available. Closing the window exits the game.

## Settings

Set these in front of the launch command, for example
`RECOMP_RENDER_SCALE=2 ./play.sh "/path/to/disc"`:

| Variable | Effect |
|----------|--------|
| `RECOMP_RENDER_SCALE=<n>` | Internal resolution multiple (launcher default 1; 2 or more is sharper and needs a stronger GPU) |
| `RECOMP_FPS_CAP=0` | No 60 fps cap (`=<hz>` for a fixed rate) |
| `RECOMP_KBM_SENS=<n>` | Mouse counts for a full turn rate (default 80, lower is faster) |
| `RECOMP_KBM_INVERT_Y=1` | Invert looking up and down |
| `RECOMP_FPS_LOG=5` | Log the frame rate every 5 seconds |
| `RECOMP_BLOCK_SLEEP=0` | Spin instead of sleeping while the frame cap holds a flip |
