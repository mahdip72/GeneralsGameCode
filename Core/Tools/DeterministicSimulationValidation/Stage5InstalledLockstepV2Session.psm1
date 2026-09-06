Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Explicit private dependencies.  This module owns the installed lockstep-v2
# process/session boundary; callers own provenance mode and final receipt shape.
$evidenceModulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
$registryModulePath = Join-Path $PSScriptRoot 'Stage5RegistryRecovery.psm1'
$profileModulePath = Join-Path $PSScriptRoot 'Stage5ValidationProfileCapability.psm1'
Import-Module $evidenceModulePath -ErrorAction Stop
Import-Module $registryModulePath -ErrorAction Stop
Import-Module $profileModulePath -ErrorAction Stop

$CommonStopFrame = 4096
$LockstepSchema = 2
$LockstepProtocolEpoch = 2
$LockstepAuthorityMask = 63
$LockstepNetworkPeerCount = 2
$LockstepNetworkRosterMask = 0x3
$LockstepSimulationRosterMask = 0x3f
$LockstepAIRosterMask = 0x3c
$LockstepAIPlayerCount = 4
$LockstepCheckpointCount = 129
$LockstepMode = 'installed-lockstep-v2-production'
$LockstepProducer = 'installed-lockstep-v2'
$LockstepMagic = 'RTS_LOCKSTEP_V2_RECEIPT'
$LockstepNegativeProbeMagic = 'RTS_LOCKSTEP_V2_NEGATIVE_PROBE'
$PostKillWaitMilliseconds = 5000
$script:LockstepHostSelfTestScratchRoot = $null

function Set-Stage5LockstepHostSelfTestScratchRoot {
    param([AllowNull()][string]$Path)
    $script:LockstepHostSelfTestScratchRoot = $Path
}

function Get-Stage5InstalledLockstepV2SystemCpuSets {
    if (-not ('Stage5InstalledLockstepV2Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Stage5InstalledLockstepV2Native {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetSystemCpuSetInformation(
        IntPtr information, uint bufferLength, out uint returnedLength,
        IntPtr process, uint flags);
}
'@
    }
    [UInt32]$required = 0
    [void][Stage5InstalledLockstepV2Native]::GetSystemCpuSetInformation(
        [IntPtr]::Zero, 0, [ref]$required, [IntPtr]::Zero, 0)
    if ($required -lt 32) {
        throw 'GetSystemCpuSetInformation returned no installed lockstep-v2 topology.'
    }
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal([int]$required)
    try {
        [UInt32]$written = 0
        if (-not [Stage5InstalledLockstepV2Native]::GetSystemCpuSetInformation(
                $buffer, $required, [ref]$written, [IntPtr]::Zero, 0)) {
            throw 'GetSystemCpuSetInformation failed for installed lockstep-v2 topology.'
        }
        $rows = @()
        $offset = 0
        while ($offset -lt $written) {
            $entry = [IntPtr]::Add($buffer, $offset)
            $size = [Runtime.InteropServices.Marshal]::ReadInt32($entry, 0)
            $type = [Runtime.InteropServices.Marshal]::ReadInt32($entry, 4)
            if ($size -lt 8 -or ($offset + $size) -gt $written) {
                throw 'GetSystemCpuSetInformation returned a malformed topology entry.'
            }
            if ($type -eq 0 -and $size -ge 32) {
                $flags = [Runtime.InteropServices.Marshal]::ReadByte($entry, 19)
                $rows += [pscustomobject]@{
                    id = [UInt32][Runtime.InteropServices.Marshal]::ReadInt32($entry, 8)
                    group = [UInt16][Runtime.InteropServices.Marshal]::ReadInt16($entry, 12)
                    logicalProcessorIndex = [byte][Runtime.InteropServices.Marshal]::ReadByte($entry, 14)
                    coreIndex = [byte][Runtime.InteropServices.Marshal]::ReadByte($entry, 15)
                    parked = (($flags -band 1) -ne 0)
                    allocated = (($flags -band 2) -ne 0)
                    available = (($flags -band 3) -eq 0)
                }
            }
            $offset += $size
        }
        return $rows
    }
    finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }
}

function Get-Stage5InstalledLockstepV2HostTopology {
    param(
        [ValidateRange(1, 256)][int]$MinimumPhysicalCores = 1,
        [ValidateRange(1, 256)][int]$MaximumPhysicalCores = 6,
        [ValidateRange(1, 4096)][int]$MaximumLogicalProcessors = 12
    )
    if ($MaximumPhysicalCores -lt $MinimumPhysicalCores) {
        throw 'Installed lockstep-v2 topology bounds are internally inconsistent.'
    }
    $cpuSets = @(Get-Stage5InstalledLockstepV2SystemCpuSets)
    $available = @($cpuSets | Where-Object { $_.available })
    $physical = @{}
    foreach ($cpuSet in $available) {
        $physical[('{0}:{1}' -f $cpuSet.group, $cpuSet.coreIndex)] = $true
    }
    if ($physical.Count -lt $MinimumPhysicalCores) {
        throw "Installed lockstep-v2 requires at least $MinimumPhysicalCores available physical cores; host exposes $($physical.Count)."
    }
    if ($physical.Count -gt $MaximumPhysicalCores) {
        throw "Installed lockstep-v2 requires at most $MaximumPhysicalCores available physical cores; host exposes $($physical.Count)."
    }
    if ($available.Count -gt $MaximumLogicalProcessors) {
        throw "Installed lockstep-v2 requires at most $MaximumLogicalProcessors available logical processors; host exposes $($available.Count)."
    }
    return [pscustomobject]@{
        source = 'GetSystemCpuSetInformation'
        physicalCoreCount = [int]$physical.Count
        logicalProcessorCount = [int]$available.Count
        cpuSets = @($available)
    }
}

function Get-UpperSha256 {
    param([string]$Path)
    return (Get-Stage5FileSha256 $Path).ToUpperInvariant()
}

function Test-CanonicalHex {
    param([string]$Value, [int]$Length)
    return $null -ne $Value -and $Value.Length -eq $Length -and
        $Value -match ('^[0-9A-Fa-f]{{{0}}}$' -f $Length)
}

function Test-LowerHex40 {
    param([string]$Value)
    return $null -ne $Value -and $Value -cmatch '^[0-9a-f]{40}$'
}

function Assert-LockstepCanonicalUuid {
    param([string]$Value, [string]$Context)
    if ($Value -notmatch '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') {
        throw "$Context must be a canonical UUID."
    }
    return $Value
}

function Assert-LockstepRuntimeClosure {
    param([object]$Value, [string]$Context)
    if ($null -eq $Value -or
        [string]$Value.dependencyManifestSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$Value.closureSha256 -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "$Context runtime closure must contain canonical dependency-manifest and closure SHA-256 values."
    }
    return [pscustomobject]@{
        dependencyManifestSha256 = ([string]$Value.dependencyManifestSha256).ToUpperInvariant()
        closureSha256 = ([string]$Value.closureSha256).ToUpperInvariant()
    }
}

