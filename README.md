# psz-melonmix

A single-screen presentation layer for **Phantasy Star Zero** (Nintendo DS) on
melonDS: the bottom screen's HUD moved onto a widescreen top screen, so the game
is pleasant to play and to capture on one display.

**You supply your own ROM.** Nothing here contains game data, and the ROM is
never modified — this is a custom build of melonDS that loads your ROM as normal.
The only thing written to the game is a single 16-bit value in emulated RAM each
frame, the way an Action Replay code works.

![status](https://github.com/kion-dgl/psz-melonmix/actions/workflows/build.yml/badge.svg)

---

## What it does

In the field, four elements are moved from the bottom screen to the corners of
the top screen: the player panel, the room minimap (with its key counter), the
locked-on target's name and attribute, and the action palette.

In menus, shops, the quest counter, the title and file select, the bottom screen
*is* the interaction — so it is presented whole instead, filling the screen.
Those screens are split scenes: character create puts its race description on the
top screen, the quest counter its "Select an area." prompt, the title its logo.
What the top screen carried is drawn back over the menu from our own art, so
nothing is lost by giving the menu the whole display.

Other bits: 16:9 widescreen (the projection term, not a stretch), SELECT toggles
a translucent area map drawn from the game's own room table, and the target box
hides itself when nothing is locked on.

**Almost none of this is reconstructed.** The elements are the game's own
rendering, relocated — which is why they have the correct art, the real room
outline, the door markers, the enemy dots and the player arrow, without any of it
being reimplemented or needing the game's internals to be understood first.

## Build

```sh
./scripts/bootstrap.sh desktop      # clone upstream at its pinned rev, inject, patch
./scripts/build-desktop.sh          # -> build/melonDS/build/melonDS

./scripts/bootstrap.sh android
ANDROID_HOME=~/Android/Sdk ./scripts/build-android.sh
```

Linux deps are listed at the top of `scripts/build-desktop.sh`; Windows uses
MSYS2 and CI has the package list. On macOS:

```sh
brew install qt pkgconf libarchive faad2 enet sdl2 zstd cmake ninja
export PKG_CONFIG_PATH="$(brew --prefix libarchive)/lib/pkgconfig"   # keg-only
./scripts/build-desktop.sh          # -> build/melonDS/build/melonDS.app
```

Qt builds an app bundle there, so the binary is inside it at
`melonDS.app/Contents/MacOS/melonDS` rather than at the path the script prints.
macOS is not in CI, so it is built by hand and can break without anything saying
so.

The single-screen 16:9 view and the widescreen cheat are on by default — there is
nothing to turn on to get the view this build exists for. Environment variables
retune it without a rebuild:

| | |
|---|---|
| `PSZ_MODAL_SCALE` | menus fill the screen; below 1 insets them, `PSZ_MODAL_DIM` behind |
| `PSZ_HUD_ELEMENT_SCALE` | size of the ported corner elements |
| `PSZ_HUD_ART` | draw the player panel and palette from our art instead of clipping them |
| `PSZ_MAP_OPACITY`, `PSZ_HUD_AREAMAP` | the SELECT area map |
| `PSZ_CHEAT_WIDESCREEN=0` | back to 4:3 |
| `PSZ_HUD_MAPRECT="x,y,w,h"` and friends | per-element source rects |

**An OpenGL context is required.** Upstream melonDS has two display paths and
picks one from the renderer setting; this build always uses the GL one, because
carrying a separate overlay for each is how the two drifted — the QPainter one
spent a release drawing the ported clips and none of our own art, so whether the
title had its logo depended on a setting nobody would connect to it. The software
3D *renderer* is untouched and still selectable; it feeds the same GL panel.

## How it is put together

```
psz/            ours, portable — the only place game knowledge lives
  PSZPlugin.*     reads state, returns a Frame: what to draw and where from
  qt/             desktop drawing (GL), at window resolution
integration/    thin hooks against pinned upstreams
upstreams.toml  pinned revs, one place
scripts/        bootstrap + per-target builds
```

**Injection rather than a fork, for a specific reason.** The desktop and Android
builds use *different* melonDS cores with independent histories — melonDS proper,
and the Android project's own fork. One fork cannot serve both. But
`psz/PSZPlugin.{h,cpp}` compiled unmodified in both of them, so a portable plugin
plus a small per-frontend hook does.

**Drawing is deliberately not in the core.** `PSZ::Composite()` exists for
frontends that only ever see the raw framebuffers (Android uploads them to a GL
texture as-is), but compositing at 256x192 locks every element to DS pixel scale,
where a 70px panel is 27% of the screen at *any* output resolution. A frontend
that knows its window size does better, and the Qt one does.

### Bumping upstream

Change a `rev` in `upstreams.toml` and re-run bootstrap. Patches are applied with
**no fuzz**, so if upstream moved under a hook the build fails immediately and
says which patch — rather than applying at an offset and being discovered later
as strange behaviour.

## Licence and credits

GPLv3, inherited from melonDS. Releases ship the assembled source tree alongside
the binaries, which is what GPLv3 asks for when distributing builds.

- [melonDS](https://github.com/melonDS-emu/melonDS) — the emulator
- [melonDS-android](https://github.com/rafaelvcaetano/melonDS-android) — the Android frontend and its core fork
- [KHMelonMix](https://github.com/vitor251093/KHMelonMix) — the plugin-layer idea this follows
- DeadSkullzJr's NDS(i) Cheat Database — the 16:9 widescreen code in `cheats/`
- [psz-re](https://github.com/kion-dgl/psz-re) — where the addresses came from
