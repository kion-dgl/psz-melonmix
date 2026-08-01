#!/usr/bin/env bash
# Populate psz/art/ from a local psz-godot checkout.
#
# WHY THIS IS A SCRIPT AND NOT COMMITTED FILES
#
# The HUD artwork is cut from Phantasy Star Zero's own sprites. psz-godot is a
# public repo and does not track it either -- `git ls-files assets/hud/*.png`
# comes back empty there; the art ships through that project's asset pack,
# separate from source. This repo has the same rule, stated harder: no ROM, no
# game assets, no savestates. Committing PSZ-derived art here would break it.
#
# So the art is fetched, never tracked. Contributors who have psz-godot get a
# HUD; everyone else gets the bottom-screen clips, which is what the build did
# before the artwork existed.
#
#   ./scripts/fetch-hud-art.sh [path-to-psz-godot]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${1:-${PSZ_GODOT:-$HOME/projects/psz-godot/psz-godot}}"
DST="$ROOT/psz/art"

[ -d "$SRC/assets/hud" ] || {
    echo "!! no assets/hud under $SRC" >&2
    echo "   pass the psz-godot path, or set PSZ_GODOT" >&2
    exit 1
}

mkdir -p "$DST"

# Named rather than globbed, and that IS the exception to this repo's usual
# rule: psz-godot's hud/ holds 86 files, most of them technique and item icons
# this build has no element for. Copying all of them would bloat every tree for
# no gain. A file listed here that does not exist is a hard error, not a skip --
# silently shipping without the player panel is exactly the failure this repo
# has hit before with "explicit list" bugs.
WANT=(
    hp-pp.png          # the player panel: level, HP and PP
    attack.png         # action palette, in palette order
    strong_attack.png
    special_attack.png
    dodge.png
    talk.png
    pickup_item.png
    push_button.png
    use_warp.png
    monomate.png
    dimate.png
    trimate.png
    monofluid.png
    trifluid.png
)

missing=0
for f in "${WANT[@]}"; do
    if [ -f "$SRC/assets/hud/$f" ]; then
        cp "$SRC/assets/hud/$f" "$DST/$f"
    else
        echo "!! missing: assets/hud/$f" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || { echo "!! artwork incomplete; not usable" >&2; exit 1; }

echo ">> ${#WANT[@]} files -> $DST (gitignored)"
