#!/usr/bin/env bash
# Packs openfront.nro for the microSD: dist/openfront-switch-v<VERSION>.zip
# holding switch/openfront.nro (romfs is inside the .nro), the LAN server
# scripts (servidor-pc/) and LEEME.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(cat VERSION)
[ -f openfront.nro ] || { echo "openfront.nro missing: run scripts/build-switch.sh" >&2; exit 1; }
tmp=$(mktemp -d)
mkdir -p "$tmp/switch" "$tmp/servidor-pc" dist
cp openfront.nro "$tmp/switch/openfront.nro"
cp server/iniciar-servidor-windows.bat server/iniciar-servidor.sh "$tmp/servidor-pc/"
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

Multijugador entre Switch (sin PC):
Todas las Switch en la misma red Wi-Fi (vale la de un movil compartiendo
datos) y con la misma version del port. Una entra en Multijugador y pulsa
"Crear partida en esta Switch"; las demas la ven en "Partidas en tu red" y
la tocan para entrar. Si no aparece, usa "Unirse" con la IP que muestra la
anfitriona (IP:47801) y el codigo de la sala.

Inalambrico local (sin router): en Multijugador elige "Inalambrico local"
(o L/R). Funciona entre Switch con el NSP instalado (ver el kit
openfront-nsp-kit); con este .nro, solo si todas abren el Homebrew Menu
desde el mismo juego.

Multijugador con navegadores (servidor en un PC):
1. En un PC de tu red instala Git (https://git-scm.com) y Node.js 24
   (https://nodejs.org).
2. Copia la carpeta "servidor-pc" de este zip al PC y abre
   iniciar-servidor-windows.bat (Windows) o iniciar-servidor.sh (Linux/Mac).
   La primera vez descarga OpenFront en la version que usa el port e
   instala sus dependencias (unos minutos). Si Windows pregunta por el
   firewall, permite el acceso en redes privadas.
3. La ventana muestra la IP del PC. En la Switch: Multijugador, escribe
   IP:9000 (por ejemplo 192.168.1.20:9000) y "Crear lobby" o "Unirse al
   lobby" con el codigo de la sala.
4. Tus amigos abren http://IP:9000 en el navegador (PC, movil...) y entran
   con "Unirse al lobby" y el codigo. Si crean ellos la sala en la web,
   deben activar "Mapa compacto": la Switch solo tiene esos mapas.
Todos tienen que estar en la misma red (o en una VPN tipo Tailscale).

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
(cd "$tmp" && zip -q -r "$OLDPWD/$out" switch servidor-pc LEEME.txt)
rm -rf "$tmp"
echo "$out"
