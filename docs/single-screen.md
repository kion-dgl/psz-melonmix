# Issue: Single-screen presentation (composite bottom-screen HUD onto widescreen top)

**Goal:** make PSZ pleasant to play/capture on one screen. Core gameplay is the
3D top screen; the bottom screen is mostly HUD + map + menus. Composite the
bottom-screen elements onto the top.

**Approach: composite, not reconstruct.** Blit 2D engine-B layers onto the top
screen — reuse the game's own rendering, no HUD reimplementation (that's psz-godot's
job). Read memory only for scene detection and the controller hacks.

## Done
- [x] 2D layer isolation — `patches/0002`, `GPU2D::DebugLayerMask` via env
      `PSZ_LAYER_MASK_B` (bit0-3=BG0-3, bit4=OBJ). Verified headless.
- [x] `tools/layer-dump.sh` — screenshot each isolated engine-B layer.
- [x] **First pixels on the top screen** — `patches/0003`, opt-in via
      `PSZ_HUD_OVERLAY=1`. Draws name, level and HP/PP bars onto the top screen,
      read straight out of emulated main RAM in `ScreenPanelNative::paintEvent`.
      Deliberately janky: fixed position, Qt text, no art.

- [x] **The game's own minimap PORTED to the top-right** (the default) — a
      sub-rect of the bottom screen (`143,109` 66x67, override with
      `PSZ_HUD_MAPRECT="x,y,w,h"`) blitted onto the top screen. kion's question
      was whether this is an element where we just change where it is drawn, and
      it is: `screen[1]` is already a QImage of the bottom screen inside
      `paintEvent`, so the port is one `drawImage` with a source rect.
      **This is the composite path beating the reconstruction path outright** —
      it gets the real room outline, the door markers with gate colours, the
      enemy dots and the player arrow, all correct by construction, and none of
      it blocked on the current-room index.
- [x] **Modal states present the whole bottom screen** — start menu, item menu,
      Mag, shop, quest counter, title, file select. This is the difference
      between ugly and *unusable*: in single-screen mode the bottom screen is not
      on the display at all, so a shop or the quest counter had nothing to
      interact with. Now the corner overlay is replaced by the bottom screen
      itself, centred and scaled to 88% of screen height, over a dimmed field.
      `PSZ_HUD_NOMODAL=1` disables it. Then **sized at 66% with a light dim
      (`PSZ_MODAL_SCALE`, `PSZ_MODAL_DIM`)** so the top screen stayed readable
      around it — the PSO-style pass. Verified on character create: the "Create
      Character" heading, the race description and the character art were all
      legible at once with the menu centred, where an 88% modal buried them.

      **Now full-screen again, and the reasoning inverted.** The top-screen
      content those insets were protecting is not the game's top screen at all —
      it is OUR art, drawn by `RenderArtLayer` into the modal's own space: the
      title logo, the character-create panels. So the inset was shrinking the
      thing it existed to make room for, and the desktop and Android builds
      disagreed about what a menu looks like. Android was already full-screen
      (`Composite` copies the bottom framebuffer over the top one outright), so
      full-screen is now both paths. `PSZ_MODAL_SCALE` below 1 restores the
      inset.

  The test needs no overlay images and no new RE: the player object pointer
  `*(0x02108D04)` is NULL in every mode that is not the main game — which covers
  shops, the quest counter, the title and file select — and inside the main game
  `*(player+0x280)` reads 5 whenever a full-screen menu is up. Either condition
  means "the bottom screen is what the player is looking at". Verified: the Main
  Menu presents full-size, and the field still draws the corner overlay.
- [x] **The minimap rect now includes the key counter row** beneath it (icon, x,
      count) — the room's remaining-key indicator, which the top screen carried
      nowhere else. Eleven more source rows rather than a new element.
- [x] **Player panel ported to the top-left** — source rect `2,4` 124x50,
      override `PSZ_HUD_PANELRECT`. Replaces the hand-drawn gauges, which move to
      `PSZ_HUD_DRAWGAUGES=1`. **Every corner is now the game's own art, and no
      element in the default overlay requires any RE.** The drawn gauges remain
      the only version that can be restyled or scaled independently of the
      game's layout, which is why they are kept rather than deleted.
- [x] **Action palette ported to the bottom-right** — same one-line move as the
      minimap. Source rect `128,6` 124x54, override `PSZ_HUD_PALRECT`.
