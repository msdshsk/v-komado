# Downloads pinned build inputs into artifacts; does not install VDD.
[CmdletBinding()]
param([switch]$SetupTools)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$downloads = Join-Path $root 'artifacts/downloads'
New-Item -ItemType Directory -Path $downloads -Force | Out-Null
function Get-VerifiedFile([string]$Url, [string]$Path, [string]$Hash) {
    if (-not (Test-Path -LiteralPath $Path)) { Invoke-WebRequest -Uri $Url -OutFile $Path }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        throw "Checksum mismatch: $Path. File was not executed or extracted."
    }
}
$archive = Join-Path $downloads 'VirtualDisplayDriver-x86.Driver.Only.zip'
Get-VerifiedFile 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip' $archive 'E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A'
$driver = Join-Path $downloads 'driver-25.7.23'
if (-not (Test-Path (Join-Path $driver 'VirtualDisplayDriver/MttVDD.inf'))) {
    Expand-Archive -LiteralPath $archive -DestinationPath $driver
}
Write-Host "Driver payload prepared (not installed): $driver"
if ($SetupTools) {
    $tools = Join-Path $root 'artifacts/tools'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    $compiler = Join-Path $tools 'inno/ISCC.exe'
    if (-not (Test-Path -LiteralPath $compiler)) {
        $installer = Join-Path $tools 'innosetup-7.1.0-x64.exe'
        Get-VerifiedFile 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe' $installer '0362A383ED217D4C4239B5933866DD96D3EB2102737DA92F80F6057A4B40DF2F'
        if ((Get-AuthenticodeSignature -LiteralPath $installer).Status -ne 'Valid') { throw 'Inno Setup signature invalid.' }
        $destination = Join-Path $tools 'inno'
        $arguments = '/PORTABLE=1 /CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /DIR="' + $destination + '"'
        $child = Start-Process -FilePath $installer -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
        if ($child.ExitCode -ne 0) { throw "Portable compiler extraction failed: $($child.ExitCode)" }
    }
    if ((Get-AuthenticodeSignature -LiteralPath $compiler).Status -ne 'Valid') { throw 'Compiler signature invalid.' }
    Write-Host "Portable installer compiler ready: $compiler"
}
