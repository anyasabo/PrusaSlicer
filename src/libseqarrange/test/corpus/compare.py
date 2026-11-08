#!/usr/bin/env python3
"""Compares two result files of seqarrange_bench (JSON lines), or shows one file as a table.

  uv run compare.py RESULTS.jsonl                 show a table
  uv run compare.py BASELINE.jsonl NEW.jsonl      compare, exit code 1 on a regression

A regression is: a failed check (if the check of the baseline did not fail too), more plates than the
baseline, fewer objects on plate 1, or more than 1.5 times the baseline time (only if the baseline time is
more than 200 ms).
"""

import json
import sys


def load(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            if "skipped" in d:
                continue
            out[(d["case"], d["printer"])] = d
    return out


def show(results):
    print(f"{'case':<18} {'printer':<13} {'obj':>4} {'ok':>3} {'plates':>6} {'p1':>3} {'ms':>9} {'checks':>6} {'unk':>4} {'det':>4}")
    for (case, printer), d in sorted(results.items()):
        print(f"{case:<18} {printer:<13} {d['objects']:>4} {'y' if d['ok'] else 'N':>3} {d['plates']:>6} {d['plate1']:>3} "
              f"{d['wall_ms']:>9.1f} {d['checks']:>6} {d['unknown']:>4} {'y' if d['deterministic'] else 'N':>4}")
    total = sum(d["wall_ms"] for d in results.values())
    plates = sum(d["plates"] for d in results.values())
    print(f"total: {len(results)} runs, {plates} plates, {total / 1000:.1f} s")


def compare(base, new):
    bad = []
    for key, n in sorted(new.items()):
        b = base.get(key)
        name = f"{key[0]}/{key[1]}"
        if not n["ok"]:
            if b is not None and not b["ok"]:
                print(f"known failure: {name}: {n.get('why', '')}")
            else:
                bad.append(f"{name}: check failed: {n.get('why', '')}")
        if b is None:
            continue
        if n["plates"] > b["plates"]:
            bad.append(f"{name}: plates {b['plates']} -> {n['plates']}")
        elif n["plates"] == b["plates"] and n["plate1"] < b["plate1"]:
            bad.append(f"{name}: objects on plate 1 {b['plate1']} -> {n['plate1']}")
        if b["wall_ms"] > 200 and n["wall_ms"] > 1.5 * b["wall_ms"]:
            bad.append(f"{name}: time {b['wall_ms']:.0f} -> {n['wall_ms']:.0f} ms")
    tb = sum(base[k]["wall_ms"] for k in new if k in base)
    tn = sum(new[k]["wall_ms"] for k in new if k in base)
    pb = sum(base[k]["plates"] for k in new if k in base)
    pn = sum(new[k]["plates"] for k in new if k in base)
    print(f"common runs: {sum(1 for k in new if k in base)}, plates {pb} -> {pn}, time {tb / 1000:.1f} -> {tn / 1000:.1f} s")
    better = [f"{k[0]}/{k[1]}: plates {base[k]['plates']} -> {n['plates']}"
              for k, n in sorted(new.items()) if k in base and n["plates"] < base[k]["plates"]]
    for line in better:
        print("better:", line)
    for line in bad:
        print("REGRESSION:", line)
    return 1 if bad else 0


def main():
    if len(sys.argv) == 2:
        show(load(sys.argv[1]))
        return 0
    if len(sys.argv) == 3:
        return compare(load(sys.argv[1]), load(sys.argv[2]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
