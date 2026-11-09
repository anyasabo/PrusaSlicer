**Not an official Prusa build.** PrusaSlicer 3.0 alpha with changes for sequential printing. Read
[README-binary.md](https://github.com/anyasabo/PrusaSlicer/blob/anya/main/.github/fork/README-binary.md) before you
install (macOS: the app has no Apple signature).

## Changes from upstream

- The sequential-print collision check is back: a warning names the objects, and it suggests an order that works.
  Rafts, supports, skirts and draft shields are in the check.
- A sequential arrange: it places the objects and sets the print order. A fast greedy placement first, then the z3
  solver if it can save a plate.
- CORE One printers: the second toolhead slice and the 99 mm ceiling.
- Fixes in libseqarrange: an overflow that gave layouts with collisions, and smaller errors.

## Files

| File | For |
| --- | --- |
| `*-macos-arm64.dmg` | macOS on Apple silicon |
| `*-linux-x64.AppImage` | Linux x86_64 |
| `*-linux-x64.tar.gz` | Linux x86_64, with the GTK and WebKitGTK of your system |
| `SHA256SUMS-*.txt` | Checksums |
