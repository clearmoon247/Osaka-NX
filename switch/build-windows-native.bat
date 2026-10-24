@echo off
rem build-windows-native.bat - EXPERIMENTAL: build osaka-nx.nro on Windows without WSL.
rem
rem This only starts build-windows-native.ps1 (same folder) with script permission for this one run, so you do not have to
rem change PowerShell's settings. What it installs and why is described at the top of that file. Extra options are passed on,
rem for example:  build-windows-native.bat -Rom "C:\Games\stadium.z64" -CheckOnly
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows-native.ps1" %*
echo.
pause