function Test-SafeHDirectory {
    param([string]$Path, [switch]$AllowWhitespace)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    $full = [IO.Path]::GetFullPath($Path)
    $isHPath = $full.Length -ge 3 -and
        $full.Substring(0, 1) -match '^[Hh]$' -and $full[1] -eq ':' -and
        ($full[2] -eq '\' -or $full[2] -eq '/') -and
        $full.Length -lt 248
    $selfTestRoot = [string]$script:LockstepHostSelfTestScratchRoot
    $isBoundedHostSelfTestPath = $false
    if (-not [string]::IsNullOrWhiteSpace($selfTestRoot)) {
        $selfTestRoot = [IO.Path]::GetFullPath($selfTestRoot).TrimEnd('\')
        $isBoundedHostSelfTestPath =
            $full -ceq $selfTestRoot -or
            $full.StartsWith($selfTestRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    }
    return $full.Length -ge 4 -and $full.Length -lt 248 -and
        ($isHPath -or $isBoundedHostSelfTestPath) -and
        $full.IndexOf('..', [StringComparison]::Ordinal) -lt 0 -and
        $full.IndexOf(';', [StringComparison]::Ordinal) -lt 0 -and
        $full.IndexOf('"', [StringComparison]::Ordinal) -lt 0 -and
        ($AllowWhitespace -or $full -notmatch '\s')
}

function Get-LockstepItemIfPresent {
    param([string]$Path)
    try { return Get-Item -LiteralPath $Path -Force -ErrorAction Stop }
    catch [System.Management.Automation.ItemNotFoundException] { return $null }
}

function Assert-LockstepNoReparse {
    param([string]$Path, [string]$Context = 'lockstep path', [string]$Boundary)
    $full = [IO.Path]::GetFullPath($Path)
    $limit = [IO.Path]::GetPathRoot($full)
    if (-not [string]::IsNullOrWhiteSpace($Boundary)) {
        $limit = [IO.Path]::GetFullPath($Boundary).TrimEnd('\')
        if ($full -cne $limit -and -not $full.StartsWith($limit + '\',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "$Context escapes its bounded root: $full"
        }
    }
    $cursor = $full
    while ($true) {
        $item = Get-LockstepItemIfPresent $cursor
        if ($null -ne $item -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Context contains a reparse point: $cursor"
        }
        if ($cursor -ceq $limit) { break }
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -ceq $cursor) { break }
        $cursor = [IO.Path]::GetFullPath($parent)
    }
}

function Ensure-LockstepHostSelfTestDirectory {
    param(
        [string]$Path,
        [string]$Context = 'lockstep host self-test directory',
        [switch]$Fresh
    )
    $full = [IO.Path]::GetFullPath($Path)
    $root = [string]$script:LockstepHostSelfTestScratchRoot
    Assert-LockstepNoReparse $full $Context $root
    Assert-LockstepNoReparse (Split-Path -Parent $full) "$Context parent"
    $item = Get-LockstepItemIfPresent $full
    if ($null -ne $item) {
        if ($Fresh) { throw "$Context was not fresh: $full" }
        if (-not $item.PSIsContainer) { throw "$Context is not a directory: $full" }
        Assert-LockstepNoReparse $full $Context $root
        return
    }
    New-Item -Path $full -ItemType Directory -ErrorAction Stop | Out-Null
    Assert-LockstepNoReparse $full $Context $root
    Assert-LockstepNoReparse (Split-Path -Parent $full) "$Context parent"
}

function Remove-LockstepHostSelfTestTree {
    param([string]$Path, [string]$Boundary)
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $root = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if (-not $root.PSIsContainer) { throw "lockstep cleanup root is not a directory: $full" }
    Assert-LockstepNoReparse $full 'lockstep cleanup root' $Boundary
    foreach ($child in @(Get-ChildItem -LiteralPath $full -Force -ErrorAction Stop)) {
        $childFull = [IO.Path]::GetFullPath($child.FullName)
        Assert-LockstepNoReparse $childFull 'lockstep cleanup child' $Boundary
        $current = Get-Item -LiteralPath $childFull -Force -ErrorAction Stop
        if ($current.PSIsContainer) {
            Remove-LockstepHostSelfTestTree $childFull $Boundary
        }
        else {
            Assert-LockstepNoReparse $childFull 'lockstep cleanup file' $Boundary
            Remove-Item -LiteralPath $childFull -Force -ErrorAction Stop
        }
    }
    Assert-LockstepNoReparse $full 'lockstep cleanup root' $Boundary
    if (@(Get-ChildItem -LiteralPath $full -Force -ErrorAction Stop).Count -ne 0) {
        throw "lockstep cleanup root changed during cleanup: $full"
    }
    Remove-Item -LiteralPath $full -Force -ErrorAction Stop
    if ($null -ne (Get-LockstepItemIfPresent $full)) {
        throw "lockstep cleanup did not remove: $full"
    }
}

function Try-NewLockstepDirectoryJunction {
    param([string]$LinkPath, [string]$TargetPath)
    try {
        New-Item -Path $LinkPath -ItemType Junction -Target $TargetPath `
            -ErrorAction Stop | Out-Null
    }
    catch { return $false }
    $item = Get-LockstepItemIfPresent $LinkPath
    return $null -ne $item -and
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
}

function Remove-LockstepDirectoryJunction {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -or
        -not $item.PSIsContainer) { throw "not a directory junction: $Path" }
    [IO.Directory]::Delete($Path)
    if ($null -ne (Get-LockstepItemIfPresent $Path)) {
        throw "directory junction remained after removal: $Path"
    }
}

function Test-SafeReceiptLeaf {
    param([string]$Name)
    return $null -ne $Name -and $Name.Length -gt 0 -and $Name.Length -lt 248 -and
        $Name.IndexOf('..', [StringComparison]::Ordinal) -lt 0 -and
        $Name.IndexOfAny([char[]]@('\', '/', ':', ';', '"')) -lt 0 -and
        $Name -notmatch '\s'
}

function Test-SafeMapName {
    param([string]$Name)
    if ($null -eq $Name -or $Name.Length -lt 4 -or $Name.Length -ge 248 -or
        $Name -match '(^[\\/]|:|\.\.|[;"])' -or
        -not $Name.EndsWith('.map', [StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }
    foreach ($character in $Name.ToCharArray()) {
        if ([char]::IsControl($character) -or
            ([char]::IsWhiteSpace($character) -and $character -ne [char]' ')) {
            return $false
        }
    }
    return $true
}

function Assert-LockstepMapCrcs {
    param([object]$Value, [string]$Context)
    $names = @()
    $isDictionary = $Value -is [Collections.IDictionary]
    if ($isDictionary) {
        $names = @($Value.Keys | ForEach-Object { [string]$_ })
    }
    elseif ($null -ne $Value) {
        $names = @($Value.PSObject.Properties | ForEach-Object { [string]$_.Name })
    }
    $expectedTitles = @('Generals', 'ZeroHour')
    if ($names.Count -ne $expectedTitles.Count -or
        @($names | Where-Object { $expectedTitles -cnotcontains $_ }).Count -ne 0) {
        throw "$Context must contain exactly the Generals and ZeroHour map CRCs."
    }
    $validated = [ordered]@{}
    foreach ($title in $expectedTitles) {
        $raw = if ($isDictionary) { $Value[$title] }
            else { $Value.PSObject.Properties[$title].Value }
        if (-not (Test-Stage5JsonInteger $raw) -or [UInt64]$raw -eq 0 -or
            [UInt64]$raw -gt [UInt64][uint32]::MaxValue) {
            throw "$Context $title map CRC must be a nonzero UInt32."
        }
        $validated[$title] = [uint32]$raw
    }
    return $validated
}

function ConvertTo-ReceiptUInt64 {
    param([string]$Value, [string]$Field)
    if ($null -eq $Value -or $Value -notmatch '^[0-9]+$') {
        throw "Receipt field $Field is not an unsigned decimal integer."
    }
    try {
        return [UInt64]::Parse($Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "Receipt field $Field is outside the UInt64 range."
    }
}

function ConvertTo-ReceiptUInt32 {
    param([string]$Value, [string]$Field)
    $parsed = ConvertTo-ReceiptUInt64 $Value $Field
    if ($parsed -gt [UInt64]::MaxValue -or $parsed -gt 4294967295) {
        throw "Receipt field $Field is outside the UInt32 range."
    }
    return [UInt32]$parsed
}

function ConvertTo-ReceiptBool {
    param([string]$Value, [string]$Field)
    if ($Value -cne '0' -and $Value -cne '1') {
        throw "Receipt field $Field is not a canonical boolean."
    }
    return $Value -cne '0'
}

function Resolve-BoundedArtifactPath {
    param([string]$Manifest, [string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\\/])\.([\\/]|$)' -or
        $RelativePath -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "Artifact path must be a nonempty manifest-relative path: $RelativePath"
    }
    $root = [IO.Path]::GetFullPath((Split-Path -Parent $Manifest)).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    $rootParts = @($root.Split([char[]]@([char]92, [char]47)) | Where-Object { $_ -ne '' })
    $candidateParts = @($candidate.Split([char[]]@([char]92, [char]47)) | Where-Object { $_ -ne '' })
    if ($candidateParts.Count -lt $rootParts.Count) {
        throw "Artifact path escapes its manifest root: $RelativePath"
    }
    for ($partIndex = 0; $partIndex -lt $rootParts.Count; ++$partIndex) {
        if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$candidateParts[$partIndex], [string]$rootParts[$partIndex])) {
            throw "Artifact path escapes its manifest root: $RelativePath"
        }
    }
    Assert-LockstepNoReparse $candidate 'artifact-set path' $root
    return $candidate
}

function Assert-HeadlessDirectExecutionOptIn {
    param([bool]$Allowed)
    if (-not $Allowed) {
        throw 'Installed lockstep-v2 execution requires the reviewed -AllowHeadlessDirectExecution exception.'
    }
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$Arguments)
    return (($Arguments | ForEach-Object {
        $argument = [string]$_
        if ($argument -match '[\s"]') {
            '"' + $argument.Replace('"', '\"') + '"'
        }
        else { $argument }
    }) -join ' ')
}

function Assert-X64PeExecutable {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) {
                throw "Installed executable is not a valid PE image: $Path"
            }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            if ($peOffset -le 0 -or $peOffset -ge ($stream.Length - 6)) {
                throw "Installed executable has an invalid PE header offset: $Path"
            }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550 -or
                $reader.ReadUInt16() -ne 0x8664) {
                throw "Installed executable is not the required native x64 image: $Path"
            }
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-LauncherRunContract {
    param(
        [string]$LauncherConfigPath,
        [string]$LauncherPath,
        [string]$RuntimeDirectory,
        [string]$Executable,
        [string]$LauncherConfigSha256,
        [string]$LauncherSha256
    )
    $configFull = [IO.Path]::GetFullPath($LauncherConfigPath)
    $launcherFull = [IO.Path]::GetFullPath($LauncherPath)
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    $executableFull = [IO.Path]::GetFullPath($Executable)
    if (-not (Test-Path -LiteralPath $configFull -PathType Leaf) -or
        -not (Test-Path -LiteralPath $launcherFull -PathType Leaf)) {
        throw "Installed launcher contract is incomplete under $runtimeFull."
    }
    if ([IO.Path]::GetFileName($configFull) -cne 'launcher.lcf' -or
        [IO.Path]::GetFileName($launcherFull) -cne 'launcher.exe') {
        throw 'Installed launcher artifacts must be named launcher.lcf and launcher.exe.'
    }
    if ([IO.Path]::GetFullPath((Split-Path -Parent $configFull)) -cne $runtimeFull -or
        [IO.Path]::GetFullPath((Split-Path -Parent $launcherFull)) -cne $runtimeFull) {
        throw 'Installed launcher, launcher.lcf, and executable must share one runtime directory.'
    }
    $lines = @(Get-Content -LiteralPath $configFull)
    $runLines = @($lines | Where-Object { $_ -match '^\s*RUN\s*=' })
    $otherLines = @($lines | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and $_ -notmatch '^\s*RUN\s*='
    })
    if ($runLines.Count -ne 1 -or $otherLines.Count -ne 0) {
        throw "launcher.lcf must contain exactly one RUN entry: $configFull"
    }
    $match = [regex]::Match($runLines[0],
        '^\s*RUN\s*=\s*(?<directory>\S+)\s+(?<executable>"[^"]+"|\S+)(?<arguments>.*)$')
    if (-not $match.Success) { throw "launcher.lcf RUN entry has an unsupported shape: $configFull" }
    $directory = $match.Groups['directory'].Value
    if ($directory -cne '.') {
        throw "launcher.lcf RUN working directory must be '.', got '$directory'."
    }
    $configuredExecutable = $match.Groups['executable'].Value.Trim('"')
    if ($configuredExecutable -notmatch '^[A-Za-z0-9._-]+\.exe$') {
        throw 'launcher.lcf RUN target must be a leaf executable name.'
    }
    $expectedExecutable = [IO.Path]::GetFileName($executableFull)
    if ($configuredExecutable -cne $expectedExecutable) {
        throw "launcher.lcf target '$configuredExecutable' does not match '$expectedExecutable'."
    }
    $targetFull = [IO.Path]::GetFullPath((Join-Path $runtimeFull $configuredExecutable))
    if ($targetFull -cne $executableFull) {
        throw 'launcher.lcf target does not resolve to the exact installed executable.'
    }

    $argumentText = $match.Groups['arguments'].Value.Trim()
    $arguments = New-Object 'Collections.Generic.List[string]'
    if (-not [string]::IsNullOrWhiteSpace($argumentText)) {
        $argumentMatches = [regex]::Matches($argumentText,
            '"(?<quoted>(?:[^"]|"")*)"|(?<bare>\S+)')
        $consumed = 0
        foreach ($argumentMatch in $argumentMatches) {
            if ($argumentMatch.Index -gt $consumed -and
                $argumentText.Substring($consumed, $argumentMatch.Index - $consumed) -notmatch '^\s+$') {
                throw 'launcher.lcf RUN arguments contain an unsupported token.'
            }
            $value = if ($argumentMatch.Groups['quoted'].Success) {
                $argumentMatch.Groups['quoted'].Value.Replace('""', '"')
            }
            else { $argumentMatch.Groups['bare'].Value }
            $arguments.Add($value) | Out-Null
            $consumed = $argumentMatch.Index + $argumentMatch.Length
        }
        if ($consumed -ne $argumentText.Length) {
            throw 'launcher.lcf RUN arguments contain an unsupported trailing token.'
        }
    }
    $launcherArguments = $arguments.ToArray()
    if (@($launcherArguments).Count -ne 4 -or
        $launcherArguments[0] -cne '-simulationMode' -or
        $launcherArguments[1] -cne 'parallel' -or
        $launcherArguments[2] -cne '-workerPolicy' -or
        $launcherArguments[3] -cne 'auto') {
        throw 'launcher.lcf may only contribute the reviewed native Stage 5 defaults.'
    }
    return [pscustomobject]@{
        schemaVersion = 1
        mode = 'headless-direct-exception'
        configPath = $configFull
        configSha256 = $LauncherConfigSha256
        launcherPath = $launcherFull
        launcherSha256 = $LauncherSha256
        directory = $directory
        executable = $configuredExecutable
        launcherTarget = $targetFull
        launcherArguments = @($launcherArguments)
        launcherWorkingDirectory = $runtimeFull
        directExecutable = $executableFull
        directWorkingDirectory = $runtimeFull
        directArguments = @($launcherArguments)
        childExitCodeObserved = $true
    }
}

function Get-LockstepWorkerProfiles {
    param([int]$PeerCount)
    if ($PeerCount -lt 2 -or $PeerCount -gt 8) {
        throw 'Lockstep-v2 worker profiling requires between two and eight peers.'
    }
    $profiles = New-Object 'Collections.Generic.List[object]'
    for ($peer = 0; $peer -lt $PeerCount; ++$peer) {
        if (($peer % 2) -eq 0) {
            $profiles.Add([pscustomobject]@{
                profile = 'explicit-two-workers'
                requestedWorkers = '2'
                workerPolicy = 'all'
                overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
            }) | Out-Null
        }
        else {
            $profiles.Add([pscustomobject]@{
                profile = 'automatic-workers'
                requestedWorkers = 'auto'
                workerPolicy = 'auto'
                # Keep the launcher policy explicit for this peer so the
                # recorded direct command remains reviewable against the
                # launcher defaults even when its value is unchanged.
                overrideArguments = @('-workerPolicy', 'auto')
            }) | Out-Null
        }
    }
    return $profiles.ToArray()
}

function Assert-MixedLockstepWorkerProfiles {
    param([object[]]$Profiles)
    if ($null -eq $Profiles -or $Profiles.Count -lt 2) {
        throw 'Lockstep-v2 requires at least two worker profiles.'
    }
    $profileKeys = @($Profiles | ForEach-Object {
        "$($_.requestedWorkers)|$($_.workerPolicy)"
    } | Select-Object -Unique)
    if ($profileKeys.Count -lt 2) {
        throw 'Lockstep-v2 worker qualification cannot use homogeneous peer worker profiles.'
    }
    foreach ($profile in $Profiles) {
        if ($profile.requestedWorkers -ceq 'auto') {
            if ($profile.workerPolicy -cne 'auto') {
                throw 'Automatic lockstep-v2 workers must use the auto policy.'
            }
        }
        else {
            $requested = 0
            if ($profile.requestedWorkers -notmatch '^[0-9]+$' -or
                -not [int]::TryParse($profile.requestedWorkers, [ref]$requested) -or
                $requested -lt 2 -or $requested -gt 64 -or
                $profile.workerPolicy -cne 'all') {
                throw 'Explicit lockstep-v2 worker profiles must request 2..64 all-policy workers.'
            }
        }
    }
}

function Get-LockstepStdoutProof {
    param(
        [string]$Stdout,
        [int]$ExpectedPeer
    )
    if ([string]::IsNullOrEmpty($Stdout)) {
        throw "Peer $ExpectedPeer did not provide executable-origin qualification stdout."
    }
    $lines = @($Stdout -split "`n" | ForEach-Object {
        $_.TrimEnd("`r")
    })
    $activeLines = @($lines | Where-Object {
        $_.StartsWith('LOCKSTEP_V2_VALIDATION_ACTIVE ',
            [StringComparison]::Ordinal)
    })
    if ($activeLines.Count -ne 1) {
        throw "Peer $ExpectedPeer must provide exactly one executable-origin lockstep-v2 active marker."
    }
    $activeMatch = [regex]::Match($activeLines[0],
        '^LOCKSTEP_V2_VALIDATION_ACTIVE peer=(?<peer>[0-9]+) frame_limit=(?<frame>[0-9]+)$')
    if (-not $activeMatch.Success -or
        [int]$activeMatch.Groups['peer'].Value -ne $ExpectedPeer -or
        [int]$activeMatch.Groups['frame'].Value -ne $CommonStopFrame) {
        throw "Peer $ExpectedPeer executable-origin active marker does not match the 4096-frame contract."
    }
    $passLines = @($lines | Where-Object {
        $_.StartsWith('LOCKSTEP_V2_VALIDATION_PASS ',
            [StringComparison]::Ordinal)
    })
    if ($passLines.Count -ne 1) {
        throw "Peer $ExpectedPeer must provide exactly one executable-origin lockstep-v2 pass marker."
    }
    $passMatch = [regex]::Match($passLines[0],
        '^LOCKSTEP_V2_VALIDATION_PASS peer=(?<peer>[0-9]+) pid=(?<pid>[0-9]+) frame=(?<frame>[0-9]+) crc=(?<crc>[0-9A-Fa-f]{8})$')
    if (-not $passMatch.Success -or
        [int]$passMatch.Groups['peer'].Value -ne $ExpectedPeer -or
        [int]$passMatch.Groups['frame'].Value -ne $CommonStopFrame) {
        throw "Peer $ExpectedPeer executable-origin pass marker does not match the 4096-frame contract."
    }
    return [pscustomobject]@{
        executableOrigin = $true
        peer = $ExpectedPeer
        pid = [int]$passMatch.Groups['pid'].Value
        frameLimit = $CommonStopFrame
        activeMarker = $activeLines[0]
        passMarker = $passLines[0]
        finalCrc = $passMatch.Groups['crc'].Value.ToUpperInvariant()
    }
}

function Get-LockstepReceiptWorkerTelemetry {
    param(
        [pscustomobject]$Parsed,
        [pscustomobject]$ExpectedProfile
    )
    if ($null -eq $ExpectedProfile) {
        throw "Receipt $($Parsed.path) has no reviewed worker profile."
    }
    $expectedWorkers = 0
    $isAutomatic = $ExpectedProfile.requestedWorkers -ceq 'auto'
    if (-not $isAutomatic -and
        (-not [int]::TryParse($ExpectedProfile.requestedWorkers, [ref]$expectedWorkers) -or
         $expectedWorkers -lt 2 -or $expectedWorkers -gt 64)) {
        throw "Receipt $($Parsed.path) has an invalid explicit worker profile."
    }
    $distinctCounts = @()
    $physicalMasks = @()
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $mask = ConvertTo-ReceiptUInt64 $Parsed.pairs["kernel_${kernel}_physical_worker_mask"] `
            "kernel_${kernel}_physical_worker_mask"
        $distinct = ConvertTo-ReceiptUInt32 $Parsed.pairs["kernel_${kernel}_distinct_physical_workers"] `
            "kernel_${kernel}_distinct_physical_workers"
        $peak = ConvertTo-ReceiptUInt32 $Parsed.pairs["kernel_${kernel}_peak_concurrent_physical_workers"] `
            "kernel_${kernel}_peak_concurrent_physical_workers"
        if ($mask -eq 0 -or $distinct -lt 2 -or $distinct -gt 64 -or
            $distinct -ne (Get-BitCount $mask) -or $peak -lt 2) {
            throw "Receipt $($Parsed.path) has invalid physical-worker telemetry for kernel $kernel."
        }
        if ($isAutomatic) {
            if ($distinct -le 2 -or $peak -ne $distinct) {
                throw "Receipt $($Parsed.path) automatic workers did not prove an effective count greater than two."
            }
        }
        elseif ($distinct -ne $expectedWorkers -or $peak -ne $expectedWorkers) {
            throw "Receipt $($Parsed.path) claimed worker count does not match effective kernel telemetry."
        }
        $distinctCounts += [int]$distinct
        $physicalMasks += [UInt64]$mask
    }
    $effectiveWorkers = [int](@($distinctCounts | Select-Object -First 1)[0])
    if (@($distinctCounts | Select-Object -Unique).Count -ne 1) {
        throw "Receipt $($Parsed.path) reports inconsistent effective worker counts across kernels."
    }
    if (-not $isAutomatic -and $effectiveWorkers -ne $expectedWorkers) {
        throw "Receipt $($Parsed.path) explicit worker override was not effective."
    }
    return [pscustomobject]@{
        requestedWorkers = $ExpectedProfile.requestedWorkers
        workerPolicy = $ExpectedProfile.workerPolicy
        effectiveWorkers = $effectiveWorkers
        distinctPhysicalWorkers = @($distinctCounts)
        physicalWorkerMasks = @($physicalMasks)
        executableOrigin = $true
    }
}

function New-LockstepTitleSessionContract {
    param(
        [string]$Title,
        [string]$SessionRoot,
        [string]$RuntimeDirectory
    )
    if ($Title -cne 'Generals' -and $Title -cne 'ZeroHour') {
        throw "Unsupported installed title for lockstep-v2 profile setup: $Title"
    }
    $sessionFull = [IO.Path]::GetFullPath($SessionRoot)
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    if (-not (Test-SafeHDirectory $sessionFull) -or
        -not (Test-SafeHDirectory $runtimeFull)) {
        throw "Lockstep-v2 title/session paths must remain on task-owned H:."
    }
    $documentsRoot = Join-Path $sessionFull 'Documents'
    $profileLeaf = if ($Title -ceq 'Generals') {
        'Command and Conquer Generals Data'
    }
    else { 'GGC-LockstepV2-ZeroHour' }
    $profileRoot = Join-Path $documentsRoot $profileLeaf
    $peerRoot = Join-Path $sessionFull 'Peers'
    $tempRoot = Join-Path $sessionFull 'Temp'
    $tmpRoot = Join-Path $sessionFull 'Tmp'
    $cacheRoot = Join-Path $sessionFull 'Cache'
    $logRoot = Join-Path $sessionFull 'Logs'
    $dumpRoot = Join-Path $sessionFull 'Dumps'
    $localAppDataRoot = Join-Path $sessionFull 'LocalAppData'
    $appDataRoot = Join-Path $sessionFull 'AppData'
    # Production sessions are constrained to H: by Test-SafeHDirectory.  The
    # host self-test also exercises this contract from its one bounded scratch
    # root, which can live on another drive on hosted runners.  Derive the
    # process home pair from the validated session path so that exception does
    # not accidentally refer back to an unmounted H: volume.
    $homeDrive = [IO.Path]::GetPathRoot($sessionFull).TrimEnd([char]92)
    $homePath = $sessionFull.Substring($homeDrive.Length)
    $environmentValues = [ordered]@{
        TEMP = $tempRoot
        TMP = $tmpRoot
        LOCALAPPDATA = $localAppDataRoot
        APPDATA = $appDataRoot
        USERPROFILE = $sessionFull
        HOMEDRIVE = $homeDrive
        HOMEPATH = $homePath
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = $cacheRoot
        RTS_STAGE5_VALIDATION_LOG_ROOT = $logRoot
        RTS_STAGE5_VALIDATION_DUMP_ROOT = $dumpRoot
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionFull
    }
    $registryValues = New-Object 'Collections.Generic.List[object]'
    if ($Title -ceq 'Generals') {
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    else {
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    return [pscustomobject]@{
        schemaVersion = 1
        title = $Title
        sessionRoot = $sessionFull
        runtimeDirectory = $runtimeFull
        documentsRoot = $documentsRoot
        profileLeaf = $profileLeaf
        profileRoot = $profileRoot
        peerRoot = $peerRoot
        profileConcurrency = 'shared-title-profile-read-only'
        environmentValues = $environmentValues
        environmentVariableNames = @($environmentValues.Keys)
        registryViews = @('Registry32', 'Registry64')
        registryValues = $registryValues.ToArray()
    }
}

function Set-LockstepProcessEnvironment {
    param([Collections.IDictionary]$Values)
    Initialize-LockstepSuspendedProcessNative
    $snapshot = [ordered]@{}
    try {
    foreach ($name in $Values.Keys) {
        $key = [string]$name
        $snapshot[$key] = [Stage5ValidationNative.SuspendedChild]::ReadEnvironmentValue($key)
        if ($null -eq $Values[$name]) {
            [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue($key, [NullString]::Value)
        }
        else {
            [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue($key, [string]$Values[$name])
        }
    }
    }
    catch {
        $setupFailure = $_
        try { Restore-LockstepProcessEnvironment $snapshot }
        catch {
            throw "Lockstep environment setup failed: $($setupFailure.Exception.Message); rollback failed: $($_.Exception.Message)"
        }
        throw $setupFailure
    }
    return $snapshot
}

function Restore-LockstepProcessEnvironment {
    param([Collections.IDictionary]$Snapshot)
    Initialize-LockstepSuspendedProcessNative
    $errors = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $Snapshot.Keys) {
        try {
            if ($null -eq $Snapshot[$name]) {
                [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue([string]$name,
                    [NullString]::Value)
            }
            else {
                [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue([string]$name,
                    [string]$Snapshot[$name])
            }
        }
        catch { $errors.Add("${name}: $($_.Exception.Message)") | Out-Null }
    }
    if ($errors.Count -gt 0) {
        throw "Lockstep-v2 process environment restoration failed: $($errors.ToArray() -join ' | ')"
    }
}

function Get-LockstepPeerEnvironment {
    param([pscustomobject]$Contract, [int]$Peer)
    $peerRoot = Join-Path $Contract.peerRoot "peer-$Peer"
    $values = [ordered]@{}
    foreach ($name in $Contract.environmentValues.Keys) {
        $key = [string]$name
        $baseValue = [string]$Contract.environmentValues[$key]
        if ($key -ceq 'TEMP' -or $key -ceq 'TMP' -or
            $key -ceq 'LOCALAPPDATA' -or $key -ceq 'APPDATA' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_CACHE_ROOT' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_LOG_ROOT' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_DUMP_ROOT') {
            $values[$key] = Join-Path $peerRoot ([IO.Path]::GetFileName($baseValue))
        }
        else { $values[$key] = $baseValue }
    }
    return [pscustomobject]@{
        peer = $Peer
        root = $peerRoot
        values = $values
        variableNames = @($values.Keys)
    }
}

function Get-LockstepRecoveryRegistryValueState {
    param([string]$View, [string]$SubKey, [string]$Name)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    try {
        $key = $base.OpenSubKey($SubKey, $false)
        if ($null -eq $key) { return [pscustomobject]@{ exists = $false; value = $null; kind = $null } }
        try {
            if (@($key.GetValueNames()) -notcontains $Name) {
                return [pscustomobject]@{ exists = $false; value = $null; kind = $null }
            }
            return [pscustomobject]@{
                exists = $true
                value = $key.GetValue($Name, $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                kind = $key.GetValueKind($Name)
            }
        }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function Get-LockstepRecoveryRegistryKeyState {
    param([string]$View, [string]$SubKey)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    try {
        $key = $base.OpenSubKey($SubKey, $false)
        if ($null -eq $key) { return [pscustomobject]@{ exists = $false; valueNames = @(); subKeyNames = @() } }
        try {
            return [pscustomobject]@{ exists = $true; valueNames = @($key.GetValueNames()); subKeyNames = @($key.GetSubKeyNames()) }
        }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function Set-LockstepRecoveryRegistryValue {
    param([string]$View, [string]$SubKey, [string]$Name, [object]$Value, [int]$Kind)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    try {
        $key = $base.OpenSubKey($SubKey, $true)
        if ($null -eq $key) { throw "Recovery could not open registry key '$SubKey'." }
        try { $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]$Kind) }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function Remove-LockstepRecoveryRegistryValue {
    param([string]$View, [string]$SubKey, [string]$Name)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    try {
        $key = $base.OpenSubKey($SubKey, $true)
        if ($null -eq $key) { return }
        try { $key.DeleteValue($Name, $false) }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function Remove-LockstepRecoveryRegistryKey {
    param([string]$View, [string]$SubKey)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    try {
        $separator = $SubKey.LastIndexOf('\')
        if ($separator -lt 0) { return }
        $parent = $SubKey.Substring(0, $separator)
        $leaf = $SubKey.Substring($separator + 1)
        $key = $base.OpenSubKey($parent, $true)
        if ($null -eq $key) { return }
        try { $key.DeleteSubKey($leaf, $false) }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function New-LockstepRecoveryInstallPathSnapshot {
    param([string]$Title, [string]$View, [string]$RuntimeValue)
    $subKey = if ($Title -ceq 'Generals') {
        'Software\Electronic Arts\EA Games\Generals'
    } else {
        'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser,
        [Microsoft.Win32.RegistryView]::$View)
    $created = New-Object 'Collections.Generic.List[string]'
    try {
        $current = ''
        foreach ($segment in @($subKey.Split('\'))) {
            $current = if ([string]::IsNullOrEmpty($current)) { $segment } else { $current + '\' + $segment }
            $probe = $base.OpenSubKey($current, $false)
            if ($null -ne $probe) { $probe.Dispose(); continue }
            $created.Add($current) | Out-Null
        }
    }
    finally { $base.Dispose() }
    $state = Get-LockstepRecoveryRegistryValueState $View $subKey 'InstallPath'
    $snapshot = New-Stage5RegistryRecoverySnapshot -Title $Title -View $View `
        -SubKey $subKey -Name 'InstallPath' -HadKey ($created.Count -eq 0) `
        -HadValue ([bool]$state.exists) -OldValue $state.value -OldKind $state.kind `
        -ExpectedValue $RuntimeValue -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -CreatedSubKeys @($created.ToArray())
    return [pscustomobject]@{ snapshot = $snapshot; createdSubKeys = @($created.ToArray()) }
}

function New-LockstepRecoveryAdapter {
    return [ordered]@{
        GetValue = { param($view, $subKey, $name) Get-LockstepRecoveryRegistryValueState $view $subKey $name }
        GetKey = { param($view, $subKey) Get-LockstepRecoveryRegistryKeyState $view $subKey }
        SetValue = { param($view, $subKey, $name, $value, $kind) Set-LockstepRecoveryRegistryValue $view $subKey $name $value $kind }
        DeleteValue = { param($view, $subKey, $name) Remove-LockstepRecoveryRegistryValue $view $subKey $name }
        DeleteKey = { param($view, $subKey) Remove-LockstepRecoveryRegistryKey $view $subKey }
    }
}

function New-LockstepRecoveryPendingIdentity {
    param([string]$Executable, [string]$ExecutableSha256)
    return [pscustomobject]@{
        launchPending = $true
        processId = 0
        creationTimeUtc100ns = 0
        executablePath = [IO.Path]::GetFullPath($Executable)
        executableSha256 = $ExecutableSha256
        exitProven = $false
    }
}

function Initialize-LockstepSuspendedProcessNative {
    if ($null -eq ('Stage5ValidationNative.SuspendedChild' -as [type])) {
        # Embedded so installed validation modules have no extra source-file dependency.
        Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Stage5ValidationNative
{
    // A native owner is never placed in Exception.Data: .NET Framework rejects
    // nonserializable values there. This local typed carrier retains ownership
    // through PowerShell's MethodInvocationException without serializing handles.
    public sealed class SuspendedLaunchException : Exception
    {
        public SuspendedChild RetainedChild { get; private set; }
        public uint CreatedProcessId { get; private set; }
        public SuspendedLaunchException(Exception operation, Exception cleanup,
            SuspendedChild child, uint pid)
            : base("Suspended launch failed and original-handle cleanup is unproven.",
                new AggregateException(operation, cleanup))
        {
            RetainedChild = child;
            CreatedProcessId = pid;
        }
    }
    public sealed class Identity
    {
        public int ProcessId { get; internal set; }
        public long CreationTimeUtc100ns { get; internal set; }
        public string ExecutablePath { get; internal set; }
        public string ExecutableSha256 { get; internal set; }
    }

    internal sealed class KernelHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        internal KernelHandle(IntPtr value) : base(true) { SetHandle(value); }
        internal void Attach(IntPtr value)
        {
            if (!IsInvalid || IsClosed || value == IntPtr.Zero || value == new IntPtr(-1))
                throw new InvalidOperationException("Native handle ownership cannot be reassigned.");
            SetHandle(value);
        }
        protected override bool ReleaseHandle() { return Native.CloseHandle(handle); }
    }

    internal static class Native
    {
        [StructLayout(LayoutKind.Sequential)]
        internal struct SecurityAttributes
        {
            internal int Length;
            internal IntPtr Descriptor;
            [MarshalAs(UnmanagedType.Bool)] internal bool Inherit;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct StartupInfo
        {
            internal int Size;
            internal string Reserved, Desktop, Title;
            internal uint X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
            internal ushort Show, ReservedSize;
            internal IntPtr ReservedBytes, Input, Output, Error;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct StartupInfoEx
        {
            internal StartupInfo Startup;
            internal IntPtr Attributes;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct ProcessInformation
        {
            internal IntPtr Process, Thread;
            internal uint ProcessId, ThreadId;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct FileTime { internal uint Low, High; }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        internal static extern IntPtr CreateFileW(string path, uint access, uint share,
            ref SecurityAttributes security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool InitializeProcThreadAttributeList(IntPtr list,
            int count, int flags, ref IntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags,
            IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
        [DllImport("kernel32.dll")]
        internal static extern void DeleteProcThreadAttributeList(IntPtr list);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CreateProcessW(string application, StringBuilder command,
            IntPtr processSecurity, IntPtr threadSecurity,
            [MarshalAs(UnmanagedType.Bool)] bool inherit, uint flags, IntPtr environment,
            string directory, ref StartupInfoEx startup, out ProcessInformation information);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint GetProcessId(IntPtr process);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetProcessTimes(IntPtr process, out FileTime creation,
            out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags,
            StringBuilder path, ref uint length);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint ResumeThread(IntPtr thread);
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetExitCodeProcess(IntPtr process, out uint code);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool TerminateProcess(IntPtr process, uint code);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        internal static extern uint GetEnvironmentVariableW(string name, StringBuilder value, uint size);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetEnvironmentVariableW(string name, string value);
        [DllImport("kernel32.dll", ExactSpelling = true)]
        internal static extern void SetLastError(uint error);
    }

    public sealed class SuspendedChild : IDisposable
    {
        private KernelHandle process, thread;
        private FileStream imagePin;
        private bool resumeAttempted, resumed, disposed;
        private Identity identity;
        private uint createdPid;

        private SuspendedChild(FileStream pin)
        {
            process = new KernelHandle(IntPtr.Zero);
            thread = new KernelHandle(IntPtr.Zero);
            imagePin = pin;
        }
        public IntPtr Handle
        {
            get
            {
                if (disposed || process.IsClosed || process.IsInvalid)
                    throw new ObjectDisposedException("SuspendedChild");
                return process.DangerousGetHandle();
            }
        }
        public int Id { get { return checked((int)createdPid); } }
        public Identity ObservedIdentity { get { return identity; } }
        public bool HasResumed { get { return resumed; } }
        public bool HasExited { get { return WaitForExit(0); } }
        public int ExitCode
        {
            get
            {
                if (!HasExited) throw new InvalidOperationException("Child has not exited.");
                uint code;
                if (!Native.GetExitCodeProcess(Handle, out code)) throw Error("GetExitCodeProcess");
                return unchecked((int)code);
            }
        }
        private static Exception Error(string operation)
        {
            int code = Marshal.GetLastWin32Error();
            return new Win32Exception(code, operation + " failed (Win32 " + code + ").");
        }
        private static SafeFileHandle OpenInherited(string path, bool input)
        {
            Native.SecurityAttributes security = new Native.SecurityAttributes();
            security.Length = Marshal.SizeOf(typeof(Native.SecurityAttributes));
            security.Inherit = true;
            IntPtr value = Native.CreateFileW(path, input ? 0x80000000u : 0x40000000u,
                input ? 3u : 1u, ref security, input ? 3u : 1u, 0x80u, IntPtr.Zero);
            if (value == new IntPtr(-1)) throw Error("CreateFileW redirect");
            return new SafeFileHandle(value, true);
        }
        private static void ValidateEnvironmentName(string name)
        {
            if (String.IsNullOrEmpty(name) || name.IndexOf('\0') >= 0 || name.IndexOf('=') >= 0)
                throw new ArgumentException("Invalid environment variable name.");
        }
        public static string ReadEnvironmentValue(string name)
        {
            ValidateEnvironmentName(name);
            StringBuilder value = new StringBuilder(32768);
            Native.SetLastError(0);
            uint length = Native.GetEnvironmentVariableW(name, value, (uint)value.Capacity);
            int error = Marshal.GetLastWin32Error();
            if (length == 0)
            {
                if (error == 203) return null;
                if (error == 0) return String.Empty;
                throw new Win32Exception(error, "Environment snapshot failed.");
            }
            if (length >= value.Capacity) throw new InvalidOperationException("Environment value exceeds its bound.");
            return value.ToString();
        }
        public static void WriteEnvironmentValue(string name, string value)
        {
            ValidateEnvironmentName(name);
            if (value != null && value.IndexOf('\0') >= 0)
                throw new ArgumentException("Invalid environment variable value.");
            if (!Native.SetEnvironmentVariableW(name, value))
                throw Error("SetEnvironmentVariableW");
        }
        private static string EnvironmentBlock(IDictionary environment)
        {
            SortedDictionary<string, string> values =
                new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (DictionaryEntry item in environment)
            {
                string key = item.Key as string;
                string value = item.Value as string;
                if (String.IsNullOrEmpty(key) || key.IndexOf('\0') >= 0 ||
                    key.IndexOf('=') >= 0 || value == null || value.IndexOf('\0') >= 0)
                    throw new ArgumentException("Invalid environment entry.");
                values.Add(key, value);
            }
            StringBuilder block = new StringBuilder();
            foreach (KeyValuePair<string, string> item in values)
                block.Append(item.Key).Append('=').Append(item.Value).Append('\0');
            block.Append('\0');
            return block.ToString();
        }

        public static SuspendedChild Create(string executable, string arguments,
            string directory, string stdout, string stderr, IDictionary environment,
            string expectedSha256)
        {
            executable = Path.GetFullPath(executable);
            directory = Path.GetFullPath(directory);
            stdout = Path.GetFullPath(stdout);
            stderr = Path.GetFullPath(stderr);
            if (String.Equals(stdout, stderr, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Stdout and stderr must be distinct.");
            if (arguments == null || arguments.IndexOf('\0') >= 0)
                throw new ArgumentException("Invalid command arguments.");
            FileStream pin = new FileStream(executable, FileMode.Open, FileAccess.Read, FileShare.Read);
            SuspendedChild child = null;
            Native.ProcessInformation info = new Native.ProcessInformation();
            IntPtr attributes = IntPtr.Zero, handles = IntPtr.Zero, environmentPointer = IntPtr.Zero;
            bool attributesInitialized = false, created = false;
            try
            {
                using (SafeFileHandle input = OpenInherited("NUL", true))
                using (SafeFileHandle output = OpenInherited(stdout, false))
                using (SafeFileHandle error = OpenInherited(stderr, false))
                {
                    IntPtr size = IntPtr.Zero;
                    Native.InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
                    if (size == IntPtr.Zero) throw Error("Attribute-list sizing");
                    attributes = Marshal.AllocHGlobal(size);
                    if (!Native.InitializeProcThreadAttributeList(attributes, 1, 0, ref size))
                        throw Error("InitializeProcThreadAttributeList");
                    attributesInitialized = true;
                    handles = Marshal.AllocHGlobal(IntPtr.Size * 3);
                    Marshal.WriteIntPtr(handles, 0, input.DangerousGetHandle());
                    Marshal.WriteIntPtr(handles, IntPtr.Size, output.DangerousGetHandle());
                    Marshal.WriteIntPtr(handles, IntPtr.Size * 2, error.DangerousGetHandle());
                    if (!Native.UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002),
                        handles, new IntPtr(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero))
                        throw Error("UpdateProcThreadAttribute HANDLE_LIST");
                    Native.StartupInfoEx startup = new Native.StartupInfoEx();
                    startup.Startup.Size = Marshal.SizeOf(typeof(Native.StartupInfoEx));
                    startup.Startup.Flags = 0x101; // USESTDHANDLES | USESHOWWINDOW
                    startup.Startup.Show = 0; // SW_HIDE
                    startup.Startup.Input = input.DangerousGetHandle();
                    startup.Startup.Output = output.DangerousGetHandle();
                    startup.Startup.Error = error.DangerousGetHandle();
                    startup.Attributes = attributes;
                    environmentPointer = Marshal.StringToHGlobalUni(EnvironmentBlock(environment));
                    StringBuilder command = new StringBuilder("\"" + executable + "\" " + arguments);
                    // Allocate all managed native-owner carriers before creation.
                    child = new SuspendedChild(pin);
                    pin = null;
                    const uint flags = 0x00000004u | 0x08000000u | 0x00080000u | 0x00000400u;
                    if (!Native.CreateProcessW(executable, command, IntPtr.Zero, IntPtr.Zero,
                        true, flags, environmentPointer, directory, ref startup, out info))
                        throw Error("CreateProcessW suspended");
                    created = true;
                    child.createdPid = info.ProcessId;
                    child.process.Attach(info.Process);
                    child.thread.Attach(info.Thread);
                    child.CaptureIdentity(info.ProcessId, executable, expectedSha256);
                    return child;
                }
            }
            catch (Exception operationFailure)
            {
                try
                {
                    if (child != null && created)
                    {
                        child.Kill();
                        if (!child.WaitForExit(5000))
                            throw new InvalidOperationException("Suspended child cleanup timed out.");
                        child.Dispose();
                    }
                    else if (child != null)
                    {
                        child.Dispose();
                    }
                }
                catch (Exception cleanupFailure)
                {
                    throw new SuspendedLaunchException(operationFailure, cleanupFailure,
                        child, created ? info.ProcessId : 0);
                }
                throw;
            }
            finally
            {
                if (attributesInitialized) Native.DeleteProcThreadAttributeList(attributes);
                if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
                if (handles != IntPtr.Zero) Marshal.FreeHGlobal(handles);
                if (environmentPointer != IntPtr.Zero) Marshal.FreeHGlobal(environmentPointer);
                if (pin != null) pin.Dispose();
            }
        }

        private void CaptureIdentity(uint expectedPid, string expectedPath, string expectedHash)
        {
            uint pid = Native.GetProcessId(Handle);
            if (pid == 0) throw Error("GetProcessId");
            if (pid != expectedPid) throw new InvalidOperationException("Created process identity differs.");
            Native.FileTime creation, exit, kernel, user;
            if (!Native.GetProcessTimes(Handle, out creation, out exit, out kernel, out user))
                throw Error("GetProcessTimes");
            ulong time = ((ulong)creation.High << 32) | creation.Low;
            if (time == 0 || time > Int64.MaxValue) throw new InvalidOperationException("Invalid creation FILETIME.");
            StringBuilder path = new StringBuilder(32768);
            uint length = (uint)path.Capacity;
            if (!Native.QueryFullProcessImageNameW(Handle, 0, path, ref length))
                throw Error("QueryFullProcessImageNameW while suspended");
            if (length == 0 || length >= path.Capacity) throw new InvalidOperationException("Invalid image path length.");
            string observedPath = Path.GetFullPath(path.ToString());
            if (!String.Equals(observedPath, expectedPath, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Observed image path differs from pinned executable.");
            string hash;
            using (SHA256 sha = SHA256.Create())
                hash = BitConverter.ToString(sha.ComputeHash(imagePin)).Replace("-", "");
            if (!String.Equals(hash, expectedHash, StringComparison.Ordinal))
                throw new InvalidOperationException("Observed pinned executable hash differs.");
            identity = new Identity { ProcessId = checked((int)pid), CreationTimeUtc100ns = checked((long)time),
                ExecutablePath = observedPath, ExecutableSha256 = hash };
        }

        public void Resume()
        {
            if (resumeAttempted || identity == null) throw new InvalidOperationException("Resume requires one captured, unresumed child.");
            resumeAttempted = true;
            uint previous = Native.ResumeThread(thread.DangerousGetHandle());
            if (previous == UInt32.MaxValue) throw Error("ResumeThread");
            if (previous != 1) throw new InvalidOperationException("ResumeThread prior suspension count was not one.");
            resumed = true;
            thread.Dispose();
            imagePin.Dispose();
            imagePin = null;
        }
        public bool WaitForExit(int milliseconds)
        {
            if (milliseconds < 0) throw new ArgumentOutOfRangeException("milliseconds");
            uint result = Native.WaitForSingleObject(Handle, (uint)milliseconds);
            if (result == 0) return true;
            if (result == 258) return false;
            throw Error("WaitForSingleObject");
        }
        public void Kill()
        {
            if (HasExited) return;
            if (!Native.TerminateProcess(Handle, 1))
            {
                Exception failure = Error("TerminateProcess");
                if (!HasExited) throw failure;
            }
        }
        public void Dispose()
        {
            if (disposed) return;
            if (!process.IsInvalid && !HasExited)
                throw new InvalidOperationException("Live child must be stopped before original handles are disposed.");
            if (imagePin != null) { imagePin.Dispose(); imagePin = null; }
            if (thread != null) thread.Dispose();
            if (process != null) process.Dispose();
            disposed = true;
        }
    }
}
'@ -ErrorAction Stop
    }
}

function New-LockstepSuspendedChild {
    param([string]$Executable, [string]$Arguments, [string]$WorkingDirectory,
        [string]$Stdout, [string]$Stderr, [string]$ExecutableSha256)
    Initialize-LockstepSuspendedProcessNative
    return [Stage5ValidationNative.SuspendedChild]::Create(
        $Executable, $Arguments, $WorkingDirectory, $Stdout, $Stderr,
        [Environment]::GetEnvironmentVariables(), $ExecutableSha256.ToUpperInvariant())
}

function Get-LockstepRetainedFailedChild {
    param([Exception]$Failure)
    $cursor = $Failure
    for ($depth = 0; $null -ne $cursor -and $depth -lt 32; ++$depth) {
        if ($cursor.GetType().FullName -ceq 'Stage5ValidationNative.SuspendedLaunchException') {
            $child = $cursor.RetainedChild
            if ($null -eq $child -or
                $child.GetType().FullName -cne 'Stage5ValidationNative.SuspendedChild') {
                throw 'Suspended-launch failure contains an invalid retained child owner.'
            }
            return $child
        }
        $cursor = $cursor.InnerException
    }
    return $null
}

function New-LockstepRecoveryStartedIdentity {
    param([object]$Process, [string]$ExecutableSha256)
    if ($null -eq $Process -or
        $Process.GetType().FullName -cne 'Stage5ValidationNative.SuspendedChild') {
        throw 'Lockstep identity requires the original owned suspended-launch adapter.'
    }
    $observed = $Process.ObservedIdentity
    $path = [IO.Path]::GetFullPath($observed.ExecutablePath)
    $creation = [Int64]$observed.CreationTimeUtc100ns
    if ($observed.ProcessId -le 0 -or $creation -le 0) {
        throw 'Lockstep process identity did not provide a positive PID and creation FILETIME.'
    }
    $actualHash = [string]$observed.ExecutableSha256
    if ($actualHash -cne ([string]$ExecutableSha256).ToUpperInvariant()) {
        throw 'Lockstep process identity executable hash does not match the requested installed executable.'
    }
    return [pscustomobject]@{
        launchPending = $false
        processId = [int]$observed.ProcessId
        creationTimeUtc100ns = $creation
        executablePath = $path
        executableSha256 = $actualHash
        processCreationUtc = [DateTime]::FromFileTimeUtc($creation).ToString(
            'o', [Globalization.CultureInfo]::InvariantCulture)
        exitProven = $false
    }
}

function Test-LockstepRecoveryIdentitiesExited {
    param([object[]]$Identities)
    return @($Identities | Where-Object {
        [bool]$_.launchPending -or -not [bool]$_.exitProven
    }).Count -eq 0
}

function New-LockstepRecoveryAuthorizationIdentities {
    param([object[]]$Identities)
    return @($Identities | ForEach-Object {
        [pscustomobject]@{
            launchPending = [bool]$_.launchPending
            processId = [int]$_.processId
            creationTimeUtc100ns = [Int64]$_.creationTimeUtc100ns
            executablePath = [string]$_.executablePath
            executableSha256 = [string]$_.executableSha256
            exitProven = [bool]$_.exitProven
        }
    })
}

function Test-LockstepNoActiveTitleProcesses {
    param([string[]]$Executables)
    foreach ($executable in @($Executables | Sort-Object -Unique)) {
        $expected = [IO.Path]::GetFullPath($executable)
        $name = [IO.Path]::GetFileNameWithoutExtension($expected)
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try {
                if ([String]::Equals([IO.Path]::GetFullPath($process.Path),
                        $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    return $false
                }
            }
            catch {
                # An inaccessible same-name process prevents an exact
                # no-active-title proof; fail closed.
                return $false
            }
        }
    }
    return $true
}

function Get-LockstepExecutionEnvironment {
    param(
        [pscustomobject]$PeerEnvironment,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [pscustomobject]$RuntimeClosure
    )
    Assert-LockstepCanonicalUuid $CohortNonce 'Lockstep-v2 execution cohortNonce' | Out-Null
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse($CohortCreatedUtc, [ref]$cohortCreated)) {
        throw 'Lockstep-v2 execution cohortCreatedUtc is invalid.'
    }
    $closure = Assert-LockstepRuntimeClosure $RuntimeClosure 'Lockstep-v2 execution'
    $values = [ordered]@{}
    foreach ($key in $PeerEnvironment.values.Keys) {
        $values[[string]$key] = [string]$PeerEnvironment.values[$key]
    }
    # These dynamic bindings are intentionally kept out of the reviewed
    # environment-equivalence list: they identify this fresh run cohort and
    # cannot be replaced by a caller's static profile fixture.
    $values['RTS_STAGE5_COHORT_NONCE'] = $CohortNonce
    $values['RTS_STAGE5_COHORT_CREATED_UTC'] = $CohortCreatedUtc
    $values['RTS_STAGE5_RUNTIME_MANIFEST_SHA256'] = $closure.dependencyManifestSha256
    $values['RTS_STAGE5_RUNTIME_CLOSURE_SHA256'] = $closure.closureSha256
    return $values
}

function Set-LockstepRegistryValue {
    param(
        [Microsoft.Win32.RegistryView]$View,
        [string]$SubKey,
        [string]$Name,
        [string]$Value,
        [Collections.Generic.List[object]]$Snapshots,
        [Collections.IDictionary]$SnapshotKeys
    )
    $snapshotKey = "$View|$SubKey|$Name"
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    try {
        $target = $base.OpenSubKey($SubKey, $true)
        $createdSubKeys = New-Object 'Collections.Generic.List[string]'
        if ($null -eq $target) {
            $readOnlyTarget = $base.OpenSubKey($SubKey, $false)
            if ($null -ne $readOnlyTarget) {
                $readOnlyTarget.Dispose()
                throw "Registry key '$SubKey' is not writable for $View."
            }
            $current = ''
            foreach ($segment in $SubKey.Split('\')) {
                $current = if ([string]::IsNullOrEmpty($current)) {
                    $segment
                } else { $current + '\' + $segment }
                $existing = $base.OpenSubKey($current, $false)
                if ($null -ne $existing) {
                    $existing.Dispose()
                    continue
                }
                $created = $base.CreateSubKey($current)
                if ($null -eq $created) {
                    throw "Could not create registry key '$current' for $View."
                }
                $created.Dispose()
                $createdSubKeys.Add($current) | Out-Null
            }
            $target = $base.OpenSubKey($SubKey, $true)
            if ($null -eq $target) {
                throw "Could not reopen registry key '$SubKey' for $View."
            }
        }
        try {
            if (-not $SnapshotKeys.Contains($snapshotKey)) {
                $hadValue = @($target.GetValueNames()) -contains $Name
                $oldValue = $null
                $oldKind = $null
                if ($hadValue) {
                    $oldValue = $target.GetValue($Name, $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $oldKind = $target.GetValueKind($Name)
                }
                $Snapshots.Add([pscustomobject]@{
                    view = $View; subKey = $SubKey; name = $Name
                    hadKey = $createdSubKeys.Count -eq 0
                    hadValue = $hadValue; oldValue = $oldValue; oldKind = $oldKind
                    createdSubKeys = $createdSubKeys.ToArray()
                }) | Out-Null
                $SnapshotKeys[$snapshotKey] = $true
            }
            $target.SetValue($Name, $Value,
                [Microsoft.Win32.RegistryValueKind]::String)
        }
        finally { $target.Dispose() }
    }
    finally { $base.Dispose() }
}

function Remove-LockstepEmptyRegistryKeys {
    param([Microsoft.Win32.RegistryKey]$Base, [object[]]$CreatedSubKeys)
    foreach ($path in @($CreatedSubKeys | Sort-Object Length -Descending -Unique)) {
        $key = $Base.OpenSubKey([string]$path, $false)
        if ($null -eq $key) { continue }
        try {
            if (@($key.GetValueNames()).Count -ne 0 -or
                @($key.GetSubKeyNames()).Count -ne 0) { continue }
        }
        finally { $key.Dispose() }
        $separator = ([string]$path).LastIndexOf('\')
        if ($separator -lt 0) {
            $Base.DeleteSubKey([string]$path, $false)
        }
        else {
            $parent = ([string]$path).Substring(0, $separator)
            $leaf = ([string]$path).Substring($separator + 1)
            $parentKey = $Base.OpenSubKey($parent, $true)
            if ($null -ne $parentKey) {
                try { $parentKey.DeleteSubKey($leaf, $false) }
                finally { $parentKey.Dispose() }
            }
        }
    }
}

function Restore-LockstepRegistrySnapshots {
    param([object[]]$Snapshots)
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $Snapshots.Count - 1; $index -ge 0; --$index) {
        $snapshot = $Snapshots[$index]
        $base = $null
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser, $snapshot.view)
            $target = $base.OpenSubKey($snapshot.subKey, $true)
            if ($snapshot.hadKey -and $null -eq $target) {
                throw "Pre-existing registry key '$($snapshot.subKey)' disappeared before restoration."
            }
            if ($null -ne $target) {
                try {
                    if ($snapshot.hadValue) {
                        $target.SetValue($snapshot.name, $snapshot.oldValue,
                            $snapshot.oldKind)
                    }
                    elseif (@($target.GetValueNames()) -contains $snapshot.name) {
                        $target.DeleteValue($snapshot.name, $false)
                    }
                }
                finally { $target.Dispose() }
            }
            if (-not $snapshot.hadKey) {
                Remove-LockstepEmptyRegistryKeys $base $snapshot.createdSubKeys
            }
        }
        catch { $errors.Add("$($snapshot.view)/$($snapshot.subKey)/$($snapshot.name): $($_.Exception.Message)") | Out-Null }
        finally {
            if ($null -ne $base) { $base.Dispose() }
        }
    }
    if ($errors.Count -gt 0) {
        throw "Lockstep-v2 registry restoration failed after every snapshot: $($errors.ToArray() -join ' | ')"
    }
}

function Initialize-LockstepTitleSessionDirectories {
    param([pscustomobject]$Contract)
    $selfTestRoot = [string]$script:LockstepHostSelfTestScratchRoot
    foreach ($directory in @(
        $Contract.sessionRoot, $Contract.documentsRoot, $Contract.profileRoot,
        $Contract.peerRoot, $Contract.environmentValues['TEMP'],
        $Contract.environmentValues['TMP'],
        $Contract.environmentValues['LOCALAPPDATA'],
        $Contract.environmentValues['APPDATA'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_CACHE_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_LOG_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_DUMP_ROOT'])) {
        if (-not [string]::IsNullOrWhiteSpace($selfTestRoot)) {
            Ensure-LockstepHostSelfTestDirectory $directory 'self-test session directory'
            continue
        }
        if (-not (Test-SafeHDirectory $directory -AllowWhitespace)) {
            throw "Lockstep-v2 title-session directory is not a safe H: path: $directory"
        }
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
}

function Assert-LockstepProfileReadOnly {
    param([string]$ProfileRoot)
    if (-not (Test-SafeHDirectory $ProfileRoot -AllowWhitespace) -or
        -not (Test-Path -LiteralPath $ProfileRoot -PathType Container)) {
        throw "Lockstep-v2 profile root disappeared: $ProfileRoot"
    }
    $files = @(Get-ChildItem -LiteralPath $ProfileRoot -File -Force -Recurse)
    if ($files.Count -gt 0) {
        throw "Lockstep-v2 shared title profile was written during concurrent qualification: $ProfileRoot"
    }
    return @($files)
}

function Remove-LockstepTitleSessionDirectories {
    param(
        [pscustomobject]$Contract,
        [string]$OutputDirectory
    )
    if ($null -eq $Contract -or [string]::IsNullOrWhiteSpace($Contract.sessionRoot)) {
        throw 'Lockstep-v2 disposable title-session cleanup requires a session contract.'
    }
    $outputFull = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
    $sessionFull = [IO.Path]::GetFullPath([string]$Contract.sessionRoot).TrimEnd('\')
    if (-not (Test-SafeHDirectory $outputFull) -or
        -not (Test-SafeHDirectory $sessionFull) -or
        [IO.Path]::GetFileName($sessionFull) -cne 'TitleSession') {
        throw "Lockstep-v2 disposable title-session path is not bounded: $sessionFull"
    }
    $outputPrefix = $outputFull + '\'
    if (-not $sessionFull.StartsWith($outputPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Lockstep-v2 disposable title-session path escapes the output root: $sessionFull"
    }
    $rootItem = Get-Item -LiteralPath $sessionFull -Force `
        -ErrorAction SilentlyContinue
    if ($null -eq $rootItem) { return }
    $selfTestRoot = [string]$script:LockstepHostSelfTestScratchRoot
    if (-not [string]::IsNullOrWhiteSpace($selfTestRoot) -and
        ($sessionFull -ceq $selfTestRoot -or
         $sessionFull.StartsWith($selfTestRoot + '\',
             [StringComparison]::OrdinalIgnoreCase))) {
        Remove-LockstepHostSelfTestTree $sessionFull $outputFull
        return
    }
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Lockstep-v2 disposable title-session root is a reparse point: $sessionFull"
    }
    Remove-Item -LiteralPath $sessionFull -Recurse -Force
    if (Test-Path -LiteralPath $sessionFull) {
        throw "Lockstep-v2 disposable title-session cleanup did not remove: $sessionFull"
    }
}

function Read-AndValidateArtifactSet {
    param([string]$Manifest, [string]$ExpectedSourceCommit)
    $full = [IO.Path]::GetFullPath($Manifest)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Artifact-set manifest was not found: $full"
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5JsonShape $document @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'artifacts', 'runtimeClosure') 'Artifact set manifest'
    if (-not (Test-Stage5JsonInteger (Get-Stage5JsonValue $document `
            'schemaVersion' 'Artifact set manifest')) -or
        (Get-Stage5JsonValue $document 'schemaVersion' 'Artifact set manifest') -ne 1 -or
        (Get-Stage5JsonValue $document 'sourceCommit' 'Artifact set manifest') -cne
            $ExpectedSourceCommit -or
        (Get-Stage5JsonValue $document 'architecture' 'Artifact set manifest') -cne 'x64') {
        throw 'Artifact-set identity does not match the requested native x64 source revision.'
    }
    $productSet = Get-Stage5JsonValue $document 'productSet' 'Artifact set manifest'
    if ($productSet -isnot [Array] -or $productSet.Count -ne 2 -or
        -not ($productSet -ccontains 'Generals') -or
        -not ($productSet -ccontains 'ZeroHour')) {
        throw 'Artifact set must contain exactly Generals and ZeroHour.'
    }
    # The six launcher-facing entries are not the complete installed runtime.
    # Reuse the authoritative evidence-module validator so this producer binds
    # every declared DLL/asset as well as the dependency manifest and closure
    # digest before any lockstep process is launched.
    $artifactDirectory = Split-Path -Parent $full
    $runtimeClosureBinding = Get-Stage5RuntimeClosureBinding `
        -ArtifactSet $document `
        -ArtifactDirectory $artifactDirectory `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -Context 'Artifact set runtime closure'
    $runtimeClosure = [pscustomobject]@{
        dependencyManifestSha256 = [string]$runtimeClosureBinding.dependencyManifestSha256
        closureSha256 = [string]$runtimeClosureBinding.closureSha256
    }
    $requiredRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifacts = Get-Stage5JsonValue $document 'artifacts' 'Artifact set manifest'
    if ($artifacts -isnot [Array] -or $artifacts.Count -ne $requiredRoles.Count) {
        throw 'Artifact set must contain exactly six installed product artifacts.'
    }
    $resolved = @{}
    foreach ($entry in $artifacts) {
        Assert-Stage5JsonShape $entry @('role', 'path', 'sha256') 'Artifact entry'
        $role = [string](Get-Stage5JsonValue $entry 'role' 'Artifact entry')
        if (-not ($requiredRoles -ccontains $role) -or $resolved.ContainsKey($role)) {
            throw "Artifact role is missing, duplicated, or unsupported: $role"
        }
        $path = Resolve-BoundedArtifactPath $full `
            ([string](Get-Stage5JsonValue $entry 'path' 'Artifact entry'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Installed artifact is missing: $path"
        }
        if ($role -eq 'generals-executable' -and
            [IO.Path]::GetFileName($path) -notmatch '^generalsv(?:-[A-Za-z0-9._-]+)?\.exe$') {
            throw "Generals artifact is not a native installed executable: $path"
        }
        if ($role -eq 'zerohour-executable' -and
            [IO.Path]::GetFileName($path) -notmatch '^generalszh(?:-[A-Za-z0-9._-]+)?\.exe$') {
            throw "Zero Hour artifact is not a native installed executable: $path"
        }
        $expected = ([string](Get-Stage5JsonValue $entry 'sha256' 'Artifact entry')).ToUpperInvariant()
        $actual = Get-UpperSha256 $path
        if ($expected -notmatch '^[0-9A-F]{64}$' -or $actual -cne $expected) {
            throw "Installed artifact hash mismatch for role $role."
        }
        $resolved[$role] = [pscustomobject]@{ path = $path; sha256 = $actual }
    }
    foreach ($role in $requiredRoles) {
        if (-not $resolved.ContainsKey($role)) { throw "Missing artifact role: $role" }
    }
    return [pscustomobject]@{
        path = $full
        sha256 = Get-UpperSha256 $full
        artifacts = $resolved
        runtimeClosure = $runtimeClosure
        runtimeFiles = @($runtimeClosureBinding.files)
    }
}

function New-NonceHex {
    param([int]$Bytes = 16)
    $buffer = New-Object byte[] $Bytes
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($buffer) }
    finally { $generator.Dispose() }
    return ([BitConverter]::ToString($buffer) -replace '-', '')
}

function Write-AtomicText {
    param([string]$Path, [string]$Text)
    $temporary = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
    [IO.File]::WriteAllText($temporary, $Text,
        (New-Object Text.UTF8Encoding($false)))
    try { [IO.File]::Move($temporary, $Path) }
    catch {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        throw
    }
}

function Wait-ForLeaf {
    param([string]$Path, [object]$Process, [datetime]$Deadline)
    while ([datetime]::UtcNow -lt $Deadline) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) { return }
        if ($Process.HasExited) {
            throw "Peer process $($Process.Id) exited before publishing $Path (exit $($Process.ExitCode))."
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Timed out waiting for peer process $($Process.Id) to publish $Path."
}

function Stop-TaskPeer {
    param([object]$Process)
    if ($null -eq $Process) { return }
    try {
        if (-not $Process.HasExited) {
            try { $Process.Kill() }
            catch [InvalidOperationException] {
                if (-not $Process.HasExited) { throw }
            }
        }
        if (-not $Process.WaitForExit($PostKillWaitMilliseconds)) {
            throw "Peer process $($Process.Id) remained alive after bounded cleanup."
        }
        return [int]$Process.ExitCode
    }
    finally { $Process.Dispose() }
}

function New-LockstepProcessLifecycleRecord {
    param([object]$Process)
    return [pscustomobject]@{
        process = $Process
        identity = $null
        startProofPublished = $false
        exitProofPublished = $false
    }
}

function Publish-LockstepProcessExitProof {
    param(
        [Parameter(Mandatory = $true)][object]$Lifecycle,
        [AllowNull()][scriptblock]$Observer,
        [Parameter(Mandatory = $true)][int]$ExitCode,
        [switch]$SuppressErrors
    )
    if ($null -eq $Observer -or -not [bool]$Lifecycle.startProofPublished -or
        [bool]$Lifecycle.exitProofPublished -or $null -eq $Lifecycle.identity) {
        return $false
    }
    try {
        & $Observer ([pscustomobject]@{
            processIdentity = $Lifecycle.identity
            exitCode = $ExitCode
            exited = $true
        })
        $Lifecycle.identity.exitProven = $true
        $Lifecycle.exitProofPublished = $true
        return $true
    }
    catch {
        if (-not $SuppressErrors) { throw }
        return $false
    }
}

function Invoke-Stage5InstalledLockstepV2SessionSet {
    param(
        [Parameter(Mandatory = $true)][string]$GeneralsExecutable,
        [Parameter(Mandatory = $true)][string]$ZeroHourExecutable,
        [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$MapName,
        [Parameter(Mandatory = $true)][object]$MapCrcs,
        [ValidateRange(2, 2)][int]$PeerCount = 2,
        [ValidateRange(1024, 65000)][int]$BasePort = 41000,
        [ValidateRange(1, 2147483646)][int]$Seed = 23063,
        [ValidateRange(30, 1800)][int]$PeerTimeoutSeconds = 300,
        [string]$ExecutionCohortNonce = '',
        [string]$ExecutionCohortCreatedUtc = '',
        [Parameter(Mandatory = $true)][object]$ExpectedRuntimeClosure,
        [Parameter(Mandatory = $true)][object]$ValidationDataBinding,
        [Parameter(Mandatory = $true)][bool]$AllowHeadlessDirectExecution,
        [Parameter(Mandatory = $true)][string]$RunnerScriptPath
    )
    # Production callers must never inherit the bounded non-H self-test
    # exception from an earlier invocation in the same PowerShell process.
    Set-Stage5LockstepHostSelfTestScratchRoot $null
    Assert-HeadlessDirectExecutionOptIn $AllowHeadlessDirectExecution
    if ($SourceCommit -notmatch '^[0-9a-f]{40}$') {
        throw 'SourceCommit must be the exact lowercase 40-hex revision.'
    }
    if ($PeerCount -ne $LockstepNetworkPeerCount) {
        throw "Installed lockstep-v2 requires exactly $LockstepNetworkPeerCount network peers."
    }
    if ([string]::IsNullOrWhiteSpace($MapName) -or -not (Test-SafeMapName $MapName)) {
        throw 'Installed lockstep-v2 map name is not a bounded safe map identity.'
    }
    if ($null -eq $ValidationDataBinding) {
        throw 'Installed lockstep-v2 session requires an independently validated data binding.'
    }
    if (-not (Test-Path -LiteralPath $RunnerScriptPath -PathType Leaf)) {
        throw "Installed lockstep-v2 runner identity script was not found: $RunnerScriptPath"
    }
    if (-not (Test-SafeHDirectory $OutputDirectory) -or
            (Test-Path -LiteralPath $OutputDirectory)) {
        throw 'Installed lockstep-v2 output must be a fresh task-owned H: directory.'
    }
    $mapCrcs = Assert-LockstepMapCrcs $MapCrcs `
        'Requested lockstep-v2 map CRCs'
    $outputFull = [IO.Path]::GetFullPath($OutputDirectory)
    $artifactSet = Read-AndValidateArtifactSet $ArtifactSetManifestPath $SourceCommit
    $runtimeClosure = Assert-LockstepRuntimeClosure $artifactSet.runtimeClosure `
        'Artifact-set runtime closure'
    if ($null -eq $ExpectedRuntimeClosure -or
            [string]$ExpectedRuntimeClosure.dependencyManifestSha256 -cne
                $runtimeClosure.dependencyManifestSha256 -or
            [string]$ExpectedRuntimeClosure.closureSha256 -cne
                $runtimeClosure.closureSha256) {
        throw 'Installed lockstep-v2 runtime closure differs from the independently supplied binding.'
    }
    $generalsFull = [IO.Path]::GetFullPath($GeneralsExecutable)
    $zeroHourFull = [IO.Path]::GetFullPath($ZeroHourExecutable)
    if ($generalsFull -cne $artifactSet.artifacts['generals-executable'].path -or
            $zeroHourFull -cne $artifactSet.artifacts['zerohour-executable'].path) {
        throw 'Requested executables must be the exact installed x64 artifact-set executables.'
    }
    Assert-X64PeExecutable $generalsFull
    Assert-X64PeExecutable $zeroHourFull
    Assert-Stage5ProcessLocalProfileCapability $generalsFull `
        -Context 'Generals installed executable' | Out-Null
    Assert-Stage5ProcessLocalProfileCapability $zeroHourFull `
        -Context 'Zero Hour installed executable' | Out-Null
    if (-not (Test-LockstepNoActiveTitleProcesses @($generalsFull, $zeroHourFull))) {
        throw 'An installed Generals or Zero Hour title process is already running; refusing registry/profile setup.'
    }
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    $executables = [ordered]@{
        Generals = $artifactSet.artifacts['generals-executable'].sha256
        ZeroHour = $artifactSet.artifacts['zerohour-executable'].sha256
    }
    $launcherContracts = [ordered]@{
        Generals = Get-LauncherRunContract `
            $artifactSet.artifacts['generals-launcher-config'].path `
            $artifactSet.artifacts['generals-launcher'].path `
            (Split-Path -Parent $generalsFull) $generalsFull `
            $artifactSet.artifacts['generals-launcher-config'].sha256 `
            $artifactSet.artifacts['generals-launcher'].sha256
        ZeroHour = Get-LauncherRunContract `
            $artifactSet.artifacts['zerohour-launcher-config'].path `
            $artifactSet.artifacts['zerohour-launcher'].path `
            (Split-Path -Parent $zeroHourFull) $zeroHourFull `
            $artifactSet.artifacts['zerohour-launcher-config'].sha256 `
            $artifactSet.artifacts['zerohour-launcher'].sha256
    }
    if ([string]::IsNullOrWhiteSpace($ExecutionCohortNonce)) {
        $ExecutionCohortNonce = [Guid]::NewGuid().ToString('D')
    }
    if ([string]::IsNullOrWhiteSpace($ExecutionCohortCreatedUtc)) {
        $ExecutionCohortCreatedUtc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    Assert-LockstepCanonicalUuid $ExecutionCohortNonce `
        'Installed lockstep-v2 execution cohortNonce' | Out-Null
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse($ExecutionCohortCreatedUtc,
            [ref]$cohortCreated)) {
        throw 'Installed lockstep-v2 execution cohortCreatedUtc is invalid.'
    }
$sessionResults = @()
$negativeProbeResults = @()
$allPorts = @()
$usedNonces = @{}
$registrySnapshots = New-Object 'Collections.Generic.List[object]'
$registrySnapshotKeys = @{}
$registryRecoveryUserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$registryRecoveryLock = Enter-Stage5RegistryRecoveryMutex $registryRecoveryUserSid
$registryRecoveryRunnerScriptSha256 = Get-UpperSha256 $RunnerScriptPath
$registryRecoveryRecords = New-Object 'Collections.Generic.List[object]'
$titleSessions = @()
try {
    foreach ($titleIndex in 0..1) {
        $title = if ($titleIndex -eq 0) { 'Generals' } else { 'ZeroHour' }
        $titleRoot = Join-Path $outputFull $title
        [IO.Directory]::CreateDirectory($titleRoot) | Out-Null
        $role = if ($title -eq 'Generals') {
            'generals-executable'
        }
        else { 'zerohour-executable' }
        $titleMapCrc = [uint32]$mapCrcs[$title]
        $titleSessionRoot = Join-Path $titleRoot 'TitleSession'
        $titleSession = New-LockstepTitleSessionContract $title $titleSessionRoot `
            (Split-Path -Parent ([string]$artifactSet.artifacts[$role].path))
        $titleSessions += $titleSession
        $recoveryJournalPath = Join-Path $titleRoot 'Stage5RegistryRecovery.json'
        $recoveryIdentity = [ordered]@{
            runNonce = ([Guid]::NewGuid().ToString('D'))
            title = $title
            taskRoot = $titleRoot
            journalPath = $recoveryJournalPath
            userSid = $registryRecoveryUserSid
            mutexName = $registryRecoveryLock.name
            identityMode = 'acceptance-bound'
            runnerScriptSha256 = $registryRecoveryRunnerScriptSha256
            executableSha256 = $executables[$title]
            sourceCommit = $SourceCommit
            artifactSetSha256 = $artifactSet.sha256
        }
        $recoverySnapshots = New-Object 'Collections.Generic.List[object]'
        $recoveryPlannedMissing = New-Object 'Collections.Generic.List[string]'
        $recoveryRecord = [pscustomobject]@{
            path = $recoveryJournalPath
            identity = $recoveryIdentity
            snapshots = $recoverySnapshots
            plannedMissing = $recoveryPlannedMissing
            processIdentities = New-Object 'Collections.Generic.List[object]'
            restored = $false
            executable = [string]$artifactSet.artifacts[$role].path
        }
        $registryRecoveryRecords.Add($recoveryRecord) | Out-Null
        Initialize-LockstepTitleSessionDirectories $titleSession
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
            [Microsoft.Win32.RegistryView]::Registry64)) {
            $viewName = [string]$view
            $probe = New-LockstepRecoveryInstallPathSnapshot $title $viewName `
                ($titleSession.runtimeDirectory + '\')
            $recoveryRecord.snapshots.Add($probe.snapshot) | Out-Null
            foreach ($created in @($probe.createdSubKeys)) {
                $recoveryPlannedMissing.Add("$viewName|$created") | Out-Null
            }
            if (-not (Test-Path -LiteralPath $recoveryJournalPath -PathType Leaf)) {
                $recoveryIdentity.snapshotPlanSha256 =
                    Get-Stage5RegistryRecoverySnapshotPlanSha256 `
                        -Title $title `
                        -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                        -Snapshots @($recoverySnapshots.ToArray())
                New-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                    -Identity $recoveryIdentity `
                    -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                    -Snapshots @($recoverySnapshots.ToArray()) `
                    -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
            }
            else {
                Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                    -ExpectedIdentity $recoveryIdentity -State 'active' `
                    -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                    -Snapshots @($recoverySnapshots.ToArray()) `
                    -ChildExitProof (Test-LockstepRecoveryIdentitiesExited `
                        $recoveryRecord.processIdentities) `
                    -NoActiveTitleProcesses (Test-LockstepNoActiveTitleProcesses @(
                        $generalsFull, $zeroHourFull)) `
                    -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
            }
            foreach ($registryValue in $titleSession.registryValues) {
                Set-LockstepRegistryValue $view $registryValue.subKey `
                    $registryValue.name $registryValue.value $registrySnapshots `
                    $registrySnapshotKeys
            }
            Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                -ExpectedIdentity $recoveryIdentity -State 'active' `
                -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                -Snapshots @($recoverySnapshots.ToArray()) `
                -ChildExitProof (Test-LockstepRecoveryIdentitiesExited `
                    $recoveryRecord.processIdentities) `
                -NoActiveTitleProcesses (Test-LockstepNoActiveTitleProcesses @(
                    $generalsFull, $zeroHourFull)) `
                -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
        }
        $launchPendingObserver = {
            param($pendingIdentity)
            $recoveryRecord.processIdentities.Add($pendingIdentity) | Out-Null
            Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                -ExpectedIdentity $recoveryIdentity -State 'child-running' `
                -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                -Snapshots @($recoverySnapshots.ToArray()) `
                -ChildExitProof $false -NoActiveTitleProcesses $false `
                -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
        }
        $processStartObserver = {
            param($startedIdentity)
            $lastIndex = $recoveryRecord.processIdentities.Count - 1
            if ($lastIndex -lt 0 -or
                -not [bool]$recoveryRecord.processIdentities[$lastIndex].launchPending) {
                throw 'Lockstep recovery start observer lost its launch-pending identity.'
            }
            $recoveryRecord.processIdentities[$lastIndex] = $startedIdentity
            Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                -ExpectedIdentity $recoveryIdentity -State 'child-running' `
                -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                -Snapshots @($recoverySnapshots.ToArray()) `
                -ChildExitProof $false -NoActiveTitleProcesses $false `
                -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
        }
        $processExitObserver = {
            param($exitObservation)
            $observed = $exitObservation.processIdentity
            $match = @($recoveryRecord.processIdentities | Where-Object {
                $_.processId -eq $observed.processId -and
                $_.creationTimeUtc100ns -eq $observed.creationTimeUtc100ns
            }) | Select-Object -First 1
            if ($null -eq $match) {
                throw 'Lockstep recovery exit observer could not bind the retained process identity.'
            }
            $match.exitProven = [bool]$exitObservation.exited
            $allExited = Test-LockstepRecoveryIdentitiesExited `
                $recoveryRecord.processIdentities
            $noActive = Test-LockstepNoActiveTitleProcesses @(
                $generalsFull, $zeroHourFull)
            try {
                Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                    -ExpectedIdentity $recoveryIdentity `
                    -State $(if ($allExited) { 'active' } else { 'child-running' }) `
                    -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                    -Snapshots @($recoverySnapshots.ToArray()) `
                    -ChildExitProof ($allExited -and $noActive) `
                    -NoActiveTitleProcesses $noActive `
                    -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
            }
            catch {
                # Do not let a failed journal publication become in-memory exit
                # authority for the outer recovery decision.
                $match.exitProven = $false
                throw
            }
        }
        Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
            -ExpectedIdentity $recoveryIdentity -State 'child-running' `
            -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
            -Snapshots @($recoverySnapshots.ToArray()) `
            -ChildExitProof $false -NoActiveTitleProcesses $false `
            -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
        $sessionResults += Invoke-LockstepSession $title `
            ([string]$artifactSet.artifacts[$role].path) `
            $executables[$title] $SourceCommit $titleRoot $PeerCount `
            ($BasePort + ($titleIndex * $PeerCount)) $MapName $titleMapCrc $Seed $PeerTimeoutSeconds `
            $launcherContracts[$title] ([bool]$AllowHeadlessDirectExecution) $titleSession `
            $executionCohortNonce $executionCohortCreatedUtc $runtimeClosure `
            -ProcessLaunchPendingObserver $launchPendingObserver `
            -ProcessStartObserver $processStartObserver `
            -ProcessExitObserver $processExitObserver
        Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
            -ExpectedIdentity $recoveryIdentity -State 'active' `
            -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
            -Snapshots @($recoverySnapshots.ToArray()) `
            -ChildExitProof (Test-LockstepRecoveryIdentitiesExited `
                $recoveryRecord.processIdentities) `
            -NoActiveTitleProcesses (Test-LockstepNoActiveTitleProcesses @(
                $generalsFull, $zeroHourFull)) `
            -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
        $profileFiles = @(Assert-LockstepProfileReadOnly $titleSession.profileRoot)
        $sessionResults[-1] | Add-Member -NotePropertyName profileReadOnlyVerified `
            -NotePropertyValue $true -Force
        $sessionResults[-1] | Add-Member -NotePropertyName profileFilesAfterRun `
            -NotePropertyValue @($profileFiles | ForEach-Object { $_.FullName }) -Force
        foreach ($port in $sessionResults[-1].ports) {
            if ($allPorts -contains $port) { throw "Lockstep-v2 host reused UDP port $port within one validation run." }
            $allPorts += $port
        }
        if ($usedNonces.ContainsKey($sessionResults[-1].sessionNonce)) {
            throw 'Lockstep-v2 host reused a session nonce across product sessions.'
        }
        $usedNonces[$sessionResults[-1].sessionNonce] = $true
        foreach ($peerEvidence in $sessionResults[-1].peers) {
            if ($usedNonces.ContainsKey($peerEvidence.runNonce)) {
                throw 'Lockstep-v2 host reused a process nonce across product sessions.'
            }
            $usedNonces[$peerEvidence.runNonce] = $true
        }
        foreach ($probeIndex in 0..1) {
            $probeMode = if ($probeIndex -eq 0) {
                'negative-cross-epoch'
            }
            else { 'negative-content-mismatch' }
            $probePortBase = $BasePort + (2 * $PeerCount) +
                ($titleIndex * 4) + ($probeIndex * 2)
            Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                -ExpectedIdentity $recoveryIdentity -State 'child-running' `
                -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                -Snapshots @($recoverySnapshots.ToArray()) `
                -ChildExitProof $false -NoActiveTitleProcesses $false `
                -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
            $negativeProbe = Invoke-LockstepNegativeProbe $title `
                ([string]$artifactSet.artifacts[$role].path) `
                $executables[$title] $SourceCommit $outputFull $titleRoot `
                $probePortBase $MapName $titleMapCrc $Seed $PeerTimeoutSeconds `
                $launcherContracts[$title] ([bool]$AllowHeadlessDirectExecution) `
                $titleSession $probeMode $executionCohortNonce `
                $executionCohortCreatedUtc $runtimeClosure `
                -ProcessLaunchPendingObserver $launchPendingObserver `
                -ProcessStartObserver $processStartObserver `
                -ProcessExitObserver $processExitObserver
            $negativeProbeResults += $negativeProbe
            Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
                -ExpectedIdentity $recoveryIdentity -State 'active' `
                -PlannedMissingSubKeys @($recoveryPlannedMissing.ToArray()) `
                -Snapshots @($recoverySnapshots.ToArray()) `
                -ChildExitProof (Test-LockstepRecoveryIdentitiesExited `
                    $recoveryRecord.processIdentities) `
                -NoActiveTitleProcesses (Test-LockstepNoActiveTitleProcesses @(
                    $generalsFull, $zeroHourFull)) `
                -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) | Out-Null
            foreach ($port in @($probePortBase, $probePortBase + 1)) {
                if ($allPorts -contains $port) {
                    throw "Lockstep-v2 host reused UDP port $port within one validation run."
                }
                $allPorts += $port
            }
            if ($usedNonces.ContainsKey($negativeProbe.sessionNonce) -or
                $usedNonces.ContainsKey($negativeProbe.runNonce)) {
                throw 'Lockstep-v2 host reused a negative-probe nonce.'
            }
            $usedNonces[$negativeProbe.sessionNonce] = $true
            $usedNonces[$negativeProbe.runNonce] = $true
        }
    }
    $recordedUtc = [DateTime]::UtcNow.ToString('o')
    }
    finally {
    $cleanupErrors = @()
    foreach ($recoveryRecord in @($registryRecoveryRecords)) {
        if (-not (Test-Path -LiteralPath $recoveryRecord.path -PathType Leaf)) {
            $cleanupErrors += "registry recovery journal is missing: $($recoveryRecord.path)"
            continue
        }
        $noActiveTitleProcesses = Test-LockstepNoActiveTitleProcesses @(
            $generalsFull, $zeroHourFull)
        $allExited = Test-LockstepRecoveryIdentitiesExited `
            $recoveryRecord.processIdentities
        if ($allExited -and $noActiveTitleProcesses) {
            $authorizationIdentities = @(New-LockstepRecoveryAuthorizationIdentities `
                $recoveryRecord.processIdentities)
            foreach ($identity in $authorizationIdentities) {
                $identity.exitProven = $true
            }
            try {
                Invoke-Stage5RegistryRecovery -Path $recoveryRecord.path `
                    -ExpectedIdentity $recoveryRecord.identity `
                    -Authorization ([pscustomobject]@{
                        childExitProven = $true
                        noActiveTitleProcesses = $true
                        processIdentities = $authorizationIdentities
                    }) `
                    -MutexLock $registryRecoveryLock `
                    -Adapter (New-LockstepRecoveryAdapter) | Out-Null
                $recoveryRecord.restored = $true
                Remove-Item -LiteralPath $recoveryRecord.path -Force
            }
            catch {
                $cleanupErrors += "registry recovery '$($recoveryRecord.identity.title)': $($_.Exception.Message); journal retained at $($recoveryRecord.path)"
            }
        }
        else {
            try {
                Update-Stage5RegistryRecoveryJournal -Path $recoveryRecord.path `
                    -ExpectedIdentity $recoveryRecord.identity -State 'child-exit-unproven' `
                    -Snapshots @($recoveryRecord.snapshots.ToArray()) `
                    -ChildExitProof $false `
                    -NoActiveTitleProcesses ([bool]($noActiveTitleProcesses -and $allExited)) `
                    -ProcessIdentities @($recoveryRecord.processIdentities.ToArray()) `
                    -FailureMessage 'Registry recovery retained because every child launch/exit identity was not proven.' | Out-Null
            }
            catch {
                $cleanupErrors += "registry recovery journal update '$($recoveryRecord.identity.title)': $($_.Exception.Message)"
            }
            $cleanupErrors += "registry recovery '$($recoveryRecord.identity.title)' was not proven; journal retained at $($recoveryRecord.path)"
        }
    }
    foreach ($titleSessionToClean in $titleSessions) {
        $recoveryRecord = @($registryRecoveryRecords | Where-Object {
            $_.identity.title -ceq $titleSessionToClean.title
        }) | Select-Object -First 1
        if ($null -ne $recoveryRecord -and $recoveryRecord.restored) {
            try {
                Remove-LockstepTitleSessionDirectories $titleSessionToClean $outputFull
            }
            catch {
                $cleanupErrors += "$($titleSessionToClean.title) title-session cleanup: $($_.Exception.Message)"
            }
        }
        else {
            $cleanupErrors += "$($titleSessionToClean.title) registry recovery was not proven; preserving title-session and journal."
        }
    }
    if ($null -ne $registryRecoveryLock) {
        try { Exit-Stage5RegistryRecoveryMutex $registryRecoveryLock }
        catch { $cleanupErrors += "registry recovery mutex cleanup: $($_.Exception.Message)" }
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "Lockstep-v2 final cleanup failed: $($cleanupErrors -join ' | ')"
    }
}

    return [pscustomobject]@{
        sourceCommit = $SourceCommit
        artifactSet = $artifactSet
        runtimeClosure = $runtimeClosure
        mapCrcs = $mapCrcs
        outputDirectory = $outputFull
        executables = $executables
        launcherContracts = $launcherContracts
        cohortNonce = $executionCohortNonce
        cohortCreatedUtc = $executionCohortCreatedUtc
        recordedUtc = $recordedUtc
        dataBinding = $ValidationDataBinding
        sessionResults = @($sessionResults)
        negativeProbeResults = @($negativeProbeResults)
    }
}


function Test-UdpPortAvailable {
    param([int]$Port)
    $client = $null
    try {
        $client = New-Object Net.Sockets.UdpClient($Port)
        return $true
    }
    catch [Net.Sockets.SocketException] { return $false }
    finally {
        if ($null -ne $client) { $client.Dispose() }
    }
}

function Get-BitCount {
    param([UInt64]$Value)
    $count = 0
    while ($Value -ne 0) {
        $Value = $Value -band ($Value - 1)
        ++$count
    }
    return $count
}

function Get-CanonicalReceiptKeyValue {
    param([string]$Line, [string]$Context)
    $equals = $Line.IndexOf('=', [StringComparison]::Ordinal)
    if ($equals -le 0 -or $equals -ge $Line.Length - 1) {
        throw "$Context contains a malformed key/value line: $Line"
    }
    return [pscustomobject]@{
        key = $Line.Substring(0, $equals)
        value = $Line.Substring($equals + 1)
    }
}

function Get-ReceiptPairs {
    param([string]$Path)
    $text = [IO.File]::ReadAllText($Path)
    if ($text.IndexOf("`r", [StringComparison]::Ordinal) -ge 0) {
        throw "Receipt contains non-canonical CR line endings: $Path"
    }
    $lines = $text.Split(@("`n"), [StringSplitOptions]::None)
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }
    if ($lines.Count -lt 4 -or $lines[0] -cne $LockstepMagic -or
        $lines[$lines.Count - 1] -cne 'END') {
        throw "Receipt is not a canonical lockstep-v2 document: $Path"
    }
    $pairs = [ordered]@{}
    for ($index = 1; $index -lt $lines.Count - 1; ++$index) {
        if ($lines[$index].Length -eq 0) {
            throw "Receipt contains an empty line: $Path"
        }
        $pair = Get-CanonicalReceiptKeyValue $lines[$index] $Path
        if ($pairs.Contains($pair.key)) {
            throw "Receipt repeats field $($pair.key): $Path"
        }
        $pairs[$pair.key] = $pair.value
    }
    if (-not $pairs.Contains('checkpoint_count')) {
        throw "Receipt has no checkpoint_count: $Path"
    }
    $checkpointCount = ConvertTo-ReceiptUInt32 $pairs['checkpoint_count'] 'checkpoint_count'
    if ($checkpointCount -gt 129) {
        throw "Receipt checkpoint_count exceeds the v2 bound: $Path"
    }
    $expectedKeys = @(
        'producer', 'mode', 'schema', 'protocol_epoch', 'local_slot', 'peer_count',
        'roster_mask', 'simulation_roster_mask', 'ai_roster_mask',
        'build_compatibility_crc', 'content_crc', 'map_crc',
        'common_stop_frame', 'proven_kernel_mask', 'packet_router_slot', 'origin_mode',
        'run_nonce', 'session_nonce', 'executable_sha256', 'source_revision',
        'network_session_token', 'final_frame', 'frame_count', 'contributed_peer_mask',
        'checkpoint_count', 'validation_authority_mask', 'executable_origin',
        'worker_telemetry_executable_origin', 'transport_path_used',
        'handshake_validated', 'clean_shutdown',
        'ai_planning_captured_snapshots', 'ai_planning_captured_candidates',
        'ai_planning_requested_batches', 'ai_planning_submitted_jobs',
        'ai_planning_completed_jobs', 'ai_planning_serial_fallbacks',
        'ai_planning_shadow_matches', 'ai_planning_shadow_mismatches',
        'ai_planning_validation_failures',
        'ai_planning_canonical_validation_invocations',
        'ai_planning_committed_batches',
        'ai_planning_parallel_authoritative_commits',
        'ai_planning_rejected_commits',
        'ai_planning_physical_worker_executions',
        'ai_planning_owner_helped_executions',
        'ai_planning_observed_physical_worker_mask',
        'ai_planning_maximum_distinct_physical_workers',
        'ai_planning_maximum_concurrent_physical_workers',
        'ai_planning_digest')
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $expectedKeys += "peer_${slot}_command_count"
        $expectedKeys += "peer_${slot}_first_command_frame"
        $expectedKeys += "peer_${slot}_last_command_frame"
        $expectedKeys += "peer_${slot}_last_command_id"
        $expectedKeys += "peer_${slot}_has_last_command_id"
        $expectedKeys += "peer_${slot}_last_command_digest"
        $expectedKeys += "peer_${slot}_command_digest"
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $expectedKeys += "kernel_${kernel}_physical_worker_mask"
        $expectedKeys += "kernel_${kernel}_physical_worker_jobs"
        $expectedKeys += "kernel_${kernel}_distinct_physical_workers"
        $expectedKeys += "kernel_${kernel}_peak_concurrent_physical_workers"
        $expectedKeys += "kernel_${kernel}_physical_worker_mask_complete"
    }
    for ($checkpoint = 0; $checkpoint -lt $checkpointCount; ++$checkpoint) {
        $expectedKeys += "checkpoint_${checkpoint}_frame"
        $expectedKeys += "checkpoint_${checkpoint}_crc"
        $expectedKeys += "checkpoint_${checkpoint}_command_digest"
    }
    if ($pairs.Count -ne $expectedKeys.Count) {
        throw "Receipt has an unexpected field count: $Path"
    }
    $actualKeys = @($pairs.Keys)
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        if ($actualKeys[$index] -cne $expectedKeys[$index]) {
            throw "Receipt field order/shape mismatch at ${index}: $Path"
        }
    }
    return [pscustomobject]@{ path = $Path; pairs = $pairs; text = $text }
}

function Get-NegativeProbePairs {
    param([string]$Path)
    $text = [IO.File]::ReadAllText($Path)
    if ($text.IndexOf("`r", [StringComparison]::Ordinal) -ge 0) {
        throw "Negative probe proof contains non-canonical CR line endings: $Path"
    }
    $lines = $text.Split(@("`n"), [StringSplitOptions]::None)
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }
    $expectedKeys = @(
        'producer', 'mode', 'schema', 'protocol_epoch', 'run_nonce',
        'session_nonce', 'executable_sha256', 'source_revision',
        'probe_build_compatibility_crc', 'probe_content_crc', 'mutation',
        'baseline_input_sha256', 'input_sha256', 'baseline_accepted',
        'mutated_accepted', 'expected_error', 'observed_error', 'process_id')
    if ($lines.Count -ne ($expectedKeys.Count + 2) -or
        $lines[0] -cne $LockstepNegativeProbeMagic -or
        $lines[$lines.Count - 1] -cne 'END') {
        throw "Negative probe proof is not a canonical installed lockstep-v2 document: $Path"
    }
    $pairs = [ordered]@{}
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        if ($lines[$index + 1].Length -eq 0) {
            throw "Negative probe proof contains an empty line: $Path"
        }
        $pair = Get-CanonicalReceiptKeyValue $lines[$index + 1] $Path
        if ($pairs.Contains($pair.key) -or $pair.key -cne $expectedKeys[$index]) {
            throw "Negative probe proof field order/shape mismatch at ${index}: $Path"
        }
        $pairs[$pair.key] = $pair.value
    }
    return [pscustomobject]@{ path = $Path; pairs = $pairs; text = $text }
}

function Get-LockstepNegativeStdoutProof {
    param([string]$Stdout)
    if ([string]::IsNullOrEmpty($Stdout)) {
        throw 'Installed lockstep-v2 negative probe did not provide stdout.'
    }
    $lines = @($Stdout -split "`n" | ForEach-Object { $_.TrimEnd("`r") })
    $passLines = @($lines | Where-Object {
        $_.StartsWith('LOCKSTEP_V2_NEGATIVE_PROBE_PASS ',
            [StringComparison]::Ordinal)
    })
    if ($passLines.Count -ne 1) {
        throw 'Installed lockstep-v2 negative probe must provide exactly one pass marker.'
    }
    $match = [regex]::Match($passLines[0],
        '^LOCKSTEP_V2_NEGATIVE_PROBE_PASS mode=(?<mode>negative-(?:cross-epoch|content-mismatch)) pid=(?<pid>[0-9]+) rejection=(?<error>[A-Za-z]+)$')
    if (-not $match.Success) {
        throw 'Installed lockstep-v2 negative probe pass marker has an unsupported shape.'
    }
    return [pscustomobject]@{
        marker = $passLines[0]
        mode = $match.Groups['mode'].Value
        pid = [int]$match.Groups['pid'].Value
        rejection = $match.Groups['error'].Value
    }
}

function Resolve-LockstepEvidenceFile {
    param([string]$Root, [string]$RelativePath, [string]$Context)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\\/])\.([\\/]|$)' -or
        $RelativePath -match '(^|[\\/])\.\.([\\/]|$)') {
        throw "$Context path is not a bounded evidence-relative path: $RelativePath"
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath))
    $rootParts = @($rootFull.Split([char[]]@([char]92, [char]47)) |
        Where-Object { $_ -ne '' })
    $candidateParts = @($candidate.Split([char[]]@([char]92, [char]47)) |
        Where-Object { $_ -ne '' })
    if ($candidateParts.Count -lt $rootParts.Count) {
        throw "$Context path is missing or escapes the native evidence root: $RelativePath"
    }
    for ($partIndex = 0; $partIndex -lt $rootParts.Count; ++$partIndex) {
        if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$candidateParts[$partIndex], [string]$rootParts[$partIndex])) {
            throw "$Context path is missing or escapes the native evidence root: $RelativePath"
        }
    }
    Assert-LockstepNoReparse $candidate $Context $rootFull
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "$Context path is missing or escapes the native evidence root: $RelativePath"
    }
    return $candidate
}

function Assert-LockstepNegativeProbeEvidence {
    param(
        [pscustomobject]$Entry,
        [string]$EvidenceRoot,
        [string]$ExpectedTitle,
        [string]$ExpectedMode,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedExecutableSha256,
        [uint32]$ExpectedMapCrc,
        [int]$ExpectedSeed,
        [string]$ExpectedExecutablePath = $null
    )
    if ($null -eq $Entry) { throw 'Native lockstep-v2 negative probe entry is missing.' }
    $required = @(
        'title', 'mode', 'producer', 'processId', 'processCreationUtc',
        'executablePath', 'runNonce', 'sessionNonce', 'executableSha256',
        'sourceCommit', 'proofPath', 'proofSha256',
        'stdoutPath', 'stdoutSha256', 'stderrPath', 'stderrSha256',
        'inputSha256', 'baselineAccepted', 'mutatedAccepted', 'mutation',
        'expectedError', 'observedError', 'exitCode', 'commandLine',
        'arguments', 'probeBuildCrc', 'probeContentCrc')
    $actual = @($Entry.PSObject.Properties | ForEach-Object { $_.Name })
    if ($actual.Count -ne $required.Count) {
        throw "Native lockstep-v2 negative probe entry has an unexpected field count for $ExpectedTitle/$ExpectedMode."
    }
    for ($index = 0; $index -lt $required.Count; ++$index) {
        if ($actual[$index] -cne $required[$index]) {
            throw "Native lockstep-v2 negative probe entry field order mismatch for $ExpectedTitle/$ExpectedMode."
        }
    }
    foreach ($field in @('processId', 'exitCode', 'probeBuildCrc', 'probeContentCrc')) {
        if (-not (Test-Stage5JsonInteger $Entry.$field)) {
            throw "Native lockstep-v2 negative probe field '$field' must be a JSON integer for $ExpectedTitle/$ExpectedMode."
        }
    }
    if ($Entry.processId -lt 1 -or
        $Entry.processId -gt [UInt64][Int32]::MaxValue -or
        $Entry.exitCode -lt 0 -or
        $Entry.exitCode -gt [UInt64][Int32]::MaxValue) {
        throw "Native lockstep-v2 negative probe process identity or exit code is outside the Int32 range for $ExpectedTitle/$ExpectedMode."
    }
    if ($Entry.probeBuildCrc -lt 0 -or
        $Entry.probeBuildCrc -gt [UInt64][UInt32]::MaxValue -or
        $Entry.probeContentCrc -lt 0 -or
        $Entry.probeContentCrc -gt [UInt64][UInt32]::MaxValue) {
        throw "Native lockstep-v2 negative probe CRC is outside the UInt32 range for $ExpectedTitle/$ExpectedMode."
    }
    if ([string]$Entry.title -cne $ExpectedTitle -or
        [string]$Entry.mode -cne $ExpectedMode -or
        [string]$Entry.producer -cne $LockstepProducer -or
        [string]$Entry.sourceCommit -cne $ExpectedSourceCommit -or
        [string]$Entry.executableSha256 -cne $ExpectedExecutableSha256 -or
        [string]$Entry.runNonce -notmatch '^[0-9A-F]{32}$' -or
        [string]$Entry.sessionNonce -notmatch '^[0-9A-F]{32}$' -or
        [string]$Entry.executableSha256 -notmatch '^[0-9A-F]{64}$' -or
        [string]$Entry.sourceCommit -notmatch '^[0-9a-f]{40}$' -or
        [int]$Entry.processId -le 0 -or [int]$Entry.exitCode -ne 0) {
        throw "Native lockstep-v2 negative probe identity is not bound to $ExpectedTitle/$ExpectedMode."
    }
    [DateTimeOffset]$processCreated = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$Entry.processCreationUtc,
            [ref]$processCreated)) {
        throw "Native lockstep-v2 negative probe process creation time is not valid for $ExpectedTitle/$ExpectedMode."
    }
    if ([string]::IsNullOrWhiteSpace([string]$Entry.executablePath)) {
        throw "Native lockstep-v2 negative probe executable path is missing for $ExpectedTitle/$ExpectedMode."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedExecutablePath) -and
        [IO.Path]::GetFullPath([string]$Entry.executablePath) -cne
        [IO.Path]::GetFullPath($ExpectedExecutablePath)) {
        throw "Native lockstep-v2 negative probe executable path is not the installed $ExpectedTitle executable."
    }
    $proofPath = Resolve-LockstepEvidenceFile $EvidenceRoot `
        ([string]$Entry.proofPath) "$ExpectedTitle/$ExpectedMode proof"
    $stdoutPath = Resolve-LockstepEvidenceFile $EvidenceRoot `
        ([string]$Entry.stdoutPath) "$ExpectedTitle/$ExpectedMode stdout"
    $stderrPath = Resolve-LockstepEvidenceFile $EvidenceRoot `
        ([string]$Entry.stderrPath) "$ExpectedTitle/$ExpectedMode stderr"
    foreach ($hashCheck in @(
        @($proofPath, [string]$Entry.proofSha256, 'proofSha256'),
        @($stdoutPath, [string]$Entry.stdoutSha256, 'stdoutSha256'),
        @($stderrPath, [string]$Entry.stderrSha256, 'stderrSha256'))) {
        if ($hashCheck[1] -notmatch '^[0-9A-F]{64}$' -or
            (Get-UpperSha256 $hashCheck[0]) -cne $hashCheck[1]) {
            throw "Native lockstep-v2 negative probe $($hashCheck[2]) is not hash-bound for $ExpectedTitle/$ExpectedMode."
        }
    }
    $parsed = Get-NegativeProbePairs $proofPath
    $pairs = $parsed.pairs
    $expectedError = if ($ExpectedMode -ceq 'negative-cross-epoch') {
        'UnsupportedEngineEpoch'
    }
    else { 'ContentHashMismatch' }
    $expectedMutation = if ($ExpectedMode -ceq 'negative-cross-epoch') {
        'engine-epoch'
    }
    else { 'content-hash' }
    if ($pairs['producer'] -cne $LockstepProducer -or
        $pairs['mode'] -cne $ExpectedMode -or
        $pairs['schema'] -cne '2' -or $pairs['protocol_epoch'] -cne '2' -or
        $pairs['run_nonce'] -cne [string]$Entry.runNonce -or
        $pairs['session_nonce'] -cne [string]$Entry.sessionNonce -or
        $pairs['executable_sha256'] -cne $ExpectedExecutableSha256 -or
        $pairs['source_revision'] -cne $ExpectedSourceCommit -or
        $pairs['mutation'] -cne $expectedMutation -or
        $pairs['expected_error'] -cne $expectedError -or
        $pairs['observed_error'] -cne $expectedError -or
        $pairs['baseline_input_sha256'] -notmatch '^[0-9A-F]{64}$' -or
        $pairs['input_sha256'] -notmatch '^[0-9A-F]{64}$' -or
        $pairs['baseline_input_sha256'] -ceq $pairs['input_sha256'] -or
        -not (ConvertTo-ReceiptBool $pairs['baseline_accepted'] 'baseline_accepted') -or
        (ConvertTo-ReceiptBool $pairs['mutated_accepted'] 'mutated_accepted') -or
        [UInt64](ConvertTo-ReceiptUInt64 $pairs['probe_build_compatibility_crc'] 'probe_build_compatibility_crc') -ne [UInt64]$Entry.probeBuildCrc -or
        [UInt64](ConvertTo-ReceiptUInt64 $pairs['probe_content_crc'] 'probe_content_crc') -ne [UInt64]$Entry.probeContentCrc -or
        [UInt64](ConvertTo-ReceiptUInt64 $pairs['probe_build_compatibility_crc'] 'probe_build_compatibility_crc') -ne [UInt64]$ExpectedMapCrc -or
        [UInt64](ConvertTo-ReceiptUInt64 $pairs['probe_content_crc'] 'probe_content_crc') -ne [UInt64]$ExpectedSeed -or
        [int](ConvertTo-ReceiptUInt64 $pairs['process_id'] 'process_id') -ne [int]$Entry.processId) {
        throw "Native lockstep-v2 negative probe raw proof did not prove the expected rejection for $ExpectedTitle/$ExpectedMode."
    }
    $stdoutText = [IO.File]::ReadAllText($stdoutPath)
    $stdoutProof = Get-LockstepNegativeStdoutProof $stdoutText
    if ($stdoutProof.mode -cne $ExpectedMode -or
        $stdoutProof.pid -ne [int]$Entry.processId -or
        $stdoutProof.rejection -cne $expectedError -or
        $stdoutText -match 'NET3_VALIDATION_PEER_PASS') {
        throw "Native lockstep-v2 negative probe stdout is not bound to the observed rejection for $ExpectedTitle/$ExpectedMode."
    }
    if ([string]$Entry.inputSha256 -cne $pairs['input_sha256'] -or
        [string]$Entry.expectedError -cne $expectedError -or
        [string]$Entry.observedError -cne $expectedError -or
        [string]$Entry.mutation -cne $expectedMutation -or
        [bool]$Entry.baselineAccepted -ne $true -or
        [bool]$Entry.mutatedAccepted -ne $false -or
        @($Entry.arguments).Count -lt 2 -or
        @($Entry.arguments | Where-Object { [string]$_ -ceq '-installedLockstepV2Validation' }).Count -ne 1 -or
        [string]$Entry.commandLine -notmatch [regex]::Escape($ExpectedMode)) {
        throw "Native lockstep-v2 negative probe result metadata is not bound to its raw proof for $ExpectedTitle/$ExpectedMode."
    }
    return [pscustomobject]@{
        title = $ExpectedTitle
        mode = $ExpectedMode
        processId = [int]$Entry.processId
        runNonce = [string]$Entry.runNonce
        sessionNonce = [string]$Entry.sessionNonce
        proofPath = $proofPath
        inputSha256 = $pairs['input_sha256']
        baselineAccepted = $true
        mutatedAccepted = $false
        expectedError = $expectedError
        observedError = $expectedError
    }
}

function Get-ReceiptProjection {
    param([pscustomobject]$Parsed, [switch]$DeterministicOnly)
    $pairs = $Parsed.pairs
    $projection = [ordered]@{}
    foreach ($key in @('mode', 'schema', 'protocol_epoch', 'peer_count', 'roster_mask',
        'simulation_roster_mask', 'ai_roster_mask', 'build_compatibility_crc',
        'content_crc', 'map_crc', 'common_stop_frame',
        'proven_kernel_mask', 'packet_router_slot', 'origin_mode', 'session_nonce',
        'executable_sha256', 'source_revision', 'final_frame', 'frame_count',
        'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
        'executable_origin', 'worker_telemetry_executable_origin', 'transport_path_used',
        'handshake_validated', 'clean_shutdown',
        'ai_planning_captured_snapshots', 'ai_planning_captured_candidates',
        'ai_planning_requested_batches', 'ai_planning_submitted_jobs',
        'ai_planning_completed_jobs', 'ai_planning_serial_fallbacks',
        'ai_planning_shadow_matches', 'ai_planning_shadow_mismatches',
         'ai_planning_validation_failures',
         'ai_planning_canonical_validation_invocations',
         'ai_planning_committed_batches',
         'ai_planning_parallel_authoritative_commits',
         'ai_planning_rejected_commits',
         'ai_planning_physical_worker_executions',
         'ai_planning_owner_helped_executions',
         'ai_planning_observed_physical_worker_mask',
         'ai_planning_maximum_distinct_physical_workers',
         'ai_planning_maximum_concurrent_physical_workers',
         'ai_planning_digest')) {
        # These observations prove that the executable actually used physical
        # workers, but they are intentionally topology-dependent.  The full
        # projection retains them for evidence binding; only the cross-peer
        # deterministic comparison omits them, matching the native planning
        # digest contract in LockstepV2Contract.cpp.
        if ($DeterministicOnly -and $key -in @(
                'ai_planning_physical_worker_executions',
                'ai_planning_observed_physical_worker_mask',
                'ai_planning_maximum_distinct_physical_workers',
                'ai_planning_maximum_concurrent_physical_workers')) {
            continue
        }
        $projection[$key] = $pairs[$key]
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $key = "peer_${slot}_${suffix}"
            $projection[$key] = $pairs[$key]
        }
    }
    for ($index = 0; $index -lt 129; ++$index) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $key = "checkpoint_${index}_${suffix}"
            if ($pairs.Contains($key)) { $projection[$key] = $pairs[$key] }
        }
    }
    return ($projection | ConvertTo-Json -Compress -Depth 5)
}

function Get-ReceiptCommandDigest {
    param([pscustomobject]$Parsed)
    Add-Type -AssemblyName System.Numerics
    $pairs = $Parsed.pairs
    $hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    $prime = [Numerics.BigInteger]::Parse('1099511628211')
    $mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    function Update-FnvUnsigned {
        param([Numerics.BigInteger]$Hash, [UInt64]$Value, [int]$Bytes,
            [Numerics.BigInteger]$Prime, [Numerics.BigInteger]$Mask)
        $updated = $Hash
        for ($byteIndex = 0; $byteIndex -lt $Bytes; ++$byteIndex) {
            $updated = (($updated -bxor ([Numerics.BigInteger]($Value -band 255))) * $Prime) -band $Mask
            $Value = $Value -shr 8
        }
        return $updated
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $hash = Update-FnvUnsigned $hash ([UInt64]$slot) 4 $prime $mask
        $hash = Update-FnvUnsigned $hash (ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_command_count"] "peer_${slot}_command_count") 4 $prime $mask
        $hash = Update-FnvUnsigned $hash (ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_first_command_frame"] "peer_${slot}_first_command_frame") 4 $prime $mask
        $hash = Update-FnvUnsigned $hash (ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_last_command_frame"] "peer_${slot}_last_command_frame") 4 $prime $mask
        $hash = Update-FnvUnsigned $hash ([UInt64](ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_last_command_id"] "peer_${slot}_last_command_id")) 2 $prime $mask
        $hasLast = ConvertTo-ReceiptBool $pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $hash = Update-FnvUnsigned $hash ([UInt64]($(if ($hasLast) { 1 } else { 0 }))) 4 $prime $mask
        $hash = Update-FnvUnsigned $hash (ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_last_command_digest"] "peer_${slot}_last_command_digest") 8 $prime $mask
        $hash = Update-FnvUnsigned $hash (ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_command_digest"] "peer_${slot}_command_digest") 8 $prime $mask
    }
    return $hash
}

function Update-ReceiptAIPlanningFnv {
    param([Numerics.BigInteger]$Hash, [UInt64]$Value, [int]$Bytes,
        [Numerics.BigInteger]$Prime, [Numerics.BigInteger]$Mask)
    $updated = $Hash
    for ($byteIndex = 0; $byteIndex -lt $Bytes; ++$byteIndex) {
        $updated = (($updated -bxor
            ([Numerics.BigInteger]($Value -band 255))) * $Prime) -band $Mask
        $Value = $Value -shr 8
    }
    return $updated
}

function Get-ReceiptAIPlanningDigest {
    param([pscustomobject]$Parsed)
    Add-Type -AssemblyName System.Numerics
    $pairs = $Parsed.pairs
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    $hash = Update-ReceiptAIPlanningFnv $hash `
        (ConvertTo-ReceiptUInt32 $pairs['simulation_roster_mask'] 'simulation_roster_mask') `
        4 $prime $mask
    $hash = Update-ReceiptAIPlanningFnv $hash `
        (ConvertTo-ReceiptUInt32 $pairs['ai_roster_mask'] 'ai_roster_mask') `
        4 $prime $mask
    foreach ($field in @(
        'captured_snapshots', 'captured_candidates', 'requested_batches',
        'submitted_jobs', 'completed_jobs', 'serial_fallbacks',
         'shadow_matches', 'shadow_mismatches', 'validation_failures',
         'canonical_validation_invocations', 'committed_batches',
         'parallel_authoritative_commits', 'rejected_commits',
         'owner_helped_executions')) {
        $hash = Update-ReceiptAIPlanningFnv $hash `
            (ConvertTo-ReceiptUInt64 $pairs["ai_planning_$field"] "ai_planning_$field") `
            8 $prime $mask
    }
    return $hash
}

function Assert-LockstepV2Receipt {
    param(
        [pscustomobject]$Parsed,
        [int]$ExpectedLocalSlot,
        [int]$ExpectedPeerCount,
        [uint32]$ExpectedMapCrc,
        [string]$ExpectedRunNonce,
        [string]$ExpectedSessionNonce,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedSourceCommit,
        [uint32]$ExpectedNetworkRosterMask = $LockstepNetworkRosterMask,
        [uint32]$ExpectedSimulationRosterMask = $LockstepSimulationRosterMask,
        [uint32]$ExpectedAIRosterMask = $LockstepAIRosterMask
    )
    $pairs = $Parsed.pairs
    if ($pairs['producer'] -cne $LockstepProducer -or $pairs['mode'] -cne $LockstepMode) {
        throw "Receipt is not an installed lockstep-v2 production receipt: $($Parsed.path)"
    }
    foreach ($check in @(
        @('schema', $LockstepSchema), @('protocol_epoch', $LockstepProtocolEpoch),
        @('local_slot', $ExpectedLocalSlot), @('peer_count', $ExpectedPeerCount),
        @('roster_mask', $ExpectedNetworkRosterMask),
        @('simulation_roster_mask', $ExpectedSimulationRosterMask),
        @('ai_roster_mask', $ExpectedAIRosterMask),
        @('map_crc', $ExpectedMapCrc), @('common_stop_frame', $CommonStopFrame),
        @('proven_kernel_mask', $LockstepAuthorityMask), @('packet_router_slot', 0),
        @('origin_mode', 2), @('final_frame', $CommonStopFrame),
        @('frame_count', $CommonStopFrame),
        @('contributed_peer_mask', ((1 -shl $ExpectedPeerCount) - 1)),
        @('checkpoint_count', $LockstepCheckpointCount),
        @('validation_authority_mask', $LockstepAuthorityMask))) {
        if ((ConvertTo-ReceiptUInt64 $pairs[$check[0]] $check[0]) -ne [UInt64]$check[1]) {
            throw "Receipt field $($check[0]) does not match the qualification contract: $($Parsed.path)"
        }
    }
    if ($ExpectedPeerCount -ne $LockstepNetworkPeerCount -or
        $ExpectedNetworkRosterMask -ne $LockstepNetworkRosterMask -or
        $ExpectedSimulationRosterMask -ne $LockstepSimulationRosterMask -or
        $ExpectedAIRosterMask -ne $LockstepAIRosterMask -or
        (Get-BitCount ([UInt64]$ExpectedAIRosterMask)) -ne $LockstepAIPlayerCount -or
        ($ExpectedNetworkRosterMask -band $ExpectedAIRosterMask) -ne 0 -or
        ($ExpectedNetworkRosterMask -bor $ExpectedAIRosterMask) -ne $ExpectedSimulationRosterMask) {
        throw "Receipt topology is not the bounded two-human/four-local-AI qualification contract: $($Parsed.path)"
    }
    if (-not (Test-CanonicalHex $pairs['run_nonce'] 32) -or
        $pairs['run_nonce'] -cne $ExpectedRunNonce -or
        -not (Test-CanonicalHex $pairs['session_nonce'] 32) -or
        $pairs['session_nonce'] -cne $ExpectedSessionNonce -or
        $pairs['executable_sha256'] -cne $ExpectedExecutableSha256 -or
        -not (Test-CanonicalHex $pairs['executable_sha256'] 64) -or
        $pairs['source_revision'] -cne $ExpectedSourceCommit -or
        -not (Test-CanonicalHex $pairs['source_revision'] 40)) {
        throw "Receipt executable/source/nonce identity does not match: $($Parsed.path)"
    }
    foreach ($field in @('executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown')) {
        if (-not (ConvertTo-ReceiptBool $pairs[$field] $field)) {
            throw "Receipt field $field is not true: $($Parsed.path)"
        }
    }
    if ((ConvertTo-ReceiptUInt64 $pairs['network_session_token'] 'network_session_token') -eq 0) {
        throw "Receipt has no network session token: $($Parsed.path)"
    }
    $aiPlanningFields = @(
        'captured_snapshots', 'captured_candidates', 'requested_batches',
        'submitted_jobs', 'completed_jobs', 'serial_fallbacks',
        'shadow_matches', 'shadow_mismatches', 'validation_failures',
        'canonical_validation_invocations', 'committed_batches',
        'parallel_authoritative_commits', 'rejected_commits',
        'physical_worker_executions', 'owner_helped_executions',
        'observed_physical_worker_mask', 'maximum_distinct_physical_workers',
        'maximum_concurrent_physical_workers')
    foreach ($field in $aiPlanningFields) {
        [void](ConvertTo-ReceiptUInt64 $pairs["ai_planning_$field"] "ai_planning_$field")
    }
    $aiSnapshots = ConvertTo-ReceiptUInt64 $pairs['ai_planning_captured_snapshots'] `
        'ai_planning_captured_snapshots'
    $aiSubmitted = ConvertTo-ReceiptUInt64 $pairs['ai_planning_submitted_jobs'] `
        'ai_planning_submitted_jobs'
    $aiCompleted = ConvertTo-ReceiptUInt64 $pairs['ai_planning_completed_jobs'] `
        'ai_planning_completed_jobs'
    $aiCommitted = ConvertTo-ReceiptUInt64 $pairs['ai_planning_committed_batches'] `
        'ai_planning_committed_batches'
    $aiParallel = ConvertTo-ReceiptUInt64 $pairs['ai_planning_parallel_authoritative_commits'] `
        'ai_planning_parallel_authoritative_commits'
    $aiMask = ConvertTo-ReceiptUInt64 $pairs['ai_planning_observed_physical_worker_mask'] `
        'ai_planning_observed_physical_worker_mask'
    $aiDistinct = ConvertTo-ReceiptUInt64 $pairs['ai_planning_maximum_distinct_physical_workers'] `
        'ai_planning_maximum_distinct_physical_workers'
    $aiPeak = ConvertTo-ReceiptUInt64 $pairs['ai_planning_maximum_concurrent_physical_workers'] `
        'ai_planning_maximum_concurrent_physical_workers'
    if ($aiSnapshots -lt $LockstepAIPlayerCount -or $aiSubmitted -eq 0 -or
        $aiCompleted -ne $aiSubmitted -or $aiCommitted -eq 0 -or $aiParallel -eq 0 -or
        $aiParallel -gt $aiCommitted -or $aiMask -eq 0 -or
        (Get-BitCount $aiMask) -lt 2 -or $aiDistinct -lt 2 -or $aiPeak -lt 2 -or
        $aiPeak -gt $aiDistinct -or
        (ConvertTo-ReceiptUInt64 $pairs['ai_planning_serial_fallbacks'] 'ai_planning_serial_fallbacks') -ne 0 -or
        (ConvertTo-ReceiptUInt64 $pairs['ai_planning_shadow_mismatches'] 'ai_planning_shadow_mismatches') -ne 0 -or
        (ConvertTo-ReceiptUInt64 $pairs['ai_planning_validation_failures'] 'ai_planning_validation_failures') -ne 0 -or
        (ConvertTo-ReceiptUInt64 $pairs['ai_planning_rejected_commits'] 'ai_planning_rejected_commits') -ne 0 -or
        (ConvertTo-ReceiptUInt64 $pairs['ai_planning_owner_helped_executions'] 'ai_planning_owner_helped_executions') -ne 0) {
        throw "Receipt does not prove an authoritative parallel AI planning commit: $($Parsed.path)"
    }
    $aiDigest = ConvertTo-ReceiptUInt64 $pairs['ai_planning_digest'] 'ai_planning_digest'
    if ($aiDigest -eq 0 -or $aiDigest -ne [UInt64](Get-ReceiptAIPlanningDigest $Parsed)) {
        throw "Receipt AI planning digest is not canonical: $($Parsed.path)"
    }
    $expectedFrames = New-Object Collections.Generic.List[uint32]
    [void]$expectedFrames.Add(1)
    for ($frame = 32; $frame -le $CommonStopFrame; $frame += 32) {
        [void]$expectedFrames.Add([uint32]$frame)
    }
    for ($index = 0; $index -lt $expectedFrames.Count; ++$index) {
        $frame = ConvertTo-ReceiptUInt32 $pairs["checkpoint_${index}_frame"] "checkpoint_${index}_frame"
        if ($frame -ne $expectedFrames[$index]) {
            throw "Receipt checkpoint $index is not on the canonical 4096-frame boundary: $($Parsed.path)"
        }
        [void](ConvertTo-ReceiptUInt32 $pairs["checkpoint_${index}_crc"] "checkpoint_${index}_crc")
        [void](ConvertTo-ReceiptUInt64 $pairs["checkpoint_${index}_command_digest"] "checkpoint_${index}_command_digest")
    }
    $authorityDigest = Get-ReceiptCommandDigest $Parsed
    $lastDigest = ConvertTo-ReceiptUInt64 $pairs["checkpoint_128_command_digest"] 'checkpoint_128_command_digest'
    if ($authorityDigest -ne [Numerics.BigInteger]$lastDigest -or $lastDigest -eq 0) {
        throw "Receipt checkpoint command digest is not canonical: $($Parsed.path)"
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $count = ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_command_count"] "peer_${slot}_command_count"
        $first = ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_first_command_frame"] "peer_${slot}_first_command_frame"
        $last = ConvertTo-ReceiptUInt32 $pairs["peer_${slot}_last_command_frame"] "peer_${slot}_last_command_frame"
        $id = ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_last_command_id"] "peer_${slot}_last_command_id"
        $has = ConvertTo-ReceiptBool $pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $lastCommandDigest = ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_last_command_digest"] "peer_${slot}_last_command_digest"
        $commandDigest = ConvertTo-ReceiptUInt64 $pairs["peer_${slot}_command_digest"] "peer_${slot}_command_digest"
        if ($slot -lt $ExpectedPeerCount) {
            if ($count -lt 1 -or $first -lt 1 -or $first -gt $CommonStopFrame -or
                $last -lt $first -or $last -gt $CommonStopFrame -or
                -not $has -or $id -eq 0 -or $lastCommandDigest -eq 0 -or
                $commandDigest -eq 0) {
                throw "Peer $slot does not contribute at least one valid gameplay command in frames 1..4096: $($Parsed.path)"
            }
        }
        elseif ($count -ne 0 -or $first -ne 0 -or $last -ne 0 -or $id -ne 0 -or $has -or
            $lastCommandDigest -ne 0 -or $commandDigest -ne 0) {
            throw "Non-roster peer $slot has a command contribution: $($Parsed.path)"
        }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $mask = ConvertTo-ReceiptUInt64 $pairs["kernel_${kernel}_physical_worker_mask"] "kernel_${kernel}_physical_worker_mask"
        $jobs = ConvertTo-ReceiptUInt32 $pairs["kernel_${kernel}_physical_worker_jobs"] "kernel_${kernel}_physical_worker_jobs"
        $distinct = ConvertTo-ReceiptUInt32 $pairs["kernel_${kernel}_distinct_physical_workers"] "kernel_${kernel}_distinct_physical_workers"
        $peak = ConvertTo-ReceiptUInt32 $pairs["kernel_${kernel}_peak_concurrent_physical_workers"] "kernel_${kernel}_peak_concurrent_physical_workers"
        if (-not (ConvertTo-ReceiptBool $pairs["kernel_${kernel}_physical_worker_mask_complete"] "kernel_${kernel}_physical_worker_mask_complete") -or
            $mask -eq 0 -or $jobs -eq 0 -or $distinct -lt 2 -or $peak -lt 2 -or
            $distinct -ne (Get-BitCount $mask)) {
            throw "Kernel $kernel lacks complete executable-origin worker telemetry: $($Parsed.path)"
        }
    }
    return $Parsed
}

function Build-LockstepConfiguration {
    param(
        [int]$LocalSlot,
        [int]$PeerCount,
        [UInt64[]]$Ports,
        [string]$RunNonce,
        [string]$SessionNonce,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$MapName,
        [uint32]$MapCrc,
        [int]$Seed,
        [string]$Directory,
        [string]$ReceiptName
    )
    $portText = ($Ports | ForEach-Object { [string]$_ }) -join ','
    return ('peer={0};peers={1};ports={2};run={3};session={4};exe={5};source={6};map={7};map_crc={8};seed={9};dir={10};receipt={11};mode=trusted-router;router=0;network_roster={12};simulation_roster={13};ai_roster={14}' -f `
        $LocalSlot, $PeerCount, $portText, $RunNonce, $SessionNonce,
        $ExecutableSha256, $SourceCommit, $MapName, $MapCrc, $Seed,
        $Directory, $ReceiptName, $LockstepNetworkRosterMask,
        $LockstepSimulationRosterMask, $LockstepAIRosterMask)
}

