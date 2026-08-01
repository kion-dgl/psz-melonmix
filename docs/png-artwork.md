# PNG artwork for the HUD

## Why this is now the blocking piece

Everything the overlay draws today is a **rectangle clipped out of the bottom
screen**. That approach has now failed the same way twice, in two unrelated
places:

- **The HUD panels.** Reported from the Retroid: the game's panels have rounded
  and notched frames, so a rectangular clip drags background in around the
  edges. No choice of rect fixes it — the art is not a rectangle.
- **The title logo.** The `ov00` composite lifts the logo from the top screen
  and draws it over the bottom. It works, and you can see the seam: a hard
  rectangular edge carrying the top screen's dark background and part of the
  planet. Retuning `PSZ_TITLE_LOGORECT` makes the box smaller, never correct.

Both want the same thing: **art with an alpha channel**, drawn over the scene
instead of stamped as a block. That also unlocks two things clipping can never
do — resizing elements without resampling the game's own pixels, and drawing
panels from *values* rather than from pixels.

## The artwork exists

psz-godot has it, cut from the game's own sprites, at DS scale:

| Element | File | Size | Our rect |
|---|---|---|---|
| Player panel | `assets/hud/hp-pp.png` | 256×120 | `PSZ_HUD_PANELRECT` 124×50 |
| Action palette frame | `assets/ui/psz-palette/palette_bg.png` | 128×67 | `PSZ_HUD_PALRECT` 124×54 |
| Palette icons (44) | `assets/ui/psz-palette/*.png` | 20×20 | — |
| Minimap frame | `assets/ui/hud/map.png` | 99×96 | `PSZ_HUD_MAPRECT` 70×80 |
| Area grid | `assets/ui/hud/map_grid.png` | 1024×1004 | — |
| Title logo | `assets/images/logo.png` | — | `PSZ_TITLE_LOGORECT` |

The sizes are the evidence: 128×67 against a 124×54 rect, 99×96 against 70×80.
These are cut from the same panels the rects currently clip.

**No target-box art exists** for `PSZ_HUD_TGTRECT` (124×56). That element stays
a clip until someone draws one.

`scripts/fetch-hud-art.sh` copies them into a gitignored `psz/art/`. They are
not committed, and that is deliberate — see "Licensing" below.

## How to get pixels into the build

Three options, in the order they were considered:

**1. Runtime PNG decode (`stb_image`).** One public-domain header, decode at
startup. Costs a vendored dependency and a real asset path on every platform —
Android needs the files inside the APK and opened through the asset manager,
which is a different code path from desktop's filesystem.

**2. Bake to a generated header at build time.** A script turns `psz/art/*.png`
into a `PSZArt.h` of raw RGBA arrays. **This is the recommended one.** No
decoder, no dependency, no file IO, no per-platform asset path, and it matches
how `PSZOverlayIDs.h` and `PSZCheats.h` already work — generated, gitignored
inputs, checked-in-or-not by the same rule as the rest.

**3. Ship a texture atlas as a binary blob.** Same as 2 but as a file rather
than a header. Only worth it if the art grows large enough that compile time
suffers. It will not: the whole set above is well under 200 KB as raw RGBA.

Recommendation: **option 2**, `scripts/gen-art.py` → `psz/PSZArt.h`, with
`psz/art/` and `psz/PSZArt.h` both gitignored.

## What still has to be built

Ordered by what unblocks the most:

1. **`gen-art.py`** — PNG → RGBA header. Python's `zlib` decodes PNG IDAT
   directly; no Pillow needed for the small set here, though Pillow is fine if
   it is already around.
2. **Alpha blending in both draw paths.** The GL path needs `GL_BLEND` enabled
   (it currently explicitly disables it). `Composite()` needs a per-pixel
   `src-over` blend instead of the straight copy `Blit` does.
3. **An atlas** so the GL path binds one texture per frame rather than one per
   element.
4. **Digit glyphs.** Drawing HP/PP/level as *values* needs numbers. There is no
   font in this build. Either cut digits from psz-godot too, or draw a tiny
   fixed 3×5 bitmap font in code.

## The data behind each element

Screen policy needs no memory layout. Drawing panels from values does, and this
is where each element stands:

| Element | Data needed | Status |
|---|---|---|
| Player panel | current/max HP, current/max PP, level | **Have all five.** `0x021A21FE`, `0x021A210C`, `0x021A2200`, `0x021A210E`, and level at `*(0x0211A530)+0x92`. Verified against 13 savestates. |
| Minimap | room table, player position, facing | **Have it.** Room table at `*(*(0x02108C64))`; position `0x021A214C`, facing `0x021A2170`. |
| Minimap key count | keys held | **Have it** — the room table carries key count. |
| Info panel | the text itself | **No.** Not extractable; stays a clip, framed by our art, shown only when `pszBoxHasText()` says there is text. |
| Action palette | which action is in each slot | **No.** 44 icons exist but nothing maps slot → action. |
| Palette R-flip | which of the two pages is showing | **No.** Holding R reveals three more slots; no signal identified. |
| PB gauge | photon blast charge | **No.** psz-re lists it under "still to find". It is a circle that fills, so it also needs an arc draw, not a sprite. |

So the player panel and the minimap can be drawn from values today. The action
palette needs RE first, and the info panel will always be a clip.

## Licensing — why the art is not committed

The PNGs are cut from Phantasy Star Zero's own sprites. psz-godot is a public
repo and **does not track them either** — `git ls-files assets/hud/*.png` comes
back empty there; they ship through that project's asset pack, separate from
source.

This repo has the same rule, stated harder: no ROM, no game assets, no
savestates. Committing PSZ-derived art here would break it.

So: fetched, never tracked. Contributors with a psz-godot checkout get a HUD;
everyone else gets the bottom-screen clips, which is what the build did before
the artwork existed.

**Open decision:** baking the art into a generated header puts it in the
*binary*, and release builds are published from CI. That is a distribution
question, not a code one, and it is kion's call — the same call psz-godot
already made by publishing its pack outside git. Until it is decided, the
generated header should be gitignored and CI releases built without art.