- [x] **Target box hides when empty** — the game draws it whether or not anything
      is locked on, and there is no flag to read, but an empty box is a light
      panel with no dark glyphs and text is near-black. Counting dark pixels in
      the inset interior separates them with no RE. Confirmed both ways: present
      reading "Garapython / Attribute:Native" in a field, and absent in town.
- [x] **SELECT toggles the area map** — centred on the top screen at 50% opacity
      (`PSZ_MAP_OPACITY`), sized to 55% of screen height, so the field stays
      readable underneath while navigating. `PSZ_HUD_AREAMAP=1` pins it on.

      Drawn on the QPainter path only, at first — so it did nothing at all on the
      renderer most people run. Now ported to the GL path from the same
      arithmetic, as solid-colour quads: the grid is built out of the game's own
      room table and has no pixels on either screen to clip.

  **SELECT was chosen by measurement, not assumption.** With the game idle,
  pressing SELECT changed 78 pixels of the bottom-screen HUD against an idle
  drift of 90 — i.e. nothing above noise. X changed 227, roughly 2.5x the drift,
  so X is probably bound to something and was left alone. The toggle only *reads*
  `KeyInput` (active-low, SELECT is bit 2) and does not intercept it, so if
  SELECT does turn out to act in a context not tested here, the game still gets
  it.
- [x] **Target info ported to the bottom-left** — the locked-on enemy's name and
      attribute. Source rect `2,96` 124x56, override `PSZ_HUD_TGTRECT`. Verified
      in-frame: the ported box read "Garapython / Attribute:Native" while the
      game's own box on the bottom screen read the same. Blank when nothing is
      targeted, which is what the game does, so there is no state to gate on.
      This is the first ported element that adds information the top screen did
      not already carry, rather than relocating something already visible.

  The four corners are now: gauges top-left (the one remaining hand-drawn
  element), room minimap top-right, target info bottom-left, action palette
  bottom-right.
- [x] **The overlay is anchored in WINDOW pixels, not DS pixels** — this is the
      fix for it feeling cramped, and it is the whole reason raising the
      resolution helps. Drawn in the DS's 256x192 space a 104px panel is 40% of
      the screen at *every* output resolution: the picture got bigger and the
      overlay grew exactly in step, so more pixels bought nothing. Anchored to
      the top screen's on-screen rect and sized in real pixels, resolution
      genuinely buys room. `PSZ_HUD_SCALE` (default 2.0) trades element size
      against it directly.

      **Superseded.** Placement now comes from `PSZMix::PlaceElement`, which
      returns fractions of the top screen so the DS compositor, the Qt path and
      both GL paths scale ONE layout definition into their own space. Sizing in
      window pixels was a fourth answer, and it could not line up with the art
      layer, which is positioned in DS space. `PSZ_HUD_ELEMENT_SCALE` is the size
      control; `PSZ_HUD_SCALE` no longer moves anything.
- [x] **Compare mode** — `PSZ_COMPARE=1` puts the 16:9 top screen beside the
      bottom screen, so a ported element can be checked against the original it
      was copied from in the same frame. `PSZ_TOPONLY=1` remains the
      single-screen goal. Both default to a 1920x1080 display.

      **Both flags are gone.** Single-screen 16:9 became the default in
      `Config.cpp` (`ScreenSizing=4`, `ScreenAspectTop=1`) rather than something
      to switch on, and compare mode went with the harness it was built for.
- [x] **16:9 widescreen** — `PSZ_WIDESCREEN=1`. From the cheat DB's "16:9
      Widescreen": if the u16 at `0x020346E0` is `0x1555`, write `0x1C71`.
      7281/5461 = 1.3333, exactly 16:9 over 4:3, so that value is the 3D
      projection's aspect term. Applied per frame the way an AR code would be,
      so no cheat-file wiring is needed. Now on by default as a quality-of-life
      cheat; `PSZ_CHEAT_WIDESCREEN=0` turns it off.
- [x] **Single-screen presentation** — `PSZ_TOPONLY=1` sets `ScreenSizing=4`
      (TopOnly) and `ScreenAspectTop=1` (16:9), widens Xvfb to 1280x720 and
      **resizes the window explicitly**. Fullscreen is only a request and there
      is no window manager on the Xvfb display to honour it, so without the
      resize a wide display just shows a 256x192 picture in the corner. The
      resize is what fires `resizeEvent`, which is what recomputes the layout.
