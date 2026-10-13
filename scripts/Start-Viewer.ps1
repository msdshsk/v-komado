[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$viewer = Join-Path $root 'artifacts/viewer-build/Release/lgwl_viewer.exe'
if (-not (Test-Path -LiteralPath $viewer)) { throw 'Run scripts/Build-Loopback.ps1 first.' }
if (Get-Process lgwl_viewer,SharedVD -ErrorAction SilentlyContinue) { throw 'Close the current viewer before starting another one.' }
$log = Join-Path $root ('artifacts/interactive-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-Host 'Click the image to move the cursor there. No confinement or input forwarding.'
Write-Host 'F6: return to primary screen center (global). F11: fullscreen (viewer focused).'
Write-Host 'Hover over the viewer for its title bar and display-size menu.'
Write-Host "Log: $log"
Start-Process -FilePath $viewer -ArgumentList @('--monitor-match','"VDD by MTT"','--require-refresh-hz','60','--log',('"{0}"' -f $log))
