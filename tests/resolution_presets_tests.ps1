$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../scripts/ResolutionPresets.psm1') -Force
function Assert([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message }; Write-Output "PASS $Message" }
$original = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'fixtures/vdd-single-resolution.xml'))
$result = Add-VddResolutionPresets $original
Assert ($result.Added.Count -eq 3) 'adds the three missing sizes'
Assert ($result.Document.SelectNodes('/vdd_settings/resolutions/resolution').Count -eq 4) 'four presets'
Assert ($result.Document.vdd_settings.gpu.friendlyname -eq 'Fixture GPU') 'retains GPU preference'
Assert ($result.Document.vdd_settings.options.HardwareCursor -eq 'true') 'retains other driver options'
Assert ($result.Document.vdd_settings.resolutions.resolution[0].width -eq '1920') 'retains original preferred resolution ordering'
$again = Add-VddResolutionPresets $result.Document.OuterXml
Assert ($again.Added.Count -eq 0 -and $again.Document.OuterXml -eq $result.Document.OuterXml) 'idempotent without duplicated modes'
$custom = $original.Replace('<refresh_rate>60</refresh_rate>', '<refresh_rate>75</refresh_rate>')
$updated = Add-VddResolutionPresets $custom
Assert ($updated.Document.SelectNodes('/vdd_settings/resolutions/resolution[width=1920]/refresh_rate').Count -eq 2) 'retains custom refresh rate and adds 60 Hz'
foreach ($invalid in @($original.Replace('<count>1</count>', '<count>2</count>'), '<other/>', '<!DOCTYPE vdd_settings [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><vdd_settings>&x;</vdd_settings>')) {
    $rejected = $false
    try { $null = Add-VddResolutionPresets $invalid } catch { $rejected = $true }
    Assert $rejected 'rejects unsupported or unsafe XML'
}
$testRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../artifacts'))
$testDirectory = Join-Path $testRoot ('resolution-replacement-test-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testDirectory)
$destination = Join-Path $testDirectory 'vdd_settings.xml'
$source = Join-Path $testDirectory 'replacement.tmp'
$backup = Join-Path $testDirectory 'original.bak'
$rejected = Join-Path $testDirectory 'rejected.bak'
try {
    [IO.File]::WriteAllText($destination, $original)
    [IO.File]::Copy($destination, $backup)
    $originalHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    $result.Document.Save($source)
    $updatedHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    # Reproduce the failing call in a disposable directory on Windows PowerShell.
    if ($PSVersionTable.PSVersion.Major -eq 5) {
        $nullRejected = $false
        try { [IO.File]::Replace($source, $destination, $null) } catch { $nullRejected = $true }
        Assert $nullRejected 'reproduces Windows PowerShell null-backup failure'
        Assert ((Get-FileHash -LiteralPath $destination).Hash -eq $originalHash) 'failed replacement leaves original unchanged'
    }
    Set-VddConfigurationFile -Source $source -Destination $destination -Backup $backup
    Assert ((Get-FileHash -LiteralPath $destination).Hash -eq $updatedHash) 'real file replacement installs new XML'
    Assert ((Get-FileHash -LiteralPath $backup).Hash -eq $originalHash) 'pre-existing backup retains original bytes'
    Assert (-not (Test-Path -LiteralPath $source)) 'replacement consumes staged file'
    [IO.File]::Copy($backup, $source)
    Set-VddConfigurationFile -Source $source -Destination $destination -Backup $rejected
    Assert ((Get-FileHash -LiteralPath $destination).Hash -eq $originalHash) 'rollback restores exact original bytes'
    Assert ((Get-FileHash -LiteralPath $backup).Hash -eq $originalHash) 'rollback preserves original backup'
    Assert ((Get-FileHash -LiteralPath $rejected).Hash -eq $updatedHash) 'rollback retains rejected configuration'
    $failed = $false
    try { Set-VddConfigurationFile -Source $source -Destination $destination -Backup $rejected } catch { $failed = $true }
    Assert $failed 'missing staged file fails safely'
    Assert ((Get-FileHash -LiteralPath $destination).Hash -eq $originalHash) 'failed rollback attempt leaves current file intact'
} finally {
    # Delete only the exact fixture files; remove the now-empty directory without recursion.
    foreach ($file in @($source, $destination, $backup, $rejected)) {
        if ([IO.File]::Exists($file)) { [IO.File]::Delete($file) }
    }
    [IO.Directory]::Delete($testDirectory, $false)
}
Write-Output 'resolution_presets_tests=PASS (no live configuration changes)'
