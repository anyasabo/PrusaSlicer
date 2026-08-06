# PR draft — XZ/YZ skew correction

Working notes for a pull request that has not been opened. Nothing here is
published; this file exists so the reasoning survives if the branch sits for a
while.

**Branch:** `feat/skew-xz-yz-correction`
**Base:** `califork/master` @ `0ffbcd3c4`
**Status:** not compile-tested, not print-tested

---

## Title

feat(skew): add XZ/YZ skew correction alongside the existing XY

## Summary

Extends `skew_xy_correction` into the full three-axis skew transform by adding
`skew_xz_correction` and `skew_yz_correction`. The slicer now reproduces
Marlin's `Planner::skew()` in its entirety rather than the XY plane alone.

## Motivation

Prusa firmware does not implement `M852`. In Prusa-Firmware-Buddy the G-code
dispatcher carries the comment `// M852: Set Skew factors REMOVED`, and
`SKEW_CORRECTION` is not defined in any Buddy printer configuration, so
Marlin's `skew_factor_t` members are compile-time zero and `Planner::skew()` /
`unskew()` are no-ops. There is no firmware path to correct skew on these
machines, which is why this fork corrects it at slicing time.

Upstream issue [prusa3d/Prusa-Firmware-Buddy#385][issue] has requested firmware
support since 2020 and remains open. The one sign of movement, from a Prusa
developer in July 2026, is a revived `M45` XY skew *calibration* routine for the
MINI — a PINDA-based measurement, still XY-only, and the same developer noted
earlier in the thread that "there are no Z skew compensations."

That matters because XY is usually the *smaller* error. XY skew is bounded by
the footprint of the part; the Z-coupled terms accumulate with height. On the
machine this was developed against:

| Pair | Factor (M852 convention) | Angle | Error over 100 mm |
|------|--------------------------|-------|-------------------|
| XY   | -0.0000997               | -0.0057° | 0.010 mm       |
| XZ   | -0.0000997               | -0.0057° | 0.010 mm       |
| YZ   | -0.0010670               | -0.0611° | 0.107 mm       |

The XY term is a tenth of an extrusion width and effectively unobservable. The
YZ term is a fifth of a millimetre on a 200 mm-tall part, which is the
difference between a press fit and a loose one. Correcting XY alone leaves the
dominant error in place.

[issue]: https://github.com/prusa3d/Prusa-Firmware-Buddy/issues/385

## What changed

| File | Change |
|------|--------|
| `src/libslic3r/PrintConfig.hpp` | Two `ConfigOptionFloat` entries |
| `src/libslic3r/PrintConfig.cpp` | Option definitions; XY tooltip gains the M852 relation |
| `src/libslic3r/GCode.hpp` | `apply_skew()`, two new factor members, `point_to_gcode` reworked |
| `src/libslic3r/GCode.cpp` | Init reads all three factors; inverse in `gcode_to_point`; arc-guard comment |
| `src/libslic3r/Preset.cpp` | Options added to `s_Preset_printer_options` |
| `src/slic3r/GUI/Tab.cpp` | Two lines in the existing Skew Correction optgroup |

Both options are printer-preset settings in degrees, range ±5, expert mode,
defaulting to 0. With them at 0 the generated G-code is byte-identical to
before.

## The transform

```
x' = x + (y - y_ref)*k_xy + z*(k_xz + k_xy*k_yz)
y' = y + z*k_yz
```

This is Marlin's `Planner::skew()`

```cpp
sx = cx - cy*XY - cz*(XZ - XY*YZ);
sy = cy - cz*YZ;
```

rewritten in the sign convention this fork already uses, where a positive
setting shears in the positive direction. The `k_xy*k_yz` cross term is
Marlin's; it is around 1e-7 in practice and is kept only so the two
implementations agree exactly.

The transform is affine, so a straight extrusion stays straight under it and
only endpoints need correcting.

## Design decisions

**Sign convention.** Kept the fork's existing `x += ...` rather than switching
to Marlin's `x -= ...`. Flipping it would silently invert every
`skew_xy_correction` value already configured in a user's printer preset. The
tooltips now state the conversion explicitly: enter `-atan(factor)` in degrees
to match an M852 factor.

**Z reference is the bed (z = 0).** Physical Z skew accumulates from the build
plate, so referencing the shear there leaves the first layer exactly where it
was sliced — no interaction with bed mesh sampling, skirt placement, or
adhesion — and lets the correction grow with height. Any other reference plane
differs only by a constant translation, so this is a placement choice rather
than a correctness one. Deliberately not exposed as an option; the Y reference
stays at bed center as before.

**Where Z comes from.** The 2D `point_to_gcode` overload has no Z of its own, so
it shears against `m_last_layer_z`, which `process_layer` assigns from `print_z`
before generating any toolpath for that layer. Exact within a layer. Under
spiral vase it quantizes the Z shear per layer, an error of one layer height
times the factor — roughly 0.2 µm at the factors above. The 3D overload uses the
point's own Z.

**Arc fitting stays keyed on XY only.** At constant Z the XZ/YZ terms reduce to
a uniform translation of the whole toolpath, which leaves arcs and their I/J
centre offsets valid. Only the XY shear turns circles into ellipses. So enabling
Z-only correction does not cost arc fitting — worth having, since Z is the term
most users will actually need.

**Inverse.** `gcode_to_point` gains the matching inverse. Y is un-sheared first,
because the X row is expressed in terms of the original Y; doing it in the other
order is wrong by `z*k_yz*k_xy`.

## Testing

The transform was cross-checked numerically against a separate transcription of
Marlin's `Planner::skew()` over 50 000 random points spanning the build volume:

- with `y_ref = 0`, the two agree exactly (max deviation 0.0 mm)
- with `y_ref` at bed center, they differ by exactly one distinct constant
  offset — a pure translation, so identical geometry
- the `gcode_to_point` inverse round-trips to 2.8e-14 mm
- XZ/YZ applied to a circle at constant Z yields exactly one distinct offset,
  confirming the arc-fitting decision above

**Not yet done, and required before merge:**

- [ ] Compile. Developed without a local build environment.
- [ ] Slice with all factors 0 and diff against a pre-change build — must be
      byte-identical.
- [ ] Slice a tall part with a YZ factor set and confirm the X/Y drift per layer
      matches `z * tan(angle)`.
- [ ] Confirm arcs still emit as G2/G3 with only XZ/YZ set.
- [ ] Check a spiral-vase print, the one case where per-layer Z quantization is
      observable in principle.
- [ ] Print and measure. Nothing here has been validated against a physical
      part.

## Known limitations

**Config change forces a full re-slice.** None of the skew options appear in
`Print::invalidate_state_by_config_options`, so they fall through to
`invalidate_all_steps()`. Correct but wasteful — they only affect G-code export
and belong in the `steps_gcode` set. Pre-existing for XY; XZ/YZ inherit it. Left
alone here because getting it wrong risks a stale wipe tower, and that wants a
test to verify.

**Custom G-code is not corrected.** The shear is applied in `point_to_gcode`, so
start/end/layer-change G-code and anything else written as literal coordinates
passes through untouched. Same as the existing XY behaviour.

**Correction is linear.** A single shear factor per plane models a
uniformly-skewed machine. Skew from a bent or twisted frame member can be
non-linear in position, and no factor will correct that — the fix there is
mechanical. Worth saying plainly in any user-facing documentation so this is not
mistaken for a repair.

## Relationship to the post-processing script

This supersedes the standalone G-code post-processor used previously. **Do not
run both**: with a skew option set in the printer preset *and* the post-processor
applying its own factor, the shear is applied twice. If migrating, zero the
post-processor's factors, or drop it from Print Settings → Output options →
Post-processing scripts entirely.
