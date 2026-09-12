$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$script:Stage5InstalledKernelBaseCommands = @(
    'Assert-Stage5FinalAcceptanceNoReparsePath',
    'Assert-Stage5FinalAcceptancePathContained',
    'Assert-Stage5FinalAcceptanceSnapshotSha256',
    'ConvertFrom-Stage5FinalAcceptanceJsonSnapshot',
    'Get-Stage5FinalAcceptanceFileSnapshot',
    'Test-Stage5JsonInteger',
    'Write-Stage5FinalAcceptanceFileAtomically')
$script:Stage5InstalledKernelEvidenceModulePath = Join-Path $PSScriptRoot `
    'DeterministicSimulationEvidence.psm1'
# Keep both dependencies in this module's private scope. A kernel module can
# be imported before its caller later reloads the base evidence module with
# -Force; ambient command visibility would then leave nested reader functions
# bound to a module scope whose snapshot helpers have been removed.
$script:Stage5InstalledKernelEvidenceModule = Import-Module `
    $script:Stage5InstalledKernelEvidenceModulePath -Scope Local `
    -PassThru -ErrorAction Stop
$missingBaseCommands = @($script:Stage5InstalledKernelBaseCommands |
    Where-Object { $null -eq (Get-Command -Name $_ -CommandType Function `
        -ErrorAction SilentlyContinue) })
if ($missingBaseCommands.Count -gt 0) {
    throw ('Installed-kernel evidence is missing required base commands: ' +
        ($missingBaseCommands -join ', ') + '.')
}

$nativeReaderName = 'Read-Stage5NativePerformanceFixtureProductionReceipt'
$script:Stage5InstalledKernelNativeModulePath = Join-Path $PSScriptRoot `
    'Stage5NativePerformanceFixtureProduction.psm1'
$script:Stage5InstalledKernelNativeModule = Import-Module `
    $script:Stage5InstalledKernelNativeModulePath -Scope Local `
    -PassThru -ErrorAction Stop
if ($null -eq (Get-Command -Name $nativeReaderName -CommandType Function `
        -ErrorAction SilentlyContinue)) {
    throw "Installed-kernel evidence is missing required native reader '$nativeReaderName'."
}

$script:Stage5InstalledKernelNames = @(
    'physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
$script:Stage5InstalledKernelUuidPattern =
    '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
$script:Stage5InstalledKernelUtcPattern =
    '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$'

function Assert-Stage5InstalledKernelCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Stage5InstalledKernelExactProperties {
    param([object]$Value, [string[]]$Names, [string]$Context)
    Assert-Stage5InstalledKernelCondition ($null -ne $Value -and
        $Value -isnot [Array]) "$Context must be one object."
    $actual = @($Value.PSObject.Properties.Name)
    $missing = @($Names | Where-Object { $actual -cnotcontains $_ })
    $extra = @($actual | Where-Object { $Names -cnotcontains $_ })
    Assert-Stage5InstalledKernelCondition ($missing.Count -eq 0 -and
        $extra.Count -eq 0) ("$Context has a non-canonical shape. Missing: " +
        "[$($missing -join ', ')]; unexpected: [$($extra -join ', ')].")
}

function Assert-Stage5InstalledKernelRawString {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelCondition ($Value -is [string]) `
        "$Context must be a JSON string."
}

function Assert-Stage5InstalledKernelRawInteger {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelCondition (Test-Stage5JsonInteger $Value) `
        "$Context must be an exact JSON integer."
}

function Assert-Stage5InstalledKernelRawBoolean {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelCondition ($Value -is [bool]) `
        "$Context must be a JSON boolean."
}

function Assert-Stage5InstalledKernelRawArray {
    param([object]$Value, [string]$Context, [int]$Minimum = 0,
        [int]$Maximum = [int]::MaxValue)
    Assert-Stage5InstalledKernelCondition ($Value -is [Array] -and
        $Value.Count -ge $Minimum -and $Value.Count -le $Maximum) `
        "$Context must be a JSON array with $Minimum to $Maximum elements."
}

function Assert-Stage5InstalledKernelRawIntegerJsonLiterals {
    param([object]$Snapshot, [string]$Context)
    # Windows PowerShell 5.1's JavaScriptSerializer can coerce a JSON decimal
    # such as 1.0 to an integer before the typed projection sees it.  Inspect
    # the original bytes so integer fields remain fail-closed on both hosts.
    Assert-Stage5InstalledKernelCondition ($null -ne $Snapshot -and
        $null -ne $Snapshot.PSObject.Properties['bytes'] -and
        $null -ne $Snapshot.PSObject.Properties['sha256'] -and
        $null -ne $Snapshot.PSObject.Properties['length']) `
        "$Context snapshot is missing raw bytes."
    $json = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    $integerProperties = @(
        'schemaVersion', 'processId', 'processCreationTimeUtc100ns',
        'creationTimeUtc100ns', 'exitCode', 'seed', 'requestedPlayerCount',
        'requestedMinimumUnitCount', 'playerCount', 'initialUnitCount',
        'minimumUnitCount', 'peakUnitCount', 'requestedCount', 'effectiveCount',
        'selectedWorkerPhysicalCoreCount', 'selectedWorkerPhysicalCoreMask',
        'submittedJobCount', 'completedJobCount', 'failedJobCount',
        'cancelledJobCount', 'maxConcurrentJobs', 'observedWorkerCount',
        'observedPhysicalCoreCount', 'observedPhysicalCoreMask', 'frameCount',
        'finalCrc', 'subtype', 'sampleCount', 'totalNanoseconds',
        'minimumNanoseconds', 'maximumNanoseconds', 'workerCount', 'ordinal',
        'measuredRuns', 'warmupRuns', 'warmupCount', 'byteCount',
        'minimumObservedUnitCount', 'maximumObservedUnitCount',
        'requestedWorkers', 'effectiveWorkers', 'selectedWorkerCount',
        'physicalCoreCount', 'frameBudget', 'replayEpoch', 'winnerTeam')
    foreach ($property in $integerProperties) {
        $escaped = [regex]::Escape($property)
        $decimalPattern = '"' + $escaped +
            '\s*:\s*(-?(?:\d+\.\d*|\d+[eE][+-]?\d+))(?=\s*[,}])'
        Assert-Stage5InstalledKernelCondition (-not [regex]::IsMatch(
            $json, $decimalPattern)) `
            "$Context.$property must use an integer JSON literal."
    }
}

function Assert-Stage5InstalledKernelSha256 {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelRawString $Value $Context
    Assert-Stage5InstalledKernelCondition ($Value -cmatch '^[0-9A-F]{64}$') `
        "$Context must be an uppercase SHA-256."
}

function Assert-Stage5InstalledKernelSourceCommit {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelRawString $Value $Context
    Assert-Stage5InstalledKernelCondition ($Value -cmatch '^[0-9a-f]{40}$') `
        "$Context must be a lowercase 40-hex commit."
}

function Assert-Stage5InstalledKernelCanonicalUtc {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelRawString $Value $Context
    [DateTime]$parsed = [DateTime]::MinValue
    $valid = $Value -cmatch $script:Stage5InstalledKernelUtcPattern -and
        [DateTime]::TryParseExact($Value, 'o',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind, [ref]$parsed) -and
        $parsed.Kind -eq [DateTimeKind]::Utc -and
        $parsed.ToString('o',
            [Globalization.CultureInfo]::InvariantCulture) -ceq $Value
    Assert-Stage5InstalledKernelCondition $valid `
        "$Context must be a canonical UTC round-trip timestamp."
    return $parsed
}

function ConvertFrom-Stage5InstalledKernelJsonSnapshot {
    param([object]$Snapshot, [string]$Context)
    Assert-Stage5InstalledKernelRawIntegerJsonLiterals $Snapshot $Context
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot $Context `
        -AsPsObject
    return ConvertTo-Stage5InstalledKernelPsObject $document
}

