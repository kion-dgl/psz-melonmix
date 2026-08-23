#!/usr/bin/env python3
"""Aggregate decoded DS field runs into the two measurements the issues want.

    scripts/read-savestate.py runs/rioh-01.ml1 --json runs/rioh-01.json
    scripts/analyze-runs.py runs/*.json

Each input is one level, read out of DS RAM by read-savestate.py. This turns a
pile of them into the two tables that kion-dgl/psz-godot#614 and #615 are
waiting on, and states what the data does and does not yet support.

KEY SCATTER (#614)
    The DS put BOTH keys of a 2-key gate in ONE room, seven rooms at zero.
    psz-godot's `_scatter_keys` round-robins over a shuffled pool, one key per
    room per pass, so a 2-key gate lands in TWO rooms -- its committed dump has
    key_count==1 in all 67 key cells across 112 sections, never 2.

    So the discriminating question per level is not WHICH room holds keys, it
    is HOW MANY rooms do. One room holding the gate's full requirement
    supports the DS rule; two rooms holding one each supports psz-godot's.
    n=1 is a coincidence; this counts levels.

TRAP COUNTS (#615)
    Observed contents only mean something against the room MODEL, which the
    record now gives directly -- never infer it from contents, that produced
    two wrong calls (psz-melonmix#8). Rooms count toward the measurement only
    when `trap_sweep` is true, i.e. everything in the room was deliberately
    set off. A room walked through is not a measurement of absence.
"""
from __future__ import annotations

import collections
import json
import pathlib
import sys

GATE_NAMES = {0: "open", 2: "key gate", 4: "enemy defeat"}


def load(paths: list[str]) -> list[dict]:
    runs = []
    for p in paths:
        doc = json.loads(pathlib.Path(p).read_text())
        doc["_file"] = pathlib.Path(p).name
        runs.append(doc)
    return runs


def key_scatter(runs: list[dict]) -> None:
    print("=" * 72)
    print("#614  KEY SCATTER -- how many rooms hold a gate's keys")
    print("=" * 72)
    print(f"\n{'run':<22} {'rooms':>5} {'keyrooms':>8} {'keys':>4}  where")

    verdicts: collections.Counter = collections.Counter()
    for r in runs:
        rooms = r["rooms"]
        holders = [c for c in rooms if c.get("keys", 0) > 0]
        total = sum(c["keys"] for c in holders)
        where = ", ".join(f"{c['cell']} {c['model']} x{c['keys']}"
                          for c in holders) or "-"
        print(f"{r['_file']:<22} {len(rooms):>5} {len(holders):>8} "
              f"{total:>4}  {where}")

        # The verdict is per gate-requirement, not per level: a level with no
        # key gate says nothing either way and must not be counted as support.
        gated = sum(1 for c in rooms for d in c.get("doors", {}).values()
                    if d.get("gate") == 2)
        if total == 0:
            verdicts["no keys in level"] += 1
        elif len(holders) == 1 and total > 1:
            verdicts["CONCENTRATED (DS rule)"] += 1
        elif len(holders) > 1 and max(c["keys"] for c in holders) == 1:
            verdicts["SPLIT (psz-godot rule)"] += 1
        else:
            verdicts["mixed"] += 1
        r["_gated"] = gated

    print("\nverdict across runs:")
    for k, v in verdicts.most_common():
        print(f"  {v:>3}  {k}")

    conc = verdicts["CONCENTRATED (DS rule)"]
    split = verdicts["SPLIT (psz-godot rule)"]
    if conc and not split:
        print(f"\n  {conc} level(s) concentrated, 0 split. psz-godot's "
              f"round-robin diverges.")
        if conc < 5:
            print("  n is still small -- a single generator quirk could do this.")
    elif split and not conc:
        print(f"\n  {split} level(s) split. psz-godot's rule is right and the "
              f"original single observation was a coincidence.")
    elif conc and split:
        print(f"\n  BOTH shapes occur ({conc} concentrated, {split} split) -- so "
              f"neither is 'the' rule and\n  something else selects. The "
              f"per-room seed at +0x14 is the candidate.")


def gates_and_models(runs: list[dict]) -> None:
    print("\n" + "=" * 72)
    print("ROOM CENSUS -- models seen, from the record")
    print("=" * 72)
    models: collections.Counter = collections.Counter()
    gates: collections.Counter = collections.Counter()
    for r in runs:
        for c in r["rooms"]:
            models[c["model"]] += 1
            for d in c.get("doors", {}).values():
                gates[d.get("gate")] += 1
    print("\nmodels: " + ", ".join(f"{m} x{n}" for m, n in models.most_common()))
    print("gates:  " + ", ".join(
        f"{GATE_NAMES.get(g, g)}={n}" for g, n in sorted(gates.items(),
                                                         key=lambda kv: -kv[1])))
    print(f"\n{len(models)} distinct models over "
          f"{sum(models.values())} rooms in {len(runs)} level(s).")


def trap_counts(runs: list[dict]) -> None:
    print("\n" + "=" * 72)
    print("#615  TRAP / BOX COUNTS -- swept rooms only")
    print("=" * 72)

    swept, walked = [], []
    for r in runs:
        for c in r["rooms"]:
            o = c.get("observed") or {}
            (swept if o.get("trap_sweep") else walked).append((r["_file"], c))

    if not swept:
        print(f"\nNo swept rooms yet. {len(walked)} room(s) decoded but not "
              f"cast-verified.\nA room that was merely walked through is not "
              f"evidence of absence -- #615's own\nundercount table is what "
              f"established that.")
        return

    print(f"\n{'run':<18} {'cell':>4} {'model':<9} {'boxes':>5} {'traps':>5}  kinds")
    per_model: dict[str, list[int]] = collections.defaultdict(list)
    for f, c in swept:
        o = c["observed"]
        traps = o.get("traps") or {}
        n = sum(traps.values())
        kinds = ", ".join(f"{k} x{v}" for k, v in sorted(traps.items())) or "-"
        boxes = o.get("boxes")
        print(f"{f:<18} {c['cell']:>4} {c['model']:<9} "
              f"{boxes if boxes is not None else '?':>5} {n:>5}  {kinds}")
        per_model[c["model"]].append(n)

    print(f"\nswept rooms: {len(swept)}   (walked-only, not counted: {len(walked)})")
    if len(swept) < 20:
        print(f"#615 asked for 20+. {20 - len(swept)} to go before the "
              f"low-trap trend can be\ndistinguished from ordinary variance.")
    else:
        print("20+ swept rooms reached -- the group-5 weight question is now "
              "answerable.")
        print("\nmean traps per model:")
        for m, v in sorted(per_model.items()):
            print(f"  {m:<9} n={len(v):<3} mean={sum(v)/len(v):.2f}  {v}")


def main(argv: list[str]) -> int:
    paths = [a for a in argv[1:] if not a.startswith("-")]
    if not paths:
        print(__doc__)
        return 2
    runs = load(paths)
    key_scatter(runs)
    gates_and_models(runs)
    trap_counts(runs)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
