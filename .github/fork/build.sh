#!/usr/bin/env bash
# Builds PrusaSlicer with the steps of doc/Build.md, then installs it into build/dist.
# The CI workflow (.github/workflows/fork-build.yml) and a local build run this same script.
#
# Usage: .github/fork/build.sh deps|app|all
#   deps  doc/Build.md step 1 and 1.1: cmake -S deps -B deps/build, then cmake --build deps/build (two times).
#   app   doc/Build.md step 2: cmake -S . -B build -DCMAKE_PREFIX_PATH=..., cmake --build build, then cmake --install.
#   all   deps, then app.
#
# Only these settings are added to doc/Build.md:
#   CMAKE_BUILD_TYPE=Release, SLIC3R_BUILD_TESTS=OFF and SLIC3R_RELEASE_DEBUG_SYMBOLS=OFF (a release build),
#   CMAKE_INSTALL_PREFIX=build/dist, SLIC3R_FORK_SUFFIX (the version suffix, see version.inc),
#   on macOS the deployment target 13.3 and -Xclang -fno-pch-timestamp (for ccache), and with clang the ThinLTO cache
#   of the linker (and lld on Linux).
# The compiler comes from the environment (CC, CXX), as for a local user. CI uses the latest LLVM (clang-23 on Linux).
#
# Environment:
#   JOBS          parallel jobs (default: all CPUs)
#   DEPS_PREFIX   the CMAKE_PREFIX_PATH of the dependencies. Default: deps/build/destdir/usr/local (doc/Build.md).
#                 A ';' list is allowed, for prebuilt dependencies (for example the devspace cache).
#   FORK_SUFFIX   the version suffix (default: from the git tag v<version>-<suffix>, else "anya-<commit>")
#   CMAKE_ARGS    more arguments for the configure of the app (optional)
# A compiler launcher (ccache) comes from the environment: CMAKE_C_COMPILER_LAUNCHER, CMAKE_CXX_COMPILER_LAUNCHER.
set -euo pipefail

cd "$(dirname "$0")/../.."
step=${1:-all}
if [ -z "${JOBS:-}" ]; then
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)
fi
# doc/Build.md, "Troubleshooting": the extraction of Boost can fail without a UTF-8 locale.
export LC_ALL=C.UTF-8 LANG=C.UTF-8
# macOS: z3 5.3.3 (deps/+z3) uses std::format with floating-point values. libc++ has std::to_chars for floating point
# from macOS 13.3. Upstream uses 11.0 (cmake/modules/OSXDeploymentTarget.cmake), and then the deps build fails.
# The dependencies and the app must use the same target.
if [ "$(uname -s)" = Darwin ] && [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    export MACOSX_DEPLOYMENT_TARGET=13.3
fi

fork_suffix() {
    if [ -n "${FORK_SUFFIX:-}" ]; then
        echo "$FORK_SUFFIX"
        return
    fi
    local tag
    tag=$(git describe --tags --exact-match 2>/dev/null || true)
    case "$tag" in
        v*-anya.*) echo "${tag##*-}" ;;
        *) echo "anya-$(git rev-parse --short=10 HEAD)" ;;
    esac
}

build_deps() {
    echo "== doc/Build.md step 1: dependencies"
    cmake -S deps -B deps/build
    cmake --build deps/build -j "$JOBS"
    echo "== doc/Build.md step 1.1: a second build must do nothing"
    cmake --build deps/build -j "$JOBS"
}

build_app() {
    local prefix=${DEPS_PREFIX:-$PWD/deps/build/destdir/usr/local}
    local suffix
    suffix=$(fork_suffix)
    echo "== doc/Build.md step 2: PrusaSlicer (suffix $suffix, prefix $prefix)"
    # The packages strip the binary, so the release build needs no debug symbols (SLIC3R_RELEASE_DEBUG_SYMBOLS adds
    # -g on macOS). With clang, a precompiled header has a time stamp, and then ccache misses each file.
    local flags=()
    if [ "$(uname -s)" = Darwin ]; then
        flags+=(-DCMAKE_CXX_FLAGS="-Xclang -fno-pch-timestamp" -DCMAKE_C_FLAGS="-Xclang -fno-pch-timestamp")
    fi
    # Release uses LTO (CMakeLists.txt, CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE). With clang, CMake uses ThinLTO, and
    # the linker can keep a cache of the ThinLTO code generation: a small change then links much faster. ld64 (macOS)
    # and lld (Linux) have such a cache. LTO_CACHE_DIR sets its place (default build/lto-cache, CI caches it).
    local lto_cache=${LTO_CACHE_DIR:-$PWD/build/lto-cache}
    local cxx=${CXX:-c++}
    if [ "$(uname -s)" = Darwin ]; then
        flags+=(-DCMAKE_EXE_LINKER_FLAGS="-Wl,-cache_path_lto,$lto_cache"
                -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-cache_path_lto,$lto_cache")
    elif "$cxx" --version 2>/dev/null | grep -q clang; then
        flags+=(-DCMAKE_LINKER_TYPE=LLD
                -DCMAKE_EXE_LINKER_FLAGS="-Wl,--thinlto-cache-dir=$lto_cache"
                -DCMAKE_MODULE_LINKER_FLAGS="-Wl,--thinlto-cache-dir=$lto_cache"
                -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--thinlto-cache-dir=$lto_cache")
    fi
    # shellcheck disable=SC2086
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_BUILD_TYPE=Release -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_RELEASE_DEBUG_SYMBOLS=OFF \
        -DCMAKE_INSTALL_PREFIX="$PWD/build/dist" -DSLIC3R_FORK_SUFFIX="$suffix" "${flags[@]}" ${CMAKE_ARGS:-}
    cmake --build build -j "$JOBS"
    rm -rf build/dist
    cmake --install build
    echo "$suffix" > build/dist/FORK_SUFFIX
}

case "$step" in
    deps) build_deps ;;
    app) build_app ;;
    all) build_deps; build_app ;;
    *) echo "usage: $0 deps|app|all" >&2; exit 2 ;;
esac
