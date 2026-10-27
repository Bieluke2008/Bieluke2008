#!/usr/bin/env bash
# Builds OpenFront's NSP with hacBrewPack and YOUR console's keys (Linux/macOS).
# Needs ./hacbrewpack (https://github.com/The-4n/hacBrewPack) and ./prod.keys
# (dumped from your own Switch with Lockpick_RCM).
set -euo pipefail
cd "$(dirname "$0")"
[ -x ./hacbrewpack ] || { echo "Copia el ejecutable hacbrewpack en esta carpeta."; exit 1; }
keys=prod.keys
[ -f keys.dat ] && keys=keys.dat
[ -f "$keys" ] || { echo "Copia tu prod.keys (sacado de tu Switch con Lockpick_RCM) en esta carpeta."; exit 1; }
./hacbrewpack -k "$keys" --titleid @TITLEID@ --nologo --exefsdir exefs --romfsdir romfs --controldir control --nspdir nsp
echo "Listo: el NSP esta en la carpeta nsp/"
