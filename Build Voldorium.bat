@echo off
rem Builds Voldorium from your own SoulCalibur (USA) disc dump. Double-click this file, or drop
rem your .gdi file onto it. Everything it downloads (the compiler, SDL2, two Flycast files and, if
rem you have no Python, a portable Python) goes into the toolchain folder here, at pinned versions
rem checked by SHA-256. Nothing is installed on the system. The result is the Voldorium folder.
setlocal EnableExtensions
cd /d "%~dp0"
title Voldorium builder
echo Voldorium builder
echo =================
echo This builds Voldorium from YOUR OWN legal disc dump of SoulCalibur (USA, T-1401N).
echo No game files are downloaded; the game's code is translated from your dump, on this PC.
echo.

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

rem --- the disc dump: dropped on this file, or chosen in a file picker ---
set "GDI=%~1"
if not defined GDI (
  echo Choose your disc dump: the .gdi file, with its track files next to it.
  for /f "usebackq delims=" %%F in (`powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'Your SoulCalibur (USA) disc dump'; $d.Filter = 'Disc dump (*.gdi)|*.gdi'; if ($d.ShowDialog() -eq 'OK') { $d.FileName }"`) do set "GDI=%%F"
)
if not defined GDI (
  echo No disc dump chosen. Nothing was built.
  goto end
)
echo Disc dump: %GDI%
echo.

rem build files in work\ here, not under AppData
"%PY%" -I tools\build.py --download --gdi "%GDI%" --player-dir "%~dp0Voldorium" --out "%~dp0work"
if errorlevel 1 goto failed
echo.
echo Finished. Start Voldorium.exe in the Voldorium folder (it opens now).
start "" explorer "%~dp0Voldorium"
goto end

:failed
echo.
echo The build did not finish. The message above says why; the README's Troubleshooting section
echo lists the usual causes. You can run this again: finished steps are not redone.

:end
echo.
pause
