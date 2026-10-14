[CmdletBinding()]
param([switch]$Diagnostic)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& "$PSScriptRoot/Build-Loopback.ps1"
$release = Join-Path $root 'artifacts/releases'
$version = if ($Diagnostic) { '1.2.0-diagnostics' } else { '1.2.0' }
$package = Join-Path $release "VKomado-$version-win-x64"
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item -LiteralPath "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" -Destination "$package/VKomado.exe" -Force
Copy-Item -LiteralPath "$root/LICENSE" -Destination "$package/LICENSE.txt" -Force
Copy-Item -LiteralPath "$root/THIRD-PARTY-NOTICES.md" -Destination "$package/THIRD-PARTY-NOTICES.md" -Force
Copy-Item -LiteralPath "$root/third_party/licenses/Loopback-MIT.txt" -Destination "$package/Loopback-MIT.txt" -Force
# Write a UTF-8 BOM for Windows text editors.
$readme = [System.IO.File]::ReadAllText("$root/packaging/README.txt")
if ($Diagnostic) {
    $readme = "診断版: ビューア自身のドラッグ時の遅延を調べるログを追加しています。原因の特定・修正済みを意味しません。`r`nログ: %LOCALAPPDATA%\SharedVD\Logs`r`n`r`n" + $readme
}
[System.IO.File]::WriteAllText("$package/README.txt", $readme, [System.Text.UTF8Encoding]::new($true))
$zip = "$package.zip"
Compress-Archive -LiteralPath $package -DestinationPath $zip -Force
Get-FileHash -LiteralPath "$package/VKomado.exe", $zip -Algorithm SHA256 |
    Format-Table -AutoSize | Out-String | Set-Content "$release/VKomado-$version-sha256.txt"
Write-Host "EXE: $package/VKomado.exe"
Write-Host "ZIP: $zip"
