@echo off
rem Crea el NSP de OpenFront con hacBrewPack y las claves de TU consola.
rem Necesita hacbrewpack.exe (https://github.com/The-4n/hacBrewPack) y prod.keys
rem (sacado de tu propia Switch con Lockpick_RCM) en esta carpeta.
cd /d "%~dp0"
if not exist hacbrewpack.exe (echo Copia hacbrewpack.exe en esta carpeta. & pause & exit /b 1)
set KEYS=prod.keys
if exist keys.dat set KEYS=keys.dat
if not exist %KEYS% (echo Copia tu prod.keys, sacado de tu Switch con Lockpick_RCM, en esta carpeta. & pause & exit /b 1)
hacbrewpack.exe -k %KEYS% --titleid @TITLEID@ --nologo --exefsdir exefs --romfsdir romfs --controldir control --nspdir nsp || (pause & exit /b 1)
echo.
echo Listo: el NSP esta en la carpeta nsp
pause