function ConvertTo-Stage5InstalledKernelPsObject {
    param([object]$Value)
    if ($null -eq $Value -or $Value -is [string] -or
        $Value.GetType().IsPrimitive -or $Value -is [DateTime]) {
        return $Value
    }
    if ($Value -is [Array]) {
        $items = @()
        foreach ($item in $Value) {
            $items += ConvertTo-Stage5InstalledKernelPsObject $item
        }
        return ,$items
    }
    if ($Value -is [Collections.IDictionary]) {
        $properties = [ordered]@{}
        foreach ($key in $Value.Keys) {
            $properties[[string]$key] =
                ConvertTo-Stage5InstalledKernelPsObject $Value[$key]
        }
        return [pscustomobject]$properties
    }
    $propertyNames = @($Value.PSObject.Properties |
        ForEach-Object { [string]$_.Name })
    if ($propertyNames.Count -eq 2 -and
        $propertyNames -contains 'value' -and
        $propertyNames -contains 'Count' -and
        $Value.value -is [Array]) {
        $items = @()
        foreach ($item in $Value.value) {
            $items += ConvertTo-Stage5InstalledKernelPsObject $item
        }
        return ,$items
    }
    $properties = [ordered]@{}
    foreach ($propertyName in $propertyNames) {
        $properties[$propertyName] =
            ConvertTo-Stage5InstalledKernelPsObject `
                $Value.PSObject.Properties[$propertyName].Value
    }
    return [pscustomobject]$properties
}

function Resolve-Stage5InstalledKernelRelativeFile {
    param([string]$Root, [object]$RelativePath, [string]$Context)
    Assert-Stage5InstalledKernelRawString $RelativePath "$Context path"
    Assert-Stage5InstalledKernelCondition (
        -not [string]::IsNullOrWhiteSpace($RelativePath) -and
        -not [IO.Path]::IsPathRooted($RelativePath) -and
        $RelativePath -notmatch ':' -and
        $RelativePath -notmatch '(^|[\\/])\.\.?([\\/]|$)') `
        "$Context must be a safe relative path."
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    Assert-Stage5FinalAcceptancePathContained $base $full $Context
    Assert-Stage5InstalledKernelCondition (Test-Path -LiteralPath $full `
        -PathType Leaf) "$Context was not found: $RelativePath"
    Assert-Stage5FinalAcceptanceNoReparsePath $base $full $Context
    return $full
}

function Read-Stage5InstalledKernelBoundFile {
    param([string]$Root, [object]$Binding, [string]$Context,
        [ValidateSet('JsonReceipt', 'RawLog', 'Replay', 'RuntimeBinary')]
        [string]$EvidenceKind = 'JsonReceipt')
    Assert-Stage5InstalledKernelExactProperties $Binding @('path', 'sha256') `
        "$Context binding"
    Assert-Stage5InstalledKernelRawString $Binding.path "$Context path"
    Assert-Stage5InstalledKernelSha256 $Binding.sha256 `
        "$Context SHA-256"
    $path = Resolve-Stage5InstalledKernelRelativeFile $Root `
        $Binding.path $Context
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path $Context `
        -EvidenceKind $EvidenceKind
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        ([string]$Binding.sha256) $Context | Out-Null
    return [pscustomobject]@{ path = $path; snapshot = $snapshot }
}

function Get-Stage5InstalledKernelRelativePath {
    param([string]$Root, [object]$Path, [string]$Context)
    Assert-Stage5InstalledKernelRawString $Path "$Context path"
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5FinalAcceptancePathContained $base $full $Context
    $relative = $full.Substring($base.Length).TrimStart('\', '/')
    Assert-Stage5InstalledKernelCondition (-not
        [string]::IsNullOrWhiteSpace($relative)) "$Context cannot be the root."
    return $relative.Replace('\', '/')
}

function Write-Stage5InstalledKernelSnapshot {
    param([string]$Root, [string]$RelativePath, [object]$Snapshot,
        [string]$Context,
        [ValidateSet('JsonReceipt', 'RawLog', 'Replay', 'RuntimeBinary')]
        [string]$EvidenceKind = 'JsonReceipt')
    $destination = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    $parent = Split-Path -Parent $destination
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Assert-Stage5FinalAcceptanceNoReparsePath $Root $parent "$Context parent"
    # The atomic publisher distinguishes JSON, replay, and opaque byte limits;
    # maps use the opaque RawLog write boundary and the RuntimeBinary rereader.
    $writeKind = if ($EvidenceKind -ceq 'RuntimeBinary') {
        'RawLog'
    } else { $EvidenceKind }
    Write-Stage5FinalAcceptanceFileAtomically -Path $destination `
        -Bytes ([byte[]]$Snapshot.bytes) -Context $Context `
        -EvidenceKind $writeKind | Out-Null
    $written = Get-Stage5FinalAcceptanceFileSnapshot $destination $Context `
        -EvidenceKind $EvidenceKind
    Assert-Stage5FinalAcceptanceSnapshotSha256 $written `
        ([string]$Snapshot.sha256) $Context | Out-Null
    return [pscustomobject][ordered]@{
        path = $RelativePath.Replace('\', '/')
        sha256 = [string]$written.sha256
    }
}

function Get-Stage5InstalledKernelMaskHex {
    param([object]$Value, [string]$Context)
    Assert-Stage5InstalledKernelCondition (Test-Stage5JsonInteger $Value) `
        "$Context must be an exact JSON integer."
    [UInt64]$mask = 0
    Assert-Stage5InstalledKernelCondition ([UInt64]::TryParse(
        [Convert]::ToString($Value,
            [Globalization.CultureInfo]::InvariantCulture),
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$mask)) `
        "$Context is outside the unsigned 64-bit range."
    return $mask.ToString('X16')
}

function Get-Stage5InstalledKernelStreamKey {
    param([object]$Stream)
    Assert-Stage5InstalledKernelRawString $Stream.name `
        'Installed-kernel timing stream name'
    Assert-Stage5InstalledKernelRawInteger $Stream.subtype `
        'Installed-kernel timing stream subtype'
    return ('{0}:{1}' -f $Stream.name, [int]$Stream.subtype)
}

function ConvertTo-Stage5InstalledKernelRunProjection {
    param(
        [object]$ReceiptSnapshot,
        [object]$RawSnapshot,
        [object]$TimingSnapshot,
        [object]$AttemptStartSnapshot,
        [object]$AttemptResultSnapshot,
        [object]$RunBinding,
        [object]$Expected,
        [hashtable]$OutputBindings
    )
    $receipt = ConvertFrom-Stage5InstalledKernelJsonSnapshot $ReceiptSnapshot `
        'Installed-kernel native receipt'
    Assert-Stage5InstalledKernelCondition (
        $ReceiptSnapshot.sha256 -ceq [string]$RunBinding.receiptSha256 -and
        $RawSnapshot.sha256 -ceq [string]$RunBinding.host.rawLogSha256 -and
        $TimingSnapshot.sha256 -ceq [string]$RunBinding.host.timingSha256) `
        'Installed-kernel run files differ from their host-observed hashes.'
    $isV6 = (Test-Stage5JsonInteger $receipt.schemaVersion) -and
        $receipt.schemaVersion -eq 6
    $receiptFields = @('schemaVersion', 'producer', 'evidenceKind', 'status',
        'role', 'producerVersion', 'title', 'runId', 'runNonce', 'cohortNonce',
        'cohortCreatedUtc', 'recordedUtc', 'architecture', 'sourceCommit',
        'artifactSetSha256', 'runtimeClosure', 'executablePath',
        'executableSha256', 'commandLine', 'process', 'fixture', 'workload',
        'frameSimulation', 'frames', 'worker', 'topology', 'rawEvidence',
        'rawLogs', 'provenance', 'schedulerMetrics', 'phases', 'kernels',
        'kernelTiming', 'measurementRole', 'kernelReference', 'simulationMode',
        'schedulerStarted')
    if ($isV6) { $receiptFields += @('phaseAccounting', 'attemptTrace') }
    Assert-Stage5InstalledKernelExactProperties $receipt $receiptFields `
        'Installed-kernel native receipt'
    Assert-Stage5InstalledKernelRawInteger $receipt.schemaVersion `
        'Installed-kernel receipt schemaVersion'
    foreach ($name in @('producer', 'evidenceKind', 'status', 'role',
            'producerVersion', 'title', 'sourceCommit', 'artifactSetSha256',
            'executableSha256', 'cohortNonce', 'cohortCreatedUtc', 'runId',
            'runNonce', 'measurementRole', 'simulationMode')) {
        Assert-Stage5InstalledKernelRawString $receipt.$name `
            "Installed-kernel receipt $name"
    }
    Assert-Stage5InstalledKernelRawString `
        $receipt.runtimeClosure.dependencyManifestSha256 `
        'Installed-kernel receipt dependency-manifest SHA-256'
    Assert-Stage5InstalledKernelRawString $receipt.runtimeClosure.closureSha256 `
        'Installed-kernel receipt runtime-closure SHA-256'
    Assert-Stage5InstalledKernelRawBoolean $receipt.schedulerStarted `
        'Installed-kernel receipt schedulerStarted'
    Assert-Stage5InstalledKernelRawInteger $receipt.process.id `
        'Installed-kernel receipt process id'
    Assert-Stage5InstalledKernelRawInteger $receipt.process.creationTimeUtc100ns `
        'Installed-kernel receipt process creationTimeUtc100ns'
    Assert-Stage5InstalledKernelRawBoolean $receipt.process.identityAvailable `
        'Installed-kernel receipt process identityAvailable'
    Assert-Stage5InstalledKernelRawBoolean $receipt.process.exitCodeKnown `
        'Installed-kernel receipt process exitCodeKnown'
    Assert-Stage5InstalledKernelRawInteger $receipt.process.exitCode `
        'Installed-kernel receipt process exitCode'
    foreach ($name in @('id', 'kind', 'contentSha256')) {
        Assert-Stage5InstalledKernelRawString $receipt.fixture.$name `
            "Installed-kernel receipt fixture $name"
    }
    foreach ($name in @('seed', 'requestedPlayerCount',
            'requestedMinimumUnitCount')) {
        Assert-Stage5InstalledKernelRawInteger $receipt.fixture.$name `
            "Installed-kernel receipt fixture $name"
    }
    Assert-Stage5InstalledKernelRawString $receipt.workload.sampling `
        'Installed-kernel receipt workload sampling'
    foreach ($name in @('playerCount', 'initialUnitCount',
            'minimumUnitCount', 'peakUnitCount')) {
        Assert-Stage5InstalledKernelRawInteger $receipt.workload.$name `
            "Installed-kernel receipt workload $name"
    }
    foreach ($name in @('rosterStable', 'contiguous')) {
        Assert-Stage5InstalledKernelRawBoolean $receipt.workload.$name `
            "Installed-kernel receipt workload $name"
    }
    foreach ($name in @('requestedCount', 'effectiveCount',
            'selectedWorkerPhysicalCoreCount', 'selectedWorkerPhysicalCoreMask')) {
        Assert-Stage5InstalledKernelRawInteger $receipt.worker.$name `
            "Installed-kernel receipt worker $name"
    }
    Assert-Stage5InstalledKernelRawString $receipt.worker.policy `
        'Installed-kernel receipt worker policy'
    foreach ($name in @('pinned', 'selectedWorkerPhysicalCoreMaskComplete')) {
        Assert-Stage5InstalledKernelRawBoolean $receipt.worker.$name `
            "Installed-kernel receipt worker $name"
    }
    foreach ($name in @('submittedJobCount', 'executedJobCount',
            'failedJobCount', 'cancelledJobCount', 'serialFallbackCount',
            'workerWaitRejectionCount', 'maximumActiveWorkers')) {
        Assert-Stage5InstalledKernelRawInteger $receipt.schedulerMetrics.$name `
            "Installed-kernel scheduler $name"
    }
    Assert-Stage5InstalledKernelRawArray $receipt.kernels `
        'Installed-kernel receipt kernels' 6 6
    Assert-Stage5InstalledKernelRawArray $receipt.kernelTiming.streams `
        'Installed-kernel timing streams' 1 8
    Assert-Stage5InstalledKernelRawArray $receipt.kernelReference.streams `
        'Installed-kernel reference streams' 1 8
    Assert-Stage5InstalledKernelRawBoolean $receipt.kernelTiming.complete `
        'Installed-kernel timing complete'
    Assert-Stage5InstalledKernelRawBoolean $receipt.kernelReference.complete `
        'Installed-kernel reference complete'
    Assert-Stage5InstalledKernelRawInteger $receipt.frames.finalCrc `
        'Installed-kernel receipt final CRC'
    Assert-Stage5InstalledKernelRawString $receipt.rawEvidence.rawLogSha256 `
        'Installed-kernel receipt raw-log SHA-256'
    Assert-Stage5InstalledKernelRawString $receipt.rawEvidence.timingSha256 `
        'Installed-kernel receipt timing SHA-256'
    Assert-Stage5InstalledKernelRawInteger $RunBinding.ordinal `
        'Installed-kernel run ordinal'
    $nativeVersion = if ($isV6) { '6' } else { '5' }
    Assert-Stage5InstalledKernelCondition (
        $receipt.producer -ceq
            "game-executable-stage5-performance-report-v$nativeVersion" -and
        $receipt.evidenceKind -ceq 'stage5-executable-originated-receipt' -and
        $receipt.status -ceq 'passed' -and
        $receipt.role -ceq 'performance-report' -and
        $receipt.producerVersion -ceq $nativeVersion -and
        $receipt.title -ceq $Expected.title -and
        $receipt.sourceCommit -ceq $Expected.sourceCommit -and
        $receipt.artifactSetSha256 -ceq $Expected.artifactSetSha256 -and
        $receipt.executableSha256 -ceq $Expected.executableSha256 -and
        $receipt.cohortNonce -ceq $Expected.cohortNonce -and
        $receipt.cohortCreatedUtc -ceq $Expected.cohortCreatedUtc -and
        $receipt.runId -ceq $RunBinding.runId -and
        $receipt.runNonce -ceq $RunBinding.runNonce -and
        $receipt.measurementRole -ceq 'throughput' -and
        $receipt.simulationMode -ceq 'parallel' -and
        $receipt.schedulerStarted -is [bool] -and
        [bool]$receipt.schedulerStarted) `
        'Installed-kernel receipt identity or parallel execution role is invalid.'
    Assert-Stage5InstalledKernelExactProperties $receipt.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Installed-kernel receipt runtime closure'
    Assert-Stage5InstalledKernelCondition (
        $receipt.runtimeClosure.dependencyManifestSha256 -ceq
            $Expected.dependencyManifestSha256 -and
        $receipt.runtimeClosure.closureSha256 -ceq
            $Expected.runtimeClosureSha256) `
        'Installed-kernel receipt runtime closure changed.'

    foreach ($name in @('recordedUtc', 'cohortCreatedUtc')) {
        [void](Assert-Stage5InstalledKernelCanonicalUtc `
            ([string]$receipt.$name) "Installed-kernel receipt $name")
    }
    Assert-Stage5InstalledKernelCondition (
        $receipt.runNonce -cmatch
            $script:Stage5InstalledKernelUuidPattern) `
        'Installed-kernel receipt run nonce is not a canonical version-4 UUID.'
    Assert-Stage5InstalledKernelExactProperties $receipt.process @('id',
        'creationTimeUtc100ns', 'startTimeUtc100ns', 'endTimeUtc100ns',
        'identityAvailable', 'exitCodeKnown', 'exitCode', 'exitBoundary') `
        'Installed-kernel receipt process'
    Assert-Stage5InstalledKernelCondition (
        (Test-Stage5JsonInteger $receipt.process.id) -and
        [Int64]$receipt.process.id -gt 0 -and
        (Test-Stage5JsonInteger $receipt.process.creationTimeUtc100ns) -and
        [Int64]$receipt.process.creationTimeUtc100ns -gt 0 -and
        $receipt.process.identityAvailable -is [bool] -and
        [bool]$receipt.process.identityAvailable -and
        $receipt.process.exitCodeKnown -is [bool] -and
        [bool]$receipt.process.exitCodeKnown -and
        [int]$receipt.process.exitCode -eq 0) `
        'Installed-kernel receipt process identity or exit proof is invalid.'

    Assert-Stage5InstalledKernelCondition (
        $receipt.fixture.id -ceq 'dense-eight-player' -and
        $receipt.fixture.kind -ceq 'replay' -and
        $receipt.fixture.contentSha256 -ceq $Expected.fixtureSha256 -and
        [int]$receipt.fixture.seed -eq [int]$Expected.fixtureSeed -and
        [int]$receipt.fixture.requestedPlayerCount -eq 8 -and
        [Int64]$receipt.fixture.requestedMinimumUnitCount -ge 8000 -and
        $receipt.workload.sampling -ceq
            'completed-simulation-frame-boundary-v1' -and
        [int]$receipt.workload.playerCount -eq 8 -and
        $receipt.workload.rosterStable -is [bool] -and
        [bool]$receipt.workload.rosterStable -and
        $receipt.workload.contiguous -is [bool] -and
        [bool]$receipt.workload.contiguous -and
        [Int64]$receipt.workload.initialUnitCount -ge 8000 -and
        [Int64]$receipt.workload.minimumUnitCount -ge 8000 -and
        [Int64]$receipt.workload.peakUnitCount -ge 8000) `
        'Installed-kernel receipt is detached from the dense eight-player replay.'

    $workerMask = Get-Stage5InstalledKernelMaskHex `
        $receipt.worker.selectedWorkerPhysicalCoreMask `
        'Installed-kernel worker physical-core mask'
    Assert-Stage5InstalledKernelCondition (
        [int]$receipt.worker.requestedCount -eq 4 -and
        [int]$receipt.worker.effectiveCount -eq 4 -and
        $receipt.worker.policy -ceq 'auto' -and
        $receipt.worker.pinned -is [bool] -and [bool]$receipt.worker.pinned -and
        [int]$receipt.worker.selectedWorkerPhysicalCoreCount -eq 4 -and
        $receipt.worker.selectedWorkerPhysicalCoreMaskComplete -is [bool] -and
        [bool]$receipt.worker.selectedWorkerPhysicalCoreMaskComplete -and
        (Get-Stage5UInt64BitCount `
            ([Convert]::ToUInt64($receipt.worker.selectedWorkerPhysicalCoreMask))) -eq 4) `
        'Installed-kernel receipt does not prove the exact physical-4 lane.'

    $scheduler = $receipt.schedulerMetrics
    Assert-Stage5InstalledKernelCondition (
        [UInt64]$scheduler.submittedJobCount -gt 0 -and
        [UInt64]$scheduler.executedJobCount -eq
            [UInt64]$scheduler.submittedJobCount -and
        [UInt64]$scheduler.failedJobCount -eq 0 -and
        [UInt64]$scheduler.cancelledJobCount -eq 0 -and
        [UInt64]$scheduler.serialFallbackCount -eq 0 -and
        [UInt64]$scheduler.workerWaitRejectionCount -eq 0 -and
        [UInt64]$scheduler.maximumActiveWorkers -ge 2 -and
        [UInt64]$scheduler.maximumActiveWorkers -le 4) `
        'Installed-kernel scheduler did not complete clean multicore work.'

    $kernels = @($receipt.kernels)
    Assert-Stage5InstalledKernelCondition ($kernels.Count -eq 6) `
        'Installed-kernel receipt must contain six kernel worker records.'
    $timingByKey = @{}
    foreach ($stream in @($receipt.kernelTiming.streams)) {
        Assert-Stage5InstalledKernelRawString $stream.name `
            'Installed-kernel timing stream name'
        Assert-Stage5InstalledKernelRawInteger $stream.subtype `
            'Installed-kernel timing stream subtype'
        foreach ($name in @('attemptedBatches', 'admittedBatches',
                'committedBatches', 'abortedBatches')) {
            Assert-Stage5InstalledKernelRawInteger $stream.$name `
                "Installed-kernel timing stream $name"
        }
        $key = Get-Stage5InstalledKernelStreamKey $stream
        Assert-Stage5InstalledKernelCondition (-not
            $timingByKey.ContainsKey($key)) `
            'Installed-kernel timing stream is duplicated.'
        $timingByKey[$key] = $stream
    }
    $referenceByKey = @{}
    foreach ($stream in @($receipt.kernelReference.streams)) {
        Assert-Stage5InstalledKernelRawString $stream.name `
            'Installed-kernel reference stream name'
        Assert-Stage5InstalledKernelRawInteger $stream.subtype `
            'Installed-kernel reference stream subtype'
        foreach ($name in @('validatedBatchCount', 'committedBatchCount',
                'abortedBatchCount', 'validatedOperationCount',
                'committedOperationCount')) {
            Assert-Stage5InstalledKernelRawInteger $stream.$name `
                "Installed-kernel reference stream $name"
        }
        foreach ($name in @('inputSha256', 'outputSha256', 'commitSha256')) {
            Assert-Stage5InstalledKernelRawString $stream.$name `
                "Installed-kernel reference stream $name"
        }
        $key = Get-Stage5InstalledKernelStreamKey $stream
        Assert-Stage5InstalledKernelCondition (-not
            $referenceByKey.ContainsKey($key)) `
            'Installed-kernel reference stream is duplicated.'
        $referenceByKey[$key] = $stream
    }
    Assert-Stage5InstalledKernelCondition (
        $receipt.kernelTiming.complete -is [bool] -and
        [bool]$receipt.kernelTiming.complete -and
        $receipt.kernelReference.complete -is [bool] -and
        [bool]$receipt.kernelReference.complete -and
        $timingByKey.Count -ge 6 -and $timingByKey.Count -le 8 -and
        $timingByKey.Count -eq $referenceByKey.Count) `
        'Installed-kernel timing/reference stream closure is incomplete.'

    $projectedKernels = New-Object 'Collections.Generic.List[object]'
    for ($index = 0; $index -lt 6; ++$index) {
        $kernel = $kernels[$index]
        $name = $script:Stage5InstalledKernelNames[$index]
        Assert-Stage5InstalledKernelRawString $kernel.name `
            "Installed-kernel $name name"
        Assert-Stage5InstalledKernelRawBoolean $kernel.available `
            "Installed-kernel $name available"
        foreach ($field in @('submittedJobs', 'completedJobs',
                'physicalWorkerJobs', 'ownerHelpedJobs', 'physicalWorkerMask',
                'distinctPhysicalWorkers')) {
            Assert-Stage5InstalledKernelRawInteger $kernel.$field `
                "Installed-kernel $name $field"
        }
        Assert-Stage5InstalledKernelRawBoolean $kernel.physicalWorkerMaskComplete `
            "Installed-kernel $name physicalWorkerMaskComplete"
        $mask = Get-Stage5InstalledKernelMaskHex $kernel.physicalWorkerMask `
            "Installed-kernel $name worker mask"
        Assert-Stage5InstalledKernelCondition (
            $kernel.name -ceq $name -and
            $kernel.available -is [bool] -and [bool]$kernel.available -and
            [UInt64]$kernel.submittedJobs -gt 0 -and
            [UInt64]$kernel.completedJobs -eq
                [UInt64]$kernel.submittedJobs -and
            [UInt64]$kernel.physicalWorkerJobs -gt 0 -and
            ([decimal][UInt64]$kernel.physicalWorkerJobs +
                [decimal][UInt64]$kernel.ownerHelpedJobs) -eq
                [decimal][UInt64]$kernel.completedJobs -and
            [UInt64]$kernel.physicalWorkerMask -ne 0 -and
            $kernel.physicalWorkerMaskComplete -is [bool] -and
            [bool]$kernel.physicalWorkerMaskComplete -and
            [UInt64]$kernel.distinctPhysicalWorkers -gt 0 -and
            [UInt64]$kernel.distinctPhysicalWorkers -le 4) `
            "Installed-kernel '$name' lacks physical-worker execution authority."
        $projectedStreams = New-Object 'Collections.Generic.List[object]'
        foreach ($key in @($timingByKey.Keys | Sort-Object)) {
            $timing = $timingByKey[$key]
            if ($timing.name -cne $name) { continue }
            Assert-Stage5InstalledKernelCondition (
                $referenceByKey.ContainsKey($key)) `
                "Installed-kernel '$name' timing lacks owner reference commit."
            $reference = $referenceByKey[$key]
            foreach ($hashName in @('inputSha256', 'outputSha256',
                    'commitSha256')) {
                Assert-Stage5InstalledKernelSha256 `
                    $reference.$hashName `
                    "Installed-kernel '$name' $hashName"
            }
            Assert-Stage5InstalledKernelCondition (
                [UInt64]$timing.attemptedBatches -gt 0 -and
                [UInt64]$timing.admittedBatches -gt 0 -and
                [UInt64]$timing.committedBatches -gt 0 -and
                [UInt64]$timing.abortedBatches -eq 0 -and
                [UInt64]$reference.validatedBatchCount -gt 0 -and
                [UInt64]$reference.committedBatchCount -gt 0 -and
                [UInt64]$reference.abortedBatchCount -eq 0 -and
                [UInt64]$reference.validatedOperationCount -gt 0 -and
                [UInt64]$reference.committedOperationCount -gt 0 -and
                [UInt64]$reference.committedBatchCount -eq
                    [UInt64]$timing.committedBatches) `
                "Installed-kernel '$name' lacks admitted, committed owner streams."
            $projectedStreams.Add([pscustomobject][ordered]@{
                subtype = [int]$timing.subtype
                attemptedBatches = [Int64]$timing.attemptedBatches
                admittedBatches = [Int64]$timing.admittedBatches
                committedBatches = [Int64]$timing.committedBatches
                abortedBatches = [Int64]$timing.abortedBatches
                validatedBatchCount = [Int64]$reference.validatedBatchCount
                committedBatchCount = [Int64]$reference.committedBatchCount
                abortedBatchCount = [Int64]$reference.abortedBatchCount
                validatedOperationCount =
                    [Int64]$reference.validatedOperationCount
                committedOperationCount =
                    [Int64]$reference.committedOperationCount
                inputSha256 = $reference.inputSha256
                outputSha256 = $reference.outputSha256
                commitSha256 = $reference.commitSha256
            }) | Out-Null
        }
        Assert-Stage5InstalledKernelCondition ($projectedStreams.Count -ge 1 -and
            $projectedStreams.Count -le 2) `
            "Installed-kernel '$name' has invalid stream coverage."
        $projectedKernels.Add([pscustomobject][ordered]@{
            name = $name
            worker = [pscustomobject][ordered]@{
                submittedJobs = [Int64]$kernel.submittedJobs
                completedJobs = [Int64]$kernel.completedJobs
                physicalWorkerJobs = [Int64]$kernel.physicalWorkerJobs
                ownerHelpedJobs = [Int64]$kernel.ownerHelpedJobs
                physicalWorkerMask = $mask
                distinctPhysicalWorkers = [int]$kernel.distinctPhysicalWorkers
                physicalWorkerMaskComplete = $true
            }
            streams = $projectedStreams.ToArray()
        }) | Out-Null
    }

    Assert-Stage5FinalAcceptanceSnapshotSha256 $RawSnapshot `
        $receipt.rawEvidence.rawLogSha256 `
        'Installed-kernel raw diagnostic' | Out-Null
    Assert-Stage5FinalAcceptanceSnapshotSha256 $TimingSnapshot `
        $receipt.rawEvidence.timingSha256 `
        'Installed-kernel timing diagnostic' | Out-Null
    Assert-Stage5InstalledKernelRawInteger $RawSnapshot.length `
        'Installed-kernel raw diagnostic length'
    Assert-Stage5InstalledKernelRawInteger $TimingSnapshot.length `
        'Installed-kernel timing diagnostic length'
    Assert-Stage5InstalledKernelCondition ($RawSnapshot.length -gt 0 -and
        $TimingSnapshot.length -gt 0) `
        'Installed-kernel raw/timing evidence is empty.'

    $attemptStart = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
        $AttemptStartSnapshot 'Installed-kernel attempt start'
    $attemptResult = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
        $AttemptResultSnapshot 'Installed-kernel attempt result'
    Assert-Stage5InstalledKernelCondition (
        $attemptStart.event -ceq 'attempt-start' -and
        $attemptStart.planSha256 -ceq $Expected.runPlanSha256 -and
        $attemptStart.entryId -ceq $receipt.runId -and
        $attemptStart.runNonce -ceq $receipt.runNonce -and
        $attemptResult.event -ceq 'attempt-result' -and
        $attemptResult.planSha256 -ceq $Expected.runPlanSha256 -and
        $attemptResult.entryId -ceq $receipt.runId -and
        $attemptResult.runNonce -ceq $receipt.runNonce -and
        $attemptResult.state -ceq 'completed' -and
        $null -eq $attemptResult.failure -and
        $attemptResult.processCleanup.exitProof -is [bool] -and
        [bool]$attemptResult.processCleanup.exitProof -and
        $attemptResult.processCleanup.blocked -is [bool] -and
        -not [bool]$attemptResult.processCleanup.blocked -and
        $attemptResult.startBinding.sha256 -ceq
            $AttemptStartSnapshot.sha256 -and
        $attemptResult.run.receiptSha256 -ceq $ReceiptSnapshot.sha256 -and
        $attemptResult.run.host.rawLogSha256 -ceq $RawSnapshot.sha256 -and
        $attemptResult.run.host.timingSha256 -ceq $TimingSnapshot.sha256) `
        'Installed-kernel attempt journal does not prove completed cleanup.'

    return [pscustomobject]@{
        projection = [pscustomobject][ordered]@{
            ordinal = [int]$RunBinding.ordinal
            runId = [string]$receipt.runId
            runNonce = [string]$receipt.runNonce
            processId = [int]$receipt.process.id
            processCreationTimeUtc100ns =
                [Int64]$receipt.process.creationTimeUtc100ns
            receipt = $OutputBindings.receipt
            rawLog = $OutputBindings.rawLog
            timing = $OutputBindings.timing
            attemptStart = $OutputBindings.attemptStart
            attemptResult = $OutputBindings.attemptResult
            finalCrc = [UInt32]$receipt.frames.finalCrc
            worker = [pscustomobject][ordered]@{
                requestedCount = 4
                effectiveCount = 4
                policy = 'auto'
                pinned = $true
                selectedWorkerPhysicalCoreCount = 4
                selectedWorkerPhysicalCoreMask = $workerMask
                selectedWorkerPhysicalCoreMaskComplete = $true
            }
            scheduler = [pscustomobject][ordered]@{
                submittedJobCount = [Int64]$scheduler.submittedJobCount
                executedJobCount = [Int64]$scheduler.executedJobCount
                failedJobCount = [Int64]$scheduler.failedJobCount
                cancelledJobCount = [Int64]$scheduler.cancelledJobCount
                serialFallbackCount = [Int64]$scheduler.serialFallbackCount
                workerWaitRejectionCount =
                    [Int64]$scheduler.workerWaitRejectionCount
                maximumActiveWorkers = [int]$scheduler.maximumActiveWorkers
            }
            kernels = $projectedKernels.ToArray()
        }
        workloadMinimumUnitCount = [Int64]$receipt.workload.minimumUnitCount
        workloadMaximumUnitCount = [Int64]$receipt.workload.peakUnitCount
    }
}

function Read-Stage5InstalledKernelAbsoluteBinding {
    param([string]$Boundary, [object]$Binding, [string]$Context,
        [ValidateSet('JsonReceipt', 'RawLog', 'Replay', 'RuntimeBinary')]
        [string]$EvidenceKind = 'JsonReceipt')
    Assert-Stage5InstalledKernelExactProperties $Binding @('path', 'sha256') `
        "$Context binding"
    Assert-Stage5InstalledKernelRawString $Binding.path "$Context path"
    Assert-Stage5InstalledKernelSha256 $Binding.sha256 `
        "$Context SHA-256"
    $path = [IO.Path]::GetFullPath($Binding.path)
    Assert-Stage5FinalAcceptancePathContained $Boundary $path $Context
    Assert-Stage5InstalledKernelCondition (Test-Path -LiteralPath $path `
        -PathType Leaf) "$Context was not found: $path"
    Assert-Stage5FinalAcceptanceNoReparsePath $Boundary $path $Context
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path $Context `
        -EvidenceKind $EvidenceKind
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        $Binding.sha256 $Context | Out-Null
    return [pscustomobject]@{ path = $path; snapshot = $snapshot }
}

function New-Stage5InstalledKernelFixtureProjection {
    param([object]$Production, [hashtable]$Bindings)
    $receipt = $Production.receipt
    $completion = $receipt.completion
    return [pscustomobject][ordered]@{
        hostReceipt = $Bindings.hostReceipt
        rawLog = $Bindings.rawLog
        reviewedFixtureManifest = $Bindings.reviewedFixtureManifest
        prelaunchPlan = $Bindings.prelaunchPlan
        attemptStart = $Bindings.attemptStart
        recordedUtc = [string]$receipt.recordedUtc
        cohortNonce = [string]$receipt.cohortNonce
        cohortCreatedUtc = [string]$receipt.cohortCreatedUtc
        qualificationMode = 'InstalledKernelExecution'
        acceptanceScope = 'fixture-production-only'
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
        sourceCommit = [string]$receipt.sourceCommit
        artifactSetSha256 = [string]$receipt.artifactSetSha256
        runtimeClosure = [pscustomobject][ordered]@{
            dependencyManifestSha256 =
                [string]$receipt.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$receipt.runtimeClosure.closureSha256
        }
        process = [pscustomobject][ordered]@{
            id = [int]$receipt.process.id
            creationTimeUtc100ns = [Int64]$receipt.process.creationTimeUtc100ns
            exitCode = 0
        }
        hostRunNonce = [string]$receipt.hostRunNonce
        nativeRunNonce = [string]$completion.nativeRunNonce
        executableSha256 = [string]$receipt.executableSha256
        commandLine = [string]$receipt.commandLine
        argumentString = [string]$receipt.argumentString
        map = [pscustomobject][ordered]@{
            path = [string]$Bindings.map.path
            key = [string]$completion.mapKey
            sha256 = [string]$completion.mapSha256
            crc32 = [string]$completion.mapCrc32
            byteCount = [Int64]$completion.mapByteCount
        }
        retainedReplay = [pscustomobject][ordered]@{
            path = [string]$Bindings.retainedReplay.path
            sha256 = [string]$completion.replaySha256
            frameCount = [Int64]$completion.replayFrameCount
        }
        frameBudget = [int]$completion.frameBudget
        endFrame = [int]$completion.endFrame
        winnerTeam = [int]$completion.winnerTeam
        finalCrc = [UInt32]$completion.finalCrc
        replayEpoch = [int]$completion.replayEpoch
        observedFirstFrame = [int]$completion.observedFirstFrame
        observedLastFrame = [int]$completion.observedLastFrame
        observedFrameSamples = [Int64]$completion.observedFrameSamples
        observedPlayerCount = 8
        initialUnitCount = [Int64]$completion.initialUnitCount
        peakUnitCount = [Int64]$completion.peakUnitCount
        playerUnits = @($completion.playerUnits | ForEach-Object {
            [pscustomobject][ordered]@{
                slot = [int]$_.slot
                initialUnitCount = [Int64]$_.initialUnitCount
                peakUnitCount = [Int64]$_.peakUnitCount
            }
        })
        lifecycle = $receipt.lifecycle
    }
}

function Read-Stage5InstalledKernelSourceHost {
    param(
        [string]$Path,
        [string]$ExpectedSha256,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [string]$ExpectedDependencyManifestSha256,
        [string]$ExpectedRuntimeClosureSha256,
        [string]$ExpectedExecutableSha256
    )
    Assert-Stage5InstalledKernelSha256 $ExpectedSha256 `
        "$ExpectedTitle installed-kernel host aggregate SHA-256"
    $full = [IO.Path]::GetFullPath($Path)
    $root = (Split-Path -Parent $full).TrimEnd('\', '/')
    $hostSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        "$ExpectedTitle installed-kernel host aggregate"
    Assert-Stage5FinalAcceptanceSnapshotSha256 $hostSnapshot $ExpectedSha256 `
        "$ExpectedTitle installed-kernel host aggregate" | Out-Null
    $hostDocument = ConvertFrom-Stage5InstalledKernelJsonSnapshot $hostSnapshot `
        "$ExpectedTitle installed-kernel host aggregate"
    $hostProperties = @('schemaVersion', 'evidenceKind', 'producer', 'status',
        'recordedUtc', 'cohortNonce', 'cohortCreatedUtc', 'qualificationMode',
        'qualificationClass', 'measurementMode', 'referencePolicy',
        'installedRuntime', 'sourceCommit', 'artifactSetSha256',
        'artifactSetManifest', 'runtimeClosure', 'title', 'executable',
        'fixtureManifest', 'stage3Baseline', 'launcher', 'profileStrategy',
        'registryViews', 'environmentVariables', 'profileConcurrency',
        'validationConcurrency', 'titleSessionProfile', 'schedule', 'topology',
        'thresholds', 'nativeReceiptBindings', 'pairedOracleBindings',
        'fixtures', 'runs', 'acceptanceScope', 'finalAcceptanceClaim',
        'performanceScalingClaim', 'fixtureProductionReceipt',
        'validationManifest', 'runPlan', 'attemptManifest')
    Assert-Stage5InstalledKernelExactProperties $hostDocument $hostProperties `
        "$ExpectedTitle installed-kernel host aggregate"
    Assert-Stage5InstalledKernelRawInteger $hostDocument.schemaVersion `
        "$ExpectedTitle host schemaVersion"
    foreach ($name in @('evidenceKind', 'producer', 'status',
            'qualificationMode', 'qualificationClass', 'referencePolicy',
            'title', 'sourceCommit', 'artifactSetSha256', 'cohortNonce',
            'cohortCreatedUtc', 'acceptanceScope')) {
        Assert-Stage5InstalledKernelRawString $hostDocument.$name `
            "$ExpectedTitle host $name"
    }
    Assert-Stage5InstalledKernelRawString $hostDocument.profileStrategy `
        "$ExpectedTitle host profileStrategy"
    Assert-Stage5InstalledKernelCondition (
        $hostDocument.profileStrategy -ceq
            'process-local-validation-profile-root') `
        "$ExpectedTitle host profileStrategy is not process-local."
    Assert-Stage5InstalledKernelRawString $hostDocument.profileConcurrency `
        "$ExpectedTitle host profileConcurrency"
    Assert-Stage5InstalledKernelCondition (
        $hostDocument.profileConcurrency -ceq 'shared-title-profile-read-only') `
        "$ExpectedTitle host profileConcurrency is not the reviewed read-only contract."
    Assert-Stage5InstalledKernelRawArray $hostDocument.registryViews `
        "$ExpectedTitle host registryViews" 2 2
    Assert-Stage5InstalledKernelCondition (
        (@($hostDocument.registryViews | ForEach-Object { [string]$_ }) -join '|') -ceq
            'Registry32|Registry64') `
        "$ExpectedTitle host registryViews are stale, reordered, or incomplete."
    Assert-Stage5InstalledKernelRawArray $hostDocument.environmentVariables `
        "$ExpectedTitle host environmentVariables" 12 12
    $expectedProfileEnvironmentVariables = @(
        'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA', 'USERPROFILE',
        'HOMEDRIVE', 'HOMEPATH', 'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
        'RTS_STAGE5_VALIDATION_CACHE_ROOT', 'RTS_STAGE5_VALIDATION_LOG_ROOT',
        'RTS_STAGE5_VALIDATION_DUMP_ROOT',
        'RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT')
    Assert-Stage5InstalledKernelCondition (
        @($hostDocument.environmentVariables |
            ForEach-Object { [string]$_ }) -join '|' -ceq
            ($expectedProfileEnvironmentVariables -join '|')) `
        "$ExpectedTitle host environmentVariables do not bind the complete process-local profile."
    foreach ($name in @('installedRuntime', 'finalAcceptanceClaim',
            'performanceScalingClaim')) {
        Assert-Stage5InstalledKernelRawBoolean $hostDocument.$name `
            "$ExpectedTitle host $name"
    }
    Assert-Stage5InstalledKernelRawString $hostDocument.executable.sha256 `
        "$ExpectedTitle host executable SHA-256"
    Assert-Stage5InstalledKernelRawString `
        $hostDocument.runtimeClosure.dependencyManifestSha256 `
        "$ExpectedTitle host dependency-manifest SHA-256"
    Assert-Stage5InstalledKernelRawString $hostDocument.runtimeClosure.closureSha256 `
        "$ExpectedTitle host runtime-closure SHA-256"
    Assert-Stage5InstalledKernelRawInteger $hostDocument.schedule.warmupRuns `
        "$ExpectedTitle host warmupRuns"
    Assert-Stage5InstalledKernelRawInteger $hostDocument.schedule.measuredRuns `
        "$ExpectedTitle host measuredRuns"
    Assert-Stage5InstalledKernelRawArray $hostDocument.pairedOracleBindings `
        "$ExpectedTitle host paired-oracle bindings" 0 0
    Assert-Stage5InstalledKernelRawArray $hostDocument.runs `
        "$ExpectedTitle host runs" 1 100
    Assert-Stage5InstalledKernelCondition (
        (Test-Stage5JsonInteger $hostDocument.schemaVersion) -and
        $hostDocument.schemaVersion -eq 2 -and
        $hostDocument.evidenceKind -ceq 'stage5-installed-kernel-execution-host' -and
        $hostDocument.producer -ceq 'Invoke-Stage5PerformanceScalingValidation.ps1' -and
        $hostDocument.status -ceq 'passed' -and
        $hostDocument.qualificationMode -ceq 'InstalledKernelExecution' -and
        $hostDocument.qualificationClass -ceq 'installed-kernel-execution-only' -and
        $hostDocument.referencePolicy -ceq 'throughput-only' -and
        $hostDocument.installedRuntime -is [bool] -and
        [bool]$hostDocument.installedRuntime -and
        $hostDocument.acceptanceScope -ceq 'kernel-execution-only' -and
        $hostDocument.finalAcceptanceClaim -is [bool] -and
        -not [bool]$hostDocument.finalAcceptanceClaim -and
        $hostDocument.performanceScalingClaim -is [bool] -and
        -not [bool]$hostDocument.performanceScalingClaim -and
        $hostDocument.title -ceq $ExpectedTitle -and
        $hostDocument.sourceCommit -ceq $ExpectedSourceCommit -and
        $hostDocument.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $hostDocument.cohortNonce -ceq $ExpectedCohortNonce -and
        $hostDocument.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $hostDocument.executable.sha256 -ceq $ExpectedExecutableSha256 -and
        $hostDocument.runtimeClosure.dependencyManifestSha256 -ceq
            $ExpectedDependencyManifestSha256 -and
        $hostDocument.runtimeClosure.closureSha256 -ceq
            $ExpectedRuntimeClosureSha256 -and
        $null -eq $hostDocument.stage3Baseline -and
        $null -eq $hostDocument.thresholds -and
        @($hostDocument.pairedOracleBindings).Count -eq 0 -and
        [int]$hostDocument.schedule.warmupRuns -eq 1 -and
        [int]$hostDocument.schedule.measuredRuns -ge 3 -and
        [int]$hostDocument.schedule.measuredRuns -le 100) `
        "$ExpectedTitle installed-kernel host identity or non-scaling scope is invalid."
    [void](Assert-Stage5InstalledKernelCanonicalUtc `
        $hostDocument.recordedUtc "$ExpectedTitle host recordedUtc")

    $validationFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
        $hostDocument.validationManifest "$ExpectedTitle validation manifest"
    $runPlanFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
        $hostDocument.runPlan `
        "$ExpectedTitle run plan"
    $attemptManifestFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
        $hostDocument.attemptManifest "$ExpectedTitle attempt manifest"
    $validation = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
        $validationFile.snapshot "$ExpectedTitle validation manifest"
    $runPlan = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
        $runPlanFile.snapshot "$ExpectedTitle run plan"
    $attemptManifest = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
        $attemptManifestFile.snapshot "$ExpectedTitle attempt manifest"
    Assert-Stage5InstalledKernelRawString `
        $hostDocument.fixtureProductionReceipt.path `
        "$ExpectedTitle fixture-production receipt path"
    Assert-Stage5InstalledKernelRawString `
        $hostDocument.fixtureProductionReceipt.sha256 `
        "$ExpectedTitle fixture-production receipt SHA-256"
    foreach ($name in @('title', 'qualificationMode', 'sourceCommit',
            'artifactSetSha256', 'cohortNonce', 'cohortCreatedUtc',
            'executableSha256')) {
        Assert-Stage5InstalledKernelRawString $validation.$name `
            "$ExpectedTitle validation $name"
    }
    Assert-Stage5InstalledKernelRawString `
        $validation.runtimeClosure.dependencyManifestSha256 `
        "$ExpectedTitle validation dependency-manifest SHA-256"
    Assert-Stage5InstalledKernelRawString $validation.runtimeClosure.closureSha256 `
        "$ExpectedTitle validation runtime-closure SHA-256"
    Assert-Stage5InstalledKernelRawString `
        $validation.fixtureProductionReceipt.path `
        "$ExpectedTitle validation fixture-production path"
    Assert-Stage5InstalledKernelRawString `
        $validation.fixtureProductionReceipt.sha256 `
        "$ExpectedTitle validation fixture-production SHA-256"
    foreach ($name in @('qualificationMode', 'cohortNonce',
            'cohortCreatedUtc')) {
        Assert-Stage5InstalledKernelRawString $runPlan.$name `
            "$ExpectedTitle run plan $name"
    }
    Assert-Stage5InstalledKernelRawString $attemptManifest.planSha256 `
        "$ExpectedTitle attempt manifest planSha256"
    Assert-Stage5InstalledKernelRawArray $runPlan.entries `
        "$ExpectedTitle run plan entries" 1 101
    Assert-Stage5InstalledKernelRawArray $attemptManifest.outcomes `
        "$ExpectedTitle attempt outcomes" 1 101
    Assert-Stage5InstalledKernelRawArray $validation.runs `
        "$ExpectedTitle validation runs" 1 101

    $runner = Join-Path $PSScriptRoot `
        'Invoke-Stage5PerformanceScalingValidation.ps1'
    Assert-Stage5InstalledKernelCondition (Test-Path -LiteralPath $runner `
        -PathType Leaf) 'Installed-kernel host self-test runner is absent.'
    & $runner -SelfTestValidationManifestPath $validationFile.path | Out-Null

    Assert-Stage5InstalledKernelCondition (
        $validation.title -ceq $ExpectedTitle -and
        $validation.qualificationMode -ceq 'InstalledKernelExecution' -and
        $validation.sourceCommit -ceq $ExpectedSourceCommit -and
        $validation.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $validation.cohortNonce -ceq $ExpectedCohortNonce -and
        $validation.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $validation.executableSha256 -ceq $ExpectedExecutableSha256 -and
        $validation.runtimeClosure.dependencyManifestSha256 -ceq
            $ExpectedDependencyManifestSha256 -and
        $validation.runtimeClosure.closureSha256 -ceq
            $ExpectedRuntimeClosureSha256 -and
        $validation.fixtureProductionReceipt.path -ceq
            $hostDocument.fixtureProductionReceipt.path -and
        $validation.fixtureProductionReceipt.sha256 -ceq
            $hostDocument.fixtureProductionReceipt.sha256 -and
        $runPlan.qualificationMode -ceq 'InstalledKernelExecution' -and
        $runPlan.cohortNonce -ceq $ExpectedCohortNonce -and
        $runPlan.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $attemptManifest.planSha256 -ceq $hostDocument.runPlan.sha256 -and
        $null -eq $attemptManifest.cohortFailure) `
        "$ExpectedTitle validation, plan, attempt, or fixture-production closure changed."

    $production = Read-Stage5NativePerformanceFixtureProductionReceipt `
        -Path $hostDocument.fixtureProductionReceipt.path `
        -ExpectedSha256 $hostDocument.fixtureProductionReceipt.sha256 `
        -ExpectedTitle $ExpectedTitle `
        -ExpectedCohortNonce $ExpectedCohortNonce `
        -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ExpectedExecutableSha256 $ExpectedExecutableSha256 `
        -ExpectedDependencyManifestSha256 $ExpectedDependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $ExpectedRuntimeClosureSha256

    foreach ($entry in @($runPlan.entries)) {
        Assert-Stage5InstalledKernelRawString $entry.entryId `
            "$ExpectedTitle run plan entryId"
        Assert-Stage5InstalledKernelRawString $entry.measurementRole `
            "$ExpectedTitle run plan measurementRole"
        Assert-Stage5InstalledKernelRawString $entry.runNonce `
            "$ExpectedTitle run plan runNonce"
        Assert-Stage5InstalledKernelRawInteger $entry.workerCount `
            "$ExpectedTitle run plan workerCount"
        Assert-Stage5InstalledKernelRawBoolean $entry.warmup `
            "$ExpectedTitle run plan warmup"
    }
    foreach ($outcome in @($attemptManifest.outcomes)) {
        Assert-Stage5InstalledKernelRawString $outcome.entryId `
            "$ExpectedTitle attempt outcome entryId"
        Assert-Stage5InstalledKernelRawString $outcome.state `
            "$ExpectedTitle attempt outcome state"
        Assert-Stage5InstalledKernelRawString $outcome.startBinding.sha256 `
            "$ExpectedTitle attempt outcome start SHA-256"
        Assert-Stage5InstalledKernelRawString $outcome.resultBinding.sha256 `
            "$ExpectedTitle attempt outcome result SHA-256"
    }
    foreach ($validationRun in @($validation.runs)) {
        foreach ($name in @('fixtureId', 'lane', 'runId', 'runNonce',
                'receiptPath', 'receiptSha256')) {
            Assert-Stage5InstalledKernelRawString $validationRun.$name `
                "$ExpectedTitle validation run $name"
        }
        Assert-Stage5InstalledKernelRawString $validationRun.host.rawLogSha256 `
            "$ExpectedTitle validation run raw-log SHA-256"
        Assert-Stage5InstalledKernelRawString $validationRun.host.timingSha256 `
            "$ExpectedTitle validation run timing SHA-256"
        Assert-Stage5InstalledKernelRawInteger $validationRun.ordinal `
            "$ExpectedTitle validation run ordinal"
        Assert-Stage5InstalledKernelRawBoolean $validationRun.warmup `
            "$ExpectedTitle validation run warmup"
    }
    foreach ($run in @($hostDocument.runs)) {
        foreach ($name in @('fixtureId', 'lane', 'runId', 'rawLogPath',
                'rawLogSha256', 'timingPath', 'timingSha256',
                'receiptSha256')) {
            Assert-Stage5InstalledKernelRawString $run.$name `
                "$ExpectedTitle host run $name"
        }
        Assert-Stage5InstalledKernelRawString $run.receiptBinding.runNonce `
            "$ExpectedTitle host run receiptBinding runNonce"
        Assert-Stage5InstalledKernelRawInteger $run.ordinal `
            "$ExpectedTitle host run ordinal"
        Assert-Stage5InstalledKernelRawBoolean $run.warmup `
            "$ExpectedTitle host run warmup"
    }

    $planById = @{}
    foreach ($entry in @($runPlan.entries)) {
        Assert-Stage5InstalledKernelCondition (-not
            $planById.ContainsKey([string]$entry.entryId)) `
            "$ExpectedTitle run plan repeats an entry."
        $planById[[string]$entry.entryId] = $entry
    }
    $outcomeById = @{}
    foreach ($outcome in @($attemptManifest.outcomes)) {
        Assert-Stage5InstalledKernelCondition (-not
            $outcomeById.ContainsKey([string]$outcome.entryId)) `
            "$ExpectedTitle attempt manifest repeats an entry."
        $outcomeById[[string]$outcome.entryId] = $outcome
    }
    Assert-Stage5InstalledKernelCondition ($planById.Count -eq
        (1 + [int]$hostDocument.schedule.measuredRuns) -and
        $outcomeById.Count -eq $planById.Count) `
        "$ExpectedTitle physical-4 run journal coverage is incomplete."
    $validationRunById = @{}
    foreach ($validationRun in @($validation.runs)) {
        $validationRunId = [string]$validationRun.runId
        Assert-Stage5InstalledKernelCondition (-not
            $validationRunById.ContainsKey($validationRunId)) `
            "$ExpectedTitle validation manifest repeats a run."
        $validationRunById[$validationRunId] = $validationRun
    }
    Assert-Stage5InstalledKernelCondition ($validationRunById.Count -eq
        $planById.Count) `
        "$ExpectedTitle validation manifest run coverage is incomplete."

    $runSources = New-Object 'Collections.Generic.List[object]'
    $measuredRuns = @($hostDocument.runs |
        Where-Object { -not $_.warmup } |
        Sort-Object ordinal)
    Assert-Stage5InstalledKernelCondition ($measuredRuns.Count -eq
        [int]$hostDocument.schedule.measuredRuns) `
        "$ExpectedTitle measured-run coverage is incomplete."
    foreach ($run in $measuredRuns) {
        Assert-Stage5InstalledKernelCondition (
            $run.fixtureId -ceq 'dense-eight-player' -and
            $run.lane -ceq 'physical-4' -and
            $planById.ContainsKey([string]$run.runId) -and
            $outcomeById.ContainsKey([string]$run.runId) -and
            $validationRunById.ContainsKey([string]$run.runId)) `
            "$ExpectedTitle measured run is detached from physical-4 plan."
        $entry = $planById[[string]$run.runId]
        $outcome = $outcomeById[[string]$run.runId]
        $validationRun = $validationRunById[[string]$run.runId]
        Assert-Stage5InstalledKernelCondition (
            $entry.measurementRole -ceq 'throughput' -and
            [int]$entry.workerCount -eq 4 -and
            -not [bool]$entry.warmup -and
            $validationRun.fixtureId -ceq $run.fixtureId -and
            $validationRun.lane -ceq $run.lane -and
            [int]$validationRun.ordinal -eq [int]$run.ordinal -and
            -not [bool]$validationRun.warmup -and
            $entry.runNonce -ceq $validationRun.runNonce -and
            $run.receiptBinding.runNonce -ceq $validationRun.runNonce -and
            $run.receiptSha256 -ceq $validationRun.receiptSha256 -and
            $run.rawLogSha256 -ceq $validationRun.host.rawLogSha256 -and
            $run.timingSha256 -ceq $validationRun.host.timingSha256 -and
            $outcome.state -ceq 'completed' -and
            $null -eq $outcome.failure) `
            "$ExpectedTitle measured physical-4 attempt did not complete."
        $receiptFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
            ([pscustomobject]@{ path = $validationRun.receiptPath
                sha256 = $validationRun.receiptSha256 }) `
            "$ExpectedTitle run '$($run.runId)' receipt"
        $rawFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
            ([pscustomobject]@{ path = $run.rawLogPath
                sha256 = $run.rawLogSha256 }) `
            "$ExpectedTitle run '$($run.runId)' raw log" RawLog
        $timingFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
            ([pscustomobject]@{ path = $run.timingPath
                sha256 = $run.timingSha256 }) `
            "$ExpectedTitle run '$($run.runId)' timing" RawLog
        $startFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
            $outcome.startBinding "$ExpectedTitle run '$($run.runId)' start"
        $resultFile = Read-Stage5InstalledKernelAbsoluteBinding $root `
            $outcome.resultBinding "$ExpectedTitle run '$($run.runId)' result"
        $runSources.Add([pscustomobject]@{
            run = $validationRun
            hostRun = $run
            entry = $entry
            outcome = $outcome
            receipt = $receiptFile
            rawLog = $rawFile
            timing = $timingFile
            attemptStart = $startFile
            attemptResult = $resultFile
        }) | Out-Null
    }

    return [pscustomobject]@{
        root = $root
        host = $hostDocument
        hostSnapshot = $hostSnapshot
        validation = $validation
        validationFile = $validationFile
        runPlan = $runPlan
        runPlanFile = $runPlanFile
        attemptManifest = $attemptManifest
        attemptManifestFile = $attemptManifestFile
        production = $production
        runSources = $runSources.ToArray()
    }
}

function Get-Stage5InstalledKernelSourcePaths {
    param([object]$SourceHost)
    $paths = New-Object 'Collections.Generic.List[string]'
    foreach ($path in @($SourceHost.hostSnapshot.path,
            $SourceHost.validationFile.path, $SourceHost.runPlanFile.path,
            $SourceHost.attemptManifestFile.path) +
            @($SourceHost.production.filePaths)) {
        $paths.Add([string]$path) | Out-Null
    }
    foreach ($run in @($SourceHost.runSources)) {
        foreach ($name in @('receipt', 'rawLog', 'timing', 'attemptStart',
                'attemptResult')) {
            $paths.Add([string]$run.$name.path) | Out-Null
        }
    }
    return $paths.ToArray()
}

function Open-Stage5InstalledKernelReadLocks {
    param([string[]]$Paths)
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $locks = New-Object 'Collections.Generic.List[IO.FileStream]'
    try {
        foreach ($path in $Paths) {
            $full = [IO.Path]::GetFullPath($path)
            if (-not $seen.Add($full)) { continue }
            Assert-Stage5InstalledKernelCondition (Test-Path -LiteralPath $full `
                -PathType Leaf) "Installed-kernel lock input is absent: $full"
            Assert-Stage5FinalAcceptanceNoReparsePath `
                ([IO.Path]::GetPathRoot($full)) $full `
                'Installed-kernel immutable source'
            $locks.Add([IO.File]::Open($full, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)) | Out-Null
        }
        return $locks.ToArray()
    }
    catch {
        foreach ($lock in $locks) {
            try { $lock.Dispose() } catch { }
        }
        throw
    }
}

