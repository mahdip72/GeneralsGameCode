Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Stage5Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function ConvertFrom-Stage5JsonTextDictionary {
    param([string]$Json)
    $json = [string]$Json
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $convertFromJson = Get-Command ConvertFrom-Json
        if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
            return $json | ConvertFrom-Json -AsHashtable -DateKind String
        }
        return $json | ConvertFrom-Json -AsHashtable
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = 10485760
    return $serializer.DeserializeObject($json)
}

function ConvertFrom-Stage5JsonDictionary {
    param([string]$Path)
    return ConvertFrom-Stage5JsonTextDictionary (Get-Content -LiteralPath $Path -Raw)
}

function Get-Stage5JsonValue {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-Stage5Condition ($Object -is [Collections.IDictionary]) "$Context must be a JSON object."
    $keys = @($Object.Keys | Where-Object { [string]$_ -ceq $Name })
    Assert-Stage5Condition ($keys.Count -eq 1) "$Context is missing property '$Name'."
    $value = $Object[$keys[0]]
    if ($value -is [Array]) { return ,$value }
    return $value
}

function Assert-Stage5JsonShape {
    param([object]$Object, [string[]]$Names, [string]$Context)
    Assert-Stage5Condition ($Object -is [Collections.IDictionary]) "$Context must be a JSON object."
    foreach ($name in $Names) { Get-Stage5JsonValue $Object $name $Context | Out-Null }
    foreach ($key in $Object.Keys) {
        Assert-Stage5Condition ($Names -ccontains [string]$key) `
            "$Context contains unsupported property '$key'."
    }
}

function Assert-Stage5JsonProperties {
    param([object]$Object, [string[]]$Names, [string]$Context)
    Assert-Stage5Condition ($Object -is [Collections.IDictionary]) "$Context must be a JSON object."
    foreach ($name in $Names) { Get-Stage5JsonValue $Object $name $Context | Out-Null }
}

function Assert-Stage5NativeFixtureObservation {
    param([object]$Document, [string]$Context, [switch]$RequireScaling)
    $fixture = $Document.fixture
    Assert-Stage5JsonProperties $fixture @('kind','workloadQualification','contentPath','contentSha256',
        'identityObserved','replayPath','retainedReplayPath','retainedReplaySha256','seed','seedKnown',
        'requestedPlayerCount','requestedMinimumUnitCount') "$Context fixture"
    Assert-Stage5Condition (@('replay','fresh-ai-map') -ccontains $fixture.kind -and
        @('minimum-qualified','observed-only') -ccontains $fixture.workloadQualification -and
        $fixture.contentPath -is [string] -and -not [string]::IsNullOrWhiteSpace($fixture.contentPath) -and
        $fixture.contentSha256 -is [string] -and $fixture.contentSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $fixture.identityObserved -is [bool] -and $fixture.identityObserved -and
        $fixture.seedKnown -is [bool] -and $fixture.seedKnown -and
        (Test-Stage5JsonInteger $fixture.seed) -and $fixture.seed -ge 0 -and $fixture.seed -le [UInt32]::MaxValue) `
        "$Context fixture kind, qualification, observed content identity or seed is invalid."
    if ($fixture.kind -ceq 'replay') {
        Assert-Stage5Condition ($fixture.replayPath -is [string] -and $fixture.replayPath -ceq $fixture.contentPath -and
            $fixture.retainedReplayPath -ceq '' -and $fixture.retainedReplaySha256 -ceq '') "$Context replay content path is inconsistent."
    } else {
        Assert-Stage5Condition ($fixture.workloadQualification -ceq 'observed-only' -and $fixture.replayPath -ceq '' -and
            $fixture.retainedReplayPath -is [string] -and -not [string]::IsNullOrWhiteSpace($fixture.retainedReplayPath) -and
            $fixture.retainedReplaySha256 -is [string] -and $fixture.retainedReplaySha256 -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context fresh-AI map and closed retained replay identities are not separate and complete."
    }
    if ($fixture.workloadQualification -ceq 'minimum-qualified') {
        Assert-Stage5DiagnosticCounter $fixture.requestedPlayerCount "$Context requested players"
        Assert-Stage5DiagnosticCounter $fixture.requestedMinimumUnitCount "$Context requested units"
        Assert-Stage5Condition ($fixture.requestedPlayerCount -gt 0 -and $fixture.requestedMinimumUnitCount -gt 0 -and
            $Document.workload.playerCount -eq $fixture.requestedPlayerCount -and
            $Document.workload.initialUnitCount -ge $fixture.requestedMinimumUnitCount) "$Context qualified workload is below its requested minimum."
    } else {
        Assert-Stage5Condition ($null -eq $fixture.requestedPlayerCount -and $null -eq $fixture.requestedMinimumUnitCount) `
            "$Context observed-only workload must not invent requested minima."
    }
    if ($RequireScaling) {
        Assert-Stage5Condition ($fixture.kind -ceq 'replay' -and $fixture.workloadQualification -ceq 'minimum-qualified' -and
            $Document.simulationMode -ceq 'parallel' -and $Document.schedulerStarted -is [bool] -and $Document.schedulerStarted) `
            "$Context scaling requires minimum-qualified parallel replay evidence with a started scheduler."
    }
}

function Assert-Stage5NativeSchedulerObservation {
    param([object]$Document, [string]$Context)
    Assert-Stage5Condition (@('serial','parallel','shadow') -ccontains $Document.simulationMode -and
        $Document.schedulerStarted -is [bool]) "$Context actual simulation mode or scheduler observation is missing."
    $worker = $Document.worker; $topology = $Document.topology
    Assert-Stage5JsonShape $worker @('requestedCount','effectiveCount','policy','pinned','availableLogicalCpuCount',
        'reservedOwnerCpuCount','selectedWorkerCpuCount','selectedWorkerPhysicalCoreCount','selectedWorkerPhysicalCoreMask',
        'selectedWorkerPhysicalCoreMaskComplete') "$Context worker"
    foreach ($field in @('requestedCount','effectiveCount','availableLogicalCpuCount','reservedOwnerCpuCount',
        'selectedWorkerCpuCount','selectedWorkerPhysicalCoreCount','selectedWorkerPhysicalCoreMask')) {
        Assert-Stage5DiagnosticCounter $worker[$field] "$Context worker $field"
    }
    Assert-Stage5JsonShape $topology @('source','cpuSets','ownerCpuSetIds','selectedWorkerCpuSetIds') "$Context topology"
    Assert-Stage5Condition (@('auto','all') -ccontains $worker.policy -and $worker.pinned -is [bool] -and
        $worker.selectedWorkerPhysicalCoreMaskComplete -is [bool] -and $topology.cpuSets -is [Array] -and
        $topology.ownerCpuSetIds -is [Array] -and $topology.selectedWorkerCpuSetIds -is [Array]) "$Context scheduler topology types are invalid."
    if (-not $Document.schedulerStarted) {
        Assert-Stage5Condition ($Document.simulationMode -ceq 'serial' -and $Document.fixture.workloadQualification -ceq 'observed-only' -and
            -not $worker.pinned -and -not $worker.selectedWorkerPhysicalCoreMaskComplete -and $worker.effectiveCount -eq 0 -and
            $worker.availableLogicalCpuCount -eq 0 -and $worker.reservedOwnerCpuCount -eq 0 -and $worker.selectedWorkerCpuCount -eq 0 -and
            $worker.selectedWorkerPhysicalCoreCount -eq 0 -and $worker.selectedWorkerPhysicalCoreMask -eq 0 -and
            $topology.source -ceq 'scheduler-not-started' -and $topology.cpuSets.Count -eq 0 -and
            $topology.ownerCpuSetIds.Count -eq 0 -and $topology.selectedWorkerCpuSetIds.Count -eq 0) `
            "$Context absent scheduler cannot claim selected workers or physical topology."
        return
    }
    Assert-Stage5Condition ($worker.pinned -and $worker.selectedWorkerPhysicalCoreMaskComplete -and $worker.effectiveCount -gt 0 -and
        $worker.effectiveCount -eq $worker.selectedWorkerCpuCount -and $worker.selectedWorkerCpuCount -eq $worker.selectedWorkerPhysicalCoreCount -and
        $worker.availableLogicalCpuCount -ge $worker.selectedWorkerCpuCount -and $worker.selectedWorkerPhysicalCoreMask -gt 0 -and
        $topology.source -ceq 'GetSystemCpuSetInformation' -and $topology.cpuSets.Count -gt 0 -and
        $topology.selectedWorkerCpuSetIds.Count -eq $worker.selectedWorkerCpuCount -and
        $topology.ownerCpuSetIds.Count -eq $worker.reservedOwnerCpuCount) "$Context started scheduler lacks complete selected topology."
    $cpuSets = @{}; $selected = @{}; $cores = @{}; $owners = @{}
    foreach ($cpu in $topology.cpuSets) {
        Assert-Stage5JsonShape $cpu @('id','efficiencyClass','group','coreIndex','logicalProcessorIndex','parked',
            'allocatedToOtherProcess','availableToProcess') "$Context CPU set"
        foreach ($field in @('id','efficiencyClass','group','coreIndex','logicalProcessorIndex')) {
            Assert-Stage5DiagnosticCounter $cpu[$field] "$Context CPU set $field"
        }
        Assert-Stage5Condition ($cpu.parked -is [bool] -and $cpu.allocatedToOtherProcess -is [bool] -and
            $cpu.availableToProcess -is [bool] -and -not $cpuSets.ContainsKey([string]$cpu.id)) "$Context CPU set is duplicated or malformed."
        $cpuSets[[string]$cpu.id] = $cpu
    }
    foreach ($id in $topology.selectedWorkerCpuSetIds) {
        Assert-Stage5DiagnosticCounter $id "$Context selected CPU set id"
        Assert-Stage5Condition ($cpuSets.ContainsKey([string]$id) -and -not $selected.ContainsKey([string]$id)) "$Context selected CPU set is absent or duplicated."
        $cpu=$cpuSets[[string]$id]; $core="$($cpu.group)|$($cpu.coreIndex)"
        Assert-Stage5Condition ($cpu.availableToProcess -and -not $cpu.parked -and -not $cpu.allocatedToOtherProcess -and
            -not $cores.ContainsKey($core)) "$Context selected physical core is unavailable or shared."
        $selected[[string]$id]=$true; $cores[$core]=$true
    }
    foreach ($id in $topology.ownerCpuSetIds) {
        Assert-Stage5DiagnosticCounter $id "$Context owner CPU set id"
        Assert-Stage5Condition ($cpuSets.ContainsKey([string]$id) -and -not $owners.ContainsKey([string]$id)) "$Context owner CPU set is absent or duplicated."
        $cpu=$cpuSets[[string]$id]
        Assert-Stage5Condition ($cpu.availableToProcess -and -not $cpu.parked -and -not $cpu.allocatedToOtherProcess) "$Context owner CPU set is unavailable."
        $owners[[string]$id]=$true
    }
}

function Assert-Stage5NativeRawPathText {
    param([string]$Path, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is empty."
    Assert-Stage5Condition ($Path -notmatch '(^|[\\/])\.\.([\\/]|$)') `
        "$Context path contains parent traversal."
    if ([IO.Path]::IsPathRooted($Path)) {
        $afterDrive = if ($Path -match '^[A-Za-z]:') { $Path.Substring(2) } else { $Path }
        Assert-Stage5Condition ($Path -notmatch '^[A-Za-z]:[^\\/]' -and
            $afterDrive -notmatch ':') `
            "$Context path is drive-relative or names an alternate data stream."
    }
    else {
        Assert-Stage5Condition ($Path -notmatch '^[A-Za-z]:' -and
            $Path -notmatch ':') `
            "$Context path is drive-relative or names an alternate data stream."
    }
}

function Assert-Stage5NativePerformanceReceiptProvenance {
    param(
        [object]$Document,
        [string]$Context,
        [string]$ExpectedTitle,
        [string]$ExpectedExecutablePath,
        [string]$ExpectedExecutableSha256,
        [int]$ExpectedProcessId,
        [string]$ExpectedProcessCreationUtc,
        [string]$ExpectedCohortCreatedUtc,
        [string]$ExpectedReceiptPath,
        [string]$ExpectedEvidenceDirectory = '',
        [object]$NativeRawBindings = $null,
        [string]$NativeReceiptSourcePath = ''
    )
    Assert-Stage5JsonProperties $Document @('schemaVersion','producer','producerVersion',
        'evidenceKind','status','measurementRole','frames','fixture','workload',
        'frameSimulation','phases','kernelTiming','kernelReference','rawEvidence','simulationMode',
        'schedulerStarted','worker','topology') $Context
    $documentProducer = $Document.producer
    $documentProducerVersion = $Document.producerVersion
    $documentStatus = $Document.status
    $documentEvidenceKind = $Document.evidenceKind
    $documentMeasurementRole = $Document.measurementRole
    $documentRole = Get-Stage5JsonValue $Document 'role' $Context
    $documentTitle = Get-Stage5JsonValue $Document 'title' $Context
    $documentArchitecture = Get-Stage5JsonValue $Document 'architecture' $Context
    Assert-Stage5Condition ($documentProducer -is [string] -and
        $documentProducerVersion -is [string] -and $documentStatus -is [string] -and
        $documentEvidenceKind -is [string] -and
        $documentMeasurementRole -is [string] -and $documentRole -is [string] -and
        $documentTitle -is [string] -and $documentArchitecture -is [string]) `
        "$Context native receipt identity fields must be JSON strings."
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Document.schemaVersion) -and
        $Document.schemaVersion -eq 5 -and
        $documentProducer -ceq 'game-executable-stage5-performance-report-v5' -and
        $documentProducerVersion -ceq '5' -and $documentStatus -ceq 'passed' -and
        $documentEvidenceKind -ceq 'stage5-executable-originated-receipt') `
        "$Context requires the current V5 native producer/version, not obsolete metadata."
    Assert-Stage5Condition ($documentMeasurementRole -ceq 'throughput') `
        "$Context oracle or unknown measurement role is not normal validation throughput evidence."
    Assert-Stage5Condition ($documentRole -ceq 'performance-report' -and
        $documentTitle -ceq $ExpectedTitle -and
        $documentArchitecture -ceq 'x64') `
        "$Context role/title/architecture is not bound to the installed run."
    $frames = $Document.frames; $workload = $Document.workload
    foreach ($field in @('start','end','final','finalCrc')) {
        Assert-Stage5DiagnosticCounter $frames[$field] "$Context frame $field"
        Assert-Stage5Condition ($frames[$field] -le [UInt32]::MaxValue) "$Context frame exceeds native storage."
    }
    foreach ($field in @('sampleCount','firstFrame','lastFrame','playerCount',
        'initialUnitCount','minimumUnitCount','peakUnitCount')) {
        Assert-Stage5DiagnosticCounter $workload[$field] "$Context workload $field"
    }
    Assert-Stage5NativeFixtureObservation $Document $Context
    Assert-Stage5NativeSchedulerObservation $Document $Context
    $workloadSampling = $workload.sampling
    Assert-Stage5Condition ($workloadSampling -is [string] -and
        $frames.end -gt $frames.start -and $frames.final -eq $frames.end -and
        $frames.finalCrcKnown -is [bool] -and $frames.finalCrcKnown -and
        $workloadSampling -ceq 'completed-simulation-frame-boundary-v1' -and
        $workload.rosterStable -is [bool] -and $workload.rosterStable -and
        $workload.contiguous -is [bool] -and $workload.contiguous -and
        $workload.playerCount -gt 0 -and
        $workload.firstFrame -eq ([decimal]$frames.start + 1) -and $workload.lastFrame -eq $frames.end -and
        [decimal]$workload.sampleCount -eq ([decimal]$frames.end - [decimal]$frames.start) -and
        $workload.minimumUnitCount -le $workload.initialUnitCount -and
        $workload.peakUnitCount -ge $workload.initialUnitCount) "$Context completed-frame workload is inconsistent."
    $frameTiming = $Document.frameSimulation
    foreach ($field in @('totalNanoseconds','maximumNanoseconds','sampleCount')) {
        Assert-Stage5DiagnosticCounter $frameTiming[$field] "$Context frame timing $field"
    }
    Assert-Stage5Condition ($frameTiming.totalNanoseconds -gt 0 -and $frameTiming.maximumNanoseconds -gt 0 -and
        $frameTiming.maximumNanoseconds -le $frameTiming.totalNanoseconds -and
        $frameTiming.sampleCount -ge $workload.sampleCount -and
        $Document.phases -is [Array] -and $Document.phases.Count -eq 5) "$Context frame/phase timing is incomplete."
    $phaseNames = @('owner-intake','legacy-mutable-island','spatial-work','owner-tail','verification-publication')
    [decimal]$phaseTotal = 0
    for ($index = 0; $index -lt 5; ++$index) {
        $phase = $Document.phases[$index]
        foreach ($field in @('totalNanoseconds','maximumNanoseconds','sampleCount','serialNanoseconds')) {
            Assert-Stage5DiagnosticCounter $phase[$field] "$Context phase $field"
        }
        $phaseName = $phase.name
        Assert-Stage5Condition ($phaseName -is [string] -and
            $phaseName -ceq $phaseNames[$index] -and $phase.available -is [bool] -and
            $phase.serialNanosecondsKnown -is [bool] -and $phase.maximumNanoseconds -le $phase.totalNanoseconds -and
            $phase.sampleCount -le $frameTiming.sampleCount -and
            (($phase.serialNanosecondsKnown -and $phase.available -and $phase.serialNanoseconds -le $phase.totalNanoseconds) -or
                (-not $phase.serialNanosecondsKnown -and $phase.serialNanoseconds -eq 0)) -and
            (($phase.available -and $phase.totalNanoseconds -gt 0 -and $phase.sampleCount -gt 0) -or
                (-not $phase.available -and $phase.totalNanoseconds -eq 0 -and $phase.sampleCount -eq 0))) `
            "$Context phase timing fabricates or loses measured/unknown data."
        $phaseTotal += [decimal]$phase.totalNanoseconds
    }
    Assert-Stage5Condition ($phaseTotal -le [decimal]$frameTiming.totalNanoseconds) "$Context phase timing overlaps frame time."
    Assert-Stage5DiagnosticKernelTiming $Document.kernelTiming $frames `
        $Document.measurementRole "$Context kernel timing"
    Assert-Stage5DiagnosticKernelReference $Document.kernelReference $Document.kernelTiming `
        $frames $Document.measurementRole "$Context kernel reference"
    $rawEvidence = $Document.rawEvidence
    foreach ($field in @('timingSessionCount','timingFrameSamples','timingFirstFrame','timingLastFrame')) {
        Assert-Stage5DiagnosticCounter $rawEvidence[$field] "$Context raw timing $field"
    }
    Assert-Stage5Condition ($rawEvidence.timingClosed -is [bool] -and $rawEvidence.timingClosed -and
        $rawEvidence.timingWriteSucceeded -is [bool] -and $rawEvidence.timingWriteSucceeded -and
        $rawEvidence.timingTruncated -is [bool] -and -not $rawEvidence.timingTruncated -and
        $rawEvidence.timingComplete -is [bool] -and $rawEvidence.timingComplete -and
        $rawEvidence.timingSessionCount -eq 1 -and $rawEvidence.timingFrameSamples -ge $workload.sampleCount -and
        $rawEvidence.timingFirstFrame -le $frames.start -and $rawEvidence.timingLastFrame -ge $frames.end) `
        "$Context raw timing is not finalized with complete replay coverage."
    # Accounting completeness is not six-kernel coverage or scaling acceptance.
    # Empty frozen ledgers and unknown phase serial portions remain explicit.
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    $documentCohortCreatedUtc = Get-Stage5JsonValue $Document 'cohortCreatedUtc' $Context
    $documentRecordedUtc = Get-Stage5JsonValue $Document 'recordedUtc' $Context
    Assert-Stage5Condition ($documentCohortCreatedUtc -is [string] -and
        $documentRecordedUtc -is [string] -and
        [DateTimeOffset]::TryParse(
        $documentCohortCreatedUtc,
        [ref]$cohortCreated) -and
        [DateTimeOffset]::TryParse(
        $documentRecordedUtc,
        [ref]$recorded) -and $recorded -ge $cohortCreated) `
        "$Context recordedUtc/cohortCreatedUtc is invalid or stale."
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortCreatedUtc)) {
        [DateTimeOffset]$expectedCohortCreated = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
            [ref]$expectedCohortCreated) -and $cohortCreated -eq $expectedCohortCreated) `
            "$Context cohortCreatedUtc is detached from the requested execution cohort."
    }
    $rawLogs = Get-Stage5JsonValue $Document 'rawLogs' $Context
    Assert-Stage5Condition ($rawLogs -is [Array] -and $rawLogs.Count -eq 2) `
        "$Context rawLogs must contain exactly raw-log and timing observations."
    $seenRawNames = @{}
    $nativeReceiptDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($ExpectedReceiptPath))
    $evidenceDirectory = if ([string]::IsNullOrWhiteSpace($ExpectedEvidenceDirectory)) {
        $nativeReceiptDirectory
    }
    else { [IO.Path]::GetFullPath($ExpectedEvidenceDirectory) }
    if ($null -ne $NativeRawBindings) {
        Assert-Stage5Condition ($NativeRawBindings -is [Array]) `
            "$Context native raw bindings must be an array."
        $bindingNames = @{}
        foreach ($binding in $NativeRawBindings) {
            Assert-Stage5JsonShape $binding @('name', 'sourcePath', 'path', 'sha256') `
                "$Context native raw binding"
            $bindingName = Get-Stage5JsonValue $binding 'name' `
                "$Context native raw binding"
            Assert-Stage5Condition ($bindingName -is [string] -and
                ($bindingName -ceq 'raw-log' -or
                $bindingName -ceq 'timing') -and
                -not $bindingNames.ContainsKey($bindingName)) `
                "$Context native raw bindings contain an invalid or duplicate name."
            $bindingNames[$bindingName] = $true
        }
        Assert-Stage5Condition ($bindingNames.Count -eq $rawLogs.Count) `
            "$Context native raw bindings contain an unexpected number of entries."
    }
    foreach ($rawLog in $rawLogs) {
        Assert-Stage5JsonShape $rawLog @('name', 'path', 'sha256') "$Context raw log"
        $rawName = Get-Stage5JsonValue $rawLog 'name' "$Context raw log"
        $rawPathText = Get-Stage5JsonValue $rawLog 'path' "$Context raw log"
        $rawExpectedHash = Get-Stage5JsonValue $rawLog 'sha256' "$Context raw log"
        Assert-Stage5Condition ($rawName -is [string] -and
            $rawPathText -is [string] -and $rawExpectedHash -is [string] -and
            $rawExpectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context raw log name, path, and hash must be JSON strings."
        $prefix = if ($rawName -ceq 'raw-log') { 'rawLog' } else { 'timing' }
        $rawObservedPath = $rawEvidence[$prefix + 'Path']
        $rawObservedHash = $rawEvidence[$prefix + 'Sha256']
        Assert-Stage5Condition ($rawObservedPath -is [string] -and
            $rawObservedHash -is [string] -and
            $rawObservedPath -ceq $rawPathText -and
            $rawObservedHash.ToUpperInvariant() -ceq $rawExpectedHash.ToUpperInvariant()) `
            "$Context raw timing/log identity is detached from executable close evidence."
        Assert-Stage5Condition (($rawName -ceq 'raw-log' -or $rawName -ceq 'timing') -and
            -not $seenRawNames.ContainsKey($rawName) -and
            -not [string]::IsNullOrWhiteSpace($rawPathText) -and
            $rawExpectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context raw log is missing an executable-observed path/hash binding."
        $rawPath = $null
        if ($null -ne $NativeRawBindings) {
            $matchingBindings = @($NativeRawBindings | Where-Object {
                $_ -is [Collections.IDictionary] -and
                @($_.Keys | Where-Object { [string]$_ -ceq 'name' }).Count -eq 1 -and
                $_.name -is [string] -and $_.name -ceq $rawName
            })
            Assert-Stage5Condition ($matchingBindings.Count -eq 1) `
                "$Context native raw log '$rawName' must have exactly one staged binding."
            $binding = $matchingBindings[0]
            Assert-Stage5JsonShape $binding @('name', 'sourcePath', 'path', 'sha256') `
                "$Context native raw log '$rawName' staged binding"
            $bindingName = Get-Stage5JsonValue $binding 'name' `
                "$Context native raw log '$rawName' staged binding"
            $bindingSourcePath = Get-Stage5JsonValue $binding 'sourcePath' `
                "$Context native raw log '$rawName' staged binding"
            $bindingPath = Get-Stage5JsonValue $binding 'path' `
                "$Context native raw log '$rawName' staged binding"
            $bindingHash = Get-Stage5JsonValue $binding 'sha256' `
                "$Context native raw log '$rawName' staged binding"
            Assert-Stage5Condition ($bindingName -is [string] -and
                $bindingSourcePath -is [string] -and $bindingPath -is [string] -and
                $bindingHash -is [string] -and $bindingHash -cmatch '^[0-9A-Fa-f]{64}$' -and
                $bindingName -ceq $rawName -and
                $bindingSourcePath -ceq $rawPathText -and
                $bindingHash.ToUpperInvariant() -ceq $rawExpectedHash.ToUpperInvariant()) `
                "$Context native raw log '$rawName' staged binding is detached from the executable receipt."
            Assert-Stage5NativeRawPathText $bindingSourcePath `
                "$Context native raw log '$rawName' source path"
            Assert-Stage5NativeRawPathText $bindingPath `
                "$Context native raw log '$rawName' staged path"
            $rawPath = Resolve-Stage5FinalAcceptanceFile $evidenceDirectory `
                $bindingPath "$Context native raw log '$rawName' staged path"
        }
        elseif ([IO.Path]::IsPathRooted($rawPathText)) {
            $rawPath = [IO.Path]::GetFullPath($rawPathText)
        }
        else {
            # Native receipts normally serialize absolute paths.  Relative
            # paths are accepted only for an immutable staged copy, and are
            # resolved beside the native receipt first so combined receipts
            # remain self-contained after source staging.
            try {
                $rawPath = Resolve-Stage5FinalAcceptanceFile $nativeReceiptDirectory `
                    $rawPathText "$Context raw log '$rawName'"
            }
            catch {
                $rawPath = Resolve-Stage5FinalAcceptanceFile $evidenceDirectory `
                    $rawPathText "$Context raw log '$rawName'"
            }
        }
        Assert-Stage5Condition ([IO.Path]::GetFullPath($rawPath) -ceq $rawPath) `
            "$Context raw log '$rawName' path could not be canonicalized."
        Assert-Stage5FinalAcceptancePathContained $evidenceDirectory $rawPath `
            "$Context raw log '$rawName'"
        $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
            "$Context raw log '$rawName'" -EvidenceKind RawLog
        Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot $rawExpectedHash `
            "$Context raw log '$rawName'" | Out-Null
        $seenRawNames[$rawName] = $true
    }
    Assert-Stage5Condition ($seenRawNames.ContainsKey('raw-log') -and
        $seenRawNames.ContainsKey('timing')) "$Context rawLogs are incomplete."
    $provenance = Get-Stage5JsonValue $Document 'provenance' $Context
    Assert-Stage5JsonShape $provenance @('kind', 'receiptPath', 'processId',
        'processCreationUtc', 'executablePath', 'executableSha256',
        'commandLine', 'exitCode') "$Context native provenance"
    $nativeProcessId = Get-Stage5JsonValue $provenance 'processId' $Context
    $nativeExitCode = Get-Stage5JsonValue $provenance 'exitCode' $Context
    Assert-Stage5Condition (Test-Stage5JsonInteger $nativeProcessId) `
        "$Context native provenance processId must be an integer."
    Assert-Stage5Condition (Test-Stage5JsonInteger $nativeExitCode) `
        "$Context native provenance exitCode must be an integer."
    $provenanceKind = Get-Stage5JsonValue $provenance 'kind' $Context
    $provenanceProcessCreationUtc = Get-Stage5JsonValue $provenance `
        'processCreationUtc' $Context
    $provenanceExecutablePath = Get-Stage5JsonValue $provenance `
        'executablePath' $Context
    $provenanceExecutableSha256 = Get-Stage5JsonValue $provenance `
        'executableSha256' $Context
    $provenanceCommandLine = Get-Stage5JsonValue $provenance 'commandLine' $Context
    $provenanceReceiptPathText = Get-Stage5JsonValue $provenance 'receiptPath' $Context
    Assert-Stage5Condition ($provenanceKind -is [string] -and
        $provenanceProcessCreationUtc -is [string] -and
        $provenanceExecutablePath -is [string] -and
        $provenanceExecutableSha256 -is [string] -and
        $provenanceCommandLine -is [string] -and
        $provenanceReceiptPathText -is [string] -and
        $provenanceExecutableSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
        "$Context native provenance identity fields must be JSON strings."
    $nativeProcessCreation = [DateTimeOffset]::MinValue
    $expectedProcessCreation = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse(
        $provenanceProcessCreationUtc,
        [ref]$nativeProcessCreation) -and
        [DateTimeOffset]::TryParse($ExpectedProcessCreationUtc,
        [ref]$expectedProcessCreation) -and
        $nativeProcessCreation.UtcTicks -eq $expectedProcessCreation.UtcTicks -and
        [Int64]$nativeProcessId -eq $ExpectedProcessId -and
        [IO.Path]::GetFullPath($provenanceExecutablePath) -ceq
            [IO.Path]::GetFullPath($ExpectedExecutablePath) -and
        $provenanceExecutableSha256.ToUpperInvariant() -ceq
            $ExpectedExecutableSha256.ToUpperInvariant() -and
        -not [string]::IsNullOrWhiteSpace($provenanceCommandLine) -and
        [Int64]$nativeExitCode -eq 0) `
        "$Context native process provenance is stale, substituted, or detached."
    if (-not [string]::IsNullOrWhiteSpace($NativeReceiptSourcePath)) {
        Assert-Stage5NativeRawPathText $NativeReceiptSourcePath `
            "$Context native provenance source receiptPath"
        Assert-Stage5Condition ($provenanceReceiptPathText -ceq $NativeReceiptSourcePath) `
            "$Context native provenance receiptPath is detached from the source native receipt binding."
    }
    $provenanceReceiptPath = if (-not [string]::IsNullOrWhiteSpace($NativeReceiptSourcePath)) {
        # The executable receipt is immutable and therefore retains the
        # original provenance path.  Combined staging binds that original
        # path above, then validates the staged native receipt at the caller's
        # expected path.
        [IO.Path]::GetFullPath($ExpectedReceiptPath)
    }
    elseif ([IO.Path]::IsPathRooted($provenanceReceiptPathText)) {
        [IO.Path]::GetFullPath($provenanceReceiptPathText)
    }
    else {
        Resolve-Stage5FinalAcceptanceFile $nativeReceiptDirectory `
            $provenanceReceiptPathText "$Context native provenance receiptPath"
    }
    Assert-Stage5Condition ($provenanceReceiptPath -ceq
        [IO.Path]::GetFullPath($ExpectedReceiptPath)) `
        "$Context native provenance receipt path is detached from the immutable receipt."
}

function Test-Stage5JsonInteger {
    param([object]$Value)
    return $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}

function Test-Stage5JsonNumber {
    param([object]$Value)
    if ((Test-Stage5JsonInteger $Value) -or $Value -is [decimal]) { return $true }
    if ($Value -is [single]) {
        return -not [single]::IsNaN($Value) -and -not [single]::IsInfinity($Value)
    }
    if ($Value -is [double]) {
        return -not [double]::IsNaN($Value) -and -not [double]::IsInfinity($Value)
    }
    return $false
}

function ConvertFrom-Stage5MetricLine {
    param([string]$Line, [string]$Prefix, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($Line)) "$Context is missing."
    Assert-Stage5Condition ($Line.StartsWith($Prefix + ' ', [StringComparison]::Ordinal)) `
        "$Context does not start with '$Prefix'."
    $fields = @{}
    $matches = [regex]::Matches($Line.Substring($Prefix.Length + 1),
        '(?<name>[A-Za-z_][A-Za-z0-9_]*)=(?:"(?<quoted>[^"]*)"|(?<plain>[^\s]+))')
    foreach ($match in $matches) {
        $name = $match.Groups['name'].Value
        Assert-Stage5Condition (-not $fields.ContainsKey($name)) "$Context repeats field '$name'."
        $fields[$name] = if ($match.Groups['quoted'].Success) {
            $match.Groups['quoted'].Value
        }
        else {
            $match.Groups['plain'].Value
        }
    }
    return $fields
}

function Get-Stage5RequiredField {
    param([hashtable]$Fields, [string]$Name, [string]$Context)
    Assert-Stage5Condition ($Fields.ContainsKey($Name)) "$Context is missing field '$Name'."
    return [string]$Fields[$Name]
}

function Get-Stage5UInt64Field {
    param([hashtable]$Fields, [string]$Name, [string]$Context)
    $text = Get-Stage5RequiredField $Fields $Name $Context
    [UInt64]$value = 0
    Assert-Stage5Condition ([UInt64]::TryParse($text, [ref]$value)) `
        "$Context field '$Name' is not an unsigned integer."
    return $value
}

function Get-Stage5SingleLine {
    param([string]$Output, [string]$Prefix, [string]$Context)
    $lines = @($Output -split "`r?`n" | Where-Object { $_.StartsWith($Prefix + ' ', [StringComparison]::Ordinal) })
    Assert-Stage5Condition ($lines.Count -eq 1) "$Context requires exactly one $Prefix line."
    return $lines[0]
}

function Get-Stage5UInt64BitCount {
    param([UInt64]$Value)
    [UInt64]$remaining = $Value
    [UInt64]$count = 0
    while ($remaining -ne 0) {
        $remaining = [UInt64]($remaining -band ($remaining - [UInt64]1))
        ++$count
    }
    return $count
}

function Get-Stage5ImmutableSpatialFieldNames {
    param([string]$Prefix = '')
    $names = @()
    $names += $Prefix + 'captured_arenas'
    $names += $Prefix + 'capture_failures'
    foreach ($collectionSuffix in @('successful_collections',
        'successful_collection_queries', 'successful_collection_ranges',
        'multi_range_collections', 'collection_submitted_jobs',
        'collection_completed_jobs', 'collection_physical_worker_jobs',
        'collection_owner_helped_jobs', 'collection_physical_worker_mask',
        'maximum_collection_queries', 'maximum_collection_ranges',
        'maximum_collection_distinct_physical_workers')) {
        $names += $Prefix + $collectionSuffix
    }
    foreach ($consumer in @('healing', 'pdl')) {
        foreach ($suffix in @('eligible_queries', 'authoritative_queries',
            'authoritative_candidates', 'shadow_queries', 'shadow_matches',
            'shadow_mismatches', 'submitted_jobs', 'completed_jobs',
            'physical_worker_jobs', 'owner_helped_jobs', 'expected_fallbacks',
            'unexpected_fallbacks', 'stale_rejections', 'validation_failures',
            'circuit_breaker_trips')) {
            $names += $Prefix + $consumer + '_' + $suffix
        }
    }
    return $names
}

function ConvertTo-Stage5LiveDictionary {
    param([object]$Value, [string]$Context)
    if ($Value -is [Collections.IDictionary]) { return $Value }
    Assert-Stage5Condition ($Value -is [pscustomobject]) "$Context must be a live-plan object."
    $dictionary = @{}
    foreach ($property in $Value.PSObject.Properties) {
        Assert-Stage5Condition (-not $dictionary.ContainsKey($property.Name)) `
            "$Context contains a duplicate live-plan property."
        $dictionary[$property.Name] = $property.Value
    }
    return $dictionary
}

function Get-Stage5LiveSelectorKey {
    param([object]$Selector, [string]$Context)
    $selectorObject = ConvertTo-Stage5LiveDictionary $Selector $Context
    Assert-Stage5JsonProperties $selectorObject @('scenario', 'seed', 'configuration', 'repeat') $Context
    Assert-Stage5Condition (@('4v2', '4v3', 'hard-ai-2v6') -ccontains $selectorObject.scenario -and
        (Test-Stage5JsonInteger $selectorObject.seed) -and $selectorObject.seed -gt 0 -and
        $selectorObject.seed -le [UInt32]::MaxValue -and
        (Test-Stage5JsonInteger $selectorObject.repeat) -and $selectorObject.repeat -gt 0 -and
        $selectorObject.configuration -is [string] -and
        $selectorObject.configuration -cmatch '^(?:serial-1|parallel-(?:1|2|4|8|16|auto)|shadow-(?:8|16))$') `
        "$Context has an invalid live plan selector."
    return "$($selectorObject.scenario)|$($selectorObject.seed)|$($selectorObject.configuration)|$($selectorObject.repeat)"
}

function Assert-Stage5LivePlanIdentityScalarTypes {
    param([Collections.IDictionary]$Candidate, [string]$Context)
    foreach ($field in @('entryId', 'kind', 'scenario', 'configuration',
            'simulationMode', 'requestedWorkers', 'workerPolicy',
            'validationRole', 'proofProfileId')) {
        $value = Get-Stage5JsonValue $Candidate $field $Context
        Assert-Stage5Condition ($value -is [string]) `
            "$Context field '$field' must be a JSON string scalar."
    }
    foreach ($field in @('sequence', 'seed', 'repeat')) {
        $value = Get-Stage5JsonValue $Candidate $field $Context
        Assert-Stage5Condition (Test-Stage5JsonInteger $value) `
            "$Context field '$field' must be a JSON integer scalar."
    }
    $stress = Get-Stage5JsonValue $Candidate 'stress' $Context
    Assert-Stage5Condition ($stress -is [bool]) `
        "$Context field 'stress' must be a JSON boolean scalar."
}

function New-Stage5LiveValidationRequirementsMap {
    param([object]$Plan)
    $context = 'Stage 5 live validation plan'
    $planObject = ConvertTo-Stage5LiveDictionary $Plan $context
    Assert-Stage5JsonProperties $planObject @('schemaVersion', 'liveQualification', 'entries') $context
    Assert-Stage5Condition ((Test-Stage5JsonInteger $planObject.schemaVersion) -and
        $planObject.schemaVersion -eq 2 -and $planObject.entries -is [Array]) `
        "$context requires the explicit V2 role contract."
    $qualification = ConvertTo-Stage5LiveDictionary $planObject.liveQualification "$context qualification"
    Assert-Stage5JsonShape $qualification @('schemaVersion', 'profileSetId', 'authorityEntries', 'shadowEntry') `
        "$context qualification"
    Assert-Stage5Condition ((Test-Stage5JsonInteger $qualification.schemaVersion) -and
        $qualification.schemaVersion -eq 1 -and
        $qualification.profileSetId -ceq 'live-all-slices-v1' -and
        $qualification.authorityEntries -is [Array] -and $qualification.authorityEntries.Count -gt 0) `
        "$context has an unknown profile or empty authority selection."

    $entriesByKey = @{}
    $entryIds = @{}
    foreach ($planned in $planObject.entries) {
        $candidate = ConvertTo-Stage5LiveDictionary $planned "$context entry"
        Assert-Stage5JsonProperties $candidate @('kind') "$context entry"
        $candidateKind = Get-Stage5JsonValue $candidate 'kind' "$context entry"
        Assert-Stage5Condition ($candidateKind -is [string]) `
            "$context entry field 'kind' must be a JSON string scalar."
        if ($candidateKind -cne 'ai') { continue }
        Assert-Stage5JsonProperties $candidate @('entryId', 'sequence', 'scenario', 'seed',
            'configuration', 'repeat', 'simulationMode', 'requestedWorkers', 'workerPolicy',
            'stress', 'validationRole', 'proofProfileId') "$context AI entry"
        Assert-Stage5LivePlanIdentityScalarTypes $candidate "$context AI entry"
        $key = Get-Stage5LiveSelectorKey $candidate "$context AI entry"
        Assert-Stage5Condition ($candidate.entryId -is [string] -and
            -not [string]::IsNullOrWhiteSpace($candidate.entryId) -and
            (Test-Stage5JsonInteger $candidate.sequence) -and $candidate.sequence -gt 0 -and
            -not $entriesByKey.ContainsKey($key) -and -not $entryIds.ContainsKey($candidate.entryId)) `
            "$context contains a missing or duplicate AI entry identity."
        $lane = $candidate.configuration.Split('-')
        Assert-Stage5Condition ($candidate.simulationMode -ceq $lane[0] -and
            [string]$candidate.requestedWorkers -ceq $lane[1] -and
            $candidate.workerPolicy -ceq 'auto' -and $candidate.stress -is [bool] -and
            $candidate.stress -eq ($candidate.scenario -ceq '4v2' -or
                $candidate.scenario -ceq 'hard-ai-2v6')) `
            "$context AI entry policy or stress applicability is inconsistent."
        $entriesByKey[$key] = $candidate
        $entryIds[$candidate.entryId] = $key
    }

    $authorityKeys = @{}
    foreach ($selector in $qualification.authorityEntries) {
        $selectorObject = ConvertTo-Stage5LiveDictionary $selector "$context authority selector"
        Assert-Stage5JsonShape $selectorObject @('scenario', 'seed', 'configuration', 'repeat') `
            "$context authority selector"
        $key = Get-Stage5LiveSelectorKey $selectorObject "$context authority selector"
        Assert-Stage5Condition ($selectorObject.scenario -ceq '4v2' -and
            $selectorObject.configuration -cmatch '^parallel-(?:2|4|8|16|auto)$' -and
            $entriesByKey.ContainsKey($key) -and -not $authorityKeys.ContainsKey($key)) `
            "$context authority selector is duplicated, incapable, or absent from the planned matrix."
        $authorityKeys[$key] = $true
    }
    $shadowSelector = ConvertTo-Stage5LiveDictionary $qualification.shadowEntry "$context shadow selector"
    Assert-Stage5JsonShape $shadowSelector @('scenario', 'seed', 'configuration', 'repeat') `
        "$context shadow selector"
    $shadowKey = Get-Stage5LiveSelectorKey $shadowSelector "$context shadow selector"
    Assert-Stage5Condition ($shadowSelector.scenario -ceq '4v2' -and
        $shadowSelector.configuration -cmatch '^shadow-(?:8|16)$' -and
        $entriesByKey.ContainsKey($shadowKey)) "$context shadow selector is absent or invalid."

    $requirementsMutable = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::Ordinal)
    $identityFields = @('entryId', 'kind', 'sequence', 'scenario', 'seed',
        'configuration', 'repeat', 'simulationMode', 'requestedWorkers',
        'workerPolicy', 'stress', 'validationRole', 'proofProfileId')
    foreach ($key in $entriesByKey.Keys) {
        $candidate = $entriesByKey[$key]
        $role = 'live-determinism'
        $profile = 'live-invariants-v1'
        if ($authorityKeys.ContainsKey($key)) {
            $role = 'live-authority-stress'
            $profile = 'live-all-slices-authority-v1'
        }
        elseif ($key -ceq $shadowKey) {
            $role = 'live-shadow-stress'
            $profile = 'live-all-slices-shadow-v1'
        }
        Assert-Stage5Condition ($candidate.validationRole -ceq $role -and
            $candidate.proofProfileId -ceq $profile -and
            ($candidate.simulationMode -cne 'shadow' -or $role -ceq 'live-shadow-stress')) `
            "$context entry has an unknown or selector-inconsistent role/profile."
        $recordMutable = New-Object 'Collections.Generic.Dictionary[string,object]' `
            ([StringComparer]::Ordinal)
        foreach ($field in $identityFields) {
            $value = if ($field -ceq 'validationRole') { $role }
                elseif ($field -ceq 'proofProfileId') { $profile }
                else { $candidate[$field] }
            $recordMutable.Add($field, $value)
        }
        $record = [Collections.ObjectModel.ReadOnlyDictionary[string,object]]::new(
            $recordMutable)
        $requirementsMutable.Add([string]$candidate.entryId, $record)
    }
    return [Collections.ObjectModel.ReadOnlyDictionary[string,object]]::new(
        $requirementsMutable)
}

function Get-Stage5LiveValidationRequirementsFromMap {
    param(
        [Collections.IDictionary]$RequirementsMap,
        [object]$Entry
    )
    $context = 'Stage 5 live validation plan'
    $requested = ConvertTo-Stage5LiveDictionary $Entry "$context requested entry"
    Assert-Stage5JsonProperties $requested @('entryId') "$context requested entry"
    Assert-Stage5Condition ($requested.entryId -is [string]) `
        "$context requested entry is not a member of the planned matrix."
    $candidate = $RequirementsMap[$requested.entryId]
    Assert-Stage5Condition ($null -ne $candidate) `
        "$context requested entry is not a member of the planned matrix."
    $identityFields = @('entryId', 'kind', 'sequence', 'scenario', 'seed',
        'configuration', 'repeat', 'simulationMode', 'requestedWorkers',
        'workerPolicy', 'stress', 'validationRole', 'proofProfileId')
    foreach ($field in $identityFields) {
        $value = Get-Stage5JsonValue $requested $field "$context requested entry"
        Assert-Stage5Condition ($value -ceq $candidate[$field]) `
            "$context requested entry does not match its frozen role identity in '$field'."
    }
    return [pscustomobject]@{
        schemaVersion = 1
        entryId = [string]$candidate['entryId']
        validationRole = [string]$candidate['validationRole']
        proofProfileId = [string]$candidate['proofProfileId']
        requireCompleteSliceSchema = $true
    }
}

function Assert-Stage5PlannedLiveEntryCoverage {
    param(
        [string[]]$RequiredEntryIds,
        [Collections.IDictionary]$Seen,
        [string]$Context
    )
    foreach ($entryId in $RequiredEntryIds) {
        Assert-Stage5Condition ($entryId -is [string] -and
            -not [string]::IsNullOrWhiteSpace($entryId) -and
            $Seen.ContainsKey($entryId)) `
            "$Context is missing required plan entry '$entryId'; an unselected positive result cannot rescue it."
    }
}

function Resolve-Stage5LiveValidationRequirements {
    param([object]$Plan, [object]$Entry)
    $requirementsMap = New-Stage5LiveValidationRequirementsMap -Plan $Plan
    return Get-Stage5LiveValidationRequirementsFromMap `
        -RequirementsMap $requirementsMap -Entry $Entry
}

function ConvertFrom-Stage5ImmutableSpatialFields {
    param([hashtable]$Fields, [string]$Prefix, [object]$Entry,
        [string]$Context, [UInt64]$EffectiveWorkers,
        [object]$LiveRequirements = $null)
    $fieldNames = @(Get-Stage5ImmutableSpatialFieldNames $Prefix)
    $isStress = $false
    if ($null -ne $LiveRequirements -or $null -ne $Entry.PSObject.Properties['stress']) {
        $isStress = [bool]$Entry.stress
    }
    $authorityScenarioEligible = $null -eq
        $Entry.PSObject.Properties['scenario'] -or $Entry.scenario -ceq '4v2'
    $qualifyingCollectionStress = $isStress -and $authorityScenarioEligible -and
        ($Entry.simulationMode -ceq 'parallel' -or
            $Entry.simulationMode -ceq 'shadow') -and
        ($Entry.configuration -match '^(?:parallel-(?:2|4|8|16|auto)|shadow-16)$' -or
         ($null -ne $LiveRequirements -and
          $LiveRequirements.validationRole -ceq 'live-shadow-stress' -and
          $Entry.configuration -ceq 'shadow-8'))
    $requirePositiveCollection = $qualifyingCollectionStress -and
        ($null -eq $LiveRequirements -or $LiveRequirements.validationRole -cne 'live-determinism')
    foreach ($numeric in $fieldNames) {
        Get-Stage5UInt64Field $Fields $numeric "$Context immutable-spatial evidence" | Out-Null
    }
    $capturedArenas = Get-Stage5UInt64Field $Fields ($Prefix + 'captured_arenas') $Context
    if ($requirePositiveCollection) {
        Assert-Stage5Condition ($capturedArenas -gt 0) `
            "$Context has no captured immutable-spatial arena."
    }
    elseif ($Entry.simulationMode -ceq 'serial') {
        Assert-Stage5Condition ($capturedArenas -eq 0) `
            "$Context serial simulation reports captured immutable-spatial arenas."
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $Fields `
        ($Prefix + 'capture_failures') $Context) -eq 0) `
        "$Context reports immutable-spatial arena capture failures."

    $successfulCollections = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collections') $Context
    $successfulCollectionQueries = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collection_queries') $Context
    $successfulCollectionRanges = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'successful_collection_ranges') $Context
    $multiRangeCollections = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'multi_range_collections') $Context
    $collectionSubmitted = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_submitted_jobs') $Context
    $collectionCompleted = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_completed_jobs') $Context
    $collectionPhysical = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_physical_worker_jobs') $Context
    $collectionOwnerHelped = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_owner_helped_jobs') $Context
    $collectionPhysicalWorkerMask = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'collection_physical_worker_mask') $Context
    $maximumCollectionQueries = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_queries') $Context
    $maximumCollectionRanges = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_ranges') $Context
    $maximumCollectionDistinctPhysicalWorkers = Get-Stage5UInt64Field $Fields `
        ($Prefix + 'maximum_collection_distinct_physical_workers') $Context
    $aggregateCollectionDistinctPhysicalWorkers =
        Get-Stage5UInt64BitCount $collectionPhysicalWorkerMask
    Assert-Stage5Condition ($successfulCollections -eq $multiRangeCollections) `
        "$Context immutable-spatial collection/multi-range counts are inconsistent."
    Assert-Stage5Condition ($collectionSubmitted -eq $collectionCompleted -and
        $collectionCompleted -eq $collectionPhysical) `
        "$Context immutable-spatial collection jobs are not balanced physical-worker work."
    Assert-Stage5Condition ($collectionOwnerHelped -eq 0) `
        "$Context immutable-spatial collection reports owner help."
    Assert-Stage5Condition ($aggregateCollectionDistinctPhysicalWorkers -le
        $collectionPhysical) `
        "$Context immutable-spatial worker-union mask exceeds cumulative physical jobs."
    Assert-Stage5Condition ($EffectiveWorkers -ge 64 -or
        ($collectionPhysicalWorkerMask -shr [int]$EffectiveWorkers) -eq 0) `
        "$Context immutable-spatial physical-worker mask exceeds the effective worker lane."
    if ($successfulCollections -gt 0) {
        Assert-Stage5Condition ($capturedArenas -gt 0) `
            "$Context has no captured immutable-spatial arena for successful collection work."
        Assert-Stage5Condition ($successfulCollectionQueries -ge
            (2 * $successfulCollections) -and $successfulCollectionRanges -ge
            (2 * $successfulCollections) -and $collectionSubmitted -eq
            (2 * $successfulCollectionRanges) -and
            $maximumCollectionQueries -ge 2 -and
            $maximumCollectionRanges -ge 2 -and
            $collectionPhysicalWorkerMask -gt 0 -and
            $maximumCollectionDistinctPhysicalWorkers -gt 0 -and
            $maximumCollectionDistinctPhysicalWorkers -le
                $maximumCollectionRanges -and
            $maximumCollectionDistinctPhysicalWorkers -le
                $aggregateCollectionDistinctPhysicalWorkers) `
            "$Context immutable-spatial collection evidence does not prove multi-query, multi-range two-pass worker execution."
    }
    else {
        Assert-Stage5Condition ($successfulCollectionQueries -eq 0 -and
            $successfulCollectionRanges -eq 0 -and $collectionSubmitted -eq 0 -and
            $collectionCompleted -eq 0 -and $collectionPhysical -eq 0 -and
            $collectionPhysicalWorkerMask -eq 0 -and
            $maximumCollectionQueries -eq 0 -and $maximumCollectionRanges -eq 0 -and
            $maximumCollectionDistinctPhysicalWorkers -eq 0) `
            "$Context reports immutable-spatial collection work without a successful collection."
    }
    if ($qualifyingCollectionStress) {
        Assert-Stage5Condition (-not $requirePositiveCollection -or ($successfulCollections -gt 0 -and
            $successfulCollectionQueries -gt $successfulCollections -and
            $successfulCollectionRanges -gt $successfulCollections -and
            $collectionSubmitted -gt 0 -and $collectionPhysical -gt 0 -and
            $maximumCollectionQueries -ge 2 -and $maximumCollectionRanges -ge 2)) `
            "$Context qualifying stress has no positive multi-query, multi-range immutable-spatial collection evidence."
        if ($Entry.configuration -match '^(?:parallel|shadow)-(2|4|8|16)$') {
            $explicitCollectionWorkers = [UInt64]$Matches[1]
            $expectedMaximumCollectionRanges = [Math]::Min(
                $explicitCollectionWorkers, $maximumCollectionQueries)
            Assert-Stage5Condition ($maximumCollectionRanges -eq
                $expectedMaximumCollectionRanges) `
                "$Context immutable-spatial maximum collection ranges do not match min(explicit workers, maximum queueable queries)."
            Assert-Stage5Condition (($collectionPhysicalWorkerMask -shr
                $explicitCollectionWorkers) -eq 0) `
                "$Context immutable-spatial physical-worker mask exceeds the explicit worker lane."
            if ($expectedMaximumCollectionRanges -ge 4) {
                Assert-Stage5Condition (
                    $maximumCollectionDistinctPhysicalWorkers -gt 1) `
                    "$Context sufficiently large immutable-spatial collection did not use more than one distinct physical worker."
            }
        }
    }
    if ($Entry.simulationMode -ceq 'serial' -or
        $Entry.configuration -ceq 'serial-1' -or
        $Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($successfulCollections -eq 0 -and
            $multiRangeCollections -eq 0 -and $collectionSubmitted -eq 0 -and
            $collectionCompleted -eq 0 -and $collectionPhysical -eq 0) `
            "$Context nonqualifying serial/one-worker lane reports immutable-spatial collection worker authority."
    }

    $consumerEvidence = @{}
    foreach ($consumer in @('healing', 'pdl')) {
        $consumerPrefix = $Prefix + $consumer + '_'
        $eligible = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'eligible_queries') $Context
        $authoritative = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'authoritative_queries') $Context
        $candidates = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'authoritative_candidates') $Context
        $shadow = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'shadow_queries') $Context
        $shadowMatches = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'shadow_matches') $Context
        $shadowMismatches = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'shadow_mismatches') $Context
        $submitted = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'submitted_jobs') $Context
        $completed = Get-Stage5UInt64Field $Fields ($consumerPrefix + 'completed_jobs') $Context
        $physical = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'physical_worker_jobs') $Context
        $ownerHelped = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'owner_helped_jobs') $Context
        $expectedFallbacks = Get-Stage5UInt64Field $Fields `
            ($consumerPrefix + 'expected_fallbacks') $Context
		$unexpectedFallbacks = Get-Stage5UInt64Field $Fields `
			($consumerPrefix + 'unexpected_fallbacks') $Context
		$staleRejections = Get-Stage5UInt64Field $Fields `
			($consumerPrefix + 'stale_rejections') $Context
        Assert-Stage5Condition ($submitted -eq $completed -and
            $completed -eq $physical) `
            "$Context $consumer immutable-spatial jobs are not balanced physical-worker work."
        Assert-Stage5Condition ($ownerHelped -eq 0) `
            "$Context $consumer immutable-spatial work reports owner help."
        Assert-Stage5Condition ($shadow -eq ($shadowMatches + $shadowMismatches)) `
            "$Context $consumer immutable-spatial shadow counters are inconsistent."
        foreach ($zeroInvariant in @('shadow_mismatches', 'unexpected_fallbacks',
            'stale_rejections', 'validation_failures', 'circuit_breaker_trips')) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $Fields `
                ($consumerPrefix + $zeroInvariant) $Context) -eq 0) `
                "$Context reports forbidden $consumer immutable-spatial evidence in '$zeroInvariant'."
        }
        if ($authoritative -gt 0) {
            Assert-Stage5Condition ($eligible -ge $authoritative -and
                $submitted -gt 0 -and $physical -gt 0) `
                "$Context reports $consumer immutable-spatial authority without eligible physical-worker queries."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($authoritative -eq 0 -and $candidates -eq 0) `
                "$Context reports $consumer immutable-spatial authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($shadow -eq 0 -and $shadowMatches -eq 0 -and
                $shadowMismatches -eq 0) `
                "$Context reports $consumer immutable-spatial shadow work outside shadow simulation."
        }
        if ($Entry.configuration -ceq 'serial-1' -or
            $Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($authoritative -eq 0 -and $candidates -eq 0 -and
                $shadow -eq 0 -and $submitted -eq 0 -and $completed -eq 0 -and
                $physical -eq 0) `
                "$Context nonqualifying serial/one-worker lane reports $consumer immutable-spatial worker authority."
        }
        $qualifyingParallelStress = $qualifyingCollectionStress -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
        if ($qualifyingParallelStress) {
            Assert-Stage5Condition ($expectedFallbacks -eq 0) `
                "$Context qualifying stress has no positive $consumer immutable-spatial authority, candidates, and balanced worker evidence."
            Assert-Stage5Condition (($null -ne $LiveRequirements -and
                $LiveRequirements.validationRole -ceq 'live-determinism') -or
                ($authoritative -gt 0 -and $candidates -gt 0 -and
                 $submitted -gt 0 -and $physical -gt 0)) `
                "$Context qualifying stress has no positive $consumer immutable-spatial authority, candidates, and balanced worker evidence."
        }
        if ($Entry.simulationMode -ceq 'shadow' -and $qualifyingCollectionStress) {
            Assert-Stage5Condition ($shadow -gt 0 -and $shadowMatches -eq $shadow -and
                $submitted -gt 0 -and $physical -gt 0 -and $authoritative -eq 0 -and
                $candidates -eq 0 -and $expectedFallbacks -eq 0) `
                "$Context shadow stress has no positive matching $consumer immutable-spatial worker comparison."
        }
        $consumerEvidence[$consumer] = [pscustomobject]@{
            eligibleQueries = $eligible
            authoritativeQueries = $authoritative
            authoritativeCandidates = $candidates
            shadowQueries = $shadow
            shadowMatches = $shadowMatches
            submittedJobs = $submitted
            completedJobs = $completed
            physicalWorkerJobs = $physical
            expectedFallbacks = $expectedFallbacks
			unexpectedFallbacks = $unexpectedFallbacks
			staleRejections = $staleRejections
        }
    }
    return [pscustomobject]@{
        capturedArenas = $capturedArenas
        successfulCollections = $successfulCollections
        successfulCollectionQueries = $successfulCollectionQueries
        successfulCollectionRanges = $successfulCollectionRanges
        collectionSubmittedJobs = $collectionSubmitted
        collectionCompletedJobs = $collectionCompleted
        collectionPhysicalWorkerJobs = $collectionPhysical
        collectionPhysicalWorkerMask = $collectionPhysicalWorkerMask
        maximumCollectionDistinctPhysicalWorkers =
            $maximumCollectionDistinctPhysicalWorkers
        healing = $consumerEvidence['healing']
        pdl = $consumerEvidence['pdl']
        fields = $Fields
    }
}

function ConvertFrom-Stage5AiCompletionCore {
    param([string]$Output, [object]$Entry, [string]$ExecutableHash,
        [bool]$RequireAuthoritativeWorkEvidence = $true, [object]$ValidationPlan = $null,
        [object]$ValidatedLiveRequirements = $null)
    $context = "AI validation entry $($Entry.sequence)"
    $liveRequirements = $null
    if ($null -ne $ValidatedLiveRequirements) {
        $liveRequirements = $ValidatedLiveRequirements
    }
    elseif ($null -ne $ValidationPlan) {
        $liveRequirements = Resolve-Stage5LiveValidationRequirements -Plan $ValidationPlan -Entry $Entry
    }
    else {
        $legacyEntry = ConvertTo-Stage5LiveDictionary $Entry $context
        Assert-Stage5Condition (-not $legacyEntry.Contains('validationRole') -and
            -not $legacyEntry.Contains('proofProfileId')) "$context has a V2 role without its validation plan."
    }
    $requireParallelCapability = $null -eq $liveRequirements -or
        $liveRequirements.validationRole -ceq 'live-authority-stress'
    $isVersionedShadowProof = $null -ne $liveRequirements -and
        $liveRequirements.validationRole -ceq 'live-shadow-stress'
    $line = Get-Stage5SingleLine $Output 'SKIRMISH_AI_TEST_COMPLETE' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SKIRMISH_AI_TEST_COMPLETE' "$context completion manifest"
    foreach ($required in @('seed', 'loaded_seed', 'scenario', 'actual_ai', 'actual_teams', 'winner_team', 'end_frame', 'executable_sha256',
        'simulation_mode', 'requested_pipeline', 'effective_pipeline', 'requested_simulation',
        'effective_simulation', 'requested_workers', 'effective_workers', 'worker_policy',
        'final_digest', 'wall_ms', 'job_submitted', 'job_executed', 'job_steals',
        'job_owner_help', 'job_waits', 'job_worker_wait_reject', 'job_failed',
        'job_cancelled', 'job_fallback', 'job_queue_latency_ns',
        'job_max_queue_latency_ns', 'job_sleeps', 'job_wakes', 'job_affinity_failures',
        'job_queue_high_water', 'job_peak_active_workers', 'available_cpus',
        'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5RequiredField $fields $required "$context completion manifest" | Out-Null
    }
    $spatialWorkFieldNames = @(Get-Stage5ImmutableSpatialFieldNames 'spatial_')
    $workFieldNames = @('authoritative_commits', 'shadow_executions', 'owner_fallbacks',
        'ai_captured_snapshots', 'ai_captured_candidates', 'ai_requested_batches',
        'ai_submitted_jobs', 'ai_completed_jobs', 'ai_serial_fallbacks',
        'ai_shadow_matches', 'ai_shadow_mismatches', 'ai_validation_failures',
        'ai_committed_batches', 'ai_parallel_authoritative_commits',
        'ai_rejected_commits',
		'direct_eligible', 'direct_submitted', 'direct_executed',
		'direct_worker_executed', 'direct_owner_helped',
		'direct_authoritative_commits',
		'direct_authoritative_multiworker_commits', 'direct_stale_rejections',
		'direct_validation_failures', 'direct_serial_fallbacks',
		'direct_unsupported_authority', 'direct_shadow_authority',
		'direct_stale_acceptance', 'direct_malformed_acceptance',
		'direct_shadow_only', 'direct_timeouts', 'direct_late_drains',
		'direct_peak_active_workers',
		'direct_callback_min', 'direct_callback_max',
		'ordinary_path_eligible', 'ordinary_path_submitted_requests',
		'ordinary_path_submitted_ranges',
		'ordinary_path_worker_executed_requests',
		'ordinary_path_worker_executed_range_jobs',
		'ordinary_path_owner_helped_range_jobs',
		'ordinary_path_failed_range_jobs',
		'ordinary_path_physical_worker_mask',
		'ordinary_path_distinct_physical_workers',
		'ordinary_path_physical_worker_mask_complete',
		'ordinary_path_authoritative_commits',
		'ordinary_path_authoritative_multiworker_commits',
		'ordinary_path_stale_rejections',
		'ordinary_path_validation_failures',
		'ordinary_path_serial_fallbacks',
		'ordinary_path_shadow_comparisons',
		'ordinary_path_shadow_mismatches',
		'ordinary_path_timeouts', 'ordinary_path_late_drains',
		'ordinary_path_peak_active_workers',
		'ordinary_path_max_batch_requests',
		'ordinary_path_max_range_count',
		'ordinary_path_max_grain_size',
        'collision_authoritative_commits', 'collision_shadow_executions',
        'collision_shadow_compared_candidates',
        'collision_shadow_mismatches', 'collision_owner_fallbacks',
        'collision_unexpected_fallbacks', 'collision_ineligible_slices',
        'collision_stale_rejections', 'collision_committed_candidates',
        'collision_prepared_pairs', 'collision_unique_candidates',
        'collision_submitted_jobs', 'collision_completed_jobs',
        'collision_physical_worker_jobs', 'collision_owner_helped_jobs',
        'collision_physical_worker_mask',
        'collision_distinct_physical_workers',
		'collision_physical_worker_mask_complete',
        'physics_authoritative_batches', 'physics_committed_prefixes',
        'physics_ranges', 'physics_submitted_jobs', 'physics_completed_jobs',
		'physics_physical_worker_jobs', 'physics_owner_helped_jobs',
		'physics_physical_worker_mask', 'physics_distinct_physical_workers',
		'physics_physical_worker_mask_complete',
		'physics_peak_concurrent_physical_workers',
        'physics_allocated_bytes', 'physics_capture_ns', 'physics_prepare_ns',
        'physics_wait_ns', 'physics_commit_ns', 'physics_storage_bytes',
        'physics_storage_capacity_bytes', 'physics_storage_allocations',
        'physics_shadow_executions', 'physics_shadow_prefixes',
        'physics_shadow_ranges', 'physics_shadow_submitted_jobs',
        'physics_shadow_completed_jobs', 'physics_shadow_matches',
        'physics_shadow_mismatches', 'physics_owner_fallbacks',
        'physics_ineligible_slices', 'physics_unexpected_fallbacks',
        'physics_stale_rejections', 'physics_circuit_breaker_trips',
		'status_authoritative_batches', 'status_committed_commands',
		'status_submitted_jobs', 'status_completed_jobs',
		'status_physical_worker_jobs', 'status_owner_helped_jobs',
		'status_physical_worker_mask', 'status_distinct_physical_workers',
		'status_physical_worker_mask_complete',
		'status_peak_concurrent_physical_workers', 'status_shadow_executions',
		'status_shadow_commands', 'status_shadow_matches',
		'status_shadow_mismatches', 'status_owner_fallbacks',
		'status_stale_rejections') +
        $spatialWorkFieldNames
    $presentWorkFieldCount = @($workFieldNames | Where-Object { $fields.ContainsKey($_) }).Count
    Assert-Stage5Condition ($presentWorkFieldCount -eq 0 -or
        $presentWorkFieldCount -eq $workFieldNames.Count) `
        "$context completion manifest contains an incomplete authoritative-work schema."
    $hasAuthoritativeWorkEvidence = $presentWorkFieldCount -eq $workFieldNames.Count
    Assert-Stage5Condition ((-not $RequireAuthoritativeWorkEvidence -and $null -eq $liveRequirements) -or
        $hasAuthoritativeWorkEvidence) `
        "$context completion manifest is missing required authoritative Stage 5 work evidence."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'executable_sha256' $context) -ceq $ExecutableHash) `
        "$context executable hash does not match the validated candidate."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'simulation_mode' $context) -ceq $Entry.simulationMode) `
        "$context simulation mode does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_pipeline' $context) -ceq 'serial') `
        "$context did not honestly request the serial pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_pipeline' $context) -ceq 'serial') `
        "$context did not run the serial pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_simulation' $context) -ceq
        $Entry.simulationMode) "$context live requested simulation policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_simulation' $context) -ceq
        $Entry.simulationMode) "$context live effective simulation policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_workers' $context) -ceq [string]$Entry.requestedWorkers) `
        "$context requested worker count does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'worker_policy' $context) -ceq 'auto') `
        "$context worker policy does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'seed' $context) -ceq [string]$Entry.seed) `
        "$context seed does not match the plan."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'loaded_seed' $context) -eq [UInt64]$Entry.seed) `
        "$context loaded_seed does not match the planned live seed."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'scenario' $context) -ceq $Entry.scenario) `
        "$context scenario does not match the plan."
    $expectedAiCount = if ($Entry.scenario -ceq '4v2') { 6 } elseif (
        $Entry.scenario -ceq 'hard-ai-2v6') { 8 } else { 7 }
    $expectedTeamShape = if ($Entry.scenario -ceq 'hard-ai-2v6') {
        '2v6'
    } else { [string]$Entry.scenario }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'actual_ai' $context) -eq $expectedAiCount) `
        "$context actual_ai does not match the planned scenario."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'actual_teams' $context) -ceq $expectedTeamShape) `
        "$context actual_teams does not match the planned scenario."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'final_digest' $context) -match '^[0-9A-Fa-f]{8}$') `
        "$context final digest is invalid."

    foreach ($numeric in @('loaded_seed', 'actual_ai', 'winner_team', 'end_frame', 'effective_workers', 'wall_ms',
        'job_submitted', 'job_executed', 'job_steals', 'job_owner_help', 'job_waits',
        'job_worker_wait_reject', 'job_failed', 'job_cancelled', 'job_fallback',
        'job_queue_latency_ns', 'job_max_queue_latency_ns', 'job_sleeps', 'job_wakes',
        'job_affinity_failures', 'job_queue_high_water', 'job_peak_active_workers',
        'available_cpus', 'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5UInt64Field $fields $numeric "$context completion manifest" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'job_failed' $context) -eq 0) `
        "$context reports failed jobs."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'job_cancelled' $context) -eq 0) `
        "$context reports cancelled jobs."

    [UInt64]$authoritativeCommits = 0
    [UInt64]$shadowExecutions = 0
    [UInt64]$ownerFallbacks = 0
    [UInt64]$aiSubmittedJobs = 0
    [UInt64]$aiCompletedJobs = 0
    [UInt64]$aiCommittedBatches = 0
    [UInt64]$aiParallelAuthoritativeCommits = 0
	[UInt64]$pathWorkerExecuted = 0
	[UInt64]$pathAuthoritativeCommits = 0
	[UInt64]$pathAuthoritativeMultiWorkerCommits = 0
	[UInt64]$pathOwnerHelped = 0
	[UInt64]$pathPeakWorkers = 0
	[UInt64]$ordinaryPathWorkerExecutedRequests = 0
	[UInt64]$ordinaryPathWorkerExecutedRangeJobs = 0
	[UInt64]$ordinaryPathOwnerHelpedRangeJobs = 0
	[UInt64]$ordinaryPathPhysicalWorkerMask = 0
	[UInt64]$ordinaryPathDistinctPhysicalWorkers = 0
	[UInt64]$ordinaryPathAuthoritativeCommits = 0
	[UInt64]$ordinaryPathAuthoritativeMultiWorkerCommits = 0
	[UInt64]$ordinaryPathPeakWorkers = 0
	[UInt64]$ordinaryPathShadowComparisons = 0
    [UInt64]$collisionAuthoritativeCommits = 0
    [UInt64]$collisionShadowExecutions = 0
    [UInt64]$collisionShadowComparedCandidates = 0
    [UInt64]$collisionOwnerFallbacks = 0
    [UInt64]$collisionCommittedCandidates = 0
    [UInt64]$collisionPreparedPairs = 0
    [UInt64]$collisionUniqueCandidates = 0
    [UInt64]$collisionSubmittedJobs = 0
    [UInt64]$collisionCompletedJobs = 0
    [UInt64]$collisionPhysicalWorkerJobs = 0
    [UInt64]$collisionOwnerHelpedJobs = 0
    [UInt64]$collisionPhysicalWorkerMask = 0
    [UInt64]$collisionDistinctPhysicalWorkers = 0
    [UInt64]$physicsAuthoritativeBatches = 0
    [UInt64]$physicsCommittedPrefixes = 0
    [UInt64]$physicsRanges = 0
    [UInt64]$physicsSubmittedJobs = 0
    [UInt64]$physicsCompletedJobs = 0
    [UInt64]$physicsShadowExecutions = 0
    [UInt64]$physicsShadowPrefixes = 0
    [UInt64]$physicsShadowRanges = 0
    [UInt64]$physicsShadowSubmittedJobs = 0
    [UInt64]$physicsShadowCompletedJobs = 0
    $spatialEvidence = $null
    if ($hasAuthoritativeWorkEvidence) {
        foreach ($numeric in $workFieldNames) {
            Get-Stage5UInt64Field $fields $numeric "$context authoritative-work evidence" | Out-Null
        }
        $authoritativeCommits = Get-Stage5UInt64Field $fields 'authoritative_commits' $context
        $shadowExecutions = Get-Stage5UInt64Field $fields 'shadow_executions' $context
        $ownerFallbacks = Get-Stage5UInt64Field $fields 'owner_fallbacks' $context
        $aiRequestedBatches = Get-Stage5UInt64Field $fields 'ai_requested_batches' $context
        $aiSubmittedJobs = Get-Stage5UInt64Field $fields 'ai_submitted_jobs' $context
        $aiCompletedJobs = Get-Stage5UInt64Field $fields 'ai_completed_jobs' $context
        $aiSerialFallbacks = Get-Stage5UInt64Field $fields 'ai_serial_fallbacks' $context
        $aiShadowMatches = Get-Stage5UInt64Field $fields 'ai_shadow_matches' $context
        $aiShadowMismatches = Get-Stage5UInt64Field $fields 'ai_shadow_mismatches' $context
        $aiCommittedBatches = Get-Stage5UInt64Field $fields 'ai_committed_batches' $context
        $aiParallelAuthoritativeCommits = Get-Stage5UInt64Field $fields `
            'ai_parallel_authoritative_commits' $context
        Assert-Stage5Condition ($authoritativeCommits -eq $aiParallelAuthoritativeCommits) `
            "$context authoritative_commits does not match the mode-specific AI parallel-authority counter."
        Assert-Stage5Condition ($aiParallelAuthoritativeCommits -le $aiCommittedBatches) `
            "$context reports more mode-specific AI parallel-authority commits than generic owner commits."
        Assert-Stage5Condition ($shadowExecutions -eq ($aiShadowMatches + $aiShadowMismatches)) `
            "$context shadow_executions does not match the AI shadow counters."
        Assert-Stage5Condition ($ownerFallbacks -eq $aiSerialFallbacks) `
            "$context owner_fallbacks does not match the AI serial-fallback counter."
        Assert-Stage5Condition ($aiCompletedJobs -le $aiSubmittedJobs) `
            "$context reports more completed AI jobs than submitted AI jobs."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_validation_failures' $context) -eq 0) `
            "$context reports AI planning validation failures."
        Assert-Stage5Condition ($aiShadowMismatches -eq 0) `
            "$context reports AI planning shadow mismatches."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_rejected_commits' $context) -eq 0) `
            "$context reports rejected AI owner commits."
        if ($authoritativeCommits -gt 0) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'ai_captured_snapshots' $context) -gt 0 -and
                (Get-Stage5UInt64Field $fields 'ai_requested_batches' $context) -gt 0) `
                "$context reports authoritative commits without captured/requested AI planning work."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($authoritativeCommits -eq 0 -and
                $aiParallelAuthoritativeCommits -eq 0) `
                "$context reports AI owner authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($shadowExecutions -eq 0 -and
                $aiShadowMatches -eq 0 -and $aiShadowMismatches -eq 0) `
                "$context reports AI shadow work outside shadow simulation."
        }
        if ($Entry.simulationMode -ceq 'serial') {
            Assert-Stage5Condition ($aiSubmittedJobs -eq 0 -and $aiCompletedJobs -eq 0 -and
                $ownerFallbacks -eq 0) `
                "$context serial simulation reports AI lane jobs or owner fallbacks."
        }
        if ($null -ne $liveRequirements -and $requireParallelCapability) {
            Assert-Stage5Condition ($aiParallelAuthoritativeCommits -gt 0 -and
                $aiSubmittedJobs -gt 0 -and $aiCompletedJobs -gt 0) `
                "$context designated authority profile has no AI-specific submitted/completed owner authority."
        }

		$pathEligible = Get-Stage5UInt64Field $fields 'direct_eligible' $context
		$pathSubmitted = Get-Stage5UInt64Field $fields 'direct_submitted' $context
		$pathExecuted = Get-Stage5UInt64Field $fields 'direct_executed' $context
		$pathWorkerExecuted = Get-Stage5UInt64Field $fields 'direct_worker_executed' $context
		$pathOwnerHelped = Get-Stage5UInt64Field $fields 'direct_owner_helped' $context
		$pathTimeouts = Get-Stage5UInt64Field $fields 'direct_timeouts' $context
		$pathAuthoritativeCommits = Get-Stage5UInt64Field $fields `
			'direct_authoritative_commits' $context
		$pathAuthoritativeMultiWorkerCommits = Get-Stage5UInt64Field $fields `
			'direct_authoritative_multiworker_commits' $context
		$pathPeakWorkers = Get-Stage5UInt64Field $fields 'direct_peak_active_workers' $context
		$pathEffectiveWorkers = Get-Stage5UInt64Field $fields 'effective_workers' $context
		$pathCallbackMin = Get-Stage5UInt64Field $fields 'direct_callback_min' $context
		$pathCallbackMax = Get-Stage5UInt64Field $fields 'direct_callback_max' $context
		Assert-Stage5Condition ($pathSubmitted -le $pathEligible) `
			"$context reports more submitted direct-path jobs than eligible requests."
		Assert-Stage5Condition ($pathExecuted -eq $pathSubmitted) `
			"$context direct-path submitted/executed job counts do not match."
		Assert-Stage5Condition (($pathWorkerExecuted + $pathOwnerHelped) -eq $pathExecuted) `
			"$context reports inconsistent direct-path execution identities."
		Assert-Stage5Condition ($pathOwnerHelped -eq 0) `
			"$context reports owner-helped direct-path jobs; the bounded batch lane is physical-worker-only."
		Assert-Stage5Condition ($pathTimeouts -eq 0) `
			"$context reports synchronous direct-path watchdog timeouts."
		Assert-Stage5Condition ($pathAuthoritativeCommits -le $pathWorkerExecuted) `
			"$context reports direct-path authority not backed by physical-worker execution."
		Assert-Stage5Condition ($pathAuthoritativeCommits -eq 0 -or
			($pathSubmitted -ge 2 -and $pathWorkerExecuted -ge 2)) `
			"$context reports direct-path authority from an impossible single-request batch."
		Assert-Stage5Condition ($pathAuthoritativeMultiWorkerCommits -le
			$pathAuthoritativeCommits) `
			"$context reports more multi-worker direct-path commits than authoritative commits."
		Assert-Stage5Condition ($pathAuthoritativeMultiWorkerCommits -eq 0 -or
			$pathPeakWorkers -gt 1) `
			"$context reports multi-worker direct-path authority without a multi-worker peak."
		$pathWorkerBound = [Math]::Min([UInt64]16, $pathEffectiveWorkers)
		Assert-Stage5Condition ($pathPeakWorkers -le $pathWorkerBound -and
			$pathPeakWorkers -le $pathWorkerExecuted -and
			(($pathWorkerExecuted -eq 0 -and $pathPeakWorkers -eq 0) -or
			 ($pathWorkerExecuted -gt 0 -and $pathPeakWorkers -gt 0))) `
			"$context reports an impossible direct-path active-worker count."
		if ($pathEligible -eq 0) {
			Assert-Stage5Condition ($pathCallbackMin -eq 0 -and $pathCallbackMax -eq 0) `
				"$context reports a direct-path callback range without eligible requests."
		}
		else {
			Assert-Stage5Condition ($pathCallbackMin -gt 0 -and
				$pathCallbackMax -ge $pathCallbackMin) `
				"$context reports an invalid direct-path callback range."
		}
		foreach ($zeroInvariant in @('direct_unsupported_authority',
			'direct_shadow_authority', 'direct_stale_acceptance',
			'direct_malformed_acceptance', 'direct_shadow_only',
			'direct_validation_failures')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $fields $zeroInvariant $context) -eq 0) `
				"$context reports forbidden direct-path acceptance evidence in '$zeroInvariant'."
		}
		$isQualifyingPathStress = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'parallel' -and
			$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
		if ($isQualifyingPathStress -and ($requireParallelCapability -or $pathWorkerExecuted -gt 0)) {
			Assert-Stage5Condition ($pathEligible -ge 2 -and $pathSubmitted -ge 2 -and
				$pathWorkerExecuted -gt 1 -and $pathPeakWorkers -gt 1) `
				"$context qualifying parallel stress has no multi-request direct-path batch backed by more than one physical path worker and an authoritative commit."
		}
		if ($isQualifyingPathStress -and $requireParallelCapability) {
			Assert-Stage5Condition ($pathAuthoritativeCommits -gt 0 -and
				$pathAuthoritativeMultiWorkerCommits -gt 0) `
				"$context qualifying parallel stress has no multi-request direct-path batch backed by more than one physical path worker and an authoritative commit."
		}
		if ($Entry.configuration -ceq 'serial-1' -or
			$Entry.configuration -ceq 'parallel-1' -or
			$Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($pathEligible -eq 0 -and $pathSubmitted -eq 0 -and
				$pathExecuted -eq 0 -and $pathWorkerExecuted -eq 0 -and
				$pathOwnerHelped -eq 0 -and $pathAuthoritativeCommits -eq 0 -and
				$pathAuthoritativeMultiWorkerCommits -eq 0 -and
				$pathPeakWorkers -eq 0) `
				"$context nonqualifying serial, one-worker, or non-parallel lane reports direct-path batch work or authority."
		}

		$ordinaryEligible = Get-Stage5UInt64Field $fields `
			'ordinary_path_eligible' $context
		$ordinarySubmittedRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_submitted_requests' $context
		$ordinarySubmittedRanges = Get-Stage5UInt64Field $fields `
			'ordinary_path_submitted_ranges' $context
		$ordinaryPathWorkerExecutedRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_worker_executed_requests' $context
		$ordinaryPathWorkerExecutedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_worker_executed_range_jobs' $context
		$ordinaryPathOwnerHelpedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_owner_helped_range_jobs' $context
		$ordinaryFailedRangeJobs = Get-Stage5UInt64Field $fields `
			'ordinary_path_failed_range_jobs' $context
		$ordinaryPathPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'ordinary_path_physical_worker_mask' $context
		$ordinaryPathDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'ordinary_path_distinct_physical_workers' $context
		$ordinaryPathPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'ordinary_path_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($ordinaryPathPhysicalWorkerMaskComplete -le 1) `
			"$context ordinary-path physical-worker mask completeness is not boolean."
		$ordinaryPathAuthoritativeCommits = Get-Stage5UInt64Field $fields `
			'ordinary_path_authoritative_commits' $context
		$ordinaryPathAuthoritativeMultiWorkerCommits = Get-Stage5UInt64Field `
			$fields 'ordinary_path_authoritative_multiworker_commits' $context
		$ordinaryShadowComparisons = Get-Stage5UInt64Field $fields `
			'ordinary_path_shadow_comparisons' $context
		$ordinaryPathShadowComparisons = $ordinaryShadowComparisons
		$ordinaryPathPeakWorkers = Get-Stage5UInt64Field $fields `
			'ordinary_path_peak_active_workers' $context
		$ordinaryMaximumBatchRequests = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_batch_requests' $context
		$ordinaryMaximumRangeCount = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_range_count' $context
		$ordinaryMaximumGrainSize = Get-Stage5UInt64Field $fields `
			'ordinary_path_max_grain_size' $context
		Assert-Stage5Condition ($ordinarySubmittedRequests -le $ordinaryEligible) `
			"$context reports more submitted ordinary-path requests than eligible requests."
		Assert-Stage5Condition ($ordinaryPathWorkerExecutedRequests -le
			$ordinarySubmittedRequests) `
			"$context reports more worker-executed ordinary-path requests than submitted requests."
		Assert-Stage5Condition (($ordinaryPathWorkerExecutedRangeJobs +
			$ordinaryPathOwnerHelpedRangeJobs + $ordinaryFailedRangeJobs) -eq
			$ordinarySubmittedRanges) `
			"$context ordinary-path range execution identities do not account for every submitted range."
		Assert-Stage5Condition ($ordinaryPathOwnerHelpedRangeJobs -eq 0) `
			"$context reports owner-helped ordinary-path range jobs; authority is physical-worker-only."
		Assert-Stage5Condition ($ordinaryFailedRangeJobs -eq 0) `
			"$context reports failed ordinary-path range jobs."
		[UInt64]$ordinaryPathMaskBitCount = Get-Stage5UInt64BitCount `
			$ordinaryPathPhysicalWorkerMask
		Assert-Stage5Condition (($ordinaryPathWorkerExecutedRangeJobs -eq 0) -eq
			($ordinaryPathDistinctPhysicalWorkers -eq 0)) `
			"$context ordinary-path physical-worker jobs and maximum distinct count disagree."
		Assert-Stage5Condition ($pathEffectiveWorkers -ge 64 -or
			($ordinaryPathPhysicalWorkerMask -shr [int]$pathEffectiveWorkers) -eq 0) `
			"$context ordinary-path physical-worker mask exceeds the effective worker lane."
		Assert-Stage5Condition ($ordinaryPathPhysicalWorkerMaskComplete -eq 1 -or
			$pathEffectiveWorkers -gt 32) `
			"$context ordinary-path physical-worker mask is incomplete inside the representable worker lane."
		if ($ordinaryPathPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($ordinaryPathDistinctPhysicalWorkers -le
				$ordinaryPathMaskBitCount) `
				"$context complete ordinary-path worker-union mask omits the maximum per-batch distinct count."
		}
		Assert-Stage5Condition ($ordinaryPathDistinctPhysicalWorkers -le
			$ordinaryPathWorkerExecutedRangeJobs -and
			$ordinaryPathMaskBitCount -le $ordinaryPathWorkerExecutedRangeJobs -and
			$ordinaryPathDistinctPhysicalWorkers -le $pathEffectiveWorkers) `
			"$context reports an impossible ordinary-path physical-worker identity count."
		Assert-Stage5Condition ($ordinaryPathPeakWorkers -le
			$ordinaryPathDistinctPhysicalWorkers -and
			$ordinaryPathPeakWorkers -le $pathEffectiveWorkers) `
			"$context reports an impossible ordinary-path active-worker peak."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeCommits -le
			$ordinaryPathWorkerExecutedRequests) `
			"$context reports ordinary-path authority not backed by physical-worker request execution."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeMultiWorkerCommits -le
			$ordinaryPathAuthoritativeCommits) `
			"$context reports more multi-worker ordinary-path commits than authoritative commits."
		Assert-Stage5Condition ($ordinaryPathAuthoritativeMultiWorkerCommits -eq 0 -or
			($ordinarySubmittedRequests -ge 2 -and
			 $ordinaryPathWorkerExecutedRequests -ge 2 -and
			 $ordinaryPathDistinctPhysicalWorkers -gt 1 -and
			 $ordinaryPathPeakWorkers -gt 1)) `
			"$context reports ordinary-path multi-worker authority without a concurrent multi-request physical-worker batch."
		Assert-Stage5Condition ($ordinaryMaximumBatchRequests -le
			$ordinarySubmittedRequests -and $ordinaryMaximumRangeCount -le
			$ordinarySubmittedRanges -and $ordinaryMaximumRangeCount -le
			$pathEffectiveWorkers -and $ordinaryMaximumGrainSize -le
			$ordinaryMaximumBatchRequests) `
			"$context reports impossible ordinary-path adaptive batch gauges."
		Assert-Stage5Condition (($ordinarySubmittedRanges -eq 0 -and
			$ordinaryMaximumBatchRequests -eq 0 -and
			$ordinaryMaximumRangeCount -eq 0 -and
			$ordinaryMaximumGrainSize -eq 0 -and
			$ordinaryPathPeakWorkers -eq 0) -or
			($ordinarySubmittedRanges -gt 0 -and
			 $ordinaryMaximumBatchRequests -gt 0 -and
			 $ordinaryMaximumRangeCount -gt 0 -and
			 $ordinaryMaximumGrainSize -gt 0 -and
			 $ordinaryPathPeakWorkers -gt 0)) `
			"$context reports inconsistent ordinary-path batch activity gauges."
		foreach ($zeroInvariant in @('ordinary_path_stale_rejections',
			'ordinary_path_validation_failures', 'ordinary_path_shadow_mismatches',
			'ordinary_path_timeouts')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $fields $zeroInvariant $context) -eq 0) `
				"$context reports forbidden ordinary-path acceptance evidence in '$zeroInvariant'."
		}
		if ($isQualifyingPathStress -and ($requireParallelCapability -or $ordinarySubmittedRanges -gt 0)) {
			Assert-Stage5Condition ($ordinaryEligible -ge 2 -and
				$ordinarySubmittedRequests -ge 2 -and
				$ordinarySubmittedRanges -ge 2 -and
				$ordinaryPathWorkerExecutedRequests -ge 2 -and
				$ordinaryPathWorkerExecutedRangeJobs -ge 2 -and
				$ordinaryPathDistinctPhysicalWorkers -gt 1 -and
				$ordinaryPathPeakWorkers -gt 1 -and
				$ordinaryMaximumBatchRequests -ge 2 -and
				$ordinaryMaximumRangeCount -ge 2) `
				"$context qualifying parallel stress has no authoritative ordinary A* batch backed by concurrent physical path workers."
		}
		if ($isQualifyingPathStress -and $requireParallelCapability) {
			Assert-Stage5Condition ($ordinaryPathAuthoritativeCommits -gt 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -gt 0) `
				"$context qualifying parallel stress has no authoritative ordinary A* batch backed by concurrent physical path workers."
		}
		if ($Entry.simulationMode -ceq 'serial' -or
			$Entry.configuration -ceq 'parallel-1') {
			Assert-Stage5Condition ($ordinarySubmittedRequests -eq 0 -and
				$ordinarySubmittedRanges -eq 0 -and
				$ordinaryPathWorkerExecutedRequests -eq 0 -and
				$ordinaryPathWorkerExecutedRangeJobs -eq 0 -and
				$ordinaryPathAuthoritativeCommits -eq 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -eq 0 -and
				$ordinaryPathPhysicalWorkerMask -eq 0 -and
				$ordinaryPathPeakWorkers -eq 0) `
				"$context serial or parallel-one-worker lane reports ordinary-path batch work or authority."
		}
		if ($Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($ordinaryPathAuthoritativeCommits -eq 0 -and
				$ordinaryPathAuthoritativeMultiWorkerCommits -eq 0) `
				"$context reports ordinary-path authority outside parallel simulation."
		}
		if ($Entry.simulationMode -cne 'shadow') {
			Assert-Stage5Condition ($ordinaryShadowComparisons -eq 0) `
				"$context reports ordinary-path shadow comparisons outside shadow simulation."
		}
		$isQualifyingOrdinaryShadow = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'shadow' -and
			($Entry.configuration -ceq 'shadow-16' -or
             ($isVersionedShadowProof -and $Entry.configuration -ceq 'shadow-8'))
		if ($isQualifyingOrdinaryShadow) {
			Assert-Stage5Condition ($ordinaryShadowComparisons -gt 0 -and
				$ordinaryPathWorkerExecutedRequests -gt 0 -and
				$ordinaryPathWorkerExecutedRangeJobs -gt 0) `
				"$context shadow stress has no physical-worker ordinary-path comparison."
		}

        $collisionAuthoritativeCommits = Get-Stage5UInt64Field $fields `
            'collision_authoritative_commits' $context
        $collisionShadowExecutions = Get-Stage5UInt64Field $fields `
            'collision_shadow_executions' $context
        $collisionShadowComparedCandidates = Get-Stage5UInt64Field $fields `
            'collision_shadow_compared_candidates' $context
        $collisionOwnerFallbacks = Get-Stage5UInt64Field $fields `
            'collision_owner_fallbacks' $context
        $collisionCommittedCandidates = Get-Stage5UInt64Field $fields `
            'collision_committed_candidates' $context
        $collisionPreparedPairs = Get-Stage5UInt64Field $fields `
            'collision_prepared_pairs' $context
        $collisionUniqueCandidates = Get-Stage5UInt64Field $fields `
            'collision_unique_candidates' $context
        $collisionSubmittedJobs = Get-Stage5UInt64Field $fields `
            'collision_submitted_jobs' $context
        $collisionCompletedJobs = Get-Stage5UInt64Field $fields `
            'collision_completed_jobs' $context
        $collisionPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
            'collision_physical_worker_jobs' $context
        $collisionOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
            'collision_owner_helped_jobs' $context
        $collisionPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
            'collision_physical_worker_mask' $context
        $collisionDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
            'collision_distinct_physical_workers' $context
		$collisionPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'collision_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -le 1) `
			"$context collision physical-worker mask completeness is not boolean."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_shadow_mismatches' $context) -eq 0) `
            "$context reports collision shadow mismatches."
        Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
            'collision_unexpected_fallbacks' $context) -eq 0) `
            "$context reports unexpected collision owner fallbacks."
        Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
            "$context collision submitted/completed job counts do not match."
        Assert-Stage5Condition (($collisionPhysicalWorkerJobs +
            $collisionOwnerHelpedJobs) -eq $collisionCompletedJobs) `
            "$context collision execution identities do not account for every completed job."
		[UInt64]$collisionMaskBitCount = Get-Stage5UInt64BitCount `
			$collisionPhysicalWorkerMask
        Assert-Stage5Condition (($collisionPhysicalWorkerJobs -eq 0) -eq
            ($collisionDistinctPhysicalWorkers -eq 0)) `
			"$context collision physical-worker jobs and maximum distinct count disagree."
        [UInt64]$collisionWorkerBound = Get-Stage5UInt64Field $fields `
            'effective_workers' $context
        Assert-Stage5Condition ($collisionWorkerBound -ge 64 -or
            ($collisionPhysicalWorkerMask -shr [int]$collisionWorkerBound) -eq 0) `
            "$context collision physical-worker mask exceeds the effective worker lane."
        Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -eq 1 -or
            $collisionWorkerBound -gt 64) `
            "$context collision physical-worker mask is incomplete inside the representable worker lane."
        if ($collisionPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
				$collisionMaskBitCount) `
				"$context complete collision worker-union mask omits the maximum per-batch distinct count."
		}
        Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
            $collisionPhysicalWorkerJobs -and
            $collisionMaskBitCount -le $collisionPhysicalWorkerJobs -and
            $collisionDistinctPhysicalWorkers -le
                $collisionWorkerBound) `
            "$context reports an impossible collision physical-worker identity count."
        Assert-Stage5Condition ($collisionUniqueCandidates -le $collisionPreparedPairs) `
            "$context reports more unique collision candidates than prepared pairs."
        Assert-Stage5Condition ($collisionCommittedCandidates -le $collisionUniqueCandidates) `
            "$context reports more committed collision contacts than unique candidates."
        Assert-Stage5Condition ($collisionShadowComparedCandidates -le $collisionUniqueCandidates) `
            "$context reports more shadow-compared collision insertions than unique candidates."
        if ($collisionAuthoritativeCommits -gt 0) {
            Assert-Stage5Condition ($collisionPreparedPairs -gt 0 -and
                $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
                "$context reports authoritative collision commits without collision-specific parallel work."
        }
        if ($Entry.simulationMode -ceq 'shadow') {
            Assert-Stage5Condition ($collisionShadowExecutions -gt 0 -and
                $collisionShadowComparedCandidates -gt 0 -and
                $collisionPreparedPairs -gt 0 -and $collisionUniqueCandidates -gt 0 -and
                $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
                "$context shadow stress did not compare positive successful legacy collision insertions with collision-specific work and jobs."
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionCommittedCandidates -eq 0) `
                "$context shadow collision evidence incorrectly reports authoritative publication."
        }
        $isQualifyingCollisionStress = $Entry.scenario -ceq '4v2' -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
        if ($isQualifyingCollisionStress) {
			$collisionStaleRejections = Get-Stage5UInt64Field $fields `
				'collision_stale_rejections' $context
			Assert-Stage5Condition ($collisionOwnerFallbacks -eq 0 -and $collisionStaleRejections -eq 0) `
                "$context qualifying parallel stress has no collision work executed by at least two distinct physical collision workers."
			Assert-Stage5Condition ($collisionCompletedJobs -eq 0 -or
                ($collisionPhysicalWorkerJobs -gt 0 -and $collisionDistinctPhysicalWorkers -ge 2)) `
                "$context qualifying parallel stress has no collision work executed by at least two distinct physical collision workers."
			Assert-Stage5Condition (-not $requireParallelCapability -or
                ($collisionAuthoritativeCommits -gt 0 -and $collisionCommittedCandidates -gt 0 -and
				 $collisionPhysicalWorkerJobs -gt 0 -and $collisionDistinctPhysicalWorkers -ge 2)) `
                "$context qualifying parallel stress has no collision work executed by at least two distinct physical collision workers."
			Assert-Stage5Condition ($collisionWorkerBound -gt 64 -or
				$collisionPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying collision stress below the mask width reports incomplete physical-worker identity evidence."
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionCommittedCandidates -eq 0) `
                "$context reports collision authority outside parallel simulation."
        }
        if ($Entry.simulationMode -cne 'shadow') {
            Assert-Stage5Condition ($collisionShadowExecutions -eq 0 -and
                $collisionShadowComparedCandidates -eq 0) `
                "$context reports collision shadow work outside shadow simulation."
        }
        if ($Entry.simulationMode -ceq 'serial') {
            foreach ($serialCollisionField in @('collision_authoritative_commits',
                'collision_shadow_executions', 'collision_shadow_compared_candidates',
                'collision_shadow_mismatches', 'collision_owner_fallbacks',
                'collision_unexpected_fallbacks', 'collision_ineligible_slices',
                'collision_stale_rejections', 'collision_committed_candidates',
                'collision_prepared_pairs', 'collision_unique_candidates',
                'collision_submitted_jobs', 'collision_completed_jobs',
                'collision_physical_worker_jobs',
                'collision_owner_helped_jobs',
                'collision_physical_worker_mask',
                'collision_distinct_physical_workers')) {
                Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
                    $serialCollisionField $context) -eq 0) `
                    "$context serial simulation reports collision lane work in '$serialCollisionField'."
            }
        }
        elseif ($Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
                $collisionShadowExecutions -eq 0 -and
                $collisionShadowComparedCandidates -eq 0 -and
                $collisionCommittedCandidates -eq 0 -and
                $collisionPreparedPairs -eq 0 -and
                $collisionUniqueCandidates -eq 0 -and
                $collisionSubmittedJobs -eq 0 -and $collisionCompletedJobs -eq 0 -and
                $collisionPhysicalWorkerJobs -eq 0 -and
                $collisionOwnerHelpedJobs -eq 0 -and
                $collisionPhysicalWorkerMask -eq 0 -and
                $collisionDistinctPhysicalWorkers -eq 0 -and
                (Get-Stage5UInt64Field $fields 'collision_stale_rejections' $context) -eq 0) `
                "$context one-worker ineligible simulation reports collision prepared/publication work."
        }

        $physicsAuthoritativeBatches = Get-Stage5UInt64Field $fields `
            'physics_authoritative_batches' $context
        $physicsCommittedPrefixes = Get-Stage5UInt64Field $fields `
            'physics_committed_prefixes' $context
        $physicsRanges = Get-Stage5UInt64Field $fields 'physics_ranges' $context
        $physicsSubmittedJobs = Get-Stage5UInt64Field $fields `
            'physics_submitted_jobs' $context
        $physicsCompletedJobs = Get-Stage5UInt64Field $fields `
            'physics_completed_jobs' $context
		$physicsPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_jobs' $context
		$physicsOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
			'physics_owner_helped_jobs' $context
		$physicsPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_mask' $context
		$physicsDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'physics_distinct_physical_workers' $context
		$physicsPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'physics_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -le 1) `
			"$context physics physical-worker mask completeness is not boolean."
		$physicsPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'physics_peak_concurrent_physical_workers' $context
        $physicsShadowExecutions = Get-Stage5UInt64Field $fields `
            'physics_shadow_executions' $context
        $physicsShadowPrefixes = Get-Stage5UInt64Field $fields `
            'physics_shadow_prefixes' $context
        $physicsShadowRanges = Get-Stage5UInt64Field $fields `
            'physics_shadow_ranges' $context
        $physicsShadowSubmittedJobs = Get-Stage5UInt64Field $fields `
            'physics_shadow_submitted_jobs' $context
        $physicsShadowCompletedJobs = Get-Stage5UInt64Field $fields `
            'physics_shadow_completed_jobs' $context
        $physicsShadowMatches = Get-Stage5UInt64Field $fields `
            'physics_shadow_matches' $context
        $physicsShadowMismatches = Get-Stage5UInt64Field $fields `
            'physics_shadow_mismatches' $context
        $physicsOwnerFallbacks = Get-Stage5UInt64Field $fields `
            'physics_owner_fallbacks' $context
        $physicsStaleRejections = Get-Stage5UInt64Field $fields `
            'physics_stale_rejections' $context
        Assert-Stage5Condition ($physicsSubmittedJobs -eq $physicsCompletedJobs -and
            $physicsRanges -le $physicsSubmittedJobs) `
            "$context reports inconsistent physics ranges or submitted/completed jobs."
		Assert-Stage5Condition (($physicsPhysicalWorkerJobs +
			$physicsOwnerHelpedJobs) -eq $physicsCompletedJobs) `
			"$context physics execution identities do not account for every completed job."
		[UInt64]$physicsMaskBitCount = Get-Stage5UInt64BitCount `
			$physicsPhysicalWorkerMask
		[UInt64]$physicsWorkerBound = Get-Stage5UInt64Field $fields `
			'effective_workers' $context
		Assert-Stage5Condition (($physicsPhysicalWorkerJobs -eq 0) -eq
			($physicsDistinctPhysicalWorkers -eq 0)) `
			"$context physics physical-worker jobs and maximum distinct count disagree."
		Assert-Stage5Condition ($physicsWorkerBound -ge 64 -or
			($physicsPhysicalWorkerMask -shr [int]$physicsWorkerBound) -eq 0) `
			"$context physics physical-worker mask exceeds the effective worker lane."
		Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -eq 1 -or
			$physicsWorkerBound -gt 64) `
			"$context physics physical-worker mask is incomplete inside the representable worker lane."
		if ($physicsPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
				$physicsMaskBitCount) `
				"$context complete physics worker-union mask omits the maximum per-batch distinct count."
		}
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
			$physicsPhysicalWorkerJobs -and
			$physicsMaskBitCount -le $physicsPhysicalWorkerJobs -and
			$physicsDistinctPhysicalWorkers -le $physicsWorkerBound -and
			$physicsPeakConcurrentPhysicalWorkers -le $physicsDistinctPhysicalWorkers -and
			$physicsPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
			"$context reports impossible physics physical-worker evidence."
        Assert-Stage5Condition ($physicsShadowExecutions -eq
            ($physicsShadowMatches + $physicsShadowMismatches)) `
            "$context reports inconsistent physics shadow counters."
        Assert-Stage5Condition ($physicsStaleRejections -le $physicsOwnerFallbacks) `
            "$context reports more stale physics rejections than owner fallbacks."
        $physicsFallbackEligible = $Entry.simulationMode -ceq 'parallel' -and
            $physicsWorkerBound -gt 1
        if (-not $physicsFallbackEligible) {
            Assert-Stage5Condition ($physicsOwnerFallbacks -eq 0 -and
                $physicsStaleRejections -eq 0) `
                "$context reports physics owner fallback evidence outside an eligible multiworker parallel lane."
        }
        foreach ($zeroPhysicsInvariant in @('physics_shadow_mismatches',
            'physics_unexpected_fallbacks', 'physics_circuit_breaker_trips')) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
                $zeroPhysicsInvariant $context) -eq 0) `
                "$context reports forbidden physics evidence in '$zeroPhysicsInvariant'."
        }
        if ($physicsAuthoritativeBatches -gt 0) {
            Assert-Stage5Condition ($physicsCommittedPrefixes -gt 0 -and
                $physicsRanges -gt 0 -and $physicsSubmittedJobs -gt 0 -and
                $physicsCompletedJobs -gt 0) `
                "$context reports authoritative physics batches without physics-specific committed work and jobs."
        }
        $isQualifyingPhysicsStress = $Entry.scenario -ceq '4v2' -and
            $Entry.simulationMode -ceq 'parallel' -and
            $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
		if ($isQualifyingPhysicsStress) {
			Assert-Stage5Condition ($physicsPhysicalWorkerJobs -eq $physicsCompletedJobs -and
				$physicsOwnerHelpedJobs -eq 0 -and
                (($physicsCompletedJobs -eq 0 -and $physicsPeakConcurrentPhysicalWorkers -eq 0) -or
                 ($physicsDistinctPhysicalWorkers -ge 2 -and $physicsPeakConcurrentPhysicalWorkers -ge 2))) `
				"$context qualifying parallel stress has no positive authoritative physics batch, prefix, range, and job evidence."
			Assert-Stage5Condition (-not $requireParallelCapability -or
                ($physicsAuthoritativeBatches -gt 0 -and $physicsCommittedPrefixes -gt 0 -and
                 $physicsRanges -gt 0 -and $physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0)) `
				"$context qualifying parallel stress has no positive authoritative physics batch, prefix, range, and job evidence."
			Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
				$physicsPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying physics stress below the mask width reports incomplete physical-worker identity evidence."
        }
        if ($Entry.simulationMode -ceq 'shadow') {
            Assert-Stage5Condition ($physicsShadowExecutions -gt 0 -and
                $physicsShadowMatches -eq $physicsShadowExecutions -and
                $physicsShadowPrefixes -gt 0 -and $physicsShadowRanges -gt 0 -and
                $physicsShadowSubmittedJobs -gt 0 -and
                $physicsShadowCompletedJobs -gt 0 -and
                $physicsShadowSubmittedJobs -eq $physicsShadowCompletedJobs -and
                $physicsShadowRanges -le $physicsShadowSubmittedJobs) `
                "$context shadow stress has no positive matching physics comparison backed by prefix, range, and job work."
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
                $physicsCommittedPrefixes -eq 0) `
                "$context shadow physics evidence incorrectly reports authoritative publication."
        }
        else {
            Assert-Stage5Condition ($physicsShadowExecutions -eq 0 -and
                $physicsShadowPrefixes -eq 0 -and $physicsShadowRanges -eq 0 -and
                $physicsShadowSubmittedJobs -eq 0 -and
                $physicsShadowCompletedJobs -eq 0 -and
                $physicsShadowMatches -eq 0 -and $physicsShadowMismatches -eq 0) `
                "$context reports physics shadow work outside shadow simulation."
        }
        if ($Entry.configuration -ceq 'serial-1' -or
            $Entry.configuration -ceq 'parallel-1') {
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
                $physicsCommittedPrefixes -eq 0 -and $physicsRanges -eq 0 -and
				$physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0 -and
				$physicsPhysicalWorkerJobs -eq 0 -and
				$physicsOwnerHelpedJobs -eq 0 -and
				$physicsPhysicalWorkerMask -eq 0 -and
				$physicsDistinctPhysicalWorkers -eq 0 -and
				$physicsPeakConcurrentPhysicalWorkers -eq 0) `
                "$context nonqualifying serial/one-worker lane reports physics authority or prepared jobs."
			foreach ($physicsPreparationField in @('physics_allocated_bytes',
				'physics_capture_ns', 'physics_prepare_ns', 'physics_wait_ns',
				'physics_commit_ns', 'physics_storage_bytes',
				'physics_storage_capacity_bytes', 'physics_storage_allocations')) {
				Assert-Stage5Condition ((Get-Stage5UInt64Field $fields `
					$physicsPreparationField $context) -eq 0) `
					"$context nonqualifying serial/one-worker lane reports physics pre-scan, capture, or storage work in '$physicsPreparationField'."
			}
        }
        if ($Entry.simulationMode -cne 'parallel') {
            Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
				$physicsCommittedPrefixes -eq 0 -and
				$physicsPhysicalWorkerJobs -eq 0 -and
				$physicsOwnerHelpedJobs -eq 0) `
                "$context reports physics authority outside parallel simulation."
        }

		$statusAuthoritativeBatches = Get-Stage5UInt64Field $fields `
			'status_authoritative_batches' $context
		$statusCommittedCommands = Get-Stage5UInt64Field $fields `
			'status_committed_commands' $context
		$statusSubmittedJobs = Get-Stage5UInt64Field $fields 'status_submitted_jobs' $context
		$statusCompletedJobs = Get-Stage5UInt64Field $fields 'status_completed_jobs' $context
		$statusPhysicalWorkerJobs = Get-Stage5UInt64Field $fields `
			'status_physical_worker_jobs' $context
		$statusOwnerHelpedJobs = Get-Stage5UInt64Field $fields `
			'status_owner_helped_jobs' $context
		$statusPhysicalWorkerMask = Get-Stage5UInt64Field $fields `
			'status_physical_worker_mask' $context
		$statusDistinctPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'status_distinct_physical_workers' $context
		$statusPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $fields `
			'status_physical_worker_mask_complete' $context
		Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -le 1) `
			"$context status physical-worker mask completeness is not boolean."
		$statusPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $fields `
			'status_peak_concurrent_physical_workers' $context
		$statusShadowExecutions = Get-Stage5UInt64Field $fields `
			'status_shadow_executions' $context
		$statusShadowCommands = Get-Stage5UInt64Field $fields `
			'status_shadow_commands' $context
		$statusShadowMatches = Get-Stage5UInt64Field $fields 'status_shadow_matches' $context
		$statusShadowMismatches = Get-Stage5UInt64Field $fields `
			'status_shadow_mismatches' $context
		$statusOwnerFallbacks = Get-Stage5UInt64Field $fields 'status_owner_fallbacks' $context
		$statusStaleRejections = Get-Stage5UInt64Field $fields 'status_stale_rejections' $context
		Assert-Stage5Condition ($statusSubmittedJobs -eq $statusCompletedJobs -and
			($statusPhysicalWorkerJobs + $statusOwnerHelpedJobs) -eq
				$statusCompletedJobs) `
			"$context reports inconsistent status submitted/completed/identity counters."
		[UInt64]$statusMaskBitCount = Get-Stage5UInt64BitCount `
			$statusPhysicalWorkerMask
		Assert-Stage5Condition (($statusPhysicalWorkerJobs -eq 0) -eq
			($statusDistinctPhysicalWorkers -eq 0)) `
			"$context status physical-worker jobs and maximum distinct count disagree."
		Assert-Stage5Condition ($physicsWorkerBound -ge 64 -or
			($statusPhysicalWorkerMask -shr [int]$physicsWorkerBound) -eq 0) `
			"$context status physical-worker mask exceeds the effective worker lane."
		Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -eq 1 -or
			$physicsWorkerBound -gt 64) `
			"$context status physical-worker mask is incomplete inside the representable worker lane."
		if ($statusPhysicalWorkerMaskComplete -eq 1) {
			Assert-Stage5Condition ($statusDistinctPhysicalWorkers -le
				$statusMaskBitCount) `
				"$context complete status worker-union mask omits the maximum per-batch distinct count."
		}
		Assert-Stage5Condition ($statusDistinctPhysicalWorkers -le $statusPhysicalWorkerJobs -and
			$statusMaskBitCount -le $statusPhysicalWorkerJobs -and
			$statusDistinctPhysicalWorkers -le $physicsWorkerBound -and
			$statusPeakConcurrentPhysicalWorkers -le $statusDistinctPhysicalWorkers -and
			$statusPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
			"$context reports impossible status physical-worker evidence."
		Assert-Stage5Condition ($statusShadowExecutions -eq
			($statusShadowMatches + $statusShadowMismatches)) `
			"$context reports inconsistent status shadow counters."
		Assert-Stage5Condition ($statusShadowMismatches -eq 0 -and
			$statusOwnerFallbacks -eq 0 -and $statusStaleRejections -eq 0) `
			"$context reports forbidden status fallback, stale, or shadow mismatch evidence."
		$isQualifyingStatusStress = $Entry.scenario -ceq '4v2' -and
			$Entry.simulationMode -ceq 'parallel' -and
			$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
		if ($isQualifyingStatusStress) {
			Assert-Stage5Condition ($statusPhysicalWorkerJobs -eq $statusCompletedJobs -and
				$statusOwnerHelpedJobs -eq 0 -and
                (($statusCompletedJobs -eq 0 -and $statusPeakConcurrentPhysicalWorkers -eq 0) -or
                 ($statusDistinctPhysicalWorkers -ge 2 -and $statusPeakConcurrentPhysicalWorkers -ge 2))) `
				"$context qualifying parallel stress has no physical live status authority."
			Assert-Stage5Condition ((-not $requireParallelCapability -and
                $statusAuthoritativeBatches -eq 0 -and $statusCommittedCommands -eq 0) -or
                ($statusAuthoritativeBatches -gt 0 -and $statusCommittedCommands -gt 0 -and
                 $statusSubmittedJobs -gt 0)) `
				"$context qualifying parallel stress has no physical live status authority."
			Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
				$statusPhysicalWorkerMaskComplete -eq 1) `
				"$context qualifying status stress below the mask width reports incomplete physical-worker identity evidence."
		}
		if ($Entry.simulationMode -ceq 'shadow') {
			Assert-Stage5Condition ($statusShadowExecutions -gt 0 -and
				$statusShadowMatches -eq $statusShadowExecutions -and
				$statusShadowCommands -gt 0 -and $statusAuthoritativeBatches -eq 0 -and
				$statusCommittedCommands -eq 0) `
				"$context shadow status evidence is missing or claims authority."
		}
		elseif ($statusShadowExecutions -ne 0 -or $statusShadowCommands -ne 0 -or
			$statusShadowMatches -ne 0) {
			throw "$context reports status shadow work outside shadow simulation."
		}
		if ($Entry.configuration -ceq 'serial-1' -or
			$Entry.configuration -ceq 'parallel-1' -or
			$Entry.simulationMode -cne 'parallel') {
			Assert-Stage5Condition ($statusAuthoritativeBatches -eq 0 -and
				$statusCommittedCommands -eq 0 -and $statusSubmittedJobs -eq 0 -and
				$statusCompletedJobs -eq 0 -and $statusPhysicalWorkerJobs -eq 0 -and
				$statusOwnerHelpedJobs -eq 0 -and $statusPhysicalWorkerMask -eq 0 -and
				$statusDistinctPhysicalWorkers -eq 0 -and
				$statusPeakConcurrentPhysicalWorkers -eq 0) `
				"$context nonparallel status lane reports live authority."
		}
        $spatialEvidence = ConvertFrom-Stage5ImmutableSpatialFields $fields `
            'spatial_' $Entry $context `
            (Get-Stage5UInt64Field $fields 'effective_workers' $context) `
            $liveRequirements
    }

    $effectiveWorkers = Get-Stage5UInt64Field $fields 'effective_workers' $context
    $submittedJobs = Get-Stage5UInt64Field $fields 'job_submitted' $context
    $executedJobs = Get-Stage5UInt64Field $fields 'job_executed' $context
    $fallbackJobs = Get-Stage5UInt64Field $fields 'job_fallback' $context
    $peakWorkers = Get-Stage5UInt64Field $fields 'job_peak_active_workers' $context
    if ($hasAuthoritativeWorkEvidence) {
        Assert-Stage5Condition ($aiSerialFallbacks -le $aiRequestedBatches) `
            "$context AI planning fallback count exceeds its requested batch count."
        Assert-Stage5Condition ($fallbackJobs -ge $aiSerialFallbacks) `
            "$context global fallback count is smaller than its AI planning fallback count."
    }
    $isExpectedOneWorkerFallback = $Entry.configuration -ceq 'parallel-1' -and
        ($fallbackJobs -gt 0 -or $submittedJobs -eq 0)
    if ($Entry.configuration -ceq 'serial-1') {
        Assert-Stage5Condition ($effectiveWorkers -eq 0 -and $submittedJobs -eq 0 -and
            $executedJobs -eq 0) "$context serial configuration reports active workers or jobs."
    }
    elseif ($Entry.configuration -match '^parallel-(1|2|4|8|16)$') {
        $expectedWorkers = [UInt64]$Matches[1]
        Assert-Stage5Condition ($effectiveWorkers -eq $expectedWorkers) `
            "$context effective worker count does not match explicit configuration '$($Entry.configuration)'."
        if (-not $isExpectedOneWorkerFallback) {
            Assert-Stage5Condition ($submittedJobs -gt 0 -and $executedJobs -gt 0) `
                "$context did not submit and execute jobs."
            Assert-Stage5Condition ($peakWorkers -gt 0) "$context did not activate any worker."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-auto') {
        Assert-Stage5Condition ($effectiveWorkers -gt 0) `
            "$context automatic configuration did not report an effective worker."
        Assert-Stage5Condition ($submittedJobs -gt 0 -and
            $executedJobs -gt 0 -and $peakWorkers -gt 0) `
            "$context automatic configuration did not execute parallel jobs."
    }
    elseif ($Entry.configuration -ceq 'shadow-16' -or
        ($isVersionedShadowProof -and $Entry.configuration -ceq 'shadow-8')) {
        $expectedShadowWorkers = if ($Entry.configuration -ceq 'shadow-8') { 8 } else { 16 }
        Assert-Stage5Condition ($effectiveWorkers -eq $expectedShadowWorkers) `
            "$context shadow stress did not run with $expectedShadowWorkers effective workers."
        Assert-Stage5Condition ($submittedJobs -gt 0 -and
            $executedJobs -gt 0 -and $peakWorkers -gt 0) `
            "$context shadow stress did not execute worker jobs."
    }
    else {
        throw "$context has unsupported worker configuration '$($Entry.configuration)'."
    }

    return [pscustomobject]@{
        line = $line
        schemaStatus = $(if ($hasAuthoritativeWorkEvidence) { 'complete' } else { 'unavailable-non-acceptance' })
        invariantStatus = $(if ($hasAuthoritativeWorkEvidence) { 'validated' } else { 'unavailable-non-acceptance' })
        capabilityProofStatus = $(if ($null -eq $liveRequirements) { 'unavailable-v1' }
            elseif ($liveRequirements.validationRole -ceq 'live-determinism') { 'not-required' }
            else { 'validated' })
        validationRole = $(if ($null -ne $liveRequirements) { $liveRequirements.validationRole } else { $null })
        proofProfileId = $(if ($null -ne $liveRequirements) { $liveRequirements.proofProfileId } else { $null })
        finalDigest = (Get-Stage5RequiredField $fields 'final_digest' $context).ToUpperInvariant()
        endFrame = Get-Stage5UInt64Field $fields 'end_frame' $context
        winnerTeam = Get-Stage5UInt64Field $fields 'winner_team' $context
        wallMilliseconds = Get-Stage5UInt64Field $fields 'wall_ms' $context
        expectedOneWorkerFallback = $isExpectedOneWorkerFallback
        authoritativeWorkStatus = $(if ($hasAuthoritativeWorkEvidence) { 'validated' }
            else { 'unavailable-non-acceptance' })
        authoritativeCommits = $authoritativeCommits
        shadowExecutions = $shadowExecutions
        ownerFallbacks = $ownerFallbacks
        aiSubmittedJobs = $aiSubmittedJobs
        aiCompletedJobs = $aiCompletedJobs
        aiCommittedBatches = $aiCommittedBatches
        aiParallelAuthoritativeCommits = $aiParallelAuthoritativeCommits
		pathWorkerExecuted = $pathWorkerExecuted
		pathOwnerHelped = $pathOwnerHelped
		pathAuthoritativeCommits = $pathAuthoritativeCommits
		pathAuthoritativeMultiWorkerCommits =
			$pathAuthoritativeMultiWorkerCommits
		pathPeakActiveWorkers = $pathPeakWorkers
		ordinaryPathWorkerExecutedRequests =
			$ordinaryPathWorkerExecutedRequests
		ordinaryPathWorkerExecutedRangeJobs =
			$ordinaryPathWorkerExecutedRangeJobs
		ordinaryPathOwnerHelpedRangeJobs =
			$ordinaryPathOwnerHelpedRangeJobs
		ordinaryPathPhysicalWorkerMask = $ordinaryPathPhysicalWorkerMask
		ordinaryPathDistinctPhysicalWorkers =
			$ordinaryPathDistinctPhysicalWorkers
		ordinaryPathAuthoritativeCommits =
			$ordinaryPathAuthoritativeCommits
		ordinaryPathAuthoritativeMultiWorkerCommits =
			$ordinaryPathAuthoritativeMultiWorkerCommits
		ordinaryPathPeakActiveWorkers = $ordinaryPathPeakWorkers
		ordinaryPathShadowComparisons = $ordinaryPathShadowComparisons
        collisionAuthoritativeCommits = $collisionAuthoritativeCommits
        collisionShadowExecutions = $collisionShadowExecutions
        collisionShadowComparedCandidates = $collisionShadowComparedCandidates
        collisionOwnerFallbacks = $collisionOwnerFallbacks
        collisionCommittedCandidates = $collisionCommittedCandidates
        collisionPreparedPairs = $collisionPreparedPairs
        collisionUniqueCandidates = $collisionUniqueCandidates
        collisionSubmittedJobs = $collisionSubmittedJobs
        collisionCompletedJobs = $collisionCompletedJobs
        collisionPhysicalWorkerJobs = $collisionPhysicalWorkerJobs
        collisionOwnerHelpedJobs = $collisionOwnerHelpedJobs
        collisionPhysicalWorkerMask = $collisionPhysicalWorkerMask
        collisionDistinctPhysicalWorkers = $collisionDistinctPhysicalWorkers
        physicsAuthoritativeBatches = $physicsAuthoritativeBatches
        physicsCommittedPrefixes = $physicsCommittedPrefixes
        physicsRanges = $physicsRanges
        physicsSubmittedJobs = $physicsSubmittedJobs
        physicsCompletedJobs = $physicsCompletedJobs
        physicsShadowExecutions = $physicsShadowExecutions
        physicsShadowPrefixes = $physicsShadowPrefixes
        physicsShadowRanges = $physicsShadowRanges
        physicsShadowSubmittedJobs = $physicsShadowSubmittedJobs
        physicsShadowCompletedJobs = $physicsShadowCompletedJobs
        spatialEvidence = $spatialEvidence
        fields = $fields
    }
}

function ConvertFrom-Stage5AiCompletion {
    param([string]$Output, [object]$Entry, [string]$ExecutableHash,
        [bool]$RequireAuthoritativeWorkEvidence = $true, [object]$ValidationPlan = $null)
    return ConvertFrom-Stage5AiCompletionCore @PSBoundParameters
}

function ConvertFrom-Stage5ReplayMetrics {
    param([string]$Output, [object]$Entry)
    $context = "Replay validation entry $($Entry.sequence)"
    $line = Get-Stage5SingleLine $Output 'SIMULATION_JOB_METRICS' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SIMULATION_JOB_METRICS' "$context metrics"
    foreach ($required in @('replay', 'requested_mode', 'effective_mode', 'requested_pipeline',
        'effective_pipeline', 'scheduler_started', 'workers', 'submitted', 'executed',
        'steals', 'owner_help', 'waits', 'worker_wait_rejections', 'failures', 'cancelled',
        'fallback', 'queue_latency_ns', 'max_queue_latency_ns', 'sleeps', 'wakes',
        'affinity_failures', 'queue_high_water', 'peak_active_workers', 'available_cpus',
        'reserved_owner_cpus', 'selected_worker_cpus')) {
        Get-Stage5RequiredField $fields $required "$context metrics" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'replay' $context) -ceq $Entry.replayArgument) `
        "$context replay path does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_mode' $context) -ceq $Entry.simulationMode) `
        "$context requested simulation mode does not match the plan."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'requested_pipeline' $context) -ceq 'serial') `
        "$context did not honestly request the serial replay pipeline."
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'effective_pipeline' $context) -ceq 'serial') `
        "$context did not run the serial replay pipeline."
    foreach ($numeric in @('scheduler_started', 'workers', 'submitted', 'executed', 'steals',
        'owner_help', 'waits', 'worker_wait_rejections', 'failures', 'cancelled', 'fallback',
        'queue_latency_ns', 'max_queue_latency_ns', 'sleeps', 'wakes', 'affinity_failures',
        'queue_high_water', 'peak_active_workers', 'available_cpus', 'reserved_owner_cpus',
        'selected_worker_cpus')) {
        Get-Stage5UInt64Field $fields $numeric "$context metrics" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'failures' $context) -eq 0) `
        "$context reports failed jobs."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $fields 'cancelled' $context) -eq 0) `
        "$context reports cancelled jobs."

    $collisionLine = Get-Stage5SingleLine $Output 'COLLISION_CANDIDATE_MANIFEST' $context
    $collisionFields = ConvertFrom-Stage5MetricLine $collisionLine `
        'COLLISION_CANDIDATE_MANIFEST' "$context collision manifest"
    $collisionFieldNames = @('authoritative_commits', 'shadow_executions',
        'shadow_compared_candidates',
        'shadow_mismatches', 'owner_fallbacks', 'unexpected_fallbacks',
        'ineligible_slices', 'stale_rejections', 'committed_candidates',
        'prepared_pairs', 'unique_candidates', 'submitted_jobs', 'completed_jobs',
        'physical_worker_jobs', 'owner_helped_jobs', 'physical_worker_mask',
		'distinct_physical_workers', 'physical_worker_mask_complete')
    foreach ($numeric in $collisionFieldNames) {
        Get-Stage5UInt64Field $collisionFields $numeric `
            "$context collision manifest" | Out-Null
    }
    $collisionAuthoritativeCommits = Get-Stage5UInt64Field $collisionFields `
        'authoritative_commits' $context
    $collisionShadowExecutions = Get-Stage5UInt64Field $collisionFields `
        'shadow_executions' $context
    $collisionShadowComparedCandidates = Get-Stage5UInt64Field $collisionFields `
        'shadow_compared_candidates' $context
    $collisionOwnerFallbacks = Get-Stage5UInt64Field $collisionFields `
        'owner_fallbacks' $context
    $collisionCommittedCandidates = Get-Stage5UInt64Field $collisionFields `
        'committed_candidates' $context
    $collisionPreparedPairs = Get-Stage5UInt64Field $collisionFields `
        'prepared_pairs' $context
    $collisionUniqueCandidates = Get-Stage5UInt64Field $collisionFields `
        'unique_candidates' $context
    $collisionSubmittedJobs = Get-Stage5UInt64Field $collisionFields `
        'submitted_jobs' $context
    $collisionCompletedJobs = Get-Stage5UInt64Field $collisionFields `
        'completed_jobs' $context
    $collisionPhysicalWorkerJobs = Get-Stage5UInt64Field $collisionFields `
        'physical_worker_jobs' $context
    $collisionOwnerHelpedJobs = Get-Stage5UInt64Field $collisionFields `
        'owner_helped_jobs' $context
    $collisionPhysicalWorkerMask = Get-Stage5UInt64Field $collisionFields `
        'physical_worker_mask' $context
    $collisionDistinctPhysicalWorkers = Get-Stage5UInt64Field $collisionFields `
        'distinct_physical_workers' $context
	$collisionPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $collisionFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -le 1) `
		"$context collision physical-worker mask completeness is not boolean."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'shadow_mismatches' $context) -eq 0) `
        "$context reports collision shadow mismatches."
    Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
        'unexpected_fallbacks' $context) -eq 0) `
        "$context reports unexpected collision owner fallbacks."
    Assert-Stage5Condition ($collisionCompletedJobs -eq $collisionSubmittedJobs) `
        "$context collision submitted/completed job counts do not match."
    Assert-Stage5Condition (($collisionPhysicalWorkerJobs +
        $collisionOwnerHelpedJobs) -eq $collisionCompletedJobs) `
        "$context collision execution identities do not account for every completed job."
	[UInt64]$collisionMaskBitCount = Get-Stage5UInt64BitCount `
		$collisionPhysicalWorkerMask
    Assert-Stage5Condition (($collisionPhysicalWorkerJobs -eq 0) -eq
        ($collisionDistinctPhysicalWorkers -eq 0)) `
		"$context collision physical-worker jobs and maximum distinct count disagree."
    [UInt64]$collisionWorkerBound = Get-Stage5UInt64Field $fields `
        'workers' $context
    Assert-Stage5Condition ($collisionWorkerBound -ge 64 -or
        ($collisionPhysicalWorkerMask -shr [int]$collisionWorkerBound) -eq 0) `
        "$context collision physical-worker mask exceeds the effective worker lane."
    Assert-Stage5Condition ($collisionPhysicalWorkerMaskComplete -eq 1 -or
        $collisionWorkerBound -gt 64) `
        "$context collision physical-worker mask is incomplete inside the representable worker lane."
    if ($collisionPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
			$collisionMaskBitCount) `
			"$context complete collision worker-union mask omits the maximum per-batch distinct count."
	}
    Assert-Stage5Condition ($collisionDistinctPhysicalWorkers -le
        $collisionPhysicalWorkerJobs -and
        $collisionMaskBitCount -le $collisionPhysicalWorkerJobs -and
        $collisionDistinctPhysicalWorkers -le
            $collisionWorkerBound) `
        "$context reports an impossible collision physical-worker identity count."
    Assert-Stage5Condition ($collisionUniqueCandidates -le $collisionPreparedPairs) `
        "$context reports more unique collision candidates than prepared pairs."
    Assert-Stage5Condition ($collisionCommittedCandidates -le $collisionUniqueCandidates) `
        "$context reports more committed collision contacts than unique candidates."
    Assert-Stage5Condition ($collisionShadowComparedCandidates -le $collisionUniqueCandidates) `
        "$context reports more shadow-compared collision insertions than unique candidates."
    if ($collisionAuthoritativeCommits -gt 0) {
        Assert-Stage5Condition ($collisionPreparedPairs -gt 0 -and
            $collisionSubmittedJobs -gt 0 -and $collisionCompletedJobs -gt 0) `
            "$context reports authoritative collision commits without collision-specific parallel work."
    }
    if ($Entry.simulationMode -cne 'parallel') {
        Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
            $collisionCommittedCandidates -eq 0) `
            "$context reports collision authority outside parallel simulation."
    }
    if ($Entry.simulationMode -cne 'shadow') {
        Assert-Stage5Condition ($collisionShadowExecutions -eq 0 -and
            $collisionShadowComparedCandidates -eq 0) `
            "$context reports collision shadow work outside shadow simulation."
    }
    $isQualifyingCollisionStress = $Entry.stress -and
        $Entry.simulationMode -ceq 'parallel' -and
        $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
    if ($isQualifyingCollisionStress) {
		$collisionStaleRejections = Get-Stage5UInt64Field $collisionFields `
			'stale_rejections' $context
		Assert-Stage5Condition ($collisionAuthoritativeCommits -gt 0 -and
			$collisionCommittedCandidates -gt 0 -and
			$collisionOwnerFallbacks -eq 0 -and $collisionStaleRejections -eq 0 -and
			$collisionPhysicalWorkerJobs -gt 0 -and
            $collisionDistinctPhysicalWorkers -ge 2) `
            "$context qualifying stress replay has no collision work executed by at least two distinct physical collision workers."
		Assert-Stage5Condition ($collisionWorkerBound -gt 64 -or
			$collisionPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying collision replay below the mask width reports incomplete physical-worker identity evidence."
    }
    if ($Entry.simulationMode -ceq 'serial') {
		foreach ($serialCollisionField in @($collisionFieldNames | Where-Object {
			$_ -cne 'physical_worker_mask_complete' })) {
            Assert-Stage5Condition ((Get-Stage5UInt64Field $collisionFields `
                $serialCollisionField $context) -eq 0) `
                "$context serial replay reports collision lane work in '$serialCollisionField'."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($collisionAuthoritativeCommits -eq 0 -and
            $collisionShadowExecutions -eq 0 -and
            $collisionShadowComparedCandidates -eq 0 -and
            $collisionCommittedCandidates -eq 0 -and
            $collisionPreparedPairs -eq 0 -and $collisionUniqueCandidates -eq 0 -and
            $collisionSubmittedJobs -eq 0 -and $collisionCompletedJobs -eq 0 -and
            $collisionPhysicalWorkerJobs -eq 0 -and
            $collisionOwnerHelpedJobs -eq 0 -and
            $collisionPhysicalWorkerMask -eq 0 -and
            $collisionDistinctPhysicalWorkers -eq 0 -and
            (Get-Stage5UInt64Field $collisionFields 'stale_rejections' $context) -eq 0) `
            "$context one-worker ineligible replay reports collision prepared/publication work."
    }

    $physicsLine = Get-Stage5SingleLine $Output 'PHYSICS_INTEGRATION_MANIFEST' $context
    $physicsFields = ConvertFrom-Stage5MetricLine $physicsLine `
        'PHYSICS_INTEGRATION_MANIFEST' "$context physics manifest"
	$physicsFieldNames = @('authoritative_batches', 'committed_prefixes', 'ranges',
		'submitted_jobs', 'completed_jobs', 'physical_worker_jobs',
		'owner_helped_jobs', 'physical_worker_mask', 'distinct_physical_workers',
		'physical_worker_mask_complete',
		'peak_concurrent_physical_workers', 'allocated_bytes', 'capture_ns',
        'prepare_ns', 'wait_ns', 'commit_ns', 'storage_bytes',
        'storage_capacity_bytes', 'storage_allocations', 'shadow_executions',
        'shadow_prefixes', 'shadow_ranges', 'shadow_submitted_jobs',
        'shadow_completed_jobs', 'shadow_matches', 'shadow_mismatches', 'owner_fallbacks',
        'ineligible_slices', 'unexpected_fallbacks', 'stale_rejections',
        'circuit_breaker_trips')
    foreach ($numeric in $physicsFieldNames) {
        Get-Stage5UInt64Field $physicsFields $numeric "$context physics manifest" | Out-Null
    }
    $physicsAuthoritativeBatches = Get-Stage5UInt64Field $physicsFields `
        'authoritative_batches' $context
    $physicsCommittedPrefixes = Get-Stage5UInt64Field $physicsFields `
        'committed_prefixes' $context
    $physicsRanges = Get-Stage5UInt64Field $physicsFields 'ranges' $context
    $physicsSubmittedJobs = Get-Stage5UInt64Field $physicsFields 'submitted_jobs' $context
    $physicsCompletedJobs = Get-Stage5UInt64Field $physicsFields 'completed_jobs' $context
	$physicsPhysicalWorkerJobs = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_jobs' $context
	$physicsOwnerHelpedJobs = Get-Stage5UInt64Field $physicsFields `
		'owner_helped_jobs' $context
	$physicsPhysicalWorkerMask = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_mask' $context
	$physicsDistinctPhysicalWorkers = Get-Stage5UInt64Field $physicsFields `
		'distinct_physical_workers' $context
	$physicsPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $physicsFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -le 1) `
		"$context physics physical-worker mask completeness is not boolean."
	$physicsPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $physicsFields `
		'peak_concurrent_physical_workers' $context
    $physicsShadowExecutions = Get-Stage5UInt64Field $physicsFields `
        'shadow_executions' $context
    $physicsShadowPrefixes = Get-Stage5UInt64Field $physicsFields `
        'shadow_prefixes' $context
    $physicsShadowRanges = Get-Stage5UInt64Field $physicsFields `
        'shadow_ranges' $context
    $physicsShadowSubmittedJobs = Get-Stage5UInt64Field $physicsFields `
        'shadow_submitted_jobs' $context
    $physicsShadowCompletedJobs = Get-Stage5UInt64Field $physicsFields `
        'shadow_completed_jobs' $context
    $physicsShadowMatches = Get-Stage5UInt64Field $physicsFields 'shadow_matches' $context
    $physicsShadowMismatches = Get-Stage5UInt64Field $physicsFields `
        'shadow_mismatches' $context
    $physicsOwnerFallbacks = Get-Stage5UInt64Field $physicsFields `
        'owner_fallbacks' $context
    $physicsStaleRejections = Get-Stage5UInt64Field $physicsFields `
        'stale_rejections' $context
    Assert-Stage5Condition ($physicsSubmittedJobs -eq $physicsCompletedJobs -and
        $physicsRanges -le $physicsSubmittedJobs) `
        "$context reports inconsistent physics ranges or submitted/completed jobs."
	[UInt64]$physicsMaskBitCount = Get-Stage5UInt64BitCount `
		$physicsPhysicalWorkerMask
	Assert-Stage5Condition (($physicsPhysicalWorkerJobs + $physicsOwnerHelpedJobs) -eq
		$physicsCompletedJobs) `
		"$context reports inconsistent physics execution identities."
	[UInt64]$physicsWorkerBound = Get-Stage5UInt64Field $fields 'workers' $context
	Assert-Stage5Condition (($physicsPhysicalWorkerJobs -eq 0) -eq
		($physicsDistinctPhysicalWorkers -eq 0)) `
		"$context physics physical-worker jobs and maximum distinct count disagree."
	Assert-Stage5Condition ($physicsWorkerBound -ge 64 -or
		($physicsPhysicalWorkerMask -shr [int]$physicsWorkerBound) -eq 0) `
		"$context physics physical-worker mask exceeds the effective worker lane."
	Assert-Stage5Condition ($physicsPhysicalWorkerMaskComplete -eq 1 -or
		$physicsWorkerBound -gt 64) `
		"$context physics physical-worker mask is incomplete inside the representable worker lane."
	if ($physicsPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
			$physicsMaskBitCount) `
			"$context complete physics worker-union mask omits the maximum per-batch distinct count."
	}
	Assert-Stage5Condition ($physicsDistinctPhysicalWorkers -le
		$physicsPhysicalWorkerJobs -and
		$physicsMaskBitCount -le $physicsPhysicalWorkerJobs -and
		$physicsDistinctPhysicalWorkers -le $physicsWorkerBound -and
		$physicsPeakConcurrentPhysicalWorkers -le $physicsDistinctPhysicalWorkers -and
		$physicsPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
		"$context reports impossible physics physical-worker evidence."
    Assert-Stage5Condition ($physicsShadowExecutions -eq
        ($physicsShadowMatches + $physicsShadowMismatches)) `
        "$context reports inconsistent physics shadow counters."
    Assert-Stage5Condition ($physicsStaleRejections -le $physicsOwnerFallbacks) `
        "$context reports more stale physics rejections than owner fallbacks."
    $physicsFallbackEligible = $Entry.simulationMode -ceq 'parallel' -and
        $physicsWorkerBound -gt 1
    if (-not $physicsFallbackEligible) {
        Assert-Stage5Condition ($physicsOwnerFallbacks -eq 0 -and
            $physicsStaleRejections -eq 0) `
            "$context reports physics owner fallback evidence outside an eligible multiworker parallel lane."
    }
    foreach ($zeroPhysicsInvariant in @('shadow_mismatches',
        'unexpected_fallbacks', 'circuit_breaker_trips')) {
        Assert-Stage5Condition ((Get-Stage5UInt64Field $physicsFields `
            $zeroPhysicsInvariant $context) -eq 0) `
            "$context reports forbidden physics evidence in '$zeroPhysicsInvariant'."
    }
    if ($physicsAuthoritativeBatches -gt 0) {
        Assert-Stage5Condition ($physicsCommittedPrefixes -gt 0 -and
            $physicsRanges -gt 0 -and $physicsSubmittedJobs -gt 0 -and
            $physicsCompletedJobs -gt 0) `
            "$context reports authoritative physics batches without physics-specific committed work and jobs."
    }
    Assert-Stage5Condition ($physicsShadowExecutions -eq 0 -and
        $physicsShadowPrefixes -eq 0 -and $physicsShadowRanges -eq 0 -and
        $physicsShadowSubmittedJobs -eq 0 -and
        $physicsShadowCompletedJobs -eq 0 -and
        $physicsShadowMatches -eq 0 -and $physicsShadowMismatches -eq 0) `
        "$context reports physics shadow work outside shadow simulation."
    $isQualifyingPhysicsStress = $Entry.stress -and
        $Entry.simulationMode -ceq 'parallel' -and
        $Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
	if ($isQualifyingPhysicsStress) {
        Assert-Stage5Condition ($physicsAuthoritativeBatches -gt 0 -and
            $physicsCommittedPrefixes -gt 0 -and $physicsRanges -gt 0 -and
			$physicsSubmittedJobs -gt 0 -and $physicsCompletedJobs -gt 0 -and
			$physicsPhysicalWorkerJobs -eq $physicsCompletedJobs -and
			$physicsOwnerHelpedJobs -eq 0 -and
			$physicsDistinctPhysicalWorkers -ge 2 -and
			$physicsPeakConcurrentPhysicalWorkers -ge 2) `
			"$context qualifying stress replay has no positive authoritative physics batch, prefix, range, and job evidence."
		Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
			$physicsPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying physics replay below the mask width reports incomplete physical-worker identity evidence."
    }
    if ($Entry.configuration -ceq 'serial-1' -or
        $Entry.configuration -ceq 'parallel-1') {
        Assert-Stage5Condition ($physicsAuthoritativeBatches -eq 0 -and
            $physicsCommittedPrefixes -eq 0 -and $physicsRanges -eq 0 -and
			$physicsSubmittedJobs -eq 0 -and $physicsCompletedJobs -eq 0 -and
			$physicsPhysicalWorkerJobs -eq 0 -and $physicsOwnerHelpedJobs -eq 0 -and
			$physicsPhysicalWorkerMask -eq 0 -and
			$physicsDistinctPhysicalWorkers -eq 0 -and
			$physicsPeakConcurrentPhysicalWorkers -eq 0) `
            "$context nonqualifying serial/one-worker replay reports physics authority or prepared jobs."
		foreach ($physicsPreparationField in @('allocated_bytes', 'capture_ns',
			'prepare_ns', 'wait_ns', 'commit_ns', 'storage_bytes',
			'storage_capacity_bytes', 'storage_allocations')) {
			Assert-Stage5Condition ((Get-Stage5UInt64Field $physicsFields `
				$physicsPreparationField $context) -eq 0) `
				"$context nonqualifying serial/one-worker replay reports physics pre-scan, capture, or storage work in '$physicsPreparationField'."
		}
    }

	$statusLine = Get-Stage5SingleLine $Output 'OBJECT_STATUS_TIMER_MANIFEST' $context
	$statusFields = ConvertFrom-Stage5MetricLine $statusLine `
		'OBJECT_STATUS_TIMER_MANIFEST' "$context status manifest"
	$statusFieldNames = @('authoritative_batches', 'committed_commands',
		'submitted_jobs', 'completed_jobs', 'physical_worker_jobs',
		'owner_helped_jobs', 'physical_worker_mask', 'distinct_physical_workers',
		'physical_worker_mask_complete',
		'peak_concurrent_physical_workers', 'shadow_executions', 'shadow_commands',
		'shadow_matches', 'shadow_mismatches', 'owner_fallbacks', 'stale_rejections')
	foreach ($numeric in $statusFieldNames) {
		Get-Stage5UInt64Field $statusFields $numeric "$context status manifest" | Out-Null
	}
	$statusAuthoritativeBatches = Get-Stage5UInt64Field $statusFields 'authoritative_batches' $context
	$statusCommittedCommands = Get-Stage5UInt64Field $statusFields 'committed_commands' $context
	$statusSubmittedJobs = Get-Stage5UInt64Field $statusFields 'submitted_jobs' $context
	$statusCompletedJobs = Get-Stage5UInt64Field $statusFields 'completed_jobs' $context
	$statusPhysicalWorkerJobs = Get-Stage5UInt64Field $statusFields 'physical_worker_jobs' $context
	$statusOwnerHelpedJobs = Get-Stage5UInt64Field $statusFields 'owner_helped_jobs' $context
	$statusPhysicalWorkerMask = Get-Stage5UInt64Field $statusFields 'physical_worker_mask' $context
	$statusDistinctPhysicalWorkers = Get-Stage5UInt64Field $statusFields 'distinct_physical_workers' $context
	$statusPhysicalWorkerMaskComplete = Get-Stage5UInt64Field $statusFields `
		'physical_worker_mask_complete' $context
	Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -le 1) `
		"$context status physical-worker mask completeness is not boolean."
	$statusPeakConcurrentPhysicalWorkers = Get-Stage5UInt64Field $statusFields `
		'peak_concurrent_physical_workers' $context
	$statusShadowExecutions = Get-Stage5UInt64Field $statusFields 'shadow_executions' $context
	$statusShadowCommands = Get-Stage5UInt64Field $statusFields 'shadow_commands' $context
	$statusShadowMatches = Get-Stage5UInt64Field $statusFields 'shadow_matches' $context
	$statusShadowMismatches = Get-Stage5UInt64Field $statusFields 'shadow_mismatches' $context
	$statusOwnerFallbacks = Get-Stage5UInt64Field $statusFields 'owner_fallbacks' $context
	$statusStaleRejections = Get-Stage5UInt64Field $statusFields 'stale_rejections' $context
	[UInt64]$statusMaskBitCount = Get-Stage5UInt64BitCount `
		$statusPhysicalWorkerMask
	Assert-Stage5Condition ($statusSubmittedJobs -eq $statusCompletedJobs -and
		($statusPhysicalWorkerJobs + $statusOwnerHelpedJobs) -eq $statusCompletedJobs) `
		"$context reports inconsistent status physical execution evidence."
	Assert-Stage5Condition (($statusPhysicalWorkerJobs -eq 0) -eq
		($statusDistinctPhysicalWorkers -eq 0)) `
		"$context status physical-worker jobs and maximum distinct count disagree."
	Assert-Stage5Condition ($physicsWorkerBound -ge 64 -or
		($statusPhysicalWorkerMask -shr [int]$physicsWorkerBound) -eq 0) `
		"$context status physical-worker mask exceeds the effective worker lane."
	Assert-Stage5Condition ($statusPhysicalWorkerMaskComplete -eq 1 -or
		$physicsWorkerBound -gt 64) `
		"$context status physical-worker mask is incomplete inside the representable worker lane."
	if ($statusPhysicalWorkerMaskComplete -eq 1) {
		Assert-Stage5Condition ($statusDistinctPhysicalWorkers -le
			$statusMaskBitCount) `
			"$context complete status worker-union mask omits the maximum per-batch distinct count."
	}
	Assert-Stage5Condition ($statusDistinctPhysicalWorkers -le $statusPhysicalWorkerJobs -and
		$statusMaskBitCount -le $statusPhysicalWorkerJobs -and
		$statusDistinctPhysicalWorkers -le $physicsWorkerBound -and
		$statusPeakConcurrentPhysicalWorkers -le $statusDistinctPhysicalWorkers -and
		$statusPeakConcurrentPhysicalWorkers -le $physicsWorkerBound) `
		"$context reports inconsistent status physical execution evidence."
	Assert-Stage5Condition ($statusShadowExecutions -eq
		($statusShadowMatches + $statusShadowMismatches) -and
		$statusShadowMismatches -eq 0 -and $statusOwnerFallbacks -eq 0 -and
		$statusStaleRejections -eq 0) `
		"$context reports forbidden status fallback, stale, or shadow mismatch evidence."
	$isQualifyingStatusStress = $Entry.stress -and
		$Entry.simulationMode -ceq 'parallel' -and
		$Entry.configuration -match '^parallel-(?:2|4|8|16|auto)$'
	if ($isQualifyingStatusStress) {
		Assert-Stage5Condition ($statusAuthoritativeBatches -gt 0 -and
			$statusCommittedCommands -gt 0 -and $statusSubmittedJobs -gt 0 -and
			$statusPhysicalWorkerJobs -eq $statusCompletedJobs -and
			$statusOwnerHelpedJobs -eq 0 -and
			$statusDistinctPhysicalWorkers -ge 2 -and
			$statusPeakConcurrentPhysicalWorkers -ge 2) `
			"$context qualifying stress replay has no physical live status authority."
		Assert-Stage5Condition ($physicsWorkerBound -gt 64 -or
			$statusPhysicalWorkerMaskComplete -eq 1) `
			"$context qualifying status replay below the mask width reports incomplete physical-worker identity evidence."
	}
	Assert-Stage5Condition ($statusShadowExecutions -eq 0 -and
		$statusShadowCommands -eq 0 -and $statusShadowMatches -eq 0) `
		"$context replay reports status shadow work outside shadow mode."
	if ($Entry.configuration -ceq 'serial-1' -or $Entry.configuration -ceq 'parallel-1') {
		Assert-Stage5Condition ($statusAuthoritativeBatches -eq 0 -and
			$statusCommittedCommands -eq 0 -and $statusSubmittedJobs -eq 0 -and
			$statusCompletedJobs -eq 0 -and $statusPhysicalWorkerJobs -eq 0 -and
			$statusOwnerHelpedJobs -eq 0 -and $statusPhysicalWorkerMask -eq 0 -and
			$statusDistinctPhysicalWorkers -eq 0 -and
			$statusPeakConcurrentPhysicalWorkers -eq 0) `
			"$context nonqualifying replay reports status authority."
	}

    $spatialLine = Get-Stage5SingleLine $Output 'IMMUTABLE_SPATIAL_MANIFEST' $context
    $spatialFields = ConvertFrom-Stage5MetricLine $spatialLine `
        'IMMUTABLE_SPATIAL_MANIFEST' "$context immutable-spatial manifest"
    $spatialEvidence = ConvertFrom-Stage5ImmutableSpatialFields $spatialFields `
        '' $Entry $context (Get-Stage5UInt64Field $fields 'workers' $context)

    $effectiveMode = Get-Stage5RequiredField $fields 'effective_mode' $context
    Assert-Stage5Condition ($effectiveMode -ceq 'serial' -or $effectiveMode -ceq 'parallel') `
        "$context effective_mode is not a supported serial/parallel value."
    $schedulerStarted = Get-Stage5UInt64Field $fields 'scheduler_started' $context
    $workers = Get-Stage5UInt64Field $fields 'workers' $context
    $submitted = Get-Stage5UInt64Field $fields 'submitted' $context
    $executed = Get-Stage5UInt64Field $fields 'executed' $context
    $fallback = Get-Stage5UInt64Field $fields 'fallback' $context
    $isExpectedOneWorkerFallback = $Entry.configuration -ceq 'parallel-1' -and
        ($fallback -gt 0 -or $submitted -eq 0)
    if ($Entry.configuration -ceq 'serial-1') {
        Assert-Stage5Condition ($effectiveMode -ceq 'serial') "$context did not remain serial."
        Assert-Stage5Condition ($schedulerStarted -eq 0 -and $workers -eq 0 -and
            $submitted -eq 0 -and $executed -eq 0) `
            "$context serial configuration reports an active scheduler, workers, or jobs."
    }
    elseif ($Entry.configuration -match '^parallel-(1|2|4|8|16)$') {
        $expectedWorkers = [UInt64]$Matches[1]
        Assert-Stage5Condition ($effectiveMode -ceq 'parallel') `
            "$context explicit parallel scheduler unexpectedly fell back to serial."
        Assert-Stage5Condition ($schedulerStarted -eq 1 -and $workers -eq $expectedWorkers) `
            "$context scheduler/worker count does not match explicit configuration '$($Entry.configuration)'."
        if (-not $isExpectedOneWorkerFallback) {
            Assert-Stage5Condition ($submitted -gt 0 -and $executed -gt 0 -and $fallback -eq 0) `
                "$context did not execute parallel jobs without fallback."
        }
    }
    elseif ($Entry.configuration -ceq 'parallel-auto') {
        Assert-Stage5Condition ($effectiveMode -ceq 'parallel' -and $schedulerStarted -eq 1 -and
            $workers -gt 0) "$context automatic configuration did not start workers."
        Assert-Stage5Condition ($submitted -gt 0 -and $executed -gt 0 -and $fallback -eq 0) `
            "$context automatic configuration did not execute parallel jobs without fallback."
    }
    else {
        throw "$context has unsupported worker configuration '$($Entry.configuration)'."
    }

    return [pscustomobject]@{
        line = $line
        effectiveMode = $effectiveMode
        workers = Get-Stage5UInt64Field $fields 'workers' $context
        availableCpus = Get-Stage5UInt64Field $fields 'available_cpus' $context
        selectedWorkerCpus = Get-Stage5UInt64Field $fields 'selected_worker_cpus' $context
        expectedOneWorkerFallback = $isExpectedOneWorkerFallback
        collisionAuthoritativeCommits = $collisionAuthoritativeCommits
        collisionShadowExecutions = $collisionShadowExecutions
        collisionShadowComparedCandidates = $collisionShadowComparedCandidates
        collisionOwnerFallbacks = $collisionOwnerFallbacks
        collisionCommittedCandidates = $collisionCommittedCandidates
        collisionSubmittedJobs = $collisionSubmittedJobs
        collisionCompletedJobs = $collisionCompletedJobs
        collisionPhysicalWorkerJobs = $collisionPhysicalWorkerJobs
        collisionOwnerHelpedJobs = $collisionOwnerHelpedJobs
        collisionPhysicalWorkerMask = $collisionPhysicalWorkerMask
        collisionDistinctPhysicalWorkers = $collisionDistinctPhysicalWorkers
        physicsAuthoritativeBatches = $physicsAuthoritativeBatches
        physicsCommittedPrefixes = $physicsCommittedPrefixes
        physicsRanges = $physicsRanges
        physicsSubmittedJobs = $physicsSubmittedJobs
        physicsCompletedJobs = $physicsCompletedJobs
		physicsPhysicalWorkerJobs = $physicsPhysicalWorkerJobs
		physicsOwnerHelpedJobs = $physicsOwnerHelpedJobs
		physicsDistinctPhysicalWorkers = $physicsDistinctPhysicalWorkers
		physicsPeakConcurrentPhysicalWorkers = $physicsPeakConcurrentPhysicalWorkers
        physicsShadowExecutions = $physicsShadowExecutions
        physicsShadowPrefixes = $physicsShadowPrefixes
        physicsShadowRanges = $physicsShadowRanges
        physicsShadowSubmittedJobs = $physicsShadowSubmittedJobs
        physicsShadowCompletedJobs = $physicsShadowCompletedJobs
        spatialEvidence = $spatialEvidence
        collisionFields = $collisionFields
        physicsFields = $physicsFields
		statusFields = $statusFields
        spatialFields = $spatialFields
        fields = $fields
    }
}

function Add-Stage5FinalAcceptanceValidatedClosureEntry {
    param([string]$Path, [string]$Sha256, [Int64]$Length)
    $collectorVariable = Get-Variable -Name `
        Stage5FinalAcceptanceValidatedClosure -Scope Script `
        -ErrorAction SilentlyContinue
    if ($null -eq $collectorVariable -or
        $collectorVariable.Value -isnot [Collections.IDictionary]) {
        return
    }
    $full = [IO.Path]::GetFullPath($Path)
    $collector = $collectorVariable.Value
    if ($collector.ContainsKey($full)) {
        $prior = $collector[$full]
        Assert-Stage5Condition ([string]$prior.sha256 -ceq
                $Sha256.ToUpperInvariant() -and
            [Int64]$prior.length -eq $Length) `
            "Final acceptance validated closure observed changing bytes at '$full'."
        return
    }
    $collector[$full] = [pscustomobject]@{
        path = $full
        sha256 = $Sha256.ToUpperInvariant()
        length = $Length
    }
}

function Get-Stage5FinalAcceptanceValidatedClosure {
    $collectorVariable = Get-Variable -Name `
        Stage5FinalAcceptanceValidatedClosure -Scope Script `
        -ErrorAction SilentlyContinue
    Assert-Stage5Condition ($null -ne $collectorVariable -and
        $collectorVariable.Value -is [Collections.IDictionary]) `
        'No completed final-acceptance validated closure is available.'
    return @($collectorVariable.Value.Values | Sort-Object path)
}

function Get-Stage5FileSha256 {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $stream = [IO.File]::OpenRead($full)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = (($sha.ComputeHash($stream) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
        $length = [Int64]$stream.Length
    }
    finally {
        $sha.Dispose()
        $stream.Dispose()
    }
    Add-Stage5FinalAcceptanceValidatedClosureEntry $full $hash $length
    return $hash
}

function ConvertFrom-Stage5ReplayResult {
    param([string]$Output, [object]$Entry)
    $context = "Replay validation entry $($Entry.sequence)"
    $line = Get-Stage5SingleLine $Output 'SIMULATION_REPLAY_RESULT' $context
    $fields = ConvertFrom-Stage5MetricLine $line 'SIMULATION_REPLAY_RESULT' "$context result"
    foreach ($required in @('replay', 'final_frame', 'final_crc')) {
        Get-Stage5RequiredField $fields $required "$context result" | Out-Null
    }
    Assert-Stage5Condition ((Get-Stage5RequiredField $fields 'replay' $context) -ceq $Entry.replayArgument) `
        "$context result replay path does not match the plan."
    $finalFrame = Get-Stage5UInt64Field $fields 'final_frame' "$context result"
    Assert-Stage5Condition ($finalFrame -gt 0) "$context result final_frame must be positive."
    $finalCRC = Get-Stage5RequiredField $fields 'final_crc' "$context result"
    Assert-Stage5Condition ($finalCRC -match '^[0-9A-Fa-f]{8}$') `
        "$context result final_crc must contain exactly eight hexadecimal characters."
    return [pscustomobject]@{
        line = $line
        finalFrame = $finalFrame
        finalCRC = $finalCRC.ToUpperInvariant()
        fields = $fields
    }
}

function Get-Stage5TimingEvidence {
    param([string]$TimingDirectory, [string]$Context)
    Assert-Stage5Condition (Test-Path -LiteralPath $TimingDirectory -PathType Container) `
        "$Context timing directory is missing."
    $files = @(Get-ChildItem -LiteralPath $TimingDirectory -Filter 'frame-timing-*.csv' -File)
    Assert-Stage5Condition ($files.Count -eq 1) `
        "$Context requires exactly one frame-timing CSV file."
    $expectedHeader = 'session,mode,frame_begin,frame_end,logic_frames,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms'
    $rawLines = @(Get-Content -LiteralPath $files[0].FullName)
    Assert-Stage5Condition ($rawLines.Count -gt 0) "$Context frame-timing CSV is empty."
    $header = [string]$rawLines[0]
    Assert-Stage5Condition ($header -ceq $expectedHeader) "$Context frame-timing CSV header is invalid."
    foreach ($rawRow in @($rawLines | Select-Object -Skip 1)) {
        Assert-Stage5Condition ($rawRow.Split(',').Count -eq 15) `
            "$Context frame-timing CSV row has an invalid column count."
    }
    $rows = @(Import-Csv -LiteralPath $files[0].FullName)
    Assert-Stage5Condition ($rows.Count -gt 0) "$Context frame-timing CSV contains no data rows."
    $phases = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $phaseSummaries = @{}
    [UInt64]$maximumFrameEnd = 0
    foreach ($row in $rows) {
        [UInt64]$frameBegin = 0
        [UInt64]$frameEnd = 0
        Assert-Stage5Condition ([UInt64]::TryParse([string]$row.frame_begin, [ref]$frameBegin) -and
            [UInt64]::TryParse([string]$row.frame_end, [ref]$frameEnd) -and $frameEnd -ge $frameBegin) `
            "$Context frame-timing row has an invalid frame range."
        foreach ($integerName in @('session', 'frame_begin', 'frame_end', 'logic_frames', 'samples',
            'over_33ms', 'over_100ms')) {
            [UInt64]$integerValue = 0
            Assert-Stage5Condition ([UInt64]::TryParse([string]$row.$integerName, [ref]$integerValue)) `
                "$Context frame-timing row has invalid $integerName."
            if ($integerName -ceq 'frame_end' -and $integerValue -gt $maximumFrameEnd) {
                $maximumFrameEnd = $integerValue
            }
            if ($integerName -ceq 'samples') {
                Assert-Stage5Condition ($integerValue -gt 0) `
                    "$Context frame-timing row samples must be positive."
            }
        }
        foreach ($decimalName in @('wall_ms', 'total_ms', 'avg_ms', 'p95_upper_ms',
            'p99_upper_ms', 'max_ms')) {
            [double]$decimalValue = 0
            Assert-Stage5Condition ([double]::TryParse([string]$row.$decimalName,
                [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
                [ref]$decimalValue) -and -not [double]::IsNaN($decimalValue) -and
                -not [double]::IsInfinity($decimalValue) -and $decimalValue -ge 0) `
                "$Context frame-timing row has invalid $decimalName."
        }
        Assert-Stage5Condition ([string]$row.mode -ceq 'headless') `
            "$Context frame-timing row must identify the headless validation mode."
        Assert-Stage5Condition ([string]$row.phase -match '^[a-z][a-z0-9_]*$') `
            "$Context frame-timing row phase is invalid."
        $phaseName = [string]$row.phase
        $phases.Add($phaseName) | Out-Null
        [UInt64]$rowSamples = 0
        [double]$rowTotalMilliseconds = 0
        [void][UInt64]::TryParse([string]$row.samples, [ref]$rowSamples)
        [void][double]::TryParse([string]$row.total_ms,
            [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
            [ref]$rowTotalMilliseconds)
        if (-not $phaseSummaries.ContainsKey($phaseName)) {
            $phaseSummaries[$phaseName] = [pscustomobject]@{
                phase = $phaseName; samples = [UInt64]0; totalMilliseconds = [double]0
            }
        }
        $phaseSummaries[$phaseName].samples = [UInt64](
            $phaseSummaries[$phaseName].samples + $rowSamples)
        $phaseSummaries[$phaseName].totalMilliseconds = [double](
            $phaseSummaries[$phaseName].totalMilliseconds + $rowTotalMilliseconds)
    }
    Assert-Stage5Condition ($maximumFrameEnd -gt 0) `
        "$Context frame-timing CSV has no positive final frame."
    Assert-Stage5Condition ($phases.Contains('frame') -and $phases.Contains('logic')) `
        "$Context frame-timing CSV must contain frame and logic phases."
    return [pscustomobject]@{
        file = $files[0].FullName
        sha256 = Get-Stage5FileSha256 $files[0].FullName
        header = $header
        rows = $rows.Count
        maximumFrameEnd = $maximumFrameEnd
        phases = @($phases | Sort-Object)
        phaseSummaries = @($phaseSummaries.Values | Sort-Object phase)
    }
}

function Get-Stage5DeterminismGroups {
    param([object[]]$Results, [string]$Context)
    return @($Results | Group-Object -Property {
        $value = if ($_ -is [Collections.IDictionary]) {
            Get-Stage5JsonValue $_ 'determinismKey' "$Context result"
        }
        else {
            $property = $_.PSObject.Properties['determinismKey']
            Assert-Stage5Condition ($null -ne $property) `
                "$Context result is missing property 'determinismKey'."
            $property.Value
        }
        Assert-Stage5Condition ($value -is [string] -and
            -not [string]::IsNullOrWhiteSpace($value)) `
            "$Context result determinismKey must be a nonempty JSON string."
        $value
    })
}

function Assert-Stage5AiDeterminism {
    param([object[]]$Results, [string[]]$ExpectedConfigurations, [int]$ExpectedRepeats,
        [string]$ShadowConfiguration = '', [string[]]$ExpectedDeterminismKeys = @())
    $aiResults = @($Results | Where-Object { $_.kind -ceq 'ai' })
    $regularResults = @($aiResults | Where-Object {
        $ExpectedConfigurations -ccontains $_.configuration
    })
    $knownConfigurations = @($ExpectedConfigurations)
    if (-not [string]::IsNullOrEmpty($ShadowConfiguration)) {
        $knownConfigurations += $ShadowConfiguration
    }
    Assert-Stage5Condition (@($aiResults | Where-Object {
        $knownConfigurations -cnotcontains $_.configuration
    }).Count -eq 0) 'AI results contain an unsupported worker configuration.'
    if ($ExpectedDeterminismKeys.Count -gt 0) {
        $expectedRegularCount = $ExpectedDeterminismKeys.Count *
            $ExpectedConfigurations.Count * $ExpectedRepeats
        Assert-Stage5Condition ($regularResults.Count -eq $expectedRegularCount) `
            "AI matrix has $($regularResults.Count) regular results; expected the complete $expectedRegularCount-result scenario/seed/configuration/repeat cross-product."
        $actualDeterminismKeys = @($regularResults | ForEach-Object { $_.determinismKey } |
            Sort-Object -Unique)
        Assert-Stage5Condition ($actualDeterminismKeys.Count -eq $ExpectedDeterminismKeys.Count -and
            @($actualDeterminismKeys | Where-Object { $ExpectedDeterminismKeys -cnotcontains $_ }).Count -eq 0) `
            'AI matrix determinism cases do not match the required scenario/seed cross-product.'
        $semanticRunKeys = @($regularResults | ForEach-Object {
            "$($_.determinismKey)|$($_.configuration)|$($_.repeat)"
        })
        Assert-Stage5Condition (@($semanticRunKeys | Sort-Object -Unique).Count -eq
            $semanticRunKeys.Count) `
            'AI matrix contains a duplicate scenario/seed/configuration/repeat result.'
    }
    foreach ($group in @(Get-Stage5DeterminismGroups $regularResults `
            'Stage 5 AI determinism')) {
        $expectedCount = $ExpectedConfigurations.Count * $ExpectedRepeats
        Assert-Stage5Condition ($group.Count -eq $expectedCount) `
            "AI case '$($group.Name)' has $($group.Count) results; expected $expectedCount."
        foreach ($configuration in $ExpectedConfigurations) {
            Assert-Stage5Condition (@($group.Group | Where-Object { $_.configuration -ceq $configuration }).Count -eq $ExpectedRepeats) `
                "AI case '$($group.Name)' is missing repeats for configuration '$configuration'."
        }
        $reference = $group.Group[0].aiEvidence
        foreach ($result in $group.Group) {
            Assert-Stage5Condition ($result.aiEvidence.finalDigest -ceq $reference.finalDigest) `
                "AI case '$($group.Name)' final_digest differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.aiEvidence.endFrame -eq $reference.endFrame) `
                "AI case '$($group.Name)' end_frame differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.aiEvidence.winnerTeam -eq $reference.winnerTeam) `
                "AI case '$($group.Name)' winner_team differs across repeats or worker configurations."
        }
    }
    if (-not [string]::IsNullOrEmpty($ShadowConfiguration)) {
        $shadowResults = @($aiResults | Where-Object {
            $_.configuration -ceq $ShadowConfiguration
        })
        Assert-Stage5Condition ($shadowResults.Count -eq 1) `
            "AI validation requires exactly one '$ShadowConfiguration' collision stress result."
        foreach ($shadowResult in $shadowResults) {
            $references = @($regularResults | Where-Object {
                $_.determinismKey -ceq $shadowResult.determinismKey
            })
            Assert-Stage5Condition ($references.Count -gt 0) `
                "AI shadow case '$($shadowResult.determinismKey)' has no regular matrix reference."
            $reference = $references[0].aiEvidence
            Assert-Stage5Condition ($shadowResult.aiEvidence.finalDigest -ceq $reference.finalDigest -and
                $shadowResult.aiEvidence.endFrame -eq $reference.endFrame -and
                $shadowResult.aiEvidence.winnerTeam -eq $reference.winnerTeam) `
                "AI shadow case '$($shadowResult.determinismKey)' differs from the regular matrix outcome."
        }
    }
}

function Assert-Stage5PlannedLiveWorkEvidence {
    param([object[]]$Results, [object]$ValidationPlan)
    $context = 'Stage 5 selected live capability evidence'
    $plannedLiveDiagnosticStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $writePlannedLiveDiagnostic = {
        param([string]$Phase, [string]$State, [int]$Index = 0,
            [int]$Total = 0)
        Write-Verbose (('STAGE5_PLANNED_LIVE_PHASE context={0} phase={1} ' +
            'state={2} index={3} total={4} elapsedMs={5}') -f $context,
            $Phase, $State, $Index, $Total,
            $plannedLiveDiagnosticStopwatch.ElapsedMilliseconds)
    }
    & $writePlannedLiveDiagnostic 'planned-live-function' 'start'
    $planObject = ConvertTo-Stage5LiveDictionary $ValidationPlan "$context plan"
    Assert-Stage5JsonProperties $planObject @('entries') "$context plan"
    $plannedAiCount = @($planObject.entries | Where-Object { $_.kind -ceq 'ai' }).Count
    Assert-Stage5Condition ($plannedAiCount -gt 0) "$context has no planned AI entries."
    # Validate the entire closed selector set even when no process result arrived.
    & $writePlannedLiveDiagnostic 'preflight-resolver' 'start'
    $requirementsMap = New-Stage5LiveValidationRequirementsMap -Plan $ValidationPlan
    [string[]]$requiredEntryIds = @($requirementsMap.Keys | ForEach-Object {
        [string]$_
    })
    Assert-Stage5Condition ($requiredEntryIds.Count -eq $plannedAiCount) `
        "$context frozen plan requirements map does not cover every planned AI entry."
    Get-Stage5LiveValidationRequirementsFromMap `
        -RequirementsMap $requirementsMap `
        -Entry $requirementsMap[$requiredEntryIds[0]] | Out-Null
    & $writePlannedLiveDiagnostic 'preflight-resolver' 'complete'
    $seen = @{}
    $verifiedOutcomes = @{}
    $aiResultIndex = 0
    & $writePlannedLiveDiagnostic 'ai-result-resolution-and-reparse' 'start' `
        0 $requiredEntryIds.Count
    foreach ($result in $Results) {
        if ($result.kind -cne 'ai') { continue }
        ++$aiResultIndex
        $reportAiResult = $aiResultIndex -eq 1 -or
            ($aiResultIndex % 32) -eq 0 -or
            $aiResultIndex -eq $requiredEntryIds.Count
        if ($reportAiResult) {
            & $writePlannedLiveDiagnostic 'per-result-resolver' 'start' `
                $aiResultIndex $requiredEntryIds.Count
        }
        $requirements = Get-Stage5LiveValidationRequirementsFromMap `
            -RequirementsMap $requirementsMap -Entry $result
        if ($reportAiResult) {
            & $writePlannedLiveDiagnostic 'per-result-resolver' 'complete' `
                $aiResultIndex $requiredEntryIds.Count
            & $writePlannedLiveDiagnostic 'per-result-ai-reparse' 'start' `
                $aiResultIndex $requiredEntryIds.Count
        }
        Assert-Stage5Condition (-not $seen.ContainsKey($requirements.entryId)) `
            "$context contains a duplicate required entry '$($requirements.entryId)'."
        $seen[$requirements.entryId] = $true
        $evidence = ConvertTo-Stage5LiveDictionary $result.aiEvidence "$context entry $($requirements.entryId)"
        Assert-Stage5JsonProperties $evidence @('line', 'fields', 'schemaStatus', 'invariantStatus',
            'capabilityProofStatus', 'validationRole', 'proofProfileId') "$context entry $($requirements.entryId)"
        Assert-Stage5Condition ($evidence.schemaStatus -ceq 'complete' -and
            $evidence.invariantStatus -ceq 'validated' -and
            $evidence.validationRole -ceq $requirements.validationRole -and
            $evidence.proofProfileId -ceq $requirements.proofProfileId) `
            "$context entry '$($requirements.entryId)' has missing or inconsistent role evidence."
        $retainedFields = ConvertTo-Stage5LiveDictionary $evidence.fields "$context raw fields"
        $executableHash = Get-Stage5JsonValue $retainedFields 'executable_sha256' "$context raw fields"
        # Reparse this exact result. Never union cached positive properties across runs.
        $verified = ConvertFrom-Stage5AiCompletionCore -Output $evidence.line `
            -Entry $result -ExecutableHash $executableHash `
            -RequireAuthoritativeWorkEvidence $true `
            -ValidatedLiveRequirements $requirements
        Assert-Stage5Condition ($retainedFields.Count -eq $verified.fields.Count) `
            "$context entry '$($requirements.entryId)' has an incomplete retained raw field set."
        foreach ($field in $verified.fields.Keys) {
            $retainedValue = Get-Stage5JsonValue $retainedFields $field "$context raw fields"
            Assert-Stage5Condition ($retainedValue -ceq $verified.fields[$field]) `
                "$context entry '$($requirements.entryId)' raw field '$field' differs from its completion line."
        }
        Assert-Stage5Condition ($evidence.capabilityProofStatus -ceq $verified.capabilityProofStatus) `
            "$context entry '$($requirements.entryId)' has an inconsistent capability claim."
        $verifiedOutcomes[$requirements.entryId] = $verified
        if ($reportAiResult) {
            & $writePlannedLiveDiagnostic 'per-result-ai-reparse' 'complete' `
                $aiResultIndex $requiredEntryIds.Count
        }
    }
    & $writePlannedLiveDiagnostic 'ai-result-resolution-and-reparse' 'complete' `
        $aiResultIndex $requiredEntryIds.Count
    & $writePlannedLiveDiagnostic 'planned-entry-coverage' 'start' `
        0 $requiredEntryIds.Count
    Assert-Stage5PlannedLiveEntryCoverage `
        -RequiredEntryIds $requiredEntryIds -Seen $seen -Context $context
    & $writePlannedLiveDiagnostic 'planned-entry-coverage' 'complete' `
        $requiredEntryIds.Count $requiredEntryIds.Count
    # Full matrix coverage remains a separate determinism gate. Bind this
    # selected shadow to its exact case using only the re-parsed raw outcomes.
    & $writePlannedLiveDiagnostic 'shadow-reference-comparison' 'start'
    $shadowOutcome = @($verifiedOutcomes.Values | Where-Object {
        $_.validationRole -ceq 'live-shadow-stress'
    })[0]
    $referenceOutcomes = @($verifiedOutcomes.Values | Where-Object {
        $_.validationRole -cne 'live-shadow-stress' -and
        $_.fields.scenario -ceq $shadowOutcome.fields.scenario -and
        [UInt64]$_.fields.seed -eq [UInt64]$shadowOutcome.fields.seed
    })
    Assert-Stage5Condition ($referenceOutcomes.Count -gt 0) `
        "$context shadow has no regular reference for its exact scenario and seed."
    foreach ($referenceOutcome in $referenceOutcomes) {
        Assert-Stage5Condition ($shadowOutcome.finalDigest -ceq $referenceOutcome.finalDigest -and
            $shadowOutcome.endFrame -eq $referenceOutcome.endFrame -and
            $shadowOutcome.winnerTeam -eq $referenceOutcome.winnerTeam) `
            "$context shadow differs from the regular matrix outcome."
    }
    & $writePlannedLiveDiagnostic 'shadow-reference-comparison' 'complete'
    & $writePlannedLiveDiagnostic 'planned-live-function' 'complete'
}

function Assert-Stage5AuthoritativeWorkEvidence {
    param([object[]]$Results, [object]$ValidationPlan = $null)
    if ($null -ne $ValidationPlan) {
        Assert-Stage5PlannedLiveWorkEvidence -Results $Results -ValidationPlan $ValidationPlan
        return
    }
    foreach ($result in $Results) {
        $legacyResult = ConvertTo-Stage5LiveDictionary $result 'Stage 5 legacy live result'
        Assert-Stage5Condition (-not $legacyResult.Contains('validationRole') -and
            -not $legacyResult.Contains('proofProfileId')) `
            'Stage 5 V2 role evidence requires its original validation plan.'
        if ($legacyResult.Contains('aiEvidence') -and $null -ne $legacyResult['aiEvidence']) {
            $legacyEvidence = ConvertTo-Stage5LiveDictionary $legacyResult['aiEvidence'] `
                'Stage 5 legacy live result evidence'
            foreach ($field in @('validationRole', 'proofProfileId')) {
                Assert-Stage5Condition (-not $legacyEvidence.Contains($field) -or
                    $null -eq $legacyEvidence[$field]) `
                    'Stage 5 nested V2 role evidence requires its original validation plan.'
            }
        }
    }
    $stressParallel = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.scenario -ceq '4v2' -and
        $_.configuration -match '^parallel-(?:2|4|8|16|auto)$'
    })
    Assert-Stage5Condition ($stressParallel.Count -gt 0) `
        'Overall Stage 5 acceptance requires a parallel AI stress scenario.'
    foreach ($result in $stressParallel) {
        Assert-Stage5Condition ($null -ne $result.aiEvidence -and
            $result.aiEvidence.authoritativeWorkStatus -ceq 'validated') `
            "AI stress entry $($result.sequence) is missing authoritative Stage 5 work evidence."
    }
    $authoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.aiParallelAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.aiSubmittedJobs -gt 0 -and
        $_.aiEvidence.aiCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($authoritative.Count -gt 0) `
        'AI stress evidence has no authoritative Stage 5 owner commit backed by AI-specific submitted/completed jobs; global or shadow-only scheduler activity is insufficient.'
    $collisionAuthoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.collisionAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.collisionCommittedCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0 -and
        $_.aiEvidence.collisionPhysicalWorkerJobs -gt 0 -and
        $_.aiEvidence.collisionDistinctPhysicalWorkers -ge 2
    })
    Assert-Stage5Condition ($collisionAuthoritative.Count -gt 0) `
        'AI stress evidence has no authoritative collision contact commit backed by collision-specific submitted/completed jobs; AI counters cannot proxy collision work.'
	$physicsAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.physicsAuthoritativeBatches -gt 0 -and
		$_.aiEvidence.physicsCommittedPrefixes -gt 0 -and
		$_.aiEvidence.physicsRanges -gt 0 -and
		$_.aiEvidence.physicsSubmittedJobs -gt 0 -and
		$_.aiEvidence.physicsCompletedJobs -gt 0
	})
	Assert-Stage5Condition ($physicsAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative physics prefix commit backed by physics-specific ranges and submitted/completed jobs; AI, collision, path, or global counters cannot proxy physics work.'
	$pathAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.pathWorkerExecuted -gt 1 -and
		$_.aiEvidence.pathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.pathAuthoritativeMultiWorkerCommits -gt 0 -and
		$_.aiEvidence.pathAuthoritativeCommits -gt 0
	})
	Assert-Stage5Condition ($pathAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative direct-path commit backed by a multi-request batch using more than one physical path worker; global scheduler, AI, collision, owner-help, or shadow counters cannot proxy path work.'
	$ordinaryPathAuthoritative = @($stressParallel | Where-Object {
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 1 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 1 -and
		$_.aiEvidence.ordinaryPathDistinctPhysicalWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeMultiWorkerCommits -gt 0
	})
	Assert-Stage5Condition ($ordinaryPathAuthoritative.Count -gt 0) `
		'AI stress evidence has no authoritative ordinary A* commit backed by concurrent physical-worker range jobs; compact-direct, global scheduler, AI, collision, owner-help, or shadow counters cannot proxy ordinary path work.'
    $spatialAuthoritative = @($stressParallel | Where-Object {
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($spatialAuthoritative.Count -gt 0) `
        'AI stress evidence has no authoritative immutable-spatial healing and point-defense-laser work backed by consumer-specific physical-worker jobs.'
    $coLocatedAuthoritative = @($stressParallel | Where-Object {
        $_.aiEvidence.aiParallelAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.aiSubmittedJobs -gt 0 -and
        $_.aiEvidence.aiCompletedJobs -gt 0 -and
        $_.aiEvidence.collisionAuthoritativeCommits -gt 0 -and
        $_.aiEvidence.collisionCommittedCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0 -and
        $_.aiEvidence.collisionPhysicalWorkerJobs -gt 0 -and
        $_.aiEvidence.collisionDistinctPhysicalWorkers -ge 2 -and
        $_.aiEvidence.physicsAuthoritativeBatches -gt 0 -and
        $_.aiEvidence.physicsCommittedPrefixes -gt 0 -and
        $_.aiEvidence.physicsRanges -gt 0 -and
        $_.aiEvidence.physicsSubmittedJobs -gt 0 -and
        $_.aiEvidence.physicsCompletedJobs -gt 0 -and
		$_.aiEvidence.pathWorkerExecuted -gt 1 -and
		$_.aiEvidence.pathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.pathAuthoritativeMultiWorkerCommits -gt 0 -and
        $_.aiEvidence.pathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 1 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 1 -and
		$_.aiEvidence.ordinaryPathDistinctPhysicalWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathPeakActiveWorkers -gt 1 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeMultiWorkerCommits -gt 0 -and
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.authoritativeCandidates -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($coLocatedAuthoritative.Count -gt 0) `
		'Overall Stage 5 acceptance requires AI, collision, physics, direct-path, ordinary-path, healing-spatial, and PDL-spatial authority on the same qualifying parallel 4v2 stress execution; evidence split across executions is insufficient.'
    $collisionShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.scenario -ceq '4v2' -and
        $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $_.aiEvidence.collisionShadowExecutions -gt 0 -and
        $_.aiEvidence.collisionShadowComparedCandidates -gt 0 -and
        $_.aiEvidence.collisionPreparedPairs -gt 0 -and
        $_.aiEvidence.collisionUniqueCandidates -gt 0 -and
        $_.aiEvidence.collisionSubmittedJobs -gt 0 -and
        $_.aiEvidence.collisionCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($collisionShadow.Count -gt 0) `
        'AI stress evidence has no installed shadow collision comparison covering a successful legacy insertion and backed by collision-specific prepared work and jobs.'
    $physicsShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.scenario -ceq '4v2' -and
        $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $_.aiEvidence.physicsShadowExecutions -gt 0 -and
        $_.aiEvidence.physicsShadowPrefixes -gt 0 -and
        $_.aiEvidence.physicsShadowRanges -gt 0 -and
        $_.aiEvidence.physicsShadowSubmittedJobs -gt 0 -and
        $_.aiEvidence.physicsShadowCompletedJobs -gt 0
    })
    Assert-Stage5Condition ($physicsShadow.Count -gt 0) `
        'AI stress evidence has no installed matching shadow physics comparison backed by physics-specific prefix, range, and submitted/completed jobs.'
	$ordinaryPathShadow = @($Results | Where-Object {
		$_.kind -ceq 'ai' -and $_.stress -and
		$_.scenario -ceq '4v2' -and
		$_.configuration -ceq 'shadow-16' -and
		$null -ne $_.aiEvidence -and
		$_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
		$_.aiEvidence.ordinaryPathShadowComparisons -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRequests -gt 0 -and
		$_.aiEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 0 -and
		$_.aiEvidence.ordinaryPathAuthoritativeCommits -eq 0
	})
	Assert-Stage5Condition ($ordinaryPathShadow.Count -gt 0) `
		'AI stress evidence has no installed ordinary-path shadow comparison backed by physical-worker request and range execution.'
    $spatialShadow = @($Results | Where-Object {
        $_.kind -ceq 'ai' -and $_.stress -and $_.scenario -ceq '4v2' -and
        $_.configuration -ceq 'shadow-16' -and
        $null -ne $_.aiEvidence -and
        $_.aiEvidence.authoritativeWorkStatus -ceq 'validated' -and
        $null -ne $_.aiEvidence.spatialEvidence -and
        $_.aiEvidence.spatialEvidence.healing.shadowQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.healing.shadowMatches -eq
            $_.aiEvidence.spatialEvidence.healing.shadowQueries -and
        $_.aiEvidence.spatialEvidence.healing.physicalWorkerJobs -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.shadowQueries -gt 0 -and
        $_.aiEvidence.spatialEvidence.pdl.shadowMatches -eq
            $_.aiEvidence.spatialEvidence.pdl.shadowQueries -and
        $_.aiEvidence.spatialEvidence.pdl.physicalWorkerJobs -gt 0
    })
    Assert-Stage5Condition ($spatialShadow.Count -gt 0) `
        'AI stress evidence has no installed matching immutable-spatial healing and PDL shadow comparison backed by consumer-specific physical-worker jobs.'
}

function Assert-Stage5CollisionTimingEvidence {
    param([object]$TimingEvidence, [object]$CollisionEvidence, [string]$Context)
    if ($null -eq $CollisionEvidence -or
        ($CollisionEvidence.collisionAuthoritativeCommits -eq 0 -and
         $CollisionEvidence.collisionShadowExecutions -eq 0)) {
        return
    }
    foreach ($requiredPhase in @('collision_admission', 'simulation_snapshot',
        'simulation_parallel', 'simulation_wait', 'simulation_reduce',
        'collision_live_validation', 'simulation_commit')) {
        Assert-Stage5Condition ($TimingEvidence.phases -contains $requiredPhase) `
            "$Context collision evidence is missing timing phase '$requiredPhase'."
    }
    if ($CollisionEvidence.collisionShadowExecutions -gt 0) {
        foreach ($shadowPhase in @('collision_existing_filter',
            'collision_commit_prepare', 'simulation_shadow_compare')) {
            Assert-Stage5Condition ($TimingEvidence.phases -contains $shadowPhase) `
                "$Context collision shadow evidence is missing timing phase '$shadowPhase'."
        }
    }
}

function Assert-Stage5ReplayDeterminism {
    param([object[]]$Results)
    $replayResults = @($Results | Where-Object { $_.kind -ceq 'replay' })
    foreach ($group in @(Get-Stage5DeterminismGroups $replayResults `
            'Stage 5 replay determinism')) {
        $reference = $group.Group[0].replayResult
        foreach ($result in $group.Group) {
            Assert-Stage5Condition ($result.replayResult.finalFrame -eq $reference.finalFrame) `
                "Replay '$($group.Name)' final_frame differs across repeats or worker configurations."
            Assert-Stage5Condition ($result.replayResult.finalCRC -ceq $reference.finalCRC) `
                "Replay '$($group.Name)' final_crc differs across repeats or worker configurations."
        }
    }
}

function Get-Stage5Median {
    param([double[]]$Values)
    Assert-Stage5Condition ($Values.Count -gt 0) 'Median requires at least one value.'
    Assert-Stage5Condition (@($Values | Where-Object {
        [double]::IsNaN($_) -or [double]::IsInfinity($_)
    }).Count -eq 0) 'Median requires finite values.'
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Read-Stage5PerformanceBaseline {
    param([string]$Path, [string]$StressFixtureSha256, [string]$ExpectedExecutableSha256)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($Path)) `
        'Stage3PerformanceBaselinePath is required when the performance gate is requested.'
    Assert-Stage5Condition ($ExpectedExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'ExpectedStage3ExecutableSha256 is required and must contain exactly 64 hexadecimal characters.'
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Stage 3 performance baseline was not found: $full"
    $baseline = ConvertFrom-Stage5JsonDictionary $full
    $baselineNames = @('schemaVersion', 'stage', 'architecture', 'executableSha256',
        'fixtureSha256', 'configuration', 'physicalCoreCount', 'availableCpus',
        'warmupRuns', 'wallMilliseconds')
    Assert-Stage5JsonShape $baseline $baselineNames 'Stage 3 performance baseline'
    $schemaVersion = Get-Stage5JsonValue $baseline 'schemaVersion' 'Stage 3 performance baseline'
    $stage = Get-Stage5JsonValue $baseline 'stage' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $stage -is [string] -and $stage -ceq 'Stage3') `
        'Stage 3 performance baseline identity is invalid.'
    $baselineExecutableHash = Get-Stage5JsonValue $baseline 'executableSha256' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($baselineExecutableHash -is [string] -and
        $baselineExecutableHash -match '^[0-9A-Fa-f]{64}$') `
        'Stage 3 performance baseline requires an exact executable SHA-256.'
    Assert-Stage5Condition ($baselineExecutableHash.ToUpperInvariant() -ceq
        $ExpectedExecutableSha256.ToUpperInvariant()) `
        'Stage 3 performance baseline executable hash does not match the independently supplied expected hash.'
    $baselineFixtureHash = Get-Stage5JsonValue $baseline 'fixtureSha256' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($baselineFixtureHash -is [string] -and
        $baselineFixtureHash.ToUpperInvariant() -ceq $StressFixtureSha256.ToUpperInvariant()) `
        'Stage 3 performance baseline fixture hash does not match the current stress fixture.'
    $architecture = Get-Stage5JsonValue $baseline 'architecture' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($architecture -is [string] -and $architecture -ceq 'x64') `
        'Stage 3 performance baseline must identify the native x64 architecture.'
    $configuration = Get-Stage5JsonValue $baseline 'configuration' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($configuration -is [string] -and $configuration -ceq 'parallel-1') `
        'Stage 3 performance baseline must use the forced one-worker configuration.'
    $baselinePhysicalCores = Get-Stage5JsonValue $baseline 'physicalCoreCount' 'Stage 3 performance baseline'
    $baselineAvailableCpus = Get-Stage5JsonValue $baseline 'availableCpus' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $baselinePhysicalCores) -and
        (Test-Stage5JsonInteger $baselineAvailableCpus) -and $baselinePhysicalCores -gt 0 -and
        $baselineAvailableCpus -gt 0) `
        'Stage 3 performance baseline requires physical-core and available-CPU topology evidence.'
    $baselineWarmupRuns = Get-Stage5JsonValue $baseline 'warmupRuns' 'Stage 3 performance baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $baselineWarmupRuns) -and $baselineWarmupRuns -ge 1) `
        'Stage 3 performance baseline must identify at least one warm-up run.'
    $sampleValues = Get-Stage5JsonValue $baseline 'wallMilliseconds' 'Stage 3 performance baseline'
    Assert-Stage5Condition ($sampleValues -is [Array]) `
        'Stage 3 performance baseline wallMilliseconds must be a JSON array.'
    Assert-Stage5Condition (@($sampleValues | Where-Object { -not (Test-Stage5JsonNumber $_) }).Count -eq 0) `
        'Stage 3 performance baseline wallMilliseconds must contain only finite JSON numbers.'
    $samples = @($sampleValues | ForEach-Object { [double]$_ })
    Assert-Stage5Condition ($samples.Count - [int]$baselineWarmupRuns -ge 3) `
        'Stage 3 performance baseline requires at least three measured runs after warm-up.'
    Assert-Stage5Condition (@($samples | Where-Object { $_ -le 0 }).Count -eq 0) `
        'Stage 3 performance baseline wall times must be positive.'
    return [pscustomobject]@{
        file = $full
        fileSha256 = Get-Stage5FileSha256 $full
        executableSha256 = $baselineExecutableHash.ToUpperInvariant()
        fixtureSha256 = $baselineFixtureHash.ToUpperInvariant()
        physicalCoreCount = [int]$baselinePhysicalCores
        availableCpus = [int]$baselineAvailableCpus
        warmupRuns = [int]$baselineWarmupRuns
        wallMilliseconds = $samples
        expectedExecutableSha256 = $ExpectedExecutableSha256.ToUpperInvariant()
        evidenceFile = $null
        measuredMedianMilliseconds = Get-Stage5Median @($samples | Select-Object -Skip ([int]$baselineWarmupRuns))
    }
}

function Get-Stage5MeasuredConfiguration {
    param([object[]]$Results, [string]$Configuration, [int]$WarmupRuns,
        [int]$MinimumMeasuredRuns)
    $ordered = @($Results | Where-Object { $_.configuration -ceq $Configuration } | Sort-Object sequence)
    Assert-Stage5Condition ($ordered.Count - $WarmupRuns -ge $MinimumMeasuredRuns) `
        "Performance configuration '$Configuration' requires one warm-up and at least $MinimumMeasuredRuns measured runs."
    $measured = @($ordered | Select-Object -Skip $WarmupRuns)
    Assert-Stage5Condition (@($ordered | Where-Object {
        $wall = [double]$_.wallMilliseconds
        [double]::IsNaN($wall) -or [double]::IsInfinity($wall) -or $wall -le 0
    }).Count -eq 0) `
        "Performance configuration '$Configuration' contains a non-finite or non-positive wall time."
    $workerCounts = @($measured | ForEach-Object { [UInt64]$_.replayMetrics.workers } | Select-Object -Unique)
    $availableCpuCounts = @($measured | ForEach-Object {
        [UInt64]$_.replayMetrics.availableCpus
    } | Select-Object -Unique)
    $selectedWorkerCpuCounts = @($measured | ForEach-Object {
        [UInt64]$_.replayMetrics.selectedWorkerCpus
    } | Select-Object -Unique)
    Assert-Stage5Condition ($workerCounts.Count -eq 1 -and $availableCpuCounts.Count -eq 1 -and
        $selectedWorkerCpuCounts.Count -eq 1) `
        "Performance configuration '$Configuration' topology varies across measured runs."
    $collisionPhaseNames = @('collision_admission', 'simulation_snapshot',
        'simulation_parallel', 'simulation_wait', 'simulation_reduce',
        'collision_live_validation', 'collision_existing_filter',
        'simulation_commit', 'collision_commit_prepare',
        'simulation_shadow_compare')
    $collisionPhaseEvidence = @($measured | ForEach-Object {
        $timingProperty = $_.PSObject.Properties['timingEvidence']
        $summaries = if ($null -ne $timingProperty -and
            $null -ne $timingProperty.Value -and
            $null -ne $timingProperty.Value.PSObject.Properties['phaseSummaries']) {
            @($timingProperty.Value.phaseSummaries | Where-Object {
                $collisionPhaseNames -ccontains $_.phase
            })
        }
        else { @() }
        [pscustomobject]@{ sequence = $_.sequence; phases = $summaries }
    })
    return [pscustomobject]@{
        configuration = $Configuration
        warmupRuns = $WarmupRuns
        measuredRuns = $measured.Count
        rawWallMilliseconds = @($ordered | ForEach-Object { [double]$_.wallMilliseconds })
        warmupWallMilliseconds = @($ordered | Select-Object -First $WarmupRuns |
            ForEach-Object { [double]$_.wallMilliseconds })
        measuredWallMilliseconds = @($measured | ForEach-Object { [double]$_.wallMilliseconds })
        medianWallMilliseconds = Get-Stage5Median @($measured | ForEach-Object { [double]$_.wallMilliseconds })
        workers = [UInt64]$workerCounts[0]
        availableCpus = [UInt64]$availableCpuCounts[0]
        selectedWorkerCpus = [UInt64]$selectedWorkerCpuCounts[0]
        collisionPhaseEvidence = $collisionPhaseEvidence
    }
}

function Measure-Stage5Performance {
    param([object[]]$Results, [int]$PhysicalCoreCount, [object]$Stage3Baseline,
        [int]$WarmupRuns = 1, [int]$MinimumMeasuredRuns = 3)
    Assert-Stage5Condition ($null -ne $Stage3Baseline) `
        'Stage 3 performance baseline evidence is required for the performance gate.'
    $stress = @($Results | Where-Object { $_.kind -ceq 'replay' -and $_.stress })
    Assert-Stage5Condition ($stress.Count -gt 0) 'Performance gate has no stress replay results.'
    $one = Get-Stage5MeasuredConfiguration $stress 'parallel-1' $WarmupRuns $MinimumMeasuredRuns
    $eight = Get-Stage5MeasuredConfiguration $stress 'parallel-8' $WarmupRuns $MinimumMeasuredRuns
    $regressionRatio = $one.medianWallMilliseconds / $Stage3Baseline.measuredMedianMilliseconds
    $speedup8 = $one.medianWallMilliseconds / $eight.medianWallMilliseconds
    $report = [ordered]@{
        schemaVersion = 2
        status = 'passed'
        measurementScope = 'aggregate-stage5-stress-replay-throughput'
        collisionSpecificSpeedupClaim = $false
        collisionEvidenceScope = 'separate-live-stress-authority-and-frame-phase-evidence'
        collisionTimingPhases = @('collision_admission', 'simulation_snapshot',
            'simulation_parallel', 'simulation_wait', 'simulation_reduce',
            'collision_live_validation', 'collision_existing_filter',
            'simulation_commit', 'collision_commit_prepare',
            'simulation_shadow_compare')
        physicalCoreCount = $PhysicalCoreCount
        warmupRuns = $WarmupRuns
        minimumMeasuredRuns = $MinimumMeasuredRuns
        stage3BaselineSourceFile = $Stage3Baseline.file
        stage3BaselineEvidenceFile = $Stage3Baseline.evidenceFile
        stage3ExecutableSha256 = $Stage3Baseline.executableSha256
        independentlyExpectedStage3ExecutableSha256 = $Stage3Baseline.expectedExecutableSha256
        stage3BaselineFileSha256 = $Stage3Baseline.fileSha256
        stage3RawWallMilliseconds = $Stage3Baseline.wallMilliseconds
        stage3PhysicalCoreCount = $Stage3Baseline.physicalCoreCount
        stage3AvailableCpus = $Stage3Baseline.availableCpus
        stage3OneWorkerMedianMilliseconds = $Stage3Baseline.measuredMedianMilliseconds
        currentOneWorker = $one
        currentEightWorker = $eight
        oneWorkerRegressionRatio = $regressionRatio
        eightWorkerSpeedup = $speedup8
        sixteenWorker = $null
        eightToSixteenSpeedup = $null
        sixteenWorkerStatus = 'unsupported-host-topology'
        failures = @()
    }
    $failures = New-Object 'Collections.Generic.List[string]'
    if ($Stage3Baseline.physicalCoreCount -ne $PhysicalCoreCount -or
        $Stage3Baseline.availableCpus -ne $one.availableCpus -or
        $eight.availableCpus -ne $one.availableCpus) {
        $failures.Add('Stage 3 baseline topology does not match the current one-worker measurement topology.') | Out-Null
        $report.status = 'failed'
    }
    $eightWorkerTopologyUnsupported = $PhysicalCoreCount -lt 8 -or
        $eight.availableCpus -lt 8 -or $eight.workers -lt 8 -or
        $eight.selectedWorkerCpus -lt 8
    if ($eightWorkerTopologyUnsupported) {
        $report.status = 'unsupported-host-topology'
        $failures.Add('Eight-worker performance target requires at least eight physical cores, available CPUs, workers, and selected worker CPUs.') | Out-Null
    }
    else {
        if ($speedup8 -lt 2.0) {
            $failures.Add(('Eight-worker median throughput speedup {0:N3}x is below 2.0x.' -f $speedup8)) | Out-Null
        }
    }
    if ($regressionRatio -gt 1.05) {
        $failures.Add(('One-worker median wall-time ratio {0:N3} exceeds the 1.05 Stage 3 regression limit.' -f $regressionRatio)) | Out-Null
        $report.status = 'failed'
    }
    if ($PhysicalCoreCount -ge 16) {
        $sixteen = Get-Stage5MeasuredConfiguration $stress 'parallel-16' $WarmupRuns $MinimumMeasuredRuns
        $report.sixteenWorker = $sixteen
        if ($sixteen.availableCpus -ne $one.availableCpus -or $sixteen.availableCpus -lt 16 -or
            $sixteen.workers -lt 16 -or $sixteen.selectedWorkerCpus -lt 16) {
            $report.sixteenWorkerStatus = 'unsupported-runtime-topology'
            $failures.Add('Host has 16 physical cores but the runtime did not expose 16 effective selected worker CPUs.') | Out-Null
        }
        else {
            $scale16 = $eight.medianWallMilliseconds / $sixteen.medianWallMilliseconds
            $report.eightToSixteenSpeedup = $scale16
            $report.sixteenWorkerStatus = if ($scale16 -gt 1.0) { 'passed' } else { 'failed' }
            if ($scale16 -le 1.0) {
                $failures.Add(('Sixteen-worker median throughput did not scale positively from eight workers ({0:N3}x).' -f $scale16)) | Out-Null
            }
        }
    }
    if ($failures.Count -gt 0 -and $report.status -ceq 'passed') { $report.status = 'failed' }
    if ($eightWorkerTopologyUnsupported) { $report.status = 'unsupported-host-topology' }
    $report.failures = $failures.ToArray()
    return [pscustomobject]$report
}

function Invoke-Stage5RegistryRestore {
    param([object[]]$Snapshots, [scriptblock]$RestoreAction)
    Assert-Stage5Condition ($null -ne $RestoreAction) 'Registry restore action is required.'
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $Snapshots.Count - 1; $index -ge 0; --$index) {
        try {
            & $RestoreAction $Snapshots[$index]
        }
        catch {
            $errors.Add("snapshot index $index`: $($_.Exception.Message)") | Out-Null
        }
    }
    if ($errors.Count -gt 0) {
        throw "Registry restoration failed after attempting every snapshot: $($errors -join ' | ')"
    }
}

function Invoke-Stage5RegistrySetupTransaction {
    param([string[]]$SubKeys, [scriptblock]$EnsureSubKeyAction,
        [scriptblock]$CaptureValueAction, [scriptblock]$SetValueAction,
        [scriptblock]$RegisterSnapshotAction, [scriptblock]$RestoreValueAction,
        [scriptblock]$CleanupCreatedSubKeysAction, [object]$ActionContext = $null,
        [scriptblock]$AfterSegmentAction = $null,
        [scriptblock]$AfterValueWriteAction = $null)
    Assert-Stage5Condition ($null -ne $EnsureSubKeyAction -and
        $null -ne $CaptureValueAction -and $null -ne $SetValueAction -and
        $null -ne $RegisterSnapshotAction -and $null -ne $RestoreValueAction -and
        $null -ne $CleanupCreatedSubKeysAction) `
        'Registry setup transaction requires every setup, registration, and rollback action.'
    $createdSubKeys = New-Object 'Collections.Generic.List[string]'
    $snapshot = $null
    $captureComplete = $false
    $setAttempted = $false
    try {
        foreach ($subKey in $SubKeys) {
            if ([bool](& $EnsureSubKeyAction $subKey $ActionContext)) {
                $createdSubKeys.Add($subKey) | Out-Null
            }
            if ($null -ne $AfterSegmentAction) {
                & $AfterSegmentAction $subKey $createdSubKeys.Count $ActionContext | Out-Null
            }
        }
        $snapshot = & $CaptureValueAction $createdSubKeys.ToArray() $ActionContext
        Assert-Stage5Condition ($null -ne $snapshot) `
            'Registry setup transaction did not capture an original-value snapshot.'
        $captureComplete = $true
        $setAttempted = $true
        & $SetValueAction $snapshot $ActionContext | Out-Null
        if ($null -ne $AfterValueWriteAction) {
            & $AfterValueWriteAction $snapshot $ActionContext | Out-Null
        }
        & $RegisterSnapshotAction $snapshot $ActionContext | Out-Null
        return
    }
    catch {
        $setupError = $_.Exception.Message
        $rollbackErrors = New-Object 'Collections.Generic.List[string]'
        if ($setAttempted -and $captureComplete) {
            try { & $RestoreValueAction $snapshot $ActionContext | Out-Null }
            catch { $rollbackErrors.Add("value restore: $($_.Exception.Message)") | Out-Null }
        }
        try { & $CleanupCreatedSubKeysAction $createdSubKeys.ToArray() $ActionContext | Out-Null }
        catch { $rollbackErrors.Add("created-key cleanup: $($_.Exception.Message)") | Out-Null }
        $rollbackStatus = if ($rollbackErrors.Count -eq 0) {
            'completed'
        }
        else {
            $rollbackErrors -join ' | '
        }
        throw "Registry setup transaction failed: setup: $setupError | rollback: $rollbackStatus"
    }
}

function Test-Stage5RegistryLeafRemoval {
    param([bool]$HadKey, [string[]]$ValueNames, [string[]]$SubKeyNames)
    return -not $HadKey -and @($ValueNames).Count -eq 0 -and @($SubKeyNames).Count -eq 0
}

function Invoke-Stage5CreatedRegistryKeyCleanup {
    param([string[]]$CreatedSubKeys, [scriptblock]$InspectAction,
        [scriptblock]$RemoveAction, [object]$ActionContext = $null)
    Assert-Stage5Condition ($null -ne $InspectAction -and $null -ne $RemoveAction) `
        'Created registry key cleanup requires inspect and remove actions.'
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $CreatedSubKeys.Count - 1; $index -ge 0; --$index) {
        try {
            $state = & $InspectAction $CreatedSubKeys[$index] $ActionContext
            if ($null -eq $state) { continue }
            if (Test-Stage5RegistryLeafRemoval $false @($state.valueNames) @($state.subKeyNames)) {
                & $RemoveAction $CreatedSubKeys[$index] $ActionContext
            }
        }
        catch {
            $errors.Add("'$($CreatedSubKeys[$index])': $($_.Exception.Message)") | Out-Null
        }
    }
    if ($errors.Count -gt 0) {
        throw "Created registry key cleanup failed after attempting every created key: $($errors -join ' | ')"
    }
}

function Get-Stage5FinalAcceptanceFileSha256 {
    param([string]$Path)
    return (Get-Stage5FinalAcceptanceFileSnapshot $Path 'final-acceptance file').sha256
}

function Assert-Stage5FinalAcceptancePathContained {
    param([string]$BaseDirectory, [string]$CandidatePath, [string]$Context)
    foreach ($rawPath in @($BaseDirectory, $CandidatePath)) {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($rawPath) -and
            $rawPath -notmatch '(^|[\\/])\.(?:\.?)(?:[\\/]|$)') `
            "$Context path contains an explicit dot segment and is not an immutable canonical path."
    }
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Stage5Condition ($baseRoot -is [string] -and
        $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."
    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    Assert-Stage5Condition ($candidateParts.Count -gt $baseParts.Count) `
        "$Context path must identify a file below its manifest directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Stage5Condition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its manifest directory."
    }
}

function Assert-Stage5FinalAcceptanceNoReparsePath {
    param([string]$BaseDirectory, [string]$CandidatePath, [string]$Context)
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    Assert-Stage5FinalAcceptancePathContained $base $candidate $Context
    $baseRoot = [IO.Path]::GetPathRoot($base)
    if ($base.Length -gt $baseRoot.Length) {
        $base = $base.TrimEnd([char[]]@(
            [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    }
    $baseItem = Get-Item -LiteralPath $base -Force -ErrorAction Stop
    Assert-Stage5Condition (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context base directory is a reparse point."
    $relative = $candidate.Substring($base.Length).TrimStart([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    $current = $base
    foreach ($segment in @($relative -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $current = Join-Path $current $segment
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Stage5Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$segment' is a reparse point."
    }
    # The manifest base can itself be below a junction/symlink.  Walk from the
    # volume root as well; checking only the lexical manifest prefix is not a
    # containment proof when an ancestor is later reparsed.
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    $rootRelative = $candidate.Substring($candidateRoot.Length)
    $rootCurrent = $candidateRoot
    foreach ($segment in @($rootRelative -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $rootCurrent = Join-Path $rootCurrent $segment
        $rootItem = Get-Item -LiteralPath $rootCurrent -Force -ErrorAction Stop
        Assert-Stage5Condition (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context ancestor path component '$segment' is a reparse point."
    }
}

function Initialize-Stage5FinalAcceptancePathNative {
    if (-not ('Stage5FinalAcceptancePathNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class Stage5FinalAcceptancePathNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct BY_HANDLE_FILE_INFORMATION {
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }
    [DllImport("kernel32.dll", EntryPoint="GetFinalPathNameByHandleW",
        CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
    public static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file, StringBuilder path, uint capacity, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetFileInformationByHandle(
        SafeFileHandle file, out BY_HANDLE_FILE_INFORMATION information);
    [StructLayout(LayoutKind.Sequential)]
    private struct FILE_DISPOSITION_INFORMATION {
        [MarshalAs(UnmanagedType.Bool)]
        public bool DeleteFile;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFileInformationByHandle(
        SafeFileHandle file, int informationClass,
        ref FILE_DISPOSITION_INFORMATION information, uint bufferSize);
    [DllImport("kernel32.dll", EntryPoint="CreateFileW", CharSet=CharSet.Unicode,
        ExactSpelling=true, SetLastError=true)]
    public static extern SafeFileHandle CreateFileW(string path, uint access,
        uint share, IntPtr securityAttributes, uint creationDisposition,
        uint flagsAndAttributes, IntPtr templateFile);
    [DllImport("kernel32.dll", EntryPoint="MoveFileExW", CharSet=CharSet.Unicode,
        ExactSpelling=true, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileExW(string existingPath,
        string destinationPath, uint flags);
    public static bool MarkFileForDeletion(SafeFileHandle file) {
        FILE_DISPOSITION_INFORMATION information =
            new FILE_DISPOSITION_INFORMATION { DeleteFile = true };
        return SetFileInformationByHandle(file, 4, ref information,
            (uint)Marshal.SizeOf(information));
    }
}
'@
    }
}

function Assert-Stage5FinalAcceptanceFileHandlePath {
    param([IO.FileStream]$Stream, [string]$ExpectedPath, [string]$Context)
    Assert-Stage5Condition ($null -ne $Stream -and
        ($Stream.CanRead -or $Stream.CanWrite)) `
        "$Context opened file handle is not usable."
    Assert-Stage5Condition (-not $Stream.SafeFileHandle.IsClosed -and
        -not $Stream.SafeFileHandle.IsInvalid) `
        "$Context opened file handle is not live."
    Initialize-Stage5FinalAcceptancePathNative
    $buffer = New-Object Text.StringBuilder 512
    $length = [Stage5FinalAcceptancePathNative]::GetFinalPathNameByHandleW(
        $Stream.SafeFileHandle, $buffer, [UInt32]$buffer.Capacity, 0)
    Assert-Stage5Condition ($length -gt 0 -and $length -lt 32768) `
        "$Context opened handle path could not be resolved."
    if ($length -ge $buffer.Capacity) {
        $buffer = New-Object Text.StringBuilder ([int]$length + 1)
        $length = [Stage5FinalAcceptancePathNative]::GetFinalPathNameByHandleW(
            $Stream.SafeFileHandle, $buffer, [UInt32]$buffer.Capacity, 0)
    }
    Assert-Stage5Condition ($length -gt 0 -and $length -lt $buffer.Capacity) `
        "$Context opened handle path is unavailable or exceeds its bound."
    $nativePath = $buffer.ToString()
    Assert-Stage5Condition ($nativePath.StartsWith('\\?\',
            [StringComparison]::Ordinal) -and $nativePath.Length -ge 7 -and
        $nativePath.Substring(4, 2) -cmatch '^[A-Za-z]:$' -and
        $nativePath[6] -eq [char]92) `
        "$Context opened handle must resolve to a local DOS volume path."
    $resolved = [IO.Path]::GetFullPath($nativePath.Substring(4))
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPath)) {
        $expected = [IO.Path]::GetFullPath($ExpectedPath)
        Assert-Stage5Condition ([String]::Equals($resolved, $expected,
                [StringComparison]::OrdinalIgnoreCase)) `
            "$Context opened handle resolves to a different path: $resolved"
    }
    $root = [IO.Path]::GetPathRoot($resolved)
    $volume = New-Object IO.DriveInfo $root
    Assert-Stage5Condition ($volume.DriveType -eq [IO.DriveType]::Fixed -and
        $volume.DriveFormat -ceq 'NTFS') `
        "$Context opened handle must reside on a fixed NTFS volume."
    return $resolved
}

function Get-Stage5FinalAcceptanceHandleIdentity {
    param([Microsoft.Win32.SafeHandles.SafeFileHandle]$Handle,
        [string]$Context)
    Assert-Stage5Condition ($null -ne $Handle -and -not $Handle.IsClosed -and
        -not $Handle.IsInvalid) "$Context handle is not live."
    $information = New-Object Stage5FinalAcceptancePathNative+BY_HANDLE_FILE_INFORMATION
    Assert-Stage5Condition ([Stage5FinalAcceptancePathNative]::GetFileInformationByHandle(
            $Handle, [ref]$information)) `
        "$Context handle identity could not be read."
    $length = ([UInt64]$information.FileSizeHigh * [UInt64]4294967296) +
        [UInt64]$information.FileSizeLow
    return [pscustomobject]@{
        attributes = [UInt32]$information.FileAttributes
        volumeSerialNumber = [UInt32]$information.VolumeSerialNumber
        fileIndexHigh = [UInt32]$information.FileIndexHigh
        fileIndexLow = [UInt32]$information.FileIndexLow
        numberOfLinks = [UInt32]$information.NumberOfLinks
        length = [UInt64]$length
        identity = '{0:X8}:{1:X8}{2:X8}' -f
            $information.VolumeSerialNumber, $information.FileIndexHigh,
            $information.FileIndexLow
    }
}

function Open-Stage5FinalAcceptanceAncestorHandles {
    param([string]$Path, [string]$Context)
    Initialize-Stage5FinalAcceptancePathNative
    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $fullPath
    $root = [IO.Path]::GetPathRoot($parent)
    $paths = New-Object 'Collections.Generic.List[string]'
    $paths.Add($root) | Out-Null
    $current = $root
    foreach ($segment in @($parent.Substring($root.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $current = Join-Path $current $segment
        $paths.Add($current) | Out-Null
    }
    $handles = New-Object 'Collections.Generic.List[object]'
    try {
        foreach ($ancestor in $paths) {
            $nativeAncestor = if ($ancestor.StartsWith('\\')) {
                '\\?\UNC\' + $ancestor.Substring(2)
            } else { '\\?\' + $ancestor }
            $handle = [Stage5FinalAcceptancePathNative]::CreateFileW(
                $nativeAncestor, 0, 3, [IntPtr]::Zero, 3, 0x02200000,
                [IntPtr]::Zero)
            Assert-Stage5Condition ($null -ne $handle -and -not $handle.IsInvalid) `
                "$Context ancestor directory could not be held: $ancestor"
            $handles.Add($handle) | Out-Null
            $buffer = New-Object Text.StringBuilder 512
            $length = [Stage5FinalAcceptancePathNative]::GetFinalPathNameByHandleW(
                $handle, $buffer, [UInt32]$buffer.Capacity, 0)
            if ($length -ge $buffer.Capacity) {
                $buffer = New-Object Text.StringBuilder ([int]$length + 1)
                $length = [Stage5FinalAcceptancePathNative]::GetFinalPathNameByHandleW(
                    $handle, $buffer, [UInt32]$buffer.Capacity, 0)
            }
            Assert-Stage5Condition ($length -gt 0 -and $length -lt $buffer.Capacity -and
                $buffer.ToString().StartsWith('\\?\', [StringComparison]::Ordinal)) `
                "$Context ancestor handle path could not be resolved."
            $resolved = [IO.Path]::GetFullPath($buffer.ToString().Substring(4)).TrimEnd('\')
            $expected = [IO.Path]::GetFullPath($ancestor).TrimEnd('\')
            Assert-Stage5Condition ([String]::Equals($resolved, $expected,
                    [StringComparison]::OrdinalIgnoreCase)) `
                "$Context ancestor handle resolves to a different path: $resolved"
            $identity = Get-Stage5FinalAcceptanceHandleIdentity $handle `
                "$Context ancestor '$ancestor'"
            Assert-Stage5Condition (($identity.attributes -band 0x10) -ne 0 -and
                ($identity.attributes -band 0x400) -eq 0) `
                "$Context ancestor handle is not a regular non-reparse directory: $ancestor"
        }
        return ,$handles.ToArray()
    }
    catch {
        foreach ($handle in $handles) { $handle.Dispose() }
        throw
    }
}

function Get-Stage5FinalAcceptanceFileSnapshot {
    param(
        [string]$Path,
        [string]$Context,
        [switch]$HashOnly,
        [ValidateSet('JsonReceipt', 'Replay', 'Trace', 'RawLog', 'RuntimeBinary')]
        [string]$EvidenceKind = 'JsonReceipt'
    )
    $fullPath = [IO.Path]::GetFullPath($Path)
    # Capture and validate one immutable read.  The reader below never opens
    # this path again: it parses the returned bytes.  FileShare.Read prevents
    # replacement/deletion on Windows while the handle is live; the metadata
    # comparison after the copy catches a same-path replacement on hosts where
    # sharing semantics are weaker.  The default digest is independently
    # recomputed from copied bytes; hash-only callers instead digest the same
    # held stream through a fixed buffer, so neither mode trusts metadata.
    Assert-Stage5FinalAcceptanceNoReparsePath (Split-Path -Parent $fullPath) `
        $fullPath $Context
    $before = Get-Item -LiteralPath $fullPath -Force -ErrorAction Stop
    Assert-Stage5Condition (($before -is [IO.FileInfo]) -and
        (($before.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
        -not [string]::IsNullOrWhiteSpace([string]$before.FullName)) `
        "$Context file is missing, not a regular file, or is a reparse point: $Path"
    $stream = New-Object IO.FileStream($fullPath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 65536,
        [IO.FileOptions]::SequentialScan)
    $ancestorHandles = @()
    try {
        Assert-Stage5FinalAcceptanceFileHandlePath $stream $fullPath $Context | Out-Null
        $ancestorHandles = @(Open-Stage5FinalAcceptanceAncestorHandles $fullPath $Context)
        Assert-Stage5FinalAcceptanceNoReparsePath ([IO.Path]::GetPathRoot($fullPath)) `
            $fullPath $Context
        $handleBefore = Get-Stage5FinalAcceptanceHandleIdentity `
            $stream.SafeFileHandle $Context
        Assert-Stage5Condition ($handleBefore.numberOfLinks -eq 1 -and
            ($handleBefore.attributes -band 0x10) -eq 0 -and
            ($handleBefore.attributes -band 0x400) -eq 0) `
            "$Context evidence must be a single-link regular non-reparse file."
        $beforeLength = [Int64]$before.Length
        $maximumBytes = switch ($EvidenceKind) {
            'JsonReceipt' { [Int64](64MB) }
            'Replay' { [Int64](256MB) }
            'Trace' { [Int64](16GB) }
            'RawLog' { [Int64](64MB) }
            'RuntimeBinary' { [Int64](4GB) }
        }
        Assert-Stage5Condition ($beforeLength -ge 0 -and
            [UInt64]$handleBefore.length -eq [UInt64]$beforeLength -and
            $stream.Length -eq $beforeLength) `
            "$Context file changed before its immutable copy began: $Path"
        Assert-Stage5Condition ($beforeLength -le $maximumBytes) `
            "$Context $EvidenceKind file exceeds its $maximumBytes-byte immutable snapshot bound: $Path"
        Assert-Stage5Condition ($EvidenceKind -cne 'Trace' -or $HashOnly) `
            "$Context trace evidence must use bounded hash-only streaming."
        $bytes = $null
        [Int64]$readLength = 0
        if ($HashOnly) {
            # Binary traces can be capped at 16 GiB, so never duplicate their
            # contents in a MemoryStream.  This single held stream is consumed
            # with a fixed 64 KiB buffer and the byte count is bound to the
            # digest before the unchanged-file metadata check below.
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $buffer = New-Object byte[] 65536
                while ($true) {
                    $read = $stream.Read($buffer, 0, $buffer.Length)
                    if ($read -le 0) { break }
                    [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
                    $readLength += [Int64]$read
                }
                [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
                $hash = (($sha.Hash | ForEach-Object {
                        $_.ToString('x2')
                    }) -join '').ToUpperInvariant()
            }
            finally { $sha.Dispose() }
        }
        else {
            $memory = New-Object IO.MemoryStream
            try {
                $stream.CopyTo($memory)
                $bytes = $memory.ToArray()
            }
            finally { $memory.Dispose() }
            $readLength = [Int64]$bytes.LongLength
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = (($sha.ComputeHash($bytes) | ForEach-Object {
                    $_.ToString('x2')
                }) -join '').ToUpperInvariant() }
            finally { $sha.Dispose() }
        }
        Assert-Stage5Condition ($stream.ReadByte() -eq -1) `
            "$Context immutable snapshot did not consume the exact file extent."
        $handleAfter = Get-Stage5FinalAcceptanceHandleIdentity `
            $stream.SafeFileHandle $Context
        Assert-Stage5Condition ($handleAfter.identity -ceq $handleBefore.identity -and
            $handleAfter.numberOfLinks -eq 1 -and
            $handleAfter.length -eq $handleBefore.length -and
            $readLength -eq $beforeLength) `
            "$Context handle identity, link count, or extent changed during its immutable snapshot."
        Assert-Stage5FinalAcceptanceFileHandlePath $stream $fullPath $Context | Out-Null
        $beforeCreation = [DateTime]$before.CreationTimeUtc
        $beforeWrite = [DateTime]$before.LastWriteTimeUtc
        $after = Get-Item -LiteralPath $fullPath -Force -ErrorAction Stop
        Assert-Stage5Condition (($after -is [IO.FileInfo]) -and
            (($after.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
            [Int64]$after.Length -eq $beforeLength -and
            $readLength -eq $beforeLength -and
            [DateTime]$after.CreationTimeUtc -eq $beforeCreation -and
            [DateTime]$after.LastWriteTimeUtc -eq $beforeWrite) `
            "$Context file changed or was replaced while it was snapshotted: $Path"
        $result = [ordered]@{
            path = $fullPath
            bytes = $bytes
            sha256 = $hash
            length = $beforeLength
            creationTimeUtc = $beforeCreation.ToString('o')
            lastWriteTimeUtc = $beforeWrite.ToString('o')
            identity = $handleBefore.identity
            numberOfLinks = $handleBefore.numberOfLinks
        }
        if ($HashOnly) { $result.hashOnly = $true }
        Add-Stage5FinalAcceptanceValidatedClosureEntry $fullPath $hash `
            $beforeLength
        return [pscustomobject]$result
    }
    finally {
        foreach ($handle in $ancestorHandles) { $handle.Dispose() }
        $stream.Dispose()
    }
}

function Write-Stage5FinalAcceptanceFileAtomically {
    param([string]$Path, [byte[]]$Bytes, [string]$Context,
        [ValidateSet('JsonReceipt', 'Replay', 'RawLog')]
        [string]$EvidenceKind = 'JsonReceipt',
        [switch]$ReplaceExisting,
        [scriptblock]$BeforePublishTestHook = $null)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $directory = Split-Path -Parent $fullPath
    $maximumBytes = if ($EvidenceKind -ceq 'Replay') { [Int64](256MB) }
        else { [Int64](64MB) }
    Assert-Stage5Condition ($null -ne $Bytes -and
        [Int64]$Bytes.LongLength -le $maximumBytes) `
        "$Context output exceeds its $maximumBytes-byte $EvidenceKind bound."
    Assert-Stage5Condition (Test-Path -LiteralPath $directory -PathType Container) `
        "$Context output directory does not exist: $directory"
    Assert-Stage5FinalAcceptanceNoReparsePath ([IO.Path]::GetPathRoot($directory)) `
        $directory $Context
    if (-not $ReplaceExisting) {
        Assert-Stage5Condition (-not (Test-Path -LiteralPath $fullPath)) `
            "$Context output already exists: $fullPath"
    }
    $temporaryPath = Join-Path $directory ('.stage5-write-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $nativeHandle = $null
    $stream = $null
    $ancestorHandles = @()
    $heldIdentity = $null
    $expectedSha256 = $null
    try {
        $ancestorHandles = @(Open-Stage5FinalAcceptanceAncestorHandles `
            $fullPath "$Context output")
        Initialize-Stage5FinalAcceptancePathNative
        # GENERIC_READ | GENERIC_WRITE | DELETE, share-delete, CREATE_NEW,
        # FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT. DELETE access
        # permits failure cleanup to target this exact object after a hostile
        # same-directory rename/substitution without trusting either pathname.
        $nativeTemporaryPath = if ($temporaryPath.StartsWith('\\')) {
            '\\?\UNC\' + $temporaryPath.Substring(2)
        } else { '\\?\' + $temporaryPath }
        $nativeHandle = [Stage5FinalAcceptancePathNative]::CreateFileW(
            $nativeTemporaryPath, [UInt32]3221291008, [UInt32]4, [IntPtr]::Zero,
            [UInt32]1, [UInt32]0x00200080, [IntPtr]::Zero)
        Assert-Stage5Condition ($null -ne $nativeHandle -and
            -not $nativeHandle.IsInvalid) `
            "$Context could not create its identity-bound temporary output."
        $stream = [IO.FileStream]::new($nativeHandle, [IO.FileAccess]::ReadWrite,
            65536, $false)
        $nativeHandle = $null
        Assert-Stage5FinalAcceptanceFileHandlePath $stream $temporaryPath `
            "$Context temporary output" | Out-Null
        $heldIdentity = Get-Stage5FinalAcceptanceHandleIdentity $stream.SafeFileHandle `
            "$Context temporary output"
        Assert-Stage5Condition ($heldIdentity.numberOfLinks -eq 1 -and
            $heldIdentity.length -eq 0 -and
            ($heldIdentity.attributes -band 0x400) -eq 0) `
            "$Context temporary output is not a single-link regular file."
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $expectedSha256 = (($sha.ComputeHash($Bytes) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $writtenIdentity = Get-Stage5FinalAcceptanceHandleIdentity `
            $stream.SafeFileHandle "$Context temporary output"
        Assert-Stage5Condition ($writtenIdentity.identity -ceq
                $heldIdentity.identity -and $writtenIdentity.numberOfLinks -eq 1 -and
            $writtenIdentity.length -eq [UInt64]$Bytes.LongLength -and
            $stream.Length -eq $Bytes.LongLength) `
            "$Context temporary output changed identity, link count, or extent while written."
        if ($null -ne $BeforePublishTestHook) {
            & $BeforePublishTestHook $temporaryPath $fullPath
        }
        $nativeFullPath = if ($fullPath.StartsWith('\\')) {
            '\\?\UNC\' + $fullPath.Substring(2)
        } else { '\\?\' + $fullPath }
        # MOVEFILE_WRITE_THROUGH plus optional MOVEFILE_REPLACE_EXISTING gives
        # PowerShell 5.1 the same durable atomic publication semantics without
        # relying on the .NET Core-only File.Move(source, destination, true).
        [UInt32]$moveFlags = 0x00000008
        if ($ReplaceExisting -and (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            $moveFlags = $moveFlags -bor [UInt32]0x00000001
        }
        $moved = [Stage5FinalAcceptancePathNative]::MoveFileExW(
            $nativeTemporaryPath, $nativeFullPath, $moveFlags)
        $moveError = if ($moved) { 0 }
            else { [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
        Assert-Stage5Condition $moved `
            "$Context could not atomically publish its output (Win32 error $moveError)."
        Assert-Stage5FinalAcceptanceFileHandlePath $stream $fullPath `
            "$Context published output" | Out-Null
        $publishedIdentity = Get-Stage5FinalAcceptanceHandleIdentity `
            $stream.SafeFileHandle "$Context published output"
        Assert-Stage5Condition ($publishedIdentity.identity -ceq
                $heldIdentity.identity -and $publishedIdentity.numberOfLinks -eq 1 -and
            $publishedIdentity.length -eq [UInt64]$Bytes.LongLength) `
            "$Context published output does not retain the created file identity and extent."
        $stream.Dispose(); $stream = $null
        Assert-Stage5FinalAcceptanceNoReparsePath $directory $fullPath $Context
        $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $fullPath $Context `
            -EvidenceKind $EvidenceKind
        Assert-Stage5Condition ([string]$snapshot.sha256 -ceq $expectedSha256 -and
            [Int64]$snapshot.length -eq [Int64]$Bytes.LongLength) `
            "$Context final output differs from the durably flushed admitted bytes."
        return $snapshot
    }
    catch {
        if ($null -ne $stream) {
            try {
                $cleanupIdentity = Get-Stage5FinalAcceptanceHandleIdentity `
                    $stream.SafeFileHandle "$Context failed output cleanup"
                if ($null -ne $heldIdentity) {
                    Assert-Stage5Condition ($cleanupIdentity.identity -ceq
                            $heldIdentity.identity -and
                        $cleanupIdentity.numberOfLinks -eq 1 -and
                        ($cleanupIdentity.attributes -band 0x400) -eq 0) `
                        "$Context cleanup observed a changed, linked, or reparsed output."
                }
            }
            catch { }
            # The retained handle, unlike either lexical pathname, cannot be
            # redirected to a substituted object. Always attempt delete-pending
            # on it even when diagnostic identity reinspection itself failed.
            try {
                [void][Stage5FinalAcceptancePathNative]::MarkFileForDeletion(
                    $stream.SafeFileHandle)
            }
            catch { }
            $stream.Dispose(); $stream = $null
        }
        elseif ($null -ne $nativeHandle) {
            try {
                if (-not $nativeHandle.IsInvalid -and -not $nativeHandle.IsClosed) {
                    [void][Stage5FinalAcceptancePathNative]::MarkFileForDeletion(
                        $nativeHandle)
                }
            }
            catch { }
            finally { $nativeHandle.Dispose(); $nativeHandle = $null }
        }
        throw
    }
    finally {
        foreach ($handle in $ancestorHandles) { $handle.Dispose() }
    }
}

function ConvertTo-Stage5FinalAcceptancePsObject {
    param([object]$Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [Collections.IDictionary]) {
        $properties = [ordered]@{}
        foreach ($key in $Value.Keys) {
            $child = $Value[$key]
            if ($child -is [Array]) {
                $items = New-Object object[] $child.Count
                for ($index = 0; $index -lt $child.Count; ++$index) {
                    $items[$index] = ConvertTo-Stage5FinalAcceptancePsObject $child[$index]
                }
                $properties[[string]$key] = $items
            }
            else {
                $properties[[string]$key] = ConvertTo-Stage5FinalAcceptancePsObject $child
            }
        }
        return [pscustomobject]$properties
    }
    if ($Value -is [Array]) {
        $items = New-Object object[] $Value.Count
        for ($index = 0; $index -lt $Value.Count; ++$index) {
            $items[$index] = ConvertTo-Stage5FinalAcceptancePsObject $Value[$index]
        }
        Write-Output -NoEnumerate $items
        return
    }
    return $Value
}

function ConvertFrom-Stage5FinalAcceptanceJsonSnapshot {
    param([object]$Snapshot, [string]$Context, [switch]$AsPsObject)
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'bytes') `
        "$Context does not contain a file snapshot."
    try {
        $document = ConvertFrom-Stage5JsonTextDictionary `
            ([Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes))
        if ($AsPsObject) {
            return ConvertTo-Stage5FinalAcceptancePsObject $document
        }
        return $document
    }
    catch {
        throw "$Context is not valid JSON: $($_.Exception.Message)"
    }
}

function Resolve-Stage5FinalAcceptanceFile {
    param([string]$BaseDirectory, [string]$RelativePath, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) `
        "$Context path is empty."
    Assert-Stage5Condition (-not [IO.Path]::IsPathRooted($RelativePath)) `
        "$Context path must be manifest-relative."
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    $candidate = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    Assert-Stage5FinalAcceptancePathContained $base $candidate $Context
    # Keep the early component check for callers that use Resolve directly, but
    # every reader still repeats it inside the copy-once snapshot immediately
    # before opening.  That second check plus the single immutable snapshot is
    # the race boundary; Resolve never supplies bytes to a reader by itself.
    Assert-Stage5FinalAcceptanceNoReparsePath $base $candidate $Context
    return $candidate
}

function Assert-Stage5FinalAcceptanceSha256 {
    param([string]$Path, [object]$Expected, [string]$Context)
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $Path $Context
    return Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $Expected $Context
}

function Assert-Stage5FinalAcceptanceSnapshotSha256 {
    param([object]$Snapshot, [object]$Expected, [string]$Context)
    Assert-Stage5Condition ($Expected -is [string] -and
        $Expected -match '^[0-9A-Fa-f]{64}$') `
        "$Context SHA-256 must contain exactly 64 hexadecimal characters."
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'sha256' -and
        $Snapshot.PSObject.Properties.Name -contains 'bytes' -and
        $Snapshot.PSObject.Properties.Name -contains 'identity') `
        "$Context does not contain a complete immutable file snapshot."
    $actual = [string]$Snapshot.sha256
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $rehashed = (($sha.ComputeHash([byte[]]$Snapshot.bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
    Assert-Stage5Condition ($actual -match '^[0-9A-Fa-f]{64}$' -and
        $actual.ToUpperInvariant() -ceq $rehashed) `
        "$Context snapshot digest is not bound to its copied bytes."
    Assert-Stage5Condition ($actual -ceq $Expected.ToUpperInvariant()) `
        "$Context SHA-256 mismatch. Expected $Expected, got $actual."
    return $actual
}

function Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 {
    param([object]$Snapshot, [object]$Expected, [Int64]$ExpectedLength,
        [string]$Context)
    Assert-Stage5Condition ($Expected -is [string] -and
        $Expected -match '^[0-9A-Fa-f]{64}$') `
        "$Context SHA-256 must contain exactly 64 hexadecimal characters."
    Assert-Stage5Condition ($ExpectedLength -ge 0) `
        "$Context byte length must be non-negative."
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'sha256' -and
        $Snapshot.PSObject.Properties.Name -contains 'length' -and
        $Snapshot.PSObject.Properties.Name -contains 'identity' -and
        $Snapshot.PSObject.Properties.Name -contains 'hashOnly') `
        "$Context does not contain a complete bounded file snapshot."
    Assert-Stage5Condition ([bool]$Snapshot.hashOnly) `
        "$Context snapshot did not identify hash-only mode."
    if ($Snapshot.PSObject.Properties.Name -contains 'bytes') {
        Assert-Stage5Condition ($null -eq $Snapshot.bytes) `
            "$Context hash-only snapshot retained copied bytes."
    }
    $actual = [string]$Snapshot.sha256
    Assert-Stage5Condition ($actual -match '^[0-9A-Fa-f]{64}$' -and
        $actual.ToUpperInvariant() -ceq $Expected.ToUpperInvariant()) `
        "$Context SHA-256 mismatch. Expected $Expected, got $actual."
    Assert-Stage5Condition ([Int64]$Snapshot.length -eq $ExpectedLength) `
        "$Context byte count mismatch. Expected $ExpectedLength, got $($Snapshot.length)."
    return $actual
}

function Get-Stage5FinalAcceptanceSha256FromBytes {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Assert-Stage5CanonicalUuid {
    param([object]$Value, [string]$Context)
    Assert-Stage5Condition ($Value -is [string] -and
        $Value -cmatch '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
        "$Context must be a canonical UUID."
    return [string]$Value
}

function Assert-Stage5RuntimeClosureBinding {
    param([object]$Value, [object]$Expected, [string]$Context)
    $requiredNames = @('dependencyManifestSha256', 'closureSha256')
    if ($Value -is [Collections.IDictionary]) {
        Assert-Stage5JsonShape $Value $requiredNames "$Context runtime closure"
        $manifestHash = Get-Stage5JsonValue $Value `
            'dependencyManifestSha256' "$Context runtime closure"
        $closureHash = Get-Stage5JsonValue $Value 'closureSha256' `
            "$Context runtime closure"
    }
    else {
        Assert-Stage5Condition ($null -ne $Value) `
            "$Context runtime closure is absent."
        $actualNames = @($Value.PSObject.Properties.Name)
        Assert-Stage5Condition ($actualNames.Count -eq $requiredNames.Count -and
            @($requiredNames | Where-Object {
                $actualNames -cnotcontains $_
            }).Count -eq 0) `
            "$Context runtime closure must contain exactly the two canonical hashes."
        $manifestHash = $Value.dependencyManifestSha256
        $closureHash = $Value.closureSha256
    }
    Assert-Stage5Condition ($manifestHash -is [string] -and
        $manifestHash -cmatch '^[0-9A-Fa-f]{64}$' -and
        $closureHash -is [string] -and $closureHash -cmatch '^[0-9A-Fa-f]{64}$') `
        "$Context runtime closure hashes are not canonical."
    $expectedNames = if ($Expected -is [Collections.IDictionary]) {
        @($Expected.Keys | ForEach-Object { [string]$_ })
    }
    elseif ($null -ne $Expected) { @($Expected.PSObject.Properties.Name) }
    else { @() }
    Assert-Stage5Condition ($null -ne $Expected -and
        @($requiredNames | Where-Object {
            $expectedNames -cnotcontains $_
        }).Count -eq 0) `
        "$Context has no independently verified runtime closure expectation."
    $expectedManifestHash = $Expected.dependencyManifestSha256
    $expectedClosureHash = $Expected.closureSha256
    Assert-Stage5Condition ($expectedManifestHash -is [string] -and
        $expectedClosureHash -is [string] -and
        $expectedManifestHash -cmatch '^[0-9A-Fa-f]{64}$' -and
        $expectedClosureHash -cmatch '^[0-9A-Fa-f]{64}$') `
        "$Context independently verified runtime closure expectation is malformed."
    Assert-Stage5Condition ($manifestHash.ToUpperInvariant() -ceq
        $expectedManifestHash.ToUpperInvariant() -and
        $closureHash.ToUpperInvariant() -ceq $expectedClosureHash.ToUpperInvariant()) `
        "$Context runtime closure is stale or substituted."
    return [pscustomobject]@{
        dependencyManifestSha256 = $manifestHash.ToUpperInvariant()
        closureSha256 = $closureHash.ToUpperInvariant()
    }
}

function Get-Stage5RuntimeClosureBinding {
    param(
        [object]$ArtifactSet,
        [string]$ArtifactDirectory,
        [string]$ExpectedSourceCommit,
        [string]$Context = 'Artifact set runtime closure'
    )
    Assert-Stage5JsonShape $ArtifactSet @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'artifacts', 'runtimeClosure') $Context
    $runtimeClosure = Get-Stage5JsonValue $ArtifactSet 'runtimeClosure' $Context
    Assert-Stage5JsonShape $runtimeClosure @('dependencyManifest', 'closureSha256') `
        "$Context runtime closure"
    $manifestReference = Get-Stage5JsonValue $runtimeClosure `
        'dependencyManifest' "$Context runtime closure"
    Assert-Stage5JsonShape $manifestReference @('path', 'sha256') `
        "$Context dependency manifest reference"
    $manifestRelative = Get-Stage5JsonValue $manifestReference 'path' `
        "$Context dependency manifest reference"
    $manifestExpectedHash = Get-Stage5JsonValue $manifestReference 'sha256' `
        "$Context dependency manifest reference"
    Assert-Stage5Condition ($manifestRelative -is [string] -and
        $manifestExpectedHash -is [string] -and
        $manifestExpectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
        "$Context dependency manifest reference is malformed."
    $manifestPath = Resolve-Stage5FinalAcceptanceFile $ArtifactDirectory `
        $manifestRelative "$Context dependency manifest"
    $manifestSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $manifestPath `
        "$Context dependency manifest"
    $manifestHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $manifestSnapshot `
        $manifestExpectedHash "$Context dependency manifest"
    $manifest = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $manifestSnapshot `
        "$Context dependency manifest"
    Assert-Stage5JsonShape $manifest @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'files') "$Context dependency manifest"
    $manifestSourceCommit = Get-Stage5JsonValue $manifest 'sourceCommit' `
        "$Context dependency manifest"
    $manifestArchitecture = Get-Stage5JsonValue $manifest 'architecture' `
        "$Context dependency manifest"
    Assert-Stage5Condition ((Test-Stage5JsonInteger $manifest.schemaVersion) -and
        $manifest.schemaVersion -eq 1 -and
        $manifestSourceCommit -is [string] -and
        $manifestArchitecture -is [string] -and
        $manifestSourceCommit -ceq $ExpectedSourceCommit -and
        $manifestArchitecture -ceq 'x64') `
        "$Context dependency manifest identity is stale or substituted."
    Assert-Stage5FinalAcceptanceStringSet $manifest.productSet `
        @('Generals', 'ZeroHour') "$Context dependency manifest productSet"
    $dependencyFiles = Get-Stage5JsonValue $manifest 'files' `
        "$Context dependency manifest"
    Assert-Stage5Condition ($dependencyFiles -is [Array] -and
        $dependencyFiles.Count -ge 8) `
        "$Context must contain the complete installed runtime closure, including dependency DLLs and assets."
    $seenPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $seenKindsByTitle = @{}
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $validatedFiles = New-Object 'Collections.Generic.List[object]'
    foreach ($dependency in $dependencyFiles) {
        Assert-Stage5JsonShape $dependency @('title', 'kind', 'path', 'sha256') `
            "$Context dependency manifest file"
        $title = Get-Stage5JsonValue $dependency 'title' "$Context dependency manifest file"
        $kind = Get-Stage5JsonValue $dependency 'kind' "$Context dependency manifest file"
        $relative = Get-Stage5JsonValue $dependency 'path' "$Context dependency manifest file"
        $expectedHash = Get-Stage5JsonValue $dependency 'sha256' "$Context dependency manifest file"
        Assert-Stage5Condition ($title -is [string] -and
            @('Generals', 'ZeroHour') -ccontains $title -and
            $kind -is [string] -and
            @('executable', 'launcher', 'launcher-config', 'dll', 'asset') -ccontains $kind -and
            $relative -is [string] -and $expectedHash -is [string] -and
            $expectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context dependency manifest file is malformed."
        $fullPath = Resolve-Stage5FinalAcceptanceFile $ArtifactDirectory $relative `
            "$Context dependency '$title/$kind'"
        $pathKey = $fullPath.ToLowerInvariant()
        Assert-Stage5Condition $seenPaths.Add($pathKey) `
            "$Context dependency manifest repeats path '$relative'."
        $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $fullPath `
            "$Context dependency '$title/$kind'" -HashOnly `
            -EvidenceKind RuntimeBinary
        $verifiedHash = Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
            $snapshot $expectedHash $snapshot.length `
            "$Context dependency '$title/$kind'"
        $normalizedRelative = ([string]$relative).Replace('\', '/')
        $canonicalLines.Add(('{0}|{1}|{2}|{3}' -f $title, $kind,
            $normalizedRelative, $verifiedHash.ToUpperInvariant())) | Out-Null
        if (-not $seenKindsByTitle.ContainsKey($title)) {
            $seenKindsByTitle[$title] = New-Object 'Collections.Generic.HashSet[string]' `
                ([StringComparer]::Ordinal)
        }
        $seenKindsByTitle[$title].Add($kind) | Out-Null
        $validatedFiles.Add([pscustomobject]@{
            title = [string]$title; kind = [string]$kind; path = [string]$relative
            sha256 = [string]$verifiedHash; fullPath = [string]$fullPath
            snapshot = $snapshot
        }) | Out-Null
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($requiredKind in @('executable', 'launcher', 'launcher-config',
            'dll', 'asset')) {
            Assert-Stage5Condition ($seenKindsByTitle.ContainsKey($title) -and
                $seenKindsByTitle[$title].Contains($requiredKind)) `
                "$Context runtime closure lacks '$requiredKind' for $title."
        }
    }
    $artifactEntries = Get-Stage5JsonValue $ArtifactSet 'artifacts' $Context
    $expectedCoreArtifacts = [ordered]@{
        'generals-executable' = [pscustomobject]@{
            title = 'Generals'; kind = 'executable'; leaf = 'generalsv.exe'
        }
        'generals-launcher' = [pscustomobject]@{
            title = 'Generals'; kind = 'launcher'; leaf = 'launcher.exe'
        }
        'generals-launcher-config' = [pscustomobject]@{
            title = 'Generals'; kind = 'launcher-config'; leaf = 'launcher.lcf'
        }
        'zerohour-executable' = [pscustomobject]@{
            title = 'ZeroHour'; kind = 'executable'; leaf = 'generalszh.exe'
        }
        'zerohour-launcher' = [pscustomobject]@{
            title = 'ZeroHour'; kind = 'launcher'; leaf = 'launcher.exe'
        }
        'zerohour-launcher-config' = [pscustomobject]@{
            title = 'ZeroHour'; kind = 'launcher-config'; leaf = 'launcher.lcf'
        }
    }
    Assert-Stage5Condition ($artifactEntries -is [Array] -and
        $artifactEntries.Count -eq $expectedCoreArtifacts.Count) `
        "$Context must contain exactly the six named core artifacts."
    $seenArtifactRoles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    foreach ($artifact in $artifactEntries) {
        Assert-Stage5JsonShape $artifact @('role', 'path', 'sha256') `
            "$Context artifact entry"
        $artifactRole = Get-Stage5JsonValue $artifact 'role' "$Context artifact entry"
        $artifactPathValue = Get-Stage5JsonValue $artifact 'path' "$Context artifact entry"
        $artifactHash = Get-Stage5JsonValue $artifact 'sha256' "$Context artifact entry"
        Assert-Stage5Condition ($artifactRole -is [string] -and
            $artifactPathValue -is [string] -and
            $artifactHash -is [string]) `
            "$Context artifact entry role, path, and hash must be JSON strings."
        $artifactPath = $artifactPathValue.Replace('\', '/')
        Assert-Stage5Condition ($expectedCoreArtifacts.Keys -ccontains $artifactRole -and
            $seenArtifactRoles.Add($artifactRole)) `
            "$Context artifact role '$artifactRole' is unknown or repeated."
        $expectedArtifact = $expectedCoreArtifacts[$artifactRole]
        Assert-Stage5Condition ([IO.Path]::GetFileName($artifactPath) -ceq
            [string]$expectedArtifact.leaf) `
            "$Context artifact '$artifactRole' has the wrong installed leaf name."
        Assert-Stage5Condition ($artifactHash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context artifact '$artifactRole' has a noncanonical SHA-256."
        $matching = @($validatedFiles | Where-Object {
            $_.path.Replace('\', '/') -ceq $artifactPath -and
            $_.sha256.ToUpperInvariant() -ceq $artifactHash.ToUpperInvariant() -and
            $_.title -ceq [string]$expectedArtifact.title -and
            $_.kind -ceq [string]$expectedArtifact.kind
        })
        Assert-Stage5Condition ($matching.Count -eq 1) `
            "$Context core artifact '$artifactRole' is not included in the hashed runtime closure with its exact title and kind."
        Assert-Stage5Condition ([IO.Path]::GetFullPath([string]$matching[0].fullPath) -ceq
            [IO.Path]::GetFullPath((Join-Path $ArtifactDirectory ([string](Get-Stage5JsonValue `
                $artifact 'path' "$Context artifact entry"))))) `
            "$Context artifact '$artifactRole' resolved outside the hashed runtime closure snapshot."
        Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 $matching[0].snapshot `
            $artifactHash $matching[0].snapshot.length `
            "$Context artifact '$artifactRole' runtime closure snapshot" | Out-Null
    }
    Assert-Stage5Condition ($seenArtifactRoles.Count -eq $expectedCoreArtifacts.Count) `
        "$Context does not bind every required core artifact role."
    $lineArray = $canonicalLines.ToArray()
    [Array]::Sort($lineArray, [StringComparer]::Ordinal)
    $closureText = ($lineArray -join "`n") + "`n"
    $closureHash = Get-Stage5FinalAcceptanceSha256FromBytes `
        ([Text.Encoding]::UTF8.GetBytes($closureText))
    $expectedClosureHash = Get-Stage5JsonValue $runtimeClosure 'closureSha256' `
        "$Context runtime closure"
    Assert-Stage5Condition ($expectedClosureHash -is [string] -and
        $expectedClosureHash -cmatch '^[0-9A-Fa-f]{64}$' -and
        $expectedClosureHash.ToUpperInvariant() -ceq $closureHash) `
        "$Context closureSha256 does not match the independently rehashed dependency closure."
    return [pscustomobject]@{
        dependencyManifestPath = [string]$manifestRelative
        dependencyManifestSha256 = [string]$manifestHash.ToUpperInvariant()
        closureSha256 = [string]$closureHash
        fileCount = [int]$validatedFiles.Count
        files = $validatedFiles.ToArray()
    }
}

function Assert-Stage5FinalAcceptanceBoolean {
    param([object]$Value, [string]$Context)
    Assert-Stage5Condition ($Value -is [bool]) "$Context must be a JSON boolean."
    Assert-Stage5Condition ([bool]$Value) "$Context must be true."
}

function Assert-Stage5FinalAcceptanceStringSet {
    param([object]$Value, [string[]]$Expected, [string]$Context)
    Assert-Stage5Condition ($Value -is [Array]) "$Context must be a JSON array."
    $actual = @($Value | ForEach-Object {
        Assert-Stage5Condition ($_ -is [string]) "$Context entries must be JSON strings."
        [string]$_
    })
    Assert-Stage5Condition ($actual.Count -eq $Expected.Count -and
        @($actual | Sort-Object -CaseSensitive -Unique).Count -eq $actual.Count) `
        "$Context must contain each required value exactly once."
    foreach ($required in $Expected) {
        Assert-Stage5Condition ($actual -ccontains $required) `
            "$Context is missing '$required'."
    }
}

function Get-Stage5FinalAcceptanceReceiptContract {
    param([string]$Role)
    $contracts = @{
        'validation-plan' = [pscustomobject]@{
            producer = 'installed-runtime-validation-plan-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $false
            detailNames = @('gateName', 'validationSet', 'entryCount')
        }
        'validation-results' = [pscustomobject]@{
            producer = 'installed-runtime-validation-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner', 'executable')
            executableProducer = 'game-executable-stage5-performance-report-v5'
            executableProducerVersion = '5'
            hostChildNativeProducers = @('game-executable-stage5-performance-report-v5')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $true
            detailNames = @('resultCount', 'allExecutionsPassed', 'resultsSha256')
        }
        'replay-results' = [pscustomobject]@{
            producer = 'installed-runtime-replay-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner', 'executable')
            executableProducer = 'game-executable-stage5-performance-report-v5'
            executableProducerVersion = '5'
            hostChildNativeProducers = @('game-executable-stage5-performance-report-v5')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $true
            detailNames = @('uniqueReplayCount', 'executionCount',
                'crcTreeSha256', 'allExecutionsPassed')
        }
        'replay-fixture-manifest' = [pscustomobject]@{
            producer = 'reviewed-replay-fixture-manifest-v2'
            producerVersion = '2'
            currentProducer = 'external reviewed fixture authority'
            trustDomain = 'reviewed-fixture'
            allowedTrustDomains = @('reviewed-fixture')
            evidenceKind = 'stage5-reviewed-fixture-receipt'
            requiresChildProvenance = $false
            detailNames = @('fixtureCount', 'stressFixtureCount',
                'fixtureSetSha256')
        }
        'ai-results' = [pscustomobject]@{
            producer = 'installed-runtime-ai-results-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner', 'executable')
            executableProducer = 'game-executable-stage5-performance-report-v5'
            executableProducerVersion = '5'
            hostChildNativeProducers = @('game-executable-stage5-performance-report-v5')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $true
            detailNames = @('scenarioCount', 'distinctSeedCount', 'repeatCount',
                'allGamesCompleted', 'digestTreeSha256')
        }
        'combined-results' = [pscustomobject]@{
            producer = 'installed-runtime-combined-results-v2'
            producerVersion = '2'
            currentProducer = 'New-Stage5CombinedHostRunnerReceipt.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner')
            hostChildNativeProducers = @('game-executable-stage5-performance-report-v5')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $true
            detailNames = @('pipelineMode', 'simulationMode', 'requestedWorkers',
                'workerPolicy', 'projectionSequence', 'projectionSemantics',
                'sourceChildCount', 'bothTitlesPassed', 'sourceCorpora')
        }
        'performance-report' = [pscustomobject]@{
            producer = 'installed-runtime-performance-report-v2'
            producerVersion = '2'
            currentProducer = 'Run-DeterministicSimulationValidation.ps1'
            trustDomain = 'host-runner'
            allowedTrustDomains = @('host-runner', 'executable')
            executableProducer = 'game-executable-stage5-performance-report-v5'
            executableProducerVersion = '5'
            hostChildNativeProducers = @('game-executable-stage5-performance-report-v5')
            evidenceKind = 'stage5-host-runner-receipt'
            requiresChildProvenance = $true
            detailNames = @()
        }
        'premium-review-results' = [pscustomobject]@{
            producer = 'stage5-premium-review-receipt-v2'
            producerVersion = '2'
            currentProducer = 'none'
            trustDomain = 'premium-review'
            allowedTrustDomains = @('premium-review')
            evidenceKind = 'stage5-premium-review-receipt'
            requiresChildProvenance = $false
            detailNames = @('reviewedCommit', 'reviewRounds',
                'independentReviewers', 'openP0', 'openP1', 'openP2')
        }
        'manual-checklist' = [pscustomobject]@{
            producer = 'installed-runtime-manual-acceptance-v2'
            producerVersion = '2'
            currentProducer = 'none'
            trustDomain = 'manual-approval'
            allowedTrustDomains = @('manual-approval')
            evidenceKind = 'stage5-manual-approval-receipt'
            requiresChildProvenance = $false
            detailNames = @('approvalScope', 'candidateHashVerified',
                'bothTitlesTested', 'cleanExitPassed')
        }
    }
    Assert-Stage5Condition ($contracts.ContainsKey($Role)) `
        "No immutable final-acceptance receipt contract exists for attachment role '$Role'."
    return $contracts[$Role]
}

function Read-Stage5FinalAcceptanceProtectedAttestation {
    param(
        [string]$Path, [Collections.IDictionary]$Document, [string]$Kind,
        [string]$Role, [string]$EvidenceTitle, [string]$TrustDomain,
        [string]$ExpectedSourceCommit, [string]$ExpectedArtifactSetSha256
    )
    $context = "Final acceptance '$Kind' attachment '$Role'"
    if ($TrustDomain -in @('premium-review', 'manual-approval')) {
        throw "$context requires a genuinely independent $TrustDomain authority; writable local JSON attestations cannot mint final acceptance."
    }
    $protection = Get-Stage5JsonValue $Document 'protection' $context
    Assert-Stage5JsonShape $protection @('kind', 'path', 'sha256') "$context protected attestation"
    $protectionKind = Get-Stage5JsonValue $protection 'kind' "$context protected attestation"
    $protectionPathValue = Get-Stage5JsonValue $protection 'path' "$context protected attestation"
    $protectionHashValue = Get-Stage5JsonValue $protection 'sha256' "$context protected attestation"
    $expectedProtectionKind = switch ($TrustDomain) {
        'reviewed-fixture' { 'external-reviewed-fixture-attestation' }
        'premium-review' { 'external-premium-review-attestation' }
        default { 'external-user-manual-approval-attestation' }
    }
    Assert-Stage5Condition ($protectionKind -is [string] -and
        $protectionPathValue -is [string] -and
        $protectionHashValue -is [string] -and
        $protectionKind -ceq $expectedProtectionKind) `
        "$context protection kind is not external/protected."
    $protectionPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
        $protectionPathValue `
        "$context protected attestation"
    $protectionSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $protectionPath `
        "$context protected attestation"
    Assert-Stage5FinalAcceptanceSnapshotSha256 $protectionSnapshot `
        $protectionHashValue `
        "$context protected attestation" | Out-Null
    $attestation = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $protectionSnapshot `
        "$context external attestation"
    Assert-Stage5JsonShape $attestation @('schemaVersion', 'evidenceKind',
        'trustDomain', 'role', 'sourceCommit', 'artifactSetSha256',
        'subjectKey', 'authority', 'issuedUtc') "$context external attestation"
    $expectedSubjectKey = '{0}|{1}|{2}|{3}|{4}' -f $ExpectedSourceCommit,
        $ExpectedArtifactSetSha256.ToUpperInvariant(), $Kind, $Role, $EvidenceTitle
    $attestationSchemaVersion = Get-Stage5JsonValue $attestation 'schemaVersion' `
        "$context external attestation"
    $attestationEvidenceKind = Get-Stage5JsonValue $attestation 'evidenceKind' `
        "$context external attestation"
    $attestationTrustDomain = Get-Stage5JsonValue $attestation 'trustDomain' `
        "$context external attestation"
    $attestationRole = Get-Stage5JsonValue $attestation 'role' `
        "$context external attestation"
    $attestationSourceCommit = Get-Stage5JsonValue $attestation 'sourceCommit' `
        "$context external attestation"
    $attestationArtifactSetSha256 = Get-Stage5JsonValue $attestation `
        'artifactSetSha256' "$context external attestation"
    $attestationSubjectKey = Get-Stage5JsonValue $attestation 'subjectKey' `
        "$context external attestation"
    $attestationAuthority = Get-Stage5JsonValue $attestation 'authority' `
        "$context external attestation"
    $attestationIssuedUtc = Get-Stage5JsonValue $attestation 'issuedUtc' `
        "$context external attestation"
    Assert-Stage5Condition ((Test-Stage5JsonInteger $attestationSchemaVersion) -and
        $attestationSchemaVersion -eq 1 -and
        $attestationEvidenceKind -is [string] -and
        $attestationTrustDomain -is [string] -and
        $attestationRole -is [string] -and
        $attestationSourceCommit -is [string] -and
        $attestationArtifactSetSha256 -is [string] -and
        $attestationArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $attestationSubjectKey -is [string] -and
        $attestationAuthority -is [string] -and
        $attestationIssuedUtc -is [string] -and
        $attestationEvidenceKind -ceq 'stage5-external-attestation' -and
        $attestationTrustDomain -ceq $TrustDomain -and
        $attestationRole -ceq $Role -and
        $attestationSourceCommit -ceq $ExpectedSourceCommit -and
        $attestationArtifactSetSha256.ToUpperInvariant() -ceq $ExpectedArtifactSetSha256.ToUpperInvariant() -and
        $attestationSubjectKey -ceq $expectedSubjectKey -and
        -not [string]::IsNullOrWhiteSpace($attestationAuthority)) `
        "$context external attestation is stale, substituted, or not bound to a protected authority."
    $expectedAuthority = switch ($TrustDomain) {
        'reviewed-fixture' { 'fixture-review-authority' }
        'premium-review' { 'premium-review-authority' }
        default { 'user-approval-authority' }
    }
    Assert-Stage5Condition ($attestationAuthority -ceq $expectedAuthority) `
        "$context external attestation authority is not the protected authority for trust domain '$TrustDomain'."
    [DateTimeOffset]$attestationIssued = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($attestationIssuedUtc, [ref]$attestationIssued)) `
        "$context external attestation issuedUtc is not a valid timestamp."
    $receiptProvenance = Get-Stage5JsonValue $Document 'provenance' $context
    $receiptReviewedUtc = Get-Stage5JsonValue $receiptProvenance 'reviewedUtc' "$context external provenance"
    [DateTimeOffset]$receiptReviewed = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ($receiptReviewedUtc -is [string] -and
        [DateTimeOffset]::TryParse($receiptReviewedUtc, [ref]$receiptReviewed) -and
        $attestationIssued -eq $receiptReviewed) `
        "$context external attestation timestamp is stale or does not bind the receipt review time."
    [DateTimeOffset]$receiptRecorded = [DateTimeOffset]::MinValue
    $receiptRecordedUtc = Get-Stage5JsonValue $Document 'recordedUtc' $context
    Assert-Stage5Condition ($receiptRecordedUtc -is [string] -and
        [DateTimeOffset]::TryParse($receiptRecordedUtc, [ref]$receiptRecorded) -and
        $attestationIssued -le $receiptRecorded) `
        "$context external attestation is recorded after the receipt timestamp."
    return $protection
}

function Get-Stage5FinalAcceptanceRelativePath {
    param([string]$BaseDirectory, [string]$Path, [string]$Context)
    $base = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    $candidate = [IO.Path]::GetFullPath($Path)
    Assert-Stage5FinalAcceptancePathContained $base $candidate $Context
    $relative = $candidate.Substring($base.Length).TrimStart([char[]]@(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($relative) -and
        -not [IO.Path]::IsPathRooted($relative)) `
        "$Context did not produce a safe relative path."
    return $relative
}

function Get-Stage5FinalAcceptancePathSegments {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) {
        $full = [IO.Path]::GetFullPath($Path)
        $root = [IO.Path]::GetPathRoot($full)
        return @($full.Substring($root.Length) -split '[\\/]' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    }
    return @($Path -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Get-Stage5FinalAcceptanceNativePathKey {
    param(
        [string]$Path,
        [string]$BaseDirectory,
        [string]$Context
    )
    Assert-Stage5NativeRawPathText $Path $Context
    $candidate = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    }
    else {
        [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetFullPath(
            $BaseDirectory)) $Path))
    }
    return $candidate.ToLowerInvariant()
}

function Add-Stage5FinalAcceptanceGlobalNativePath {
    param(
        [Collections.IDictionary]$Owners,
        [string]$Path,
        [string]$BaseDirectory,
        [ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [string]$Context
    )
    if ($null -eq $Owners) { return }
    $key = Get-Stage5FinalAcceptanceNativePathKey $Path $BaseDirectory $Context
    if ($Owners.ContainsKey($key)) {
        Assert-Stage5Condition ([string]$Owners[$key] -ceq $Title) `
            "$Context aliases a native path already owned by title '$($Owners[$key])'."
    }
    else {
        $Owners[$key] = $Title
    }
}

function Test-Stage5FinalAcceptancePathSuffix {
    param([string[]]$Candidate, [string[]]$Expected, [int]$Minimum = 1)
    if ($Expected.Count -lt $Minimum -or $Candidate.Count -lt $Expected.Count) {
        return $false
    }
    $offset = $Candidate.Count - $Expected.Count
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        if (-not $Candidate[$offset + $index].Equals($Expected[$index],
                [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
    }
    return $true
}

function Get-Stage5FinalAcceptanceBoundedFiles {
    param([string]$BaseDirectory, [string]$Context)
    $base = [IO.Path]::GetFullPath($BaseDirectory)
    $baseItem = Get-Item -LiteralPath $base -Force -ErrorAction Stop
    Assert-Stage5Condition ($baseItem.PSIsContainer -and
        ($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context root must be a regular directory, not a reparse point."
    $directories = New-Object 'Collections.Generic.Queue[string]'
    $directories.Enqueue($base)
    $files = New-Object 'Collections.Generic.List[string]'
    $directoryCount = 0
    while ($directories.Count -gt 0) {
        $directory = $directories.Dequeue()
        ++$directoryCount
        Assert-Stage5Condition ($directoryCount -le 1024) `
            "$Context exceeds the 1024-directory inspection bound."
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)) {
            Assert-Stage5Condition (($item.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -eq 0) `
                "$Context contains a reparse-point alias '$($item.FullName)'."
            if ($item.PSIsContainer) {
                $directories.Enqueue([IO.Path]::GetFullPath($item.FullName))
            }
            else {
                $files.Add([IO.Path]::GetFullPath($item.FullName)) | Out-Null
                Assert-Stage5Condition ($files.Count -le 16384) `
                    "$Context exceeds the 16384-file inspection bound."
            }
        }
    }
    return $files.ToArray()
}

function Get-Stage5FinalAcceptanceNativeRelocationBinding {
    param(
        [string]$Path,
        [string]$EvidenceDirectory,
        [Collections.IDictionary]$GlobalNativePathOwners = $null,
        [string]$GlobalNativeTitle = ''
    )
    $context = "Final acceptance native relocation '$Path'"
    $receiptPath = [IO.Path]::GetFullPath($Path)
    $acceptanceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
    Assert-Stage5FinalAcceptanceNoReparsePath $acceptanceDirectory $receiptPath $context
    if ($null -ne $GlobalNativePathOwners) {
        Assert-Stage5Condition (@('Generals', 'ZeroHour') -ccontains
                $GlobalNativeTitle) `
            "$context global native-path ownership requires one exact title."
    }
    $relocationRoot = Split-Path -Parent $receiptPath
    $receiptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $receiptPath `
        "$context host receipt"
    $receipt = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $receiptSnapshot `
        "$context host receipt"
    Assert-Stage5Condition ($receipt -is [Collections.IDictionary] -and
        (Get-Stage5JsonValue $receipt 'trustDomain' "$context host receipt") -ceq
            'host-runner') `
        "$context requires an immutable host-runner receipt."
    $provenance = Get-Stage5JsonValue $receipt 'provenance' "$context host receipt"
    $children = Get-Stage5JsonValue $provenance 'children' "$context host receipt"
    Assert-Stage5Condition ($children -is [Array] -and $children.Count -gt 0 -and
        $children.Count -le 1024) `
        "$context requires a bounded non-empty set of bound native children."

    $files = @(Get-Stage5FinalAcceptanceBoundedFiles $relocationRoot $context)
    $allBoundPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $allNativePaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $childBindings = New-Object 'Collections.Generic.List[object]'
    foreach ($child in $children) {
        $childNonce = [string](Get-Stage5JsonValue $child 'runNonce' `
            "$context host child")
        $sequence = if (@($child.Keys | Where-Object {
                [string]$_ -ceq 'sequence'
            }).Count -eq 1) {
            [int](Get-Stage5JsonValue $child 'sequence' "$context host child")
        }
        else { 0 }
        $childContext = if ($sequence -gt 0) {
            "$context host child $sequence"
        }
        else { "$context host child '$childNonce'" }
        $nativeReference = Get-Stage5JsonValue $child 'nativeReceipt' $childContext
        Assert-Stage5JsonShape $nativeReference @('path', 'sha256', 'producer',
            'runNonce', 'cohortNonce') "$childContext native receipt reference"
        $nativeReferencePath = [string](Get-Stage5JsonValue $nativeReference 'path' `
            "$childContext native receipt reference")
        $nativePath = Resolve-Stage5FinalAcceptanceFile $relocationRoot `
            $nativeReferencePath "$childContext native receipt"
        Assert-Stage5Condition ($allNativePaths.Add($nativePath)) `
            "$childContext aliases another child's staged native receipt."
        if ($null -ne $GlobalNativePathOwners) {
            [void](Add-Stage5FinalAcceptanceGlobalNativePath `
                -Owners $GlobalNativePathOwners -Path $nativePath `
                -BaseDirectory $relocationRoot -Title $GlobalNativeTitle `
                -Context "$childContext staged native receipt")
        }
        $nativeSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $nativePath `
            "$childContext native receipt"
        Assert-Stage5FinalAcceptanceSnapshotSha256 $nativeSnapshot `
            ([string](Get-Stage5JsonValue $nativeReference 'sha256' `
                "$childContext native receipt reference")) `
            "$childContext native receipt" | Out-Null
        $native = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $nativeSnapshot `
            "$childContext native receipt"
        $rawLogs = Get-Stage5JsonValue $native 'rawLogs' "$childContext native receipt"
        Assert-Stage5Condition ($rawLogs -is [Array] -and $rawLogs.Count -eq 2) `
            "$childContext native receipt must bind exactly raw-log and timing files."
        $nativeProvenance = Get-Stage5JsonValue $native 'provenance' `
            "$childContext native receipt"
        $nativeReceiptSourcePath = [string](Get-Stage5JsonValue $nativeProvenance `
            'receiptPath' "$childContext native provenance")
        Assert-Stage5NativeRawPathText $nativeReceiptSourcePath `
            "$childContext native provenance source receiptPath"
        if ($null -ne $GlobalNativePathOwners) {
            [void](Add-Stage5FinalAcceptanceGlobalNativePath `
                -Owners $GlobalNativePathOwners -Path $nativeReceiptSourcePath `
                -BaseDirectory (Split-Path -Parent $nativePath) `
                -Title $GlobalNativeTitle `
                -Context "$childContext native provenance source receiptPath")
        }
        $nativeReferenceSegments = @(Get-Stage5FinalAcceptancePathSegments `
            $nativeReferencePath)
        $nativeSourceSegments = @(Get-Stage5FinalAcceptancePathSegments `
            $nativeReceiptSourcePath)
        Assert-Stage5Condition ($nativeReferenceSegments.Count -gt 0 -and
            (Test-Stage5FinalAcceptancePathSuffix $nativeSourceSegments `
                $nativeReferenceSegments)) `
            "$childContext native provenance source receiptPath does not end with the staged native receipt path."

        $bindings = New-Object 'Collections.Generic.List[object]'
        $boundNames = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::Ordinal)
        foreach ($raw in $rawLogs) {
            Assert-Stage5JsonShape $raw @('name', 'path', 'sha256') `
                "$childContext native raw log"
            $name = [string](Get-Stage5JsonValue $raw 'name' `
                "$childContext native raw log")
            $sourcePath = [string](Get-Stage5JsonValue $raw 'path' `
                "$childContext native raw log")
            $expectedHash = [string](Get-Stage5JsonValue $raw 'sha256' `
                "$childContext native raw log")
            Assert-Stage5Condition (($name -ceq 'raw-log' -or $name -ceq 'timing') -and
                $boundNames.Add($name) -and $expectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
                "$childContext native raw log identity is invalid or duplicated."
            Assert-Stage5NativeRawPathText $sourcePath `
                "$childContext native raw log '$name' source path"
            $candidatePath = $null
            if (-not [IO.Path]::IsPathRooted($sourcePath)) {
                $candidatePath = Resolve-Stage5FinalAcceptanceFile `
                    (Split-Path -Parent $nativePath) $sourcePath `
                    "$childContext native raw log '$name'"
                Assert-Stage5FinalAcceptancePathContained $relocationRoot $candidatePath `
                    "$childContext native raw log '$name'"
            }
            else {
                $sourceSegments = @(Get-Stage5FinalAcceptancePathSegments $sourcePath)
                $matches = New-Object 'Collections.Generic.List[object]'
                foreach ($file in $files) {
                    $relative = Get-Stage5FinalAcceptanceRelativePath $relocationRoot `
                        $file "$childContext candidate"
                    $candidateSegments = @(Get-Stage5FinalAcceptancePathSegments $relative)
                    $maximum = [Math]::Min($sourceSegments.Count, $candidateSegments.Count)
                    $suffixLength = 0
                    for ($count = 1; $count -le $maximum; ++$count) {
                        if ($sourceSegments[$sourceSegments.Count - $count].Equals(
                                $candidateSegments[$candidateSegments.Count - $count],
                                [StringComparison]::OrdinalIgnoreCase)) {
                            $suffixLength = $count
                        }
                        else { break }
                    }
                    if ($suffixLength -ge 2) {
                        $matches.Add([pscustomobject]@{
                            path = $file; suffixLength = $suffixLength
                        }) | Out-Null
                    }
                }
                Assert-Stage5Condition ($matches.Count -gt 0) `
                    "$childContext native raw log '$name' has no staged candidate matching its source path."
                $maximumSuffix = [int](($matches | Measure-Object -Property suffixLength `
                    -Maximum).Maximum)
                $best = @($matches | Where-Object {
                    [int]$_.suffixLength -eq $maximumSuffix
                })
                Assert-Stage5Condition ($best.Count -eq 1) `
                    "$childContext native raw log '$name' has multiple staged candidates and is ambiguous."
                $candidatePath = [string]$best[0].path
            }
            Assert-Stage5Condition ($allBoundPaths.Add(
                    [IO.Path]::GetFullPath($candidatePath))) `
                "$childContext native raw log '$name' aliases another staged native raw log."
            if ($null -ne $GlobalNativePathOwners) {
                [void](Add-Stage5FinalAcceptanceGlobalNativePath `
                    -Owners $GlobalNativePathOwners -Path $candidatePath `
                    -BaseDirectory $relocationRoot -Title $GlobalNativeTitle `
                    -Context "$childContext native raw log '$name' staged path")
                [void](Add-Stage5FinalAcceptanceGlobalNativePath `
                    -Owners $GlobalNativePathOwners -Path $sourcePath `
                    -BaseDirectory (Split-Path -Parent $nativePath) `
                    -Title $GlobalNativeTitle `
                    -Context "$childContext native raw log '$name' source path")
            }
            $candidateSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $candidatePath `
                "$childContext native raw log '$name'" -EvidenceKind RawLog
            Assert-Stage5FinalAcceptanceSnapshotSha256 $candidateSnapshot $expectedHash `
                "$childContext native raw log '$name'" | Out-Null
            $bindings.Add([ordered]@{
                name = $name
                sourcePath = $sourcePath
                path = Get-Stage5FinalAcceptanceRelativePath $relocationRoot `
                    $candidatePath "$childContext native raw log '$name' staged path"
                sha256 = $expectedHash.ToUpperInvariant()
            }) | Out-Null
        }
        Assert-Stage5Condition ($boundNames.Count -eq 2 -and
            $boundNames.Contains('raw-log') -and $boundNames.Contains('timing')) `
            "$childContext native raw bindings are incomplete."
        # Relocation bindings are consumed by strict JSON-shape readers. Keep
        # each child as an IDictionary (the same representation produced by
        # ConvertFrom-Json -AsHashtable), rather than a PSCustomObject.
        $childBindings.Add([ordered]@{
            sequence = $sequence
            runNonce = $childNonce
            evidenceDirectory = $relocationRoot
            nativeRawBindings = @($bindings.ToArray())
            nativeReceiptSourcePath = $nativeReceiptSourcePath
        }) | Out-Null
    }
    $result = [ordered]@{
        evidenceDirectory = $relocationRoot
        children = @($childBindings.ToArray())
    }
    if ($childBindings.Count -eq 1) {
        $result['nativeRawBindings'] = $childBindings[0].nativeRawBindings
        $result['nativeReceiptSourcePath'] =
            $childBindings[0].nativeReceiptSourcePath
    }
    return [pscustomobject]$result
}

function Assert-Stage5CombinedNativeRawBindings {
    param(
        [object]$NativeDocument,
        [object]$NativeRawBindings,
        [string]$EvidenceDirectory,
        [string]$Title,
        [string]$Context,
        [Collections.Generic.HashSet[string]]$SeenStagedPaths = $null
    )
    Assert-Stage5Condition ($NativeDocument -is [Collections.IDictionary]) `
        "$Context native receipt is not a JSON object."
    $rawLogs = Get-Stage5JsonValue $NativeDocument 'rawLogs' "$Context native receipt"
    Assert-Stage5Condition ($rawLogs -is [Array] -and $rawLogs.Count -eq 2) `
        "$Context native receipt must contain exactly raw-log and timing observations."
    Assert-Stage5Condition ($NativeRawBindings -is [Array] -and
        $NativeRawBindings.Count -eq $rawLogs.Count) `
        "$Context must contain one staged binding for each native raw log."
    $rawByName = @{}
    foreach ($raw in $rawLogs) {
        Assert-Stage5JsonShape $raw @('name', 'path', 'sha256') "$Context native raw log"
        $name = Get-Stage5JsonValue $raw 'name' "$Context native raw log"
        $rawPath = Get-Stage5JsonValue $raw 'path' "$Context native raw log"
        $rawHash = Get-Stage5JsonValue $raw 'sha256' "$Context native raw log"
        Assert-Stage5Condition ($name -is [string] -and
            $rawPath -is [string] -and $rawHash -is [string]) `
            "$Context native raw log name, path, and hash must be JSON strings."
        Assert-Stage5Condition (($name -ceq 'raw-log' -or $name -ceq 'timing') -and
            -not $rawByName.ContainsKey($name)) `
            "$Context native raw log names are invalid or duplicated."
        $rawByName[$name] = $raw
    }
    $boundNames = @{}
    $boundPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($binding in $NativeRawBindings) {
        Assert-Stage5JsonShape $binding @('name', 'sourcePath', 'path', 'sha256') `
            "$Context native raw binding"
        $name = Get-Stage5JsonValue $binding 'name' "$Context native raw binding"
        $sourcePath = Get-Stage5JsonValue $binding 'sourcePath' "$Context native raw binding"
        $stagedPath = Get-Stage5JsonValue $binding 'path' "$Context native raw binding"
        $hash = Get-Stage5JsonValue $binding 'sha256' "$Context native raw binding"
        Assert-Stage5Condition ($name -is [string] -and
            $sourcePath -is [string] -and $stagedPath -is [string] -and
            $hash -is [string]) `
            "$Context native raw binding name and paths/hash must be JSON strings."
        Assert-Stage5Condition ($rawByName.ContainsKey($name) -and
            -not $boundNames.ContainsKey($name)) `
            "$Context native raw binding names are invalid or duplicated."
        Assert-Stage5NativeRawPathText $sourcePath "$Context native raw binding source path"
        Assert-Stage5NativeRawPathText $stagedPath "$Context native raw binding staged path"
        $canonicalTitlePrefix = "sources\$Title\"
        Assert-Stage5Condition ($stagedPath.Replace('/', '\').StartsWith(
                $canonicalTitlePrefix, [StringComparison]::Ordinal)) `
            "$Context native raw binding '$name' is outside its title-qualified source corpus."
        Assert-Stage5Condition ($hash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$Context native raw binding '$name' hash is not canonical."
        $raw = $rawByName[$name]
        Assert-Stage5Condition ($sourcePath -ceq $raw.path -and
            $hash.ToUpperInvariant() -ceq $raw.sha256.ToUpperInvariant()) `
            "$Context native raw binding '$name' is detached from the byte-bound executable receipt."
        $stagedFull = Resolve-Stage5FinalAcceptanceFile $EvidenceDirectory $stagedPath `
            "$Context native raw binding '$name'"
        Assert-Stage5Condition ($boundPaths.Add($stagedFull)) `
            "$Context native raw binding '$name' aliases another staged raw log."
        if ($null -ne $SeenStagedPaths) {
            Assert-Stage5Condition ($SeenStagedPaths.Add($stagedFull)) `
                "$Context native raw binding '$name' aliases another title corpus."
        }
        $stagedSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $stagedFull `
            "$Context native raw binding '$name'" -EvidenceKind RawLog
        Assert-Stage5FinalAcceptanceSnapshotSha256 $stagedSnapshot $hash `
            "$Context native raw binding '$name'" | Out-Null
        $boundNames[$name] = $true
    }
    Assert-Stage5Condition ($boundNames.Count -eq $rawByName.Count -and
        $boundNames.ContainsKey('raw-log') -and $boundNames.ContainsKey('timing')) `
        "$Context native raw bindings are incomplete."
}

function Assert-Stage5SimulationQualificationBindingEqual {
    param(
        [object]$Actual,
        [object]$Expected,
        [string]$Context
    )
    Assert-Stage5JsonShape $Actual @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') $Context
    Assert-Stage5JsonShape $Expected @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') "$Context expected binding"
    $actualPath = Get-Stage5JsonValue $Actual 'path' $Context
    $actualTitle = Get-Stage5JsonValue $Actual 'title' $Context
    $actualManifestSha256 = Get-Stage5JsonValue $Actual 'manifestSha256' $Context
    $actualClosureSha256 = Get-Stage5JsonValue $Actual 'closureSha256' $Context
    $actualFileCount = Get-Stage5JsonValue $Actual 'fileCount' $Context
    $expectedPath = Get-Stage5JsonValue $Expected 'path' "$Context expected binding"
    $expectedTitle = Get-Stage5JsonValue $Expected 'title' "$Context expected binding"
    $expectedManifestSha256 = Get-Stage5JsonValue $Expected 'manifestSha256' `
        "$Context expected binding"
    $expectedClosureSha256 = Get-Stage5JsonValue $Expected 'closureSha256' `
        "$Context expected binding"
    $expectedFileCount = Get-Stage5JsonValue $Expected 'fileCount' `
        "$Context expected binding"
    Assert-Stage5Condition ($actualPath -is [string] -and
        $actualTitle -is [string] -and
        $actualManifestSha256 -is [string] -and
        $actualClosureSha256 -is [string] -and
        $actualFileCount -isnot [Array] -and
        $expectedPath -is [string] -and
        $expectedTitle -is [string] -and
        $expectedManifestSha256 -is [string] -and
        $expectedClosureSha256 -is [string] -and
        $expectedFileCount -isnot [Array] -and
        (Test-Stage5JsonInteger $actualFileCount) -and
        (Test-Stage5JsonInteger $expectedFileCount)) `
        "$Context title/path/hash fields must be strings and fileCount must be a JSON integer."
    foreach ($field in @('path', 'title', 'manifestSha256', 'closureSha256',
            'fileCount')) {
        $actualValue = Get-Stage5JsonValue $Actual $field $Context
        $expectedValue = Get-Stage5JsonValue $Expected $field "$Context expected binding"
        Assert-Stage5Condition (($actualValue -is [string] -and
                $expectedValue -is [string] -and $actualValue -ceq $expectedValue) -or
            ((Test-Stage5JsonInteger $actualValue) -and
                (Test-Stage5JsonInteger $expectedValue) -and
                [int]$actualValue -eq [int]$expectedValue)) `
            "$Context field '$field' differs from the independently validated title data closure."
    }
    return [pscustomobject]@{
        path = $actualPath
        title = $actualTitle
        manifestSha256 = $actualManifestSha256
        closureSha256 = $actualClosureSha256
        fileCount = [int]$actualFileCount
    }
}

function Get-Stage5DevelopmentReadinessArtifactRelocationBinding {
    param(
        [object]$ValidationPlan,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle,
        [string]$ExpectedExecutableSha256,
        [string]$CurrentExecutablePath,
        [switch]$RequireCurrentArtifactRelocation,
        [string]$Context
    )
    Assert-Stage5Condition ($ValidationPlan -is [Collections.IDictionary]) `
        "$Context validation plan is not a JSON object."
    foreach ($name in @('runtimeRoot', 'executable', 'title',
            'executableSha256')) {
        Get-Stage5JsonValue $ValidationPlan $name "$Context validation plan" |
            Out-Null
    }
    $recordedRootText = Get-Stage5JsonValue $ValidationPlan 'runtimeRoot' `
        "$Context validation plan"
    $recordedExecutableText = Get-Stage5JsonValue $ValidationPlan `
        'executable' "$Context validation plan"
    $recordedTitle = Get-Stage5JsonValue $ValidationPlan 'title' `
        "$Context validation plan"
    $recordedHash = Get-Stage5JsonValue $ValidationPlan 'executableSha256' `
        "$Context validation plan"
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($recordedRootText) -and
        -not [string]::IsNullOrWhiteSpace($recordedExecutableText) -and
        $recordedRootText -is [string] -and
        $recordedExecutableText -is [string] -and
        $recordedTitle -is [string] -and
        $recordedHash -is [string] -and
        [IO.Path]::IsPathRooted($recordedRootText) -and
        [IO.Path]::IsPathRooted($recordedExecutableText) -and
        $ExpectedExecutableSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $recordedTitle -ceq $ExpectedTitle -and
        $recordedHash -cmatch '^[0-9A-Fa-f]{64}$' -and
        $recordedHash.ToUpperInvariant() -ceq
            $ExpectedExecutableSha256.ToUpperInvariant()) `
        "$Context recorded artifact identity is malformed, title-swapped, or hash-detached."
    $recordedRoot = [IO.Path]::GetFullPath($recordedRootText).TrimEnd(
        [char[]]@([IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar))
    $recordedExecutable = [IO.Path]::GetFullPath($recordedExecutableText)
    $recordedParent = [IO.Path]::GetFullPath((Split-Path -Parent `
        $recordedExecutable)).TrimEnd([char[]]@(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar))
    $expectedPrefix = if ($ExpectedTitle -ceq 'Generals') {
        'generalsv'
    }
    else { 'generalszh' }
    $recordedLeaf = [IO.Path]::GetFileName($recordedExecutable)
    Assert-Stage5Condition ($recordedRoot.Length -gt
            ([IO.Path]::GetPathRoot($recordedRoot)).Length -and
        $recordedParent -ceq $recordedRoot -and
        $recordedExecutableText -ceq $recordedExecutable -and
        $recordedRootText.TrimEnd([char[]]@('\', '/')) -ceq $recordedRoot -and
        $recordedLeaf -cmatch ('^' + $expectedPrefix +
            '(?:-[A-Za-z0-9._-]+)?\.exe$')) `
        "$Context recorded executable is not the canonical title leaf directly below its authenticated runtimeRoot."

    if ([string]::IsNullOrWhiteSpace($CurrentExecutablePath)) {
        Assert-Stage5Condition (-not $RequireCurrentArtifactRelocation) `
            "$Context requires the independently rehashed current relocated executable."
        return [pscustomobject]@{
            title = $ExpectedTitle
            recordedRuntimeRoot = $recordedRoot
            recordedExecutablePath = $recordedExecutable
            currentExecutablePath = $null
            sha256 = $ExpectedExecutableSha256.ToUpperInvariant()
        }
    }
    Assert-Stage5Condition ([IO.Path]::IsPathRooted($CurrentExecutablePath)) `
        "$Context current relocated executable path is not rooted."
    $currentExecutable = [IO.Path]::GetFullPath($CurrentExecutablePath)
    if ($RequireCurrentArtifactRelocation) {
        Assert-Stage5Condition (-not [String]::Equals($currentExecutable,
                $recordedExecutable, [StringComparison]::OrdinalIgnoreCase)) `
            "$Context current executable was not relocated away from the recorded execution path."
    }
    $currentParent = Split-Path -Parent $currentExecutable
    Assert-Stage5FinalAcceptanceNoReparsePath $currentParent $currentExecutable `
        "$Context current relocated executable"
    Assert-Stage5Condition ((Test-Path -LiteralPath $currentExecutable -PathType Leaf) -and
        [IO.Path]::GetFileName($currentExecutable) -ceq $recordedLeaf) `
        "$Context current relocated executable does not preserve the canonical title leaf."
    $currentHash = Get-Stage5FileSha256 $currentExecutable
    Assert-Stage5Condition ($currentHash -ceq
        $ExpectedExecutableSha256.ToUpperInvariant()) `
        "$Context current relocated executable bytes differ from the independently validated artifact set."
    return [pscustomobject]@{
        title = $ExpectedTitle
        recordedRuntimeRoot = $recordedRoot
        recordedExecutablePath = $recordedExecutable
        currentExecutablePath = $currentExecutable
        sha256 = $currentHash
    }
}

function Assert-Stage5SupportingReceiptChildInExecutionCorpus {
    param(
        [object]$SupportingReceipt,
        [object]$ValidationReceipt,
        [ValidateSet('replay-results', 'ai-results')][string]$Role,
        [string]$Context
    )
    $supportingChildren = @($SupportingReceipt.validatedChildren)
    $validationChildren = @($ValidationReceipt.validatedChildren)
    Assert-Stage5Condition ($supportingChildren.Count -eq 1 -and
        $validationChildren.Count -eq 253) `
        "$Context must bind one real supporting child into the complete 253-child execution corpus."
    $supporting = $supportingChildren[0]
    $runNonce = [string](Get-Stage5JsonValue $supporting 'runNonce' $Context)
    $matches = @($validationChildren | Where-Object {
        [string]$_.runNonce -ceq $runNonce
    })
    Assert-Stage5Condition ($matches.Count -eq 1 -and
        [string](Get-Stage5JsonValue $supporting 'role' $Context) -ceq $Role -and
        [string](Get-Stage5JsonValue $matches[0] 'role' $Context) -ceq
            'validation-results') `
        "$Context child is not a unique member of the complete execution corpus."
    $source = $matches[0]
    foreach ($field in @('title', 'runNonce', 'processId',
            'processCreationUtc', 'executablePath', 'executableSha256',
            'commandLine', 'exitCode', 'stdout', 'stderr', 'nativeReceipt',
            'qualificationData')) {
        Assert-Stage5DevelopmentReadinessSemanticEqual `
            (Get-Stage5JsonValue $supporting $field $Context) `
            (Get-Stage5JsonValue $source $field $Context) `
            "$Context field '$field'"
    }
    return $source
}

function Assert-Stage5CombinedHostSourceBindings {
    param(
        [string]$Path,
        [object]$Details,
        [object[]]$ValidatedRawLogs,
        [object[]]$Children,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$ArtifactPaths,
        [Collections.IDictionary]$SeenRunNonces,
        [string]$CombinedRunNonce,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [object]$ExpectedRuntimeClosure,
        [Collections.IDictionary]$GlobalNativePathOwners = $null
    )
    $context = "Final acceptance combined-results source bindings '$Path'"
    $combinedDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $sourceCorpora = Get-Stage5JsonValue $Details 'sourceCorpora' "$context details"
    Assert-Stage5Condition ($sourceCorpora -is [Array] -and
        $sourceCorpora.Count -eq 2) `
        "$context must contain exactly two complete title source corpora."
    Assert-Stage5Condition ($Children -is [Array] -and $Children.Count -eq 2) `
        "$context must bind exactly two deterministic lineage children."

    $rawByPath = @{}
    foreach ($raw in @($ValidatedRawLogs)) {
        $key = ([string]$raw.path).Replace('/', '\').ToLowerInvariant()
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($key) -and
            -not $rawByPath.ContainsKey($key)) `
            "$context has an empty or duplicate retained raw-log path."
        $rawByPath[$key] = $raw
    }

    $expectedTitles = @('Generals', 'ZeroHour')
    $combinedChildrenByTitle = @{}
    $combinedNativeClosurePaths = New-Object `
        'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    for ($childIndex = 0; $childIndex -lt $Children.Count; ++$childIndex) {
        $child = $Children[$childIndex]
        $title = $expectedTitles[$childIndex]
        $childTitle = Get-Stage5JsonValue $child 'title' `
            "$context combined child"
        $childSourceSequence = Get-Stage5JsonValue $child 'sourceSequence' `
            "$context $title combined child"
        Assert-Stage5Condition ($childTitle -is [string] -and
            $childTitle -ceq $title -and
            (Test-Stage5JsonInteger $childSourceSequence) -and
            [int]$childSourceSequence -eq 1 -and
            -not $combinedChildrenByTitle.ContainsKey($title)) `
            "$context combined children are swapped, duplicated, or not sequence-1 lineage pointers."
        $combinedChildrenByTitle[$title] = $child
    }

    $corpusResults = New-Object 'Collections.Generic.List[object]'
    for ($corpusIndex = 0; $corpusIndex -lt $sourceCorpora.Count; ++$corpusIndex) {
        $corpus = $sourceCorpora[$corpusIndex]
        $title = $expectedTitles[$corpusIndex]
        $corpusContext = "$context $title corpus"
        Assert-Stage5JsonShape $corpus @('title', 'sourceChildCount',
            'qualificationData', 'reviewedFixture', 'receipts') $corpusContext
        $corpusTitle = Get-Stage5JsonValue $corpus 'title' $corpusContext
        $sourceChildCount = Get-Stage5JsonValue $corpus 'sourceChildCount' `
            $corpusContext
        Assert-Stage5Condition ($corpusTitle -is [string] -and
            $corpusTitle -ceq $title -and
            (Test-Stage5JsonInteger $sourceChildCount) -and
            [int]$sourceChildCount -eq 253) `
            "$corpusContext is swapped or does not bind all 253 native executions."

        $qualificationData = Get-Stage5JsonValue $corpus 'qualificationData' `
            $corpusContext
        Assert-Stage5JsonShape $qualificationData @('path', 'title',
            'manifestSha256', 'closureSha256', 'fileCount') `
            "$corpusContext qualificationData"
        $qualificationRelative = "sources\$title\QualificationData.json"
        $qualificationPath = Get-Stage5JsonValue $qualificationData 'path' `
            "$corpusContext qualificationData"
        $qualificationTitle = Get-Stage5JsonValue $qualificationData 'title' `
            "$corpusContext qualificationData"
        $qualificationManifestSha256 = Get-Stage5JsonValue $qualificationData `
            'manifestSha256' "$corpusContext qualificationData"
        $qualificationClosureSha256 = Get-Stage5JsonValue $qualificationData `
            'closureSha256' "$corpusContext qualificationData"
        $qualificationFileCount = Get-Stage5JsonValue $qualificationData `
            'fileCount' "$corpusContext qualificationData"
        Assert-Stage5Condition ($qualificationPath -is [string] -and
            $qualificationTitle -is [string] -and
            $qualificationManifestSha256 -is [string] -and
            $qualificationClosureSha256 -is [string] -and
            $qualificationFileCount -isnot [Array] -and
            $qualificationPath -ceq 'QualificationData.json' -and
            $qualificationTitle -ceq $title -and
            $qualificationManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
            $qualificationClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
            (Test-Stage5JsonInteger $qualificationFileCount) -and
            [int]$qualificationFileCount -eq 6) `
            "$corpusContext qualificationData binding is malformed or title-swapped."
        $qualificationKey = $qualificationRelative.ToLowerInvariant()
        Assert-Stage5Condition ($rawByPath.ContainsKey($qualificationKey) -and
            [string]$rawByPath[$qualificationKey].sha256 -ceq
                [string]$qualificationData.manifestSha256) `
            "$corpusContext retained qualificationData manifest is not a combined raw-log binding."
        $qualificationPath = Resolve-Stage5FinalAcceptanceFile `
            $combinedDirectory $qualificationRelative `
            "$corpusContext qualificationData manifest"
        $qualificationEvidence = Read-Stage5SimulationQualificationDataEvidence `
            -Path $qualificationPath -Binding $qualificationData `
            -ExpectedSourceCommit $ExpectedSourceCommit -ExpectedTitle $title
        Assert-Stage5Condition ($qualificationEvidence.manifestSha256 -is [string] -and
            $qualificationEvidence.closureSha256 -is [string] -and
            $qualificationEvidence.manifestSha256 -ceq
                $qualificationManifestSha256 -and
            $qualificationEvidence.closureSha256 -ceq
                $qualificationClosureSha256 -and
            [int]$qualificationEvidence.fileCount -eq 6) `
            "$corpusContext qualificationData manifest is detached."

        $reviewed = Get-Stage5JsonValue $corpus 'reviewedFixture' `
            $corpusContext
        Assert-Stage5JsonShape $reviewed @('receipt', 'protection', 'manifest',
            'fileCount', 'closureSha256') "$corpusContext reviewedFixture"
        $reviewedFileCount = Get-Stage5JsonValue $reviewed 'fileCount' `
            "$corpusContext reviewedFixture"
        $reviewedClosureSha256 = Get-Stage5JsonValue $reviewed 'closureSha256' `
            "$corpusContext reviewedFixture"
        Assert-Stage5Condition ($reviewedFileCount -isnot [Array] -and
            $reviewedClosureSha256 -is [string] -and
            (Test-Stage5JsonInteger $reviewedFileCount) -and
            [int]$reviewedFileCount -eq 13 -and
            $reviewedClosureSha256 -cmatch '^[0-9A-F]{64}$') `
            "$corpusContext reviewed fixture closure count or hash is invalid."
        $reviewedBindings = @{}
        foreach ($reviewedRole in @('receipt', 'protection', 'manifest')) {
            $binding = Get-Stage5JsonValue $reviewed $reviewedRole `
                "$corpusContext reviewedFixture"
            Assert-Stage5JsonShape $binding @('path', 'sha256') `
                "$corpusContext reviewed fixture $reviewedRole"
            $relative = Get-Stage5JsonValue $binding 'path' `
                "$corpusContext reviewed fixture $reviewedRole"
            $hash = Get-Stage5JsonValue $binding 'sha256' `
                "$corpusContext reviewed fixture $reviewedRole"
            Assert-Stage5Condition ($relative -is [string] -and
                $hash -is [string]) `
                "$corpusContext reviewed fixture $reviewedRole path/hash must be JSON strings."
            $key = $relative.Replace('/', '\').ToLowerInvariant()
            Assert-Stage5Condition (-not [IO.Path]::IsPathRooted($relative) -and
                $relative -notmatch ':' -and
                $relative -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
                $hash -cmatch '^[0-9A-F]{64}$' -and
                $rawByPath.ContainsKey($key) -and
                [string]$rawByPath[$key].sha256 -ceq $hash) `
                "$corpusContext reviewed fixture $reviewedRole is unsafe or not byte-bound as a combined raw log."
            $reviewedBindings[$reviewedRole] = [pscustomobject]@{
                relative = $relative
                hash = $hash
                path = Resolve-Stage5FinalAcceptanceFile $combinedDirectory `
                    $relative "$corpusContext reviewed fixture $reviewedRole"
                snapshot = $rawByPath[$key].snapshot
            }
        }
        $expectedReviewedReceiptRelative =
            "sources\$title\reviewed\receipt.json"
        Assert-Stage5Condition ([string]$reviewedBindings.receipt.relative -ceq
            $expectedReviewedReceiptRelative) `
            "$corpusContext reviewed receipt is outside its canonical title-qualified closure."
        $reviewedArguments = [ordered]@{
            Path = [string]$reviewedBindings.receipt.path
            Kind = 'replay-determinism'
            Role = 'replay-fixture-manifest'
            EvidenceTitle = $title
            ExpectedSourceCommit = $ExpectedSourceCommit
            ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
            ArtifactHashes = $ArtifactHashes
            ExpectedEvidenceSha256 = [string]$reviewedBindings.receipt.hash
            EvidenceSnapshot = $reviewedBindings.receipt.snapshot
            ExpectedRuntimeClosure = $ExpectedRuntimeClosure
        }
        $reviewedRead = Read-Stage5FinalAcceptanceImmutableReceipt `
            @reviewedArguments
        Assert-Stage5Condition ([string]$reviewedRead.trustDomain -ceq
                'reviewed-fixture' -and
            [string]$reviewedRead.producer -ceq
                'reviewed-replay-fixture-manifest-v2' -and
            $null -ne $reviewedRead.reviewedFixtureManifest) `
            "$corpusContext reviewed receipt is not the protected reviewed-fixture authority."

        $reviewedRoot = Split-Path -Parent `
            ([string]$reviewedBindings.receipt.path)
        $internalProtection = Get-Stage5JsonValue $reviewedRead.document `
            'protection' "$corpusContext reviewed receipt"
        $internalManifest = Get-Stage5JsonValue $reviewedRead.provenance `
            'fixtureManifest' "$corpusContext reviewed receipt"
        $internalProtectionPathValue = Get-Stage5JsonValue $internalProtection `
            'path' "$corpusContext protected attestation"
        $internalProtectionHashValue = Get-Stage5JsonValue $internalProtection `
            'sha256' "$corpusContext protected attestation"
        $internalManifestPathValue = Get-Stage5JsonValue $internalManifest `
            'path' "$corpusContext reviewed manifest"
        $internalManifestHashValue = Get-Stage5JsonValue $internalManifest `
            'sha256' "$corpusContext reviewed manifest"
        Assert-Stage5Condition ($internalProtectionPathValue -is [string] -and
            $internalProtectionHashValue -is [string] -and
            $internalManifestPathValue -is [string] -and
            $internalManifestHashValue -is [string]) `
            "$corpusContext reviewed protection/manifest path and hash fields must be JSON strings."
        $internalProtectionPath = Resolve-Stage5FinalAcceptanceFile `
            $reviewedRoot $internalProtectionPathValue `
            "$corpusContext protected attestation"
        $internalManifestPath = Resolve-Stage5FinalAcceptanceFile `
            $reviewedRoot $internalManifestPathValue `
            "$corpusContext reviewed manifest"
        Assert-Stage5Condition ([IO.Path]::GetFullPath($internalProtectionPath) -ceq
                [IO.Path]::GetFullPath($reviewedBindings.protection.path) -and
            $internalProtectionHashValue -ceq
                [string]$reviewedBindings.protection.hash -and
            [IO.Path]::GetFullPath($internalManifestPath) -ceq
                [IO.Path]::GetFullPath($reviewedBindings.manifest.path) -and
            $internalManifestHashValue -ceq
                [string]$reviewedBindings.manifest.hash) `
            "$corpusContext reviewed protection or manifest binding is detached."

        $reviewedRows = New-Object 'Collections.Generic.List[string]'
        $reviewedPaths = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        foreach ($fixed in @(
            [pscustomobject]@{ path = $reviewedBindings.receipt.path
                hash = $reviewedBindings.receipt.hash },
            [pscustomobject]@{ path = $reviewedBindings.protection.path
                hash = $reviewedBindings.protection.hash },
            [pscustomobject]@{ path = $reviewedBindings.manifest.path
                hash = $reviewedBindings.manifest.hash }
        )) {
            $relative = (Get-Stage5FinalAcceptanceRelativePath $reviewedRoot `
                ([string]$fixed.path) "$corpusContext reviewed closure").Replace('\', '/')
            Assert-Stage5Condition ($reviewedPaths.Add(
                    [IO.Path]::GetFullPath([string]$fixed.path))) `
                "$corpusContext reviewed closure aliases a fixed file."
            $reviewedRows.Add("$relative|$([string]$fixed.hash)") | Out-Null
        }
        $fixtureManifestDirectory = Split-Path -Parent $internalManifestPath
        $fixtureEntries = Get-Stage5JsonValue `
            $reviewedRead.reviewedFixtureManifest 'fixtures' `
            "$corpusContext reviewed manifest"
        Assert-Stage5Condition ($fixtureEntries.Count -eq 10) `
            "$corpusContext reviewed manifest must contain exactly ten replays."
        foreach ($fixture in $fixtureEntries) {
            if (@($fixture.Keys | Where-Object {
                    [string]$_ -ceq 'maps'
                }).Count -gt 0) {
                Assert-Stage5Condition (@($fixture.maps).Count -eq 0) `
                    "$corpusContext reviewed fixture may not depend on unstaged map bytes."
            }
            $fixtureSource = Get-Stage5JsonValue $fixture 'source' `
                "$corpusContext reviewed replay"
            $fixtureId = Get-Stage5JsonValue $fixture 'id' `
                "$corpusContext reviewed replay"
            $fixtureExpectedHash = Get-Stage5JsonValue $fixture 'sha256' `
                "$corpusContext reviewed replay"
            Assert-Stage5Condition ($fixtureSource -is [string] -and
                $fixtureId -is [string] -and $fixtureExpectedHash -is [string] -and
                $fixtureExpectedHash -cmatch '^[0-9A-Fa-f]{64}$') `
                "$corpusContext reviewed replay id, source, and SHA-256 must be JSON strings."
            $fixturePath = Resolve-Stage5FinalAcceptanceFile `
                $fixtureManifestDirectory $fixtureSource `
                "$corpusContext reviewed replay '$fixtureId'"
            Assert-Stage5FinalAcceptancePathContained $reviewedRoot $fixturePath `
                "$corpusContext reviewed replay '$fixtureId'"
            $fixtureSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $fixturePath `
                "$corpusContext reviewed replay '$fixtureId'" `
                -HashOnly -EvidenceKind Replay
            $fixtureHash = Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
                $fixtureSnapshot $fixtureExpectedHash `
                $fixtureSnapshot.length `
                "$corpusContext reviewed replay '$fixtureId'"
            Assert-Stage5Condition ($reviewedPaths.Add($fixturePath)) `
                "$corpusContext reviewed replay aliases another closure file."
            $relative = (Get-Stage5FinalAcceptanceRelativePath $reviewedRoot `
                $fixturePath "$corpusContext reviewed replay").Replace('\', '/')
            $reviewedRows.Add("$relative|$fixtureHash") | Out-Null
        }
        Assert-Stage5Condition ($reviewedRows.Count -eq 13 -and
            $reviewedPaths.Count -eq 13) `
            "$corpusContext reviewed closure is not exactly receipt, attestation, manifest, and ten replay bytes."
        [string[]]$sortedReviewedRows = $reviewedRows.ToArray()
        [Array]::Sort($sortedReviewedRows, [StringComparer]::Ordinal)
        $reviewedClosureText = ($sortedReviewedRows -join "`n") + "`n"
        $reviewedClosureHash = Get-Stage5FinalAcceptanceSha256FromBytes `
            ([Text.Encoding]::UTF8.GetBytes($reviewedClosureText))
        Assert-Stage5Condition ($reviewedClosureHash -ceq
            $reviewedClosureSha256) `
            "$corpusContext reviewed-fixture closure hash is detached."

        $receiptBindings = Get-Stage5JsonValue $corpus 'receipts' `
            $corpusContext
        $expectedRoles = @('validation-plan', 'validation-results',
            'replay-results', 'ai-results')
        Assert-Stage5Condition ($receiptBindings -is [Array] -and
            $receiptBindings.Count -eq 4) `
            "$corpusContext must bind exactly the four semantic authority receipts."
        $receiptReads = @{}
        for ($receiptIndex = 0; $receiptIndex -lt 4; ++$receiptIndex) {
            $binding = $receiptBindings[$receiptIndex]
            $role = $expectedRoles[$receiptIndex]
            Assert-Stage5JsonShape $binding @('role', 'path', 'sha256',
                'runNonce', 'cohortNonce') "$corpusContext receipt"
            $bindingRole = Get-Stage5JsonValue $binding 'role' "$corpusContext receipt"
            $relative = Get-Stage5JsonValue $binding 'path' "$corpusContext receipt"
            $expectedRelative = "sources\$title\$role-receipt.json"
            $hash = Get-Stage5JsonValue $binding 'sha256' "$corpusContext receipt"
            $bindingRunNonce = Get-Stage5JsonValue $binding 'runNonce' `
                "$corpusContext receipt"
            $bindingCohortNonce = Get-Stage5JsonValue $binding 'cohortNonce' `
                "$corpusContext receipt"
            Assert-Stage5Condition ($bindingRole -is [string] -and
                $relative -is [string] -and $hash -is [string] -and
                $bindingRunNonce -is [string] -and
                $bindingCohortNonce -is [string]) `
                "$corpusContext receipt role, path, hash, and nonce fields must be JSON strings."
            $rawKey = $relative.Replace('/', '\').ToLowerInvariant()
            Assert-Stage5Condition ($bindingRole -ceq $role -and
                $relative -ceq $expectedRelative -and
                $hash -cmatch '^[0-9A-F]{64}$' -and
                $bindingCohortNonce -ceq $ExpectedCohortNonce -and
                $bindingRunNonce -cmatch
                    '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
                $rawByPath.ContainsKey($rawKey) -and
                [string]$rawByPath[$rawKey].sha256 -ceq $hash) `
                "$corpusContext $role receipt is swapped, unsafe, stale, or not byte-bound."
            $receiptPath = Resolve-Stage5FinalAcceptanceFile `
                $combinedDirectory $relative "$corpusContext $role receipt"
            $receiptSnapshot = $rawByPath[$rawKey].snapshot
            $roleKind = if ($role -ceq 'replay-results') {
                'replay-determinism'
            }
            elseif ($role -ceq 'ai-results') { 'fresh-ai' }
            else { 'deterministic-runtime' }
            $readArguments = [ordered]@{
                Path = $receiptPath
                Kind = $roleKind
                Role = $role
                EvidenceTitle = $title
                ExpectedSourceCommit = $ExpectedSourceCommit
                ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
                ArtifactHashes = $ArtifactHashes
                SeenRunNonces = $SeenRunNonces
                ExpectedEvidenceSha256 = $hash
                EvidenceSnapshot = $receiptSnapshot
                ExpectedCohortNonce = $ExpectedCohortNonce
                ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
                ExpectedRuntimeClosure = $ExpectedRuntimeClosure
                ExpectedQualificationData = $qualificationData
                ExpectedEvidenceDirectory = Split-Path -Parent $receiptPath
                AllowIdenticalNonceReuse = $true
            }
            if ($ArtifactPaths -is [Collections.IDictionary]) {
                $readArguments['ArtifactPaths'] = $ArtifactPaths
            }
            if ($role -in @('validation-results', 'replay-results',
                    'ai-results')) {
                $nativeRelocation = `
                    Get-Stage5FinalAcceptanceNativeRelocationBinding `
                        -Path $receiptPath `
                        -EvidenceDirectory (Split-Path -Parent $receiptPath) `
                        -GlobalNativePathOwners $GlobalNativePathOwners `
                        -GlobalNativeTitle $title
                if ($role -ceq 'validation-results') {
                    $readArguments['NativeRelocationBindings'] =
                        @($nativeRelocation.children)
                }
                else {
                    $readArguments['NativeRawBindings'] =
                        $nativeRelocation.nativeRawBindings
                    $readArguments['NativeReceiptSourcePath'] =
                        $nativeRelocation.nativeReceiptSourcePath
                }
            }
            $read = Read-Stage5FinalAcceptanceImmutableReceipt @readArguments
            Assert-Stage5Condition ($read.runNonce -is [string] -and
                    $read.cohortNonce -is [string] -and
                $read.runNonce -ceq $bindingRunNonce -and
                $read.cohortNonce -ceq $bindingCohortNonce) `
                "$corpusContext $role receipt identity differs from its sourceCorpora binding."
            [void](Assert-Stage5SimulationQualificationBindingEqual `
                $read.qualificationData $qualificationData `
                "$corpusContext $role qualificationData")
            $receiptReads[$role] = $read
        }
        [void](Assert-Stage5SupportingReceiptChildInExecutionCorpus `
            $receiptReads['replay-results'] `
            $receiptReads['validation-results'] 'replay-results' `
            "$corpusContext replay-results lineage")
        [void](Assert-Stage5SupportingReceiptChildInExecutionCorpus `
            $receiptReads['ai-results'] `
            $receiptReads['validation-results'] 'ai-results' `
            "$corpusContext ai-results lineage")

        $planRaw = Get-Stage5DevelopmentReadinessRawLog `
            $receiptReads['validation-plan'].rawLogs 'validation-plan.json' `
            "$corpusContext validation plan"
        $validationRaw = Get-Stage5DevelopmentReadinessRawLog `
            $receiptReads['validation-results'].rawLogs `
            'validation-results.json' "$corpusContext validation results"
        $replayRaw = Get-Stage5DevelopmentReadinessRawLog `
            $receiptReads['replay-results'].rawLogs `
            'validation-results.json' "$corpusContext replay results"
        $aiRaw = Get-Stage5DevelopmentReadinessRawLog `
            $receiptReads['ai-results'].rawLogs `
            'validation-results.json' "$corpusContext AI results"
        $plan = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
            $planRaw.snapshot "$corpusContext validation plan"
        $results = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
            $validationRaw.snapshot "$corpusContext validation results"
        Assert-Stage5Condition ($results -is [Array]) `
            "$corpusContext validation results are not a JSON array."
        $executableRole = if ($title -ceq 'Generals') {
            'generals-executable'
        }
        else { 'zerohour-executable' }
        $currentExecutablePath = ''
        $requireCurrentArtifact = $false
        if ($ArtifactPaths -is [Collections.IDictionary]) {
            Assert-Stage5Condition ($ArtifactPaths.Contains($executableRole)) `
                "$corpusContext current artifact set lacks '$executableRole'."
            $currentExecutablePath = [string]$ArtifactPaths[$executableRole]
            $requireCurrentArtifact = $true
        }
        $semanticArguments = [ordered]@{
            ValidationPlan = $plan
            Results = @($results)
            ReviewedFixtureManifest = $reviewedRead.reviewedFixtureManifest
            PlanDetails = $receiptReads['validation-plan'].details
            ValidationDetails = $receiptReads['validation-results'].details
            ReplayDetails = $receiptReads['replay-results'].details
            AiDetails = $receiptReads['ai-results'].details
            ValidatedRawLogs = $receiptReads['validation-results'].rawLogs
            ValidatedChildren =
                $receiptReads['validation-results'].validatedChildren
            ExpectedPlanSha256 = [string]$planRaw.sha256
            ValidationResultsSha256 = [string]$validationRaw.sha256
            ReplayResultsSha256 = [string]$replayRaw.sha256
            AiResultsSha256 = [string]$aiRaw.sha256
            ExpectedSourceCommit = $ExpectedSourceCommit
            ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
            ExpectedCohortNonce = $ExpectedCohortNonce
            ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
            ExpectedRuntimeClosure = $ExpectedRuntimeClosure
            ExpectedQualificationData = $qualificationData
            ExpectedCurrentExecutablePath = $currentExecutablePath
            RequireCurrentArtifactRelocation = $requireCurrentArtifact
            ExpectedTitle = $title
        }
        $semanticProof = `
            Assert-Stage5DevelopmentReadinessExecutionEvidence `
                @semanticArguments

        $combinedChild = $combinedChildrenByTitle[$title]
        [void](Assert-Stage5SimulationQualificationBindingEqual `
            (Get-Stage5JsonValue $combinedChild 'qualificationData' `
                "$corpusContext combined child") `
            $qualificationData "$corpusContext combined child qualificationData")
        $sourceChildren =
            @($receiptReads['validation-results'].validatedChildren)
        Assert-Stage5Condition ($sourceChildren.Count -eq 253) `
            "$corpusContext validation receipt lost its complete child corpus."
        $sourceChild = $sourceChildren[0]
        foreach ($field in @('runNonce', 'processId', 'processCreationUtc',
                'executablePath', 'executableSha256', 'commandLine',
                'exitCode')) {
            Assert-Stage5Condition ([string]$combinedChild[$field] -ceq
                [string]$sourceChild[$field]) `
                "$corpusContext projection field '$field' differs from source sequence 1."
        }
        foreach ($streamName in @('stdout', 'stderr')) {
            $combinedStream = Get-Stage5JsonValue $combinedChild $streamName `
                "$corpusContext combined child"
            $sourceStream = Get-Stage5JsonValue $sourceChild $streamName `
                "$corpusContext source child"
            $combinedStreamPath = Resolve-Stage5FinalAcceptanceFile `
                $combinedDirectory ([string]$combinedStream.path) `
                "$corpusContext projected $streamName"
            $sourceStreamPath = Resolve-Stage5FinalAcceptanceFile `
                (Split-Path -Parent `
                    ([string]$receiptReads['validation-results'].path)) `
                ([string]$sourceStream.path) `
                "$corpusContext source sequence-1 $streamName"
            Assert-Stage5Condition ([string]$combinedStream.sha256 -ceq
                    [string]$sourceStream.sha256 -and
                [IO.Path]::GetFullPath($combinedStreamPath) -ceq
                    [IO.Path]::GetFullPath($sourceStreamPath) -and
                $combinedNativeClosurePaths.Add($combinedStreamPath)) `
                "$corpusContext projected $streamName path or hash differs from source sequence 1 or aliases another title corpus."
        }
        $combinedNative = Get-Stage5JsonValue $combinedChild 'nativeReceipt' `
            "$corpusContext combined child"
        $sourceNative = Get-Stage5JsonValue $sourceChild 'nativeReceipt' `
            "$corpusContext source child"
        foreach ($field in @('sha256', 'producer', 'runNonce',
                'cohortNonce')) {
            Assert-Stage5Condition ([string]$combinedNative[$field] -ceq
                [string]$sourceNative[$field]) `
                "$corpusContext projected native field '$field' differs from source sequence 1."
        }
        $sourceNativePath = Resolve-Stage5FinalAcceptanceFile `
            (Split-Path -Parent `
                ([string]$receiptReads['validation-results'].path)) `
            ([string]$sourceNative.path) `
            "$corpusContext source sequence-1 native receipt"
        $combinedNativePath = Resolve-Stage5FinalAcceptanceFile `
            $combinedDirectory ([string]$combinedNative.path) `
            "$corpusContext projected native receipt"
        Assert-Stage5Condition ([IO.Path]::GetFullPath($combinedNativePath) -ceq
                [IO.Path]::GetFullPath($sourceNativePath) -and
            $combinedNativeClosurePaths.Add($combinedNativePath)) `
            "$corpusContext projected native receipt path differs from source sequence 1 or aliases another title corpus."
        $sourceNativeSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
            $sourceNativePath "$corpusContext source sequence-1 native receipt"
        $sourceNativeDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
            $sourceNativeSnapshot "$corpusContext source sequence-1 native receipt"
        Assert-Stage5CombinedNativeRawBindings $sourceNativeDocument `
            (Get-Stage5JsonValue $combinedChild 'nativeRawBindings' `
                "$corpusContext combined child") `
            $combinedDirectory $title `
            "$corpusContext projected native raw bindings" `
            -SeenStagedPaths $combinedNativeClosurePaths

        $corpusResults.Add([pscustomobject]@{
            title = $title
            sourceChildCount = 253
            qualificationData = $qualificationData
            qualificationDataEvidence = $qualificationEvidence
            reviewedReceiptSha256 =
                ([string]$reviewedRead.sha256).ToUpperInvariant()
            reviewedManifestSha256 =
                ([string]$reviewedRead.reviewedFixtureManifestSnapshot.sha256).ToUpperInvariant()
            reviewedFixtureClosureSha256 = $reviewedClosureHash
            receipts = $receiptReads
            semanticProof = $semanticProof
        }) | Out-Null
    }
    Assert-Stage5Condition ([string]$CombinedRunNonce -cne
            [string]$corpusResults[0].receipts['validation-results'].runNonce -and
        [string]$CombinedRunNonce -cne
            [string]$corpusResults[1].receipts['validation-results'].runNonce) `
        "$context combined runNonce aliases a source validation receipt."
    return [pscustomobject]@{
        sourceCorpora = @($corpusResults.ToArray())
    }
}
function ConvertTo-Stage5FinalAcceptanceProcessArgumentString {
    param([string[]]$Arguments)
    return (($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' }
        else { $_ }
    }) -join ' ')
}

function Assert-Stage5FinalAcceptanceNativeCommandLine {
    param(
        [string]$NativeCommandLine,
        [string]$ExpectedExecutablePath,
        [string[]]$ExpectedArguments,
        [string]$Context
    )
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($NativeCommandLine) -and
        -not [string]::IsNullOrWhiteSpace($ExpectedExecutablePath) -and
        @($ExpectedArguments).Count -gt 0) `
        "$Context command line or expected plan arguments are empty."
    $argumentString = ConvertTo-Stage5FinalAcceptanceProcessArgumentString `
        $ExpectedArguments
    $suffix = ' ' + $argumentString
    Assert-Stage5Condition ($NativeCommandLine.EndsWith($suffix,
            [StringComparison]::Ordinal)) `
        "$Context native command line arguments differ from the execution plan."
    $executableToken = $NativeCommandLine.Substring(0,
        $NativeCommandLine.Length - $suffix.Length)
    if ($executableToken.Length -ge 2 -and $executableToken[0] -eq '"' -and
        $executableToken[$executableToken.Length - 1] -eq '"') {
        $executableToken = $executableToken.Substring(1,
            $executableToken.Length - 2)
    }
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($executableToken) -and
        [String]::Equals([IO.Path]::GetFullPath($executableToken),
            [IO.Path]::GetFullPath($ExpectedExecutablePath),
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context native command line executable differs from the execution plan."
    return $argumentString
}

function Read-Stage5FinalAcceptanceImmutableReceipt {
    param(
        [string]$Path,
        [string]$Kind,
        [string]$Role,
        [string]$EvidenceTitle,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$SeenRunNonces = $null,
        [string]$ExpectedEvidenceSha256 = $null,
        [object]$EvidenceSnapshot = $null,
        [string]$ExpectedCohortNonce = $null,
        [string]$ExpectedCohortCreatedUtc = $null,
        [object]$ExpectedRuntimeClosure = $null,
        [Collections.IDictionary]$ArtifactPaths = $null,
        [object]$ExpectedQualificationData = $null,
        [string]$ExpectedEvidenceDirectory = '',
        [object]$NativeRawBindings = $null,
        [string]$NativeReceiptSourcePath = '',
        [object]$NativeRelocationBindings = $null,
        [Collections.IDictionary]$GlobalNativePathOwners = $null,
        [switch]$AllowIdenticalNonceReuse
    )
    $context = "Final acceptance '$Kind' attachment '$Role'"
    $receiptDiagnosticStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $writeReceiptDiagnostic = {
        param([string]$Phase, [string]$State, [int]$Index = 0,
            [int]$Total = 0)
        Write-Verbose (('STAGE5_RECEIPT_PHASE context={0} phase={1} state={2} ' +
            'index={3} total={4} elapsedMs={5}') -f $context, $Phase, $State,
            $Index, $Total, $receiptDiagnosticStopwatch.ElapsedMilliseconds)
    }
    & $writeReceiptDiagnostic 'envelope-validation' 'start'
    $contract = Get-Stage5FinalAcceptanceReceiptContract $Role
    $receiptBaseDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $nativeEvidenceDirectory = if ([string]::IsNullOrWhiteSpace($ExpectedEvidenceDirectory)) {
        $receiptBaseDirectory
    }
    else { [IO.Path]::GetFullPath($ExpectedEvidenceDirectory) }
    if ($null -eq $EvidenceSnapshot) {
        Assert-Stage5FinalAcceptanceNoReparsePath $receiptBaseDirectory $Path $context
    }
    $documentSnapshot = if ($null -eq $EvidenceSnapshot) {
        Get-Stage5FinalAcceptanceFileSnapshot $Path $context
    }
    else { $EvidenceSnapshot }
    if ($null -ne $EvidenceSnapshot) {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($ExpectedEvidenceSha256)) `
            "$context caller-supplied snapshot must include its independently expected SHA-256."
    }
    Assert-Stage5Condition ($null -ne $documentSnapshot -and
        $documentSnapshot.PSObject.Properties.Name -contains 'path' -and
        [IO.Path]::GetFullPath([string]$documentSnapshot.path) -ceq
            [IO.Path]::GetFullPath($Path)) `
        "$context receipt snapshot is bound to a different path."
    if ([string]::IsNullOrWhiteSpace($ExpectedEvidenceSha256)) {
        # Internally captured snapshots are already immutable and are checked
        # against their own recomputed digest below.  A caller-provided
        # snapshot never takes this path.
        $ExpectedEvidenceSha256 = [string]$documentSnapshot.sha256
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $documentSnapshot `
        $ExpectedEvidenceSha256 "$context receipt file" | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $documentSnapshot $context
    Assert-Stage5Condition ($document -is [Collections.IDictionary]) `
        "$context is not a JSON receipt object."
    $documentTrustDomain = Get-Stage5JsonValue $document 'trustDomain' $context
    $allowedTrustDomains = @($contract.allowedTrustDomains)
    Assert-Stage5Condition ($documentTrustDomain -is [string] -and
        $allowedTrustDomains -ccontains [string]$documentTrustDomain) `
        "$context trustDomain is substituted or not an allowlisted producer domain: '$documentTrustDomain' for role '$Role'."
    $commonNames = @('schemaVersion', 'evidenceKind', 'status', 'role',
        'trustDomain', 'producer', 'producerVersion', 'sourceCommit', 'title',
        'architecture', 'artifactSetSha256', 'recordedUtc', 'cohortNonce',
        'runtimeClosure', 'details')
    $names = @($commonNames)
    if ($documentTrustDomain -eq 'host-runner' -or
        $documentTrustDomain -eq 'executable') {
        $names += @('runNonce', 'executableSha256', 'rawLogs', 'provenance')
    }
    else {
        $names += @('provenance', 'protection')
    }
    $missing = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $names) {
        if (@($document.Keys | Where-Object { [string]$_ -ceq $name }).Count -eq 0) {
            $missing.Add($name) | Out-Null
        }
    }
    if ($missing.Count -gt 0) {
        throw "$context lacks required '$documentTrustDomain' receipt fields '$($missing -join ', ')'."
    }
    Assert-Stage5JsonShape $document $names $context
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' $context
    $evidenceKind = Get-Stage5JsonValue $document 'evidenceKind' $context
    $status = Get-Stage5JsonValue $document 'status' $context
    $receiptRole = Get-Stage5JsonValue $document 'role' $context
    $trustDomain = Get-Stage5JsonValue $document 'trustDomain' $context
    $producer = Get-Stage5JsonValue $document 'producer' $context
    $producerVersion = Get-Stage5JsonValue $document 'producerVersion' $context
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' $context
    $title = Get-Stage5JsonValue $document 'title' $context
    $architecture = Get-Stage5JsonValue $document 'architecture' $context
    $artifactSetSha256 = Get-Stage5JsonValue $document 'artifactSetSha256' $context
    $recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' $context
    $expectedProducer = if ($documentTrustDomain -ceq 'executable') {
        [string]$contract.executableProducer
    }
    else { [string]$contract.producer }
    $expectedProducerVersion = if ($documentTrustDomain -ceq 'executable') {
        [string]$contract.executableProducerVersion
    } else { [string]$contract.producerVersion }
    $expectedEvidenceKind = if ($documentTrustDomain -ceq 'executable') {
        'stage5-executable-originated-receipt'
    }
    else { [string]$contract.evidenceKind }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and
        $schemaVersion -eq 1 -and
        $evidenceKind -is [string] -and
        $evidenceKind -ceq $expectedEvidenceKind -and
        $status -is [string] -and $status -ceq 'passed') `
        "$context has an invalid receipt identity for trust domain '$documentTrustDomain'."
    Assert-Stage5Condition ($receiptRole -is [string] -and $receiptRole -ceq $Role) `
        "$context role is substituted; receipt role must be '$Role'."
    Assert-Stage5Condition ($trustDomain -is [string] -and
        $trustDomain -ceq [string]$documentTrustDomain) `
        "$context trustDomain is substituted; expected '$documentTrustDomain'."
    Assert-Stage5Condition ($producer -is [string] -and
        $producer -ceq $expectedProducer -and
        $producerVersion -is [string] -and
        $producerVersion -ceq $expectedProducerVersion) `
        "$context has an unregistered producer/version for trust domain '$documentTrustDomain'. Expected '$expectedProducer' version $expectedProducerVersion."
    Assert-Stage5Condition ($sourceCommit -is [string] -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        "$context sourceCommit is stale or does not match the final acceptance commit."
    Assert-Stage5Condition ($title -is [string] -and $title -ceq $EvidenceTitle) `
        "$context title scope is substituted; expected '$EvidenceTitle'."
    Assert-Stage5Condition ($architecture -is [string] -and
        $architecture -ceq 'x64') "$context must identify x64 architecture."
    Assert-Stage5Condition ($artifactSetSha256 -is [string] -and
        $artifactSetSha256 -match '^[0-9A-Fa-f]{64}$' -and
        $artifactSetSha256.ToUpperInvariant() -ceq
            $ExpectedArtifactSetSha256.ToUpperInvariant()) `
        "$context artifactSetSha256 does not bind the independently hashed artifact set."
    Assert-Stage5Condition ($recordedUtc -is [string]) `
        "$context recordedUtc must be a JSON string."
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($recordedUtc, [ref]$recorded)) `
        "$context recordedUtc is not a valid timestamp."
    $cohortNonce = Assert-Stage5CanonicalUuid `
        (Get-Stage5JsonValue $document 'cohortNonce' $context) "$context cohortNonce"
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortNonce)) {
        Assert-Stage5CanonicalUuid $ExpectedCohortNonce "$context expected cohortNonce" | Out-Null
        Assert-Stage5Condition ($cohortNonce -ceq $ExpectedCohortNonce) `
            "$context cohortNonce is stale or detached from the execution cohort."
    }
    $runtimeClosure = Get-Stage5JsonValue $document 'runtimeClosure' $context
    if ($null -ne $ExpectedRuntimeClosure) {
        [void](Assert-Stage5RuntimeClosureBinding $runtimeClosure `
            $ExpectedRuntimeClosure "$context")
    }
    else {
        Assert-Stage5JsonShape $runtimeClosure `
            @('dependencyManifestSha256', 'closureSha256') "$context runtime closure"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortCreatedUtc)) {
        [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
            [ref]$cohortCreated) -and $recorded -ge $cohortCreated) `
            "$context recordedUtc predates the execution cohort."
    }

    $details = Get-Stage5JsonValue $document 'details' $context
    try {
        foreach ($detailName in $contract.detailNames) {
            Get-Stage5JsonValue $details $detailName "$context details" | Out-Null
        }
    }
    catch {
        throw "$context does not contain semantically parsed role-specific details: $($_.Exception.Message)"
    }
    & $writeReceiptDiagnostic 'envelope-validation' 'complete'

    $runNonce = $null
    $validatedRawLogs = New-Object 'Collections.Generic.List[object]'
    $validatedChildren = New-Object 'Collections.Generic.List[object]'
    $qualificationData = $null
    $qualificationDataEvidence = $null
    $combinedSourceBindings = $null
    $provenance = Get-Stage5JsonValue $document 'provenance' $context
    $protection = $null
    $reviewedFixtureManifest = $null
    $reviewedFixtureManifestSnapshot = $null
    $expectedGenerals = [string]$ArtifactHashes['generals-executable']
    $expectedZeroHour = [string]$ArtifactHashes['zerohour-executable']
    if ($documentTrustDomain -eq 'host-runner' -or
        $documentTrustDomain -eq 'executable') {
        $runNonce = Get-Stage5JsonValue $document 'runNonce' $context
        Assert-Stage5Condition ($runNonce -is [string] -and
            $runNonce -match '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$') `
            "$context runNonce is not a canonical UUID nonce."
        $executableSha256 = Get-Stage5JsonValue $document 'executableSha256' $context
        if ($EvidenceTitle -ceq 'Both') {
            Assert-Stage5JsonShape $executableSha256 @('Generals', 'ZeroHour') `
                "$context executableSha256"
            Assert-Stage5Condition (
                (Get-Stage5JsonValue $executableSha256 'Generals' "$context executableSha256") -is [string] -and
                (Get-Stage5JsonValue $executableSha256 'ZeroHour' "$context executableSha256") -is [string] -and
                (Get-Stage5JsonValue $executableSha256 'Generals' "$context executableSha256").ToUpperInvariant() -ceq
                    $expectedGenerals.ToUpperInvariant() -and
                (Get-Stage5JsonValue $executableSha256 'ZeroHour' "$context executableSha256").ToUpperInvariant() -ceq
                    $expectedZeroHour.ToUpperInvariant()) `
                "$context executable SHA-256 binding does not match the artifact set."
        }
        else {
            $expectedExecutable = if ($EvidenceTitle -ceq 'Generals') {
                $expectedGenerals
            }
            else { $expectedZeroHour }
            Assert-Stage5Condition ($executableSha256 -is [string] -and
                $executableSha256 -match '^[0-9A-Fa-f]{64}$' -and
                $executableSha256.ToUpperInvariant() -ceq $expectedExecutable.ToUpperInvariant()) `
                "$context executable SHA-256 binding does not match the artifact set."
        }
        $rawLogs = Get-Stage5JsonValue $document 'rawLogs' $context
        Assert-Stage5Condition ($rawLogs -is [Array] -and $rawLogs.Count -gt 0) `
            "$context has no raw-log hash bindings for trust domain '$documentTrustDomain'."
        $rawNames = New-Object 'Collections.Generic.List[string]'
        $rawPaths = New-Object 'Collections.Generic.List[string]'
        & $writeReceiptDiagnostic 'raw-log-snapshots' 'start' 0 $rawLogs.Count
        $rawLogIndex = 0
        foreach ($rawLog in $rawLogs) {
            ++$rawLogIndex
            if ($rawLogIndex -eq 1 -or ($rawLogIndex % 128) -eq 0 -or
                $rawLogIndex -eq $rawLogs.Count) {
                & $writeReceiptDiagnostic 'raw-log-snapshots' 'progress' `
                    $rawLogIndex $rawLogs.Count
            }
            Assert-Stage5JsonShape $rawLog @('name', 'path', 'sha256') `
                "$context raw log"
            $rawName = Get-Stage5JsonValue $rawLog 'name' "$context raw log"
            $rawRelative = Get-Stage5JsonValue $rawLog 'path' "$context raw log"
            $rawExpectedHash = Get-Stage5JsonValue $rawLog 'sha256' "$context raw log"
            Assert-Stage5Condition ($rawName -is [string] -and
                $rawRelative -is [string] -and
                $rawExpectedHash -is [string]) `
                "$context raw log name, path, and SHA-256 must be strings."
            Assert-Stage5Condition (-not ($rawNames -contains $rawName)) `
                "$context repeats raw log '$rawName'."
            $rawPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
                $rawRelative "$context raw log '$rawName'"
            Assert-Stage5Condition (-not ($rawPaths -contains $rawPath.ToLowerInvariant())) `
                "$context aliases raw log path '$rawRelative'."
            $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
                "$context raw log '$rawName'" -EvidenceKind RawLog
            $rawHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot `
                $rawExpectedHash "$context raw log '$rawName'"
            $rawNames.Add($rawName) | Out-Null
            $rawPaths.Add($rawPath.ToLowerInvariant()) | Out-Null
            $validatedRawLogs.Add([pscustomobject]@{
                name = $rawName; path = $rawRelative; sha256 = $rawHash
                snapshot = $rawSnapshot
            }) | Out-Null
        }
        & $writeReceiptDiagnostic 'raw-log-snapshots' 'complete' `
            $rawLogIndex $rawLogs.Count
        if ($documentTrustDomain -ceq 'host-runner' -and
            $Role -cne 'combined-results') {
            & $writeReceiptDiagnostic 'qualification-data-validation' 'start'
            $qualificationData = Get-Stage5JsonValue $details 'qualificationData' `
                "$context details"
            Assert-Stage5JsonShape $qualificationData @('path', 'title',
                'manifestSha256', 'closureSha256', 'fileCount') `
                "$context qualificationData"
            $qualificationDataPath = Get-Stage5JsonValue $qualificationData 'path' `
                "$context qualificationData"
            $qualificationDataTitle = Get-Stage5JsonValue $qualificationData 'title' `
                "$context qualificationData"
            $qualificationDataManifestSha256 = Get-Stage5JsonValue `
                $qualificationData 'manifestSha256' "$context qualificationData"
            $qualificationDataClosureSha256 = Get-Stage5JsonValue `
                $qualificationData 'closureSha256' "$context qualificationData"
            $qualificationDataFileCount = Get-Stage5JsonValue `
                $qualificationData 'fileCount' "$context qualificationData"
            Assert-Stage5Condition ($qualificationDataPath -is [string] -and
                $qualificationDataTitle -is [string] -and
                $qualificationDataManifestSha256 -is [string] -and
                $qualificationDataClosureSha256 -is [string] -and
                $qualificationDataPath -ceq 'QualificationData.json' -and
                $qualificationDataTitle -ceq $EvidenceTitle -and
                $qualificationDataManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
                $qualificationDataClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
                (Test-Stage5JsonInteger $qualificationDataFileCount) -and
                [int]$qualificationDataFileCount -eq 6) `
                "$context qualificationData is malformed, title-swapped, or incomplete."
            if ($null -ne $ExpectedQualificationData) {
                # Keep the receipt's raw JSON binding for the strict manifest
                # reader.  Assert-Stage5SimulationQualificationBindingEqual
                # returns a typed proof object, which is not a JSON dictionary
                # and must not replace the validated receipt binding.
                [void](Assert-Stage5SimulationQualificationBindingEqual `
                    $qualificationData $ExpectedQualificationData `
                    "$context qualificationData")
            }
            $qualificationPath = Resolve-Stage5FinalAcceptanceFile `
                (Split-Path -Parent $Path) $qualificationDataPath `
                "$context qualificationData manifest"
            $qualificationDataEvidence =
                Read-Stage5SimulationQualificationDataEvidence `
                    -Path $qualificationPath -Binding $qualificationData `
                    -ExpectedSourceCommit $ExpectedSourceCommit `
                    -ExpectedTitle $EvidenceTitle
            # Read-Stage5SimulationQualificationDataEvidence returns a typed
            # PSCustomObject proof, not the raw IDictionary produced by
            # ConvertFrom-Json.  Use its typed fields here; routing this proof
            # back through Get-Stage5JsonValue incorrectly rejects every
            # host-runner validation-results receipt as "not a JSON object".
            $qualificationEvidenceManifestSha256 =
                [string]$qualificationDataEvidence.manifestSha256
            $qualificationEvidenceClosureSha256 =
                [string]$qualificationDataEvidence.closureSha256
            $qualificationEvidenceFileCount = $qualificationDataEvidence.fileCount
            Assert-Stage5Condition ($qualificationEvidenceManifestSha256 -is [string] -and
                    $qualificationEvidenceClosureSha256 -is [string] -and
                    (Test-Stage5JsonInteger $qualificationEvidenceFileCount) -and
                    $qualificationEvidenceManifestSha256 -ceq
                        $qualificationDataManifestSha256 -and
                    $qualificationEvidenceClosureSha256 -ceq
                        $qualificationDataClosureSha256 -and
                    [int]$qualificationEvidenceFileCount -eq 6) `
                "$context retained qualificationData manifest is detached from its receipt."
            & $writeReceiptDiagnostic 'qualification-data-validation' 'complete'
        }
        if ($documentTrustDomain -eq 'host-runner') {
            Assert-Stage5JsonShape $provenance @('kind', 'runner', 'runnerVersion',
                'childProvenance', 'children') "$context host provenance"
            Assert-Stage5Condition ((Get-Stage5JsonValue $provenance 'kind' "$context host provenance") -ceq
                'host-runner-observation' -and
                (Get-Stage5JsonValue $provenance 'runner' "$context host provenance") -ceq
                [string]$contract.currentProducer -and
                (Get-Stage5JsonValue $provenance 'runnerVersion' "$context host provenance") -ceq '1') `
                "$context host provenance is not bound to the registered runner."
            $childProvenance = Get-Stage5JsonValue $provenance 'childProvenance' "$context host provenance"
            $children = Get-Stage5JsonValue $provenance 'children' "$context host provenance"
                Assert-Stage5Condition (($childProvenance -ceq 'bound') -or
                    ($childProvenance -ceq 'not-applicable')) `
                    "$context host provenance has an invalid childProvenance state."
            $isExecutionCorpusReceipt = $Role -ceq 'validation-results'
            if ([bool]$contract.requiresChildProvenance) {
                $expectedChildCount = if ($isExecutionCorpusReceipt) {
                    [int](Get-Stage5JsonValue $details 'resultCount' `
                        "$context details")
                }
                elseif ($Role -ceq 'combined-results') { 2 }
                else { 1 }
                $childCountValid = if ($isExecutionCorpusReceipt) {
                    $children -is [Array] -and $children.Count -eq 253 -and
                        $children.Count -eq $expectedChildCount
                }
                else {
                    $children -is [Array] -and
                        $children.Count -eq $expectedChildCount
                }
                Assert-Stage5Condition ($childProvenance -ceq 'bound' -and
                    $childCountValid) `
                    "$context host receipt lacks required child/native provenance."
            }
            else {
                Assert-Stage5Condition ($childProvenance -ceq 'not-applicable' -and
                    $children -is [Array] -and $children.Count -eq 0) `
                    "$context host receipt claims child provenance where no child was available."
            }
            $seenChildTitles = New-Object 'Collections.Generic.List[string]'
            $seenChildProcesses = New-Object 'Collections.Generic.List[string]'
            $seenChildNonces = New-Object 'Collections.Generic.HashSet[string]' `
                ([StringComparer]::Ordinal)
            $seenChildNativePaths = New-Object 'Collections.Generic.HashSet[string]' `
                ([StringComparer]::OrdinalIgnoreCase)
            & $writeReceiptDiagnostic 'whole-result-validation' 'start' 0 $children.Count
            $childIndex = 0
            foreach ($child in $children) {
                ++$childIndex
                $reportChildProgress = $childIndex -eq 1 -or
                    ($childIndex % 32) -eq 0 -or
                    $childIndex -eq 169 -or $childIndex -eq 170 -or
                    $childIndex -eq $children.Count
                if ($reportChildProgress) {
                    & $writeReceiptDiagnostic 'child-metadata' 'start' `
                        $childIndex $children.Count
                }
                $childNames = @('role', 'title', 'runNonce',
                    'processId', 'processCreationUtc', 'executablePath',
                    'executableSha256', 'commandLine', 'exitCode', 'stdout',
                    'stderr')
                if ($isExecutionCorpusReceipt) {
                    $childNames += @('sequence', 'arguments')
                }
                if ($Role -cne 'combined-results') {
                    $childNames += 'qualificationData'
                }
                if ([bool]$contract.requiresChildProvenance) {
                    $childNames += 'nativeReceipt'
                }
                if ($Role -ceq 'combined-results') {
                    $childNames += @('sourceSequence', 'qualificationData',
                        'nativeRawBindings', 'nativeReceiptSourcePath')
                }
                Assert-Stage5JsonShape $child $childNames "$context host child"
                $childRole = Get-Stage5JsonValue $child 'role' "$context host child"
                $childTitle = Get-Stage5JsonValue $child 'title' "$context host child"
                $childNonce = Get-Stage5JsonValue $child 'runNonce' "$context host child"
                $childPid = Get-Stage5JsonValue $child 'processId' "$context host child"
                $childCreation = Get-Stage5JsonValue $child 'processCreationUtc' "$context host child"
                $childExecutablePath = Get-Stage5JsonValue $child 'executablePath' "$context host child"
                $childExecutableHash = Get-Stage5JsonValue $child 'executableSha256' "$context host child"
                $childCommandLine = Get-Stage5JsonValue $child 'commandLine' "$context host child"
                $childExitCode = Get-Stage5JsonValue $child 'exitCode' "$context host child"
                $childSequence = if ($isExecutionCorpusReceipt) {
                    Get-Stage5JsonValue $child 'sequence' "$context host child"
                }
                else { 0 }
                $childArguments = if ($isExecutionCorpusReceipt) {
                    Get-Stage5JsonValue $child 'arguments' "$context host child"
                }
                else { @() }
                $childNonceMatchesReceipt = $childNonce -is [string] -and
                    $childNonce -match '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$'
                Assert-Stage5Condition ($childRole -is [string] -and $childRole -ceq $Role -and
                    $childTitle -is [string] -and @('Generals', 'ZeroHour') -ccontains $childTitle -and
                    ($isExecutionCorpusReceipt -or -not ($seenChildTitles -contains $childTitle)) -and
                    $childNonceMatchesReceipt -and $seenChildNonces.Add([string]$childNonce) -and
                    (Test-Stage5JsonInteger $childPid) -and [Int64]$childPid -gt 0 -and
                    $childCreation -is [string] -and $childExecutablePath -is [string] -and
                    -not [string]::IsNullOrWhiteSpace($childExecutablePath) -and
                    $childCommandLine -is [string] -and
                    -not [string]::IsNullOrWhiteSpace($childCommandLine) -and
                    (-not $isExecutionCorpusReceipt -or
                        ((Test-Stage5JsonInteger $childSequence) -and
                            [int]$childSequence -eq $childIndex -and
                            $childArguments -is [Array] -and
                            $childArguments.Count -gt 0 -and
                            @($childArguments | Where-Object {
                                $_ -isnot [string]
                            }).Count -eq 0)) -and
                    (Test-Stage5JsonInteger $childExitCode) -and [Int64]$childExitCode -eq 0) `
                    "$context host child process provenance is invalid or stale."
                [DateTimeOffset]$childRecorded = [DateTimeOffset]::MinValue
                [DateTimeOffset]$expectedChildCohort = [DateTimeOffset]::MinValue
                Assert-Stage5Condition ([DateTimeOffset]::TryParse($childCreation,
                        [ref]$childRecorded) -and
                    [DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
                        [ref]$expectedChildCohort) -and
                    $childRecorded -ge $expectedChildCohort -and
                    $childRecorded -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
                    "$context host child processCreationUtc is invalid or outside the current execution cohort."
                $childProcessKey = '{0}|{1}' -f $childPid, $childRecorded.UtcDateTime.Ticks
                Assert-Stage5Condition (-not ($seenChildProcesses -contains $childProcessKey)) `
                    "$context host provenance reuses a child process identity."
                $seenChildProcesses.Add($childProcessKey) | Out-Null
                $childExpectedHash = if ($childTitle -ceq 'Generals') {
                    $expectedGenerals
                }
                else { $expectedZeroHour }
                Assert-Stage5Condition ($childExecutableHash -is [string] -and
                    $childExecutableHash -match '^[0-9A-Fa-f]{64}$' -and
                    $childExecutableHash.ToUpperInvariant() -ceq $childExpectedHash.ToUpperInvariant()) `
                    "$context host child executable SHA-256 binding is stale or substituted."
                if ($Role -cne 'combined-results') {
                    [void](Assert-Stage5SimulationQualificationBindingEqual `
                        (Get-Stage5JsonValue $child 'qualificationData' `
                            "$context host child") $qualificationData `
                        "$context host child qualificationData")
                }
                if ($ArtifactPaths -is [Collections.IDictionary]) {
                    $childRolePath = if ($childTitle -ceq 'Generals') {
                        'generals-executable'
                    } else { 'zerohour-executable' }
                    Assert-Stage5Condition ($ArtifactPaths.Contains($childRolePath)) `
                        "$context has no current relocated executable for '$childRolePath'."
                    $currentArtifactPath = [IO.Path]::GetFullPath(
                        [string]$ArtifactPaths[$childRolePath])
                    $currentArtifactParent = Split-Path -Parent $currentArtifactPath
                    Assert-Stage5FinalAcceptanceNoReparsePath $currentArtifactParent `
                        $currentArtifactPath "$context current relocated executable"
                    Assert-Stage5Condition (
                        (Test-Path -LiteralPath $currentArtifactPath -PathType Leaf) -and
                        [IO.Path]::GetFileName($currentArtifactPath) -ceq
                            [IO.Path]::GetFileName([IO.Path]::GetFullPath(
                                $childExecutablePath)) -and
                        (Get-Stage5FileSha256 $currentArtifactPath) -ceq
                            $childExpectedHash.ToUpperInvariant()) `
                        "$context current relocated executable leaf or bytes differ from the authenticated recorded artifact."
                }
                if ($reportChildProgress) {
                    & $writeReceiptDiagnostic 'child-metadata' 'complete' `
                        $childIndex $children.Count
                    & $writeReceiptDiagnostic 'child-stream-snapshots' 'start' `
                        $childIndex $children.Count
                }
                foreach ($streamName in @('stdout', 'stderr')) {
                    $stream = Get-Stage5JsonValue $child $streamName "$context host child"
                    Assert-Stage5JsonShape $stream @('path', 'sha256') "$context host child $streamName"
                    $streamPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
                        (Get-Stage5JsonValue $stream 'path' "$context host child $streamName") `
                        "$context host child $streamName"
                    $streamSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $streamPath `
                        "$context host child $streamName" -EvidenceKind RawLog
                    Assert-Stage5FinalAcceptanceSnapshotSha256 $streamSnapshot `
                        (Get-Stage5JsonValue $stream 'sha256' "$context host child $streamName") `
                        "$context host child $streamName" | Out-Null
                }
                if ($reportChildProgress) {
                    & $writeReceiptDiagnostic 'child-stream-snapshots' 'complete' `
                        $childIndex $children.Count
                    & $writeReceiptDiagnostic 'native-proof' 'start' `
                        $childIndex $children.Count
                }
                if ([bool]$contract.requiresChildProvenance -or
                    @($child.Keys | Where-Object { [string]$_ -ceq 'nativeReceipt' }).Count -gt 0) {
                    $native = Get-Stage5JsonValue $child 'nativeReceipt' "$context host child"
                    Assert-Stage5JsonShape $native @('path', 'sha256', 'producer',
                        'runNonce', 'cohortNonce') "$context host child nativeReceipt"
                    $nativePathValue = Get-Stage5JsonValue $native 'path' `
                        "$context host child nativeReceipt"
                    $nativeHashValue = Get-Stage5JsonValue $native 'sha256' `
                        "$context host child nativeReceipt"
                    $nativeProducer = Get-Stage5JsonValue $native 'producer' `
                        "$context host child nativeReceipt"
                    $nativeReferenceRunNonce = Get-Stage5JsonValue $native `
                        'runNonce' "$context host child nativeReceipt"
                    $nativeReferenceCohortNonce = Get-Stage5JsonValue $native `
                        'cohortNonce' "$context host child nativeReceipt"
                    Assert-Stage5Condition ($nativePathValue -is [string] -and
                        $nativeHashValue -is [string] -and
                        $nativeProducer -is [string] -and
                        $nativeReferenceRunNonce -is [string] -and
                        $nativeReferenceCohortNonce -is [string]) `
                        "$context host child nativeReceipt identity fields must be JSON strings."
                    $nativeProducerAllowlist = @()
                    if (@($contract.PSObject.Properties.Name) -contains
                        'hostChildNativeProducers') {
                        $nativeProducerAllowlist = @($contract.hostChildNativeProducers)
                    }
                    if ($nativeProducerAllowlist.Count -eq 0) {
                        throw "$context has no installed native producer contract for role '$Role'."
                    }
                    $expectedNativeProducer = ($nativeProducerAllowlist | ForEach-Object {
                        [regex]::Escape([string]$_)
                    }) -join '|'
                    $nativeCohortValid = $nativeReferenceCohortNonce -ceq $cohortNonce
                    $nativeNonceValid = $nativeReferenceRunNonce -ceq $childNonce
                    Assert-Stage5Condition ($nativeProducer -match
                        "^(?:$expectedNativeProducer)$" -and
                        $nativeNonceValid -and $nativeCohortValid) `
                        "$context host child native receipt producer or nonce is invalid."
                    $nativePath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
                        $nativePathValue `
                        "$context host child nativeReceipt"
                    Assert-Stage5Condition ($seenChildNativePaths.Add($nativePath)) `
                        "$context host provenance reuses a child native receipt path."
                    $nativeSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $nativePath `
                        "$context host child nativeReceipt"
                    Assert-Stage5FinalAcceptanceSnapshotSha256 $nativeSnapshot `
                        $nativeHashValue `
                        "$context host child nativeReceipt" | Out-Null
                    $nativeDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $nativeSnapshot `
                        "$context host child nativeReceipt"
                    Assert-Stage5JsonProperties $nativeDocument @('schemaVersion',
                        'evidenceKind', 'status', 'producer', 'producerVersion',
                        'runNonce', 'sourceCommit', 'artifactSetSha256',
                        'executableSha256', 'cohortNonce', 'runtimeClosure',
                        'role', 'title', 'architecture', 'cohortCreatedUtc',
                        'recordedUtc', 'rawLogs', 'provenance') `
                        "$context host child nativeReceipt"
                    $nativeReceiptExecutableHash = Get-Stage5JsonValue $nativeDocument `
                        'executableSha256' "$context host child nativeReceipt"
                    $nativeReceiptSchemaVersion = Get-Stage5JsonValue $nativeDocument `
                        'schemaVersion' "$context host child nativeReceipt"
                    $nativeEvidenceKind = Get-Stage5JsonValue $nativeDocument `
                        'evidenceKind' "$context host child nativeReceipt"
                    $nativeStatus = Get-Stage5JsonValue $nativeDocument `
                        'status' "$context host child nativeReceipt"
                    $nativeDocumentProducer = Get-Stage5JsonValue $nativeDocument `
                        'producer' "$context host child nativeReceipt"
                    $nativeProducerVersion = Get-Stage5JsonValue $nativeDocument `
                        'producerVersion' "$context host child nativeReceipt"
                    $nativeRunNonce = Get-Stage5JsonValue $nativeDocument `
                        'runNonce' "$context host child nativeReceipt"
                    $nativeSourceCommit = Get-Stage5JsonValue $nativeDocument `
                        'sourceCommit' "$context host child nativeReceipt"
                    $nativeArtifactSetSha256 = Get-Stage5JsonValue $nativeDocument `
                        'artifactSetSha256' "$context host child nativeReceipt"
                    $nativeCohortNonce = Get-Stage5JsonValue $nativeDocument `
                        'cohortNonce' "$context host child nativeReceipt"
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $nativeReceiptSchemaVersion) -and
                        $nativeReceiptSchemaVersion -eq 5 -and
                        $nativeEvidenceKind -is [string] -and
                        $nativeStatus -is [string] -and
                        $nativeDocumentProducer -is [string] -and
                        $nativeProducerVersion -is [string] -and
                        $nativeRunNonce -is [string] -and
                        $nativeSourceCommit -is [string] -and
                        $nativeArtifactSetSha256 -is [string] -and
                        $nativeArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
                        $nativeCohortNonce -is [string] -and
                        $nativeEvidenceKind -ceq
                            'stage5-executable-originated-receipt' -and
                        $nativeStatus -ceq 'passed' -and
                        $nativeDocumentProducer -match "^(?:$expectedNativeProducer)$" -and
                        $nativeProducerVersion -ceq '5' -and
                        $nativeRunNonce -ceq $childNonce -and
                        $nativeSourceCommit -ceq $ExpectedSourceCommit -and
                        $nativeArtifactSetSha256.ToUpperInvariant() -ceq
                            $ExpectedArtifactSetSha256.ToUpperInvariant() -and
                        $nativeReceiptExecutableHash -is [string] -and
                        $nativeReceiptExecutableHash.ToUpperInvariant() -ceq
                            $childExpectedHash.ToUpperInvariant()) `
                        "$context host child native receipt is stale, substituted, or from the wrong role."
                    Assert-Stage5Condition ($nativeCohortNonce -ceq $cohortNonce) `
                        "$context host child native receipt cohort is stale or detached."
                    [void](Assert-Stage5RuntimeClosureBinding `
                        (Get-Stage5JsonValue $nativeDocument 'runtimeClosure' `
                            "$context host child nativeReceipt") $runtimeClosure `
                        "$context host child nativeReceipt")
                    $nativeRelocationForChild = $null
                    if ($isExecutionCorpusReceipt) {
                        Assert-Stage5Condition ($NativeRelocationBindings -is [Array] -and
                            $NativeRelocationBindings.Count -eq $children.Count) `
                            "$context requires one native relocation binding per execution child."
                        $matchingRelocations = @($NativeRelocationBindings | Where-Object {
                            [int]$_.sequence -eq [int]$childSequence -and
                            [string]$_.runNonce -ceq [string]$childNonce
                        })
                        Assert-Stage5Condition ($matchingRelocations.Count -eq 1) `
                            "$context host child $childSequence lacks one exact native relocation binding."
                        $nativeRelocationForChild = $matchingRelocations[0]
                    }
                    $nativeRawBindingsForChild = if ($Role -ceq 'combined-results') {
                        Get-Stage5JsonValue $child 'nativeRawBindings' `
                            "$context host child nativeRawBindings"
                    }
                    elseif ($isExecutionCorpusReceipt) {
                        $nativeRelocationForChild.nativeRawBindings
                    }
                    else { $NativeRawBindings }
                    $nativeReceiptSourcePath = if ($Role -ceq 'combined-results') {
                        [string](Get-Stage5JsonValue $child 'nativeReceiptSourcePath' `
                            "$context host child nativeReceiptSourcePath")
                    }
                    elseif ($isExecutionCorpusReceipt) {
                        [string]$nativeRelocationForChild.nativeReceiptSourcePath
                    }
                    else { $NativeReceiptSourcePath }
                    $nativeEvidenceDirectoryForChild = if ($isExecutionCorpusReceipt) {
                        [string]$nativeRelocationForChild.evidenceDirectory
                    }
                    else { $nativeEvidenceDirectory }
                    [void](Assert-Stage5NativePerformanceReceiptProvenance `
                        $nativeDocument "$context host child nativeReceipt" `
                        $childTitle $childExecutablePath $childExpectedHash `
                        ([int]$childPid) $childCreation $ExpectedCohortCreatedUtc `
                        $nativePath $nativeEvidenceDirectoryForChild `
                        $nativeRawBindingsForChild $nativeReceiptSourcePath)
                    if ($isExecutionCorpusReceipt) {
                        $nativeProcess = Get-Stage5JsonValue $nativeDocument `
                            'provenance' "$context host child nativeReceipt"
                        [void](Assert-Stage5FinalAcceptanceNativeCommandLine `
                            ([string](Get-Stage5JsonValue $nativeProcess `
                                'commandLine' "$context host child native provenance")) `
                            $childExecutablePath ([string[]]$childArguments) `
                            "$context host child $childSequence")
                    }
                }
                if ($reportChildProgress) {
                    & $writeReceiptDiagnostic 'native-proof' 'complete' `
                        $childIndex $children.Count
                }
                $seenChildTitles.Add($childTitle) | Out-Null
                $validatedChildren.Add($child) | Out-Null
            }
            & $writeReceiptDiagnostic 'whole-result-validation' 'complete' `
                $childIndex $children.Count
            if ($EvidenceTitle -ceq 'Both' -and [bool]$contract.requiresChildProvenance) {
                Assert-Stage5Condition ($seenChildTitles -contains 'Generals' -and
                    $seenChildTitles -contains 'ZeroHour') `
                    "$context host provenance does not bind both title processes."
            }
            if ($Role -ceq 'combined-results') {
                $combinedSourceBindings = Assert-Stage5CombinedHostSourceBindings `
                    -Path $Path -Details $details `
                    -ValidatedRawLogs $validatedRawLogs.ToArray() `
                    -Children $children -ExpectedSourceCommit $ExpectedSourceCommit `
                    -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
                    -ArtifactHashes $ArtifactHashes -ArtifactPaths $ArtifactPaths `
                    -SeenRunNonces $SeenRunNonces `
                    -CombinedRunNonce $runNonce `
                    -ExpectedCohortNonce $cohortNonce `
                    -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
                    -ExpectedRuntimeClosure $runtimeClosure `
                    -GlobalNativePathOwners $GlobalNativePathOwners
            }
        }
        else {
            Assert-Stage5Condition ($EvidenceTitle -ne 'Both') `
                "$context executable trust-domain receipts must bind one title process."
            Assert-Stage5JsonShape $provenance @('kind', 'receiptPath',
                'receiptSha256', 'processId', 'processCreationUtc',
                'executablePath', 'executableSha256', 'commandLine',
                'exitCode') "$context native provenance"
            $nativeProvenanceKind = Get-Stage5JsonValue $provenance 'kind' `
                "$context native provenance"
            $nativeProvenanceReceiptPath = Get-Stage5JsonValue $provenance `
                'receiptPath' "$context native provenance"
            $nativeProvenanceReceiptSha256 = Get-Stage5JsonValue $provenance `
                'receiptSha256' "$context native provenance"
            $nativeProvenanceExecutablePath = Get-Stage5JsonValue $provenance `
                'executablePath' "$context native provenance"
            $nativeProvenanceProcessCreationUtc = Get-Stage5JsonValue $provenance `
                'processCreationUtc' "$context native provenance"
            Assert-Stage5Condition ($nativeProvenanceKind -is [string] -and
                $nativeProvenanceReceiptPath -is [string] -and
                $nativeProvenanceReceiptSha256 -is [string] -and
                $nativeProvenanceReceiptSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
                $nativeProvenanceExecutablePath -is [string] -and
                $nativeProvenanceProcessCreationUtc -is [string]) `
                "$context native provenance identity fields must be JSON strings."
            Assert-Stage5Condition ($nativeProvenanceKind -ceq
                'native-executable-observation') `
                "$context native provenance kind is invalid."
            $nativeReceiptPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
                $nativeProvenanceReceiptPath `
                "$context native provenance receipt"
            Assert-Stage5Condition ($nativeReceiptPath.ToLowerInvariant() -ne
                ([IO.Path]::GetFullPath($Path)).ToLowerInvariant()) `
                "$context native provenance receipt aliases the wrapper receipt."
            $nativeSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $nativeReceiptPath `
                "$context native provenance receipt"
            Assert-Stage5FinalAcceptanceSnapshotSha256 $nativeSnapshot `
                $nativeProvenanceReceiptSha256 `
                "$context native provenance receipt" | Out-Null
            $nativeDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $nativeSnapshot `
                "$context native receipt"
            Assert-Stage5JsonProperties $nativeDocument @('schemaVersion', 'evidenceKind',
                'status', 'producer', 'producerVersion', 'runNonce',
                'sourceCommit', 'artifactSetSha256', 'executableSha256',
                'cohortNonce', 'runtimeClosure', 'role', 'title', 'architecture',
                'cohortCreatedUtc', 'recordedUtc', 'rawLogs', 'provenance') `
                "$context native receipt"
            $nativeExpectedExecutable = if ($EvidenceTitle -ceq 'Generals') {
                $expectedGenerals
            }
            else { $expectedZeroHour }
            [void](Assert-Stage5NativePerformanceReceiptProvenance `
                $nativeDocument "$context native receipt" $EvidenceTitle `
                $nativeProvenanceExecutablePath `
                $nativeExpectedExecutable `
                ([int](Get-Stage5JsonValue $provenance 'processId' `
                    "$context native provenance")) `
                $nativeProvenanceProcessCreationUtc `
                $ExpectedCohortCreatedUtc $nativeReceiptPath (Split-Path -Parent $Path))
            $nativeDocumentEvidenceKind = Get-Stage5JsonValue $nativeDocument `
                'evidenceKind' "$context native receipt"
            $nativeDocumentStatus = Get-Stage5JsonValue $nativeDocument `
                'status' "$context native receipt"
            $nativeDocumentProducer = Get-Stage5JsonValue $nativeDocument `
                'producer' "$context native receipt"
            $nativeDocumentProducerVersion = Get-Stage5JsonValue $nativeDocument `
                'producerVersion' "$context native receipt"
            $nativeDocumentRunNonce = Get-Stage5JsonValue $nativeDocument `
                'runNonce' "$context native receipt"
            $nativeDocumentSourceCommit = Get-Stage5JsonValue $nativeDocument `
                'sourceCommit' "$context native receipt"
            $nativeDocumentArtifactSetSha256 = Get-Stage5JsonValue $nativeDocument `
                'artifactSetSha256' "$context native receipt"
            $nativeDocumentCohortNonce = Get-Stage5JsonValue $nativeDocument `
                'cohortNonce' "$context native receipt"
            Assert-Stage5Condition ($nativeDocumentEvidenceKind -is [string] -and
                $nativeDocumentStatus -is [string] -and
                $nativeDocumentProducer -is [string] -and
                $nativeDocumentProducerVersion -is [string] -and
                $nativeDocumentRunNonce -is [string] -and
                $nativeDocumentSourceCommit -is [string] -and
                $nativeDocumentArtifactSetSha256 -is [string] -and
                $nativeDocumentArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
                $nativeDocumentCohortNonce -is [string]) `
                "$context native receipt identity fields must be JSON strings."
            Assert-Stage5Condition ($nativeDocumentEvidenceKind -ceq
                'stage5-executable-originated-receipt' -and
                $nativeDocumentStatus -ceq 'passed' -and
                $nativeDocumentProducer -ceq
                    'game-executable-stage5-performance-report-v5' -and
                $nativeDocumentProducerVersion -ceq '5' -and
                $nativeDocumentRunNonce -ceq $runNonce -and
                $nativeDocumentSourceCommit -ceq $ExpectedSourceCommit -and
                $nativeDocumentArtifactSetSha256.ToUpperInvariant() -ceq
                    $ExpectedArtifactSetSha256.ToUpperInvariant() -and
                $nativeDocumentCohortNonce -ceq $cohortNonce) `
                "$context native receipt identity is stale, substituted, or from the wrong role."
            [void](Assert-Stage5RuntimeClosureBinding `
                (Get-Stage5JsonValue $nativeDocument 'runtimeClosure' "$context native receipt") `
                $runtimeClosure "$context native receipt")
            $nativeReceiptExecutableHash = Get-Stage5JsonValue $nativeDocument `
                'executableSha256' "$context native receipt"
            Assert-Stage5Condition ($nativeReceiptExecutableHash -is [string] -and
                $nativeReceiptExecutableHash -match '^[0-9A-Fa-f]{64}$' -and
                $nativeReceiptExecutableHash.ToUpperInvariant() -ceq
                    $nativeExpectedExecutable.ToUpperInvariant()) `
                "$context native receipt executable SHA-256 binding is stale or substituted."
            Assert-Stage5Condition ((Test-Stage5JsonInteger (Get-Stage5JsonValue $provenance 'processId' "$context native provenance")) -and
                [Int64](Get-Stage5JsonValue $provenance 'processId' "$context native provenance") -gt 0 -and
                (Get-Stage5JsonValue $provenance 'executablePath' "$context native provenance") -is [string] -and
                -not [string]::IsNullOrWhiteSpace((Get-Stage5JsonValue $provenance 'executablePath' "$context native provenance")) -and
                (Get-Stage5JsonValue $provenance 'commandLine' "$context native provenance") -is [string] -and
                (Test-Stage5JsonInteger (Get-Stage5JsonValue $provenance 'exitCode' "$context native provenance")) -and
                [Int64](Get-Stage5JsonValue $provenance 'exitCode' "$context native provenance") -eq 0) `
                "$context native process observation is invalid."
            [DateTimeOffset]$nativeRecorded = [DateTimeOffset]::MinValue
            Assert-Stage5Condition ([DateTimeOffset]::TryParse((Get-Stage5JsonValue $provenance 'processCreationUtc' "$context native provenance"), [ref]$nativeRecorded)) `
                "$context native processCreationUtc is not a valid timestamp."
            $nativeExecutableHash = Get-Stage5JsonValue $provenance 'executableSha256' "$context native provenance"
            Assert-Stage5Condition ($nativeExecutableHash -is [string] -and
                $nativeExecutableHash -match '^[0-9A-Fa-f]{64}$' -and
                $nativeExecutableHash.ToUpperInvariant() -ceq
                    $nativeExpectedExecutable.ToUpperInvariant()) `
                "$context native process executable hash is substituted."
        }
    }
    elseif ($documentTrustDomain -eq 'reviewed-fixture') {
        $protection = Read-Stage5FinalAcceptanceProtectedAttestation `
            -Path $Path -Document $document -Kind $Kind -Role $Role `
            -EvidenceTitle $EvidenceTitle -TrustDomain $documentTrustDomain `
            -ExpectedSourceCommit $ExpectedSourceCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256
        Assert-Stage5JsonShape $provenance @('kind', 'reviewedBy', 'reviewedUtc',
            'fixtureManifest') "$context reviewed-fixture provenance"
        $reviewedProvenanceKind = Get-Stage5JsonValue $provenance 'kind' `
            "$context reviewed-fixture provenance"
        $reviewedBy = Get-Stage5JsonValue $provenance 'reviewedBy' `
            "$context reviewed-fixture provenance"
        $reviewedUtc = Get-Stage5JsonValue $provenance 'reviewedUtc' `
            "$context reviewed-fixture provenance"
        Assert-Stage5Condition ($reviewedProvenanceKind -is [string] -and
            $reviewedBy -is [string] -and $reviewedUtc -is [string] -and
            $reviewedProvenanceKind -ceq 'reviewed-fixture' -and
            -not [string]::IsNullOrWhiteSpace($reviewedBy)) `
            "$context reviewed-fixture provenance is not an external review record."
        [DateTimeOffset]$reviewed = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($reviewedUtc, [ref]$reviewed)) `
            "$context reviewed-fixture reviewedUtc is not a valid timestamp."
        $manifestReference = Get-Stage5JsonValue $provenance 'fixtureManifest' "$context reviewed-fixture provenance"
        Assert-Stage5JsonShape $manifestReference @('path', 'sha256') "$context fixture manifest reference"
        $manifestReferencePath = Get-Stage5JsonValue $manifestReference 'path' `
            "$context fixture manifest reference"
        $manifestReferenceSha256 = Get-Stage5JsonValue $manifestReference 'sha256' `
            "$context fixture manifest reference"
        Assert-Stage5Condition ($manifestReferencePath -is [string] -and
            $manifestReferenceSha256 -is [string] -and
            $manifestReferenceSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
            "$context fixture manifest reference path and hash must be JSON strings."
        $manifestPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $Path) `
            $manifestReferencePath `
            "$context fixture manifest reference"
        $manifestSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $manifestPath `
            "$context fixture manifest reference"
        $manifestHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $manifestSnapshot `
            $manifestReferenceSha256 `
            "$context fixture manifest reference"
        $fixtureDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $manifestSnapshot `
            "$context reviewed fixture manifest"
        $reviewedFixtureManifest = $fixtureDocument
        $reviewedFixtureManifestSnapshot = $manifestSnapshot
        Assert-Stage5JsonShape $fixtureDocument @('schemaVersion', 'title',
            'executable', 'executableSha256', 'fixtures', 'ai') `
            "$context reviewed fixture manifest"
        $fixtureSchemaVersion = Get-Stage5JsonValue $fixtureDocument `
            'schemaVersion' "$context reviewed fixture manifest"
        $fixtureManifestTitle = Get-Stage5JsonValue $fixtureDocument 'title' `
            "$context reviewed fixture manifest"
        Assert-Stage5Condition ((Test-Stage5JsonInteger $fixtureSchemaVersion) -and
            $fixtureSchemaVersion -eq 2 -and
            $fixtureManifestTitle -is [string] -and
            $fixtureManifestTitle -ceq $EvidenceTitle) `
            "$context reviewed fixture manifest must be the title-qualified V2 live-qualification authority."
        $manifestExecutableHash = Get-Stage5JsonValue $fixtureDocument 'executableSha256' "$context reviewed fixture manifest"
        $expectedManifestExecutable = if ($EvidenceTitle -ceq 'Generals') { $expectedGenerals } else { $expectedZeroHour }
        Assert-Stage5Condition ($manifestExecutableHash -is [string] -and
            $manifestExecutableHash -cmatch '^[0-9A-Fa-f]{64}$' -and
            $manifestExecutableHash.ToUpperInvariant() -ceq $expectedManifestExecutable.ToUpperInvariant()) `
            "$context reviewed fixture manifest executable binding is stale."
        $fixtureEntries = Get-Stage5JsonValue $fixtureDocument 'fixtures' "$context reviewed fixture manifest"
        Assert-Stage5Condition ($fixtureEntries -is [Array]) "$context reviewed fixture list is not an array."
        $fixtureIds = New-Object 'Collections.Generic.List[string]'
        $stressCount = 0
        foreach ($fixture in $fixtureEntries) {
            Assert-Stage5JsonShape $fixture @('id', 'source', 'sha256', 'stress') "$context reviewed fixture"
            $fixtureId = Get-Stage5JsonValue $fixture 'id' "$context reviewed fixture"
            $fixtureSource = Get-Stage5JsonValue $fixture 'source' "$context reviewed fixture"
            $fixtureExpectedHash = Get-Stage5JsonValue $fixture 'sha256' "$context reviewed fixture"
            $fixtureStress = Get-Stage5JsonValue $fixture 'stress' "$context reviewed fixture"
            Assert-Stage5Condition ($fixtureId -is [string] -and -not ($fixtureIds -contains $fixtureId) -and
                $fixtureSource -is [string] -and $fixtureExpectedHash -is [string] -and
                $fixtureExpectedHash -cmatch '^[0-9A-Fa-f]{64}$' -and
                $fixtureStress -is [bool]) "$context reviewed fixture entry is malformed or duplicated."
            $fixturePath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $manifestPath) `
                $fixtureSource "$context reviewed fixture '$fixtureId'"
            $fixtureSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $fixturePath `
                "$context reviewed fixture '$fixtureId'" -HashOnly -EvidenceKind Replay
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 $fixtureSnapshot `
                $fixtureExpectedHash $fixtureSnapshot.length `
                "$context reviewed fixture '$fixtureId'" | Out-Null
            if ([bool]$fixtureStress) { ++$stressCount }
            $fixtureIds.Add($fixtureId) | Out-Null
            if (@($fixture.Keys | Where-Object { [string]$_ -ceq 'maps' }).Count -gt 0) {
                $maps = Get-Stage5JsonValue $fixture 'maps' "$context reviewed fixture '$fixtureId'"
                Assert-Stage5Condition ($maps -is [Array]) "$context reviewed fixture '$fixtureId' maps is not an array."
                foreach ($map in $maps) {
                    Assert-Stage5JsonShape $map @('source', 'profileRelativePath', 'sha256') `
                        "$context reviewed fixture '$fixtureId' map"
                    $mapSource = Get-Stage5JsonValue $map 'source' `
                        "$context reviewed fixture '$fixtureId' map"
                    $mapSha256 = Get-Stage5JsonValue $map 'sha256' `
                        "$context reviewed fixture '$fixtureId' map"
                    Assert-Stage5Condition ($mapSource -is [string] -and
                        $mapSha256 -is [string] -and
                        $mapSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
                        "$context reviewed fixture '$fixtureId' map source and hash must be JSON strings."
                    $mapPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $manifestPath) `
                        $mapSource `
                        "$context reviewed fixture '$fixtureId' map"
                    $mapSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $mapPath `
                        "$context reviewed fixture '$fixtureId' map"
                    Assert-Stage5FinalAcceptanceSnapshotSha256 $mapSnapshot `
                        $mapSha256 `
                        "$context reviewed fixture '$fixtureId' map" | Out-Null
                }
            }
        }
        $fixtureDetails = Get-Stage5JsonValue $details 'fixtureCount' "$context details"
        $stressDetails = Get-Stage5JsonValue $details 'stressFixtureCount' "$context details"
        $fixtureSetDetails = Get-Stage5JsonValue $details 'fixtureSetSha256' "$context details"
        Assert-Stage5Condition ((Test-Stage5JsonInteger $fixtureDetails) -and
            [Int64]$fixtureDetails -eq $fixtureEntries.Count -and
            (Test-Stage5JsonInteger $stressDetails) -and [Int64]$stressDetails -eq $stressCount -and
            $fixtureSetDetails -is [string] -and $fixtureSetDetails.ToUpperInvariant() -ceq $manifestHash.ToUpperInvariant()) `
            "$context reviewed fixture details do not match the independently hashed fixture manifest."
    }
    else {
        Assert-Stage5JsonShape $provenance @('kind', 'reviewedBy', 'reviewedUtc') `
            "$context external provenance"
        $expectedKind = if ($documentTrustDomain -eq 'premium-review') {
            'premium-review'
        }
        else { 'manual-approval' }
        $externalKind = Get-Stage5JsonValue $provenance 'kind' `
            "$context external provenance"
        $externalReviewedBy = Get-Stage5JsonValue $provenance 'reviewedBy' `
            "$context external provenance"
        $externalReviewedUtc = Get-Stage5JsonValue $provenance 'reviewedUtc' `
            "$context external provenance"
        Assert-Stage5Condition ($externalKind -is [string] -and
            $externalKind -ceq $expectedKind -and
            $externalReviewedBy -is [string] -and
            -not [string]::IsNullOrWhiteSpace($externalReviewedBy) -and
            $externalReviewedUtc -is [string]) `
            "$context external provenance is not an authorized protected record."
        [DateTimeOffset]$externalRecorded = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($externalReviewedUtc, [ref]$externalRecorded)) `
            "$context external provenance reviewedUtc is not a valid timestamp."
        $protection = Read-Stage5FinalAcceptanceProtectedAttestation `
            -Path $Path -Document $document -Kind $Kind -Role $Role `
            -EvidenceTitle $EvidenceTitle -TrustDomain $documentTrustDomain `
            -ExpectedSourceCommit $ExpectedSourceCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256
        if ($documentTrustDomain -eq 'manual-approval') {
            Assert-Stage5Condition ($externalReviewedBy -notmatch '(?i)runner|script|automation|pipeline') `
                "$context manual approval is attributed to an automated producer."
        }
    }
    switch ($Role) {
        'validation-plan' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'gateName' "$context details") -is [string] -and
                (Get-Stage5JsonValue $details 'gateName' "$context details") -ceq 'deterministic-runtime' -and
                (Get-Stage5JsonValue $details 'validationSet' "$context details") -is [string] -and
                (Get-Stage5JsonValue $details 'validationSet' "$context details") -ceq 'All' -and
                (Test-Stage5JsonInteger (Get-Stage5JsonValue $details 'entryCount' "$context details")) -and
                [Int64](Get-Stage5JsonValue $details 'entryCount' "$context details") -gt 0) `
                "$context role details are not semantically valid."
        }
        'validation-results' {
            $resultCount = Get-Stage5JsonValue $details 'resultCount' "$context details"
            $allPassed = Get-Stage5JsonValue $details 'allExecutionsPassed' "$context details"
            $resultsHash = Get-Stage5JsonValue $details 'resultsSha256' "$context details"
            Assert-Stage5Condition ($resultsHash -is [string] -and
                $resultsHash -cmatch '^[0-9A-Fa-f]{64}$') `
                "$context validation-results hash must be a SHA-256 string."
            $matchingResultsLogs = @($validatedRawLogs | Where-Object {
                $_.sha256 -ceq $resultsHash.ToUpperInvariant()
            })
            Assert-Stage5Condition ((Test-Stage5JsonInteger $resultCount) -and
                [Int64]$resultCount -gt 0 -and $allPassed -is [bool] -and
                $allPassed -and $resultsHash -is [string] -and
                $matchingResultsLogs.Count -eq 1) `
                ("$context role details are not semantically valid or do not bind exactly one retained validation-results log " +
                "(declared=$resultsHash; retained=$(@($validatedRawLogs | ForEach-Object { $_.name + ':' + $_.sha256 }) -join ',')).")
        }
        'replay-results' {
            $unique = Get-Stage5JsonValue $details 'uniqueReplayCount' "$context details"
            $executions = Get-Stage5JsonValue $details 'executionCount' "$context details"
            $crcTree = Get-Stage5JsonValue $details 'crcTreeSha256' "$context details"
            $allPassed = Get-Stage5JsonValue $details 'allExecutionsPassed' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $unique) -and
                [Int64]$unique -ge 10 -and (Test-Stage5JsonInteger $executions) -and
                [Int64]$executions -ge [Int64]$unique -and $allPassed -is [bool] -and
                $allPassed -and $crcTree -is [string] -and
                $crcTree -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'replay-fixture-manifest' {
            # Reviewed fixture semantics, including independent fixture and map
            # rehashes, are validated in the reviewed-fixture trust-domain branch.
        }
        'ai-results' {
            $scenarioCount = Get-Stage5JsonValue $details 'scenarioCount' "$context details"
            $seedCount = Get-Stage5JsonValue $details 'distinctSeedCount' "$context details"
            $repeatCount = Get-Stage5JsonValue $details 'repeatCount' "$context details"
            $allCompleted = Get-Stage5JsonValue $details 'allGamesCompleted' "$context details"
            $digestTree = Get-Stage5JsonValue $details 'digestTreeSha256' "$context details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $scenarioCount) -and
                [Int64]$scenarioCount -ge 2 -and (Test-Stage5JsonInteger $seedCount) -and
                [Int64]$seedCount -ge 3 -and (Test-Stage5JsonInteger $repeatCount) -and
                [Int64]$repeatCount -ge 2 -and $allCompleted -is [bool] -and
                $allCompleted -and $digestTree -is [string] -and
                $digestTree -match '^[0-9A-Fa-f]{64}$') `
                "$context role details are not semantically valid."
        }
        'combined-results' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'pipelineMode' "$context details") -ceq 'serial' -and
                (Get-Stage5JsonValue $details 'simulationMode' "$context details") -ceq 'serial' -and
                (Get-Stage5JsonValue $details 'requestedWorkers' "$context details") -ceq '1' -and
                (Get-Stage5JsonValue $details 'workerPolicy' "$context details") -ceq 'auto' -and
                (Test-Stage5JsonInteger (Get-Stage5JsonValue $details `
                    'projectionSequence' "$context details")) -and
                [int](Get-Stage5JsonValue $details 'projectionSequence' `
                    "$context details") -eq 1 -and
                (Get-Stage5JsonValue $details 'projectionSemantics' `
                    "$context details") -ceq 'deterministic-lineage-pointer' -and
                (Test-Stage5JsonInteger (Get-Stage5JsonValue $details `
                    'sourceChildCount' "$context details")) -and
                [int](Get-Stage5JsonValue $details 'sourceChildCount' `
                    "$context details") -eq 253 -and
                (Get-Stage5JsonValue $details 'bothTitlesPassed' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'bothTitlesPassed' "$context details")) `
                "$context role details are not semantically valid."
            $sourceCorpora = Get-Stage5JsonValue $details 'sourceCorpora' `
                "$context details"
            Assert-Stage5Condition ($sourceCorpora -is [Array] -and
                $sourceCorpora.Count -eq 2) `
                "$context sourceCorpora must be the exact ordered Generals/ZeroHour pair."
            $sourceCorpusTitle0 = Get-Stage5JsonValue $sourceCorpora[0] 'title' `
                "$context sourceCorpora[0]"
            $sourceCorpusTitle1 = Get-Stage5JsonValue $sourceCorpora[1] 'title' `
                "$context sourceCorpora[1]"
            Assert-Stage5Condition ($sourceCorpusTitle0 -is [string] -and
                $sourceCorpusTitle1 -is [string] -and
                $sourceCorpusTitle0 -ceq 'Generals' -and
                $sourceCorpusTitle1 -ceq 'ZeroHour') `
                "$context sourceCorpora must be the exact ordered Generals/ZeroHour pair."
        }
        'premium-review-results' {
            $reviewedCommit = Get-Stage5JsonValue $details 'reviewedCommit' "$context details"
            $rounds = Get-Stage5JsonValue $details 'reviewRounds' "$context details"
            $reviewers = Get-Stage5JsonValue $details 'independentReviewers' "$context details"
            $openP0 = Get-Stage5JsonValue $details 'openP0' "$context details"
            $openP1 = Get-Stage5JsonValue $details 'openP1' "$context details"
            $openP2 = Get-Stage5JsonValue $details 'openP2' "$context details"
            Assert-Stage5Condition ($reviewedCommit -is [string] -and
                $reviewedCommit -ceq $ExpectedSourceCommit -and
                (Test-Stage5JsonInteger $rounds) -and [Int64]$rounds -gt 0 -and
                (Test-Stage5JsonInteger $reviewers) -and [Int64]$reviewers -gt 0 -and
                (Test-Stage5JsonInteger $openP0) -and [Int64]$openP0 -eq 0 -and
                (Test-Stage5JsonInteger $openP1) -and [Int64]$openP1 -eq 0 -and
                (Test-Stage5JsonInteger $openP2) -and [Int64]$openP2 -eq 0) `
                "$context role details are not semantically valid."
        }
        'manual-checklist' {
            Assert-Stage5Condition ((Get-Stage5JsonValue $details 'approvalScope' "$context details") -ceq
                'final-stage5-installed-runtime' -and
                (Get-Stage5JsonValue $details 'candidateHashVerified' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'candidateHashVerified' "$context details") -and
                (Get-Stage5JsonValue $details 'bothTitlesTested' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'bothTitlesTested' "$context details") -and
                (Get-Stage5JsonValue $details 'cleanExitPassed' "$context details") -is [bool] -and
                (Get-Stage5JsonValue $details 'cleanExitPassed' "$context details")) `
                "$context role details are not semantically valid."
        }
    }
    if ($null -ne $SeenRunNonces) {
        if ($null -ne $runNonce -and $SeenRunNonces.Contains($runNonce)) {
            $prior = $SeenRunNonces[$runNonce]
            $sameImmutableReceipt = $prior -is [Collections.IDictionary] -and
                [string]$prior['binding'] -ceq "$Kind/$Role" -and
                [string]$prior['title'] -ceq $EvidenceTitle -and
                [string]$prior['sha256'] -ceq
                    ([string]$documentSnapshot.sha256).ToUpperInvariant()
            if (-not $AllowIdenticalNonceReuse -or -not $sameImmutableReceipt) {
                throw "replayed: $context reuses runNonce '$runNonce' already bound to '$prior'."
            }
        }
        elseif ($null -ne $runNonce) {
            $SeenRunNonces[$runNonce] = [ordered]@{
                binding = "$Kind/$Role"
                title = $EvidenceTitle
                sha256 = ([string]$documentSnapshot.sha256).ToUpperInvariant()
                identity = [string]$documentSnapshot.identity
                path = [string]$documentSnapshot.path
            }
        }
    }
    return [pscustomobject]@{
        role = $Role
        trustDomain = $documentTrustDomain
        runNonce = $runNonce
        producer = $producer
        producerVersion = $producerVersion
        cohortNonce = $cohortNonce
        runtimeClosure = $runtimeClosure
        path = [string]$documentSnapshot.path
        sha256 = ([string]$documentSnapshot.sha256).ToUpperInvariant()
        snapshot = $documentSnapshot
        document = $document
        details = $details
        rawLogs = $validatedRawLogs.ToArray()
        validatedChildren = $validatedChildren.ToArray()
        provenance = $provenance
        protection = $protection
        reviewedFixtureManifest = $reviewedFixtureManifest
        reviewedFixtureManifestSnapshot = $reviewedFixtureManifestSnapshot
        qualificationData = $qualificationData
        qualificationDataEvidence = $qualificationDataEvidence
        combinedSourceBindings = $combinedSourceBindings
        acceptanceFailure = $null
    }
}

function Get-Stage5DevelopmentReadinessResultTreeSha256 {
    param([object[]]$Results, [ValidateSet('replay', 'ai')][string]$Kind)
    $lines = New-Object 'Collections.Generic.List[string]'
    $orderedResults = @($Results | Where-Object {
                [string]$_.kind -ceq $Kind
            } | Sort-Object -Property @{
                Expression = { [Int64]$_.sequence }
                Ascending = $true
            })
    foreach ($result in $orderedResults) {
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
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
        (($lines.ToArray() -join "`n") + "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Assert-Stage5DevelopmentReadinessSemanticEqual {
    param([object]$Actual, [object]$Expected, [string]$Context)
    $actualJson = ConvertTo-Json -InputObject $Actual -Compress -Depth 64
    $expectedJson = ConvertTo-Json -InputObject $Expected -Compress -Depth 64
    Assert-Stage5Condition ($actualJson -ceq $expectedJson) `
        "$Context differs from the independently re-parsed retained output."
}

function Assert-Stage5DevelopmentReadinessExecutionStreamEvidence {
    param(
        [object]$Entry,
        [object]$Result,
        [object[]]$ValidatedRawLogs,
        [string]$Context
    )
    $stdoutPathText = Get-Stage5JsonValue $Entry 'stdout' "$Context plan entry"
    $stderrPathText = Get-Stage5JsonValue $Entry 'stderr' "$Context plan entry"
    Assert-Stage5Condition ($stdoutPathText -is [string] -and
        $stderrPathText -is [string]) `
        "$Context stream paths must be JSON strings."
    $stdoutLeaf = [IO.Path]::GetFileName($stdoutPathText)
    $stderrLeaf = [IO.Path]::GetFileName($stderrPathText)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($stdoutLeaf) -and
        -not [string]::IsNullOrWhiteSpace($stderrLeaf) -and
        $stdoutLeaf -cne $stderrLeaf) `
        "$Context stream paths must have distinct canonical leaves."
    $stdout = Get-Stage5DevelopmentReadinessRawLog $ValidatedRawLogs $stdoutLeaf `
        "$Context stdout"
    $stderr = Get-Stage5DevelopmentReadinessRawLog $ValidatedRawLogs $stderrLeaf `
        "$Context stderr"
    $stdoutResultSha256 = Get-Stage5JsonValue $Result 'stdoutSha256' $Context
    $stderrResultSha256 = Get-Stage5JsonValue $Result 'stderrSha256' $Context
    Assert-Stage5Condition ($stdoutResultSha256 -is [string] -and
        $stderrResultSha256 -is [string] -and
        $stdoutResultSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $stderrResultSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $stdoutResultSha256.ToUpperInvariant() -ceq $stdout.sha256 -and
        $stderrResultSha256.ToUpperInvariant() -ceq $stderr.sha256) `
        "$Context result is detached from its retained child streams."
    $strictUtf8 = New-Object Text.UTF8Encoding($false, $true)
    try {
        $stdoutText = $strictUtf8.GetString([byte[]]$stdout.snapshot.bytes)
    }
    catch {
        throw "$Context stdout is not canonical UTF-8: $($_.Exception.Message)"
    }
    try {
        $stderrText = $strictUtf8.GetString([byte[]]$stderr.snapshot.bytes)
    }
    catch {
        throw "$Context stderr is not canonical UTF-8: $($_.Exception.Message)"
    }
    return [pscustomobject]@{
        stdout = $stdout
        stderr = $stderr
        stdoutText = $stdoutText
        stderrText = $stderrText
    }
}

function Get-Stage5DevelopmentReadinessRawLog {
    param([object[]]$RawLogs, [string]$LeafName, [string]$Context)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($LeafName) -and
        [IO.Path]::GetFileName($LeafName) -ceq $LeafName) `
        "$Context requires a canonical raw-log leaf name."
    $matches = @($RawLogs | Where-Object {
        [string]$_.name -ceq $LeafName
    })
    Assert-Stage5Condition ($matches.Count -eq 1) `
        "$Context requires exactly one retained raw log named '$LeafName'."
    $record = $matches[0]
    Assert-Stage5Condition ($null -ne $record.snapshot -and
        $null -ne $record.snapshot.bytes -and
        [string]$record.sha256 -cmatch '^[0-9A-F]{64}$') `
        "$Context raw-log snapshot is unavailable or malformed."
    return $record
}

function Assert-Stage5DevelopmentReadinessArguments {
    param([object]$Entry, [string]$ExecutableSha256, [string]$Context)
    $workerCounts = @{
        'serial-1' = '1'; 'parallel-1' = '1'; 'parallel-2' = '2'
        'parallel-4' = '4'; 'parallel-8' = '8'; 'parallel-16' = '16'
        'parallel-auto' = $null; 'shadow-16' = '16'
    }
    $configuration = Get-Stage5JsonValue $Entry 'configuration' $Context
    Assert-Stage5Condition ($configuration -is [string] -and
        $workerCounts.ContainsKey($configuration)) `
        "$Context uses unsupported configuration '$configuration'."
    $simulationMode = Get-Stage5JsonValue $Entry 'simulationMode' $Context
    Assert-Stage5Condition ($simulationMode -is [string]) `
        "$Context simulationMode must be a JSON string."
    $expected = @(
        '-headless', '-noFPSLimit', '-pipelineMode', 'serial',
        '-simulationMode', $simulationMode,
        '-workerPolicy', 'auto', '-validationExecutableSha256', $ExecutableSha256
    )
    if ($null -ne $workerCounts[$configuration]) {
        $expected += @('-workerCount', [string]$workerCounts[$configuration])
    }
    $kind = Get-Stage5JsonValue $Entry 'kind' $Context
    Assert-Stage5Condition ($kind -is [string]) `
        "$Context kind must be a JSON string."
    if ($kind -ceq 'replay') {
        $replayArgument = Get-Stage5JsonValue $Entry 'replayArgument' $Context
        Assert-Stage5Condition ($replayArgument -is [string]) `
            "$Context replayArgument must be a JSON string."
        $expected += @('-replay', $replayArgument)
    }
    elseif ($kind -ceq 'ai') {
        $scenario = Get-Stage5JsonValue $Entry 'scenario' $Context
        Assert-Stage5Condition ($scenario -is [string]) `
            "$Context scenario must be a JSON string."
        $runnerFlag = switch ($scenario) {
            '4v2' { '-runSkirmishAITest4v2' }
            '4v3' { '-runSkirmishAITest' }
            default { throw "$Context uses unsupported AI scenario '$scenario'." }
        }
        $seed = Get-Stage5JsonValue $Entry 'seed' $Context
        Assert-Stage5Condition (Test-Stage5JsonInteger $seed) `
            "$Context seed must retain its JSON integer type."
        $expected += @($runnerFlag, [string]$seed)
    }
    else { throw "$Context uses unsupported execution kind '$kind'." }
    $actual = Get-Stage5JsonValue $Entry 'arguments' $Context
    Assert-Stage5Condition ($actual -is [Array] -and
        @($actual | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        $actual.Count -eq $expected.Count) `
        "$Context arguments contain missing, extra, or behavior-changing tokens."
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        Assert-Stage5Condition ($actual[$index] -ceq $expected[$index]) `
            "$Context arguments differ from the canonical execution contract at token $index."
    }
}

function Assert-Stage5DevelopmentReadinessExecutionEvidence {
    param(
        [object]$ValidationPlan,
        [object[]]$Results,
        [object]$ReviewedFixtureManifest,
        [object]$PlanDetails,
        [object]$ValidationDetails,
        [object]$ReplayDetails,
        [object]$AiDetails,
        [object[]]$ValidatedRawLogs,
        [object[]]$ValidatedChildren,
        [string]$ExpectedPlanSha256,
        [string]$ValidationResultsSha256,
        [string]$ReplayResultsSha256,
        [string]$AiResultsSha256,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [object]$ExpectedRuntimeClosure,
        [object]$ExpectedQualificationData,
        [string]$ExpectedCurrentExecutablePath = '',
        [switch]$RequireCurrentArtifactRelocation,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle = 'ZeroHour'
    )
    $context = 'Stage 5 development-readiness execution evidence'
    $readinessDiagnosticStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $writeReadinessDiagnostic = {
        param([string]$Phase, [string]$State, [int]$Index = 0,
            [int]$Total = 0)
        Write-Verbose (('STAGE5_READINESS_PHASE context={0} phase={1} state={2} ' +
            'index={3} total={4} elapsedMs={5}') -f $context, $Phase, $State,
            $Index, $Total, $readinessDiagnosticStopwatch.ElapsedMilliseconds)
    }
    & $writeReadinessDiagnostic 'input-and-cardinality' 'start'
    Assert-Stage5Condition ($null -ne $ValidationPlan -and
        $null -ne $ReviewedFixtureManifest -and $Results.Count -gt 0) `
        "$context is missing its validation plan, result set, or reviewed corpus."
    foreach ($hash in @($ExpectedPlanSha256, $ValidationResultsSha256,
            $ReplayResultsSha256, $AiResultsSha256,
            $ExpectedArtifactSetSha256)) {
        Assert-Stage5Condition ($hash -cmatch '^[0-9A-Fa-f]{64}$') `
            "$context plan, result-set, or artifact binding is not a SHA-256 digest."
    }
    Assert-Stage5Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$') `
        "$context expected source commit is not canonical lowercase 40-hex."
    Assert-Stage5CanonicalUuid $ExpectedCohortNonce `
        "$context expected cohortNonce" | Out-Null
    [DateTimeOffset]$expectedCohortCreated = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
            [ref]$expectedCohortCreated) -and
        $expectedCohortCreated -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
        "$context expected cohortCreatedUtc is invalid or in the future."
    [void](Assert-Stage5RuntimeClosureBinding $ExpectedRuntimeClosure `
        $ExpectedRuntimeClosure "$context expected runtime closure")
    $resultsSha256 = $ValidationResultsSha256.ToUpperInvariant()
    Assert-Stage5Condition ($ReplayResultsSha256.ToUpperInvariant() -ceq
            $resultsSha256 -and $AiResultsSha256.ToUpperInvariant() -ceq
            $resultsSha256) `
        "$context receipts do not bind one byte-identical validation result set."

    $planEntries = Get-Stage5JsonValue $ValidationPlan 'entries' "$context plan"
    $executableSha256 = Get-Stage5JsonValue $ValidationPlan `
        'executableSha256' "$context plan"
    Assert-Stage5Condition ($executableSha256 -is [string] -and
        $executableSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        (Get-Stage5JsonValue $ValidationPlan 'gateName' "$context plan") -ceq
            'deterministic-runtime' -and
        (Get-Stage5JsonValue $ValidationPlan 'validationSet' "$context plan") -ceq
            'All' -and $planEntries.Count -eq 253 -and $Results.Count -eq 253) `
        "$context does not contain the exact complete deterministic-runtime plan/result cardinality."
    $executableSha256 = $executableSha256.ToUpperInvariant()
    $planExecutablePath = Get-Stage5JsonValue $ValidationPlan 'executable' `
        "$context plan"
    Assert-Stage5Condition ($planExecutablePath -is [string] -and
        -not [string]::IsNullOrWhiteSpace($planExecutablePath) -and
        [IO.Path]::IsPathRooted($planExecutablePath) -and
        (Get-Stage5JsonValue $ValidationPlan 'title' "$context plan") -ceq
            $ExpectedTitle -and
        (Get-Stage5JsonValue $ValidationPlan 'cohortNonce' "$context plan") -ceq
            $ExpectedCohortNonce -and
        (Get-Stage5JsonValue $ValidationPlan 'cohortCreatedUtc' "$context plan") -ceq
            $ExpectedCohortCreatedUtc) `
        "$context plan is detached from the installed executable or current cohort."
    $artifactRelocation = Get-Stage5DevelopmentReadinessArtifactRelocationBinding `
        -ValidationPlan $ValidationPlan -ExpectedTitle $ExpectedTitle `
        -ExpectedExecutableSha256 $executableSha256 `
        -CurrentExecutablePath $ExpectedCurrentExecutablePath `
        -RequireCurrentArtifactRelocation:$RequireCurrentArtifactRelocation `
        -Context "$context $ExpectedTitle artifact relocation"
    [void](Assert-Stage5SimulationQualificationBindingEqual `
        (Get-Stage5JsonValue $ValidationPlan 'qualificationData' "$context plan") `
        $ExpectedQualificationData "$context plan qualificationData")
    $reviewedCorpusSchemaVersion = Get-Stage5JsonValue $ReviewedFixtureManifest `
        'schemaVersion' "$context reviewed corpus"
    Assert-Stage5Condition ((Test-Stage5JsonInteger $reviewedCorpusSchemaVersion) -and
        $reviewedCorpusSchemaVersion -eq 2) `
        "$context reviewed corpus is not the V2 live-qualification authority."
    $reviewedAiContract = Get-Stage5JsonValue $ReviewedFixtureManifest 'ai' `
        "$context reviewed corpus"
    Assert-Stage5DevelopmentReadinessSemanticEqual `
        (Get-Stage5JsonValue $ValidationPlan 'liveQualification' "$context plan") `
        (Get-Stage5JsonValue $reviewedAiContract 'liveQualification' `
            "$context reviewed corpus AI") `
        "$context plan liveQualification"
    [void](Assert-Stage5RuntimeClosureBinding `
        (Get-Stage5JsonValue $ValidationPlan 'runtimeClosure' "$context plan") `
        $ExpectedRuntimeClosure "$context plan")
    $validationDetailsResultsSha256 = Get-Stage5JsonValue $ValidationDetails `
        'resultsSha256' "$context validation-results receipt details"
    Assert-Stage5Condition ($validationDetailsResultsSha256 -is [string] -and
        $validationDetailsResultsSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        [int](Get-Stage5JsonValue $PlanDetails 'entryCount' `
            "$context validation-plan receipt details") -eq 253 -and
        [int](Get-Stage5JsonValue $ValidationDetails 'resultCount' `
            "$context validation-results receipt details") -eq 253 -and
        [bool](Get-Stage5JsonValue $ValidationDetails 'allExecutionsPassed' `
            "$context validation-results receipt details") -and
            $validationDetailsResultsSha256.ToUpperInvariant() -ceq
            $resultsSha256) `
        "$context receipt details are detached from the exact complete execution set."
    & $writeReadinessDiagnostic 'input-and-cardinality' 'complete'
    foreach ($receiptDetails in @(
        [pscustomobject]@{ value = $PlanDetails; name = 'validation-plan' },
        [pscustomobject]@{ value = $ValidationDetails; name = 'validation-results' },
        [pscustomobject]@{ value = $ReplayDetails; name = 'replay-results' },
        [pscustomobject]@{ value = $AiDetails; name = 'ai-results' }
    )) {
        [void](Assert-Stage5SimulationQualificationBindingEqual `
            (Get-Stage5JsonValue $receiptDetails.value 'qualificationData' `
                "$context $($receiptDetails.name) receipt details") `
            $ExpectedQualificationData `
            "$context $($receiptDetails.name) qualificationData")
    }

    Assert-Stage5Condition ($ValidatedChildren -is [Array] -and
        $ValidatedChildren.Count -eq 253) `
        "$context requires exactly one validated native child for every result."
    $childrenBySequence = @{}
    $childNonces = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    $childProcesses = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    $childNativePaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    & $writeReadinessDiagnostic 'validated-child-native-closure' 'start' `
        0 $ValidatedChildren.Count
    for ($childIndex = 0; $childIndex -lt $ValidatedChildren.Count; ++$childIndex) {
        $childNumber = $childIndex + 1
        if ($childNumber -eq 1 -or ($childNumber % 32) -eq 0 -or
            $childNumber -eq 169 -or $childNumber -eq 170 -or
            $childNumber -eq $ValidatedChildren.Count) {
            & $writeReadinessDiagnostic 'validated-child-native-closure' `
                'progress' $childNumber $ValidatedChildren.Count
        }
        $child = $ValidatedChildren[$childIndex]
        $childContext = "$context validated child $($childIndex + 1)"
        Assert-Stage5JsonShape $child @('sequence', 'role', 'title', 'runNonce',
            'processId', 'processCreationUtc', 'executablePath',
            'executableSha256', 'commandLine', 'arguments', 'exitCode', 'stdout',
            'stderr', 'nativeReceipt', 'qualificationData') $childContext
        $childSequence = Get-Stage5JsonValue $child 'sequence' $childContext
        $childNonce = Get-Stage5JsonValue $child 'runNonce' $childContext
        $childProcessId = Get-Stage5JsonValue $child 'processId' $childContext
        $childCreationText = Get-Stage5JsonValue $child `
            'processCreationUtc' $childContext
        $childArguments = Get-Stage5JsonValue $child 'arguments' $childContext
        $childNative = Get-Stage5JsonValue $child 'nativeReceipt' $childContext
        Assert-Stage5JsonShape $childNative @('path', 'sha256', 'producer',
            'runNonce', 'cohortNonce') "$childContext native receipt"
        $childNativePath = Get-Stage5JsonValue $childNative 'path' `
            "$childContext native receipt"
        $childExecutablePath = Get-Stage5JsonValue $child 'executablePath' `
            $childContext
        $childExecutableSha256 = Get-Stage5JsonValue $child 'executableSha256' `
            $childContext
        $childCommandLine = Get-Stage5JsonValue $child 'commandLine' $childContext
        $childNativeSha256 = Get-Stage5JsonValue $childNative 'sha256' `
            "$childContext native receipt"
        Assert-Stage5Condition ($childNonce -is [string] -and
            $childCreationText -is [string] -and
            $childNativePath -is [string] -and
            $childExecutablePath -is [string] -and
            $childExecutableSha256 -is [string] -and
            $childExecutableSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
            $childCommandLine -is [string] -and
            $childNativeSha256 -is [string] -and
            $childNativeSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
            "$childContext identity and hash fields must be JSON strings."
        [DateTimeOffset]$childCreation = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ((Test-Stage5JsonInteger $childSequence) -and
            [int]$childSequence -eq ($childIndex + 1) -and
            -not $childrenBySequence.ContainsKey([int]$childSequence) -and
            (Get-Stage5JsonValue $child 'role' $childContext) -ceq
                'validation-results' -and
            (Get-Stage5JsonValue $child 'title' $childContext) -ceq $ExpectedTitle -and
            $childNonce -cmatch
                '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
            $childNonces.Add($childNonce) -and
            (Test-Stage5JsonInteger $childProcessId) -and
            [Int64]$childProcessId -gt 0 -and
            [DateTimeOffset]::TryParse($childCreationText, [ref]$childCreation) -and
            $childCreation -ge $expectedCohortCreated -and
            $childCreation -le [DateTimeOffset]::UtcNow.AddMinutes(5) -and
            $childProcesses.Add(('{0}|{1}' -f $childProcessId,
                    $childCreation.UtcDateTime.Ticks)) -and
            [IO.Path]::GetFullPath($childExecutablePath) -ceq
                [IO.Path]::GetFullPath($planExecutablePath) -and
            $childExecutableSha256.ToUpperInvariant() -ceq $executableSha256 -and
            -not [string]::IsNullOrWhiteSpace($childCommandLine) -and
            $childArguments -is [Array] -and $childArguments.Count -gt 0 -and
            @($childArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
            (Test-Stage5JsonInteger (Get-Stage5JsonValue $child 'exitCode' `
                    $childContext)) -and [int]$child.exitCode -eq 0 -and
            -not [string]::IsNullOrWhiteSpace($childNativePath) -and
            $childNativePaths.Add($childNativePath) -and
            $childNativeSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
            (Get-Stage5JsonValue $childNative 'producer' `
                "$childContext native receipt") -ceq
                'game-executable-stage5-performance-report-v5' -and
            (Get-Stage5JsonValue $childNative 'runNonce' `
                "$childContext native receipt") -ceq $childNonce -and
            (Get-Stage5JsonValue $childNative 'cohortNonce' `
                "$childContext native receipt") -ceq $ExpectedCohortNonce) `
            "$childContext is stale, aliased, synthetic, or detached from its native execution."
        [void](Assert-Stage5SimulationQualificationBindingEqual `
            (Get-Stage5JsonValue $child 'qualificationData' $childContext) `
            $ExpectedQualificationData "$childContext qualificationData")
        foreach ($streamName in @('stdout', 'stderr')) {
            $stream = Get-Stage5JsonValue $child $streamName $childContext
            Assert-Stage5JsonShape $stream @('path', 'sha256') `
                "$childContext $streamName"
            $streamPath = Get-Stage5JsonValue $stream 'path' `
                "$childContext $streamName"
            $streamSha256 = Get-Stage5JsonValue $stream 'sha256' `
                "$childContext $streamName"
            Assert-Stage5Condition ($streamPath -is [string] -and
                $streamSha256 -is [string] -and
                -not [string]::IsNullOrWhiteSpace($streamPath) -and
                $streamSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
                "$childContext $streamName binding is malformed."
        }
        $childrenBySequence[[int]$childSequence] = $child
    }
    & $writeReadinessDiagnostic 'validated-child-native-closure' 'complete' `
        $ValidatedChildren.Count $ValidatedChildren.Count

    $resultsBySequence = @{}
    & $writeReadinessDiagnostic 'result-identity' 'start' 0 $Results.Count
    $resultIndex = 0
    foreach ($result in $Results) {
        ++$resultIndex
        if ($resultIndex -eq 1 -or ($resultIndex % 32) -eq 0 -or
            $resultIndex -eq $Results.Count) {
            & $writeReadinessDiagnostic 'result-identity' 'progress' `
                $resultIndex $Results.Count
        }
        $resultSequence = Get-Stage5JsonValue $result 'sequence' `
            "$context result"
        $resultExitCode = Get-Stage5JsonValue $result 'exitCode' `
            "$context result"
        $resultTimedOut = Get-Stage5JsonValue $result 'timedOut' `
            "$context result"
        Assert-Stage5Condition ((Test-Stage5JsonInteger $resultSequence) -and
            (Test-Stage5JsonInteger $resultExitCode) -and
            $resultTimedOut -is [bool]) `
            "$context result sequence, exitCode, and timedOut must retain their JSON scalar types."
        $sequence = [int]$resultSequence
        Assert-Stage5Condition ($sequence -ge 1 -and $sequence -le 253 -and
            -not $resultsBySequence.ContainsKey($sequence) -and
            (Get-Stage5JsonValue $result 'title' "$context result $sequence") -ceq
                $ExpectedTitle -and
            [int]$resultExitCode -eq 0 -and
            -not $resultTimedOut) `
            "$context has a failed, missing, duplicate, or out-of-range result sequence."
        foreach ($hashField in @('stdoutSha256', 'stderrSha256')) {
            $hashValue = Get-Stage5JsonValue $result $hashField `
                "$context result $sequence"
            Assert-Stage5Condition ($hashValue -is [string] -and
                $hashValue -cmatch '^[0-9A-Fa-f]{64}$') `
                "$context result $sequence field '$hashField' must be a SHA-256 string."
        }
        $resultsBySequence[$sequence] = $result
    }
    & $writeReadinessDiagnostic 'result-identity' 'complete' `
        $resultIndex $Results.Count
    $identityFields = @('kind', 'caseId', 'determinismKey', 'configuration',
        'simulationMode', 'requestedWorkers', 'workerPolicy', 'repeat',
        'matrixRepeat', 'replayArgument', 'seed', 'scenario', 'fixtureSha256',
        'stress')
    $planStringFields = @('kind', 'caseId', 'determinismKey', 'configuration',
        'simulationMode', 'requestedWorkers', 'workerPolicy', 'replayArgument',
        'scenario', 'fixtureSha256', 'stdout', 'stderr', 'command')
    $planBySequence = @{}
    & $writeReadinessDiagnostic 'plan-result-identity' 'start' 0 $planEntries.Count
    $planIdentityIndex = 0
    foreach ($entry in $planEntries) {
        ++$planIdentityIndex
        if ($planIdentityIndex -eq 1 -or ($planIdentityIndex % 32) -eq 0 -or
            $planIdentityIndex -eq 169 -or $planIdentityIndex -eq 170 -or
            $planIdentityIndex -eq $planEntries.Count) {
            & $writeReadinessDiagnostic 'plan-result-identity' 'progress' `
                $planIdentityIndex $planEntries.Count
        }
        $planSequence = Get-Stage5JsonValue $entry 'sequence' `
            "$context plan entry"
        Assert-Stage5Condition (Test-Stage5JsonInteger $planSequence) `
            "$context plan sequence must retain its JSON integer type."
        $sequence = [int]$planSequence
        Assert-Stage5Condition ($sequence -ge 1 -and $sequence -le 253 -and
            -not $planBySequence.ContainsKey($sequence) -and
            $resultsBySequence.ContainsKey($sequence)) `
            "$context has a missing, duplicate, or out-of-range plan sequence."
        $planBySequence[$sequence] = $entry
        $result = $resultsBySequence[$sequence]
        foreach ($field in $planStringFields) {
            $value = Get-Stage5JsonValue $entry $field `
                "$context plan entry $sequence"
            Assert-Stage5Condition ($value -is [string]) `
                "$context plan entry $sequence field '$field' must be a JSON string."
        }
        foreach ($field in $identityFields) {
            Assert-Stage5DevelopmentReadinessSemanticEqual `
                (Get-Stage5JsonValue $result $field "$context result $sequence") `
                (Get-Stage5JsonValue $entry $field "$context plan entry $sequence") `
                "$context result $sequence field '$field'"
        }
        Assert-Stage5DevelopmentReadinessArguments $entry $executableSha256 `
            "$context plan entry $sequence"
        $child = $childrenBySequence[$sequence]
        $childExecutablePathText = Get-Stage5JsonValue $child 'executablePath' `
            "$context child $sequence"
        $childCommandLineText = Get-Stage5JsonValue $child 'commandLine' `
            "$context child $sequence"
        $planCommandText = Get-Stage5JsonValue $entry 'command' `
            "$context plan entry $sequence"
        $planArguments = Get-Stage5JsonValue $entry 'arguments' `
            "$context plan entry $sequence"
        Assert-Stage5Condition ($childExecutablePathText -is [string] -and
            $childCommandLineText -is [string] -and
            $planCommandText -is [string] -and $planArguments -is [Array] -and
            @($planArguments | Where-Object { $_ -isnot [string] }).Count -eq 0) `
            "$context plan/child execution identity must retain JSON scalar strings."
        Assert-Stage5Condition ([IO.Path]::IsPathRooted(
                $childExecutablePathText) -and
            [IO.Path]::GetFullPath($childExecutablePathText) -ceq
                $artifactRelocation.recordedExecutablePath) `
            "$context child $sequence executable path differs from the authenticated recorded runtimeRoot/executable binding."
        Assert-Stage5Condition ($childCommandLineText -ceq $planCommandText) `
            "$context child $sequence command line differs from the frozen plan."
        Assert-Stage5DevelopmentReadinessSemanticEqual `
            (Get-Stage5JsonValue $child 'arguments' "$context child $sequence") `
            (Get-Stage5JsonValue $entry 'arguments' "$context plan entry $sequence") `
            "$context child $sequence arguments"
        [void](Assert-Stage5FinalAcceptanceNativeCommandLine `
            $planCommandText $artifactRelocation.recordedExecutablePath `
            ([string[]]$planArguments) `
            "$context plan entry $sequence")

        $execution = Get-Stage5JsonValue $result 'executionProvenance' `
            "$context result $sequence"
        Assert-Stage5JsonShape $execution @('schemaVersion', 'sequence', 'role',
            'title', 'runNonce', 'processId', 'processCreationUtc',
            'executablePath', 'executableSha256', 'commandLine', 'arguments',
            'exitCode', 'stdout', 'stderr', 'nativeReceipt', 'plan',
            'sourceCommit', 'artifactSetSha256', 'cohortNonce',
            'cohortCreatedUtc', 'runtimeClosure', 'qualificationData') `
            "$context result $sequence execution provenance"
        $executionSchemaVersion = Get-Stage5JsonValue $execution `
            'schemaVersion' "$context result $sequence execution provenance"
        $executionRole = Get-Stage5JsonValue $execution 'role' `
            "$context result $sequence execution provenance"
        $executionTitle = Get-Stage5JsonValue $execution 'title' `
            "$context result $sequence execution provenance"
        $executionSourceCommit = Get-Stage5JsonValue $execution 'sourceCommit' `
            "$context result $sequence execution provenance"
        $executionArtifactSetSha256 = Get-Stage5JsonValue $execution `
            'artifactSetSha256' "$context result $sequence execution provenance"
        $executionCohortNonce = Get-Stage5JsonValue $execution 'cohortNonce' `
            "$context result $sequence execution provenance"
        $executionCohortCreatedUtc = Get-Stage5JsonValue $execution `
            'cohortCreatedUtc' "$context result $sequence execution provenance"
        Assert-Stage5Condition ($executionRole -is [string] -and
            $executionTitle -is [string] -and
            $executionSourceCommit -is [string] -and
            $executionArtifactSetSha256 -is [string] -and
            $executionArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
            $executionCohortNonce -is [string] -and
            $executionCohortCreatedUtc -is [string]) `
            "$context result $sequence execution provenance identity and hash fields must be JSON strings."
        Assert-Stage5Condition ((Test-Stage5JsonInteger $executionSchemaVersion) -and
            $executionSchemaVersion -eq 1 -and
            [int]$execution.sequence -eq $sequence -and
            $executionRole -ceq 'validation-results' -and
            $executionTitle -ceq $ExpectedTitle -and
            $executionSourceCommit -ceq $ExpectedSourceCommit -and
            $executionArtifactSetSha256.ToUpperInvariant() -ceq
                $ExpectedArtifactSetSha256.ToUpperInvariant() -and
            $executionCohortNonce -ceq $ExpectedCohortNonce -and
            $executionCohortCreatedUtc -ceq $ExpectedCohortCreatedUtc) `
            "$context result $sequence execution provenance is stale or detached from the candidate cohort."
        [void](Assert-Stage5RuntimeClosureBinding $execution.runtimeClosure `
            $ExpectedRuntimeClosure "$context result $sequence execution provenance")
        [void](Assert-Stage5SimulationQualificationBindingEqual `
            (Get-Stage5JsonValue $execution 'qualificationData' `
                "$context result $sequence execution provenance") `
            $ExpectedQualificationData `
            "$context result $sequence execution qualificationData")
        foreach ($field in @('sequence', 'role', 'title', 'runNonce',
                'processId', 'processCreationUtc', 'executablePath',
                'executableSha256', 'commandLine', 'arguments', 'exitCode',
                'stdout', 'stderr', 'nativeReceipt')) {
            Assert-Stage5DevelopmentReadinessSemanticEqual `
                (Get-Stage5JsonValue $execution $field `
                    "$context result $sequence execution provenance") `
                (Get-Stage5JsonValue $child $field "$context child $sequence") `
                "$context result $sequence execution field '$field'"
        }
        $executionPlan = Get-Stage5JsonValue $execution 'plan' `
            "$context result $sequence execution provenance"
        Assert-Stage5JsonShape $executionPlan @('path', 'sha256') `
            "$context result $sequence execution plan"
        $executionPlanPath = Get-Stage5JsonValue $executionPlan 'path' `
            "$context result $sequence execution plan"
        $executionPlanSha256 = Get-Stage5JsonValue $executionPlan 'sha256' `
            "$context result $sequence execution plan"
        Assert-Stage5Condition ($executionPlanPath -is [string] -and
            $executionPlanSha256 -is [string] -and
            $executionPlanSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
            "$context result $sequence execution plan path and hash must be JSON strings."
        Assert-Stage5Condition (-not [IO.Path]::IsPathRooted($executionPlanPath) -and
            $executionPlanPath -notmatch ':' -and
            $executionPlanPath -notmatch '(^|[\\/])\.\.([\\/]|$)' -and
            [IO.Path]::GetFileName($executionPlanPath) -ceq 'validation-plan.json' -and
            $executionPlanSha256.ToUpperInvariant() -ceq
                $ExpectedPlanSha256.ToUpperInvariant()) `
            "$context result $sequence execution plan is stale, unsafe, or detached."
        foreach ($streamName in @('stdout', 'stderr')) {
            $stream = Get-Stage5JsonValue $execution $streamName `
                "$context result $sequence execution provenance"
            $resultHashName = $streamName + 'Sha256'
            $planPathText = Get-Stage5JsonValue $entry $streamName `
                "$context plan entry $sequence"
            $streamPath = Get-Stage5JsonValue $stream 'path' `
                "$context result $sequence $streamName"
            $streamSha256 = Get-Stage5JsonValue $stream 'sha256' `
                "$context result $sequence $streamName"
            $resultHash = Get-Stage5JsonValue $result $resultHashName `
                "$context result $sequence"
            Assert-Stage5Condition ($planPathText -is [string] -and
                $streamPath -is [string] -and $streamSha256 -is [string] -and
                $streamSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
                $resultHash -is [string] -and
                $resultHash -cmatch '^[0-9A-Fa-f]{64}$') `
                "$context result $sequence $streamName path and hash fields must be JSON strings."
            Assert-Stage5Condition ([IO.Path]::GetFileName($streamPath) -ceq
                    [IO.Path]::GetFileName($planPathText) -and
                $streamSha256.ToUpperInvariant() -ceq
                    $resultHash.ToUpperInvariant()) `
                "$context result $sequence $streamName provenance is detached from its plan or result hash."
        }
    }
    & $writeReadinessDiagnostic 'plan-result-identity' 'complete' `
        $planIdentityIndex $planEntries.Count

    $workers = @('serial-1', 'parallel-1', 'parallel-2', 'parallel-4',
        'parallel-8', 'parallel-16', 'parallel-auto')
    $workerContracts = @{
        'serial-1' = @('serial', '1'); 'parallel-1' = @('parallel', '1')
        'parallel-2' = @('parallel', '2'); 'parallel-4' = @('parallel', '4')
        'parallel-8' = @('parallel', '8'); 'parallel-16' = @('parallel', '16')
        'parallel-auto' = @('parallel', 'auto'); 'shadow-16' = @('shadow', '16')
    }
    foreach ($entry in $planEntries) {
        $configuration = Get-Stage5JsonValue $entry 'configuration' `
            "$context plan entry configuration"
        $simulationMode = Get-Stage5JsonValue $entry 'simulationMode' `
            "$context plan entry configuration"
        $requestedWorkers = Get-Stage5JsonValue $entry 'requestedWorkers' `
            "$context plan entry configuration"
        $workerPolicy = Get-Stage5JsonValue $entry 'workerPolicy' `
            "$context plan entry configuration"
        Assert-Stage5Condition ($configuration -is [string] -and
            $simulationMode -is [string] -and $requestedWorkers -is [string] -and
            $workerPolicy -is [string] -and
            $workerContracts.ContainsKey($configuration) -and
            $simulationMode -ceq $workerContracts[$configuration][0] -and
            $requestedWorkers -ceq $workerContracts[$configuration][1] -and
            $workerPolicy -ceq 'auto') `
            "$context plan configuration '$configuration' is not canonical."
    }

    $reviewedFixtures = Get-Stage5JsonValue $ReviewedFixtureManifest 'fixtures' `
        "$context reviewed corpus"
    $fixturesById = @{}
    $fixtureHashes = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $stressIds = New-Object 'Collections.Generic.List[string]'
    foreach ($fixture in $reviewedFixtures) {
        $fixtureId = Get-Stage5JsonValue $fixture 'id' "$context reviewed fixture"
        $fixtureHash = Get-Stage5JsonValue $fixture 'sha256' `
            "$context reviewed fixture '$fixtureId'"
        $stress = Get-Stage5JsonValue $fixture 'stress' "$context reviewed fixture '$fixtureId'"
        Assert-Stage5Condition ($fixtureId -is [string] -and
            $fixtureHash -is [string] -and
            -not [string]::IsNullOrWhiteSpace($fixtureId) -and
            -not $fixturesById.ContainsKey($fixtureId) -and
            $fixtureHash -cmatch '^[0-9A-Fa-f]{64}$' -and
            $fixtureHashes.Add($fixtureHash) -and $stress -is [bool]) `
            "$context reviewed fixture identities are malformed, aliased, or duplicated."
        $fixturesById[$fixtureId] = [pscustomobject]@{
            sha256 = $fixtureHash.ToUpperInvariant(); stress = [bool]$stress
        }
        if ([bool]$stress) { $stressIds.Add($fixtureId) | Out-Null }
    }
    Assert-Stage5Condition ($fixturesById.Count -eq 10 -and $stressIds.Count -eq 1) `
        "$context reviewed corpus must contain exactly ten unique fixtures and one stress fixture."

    $replays = @($planEntries | Where-Object { $_.kind -ceq 'replay' })
    Assert-Stage5Condition ($replays.Count -eq 168 -and
        [int](Get-Stage5JsonValue $ReplayDetails 'uniqueReplayCount' `
            "$context replay receipt details") -eq 10 -and
        [int](Get-Stage5JsonValue $ReplayDetails 'executionCount' `
            "$context replay receipt details") -eq 168 -and
        [bool](Get-Stage5JsonValue $ReplayDetails 'allExecutionsPassed' `
            "$context replay receipt details")) `
        "$context replay matrix or receipt cardinality is not exact."
    $replayMatrixTotal = $workers.Count * 2 * $fixturesById.Count
    & $writeReadinessDiagnostic 'replay-matrix' 'start' 0 $replayMatrixTotal
    $replayMatrixIndex = 0
    foreach ($worker in $workers) {
        foreach ($matrixPass in @(1, 2)) {
            foreach ($fixtureId in @($fixturesById.Keys)) {
                ++$replayMatrixIndex
                if ($replayMatrixIndex -eq 1 -or
                    ($replayMatrixIndex % 32) -eq 0 -or
                    $replayMatrixIndex -eq $replayMatrixTotal) {
                    & $writeReadinessDiagnostic 'replay-matrix' 'progress' `
                        $replayMatrixIndex $replayMatrixTotal
                }
                $fixture = $fixturesById[$fixtureId]
                $matches = @($replays | Where-Object {
                    $_.configuration -ceq $worker -and
                    [int]$_.matrixRepeat -eq $matrixPass -and
                    $_.determinismKey -ceq $fixtureId
                })
                $expectedCount = if ([bool]$fixture.stress) { 3 } else { 1 }
                $repeats = @($matches | ForEach-Object { [int]$_.repeat } |
                    Sort-Object -Unique)
                Assert-Stage5Condition ($matches.Count -eq $expectedCount -and
                    ($repeats -join '|') -ceq ((1..$expectedCount) -join '|') -and
                    @($matches | Where-Object {
                        $_.fixtureSha256.ToUpperInvariant() -cne
                            $fixture.sha256 -or [bool]$_.stress -ne
                            [bool]$fixture.stress -or
                        $_.replayArgument -cne
                            "Stage5Validation\$fixtureId.rep" -or
                        $_.caseId -cne "$fixtureId-p$matrixPass"
                    }).Count -eq 0) `
                    "$context replay matrix is detached from reviewed fixture '$fixtureId' for '$worker' pass $matrixPass."
            }
        }
    }
    & $writeReadinessDiagnostic 'replay-matrix' 'complete' `
        $replayMatrixIndex $replayMatrixTotal

    $reviewedAi = Get-Stage5JsonValue $ReviewedFixtureManifest 'ai' `
        "$context reviewed corpus"
    $aiSeeds = Get-Stage5JsonValue $reviewedAi 'seeds' "$context reviewed AI"
    $aiScenarios = Get-Stage5JsonValue $reviewedAi 'scenarios' "$context reviewed AI"
    $aiRepeats = [int](Get-Stage5JsonValue $reviewedAi 'repeats' "$context reviewed AI")
    Assert-Stage5Condition ($aiSeeds -is [Array] -and
        $aiScenarios -is [Array] -and
        @($aiScenarios | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        $aiSeeds.Count -eq 3 -and
        @($aiSeeds | Sort-Object -Unique).Count -eq 3 -and
        (@($aiScenarios | Sort-Object -CaseSensitive) -join '|') -ceq '4v2|4v3' -and
        $aiRepeats -eq 2) `
        "$context reviewed AI configuration is not the exact scenario/seed/repeat set."
    $regularAi = @($planEntries | Where-Object {
        $_.kind -ceq 'ai' -and $workers -ccontains $_.configuration
    })
    $shadowAi = @($planEntries | Where-Object {
        $_.kind -ceq 'ai' -and $_.configuration -ceq 'shadow-16'
    })
    Assert-Stage5Condition ($regularAi.Count -eq 84 -and $shadowAi.Count -eq 1 -and
        $shadowAi[0].scenario -ceq '4v2' -and
        [int]$shadowAi[0].seed -eq [int]$aiSeeds[0] -and
        [int]$shadowAi[0].repeat -eq 1 -and
        [int](Get-Stage5JsonValue $AiDetails 'scenarioCount' `
            "$context AI receipt details") -eq 2 -and
        [int](Get-Stage5JsonValue $AiDetails 'distinctSeedCount' `
            "$context AI receipt details") -eq 3 -and
        [int](Get-Stage5JsonValue $AiDetails 'repeatCount' `
            "$context AI receipt details") -eq 2 -and
        [bool](Get-Stage5JsonValue $AiDetails 'allGamesCompleted' `
            "$context AI receipt details")) `
        "$context AI matrix, shadow execution, or receipt cardinality is not exact."
    $aiMatrixTotal = $workers.Count * $aiScenarios.Count * $aiSeeds.Count * 2
    & $writeReadinessDiagnostic 'ai-matrix' 'start' 0 $aiMatrixTotal
    $aiMatrixIndex = 0
    foreach ($worker in $workers) {
        foreach ($scenario in $aiScenarios) {
            foreach ($seed in $aiSeeds) {
                foreach ($repeat in 1..2) {
                    ++$aiMatrixIndex
                    if ($aiMatrixIndex -eq 1 -or ($aiMatrixIndex % 32) -eq 0 -or
                        $aiMatrixIndex -eq $aiMatrixTotal) {
                        & $writeReadinessDiagnostic 'ai-matrix' 'progress' `
                            $aiMatrixIndex $aiMatrixTotal
                    }
                    Assert-Stage5Condition (@($regularAi | Where-Object {
                        $_.configuration -ceq $worker -and
                        $_.scenario -ceq $scenario -and
                        [int]$_.seed -eq [int]$seed -and
                        [int]$_.repeat -eq $repeat
                    }).Count -eq 1) `
                        "$context AI matrix has a missing or duplicate '$scenario' seed $seed repeat $repeat for '$worker'."
                }
            }
        }
    }
    & $writeReadinessDiagnostic 'ai-matrix' 'complete' `
        $aiMatrixIndex $aiMatrixTotal

    Assert-Stage5Condition ($ValidatedRawLogs.Count -eq 507) `
        "$context validation receipt must retain exactly one result file and 253 stdout/stderr pairs."
    $stdoutLeaves = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $stderrLeaves = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    & $writeReadinessDiagnostic 'stream-parsing-reparse' 'start' 0 253
    $streamParseIndex = 0
    foreach ($sequence in 1..253) {
        ++$streamParseIndex
        if ($streamParseIndex -eq 1 -or ($streamParseIndex % 32) -eq 0 -or
            $streamParseIndex -eq 169 -or $streamParseIndex -eq 170 -or
            $streamParseIndex -eq 253) {
            & $writeReadinessDiagnostic 'stream-parsing-reparse' 'progress' `
                $streamParseIndex 253
        }
        $entry = $planBySequence[$sequence]
        $result = $resultsBySequence[$sequence]
        $stdoutPathText = Get-Stage5JsonValue $entry 'stdout' `
            "$context plan entry $sequence"
        $stderrPathText = Get-Stage5JsonValue $entry 'stderr' `
            "$context plan entry $sequence"
        Assert-Stage5Condition ($stdoutPathText -is [string] -and
            $stderrPathText -is [string]) `
            "$context plan entry $sequence stream paths must be JSON strings."
        $stdoutLeaf = [IO.Path]::GetFileName($stdoutPathText)
        $stderrLeaf = [IO.Path]::GetFileName($stderrPathText)
        Assert-Stage5Condition ($stdoutLeaves.Add($stdoutLeaf) -and
            $stderrLeaves.Add($stderrLeaf) -and $stdoutLeaf -cne $stderrLeaf) `
            "$context plan aliases or duplicates child stream leaf names."
        $streamEvidence = Assert-Stage5DevelopmentReadinessExecutionStreamEvidence `
            -Entry $entry -Result $result -ValidatedRawLogs $ValidatedRawLogs `
            -Context "$context result $sequence"
        $stdout = $streamEvidence.stdout
        $stderr = $streamEvidence.stderr
        $stdoutText = $streamEvidence.stdoutText
        if ($entry.kind -ceq 'replay') {
            $verifiedMetrics = ConvertFrom-Stage5ReplayMetrics $stdoutText $entry
            $verifiedResult = ConvertFrom-Stage5ReplayResult $stdoutText $entry
            Assert-Stage5DevelopmentReadinessSemanticEqual $result.replayMetrics `
                $verifiedMetrics "$context replay result $sequence metrics"
            Assert-Stage5DevelopmentReadinessSemanticEqual $result.replayResult `
                $verifiedResult "$context replay result $sequence outcome"
            Assert-Stage5Condition ($null -eq $result.aiEvidence) `
                "$context replay result $sequence contains substituted AI evidence."
        }
        else {
            $verifiedAi = ConvertFrom-Stage5AiCompletion $stdoutText $entry `
                $executableSha256 $true -ValidationPlan $ValidationPlan
            Assert-Stage5DevelopmentReadinessSemanticEqual $result.aiEvidence `
                $verifiedAi "$context AI result $sequence outcome"
            Assert-Stage5Condition ($null -eq $result.replayMetrics -and
                $null -eq $result.replayResult) `
                "$context AI result $sequence contains substituted replay evidence."
        }
    }
    & $writeReadinessDiagnostic 'stream-parsing-reparse' 'complete' `
        $streamParseIndex 253

    & $writeReadinessDiagnostic 'replay-determinism' 'start'
    Assert-Stage5ReplayDeterminism $Results
    & $writeReadinessDiagnostic 'replay-determinism' 'complete'
    $determinismKeys = @(
        foreach ($scenario in $aiScenarios) {
            foreach ($seed in $aiSeeds) { "$scenario-seed-$seed" }
        }
    )
    & $writeReadinessDiagnostic 'ai-determinism' 'start'
    Assert-Stage5AiDeterminism $Results $workers 2 'shadow-16' $determinismKeys
    & $writeReadinessDiagnostic 'ai-determinism' 'complete'
    & $writeReadinessDiagnostic 'authoritative-work-evidence' 'start'
    Assert-Stage5AuthoritativeWorkEvidence -Results $Results `
        -ValidationPlan $ValidationPlan
    & $writeReadinessDiagnostic 'authoritative-work-evidence' 'complete'
    & $writeReadinessDiagnostic 'result-tree-hashes' 'start'
    $replayTree = Get-Stage5DevelopmentReadinessResultTreeSha256 $Results 'replay'
    $aiTree = Get-Stage5DevelopmentReadinessResultTreeSha256 $Results 'ai'
    & $writeReadinessDiagnostic 'result-tree-hashes' 'complete'
    $replayDetailsTree = Get-Stage5JsonValue $ReplayDetails 'crcTreeSha256' `
        "$context replay receipt details"
    $aiDetailsTree = Get-Stage5JsonValue $AiDetails 'digestTreeSha256' `
        "$context AI receipt details"
    Assert-Stage5Condition ($replayDetailsTree -is [string] -and
        $aiDetailsTree -is [string] -and
        $replayDetailsTree -cmatch '^[0-9A-Fa-f]{64}$' -and
        $aiDetailsTree -cmatch '^[0-9A-Fa-f]{64}$' -and
        $replayTree -ceq $replayDetailsTree.ToUpperInvariant() -and
        $aiTree -ceq $aiDetailsTree.ToUpperInvariant()) `
        "$context replay or AI receipt tree is detached from the independently verified outcomes."
    return [pscustomobject]@{
        resultCount = 253; replayCount = 168; aiCount = 85
        workerConfigurations = $workers; replayTreeSha256 = $replayTree
        aiTreeSha256 = $aiTree; aiSeeds = @($aiSeeds); aiScenarios = @($aiScenarios)
        aiRepeats = 2; matrixPasses = 2; stressExecutionsPerConfiguration = 6
        artifactRelocation = $artifactRelocation
        qualificationData = $ExpectedQualificationData
    }
}

function Assert-Stage5FinalAcceptanceDetails {
    param([string]$Kind, [object]$Details, [string]$SourceCommit,
        [Collections.IDictionary]$EvidenceHashes,
        [string]$MixedNativeEvidenceSha256 = $null,
        [object]$MixedNativeEvidence = $null,
        [object]$ScalingProof = $null)
    $workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
        'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
    switch ($Kind) {
        'deterministic-runtime' {
            $names = @('gateName', 'isolatedPipelineMode', 'simulationModes',
                'workerConfigurations', 'isolatedMatrixPassed',
                'finalAcceptanceClaim', 'replayEvidenceSha256',
                'freshAiEvidenceSha256', 'performanceEvidenceSha256',
                'installedKernelExecution')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'gateName' "$Kind details") `
                -ceq 'deterministic-runtime') 'The runtime evidence must name the deterministic-runtime gate.'
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'isolatedPipelineMode' "$Kind details") `
                -ceq 'serial') 'The deterministic-runtime matrix must isolate Stage 5 with the serial Stage 4 pipeline.'
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'simulationModes' "$Kind details") `
                @('serial', 'parallel', 'shadow') "$Kind simulationModes"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            Assert-Stage5FinalAcceptanceBoolean `
                (Get-Stage5JsonValue $Details 'isolatedMatrixPassed' "$Kind details") `
                "$Kind isolatedMatrixPassed"
            $claim = Get-Stage5JsonValue $Details 'finalAcceptanceClaim' "$Kind details"
            Assert-Stage5Condition ($claim -is [bool] -and -not [bool]$claim) `
                'The deterministic-runtime gate must not claim final Stage 5 acceptance.'
            $installedKernel = Get-Stage5JsonValue $Details `
                'installedKernelExecution' "$Kind details"
            Assert-Stage5JsonShape $installedKernel @('status', 'claim',
                'reason', 'sha256') "$Kind installedKernelExecution"
            $installedStatus = Get-Stage5JsonValue $installedKernel 'status' `
                "$Kind installedKernelExecution"
            $installedClaim = Get-Stage5JsonValue $installedKernel 'claim' `
                "$Kind installedKernelExecution"
            $installedReason = Get-Stage5JsonValue $installedKernel 'reason' `
                "$Kind installedKernelExecution"
            $installedHash = Get-Stage5JsonValue $installedKernel 'sha256' `
                "$Kind installedKernelExecution"
            $installedPassed = $installedStatus -ceq 'passed' -and
                $installedClaim -is [bool] -and [bool]$installedClaim -and
                $null -eq $installedReason -and $installedHash -is [string] -and
                $installedHash -cmatch '^[0-9A-F]{64}$'
            $installedSkipped = $installedStatus -ceq 'skipped' -and
                $installedClaim -is [bool] -and -not [bool]$installedClaim -and
                $installedReason -is [string] -and
                $installedReason -ceq
                    'external-qualification-exempt-and-reviewed-native-fixture-unavailable' -and
                $null -eq $installedHash
            Assert-Stage5Condition ($installedPassed -or $installedSkipped) `
                "$Kind installedKernelExecution is neither an authenticated passed proof nor the exact explicit exemption disposition."
            foreach ($binding in @(
                @('replayEvidenceSha256', 'replay-determinism'),
                @('freshAiEvidenceSha256', 'fresh-ai'),
                @('performanceEvidenceSha256', 'performance-scaling')
            )) {
                $value = Get-Stage5JsonValue $Details $binding[0] "$Kind details"
                Assert-Stage5Condition ($value -is [string] -and
                    $value.ToUpperInvariant() -ceq [string]$EvidenceHashes[$binding[1]]) `
                    "$Kind $($binding[0]) does not bind the independently hashed $($binding[1]) evidence."
            }
        }
        'replay-determinism' {
            $names = @('uniqueReplayCount', 'executionCount', 'matrixPasses',
                'stressExecutionsPerConfiguration', 'workerConfigurations',
                'allExecutionsPassed', 'deterministicAcrossWorkers')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            foreach ($metric in @(
                @('uniqueReplayCount', 10), @('executionCount', 168),
                @('matrixPasses', 2), @('stressExecutionsPerConfiguration', 6)
            )) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -eq [Int64]$metric[1]) `
                    "$Kind $($metric[0]) must equal the exact canonical value $($metric[1])."
            }
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            foreach ($name in @('allExecutionsPassed', 'deterministicAcrossWorkers')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'fresh-ai' {
            $names = @('scenarios', 'distinctSeeds', 'repeats', 'workerConfigurations',
                'freshGames', 'allGamesCompleted', 'deterministicAcrossWorkers')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'scenarios' "$Kind details") `
                @('4v3', '4v2') "$Kind scenarios"
            foreach ($metric in @(@('distinctSeeds', 3), @('repeats', 2))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -eq [Int64]$metric[1]) `
                    "$Kind $($metric[0]) must equal the exact canonical value $($metric[1])."
            }
            Assert-Stage5FinalAcceptanceStringSet `
                (Get-Stage5JsonValue $Details 'workerConfigurations' "$Kind details") `
                $workerConfigurations "$Kind workerConfigurations"
            foreach ($name in @('freshGames', 'allGamesCompleted', 'deterministicAcrossWorkers')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        'performance-scaling' {
            $names = @('physicalCoreCount', 'oneWorkerRegressionRatio',
                'eightWorkerSpeedup', 'sixteenWorkerStatus', 'eightToSixteenSpeedup')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            $cores = Get-Stage5JsonValue $Details 'physicalCoreCount' "$Kind details"
            $regression = Get-Stage5JsonValue $Details 'oneWorkerRegressionRatio' "$Kind details"
            $speedup = Get-Stage5JsonValue $Details 'eightWorkerSpeedup' "$Kind details"
            Assert-Stage5Condition ((Test-Stage5JsonInteger $cores) -and [Int64]$cores -ge 8) `
                'Performance evidence requires at least eight physical cores.'
            Assert-Stage5Condition ((Test-Stage5JsonNumber $regression) -and
                [double]$regression -gt 0 -and [double]$regression -le 1.05) `
                'Performance evidence exceeds the 5 percent one-worker regression limit.'
            Assert-Stage5Condition ((Test-Stage5JsonNumber $speedup) -and
                [double]$speedup -ge 2.0) `
                'Performance evidence is below the required 2.0x eight-worker speedup.'
            $sixteenStatus = Get-Stage5JsonValue $Details 'sixteenWorkerStatus' "$Kind details"
            $sixteenSpeedup = Get-Stage5JsonValue $Details 'eightToSixteenSpeedup' "$Kind details"
            if ([Int64]$cores -ge 16) {
                Assert-Stage5Condition ($sixteenStatus -is [string] -and
                    $sixteenStatus -ceq 'passed' -and
                    (Test-Stage5JsonNumber $sixteenSpeedup) -and [double]$sixteenSpeedup -gt 1.0) `
                    'A supported 16-core host requires positive scaling from eight to sixteen workers.'
            }
            else {
                Assert-Stage5Condition ($sixteenStatus -is [string] -and
                    $sixteenStatus -ceq 'unsupported-host-topology' -and $null -eq $sixteenSpeedup) `
                    'A host below 16 physical cores must report sixteen-worker evidence as unsupported-host-topology.'
            }
            Assert-Stage5Condition ($null -ne $ScalingProof) `
                'Performance details require the independently regenerated scaling proof.'
            $expectedSixteenStatus = if ([int]$ScalingProof.physicalCoreCount -ge 16) {
                'passed'
            }
            else { 'unsupported-host-topology' }
            $sixteenBindingMatches = if ([int]$ScalingProof.physicalCoreCount -ge 16) {
                (Test-Stage5JsonNumber $sixteenSpeedup) -and
                    [Math]::Abs([double]$sixteenSpeedup -
                        [double]$ScalingProof.minimumEightToSixteenSpeedup) -le 1.0e-12
            }
            else { $null -eq $sixteenSpeedup }
            Assert-Stage5Condition ([int]$cores -eq
                    [int]$ScalingProof.physicalCoreCount -and
                [Math]::Abs([double]$regression -
                    [double]$ScalingProof.maximumOneWorkerRegressionRatio) -le 1.0e-12 -and
                [Math]::Abs([double]$speedup -
                    [double]$ScalingProof.minimumEightWorkerSpeedup) -le 1.0e-12 -and
                [string]$sixteenStatus -ceq $expectedSixteenStatus -and
                $sixteenBindingMatches) `
                'Performance summary details are detached from the regenerated scaling extrema.'
        }
        'mixed-worker-multiplayer' {
            # The NET3 diagnostic receipt used matchRecords=16, peerRecords=40,
            # two fixed seeds, and a single provenKernelMask.  Those counts are
            # not the lockstep-v2 contract and must never be accepted as a
            # substitute for the installed lockstep producer.  The v2 envelope
            # binds its native evidence by content hash and states the exact
            # roster/session topology instead.
            $names = @('nativeEvidenceKind', 'producer', 'nativeEvidenceSha256',
                'networkRosterMask', 'simulationRosterMask', 'aiRosterMask',
                'aiPlayerCount', 'title', 'sessionCount', 'peerCount',
                'commonStopFrame', 'allMatchesCompleted', 'stateTracesIdentical',
                'crossEpochRejected', 'contentMismatchRejected')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'nativeEvidenceKind' "$Kind details") -ceq
                'lockstep-v2-multiplayer' -and
                (Get-Stage5JsonValue $Details 'producer' "$Kind details") -ceq
                'installed-lockstep-v2') `
                "$Kind details are not bound to the installed lockstep-v2 native producer."
            $nativeHash = Get-Stage5JsonValue $Details 'nativeEvidenceSha256' "$Kind details"
            Assert-Stage5Condition ($nativeHash -is [string] -and
                $nativeHash -match '^[0-9A-Fa-f]{64}$') `
                "$Kind nativeEvidenceSha256 is not a canonical SHA-256."
            foreach ($metric in @(
                @('networkRosterMask', 3), @('simulationRosterMask', 63),
                @('aiRosterMask', 60), @('aiPlayerCount', 4),
                @('sessionCount', 2), @('peerCount', 2),
                @('commonStopFrame', 4096))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -eq [Int64]$metric[1]) `
                    "Mixed-worker multiplayer $($metric[0]) must equal $($metric[1]) in the lockstep-v2 contract."
            }
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'title' "$Kind details") -ceq 'Both') `
                "$Kind details must cover both title sessions."
            foreach ($name in @('allMatchesCompleted', 'stateTracesIdentical',
                'crossEpochRejected', 'contentMismatchRejected')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
            if ($null -ne $MixedNativeEvidence) {
                Assert-Stage5Condition ([bool]$MixedNativeEvidence.crossEpochRejected -and
                    [bool]$MixedNativeEvidence.contentMismatchRejected) `
                    "$Kind native lockstep evidence did not prove both observed negative probes."
                Assert-Stage5Condition ([bool](Get-Stage5JsonValue $Details 'crossEpochRejected' "$Kind details") -eq
                    [bool]$MixedNativeEvidence.crossEpochRejected -and
                    [bool](Get-Stage5JsonValue $Details 'contentMismatchRejected' "$Kind details") -eq
                    [bool]$MixedNativeEvidence.contentMismatchRejected) `
                    "$Kind negative-probe summary is detached from the native lockstep evidence."
            }
            $nativeBindingHash = $MixedNativeEvidenceSha256
            if ([string]::IsNullOrWhiteSpace($nativeBindingHash) -and
                $null -ne $EvidenceHashes -and
                $EvidenceHashes.Contains('mixed-worker-multiplayer-native')) {
                $nativeBindingHash = [string]$EvidenceHashes['mixed-worker-multiplayer-native']
            }
            if (-not [string]::IsNullOrWhiteSpace($nativeBindingHash)) {
                Assert-Stage5Condition ($nativeHash.ToUpperInvariant() -ceq
                    $nativeBindingHash.ToUpperInvariant()) `
                    "$Kind nativeEvidenceSha256 does not bind the independently hashed lockstep-v2 native evidence."
            }
        }
        'combined-stage4-stage5-installed-runtime' {
            $names = @('installedRuntime', 'pipelineMode', 'simulationMode',
                'requestedWorkers', 'workerPolicy', 'projectionSequence',
                'projectionSemantics', 'sourceChildCount', 'bothTitlesPassed')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            foreach ($pair in @(
                @('pipelineMode', 'serial'), @('simulationMode', 'serial'),
                @('requestedWorkers', '1'), @('workerPolicy', 'auto'),
                @('projectionSemantics', 'deterministic-lineage-pointer')
            )) {
                Assert-Stage5Condition ((Get-Stage5JsonValue $Details $pair[0] "$Kind details") `
                    -ceq $pair[1]) "$Kind requires $($pair[0])=$($pair[1])."
            }
            foreach ($name in @('installedRuntime', 'bothTitlesPassed')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
            foreach ($integerBinding in @(@('projectionSequence', 1),
                    @('sourceChildCount', 253))) {
                $value = Get-Stage5JsonValue $Details $integerBinding[0] `
                    "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [int]$value -eq [int]$integerBinding[1]) `
                    "$Kind requires $($integerBinding[0])=$($integerBinding[1])."
            }
        }
        'premium-review' {
            $names = @('reviewedCommit', 'reviewRounds', 'independentReviewers',
                'completeDiffReviewed', 'fixesRetested', 'openP0', 'openP1', 'openP2')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'reviewedCommit' "$Kind details") `
                -ceq $SourceCommit) 'Premium review evidence identifies a different commit.'
            foreach ($metric in @(@('reviewRounds', 1), @('independentReviewers', 3))) {
                $value = Get-Stage5JsonValue $Details $metric[0] "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and
                    [Int64]$value -ge [Int64]$metric[1]) `
                    "$Kind $($metric[0]) is below the required minimum $($metric[1])."
            }
            foreach ($name in @('completeDiffReviewed', 'fixesRetested')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
            foreach ($name in @('openP0', 'openP1', 'openP2')) {
                $value = Get-Stage5JsonValue $Details $name "$Kind details"
                Assert-Stage5Condition ((Test-Stage5JsonInteger $value) -and [Int64]$value -eq 0) `
                    "$Kind $name must be zero."
            }
        }
        'manual-acceptance' {
            $names = @('approvalScope', 'approvedByUser', 'candidateHashVerified',
                'bothTitlesTested', 'graphicsPassed', 'audioPassed', 'inputPassed',
                'saveLoadPassed', 'largeMatchPassed', 'cleanExitPassed')
            Assert-Stage5JsonShape $Details $names "$Kind details"
            Assert-Stage5Condition ((Get-Stage5JsonValue $Details 'approvalScope' "$Kind details") `
                -ceq 'final-stage5-installed-runtime') `
                'Manual evidence must cover the final Stage 5 installed runtime.'
            foreach ($name in @('approvedByUser', 'candidateHashVerified',
                'bothTitlesTested', 'graphicsPassed', 'audioPassed', 'inputPassed',
                'saveLoadPassed', 'largeMatchPassed', 'cleanExitPassed')) {
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $Details $name "$Kind details") "$Kind $name"
            }
        }
        default { throw "Unsupported final acceptance evidence kind '$Kind'." }
    }
}

function Read-Stage5Net3LoopbackEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedGeneralsExecutableSha256,
        [string]$ExpectedZeroHourExecutableSha256,
        [UInt32]$ExpectedGeneralsBuildCompatibilityCrc = 0,
        [UInt32]$ExpectedZeroHourBuildCompatibilityCrc = 0,
        [UInt32]$ExpectedGeneralsContentCrc = 0,
        [UInt32]$ExpectedZeroHourContentCrc = 0
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Installed NET3 loopback evidence was not found: $full"
    Assert-Stage5Condition ($ExpectedSourceCommit -match '^[0-9a-f]{40}$') `
        'ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    foreach ($binding in @(
        @('ExpectedArtifactSetSha256', $ExpectedArtifactSetSha256),
        @('ExpectedGeneralsExecutableSha256', $ExpectedGeneralsExecutableSha256),
        @('ExpectedZeroHourExecutableSha256', $ExpectedZeroHourExecutableSha256)
    )) {
        Assert-Stage5Condition ($binding[1] -match '^[0-9A-F]{64}$') `
            "$($binding[0]) must be an independently supplied uppercase SHA-256."
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    $documentNames = @('schemaVersion', 'evidenceKind', 'status', 'producer',
        'validationMode', 'installedRuntime', 'independentProcessHashing',
        'sourceCommit', 'artifactSetSha256', 'supportedKernelMask',
        'policySchema', 'engineEpoch', 'determinismEpoch',
        'buildCompatibilityCrc', 'contentCrc', 'executables', 'fixedSeeds', 'matches')
    Assert-Stage5JsonShape $document $documentNames 'Installed NET3 loopback evidence'
    $schemaVersion = Get-Stage5JsonValue $document 'schemaVersion' 'Installed NET3 loopback evidence'
    $kind = Get-Stage5JsonValue $document 'evidenceKind' 'Installed NET3 loopback evidence'
    $status = Get-Stage5JsonValue $document 'status' 'Installed NET3 loopback evidence'
    $producer = Get-Stage5JsonValue $document 'producer' 'Installed NET3 loopback evidence'
    $validationMode = Get-Stage5JsonValue $document 'validationMode' 'Installed NET3 loopback evidence'
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' 'Installed NET3 loopback evidence'
    $artifactHash = Get-Stage5JsonValue $document 'artifactSetSha256' 'Installed NET3 loopback evidence'
    $kernelMask = Get-Stage5JsonValue $document 'supportedKernelMask' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $kind -is [string] -and $kind -ceq 'installed-net3-loopback' -and
        $status -is [string] -and $status -ceq 'passed' -and
        $producer -is [string] -and $producer -ceq 'installed-runtime-runner-v1' -and
        $validationMode -is [string] -and
        $validationMode -ceq 'scoped-net3-loopback-release-proof') `
        'Installed NET3 loopback evidence identity/status is invalid.'
    Assert-Stage5FinalAcceptanceBoolean `
        (Get-Stage5JsonValue $document 'installedRuntime' 'Installed NET3 loopback evidence') `
        'Installed NET3 loopback evidence installedRuntime'
    Assert-Stage5FinalAcceptanceBoolean `
        (Get-Stage5JsonValue $document 'independentProcessHashing' 'Installed NET3 loopback evidence') `
        'Installed NET3 loopback evidence independentProcessHashing'
    Assert-Stage5Condition ($sourceCommit -is [string] -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        'Installed NET3 loopback evidence source commit does not match independent provenance.'
    Assert-Stage5Condition ($artifactHash -is [string] -and
        $artifactHash -ceq $ExpectedArtifactSetSha256) `
        'Installed NET3 loopback evidence artifact-set SHA-256 does not match independent provenance.'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $kernelMask) -and [UInt64]$kernelMask -eq 0x3F) `
        'Installed NET3 loopback evidence must advertise exactly the integrated kernel mask 0x3F.'

    foreach ($epochBinding in @(
        @('policySchema', 1), @('engineEpoch', 1), @('determinismEpoch', 1)
    )) {
        $epochValue = Get-Stage5JsonValue $document $epochBinding[0] `
            'Installed NET3 loopback evidence'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $epochValue) -and
            [UInt64]$epochValue -eq [UInt64]$epochBinding[1]) `
            "Installed NET3 loopback evidence $($epochBinding[0]) is incompatible."
    }

    $buildCrcs = Get-Stage5JsonValue $document 'buildCompatibilityCrc' `
        'Installed NET3 loopback evidence'
    $contentCrcs = Get-Stage5JsonValue $document 'contentCrc' `
        'Installed NET3 loopback evidence'
    Assert-Stage5JsonShape $buildCrcs @('Generals', 'ZeroHour') `
        'Installed NET3 loopback build compatibility CRCs'
    Assert-Stage5JsonShape $contentCrcs @('Generals', 'ZeroHour') `
        'Installed NET3 loopback content CRCs'
    $validatedBuildCrcs = @{}
    $validatedContentCrcs = @{}
    $expectedBuildCrcs = @{
        Generals = $ExpectedGeneralsBuildCompatibilityCrc
        ZeroHour = $ExpectedZeroHourBuildCompatibilityCrc
    }
    $expectedContentCrcs = @{
        Generals = $ExpectedGeneralsContentCrc
        ZeroHour = $ExpectedZeroHourContentCrc
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        $buildCrc = Get-Stage5JsonValue $buildCrcs $title `
            'Installed NET3 loopback build compatibility CRCs'
        $contentCrc = Get-Stage5JsonValue $contentCrcs $title `
            'Installed NET3 loopback content CRCs'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $buildCrc) -and
            [UInt64]$buildCrc -gt 0 -and [UInt64]$buildCrc -le [UInt32]::MaxValue -and
            (Test-Stage5JsonInteger $contentCrc) -and
            [UInt64]$contentCrc -gt 0 -and [UInt64]$contentCrc -le [UInt32]::MaxValue) `
            "Installed NET3 loopback evidence has invalid build/content CRC for $title."
        if ($expectedBuildCrcs[$title] -ne 0) {
            Assert-Stage5Condition ([UInt32]$buildCrc -eq $expectedBuildCrcs[$title]) `
                "Installed NET3 loopback build CRC for $title does not match independent provenance."
        }
        if ($expectedContentCrcs[$title] -ne 0) {
            Assert-Stage5Condition ([UInt32]$contentCrc -eq $expectedContentCrcs[$title]) `
                "Installed NET3 loopback content CRC for $title does not match independent provenance."
        }
        $validatedBuildCrcs[$title] = [UInt32]$buildCrc
        $validatedContentCrcs[$title] = [UInt32]$contentCrc
    }

    $executables = Get-Stage5JsonValue $document 'executables' 'Installed NET3 loopback evidence'
    Assert-Stage5JsonShape $executables @('Generals', 'ZeroHour') `
        'Installed NET3 loopback evidence executables'
    $expectedExecutableHashes = @{
        Generals = $ExpectedGeneralsExecutableSha256
        ZeroHour = $ExpectedZeroHourExecutableSha256
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        $hash = Get-Stage5JsonValue $executables $title `
            'Installed NET3 loopback evidence executables'
        Assert-Stage5Condition ($hash -is [string] -and
            $hash -ceq $expectedExecutableHashes[$title]) `
            "Installed NET3 loopback evidence executable hash for $title does not match independent provenance."
    }

    $fixedSeeds = Get-Stage5JsonValue $document 'fixedSeeds' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ($fixedSeeds -is [Array] -and $fixedSeeds.Count -eq 2 -and
        (Test-Stage5JsonInteger $fixedSeeds[0]) -and (Test-Stage5JsonInteger $fixedSeeds[1]) -and
        [UInt64]$fixedSeeds[0] -eq 23063 -and [UInt64]$fixedSeeds[1] -eq 49374) `
        'Installed NET3 loopback evidence requires the exact fixed nonzero seeds 23063 and 49374 in canonical order.'

    $topologies = @(
        [pscustomobject]@{ id = 'two-peer-1-v-16'; workers = @('1', '16') },
        [pscustomobject]@{ id = 'two-peer-2-v-auto'; workers = @('2', 'auto') },
        [pscustomobject]@{ id = 'two-peer-4-v-8'; workers = @('4', '8') },
        [pscustomobject]@{ id = 'four-peer-mixed-workers'; workers = @('1', '2', '8', 'auto') }
    )
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelBits = @(1, 2, 4, 8, 16, 32)
    # Do not name this variable $matches: PowerShell's case-insensitive
    # automatic $Matches table is rewritten by every later -match expression.
    $matchRecords = Get-Stage5JsonValue $document 'matches' 'Installed NET3 loopback evidence'
    Assert-Stage5Condition ($matchRecords -is [Array] -and $matchRecords.Count -eq 16) `
        'Installed NET3 loopback evidence requires exactly 16 match records.'
    $matchIndex = 0
    $peerRecordCount = 0
    $observedRawOutputPaths = @{}
	$validatedRawOutputs = @()
	[UInt64]$provenKernelMask = 0
    $topologyCaseIndices = @{
        'two-peer-1-v-16' = 0
        'two-peer-2-v-auto' = 1
        'two-peer-4-v-8' = 2
        'four-peer-mixed-workers' = 3
    }
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($topology in $topologies) {
            foreach ($seed in @(23063, 49374)) {
                $match = $matchRecords[$matchIndex]
                $context = "Installed NET3 loopback match index $matchIndex"
                Assert-Stage5JsonShape $match @('recordId', 'sourceCommit', 'title',
                    'executableSha256', 'artifactSetSha256', 'topologyId', 'seed',
                    'networkHelloReady', 'rosterExact', 'rosterSha256', 'policyMask', 'peers') $context
                $recordId = "$title/$($topology.id)/$seed"
                $matchRecordId = Get-Stage5JsonValue $match 'recordId' $context
                $matchSourceCommit = Get-Stage5JsonValue $match 'sourceCommit' $context
                $matchTitle = Get-Stage5JsonValue $match 'title' $context
                $matchExecutableHash = Get-Stage5JsonValue $match 'executableSha256' $context
                $matchArtifactHash = Get-Stage5JsonValue $match 'artifactSetSha256' $context
                $matchTopologyId = Get-Stage5JsonValue $match 'topologyId' $context
                Assert-Stage5Condition ($matchRecordId -is [string] -and
                    $matchSourceCommit -is [string] -and $matchTitle -is [string] -and
                    $matchExecutableHash -is [string] -and
                    $matchArtifactHash -is [string] -and
                    $matchTopologyId -is [string] -and
                    $matchRecordId -ceq $recordId -and
                    $matchSourceCommit -ceq $ExpectedSourceCommit -and
                    $matchTitle -ceq $title -and
                    $matchExecutableHash -ceq $expectedExecutableHashes[$title] -and
                    $matchArtifactHash -ceq $ExpectedArtifactSetSha256 -and
                    $matchTopologyId -ceq $topology.id -and
                    (Test-Stage5JsonInteger (Get-Stage5JsonValue $match 'seed' $context)) -and
                    (Get-Stage5JsonValue $match 'seed' $context) -eq $seed) `
                    "$context provenance/topology identity is not canonical."
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $match 'networkHelloReady' $context) `
                    "$context networkHelloReady"
                Assert-Stage5FinalAcceptanceBoolean `
                    (Get-Stage5JsonValue $match 'rosterExact' $context) "$context rosterExact"
                $rosterHash = Get-Stage5JsonValue $match 'rosterSha256' $context
                Assert-Stage5Condition ($rosterHash -is [string] -and
                    $rosterHash -match '^[0-9A-F]{64}$') `
                    "$context rosterSha256 is invalid."
                $matchPolicy = Get-Stage5JsonValue $match 'policyMask' $context
                Assert-Stage5Condition ((Test-Stage5JsonInteger $matchPolicy) -and
                    [UInt64]$matchPolicy -eq 0x3F) "$context policyMask must equal 0x3F."
                $peers = Get-Stage5JsonValue $match 'peers' $context
                Assert-Stage5Condition ($peers -is [Array] -and
                    $peers.Count -eq $topology.workers.Count) `
                    "$context does not contain the exact topology peer roster."
                $referenceCRC = $null
                $referenceFrame = $null
                for ($peerIndex = 0; $peerIndex -lt $topology.workers.Count; ++$peerIndex) {
                    $peer = $peers[$peerIndex]
                    $peerContext = "$context peer $peerIndex"
                    Assert-Stage5JsonShape $peer @('ordinal', 'processId',
                        'observedExecutableSha256', 'observedArtifactSetSha256',
                        'rawOutputPath', 'rawOutputSha256', 'requestedWorkers',
                        'effectiveWorkers', 'networkHelloReady', 'rosterExact', 'rosterSha256',
                        'policyMask', 'finalFrame', 'finalCRC', 'exitCode', 'cleanShutdown', 'kernels') `
                        $peerContext
                    $ordinal = Get-Stage5JsonValue $peer 'ordinal' $peerContext
                    $processId = Get-Stage5JsonValue $peer 'processId' $peerContext
                    $observedExecutableHash = Get-Stage5JsonValue $peer `
                        'observedExecutableSha256' $peerContext
                    $observedArtifactHash = Get-Stage5JsonValue $peer `
                        'observedArtifactSetSha256' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $processId) -and
                        [UInt64]$processId -gt 0 -and
                        $observedExecutableHash -is [string] -and
                        $observedExecutableHash -ceq $expectedExecutableHashes[$title] -and
                        $observedArtifactHash -is [string] -and
                        $observedArtifactHash -ceq $ExpectedArtifactSetSha256) `
                        "$peerContext lacks an independent exact process/artifact hash observation."
                    $rawOutputPath = Get-Stage5JsonValue $peer 'rawOutputPath' $peerContext
                    $rawOutputSha = Get-Stage5JsonValue $peer 'rawOutputSha256' $peerContext
                    Assert-Stage5Condition ($rawOutputPath -is [string] -and
                        $rawOutputSha -is [string] -and
                        $rawOutputSha -match '^[0-9A-Fa-f]{64}$' -and
                        $rawOutputPath.StartsWith('Net3Raw\', [StringComparison]::Ordinal) -and
                        -not [IO.Path]::IsPathRooted($rawOutputPath) -and
                        -not $rawOutputPath.Contains(':') -and
                        -not $rawOutputPath.Contains('..') -and
                        -not $observedRawOutputPaths.ContainsKey($rawOutputPath)) `
                        "$peerContext raw output path is unsafe, duplicated, or outside Net3Raw."
                    $observedRawOutputPaths[$rawOutputPath] = $true
                    $rawOutputFull = Resolve-Stage5FinalAcceptanceFile `
                        (Split-Path -Parent $full) $rawOutputPath "$peerContext raw output"
                    Assert-Stage5FinalAcceptanceSha256 $rawOutputFull $rawOutputSha `
                        "$peerContext raw output" | Out-Null
                    $validatedRawOutputs += [pscustomobject]@{
                        path = $rawOutputPath
                        sha256 = $rawOutputSha.ToUpperInvariant()
                    }
                    $rawRecord = ConvertFrom-Stage5JsonDictionary $rawOutputFull
                    Assert-Stage5JsonShape $rawRecord @('schemaVersion', 'producer',
                        'validationMode', 'kernelFixture', 'processId', 'title', 'caseIndex', 'seed',
                        'ordinal', 'peerCount', 'sourceCommit', 'executableSha256',
                        'artifactSetSha256', 'buildCompatibilityCrc', 'contentCrc',
                        'requestedWorkers', 'effectiveWorkers', 'networkHelloReady',
                        'rosterExact', 'rosterSha256', 'policyMask', 'finalFrame',
                        'finalCRC', 'cleanShutdown', 'kernels') "$peerContext raw output"
                    $requestedWorkers = Get-Stage5JsonValue $peer 'requestedWorkers' $peerContext
                    $effectiveWorkers = Get-Stage5JsonValue $peer 'effectiveWorkers' $peerContext
                    $rawProducer = Get-Stage5JsonValue $rawRecord 'producer' "$peerContext raw output"
                    $rawValidationMode = Get-Stage5JsonValue $rawRecord 'validationMode' `
                        "$peerContext raw output"
                    $rawKernelFixture = Get-Stage5JsonValue $rawRecord 'kernelFixture' `
                        "$peerContext raw output"
                    $rawTitle = Get-Stage5JsonValue $rawRecord 'title' "$peerContext raw output"
                    $rawSourceCommit = Get-Stage5JsonValue $rawRecord 'sourceCommit' `
                        "$peerContext raw output"
                    $rawExecutableHash = Get-Stage5JsonValue $rawRecord 'executableSha256' `
                        "$peerContext raw output"
                    $rawArtifactHash = Get-Stage5JsonValue $rawRecord 'artifactSetSha256' `
                        "$peerContext raw output"
                    $rawRequestedWorkers = Get-Stage5JsonValue $rawRecord 'requestedWorkers' `
                        "$peerContext raw output"
                    $rawRosterHash = Get-Stage5JsonValue $rawRecord 'rosterSha256' `
                        "$peerContext raw output"
                    $rawFinalCrc = Get-Stage5JsonValue $rawRecord 'finalCRC' `
                        "$peerContext raw output"
                    Assert-Stage5Condition ($rawProducer -is [string] -and
                        $rawValidationMode -is [string] -and
                        $rawKernelFixture -is [string] -and $rawTitle -is [string] -and
                        $rawSourceCommit -is [string] -and
                        $rawExecutableHash -is [string] -and
                        $rawArtifactHash -is [string] -and
                        $rawRequestedWorkers -is [string] -and
                        $rawRosterHash -is [string] -and $rawFinalCrc -is [string]) `
                        "$peerContext raw output identity fields must be JSON strings."
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $ordinal) -and
                        [int]$ordinal -eq $peerIndex -and $requestedWorkers -is [string] -and
                        $requestedWorkers -ceq $topology.workers[$peerIndex] -and
                        (Test-Stage5JsonInteger $effectiveWorkers) -and [int]$effectiveWorkers -ge 1) `
                        "$peerContext worker identity does not match the topology."
                    if ($requestedWorkers -ceq 'auto') {
                        Assert-Stage5Condition ([int]$effectiveWorkers -gt 1) `
                            "$peerContext automatic workers did not expose a multicore lane."
                    }
                    else {
                        Assert-Stage5Condition ([int]$effectiveWorkers -eq [int]$requestedWorkers) `
                            "$peerContext effective worker count differs from the forced count."
                    }
                    foreach ($name in @('networkHelloReady', 'rosterExact', 'cleanShutdown')) {
                        Assert-Stage5FinalAcceptanceBoolean `
                            (Get-Stage5JsonValue $peer $name $peerContext) "$peerContext $name"
                    }
                    Assert-Stage5Condition ((Get-Stage5JsonValue $peer 'rosterSha256' $peerContext) `
                        -ceq $rosterHash) "$peerContext roster SHA-256 differs from the exact match roster."
                    $peerPolicy = Get-Stage5JsonValue $peer 'policyMask' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $peerPolicy) -and
                        [UInt64]$peerPolicy -eq 0x3F) "$peerContext policyMask must equal 0x3F."
                    $exitCode = Get-Stage5JsonValue $peer 'exitCode' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $exitCode) -and
                        [Int64]$exitCode -eq 0) "$peerContext did not exit successfully."
                    $finalFrame = Get-Stage5JsonValue $peer 'finalFrame' $peerContext
                    $finalCRC = Get-Stage5JsonValue $peer 'finalCRC' $peerContext
                    Assert-Stage5Condition ((Test-Stage5JsonInteger $finalFrame) -and
                        [UInt64]$finalFrame -gt 0 -and $finalCRC -is [string] -and
                        $finalCRC -match '^[0-9A-F]{8}$') `
                        "$peerContext final frame/CRC evidence is invalid."
                    $rawRecordSchemaVersion = Get-Stage5JsonValue $rawRecord `
                        'schemaVersion' "$peerContext raw output"
                    Assert-Stage5Condition (
                        (Test-Stage5JsonInteger $rawRecordSchemaVersion) -and
                        $rawRecordSchemaVersion -eq 1 -and
                        $rawProducer -ceq 'installed-runtime-net3-peer-v1' -and
                        $rawValidationMode -ceq 'scoped-net3-loopback-release-proof' -and
                        $rawKernelFixture -ceq 'actual-stage5-kernels-v1' -and
                        (Get-Stage5JsonValue $rawRecord 'processId' "$peerContext raw output") -eq $processId -and
                        $rawTitle -ceq $title -and
                        (Get-Stage5JsonValue $rawRecord 'caseIndex' "$peerContext raw output") -eq
                            $topologyCaseIndices[$topology.id] -and
                        (Get-Stage5JsonValue $rawRecord 'seed' "$peerContext raw output") -eq $seed -and
                        (Get-Stage5JsonValue $rawRecord 'ordinal' "$peerContext raw output") -eq $peerIndex -and
                        (Get-Stage5JsonValue $rawRecord 'peerCount' "$peerContext raw output") -eq
                            $topology.workers.Count -and
                        $rawSourceCommit -ceq $ExpectedSourceCommit -and
                        $rawExecutableHash -ceq $expectedExecutableHashes[$title] -and
                        $rawArtifactHash -ceq $ExpectedArtifactSetSha256 -and
                        (Get-Stage5JsonValue $rawRecord 'buildCompatibilityCrc' "$peerContext raw output") -eq
                            $validatedBuildCrcs[$title] -and
                        (Get-Stage5JsonValue $rawRecord 'contentCrc' "$peerContext raw output") -eq
                            $validatedContentCrcs[$title] -and
                        $rawRequestedWorkers -ceq $requestedWorkers -and
                        (Get-Stage5JsonValue $rawRecord 'effectiveWorkers' "$peerContext raw output") -eq
                            $effectiveWorkers -and
                        (Get-Stage5JsonValue $rawRecord 'networkHelloReady' "$peerContext raw output") -eq $true -and
                        (Get-Stage5JsonValue $rawRecord 'rosterExact' "$peerContext raw output") -eq $true -and
                        $rawRosterHash -ceq $rosterHash -and
                        (Get-Stage5JsonValue $rawRecord 'policyMask' "$peerContext raw output") -eq 63 -and
                        (Get-Stage5JsonValue $rawRecord 'finalFrame' "$peerContext raw output") -eq $finalFrame -and
                        $rawFinalCrc -ceq $finalCRC -and
                        (Get-Stage5JsonValue $rawRecord 'cleanShutdown' "$peerContext raw output") -eq $true) `
                        "$peerContext raw peer record does not match the independently observed evidence."
                    if ($null -eq $referenceCRC) {
                        $referenceCRC = $finalCRC
                        $referenceFrame = [UInt64]$finalFrame
                    }
                    else {
                        Assert-Stage5Condition ($finalCRC -ceq $referenceCRC -and
                            [UInt64]$finalFrame -eq $referenceFrame) `
                            "$context peer final CRC/frame values differ."
                    }
                    $kernels = Get-Stage5JsonValue $peer 'kernels' $peerContext
                    $rawKernels = Get-Stage5JsonValue $rawRecord 'kernels' "$peerContext raw output"
                    Assert-Stage5Condition ($kernels -is [Array] -and $kernels.Count -eq 6) `
                        "$peerContext must contain exactly six kernel records."
                    Assert-Stage5Condition ($rawKernels -is [Array] -and $rawKernels.Count -eq 6) `
                        "$peerContext raw output must contain exactly six kernel records."
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $kernel = $kernels[$kernelIndex]
                        $kernelContext = "$peerContext kernel $kernelIndex"
						Assert-Stage5JsonShape $kernel @('name', 'bit', 'submitted', 'completed',
							'physicalWorkerJobs', 'ownerHelpedJobs', 'physicalWorkerMask',
							'distinctPhysicalWorkers', 'physicalWorkerMaskComplete',
							'peakConcurrentPhysicalWorkers') $kernelContext
						$kernelName = Get-Stage5JsonValue $kernel 'name' $kernelContext
						$kernelBit = Get-Stage5JsonValue $kernel 'bit' $kernelContext
						Assert-Stage5Condition ($kernelName -is [string] -and
							(Test-Stage5JsonInteger $kernelBit) -and
							$kernelName -ceq $kernelNames[$kernelIndex] -and
							$kernelBit -eq $kernelBits[$kernelIndex]) `
							"$kernelContext name/bit is not canonical."
                        $submitted = Get-Stage5JsonValue $kernel 'submitted' $kernelContext
                        $completed = Get-Stage5JsonValue $kernel 'completed' $kernelContext
                        $physical = Get-Stage5JsonValue $kernel 'physicalWorkerJobs' $kernelContext
                        $ownerHelped = Get-Stage5JsonValue $kernel 'ownerHelpedJobs' $kernelContext
                        $physicalMask = Get-Stage5JsonValue $kernel 'physicalWorkerMask' $kernelContext
                        $distinct = Get-Stage5JsonValue $kernel 'distinctPhysicalWorkers' $kernelContext
						$maskComplete = Get-Stage5JsonValue $kernel 'physicalWorkerMaskComplete' $kernelContext
						$peak = Get-Stage5JsonValue $kernel 'peakConcurrentPhysicalWorkers' $kernelContext
                        $rawKernel = $rawKernels[$kernelIndex]
                        Assert-Stage5JsonShape $rawKernel @('name', 'bit', 'submitted',
                            'completed', 'physicalWorkerJobs', 'ownerHelpedJobs',
							'physicalWorkerMask', 'distinctPhysicalWorkers',
							'physicalWorkerMaskComplete', 'peakConcurrentPhysicalWorkers') `
                            "$kernelContext raw output"
                        $rawKernelName = Get-Stage5JsonValue $rawKernel 'name' `
                            "$kernelContext raw output"
                        $rawKernelBit = Get-Stage5JsonValue $rawKernel 'bit' `
                            "$kernelContext raw output"
                        Assert-Stage5Condition (
                            $kernelName -is [string] -and $rawKernelName -is [string] -and
                            $rawKernelBit -isnot [Array] -and
                            $kernelBit -isnot [Array] -and
                            $rawKernelName -ceq $kernelName -and
                            (Test-Stage5JsonInteger $rawKernelBit) -and
                            (Test-Stage5JsonInteger $kernelBit) -and
                            $rawKernelBit -eq $kernelBit -and
                            (Get-Stage5JsonValue $rawKernel 'submitted' "$kernelContext raw output") -eq $submitted -and
                            (Get-Stage5JsonValue $rawKernel 'completed' "$kernelContext raw output") -eq $completed -and
                            (Get-Stage5JsonValue $rawKernel 'physicalWorkerJobs' "$kernelContext raw output") -eq $physical -and
                            (Get-Stage5JsonValue $rawKernel 'ownerHelpedJobs' "$kernelContext raw output") -eq $ownerHelped -and
                            (Get-Stage5JsonValue $rawKernel 'physicalWorkerMask' "$kernelContext raw output") -eq $physicalMask -and
							(Get-Stage5JsonValue $rawKernel 'distinctPhysicalWorkers' "$kernelContext raw output") -eq $distinct -and
							(Get-Stage5JsonValue $rawKernel 'physicalWorkerMaskComplete' "$kernelContext raw output") -eq $maskComplete -and
							(Get-Stage5JsonValue $rawKernel 'peakConcurrentPhysicalWorkers' "$kernelContext raw output") -eq $peak) `
                            "$kernelContext raw counters do not match the accepted peer evidence."
                        foreach ($counter in @($submitted, $completed, $physical, $ownerHelped,
                            $physicalMask, $distinct, $peak)) {
                            Assert-Stage5Condition ((Test-Stage5JsonInteger $counter) -and
                                [Int64]$counter -ge 0) "$kernelContext counters must be nonnegative integers."
                        }
                        Assert-Stage5Condition ([UInt64]$submitted -eq [UInt64]$completed) `
                            "$kernelContext submitted/completed jobs differ."
                        Assert-Stage5Condition ([UInt64]$physical + [UInt64]$ownerHelped -eq
                            [UInt64]$completed) `
                            "$kernelContext physical/owner execution accounting differs from completed jobs."
						Assert-Stage5Condition ($maskComplete -is [bool]) `
							"$kernelContext physicalWorkerMaskComplete must be a boolean."
						[UInt64]$kernelMaskBitCount = Get-Stage5UInt64BitCount `
							([UInt64]$physicalMask)
						Assert-Stage5Condition (([UInt64]$physical -eq 0) -eq
							([UInt64]$distinct -eq 0)) `
							"$kernelContext physical-worker jobs and maximum distinct count disagree."
						Assert-Stage5Condition ([UInt64]$effectiveWorkers -ge 64 -or
							(([UInt64]$physicalMask -shr [int]$effectiveWorkers) -eq 0)) `
							"$kernelContext physical-worker mask exceeds the effective worker lane."
						Assert-Stage5Condition ($maskComplete -or
							[UInt64]$effectiveWorkers -gt 64) `
							"$kernelContext physical-worker mask is incomplete inside the representable worker lane."
						if ($maskComplete) {
							Assert-Stage5Condition ([UInt64]$distinct -eq
								$kernelMaskBitCount) `
								"$kernelContext complete physical-worker mask/count is inconsistent."
						}
						Assert-Stage5Condition ($kernelMaskBitCount -le [UInt64]$physical -and
							[UInt64]$distinct -le [UInt64]$physical -and
							[UInt64]$distinct -le [UInt64]$effectiveWorkers -and
							[UInt64]$peak -le [UInt64]$distinct) `
							"$kernelContext reports impossible aggregate physical-worker evidence."
                        if ([int]$effectiveWorkers -eq 1) {
                            Assert-Stage5Condition ([UInt64]$submitted -eq 0 -and
                                [UInt64]$physical -eq 0 -and [UInt64]$ownerHelped -eq 0 -and
                                [UInt64]$physicalMask -eq 0 -and [UInt64]$distinct -eq 0 -and
                                [UInt64]$peak -eq 0) `
                                "$kernelContext forced-one evidence must report zero physical work."
                        }
						else {
							Assert-Stage5Condition ([UInt64]$submitted -gt 0 -and
								[UInt64]$physical -gt 0 -and [UInt64]$physical -eq [UInt64]$completed -and
								[UInt64]$ownerHelped -eq 0 -and [UInt64]$distinct -gt 1 -and
                                [UInt64]$distinct -le [UInt64]$effectiveWorkers -and
                                [UInt64]$peak -gt 1 -and [UInt64]$peak -le [UInt64]$effectiveWorkers) `
								"$kernelContext does not prove concurrent work on more than one physical worker."
							$provenKernelMask = $provenKernelMask -bor [UInt64]$kernelBits[$kernelIndex]
						}
                    }
                    ++$peerRecordCount
                }
                ++$matchIndex
            }
        }
    }
	Assert-Stage5Condition ($matchIndex -eq 16 -and $peerRecordCount -eq 40) `
		'Installed NET3 loopback evidence must contain exactly 16 matches and 40 nested peer records (20 peers per title).'
	Assert-Stage5Condition ($provenKernelMask -eq [UInt64]0x3F) `
		'Installed NET3 loopback evidence did not prove every advertised live kernel.'
    return [pscustomobject]@{
        schemaVersion = 1
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = Get-Stage5FinalAcceptanceFileSha256 $full
        generalsExecutableSha256 = $ExpectedGeneralsExecutableSha256
        zeroHourExecutableSha256 = $ExpectedZeroHourExecutableSha256
        generalsBuildCompatibilityCrc = $validatedBuildCrcs.Generals
        zeroHourBuildCompatibilityCrc = $validatedBuildCrcs.ZeroHour
        generalsContentCrc = $validatedContentCrcs.Generals
        zeroHourContentCrc = $validatedContentCrcs.ZeroHour
		provenKernelMask = $provenKernelMask
        matchCount = 16
        peerRecordCount = 40
        rawEvidenceEntries = $validatedRawOutputs
    }
}

function Get-Stage5ScalingRunCommand {
    param([string]$Title, [string]$Fixture, [string]$Lane,
        [string]$ExecutableSha256)
    $executable = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    $workerCount = switch ($Lane) {
        'stage3-forced-one' { 1 }
        'forced-one' { 1 }
        'physical-8' { 8 }
        'physical-16' { 16 }
        default { throw "Unsupported Stage 5 scaling lane '$Lane'." }
    }
    $replayArgument = "Stage5Scaling\$Fixture.rep"
    return "$executable -headless -noFPSLimit -pipelineMode serial -simulationMode parallel -workerPolicy auto -validationExecutableSha256 $ExecutableSha256 -workerCount $workerCount -replay $replayArgument"
}

function Read-Stage5PerformanceScalingTopologyReceipt {
    param([string]$Path, [string]$ExpectedSourceCommit,
        [string]$ExpectedExecutableSha256, [string]$ExpectedTitle,
        [object]$Snapshot = $null, [string]$ExpectedSnapshotSha256 = '')
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 5 scaling topology receipt'
    }
    else {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) `
            'Stage 5 scaling topology caller-supplied snapshot must include its independently expected SHA-256.'
    }
    $topologySnapshotPath = $Snapshot.path
    $topologySnapshotHash = $Snapshot.sha256
    Assert-Stage5Condition ($Snapshot.PSObject.Properties.Name -contains 'path' -and
        $topologySnapshotPath -is [string] -and
        [IO.Path]::GetFullPath($topologySnapshotPath) -ceq $full) `
        'Stage 5 scaling topology snapshot is bound to a different path.'
    if ([string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) {
        Assert-Stage5Condition ($topologySnapshotHash -is [string]) `
            'Stage 5 scaling topology snapshot SHA-256 must be a JSON string.'
        $ExpectedSnapshotSha256 = $topologySnapshotHash
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot $ExpectedSnapshotSha256 `
        'Stage 5 scaling topology receipt' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 5 scaling topology receipt'
    Assert-Stage5JsonShape $document @('schemaVersion', 'producer', 'source',
        'sourceCommit', 'executableSha256', 'runId', 'processId',
        'processCreationTimeUtc100ns', 'argumentString', 'commandLine',
        'logicalProcessors', 'selectedLanes') 'Stage 5 scaling topology receipt'
    $runId = Get-Stage5JsonValue $document 'runId' 'Stage 5 scaling topology receipt'
    $processId = Get-Stage5JsonValue $document 'processId' 'Stage 5 scaling topology receipt'
    $processCreation = Get-Stage5JsonValue $document 'processCreationTimeUtc100ns' `
        'Stage 5 scaling topology receipt'
    $argumentString = Get-Stage5JsonValue $document 'argumentString' `
        'Stage 5 scaling topology receipt'
    $commandLine = Get-Stage5JsonValue $document 'commandLine' `
        'Stage 5 scaling topology receipt'
    $expectedExecutable = if ($ExpectedTitle -ceq 'Generals') {
        'generalsv.exe'
    } else { 'generalszh.exe' }
    $topologySchemaVersion = Get-Stage5JsonValue $document 'schemaVersion' `
        'Stage 5 scaling topology receipt'
    $topologyProducer = Get-Stage5JsonValue $document 'producer' `
        'Stage 5 scaling topology receipt'
    $topologySource = Get-Stage5JsonValue $document 'source' `
        'Stage 5 scaling topology receipt'
    $topologySourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Stage 5 scaling topology receipt'
    $topologyExecutableHash = Get-Stage5JsonValue $document 'executableSha256' `
        'Stage 5 scaling topology receipt'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $topologySchemaVersion) -and
        $topologySchemaVersion -eq 2 -and
        $topologyProducer -is [string] -and
        $topologySource -is [string] -and
        $topologySourceCommit -is [string] -and
        $topologyExecutableHash -is [string] -and
        $topologyExecutableHash -cmatch '^[0-9A-Fa-f]{64}$' -and
        $topologyProducer -ceq 'installed-runtime-scaling-runner-v2' -and
        $topologySource -ceq 'GetSystemCpuSetInformation' -and
        $topologySourceCommit -ceq $ExpectedSourceCommit -and
        $topologyExecutableHash -ceq $ExpectedExecutableSha256 -and
        $runId -is [string] -and $runId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and
        -not $runId.Contains('..') -and
        (Test-Stage5JsonInteger $processId) -and [Int64]$processId -gt 0 -and
        (Test-Stage5JsonInteger $processCreation) -and [Int64]$processCreation -gt 0 -and
        $argumentString -is [string] -and -not [string]::IsNullOrWhiteSpace($argumentString) -and
        $argumentString -match ('(?:^| )-validationExecutableSha256 ' +
            [Regex]::Escape($ExpectedExecutableSha256) + '(?: |$)') -and
        $argumentString -match '(?:^| )-workerCount 1(?: |$)' -and
        $commandLine -is [string] -and
        $commandLine.IndexOf($expectedExecutable, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
        $commandLine.EndsWith(' ' + $argumentString, [StringComparison]::Ordinal)) `
        'Stage 5 scaling topology receipt is not bound to the exact installed executable command.'

    $logicalProcessors = Get-Stage5JsonValue $document 'logicalProcessors' `
        'Stage 5 scaling topology receipt'
    Assert-Stage5Condition ($logicalProcessors -is [Array] -and $logicalProcessors.Count -ge 16) `
        'Stage 5 scaling topology receipt requires at least 16 logical processor rows.'
    $physicalByLogical = @{}
    $physicalCores = @{}
    for ($index = 0; $index -lt $logicalProcessors.Count; ++$index) {
        $logical = $logicalProcessors[$index]
        $context = "Stage 5 scaling topology logical processor $index"
        Assert-Stage5JsonShape $logical @('logicalProcessorIndex', 'physicalCoreIndex') $context
        $logicalIndex = Get-Stage5JsonValue $logical 'logicalProcessorIndex' $context
        $physicalIndex = Get-Stage5JsonValue $logical 'physicalCoreIndex' $context
        Assert-Stage5Condition ((Test-Stage5JsonInteger $logicalIndex) -and
            [int]$logicalIndex -eq $index -and (Test-Stage5JsonInteger $physicalIndex) -and
            [int]$physicalIndex -ge 0 -and [int]$physicalIndex -lt 64) `
            "$context is not a canonical CPU-set mapping."
        $physicalByLogical[$index] = [int]$physicalIndex
        $physicalCores[[int]$physicalIndex] = $true
    }
    Assert-Stage5Condition ($physicalCores.Count -ge 16) `
        'Stage 5 scaling topology receipt does not contain 16 distinct physical cores.'

    $rawLanes = Get-Stage5JsonValue $document 'selectedLanes' 'Stage 5 scaling topology receipt'
    $laneNames = @('forced-one', 'physical-8', 'physical-16')
    $laneWorkers = @(1, 8, 16)
    Assert-Stage5Condition ($rawLanes -is [Array] -and $rawLanes.Count -eq 3) `
        'Stage 5 scaling topology receipt requires exactly three selected lanes.'
    $lanes = @()
    for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
        $lane = $rawLanes[$laneIndex]
        $context = "Stage 5 scaling topology lane $laneIndex"
        Assert-Stage5JsonShape $lane @('name', 'requestedWorkers',
            'selectedLogicalProcessorIndices') $context
        $selected = Get-Stage5JsonValue $lane 'selectedLogicalProcessorIndices' $context
        $laneName = Get-Stage5JsonValue $lane 'name' $context
        $laneRequestedWorkers = Get-Stage5JsonValue $lane 'requestedWorkers' $context
        Assert-Stage5Condition ($laneName -is [string] -and
            (Test-Stage5JsonInteger $laneRequestedWorkers) -and
            $laneName -ceq $laneNames[$laneIndex] -and
            $laneRequestedWorkers -eq $laneWorkers[$laneIndex] -and
            $selected -is [Array] -and $selected.Count -eq $laneWorkers[$laneIndex]) `
            "$context does not identify the exact requested physical lane."
        $seenLogical = @{}
        $seenPhysical = @{}
        [UInt64]$physicalMask = 0
        foreach ($selectedIndex in $selected) {
            Assert-Stage5Condition ((Test-Stage5JsonInteger $selectedIndex) -and
                [int]$selectedIndex -ge 0 -and
                [int]$selectedIndex -lt $logicalProcessors.Count -and
                -not $seenLogical.ContainsKey([int]$selectedIndex)) `
                "$context contains a duplicate or unavailable logical processor."
            $seenLogical[[int]$selectedIndex] = $true
            $physicalIndex = [int]$physicalByLogical[[int]$selectedIndex]
            Assert-Stage5Condition (-not $seenPhysical.ContainsKey($physicalIndex)) `
                "$context selects sibling logical processors from one physical core."
            $seenPhysical[$physicalIndex] = $true
            $physicalMask = $physicalMask -bor ([UInt64]1 -shl $physicalIndex)
        }
        Assert-Stage5Condition ($seenPhysical.Count -eq $laneWorkers[$laneIndex]) `
            "$context does not select the exact distinct physical-core count."
        $lanes += [pscustomobject]@{
            name = $laneNames[$laneIndex]
            requestedWorkers = $laneWorkers[$laneIndex]
            selectedLogicalProcessors = $selected.Count
            selectedDistinctPhysicalCores = $seenPhysical.Count
            selectedPhysicalCoreMask = $physicalMask.ToString('X16')
        }
    }
    return [pscustomobject]@{
        runId = [string]$runId
        processId = [Int64]$processId
        processCreationTimeUtc100ns = [Int64]$processCreation
        argumentString = [string]$argumentString
        commandLine = [string]$commandLine
        physicalCoreCount = $physicalCores.Count
        logicalProcessorCount = $logicalProcessors.Count
        selectedLanes = $lanes
    }
}

function Read-Stage5PerformanceScalingStage3Baseline {
    param([string]$Path, [string]$ExpectedSha256,
        [string]$ExpectedExecutableSha256, [string]$ExpectedTitle,
        [object]$Snapshot = $null)
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 3 protected scaling baseline'
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot $ExpectedSha256 `
        'Stage 3 protected scaling baseline' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 3 protected scaling baseline'
    Assert-Stage5JsonShape $document @('schemaVersion', 'stage', 'architecture',
        'title', 'executableSha256', 'fixtureManifestSha256', 'configuration',
        'physicalCoreCount', 'availableCpus', 'logicalProcessorCount',
        'warmupRuns', 'fixtures') 'Stage 3 protected scaling baseline'
    $physical = Get-Stage5JsonValue $document 'physicalCoreCount' `
        'Stage 3 protected scaling baseline'
    $available = Get-Stage5JsonValue $document 'availableCpus' `
        'Stage 3 protected scaling baseline'
    $logical = Get-Stage5JsonValue $document 'logicalProcessorCount' `
        'Stage 3 protected scaling baseline'
    $warmupRuns = Get-Stage5JsonValue $document 'warmupRuns' `
        'Stage 3 protected scaling baseline'
    $stage3SchemaVersion = Get-Stage5JsonValue $document 'schemaVersion' `
        'Stage 3 protected scaling baseline'
    $stage3Stage = Get-Stage5JsonValue $document 'stage' `
        'Stage 3 protected scaling baseline'
    $stage3Architecture = Get-Stage5JsonValue $document 'architecture' `
        'Stage 3 protected scaling baseline'
    $stage3Title = Get-Stage5JsonValue $document 'title' `
        'Stage 3 protected scaling baseline'
    $stage3ExecutableHash = Get-Stage5JsonValue $document 'executableSha256' `
        'Stage 3 protected scaling baseline'
    $stage3FixtureManifestHash = Get-Stage5JsonValue $document `
        'fixtureManifestSha256' 'Stage 3 protected scaling baseline'
    $stage3Configuration = Get-Stage5JsonValue $document 'configuration' `
        'Stage 3 protected scaling baseline'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $stage3SchemaVersion) -and
        $stage3SchemaVersion -eq 1 -and
        $stage3Stage -is [string] -and $stage3Architecture -is [string] -and
        $stage3Title -is [string] -and $stage3ExecutableHash -is [string] -and
        $stage3FixtureManifestHash -is [string] -and
        $stage3Configuration -is [string] -and
        $stage3Stage -ceq 'Stage3' -and
        $stage3Architecture -ceq 'x64' -and
        $stage3Title -ceq $ExpectedTitle -and
        $stage3ExecutableHash -ceq $ExpectedExecutableSha256 -and
        $stage3FixtureManifestHash -cmatch '^[0-9A-F]{64}$' -and
        $stage3Configuration -ceq 'parallel-1' -and
        (Test-Stage5JsonInteger $physical) -and [int]$physical -ge 16 -and
        (Test-Stage5JsonInteger $available) -and [int]$available -ge [int]$physical -and
        (Test-Stage5JsonInteger $logical) -and [int]$logical -ge 16 -and
        (Test-Stage5JsonInteger $warmupRuns) -and [int]$warmupRuns -eq 1) `
        'Stage 3 protected scaling baseline provenance or topology is invalid.'

    $fixtureNames = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $minimumUnits = @(1000, 4000, 8000, 8000)
    $fixtures = Get-Stage5JsonValue $document 'fixtures' `
        'Stage 3 protected scaling baseline'
    Assert-Stage5Condition ($fixtures -is [Array] -and $fixtures.Count -eq 4) `
        'Stage 3 protected scaling baseline requires the four canonical fixtures.'
    $validatedFixtures = @()
    for ($index = 0; $index -lt 4; ++$index) {
        $fixture = $fixtures[$index]
        $context = "Stage 3 protected scaling fixture $index"
        Assert-Stage5JsonShape $fixture @('id', 'fixtureSha256', 'playerCount',
            'peakUnitCount', 'wallMilliseconds') $context
        $fixtureId = Get-Stage5JsonValue $fixture 'id' $context
        $fixtureHash = Get-Stage5JsonValue $fixture 'fixtureSha256' $context
        $fixturePlayerCount = Get-Stage5JsonValue $fixture 'playerCount' $context
        $peak = Get-Stage5JsonValue $fixture 'peakUnitCount' $context
        $samples = Get-Stage5JsonValue $fixture 'wallMilliseconds' $context
        Assert-Stage5Condition ($fixtureId -is [string] -and
            $fixtureHash -is [string] -and
            (Test-Stage5JsonInteger $fixturePlayerCount) -and
            $fixtureId -ceq $fixtureNames[$index] -and
            $fixtureHash -cmatch '^[0-9A-F]{64}$' -and
            $fixturePlayerCount -eq 8 -and
            (Test-Stage5JsonInteger $peak) -and [int]$peak -ge $minimumUnits[$index] -and
            $samples -is [Array] -and $samples.Count -ge 4) `
            "$context identity, workload, or sample coverage is invalid."
        $validatedSamples = @()
        foreach ($sample in $samples) {
            Assert-Stage5Condition ((Test-Stage5JsonNumber $sample) -and
                [double]$sample -gt 0.0 -and -not [double]::IsInfinity([double]$sample) -and
                -not [double]::IsNaN([double]$sample)) `
                "$context contains a non-finite or non-positive wall-clock sample."
            $validatedSamples += [double]$sample
        }
        $validatedFixtures += [pscustomobject]@{
            id = $fixtureNames[$index]
            fixtureSha256 = $fixtureHash
            playerCount = 8
            peakUnitCount = [int]$peak
            wallMilliseconds = $validatedSamples
        }
    }
    return [pscustomobject]@{
        sha256 = [string]$Snapshot.sha256
        fixtureManifestSha256 = $stage3FixtureManifestHash
        warmupRuns = [int]$warmupRuns
        physicalCoreCount = [int]$physical
        logicalProcessorCount = [int]$logical
        fixtures = $validatedFixtures
    }
}

function Assert-Stage5PerformancePhaseBaselineProfile {
    param(
        [object]$Profile,
        [string]$ExpectedFixtureSha256,
        [int]$ExpectedWarmupRuns,
        [int]$ExpectedMeasuredRuns,
        [string]$Context = 'Stage 5 performance phase-baseline profile'
    )
    Assert-Stage5JsonShape $Profile @('profileId', 'fixtureId', 'sourceLane',
        'sourcePolicySha256', 'limits', 'residentAttemptCapacity',
        'residentRangeCapacity', 'fixtureSha256', 'window', 'warmupRuns',
        'measuredRuns') $Context
    Assert-Stage5JsonShape $Profile.limits @('maximumBytes', 'maximumRecords',
        'maximumLogicalEvents', 'maximumAttempts', 'maximumRanges') `
        "$Context limits"
    Assert-Stage5JsonShape $Profile.window @('firstCompletedFrame',
        'lastCompletedFrame', 'completedFrameCount', 'controlWindowCount') `
        "$Context window"
    foreach ($field in @('residentAttemptCapacity', 'residentRangeCapacity',
            'warmupRuns', 'measuredRuns')) {
        Assert-Stage5DiagnosticCounter $Profile[$field] "$Context $field"
    }
    foreach ($field in @('maximumBytes', 'maximumRecords',
            'maximumLogicalEvents', 'maximumAttempts', 'maximumRanges')) {
        Assert-Stage5DiagnosticCounter $Profile.limits[$field] `
            "$Context limits $field"
        Assert-Stage5Condition ([UInt64]$Profile.limits[$field] -gt 0) `
            "$Context limit '$field' must be positive."
    }
    foreach ($field in @('firstCompletedFrame', 'lastCompletedFrame',
            'completedFrameCount', 'controlWindowCount')) {
        Assert-Stage5DiagnosticCounter $Profile.window[$field] `
            "$Context window $field"
    }
    $profileId = Get-Stage5JsonValue $Profile 'profileId' $Context
    $fixtureId = Get-Stage5JsonValue $Profile 'fixtureId' $Context
    $sourceLane = Get-Stage5JsonValue $Profile 'sourceLane' $Context
    $sourcePolicySha256 = Get-Stage5JsonValue $Profile 'sourcePolicySha256' $Context
    $fixtureSha256 = Get-Stage5JsonValue $Profile 'fixtureSha256' $Context
    Assert-Stage5Condition ($ExpectedFixtureSha256 -cmatch '^[0-9A-F]{64}$' -and
        $profileId -is [string] -and
        $fixtureId -is [string] -and
        $sourceLane -is [string] -and
        $sourcePolicySha256 -is [string] -and
        $fixtureSha256 -is [string] -and
        -not [string]::IsNullOrWhiteSpace($profileId) -and
        $fixtureId -ceq 'dense-eight-player' -and
        $sourceLane -ceq 'forced-one' -and
        $sourcePolicySha256 -cmatch '^[0-9A-F]{64}$' -and
        $fixtureSha256 -cmatch '^[0-9A-F]{64}$' -and
        $fixtureSha256 -ceq $ExpectedFixtureSha256 -and
        [UInt64]$Profile.residentAttemptCapacity -gt 0 -and
        [UInt64]$Profile.residentRangeCapacity -gt 0 -and
        [int]$Profile.warmupRuns -eq $ExpectedWarmupRuns -and
        [int]$Profile.measuredRuns -eq $ExpectedMeasuredRuns -and
        $ExpectedWarmupRuns -eq 1 -and $ExpectedMeasuredRuns -ge 3 -and
        [UInt64]$Profile.window.firstCompletedFrame -gt 0 -and
        [UInt64]$Profile.window.lastCompletedFrame -le [UInt32]::MaxValue -and
        [UInt64]$Profile.window.completedFrameCount -gt 0 -and
        [UInt64]$Profile.window.controlWindowCount -gt 0 -and
        [decimal]$Profile.window.lastCompletedFrame -
            [decimal]$Profile.window.firstCompletedFrame + 1 -eq
            [decimal]$Profile.window.completedFrameCount) `
        "$Context identity, schedule, or accounting bounds are invalid."
    return $Profile
}

function Read-Stage5PerformanceQualificationDataEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedTitle,
        [string]$ExpectedManifestSha256,
        [string]$ExpectedClosureSha256,
        [object]$Snapshot = $null
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition ($ExpectedManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $ExpectedClosureSha256 -cmatch '^[0-9A-F]{64}$') `
        'Stage 5 performance qualification-data expected hashes are invalid.'
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 5 performance qualification-data manifest'
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot `
        $ExpectedManifestSha256 `
        'Stage 5 performance qualification-data manifest' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 5 performance qualification-data manifest'
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'title', 'archiveSource', 'runtimeRoot',
        'files', 'closureSha256') `
        'Stage 5 performance qualification-data manifest'
    $archive = Get-Stage5JsonValue $document 'archiveSource' `
        'Stage 5 performance qualification-data manifest'
    Assert-Stage5JsonShape $archive @('object', 'sha256') `
        'Stage 5 performance qualification-data archive source'
    $runtimeRoot = Get-Stage5JsonValue $document 'runtimeRoot' `
        'Stage 5 performance qualification-data manifest'
    $performanceDataSchemaVersion = Get-Stage5JsonValue $document `
        'schemaVersion' 'Stage 5 performance qualification-data manifest'
    $performanceDataEvidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Stage 5 performance qualification-data manifest'
    $performanceDataProducer = Get-Stage5JsonValue $document 'producer' `
        'Stage 5 performance qualification-data manifest'
    $performanceDataSourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Stage 5 performance qualification-data manifest'
    $performanceDataTitle = Get-Stage5JsonValue $document 'title' `
        'Stage 5 performance qualification-data manifest'
    $archiveObject = Get-Stage5JsonValue $archive 'object' `
        'Stage 5 performance qualification-data archive source'
    $archiveHash = Get-Stage5JsonValue $archive 'sha256' `
        'Stage 5 performance qualification-data archive source'
    $performanceDataClosureSha256 = Get-Stage5JsonValue $document `
        'closureSha256' 'Stage 5 performance qualification-data manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $performanceDataSchemaVersion) -and
        $performanceDataSchemaVersion -eq 1 -and
        $performanceDataEvidenceKind -is [string] -and
        $performanceDataProducer -is [string] -and
        $performanceDataSourceCommit -is [string] -and
        $performanceDataTitle -is [string] -and
        $archiveObject -is [string] -and $archiveHash -is [string] -and
        $runtimeRoot -is [string] -and
        $performanceDataClosureSha256 -is [string] -and
        $performanceDataEvidenceKind -ceq
            'stage5-performance-qualification-data' -and
        $performanceDataProducer -ceq 'genci-r2-trimmed-data-v1' -and
        $performanceDataSourceCommit -ceq $ExpectedSourceCommit -and
        $performanceDataTitle -ceq $ExpectedTitle -and $ExpectedTitle -ceq 'ZeroHour' -and
        $archiveObject -ceq 's3://github-ci/zerohour104_gamedata_trimmed.7z' -and
        $archiveHash -ceq
            '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21' -and
        [IO.Path]::IsPathRooted($runtimeRoot) -and
        [IO.Path]::GetFullPath($runtimeRoot).StartsWith('H:\',
            [StringComparison]::OrdinalIgnoreCase) -and
        $performanceDataClosureSha256 -ceq $ExpectedClosureSha256) `
        'Stage 5 performance qualification-data identity or archive provenance is invalid.'

    $required = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($requiredPath in @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')) {
        [void]$required.Add($requiredPath)
    }
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $files = Get-Stage5JsonValue $document 'files' `
        'Stage 5 performance qualification-data manifest'
    Assert-Stage5Condition ($files -is [Array] -and $files.Count -ge 6) `
        'Stage 5 performance qualification-data file coverage is incomplete.'
    $previousPath = $null
    foreach ($entry in $files) {
        Assert-Stage5JsonShape $entry @('path', 'sha256', 'length') `
            'Stage 5 performance qualification-data file'
        $relative = Get-Stage5JsonValue $entry 'path' `
            'Stage 5 performance qualification-data file'
        $hash = Get-Stage5JsonValue $entry 'sha256' `
            'Stage 5 performance qualification-data file'
        $length = Get-Stage5JsonValue $entry 'length' `
            'Stage 5 performance qualification-data file'
        $isRootBig = $relative -is [string] -and
            $relative.IndexOf('/') -lt 0 -and
            $relative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $relative -is [string] -and
            $relative.StartsWith('Data/', [StringComparison]::OrdinalIgnoreCase)
        Assert-Stage5Condition ($relative -is [string] -and
            $relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*$' -and
            $relative -cnotmatch '(^|/)\.\.?(/|$)' -and
            ($isRootBig -or $isDataFile) -and
            $hash -is [string] -and $hash -cmatch '^[0-9A-F]{64}$' -and
            (Test-Stage5JsonInteger $length) -and [Int64]$length -gt 0 -and
            ($null -eq $previousPath -or
                [StringComparer]::Ordinal.Compare($previousPath, $relative) -lt 0) -and
            $seen.Add([string]$relative)) `
            "Stage 5 performance qualification-data path is unsafe, duplicated, or unsorted: $relative"
        [void]$required.Remove([string]$relative)
        $canonicalLines.Add(('{0}|{1}|{2}' -f $relative, $hash,
            [Int64]$length)) | Out-Null
        $previousPath = [string]$relative
    }
    Assert-Stage5Condition ($required.Count -eq 0) `
        'Stage 5 performance qualification-data omits a required Zero Hour file.'
    $canonicalText = ($canonicalLines.ToArray() -join "`n") + "`n"
    $canonicalBytes = [Text.Encoding]::UTF8.GetBytes($canonicalText)
    $computedClosure = Get-Stage5FinalAcceptanceSha256FromBytes $canonicalBytes
    Assert-Stage5Condition ($computedClosure -ceq $ExpectedClosureSha256) `
        'Stage 5 performance qualification-data closure is stale or substituted.'
    return [pscustomobject]@{
        path = $full
        sha256 = [string]$Snapshot.sha256
        closureSha256 = $computedClosure
        fileCount = $files.Count
        runtimeRoot = [IO.Path]::GetFullPath([string]$runtimeRoot).TrimEnd('\', '/')
    }
}

function Read-Stage5PerformanceScalingRawSamples {
    param([string]$Path, [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256, [string]$ExpectedExecutableSha256,
        [string]$ExpectedStage3BaselineSha256,
        [string]$ExpectedPhaseBaselineProfileSha256, [string]$ExpectedTitle,
        [object]$Snapshot = $null, [string]$ExpectedSnapshotSha256 = '')
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 5 scaling raw-sample manifest'
    }
    else {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) `
            'Stage 5 scaling raw-sample caller-supplied snapshot must include its independently expected SHA-256.'
    }
    $rawSampleSnapshotPath = $Snapshot.path
    $rawSampleSnapshotHash = $Snapshot.sha256
    Assert-Stage5Condition ($Snapshot.PSObject.Properties.Name -contains 'path' -and
        $rawSampleSnapshotPath -is [string] -and
        [IO.Path]::GetFullPath($rawSampleSnapshotPath) -ceq $full) `
        'Stage 5 scaling raw-sample snapshot is bound to a different path.'
    if ([string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) {
        Assert-Stage5Condition ($rawSampleSnapshotHash -is [string]) `
            'Stage 5 scaling raw-sample snapshot SHA-256 must be a JSON string.'
        $ExpectedSnapshotSha256 = $rawSampleSnapshotHash
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot $ExpectedSnapshotSha256 `
        'Stage 5 scaling raw-sample manifest' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind', 'producer',
        'recordedUtc', 'cohortNonce', 'cohortCreatedUtc', 'qualificationMode',
        'referencePolicy', 'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'title', 'executableSha256',
        'stage3SourceCommit', 'stage3ExecutableSha256', 'stage3BaselineSha256',
        'measurementMode', 'installedRuntime', 'recordedTaskRoot',
        'relocationManifest', 'hostQualification',
        'phaseBaselineProfile', 'performanceData', 'stage3Baseline',
        'topologyReceipt', 'stage3Samples',
        'fixtureSamples', 'phaseSamples',
        'phaseAccountingSamples', 'kernelSamples') 'Stage 5 scaling raw-sample manifest'
    $stage3SourceCommit = Get-Stage5JsonValue $document 'stage3SourceCommit' `
        'Stage 5 scaling raw-sample manifest'
    $stage3ExecutableSha256 = Get-Stage5JsonValue $document 'stage3ExecutableSha256' `
        'Stage 5 scaling raw-sample manifest'
    $recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' `
        'Stage 5 scaling raw-sample manifest'
    $cohortNonce = Get-Stage5JsonValue $document 'cohortNonce' `
        'Stage 5 scaling raw-sample manifest'
    $cohortCreatedUtc = Get-Stage5JsonValue $document 'cohortCreatedUtc' `
        'Stage 5 scaling raw-sample manifest'
    $runtimeClosure = Get-Stage5JsonValue $document 'runtimeClosure' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $runtimeClosure @('dependencyManifestSha256',
        'closureSha256') 'Stage 5 scaling raw-sample runtime closure'
    $runtimeClosureManifestSha256 = Get-Stage5JsonValue $runtimeClosure `
        'dependencyManifestSha256' 'Stage 5 scaling raw-sample runtime closure'
    $runtimeClosureSha256 = Get-Stage5JsonValue $runtimeClosure 'closureSha256' `
        'Stage 5 scaling raw-sample runtime closure'
    Assert-Stage5Condition ($runtimeClosureManifestSha256 -is [string] -and
        $runtimeClosureSha256 -is [string] -and
        $runtimeClosureManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $runtimeClosureSha256 -cmatch '^[0-9A-F]{64}$') `
        'Stage 5 scaling raw-sample runtime closure hashes must be JSON strings.'
    [DateTimeOffset]$parsedRecordedUtc = [DateTimeOffset]::MinValue
    [DateTimeOffset]$parsedCohortCreatedUtc = [DateTimeOffset]::MinValue
    $recordedValid = $recordedUtc -is [string] -and
        [DateTimeOffset]::TryParseExact($recordedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$parsedRecordedUtc)
    $cohortValid = $cohortCreatedUtc -is [string] -and
        [DateTimeOffset]::TryParseExact($cohortCreatedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$parsedCohortCreatedUtc)
    $rawSampleSchemaVersion = Get-Stage5JsonValue $document 'schemaVersion' `
        'Stage 5 scaling raw-sample manifest'
    $rawSampleEvidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Stage 5 scaling raw-sample manifest'
    $rawSampleProducer = Get-Stage5JsonValue $document 'producer' `
        'Stage 5 scaling raw-sample manifest'
    $rawSampleQualificationMode = Get-Stage5JsonValue $document `
        'qualificationMode' 'Stage 5 scaling raw-sample manifest'
    $rawSampleReferencePolicy = Get-Stage5JsonValue $document `
        'referencePolicy' 'Stage 5 scaling raw-sample manifest'
    $rawSampleSourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Stage 5 scaling raw-sample manifest'
    $rawSampleArtifactSetSha256 = Get-Stage5JsonValue $document `
        'artifactSetSha256' 'Stage 5 scaling raw-sample manifest'
    $rawSampleTitle = Get-Stage5JsonValue $document 'title' `
        'Stage 5 scaling raw-sample manifest'
    $rawSampleExecutableSha256 = Get-Stage5JsonValue $document `
        'executableSha256' 'Stage 5 scaling raw-sample manifest'
    $rawSampleStage3BaselineSha256 = Get-Stage5JsonValue $document `
        'stage3BaselineSha256' 'Stage 5 scaling raw-sample manifest'
    $rawSampleMeasurementMode = Get-Stage5JsonValue $document `
        'measurementMode' 'Stage 5 scaling raw-sample manifest'
    $rawSampleInstalledRuntime = Get-Stage5JsonValue $document `
        'installedRuntime' 'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $rawSampleSchemaVersion) -and
        $rawSampleSchemaVersion -eq 3 -and
        $rawSampleEvidenceKind -is [string] -and
        $rawSampleProducer -is [string] -and
        $rawSampleQualificationMode -is [string] -and
        $rawSampleReferencePolicy -is [string] -and
        $rawSampleSourceCommit -is [string] -and
        $rawSampleArtifactSetSha256 -is [string] -and
        $rawSampleTitle -is [string] -and
        $rawSampleExecutableSha256 -is [string] -and
        $rawSampleStage3BaselineSha256 -is [string] -and
        $rawSampleMeasurementMode -is [string] -and
        $rawSampleEvidenceKind -ceq
            'stage5-performance-scaling-raw-samples' -and
        $rawSampleProducer -ceq 'installed-runtime-scaling-runner-v3' -and
        $recordedValid -and $cohortValid -and
        $recordedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        $cohortCreatedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        $parsedRecordedUtc -ge $parsedCohortCreatedUtc -and
        $cohortNonce -is [string] -and $cohortNonce -cmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
        $rawSampleQualificationMode -ceq 'External16Core' -and
        $rawSampleReferencePolicy -ceq 'paired-serial-oracle-v1' -and
        $rawSampleSourceCommit -ceq $ExpectedSourceCommit -and
        $rawSampleArtifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $rawSampleTitle -ceq $ExpectedTitle -and
        $rawSampleExecutableSha256 -ceq $ExpectedExecutableSha256 -and
        $stage3SourceCommit -is [string] -and $stage3SourceCommit -match '^[0-9a-f]{40}$' -and
        $stage3ExecutableSha256 -is [string] -and $stage3ExecutableSha256 -match '^[0-9A-F]{64}$' -and
        $rawSampleStage3BaselineSha256 -ceq $ExpectedStage3BaselineSha256 -and
        $rawSampleMeasurementMode -ceq 'headless-throughput' -and
        $rawSampleInstalledRuntime -is [bool] -and
        $rawSampleInstalledRuntime -eq $true) `
        'Stage 5 scaling raw-sample manifest provenance is invalid.'
    foreach ($field in @('dependencyManifestSha256', 'closureSha256')) {
        $runtimeClosureHash = Get-Stage5JsonValue $runtimeClosure $field `
            'Stage 5 scaling raw-sample runtime closure'
        Assert-Stage5Condition ($runtimeClosureHash -is [string] -and
                $runtimeClosureHash -cmatch '^[0-9A-F]{64}$') `
            "Stage 5 scaling raw-sample runtime closure field '$field' is invalid."
    }

    $currentTaskRoot = [IO.Path]::GetFullPath(
        (Split-Path -Parent $full)).TrimEnd('\', '/')
    $recordedTaskRootValue = Get-Stage5JsonValue $document `
        'recordedTaskRoot' 'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($recordedTaskRootValue -is [string] -and
        [IO.Path]::IsPathRooted($recordedTaskRootValue)) `
        'Stage 5 scaling raw-sample recorded task root is not absolute.'
    $recordedTaskRoot = [IO.Path]::GetFullPath(
        $recordedTaskRootValue).TrimEnd('\', '/')
    Assert-Stage5Condition ($recordedTaskRoot.StartsWith('H:\',
            [StringComparison]::OrdinalIgnoreCase) -and
        $recordedTaskRoot.Length -gt 3) `
        'Stage 5 scaling raw-sample recorded task root is not an explicit task-owned H: path.'
    $relocationRows = Get-Stage5JsonValue $document 'relocationManifest' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($relocationRows -is [Array] -and
        $relocationRows.Count -gt 0) `
        'Stage 5 scaling raw-sample relocation manifest is empty.'
    $relocationMap = @{}
    $relocatedPaths = @{}
    foreach ($row in $relocationRows) {
        $context = 'Stage 5 scaling relocation entry'
        Assert-Stage5JsonShape $row @('recordedPath', 'path', 'sha256',
            'length', 'evidenceKind') $context
        $recordedPathValue = Get-Stage5JsonValue $row 'recordedPath' $context
        $relativePath = Get-Stage5JsonValue $row 'path' $context
        $rowSha256 = Get-Stage5JsonValue $row 'sha256' $context
        $rowLength = Get-Stage5JsonValue $row 'length' $context
        $evidenceKind = Get-Stage5JsonValue $row 'evidenceKind' $context
        Assert-Stage5Condition ($recordedPathValue -is [string] -and
            [IO.Path]::IsPathRooted($recordedPathValue) -and
            $relativePath -is [string] -and
            -not [IO.Path]::IsPathRooted($relativePath) -and
            -not [string]::IsNullOrWhiteSpace($relativePath) -and
            $rowSha256 -is [string] -and $rowSha256 -cmatch '^[0-9A-F]{64}$' -and
            (Test-Stage5JsonInteger $rowLength) -and [Int64]$rowLength -gt 0 -and
            $evidenceKind -is [string] -and
            @('JsonReceipt', 'Replay', 'RawLog', 'Trace') -ccontains $evidenceKind) `
            "$context is malformed."
        $recordedPath = [IO.Path]::GetFullPath($recordedPathValue)
        Assert-Stage5FinalAcceptancePathContained $recordedTaskRoot `
            $recordedPath "$context recorded path"
        $recordedRelative = $recordedPath.Substring($recordedTaskRoot.Length).TrimStart('\', '/')
        $relocatedPath = Resolve-Stage5FinalAcceptanceFile $currentTaskRoot `
            $relativePath "$context relocated path"
        Assert-Stage5Condition ($recordedRelative.Replace('\', '/') -ceq
                $relativePath.Replace('\', '/') -and
            -not $relocationMap.ContainsKey($recordedPath) -and
            -not $relocatedPaths.ContainsKey($relocatedPath)) `
            "$context aliases, duplicates, or changes its exact task-root-relative suffix."
        $rowSnapshot = if (@('Trace', 'Replay') -ccontains $evidenceKind) {
            Get-Stage5FinalAcceptanceFileSnapshot $relocatedPath $context `
                -HashOnly -EvidenceKind $evidenceKind
        }
        else {
            Get-Stage5FinalAcceptanceFileSnapshot $relocatedPath $context `
                -EvidenceKind $evidenceKind
        }
        if (@('Trace', 'Replay') -ccontains $evidenceKind) {
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 $rowSnapshot `
                $rowSha256 ([Int64]$rowLength) $context | Out-Null
        }
        else {
            Assert-Stage5FinalAcceptanceSnapshotSha256 $rowSnapshot $rowSha256 `
                $context | Out-Null
            Assert-Stage5Condition ([Int64]$rowSnapshot.length -eq
                [Int64]$rowLength) "$context byte length changed."
        }
        $relocationMap[$recordedPath] = $relocatedPath
        $relocatedPaths[$relocatedPath] = $true
    }

    $hostEntry = Get-Stage5JsonValue $document 'hostQualification' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $hostEntry @('path', 'sha256') `
        'Stage 5 scaling host qualification reference'
    $hostPathValue = Get-Stage5JsonValue $hostEntry 'path' `
        'Stage 5 scaling host qualification reference'
    $hostShaValue = Get-Stage5JsonValue $hostEntry 'sha256' `
        'Stage 5 scaling host qualification reference'
    Assert-Stage5Condition ($hostPathValue -is [string] -and
        $hostShaValue -is [string]) `
        'Stage 5 scaling host qualification reference path/hash must be JSON strings.'
    $hostPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        $hostPathValue `
        'Stage 5 scaling host qualification'
    $hostSha256 = $hostShaValue
    $isRelocated = -not [String]::Equals($currentTaskRoot,
        $recordedTaskRoot, [StringComparison]::OrdinalIgnoreCase)
    $hostDiagnostics = if ($isRelocated) {
        ConvertTo-Stage5PerformanceDiagnostics $hostPath $hostSha256 `
            $ExpectedSourceCommit $ExpectedArtifactSetSha256 `
            $ExpectedExecutableSha256 -ExpectedTitle $ExpectedTitle `
            -RelocationMap $relocationMap -RecordedTaskRoot $recordedTaskRoot
    }
    else {
        ConvertTo-Stage5PerformanceDiagnostics $hostPath $hostSha256 `
            $ExpectedSourceCommit $ExpectedArtifactSetSha256 `
            $ExpectedExecutableSha256 -ExpectedTitle $ExpectedTitle
    }
    Assert-Stage5Condition ($hostDiagnostics.qualificationMode -ceq 'External16Core' -and
        $hostDiagnostics.referencePolicy -ceq 'paired-serial-oracle-v1') `
        'Stage 5 scaling raw samples are not rooted in the external paired host qualification.'
    $hostSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $hostPath `
        'Stage 5 scaling host qualification'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $hostSnapshot $hostSha256 `
        'Stage 5 scaling host qualification' | Out-Null
    $hostAggregate = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $hostSnapshot `
        'Stage 5 scaling host qualification'
    $hostAggregateCohortNonce = Get-Stage5JsonValue $hostAggregate 'cohortNonce' `
        'Stage 5 scaling host qualification'
    $hostAggregateCohortCreatedUtc = Get-Stage5JsonValue $hostAggregate `
        'cohortCreatedUtc' 'Stage 5 scaling host qualification'
    $hostAggregateRecordedUtc = Get-Stage5JsonValue $hostAggregate 'recordedUtc' `
        'Stage 5 scaling host qualification'
    $hostAggregateClosure = Get-Stage5JsonValue $hostAggregate 'runtimeClosure' `
        'Stage 5 scaling host qualification'
    $hostAggregateStage3 = Get-Stage5JsonValue $hostAggregate 'stage3Baseline' `
        'Stage 5 scaling host qualification'
    $hostAggregateManifestHash = Get-Stage5JsonValue $hostAggregateClosure `
        'dependencyManifestSha256' 'Stage 5 scaling host qualification runtime closure'
    $hostAggregateClosureHash = Get-Stage5JsonValue $hostAggregateClosure `
        'closureSha256' 'Stage 5 scaling host qualification runtime closure'
    $hostAggregateStage3SourceCommit = Get-Stage5JsonValue $hostAggregateStage3 `
        'sourceCommit' 'Stage 5 scaling host qualification Stage 3 baseline'
    $hostAggregateStage3ExecutableHash = Get-Stage5JsonValue $hostAggregateStage3 `
        'executableSha256' 'Stage 5 scaling host qualification Stage 3 baseline'
    $hostAggregateStage3Hash = Get-Stage5JsonValue $hostAggregateStage3 `
        'sha256' 'Stage 5 scaling host qualification Stage 3 baseline'
    Assert-Stage5Condition ($hostAggregateCohortNonce -is [string] -and
        $hostAggregateCohortCreatedUtc -is [string] -and
        $hostAggregateRecordedUtc -is [string] -and
        $hostAggregateManifestHash -is [string] -and
        $hostAggregateClosureHash -is [string] -and
        $hostAggregateStage3SourceCommit -is [string] -and
        $hostAggregateStage3ExecutableHash -is [string] -and
        $hostAggregateStage3Hash -is [string] -and
        $hostAggregateCohortNonce -ceq $cohortNonce -and
        $hostAggregateCohortCreatedUtc -ceq $cohortCreatedUtc -and
        $hostAggregateRecordedUtc -ceq $recordedUtc -and
        $hostAggregateManifestHash -ceq $runtimeClosureManifestSha256 -and
        $hostAggregateClosureHash -ceq $runtimeClosureSha256 -and
        $hostAggregateStage3SourceCommit -ceq $stage3SourceCommit -and
        $hostAggregateStage3ExecutableHash -ceq $stage3ExecutableSha256 -and
        $hostAggregateStage3Hash -ceq $ExpectedStage3BaselineSha256) `
        'Stage 5 scaling raw provenance differs from its validated host qualification.'

    $phaseProfileEntry = Get-Stage5JsonValue $document 'phaseBaselineProfile' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $phaseProfileEntry @('path', 'sha256') `
        'Stage 5 scaling phase-baseline profile reference'
    $phaseProfilePathValue = Get-Stage5JsonValue $phaseProfileEntry 'path' `
        'Stage 5 scaling phase-baseline profile reference'
    $phaseProfileHashValue = Get-Stage5JsonValue $phaseProfileEntry 'sha256' `
        'Stage 5 scaling phase-baseline profile reference'
    Assert-Stage5Condition ($phaseProfilePathValue -is [string] -and
        $phaseProfileHashValue -is [string]) `
        'Stage 5 scaling phase-baseline profile reference path/hash must be JSON strings.'
    $phaseProfilePath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        $phaseProfilePathValue `
        'Stage 5 scaling phase-baseline profile'
    $phaseProfileSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $phaseProfilePath `
        'Stage 5 scaling phase-baseline profile'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $phaseProfileSnapshot `
        $phaseProfileHashValue `
        'Stage 5 scaling phase-baseline profile' | Out-Null
    Assert-Stage5Condition ($ExpectedPhaseBaselineProfileSha256 -cmatch
            '^[0-9A-F]{64}$' -and
        $phaseProfileHashValue -ceq $ExpectedPhaseBaselineProfileSha256) `
        'Stage 5 scaling phase-baseline profile differs from its independently expected reviewed hash.'
    $phaseProfile = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
        $phaseProfileSnapshot 'Stage 5 scaling phase-baseline profile'
    $hostAggregatePhaseProfiles = Get-Stage5JsonValue $hostAggregate `
        'phaseBaselineProfiles' 'Stage 5 scaling host qualification'
    $hostAggregatePhaseProfile = Get-Stage5JsonValue $hostAggregate `
        'phaseBaselineProfile' 'Stage 5 scaling host qualification'
    $hostAggregatePhaseProfileHash = Get-Stage5JsonValue `
        $hostAggregatePhaseProfile 'sha256' `
        'Stage 5 scaling host qualification phase-baseline profile'
    $hostAggregatePhaseProfilePath = Get-Stage5JsonValue `
        $hostAggregatePhaseProfile 'path' `
        'Stage 5 scaling host qualification phase-baseline profile'
    Assert-Stage5Condition ($hostAggregatePhaseProfiles -is [Array] -and
        $hostAggregatePhaseProfiles.Count -eq 1 -and
        $hostAggregatePhaseProfileHash -is [string] -and
        $hostAggregatePhaseProfilePath -is [string] -and
        ($hostAggregatePhaseProfiles[0] | ConvertTo-Json -Compress -Depth 20) -ceq
            ($phaseProfile | ConvertTo-Json -Compress -Depth 20) -and
        $hostAggregatePhaseProfileHash -ceq $phaseProfileHashValue) `
        'Stage 5 scaling phase-baseline profile differs from the validated host qualification.'
    $recordedPhaseProfilePath = [IO.Path]::GetFullPath(
        $hostAggregatePhaseProfilePath)
    $resolvedHostPhaseProfilePath = if ($isRelocated) {
        Assert-Stage5Condition ($relocationMap.ContainsKey($recordedPhaseProfilePath)) `
            'Stage 5 host phase-baseline profile is absent from the relocation manifest.'
        [string]$relocationMap[$recordedPhaseProfilePath]
    } else { $recordedPhaseProfilePath }
    Assert-Stage5Condition ([String]::Equals($resolvedHostPhaseProfilePath,
        $phaseProfilePath, [StringComparison]::OrdinalIgnoreCase)) `
        'Stage 5 host phase-baseline profile path differs from the raw closure.'

    $performanceDataEntry = Get-Stage5JsonValue $document 'performanceData' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $performanceDataEntry @('path', 'sha256',
        'closureSha256', 'fileCount') `
        'Stage 5 scaling performance qualification-data reference'
    $performanceDataPath = Get-Stage5JsonValue $performanceDataEntry 'path' `
        'Stage 5 scaling performance qualification-data reference'
    $performanceDataSha256 = Get-Stage5JsonValue $performanceDataEntry 'sha256' `
        'Stage 5 scaling performance qualification-data reference'
    $performanceDataClosureSha256 = Get-Stage5JsonValue $performanceDataEntry `
        'closureSha256' 'Stage 5 scaling performance qualification-data reference'
    $performanceDataFileCount = Get-Stage5JsonValue $performanceDataEntry `
        'fileCount' 'Stage 5 scaling performance qualification-data reference'
    Assert-Stage5Condition ($performanceDataPath -is [string] -and
        $performanceDataSha256 -is [string] -and
        $performanceDataSha256 -cmatch '^[0-9A-F]{64}$' -and
        $performanceDataClosureSha256 -is [string] -and
        $performanceDataClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $performanceDataFileCount)) `
        'Stage 5 scaling performance-data reference fields are malformed.'
    $performanceDataPathValue = Get-Stage5JsonValue $performanceDataEntry 'path' `
        'Stage 5 scaling performance qualification-data reference'
    $performanceDataHashValue = Get-Stage5JsonValue $performanceDataEntry 'sha256' `
        'Stage 5 scaling performance qualification-data reference'
    $performanceDataClosureValue = Get-Stage5JsonValue $performanceDataEntry `
        'closureSha256' 'Stage 5 scaling performance qualification-data reference'
    $performanceDataFileCountValue = Get-Stage5JsonValue $performanceDataEntry `
        'fileCount' 'Stage 5 scaling performance qualification-data reference'
    Assert-Stage5Condition ($performanceDataPathValue -is [string] -and
        $performanceDataHashValue -is [string] -and
        $performanceDataClosureValue -is [string] -and
        $performanceDataFileCountValue -isnot [Array] -and
        (Test-Stage5JsonInteger $performanceDataFileCountValue)) `
        'Stage 5 scaling performance qualification-data reference fields are malformed.'
    $performanceDataPath = Resolve-Stage5FinalAcceptanceFile `
        (Split-Path -Parent $full) `
        $performanceDataPathValue `
        'Stage 5 performance qualification-data manifest'
    $performanceDataSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
        $performanceDataPath 'Stage 5 performance qualification-data manifest'
    $performanceData = Read-Stage5PerformanceQualificationDataEvidence `
        $performanceDataPath $ExpectedSourceCommit $ExpectedTitle `
        $performanceDataHashValue $performanceDataClosureValue `
        $performanceDataSnapshot
    $hostAggregatePerformanceData = Get-Stage5JsonValue $hostAggregate `
        'performanceData' 'Stage 5 scaling host qualification'
    $hostAggregatePerformanceDataPath = Get-Stage5JsonValue `
        $hostAggregatePerformanceData 'path' 'Stage 5 scaling host performance data'
    $hostAggregatePerformanceDataHash = Get-Stage5JsonValue `
        $hostAggregatePerformanceData 'sha256' 'Stage 5 scaling host performance data'
    $hostAggregatePerformanceDataClosure = Get-Stage5JsonValue `
        $hostAggregatePerformanceData 'closureSha256' 'Stage 5 scaling host performance data'
    $hostAggregatePerformanceDataFileCount = Get-Stage5JsonValue `
        $hostAggregatePerformanceData 'fileCount' 'Stage 5 scaling host performance data'
    Assert-Stage5Condition ((Test-Stage5JsonInteger `
            $performanceDataFileCountValue) -and
        [int]$performanceDataFileCountValue -eq
            $performanceData.fileCount -and
        $hostAggregatePerformanceDataPath -is [string] -and
        $hostAggregatePerformanceDataHash -is [string] -and
        $hostAggregatePerformanceDataClosure -is [string] -and
        $hostAggregatePerformanceDataFileCount -isnot [Array] -and
        (Test-Stage5JsonInteger $hostAggregatePerformanceDataFileCount) -and
        $performanceData.sha256 -is [string] -and
        $performanceData.closureSha256 -is [string] -and
        $hostAggregatePerformanceDataHash -ceq $performanceData.sha256 -and
        $hostAggregatePerformanceDataClosure -ceq $performanceData.closureSha256 -and
        [int]$hostAggregatePerformanceDataFileCount -eq $performanceData.fileCount) `
        'Stage 5 performance qualification-data differs from the validated host qualification.'
    $recordedPerformanceDataPath = [IO.Path]::GetFullPath(
        $hostAggregatePerformanceDataPath)
    $resolvedHostPerformanceDataPath = if ($isRelocated) {
        Assert-Stage5Condition ($relocationMap.ContainsKey(
                $recordedPerformanceDataPath)) `
            'Stage 5 host performance qualification-data is absent from the relocation manifest.'
        [string]$relocationMap[$recordedPerformanceDataPath]
    } else { $recordedPerformanceDataPath }
    Assert-Stage5Condition ([String]::Equals($resolvedHostPerformanceDataPath,
        $performanceDataPath, [StringComparison]::OrdinalIgnoreCase)) `
        'Stage 5 host performance qualification-data path differs from the raw closure.'

    $stage3Entry = Get-Stage5JsonValue $document 'stage3Baseline' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $stage3Entry @('path', 'sha256') `
        'Stage 3 protected scaling baseline reference'
    $stage3EntryPath = Get-Stage5JsonValue $stage3Entry 'path' `
        'Stage 3 protected scaling baseline reference'
    $stage3EntryHash = Get-Stage5JsonValue $stage3Entry 'sha256' `
        'Stage 3 protected scaling baseline reference'
    Assert-Stage5Condition ($stage3EntryPath -is [string] -and
        $stage3EntryHash -is [string] -and
        $stage3EntryHash -ceq $ExpectedStage3BaselineSha256) `
        'Stage 3 protected scaling baseline reference changed its independently expected hash.'
    $stage3Path = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        $stage3EntryPath `
        'Stage 3 protected scaling baseline'
    $stage3Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $stage3Path `
        'Stage 3 protected scaling baseline'
    $stage3Baseline = Read-Stage5PerformanceScalingStage3Baseline $stage3Path `
        $ExpectedStage3BaselineSha256 $stage3ExecutableSha256 $ExpectedTitle `
        $stage3Snapshot
    Assert-Stage5JsonShape $hostAggregate.schedule @('warmupRuns',
        'measuredRuns') 'Stage 5 scaling host schedule'
    Assert-Stage5DiagnosticCounter $hostAggregate.schedule.warmupRuns `
        'Stage 5 scaling host warmup runs'
    Assert-Stage5DiagnosticCounter $hostAggregate.schedule.measuredRuns `
        'Stage 5 scaling host measured runs'
    $phaseProfile = Assert-Stage5PerformancePhaseBaselineProfile $phaseProfile `
        $stage3Baseline.fixtures[3].fixtureSha256 `
        ([int]$hostAggregate.schedule.warmupRuns) `
        ([int]$hostAggregate.schedule.measuredRuns) `
        'Stage 5 retained performance phase-baseline profile'

    $topologyEntry = Get-Stage5JsonValue $document 'topologyReceipt' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5JsonShape $topologyEntry @('path', 'sha256') `
        'Stage 5 scaling topology receipt reference'
    $topologyEntryPath = Get-Stage5JsonValue $topologyEntry 'path' `
        'Stage 5 scaling topology receipt reference'
    $topologyEntryHash = Get-Stage5JsonValue $topologyEntry 'sha256' `
        'Stage 5 scaling topology receipt reference'
    Assert-Stage5Condition ($topologyEntryPath -is [string] -and
        $topologyEntryHash -is [string]) `
        'Stage 5 scaling topology receipt reference path/hash must be JSON strings.'
    $topologyPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        $topologyEntryPath `
        'Stage 5 scaling topology receipt'
    $topologySnapshot = Get-Stage5FinalAcceptanceFileSnapshot $topologyPath `
        'Stage 5 scaling topology receipt'
    $topologySha256 = Assert-Stage5FinalAcceptanceSnapshotSha256 $topologySnapshot `
        $topologyEntryHash `
        'Stage 5 scaling topology receipt'
    $topology = Read-Stage5PerformanceScalingTopologyReceipt $topologyPath `
        $ExpectedSourceCommit $ExpectedExecutableSha256 $ExpectedTitle `
        $topologySnapshot $topologySha256
    foreach ($referencePath in @($hostPath, $phaseProfilePath,
            $performanceDataPath, $stage3Path, $topologyPath)) {
        $recordedReferencePath = if ($isRelocated) {
            $match = @($relocationMap.GetEnumerator() | Where-Object {
                [String]::Equals([string]$_.Value, $referencePath,
                    [StringComparison]::OrdinalIgnoreCase)
            })
            Assert-Stage5Condition ($match.Count -eq 1) `
                'Stage 5 scaling direct evidence reference is absent from the relocation manifest.'
            [string]$match[0].Key
        }
        else { [IO.Path]::GetFullPath($referencePath) }
        Assert-Stage5Condition ($relocationMap.ContainsKey($recordedReferencePath)) `
            'Stage 5 scaling direct evidence reference is not relocation-bound.'
    }
    foreach ($recordedFixtureReference in @(
            [string]$hostDiagnostics.fixtureManifest.recordedPath) +
            @($hostDiagnostics.reviewedFixtures | ForEach-Object {
                [string]$_.recordedPath
            })) {
        $recordedFixtureReference = [IO.Path]::GetFullPath(
            $recordedFixtureReference)
        Assert-Stage5Condition ($relocationMap.ContainsKey(
                $recordedFixtureReference)) `
            'Stage 5 reviewed fixture evidence is absent from the relocation manifest.'
        if ($isRelocated) {
            Assert-Stage5FinalAcceptancePathContained $currentTaskRoot `
                ([string]$relocationMap[$recordedFixtureReference]) `
                'Stage 5 relocated reviewed fixture evidence'
        }
    }
    Assert-Stage5Condition ($stage3Baseline.fixtureManifestSha256 -ceq
            [string]$hostAggregate.fixtureManifest.sha256 -and
        $hostDiagnostics.fixtureManifest.sha256 -ceq
            $stage3Baseline.fixtureManifestSha256 -and
        @($hostDiagnostics.reviewedFixtures).Count -eq 4 -and
        @($hostDiagnostics.reviewedFixtures | Where-Object {
            $fixtureIndex = [Array]::IndexOf(@('one-thousand-units',
                'four-thousand-units', 'eight-thousand-units',
                'dense-eight-player'), [string]$_.id)
            $fixtureIndex -lt 0 -or $_.sha256 -cne
                $stage3Baseline.fixtures[$fixtureIndex].fixtureSha256
        }).Count -eq 0 -and
        $stage3Baseline.physicalCoreCount -eq $topology.physicalCoreCount -and
        $stage3Baseline.logicalProcessorCount -eq $topology.logicalProcessorCount) `
        'Stage 3 protected scaling baseline fixture manifest or topology differs from the qualified host.'

    $fixtureNames = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $minimumUnits = @(1000, 4000, 8000, 8000)
    $stage3Samples = Get-Stage5JsonValue $document 'stage3Samples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($stage3Samples -is [Array] -and
        $stage3Samples.Count -ge 12 -and $stage3Samples.Count % 4 -eq 0) `
        'Stage 3 raw samples require equal measured coverage for all four fixtures.'
    $stage3RepeatCount = [int]($stage3Samples.Count / 4)
    Assert-Stage5Condition ($stage3RepeatCount -ge 3) `
        'Stage 3 raw samples require at least three measured repeats per fixture.'
    $stage3Values = @{}
    $rowIndex = 0
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $stage3Values[$fixtureNames[$fixtureIndex]] = @()
        $baselineFixture = $stage3Baseline.fixtures[$fixtureIndex]
        Assert-Stage5Condition ($baselineFixture.wallMilliseconds.Count -eq
            $stage3RepeatCount + $stage3Baseline.warmupRuns) `
            "Stage 3 protected scaling fixture '$($fixtureNames[$fixtureIndex])' sample count does not match its raw rows."
        for ($repeat = 0; $repeat -lt $stage3RepeatCount; ++$repeat) {
            $sample = $stage3Samples[$rowIndex++]
            $context = "Stage 3 scaling raw sample $($rowIndex - 1)"
            Assert-Stage5JsonShape $sample @('fixture', 'fixtureSha256',
                'playerCount', 'peakUnitCount', 'lane', 'repeat',
                'baselineSampleIndex', 'executableSha256',
                'elapsedMilliseconds') $context
            $baselineSampleIndex = Get-Stage5JsonValue $sample `
                'baselineSampleIndex' $context
            $elapsed = Get-Stage5JsonValue $sample 'elapsedMilliseconds' $context
            $stage3SampleFixture = Get-Stage5JsonValue $sample 'fixture' $context
            $stage3SampleFixtureSha256 = Get-Stage5JsonValue $sample `
                'fixtureSha256' $context
            $stage3SamplePlayerCount = Get-Stage5JsonValue $sample `
                'playerCount' $context
            $stage3SamplePeakUnitCount = Get-Stage5JsonValue $sample `
                'peakUnitCount' $context
            $stage3SampleLane = Get-Stage5JsonValue $sample 'lane' $context
            $stage3SampleRepeat = Get-Stage5JsonValue $sample 'repeat' $context
            $stage3SampleExecutableSha256 = Get-Stage5JsonValue $sample `
                'executableSha256' $context
            $expectedSampleIndex = $stage3Baseline.warmupRuns + $repeat
            Assert-Stage5Condition ($stage3SampleFixture -is [string] -and
                $stage3SampleFixtureSha256 -is [string] -and
                (Test-Stage5JsonInteger $stage3SamplePlayerCount) -and
                (Test-Stage5JsonInteger $stage3SamplePeakUnitCount) -and
                $stage3SampleLane -is [string] -and
                $stage3SampleExecutableSha256 -is [string] -and
                $stage3SampleFixture -ceq $fixtureNames[$fixtureIndex] -and
                $stage3SampleFixtureSha256 -ceq $baselineFixture.fixtureSha256 -and
                $stage3SamplePlayerCount -eq 8 -and
                $stage3SamplePeakUnitCount -eq $baselineFixture.peakUnitCount -and
                $stage3SampleLane -ceq 'stage3-forced-one' -and
                (Test-Stage5JsonInteger $stage3SampleRepeat) -and
                $stage3SampleRepeat -eq $repeat -and
                (Test-Stage5JsonInteger $baselineSampleIndex) -and
                [int]$baselineSampleIndex -eq $expectedSampleIndex -and
                $stage3SampleExecutableSha256 -ceq $stage3ExecutableSha256 -and
                (Test-Stage5JsonNumber $elapsed) -and [double]$elapsed -gt 0.0 -and
                [Math]::Abs([double]$elapsed -
                    [double]$baselineFixture.wallMilliseconds[$expectedSampleIndex]) -le 0.0001) `
                "$context is not an exact indexed row from the protected Stage 3 baseline."
            $stage3Values[$fixtureNames[$fixtureIndex]] += [double]$elapsed
        }
    }

    $laneNames = @('forced-one', 'physical-8', 'physical-16')
    $laneWorkers = @(1, 8, 16)
    $fixtureSamples = Get-Stage5JsonValue $document 'fixtureSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($fixtureSamples -is [Array] -and
        $fixtureSamples.Count -ge 36 -and $fixtureSamples.Count % 12 -eq 0) `
        'Stage 5 scaling raw samples require equal per-repeat rows for four fixtures and three current lanes.'
    $repeatCount = [int]($fixtureSamples.Count / 12)
    Assert-Stage5Condition ($repeatCount -ge 3) `
        'Stage 5 scaling raw samples require at least three repeats per fixture lane.'
    $nativeThroughputByKey = @{}
    foreach ($nativeRun in @($hostDiagnostics.runs)) {
        $nativeKey = "$($nativeRun.fixtureId)|$($nativeRun.lane)|$($nativeRun.ordinal)"
        Assert-Stage5Condition ($fixtureNames -ccontains [string]$nativeRun.fixtureId -and
            $laneNames -ccontains [string]$nativeRun.lane -and
            (Test-Stage5JsonInteger $nativeRun.ordinal) -and
            [int]$nativeRun.ordinal -ge 0 -and [int]$nativeRun.ordinal -le $repeatCount -and
            $nativeRun.warmup -is [bool] -and
            [bool]$nativeRun.warmup -eq ([int]$nativeRun.ordinal -eq 0) -and
            -not $nativeThroughputByKey.ContainsKey($nativeKey)) `
            'Validated host qualification contains a duplicate or unexpected throughput schedule row.'
        $nativeThroughputByKey[$nativeKey] = $nativeRun
    }
    Assert-Stage5Condition ($nativeThroughputByKey.Count -eq
            12 * ($repeatCount + 1)) `
        'Validated host qualification throughput coverage differs from the raw-sample schedule.'
    $seenProcesses = @{}
    $runReceipts = @{}
    $fixtureAggregates = @()
    $rowIndex = 0
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $laneValues = @{}
        foreach ($laneName in $laneNames) { $laneValues[$laneName] = @() }
        $peakUnitCount = 0
        $minimumInitialUnitCount = [int]::MaxValue
        for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
            for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
                $sample = $fixtureSamples[$rowIndex++]
                $context = "Stage 5 scaling fixture raw sample $($rowIndex - 1)"
                Assert-Stage5JsonShape $sample @('fixture', 'fixtureSha256',
                    'playerCount', 'peakUnitCount',
                    'requestedMinimumUnitCount', 'initialUnitCount',
                    'lane', 'repeat', 'runId', 'processId',
                    'processCreationTimeUtc100ns', 'executableSha256',
                    'argumentString', 'commandLine', 'elapsedMilliseconds') $context
                $units = Get-Stage5JsonValue $sample 'peakUnitCount' $context
                $initialUnits = Get-Stage5JsonValue $sample 'initialUnitCount' $context
                $requestedUnits = Get-Stage5JsonValue $sample 'requestedMinimumUnitCount' $context
                $runId = Get-Stage5JsonValue $sample 'runId' $context
                $processId = Get-Stage5JsonValue $sample 'processId' $context
                $processCreation = Get-Stage5JsonValue $sample `
                    'processCreationTimeUtc100ns' $context
                $argumentString = Get-Stage5JsonValue $sample 'argumentString' $context
                $commandLine = Get-Stage5JsonValue $sample 'commandLine' $context
                $elapsed = Get-Stage5JsonValue $sample 'elapsedMilliseconds' $context
                $sampleFixture = Get-Stage5JsonValue $sample 'fixture' $context
                $sampleFixtureSha256 = Get-Stage5JsonValue $sample 'fixtureSha256' $context
                $samplePlayerCount = Get-Stage5JsonValue $sample 'playerCount' $context
                $sampleLane = Get-Stage5JsonValue $sample 'lane' $context
                $sampleRepeat = Get-Stage5JsonValue $sample 'repeat' $context
                $sampleExecutableSha256 = Get-Stage5JsonValue $sample `
                    'executableSha256' $context
                $processIdentity = "$processId|$processCreation"
                $nativeKey = "$($fixtureNames[$fixtureIndex])|$($laneNames[$laneIndex])|$($repeat + 1)"
                $nativeRun = $nativeThroughputByKey[$nativeKey]
                $expectedExecutable = if ($ExpectedTitle -ceq 'Generals') {
                    'generalsv.exe'
                } else { 'generalszh.exe' }
                Assert-Stage5Condition ($sampleFixture -is [string] -and
                    $sampleFixtureSha256 -is [string] -and
                    (Test-Stage5JsonInteger $samplePlayerCount) -and
                    $sampleLane -is [string] -and
                    $sampleExecutableSha256 -is [string] -and
                    $sampleFixture -ceq $fixtureNames[$fixtureIndex] -and
                    $sampleFixtureSha256 -ceq
                        $stage3Baseline.fixtures[$fixtureIndex].fixtureSha256 -and
                    $samplePlayerCount -eq 8 -and
                    (Test-Stage5JsonInteger $units) -and [int]$units -ge $minimumUnits[$fixtureIndex] -and
                    (Test-Stage5JsonInteger $requestedUnits) -and
                    [int]$requestedUnits -eq
                        [int]$stage3Baseline.fixtures[$fixtureIndex].peakUnitCount -and
                    (Test-Stage5JsonInteger $initialUnits) -and
                    [int]$initialUnits -ge [int]$requestedUnits -and [int]$initialUnits -le [int]$units -and
                    $sampleLane -ceq $laneNames[$laneIndex] -and
                    (Test-Stage5JsonInteger $sampleRepeat) -and
                    $sampleRepeat -eq $repeat -and
                    $runId -is [string] -and $runId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and
                    -not $runId.Contains('..') -and
                    (Test-Stage5JsonInteger $processId) -and [Int64]$processId -gt 0 -and
                    (Test-Stage5JsonInteger $processCreation) -and
                        [Int64]$processCreation -gt 0 -and
                    -not $seenProcesses.ContainsKey($processIdentity) -and
                    $sampleExecutableSha256 -ceq $ExpectedExecutableSha256 -and
                    $argumentString -is [string] -and
                        -not [string]::IsNullOrWhiteSpace($argumentString) -and
                    $argumentString -match ('(?:^| )-validationExecutableSha256 ' +
                        [Regex]::Escape($ExpectedExecutableSha256) + '(?: |$)') -and
                    $argumentString -match ('(?:^| )-workerCount ' +
                        $laneWorkers[$laneIndex] + '(?: |$)') -and
                    $argumentString -match '(?:^| )-replay .+$' -and
                    $commandLine -is [string] -and
                    $commandLine.IndexOf($expectedExecutable,
                        [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
                    $commandLine.EndsWith(' ' + $argumentString,
                        [StringComparison]::Ordinal) -and
                    (Test-Stage5JsonNumber $elapsed) -and [double]$elapsed -gt 0.0 -and
                    $null -ne $nativeRun -and -not [bool]$nativeRun.warmup -and
                    [string]$nativeRun.runId -ceq [string]$runId -and
                    [Int64]$nativeRun.processId -eq [Int64]$processId -and
                    [Int64]$nativeRun.processCreationTimeUtc100ns -eq
                        [Int64]$processCreation -and
                    [string]$nativeRun.argumentString -ceq [string]$argumentString -and
                    [string]$nativeRun.commandLine -ceq [string]$commandLine -and
                    [string]$nativeRun.fixtureSha256 -ceq
                        [string](Get-Stage5JsonValue $sample 'fixtureSha256' $context) -and
                    [int]$nativeRun.workload.playerCount -eq 8 -and
                    [int]$nativeRun.workload.peakUnitCount -eq [int]$units -and
                    [int]$nativeRun.workload.initialUnitCount -eq [int]$initialUnits -and
                    [int]$nativeRun.requestedMinimumUnitCount -eq [int]$requestedUnits -and
                    [Math]::Abs([double]$nativeRun.elapsedMilliseconds -
                        [double]$elapsed) -le 0.0001) `
                    "$context is not an exact installed per-process timing receipt."
                $peakUnitCount = [Math]::Max($peakUnitCount, [int]$units)
                $minimumInitialUnitCount = [Math]::Min($minimumInitialUnitCount, [int]$initialUnits)
                $seenProcesses[$processIdentity] = $true
                $runReceipts["$($fixtureNames[$fixtureIndex])|$($laneNames[$laneIndex])|$repeat"] =
                    [pscustomobject]@{
                        runId = [string]$runId
                        processId = [Int64]$processId
                        processCreationTimeUtc100ns = [Int64]$processCreation
                        argumentString = [string]$argumentString
                        commandLine = [string]$commandLine
                    }
                $laneValues[$laneNames[$laneIndex]] += [double]$elapsed
            }
        }
        $stage3 = Get-Stage5Median $stage3Values[$fixtureNames[$fixtureIndex]]
        $stage5 = Get-Stage5Median $laneValues['forced-one']
        $eight = Get-Stage5Median $laneValues['physical-8']
        $sixteen = Get-Stage5Median $laneValues['physical-16']
        $fixtureAggregates += [pscustomobject]@{
            name = $fixtureNames[$fixtureIndex]
            playerCount = 8
            peakUnitCount = $peakUnitCount
            requestedMinimumUnitCount =
                [int]$stage3Baseline.fixtures[$fixtureIndex].peakUnitCount
            minimumInitialUnitCount = $minimumInitialUnitCount
            repeats = $repeatCount
            stage3OneWorkerMilliseconds = $stage3
            stage5OneWorkerMilliseconds = $stage5
            eightPhysicalCoreMilliseconds = $eight
            sixteenPhysicalCoreMilliseconds = $sixteen
            oneWorkerRegressionRatio = $stage5 / $stage3
            eightPhysicalCoreSpeedup = $stage5 / $eight
            eightToSixteenSpeedup = $eight / $sixteen
        }
    }

    $topologyRunReceipt = $runReceipts['one-thousand-units|forced-one|0']
    Assert-Stage5Condition ($topology.runId -ceq $topologyRunReceipt.runId -and
        $topology.processId -eq $topologyRunReceipt.processId -and
        $topology.processCreationTimeUtc100ns -eq
            $topologyRunReceipt.processCreationTimeUtc100ns -and
        $topology.argumentString -ceq $topologyRunReceipt.argumentString -and
        $topology.commandLine -ceq $topologyRunReceipt.commandLine) `
        'Stage 5 scaling topology receipt is not correlated with its exact installed one-worker run.'

    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    $phaseSamples = Get-Stage5JsonValue $document 'phaseSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($phaseSamples -is [Array] -and
        $phaseSamples.Count -eq 2 * $phaseNames.Count * $repeatCount) `
        'Stage 5 scaling raw samples require every world/control one-worker phase for every repeat.'
    $phaseAggregates = @()
    $worldPhaseTotals = New-Object 'double[]' $repeatCount
    $worldPhaseSerialTotals = New-Object 'double[]' $repeatCount
    $controlPhaseTotals = New-Object 'double[]' $repeatCount
    $controlPhaseSerialTotals = New-Object 'double[]' $repeatCount
    $phaseBaselineReceipts = @{}
    $nativePhaseBaselineBySource = @{}
    foreach ($pair in @($hostDiagnostics.pairedPhaseBaselineBindings)) {
        $pairProfileId = $pair.profileId
        $pairThroughputRunId = $pair.throughputRunId
        $phaseProfileId = $phaseProfile.profileId
        Assert-Stage5Condition ($pairProfileId -is [string] -and
            $phaseProfileId -is [string] -and
            $pairProfileId -ceq $phaseProfileId -and
            $pairThroughputRunId -is [string] -and
            $null -ne $pair.baselineRun -and
            -not $nativePhaseBaselineBySource.ContainsKey(
                $pairThroughputRunId)) `
            'Validated host qualification contains a duplicate or unexpected phase-baseline pair.'
        $nativePhaseBaselineBySource[$pairThroughputRunId] = $pair.baselineRun
    }
    Assert-Stage5Condition ($nativePhaseBaselineBySource.Count -eq
            $repeatCount + 1) `
        'Validated host qualification phase-baseline coverage differs from the selected measured source schedule.'
    $rowIndex = 0
    foreach ($scope in @('world', 'control')) {
        foreach ($phaseName in $phaseNames) {
            $elapsedValues = @()
            $serialValues = @()
            for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
            $sample = $phaseSamples[$rowIndex++]
            $context = "Stage 5 scaling phase raw sample $($rowIndex - 1)"
            Assert-Stage5JsonShape $sample @('scope', 'phase', 'repeat', 'sourceRunId',
                'sourceProcessId', 'sourceProcessCreationTimeUtc100ns',
                'sourceArgumentString', 'sourceCommandLine', 'baselineRunId',
                'baselineProcessId', 'baselineProcessCreationTimeUtc100ns',
                'baselineArgumentString', 'baselineCommandLine',
                'elapsedMilliseconds', 'serialMilliseconds', 'serialMillisecondsKnown') $context
            $receipt = $runReceipts["dense-eight-player|forced-one|$repeat"]
            $baselineRunId = Get-Stage5JsonValue $sample 'baselineRunId' $context
            $baselineProcessId = Get-Stage5JsonValue $sample 'baselineProcessId' $context
            $baselineProcessCreation = Get-Stage5JsonValue $sample `
                'baselineProcessCreationTimeUtc100ns' $context
            $baselineArgumentString = Get-Stage5JsonValue $sample `
                'baselineArgumentString' $context
            $baselineCommandLine = Get-Stage5JsonValue $sample `
                'baselineCommandLine' $context
            $elapsed = Get-Stage5JsonValue $sample 'elapsedMilliseconds' $context
            $serial = Get-Stage5JsonValue $sample 'serialMilliseconds' $context
            $sampleScope = Get-Stage5JsonValue $sample 'scope' $context
            $samplePhase = Get-Stage5JsonValue $sample 'phase' $context
            $sampleRepeat = Get-Stage5JsonValue $sample 'repeat' $context
            $sampleSourceRunId = Get-Stage5JsonValue $sample 'sourceRunId' $context
            $sampleSourceProcessId = Get-Stage5JsonValue $sample 'sourceProcessId' $context
            $sampleSourceProcessCreation = Get-Stage5JsonValue $sample `
                'sourceProcessCreationTimeUtc100ns' $context
            $sampleSourceArgumentString = Get-Stage5JsonValue $sample `
                'sourceArgumentString' $context
            $sampleSourceCommandLine = Get-Stage5JsonValue $sample `
                'sourceCommandLine' $context
            $sampleSerialKnown = Get-Stage5JsonValue $sample `
                'serialMillisecondsKnown' $context
            Assert-Stage5Condition ($sampleScope -is [string] -and
                $samplePhase -is [string] -and
                (Test-Stage5JsonInteger $sampleRepeat) -and
                $sampleSourceRunId -is [string] -and
                (Test-Stage5JsonInteger $sampleSourceProcessId) -and
                (Test-Stage5JsonInteger $sampleSourceProcessCreation) -and
                $sampleSourceArgumentString -is [string] -and
                $sampleSourceCommandLine -is [string] -and
                $sampleSerialKnown -is [bool]) `
                "$context source/phase identity fields must have JSON scalar types."
            $nativeBaseline = $nativePhaseBaselineBySource[[string]$receipt.runId]
            Assert-Stage5Condition ($null -ne $nativeBaseline) `
                "$context has no validated native phase-baseline pair."
            $phaseIndex = [Array]::IndexOf($phaseNames, $phaseName)
            $nativePhase = if ($scope -ceq 'world') {
                $nativeBaseline.phases[$phaseIndex]
            }
            else { $nativeBaseline.phaseAccounting.controlAccounting.phases[$phaseIndex] }
            [double]$expectedElapsed = [double]$nativePhase.totalNanoseconds / 1000000.0
            [double]$expectedSerial = [double]$nativePhase.serialNanoseconds / 1000000.0
            Assert-Stage5Condition ($sampleScope -ceq $scope -and
                $samplePhase -ceq $phaseName -and
                $sampleRepeat -eq $repeat -and
                $sampleSourceRunId -ceq $receipt.runId -and
                $sampleSourceProcessId -eq $receipt.processId -and
                $sampleSourceProcessCreation -eq $receipt.processCreationTimeUtc100ns -and
                $sampleSourceArgumentString -ceq $receipt.argumentString -and
                $sampleSourceCommandLine -ceq $receipt.commandLine -and
                $baselineRunId -is [string] -and
                    $baselineRunId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and
                    -not $baselineRunId.Contains('..') -and
                (Test-Stage5JsonInteger $baselineProcessId) -and
                    [Int64]$baselineProcessId -gt 0 -and
                (Test-Stage5JsonInteger $baselineProcessCreation) -and
                    [Int64]$baselineProcessCreation -gt 0 -and
                "$baselineProcessId|$baselineProcessCreation" -cne
                    "$($receipt.processId)|$($receipt.processCreationTimeUtc100ns)" -and
                $baselineArgumentString -is [string] -and
                    $baselineArgumentString -ceq $receipt.argumentString -and
                $baselineCommandLine -is [string] -and
                    $baselineCommandLine -ceq $receipt.commandLine -and
                (Test-Stage5JsonNumber $elapsed) -and [double]$elapsed -gt 0.0 -and
                (Test-Stage5JsonNumber $serial) -and [double]$serial -ge 0.0 -and
                $sampleSerialKnown -and
                [double]$serial -le [double]$elapsed -and
                $null -ne $nativeBaseline -and
                [string]$nativeBaseline.runId -ceq [string]$baselineRunId -and
                [Int64]$nativeBaseline.processId -eq [Int64]$baselineProcessId -and
                [Int64]$nativeBaseline.processCreationTimeUtc100ns -eq
                    [Int64]$baselineProcessCreation -and
                [string]$nativeBaseline.argumentString -ceq
                    [string]$baselineArgumentString -and
                [string]$nativeBaseline.commandLine -ceq
                    [string]$baselineCommandLine -and
                [string]$nativeBaseline.measurementRole -ceq
                    'phase-serial-baseline' -and
                $null -ne $nativeBaseline.phaseAccounting -and
                [string]$nativePhase.name -ceq [string]$phaseName -and
                [bool]$nativePhase.serialNanosecondsKnown -and
                [Math]::Abs([double]$elapsed - $expectedElapsed) -le 0.0001 -and
                [Math]::Abs([double]$serial - $expectedSerial) -le 0.0001) `
                "$context is not correlated with its exact installed one-worker run."
            if ($phaseBaselineReceipts.ContainsKey($repeat)) {
                $baselineReceipt = $phaseBaselineReceipts[$repeat]
                Assert-Stage5Condition ($baselineReceipt.runId -ceq $baselineRunId -and
                    $baselineReceipt.processId -eq $baselineProcessId -and
                    $baselineReceipt.processCreationTimeUtc100ns -eq
                        $baselineProcessCreation -and
                    $baselineReceipt.argumentString -ceq $baselineArgumentString -and
                    $baselineReceipt.commandLine -ceq $baselineCommandLine) `
                    "$context changes its paired phase-baseline process identity."
            }
            else {
                $baselineProcessIdentity = "$baselineProcessId|$baselineProcessCreation"
                Assert-Stage5Condition (-not $seenProcesses.ContainsKey(
                        $baselineProcessIdentity)) `
                    "$context reuses a throughput or oracle process identity."
                $seenProcesses[$baselineProcessIdentity] = $true
                $phaseBaselineReceipts[$repeat] = [pscustomobject]@{
                    runId = [string]$baselineRunId
                    processId = [Int64]$baselineProcessId
                    processCreationTimeUtc100ns = [Int64]$baselineProcessCreation
                    argumentString = [string]$baselineArgumentString
                    commandLine = [string]$baselineCommandLine
                }
            }
                $elapsedValues += [double]$elapsed
                $serialValues += [double]$serial
                if ($scope -ceq 'world') {
                    $worldPhaseTotals[$repeat] += [double]$elapsed
                    $worldPhaseSerialTotals[$repeat] += [double]$serial
                }
                else {
                    $controlPhaseTotals[$repeat] += [double]$elapsed
                    $controlPhaseSerialTotals[$repeat] += [double]$serial
                }
            }
            if ($scope -ceq 'world') {
                $phaseAggregates += [pscustomobject]@{
                    name = $phaseName
                    elapsedMilliseconds = Get-Stage5Median $elapsedValues
                    serialMilliseconds = Get-Stage5Median $serialValues
                    serialMillisecondsKnown = $true
                }
            }
        }
    }

    $phaseAccountingSamples = Get-Stage5JsonValue $document `
        'phaseAccountingSamples' 'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($phaseAccountingSamples -is [Array] -and
        $phaseAccountingSamples.Count -eq $repeatCount) `
        'Stage 5 scaling raw samples require exact whole-frame phase accounting for every measured repeat.'
    [double]$worstSerialFraction = -1.0
    [double]$amdahlTotalOne = 0.0
    [double]$amdahlTotalSerial = 0.0
    for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
        $sample = $phaseAccountingSamples[$repeat]
        $context = "Stage 5 whole-frame phase-accounting sample $repeat"
        Assert-Stage5JsonShape $sample @('repeat', 'sourceRunId',
            'sourceProcessId', 'sourceProcessCreationTimeUtc100ns',
            'sourceArgumentString', 'sourceCommandLine', 'baselineRunId',
            'baselineProcessId', 'baselineProcessCreationTimeUtc100ns',
            'baselineArgumentString', 'baselineCommandLine', 'accountingOrigin',
            'worldMilliseconds', 'controlMilliseconds',
            'completionSerialMilliseconds', 'worldUnscopedSerialMilliseconds',
            'controlUnscopedSerialMilliseconds',
            'totalOneWorkerMilliseconds', 'totalSerialMilliseconds') $context
        $receipt = $runReceipts["dense-eight-player|forced-one|$repeat"]
        $baselineReceipt = $phaseBaselineReceipts[$repeat]
        Assert-Stage5Condition ((Get-Stage5JsonValue $sample 'repeat' $context) -eq $repeat -and
            (Get-Stage5JsonValue $sample 'sourceRunId' $context) -ceq $receipt.runId -and
            (Get-Stage5JsonValue $sample 'sourceProcessId' $context) -eq
                $receipt.processId -and
            (Get-Stage5JsonValue $sample 'sourceProcessCreationTimeUtc100ns' $context) -eq
                $receipt.processCreationTimeUtc100ns -and
            (Get-Stage5JsonValue $sample 'sourceArgumentString' $context) -ceq
                $receipt.argumentString -and
            (Get-Stage5JsonValue $sample 'sourceCommandLine' $context) -ceq
                $receipt.commandLine -and
            (Get-Stage5JsonValue $sample 'baselineRunId' $context) -ceq
                $baselineReceipt.runId -and
            (Get-Stage5JsonValue $sample 'baselineProcessId' $context) -eq
                $baselineReceipt.processId -and
            (Get-Stage5JsonValue $sample 'baselineProcessCreationTimeUtc100ns' $context) -eq
                $baselineReceipt.processCreationTimeUtc100ns -and
            (Get-Stage5JsonValue $sample 'baselineArgumentString' $context) -ceq
                $baselineReceipt.argumentString -and
            (Get-Stage5JsonValue $sample 'baselineCommandLine' $context) -ceq
                $baselineReceipt.commandLine -and
            (Get-Stage5JsonValue $sample 'accountingOrigin' $context) -ceq
                'kernel-performance-ledger-whole-frame-v1') `
            "$context changes its exact paired source/baseline identity."
        $values = @{}
        foreach ($name in @('worldMilliseconds', 'controlMilliseconds',
                'completionSerialMilliseconds', 'worldUnscopedSerialMilliseconds',
                'controlUnscopedSerialMilliseconds',
                'totalOneWorkerMilliseconds', 'totalSerialMilliseconds')) {
            $value = Get-Stage5JsonValue $sample $name $context
            Assert-Stage5Condition ((Test-Stage5JsonNumber $value) -and
                [double]$value -ge 0.0 -and -not [double]::IsInfinity([double]$value) -and
                -not [double]::IsNaN([double]$value)) `
                "$context field '$name' is not a finite non-negative observation."
            $values[$name] = [double]$value
        }
        [double]$computedOne = $values.worldMilliseconds +
            $values.controlMilliseconds + $values.completionSerialMilliseconds
        [double]$computedSerial = $values.worldUnscopedSerialMilliseconds +
            $values.controlUnscopedSerialMilliseconds +
            $worldPhaseSerialTotals[$repeat] +
            $controlPhaseSerialTotals[$repeat] +
            $values.completionSerialMilliseconds
        $nativeBaseline = $nativePhaseBaselineBySource[[string]$receipt.runId]
        $nativeAccounting = $nativeBaseline.phaseAccounting
        [double]$nativeWorld = [double]$nativeAccounting.frameNanoseconds / 1000000.0
        [double]$nativeControl = [double]$nativeAccounting.controlAccounting.totalNanoseconds / 1000000.0
        [double]$nativeCompletion = [double]$nativeAccounting.completionSerialNanoseconds / 1000000.0
        [double]$nativeWorldUnscoped = [double]$nativeAccounting.unscopedSerialNanoseconds / 1000000.0
        [double]$nativeControlUnscoped = [double]$nativeAccounting.controlAccounting.unscopedSerialNanoseconds / 1000000.0
        Assert-Stage5Condition ($values.worldMilliseconds -gt 0.0 -and
            $values.totalOneWorkerMilliseconds -gt 0.0 -and
            [Math]::Abs($values.totalOneWorkerMilliseconds - $computedOne) -le 0.0001 -and
            [Math]::Abs($values.totalSerialMilliseconds - $computedSerial) -le 0.0001 -and
            $values.totalSerialMilliseconds -le $values.totalOneWorkerMilliseconds -and
            [Math]::Abs(($worldPhaseTotals[$repeat] +
                    $values.worldUnscopedSerialMilliseconds) -
                $values.worldMilliseconds) -le 0.0001 -and
            [Math]::Abs(($controlPhaseTotals[$repeat] +
                    $values.controlUnscopedSerialMilliseconds) -
                $values.controlMilliseconds) -le 0.0001 -and
            [Math]::Abs($values.worldMilliseconds - $nativeWorld) -le 0.0001 -and
            [Math]::Abs($values.controlMilliseconds - $nativeControl) -le 0.0001 -and
            [Math]::Abs($values.completionSerialMilliseconds - $nativeCompletion) -le 0.0001 -and
            [Math]::Abs($values.worldUnscopedSerialMilliseconds -
                $nativeWorldUnscoped) -le 0.0001 -and
            [Math]::Abs($values.controlUnscopedSerialMilliseconds -
                $nativeControlUnscoped) -le 0.0001) `
            "$context omits, overlaps, or misattributes whole-frame serial work."
        [double]$fraction = $values.totalSerialMilliseconds /
            $values.totalOneWorkerMilliseconds
        if ($fraction -gt $worstSerialFraction) {
            $worstSerialFraction = $fraction
            $amdahlTotalOne = $values.totalOneWorkerMilliseconds
            $amdahlTotalSerial = $values.totalSerialMilliseconds
        }
    }

    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelSamples = Get-Stage5JsonValue $document 'kernelSamples' `
        'Stage 5 scaling raw-sample manifest'
    Assert-Stage5Condition ($kernelSamples -is [Array] -and
        $kernelSamples.Count -eq $kernelNames.Count * $repeatCount) `
        'Stage 5 scaling raw samples require every kernel timing for every repeat.'
    $kernelAggregates = @()
    $parts = @('captureMilliseconds', 'scheduleMilliseconds', 'waitMilliseconds',
        'validateMilliseconds', 'commitMilliseconds')
    $nativeOracleBySource = @{}
    foreach ($pair in @($hostDiagnostics.pairedOracleBindings)) {
        Assert-Stage5Condition ($pair.throughputRunId -is [string] -and
            $null -ne $pair.oracleRun -and
            -not $nativeOracleBySource.ContainsKey([string]$pair.throughputRunId)) `
            'Validated host qualification contains a duplicate or malformed paired serial oracle.'
        $nativeOracleBySource[[string]$pair.throughputRunId] = $pair.oracleRun
    }
    Assert-Stage5Condition ($nativeOracleBySource.Count -eq
            $nativeThroughputByKey.Count) `
        'Validated host qualification does not pair every throughput run with a serial oracle.'
    $rowIndex = 0
    $kernelOracleReceipts = @{}
    foreach ($kernelName in $kernelNames) {
        $admittedValues = @()
        $parallelValues = @()
        $partValues = @{}
        foreach ($part in $parts) { $partValues[$part] = @() }
        $serialValues = @()
        for ($repeat = 0; $repeat -lt $repeatCount; ++$repeat) {
            $sample = $kernelSamples[$rowIndex++]
            $context = "Stage 5 scaling kernel raw sample $($rowIndex - 1)"
            Assert-Stage5JsonShape $sample @('kernel', 'repeat', 'runId',
                'processId', 'processCreationTimeUtc100ns', 'argumentString',
                'commandLine', 'oracleRunId', 'oracleProcessId',
                'oracleProcessCreationTimeUtc100ns', 'oracleArgumentString',
                'oracleCommandLine',
                'admittedSlices', 'captureMilliseconds', 'scheduleMilliseconds',
                'waitMilliseconds', 'validateMilliseconds', 'commitMilliseconds',
                'exactSerialOperationMilliseconds', 'exactSerialOperationMillisecondsKnown',
                'timingAttribution') $context
            $receipt = $runReceipts["dense-eight-player|physical-8|$repeat"]
            $nativeRun = $nativeThroughputByKey[
                "dense-eight-player|physical-8|$($repeat + 1)"]
            $nativeOracle = $nativeOracleBySource[[string]$receipt.runId]
            Assert-Stage5Condition ($null -ne $nativeRun -and
                $null -ne $nativeOracle) `
                "$context has no validated native throughput/oracle pair."
            $admitted = Get-Stage5JsonValue $sample 'admittedSlices' $context
            $serial = Get-Stage5JsonValue $sample 'exactSerialOperationMilliseconds' $context
            $oracleRunId = Get-Stage5JsonValue $sample 'oracleRunId' $context
            $oracleProcessId = Get-Stage5JsonValue $sample 'oracleProcessId' $context
            $oracleProcessCreation = Get-Stage5JsonValue $sample `
                'oracleProcessCreationTimeUtc100ns' $context
            $oracleArgumentString = Get-Stage5JsonValue $sample `
                'oracleArgumentString' $context
            $oracleCommandLine = Get-Stage5JsonValue $sample 'oracleCommandLine' $context
            $oracleProcessIdentity = "$oracleProcessId|$oracleProcessCreation"
            $nativeTimingStreams = @($nativeRun.kernelTiming.streams | Where-Object {
                $_.name -ceq $kernelName
            })
            $nativeOracleStreams = @($nativeOracle.kernelReference.streams | Where-Object {
                $_.name -ceq $kernelName
            })
            [UInt64]$expectedAdmitted = 0
            $expectedParts = @{}
            foreach ($part in $parts) { $expectedParts[$part] = [decimal]0 }
            foreach ($stream in $nativeTimingStreams) {
                $expectedAdmitted += [UInt64]$stream.admittedBatches
                foreach ($nativeStage in $stream.stages) {
                    $partName = [string]$nativeStage.name + 'Milliseconds'
                    Assert-Stage5Condition ($expectedParts.ContainsKey($partName)) `
                        "$context native timing stream has an unexpected stage."
                    $expectedParts[$partName] += [decimal]$nativeStage.totalNanoseconds / 1000000
                }
            }
            [decimal]$expectedSerial = 0
            foreach ($stream in $nativeOracleStreams) {
                $expectedSerial += [decimal]$stream.serialNanoseconds / 1000000
            }
            Assert-Stage5Condition ((Get-Stage5JsonValue $sample 'kernel' $context) -ceq $kernelName -and
                (Get-Stage5JsonValue $sample 'repeat' $context) -eq $repeat -and
                (Get-Stage5JsonValue $sample 'runId' $context) -ceq $receipt.runId -and
                (Get-Stage5JsonValue $sample 'processId' $context) -eq $receipt.processId -and
                (Get-Stage5JsonValue $sample 'processCreationTimeUtc100ns' $context) -eq
                    $receipt.processCreationTimeUtc100ns -and
                (Get-Stage5JsonValue $sample 'argumentString' $context) -ceq
                    $receipt.argumentString -and
                (Get-Stage5JsonValue $sample 'commandLine' $context) -ceq $receipt.commandLine -and
                $oracleRunId -is [string] -and
                $oracleRunId -cmatch
                    '^[A-Za-z0-9_.-]{1,256}$' -and
                $oracleRunId -cne $receipt.runId -and
                (Test-Stage5JsonInteger $oracleProcessId) -and
                [Int64]$oracleProcessId -gt 0 -and
                (Test-Stage5JsonInteger $oracleProcessCreation) -and
                [Int64]$oracleProcessCreation -gt 0 -and
                $oracleProcessIdentity -cne
                    "$($receipt.processId)|$($receipt.processCreationTimeUtc100ns)" -and
                $oracleArgumentString -ceq $receipt.argumentString -and
                $oracleCommandLine -ceq $receipt.commandLine -and
                (Test-Stage5JsonInteger $admitted) -and [int]$admitted -gt 0 -and
                (Get-Stage5JsonValue $sample 'exactSerialOperationMillisecondsKnown' $context) -is [bool] -and
                (Get-Stage5JsonValue $sample 'exactSerialOperationMillisecondsKnown' $context) -and
                (Get-Stage5JsonValue $sample 'timingAttribution' $context) -ceq 'owner-stack-exclusive-v1' -and
                (Test-Stage5JsonNumber $serial) -and [double]$serial -gt 0.0 -and
                $nativeTimingStreams.Count -gt 0 -and
                $nativeOracleStreams.Count -eq $nativeTimingStreams.Count -and
                [UInt64]$admitted -eq $expectedAdmitted -and
                [string]$nativeOracle.runId -ceq [string]$oracleRunId -and
                [Int64]$nativeOracle.processId -eq [Int64]$oracleProcessId -and
                [Int64]$nativeOracle.processCreationTimeUtc100ns -eq
                    [Int64]$oracleProcessCreation -and
                [string]$nativeOracle.argumentString -ceq
                    [string]$oracleArgumentString -and
                [string]$nativeOracle.commandLine -ceq [string]$oracleCommandLine -and
                [Math]::Abs([double]$serial - [double]$expectedSerial) -le 0.0001) `
                "$context is not correlated with its exact installed physical-8 run."
            if ($kernelOracleReceipts.ContainsKey($repeat)) {
                $oracleReceipt = $kernelOracleReceipts[$repeat]
                Assert-Stage5Condition ($oracleReceipt.runId -ceq $oracleRunId -and
                    $oracleReceipt.processId -eq $oracleProcessId -and
                    $oracleReceipt.processCreationTimeUtc100ns -eq
                        $oracleProcessCreation -and
                    $oracleReceipt.argumentString -ceq $oracleArgumentString -and
                    $oracleReceipt.commandLine -ceq $oracleCommandLine) `
                    "$context changes the paired serial-oracle process identity."
            }
            else {
                Assert-Stage5Condition (-not $seenProcesses.ContainsKey(
                        $oracleProcessIdentity)) `
                    "$context reuses a throughput or phase-baseline process identity."
                $seenProcesses[$oracleProcessIdentity] = $true
                $kernelOracleReceipts[$repeat] = [pscustomobject]@{
                    runId = [string]$oracleRunId
                    processId = [Int64]$oracleProcessId
                    processCreationTimeUtc100ns = [Int64]$oracleProcessCreation
                    argumentString = [string]$oracleArgumentString
                    commandLine = [string]$oracleCommandLine
                }
            }
            $admittedValues += [double]$admitted
            [double]$runParallel = 0.0
            foreach ($part in $parts) {
                $value = Get-Stage5JsonValue $sample $part $context
                Assert-Stage5Condition ((Test-Stage5JsonNumber $value) -and [double]$value -ge 0.0) `
                    "$context $part is invalid."
                Assert-Stage5Condition ([Math]::Abs([double]$value -
                        [double]$expectedParts[$part]) -le 0.0001) `
                    "$context $part differs from the validated native timing streams."
                $partValues[$part] += [double]$value
                $runParallel += [double]$value
            }
            Assert-Stage5Condition ($runParallel -gt 0.0 -and
                -not [double]::IsInfinity($runParallel)) `
                "$context has no finite positive active pipeline cost."
            $parallelValues += $runParallel
            $serialValues += [double]$serial
        }
        $capture = Get-Stage5Median $partValues.captureMilliseconds
        $schedule = Get-Stage5Median $partValues.scheduleMilliseconds
        $wait = Get-Stage5Median $partValues.waitMilliseconds
        $validate = Get-Stage5Median $partValues.validateMilliseconds
        $commit = Get-Stage5Median $partValues.commitMilliseconds
        # Components can be anticorrelated between runs. Their separate
        # medians are descriptive, never an observed total pipeline cost.
        $parallelTotal = Get-Stage5Median $parallelValues
        $serialOperation = Get-Stage5Median $serialValues
        $kernelAggregates += [pscustomobject]@{
            name = $kernelName
            admittedSlices = [int](Get-Stage5Median $admittedValues)
            captureMilliseconds = $capture
            scheduleMilliseconds = $schedule
            waitMilliseconds = $wait
            validateMilliseconds = $validate
            commitMilliseconds = $commit
            totalParallelMilliseconds = $parallelTotal
            exactSerialOperationMilliseconds = $serialOperation
            exactSerialOperationMillisecondsKnown = $true
            timingAttribution = 'owner-stack-exclusive-v1'
            netSpeedup = $serialOperation / $parallelTotal
        }
    }
    return [pscustomobject]@{
        manifestSha256 = [string]$Snapshot.sha256
        recordedUtc = [string]$recordedUtc
        cohortNonce = [string]$cohortNonce
        cohortCreatedUtc = [string]$cohortCreatedUtc
        qualificationMode = 'External16Core'
        referencePolicy = 'paired-serial-oracle-v1'
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = [string]$runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$runtimeClosure.closureSha256
        }
        stage3SourceCommit = [string]$stage3SourceCommit
        stage3ExecutableSha256 = [string]$stage3ExecutableSha256
        hostQualification = [pscustomobject]@{
            path = [string](Get-Stage5JsonValue $hostEntry 'path' `
                'Stage 5 scaling host qualification reference')
            sha256 = [string]$hostSha256
        }
        phaseBaselineProfile = [pscustomobject]@{
            path = [string](Get-Stage5JsonValue $phaseProfileEntry 'path' `
                'Stage 5 scaling phase-baseline profile reference')
            sha256 = [string](Get-Stage5JsonValue $phaseProfileEntry 'sha256' `
                'Stage 5 scaling phase-baseline profile reference')
        }
        performanceData = [pscustomobject]@{
            path = [string](Get-Stage5JsonValue $performanceDataEntry 'path' `
                'Stage 5 scaling performance qualification-data reference')
            sha256 = [string]$performanceData.sha256
            closureSha256 = [string]$performanceData.closureSha256
            fileCount = [int]$performanceData.fileCount
        }
        stage3Baseline = [pscustomobject]@{
            path = [string](Get-Stage5JsonValue $stage3Entry 'path' `
                'Stage 3 protected scaling baseline reference')
            sha256 = [string]$ExpectedStage3BaselineSha256
        }
        topologySha256 = $topologySha256
        topology = $topology
        repeatCount = $repeatCount
        stage3RepeatCount = $stage3RepeatCount
        totalOneWorkerMilliseconds = $amdahlTotalOne
        totalSerialMilliseconds = $amdahlTotalSerial
        phases = $phaseAggregates
        kernels = $kernelAggregates
        fixtures = $fixtureAggregates
    }
}

function Assert-Stage5PhaseAccountingContract {
    param([object]$Receipt, [string]$Context)
    # The runner supplies PSCustomObject JSON; the immutable converter supplies
    # dictionaries. Validate either representation without a lossy JSON roundtrip.
    $shape = {
        param($value, [string[]]$names, [string]$label)
        Assert-Stage5Condition ($value -is [Collections.IDictionary] -or
            $value -is [pscustomobject]) "$label must be an object."
        $keys = if ($value -is [Collections.IDictionary]) { @($value.Keys) }
            else { @($value.PSObject.Properties.Name) }
        Assert-Stage5Condition ($keys.Count -eq $names.Count) "$label has an invalid shape."
        foreach ($name in $names) {
            Assert-Stage5Condition (@($keys | Where-Object { $_ -ceq $name }).Count -eq 1) `
                "$label lacks exact field '$name'."
        }
    }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        $Receipt.schemaVersion -eq 6 -and
        $Receipt.producer -ceq 'game-executable-stage5-performance-report-v6' -and
        $Receipt.producerVersion -ceq '6' -and
        $Receipt.measurementRole -ceq 'phase-serial-baseline') `
        "$Context requires the exact V6 phase-baseline role."
    $phase = $Receipt.phaseAccounting
    & $shape $phase @('schemaVersion','mode','accountingOrigin','frozen','complete','errors',
        'completedFrameCount','firstCompletedFrame','lastCompletedFrame','frameNanoseconds',
        'maximumFrameNanoseconds','unscopedSerialNanoseconds','completionSerialNanoseconds',
        'completionSampleCount','schedulerClosureKnown','schedulerBegin','schedulerEnd',
        'controlAccounting') "$Context phase accounting"
    Assert-Stage5Condition ((Test-Stage5JsonInteger $phase.schemaVersion) -and
        $phase.schemaVersion -eq 1 -and
        $phase.mode -ceq 'owner-inline-source-admissions-v1' -and
        $phase.accountingOrigin -ceq 'kernel-performance-ledger-whole-frame-v1' -and
        $phase.frozen -is [bool] -and $phase.frozen -and
        $phase.complete -is [bool] -and $phase.complete -and
        $phase.schedulerClosureKnown -is [bool] -and $phase.schedulerClosureKnown) `
        "$Context phase accounting is not frozen and closed."
    foreach ($name in @('schemaVersion','errors','completedFrameCount','firstCompletedFrame',
            'lastCompletedFrame','frameNanoseconds','maximumFrameNanoseconds',
            'unscopedSerialNanoseconds','completionSerialNanoseconds','completionSampleCount')) {
        Assert-Stage5DiagnosticCounter $phase.$name "$Context $name"
    }
    foreach ($name in @('sampleCount','firstFrame','lastFrame')) {
        Assert-Stage5DiagnosticCounter $Receipt.workload.$name "$Context workload $name"
    }
    Assert-Stage5Condition ($phase.errors -eq 0 -and $phase.completedFrameCount -gt 0 -and
        $phase.firstCompletedFrame -gt 0 -and $phase.lastCompletedFrame -le [UInt32]::MaxValue -and
        [decimal]$phase.lastCompletedFrame - [decimal]$phase.firstCompletedFrame + 1 -eq
            [decimal]$phase.completedFrameCount -and
        $phase.completedFrameCount -eq $Receipt.workload.sampleCount -and
        $phase.firstCompletedFrame -eq $Receipt.workload.firstFrame -and
        $phase.lastCompletedFrame -eq $Receipt.workload.lastFrame -and
        $phase.frameNanoseconds -gt 0 -and
        $phase.maximumFrameNanoseconds -le $phase.frameNanoseconds -and
        ($phase.completionSerialNanoseconds -eq 0 -or $phase.completionSampleCount -gt 0)) `
        "$Context world/completion coverage is inconsistent."
    $schedulerFields = @('submittedJobs','executedJobs','ownerHelpJobs','outstandingJobs','pendingJobs')
    foreach ($boundary in @($phase.schedulerBegin, $phase.schedulerEnd)) {
        & $shape $boundary $schedulerFields "$Context scheduler boundary"
        foreach ($name in $schedulerFields) {
            Assert-Stage5DiagnosticCounter $boundary.$name "$Context scheduler $name"
        }
        Assert-Stage5Condition ($boundary.outstandingJobs -eq 0 -and $boundary.pendingJobs -eq 0) `
            "$Context retains outstanding or pending work."
    }
    foreach ($name in @('submittedJobs','executedJobs','ownerHelpJobs')) {
        Assert-Stage5Condition ($phase.schedulerBegin.$name -eq $phase.schedulerEnd.$name) `
            "$Context scheduler $name changed or reset inside the baseline."
    }
    $control = $phase.controlAccounting
    & $shape $control @('windowCount','firstSampleOrdinal','lastSampleOrdinal','totalNanoseconds',
        'maximumNanoseconds','unscopedSerialNanoseconds','phases') "$Context control accounting"
    foreach ($name in @('windowCount','firstSampleOrdinal','lastSampleOrdinal','totalNanoseconds',
            'maximumNanoseconds','unscopedSerialNanoseconds')) {
        Assert-Stage5DiagnosticCounter $control.$name "$Context control $name"
    }
    Assert-Stage5Condition ($control.maximumNanoseconds -le $control.totalNanoseconds -and
        (($control.windowCount -eq 0 -and $control.firstSampleOrdinal -eq 0 -and
            $control.lastSampleOrdinal -eq 0 -and $control.totalNanoseconds -eq 0 -and
            $control.unscopedSerialNanoseconds -eq 0) -or
         ($control.windowCount -gt 0 -and $control.firstSampleOrdinal -gt 0 -and
            $control.lastSampleOrdinal -ge $control.firstSampleOrdinal -and
            [decimal]$control.lastSampleOrdinal - [decimal]$control.firstSampleOrdinal + 1 -eq
                [decimal]$control.windowCount -and $control.totalNanoseconds -gt 0))) `
        "$Context control-window count/ordinal/extent is invalid."
    $phaseNames = @('owner-intake','legacy-mutable-island','spatial-work','owner-tail','verification-publication')
    [decimal]$serial = 0; [decimal]$pure = 0
    foreach ($scope in @(
            @{ rows=$Receipt.phases; count=$phase.completedFrameCount; total=$phase.frameNanoseconds; unscoped=$phase.unscopedSerialNanoseconds; name='world' },
            @{ rows=$control.phases; count=$control.windowCount; total=$control.totalNanoseconds; unscoped=$control.unscopedSerialNanoseconds; name='control' })) {
        Assert-Stage5Condition ($scope.rows -is [Array] -and $scope.rows.Count -eq 5) `
            "$Context $($scope.name) requires exactly five phases."
        [decimal]$total = $scope.unscoped
        $serial += [decimal]$scope.unscoped
        for ($index = 0; $index -lt 5; ++$index) {
            $row = $scope.rows[$index]
            & $shape $row @('name','available','totalNanoseconds','maximumNanoseconds','sampleCount',
                'serialNanoseconds','serialNanosecondsKnown','pureNanoseconds','pureNanosecondsKnown') `
                "$Context $($scope.name) phase $index"
            foreach ($name in @('totalNanoseconds','maximumNanoseconds','sampleCount','serialNanoseconds','pureNanoseconds')) {
                Assert-Stage5DiagnosticCounter $row.$name "$Context phase $name"
            }
            Assert-Stage5Condition ($row.name -ceq $phaseNames[$index] -and
                $row.available -is [bool] -and $row.available -eq ($scope.count -gt 0) -and
                $row.serialNanosecondsKnown -is [bool] -and $row.serialNanosecondsKnown -and
                $row.pureNanosecondsKnown -is [bool] -and $row.pureNanosecondsKnown -and
                $row.sampleCount -eq $scope.count -and $row.maximumNanoseconds -le $row.totalNanoseconds -and
                ($scope.count -gt 0 -or $row.totalNanoseconds -eq 0) -and
                [decimal]$row.totalNanoseconds -eq
                    ([decimal]$row.serialNanoseconds + [decimal]$row.pureNanoseconds)) `
                "$Context $($scope.name) phase partition is invalid."
            $total += [decimal]$row.totalNanoseconds
            $serial += [decimal]$row.serialNanoseconds
            $pure += [decimal]$row.pureNanoseconds
        }
        Assert-Stage5Condition ($total -eq [decimal]$scope.total) `
            "$Context $($scope.name) phases omit or overlap unscoped owner work."
    }
    $serial += [decimal]$phase.completionSerialNanoseconds
    [decimal]$accounted = [decimal]$phase.frameNanoseconds +
        [decimal]$control.totalNanoseconds + [decimal]$phase.completionSerialNanoseconds
    Assert-Stage5Condition ($accounted -le [decimal][UInt64]::MaxValue -and
        $accounted -eq $serial + $pure) "$Context accounted time overflows or is not partitioned."
    return [pscustomobject]@{
        completedFrameCount=[UInt64]$phase.completedFrameCount
        frameNanoseconds=[UInt64]$phase.frameNanoseconds
        controlNanoseconds=[UInt64]$control.totalNanoseconds
        completionSerialNanoseconds=[UInt64]$phase.completionSerialNanoseconds
        accountedNanoseconds=[UInt64]$accounted
        serialNanoseconds=[UInt64]$serial; pureNanoseconds=[UInt64]$pure
    }
}

function Assert-Stage5DiagnosticCounter {
    param([object]$Value, [string]$Context)
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Value) -and
        [decimal]$Value -ge 0 -and [decimal]$Value -le [decimal][UInt64]::MaxValue) `
        "$Context must be an exact unsigned 64-bit counter."
}

function Assert-Stage5DiagnosticRealMulticoreKernelEvidence {
    param([object]$Document, [string]$Context)
    $canonicalNames = @('physics', 'status', 'collision', 'ai-planning',
        'spatial', 'path')
    $streams = @($Document.kernelTiming.streams)
    $streamFamilies = @($streams | ForEach-Object { [string]$_.name } |
        Sort-Object -Unique)
    Assert-Stage5Condition ($streams.Count -ge 6 -and $streams.Count -le 8 -and
        @($streams | Where-Object { $_.committedBatches -le 0 }).Count -eq 0 -and
        $streamFamilies.Count -eq $canonicalNames.Count -and
        @($canonicalNames | Where-Object { $streamFamilies -cnotcontains $_ }).Count -eq 0) `
        "$Context does not contain 6-8 committed unique timing streams covering all six kernel families."

    Assert-Stage5Condition ($Document.kernels -is [Array] -and
        $Document.kernels.Count -eq $canonicalNames.Count) `
        "$Context does not contain the six canonical kernel worker records."
    [UInt64]$effectiveWorkers = $Document.worker.effectiveCount
    [UInt64]$workerOrdinalMask = if ($effectiveWorkers -ge 64) {
        [UInt64]::MaxValue
    } else { ([UInt64]1 -shl [int]$effectiveWorkers) - 1 }
    for ($index = 0; $index -lt $canonicalNames.Count; ++$index) {
        $kernel = $Document.kernels[$index]
        Assert-Stage5JsonShape $kernel @('name','available','submittedJobs',
            'completedJobs','physicalWorkerJobs','ownerHelpedJobs',
            'physicalWorkerMask','distinctPhysicalWorkers',
            'physicalWorkerMaskComplete','elapsedNanoseconds',
            'elapsedNanosecondsKnown') "$Context kernel $index"
        foreach ($field in @('submittedJobs','completedJobs',
                'physicalWorkerJobs','ownerHelpedJobs','physicalWorkerMask',
                'distinctPhysicalWorkers','elapsedNanoseconds')) {
            Assert-Stage5DiagnosticCounter $kernel[$field] `
                "$Context kernel '$($canonicalNames[$index])' $field"
        }
        [UInt64]$submitted = $kernel.submittedJobs
        [UInt64]$completed = $kernel.completedJobs
        [UInt64]$physical = $kernel.physicalWorkerJobs
        [UInt64]$ownerHelped = $kernel.ownerHelpedJobs
        [UInt64]$mask = $kernel.physicalWorkerMask
        [UInt64]$distinct = $kernel.distinctPhysicalWorkers
        [UInt64]$maskBitCount = Get-Stage5UInt64BitCount $mask
        Assert-Stage5Condition ($kernel.name -is [string] -and
            $kernel.name -ceq $canonicalNames[$index] -and
            $kernel.available -is [bool] -and $kernel.available -and
            $kernel.physicalWorkerMaskComplete -is [bool] -and
            $kernel.physicalWorkerMaskComplete -and
            $kernel.elapsedNanosecondsKnown -is [bool] -and
            $submitted -gt 0 -and $completed -eq $submitted -and
            $physical -gt 0 -and
            [decimal]$physical + [decimal]$ownerHelped -eq [decimal]$completed -and
            $mask -ne 0 -and ($mask -band $workerOrdinalMask) -eq $mask -and
            $distinct -gt 0 -and $distinct -le $maskBitCount -and
            $maskBitCount -le $physical -and $distinct -le $physical -and
            $distinct -le $effectiveWorkers) `
            "$Context kernel '$($canonicalNames[$index])' lacks reconciled physical-worker counters/mask evidence."
    }
}

function Assert-Stage5DiagnosticKernelTiming {
    param([object]$Timing, [object]$Frames, [string]$MeasurementRole,
        [string]$Context)
    Assert-Stage5JsonShape $Timing @('schemaVersion', 'mode', 'attribution', 'enabled',
        'frozen', 'complete', 'errors', 'generation', 'serialReferenceKnown', 'streams') $Context
    Assert-Stage5DiagnosticCounter $Timing.generation "$Context generation"
    $isBaseline = $MeasurementRole -ceq 'phase-serial-baseline'
    $expectedMode = if ($isBaseline) { 'owner-inline-baseline-observation' }
        else { 'owner-pipeline-observation' }
    $expectedAttribution = if ($isBaseline) {
        'owner-inline-baseline-exclusive-v1'
    } else { 'owner-stack-exclusive-v1' }
    Assert-Stage5Condition (@('throughput','serial-oracle','phase-serial-baseline') `
            -ccontains $MeasurementRole -and
        (Test-Stage5JsonInteger $Timing.schemaVersion) -and
        $Timing.schemaVersion -eq 1 -and
        $Timing.mode -ceq $expectedMode -and
        $Timing.attribution -ceq $expectedAttribution -and
        $Timing.enabled -is [bool] -and $Timing.enabled -and
        $Timing.frozen -is [bool] -and $Timing.frozen -and
        $Timing.complete -is [bool] -and
        (Test-Stage5JsonInteger $Timing.errors) -and $Timing.errors -eq 0 -and
        [decimal]$Timing.generation -gt 0 -and
        $Timing.serialReferenceKnown -is [bool] -and -not $Timing.serialReferenceKnown -and
        $Timing.streams -is [Array] -and $Timing.streams.Count -le 16 -and
        $Timing.complete -eq ($Timing.streams.Count -gt 0)) `
        "$Context is not a finalized role-consistent timing ledger."
    $names = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $stages = @('capture', 'schedule', 'wait', 'validate', 'commit')
    $seen = @{}
    foreach ($stream in $Timing.streams) {
        Assert-Stage5JsonShape $stream @('name', 'subtype', 'attemptedBatches', 'admittedBatches',
            'committedBatches', 'abortedBatches', 'firstFrame', 'lastFrame',
            'activePipelineNanoseconds', 'inclusiveBatchNanoseconds', 'maximumBatchNanoseconds', 'stages') $Context
        foreach ($field in @('subtype', 'attemptedBatches', 'admittedBatches', 'committedBatches',
            'abortedBatches', 'firstFrame', 'lastFrame', 'activePipelineNanoseconds',
            'inclusiveBatchNanoseconds', 'maximumBatchNanoseconds')) {
            Assert-Stage5DiagnosticCounter $stream[$field] "$Context $field"
        }
        $identity = "$($stream.name)|$($stream.subtype)"
        $maximumSubtype = if (@('ai-planning', 'path') -ccontains $stream.name) { 1 } else { 0 }
        Assert-Stage5Condition ($names -ccontains $stream.name -and
            -not $seen.ContainsKey($identity) -and $stream.subtype -le $maximumSubtype -and
            $stream.attemptedBatches -gt 0 -and $stream.admittedBatches -le $stream.attemptedBatches -and
            $stream.committedBatches -le $stream.admittedBatches -and
            [decimal]$stream.abortedBatches -eq ([decimal]$stream.admittedBatches - [decimal]$stream.committedBatches) -and
            $stream.firstFrame -ge $Frames.start -and $stream.lastFrame -le $Frames.end -and
            $stream.firstFrame -le $stream.lastFrame -and
            $stream.activePipelineNanoseconds -le $stream.inclusiveBatchNanoseconds -and
            $stream.maximumBatchNanoseconds -le $stream.inclusiveBatchNanoseconds -and
            $stream.stages -is [Array] -and $stream.stages.Count -eq 5) `
            "$Context stream identity, disposition, frame, or latency accounting is inconsistent."
        $seen[$identity] = $true
        [decimal]$active = 0
        for ($index = 0; $index -lt 5; ++$index) {
            $stage = $stream.stages[$index]
            Assert-Stage5JsonShape $stage @('name', 'totalNanoseconds', 'sampleCount') $Context
            Assert-Stage5DiagnosticCounter $stage.totalNanoseconds "$Context stage time"
            Assert-Stage5DiagnosticCounter $stage.sampleCount "$Context stage samples"
            $waitIsAbsent = $isBaseline -and $index -eq 2
            Assert-Stage5Condition ($stage.name -ceq $stages[$index] -and
                (($waitIsAbsent -and $stage.totalNanoseconds -eq 0 -and
                    $stage.sampleCount -eq 0) -or
                 (-not $waitIsAbsent -and
                    $stage.sampleCount -ge $stream.committedBatches -and
                    ($stage.totalNanoseconds -eq 0 -or $stage.sampleCount -gt 0)))) `
                "$Context committed stage coverage is incomplete."
            $active += [decimal]$stage.totalNanoseconds
        }
        Assert-Stage5Condition ($active -eq [decimal]$stream.activePipelineNanoseconds) `
            "$Context exclusive stage sum does not match active pipeline time."
    }
}

function Assert-Stage5DiagnosticKernelReference {
    param([object]$Reference, [object]$Timing, [object]$Frames,
        [string]$MeasurementRole, [string]$Context)
    Assert-Stage5JsonShape $Reference @('schemaVersion', 'mode', 'frozen', 'complete',
        'errors', 'generation', 'streams') $Context
    Assert-Stage5DiagnosticCounter $Reference.generation "$Context generation"
    $expectedMode = switch ($MeasurementRole) {
        'throughput' { 'throughput-binding' }
        'serial-oracle' { 'serial-oracle' }
        'phase-serial-baseline' { 'phase-baseline-binding' }
        default { '' }
    }
    Assert-Stage5Condition (@('throughput', 'serial-oracle',
            'phase-serial-baseline') -ccontains $MeasurementRole -and
        (Test-Stage5JsonInteger $Reference.schemaVersion) -and $Reference.schemaVersion -eq 1 -and
        $Reference.mode -ceq $expectedMode -and
        $Reference.frozen -is [bool] -and $Reference.frozen -and
        $Reference.complete -is [bool] -and
        (Test-Stage5JsonInteger $Reference.errors) -and $Reference.errors -eq 0 -and
        [decimal]$Reference.generation -gt 0 -and $Reference.streams -is [Array] -and
        $Reference.streams.Count -le 16 -and
        $Reference.complete -eq ($Reference.streams.Count -gt 0)) `
        "$Context is not a finalized role-consistent canonical reference ledger."
    $names = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $seen = @{}
    foreach ($stream in $Reference.streams) {
        Assert-Stage5JsonShape $stream @('name', 'subtype', 'fieldSchema', 'firstFrame', 'lastFrame',
            'validatedBatchCount', 'committedBatchCount', 'abortedBatchCount',
            'validatedOperationCount', 'committedOperationCount', 'serialSampleCount',
            'serialNanoseconds', 'maximumSerialNanoseconds', 'inputSha256', 'outputSha256', 'commitSha256') $Context
        foreach ($field in @('subtype', 'fieldSchema', 'firstFrame', 'lastFrame', 'validatedBatchCount',
            'committedBatchCount', 'abortedBatchCount', 'validatedOperationCount',
            'committedOperationCount', 'serialSampleCount', 'serialNanoseconds', 'maximumSerialNanoseconds')) {
            Assert-Stage5DiagnosticCounter $stream[$field] "$Context $field"
        }
        $identity = "$($stream.name)|$($stream.subtype)"
        $maximumSubtype = if (@('ai-planning', 'path') -ccontains $stream.name) { 1 } else { 0 }
        Assert-Stage5Condition ($names -ccontains $stream.name -and
            -not $seen.ContainsKey($identity) -and $stream.subtype -le $maximumSubtype -and
            $stream.fieldSchema -gt 0 -and $stream.fieldSchema -le [UInt32]::MaxValue -and
            $stream.firstFrame -le [UInt32]::MaxValue -and $stream.lastFrame -le [UInt32]::MaxValue -and
            $stream.firstFrame -ge $Frames.start -and $stream.lastFrame -le $Frames.end -and
            $stream.firstFrame -le $stream.lastFrame -and $stream.validatedBatchCount -gt 0 -and
            $stream.committedBatchCount -le $stream.validatedBatchCount -and
            [decimal]$stream.abortedBatchCount -eq ([decimal]$stream.validatedBatchCount - [decimal]$stream.committedBatchCount) -and
            $stream.validatedOperationCount -ge $stream.validatedBatchCount -and
            $stream.committedOperationCount -ge $stream.committedBatchCount -and
            $stream.committedOperationCount -le $stream.validatedOperationCount -and
            [decimal]$stream.validatedOperationCount - [decimal]$stream.committedOperationCount -ge [decimal]$stream.abortedBatchCount -and
            ($stream.committedBatchCount -gt 0 -or $stream.committedOperationCount -eq 0) -and
            ($stream.abortedBatchCount -gt 0 -or $stream.validatedOperationCount -eq $stream.committedOperationCount) -and
            $stream.maximumSerialNanoseconds -le $stream.serialNanoseconds) `
            "$Context reference identity, frame, batch, or operation accounting is inconsistent."
        foreach ($field in @('inputSha256', 'outputSha256', 'commitSha256')) {
            Assert-Stage5Condition ($stream[$field] -is [string] -and $stream[$field] -cmatch '^[0-9A-F]{64}$') `
                "$Context canonical reference $field digest is unavailable."
        }
        $seen[$identity] = $true
        $matchingTiming = @($Timing.streams | Where-Object {
            $_.name -ceq $stream.name -and $_.subtype -eq $stream.subtype
        })
        Assert-Stage5Condition ($matchingTiming.Count -eq 1) "$Context reference stream has no unique timing stream."
        $matched = $matchingTiming[0]
        Assert-Stage5Condition ($stream.committedBatchCount -eq $matched.committedBatches -and
            $stream.validatedBatchCount -le $matched.admittedBatches -and
            $stream.firstFrame -ge $matched.firstFrame -and $stream.lastFrame -le $matched.lastFrame) `
            "$Context reference and timing batch commit/frame coverage do not match."
        if ($MeasurementRole -ceq 'throughput' -or
            $MeasurementRole -ceq 'phase-serial-baseline') {
            Assert-Stage5Condition ($stream.serialSampleCount -eq 0 -and $stream.serialNanoseconds -eq 0 -and
                $stream.maximumSerialNanoseconds -eq 0) "$Context throughput reference contains serial-oracle work."
        } else {
            Assert-Stage5Condition ($stream.serialSampleCount -eq $stream.committedBatchCount -and
                ($stream.serialNanoseconds -eq 0 -or $stream.serialSampleCount -gt 0)) `
                "$Context serial reference samples do not match committed batches."
        }
    }
}

function Get-Stage5DiagnosticInputBindings {
    param([object]$Aggregate, [string]$SourceCommit, [string]$ArtifactHash,
        [string]$ExecutableHash, [string]$Title)
    # Bind the already-reviewed closure manifest once. This diagnostic
    # conversion does not claim to requalify every installed asset or replace
    # the host's locked runtime-closure check.
    Assert-Stage5JsonShape $Aggregate.artifactSetManifest @('path', 'sha256') 'Diagnostic artifact manifest'
    $artifactPath = [IO.Path]::GetFullPath([string]$Aggregate.artifactSetManifest.path)
    $artifactSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $artifactPath 'Diagnostic artifact manifest'
    Assert-Stage5Condition ($Aggregate.artifactSetManifest.sha256 -ceq $ArtifactHash) 'Diagnostic artifact manifest binding differs.'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $artifactSnapshot $ArtifactHash 'Diagnostic artifact manifest' | Out-Null
    $artifact = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $artifactSnapshot 'Diagnostic artifact manifest'
    Assert-Stage5JsonShape $artifact @('schemaVersion', 'sourceCommit', 'productSet', 'architecture',
        'artifacts', 'runtimeClosure') 'Diagnostic artifact manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $artifact.schemaVersion) -and
        $artifact.schemaVersion -eq 1 -and $artifact.sourceCommit -ceq $SourceCommit -and
        $artifact.architecture -ceq 'x64') 'Diagnostic artifact identity differs.'
    Assert-Stage5FinalAcceptanceStringSet $artifact.productSet @('Generals', 'ZeroHour') 'Diagnostic artifact products'
    $artifactDirectory = Split-Path -Parent $artifactPath
    $reference = $artifact.runtimeClosure.dependencyManifest
    Assert-Stage5JsonShape $reference @('path', 'sha256') 'Diagnostic dependency manifest'
    $dependencyPath = Resolve-Stage5FinalAcceptanceFile $artifactDirectory $reference.path 'Diagnostic dependency manifest'
    $dependencySnapshot = Get-Stage5FinalAcceptanceFileSnapshot $dependencyPath 'Diagnostic dependency manifest'
    $dependencyHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $dependencySnapshot $reference.sha256 'Diagnostic dependency manifest'
    $dependency = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $dependencySnapshot 'Diagnostic dependency manifest'
    Assert-Stage5JsonShape $dependency @('schemaVersion', 'sourceCommit', 'productSet', 'architecture', 'files') 'Diagnostic dependency manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $dependency.schemaVersion) -and
        $dependency.schemaVersion -eq 1 -and $dependency.sourceCommit -ceq $SourceCommit -and
        $dependency.architecture -ceq 'x64' -and $dependency.files -is [Array] -and $dependency.files.Count -ge 10) `
        'Diagnostic dependency closure identity is invalid.'
    Assert-Stage5FinalAcceptanceStringSet $dependency.productSet @('Generals', 'ZeroHour') 'Diagnostic dependency products'
    $lines = @(); $seenPaths = @{}; $kinds = @{}; $executableMatches = 0
    foreach ($file in $dependency.files) {
        Assert-Stage5JsonShape $file @('title', 'kind', 'path', 'sha256') 'Diagnostic dependency entry'
        Assert-Stage5Condition (@('Generals', 'ZeroHour') -ccontains $file.title -and
            @('executable', 'launcher', 'launcher-config', 'dll', 'asset') -ccontains $file.kind -and
            $file.sha256 -is [string] -and $file.sha256 -cmatch '^[0-9A-F]{64}$') 'Diagnostic dependency entry is invalid.'
        $filePath = Resolve-Stage5FinalAcceptanceFile $artifactDirectory $file.path 'Diagnostic dependency path'
        Assert-Stage5Condition (-not $seenPaths.ContainsKey($filePath)) 'Diagnostic dependency path is duplicated.'
        $seenPaths[$filePath] = $file.sha256; $kinds["$($file.title)|$($file.kind)"] = $true
        $lines += "$($file.title)|$($file.kind)|$(([string]$file.path).Replace('\', '/'))|$($file.sha256)"
        if ($file.title -ceq $Title -and $file.kind -ceq 'executable' -and $file.sha256 -ceq $ExecutableHash -and
            [String]::Equals($filePath, [IO.Path]::GetFullPath([string]$Aggregate.executable.path), [StringComparison]::OrdinalIgnoreCase)) {
            ++$executableMatches
        }
    }
    foreach ($product in @('Generals', 'ZeroHour')) {
        foreach ($kind in @('executable', 'launcher', 'launcher-config', 'dll', 'asset')) {
            Assert-Stage5Condition ($kinds.ContainsKey("$product|$kind")) 'Diagnostic dependency closure is incomplete.'
        }
    }
    Assert-Stage5Condition ($executableMatches -eq 1) 'Diagnostic executable is detached from the artifact closure.'
    foreach ($entry in $artifact.artifacts) {
        Assert-Stage5JsonShape $entry @('role', 'path', 'sha256') 'Diagnostic core artifact'
        $entryPath = Resolve-Stage5FinalAcceptanceFile $artifactDirectory $entry.path 'Diagnostic core artifact'
        Assert-Stage5Condition ($seenPaths.ContainsKey($entryPath) -and $seenPaths[$entryPath] -ceq $entry.sha256) `
            'Diagnostic core artifact is detached from the closure.'
    }
    [Array]::Sort($lines, [StringComparer]::Ordinal)
    $closureHash = Get-Stage5FinalAcceptanceSha256FromBytes ([Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n"))
    $closure = $Aggregate.runtimeClosure
    Assert-Stage5JsonShape $closure @('dependencyManifestPath', 'dependencyManifestSha256', 'closureSha256', 'fileCount') 'Diagnostic runtime closure'
    Assert-Stage5Condition ($artifact.runtimeClosure.closureSha256 -ceq $closureHash -and
        $closure.dependencyManifestPath -ceq $reference.path -and $closure.dependencyManifestSha256 -ceq $dependencyHash -and
        $closure.closureSha256 -ceq $closureHash -and $closure.fileCount -eq $dependency.files.Count) `
        'Diagnostic runtime closure binding does not match its manifest bytes.'
    Assert-Stage5JsonShape $Aggregate.fixtureManifest @('path', 'sha256') 'Diagnostic fixture manifest'
    $fixturePath = [IO.Path]::GetFullPath([string]$Aggregate.fixtureManifest.path)
    $fixtureSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $fixturePath 'Diagnostic fixture manifest'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $fixtureSnapshot $Aggregate.fixtureManifest.sha256 'Diagnostic fixture manifest' | Out-Null
    $fixtureDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $fixtureSnapshot 'Diagnostic fixture manifest'
    Assert-Stage5JsonShape $fixtureDocument @('schemaVersion', 'evidenceKind', 'title', 'executableSha256', 'fixtures') 'Diagnostic fixture manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $fixtureDocument.schemaVersion) -and
        $fixtureDocument.schemaVersion -eq 1 -and
        $fixtureDocument.evidenceKind -ceq 'stage5-performance-scaling-fixtures' -and
        $fixtureDocument.title -ceq $Title -and $fixtureDocument.executableSha256 -ceq $ExecutableHash -and
        $fixtureDocument.fixtures -is [Array] -and $fixtureDocument.fixtures.Count -eq 4) 'Diagnostic fixture manifest identity is invalid.'
    $fixtureMinimums = @{ 'one-thousand-units' = 1000; 'four-thousand-units' = 4000; 'eight-thousand-units' = 8000; 'dense-eight-player' = 8000 }
    $fixtures = @{}
    foreach ($fixture in $fixtureDocument.fixtures) {
        Assert-Stage5JsonShape $fixture @('id', 'source', 'sha256', 'seed', 'playerCount', 'peakUnitCount') 'Diagnostic reviewed fixture'
        Assert-Stage5Condition ($fixtureMinimums.ContainsKey([string]$fixture.id) -and -not $fixtures.ContainsKey([string]$fixture.id) -and
            $fixture.playerCount -eq 8 -and (Test-Stage5JsonInteger $fixture.seed) -and
            $fixture.seed -ge 0 -and $fixture.seed -le [UInt32]::MaxValue -and
            (Test-Stage5JsonInteger $fixture.peakUnitCount) -and
            (($fixture.id -ceq 'dense-eight-player' -and $fixture.peakUnitCount -ge 8000) -or
                ($fixture.id -cne 'dense-eight-player' -and $fixture.peakUnitCount -eq $fixtureMinimums[$fixture.id]))) `
            'Diagnostic reviewed fixture workload or seed is invalid.'
        $replayPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $fixturePath) $fixture.source 'Diagnostic reviewed replay'
        $replaySnapshot = Get-Stage5FinalAcceptanceFileSnapshot $replayPath `
            'Diagnostic reviewed replay' -HashOnly -EvidenceKind Replay
        $replayHash = Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
            $replaySnapshot $fixture.sha256 $replaySnapshot.length `
            'Diagnostic reviewed replay'
        $fixtures[$fixture.id] = [pscustomobject]@{ path = $replayPath; sha256 = $replayHash
            seed = $fixture.seed; playerCount = $fixture.playerCount; minimumUnitCount = $fixture.peakUnitCount }
    }
    return [pscustomobject]@{ fixtures = $fixtures; dependencyManifestSha256 = $dependencyHash; closureSha256 = $closureHash }
}

function Get-Stage5PerformancePhaseRunIdentitySha256 {
    param([string]$RunId, [string]$RunNonce, [object]$ProcessId,
        [object]$ProcessCreationTimeUtc100ns)
    Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($RunId) -and
        -not [string]::IsNullOrWhiteSpace($RunNonce)) `
        'Performance phase run identity strings are empty.'
    Assert-Stage5DiagnosticCounter $ProcessId `
        'Performance phase run identity process id'
    Assert-Stage5DiagnosticCounter $ProcessCreationTimeUtc100ns `
        'Performance phase run identity process creation time'
    Assert-Stage5Condition ([UInt64]$ProcessId -gt 0 -and
        [UInt64]$ProcessId -le [UInt32]::MaxValue -and
        [UInt64]$ProcessCreationTimeUtc100ns -gt 0) `
        'Performance phase run identity process values are invalid.'
    $memory = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($memory)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RTS-KERNEL-FIELDS-v1'))
        $writer.Write([UInt32]0x5003)
        [UInt32]$tag = 1
        foreach ($text in @($RunId, $RunNonce)) {
            $bytes = [Text.Encoding]::ASCII.GetBytes([string]$text)
            $writer.Write([byte]6)
            $writer.Write($tag)
            $writer.Write([UInt32]$bytes.Length)
            foreach ($value in $bytes) {
                $writer.Write([byte]1)
                $writer.Write($tag)
                $writer.Write([UInt32]$value)
            }
            ++$tag
        }
        $writer.Write([byte]1)
        $writer.Write([UInt32]3)
        $writer.Write([UInt32]$ProcessId)
        $writer.Write([byte]3)
        $writer.Write([UInt32]4)
        $writer.Write([UInt64]$ProcessCreationTimeUtc100ns)
        $writer.Flush()
        return Get-Stage5FinalAcceptanceSha256FromBytes $memory.ToArray()
    }
    finally {
        $writer.Dispose()
        $memory.Dispose()
    }
}

function Assert-Stage5PerformancePhaseTraceSemantics {
    param([object]$Receipt, [object]$Profile, [string]$Context)
    $trace = $Receipt.attemptTrace
    $counters = @('residentAttemptCapacity', 'residentRangeCapacity',
        'residentAttemptCount', 'residentAttemptHighWater',
        'residentRangeCount', 'residentRangeHighWater', 'recordCount',
        'logicalEventCount', 'coalescedSpanCount', 'coalescedAttemptCount',
        'attemptCount', 'admittedAttemptCount', 'notAdmittedAttemptCount',
        'abortedAfterAdmissionAttemptCount', 'reapCount',
        'capturedAttemptCount', 'capturedOperationCount', 'dispatchCount',
        'rangeCount', 'releasedRangeCount', 'windowBoundaryCount',
        'completedWindowCount', 'controlWindowCount')
    Assert-Stage5JsonShape $trace (@('schemaVersion', 'encoding',
        'fieldSchema', 'mode', 'frozen', 'complete', 'errors',
        'observationIngressSealed', 'executionClosureSealed', 'file',
        'binding', 'limits', 'sourceBinding') + $counters) `
        "$Context attempt trace"
    foreach ($field in @('schemaVersion', 'fieldSchema', 'errors') +
            $counters) {
        Assert-Stage5DiagnosticCounter $trace[$field] `
            "$Context attempt trace $field"
    }
    foreach ($field in @('frozen', 'complete', 'observationIngressSealed',
            'executionClosureSealed')) {
        Assert-Stage5Condition ($trace[$field] -is [bool] -and
            [bool]$trace[$field]) `
            "$Context attempt trace $field is not proven."
    }
    $expectedMode = if ($Receipt.measurementRole -ceq
        'phase-serial-baseline') { 'consume' } else { 'record' }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        $Receipt.schemaVersion -eq 6 -and
        @('throughput', 'phase-serial-baseline') -ccontains
            [string]$Receipt.measurementRole -and
        (Test-Stage5JsonInteger $trace.schemaVersion) -and
        $trace.schemaVersion -eq 1 -and
        $trace.encoding -ceq 'typed-canonical-le-v1' -and
        $trace.fieldSchema -eq 20481 -and $trace.errors -eq 0 -and
        $trace.mode -ceq $expectedMode) `
        "$Context attempt trace version or role is invalid."
    Assert-Stage5JsonShape $trace.file @('path', 'sha256', 'byteCount') `
        "$Context attempt trace file"
    Assert-Stage5JsonShape $trace.binding @('nativeRunIdentitySha256',
        'executableSha256', 'fixtureSha256', 'sourcePolicySha256') `
        "$Context attempt trace identity"
    foreach ($field in @('nativeRunIdentitySha256', 'executableSha256',
            'fixtureSha256', 'sourcePolicySha256')) {
        Assert-Stage5Condition ($trace.binding[$field] -is [string] -and
            $trace.binding[$field] -cmatch '^[0-9A-F]{64}$') `
            "$Context attempt trace $field is not an uppercase SHA-256."
    }
    Assert-Stage5JsonShape $trace.limits @('maximumBytes',
        'maximumRecords', 'maximumLogicalEvents', 'maximumAttempts',
        'maximumRanges') "$Context attempt trace limits"
    foreach ($field in @('maximumBytes', 'maximumRecords',
            'maximumLogicalEvents', 'maximumAttempts', 'maximumRanges')) {
        Assert-Stage5DiagnosticCounter $trace.limits[$field] `
            "$Context attempt trace limit $field"
        Assert-Stage5Condition ([UInt64]$trace.limits[$field] -gt 0) `
            "$Context attempt trace limit $field is not positive."
    }
    Assert-Stage5Condition ($trace.file.path -is [string] -and
        -not [string]::IsNullOrWhiteSpace([string]$trace.file.path) -and
        $trace.file.sha256 -is [string] -and
        $trace.file.sha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $trace.file.byteCount) -and
        [UInt64]$trace.file.byteCount -gt 0 -and
        [UInt64]$trace.file.byteCount -le [UInt64]$trace.limits.maximumBytes -and
        [UInt64]$trace.recordCount -gt 0 -and
        [UInt64]$trace.recordCount -le [UInt64]$trace.limits.maximumRecords -and
        [UInt64]$trace.logicalEventCount -ge [UInt64]$trace.recordCount -and
        [UInt64]$trace.logicalEventCount -le
            [UInt64]$trace.limits.maximumLogicalEvents -and
        ([UInt64]$trace.coalescedSpanCount -ne 0 -or
            [UInt64]$trace.logicalEventCount -eq [UInt64]$trace.recordCount) -and
        [UInt64]$trace.coalescedSpanCount -le [UInt64]$trace.recordCount -and
        [UInt64]$trace.coalescedSpanCount -le
            [UInt64]$trace.coalescedAttemptCount -and
        [UInt64]$trace.coalescedAttemptCount -le
            [UInt64]$trace.attemptCount -and
        [UInt64]$trace.attemptCount -le [UInt64]$trace.limits.maximumAttempts -and
        [decimal]$trace.attemptCount -eq
            ([decimal]$trace.admittedAttemptCount +
             [decimal]$trace.notAdmittedAttemptCount) -and
        [UInt64]$trace.abortedAfterAdmissionAttemptCount -le
            [UInt64]$trace.admittedAttemptCount -and
        [UInt64]$trace.reapCount -eq [UInt64]$trace.attemptCount -and
        [UInt64]$trace.capturedAttemptCount -le [UInt64]$trace.attemptCount -and
        [UInt64]$trace.rangeCount -le [UInt64]$trace.limits.maximumRanges -and
        [UInt64]$trace.releasedRangeCount -eq [UInt64]$trace.rangeCount -and
        [UInt64]$trace.residentAttemptCapacity -gt 0 -and
        [UInt64]$trace.residentRangeCapacity -gt 0 -and
        [UInt64]$trace.residentAttemptCount -eq 0 -and
        [UInt64]$trace.residentRangeCount -eq 0 -and
        [UInt64]$trace.residentAttemptHighWater -le
            [UInt64]$trace.residentAttemptCapacity -and
        [UInt64]$trace.residentRangeHighWater -le
            [UInt64]$trace.residentRangeCapacity -and
        [UInt64]$trace.windowBoundaryCount -gt 0 -and
        [UInt64]$trace.windowBoundaryCount -le [UInt64]$trace.recordCount -and
        [UInt64]$trace.completedWindowCount -eq
            [UInt64]$Receipt.workload.sampleCount -and
        ([decimal]$trace.completedWindowCount +
            [decimal]$trace.controlWindowCount) -le
            [decimal]$trace.windowBoundaryCount) `
        "$Context attempt trace counts do not close within frozen limits."
    if ($expectedMode -ceq 'consume') {
        Assert-Stage5Condition ([UInt64]$trace.controlWindowCount -eq
            [UInt64]$Receipt.phaseAccounting.controlAccounting.windowCount) `
            "$Context attempt trace control-window count differs from phase accounting."
        Assert-Stage5JsonShape $trace.sourceBinding @('receipt', 'runId',
            'runNonce', 'processId', 'processCreationTimeUtc100ns') `
            "$Context attempt trace source binding"
        Assert-Stage5JsonShape $trace.sourceBinding.receipt @('path', 'sha256') `
            "$Context attempt trace source receipt"
        Assert-Stage5Condition ($trace.sourceBinding.receipt.path -is [string] -and
            [IO.Path]::IsPathRooted([string]$trace.sourceBinding.receipt.path) -and
            $trace.sourceBinding.receipt.sha256 -is [string] -and
            $trace.sourceBinding.receipt.sha256 -cmatch '^[0-9A-F]{64}$') `
            "$Context attempt trace source receipt binding is malformed."
        $identitySha256 = Get-Stage5PerformancePhaseRunIdentitySha256 `
            ([string]$trace.sourceBinding.runId) `
            ([string]$trace.sourceBinding.runNonce) `
            $trace.sourceBinding.processId `
            $trace.sourceBinding.processCreationTimeUtc100ns
    }
    else {
        Assert-Stage5Condition ($null -eq $trace.sourceBinding) `
            "$Context record trace cannot claim a source receipt."
        $identitySha256 = Get-Stage5PerformancePhaseRunIdentitySha256 `
            ([string]$Receipt.runId) ([string]$Receipt.runNonce) `
            $Receipt.process.id $Receipt.process.creationTimeUtc100ns
    }
    Assert-Stage5Condition ($trace.binding.nativeRunIdentitySha256 -ceq
            $identitySha256 -and
        $trace.binding.executableSha256 -ceq $Receipt.executableSha256 -and
        $trace.binding.fixtureSha256 -ceq $Receipt.fixture.contentSha256 -and
        $trace.binding.sourcePolicySha256 -ceq $Profile.sourcePolicySha256 -and
        ($trace.limits | ConvertTo-Json -Compress -Depth 10) -ceq
            ($Profile.limits | ConvertTo-Json -Compress -Depth 10) -and
        [UInt64]$trace.residentAttemptCapacity -eq
            [UInt64]$Profile.residentAttemptCapacity -and
        [UInt64]$trace.residentRangeCapacity -eq
            [UInt64]$Profile.residentRangeCapacity) `
        "$Context attempt trace is detached from the reviewed profile or native identity."
    return $trace
}

function ConvertTo-Stage5PerformanceDiagnostics {
    param([string]$HostAggregatePath, [string]$ExpectedHostAggregateSha256,
        [string]$ExpectedSourceCommit, [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedExecutableSha256,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle = 'ZeroHour',
        [switch]$RequireAcceptance,
        [Collections.IDictionary]$RelocationMap = $null,
        [string]$RecordedTaskRoot = '')
    # Independent oracle processes can establish exact kernel references, not
    # whole-frame serial coverage. This converter has no acceptance branch.
    $full = [IO.Path]::GetFullPath($HostAggregatePath)
    $base = Split-Path -Parent $full
    $isRelocated = $null -ne $RelocationMap
    $recordedBase = if ($isRelocated) {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace(
                $RecordedTaskRoot)) `
            'Performance relocation requires the exact recorded task root.'
        [IO.Path]::GetFullPath($RecordedTaskRoot).TrimEnd('\', '/')
    } else { $base }
    $resolveTaskEvidence = {
        param([string]$RecordedPath, [string]$Context)
        $recordedFull = [IO.Path]::GetFullPath($RecordedPath)
        if (-not $isRelocated) { return $recordedFull }
        Assert-Stage5FinalAcceptancePathContained $recordedBase $recordedFull `
            "$Context recorded evidence"
        Assert-Stage5Condition ($RelocationMap.ContainsKey($recordedFull)) `
            "$Context is absent from the verified relocation manifest: $recordedFull"
        $relocated = [IO.Path]::GetFullPath([string]$RelocationMap[$recordedFull])
        Assert-Stage5FinalAcceptancePathContained $base $relocated $Context
        return $relocated
    }
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full 'Performance host aggregate'
    $hostHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        $ExpectedHostAggregateSha256 'Performance host aggregate'
    $aggregate = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot 'Performance host aggregate'
    Assert-Stage5JsonProperties $aggregate @('schemaVersion', 'evidenceKind', 'producer', 'status',
        'qualificationMode', 'measurementMode', 'installedRuntime', 'sourceCommit',
        'artifactSetSha256', 'title', 'executable', 'nativeReceiptBindings', 'runs', 'referencePolicy',
        'pairedOracleBindings', 'artifactSetManifest', 'runtimeClosure', 'fixtureManifest') 'Performance host aggregate'
    $expectedKind = if ($aggregate.qualificationMode -ceq 'LocalCapacitySmoke') {
        'stage5-performance-scaling-local-capacity-smoke'
    } else { 'stage5-performance-scaling-host-qualification' }
    Assert-Stage5Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $ExpectedArtifactSetSha256 -cmatch '^[0-9A-F]{64}$' -and
        $ExpectedExecutableSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $aggregate.schemaVersion) -and
        $aggregate.schemaVersion -eq 2 -and $aggregate.evidenceKind -ceq $expectedKind -and
        $aggregate.producer -ceq 'Invoke-Stage5PerformanceScalingValidation.ps1' -and
        $aggregate.status -ceq 'passed' -and
        @('LocalCapacitySmoke', 'External16Core') -ccontains $aggregate.qualificationMode -and
        $aggregate.measurementMode -ceq 'headless-throughput' -and
        $aggregate.installedRuntime -is [bool] -and $aggregate.installedRuntime -and
        $aggregate.sourceCommit -ceq $ExpectedSourceCommit -and
        $aggregate.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $aggregate.title -ceq $ExpectedTitle -and
        $aggregate.executable.sha256 -ceq $ExpectedExecutableSha256 -and
        $aggregate.runs -is [Array] -and $aggregate.runs.Count -gt 0 -and
        $aggregate.nativeReceiptBindings -is [Array] -and
        $aggregate.nativeReceiptBindings.Count -eq $aggregate.runs.Count) `
        'Performance host aggregate is not bound to the requested native installed artifact.'
    Assert-Stage5Condition (@('throughput-only', 'paired-serial-oracle-v1') -ccontains $aggregate.referencePolicy -and
        $aggregate.pairedOracleBindings -is [Array] -and
        (($aggregate.referencePolicy -ceq 'throughput-only' -and $aggregate.pairedOracleBindings.Count -eq 0) -or
            ($aggregate.referencePolicy -ceq 'paired-serial-oracle-v1' -and $aggregate.pairedOracleBindings.Count -eq $aggregate.runs.Count))) `
        'Performance paired oracle policy or pair count is invalid.'
    $phaseFields = @('phaseBaselinePolicy', 'phaseBaselineProfiles',
        'phaseBaselinePlan', 'phaseBaselineAttemptManifest',
        'pairedPhaseBaselineBindings')
    $phaseFieldCount = @($phaseFields | Where-Object {
        $aggregate.Keys -ccontains $_
    }).Count
    Assert-Stage5Condition ($phaseFieldCount -eq 0 -or
        $phaseFieldCount -eq $phaseFields.Count) `
        'Performance phase-baseline aggregate metadata is partially stripped.'
    $hasPhaseBaselines = $phaseFieldCount -eq $phaseFields.Count
    $phaseDocuments = @{}
    if ($hasPhaseBaselines) {
        Assert-Stage5Condition ($aggregate.phaseBaselinePolicy -is [string] -and
            $aggregate.phaseBaselinePolicy -ceq 'paired-source-admissions-v1' -and
            $aggregate.phaseBaselineProfiles -is [Array] -and
            $aggregate.phaseBaselineProfiles.Count -gt 0 -and
            $aggregate.pairedPhaseBaselineBindings -is [Array] -and
            $aggregate.pairedPhaseBaselineBindings.Count -gt 0) `
            'Performance phase-baseline policy, profiles, or bindings are incomplete.'
        foreach ($phaseArtifact in @(
            [pscustomobject]@{ binding = $aggregate.phaseBaselinePlan
                context = 'Performance frozen phase plan' }
            [pscustomobject]@{ binding = $aggregate.phaseBaselineAttemptManifest
                context = 'Performance phase attempt manifest' }
        )) {
            $phaseBinding = $phaseArtifact.binding
            $phaseContext = [string]$phaseArtifact.context
            Assert-Stage5JsonShape $phaseBinding @('path', 'sha256') $phaseContext
            $phasePath = & $resolveTaskEvidence ([string]$phaseBinding.path) `
                $phaseContext
            Assert-Stage5FinalAcceptancePathContained $base $phasePath $phaseContext
            $phaseSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $phasePath $phaseContext
            Assert-Stage5FinalAcceptanceSnapshotSha256 $phaseSnapshot `
                ([string]$phaseBinding.sha256) $phaseContext | Out-Null
            $phaseDocuments[$phaseContext] =
                ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $phaseSnapshot `
                    $phaseContext
        }
    }
    $inputBindings = if ($isRelocated) {
        Assert-Stage5JsonShape $aggregate.runtimeClosure @(
            'dependencyManifestPath','dependencyManifestSha256','closureSha256',
            'fileCount') 'Relocated performance runtime closure'
        Assert-Stage5Condition ($aggregate.runtimeClosure.dependencyManifestSha256 `
                -cmatch '^[0-9A-F]{64}$' -and
            $aggregate.runtimeClosure.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
            (Test-Stage5JsonInteger $aggregate.runtimeClosure.fileCount) -and
            [int]$aggregate.runtimeClosure.fileCount -gt 0) `
            'Relocated performance runtime closure binding is malformed.'
        Assert-Stage5JsonShape $aggregate.fixtureManifest @('path', 'sha256') `
            'Relocated performance fixture manifest'
        $recordedFixtureManifestPath = [IO.Path]::GetFullPath(
            [string]$aggregate.fixtureManifest.path)
        $fixtureManifestPath = & $resolveTaskEvidence `
            $recordedFixtureManifestPath 'Relocated performance fixture manifest'
        $fixtureManifestSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
            $fixtureManifestPath 'Relocated performance fixture manifest'
        Assert-Stage5FinalAcceptanceSnapshotSha256 $fixtureManifestSnapshot `
            ([string]$aggregate.fixtureManifest.sha256) `
            'Relocated performance fixture manifest' | Out-Null
        $fixtureDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
            $fixtureManifestSnapshot 'Relocated performance fixture manifest'
        Assert-Stage5JsonShape $fixtureDocument @('schemaVersion',
            'evidenceKind', 'title', 'executableSha256', 'fixtures') `
            'Relocated performance fixture manifest'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $fixtureDocument.schemaVersion) -and
            $fixtureDocument.schemaVersion -eq 1 -and
            $fixtureDocument.evidenceKind -ceq
                'stage5-performance-scaling-fixtures' -and
            $fixtureDocument.title -ceq $ExpectedTitle -and
            $fixtureDocument.executableSha256 -ceq $ExpectedExecutableSha256 -and
            $fixtureDocument.fixtures -is [Array] -and
            $fixtureDocument.fixtures.Count -eq 4) `
            'Relocated performance fixture manifest identity is invalid.'
        $canonicalFixtureIds = @('one-thousand-units',
            'four-thousand-units', 'eight-thousand-units',
            'dense-eight-player')
        $canonicalFixtureUnits = @(1000, 4000, 8000, 8000)
        $reviewedFixtures = @{}
        for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
            $fixture = $fixtureDocument.fixtures[$fixtureIndex]
            $fixtureContext = "Relocated reviewed fixture $fixtureIndex"
            Assert-Stage5JsonShape $fixture @('id', 'source', 'sha256',
                'seed', 'playerCount', 'peakUnitCount') $fixtureContext
            $source = [string]$fixture.source
            Assert-Stage5Condition ($fixture.id -ceq
                    $canonicalFixtureIds[$fixtureIndex] -and
                $source -is [string] -and
                -not [string]::IsNullOrWhiteSpace($source) -and
                -not [IO.Path]::IsPathRooted($source) -and
                $source -notmatch ':' -and
                $source -notmatch '(^|[\\/])\.\.?([\\/]|$)' -and
                $fixture.sha256 -is [string] -and
                $fixture.sha256 -cmatch '^[0-9A-F]{64}$' -and
                (Test-Stage5JsonInteger $fixture.seed) -and
                [decimal]$fixture.seed -ge 0 -and
                [decimal]$fixture.seed -le [UInt32]::MaxValue -and
                $fixture.playerCount -eq 8 -and
                (Test-Stage5JsonInteger $fixture.peakUnitCount) -and
                (($fixtureIndex -eq 3 -and
                    [int]$fixture.peakUnitCount -ge 8000) -or
                 ($fixtureIndex -ne 3 -and
                    [int]$fixture.peakUnitCount -eq
                        $canonicalFixtureUnits[$fixtureIndex]))) `
                "$fixtureContext identity, source, seed, or workload is invalid."
            $recordedReplayCandidate = Join-Path `
                (Split-Path -Parent $recordedFixtureManifestPath) $source
            $recordedReplayPath = [IO.Path]::GetFullPath(
                $recordedReplayCandidate)
            Assert-Stage5FinalAcceptancePathContained $recordedBase `
                $recordedReplayPath "$fixtureContext recorded replay"
            $replayPath = & $resolveTaskEvidence $recordedReplayPath `
                "$fixtureContext replay"
            $replaySnapshot = Get-Stage5FinalAcceptanceFileSnapshot $replayPath `
                "$fixtureContext replay" -HashOnly -EvidenceKind Replay
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 $replaySnapshot `
                ([string]$fixture.sha256) ([Int64]$replaySnapshot.length) `
                "$fixtureContext replay" | Out-Null
            Assert-Stage5Condition (-not $reviewedFixtures.ContainsKey(
                    [string]$fixture.id)) `
                "$fixtureContext is duplicated."
            $reviewedFixtures[[string]$fixture.id] = [pscustomobject]@{
                path = $recordedReplayPath
                relocatedPath = $replayPath
                sha256 = [string]$fixture.sha256
                seed = [UInt32]$fixture.seed
                playerCount = 8
                minimumUnitCount = [int]$fixture.peakUnitCount
            }
        }
        [pscustomobject]@{
            fixtures = $reviewedFixtures
            dependencyManifestSha256 = [string]$aggregate.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$aggregate.runtimeClosure.closureSha256
        }
    }
    else {
        Get-Stage5DiagnosticInputBindings $aggregate $ExpectedSourceCommit `
            $ExpectedArtifactSetSha256 $ExpectedExecutableSha256 $ExpectedTitle
    }
    $entries = @(); $scheduled = @{}; $pairsByThroughput = @{}
    for ($index = 0; $index -lt $aggregate.runs.Count; ++$index) {
        $run = $aggregate.runs[$index]
        Assert-Stage5Condition ($run.runId -is [string] -and -not $scheduled.ContainsKey($run.runId)) 'Performance throughput run is duplicated.'
        $scheduled[$run.runId] = $run
        $entries += [pscustomobject]@{ run = $run; binding = $aggregate.nativeReceiptBindings[$index]; role = 'throughput' }
    }
    foreach ($pair in $aggregate.pairedOracleBindings) {
        Assert-Stage5JsonShape $pair @('throughputRunId', 'oracleRun') 'Performance paired oracle binding'
        Assert-Stage5Condition ($pair.throughputRunId -is [string] -and $scheduled.ContainsKey($pair.throughputRunId) -and
            -not $pairsByThroughput.ContainsKey($pair.throughputRunId)) 'Performance oracle pair is duplicated or refers to an unknown run.'
        $pairsByThroughput[$pair.throughputRunId] = $pair.oracleRun
        $entries += [pscustomobject]@{ run = $pair.oracleRun; binding = $pair.oracleRun.receiptBinding; role = 'serial-oracle' }
    }
    $phasePairsByThroughput = @{}
    $phasePairsByBaseline = @{}
    if ($hasPhaseBaselines) {
        Assert-Stage5JsonShape $aggregate.schedule @('warmupRuns',
            'measuredRuns') 'Performance phase-baseline schedule'
        Assert-Stage5DiagnosticCounter $aggregate.schedule.warmupRuns `
            'Performance phase-baseline warmup runs'
        Assert-Stage5DiagnosticCounter $aggregate.schedule.measuredRuns `
            'Performance phase-baseline measured runs'
        Assert-Stage5Condition ([int]$aggregate.schedule.warmupRuns -eq 1 -and
            [int]$aggregate.schedule.measuredRuns -ge 3) `
            'Performance phase-baseline schedule is not acceptance-capable.'
        $phaseProfilesById = @{}
        foreach ($profile in $aggregate.phaseBaselineProfiles) {
            $profile = Assert-Stage5PerformancePhaseBaselineProfile $profile `
                $inputBindings.fixtures['dense-eight-player'].sha256 `
                ([int]$aggregate.schedule.warmupRuns) `
                ([int]$aggregate.schedule.measuredRuns) `
                'Performance phase baseline profile'
            Assert-Stage5Condition (-not $phaseProfilesById.ContainsKey(
                    [string]$profile.profileId)) `
                'Performance phase baseline profile is duplicated.'
            $phaseProfilesById[[string]$profile.profileId] = $profile
        }
        foreach ($pair in $aggregate.pairedPhaseBaselineBindings) {
            Assert-Stage5JsonShape $pair @('profileId', 'throughputRunId',
                'baselineRun') 'Performance phase baseline binding'
            Assert-Stage5Condition ($pair.profileId -is [string] -and
                -not [string]::IsNullOrWhiteSpace([string]$pair.profileId) -and
                $phaseProfilesById.ContainsKey([string]$pair.profileId) -and
                $pair.throughputRunId -is [string] -and
                $scheduled.ContainsKey([string]$pair.throughputRunId) -and
                -not $phasePairsByThroughput.ContainsKey(
                    [string]$pair.throughputRunId)) `
                'Performance phase baseline binding is duplicated or detached from throughput.'
            $phasePairsByThroughput[[string]$pair.throughputRunId] = $pair
            $baselineRun = $pair.baselineRun
            Assert-Stage5Condition ($null -ne $baselineRun -and
                $null -ne $baselineRun.receiptBinding -and
                $baselineRun.runId -is [string] -and
                -not $phasePairsByBaseline.ContainsKey(
                    [string]$baselineRun.runId)) `
                'Performance phase baseline binding lacks its immutable native receipt binding.'
            $phasePairsByBaseline[[string]$baselineRun.runId] = $pair
            $throughputRun = $scheduled[[string]$pair.throughputRunId]
            $profile = $phaseProfilesById[[string]$pair.profileId]
            Assert-Stage5Condition ($throughputRun.fixtureId -ceq
                    $profile.fixtureId -and
                $throughputRun.lane -ceq $profile.sourceLane -and
                $baselineRun.fixtureId -ceq $throughputRun.fixtureId -and
                $baselineRun.lane -ceq $throughputRun.lane -and
                $baselineRun.ordinal -eq $throughputRun.ordinal -and
                $baselineRun.warmup -is [bool] -and
                $baselineRun.warmup -eq $throughputRun.warmup -and
                $baselineRun.runId -is [string] -and
                $baselineRun.runId -cne $throughputRun.runId) `
                'Performance phase baseline changed its selected source identity.'
            $entries += [pscustomobject]@{
                run = $baselineRun
                binding = $baselineRun.receiptBinding
                role = 'phase-serial-baseline'
            }
        }
        $phasePlan = $phaseDocuments['Performance frozen phase plan']
        $phaseAttempts = $phaseDocuments['Performance phase attempt manifest']
        $phasePlanProperties = @('schemaVersion','title',
            'qualificationMode','taskRoot','cohortNonce','cohortCreatedUtc',
            'sourceCommit','executablePath','executableSha256',
            'artifactSetSha256','runtimeClosure','fixtureManifestPath',
            'fixtureManifestSha256','fixtures','referencePolicy','warmupRuns',
            'measuredRuns','timeoutSeconds','titleSessionEnvironment',
            'phaseBaselineProfiles','outputFilePolicy','entries')
        $hasPerformanceData = $aggregate.Keys -ccontains 'performanceData'
        if ($hasPerformanceData) { $phasePlanProperties += 'performanceData' }
        Assert-Stage5JsonShape $phasePlan $phasePlanProperties `
            'Performance frozen phase plan'
        Assert-Stage5JsonShape $phaseAttempts @('schemaVersion', 'planSha256',
            'outcomes', 'cohortFailure') 'Performance phase attempt manifest'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $phasePlan.schemaVersion) -and
            $phasePlan.schemaVersion -eq 1 -and
            $phasePlan.sourceCommit -ceq $ExpectedSourceCommit -and
            $phasePlan.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
            $phasePlan.executableSha256 -ceq $ExpectedExecutableSha256 -and
            $phasePlan.cohortNonce -ceq $aggregate.cohortNonce -and
            [IO.Path]::GetFullPath([string]$phasePlan.taskRoot) -ceq $recordedBase -and
            [IO.Path]::GetFullPath([string]$phasePlan.executablePath) -ceq
                [IO.Path]::GetFullPath([string]$aggregate.executable.path) -and
            $phasePlan.fixtureManifestSha256 -ceq $aggregate.fixtureManifest.sha256 -and
            $phasePlan.referencePolicy -ceq $aggregate.referencePolicy -and
            $phasePlan.outputFilePolicy -ceq
                'native-runid-pid-receipt-pid-tick-timing-v1' -and
            $phasePlan.entries -is [Array] -and
            ($phasePlan.phaseBaselineProfiles | ConvertTo-Json -Compress -Depth 20) -ceq
                ($aggregate.phaseBaselineProfiles | ConvertTo-Json -Compress -Depth 20) -and
            (-not $hasPerformanceData -or
                ($phasePlan.performanceData | ConvertTo-Json -Compress -Depth 5) -ceq
                    ($aggregate.performanceData | ConvertTo-Json -Compress -Depth 5))) `
            'Performance frozen phase plan is stale or detached from the aggregate.'
        Assert-Stage5Condition ((Test-Stage5JsonInteger $phaseAttempts.schemaVersion) -and
            $phaseAttempts.schemaVersion -eq 1 -and
            $phaseAttempts.planSha256 -ceq
                [string]$aggregate.phaseBaselinePlan.sha256 -and
            $phaseAttempts.outcomes -is [Array] -and
            $null -eq $phaseAttempts.cohortFailure) `
            'Performance phase attempt manifest is failed or detached from its frozen plan.'
        $actualEntriesById = @{}
        foreach ($actualEntry in $entries) {
            $actualEntriesById[[string]$actualEntry.run.runId] = $actualEntry
        }
        $plannedIds = @{}; $plannedEntriesById = @{}; $outcomeIds = @{}
        foreach ($plannedEntry in $phasePlan.entries) {
            Assert-Stage5JsonShape $plannedEntry @('entryId','measurementRole',
                'profileId','fixtureId','lane','ordinal','warmup','sourceEntryId',
                'runNonce','workerCount','expectedArgumentString','outputPaths') `
                'Performance frozen phase plan entry'
            Assert-Stage5Condition ($plannedEntry.entryId -is [string] -and
                -not $plannedIds.ContainsKey([string]$plannedEntry.entryId) -and
                $actualEntriesById.ContainsKey([string]$plannedEntry.entryId) -and
                $plannedEntry.measurementRole -is [string] -and
                @('throughput','serial-oracle','phase-serial-baseline') -ccontains
                    [string]$plannedEntry.measurementRole -and
                $plannedEntry.fixtureId -is [string] -and
                $plannedEntry.lane -is [string] -and
                (Test-Stage5JsonInteger $plannedEntry.ordinal) -and
                $plannedEntry.warmup -is [bool] -and
                $plannedEntry.runNonce -is [string] -and
                $plannedEntry.runNonce -cmatch
                    '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
                (Test-Stage5JsonInteger $plannedEntry.workerCount) -and
                [UInt64]$plannedEntry.workerCount -gt 0 -and
                $plannedEntry.expectedArgumentString -is [string]) `
                'Performance frozen phase plan has a duplicate or malformed entry identity.'
            $actualEntry = $actualEntriesById[[string]$plannedEntry.entryId]
            $expectedSourceId = $null
            $expectedProfileId = $null
            if ($plannedEntry.measurementRole -ceq 'serial-oracle') {
                $sourcePair = @($aggregate.pairedOracleBindings | Where-Object {
                    $_.oracleRun.runId -ceq $plannedEntry.entryId
                })
                Assert-Stage5Condition ($sourcePair.Count -eq 1) `
                    'Performance frozen oracle plan entry has no unique source pair.'
                $expectedSourceId = [string]$sourcePair[0].throughputRunId
            }
            elseif ($plannedEntry.measurementRole -ceq 'phase-serial-baseline') {
                $sourcePair = @($aggregate.pairedPhaseBaselineBindings | Where-Object {
                    $_.baselineRun.runId -ceq $plannedEntry.entryId
                })
                Assert-Stage5Condition ($sourcePair.Count -eq 1) `
                    'Performance frozen baseline plan entry has no unique source pair.'
                $expectedSourceId = [string]$sourcePair[0].throughputRunId
                $expectedProfileId = [string]$sourcePair[0].profileId
            }
            Assert-Stage5Condition ($plannedEntry.measurementRole -ceq
                    [string]$actualEntry.role -and
                $plannedEntry.fixtureId -ceq [string]$actualEntry.run.fixtureId -and
                $plannedEntry.lane -ceq [string]$actualEntry.run.lane -and
                $plannedEntry.ordinal -eq $actualEntry.run.ordinal -and
                $plannedEntry.warmup -eq $actualEntry.run.warmup -and
                $plannedEntry.runNonce -ceq [string]$actualEntry.binding.runNonce -and
                ($null -eq $plannedEntry.sourceEntryId -or
                    $plannedEntry.sourceEntryId -is [string]) -and
                ($null -eq $plannedEntry.profileId -or
                    $plannedEntry.profileId -is [string]) -and
                $plannedEntry.sourceEntryId -ceq $expectedSourceId -and
                $plannedEntry.profileId -ceq $expectedProfileId -and
                $plannedEntry.expectedArgumentString -ceq
                    [string]$actualEntry.run.expectedArgumentString) `
                'Performance frozen phase plan entry differs from the aggregate run identity.'
            Assert-Stage5JsonShape $plannedEntry.outputPaths @('runRoot',
                'receiptDirectory','rawLogPath','timingDirectory','stdoutPath',
                'stderrPath','tempDirectory','attemptTracePath',
                'attemptStartPath','attemptResultPath','sourceBindingPath') `
                'Performance frozen phase plan output paths'
            $expectedRunRoot = Join-Path $recordedBase ([string]$plannedEntry.entryId)
            Assert-Stage5Condition (
                [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.runRoot) -ceq
                    $expectedRunRoot -and
                [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.receiptDirectory) -ceq
                    (Join-Path $expectedRunRoot 'receipt') -and
                [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.rawLogPath) -ceq
                    [IO.Path]::GetFullPath([string]$actualEntry.run.rawLogPath) -and
                [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.attemptStartPath) -ceq
                    (Join-Path $recordedBase ('attempts/' + $plannedEntry.entryId + '.start.json')) -and
                [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.attemptResultPath) -ceq
                    (Join-Path $recordedBase ('attempts/' + $plannedEntry.entryId + '.result.json'))) `
                'Performance frozen phase plan output paths escaped or changed their run identity.'
            $plannedIds[[string]$plannedEntry.entryId] =
                [string]$plannedEntry.measurementRole
            $plannedEntriesById[[string]$plannedEntry.entryId] = $plannedEntry
        }
        $readPhaseJournal = {
            param([object]$binding, [string]$journalContext)
            Assert-Stage5JsonShape $binding @('path','sha256') $journalContext
            $journalPath = & $resolveTaskEvidence ([string]$binding.path) `
                $journalContext
            Assert-Stage5FinalAcceptancePathContained $base $journalPath $journalContext
            $journalSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $journalPath `
                $journalContext
            Assert-Stage5FinalAcceptanceSnapshotSha256 $journalSnapshot `
                ([string]$binding.sha256) $journalContext | Out-Null
            return ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $journalSnapshot `
                $journalContext
        }
        foreach ($outcome in $phaseAttempts.outcomes) {
            Assert-Stage5JsonProperties $outcome @('entryId', 'state', 'failure',
                'startBinding', 'resultBinding') 'Performance phase attempt outcome'
            Assert-Stage5Condition ($outcome.entryId -is [string] -and
                $plannedIds.ContainsKey([string]$outcome.entryId) -and
                -not $outcomeIds.ContainsKey([string]$outcome.entryId) -and
                $outcome.state -ceq 'completed' -and
                $null -eq $outcome.failure -and
                $null -ne $outcome.startBinding -and
                $null -ne $outcome.resultBinding) `
                'Performance phase attempt outcome is incomplete, duplicated, or unplanned.'
            $plannedEntry = $plannedEntriesById[[string]$outcome.entryId]
            Assert-Stage5Condition (
                [IO.Path]::GetFullPath([string]$outcome.startBinding.path) -ceq
                    [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.attemptStartPath) -and
                [IO.Path]::GetFullPath([string]$outcome.resultBinding.path) -ceq
                    [IO.Path]::GetFullPath([string]$plannedEntry.outputPaths.attemptResultPath)) `
                'Performance phase attempt outcome changed its planned journal paths.'
            $start = & $readPhaseJournal $outcome.startBinding `
                'Performance original attempt start'
            $result = & $readPhaseJournal $outcome.resultBinding `
                'Performance original attempt result'
            Assert-Stage5JsonShape $start @('schemaVersion','event','planSha256',
                'entryId','runNonce','recordedUtc','sourceBinding') `
                'Performance original attempt start'
            Assert-Stage5JsonShape $result @('schemaVersion','event','planSha256',
                'entryId','runNonce','recordedUtc','startBinding','state',
                'failure','run','processCleanup') `
                'Performance original attempt result'
            $journalRun = $result.run
            $aggregateEntry = $actualEntriesById[
                [string]$plannedEntry.entryId]
            Assert-Stage5JsonShape $journalRun @('fixtureId','lane','ordinal',
                'warmup','runId','runNonce','expectedArgumentString',
                'receiptPath','receiptSha256','host') `
                'Performance original attempt run'
            Assert-Stage5JsonShape $journalRun.host @('processId',
                'creationTimeUtc100ns','executablePath','executableSha256',
                'commandLine','parentProcessId','parentCreationTimeUtc100ns',
                'argumentString','exitCode','elapsedMilliseconds',
                'rawLogSha256','timingSha256') `
                'Performance original attempt host observation'
            Assert-Stage5JsonShape $result.processCleanup @('processId',
                'exitProof','blocked','errors') `
                'Performance original attempt process cleanup'
            Assert-Stage5Condition ($journalRun.runId -ceq
                    [string]$plannedEntry.entryId -and
                $journalRun.runNonce -ceq [string]$plannedEntry.runNonce -and
                $journalRun.fixtureId -ceq [string]$aggregateEntry.run.fixtureId -and
                $journalRun.lane -ceq [string]$aggregateEntry.run.lane -and
                $journalRun.ordinal -eq $aggregateEntry.run.ordinal -and
                $journalRun.warmup -is [bool] -and
                $journalRun.warmup -eq $aggregateEntry.run.warmup -and
                $journalRun.expectedArgumentString -ceq
                    [string]$aggregateEntry.run.expectedArgumentString -and
                [IO.Path]::GetFullPath([string]$journalRun.receiptPath) -ceq
                    [IO.Path]::GetFullPath([string]$aggregateEntry.binding.path) -and
                $journalRun.receiptSha256 -ceq
                    [string]$aggregateEntry.binding.sha256 -and
                (Test-Stage5JsonInteger $journalRun.host.processId) -and
                $journalRun.host.processId -eq $aggregateEntry.run.processId -and
                $journalRun.host.processId -eq $aggregateEntry.binding.processId -and
                (Test-Stage5JsonInteger $journalRun.host.creationTimeUtc100ns) -and
                $journalRun.host.creationTimeUtc100ns -eq
                    $aggregateEntry.run.processCreationTimeUtc100ns -and
                $journalRun.host.creationTimeUtc100ns -eq
                    $aggregateEntry.binding.processCreationTimeUtc100ns -and
                [String]::Equals([IO.Path]::GetFullPath(
                        [string]$journalRun.host.executablePath),
                    [IO.Path]::GetFullPath(
                        [string]$aggregateEntry.binding.executablePath),
                    [StringComparison]::OrdinalIgnoreCase) -and
                $journalRun.host.executableSha256 -ceq
                    [string]$aggregateEntry.binding.executableSha256 -and
                $journalRun.host.commandLine -ceq
                    [string]$aggregateEntry.binding.commandLine -and
                $journalRun.host.argumentString -ceq
                    [string]$plannedEntry.expectedArgumentString -and
                (Test-Stage5JsonInteger $journalRun.host.parentProcessId) -and
                [Int64]$journalRun.host.parentProcessId -ge 0 -and
                (Test-Stage5JsonInteger $journalRun.host.parentCreationTimeUtc100ns) -and
                [Int64]$journalRun.host.parentCreationTimeUtc100ns -ge 0 -and
                (Test-Stage5JsonInteger $journalRun.host.exitCode) -and
                $journalRun.host.exitCode -eq 0 -and
                (Test-Stage5JsonNumber $journalRun.host.elapsedMilliseconds) -and
                [double]$journalRun.host.elapsedMilliseconds -eq
                    [double]$aggregateEntry.run.elapsedMilliseconds -and
                $journalRun.host.rawLogSha256 -ceq
                    [string]$aggregateEntry.run.rawLogSha256 -and
                $journalRun.host.timingSha256 -ceq
                    [string]$aggregateEntry.run.timingSha256 -and
                (Test-Stage5JsonInteger $result.processCleanup.processId) -and
                $result.processCleanup.processId -eq $journalRun.host.processId -and
                $result.processCleanup.exitProof -is [bool] -and
                [bool]$result.processCleanup.exitProof -and
                $result.processCleanup.blocked -is [bool] -and
                -not [bool]$result.processCleanup.blocked -and
                $result.processCleanup.errors -is [Array] -and
                @($result.processCleanup.errors).Count -eq 0) `
                'Performance original attempt run is detached from its plan, host observation, or validated receipt.'
            Assert-Stage5Condition ((Test-Stage5JsonInteger $start.schemaVersion) -and
                $start.schemaVersion -eq 1 -and
                $start.event -ceq 'attempt-start' -and
                $start.planSha256 -ceq [string]$aggregate.phaseBaselinePlan.sha256 -and
                $start.entryId -ceq [string]$plannedEntry.entryId -and
                $start.runNonce -ceq [string]$plannedEntry.runNonce -and
                (Test-Stage5JsonInteger $result.schemaVersion) -and
                $result.schemaVersion -eq 1 -and
                $result.event -ceq 'attempt-result' -and
                $result.planSha256 -ceq [string]$aggregate.phaseBaselinePlan.sha256 -and
                $result.entryId -ceq [string]$plannedEntry.entryId -and
                $result.runNonce -ceq [string]$plannedEntry.runNonce -and
                $result.state -ceq 'completed' -and $null -eq $result.failure -and
                ($result.startBinding | ConvertTo-Json -Compress -Depth 10) -ceq
                    ($outcome.startBinding | ConvertTo-Json -Compress -Depth 10)) `
                'Performance phase journal is detached from its plan or exact aggregate run.'
            $outcomeIds[[string]$outcome.entryId] = $true
        }
        Assert-Stage5Condition ($plannedIds.Count -eq $entries.Count -and
            $outcomeIds.Count -eq $entries.Count -and
            @($entries | Where-Object {
                -not $plannedIds.ContainsKey([string]$_.run.runId) -or
                -not $outcomeIds.ContainsKey([string]$_.run.runId) -or
                $plannedIds[[string]$_.run.runId] -cne [string]$_.role
            }).Count -eq 0) `
            'Performance phase plan/attempt journal differs from the complete aggregate role set.'
    }
    $runs = @()
    $nativeByRunId = @{}; $diagnosticsByRunId = @{}
    $seenProcesses = @{}; $seenRunIds = @{}; $seenNonces = @{}; $seenReceipts = @{}
    $cohort = $null
    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $run = $entries[$index].run
        $context = "Performance diagnostic run $index"
        Assert-Stage5JsonProperties $run @('fixtureId', 'lane', 'ordinal', 'warmup', 'runId',
            'expectedArgumentString',
            'processId', 'processCreationTimeUtc100ns', 'elapsedMilliseconds', 'receiptPath',
            'receiptSha256', 'receiptBinding', 'rawLogPath', 'rawLogSha256', 'timingPath', 'timingSha256',
            'selectedWorkerCpuSetIds', 'selectedPhysicalCoreMask') $context
        $binding = $run.receiptBinding
        $outerBinding = $entries[$index].binding
        $bindingFields = @('path', 'sha256', 'runId', 'runNonce', 'cohortNonce', 'processId',
            'processCreationTimeUtc100ns', 'executablePath', 'executableSha256', 'commandLine',
            'rawLogPath', 'rawLogSha256', 'timingPath', 'timingSha256')
        Assert-Stage5JsonShape $binding $bindingFields $context
        Assert-Stage5JsonShape $outerBinding $bindingFields $context
        foreach ($field in $bindingFields) {
            Assert-Stage5Condition ($binding[$field] -ceq $outerBinding[$field]) "$context has conflicting receipt bindings."
        }
        $recordedReceiptPath = [IO.Path]::GetFullPath([string]$run.receiptPath)
        Assert-Stage5Condition ($recordedReceiptPath -ceq
                [IO.Path]::GetFullPath([string]$binding.path)) `
            "$context receipt path differs from its immutable binding."
        $receiptPath = & $resolveTaskEvidence $recordedReceiptPath $context
        Assert-Stage5FinalAcceptancePathContained $base $receiptPath $context
        Assert-Stage5Condition ($run.receiptSha256 -ceq $binding.sha256 -and
            -not $seenReceipts.ContainsKey($receiptPath)) `
            "$context repeats or detaches a receipt."
        $nativeSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $receiptPath $context
        $nativeHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $nativeSnapshot $binding.sha256 $context
        $native = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $nativeSnapshot $context
        $plannedEntry = if ($hasPhaseBaselines) {
            $plannedEntriesById[[string]$run.runId]
        } else { $null }
        $nativeProperties = @('schemaVersion', 'producer', 'producerVersion', 'evidenceKind',
            'status', 'title', 'sourceCommit', 'artifactSetSha256', 'executablePath', 'executableSha256',
            'runId', 'runNonce', 'cohortNonce', 'commandLine', 'process', 'fixture', 'frames', 'workload',
            'frameSimulation', 'phases', 'kernelTiming', 'measurementRole', 'kernelReference', 'rawEvidence',
            'runtimeClosure', 'worker', 'topology', 'cohortCreatedUtc', 'architecture', 'simulationMode', 'schedulerStarted')
        $nativeSchema = Get-Stage5JsonValue $native 'schemaVersion' $context
        Assert-Stage5Condition ((Test-Stage5JsonInteger $nativeSchema) -and
            @([Int64]5, [Int64]6) -ccontains [Int64]$nativeSchema) `
            "$context native schema version is unsupported."
        $nativeVersion = [string][Int64]$nativeSchema
        if ($nativeSchema -eq 6) { $nativeProperties += @('phaseAccounting', 'attemptTrace') }
        Assert-Stage5JsonProperties $native $nativeProperties $context
        Assert-Stage5Condition ($native.producer -ceq
            "game-executable-stage5-performance-report-v$nativeVersion" -and
            $native.producerVersion -ceq $nativeVersion -and $native.status -ceq 'passed' -and
            $native.evidenceKind -ceq 'stage5-executable-originated-receipt' -and
            $native.title -ceq $ExpectedTitle -and $native.sourceCommit -ceq $ExpectedSourceCommit -and
            $native.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
            $native.executableSha256 -ceq $ExpectedExecutableSha256 -and
            $native.executablePath -ceq $aggregate.executable.path) "$context native provenance does not match the host artifact."
        $selectedPhaseThroughput = $entries[$index].role -ceq 'throughput' -and
            $hasPhaseBaselines -and
            $phasePairsByThroughput.ContainsKey([string]$native.runId)
        $expectedNativeSchema = if ($entries[$index].role -ceq
            'phase-serial-baseline' -or $selectedPhaseThroughput) { 6 } else { 5 }
        Assert-Stage5Condition ($nativeSchema -eq $expectedNativeSchema) `
            "$context native schema version is detached from the aggregate phase-baseline contract."
        Assert-Stage5Condition ($native.measurementRole -ceq $entries[$index].role) `
            "$context measurement role cannot cross the throughput/oracle boundary."
        if ($hasPhaseBaselines) {
            Assert-Stage5Condition ($null -ne $plannedEntry -and
                $native.worker.requestedCount -eq $plannedEntry.workerCount) `
                "$context native worker request differs from the frozen plan."
        }
        if ($entries[$index].role -ceq 'phase-serial-baseline') {
            Assert-Stage5Condition ($null -ne $native.attemptTrace) `
                "$context phase baseline stripped its consumed attempt trace."
            [void](Assert-Stage5PhaseAccountingContract $native $context)
        }
        elseif ($entries[$index].role -ceq 'throughput' -and $hasPhaseBaselines) {
            Assert-Stage5Condition (($selectedPhaseThroughput -and
                    $null -ne $native.attemptTrace) -or
                (-not $selectedPhaseThroughput -and $nativeSchema -eq 5)) `
                "$context phase source trace selection is detached from the aggregate pair set."
        }
        if ($nativeSchema -eq 6 -and $null -ne $native.attemptTrace) {
            Assert-Stage5JsonProperties $native.attemptTrace @('file') `
                "$context attempt trace"
            Assert-Stage5JsonShape $native.attemptTrace.file `
                @('path', 'sha256', 'byteCount') "$context attempt trace file"
            $tracePathText = [string]$native.attemptTrace.file.path
            $recordedTracePath = if ([IO.Path]::IsPathRooted($tracePathText)) {
                [IO.Path]::GetFullPath($tracePathText)
            }
            else {
                [IO.Path]::GetFullPath((Join-Path $recordedBase $tracePathText))
            }
            $tracePath = & $resolveTaskEvidence $recordedTracePath `
                "$context attempt trace file"
            Assert-Stage5FinalAcceptancePathContained $base $tracePath `
                "$context attempt trace file"
            Assert-Stage5Condition ($native.attemptTrace.file.sha256 -is [string] -and
                [string]$native.attemptTrace.file.sha256 -cmatch
                    '^[0-9A-Fa-f]{64}$' -and
                (Test-Stage5JsonInteger $native.attemptTrace.file.byteCount) -and
                [Int64]$native.attemptTrace.file.byteCount -gt 0) `
                "$context attempt trace file binding is malformed."
            $traceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $tracePath `
                "$context attempt trace file" -HashOnly -EvidenceKind Trace
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 $traceSnapshot `
                ([string]$native.attemptTrace.file.sha256) `
                ([Int64]$native.attemptTrace.file.byteCount) `
                "$context attempt trace file" | Out-Null
            $tracePair = if ($entries[$index].role -ceq 'throughput') {
                $phasePairsByThroughput[[string]$native.runId]
            }
            else {
                $phasePairsByBaseline[[string]$native.runId]
            }
            Assert-Stage5Condition ($null -ne $tracePair -and
                $phaseProfilesById.ContainsKey([string]$tracePair.profileId)) `
                "$context attempt trace has no unique reviewed profile pair."
            [void](Assert-Stage5PerformancePhaseTraceSemantics $native `
                $phaseProfilesById[[string]$tracePair.profileId] $context)
        }
        Assert-Stage5Condition ($native.architecture -ceq 'x64') "$context native architecture differs."
        Assert-Stage5RuntimeClosureBinding $native.runtimeClosure $inputBindings "$context pair provenance" | Out-Null
        Assert-Stage5NativeFixtureObservation $native $context -RequireScaling
        Assert-Stage5NativeSchedulerObservation $native $context
        Assert-Stage5JsonProperties $native.schedulerMetrics @(
            'affinityFailureCount') "$context scheduler metrics"
        Assert-Stage5DiagnosticCounter $native.schedulerMetrics.affinityFailureCount `
            "$context affinity failures"
        Assert-Stage5Condition ($native.schedulerMetrics.affinityFailureCount -eq 0) `
            "$context pinned physical lane recorded an affinity failure."
        if ([string]$run.lane -cne 'forced-one' -and
            $entries[$index].role -cne 'phase-serial-baseline') {
            $scheduler = $native.schedulerMetrics
            Assert-Stage5DiagnosticRealMulticoreKernelEvidence $native $context
            foreach ($field in @('submittedJobCount','executedJobCount',
                'workerBusyNanoseconds','maximumActiveWorkers','failedJobCount',
                'cancelledJobCount','serialFallbackCount','workerWaitRejectionCount',
                'selectedWorkerPhysicalCoreMask')) {
                Assert-Stage5DiagnosticCounter $scheduler[$field] `
                    "$context scheduler $field"
            }
            Assert-Stage5Condition ($native.worker.pinned -is [bool] -and
                $native.worker.pinned -and $native.worker.effectiveCount -gt 1 -and
                $native.worker.selectedWorkerCpuCount -gt 1 -and
                $native.worker.selectedWorkerPhysicalCoreCount -gt 1 -and
                $native.worker.selectedWorkerPhysicalCoreMask -ne 0 -and
                (Get-Stage5UInt64BitCount $native.worker.selectedWorkerPhysicalCoreMask) -eq
                    $native.worker.selectedWorkerPhysicalCoreCount -and
                $scheduler.submittedJobCount -gt 0 -and
                $scheduler.executedJobCount -eq $scheduler.submittedJobCount -and
                $scheduler.workerBusyNanoseconds -gt 0 -and
                $scheduler.maximumActiveWorkers -gt 1 -and
                $scheduler.maximumActiveWorkers -le $native.worker.effectiveCount -and
                $scheduler.selectedWorkerPhysicalCoreMask -eq
                    $native.worker.selectedWorkerPhysicalCoreMask -and
                $scheduler.failedJobCount -eq 0 -and
                $scheduler.cancelledJobCount -eq 0 -and
                $scheduler.serialFallbackCount -eq 0 -and
                $scheduler.workerWaitRejectionCount -eq 0) `
                "$context does not prove that all six kernel families used physical workers and the run demonstrated scheduler-wide multicore concurrency."
        }
        foreach ($field in @('runId', 'runNonce', 'cohortNonce', 'executablePath', 'executableSha256', 'commandLine')) {
            Assert-Stage5Condition ($native[$field] -is [string] -and
                -not [string]::IsNullOrWhiteSpace($native[$field]) -and $native[$field] -ceq $binding[$field]) `
                "$context native identity differs from its retained host observation."
        }
        $process = $native.process
        Assert-Stage5DiagnosticCounter $run.ordinal "$context ordinal"
        Assert-Stage5DiagnosticCounter $process.id "$context process id"
        Assert-Stage5DiagnosticCounter $process.creationTimeUtc100ns "$context process creation time"
        Assert-Stage5Condition ($process.id -gt 0 -and $process.creationTimeUtc100ns -gt 0 -and
            $process.identityAvailable -is [bool] -and $process.identityAvailable -and
            $process.exitCodeKnown -is [bool] -and $process.exitCodeKnown -and $process.exitCode -eq 0 -and
            $process.id -eq $binding.processId -and $process.id -eq $run.processId -and
            $process.creationTimeUtc100ns -eq $binding.processCreationTimeUtc100ns -and
            $process.creationTimeUtc100ns -eq $run.processCreationTimeUtc100ns -and
            $native.runId -ceq $run.runId -and
            $run.fixtureId -is [string] -and -not [string]::IsNullOrWhiteSpace($run.fixtureId) -and
            $run.lane -is [string] -and -not [string]::IsNullOrWhiteSpace($run.lane) -and
            -not $seenProcesses.ContainsKey([string]$process.id) -and
            -not $seenRunIds.ContainsKey($native.runId) -and -not $seenNonces.ContainsKey($native.runNonce) -and
            $run.warmup -is [bool] -and (Test-Stage5JsonNumber $run.elapsedMilliseconds) -and
            $run.elapsedMilliseconds -gt 0) "$context process identity, completion, or elapsed observation is invalid."
        if ($null -eq $cohort) { $cohort = $native.cohortNonce }
        Assert-Stage5Condition ($cohort -ceq $native.cohortNonce) "$context mixes execution cohorts."
        $seenProcesses[[string]$process.id] = $true; $seenRunIds[$native.runId] = $true
        $seenNonces[$native.runNonce] = $true; $seenReceipts[$receiptPath] = $true
        $workload = $native.workload; $frames = $native.frames
        Assert-Stage5Condition ($inputBindings.fixtures.ContainsKey(
                [string]$native.fixture.id)) "$context fixture is not reviewed."
        $reviewedFixture = $inputBindings.fixtures[$native.fixture.id]
        Assert-Stage5Condition ($native.fixture.contentSha256 -ceq $reviewedFixture.sha256 -and
            $native.fixture.seedKnown -is [bool] -and $native.fixture.seedKnown -and
            (Test-Stage5JsonInteger $native.fixture.seed) -and $native.fixture.seed -eq $reviewedFixture.seed -and
            [String]::Equals([IO.Path]::GetFullPath([string]$native.fixture.replayPath), $reviewedFixture.path, [StringComparison]::OrdinalIgnoreCase) -and
            $native.fixture.requestedPlayerCount -eq $reviewedFixture.playerCount -and
            $native.fixture.requestedMinimumUnitCount -eq $reviewedFixture.minimumUnitCount) `
            "$context fixture identity does not match the retained reviewed fixture binding."
        Assert-Stage5Condition ($native.worker.policy -is [string] -and @('auto', 'all') -ccontains $native.worker.policy -and
            $native.worker.pinned -is [bool] -and $native.worker.pinned -and
            $native.worker.effectiveCount -gt 0 -and $native.worker.effectiveCount -eq $native.worker.selectedWorkerCpuCount -and
            $native.worker.selectedWorkerCpuCount -eq $native.worker.selectedWorkerPhysicalCoreCount -and
            $native.worker.selectedWorkerPhysicalCoreMaskComplete -is [bool] -and $native.worker.selectedWorkerPhysicalCoreMaskComplete -and
            $native.topology.source -ceq 'GetSystemCpuSetInformation' -and
            $native.topology.selectedWorkerCpuSetIds -is [Array] -and
            $native.topology.selectedWorkerCpuSetIds.Count -eq $native.worker.selectedWorkerCpuCount -and
            $run.selectedWorkerCpuSetIds -is [Array] -and
            (($run.selectedWorkerCpuSetIds | ConvertTo-Json -Compress) -ceq ($native.topology.selectedWorkerCpuSetIds | ConvertTo-Json -Compress)) -and
            $run.selectedPhysicalCoreMask -ceq ([UInt64]$native.worker.selectedWorkerPhysicalCoreMask).ToString('X16')) `
            "$context worker/topology observation is not bound to the selected production policy."
        Assert-Stage5DiagnosticCounter $native.fixture.requestedPlayerCount "$context requested player count"
        Assert-Stage5DiagnosticCounter $native.fixture.requestedMinimumUnitCount "$context requested minimum units"
        foreach ($field in @('start', 'end', 'final', 'finalCrc')) {
            Assert-Stage5DiagnosticCounter $frames[$field] "$context frame $field"
        }
        foreach ($field in @('sampleCount', 'firstFrame', 'lastFrame', 'playerCount',
            'initialUnitCount', 'minimumUnitCount', 'peakUnitCount')) {
            Assert-Stage5DiagnosticCounter $workload[$field] "$context workload $field"
        }
        Assert-Stage5Condition ($native.fixture.id -ceq $run.fixtureId -and
            $native.fixture.requestedPlayerCount -eq 8 -and $workload.playerCount -eq 8 -and
            $workload.sampling -ceq 'completed-simulation-frame-boundary-v1' -and
            $workload.rosterStable -is [bool] -and $workload.rosterStable -and
            $workload.contiguous -is [bool] -and $workload.contiguous -and
            $frames.end -gt $frames.start -and $frames.final -eq $frames.end -and
            $frames.finalCrcKnown -is [bool] -and $frames.finalCrcKnown -and
            [decimal]$workload.sampleCount -eq ([decimal]$frames.end - [decimal]$frames.start) -and
            $workload.firstFrame -eq ([decimal]$frames.start + 1) -and $workload.lastFrame -eq $frames.end -and
            $native.fixture.requestedMinimumUnitCount -gt 0 -and
            $workload.initialUnitCount -ge $native.fixture.requestedMinimumUnitCount -and
            $workload.minimumUnitCount -le $workload.initialUnitCount -and
            $workload.peakUnitCount -ge $workload.initialUnitCount) "$context completed-frame workload is inconsistent."
        $raw = $native.rawEvidence
        Assert-Stage5Condition ($raw.timingClosed -is [bool] -and $raw.timingClosed -and
            $raw.timingWriteSucceeded -is [bool] -and $raw.timingWriteSucceeded -and
            $raw.timingTruncated -is [bool] -and -not $raw.timingTruncated -and
            $raw.timingComplete -is [bool] -and $raw.timingComplete -and
            $raw.timingSessionCount -eq 1 -and $raw.timingFrameSamples -ge $workload.sampleCount -and
            $raw.timingFirstFrame -le $frames.start -and $raw.timingLastFrame -ge $frames.end) `
            "$context raw timing was not finalized with complete replay coverage."
        foreach ($prefix in @('rawLog', 'timing')) {
            $pathField = $prefix + 'Path'; $hashField = $prefix + 'Sha256'
            Assert-Stage5Condition ($raw[$pathField] -ceq $binding[$pathField] -and
                $raw[$hashField] -ceq $binding[$hashField] -and
                $run[$pathField] -ceq $binding[$pathField] -and $run[$hashField] -ceq $binding[$hashField]) `
                "$context raw file binding is inconsistent."
            $recordedRawPath = [IO.Path]::GetFullPath([string]$raw[$pathField])
            $rawPath = & $resolveTaskEvidence $recordedRawPath "$context $prefix"
            Assert-Stage5FinalAcceptancePathContained $base $rawPath $context
            $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
                "$context $prefix" -EvidenceKind RawLog
            Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot $raw[$hashField] "$context $prefix" | Out-Null
        }
        $frameTiming = $native.frameSimulation
        foreach ($field in @('totalNanoseconds', 'maximumNanoseconds', 'sampleCount')) {
            Assert-Stage5DiagnosticCounter $frameTiming[$field] "$context frame timing $field"
        }
        Assert-Stage5Condition ($frameTiming.totalNanoseconds -gt 0 -and
            $frameTiming.maximumNanoseconds -gt 0 -and
            $frameTiming.maximumNanoseconds -le $frameTiming.totalNanoseconds -and
            $frameTiming.sampleCount -ge $workload.sampleCount -and
            $native.phases -is [Array] -and $native.phases.Count -eq 5) "$context frame/phase timing is incomplete."
        [decimal]$phaseTotal = 0
        for ($phaseIndex = 0; $phaseIndex -lt 5; ++$phaseIndex) {
            $phase = $native.phases[$phaseIndex]
            foreach ($field in @('totalNanoseconds', 'maximumNanoseconds', 'sampleCount', 'serialNanoseconds')) {
                Assert-Stage5DiagnosticCounter $phase[$field] "$context phase $field"
            }
            Assert-Stage5Condition ($phase.name -ceq $phaseNames[$phaseIndex] -and
                $phase.available -is [bool] -and $phase.serialNanosecondsKnown -is [bool] -and
                $phase.maximumNanoseconds -le $phase.totalNanoseconds -and
                $phase.sampleCount -le $frameTiming.sampleCount -and
                (($phase.serialNanosecondsKnown -and $phase.available -and $phase.serialNanoseconds -le $phase.totalNanoseconds) -or
                    (-not $phase.serialNanosecondsKnown -and $phase.serialNanoseconds -eq 0)) -and
                (($phase.available -and $phase.totalNanoseconds -gt 0 -and $phase.sampleCount -gt 0) -or
                    (-not $phase.available -and $phase.totalNanoseconds -eq 0 -and $phase.sampleCount -eq 0))) `
                "$context owner phase timing is inconsistent or fabricates unknown serial data."
            $phaseTotal += [decimal]$phase.totalNanoseconds
        }
        Assert-Stage5Condition ($phaseTotal -le [decimal]$frameTiming.totalNanoseconds) "$context owner phases overlap frame time."
        Assert-Stage5DiagnosticKernelTiming $native.kernelTiming $frames `
            $native.measurementRole "$context kernel timing"
        Assert-Stage5DiagnosticKernelReference $native.kernelReference $native.kernelTiming `
            $frames $native.measurementRole "$context kernel reference"
        $diagnostic = [pscustomobject]@{ fixtureId = $run.fixtureId; lane = $run.lane; ordinal = $run.ordinal
            warmup = $run.warmup; elapsedMilliseconds = $run.elapsedMilliseconds
            receipt = [pscustomobject]@{ path = $receiptPath; sha256 = $nativeHash }
            runId = $native.runId; processId = $process.id; processCreationTimeUtc100ns = $process.creationTimeUtc100ns
            argumentString = [string]$run.expectedArgumentString
            commandLine = [string]$native.commandLine
            fixtureSha256 = [string]$native.fixture.contentSha256
            selectedWorkerCpuSetIds = @($native.topology.selectedWorkerCpuSetIds)
            selectedPhysicalCoreMask = ([UInt64]$native.worker.selectedWorkerPhysicalCoreMask).ToString('X16')
            requestedMinimumUnitCount = $native.fixture.requestedMinimumUnitCount
            workload = $workload; topology = $native.topology
            frameSimulation = $frameTiming; phases = $native.phases; kernelTiming = $native.kernelTiming
            measurementRole = $native.measurementRole; kernelReference = $native.kernelReference
            phaseAccounting = $(if ($nativeSchema -eq 6) { $native.phaseAccounting } else { $null }) }
        $nativeByRunId[$native.runId] = $native; $diagnosticsByRunId[$native.runId] = $diagnostic
        if ($entries[$index].role -ceq 'throughput') { $runs += $diagnostic }
    }
    if ($hasPhaseBaselines) {
        foreach ($pair in $aggregate.pairedPhaseBaselineBindings) {
            $sourceRun = $scheduled[[string]$pair.throughputRunId]
            $baselineRun = $pair.baselineRun
            $source = $nativeByRunId[[string]$sourceRun.runId]
            $baseline = $nativeByRunId[[string]$baselineRun.runId]
            $profile = $phaseProfilesById[[string]$pair.profileId]
            $pairContext = "Performance phase pair '$($sourceRun.runId)'"
            Assert-Stage5Condition ($null -ne $source -and $null -ne $baseline -and
                $source.measurementRole -ceq 'throughput' -and
                $baseline.measurementRole -ceq 'phase-serial-baseline' -and
                $source.attemptTrace.mode -ceq 'record' -and
                $baseline.attemptTrace.mode -ceq 'consume') `
                "$pairContext roles or trace modes are invalid."
            foreach ($field in @('title', 'architecture', 'sourceCommit',
                    'artifactSetSha256', 'runtimeClosure', 'executablePath',
                    'executableSha256', 'commandLine', 'cohortNonce',
                    'cohortCreatedUtc', 'simulationMode', 'schedulerStarted',
                    'fixture', 'workload', 'frames', 'worker', 'topology')) {
                Assert-Stage5Condition (($source[$field] | ConvertTo-Json `
                            -Depth 30 -Compress) -ceq
                        ($baseline[$field] | ConvertTo-Json -Depth 30 -Compress)) `
                    "$pairContext differs in $field."
            }
            Assert-Stage5Condition ($source.process.id -ne $baseline.process.id -and
                $source.process.creationTimeUtc100ns -ne
                    $baseline.process.creationTimeUtc100ns -and
                ($source.kernelReference.streams | ConvertTo-Json -Depth 30 -Compress) `
                    -ceq ($baseline.kernelReference.streams | ConvertTo-Json `
                        -Depth 30 -Compress) -and
                $source.kernelTiming.streams.Count -eq
                    $baseline.kernelTiming.streams.Count) `
                "$pairContext reused process identity or changed canonical results."
            for ($streamIndex = 0; $streamIndex -lt
                    $source.kernelTiming.streams.Count; ++$streamIndex) {
                foreach ($field in @('name', 'subtype', 'attemptedBatches',
                        'admittedBatches', 'committedBatches', 'abortedBatches',
                        'firstFrame', 'lastFrame')) {
                    Assert-Stage5Condition (
                        $source.kernelTiming.streams[$streamIndex][$field] -ceq
                        $baseline.kernelTiming.streams[$streamIndex][$field]) `
                        "$pairContext changed timing stream $field."
                }
            }
            $sourceBinding = $baseline.attemptTrace.sourceBinding
            Assert-Stage5Condition ([IO.Path]::GetFullPath(
                    [string]$sourceBinding.receipt.path) -ceq
                    [IO.Path]::GetFullPath([string]$sourceRun.receiptPath) -and
                $sourceBinding.receipt.sha256 -ceq $sourceRun.receiptSha256 -and
                $sourceBinding.runId -ceq $source.runId -and
                $sourceBinding.runNonce -ceq $source.runNonce -and
                $sourceBinding.processId -eq $source.process.id -and
                $sourceBinding.processCreationTimeUtc100ns -eq
                    $source.process.creationTimeUtc100ns -and
                $baseline.attemptTrace.binding.sourcePolicySha256 -ceq
                    $profile.sourcePolicySha256 -and
                $source.fixture.contentSha256 -ceq $profile.fixtureSha256 -and
                $baseline.phaseAccounting.completedFrameCount -eq
                    $profile.window.completedFrameCount -and
                $baseline.phaseAccounting.firstCompletedFrame -eq
                    $profile.window.firstCompletedFrame -and
                $baseline.phaseAccounting.lastCompletedFrame -eq
                    $profile.window.lastCompletedFrame -and
                $baseline.phaseAccounting.controlAccounting.windowCount -eq
                    $profile.window.controlWindowCount) `
                "$pairContext source receipt/profile/window binding changed."
            $sourceTraceNames = if ($source.attemptTrace -is
                [Collections.IDictionary]) { @($source.attemptTrace.Keys) }
                else { @($source.attemptTrace.PSObject.Properties.Name) }
            foreach ($field in $sourceTraceNames) {
                if ($field -ceq 'mode' -or $field -ceq 'sourceBinding') {
                    continue
                }
                Assert-Stage5Condition (($source.attemptTrace[$field] |
                            ConvertTo-Json -Depth 30 -Compress) -ceq
                        ($baseline.attemptTrace[$field] | ConvertTo-Json `
                            -Depth 30 -Compress)) `
                    "$pairContext consumed trace changed source $field."
            }
            foreach ($field in @('limits', 'residentAttemptCapacity',
                    'residentRangeCapacity')) {
                Assert-Stage5Condition (($baseline.attemptTrace[$field] |
                            ConvertTo-Json -Depth 20 -Compress) -ceq
                        ($profile[$field] | ConvertTo-Json -Depth 20 -Compress)) `
                    "$pairContext consumed trace changed reviewed $field."
            }
        }
    }
    if ($isRelocated) {
        $canonicalFixtureIds = @('one-thousand-units', 'four-thousand-units',
            'eight-thousand-units', 'dense-eight-player')
        Assert-Stage5Condition ($inputBindings.fixtures.Count -eq 4 -and
            @($canonicalFixtureIds | Where-Object {
                -not $inputBindings.fixtures.ContainsKey($_)
            }).Count -eq 0) `
            'Relocated performance receipts do not cover the four canonical reviewed fixtures.'
    }
    $pairedDiagnostics = @(); $kernelSamples = @(); $allKernelCoverage = $true
    foreach ($throughput in $runs) {
        if (-not $pairsByThroughput.ContainsKey($throughput.runId)) { continue }
        $oracleRun = $pairsByThroughput[$throughput.runId]
        $oracle = $diagnosticsByRunId[$oracleRun.runId]
        $left = $nativeByRunId[$throughput.runId]; $right = $nativeByRunId[$oracleRun.runId]
        $context = "Performance pair '$($throughput.runId)'"
        Assert-Stage5Condition ($throughput.fixtureId -ceq $oracle.fixtureId -and $throughput.lane -ceq $oracle.lane -and
            $throughput.ordinal -eq $oracle.ordinal -and $throughput.warmup -eq $oracle.warmup -and
            $throughput.processCreationTimeUtc100ns -ne $oracle.processCreationTimeUtc100ns) "$context scheduled identity differs."
        foreach ($field in @('commandLine', 'cohortCreatedUtc', 'cohortNonce', 'architecture')) {
            Assert-Stage5Condition ($left[$field] -ceq $right[$field]) "$context production $field does not match."
        }
        foreach ($field in @('fixture', 'workload', 'frames', 'worker', 'topology')) {
            Assert-Stage5Condition (($left[$field] | ConvertTo-Json -Depth 30 -Compress) -ceq
                ($right[$field] | ConvertTo-Json -Depth 30 -Compress)) "$context production $field does not match."
        }
        $leftStreams = $left.kernelReference.streams; $rightStreams = $right.kernelReference.streams
        Assert-Stage5Condition ($leftStreams.Count -gt 0 -and $leftStreams.Count -eq $rightStreams.Count) "$context canonical reference stream coverage is missing."
        $oracleStreams = @{}; $timingStreams = @{}; $oracleTimingStreams = @{}
        foreach ($stream in $rightStreams) { $oracleStreams["$($stream.name)|$($stream.subtype)"] = $stream }
        foreach ($stream in $left.kernelTiming.streams) { $timingStreams["$($stream.name)|$($stream.subtype)"] = $stream }
        foreach ($stream in $right.kernelTiming.streams) { $oracleTimingStreams["$($stream.name)|$($stream.subtype)"] = $stream }
        Assert-Stage5Condition ($timingStreams.Count -eq $oracleTimingStreams.Count) "$context timing stream coverage differs."
        foreach ($key in $timingStreams.Keys) {
            Assert-Stage5Condition ($oracleTimingStreams.ContainsKey($key)) "$context timing stream is unmatched."
            foreach ($field in @('attemptedBatches', 'admittedBatches', 'committedBatches', 'abortedBatches', 'firstFrame', 'lastFrame')) {
                Assert-Stage5Condition ($timingStreams[$key][$field] -eq $oracleTimingStreams[$key][$field]) "$context timing disposition does not match."
            }
        }
        $sampleByKernel = @{}; $committedReferences = @{}
        foreach ($stream in $leftStreams) {
            $key = "$($stream.name)|$($stream.subtype)"
            Assert-Stage5Condition ($oracleStreams.ContainsKey($key)) "$context canonical reference stream is unmatched."
            $matched = $oracleStreams[$key]
            foreach ($field in @('name', 'subtype', 'fieldSchema', 'firstFrame', 'lastFrame', 'validatedBatchCount',
                'committedBatchCount', 'abortedBatchCount', 'validatedOperationCount', 'committedOperationCount',
                'inputSha256', 'outputSha256', 'commitSha256')) {
                Assert-Stage5Condition ($stream[$field] -ceq $matched[$field]) "$context canonical reference $field does not match."
            }
            if ($stream.committedBatchCount -gt 0) { $committedReferences[$key] = $true }
            if (-not $sampleByKernel.ContainsKey($stream.name)) {
                $sampleByKernel[$stream.name] = [pscustomobject]@{ fixtureId = $throughput.fixtureId; lane = $throughput.lane
                    name = $stream.name; pipelineNanoseconds = [decimal]0; serialNanoseconds = [decimal]0; committedBatches = [decimal]0 }
            }
            $sample = $sampleByKernel[$stream.name]
            # Sum actual per-run exclusive components/subtypes before taking a
            # median. Inclusive oracle latency never enters this denominator.
            foreach ($stage in $timingStreams[$key].stages) { $sample.pipelineNanoseconds += [decimal]$stage.totalNanoseconds }
            $sample.serialNanoseconds += [decimal]$matched.serialNanoseconds
            $sample.committedBatches += [decimal]$stream.committedBatchCount
        }
        if (-not $throughput.warmup) {
            foreach ($name in @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')) {
                if (-not $sampleByKernel.ContainsKey($name) -or $sampleByKernel[$name].committedBatches -eq 0) { $allKernelCoverage = $false }
            }
            foreach ($key in $timingStreams.Keys) {
                if ($timingStreams[$key].committedBatches -gt 0 -and -not $committedReferences.ContainsKey($key)) { $allKernelCoverage = $false }
            }
            foreach ($sample in $sampleByKernel.Values) { $kernelSamples += $sample }
        }
        $pairedDiagnostics += [pscustomobject]@{ throughputRunId = $throughput.runId; oracleRun = $oracle }
    }
    $kernelComparisons = @()
    foreach ($group in @($kernelSamples | Group-Object fixtureId, lane, name | Sort-Object Name)) {
        $pipeline = Get-Stage5Median @($group.Group | ForEach-Object { [double]$_.pipelineNanoseconds })
        $serial = Get-Stage5Median @($group.Group | ForEach-Object { [double]$_.serialNanoseconds })
        $ratioKnown = $pipeline -gt 0 -and $serial -gt 0
        $kernelComparisons += [pscustomobject]@{ fixtureId = $group.Group[0].fixtureId; lane = $group.Group[0].lane
            name = $group.Group[0].name; measuredPairs = $group.Count; medianPipelineNanoseconds = $pipeline
            medianSerialNanoseconds = $serial; netSpeedupKnown = $ratioKnown
            netSpeedup = $(if ($ratioKnown) { $serial / $pipeline } else { $null }) }
    }
    $lanes = @()
    foreach ($group in @($runs | Group-Object fixtureId, lane)) {
        $measured = @($group.Group | Where-Object { -not $_.warmup })
        Assert-Stage5Condition ($measured.Count -gt 0) 'Performance diagnostics contain a lane without a measured run.'
        $lanes += [pscustomobject]@{ fixtureId = $measured[0].fixtureId; lane = $measured[0].lane
            measuredRuns = $measured.Count; warmupRuns = $group.Count - $measured.Count
            medianElapsedMilliseconds = Get-Stage5Median @($measured | ForEach-Object { [double]$_.elapsedMilliseconds }) }
    }
    if ($RequireAcceptance) {
        throw 'V5 diagnostics cannot establish scaling acceptance: independent whole-frame phase serial coverage remains unavailable.'
    }
    $pairedPhaseDiagnostics = @()
    if ($hasPhaseBaselines) {
        foreach ($pair in $aggregate.pairedPhaseBaselineBindings) {
            $pairedPhaseDiagnostics += [pscustomobject]@{
                profileId = [string]$pair.profileId
                throughputRunId = [string]$pair.throughputRunId
                baselineRun = $diagnosticsByRunId[[string]$pair.baselineRun.runId]
            }
        }
    }
    $reviewedFixtureDiagnostics = @()
    foreach ($fixtureId in @('one-thousand-units', 'four-thousand-units',
            'eight-thousand-units', 'dense-eight-player')) {
        Assert-Stage5Condition ($inputBindings.fixtures.ContainsKey($fixtureId)) `
            "Performance diagnostics lack reviewed fixture '$fixtureId'."
        $reviewed = $inputBindings.fixtures[$fixtureId]
        $reviewedFixtureDiagnostics += [pscustomobject]@{
            id = $fixtureId
            recordedPath = [IO.Path]::GetFullPath([string]$reviewed.path)
            sha256 = [string]$reviewed.sha256
            seed = [UInt32]$reviewed.seed
            playerCount = [int]$reviewed.playerCount
            minimumUnitCount = [int]$reviewed.minimumUnitCount
        }
    }
    return [pscustomobject]@{
        schemaVersion = 3; evidenceKind = 'stage5-performance-scaling-diagnostics'; status = 'diagnostic'
        finalAcceptanceClaim = $false; qualificationMode = $aggregate.qualificationMode
        sourceCommit = $ExpectedSourceCommit; artifactSetSha256 = $ExpectedArtifactSetSha256
        executableSha256 = $ExpectedExecutableSha256; title = $ExpectedTitle
        measurementMode = 'headless-throughput'; installedRuntime = $true
        hostAggregate = [pscustomobject]@{ path = $full; sha256 = $hostHash }
        referencePolicy = $aggregate.referencePolicy
        blockedBy = @('phase-serial-coverage-unknown') + $(if ($pairedDiagnostics.Count -eq 0) {
            @('same-input-serial-reference-unavailable')
        } elseif (-not $allKernelCoverage) { @('kernel-reference-coverage-incomplete') } else { @() })
        lanes = $lanes; runs = $runs; observedRuns = @($diagnosticsByRunId.Values)
        pairedOracleBindings = $pairedDiagnostics
        pairedPhaseBaselineBindings = $pairedPhaseDiagnostics
        kernelComparisons = $kernelComparisons
        fixtureManifest = [pscustomobject]@{
            recordedPath = [IO.Path]::GetFullPath(
                [string]$aggregate.fixtureManifest.path)
            sha256 = [string]$aggregate.fixtureManifest.sha256
        }
        reviewedFixtures = $reviewedFixtureDiagnostics
    }
}

function Read-Stage5PerformanceScalingEvidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedStage3BaselineSha256,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle = 'ZeroHour',
        [object]$Snapshot = $null, [string]$ExpectedSnapshotSha256 = '',
        [string]$ExpectedCohortNonce = '',
        [string]$ExpectedCohortCreatedUtc = '',
        [object]$ExpectedRuntimeClosure = $null,
        [string]$ExpectedPhaseBaselineProfileSha256 = ''
    )
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 5 scaling evidence'
    }
    else {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) `
            'Stage 5 scaling evidence caller-supplied snapshot must include its independently expected SHA-256.'
    }
    $snapshotPath = if ($Snapshot.PSObject.Properties.Name -contains 'path') {
        $Snapshot.path
    }
    else { $null }
    Assert-Stage5Condition ($Snapshot.PSObject.Properties.Name -contains 'path' -and
        $snapshotPath -is [string] -and
        [IO.Path]::GetFullPath($snapshotPath) -ceq $full) `
        'Stage 5 scaling evidence snapshot is bound to a different path.'
    $snapshotSha256 = if ($Snapshot.PSObject.Properties.Name -contains 'sha256') {
        $Snapshot.sha256
    }
    else { $null }
    Assert-Stage5Condition ($snapshotSha256 -is [string] -and
        $snapshotSha256 -cmatch '^[0-9A-F]{64}$') `
        'Stage 5 scaling evidence snapshot SHA-256 must be a JSON string.'
    if ([string]::IsNullOrWhiteSpace($ExpectedSnapshotSha256)) {
        $ExpectedSnapshotSha256 = $snapshotSha256
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot $ExpectedSnapshotSha256 `
        'Stage 5 scaling evidence' | Out-Null
    Assert-Stage5Condition ($ExpectedSourceCommit -match '^[0-9a-f]{40}$') `
        'ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    foreach ($binding in @(
        @('ExpectedArtifactSetSha256', $ExpectedArtifactSetSha256),
        @('ExpectedExecutableSha256', $ExpectedExecutableSha256),
        @('ExpectedStage3BaselineSha256', $ExpectedStage3BaselineSha256),
        @('ExpectedPhaseBaselineProfileSha256',
            $ExpectedPhaseBaselineProfileSha256)
    )) {
        Assert-Stage5Condition ($binding[1] -match '^[0-9A-F]{64}$') `
            "$($binding[0]) must be an independently supplied uppercase SHA-256."
    }
    Assert-Stage5Condition ($ExpectedCohortNonce -cmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
        'ExpectedCohortNonce must be an independently supplied lowercase RFC 4122 version-4 UUID.'
    [DateTimeOffset]$parsedExpectedCohortCreatedUtc = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ($ExpectedCohortCreatedUtc -cmatch
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        [DateTimeOffset]::TryParseExact($ExpectedCohortCreatedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$parsedExpectedCohortCreatedUtc)) `
        'ExpectedCohortCreatedUtc must be an independently supplied canonical UTC timestamp.'
    $verifiedExpectedRuntimeClosure = Assert-Stage5RuntimeClosureBinding `
        $ExpectedRuntimeClosure $ExpectedRuntimeClosure `
        'Stage 5 scaling independently expected'
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind', 'status',
        'recordedUtc', 'cohortNonce', 'cohortCreatedUtc', 'qualificationMode',
        'referencePolicy', 'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'title', 'executableSha256', 'stage3SourceCommit',
        'stage3ExecutableSha256', 'stage3BaselineSha256', 'hostQualification',
        'phaseBaselineProfile', 'performanceData', 'stage3Baseline',
        'rawSampleManifest', 'topology',
        'measurementMode', 'installedRuntime', 'selectedLanes', 'oneWorkerPhases',
        'amdahl', 'kernelTimings', 'fixtures') `
        'Stage 5 scaling evidence'
    $recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' `
        'Stage 5 scaling evidence'
    $cohortNonce = Get-Stage5JsonValue $document 'cohortNonce' `
        'Stage 5 scaling evidence'
    $cohortCreatedUtc = Get-Stage5JsonValue $document 'cohortCreatedUtc' `
        'Stage 5 scaling evidence'
    $runtimeClosure = Get-Stage5JsonValue $document 'runtimeClosure' `
        'Stage 5 scaling evidence'
    $evidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Stage 5 scaling evidence'
    $evidenceStatus = Get-Stage5JsonValue $document 'status' `
        'Stage 5 scaling evidence'
    $qualificationMode = Get-Stage5JsonValue $document 'qualificationMode' `
        'Stage 5 scaling evidence'
    $referencePolicy = Get-Stage5JsonValue $document 'referencePolicy' `
        'Stage 5 scaling evidence'
    $sourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Stage 5 scaling evidence'
    $artifactSetSha256 = Get-Stage5JsonValue $document 'artifactSetSha256' `
        'Stage 5 scaling evidence'
    $evidenceTitle = Get-Stage5JsonValue $document 'title' `
        'Stage 5 scaling evidence'
    $executableSha256 = Get-Stage5JsonValue $document 'executableSha256' `
        'Stage 5 scaling evidence'
    $stage3SourceCommit = Get-Stage5JsonValue $document 'stage3SourceCommit' `
        'Stage 5 scaling evidence'
    $stage3ExecutableSha256 = Get-Stage5JsonValue $document 'stage3ExecutableSha256' `
        'Stage 5 scaling evidence'
    $stage3BaselineSha256 = Get-Stage5JsonValue $document 'stage3BaselineSha256' `
        'Stage 5 scaling evidence'
    $measurementMode = Get-Stage5JsonValue $document 'measurementMode' `
        'Stage 5 scaling evidence'
    $installedRuntime = Get-Stage5JsonValue $document 'installedRuntime' `
        'Stage 5 scaling evidence'
    Assert-Stage5Condition ($evidenceKind -is [string] -and
        $evidenceStatus -is [string] -and $qualificationMode -is [string] -and
        $referencePolicy -is [string] -and $sourceCommit -is [string] -and
        $stage3SourceCommit -is [string] -and
        $artifactSetSha256 -is [string] -and
        $executableSha256 -is [string] -and
        $stage3ExecutableSha256 -is [string] -and
        $stage3BaselineSha256 -is [string] -and
        $measurementMode -is [string] -and
        $evidenceTitle -is [string] -and
        $recordedUtc -is [string] -and $cohortNonce -is [string] -and
        $cohortCreatedUtc -is [string]) `
        'Stage 5 scaling evidence identity and provenance fields must be JSON strings.'
    Assert-Stage5Condition ($artifactSetSha256 -cmatch '^[0-9A-F]{64}$' -and
        $executableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $stage3ExecutableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $stage3BaselineSha256 -cmatch '^[0-9A-F]{64}$' -and
        $sourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $stage3SourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Stage 5 scaling evidence hashes and commits are not canonical strings.'
    Assert-Stage5JsonShape $runtimeClosure @('dependencyManifestSha256',
        'closureSha256') 'Stage 5 scaling runtime closure'
    [DateTimeOffset]$parsedRecordedUtc = [DateTimeOffset]::MinValue
    [DateTimeOffset]$parsedCohortCreatedUtc = [DateTimeOffset]::MinValue
    $recordedValid = $recordedUtc -is [string] -and
        [DateTimeOffset]::TryParseExact($recordedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$parsedRecordedUtc)
    $cohortValid = $cohortCreatedUtc -is [string] -and
        [DateTimeOffset]::TryParseExact($cohortCreatedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$parsedCohortCreatedUtc)
    $scalingSchemaVersion = Get-Stage5JsonValue $document 'schemaVersion' `
        'Stage 5 scaling evidence'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $scalingSchemaVersion) -and
        $scalingSchemaVersion -eq 2 -and
        $evidenceKind -ceq 'stage5-performance-scaling' -and
        $evidenceStatus -ceq 'passed' -and
        $recordedValid -and $cohortValid -and
        $recordedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        $cohortCreatedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        $parsedRecordedUtc -ge $parsedCohortCreatedUtc -and
        $cohortNonce -is [string] -and $cohortNonce -cmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
        $cohortNonce -ceq $ExpectedCohortNonce -and
        $cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $qualificationMode -ceq 'External16Core' -and
        $referencePolicy -ceq 'paired-serial-oracle-v1' -and
        $sourceCommit -ceq $ExpectedSourceCommit -and
        $artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $evidenceTitle -ceq $ExpectedTitle -and
        $executableSha256 -ceq $ExpectedExecutableSha256 -and
        $stage3SourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $stage3ExecutableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $stage3BaselineSha256 -ceq $ExpectedStage3BaselineSha256 -and
        $measurementMode -ceq 'headless-throughput' -and
        $installedRuntime -is [bool] -and $installedRuntime) `
        'Stage 5 scaling evidence provenance is invalid.'
    foreach ($field in @('dependencyManifestSha256', 'closureSha256')) {
        $runtimeClosureValue = Get-Stage5JsonValue $runtimeClosure $field `
            'Stage 5 scaling runtime closure'
        Assert-Stage5Condition ($runtimeClosureValue -is [string] -and
            $runtimeClosureValue -cmatch '^[0-9A-F]{64}$') `
            "Stage 5 scaling runtime closure field '$field' is invalid."
    }
    [void](Assert-Stage5RuntimeClosureBinding $runtimeClosure `
        $verifiedExpectedRuntimeClosure 'Stage 5 scaling evidence')

    $rawEntry = Get-Stage5JsonValue $document 'rawSampleManifest' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $rawEntry @('path', 'sha256') `
        'Stage 5 scaling raw-sample manifest reference'
    $rawEntryPath = Get-Stage5JsonValue $rawEntry 'path' `
        'Stage 5 scaling raw-sample manifest reference'
    $rawEntrySha256 = Get-Stage5JsonValue $rawEntry 'sha256' `
        'Stage 5 scaling raw-sample manifest reference'
    Assert-Stage5Condition ($rawEntryPath -is [string] -and
        $rawEntrySha256 -is [string] -and
        $rawEntrySha256 -cmatch '^[0-9A-F]{64}$') `
        'Stage 5 scaling raw-sample manifest reference path and hash must be JSON strings.'
    $rawPath = Resolve-Stage5FinalAcceptanceFile (Split-Path -Parent $full) `
        $rawEntryPath `
        'Stage 5 scaling raw-sample manifest'
    $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
        'Stage 5 scaling raw-sample manifest'
    $rawManifestSha256 = Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot `
        $rawEntrySha256 `
        'Stage 5 scaling raw-sample manifest'
    $raw = Read-Stage5PerformanceScalingRawSamples $rawPath $ExpectedSourceCommit `
        $ExpectedArtifactSetSha256 $ExpectedExecutableSha256 `
        $ExpectedStage3BaselineSha256 $ExpectedPhaseBaselineProfileSha256 `
        $ExpectedTitle $rawSnapshot $rawManifestSha256
    Assert-Stage5Condition ($raw.manifestSha256 -ceq $rawManifestSha256) `
        'Stage 5 scaling raw-sample manifest hash changed during validation.'
    foreach ($entryName in @('hostQualification', 'phaseBaselineProfile',
            'stage3Baseline')) {
        $entry = Get-Stage5JsonValue $document $entryName `
            "Stage 5 scaling $entryName reference"
        Assert-Stage5JsonShape $entry @('path', 'sha256') `
            "Stage 5 scaling $entryName reference"
        Assert-Stage5Condition (($entry | ConvertTo-Json -Compress -Depth 5) -ceq
                ($raw.$entryName | ConvertTo-Json -Compress -Depth 5)) `
            "Stage 5 scaling $entryName reference differs from the validated raw closure."
    }
    $phaseBaselineProfileSha256 = Get-Stage5JsonValue $document.phaseBaselineProfile `
        'sha256' 'Stage 5 scaling phase-baseline profile reference'
    Assert-Stage5Condition ($phaseBaselineProfileSha256 -is [string] -and
        $phaseBaselineProfileSha256 -cmatch '^[0-9A-F]{64}$' -and
        $phaseBaselineProfileSha256 -ceq
            $ExpectedPhaseBaselineProfileSha256) `
        'Stage 5 scaling final phase-baseline profile differs from the independently expected reviewed hash.'
    $performanceDataEntry = Get-Stage5JsonValue $document 'performanceData' `
        'Stage 5 scaling performance qualification-data reference'
    Assert-Stage5JsonShape $performanceDataEntry @('path', 'sha256',
        'closureSha256', 'fileCount') `
        'Stage 5 scaling performance qualification-data reference'
    Assert-Stage5Condition (($performanceDataEntry | ConvertTo-Json -Compress -Depth 5) -ceq
            ($raw.performanceData | ConvertTo-Json -Compress -Depth 5)) `
        'Stage 5 scaling performance qualification-data differs from the validated raw closure.'
    Assert-Stage5Condition ($recordedUtc -ceq $raw.recordedUtc -and
        $cohortNonce -ceq $raw.cohortNonce -and
        $cohortCreatedUtc -ceq $raw.cohortCreatedUtc -and
        (Get-Stage5JsonValue $document 'qualificationMode' `
            'Stage 5 scaling evidence') -ceq $raw.qualificationMode -and
        (Get-Stage5JsonValue $document 'referencePolicy' `
            'Stage 5 scaling evidence') -ceq $raw.referencePolicy -and
        (Get-Stage5JsonValue $document 'stage3SourceCommit' `
            'Stage 5 scaling evidence') -ceq $raw.stage3SourceCommit -and
        (Get-Stage5JsonValue $document 'stage3ExecutableSha256' `
            'Stage 5 scaling evidence') -ceq $raw.stage3ExecutableSha256 -and
        $runtimeClosure.dependencyManifestSha256 -ceq
            $raw.runtimeClosure.dependencyManifestSha256 -and
        $runtimeClosure.closureSha256 -ceq $raw.runtimeClosure.closureSha256) `
        'Stage 5 scaling final provenance differs from its validated raw closure.'

    $topology = Get-Stage5JsonValue $document 'topology' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $topology @('source', 'topologySha256', 'physicalCoreCount',
        'logicalProcessorCount') 'Stage 5 scaling topology'
    $physicalCores = Get-Stage5JsonValue $topology 'physicalCoreCount' 'Stage 5 scaling topology'
    $logicalProcessors = Get-Stage5JsonValue $topology 'logicalProcessorCount' 'Stage 5 scaling topology'
    $topologySource = Get-Stage5JsonValue $topology 'source' 'Stage 5 scaling topology'
    $topologySha256 = Get-Stage5JsonValue $topology 'topologySha256' 'Stage 5 scaling topology'
    Assert-Stage5Condition ($topologySource -is [string] -and
        $topologySha256 -is [string] -and
        $topologySha256 -cmatch '^[0-9A-F]{64}$' -and
        $topologySource -ceq 'GetSystemCpuSetInformation' -and
        $topologySha256 -ceq $raw.topologySha256 -and
        (Test-Stage5JsonInteger $physicalCores) -and
        [int]$physicalCores -eq $raw.topology.physicalCoreCount -and
        (Test-Stage5JsonInteger $logicalProcessors) -and
        [int]$logicalProcessors -eq $raw.topology.logicalProcessorCount) `
        'Stage 5 scaling evidence topology does not match its parsed CPU-set receipt.'

    $lanes = Get-Stage5JsonValue $document 'selectedLanes' 'Stage 5 scaling evidence'
    Assert-Stage5Condition ($lanes -is [Array] -and $lanes.Count -eq 3) `
        'Stage 5 scaling evidence requires exactly forced-one, physical-8, and physical-16 lanes.'
    $laneNames = @('forced-one', 'physical-8', 'physical-16')
    $laneWorkers = @(1, 8, 16)
    for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
        $lane = $lanes[$laneIndex]
        $context = "Stage 5 scaling lane $laneIndex"
        Assert-Stage5JsonShape $lane @('name', 'requestedWorkers', 'selectedLogicalProcessors',
            'selectedDistinctPhysicalCores', 'selectedPhysicalCoreMask') $context
        $logical = Get-Stage5JsonValue $lane 'selectedLogicalProcessors' $context
        $distinct = Get-Stage5JsonValue $lane 'selectedDistinctPhysicalCores' $context
        $mask = Get-Stage5JsonValue $lane 'selectedPhysicalCoreMask' $context
        $laneName = Get-Stage5JsonValue $lane 'name' $context
        $requestedWorkers = Get-Stage5JsonValue $lane 'requestedWorkers' $context
        Assert-Stage5Condition ($laneName -is [string] -and
            (Test-Stage5JsonInteger $requestedWorkers) -and
            $laneName -ceq $laneNames[$laneIndex] -and
            $requestedWorkers -eq $laneWorkers[$laneIndex] -and
            (Test-Stage5JsonInteger $logical) -and [int]$logical -eq $laneWorkers[$laneIndex] -and
            (Test-Stage5JsonInteger $distinct) -and [int]$distinct -eq $laneWorkers[$laneIndex] -and
            $mask -is [string] -and $mask -match '^[0-9A-F]{16}$') `
            "$context does not prove the exact selected logical and distinct physical-core count."
        $maskValue = [Convert]::ToUInt64($mask, 16)
        $maskBits = 0
        while ($maskValue -ne 0) { $maskBits += [int]($maskValue -band 1); $maskValue = $maskValue -shr 1 }
        Assert-Stage5Condition ($maskBits -eq $laneWorkers[$laneIndex]) `
            "$context physical-core mask does not contain the exact selected core count."
        $rawLane = $raw.topology.selectedLanes[$laneIndex]
        Assert-Stage5Condition ($rawLane.name -is [string] -and
            $rawLane.name -ceq $laneNames[$laneIndex] -and
            (Test-Stage5JsonInteger $rawLane.requestedWorkers) -and
            $rawLane.requestedWorkers -eq $laneWorkers[$laneIndex] -and
            $rawLane.selectedLogicalProcessors -eq [int]$logical -and
            $rawLane.selectedDistinctPhysicalCores -eq [int]$distinct -and
            $rawLane.selectedPhysicalCoreMask -ceq $mask) `
            "$context summary does not match the raw CPU-set lane receipt."
    }

    $phases = Get-Stage5JsonValue $document 'oneWorkerPhases' 'Stage 5 scaling evidence'
    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    Assert-Stage5Condition ($phases -is [Array] -and $phases.Count -eq $phaseNames.Count) `
        'Stage 5 scaling evidence requires exact one-worker timing for all simulation phases.'
    [double]$totalOne = $raw.totalOneWorkerMilliseconds
    [double]$totalSerial = $raw.totalSerialMilliseconds
    for ($phaseIndex = 0; $phaseIndex -lt $phaseNames.Count; ++$phaseIndex) {
        $phase = $phases[$phaseIndex]
        $context = "Stage 5 one-worker phase $phaseIndex"
        Assert-Stage5JsonShape $phase @('name', 'elapsedMilliseconds', 'serialMilliseconds',
            'serialMillisecondsKnown') $context
        $elapsed = Get-Stage5JsonValue $phase 'elapsedMilliseconds' $context
        $serial = Get-Stage5JsonValue $phase 'serialMilliseconds' $context
        $phaseName = Get-Stage5JsonValue $phase 'name' $context
        Assert-Stage5Condition ($phaseName -is [string] -and
            $phaseName -ceq $phaseNames[$phaseIndex] -and
            (Test-Stage5JsonNumber $elapsed) -and
            [double]$elapsed -gt 0.0 -and (Test-Stage5JsonNumber $serial) -and
            [double]$serial -ge 0.0 -and [double]$serial -le [double]$elapsed -and
            (Get-Stage5JsonValue $phase 'serialMillisecondsKnown' $context) -is [bool] -and
            (Get-Stage5JsonValue $phase 'serialMillisecondsKnown' $context) -and
            [Math]::Abs([double]$elapsed -
                [double]$raw.phases[$phaseIndex].elapsedMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$serial -
                [double]$raw.phases[$phaseIndex].serialMilliseconds) -le 0.0001) `
            "$context timing does not match the raw per-repeat median."
    }
    $amdahl = Get-Stage5JsonValue $document 'amdahl' 'Stage 5 scaling evidence'
    Assert-Stage5JsonShape $amdahl @('totalOneWorkerMilliseconds', 'totalSerialMilliseconds',
        'serialFraction', 'maximumSpeedup', 'reachesTwoX') 'Stage 5 Amdahl evidence'
    $reportedOne = Get-Stage5JsonValue $amdahl 'totalOneWorkerMilliseconds' 'Stage 5 Amdahl evidence'
    $reportedSerial = Get-Stage5JsonValue $amdahl 'totalSerialMilliseconds' 'Stage 5 Amdahl evidence'
    $fraction = Get-Stage5JsonValue $amdahl 'serialFraction' 'Stage 5 Amdahl evidence'
    $maximum = Get-Stage5JsonValue $amdahl 'maximumSpeedup' 'Stage 5 Amdahl evidence'
    $computedFraction = $totalSerial / $totalOne
    $computedMaximum = if ($computedFraction -eq 0.0) { [double]::PositiveInfinity } else { 1.0 / $computedFraction }
    Assert-Stage5Condition ((Test-Stage5JsonNumber $reportedOne) -and
        [Math]::Abs([double]$reportedOne - $totalOne) -le 0.0001 -and
        (Test-Stage5JsonNumber $reportedSerial) -and
        [Math]::Abs([double]$reportedSerial - $totalSerial) -le 0.0001 -and
        (Test-Stage5JsonNumber $fraction) -and
        [Math]::Abs([double]$fraction - $computedFraction) -le 0.000001 -and
        (Test-Stage5JsonNumber $maximum) -and
        [Math]::Abs([double]$maximum - $computedMaximum) -le 0.0001 -and
        (Get-Stage5JsonValue $amdahl 'reachesTwoX' 'Stage 5 Amdahl evidence') -is [bool] -and
        (Get-Stage5JsonValue $amdahl 'reachesTwoX' 'Stage 5 Amdahl evidence') -and
        $computedMaximum -ge 2.0) `
        'Stage 5 Amdahl evidence does not prove that the measured workload can reach 2x.'

    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelTimings = Get-Stage5JsonValue $document 'kernelTimings' 'Stage 5 scaling evidence'
    Assert-Stage5Condition ($kernelTimings -is [Array] -and $kernelTimings.Count -eq 6) `
        'Stage 5 scaling evidence requires timing for exactly six integrated kernels.'
    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
        $kernel = $kernelTimings[$kernelIndex]
        $context = "Stage 5 scaling kernel timing $kernelIndex"
        Assert-Stage5JsonShape $kernel @('name', 'admittedSlices', 'captureMilliseconds',
            'scheduleMilliseconds', 'waitMilliseconds', 'validateMilliseconds',
            'commitMilliseconds', 'totalParallelMilliseconds', 'exactSerialOperationMilliseconds',
            'exactSerialOperationMillisecondsKnown', 'timingAttribution', 'netSpeedup') $context
        $admitted = Get-Stage5JsonValue $kernel 'admittedSlices' $context
        $parts = @('captureMilliseconds', 'scheduleMilliseconds', 'waitMilliseconds',
            'validateMilliseconds', 'commitMilliseconds')
        foreach ($part in $parts) {
            $value = Get-Stage5JsonValue $kernel $part $context
            Assert-Stage5Condition ((Test-Stage5JsonNumber $value) -and [double]$value -ge 0.0) `
                "$context $part is invalid."
        }
        $reportedParallel = Get-Stage5JsonValue $kernel 'totalParallelMilliseconds' $context
        $serialOperation = Get-Stage5JsonValue $kernel 'exactSerialOperationMilliseconds' $context
        $netSpeedup = Get-Stage5JsonValue $kernel 'netSpeedup' $context
        $rawKernel = $raw.kernels[$kernelIndex]
        [double]$parallelTotal = $rawKernel.totalParallelMilliseconds
        $kernelName = Get-Stage5JsonValue $kernel 'name' $context
        $timingAttribution = Get-Stage5JsonValue $kernel 'timingAttribution' $context
        Assert-Stage5Condition ($kernelName -is [string] -and
            $timingAttribution -is [string] -and $kernelName -ceq $kernelNames[$kernelIndex] -and
            (Test-Stage5JsonInteger $admitted) -and
            (Get-Stage5JsonValue $kernel 'exactSerialOperationMillisecondsKnown' $context) -is [bool] -and
            (Get-Stage5JsonValue $kernel 'exactSerialOperationMillisecondsKnown' $context) -and
            $timingAttribution -ceq 'owner-stack-exclusive-v1' -and
            [int]$admitted -eq $rawKernel.admittedSlices -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'captureMilliseconds' $context) -
                [double]$rawKernel.captureMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'scheduleMilliseconds' $context) -
                [double]$rawKernel.scheduleMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'waitMilliseconds' $context) -
                [double]$rawKernel.waitMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'validateMilliseconds' $context) -
                [double]$rawKernel.validateMilliseconds) -le 0.0001 -and
            [Math]::Abs([double](Get-Stage5JsonValue $kernel 'commitMilliseconds' $context) -
                [double]$rawKernel.commitMilliseconds) -le 0.0001 -and
            (Test-Stage5JsonNumber $reportedParallel) -and
            [Math]::Abs([double]$reportedParallel - $parallelTotal) -le 0.0001 -and
            [Math]::Abs([double]$reportedParallel -
                [double]$rawKernel.totalParallelMilliseconds) -le 0.0001 -and
            $parallelTotal -gt 0.0 -and (Test-Stage5JsonNumber $serialOperation) -and
            [Math]::Abs([double]$serialOperation -
                [double]$rawKernel.exactSerialOperationMilliseconds) -le 0.0001 -and
            [double]$serialOperation -gt $parallelTotal -and (Test-Stage5JsonNumber $netSpeedup) -and
            [Math]::Abs([double]$netSpeedup - ([double]$serialOperation / $parallelTotal)) -le 0.0001 -and
            [Math]::Abs([double]$netSpeedup - [double]$rawKernel.netSpeedup) -le 0.0001 -and
            [double]$netSpeedup -gt 1.0) `
            "$context does not match raw installed runs or prove positive net speedup."
    }

    $fixtures = Get-Stage5JsonValue $document 'fixtures' 'Stage 5 scaling evidence'
    $fixtureNames = @('one-thousand-units', 'four-thousand-units', 'eight-thousand-units',
        'dense-eight-player')
    $minimumUnits = @(1000, 4000, 8000, 8000)
    Assert-Stage5Condition ($fixtures -is [Array] -and $fixtures.Count -eq 4) `
        'Stage 5 scaling evidence requires exact 1k, 4k, 8k, and dense eight-player fixtures.'
    [double]$maximumOneWorkerRegressionRatio = 0.0
    [double]$minimumEightWorkerSpeedup = [double]::PositiveInfinity
    [double]$minimumEightToSixteenSpeedup = [double]::PositiveInfinity
    foreach ($fixtureIndex in 0..3) {
        $fixture = $fixtures[$fixtureIndex]
        $context = "Stage 5 scaling fixture $fixtureIndex"
        Assert-Stage5JsonShape $fixture @('name', 'playerCount', 'peakUnitCount', 'repeats',
            'requestedMinimumUnitCount', 'minimumInitialUnitCount',
            'stage3OneWorkerMilliseconds', 'stage5OneWorkerMilliseconds',
            'eightPhysicalCoreMilliseconds', 'sixteenPhysicalCoreMilliseconds',
            'oneWorkerRegressionRatio', 'eightPhysicalCoreSpeedup', 'eightToSixteenSpeedup') $context
        $players = Get-Stage5JsonValue $fixture 'playerCount' $context
        $units = Get-Stage5JsonValue $fixture 'peakUnitCount' $context
        $repeats = Get-Stage5JsonValue $fixture 'repeats' $context
        $stage3 = Get-Stage5JsonValue $fixture 'stage3OneWorkerMilliseconds' $context
        $stage5 = Get-Stage5JsonValue $fixture 'stage5OneWorkerMilliseconds' $context
        $eight = Get-Stage5JsonValue $fixture 'eightPhysicalCoreMilliseconds' $context
        $sixteen = Get-Stage5JsonValue $fixture 'sixteenPhysicalCoreMilliseconds' $context
        foreach ($timing in @($stage3, $stage5, $eight, $sixteen)) {
            Assert-Stage5Condition ((Test-Stage5JsonNumber $timing) -and [double]$timing -gt 0.0) `
                "$context contains an invalid measured timing."
        }
        $regression = [double]$stage5 / [double]$stage3
        $speedup8 = [double]$stage5 / [double]$eight
        $scale16 = [double]$eight / [double]$sixteen
        $reportedRegression = Get-Stage5JsonValue $fixture 'oneWorkerRegressionRatio' $context
        $reportedSpeedup8 = Get-Stage5JsonValue $fixture 'eightPhysicalCoreSpeedup' $context
        $reportedScale16 = Get-Stage5JsonValue $fixture 'eightToSixteenSpeedup' $context
        $rawFixture = $raw.fixtures[$fixtureIndex]
        $fixtureName = Get-Stage5JsonValue $fixture 'name' $context
        Assert-Stage5Condition ($fixtureName -is [string] -and
            $fixtureName -ceq $fixtureNames[$fixtureIndex] -and
            (Test-Stage5JsonInteger $players) -and
            [int]$players -eq 8 -and (Test-Stage5JsonInteger $units) -and
            [int]$units -eq $rawFixture.peakUnitCount -and
            [int]$units -ge $minimumUnits[$fixtureIndex] -and
            (Get-Stage5JsonValue $fixture 'requestedMinimumUnitCount' $context) -eq
                $rawFixture.requestedMinimumUnitCount -and
            (Get-Stage5JsonValue $fixture 'minimumInitialUnitCount' $context) -eq
                $rawFixture.minimumInitialUnitCount -and
            (Test-Stage5JsonInteger $repeats) -and [int]$repeats -eq $raw.repeatCount -and
            [Math]::Abs([double]$stage3 - [double]$rawFixture.stage3OneWorkerMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$stage5 - [double]$rawFixture.stage5OneWorkerMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$eight - [double]$rawFixture.eightPhysicalCoreMilliseconds) -le 0.0001 -and
            [Math]::Abs([double]$sixteen - [double]$rawFixture.sixteenPhysicalCoreMilliseconds) -le 0.0001 -and
            (Test-Stage5JsonNumber $reportedRegression) -and
            [Math]::Abs([double]$reportedRegression - $regression) -le 0.0001 -and
            [Math]::Abs([double]$reportedRegression - [double]$rawFixture.oneWorkerRegressionRatio) -le 0.0001 -and
            $regression -le 1.05 -and (Test-Stage5JsonNumber $reportedSpeedup8) -and
            [Math]::Abs([double]$reportedSpeedup8 - $speedup8) -le 0.0001 -and
            [Math]::Abs([double]$reportedSpeedup8 - [double]$rawFixture.eightPhysicalCoreSpeedup) -le 0.0001 -and
            $speedup8 -ge 2.0 -and (Test-Stage5JsonNumber $reportedScale16) -and
            [Math]::Abs([double]$reportedScale16 - $scale16) -le 0.0001 -and
            [Math]::Abs([double]$reportedScale16 - [double]$rawFixture.eightToSixteenSpeedup) -le 0.0001 -and
            $scale16 -gt 1.0) `
            "$context does not match raw per-repeat medians or meet the scaling gates."
        $maximumOneWorkerRegressionRatio = [Math]::Max(
            $maximumOneWorkerRegressionRatio, $regression)
        $minimumEightWorkerSpeedup = [Math]::Min(
            $minimumEightWorkerSpeedup, $speedup8)
        $minimumEightToSixteenSpeedup = [Math]::Min(
            $minimumEightToSixteenSpeedup, $scale16)
    }
    return [pscustomobject]@{
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        evidenceManifestSha256 = [string]$Snapshot.sha256
        rawSampleManifestSha256 = $rawManifestSha256
        executableSha256 = $ExpectedExecutableSha256
        recordedUtc = [string]$recordedUtc
        cohortNonce = [string]$cohortNonce
        cohortCreatedUtc = [string]$cohortCreatedUtc
        qualificationMode = 'External16Core'
        referencePolicy = 'paired-serial-oracle-v1'
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = [string]$runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$runtimeClosure.closureSha256
        }
        stage3SourceCommit = [string]$raw.stage3SourceCommit
        stage3ExecutableSha256 = [string]$raw.stage3ExecutableSha256
        hostQualificationSha256 = [string]$raw.hostQualification.sha256
        phaseBaselineProfileSha256 = [string]$raw.phaseBaselineProfile.sha256
        performanceDataManifestSha256 = [string]$raw.performanceData.sha256
        performanceDataClosureSha256 = [string]$raw.performanceData.closureSha256
        performanceDataFileCount = [int]$raw.performanceData.fileCount
        stage3BaselineSha256 = [string]$raw.stage3Baseline.sha256
        physicalCoreCount = [int]$physicalCores
        fixtureCount = 4
        kernelCount = 6
        eightPhysicalCoreSpeedupFloor = 2.0
        maximumOneWorkerRegressionRatio = $maximumOneWorkerRegressionRatio
        minimumEightWorkerSpeedup = $minimumEightWorkerSpeedup
        minimumEightToSixteenSpeedup = $minimumEightToSixteenSpeedup
    }
}

function ConvertTo-Stage5LockstepReceiptUInt64 {
    param([object]$Value, [string]$Field)
    Assert-Stage5Condition ($Value -is [string] -and $Value -match '^[0-9]+$') `
        "Lockstep-v2 receipt field '$Field' is not an unsigned decimal integer."
    [UInt64]$parsed = 0
    try {
        $parsed = [UInt64]::Parse($Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "Lockstep-v2 receipt field '$Field' is outside the UInt64 range."
    }
    return $parsed
}

function ConvertTo-Stage5LockstepReceiptUInt32 {
    param([object]$Value, [string]$Field)
    $parsed = ConvertTo-Stage5LockstepReceiptUInt64 $Value $Field
    Assert-Stage5Condition ($parsed -le [UInt64]4294967295) `
        "Lockstep-v2 receipt field '$Field' is outside the UInt32 range."
    return [UInt32]$parsed
}

function ConvertTo-Stage5LockstepReceiptBoolean {
    param([object]$Value, [string]$Field)
    Assert-Stage5Condition ($Value -is [string] -and ($Value -ceq '0' -or $Value -ceq '1')) `
        "Lockstep-v2 receipt field '$Field' is not a canonical boolean."
    return $Value -ceq '1'
}

function Get-Stage5LockstepReceiptDigest {
    param([Collections.IDictionary]$Pairs)
    Add-Type -AssemblyName System.Numerics
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    function Update-Stage5LockstepFnv {
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
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $hash = Update-Stage5LockstepFnv $hash ([UInt64]$slot) 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_command_count"] `
                "peer_${slot}_command_count") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_first_command_frame"] `
                "peer_${slot}_first_command_frame") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs["peer_${slot}_last_command_frame"] `
                "peer_${slot}_last_command_frame") 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            ([UInt64](ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_last_command_id"] `
                "peer_${slot}_last_command_id")) 2 $prime $mask
        $hasLast = ConvertTo-Stage5LockstepReceiptBoolean `
            $Pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $hash = Update-Stage5LockstepFnv $hash `
            ([UInt64]$(if ($hasLast) { 1 } else { 0 })) 4 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_last_command_digest"] `
                "peer_${slot}_last_command_digest") 8 $prime $mask
        $hash = Update-Stage5LockstepFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["peer_${slot}_command_digest"] `
                "peer_${slot}_command_digest") 8 $prime $mask
    }
    return $hash
}

function Get-Stage5LockstepReceiptAIPlanningDigest {
    param([Collections.IDictionary]$Pairs)
    Add-Type -AssemblyName System.Numerics
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    function Update-Stage5LockstepAIPlanningFnv {
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
    $hash = Update-Stage5LockstepAIPlanningFnv $hash `
        (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs['simulation_roster_mask'] `
            'simulation_roster_mask') 4 $prime $mask
    $hash = Update-Stage5LockstepAIPlanningFnv $hash `
        (ConvertTo-Stage5LockstepReceiptUInt32 $Pairs['ai_roster_mask'] `
            'ai_roster_mask') 4 $prime $mask
    foreach ($field in @(
        'captured_snapshots', 'captured_candidates', 'requested_batches',
        'submitted_jobs', 'completed_jobs', 'serial_fallbacks',
        'shadow_matches', 'shadow_mismatches', 'validation_failures',
        'canonical_validation_invocations', 'committed_batches',
        'parallel_authoritative_commits', 'rejected_commits',
        'owner_helped_executions')) {
        $hash = Update-Stage5LockstepAIPlanningFnv $hash `
            (ConvertTo-Stage5LockstepReceiptUInt64 $Pairs["ai_planning_$field"] `
                "ai_planning_$field") 8 $prime $mask
    }
    return $hash
}

function Get-Stage5LockstepReceiptProjectionSha256 {
    param([Collections.IDictionary]$Pairs)
    $projection = [ordered]@{}
    foreach ($key in @('mode', 'schema', 'protocol_epoch', 'peer_count', 'roster_mask',
        'simulation_roster_mask', 'ai_roster_mask', 'build_compatibility_crc',
        'content_crc', 'map_crc', 'common_stop_frame',
        'proven_kernel_mask', 'packet_router_slot', 'origin_mode', 'session_nonce',
        'executable_sha256', 'source_revision', 'final_frame', 'frame_count',
        'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
        'executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown',
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
        $projection[$key] = $Pairs[$key]
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $key = "peer_${slot}_${suffix}"
            $projection[$key] = $Pairs[$key]
        }
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $key = "checkpoint_${checkpoint}_${suffix}"
            $projection[$key] = $Pairs[$key]
        }
    }
    $text = $projection | ConvertTo-Json -Compress -Depth 5
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Stage5LockstepReceiptPairs {
    param(
        [string]$Path,
        [object]$Snapshot = $null
    )
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Lockstep-v2 receipt'
    }
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'path' -and
        [IO.Path]::GetFullPath([string]$Snapshot.path) -ceq $full) `
        "Lockstep-v2 receipt snapshot is bound to a different path: $full"
    $text = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    Assert-Stage5Condition ($text.IndexOf("`r", [StringComparison]::Ordinal) -lt 0) `
        "Lockstep-v2 receipt contains non-canonical CR line endings: $full"
    $lines = $text.Split(@("`n"), [StringSplitOptions]::None)
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') {
        $lines = $lines[0..($lines.Count - 2)]
    }
    Assert-Stage5Condition ($lines.Count -ge 4 -and
        $lines[0] -ceq 'RTS_LOCKSTEP_V2_RECEIPT' -and
        $lines[$lines.Count - 1] -ceq 'END') `
        "Lockstep-v2 receipt is not a canonical v2 document: $full"
    $pairs = [ordered]@{}
    for ($index = 1; $index -lt $lines.Count - 1; ++$index) {
        $line = $lines[$index]
        Assert-Stage5Condition ($line.Length -gt 0) `
            "Lockstep-v2 receipt contains an empty line: $full"
        $equals = $line.IndexOf('=', [StringComparison]::Ordinal)
        Assert-Stage5Condition ($equals -gt 0 -and $equals -lt ($line.Length - 1) -and
            $line.Substring(0, $equals) -match '^[A-Za-z_][A-Za-z0-9_]*$') `
            "Lockstep-v2 receipt contains a malformed key/value line: $full"
        $key = $line.Substring(0, $equals)
        Assert-Stage5Condition (-not $pairs.Contains($key)) `
            "Lockstep-v2 receipt repeats field '$key': $full"
        $pairs[$key] = $line.Substring($equals + 1)
    }
    Assert-Stage5Condition ($pairs.Contains('checkpoint_count')) `
        "Lockstep-v2 receipt has no checkpoint_count: $full"
    $checkpointCount = ConvertTo-Stage5LockstepReceiptUInt32 `
        $pairs['checkpoint_count'] 'checkpoint_count'
    Assert-Stage5Condition ($checkpointCount -eq 129) `
        "Lockstep-v2 receipt must contain exactly 129 checkpoints: $full"
    $expectedKeys = New-Object 'Collections.Generic.List[string]'
    foreach ($key in @('producer', 'mode', 'schema', 'protocol_epoch', 'local_slot',
        'peer_count', 'roster_mask', 'simulation_roster_mask', 'ai_roster_mask',
        'build_compatibility_crc', 'content_crc',
        'map_crc', 'common_stop_frame', 'proven_kernel_mask', 'packet_router_slot',
        'origin_mode', 'run_nonce', 'session_nonce', 'executable_sha256',
         'source_revision', 'network_session_token', 'final_frame', 'frame_count',
         'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
         'executable_origin', 'worker_telemetry_executable_origin',
         'transport_path_used', 'handshake_validated', 'clean_shutdown',
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
        $expectedKeys.Add($key) | Out-Null
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $expectedKeys.Add("peer_${slot}_${suffix}") | Out-Null
        }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        foreach ($suffix in @('physical_worker_mask', 'physical_worker_jobs',
            'distinct_physical_workers', 'peak_concurrent_physical_workers',
            'physical_worker_mask_complete')) {
            $expectedKeys.Add("kernel_${kernel}_${suffix}") | Out-Null
        }
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $expectedKeys.Add("checkpoint_${checkpoint}_${suffix}") | Out-Null
        }
    }
    Assert-Stage5Condition ($pairs.Count -eq $expectedKeys.Count) `
        "Lockstep-v2 receipt has an unexpected field count: $full"
    $actualKeys = @($pairs.Keys)
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        Assert-Stage5Condition ($actualKeys[$index] -ceq $expectedKeys[$index]) `
            "Lockstep-v2 receipt field order/shape mismatch at ${index}: $full"
    }
    return [pscustomobject]@{
        path = $full; pairs = $pairs; text = $text; snapshot = $Snapshot
    }
}

function Read-Stage5LockstepV2Receipt {
    param(
        [string]$Path,
        [int]$ExpectedLocalSlot,
        [int]$ExpectedPeerCount,
        [UInt32]$ExpectedMapCrc,
        [string]$ExpectedRunNonce,
        [string]$ExpectedSessionNonce,
        [string]$ExpectedExecutableSha256,
        [string]$ExpectedSourceCommit,
        [object]$Snapshot = $null
    )
    $parsed = Get-Stage5LockstepReceiptPairs $Path $Snapshot
    $pairs = $parsed.pairs
    $context = "Lockstep-v2 receipt '$Path'"
    Assert-Stage5Condition ($pairs['producer'] -ceq 'installed-lockstep-v2' -and
        $pairs['mode'] -ceq 'installed-lockstep-v2-production') `
        "$context has a noncanonical producer/mode boundary."
    foreach ($check in @(
        @('schema', 2), @('protocol_epoch', 2),
        @('local_slot', $ExpectedLocalSlot), @('peer_count', $ExpectedPeerCount),
        @('roster_mask', ((1 -shl $ExpectedPeerCount) - 1)),
        @('simulation_roster_mask', 63), @('ai_roster_mask', 60),
        @('map_crc', $ExpectedMapCrc), @('common_stop_frame', 4096),
        @('proven_kernel_mask', 63), @('packet_router_slot', 0),
        @('origin_mode', 2), @('final_frame', 4096),
        @('frame_count', 4096),
        @('contributed_peer_mask', ((1 -shl $ExpectedPeerCount) - 1)),
        @('checkpoint_count', 129), @('validation_authority_mask', 63)
    )) {
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs[$check[0]] $check[0]) -eq [UInt64]$check[1]) `
            "$context field '$($check[0])' does not match the v2 qualification contract."
    }
    foreach ($crcField in @('build_compatibility_crc', 'content_crc')) {
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs[$crcField] $crcField) -gt 0) `
            "$context field '$crcField' is not a positive executable-originated CRC."
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
    $aiValues = @{}
    foreach ($field in $aiPlanningFields) {
        $aiValues[$field] = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["ai_planning_$field"] "ai_planning_$field"
    }
    Assert-Stage5Condition ($aiValues['captured_snapshots'] -ge 4 -and
        $aiValues['captured_candidates'] -ge $aiValues['captured_snapshots'] -and
        $aiValues['requested_batches'] -gt 0 -and
        $aiValues['submitted_jobs'] -gt 0 -and
        $aiValues['completed_jobs'] -eq $aiValues['submitted_jobs'] -and
        $aiValues['serial_fallbacks'] -eq 0 -and
        $aiValues['shadow_matches'] -gt 0 -and
        $aiValues['shadow_mismatches'] -eq 0 -and
        $aiValues['validation_failures'] -eq 0 -and
        $aiValues['canonical_validation_invocations'] -gt 0 -and
        $aiValues['committed_batches'] -gt 0 -and
        $aiValues['parallel_authoritative_commits'] -gt 0 -and
        $aiValues['parallel_authoritative_commits'] -le $aiValues['committed_batches'] -and
        $aiValues['rejected_commits'] -eq 0 -and
        $aiValues['physical_worker_executions'] -ge $aiValues['submitted_jobs'] -and
        $aiValues['owner_helped_executions'] -eq 0 -and
        $aiValues['observed_physical_worker_mask'] -ne 0 -and
        (Get-Stage5UInt64BitCount $aiValues['observed_physical_worker_mask']) -ge 2 -and
        $aiValues['maximum_distinct_physical_workers'] -ge 2 -and
        $aiValues['maximum_concurrent_physical_workers'] -ge 2 -and
        $aiValues['maximum_concurrent_physical_workers'] -le
            $aiValues['maximum_distinct_physical_workers']) `
        "$context AI planning telemetry is incomplete, nonparallel, or reports a failure."
    $aiDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
        $pairs['ai_planning_digest'] 'ai_planning_digest'
    Assert-Stage5Condition ($aiDigest -gt 0 -and
        [Numerics.BigInteger]$aiDigest -eq
            [Numerics.BigInteger](Get-Stage5LockstepReceiptAIPlanningDigest $pairs)) `
        "$context AI planning digest is not canonical."
    Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
        $pairs['network_session_token'] 'network_session_token') -gt 0) `
        "$context has no network session token."
    Assert-Stage5Condition ($pairs['run_nonce'] -ceq $ExpectedRunNonce -and
        $pairs['run_nonce'] -cmatch '^[0-9A-F]{32}$' -and
        $pairs['session_nonce'] -ceq $ExpectedSessionNonce -and
        $pairs['session_nonce'] -cmatch '^[0-9A-F]{32}$' -and
        $pairs['executable_sha256'] -cmatch '^[0-9A-F]{64}$' -and
        $pairs['executable_sha256'] -ceq $ExpectedExecutableSha256.ToUpperInvariant() -and
        $pairs['source_revision'] -cmatch '^[0-9a-f]{40}$' -and
        $pairs['source_revision'] -ceq $ExpectedSourceCommit) `
        "$context executable/source/run identity does not match the installed x64 binding."
    foreach ($field in @('executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown')) {
        Assert-Stage5Condition (ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs[$field] $field) "$context field '$field' is not true."
    }
    $expectedFrames = New-Object 'Collections.Generic.List[UInt32]'
    $expectedFrames.Add(1) | Out-Null
    for ($frame = 32; $frame -le 4096; $frame += 32) {
        $expectedFrames.Add([UInt32]$frame) | Out-Null
    }
    for ($index = 0; $index -lt $expectedFrames.Count; ++$index) {
        $frame = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["checkpoint_${index}_frame"] "checkpoint_${index}_frame"
        Assert-Stage5Condition ($frame -eq $expectedFrames[$index]) `
            "$context checkpoint $index is not on the canonical 4096-frame boundary."
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["checkpoint_${index}_crc"] "checkpoint_${index}_crc") -gt 0) `
            "$context checkpoint $index has no executable-originated CRC."
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["checkpoint_${index}_command_digest"] `
                "checkpoint_${index}_command_digest") -gt 0) `
            "$context checkpoint $index has no command digest."
    }
    $authorityDigest = Get-Stage5LockstepReceiptDigest $pairs
    $lastDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
        $pairs['checkpoint_128_command_digest'] 'checkpoint_128_command_digest'
    Assert-Stage5Condition ($lastDigest -gt 0 -and
        $authorityDigest -eq [Numerics.BigInteger]$lastDigest) `
        "$context command digest is not the canonical peer contribution digest."
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $count = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_command_count"] "peer_${slot}_command_count"
        $first = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_first_command_frame"] "peer_${slot}_first_command_frame"
        $last = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["peer_${slot}_last_command_frame"] "peer_${slot}_last_command_frame"
        $id = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_last_command_id"] "peer_${slot}_last_command_id"
        $has = ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs["peer_${slot}_has_last_command_id"] "peer_${slot}_has_last_command_id"
        $lastCommandDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_last_command_digest"] "peer_${slot}_last_command_digest"
        $commandDigest = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["peer_${slot}_command_digest"] "peer_${slot}_command_digest"
        if ($slot -lt $ExpectedPeerCount) {
            Assert-Stage5Condition ($count -ge 1 -and $first -ge 1 -and
                $first -le 4096 -and $last -ge $first -and $last -le 4096 -and
                $has -and $id -gt 0 -and
                $lastCommandDigest -gt 0 -and $commandDigest -gt 0) `
                "$context peer $slot has no valid gameplay command contribution in frames 1..4096."
        }
        else {
            Assert-Stage5Condition ($count -eq 0 -and $first -eq 0 -and $last -eq 0 -and
                $id -eq 0 -and -not $has -and $lastCommandDigest -eq 0 -and
                $commandDigest -eq 0) `
                "$context non-roster peer $slot has a command contribution."
        }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $mask = ConvertTo-Stage5LockstepReceiptUInt64 `
            $pairs["kernel_${kernel}_physical_worker_mask"] `
            "kernel_${kernel}_physical_worker_mask"
        $jobs = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_physical_worker_jobs"] `
            "kernel_${kernel}_physical_worker_jobs"
        $distinct = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_distinct_physical_workers"] `
            "kernel_${kernel}_distinct_physical_workers"
        $peak = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs["kernel_${kernel}_peak_concurrent_physical_workers"] `
            "kernel_${kernel}_peak_concurrent_physical_workers"
        Assert-Stage5Condition ((ConvertTo-Stage5LockstepReceiptBoolean `
            $pairs["kernel_${kernel}_physical_worker_mask_complete"] `
                "kernel_${kernel}_physical_worker_mask_complete") -and
            $mask -ne 0 -and $jobs -gt 0 -and $distinct -ge 2 -and $peak -ge 2 -and
            $distinct -eq (Get-Stage5UInt64BitCount $mask)) `
            "$context kernel $kernel lacks complete executable-origin worker telemetry."
    }
    return [pscustomobject]@{
        parsed = $parsed
        projectionSha256 = Get-Stage5LockstepReceiptProjectionSha256 $pairs
        finalFrame = [UInt32]4096
        finalCRC = ConvertTo-Stage5LockstepReceiptUInt32 `
            $pairs['checkpoint_128_crc'] 'checkpoint_128_crc'
    }
}

function Test-Stage5LockstepSafeMapName {
    param([object]$Value)
    if ($Value -isnot [string] -or $Value.Length -lt 4 -or
        $Value.Length -ge 248 -or
        -not $Value.EndsWith('.map', [StringComparison]::OrdinalIgnoreCase) -or
        $Value -match '(^[\\/]|:|\.\.|[;"])') {
        return $false
    }
    foreach ($character in $Value.ToCharArray()) {
        if ([char]::IsControl($character) -or
            ([char]::IsWhiteSpace($character) -and $character -ne [char]0x20)) {
            return $false
        }
    }
    return $true
}

function Assert-Stage5LockstepMapCrcs {
    param([object]$Value, [string]$Context)

    Assert-Stage5JsonShape $Value @('Generals', 'ZeroHour') $Context
    $result = [ordered]@{}
    foreach ($title in @('Generals', 'ZeroHour')) {
        Assert-Stage5Condition ((Test-Stage5JsonInteger $Value[$title]) -and
            [UInt64]$Value[$title] -gt 0 -and
            [UInt64]$Value[$title] -le [UInt64][UInt32]::MaxValue) `
            "$Context '$title' must be a nonzero UInt32 JSON integer."
        $result[$title] = [UInt32]$Value[$title]
    }
    return $result
}

function Assert-Stage5LockstepLauncherContract {
    param(
        [object]$Contract,
        [string]$Title,
        [Collections.IDictionary]$ArtifactHashes,
        [string]$Context,
        [Collections.IDictionary]$ArtifactPaths = $null,
        [string]$ArtifactRootDirectory = $null
    )
    Assert-Stage5JsonShape $Contract @('schemaVersion', 'mode', 'configPath',
        'configSha256', 'launcherPath', 'launcherSha256', 'directory',
        'executable', 'launcherTarget', 'launcherArguments',
        'launcherWorkingDirectory', 'directExecutable',
        'directWorkingDirectory', 'directArguments', 'childExitCodeObserved') $Context
    $executableRole = if ($Title -ceq 'Generals') {
        'generals-executable'
    }
    else { 'zerohour-executable' }
    $launcherRole = if ($Title -ceq 'Generals') {
        'generals-launcher'
    }
    else { 'zerohour-launcher' }
    $configRole = if ($Title -ceq 'Generals') {
        'generals-launcher-config'
    }
    else { 'zerohour-launcher-config' }
    $expectedExecutableSha256 = $ArtifactHashes[$executableRole]
    $expectedLauncherSha256 = $ArtifactHashes[$launcherRole]
    $expectedConfigSha256 = $ArtifactHashes[$configRole]
    Assert-Stage5Condition ($expectedExecutableSha256 -is [string] -and
        $expectedLauncherSha256 -is [string] -and
        $expectedConfigSha256 -is [string] -and
        $expectedExecutableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $expectedLauncherSha256 -cmatch '^[0-9A-F]{64}$' -and
        $expectedConfigSha256 -cmatch '^[0-9A-F]{64}$') `
        "$Context artifact-set executable, launcher, and config hashes must be JSON strings."
    foreach ($hashBinding in @(
        @('launcherSha256', $expectedLauncherSha256),
        @('configSha256', $expectedConfigSha256)
    )) {
        Assert-Stage5Condition ($Contract[$hashBinding[0]] -is [string] -and
            $Contract[$hashBinding[0]] -cmatch '^[0-9A-F]{64}$' -and
            $Contract[$hashBinding[0]] -ceq $hashBinding[1].ToUpperInvariant()) `
            "$Context $($hashBinding[0]) does not bind the artifact-set hash."
    }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Contract['schemaVersion']) -and
        $Contract['schemaVersion'] -eq 1 -and
        $Contract['mode'] -is [string] -and $Contract['mode'] -ceq 'headless-direct-exception' -and
        $Contract['directory'] -is [string] -and $Contract['directory'] -ceq '.' -and
        $Contract['childExitCodeObserved'] -is [bool] -and
        [bool]$Contract['childExitCodeObserved']) `
        "$Context is not the reviewed bounded launcher-equivalence contract."
    foreach ($pathField in @('configPath', 'launcherPath', 'launcherTarget',
        'launcherWorkingDirectory', 'directExecutable', 'directWorkingDirectory')) {
        Assert-Stage5Condition ($Contract[$pathField] -is [string] -and
            [IO.Path]::IsPathRooted([string]$Contract[$pathField]) -and
            [string]$Contract[$pathField] -notmatch '[\";]') `
            "$Context $pathField is not an absolute executable-origin path."
    }
    $contractExecutable = $Contract['executable']
    Assert-Stage5Condition ($contractExecutable -is [string] -and
        $contractExecutable -match '^[A-Za-z0-9._-]+\.exe$') `
        "$Context executable identity must be a canonical JSON string."
    $configPath = [IO.Path]::GetFullPath($Contract['configPath'])
    $launcherPath = [IO.Path]::GetFullPath($Contract['launcherPath'])
    $targetPath = [IO.Path]::GetFullPath($Contract['launcherTarget'])
    $directPath = [IO.Path]::GetFullPath($Contract['directExecutable'])
    $launcherWorkingDirectory = [IO.Path]::GetFullPath($Contract['launcherWorkingDirectory'])
    $directWorkingDirectory = [IO.Path]::GetFullPath($Contract['directWorkingDirectory'])
    Assert-Stage5Condition ([IO.Path]::GetFileName($configPath) -ceq 'launcher.lcf' -and
        [IO.Path]::GetFileName($launcherPath) -ceq 'launcher.exe' -and
        [IO.Path]::GetFileName($directPath) -ceq $contractExecutable -and
        [IO.Path]::GetFileName($targetPath) -ceq $contractExecutable -and
        $targetPath -ceq $directPath -and
        [IO.Path]::GetFullPath((Split-Path -Parent $configPath)) -ceq $directWorkingDirectory -and
        [IO.Path]::GetFullPath((Split-Path -Parent $launcherPath)) -ceq $directWorkingDirectory -and
        $launcherWorkingDirectory -ceq $directWorkingDirectory) `
        "$Context does not bind launcher, configuration, and executable to one runtime directory."
    $launcherArguments = $Contract['launcherArguments']
    $directArguments = $Contract['directArguments']
    Assert-Stage5Condition ($launcherArguments -is [Array] -and
        $directArguments -is [Array] -and
        $launcherArguments.Count -eq 4 -and
        $directArguments.Count -eq 4 -and
        @($launcherArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        @($directArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        $launcherArguments[0] -ceq '-simulationMode' -and
        $launcherArguments[1] -ceq 'parallel' -and
        $launcherArguments[2] -ceq '-workerPolicy' -and
        $launcherArguments[3] -ceq 'auto' -and
        ($directArguments -join '|') -ceq ($launcherArguments -join '|')) `
        "$Context does not retain the exact reviewed launcher defaults."
    Assert-Stage5Condition ($Contract['launcherTarget'] -ceq $Contract['directExecutable'] -and
        $Contract['launcherWorkingDirectory'] -ceq $Contract['directWorkingDirectory']) `
        "$Context launcher target is not the direct executable identity."
    $canonicalPathRoles = @($executableRole, $launcherRole, $configRole)
    $hasCanonicalArtifactPaths = $ArtifactPaths -is [Collections.IDictionary]
    foreach ($canonicalRole in $canonicalPathRoles) {
        if ($hasCanonicalArtifactPaths -and
            -not $ArtifactPaths.Contains($canonicalRole)) {
            $hasCanonicalArtifactPaths = $false
            break
        }
    }
    if ($hasCanonicalArtifactPaths) {
        $expectedExecutablePathText = $ArtifactPaths[$executableRole]
        $expectedLauncherPathText = $ArtifactPaths[$launcherRole]
        $expectedConfigPathText = $ArtifactPaths[$configRole]
        Assert-Stage5Condition ($expectedExecutablePathText -is [string] -and
            $expectedLauncherPathText -is [string] -and
            $expectedConfigPathText -is [string]) `
            "$Context canonical artifact paths must be JSON strings."
        $expectedExecutablePath = [IO.Path]::GetFullPath(
            $expectedExecutablePathText)
        $expectedLauncherPath = [IO.Path]::GetFullPath(
            $expectedLauncherPathText)
        $expectedConfigPath = [IO.Path]::GetFullPath(
            $expectedConfigPathText)
        Assert-Stage5Condition ($expectedExecutablePath -ne '' -and
            $expectedLauncherPath -ne '' -and $expectedConfigPath -ne '') `
            "$Context canonical artifact paths are empty."
        if (-not [string]::IsNullOrWhiteSpace($ArtifactRootDirectory)) {
            Assert-Stage5FinalAcceptanceNoReparsePath $ArtifactRootDirectory `
                $expectedExecutablePath "$Context canonical executable"
            Assert-Stage5FinalAcceptanceNoReparsePath $ArtifactRootDirectory `
                $expectedLauncherPath "$Context canonical launcher"
            Assert-Stage5FinalAcceptanceNoReparsePath $ArtifactRootDirectory `
                $expectedConfigPath "$Context canonical launcher configuration"
        }
        else {
            foreach ($canonicalPath in @($expectedExecutablePath,
                $expectedLauncherPath, $expectedConfigPath)) {
                $canonicalItem = Get-Item -LiteralPath $canonicalPath -Force -ErrorAction Stop
                Assert-Stage5Condition (($canonicalItem.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -eq 0) `
                    "$Context canonical artifact is a reparse point: $canonicalPath"
            }
        }
        Assert-Stage5Condition ($Contract['directExecutable'] -ceq
            $expectedExecutablePath -and
            $Contract['launcherTarget'] -ceq $expectedExecutablePath -and
            $Contract['launcherPath'] -ceq $expectedLauncherPath -and
            $Contract['configPath'] -ceq $expectedConfigPath -and
            $Contract['launcherWorkingDirectory'] -ceq
                ([IO.Path]::GetDirectoryName($expectedExecutablePath)) -and
            $Contract['directWorkingDirectory'] -ceq
                ([IO.Path]::GetDirectoryName($expectedExecutablePath))) `
            "$Context launch paths are not bound to the canonical artifact-set files."
    }
    return $Contract
}

function Assert-Stage5LockstepTitleSessionContract {
    param(
        [object]$Contract,
        [string]$Title,
        [string]$SessionDirectory,
        [object]$LauncherContract,
        [string]$Context
    )
    Assert-Stage5JsonShape $Contract @('schemaVersion', 'title',
        'sessionRoot', 'runtimeDirectory', 'documentsRoot', 'profileLeaf',
        'profileRoot', 'peerRoot', 'profileConcurrency', 'environmentValues',
        'environmentVariableNames', 'registryViews', 'registryValues') $Context
    Assert-Stage5Condition ((Test-Stage5JsonInteger $Contract['schemaVersion']) -and
        $Contract['schemaVersion'] -eq 1 -and
        $Contract['title'] -is [string] -and $Contract['title'] -ceq $Title -and
        $Contract['profileConcurrency'] -is [string] -and
        $Contract['profileConcurrency'] -ceq 'shared-title-profile-read-only') `
        "$Context has an invalid title-session profile identity."
    $outputTitleFull = [IO.Path]::GetFullPath($SessionDirectory)
    $sessionFull = Join-Path $outputTitleFull 'TitleSession'
    $launcherWorkingDirectory = $LauncherContract['directWorkingDirectory']
    Assert-Stage5Condition ($launcherWorkingDirectory -is [string]) `
        "$Context launcher working directory must be a JSON string."
    $runtimeFull = [IO.Path]::GetFullPath($launcherWorkingDirectory)
    $documentsRoot = Join-Path $sessionFull 'Documents'
    $profileLeaf = if ($Title -ceq 'Generals') {
        'Command and Conquer Generals Data'
    }
    else { 'GGC-LockstepV2-ZeroHour' }
    $profileRoot = Join-Path $documentsRoot $profileLeaf
    $peerRoot = Join-Path $sessionFull 'Peers'
    Assert-Stage5Condition ($Contract['sessionRoot'] -is [string] -and
        $Contract['sessionRoot'] -ceq $sessionFull) `
        "$Context sessionRoot is stale or substituted (actual='$($Contract['sessionRoot'])', expected='$sessionFull')."
    Assert-Stage5Condition ($Contract['runtimeDirectory'] -is [string] -and
        $Contract['runtimeDirectory'] -ceq $runtimeFull) `
        "$Context runtimeDirectory is stale or substituted."
    Assert-Stage5Condition ($Contract['documentsRoot'] -is [string] -and
        $Contract['documentsRoot'] -ceq $documentsRoot) `
        "$Context documentsRoot is stale or substituted."
    Assert-Stage5Condition ($Contract['profileLeaf'] -is [string] -and
        $Contract['profileLeaf'] -ceq $profileLeaf) `
        "$Context profileLeaf is stale or substituted."
    Assert-Stage5Condition ($Contract['profileRoot'] -is [string] -and
        $Contract['profileRoot'] -ceq $profileRoot) `
        "$Context profileRoot is stale or substituted."
    Assert-Stage5Condition ($Contract['peerRoot'] -is [string] -and
        $Contract['peerRoot'] -ceq $peerRoot) `
        "$Context peerRoot is stale or substituted."
    $expectedEnvironment = [ordered]@{
        TEMP = Join-Path $sessionFull 'Temp'
        TMP = Join-Path $sessionFull 'Tmp'
        LOCALAPPDATA = Join-Path $sessionFull 'LocalAppData'
        APPDATA = Join-Path $sessionFull 'AppData'
        USERPROFILE = $sessionFull
        HOMEDRIVE = 'H:'
        HOMEPATH = $sessionFull.Substring(2)
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = Join-Path $sessionFull 'Cache'
        RTS_STAGE5_VALIDATION_LOG_ROOT = Join-Path $sessionFull 'Logs'
        RTS_STAGE5_VALIDATION_DUMP_ROOT = Join-Path $sessionFull 'Dumps'
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionFull
    }
    $environmentNames = @($expectedEnvironment.Keys | ForEach-Object { [string]$_ })
    Assert-Stage5JsonShape $Contract['environmentValues'] $environmentNames `
        "$Context environment values"
    foreach ($name in $environmentNames) {
        Assert-Stage5Condition ($Contract['environmentValues'][$name] -is [string] -and
            $Contract['environmentValues'][$name] -ceq
                $expectedEnvironment[$name]) `
            "$Context environment value '$name' is stale or substituted."
    }
    Assert-Stage5Condition ($Contract['environmentVariableNames'] -is [Array] -and
        @($Contract['environmentVariableNames'] |
            Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        (@($Contract['environmentVariableNames']) -join '|') -ceq
            ($environmentNames -join '|')) `
        "$Context environment variable names are stale or reordered."
    $expectedViews = @('Registry32', 'Registry64')
    Assert-Stage5Condition ($Contract['registryViews'] -is [Array] -and
        @($Contract['registryViews'] |
            Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        (@($Contract['registryViews']) -join '|') -ceq
            ($expectedViews -join '|')) `
        "$Context registry-view coverage is incomplete or substituted."
    # The installed title consumes the complete process-local profile root
    # through RTS_STAGE5_VALIDATION_PROFILE_ROOT. Only the title install path
    # remains a registry binding; global Documents-folder and title-leaf
    # redirection must not re-enter the process-local contract.
    $expectedRegistryValues = New-Object 'Collections.Generic.List[object]'
    if ($Title -ceq 'Generals') {
        $expectedRegistryValues.Add([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    else {
        $expectedRegistryValues.Add([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    Assert-Stage5Condition ($Contract['registryValues'] -is [Array] -and
        $Contract['registryValues'].Count -eq $expectedRegistryValues.Count) `
        "$Context registry-value evidence has an unexpected count."
    for ($index = 0; $index -lt $expectedRegistryValues.Count; ++$index) {
        $actualEntry = $Contract['registryValues'][$index]
        $expectedEntry = $expectedRegistryValues[$index]
        Assert-Stage5JsonShape $actualEntry @('subKey', 'name', 'value', 'purpose') `
            "$Context registry value $index"
        foreach ($field in @('subKey', 'name', 'value', 'purpose')) {
            $actualValue = $actualEntry[$field]
            $expectedValue = $expectedEntry[$field]
            Assert-Stage5Condition ($actualValue -is [string] -and
                $expectedValue -is [string] -and
                $actualValue -ceq $expectedValue) `
                "$Context registry value $index field '$field' is stale or substituted."
        }
    }
    return $Contract
}

function Assert-Stage5LockstepRegistryEquivalence {
    param(
        [object]$Equivalence,
        [object]$TitleSessionProfile,
        [string]$Context
    )
    Assert-Stage5JsonShape $Equivalence @('strategy', 'views', 'values',
        'profileRoot') $Context
    $equivalenceStrategy = $Equivalence['strategy']
    $equivalenceProfileRoot = $Equivalence['profileRoot']
    $equivalenceViews = $Equivalence['views']
    $profileViews = $TitleSessionProfile['registryViews']
    Assert-Stage5Condition ($equivalenceStrategy -is [string] -and
        $equivalenceProfileRoot -is [string] -and
        $equivalenceViews -is [Array] -and $profileViews -is [Array] -and
        @($equivalenceViews | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        @($profileViews | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        $equivalenceStrategy -ceq 'process-local-validation-profile-root' -and
        $equivalenceProfileRoot -ceq $TitleSessionProfile['profileRoot'] -and
        (@($equivalenceViews) -join '|') -ceq
            (@($profileViews) -join '|') -and
        ($Equivalence['values'] | ConvertTo-Json -Compress -Depth 12) -ceq
            ($TitleSessionProfile['registryValues'] | ConvertTo-Json -Compress -Depth 12)) `
        "$Context is not bound to the reviewed registry equivalence."
    return $Equivalence
}

function Assert-Stage5LockstepEnvironmentEquivalence {
    param(
        [object]$Equivalence,
        [object]$TitleSessionProfile,
        [int]$PeerIndex,
        [string]$Context
    )
    Assert-Stage5JsonShape $Equivalence @('peer', 'root', 'values',
        'variableNames') $Context
    Assert-Stage5Condition (Test-Stage5JsonInteger $Equivalence['peer']) `
        "$Context peer identity must be an integer."
    Assert-Stage5Condition ($Equivalence['peer'] -ge 0 -and
        $Equivalence['peer'] -le [UInt64][Int64]::MaxValue -and
        [Int64]$Equivalence['peer'] -eq $PeerIndex) `
        "$Context peer identity is stale or substituted."
    $expectedRoot = Join-Path $TitleSessionProfile['peerRoot'] "peer-$PeerIndex"
    Assert-Stage5Condition ($Equivalence['root'] -is [string] -and
        $Equivalence['root'] -ceq $expectedRoot) `
        "$Context root is not bound to the title-session peer root."
    $baseValues = $TitleSessionProfile['environmentValues']
    $environmentNames = @($TitleSessionProfile['environmentVariableNames'])
    $expectedValues = [ordered]@{}
    foreach ($name in $environmentNames) {
        $baseValue = $baseValues[$name]
        if ($name -ceq 'TEMP' -or $name -ceq 'TMP' -or
            $name -ceq 'LOCALAPPDATA' -or $name -ceq 'APPDATA' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_CACHE_ROOT' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_LOG_ROOT' -or
            $name -ceq 'RTS_STAGE5_VALIDATION_DUMP_ROOT') {
            $expectedValues[$name] = Join-Path $expectedRoot ([IO.Path]::GetFileName($baseValue))
        }
        else { $expectedValues[$name] = $baseValue }
    }
    Assert-Stage5JsonShape $Equivalence['values'] $environmentNames `
        "$Context environment values"
    foreach ($name in $environmentNames) {
        Assert-Stage5Condition ($Equivalence['values'][$name] -is [string] -and
            $Equivalence['values'][$name] -ceq $expectedValues[$name]) `
            "$Context environment value '$name' is stale or substituted."
    }
    Assert-Stage5Condition ($Equivalence['variableNames'] -is [Array] -and
        @($Equivalence['variableNames'] |
            Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        (@($Equivalence['variableNames']) -join '|') -ceq
            ($environmentNames -join '|')) `
        "$Context environment variable names are stale or reordered."
    return $Equivalence
}

function Assert-Stage5LockstepPeerLaunchBinding {
    param(
        [object]$Peer,
        [object]$LauncherContract,
        [string]$Title,
        [int]$PeerIndex,
        [int]$PeerCount,
        [int[]]$Ports,
        [string]$RunNonce,
        [string]$SessionNonce,
        [string]$MapName,
        [UInt32]$MapCrc,
        [int]$Seed,
        [string]$SessionDirectory,
        [string]$Context
    )
    $peerWorkingDirectory = $Peer['workingDirectory']
    $launcherDirectWorkingDirectory = $LauncherContract['directWorkingDirectory']
    $peerLauncherPath = $Peer['launcherPath']
    $launcherPath = $LauncherContract['launcherPath']
    $peerLauncherConfigPath = $Peer['launcherConfigPath']
    $launcherConfigPath = $LauncherContract['configPath']
    $peerLauncherSha256 = $Peer['launcherSha256']
    $launcherSha256 = $LauncherContract['launcherSha256']
    $peerLauncherConfigSha256 = $Peer['launcherConfigSha256']
    $launcherConfigSha256 = $LauncherContract['configSha256']
    Assert-Stage5Condition ($peerWorkingDirectory -is [string] -and
        $launcherDirectWorkingDirectory -is [string] -and
        $peerLauncherPath -is [string] -and $launcherPath -is [string] -and
        $peerLauncherConfigPath -is [string] -and $launcherConfigPath -is [string] -and
        $peerLauncherSha256 -is [string] -and $launcherSha256 -is [string] -and
        $peerLauncherConfigSha256 -is [string] -and $launcherConfigSha256 -is [string] -and
        $peerLauncherSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $launcherSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $peerLauncherConfigSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $launcherConfigSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $Peer['directExecutionOptIn'] -is [bool] -and
        [bool]$Peer['directExecutionOptIn'] -and
        $peerWorkingDirectory -ceq $launcherDirectWorkingDirectory -and
        $peerLauncherPath -ceq $launcherPath -and
        $peerLauncherConfigPath -ceq $launcherConfigPath -and
        $peerLauncherSha256 -ceq $launcherSha256 -and
        $peerLauncherConfigSha256 -ceq $launcherConfigSha256) `
        "$Context launch provenance is substituted or stale."
    $launcherDefaultArguments = $Peer['launcherDefaultArguments']
    $launcherArguments = $LauncherContract['launcherArguments']
    $peerArguments = $Peer['arguments']
    $directArguments = $Peer['directArguments']
    Assert-Stage5Condition ($launcherDefaultArguments -is [Array] -and
        $launcherArguments -is [Array] -and $peerArguments -is [Array] -and
        $directArguments -is [Array] -and
        $launcherDefaultArguments.Count -eq 4 -and
        $launcherArguments.Count -eq 4 -and
        @($launcherDefaultArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        @($launcherArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        @($peerArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        @($directArguments | Where-Object { $_ -isnot [string] }).Count -eq 0 -and
        ($launcherDefaultArguments -join '|') -ceq
            ($launcherArguments -join '|') -and
        ($directArguments -join '|') -ceq ($peerArguments -join '|') -and
        $peerArguments[0] -ceq '-simulationMode' -and
        $peerArguments[1] -ceq 'parallel' -and
        $peerArguments[2] -ceq '-workerPolicy' -and
        $peerArguments[3] -ceq 'auto') `
        "$Context process arguments do not bind the reviewed lockstep-v2 launch."
    $arguments = @($peerArguments)
    $markerIndices = @(
        for ($argumentIndex = 4; $argumentIndex -lt $arguments.Count; ++$argumentIndex) {
            if ($arguments[$argumentIndex] -ceq '-installedLockstepV2Validation') {
                $argumentIndex
            }
        }
    )
    Assert-Stage5Condition ($markerIndices.Count -eq 1 -and
        $markerIndices[0] -gt 4 -and $markerIndices[0] -lt ($arguments.Count - 1)) `
        "$Context process arguments do not contain one terminal lockstep-v2 validation switch."
    $overrideArguments = $Peer['workerOverride'].overrideArguments
    Assert-Stage5Condition ($overrideArguments -is [Array] -and
        @($overrideArguments | Where-Object { $_ -isnot [string] }).Count -eq 0) `
        "$Context worker override arguments must be JSON strings."
    $overrideArguments = @($overrideArguments)
    Assert-Stage5Condition ($markerIndices[0] -eq (4 + $overrideArguments.Count) -and
        (@($arguments[4..($markerIndices[0] - 1)]) -join '|') -ceq
            ($overrideArguments -join '|')) `
        "$Context process arguments do not bind the executable worker override."
    $configuration = $arguments[$markerIndices[0] + 1]
    $peerExecutableSha256 = $Peer['executableSha256']
    $peerSourceCommit = $Peer['sourceCommit']
    $peerReceiptPath = $Peer['receiptPath']
    Assert-Stage5Condition ($peerExecutableSha256 -is [string] -and
        $peerExecutableSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $peerSourceCommit -is [string] -and
        $peerSourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $peerReceiptPath -is [string] -and
        $configuration -is [string]) `
        "$Context executable, source, receipt, and configuration identities must be JSON strings."
    $portsText = ($Ports | ForEach-Object { [string]$_ }) -join ','
    $pattern = '^peer=(?<peer>[0-9]+);peers=(?<peers>[0-9]+);ports=(?<ports>[0-9,]+);run=(?<run>[0-9A-F]{32});session=(?<session>[0-9A-F]{32});exe=(?<exe>[0-9A-F]{64});source=(?<source>[0-9a-f]{40});map=(?<map>[^;]+);map_crc=(?<mapCrc>[0-9]+);seed=(?<seed>[0-9]+);dir=(?<dir>[^;]+);receipt=(?<receipt>[^;]+);mode=trusted-router;router=(?<router>0);network_roster=(?<networkRoster>[0-9]+);simulation_roster=(?<simulationRoster>[0-9]+);ai_roster=(?<aiRoster>[0-9]+)$'
    $match = [regex]::Match($configuration, $pattern)
    Assert-Stage5Condition ($match.Success -and
        $match.Groups['peer'].Value -ceq [string]$PeerIndex -and
        $match.Groups['peers'].Value -ceq [string]$PeerCount -and
        $match.Groups['ports'].Value -ceq $portsText -and
        $match.Groups['run'].Value -ceq $RunNonce -and
        $match.Groups['session'].Value -ceq $SessionNonce -and
        $match.Groups['exe'].Value -ceq $peerExecutableSha256.ToUpperInvariant() -and
        $match.Groups['source'].Value -ceq $peerSourceCommit -and
        $match.Groups['map'].Value -ceq $MapName -and
        $match.Groups['mapCrc'].Value -ceq [string]$MapCrc -and
        $match.Groups['seed'].Value -ceq [string]$Seed -and
        [IO.Path]::GetFullPath($match.Groups['dir'].Value) -ceq
            [IO.Path]::GetFullPath($SessionDirectory) -and
        $match.Groups['receipt'].Value -ceq $peerReceiptPath -and
        $match.Groups['router'].Value -ceq '0' -and
        $match.Groups['networkRoster'].Value -ceq '3' -and
        $match.Groups['simulationRoster'].Value -ceq '63' -and
        $match.Groups['aiRoster'].Value -ceq '60') `
        "$Context configuration does not bind the exact peer, port, run, map, or receipt identity."
    $peerCommandLine = $Peer['commandLine']
    $launcherExecutable = $LauncherContract['directExecutable']
    Assert-Stage5Condition ($peerCommandLine -is [string] -and
        $launcherExecutable -is [string] -and
        $peerCommandLine.StartsWith('"' + $launcherExecutable + '" ',
            [StringComparison]::Ordinal) -and
        $peerCommandLine.IndexOf($configuration) -ge 0) `
        "$Context command line does not bind the retained executable and configuration identity."
}

function Assert-Stage5LockstepWorkerEvidence {
    param(
        [object]$Peer,
        [int]$PeerIndex,
        [string]$Context
    )
    $expectedProfile = if (($PeerIndex % 2) -eq 0) {
        [ordered]@{
            profile = 'explicit-two-workers'; requestedWorkers = '2'
            workerPolicy = 'all'; overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
        }
    }
    else {
        [ordered]@{
            profile = 'automatic-workers'; requestedWorkers = 'auto'
            workerPolicy = 'auto'; overrideArguments = @('-workerPolicy', 'auto')
        }
    }
    Assert-Stage5JsonShape $Peer['workerOverride'] @('profile',
        'requestedWorkers', 'workerPolicy', 'overrideArguments') "$Context worker override"
    Assert-Stage5Condition ($Peer['workerOverride']['profile'] -ceq $expectedProfile.profile -and
        $Peer['workerOverride']['requestedWorkers'] -ceq $expectedProfile.requestedWorkers -and
        $Peer['workerOverride']['workerPolicy'] -ceq $expectedProfile.workerPolicy -and
        $Peer['workerOverride']['overrideArguments'] -is [Array] -and
        (@($Peer['workerOverride']['overrideArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedProfile.overrideArguments -join '|')) `
        "$Context worker override is stale, homogeneous, or substituted."
    Assert-Stage5Condition ($Peer['requestedWorkers'] -ceq $expectedProfile.requestedWorkers -and
        $Peer['workerPolicy'] -ceq $expectedProfile.workerPolicy) `
        "$Context worker profile summary is substituted."
    Assert-Stage5JsonShape $Peer['receiptWorkerTelemetry'] @('requestedWorkers',
        'workerPolicy', 'effectiveWorkers', 'distinctPhysicalWorkers',
        'physicalWorkerMasks', 'executableOrigin') "$Context receipt worker telemetry"
    $telemetryEffectiveWorkers = $Peer['receiptWorkerTelemetry']['effectiveWorkers']
    Assert-Stage5Condition (Test-Stage5JsonInteger $telemetryEffectiveWorkers) `
        "$Context receipt worker telemetry effectiveWorkers must be an integer."
    Assert-Stage5Condition ($telemetryEffectiveWorkers -ge 0 -and
        $telemetryEffectiveWorkers -le [UInt64][Int32]::MaxValue) `
        "$Context receipt worker telemetry effectiveWorkers is outside the Int32 range."
    Assert-Stage5Condition ($Peer['receiptWorkerTelemetry']['requestedWorkers'] -ceq
        $expectedProfile.requestedWorkers -and
        $Peer['receiptWorkerTelemetry']['workerPolicy'] -ceq $expectedProfile.workerPolicy -and
        $telemetryEffectiveWorkers -ge 2 -and
        $Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'] -is [Array] -and
        $Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'].Count -eq 6 -and
        $Peer['receiptWorkerTelemetry']['physicalWorkerMasks'] -is [Array] -and
        $Peer['receiptWorkerTelemetry']['physicalWorkerMasks'].Count -eq 6 -and
        $Peer['receiptWorkerTelemetry']['executableOrigin'] -is [bool] -and
        [bool]$Peer['receiptWorkerTelemetry']['executableOrigin']) `
        "$Context receipt worker telemetry is incomplete or not executable-originated."
    foreach ($distinctValue in @($Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'])) {
        Assert-Stage5Condition (Test-Stage5JsonInteger $distinctValue) `
            "$Context receipt worker telemetry has a non-integer effective worker count."
        Assert-Stage5Condition ($distinctValue -ge 2 -and
            $distinctValue -le [UInt64][Int32]::MaxValue) `
            "$Context receipt worker telemetry effective worker count is outside the Int32 range."
    }
    foreach ($maskValue in @($Peer['receiptWorkerTelemetry']['physicalWorkerMasks'])) {
        Assert-Stage5Condition (Test-Stage5JsonInteger $maskValue) `
            "$Context receipt worker telemetry physical-worker mask must be an integer."
        Assert-Stage5Condition ($maskValue -gt 0 -and
            $maskValue -le [UInt64]::MaxValue) `
            "$Context receipt worker telemetry has an empty physical-worker mask."
    }
    $effective = [int]$telemetryEffectiveWorkers
    $distinct = @($Peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'] |
        ForEach-Object { [int]$_ })
    Assert-Stage5Condition (@($distinct | Select-Object -Unique).Count -eq 1 -and
        $distinct[0] -eq $effective -and
        (($expectedProfile.requestedWorkers -ceq 'auto' -and $effective -gt 2) -or
            ($expectedProfile.requestedWorkers -ne 'auto' -and $effective -eq 2))) `
        "$Context receipt worker telemetry does not prove the reviewed effective worker profile."
    Assert-Stage5JsonShape $Peer['stdoutProof'] @('executableOrigin', 'peer',
        'pid', 'frameLimit', 'activeMarker', 'passMarker', 'finalCrc') "$Context stdout proof"
    foreach ($field in @('peer', 'pid', 'frameLimit')) {
        $value = $Peer['stdoutProof'][$field]
        Assert-Stage5Condition (Test-Stage5JsonInteger $value) `
            "$Context stdout proof field '$field' must be an integer."
        Assert-Stage5Condition ($value -ge 0 -and
            $value -le [UInt64][Int64]::MaxValue) `
            "$Context stdout proof field '$field' is outside the Int64 range."
    }
    Assert-Stage5Condition ($Peer['stdoutProof']['executableOrigin'] -is [bool] -and
        [bool]$Peer['stdoutProof']['executableOrigin'] -and
        [Int64]$Peer['stdoutProof']['peer'] -eq $PeerIndex -and
        [Int64]$Peer['stdoutProof']['pid'] -gt 0 -and
        [Int64]$Peer['stdoutProof']['frameLimit'] -eq 4096 -and
        $Peer['stdoutProof']['activeMarker'] -is [string] -and
        $Peer['stdoutProof']['passMarker'] -is [string] -and
        $Peer['stdoutProof']['finalCrc'] -is [string] -and
        $Peer['stdoutProof']['finalCrc'] -cmatch '^[0-9A-F]{8}$') `
        "$Context stdout proof is not a canonical bounded lockstep-v2 marker set."
    return [pscustomobject]@{
        effectiveWorkers = $effective
        expectedProfile = $expectedProfile
    }
}

function Get-Stage5LockstepNegativeProbePairs {
    param(
        [string]$Path,
        [object]$Snapshot = $null
    )
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Lockstep-v2 negative probe proof'
    }
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'path' -and
        [IO.Path]::GetFullPath([string]$Snapshot.path) -ceq $full) `
        "Lockstep-v2 negative probe snapshot is bound to a different path: $full"
    $text = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    Assert-Stage5Condition ($text.IndexOf("`r", [StringComparison]::Ordinal) -lt 0) `
        "Lockstep-v2 negative probe proof contains non-canonical CR line endings: $full"
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
    Assert-Stage5Condition ($lines.Count -eq ($expectedKeys.Count + 2) -and
        $lines[0] -ceq 'RTS_LOCKSTEP_V2_NEGATIVE_PROBE' -and
        $lines[$lines.Count - 1] -ceq 'END') `
        "Lockstep-v2 negative probe proof is not a canonical v2 document: $full"
    $pairs = [ordered]@{}
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        $line = $lines[$index + 1]
        Assert-Stage5Condition ($line.Length -gt 0) `
            "Lockstep-v2 negative probe proof contains an empty line: $full"
        $equals = $line.IndexOf('=', [StringComparison]::Ordinal)
        Assert-Stage5Condition ($equals -gt 0 -and $equals -lt ($line.Length - 1) -and
            $line.Substring(0, $equals) -match '^[A-Za-z_][A-Za-z0-9_]*$') `
            "Lockstep-v2 negative probe proof contains a malformed key/value line: $full"
        $key = $line.Substring(0, $equals)
        Assert-Stage5Condition ($key -ceq $expectedKeys[$index] -and
            -not $pairs.Contains($key)) `
            "Lockstep-v2 negative probe proof field order/shape mismatch at ${index}: $full"
        $pairs[$key] = $line.Substring($equals + 1)
    }
    return [pscustomobject]@{
        path = $full; pairs = $pairs; text = $text; snapshot = $Snapshot
    }
}

function Get-Stage5LockstepNegativeStdoutProof {
    param([object]$Snapshot, [string]$Context)
    Assert-Stage5Condition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'bytes') `
        "$Context does not contain a stdout snapshot."
    $text = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    Assert-Stage5Condition (-not [string]::IsNullOrEmpty($text)) `
        "$Context did not provide stdout."
    $lines = @($text -split "`n" | ForEach-Object { $_.TrimEnd("`r") })
    $passLines = @($lines | Where-Object {
        $_.StartsWith('LOCKSTEP_V2_NEGATIVE_PROBE_PASS ',
            [StringComparison]::Ordinal)
    })
    Assert-Stage5Condition ($passLines.Count -eq 1) `
        "$Context must provide exactly one negative-probe pass marker."
    $match = [regex]::Match($passLines[0],
        '^LOCKSTEP_V2_NEGATIVE_PROBE_PASS mode=(?<mode>negative-(?:cross-epoch|content-mismatch)) pid=(?<pid>[0-9]+) rejection=(?<error>[A-Za-z]+)$')
    Assert-Stage5Condition ($match.Success) `
        "$Context negative-probe pass marker has an unsupported shape."
    Assert-Stage5Condition ($text -notmatch 'NET3_VALIDATION_PEER_PASS') `
        "$Context contains a diagnostic NET3 pass marker."
    return [pscustomobject]@{
        marker = $passLines[0]
        mode = $match.Groups['mode'].Value
        pid = [int]$match.Groups['pid'].Value
        rejection = $match.Groups['error'].Value
    }
}

function Assert-Stage5LockstepNegativeProbeEvidence {
    param(
        [object]$Entry,
        [string]$EvidenceRoot,
        [string]$ExpectedTitle,
        [string]$ExpectedMode,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedExecutableSha256,
        [UInt32]$ExpectedMapCrc,
        [int]$ExpectedSeed,
        [string]$ExpectedExecutablePath = $null,
        [string]$ExpectedCohortCreatedUtc = $null
    )
    $context = "Lockstep-v2 $ExpectedTitle/$ExpectedMode negative probe"
    $required = @(
        'title', 'mode', 'producer', 'processId', 'processCreationUtc',
        'executablePath', 'runNonce', 'sessionNonce',
        'executableSha256', 'sourceCommit', 'proofPath', 'proofSha256',
        'stdoutPath', 'stdoutSha256', 'stderrPath', 'stderrSha256',
        'inputSha256', 'baselineAccepted', 'mutatedAccepted', 'mutation',
        'expectedError', 'observedError', 'exitCode', 'commandLine',
        'arguments', 'probeBuildCrc', 'probeContentCrc')
    Assert-Stage5JsonShape $Entry $required $context
    $entryTitle = Get-Stage5JsonValue $Entry 'title' $context
    $entryMode = Get-Stage5JsonValue $Entry 'mode' $context
    $entryProducer = Get-Stage5JsonValue $Entry 'producer' $context
    $processId = Get-Stage5JsonValue $Entry 'processId' $context
    $processCreationUtc = Get-Stage5JsonValue $Entry 'processCreationUtc' $context
    $executablePath = Get-Stage5JsonValue $Entry 'executablePath' $context
    $runNonce = Get-Stage5JsonValue $Entry 'runNonce' $context
    $sessionNonce = Get-Stage5JsonValue $Entry 'sessionNonce' $context
    $executableSha256 = Get-Stage5JsonValue $Entry 'executableSha256' $context
    $sourceCommit = Get-Stage5JsonValue $Entry 'sourceCommit' $context
    Assert-Stage5Condition (Test-Stage5JsonInteger $processId) `
        "$context processId must be an integer."
    Assert-Stage5Condition ($processId -gt 0 -and
        $processId -le [UInt64][Int32]::MaxValue) `
        "$context processId is outside the Int32 range."
    Assert-Stage5Condition ($entryTitle -is [string] -and $entryTitle -ceq $ExpectedTitle -and
        $entryMode -is [string] -and $entryMode -ceq $ExpectedMode -and
        $entryProducer -is [string] -and $entryProducer -ceq 'installed-lockstep-v2' -and
        [Int64]$processId -gt 0 -and
        $processCreationUtc -is [string] -and
        $executablePath -is [string] -and
        -not [string]::IsNullOrWhiteSpace($executablePath) -and
        $runNonce -is [string] -and $runNonce -cmatch '^[0-9A-F]{32}$' -and
        $sessionNonce -is [string] -and $sessionNonce -cmatch '^[0-9A-F]{32}$' -and
        $executableSha256 -is [string] -and $executableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $executableSha256 -ceq $ExpectedExecutableSha256.ToUpperInvariant() -and
        $sourceCommit -is [string] -and $sourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $sourceCommit -ceq $ExpectedSourceCommit) `
        "$context identity is stale, substituted, or noncanonical."
    [DateTimeOffset]$processCreated = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($processCreationUtc,
        [ref]$processCreated)) "$context processCreationUtc is not a valid timestamp."
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortCreatedUtc)) {
        [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
            [ref]$cohortCreated) -and $processCreated -ge $cohortCreated) `
            "$context processCreationUtc predates the execution cohort."
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedExecutablePath)) {
        Assert-Stage5Condition ([IO.Path]::GetFullPath($executablePath) -ceq
            [IO.Path]::GetFullPath($ExpectedExecutablePath)) `
            "$context executablePath is not the canonical installed executable."
    }
    $expectedError = if ($ExpectedMode -ceq 'negative-cross-epoch') {
        'UnsupportedEngineEpoch'
    }
    else { 'ContentHashMismatch' }
    $expectedMutation = if ($ExpectedMode -ceq 'negative-cross-epoch') {
        'engine-epoch'
    }
    else { 'content-hash' }
    $expectedProof = Get-Stage5JsonValue $Entry 'proofPath' $context
    $expectedStdout = Get-Stage5JsonValue $Entry 'stdoutPath' $context
    $expectedStderr = Get-Stage5JsonValue $Entry 'stderrPath' $context
    foreach ($relative in @($expectedProof, $expectedStdout, $expectedStderr)) {
        Assert-Stage5Condition ($relative -is [string]) "$context evidence path is not a JSON string."
    }
    $proofPath = Resolve-Stage5FinalAcceptanceFile $EvidenceRoot $expectedProof "$context proof"
    $stdoutPath = Resolve-Stage5FinalAcceptanceFile $EvidenceRoot $expectedStdout "$context stdout"
    $stderrPath = Resolve-Stage5FinalAcceptanceFile $EvidenceRoot $expectedStderr "$context stderr"
    $proofSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $proofPath `
        "$context proof" -EvidenceKind RawLog
    $stdoutSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $stdoutPath `
        "$context stdout" -EvidenceKind RawLog
    $stderrSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $stderrPath `
        "$context stderr" -EvidenceKind RawLog
    foreach ($binding in @(
        @($proofSnapshot, (Get-Stage5JsonValue $Entry 'proofSha256' $context), 'proof'),
        @($stdoutSnapshot, (Get-Stage5JsonValue $Entry 'stdoutSha256' $context), 'stdout'),
        @($stderrSnapshot, (Get-Stage5JsonValue $Entry 'stderrSha256' $context), 'stderr')
    )) {
        Assert-Stage5FinalAcceptanceSnapshotSha256 $binding[0] $binding[1] `
            "$context $($binding[2]) SHA-256 binding" | Out-Null
    }
    $parsed = Get-Stage5LockstepNegativeProbePairs $proofPath $proofSnapshot
    $pairs = $parsed.pairs
    Assert-Stage5Condition ($pairs['producer'] -ceq 'installed-lockstep-v2' -and
        $pairs['mode'] -ceq $ExpectedMode -and $pairs['schema'] -ceq '2' -and
        $pairs['protocol_epoch'] -ceq '2' -and
        $pairs['run_nonce'] -ceq $runNonce -and
        $pairs['session_nonce'] -ceq $sessionNonce -and
        $pairs['executable_sha256'] -ceq $executableSha256 -and
        $pairs['source_revision'] -ceq $ExpectedSourceCommit -and
        $pairs['mutation'] -ceq $expectedMutation -and
        $pairs['expected_error'] -ceq $expectedError -and
        $pairs['observed_error'] -ceq $expectedError -and
        $pairs['baseline_input_sha256'] -match '^[0-9A-F]{64}$' -and
        $pairs['input_sha256'] -match '^[0-9A-F]{64}$' -and
        $pairs['baseline_input_sha256'] -cne $pairs['input_sha256'] -and
        $pairs['baseline_accepted'] -ceq '1' -and
        $pairs['mutated_accepted'] -ceq '0' -and
        [UInt64](ConvertTo-Stage5LockstepReceiptUInt64 $pairs['probe_build_compatibility_crc'] 'probe_build_compatibility_crc') -eq [UInt64]$ExpectedMapCrc -and
        [UInt64](ConvertTo-Stage5LockstepReceiptUInt64 $pairs['probe_content_crc'] 'probe_content_crc') -eq [UInt64]$ExpectedSeed -and
        [UInt64](ConvertTo-Stage5LockstepReceiptUInt64 $pairs['process_id'] 'process_id') -eq [UInt64]$processId) `
        "$context raw proof is not bound to the expected observed rejection."
    $stdoutProof = Get-Stage5LockstepNegativeStdoutProof $stdoutSnapshot "$context stdout"
    $entryInput = Get-Stage5JsonValue $Entry 'inputSha256' $context
    $entryExpectedError = Get-Stage5JsonValue $Entry 'expectedError' $context
    $entryObservedError = Get-Stage5JsonValue $Entry 'observedError' $context
    $entryMutation = Get-Stage5JsonValue $Entry 'mutation' $context
    $entryBaseline = Get-Stage5JsonValue $Entry 'baselineAccepted' $context
    $entryMutated = Get-Stage5JsonValue $Entry 'mutatedAccepted' $context
    $exitCode = Get-Stage5JsonValue $Entry 'exitCode' $context
    $commandLine = Get-Stage5JsonValue $Entry 'commandLine' $context
    $arguments = Get-Stage5JsonValue $Entry 'arguments' $context
    $probeBuildCrc = Get-Stage5JsonValue $Entry 'probeBuildCrc' $context
    $probeContentCrc = Get-Stage5JsonValue $Entry 'probeContentCrc' $context
    foreach ($field in @('exitCode', 'probeBuildCrc', 'probeContentCrc')) {
        $value = Get-Stage5JsonValue $Entry $field $context
        Assert-Stage5Condition (Test-Stage5JsonInteger $value) `
            "$context field '$field' must be an integer."
        $maximum = if ($field -ceq 'exitCode') {
            [UInt64][Int64]::MaxValue
        }
        else { [UInt64]::MaxValue }
        Assert-Stage5Condition ($value -ge 0 -and $value -le $maximum) `
            "$context field '$field' is outside its integer range."
    }
    Assert-Stage5Condition ($entryInput -is [string] -and
        $entryInput -cmatch '^[0-9A-F]{64}$' -and
        $entryInput -ceq $pairs['input_sha256'] -and
        $entryExpectedError -is [string] -and $entryExpectedError -ceq $expectedError -and
        $entryObservedError -is [string] -and $entryObservedError -ceq $expectedError -and
        $entryMutation -is [string] -and $entryMutation -ceq $expectedMutation -and
        $entryBaseline -is [bool] -and [bool]$entryBaseline -and
        $entryMutated -is [bool] -and -not [bool]$entryMutated -and
        [Int64]$exitCode -eq 0 -and
        $commandLine -is [string] -and
        $commandLine -match [regex]::Escape($ExpectedMode) -and
        $arguments -is [Array] -and @($arguments).Count -ge 2 -and
        @($arguments | Where-Object {
            [string]$_ -ceq '-installedLockstepV2Validation'
        }).Count -eq 1 -and
        [UInt64]$probeBuildCrc -eq [UInt64]$ExpectedMapCrc -and
        [UInt64]$probeContentCrc -eq [UInt64]$ExpectedSeed -and
        $stdoutProof.mode -ceq $ExpectedMode -and
        $stdoutProof.pid -eq [Int64]$processId -and
        $stdoutProof.rejection -ceq $expectedError) `
        "$context metadata is stale, substituted, or detached from the raw proof."
    return [pscustomobject]@{
        title = $ExpectedTitle; mode = $ExpectedMode
        processId = [int]$processId; runNonce = [string]$runNonce
        processCreationUtc = [string]$processCreationUtc
        executablePath = [string]$executablePath
        sessionNonce = [string]$sessionNonce; proofPath = $proofPath
        stdoutPath = $stdoutPath; stderrPath = $stderrPath
        proofSha256 = [string]$proofSnapshot.sha256
        stdoutSha256 = [string]$stdoutSnapshot.sha256
        stderrSha256 = [string]$stderrSnapshot.sha256
        inputSha256 = [string]$pairs['input_sha256']
        baselineAccepted = $true; mutatedAccepted = $false
        expectedError = $expectedError; observedError = $expectedError
    }
}

function Read-Stage5SimulationQualificationDataEvidence {
    param(
        [string]$Path,
        [object]$Binding,
        [string]$ExpectedSourceCommit,
        [ValidateSet('Generals', 'ZeroHour')][string]$ExpectedTitle
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Stage 5 simulation qualification-data manifest was not found: $full"
    Assert-Stage5Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Stage 5 simulation qualification-data ExpectedSourceCommit must be canonical lowercase 40-hex.'
    Assert-Stage5JsonShape $Binding @('path', 'title', 'manifestSha256',
        'closureSha256', 'fileCount') `
        'Stage 5 simulation qualification-data binding'
    $bindingPath = Get-Stage5JsonValue $Binding 'path' `
        'Stage 5 simulation qualification-data binding'
    $bindingTitle = Get-Stage5JsonValue $Binding 'title' `
        'Stage 5 simulation qualification-data binding'
    $expectedManifestSha256 = Get-Stage5JsonValue $Binding 'manifestSha256' `
        'Stage 5 simulation qualification-data binding'
    $expectedClosureSha256 = Get-Stage5JsonValue $Binding 'closureSha256' `
        'Stage 5 simulation qualification-data binding'
    $expectedFileCount = Get-Stage5JsonValue $Binding 'fileCount' `
        'Stage 5 simulation qualification-data binding'
    Assert-Stage5Condition ($bindingPath -is [string] -and
        $bindingPath -ceq 'QualificationData.json' -and
        [IO.Path]::GetFileName($full) -ceq $bindingPath -and
        $bindingTitle -is [string] -and $bindingTitle -ceq $ExpectedTitle -and
        $expectedManifestSha256 -is [string] -and
        $expectedManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $expectedClosureSha256 -is [string] -and
        $expectedClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $expectedFileCount)) `
        'Stage 5 simulation qualification-data binding is malformed, stale, or substituted.'

    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        'Stage 5 simulation qualification-data manifest'
    $manifestSha256 = Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        $expectedManifestSha256 'Stage 5 simulation qualification-data manifest'
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot `
        'Stage 5 simulation qualification-data manifest'
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'title', 'archiveSource', 'files',
        'closureSha256') 'Stage 5 simulation qualification-data manifest'
    $manifestTitle = Get-Stage5JsonValue $document 'title' `
        'Stage 5 simulation qualification-data manifest'
    $manifestSourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Stage 5 simulation qualification-data manifest'
    $manifestClosureSha256 = Get-Stage5JsonValue $document 'closureSha256' `
        'Stage 5 simulation qualification-data manifest'
    $qualificationDataSchemaVersion = Get-Stage5JsonValue $document `
        'schemaVersion' 'Stage 5 simulation qualification-data manifest'
    $manifestEvidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Stage 5 simulation qualification-data manifest'
    $manifestProducer = Get-Stage5JsonValue $document 'producer' `
        'Stage 5 simulation qualification-data manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $qualificationDataSchemaVersion) -and
        $qualificationDataSchemaVersion -eq 1 -and
        $manifestEvidenceKind -is [string] -and
        $manifestProducer -is [string] -and
        $manifestEvidenceKind -ceq 'stage5-simulation-qualification-data' -and
        $manifestProducer -ceq 'genci-r2-trimmed-data' -and
        $manifestSourceCommit -is [string] -and
        $manifestSourceCommit -ceq $ExpectedSourceCommit -and
        $manifestTitle -is [string] -and $manifestTitle -ceq $ExpectedTitle -and
        $manifestClosureSha256 -is [string] -and
        $manifestClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
        $manifestClosureSha256 -ceq $expectedClosureSha256) `
        'Stage 5 simulation qualification-data sourceCommit, title, or closure identity is stale or substituted.'

    $expectedArchive = if ($ExpectedTitle -ceq 'Generals') {
        [ordered]@{
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        }
    }
    else {
        [ordered]@{
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
    }
    $archiveSource = Get-Stage5JsonValue $document 'archiveSource' `
        'Stage 5 simulation qualification-data manifest'
    Assert-Stage5JsonShape $archiveSource @('object', 'sha256') `
        'Stage 5 simulation qualification-data archive source'
    $archiveObject = Get-Stage5JsonValue $archiveSource 'object' `
        'Stage 5 simulation qualification-data archive source'
    $archiveHash = Get-Stage5JsonValue $archiveSource 'sha256' `
        'Stage 5 simulation qualification-data archive source'
    Assert-Stage5Condition (
        $archiveObject -is [string] -and $archiveHash -is [string] -and
        $archiveObject -ceq $expectedArchive.object -and
        $archiveHash -ceq $expectedArchive.sha256) `
        'Stage 5 simulation qualification-data archive source is unreviewed or substituted.'

    [string[]]$requiredFiles = if ($ExpectedTitle -ceq 'Generals') {
        @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
    }
    else {
        @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    [Array]::Sort($requiredFiles, [StringComparer]::Ordinal)
    $requiredSet = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    foreach ($required in $requiredFiles) { [void]$requiredSet.Add($required) }
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $files = Get-Stage5JsonValue $document 'files' `
        'Stage 5 simulation qualification-data manifest'
    Assert-Stage5Condition ($files -is [Array] -and
        $files.Count -eq $requiredFiles.Count -and
        [Int64]$expectedFileCount -eq $requiredFiles.Count) `
        'Stage 5 simulation qualification-data file coverage or count is incomplete.'
    $previousPath = $null
    foreach ($entry in $files) {
        Assert-Stage5JsonShape $entry @('path', 'sha256') `
            'Stage 5 simulation qualification-data file'
        $relative = Get-Stage5JsonValue $entry 'path' `
            'Stage 5 simulation qualification-data file'
        $hash = Get-Stage5JsonValue $entry 'sha256' `
            'Stage 5 simulation qualification-data file'
        $segments = if ($relative -is [string]) { @($relative -split '/') }
            else { @() }
        $hasUnsafeSegment = @($segments | Where-Object {
            [string]::IsNullOrEmpty($_) -or $_ -ceq '.' -or $_ -ceq '..' -or
                $_.EndsWith('.') -or $_.EndsWith(' ')
        }).Count -ne 0
        $isRootBig = $relative -is [string] -and
            $relative.IndexOf('/') -lt 0 -and
            $relative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $relative -is [string] -and
            $relative.StartsWith('Data/', [StringComparison]::OrdinalIgnoreCase)
        Assert-Stage5Condition ($relative -is [string] -and
            $relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*\z' -and
            $relative -cnotmatch '[\x00-\x1F\x7F]' -and
            -not $hasUnsafeSegment -and ($isRootBig -or $isDataFile) -and
            $requiredSet.Contains($relative) -and
            $hash -is [string] -and $hash -cmatch '^[0-9A-F]{64}$' -and
            ($null -eq $previousPath -or
                [StringComparer]::Ordinal.Compare($previousPath, $relative) -lt 0) -and
            $seen.Add($relative)) `
            "Stage 5 simulation qualification-data file is unsafe, undeclared, duplicated, or unsorted: $relative"
        $canonicalLines.Add(('{0}|{1}' -f $relative, $hash)) | Out-Null
        $previousPath = $relative
    }
    Assert-Stage5Condition ($seen.Count -eq $requiredSet.Count) `
        'Stage 5 simulation qualification-data manifest omits a required BIG/Data file.'
    $canonicalText = ($canonicalLines.ToArray() -join "`n") + "`n"
    $computedClosureSha256 = Get-Stage5FinalAcceptanceSha256FromBytes `
        ([Text.Encoding]::UTF8.GetBytes($canonicalText))
    Assert-Stage5Condition ($computedClosureSha256 -ceq
        $manifestClosureSha256) `
        'Stage 5 simulation qualification-data file closure SHA-256 is stale or substituted.'

    return [pscustomobject]@{
        path = $full
        title = $manifestTitle
        sourceCommit = $manifestSourceCommit
        manifestSha256 = $manifestSha256
        closureSha256 = $computedClosureSha256
        fileCount = $files.Count
        archiveSource = $archiveSource
        files = @($files)
    }
}

function Read-Stage5LockstepQualificationDataEvidence {
    param(
        [string]$Path,
        [object]$Binding,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedMapName,
        [object]$ExpectedMapCrcs
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
        "Lockstep-v2 retained qualification-data manifest was not found: $full"
    Assert-Stage5JsonShape $Binding `
        @('manifestSha256', 'closureSha256', 'fileCount') `
        'Lockstep-v2 qualification-data binding'
    $expectedManifestSha256 = Get-Stage5JsonValue $Binding 'manifestSha256' `
        'Lockstep-v2 qualification-data binding'
    $expectedClosureSha256 = Get-Stage5JsonValue $Binding 'closureSha256' `
        'Lockstep-v2 qualification-data binding'
    $expectedFileCount = Get-Stage5JsonValue $Binding 'fileCount' `
        'Lockstep-v2 qualification-data binding'
    Assert-Stage5Condition ($expectedManifestSha256 -is [string] -and
        $expectedManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $expectedClosureSha256 -is [string] -and
        $expectedClosureSha256 -cmatch '^[0-9A-F]{64}$' -and
        (Test-Stage5JsonInteger $expectedFileCount) -and
        [Int64]$expectedFileCount -ge 12) `
        'Lockstep-v2 qualification-data binding is malformed.'

    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestSha256 = Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot `
        $expectedManifestSha256 'Lockstep-v2 qualification-data manifest'
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot `
        'Lockstep-v2 retained qualification-data manifest'
    Assert-Stage5JsonShape $document @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'productSet', 'mapName', 'mapCrcs',
        'archiveSources', 'files', 'closureSha256') `
        'Lockstep-v2 retained qualification-data manifest'
    $productSet = Get-Stage5JsonValue $document 'productSet' `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestMapCrcs = Assert-Stage5LockstepMapCrcs `
        (Get-Stage5JsonValue $document 'mapCrcs' `
            'Lockstep-v2 retained qualification-data manifest') `
        'Lockstep-v2 retained qualification-data map CRCs'
    $expectedMapCrcs = Assert-Stage5LockstepMapCrcs $ExpectedMapCrcs `
        'Lockstep-v2 expected qualification-data map CRCs'
    $manifestEvidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestProducer = Get-Stage5JsonValue $document 'producer' `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestSourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestMapName = Get-Stage5JsonValue $document 'mapName' `
        'Lockstep-v2 retained qualification-data manifest'
    $manifestClosureSha256 = Get-Stage5JsonValue $document 'closureSha256' `
        'Lockstep-v2 retained qualification-data manifest'
    Assert-Stage5Condition ($manifestEvidenceKind -is [string] -and
        $manifestProducer -is [string] -and
        $manifestSourceCommit -is [string] -and
        $manifestMapName -is [string] -and
        $manifestClosureSha256 -is [string] -and
        $manifestClosureSha256 -cmatch '^[0-9A-F]{64}$') `
        'Lockstep-v2 qualification-data identity and closure fields must be JSON strings.'
    $lockstepQualificationSchemaVersion = Get-Stage5JsonValue $document `
        'schemaVersion' 'Lockstep-v2 retained qualification-data manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $lockstepQualificationSchemaVersion) -and
        $lockstepQualificationSchemaVersion -eq 2 -and
        $manifestEvidenceKind -ceq 'lockstep-v2-qualification-data' -and
        $manifestProducer -ceq 'genci-r2-trimmed-data' -and
        $manifestSourceCommit -ceq $ExpectedSourceCommit -and
        $productSet -is [Array] -and $productSet.Count -eq 2 -and
        $productSet[0] -is [string] -and $productSet[1] -is [string] -and
        $productSet[0] -ceq 'Generals' -and
        $productSet[1] -ceq 'ZeroHour' -and
        $manifestMapName -ceq $ExpectedMapName -and
        $manifestMapCrcs.Generals -eq $expectedMapCrcs.Generals -and
        $manifestMapCrcs.ZeroHour -eq $expectedMapCrcs.ZeroHour -and
        $manifestClosureSha256 -ceq $expectedClosureSha256) `
        'Lockstep-v2 qualification-data identity or map binding is stale or substituted.'

    $expectedArchives = @(
        [ordered]@{
            title = 'Generals'
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        },
        [ordered]@{
            title = 'ZeroHour'
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
    )
    $archiveSources = Get-Stage5JsonValue $document 'archiveSources' `
        'Lockstep-v2 retained qualification-data manifest'
    Assert-Stage5Condition ($archiveSources -is [Array] -and
        $archiveSources.Count -eq $expectedArchives.Count) `
        'Lockstep-v2 qualification-data archive provenance is incomplete.'
    for ($archiveIndex = 0; $archiveIndex -lt $expectedArchives.Count;
        ++$archiveIndex) {
        $archive = $archiveSources[$archiveIndex]
        $expectedArchive = $expectedArchives[$archiveIndex]
        Assert-Stage5JsonShape $archive @('title', 'object', 'sha256') `
            'Lockstep-v2 qualification-data archive source'
        $archiveTitle = Get-Stage5JsonValue $archive 'title' `
            'Lockstep-v2 qualification-data archive source'
        $archiveObject = Get-Stage5JsonValue $archive 'object' `
            'Lockstep-v2 qualification-data archive source'
        $archiveHash = Get-Stage5JsonValue $archive 'sha256' `
            'Lockstep-v2 qualification-data archive source'
        Assert-Stage5Condition (
            $archiveTitle -is [string] -and $archiveObject -is [string] -and
            $archiveHash -is [string] -and $archiveHash -cmatch '^[0-9A-F]{64}$' -and
            $archiveTitle -ceq $expectedArchive.title -and
            $archiveObject -ceq $expectedArchive.object -and
            $archiveHash -ceq $expectedArchive.sha256) `
            'Lockstep-v2 qualification-data archive source is unreviewed, reordered, or substituted.'
    }

    $requiredByTitle = [ordered]@{
        Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
        ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    $missingByTitle = @{
        Generals = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        ZeroHour = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
    }
    foreach ($title in $requiredByTitle.Keys) {
        foreach ($requiredPath in $requiredByTitle[$title]) {
            [void]$missingByTitle[$title].Add($requiredPath)
        }
    }
    $seenIdentities = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $files = Get-Stage5JsonValue $document 'files' `
        'Lockstep-v2 retained qualification-data manifest'
    Assert-Stage5Condition ($files -is [Array] -and $files.Count -ge 12) `
        'Lockstep-v2 qualification-data file coverage is incomplete.'
    $previousIdentity = $null
    foreach ($entry in $files) {
        Assert-Stage5JsonShape $entry @('title', 'path', 'sha256') `
            'Lockstep-v2 qualification-data file'
        $title = Get-Stage5JsonValue $entry 'title' `
            'Lockstep-v2 qualification-data file'
        $relative = Get-Stage5JsonValue $entry 'path' `
            'Lockstep-v2 qualification-data file'
        $hash = Get-Stage5JsonValue $entry 'sha256' `
            'Lockstep-v2 qualification-data file'
        Assert-Stage5Condition ($title -is [string] -and
            $relative -is [string] -and $hash -is [string] -and
            $hash -cmatch '^[0-9A-F]{64}$') `
            'Lockstep-v2 qualification-data file title, path, and hash must be JSON strings.'
        $segments = if ($relative -is [string]) {
            @($relative -split '/')
        }
        else { @() }
        $hasUnsafeSegment = @($segments | Where-Object {
            [string]::IsNullOrEmpty($_) -or $_ -ceq '.' -or $_ -ceq '..' -or
                $_.EndsWith('.') -or $_.EndsWith(' ')
        }).Count -ne 0
        $runtimePrefix = if ($title -ceq 'Generals') {
            'GeneralsRuntime/'
        }
        elseif ($title -ceq 'ZeroHour') { 'ZeroHourRuntime/' }
        else { '' }
        $titleRelative = if ($relative -is [string] -and
            $relative.StartsWith($runtimePrefix, [StringComparison]::Ordinal)) {
            $relative.Substring($runtimePrefix.Length)
        }
        else { '' }
        $isRootBig = $titleRelative.IndexOf('/') -lt 0 -and
            $titleRelative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $titleRelative.StartsWith('Data/',
            [StringComparison]::OrdinalIgnoreCase)
        $identity = "$title|$relative"
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($runtimePrefix) -and
            $relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*\z' -and
            $relative -cnotmatch '[\x00-\x1F\x7F]' -and
            $relative -cnotmatch '(^|/)\.\.?(/|$)' -and
            -not $hasUnsafeSegment -and
            -not [string]::IsNullOrWhiteSpace($titleRelative) -and
            ($isRootBig -or $isDataFile) -and
            ($null -eq $previousIdentity -or
                [StringComparer]::Ordinal.Compare($previousIdentity,
                    $identity) -lt 0) -and
            $seenIdentities.Add($identity)) `
            "Lockstep-v2 qualification-data file is unsafe, duplicated, or unsorted: $relative"
        [void]$missingByTitle[$title].Remove($titleRelative)
        $canonicalLines.Add(('{0}|{1}|{2}' -f $title, $relative, $hash)) |
            Out-Null
        $previousIdentity = $identity
    }
    foreach ($title in $requiredByTitle.Keys) {
        Assert-Stage5Condition ($missingByTitle[$title].Count -eq 0) `
            "Lockstep-v2 qualification data for $title omits a required file."
    }
    $canonicalText = ($canonicalLines.ToArray() -join "`n") + "`n"
    $computedClosureSha256 = Get-Stage5FinalAcceptanceSha256FromBytes `
        ([Text.Encoding]::UTF8.GetBytes($canonicalText))
    Assert-Stage5Condition ($computedClosureSha256 -ceq $manifestClosureSha256) `
        'Lockstep-v2 qualification-data file closure SHA-256 is stale or substituted.'
    Assert-Stage5Condition ([Int64]$expectedFileCount -eq $files.Count) `
        'Lockstep-v2 qualification-data file count is stale or substituted.'

    return [pscustomobject]@{
        path = $full
        manifestSha256 = $manifestSha256
        closureSha256 = $computedClosureSha256
        fileCount = $files.Count
    }
}

function Read-Stage5LockstepV2Evidence {
    param(
        [string]$Path,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$ArtifactPaths = $null,
        [string]$ArtifactRootDirectory = $null,
        [string]$ExpectedEvidenceSha256 = $null,
        [object]$EvidenceSnapshot = $null,
        [string]$ExpectedCohortNonce = $null,
        [string]$ExpectedCohortCreatedUtc = $null,
        [object]$ExpectedRuntimeClosure = $null
    )
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $EvidenceSnapshot) {
        Assert-Stage5Condition (Test-Path -LiteralPath $full -PathType Leaf) `
            "Lockstep-v2 multiplayer evidence was not found: $full"
    }
    Assert-Stage5Condition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Lockstep-v2 ExpectedSourceCommit must be an independently supplied lowercase 40-hex commit.'
    Assert-Stage5Condition ($ExpectedArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
        'Lockstep-v2 ExpectedArtifactSetSha256 must be an independently supplied SHA-256.'
    Assert-Stage5Condition ($ArtifactHashes -is [Collections.IDictionary] -and
        $ArtifactHashes.Contains('generals-executable') -and
        $ArtifactHashes.Contains('zerohour-executable')) `
        'Lockstep-v2 executable bindings are incomplete.'
    $documentSnapshot = if ($null -eq $EvidenceSnapshot) {
        Get-Stage5FinalAcceptanceFileSnapshot $full 'Lockstep-v2 multiplayer evidence'
    }
    else { $EvidenceSnapshot }
    if ($null -ne $EvidenceSnapshot) {
        Assert-Stage5Condition (-not [string]::IsNullOrWhiteSpace($ExpectedEvidenceSha256)) `
            'Lockstep-v2 multiplayer evidence caller-supplied snapshot must include its independently expected SHA-256.'
    }
    $snapshotPath = if ($null -ne $documentSnapshot -and
        $documentSnapshot.PSObject.Properties.Name -contains 'path') {
        $documentSnapshot.path
    }
    else { $null }
    Assert-Stage5Condition ($null -ne $documentSnapshot -and
        $snapshotPath -is [string] -and
        [IO.Path]::GetFullPath($snapshotPath) -ceq $full) `
        'Lockstep-v2 multiplayer evidence snapshot is bound to a different path.'
    $snapshotSha256 = if ($documentSnapshot.PSObject.Properties.Name -contains 'sha256') {
        $documentSnapshot.sha256
    }
    else { $null }
    Assert-Stage5Condition ($snapshotSha256 -is [string] -and
        $snapshotSha256 -cmatch '^[0-9A-F]{64}$') `
        'Lockstep-v2 multiplayer evidence snapshot SHA-256 must be a JSON string.'
    if ([string]::IsNullOrWhiteSpace($ExpectedEvidenceSha256)) {
        $ExpectedEvidenceSha256 = $snapshotSha256
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $documentSnapshot `
        $ExpectedEvidenceSha256 'Lockstep-v2 multiplayer evidence' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $documentSnapshot `
        'Lockstep-v2 multiplayer evidence'
    Assert-Stage5Condition ($document -is [Collections.IDictionary]) `
        'Lockstep-v2 multiplayer evidence must be a JSON object.'
    $isDiagnosticV1 = $false
    if ($document.Keys -contains 'schemaVersion' -and
        $document.Keys -contains 'evidenceKind' -and
        $document.Keys -contains 'producer' -and
        $document.Keys -contains 'validationMode') {
        $diagnosticEvidenceKind = $document['evidenceKind']
        $diagnosticProducer = $document['producer']
        $diagnosticValidationMode = $document['validationMode']
        $isDiagnosticV1 = (Test-Stage5JsonInteger $document['schemaVersion']) -and
            $document['schemaVersion'] -eq 1 -and
            $diagnosticEvidenceKind -is [string] -and
            $diagnosticProducer -is [string] -and
            $diagnosticValidationMode -is [string] -and
            $diagnosticEvidenceKind -ceq 'installed-net3-loopback' -and
            $diagnosticProducer -ceq 'installed-runtime-runner-v1' -and
            $diagnosticValidationMode -ceq 'scoped-net3-loopback-release-proof'
    }
    $boundary = "schemaVersion=2, evidenceKind='lockstep-v2-multiplayer', producer='installed-lockstep-v2', validationMode='installed-lockstep-v2-production'"
    if ($isDiagnosticV1) {
        throw "Mixed-worker multiplayer attachment is diagnostic NET3 v1 and cannot satisfy final Stage 5 acceptance. It is supplementary only. Required boundary: $boundary"
    }
    $names = @('schemaVersion', 'evidenceKind', 'status', 'producer',
        'validationMode', 'architecture', 'sourceCommit', 'artifactSetSha256',
        'recordedUtc', 'cohortNonce', 'runtimeClosure', 'qualificationData',
        'allowHeadlessDirectExecution', 'launcherEquivalence',
        'commonStopFrame', 'peerCount', 'networkRosterMask',
        'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount',
        'mapName', 'mapCrcs', 'seed',
        'negativeProbes',
        'v1Accepted', 'profileStrategy', 'registryViews',
        'environmentVariables', 'profileConcurrency',
        'titleSessionDisposition', 'sessions')
    Assert-Stage5JsonShape $document $names 'Lockstep-v2 multiplayer evidence'
    $documentEvidenceKind = Get-Stage5JsonValue $document 'evidenceKind' `
        'Lockstep-v2 multiplayer evidence'
    $documentStatus = Get-Stage5JsonValue $document 'status' `
        'Lockstep-v2 multiplayer evidence'
    $documentProducer = Get-Stage5JsonValue $document 'producer' `
        'Lockstep-v2 multiplayer evidence'
    $documentValidationMode = Get-Stage5JsonValue $document 'validationMode' `
        'Lockstep-v2 multiplayer evidence'
    $documentArchitecture = Get-Stage5JsonValue $document 'architecture' `
        'Lockstep-v2 multiplayer evidence'
    $documentSourceCommit = Get-Stage5JsonValue $document 'sourceCommit' `
        'Lockstep-v2 multiplayer evidence'
    $documentArtifactSetSha256 = Get-Stage5JsonValue $document `
        'artifactSetSha256' 'Lockstep-v2 multiplayer evidence'
    $documentRecordedUtc = Get-Stage5JsonValue $document 'recordedUtc' `
        'Lockstep-v2 multiplayer evidence'
    $documentCohortNonce = Get-Stage5JsonValue $document 'cohortNonce' `
        'Lockstep-v2 multiplayer evidence'
    Assert-Stage5Condition ($documentEvidenceKind -is [string] -and
        $documentStatus -is [string] -and $documentProducer -is [string] -and
        $documentValidationMode -is [string] -and
        $documentArchitecture -is [string] -and
        $documentSourceCommit -is [string] -and
        $documentArtifactSetSha256 -is [string] -and
        $documentArtifactSetSha256 -cmatch '^[0-9A-Fa-f]{64}$' -and
        $documentRecordedUtc -is [string] -and $documentCohortNonce -is [string]) `
        'Lockstep-v2 multiplayer evidence identity and provenance fields must be JSON strings.'
    foreach ($field in @('schemaVersion', 'commonStopFrame', 'peerCount',
        'networkRosterMask', 'simulationRosterMask', 'aiRosterMask',
        'aiPlayerCount', 'seed')) {
        Assert-Stage5Condition (Test-Stage5JsonInteger $document[$field]) `
            "Lockstep-v2 multiplayer evidence field '$field' must be an integer."
        $maximum = if ($field -ceq 'seed') {
            [UInt64][Int32]::MaxValue
        }
        else { [UInt64][Int64]::MaxValue }
        Assert-Stage5Condition ($document[$field] -ge 0 -and
            $document[$field] -le $maximum) `
            "Lockstep-v2 multiplayer evidence field '$field' is outside its integer range."
    }
    Assert-Stage5Condition ((Test-Stage5JsonInteger $document['schemaVersion']) -and
        $document['schemaVersion'] -eq 2 -and
        $documentEvidenceKind -ceq 'lockstep-v2-multiplayer' -and
        $documentStatus -ceq 'passed' -and
        $documentProducer -ceq 'installed-lockstep-v2' -and
        $documentValidationMode -ceq 'installed-lockstep-v2-production' -and
        $documentArchitecture -ceq 'x64') `
        "Lockstep-v2 multiplayer evidence has an invalid schema/producer/mode boundary; required boundary: $boundary"
    Assert-Stage5Condition ($documentSourceCommit -ceq $ExpectedSourceCommit -and
        $documentSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Lockstep-v2 multiplayer evidence sourceCommit is stale or substituted.'
    Assert-Stage5Condition ($documentArtifactSetSha256 -match '^[0-9A-Fa-f]{64}$' -and
        $documentArtifactSetSha256.ToUpperInvariant() -ceq
            $ExpectedArtifactSetSha256.ToUpperInvariant()) `
        'Lockstep-v2 multiplayer evidence artifactSetSha256 does not match the independently hashed artifact set.'
    $documentMapName = Get-Stage5JsonValue $document 'mapName' `
        'Lockstep-v2 multiplayer evidence'
    Assert-Stage5Condition ($documentMapName -is [string]) `
        'Lockstep-v2 multiplayer evidence mapName must be a JSON string.'
    $mapCrcs = Assert-Stage5LockstepMapCrcs $document['mapCrcs'] `
        'Lockstep-v2 multiplayer evidence map CRCs'
    $qualificationDataPath = Resolve-Stage5FinalAcceptanceFile `
        (Split-Path -Parent $full) 'QualificationData.json' `
        'Lockstep-v2 retained qualification-data manifest'
    $qualificationData = Read-Stage5LockstepQualificationDataEvidence `
        -Path $qualificationDataPath -Binding $document['qualificationData'] `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedMapName $documentMapName `
        -ExpectedMapCrcs $mapCrcs
    Assert-Stage5Condition ([Int64]$document['commonStopFrame'] -eq 4096 -and
        [Int64]$document['peerCount'] -eq 2 -and
        [Int64]$document['networkRosterMask'] -eq 3 -and
        [Int64]$document['simulationRosterMask'] -eq 63 -and
        [Int64]$document['aiRosterMask'] -eq 60 -and
        [Int64]$document['aiPlayerCount'] -eq 4 -and
        [Int64]$document['seed'] -gt 0 -and
        $documentMapName -is [string] -and
        (Test-Stage5LockstepSafeMapName $documentMapName) -and
        $document['v1Accepted'] -is [bool] -and -not [bool]$document['v1Accepted']) `
        'Lockstep-v2 multiplayer evidence does not prove the bounded x64 4096-frame v2 contract.'
    Assert-Stage5Condition ($documentRecordedUtc -is [string]) `
        'Lockstep-v2 multiplayer evidence recordedUtc must be a JSON string.'
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ([DateTimeOffset]::TryParse($documentRecordedUtc, [ref]$recorded)) `
        'Lockstep-v2 multiplayer evidence recordedUtc is not a valid timestamp.'
    $cohortNonce = Assert-Stage5CanonicalUuid $documentCohortNonce `
        'Lockstep-v2 multiplayer evidence cohortNonce'
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortNonce)) {
        Assert-Stage5CanonicalUuid $ExpectedCohortNonce `
            'Lockstep-v2 expected cohortNonce' | Out-Null
        Assert-Stage5Condition ($cohortNonce -ceq $ExpectedCohortNonce) `
            'Lockstep-v2 multiplayer evidence cohortNonce is stale or detached.'
    }
    $runtimeClosure = $document['runtimeClosure']
    if ($null -ne $ExpectedRuntimeClosure) {
        [void](Assert-Stage5RuntimeClosureBinding $runtimeClosure `
            $ExpectedRuntimeClosure 'Lockstep-v2 multiplayer evidence')
    }
    else {
        Assert-Stage5JsonShape $runtimeClosure `
            @('dependencyManifestSha256', 'closureSha256') `
            'Lockstep-v2 multiplayer evidence runtime closure'
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedCohortCreatedUtc)) {
        [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ([DateTimeOffset]::TryParse($ExpectedCohortCreatedUtc,
            [ref]$cohortCreated) -and $recorded -ge $cohortCreated) `
            'Lockstep-v2 multiplayer evidence recordedUtc predates the execution cohort.'
    }
    Assert-Stage5Condition ($document['allowHeadlessDirectExecution'] -is [bool] -and
        [bool]$document['allowHeadlessDirectExecution']) `
        'Lockstep-v2 multiplayer evidence did not record the reviewed direct-execution opt-in.'
    Assert-Stage5Condition ($document['profileStrategy'] -is [string] -and
        $document['profileStrategy'] -ceq 'process-local-validation-profile-root' -and
        $document['profileConcurrency'] -is [string] -and
        $document['profileConcurrency'] -ceq 'shared-title-profile-read-only') `
        'Lockstep-v2 multiplayer evidence did not retain the reviewed profile-isolation strategy.'
    Assert-Stage5Condition ($document['titleSessionDisposition'] -is [string] -and
        $document['titleSessionDisposition'] -ceq
            'removed-after-peer-exit-before-evidence-persist') `
        'Lockstep-v2 multiplayer evidence did not prove title-session cleanup before evidence persistence.'
    $expectedRegistryViews = @('Registry32', 'Registry64')
    Assert-Stage5Condition ($document['registryViews'] -is [Array] -and
        (@($document['registryViews'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedRegistryViews -join '|')) `
        'Lockstep-v2 multiplayer evidence does not cover both reviewed registry views.'
    $expectedEnvironmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
        'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
        'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
        'RTS_STAGE5_VALIDATION_CACHE_ROOT',
        'RTS_STAGE5_VALIDATION_LOG_ROOT',
        'RTS_STAGE5_VALIDATION_DUMP_ROOT')
    Assert-Stage5Condition ($document['environmentVariables'] -is [Array] -and
        (@($document['environmentVariables'] | ForEach-Object { [string]$_ }) -join '|') -ceq
            ($expectedEnvironmentVariables -join '|')) `
        'Lockstep-v2 multiplayer evidence does not retain the reviewed peer environment boundary.'
    $negativeProbes = $document['negativeProbes']
    Assert-Stage5JsonShape $negativeProbes @('crossEpoch', 'contentMismatch') `
        'Lockstep-v2 native negative-probe collection'
    Assert-Stage5Condition ($negativeProbes['crossEpoch'] -is [Array] -and
        $negativeProbes['crossEpoch'].Count -eq 2 -and
        $negativeProbes['contentMismatch'] -is [Array] -and
        $negativeProbes['contentMismatch'].Count -eq 2) `
        'Lockstep-v2 native evidence must contain one observed negative proof per title and mode.'
    $aggregateLauncherContracts = $document['launcherEquivalence']
    Assert-Stage5JsonShape $aggregateLauncherContracts @('Generals', 'ZeroHour') `
        'Lockstep-v2 launcher-equivalence aggregate'
    foreach ($launcherTitle in @('Generals', 'ZeroHour')) {
        [void](Assert-Stage5LockstepLauncherContract $aggregateLauncherContracts[$launcherTitle] `
            $launcherTitle $ArtifactHashes "Lockstep-v2 $launcherTitle launcher-equivalence" `
            $ArtifactPaths $ArtifactRootDirectory
        )
    }

    $sessions = $document['sessions']
    Assert-Stage5Condition ($sessions -is [Array] -and $sessions.Count -eq 2) `
        'Lockstep-v2 multiplayer evidence must contain exactly the Generals and ZeroHour sessions.'
    $sessionTitles = @('Generals', 'ZeroHour')
    $seenSessionNonces = @{}
    $seenProcessIds = @{}
    $seenPorts = @{}
    $seenRunNonces = @{}
    $sessionReports = New-Object 'Collections.Generic.List[object]'
    for ($sessionIndex = 0; $sessionIndex -lt $sessions.Count; ++$sessionIndex) {
        $session = $sessions[$sessionIndex]
        $title = $sessionTitles[$sessionIndex]
        $sessionMapCrc = [UInt32]$mapCrcs[$title]
        $sessionContext = "Lockstep-v2 '$title' session"
        $sessionNames = @('title', 'peerCount', 'networkRosterMask',
            'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount', 'mapCrc',
            'ports', 'sessionNonce',
            'launcherEquivalence', 'titleSessionProfile',
            'registryEquivalence', 'workerProfiles', 'effectiveWorkerCounts',
            'mixedWorkerProof', 'comparableProjectionSha256', 'peers',
            'profileReadOnlyVerified', 'profileFilesAfterRun')
        Assert-Stage5JsonShape $session $sessionNames $sessionContext
        foreach ($field in @('peerCount', 'networkRosterMask',
            'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount')) {
            Assert-Stage5Condition (Test-Stage5JsonInteger $session[$field]) `
                "$sessionContext field '$field' must be an integer."
            Assert-Stage5Condition ($session[$field] -ge 0 -and
                $session[$field] -le [UInt64][Int64]::MaxValue) `
                "$sessionContext field '$field' is outside the Int64 range."
        }
        Assert-Stage5Condition (Test-Stage5JsonInteger $session['mapCrc']) `
            "$sessionContext field 'mapCrc' must be an integer."
        Assert-Stage5Condition ($session['mapCrc'] -ge 0 -and
            $session['mapCrc'] -le [UInt64][UInt32]::MaxValue) `
            "$sessionContext field 'mapCrc' is outside the UInt32 range."
        [void](Assert-Stage5LockstepLauncherContract $session['launcherEquivalence'] `
            $title $ArtifactHashes "$sessionContext launcher-equivalence" `
            $ArtifactPaths $ArtifactRootDirectory)
        Assert-Stage5Condition (
            ($session['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8) -ceq
            ($aggregateLauncherContracts[$title] | ConvertTo-Json -Compress -Depth 8)) `
            "$sessionContext launcher-equivalence is not bound to the aggregate contract."
        Assert-Stage5Condition ($session['title'] -is [string] -and
            $session['title'] -ceq $title -and
            (Test-Stage5JsonInteger $session['peerCount']) -and
            [Int64]$session['peerCount'] -eq [Int64]$document['peerCount'] -and
            (Test-Stage5JsonInteger $session['mapCrc']) -and
            [UInt64]$session['mapCrc'] -eq [UInt64]$sessionMapCrc) `
            "$sessionContext title, peer count, or title-specific map CRC is substituted."
        Assert-Stage5Condition (
            $session['networkRosterMask'] -eq $document['networkRosterMask'] -and
            $session['simulationRosterMask'] -eq $document['simulationRosterMask'] -and
            $session['aiRosterMask'] -eq $document['aiRosterMask'] -and
            $session['aiPlayerCount'] -eq $document['aiPlayerCount']) `
            "$sessionContext roster masks or AI player count are stale or substituted."
        Assert-Stage5Condition ($session['sessionNonce'] -is [string] -and
            $session['sessionNonce'] -cmatch '^[0-9A-F]{32}$' -and
            -not $seenSessionNonces.ContainsKey($session['sessionNonce'])) `
            "$sessionContext has a duplicate or noncanonical session nonce."
        $seenSessionNonces[$session['sessionNonce']] = $true
        Assert-Stage5Condition ($session['ports'] -is [Array] -and
            $session['ports'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list exactly one port per peer."
        $sessionPorts = @()
        for ($portIndex = 0; $portIndex -lt $session['ports'].Count; ++$portIndex) {
            $port = $session['ports'][$portIndex]
            Assert-Stage5Condition ((Test-Stage5JsonInteger $port) -and
                [Int64]$port -ge 1024 -and [Int64]$port -le 65535) `
                "$sessionContext has an invalid UDP port at peer $portIndex."
            $portKey = [string][Int64]$port
            Assert-Stage5Condition (-not $seenPorts.ContainsKey($portKey)) `
                "$sessionContext reuses UDP port $port."
            $seenPorts[$portKey] = $true
            $sessionPorts += [int]$port
        }
        Assert-Stage5Condition ($session['workerProfiles'] -is [Array] -and
            $session['workerProfiles'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list one executable worker profile per peer."
        Assert-Stage5Condition ($session['effectiveWorkerCounts'] -is [Array] -and
            $session['effectiveWorkerCounts'].Count -eq [Int64]$document['peerCount']) `
            "$sessionContext does not list one effective worker count per peer."
        foreach ($effectiveWorkerCount in @($session['effectiveWorkerCounts'])) {
            Assert-Stage5Condition (Test-Stage5JsonInteger $effectiveWorkerCount) `
                "$sessionContext effective worker counts must contain only integers."
            Assert-Stage5Condition ($effectiveWorkerCount -ge 0 -and
                $effectiveWorkerCount -le [UInt64][Int64]::MaxValue) `
                "$sessionContext effective worker count is outside the Int64 range."
        }
        Assert-Stage5Condition ($session['mixedWorkerProof'] -is [bool] -and
            [bool]$session['mixedWorkerProof']) `
            "$sessionContext does not prove mixed executable worker profiles."
        for ($profileIndex = 0; $profileIndex -lt $session['workerProfiles'].Count; ++$profileIndex) {
            $profileContext = "$sessionContext worker profile $profileIndex"
            $profile = $session['workerProfiles'][$profileIndex]
            Assert-Stage5JsonShape $profile @('profile', 'requestedWorkers',
                'workerPolicy', 'overrideArguments') $profileContext
            $expectedProfile = if (($profileIndex % 2) -eq 0) {
                [ordered]@{
                    profile = 'explicit-two-workers'; requestedWorkers = '2'
                    workerPolicy = 'all'; overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
                }
            }
            else {
                [ordered]@{
                    profile = 'automatic-workers'; requestedWorkers = 'auto'
                    workerPolicy = 'auto'; overrideArguments = @('-workerPolicy', 'auto')
                }
            }
            $profileName = $profile['profile']
            $profileRequestedWorkers = $profile['requestedWorkers']
            $profileWorkerPolicy = $profile['workerPolicy']
            Assert-Stage5Condition ($profileName -is [string] -and
                $profileRequestedWorkers -is [string] -and
                $profileWorkerPolicy -is [string] -and
                $profileName -ceq $expectedProfile.profile -and
                $profileRequestedWorkers -ceq $expectedProfile.requestedWorkers -and
                $profileWorkerPolicy -ceq $expectedProfile.workerPolicy -and
                $profile['overrideArguments'] -is [Array] -and
                (@($profile['overrideArguments'] | ForEach-Object { [string]$_ }) -join '|') -ceq
                    ($expectedProfile.overrideArguments -join '|')) `
                "$profileContext is stale, homogeneous, or substituted."
            Assert-Stage5Condition (Test-Stage5JsonInteger $session['effectiveWorkerCounts'][$profileIndex]) `
                "$sessionContext effective worker count $profileIndex is not an integer."
        }
        $sessionDirectory = Join-Path (Split-Path -Parent $full) $title
        [void](Assert-Stage5LockstepTitleSessionContract `
            $session['titleSessionProfile'] $title $sessionDirectory `
            $session['launcherEquivalence'] "$sessionContext title-session profile")
        [void](Assert-Stage5LockstepRegistryEquivalence `
            $session['registryEquivalence'] $session['titleSessionProfile'] `
            "$sessionContext registry equivalence")
        Assert-Stage5Condition ($session['profileReadOnlyVerified'] -is [bool] -and
            [bool]$session['profileReadOnlyVerified'] -and
            $session['profileFilesAfterRun'] -is [Array] -and
            $session['profileFilesAfterRun'].Count -eq 0) `
            "$sessionContext did not prove the shared title profile remained read-only."
        Assert-Stage5Condition ($session['comparableProjectionSha256'] -is [string] -and
            $session['comparableProjectionSha256'] -cmatch '^[0-9A-F]{64}$') `
            "$sessionContext has no canonical cross-peer projection hash."
        $peers = $session['peers']
        Assert-Stage5Condition ($peers -is [Array] -and
            $peers.Count -eq [Int64]$document['peerCount']) `
            "$sessionContext must contain every peer contribution."
        $referenceProjection = $null
        $peerReports = New-Object 'Collections.Generic.List[object]'
        for ($peerIndex = 0; $peerIndex -lt $peers.Count; ++$peerIndex) {
            $peer = $peers[$peerIndex]
            $peerContext = "$sessionContext peer $peerIndex"
            $peerNames = @('schemaVersion', 'producer', 'validationMode', 'title',
                'processId', 'peer', 'peerCount', 'networkRosterMask',
                'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount', 'port',
                'runNonce', 'sessionNonce',
                'executableSha256', 'sourceCommit', 'launcherEquivalence',
                'launcherPath', 'launcherSha256', 'launcherConfigPath',
                'launcherConfigSha256', 'directExecutionOptIn', 'workingDirectory',
                'commandLine', 'arguments', 'launcherDefaultArguments',
                'directArguments', 'workerOverride', 'stdoutProof',
                'receiptWorkerTelemetry', 'requestedWorkers', 'workerPolicy',
                'effectiveWorkers', 'titleSessionProfile', 'registryEquivalence',
                'environmentEquivalence', 'receiptPath', 'receiptSha256', 'stdoutSha256',
                'stderrSha256', 'exitCode', 'finalFrame', 'finalCRC',
                'comparableProjectionSha256', 'lockstepV2Receipt',
                'v1ReceiptAccepted')
            Assert-Stage5JsonShape $peer $peerNames $peerContext
            foreach ($field in @('schemaVersion', 'processId', 'peer', 'peerCount',
                'networkRosterMask', 'simulationRosterMask', 'aiRosterMask',
                'aiPlayerCount', 'port', 'exitCode', 'finalFrame', 'finalCRC',
                'effectiveWorkers')) {
                Assert-Stage5Condition (Test-Stage5JsonInteger $peer[$field]) `
                    "$peerContext field '$field' must be an integer."
                Assert-Stage5Condition ($peer[$field] -ge 0 -and
                    $peer[$field] -le [UInt64][Int64]::MaxValue) `
                    "$peerContext field '$field' is outside the Int64 range."
            }
            Assert-Stage5Condition ($peer['finalCRC'] -le [UInt64][UInt32]::MaxValue) `
                "$peerContext field 'finalCRC' is outside the UInt32 range."
            $peerProducer = $peer['producer']
            $peerValidationMode = $peer['validationMode']
            $peerTitle = $peer['title']
            $peerSourceCommit = $peer['sourceCommit']
            $peerExecutableSha256 = $peer['executableSha256']
            $peerRunNonce = $peer['runNonce']
            $peerSessionNonce = $peer['sessionNonce']
            $peerReceiptPath = $peer['receiptPath']
            $peerReceiptSha256 = $peer['receiptSha256']
            $peerStdoutSha256 = $peer['stdoutSha256']
            $peerStderrSha256 = $peer['stderrSha256']
            Assert-Stage5Condition ($peerProducer -is [string] -and
                $peerValidationMode -is [string] -and $peerTitle -is [string] -and
                $peerSourceCommit -is [string] -and
                $peerExecutableSha256 -is [string] -and
                $peerExecutableSha256 -cmatch '^[0-9A-F]{64}$' -and
                $peerRunNonce -is [string] -and
                $peerSessionNonce -is [string] -and
                $peerReceiptPath -is [string] -and
                $peerReceiptSha256 -is [string] -and
                $peerReceiptSha256 -cmatch '^[0-9A-F]{64}$' -and
                $peerStdoutSha256 -is [string] -and
                $peerStdoutSha256 -cmatch '^[0-9A-F]{64}$' -and
                $peerStderrSha256 -is [string] -and
                $peerStderrSha256 -cmatch '^[0-9A-F]{64}$') `
                "$peerContext string identity and hash fields must be JSON strings."
            $expectedExecutable = [string]$ArtifactHashes[($(if ($title -ceq 'Generals') {
                'generals-executable'
            } else { 'zerohour-executable' }))].ToUpperInvariant()
            Assert-Stage5Condition ((Test-Stage5JsonInteger $peer['schemaVersion']) -and
                $peer['schemaVersion'] -eq 2 -and
                $peerProducer -ceq 'installed-lockstep-v2' -and
                $peerValidationMode -ceq 'installed-lockstep-v2-production' -and
                $peerTitle -ceq $title -and [Int64]$peer['peer'] -eq $peerIndex -and
                [Int64]$peer['peerCount'] -eq [Int64]$document['peerCount'] -and
                [Int64]$peer['networkRosterMask'] -eq [Int64]$document['networkRosterMask'] -and
                [Int64]$peer['simulationRosterMask'] -eq [Int64]$document['simulationRosterMask'] -and
                [Int64]$peer['aiRosterMask'] -eq [Int64]$document['aiRosterMask'] -and
                [Int64]$peer['aiPlayerCount'] -eq [Int64]$document['aiPlayerCount'] -and
                [Int64]$peer['port'] -eq $sessionPorts[$peerIndex] -and
                [Int64]$peer['processId'] -gt 0 -and
                [Int64]$peer['exitCode'] -eq 0 -and [Int64]$peer['finalFrame'] -eq 4096 -and
                [Int64]$peer['finalCRC'] -gt 0 -and
                $peerSourceCommit -ceq $ExpectedSourceCommit -and
                $peerExecutableSha256 -ceq $expectedExecutable -and
                $peer['lockstepV2Receipt'] -is [bool] -and [bool]$peer['lockstepV2Receipt'] -and
                $peer['v1ReceiptAccepted'] -is [bool] -and -not [bool]$peer['v1ReceiptAccepted']) `
                "$peerContext has a stale, substituted, or non-clean process identity."
            [void](Assert-Stage5LockstepLauncherContract $peer['launcherEquivalence'] `
                $title $ArtifactHashes "$peerContext launcher-equivalence" `
                $ArtifactPaths $ArtifactRootDirectory)
            Assert-Stage5Condition (
                ($peer['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8) -ceq
                ($session['launcherEquivalence'] | ConvertTo-Json -Compress -Depth 8)) `
                "$peerContext launcher-equivalence is substituted from another title."
            Assert-Stage5Condition (
                ($peer['titleSessionProfile'] | ConvertTo-Json -Compress -Depth 12) -ceq
                ($session['titleSessionProfile'] | ConvertTo-Json -Compress -Depth 12)) `
                "$peerContext title-session profile is substituted from another session."
            [void](Assert-Stage5LockstepRegistryEquivalence `
                $peer['registryEquivalence'] $session['titleSessionProfile'] `
                "$peerContext registry equivalence")
            [void](Assert-Stage5LockstepEnvironmentEquivalence `
                $peer['environmentEquivalence'] $session['titleSessionProfile'] `
                $peerIndex "$peerContext environment equivalence")
            [void](Assert-Stage5LockstepPeerLaunchBinding $peer `
                $session['launcherEquivalence'] $title $peerIndex `
                ([int]$document['peerCount']) $sessionPorts `
                $peerRunNonce $peerSessionNonce `
                $documentMapName $sessionMapCrc `
                ([int]$document['seed']) $sessionDirectory $peerContext)
            $workerEvidence = Assert-Stage5LockstepWorkerEvidence $peer $peerIndex $peerContext
            Assert-Stage5Condition ($workerEvidence.expectedProfile.profile -ceq
                $session['workerProfiles'][$peerIndex]['profile'] -and
                $workerEvidence.expectedProfile.requestedWorkers -ceq
                    $session['workerProfiles'][$peerIndex]['requestedWorkers'] -and
                $workerEvidence.expectedProfile.workerPolicy -ceq
                    $session['workerProfiles'][$peerIndex]['workerPolicy'] -and
                (@($workerEvidence.expectedProfile.overrideArguments | ForEach-Object { [string]$_ }) -join '|') -ceq
                    (@($session['workerProfiles'][$peerIndex]['overrideArguments'] |
                        ForEach-Object { [string]$_ }) -join '|')) `
                "$peerContext worker override is not bound to its session profile."
            Assert-Stage5Condition ([Int64]$peer['effectiveWorkers'] -eq
                [Int64]$workerEvidence.effectiveWorkers -and
                [Int64]$session['effectiveWorkerCounts'][$peerIndex] -eq
                    [Int64]$workerEvidence.effectiveWorkers) `
                "$peerContext effective worker count is stale or substituted."
            $processKey = [string][Int64]$peer['processId']
            Assert-Stage5Condition (-not $seenProcessIds.ContainsKey($processKey)) `
                "$peerContext reuses a process identity."
            $seenProcessIds[$processKey] = $true
            Assert-Stage5Condition ($peerRunNonce -cmatch '^[0-9A-F]{32}$' -and
                -not $seenRunNonces.ContainsKey($peerRunNonce)) `
                "$peerContext has a duplicate or noncanonical run nonce."
            $seenRunNonces[$peerRunNonce] = $true
            Assert-Stage5Condition ($peerSessionNonce -ceq $session['sessionNonce']) `
                "$peerContext is bound to a different session nonce."
            $stdoutProof = $peer['stdoutProof']
            Assert-Stage5JsonShape $stdoutProof @('executableOrigin', 'peer',
                'pid', 'frameLimit', 'activeMarker', 'passMarker', 'finalCrc') `
                "$peerContext stdout proof"
            $stdoutProofActiveMarker = $stdoutProof['activeMarker']
            $stdoutProofPassMarker = $stdoutProof['passMarker']
            $stdoutProofFinalCrc = $stdoutProof['finalCrc']
            Assert-Stage5Condition ($stdoutProofActiveMarker -is [string] -and
                $stdoutProofPassMarker -is [string] -and
                $stdoutProofFinalCrc -is [string] -and
                $stdoutProofFinalCrc -cmatch '^[0-9A-F]{8}$') `
                "$peerContext stdout proof identity fields must be JSON strings."
            foreach ($hashField in @('receiptSha256', 'stdoutSha256', 'stderrSha256',
                'comparableProjectionSha256')) {
                Assert-Stage5Condition ($peer[$hashField] -is [string] -and
                    $peer[$hashField] -cmatch '^[0-9A-F]{64}$') `
                    "$peerContext field '$hashField' is not a canonical SHA-256."
            }
            $receiptLeaf = "lockstep-v2-$title-peer-$peerIndex.receipt"
            Assert-Stage5Condition ($peerReceiptPath -ceq $receiptLeaf) `
                "$peerContext receipt path is substituted."
            $receiptPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $receiptLeaf "$peerContext receipt"
            $stdoutLeaf = "peer-$peerIndex.stdout.log"
            $stderrLeaf = "peer-$peerIndex.stderr.log"
            $stdoutPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $stdoutLeaf "$peerContext stdout"
            $stderrPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory `
                $stderrLeaf "$peerContext stderr"
            $receiptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $receiptPath `
                "$peerContext receipt"
            Assert-Stage5FinalAcceptanceSnapshotSha256 $receiptSnapshot `
                $peerReceiptSha256 `
                "$peerContext receipt SHA-256 binding does not match the producer file" | Out-Null
            $stdoutSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $stdoutPath `
                "$peerContext stdout" -EvidenceKind RawLog
            Assert-Stage5FinalAcceptanceSnapshotSha256 $stdoutSnapshot `
                $peerStdoutSha256 `
                "$peerContext stdout SHA-256 binding does not match the producer file" | Out-Null
            $stderrSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $stderrPath `
                "$peerContext stderr" -EvidenceKind RawLog
            Assert-Stage5FinalAcceptanceSnapshotSha256 $stderrSnapshot `
                $peerStderrSha256 `
                "$peerContext stderr SHA-256 binding does not match the producer file" | Out-Null
            $stdoutText = [Text.Encoding]::UTF8.GetString([byte[]]$stdoutSnapshot.bytes)
            $stdoutLines = @($stdoutText -split "`n" | ForEach-Object {
                $_.TrimEnd("`r")
            })
            $activeMarkerLines = @($stdoutLines | Where-Object {
                $_.StartsWith('LOCKSTEP_V2_VALIDATION_ACTIVE ',
                    [StringComparison]::Ordinal)
            })
            $passMarkerLines = @($stdoutLines | Where-Object {
                $_.StartsWith('LOCKSTEP_V2_VALIDATION_PASS ',
                    [StringComparison]::Ordinal)
            })
            Assert-Stage5Condition ($activeMarkerLines.Count -eq 1 -and
                $passMarkerLines.Count -eq 1 -and
                $stdoutText -notmatch 'NET3_VALIDATION_PEER_PASS') `
                "$peerContext stdout is not an exclusive bounded lockstep-v2 clean exit."
            $activeMarkerMatch = [regex]::Match($activeMarkerLines[0],
                '^LOCKSTEP_V2_VALIDATION_ACTIVE peer=(?<peer>[0-9]+) frame_limit=(?<frame>[0-9]+)$')
            $passMarkerMatch = [regex]::Match($passMarkerLines[0],
                '^LOCKSTEP_V2_VALIDATION_PASS peer=(?<peer>[0-9]+) pid=(?<pid>[0-9]+) frame=(?<frame>[0-9]+) crc=(?<crc>[0-9A-Fa-f]{8})$')
            $expectedStdoutCrc = '{0:X8}' -f ([UInt32]$peer['finalCRC'])
            Assert-Stage5Condition ($activeMarkerMatch.Success -and
                [Int64]$activeMarkerMatch.Groups['peer'].Value -eq $peerIndex -and
                [Int64]$activeMarkerMatch.Groups['frame'].Value -eq 4096 -and
                $passMarkerMatch.Success -and
                [Int64]$passMarkerMatch.Groups['peer'].Value -eq $peerIndex -and
                [Int64]$passMarkerMatch.Groups['pid'].Value -eq [Int64]$peer['processId'] -and
                [Int64]$passMarkerMatch.Groups['frame'].Value -eq 4096 -and
                $passMarkerMatch.Groups['crc'].Value.ToUpperInvariant() -ceq $expectedStdoutCrc -and
                [Int64]$stdoutProof['pid'] -eq [Int64]$peer['processId'] -and
                $stdoutProofActiveMarker -ceq $activeMarkerLines[0] -and
                $stdoutProofPassMarker -ceq $passMarkerLines[0] -and
                $stdoutProofFinalCrc -ceq $expectedStdoutCrc) `
                "$peerContext stdout proof is stale or detached from the clean executable exit."
            $receipt = Read-Stage5LockstepV2Receipt $receiptPath $peerIndex `
                ([int]$document['peerCount']) $sessionMapCrc `
                $peerRunNonce $peerSessionNonce `
                $peerExecutableSha256 $ExpectedSourceCommit `
                $receiptSnapshot
            for ($kernel = 0; $kernel -lt 6; ++$kernel) {
                $receiptMask = ConvertTo-Stage5LockstepReceiptUInt64 `
                    $receipt.parsed.pairs["kernel_${kernel}_physical_worker_mask"] `
                    "kernel_${kernel}_physical_worker_mask"
                $receiptDistinct = ConvertTo-Stage5LockstepReceiptUInt32 `
                    $receipt.parsed.pairs["kernel_${kernel}_distinct_physical_workers"] `
                    "kernel_${kernel}_distinct_physical_workers"
                Assert-Stage5Condition ([UInt64]$peer['receiptWorkerTelemetry']['physicalWorkerMasks'][$kernel] -eq
                    $receiptMask -and
                    [Int64]$peer['receiptWorkerTelemetry']['distinctPhysicalWorkers'][$kernel] -eq
                        [Int64]$receiptDistinct) `
                    "$peerContext worker telemetry is detached from the executable receipt kernel $kernel."
            }
            Assert-Stage5Condition ([Int64]$peer['finalFrame'] -eq $receipt.finalFrame -and
                [Int64]$peer['finalCRC'] -eq $receipt.finalCRC -and
            $peer['comparableProjectionSha256'] -ceq
                    $receipt.projectionSha256) `
                "$peerContext aggregate fields do not bind the canonical receipt projection."
            if ($null -eq $referenceProjection) {
                $referenceProjection = $receipt.projectionSha256
            }
            else {
                Assert-Stage5Condition ($referenceProjection -ceq $receipt.projectionSha256) `
                    "$sessionContext peers disagree on CRC, checkpoint, or command digests."
            }
            $rawLeaf = "peer-$peerIndex.raw.json"
            $rawPath = Resolve-Stage5FinalAcceptanceFile $sessionDirectory $rawLeaf `
                "$peerContext raw receipt index"
            $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
                "$peerContext raw receipt index" -EvidenceKind RawLog
            $raw = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $rawSnapshot `
                "$peerContext raw receipt index"
            Assert-Stage5JsonShape $raw $peerNames "$peerContext raw receipt index"
            foreach ($field in $peerNames) {
                $aggregateValue = $peer[$field]
                $rawValue = $raw[$field]
                $aggregateJson = $aggregateValue | ConvertTo-Json -Compress -Depth 20
                $rawJson = $rawValue | ConvertTo-Json -Compress -Depth 20
                Assert-Stage5Condition ($aggregateJson -ceq $rawJson) `
                    "$peerContext raw receipt index field '$field' is substituted."
            }
            $peerReports.Add([pscustomobject]@{
                peer = $peerIndex; processId = [int]$peer['processId']
                port = [int]$peer['port']; runNonce = $peerRunNonce
                receiptSha256 = $peerReceiptSha256
                stdoutSha256 = $peerStdoutSha256
                stderrSha256 = $peerStderrSha256
                projectionSha256 = $receipt.projectionSha256
            }) | Out-Null
        }
        Assert-Stage5Condition ($referenceProjection -ceq
            $session['comparableProjectionSha256']) `
            "$sessionContext comparableProjectionSha256 is stale or substituted."
        $sessionReports.Add([pscustomobject]@{
            title = $title; peerCount = [int]$document['peerCount']
            mapCrc = $sessionMapCrc
            sessionNonce = $session['sessionNonce']
            comparableProjectionSha256 = [string]$referenceProjection
            peers = $peerReports.ToArray()
        }) | Out-Null
    }
    $validatedNegativeCrossEpoch = New-Object 'Collections.Generic.List[object]'
    $validatedNegativeContentMismatch = New-Object 'Collections.Generic.List[object]'
    $negativeRunNonces = @{}
    $negativeSessionNonces = @{}
    $negativePaths = @{}
    foreach ($negativeSpec in @(
        @('crossEpoch', 'negative-cross-epoch'),
        @('contentMismatch', 'negative-content-mismatch')
    )) {
        $collectionName = $negativeSpec[0]
        $mode = $negativeSpec[1]
        $entries = @($negativeProbes[$collectionName] | Where-Object {
            $_.mode -is [string] -and $_.mode -ceq $mode
        })
        Assert-Stage5Condition ($entries.Count -eq 2) `
            "Lockstep-v2 native negative-probe collection '$collectionName' must contain one entry per title."
        foreach ($titleIndex in 0..1) {
            $title = if ($titleIndex -eq 0) { 'Generals' } else { 'ZeroHour' }
            $titleEntries = @($entries | Where-Object {
                $_.title -is [string] -and $_.title -ceq $title
            })
            Assert-Stage5Condition ($titleEntries.Count -eq 1) `
                "Lockstep-v2 native negative-probe collection '$collectionName' must contain one $title entry."
            $expectedExecutableSha256 = $sessions[$titleIndex]['peers'][0]['executableSha256']
            $validated = Assert-Stage5LockstepNegativeProbeEvidence `
                $titleEntries[0] (Split-Path -Parent $full) $title $mode `
                $ExpectedSourceCommit $expectedExecutableSha256 `
                ([UInt32]$mapCrcs[$title]) ([int]$document['seed']) `
                $sessions[$titleIndex]['peers'][0]['launcherEquivalence']['directExecutable'] `
                $ExpectedCohortCreatedUtc
            Assert-Stage5Condition (-not $seenRunNonces.ContainsKey($validated.runNonce) -and
                -not $negativeRunNonces.ContainsKey($validated.runNonce) -and
                -not $seenSessionNonces.ContainsKey($validated.sessionNonce) -and
                -not $negativeSessionNonces.ContainsKey($validated.sessionNonce)) `
                "Lockstep-v2 native negative probe $title/$mode reuses a positive or negative nonce."
            $negativeRunNonces[$validated.runNonce] = $true
            $negativeSessionNonces[$validated.sessionNonce] = $true
            foreach ($probePath in @($validated.proofPath, $validated.stdoutPath,
                $validated.stderrPath)) {
                $probePathKey = [IO.Path]::GetFullPath([string]$probePath).ToLowerInvariant()
                Assert-Stage5Condition (-not $negativePaths.ContainsKey($probePathKey) -and
                    $probePathKey -cne $full.ToLowerInvariant()) `
                    "Lockstep-v2 native negative probe $title/$mode reuses an evidence path."
                $negativePaths[$probePathKey] = $true
            }
            if ($collectionName -ceq 'crossEpoch') {
                $validatedNegativeCrossEpoch.Add($validated) | Out-Null
            }
            else {
                $validatedNegativeContentMismatch.Add($validated) | Out-Null
            }
        }
    }
    return [pscustomobject]@{
        schemaVersion = 2; evidenceKind = 'lockstep-v2-multiplayer'
        producer = 'installed-lockstep-v2'
        validationMode = 'installed-lockstep-v2-production'
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256.ToUpperInvariant()
        cohortNonce = $cohortNonce
        runtimeClosure = $runtimeClosure
        qualificationData = $qualificationData
        titleSessionDisposition = $document['titleSessionDisposition']
        mapName = $documentMapName; mapCrcs = $mapCrcs
        commonStopFrame = 4096; peerCount = [int]$document['peerCount']
        sessions = $sessionReports.ToArray()
        negativeProbes = [pscustomobject]@{
            crossEpoch = $validatedNegativeCrossEpoch.ToArray()
            contentMismatch = $validatedNegativeContentMismatch.ToArray()
        }
        crossEpochRejected = ($validatedNegativeCrossEpoch.Count -eq 2)
        contentMismatchRejected = ($validatedNegativeContentMismatch.Count -eq 2)
    }
}

function Get-Stage5LockstepV2AcceptanceFailure {
    param(
        [string]$Path,
        [string]$Context,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [Collections.IDictionary]$ArtifactPaths = $null,
        [string]$ArtifactRootDirectory = $null,
        [string]$ExpectedEvidenceSha256 = $null,
        [object]$EvidenceSnapshot = $null,
        [string]$ExpectedCohortNonce = $null,
        [string]$ExpectedCohortCreatedUtc = $null,
        [object]$ExpectedRuntimeClosure = $null
    )
    try {
        [void](Read-Stage5LockstepV2Evidence $Path $ExpectedSourceCommit `
            $ExpectedArtifactSetSha256 $ArtifactHashes $ArtifactPaths `
            $ArtifactRootDirectory $ExpectedEvidenceSha256 $EvidenceSnapshot `
            $ExpectedCohortNonce $ExpectedCohortCreatedUtc $ExpectedRuntimeClosure)
        return $null
    }
    catch {
        return "$Context failed strict lockstep-v2 final acceptance: $($_.Exception.Message)"
    }
}

function Invoke-Stage5FinalAcceptanceAggregation {
    param(
        [string]$AcceptanceManifestPath,
        [string]$ReadinessMode = 'development-readiness',
        [switch]$DevelopmentReadiness,
        [switch]$ExternalQualificationExempt
    )
    Assert-Stage5Condition ($DevelopmentReadiness -or
        $ReadinessMode -in @('development', 'development-readiness', 'pre-manual')) `
        "Unsupported Stage 5 acceptance readiness mode '$ReadinessMode'."
    $script:Stage5FinalAcceptanceValidatedClosure =
        New-Object 'Collections.Generic.Dictionary[string,object]' `
            ([StringComparer]::OrdinalIgnoreCase)
    $requestPath = [IO.Path]::GetFullPath($AcceptanceManifestPath)
    Assert-Stage5Condition (Test-Path -LiteralPath $requestPath -PathType Leaf) `
        "Final acceptance manifest was not found: $requestPath"
    $requestDirectory = Split-Path -Parent $requestPath
    Assert-Stage5FinalAcceptanceNoReparsePath $requestDirectory $requestPath `
        'Final acceptance manifest'
    $requestSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $requestPath `
        'Final acceptance manifest'
    $request = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $requestSnapshot `
        'Final acceptance manifest'
    $requestNames = @('schemaVersion', 'gateName', 'sourceCommit', 'cohortNonce',
        'cohortCreatedUtc', 'artifactSet', 'evidence')
    Assert-Stage5JsonShape $request $requestNames 'Final acceptance manifest'
    $schemaVersion = Get-Stage5JsonValue $request 'schemaVersion' 'Final acceptance manifest'
    $gateName = Get-Stage5JsonValue $request 'gateName' 'Final acceptance manifest'
    $sourceCommit = Get-Stage5JsonValue $request 'sourceCommit' 'Final acceptance manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $schemaVersion) -and $schemaVersion -eq 1 -and
        $gateName -is [string] -and $gateName -ceq 'final-stage5-acceptance' -and
        $sourceCommit -is [string] -and $sourceCommit -match '^[0-9A-Fa-f]{40}$') `
        'Final acceptance manifest identity is invalid.'
    $sourceCommit = $sourceCommit.ToLowerInvariant()
    $cohortNonce = Assert-Stage5CanonicalUuid `
        (Get-Stage5JsonValue $request 'cohortNonce' 'Final acceptance manifest') `
        'Final acceptance manifest cohortNonce'
    $cohortCreatedUtc = Get-Stage5JsonValue $request 'cohortCreatedUtc' `
        'Final acceptance manifest'
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    Assert-Stage5Condition ($cohortCreatedUtc -is [string] -and
        $cohortCreatedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        [DateTimeOffset]::TryParseExact($cohortCreatedUtc,
            'yyyy-MM-ddTHH:mm:ss.fffffffZ',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$cohortCreated) -and
        $cohortCreated -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
        'Final acceptance manifest cohortCreatedUtc is not a canonical current UTC timestamp.'

    $artifactEntry = Get-Stage5JsonValue $request 'artifactSet' 'Final acceptance manifest'
    Assert-Stage5JsonShape $artifactEntry @('path', 'sha256') 'Final acceptance artifactSet'
    $artifactRelative = Get-Stage5JsonValue $artifactEntry 'path' 'Final acceptance artifactSet'
    $artifactExpectedHash = Get-Stage5JsonValue $artifactEntry 'sha256' 'Final acceptance artifactSet'
    Assert-Stage5Condition ($artifactRelative -is [string]) 'Final acceptance artifactSet path must be a JSON string.'
    $artifactPath = Resolve-Stage5FinalAcceptanceFile $requestDirectory $artifactRelative `
        'Final acceptance artifactSet'
    $artifactSetSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $artifactPath `
        'Final acceptance artifactSet'
    $artifactSetHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $artifactSetSnapshot `
        $artifactExpectedHash 'Final acceptance artifactSet'
    $artifactDirectory = Split-Path -Parent $artifactPath
    $artifactSet = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $artifactSetSnapshot `
        'Artifact set manifest'
    $artifactNames = @('schemaVersion', 'sourceCommit', 'productSet', 'architecture',
        'artifacts', 'runtimeClosure')
    Assert-Stage5JsonShape $artifactSet $artifactNames 'Artifact set manifest'
    $artifactSetSchemaVersion = Get-Stage5JsonValue $artifactSet `
        'schemaVersion' 'Artifact set manifest'
    $artifactSetSourceCommit = Get-Stage5JsonValue $artifactSet `
        'sourceCommit' 'Artifact set manifest'
    $artifactSetArchitecture = Get-Stage5JsonValue $artifactSet `
        'architecture' 'Artifact set manifest'
    Assert-Stage5Condition ((Test-Stage5JsonInteger $artifactSetSchemaVersion) -and
        $artifactSetSchemaVersion -eq 1 -and
        $artifactSetSourceCommit -is [string] -and
        $artifactSetArchitecture -is [string] -and
        $artifactSetSourceCommit -ceq $sourceCommit -and
        $artifactSetArchitecture -ceq 'x64') `
        'Artifact set manifest identity does not match the final acceptance request.'
    Assert-Stage5FinalAcceptanceStringSet `
        (Get-Stage5JsonValue $artifactSet 'productSet' 'Artifact set manifest') `
        @('Generals', 'ZeroHour') 'Artifact set productSet'
    $runtimeClosureResult = Get-Stage5RuntimeClosureBinding $artifactSet `
        $artifactDirectory $sourceCommit 'Artifact set runtime closure'
    $expectedRuntimeClosure = [pscustomobject]@{
        dependencyManifestSha256 = $runtimeClosureResult.dependencyManifestSha256
        closureSha256 = $runtimeClosureResult.closureSha256
    }
    $requiredArtifactRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifacts = Get-Stage5JsonValue $artifactSet 'artifacts' 'Artifact set manifest'
    Assert-Stage5Condition ($artifacts -is [Array]) 'Artifact set artifacts must be a JSON array.'
    $artifactRoles = New-Object 'Collections.Generic.List[string]'
    $artifactPaths = New-Object 'Collections.Generic.List[string]'
    $artifactHashes = @{}
    $artifactPathsByRole = @{}
    $runtimeClosureFilesByPath = @{}
    foreach ($closureFile in @($runtimeClosureResult.files)) {
        Assert-Stage5Condition ($null -ne $closureFile -and
            @('path', 'sha256', 'fullPath', 'snapshot' | Where-Object {
                @($closureFile.PSObject.Properties.Name) -contains $_
            }).Count -eq 4) `
            'Artifact set runtime closure snapshot is incomplete.'
        $closureKey = ([string]$closureFile.path).Replace('\', '/').ToLowerInvariant()
        Assert-Stage5Condition (-not $runtimeClosureFilesByPath.ContainsKey($closureKey)) `
            "Artifact set runtime closure repeats path '$($closureFile.path)'."
        $runtimeClosureFilesByPath[$closureKey] = $closureFile
    }
    foreach ($artifact in $artifacts) {
        Assert-Stage5JsonShape $artifact @('role', 'path', 'sha256') 'Artifact set entry'
        $role = Get-Stage5JsonValue $artifact 'role' 'Artifact set entry'
        $relative = Get-Stage5JsonValue $artifact 'path' 'Artifact set entry'
        $expectedHash = Get-Stage5JsonValue $artifact 'sha256' 'Artifact set entry'
        Assert-Stage5Condition ($role -is [string] -and $relative -is [string] -and
            $expectedHash -is [string]) `
            'Artifact set role, path, and hash must be JSON strings.'
        Assert-Stage5Condition (-not ($artifactRoles -contains $role)) `
            "Artifact set repeats role '$role'."
        $artifactKey = ([string]$relative).Replace('\', '/').ToLowerInvariant()
        Assert-Stage5Condition ($runtimeClosureFilesByPath.ContainsKey($artifactKey)) `
            "Artifact set role '$role' is not bound to a snapshotted runtime-closure file."
        $closureFile = $runtimeClosureFilesByPath[$artifactKey]
        $artifactFile = [IO.Path]::GetFullPath([string]$closureFile.fullPath)
        $expectedArtifactPath = [IO.Path]::GetFullPath((Join-Path $artifactDirectory $relative))
        Assert-Stage5Condition ($artifactFile -ceq $expectedArtifactPath) `
            "Artifact set role '$role' resolved outside its snapshotted runtime-closure file."
        Assert-Stage5Condition (-not ($artifactPaths -contains $artifactFile.ToLowerInvariant())) `
            "Artifact set aliases path '$relative'."
        $verifiedArtifactHash = Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
            $closureFile.snapshot $expectedHash $closureFile.snapshot.length `
            "Artifact set role '$role' snapshot"
        $artifactRoles.Add($role) | Out-Null
        $artifactPaths.Add($artifactFile.ToLowerInvariant()) | Out-Null
        $artifactHashes[$role] = $verifiedArtifactHash
        $artifactPathsByRole[$role] = $artifactFile
    }
    Assert-Stage5FinalAcceptanceStringSet $artifactRoles.ToArray() $requiredArtifactRoles `
        'Artifact set roles'

    # Local acceptance has exactly one authority: development evidence ready
    # for a later, external manual decision.  Premium review and manual
    # approval are deliberately out-of-band and can never be entries in this
    # manifest or local JSON output.
    $requiredEvidenceKinds = @('deterministic-runtime', 'replay-determinism',
        'fresh-ai', 'performance-scaling', 'mixed-worker-multiplayer',
        'combined-stage4-stage5-installed-runtime')
    $evidenceEntries = Get-Stage5JsonValue $request 'evidence' 'Final acceptance manifest'
    Assert-Stage5Condition ($evidenceEntries -is [Array]) `
        'Final acceptance evidence must be a JSON array.'
    $evidenceByKind = @{}
    $evidenceHashes = @{}
    $evidencePaths = New-Object 'Collections.Generic.List[string]'
    foreach ($entry in $evidenceEntries) {
        Assert-Stage5JsonShape $entry @('kind', 'path', 'sha256') 'Final acceptance evidence entry'
        $kind = Get-Stage5JsonValue $entry 'kind' 'Final acceptance evidence entry'
        $relative = Get-Stage5JsonValue $entry 'path' 'Final acceptance evidence entry'
        $expectedHash = Get-Stage5JsonValue $entry 'sha256' 'Final acceptance evidence entry'
        Assert-Stage5Condition ($kind -is [string] -and $relative -is [string] -and
            $expectedHash -is [string]) `
            'Final acceptance evidence kind, path, and hash must be JSON strings.'
        Assert-Stage5Condition ($requiredEvidenceKinds -ccontains $kind) `
            "Final acceptance evidence kind '$kind' is not part of the local pre-manual contract; premium review and manual approval are out-of-band."
        Assert-Stage5Condition (-not $evidenceByKind.ContainsKey($kind)) `
            "Final acceptance evidence repeats kind '$kind'."
        $evidencePath = Resolve-Stage5FinalAcceptanceFile $requestDirectory $relative `
            "Final acceptance evidence '$kind'"
        Assert-Stage5Condition (-not ($evidencePaths -contains $evidencePath.ToLowerInvariant())) `
            "Final acceptance evidence aliases path '$relative'."
        $evidenceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $evidencePath `
            "Final acceptance evidence '$kind'"
        $evidenceHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $evidenceSnapshot `
            $expectedHash "Final acceptance evidence '$kind'"
        $evidenceDocument = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $evidenceSnapshot `
            "Evidence '$kind'"
        $evidenceNames = @('schemaVersion', 'evidenceKind', 'status', 'sourceCommit',
            'title', 'architecture', 'artifactSetSha256', 'recordedUtc',
            'cohortNonce', 'runtimeClosure', 'attachments', 'details')
        Assert-Stage5JsonShape $evidenceDocument $evidenceNames "Evidence '$kind'"
        $evidenceSchemaVersion = Get-Stage5JsonValue $evidenceDocument `
            'schemaVersion' "Evidence '$kind'"
        $evidenceKind = Get-Stage5JsonValue $evidenceDocument `
            'evidenceKind' "Evidence '$kind'"
        $evidenceStatus = Get-Stage5JsonValue $evidenceDocument `
            'status' "Evidence '$kind'"
        $evidenceSourceCommit = Get-Stage5JsonValue $evidenceDocument `
            'sourceCommit' "Evidence '$kind'"
        $evidenceArchitecture = Get-Stage5JsonValue $evidenceDocument `
            'architecture' "Evidence '$kind'"
        $evidenceArtifactSetSha256 = Get-Stage5JsonValue $evidenceDocument `
            'artifactSetSha256' "Evidence '$kind'"
        Assert-Stage5Condition ((Test-Stage5JsonInteger $evidenceSchemaVersion) -and
            $evidenceSchemaVersion -eq 1 -and
            $evidenceKind -is [string] -and
            $evidenceStatus -is [string] -and
            $evidenceSourceCommit -is [string] -and
            $evidenceArchitecture -is [string] -and
            $evidenceArtifactSetSha256 -is [string] -and
            $evidenceKind -ceq $kind -and
            $evidenceStatus -ceq 'passed' -and
            $evidenceSourceCommit -ceq $sourceCommit -and
            $evidenceArchitecture -ceq 'x64' -and
            $evidenceArtifactSetSha256 -ceq $artifactSetHash) `
            "Evidence '$kind' does not identify the same passed x64 commit and artifact set."
        $evidenceCohortNonce = Assert-Stage5CanonicalUuid `
            (Get-Stage5JsonValue $evidenceDocument 'cohortNonce' "Evidence '$kind'") `
            "Evidence '$kind' cohortNonce"
        Assert-Stage5Condition ($evidenceCohortNonce -ceq $cohortNonce) `
            "Evidence '$kind' is stale or detached from the requested execution cohort."
        [void](Assert-Stage5RuntimeClosureBinding `
            (Get-Stage5JsonValue $evidenceDocument 'runtimeClosure' "Evidence '$kind'") `
            $expectedRuntimeClosure "Evidence '$kind'")
        $title = Get-Stage5JsonValue $evidenceDocument 'title' "Evidence '$kind'"
        $expectedTitle = if ($kind -ceq 'replay-determinism' -or
            $kind -ceq 'mixed-worker-multiplayer' -or
            $kind -ceq 'combined-stage4-stage5-installed-runtime') {
            'Both'
        }
        else { 'ZeroHour' }
        Assert-Stage5Condition ($title -is [string] -and
            $title -ceq $expectedTitle) `
            "Evidence '$kind' must have exact title scope '$expectedTitle'."
        $recordedUtc = Get-Stage5JsonValue $evidenceDocument 'recordedUtc' "Evidence '$kind'"
        [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
        Assert-Stage5Condition ($recordedUtc -is [string] -and
            $recordedUtc -cmatch
                '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
            [DateTimeOffset]::TryParseExact($recordedUtc,
                'yyyy-MM-ddTHH:mm:ss.fffffffZ',
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::AssumeUniversal,
                [ref]$recorded)) `
            "Evidence '$kind' recordedUtc is not a canonical UTC timestamp."
        Assert-Stage5Condition ($recorded -ge $cohortCreated) `
            "Evidence '$kind' recordedUtc predates the requested execution cohort."
        Assert-Stage5Condition ($recorded -le [DateTimeOffset]::UtcNow.AddMinutes(5)) `
            "Evidence '$kind' recordedUtc is unreasonably in the future."
        $evidenceByKind[$kind] = [pscustomobject]@{
            relativePath = $relative
            fullPath = $evidencePath
            sha256 = $evidenceHash
            document = $evidenceDocument
        }
        $evidenceHashes[$kind] = $evidenceHash
        $evidencePaths.Add($evidencePath.ToLowerInvariant()) | Out-Null
    }
    foreach ($requiredKind in $requiredEvidenceKinds) {
        Assert-Stage5Condition ($evidenceByKind.ContainsKey($requiredKind)) `
            "Final acceptance evidence is missing required kind '$requiredKind'."
    }

    $attachmentBindings = @{
        'deterministic-runtime' = @('validation-plan|ZeroHour',
            'validation-results|ZeroHour', 'performance-report|ZeroHour')
        'replay-determinism' = @('replay-results|ZeroHour',
            'replay-fixture-manifest|Generals',
            'replay-fixture-manifest|ZeroHour')
        'fresh-ai' = @('ai-results|ZeroHour')
        'performance-scaling' = @('performance-report|ZeroHour',
            'stage3-baseline|ZeroHour', 'phase-baseline-profile|ZeroHour')
        'mixed-worker-multiplayer' = @('multiplayer-results|Both')
        'combined-stage4-stage5-installed-runtime' = @('combined-results|Both')
    }
    $attachmentTrustDomains = @{
        'validation-plan' = 'host-runner'
        'validation-results' = 'host-runner'
        'replay-results' = 'host-runner'
        'replay-fixture-manifest' = 'reviewed-fixture'
        'ai-results' = 'host-runner'
        'performance-report' = 'host-runner'
        'stage3-baseline' = 'reviewed-fixture'
        'phase-baseline-profile' = 'reviewed-fixture'
        'installed-kernel-execution' = 'installed-runtime'
        'multiplayer-results' = 'host-runner'
        'combined-results' = 'host-runner'
        'premium-review-results' = 'premium-review'
        'manual-checklist' = 'manual-approval'
    }
    $lockstepV2Failure = $null
    $immutableReceiptRoles = @('validation-plan', 'validation-results',
        'replay-results', 'replay-fixture-manifest', 'ai-results',
        'combined-results', 'premium-review-results', 'manual-checklist')
    $receiptFailures = New-Object 'Collections.Generic.List[string]'
    $receiptRunNonces = @{}
    $executionReceipts = @{}
    $titleQualificationData = @{}
    $reportEvidence = New-Object 'Collections.Generic.List[object]'
    $mixedNativeEvidenceRead = $null
    $installedKernelExecutionDisposition = $null
    $installedKernelExecutionProof = $null
    $globalNativePathOwners = @{}
    foreach ($kind in $requiredEvidenceKinds) {
        $record = $evidenceByKind[$kind]
        $document = $record.document
        $attachments = Get-Stage5JsonValue $document 'attachments' "Evidence '$kind'"
        Assert-Stage5Condition ($attachments -is [Array]) `
            "Evidence '$kind' attachments must be a JSON array."
        $seenBindings = New-Object 'Collections.Generic.List[string]'
        $seenPaths = New-Object 'Collections.Generic.List[string]'
        $attachmentReport = New-Object 'Collections.Generic.List[object]'
        $scalingAttachmentPath = $null
        $scalingAttachmentHash = $null
        $scalingProof = $null
        $stage3BaselineAttachmentHash = $null
        $phaseBaselineProfileAttachmentHash = $null
        $mixedNativeEvidenceAttachmentHash = $null
        $evidenceDirectory = Split-Path -Parent $record.fullPath
        $evidenceTitle = Get-Stage5JsonValue $document 'title' "Evidence '$kind'"
        $expectedAttachmentBindings = @($attachmentBindings[$kind])
        if ($kind -ceq 'deterministic-runtime') {
            $runtimeDetails = Get-Stage5JsonValue $document 'details' `
                "Evidence '$kind'"
            $installedKernelExecutionDisposition = Get-Stage5JsonValue `
                $runtimeDetails 'installedKernelExecution' `
                "Evidence '$kind' details"
            Assert-Stage5JsonShape $installedKernelExecutionDisposition `
                @('status', 'claim', 'reason', 'sha256') `
                "Evidence '$kind' installedKernelExecution"
            $kernelStatus = Get-Stage5JsonValue `
                $installedKernelExecutionDisposition 'status' `
                "Evidence '$kind' installedKernelExecution"
            Assert-Stage5Condition ($kernelStatus -is [string]) `
                "Evidence '$kind' installedKernelExecution status must be a JSON string."
            if ($kernelStatus -ceq 'passed') {
                Assert-Stage5Condition (-not $ExternalQualificationExempt) `
                    'Installed-kernel execution cannot be passed while the caller explicitly authorizes the external-qualification exemption.'
                $expectedAttachmentBindings += 'installed-kernel-execution|Both'
            }
            elseif ($kernelStatus -ceq 'skipped') {
                Assert-Stage5Condition ($ExternalQualificationExempt) `
                    'Installed-kernel execution may be skipped only when the caller explicitly authorizes the external-qualification exemption.'
            }
            else {
                throw "Evidence '$kind' installedKernelExecution status is invalid."
            }
        }
        foreach ($attachment in $attachments) {
            Assert-Stage5JsonShape $attachment @('role', 'title', 'path',
                'sha256', 'trustDomain') `
                "Evidence '$kind' attachment"
            $role = Get-Stage5JsonValue $attachment 'role' "Evidence '$kind' attachment"
            $attachmentTitle = Get-Stage5JsonValue $attachment 'title' `
                "Evidence '$kind' attachment"
            $relative = Get-Stage5JsonValue $attachment 'path' "Evidence '$kind' attachment"
            $expectedHash = Get-Stage5JsonValue $attachment 'sha256' "Evidence '$kind' attachment"
            $attachmentTrustDomain = Get-Stage5JsonValue $attachment 'trustDomain' `
                "Evidence '$kind' attachment"
            Assert-Stage5Condition ($role -is [string] -and
                $attachmentTitle -is [string] -and $relative -is [string]) `
                "Evidence '$kind' attachment role, title, and path must be JSON strings."
            Assert-Stage5Condition ($attachmentTrustDomains.ContainsKey($role) -and
                $attachmentTrustDomain -is [string] -and
                $attachmentTrustDomain -ceq [string]$attachmentTrustDomains[$role]) `
                "Evidence '$kind' attachment '$role' has the wrong trust domain."
            $attachmentBinding = "$role|$attachmentTitle"
            Assert-Stage5Condition ($expectedAttachmentBindings -ccontains
                    $attachmentBinding -and
                -not ($seenBindings -ccontains $attachmentBinding)) `
                "Evidence '$kind' repeats or does not authorize attachment '$attachmentBinding'."
            $attachmentPath = Resolve-Stage5FinalAcceptanceFile $evidenceDirectory $relative `
                "Evidence '$kind' attachment '$role'"
            Assert-Stage5Condition (-not ($seenPaths -contains $attachmentPath.ToLowerInvariant())) `
                "Evidence '$kind' aliases attachment path '$relative'."
            $attachmentSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $attachmentPath `
                "Evidence '$kind' attachment '$role'"
            $attachmentHash = Assert-Stage5FinalAcceptanceSnapshotSha256 `
                $attachmentSnapshot $expectedHash "Evidence '$kind' attachment '$role'"
            $readGenericReceipt = $immutableReceiptRoles -ccontains $role -or
                ($kind -ceq 'deterministic-runtime' -and $role -ceq 'performance-report')
            if ($readGenericReceipt) {
                try {
                    $receiptArguments = [ordered]@{
                        Path = $attachmentPath
                        Kind = $kind
                        Role = $role
                        EvidenceTitle = $attachmentTitle
                        ExpectedSourceCommit = $sourceCommit
                        ExpectedArtifactSetSha256 = $artifactSetHash
                        ArtifactHashes = $artifactHashes
                        ArtifactPaths = $artifactPathsByRole
                        SeenRunNonces = $receiptRunNonces
                        ExpectedEvidenceSha256 = $attachmentHash
                        EvidenceSnapshot = $attachmentSnapshot
                        ExpectedRuntimeClosure = $expectedRuntimeClosure
                        GlobalNativePathOwners = $globalNativePathOwners
                    }
                    if ($titleQualificationData.ContainsKey($attachmentTitle)) {
                        $receiptArguments['ExpectedQualificationData'] =
                            $titleQualificationData[$attachmentTitle]
                    }
                    if ($role -cne 'replay-fixture-manifest') {
                        $receiptArguments['ExpectedCohortNonce'] = $cohortNonce
                        $receiptArguments['ExpectedCohortCreatedUtc'] = $cohortCreatedUtc
                    }
                    if ($role -in @('validation-results', 'replay-results',
                            'ai-results', 'performance-report')) {
                        $attachmentReceipt = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
                            $attachmentSnapshot "Evidence '$kind' attachment '$role'"
                        if ((Get-Stage5JsonValue $attachmentReceipt 'trustDomain' `
                                "Evidence '$kind' attachment '$role'") -ceq 'host-runner') {
                            $relocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
                                -Path $attachmentPath -EvidenceDirectory $evidenceDirectory `
                                -GlobalNativePathOwners $globalNativePathOwners `
                                -GlobalNativeTitle $attachmentTitle
                            $receiptArguments['ExpectedEvidenceDirectory'] =
                                $relocation.evidenceDirectory
                            if ($role -ceq 'validation-results') {
                                $receiptArguments['NativeRelocationBindings'] =
                                    @($relocation.children)
                            }
                            else {
                                $receiptArguments['NativeRawBindings'] =
                                    $relocation.nativeRawBindings
                                $receiptArguments['NativeReceiptSourcePath'] =
                                    $relocation.nativeReceiptSourcePath
                            }
                        }
                    }
                    $receipt = Read-Stage5FinalAcceptanceImmutableReceipt @receiptArguments
                    if ($null -ne $receipt.acceptanceFailure) {
                        $receiptFailures.Add($receipt.acceptanceFailure) | Out-Null
                    }
                    else {
                        $executionReceipts["$kind/$role/$attachmentTitle"] = $receipt
                        if ($null -ne $receipt.qualificationData) {
                            if ($titleQualificationData.ContainsKey($attachmentTitle)) {
                                [void](Assert-Stage5SimulationQualificationBindingEqual `
                                    $receipt.qualificationData `
                                    $titleQualificationData[$attachmentTitle] `
                                    "Evidence '$kind' attachment '$attachmentBinding' qualificationData")
                            }
                            else {
                                $titleQualificationData[$attachmentTitle] =
                                    $receipt.qualificationData
                            }
                        }
                    }
                }
                catch {
                    $receiptFailures.Add($_.Exception.Message) | Out-Null
                }
            }
            if ($kind -ceq 'mixed-worker-multiplayer' -and $role -ceq 'multiplayer-results') {
                # Read-Stage5Net3LoopbackEvidence remains available for the
                # supplementary v1 diagnostic gate. It is deliberately not a
                # final-acceptance authority. The v2 host runner is the only
                # producer accepted by the final gate; defer this failure until
                # all other evidence checks have run so negative tests remain
                # specific.
                try {
                    $mixedNativeEvidenceRead = Read-Stage5LockstepV2Evidence `
                        $attachmentPath $sourceCommit $artifactSetHash $artifactHashes `
                        $artifactPathsByRole $artifactDirectory $attachmentHash $attachmentSnapshot `
                        $cohortNonce $cohortCreatedUtc $expectedRuntimeClosure
                    $lockstepV2Failure = $null
                }
                catch {
                    $mixedNativeEvidenceRead = $null
                    $lockstepV2Failure = $_.Exception.Message
                }
                if ($null -eq $lockstepV2Failure) {
                    # The v2 details hash is the independently rehashed native
                    # attachment, not the outer acceptance envelope hash.
                    $mixedNativeEvidenceAttachmentHash = $attachmentHash
                }
            }
            if ($kind -ceq 'performance-scaling' -and $role -ceq 'performance-report') {
                $scalingAttachmentPath = $attachmentPath
                $scalingAttachmentHash = $attachmentHash
            }
            if ($kind -ceq 'performance-scaling' -and $role -ceq 'stage3-baseline') {
                $stage3BaselineAttachmentHash = $attachmentHash
            }
            if ($kind -ceq 'performance-scaling' -and
                $role -ceq 'phase-baseline-profile') {
                $phaseBaselineProfileAttachmentHash = $attachmentHash
            }
            if ($kind -ceq 'deterministic-runtime' -and
                $role -ceq 'installed-kernel-execution') {
                if ($null -eq (Get-Command Read-Stage5InstalledKernelExecutionEvidence `
                            -ErrorAction SilentlyContinue)) {
                    Import-Module (Join-Path $PSScriptRoot `
                        'Stage5InstalledKernelExecutionEvidence.psm1') -Force
                }
                $installedKernelExecutionProof =
                    Read-Stage5InstalledKernelExecutionEvidence `
                        -Path $attachmentPath -ExpectedSha256 $attachmentHash `
                        -ExpectedSourceCommit $sourceCommit `
                        -ExpectedArtifactSetSha256 $artifactSetHash `
                        -ExpectedCohortNonce $cohortNonce `
                        -ExpectedCohortCreatedUtc $cohortCreatedUtc `
                        -ExpectedDependencyManifestSha256 `
                            $expectedRuntimeClosure.dependencyManifestSha256 `
                        -ExpectedRuntimeClosureSha256 `
                            $expectedRuntimeClosure.closureSha256 `
                        -GeneralsExecutableSha256 `
                            $artifactHashes['generals-executable'] `
                        -ZeroHourExecutableSha256 `
                            $artifactHashes['zerohour-executable']
                Assert-Stage5Condition (
                    [string]$installedKernelExecutionProof.sha256 -ceq
                        [string]$installedKernelExecutionDisposition.sha256 -and
                    -not [bool]$installedKernelExecutionProof.finalAcceptanceClaim -and
                    -not [bool]$installedKernelExecutionProof.performanceScalingClaim) `
                    'Installed-kernel execution attachment is detached or overclaims final/scaling acceptance.'
            }
            $seenBindings.Add($attachmentBinding) | Out-Null
            $seenPaths.Add($attachmentPath.ToLowerInvariant()) | Out-Null
            $attachmentReport.Add([pscustomobject]@{
                role = $role; title = $attachmentTitle
                path = $relative; sha256 = $attachmentHash
                trustDomain = $attachmentTrustDomain
            }) | Out-Null
        }
        Assert-Stage5FinalAcceptanceStringSet $seenBindings.ToArray() `
            $expectedAttachmentBindings "Evidence '$kind' attachment bindings"
        if ($kind -ceq 'performance-scaling') {
            Assert-Stage5Condition ($null -ne $scalingAttachmentPath -and
                $null -ne $stage3BaselineAttachmentHash -and
                $null -ne $phaseBaselineProfileAttachmentHash) `
                'Performance evidence is missing its report, Stage 3 baseline, or reviewed phase-baseline profile binding.'
            $scalingProof = Read-Stage5PerformanceScalingEvidence -Path $scalingAttachmentPath `
                -ExpectedSourceCommit $sourceCommit `
                -ExpectedArtifactSetSha256 $artifactSetHash `
                -ExpectedExecutableSha256 $artifactHashes['zerohour-executable'] `
                -ExpectedStage3BaselineSha256 $stage3BaselineAttachmentHash `
                -ExpectedCohortNonce $cohortNonce `
                -ExpectedCohortCreatedUtc $cohortCreatedUtc `
                -ExpectedRuntimeClosure $expectedRuntimeClosure `
                -ExpectedPhaseBaselineProfileSha256 `
                    $phaseBaselineProfileAttachmentHash
            Assert-Stage5Condition ($scalingProof.evidenceManifestSha256 -ceq $scalingAttachmentHash -and
                $scalingProof.fixtureCount -eq 4 -and $scalingProof.kernelCount -eq 6 -and
                $scalingProof.eightPhysicalCoreSpeedupFloor -ge 2.0) `
                'Performance attachment did not produce the exact physical-core scaling proof.'
        }
        Assert-Stage5FinalAcceptanceDetails $kind `
            (Get-Stage5JsonValue $document 'details' "Evidence '$kind'") `
            $sourceCommit $evidenceHashes $mixedNativeEvidenceAttachmentHash `
            $mixedNativeEvidenceRead -ScalingProof $scalingProof
        $reportEvidence.Add([pscustomobject]@{
            kind = $kind
            path = $record.relativePath
            sha256 = $record.sha256
            freshness = 'current-cohort'
            recordedUtc = Get-Stage5JsonValue $document 'recordedUtc' "Evidence '$kind'"
            attachments = $attachmentReport.ToArray()
        }) | Out-Null
    }
    if ($null -ne $lockstepV2Failure -and
        $lockstepV2Failure -match 'diagnostic NET3 v1') {
        throw $lockstepV2Failure
    }
    if ($receiptFailures.Count -gt 0) {
        throw ($receiptFailures -join ' | ')
    }
    if ($null -ne $lockstepV2Failure) {
        throw $lockstepV2Failure
    }
    $planReceipt =
        $executionReceipts['deterministic-runtime/validation-plan/ZeroHour']
    $validationReceipt =
        $executionReceipts['deterministic-runtime/validation-results/ZeroHour']
    $replayReceipt =
        $executionReceipts['replay-determinism/replay-results/ZeroHour']
    $aiReceipt = $executionReceipts['fresh-ai/ai-results/ZeroHour']
    $generalsReviewedReceipt =
        $executionReceipts['replay-determinism/replay-fixture-manifest/Generals']
    $reviewedReceipt =
        $executionReceipts['replay-determinism/replay-fixture-manifest/ZeroHour']
    $combinedReceipt =
        $executionReceipts['combined-stage4-stage5-installed-runtime/combined-results/Both']
    Assert-Stage5Condition ($null -ne $planReceipt -and
        $null -ne $validationReceipt -and $null -ne $replayReceipt -and
        $null -ne $aiReceipt -and $null -ne $generalsReviewedReceipt -and
        $null -ne $reviewedReceipt -and $null -ne $combinedReceipt -and
        $null -ne $combinedReceipt.combinedSourceBindings) `
        'Final acceptance is missing one or more independently validated execution-authority receipts.'
    $combinedCorpora = @($combinedReceipt.combinedSourceBindings.sourceCorpora)
    Assert-Stage5Condition ($combinedCorpora.Count -eq 2 -and
        [string]$combinedCorpora[0].title -ceq 'Generals' -and
        [string]$combinedCorpora[1].title -ceq 'ZeroHour' -and
        [string]$combinedCorpora[0].reviewedReceiptSha256 -ceq
            [string]$generalsReviewedReceipt.sha256 -and
        [string]$combinedCorpora[1].reviewedReceiptSha256 -ceq
            [string]$reviewedReceipt.sha256) `
        'Final acceptance combined title corpora are detached from the two independently protected reviewed-fixture authorities.'
    [void](Assert-Stage5SimulationQualificationBindingEqual `
        $combinedCorpora[1].qualificationData `
        $validationReceipt.qualificationData `
        'Final acceptance combined ZeroHour qualificationData')
    $zeroHourCombinedReceipts = $combinedCorpora[1].receipts
    foreach ($binding in @(
        [pscustomobject]@{ role = 'validation-plan'; receipt = $planReceipt },
        [pscustomobject]@{ role = 'validation-results'; receipt = $validationReceipt },
        [pscustomobject]@{ role = 'replay-results'; receipt = $replayReceipt },
        [pscustomobject]@{ role = 'ai-results'; receipt = $aiReceipt }
    )) {
        $combinedSourceReceipt = $zeroHourCombinedReceipts[$binding.role]
        Assert-Stage5Condition ($null -ne $combinedSourceReceipt -and
            [string]$combinedSourceReceipt.sha256 -ceq
                [string]$binding.receipt.sha256 -and
            [string]$combinedSourceReceipt.runNonce -ceq
                [string]$binding.receipt.runNonce) `
            "Final acceptance combined ZeroHour '$($binding.role)' authority differs from its independently attached title receipt."
    }
    $planRaw = Get-Stage5DevelopmentReadinessRawLog $planReceipt.rawLogs `
        'validation-plan.json' 'Final acceptance validation plan'
    $validationResultsRaw = Get-Stage5DevelopmentReadinessRawLog `
        $validationReceipt.rawLogs 'validation-results.json' `
        'Final acceptance validation results'
    $replayResultsRaw = Get-Stage5DevelopmentReadinessRawLog `
        $replayReceipt.rawLogs 'validation-results.json' `
        'Final acceptance replay results'
    $aiResultsRaw = Get-Stage5DevelopmentReadinessRawLog `
        $aiReceipt.rawLogs 'validation-results.json' `
        'Final acceptance AI results'
    $validationPlan = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
        $planRaw.snapshot 'Final acceptance validation plan'
    $validationResults = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
        $validationResultsRaw.snapshot 'Final acceptance validation results'
    Assert-Stage5Condition ($validationResults -is [Array]) `
        'Final acceptance validation results must be a JSON array.'
    [void](Assert-Stage5SupportingReceiptChildInExecutionCorpus `
        $replayReceipt $validationReceipt 'replay-results' `
        'Final acceptance ZeroHour replay-results lineage')
    [void](Assert-Stage5SupportingReceiptChildInExecutionCorpus `
        $aiReceipt $validationReceipt 'ai-results' `
        'Final acceptance ZeroHour ai-results lineage')
    [void](Assert-Stage5DevelopmentReadinessExecutionEvidence `
        -ValidationPlan $validationPlan -Results @($validationResults) `
        -ReviewedFixtureManifest $reviewedReceipt.reviewedFixtureManifest `
        -PlanDetails $planReceipt.details `
        -ValidationDetails $validationReceipt.details `
        -ReplayDetails $replayReceipt.details -AiDetails $aiReceipt.details `
        -ValidatedRawLogs $validationReceipt.rawLogs `
        -ValidatedChildren $validationReceipt.validatedChildren `
        -ExpectedPlanSha256 ([string]$planRaw.sha256) `
        -ValidationResultsSha256 ([string]$validationResultsRaw.sha256) `
        -ReplayResultsSha256 ([string]$replayResultsRaw.sha256) `
        -AiResultsSha256 ([string]$aiResultsRaw.sha256) `
        -ExpectedSourceCommit $sourceCommit `
        -ExpectedArtifactSetSha256 $artifactSetHash `
        -ExpectedCohortNonce $cohortNonce `
        -ExpectedCohortCreatedUtc $cohortCreatedUtc `
        -ExpectedRuntimeClosure $expectedRuntimeClosure `
        -ExpectedQualificationData $validationReceipt.qualificationData `
        -ExpectedCurrentExecutablePath `
            $artifactPathsByRole['zerohour-executable'] `
        -RequireCurrentArtifactRelocation -ExpectedTitle 'ZeroHour')
    return [pscustomobject]@{
        schemaVersion = 1
        gateName = 'stage5-development-readiness'
        status = 'ready-for-manual-approval'
        readiness = 'pre-manual'
        cohortNonce = $cohortNonce
        cohortCreatedUtc = $cohortCreatedUtc
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = $expectedRuntimeClosure.dependencyManifestSha256
            closureSha256 = $expectedRuntimeClosure.closureSha256
        }
        evidenceFreshness = 'current-cohort'
        installedKernelExecution = $installedKernelExecutionDisposition
        premiumReviewRequired = $true
        manualApprovalRequired = $true
        externalApprovalRequired = @('premium-review', 'manual-acceptance')
        finalAcceptanceClaim = $false
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        sourceCommit = $sourceCommit
        artifactSet = [pscustomobject]@{
            path = [string]$artifactRelative
            sha256 = $artifactSetHash
        }
        evidence = $reportEvidence.ToArray()
    }
}

# These structural and scalar helpers are part of the validation-module
# boundary. Installed-runtime validators use the same strict JSON and counter
# rules as the aggregate readers; keeping them exported avoids dot-sourcing a
# .psm1 file into a caller scope, where Export-ModuleMember is invalid and the
# private commands are unavailable.
Export-ModuleMember -Function ConvertFrom-Stage5JsonDictionary, Get-Stage5JsonValue, `
    Assert-Stage5NativePerformanceReceiptProvenance, `
    Assert-Stage5PhaseAccountingContract, `
    Assert-Stage5JsonShape, Test-Stage5JsonInteger, Test-Stage5JsonNumber, `
    Get-Stage5UInt64BitCount, Get-Stage5FileSha256, `
    ConvertFrom-Stage5AiCompletion, ConvertFrom-Stage5ReplayMetrics, Resolve-Stage5LiveValidationRequirements, `
    ConvertFrom-Stage5ReplayResult, Get-Stage5TimingEvidence, Assert-Stage5AiDeterminism, Assert-Stage5ReplayDeterminism, `
    Assert-Stage5AuthoritativeWorkEvidence, Assert-Stage5CollisionTimingEvidence, `
    Assert-Stage5DevelopmentReadinessExecutionEvidence, `
    Get-Stage5DevelopmentReadinessRawLog, `
    Read-Stage5PerformanceBaseline, Measure-Stage5Performance, Invoke-Stage5RegistryRestore, `
    Invoke-Stage5RegistrySetupTransaction, `
    Test-Stage5RegistryLeafRemoval, Invoke-Stage5CreatedRegistryKeyCleanup, `
    Invoke-Stage5FinalAcceptanceAggregation, Read-Stage5Net3LoopbackEvidence, `
    Read-Stage5PerformanceScalingEvidence, ConvertTo-Stage5PerformanceDiagnostics, Read-Stage5FinalAcceptanceImmutableReceipt, `
    Read-Stage5LockstepV2Evidence, Read-Stage5SimulationQualificationDataEvidence, `
    Get-Stage5RuntimeClosureBinding, `
    Assert-Stage5FinalAcceptancePathContained, Assert-Stage5FinalAcceptanceNoReparsePath, `
    Assert-Stage5FinalAcceptanceFileHandlePath, Get-Stage5FinalAcceptanceFileSnapshot, `
    Get-Stage5FinalAcceptanceNativeRelocationBinding, `
    Get-Stage5FinalAcceptanceValidatedClosure, `
    Write-Stage5FinalAcceptanceFileAtomically, `
    ConvertFrom-Stage5FinalAcceptanceJsonSnapshot, Assert-Stage5FinalAcceptanceSnapshotSha256, `
    Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256
