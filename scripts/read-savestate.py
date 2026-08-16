#!/usr/bin/env python3
"""Read the PSZ level structure out of a melonDS savestate.

Rather than parse melonDS's chunk format, MainRAM is LOCATED BY VALIDATION: the
DS maps it at 0x02000000, and psz-re's pointers give three facts that a wrong
offset cannot satisfy at once --

    *(0x02108C64)          room root, must point into main RAM
    *(root)                room table base, must point into main RAM
    *(base + 0x410)        room count, must be 1..20

Scanning for the file offset where all three hold pins MainRAM exactly. Room
record layout (psz-re): 0x34 bytes, cell +0x2E/+0x2F, exits +0x18..+0x1B,
gate types +0x1C..+0x1F, keys +0x2C. Player position is at 0x021A2164 (x),
+0x68 (y), +0x6C (z) in 1/4096 fixed point, with facing at +0x70. Current room is a
u8 at root+0x16, previous room at root+0x0E.
"""
import sys, json, struct, zlib, pathlib

RAM_BASE, RAM_SIZE = 0x02000000, 0x400000
DIRS = "NESW"


def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def find_ram(buf):
    """Return the file offset where main RAM begins, or None."""
    # 4-byte aligned, not page aligned: melonDS writes main RAM at file
    # offset 0x24, right after the MELN header and the NDSG section tag.
    for base in range(0, min(len(buf) - RAM_SIZE + 1, 0x100000), 4):
        try:
            root = u32(buf, base + 0x108C64)
            if not (RAM_BASE <= root < RAM_BASE + RAM_SIZE):
                continue
            tbl = u32(buf, base + (root - RAM_BASE))
            if not (RAM_BASE <= tbl < RAM_BASE + RAM_SIZE):
                continue
            n = u32(buf, base + (tbl - RAM_BASE) + 0x410)
            if 1 <= n <= 20:
                return base, root, tbl, n
        except Exception:
            continue
    return None


def cell_name(cx, cy):
    return f"{chr(ord('A') + cx)}{cy + 1}"


def main(path):
    raw = pathlib.Path(path).read_bytes()
    hit = find_ram(raw)
    if hit is None:
        try:
            raw = zlib.decompress(raw)
            hit = find_ram(raw)
        except Exception:
            pass
    if hit is None:
        print("could not locate main RAM in this file", file=sys.stderr)
        return 1

    off, root, tbl, n = hit
    ram = lambda a: off + (a - RAM_BASE)
    print(f"main RAM at file offset 0x{off:X}; root=0x{root:08X} table=0x{tbl:08X} rooms={n}")

    px = struct.unpack_from("<i", raw, ram(0x021A2164))[0]
    pz = struct.unpack_from("<i", raw, ram(0x021A216C))[0]
    print(f"player world x={px/4096:.2f} z={pz/4096:.2f}")

    rooms = []
    print(f"\n{'#':>2} {'cell':>4}  {'exits':<16} {'gates':<16} keys  record")
    for i in range(n):
        rec = ram(tbl) + i * 0x34
        r = raw[rec:rec + 0x34]
        cx, cy = r[0x2E], r[0x2F]
        ex = [r[0x18 + k] for k in range(4)]
        ga = [r[0x1C + k] for k in range(4)]
        keys = r[0x2C]
        rooms.append((cx, cy, ex, ga, keys))
        es = ",".join(f"{DIRS[k]}>{ex[k]}" for k in range(4) if ex[k] != 0xFF) or "-"
        gs = ",".join(f"{DIRS[k]}={ga[k]}" for k in range(4) if ex[k] != 0xFF) or "-"
        print(f"{i:2d} {cell_name(cx,cy):>4}  {es:<16} {gs:<16} {keys:>4}  {r.hex()}")

    # Where the player is, read rather than derived. Deriving a cell from
    # world position was tried and is wrong -- the coordinates look room-local
    # and the grid origin is not cell 0 -- so this reads the index the game
    # itself keeps and indexes the table with it.
    cur = raw[ram(root + 0x16)]
    prev = raw[ram(root + 0x0E)]
    here = f"{cell_name(*rooms[cur][:2])} (room {cur})" if cur < n else f"?? ({cur})"
    was = f"{cell_name(*rooms[prev][:2])} (room {prev})" if prev < n else f"?? ({prev})"
    print(f"\ncurrent room: {here}")
    print(f"previous room: {was}   <- stale until the first room change of a level")

    # A RUN AS DATA, so a field can be recreated rather than described. Only
    # what the game itself holds goes in here -- rooms, cells, doors, gate
    # types, keys. Observed contents (waves, boxes, traps) are deliberately NOT
    # invented: they are left as empty slots for a play report to fill, so the
    # file never claims to know something nobody measured.
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
        doors = lambda r: {DIRS[k]: {"to": r[2][k], "gate": r[3][k]}
                           for k in range(4) if r[2][k] != 0xFF}
        shape = lambda r: ("n" if len(doors(r)) == 1 else
                           ("i" if (r[2][0] != 0xFF and r[2][2] != 0xFF) or
                                   (r[2][1] != 0xFF and r[2][3] != 0xFF) else "l")
                           if len(doors(r)) == 2 else
                           "t" if len(doors(r)) == 3 else "x")
        doc = {
            "source": str(pathlib.Path(path).name),
            "note": "Rooms/doors/gates/keys are read from DS RAM. 'observed' is "
                    "for a play report and is empty until filled in by hand.",
            "stage_field_0x00": raw[ram(tbl) + 0x00],
            "gate_types": {"0": "open", "2": "key gate (2 keys)", "4": "enemy defeat"},
            "current_room": cur if cur < n else None,
            "rooms": [
                {
                    "index": i,
                    "cell": cell_name(r[0], r[1]),
                    "cx": r[0], "cy": r[1],
                    "doors": doors(r),
                    "shape": shape(r),
                    "keys": r[4],
                    "record": raw[ram(tbl) + i * 0x34: ram(tbl) + (i + 1) * 0x34].hex(),
                    "observed": {"model": None, "waves": [], "boxes": None, "traps": {}},
                }
                for i, r in enumerate(rooms)
            ],
        }
        pathlib.Path(out).write_text(json.dumps(doc, indent=2))
        print(f"\nwrote {out}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
