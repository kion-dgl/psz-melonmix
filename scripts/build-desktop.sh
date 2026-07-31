#!/usr/bin/env bash
# Build the desktop (Qt) melonDS with the PSZ overlay. Linux and Windows both
# use this; the Windows runner supplies MSYS2's toolchain.
#
# Deps (Debian/Ubuntu):
#   build-essential cmake extra-cmake-modules ninja-build libsdl2-dev libpcap-dev
#   libarchive-dev libzstd-dev libenet-dev libslirp-dev libfaad-dev
#   qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libqt6svg6-dev
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${PSZ_WORK:-$ROOT/build}/melonDS"
[ -d "$DIR" ] || "$ROOT/scripts/bootstrap.sh" desktop
cmake -S "$DIR" -B "$DIR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$DIR/build" -j"$(nproc 2>/dev/null || echo 4)"
echo ">> built: $DIR/build/melonDS"
