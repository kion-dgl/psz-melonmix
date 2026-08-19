#!/usr/bin/env python3
"""Read the PSZ level structure out of a melonDS savestate.

Rather than parse melonDS's chunk format, MainRAM is LOCATED BY VALIDATION: the
DS maps it at 0x02000000, and psz-re's pointers give three facts that a wrong
offset cannot satisfy at once --

    *(0x02108C64)          room root, must point into main RAM
    *(root)                room table base, must point into main RAM
    *(base + 0x410)        room count, must be 1..20

Scanning for the file offset where all three hold pins MainRAM exactly. Room
record layout (psz-re): 0x34 bytes, stage +0x00, model name +0x08/+0x0C/+0x10,
per-room seed +0x14, exits +0x18..+0x1B, gate types +0x1C..+0x1F, keys +0x2C,
cell +0x2E/+0x2F. Player position is at 0x021A2164 (x),
+0x68 (y), +0x6C (z) in 1/4096 fixed point, with facing at +0x70. Current room is a
u8 at root+0x16, previous room at root+0x0E.

THE MODEL NAME IS IN THE RECORD -- it is not derived from the door count and it
is not inferred from contents. Inferring it from contents is wrong, not merely
slower: it produced two bad calls (two lc1 rooms read as lc2, an ic3 argued down
to ib2) that only surfaced once these bytes were decoded. See
kion-dgl/psz-melonmix#8.
"""
import sys, json, struct, zlib, pathlib

RAM_BASE, RAM_SIZE = 0x02000000, 0x400000
DIRS = "NESW"
SHAPES = "iltxnsg"   # +0x08
LETTERS = "abcdr"    # +0x0C -- class 'r' (index 4) is the rare-room (nr*) class;
                     # a 4-char "abcd" table decodes every forced rare room as '?'
                     # (the false-negative documented in psz-godot's RE reference).


def model_name(stage, shape, letter, digit):
    sh = SHAPES[shape] if shape < len(SHAPES) else "?"
    le = LETTERS[letter] if letter < len(LETTERS) else "?"
    return f"s{stage:02d}_{sh}{le}{digit}"


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
    print(f"\n{'#':>2} {'cell':>4} {'model':<8} {'exits':<16} {'gates':<16} keys "
          f"{'seed':>10}  record")
    for i in range(n):
        rec = ram(tbl) + i * 0x34
        r = raw[rec:rec + 0x34]
        room = {
            "index": i,
            "cx": r[0x2E], "cy": r[0x2F],
            "exits": [r[0x18 + k] for k in range(4)],
            "gates": [r[0x1C + k] for k in range(4)],
            "keys": r[0x2C],
            "stage": r[0x00],
            "shape": r[0x08], "letter": r[0x0C], "digit": r[0x10],
            "seed": u32(r, 0x14),
            "record": r.hex(),
        }
        room["cell"] = cell_name(room["cx"], room["cy"])
        room["model"] = model_name(room["stage"], room["shape"],
                                   room["letter"], room["digit"])
        rooms.append(room)
        ex, ga = room["exits"], room["gates"]
        es = ",".join(f"{DIRS[k]}>{ex[k]}" for k in range(4) if ex[k] != 0xFF) or "-"
        gs = ",".join(f"{DIRS[k]}={ga[k]}" for k in range(4) if ex[k] != 0xFF) or "-"
        print(f"{i:2d} {room['cell']:>4} {room['model']:<8} {es:<16} {gs:<16} "
              f"{room['keys']:>4} 0x{room['seed']:08X}  {room['record']}")

    # Where the player is, read rather than derived. Deriving a cell from
    # world position was tried and is wrong -- the coordinates look room-local
    # and the grid origin is not cell 0 -- so this reads the index the game
    # itself keeps and indexes the table with it.
    cur = raw[ram(root + 0x16)]
    prev = raw[ram(root + 0x0E)]
    label = lambda i: f"{rooms[i]['cell']} {rooms[i]['model']} (room {i})"
    here = label(cur) if cur < n else f"?? ({cur})"
    was = label(prev) if prev < n else f"?? ({prev})"
    print(f"\ncurrent room: {here}")
    print(f"previous room: {was}   <- stale until the first room change of a level")

    # A RUN AS DATA, so a field can be recreated rather than described. Only
    # what the game itself holds goes in here -- rooms, cells, doors, gate
    # types, keys. Observed contents (waves, boxes, traps) are deliberately NOT
    # invented: they are left as empty slots for a play report to fill, so the
    # file never claims to know something nobody measured.
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
        doors = lambda r: {DIRS[k]: {"to": r["exits"][k], "gate": r["gates"][k]}
                           for k in range(4) if r["exits"][k] != 0xFF}
        doc = {
            "source": str(pathlib.Path(path).name),
            "note": "Rooms/doors/gates/keys/model are read from DS RAM. 'observed' "
                    "is for a play report and is empty until filled in by hand. "
                    "Do NOT write 'model' from observed contents -- the record "
                    "already says it (psz-melonmix#8).",
            "stage_field_0x00": raw[ram(tbl) + 0x00],
            "gate_types": {"0": "open", "2": "key gate (2 keys)", "4": "enemy defeat"},
            "current_room": cur if cur < n else None,
            "rooms": [
                {
                    "index": r["index"],
                    "cell": r["cell"],
                    "cx": r["cx"], "cy": r["cy"],
                    "model": r["model"],
                    "doors": doors(r),
                    "shape": SHAPES[r["shape"]] if r["shape"] < len(SHAPES) else "?",
                    "keys": r["keys"],
                    "seed": r["seed"],
                    "record": r["record"],
                    # What a play report fills in. 'traps' is the measurement
                    # kion-dgl/psz-godot#615 is actually waiting on -- counts per
                    # trap kind, from DELIBERATELY triggering them, not from
                    # what happened to go off in passing.
                    "observed": {"waves": [], "boxes": None, "rare_box": None,
                                 "traps": {}, "trap_sweep": False},
                }
                for r in rooms
            ],
        }
        pathlib.Path(out).write_text(json.dumps(doc, indent=2))
        print(f"\nwrote {out}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
