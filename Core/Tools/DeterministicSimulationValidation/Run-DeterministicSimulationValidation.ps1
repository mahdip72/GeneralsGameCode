[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RuntimeRoot,
    [Parameter(Mandatory = $true)][string]$FixtureManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$TaskRoot = '',
    [string]$CorpusExportRoot = '',
    [ValidateSet('Replay', 'AI', 'All')][string]$ValidationSet = 'All',
    [ValidateSet('Canonical', 'LocalCapacity')][string]$CapacityMode = 'Canonical',
    [int]$ReplayMatrixRepeats = 2,
    [int]$StressRepeats = 3,
    [int]$ReplayTimeoutSeconds = 600,
    [int]$AiTimeoutSeconds = 1800,
    [UInt64]$MinimumFreeBytes = 10737418240,
    [string]$ExpectedExecutableSha256 = '',
    [string]$ProfileLeafName = '',
    [string]$GeneralsInstallRoot = '',
    [switch]$AllowNonStandardCorpus,
    [switch]$PlanOnly,
    [switch]$DisableFrameTiming,
    [switch]$DiagnosticNonAcceptance,
    [Alias('HeadlessDirectExecutionException')]
    [switch]$AllowHeadlessDirectExecution,
    [switch]$RequireX64,
    [switch]$EnforcePerformance,
    [ValidateSet('Generals', 'ZeroHour')][string]$Title = 'ZeroHour',
    [string]$Stage3PerformanceBaselinePath = '',
    [string]$ExpectedStage3ExecutableSha256 = '',
    [string]$AcceptanceSourceCommit = '',
    [string]$AcceptanceArtifactSetSha256 = '',
    [string]$ExecutionCohortNonce = '',
    [string]$ExecutionCohortCreatedUtc = '',
    [string]$AcceptanceRuntimeDependencyManifestSha256 = '',
    [string]$AcceptanceRuntimeClosureSha256 = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'Stage5ReplayCorpusExporter.psm1') -Force

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-Sha256Text {
    param([Parameter(Mandatory = $true)][string]$Value)
    $encoding = New-Object Text.UTF8Encoding($false)
    $bytes = $encoding.GetBytes($Value)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Sha256Bytes {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Stage5FileSnapshot {
    param([string]$Path, [string]$Context)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "$Context file was not found: $full"
    Assert-ContainedPathNoReparse (Split-Path -Parent $full) $full $Context | Out-Null
    $before = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-Condition (($before -is [IO.FileInfo]) -and
        (($before.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) `
        "$Context file is not a regular non-reparse file: $full"
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $beforeLength = [Int64]$before.Length
        Assert-Condition ($stream.Length -eq $beforeLength) `
            "$Context file changed before its immutable copy began: $full"
        $memory = New-Object IO.MemoryStream
        try {
            $stream.CopyTo($memory)
            $bytes = $memory.ToArray()
        }
        finally { $memory.Dispose() }
        }
    finally { $stream.Dispose() }
    $after = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-Condition (($after -is [IO.FileInfo]) -and
        (($after.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
        [Int64]$after.Length -eq $beforeLength -and
        [Int64]$bytes.LongLength -eq $beforeLength -and
        [DateTime]$after.CreationTimeUtc -eq [DateTime]$before.CreationTimeUtc -and
        [DateTime]$after.LastWriteTimeUtc -eq [DateTime]$before.LastWriteTimeUtc) `
        "$Context file changed or was replaced while it was snapshotted: $full"
    return [pscustomobject]@{
        path = $full
        bytes = $bytes
        sha256 = Get-Sha256Bytes $bytes
        length = $beforeLength
        creationTimeUtc = ([DateTime]$before.CreationTimeUtc).ToString('o')
        lastWriteTimeUtc = ([DateTime]$before.LastWriteTimeUtc).ToString('o')
        identity = '{0}|{1}|{2}' -f $beforeLength,
            ([DateTime]$before.CreationTimeUtc).Ticks,
            ([DateTime]$before.LastWriteTimeUtc).Ticks
    }
}

function ConvertTo-UtcIsoTimestamp {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return '' }
    if ($Value -is [DateTimeOffset]) {
        return $Value.ToUniversalTime().ToString('o')
    }
    if ($Value -is [DateTime]) {
        return $Value.ToUniversalTime().ToString('o')
    }
    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) { return '' }
    try {
        $date = [Management.ManagementDateTimeConverter]::ToDateTime($text)
        return $date.ToUniversalTime().ToString('o')
    }
    catch { }
    try {
        [DateTimeOffset]$parsed = [DateTimeOffset]::MinValue
        if ([DateTimeOffset]::TryParse($text, [ref]$parsed)) {
            return $parsed.ToUniversalTime().ToString('o')
        }
    }
    catch { }
    return ''
}

function ConvertTo-OutputRelativePath {
    param([string]$Path, [string]$OutputRoot, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Path)) "$Context path is empty."
    $rootFull = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($Path)
    $rootValue = Get-Item -LiteralPath $rootFull -Force -ErrorAction Stop
    Assert-Condition (($rootValue.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context output root is a reparse point: $rootFull"
    $rootDrive = [IO.Path]::GetPathRoot($rootFull)
    $candidateDrive = [IO.Path]::GetPathRoot($candidate)
    Assert-Condition ($rootDrive -is [string] -and $candidateDrive -is [string] -and
        $rootDrive.Equals($candidateDrive, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume: $candidate"
    $rootParts = @($rootFull.Substring($rootDrive.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateDrive.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $inside = $candidateParts.Count -gt $rootParts.Count
    if ($inside) {
        for ($index = 0; $index -lt $rootParts.Count; ++$index) {
            if (-not $candidateParts[$index].Equals($rootParts[$index],
                    [StringComparison]::OrdinalIgnoreCase)) {
                $inside = $false
                break
            }
        }
    }
    Assert-Condition $inside "$Context path must remain below output root '$rootFull': $candidate"
    $current = $rootFull
    foreach ($segment in @($candidateParts[$rootParts.Count..($candidateParts.Count - 1)])) {
        $current = Join-Path $current $segment
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$segment' is a reparse point: $current"
    }
    Assert-Condition (-not [String]::Equals($candidate, $rootFull,
        [StringComparison]::OrdinalIgnoreCase)) "$Context path must name a file below output root."
    return ($candidateParts[$rootParts.Count..($candidateParts.Count - 1)] -join '\')
}

function Get-NativePerformanceReceiptReference {
    param(
        [string]$OutputText,
        [string]$OutputRoot,
        [string]$WorkingDirectory,
        [string]$Role,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$ExecutableSha256,
        [string]$RunNonce,
        [string]$CohortNonce,
        [Collections.IDictionary]$RuntimeClosure,
        [string]$ExpectedTitle,
        [int]$ProcessId,
        [string]$ProcessCreationUtc,
        [string]$ExpectedExecutablePath,
        [string[]]$ExpectedProducers = @('game-executable-stage5-performance-report-v5'),
        [string]$ExpectedCohortCreatedUtc = ''
    )
    if ([string]::IsNullOrWhiteSpace($OutputText) -or
        [string]::IsNullOrWhiteSpace($OutputRoot)) { return $null }
    $matches = [regex]::Matches($OutputText,
        '(?m)^SIMULATION_PERFORMANCE_RECEIPT\s+status=written\s+path=(?<path>[^\r\n]+)$')
    foreach ($match in $matches) {
        $candidateText = $match.Groups['path'].Value.Trim().Trim('"')
        if ([string]::IsNullOrWhiteSpace($candidateText)) { continue }
        try {
            $candidate = if ([IO.Path]::IsPathRooted($candidateText)) {
                [IO.Path]::GetFullPath($candidateText)
            }
            else {
                [IO.Path]::GetFullPath((Join-Path $WorkingDirectory $candidateText))
            }
            Assert-ContainedPathNoReparse $OutputRoot $candidate `
                'Native receipt reference' | Out-Null
            $relative = ConvertTo-OutputRelativePath $candidate $OutputRoot `
                'Native receipt reference'
            $nativeSnapshot = Get-Stage5FileSnapshot $candidate `
                'Native receipt reference'
            $nativeText = [Text.Encoding]::UTF8.GetString([byte[]]$nativeSnapshot.bytes)
            $native = if ($PSVersionTable.PSVersion.Major -ge 6) {
                if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) {
                    $nativeText | ConvertFrom-Json -AsHashtable -DateKind String
                } else { $nativeText | ConvertFrom-Json -AsHashtable }
            }
            else {
                Add-Type -AssemblyName System.Web.Extensions
                $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
                $serializer.DeserializeObject($nativeText)
            }
            if ($native -isnot [Collections.IDictionary]) { continue }
            $expectedProducer = [string]$native['producer']
            if ($ExpectedProducers -notcontains $expectedProducer) { continue }
            $nativeRuntimeClosure = $native['runtimeClosure']
            $nativeProvenance = $native['provenance']
            $nativeRawLogs = $native['rawLogs']
            if ($native['schemaVersion'] -ne 5 -or
                [string]$native['role'] -cne 'performance-report' -or
                [string]$native['title'] -cne $ExpectedTitle -or
                [string]$native['architecture'] -cne 'x64' -or
                [string]$native['cohortCreatedUtc'] -notmatch
                    '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$' -or
                [string]$native['recordedUtc'] -notmatch
                    '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$' -or
                [string]$native['recordedUtc'] -lt [string]$native['cohortCreatedUtc'] -or
                [string]$native['producerVersion'] -cne '5' -or
                $null -eq $nativeRuntimeClosure -or
                [string]$nativeRuntimeClosure['dependencyManifestSha256'].ToUpperInvariant() -cne
                    [string]$RuntimeClosure['dependencyManifestSha256'].ToUpperInvariant() -or
                [string]$nativeRuntimeClosure['closureSha256'].ToUpperInvariant() -cne
                    [string]$RuntimeClosure['closureSha256'].ToUpperInvariant() -or
                $nativeRawLogs -isnot [Array] -or $nativeRawLogs.Count -ne 2 -or
                $nativeProvenance -isnot [Collections.IDictionary]) { continue }
            Assert-Stage5NativePerformanceReceiptProvenance $native 'Native receipt reference' `
                $ExpectedTitle $ExpectedExecutablePath $ExecutableSha256 $ProcessId `
                $ProcessCreationUtc $ExpectedCohortCreatedUtc $candidate $OutputRoot
            $rawLogsValid = $true
            foreach ($rawLog in $nativeRawLogs) {
                if ($rawLog -isnot [Collections.IDictionary] -or
                    [string]$rawLog['sha256'] -notmatch '^[0-9A-Fa-f]{64}$' -or
                    [string]::IsNullOrWhiteSpace([string]$rawLog['path'])) {
                    $rawLogsValid = $false; break
                }
                try {
                    $rawCandidateText = [string]$rawLog['path']
                    $rawCandidate = if ([IO.Path]::IsPathRooted($rawCandidateText)) {
                        [IO.Path]::GetFullPath($rawCandidateText)
                    } else {
                        [IO.Path]::GetFullPath((Join-Path $WorkingDirectory $rawCandidateText))
                    }
                    Assert-ContainedPathNoReparse $OutputRoot $rawCandidate `
                        'Native receipt raw evidence' | Out-Null
                    $rawSnapshot = Get-Stage5FileSnapshot $rawCandidate `
                        'Native receipt raw evidence'
                    if ($rawSnapshot.sha256 -cne ([string]$rawLog['sha256']).ToUpperInvariant()) {
                        $rawLogsValid = $false; break
                    }
                } catch { $rawLogsValid = $false; break }
            }
            if (-not $rawLogsValid) { continue }
            if ([string]$nativeProvenance['kind'] -cne 'native-executable-observation' -or
                [int]$nativeProvenance['processId'] -ne $ProcessId -or
                [string]$nativeProvenance['processCreationUtc'] -cne $ProcessCreationUtc -or
                [IO.Path]::GetFullPath([string]$nativeProvenance['executablePath']) -cne
                    [IO.Path]::GetFullPath($ExpectedExecutablePath) -or
                [string]$nativeProvenance['executableSha256'].ToUpperInvariant() -cne
                    $ExecutableSha256.ToUpperInvariant() -or
                [int]$nativeProvenance['exitCode'] -ne 0 -or
                [string]::IsNullOrWhiteSpace([string]$nativeProvenance['commandLine'])) { continue }
            if ($native['schemaVersion'] -ne 5 -or
                [string]$native['evidenceKind'] -cne 'stage5-executable-originated-receipt' -or
                [string]$native['status'] -cne 'passed' -or
                [string]$native['producer'] -notin $ExpectedProducers -or
                [string]$native['runNonce'] -cne $RunNonce -or
                [string]$native['sourceCommit'] -cne $SourceCommit -or
                [string]$native['artifactSetSha256'].ToUpperInvariant() -cne
                    $ArtifactSetSha256.ToUpperInvariant() -or
                [string]$native['executableSha256'].ToUpperInvariant() -cne
                    $ExecutableSha256.ToUpperInvariant() -or
                [string]$native['cohortNonce'] -cne $CohortNonce) { continue }
            return [pscustomobject]@{
                path = $relative
                sha256 = $nativeSnapshot.sha256
                producer = $expectedProducer
                runNonce = $RunNonce
                cohortNonce = $CohortNonce
            }
        }
        catch {
            # A legacy or malformed marker is diagnostic only.  It must never
            # be promoted to a current native provenance reference.
        }
    }
    return $null
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-RequiredProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-Condition ($null -ne $Object -and $Object -is [Collections.IDictionary]) `
        "$Context must be a JSON object."
    $keys = @($Object.Keys | Where-Object { [string]$_ -ceq $Name })
    Assert-Condition ($keys.Count -eq 1) "$Context is missing required property '$Name'."
    $value = $Object[$keys[0]]
    if ($value -is [Array]) { return ,$value }
    return $value
}

function Assert-JsonObjectShape {
    param([object]$Object, [string[]]$Required, [string[]]$Allowed, [string]$Context)
    Assert-Condition ($null -ne $Object -and $Object -is [Collections.IDictionary]) `
        "$Context must be a JSON object."
    foreach ($requiredName in $Required) {
        Get-RequiredProperty $Object $requiredName $Context | Out-Null
    }
    foreach ($propertyName in $Object.Keys) {
        Assert-Condition ($Allowed -ccontains [string]$propertyName) `
            "$Context contains unsupported property '$propertyName'."
    }
}

function Assert-JsonString {
    param([object]$Value, [string]$Context)
    Assert-Condition ($Value -is [string]) "$Context must be a JSON string."
}

function Assert-JsonBoolean {
    param([object]$Value, [string]$Context)
    Assert-Condition ($Value -is [bool]) "$Context must be a JSON boolean."
}

function Assert-JsonArray {
    param([object]$Value, [string]$Context)
    Assert-Condition ($null -ne $Value -and $Value -is [Array]) "$Context must be a JSON array."
}

function Assert-JsonInteger {
    param([object]$Value, [string]$Context)
    $isInteger = $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
    Assert-Condition $isInteger "$Context must be a JSON integer."
}

function Test-Sha256Text {
    param([string]$Value)
    return $null -ne $Value -and $Value -match '^[0-9A-Fa-f]{64}$'
}

function Assert-CanonicalUuid {
    param([string]$Value, [string]$Context)
    Assert-Condition ($Value -is [string] -and
        $Value -match '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
        "$Context must be a canonical UUID."
    return $Value
}

function Get-HostRunnerRuntimeClosure {
    param([string]$Context)
    Assert-Condition ((Test-Sha256Text $AcceptanceRuntimeDependencyManifestSha256) -and
        (Test-Sha256Text $AcceptanceRuntimeClosureSha256)) `
        "$Context requires independently supplied dependency-manifest and closure SHA-256 values."
    return [ordered]@{
        dependencyManifestSha256 = $AcceptanceRuntimeDependencyManifestSha256.ToUpperInvariant()
        closureSha256 = $AcceptanceRuntimeClosureSha256.ToUpperInvariant()
    }
}

function Get-NativeObservationBinding {
    param([AllowNull()][object]$Binding)
    if ($null -eq $Binding) { return $null }
    # This is a transport boundary, not artifact verification or acceptance.
    # Callers must independently verify the installed source/artifact closure.
    $context = 'Native observation binding'
    $fields = @('sourceCommit','artifactSetSha256','runtimeClosure')
    Assert-JsonObjectShape $Binding $fields $fields $context
    Assert-JsonString $Binding.sourceCommit "$context sourceCommit"
    Assert-Condition ($Binding.sourceCommit -cmatch '^[0-9a-f]{40}$') `
        "$context sourceCommit must be an independently supplied lowercase 40-hex commit."
    Assert-JsonString $Binding.artifactSetSha256 "$context artifactSetSha256"
    Assert-Condition (Test-Sha256Text $Binding.artifactSetSha256) `
        "$context artifactSetSha256 must contain exactly 64 hexadecimal characters."
    $closure = $Binding.runtimeClosure
    $closureFields = @('dependencyManifestSha256','closureSha256')
    Assert-JsonObjectShape $closure $closureFields $closureFields "$context runtimeClosure"
    foreach ($field in $closureFields) {
        Assert-JsonString $closure[$field] "$context runtimeClosure.$field"
        Assert-Condition (Test-Sha256Text $closure[$field]) `
            "$context runtimeClosure.$field must contain exactly 64 hexadecimal characters."
    }
    return [ordered]@{
        sourceCommit = $Binding.sourceCommit
        artifactSetSha256 = $Binding.artifactSetSha256.ToUpperInvariant()
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $closure.dependencyManifestSha256.ToUpperInvariant()
            closureSha256 = $closure.closureSha256.ToUpperInvariant()
        }
    }
}

function Assert-ContainedPathNoReparse {
    param(
        [string]$BaseDirectory,
        [string]$CandidatePath,
        [string]$Context,
        [switch]$AllowBase
    )
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Condition ($baseRoot -is [string] -and $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."
    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $minimumParts = if ($AllowBase) { $baseParts.Count } else { $baseParts.Count + 1 }
    Assert-Condition ($candidateParts.Count -ge $minimumParts) `
        "$Context path escapes or is not below its containing directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Condition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its containing directory."
    }
    $rootCurrent = $baseRoot
    foreach ($segment in @($candidate.Substring($candidateRoot.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $rootCurrent = Join-Path $rootCurrent $segment
        if (-not (Test-Path -LiteralPath $rootCurrent)) { break }
        $rootItem = Get-Item -LiteralPath $rootCurrent -Force -ErrorAction Stop
        Assert-Condition (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context ancestor path component '$segment' is a reparse point."
    }
    $baseItem = Get-Item -LiteralPath $base -Force -ErrorAction Stop
    Assert-Condition (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context containing directory is a reparse point."
    $current = $base
    for ($index = $baseParts.Count; $index -lt $candidateParts.Count; ++$index) {
        $current = Join-Path $current $candidateParts[$index]
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$($candidateParts[$index])' is a reparse point."
    }
    if (-not $AllowBase) {
        Assert-Condition (-not [String]::Equals($candidate, $base,
            [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path must not be the containing directory itself."
    }
    return $candidate
}

function Resolve-ManifestFile {
    param([string]$ManifestDirectory, [string]$RelativePath, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) "$Context path is empty."
    Assert-Condition (-not [IO.Path]::IsPathRooted($RelativePath)) "$Context path must be manifest-relative."
    $manifestFull = [IO.Path]::GetFullPath($ManifestDirectory)
    $candidate = [IO.Path]::GetFullPath((Join-Path $manifestFull $RelativePath))
    try {
        Assert-ContainedPathNoReparse $manifestFull $candidate $Context | Out-Null
    }
    catch {
        throw "$Context path escapes the manifest directory: $($_.Exception.Message)"
    }
    Assert-Condition (Test-Path -LiteralPath $candidate -PathType Leaf) "$Context file was not found: $RelativePath"
    return $candidate
}

function Assert-FileHash {
    param([string]$Path, [string]$Expected, [string]$Context)
    Assert-Condition (Test-Sha256Text $Expected) "$Context SHA-256 must contain exactly 64 hexadecimal characters."
    $actual = Get-Sha256 $Path
    Assert-Condition ($actual -ceq $Expected.ToUpperInvariant()) "$Context SHA-256 mismatch. Expected $Expected, got $actual."
    return $actual
}

function Assert-FreeSpace {
    param([string]$Path, [UInt64]$RequiredBytes, [string]$Context)
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($root)) "$Context has no resolvable volume."
    $drive = New-Object IO.DriveInfo($root)
    Assert-Condition ([UInt64]$drive.AvailableFreeSpace -ge $RequiredBytes) `
        "$Context requires at least $RequiredBytes free bytes; $($drive.AvailableFreeSpace) are available on $root."
}

function Resolve-ExplicitTaskRoot {
    param([string]$Path, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context is required for installed execution."
    $full = [IO.Path]::GetFullPath($Path)
    $volumeRoot = [IO.Path]::GetPathRoot($full)
    Assert-Condition ([String]::Equals($volumeRoot, 'H:\', [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must be an explicit task-owned H: path: $full"
    Assert-Condition (-not [String]::Equals($full, $volumeRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must name a task directory below H:\, not the volume root."
    return $full.TrimEnd('\')
}

function Assert-TaskOwnedPath {
    param([string]$Path, [string]$TaskRoot, [string]$Context, [bool]$AllowRoot = $false)
    return Assert-ContainedPathNoReparse $TaskRoot $Path $Context `
        -AllowBase:$AllowRoot
}

function Resolve-TaskOwnedChildPath {
    param([string]$Path, [string]$TaskRoot, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context is required."
    $candidate = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    }
    else {
        [IO.Path]::GetFullPath((Join-Path $TaskRoot $Path))
    }
    Assert-TaskOwnedPath $candidate $TaskRoot $Context | Out-Null
    return $candidate.TrimEnd('\')
}

function Test-PathWithin {
    param([string]$BaseDirectory, [string]$CandidatePath)
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($CandidatePath).TrimEnd('\')
    return [String]::Equals($candidate, $base, [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Remove-TaskOwnedDirectory {
    param([string]$Path, [string]$TaskRoot, [string]$Context)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return
    }
    $full = Assert-TaskOwnedPath $Path $TaskRoot $Context
    $item = Get-Item -LiteralPath $full -Force
    Assert-Condition ($item.PSIsContainer) "$Context is not a directory: $full"
    Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context is a reparse point; refusing recursive cleanup: $full"
    Remove-Item -LiteralPath $full -Recurse -Force
    Assert-Condition (-not (Test-Path -LiteralPath $full)) "$Context was not removed: $full"
}

function Get-LauncherRunContract {
    param([string]$LauncherConfigPath, [string]$RuntimeDirectory, [string]$Executable)
    $runLines = @(Get-Content -LiteralPath $LauncherConfigPath |
        Where-Object { $_ -match '^\s*RUN\s*=' })
    Assert-Condition ($runLines.Count -eq 1) `
        'launcher.lcf must contain exactly one RUN entry for validation.'
    $match = [regex]::Match($runLines[0],
        '^\s*RUN\s*=\s*(?<directory>\S+)\s+(?<executable>"[^"]+"|\S+)(?<arguments>.*)$')
    Assert-Condition $match.Success 'launcher.lcf RUN entry has an unsupported shape.'
    $directory = $match.Groups['directory'].Value
    Assert-Condition ($directory -ceq '.') `
        "launcher.lcf RUN working directory must be '.', got '$directory'."
    $configuredExecutable = $match.Groups['executable'].Value.Trim('"')
    Assert-Condition ($configuredExecutable -match '^[A-Za-z0-9._-]+\.exe$') `
        'launcher.lcf RUN target must be a leaf executable name.'
    $expectedExecutable = [IO.Path]::GetFileName($Executable)
    Assert-Condition ($configuredExecutable -ceq $expectedExecutable) `
        "launcher.lcf target '$configuredExecutable' does not match '$expectedExecutable'."

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
        Assert-Condition ($consumed -eq $argumentText.Length) `
            'launcher.lcf RUN arguments contain an unsupported trailing token.'
    }
    return [pscustomobject]@{
        configPath = [IO.Path]::GetFullPath($LauncherConfigPath)
        directory = $directory
        executable = $configuredExecutable
        arguments = $arguments.ToArray()
        launcherPath = [IO.Path]::GetFullPath((Join-Path $RuntimeDirectory 'launcher.exe'))
    }
}

function Assert-LauncherEquivalenceContract {
    param(
        [object]$LauncherContract,
        [string]$Executable,
        [string]$WorkingDirectory,
        [object[]]$Plan,
        [string]$ProfileLeafName = '',
        [string]$DocumentsRoot = ''
    )
    $executableFull = [IO.Path]::GetFullPath($Executable)
    $workingDirectoryFull = [IO.Path]::GetFullPath($WorkingDirectory)
    $launcherTarget = [IO.Path]::GetFullPath((Join-Path $workingDirectoryFull $LauncherContract.executable))
    Assert-Condition ([String]::Equals($launcherTarget, $executableFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Direct validation target must be the executable named by launcher.lcf.'
    $launcherArguments = @($LauncherContract.arguments)
    if ($launcherArguments.Count -gt 0) {
        Assert-Condition ($launcherArguments.Count -eq 4 -and
            $launcherArguments[0] -ceq '-simulationMode' -and
            $launcherArguments[1] -ceq 'parallel' -and
            $launcherArguments[2] -ceq '-workerPolicy' -and
            $launcherArguments[3] -ceq 'auto') `
            'launcher.lcf may only contribute the reviewed native Stage 5 defaults.'
    }
    foreach ($entry in @($Plan)) {
        $arguments = @($entry.arguments)
        $headlessIndex = [Array]::IndexOf([object[]]$arguments, '-headless')
        Assert-Condition ($headlessIndex -ge 0) `
            "Validation entry $($entry.sequence) must remain headless for the direct exception."
        $pipelineIndex = [Array]::IndexOf([object[]]$arguments, '-pipelineMode')
        $simulationIndex = [Array]::IndexOf([object[]]$arguments, '-simulationMode')
        $workerPolicyIndex = [Array]::IndexOf([object[]]$arguments, '-workerPolicy')
        Assert-Condition ($pipelineIndex -ge 0 -and $arguments[$pipelineIndex + 1] -ceq 'serial' -and
            $simulationIndex -ge 0 -and $arguments[$simulationIndex + 1] -ceq [string]$entry.simulationMode -and
            $workerPolicyIndex -ge 0 -and $arguments[$workerPolicyIndex + 1] -ceq 'auto') `
            "Validation entry $($entry.sequence) does not preserve the launcher-equivalent policy contract."
        if ($launcherArguments.Count -gt 0) {
            # A serial/shadow matrix entry deliberately overrides the launcher's
            # parallel default. The worker policy remains the same final value;
            # requiring a duplicate parallel token here would reject a valid
            # explicit last-option-wins override.
            $preservesLauncherSimulation = $entry.simulationMode -ceq 'parallel' `
                -and $arguments -contains 'parallel'
            Assert-Condition ($preservesLauncherSimulation -or $entry.simulationMode -cne 'parallel') `
                "Validation entry $($entry.sequence) dropped the launcher's parallel default without an explicit simulation override."
            Assert-Condition ($arguments -contains '-simulationMode' -and
                $arguments -contains '-workerPolicy' -and $arguments -contains 'auto') `
                "Validation entry $($entry.sequence) dropped a launcher.lcf default."
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($DocumentsRoot)) {
        Assert-Condition ([String]::Equals(
            [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($DocumentsRoot)),
            'H:\', [StringComparison]::OrdinalIgnoreCase)) `
            'Validation Documents redirection must remain on H:.'
    }
    return [pscustomobject]@{
        schemaVersion = 1
        mode = 'headless-direct-exception'
        reason = 'launcher-main-does-not-propagate-child-exit-code'
        launcherExecutable = $LauncherContract.launcherPath
        launcherConfig = $LauncherContract.configPath
        launcherTarget = $launcherTarget
        launcherArguments = $launcherArguments
        launcherWorkingDirectory = $workingDirectoryFull
        directExecutable = $executableFull
        directArguments = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
            '-simulationMode', '<matrix>', '-workerPolicy', 'auto',
            '-validationExecutableSha256', '<manifest>')
        directWorkingDirectory = $workingDirectoryFull
        environmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
            'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH', 'RTS_FRAME_TIMING_DIR')
        profileStrategy = 'known-folder-registry-redirect'
        profileLeafName = $ProfileLeafName
        documentsRoot = if ([string]::IsNullOrWhiteSpace($DocumentsRoot)) { $null } else { [IO.Path]::GetFullPath($DocumentsRoot) }
        profileRegistryValues = @(
            'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders\Personal',
            'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders\Personal')
        childExitCodeObserved = $true
    }
}

function Get-ProcessorTopology {
    try {
        $processors = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop)
        if ($processors.Count -eq 0) {
            return [pscustomobject]@{
                source = 'Win32_Processor'
                physicalCoreCount = 0
                logicalProcessorCount = 0
            }
        }
        return [pscustomobject]@{
            source = 'Win32_Processor'
            physicalCoreCount = [int](($processors |
                Measure-Object -Property NumberOfCores -Sum).Sum)
            logicalProcessorCount = [int](($processors |
                Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum)
        }
    }
    catch {
        return [pscustomobject]@{
            source = 'Win32_Processor'
            physicalCoreCount = 0
            logicalProcessorCount = 0
        }
    }
}

function Get-PhysicalCoreCount {
    return [int](Get-ProcessorTopology).physicalCoreCount
}

function Get-LocalCapacityTopology {
    $topology = Get-ProcessorTopology
    Assert-Condition ($topology.physicalCoreCount -ge 4 -and
        $topology.physicalCoreCount -le 6) `
        "LocalCapacity requires between 4 and 6 physical cores; host exposes $($topology.physicalCoreCount)."
    Assert-Condition ($topology.logicalProcessorCount -ge $topology.physicalCoreCount -and
        $topology.logicalProcessorCount -le 12) `
        "LocalCapacity requires at most 12 logical processors; host exposes $($topology.logicalProcessorCount)."
    return [pscustomobject]@{
        source = $topology.source
        physicalCoreCount = [int]$topology.physicalCoreCount
        logicalProcessorCount = [int]$topology.logicalProcessorCount
        minimumPhysicalCoreCount = 4
        maximumPhysicalCoreCount = 6
        maximumLogicalProcessorCount = 12
        autoWorkerCap = 12
    }
}

function Assert-X64PeExecutable {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            Assert-Condition ($reader.ReadUInt16() -eq 0x5A4D) `
                'x64 validation requires a valid PE executable.'
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            Assert-Condition ($peOffset -gt 0 -and $peOffset -lt $stream.Length - 6) `
                'x64 validation requires a valid PE header.'
            $stream.Position = $peOffset
            Assert-Condition ($reader.ReadUInt32() -eq 0x00004550) `
                'x64 validation requires a valid PE signature.'
            Assert-Condition ($reader.ReadUInt16() -eq 0x8664) `
                'x64 validation requires the exact native x64 candidate.'
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-WorkerConfigurations {
    param([ValidateSet('Canonical', 'LocalCapacity')][string]$CapacityMode = 'Canonical')
    if ($CapacityMode -ceq 'LocalCapacity') {
        return @(
            [pscustomobject]@{ Id = 'serial-1'; Mode = 'serial'; WorkerCount = 1; HasWorkerCount = $true },
            [pscustomobject]@{ Id = 'parallel-1'; Mode = 'parallel'; WorkerCount = 1; HasWorkerCount = $true },
            [pscustomobject]@{ Id = 'parallel-2'; Mode = 'parallel'; WorkerCount = 2; HasWorkerCount = $true },
            [pscustomobject]@{ Id = 'parallel-4'; Mode = 'parallel'; WorkerCount = 4; HasWorkerCount = $true },
            [pscustomobject]@{ Id = 'parallel-8'; Mode = 'parallel'; WorkerCount = 8; HasWorkerCount = $true },
            [pscustomobject]@{ Id = 'parallel-auto'; Mode = 'parallel'; WorkerCount = 0; HasWorkerCount = $false }
        )
    }
    return @(
        [pscustomobject]@{ Id = 'serial-1'; Mode = 'serial'; WorkerCount = 1; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-1'; Mode = 'parallel'; WorkerCount = 1; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-2'; Mode = 'parallel'; WorkerCount = 2; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-4'; Mode = 'parallel'; WorkerCount = 4; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-8'; Mode = 'parallel'; WorkerCount = 8; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-16'; Mode = 'parallel'; WorkerCount = 16; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-auto'; Mode = 'parallel'; WorkerCount = 0; HasWorkerCount = $false }
    )
}

function Get-CollisionShadowConfiguration {
    param([ValidateSet('Canonical', 'LocalCapacity')][string]$CapacityMode = 'Canonical')
    if ($CapacityMode -ceq 'LocalCapacity') {
        return [pscustomobject]@{
            Id = 'shadow-8'; Mode = 'shadow'; WorkerCount = 8; HasWorkerCount = $true
        }
    }
    return [pscustomobject]@{
        Id = 'shadow-16'; Mode = 'shadow'; WorkerCount = 16; HasWorkerCount = $true
    }
}

function New-CommonArguments {
    param([object]$Configuration, [string]$ExecutableHash)
    $arguments = @(
        '-headless', '-noFPSLimit',
        '-pipelineMode', 'serial',
        '-simulationMode', [string]$Configuration.Mode,
        '-workerPolicy', 'auto',
        '-validationExecutableSha256', $ExecutableHash
    )
    if ($Configuration.HasWorkerCount) {
        $arguments += @('-workerCount', [string]$Configuration.WorkerCount)
    }
    return $arguments
}

function ConvertTo-DisplayCommand {
    param([string]$Executable, [string[]]$Arguments)
    $display = @('"' + $Executable.Replace('"', '""') + '"')
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') {
            $display += '"' + $argument.Replace('"', '""') + '"'
        }
        else {
            $display += $argument
        }
    }
    return $display -join ' '
}

function Add-PlanEntry {
    param(
        [Collections.Generic.List[object]]$Plan,
        [string]$Kind,
        [string]$CaseId,
        [object]$Configuration,
        [int]$Repeat,
        [int]$TimeoutSeconds,
        [string[]]$Arguments,
        [string]$Executable,
        [string]$OutputDirectory,
        [string]$DeterminismKey,
        [string]$ReplayArgument,
        [string]$FixtureSha256,
        [bool]$Stress,
        [int]$MatrixRepeat,
        [int]$Seed,
        [string]$Scenario
    )
    $sequence = $Plan.Count + 1
    $safeId = "$($Configuration.Id)-$CaseId-r$Repeat"
    $prefix = '{0:D4}-{1}' -f $sequence, $safeId
    $Plan.Add([pscustomobject]@{
        sequence = $sequence
        kind = $Kind
        caseId = $CaseId
        determinismKey = $DeterminismKey
        configuration = $Configuration.Id
        simulationMode = $Configuration.Mode
        requestedWorkers = $(if ($Configuration.HasWorkerCount) { [string]$Configuration.WorkerCount } else { 'auto' })
        workerPolicy = 'auto'
        repeat = $Repeat
        matrixRepeat = $MatrixRepeat
        replayArgument = $ReplayArgument
        fixtureSha256 = $FixtureSha256
        stress = $Stress
        seed = $Seed
        scenario = $Scenario
        timeoutSeconds = $TimeoutSeconds
        arguments = $Arguments
        command = ConvertTo-DisplayCommand $Executable $Arguments
        stdout = Join-Path $OutputDirectory "runs\$prefix.stdout.log"
        stderr = Join-Path $OutputDirectory "runs\$prefix.stderr.log"
        timingDirectory = Join-Path $OutputDirectory "timing\$prefix"
        runtimeLogDirectory = Join-Path $OutputDirectory "runtime-logs\$prefix"
    }) | Out-Null
}

function Get-ManifestData {
    param([string]$Path, [string]$Set, [bool]$AllowNonStandard,
        [string]$ExecutableHashOverride, [string]$ExpectedTitle = 'ZeroHour',
        [bool]$AllowEmptyReplayCorpus = $false)
    $manifestFile = [IO.Path]::GetFullPath($Path)
    Assert-Condition (Test-Path -LiteralPath $manifestFile -PathType Leaf) "Fixture manifest was not found: $manifestFile"
    $manifestDirectory = Split-Path -Parent $manifestFile
    $manifestJson = Get-Content -LiteralPath $manifestFile -Raw
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $manifest = $manifestJson | ConvertFrom-Json -AsHashtable
    }
    else {
        Add-Type -AssemblyName System.Web.Extensions
        $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $serializer.MaxJsonLength = 10485760
        $manifest = $serializer.DeserializeObject($manifestJson)
    }
    Assert-JsonObjectShape $manifest @('schemaVersion', 'title', 'executable',
        'executableSha256', 'fixtures', 'ai') @('schemaVersion', 'title', 'executable',
        'executableSha256', 'fixtures', 'ai') 'Fixture manifest'
    $schemaVersion = Get-RequiredProperty $manifest 'schemaVersion' 'Fixture manifest'
    Assert-JsonInteger $schemaVersion 'Fixture manifest schemaVersion'
    Assert-Condition ($schemaVersion -eq 1) `
        'Fixture manifest schemaVersion must be 1.'
    $title = Get-RequiredProperty $manifest 'title' 'Fixture manifest'
    Assert-JsonString $title 'Fixture manifest title'
    Assert-Condition ($title -ceq $ExpectedTitle) `
        "Fixture manifest title '$title' does not match the requested installed-runtime title '$ExpectedTitle'."
    $executableName = Get-RequiredProperty $manifest 'executable' 'Fixture manifest'
    Assert-JsonString $executableName 'Fixture manifest executable'
    Assert-Condition ($executableName -match '^[A-Za-z0-9._-]+\.exe$') `
        'Fixture manifest executable must be a leaf .exe name.'
    $expectedExecutablePrefix = if ($ExpectedTitle -ceq 'Generals') { 'generalsv' } else { 'generalszh' }
    Assert-Condition ($executableName -match ('^' + $expectedExecutablePrefix +
        '(?:-[A-Za-z0-9._-]+)?\.exe$')) `
        "Fixture manifest executable '$executableName' does not belong to the requested title '$ExpectedTitle'."
    $executableHash = Get-RequiredProperty $manifest 'executableSha256' 'Fixture manifest'
    Assert-JsonString $executableHash 'Fixture manifest executableSha256'
    Assert-Condition (Test-Sha256Text $executableHash) 'Fixture manifest executableSha256 is invalid.'
    $executableHashSource = 'manifest'
    if (-not [string]::IsNullOrWhiteSpace($ExecutableHashOverride)) {
        Assert-Condition (Test-Sha256Text $ExecutableHashOverride) `
            'ExpectedExecutableSha256 must contain exactly 64 hexadecimal characters.'
        $executableHash = $ExecutableHashOverride
        $executableHashSource = 'argument'
    }

    $fixtures = New-Object 'Collections.Generic.List[object]'
    $fixtureIds = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $replayArguments = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $fixtureItemsValue = Get-RequiredProperty $manifest 'fixtures' 'Fixture manifest'
    Assert-JsonArray $fixtureItemsValue 'Fixture manifest fixtures'
    $fixtureItems = @($fixtureItemsValue)
    $minimumFixtureCount = if ($AllowEmptyReplayCorpus -and $Set -ceq 'AI') { 0 } else { 1 }
    Assert-Condition ($fixtureItems.Count -ge $minimumFixtureCount -and
        $fixtureItems.Count -le 100) `
        "Fixture manifest fixtures must contain between $minimumFixtureCount and 100 entries."
    foreach ($fixture in $fixtureItems) {
            Assert-JsonObjectShape $fixture @('id', 'source', 'sha256', 'stress') `
                @('id', 'source', 'sha256', 'stress', 'maps') 'Replay fixture'
            $id = Get-RequiredProperty $fixture 'id' 'Replay fixture'
            Assert-JsonString $id 'Replay fixture id'
            Assert-Condition ($id -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') "Replay fixture id '$id' is invalid."
            Assert-Condition ($fixtureIds.Add($id)) `
                "Replay fixture id '$id' collides case-insensitively with another fixture id."
            $replayArgument = "Stage5Validation\$id.rep"
            Assert-Condition ($replayArguments.Add($replayArgument)) `
                "Replay argument '$replayArgument' collides case-insensitively with another staged replay path."
            $sourceRelative = Get-RequiredProperty $fixture 'source' "Replay fixture '$id'"
            Assert-JsonString $sourceRelative "Replay fixture '$id' source"
            $sourceFull = Resolve-ManifestFile $manifestDirectory $sourceRelative "Replay fixture '$id'"
            $sha256 = Get-RequiredProperty $fixture 'sha256' "Replay fixture '$id'"
            Assert-JsonString $sha256 "Replay fixture '$id' sha256"
            Assert-FileHash $sourceFull $sha256 "Replay fixture '$id'" | Out-Null
            $stress = Get-RequiredProperty $fixture 'stress' "Replay fixture '$id'"
            Assert-JsonBoolean $stress "Replay fixture '$id' stress"
            $maps = New-Object 'Collections.Generic.List[object]'
            $mapsKeys = @($fixture.Keys | Where-Object { [string]$_ -ceq 'maps' })
            if ($mapsKeys.Count -eq 1) {
                $mapValues = $fixture[$mapsKeys[0]]
                Assert-JsonArray $mapValues "Replay fixture '$id' maps"
                foreach ($map in @($mapValues)) {
                    Assert-JsonObjectShape $map @('source', 'profileRelativePath', 'sha256') `
                        @('source', 'profileRelativePath', 'sha256') "Replay fixture '$id' map"
                    $mapSourceRelative = Get-RequiredProperty $map 'source' "Replay fixture '$id' map"
                    Assert-JsonString $mapSourceRelative "Replay fixture '$id' map source"
                    $mapSourceFull = Resolve-ManifestFile $manifestDirectory $mapSourceRelative "Replay fixture '$id' map"
                    $mapRelative = Get-RequiredProperty $map 'profileRelativePath' "Replay fixture '$id' map"
                    Assert-JsonString $mapRelative "Replay fixture '$id' map profileRelativePath"
                    Assert-Condition (-not [IO.Path]::IsPathRooted($mapRelative)) `
                        "Replay fixture '$id' map profileRelativePath must be relative."
                    $normalizedMap = $mapRelative.Replace('/', '\')
                    Assert-Condition ($normalizedMap -match '^Maps\\' -and $normalizedMap -notmatch '(^|\\)\.\.(\\|$)') `
                        "Replay fixture '$id' map profileRelativePath must stay below Maps."
                    $mapHash = Get-RequiredProperty $map 'sha256' "Replay fixture '$id' map"
                    Assert-JsonString $mapHash "Replay fixture '$id' map sha256"
                    Assert-FileHash $mapSourceFull $mapHash "Replay fixture '$id' map" | Out-Null
                    $maps.Add([pscustomobject]@{ source = $mapSourceFull; relative = $normalizedMap; sha256 = $mapHash.ToUpperInvariant() }) | Out-Null
                }
            }
            $fixtures.Add([pscustomobject]@{
                id = $id
                source = $sourceFull
                sha256 = $sha256.ToUpperInvariant()
                stress = $stress
                replayArgument = $replayArgument
                maps = $maps.ToArray()
            }) | Out-Null
    }
    if ($Set -ne 'AI') {
        if (-not $AllowNonStandard) {
            Assert-Condition ($fixtures.Count -eq 10) 'The standard replay corpus must contain exactly 10 fixtures.'
            Assert-Condition (@($fixtures | Where-Object { $_.stress }).Count -eq 1) `
                'The standard replay corpus must mark exactly one stress fixture.'
        }
        else {
            Assert-Condition ($fixtures.Count -gt 0) 'Replay validation requires at least one fixture.'
        }
    }
    $uniqueSourceHashes = @($fixtures | ForEach-Object { $_.sha256 } | Select-Object -Unique)
    Assert-Condition ($uniqueSourceHashes.Count -eq $fixtures.Count) `
        'Replay validation requires a unique source SHA-256 for every fixture.'

    $aiObject = Get-RequiredProperty $manifest 'ai' 'Fixture manifest'
    Assert-JsonObjectShape $aiObject @('seeds', 'scenarios', 'repeats') `
        @('seeds', 'scenarios', 'repeats') 'AI manifest'
    $seedValues = Get-RequiredProperty $aiObject 'seeds' 'AI manifest'
    Assert-JsonArray $seedValues 'AI manifest seeds'
    $seeds = @($seedValues)
        Assert-Condition ($seeds.Count -gt 0) 'AI manifest seeds must not be empty.'
        Assert-Condition ($seeds.Count -le 32) 'AI manifest accepts at most 32 seeds.'
        foreach ($seed in $seeds) {
            Assert-JsonInteger $seed 'AI manifest seed'
            Assert-Condition ($seed -gt 0) 'AI manifest seeds must be positive integers.'
        }
        Assert-Condition (@($seeds | Sort-Object -Unique).Count -eq $seeds.Count) `
            'AI manifest seeds must be distinct.'
    $scenarioValues = Get-RequiredProperty $aiObject 'scenarios' 'AI manifest'
    Assert-JsonArray $scenarioValues 'AI manifest scenarios'
    $scenarios = @($scenarioValues)
        Assert-Condition ($scenarios.Count -gt 0) 'AI manifest scenarios must not be empty.'
        foreach ($scenario in $scenarios) {
            Assert-JsonString $scenario 'AI manifest scenario'
            Assert-Condition ($scenario -ceq '4v3' -or $scenario -ceq '4v2') `
                "AI manifest scenario '$scenario' is invalid."
        }
        Assert-Condition (@($scenarios | Sort-Object -Unique).Count -eq $scenarios.Count) `
            'AI manifest scenarios must be distinct.'
    $aiRepeats = Get-RequiredProperty $aiObject 'repeats' 'AI manifest'
    Assert-JsonInteger $aiRepeats 'AI manifest repeats'
        Assert-Condition ($aiRepeats -gt 0 -and $aiRepeats -le 10) `
            'AI manifest repeats must be between 1 and 10.'
    if ($Set -ceq 'All') {
        Assert-Condition ($scenarios -ccontains '4v3' -and $scenarios -ccontains '4v2') `
            'The deterministic-runtime gate requires both the 4v3 and 4v2 live-AI scenarios.'
        Assert-Condition ($seeds.Count -ge 3) `
            'The deterministic-runtime gate requires at least three distinct live-AI seeds.'
    }
    $ai = [pscustomobject]@{ seeds = $seeds; scenarios = $scenarios; repeats = [int]$aiRepeats }

    return [pscustomobject]@{
        file = $manifestFile
        directory = $manifestDirectory
        title = $title
        executable = $executableName
        executableSha256 = $executableHash.ToUpperInvariant()
        executableSha256Source = $executableHashSource
        fixtures = $fixtures.ToArray()
        ai = $ai
    }
}

function Set-NativePerformanceFixtureEnvironment {
    param([object]$Environment, [object]$Entry)
    $Environment['RTS_PERFORMANCE_WORKLOAD_QUALIFICATION'] = 'observed-only'
    $Environment['RTS_PERFORMANCE_FIXTURE_KIND'] = if ($Entry.kind -ceq 'replay') { 'replay' } else { 'fresh-ai-map' }
    $Environment['RTS_PERFORMANCE_FIXTURE_ID'] = [string]$Entry.caseId
    foreach ($name in @('RTS_PERFORMANCE_PLAYER_COUNT','RTS_PERFORMANCE_UNIT_COUNT',
        'RTS_PERFORMANCE_SEED','RTS_PERFORMANCE_FIXTURE_SHA256')) {
        $Environment.Remove($name) | Out-Null
    }
    if ($Entry.kind -ceq 'replay') {
        # A plan's seed zero is a placeholder, not a parsed replay seed. The
        # executable observes the actual loaded game seed before publication.
        $Environment['RTS_PERFORMANCE_FIXTURE_SHA256'] = [string]$Entry.fixtureSha256
    } else {
        $Environment['RTS_PERFORMANCE_SEED'] = [string]$Entry.seed
    }
}

function Set-NativePerformanceObservationEnvironment {
    param(
        [object]$Environment,
        [AllowNull()][object]$Binding,
        [object]$Entry,
        [string]$EvidenceRoot,
        [string]$RunNonce,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc
    )
    $nativeBinding = Get-NativeObservationBinding $Binding
    $nativeReceiptDirectory = $null
    if ($null -ne $nativeBinding) {
        Assert-CanonicalUuid $RunNonce 'Native observation run nonce' | Out-Null
        Assert-CanonicalUuid $CohortNonce 'Native observation cohort nonce' | Out-Null
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($EvidenceRoot) -and
            -not [string]::IsNullOrWhiteSpace($CohortCreatedUtc) -and
            -not [string]::IsNullOrWhiteSpace([string]$Entry.timingDirectory)) `
            'Native observation requires output, timing, and cohort creation bindings.'
        $nativeReceiptDirectory = Join-Path ([IO.Path]::GetFullPath($EvidenceRoot)) `
            ('native-performance-receipts\' + $RunNonce)
    }
    # An absent binding is disabled, not permission to reuse inherited metadata.
    # Clear the whole diagnostic namespace before installing this child's values.
    foreach ($name in @($Environment.Keys)) {
        if ([string]$name -like 'RTS_PERFORMANCE_*' -or
            [string]$name -ieq 'RTS_STAGE5_RUNTIME_CLOSURE_SHA256' -or
            [string]$name -ieq 'RTS_STAGE5_RUNTIME_MANIFEST_SHA256') {
            $Environment.Remove([string]$name) | Out-Null
        }
    }
    if ($null -eq $nativeBinding) { return $null }
    $Environment['RTS_STAGE5_RUNTIME_CLOSURE_SHA256'] = $nativeBinding.runtimeClosure.closureSha256
    $Environment['RTS_STAGE5_RUNTIME_MANIFEST_SHA256'] = $nativeBinding.runtimeClosure.dependencyManifestSha256
    $Environment['RTS_PERFORMANCE_ROLE'] = 'performance-report'
    $Environment['RTS_PERFORMANCE_REFERENCE_MODE'] = 'throughput-binding'
    $Environment['RTS_PERFORMANCE_RUN_ID'] = 'stage5-' + [string]$Entry.sequence + '-' + $RunNonce
    $Environment['RTS_PERFORMANCE_RUN_NONCE'] = $RunNonce
    $Environment['RTS_PERFORMANCE_COHORT_NONCE'] = $CohortNonce
    $Environment['RTS_PERFORMANCE_COHORT_CREATED_UTC'] = $CohortCreatedUtc
    $Environment['RTS_PERFORMANCE_RECEIPT_DIR'] = $nativeReceiptDirectory
    $Environment['RTS_PERFORMANCE_SOURCE_COMMIT'] = $nativeBinding.sourceCommit
    $Environment['RTS_PERFORMANCE_ARTIFACT_SET_SHA256'] = $nativeBinding.artifactSetSha256
    $Environment['RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256'] = $nativeBinding.runtimeClosure.dependencyManifestSha256
    $Environment['RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256'] = $nativeBinding.runtimeClosure.closureSha256
    Set-NativePerformanceFixtureEnvironment $Environment $Entry
    $Environment['RTS_PERFORMANCE_RAW_LOG_PATH'] = Join-Path $nativeReceiptDirectory 'performance-raw.log'
    $Environment['RTS_PERFORMANCE_TIMING_PATH'] = $Entry.timingDirectory
    $Environment['RTS_PERFORMANCE_VERIFIER_BOUNDARY'] = 'stage5-host-runner-closed-native-evidence'
    return $nativeReceiptDirectory
}

function New-ValidationPlan {
    param([object]$Data, [string]$Set, [int]$ReplayPasses, [int]$StressRunCount,
        [int]$ReplayTimeout, [int]$AiTimeout, [string]$Executable, [string]$OutputDirectory,
        [ValidateSet('Canonical', 'LocalCapacity')][string]$CapacityMode = 'Canonical')
    $plan = New-Object 'Collections.Generic.List[object]'
    foreach ($configuration in Get-WorkerConfigurations -CapacityMode $CapacityMode) {
        $common = @(New-CommonArguments $configuration $Data.executableSha256)
        if ($Set -ne 'AI') {
            for ($matrixRepeat = 1; $matrixRepeat -le $ReplayPasses; ++$matrixRepeat) {
                foreach ($fixture in $Data.fixtures) {
                    $fixtureRepeats = if ($fixture.stress) { $StressRunCount } else { 1 }
                    for ($repeat = 1; $repeat -le $fixtureRepeats; ++$repeat) {
                        $arguments = @($common) + @('-replay', $fixture.replayArgument)
                        Add-PlanEntry $plan 'replay' "$($fixture.id)-p$matrixRepeat" $configuration $repeat `
                            $ReplayTimeout $arguments $Executable $OutputDirectory $fixture.id `
                            $fixture.replayArgument $fixture.sha256 $fixture.stress $matrixRepeat 0 ''
                    }
                }
            }
        }
        if ($Set -ne 'Replay') {
            foreach ($scenario in $Data.ai.scenarios) {
                foreach ($seed in $Data.ai.seeds) {
                    for ($repeat = 1; $repeat -le $Data.ai.repeats; ++$repeat) {
                        $runnerFlag = if ($scenario -ceq '4v2') { '-runSkirmishAITest4v2' } else { '-runSkirmishAITest' }
                        $arguments = @($common) + @($runnerFlag, [string]$seed)
                        Add-PlanEntry $plan 'ai' "$scenario-seed-$seed" $configuration $repeat `
                            $AiTimeout $arguments $Executable $OutputDirectory "$scenario-seed-$seed" `
                            '' '' ($scenario -ceq '4v2') 0 $seed $scenario
                    }
                }
            }
        }
    }
    if ($Set -ne 'Replay' -and $Data.ai.scenarios -ccontains '4v2') {
        # One bounded installed-runtime shadow stress execution proves the
        # post-legacy collision oracle without multiplying the full matrix.
        $shadowConfiguration = Get-CollisionShadowConfiguration -CapacityMode $CapacityMode
        $shadowSeed = [int]$Data.ai.seeds[0]
        $shadowCommon = @(New-CommonArguments $shadowConfiguration $Data.executableSha256)
        $shadowArguments = @($shadowCommon) + @('-runSkirmishAITest4v2', [string]$shadowSeed)
        Add-PlanEntry $plan 'ai' "4v2-shadow-seed-$shadowSeed" $shadowConfiguration 1 `
            $AiTimeout $shadowArguments $Executable $OutputDirectory "4v2-seed-$shadowSeed" `
            '' '' $true 0 $shadowSeed '4v2'
    }
    return $plan.ToArray()
}

function Set-PreservedRegistryValue {
    param([Microsoft.Win32.RegistryView]$View, [string]$SubKey, [string]$Name,
        [string]$Value, [Collections.Generic.List[object]]$Snapshots)
    Assert-Condition ($null -ne $Snapshots) `
        'Registry snapshot destination is required before setup can mutate state.'
    $subKeys = New-Object 'Collections.Generic.List[string]'
    $currentSubKey = ''
    foreach ($segment in @($SubKey.Split('\'))) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($segment)) `
            "Registry path '$SubKey' contains an empty segment."
        $currentSubKey = if ([string]::IsNullOrEmpty($currentSubKey)) {
            $segment
        }
        else {
            $currentSubKey + '\' + $segment
        }
        $subKeys.Add($currentSubKey) | Out-Null
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    try {
        Invoke-Stage5RegistrySetupTransaction $subKeys.ToArray() -ActionContext $base `
            -EnsureSubKeyAction {
            param($candidateSubKey, $registryBase)
            $existingKey = $registryBase.OpenSubKey($candidateSubKey)
            if ($null -ne $existingKey) {
                $existingKey.Dispose()
                return $false
            }
            $createdKey = $null
            $createdKey = $registryBase.CreateSubKey($candidateSubKey)
            try {
                Assert-Condition ($null -ne $createdKey) `
                    "Failed to create registry key '$candidateSubKey'."
            }
            finally {
                if ($null -ne $createdKey) { $createdKey.Dispose() }
            }
            return $true
        } -CaptureValueAction {
            param($createdSubKeys, $registryBase)
            $key = $registryBase.OpenSubKey($SubKey, $true)
            Assert-Condition ($null -ne $key) "Failed to open registry key '$SubKey' for validation."
            try {
                $names = @($key.GetValueNames())
                $hadValue = $names -contains $Name
                $oldValue = $null
                $oldKind = $null
                if ($hadValue) {
                    $oldValue = $key.GetValue($Name, $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $oldKind = $key.GetValueKind($Name)
                }
                return [pscustomobject]@{
                    view = $View; subKey = $SubKey; name = $Name
                    hadKey = -not ($createdSubKeys -ccontains $SubKey)
                    hadValue = $hadValue; oldValue = $oldValue; oldKind = $oldKind
                    createdSubKeys = @($createdSubKeys)
                }
            }
            finally { $key.Dispose() }
        } -SetValueAction {
            param($snapshot, $registryBase)
            $key = $registryBase.OpenSubKey($snapshot.subKey, $true)
            Assert-Condition ($null -ne $key) `
                "Failed to reopen registry key '$($snapshot.subKey)' for validation."
            try {
                $key.SetValue($snapshot.name, $Value,
                    [Microsoft.Win32.RegistryValueKind]::String)
            }
            finally { $key.Dispose() }
        } -RegisterSnapshotAction {
            param($snapshot)
            $Snapshots.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $registryBase)
            $key = $registryBase.OpenSubKey($snapshot.subKey, $true)
            if ($null -eq $key) {
                Assert-Condition (-not $snapshot.hadKey) `
                    "Pre-existing registry key '$($snapshot.subKey)' disappeared during setup rollback."
                return
            }
            try {
                if ($snapshot.hadValue) {
                    $key.SetValue($snapshot.name, $snapshot.oldValue, $snapshot.oldKind)
                }
                else {
                    $key.DeleteValue($snapshot.name, $false)
                }
            }
            finally { $key.Dispose() }
        } -CleanupCreatedSubKeysAction {
            param($createdSubKeys, $registryBase)
            Invoke-Stage5CreatedRegistryKeyCleanup @($createdSubKeys) -ActionContext $registryBase `
                -InspectAction {
                param($createdSubKey, $cleanupBase)
                $createdKey = $cleanupBase.OpenSubKey($createdSubKey)
                if ($null -eq $createdKey) { return $null }
                try {
                    return [pscustomobject]@{
                        valueNames = @($createdKey.GetValueNames())
                        subKeyNames = @($createdKey.GetSubKeyNames())
                    }
                }
                finally { $createdKey.Dispose() }
            } -RemoveAction {
                param($createdSubKey, $cleanupBase)
                $cleanupBase.DeleteSubKey($createdSubKey, $false)
            }
        } | Out-Null
    }
    finally {
        $base.Dispose()
    }
}

function Restore-RegistryValue {
    param([object]$Snapshot)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $Snapshot.view)
    try {
        $restoreErrors = New-Object 'Collections.Generic.List[string]'
        try {
            $key = $base.OpenSubKey($Snapshot.subKey, $true)
            if ($null -eq $key) {
                Assert-Condition (-not $Snapshot.hadKey) `
                    "Pre-existing registry key '$($Snapshot.subKey)' disappeared before restoration."
            }
            else {
                try {
                    if ($Snapshot.hadValue) {
                        $key.SetValue($Snapshot.name, $Snapshot.oldValue, $Snapshot.oldKind)
                    }
                    else {
                        $key.DeleteValue($Snapshot.name, $false)
                    }
                }
                finally { $key.Dispose() }
            }
        }
        catch { $restoreErrors.Add("value restore: $($_.Exception.Message)") | Out-Null }
        try {
            Invoke-Stage5CreatedRegistryKeyCleanup @($Snapshot.createdSubKeys) -ActionContext $base `
                -InspectAction {
                param($createdSubKey, $registryBase)
                $createdKey = $registryBase.OpenSubKey($createdSubKey)
                if ($null -eq $createdKey) { return $null }
                try {
                    return [pscustomobject]@{
                        valueNames = @($createdKey.GetValueNames())
                        subKeyNames = @($createdKey.GetSubKeyNames())
                    }
                }
                finally { $createdKey.Dispose() }
            } -RemoveAction {
                param($createdSubKey, $registryBase)
                $registryBase.DeleteSubKey($createdSubKey, $false)
            }
        }
        catch { $restoreErrors.Add("created-key cleanup: $($_.Exception.Message)") | Out-Null }
        if ($restoreErrors.Count -gt 0) {
            throw "Registry snapshot restore failed after attempting value and key cleanup: $($restoreErrors -join ' | ')"
        }
    }
    finally {
        $base.Dispose()
    }
}

function Get-ValidationProcessIdentity {
    param([Parameter(Mandatory = $true)][int]$ProcessId)
    $processInfo = Get-CimInstance -ClassName Win32_Process `
        -Filter ("ProcessId = {0}" -f $ProcessId) -ErrorAction SilentlyContinue
    if ($null -eq $processInfo) { return $null }
    if ([string]::IsNullOrWhiteSpace([string]$processInfo.ExecutablePath)) {
        throw "Could not bind validation PID $ProcessId to an executable path."
    }
    $parentInfo = Get-CimInstance -ClassName Win32_Process `
        -Filter ("ProcessId = {0}" -f [int]$processInfo.ParentProcessId) `
        -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        processId = [int]$processInfo.ProcessId
        executablePath = [IO.Path]::GetFullPath([string]$processInfo.ExecutablePath)
        creationToken = [string]$processInfo.CreationDate
        processCreationUtc = ConvertTo-UtcIsoTimestamp $processInfo.CreationDate
        parentProcessId = [int]$processInfo.ParentProcessId
        parentCreationToken = if ($null -eq $parentInfo) { '' } else { [string]$parentInfo.CreationDate }
    }
}

function Stop-ValidationProcessSafely {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [AllowNull()][object]$ExpectedIdentity = $null,
        [int]$PostKillWaitMilliseconds = 30000
    )
    if ($Process.HasExited) {
        # The process exited between the timeout check and cleanup.  Do not
        # issue a second termination against a potentially reused PID.
        return $false
    }
    if ($null -ne $ExpectedIdentity) {
        $currentIdentity = $null
        try {
            $currentIdentity = Get-ValidationProcessIdentity $Process.Id
        }
        catch {
            # CIM is supplementary evidence only.  The Process object returned
            # by Start() remains bound to the original OS process handle, so a
            # transient identity-query failure must not discard that handle or
            # fall back to a fresh PID lookup.
            Write-Warning "Could not refresh validation PID $($Process.Id) identity; terminating through the original Process handle."
        }
        if ($null -ne $currentIdentity) {
            $sameExecutable = [String]::Equals(
                $currentIdentity.executablePath, $ExpectedIdentity.executablePath,
                [StringComparison]::OrdinalIgnoreCase)
            $sameCreation = $currentIdentity.creationToken -ceq $ExpectedIdentity.creationToken
            $sameParent = $currentIdentity.parentProcessId -eq $ExpectedIdentity.parentProcessId -and
                $currentIdentity.parentCreationToken -ceq $ExpectedIdentity.parentCreationToken
            if (-not ($sameExecutable -and $sameCreation -and $sameParent)) {
                throw "Refusing to terminate validation PID $($Process.Id) because its path or process/parent creation identity changed."
            }
        }
        elseif ($Process.HasExited) {
            return $false
        }
        else {
            Write-Warning "Validation PID $($Process.Id) identity is unavailable; terminating through the original Process handle."
        }
    }
    try {
        # Kill only through the Process instance retained immediately after
        # Start().  Never reacquire by PID: doing so reopens a PID-reuse TOCTOU
        # window between identity inspection and termination.
        $Process.Kill()
    }
    catch {
        try {
            if ($Process.HasExited) { return $false }
        }
        catch { }
        throw "Identity-bound validation process termination failed: $($_.Exception.Message)"
    }
    if (-not $Process.WaitForExit($PostKillWaitMilliseconds)) {
        throw "Validation PID $($Process.Id) did not exit within the bounded post-kill wait."
    }
    return $true
}

function Invoke-ValidationProcess {
    param(
        [string]$Executable,
        [string]$WorkingDirectory,
        [object]$Entry,
        [bool]$CaptureTiming,
        [hashtable]$Environment,
        [string]$EvidenceRoot = '',
        [AllowNull()][object]$NativeObservationBinding = $null
    )
    $nativeBinding = Get-NativeObservationBinding $NativeObservationBinding
    Assert-Condition ($null -eq $nativeBinding -or $CaptureTiming) `
        'Native observation requires frame timing before starting the installed process.'
    $processName = [IO.Path]::GetFileNameWithoutExtension($Executable)
    Assert-Condition (@(Get-Process -Name $processName -ErrorAction SilentlyContinue).Count -eq 0) `
        "A $processName process is already running. Validation will not overlap game processes."
    $stdoutDirectory = Split-Path -Parent $Entry.stdout
    if (-not (Test-Path -LiteralPath $stdoutDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $stdoutDirectory | Out-Null
    }
    if ($CaptureTiming) {
        New-Item -ItemType Directory -Path $Entry.timingDirectory -Force | Out-Null
    }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.Arguments = (($Entry.arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($environmentName in $Environment.Keys) {
        $startInfo.EnvironmentVariables[[string]$environmentName] =
            [string]$Environment[$environmentName]
    }
    if ($CaptureTiming) {
        $startInfo.EnvironmentVariables['RTS_FRAME_TIMING_DIR'] = $Entry.timingDirectory
    }
    elseif ($startInfo.EnvironmentVariables.ContainsKey('RTS_FRAME_TIMING_DIR')) {
        $startInfo.EnvironmentVariables.Remove('RTS_FRAME_TIMING_DIR')
    }
    $processRunNonce = [Guid]::NewGuid().ToString()
    $startInfo.EnvironmentVariables['RTS_STAGE5_RUN_NONCE'] = $processRunNonce
    $startInfo.EnvironmentVariables['RTS_STAGE5_COHORT_NONCE'] = $executionCohortNonce
    $startInfo.EnvironmentVariables['RTS_STAGE5_COHORT_CREATED_UTC'] = $executionCohortCreatedUtc
    $nativeReceiptDirectory = Set-NativePerformanceObservationEnvironment `
        -Environment $startInfo.EnvironmentVariables -Binding $nativeBinding -Entry $Entry `
        -EvidenceRoot $EvidenceRoot -RunNonce $processRunNonce `
        -CohortNonce $executionCohortNonce -CohortCreatedUtc $executionCohortCreatedUtc
    if ($null -ne $nativeReceiptDirectory) {
        if (-not (Test-Path -LiteralPath $nativeReceiptDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $nativeReceiptDirectory -Force | Out-Null
        }
    }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    $startedAt = [DateTime]::UtcNow
    $started = $false
    $terminationAttempted = $false
    $processIdentity = $null
    $processCreationUtc = ''
    $stdoutTask = $null
    $stderrTask = $null
    $exited = $false
    $timedOut = $false
    $postKillWaitMilliseconds = 30000
    try {
        Assert-Condition ($process.Start()) "Failed to start installed runtime process."
        $started = $true
        try {
            $processCreationUtc = ConvertTo-UtcIsoTimestamp $process.StartTime
        }
        catch { $processCreationUtc = '' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        try {
            $processIdentity = Get-ValidationProcessIdentity $process.Id
        }
        catch {
            Write-Warning "Could not capture validation PID $($process.Id) identity; retaining the original Process handle for bounded cleanup."
            $processIdentity = $null
        }
        if ($null -eq $processIdentity -and -not $process.HasExited) {
            Write-Warning "Validation PID $($process.Id) identity is unavailable; retaining the original Process handle for bounded cleanup."
        }
        if ($null -ne $processIdentity -and
                -not [String]::Equals($processIdentity.executablePath,
                    [IO.Path]::GetFullPath($Executable), [StringComparison]::OrdinalIgnoreCase)) {
            throw "Validation process path does not match the requested executable: $($processIdentity.executablePath)"
        }
        if ($null -ne $processIdentity -and
            -not [string]::IsNullOrWhiteSpace([string]$processIdentity.processCreationUtc)) {
            $processCreationUtc = [string]$processIdentity.processCreationUtc
        }

        $exited = $process.WaitForExit($Entry.timeoutSeconds * 1000)
        if (-not $exited) {
            $timedOut = $true
            $terminationAttempted = $true
            Stop-ValidationProcessSafely $process $processIdentity $postKillWaitMilliseconds | Out-Null
            # Keep the final state bounded even if the process disappeared in
            # the race between identity inspection and Kill().
            $exited = $process.WaitForExit($postKillWaitMilliseconds)
            if (-not $exited) {
                throw "Validation process remained alive after the bounded post-kill wait."
            }
        }
        if ($null -eq $stdoutTask -or -not $stdoutTask.Wait($postKillWaitMilliseconds)) {
            throw 'Validation stdout did not complete within the bounded post-process wait.'
        }
        if ($null -eq $stderrTask -or -not $stderrTask.Wait($postKillWaitMilliseconds)) {
            throw 'Validation stderr did not complete within the bounded post-process wait.'
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        [IO.File]::WriteAllText($Entry.stdout, $stdout)
        [IO.File]::WriteAllText($Entry.stderr, $stderr)
        $childProcess = $null
        if (-not [string]::IsNullOrWhiteSpace($processCreationUtc)) {
            $childProcess = [pscustomobject]@{
                processId = [int]$process.Id
                runNonce = $processRunNonce
                processCreationUtc = $processCreationUtc
                executablePath = [IO.Path]::GetFullPath($Executable)
                commandLine = [string]$Entry.command
                nativeReceipt = $null
            }
            if (-not [string]::IsNullOrWhiteSpace($EvidenceRoot) -and
                $null -ne $nativeBinding) {
                $nativeRole = if ($Entry.kind -ceq 'ai') { 'ai-results' } else { 'replay-results' }
                $childProcess.nativeReceipt = Get-NativePerformanceReceiptReference `
                    -OutputText ($stdout + "`n" + $stderr) `
                    -OutputRoot $EvidenceRoot -WorkingDirectory $WorkingDirectory `
                    -Role $nativeRole -SourceCommit $nativeBinding.sourceCommit `
                    -ArtifactSetSha256 $nativeBinding.artifactSetSha256 `
                    -ExecutableSha256 $manifestData.executableSha256 `
                    -RunNonce $processRunNonce `
                    -CohortNonce $executionCohortNonce `
                    -RuntimeClosure $nativeBinding.runtimeClosure `
                    -ExpectedTitle $manifestData.title `
                    -ProcessId ([int]$process.Id) `
                    -ProcessCreationUtc $processCreationUtc `
                    -ExpectedCohortCreatedUtc $executionCohortCreatedUtc `
                    -ExpectedExecutablePath ([IO.Path]::GetFullPath($Executable))
            }
        }
        $runtimeLogText = New-Object 'Collections.Generic.List[string]'
        foreach ($runtimeLog in @(Get-ChildItem -LiteralPath $WorkingDirectory -Filter '*DebugLogFile*.txt' -File |
            Where-Object { $_.LastWriteTimeUtc -ge $startedAt.AddSeconds(-2) })) {
            if (-not (Test-Path -LiteralPath $Entry.runtimeLogDirectory -PathType Container)) {
                New-Item -ItemType Directory -Path $Entry.runtimeLogDirectory -Force | Out-Null
            }
            Copy-Item -LiteralPath $runtimeLog.FullName -Destination `
                (Join-Path $Entry.runtimeLogDirectory $runtimeLog.Name)
            $runtimeLogText.Add((Get-Content -LiteralPath $runtimeLog.FullName -Raw)) | Out-Null
        }
        return [pscustomobject]@{
            timedOut = $timedOut
            exitCode = $(if ($exited) { $process.ExitCode } else { -1 })
            wallMilliseconds = [int64]([DateTime]::UtcNow - $startedAt).TotalMilliseconds
            stdout = $stdout
            stderr = $stderr
            runtimeLogText = $runtimeLogText.ToArray() -join "`n"
            childProcess = $childProcess
        }
    }
    finally {
        $cleanupErrors = New-Object 'Collections.Generic.List[string]'
        if ($started) {
            try {
                if (-not $process.HasExited) {
                    if ($terminationAttempted) {
                        if (-not $process.WaitForExit($postKillWaitMilliseconds)) {
                            throw 'Validation process remained alive after its bounded termination attempt.'
                        }
                    }
                    else {
                        $terminationAttempted = $true
                        Stop-ValidationProcessSafely $process $processIdentity $postKillWaitMilliseconds | Out-Null
                    }
                }
            }
            catch { $cleanupErrors.Add($_.Exception.Message) | Out-Null }
        }
        if ($null -ne $process) { $process.Dispose() }
        if ($cleanupErrors.Count -gt 0) {
            throw "Validation process cleanup failed: $($cleanupErrors -join ' | ')"
        }
    }
}

function Get-Stage5ReceiptRawLogBindings {
    param(
        [string]$OutputRoot,
        [object[]]$Paths,
        [string]$Context
    )
    $bindings = New-Object 'Collections.Generic.List[object]'
    $seenPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $seenNames = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    foreach ($pathValue in @($Paths)) {
        if ($null -eq $pathValue -or [string]::IsNullOrWhiteSpace([string]$pathValue)) {
            continue
        }
        $path = [IO.Path]::GetFullPath([string]$pathValue)
        Assert-Condition (Test-Path -LiteralPath $path -PathType Leaf) `
            "$Context raw log was not written: $path"
        $relative = ConvertTo-OutputRelativePath $path $OutputRoot "$Context raw log"
        Assert-Condition $seenPaths.Add($relative) `
            "$Context repeats raw log path '$relative'."
        $name = [IO.Path]::GetFileName($relative)
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($name) -and $seenNames.Add($name)) `
            "$Context repeats raw log name '$name'."
        $snapshot = Get-Stage5FileSnapshot $path "$Context raw log"
        $bindings.Add([ordered]@{
            name = $name
            path = $relative
            sha256 = $snapshot.sha256
        }) | Out-Null
    }
    Assert-Condition ($bindings.Count -gt 0) "$Context must retain at least one raw log."
    return ,$bindings.ToArray()
}

function New-Stage5HostChildBinding {
    param(
        [object]$ChildRun,
        [string]$Role,
        [string]$Title,
        [string]$ReceiptNonce,
        [string]$CohortNonce,
        [string]$OutputRoot,
        [string]$ExecutableSha256,
        [Collections.IDictionary]$RuntimeClosure,
        [string]$Context
    )
    Assert-Condition ($null -ne $ChildRun -and $null -ne $ChildRun.childProcess) `
        "$Context has no retained process provenance."
    $child = $ChildRun.childProcess
    Assert-Condition (([int]$child.processId) -gt 0 -and
        -not [string]::IsNullOrWhiteSpace([string]$child.processCreationUtc) -and
        -not [string]::IsNullOrWhiteSpace([string]$child.executablePath) -and
        -not [string]::IsNullOrWhiteSpace([string]$child.commandLine)) `
        "$Context retained process provenance is incomplete."
    $entry = $ChildRun.entry
    $stdoutPath = [IO.Path]::GetFullPath([string]$entry.stdout)
    $stderrPath = [IO.Path]::GetFullPath([string]$entry.stderr)
    $stdoutSnapshot = Get-Stage5FileSnapshot $stdoutPath "$Context stdout"
    $stderrSnapshot = Get-Stage5FileSnapshot $stderrPath "$Context stderr"
    $binding = [ordered]@{
        role = $Role
        title = $Title
        runNonce = $ReceiptNonce
        processId = [int]$child.processId
        processCreationUtc = [string]$child.processCreationUtc
        executablePath = [IO.Path]::GetFullPath([string]$child.executablePath)
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        commandLine = [string]$child.commandLine
        exitCode = [int]$ChildRun.run.exitCode
        stdout = [ordered]@{
            path = ConvertTo-OutputRelativePath $stdoutPath $OutputRoot "$Context stdout"
            sha256 = $stdoutSnapshot.sha256
        }
        stderr = [ordered]@{
            path = ConvertTo-OutputRelativePath $stderrPath $OutputRoot "$Context stderr"
            sha256 = $stderrSnapshot.sha256
        }
    }
    Assert-Condition ($null -ne $child.nativeReceipt -and
        [string]$child.nativeReceipt.runNonce -ceq $ReceiptNonce -and
        [string]$child.nativeReceipt.cohortNonce -ceq $CohortNonce) `
        "$Context lacks a native executable receipt bound to the same run and execution cohort."
    $nativePath = Join-Path $OutputRoot ([string]$child.nativeReceipt.path)
    $nativeSnapshot = Get-Stage5FileSnapshot $nativePath `
        "$Context native v2 receipt"
    $binding.nativeReceipt = [ordered]@{
        path = ConvertTo-OutputRelativePath $nativePath $OutputRoot "$Context native receipt"
        sha256 = $nativeSnapshot.sha256
        producer = [string]$child.nativeReceipt.producer
        runNonce = [string]$child.nativeReceipt.runNonce
        cohortNonce = $CohortNonce
    }
    return $binding
}

function Write-Stage5HostRunnerReceipt {
    param(
        [string]$Role,
        [string]$OutputRoot,
        [string]$ReceiptPath,
        [string]$Title,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$ExecutableSha256,
        [object[]]$RawLogPaths,
        [object[]]$ChildRuns,
        [Collections.IDictionary]$Details,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [Collections.IDictionary]$RuntimeClosure
    )
    Assert-Condition ($Role -in @('validation-plan', 'validation-results',
        'replay-results', 'ai-results', 'performance-report')) `
        "Unsupported host-runner receipt role '$Role'."
    Assert-Condition ($SourceCommit -cmatch '^[0-9a-f]{40}$') `
        'AcceptanceSourceCommit must be an independently supplied lowercase 40-hex commit.'
    Assert-Condition ($ArtifactSetSha256 -match '^[0-9A-Fa-f]{64}$') `
        'AcceptanceArtifactSetSha256 must contain exactly 64 hexadecimal characters.'
    Assert-Condition ($ExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Host-runner receipt executable SHA-256 is invalid.'
    Assert-CanonicalUuid $CohortNonce 'Host-runner receipt cohortNonce' | Out-Null
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    Assert-Condition ($CohortCreatedUtc -is [string] -and
        [DateTimeOffset]::TryParse($CohortCreatedUtc, [ref]$cohortCreated)) `
        'Host-runner receipt cohortCreatedUtc is not a valid timestamp.'
    Assert-Condition (($RuntimeClosure -is [Collections.IDictionary]) -and
        (Test-Sha256Text ([string]$RuntimeClosure['dependencyManifestSha256'])) -and
        (Test-Sha256Text ([string]$RuntimeClosure['closureSha256']))) `
        'Host-runner receipt runtime closure binding is incomplete.'
    $receiptFull = [IO.Path]::GetFullPath($ReceiptPath)
    Assert-Condition (-not (Test-Path -LiteralPath $receiptFull)) `
        "Host-runner receipt output already exists: $receiptFull"
    $rawLogs = Get-Stage5ReceiptRawLogBindings $OutputRoot $RawLogPaths `
        "Host-runner '$Role' receipt"
    $children = New-Object 'Collections.Generic.List[object]'
    $candidate = $null
    if ($Role -ne 'validation-plan') {
        # The immutable receipt contract binds one observed child identity to
        # each role receipt.  The complete execution set remains covered by
        # the role's hashed result log; selecting a real child here avoids
        # inventing a process identity or a synthetic native receipt.
        $expectedNativeProducers = switch ($Role) {
            default { @('game-executable-stage5-performance-report-v5') }
        }
        $candidate = @($ChildRuns | Where-Object {
            $null -ne $_.childProcess -and
            $null -ne $_.childProcess.nativeReceipt -and
            $expectedNativeProducers -ccontains
                [string]$_.childProcess.nativeReceipt.producer
        }) | Select-Object -First 1
        Assert-Condition ($null -ne $candidate) `
            "Host-runner '$Role' receipt cannot pass without a retained child process and native receipt from '$($expectedNativeProducers -join ', ')'."
        $childTitle = $Title
        Assert-Condition ($childTitle -in @('Generals', 'ZeroHour')) `
            "Host-runner '$Role' receipt must bind a concrete title child."
    }
    $receiptNonce = [Guid]::NewGuid().ToString()
    if ($null -ne $candidate -and $null -ne $candidate.childProcess.nativeReceipt -and
        [string]$candidate.childProcess.runNonce -match
            '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') {
        # A native v2 receipt is only carried when its producer-bound nonce can
        # also identify this host child.  If multiple role receipts select the
        # same process, the host-domain nonce remains fresh and the native
        # reference is omitted rather than weakening the global replay check.
        $receiptNonce = [string]$candidate.childProcess.runNonce
    }
    if ($null -ne $candidate) {
        $children.Add((New-Stage5HostChildBinding $candidate $Role $childTitle `
            $receiptNonce $CohortNonce $OutputRoot $ExecutableSha256 $RuntimeClosure `
            "Host-runner '$Role' child")) | Out-Null
    }
    $recordedUtc = [DateTimeOffset]::UtcNow
    Assert-Condition ($recordedUtc -ge $cohortCreated) `
        "Host-runner '$Role' receipt recordedUtc predates the execution cohort."
    $document = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-host-runner-receipt'
        status = 'passed'
        role = $Role
        trustDomain = 'host-runner'
        producer = "installed-runtime-$Role-v2"
        producerVersion = '2'
        runNonce = $receiptNonce
        sourceCommit = $SourceCommit
        title = $Title
        architecture = 'x64'
        artifactSetSha256 = $ArtifactSetSha256.ToUpperInvariant()
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        cohortNonce = $CohortNonce
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = ([string]$RuntimeClosure['dependencyManifestSha256']).ToUpperInvariant()
            closureSha256 = ([string]$RuntimeClosure['closureSha256']).ToUpperInvariant()
        }
        recordedUtc = $recordedUtc.ToString('o')
        rawLogs = $rawLogs
        provenance = [ordered]@{
            kind = 'host-runner-observation'
            runner = 'Run-DeterministicSimulationValidation.ps1'
            runnerVersion = '1'
            childProvenance = if ($Role -eq 'validation-plan') { 'not-applicable' } else { 'bound' }
            children = @($children.ToArray())
        }
        details = $Details
    }
    [IO.File]::WriteAllText($receiptFull, ($document | ConvertTo-Json -Depth 12))
    return [pscustomobject]@{
        path = $receiptFull
        sha256 = Get-Sha256 $receiptFull
        role = $Role
        runNonce = $receiptNonce
        cohortNonce = $CohortNonce
    }
}

function Write-LocalCapacityReceipt {
    param(
        [string]$Path,
        [string]$Status,
        [string]$ValidationSet,
        [object[]]$Entries,
        [object[]]$Results,
        [object[]]$Configurations,
        [object]$ShadowConfiguration,
        [object]$Topology,
        [string]$PlanPath,
        [string]$ResultsPath,
        [string]$CorpusExportRoot = '',
        [object]$CorpusExport = $null
    )
    Assert-Condition ($Status -in @('planned-non-acceptance',
        'passed-non-acceptance')) `
        "Unsupported LocalCapacity receipt status '$Status'."
    $receiptFull = [IO.Path]::GetFullPath($Path)
    Assert-Condition (-not (Test-Path -LiteralPath $receiptFull)) `
        "LocalCapacity receipt output already exists: $receiptFull"
    $entryArray = @($Entries)
    $resultArray = @($Results)
    $corpusExportRequested = -not [string]::IsNullOrWhiteSpace($CorpusExportRoot)
    $corpusExportDocument = $null
    if ($corpusExportRequested) {
        Assert-Condition ($null -ne $CorpusExport -and
            [string]$CorpusExport.status -ceq 'passed') `
            'LocalCapacity corpus export was requested but did not complete.'
        Assert-Condition ($null -ne $CorpusExport.artifactIndex -and
            -not [string]::IsNullOrWhiteSpace([string]$CorpusExport.artifactIndex.path) -and
            [string]$CorpusExport.artifactIndex.sha256 -match '^[0-9A-Fa-f]{64}$') `
            'LocalCapacity corpus export did not return a valid artifact-index binding.'
        $artifactIndexPath = [IO.Path]::GetFullPath([string]$CorpusExport.artifactIndex.path)
        Assert-Condition (Test-Path -LiteralPath $artifactIndexPath -PathType Leaf) `
            "LocalCapacity artifact index was not found: $artifactIndexPath"
        $artifactIndexSha256 = Get-Sha256 $artifactIndexPath
        Assert-Condition ($artifactIndexSha256 -ceq
            ([string]$CorpusExport.artifactIndex.sha256).ToUpperInvariant()) `
            'LocalCapacity artifact index SHA-256 changed before receipt binding.'
        $corpusExportDocument = [ordered]@{
            status = 'passed'
            artifactIndexPath = $artifactIndexPath
            artifactIndexSha256 = $artifactIndexSha256
            recordCount = [int]$CorpusExport.recordCount
            records = @($CorpusExport.records)
        }
    }
    $document = [ordered]@{
        schemaVersion = 1
        receiptKind = 'stage5-local-capacity-receipt'
        status = $Status
        validationMode = 'LocalCapacity'
        capacityMode = 'LocalCapacity'
        notAnAcceptanceEnvelope = $true
        acceptanceEligible = $false
        externalAcceptanceEligible = $false
        deterministicRuntimeEligible = $false
        finalAcceptanceEligible = $false
        canonicalFinalAcceptanceEligible = $false
        validationSet = $ValidationSet
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        topology = $Topology
        regularConfigurations = @($Configurations | ForEach-Object { $_.Id })
        shadowConfiguration = $ShadowConfiguration.Id
        entryCount = $entryArray.Count
        resultCount = $resultArray.Count
        replayResultCount = @($resultArray | Where-Object {
            $_.kind -ceq 'replay'
        }).Count
        aiResultCount = @($resultArray | Where-Object {
            $_.kind -ceq 'ai'
        }).Count
        corpusExportRequested = $corpusExportRequested
        corpusExportRoot = if ($corpusExportRequested) {
            [IO.Path]::GetFullPath($CorpusExportRoot)
        } else { $null }
        corpusExport = $corpusExportDocument
        replayDeterminismAsserted = $Status -ceq 'passed-non-acceptance' -and
            $ValidationSet -ne 'AI'
        aiDeterminismAsserted = $Status -ceq 'passed-non-acceptance' -and
            $ValidationSet -ne 'Replay'
        planSha256 = if (Test-Path -LiteralPath $PlanPath -PathType Leaf) {
            Get-Sha256 $PlanPath
        } else { $null }
        resultsSha256 = if (Test-Path -LiteralPath $ResultsPath -PathType Leaf) {
            Get-Sha256 $ResultsPath
        } else { $null }
    }
    [IO.File]::WriteAllText($receiptFull, ($document | ConvertTo-Json -Depth 12))
    return [pscustomobject]@{
        path = $receiptFull
        sha256 = Get-Sha256 $receiptFull
    }
}

function Export-LocalCapacityAiCorpus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$TaskRunRoot,
        [Parameter(Mandatory = $true)][string]$ProfileRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][object[]]$ChildRuns,
        [Parameter(Mandatory = $true)][object[]]$Results,
        [Parameter(Mandatory = $true)][string]$ValidationResultsPath
    )
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($TaskRoot) -and
        -not [string]::IsNullOrWhiteSpace($TaskRunRoot) -and
        -not [string]::IsNullOrWhiteSpace($ProfileRoot) -and
        -not [string]::IsNullOrWhiteSpace($CorpusExportRoot)) `
        'LocalCapacity corpus export requires explicit task-owned paths.'
    Assert-Condition (-not (Test-PathWithin $TaskRunRoot $CorpusExportRoot) -and
        -not (Test-PathWithin $CorpusExportRoot $TaskRunRoot)) `
        'CorpusExportRoot must be a durable TaskRoot sibling of the ephemeral validation run root.'
    $childRunArray = @($ChildRuns)
    $resultArray = @($Results)
    Assert-Condition ($childRunArray.Count -gt 0 -and $resultArray.Count -gt 0) `
        'LocalCapacity corpus export requires completed AI executions.'
    Assert-Condition (@($childRunArray | Where-Object {
        $null -eq $_.entry -or $_.entry.kind -cne 'ai'
    }).Count -eq 0 -and @($resultArray | Where-Object {
        $null -eq $_ -or $_.kind -cne 'ai'
    }).Count -eq 0) `
        'LocalCapacity corpus export accepts AI-only completed records.'
    $aiRuns = @($childRunArray | Sort-Object { [int]$_.entry.sequence })
    $aiResults = @($resultArray | Sort-Object { [int]$_.sequence })
    Assert-Condition ($aiRuns.Count -eq $aiResults.Count) `
        'LocalCapacity corpus export child-run and result counts differ.'
    $seenSequences = New-Object 'Collections.Generic.HashSet[int]'
    $records = New-Object 'Collections.Generic.List[object]'
    foreach ($child in $aiRuns) {
        $entry = $child.entry
        $run = $child.run
        Assert-Condition ($null -ne $run -and $run.exitCode -eq 0 -and
            -not [bool]$run.timedOut) `
            "LocalCapacity corpus export requires a completed AI run at sequence $($entry.sequence)."
        Assert-Condition $seenSequences.Add([int]$entry.sequence) `
            "LocalCapacity corpus export repeats AI sequence $($entry.sequence)."
        $matchingResults = @($aiResults | Where-Object {
            [int]$_.sequence -eq [int]$entry.sequence
        })
        Assert-Condition ($matchingResults.Count -eq 1 -and
            $null -ne $matchingResults[0].aiEvidence) `
            "LocalCapacity corpus export has no parsed AI result for sequence $($entry.sequence)."
        $completion = Get-Stage5ReplayCompletionFields `
            -Output ([string]$run.stdout) -ExpectedSeed ([int]$entry.seed) `
            -ExpectedScenario ([string]$entry.scenario) `
            -Context "LocalCapacity AI sequence $($entry.sequence) completion"
        $artifact = Export-Stage5FreshReplayArtifact `
            -SourcePath $completion.replayRetained `
            -ExpectedSha256 $completion.replaySha256 `
            -TaskRoot $TaskRoot -TaskRunRoot $TaskRunRoot `
            -ProfileRoot $ProfileRoot -CorpusExportRoot $CorpusExportRoot `
            -Metadata ([ordered]@{
                title = $Title
                category = 'local-capacity-ai'
                scenario = [string]$entry.scenario
                seed = [string]$entry.seed
                runNonce = $completion.runNonce
                executableSha256 = $ExecutableSha256
                origin = 'native-fresh-runtime'
            })
        $records.Add([pscustomobject]@{
            sequence = [int]$entry.sequence
            configuration = [string]$entry.configuration
            repeat = [int]$entry.repeat
            scenario = [string]$entry.scenario
            seed = [int]$entry.seed
            runNonce = $completion.runNonce
            replayEpoch = $completion.replayEpoch
            replaySha256 = $completion.replaySha256
            origin = $artifact.origin
            title = $artifact.title
            category = $artifact.category
            executableSha256 = $artifact.executableSha256
            sourceProfileRoot = $artifact.sourceProfileRoot
            sourcePath = $artifact.sourcePath
            sourceSha256 = $artifact.sourceSha256
            destinationPath = $artifact.destinationPath
            destinationSha256 = $artifact.destinationSha256
            length = $artifact.length
            containerMagic = $artifact.containerMagic
            containerSchemaVersion = $artifact.containerSchemaVersion
            containerEngineEpoch = $artifact.containerEngineEpoch
            payloadMagic = $artifact.payloadMagic
            skirmishAiReplayEpoch = $artifact.skirmishAiReplayEpoch
            exportedUtc = $artifact.exportedUtc
        }) | Out-Null
    }
    Assert-Condition ($records.Count -gt 0) `
        'LocalCapacity corpus export produced no replay records.'
    $artifactIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $TaskRoot -CorpusExportRoot $CorpusExportRoot `
        -Title $Title -ExecutableSha256 $ExecutableSha256 `
        -Records $records.ToArray() -ValidationResultsPath $ValidationResultsPath
    return [pscustomobject]@{
        status = 'passed'
        corpusExportRoot = [IO.Path]::GetFullPath($CorpusExportRoot)
        artifactIndex = $artifactIndex
        recordCount = $records.Count
        records = $records.ToArray()
    }
}

function Get-Stage5ResultTreeSha256 {
    param([object[]]$Results, [string]$Kind)
    $lines = New-Object 'Collections.Generic.List[string]'
    foreach ($result in @($Results | Where-Object { $_.kind -ceq $Kind } |
        Sort-Object sequence)) {
        if ($Kind -ceq 'replay') {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.determinismKey, $result.matrixRepeat,
                $result.repeat, $result.replayResult.finalFrame,
                $result.replayResult.finalCRC)) | Out-Null
        }
        else {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.scenario, $result.seed,
                $result.configuration, $result.repeat,
                $result.aiEvidence.finalDigest)) | Out-Null
        }
    }
    return Get-Sha256Text ((($lines.ToArray()) -join "`n") + "`n")
}

Assert-Condition ($ReplayMatrixRepeats -gt 0 -and $ReplayMatrixRepeats -le 10) `
    'ReplayMatrixRepeats must be between 1 and 10.'
Assert-Condition ($StressRepeats -gt 0 -and $StressRepeats -le 10) `
    'StressRepeats must be between 1 and 10.'
$localCapacityRequested = $CapacityMode -ceq 'LocalCapacity'
$corpusExportRequested = -not [string]::IsNullOrWhiteSpace($CorpusExportRoot)
$diagnosticNonAcceptanceRequested = [bool]$DiagnosticNonAcceptance -or
    $localCapacityRequested
$deterministicRuntimeContractRequested = $ValidationSet -ceq 'All' -and
    -not $diagnosticNonAcceptanceRequested -and -not [bool]$AllowNonStandardCorpus
Assert-Condition (-not $deterministicRuntimeContractRequested -or $ReplayMatrixRepeats -eq 2) `
    'The deterministic-runtime gate requires exactly two complete replay matrix passes.'
Assert-Condition (-not $deterministicRuntimeContractRequested -or $StressRepeats -eq 3) `
    'The deterministic-runtime gate requires exactly three executions of the stress replay per configuration and matrix pass.'
Assert-Condition ($ReplayTimeoutSeconds -gt 0 -and $ReplayTimeoutSeconds -le 86400 -and
    $AiTimeoutSeconds -gt 0 -and $AiTimeoutSeconds -le 86400) `
    'Timeouts must be between 1 and 86400 seconds.'
Assert-Condition (-not $EnforcePerformance -or $ValidationSet -ne 'AI') `
    'Performance validation requires the replay matrix.'
Assert-Condition (-not $AllowNonStandardCorpus -or $PlanOnly -or $diagnosticNonAcceptanceRequested) `
    'AllowNonStandardCorpus is limited to PlanOnly or DiagnosticNonAcceptance; accepting execution requires exactly 10 unique replay sources.'
Assert-Condition (-not $DisableFrameTiming -or $diagnosticNonAcceptanceRequested) `
    'DisableFrameTiming is allowed only with DiagnosticNonAcceptance; timing is mandatory for an accepting gate.'
Assert-Condition (-not $EnforcePerformance -or (-not $DisableFrameTiming -and -not $diagnosticNonAcceptanceRequested)) `
    'Performance validation cannot run in non-acceptance diagnostic mode or without frame timing.'
Assert-Condition (-not $EnforcePerformance -or
    ($ReplayMatrixRepeats * $StressRepeats) -ge 4) `
    'Performance validation requires one warm-up and at least three measured stress runs per configuration.'
$acceptanceBindingsRequested = -not [string]::IsNullOrWhiteSpace($AcceptanceSourceCommit) -or
    -not [string]::IsNullOrWhiteSpace($AcceptanceArtifactSetSha256) -or
    -not [string]::IsNullOrWhiteSpace($AcceptanceRuntimeDependencyManifestSha256) -or
    -not [string]::IsNullOrWhiteSpace($AcceptanceRuntimeClosureSha256)
if ($acceptanceBindingsRequested) {
    Assert-Condition ($AcceptanceSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'AcceptanceSourceCommit must be an independently supplied lowercase 40-hex commit.'
    Assert-Condition ($AcceptanceArtifactSetSha256 -match '^[0-9A-Fa-f]{64}$') `
        'AcceptanceArtifactSetSha256 must contain exactly 64 hexadecimal characters.'
    Assert-Condition ((Test-Sha256Text $AcceptanceRuntimeDependencyManifestSha256) -and
        (Test-Sha256Text $AcceptanceRuntimeClosureSha256)) `
        'Acceptance runtime closure requires independently supplied dependency-manifest and closure SHA-256 values.'
}
Assert-Condition (-not ($localCapacityRequested -and $EnforcePerformance)) `
    'LocalCapacity cannot request performance enforcement; use the canonical performance lane.'
Assert-Condition (-not ($localCapacityRequested -and $acceptanceBindingsRequested)) `
    'LocalCapacity cannot request canonical acceptance bindings or receipts.'
Assert-Condition (-not $corpusExportRequested -or
    ($localCapacityRequested -and $ValidationSet -ceq 'AI' -and -not $PlanOnly)) `
    'CorpusExportRoot requires an executing LocalCapacity AI-only validation.'
$executionCohortNonce = if ([string]::IsNullOrWhiteSpace($ExecutionCohortNonce)) {
    [Guid]::NewGuid().ToString()
} else {
    Assert-CanonicalUuid $ExecutionCohortNonce 'ExecutionCohortNonce'
}
$executionCohortCreated = [DateTimeOffset]::UtcNow
if (-not [string]::IsNullOrWhiteSpace($ExecutionCohortCreatedUtc)) {
    [DateTimeOffset]$providedCohortCreated = [DateTimeOffset]::MinValue
    Assert-Condition ([DateTimeOffset]::TryParse($ExecutionCohortCreatedUtc,
        [ref]$providedCohortCreated)) `
        'ExecutionCohortCreatedUtc must be a valid timestamp.'
    $executionCohortCreated = $providedCohortCreated.ToUniversalTime()
}
$executionCohortCreatedUtc = $executionCohortCreated.ToString('o')
$hostRunnerRuntimeClosure = if ($acceptanceBindingsRequested) {
    Get-HostRunnerRuntimeClosure 'Acceptance-bound host-runner execution'
} else { $null }
$nativeObservationBinding = if ($acceptanceBindingsRequested) {
    Get-NativeObservationBinding ([ordered]@{
        sourceCommit = $AcceptanceSourceCommit
        artifactSetSha256 = $AcceptanceArtifactSetSha256
        runtimeClosure = $hostRunnerRuntimeClosure
    })
} else { $null }
if (-not $PlanOnly) {
    Assert-Condition ([bool]$AllowHeadlessDirectExecution) `
        'Installed validation requires the reviewed -AllowHeadlessDirectExecution exception.'
}

$runtimeFull = [IO.Path]::GetFullPath($RuntimeRoot)
$manifestData = Get-ManifestData $FixtureManifestPath $ValidationSet `
    ([bool]$AllowNonStandardCorpus) $ExpectedExecutableSha256 $Title `
    ($localCapacityRequested -and $ValidationSet -ceq 'AI')
$executableFull = Join-Path $runtimeFull $manifestData.executable
Assert-Condition (Test-Path -LiteralPath $runtimeFull -PathType Container) "Installed runtime root was not found: $runtimeFull"
Assert-Condition (Test-Path -LiteralPath (Join-Path $runtimeFull 'launcher.exe') -PathType Leaf) `
    'Installed runtime root is missing launcher.exe.'
Assert-Condition (Test-Path -LiteralPath (Join-Path $runtimeFull 'launcher.lcf') -PathType Leaf) `
    'Installed runtime root is missing launcher.lcf.'
Assert-Condition (Test-Path -LiteralPath $executableFull -PathType Leaf) `
    "Installed runtime executable was not found: $executableFull"
Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable' | Out-Null
$launcherConfigFull = [IO.Path]::GetFullPath((Join-Path $runtimeFull 'launcher.lcf'))
$launcherContract = Get-LauncherRunContract $launcherConfigFull $runtimeFull $executableFull

$physicalCoreCount = 0
$logicalProcessorCount = 0
$localCapacityTopology = $null
$stage3PerformanceBaseline = $null
$stage3PerformanceBaselineEvidencePath = $null
if ($RequireX64 -or $EnforcePerformance) {
    Assert-X64PeExecutable $executableFull
}
if ($EnforcePerformance) {
    $stressFixtures = @($manifestData.fixtures | Where-Object { $_.stress })
    Assert-Condition ($stressFixtures.Count -eq 1) `
        'Performance validation requires exactly one stress replay fixture.'
    $stage3PerformanceBaseline = Read-Stage5PerformanceBaseline `
        $Stage3PerformanceBaselinePath $stressFixtures[0].sha256 $ExpectedStage3ExecutableSha256
    $physicalCoreCount = Get-PhysicalCoreCount
}
if ($localCapacityRequested) {
    $localCapacityTopology = Get-LocalCapacityTopology
    $physicalCoreCount = $localCapacityTopology.physicalCoreCount
    $logicalProcessorCount = $localCapacityTopology.logicalProcessorCount
}

$outputFull = [IO.Path]::GetFullPath($OutputRoot)
Assert-Condition (-not (Test-Path -LiteralPath $outputFull)) `
    'OutputRoot must not already exist; every validation run owns a fresh evidence directory.'
$taskRootFull = $null
$corpusExportRootFull = $null
if (-not $PlanOnly) {
    $taskRootFull = Resolve-ExplicitTaskRoot $TaskRoot 'TaskRoot'
    Assert-Condition (Test-Path -LiteralPath $taskRootFull -PathType Container) `
        "TaskRoot must already exist as a task-owned directory: $taskRootFull"
    Assert-TaskOwnedPath $outputFull $taskRootFull 'OutputRoot' | Out-Null
    Assert-FreeSpace $taskRootFull $MinimumFreeBytes 'Validation task volume'
    if ($corpusExportRequested) {
        $corpusExportRootFull = Resolve-TaskOwnedChildPath $CorpusExportRoot `
            $taskRootFull 'CorpusExportRoot'
        Assert-Condition (-not (Test-Path -LiteralPath $corpusExportRootFull)) `
            "CorpusExportRoot must be a fresh task-owned directory: $corpusExportRootFull"
    }
}
Assert-FreeSpace (Split-Path -Parent $outputFull) $MinimumFreeBytes 'Validation output volume'
New-Item -ItemType Directory -Path $outputFull | Out-Null
if ($null -ne $stage3PerformanceBaseline) {
    $stage3PerformanceBaselineEvidencePath = Join-Path $outputFull 'stage3-performance-baseline.json'
    Copy-Item -LiteralPath $stage3PerformanceBaseline.file -Destination $stage3PerformanceBaselineEvidencePath
    Assert-FileHash $stage3PerformanceBaselineEvidencePath $stage3PerformanceBaseline.fileSha256 `
        'Copied Stage 3 performance baseline evidence' | Out-Null
    $stage3PerformanceBaseline.evidenceFile = $stage3PerformanceBaselineEvidencePath
}

$workerConfigurations = @(Get-WorkerConfigurations -CapacityMode $CapacityMode)
$collisionShadowConfiguration = Get-CollisionShadowConfiguration -CapacityMode $CapacityMode
$plan = @(New-ValidationPlan $manifestData $ValidationSet $ReplayMatrixRepeats $StressRepeats `
    $ReplayTimeoutSeconds $AiTimeoutSeconds $executableFull $outputFull $CapacityMode)
Assert-Condition ($plan.Count -gt 0) 'The fixture manifest produced an empty validation plan.'
Assert-Condition ($plan.Count -le 10000) 'The fixture manifest produced more than 10000 validation entries.'
$launcherEquivalence = Assert-LauncherEquivalenceContract $launcherContract `
    $executableFull $runtimeFull $plan
if ($deterministicRuntimeContractRequested) {
    foreach ($configuration in @($workerConfigurations | ForEach-Object { $_.Id })) {
        $configurationReplayPlan = @($plan | Where-Object {
            $_.kind -ceq 'replay' -and $_.configuration -ceq $configuration
        })
        Assert-Condition ($configurationReplayPlan.Count -eq 24) `
            "The deterministic-runtime gate requires exactly 24 replay executions for configuration '$configuration' across two complete passes."
        foreach ($matrixPass in @(1, 2)) {
            $passReplayPlan = @($configurationReplayPlan | Where-Object {
                $_.matrixRepeat -eq $matrixPass
            })
            Assert-Condition ($passReplayPlan.Count -eq 12 -and
                @($passReplayPlan | Where-Object { $_.stress }).Count -eq 3 -and
                @($passReplayPlan | Where-Object { -not $_.stress }).Count -eq 9 -and
                @($passReplayPlan.determinismKey | Sort-Object -Unique).Count -eq 10) `
                "The deterministic-runtime gate requires one execution of each of nine non-stress replays and three executions of the one stress replay for configuration '$configuration' in matrix pass $matrixPass."
        }
    }
}
if ($ValidationSet -ne 'Replay') {
    $workerConfigurationCount = $workerConfigurations.Count
    $regularAiPlan = @($plan | Where-Object {
        $_.kind -ceq 'ai' -and $_.configuration -cne $collisionShadowConfiguration.Id
    })
    $expectedRegularAiCount = $workerConfigurationCount * $manifestData.ai.scenarios.Count *
        $manifestData.ai.seeds.Count * $manifestData.ai.repeats
    Assert-Condition ($regularAiPlan.Count -eq $expectedRegularAiCount) `
        "AI validation plan does not contain the complete $expectedRegularAiCount-result scenario/seed/configuration/repeat cross-product."
    $regularAiKeys = @($regularAiPlan | ForEach-Object {
        "$($_.scenario)|$($_.seed)|$($_.configuration)|$($_.repeat)"
    })
    Assert-Condition (@($regularAiKeys | Sort-Object -Unique).Count -eq $regularAiKeys.Count) `
        'AI validation plan contains a duplicate scenario/seed/configuration/repeat entry.'
}
$deterministicRuntimeEligible = $deterministicRuntimeContractRequested -and [bool]$EnforcePerformance
$planDocument = [pscustomobject]@{
    schemaVersion = 1
    gateName = 'deterministic-runtime'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    cohortNonce = $executionCohortNonce
    cohortCreatedUtc = $executionCohortCreatedUtc
    runtimeClosure = $hostRunnerRuntimeClosure
    runtimeRoot = $runtimeFull
    executable = $executableFull
    title = $manifestData.title
    capacityMode = $CapacityMode
    validationMode = if ($localCapacityRequested) { 'LocalCapacity' } else { 'Canonical' }
    executableSha256 = $manifestData.executableSha256
    executableSha256Source = $manifestData.executableSha256Source
    fixtureManifest = $manifestData.file
    validationSet = $ValidationSet
    replayCorpusRequired = $ValidationSet -ne 'AI'
    replayFixtureCount = @($manifestData.fixtures).Count
    replayMatrixRepeats = $ReplayMatrixRepeats
    stressRepeats = $StressRepeats
    x64Required = [bool]($RequireX64 -or $EnforcePerformance)
    performanceRequested = [bool]$EnforcePerformance
    performanceRequiredForDeterministicRuntimeGate = $true
    performanceMeasurementScope = 'aggregate-stage5-stress-replay-throughput'
    collisionSpecificReplayPerformanceClaim = $false
    diagnosticNonAcceptance = $diagnosticNonAcceptanceRequested
    directExecutionExceptionRequested = [bool]$AllowHeadlessDirectExecution
    frameTimingRequired = -not [bool]$DisableFrameTiming
    authoritativeWorkEvidenceRequired = $deterministicRuntimeEligible
    deterministicRuntimeEligible = $deterministicRuntimeEligible
    finalAcceptanceEligible = $false
    acceptanceReceiptRequested = $acceptanceBindingsRequested
    acceptanceReceiptEligible = $deterministicRuntimeEligible -and
        $acceptanceBindingsRequested
    acceptanceSourceCommit = if ($acceptanceBindingsRequested) {
        $AcceptanceSourceCommit
    } else { $null }
    acceptanceArtifactSetSha256 = if ($acceptanceBindingsRequested) {
        $AcceptanceArtifactSetSha256.ToUpperInvariant()
    } else { $null }
    taskRoot = $(if ($PlanOnly) { $null } else { $taskRootFull })
    launcherContract = $launcherEquivalence
    physicalCoreCount = $physicalCoreCount
    logicalProcessorCount = $logicalProcessorCount
    localCapacity = if ($localCapacityRequested) {
        [pscustomobject]@{
            hostTopology = $localCapacityTopology
            minimumPhysicalCoreCount = $localCapacityTopology.minimumPhysicalCoreCount
            maximumPhysicalCoreCount = $localCapacityTopology.maximumPhysicalCoreCount
            maximumLogicalProcessorCount = $localCapacityTopology.maximumLogicalProcessorCount
            regularConfigurations = @($workerConfigurations | ForEach-Object { $_.Id })
            shadowConfiguration = $collisionShadowConfiguration.Id
            externalAcceptanceEligible = $false
            canonicalFinalAcceptanceEligible = $false
        }
    } else { $null }
    corpusExportRequested = [bool]$corpusExportRequested
    corpusExportRoot = $corpusExportRootFull
    corpusExportMode = if ($corpusExportRequested) { 'local-capacity-ai' } else { $null }
    stage3PerformanceBaseline = $(if ($null -ne $stage3PerformanceBaseline) {
        [pscustomobject]@{
            file = $stage3PerformanceBaseline.file
            evidenceFile = $stage3PerformanceBaselineEvidencePath
            fileSha256 = $stage3PerformanceBaseline.fileSha256
            executableSha256 = $stage3PerformanceBaseline.executableSha256
            expectedExecutableSha256 = $ExpectedStage3ExecutableSha256.ToUpperInvariant()
            fixtureSha256 = $stage3PerformanceBaseline.fixtureSha256
            physicalCoreCount = $stage3PerformanceBaseline.physicalCoreCount
            availableCpus = $stage3PerformanceBaseline.availableCpus
            warmupRuns = $stage3PerformanceBaseline.warmupRuns
            rawWallMilliseconds = $stage3PerformanceBaseline.wallMilliseconds
            measuredMedianMilliseconds = $stage3PerformanceBaseline.measuredMedianMilliseconds
        }
    } else { $null })
    entries = $plan
}
$planPath = Join-Path $outputFull 'validation-plan.json'
[IO.File]::WriteAllText($planPath, ($planDocument | ConvertTo-Json -Depth 8))
if ($localCapacityRequested) {
    Write-LocalCapacityReceipt `
        -Path (Join-Path $outputFull 'local-capacity-plan-receipt.json') `
        -Status 'planned-non-acceptance' `
        -ValidationSet $ValidationSet `
        -Entries $plan `
        -Results @() `
        -Configurations $workerConfigurations `
        -ShadowConfiguration $collisionShadowConfiguration `
        -Topology $localCapacityTopology `
        -PlanPath $planPath `
        -ResultsPath (Join-Path $outputFull 'validation-results.json') | Out-Null
}
if ($EnforcePerformance -and $physicalCoreCount -lt 8) {
    $unsupportedPerformance = [pscustomobject]@{
        schemaVersion = 1
        status = 'unsupported-host-topology'
        measurementScope = 'aggregate-stage5-stress-replay-throughput'
        collisionSpecificSpeedupClaim = $false
        physicalCoreCount = $physicalCoreCount
        failures = @('Performance enforcement requires topology evidence for at least eight physical cores.')
    }
    [IO.File]::WriteAllText((Join-Path $outputFull 'performance-report.json'),
        ($unsupportedPerformance | ConvertTo-Json -Depth 4))
    throw 'Stage 5 performance validation is unsupported: fewer than eight physical cores were detected.'
}
if ($PlanOnly) {
    if ($deterministicRuntimeEligible -and $acceptanceBindingsRequested) {
        $planRawPath = Join-Path $outputFull 'validation-plan.raw.log'
        $planRawText = @(
            'STAGE5_HOST_RUNNER_PLAN_V2'
            "sourceCommit=$AcceptanceSourceCommit"
            "artifactSetSha256=$($AcceptanceArtifactSetSha256.ToUpperInvariant())"
            "cohortNonce=$executionCohortNonce"
            "cohortCreatedUtc=$executionCohortCreatedUtc"
            "runtimeClosureSha256=$($hostRunnerRuntimeClosure.closureSha256)"
            "title=$($manifestData.title)"
            "validationSet=$ValidationSet"
            "entryCount=$($plan.Count)"
            "planSha256=$(Get-Sha256 $planPath)"
        ) -join "`n"
        [IO.File]::WriteAllText($planRawPath, $planRawText + "`n")
        Write-Stage5HostRunnerReceipt -Role 'validation-plan' `
            -OutputRoot $outputFull `
            -ReceiptPath (Join-Path $outputFull 'validation-plan-receipt.json') `
            -Title $manifestData.title `
            -SourceCommit $AcceptanceSourceCommit `
            -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
            -ExecutableSha256 $manifestData.executableSha256 `
            -RawLogPaths @($planPath, $planRawPath) `
            -ChildRuns @() `
            -Details ([ordered]@{
                gateName = 'deterministic-runtime'
                validationSet = 'All'
                entryCount = $plan.Count
            }) `
            -CohortNonce $executionCohortNonce `
            -CohortCreatedUtc $executionCohortCreatedUtc `
            -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
    }
    if (-not $deterministicRuntimeEligible) {
        Write-Output "Stage 5 focused/diagnostic deterministic-runtime plan completed: $($plan.Count) synchronous installed-runtime executions."
    }
    else {
        Write-Output "Stage 5 deterministic-runtime plan passed: $($plan.Count) synchronous installed-runtime executions."
    }
    return
}

if ($Title -ceq 'Generals') {
    $generalsProfileLeaf = 'Command and Conquer Generals Data'
    if (-not [string]::IsNullOrWhiteSpace($ProfileLeafName)) {
        Assert-Condition ($ProfileLeafName -ceq $generalsProfileLeaf) `
            "Generals reads UserDataLeafName from INI; ProfileLeafName must be '$generalsProfileLeaf'."
    }
    $ProfileLeafName = $generalsProfileLeaf
}
elseif ([string]::IsNullOrWhiteSpace($ProfileLeafName)) {
    $ProfileLeafName = 'GGC-Stage5-Validation-{0}-{1}' -f ([DateTime]::UtcNow.ToString('yyyyMMddHHmmss')), $PID
}
Assert-Condition ($ProfileLeafName -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
    'ProfileLeafName must be a simple Documents leaf name.'

$taskRunRoot = [IO.Path]::GetFullPath((Join-Path $taskRootFull `
    ('validation-run-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))))
Assert-TaskOwnedPath $taskRunRoot $taskRootFull 'Validation run root' | Out-Null
Assert-Condition (-not (Test-Path -LiteralPath $taskRunRoot)) `
    'The generated task validation run root already exists.'
if ($corpusExportRequested) {
    Assert-Condition (-not (Test-PathWithin $taskRunRoot $corpusExportRootFull) -and
        -not (Test-PathWithin $corpusExportRootFull $taskRunRoot)) `
        'CorpusExportRoot must be a durable TaskRoot sibling of the ephemeral validation run root.'
}
$documentsRoot = Join-Path $taskRunRoot 'Documents'
$profileFull = [IO.Path]::GetFullPath((Join-Path $documentsRoot $ProfileLeafName))
Assert-TaskOwnedPath $documentsRoot $taskRootFull 'Validation Documents root' | Out-Null
Assert-TaskOwnedPath $profileFull $taskRootFull 'Validation profile' | Out-Null
$tempRoot = Join-Path $taskRunRoot 'Temp'
$tmpRoot = Join-Path $taskRunRoot 'Tmp'
$cacheRoot = Join-Path $taskRunRoot 'Cache'
$localAppDataRoot = Join-Path $taskRunRoot 'LocalAppData'
$appDataRoot = Join-Path $taskRunRoot 'AppData'
$taskRunPath = $taskRunRoot.Substring(2)
if (-not $taskRunPath.StartsWith('\')) { $taskRunPath = '\' + $taskRunPath }
$validationEnvironment = @{
    TEMP = $tempRoot
    TMP = $tmpRoot
    LOCALAPPDATA = $localAppDataRoot
    APPDATA = $appDataRoot
    USERPROFILE = $taskRunRoot
    HOMEDRIVE = 'H:'
    HOMEPATH = $taskRunPath
    RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileFull
    RTS_STAGE5_VALIDATION_CACHE_ROOT = $cacheRoot
}

$registrySnapshots = New-Object 'Collections.Generic.List[object]'
$results = New-Object 'Collections.Generic.List[object]'
$childRuns = New-Object 'Collections.Generic.List[object]'
$corpusExport = $null
$localCapacityReceipt = $null
$resultsPath = Join-Path $outputFull 'validation-results.json'
$fatalPattern = '(?i)(CRC Mismatch|game thread ownership violation|assertion failed|fatal error|missing map|replay read error|SKIRMISH_AI_TEST_FAIL|SIMULATION_JOB_SYSTEM_FALLBACK|SIMULATION_SHADOW_(?:MISMATCH|FAIL)|SIMULATION_COLLISION_MISMATCH)'
try {
    New-Item -ItemType Directory -Path $taskRunRoot -Force | Out-Null
    foreach ($directory in @($documentsRoot, $tempRoot, $tmpRoot, $cacheRoot,
            $localAppDataRoot, $appDataRoot, (Join-Path $profileFull 'Replays\Stage5Validation'))) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

foreach ($fixture in $manifestData.fixtures) {
    $replayDestination = Join-Path $profileFull "Replays\$($fixture.replayArgument)"
    Copy-Item -LiteralPath $fixture.source -Destination $replayDestination
    Assert-FileHash $replayDestination $fixture.sha256 `
        "Staged replay '$($fixture.replayArgument)'" | Out-Null
    foreach ($map in $fixture.maps) {
        $mapDestination = Join-Path $profileFull $map.relative
        $mapDirectory = Split-Path -Parent $mapDestination
        if (-not (Test-Path -LiteralPath $mapDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $mapDirectory -Force | Out-Null
        }
        if (Test-Path -LiteralPath $mapDestination) {
            Assert-FileHash $mapDestination $map.sha256 "Shared staged map '$($map.relative)'" | Out-Null
        }
        else {
            Copy-Item -LiteralPath $map.source -Destination $mapDestination
        }
        Assert-FileHash $mapDestination $map.sha256 `
            "Staged map '$($map.relative)'" | Out-Null
    }
}

# Rehash every staged destination as one complete corpus immediately before any
# registry mutation or installed-runtime execution. This catches overwrite,
# case-alias, and copy corruption independently of the source-file preflight.
foreach ($fixture in $manifestData.fixtures) {
    Assert-FileHash (Join-Path $profileFull "Replays\$($fixture.replayArgument)") `
        $fixture.sha256 "Execution-ready replay '$($fixture.replayArgument)'" | Out-Null
    foreach ($map in $fixture.maps) {
        Assert-FileHash (Join-Path $profileFull $map.relative) $map.sha256 `
            "Execution-ready map '$($map.relative)'" | Out-Null
    }
}

if ([string]::IsNullOrWhiteSpace($GeneralsInstallRoot)) {
    $GeneralsInstallRoot = $runtimeFull
}
$generalsInstallFull = [IO.Path]::GetFullPath($GeneralsInstallRoot)
Assert-Condition (Test-Path -LiteralPath $generalsInstallFull -PathType Container) `
    "GeneralsInstallRoot was not found: $generalsInstallFull"

    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        # Both title variants call SHGetKnownFolderPath(FOLDERID_Documents),
        # but Generals reads its leaf from INI while Zero Hour reads the leaf
        # from the title registry key. Redirect the known folder itself so no
        # validation profile is created below the user's live C: Documents.
        Set-PreservedRegistryValue $view `
            'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' `
            'Personal' $documentsRoot $registrySnapshots
        Set-PreservedRegistryValue $view `
            'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders' `
            'Personal' $documentsRoot $registrySnapshots
        if ($Title -ceq 'Generals') {
            Set-PreservedRegistryValue $view `
                'Software\Electronic Arts\EA Games\Generals' 'InstallPath' `
                ($generalsInstallFull + '\') $registrySnapshots
        }
        else {
            Set-PreservedRegistryValue $view `
                'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour' `
                'InstallPath' ($runtimeFull + '\') $registrySnapshots
            Set-PreservedRegistryValue $view `
                'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour' `
                'UserDataLeafName' $ProfileLeafName $registrySnapshots
        }
    }

    $launcherEquivalence = Assert-LauncherEquivalenceContract $launcherContract `
        $executableFull $runtimeFull $plan $ProfileLeafName $documentsRoot
    $planDocument.launcherContract = $launcherEquivalence
    [IO.File]::WriteAllText($planPath, ($planDocument | ConvertTo-Json -Depth 8))

    if ($deterministicRuntimeEligible -and $acceptanceBindingsRequested) {
        $planRawPath = Join-Path $outputFull 'validation-plan.raw.log'
        $planRawText = @(
            'STAGE5_HOST_RUNNER_PLAN_V2'
            "sourceCommit=$AcceptanceSourceCommit"
            "artifactSetSha256=$($AcceptanceArtifactSetSha256.ToUpperInvariant())"
            "cohortNonce=$executionCohortNonce"
            "cohortCreatedUtc=$executionCohortCreatedUtc"
            "runtimeClosureSha256=$($hostRunnerRuntimeClosure.closureSha256)"
            "title=$($manifestData.title)"
            "validationSet=$ValidationSet"
            "entryCount=$($plan.Count)"
            "planSha256=$(Get-Sha256 $planPath)"
        ) -join "`n"
        [IO.File]::WriteAllText($planRawPath, $planRawText + "`n")
        Write-Stage5HostRunnerReceipt -Role 'validation-plan' `
            -OutputRoot $outputFull `
            -ReceiptPath (Join-Path $outputFull 'validation-plan-receipt.json') `
            -Title $manifestData.title `
            -SourceCommit $AcceptanceSourceCommit `
            -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
            -ExecutableSha256 $manifestData.executableSha256 `
            -RawLogPaths @($planPath, $planRawPath) `
            -ChildRuns @() `
            -Details ([ordered]@{
                gateName = 'deterministic-runtime'
                validationSet = 'All'
                entryCount = $plan.Count
            }) `
            -CohortNonce $executionCohortNonce `
            -CohortCreatedUtc $executionCohortCreatedUtc `
            -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
    }

    foreach ($entry in $plan) {
        Assert-FreeSpace $outputFull $MinimumFreeBytes 'Validation evidence volume'
        Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable before run' | Out-Null
        $run = Invoke-ValidationProcess $executableFull $runtimeFull $entry `
            (-not $DisableFrameTiming) $validationEnvironment $outputFull `
            -NativeObservationBinding $nativeObservationBinding
        $childRuns.Add([pscustomobject]@{
            entry = $entry
            run = $run
            childProcess = $run.childProcess
        }) | Out-Null
        $combined = $run.stdout + "`n" + $run.stderr + "`n" + $run.runtimeLogText
        Assert-Condition (-not $run.timedOut) "Validation entry $($entry.sequence) timed out."
        Assert-Condition ($run.exitCode -eq 0) "Validation entry $($entry.sequence) exited with code $($run.exitCode)."
        Assert-Condition ($combined -notmatch $fatalPattern) `
            "Validation entry $($entry.sequence) reported a fatal runtime marker."
        $aiEvidence = $null
        $replayMetrics = $null
        $replayResult = $null
        $timingEvidence = if ($DisableFrameTiming) {
            [pscustomobject]@{ status = 'disabled-non-acceptance-diagnostic' }
        }
        else {
            Get-Stage5TimingEvidence $entry.timingDirectory "Validation entry $($entry.sequence)"
        }
        if ($entry.kind -ceq 'ai') {
            $aiEvidence = ConvertFrom-Stage5AiCompletion $run.stdout $entry `
                $manifestData.executableSha256 $deterministicRuntimeEligible
            if (-not $DisableFrameTiming) {
                Assert-Condition ($timingEvidence.maximumFrameEnd -eq $aiEvidence.endFrame) `
                    "AI validation entry $($entry.sequence) timing final frame does not match the completion manifest."
                Assert-Stage5CollisionTimingEvidence $timingEvidence $aiEvidence `
                    "AI validation entry $($entry.sequence)"
            }
        }
        else {
            $replayMetrics = ConvertFrom-Stage5ReplayMetrics $run.stdout $entry
            $replayResult = ConvertFrom-Stage5ReplayResult $run.stdout $entry
            if (-not $DisableFrameTiming) {
                Assert-Condition ($timingEvidence.maximumFrameEnd -eq $replayResult.finalFrame) `
                    "Replay validation entry $($entry.sequence) timing final frame does not match SIMULATION_REPLAY_RESULT."
                Assert-Stage5CollisionTimingEvidence $timingEvidence $replayMetrics `
                    "Replay validation entry $($entry.sequence)"
            }
        }
        $results.Add([pscustomobject]@{
            sequence = $entry.sequence
            title = $manifestData.title
            kind = $entry.kind
            caseId = $entry.caseId
            determinismKey = $entry.determinismKey
            configuration = $entry.configuration
            simulationMode = $entry.simulationMode
            requestedWorkers = $entry.requestedWorkers
            workerPolicy = $entry.workerPolicy
            repeat = $entry.repeat
            matrixRepeat = $entry.matrixRepeat
            replayArgument = $entry.replayArgument
            seed = $entry.seed
            scenario = $entry.scenario
            fixtureSha256 = $entry.fixtureSha256
            stress = $entry.stress
            exitCode = $run.exitCode
            timedOut = $run.timedOut
            wallMilliseconds = $run.wallMilliseconds
            stdoutSha256 = Get-Sha256 $entry.stdout
            stderrSha256 = Get-Sha256 $entry.stderr
            aiEvidence = $aiEvidence
            replayMetrics = $replayMetrics
            replayResult = $replayResult
            timingEvidence = $timingEvidence
        }) | Out-Null
        [IO.File]::WriteAllText($resultsPath, ($results.ToArray() | ConvertTo-Json -Depth 5))
    }
    $workerConfigurationIds = @($workerConfigurations | ForEach-Object { $_.Id })
    $shadowConfigurationId = if ($manifestData.ai.scenarios -ccontains '4v2') {
        [string]$collisionShadowConfiguration.Id
    } else { '' }
    if ($ValidationSet -ne 'Replay') {
        $expectedAiDeterminismKeys = @(
            foreach ($scenario in $manifestData.ai.scenarios) {
                foreach ($seed in $manifestData.ai.seeds) {
                    "$scenario-seed-$seed"
                }
            }
        )
        Assert-Stage5AiDeterminism $results.ToArray() $workerConfigurationIds `
            $manifestData.ai.repeats $shadowConfigurationId $expectedAiDeterminismKeys
    }
    if ($deterministicRuntimeEligible) {
        Assert-Stage5AuthoritativeWorkEvidence $results.ToArray()
    }
    if ($ValidationSet -ne 'AI') {
        Assert-Stage5ReplayDeterminism $results.ToArray()
    }
    if ($EnforcePerformance) {
        $performanceReport = Measure-Stage5Performance $results.ToArray() $physicalCoreCount `
            $stage3PerformanceBaseline 1 3
        # Keep the performance evidence title-specific when the same reusable
        # runner serves both Generals and Zero Hour lanes.
        $performanceReport | Add-Member -NotePropertyName title `
            -NotePropertyValue $manifestData.title -Force
        $performanceReportPath = Join-Path $outputFull 'performance-report.json'
        [IO.File]::WriteAllText($performanceReportPath, ($performanceReport | ConvertTo-Json -Depth 8))
        Assert-Condition ($performanceReport.status -ceq 'passed') `
            "Stage 5 performance validation status is '$($performanceReport.status)': $($performanceReport.failures -join ' ')"
    }
    Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable after matrix' | Out-Null

    if ($corpusExportRequested) {
        $corpusExport = Export-LocalCapacityAiCorpus `
            -TaskRoot $taskRootFull -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileFull -CorpusExportRoot $corpusExportRootFull `
            -Title $manifestData.title -ExecutableSha256 $manifestData.executableSha256 `
            -ChildRuns $childRuns.ToArray() -Results $results.ToArray() `
            -ValidationResultsPath $resultsPath
    }

    if ($localCapacityRequested) {
        $localCapacityReceipt = Write-LocalCapacityReceipt `
            -Path (Join-Path $outputFull 'local-capacity-receipt.json') `
            -Status 'passed-non-acceptance' `
            -ValidationSet $ValidationSet `
            -Entries $plan `
            -Results $results.ToArray() `
            -Configurations $workerConfigurations `
            -ShadowConfiguration $collisionShadowConfiguration `
            -Topology $localCapacityTopology `
            -PlanPath $planPath `
            -ResultsPath $resultsPath `
            -CorpusExportRoot $corpusExportRootFull `
            -CorpusExport $corpusExport
    }

    if ($corpusExportRequested) {
        Assert-Condition ($null -ne $localCapacityReceipt -and
            (Test-Path -LiteralPath $localCapacityReceipt.path -PathType Leaf)) `
            'LocalCapacity corpus export requires the final receipt before manifest emission.'
        $corpusManifest = Write-Stage5FreshReplayCorpusManifest `
            -TaskRoot $taskRootFull -CorpusExportRoot $corpusExportRootFull `
            -Title $manifestData.title -ExecutableSha256 $manifestData.executableSha256 `
            -Records $corpusExport.records -ValidationResultsPath $resultsPath `
            -ValidationReceiptPath $localCapacityReceipt.path
        $corpusExport | Add-Member -NotePropertyName manifest `
            -NotePropertyValue $corpusManifest -Force
    }

    if ($deterministicRuntimeEligible -and $acceptanceBindingsRequested) {
        $resultArray = @($results.ToArray())
        $allChildStreamPaths = @($childRuns.ToArray() | ForEach-Object {
            $_.entry.stdout
            $_.entry.stderr
        })
        $replayRuns = @($childRuns.ToArray() | Where-Object {
            $_.entry.kind -ceq 'replay'
        })
        $aiRuns = @($childRuns.ToArray() | Where-Object {
            $_.entry.kind -ceq 'ai'
        })
        $replayResults = @($resultArray | Where-Object { $_.kind -ceq 'replay' })
        $aiResults = @($resultArray | Where-Object { $_.kind -ceq 'ai' })
        Write-Stage5HostRunnerReceipt -Role 'validation-results' `
            -OutputRoot $outputFull `
            -ReceiptPath (Join-Path $outputFull 'validation-results-receipt.json') `
            -Title $manifestData.title `
            -SourceCommit $AcceptanceSourceCommit `
            -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
            -ExecutableSha256 $manifestData.executableSha256 `
            -RawLogPaths (@($resultsPath) + $allChildStreamPaths) `
            -ChildRuns $childRuns.ToArray() `
            -Details ([ordered]@{
                resultCount = $resultArray.Count
                allExecutionsPassed = $true
                resultsSha256 = Get-Sha256 $resultsPath
            }) `
            -CohortNonce $executionCohortNonce `
            -CohortCreatedUtc $executionCohortCreatedUtc `
            -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
        Write-Stage5HostRunnerReceipt -Role 'replay-results' `
            -OutputRoot $outputFull `
            -ReceiptPath (Join-Path $outputFull 'replay-results-receipt.json') `
            -Title $manifestData.title `
            -SourceCommit $AcceptanceSourceCommit `
            -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
            -ExecutableSha256 $manifestData.executableSha256 `
            -RawLogPaths (@($resultsPath) + $allChildStreamPaths) `
            -ChildRuns $replayRuns `
            -Details ([ordered]@{
                uniqueReplayCount = @($replayResults.determinismKey | Sort-Object -Unique).Count
                executionCount = $replayResults.Count
                crcTreeSha256 = Get-Stage5ResultTreeSha256 $resultArray 'replay'
                allExecutionsPassed = $true
            }) `
            -CohortNonce $executionCohortNonce `
            -CohortCreatedUtc $executionCohortCreatedUtc `
            -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
        Write-Stage5HostRunnerReceipt -Role 'ai-results' `
            -OutputRoot $outputFull `
            -ReceiptPath (Join-Path $outputFull 'ai-results-receipt.json') `
            -Title $manifestData.title `
            -SourceCommit $AcceptanceSourceCommit `
            -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
            -ExecutableSha256 $manifestData.executableSha256 `
            -RawLogPaths (@($resultsPath) + $allChildStreamPaths) `
            -ChildRuns $aiRuns `
            -Details ([ordered]@{
                scenarioCount = @($aiResults.scenario | Sort-Object -Unique).Count
                distinctSeedCount = @($aiResults.seed | Sort-Object -Unique).Count
                repeatCount = $manifestData.ai.repeats
                allGamesCompleted = $true
                digestTreeSha256 = Get-Stage5ResultTreeSha256 $resultArray 'ai'
            }) `
            -CohortNonce $executionCohortNonce `
            -CohortCreatedUtc $executionCohortCreatedUtc `
            -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
        if ($EnforcePerformance) {
            $performanceReportPath = Join-Path $outputFull 'performance-report.json'
            Write-Stage5HostRunnerReceipt -Role 'performance-report' `
                -OutputRoot $outputFull `
                -ReceiptPath (Join-Path $outputFull 'performance-report-receipt.json') `
                -Title $manifestData.title `
                -SourceCommit $AcceptanceSourceCommit `
                -ArtifactSetSha256 $AcceptanceArtifactSetSha256 `
                -ExecutableSha256 $manifestData.executableSha256 `
                -RawLogPaths (@($performanceReportPath, $resultsPath) + $allChildStreamPaths) `
                -ChildRuns $replayRuns `
                -Details ([ordered]@{}) `
                -CohortNonce $executionCohortNonce `
                -CohortCreatedUtc $executionCohortCreatedUtc `
                -RuntimeClosure $hostRunnerRuntimeClosure | Out-Null
        }
    }
}
finally {
    $cleanupErrors = New-Object 'Collections.Generic.List[string]'
    try {
        Invoke-Stage5RegistryRestore -Snapshots @($registrySnapshots.ToArray()) -RestoreAction {
            param($snapshot)
            Restore-RegistryValue $snapshot
        }
    }
    catch {
        $cleanupErrors.Add("registry restoration: $($_.Exception.Message)") | Out-Null
    }
    try {
        Remove-TaskOwnedDirectory $taskRunRoot $taskRootFull 'Validation task scratch root'
    }
    catch {
        $cleanupErrors.Add("task scratch cleanup: $($_.Exception.Message)") | Out-Null
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "Stage 5 validation cleanup failed after attempting every cleanup action: $($cleanupErrors -join ' | ')"
    }
}

if (-not $deterministicRuntimeEligible) {
    Write-Output "Stage 5 focused/diagnostic deterministic-runtime validation completed: $($results.Count) synchronous installed-runtime executions."
}
else {
    Write-Output "Stage 5 deterministic-runtime gate passed: $($results.Count) synchronous installed-runtime executions. Final acceptance remains a separate evidence aggregation."
}
