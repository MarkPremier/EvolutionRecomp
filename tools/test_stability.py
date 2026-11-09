"""Run focused tests against the real source, without game data (Windows/MSVC).

    py -3 tools/test_stability.py

The renderer tests use D3D11 WARP, so they do not require a physical GPU.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


def game_functions(root, output):
    source = (root / "src/recomp_manual.c").read_text(encoding="utf-8")
    parts = [
        '#define TUROK_MEM32(va) (*(uint32_t *)(memory + (va)))\n',
        'typedef void (*recomp_func_t)(void);\n',
        'extern void sub_000AF2A0_gen(void), sub_000B98D0_gen(void);\n',
        'extern int video_settings_keep_decals(void);\n',
        'recomp_func_t recomp_lookup(uint32_t va);\n',
        function(source, "recomp_func_t recomp_lookup_manual(uint32_t xbox_va)\n{"),
        function(source, "static float guest_float(uint32_t va)"),
        source[source.index("extern void sub_001B23DD_gen(void);"):source.index("void sub_001B23DD(void)")],
        function(source, "void sub_001B23DD(void)"),
        source[source.index("#define AIM_WOBBLE_RATE"):source.index("extern void sub_0011B220_gen")],
        source[source.index("#define KEEP_DECAL_SECONDS"):source.index("static int partdef_is_keepsake")],
        function(source, "void sub_000AF2A0(void)"),
        function(source, "static void keep_decals_frame(void)"),
        source[source.index("#define PARTICLE_STAMPS"):source.index("static int32_t particle_capacity")],
    ]
    for signature in ("static uint32_t *particle_stamp(", "void sub_000B98D0(void)",
                      "static int particle_kept(", "static int particle_kill(",
                      "static int kept_older(", "static void particle_trim("):
        parts.append(function(source, signature))
    (output / "game_overrides_under_test.inc").write_text("\n".join(parts), encoding="utf-8")


def main():
    root = Path(__file__).resolve().parents[1]
    output = root / "build" / "stability_tests"
    output.mkdir(parents=True, exist_ok=True)
    game_functions(root, output)
    environment = {key.upper(): value for key, value in os.environ.items()}
    if not shutil.which("cl.exe"):
        installer = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        install = subprocess.check_output([str(installer), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip()
        setup = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
        setup_script = output / "msvc_environment.cmd"
        setup_script.write_text(f'@echo off\ncall "{setup}" >nul\nset\n', encoding="utf-8")
        environment["PATH"] = str(installer.parent) + os.pathsep + environment.get("PATH", "")
        result = subprocess.check_output(["cmd.exe", "/d", "/c", ".\\" + setup_script.name],
                                         cwd=output, env=environment, text=True)
        for line in result.splitlines():
            key, separator, value = line.partition("=")
            if separator and key:
                environment[key.upper()] = value
    compiler = shutil.which("cl.exe", path=environment.get("PATH"))
    if not compiler:
        raise SystemExit("MSVC cl.exe was not found; install Desktop development with C++.")
    for name, libraries in (
        ("game_fixes.c", []),
        ("game_files.c", ["user32.lib", "gdi32.lib"]),
        ("gpu_shader_cache.cpp", ["d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "user32.lib"]),
        ("apu_voices.c", [str(root / "build/xboxrecomp/src/apu/Release/xbox_apu.lib")]),
    ):
        if len(sys.argv) > 1 and name not in sys.argv[1:]:
            continue
        source = root / "tools/stability" / name
        executable = output / (source.stem + ".exe")
        subprocess.run([compiler, "/nologo", "/W3", "/O2", "/Gy", "/MD", "/EHsc", "/std:c++17", "/utf-8",
                        "/D_CRT_SECURE_NO_WARNINGS", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN",
                        "/D_USE_MATH_DEFINES", f"/I{root.parent / 'xboxrecomp_fork/src'}",
                        f"/I{output}", str(source), f"/Fe:{executable}",
                        "/link", "/OPT:REF", "/INCREMENTAL:NO", *libraries],
                       cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], cwd=output, env=environment, check=True)


if __name__ == "__main__":
    main()