function Close-Stage5InstalledKernelReadLocks {
    param([object[]]$Locks)
    $errors = New-Object 'Collections.Generic.List[string]'
    foreach ($lock in @($Locks)) {
        try { $lock.Dispose() }
        catch { $errors.Add($_.Exception.Message) | Out-Null }
    }
    Assert-Stage5InstalledKernelCondition ($errors.Count -eq 0) `
        "Installed-kernel read-lock cleanup failed: $($errors -join ' | ')"
}

function Get-Stage5InstalledKernelProductionSnapshots {
    param([object]$Production)
    $root = (Split-Path -Parent $Production.path).TrimEnd('\', '/')
    $entries = [ordered]@{
        hostReceipt = [pscustomobject]@{
            path = $Production.path; kind = 'JsonReceipt'
            sha256 = $Production.sha256
        }
        reviewedFixtureManifest = [pscustomobject]@{
            path = $Production.reviewedFixture.path; kind = 'JsonReceipt'
            sha256 = $Production.reviewedFixture.sha256
        }
        map = [pscustomobject]@{
            path = $Production.reviewedFixture.fixture.sourcePath
            kind = 'RuntimeBinary'
            sha256 = $Production.reviewedFixture.fixture.sha256
        }
        prelaunchPlan = [pscustomobject]@{
            path = $Production.prelaunchPlan.path; kind = 'JsonReceipt'
            sha256 = $Production.prelaunchPlan.sha256
        }
        attemptStart = [pscustomobject]@{
            path = $Production.attemptStart.path; kind = 'JsonReceipt'
            sha256 = $Production.attemptStart.sha256
        }
        rawLog = [pscustomobject]@{
            path = $Production.rawLog.path; kind = 'RawLog'
            sha256 = $Production.rawLog.sha256
        }
        retainedReplay = [pscustomobject]@{
            path = $Production.retainedReplay.path; kind = 'Replay'
            sha256 = $Production.retainedReplay.sha256
        }
    }
    $result = [ordered]@{}
    foreach ($name in $entries.Keys) {
        $entry = $entries[$name]
        $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.path `
            "Installed-kernel fixture production $name" `
            -EvidenceKind $entry.kind
        Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $entry.sha256 `
            "Installed-kernel fixture production $name" | Out-Null
        $result[$name] = [pscustomobject]@{
            sourcePath = [string]$entry.path
            relativePath = Get-Stage5InstalledKernelRelativePath $root `
                ([string]$entry.path) `
                "Installed-kernel fixture production $name"
            snapshot = $snapshot
            kind = [string]$entry.kind
        }
    }
    return [pscustomobject]$result
}

