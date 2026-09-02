[CmdletBinding()]
param(
    [string]$GeneralsExecutable,
    [string]$ZeroHourExecutable,
    [string]$ArtifactSetManifestPath,
    [string]$SourceCommit,
    [string]$OutputDirectory,
    [string]$MapName,
    [uint32]$MapCrc,
    [ValidateRange(2, 8)][int]$PeerCount = 2,
    [ValidateRange(1, 2147483646)][int]$Seed = 23063,
    [ValidateRange(1024, 65000)][int]$BasePort = 41000,
    [ValidateRange(30, 1800)][int]$PeerTimeoutSeconds = 300,
    [switch]$AllowHeadlessDirectExecution,
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

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
$PostKillWaitMilliseconds = 5000

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

function Test-SafeHDirectory {
    param([string]$Path, [switch]$AllowWhitespace)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    $full = [IO.Path]::GetFullPath($Path)
    return $full.Length -ge 4 -and $full.Length -lt 248 -and
        $full.Substring(0, 1) -match '^[Hh]$' -and $full[1] -eq ':' -and
        ($full[2] -eq '\' -or $full[2] -eq '/') -and
        $full.IndexOf('..', [StringComparison]::Ordinal) -lt 0 -and
        $full.IndexOf(';', [StringComparison]::Ordinal) -lt 0 -and
        $full.IndexOf('"', [StringComparison]::Ordinal) -lt 0 -and
        ($AllowWhitespace -or $full -notmatch '\s')
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
    return $null -ne $Name -and $Name.Length -ge 4 -and $Name.Length -lt 248 -and
        $Name -notmatch '(^[\\/]|:|\.\.)' -and
        $Name.EndsWith('.map', [StringComparison]::OrdinalIgnoreCase) -and
        $Name -notmatch '\s'
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
        [IO.Path]::IsPathRooted($RelativePath)) {
        throw "Artifact path must be a nonempty manifest-relative path: $RelativePath"
    }
    $root = [IO.Path]::GetFullPath((Split-Path -Parent $Manifest)).TrimEnd('\') + '\'
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Artifact path escapes its manifest root: $RelativePath"
    }
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
    $homePath = $sessionFull.Substring(2)
    if (-not $homePath.StartsWith('')) { $homePath = '' + $homePath }
    $environmentValues = [ordered]@{
        TEMP = $tempRoot
        TMP = $tmpRoot
        LOCALAPPDATA = $localAppDataRoot
        APPDATA = $appDataRoot
        USERPROFILE = $sessionFull
        HOMEDRIVE = 'H:'
        HOMEPATH = $homePath
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = $cacheRoot
        RTS_STAGE5_VALIDATION_LOG_ROOT = $logRoot
        RTS_STAGE5_VALIDATION_DUMP_ROOT = $dumpRoot
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionFull
    }
    $registryValues = New-Object 'Collections.Generic.List[object]'
    $registryValues.Add([pscustomobject]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
    $registryValues.Add([pscustomobject]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
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
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'UserDataLeafName'; value = $profileLeaf; purpose = 'title-profile-leaf'
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
    $snapshot = [ordered]@{}
    foreach ($name in $Values.Keys) {
        $key = [string]$name
        $snapshot[$key] = [Environment]::GetEnvironmentVariable(
            $key, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable($key, [string]$Values[$name],
            [EnvironmentVariableTarget]::Process)
    }
    return $snapshot
}

function Restore-LockstepProcessEnvironment {
    param([Collections.IDictionary]$Snapshot)
    $errors = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $Snapshot.Keys) {
        try {
            [Environment]::SetEnvironmentVariable([string]$name,
                $(if ($null -eq $Snapshot[$name]) { $null } else { [string]$Snapshot[$name] }),
                [EnvironmentVariableTarget]::Process)
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
    foreach ($directory in @(
        $Contract.sessionRoot, $Contract.documentsRoot, $Contract.profileRoot,
        $Contract.peerRoot, $Contract.environmentValues['TEMP'],
        $Contract.environmentValues['TMP'],
        $Contract.environmentValues['LOCALAPPDATA'],
        $Contract.environmentValues['APPDATA'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_CACHE_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_LOG_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_DUMP_ROOT'])) {
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
    if (-not (Test-Path -LiteralPath $sessionFull)) { return }
    $rootItem = Get-Item -LiteralPath $sessionFull -Force
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
        'productSet', 'architecture', 'artifacts') 'Artifact set manifest'
    if ((Get-Stage5JsonValue $document 'schemaVersion' 'Artifact set manifest') -ne 1 -or
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
    param([string]$Path, [Diagnostics.Process]$Process, [datetime]$Deadline)
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
    param([Diagnostics.Process]$Process)
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
    }
    finally { $Process.Dispose() }
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

function Get-ReceiptProjection {
    param([pscustomobject]$Parsed)
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
         'ai_planning_owner_helped_executions',
         'ai_planning_digest')) {
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

function Get-ComparableReceiptHash {
    param([pscustomobject]$Parsed)
    $projection = Get-ReceiptProjection $Parsed
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
        [pscustomobject]$TitleSessionContract
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
            $environmentSnapshot = $null
            try {
                $environmentSnapshot = Set-LockstepProcessEnvironment $peerEnvironment.values
                $process = Start-Process -FilePath $Executable -ArgumentList $argumentString `
                    -WorkingDirectory $workingDirectory -PassThru `
                    -WindowStyle Hidden -RedirectStandardOutput $stdout `
                    -RedirectStandardError $stderr
            }
            finally {
                if ($null -ne $environmentSnapshot) {
                    Restore-LockstepProcessEnvironment $environmentSnapshot
                }
            }
            $processes += $process
            $records += [pscustomobject]@{
                process = $process; peer = $peer; port = [int]$ports[$peer]
                runNonce = $runNonces[$peer]; sessionNonce = $sessionNonce
                receipt = Join-Path $SessionDirectory $receiptName
                stdout = $stdout; stderr = $stderr; configuration = $configuration
                arguments = @($arguments); argumentString = $argumentString
                commandLine = $commandLine; workerProfile = $workerProfile
                environment = $peerEnvironment
            }
        }
        $deadline = [datetime]::UtcNow.AddSeconds($PeerTimeoutSeconds)
        foreach ($record in $records) {
            Wait-ForLeaf $record.receipt $record.process $deadline
            $record.process.Refresh()
            $observedPath = [IO.Path]::GetFullPath($record.process.Path)
            if ($observedPath -cne $Executable) {
                throw "Peer PID $($record.process.Id) is not the requested installed $Title executable."
            }
            if ((Get-UpperSha256 $observedPath) -cne $ExecutableSha256) {
                throw "Peer PID $($record.process.Id) executable hash changed during qualification."
            }
        }
        foreach ($record in $records) {
            $remaining = [Math]::Max(1, [int]($deadline - [datetime]::UtcNow).TotalMilliseconds)
            if (-not $record.process.WaitForExit($remaining)) {
                throw "Peer PID $($record.process.Id) exceeded the bounded qualification timeout."
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
                processId = $record.process.Id
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
                    strategy = 'known-folder-registry-redirect'
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
                strategy = 'known-folder-registry-redirect'
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
        foreach ($process in $processes) {
            if ($null -ne $process) {
                try { Stop-TaskPeer $process } catch { }
            }
        }
    }
}

function New-SyntheticReceiptText {
    param([int]$LocalSlot = 0, [int]$PeerCount = 2,
        [string]$RunNonce = '0123456789ABCDEF0123456789ABCDEF',
        [string]$SessionNonce = 'ABCDEF0123456789ABCDEF0123456789',
        [string]$ExecutableSha256 = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA',
        [string]$SourceCommit = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
        [int]$PhysicalWorkerCount = 2)
    $lines = New-Object Collections.Generic.List[string]
    [void]$lines.Add($LockstepMagic)
    [void]$lines.Add("producer=$LockstepProducer")
    [void]$lines.Add("mode=$LockstepMode")
    foreach ($line in @(
        'schema=2', 'protocol_epoch=2', "local_slot=$LocalSlot", "peer_count=$PeerCount",
        "roster_mask=$LockstepNetworkRosterMask",
        "simulation_roster_mask=$LockstepSimulationRosterMask",
        "ai_roster_mask=$LockstepAIRosterMask", 'build_compatibility_crc=1',
        'content_crc=1', 'map_crc=1', "common_stop_frame=$CommonStopFrame",
        "proven_kernel_mask=$LockstepAuthorityMask", 'packet_router_slot=0',
        'origin_mode=2', "run_nonce=$RunNonce", "session_nonce=$SessionNonce",
        "executable_sha256=$ExecutableSha256", "source_revision=$SourceCommit",
        'network_session_token=1', "final_frame=$CommonStopFrame",
        "frame_count=$CommonStopFrame", "contributed_peer_mask=$((1 -shl $PeerCount) - 1)",
        "checkpoint_count=$LockstepCheckpointCount", "validation_authority_mask=$LockstepAuthorityMask",
        'executable_origin=1', 'worker_telemetry_executable_origin=1',
        'transport_path_used=1', 'handshake_validated=1', 'clean_shutdown=1',
        'ai_planning_captured_snapshots=4', 'ai_planning_captured_candidates=16',
        'ai_planning_requested_batches=2', 'ai_planning_submitted_jobs=8',
        'ai_planning_completed_jobs=8', 'ai_planning_serial_fallbacks=0',
        'ai_planning_shadow_matches=8', 'ai_planning_shadow_mismatches=0',
        'ai_planning_validation_failures=0',
        'ai_planning_canonical_validation_invocations=2',
        'ai_planning_committed_batches=2',
        'ai_planning_parallel_authoritative_commits=2',
        'ai_planning_rejected_commits=0',
        'ai_planning_physical_worker_executions=8',
        'ai_planning_owner_helped_executions=0',
        "ai_planning_observed_physical_worker_mask=$((1 -shl $PhysicalWorkerCount) - 1)",
        "ai_planning_maximum_distinct_physical_workers=$PhysicalWorkerCount",
        "ai_planning_maximum_concurrent_physical_workers=$PhysicalWorkerCount",
        'ai_planning_digest=1')) {
        [void]$lines.Add($line)
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $count = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        $first = if ($slot -lt $PeerCount) { 8 } else { 0 }
        $last = if ($slot -lt $PeerCount) { 8 + $slot } else { 0 }
        $has = if ($slot -lt $PeerCount) { 1 } else { 0 }
        $id = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        $digest = if ($slot -lt $PeerCount) { $slot + 1 } else { 0 }
        [void]$lines.Add("peer_${slot}_command_count=$count")
        [void]$lines.Add("peer_${slot}_first_command_frame=$first")
        [void]$lines.Add("peer_${slot}_last_command_frame=$last")
        [void]$lines.Add("peer_${slot}_last_command_id=$id")
        [void]$lines.Add("peer_${slot}_has_last_command_id=$has")
        [void]$lines.Add("peer_${slot}_last_command_digest=$digest")
        [void]$lines.Add("peer_${slot}_command_digest=$digest")
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        [void]$lines.Add("kernel_${kernel}_physical_worker_mask=$((1 -shl $PhysicalWorkerCount) - 1)")
        [void]$lines.Add("kernel_${kernel}_physical_worker_jobs=64")
        [void]$lines.Add("kernel_${kernel}_distinct_physical_workers=$PhysicalWorkerCount")
        [void]$lines.Add("kernel_${kernel}_peak_concurrent_physical_workers=$PhysicalWorkerCount")
        [void]$lines.Add("kernel_${kernel}_physical_worker_mask_complete=1")
    }
    for ($checkpoint = 0; $checkpoint -lt $LockstepCheckpointCount; ++$checkpoint) {
        $frame = if ($checkpoint -eq 0) { 1 } else { $checkpoint * 32 }
        [void]$lines.Add("checkpoint_${checkpoint}_frame=$frame")
        [void]$lines.Add("checkpoint_${checkpoint}_crc=1")
        [void]$lines.Add("checkpoint_${checkpoint}_command_digest=1")
    }
    [void]$lines.Add('END')
    return (($lines.ToArray() -join "`n") + "`n")
}

function Invoke-SelfTest {
    if (-not (Test-CanonicalHex ('A' * 32) 32) -or
        (Test-CanonicalHex ('A' * 31) 32) -or
        -not (Test-LowerHex40 ('a' * 40)) -or
        (Test-LowerHex40 ('A' * 40))) {
        throw 'Lockstep-v2 host nonce/source lexical self-test failed.'
    }
    $root = 'H:\GGC-LockstepV2HostSelfTest-' + [guid]::NewGuid().ToString('N')
    [IO.Directory]::CreateDirectory($root) | Out-Null
    try {
        $runtime = Join-Path $root 'runtime'
        [IO.Directory]::CreateDirectory($runtime) | Out-Null
        $fixtureExecutable = Join-Path $runtime 'generalsv.exe'
        $fixtureLauncher = Join-Path $runtime 'launcher.exe'
        $fixtureConfig = Join-Path $runtime 'launcher.lcf'
        [IO.File]::WriteAllText($fixtureExecutable, 'fixture executable')
        [IO.File]::WriteAllText($fixtureLauncher, 'fixture launcher')
        [IO.File]::WriteAllText($fixtureConfig,
            'RUN = . generalsv.exe -simulationMode parallel -workerPolicy auto')
        $launcher = Get-LauncherRunContract $fixtureConfig $fixtureLauncher $runtime `
            $fixtureExecutable ('A' * 64) ('B' * 64)
        if ($launcher.directory -cne '.' -or
            $launcher.executable -cne 'generalsv.exe' -or
            (@($launcher.launcherArguments) -join '|') -cne
                '-simulationMode|parallel|-workerPolicy|auto') {
            throw 'Lockstep-v2 launcher-equivalence self-test did not retain Stage 5 defaults.'
        }
        $x64Rejected = $false
        try { Assert-X64PeExecutable $fixtureExecutable }
        catch { $x64Rejected = $true }
        if (-not $x64Rejected) {
            throw 'Lockstep-v2 host self-test accepted a non-PE installed executable fixture.'
        }
        $missingSwitchRejected = $false
        try { Assert-HeadlessDirectExecutionOptIn $false }
        catch { $missingSwitchRejected = $true }
        if (-not $missingSwitchRejected) {
            throw 'Lockstep-v2 host self-test accepted direct execution without its opt-in switch.'
        }
        $workerProfiles = @(Get-LockstepWorkerProfiles 2)
        Assert-MixedLockstepWorkerProfiles $workerProfiles
        if ((@($workerProfiles[0].overrideArguments) -join '|') -cne
                '-workerCount|2|-workerPolicy|all' -or
            (@($workerProfiles[1].overrideArguments) -join '|') -cne
                '-workerPolicy|auto') {
            throw 'Lockstep-v2 worker self-test did not bind distinct reviewed overrides.'
        }
        $homogeneousRejected = $false
        try { Assert-MixedLockstepWorkerProfiles @($workerProfiles[0], $workerProfiles[0]) }
        catch { $homogeneousRejected = $true }
        if (-not $homogeneousRejected) {
            throw 'Lockstep-v2 host self-test accepted homogeneous worker profiles.'
        }
        $titleSession = New-LockstepTitleSessionContract 'Generals' `
            'H:\GGC-LockstepV2HostSelfTest-TitleSession' `
            'H:\GGC-LockstepV2HostSelfTest-Runtime'
        $peerEnvironment0 = Get-LockstepPeerEnvironment $titleSession 0
        $peerEnvironment1 = Get-LockstepPeerEnvironment $titleSession 1
        if ([IO.Path]::GetPathRoot($titleSession.profileRoot) -cne 'H:\' -or
            @($titleSession.registryValues).Count -ne 3 -or
            $peerEnvironment0.values['TEMP'] -ceq $peerEnvironment1.values['TEMP'] -or
            [IO.Path]::GetPathRoot($peerEnvironment0.values['TEMP']) -cne 'H:\' -or
            @($titleSession.environmentVariableNames) -notcontains 'TEMP') {
            throw 'Lockstep-v2 host self-test did not isolate H: profile/environment paths.'
        }
        $scriptText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot `
            'Invoke-InstalledLockstepV2Validation.ps1'))
        if ($scriptText -notmatch 'Restore-LockstepRegistrySnapshots' -or
            $scriptText -notmatch 'Set-LockstepProcessEnvironment' -or
            $scriptText -notmatch 'Assert-LockstepProfileReadOnly' -or
            $scriptText -notmatch 'Remove-LockstepTitleSessionDirectories') {
            throw 'Lockstep-v2 host self-test did not retain registry/environment cleanup guards.'
        }
        $escapeCleanupRejected = $false
        try { Remove-LockstepTitleSessionDirectories $titleSession $root }
        catch { $escapeCleanupRejected = $true }
        if (-not $escapeCleanupRejected) {
            throw 'Lockstep-v2 host self-test accepted a title-session cleanup path outside the output root.'
        }
        $cleanupRoot = Join-Path $root 'TitleSession'
        $cleanupContract = New-LockstepTitleSessionContract 'Generals' $cleanupRoot $runtime
        Initialize-LockstepTitleSessionDirectories $cleanupContract
        [IO.File]::WriteAllText((Join-Path $cleanupContract.profileRoot 'transient.bin'), 'transient')
        $cleanupRan = $false
        try {
            try { throw 'synthetic qualification failure' }
            finally {
                Remove-LockstepTitleSessionDirectories $cleanupContract $root
                $cleanupRan = $true
            }
        }
        catch {
            if (-not $cleanupRan) {
                throw "Lockstep-v2 host self-test could not clean a thrown qualification path: $($_.Exception.Message)"
            }
        }
        if (Test-Path -LiteralPath $cleanupRoot) {
            throw 'Lockstep-v2 host self-test left disposable title-session directories behind.'
        }
        $stdoutProof = Get-LockstepStdoutProof `
            "LOCKSTEP_V2_VALIDATION_ACTIVE peer=0 frame_limit=4096`nLOCKSTEP_V2_VALIDATION_PASS peer=0 pid=1 frame=4096 crc=00000001`n" 0
        if (-not $stdoutProof.executableOrigin -or $stdoutProof.pid -ne 1 -or
            $stdoutProof.frameLimit -ne 4096) {
            throw 'Lockstep-v2 host self-test did not bind executable-origin stdout markers.'
        }
        $negativeLauncherCases = @(
            'RUN = . other.exe -simulationMode parallel -workerPolicy auto',
            'RUN = child generalsv.exe -simulationMode parallel -workerPolicy auto',
            'RUN = . generalsv.exe -unsupported value')
        foreach ($badLine in $negativeLauncherCases) {
            [IO.File]::WriteAllText($fixtureConfig, $badLine)
            $rejected = $false
            try {
                [void](Get-LauncherRunContract $fixtureConfig $fixtureLauncher $runtime `
                    $fixtureExecutable ('A' * 64) ('B' * 64))
            }
            catch { $rejected = $true }
            if (-not $rejected) {
                throw "Lockstep-v2 launcher parser accepted a negative fixture: $badLine"
            }
        }
        $receiptPath = Join-Path $root 'synthetic.receipt'
        $text = New-SyntheticReceiptText
        [IO.File]::WriteAllText($receiptPath, $text, (New-Object Text.UTF8Encoding($false)))
        $parsed = Get-ReceiptPairs $receiptPath
        $pairs = $parsed.pairs
        if ($pairs['schema'] -cne '2' -or
            $pairs['common_stop_frame'] -cne '4096' -or
            $pairs['final_frame'] -cne '4096' -or
            $pairs['frame_count'] -cne '4096' -or
            $pairs['checkpoint_count'] -cne '129' -or
            $pairs['proven_kernel_mask'] -cne '63' -or
            $pairs['validation_authority_mask'] -cne '63') {
            throw 'Lockstep-v2 host parser did not retain schema/checkpoint fields.'
        }
        $digest = Get-ReceiptCommandDigest $parsed
        $text = $text.Replace('checkpoint_128_command_digest=1',
            "checkpoint_128_command_digest=$digest")
        [IO.File]::WriteAllText($receiptPath, $text,
            (New-Object Text.UTF8Encoding($false)))
        $parsed = Get-ReceiptPairs $receiptPath
        $aiDigest = Get-ReceiptAIPlanningDigest $parsed
        $text = $text.Replace('ai_planning_digest=1',
            "ai_planning_digest=$aiDigest")
        [IO.File]::WriteAllText($receiptPath, $text,
            (New-Object Text.UTF8Encoding($false)))
        $parsed = Get-ReceiptPairs $receiptPath
        [void](Assert-LockstepV2Receipt $parsed 0 2 1 `
            '0123456789ABCDEF0123456789ABCDEF' `
            'ABCDEF0123456789ABCDEF0123456789' `
            ('A' * 64) ('a' * 40))
        $explicitTelemetry = Get-LockstepReceiptWorkerTelemetry $parsed $workerProfiles[0]
        if ($explicitTelemetry.effectiveWorkers -ne 2) {
            throw 'Lockstep-v2 host self-test did not retain forced-two worker telemetry.'
        }
        $claimedMismatchRejected = $false
        try { [void](Get-LockstepReceiptWorkerTelemetry $parsed $workerProfiles[1]) }
        catch { $claimedMismatchRejected = $true }
        if (-not $claimedMismatchRejected) {
            throw 'Lockstep-v2 host self-test accepted an auto profile with forced-two telemetry.'
        }
        $peerReceiptPath = Join-Path $root 'synthetic-peer.receipt'
        $peerText = New-SyntheticReceiptText -LocalSlot 1 -PhysicalWorkerCount 4 `
            -RunNonce 'FEDCBA9876543210FEDCBA9876543210'
        [IO.File]::WriteAllText($peerReceiptPath, $peerText,
            (New-Object Text.UTF8Encoding($false)))
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        $peerDigest = Get-ReceiptCommandDigest $peerParsed
        $peerText = $peerText.Replace('checkpoint_128_command_digest=1',
            "checkpoint_128_command_digest=$peerDigest")
        [IO.File]::WriteAllText($peerReceiptPath, $peerText,
            (New-Object Text.UTF8Encoding($false)))
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        $peerAIDigest = Get-ReceiptAIPlanningDigest $peerParsed
        $peerText = $peerText.Replace('ai_planning_digest=1',
            "ai_planning_digest=$peerAIDigest")
        [IO.File]::WriteAllText($peerReceiptPath, $peerText,
            (New-Object Text.UTF8Encoding($false)))
        $peerParsed = Get-ReceiptPairs $peerReceiptPath
        [void](Assert-LockstepV2Receipt $peerParsed 1 2 1 `
            'FEDCBA9876543210FEDCBA9876543210' `
            'ABCDEF0123456789ABCDEF0123456789' `
            ('A' * 64) ('a' * 40))
        $automaticTelemetry = Get-LockstepReceiptWorkerTelemetry $peerParsed $workerProfiles[1]
        if ($automaticTelemetry.effectiveWorkers -ne 4 -or
            (Get-ComparableReceiptHash $parsed) -cne (Get-ComparableReceiptHash $peerParsed)) {
            throw 'Lockstep-v2 host self-test did not preserve mixed-worker receipt/projection semantics.'
        }
        $explicitMismatchRejected = $false
        try { [void](Get-LockstepReceiptWorkerTelemetry $peerParsed $workerProfiles[0]) }
        catch { $explicitMismatchRejected = $true }
        if (-not $explicitMismatchRejected) {
            throw 'Lockstep-v2 host self-test accepted an explicit-two profile with four-worker telemetry.'
        }
        $mutated = $text.Replace($LockstepMagic, 'RTS_NET3_RECEIPT')
        [IO.File]::WriteAllText($receiptPath, $mutated, (New-Object Text.UTF8Encoding($false)))
        $rejected = $false
        try { [void](Get-ReceiptPairs $receiptPath) } catch { $rejected = $true }
        if (-not $rejected) { throw 'Host parser accepted a v1 receipt magic.' }
        $sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
        $cmake = [IO.File]::ReadAllText((Join-Path $sourceRoot 'Core\Tools\DeterministicSimulationValidation\CMakeLists.txt'))
        if ($cmake -notmatch 'core_installed_lockstep_v2_host_self_test' -or
            $cmake -notmatch 'NOT IS_VS6_BUILD') {
            throw 'Lockstep-v2 host self-test is not x64/non-VC6 registered.'
        }
        Write-Output 'LOCKSTEP_V2_HOST_SELF_TEST_PASS'
    }
    finally {
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

Assert-HeadlessDirectExecutionOptIn ([bool]$AllowHeadlessDirectExecution)
if (-not (Test-LowerHex40 $SourceCommit)) {
    throw 'SourceCommit must be the exact lowercase 40-hex revision.'
}
if ([string]::IsNullOrWhiteSpace($MapName) -or -not (Test-SafeMapName $MapName) -or
    $MapCrc -eq 0 -or $PeerCount -ne $LockstepNetworkPeerCount) {
    throw 'MapName/MapCrc/PeerCount do not form the bounded two-human/four-local-AI installed lockstep-v2 contract.'
}
if (-not (Test-SafeHDirectory $OutputDirectory) -or (Test-Path -LiteralPath $OutputDirectory)) {
    throw 'OutputDirectory must be a fresh task-owned directory on H:.'
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
$artifactSet = Read-AndValidateArtifactSet $ArtifactSetManifestPath $SourceCommit
$generalsFull = [IO.Path]::GetFullPath($GeneralsExecutable)
$zeroHourFull = [IO.Path]::GetFullPath($ZeroHourExecutable)
if ($generalsFull -cne $artifactSet.artifacts['generals-executable'].path -or
    $zeroHourFull -cne $artifactSet.artifacts['zerohour-executable'].path) {
    throw 'Requested executables must be the exact installed x64 artifact-set executables.'
}
Assert-X64PeExecutable $generalsFull
Assert-X64PeExecutable $zeroHourFull
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
if ($BasePort + (2 * $PeerCount) - 1 -gt 65535) {
    throw 'BasePort and PeerCount would exceed the 16-bit UDP port range.'
}
$allPorts = @()
$sessionResults = @()
$usedNonces = @{}
$registrySnapshots = New-Object 'Collections.Generic.List[object]'
$registrySnapshotKeys = @{}
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
        $titleSessionRoot = Join-Path $titleRoot 'TitleSession'
        $titleSession = New-LockstepTitleSessionContract $title $titleSessionRoot `
            (Split-Path -Parent ([string]$artifactSet.artifacts[$role].path))
        $titleSessions += $titleSession
        Initialize-LockstepTitleSessionDirectories $titleSession
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
            [Microsoft.Win32.RegistryView]::Registry64)) {
            foreach ($registryValue in $titleSession.registryValues) {
                Set-LockstepRegistryValue $view $registryValue.subKey `
                    $registryValue.name $registryValue.value $registrySnapshots `
                    $registrySnapshotKeys
            }
        }
        $sessionResults += Invoke-LockstepSession $title `
            ([string]$artifactSet.artifacts[$role].path) `
            $executables[$title] $SourceCommit $titleRoot $PeerCount `
            ($BasePort + ($titleIndex * $PeerCount)) $MapName $MapCrc $Seed $PeerTimeoutSeconds `
            $launcherContracts[$title] ([bool]$AllowHeadlessDirectExecution) $titleSession
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
    }
    $evidence = [ordered]@{
        schemaVersion = 2
        evidenceKind = 'lockstep-v2-multiplayer'
        status = 'passed'
        producer = $LockstepProducer
        validationMode = $LockstepMode
        architecture = 'x64'
        sourceCommit = $SourceCommit
        artifactSetSha256 = $artifactSet.sha256
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        allowHeadlessDirectExecution = [bool]$AllowHeadlessDirectExecution
        launcherEquivalence = $launcherContracts
        commonStopFrame = $CommonStopFrame
        peerCount = $PeerCount
        networkRosterMask = $LockstepNetworkRosterMask
        simulationRosterMask = $LockstepSimulationRosterMask
        aiRosterMask = $LockstepAIRosterMask
        aiPlayerCount = $LockstepAIPlayerCount
        mapName = $MapName
        mapCrc = $MapCrc
        seed = $Seed
        v1Accepted = $false
        profileStrategy = 'known-folder-registry-redirect'
        registryViews = @('Registry32', 'Registry64')
        environmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
            'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
            'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
            'RTS_STAGE5_VALIDATION_CACHE_ROOT',
            'RTS_STAGE5_VALIDATION_LOG_ROOT',
            'RTS_STAGE5_VALIDATION_DUMP_ROOT')
        profileConcurrency = 'shared-title-profile-read-only'
        titleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
        sessions = $sessionResults
    }
}
finally {
    $cleanupErrors = @()
    try {
        Restore-LockstepRegistrySnapshots @($registrySnapshots.ToArray())
    }
    catch {
        $cleanupErrors += "registry restoration: $($_.Exception.Message)"
    }
    foreach ($titleSessionToClean in $titleSessions) {
        try {
            Remove-LockstepTitleSessionDirectories $titleSessionToClean $outputFull
        }
        catch {
            $cleanupErrors += "$($titleSessionToClean.title) title-session cleanup: $($_.Exception.Message)"
        }
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "Lockstep-v2 final cleanup failed: $($cleanupErrors -join ' | ')"
    }
}
$evidencePath = Join-Path $outputFull 'LockstepV2LoopbackEvidence.json'
Write-AtomicText $evidencePath ($evidence | ConvertTo-Json -Depth 12)
Write-Output ("LOCKSTEP_V2_HOST_PASS sessions={0} peers={1} frame={2}" -f `
    $sessionResults.Count, $PeerCount, $CommonStopFrame)
