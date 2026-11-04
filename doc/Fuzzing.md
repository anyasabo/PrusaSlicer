# Fuzz testing

PrusaSlicer opens files from the internet: Printables / MakerWorld 3MFs (via the
`prusaslicer://` download handler), STL/OBJ/STEP/SVG models, G-code and binary G-code, and
config bundles. The parsers for these are fuzzed with [libFuzzer] harnesses that live next to
the code they test, in each module's `fuzz/` directory (mirroring `test/`).

## Layout

```
cmake/modules/Fuzzing.cmake               SLIC3R_FUZZ option, slic3r_add_fuzzer()
src/slic3r-test-utils/fuzz/               replay main for non-libFuzzer builds
src/<module>/fuzz/<Name>Fuzzer.cpp        harness: defines LLVMFuzzerTestOneInput
src/<module>/fuzz/<name>.dict             libFuzzer dictionary (tokens of the format)
src/<module>/fuzz/corpus/<name>/          seeds; replayed by ctest, must never crash
src/<module>/fuzz/crashers/<name>/        local only (git-ignored): reproducers of open bugs. Keep
                                          them out of public branches until the bug is reported;
                                          move them into corpus/ once fixed
```

## Two build flavours, same harness

| | `SLIC3R_FUZZ=ON` | default (`SLIC3R_FUZZ=OFF`, `SLIC3R_BUILD_TESTS=ON`) |
| --- | --- | --- |
| Compiler | Clang only (on macOS: Homebrew LLVM; AppleClang has no libFuzzer) | any, incl. MSVC |
| Instrumentation | `-fsanitize=fuzzer-no-link` on all project code; libc++/libstdc++ hardening | none beyond `SLIC3R_ASAN`/`SLIC3R_UBSAN` if set |
| Harness links | libFuzzer, or `$LIB_FUZZING_ENGINE` (OSS-Fuzz / ClusterFuzzLite) | `slic3r-fuzz-replay` |
| ctest | `fuzz-<name> -runs=0 corpus/` | `fuzz-<name> corpus/` |

The default flavour keeps every harness compiling on every platform and turns the corpus into
a regression suite (`ctest -L fuzz`), which is the part that actually prevents re-introducing
a fixed bug.

## Running a fuzzer locally (macOS)

```sh
LLVM=$(brew --prefix llvm)
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) \
  -DCMAKE_CXX_FLAGS="-stdlib++-isystem $(xcrun --show-sdk-path)/usr/include/c++/v1" \
  -DSLIC3R_FUZZ=ON -DSLIC3R_ASAN=ON -DSLIC3R_UBSAN=ON -DSLIC3R_PCH=OFF \
  -DCMAKE_PREFIX_PATH=$PWD/deps/build/destdir/usr/local
cmake --build build-fuzz --target fuzz-facets_deserialize

M=src/slic3r-biz-algorithms/fuzz
mkdir -p /tmp/fz-work && build-fuzz/.../fuzz-facets_deserialize \
  /tmp/fz-work $M/corpus/facets_deserialize -max_total_time=600 -jobs=8
# Load3mfFuzzer / ObjFuzzer: add -dict=build-fuzz/.../load_3mf.dict and -max_len=262144
```

The `CMAKE_CXX_FLAGS` line keeps Homebrew clang (which ships libFuzzer) on the SDK's libc++
headers, the ones the deps were built against. Homebrew's own newer libc++ drops transitive
includes PrusaSlicer still relies on. Use `-stdlib++-isystem` (not `-isystem`, which lands after the C headers and breaks
`<cmath>`), without `-nostdinc++` (which disables it), and set the sysroot to the same SDK.

Set `UBSAN_OPTIONS=halt_on_error=1:suppressions=$PWD/src/slic3r-shared/fuzz/ubsan.supp`: without
`halt_on_error` UBSan findings are printed but not saved as crashes; the suppressions cover known,
benign reports that would otherwise mask real ones (ctest sets both).

Pass the scratch directory first so new inputs land there, not in the checked-in seeds; merge
interesting ones back with `-merge=1 corpus/<name> /tmp/fz-work`.

## Writing a harness

- Call the same entry point the application calls, with the same exception handling around
  it. Anything the application would not catch is allowed to escape.
- Prefer a structure-aware input over raw container bytes. `Load3mfFuzzer` takes a text
  container of `--- <archive path>` entries and zips it itself (no compression), so mutations
  reach the XML/JSON parsers instead of breaking zip CRCs. Inputs starting with `PK` are used
  as-is so real `.3mf` files can still be seeds; `fuzz/tools/3mf_to_corpus.py` converts them.
- When an outer layer validates (or `ASSERT`s on) input that an inner layer then trusts, add a
  second harness that feeds the inner layer well-formed-but-hostile data
  (`FacetsDeserializeFuzzer` behind the 3MF loader), so the fuzzer is not stuck on the first
  check.
- No global state between runs, no network, no GUI. Temp files are acceptable for loaders that
  only take a path; reuse one path per process.

## Priorities

Ordered by reachability from an untrusted file × how much hand-written, unchecked indexing
sits behind it:

1. **3MF paint data → `TriangleSelector`** (`facets_deserialize`, `load_3mf`). Per-triangle
   hex strings from the 3MF (`slic3rpe:mmu_segmentation`, `custom_supports`, Bambu's
   `paint_color`, new-format `Slic3r_facets_annotation.json`) are decoded and replayed with
   bounds guarded only by `assert()`.
2. **3MF loaders** (`load_3mf`): the expat SAX handlers in `Model3mf.cpp` and the ~4k-line
   legacy importer, which is what every PrusaSlicer 2.x, Bambu and Orca 3MF goes through.
3. **G-code / binary G-code**: `libpgcode` processor and `GCodeReader` for the G-code viewer,
   and the bgcode metadata reader in `Config.cpp`. libbgcode itself is shared with the Buddy
   firmware, so its block/heatshrink/MeatPack decoders are better fuzzed upstream in
   libbgcode, where both projects benefit.
4. **Mesh formats**: OBJ (`objparser.cpp`, hand-rolled C), STL (`admesh`), SVG (NanoSVG; known
   crash-prone upstream).
5. **Config / presets**: `ConfigBase::set_deserialize` per option type, `PlaceholderParser`
   (custom G-code templates are carried inside 3MFs), preset-bundle INI and the YAML adapters.
6. **Network responses** (PrusaLink, OctoPrint, Moonraker, Connect webview messages): parsed
   with nlohmann/boost, so memory-safety yield is low; fuzz only after the above.

[libFuzzer]: https://llvm.org/docs/LibFuzzer.html
