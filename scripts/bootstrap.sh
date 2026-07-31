#!/usr/bin/env bash
# Assemble a buildable tree: clone an upstream at its pinned rev, copy our
# portable sources in, then apply the thin integration patches.
#
#   ./scripts/bootstrap.sh desktop     -> build/melonDS
#   ./scripts/bootstrap.sh android     -> build/melonDS-android
#   ./scripts/bootstrap.sh all
#
# Idempotent: re-running resets the clone to the pinned rev first, so a failed
# or half-applied run never leaves a tree that builds something unexpected.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${PSZ_WORK:-$ROOT/build}"
mkdir -p "$WORK"

# Read one key out of upstreams.toml without needing a TOML parser.
pin() {
    awk -v sect="[$1]" -v key="$2" '
        $0 == sect { in_s = 1; next }
        /^\[/      { in_s = 0 }
        in_s && $1 == key { gsub(/"/, "", $3); print $3; exit }
    ' "$ROOT/upstreams.toml"
}

# Clone (or reuse) and hard-reset to the pinned rev.
fetch() {
    local name="$1" dir="$2" url rev
    url="$(pin "$name" url)"; rev="$(pin "$name" rev)"
    [ -n "$url" ] && [ -n "$rev" ] || { echo "!! no pin for $name" >&2; exit 1; }

    if [ ! -d "$dir/.git" ]; then
        echo ">> cloning $name"
        git clone -q "$url" "$dir"
    fi
    echo ">> $name -> $rev"
    git -C "$dir" fetch -q origin "$rev" 2>/dev/null || git -C "$dir" fetch -q origin
    git -C "$dir" checkout -q "$rev"
    # Discard any previous run's patches AND any files we copied in, so the tree
    # is exactly upstream before we touch it.
    git -C "$dir" reset -q --hard "$rev"
    git -C "$dir" clean -qfd
}

apply_patches() {
    local dir="$1" pdir="$2"
    for p in "$pdir"/*.patch; do
        [ -e "$p" ] || continue
        echo "   applying $(basename "$p")"
        # No fuzz. A patch that no longer applies cleanly means upstream moved
        # under a hook, and that should fail loudly here rather than be papered
        # over and discovered as strange behaviour later.
        git -C "$dir" apply --whitespace=nowarn "$p"
    done
}

desktop() {
    local dir="$WORK/melonDS"
    fetch melonds "$dir"
    cp "$ROOT/psz/PSZPlugin.h" "$ROOT/psz/PSZPlugin.cpp" "$dir/src/"
    cp "$ROOT/psz/qt/PSZOverlayQt.h" "$ROOT/psz/qt/PSZOverlayQt.cpp" "$dir/src/frontend/qt_sdl/"
    apply_patches "$dir" "$ROOT/integration/melonds"
    echo ">> desktop tree ready: $dir"
}

android() {
    local dir="$WORK/melonDS-android"
    fetch melonds-android "$dir"
    echo ">> submodules (native deps + core fork)"
    git -C "$dir" submodule update --init --depth 1 \
        melonDS-android-lib app/src/main/cpp/oboe app/src/main/cpp/faad2 app/src/main/cpp/enet

    local lib="$dir/melonDS-android-lib"
    git -C "$lib" checkout -q "$(pin melonds-android-lib rev)"
    cp "$ROOT/psz/PSZPlugin.h" "$ROOT/psz/PSZPlugin.cpp" "$lib/src/"

    # The app and its core are separate repos, so the patches land in each.
    git -C "$dir" apply --whitespace=nowarn "$ROOT/integration/melonds-android/0001-hook-psz-composite.patch"
    git -C "$lib" apply --whitespace=nowarn "$ROOT/integration/melonds-android/0002-lib-build-pszplugin.patch"
    echo ">> android tree ready: $dir"
}

case "${1:-all}" in
    desktop) desktop ;;
    android) android ;;
    all)     desktop; android ;;
    *) echo "usage: bootstrap.sh [desktop|android|all]" >&2; exit 2 ;;
esac
