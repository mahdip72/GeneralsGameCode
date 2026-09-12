Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Test-Stage5AsciiMarkerInFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Marker
    )
    if ([string]::IsNullOrWhiteSpace($Marker)) {
        throw 'Stage 5 executable capability marker must not be empty.'
    }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] 65536
        $carry = ''
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $chunk = [Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            $window = $carry + $chunk
            if ($window.IndexOf($Marker, [StringComparison]::Ordinal) -ge 0) {
                return $true
            }
            $carryLength = [Math]::Min($window.Length, [Math]::Max(0, $Marker.Length - 1))
            $carry = if ($carryLength -eq 0) { '' } else {
                $window.Substring($window.Length - $carryLength, $carryLength)
            }
        }
        return $false
    }
    finally { $stream.Dispose() }
}

function Assert-Stage5ProcessLocalProfileCapability {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [string]$Context = 'Installed executable'
    )
    $markers = @(
        'RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1',
        '-validationExecutableSha256')
    foreach ($marker in $markers) {
        if (-not (Test-Stage5AsciiMarkerInFile $Executable $marker)) {
            throw "$Context does not advertise required process-local profile capability marker '$marker'; refusing unsupported validation binary before profile setup."
        }
    }
    return [pscustomobject]@{
        strategy = 'process-local-validation-profile-root'
        capabilityVersion = 'v1'
        markers = @($markers)
    }
}

Export-ModuleMember -Function Test-Stage5AsciiMarkerInFile, `
    Assert-Stage5ProcessLocalProfileCapability