- [x] **Hand-drawn room minimap**, now behind `PSZ_HUD_DRAWMAP=1` — the current room with a player
      arrow, from position `player+0x9C` / `+0xA4` (1.19.12) and yaw `+0xC0`
      (65536/turn, 0 = +Z toward +X, psz-re `sys.facing-convention`). Verified
      against the game's own room panel in the same frame: with yaw exactly 0 the
      game draws its arrow pointing up and so does this one, and the position
      lands low-left in both. The room **outline** is not drawn — see the block
      below.
- [x] **Area grid, opt-in via `PSZ_HUD_AREAMAP=1`** — takes the same top-right
      slot. Off by default because it shows the whole generated layout including
      rooms never visited, which the game's own map deliberately does not.
      Drawn from the room table
      (`*(*(0x02108C64))`, count at `+0x410`, 0x34-byte records, cell at
      `+0x2E`/`+0x2F`, exits `+0x18..+0x1B`, gates `+0x1C..+0x1F`).

### What the map settles, and what it refutes

**The geometry is right, checked against the game's own map.** In the 8-room
Gurhacia state the overlay draws exactly the eight cells the table holds — B4,
B3, B2, A3, C3, D3, D2, B1 — and room 0's north exit leads to B3, which confirms
**north = row − 1**. That was a guess when the code was written and is now
checked.

**`+0x414` is NOT the current room — refuted.** It was the only word in the
field manager that is always a valid room index and varies between states, so it
was the obvious candidate. In that same state it reads **6**, which is cell D2, a
dead end at the far side, while the game's own area map puts the player in
**B4** (room 0). The overlay now draws no "you are here" at all: a wrong one is
worse than none. Finding the real field is psz-re's to do.

**The game's map only shows visited rooms; this one shows the whole layout.**
Not a bug, but it does reveal unexplored geometry, so it is a design decision to
make deliberately rather than inherit.

**The area map screen does not set the menu flag.** The overlay stayed visible
with the Map screen open, so `*(player + 0x280)` is not 5 there. psz-re's note
that dialogue and cutscenes are untested should be widened: the flag covers the
main/item/Mag menus, and the map screen is a fourth full-screen UI that it does
*not* cover.

`PSZ_HUD_FAKEMAP=1` fills a known layout without touching RAM. It is how the
renderer was separated from the read path when the map first failed to draw, and
it is worth keeping for the next element.

### What 0003 settles

**The read path is free.** Inside the emulator there is no GDB stub and no core
halt — the overlay reads a handful of bytes per frame in the paint path. The
stub's per-sample halt only constrains *external* tooling, not this.

**Reconstruct-from-memory works, and it self-verifies.** In one frame the
overlay reported `Lv 1, HP 82/82, PP 67/67` on the top screen while the game's
own panel on the bottom screen showed the same three values. Two independent
paths to the same numbers in the same frame is the cheapest possible oracle, and
it is available on every future element.

**The menu gate works.** `*(*(0x02108D04) + 0x280) == 5` hides the overlay while
a full-screen menu is up, and it returns on close — measured as bright pixels in
the overlay region across three consecutive states: 849 gameplay, 319 menu, 850
after closing.

Offsets came from psz-re and were checked statically against every savestate in
`sandbox*/` *before* any C++ was written — 41 of 41 in-game states plausible,
3 null player pointers (the non-`ov04` modes), menu flag only ever 1 or 5. That
ordering is worth keeping: an address bug and a rendering bug at the same time
are much harder to separate than either alone.

