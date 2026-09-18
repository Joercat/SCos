#!/bin/sh
# Fetch the v86 emulator runtime (browser + node builds) and SeaBIOS/VGA BIOS
# into .preview/vendor/ so that tests/harness.mjs and the live preview server
# work without anything outside the repository.
#
# Usage: tools/setup_preview.sh
set -e
cd "$(dirname "$0")/.."
V=.preview/vendor
if [ -f "$V/v86.wasm" ] && [ -f "$V/seabios.bin" ] && [ -f "$V/vgabios.bin" ]; then
    echo "preview vendor assets already present in $V"
    exit 0
fi
mkdir -p "$V"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

echo "fetching v86 npm package..."
npm pack v86 --pack-destination "$W" >/dev/null 2>&1
tar -xzf "$W"/v86-*.tgz -C "$W"
cp "$W"/package/build/libv86.js "$V"/
cp "$W"/package/build/libv86.mjs "$V"/ 2>/dev/null || true
cp "$W"/package/build/v86.wasm "$V"/

echo "fetching v86 sources (for SeaBIOS/VGA BIOS)..."
curl -sL https://codeload.github.com/copy/v86/tar.gz/refs/heads/master -o "$W/v86src.tgz"
tar -xzf "$W/v86src.tgz" -C "$W"
B=$(echo "$W"/v86-*/bios)
cp "$B/seabios.bin" "$V"/
cp "$B/vgabios.bin" "$V"/

echo "ok: $(ls "$V")"
