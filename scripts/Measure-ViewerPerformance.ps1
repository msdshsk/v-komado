[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ViewerPath,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label = 'current'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$viewer = (Resolve-Path -LiteralPath $ViewerPath).Path
$benchmark = Join-Path $root 'artifacts/regression-build/Release/render_benchmark.exe'
if (-not (Test-Path -LiteralPath $benchmark)) { throw 'Build the tests first with scripts/Test-Viewer.ps1.' }
$log = Join-Path $root ('artifacts/render-benchmark-{0}-{1}.log' -f $Label,(Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-Host 'Creates and moves only a test window. Close VKomado first and leave input idle during the test.'
& $benchmark $viewer $log
if ($LASTEXITCODE -ne 0) { throw "Benchmark failed. Review $log" }
$samples = @(Get-Content -LiteralPath $log | Where-Object { $_ -match 'telemetry' } | Select-Object -Skip 2 | ForEach-Object {
    if ($_ -match 'fps=([\d.]+)') { [double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture) }
})
if ($samples.Count -eq 0) { throw 'No steady-state telemetry samples found.' }
[pscustomobject]@{
    Viewer = $viewer
    Samples = $samples.Count
    MeanFPS = ($samples | Measure-Object -Average).Average
    MinimumFPS = ($samples | Measure-Object -Minimum).Minimum
    Log = $log
}
