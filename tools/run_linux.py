#!/usr/bin/env python3
"""Run the native CPU port with its own disc view and saves."""
import argparse
import os
from pathlib import Path
import sys


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_dir", type=Path, help="extracted USA disc directory")
    parser.add_argument("--executable", type=Path,
                        default=repo / "build/linux-native/turok_evolution")
    parser.add_argument("--state-dir", type=Path,
                        help="Linux disc view, saves and cache (default: XDG data directory)")
    parser.add_argument("--prepare-only", action="store_true", help="prepare the disc view without launching")
    args = parser.parse_args()
    source = args.game_dir.expanduser().resolve()
    data_home = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    state = (args.state_dir or data_home / "evolutionrecomp/linux-native").expanduser().resolve()
    executable = args.executable.expanduser().resolve()
    names = ["default.xbe", "Game.txt", *(f"supertree{i}.tre" for i in range(6)), "Data"]
    try:
        entries = {p.name.lower(): p for p in source.iterdir()}
        files = {name: entries[name.lower()] for name in names}
    except (OSError, KeyError) as error:
        parser.error(f"incomplete extracted disc at {source}: {error}")
    if not args.prepare_only and not os.access(executable, os.X_OK):
        parser.error(f"native executable not found or not executable: {executable}")
    view = state / "disc"
    if source == view or source in view.parents or view in source.parents:
        parser.error("the Linux state directory must be separate from the source disc")
    view.mkdir(parents=True, exist_ok=True)
    marker = view / ".linux-native-view"
    if not marker.exists() and any(view.iterdir()):
        parser.error(f"refusing to reuse an unmanaged directory: {view}")
    marker.write_text("Managed native Linux disc view. Saves in TDATA and UDATA stay here.\n")
    for name, target in files.items():
        link = view / name
        if link.is_symlink():
            if link.resolve() == target.resolve():
                continue
            link.unlink()
        elif link.exists():
            parser.error(f"expected a managed symlink: {link}")
        link.symlink_to(target, target_is_directory=target.is_dir())
    # Only original disc assets enter this view. The Windows settings patch,
    # TDATA and UDATA are never linked or modified.
    for name in ("TDATA", "UDATA"):
        path = view / name
        if path.is_symlink():
            parser.error(f"save directory must not be a symlink: {path}")
        path.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment["XDG_DATA_HOME"] = str(state / "runtime")
    environment.setdefault("RECOMP_KEYBOARD", "1")
    environment.setdefault("RECOMP_USB_PADS", "1")
    # The GPU renderer's automatic render scale follows the monitor: 4x on a
    # 2256x1504 laptop panel, which saturated the integrated Radeon (title
    # screen ~4 fps). With the straight-line combiner shaders the first level
    # holds the 60 fps cap at scale 1 (GPU about half busy; ~170 fps
    # uncapped) and at scale 2 (GPU ~96% busy, so busier scenes drop). Set
    # RECOMP_RENDER_SCALE to override.
    environment.setdefault("RECOMP_RENDER_SCALE", "1")
    print(f"Linux disc view and saves: {view}", flush=True)
    if args.prepare_only:
        return 0
    os.chdir(state)
    os.execve(executable, [str(executable), str(view)], environment)


if __name__ == "__main__":
    sys.exit(main())
