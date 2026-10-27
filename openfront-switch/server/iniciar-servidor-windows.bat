@echo off
rem Servidor de OpenFront para jugar en red local con la Switch y con navegadores.
rem Necesita Git (https://git-scm.com) y Node.js 24 (https://nodejs.org).
rem La version de OpenFront es la misma que lleva el port: si no coincide, las
rem partidas se desincronizan.
setlocal
cd /d "%~dp0"
set COMMIT=2e779e235b24fe5d531996efb28254e8e7b360ca
where git >nul 2>nul || (echo Falta Git: instalalo desde https://git-scm.com y vuelve a abrir este archivo. & pause & exit /b 1)
where node >nul 2>nul || (echo Falta Node.js 24: instalalo desde https://nodejs.org y vuelve a abrir este archivo. & pause & exit /b 1)
node -e "process.exit(+process.versions.node.split('.')[0] === 24 ? 0 : 1)" || (echo OpenFront necesita Node.js 24: instala la version 24 LTS de https://nodejs.org. & pause & exit /b 1)
if not exist OpenFrontIO\.git (
  echo Descargando OpenFront...
  git clone https://github.com/openfrontio/OpenFrontIO.git OpenFrontIO || (pause & exit /b 1)
)
cd OpenFrontIO
git fetch -q origin
git checkout -q %COMMIT% || (echo No se pudo usar la version %COMMIT%. & pause & exit /b 1)
if not exist node_modules\.ok-%COMMIT% (
  echo Instalando dependencias, tarda unos minutos la primera vez...
  rem OpenFront pide npm 12 y npm ci --ignore-scripts (su README); npx lo usa sin instalarlo.
  call npx -y npm@12.1.0 ci --ignore-scripts --no-fund --no-audit || (pause & exit /b 1)
  echo ok> node_modules\.ok-%COMMIT%
)
echo.
echo ================================================================
echo  En la Switch escribe como servidor la IP de este PC y :9000
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do echo    %%a:9000
echo  Tus amigos juegan en el navegador en http://IP:9000
echo  Si Windows pregunta por el firewall, permite el acceso en redes privadas.
echo  Para parar el servidor cierra esta ventana.
echo ================================================================
echo.
set SKIP_BROWSER_OPEN=true
call npm run dev:host
pause