function Build-LockstepNegativeProbeConfiguration {
    param(
        [string]$Mode,
        [int]$LocalSlot,
        [int]$PeerCount,
        [UInt64[]]$Ports,
        [string]$RunNonce,
        [string]$SessionNonce,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$MapName,
        [uint32]$MapCrc,
        [int]$Seed,
        [uint32]$ProbeBuildCrc,
        [uint32]$ProbeContentCrc,
        [string]$Directory,
        [string]$ReceiptName
    )
    if ($Mode -cne 'negative-cross-epoch' -and
        $Mode -cne 'negative-content-mismatch') {
        throw "Unsupported installed lockstep-v2 negative probe mode: $Mode"
    }
    if ($PeerCount -ne $LockstepNetworkPeerCount -or $LocalSlot -lt 0 -or
        $LocalSlot -ge $PeerCount -or $ProbeBuildCrc -eq 0 -or
        $ProbeContentCrc -eq 0) {
        throw 'Negative probe configuration does not satisfy the two-human contract.'
    }
    $portText = ($Ports | ForEach-Object { [string]$_ }) -join ','
    return ('peer={0};peers={1};ports={2};run={3};session={4};exe={5};source={6};map={7};map_crc={8};seed={9};dir={10};receipt={11};mode={12};router=0;build={13};content={14};network_roster={15};simulation_roster={16};ai_roster={17}' -f `
        $LocalSlot, $PeerCount, $portText, $RunNonce, $SessionNonce,
        $ExecutableSha256, $SourceCommit, $MapName, $MapCrc, $Seed,
        $Directory, $ReceiptName, $Mode, $ProbeBuildCrc, $ProbeContentCrc,
        $LockstepNetworkRosterMask, $LockstepSimulationRosterMask,
        $LockstepAIRosterMask)
}

function Get-ComparableReceiptHash {
    param([pscustomobject]$Parsed)
    $projection = Get-ReceiptProjection $Parsed -DeterministicOnly
    $bytes = [Text.Encoding]::UTF8.GetBytes($projection)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '') }
    finally { $sha.Dispose() }
}

function Invoke-LockstepSession {
    param(
        [string]$Title,
        [string]$Executable,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$SessionDirectory,
        [int]$PeerCount,
        [int]$BasePort,
        [string]$MapName,
        [uint32]$MapCrc,
        [int]$Seed,
        [int]$PeerTimeoutSeconds,
        [pscustomobject]$LauncherContract,
        [bool]$AllowHeadlessDirectExecution,
        [pscustomobject]$TitleSessionContract,
        [string]$ExecutionCohortNonce,
        [string]$ExecutionCohortCreatedUtc,
        [pscustomobject]$RuntimeClosure,
        [AllowNull()][scriptblock]$ProcessLaunchPendingObserver = $null,
        [AllowNull()][scriptblock]$ProcessStartObserver = $null,
        [AllowNull()][scriptblock]$ProcessExitObserver = $null
    )
    Assert-HeadlessDirectExecutionOptIn $AllowHeadlessDirectExecution
    if ($PeerCount -ne $LockstepNetworkPeerCount) {
        throw "Installed lockstep-v2 mixed qualification requires exactly $LockstepNetworkPeerCount network humans; local AI slots are not peers."
    }
    if ($null -eq $LauncherContract -or
        @($LauncherContract.launcherArguments).Count -eq 0) {
        throw "No validated launcher-equivalence contract was provided for $Title."
    }
    if ($null -eq $TitleSessionContract -or
        $TitleSessionContract.title -cne $Title) {
        throw "No validated H: title-session profile contract was provided for $Title."
    }
    $workingDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $Executable))
    if ($LauncherContract.directExecutable -cne $Executable -or
        $LauncherContract.directWorkingDirectory -cne $workingDirectory) {
        throw "Launcher-equivalence contract does not bind the direct $Title process identity."
    }
    if ((Get-UpperSha256 $LauncherContract.configPath) -cne $LauncherContract.configSha256 -or
        (Get-UpperSha256 $LauncherContract.launcherPath) -cne $LauncherContract.launcherSha256 -or
        (Get-UpperSha256 $Executable) -cne $ExecutableSha256) {
        throw "Installed $Title launcher/executable changed after contract validation."
    }
    $workerProfiles = @(Get-LockstepWorkerProfiles $PeerCount)
    Assert-MixedLockstepWorkerProfiles $workerProfiles
    [IO.Directory]::CreateDirectory($SessionDirectory) | Out-Null
    $ports = New-Object UInt64[] $PeerCount
    $usedPorts = @{}
    for ($index = 0; $index -lt $PeerCount; ++$index) {
        $ports[$index] = [UInt64]($BasePort + $index)
        if ($usedPorts.ContainsKey($ports[$index]) -or
            -not (Test-UdpPortAvailable ([int]$ports[$index]))) {
            throw "Lockstep-v2 UDP port $($ports[$index]) is unavailable or duplicated."
        }
        $usedPorts[$ports[$index]] = $true
    }
    $sessionNonce = New-NonceHex
    $runNonces = @()
    $seenNonces = @{}
    for ($index = 0; $index -lt $PeerCount; ++$index) {
        $runNonce = New-NonceHex
        if ($seenNonces.ContainsKey($runNonce)) { throw 'Nonce generator returned a duplicate process nonce.' }
        $seenNonces[$runNonce] = $true
        $runNonces += $runNonce
    }
    $processes = @()
    $lifecycleRecords = New-Object 'Collections.Generic.List[object]'
    $records = @()
    $workerEvidence = @{}
    try {
        for ($peer = 0; $peer -lt $PeerCount; ++$peer) {
            $receiptName = "lockstep-v2-$Title-peer-$peer.receipt"
            $configuration = Build-LockstepConfiguration $peer $PeerCount $ports `
                $runNonces[$peer] $sessionNonce $ExecutableSha256 $SourceCommit `
                $MapName $MapCrc $Seed $SessionDirectory $receiptName
            $stdout = Join-Path $SessionDirectory "peer-$peer.stdout.log"
            $stderr = Join-Path $SessionDirectory "peer-$peer.stderr.log"
            $workerProfile = $workerProfiles[$peer]
            $arguments = @($LauncherContract.launcherArguments +
                $workerProfile.overrideArguments + @(
                '-installedLockstepV2Validation', $configuration))
            $argumentString = ConvertTo-ProcessArgumentString $arguments
            $commandLine = '"{0}" {1}' -f $Executable, $argumentString
            $peerEnvironment = Get-LockstepPeerEnvironment $TitleSessionContract $peer
            foreach ($directory in @(
                $peerEnvironment.root, $peerEnvironment.values['TEMP'],
                $peerEnvironment.values['TMP'],
                $peerEnvironment.values['LOCALAPPDATA'],
                $peerEnvironment.values['APPDATA'],
                $peerEnvironment.values['RTS_STAGE5_VALIDATION_CACHE_ROOT'],
                $peerEnvironment.values['RTS_STAGE5_VALIDATION_LOG_ROOT'],
                $peerEnvironment.values['RTS_STAGE5_VALIDATION_DUMP_ROOT'])) {
                [IO.Directory]::CreateDirectory($directory) | Out-Null
            }
            if ($null -ne $ProcessLaunchPendingObserver) {
                & $ProcessLaunchPendingObserver (
                    New-LockstepRecoveryPendingIdentity $Executable $ExecutableSha256)
            }
            $environmentSnapshot = $null
            try {
                $executionEnvironment = Get-LockstepExecutionEnvironment `
                    $peerEnvironment $ExecutionCohortNonce `
                    $ExecutionCohortCreatedUtc $RuntimeClosure
                $environmentSnapshot = Set-LockstepProcessEnvironment $executionEnvironment
                try {
                    $process = New-LockstepSuspendedChild $Executable $argumentString `
                        $workingDirectory $stdout $stderr $ExecutableSha256
                }
                catch {
                    $retainedChild = Get-LockstepRetainedFailedChild $_.Exception
                    if ($null -ne $retainedChild) {
                        $process = $retainedChild
                        $lifecycle = New-LockstepProcessLifecycleRecord $retainedChild
                        $lifecycleRecords.Add($lifecycle) | Out-Null
                        $processes += $retainedChild
                    }
                    throw
                }
                # Retain the original Process object before publishing its
                # identity. If identity publication fails after Start, the
                # outer bounded cleanup must still stop this exact process.
                $lifecycle = New-LockstepProcessLifecycleRecord $process
                $lifecycleRecords.Add($lifecycle) | Out-Null
                $processes += $process
            }
            finally {
                if ($null -ne $environmentSnapshot) {
                    Restore-LockstepProcessEnvironment $environmentSnapshot
                }
            }
            $startedIdentity = New-LockstepRecoveryStartedIdentity `
                $process $ExecutableSha256
            $lifecycle.identity = $startedIdentity
            if ($null -ne $ProcessStartObserver) {
                & $ProcessStartObserver $startedIdentity
                $lifecycle.startProofPublished = $true
            }
            $process.Resume()
            $records += [pscustomobject]@{
                process = $process; peer = $peer; port = [int]$ports[$peer]
                runNonce = $runNonces[$peer]; sessionNonce = $sessionNonce
                receipt = Join-Path $SessionDirectory $receiptName
                stdout = $stdout; stderr = $stderr; configuration = $configuration
                arguments = @($arguments); argumentString = $argumentString
                commandLine = $commandLine; workerProfile = $workerProfile
                environment = $peerEnvironment; processIdentity = $startedIdentity
                lifecycle = $lifecycle
            }
        }
        $deadline = [datetime]::UtcNow.AddSeconds($PeerTimeoutSeconds)
        foreach ($record in $records) {
            Wait-ForLeaf $record.receipt $record.process $deadline
            $identity = $record.processIdentity
            if ($null -eq $identity -or [bool]$identity.launchPending) {
                throw "Peer $($record.peer) has no retained start identity for executable validation."
            }
            $observedPath = [IO.Path]::GetFullPath([string]$identity.executablePath)
            if ($observedPath -cne $Executable) {
                throw "Peer PID $($record.process.Id) is not the requested installed $Title executable."
            }
            if ([string]$identity.executableSha256 -cne
                    ([string]$ExecutableSha256).ToUpperInvariant()) {
                throw "Peer PID $($record.process.Id) executable hash changed during qualification."
            }
        }
        foreach ($record in $records) {
            $remaining = [Math]::Max(1, [int]($deadline - [datetime]::UtcNow).TotalMilliseconds)
            if (-not $record.process.WaitForExit($remaining)) {
                throw "Peer PID $($record.process.Id) exceeded the bounded qualification timeout."
            }
            if ($null -ne $ProcessExitObserver) {
                if ($null -eq $record.lifecycle -or
                    -not [bool]$record.lifecycle.startProofPublished) {
                    throw "Peer $($record.peer) exit proof lacks a published start identity."
                }
                $exitCode = [int]$record.process.ExitCode
                if (-not (Publish-LockstepProcessExitProof `
                        $record.lifecycle $ProcessExitObserver $exitCode)) {
                    throw "Peer $($record.peer) exit proof was not published."
                }
            }
            if ($record.process.ExitCode -ne 0) {
                $errorText = if (Test-Path -LiteralPath $record.stderr) {
                    [IO.File]::ReadAllText($record.stderr)
                } else { '' }
                throw "Peer PID $($record.process.Id) failed with exit $($record.process.ExitCode): $errorText"
            }
            $stdoutText = if (Test-Path -LiteralPath $record.stdout) {
                [IO.File]::ReadAllText($record.stdout)
            } else { '' }
            if ($stdoutText -notmatch 'LOCKSTEP_V2_VALIDATION_PASS' -or
                $stdoutText -match 'NET3_VALIDATION_PEER_PASS') {
                throw "Peer PID $($record.process.Id) did not publish an exclusive lockstep-v2 pass marker."
            }
            $stdoutProof = Get-LockstepStdoutProof $stdoutText $record.peer
            if ($stdoutProof.pid -ne $record.process.Id) {
                throw "Peer stdout PID $($stdoutProof.pid) does not match the observed process PID $($record.process.Id)."
            }
            $workerEvidence[$record.peer] = [pscustomobject]@{
                stdout = $stdoutProof
                receipt = $null
            }
        }
        $parsedReceipts = @()
        foreach ($record in $records) {
            $parsed = Get-ReceiptPairs $record.receipt
            Assert-LockstepV2Receipt $parsed $record.peer $PeerCount $MapCrc `
                $record.runNonce $record.sessionNonce $ExecutableSha256 $SourceCommit
            $workerEvidence[$record.peer].receipt =
                Get-LockstepReceiptWorkerTelemetry $parsed $record.workerProfile
            $parsedReceipts += $parsed
        }
        $effectiveWorkerCounts = @($records | ForEach-Object {
            $workerEvidence[$_.peer].receipt.effectiveWorkers
        })
        if (@($effectiveWorkerCounts | Select-Object -Unique).Count -lt 2) {
            throw 'Lockstep-v2 peers did not prove distinct effective worker counts.'
        }
        $tokens = @{}
        $projectionHash = $null
        foreach ($index in 0..($parsedReceipts.Count - 1)) {
            $parsed = $parsedReceipts[$index]
            $token = $parsed.pairs['network_session_token']
            if ($tokens.ContainsKey($token)) { throw 'Receipt network session tokens are not unique.' }
            $tokens[$token] = $true
            $hash = Get-ComparableReceiptHash $parsed
            if ($null -eq $projectionHash) { $projectionHash = $hash }
            elseif ($projectionHash -cne $hash) {
                throw "Lockstep-v2 peers disagree on the canonical frame/command/checkpoint projection."
            }
        }
        $raw = @()
        foreach ($record in $records) {
            $parsed = $parsedReceipts[$record.peer]
            $rawPath = Join-Path $SessionDirectory "peer-$($record.peer).raw.json"
            $rawDocument = [ordered]@{
                schemaVersion = 2
                producer = $LockstepProducer
                validationMode = $LockstepMode
                title = $Title
                processId = [int]$record.processIdentity.processId
                peer = $record.peer
                peerCount = $PeerCount
                # These masks make the network/simulation boundary explicit in
                # every peer artifact: only slots 0/1 are transport humans;
                # slots 2..5 are local skirmish-AI owners.
                networkRosterMask = $LockstepNetworkRosterMask
                simulationRosterMask = $LockstepSimulationRosterMask
                aiRosterMask = $LockstepAIRosterMask
                aiPlayerCount = $LockstepAIPlayerCount
                port = $record.port
                runNonce = $record.runNonce
                sessionNonce = $record.sessionNonce
                executableSha256 = $ExecutableSha256
                sourceCommit = $SourceCommit
                launcherEquivalence = $LauncherContract
                launcherPath = $LauncherContract.launcherPath
                launcherSha256 = $LauncherContract.launcherSha256
                launcherConfigPath = $LauncherContract.configPath
                launcherConfigSha256 = $LauncherContract.configSha256
                directExecutionOptIn = $AllowHeadlessDirectExecution
                workingDirectory = $workingDirectory
                commandLine = $record.commandLine
                arguments = @($record.arguments)
                launcherDefaultArguments = @($LauncherContract.launcherArguments)
                directArguments = @($record.arguments)
                workerOverride = $record.workerProfile
                stdoutProof = $workerEvidence[$record.peer].stdout
                receiptWorkerTelemetry = $workerEvidence[$record.peer].receipt
                requestedWorkers = $record.workerProfile.requestedWorkers
                workerPolicy = $record.workerProfile.workerPolicy
                effectiveWorkers = $workerEvidence[$record.peer].receipt.effectiveWorkers
                titleSessionProfile = $TitleSessionContract
                registryEquivalence = [pscustomobject]@{
                    strategy = 'process-local-validation-profile-root'
                    views = @($TitleSessionContract.registryViews)
                    values = @($TitleSessionContract.registryValues)
                    profileRoot = $TitleSessionContract.profileRoot
                }
                environmentEquivalence = $record.environment
                receiptPath = [IO.Path]::GetFileName($record.receipt)
                receiptSha256 = Get-UpperSha256 $record.receipt
                stdoutSha256 = if (Test-Path -LiteralPath $record.stdout) { Get-UpperSha256 $record.stdout } else { '' }
                stderrSha256 = if (Test-Path -LiteralPath $record.stderr) { Get-UpperSha256 $record.stderr } else { '' }
                exitCode = $record.process.ExitCode
                finalFrame = [int](ConvertTo-ReceiptUInt32 $parsed.pairs['final_frame'] 'final_frame')
                finalCRC = [UInt32](ConvertTo-ReceiptUInt32 $parsed.pairs['checkpoint_128_crc'] 'checkpoint_128_crc')
                comparableProjectionSha256 = $projectionHash
                lockstepV2Receipt = $true
                v1ReceiptAccepted = $false
            }
            Write-AtomicText $rawPath ($rawDocument | ConvertTo-Json -Depth 8)
            $raw += $rawDocument
        }
        return [pscustomobject]@{
            title = $Title
            mapCrc = $MapCrc
            peerCount = $PeerCount
            networkRosterMask = $LockstepNetworkRosterMask
            simulationRosterMask = $LockstepSimulationRosterMask
            aiRosterMask = $LockstepAIRosterMask
            aiPlayerCount = $LockstepAIPlayerCount
            ports = @($ports)
            sessionNonce = $sessionNonce
            launcherEquivalence = $LauncherContract
            titleSessionProfile = $TitleSessionContract
            registryEquivalence = [pscustomobject]@{
                strategy = 'process-local-validation-profile-root'
                views = @($TitleSessionContract.registryViews)
                values = @($TitleSessionContract.registryValues)
                profileRoot = $TitleSessionContract.profileRoot
            }
            workerProfiles = @($workerProfiles)
            effectiveWorkerCounts = @($effectiveWorkerCounts)
            mixedWorkerProof = $true
            comparableProjectionSha256 = $projectionHash
            peers = $raw
        }
    }
    finally {
        foreach ($lifecycle in $lifecycleRecords) {
            if ($null -eq $lifecycle -or $null -eq $lifecycle.process) {
                continue
            }
            $stopped = $false
            $exitCode = 0
            try {
                $exitCode = Stop-TaskPeer $lifecycle.process
                $stopped = $true
            }
            catch { }
            if ($stopped -and $null -ne $ProcessExitObserver -and
                [bool]$lifecycle.startProofPublished -and
                -not [bool]$lifecycle.exitProofPublished) {
                [void](Publish-LockstepProcessExitProof $lifecycle `
                    $ProcessExitObserver $exitCode -SuppressErrors)
            }
        }
    }
}

