#!/usr/bin/env bash
# Packages build/dist (from build.sh) for Linux: a tar.gz and an AppImage, in build/package.
# The CI workflow and a local build run this same script.
#
# Layout (src/slic3r-shared/src/Slic3r/App/Init.cpp): the binary finds its resources at <binary dir>/../resources.
# STEP import loads OCCTWrapper.so from the directory of the binary (Biz/Format/STEP.cpp).
#
# Environment: LINUXDEPLOY (path of linuxdeploy; default: download the continuous release into build/tools).
set -euo pipefail

cd "$(dirname "$0")/../.."
dist=build/dist
out=build/package
version=$(sed -n 's/^set(SLIC3R_VERSION "\(.*\)")/\1/p' version.inc)
suffix=$(cat "$dist/FORK_SUFFIX")
# A build without a release tag: the commit goes into the file names (not into the build ID, see build.sh).
[ "$suffix" = anya-dev ] && suffix="anya-dev-$(git rev-parse --short=10 HEAD)"
arch=$(uname -m)
case "$arch" in x86_64) tag=x64 ;; aarch64) tag=arm64 ;; *) tag=$arch ;; esac
name="PrusaSlicer-${version}+${suffix}-linux-${tag}"

test -x "$dist/bin/slic3r-app-launcher" || { echo "missing $dist/bin/slic3r-app-launcher (run build.sh first)" >&2; exit 1; }
test -f "$dist/bin/OCCTWrapper.so" || { echo "missing $dist/bin/OCCTWrapper.so: STEP import would fail" >&2; exit 1; }
test -d "$dist/resources/profiles" || test -d "$dist/resources/icons" || { echo "missing $dist/resources" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out"

# 1. tar.gz: bin/prusa-slicer, bin/OCCTWrapper.so, resources/. It uses the system libraries (GTK 3, WebKitGTK 4.1).
root="$out/$name"
mkdir -p "$root/bin"
cp -a "$dist/bin/slic3r-app-launcher" "$root/bin/prusa-slicer"
cp -a "$dist/bin/OCCTWrapper.so" "$root/bin/"
cp -a "$dist/resources" "$root/resources"
strip --strip-debug "$root/bin/prusa-slicer" "$root/bin/OCCTWrapper.so"
cp src/platform/unix/PrusaSlicer.desktop resources/icons/PrusaSlicer_128px.png "$root/"
cp .github/fork/README-binary.md "$root/README.md"
# Each library of the binaries must resolve on the build machine.
if ldd "$root/bin/prusa-slicer" "$root/bin/OCCTWrapper.so" | grep "not found"; then
    echo "unresolved libraries" >&2
    exit 1
fi
tar -C "$out" -czf "$out/$name.tar.gz" "$name"

# 2. AppImage: the same files in an AppDir. linuxdeploy adds the libraries that are not on each system.
appdir="$out/AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/128x128/apps"
cp -a "$root/bin/." "$appdir/usr/bin/"
cp -a "$root/resources" "$appdir/usr/resources"
cp src/platform/unix/PrusaSlicer.desktop "$appdir/usr/share/applications/"
cp resources/icons/PrusaSlicer_128px.png "$appdir/usr/share/icons/hicolor/128x128/apps/PrusaSlicer.png"
linuxdeploy=${LINUXDEPLOY:-}
if [ -z "$linuxdeploy" ]; then
    mkdir -p build/tools
    linuxdeploy=build/tools/linuxdeploy-$arch.AppImage
    if [ ! -x "$linuxdeploy" ]; then
        curl -fsSL -o "$linuxdeploy" "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"
        chmod +x "$linuxdeploy"
    fi
fi
linuxdeploy=$(realpath "$linuxdeploy")
# No FUSE in containers: the AppImage of linuxdeploy extracts itself.
export APPIMAGE_EXTRACT_AND_RUN=1
(cd "$out" && OUTPUT="$name.AppImage" "$linuxdeploy" --appdir AppDir \
    --desktop-file AppDir/usr/share/applications/PrusaSlicer.desktop --output appimage)
# linuxdeploy changes the RPATH of the ELF files: OCCTWrapper.so must still be there.
(cd "$out" && rm -rf squashfs-root && "./$name.AppImage" --appimage-extract usr/bin/OCCTWrapper.so >/dev/null &&
    test -f squashfs-root/usr/bin/OCCTWrapper.so && rm -rf squashfs-root) ||
    { echo "OCCTWrapper.so is not in the AppImage" >&2; exit 1; }

rm -rf "$appdir" "$root"
(cd "$out" && sha256sum ./*.tar.gz ./*.AppImage > "SHA256SUMS-linux-$tag.txt")
ls -la "$out"
