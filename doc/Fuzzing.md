# Fuzz testing

PrusaSlicer opens files from the internet: Printables / MakerWorld 3MFs (via the
`prusaslicer://` download handler) and STL and OBJ models. The loaders of these formats have
[libFuzzer] fuzz targets that live next to the code they test, in each module's `fuzz/`
directory (mirroring `test/`):

| Target | Input |
| --- | --- |
| `fuzz-load_3mf` | A 3MF project: the current loader, then the legacy loader, then the paint-on queries |
| `fuzz-facets_deserialize` | Paint-on data that passes the checks of the 3MF loaders, into `TriangleSelector` |
| `fuzz-stl` | An STL file through `Biz::load_stl()` |
| `fuzz-obj` | An OBJ file through `Biz::load_obj()` |

All four are in `src/slic3r-shared/fuzz/`.

## Layout

```
cmake/modules/Fuzzing.cmake               SLIC3R_FUZZ option, slic3r_add_fuzzer()
src/slic3r-test-utils/fuzz/               replay main for non-libFuzzer builds
src/<module>/fuzz/<Name>Fuzzer.cpp        harness: defines LLVMFuzzerTestOneInput
src/<module>/fuzz/<name>.dict             libFuzzer dictionary (tokens of the format)
src/<module>/fuzz/corpus/<name>/          seeds; replayed by ctest, must never crash
src/<module>/fuzz/crashers/<name>/        local only (git-ignored): reproducers of open bugs;
                                          move them into corpus/ once fixed
```

## Two build flavours, same harness

| | `SLIC3R_FUZZ=ON` | default (`SLIC3R_FUZZ=OFF`, `SLIC3R_BUILD_TESTS=ON`) |
| --- | --- | --- |
| Compiler | Clang only (on macOS: Homebrew LLVM; AppleClang has no libFuzzer) | any |
| Instrumentation | `-fsanitize=fuzzer-no-link` on all project code; libc++/libstdc++ hardening | none beyond `SLIC3R_ASAN`/`SLIC3R_UBSAN` if set |
| Harness links | libFuzzer, or `$LIB_FUZZING_ENGINE` (OSS-Fuzz / ClusterFuzzLite) | `slic3r-fuzz-replay` |
| ctest | `fuzz-<name> -runs=0 corpus/` | `fuzz-<name> corpus/` |

The default flavour keeps every harness compiling on every platform and turns the corpus into
a regression suite (`ctest -L fuzz`), which is the part that actually prevents re-introducing
a fixed bug.

## Running a fuzzer locally

Linux:

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DSLIC3R_FUZZ=ON -DSLIC3R_ASAN=ON -DSLIC3R_UBSAN=ON \
  -DCMAKE_PREFIX_PATH=$PWD/deps/build/destdir/usr/local
cmake --build build-fuzz --target fuzz-load_3mf

F=src/slic3r-shared/fuzz
B=build-fuzz/src/slic3r-shared
mkdir -p /tmp/fz-work && $B/fuzz-load_3mf /tmp/fz-work $F/corpus/load_3mf \
  -dict=$B/load_3mf.dict -max_len=262144 -max_total_time=600
```

The other targets work the same way. `fuzz-stl` and `fuzz-obj` have the dictionaries
`stl.dict` and `obj.dict`. `fuzz-facets_deserialize` has no dictionary.

macOS: use Homebrew LLVM, and keep it on the libc++ headers of the SDK, which the deps were
built against:

```sh
LLVM=$(brew --prefix llvm)
SDK=$(xcrun --show-sdk-path)
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=$LLVM/bin/clang -DCMAKE_CXX_COMPILER=$LLVM/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_CXX_FLAGS="-stdlib++-isystem $SDK/usr/include/c++/v1" \
  -DSLIC3R_FUZZ=ON -DSLIC3R_ASAN=ON -DSLIC3R_UBSAN=ON -DSLIC3R_PCH=OFF \
  -DCMAKE_PREFIX_PATH=$PWD/deps/build/destdir/usr/local
```

Homebrew's own newer libc++ drops transitive includes PrusaSlicer still relies on. Use
`-stdlib++-isystem` (not `-isystem`, which lands after the C headers and breaks `<cmath>`),
without `-nostdinc++` (which disables it), and set the sysroot to the same SDK.

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
- When an outer layer validates input that an inner layer then trusts, add a second harness
  that applies the same checks and gives the rest to the inner layer
  (`FacetsDeserializeFuzzer` behind the 3MF loaders), so the fuzzer is not stuck on the first
  check. Use the validator of the outer layer, so the two cannot drift apart.
- No global state between runs, no network, no GUI. Temp files are acceptable for loaders that
  only take a path; reuse one path per process.

[libFuzzer]: https://llvm.org/docs/LibFuzzer.html
