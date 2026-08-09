#!/usr/bin/env python3
"""Emit psz/android/assets/psz-cheats.xml -- the cheats the app ships with.

WHY THIS EXISTS SEPARATELY FROM gen-cheats.py

gen-cheats.py bakes codes into PSZCheats.h for the PLUGIN's own interpreter.
That is the right home for exactly one thing -- 16:9 widescreen -- because it is
an unambiguous improvement that should simply be on, with no menu and no
decision to make.

Everything else in the community database is a taste question: faster movement,
experience multipliers, maxed stats. Those want to be OFFERED, not applied, and
the emulator already has a cheat menu with a toggle per code. What it does not
have is any cheats in it: melonDS ships empty and expects you to find a database
on the web and import it, which on a handheld is a genuinely unpleasant errand.

So this writes the same community codes into the XML format melonDS-android's
own importer reads, the app seeds it on first run, and they show up in the pause
menu already there and all switched off.

TWO THINGS THE IMPORTER REQUIRES, both learned by reading its parser:

  - Cheats must be nested inside a <folder>. XmlCheatDatabaseSAXHandler only
    starts a cheat when `parsingFolder` is true, so a <cheat> sitting directly
    under <game> -- which the source database has several of -- is silently
    skipped.
  - The <gameid> is matched against the ROM, so it has to keep the code and
    checksum the source database carries.

Regenerate:  python3 scripts/gen-cheat-db.py
"""
import pathlib
import re
import sys
import xml.sax.saxutils as sx

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "cheats" / "psz_C24E_melonds.xml"
OUT = ROOT / "psz" / "android" / "assets" / "psz-cheats.xml"

DB_NAME = "PSZ MelonMix (bundled)"

# WIDESCREEN IS DELIBERATELY ABSENT. The plugin applies it every frame and it is
# always on; listing it here too would put a switch in the menu that does not
# turn it off, which is worse than not offering it.
#
# Folders, because the importer needs them and because they are how the menu
# groups things. Names are matched against the source database exactly.
FOLDERS = [
    ("Character", [
        "Max Health",
        "Max PP",
        "Max Attack",
        "Max Defense",
        "Max Magic Attack",
        "Max Accuracy",
        "Max Evasion",
        "Infinite HP",
        "Infinite PP",
    ]),
    ("Experience", [
        "x2", "x4", "x8", "x16", "x32", "x64",
        "Super Quick Exp Gain",
    ]),
    ("Movement", [
        "2x Movement Speed",
        "Even Quicker Anims",
    ]),
    ("Items", [
        "Boost Material x99 each",
        "Soul Item x99 each",
    ]),
]

# CHEATS THAT ARE OURS, not the community database's. These come out of psz-re's
# own decompilation and exist nowhere else, which is the whole reason to ship
# them: nobody can find these on the web because nobody else has them.
#
# Both are one-word writes to a literal-pool constant, and psz-re checked the
# thing that makes that safe: each literal has EXACTLY ONE reference in the whole
# binary -- 0x020b2af8 from FUN_020b2a54, 0x02082bb8 from FUN_02082b30 -- and
# literal pools are per-function here, so neither can perturb another RNG call.
FIRST_PARTY = [
    ("Rare rooms", [
        ("Rare room always appears", "020B2AF8 00000001",
         "The special nr* room the area is entitled to, every field instead of "
         "roughly 1 in 40. Stages 1-2 give the cake shop, 3 the snowfield room, "
         "4 the pumpkin room, 5 the paru room, 6-7 the arca plant pizza shop."),
        ("Coliseum always appears", "02082BB8 00000001",
         "The coliseum warp in the E transition, every time instead of 8%."),
    ]),
]

# WHY THE ROLL RANGE AND NOT THE WEIGHTS. FUN_020b2a54 draws rand(10000) and
# returns the first slot whose running weight total exceeds the roll; usually it
# runs off the end and returns -1, which is what makes these rooms rare. Setting
# the range to 1 makes rand(1) return 0, so the first eligible slot wins
# deterministically. psz-re tried the weights first -- all eight slot bytes, all
# eight masks, all weights maxed -- and the outcome never moved, because the
# merged parameter block is not sourced where those patches were written.

# NOTHING [SELECT]-PREFIXED. The database has a family of codes that activate
# while SELECT is held -- money, photon drops, a character modifier -- and this
# build binds SELECT to the area-map toggle. They would work, but every use
# would also flip the map up, which reads as a bug rather than a cheat.



def main():
    if not SRC.is_file():
        sys.exit("!! missing %s" % SRC)
    xml = SRC.read_text()

    m = re.search(r"<gameid>([^<]+)</gameid>", xml)
    if not m:
        sys.exit("!! no <gameid> in %s -- the importer matches ROMs on it" % SRC.name)
    gameid = m.group(1).strip()

    pairs = re.findall(
        r"<cheat>\s*<name>([^<]+)</name>.*?<codes>(.*?)</codes>", xml, re.S)
    # AMBIGUITY IS REFUSED, not resolved by taking the last one. "x2" is an
    # experience multiplier here, but a database that grew a movement "x2"
    # would silently swap the code under a menu entry that still read the same.
    seen = {}
    for n, c in pairs:
        seen.setdefault(n, []).append(c)
    blocks = {n: c[0] for n, c in seen.items() if len(c) == 1}
    ambiguous = {n for n, c in seen.items() if len(c) > 1}

    OUT.parent.mkdir(parents=True, exist_ok=True)
    kept, missing = 0, []
    with OUT.open("w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<codelist>\n')
        f.write("  <name>%s</name>\n" % sx.escape(DB_NAME))
        f.write("  <game>\n    <name>Phantasy Star 0 (USA)</name>\n")
        f.write("    <gameid>%s</gameid>\n" % sx.escape(gameid))
        for folder, names in FOLDERS:
            present = [n for n in names if n in blocks]
            missing += [(n, "AMBIGUOUS -- appears %d times" % len(seen[n])
                         if n in ambiguous else "not in the database")
                        for n in names if n not in blocks]
            if not present:
                continue
            f.write("    <folder>\n      <name>%s</name>\n" % sx.escape(folder))
            for n in present:
                codes = " ".join(blocks[n].split())
                f.write("      <cheat>\n        <name>%s</name>\n" % sx.escape(n))
                f.write("        <codes>%s</codes>\n      </cheat>\n" % codes)
                kept += 1
            f.write("    </folder>\n")
        for folder, entries in FIRST_PARTY:
            f.write("    <folder>\n      <name>%s</name>\n" % sx.escape(folder))
            for name, codes, note in entries:
                f.write("      <cheat>\n        <name>%s</name>\n" % sx.escape(name))
                f.write("        <note>%s</note>\n" % sx.escape(note))
                f.write("        <codes>%s</codes>\n      </cheat>\n" % codes)
                kept += 1
            f.write("    </folder>\n")
        f.write("  </game>\n</codelist>\n")

    print(">> %s  (%d cheats, gameid %s)" % (OUT, kept, gameid))
    # Named rather than silently dropped: a rename upstream should be visible,
    # not quietly shrink the list people see in the menu.
    for n, why in missing:
        print("   SKIPPED %s: %s" % (n, why))


if __name__ == "__main__":
    main()
