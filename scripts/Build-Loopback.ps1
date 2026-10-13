[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'viewer'
$build = Join-Path $root 'artifacts/viewer-build'
if (-not (Test-Path "$source/CMakeLists.txt")) { throw 'SharedVD viewer source is missing. Clone the complete repository.' }
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio C++ tools / CMake not found.' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
if (-not (Test-Path $cmake)) { throw 'CMake not found. No dependency installer was run.' }
New-Item -ItemType Directory -Path $build -Force | Out-Null
& $cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 "-DSHAREDVD_RESOURCES=$root/assets" 2>&1 | Tee-Object "$root/artifacts/build-configure.log"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmake --build $build --config Release --parallel 2>&1 | Tee-Object "$root/artifacts/build-release.log"
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
