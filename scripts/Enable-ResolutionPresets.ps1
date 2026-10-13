# For VKomado and legacy SharedVD installs using the signed VDD 25.7.23 XML profile.
[CmdletBinding()]
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ResolutionPresets.psm1') -Force

if (-not [Environment]::Is64BitProcess) { throw 'Please use 64-bit PowerShell.' }
$state = Get-ItemProperty 'HKLM:/SOFTWARE/SharedVD'
$install = Join-Path $env:ProgramFiles 'SharedVD'
if ($state.Phase -ne 'ready' -or $state.Directory -ne $install) { throw 'VKomado installation is not ready.' }
$helper = Join-Path $install 'VKomado.Device.exe'
if (-not (Test-Path -LiteralPath $helper)) { $helper = Join-Path $install 'SharedVD.Device.exe' }
$driverKey = Get-ItemProperty 'HKLM:/SOFTWARE/MikeTheTech/VirtualDisplayDriver' -ErrorAction SilentlyContinue
$configDirectory = 'C:\VirtualDisplayDriver'
if ($driverKey -and $driverKey.PSObject.Properties['VDDPATH'] -and $driverKey.VDDPATH) {
    $configDirectory = $driverKey.VDDPATH
}
$configDirectory = [IO.Path]::GetFullPath($configDirectory).TrimEnd('\')
if ($configDirectory -notin @('C:\VirtualDisplayDriver', (Join-Path $install 'Driver'))) {
    throw "Custom VDD configuration path: $configDirectory. No changes made."
}
$config = Join-Path $configDirectory 'vdd_settings.xml'
foreach ($path in @($config, $helper)) {
    $item = Get-Item -LiteralPath $path
    while ($item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point is not supported: $($item.FullName)" }
        $item = if ($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
    }
}
$beforeHash = (Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
$profile = Add-VddResolutionPresets ([IO.File]::ReadAllText($config))
$device = Get-PnpDevice -InstanceId $state.DeviceId
$ids = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds').Data
if (-not (@($ids) -contains 'Root\MttVDD' -or @($ids) -contains 'MttVDD')) { throw 'The recorded device is not VDD.' }
$problem = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
if ($problem -notin @(0,22)) { throw "VDD device is not ready (problem $problem)." }
$wasEnabled = $problem -eq 0

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public static class VKomadoResolutionModes {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct Display {
        public uint Size;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Description;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Key;
    }
    [StructLayout(LayoutKind.Explicit, Size=220)]
    public struct Mode {
        [FieldOffset(68)] public ushort Size;
        [FieldOffset(168)] public uint Bits;
        [FieldOffset(172)] public uint Width;
        [FieldOffset(176)] public uint Height;
        [FieldOffset(184)] public uint Frequency;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern bool EnumDisplayDevices(string name, uint index, ref Display display, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern bool EnumDisplaySettingsEx(string name, int index, ref Mode mode, uint flags);
    public static string[] Read(string instance) {
        var result = new HashSet<string>();
        for (uint i=0;;i++) {
            var display = new Display { Size=(uint)Marshal.SizeOf(typeof(Display)) };
            if (!EnumDisplayDevices(null,i,ref display,0)) break;
            if (!string.Equals(display.Id,"Root\\MttVDD",StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(display.Id,instance,StringComparison.OrdinalIgnoreCase)) continue;
            var mode = new Mode { Size=220 };
            for (int j=0;EnumDisplaySettingsEx(display.Name,j,ref mode,0);j++)
                if (mode.Bits==32) result.Add(mode.Width+"x"+mode.Height+"@"+mode.Frequency);
        }
        var array = new string[result.Count]; result.CopyTo(array); return array;
    }
}
'@
$required = @('1280x720@60','1920x1080@60','2560x1440@60','3840x2160@60')
$modes = @([VKomadoResolutionModes]::Read($state.DeviceId))
Write-Host "Configuration: $config"
Write-Host "Device: $($state.DeviceId) / currently enabled: $wasEnabled"
Write-Host "Presets: $($required -join ', ')"
Write-Host "Currently advertised: $($modes -join ', ')"
Write-Host "Add to XML: $($profile.Added -join ', ')"
if ($Preview) { Write-Host 'PREVIEW ONLY: no files, windows or devices were changed.'; return }

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run Enable-ResolutionPresets.cmd and approve Windows administrator confirmation.'
}
if (Get-Process -Name VKomado.Control,SharedVD.Control -ErrorAction SilentlyContinue) {
    throw 'Close the VKomado control window, then run this script again.'
}
function Invoke-Device([string]$Action) {
    $child = Start-Process -FilePath $helper -ArgumentList $Action -WindowStyle Hidden -PassThru -Wait
    if ($child.ExitCode -ne 0) { throw "VKomado $Action failed (exit $($child.ExitCode)). No automatic reboot was performed." }
}
Invoke-Device '--preflight'
if ($profile.Added.Count -eq 0 -and @($required | Where-Object { $_ -notin $modes }).Count -eq 0) {
    Write-Host 'All four presets are already available. No changes needed.'; return
}
$backup = $config + '.before-resolution-presets-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N') + '.bak'
$temporary = $config + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
$changed = $false
$disabled = $false
try {
    if ((Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash -ne $beforeHash) { throw 'Configuration changed during preparation. Retry.' }
    [IO.File]::Copy($config, $backup, $false)
    Write-Host "Backup: $backup"
    # Native helper rescues apps first and aborts on failed rescue; only then disables VDD.
    Invoke-Device '--disable'
    $disabled = $true
    if ((Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash -ne $beforeHash) { throw 'Configuration changed before update. No overwrite.' }
    $profile.Document.Save($temporary)
    Set-VddConfigurationFile -Source $temporary -Destination $config -Backup $backup
    $changed = $true
    Invoke-Device '--enable'
    $modes = @([VKomadoResolutionModes]::Read($state.DeviceId))
    $missing = @($required | Where-Object { $_ -notin $modes })
    if ($missing.Count) { throw "Driver did not advertise presets: $($missing -join ', ')" }
    if (-not $wasEnabled) { Invoke-Device '--disable' }
    Write-Host 'SUCCESS: all four resolutions are available at 60 Hz.'
    Write-Host 'Open Windows Settings > System > Display, select VDD, then choose Display resolution.'
    Write-Host 'Use VKomado to open the viewer again. The backup is retained at the path above.'
} catch {
    $failure = $_
    if ($changed) {
        try {
            Invoke-Device '--disable'
            [IO.File]::Copy($backup, $temporary, $true)
            $failedConfig = $config + '.failed-resolution-presets-' + [Guid]::NewGuid().ToString('N') + '.bak'
            Set-VddConfigurationFile -Source $temporary -Destination $config -Backup $failedConfig
            Write-Warning 'The original XML configuration was restored.'
            Write-Warning "The rejected configuration was retained at: $failedConfig"
        } catch { Write-Warning "Automatic restore failed. Backup: $backup. $($_.Exception.Message)" }
    }
    if ($disabled -and $wasEnabled) {
        try { Invoke-Device '--enable' } catch { Write-Warning "Could not re-enable VDD. Use VKomado ON. $($_.Exception.Message)" }
    }
    throw $failure
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
