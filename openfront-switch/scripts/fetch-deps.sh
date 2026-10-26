#!/usr/bin/env bash
# Fetches the pinned sources the port builds from:
#   deps/openfront  OpenFront (engine, maps, translations, fonts)
#   deps/quickjs    QuickJS-ng (the JavaScript engine the game runs on)
#   deps/cjson      cJSON (UI state from the engine thread to the renderer)
#   deps/inter      Inter, the font the web client's UI renders with
#   deps/dejavu     DejaVu Sans Mono (the web UI's font-mono, as Chromium on Linux shows it)
# and installs the npm dependencies: OpenFront's own (esbuild, zod, colord)
# and tools/ (resvg, to rasterize OpenFront's SVG icons and flags).
set -euo pipefail
cd "$(dirname "$0")/.."

# Bump these together with a test on hardware: the adapter uses engine
# internals (GameRunner, GameMapImpl arrays) that can change upstream.
OPENFRONT_REPO=https://github.com/openfrontio/OpenFrontIO.git
OPENFRONT_COMMIT=2e779e235b24fe5d531996efb28254e8e7b360ca
QUICKJS_REPO=https://github.com/quickjs-ng/quickjs.git
QUICKJS_TAG=v0.16.2
CJSON_REPO=https://github.com/DaveGamble/cJSON.git
CJSON_TAG=v1.7.18
INTER_URL=https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
DEJAVU_URL=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip

mkdir -p deps

if [ ! -d deps/openfront/.git ]; then
  git init -q deps/openfront
  git -C deps/openfront remote add origin "$OPENFRONT_REPO"
fi
if [ "$(git -C deps/openfront rev-parse HEAD 2>/dev/null || true)" != "$OPENFRONT_COMMIT" ]; then
  git -C deps/openfront fetch -q --depth 1 origin "$OPENFRONT_COMMIT"
  git -C deps/openfront checkout -q FETCH_HEAD
fi
if [ ! -d deps/openfront/node_modules/esbuild ]; then
  (cd deps/openfront && npm ci --ignore-scripts --engine-strict=false --no-audit --no-fund)
fi

if [ ! -d deps/quickjs ]; then
  git clone -q --depth 1 --branch "$QUICKJS_TAG" "$QUICKJS_REPO" deps/quickjs
fi

if [ ! -d deps/cjson ]; then
  git clone -q --depth 1 --branch "$CJSON_TAG" "$CJSON_REPO" deps/cjson
fi

if [ ! -f deps/inter/Inter-Bold.ttf ]; then
  mkdir -p deps/inter
  tmp=$(mktemp -d)
  curl -sSL -o "$tmp/inter.zip" "$INTER_URL"
  for w in Regular Medium SemiBold Bold ExtraBold; do
    unzip -o -j -q "$tmp/inter.zip" "extras/ttf/Inter-$w.ttf" -d deps/inter
  done
  unzip -o -j -q "$tmp/inter.zip" LICENSE.txt -d deps/inter
  rm -rf "$tmp"
fi

if [ ! -f deps/dejavu/DejaVuSansMono-Bold.ttf ]; then
  mkdir -p deps/dejavu
  tmp=$(mktemp -d)
  curl -sSL -o "$tmp/dejavu.zip" "$DEJAVU_URL"
  for f in DejaVuSansMono DejaVuSansMono-Bold; do
    unzip -o -j -q "$tmp/dejavu.zip" "dejavu-fonts-ttf-2.37/ttf/$f.ttf" -d deps/dejavu
  done
  unzip -o -j -q "$tmp/dejavu.zip" dejavu-fonts-ttf-2.37/LICENSE -d deps/dejavu
  rm -rf "$tmp"
fi

if [ ! -d tools/node_modules/@resvg ]; then
  (cd tools && npm install --no-audit --no-fund)
fi

echo "deps ready: openfront@${OPENFRONT_COMMIT:0:10}, quickjs-ng ${QUICKJS_TAG}"
