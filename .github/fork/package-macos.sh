#!/usr/bin/env bash
# Packages build/dist (from build.sh) for macOS: PrusaSlicer.app in a dmg, in build/package.
# The CI workflow and a local build on a Mac run this same script.
#
# Layout (src/slic3r-shared/src/Slic3r/App/Init.cpp): the binary is Contents/MacOS/<CFBundleExecutable>, and it finds
# its resources at Contents/Resources. On macOS, the OCCT wrapper for STEP is static (src/occt_wrapper/CMakeLists.txt).
# The app has an ad-hoc signature only: no Apple Developer ID. The first start needs a right-click and Open, or
# "xattr -dr com.apple.quarantine PrusaSlicer.app".
set -euo pipefail

cd "$(dirname "$0")/../.."
dist=build/dist
out=build/package
version=$(sed -n 's/^set(SLIC3R_VERSION "\(.*\)")/\1/p' version.inc)
suffix=$(cat "$dist/FORK_SUFFIX")
arch=$(uname -m)
name="PrusaSlicer-${version}+${suffix}-macos-${arch}"

test -x "$dist/bin/slic3r-app-launcher" || { echo "missing $dist/bin/slic3r-app-launcher (run build.sh first)" >&2; exit 1; }
test -f build/src/Info.plist || { echo "missing build/src/Info.plist (CMake makes it from Info.plist.in)" >&2; exit 1; }

rm -rf "$out"
app="$out/dmg/PrusaSlicer.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
# CFBundleExecutable is SLIC3R_APP_KEY ("PrusaSlicer").
cp -a "$dist/bin/slic3r-app-launcher" "$app/Contents/MacOS/PrusaSlicer"
cp -a "$dist/resources/." "$app/Contents/Resources/"
cp resources/icons/PrusaSlicer.icns "$app/Contents/Resources/PrusaSlicer.icns"
cp build/src/Info.plist "$app/Contents/Info.plist"
printf 'APPL????' > "$app/Contents/PkgInfo"
strip -S "$app/Contents/MacOS/PrusaSlicer"

# Only system libraries and frameworks: the dependencies are static.
if otool -L "$app/Contents/MacOS/PrusaSlicer" | tail -n +2 | grep -v -E '^\s+(/usr/lib/|/System/Library/)'; then
    echo "the binary links a library that is not on each Mac" >&2
    exit 1
fi

# Apple silicon runs only signed code. An ad-hoc signature seals the bundle.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

cp .github/fork/README-binary.md "$out/dmg/README.md"
ln -s /Applications "$out/dmg/Applications"
hdiutil create -volname "PrusaSlicer $version+$suffix" -srcfolder "$out/dmg" -ov -format UDZO "$out/$name.dmg"
rm -rf "$out/dmg"
(cd "$out" && shasum -a 256 ./*.dmg > "SHA256SUMS-macos-$arch.txt")
ls -la "$out"
