@echo off
rem Voldorium's first start (m1/module.c runs this, in the builder folder next to Voldorium.exe):
rem builds the game module sc_game.dll from the player's own disc dump, into the Voldorium
rem folder. Usage: first_run.bat <disc.gdi> <Voldorium folder>. Everything it downloads (the
rem compiler, two Flycast files and, if there is no Python, a portable Python) goes into
rem toolchain\ here, at pinned versions checked by SHA-256. Nothing is installed on the system.
setlocal EnableExtensions
cd /d "%~dp0"
title Voldorium: building the game from your disc
echo Voldorium: building the game from your disc
echo ============================================
echo The game's code is translated from YOUR OWN disc dump, on this PC. Nothing of the game is
echo downloaded. This takes a few minutes, once.
echo.
if "%~2"=="" (
  echo usage: first_run.bat disc.gdi VoldoriumFolder
  goto failed
)

rem --- Python 3.10 or newer: an installed one, else a portable one in toolchain\python. Not the
rem     Microsoft Store Python: it keeps what it writes under AppData where the compiler cannot see it
set "PY="
for %%P in (py python) do (
  if not defined PY (
    %%P -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) and 'WindowsApps' not in sys.executable else 1)" >nul 2>&1 && set "PY=%%P"
  )
)
if not defined PY if exist "toolchain\python\python.exe" set "PY=toolchain\python\python.exe"
if not defined PY (
  echo Python was not found: downloading a portable Python 3.12 from python.org, about 11 MB...
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; $u='https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip';" ^
    "$z=Join-Path $env:TEMP 'voldorium-python.zip'; Invoke-WebRequest -UseBasicParsing $u -OutFile $z;" ^
    "$h=(Get-FileHash $z -Algorithm SHA256).Hash.ToLower(); $want='4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3';" ^
    "if ($h -ne $want) { Remove-Item $z; throw ('python download does not match its SHA-256: ' + $h) };" ^
    "Expand-Archive $z -DestinationPath 'toolchain\python' -Force; Remove-Item $z"
  if errorlevel 1 goto failed
  set "PY=toolchain\python\python.exe"
)

"%PY%" -I tools\build.py --download --only module --gdi "%~1" --module-dir "%~2" --out "%~dp0work"
if errorlevel 1 goto failed
echo.
echo Finished: Voldorium starts now.
exit /b 0

:failed
echo.
echo The game was not built. The message above says why; README.txt's Troubleshooting section
echo lists the usual causes. Start Voldorium again to retry: finished steps are not redone.
echo.
pause
exit /b 1