function New-Stage5InstalledKernelExecutionEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$GeneralsHostPath,
        [Parameter(Mandatory = $true)][string]$GeneralsHostSha256,
        [Parameter(Mandatory = $true)][string]$ZeroHourHostPath,
        [Parameter(Mandatory = $true)][string]$ZeroHourHostSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$ExpectedDependencyManifestSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedRuntimeClosureSha256,
        [Parameter(Mandatory = $true)][string]$GeneralsExecutableSha256,
        [Parameter(Mandatory = $true)][string]$ZeroHourExecutableSha256,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )
    Assert-Stage5InstalledKernelSourceCommit $ExpectedSourceCommit `
        'Installed-kernel expected source commit'
    foreach ($binding in @(
            @($GeneralsHostSha256, 'Generals host aggregate SHA-256'),
            @($ZeroHourHostSha256, 'Zero Hour host aggregate SHA-256'),
            @($ExpectedArtifactSetSha256, 'Expected artifact-set SHA-256'),
            @($ExpectedDependencyManifestSha256,
                'Expected dependency-manifest SHA-256'),
            @($ExpectedRuntimeClosureSha256,
                'Expected runtime-closure SHA-256'),
            @($GeneralsExecutableSha256, 'Generals executable SHA-256'),
            @($ZeroHourExecutableSha256, 'Zero Hour executable SHA-256'))) {
        Assert-Stage5InstalledKernelSha256 ([string]$binding[0]) `
            ([string]$binding[1])
    }
    Assert-Stage5InstalledKernelCondition ($ExpectedCohortNonce -cmatch
        $script:Stage5InstalledKernelUuidPattern) `
        'Expected installed-kernel cohort nonce is not a canonical version-4 UUID.'
    [void](Assert-Stage5InstalledKernelCanonicalUtc $ExpectedCohortCreatedUtc `
        'Expected installed-kernel cohort timestamp')

    $hostArguments = @{
        ExpectedSourceCommit = $ExpectedSourceCommit
        ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
        ExpectedCohortNonce = $ExpectedCohortNonce
        ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
        ExpectedDependencyManifestSha256 = $ExpectedDependencyManifestSha256
        ExpectedRuntimeClosureSha256 = $ExpectedRuntimeClosureSha256
    }
    $output = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\', '/')
    Assert-Stage5InstalledKernelCondition ($output.StartsWith('H:\',
        [StringComparison]::OrdinalIgnoreCase) -and $output.Length -gt 3 -and
        $output.Length -lt 220 -and -not (Test-Path -LiteralPath $output)) `
        'Installed-kernel output must be a fresh explicit task-owned H: path.'
    $parent = Split-Path -Parent $output
    Assert-Stage5InstalledKernelCondition (Test-Path -LiteralPath $parent `
        -PathType Container) 'Installed-kernel output parent is absent.'
    Assert-Stage5FinalAcceptanceNoReparsePath `
        ([IO.Path]::GetPathRoot($parent)) $parent `
        'Installed-kernel output parent'

    $preliminary = @(
        Read-Stage5InstalledKernelSourceHost -Path $GeneralsHostPath `
            -ExpectedSha256 $GeneralsHostSha256 -ExpectedTitle Generals `
            -ExpectedExecutableSha256 $GeneralsExecutableSha256 @hostArguments
        Read-Stage5InstalledKernelSourceHost -Path $ZeroHourHostPath `
            -ExpectedSha256 $ZeroHourHostSha256 -ExpectedTitle ZeroHour `
            -ExpectedExecutableSha256 $ZeroHourExecutableSha256 @hostArguments
    )
    $locks = @()
    $created = $false
    try {
        $locks = Open-Stage5InstalledKernelReadLocks @($preliminary |
            ForEach-Object { Get-Stage5InstalledKernelSourcePaths $_ })
        # Revalidate after acquiring deny-write capabilities for every evidence
        # file. No source bytes are read from an unlocked pathname below.
        $hosts = @(
            Read-Stage5InstalledKernelSourceHost -Path $GeneralsHostPath `
                -ExpectedSha256 $GeneralsHostSha256 -ExpectedTitle Generals `
                -ExpectedExecutableSha256 $GeneralsExecutableSha256 @hostArguments
            Read-Stage5InstalledKernelSourceHost -Path $ZeroHourHostPath `
                -ExpectedSha256 $ZeroHourHostSha256 -ExpectedTitle ZeroHour `
                -ExpectedExecutableSha256 $ZeroHourExecutableSha256 @hostArguments
        )
        New-Item -ItemType Directory -Path $output | Out-Null
        $created = $true
        Assert-Stage5FinalAcceptanceNoReparsePath $parent $output `
            'Installed-kernel output root'
        $titleProofs = New-Object 'Collections.Generic.List[object]'
        foreach ($sourceHost in $hosts) {
            $title = [string]$sourceHost.host.title
            $titleRootRelative = "Titles/$title"
            $hostBinding = Write-Stage5InstalledKernelSnapshot $output `
                "$titleRootRelative/Stage5InstalledKernelExecutionHost.json" `
                $sourceHost.hostSnapshot "$title host aggregate"
            $validationBinding = Write-Stage5InstalledKernelSnapshot $output `
                "$titleRootRelative/Stage5InstalledKernelExecutionValidationManifest.json" `
                $sourceHost.validationFile.snapshot "$title validation manifest"
            $runPlanBinding = Write-Stage5InstalledKernelSnapshot $output `
                "$titleRootRelative/phase-plan.json" `
                $sourceHost.runPlanFile.snapshot "$title run plan"
            $attemptManifestBinding = Write-Stage5InstalledKernelSnapshot `
                $output "$titleRootRelative/phase-attempts.json" `
                $sourceHost.attemptManifestFile.snapshot "$title attempt manifest"

            $productionSnapshots =
                Get-Stage5InstalledKernelProductionSnapshots `
                    $sourceHost.production
            $productionBindings = @{}
            foreach ($name in @('hostReceipt', 'reviewedFixtureManifest',
                    'map', 'prelaunchPlan', 'attemptStart', 'rawLog',
                    'retainedReplay')) {
                $source = $productionSnapshots.$name
                $productionBindings[$name] =
                    Write-Stage5InstalledKernelSnapshot $output `
                        "$titleRootRelative/FixtureProduction/$($source.relativePath)" `
                        $source.snapshot "$title fixture production $name" `
                        -EvidenceKind $source.kind
            }
            $fixtureProjection = New-Stage5InstalledKernelFixtureProjection `
                $sourceHost.production $productionBindings

            $runs = New-Object 'Collections.Generic.List[object]'
            [Int64]$minimumUnits = [Int64]::MaxValue
            [Int64]$maximumUnits = 0
            foreach ($sourceRun in @($sourceHost.runSources)) {
                $runId = [string]$sourceRun.run.runId
                $safeRunId = $runId
                Assert-Stage5InstalledKernelCondition ($safeRunId -cmatch
                    '^[A-Za-z0-9_.-]{1,256}$' -and
                    -not $safeRunId.Contains('..')) `
                    "$title measured run id is unsafe."
                $runRootRelative = "$titleRootRelative/Runs/$safeRunId"
                $bindings = @{}
                foreach ($definition in @(
                        @('receipt', 'receipt.json', 'JsonReceipt'),
                        @('rawLog', 'raw.log', 'RawLog'),
                        @('timing', 'timing.csv', 'RawLog'),
                        @('attemptStart', 'attempt.start.json', 'JsonReceipt'),
                        @('attemptResult', 'attempt.result.json', 'JsonReceipt'))) {
                    $name = [string]$definition[0]
                    $bindings[$name] = Write-Stage5InstalledKernelSnapshot `
                        $output "$runRootRelative/$($definition[1])" `
                        $sourceRun.$name.snapshot "$title run '$runId' $name" `
                        -EvidenceKind ([string]$definition[2])
                }
                $expected = [pscustomobject]@{
                    title = $title
                    sourceCommit = $ExpectedSourceCommit
                    artifactSetSha256 = $ExpectedArtifactSetSha256
                    executableSha256 = if ($title -ceq 'Generals') {
                        $GeneralsExecutableSha256
                    } else { $ZeroHourExecutableSha256 }
                    cohortNonce = $ExpectedCohortNonce
                    cohortCreatedUtc = $ExpectedCohortCreatedUtc
                    dependencyManifestSha256 =
                        $ExpectedDependencyManifestSha256
                    runtimeClosureSha256 = $ExpectedRuntimeClosureSha256
                    fixtureSha256 = $sourceHost.production.fixture.sha256
                    fixtureSeed = $sourceHost.production.fixture.seed
                    runPlanSha256 = $sourceHost.runPlanFile.snapshot.sha256
                }
                $converted = ConvertTo-Stage5InstalledKernelRunProjection `
                    $sourceRun.receipt.snapshot $sourceRun.rawLog.snapshot `
                    $sourceRun.timing.snapshot $sourceRun.attemptStart.snapshot `
                    $sourceRun.attemptResult.snapshot $sourceRun.run $expected `
                    $bindings
                $runs.Add($converted.projection) | Out-Null
                $minimumUnits = [Math]::Min($minimumUnits,
                    [Int64]$converted.workloadMinimumUnitCount)
                $maximumUnits = [Math]::Max($maximumUnits,
                    [Int64]$converted.workloadMaximumUnitCount)
            }
            $completion = $sourceHost.production.receipt.completion
            $titleProofs.Add([pscustomobject][ordered]@{
                title = $title
                executableSha256 = if ($title -ceq 'Generals') {
                    $GeneralsExecutableSha256
                } else { $ZeroHourExecutableSha256 }
                hostAggregate = $hostBinding
                validationManifest = $validationBinding
                runPlan = $runPlanBinding
                attemptManifest = $attemptManifestBinding
                fixtureProduction = $fixtureProjection
                fixture = [pscustomobject][ordered]@{
                    id = 'dense-eight-player'
                    kind = 'replay'
                    sha256 = [string]$sourceHost.production.fixture.sha256
                    seed = [int]$sourceHost.production.fixture.seed
                    playerCount = 8
                    requestedMinimumUnitCount = 8000
                    minimumInitialUnitCount =
                        [Int64]$completion.initialUnitCount
                    minimumObservedUnitCount = $minimumUnits
                    maximumObservedUnitCount = $maximumUnits
                    rosterStable = $true
                    contiguous = $true
                }
                schedule = [pscustomobject][ordered]@{
                    lane = 'physical-4'
                    requestedWorkers = 4
                    warmupRuns = 1
                    measuredRuns = [int]$sourceHost.host.schedule.measuredRuns
                }
                runs = $runs.ToArray()
            }) | Out-Null
        }
        $document = [pscustomobject][ordered]@{
            schemaVersion = 1
            evidenceKind = 'stage5-installed-kernel-execution'
            producer = 'installed-runtime-kernel-execution-projector-v1'
            status = 'passed'
            recordedUtc = [DateTime]::UtcNow.ToString('o',
                [Globalization.CultureInfo]::InvariantCulture)
            cohortNonce = $ExpectedCohortNonce
            cohortCreatedUtc = $ExpectedCohortCreatedUtc
            qualificationMode = 'InstalledKernelExecution'
            acceptanceScope = 'kernel-execution-only'
            finalAcceptanceClaim = $false
            performanceScalingClaim = $false
            sourceCommit = $ExpectedSourceCommit
            artifactSetSha256 = $ExpectedArtifactSetSha256
            runtimeClosure = [pscustomobject][ordered]@{
                dependencyManifestSha256 = $ExpectedDependencyManifestSha256
                closureSha256 = $ExpectedRuntimeClosureSha256
            }
            titleProofs = $titleProofs.ToArray()
        }
        $schemaPath = Join-Path $PSScriptRoot `
            'Stage5InstalledKernelExecution.schema.json'
        $json = $document | ConvertTo-Json -Depth 50
        Assert-Stage5InstalledKernelCondition ($json | Test-Json `
            -SchemaFile $schemaPath -ErrorAction SilentlyContinue) `
            'Installed-kernel projected evidence violates its closed schema.'
        $path = Join-Path $output 'Stage5InstalledKernelExecution.json'
        Write-Stage5FinalAcceptanceFileAtomically -Path $path `
            -Bytes ((New-Object Text.UTF8Encoding($false)).GetBytes($json)) `
            -Context 'Installed-kernel final projection' `
            -EvidenceKind JsonReceipt | Out-Null
        $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path `
            'Installed-kernel final projection'
        return Read-Stage5InstalledKernelExecutionEvidence -Path $path `
            -ExpectedSha256 $snapshot.sha256 `
            -ExpectedSourceCommit $ExpectedSourceCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
            -ExpectedCohortNonce $ExpectedCohortNonce `
            -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
            -ExpectedDependencyManifestSha256 `
                $ExpectedDependencyManifestSha256 `
            -ExpectedRuntimeClosureSha256 $ExpectedRuntimeClosureSha256 `
            -GeneralsExecutableSha256 $GeneralsExecutableSha256 `
            -ZeroHourExecutableSha256 $ZeroHourExecutableSha256
    }
    catch {
        if ($created -and (Test-Path -LiteralPath $output)) {
            $item = Get-Item -LiteralPath $output -Force
            if ($item.PSIsContainer -and
                ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and
                $output.StartsWith('H:\',
                    [StringComparison]::OrdinalIgnoreCase) -and
                $output.Length -gt 3) {
                Remove-Item -LiteralPath $output -Recurse -Force
            }
        }
        throw
    }
    finally { Close-Stage5InstalledKernelReadLocks $locks }
}

