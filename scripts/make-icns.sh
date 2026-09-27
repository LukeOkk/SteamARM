#!/bin/bash
# Builds AppIcon.icns from the generated 1024x1024 PNG.
set -euo pipefail

src="${1:?usage: make-icns.sh <png> <out.icns>}"
out="${2:?usage: make-icns.sh <png> <out.icns>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
set="$work/AppIcon.iconset"
mkdir -p "$set"

for size in 16 32 128 256 512; do
    sips -z $size $size "$src" --out "$set/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z $double $double "$src" --out "$set/icon_${size}x${size}@2x.png" >/dev/null
done

iconutil -c icns "$set" -o "$out"
echo "wrote $out"
