@echo off
rem build-windows.bat - double-click to build osaka-nx.nro on Windows.
rem
rem This only starts build-windows.ps1 (same folder) with script permission for this one run, so you do not have to
rem change PowerShell's settings. Everything it does is described at the top of that file. Extra options are passed on,
rem for example:  build-windows.bat -Rom "C:\Games\stadium.z64" -Fast
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*
echo.
pause
