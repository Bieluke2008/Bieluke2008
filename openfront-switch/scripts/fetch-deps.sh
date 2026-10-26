#!/usr/bin/env bash
# Fetches the pinned sources the port builds from:
#   deps/openfront  OpenFront (engine, maps, translations, fonts)
#   deps/quickjs    QuickJS-ng (the JavaScript engine the game runs on)
# and installs OpenFront's npm dependencies (esbuild and zod are what we use).
set -euo pipefail
cd "$(dirname "$0")/.."

# Bump these together with a test on hardware: the adapter uses engine
# internals (GameRunner, GameMapImpl arrays) that can change upstream.
OPENFRONT_REPO=https://github.com/openfrontio/OpenFrontIO.git
OPENFRONT_COMMIT=2e779e235b24fe5d531996efb28254e8e7b360ca
QUICKJS_REPO=https://github.com/quickjs-ng/quickjs.git
QUICKJS_TAG=v0.16.2

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

echo "deps ready: openfront@${OPENFRONT_COMMIT:0:10}, quickjs-ng ${QUICKJS_TAG}"
