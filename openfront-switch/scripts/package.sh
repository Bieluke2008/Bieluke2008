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

Musica (opcional): la musica de OpenFront no es libre y no va incluida.
Si la tienes, copia sus mp3 a /switch/openfront/music/ con estos nombres:
menu-theme.mp3, gameplay.mp3, gameplay-2.mp3, gameplay-3.mp3.
Los efectos de sonido y el ambiente si van incluidos.

Controles: en la partida, Menu (+) > Ajustes muestra la lista completa.
Tactil: tocar = clic, mantener = menu radial, arrastrar = mover, pellizcar = zoom.

Notas para las pruebas: si algo falla, anota el mapa, el numero de tribus
y que estabas haciendo. Para medir la velocidad, compara el reloj de la
partida (arriba) con un cronometro: a velocidad x1 deberian avanzar igual.
Si va lento: mapas pequenos y menos tribus/naciones.
TXT
out="dist/openfront-switch-v${version}.zip"
rm -f "$out"
(cd "$tmp" && zip -q -r "$OLDPWD/$out" switch LEEME.txt)
rm -rf "$tmp"
echo "$out"
