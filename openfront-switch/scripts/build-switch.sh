#!/usr/bin/env bash
# Builds openfront.nro inside the official devkitPro Docker image, so no local
# devkitPro install is needed. Run scripts/fetch-deps.sh and
# scripts/build-romfs.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64:latest \
  make -j"$(nproc)" "$@"
