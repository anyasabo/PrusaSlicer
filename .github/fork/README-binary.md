# PrusaSlicer 3.0 alpha: fork anyasabo/PrusaSlicer

This is not an official Prusa build. It is PrusaSlicer 3.0 alpha from Prusa Research, with changes for sequential
printing ("Complete individual objects"). Source: https://github.com/anyasabo/PrusaSlicer, branch `anya/main`.
License: AGPL-3.0, as upstream.

## What is different from upstream

- The slicer checks a sequential print again. It warns if the extruder or the gantry will hit a printed object, and
  it suggests a print order that works. Rafts, supports, skirts and draft shields are in the check.
- The arrange places the objects for a sequential print and sets the print order. It uses the shapes of the
  extruder and the gantry of each Prusa printer.
- Corrected toolhead data for the CORE One printers (a second slice of the toolhead, a 99 mm ceiling).

The settings and profiles are the same as upstream 3.0 alpha. This build uses the same profile directory.

## Install

- **macOS (Apple silicon):** open the dmg and move PrusaSlicer to Applications. The app has no Apple signature. For
  the first start, right-click the app and select Open. Or run `xattr -dr com.apple.quarantine /Applications/PrusaSlicer.app`.
- **Linux, AppImage:** `chmod +x PrusaSlicer-*.AppImage`, then start it.
- **Linux, tar.gz:** extract it and start `bin/prusa-slicer`. It needs GTK 3 and WebKitGTK 4.1 from your system
  (Ubuntu and Debian: `libgtk-3-0t64 libwebkit2gtk-4.1-0`). It needs glibc 2.39 or newer (Ubuntu 24.04, Fedora 40).

## Problems

Report problems with this build at https://github.com/anyasabo/PrusaSlicer/issues, not to Prusa Research.
