@echo off
set "SHAREDVD_PRESET_SCRIPT=%~dp0Enable-ResolutionPresets.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { Start-Process -FilePath ($env:SystemRoot + '\System32\WindowsPowerShell\v1.0\powershell.exe') -Verb RunAs -ArgumentList ('-NoProfile -NoExit -ExecutionPolicy Bypass -File ' + [char]34 + $env:SHAREDVD_PRESET_SCRIPT + [char]34) -ErrorAction Stop } catch { Write-Error $_; exit 1 }"
if errorlevel 1 pause
