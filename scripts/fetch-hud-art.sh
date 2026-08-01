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

[ -d "$SRC/assets/ui/psz-palette" ] || {
    echo "!! no assets/ui/psz-palette under $SRC" >&2
    echo "   pass the psz-godot path, or set PSZ_GODOT" >&2
    exit 1
}

mkdir -p "$DST"

# Paths relative to the psz-godot assets/ root.
#
# Named rather than globbed, and that IS the exception to this repo's usual
# rule: psz-godot's art directories hold well over a hundred files, most of
# them technique and item icons this build has no element for.
#
# These were picked by matching against the source rects in PSZPlugin.cpp --
# palette_bg is 128x67 against a PSZ_HUD_PALRECT of 124x54, map.png 99x96
# against a 70x80 map rect. They are cut from the same panels the rects clip,
# which is exactly why they can replace them.
#
# NOT assets/hud/*.png at 200x173: those are psz-godot's own large touch
# buttons, drawn for a phone-sized on-screen control pad rather than a DS HUD.
# The psz-palette set is 20x20 -- DS scale.
#
# A file listed here that does not exist is a hard error, not a skip: silently
# shipping without the player panel is the "explicit list" failure this repo
# has already hit four times.
WANT=(
    hud/hp-pp.png                     # player panel: level, HP, PP
    ui/hud/map.png                    # minimap frame
    ui/hud/map_grid.png               # area grid, behind PSZ_HUD_AREAMAP
    ui/psz-palette/palette_bg.png     # action palette frame
    ui/psz-palette/palette_bg_r.png   # ... right-aligned variant
    ui/psz-palette/attack.png         # palette icons, all 20x20
    ui/psz-palette/strong_attack.png
    ui/psz-palette/dodge.png
    ui/psz-palette/pickup_item.png
    ui/psz-palette/telepipe.png
    ui/psz-palette/monomate.png
    ui/psz-palette/dimate.png
    ui/psz-palette/trimate.png
    ui/psz-palette/monofluid.png
    ui/psz-palette/difluid.png
    ui/psz-palette/trifluid.png
    ui/psz-palette/moon_atomizer.png
    ui/psz-palette/sol_atomizer.png
    ui/psz-palette/star_atomizer.png
    ui/psz-palette/resta.png
    ui/psz-palette/anti.png
    ui/psz-palette/foie.png
    ui/psz-palette/barta.png
    ui/psz-palette/zonde.png
    ui/psz-palette/grants.png
    ui/psz-palette/megid.png
    ui/psz-palette/shifta.png
    ui/psz-palette/deband.png
    ui/psz-palette/jellen.png
    ui/psz-palette/zalure.png
)

missing=0
for f in "${WANT[@]}"; do
    if [ -f "$SRC/assets/$f" ]; then
        cp "$SRC/assets/$f" "$DST/$(basename "$f")"
    else
        echo "!! missing: assets/$f" >&2
        missing=1
    fi
done
[ "$missing" -eq 0 ] || { echo "!! artwork incomplete; not usable" >&2; exit 1; }

echo ">> ${#WANT[@]} files -> $DST (gitignored)"