function Read-Stage5InstalledKernelExecutionEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$ExpectedDependencyManifestSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedRuntimeClosureSha256,
        [Parameter(Mandatory = $true)][string]$GeneralsExecutableSha256,
        [Parameter(Mandatory = $true)][string]$ZeroHourExecutableSha256
    )
    Assert-Stage5InstalledKernelSha256 $ExpectedSha256 `
        'Installed-kernel projection SHA-256'
    Assert-Stage5InstalledKernelSourceCommit $ExpectedSourceCommit `
        'Installed-kernel expected source commit'
    foreach ($binding in @(
            @($ExpectedArtifactSetSha256, 'Expected artifact-set SHA-256'),
            @($ExpectedDependencyManifestSha256,
                'Expected dependency-manifest SHA-256'),
            @($ExpectedRuntimeClosureSha256,
                'Expected runtime-closure SHA-256'),
            @($GeneralsExecutableSha256, 'Generals executable SHA-256'),
            @($ZeroHourExecutableSha256, 'Zero Hour executable SHA-256'))) {
        Assert-Stage5InstalledKernelSha256 ([string]$binding[0]) `
            ([string]$binding[1])
    }
    Assert-Stage5InstalledKernelCondition ($ExpectedCohortNonce -cmatch
        $script:Stage5InstalledKernelUuidPattern) `
        'Expected installed-kernel cohort nonce is not canonical.'
    [void](Assert-Stage5InstalledKernelCanonicalUtc $ExpectedCohortCreatedUtc `
        'Expected installed-kernel cohort timestamp')

    $full = [IO.Path]::GetFullPath($Path)
    $root = (Split-Path -Parent $full).TrimEnd('\', '/')
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        'Installed-kernel projection'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $ExpectedSha256 `
        'Installed-kernel projection' | Out-Null
    $strictJson = try {
        (New-Object Text.UTF8Encoding($false, $true)).GetString(
            [byte[]]$snapshot.bytes)
    }
    catch {
        throw "Installed-kernel projection is not strict UTF-8: $($_.Exception.Message)"
    }
    $schemaPath = Join-Path $PSScriptRoot `
        'Stage5InstalledKernelExecution.schema.json'
    Assert-Stage5InstalledKernelCondition ($strictJson | Test-Json `
        -SchemaFile $schemaPath -ErrorAction SilentlyContinue) `
        'Installed-kernel projection violates its closed schema.'
    $document = ConvertFrom-Stage5InstalledKernelJsonSnapshot $snapshot `
        'Installed-kernel projection'
    foreach ($name in @('sourceCommit', 'artifactSetSha256', 'cohortNonce',
            'cohortCreatedUtc', 'recordedUtc', 'acceptanceScope',
            'evidenceKind', 'producer', 'status', 'qualificationMode')) {
        Assert-Stage5InstalledKernelRawString $document.$name `
            "Installed-kernel projection $name"
    }
    Assert-Stage5InstalledKernelRawString `
        $document.runtimeClosure.dependencyManifestSha256 `
        'Installed-kernel projection dependency-manifest SHA-256'
    Assert-Stage5InstalledKernelRawString $document.runtimeClosure.closureSha256 `
        'Installed-kernel projection runtime-closure SHA-256'
    Assert-Stage5InstalledKernelRawBoolean $document.finalAcceptanceClaim `
        'Installed-kernel projection finalAcceptanceClaim'
    Assert-Stage5InstalledKernelRawBoolean $document.performanceScalingClaim `
        'Installed-kernel projection performanceScalingClaim'
    Assert-Stage5InstalledKernelRawArray $document.titleProofs `
        'Installed-kernel projection titleProofs' 2 2
    $recorded = Assert-Stage5InstalledKernelCanonicalUtc `
        $document.recordedUtc 'Installed-kernel projection recordedUtc'
    $cohortCreated = Assert-Stage5InstalledKernelCanonicalUtc `
        $document.cohortCreatedUtc `
        'Installed-kernel projection cohortCreatedUtc'
    Assert-Stage5InstalledKernelCondition (
        $document.sourceCommit -ceq $ExpectedSourceCommit -and
        $document.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $document.cohortNonce -ceq $ExpectedCohortNonce -and
        $document.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $document.runtimeClosure.dependencyManifestSha256 -ceq
            $ExpectedDependencyManifestSha256 -and
        $document.runtimeClosure.closureSha256 -ceq
            $ExpectedRuntimeClosureSha256 -and
        $recorded -ge $cohortCreated -and
        $document.finalAcceptanceClaim -is [bool] -and
        -not [bool]$document.finalAcceptanceClaim -and
        $document.performanceScalingClaim -is [bool] -and
        -not [bool]$document.performanceScalingClaim) `
        'Installed-kernel projection is detached from the expected cohort or candidate closure.'

    $validatedTitles = New-Object 'Collections.Generic.List[object]'
    for ($titleIndex = 0; $titleIndex -lt 2; ++$titleIndex) {
        $proof = @($document.titleProofs)[$titleIndex]
        $title = if ($titleIndex -eq 0) { 'Generals' } else { 'ZeroHour' }
        Assert-Stage5InstalledKernelRawString $proof.title `
            "$title proof title"
        Assert-Stage5InstalledKernelRawString $proof.executableSha256 `
            "$title proof executable SHA-256"
        $executableSha256 = if ($title -ceq 'Generals') {
            $GeneralsExecutableSha256
        } else { $ZeroHourExecutableSha256 }
        Assert-Stage5InstalledKernelCondition ($proof.title -ceq $title -and
            $proof.executableSha256 -ceq $executableSha256) `
            'Installed-kernel title proofs are missing or reordered.'

        $hostFile = Read-Stage5InstalledKernelBoundFile $root `
            $proof.hostAggregate "$title host aggregate"
        $validationFile = Read-Stage5InstalledKernelBoundFile $root `
            $proof.validationManifest "$title validation manifest"
        $runPlanFile = Read-Stage5InstalledKernelBoundFile $root `
            $proof.runPlan "$title run plan"
        $attemptManifestFile = Read-Stage5InstalledKernelBoundFile $root `
            $proof.attemptManifest "$title attempt manifest"
        $hostDocument = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
            $hostFile.snapshot "$title host aggregate"
        $validation = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
            $validationFile.snapshot "$title validation manifest"
        $runPlan = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
            $runPlanFile.snapshot "$title run plan"
        $attemptManifest = ConvertFrom-Stage5InstalledKernelJsonSnapshot `
            $attemptManifestFile.snapshot "$title attempt manifest"
        foreach ($name in @('evidenceKind', 'status', 'title',
                'qualificationMode', 'qualificationClass', 'acceptanceScope',
                'sourceCommit', 'artifactSetSha256', 'cohortNonce',
                'cohortCreatedUtc')) {
            Assert-Stage5InstalledKernelRawString $hostDocument.$name `
                "$title host $name"
        }
        foreach ($name in @('finalAcceptanceClaim',
                'performanceScalingClaim')) {
            Assert-Stage5InstalledKernelRawBoolean $hostDocument.$name `
                "$title host $name"
        }
        Assert-Stage5InstalledKernelRawString $hostDocument.executable.sha256 `
            "$title host executable SHA-256"
        Assert-Stage5InstalledKernelRawString `
            $hostDocument.runtimeClosure.dependencyManifestSha256 `
            "$title host dependency-manifest SHA-256"
        Assert-Stage5InstalledKernelRawString $hostDocument.runtimeClosure.closureSha256 `
            "$title host runtime-closure SHA-256"
        Assert-Stage5InstalledKernelRawInteger $hostDocument.schedule.warmupRuns `
            "$title host warmupRuns"
        Assert-Stage5InstalledKernelRawInteger $hostDocument.schedule.measuredRuns `
            "$title host measuredRuns"
        Assert-Stage5InstalledKernelRawArray $hostDocument.pairedOracleBindings `
            "$title host paired-oracle bindings" 0 0
        foreach ($name in @('title', 'qualificationMode', 'sourceCommit',
                'artifactSetSha256', 'cohortNonce', 'cohortCreatedUtc',
                'executableSha256')) {
            Assert-Stage5InstalledKernelRawString $validation.$name `
                "$title validation $name"
        }
        Assert-Stage5InstalledKernelRawString `
            $validation.runtimeClosure.dependencyManifestSha256 `
            "$title validation dependency-manifest SHA-256"
        Assert-Stage5InstalledKernelRawString $validation.runtimeClosure.closureSha256 `
            "$title validation runtime-closure SHA-256"
        Assert-Stage5InstalledKernelRawString `
            $validation.fixtureProductionReceipt.path `
            "$title validation fixture-production path"
        Assert-Stage5InstalledKernelRawString `
            $validation.fixtureProductionReceipt.sha256 `
            "$title validation fixture-production SHA-256"
        foreach ($name in @('qualificationMode', 'title', 'cohortNonce',
                'cohortCreatedUtc')) {
            Assert-Stage5InstalledKernelRawString $runPlan.$name `
                "$title run plan $name"
        }
        Assert-Stage5InstalledKernelRawString $attemptManifest.planSha256 `
            "$title attempt manifest planSha256"
        Assert-Stage5InstalledKernelRawArray $hostDocument.runs `
            "$title host runs" 1 100
        Assert-Stage5InstalledKernelRawArray $validation.runs `
            "$title validation runs" 1 101
        Assert-Stage5InstalledKernelRawArray $runPlan.entries `
            "$title run plan entries" 1 101
        Assert-Stage5InstalledKernelRawArray $attemptManifest.outcomes `
            "$title attempt outcomes" 1 101
        Assert-Stage5InstalledKernelRawString `
            $proof.fixtureProduction.hostReceipt.path `
            "$title fixture-production receipt path"
        Assert-Stage5InstalledKernelRawString `
            $proof.fixtureProduction.hostReceipt.sha256 `
            "$title fixture-production receipt SHA-256"
        foreach ($entry in @($runPlan.entries)) {
            Assert-Stage5InstalledKernelRawString $entry.entryId `
                "$title run plan entryId"
            Assert-Stage5InstalledKernelRawString $entry.measurementRole `
                "$title run plan measurementRole"
            Assert-Stage5InstalledKernelRawString $entry.runNonce `
                "$title run plan runNonce"
            Assert-Stage5InstalledKernelRawInteger $entry.workerCount `
                "$title run plan workerCount"
            Assert-Stage5InstalledKernelRawBoolean $entry.warmup `
                "$title run plan warmup"
        }
        foreach ($outcome in @($attemptManifest.outcomes)) {
            Assert-Stage5InstalledKernelRawString $outcome.entryId `
                "$title attempt outcome entryId"
            Assert-Stage5InstalledKernelRawString $outcome.state `
                "$title attempt outcome state"
            Assert-Stage5InstalledKernelRawString $outcome.startBinding.sha256 `
                "$title attempt outcome start SHA-256"
            Assert-Stage5InstalledKernelRawString $outcome.resultBinding.sha256 `
                "$title attempt outcome result SHA-256"
        }
        foreach ($validationRun in @($validation.runs)) {
            foreach ($name in @('fixtureId', 'lane', 'runId', 'runNonce',
                    'receiptPath', 'receiptSha256')) {
                Assert-Stage5InstalledKernelRawString $validationRun.$name `
                    "$title validation run $name"
            }
            Assert-Stage5InstalledKernelRawString $validationRun.host.rawLogSha256 `
                "$title validation run raw-log SHA-256"
            Assert-Stage5InstalledKernelRawString $validationRun.host.timingSha256 `
                "$title validation run timing SHA-256"
            Assert-Stage5InstalledKernelRawInteger $validationRun.ordinal `
                "$title validation run ordinal"
            Assert-Stage5InstalledKernelRawBoolean $validationRun.warmup `
                "$title validation run warmup"
        }
        foreach ($run in @($hostDocument.runs)) {
            foreach ($name in @('fixtureId', 'lane', 'runId', 'rawLogPath',
                    'rawLogSha256', 'timingPath', 'timingSha256',
                    'receiptSha256')) {
                Assert-Stage5InstalledKernelRawString $run.$name `
                    "$title host run $name"
            }
            Assert-Stage5InstalledKernelRawString $run.receiptBinding.runNonce `
                "$title host run receiptBinding runNonce"
            Assert-Stage5InstalledKernelRawInteger $run.ordinal `
                "$title host run ordinal"
            Assert-Stage5InstalledKernelRawBoolean $run.warmup `
                "$title host run warmup"
        }
        Assert-Stage5InstalledKernelRawString $proof.fixture.sha256 `
            "$title fixture SHA-256"
        Assert-Stage5InstalledKernelRawInteger $proof.fixture.seed `
            "$title fixture seed"
        Assert-Stage5InstalledKernelRawInteger $proof.fixture.playerCount `
            "$title fixture playerCount"
        Assert-Stage5InstalledKernelRawInteger `
            $proof.fixture.minimumInitialUnitCount `
            "$title fixture minimumInitialUnitCount"
        foreach ($name in @('rosterStable', 'contiguous')) {
            Assert-Stage5InstalledKernelRawBoolean $proof.fixture.$name `
                "$title fixture $name"
        }
        Assert-Stage5InstalledKernelRawString $proof.schedule.lane `
            "$title schedule lane"
        foreach ($name in @('requestedWorkers', 'warmupRuns', 'measuredRuns')) {
            Assert-Stage5InstalledKernelRawInteger $proof.schedule.$name `
                "$title schedule $name"
        }
        Assert-Stage5InstalledKernelCondition (
            $hostDocument.evidenceKind -ceq
                'stage5-installed-kernel-execution-host' -and
            $hostDocument.status -ceq 'passed' -and
            $hostDocument.title -ceq $title -and
            $hostDocument.qualificationMode -ceq
                'InstalledKernelExecution' -and
            $hostDocument.qualificationClass -ceq
                'installed-kernel-execution-only' -and
            $hostDocument.acceptanceScope -ceq 'kernel-execution-only' -and
            -not [bool]$hostDocument.finalAcceptanceClaim -and
            -not [bool]$hostDocument.performanceScalingClaim -and
            $hostDocument.sourceCommit -ceq $ExpectedSourceCommit -and
            $hostDocument.artifactSetSha256 -ceq
                $ExpectedArtifactSetSha256 -and
            $hostDocument.cohortNonce -ceq $ExpectedCohortNonce -and
            $hostDocument.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
            $hostDocument.executable.sha256 -ceq $executableSha256 -and
            $hostDocument.runtimeClosure.dependencyManifestSha256 -ceq
                $ExpectedDependencyManifestSha256 -and
            $hostDocument.runtimeClosure.closureSha256 -ceq
                $ExpectedRuntimeClosureSha256 -and
            $hostDocument.validationManifest.sha256 -ceq
                $proof.validationManifest.sha256 -and
            $hostDocument.runPlan.sha256 -ceq $proof.runPlan.sha256 -and
            $hostDocument.attemptManifest.sha256 -ceq
                $proof.attemptManifest.sha256 -and
            $hostDocument.fixtureProductionReceipt.sha256 -ceq
                $proof.fixtureProduction.hostReceipt.sha256 -and
            [int]$hostDocument.schedule.warmupRuns -eq 1 -and
            [int]$hostDocument.schedule.measuredRuns -eq
                [int]$proof.schedule.measuredRuns -and
            $null -eq $hostDocument.thresholds -and
            @($hostDocument.pairedOracleBindings).Count -eq 0) `
            "$title host aggregate is detached from its final projection."
        Assert-Stage5InstalledKernelCondition (
            $validation.title -ceq $title -and
            $validation.qualificationMode -ceq 'InstalledKernelExecution' -and
            $validation.sourceCommit -ceq $ExpectedSourceCommit -and
            $validation.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
            $validation.cohortNonce -ceq $ExpectedCohortNonce -and
            $validation.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
            $validation.executableSha256 -ceq $executableSha256 -and
            $validation.runtimeClosure.dependencyManifestSha256 -ceq
                $ExpectedDependencyManifestSha256 -and
            $validation.runtimeClosure.closureSha256 -ceq
                $ExpectedRuntimeClosureSha256 -and
            $validation.fixtureProductionReceipt.sha256 -ceq
                $proof.fixtureProduction.hostReceipt.sha256 -and
            $runPlan.qualificationMode -ceq 'InstalledKernelExecution' -and
            $runPlan.title -ceq $title -and
            $runPlan.cohortNonce -ceq $ExpectedCohortNonce -and
            $runPlan.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
            $attemptManifest.planSha256 -ceq $proof.runPlan.sha256 -and
            $null -eq $attemptManifest.cohortFailure) `
            "$title validation and execution journals are detached."

        $productionHostFile = Read-Stage5InstalledKernelBoundFile $root `
            $proof.fixtureProduction.hostReceipt `
            "$title fixture-production receipt"
        $production = Read-Stage5NativePerformanceFixtureProductionReceipt `
            -Path $productionHostFile.path `
            -ExpectedSha256 $proof.fixtureProduction.hostReceipt.sha256 `
            -ExpectedTitle $title `
            -ExpectedCohortNonce $ExpectedCohortNonce `
            -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
            -ExpectedSourceCommit $ExpectedSourceCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
            -ExpectedExecutableSha256 $executableSha256 `
            -ExpectedDependencyManifestSha256 `
                $ExpectedDependencyManifestSha256 `
            -ExpectedRuntimeClosureSha256 $ExpectedRuntimeClosureSha256
        $productionBindings = @{
            hostReceipt = $proof.fixtureProduction.hostReceipt
            rawLog = $proof.fixtureProduction.rawLog
            reviewedFixtureManifest =
                $proof.fixtureProduction.reviewedFixtureManifest
            prelaunchPlan = $proof.fixtureProduction.prelaunchPlan
            attemptStart = $proof.fixtureProduction.attemptStart
            map = $proof.fixtureProduction.map
            retainedReplay = $proof.fixtureProduction.retainedReplay
        }
        $expectedFixtureProjection =
            New-Stage5InstalledKernelFixtureProjection $production `
                $productionBindings
        Assert-Stage5InstalledKernelCondition (
            (ConvertTo-Json $expectedFixtureProjection -Depth 40 -Compress) -ceq
                (ConvertTo-Json $proof.fixtureProduction -Depth 40 -Compress)) `
            "$title fixture-production projection changed."
        $outerProductionFiles = @(
            [pscustomobject]@{ binding = $proof.fixtureProduction.rawLog
                kind = 'RawLog' },
            [pscustomobject]@{
                binding = $proof.fixtureProduction.reviewedFixtureManifest
                kind = 'JsonReceipt' },
            [pscustomobject]@{ binding = $proof.fixtureProduction.prelaunchPlan
                kind = 'JsonReceipt' },
            [pscustomobject]@{ binding = $proof.fixtureProduction.attemptStart
                kind = 'JsonReceipt' },
            [pscustomobject]@{ binding = [pscustomobject]@{
                    path = $proof.fixtureProduction.map.path
                    sha256 = $proof.fixtureProduction.map.sha256 }
                kind = 'RuntimeBinary' },
            [pscustomobject]@{ binding = [pscustomobject]@{
                    path = $proof.fixtureProduction.retainedReplay.path
                    sha256 = $proof.fixtureProduction.retainedReplay.sha256 }
                kind = 'Replay' })
        foreach ($file in $outerProductionFiles) {
            [void](Read-Stage5InstalledKernelBoundFile $root $file.binding `
                "$title fixture-production retained file" $file.kind)
        }
        Assert-Stage5InstalledKernelCondition (
            $production.fixture.sha256 -ceq $proof.fixture.sha256 -and
            [int]$production.fixture.seed -eq [int]$proof.fixture.seed -and
            [int]$proof.fixture.playerCount -eq 8 -and
            [Int64]$proof.fixture.minimumInitialUnitCount -eq
                [Int64]$production.receipt.completion.initialUnitCount -and
            [bool]$proof.fixture.rosterStable -and
            [bool]$proof.fixture.contiguous -and
            $proof.schedule.lane -ceq 'physical-4' -and
            [int]$proof.schedule.requestedWorkers -eq 4 -and
            [int]$proof.schedule.warmupRuns -eq 1) `
            "$title dense fixture or schedule projection changed."

        $validationRuns = @{}
        foreach ($run in @($validation.runs)) {
            $validationRuns[[string]$run.runId] = $run
        }
        $hostRuns = @{}
        foreach ($run in @($hostDocument.runs)) {
            $hostRuns[[string]$run.runId] = $run
        }
        $planEntries = @{}
        foreach ($entry in @($runPlan.entries)) {
            $planEntries[[string]$entry.entryId] = $entry
        }
        $outcomes = @{}
        foreach ($outcome in @($attemptManifest.outcomes)) {
            $outcomes[[string]$outcome.entryId] = $outcome
        }
        Assert-Stage5InstalledKernelCondition (
            $validationRuns.Count -eq (1 + [int]$proof.schedule.measuredRuns) -and
            $hostRuns.Count -eq $validationRuns.Count -and
            $planEntries.Count -eq $validationRuns.Count -and
            $outcomes.Count -eq $validationRuns.Count) `
            "$title warmup/measured journal coverage changed."

        [Int64]$minimumUnits = [Int64]::MaxValue
        [Int64]$maximumUnits = 0
        $seenRunIds = @{}
        foreach ($projectedRun in @($proof.runs)) {
            $runId = [string]$projectedRun.runId
            Assert-Stage5InstalledKernelCondition (-not
                $seenRunIds.ContainsKey($runId) -and
                $validationRuns.ContainsKey($runId) -and
                $hostRuns.ContainsKey($runId) -and
                $planEntries.ContainsKey($runId) -and
                $outcomes.ContainsKey($runId)) `
                "$title projected run is duplicated or detached."
            $seenRunIds[$runId] = $true
            $validationRun = $validationRuns[$runId]
            $hostRun = $hostRuns[$runId]
            $entry = $planEntries[$runId]
            $outcome = $outcomes[$runId]
            Assert-Stage5InstalledKernelCondition (
                -not [bool]$validationRun.warmup -and
                -not [bool]$hostRun.warmup -and
                -not [bool]$entry.warmup -and
                $validationRun.runNonce -ceq $projectedRun.runNonce -and
                $hostRun.receiptBinding.runNonce -ceq
                    $projectedRun.runNonce -and
                $entry.runNonce -ceq $projectedRun.runNonce -and
                [int]$entry.workerCount -eq 4 -and
                $entry.measurementRole -ceq 'throughput' -and
                $outcome.state -ceq 'completed' -and
                $outcome.startBinding.sha256 -ceq
                    $projectedRun.attemptStart.sha256 -and
                $outcome.resultBinding.sha256 -ceq
                    $projectedRun.attemptResult.sha256 -and
                $validationRun.receiptSha256 -ceq
                    $projectedRun.receipt.sha256 -and
                $validationRun.host.rawLogSha256 -ceq
                    $projectedRun.rawLog.sha256 -and
                $validationRun.host.timingSha256 -ceq
                    $projectedRun.timing.sha256 -and
                $hostRun.receiptSha256 -ceq $projectedRun.receipt.sha256) `
                "$title projected run differs from its original journals."
            $receiptFile = Read-Stage5InstalledKernelBoundFile $root `
                $projectedRun.receipt "$title run '$runId' receipt"
            $rawFile = Read-Stage5InstalledKernelBoundFile $root `
                $projectedRun.rawLog "$title run '$runId' raw log" RawLog
            $timingFile = Read-Stage5InstalledKernelBoundFile $root `
                $projectedRun.timing "$title run '$runId' timing" RawLog
            $startFile = Read-Stage5InstalledKernelBoundFile $root `
                $projectedRun.attemptStart "$title run '$runId' start"
            $resultFile = Read-Stage5InstalledKernelBoundFile $root `
                $projectedRun.attemptResult "$title run '$runId' result"
            $expected = [pscustomobject]@{
                title = $title
                sourceCommit = $ExpectedSourceCommit
                artifactSetSha256 = $ExpectedArtifactSetSha256
                executableSha256 = $executableSha256
                cohortNonce = $ExpectedCohortNonce
                cohortCreatedUtc = $ExpectedCohortCreatedUtc
                dependencyManifestSha256 = $ExpectedDependencyManifestSha256
                runtimeClosureSha256 = $ExpectedRuntimeClosureSha256
                fixtureSha256 = $production.fixture.sha256
                fixtureSeed = $production.fixture.seed
                runPlanSha256 = $proof.runPlan.sha256
            }
            $converted = ConvertTo-Stage5InstalledKernelRunProjection `
                $receiptFile.snapshot $rawFile.snapshot $timingFile.snapshot `
                $startFile.snapshot $resultFile.snapshot $validationRun `
                $expected @{
                    receipt = $projectedRun.receipt
                    rawLog = $projectedRun.rawLog
                    timing = $projectedRun.timing
                    attemptStart = $projectedRun.attemptStart
                    attemptResult = $projectedRun.attemptResult
                }
            Assert-Stage5InstalledKernelCondition (
                (ConvertTo-Json $converted.projection -Depth 40 -Compress) -ceq
                    (ConvertTo-Json $projectedRun -Depth 40 -Compress)) `
                "$title run '$runId' authority projection changed."
            $minimumUnits = [Math]::Min($minimumUnits,
                [Int64]$converted.workloadMinimumUnitCount)
            $maximumUnits = [Math]::Max($maximumUnits,
                [Int64]$converted.workloadMaximumUnitCount)
        }
        Assert-Stage5InstalledKernelCondition (
            $seenRunIds.Count -eq [int]$proof.schedule.measuredRuns -and
            [Int64]$proof.fixture.minimumObservedUnitCount -eq $minimumUnits -and
            [Int64]$proof.fixture.maximumObservedUnitCount -eq $maximumUnits) `
            "$title projected dense workload coverage is incomplete."
        $validatedTitles.Add([pscustomobject]@{
            title = $title
            executableSha256 = $executableSha256
            measuredRuns = $seenRunIds.Count
            fixtureSha256 = [string]$proof.fixture.sha256
        }) | Out-Null
    }
    return [pscustomobject]@{
        path = $full
        sha256 = [string]$snapshot.sha256
        document = $document
        titles = $validatedTitles.ToArray()
        acceptanceScope = 'kernel-execution-only'
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
    }
}

Export-ModuleMember -Function New-Stage5InstalledKernelExecutionEvidence, `
    Read-Stage5InstalledKernelExecutionEvidence