- [x] **PB gauge and the gate-key count**, the two elements kion asked for and
      the overlay has been missing since the first HUD pass. Both come from
      psz-re `docs/melonmix-hud-values.md`, and the confidence is not the same
      for the two.

      **Photon blast is `*(u32*)0x021A2204`, 0..10000** — confirmed there, with
      the draw path traced and two independent corroborations.

      **Drawn as a RING that fills, not a row of text.** It was a labelled bar
      with a percentage first; kion played that on the Retroid and asked for
      something quieter and closer to the game, which draws it as a circle. So it
      is a ring in the plate's right-hand end — no label, no number, nothing to
      read unless you look at it — sweeping clockwise from twelve, with the whole
      disc lit at exactly full. That last part is the game's own distinction: it
      changes *glyph* at 100 rather than completing the circle, so full is a state
      rather than the end of a ramp, and it stays legible without a number.

      Two things the first ring got wrong, both fixed by looking at it magnified
      rather than at a screenshot: at r=5 with a one-pixel wall it rasterises as a
      rounded square, and a near-black trough over the already-dark plate made an
      empty gauge read as a smudge instead of as an empty gauge.

      **It is drawn as an ellipse so it lands as a circle.** This layer is 256x192
      and the single-screen presentation stretches it to 16:9, so a true circle
      here arrives visibly wider than it is tall. The x radius is squashed to 3/4
      to cancel that. The rest of the overlay is stretched the same way and it
      does not matter, because stretched text still reads as text — a stretched
      circle reads as an oval. `PSZ_HUD_RINGASPECT` retunes it; 1.0 is off, which
      is what a 4:3 presentation would want.

      **This corrects our own note**, which had the PB at `panel+0x05` behind a
      panel object pointer nobody had. It is four bytes past current PP in the
      block HP and PP already come from — no pointer, no new plumbing.

      **Gate keys are a DIFFERENCE, not a value.** There is no held-key count in
      memory: two monotone counters live in the stage context at
      `*(0x02108C60)`, `+0x7C` rising by each gate's cost and `+0xCC` on pickup,
      and what the player carries is `collected - used`. A two-key gate charges
      2 in one step, so two barriers can be one gate.

      **Drawn under the minimap, right-aligned to it, shown at zero, and only in
      a field.** All of that came out of kion testing it. It first sat in the top-left readout,
      because our map rect crops the game's own key row off and there was nothing
      to anchor to; kion's answer was that proximity to the map is the whole
      point, so the map now records where it landed and the count hangs off that
      and follows it when the map moves. And it first appeared only once a key had
      been collected, on my reasoning that both counters read zero outside a field
      so a standing "KEYS 0" would be noise — kion wants it visible in the field
      from the start, and **the game agrees with him**: its own key row reads `x0`
      before you have found anything.

      Which then made it visible in Dairon City, where he does not want it — so
      the field test I had dodged is needed after all, and it is the room table
      rather than the counters. Measured, all three cases:

      | | rooms |
      |---|---|
      | Dairon City, and the transporter hub | **0** |
      | the arena | **1**, all four exits `0xFF` |
      | every capture that has ever held a key | **8, 9, 10, 14, 15** |

      So the gate is **two or more rooms** — somewhere you can walk between rooms
      is somewhere gates and keys exist, and a single exitless room is an arena,
      where a key count is exactly the noise being complained about.

      Checked here before drawing: over **83 field captures** the PB never
      exceeds 10000 and `used` is never greater than `collected` — the invariant
      that breaks first if the offsets are wrong — and the five states with keys
      give 1 or 2 held.

      Then walked on the real build, into a real field via the quest counter: HP
      and PP track live and match the game's own panel frame for frame, the ring
      sits empty where the game's own ring is empty, and `KEYS 0` shows under the
      map where the game's row reads `x0`. Driving the counters over the debugger
      afterwards takes it to a gold `KEYS 2` and back to `0` when they are spent,
      and the ring through 25%, 64% and full.

      **Stated plainly: the values are corpus-verified, the rendering was
      exercised by writing the counters over the GDB stub.** A field run with
      keys picked up naturally would be better evidence and has not been done.
      psz-re flags the key offsets as validated-against-savestates rather than
      gated, and the game's own HUD digit is *not* known to be computed this way.

      The art-HUD path (`PSZ_HUD_ART`) is unchanged: `hp-pp.png` has troughs for
      HP and PP and no PB element at all — the ring is drawn live by the game,
      not baked into the panel — so adding one there would be inventing a widget
      and guessing where it goes.