function Invoke-LockstepNegativeProbe {
    param(
        [string]$Title,
        [string]$Executable,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$EvidenceRoot,
        [string]$SessionDirectory,
        [int]$BasePort,
        [string]$MapName,
        [uint32]$MapCrc,
        [int]$Seed,
        [int]$PeerTimeoutSeconds,
        [pscustomobject]$LauncherContract,
        [bool]$AllowHeadlessDirectExecution,
        [pscustomobject]$TitleSessionContract,
        [string]$Mode,
        [string]$ExecutionCohortNonce,
        [string]$ExecutionCohortCreatedUtc,
        [pscustomobject]$RuntimeClosure,
        [AllowNull()][scriptblock]$ProcessLaunchPendingObserver = $null,
        [AllowNull()][scriptblock]$ProcessStartObserver = $null,
        [AllowNull()][scriptblock]$ProcessExitObserver = $null
    )
    Assert-HeadlessDirectExecutionOptIn $AllowHeadlessDirectExecution
    if ($Mode -cne 'negative-cross-epoch' -and
        $Mode -cne 'negative-content-mismatch') {
        throw "Unsupported installed lockstep-v2 negative probe mode: $Mode"
    }
    if ($null -eq $LauncherContract -or
        @($LauncherContract.launcherArguments).Count -eq 0 -or
        $null -eq $TitleSessionContract -or
        $TitleSessionContract.title -cne $Title) {
        throw "No validated launcher/title-session contract was provided for $Title negative probe."
    }
    $workingDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $Executable))
    if ($LauncherContract.directExecutable -cne $Executable -or
        $LauncherContract.directWorkingDirectory -cne $workingDirectory -or
        (Get-UpperSha256 $Executable) -cne $ExecutableSha256 -or
        (Get-UpperSha256 $LauncherContract.configPath) -cne $LauncherContract.configSha256 -or
        (Get-UpperSha256 $LauncherContract.launcherPath) -ne $LauncherContract.launcherSha256) {
        throw "Installed $Title launcher/executable identity changed before negative probe."
    }
    if ($BasePort -lt 1024 -or $BasePort + 1 -gt 65535) {
        throw 'Negative probe ports are outside the bounded UDP range.'
    }
    $ports = [UInt64[]]@($BasePort, $BasePort + 1)
    if (-not (Test-UdpPortAvailable $BasePort) -or
        -not (Test-UdpPortAvailable ($BasePort + 1))) {
        throw "Negative probe UDP ports $BasePort/$($BasePort + 1) are unavailable."
    }
    $negativeRoot = Join-Path $SessionDirectory 'NegativeProbes'
    [IO.Directory]::CreateDirectory($negativeRoot) | Out-Null
    $runNonce = New-NonceHex
    $sessionNonce = New-NonceHex
    $receiptName = if ($Mode -ceq 'negative-cross-epoch') {
        'cross-epoch.proof'
    }
    else { 'content-mismatch.proof' }
    $proofPath = Join-Path $negativeRoot $receiptName
    $stdoutPath = Join-Path $negativeRoot ($Mode + '.stdout.log')
    $stderrPath = Join-Path $negativeRoot ($Mode + '.stderr.log')
    foreach ($existing in @($proofPath, $stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $existing) {
            throw "Negative probe output was not fresh: $existing"
        }
    }
    $configuration = Build-LockstepNegativeProbeConfiguration $Mode 0 `
        $LockstepNetworkPeerCount $ports $runNonce $sessionNonce `
        $ExecutableSha256 $SourceCommit $MapName $MapCrc $Seed $MapCrc `
        ([uint32]$Seed) $negativeRoot $receiptName
    $arguments = @($LauncherContract.launcherArguments + @(
        '-installedLockstepV2Validation', $configuration))
    $argumentString = ConvertTo-ProcessArgumentString $arguments
    $commandLine = '"{0}" {1}' -f $Executable, $argumentString
    $peerEnvironment = Get-LockstepPeerEnvironment $TitleSessionContract 0
    foreach ($directory in @(
        $peerEnvironment.root, $peerEnvironment.values['TEMP'],
        $peerEnvironment.values['TMP'], $peerEnvironment.values['LOCALAPPDATA'],
        $peerEnvironment.values['APPDATA'],
        $peerEnvironment.values['RTS_STAGE5_VALIDATION_CACHE_ROOT'],
        $peerEnvironment.values['RTS_STAGE5_VALIDATION_LOG_ROOT'],
        $peerEnvironment.values['RTS_STAGE5_VALIDATION_DUMP_ROOT'])) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    $process = $null
    $exitCode = $null
    $lifecycle = New-LockstepProcessLifecycleRecord $null
    try {
        if ($null -ne $ProcessLaunchPendingObserver) {
            & $ProcessLaunchPendingObserver (
                New-LockstepRecoveryPendingIdentity $Executable $ExecutableSha256)
        }
        $environmentSnapshot = $null
        try {
            $executionEnvironment = Get-LockstepExecutionEnvironment `
                $peerEnvironment $ExecutionCohortNonce `
                $ExecutionCohortCreatedUtc $RuntimeClosure
            $environmentSnapshot = Set-LockstepProcessEnvironment $executionEnvironment
            try {
                $process = New-LockstepSuspendedChild $Executable $argumentString `
                    $workingDirectory $stdoutPath $stderrPath $ExecutableSha256
            }
            catch {
                $retainedChild = Get-LockstepRetainedFailedChild $_.Exception
                if ($null -ne $retainedChild) {
                    $process = $retainedChild
                    $lifecycle.process = $retainedChild
                }
                throw
            }
            $lifecycle.process = $process
        }
        finally {
            if ($null -ne $environmentSnapshot) {
                Restore-LockstepProcessEnvironment $environmentSnapshot
            }
        }
        $startedIdentity = New-LockstepRecoveryStartedIdentity `
            $process $ExecutableSha256
        $lifecycle.identity = $startedIdentity
        if ($null -ne $ProcessStartObserver) {
            & $ProcessStartObserver $startedIdentity
            $lifecycle.startProofPublished = $true
        }
        $process.Resume()
        $deadline = [datetime]::UtcNow.AddSeconds($PeerTimeoutSeconds)
        Wait-ForLeaf $proofPath $process $deadline
        if ($null -eq $lifecycle.identity -or
            -not [bool]$lifecycle.startProofPublished) {
            throw 'Negative probe has no published retained start identity.'
        }
        $startedIdentity = $lifecycle.identity
        $processId = [int]$startedIdentity.processId
        $processCreationUtc = [string]$startedIdentity.processCreationUtc
        $observedPath = [IO.Path]::GetFullPath(
            [string]$startedIdentity.executablePath)
        if ($observedPath -cne $Executable -or
            [string]$startedIdentity.executableSha256 -cne
                ([string]$ExecutableSha256).ToUpperInvariant()) {
            throw "Negative probe PID $processId did not run the exact installed $Title executable."
        }
        $remaining = [Math]::Max(1, [int]($deadline - [datetime]::UtcNow).TotalMilliseconds)
        if (-not $process.WaitForExit($remaining)) {
            throw "Negative probe PID $processId exceeded the bounded timeout."
        }
        if ($null -ne $ProcessExitObserver) {
            if (-not [bool]$lifecycle.startProofPublished) {
                throw 'Negative probe exit proof lacks a published start identity.'
            }
            $exitCode = [int]$process.ExitCode
            if (-not (Publish-LockstepProcessExitProof `
                    $lifecycle $ProcessExitObserver $exitCode)) {
                throw 'Negative probe exit proof was not published.'
            }
        }
        $observedExitCode = if ($null -ne $exitCode) {
            $exitCode
        } else { [int]$process.ExitCode }
        if ($observedExitCode -ne 0) {
            $errorText = if (Test-Path -LiteralPath $stderrPath) {
                [IO.File]::ReadAllText($stderrPath)
            } else { '' }
            throw "Negative probe PID $processId failed with exit $observedExitCode`: $errorText"
        }
        $stdoutText = if (Test-Path -LiteralPath $stdoutPath) {
            [IO.File]::ReadAllText($stdoutPath)
        } else { '' }
        $stdoutProof = Get-LockstepNegativeStdoutProof $stdoutText
        if ($stdoutProof.mode -cne $Mode -or $stdoutProof.pid -ne $processId) {
            throw "Negative probe stdout PID/mode does not match process $processId."
        }
        $parsed = Get-NegativeProbePairs $proofPath
        $pairs = $parsed.pairs
        $expectedError = if ($Mode -ceq 'negative-cross-epoch') {
            'UnsupportedEngineEpoch'
        }
        else { 'ContentHashMismatch' }
        $expectedMutation = if ($Mode -ceq 'negative-cross-epoch') {
            'engine-epoch'
        }
        else { 'content-hash' }
        $evidenceRootFull = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
        $proofFull = [IO.Path]::GetFullPath($proofPath)
        $stdoutFull = [IO.Path]::GetFullPath($stdoutPath)
        $stderrFull = [IO.Path]::GetFullPath($stderrPath)
        $evidencePrefix = $evidenceRootFull + '\'
        foreach ($path in @($proofFull, $stdoutFull, $stderrFull)) {
            if (-not $path.StartsWith($evidencePrefix,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Negative probe output escaped the native evidence root: $path"
            }
        }
        $entry = [ordered]@{
            title = $Title
            mode = $Mode
            producer = $LockstepProducer
            processId = $processId
            processCreationUtc = $processCreationUtc
            executablePath = $observedPath
            runNonce = $runNonce
            sessionNonce = $sessionNonce
            executableSha256 = $ExecutableSha256
            sourceCommit = $SourceCommit
            proofPath = $proofFull.Substring($evidencePrefix.Length).Replace('\', '/')
            proofSha256 = Get-UpperSha256 $proofFull
            stdoutPath = $stdoutFull.Substring($evidencePrefix.Length).Replace('\', '/')
            stdoutSha256 = Get-UpperSha256 $stdoutFull
            stderrPath = $stderrFull.Substring($evidencePrefix.Length).Replace('\', '/')
            stderrSha256 = Get-UpperSha256 $stderrFull
            inputSha256 = $pairs['input_sha256']
            baselineAccepted = ConvertTo-ReceiptBool $pairs['baseline_accepted'] 'baseline_accepted'
            mutatedAccepted = ConvertTo-ReceiptBool $pairs['mutated_accepted'] 'mutated_accepted'
            mutation = $pairs['mutation']
            expectedError = $pairs['expected_error']
            observedError = $pairs['observed_error']
            exitCode = $observedExitCode
            commandLine = $commandLine
            arguments = @($arguments)
            probeBuildCrc = [uint32](ConvertTo-ReceiptUInt32 $pairs['probe_build_compatibility_crc'] 'probe_build_compatibility_crc')
            probeContentCrc = [uint32](ConvertTo-ReceiptUInt32 $pairs['probe_content_crc'] 'probe_content_crc')
        }
        [void](Assert-LockstepNegativeProbeEvidence ([pscustomobject]$entry) `
            $EvidenceRoot $Title $Mode $SourceCommit $ExecutableSha256 $MapCrc $Seed `
            $Executable)
        return [pscustomobject]$entry
    }
    finally {
        if ($null -ne $process) {
            $stopped = $false
            $cleanupExitCode = 0
            try {
                $cleanupExitCode = Stop-TaskPeer $process
                $stopped = $true
            }
            catch { }
            if ($stopped -and $null -ne $ProcessExitObserver -and
                [bool]$lifecycle.startProofPublished -and
                -not [bool]$lifecycle.exitProofPublished) {
                [void](Publish-LockstepProcessExitProof $lifecycle `
                    $ProcessExitObserver $cleanupExitCode -SuppressErrors)
            }
        }
    }
}

# Keep archive qualification, final-acceptance writing, registry mutation, and
# process-environment mutation private to this module.  The canonical runner's
# existing self-test remains its owner, so only the pure/session-fixture helpers
# that self-test calls need compatibility exports; host topology, artifact
# closure, and the installed session set are the production caller boundaries.
Export-ModuleMember -Function `
    Set-Stage5LockstepHostSelfTestScratchRoot, `
    Get-Stage5InstalledLockstepV2HostTopology, `
    Get-UpperSha256, Test-CanonicalHex, Test-LowerHex40, `
    Assert-LockstepCanonicalUuid, Assert-LockstepRuntimeClosure, Assert-LockstepMapCrcs, `
    Resolve-BoundedArtifactPath, `
    Test-SafeHDirectory, Get-LockstepItemIfPresent, Assert-LockstepNoReparse, `
    Ensure-LockstepHostSelfTestDirectory, Remove-LockstepHostSelfTestTree, `
    Try-NewLockstepDirectoryJunction, Remove-LockstepDirectoryJunction, `
    Test-SafeMapName, Assert-HeadlessDirectExecutionOptIn, `
    Assert-X64PeExecutable, Get-LauncherRunContract, `
    Get-LockstepWorkerProfiles, Assert-MixedLockstepWorkerProfiles, `
    Get-LockstepStdoutProof, Get-LockstepReceiptWorkerTelemetry, `
    New-LockstepTitleSessionContract, Get-LockstepPeerEnvironment, `
    Test-LockstepNoActiveTitleProcesses, `
    Initialize-LockstepTitleSessionDirectories, Remove-LockstepTitleSessionDirectories, `
    Read-AndValidateArtifactSet, Write-AtomicText, Get-ReceiptPairs, Get-ReceiptCommandDigest, `
    Get-ReceiptAIPlanningDigest, `
    Assert-LockstepNegativeProbeEvidence, Assert-LockstepV2Receipt, Build-LockstepConfiguration, `
    Get-ComparableReceiptHash, `
    Invoke-Stage5InstalledLockstepV2SessionSet
