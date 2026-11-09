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
#   CMAKE_BUILD_TYPE=Release, SLIC3R_BUILD_TESTS=OFF (a release build), CMAKE_INSTALL_PREFIX=build/dist,
#   SLIC3R_FORK_SUFFIX (the version suffix, see version.inc).
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
    # shellcheck disable=SC2086
    cmake -S . -B build -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_BUILD_TYPE=Release -DSLIC3R_BUILD_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX="$PWD/build/dist" -DSLIC3R_FORK_SUFFIX="$suffix" ${CMAKE_ARGS:-}
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