- [x] **Emulator presentation defaults**, so a fresh install is already set up
      for this one game instead of making every user find the settings.

      | | was | now |
      |---|---|---|
      | Android renderer | software | **OpenGL**, and software is no longer offered |
      | Android internal resolution | 1x | **2x** |
      | Desktop internal resolution | 1x | **2x** |
      | Face buttons (Android) | DS A on physical B, X on Y | **1:1 with the labels** |
      | DSi camera settings | shown | hidden |
      | Microphone settings | shown | hidden |

      **The face buttons were crossed on purpose upstream** — DS `A` onto
      `KEYCODE_BUTTON_B`, `X` onto `Y` — which is right for a Nintendo-labelled
      pad, where the positions are mirrored against the Xbox convention. The
      Retroid is Xbox-labelled, so the cross means the button under your thumb is
      never the one the game names.

      **Two defaults per setting, and they have to agree.** The Android renderer
      and resolution each have an XML `defaultValue` *and* a hardcoded fallback
      in `SharedPreferencesSettingsRepository`. The XML one only lands once the
      settings screen has been opened, so a fresh profile takes the code one —
      changing only the XML would make the default depend on whether the user had
      been to Settings.

      **Hidden, not removed**, for the software renderer and for camera and mic.
      An existing profile can still hold `renderer3D_Software`, and the desktop
      dialog indexes its button group *by that value* — dropping the radio would
      dereference null on exactly the profiles most in need of moving off it. So
      the control is hidden, the value still resolves, and the dialog quietly
      moves such a profile to OpenGL. Taking camera and mic out properly means
      the manifest feature, the runtime permission and `CameraManager` as well,
      which is its own piece of work — filed as an issue rather than smuggled in
      here.

      **What this does NOT do is change a profile that already exists.** These
      are defaults; a device that has already written these preferences keeps
      what it has, including the button mapping. On the Retroid that needs a
      reset-to-defaults in the app, or clearing the setting.

- [x] **The cheat menu comes with cheats in it.** melonDS ships an empty cheat
      list and expects you to find a database on the web and import it through
      Settings — a real errand on a handheld, and the same errand every time for
      a build that plays one game. So the database is bundled and seeded into the
      menu on first run.

      **The split is the point, and it matches how these codes differ.** 16:9
      widescreen is an unambiguous improvement with no decision attached, so the
      plugin applies it every frame and it is **deliberately absent from the
      database** — a switch that does not turn something off is worse than no
      switch. Everything else is taste: stat maxers, experience multipliers,
      movement codes. Those are **offered, not applied**, and arrive disabled.

      20 cheats in four folders, from the same community database
      `scripts/gen-cheats.py` already reads, selected by
      `scripts/gen-cheat-db.py` into the XML melonDS-android's own importer
      understands.

      Three things learned by reading that importer rather than guessing:

      - **Cheats must be nested in a `<folder>`.** `XmlCheatDatabaseSAXHandler`
        only opens a cheat while `parsingFolder` is true, so the top-level ones
        the source database carries are silently skipped. Checked: zero orphans
        in what we ship.
      - **The `<gameid>` is what matches the ROM**, so it keeps the source's code
        and checksum, `C24E 0AFFC6C3`.
      - **Nothing `[SELECT]`-prefixed.** That family of codes activates while
        SELECT is held and this build binds SELECT to the area map. They would
        work, and every use would also flip the map up.

      Ambiguity is refused rather than resolved: if a cheat name appears twice in
      the source, the generator skips it and says so, because `x2` silently
      becoming a movement code under a menu entry that still reads "x2" is the
      failure that would never get noticed.

      Seeding is guarded by a preference, not by looking for the database — a
      user who deletes it meant to, and re-seeding on next launch would make it
      impossible to get rid of.

## TODO
- [ ] **Grow the overlay element by element**, each one verified against the
      bottom screen in the same frame the way HP/PP was. PB and the key count
      are done (below); the minimap from the room table is next.
- [ ] **Position and art** — the overlay is currently 104px in the top-left
      corner over the scene. Once widescreen lands it belongs in a margin.
- [ ] **Find the real current-room field** (psz-re) — this is now the single
      blocker on the room minimap's outline: the shape and rotation live in the
      current room's record and nothing knows the index. `+0x414` is refuted
      above. Three further searches came up empty and are recorded so they are
      not repeated:
      - *No pointer to the current room record exists in main RAM.* Scanning all
        4 MB across 16 multi-room states for a u32 equal to `base + i*0x34`
        leaves exactly one survivor, `0x02243E30`, which is just the table base.
      - *A bare index is unfindable with the states on disk.* 1.16 M byte offsets
        survive "always < count", because a small integer matches everywhere.
      - *The cell-pair search finds only `base + 0x2E`* — room 0's own cell.
      The reason all three fail is the same: **in every savestate on disk the
      player is still in room 0**, so nothing distinguishes "current room" from
      the constant 0. **What would settle it: one savestate taken in a second
      room.** That is a capture task, not an RE task.
- [ ] **The GDB stub never resumes the core once attached.** After `cont()` and
      after the client detaches, frames stop advancing — two screenshots two
      seconds apart are byte-identical. So injecting state over the stub and then
      screenshotting the result does not work: every shot is a frozen frame from
      before the write. This cost an hour here; the way round it was to build the
      major-13 fork and load a real field instead.
