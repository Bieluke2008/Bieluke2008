#!/usr/bin/env bash
# Packs the installable NSP's ingredients: dist/openfront-nsp-kit-v<VERSION>.zip
# with exefs/ and control.nacp (make nsp-exefs), the icon, romfs/ and the
# scripts that turn them into an NSP with hacBrewPack and the player's own
# console keys (prod.keys), which nobody else may distribute.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(cat VERSION)
title_id=$(python3 -c "import json; print(json.load(open('nsp/openfront.json'))['program_id'][2:])")
[ -f nsp-exefs/main ] && [ -f nsp-exefs/main.npdm ] && [ -f nsp-exefs/control.nacp ] || { echo "run: make nsp-exefs" >&2; exit 1; }
tmp=$(mktemp -d)
kit="$tmp/openfront-nsp-kit"
mkdir -p "$kit/exefs" "$kit/control" dist
cp nsp-exefs/main nsp-exefs/main.npdm "$kit/exefs/"
cp nsp-exefs/control.nacp "$kit/control/control.nacp"
# A 256x256 JPEG without EXIF (hacBrewPack's advice: EXIF can break the icon).
python3 -c "from PIL import Image; Image.open('icon.jpg').convert('RGB').resize((256, 256)).save('$kit/control/icon_AmericanEnglish.dat', 'JPEG', quality=92)"
cp -r romfs "$kit/romfs"
sed "s/@TITLEID@/$title_id/g" nsp/crear-nsp-windows.bat > "$kit/crear-nsp-windows.bat"
sed "s/@TITLEID@/$title_id/g" nsp/crear-nsp.sh > "$kit/crear-nsp.sh"
chmod +x "$kit/crear-nsp.sh"
sed "s/@VERSION@/$version/g; s/@TITLEID@/$title_id/g" nsp/LEEME-NSP.txt > "$kit/LEEME-NSP.txt"
out="dist/openfront-nsp-kit-v${version}.zip"
rm -f "$out"
(cd "$tmp" && zip -q -r "$OLDPWD/$out" openfront-nsp-kit)
rm -rf "$tmp"
echo "$out"
