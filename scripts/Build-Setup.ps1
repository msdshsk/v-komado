[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$driver = Join-Path $root 'artifacts/downloads/driver-25.7.23/VirtualDisplayDriver'
$archive = Join-Path $root 'artifacts/downloads/VirtualDisplayDriver-x86.Driver.Only.zip'
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne 'E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A') { throw 'Driver archive hash mismatch' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($name in @('MttVDD.inf','MttVDD.dll','mttvdd.cat')) {
        $entry = $zip.GetEntry("VirtualDisplayDriver/$name")
        if (-not $entry) { throw "Missing driver member: $name" }
        $stream = $entry.Open()
        $hash = [Security.Cryptography.SHA256]::Create()
        try { $expected = [Convert]::ToHexString($hash.ComputeHash($stream)) }
        finally { $hash.Dispose(); $stream.Dispose() }
        if ((Get-FileHash "$driver/$name" -Algorithm SHA256).Hash -ne $expected) { throw "Driver payload mismatch: $name" }
    }
} finally { $zip.Dispose() }
foreach ($name in @('MttVDD.dll','mttvdd.cat')) {
    if ((Get-AuthenticodeSignature "$driver/$name").Status -ne 'Valid') { throw "Driver signature invalid: $name" }
}
& "$PSScriptRoot/Build-Loopback.ps1"
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S "$root/app" -B "$root/artifacts/manager-build" -G 'Visual Studio 17 2022' -A x64 "-DPAYLOAD_DIR=$driver"
if ($LASTEXITCODE -ne 0) { throw 'Manager configure failed' }
& $cmake --build "$root/artifacts/manager-build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Manager build failed' }
& "$root/artifacts/manager-build/Release/manager_tests.exe" $driver
if ($LASTEXITCODE -ne 0) { throw 'Manager safety tests failed' }
$compiler = Join-Path $root 'artifacts/tools/inno/ISCC.exe'
if (-not (Test-Path $compiler)) { throw 'Inno Setup 7.1.0 portable compiler is required at artifacts/tools/inno. See docs/SETUP.md.' }
if ((Get-AuthenticodeSignature $compiler).Status -ne 'Valid') { throw 'Installer compiler signature invalid' }
& $compiler "/DRoot=$root" "$root/packaging/VKomado.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }
$setup = Join-Path $root 'artifacts/releases/VKomado-1.2.0-Setup-win-x64.exe'
Get-FileHash $setup -Algorithm SHA256 | Format-List | Out-String | Set-Content "$setup.sha256.txt"
Write-Host "SETUP: $setup"