- [ ] **Savestate major version splits the test corpus.** The patched build is
      major 14 and can load only 5 of the 45 states on disk (a town and two
      arenas — no multi-room field). `patches/0003` applies to `melonDS-ss13`
      with `patch --fuzz=5` and builds, which is how the map got tested on real
      data. Worth keeping both builds patched.
- [ ] **`--savestate` in `gdb-headless.sh` is dead code** — it passes
      `--loadstate` to melonDS, which has no such option, so the emulator dies
      with `Unknown option 'loadstate'`. Use `--state` (installs slot 8) + F8.
      Either wire it to the same mechanism or drop the flag.
- [ ] **Live layer-toggle hotkeys** (no relaunch per layer) — faster than the env sweep.
- [ ] **Identify which BG/OBJ holds each HUD element** (map, HP/PP, status, item
      shortcuts, menus) — sweep the mask, match against the DashGL psz-asset-viewer.
- [ ] **`detectGameScene()`** — field / menu / dual (title, char-create, char-select
      stay dual-screen; they don't need compositing). *Needs the game's scene/mode
      variable* → RE / psz-re.
- [ ] **Composite** the identified HUD layers onto the widescreen top, per scene.
      Pairs with the 16:9 widescreen cheat (wide 3D + HUD in corners).
- [x] ~~**Controller → touch input hook**~~ — **effectively closed, and it was
      never as large as this entry assumed.** It read "enumerate *all* touch-only
      actions (map, item shortcuts, word-select chat) and map each", which framed
      touch as a broad blocker needing the touch handler and its hitbox
      coordinates from RE. Per kion, who has played the game: **the area map was
      the only thing that expected touch.** Item shortcuts and word-select are
      reachable from buttons.

      That single case is already served — SELECT toggles the drawn area map — so
      no touch injection, no hitbox coordinates and no RE are needed for
      single-screen play. Modal screens (menus, shop, quest counter) are driven
      by d-pad and buttons and work under the modal presentation.

      Confidence: this is kion's report from play, not a measurement. What would
      confirm it: drive a shop and the quest counter to completion with buttons
      only. Recorded as a claim rather than a verified fact because the previous
      version of this entry was wrong in the expensive direction, and being wrong
      the other way would be found the moment someone tries to buy something.
- [ ] **PSO-style modal menus** (kion's idea, for after the first pass). The
      current modal presentation blows the whole bottom screen up to 88% of
      screen height, which is correct and readable but takes the entire view.
      PSO instead keeps the field visible around a menu that occupies only part
      of the screen. Two routes, and they differ in cost by a lot:
      - *Cheap:* shrink and reposition the modal blit, and drop the dimming, so
        the game's own menu art sits over a still-visible field. No RE, and it is
        a few lines on `pszDrawModal`.
      - *Expensive:* recompose the menu from its constituent panels so the layout
        itself changes rather than just its size. That needs the menu's own
        sub-rects, and they move per menu.
      Start with the cheap one — it may well be enough, and it is reversible.
- [ ] Handle the resolution mismatch (hi-res 3D + fixed low-res 2D HUD) — scale for
      now; reconstruct hi-res HUD later (optional, uses the extracted assets).

## Architecture — the game knowledge is in the CORE

`melonDS/src/PSZPlugin.{h,cpp}` holds everything PSZ-specific: the addresses, the
mode test, the room table read, the widescreen poke and the SELECT toggle.
`PSZ::Update(nds)` is called once per frame and returns a `Frame` describing what
to draw — a list of source rects with the corner each belongs to, a modal flag,
and the room graph. The Qt frontend contains no game addresses at all any more.

**Drawing deliberately stayed in the frontend.** Compositing in the core would
mean writing into the 256x192 framebuffer, where a 70px panel is 27% of the
screen at *every* output resolution — which is exactly the crowding that
anchoring the overlay in window pixels fixed. So the core decides *what* and
*where from*; the frontend decides *how big*, at its own resolution.

That is what makes the Android port tractable: melonDS-android shares `src/`, so
it inherits `PSZPlugin` and has to implement drawing only — four scaled blits, a
full-screen blit for modals, and a grid of rectangles. No game knowledge crosses
over, and no address gets re-derived.

**One drawing path per frontend, though — the split has a cost and it was paid.**
The desktop had two, because upstream has two display panels and picks between
them from the renderer setting. Both had to carry an overlay, and the QPainter one
fell behind without anything failing: it drew the ported clips and none of our own
art, so the title had no logo and character create had no UI, on whichever path
the user's settings happened to select. There was no signal, because each path was
individually plausible.

`createScreenPanel` is now patched to always build `ScreenPanelGL`, and the
QPainter overlay is deleted. The software 3D *renderer* is untouched and still
selectable — it feeds the same GL panel. What went is the software *display* path,
which is the thing that chose between overlays. An OpenGL context is now required
to run this build at all, which is the trade: one path that is always the tested
one, against a machine with no working GL getting an empty window and a log line.

## Front-to-end flow, walked under the single-screen overlay

All of it works. Booted cold and driven with buttons only, no touch:

| Scene | Result |
|---|---|
| Intro / attract | plays, then START skips to title |
| **Title** | PRESS START art presented, logo dimmed behind |
| **File select** | three slots, Friend Roster, WFC Settings — readable and selectable |
| **Character create** | race select (Human / CAST / Newman) with art behind |
| **Quest counter** (`ov12`) | area select — Gurhacia Valley, description, difficulty stars |

### The contextual box: the pixels are the only signal

The bottom-left box is captioned from `0x0211CCD0`, and psz-re **disconfirmed
that address as the box's storage** (Q1). It is a shared, stale-retaining scratch
buffer — story dialogue under `ov17`, a clock under `ov16` — and the eight
captures that founded "this is the box's buffer" agreed only because all eight
were field captures. So its contents say nothing about whether there is a box to
caption, and reading it unasked put last room's item name under an empty corner.

**And no fixed address can say whether the box is up.** psz-re established the
widget descends the call chain as an argument, and that the three RAM-range
literals reachable from the drawing functions read zero in 47 of 48 captures —
including the 45 field ones **with the box on screen**. There is no global to
dereference. The pixels are the signal, which is why the test counts dark ones.

The core reads them where it can. Under an accelerated renderer it cannot:
`GetFramebuffers()` returns false and the frame exists only as a texture. So the
frontend answers, through `SetBoxHasTextHint()` — and answers on the GPU, with an
**occlusion query** rather than a readback, since reading pixels back costs the
pipeline stall the overlay is otherwise careful to avoid. The shader discards
everything above the brightness threshold and the query reports how many
fragments survived; that is the CPU test's dark-pixel count, same threshold,
asked as the one question a GPU answers without handing pixels over. The result
is collected a frame late on purpose.

**Inset by six pixels, and that is not a detail.** The box's own border is dark
and always present: measured, the full rect floors at **130** dark pixels with
the box empty, against a threshold of 24. A probe that forgets the inset reports
text permanently, and did. Inset, the count is **exactly zero** when hidden and
48–158 when showing — a gap, not a tuned threshold.

**The buffer turned out to be usable after all**, which is worth recording
because the plan was to abandon it for clipping the box instead. Across a walk
through town every frame the box was up, it held the right caption — `Cyan`,
`Millio`, `Ohyo`, `Item Shop`, `Weapon Shop`, `Custom Shop`, `Natsume`. The one
dialogue string seen in it appeared while the box was **hidden**. Being shared
and stale-retaining is a reason never to read it unasked; it is not a reason the
drawn panel cannot work.

### Why character create draws its own UI at all — the preview

The option lists on these screens are meaningless on their own. Race, class and
especially appearance are choices you make *by looking at the character*, and the
character is on the TOP screen. Present the bottom screen whole, the way the title
and file select are presented, and you get sliders with nothing to judge them
against — the interaction survives and the reason for it does not.

That is the whole reason these screens are drawn from our own art rather than
clipped or presented: it is the one way to have the option list and the preview at
the same time. Appearance is what forces it, and race and class follow because the
machinery already exists and the preview is just as much the point there.

**So the bottom screen is right for exactly one screen in create: name entry.** It
is the only one where the preview does not matter, and the only one that cannot
work without the game's own keyboard. Anything that presents the bottom screen
more widely than that is trading the preview away, and the preview is the feature.

Worth stating plainly because the obvious fix for a name-entry detection failure —
"just present the bottom screen for the whole appearance screen" — looks like a
small concession and is actually the removal of the thing these screens exist for.
`PSZ_CC_BOTTOM` and the SELECT toggle are escape hatches for when detection fails,
not a way of working.

**Name entry is detected by `0x02124A64`**, a word in `ov11`'s BSS: null on
appearance, a heap pointer on the name-entry screen. psz-re's measurement
(unattended-backlog item E(a)), and it replaced a heuristic of ours that keyed on
the cursor reading 33 — which did not reproduce here at all, the cursor reading
0..6 and nothing else across a full pass, which is exactly why pressing "Next
Settings" used to leave the sliders up with the keyboard unreachable.

What makes the slot trustworthy where the cursor was not is that its own worst
objection was measured rather than argued. `FUN_0211E93C` is a get-or-create, and
a get-or-create is a cache — a cache would stay set after the screen closed and
report "name entry has been *visited*". So it was tested by entering name entry
and backing out with B, screen-confirmed at all three points: `0`, a pointer, `0`
again. Created on entry, destroyed on exit.

### And name entry is not the last screen — the confirmation prompt

Answering the keyboard's OK raises "Is this okay?" on the bottom screen. By then
the keyboard object is destroyed, so `CcNameSlot` is 0, a name-entry-only rule
reports appearance, and the overlay returns to the preview while the question the
player has to answer is on a screen it is not showing. It reads as the game having
failed at the last step of onboarding.

`0x02124B20` holds the currently open sub-screen, and **the class is what decides,
not the pointer.** Two wrong turns on the way to that, both worth keeping:

- *"A sub-screen is live"* — false nearly always, because the appearance list is a
  sub-screen too. The capture that suggested otherwise had its baseline taken one
  frame before the list object was constructed, so a construction gap was read as
  a state. One capture, one plausible story, shipped: the cursor-33 mistake again.
- *Matching the pointer* — the prompt lands on `0226A600`, the block the first
  sub-screen freed. Address is not identity under heap reuse.

Word 0 of the object is its vtable, constant per class, and separates them:

| vtable | screen |
|---|---|
| `02124700` | the first sub-screen — **still unidentified** |
| `02124634` | the appearance list (holds a `0620xxxx` VRAM address) |
| `02124A20` | the keyboard — the name slot is set on exactly these frames |
| `02124A40` | "Is this okay?" |

Reproduced identically over two full passes, and they sit in `ov11`'s own data
beside `CcNameSlot`, which is where that overlay's vtables belong.

**An allowlist, deliberately.** The rule presents on classes known to want the
bottom screen rather than on "anything that is not the list", so an unseen
sub-screen — `02124700` is one — keeps the drawn UI and the preview instead of
inheriting a guess. Being wrong about an unknown screen should cost a missing
presentation, not a broken appearance screen, which is the failure this cost
twice.

**The handover is held.** The keyboard object is destroyed before the prompt is
constructed, and over those frames nothing says "present", which surfaced as the
top screen flashing in before the prompt. The presentation holds while the handle
is *empty*, bounded at fifteen frames; a live handle of an unknown class ends it
at once, so the hold cannot keep the bottom screen over a screen that wants the
preview.

**A psz-re claim is contradicted by this.** `docs/game-state.md` says under Traps:
"The title screen runs with the bottom screen off, while file select uses both",
and offers reading the display control registers as a way to separate the
pre-game sub-states. The title screen's bottom screen is **not** off — it carries
the PRESS START art, which is what the modal presentation shows. The top screen
holds the logo. So that discriminator does not work as written, and
`game-state.md`'s own scene table ("Title | logo | press start") already
disagreed with its Traps section.

**The one real limitation, and it is the same one in every pre-game screen.**
These are *split* scenes: the top screen carries real content — the character
description in create, the "Select an area." prompt and Quest Counter heading at
the counter, the logo at the title — and presenting the bottom screen at 88% of
screen height covers it. Nothing is unusable, but information is hidden that the
dual-screen original shows.

This is exactly the case the PSO-style partial modal solves, and it is a better
argument for it than aesthetics: a smaller menu that leaves the top screen
readable would make these screens *complete*, not just prettier.

## Observed scenes (from play, not RE)
| Scene | Top screen | Bottom screen |
|---|---|---|
| Title | logo | "press start" |
| File select | character preview (3D) | the three save files |

Both are **split** scenes, not top-only — real content lives on each screen. So a
blanket "blit bottom onto top" is wrong even for the menus, and `detectGameScene()`
is required before any compositing, not just an optimisation for the field.

## Notes
- Area-map vs room-map: tapping opens the *area* map; the button hook should
  trigger that.
- The hard half (rendering a layer in isolation) is done; blitting to the top is
  the easy half.
- Bidirectional with psz-re: layer isolation (dynamic) says "map = BG1"; the decomp
  (static) finds the code that draws it, the scene flag, and the touch region.
