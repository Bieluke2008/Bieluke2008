#!/usr/bin/env bash
# Servidor de OpenFront para jugar en red local con la Switch y con navegadores
# (Linux y macOS). Necesita git y Node.js 24. La version de OpenFront es la
# misma que lleva el port: si no coincide, las partidas se desincronizan.
set -euo pipefail
cd "$(dirname "$0")"
COMMIT=2e779e235b24fe5d531996efb28254e8e7b360ca
command -v git >/dev/null || { echo "Falta git."; exit 1; }
command -v node >/dev/null || { echo "Falta Node.js 24 (https://nodejs.org)."; exit 1; }
node -e "process.exit(+process.versions.node.split('.')[0] === 24 ? 0 : 1)" ||
  { echo "OpenFront necesita Node.js 24 (tienes $(node -v)): instala la version 24 LTS de https://nodejs.org."; exit 1; }
[ -d OpenFrontIO/.git ] || git clone https://github.com/openfrontio/OpenFrontIO.git OpenFrontIO
cd OpenFrontIO
git fetch -q origin
git checkout -q "$COMMIT"
if [ ! -f "node_modules/.ok-$COMMIT" ]; then
  echo "Instalando dependencias, tarda unos minutos la primera vez..."
  # OpenFront pide npm 12 y npm ci --ignore-scripts (su README); npx lo usa sin instalarlo.
  npx -y npm@12.1.0 ci --ignore-scripts --no-fund --no-audit
  touch "node_modules/.ok-$COMMIT"
fi
ip=$( (hostname -I 2>/dev/null || ipconfig getifaddr en0 2>/dev/null || echo "IP-de-este-PC") | awk '{print $1}')
echo
echo "================================================================"
echo " En la Switch escribe como servidor: $ip:9000"
echo " Tus amigos juegan en el navegador en http://$ip:9000"
echo " Para parar el servidor: Ctrl+C"
echo "================================================================"
echo
SKIP_BROWSER_OPEN=true exec npm run dev:host
