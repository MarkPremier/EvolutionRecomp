#!/bin/bash
# Build a Linux release tarball of the native GPU build.
#
#   tools/package_linux.sh 0.1.0
#
# Builds build/linux-release with the libraries looked up beside the binary
# ($ORIGIN/lib), then packs dist/EvolutionRecomp-linux-x86_64-v<version>.tar.gz:
#   turok_evolution, lib/ (DXVK d3d11 + dxgi, vkd3d-shader), play.sh,
#   run_linux.py, LINUX.md, THIRD_PARTY.md and the libraries' licenses.
# Needs the DXVK and vkd3d prefix from LINUX.md (DEPS, default ../deps).
set -euo pipefail

version="${1:?usage: tools/package_linux.sh <version>}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$(cd "${DEPS:-$repo/../deps}" && pwd)"
prefix="$deps/native"
name="EvolutionRecomp-linux-x86_64-v$version"
stage="$repo/dist/$name"

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib/x86_64-linux-gnu/pkgconfig"
cmake -S "$repo" -B "$repo/build/linux-release" -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_GPU_NATIVE=ON -DCMAKE_SKIP_BUILD_RPATH=ON \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN/lib' >/dev/null
cmake --build "$repo/build/linux-release" --parallel "$(nproc)"

binary="$repo/build/linux-release/turok_evolution"
if ! readelf -d "$binary" | grep -q 'RUNPATH.*\$ORIGIN/lib'; then
    echo "release binary does not look in \$ORIGIN/lib" >&2
    exit 1
fi

if [ -e "$stage" ]; then
    echo "$stage already exists; remove it or pick another version" >&2
    exit 1
fi
mkdir -p "$stage/lib" "$stage/licenses"
install -m 755 "$binary" "$stage/turok_evolution"
strip --strip-unneeded "$stage/turok_evolution"
# Real files under their sonames (what the loader asks for).
for lib in "$prefix/lib/x86_64-linux-gnu/libdxvk_d3d11.so.0" \
           "$prefix/lib/x86_64-linux-gnu/libdxvk_dxgi.so.0" \
           "$prefix/lib/libvkd3d-shader.so.1"; do
    install -m 755 "$(readlink -f "$lib")" "$stage/lib/$(basename "$lib")"
done
strip --strip-unneeded "$stage"/lib/*
install -m 755 "$repo/tools/run_linux.py" "$stage/run_linux.py"
install -m 644 "$repo/LINUX.md" "$stage/LINUX.md"
install -m 644 "$deps/dxvk/LICENSE" "$stage/licenses/DXVK-LICENSE"
install -m 644 "$deps/vkd3d-release-1.19/COPYING" "$stage/licenses/vkd3d-COPYING"
install -m 644 "$deps/vkd3d-release-1.19/LICENSE" "$stage/licenses/vkd3d-LICENSE"

cat > "$stage/play.sh" <<'EOF'
#!/bin/bash
# Play Turok: Evolution (native Linux build).
#   ./play.sh "/path/to/Turok - Evolution (USA)"
# The folder is an extracted USA disc (it holds default.xbe). Settings go in
# front, e.g. RECOMP_RENDER_SCALE=2 ./play.sh "...". See LINUX.md.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ $# -lt 1 ]; then
    echo "usage: $0 \"/path/to/Turok - Evolution (USA)\"" >&2
    exit 2
fi
exec python3 "$here/run_linux.py" --executable "$here/turok_evolution" "$@"
EOF
chmod 755 "$stage/play.sh"

cat > "$stage/THIRD_PARTY.md" <<EOF
# Third-party components

- DXVK $(git -C "$deps/dxvk" describe --tags 2>/dev/null || echo v3.2) (native build): lib/libdxvk_d3d11.so.0,
  lib/libdxvk_dxgi.so.0. zlib/libpng license, licenses/DXVK-LICENSE.
  Source: https://github.com/doitsujin/dxvk
- vkd3d-shader 1.19: lib/libvkd3d-shader.so.1. GNU LGPL 2.1 or later,
  licenses/vkd3d-COPYING and licenses/vkd3d-LICENSE. Dynamically linked; you
  may replace it with your own build of the same interface.
  Source: https://gitlab.winehq.org/wine/vkd3d (tag vkd3d-1.19)

The game data is not included: play needs your own copy of the disc.
EOF

tar -C "$repo/dist" -czf "$repo/dist/$name.tar.gz" "$name"
(cd "$repo/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$repo/dist/$name.tar.gz"
cat "$repo/dist/$name.tar.gz.sha256"
