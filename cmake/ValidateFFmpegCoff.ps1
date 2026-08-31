[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Path,
    [Parameter(Mandatory = $true)]
    [UInt16] $ExpectedMachine
)

function Get-UInt16LE([byte[]] $Bytes, [int] $Offset) {
    return [UInt16](([int]$Bytes[$Offset]) -bor (([int]$Bytes[$Offset + 1]) -shl 8))
}

function Get-UInt32LE([byte[]] $Bytes, [int] $Offset) {
    return [UInt32](([int]$Bytes[$Offset]) -bor (([int]$Bytes[$Offset + 1]) -shl 8) -bor
        (([int]$Bytes[$Offset + 2]) -shl 16) -bor (([int]$Bytes[$Offset + 3]) -shl 24))
}

function Get-ArchiveMemberMachines([byte[]] $Bytes) {
	$magic = [Text.Encoding]::ASCII.GetBytes("!<arch>`n")
    if ($Bytes.Length -lt $magic.Length) { return @() }
    for ($i = 0; $i -lt $magic.Length; ++$i) {
        if ($Bytes[$i] -ne $magic[$i]) { return @() }
    }

    $machines = New-Object System.Collections.Generic.List[UInt16]
    $offset = 8
    while ($offset + 60 -le $Bytes.Length) {
        $name = [Text.Encoding]::ASCII.GetString($Bytes, $offset, 16).Trim()
        $sizeText = [Text.Encoding]::ASCII.GetString($Bytes, $offset + 48, 10).Trim()
        [Int64] $size = 0
        if (-not [Int64]::TryParse($sizeText, [Globalization.NumberStyles]::Integer,
                [Globalization.CultureInfo]::InvariantCulture, [ref] $size) -or $size -lt 0) {
            throw "Invalid archive member size in '$Path'."
        }
        $dataOffset = $offset + 60
        if ($dataOffset + $size -gt $Bytes.Length) {
            throw "Truncated archive member in '$Path'."
        }
        if (($name -ne '/') -and ($name -ne '//') -and ($name -notmatch '^\s*$') -and ($name -notmatch '^/__\.SYMDEF')) {
            if ($size -ge 20) {
                $machineOffset = 0
                if (($Bytes[$dataOffset] -eq 0) -and ($Bytes[$dataOffset + 1] -eq 0) -and ($Bytes[$dataOffset + 2] -eq 0xff) -and ($Bytes[$dataOffset + 3] -eq 0xff)) {
                    $machineOffset = 6
                }
                $machine = Get-UInt16LE $Bytes ($dataOffset + $machineOffset)
                if ($machine -eq 0x014c -or $machine -eq 0x8664 -or $machine -eq 0xaa64) {
                    $machines.Add($machine)
                }
            }
        }
        $offset = $dataOffset + $size
        if (($offset % 2) -ne 0) { ++$offset }
    }
    return $machines.ToArray()
}

$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
$machines = Get-ArchiveMemberMachines $bytes
if ($machines.Count -eq 0 -and $bytes.Length -ge 20) {
    $machineOffset = 0
    if (($bytes[0] -eq 0) -and ($bytes[1] -eq 0) -and ($bytes[2] -eq 0xff) -and ($bytes[3] -eq 0xff)) {
        $machineOffset = 6
    }
    $machine = Get-UInt16LE $bytes $machineOffset
    if ($machine -eq 0x014c -or $machine -eq 0x8664 -or $machine -eq 0xaa64) {
        $machines = @($machine)
    }
}

if ($machines.Count -eq 0) {
    throw "No COFF object machine records were found in FFmpeg import library '$Path'."
}
$wrong = @($machines | Where-Object { $_ -ne $ExpectedMachine })
if ($wrong.Count -ne 0) {
    $actual = (($machines | Sort-Object -Unique | ForEach-Object { '0x{0:X4}' -f $_ }) -join ', ')
    throw "FFmpeg import library '$Path' has machine record(s) $actual; expected 0x$('{0:X4}' -f $ExpectedMachine)."
}
