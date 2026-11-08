# Corpus of the sequential arrange benchmark

`seqarrange_bench` (source: `../seqarrange_bench.cpp`) arranges each case of this directory for each
printer. It checks each result and writes one JSON line for each case and printer.

## Files

- `*.txt`: the cases. The format is the text export of the arrange input (`load_exported_data_from_text()`),
  with three extra lines: `#NAME`, `#PRINTERS` and `#GLUED`. The bench source tells about the format.
- `generate.py`: makes the `*.txt` files. The output is the same each time (fixed seeds).
- `compare.py`: shows a result file as a table, or compares a result file with a baseline.
- `baseline.jsonl`: the results of 2026-11-08 (z3 5.3.3, `--rlimit 40000000 --repeat 3`, GCC 13
  RelWithDebInfo, Ryzen 7 8700G). The times are for this machine only. On another machine, make a new
  baseline before a change, and compare with it.

## Found errors

Before the fix of `Rational` (int64_t overflow), `random_11` on the CORE One gave a layout that failed the
collision check (objects 7 and 3). The fix changed only this layout.

## Run

The bench is built with `SLIC3R_BUILD_TESTS=ON` (target `seqarrange_bench`).

```sh
uv run src/libseqarrange/test/corpus/generate.py
build/src/libseqarrange/seqarrange_bench --geometries resources/data/printer_gantries/geometries.json \
    --rlimit 40000000 src/libseqarrange/test/corpus > new.jsonl
uv run src/libseqarrange/test/corpus/compare.py baseline.jsonl new.jsonl
```

## Checks

- Each layout must pass `check_ScheduledObjectsForSequentialConflict()`. Each object must be on one plate
  only, and inside the bed. If not, the line has `"ok": false`, and the exit code is 1.
- `compare.py` compares each run with the baseline. A regression is a run with more plates, with fewer
  objects on plate 1, or with more than 1.5 times the time.

## Repeatable results

- The GUI uses a wall-time timeout for each z3 call (8 s). A timeout counts as "does not fit", so the result
  changes with the machine load. For the benchmark, use `--rlimit` (the z3 resource limit). Then the bench
  sets no wall-time timeout. With 40000000, a z3 call that does not finish stops after
  approximately 7 s on a Ryzen 7 8700G. That is near the 8 s timeout of the GUI.
- z3 keeps state in the process between calls. The second arrange of the same input in one process can give
  a different layout. Thus the bench runs each arrange in a child process. `--no-fork` runs all arranges in
  one process, for example for a profiler.
- `--repeat 3` runs each case 3 times. The field `deterministic` shows whether the 3 layouts are the same.
