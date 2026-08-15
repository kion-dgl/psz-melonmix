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
+0x68 (y), +0x6C (z) in 1/4096 fixed point, with facing at +0x70.
"""
import sys, struct, zlib, pathlib

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

    # Which cell the player is standing in: cells are 44x44 world units spanning
    # -22..+22 (psz-re room_doorways.json), so the cell is arithmetic. Reported
    # for every plausible axis/sign convention, since which one the field uses
    # is the one thing still unpinned -- the row that names a cell the table
    # actually has is the right convention.
    print("\nplayer cell under each convention (only rows hitting a real room matter):")
    cells = {(cx, cy): i for i, (cx, cy, *_ ) in enumerate(rooms)}
    cw = 44 * 4096
    fl = lambda v: (v + cw // 2) // cw if (v + cw // 2) >= 0 else -((-(v + cw // 2) + cw - 1) // cw)
    rx, rz = fl(px), fl(pz)
    for swap in (0, 1):
        for sx in (1, -1):
            for sz in (1, -1):
                a, b = (rz, rx) if swap else (rx, rz)
                for dx in range(-8, 9):
                    for dy in range(-8, 9):
                        c = (sx * a + dx, sz * b + dy)
                        if c in cells:
                            print(f"  swap={swap} sx={sx:+d} sz={sz:+d} off=({dx},{dy}) "
                                  f"-> cell {cell_name(*c)} = room {cells[c]}")
                            break
                    else:
                        continue
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
