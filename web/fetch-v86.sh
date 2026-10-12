#!/usr/bin/env bash
# Downloads the v86 emulator (BSD-2-Clause) and its BIOS images (SeaBIOS and
# the Bochs VGA BIOS, LGPL) into web/v86/. Pinned versions; not kept in git.
set -euo pipefail
V86_VERSION=0.5.471
V86_COMMIT=94faf3d6b19ecabc53a4010aaa24252fa6a162e2
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/v86"
mkdir -p "$out"
tmp="$(mktemp -d)"
curl -fsSL "https://registry.npmjs.org/v86/-/v86-$V86_VERSION.tgz" | tar xz -C "$tmp"
cp "$tmp/package/build/libv86.js" "$tmp/package/build/v86.wasm" "$tmp/package/LICENSE" "$out/"
for f in seabios.bin vgabios.bin; do
  curl -fsSL -o "$out/$f" "https://raw.githubusercontent.com/copy/v86/$V86_COMMIT/bios/$f"
done
rm -rf "$tmp"
echo "v86 $V86_VERSION -> $out"
