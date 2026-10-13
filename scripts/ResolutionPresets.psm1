Set-StrictMode -Version Latest

function Add-VddResolutionPresets {
    param([Parameter(Mandatory)][string]$Xml)
    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create([IO.StringReader]::new($Xml), $settings)
    $document = [Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.XmlResolver = $null
    try { $document.Load($reader) } finally { $reader.Dispose() }
    if ($document.DocumentElement.Name -ne 'vdd_settings' -or
        $document.DocumentElement.NamespaceURI -ne '' -or
        $document.SelectNodes('/vdd_settings/resolutions').Count -ne 1 -or
        $document.SelectSingleNode('/vdd_settings/monitors/count').InnerText -ne '1') {
        throw 'Expected the single-monitor VDD XML profile. Configuration was not changed.'
    }
    $resolutions = $document.SelectSingleNode('/vdd_settings/resolutions')
    $added = [Collections.Generic.List[string]]::new()
    foreach ($size in @(@(1920,1080), @(1280,720), @(2560,1440), @(3840,2160))) {
        $width = $size[0]; $height = $size[1]
        $node = $resolutions.SelectSingleNode("resolution[width='$width' and height='$height']")
        if (-not $node) {
            $node = $document.CreateElement('resolution')
            foreach ($pair in @(@('width', $width), @('height', $height))) {
                $child = $document.CreateElement($pair[0]); $child.InnerText = [string]$pair[1]
                [void]$node.AppendChild($child)
            }
            [void]$resolutions.AppendChild($node)
        }
        if (-not $node.SelectSingleNode("refresh_rate[text()='60']")) {
            $rate = $document.CreateElement('refresh_rate'); $rate.InnerText = '60'
            [void]$node.AppendChild($rate)
            $added.Add("${width}x${height}@60")
        }
    }
    [pscustomobject]@{ Document = $document; Added = $added.ToArray() }
}
function Set-VddConfigurationFile {
    param(
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Source,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Destination,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Backup
    )
    # Windows PowerShell 5.1 can marshal $null to an empty string for this
    # overload. Always supply a real backup path, including during rollback.
    $paths = @($Source, $Destination, $Backup) | ForEach-Object { [IO.Path]::GetFullPath($_) }
    if ($paths[0] -eq $paths[1] -or $paths[0] -eq $paths[2] -or $paths[1] -eq $paths[2]) {
        throw 'Source, destination and backup must be distinct paths.'
    }
    [IO.File]::Replace($paths[0], $paths[1], $paths[2])
}
Export-ModuleMember -Function Add-VddResolutionPresets, Set-VddConfigurationFile
