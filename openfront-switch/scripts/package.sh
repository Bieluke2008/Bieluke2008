#!/usr/bin/env bash
# Packs openfront.nro for the microSD: dist/openfront-switch-v<VERSION>.zip
# holding switch/openfront.nro (romfs is inside the .nro) and LEEME.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(cat VERSION)
[ -f openfront.nro ] || { echo "openfront.nro missing: run scripts/build-switch.sh" >&2; exit 1; }
tmp=$(mktemp -d)
mkdir -p "$tmp/switch" dist
cp openfront.nro "$tmp/switch/openfront.nro"
cat > "$tmp/LEEME.txt" <<TXT
OpenFront para Nintendo Switch - v${version}
Port homebrew no oficial de OpenFront (https://github.com/openfrontio/OpenFrontIO)

Instalar (Switch con Atmosphere y Homebrew Menu):
1. Copia la carpeta "switch" de este zip a la raiz de la microSD
   (queda /switch/openfront.nro). No hace falta nada mas: los mapas,
   idiomas, iconos y fuentes van dentro del .nro.
2. Abre el Homebrew Menu manteniendo R al abrir un juego (no desde el
   album: ahi la app tiene mucha menos memoria).
3. Elige OpenFront.

Controles: en la partida, Menu (+) > Ajustes muestra la lista completa.
Tactil: tocar = clic, mantener = menu radial, arrastrar = mover, pellizcar = zoom.

Notas para las pruebas: esta version no se ha probado aun en una Switch
real. Si algo falla, anota el mapa, el numero de tribus y que estabas
haciendo. Para que vaya mas fluido: mapas pequenos y menos tribus.
TXT
out="dist/openfront-switch-v${version}.zip"
rm -f "$out"
(cd "$tmp" && zip -q -r "$OLDPWD/$out" switch LEEME.txt)
rm -rf "$tmp"
echo "$out"
