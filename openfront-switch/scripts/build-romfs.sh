#!/usr/bin/env bash
# Builds romfs/: the bundled engine (engine.js) plus the data from OpenFront.
# Pass map folder names to include only those maps (smaller test builds).
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf romfs
mkdir -p romfs
node scripts/build-bundle.mjs deps/openfront romfs/engine.js
node scripts/prepare-romfs.mjs deps/openfront romfs "$@"
node scripts/prepare-assets.mjs deps/openfront deps/inter romfs
du -sh romfs
