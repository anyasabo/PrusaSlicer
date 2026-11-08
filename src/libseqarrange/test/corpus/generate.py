#!/usr/bin/env python3
"""Generates the corpus of the sequential arrange benchmark (seqarrange_bench).

Run it from any directory: uv run src/libseqarrange/test/corpus/generate.py
The output is deterministic (fixed seeds). The script writes one .txt file for each case next to itself.

Each object is a footprint (a polygon at height 0) and a total height. seqarrange_bench extrudes the
footprint to the total height for each printer. See test/seqarrange_bench.cpp for the file format.
"""

import math
import random
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCALE = 1_000_000  # slicer units for each mm

# The printers with a bed that holds a footprint of this size (mm), with 10 mm of margin.
BEDS = {
    "MK4S": (250, 210),
    "COREONE": (250, 220),
    "COREONEL": (300, 300),
    "COREONE_INDX": (248, 205),
    "XL": (360, 360),
    "MINI": (180, 180),
}


def printers_for(max_x, max_y):
    out = []
    for name, (bx, by) in BEDS.items():
        if max_x + 10 <= bx and max_y + 10 <= by:
            out.append(name)
    return out


def rect(w, h):
    """A w x h mm rectangle, centered, counterclockwise."""
    x, y = w / 2, h / 2
    return [(-x, -y), (x, -y), (x, y), (-x, y)]


def ngon(r, n):
    """A regular polygon with n corners and radius r mm, counterclockwise."""
    return [(r * math.cos(2 * math.pi * i / n), r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def size(pgn):
    xs = [p[0] for p in pgn]
    ys = [p[1] for p in pgn]
    return max(xs) - min(xs), max(ys) - min(ys)


def prusa_parts():
    """The footprints of PRUSA_PART_POLYGONS in test/prusaparts.cpp, in mm, centered."""
    text = (HERE.parent / "prusaparts.cpp").read_text()
    start = text.index("PRUSA_PART_POLYGONS =")
    end = text.index("};", start)
    body = text[start:end]
    parts = []
    for block in re.findall(r"\{\s*((?:\{-?\d+, -?\d+\},\s*)+)\}", body):
        pts = [(int(a) / SCALE, int(b) / SCALE) for a, b in re.findall(r"\{(-?\d+), (-?\d+)\}", block)]
        if len(pts) > 1 and pts[0] == pts[-1]:
            pts = pts[:-1]
        cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2
        cy = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2
        parts.append([(x - cx, y - cy) for x, y in pts])
    return parts


def write_case(name, objects, comment, printers=None):
    """objects: a list of (footprint in mm, height in mm, glued_to_next)."""
    if printers is None:
        w = max(size(o[0])[0] for o in objects)
        h = max(size(o[0])[1] for o in objects)
        printers = printers_for(w, h)
    lines = [f"#NAME {name}", f"# {comment}", "#PRINTERS " + " ".join(printers)]
    for i, (pgn, height, glued) in enumerate(objects, start=1):
        lines.append(f"OBJECT_ID{i}")
        lines.append(f"TOTAL_HEIGHT{round(height * SCALE)}")
        lines.append("POLYGON_AT_HEIGHT0")
        for x, y in pgn:
            lines.append(f"POINT{round(x * SCALE)} {round(y * SCALE)}")
        if glued:
            lines.append("#GLUED")
    (HERE / f"{name}.txt").write_text("\n".join(lines) + "\n")


def main():
    for old in HERE.glob("*.txt"):
        old.unlink()

    for n in (10, 20, 50):
        write_case(f"identical_{n:02d}", [(rect(20, 20), 10, False)] * n,
                   f"{n} identical 20 x 20 x 10 mm boxes: the time growth with the object count")

    parts = prusa_parts()
    rng = random.Random(1)
    small = [p for p in parts if max(size(p)) <= 120]
    write_case("prusaparts_mixed", [(p, rng.uniform(5, 20), False) for p in small[:16]],
               "16 footprints from prusaparts.cpp, low (5 to 20 mm)")

    write_case("tall_gantry", [(rect(30, 30), 60, False)] * 2 + [(rect(20, 20), 10, False)] * 6,
               "2 boxes higher than each gantry (60 mm) and 6 low boxes: the gantry of a later object must not cross a tall box")

    rng = random.Random(2)
    objs = [(rect(25, 25), rng.choice((8, 15, 28, 40)), False) for _ in range(12)]
    write_case("tall_short_mixed", objs, "12 boxes of 8, 15, 28 or 40 mm: near the gantry heights")

    write_case("long_thin", [(rect(150, 15), 5, False)] * 6,
               "6 bars of 150 x 15 x 5 mm, along X: a case for rotation")
    write_case("long_thin_y", [(rect(15, 150), 5, False)] * 6,
               "6 bars of 15 x 150 x 5 mm, along Y: a case for rotation")

    write_case("near_bed", [(rect(200, 170), 5, False), (rect(20, 20), 5, False)],
               "one plate-size part and one small box")

    write_case("instances_glued", [(rect(30, 30), 12, i < 4) for i in range(5)] + [(rect(40, 20), 8, False)] * 3,
               "5 instances of one object (glued_to_next) and 3 other objects")

    write_case("overflow", [(rect(60, 60), 10, False)] * 24,
               "24 boxes of 60 x 60 mm: more than one plate")

    write_case("round_parts", [(ngon(18, 24), 15, False)] * 10, "10 round parts (24-gons, diameter 36 mm)")

    for seed in (11, 12, 13):
        rng = random.Random(seed)
        objs = []
        for _ in range(rng.randint(8, 18)):
            kind = rng.random()
            if kind < 0.5:
                pgn = rect(rng.uniform(10, 60), rng.uniform(10, 60))
            elif kind < 0.8:
                pgn = ngon(rng.uniform(6, 30), rng.randint(5, 12))
            else:
                pgn = rng.choice(small)
            height = rng.choice((rng.uniform(2, 20), rng.uniform(2, 20), rng.uniform(20, 50)))
            objs.append((pgn, height, False))
        write_case(f"random_{seed}", objs, f"random scene, seed {seed}")


if __name__ == "__main__":
    main()
