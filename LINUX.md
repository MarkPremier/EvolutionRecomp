# Native Linux port

This `linux-native` branch builds the fork natively using its CPU renderer,
SDL2 presentation and stereo audio, and the DSP56300 engine. It has booted
through the intro movie and menus, created and reloaded a save, and reached
Chapter 1's first-person view with the weapon and health HUD. Rendering gaps
remain, and the CPU renderer is slow (roughly 1–3 fps in the 3D front end,
around 1 fps in the initial level on the development laptop). This is not a
finished GPU port. The Windows release instructions and performance figures
in README.md describe that release.

## Build

Use the matching `linux-native` runtime worktree beside this one, named
`xboxrecomp_fork`. The generated guest C sources must already exist in
`src/recomp/gen`; see the handoff notes for the disassembly/recompilation
steps. Regenerate with this branch's generator after changing its frame
handling. Native dependencies are CMake, a C/C++ toolchain, SDL2, OpenSSL,
Epoxy/OpenGL and the FFmpeg development libraries.

```sh
cmake -S . -B build/linux-native -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-native --parallel 6
python3 tools/run_linux.py "/path/to/Turok - Evolution (USA)"
```

The launcher creates a Linux-only disc view under
`$XDG_DATA_HOME/evolutionrecomp/linux-native` (normally
`~/.local/share/evolutionrecomp/linux-native`). It links the original disc
assets and keeps its own TDATA/UDATA saves and runtime cache there. The
Windows settings patch and Windows saves are excluded. Override the location
with `--state-dir`, or use `--executable` for a binary built elsewhere. It
requires an extracted USA disc; native ISO import is still pending.

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

## Next stage

Claude's planned GPU route is DXVK-native for D3D11 on Vulkan, with
vkd3d-shader for HLSL compilation. Sources are in the sibling `deps` folder;
this backend is not yet integrated. The owner installs system packages:

```sh
sudo apt install meson glslang-tools libvulkan-dev bison flex libjson-perl
```

The staged `deps/vkd3d-release-1.19` includes generated configure and IDL
files, avoiding the additional Autotools and IDL compiler requirements of
the Git checkout. Its configure check currently stops at missing Perl JSON.
