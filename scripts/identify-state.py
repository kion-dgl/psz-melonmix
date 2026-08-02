#!/usr/bin/env python3
"""Report which game state a melonDS savestate is sitting in.

Answers "what mode is this?" without a device, an emulator or a capture, which
is what identifying the remaining screens (select mode, name entry) needs.

    python3 scripts/identify-state.py <savestate>...

Reads two things out of the state's main RAM:

  the overlay resident at 0x0211E640   ov00..ov18 share one slot, so which one
                                       is loaded IS the game's own notion of
                                       its mode (psz-re docs/game-state.md)
  *(0x02108D04)                        the player object; NULL in every mode
                                       that is not the main game

FINDING MAIN RAM. The savestate stores it at an offset that moves between
melonDS versions -- 0x24 and 0x54 both occur across the states in this repo's
sandboxes -- so it is located by a landmark instead of a constant: the ROM
header copy the BIOS leaves at 0x023FFE00, whose gamecode is "C24E". Note the
4MB mirror; 0x027FFE00 is the address usually quoted and it is not where a
retail DS state has it.

This reads states, never writes them, and prints no game content -- only which
overlay id matched.
"""
import glob
import pathlib
import re
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
SLOT = 0x0211E640 - 0x02000000
PLAYER = 0x02108D04 - 0x02000000
HDRCOPY = 0x023FFE0C - 0x02000000

# From psz-re docs/game-state.md, which names eight of the nineteen. Eleven are
# genuinely unmapped -- an id printed as UNIDENTIFIED is the interesting case,
# not a formatting gap.
#
# ov16/ov17 carry BOTH accounts because they disagree: psz-re names them from
# the boot sequence, this project's captures from in-game content. Not resolved.
KNOWN = {
    0: "title -- PRESS START",
    4: "main game (field / town)",
    6: "ending / credits",
    11: "character create",
    12: "counter / shop UI",
    14: "file select",
    16: "intro logo / cutscene (?)",
    17: "attract cutscene / dialogue (?)",
}


def fingerprints():
    hdr = (HERE.parent / "psz" / "PSZOverlayIDs.h").read_text()
    pairs = re.findall(r"\{\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+)u?\s*\}", hdr)
    return {int(crc, 0): int(oid) for oid, crc in pairs}


def identify(path, fps):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"MELN":
        return "not a melonDS savestate"

    for m in re.finditer(b"C24E", data):
        base = m.start() - HDRCOPY
        if base < 0 or base + SLOT + 512 > len(data):
            continue
        crc = zlib.crc32(data[base + SLOT:base + SLOT + 512]) & 0xFFFFFFFF
        if crc not in fps:
            continue
        oid = fps[crc]
        player = int.from_bytes(data[base + PLAYER:base + PLAYER + 4], "little")
        label = KNOWN.get(oid, "UNIDENTIFIED")
        return "ov=%-3d %-26s player=%s" % (
            oid, label, "0x%08x" % player if player else "NULL")

    return "no known overlay resident (state not recognised)"


def main():
    args = sys.argv[1:]
    if not args:
        args = sorted(glob.glob(str(pathlib.Path.home() /
                      "projects/psz-melonmix/sandbox*/savestates/*.ml*")))
    if not args:
        print("usage: identify-state.py <savestate>...", file=sys.stderr)
        return 2

    fps = fingerprints()
    for path in args:
        name = "/".join(pathlib.Path(path).parts[-3:])
        print("%-52s %s" % (name, identify(path, fps)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
