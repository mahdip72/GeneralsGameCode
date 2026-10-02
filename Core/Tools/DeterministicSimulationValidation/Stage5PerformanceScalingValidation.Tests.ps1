param(
    [string]$ScratchRoot = '',
    [switch]$HeldInputPreflightOnly,
    [switch]$OutputCapturePreflightOnly,
    [switch]$ProductionTimingPreflightOnly,
    [switch]$SourceContractPreflightOnly,
    [switch]$ProfileClosurePreflightOnly,
    [switch]$JsonScalarTypingPreflightOnly,
    [switch]$FinalizationPreflightOnly,
    [string]$ExportAuthoritativeFixtureRoot = '',
    [string]$ExportArtifactSetManifestPath = '',
    [string]$ExportSourceCommit = '',
    [string]$ExportCohortNonce = '',
    [string]$ExportCohortCreatedUtc = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Sha256 {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return (($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $algorithm.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-Sha256Text {
    param([string]$Value)
    $encoding = New-Object Text.UTF8Encoding($false)
    $bytes = $encoding.GetBytes($Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Write-Json {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 20))
}

function Read-TestJson {
    param([string]$Path)
    $json = Get-Content -LiteralPath $Path -Raw
    $convertFromJson = Get-Command ConvertFrom-Json
    if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
        return $json | ConvertFrom-Json -DateKind String
    }
    return $json | ConvertFrom-Json
}

function Get-Stage5HostFixtureDirectoryName {
    param([string]$Name)
    Assert-True (-not [string]::IsNullOrWhiteSpace($Name)) `
        'Host fixture directory names must not be empty.'
    $reviewedNames = @{
        'prelaunch-cleanup-publication' = 'cleanup-pub'
        'prelaunch-plan-rejections' = 'plan-reject'
        'prelaunch-role-environment' = 'role-env'
        'prelaunch-native-files' = 'native-files'
        'prelaunch-receipt-snapshot' = 'receipt-snap'
        'prelaunch-publication-faults' = 'pub-faults'
        'prelaunch-journal-reader' = 'journal-read'
        'prelaunch-create-new' = 'create-new'
        'prelaunch-identity-plan' = 'identity-plan'
        'prelaunch-start-info' = 'start-info'
        'prelaunch-attempt-failure' = 'attempt-fail'
        'phase-selected-cohort' = 'phase-cohort'
        'phase-host-boundaries' = 'phase-host'
        'reference-range-outside-matching-timing' = 'ref-range'
        'phase-bounded-trace' = 'trace-bound'
        'phase-v6-footer-repeated-control' = 'v6-repeat'
    }
    if ($reviewedNames.ContainsKey($Name)) { return $reviewedNames[$Name] }
    $safe = $Name -replace '[^A-Za-z0-9_-]', '-'
    if ($safe.Length -le 24) { return $safe }
    # Keep unlisted future mutation labels unique while bounding their path
    # contribution.  GetHashCode is intentionally avoided because its result
    # is not a stable cross-process contract on every supported CLR.
    $prefix = if ($safe.Length -gt 12) { $safe.Substring(0, 12) } else { $safe }
    return ($prefix + '-' + (Get-Sha256Text $Name).Substring(0, 8))
}

function Test-Stage5HostFixturePathBudget {
    # Keep a deliberately long H: parent so this regression catches fixture
    # labels that only work from a short local path.  The native title profile
    # contract rejects paths at 248 characters, including both supported title
    # profile leaves.
    $stressRoot = 'H:\' + ('x' * 168)
    $profileLeaves = @(
        'TitleSession\Documents\GGC-LockstepV2-ZeroHour',
        'TitleSession\Documents\Command and Conquer Generals Data')
    $cases = @(
        @{ name = 'prelaunch-cleanup-publication'; compact = 'cleanup-pub' },
        @{ name = 'phase-host-boundaries'; compact = 'phase-host' },
        @{ name = 'reference-range-outside-matching-timing'; compact = 'ref-range' })
    foreach ($case in $cases) {
        $legacyRoot = Join-Path $stressRoot $case.name
        $legacyProfiles = @($profileLeaves | ForEach-Object {
            Join-Path $legacyRoot $_
        })
        Assert-True (@($legacyProfiles | Where-Object {
            [IO.Path]::GetFullPath($_).Length -ge 248
        }).Count -gt 0) `
            "The long fixture label '$($case.name)' must reproduce the native 248-character boundary for at least one title."
        $compact = Get-Stage5HostFixtureDirectoryName $case.name
        Assert-True ($compact -ceq $case.compact) `
            "Fixture label '$($case.name)' must use its reviewed compact directory name '$($case.compact)'."
        $compactRoot = Join-Path $stressRoot $compact
        foreach ($profileLeaf in $profileLeaves) {
            $compactProfile = Join-Path $compactRoot $profileLeaf
            Assert-True ([IO.Path]::GetFullPath($compactProfile).Length -lt 248) `
                "Compacted fixture label '$compact' must leave every title profile below the native path boundary."
        }
    }
}

function Test-Stage5UnsafePathDiagnostic {
    $tooLongProfile = 'H:\' + ('x' * 245)
    $message = ''
    try {
        Get-Stage5ProfileTreeHash $tooLongProfile | Out-Null
    }
    catch { $message = $_.Exception.Message }
    Assert-True ($message -match '(?i)unsafe|too long' -and
        $message -notmatch '(?i)disappeared|not found') `
        'An unsafe or too-long title profile must be diagnosed as a bounded-path rejection, not as a missing path.'
}

function New-ArtifactSetFixture {
    param([string]$Root, [string]$SourceCommit)
    $artifactRoot = Join-Path $Root 'artifacts'
    New-Item -ItemType Directory -Path $artifactRoot | Out-Null
    $dependencyEntries = @()
    $artifactEntries = @()
    $products = @(
        [pscustomobject]@{ title = 'Generals'; executable = 'generalsv.exe' },
        [pscustomobject]@{ title = 'ZeroHour'; executable = 'generalszh.exe' })
    foreach ($product in $products) {
        $productRoot = Join-Path $artifactRoot $product.title
        New-Item -ItemType Directory -Path $productRoot | Out-Null
        $files = @(
            [pscustomobject]@{
                kind = 'executable'; name = $product.executable
                content = "stage5 synthetic $($product.title) executable"
            },
            [pscustomobject]@{
                kind = 'launcher'; name = 'launcher.exe'
                content = "stage5 synthetic $($product.title) launcher"
            },
            [pscustomobject]@{
                kind = 'launcher-config'; name = 'launcher.lcf'
                content = "RUN = . $($product.executable)`r`n"
            },
            [pscustomobject]@{
                kind = 'dll'; name = 'runtime.dll'
                content = "stage5 synthetic $($product.title) runtime dependency"
            },
            [pscustomobject]@{
                kind = 'asset'; name = 'asset.big'
                content = "stage5 synthetic $($product.title) asset"
            })
        foreach ($file in $files) {
            $path = Join-Path $productRoot $file.name
            [IO.File]::WriteAllText($path, $file.content)
            $relative = ('artifacts/{0}/{1}' -f $product.title, $file.name)
            $hash = Get-Sha256 $path
            $dependencyEntries += [ordered]@{
                title = $product.title; kind = $file.kind
                path = $relative; sha256 = $hash
            }
            if ($file.kind -ceq 'executable' -or
                $file.kind -ceq 'launcher' -or
                $file.kind -ceq 'launcher-config') {
                $rolePrefix = if ($product.title -ceq 'Generals') {
                    'generals'
                } else { 'zerohour' }
                $roleSuffix = switch ($file.kind) {
                    'executable' { 'executable' }
                    'launcher' { 'launcher' }
                    default { 'launcher-config' }
                }
                $artifactEntries += [ordered]@{
                    role = "$rolePrefix-$roleSuffix"
                    path = $relative; sha256 = $hash
                }
            }
        }
    }
    $dependencyManifestPath = Join-Path $Root 'runtime-dependencies.json'
    $dependencyManifest = [ordered]@{
        schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
        files = $dependencyEntries
    }
    Write-Json $dependencyManifestPath $dependencyManifest
    $dependencyHash = Get-Sha256 $dependencyManifestPath
    [string[]]$canonicalLines = @($dependencyEntries | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.kind,
            ([string]$_.path).Replace('\', '/'), ([string]$_.sha256).ToUpperInvariant()
    })
    [Array]::Sort($canonicalLines, [StringComparer]::Ordinal)
    $closureHash = Get-Sha256Text (($canonicalLines -join "`n") + "`n")
    $artifactSetPath = Join-Path $Root 'artifact-set.json'
    $artifactSet = [ordered]@{
        schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
        artifacts = $artifactEntries
        runtimeClosure = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'runtime-dependencies.json'; sha256 = $dependencyHash
            }
            closureSha256 = $closureHash
        }
    }
    Write-Json $artifactSetPath $artifactSet
    $zeroHourExecutable = Join-Path $artifactRoot 'ZeroHour\generalszh.exe'
    return [pscustomobject]@{
        path = $artifactSetPath; sha256 = Get-Sha256 $artifactSetPath
        executablePath = $zeroHourExecutable
        executableHash = Get-Sha256 $zeroHourExecutable
        runtimeClosure = [pscustomobject]@{
            dependencyManifestPath = 'runtime-dependencies.json'
            dependencyManifestSha256 = $dependencyHash
            closureSha256 = $closureHash
        }
    }
}

function Write-RunReceipt {
    param([string]$Root, [string]$Executable, [string]$FixtureId,
        [string]$FixturePath, [string]$Lane, [int]$Workers, [int]$Ordinal,
        [int]$ProcessId, [double]$Elapsed, [int]$UnitCount,
        [string]$ExecutableHash, [string]$ArtifactSetHash,
        [string]$FixtureHash, [object]$RuntimeClosure,
        [string]$SourceCommit, [string]$CohortNonce,
        [string]$CohortCreatedUtc, [bool]$SerialKnown = $true,
        [string]$MeasurementRole = 'throughput',
        [string]$ReferenceMode = 'throughput-binding')
    $runId = "test-$FixtureId-$Lane-$Ordinal-$ProcessId"
    $runRoot = Join-Path $Root $runId
    New-Item -ItemType Directory -Path $runRoot | Out-Null
    $rawPath = Join-Path $runRoot 'raw.log'
    $timingPath = Join-Path $runRoot 'timing.csv'
    $receiptPath = Join-Path $runRoot "performance-receipt-$runId-$ProcessId.json"
    $argumentString = "-headless -noFPSLimit -pipelineMode serial -simulationMode parallel -workerPolicy auto -validationExecutableSha256 $ExecutableHash -workerCount $Workers -replay $FixturePath"
    $commandLine = "$Executable $argumentString"
    $creation = [Int64](133000000000000000 + $ProcessId)
    $runNonce = '00000000-0000-4000-8000-{0:D12}' -f $ProcessId
    $cohortNonce = $CohortNonce
    $cohortCreatedUtc = $CohortCreatedUtc
    # Preserve the default fixture timestamp while keeping explicitly supplied
    # fresh cohorts temporally coherent with their synthetic native receipts.
    $recordedUtc = ([DateTime]::Parse($cohortCreatedUtc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind)).AddSeconds(1).ToString(
            'o', [Globalization.CultureInfo]::InvariantCulture)
    $mask = if ($Workers -eq 16) { [UInt64]0xFFFF } else {
        ([UInt64]1 -shl $Workers) - 1
    }
    $cpuSets = @()
    for ($index = 0; $index -lt 16; ++$index) {
        $cpuSets += [ordered]@{
            id = $index + 1; efficiencyClass = 0; group = 0
            coreIndex = $index; logicalProcessorIndex = $index
            parked = $false; allocatedToOtherProcess = $false
            availableToProcess = $true
        }
    }
    $selected = @(1..$Workers)
    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    $phases = @()
    for ($phaseIndex = 0; $phaseIndex -lt $phaseNames.Count; ++$phaseIndex) {
        $phases += [ordered]@{
            name = $phaseNames[$phaseIndex]; available = $true
            totalNanoseconds = 1000 + $phaseIndex
            maximumNanoseconds = 1000 + $phaseIndex; sampleCount = 100
            serialNanoseconds = if ($SerialKnown) { 1 } else { 0 }
            serialNanosecondsKnown = $SerialKnown
        }
    }
    $kernelStreams = @()
    foreach ($kernelName in @('physics','status','collision','ai-planning',
        'spatial','path')) {
        $kernelStreams += [ordered]@{
            name = $kernelName; subtype = 0
            attemptedBatches = 1; admittedBatches = 1; committedBatches = 1
            abortedBatches = 0; firstFrame = 0; lastFrame = 100
            activePipelineNanoseconds = 50; inclusiveBatchNanoseconds = 100
            maximumBatchNanoseconds = 100
            stages = @(
                [ordered]@{ name = 'capture'; totalNanoseconds = 10; sampleCount = 1 }
                [ordered]@{ name = 'schedule'; totalNanoseconds = 10; sampleCount = 1 }
                [ordered]@{ name = 'wait'; totalNanoseconds = 10; sampleCount = 1 }
                [ordered]@{ name = 'validate'; totalNanoseconds = 10; sampleCount = 1 }
                [ordered]@{ name = 'commit'; totalNanoseconds = 10; sampleCount = 1 }
            )
        }
    }
    $kernelReferenceStreams = @()
    if ($SerialKnown) {
        $referenceSerialSamples = if ($ReferenceMode -ceq 'serial-oracle') { 1 } else { 0 }
        $referenceSerialNanoseconds = if ($ReferenceMode -ceq 'serial-oracle') { 100 } else { 0 }
        foreach ($kernelName in @('physics','status','collision','ai-planning',
            'spatial','path')) {
            $kernelReferenceStreams += [ordered]@{
                name = $kernelName; subtype = 0; fieldSchema = 1
                firstFrame = 0; lastFrame = 100
                validatedBatchCount = 1; committedBatchCount = 1; abortedBatchCount = 0
                validatedOperationCount = 1; committedOperationCount = 1
                serialSampleCount = $referenceSerialSamples
                serialNanoseconds = $referenceSerialNanoseconds
                maximumSerialNanoseconds = $referenceSerialNanoseconds
                inputSha256 = ('11' * 32); outputSha256 = ('22' * 32)
                commitSha256 = ('33' * 32)
            }
        }
    }
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial',
        'path')
    $kernels = @()
    for ($kernelIndex = 0; $kernelIndex -lt $kernelNames.Count; ++$kernelIndex) {
        $kernels += [ordered]@{
            name = $kernelNames[$kernelIndex]; available = $true
            submittedJobs = $Workers; completedJobs = $Workers
            physicalWorkerJobs = $Workers
            ownerHelpedJobs = 0; physicalWorkerMask = $mask
            distinctPhysicalWorkers = $Workers; physicalWorkerMaskComplete = $true
            # The executable does not expose authoritative aggregate timing for
            # individual kernels.  Timing truth comes from the complete 6-8
            # stream ledger below, so keep this fixture production-shaped.
            elapsedNanoseconds = 0
            elapsedNanosecondsKnown = $false
        }
    }
    $receipt = [ordered]@{
        schemaVersion = 5
        producer = 'game-executable-stage5-performance-report-v5'
        evidenceKind = 'stage5-executable-originated-receipt'
        status = 'passed'
        role = 'performance-report'; producerVersion = '5'
        measurementRole = $MeasurementRole
        simulationMode = 'parallel'; schedulerStarted = $true
        title = 'ZeroHour'
        runId = $runId
        runNonce = $runNonce; cohortNonce = $cohortNonce
        cohortCreatedUtc = $cohortCreatedUtc; recordedUtc = $recordedUtc
        architecture = 'x64'
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetHash
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $RuntimeClosure.dependencyManifestSha256
            closureSha256 = $RuntimeClosure.closureSha256
        }
        executablePath = $Executable
        executableSha256 = $ExecutableHash
        commandLine = $commandLine
        process = [ordered]@{
            id = $ProcessId; creationTimeUtc100ns = $creation
            startTimeUtc100ns = $creation + 1; endTimeUtc100ns = $creation + 2
            identityAvailable = $true; exitCodeKnown = $true; exitCode = 0
            exitBoundary = 'ReplaySimulation::simulateReplaysInThisProcess:return'
        }
        fixture = [ordered]@{
            id = $FixtureId; kind = 'replay'
            workloadQualification = 'minimum-qualified'
            contentPath = $FixturePath; identityObserved = $true
            contentSha256 = $FixtureHash; replayPath = $FixturePath
            retainedReplayPath = ''; retainedReplaySha256 = ''
            seed = 7; seedKnown = $true; requestedPlayerCount = 8
            requestedMinimumUnitCount = $UnitCount
        }
        workload = [ordered]@{
            sampling = 'completed-simulation-frame-boundary-v1'
            sampleCount = 100; firstFrame = 1; lastFrame = 100
            playerCount = 8; rosterStable = $true; contiguous = $true
            initialUnitCount = $UnitCount; minimumUnitCount = $UnitCount
            peakUnitCount = $UnitCount
        }
        frameSimulation = [ordered]@{
            totalNanoseconds = 10000; maximumNanoseconds = 2000
            sampleCount = 100
        }
        frames = [ordered]@{
            start = 0; end = 100; final = 100; finalCrcKnown = $true
            finalCrc = 305419896
        }
        worker = [ordered]@{
            requestedCount = $Workers; effectiveCount = $Workers; policy = 'auto'
            pinned = $true; availableLogicalCpuCount = 16; reservedOwnerCpuCount = 0
            selectedWorkerCpuCount = $Workers
            selectedWorkerPhysicalCoreCount = $Workers
            selectedWorkerPhysicalCoreMask = $mask
            selectedWorkerPhysicalCoreMaskComplete = $true
        }
        topology = [ordered]@{
            source = 'GetSystemCpuSetInformation'; cpuSets = $cpuSets
            ownerCpuSetIds = @(); selectedWorkerCpuSetIds = $selected
        }
        rawEvidence = [ordered]@{
            verifierBoundary = 'stage5-host-independent-correlation-v1'
            rawLogPath = $rawPath; rawLogSha256 = ''
            timingPath = $timingPath; timingSha256 = ''
            timingClosed = $true; timingWriteSucceeded = $true
            timingTruncated = $false; timingComplete = $true
            timingSessionCount = 1; timingFrameSamples = 100
            timingFirstFrame = 0; timingLastFrame = 100
        }
        rawLogs = @(
            [ordered]@{ name = 'raw-log'; path = $rawPath; sha256 = '' }
            [ordered]@{ name = 'timing'; path = $timingPath; sha256 = '' }
        )
        provenance = [ordered]@{
            kind = 'native-executable-observation'; receiptPath = $receiptPath
            processId = $ProcessId
            processCreationUtc = ([DateTimeOffset]::FromFileTime($creation).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ss.fffffff') + 'Z')
            executablePath = $Executable; executableSha256 = $ExecutableHash
            commandLine = $commandLine; exitCode = 0
        }
        schedulerMetrics = [ordered]@{
            submittedJobCount = 8; executedJobCount = 8; stealCount = 0
            ownerHelpCount = 0; waitCount = 0; workerWaitRejectionCount = 0
            failedJobCount = 0; cancelledJobCount = 0; serialFallbackCount = 0
            totalQueueLatencyNanoseconds = 0; maximumQueueLatencyNanoseconds = 0
            workerBusyNanoseconds = 2000; workerWaitNanoseconds = 0
            affinityFailureCount = 0; injectionHighWater = 1
            maximumActiveWorkers = $Workers; availableLogicalCpuCount = 16
            reservedOwnerCpuCount = 0; selectedWorkerCpuCount = $Workers
            selectedWorkerPhysicalCoreCount = $Workers
            selectedWorkerPhysicalCoreMask = $mask
            selectedWorkerPhysicalCoreMaskComplete = $true
        }
        phases = $phases
        kernels = $kernels
        kernelTiming = [ordered]@{
            schemaVersion = 1; mode = 'owner-pipeline-observation'
            attribution = 'owner-stack-exclusive-v1'; enabled = $true; frozen = $true
            complete = ($kernelStreams.Count -ne 0); errors = 0; generation = 1
            serialReferenceKnown = $false; streams = $kernelStreams
        }
        kernelReference = [ordered]@{
            schemaVersion = 1; mode = $ReferenceMode
            frozen = $true; complete = ($kernelReferenceStreams.Count -ne 0)
            errors = 0; generation = 1; streams = $kernelReferenceStreams
        }
    }
    $raw = @(
        'producer=game-executable-performance-receipt-v5', 'game_owned=1',
        "run_id=$runId", "process_id=$ProcessId",
        "process_creation_time_utc_100ns=$creation",
        "executable_sha256=$ExecutableHash", "command_line=$commandLine",
        "fixture_id=$FixtureId", "fixture_sha256=$FixtureHash", 'frame=100',
        'final_crc=12345678', 'close_boundary=game-owned-raw-diagnostic-closed-v1'
    ) -join [Environment]::NewLine
    [IO.File]::WriteAllText($rawPath, $raw)
    [IO.File]::WriteAllText($timingPath, "frame,logic_ns`r`n1,100`r`n")
    $receipt.rawEvidence.rawLogSha256 = Get-Sha256 $rawPath
    $receipt.rawEvidence.timingSha256 = Get-Sha256 $timingPath
    $receipt.rawLogs[0].sha256 = $receipt.rawEvidence.rawLogSha256
    $receipt.rawLogs[1].sha256 = $receipt.rawEvidence.timingSha256
    Write-Json $receiptPath $receipt
    return [ordered]@{
        fixtureId = $FixtureId; lane = $Lane; ordinal = $Ordinal
        warmup = ($Ordinal -eq 0); runId = $runId; runNonce = $runNonce
        expectedArgumentString = $argumentString
        receiptPath = $receiptPath; receiptSha256 = Get-Sha256 $receiptPath
        host = [ordered]@{
            processId = $ProcessId; creationTimeUtc100ns = $creation
            executablePath = $Executable; executableSha256 = $ExecutableHash
            commandLine = $commandLine; argumentString = $argumentString
            parentProcessId = 0; parentCreationTimeUtc100ns = [Int64]0
            exitCode = 0; elapsedMilliseconds = $Elapsed
            rawLogSha256 = Get-Sha256 $rawPath
            timingSha256 = Get-Sha256 $timingPath
        }
    }
}

function New-ValidationFixture {
    param([string]$Root, [string]$Mode = 'External16Core',
        [object]$ArtifactBinding = $null,
        [string]$SourceCommit = ('a' * 40),
        [string]$FixtureCohortNonce = '',
        [string]$FixtureCohortCreatedUtc = '')
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        New-Item -ItemType Directory -Path $Root | Out-Null
    }
    if ($null -eq $ArtifactBinding) {
        $ArtifactBinding = New-ArtifactSetFixture $Root $SourceCommit
    }
    $executable = $artifactBinding.executablePath
    $fixtureIds = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $unitCounts = @(1000, 4000, 8000, 12000)
    $fixtures = @()
    $stage3 = @()
    $runs = New-Object 'Collections.Generic.List[object]'
    $processId = 20000
    $cohortNonce = if ([string]::IsNullOrWhiteSpace($FixtureCohortNonce)) {
        '00000000-0000-4000-8000-000000000001'
    } else { $FixtureCohortNonce }
    $cohortCreatedUtc = if ([string]::IsNullOrWhiteSpace($FixtureCohortCreatedUtc)) {
        '2026-09-01T00:00:00.0000000Z'
    } else { $FixtureCohortCreatedUtc }
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $fixturePath = Join-Path $Root "$($fixtureIds[$fixtureIndex]).rep"
        [IO.File]::WriteAllText($fixturePath, "fixture-$fixtureIndex")
        $fixtureHash = Get-Sha256 $fixturePath
        $fixtures += [ordered]@{
            id = $fixtureIds[$fixtureIndex]; path = $fixturePath
            sha256 = $fixtureHash; seed = 7; playerCount = 8
            peakUnitCount = $unitCounts[$fixtureIndex]
        }
        if ($Mode -ceq 'External16Core') {
            $stage3 += [ordered]@{
                id = $fixtureIds[$fixtureIndex]; measuredMedianMilliseconds = 100.0
            }
        }
        $lanes = if ($Mode -ceq 'LocalCapacitySmoke') {
            @(
                [pscustomobject]@{ name = 'forced-one'; workers = 1; elapsed = 100.0 },
                [pscustomobject]@{ name = 'physical-2'; workers = 2; elapsed = 60.0 },
                [pscustomobject]@{ name = 'physical-4'; workers = 4; elapsed = 40.0 })
        } else {
            @(
                [pscustomobject]@{ name = 'forced-one'; workers = 1; elapsed = 100.0 },
                [pscustomobject]@{ name = 'physical-8'; workers = 8; elapsed = 40.0 },
                [pscustomobject]@{ name = 'physical-16'; workers = 16; elapsed = 30.0 })
        }
        foreach ($lane in $lanes) {
            for ($ordinal = 0; $ordinal -lt 4; ++$ordinal) {
                ++$processId
                $runs.Add((Write-RunReceipt $Root $executable $fixtureIds[$fixtureIndex] `
                    $fixturePath $lane.name $lane.workers $ordinal $processId `
                    $lane.elapsed $unitCounts[$fixtureIndex] `
                    $artifactBinding.executableHash $artifactBinding.sha256 $fixtureHash `
                    $artifactBinding.runtimeClosure $sourceCommit `
                    $cohortNonce $cohortCreatedUtc `
                    ($Mode -ceq 'External16Core'))) | Out-Null
            }
        }
    }
    $topologyCpuSets = @(0..15 | ForEach-Object {
        [ordered]@{
            id = $_ + 1; efficiencyClass = 0; group = 0
            coreIndex = $_; logicalProcessorIndex = $_
            parked = $false; allocated = $false; available = $true
        }
    })
    $document = [ordered]@{
        schemaVersion = 1; title = 'ZeroHour'; qualificationMode = $Mode
        stage3SourceCommit = if ($Mode -ceq 'External16Core') { ('b' * 40) } else { '' }
        sourceCommit = $sourceCommit
        artifactSetSha256 = $artifactBinding.sha256
        artifactSetManifestPath = $artifactBinding.path
        executablePath = $executable
        executableSha256 = $artifactBinding.executableHash
        fixtureManifestSha256 = ('D' * 64)
        cohortNonce = $cohortNonce
        cohortCreatedUtc = $cohortCreatedUtc
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $artifactBinding.runtimeClosure.dependencyManifestSha256
            closureSha256 = $artifactBinding.runtimeClosure.closureSha256
        }
        stage3BaselineSha256 = if ($Mode -ceq 'External16Core') { ('E' * 64) } else { '' }
        taskRoot = $Root
        warmupRuns = 1; measuredRuns = 3; fixtures = $fixtures
        stage3Fixtures = $stage3
        topology = [ordered]@{
            source = 'GetSystemCpuSetInformation'
            physicalCoreCount = if ($Mode -ceq 'External16Core') { 16 } else { 6 }
            logicalProcessorCount = if ($Mode -ceq 'External16Core') { 16 } else { 12 }
            cpuSets = $topologyCpuSets
        }
        runs = $runs.ToArray()
    }
    $manifest = Join-Path $Root 'validation.json'
    Write-Json $manifest $document
    return $manifest
}

function New-PairedValidationFixture {
    param([string]$Root, [object]$ArtifactBinding = $null,
        [string]$SourceCommit = ('a' * 40))
    $manifest = New-ValidationFixture $Root 'External16Core' `
        $ArtifactBinding $SourceCommit
    $document = Read-TestJson $manifest
    $oracleBindings = New-Object 'Collections.Generic.List[object]'
    $oracleProcessId = 80000
    foreach ($throughputRun in @($document.runs)) {
        ++$oracleProcessId
        $fixture = @($document.fixtures | Where-Object {
            $_.id -ceq $throughputRun.fixtureId
        })[0]
        $workers = switch ([string]$throughputRun.lane) {
            'forced-one' { 1; break }
            'physical-8' { 8; break }
            'physical-16' { 16; break }
            default { throw "Unexpected paired fixture lane '$($throughputRun.lane)'." }
        }
        $oracleRun = Write-RunReceipt $Root $document.executablePath `
            $throughputRun.fixtureId $fixture.path $throughputRun.lane $workers `
            $throughputRun.ordinal $oracleProcessId 80.0 $fixture.peakUnitCount `
            $document.executableSha256 $document.artifactSetSha256 $fixture.sha256 `
            $document.runtimeClosure $document.sourceCommit $document.cohortNonce `
            $document.cohortCreatedUtc $true 'serial-oracle' 'serial-oracle'
        $oracleBindings.Add([ordered]@{
            throughputRunId = [string]$throughputRun.runId
            oracleRun = $oracleRun
        }) | Out-Null
    }
    $document | Add-Member -MemberType NoteProperty -Name referencePolicy `
        -Value 'paired-serial-oracle-v1' -Force
    $document | Add-Member -MemberType NoteProperty -Name pairedOracleBindings `
        -Value $oracleBindings.ToArray() -Force
    Write-Json $manifest $document
    return $manifest
}

function Copy-Stage5AuthoritativeArtifactFixture {
    param([string]$Root, [string]$ArtifactSetManifestPath,
        [string]$SourceCommit, [string]$Title = 'ZeroHour')
    $sourcePath = [IO.Path]::GetFullPath($ArtifactSetManifestPath)
    $sourceRoot = Split-Path -Parent $sourcePath
    $artifactSet = Read-TestJson $sourcePath
    Assert-True ($artifactSet.sourceCommit -ceq $SourceCommit -and
        $artifactSet.runtimeClosure.dependencyManifest.path -is [string]) `
        'Authoritative export artifact set does not match its requested source commit.'
    $copyFile = {
        param([string]$RelativePath)
        $source = [IO.Path]::GetFullPath((Join-Path $sourceRoot $RelativePath))
        $destination = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
        Assert-True ($source.StartsWith($sourceRoot.TrimEnd('\', '/') + '\',
                [StringComparison]::OrdinalIgnoreCase) -and
            $destination.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + '\',
                [StringComparison]::OrdinalIgnoreCase)) `
            "Authoritative export artifact path escaped its reviewed root: $RelativePath"
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) `
            -Force | Out-Null
        [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
        return $destination
    }
    $dependencyRelative = [string]$artifactSet.runtimeClosure.dependencyManifest.path
    $dependencySourcePath = Join-Path $sourceRoot $dependencyRelative
    $dependency = Read-TestJson $dependencySourcePath
    foreach ($entry in @($dependency.files)) {
        $copied = & $copyFile ([string]$entry.path)
        Assert-True ((Get-Sha256 $copied) -ceq [string]$entry.sha256) `
            "Authoritative export changed runtime artifact '$($entry.path)'."
    }
    $dependencyDestination = & $copyFile $dependencyRelative
    Assert-True ((Get-Sha256 $dependencyDestination) -ceq
            [string]$artifactSet.runtimeClosure.dependencyManifest.sha256) `
        'Authoritative export changed the runtime dependency manifest.'
    $artifactDestination = Join-Path $Root 'Stage5ArtifactSet.json'
    [IO.File]::WriteAllBytes($artifactDestination,
        [IO.File]::ReadAllBytes($sourcePath))
    $artifactHash = Get-Sha256 $artifactDestination
    $role = if ($Title -ceq 'Generals') { 'generals-executable' }
        else { 'zerohour-executable' }
    $executableEntry = @($artifactSet.artifacts | Where-Object {
        $_.role -ceq $role
    })
    Assert-True ($executableEntry.Count -eq 1) `
        "Authoritative export artifact set lacks exact role '$role'."
    $executablePath = Join-Path $Root ([string]$executableEntry[0].path)
    Assert-True ((Get-Sha256 $executablePath) -ceq
            [string]$executableEntry[0].sha256) `
        'Authoritative export executable bytes changed.'
    return [pscustomobject]@{
        path = $artifactDestination
        sha256 = $artifactHash
        executablePath = [IO.Path]::GetFullPath($executablePath)
        executableHash = [string]$executableEntry[0].sha256
        runtimeClosure = [pscustomobject]@{
            dependencyManifestPath = $dependencyRelative
            dependencyManifestSha256 =
                [string]$artifactSet.runtimeClosure.dependencyManifest.sha256
            closureSha256 = [string]$artifactSet.runtimeClosure.closureSha256
            fileCount = @($dependency.files).Count
        }
    }
}

function New-Stage5AuthoritativeValidationFixture {
    param([string]$Root, [string]$ArtifactSetManifestPath,
        [string]$SourceCommit, [string]$CohortNonce,
        [string]$CohortCreatedUtc)
    New-Item -ItemType Directory -Path $Root | Out-Null
    $artifactBinding = Copy-Stage5AuthoritativeArtifactFixture $Root `
        $ArtifactSetManifestPath $SourceCommit 'ZeroHour'
    $manifest = New-PairedValidationFixture $Root $artifactBinding $SourceCommit
    $document = Read-TestJson $manifest
    $document.cohortNonce = $CohortNonce
    $document.cohortCreatedUtc = $CohortCreatedUtc
    foreach ($run in @($document.runs) +
            @($document.pairedOracleBindings | ForEach-Object { $_.oracleRun })) {
        $receipt = Read-TestJson $run.receiptPath
        $receipt.cohortNonce = $CohortNonce
        $receipt.cohortCreatedUtc = $CohortCreatedUtc
        Write-Json $run.receiptPath $receipt
        $run.receiptSha256 = Get-Sha256 $run.receiptPath
    }

    $dataRuntimeRoot = Split-Path -Parent $document.executablePath
    $dataRows = @()
    $dataFilePaths = @()
    foreach ($relative in @('Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini', 'Data/Scripts/SkirmishScripts.scb',
            'INIZH.big', 'MapsZH.big', 'W3DZH.big')) {
        $dataPath = Join-Path $dataRuntimeRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $dataPath) `
            -Force | Out-Null
        [IO.File]::WriteAllText($dataPath,
            "synthetic Stage 5 qualification data:$relative")
        $dataRows += [ordered]@{
            path = $relative; sha256 = Get-Sha256 $dataPath
            length = [Int64](Get-Item -LiteralPath $dataPath).Length
        }
        $dataFilePaths += [IO.Path]::GetFullPath($dataPath)
    }
    $dataLines = @($dataRows | ForEach-Object {
        '{0}|{1}|{2}' -f $_.path, $_.sha256, $_.length
    })
    $dataClosureSha256 = Get-Sha256Text (($dataLines -join "`n") + "`n")
    $performanceDataPath = Join-Path $Root `
        'Stage5PerformanceQualificationData.json'
    Write-Json $performanceDataPath ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-performance-qualification-data'
        producer = 'genci-r2-trimmed-data-v1'
        sourceCommit = $SourceCommit
        title = 'ZeroHour'
        archiveSource = [ordered]@{
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
        runtimeRoot = $dataRuntimeRoot
        files = $dataRows
        closureSha256 = $dataClosureSha256
    })
    $performanceData = [pscustomobject][ordered]@{
        sourceManifestPath = $performanceDataPath
        path = $performanceDataPath
        sha256 = Get-Sha256 $performanceDataPath
        closureSha256 = $dataClosureSha256
        runtimeRoot = $dataRuntimeRoot
        fileCount = $dataRows.Count
        filePaths = @($dataFilePaths)
    }
    $document | Add-Member NoteProperty performanceData $performanceData -Force

    $limits = [pscustomobject][ordered]@{
        maximumBytes = [UInt64]1048576
        maximumRecords = [UInt64]100000
        maximumLogicalEvents = [UInt64]100000
        maximumAttempts = [UInt64]10000
        maximumRanges = [UInt64]500000
    }
    $profile = [pscustomobject][ordered]@{
        profileId = 'dense-external-forced-one'
        fixtureId = 'dense-eight-player'
        sourceLane = 'forced-one'
        sourcePolicySha256 = ('7A' * 32)
        limits = $limits
        residentAttemptCapacity = [UInt64]15
        residentRangeCapacity = [UInt64]340
        fixtureSha256 = [string]$document.fixtures[3].sha256
        window = [pscustomobject][ordered]@{
            firstCompletedFrame = [UInt64]1
            lastCompletedFrame = [UInt64]100
            completedFrameCount = [UInt64]100
            controlWindowCount = [UInt64]1
        }
        warmupRuns = 1
        measuredRuns = 3
    }
    $document | Add-Member NoteProperty phaseBaselinePolicy `
        'paired-source-admissions-v1' -Force
    $document | Add-Member NoteProperty phaseBaselineProfiles @($profile) -Force

    $pairs = @()
    $planEntries = @()
    foreach ($run in $document.runs) {
        $planEntries += [pscustomobject]@{
            entryId = $run.runId; measurementRole = 'throughput'
            profileId = $null; fixtureId = $run.fixtureId; lane = $run.lane
            ordinal = $run.ordinal; warmup = $run.warmup; sourceEntryId = $null
        }
        $oracle = @($document.pairedOracleBindings | Where-Object {
            $_.throughputRunId -ceq $run.runId
        })[0].oracleRun
        $planEntries += [pscustomobject]@{
            entryId = $oracle.runId; measurementRole = 'serial-oracle'
            profileId = $null; fixtureId = $oracle.fixtureId; lane = $oracle.lane
            ordinal = $oracle.ordinal; warmup = $oracle.warmup
            sourceEntryId = $run.runId
        }
    }
    $baselineProcessId = 90000
    $sourceRuns = @($document.runs | Where-Object {
        $_.fixtureId -ceq 'dense-eight-player' -and $_.lane -ceq 'forced-one'
    } | Sort-Object ordinal)
    foreach ($sourceRun in $sourceRuns) {
        $ordinal = [int]$sourceRun.ordinal
        $source = Read-TestJson $sourceRun.receiptPath
        $tracePath = Join-Path (Split-Path -Parent $sourceRun.receiptPath) `
            "synthetic-wire-trace-$ordinal.bin"
        $traceBytes = New-Object byte[] 4096
        $traceMarker = [Text.Encoding]::ASCII.GetBytes(
            "authoritative-host-wire-trace-$ordinal")
        [Array]::Copy($traceMarker, $traceBytes, $traceMarker.Length)
        [IO.File]::WriteAllBytes($tracePath, $traceBytes)
        $trace = [pscustomobject][ordered]@{
            schemaVersion = 1; encoding = 'typed-canonical-le-v1'
            fieldSchema = 20481; mode = 'record'; frozen = $true
            complete = $true; errors = 0; observationIngressSealed = $true
            executionClosureSealed = $true
            file = [pscustomobject]@{
                path = $tracePath; sha256 = Get-Sha256 $tracePath
                byteCount = [UInt64]([IO.FileInfo]$tracePath).Length
            }
            binding = [pscustomobject]@{
                nativeRunIdentitySha256 = Get-Stage5PhaseTestRunIdentitySha256 $source
                executableSha256 = $source.executableSha256
                fixtureSha256 = $source.fixture.contentSha256
                sourcePolicySha256 = $profile.sourcePolicySha256
            }
            limits = $limits; residentAttemptCapacity = [UInt64]15
            residentRangeCapacity = [UInt64]340; residentAttemptCount = [UInt64]0
            residentAttemptHighWater = [UInt64]2; residentRangeCount = [UInt64]0
            residentRangeHighWater = [UInt64]3; recordCount = [UInt64]101
            logicalEventCount = [UInt64]101; windowBoundaryCount = [UInt64]101
            completedWindowCount = [UInt64]100; controlWindowCount = [UInt64]1
            coalescedSpanCount = [UInt64]0; coalescedAttemptCount = [UInt64]0
            attemptCount = [UInt64]4; admittedAttemptCount = [UInt64]2
            notAdmittedAttemptCount = [UInt64]2
            abortedAfterAdmissionAttemptCount = [UInt64]1; reapCount = [UInt64]4
            capturedAttemptCount = [UInt64]2; capturedOperationCount = [UInt64]5
            dispatchCount = [UInt64]2; rangeCount = [UInt64]5
            releasedRangeCount = [UInt64]5; sourceBinding = $null
        }
        $source | Add-Member NoteProperty attemptTrace $trace -Force
        foreach ($phase in $source.phases) {
            $phase.serialNanoseconds = [UInt64]0
            $phase.serialNanosecondsKnown = $false
            if ($phase.PSObject.Properties.Name -notcontains
                    'pureNanoseconds') {
                $phase | Add-Member NoteProperty pureNanoseconds ([UInt64]0)
                $phase | Add-Member NoteProperty pureNanosecondsKnown $false
            }
            else {
                $phase.pureNanoseconds = [UInt64]0
                $phase.pureNanosecondsKnown = $false
            }
        }
        Set-Stage5V6WireRun $sourceRun $source
        $source = Read-TestJson $sourceRun.receiptPath

        ++$baselineProcessId
        $baselineRun = Write-RunReceipt $Root $document.executablePath `
            $sourceRun.fixtureId $document.fixtures[3].path 'forced-one' 1 `
            $ordinal $baselineProcessId (1000000.0 + $ordinal) `
            $document.fixtures[3].peakUnitCount $document.executableSha256 `
            $document.artifactSetSha256 $document.fixtures[3].sha256 `
            $document.runtimeClosure $document.sourceCommit $document.cohortNonce `
            $document.cohortCreatedUtc $true 'phase-serial-baseline' `
            'phase-baseline-binding'
        $baseline = Read-TestJson $baselineRun.receiptPath
        $baseline.kernelTiming.mode = 'owner-inline-baseline-observation'
        $baseline.kernelTiming.attribution = 'owner-inline-baseline-exclusive-v1'
        $baseline.kernelReference.mode = 'phase-baseline-binding'
        foreach ($stream in $baseline.kernelTiming.streams) {
            $stream.stages[2].totalNanoseconds = 0
            $stream.stages[2].sampleCount = 0
            $stream.activePipelineNanoseconds = 40
        }
        $worldRows = @(New-Stage5PhaseWireRows `
            @([UInt64]100,[UInt64]100,[UInt64]100,[UInt64]100,[UInt64]100) `
            @([UInt64]10,[UInt64]10,[UInt64]10,[UInt64]10,[UInt64]10) 100)
        $controlRows = @(New-Stage5PhaseWireRows `
            @([UInt64]20,[UInt64]20,[UInt64]20,[UInt64]20,[UInt64]20) `
            @([UInt64]2,[UInt64]2,[UInt64]2,[UInt64]2,[UInt64]2) 1)
        $baseline.phases = $worldRows
        $baseline.workload.sampleCount = [UInt64]100
        $baseline.workload.firstFrame = [UInt64]1
        $baseline.workload.lastFrame = [UInt64]100
        $baseline.schedulerMetrics.submittedJobCount = [UInt64]90
        $baseline.schedulerMetrics.executedJobCount = [UInt64]90
        $baseline.schedulerMetrics.ownerHelpCount = [UInt64]7
        $baseline | Add-Member NoteProperty phaseAccounting `
            ([pscustomobject][ordered]@{
                schemaVersion = 1; mode = 'owner-inline-source-admissions-v1'
                accountingOrigin = 'kernel-performance-ledger-whole-frame-v1'
                frozen = $true; complete = $true; errors = [UInt64]0
                completedFrameCount = [UInt64]100; firstCompletedFrame = [UInt64]1
                lastCompletedFrame = [UInt64]100; frameNanoseconds = [UInt64]500
                maximumFrameNanoseconds = [UInt64]5
                unscopedSerialNanoseconds = [UInt64]0
                completionSerialNanoseconds = [UInt64]20
                completionSampleCount = [UInt64]1; schedulerClosureKnown = $true
                schedulerBegin = [pscustomobject]@{
                    submittedJobs=[UInt64]90; executedJobs=[UInt64]90
                    ownerHelpJobs=[UInt64]7; outstandingJobs=[UInt64]0
                    pendingJobs=[UInt64]0
                }
                schedulerEnd = [pscustomobject]@{
                    submittedJobs=[UInt64]90; executedJobs=[UInt64]90
                    ownerHelpJobs=[UInt64]7; outstandingJobs=[UInt64]0
                    pendingJobs=[UInt64]0
                }
                controlAccounting = [pscustomobject][ordered]@{
                    windowCount=[UInt64]1; firstSampleOrdinal=[UInt64]1
                    lastSampleOrdinal=[UInt64]1; totalNanoseconds=[UInt64]100
                    maximumNanoseconds=[UInt64]100
                    unscopedSerialNanoseconds=[UInt64]0; phases=$controlRows
                }
            }) -Force
        $baseline | Add-Member NoteProperty attemptTrace $source.attemptTrace -Force
        $baseline.attemptTrace.mode = 'consume'
        $baseline.attemptTrace.sourceBinding = [pscustomobject][ordered]@{
            receipt = [pscustomobject]@{
                path = $sourceRun.receiptPath; sha256 = $sourceRun.receiptSha256
            }
            runId = $source.runId; runNonce = $source.runNonce
            processId = $source.process.id
            processCreationTimeUtc100ns = $source.process.creationTimeUtc100ns
        }
        foreach ($kernel in $baseline.kernels) {
            $kernel.submittedJobs = 0; $kernel.completedJobs = 0
            $kernel.physicalWorkerJobs = 0; $kernel.ownerHelpedJobs = 0
            $kernel.physicalWorkerMask = 0; $kernel.distinctPhysicalWorkers = 0
            $kernel.elapsedNanoseconds = 0; $kernel.elapsedNanosecondsKnown = $false
        }
        Set-Stage5V6WireRun $baselineRun $baseline
        $pairs += [pscustomobject]@{
            profileId = $profile.profileId
            throughputRunId = $sourceRun.runId
            baselineRun = $baselineRun
        }
        $planEntries += [pscustomobject]@{
            entryId = $baselineRun.runId
            measurementRole = 'phase-serial-baseline'
            profileId = $profile.profileId; fixtureId = $baselineRun.fixtureId
            lane = $baselineRun.lane; ordinal = $ordinal
            warmup = $baselineRun.warmup; sourceEntryId = $sourceRun.runId
        }
    }
    $document | Add-Member NoteProperty pairedPhaseBaselineBindings $pairs -Force
    $planPath = Join-Path $Root 'phase-plan.json'
    Write-Json $planPath ([ordered]@{
        schemaVersion = 1; cohortNonce = $document.cohortNonce
        executableSha256 = $document.executableSha256
        sourceCommit = $document.sourceCommit
        phaseBaselineProfiles = @($profile); entries = $planEntries
    })
    $attemptsPath = Join-Path $Root 'phase-attempts.json'
    Write-Json $attemptsPath ([ordered]@{
        schemaVersion = 1; planSha256 = Get-Sha256 $planPath
        outcomes = @(); cohortFailure = $null
    })
    $document | Add-Member NoteProperty phaseBaselinePlan `
        ([pscustomobject]@{ path=$planPath; sha256=(Get-Sha256 $planPath) }) -Force
    $document | Add-Member NoteProperty phaseBaselineAttemptManifest `
        ([pscustomobject]@{ path=$attemptsPath; sha256=(Get-Sha256 $attemptsPath) }) -Force
    Write-Json $manifest $document
    $completed = Complete-Stage5TestJournalFixture $manifest
    Register-Stage5TestDocumentInputLocks $completed.document
    return [pscustomobject]@{
        manifest = $manifest; document = $completed.document
        artifactBinding = $artifactBinding; performanceData = $performanceData
    }
}

function Update-Receipt {
    param([object]$Run, [scriptblock]$Mutation)
    $receipt = Read-TestJson $Run.receiptPath
    & $Mutation $receipt
    Write-Json $Run.receiptPath $receipt
    $Run.receiptSha256 = Get-Sha256 $Run.receiptPath
}

function Update-Stage5PhasePlanBinding {
    param([object]$Document)
    $Document.phaseBaselinePlan.sha256 = Get-Sha256 $Document.phaseBaselinePlan.path
    $attempts = Read-TestJson $Document.phaseBaselineAttemptManifest.path
    $attempts.planSha256 = $Document.phaseBaselinePlan.sha256
    Write-Json $Document.phaseBaselineAttemptManifest.path $attempts
    $Document.phaseBaselineAttemptManifest.sha256 =
        Get-Sha256 $Document.phaseBaselineAttemptManifest.path
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Mutation)
    $caseRoot = Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName $Name)
    $manifest = New-ValidationFixture $caseRoot
    $document = Read-TestJson $manifest
    & $Mutation $document
    Write-Json $manifest $document
    $rejected = $false
    try { & $runner -SelfTestValidationManifestPath $manifest | Out-Null }
    catch { $rejected = $true }
    Assert-True $rejected "Negative self-test '$Name' was not rejected."
}

function Assert-PairedRejected {
    param([string]$Name, [scriptblock]$Mutation)
    $caseRoot = Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName $Name)
    $manifest = New-PairedValidationFixture $caseRoot
    $document = Read-TestJson $manifest
    & $Mutation $document
    Write-Json $manifest $document
    $rejected = $false
    try { & $runner -SelfTestValidationManifestPath $manifest | Out-Null }
    catch { $rejected = $true }
    Assert-True $rejected "Negative paired self-test '$Name' was not rejected."
}

function New-Stage5PhaseWireRows {
    param([UInt64[]]$Totals, [UInt64[]]$Serial, [UInt64]$Samples)
    $names = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    for ($index = 0; $index -lt 5; ++$index) {
        [pscustomobject][ordered]@{
            name = $names[$index]; available = ($Samples -gt 0)
            totalNanoseconds = $Totals[$index]
            maximumNanoseconds = $Totals[$index]
            sampleCount = $Samples
            serialNanoseconds = $Serial[$index]
            serialNanosecondsKnown = $true
            pureNanoseconds = [UInt64]([decimal]$Totals[$index] - [decimal]$Serial[$index])
            pureNanosecondsKnown = $true
        }
    }
}

function New-Stage5PhaseAccountingWireFixture {
    param([int]$Control = 0, [int]$AnticorrelatedRepeat = -1)
    $worldRows = @(New-Stage5PhaseWireRows @(10,20,100,10,20) @(10,20,65,10,20) 1)
    $controlRows = @(New-Stage5PhaseWireRows @(0,0,0,0,0) @(0,0,0,0,0) 0)
    [UInt64]$frame = 200; [UInt64]$unscoped = 40; [UInt64]$completion = 70
    [UInt64]$controlTotal = 0; [UInt64]$controlMaximum = 0; [UInt64]$controlUnscoped = 0
    if ($Control -eq 1) {
        $controlRows = @(New-Stage5PhaseWireRows @(12,4,24,4,4) @(12,4,4,4,4) 1)
        $controlTotal = 50; $controlMaximum = 50; $controlUnscoped = 2
    }
    elseif ($Control -eq 2) {
        # These are independent literals from the native two-control
        # projection: totals 40/20/20/20/10, maxima 30/10/10/10/5,
        # two samples per row, and a 180 ns enclosing control extent.
        $controlRows = @(New-Stage5PhaseWireRows @(40,20,20,20,10) @(40,20,20,20,10) 2)
        $controlRows[0].maximumNanoseconds = [UInt64]30
        $controlRows[1].maximumNanoseconds = [UInt64]10
        $controlRows[2].maximumNanoseconds = [UInt64]10
        $controlRows[3].maximumNanoseconds = [UInt64]10
        $controlRows[4].maximumNanoseconds = [UInt64]5
        $controlTotal = 180; $controlMaximum = 100; $controlUnscoped = 70
    }
    if ($AnticorrelatedRepeat -ge 0) {
        $Control = 1
        [UInt64[]]$totals = @(1,1,1,1,1)
        [UInt64[]]$serial = @(1,1,1,1,1)
        $totals[$AnticorrelatedRepeat] = 101
        $serial[$AnticorrelatedRepeat] = 21
        $worldRows = @(New-Stage5PhaseWireRows $totals $serial 1)
        $frame = 165; $unscoped = 60; $completion = 35
        $controlRows = @(New-Stage5PhaseWireRows @(102,1,1,1,1) @(3,1,1,1,1) 1)
        $controlTotal = 110; $controlMaximum = 110; $controlUnscoped = 4
    }
    [UInt64]$controlFirstOrdinal = if ($Control -gt 0) { 1 } else { 0 }
    [pscustomobject][ordered]@{
        schemaVersion = 6
        producer = 'game-executable-stage5-performance-report-v6'
        producerVersion = '6'
        measurementRole = 'phase-serial-baseline'
        workload = [pscustomobject]@{
            sampleCount = [UInt64]1; firstFrame = 8; lastFrame = 8
        }
        frameSimulation = [pscustomobject]@{
            totalNanoseconds = [UInt64]777; maximumNanoseconds = [UInt64]777
            sampleCount = [UInt64]1
        }
        phases = $worldRows
        phaseAccounting = [pscustomobject][ordered]@{
            schemaVersion = 1; mode = 'owner-inline-source-admissions-v1'
            accountingOrigin = 'kernel-performance-ledger-whole-frame-v1'
            frozen = $true; complete = $true; errors = 0
            completedFrameCount = [UInt64]1
            firstCompletedFrame = 8; lastCompletedFrame = 8
            frameNanoseconds = $frame; maximumFrameNanoseconds = $frame
            unscopedSerialNanoseconds = $unscoped
            completionSerialNanoseconds = $completion; completionSampleCount = [UInt64]1
            schedulerClosureKnown = $true
            schedulerBegin = [pscustomobject]@{
                submittedJobs = [UInt64]90; executedJobs = [UInt64]90; ownerHelpJobs = [UInt64]7
                outstandingJobs = [UInt64]0; pendingJobs = [UInt64]0
            }
            schedulerEnd = [pscustomobject]@{
                submittedJobs = [UInt64]90; executedJobs = [UInt64]90; ownerHelpJobs = [UInt64]7
                outstandingJobs = [UInt64]0; pendingJobs = [UInt64]0
            }
            controlAccounting = [pscustomobject][ordered]@{
                windowCount = [UInt64]$Control
                firstSampleOrdinal = $controlFirstOrdinal
                lastSampleOrdinal = [UInt64]$Control
                totalNanoseconds = $controlTotal; maximumNanoseconds = $controlMaximum
                unscopedSerialNanoseconds = $controlUnscoped
                phases = $controlRows
            }
        }
    }
}

function Test-Stage5PhaseAccountingWireContract {
    # These assertions exercise a real shared module function. The linkable
    # fail-closed stub is a prerequisite, not a command-not-found RED.
    $plain = New-Stage5PhaseAccountingWireFixture
    $partition = Assert-Stage5PhaseAccountingContract $plain 'literal no-control partition'
    Assert-True ($partition.accountedNanoseconds -eq 270 -and
        $partition.serialNanoseconds -eq 235 -and $partition.pureNanoseconds -eq 35 -and
        $partition.frameNanoseconds -eq 200 -and $partition.controlNanoseconds -eq 0) `
        'Full mixed partition must include late serial cost and exclude the independent 777 ns legacy clock.'
    Assert-True (([decimal]$partition.serialNanoseconds / [decimal]$partition.accountedNanoseconds) -gt 0.5) `
        'Known coverage must not convert this measured serial-fraction failure into 2x qualification.'

    $withControl = New-Stage5PhaseAccountingWireFixture -Control 1
    $partition = Assert-Stage5PhaseAccountingContract $withControl 'literal control partition'
    Assert-True ($partition.accountedNanoseconds -eq 320 -and
        $partition.serialNanoseconds -eq 265 -and $partition.pureNanoseconds -eq 55 -and
        $partition.completedFrameCount -eq 1 -and $partition.controlNanoseconds -eq 50) `
        'Control retains its 20 ns pure work and 30 ns serial work without increasing completed-world coverage.'

    $totals = @(); $serials = @()
    for ($repeat = 0; $repeat -lt 3; ++$repeat) {
        $receipt = New-Stage5PhaseAccountingWireFixture -AnticorrelatedRepeat $repeat
        $partition = Assert-Stage5PhaseAccountingContract $receipt "anticorrelated repeat $repeat"
        Assert-True ($partition.accountedNanoseconds -eq 310 -and
            $partition.serialNanoseconds -eq 131 -and $partition.pureNanoseconds -eq 179) `
            'Each complete repeat must sum world, control, unscoped and late completion before statistics.'
        $totals += [decimal]$partition.accountedNanoseconds
        $serials += [decimal]$partition.serialNanoseconds
    }
    # Each literal total is equal, so the three-run medians are independent
    # constants. Summing the component medians would instead yield T=210/R=111.
    Assert-True ($totals[1] -eq 310 -and $serials[1] -eq 131) `
        'The fixture fixes median totals independently of any production median implementation.'

    $mutations = @(
        @{ name='world-gap'; change={ param($r) $r.phaseAccounting.unscopedSerialNanoseconds-- } },
        @{ name='pure-substitution'; change={ param($r) $r.phases[2].pureNanoseconds++ } },
        @{ name='unknown-serial'; change={ param($r) $r.phases[2].serialNanosecondsKnown=$false } },
        @{ name='unknown-pure'; change={ param($r) $r.phases[2].pureNanosecondsKnown=$false } },
        @{ name='missing-phase'; change={ param($r) $r.phases=@($r.phases | Select-Object -Skip 1) } },
        @{ name='wrong-world-window'; change={ param($r) $r.phaseAccounting.lastCompletedFrame=9 } },
        @{ name='world-frame-zero'; change={ param($r) $r.phaseAccounting.firstCompletedFrame=0 } },
        @{ name='sample-substitution'; change={ param($r) $r.phases[0].sampleCount=2 } },
        @{ name='completion-without-sample'; change={ param($r) $r.phaseAccounting.completionSampleCount=0 } },
        @{ name='worker-executed'; change={ param($r) $r.phaseAccounting.schedulerEnd.executedJobs++ } },
        @{ name='worker-counter-reset'; change={ param($r) $r.phaseAccounting.schedulerEnd.executedJobs=0 } },
        @{ name='pending-completion'; change={ param($r) $r.phaseAccounting.schedulerEnd.pendingJobs=1 } },
        @{ name='unknown-closure'; change={ param($r) $r.phaseAccounting.schedulerClosureKnown=$false } },
        @{ name='control-gap'; change={ param($r) $r.phaseAccounting.controlAccounting.unscopedSerialNanoseconds-- } },
        @{ name='control-pure-loss'; change={ param($r) $r.phaseAccounting.controlAccounting.phases[2].pureNanoseconds=0 } },
        @{ name='control-missing'; change={ param($r) $r.phaseAccounting.PSObject.Properties.Remove('controlAccounting') } },
        @{ name='control-zero-ordinal'; change={ param($r) $r.phaseAccounting.controlAccounting.firstSampleOrdinal=0 } },
        @{ name='control-fake-world'; change={ param($r) $r.phaseAccounting.completedFrameCount++ } },
        @{ name='control-unaccounted-count'; change={ param($r) $r.phaseAccounting.controlAccounting.windowCount=0 } },
        @{ name='baseline-role-stripped'; change={ param($r) $r.measurementRole='throughput' } },
        @{ name='false-frozen'; change={ param($r) $r.phaseAccounting.frozen=$false } },
        @{ name='false-complete'; change={ param($r) $r.phaseAccounting.complete=$false } },
        @{ name='u64-overflow'; change={ param($r) $r.phaseAccounting.completionSerialNanoseconds=[UInt64]::MaxValue } },
        @{ name='fractional-nanoseconds'; change={ param($r) $r.phases[0].serialNanoseconds=1.5 } }
    )
    foreach ($case in $mutations) {
        $invalid = New-Stage5PhaseAccountingWireFixture -Control 1
        & $case.change $invalid
        $rejected = $false
        try { Assert-Stage5PhaseAccountingContract $invalid $case.name | Out-Null }
        catch { $rejected = $true }
        Assert-True $rejected "Malformed phase/control partition '$($case.name)' was accepted."
    }
}

function Test-Stage5V6FooterAndRepeatedControlContract {
    $failures = New-Object 'Collections.Generic.List[string]'

    try {
        # Independent native projection literals: world 200 ns, controls
        # 180 ns, completion 70 ns => accounted 450, serial 415, pure 35.
        $repeated = New-Stage5PhaseAccountingWireFixture -Control 2
        $partition = Assert-Stage5PhaseAccountingContract $repeated `
            'literal repeated-control partition'
        $control = $repeated.phaseAccounting.controlAccounting
        [UInt64[]]$expectedMaxima = @(30,10,10,10,5)
        $maximaMatch = $control.phases.Count -eq $expectedMaxima.Count
        for ($index = 0; $index -lt $expectedMaxima.Count; ++$index) {
            $maximaMatch = $maximaMatch -and
                $control.phases[$index].maximumNanoseconds -eq $expectedMaxima[$index]
        }
        Assert-True ($control.windowCount -eq 2 -and
            $control.firstSampleOrdinal -eq 1 -and
            $control.lastSampleOrdinal -eq 2 -and
            $control.totalNanoseconds -eq 180 -and
            $control.maximumNanoseconds -eq 100 -and
            $partition.controlNanoseconds -eq 180 -and
            $partition.accountedNanoseconds -eq 450 -and
            $partition.serialNanoseconds -eq 415 -and
            $partition.pureNanoseconds -eq 35 -and $maximaMatch) `
            'Two control windows must retain independent extent/maxima and exact partition.'
        foreach ($row in @($control.phases)) {
            Assert-True ($row.sampleCount -eq 2) `
                'Every repeated-control phase row must count both control windows.'
        }
    }
    catch {
        $failures.Add("repeated-control accounting: $($_.Exception.Message)") | Out-Null
    }

    try {
        # This is a synthetic 100-frame host projection.  It exercises the
        # JSON footer contract only; its opaque bytes are not native trace
        # evidence or proof of the binary record body.
        $root = Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'phase-v6-footer-repeated-control')
        $manifest = New-Stage5PhaseValidationFixture $root -ControlWindows 2
        $document = Read-TestJson $manifest
        $result = Assert-Stage5PerformanceRunSet $document
        Assert-True (@($result.pairedPhaseBaselineBindings).Count -eq 4 -and
            $document.phaseBaselineProfiles[0].window.controlWindowCount -eq 2) `
            'Repeated-control cohort must retain all four baseline pairs and its declared profile count.'
        foreach ($pair in @($document.pairedPhaseBaselineBindings)) {
            $baseline = Read-TestJson $pair.baselineRun.receiptPath
            $trace = $baseline.attemptTrace
            Assert-True ($trace.windowBoundaryCount -eq 102 -and
                $trace.completedWindowCount -eq 100 -and
                $trace.controlWindowCount -eq 2 -and
                $baseline.phaseAccounting.controlAccounting.windowCount -eq 2) `
                'V6 trace footer must retain boundary, completed-world, and control counts.'
        }
    }
    catch {
        $failures.Add("repeated-control runner path: $($_.Exception.Message)") | Out-Null
    }

    # Reuse the existing table-driven malformed-fixture pattern.  Each new
    # footer counter has both a missing-field and an exact-type negative case.
    $footerMutations = @(
        @{ name='missing-window-boundary-count'; change={ param($t)
            $t.PSObject.Properties.Remove('windowBoundaryCount')
        } },
        @{ name='window-boundary-count-string'; change={ param($t)
            $t.windowBoundaryCount = '102'
        } },
        @{ name='missing-completed-window-count'; change={ param($t)
            $t.PSObject.Properties.Remove('completedWindowCount')
        } },
        @{ name='completed-window-count-string'; change={ param($t)
            $t.completedWindowCount = '100'
        } },
        @{ name='missing-control-window-count'; change={ param($t)
            $t.PSObject.Properties.Remove('controlWindowCount')
        } },
        @{ name='control-window-count-string'; change={ param($t)
            $t.controlWindowCount = '2'
        } },
        @{ name='window-boundary-count-too-small'; change={ param($t)
            $t.windowBoundaryCount = [UInt64]101
        } },
        @{ name='completed-window-count-mismatch'; change={ param($t)
            $t.completedWindowCount = [UInt64]99
        } },
        @{ name='control-window-count-mismatch'; change={ param($t)
            $t.controlWindowCount = [UInt64]1
        } }
    )
    foreach ($case in $footerMutations) {
        try {
            $caseRoot = Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName ('phase-v6-footer-' + $case.name))
            $caseManifest = New-Stage5PhaseValidationFixture $caseRoot -ControlWindows 2
            $caseDocument = Read-TestJson $caseManifest
            $pair = @($caseDocument.pairedPhaseBaselineBindings)[0]
            $baseline = Read-TestJson $pair.baselineRun.receiptPath
            & $case.change $baseline.attemptTrace
            Write-Json $pair.baselineRun.receiptPath $baseline
            $pair.baselineRun.receiptSha256 = Get-Sha256 $pair.baselineRun.receiptPath
            $rejected = $false
            try { Assert-Stage5PerformanceRunSet $caseDocument | Out-Null }
            catch { $rejected = $true }
            if (-not $rejected) {
                $failures.Add("V6 footer mutation '$($case.name)' was accepted.") | Out-Null
            }
        }
        catch {
            $failures.Add("V6 footer mutation '$($case.name)': $($_.Exception.Message)") | Out-Null
        }
    }

    try {
        $ordinalGap = New-Stage5PhaseAccountingWireFixture -Control 2
        $ordinalGap.phaseAccounting.controlAccounting.lastSampleOrdinal = 3
        $rejected = $false
        try {
            Assert-Stage5PhaseAccountingContract $ordinalGap 'repeated-control ordinal gap' | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected `
            'A non-contiguous repeated-control ordinal range was accepted.'
    }
    catch {
        $failures.Add("repeated-control ordinal range: $($_.Exception.Message)") | Out-Null
    }

    if ($failures.Count -gt 0) {
        throw ($failures.ToArray() -join ' | ')
    }
}

function Get-Stage5PhaseTestRunIdentitySha256 {
    param([object]$Receipt)
    $memory = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($memory)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RTS-KERNEL-FIELDS-v1'))
        $writer.Write([UInt32]0x5003)
        [UInt32]$tag = 1
        foreach ($text in @([string]$Receipt.runId, [string]$Receipt.runNonce)) {
            $bytes = [Text.Encoding]::ASCII.GetBytes($text)
            $writer.Write([byte]6); $writer.Write($tag); $writer.Write([UInt32]$bytes.Length)
            foreach ($value in $bytes) {
                $writer.Write([byte]1); $writer.Write($tag); $writer.Write([UInt32]$value)
            }
            ++$tag
        }
        $writer.Write([byte]1); $writer.Write([UInt32]3); $writer.Write([UInt32]$Receipt.process.id)
        $writer.Write([byte]3); $writer.Write([UInt32]4); $writer.Write([UInt64]$Receipt.process.creationTimeUtc100ns)
        $writer.Flush()
        $hash = [Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($hash.ComputeHash($memory.ToArray()))).Replace('-', '') }
        finally { $hash.Dispose() }
    }
    finally { $writer.Dispose(); $memory.Dispose() }
}

function Set-Stage5V6WireRun {
    param([object]$Run, [object]$Receipt)
    $Receipt.schemaVersion = 6
    $Receipt.producer = 'game-executable-stage5-performance-report-v6'
    $Receipt.producerVersion = '6'
    if ($Receipt.PSObject.Properties.Name -notcontains 'phaseAccounting') {
        $Receipt | Add-Member NoteProperty phaseAccounting $null
    }
    if ($Receipt.PSObject.Properties.Name -notcontains 'attemptTrace') {
        $Receipt | Add-Member NoteProperty attemptTrace $null
    }
    foreach ($phase in $Receipt.phases) {
        if ($phase.PSObject.Properties.Name -notcontains 'pureNanoseconds') {
            $phase | Add-Member NoteProperty pureNanoseconds ([UInt64]0)
            $phase | Add-Member NoteProperty pureNanosecondsKnown $false
        }
    }
    $rawPath = [string]$Receipt.rawEvidence.rawLogPath
    $raw = [IO.File]::ReadAllText($rawPath).Replace(
        'game-executable-performance-receipt-v5', 'game-executable-performance-receipt-v6')
    [IO.File]::WriteAllText($rawPath, $raw)
    $Receipt.rawEvidence.rawLogSha256 = Get-Sha256 $rawPath
    $Receipt.rawLogs[0].sha256 = $Receipt.rawEvidence.rawLogSha256
    $Run.host.rawLogSha256 = $Receipt.rawEvidence.rawLogSha256
    Write-Json $Run.receiptPath $Receipt
    $Run.receiptSha256 = Get-Sha256 $Run.receiptPath
}

function New-Stage5PhaseValidationFixture {
    param([string]$Root, [int]$ControlWindows = 1)
    # Current existing fixture factory; no game, worker or fabricated returned
    # validator result. Files are synthetic host-contract data, not runtime proof.
    [UInt64]$windowBoundaryCount = 0
    if ($ControlWindows -eq 1) { $windowBoundaryCount = 101 }
    elseif ($ControlWindows -eq 2) { $windowBoundaryCount = 102 }
    else { throw "Synthetic V6 fixture supports only one or two control windows." }
    $manifest = New-ValidationFixture $Root 'LocalCapacitySmoke'
    $document = Read-TestJson $manifest
    # Keep unselected throughput runs on the production V5 wire shape. Only
    # traced sources and their paired baselines are V6.
    $document | Add-Member NoteProperty referencePolicy 'throughput-only'
    $document | Add-Member NoteProperty pairedOracleBindings @()
    $document | Add-Member NoteProperty phaseBaselinePolicy 'paired-source-admissions-v1'
    $limits = [pscustomobject]@{
        maximumBytes = [UInt64]1048576; maximumRecords = [UInt64]100000
        maximumLogicalEvents = [UInt64]100000; maximumAttempts = [UInt64]10000
        maximumRanges = [UInt64]500000
    }
    $profile = [pscustomobject][ordered]@{
        profileId = 'dense-local-physical4'; fixtureId = 'dense-eight-player'
        sourceLane = 'physical-4'; sourcePolicySha256 = ('7A' * 32)
        limits = $limits; residentAttemptCapacity = [UInt64]15; residentRangeCapacity = [UInt64]340
        fixtureSha256 = [string]$document.fixtures[3].sha256
        window = [pscustomobject]@{ firstCompletedFrame=1; lastCompletedFrame=100; completedFrameCount=100; controlWindowCount=$ControlWindows }
        warmupRuns = 1; measuredRuns = 3
    }
    $document | Add-Member NoteProperty phaseBaselineProfiles @($profile)
    $pairs = @(); $planEntries = @(); $outcomes = @()
    foreach ($run in $document.runs) {
        $planEntries += [pscustomobject]@{
            entryId=$run.runId; measurementRole='throughput'; profileId=$null
            fixtureId=$run.fixtureId; lane=$run.lane; ordinal=$run.ordinal
            warmup=$run.warmup; sourceEntryId=$null
        }
        $outcomes += [pscustomobject]@{ entryId=$run.runId; state='completed'; failure=$null }
    }
    $sourceRuns = @($document.runs | Where-Object {
        $_.fixtureId -ceq 'dense-eight-player' -and $_.lane -ceq 'physical-4'
    } | Sort-Object ordinal)
    foreach ($sourceRun in $sourceRuns) {
        $ordinal = [int]$sourceRun.ordinal
        $source = Read-TestJson $sourceRun.receiptPath
        $baselineRun = Write-RunReceipt $Root $document.executablePath `
            $sourceRun.fixtureId $document.fixtures[3].path 'physical-4' 4 `
            $ordinal (90000 + $ordinal) (1000000.0 + $ordinal) $document.fixtures[3].peakUnitCount `
            $document.executableSha256 $document.artifactSetSha256 $document.fixtures[3].sha256 `
            $document.runtimeClosure $document.sourceCommit $document.cohortNonce `
            $document.cohortCreatedUtc $true 'phase-serial-baseline' 'phase-baseline-binding'
        $baseline = Read-TestJson $baselineRun.receiptPath
        foreach ($extraName in @('ai-planning','path')) {
            if ($extraName -ceq 'path' -and $ordinal -eq 0) { continue }
            $timingExtra = @($baseline.kernelTiming.streams | Where-Object {
                $_.name -ceq $extraName -and $_.subtype -eq 0
            })[0] | ConvertTo-Json -Depth 20 | ConvertFrom-Json
            $timingExtra.subtype = 1
            $referenceExtra = @($baseline.kernelReference.streams | Where-Object {
                $_.name -ceq $extraName -and $_.subtype -eq 0
            })[0] | ConvertTo-Json -Depth 20 | ConvertFrom-Json
            $referenceExtra.subtype = 1
            $baseline.kernelTiming.streams += $timingExtra
            $baseline.kernelReference.streams += $referenceExtra
        }
        $baseline.kernelTiming.streams[5].attemptedBatches = 4
        $baseline.kernelTiming.streams[5].admittedBatches = 2
        $baseline.kernelTiming.streams[5].abortedBatches = 1
        $baseline.kernelReference.streams[5].validatedOperationCount = 2
        $baseline.kernelReference.streams[5].committedOperationCount = 2
        $source.kernelTiming = $baseline.kernelTiming
        $source.kernelReference = $baseline.kernelReference
        $source.kernelReference.mode = 'throughput-binding'
        # Opaque actual bytes for HOST hashing/length tests only. A's native
        # parser tests use its real generated trace; these are never fed to A
        # or presented as executable-originated live evidence.
        $tracePath = Join-Path (Split-Path -Parent $sourceRun.receiptPath) 'synthetic-wire-trace.bin'
        $traceBytes = New-Object byte[] 4096
        $traceMarker = [Text.Encoding]::ASCII.GetBytes("host-wire-trace-$ordinal")
        [Array]::Copy($traceMarker, $traceBytes, $traceMarker.Length)
        [IO.File]::WriteAllBytes($tracePath, $traceBytes)
        $trace = [pscustomobject][ordered]@{
            schemaVersion=1; encoding='typed-canonical-le-v1'; fieldSchema=20481
            mode='record'; frozen=$true; complete=$true; errors=0
            observationIngressSealed=$true; executionClosureSealed=$true
            file=[pscustomobject]@{ path=$tracePath; sha256=(Get-Sha256 $tracePath); byteCount=[UInt64]([IO.FileInfo]$tracePath).Length }
            binding=[pscustomobject]@{
                nativeRunIdentitySha256=(Get-Stage5PhaseTestRunIdentitySha256 $source)
                executableSha256=$source.executableSha256; fixtureSha256=$source.fixture.contentSha256
                sourcePolicySha256=$profile.sourcePolicySha256
            }
            limits=$limits; residentAttemptCapacity=[UInt64]15; residentRangeCapacity=[UInt64]340
            residentAttemptCount=[UInt64]0; residentAttemptHighWater=[UInt64]2
            residentRangeCount=[UInt64]0; residentRangeHighWater=[UInt64]3
            # This remains a synthetic host projection.  Its record/logical
            # counts are set to the hand-declared boundary count solely to
            # satisfy the V6 footer inequalities; these bytes are not native
            # binary trace proof.
            recordCount=$windowBoundaryCount; logicalEventCount=$windowBoundaryCount
            windowBoundaryCount=$windowBoundaryCount; completedWindowCount=[UInt64]100
            controlWindowCount=[UInt64]$ControlWindows
            coalescedSpanCount=[UInt64]0; coalescedAttemptCount=[UInt64]0
            attemptCount=[UInt64]4; admittedAttemptCount=[UInt64]2; notAdmittedAttemptCount=[UInt64]2
            abortedAfterAdmissionAttemptCount=[UInt64]1; reapCount=[UInt64]4
            capturedAttemptCount=[UInt64]2; capturedOperationCount=[UInt64]5
            dispatchCount=[UInt64]2; rangeCount=[UInt64]5; releasedRangeCount=[UInt64]5
            sourceBinding=$null
        }
        if ($source.PSObject.Properties.Name -contains 'attemptTrace') {
            $source.attemptTrace = $trace
        }
        else {
            $source | Add-Member NoteProperty attemptTrace $trace
        }
        Set-Stage5V6WireRun $sourceRun $source
        # Reparse closed source data; do not keep shared mutable fixture members
        # as source authority while changing the consumer.
        $source = Read-TestJson $sourceRun.receiptPath
        $baseline.kernelReference.mode = 'phase-baseline-binding'
        $baseline.kernelTiming.mode = 'owner-inline-baseline-observation'
        $baseline.kernelTiming.attribution = 'owner-inline-baseline-exclusive-v1'
        foreach ($stream in $baseline.kernelTiming.streams) {
            $stream.stages[2].totalNanoseconds = 0
            $stream.stages[2].sampleCount = 0
            $stream.activePipelineNanoseconds = 40
        }
        $accounting = New-Stage5PhaseAccountingWireFixture -Control $ControlWindows
        $accounting.phaseAccounting.completedFrameCount = 100
        $accounting.phaseAccounting.firstCompletedFrame = 1
        $accounting.phaseAccounting.lastCompletedFrame = 100
        foreach ($row in $accounting.phases) { $row.sampleCount = 100 }
        $baseline.phases = $accounting.phases
        $baseline.schedulerMetrics.submittedJobCount = 90
        $baseline.schedulerMetrics.executedJobCount = 90
        $baseline.schedulerMetrics.ownerHelpCount = 7
        $baseline | Add-Member NoteProperty phaseAccounting $accounting.phaseAccounting
        $baseline | Add-Member NoteProperty attemptTrace $source.attemptTrace
        $baseline.attemptTrace.mode = 'consume'
        $baseline.attemptTrace.sourceBinding = [pscustomobject][ordered]@{
            receipt=[pscustomobject]@{ path=$sourceRun.receiptPath; sha256=$sourceRun.receiptSha256 }
            runId=$source.runId; runNonce=$source.runNonce; processId=$source.process.id
            processCreationTimeUtc100ns=$source.process.creationTimeUtc100ns
        }
        foreach ($kernel in $baseline.kernels) {
            $kernel.submittedJobs=0; $kernel.completedJobs=0; $kernel.physicalWorkerJobs=0
            $kernel.ownerHelpedJobs=0; $kernel.physicalWorkerMask=0; $kernel.distinctPhysicalWorkers=0
            $kernel.elapsedNanoseconds=0; $kernel.elapsedNanosecondsKnown=$false
        }
        Set-Stage5V6WireRun $baselineRun $baseline
        $pairs += [pscustomobject]@{
            profileId=$profile.profileId; throughputRunId=$sourceRun.runId; baselineRun=$baselineRun
        }
        $planEntries += [pscustomobject]@{
            entryId=$baselineRun.runId; measurementRole='phase-serial-baseline'; profileId=$profile.profileId
            fixtureId=$baselineRun.fixtureId; lane=$baselineRun.lane; ordinal=$ordinal
            warmup=$baselineRun.warmup; sourceEntryId=$sourceRun.runId
        }
        $outcomes += [pscustomobject]@{ entryId=$baselineRun.runId; state='completed'; failure=$null }
    }
    $document | Add-Member NoteProperty pairedPhaseBaselineBindings $pairs
    $planPath = Join-Path $Root 'phase-plan.json'
    Write-Json $planPath ([ordered]@{
        schemaVersion=1; cohortNonce=$document.cohortNonce
        executableSha256=$document.executableSha256; sourceCommit=$document.sourceCommit
        phaseBaselineProfiles=@($profile); entries=$planEntries
    })
    $attemptsPath = Join-Path $Root 'phase-attempts.json'
    Write-Json $attemptsPath ([ordered]@{
        schemaVersion=1; planSha256=(Get-Sha256 $planPath); outcomes=$outcomes
    })
    $document | Add-Member NoteProperty phaseBaselinePlan ([pscustomobject]@{ path=$planPath; sha256=(Get-Sha256 $planPath) })
    $document | Add-Member NoteProperty phaseBaselineAttemptManifest ([pscustomobject]@{ path=$attemptsPath; sha256=(Get-Sha256 $attemptsPath) })
    Write-Json $manifest $document
    Complete-Stage5TestJournalFixture $manifest | Out-Null
    Register-Stage5TestDocumentInputLocks (Read-TestJson $manifest)
    return $manifest
}

function Test-Stage5PhaseSelectedRunSetContract {
    param([string]$Root)
    $manifest = New-Stage5PhaseValidationFixture $Root
    $document = Read-TestJson $manifest
    $result = Assert-Stage5PerformanceRunSet $document
    Assert-True (@($result.runs).Count -eq 48 -and
        @($result.pairedPhaseBaselineBindings).Count -eq 4 -and
        $result.phaseBaselinePolicy -ceq 'paired-source-admissions-v1') `
        'Real run-set validation must retain all 48 throughput runs and exactly the four predeclared dense baseline pairs.'
    Assert-True ($result.fixtures[3].laneMedians.'physical-4' -eq 40.0) `
        'Baseline million-millisecond elapsed must never enter the 40 ms physical-4 throughput median.'
    $mutations = @(
        @{ name='missing-warmup'; change={ param($d) $d.pairedPhaseBaselineBindings=@($d.pairedPhaseBaselineBindings | Where-Object { -not $_.baselineRun.warmup }) } },
        @{ name='duplicate-pair'; change={ param($d) $d.pairedPhaseBaselineBindings+= $d.pairedPhaseBaselineBindings[0] } },
        @{ name='wrong-source'; change={ param($d) $d.pairedPhaseBaselineBindings[0].throughputRunId=$d.runs[0].runId } },
        @{ name='changed-source-lane'; change={ param($d) $d.phaseBaselineProfiles[0].sourceLane='physical-2' } },
        @{ name='changed-source-hash'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.attemptTrace.sourceBinding.receipt.sha256=('AB'*32) } } },
        @{ name='source-filetime-one-tick'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.attemptTrace.sourceBinding.processCreationTimeUtc100ns++ } } },
        @{ name='lost-source-attempt'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.attemptTrace.attemptCount-- } } },
        @{ name='source-role-is-baseline'; change={ param($d) Update-Receipt $d.runs[44] { param($r) $r.measurementRole='phase-serial-baseline' } } },
        @{ name='missing-consume'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.attemptTrace=$null } } },
        @{ name='baseline-wait-stage-present'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.kernelTiming.streams[0].stages[2].totalNanoseconds=1; $r.kernelTiming.streams[0].stages[2].sampleCount=1; $r.kernelTiming.streams[0].activePipelineNanoseconds++ } } },
        @{ name='changed-output'; change={ param($d) Update-Receipt $d.pairedPhaseBaselineBindings[0].baselineRun { param($r) $r.kernelReference.streams[0].outputSha256=('EF'*32) } } },
        @{ name='dropped-attempt-outcome'; change={ param($d) $a=Read-TestJson $d.phaseBaselineAttemptManifest.path; $a.outcomes=@($a.outcomes | Select-Object -Skip 1); Write-Json $d.phaseBaselineAttemptManifest.path $a; $d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $d.phaseBaselineAttemptManifest.path } },
        @{ name='failed-attempt-presented-passed'; change={ param($d) $a=Read-TestJson $d.phaseBaselineAttemptManifest.path; $a.outcomes[-1].state='failed'; $a.outcomes[-1].failure='receipt-invalid'; Write-Json $d.phaseBaselineAttemptManifest.path $a; $d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $d.phaseBaselineAttemptManifest.path } },
        @{ name='trace-bytes-changed'; change={ param($d) $r=Read-TestJson $d.pairedPhaseBaselineBindings[0].baselineRun.receiptPath; [IO.File]::AppendAllText($r.attemptTrace.file.path, 'changed') } }
    )
    foreach ($case in $mutations) {
        $caseManifest = New-Stage5PhaseValidationFixture (Join-Path $Root (Get-Stage5HostFixtureDirectoryName $case.name))
        $invalid = Read-TestJson $caseManifest
        & $case.change $invalid
        $rejected = $false
        try { Assert-Stage5PerformanceRunSet $invalid | Out-Null }
        catch { $rejected = $true }
        Assert-True $rejected "Changed selected phase cohort '$($case.name)' was accepted."
    }
}

function Test-Stage5PhaseHostBoundaryContracts {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $failures = New-Object 'Collections.Generic.List[string]'

    $junctionTaskRoot = Join-Path $Root 'junction-task-root'
    $junctionDestination = Join-Path $Root 'junction-destination'
    $junctionPath = Join-Path $junctionTaskRoot 'phase-link'
    $destinationFile = Join-Path $junctionDestination 'phase-plan.json'
    $junctionCreated = $false
    $destinationCreated = $false
    try {
        New-Item -ItemType Directory -Path $junctionTaskRoot -Force | Out-Null
        New-Item -ItemType Directory -Path $junctionDestination -Force | Out-Null
        [IO.File]::WriteAllText($destinationFile, '{"schemaVersion":1}')
        $destinationCreated = $true
        New-Item -ItemType Junction -Path $junctionPath -Target $junctionDestination | Out-Null
        $junctionCreated = $true
        $lexicalCandidate = Join-Path $junctionPath 'phase-plan.json'
        $lexicalFull = [IO.Path]::GetFullPath($lexicalCandidate)
        $junctionFull = [IO.Path]::GetFullPath($junctionPath)
        $taskRootFull = [IO.Path]::GetFullPath($junctionTaskRoot).TrimEnd('\', '/')
        $destinationFull = [IO.Path]::GetFullPath($destinationFile)
        Assert-True ([String]::Equals(
            [IO.Path]::GetDirectoryName($junctionFull), $taskRootFull,
            [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($junctionFull) -ceq 'phase-link') `
            'Junction negative fixture must be the exact normalized child of its unique task root.'
        Assert-True ($lexicalFull.StartsWith(
            ($taskRootFull +
                [IO.Path]::DirectorySeparatorChar),
            [StringComparison]::OrdinalIgnoreCase)) `
            'Junction negative fixture must remain lexically below its task root.'
        Assert-True ($lexicalFull -cne $destinationFull -and
            (Test-Path -LiteralPath $destinationFull -PathType Leaf)) `
            'Junction negative fixture must distinguish the lexical path from its destination.'
        $linkItem = Get-Item -LiteralPath $junctionPath -Force
        Assert-True (($linkItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) `
            'Junction negative fixture was not created as a reparse point.'
        $rejected = $false
        try {
            Resolve-Stage5RunEvidenceFile $junctionTaskRoot $lexicalCandidate `
                'Junction phase evidence'
        }
        catch { $rejected = $true }
        if (-not $rejected) {
            $failures.Add('Resolve-Stage5RunEvidenceFile accepted a phase file below a directory junction.') | Out-Null
        }
    }
    catch {
        $failures.Add("junction path negative: $($_.Exception.Message)") | Out-Null
    }
    finally {
        # Remove the link itself before any recursive scratch-root cleanup; the
        # destination is task-owned but must never be traversed through a link.
        if ($junctionCreated) {
            try {
                [IO.Directory]::Delete([IO.Path]::GetFullPath($junctionPath))
            }
            catch {
                $script:Stage5SelfTestCleanupBlocked = $true
                $failures.Add("junction cleanup failed: $($_.Exception.Message)") | Out-Null
            }
            try {
                if (Test-Path -LiteralPath $junctionPath -ErrorAction Stop) {
                    $script:Stage5SelfTestCleanupBlocked = $true
                    $failures.Add('junction cleanup reported success but the exact link path remains.') | Out-Null
                }
            }
            catch {
                $script:Stage5SelfTestCleanupBlocked = $true
                $failures.Add("junction cleanup verification failed: $($_.Exception.Message)") | Out-Null
            }
        }
        if ($destinationCreated) {
            try {
                if (-not (Test-Path -LiteralPath $destinationFile -PathType Leaf -ErrorAction Stop)) {
                    $failures.Add('junction cleanup changed or removed the task-owned destination file.') | Out-Null
                }
            }
            catch {
                $failures.Add("junction destination verification failed: $($_.Exception.Message)") | Out-Null
            }
        }
    }

    $mutations = @(
        @{ name='reference-range-outside-matching-timing'; change={
            param($d)
            $pair = @($d.pairedPhaseBaselineBindings)[0]
            $sourceRun = @($d.runs | Where-Object {
                $_.runId -ceq $pair.throughputRunId
            })[0]
            $source = Read-TestJson $sourceRun.receiptPath
            $source.kernelTiming.streams[0].firstFrame = 10
            $source.kernelTiming.streams[0].lastFrame = 90
            Write-Json $sourceRun.receiptPath $source
            $sourceRun.receiptSha256 = Get-Sha256 $sourceRun.receiptPath
            $baselineRun = $pair.baselineRun
            $baseline = Read-TestJson $baselineRun.receiptPath
            $baseline.kernelTiming.streams[0].firstFrame = 10
            $baseline.kernelTiming.streams[0].lastFrame = 90
            $baseline.attemptTrace.sourceBinding.receipt.sha256 =
                $sourceRun.receiptSha256
            Write-Json $baselineRun.receiptPath $baseline
            $baselineRun.receiptSha256 = Get-Sha256 $baselineRun.receiptPath
        } }
        @{ name='plan-schema-string'; change={
            param($d)
            $plan = Read-TestJson $d.phaseBaselinePlan.path
            $plan.schemaVersion = '1'
            Write-Json $d.phaseBaselinePlan.path $plan
            Update-Stage5PhasePlanBinding $d
        } }
        @{ name='attempt-journal-schema-string'; change={
            param($d)
            $attempts = Read-TestJson $d.phaseBaselineAttemptManifest.path
            $attempts.schemaVersion = '1'
            Write-Json $d.phaseBaselineAttemptManifest.path $attempts
            $d.phaseBaselineAttemptManifest.sha256 =
                Get-Sha256 $d.phaseBaselineAttemptManifest.path
        } }
        @{ name='warmup-count-string'; change={ param($d) $d.warmupRuns = '1' } }
        @{ name='measured-count-string'; change={ param($d) $d.measuredRuns = '3' } }
        @{ name='plan-entry-measurement-role-array'; change={
            param($d)
            $plan = Read-TestJson $d.phaseBaselinePlan.path
            $plan.entries[0].measurementRole = @('throughput')
            Write-Json $d.phaseBaselinePlan.path $plan
            Update-Stage5PhasePlanBinding $d
        } }
        @{ name='plan-entry-fixture-id-array'; change={
            param($d)
            $plan = Read-TestJson $d.phaseBaselinePlan.path
            $plan.entries[0].fixtureId = @('one-thousand-units')
            Write-Json $d.phaseBaselinePlan.path $plan
            Update-Stage5PhasePlanBinding $d
        } }
        @{ name='plan-entry-lane-array'; change={
            param($d)
            $plan = Read-TestJson $d.phaseBaselinePlan.path
            $plan.entries[0].lane = @('forced-one')
            Write-Json $d.phaseBaselinePlan.path $plan
            Update-Stage5PhasePlanBinding $d
        } }
        @{ name='plan-entry-source-id-array'; change={
            param($d)
            $plan = Read-TestJson $d.phaseBaselinePlan.path
            $entry = @($plan.entries | Where-Object {
                $_.measurementRole -ceq 'phase-serial-baseline'
            })[0]
            $entry.sourceEntryId = @([string]$entry.sourceEntryId)
            Write-Json $d.phaseBaselinePlan.path $plan
            Update-Stage5PhasePlanBinding $d
        } }
    )
    foreach ($case in $mutations) {
        try {
            $caseRoot = Join-Path $Root (Get-Stage5HostFixtureDirectoryName $case.name)
            $caseManifest = New-Stage5PhaseValidationFixture $caseRoot
            $invalid = Read-TestJson $caseManifest
            & $case.change $invalid
            $rejected = $false
            try { Assert-Stage5PerformanceRunSet $invalid | Out-Null }
            catch { $rejected = $true }
            if (-not $rejected) {
                $failures.Add("Phase host boundary mutation '$($case.name)' was accepted.") | Out-Null
            }
        }
        catch {
            $failures.Add("phase host boundary '$($case.name)': $($_.Exception.Message)") | Out-Null
        }
    }
    Assert-True ($failures.Count -eq 0) ($failures.ToArray() -join ' | ')
}

function Test-Stage5BoundedTraceSnapshotContract {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $binaryPath = Join-Path $Root 'bounded-trace.bin'
    $jsonPath = Join-Path $Root 'default-json.json'
    # Cross the fixed 64 KiB read boundary twice, with a non-aligned tail.  The
    # expected length is a fixture constant and the digest is computed through
    # this test's independent stream-based helper, not the production snapshot.
    [Int64]$length = (2 * 65536) + 17
    try {
        $bytes = New-Object byte[] $length
        for ($index = 0; $index -lt $bytes.Length; ++$index) {
            $bytes[$index] = [byte](($index * 31 + 7) % 256)
        }
        [IO.File]::WriteAllBytes($binaryPath, $bytes)
        $expectedHash = Get-Sha256 $binaryPath

        $hashSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $binaryPath `
            'bounded trace helper' -HashOnly -EvidenceKind Trace
        $hashOnlyProperties = @($hashSnapshot.PSObject.Properties.Name)
        Assert-True ($hashOnlyProperties -contains 'hashOnly' -and
            [bool]$hashSnapshot.hashOnly) `
            'Hash-only trace snapshots must identify their bounded mode.'
        Assert-True (($hashOnlyProperties -notcontains 'bytes') -or
            $null -eq $hashSnapshot.bytes) `
            'Hash-only trace snapshots must not retain the full binary payload.'
        Assert-True ([Int64]$hashSnapshot.length -eq $length -and
            $hashSnapshot.sha256 -ceq $expectedHash) `
            'Hash-only trace snapshots must report the independently verified length and digest.'
        Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
            -Snapshot $hashSnapshot -Expected $expectedHash `
            -ExpectedLength $length -Context 'bounded trace helper digest' | Out-Null

        $rejected = $false
        try {
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
                -Snapshot $hashSnapshot -Expected ('00' * 32) `
                -ExpectedLength $length -Context 'wrong bounded trace digest' | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected 'A wrong declared trace hash must be rejected.'

        $rejected = $false
        try {
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
                -Snapshot $hashSnapshot -Expected $expectedHash `
                -ExpectedLength ($length + 1) -Context 'wrong bounded trace length' | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected 'A wrong declared trace length must be rejected.'

        # A fresh bounded read of changed input must not satisfy the old
        # declaration.  This checks stale binding without fabricating a writer
        # racing the held stream.
        [IO.File]::AppendAllText($binaryPath, 'changed')
        $changedSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $binaryPath `
            'changed bounded trace helper' -HashOnly -EvidenceKind Trace
        $rejected = $false
        try {
            Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
                -Snapshot $changedSnapshot -Expected $expectedHash `
                -ExpectedLength $length -Context 'changed bounded trace declaration' | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected 'A changed trace input must not satisfy its prior hash/length declaration.'

        [IO.File]::WriteAllText($jsonPath, '{"kind":"phase-test","count":2}')
        $jsonSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $jsonPath `
            'default JSON snapshot'
        Assert-True ($jsonSnapshot.PSObject.Properties.Name -contains 'bytes' -and
            $jsonSnapshot.bytes.Length -gt 0) `
            'Default JSON snapshots must retain copied bytes.'
        $jsonHash = Get-Sha256 $jsonPath
        Assert-Stage5FinalAcceptanceSnapshotSha256 $jsonSnapshot $jsonHash `
            'default JSON snapshot digest' | Out-Null
        $json = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $jsonSnapshot `
            'default JSON snapshot' -AsPsObject
        Assert-True ($json.kind -ceq 'phase-test' -and $json.count -eq 2) `
            'Default JSON snapshots must parse from their copied bytes.'
        $originalJsonByte = $jsonSnapshot.bytes[0]
        $jsonSnapshot.bytes[0] = [byte](($originalJsonByte + 1) % 256)
        $rejected = $false
        try {
            Assert-Stage5FinalAcceptanceSnapshotSha256 $jsonSnapshot $jsonHash `
                'mutated default JSON snapshot digest' | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected `
            'The legacy JSON snapshot assertion must rehash its copied bytes.'
    }
    finally {
        if (Test-Path -LiteralPath $Root) {
            Remove-Item -LiteralPath $Root -Recurse -Force
        }
    }
}

function Test-Stage5SnapshotIdentityAndSparseBounds {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $source = Join-Path $Root 'single-link.json'
    $hardLink = Join-Path $Root 'second-link.json'
    $jsonOversize = Join-Path $Root 'oversize.json'
    $replayOversize = Join-Path $Root 'oversize.rep'
    try {
        [IO.File]::WriteAllText($source, '{}')
        New-Item -ItemType HardLink -Path $hardLink -Target $source | Out-Null
        $rejected = $false
        try { Get-Stage5FinalAcceptanceFileSnapshot $source 'hard-link evidence' | Out-Null }
        catch { $rejected = $_.Exception.Message -match 'single-link' }
        Assert-True $rejected 'Immutable evidence must reject a file with more than one hard link.'
        Remove-Item -LiteralPath $hardLink -Force

        foreach ($case in @(
            [pscustomobject]@{ path=$jsonOversize; length=[Int64](64MB)+1; kind='JsonReceipt' },
            [pscustomobject]@{ path=$replayOversize; length=[Int64](256MB)+1; kind='Replay' }
        )) {
            [IO.File]::WriteAllBytes($case.path, (New-Object byte[] 0))
            & fsutil sparse setflag $case.path | Out-Null
            Assert-True ($LASTEXITCODE -eq 0) `
                "Sparse-bound fixture could not mark '$($case.path)' sparse."
            $stream = New-Object IO.FileStream($case.path, [IO.FileMode]::Open,
                [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $stream.SetLength($case.length) }
            finally { $stream.Dispose() }
            $rejected = $false
            try {
                Get-Stage5FinalAcceptanceFileSnapshot $case.path `
                    "oversize $($case.kind) evidence" -EvidenceKind $case.kind | Out-Null
            }
            catch { $rejected = $_.Exception.Message -match 'exceeds its .* snapshot bound' }
            Assert-True $rejected `
                "$($case.kind) immutable snapshots must reject sparse oversize input before allocation/read."
        }
    }
    finally {
        foreach ($path in @($hardLink,$source,$jsonOversize,$replayOversize)) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }
}


# Immutable prelaunch identity and append-only journal boundary regressions.
function Test-Stage5CreateNewEvidencePublication {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root | Out-Null
    $path = Join-Path $Root 'attempt-start.json'
    Write-Stage5JsonAtomically $path ([pscustomobject]@{
        entryId='literal-entry'; event='attempt-start'; sequence=1
    }) -CreateNew
    $originalHash = Get-Sha256 $path
    $rejected = $false
    try {
        Write-Stage5JsonAtomically $path ([pscustomobject]@{
            entryId='replacement-entry'; event='attempt-start'; sequence=2
        }) -CreateNew
    }
    catch { $rejected = $true }
    Assert-True ($rejected -and (Get-Sha256 $path) -ceq $originalHash) `
        'Create-new plan/journal publication must reject replacement and preserve original bytes.'
    Assert-True (@(Get-ChildItem -LiteralPath $Root -File -Filter '.stage5-write-*.tmp').Count -eq 0) `
        'A rejected create-new publication must not leave its temporary file.'
    Write-Stage5JsonAtomically $path ([pscustomobject]@{
        entryId='literal-entry'; event='restored'; sequence=2
    })
    Assert-True ((Read-TestJson $path).event -ceq 'restored') `
        'The default atomic writer must retain existing registry-recovery replacement semantics.'

    $racePath = Join-Path $Root 'identity-race.json'
    $movedHeldPath = Join-Path $Root 'identity-race-held.json'
    $raceRejected = $false
    try {
        $raceBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
            '{"state":"trusted"}')
        Write-Stage5FinalAcceptanceFileAtomically $racePath $raceBytes `
            'Identity-race publication' -BeforePublishTestHook {
                param($temporaryPath, $destinationPath)
                [IO.File]::Move($temporaryPath, $movedHeldPath)
                [IO.File]::WriteAllText($temporaryPath, '{"state":"forged"}')
            } | Out-Null
    }
    catch {
        $raceRejected = $_.Exception.Message -match
            'opened handle resolves to a different path|retain the created file identity'
    }
    Assert-True ($raceRejected -and
        (Test-Path -LiteralPath $racePath -PathType Leaf) -and
        -not (Test-Path -LiteralPath $movedHeldPath) -and
        ([IO.File]::ReadAllText($racePath) -ceq '{"state":"forged"}')) `
        'Same-directory temporary replacement must fail identity-bound publication and clean only the held file.'
    Remove-Item -LiteralPath $racePath -Force
}

function New-Stage5PrelaunchContractFixture {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root | Out-Null
    $inputs = Join-Path $Root 'inputs'
    $taskRoot = Join-Path $Root 'cohort'
    New-Item -ItemType Directory -Path $inputs,$taskRoot | Out-Null
    $commit = 'a' * 40
    $artifact = New-ArtifactSetFixture $inputs $commit
    $ids = @('one-thousand-units','four-thousand-units','eight-thousand-units','dense-eight-player')
    $units = @(1000,4000,8000,12000)
    $fixtureRows = @()
    for ($index=0; $index -lt 4; ++$index) {
        $leaf = $ids[$index] + '.rep'
        $path = Join-Path $inputs $leaf
        [IO.File]::WriteAllText($path, "prelaunch-host-fixture-$index")
        $fixtureRows += [pscustomobject]@{
            id=$ids[$index]; source=$leaf; sha256=(Get-Sha256 $path)
            seed=7; playerCount=8; peakUnitCount=$units[$index]
        }
    }
    $fixturePath = Join-Path $inputs 'fixtures.json'
    Write-Json $fixturePath ([pscustomobject]@{
        schemaVersion=1; evidenceKind='stage5-performance-scaling-fixtures'
        title='ZeroHour'; executableSha256=$artifact.executableHash; fixtures=$fixtureRows
    })
    $fixtureBinding = Read-Stage5ScalingFixtureManifest $fixturePath `
        (Get-Sha256 $fixturePath) 'ZeroHour' $artifact.executableHash
    $context = [pscustomobject]@{
        schemaVersion=1; title='ZeroHour'; qualificationMode='LocalCapacitySmoke'
        sourceCommit=$commit; stage3SourceCommit=''
        artifactSetSha256=$artifact.sha256; artifactSetManifestPath=$artifact.path
        runtimeClosure=[pscustomobject]@{
            dependencyManifestSha256=$artifact.runtimeClosure.dependencyManifestSha256
            closureSha256=$artifact.runtimeClosure.closureSha256
        }
        cohortNonce='10000000-0000-4000-8000-000000000001'
        cohortCreatedUtc='2026-09-01T00:00:00.0000000Z'
        executablePath=$artifact.executablePath; executableSha256=$artifact.executableHash
        fixtureManifestPath=$fixtureBinding.path; fixtureManifestSha256=$fixtureBinding.sha256
        referencePolicy='paired-serial-oracle-v1'; pairedOracleBindings=@()
        stage3BaselineSha256=''; taskRoot=$taskRoot; warmupRuns=1; measuredRuns=3
        fixtures=@($fixtureBinding.fixtures); stage3Fixtures=@(); runs=@()
        topology=[pscustomobject]@{
            source='GetSystemCpuSetInformation'; physicalCoreCount=6; logicalProcessorCount=12
            cpuSets=@(0..11 | ForEach-Object {
                [pscustomobject]@{
                    id=($_ + 1); efficiencyClass=0; group=0
                    coreIndex=[int][Math]::Floor($_ / 2); logicalProcessorIndex=$_
                    parked=$false; allocated=$false; available=$true
                }
            })
        }
        processCleanup=[pscustomobject]@{ processId=0; exitProof=$true; blocked=$false; errors=@() }
    }
    $titleSession = New-Stage5TitleSessionContract 'ZeroHour' `
        (Join-Path $taskRoot 'TitleSession') `
        (Split-Path -Parent $artifact.executablePath) $taskRoot
    # These are explicit synthetic host-plan inputs, never a native source-policy
    # receipt or a claim that a game emitted any trace.
    $profile = [pscustomobject]@{
        profileId='dense-local-physical4'; fixtureId='dense-eight-player'; sourceLane='physical-4'
        sourcePolicySha256=('7A' * 32); fixtureSha256=$fixtureBinding.fixtures[3].sha256
        limits=[pscustomobject]@{
            maximumBytes=[UInt64]1048576; maximumRecords=[UInt64]100000
            maximumLogicalEvents=[UInt64]100000; maximumAttempts=[UInt64]10000
            maximumRanges=[UInt64]500000
        }
        residentAttemptCapacity=[UInt64]15; residentRangeCapacity=[UInt64]340
        window=[pscustomobject]@{
            firstCompletedFrame=1; lastCompletedFrame=100; completedFrameCount=100; controlWindowCount=1
        }
        warmupRuns=1; measuredRuns=3
    }
    return [pscustomobject]@{ context=$context; titleSession=$titleSession; profiles=@($profile) }
}

function Register-Stage5TestInputLocks {
    param([object]$Fixture)
    Register-Stage5TestDocumentInputLocks $Fixture.context
}

function Register-Stage5TestDocumentInputLocks {
    param([object]$Context)
    $key = [IO.Path]::GetFullPath([string]$Context.taskRoot)
    if ($script:Stage5TestInputLocks.ContainsKey($key)) { return }
    $artifact = Read-Stage5PerformanceArtifactSet $Context.artifactSetManifestPath `
        $Context.artifactSetSha256 $Context.sourceCommit $Context.title `
        $Context.executablePath $Context.executableSha256
    $additionalPaths = @()
    if ($Context.PSObject.Properties.Name -ccontains 'performanceData') {
        $additionalPaths = @($Context.performanceData.sourceManifestPath,
            $Context.performanceData.path) +
            @($Context.performanceData.filePaths)
    }
    $locks = Open-Stage5PerformanceReadOnlyLocks $artifact $Context.fixtures `
        $Context.fixtureManifestPath $additionalPaths
    $script:Stage5TestInputLocks[$key] = @($locks)
}

function Test-Stage5HeldInputRegistrationArrayStability {
    param([string]$Root)
    $first = New-Stage5PrelaunchContractFixture (Join-Path $Root 'first')
    Register-Stage5TestInputLocks $first
    $singleRegistration = @($script:Stage5HeldInputRegistrations)[0]
    Assert-True ($null -ne $singleRegistration) `
        'Held-input registration fixture did not create its first capability.'
    # PowerShell control-flow output can unwrap a single-element array. The
    # production registrar must remain safe when handed that scalar state.
    $script:Stage5HeldInputRegistrations = $singleRegistration
    $second = New-Stage5PrelaunchContractFixture (Join-Path $Root 'second')
    Register-Stage5TestInputLocks $second
    Assert-True (@($script:Stage5HeldInputRegistrations).Count -eq 2) `
        'Held-input registration must preserve both capabilities after scalar unwrapping.'
}

function Test-Stage5PrelaunchIdentityPlan {
    param([string]$Root)
    $fixture = New-Stage5PrelaunchContractFixture $Root
    Register-Stage5TestInputLocks $fixture
    $binding = New-Stage5PerformanceRunPlan $fixture.context 60 $fixture.titleSession $fixture.profiles
    Assert-True ($null -ne $binding -and (Test-Path -LiteralPath $binding.path -PathType Leaf)) `
        'The complete original run plan must be persisted before returning any launch binding.'
    $plan = Read-Stage5PhaseBoundJson $binding $fixture.context.taskRoot 'prelaunch test plan'
    Assert-True ($plan.entries.Count -eq 100 -and
        @($plan.entries | Where-Object { $_.measurementRole -ceq 'throughput' }).Count -eq 48 -and
        @($plan.entries | Where-Object { $_.measurementRole -ceq 'serial-oracle' }).Count -eq 48 -and
        @($plan.entries | Where-Object { $_.measurementRole -ceq 'phase-serial-baseline' }).Count -eq 4 -and
        @($plan.entries | Where-Object { $_.warmup }).Count -eq 25) `
        'Prelaunch must declare all 100 local synthetic roles, including all 25 warmups.'
    Assert-True (@($plan.entries.entryId | Select-Object -Unique).Count -eq 100 -and
        @($plan.entries.runNonce | Select-Object -Unique).Count -eq 100) `
        'Every predeclared role needs one unique immutable native ID and nonce.'
    $ids = @{}
    foreach ($entry in $plan.entries) {
        Assert-True ($entry.runNonce -is [string] -and
            $entry.runNonce -cmatch '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
            'The native nonce must be minted once as a scalar canonical UUID.'
        Assert-True (-not (Test-Path -LiteralPath $entry.outputPaths.runRoot)) `
            'Plan creation must not create a role output directory.'
        Assert-True ($entry.outputPaths.runRoot -ceq (Join-Path $fixture.context.taskRoot $entry.entryId) -and
            $entry.outputPaths.tempDirectory -ceq (Join-Path $entry.outputPaths.runRoot 'temp') -and
            $entry.outputPaths.receiptDirectory -ceq (Join-Path $entry.outputPaths.runRoot 'receipt') -and
            $entry.outputPaths.timingDirectory -ceq (Join-Path $entry.outputPaths.runRoot 'timing')) `
            'The plan must freeze the deterministic exclusive role directories.'
        if ($entry.measurementRole -cne 'throughput') {
            Assert-True ($ids.ContainsKey($entry.sourceEntryId) -and
                $ids[$entry.sourceEntryId].measurementRole -ceq 'throughput') `
                'Every dependent role must name an earlier declared throughput source.'
        }
        $ids[$entry.entryId] = $entry
    }
    Assert-True (@($plan.entries | Where-Object { $null -ne $_.outputPaths.attemptTracePath }).Count -eq 4) `
        'Only the four preselected throughput sources may own a produced attempt trace.'
    Assert-True ($plan.outputFilePolicy -ceq 'native-runid-pid-receipt-pid-tick-timing-v1') `
        'Prelaunch must preserve native PID/tick filename authority instead of inventing exact filenames.'
}

function Test-Stage5PrelaunchStartInfoBinding {
    param([string]$Root)
    $literal = New-Stage5LiteralPrelaunchPlanFixture $Root
    $fixture = $literal.fixture
    Register-Stage5TestInputLocks $fixture
    $binding = $literal.binding
    $plan = Read-Stage5PhaseBoundJson $binding $fixture.context.taskRoot 'start-info original plan'
    $entry = $plan.entries[0]
    $nativeId = $entry.entryId
    $nativeNonce = $entry.runNonce
    $nativeRoot = Join-Path $fixture.context.taskRoot $nativeId
    $entry.runNonce = '20000000-0000-4000-8000-000000000099'
    $entry.outputPaths.tempDirectory = Join-Path $fixture.context.taskRoot 'unplanned-temp'
    $launch = Resolve-Stage5PlannedPerformanceLaunch $fixture.context $binding $nativeId $fixture.titleSession
    Assert-True ($null -ne $launch -and $null -ne $launch.startInfo) `
        'The real immutable-plan resolver must produce the actual ProcessStartInfo without launching.'
    $info = $launch.startInfo
    Assert-True ($info -is [Diagnostics.ProcessStartInfo] -and
        $info.EnvironmentVariables['RTS_PERFORMANCE_RUN_ID'] -ceq $nativeId -and
        $info.EnvironmentVariables['RTS_PERFORMANCE_RUN_NONCE'] -ceq $nativeNonce -and
        $info.EnvironmentVariables['RTS_STAGE5_RUN_NONCE'] -ceq $nativeNonce) `
        'Actual launch identity must come from reopened original bytes, not an edited parsed object or a new GUID.'
    Assert-True ($info.EnvironmentVariables['TEMP'] -ceq (Join-Path $nativeRoot 'temp') -and
        $info.EnvironmentVariables['TMP'] -ceq (Join-Path $nativeRoot 'temp') -and
        $info.EnvironmentVariables['USERPROFILE'] -ceq $fixture.titleSession.sessionRoot) `
        'Role TEMP/TMP must override the shared title-session temp values without rewriting the title profile.'
    Assert-True ($info.EnvironmentVariables['RTS_PERFORMANCE_RECEIPT_DIR'] -ceq (Join-Path $nativeRoot 'receipt') -and
        $info.EnvironmentVariables['RTS_PERFORMANCE_RAW_LOG_PATH'] -ceq (Join-Path $nativeRoot 'game-owned-raw.log') -and
        $info.EnvironmentVariables['RTS_FRAME_TIMING_DIR'] -ceq (Join-Path $nativeRoot 'timing')) `
        'Actual child output destinations must be the frozen role paths.'
    Assert-True (-not (Test-Path -LiteralPath $nativeRoot)) `
        'Resolving launch identity and ProcessStartInfo must not create the role directory.'
    # Change actual bytes but keep the original binding: acceptance must fail
    # before role directory creation. A successfully rehashed replacement is not
    # the originally persisted prelaunch capability.
    $changed = Read-TestJson $binding.path
    $changed.entries[0].runNonce = '30000000-0000-4000-8000-000000000099'
    Write-Json $binding.path $changed
    $rejected = $false
    try { Resolve-Stage5PlannedPerformanceLaunch $fixture.context $binding $nativeId $fixture.titleSession | Out-Null }
    catch { $rejected = $true }
    Assert-True ($rejected -and -not (Test-Path -LiteralPath $nativeRoot)) `
        'Changed frozen plan bytes must fail before output directory creation or Process.Start.'
}

function Test-Stage5AttemptFailureClosure {
    param([string]$Root)
    $literal = New-Stage5LiteralPrelaunchPlanFixture $Root
    $fixture = $literal.fixture
    Register-Stage5TestInputLocks $fixture
    $binding = $literal.binding
    $plan = Read-Stage5PhaseBoundJson $binding $fixture.context.taskRoot 'attempt closure plan'
    $script:Stage5AttemptTestBoundaryCalls = 0
    $script:Stage5AttemptTestStartObserved = $false
    # The sole external boundary is replaced. Real plan, sequential orchestration,
    # atomic writer, start/result files and independent readers remain exercised.
    # This never returns a fake successful game receipt or validator result.
    function Invoke-Stage5InstalledPerformanceRun {
        param($Context, $PlanBinding, $EntryId, $TitleSessionContract)
        ++$script:Stage5AttemptTestBoundaryCalls
        $original = Read-Stage5PhaseBoundJson $PlanBinding $Context.taskRoot 'boundary original plan'
        $planned = @($original.entries | Where-Object { $_.entryId -ceq $EntryId })[0]
        $start = Read-TestJson $planned.outputPaths.attemptStartPath
        $script:Stage5AttemptTestStartObserved = ($original.entries.Count -eq 100 -and
            $start.event -ceq 'attempt-start' -and $start.entryId -ceq $EntryId -and
            $start.runNonce -ceq $planned.runNonce -and $start.planSha256 -ceq $PlanBinding.sha256)
        throw 'synthetic Process.Start failure after durable start'
    }
    $summary = Invoke-Stage5PerformanceRunPlan $fixture.context $binding $fixture.titleSession
    Assert-True ($script:Stage5AttemptTestBoundaryCalls -eq 1 -and $script:Stage5AttemptTestStartObserved) `
        'The complete plan and immutable attempt-start must exist before the real launch boundary is invoked.'
    Assert-True ($null -ne $summary -and $null -ne $summary.failure -and
        $summary.failure.message -match 'synthetic Process.Start failure after durable start') `
        'The execution summary must preserve the original launch failure; it is never a qualifying result.'
    Assert-True ($summary.outcomes.Count -eq 100 -and
        @($summary.outcomes | Where-Object { $_.state -ceq 'failed' }).Count -eq 1 -and
        @($summary.outcomes | Where-Object { $_.state -ceq 'not-attempted' }).Count -eq 99 -and
        @($summary.outcomes | Where-Object { $_.state -ceq 'completed' }).Count -eq 0) `
        'A failed first launch must retain one failed and all 99 unattempted planned roles, with no retry.'
    foreach ($entry in $plan.entries) {
        $result = Read-TestJson $entry.outputPaths.attemptResultPath
        Assert-True ($result.event -ceq 'attempt-result' -and $result.entryId -ceq $entry.entryId -and
            $result.runNonce -ceq $entry.runNonce -and $result.planSha256 -ceq $binding.sha256) `
            'Every retained terminal record must preserve its exact planned identity.'
        if ($entry.entryId -ceq $plan.entries[0].entryId) {
            Assert-True ($result.state -ceq 'failed' -and $null -ne $result.startBinding -and
                $result.startBinding.sha256 -ceq (Get-Sha256 $entry.outputPaths.attemptStartPath)) `
                'The failed launch result must bind its independently persisted original start.'
        }
        else {
            Assert-True ($result.state -ceq 'not-attempted' -and $null -eq $result.startBinding -and
                $null -eq $result.run -and -not (Test-Path -LiteralPath $entry.outputPaths.attemptStartPath)) `
                'Unattempted roles must not invent process/start evidence or disappear from closure.'
        }
    }
}

function New-Stage5LiteralPrelaunchPlanFixture {
    param([string]$Root)
    $fixture=New-Stage5PrelaunchContractFixture $Root
    $context=$fixture.context
    $entries=@(); $sequence=0
    foreach ($inputFixture in $context.fixtures) {
        foreach ($lane in @(@{name='forced-one';workers=1},@{name='physical-2';workers=2},@{name='physical-4';workers=4})) {
            for ($ordinal=0;$ordinal -lt 4;++$ordinal) {
                $sourceId=$null
                $selected=($inputFixture.id -ceq 'dense-eight-player' -and $lane.name -ceq 'physical-4')
                $roles=@('throughput','serial-oracle')
                if ($selected) { $roles+= 'phase-serial-baseline' }
                foreach ($role in $roles) {
                    ++$sequence
                    $id='literal-role-{0:D3}' -f $sequence
                    $nonce='40000000-0000-4000-8000-{0:D12}' -f $sequence
                    $runRoot=Join-Path $context.taskRoot $id
                    $receiptDirectory=Join-Path $runRoot 'receipt'
                    $profileId=if($role -ceq 'phase-serial-baseline'){'dense-local-physical4'}else{$null}
                    # Hand-declared argument order and explicit quoted replay path;
                    # no call to the production command builder for expected data.
                    $replayArgument=if($inputFixture.path -match '\s'){'"'+$inputFixture.path+'"'}else{$inputFixture.path}
                    $arguments='-headless -noFPSLimit -pipelineMode serial -simulationMode parallel -workerPolicy auto -validationExecutableSha256 '+$context.executableSha256+' -workerCount '+$lane.workers+' -replay '+$replayArgument
                    $entry=[pscustomobject][ordered]@{
                        entryId=$id; measurementRole=$role; profileId=$profileId
                        fixtureId=$inputFixture.id; lane=$lane.name; ordinal=$ordinal; warmup=($ordinal -eq 0)
                        sourceEntryId=if($role -ceq 'throughput'){$null}else{$sourceId}
                        runNonce=$nonce; workerCount=$lane.workers; expectedArgumentString=$arguments
                        outputPaths=[pscustomobject][ordered]@{
                            runRoot=$runRoot; receiptDirectory=$receiptDirectory
                            rawLogPath=(Join-Path $runRoot 'game-owned-raw.log')
                            timingDirectory=(Join-Path $runRoot 'timing')
                            stdoutPath=(Join-Path $runRoot 'host-stdout.log'); stderrPath=(Join-Path $runRoot 'host-stderr.log')
                            tempDirectory=(Join-Path $runRoot 'temp')
                            attemptTracePath=if($role -ceq 'throughput' -and $selected){Join-Path $receiptDirectory 'attempt-trace.bin'}else{$null}
                            attemptStartPath=(Join-Path $context.taskRoot ('attempts/'+$id+'.start.json'))
                            attemptResultPath=(Join-Path $context.taskRoot ('attempts/'+$id+'.result.json'))
                            sourceBindingPath=if($role -ceq 'phase-serial-baseline'){Join-Path $context.taskRoot ('bindings/'+$id+'.source.json')}else{$null}
                        }
                    }
                    $entries+=$entry
                    if($role -ceq 'throughput'){$sourceId=$id}
                }
            }
        }
    }
    $plan=[pscustomobject][ordered]@{
        schemaVersion=1; title=$context.title; qualificationMode=$context.qualificationMode; taskRoot=$context.taskRoot
        cohortNonce=$context.cohortNonce; cohortCreatedUtc=$context.cohortCreatedUtc; sourceCommit=$context.sourceCommit
        executablePath=$context.executablePath; executableSha256=$context.executableSha256
        artifactSetSha256=$context.artifactSetSha256; runtimeClosure=$context.runtimeClosure
        fixtureManifestPath=$context.fixtureManifestPath; fixtureManifestSha256=$context.fixtureManifestSha256
        fixtures=$context.fixtures; referencePolicy=$context.referencePolicy; warmupRuns=1; measuredRuns=3; timeoutSeconds=60
        titleSessionEnvironment=$fixture.titleSession.environmentValues; phaseBaselineProfiles=$fixture.profiles
        outputFilePolicy='native-runid-pid-receipt-pid-tick-timing-v1'; entries=$entries
    }
    $path=Join-Path $context.taskRoot 'phase-plan.json'
    Write-Json $path $plan
    return [pscustomobject]@{
        fixture=$fixture; plan=$plan; binding=[pscustomobject]@{path=$path;sha256=(Get-Sha256 $path)}
    }
}

function Test-Stage5PrelaunchPlanRejections {
    param([string]$Root)
    $literal=New-Stage5LiteralPrelaunchPlanFixture $Root
    Register-Stage5TestInputLocks $literal.fixture
    $originalBytes=[IO.File]::ReadAllBytes($literal.binding.path)
    $failures=New-Object 'Collections.Generic.List[string]'
    $cases=@(
        @{name='duplicate-id';change={param($p)$p.entries[1].entryId=$p.entries[0].entryId}},
        @{name='duplicate-nonce';change={param($p)$p.entries[1].runNonce=$p.entries[0].runNonce}},
        @{name='nonce-array';change={param($p)$p.entries[0].runNonce=@($p.entries[0].runNonce)}},
        @{name='id-dot-segment';change={param($p)$p.entries[0].entryId='literal..escape'}},
        @{name='worker-argument-conflict';change={param($p)$p.entries[0].workerCount=4}},
        @{name='command-substitution';change={param($p)$p.entries[0].expectedArgumentString+=' -unplanned'}},
        @{name='missing-warmup';change={param($p)$p.entries=@($p.entries|Select-Object -Skip 1)}},
        @{name='wrong-source-role';change={param($p)$p.entries[3].sourceEntryId=$p.entries[1].entryId}},
        @{name='different-run-root';change={param($p)$p.entries[0].outputPaths.runRoot=Join-Path $p.taskRoot 'unplanned'}},
        @{name='shared-receipt-directory';change={param($p)$p.entries[1].outputPaths.receiptDirectory=$p.entries[0].outputPaths.receiptDirectory}},
        @{name='trace-outside-receipt-bundle';change={param($p)$e=@($p.entries|Where-Object{$null -ne $_.outputPaths.attemptTracePath})[0];$e.outputPaths.attemptTracePath=Join-Path $e.outputPaths.runRoot 'attempt-trace.bin'}},
        @{name='unknown-output-field';change={param($p)$p.entries[0].outputPaths|Add-Member NoteProperty alternateOutputPath 'unplanned'}},
        @{name='unplanned-title-environment';change={param($p)$p.titleSessionEnvironment|Add-Member NoteProperty RTS_PERFORMANCE_RUN_ID 'override'}},
        @{name='changed-cohort';change={param($p)$p.cohortNonce='50000000-0000-4000-8000-000000000001'}},
        @{name='changed-runtime-closure';change={param($p)$p.runtimeClosure.closureSha256='AB'*32}}
    )
    foreach($case in $cases){
        try{
            [IO.File]::WriteAllBytes($literal.binding.path,$originalBytes)
            $changed=Read-TestJson $literal.binding.path
            & $case.change $changed
            Write-Json $literal.binding.path $changed
            # Rehash deliberately: this isolates semantic validation instead of
            # allowing the original-hash mismatch to mask an invalid plan.
            $changedBinding=[pscustomobject]@{path=$literal.binding.path;sha256=(Get-Sha256 $literal.binding.path)}
            $rejected=$false
            try{Resolve-Stage5PlannedPerformanceLaunch $literal.fixture.context $changedBinding 'literal-role-001' $literal.fixture.titleSession|Out-Null}
            catch{$rejected=$true}
            if(-not $rejected){$failures.Add($case.name+' was accepted')|Out-Null}
            if(Test-Path -LiteralPath (Join-Path $literal.fixture.context.taskRoot 'literal-role-001')){
                $failures.Add($case.name+' created a role directory')|Out-Null
            }
        }
        catch{$failures.Add($case.name+': '+$_.Exception.Message)|Out-Null}
        finally{[IO.File]::WriteAllBytes($literal.binding.path,$originalBytes)}
    }
    Assert-True ($failures.Count -eq 0) ($failures.ToArray()-join ' | ')
}

function Test-Stage5AttemptPublicationFaults {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root|Out-Null
    $failures=New-Object 'Collections.Generic.List[string]'
    foreach($fault in @('start','result')){
        try{
            & {
            param($fault)
            $literal=New-Stage5LiteralPrelaunchPlanFixture (Join-Path $Root $fault)
            Register-Stage5TestInputLocks $literal.fixture
            $first=$literal.plan.entries[0]
            $faultPath=if($fault -ceq 'start'){$first.outputPaths.attemptStartPath}else{$first.outputPaths.attemptResultPath}
            $realWriter=(Get-Command Write-Stage5JsonAtomically -CommandType Function).ScriptBlock
            $script:Stage5FaultBoundaryCalls=0
            function Write-Stage5JsonAtomically {
                param([string]$Path,[object]$Value,[switch]$CreateNew)
                if($Path -ceq $faultPath){throw ('synthetic '+$fault+' publication failure')}
                & $realWriter $Path $Value -CreateNew:$CreateNew
            }
            function Invoke-Stage5InstalledPerformanceRun {
                param($Context,$PlanBinding,$EntryId,$TitleSessionContract)
                ++$script:Stage5FaultBoundaryCalls
                throw 'synthetic original launch failure'
            }
            $summary=$null; $errorText=''
            try{$summary=Invoke-Stage5PerformanceRunPlan $literal.fixture.context $literal.binding $literal.fixture.titleSession}
            catch{$errorText=$_.Exception.Message}
            if($null -ne $summary -and $null -ne $summary.failure){$errorText+=' '+$summary.failure.message}
            Assert-True ($errorText -match ('synthetic '+$fault+' publication failure')) `
                'Publication errors must be returned or thrown; they cannot produce a successful execution summary.'
            if($fault -ceq 'start'){
                Assert-True ($script:Stage5FaultBoundaryCalls -eq 0) `
                    'Failed attempt-start publication must prevent the installed launch boundary.'
            }
            else{
                Assert-True ($script:Stage5FaultBoundaryCalls -eq 1 -and
                    (Test-Path -LiteralPath $first.outputPaths.attemptStartPath) -and
                    -not (Test-Path -LiteralPath $first.outputPaths.attemptResultPath) -and
                    $errorText -match 'synthetic original launch failure') `
                    'Failed result persistence must retain durable start and primary launch failure without retry or fabricated terminal success.'
            }
            Assert-True (-not (Test-Path -LiteralPath (Join-Path $literal.fixture.context.taskRoot 'Stage5PerformanceLocalCapacitySmoke.json'))) `
                'A publication failure must not create a passed aggregate.'
            } $fault
        }
        catch{$failures.Add($fault+': '+$_.Exception.Message)|Out-Null}
    }
    Assert-True ($failures.Count -eq 0) ($failures.ToArray()-join ' | ')
}

function Test-Stage5ReceiptSingleSnapshotBoundary {
    param([string]$Root)
    $manifest=New-ValidationFixture $Root 'LocalCapacitySmoke'
    $document=Read-TestJson $manifest
    $run=$document.runs[0]
    $targetPath=$run.receiptPath
    $originalHash=$run.receiptSha256
    $realHash=(Get-Command Get-Stage5PerformanceSha256 -CommandType Function).ScriptBlock
    $realSnapshot=Get-Command Get-Stage5FinalAcceptanceFileSnapshot -CommandType Function
    $script:Stage5ReceiptReplaced=$false
    function Set-Stage5ReceiptAfterFirstRead {
        param([string]$Path)
        if($Path -ceq $targetPath -and -not $script:Stage5ReceiptReplaced){
            $changed=Read-TestJson $targetPath
            $changed.producer='changed-after-first-read'
            Write-Json $targetPath $changed
            $script:Stage5ReceiptReplaced=$true
        }
    }
    function Get-Stage5PerformanceSha256 {
        param([string]$Path)
        $result=& $realHash $Path
        Set-Stage5ReceiptAfterFirstRead $Path
        return $result
    }
    function Get-Stage5FinalAcceptanceFileSnapshot {
        param([string]$Path,[string]$Context,[switch]$HashOnly)
        $result=& $realSnapshot $Path $Context -HashOnly:$HashOnly
        Set-Stage5ReceiptAfterFirstRead $Path
        return $result
    }
    # Current code hashes then reparses altered bytes and fails. The corrected
    # path snapshots once, validates/parses those bytes, and keeps their binding.
    $validated=Assert-Stage5Receipt $run $document @{} @{} @{} @{}
    Assert-True ($script:Stage5ReceiptReplaced -and $validated.receiptSha256 -ceq $originalHash -and
        (Get-Sha256 $targetPath) -cne $originalHash -and $validated.receiptBinding.runId -ceq $run.runId) `
        'Receipt identity and hash must describe one immutable copied read even when the path changes immediately afterward.'
}

function Test-Stage5RawEvidenceSingleSnapshotBoundary {
    param([string]$Root)
    $manifest = New-ValidationFixture $Root 'LocalCapacitySmoke'
    $document = Read-TestJson $manifest
    $run = $document.runs[0]
    $receipt = Read-TestJson $run.receiptPath
    $targetPath = [IO.Path]::GetFullPath([string]$receipt.rawEvidence.rawLogPath)
    $originalHash = [string]$receipt.rawEvidence.rawLogSha256
    $realSnapshot = (Get-Command Get-Stage5FinalAcceptanceFileSnapshot `
        -CommandType Function).ScriptBlock
    $script:Stage5RawEvidenceReplaced = $false
    function Get-Stage5FinalAcceptanceFileSnapshot {
        param([string]$Path,[string]$Context,[switch]$HashOnly)
        $result = & $realSnapshot $Path $Context -HashOnly:$HashOnly
        if ([IO.Path]::GetFullPath($Path) -ceq $targetPath -and
            -not $script:Stage5RawEvidenceReplaced) {
            $changed = [IO.File]::ReadAllText($targetPath).Replace(
                'final_crc=12345678', 'final_crc=87654321')
            [IO.File]::WriteAllText($targetPath, $changed)
            $script:Stage5RawEvidenceReplaced = $true
        }
        return $result
    }
    $validated = Assert-Stage5Receipt $run $document @{} @{} @{} @{}
    Assert-True ($script:Stage5RawEvidenceReplaced -and
        $validated.rawLogSha256 -ceq $originalHash -and
        (Get-Sha256 $targetPath) -cne $originalHash) `
        'Raw diagnostic parsing and hashing must describe one immutable copied read even when the path changes immediately afterward.'
}

function Test-Stage5HeldInputCapabilityRequired {
    param([string]$Root)
    $literal = New-Stage5LiteralPrelaunchPlanFixture $Root
    $savedRegistrations = if ($null -ne (Get-Variable -Name Stage5HeldInputRegistrations `
            -Scope Script -ErrorAction SilentlyContinue)) {
        @($script:Stage5HeldInputRegistrations)
    } else { @() }
    $script:Stage5HeldInputRegistrations = @()
    $locks = $null
    try {
        $rejected = $false
        try {
            Read-Stage5PlannedPerformanceInputs $literal.fixture.context `
                $literal.binding $literal.plan | Out-Null
        }
        catch { $rejected = $_.Exception.Message -match 'held|immutable|capability' }
        Assert-True $rejected `
            'A frozen plan must fail closed when no exact live immutable input capability is registered.'

        $context = $literal.fixture.context
        $artifact = Read-Stage5PerformanceArtifactSet $context.artifactSetManifestPath `
            $context.artifactSetSha256 $context.sourceCommit $context.title `
            $context.executablePath $context.executableSha256
        $locks = Open-Stage5PerformanceReadOnlyLocks $artifact $context.fixtures `
            $context.fixtureManifestPath
        $projection = Read-Stage5PlannedPerformanceInputs $context `
            $literal.binding $literal.plan
        Assert-True ($projection.fixtures.Count -eq 4) `
            'An exact live held-input capability must retain the normal frozen-plan projection.'
    }
    finally {
        if ($null -ne $locks) { Dispose-Stage5PerformanceReadOnlyLocks $locks }
        $script:Stage5HeldInputRegistrations = $savedRegistrations
    }
}

function Test-Stage5CleanupFailureWithholdsAggregate {
    param([string]$Root)
    $fixtureCohort = Resolve-Stage5PerformanceExecutionCohort `
        'LocalCapacitySmoke' '' '' $false $false
    $manifest=New-ValidationFixture -Root $Root -Mode 'LocalCapacitySmoke' `
        -FixtureCohortNonce $fixtureCohort.nonce `
        -FixtureCohortCreatedUtc $fixtureCohort.createdUtc
    $context=Read-TestJson $manifest
    $taskFull=$context.taskRoot; $Title=$context.title
    $executableFull=$context.executablePath; $runtimeFull=Split-Path -Parent $executableFull
    $ExpectedExecutableSha256=$context.executableSha256; $ExpectedSourceCommit=$context.sourceCommit
    $ExpectedArtifactSetSha256=$context.artifactSetSha256; $ExpectedFixtureManifestSha256=$context.fixtureManifestSha256
    $QualificationMode='LocalCapacitySmoke'; $ReferencePolicy='throughput-only'; $MeasuredRuns=3
    $hostTopology=$context.topology
    $isExternalQualification = $QualificationMode -ceq 'External16Core'
    $isInstalledKernelExecution = $QualificationMode -ceq 'InstalledKernelExecution'
    $executionCohort = $fixtureCohort
    $PhaseBaselineProfiles = @()
    $script:Stage5CurrentProcessStarted = $false
    $script:Stage5CurrentProcessIdentity = $null
    $baseline=$null
    $performanceData=$null
    $fixtureProduction=$null
    $phaseAttemptManifestPath=$null
    $phaseAttemptManifestBinding=$null
    $validationManifestPath=$null
    $validationManifestBinding=$null
    $stagedPhaseProfilePath=$null
    $stagedPerformanceDataPath=$null
    $attemptManifestBinding=$null
    $aggregateSha256=$null
    $authoritativeEvidence=$null
    $artifactBinding=Read-Stage5PerformanceArtifactSet $context.artifactSetManifestPath `
        $context.artifactSetSha256 $context.sourceCommit $Title $executableFull $ExpectedExecutableSha256
    $fixtureManifest=[pscustomobject]@{path=$manifest;fixtures=$context.fixtures}
    $launcherContract=Get-Stage5LauncherContract $runtimeFull $executableFull
    $titleSession=New-Stage5TitleSessionContract $Title (Join-Path $taskFull 'TitleSession') $runtimeFull $taskFull
    New-Item -ItemType Directory -Path $titleSession.profileRoot -Force|Out-Null
    $profileBefore=Get-Stage5ProfileTreeHash $titleSession.profileRoot
    $profileAfter=$null; $readOnlyLocks=$null; $aggregatePath=$null; $primaryError=$null
    $runPlanBinding=$null; $runPlanExecution=$null
    $registrySnapshots=New-Object 'Collections.Generic.List[object]'
    $registryRecoveryPath=Join-Path $taskFull 'Stage5RegistryRecovery.json'
    $validationMutex=$null
    $context|Add-Member NoteProperty processCleanup ([pscustomobject]@{
        processId=0;exitProof=$true;blocked=$false;errors=@()
    })
    $registryRecovery=[pscustomobject]@{
        path=$registryRecoveryPath
        identity=[ordered]@{}
        processIdentities=(New-Object 'Collections.Generic.List[object]')
        snapshots=(New-Object 'Collections.Generic.List[object]')
        adapter=(New-Stage5RegistryRecoveryAdapter)
    }
    Write-Json $registryRecoveryPath ([ordered]@{synthetic=$true})
    function Update-Stage5RegistryRecoveryState { param($Recovery,$State,$ChildExitProof,$NoActiveTitleProcesses) }
    $cleanupInjectionState = [pscustomobject]@{ called = $false }
    function Invoke-Stage5RegistryRecovery {
        $cleanupInjectionState.called = $true
        throw 'synthetic final registry restoration failure'
    }
    # Read-only AST extraction chooses the actual top-level run try/finally and
    # executes its tail beginning with real RunSet validation. No fake pass,
    # replacement aggregate writer, registry write or process boundary is used.
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($script:Stage5PrelaunchRunnerPath,[ref]$tokens,[ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Finalization test requires a parseable actual runner.'
    $mainTry=@($ast.EndBlock.Statements|Where-Object{$_ -is [Management.Automation.Language.TryStatementAst]})[-1]
    $tailStart=-1
    for($index=0;$index -lt $mainTry.Body.Statements.Count;++$index){
        $statement=$mainTry.Body.Statements[$index]
        if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
            $statement.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $statement.Left.VariablePath.UserPath -ceq 'validated'){$tailStart=$index;break}
    }
    Assert-True ($tailStart -ge 0) 'Finalization extraction prerequisite: locate the real validated-run assignment.'
    $tailText=@($mainTry.Body.Statements[$tailStart..($mainTry.Body.Statements.Count-1)]|ForEach-Object{$_.Extent.Text}) -join "`n"
    $catchText=@($mainTry.CatchClauses|ForEach-Object{$_.Extent.Text}) -join "`n"
    $suffix=@($ast.EndBlock.Statements|Where-Object{$_.Extent.StartOffset -gt $mainTry.Extent.EndOffset}|ForEach-Object{$_.Extent.Text}) -join "`n"
    $actualFinalization=[scriptblock]::Create("try {`n"+$tailText+"`n}`n"+$catchText+"`nfinally "+$mainTry.Finally.Extent.Text+"`n"+$suffix)
    $failure=''
    try{. $actualFinalization|Out-Null}catch{$failure=$_.Exception.Message}
    $primaryFailureMessage=if($null -eq $primaryError){''}else{$primaryError.Exception.Message}
    Assert-True ($null -eq $primaryError) `
        ('Finalization prerequisite failed before controlled cleanup: '+$primaryFailureMessage)
    Assert-True ($cleanupInjectionState.called -and
        $failure -match 'synthetic final registry restoration failure') `
        ('The controlled cleanup failure must be reached after actual synthetic RunSet validation. Observed: ' + $failure)
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $taskFull 'Stage5PerformanceLocalCapacitySmoke.json'))) `
        'A final cleanup failure must leave the passed aggregate absent, not publish it before finally.'
}

function Test-Stage5NativeRoleEnvironmentBinding {
    param([string]$Root)
    $literal=New-Stage5LiteralPrelaunchPlanFixture $Root
    Register-Stage5TestInputLocks $literal.fixture
    $ambient=@{}
    $keys=@('RTS_PERFORMANCE_ATTEMPT_TRACE_PATH','RTS_PERFORMANCE_SOURCE_RECEIPT_PATH','RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256')
    foreach($key in $keys){
        $ambient[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,'ambient-must-not-leak','Process')
    }
    try{
        $ordinary=Resolve-Stage5PlannedPerformanceLaunch $literal.fixture.context $literal.binding `
            $literal.plan.entries[0].entryId $literal.fixture.titleSession
        Assert-True ($null -ne $ordinary -and $null -ne $ordinary.startInfo) `
            'Ordinary launch must resolve its original plan before environment checks.'
        foreach($key in $keys){
            Assert-True (-not $ordinary.startInfo.EnvironmentVariables.ContainsKey($key)) `
                'Unselected throughput must not inherit record/source transport keys.'
        }
        $record=@($literal.plan.entries|Where-Object{$null -ne $_.outputPaths.attemptTracePath})[0]
        $launch=Resolve-Stage5PlannedPerformanceLaunch $literal.fixture.context $literal.binding $record.entryId $literal.fixture.titleSession
        Assert-True ($null -ne $launch -and $null -ne $launch.startInfo -and
            $launch.startInfo.EnvironmentVariables['RTS_PERFORMANCE_REFERENCE_MODE'] -ceq 'throughput-binding' -and
            $launch.startInfo.EnvironmentVariables['RTS_PERFORMANCE_ATTEMPT_TRACE_PATH'] -ceq (Join-Path $record.outputPaths.receiptDirectory 'attempt-trace.bin') -and
            -not $launch.startInfo.EnvironmentVariables.ContainsKey('RTS_PERFORMANCE_SOURCE_RECEIPT_PATH') -and
            -not $launch.startInfo.EnvironmentVariables.ContainsKey('RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256')) `
            'Only a selected record role gets its frozen receipt-bundle trace path; no inherited consume identity may survive.'
        $baseline=@($literal.plan.entries|Where-Object{$_.sourceEntryId -ceq $record.entryId -and $_.measurementRole -ceq 'phase-serial-baseline'})[0]
        $rejected=$false
        try{Resolve-Stage5PlannedPerformanceLaunch $literal.fixture.context $literal.binding $baseline.entryId $literal.fixture.titleSession|Out-Null}
        catch{$rejected=$true}
        Assert-True ($rejected -and -not (Test-Path -LiteralPath $baseline.outputPaths.runRoot)) `
            'A baseline missing its selected source binding must reject before creating its role directory.'
    }
    finally{foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$ambient[$key],'Process')}}
}

function Test-Stage5NativeArtifactSelectionBoundary {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root|Out-Null
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($script:Stage5PrelaunchRunnerPath,[ref]$tokens,[ref]$errors)
    Assert-True (@($errors).Count -eq 0) 'Artifact-selection extraction requires a parseable runner.'
    $function=@($ast.EndBlock.Statements|Where-Object{
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -ceq 'Invoke-Stage5InstalledPerformanceRun'
    })[0]
    $statements=$function.Body.EndBlock.Statements
    $start=-1
    for($index=0;$index -lt $statements.Count;++$index){
        $statement=$statements[$index]
        if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
            $statement.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $statement.Left.VariablePath.UserPath -ceq 'diagnosticText'){$start=$index;break}
    }
    Assert-True ($start -ge 0) 'Artifact-selection extraction must find the actual immutable-output and file checks.'
    $tail=[scriptblock]::Create((@($statements[$start..($statements.Count-1)]|ForEach-Object{$_.Extent.Text})-join "`n"))
    $failures=New-Object 'Collections.Generic.List[string]'
    $cases=@(
        @{name='one-receipt-and-timing';receiptCount=1;timingCount=1;receiptPid=4242;timingPid=4242;run='file-role';reject=$false},
        @{name='no-receipt';receiptCount=0;timingCount=1;receiptPid=4242;timingPid=4242;run='file-role';reject=$true},
        @{name='two-receipts';receiptCount=2;timingCount=1;receiptPid=4242;timingPid=4242;run='file-role';reject=$true},
        @{name='no-timing';receiptCount=1;timingCount=0;receiptPid=4242;timingPid=4242;run='file-role';reject=$true},
        @{name='two-timings';receiptCount=1;timingCount=2;receiptPid=4242;timingPid=4242;run='file-role';reject=$true},
        @{name='foreign-receipt-pid';receiptCount=1;timingCount=1;receiptPid=4243;timingPid=4242;run='file-role';reject=$true},
        @{name='foreign-timing-pid';receiptCount=1;timingCount=1;receiptPid=4242;timingPid=4243;run='file-role';reject=$true},
        @{name='foreign-receipt-run';receiptCount=1;timingCount=1;receiptPid=4242;timingPid=4242;run='other-role';reject=$true}
    )
    foreach($case in $cases){
        try{
            & {
                param($case)
                $runRoot=Join-Path $Root $case.name
                $receiptDirectory=Join-Path $runRoot 'receipt';$timingDirectory=Join-Path $runRoot 'timing'
                New-Item -ItemType Directory -Path $receiptDirectory,$timingDirectory|Out-Null
                $rawPath=Join-Path $runRoot 'game-owned-raw.log';$stdoutPath=Join-Path $runRoot 'host-stdout.log';$stderrPath=Join-Path $runRoot 'host-stderr.log'
                [IO.File]::WriteAllText($rawPath,'file-selection fixture')
                [IO.File]::WriteAllText($stdoutPath,'');[IO.File]::WriteAllText($stderrPath,'')
                $stdoutSnapshot=[pscustomobject]@{bytes=[byte[]]@()}
                $stderrSnapshot=[pscustomobject]@{bytes=[byte[]]@()}
                for($index=0;$index -lt $case.receiptCount;++$index){
                    [IO.File]::WriteAllText((Join-Path $receiptDirectory ('performance-receipt-'+$case.run+'-'+($case.receiptPid+$index)+'.json')),'{}')
                }
                for($index=0;$index -lt $case.timingCount;++$index){
                    [IO.File]::WriteAllText((Join-Path $timingDirectory ('frame-timing-'+$case.timingPid+'-'+(123+$index)+'.csv')), "frame,logic_ns`r`n1,100`r`n")
                }
                # A permitted trace sibling must not count as a second JSON.
                [IO.File]::WriteAllBytes((Join-Path $receiptDirectory 'attempt-trace.bin'),[byte[]]@(1,2,3))
                $Fixture=[pscustomobject]@{id='dense-eight-player'};$Lane='physical-4';$Ordinal=0
                $runId='file-role';$runNonce='80000000-0000-4000-8000-000000000001';$arguments='file-selection-only'
                $hostProcessId=4242;$hostCreationTime=[Int64]133000000000000001
                $hostExecutablePath='synthetic-file-selection.exe';$hostExecutableHash='A'*64;$hostCommandLine='file-selection-only';$exitCode=0
                $processIdentity=[pscustomobject]@{parentProcessId=42;parentCreationTimeUtc100ns=[Int64]133000000000000000}
                $stopwatch=[Diagnostics.Stopwatch]::StartNew();$stopwatch.Stop()
                # Only the real post-process file-selection tail runs. These
                # declared scalar observations never claim an actual process or
                # native receipt validity; the real receipt validator is not
                # replaced, and no test returns a fake qualified game result.
                $rejected=$false
                try{& $tail|Out-Null}catch{$rejected=$true}
                Assert-True ($rejected -eq $case.reject) ('Native file selection violated '+$case.name)
            } $case
        }
        catch{$failures.Add($case.name+': '+$_.Exception.Message)|Out-Null}
    }
    Assert-True ($failures.Count -eq 0) ($failures.ToArray()-join ' | ')
}

function Test-Stage5BoundedOutputCaptureContract {
    $exactBytes = [Text.Encoding]::UTF8.GetBytes('fatal error')
    $exactStream = New-Object IO.MemoryStream(,$exactBytes)
    try {
        $exactTask = Start-Stage5BoundedOutputCapture $exactStream `
            $exactBytes.LongLength 'exact-limit output'
        Assert-True ($exactTask.Wait(5000)) `
            'Exact-limit output capture did not complete within its test bound.'
        $captured = [byte[]]$exactTask.GetAwaiter().GetResult()
        Assert-True ($captured.Length -eq $exactBytes.Length -and
            [Linq.Enumerable]::SequenceEqual($captured, $exactBytes)) `
            'Exact-limit output capture changed the original bytes.'
        $decoded = ConvertFrom-Stage5StrictUtf8Bytes $captured 'exact-limit output'
        Assert-True ($decoded -cmatch 'fatal error') `
            'Strict decoding did not preserve an ASCII fatal diagnostic.'
        $stderrDiagnostic = ConvertFrom-Stage5StrictUtf8OutputPair `
            ([byte[]]@()) $captured
        Assert-True ($stderrDiagnostic -match $script:Stage5FatalPattern) `
            'A fatal diagnostic present only in stderr must reach the fatal gate.'
    }
    finally { $exactStream.Dispose() }

    $oversizeStream = New-Object IO.MemoryStream(,[byte[]]@(1, 2, 3, 4))
    try {
        $oversizeRejected = $false
        try {
            $oversizeTask = Start-Stage5BoundedOutputCapture $oversizeStream 3 `
                'over-limit output'
            [void]$oversizeTask.GetAwaiter().GetResult()
        }
        catch {
            $oversizeRejected = $_.Exception.GetBaseException().Message -match `
                'exceeds.*3-byte'
        }
        Assert-True $oversizeRejected `
            'Output capture must fail closed on the first byte beyond its bound.'
    }
    finally { $oversizeStream.Dispose() }

    $malformedBytes = [byte[]]@(0x66, 0x61, 0x74, 0x61, 0x6C, 0xC3,
        0x28, 0x20, 0x65, 0x72, 0x72, 0x6F, 0x72)
    $malformedStream = New-Object IO.MemoryStream(,$malformedBytes)
    try {
        $malformedRejected = $false
        try {
            $malformedTask = Start-Stage5BoundedOutputCapture $malformedStream `
                $malformedBytes.LongLength 'malformed output'
            $malformedCapture = [byte[]]$malformedTask.GetAwaiter().GetResult()
            ConvertFrom-Stage5StrictUtf8Bytes $malformedCapture `
                'malformed output' | Out-Null
        }
        catch { $malformedRejected = $_.Exception.Message -match 'strict UTF-8' }
        Assert-True $malformedRejected `
            'Malformed original output bytes must fail closed before fatal-pattern evaluation.'
    }
    finally { $malformedStream.Dispose() }
}


# Synthetic file-based reader tests only. They never claim that the test factory
# launched a process or that its post-constructed fixture is live prelaunch proof.
# The orchestration tests independently prove actual prelaunch publication order.

function Complete-Stage5TestJournalFixture {
    param([string]$Manifest)
    $Root=Split-Path -Parent $Manifest
    $manifest=$Manifest
    $document=Read-TestJson $manifest
    $oldPlan=Read-TestJson $document.phaseBaselinePlan.path
    $runById=@{}
    foreach($run in @($document.runs)+
            @($document.pairedOracleBindings|ForEach-Object{$_.oracleRun})+
            @($document.pairedPhaseBaselineBindings|ForEach-Object{$_.baselineRun})){
        $runById[$run.runId]=$run
        $receipt=Read-TestJson $run.receiptPath
        $runRoot=Join-Path $Root $run.runId
        $receiptDirectory=Join-Path $runRoot 'receipt'
        $timingDirectory=Join-Path $runRoot 'timing'
        New-Item -ItemType Directory -Path $receiptDirectory,$timingDirectory,(Join-Path $runRoot 'temp')|Out-Null
        $rawPath=Join-Path $runRoot 'game-owned-raw.log'
        $timingPath=Join-Path $timingDirectory ('frame-timing-'+$run.host.processId+'-123.csv')
        [IO.File]::WriteAllBytes($rawPath,[IO.File]::ReadAllBytes($receipt.rawEvidence.rawLogPath))
        [IO.File]::WriteAllBytes($timingPath,[IO.File]::ReadAllBytes($receipt.rawEvidence.timingPath))
        [IO.File]::WriteAllText((Join-Path $runRoot 'host-stdout.log'),'')
        [IO.File]::WriteAllText((Join-Path $runRoot 'host-stderr.log'),'')
        $receipt.rawEvidence.rawLogPath=$rawPath; $receipt.rawEvidence.timingPath=$timingPath
        $receipt.rawLogs[0].path=$rawPath; $receipt.rawLogs[1].path=$timingPath
        $run.receiptPath=Join-Path $receiptDirectory ('performance-receipt-'+$run.runId+'-'+$run.host.processId+'.json')
        $receipt.provenance.receiptPath=$run.receiptPath
        $attemptTraceProperty = $receipt.PSObject.Properties['attemptTrace']
        if($null -ne $attemptTraceProperty -and $null -ne $attemptTraceProperty.Value -and
            $attemptTraceProperty.Value.mode -ceq 'record'){
            $tracePath=Join-Path $receiptDirectory 'attempt-trace.bin'
            [IO.File]::WriteAllBytes($tracePath,[IO.File]::ReadAllBytes($attemptTraceProperty.Value.file.path))
            $attemptTraceProperty.Value.file.path=$tracePath
        }
        Write-Json $run.receiptPath $receipt
        $run.receiptSha256=Get-Sha256 $run.receiptPath
    }
    foreach($pair in $document.pairedPhaseBaselineBindings){
        $sourceRun=$runById[$pair.throughputRunId]
        $source=Read-TestJson $sourceRun.receiptPath
        $baseline=Read-TestJson $pair.baselineRun.receiptPath
        $baseline.attemptTrace.file=$source.attemptTrace.file
        $baseline.attemptTrace.sourceBinding.receipt.path=$sourceRun.receiptPath
        $baseline.attemptTrace.sourceBinding.receipt.sha256=$sourceRun.receiptSha256
        Write-Json $pair.baselineRun.receiptPath $baseline
        $pair.baselineRun.receiptSha256=Get-Sha256 $pair.baselineRun.receiptPath
    }
    $fixturePath=Join-Path $Root 'reviewed-fixtures.json'
    Write-Json $fixturePath ([pscustomobject]@{
        schemaVersion=1; evidenceKind='stage5-performance-scaling-fixtures'; title=$document.title
        executableSha256=$document.executableSha256
        fixtures=@($document.fixtures|ForEach-Object{
            [pscustomobject]@{id=$_.id;source=[IO.Path]::GetFileName($_.path);sha256=$_.sha256;seed=$_.seed;playerCount=$_.playerCount;peakUnitCount=$_.peakUnitCount}
        })
    })
    $document|Add-Member NoteProperty fixtureManifestPath $fixturePath -Force
    $document.fixtureManifestSha256=Get-Sha256 $fixturePath
    $titleSession=New-Stage5TitleSessionContract $document.title (Join-Path $Root 'TitleSession') `
        (Split-Path -Parent $document.executablePath) $Root
    $orderedOldEntries=@()
    foreach($run in $document.runs){
        $orderedOldEntries+=@($oldPlan.entries|Where-Object{$_.entryId -ceq $run.runId})
        $orderedOldEntries+=@($oldPlan.entries|Where-Object{$_.sourceEntryId -ceq $run.runId})
    }
    $entries=@()
    foreach($oldEntry in $orderedOldEntries){
        $run=$runById[$oldEntry.entryId]
        $receipt=Read-TestJson $run.receiptPath
        $attemptTraceProperty = $receipt.PSObject.Properties['attemptTrace']
        $runRoot=Join-Path $Root $run.runId
        $entry=$oldEntry
        $entry|Add-Member NoteProperty runNonce $run.runNonce
        $entry|Add-Member NoteProperty workerCount ([int]$receipt.worker.requestedCount)
        $entry|Add-Member NoteProperty expectedArgumentString $run.expectedArgumentString
        $entry|Add-Member NoteProperty outputPaths ([pscustomobject][ordered]@{
            runRoot=$runRoot;receiptDirectory=(Join-Path $runRoot 'receipt');rawLogPath=(Join-Path $runRoot 'game-owned-raw.log')
            timingDirectory=(Join-Path $runRoot 'timing');stdoutPath=(Join-Path $runRoot 'host-stdout.log');stderrPath=(Join-Path $runRoot 'host-stderr.log')
            tempDirectory=(Join-Path $runRoot 'temp')
            attemptTracePath=if($null -ne $attemptTraceProperty -and
                $null -ne $attemptTraceProperty.Value -and
                $attemptTraceProperty.Value.mode -ceq 'record'){
                    $attemptTraceProperty.Value.file.path
                }else{$null}
            attemptStartPath=(Join-Path $Root ('attempts/'+$run.runId+'.start.json'))
            attemptResultPath=(Join-Path $Root ('attempts/'+$run.runId+'.result.json'))
            sourceBindingPath=if($entry.measurementRole -ceq 'phase-serial-baseline'){Join-Path $Root ('bindings/'+$run.runId+'.source.json')}else{$null}
        })
        $entries+=$entry
    }
    $plan=[pscustomobject][ordered]@{
        schemaVersion=1;title=$document.title;qualificationMode=$document.qualificationMode;taskRoot=$Root
        cohortNonce=$document.cohortNonce;cohortCreatedUtc=$document.cohortCreatedUtc;sourceCommit=$document.sourceCommit
        executablePath=$document.executablePath;executableSha256=$document.executableSha256
        artifactSetSha256=$document.artifactSetSha256;runtimeClosure=$document.runtimeClosure
        fixtureManifestPath=$document.fixtureManifestPath;fixtureManifestSha256=$document.fixtureManifestSha256;fixtures=$document.fixtures
        referencePolicy=$document.referencePolicy;warmupRuns=1;measuredRuns=3;timeoutSeconds=60
        titleSessionEnvironment=$titleSession.environmentValues;phaseBaselineProfiles=$document.phaseBaselineProfiles
        outputFilePolicy='native-runid-pid-receipt-pid-tick-timing-v1';entries=$entries
    }
    if ($document.PSObject.Properties.Name -contains 'performanceData') {
        $plan | Add-Member NoteProperty performanceData ([pscustomobject]@{
            path = [string]$document.performanceData.path
            sha256 = [string]$document.performanceData.sha256
            closureSha256 = [string]$document.performanceData.closureSha256
            fileCount = [int]$document.performanceData.fileCount
        })
    }
    Write-Json $document.phaseBaselinePlan.path $plan
    $document.phaseBaselinePlan.sha256=Get-Sha256 $document.phaseBaselinePlan.path
    New-Item -ItemType Directory -Path (Join-Path $Root 'attempts'),(Join-Path $Root 'bindings')|Out-Null
    $outcomes=@()
    foreach($entry in $plan.entries){
        $run=$runById[$entry.entryId]
        $sourceBinding=$null
        if($entry.measurementRole -ceq 'phase-serial-baseline'){
            $native=Read-TestJson $run.receiptPath
            # Existing native sourceBinding grammar only. This sidecar is audit
            # binding, never a new native parser input or host tuple authority.
            Write-Json $entry.outputPaths.sourceBindingPath $native.attemptTrace.sourceBinding
            $sourceBinding=[pscustomobject]@{path=$entry.outputPaths.sourceBindingPath;sha256=(Get-Sha256 $entry.outputPaths.sourceBindingPath)}
        }
        $start=[pscustomobject][ordered]@{
            schemaVersion=1;event='attempt-start';planSha256=$document.phaseBaselinePlan.sha256
            entryId=$entry.entryId;runNonce=$entry.runNonce;recordedUtc='2026-09-01T00:00:00.0000000Z';sourceBinding=$sourceBinding
        }
        Write-Json $entry.outputPaths.attemptStartPath $start
        $startBinding=[pscustomobject]@{path=$entry.outputPaths.attemptStartPath;sha256=(Get-Sha256 $entry.outputPaths.attemptStartPath)}
        $result=[pscustomobject][ordered]@{
            schemaVersion=1;event='attempt-result';planSha256=$document.phaseBaselinePlan.sha256
            entryId=$entry.entryId;runNonce=$entry.runNonce;recordedUtc='2026-09-01T00:00:02.0000000Z'
            startBinding=$startBinding;state='completed';failure=$null;run=$run
            processCleanup=[pscustomobject]@{processId=$run.host.processId;exitProof=$true;blocked=$false;errors=@()}
        }
        Write-Json $entry.outputPaths.attemptResultPath $result
        $outcomes+=[pscustomobject]@{
            entryId=$entry.entryId;state='completed';failure=$null;startBinding=$startBinding
            resultBinding=[pscustomobject]@{path=$entry.outputPaths.attemptResultPath;sha256=(Get-Sha256 $entry.outputPaths.attemptResultPath)}
        }
    }
    Write-Json $document.phaseBaselineAttemptManifest.path ([pscustomobject][ordered]@{
        schemaVersion=1;planSha256=$document.phaseBaselinePlan.sha256;outcomes=$outcomes;cohortFailure=$null
    })
    $document.phaseBaselineAttemptManifest.sha256=Get-Sha256 $document.phaseBaselineAttemptManifest.path
    Write-Json $manifest $document
    return [pscustomobject]@{manifest=$manifest;document=$document;plan=$plan}
}

function Export-Stage5AuthoritativePerformanceFixture {
    param([string]$Root, [string]$ArtifactSetManifestPath,
        [string]$SourceCommit, [string]$CohortNonce,
        [string]$CohortCreatedUtc)
    $fixture = New-Stage5AuthoritativeValidationFixture $Root `
        $ArtifactSetManifestPath $SourceCommit $CohortNonce $CohortCreatedUtc
    $document = $fixture.document
    $validated = Assert-Stage5PerformanceRunSet $document

    $phaseProfilePath = Join-Path $Root `
        'Stage5PerformancePhaseBaselineProfile.json'
    Write-Json $phaseProfilePath $document.phaseBaselineProfiles[0]
    $phaseProfileSha256 = Get-Sha256 $phaseProfilePath
    $fixtureManifest = Read-TestJson $document.fixtureManifestPath

    $stage3SourceCommit = 'b' * 40
    $stage3ExecutableSha256 = 'C' * 64
    $stage3Fixtures = @()
    foreach ($fixtureEntry in @($fixtureManifest.fixtures)) {
        $stage3Fixtures += [ordered]@{
            id = $fixtureEntry.id
            fixtureSha256 = $fixtureEntry.sha256
            playerCount = 8
            peakUnitCount = $fixtureEntry.peakUnitCount
            wallMilliseconds = @(101.0, 100.0, 100.0, 100.0)
        }
    }
    $stage3Path = Join-Path $Root 'Stage3PerformanceBaseline.json'
    Write-Json $stage3Path ([ordered]@{
        schemaVersion = 1; stage = 'Stage3'; architecture = 'x64'
        title = 'ZeroHour'; executableSha256 = $stage3ExecutableSha256
        fixtureManifestSha256 = $document.fixtureManifestSha256
        configuration = 'parallel-1'; physicalCoreCount = 16
        availableCpus = 16; logicalProcessorCount = 16; warmupRuns = 1
        fixtures = $stage3Fixtures
    })
    $stage3Sha256 = Get-Sha256 $stage3Path
    $stage3Baseline = [pscustomobject]@{
        path = $stage3Path; sha256 = $stage3Sha256
        executableSha256 = $stage3ExecutableSha256
        stage3SourceCommit = $stage3SourceCommit
        fixtures = @($stage3Fixtures | ForEach-Object {
            [pscustomobject]@{
                id = $_.id; rawWallMilliseconds = @($_.wallMilliseconds)
                measuredMedianMilliseconds = 100.0
            }
        })
    }

    $performanceData = [pscustomobject]@{
        path = [string]$document.performanceData.path
        manifestSha256 = [string]$document.performanceData.sha256
        closureSha256 = [string]$document.performanceData.closureSha256
        fileCount = [int]$document.performanceData.fileCount
        runtimeRoot = [string]$document.performanceData.runtimeRoot
    }
    $recordedUtc = '2026-09-01T00:00:03.0000000Z'
    $hostAggregate = [ordered]@{
        schemaVersion = 2
        evidenceKind = 'stage5-performance-scaling-host-qualification'
        producer = 'Invoke-Stage5PerformanceScalingValidation.ps1'
        status = 'passed'; recordedUtc = $recordedUtc
        cohortNonce = $CohortNonce; cohortCreatedUtc = $CohortCreatedUtc
        qualificationMode = 'External16Core'
        qualificationClass = 'external-16-core-qualification'
        measurementMode = 'headless-throughput'
        referencePolicy = 'paired-serial-oracle-v1'; installedRuntime = $true
        sourceCommit = $SourceCommit
        artifactSetSha256 = $fixture.artifactBinding.sha256
        artifactSetManifest = [ordered]@{
            path = $fixture.artifactBinding.path
            sha256 = $fixture.artifactBinding.sha256
        }
        runtimeClosure = [ordered]@{
            dependencyManifestPath =
                $fixture.artifactBinding.runtimeClosure.dependencyManifestPath
            dependencyManifestSha256 =
                $fixture.artifactBinding.runtimeClosure.dependencyManifestSha256
            closureSha256 = $fixture.artifactBinding.runtimeClosure.closureSha256
            fileCount = $fixture.artifactBinding.runtimeClosure.fileCount
        }
        title = 'ZeroHour'
        executable = [ordered]@{
            path = $fixture.artifactBinding.executablePath
            sha256 = $fixture.artifactBinding.executableHash
        }
        fixtureManifest = [ordered]@{
            path = $document.fixtureManifestPath
            sha256 = $document.fixtureManifestSha256
        }
        stage3Baseline = [ordered]@{
            path = $stage3Path; sha256 = $stage3Sha256
            sourceCommit = $stage3SourceCommit
            executableSha256 = $stage3ExecutableSha256
        }
        schedule = [ordered]@{ warmupRuns = 1; measuredRuns = 3 }
        topology = $document.topology
        thresholds = [ordered]@{
            maximumForcedOneRegressionRatio = 1.05
            minimumPhysical8Speedup = 2.0
            minimumPhysical8To16SpeedupExclusive = 1.0
        }
        nativeReceiptBindings = @($validated.runs | ForEach-Object {
            $_.receiptBinding
        })
        pairedOracleBindings = @($validated.pairedOracleBindings)
        fixtures = @($validated.fixtures); runs = @($validated.runs)
        phaseBaselinePolicy = $validated.phaseBaselinePolicy
        phaseBaselineProfiles = @($document.phaseBaselineProfiles)
        phaseBaselineProfile = [ordered]@{
            path = $phaseProfilePath; sha256 = $phaseProfileSha256
        }
        performanceData = [ordered]@{
            path = $performanceData.path
            sha256 = $performanceData.manifestSha256
            closureSha256 = $performanceData.closureSha256
            fileCount = $performanceData.fileCount
        }
        phaseBaselinePlan = $document.phaseBaselinePlan
        phaseBaselineAttemptManifest = $document.phaseBaselineAttemptManifest
        pairedPhaseBaselineBindings = @($validated.pairedPhaseBaselineBindings)
    }
    $hostPath = Join-Path $Root 'Stage5PerformanceScalingQualification.json'
    Write-Json $hostPath $hostAggregate
    $hostSha256 = Get-Sha256 $hostPath
    $context = [pscustomobject]@{
        qualificationMode = 'External16Core'
        referencePolicy = 'paired-serial-oracle-v1'
        phaseBaselineProfiles = @($document.phaseBaselineProfiles)
        measuredRuns = 3; warmupRuns = 1; topology = $document.topology
        taskRoot = $Root; sourceCommit = $SourceCommit
        artifactSetSha256 = $fixture.artifactBinding.sha256
        executableSha256 = $fixture.artifactBinding.executableHash
        title = 'ZeroHour'; stage3SourceCommit = $stage3SourceCommit
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 =
                $fixture.artifactBinding.runtimeClosure.dependencyManifestSha256
            closureSha256 = $fixture.artifactBinding.runtimeClosure.closureSha256
        }
        cohortNonce = $CohortNonce; cohortCreatedUtc = $CohortCreatedUtc
    }
    $result = New-Stage5AuthoritativePerformanceEvidence $context `
        $hostAggregate $hostPath $hostSha256 $stage3Baseline $fixtureManifest `
        $phaseProfilePath $phaseProfileSha256 $performanceData
    return [pscustomobject]@{
        finalPath = $result.finalPath; rawPath = $result.rawPath
        stage3BaselinePath = $stage3Path
        phaseBaselineProfilePath = $phaseProfilePath
        cohortNonce = $CohortNonce; cohortCreatedUtc = $CohortCreatedUtc
        runtimeClosure = $context.runtimeClosure
    }
}

function New-Stage5ClosedJournalReaderFixture {
    param([string]$Root)
    $manifest=New-Stage5PhaseValidationFixture $Root
    $document=Read-TestJson $manifest
    return [pscustomobject]@{manifest=$manifest;document=$document;plan=(Read-TestJson $document.phaseBaselinePlan.path)}
}

function Update-Stage5TestTerminalBinding {
    param([object]$Document,[int]$Index=0)
    $attempts=Read-TestJson $Document.phaseBaselineAttemptManifest.path
    $attempts.outcomes[$Index].resultBinding.sha256=Get-Sha256 $attempts.outcomes[$Index].resultBinding.path
    Write-Json $Document.phaseBaselineAttemptManifest.path $attempts
    $Document.phaseBaselineAttemptManifest.sha256=Get-Sha256 $Document.phaseBaselineAttemptManifest.path
}

function Update-Stage5TestStartBinding {
    param([object]$Document,[int]$Index=0)
    $attempts=Read-TestJson $Document.phaseBaselineAttemptManifest.path
    $outcome=$attempts.outcomes[$Index]
    $outcome.startBinding.sha256=Get-Sha256 $outcome.startBinding.path
    $result=Read-TestJson $outcome.resultBinding.path
    $result.startBinding=$outcome.startBinding
    Write-Json $outcome.resultBinding.path $result
    $outcome.resultBinding.sha256=Get-Sha256 $outcome.resultBinding.path
    Write-Json $Document.phaseBaselineAttemptManifest.path $attempts
    $Document.phaseBaselineAttemptManifest.sha256=Get-Sha256 $Document.phaseBaselineAttemptManifest.path
}

function Test-Stage5JournalReaderMutationClosure {
    param([string]$Root)
    $fixture=New-Stage5ClosedJournalReaderFixture $Root
    $failures=New-Object 'Collections.Generic.List[string]'
    try{
        $validated=Assert-Stage5PerformanceRunSet $fixture.document
        Assert-True ($validated.runs.Count -eq 48 -and $validated.pairedPhaseBaselineBindings.Count -eq 4 -and
            $validated.fixtures[3].laneMedians.'physical-4' -eq 40.0) `
            'Complete immutable start/result closure must retain the existing 48+4 synthetic cohort and throughput-only median.'
    }
    catch{$failures.Add('complete-record positive: '+$_.Exception.Message)|Out-Null}
    $startPath=$fixture.plan.entries[0].outputPaths.attemptStartPath
    $resultPath=$fixture.plan.entries[0].outputPaths.attemptResultPath
    $manifestPath=$fixture.document.phaseBaselineAttemptManifest.path
    $baselineIndex=-1
    for($index=0;$index -lt $fixture.plan.entries.Count;++$index){
        if($fixture.plan.entries[$index].measurementRole -ceq 'phase-serial-baseline'){$baselineIndex=$index;break}
    }
    Assert-True ($baselineIndex -ge 0) 'The closed journal fixture must contain its selected baseline.'
    $baselineEntry=$fixture.plan.entries[$baselineIndex]
    $baselineStart=$baselineEntry.outputPaths.attemptStartPath
    $baselineResult=$baselineEntry.outputPaths.attemptResultPath
    $sourceBindingPath=$baselineEntry.outputPaths.sourceBindingPath
    $restoredPaths=@($startPath,$resultPath,$manifestPath,$baselineStart,$baselineResult,$sourceBindingPath)
    $saved=@{}; $savedHashes=@{}
    foreach($path in $restoredPaths){
        $saved[$path]=[IO.File]::ReadAllBytes($path); $savedHashes[$path]=Get-Sha256 $path
    }
    $cases=@(
        @{name='start-only-missing-result';change={param($d)[IO.File]::Move($resultPath,$resultPath+'.held')}},
        @{name='missing-start';change={param($d)[IO.File]::Move($startPath,$startPath+'.held')}},
        @{name='missing-final-manifest';change={param($d)[IO.File]::Move($manifestPath,$manifestPath+'.held')}},
        @{name='changed-result-bytes';change={param($d)$r=Read-TestJson $resultPath;$r.runNonce='60000000-0000-4000-8000-000000000001';Write-Json $resultPath $r}},
        @{name='rehashed-result-wrong-nonce';change={param($d)$r=Read-TestJson $resultPath;$r.runNonce='60000000-0000-4000-8000-000000000001';Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='rehashed-result-wrong-entry';change={param($d)$r=Read-TestJson $resultPath;$r.entryId='unplanned-entry';Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='rehashed-result-failed-summary-completed';change={param($d)$r=Read-TestJson $resultPath;$r.state='failed';$r.failure=[pscustomobject]@{stage='execution';message='retained failure'};Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='result-stripped-start';change={param($d)$r=Read-TestJson $resultPath;$r.startBinding=$null;Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='result-unproven-cleanup';change={param($d)$r=Read-TestJson $resultPath;$r.processCleanup.exitProof=$false;$r.processCleanup.blocked=$true;Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='result-changed-receipt-binding';change={param($d)$r=Read-TestJson $resultPath;$r.run.receiptSha256='CD'*32;Write-Json $resultPath $r;Update-Stage5TestTerminalBinding $d}},
        @{name='rehashed-start-wrong-nonce';change={param($d)$s=Read-TestJson $startPath;$s.runNonce='70000000-0000-4000-8000-000000000001';Write-Json $startPath $s;Update-Stage5TestStartBinding $d}},
        @{name='missing-baseline-source-binding';change={param($d)[IO.File]::Move($sourceBindingPath,$sourceBindingPath+'.held')}},
        @{name='rehashed-source-binding-substitution';change={param($d)$s=Read-TestJson $sourceBindingPath;$s.runId='unselected-source';Write-Json $sourceBindingPath $s;$a=Read-TestJson $baselineStart;$a.sourceBinding.sha256=Get-Sha256 $sourceBindingPath;Write-Json $baselineStart $a;Update-Stage5TestStartBinding $d $baselineIndex}},
        @{name='dropped-outcome';change={param($d)$m=Read-TestJson $manifestPath;$m.outcomes=@($m.outcomes|Select-Object -Skip 1);Write-Json $manifestPath $m;$d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $manifestPath}},
        @{name='duplicate-outcome';change={param($d)$m=Read-TestJson $manifestPath;$m.outcomes[1]=$m.outcomes[0];Write-Json $manifestPath $m;$d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $manifestPath}},
        @{name='summary-substitutes-other-result';change={param($d)$m=Read-TestJson $manifestPath;$m.outcomes[0].resultBinding=$m.outcomes[1].resultBinding;Write-Json $manifestPath $m;$d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $manifestPath}},
        @{name='cleanup-failure-all-child-records-completed';change={param($d)$m=Read-TestJson $manifestPath;$m.cohortFailure=[pscustomobject]@{stage='cleanup';message='retained restoration failure'};Write-Json $manifestPath $m;$d.phaseBaselineAttemptManifest.sha256=Get-Sha256 $manifestPath}}
    )
    foreach($case in $cases){
        try{
            $document=Read-TestJson $fixture.manifest
            & $case.change $document
            $rejected=$false
            try{Assert-Stage5PerformanceRunSet $document|Out-Null}catch{$rejected=$true}
            if(-not $rejected){$failures.Add($case.name+' was accepted')|Out-Null}
            if($case.name -ceq 'cleanup-failure-all-child-records-completed'){
                Assert-True ((Get-Sha256 $startPath) -ceq $savedHashes[$startPath] -and
                    (Get-Sha256 $resultPath) -ceq $savedHashes[$resultPath]) `
                    'A cohort cleanup failure must not rewrite original completed child start/result records.'
            }
        }
        catch{$failures.Add($case.name+': '+$_.Exception.Message)|Out-Null}
        finally{
            foreach($path in $restoredPaths){
                # Only exact task-created regular files are moved; never recurse
                # or follow a link. Restore original fixture bytes for the next
                # independent mutation, not production evidence or a real run.
                if(Test-Path -LiteralPath ($path+'.held')){[IO.File]::Move($path+'.held',$path)}
                [IO.File]::WriteAllBytes($path,$saved[$path])
            }
        }
    }
    Assert-True ($failures.Count -eq 0) ($failures.ToArray()-join ' | ')
}

function Test-Stage5RegistryJournalExistingFileUpdate {
    param([string]$Root)
    $journalPath = Join-Path $Root 'synthetic-recovery-journal-update.json'
    $userSid = 'S-1-5-21-1-2-3-1000'
    $identity = [ordered]@{
        runNonce = '11111111-1111-4111-8111-111111111111'
        title = 'ZeroHour'; taskRoot = $Root; journalPath = $journalPath
        userSid = $userSid
        mutexName = Get-Stage5RegistryRecoveryMutexName $userSid
        identityMode = 'acceptance-bound'
        runnerScriptSha256 = ('A' * 64)
        executableSha256 = ('B' * 64)
        sourceCommit = ('c' * 40)
        artifactSetSha256 = ('D' * 64)
    }
    $installKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
    $snapshots = @(
        (New-Stage5RegistryRecoverySnapshot -Title ZeroHour -View Registry32 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue '%STAGE5_TEST_DOCUMENTS%\Documents' `
            -OldKind ([Microsoft.Win32.RegistryValueKind]::ExpandString) `
            -ExpectedValue 'H:\Installed\ZeroHour\' `
            -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String)),
        (New-Stage5RegistryRecoverySnapshot -Title ZeroHour -View Registry64 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $false `
            -ExpectedValue 'H:\Installed\ZeroHour\' `
            -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String))
    )
    $identity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ZeroHour -PlannedMissingSubKeys @() -Snapshots $snapshots
    New-Stage5RegistryRecoveryJournal -Path $journalPath -Identity $identity `
        -PlannedMissingSubKeys @() -Snapshots $snapshots `
        -ProcessIdentities @() | Out-Null
    $pendingJournal = Read-TestJson $journalPath
    Assert-True ($pendingJournal.state -ceq 'planned' -and
        @($pendingJournal.snapshots).Count -eq 2 -and
        $pendingJournal.snapshots[0].oldValue.type -ceq 'ExpandString' -and
        $pendingJournal.snapshots[0].oldValue.value -ceq '%STAGE5_TEST_DOCUMENTS%\Documents' -and
        [int]$pendingJournal.snapshots[0].oldKind -eq [int][Microsoft.Win32.RegistryValueKind]::ExpandString -and
        -not [bool]$pendingJournal.snapshots[1].hadValue) `
        'Initial recovery journal must preserve raw expansion, type, and absence.'
    $pendingHash = Get-Sha256 $journalPath
    try {
        # This second write must exercise replacement of an existing file;
        # a fresh-file-only test misses the PowerShell null-string binding bug.
        Update-Stage5RegistryRecoveryJournal -Path $journalPath `
            -ExpectedIdentity $identity -State restored -Snapshots $snapshots `
            -ChildExitProof $true -NoActiveTitleProcesses $true `
            -ProcessIdentities @() | Out-Null
    }
    catch {
        Assert-True ((Get-Sha256 $journalPath) -ceq $pendingHash) `
            'Failed journal replacement must leave the pending recovery data intact.'
        throw
    }
    $restoredJournal = Read-TestJson $journalPath
    Assert-True ($restoredJournal.state -ceq 'restored' -and
        [bool]$restoredJournal.childExitProof -and
        [bool]$restoredJournal.noActiveTitleProcesses -and
        @($restoredJournal.processIdentities).Count -eq 0 -and
        @($restoredJournal.snapshots).Count -eq 2 -and
        $restoredJournal.snapshots[0].oldValue.type -ceq 'ExpandString' -and
        $restoredJournal.snapshots[0].oldValue.value -ceq '%STAGE5_TEST_DOCUMENTS%\Documents' -and
        [int]$restoredJournal.snapshots[0].oldKind -eq [int][Microsoft.Win32.RegistryValueKind]::ExpandString -and
        -not [bool]$restoredJournal.snapshots[1].hadValue -and
        $null -eq $restoredJournal.snapshots[1].oldValue -and
        $null -eq $restoredJournal.snapshots[1].oldKind) `
        'Existing recovery journal must atomically publish restored state without changing originals.'
    Assert-True (@(Get-ChildItem -LiteralPath $Root -File `
            -Filter 'synthetic-recovery-journal-update.json.tmp-*').Count -eq 0) `
        'Successful journal replacement must not leave a temporary recovery file.'
}

function Invoke-Stage5ExecutionCohortResolverUnderTest {
    param(
        [string]$RunnerPath,
        [string]$Mode,
        [string]$Nonce,
        [string]$CreatedUtc,
        [bool]$NonceSupplied,
        [bool]$CreatedUtcSupplied
    )
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $RunnerPath, [ref]$tokens, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) `
        'The performance runner must parse before its cohort resolver can be exercised.'
    $functions = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
        @('Assert-Stage5PerformanceCondition',
            'Resolve-Stage5PerformanceExecutionCohort') -ccontains $_.Name
    })
    Assert-True ($functions.Count -eq 2) `
        'The performance runner must expose its real execution-cohort resolver to host contract tests.'
    $definitions = @($functions | Sort-Object {
        if ($_.Name -ceq 'Assert-Stage5PerformanceCondition') { 0 } else { 1 }
    } | ForEach-Object { $_.Extent.Text }) -join "`n"
    $harness = [scriptblock]::Create(@"
param(`$Mode, `$Nonce, `$CreatedUtc, `$NonceSupplied, `$CreatedUtcSupplied)
$definitions
Resolve-Stage5PerformanceExecutionCohort `$Mode `$Nonce `$CreatedUtc `$NonceSupplied `$CreatedUtcSupplied
"@)
    return & $harness $Mode $Nonce $CreatedUtc $NonceSupplied $CreatedUtcSupplied
}

function Test-Stage5ExecutionCohortInputContract {
    param([string]$RunnerPath)
    $runnerCommand = Get-Command -Name $RunnerPath -CommandType ExternalScript
    Assert-True ($runnerCommand.Parameters.ContainsKey('ExecutionCohortNonce') -and
        $runnerCommand.Parameters.ContainsKey('ExecutionCohortCreatedUtc')) `
        'The public performance runner must accept the shared workflow execution-cohort pair.'

    $nonce = '8a7b6c5d-4e3f-4a2b-9c8d-7e6f5a4b3c2d'
    $createdUtc = '2026-09-04T12:34:56.1234567Z'
    $external = Invoke-Stage5ExecutionCohortResolverUnderTest $RunnerPath `
        'External16Core' $nonce $createdUtc $true $true
    Assert-True ($external.nonce -ceq $nonce -and
        $external.createdUtc -ceq $createdUtc -and
        [bool]$external.externallySupplied) `
        'External qualification must preserve the exact canonical workflow cohort pair.'

    $rejections = @(
        [pscustomobject]@{ name='external-missing-pair'; mode='External16Core'
            nonce=''; created=''; nonceSupplied=$false; createdSupplied=$false },
        [pscustomobject]@{ name='external-nonce-only'; mode='External16Core'
            nonce=$nonce; created=''; nonceSupplied=$true; createdSupplied=$false },
        [pscustomobject]@{ name='external-created-only'; mode='External16Core'
            nonce=''; created=$createdUtc; nonceSupplied=$false; createdSupplied=$true },
        [pscustomobject]@{ name='uppercase-uuid'; mode='External16Core'
            nonce=$nonce.ToUpperInvariant(); created=$createdUtc
            nonceSupplied=$true; createdSupplied=$true },
        [pscustomobject]@{ name='non-v4-uuid'; mode='External16Core'
            nonce='8a7b6c5d-4e3f-1a2b-9c8d-7e6f5a4b3c2d'; created=$createdUtc
            nonceSupplied=$true; createdSupplied=$true },
        [pscustomobject]@{ name='short-fraction-timestamp'; mode='External16Core'
            nonce=$nonce; created='2026-09-04T12:34:56Z'
            nonceSupplied=$true; createdSupplied=$true },
        [pscustomobject]@{ name='offset-timestamp'; mode='External16Core'
            nonce=$nonce; created='2026-09-04T12:34:56.1234567+00:00'
            nonceSupplied=$true; createdSupplied=$true },
        [pscustomobject]@{ name='local-external-pair'; mode='LocalCapacitySmoke'
            nonce=$nonce; created=$createdUtc
            nonceSupplied=$true; createdSupplied=$true }
    )
    foreach ($case in $rejections) {
        $rejected = $false
        try {
            Invoke-Stage5ExecutionCohortResolverUnderTest $RunnerPath $case.mode `
                $case.nonce $case.created $case.nonceSupplied `
                $case.createdSupplied | Out-Null
        }
        catch { $rejected = $true }
        Assert-True $rejected `
            "Execution-cohort input '$($case.name)' was accepted."
    }

    $localFirst = Invoke-Stage5ExecutionCohortResolverUnderTest $RunnerPath `
        'LocalCapacitySmoke' '' '' $false $false
    $localSecond = Invoke-Stage5ExecutionCohortResolverUnderTest $RunnerPath `
        'LocalCapacitySmoke' '' '' $false $false
    Assert-True (-not [bool]$localFirst.externallySupplied -and
        $localFirst.nonce -cmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
        $localFirst.createdUtc -cmatch
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        $localFirst.nonce -cne $localSecond.nonce) `
        'Local-capacity smoke must mint a fresh canonical non-acceptance cohort.'
}

function Test-Stage5ProfileClosureContract {
    $modulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
    $module = Import-Module $modulePath -Force -PassThru
    $limits = [ordered]@{
        maximumBytes = 1048576
        maximumRecords = 100000
        maximumLogicalEvents = 100000
        maximumAttempts = 10000
        maximumRanges = 500000
    }
    $profile = [ordered]@{
        profileId = 'dense-external-forced-one'
        fixtureId = 'dense-eight-player'
        sourceLane = 'forced-one'
        sourcePolicySha256 = ('7A' * 32)
        limits = $limits
        residentAttemptCapacity = 15
        residentRangeCapacity = 340
        fixtureSha256 = ('8B' * 32)
        window = [ordered]@{
            firstCompletedFrame = 1
            lastCompletedFrame = 100
            completedFrameCount = 100
            controlWindowCount = 1
        }
        warmupRuns = 1
        measuredRuns = 3
    }
    $identity = & $module {
        param($runId, $runNonce, $processId, $creation)
        Get-Stage5PerformancePhaseRunIdentitySha256 $runId $runNonce `
            $processId $creation
    } 'profile-source-run' '9d129c64-353f-4cd2-b7f9-3ea697325251' 123 456789
    $receipt = [ordered]@{
        schemaVersion = 6
        measurementRole = 'throughput'
        runId = 'profile-source-run'
        runNonce = '9d129c64-353f-4cd2-b7f9-3ea697325251'
        process = [ordered]@{ id = 123; creationTimeUtc100ns = 456789 }
        executableSha256 = ('9C' * 32)
        fixture = [ordered]@{ contentSha256 = ('8B' * 32) }
        workload = [ordered]@{ sampleCount = 100 }
        attemptTrace = [ordered]@{
            schemaVersion = 1
            encoding = 'typed-canonical-le-v1'
            fieldSchema = 20481
            mode = 'record'
            frozen = $true
            complete = $true
            errors = 0
            observationIngressSealed = $true
            executionClosureSealed = $true
            file = [ordered]@{
                path = 'H:\profile-trace.bin'
                sha256 = ('6D' * 32)
                byteCount = 4096
            }
            binding = [ordered]@{
                nativeRunIdentitySha256 = $identity
                executableSha256 = ('9C' * 32)
                fixtureSha256 = ('8B' * 32)
                sourcePolicySha256 = ('7A' * 32)
            }
            limits = $limits
            sourceBinding = $null
            residentAttemptCapacity = 15
            residentRangeCapacity = 340
            residentAttemptCount = 0
            residentAttemptHighWater = 2
            residentRangeCount = 0
            residentRangeHighWater = 3
            recordCount = 101
            logicalEventCount = 101
            coalescedSpanCount = 0
            coalescedAttemptCount = 0
            attemptCount = 4
            admittedAttemptCount = 2
            notAdmittedAttemptCount = 2
            abortedAfterAdmissionAttemptCount = 1
            reapCount = 4
            capturedAttemptCount = 2
            capturedOperationCount = 5
            dispatchCount = 2
            rangeCount = 5
            releasedRangeCount = 5
            windowBoundaryCount = 101
            completedWindowCount = 100
            controlWindowCount = 1
        }
    }
    & $module {
        param($candidateProfile, $candidateReceipt)
        [void](Assert-Stage5PerformancePhaseBaselineProfile $candidateProfile `
            $candidateProfile.fixtureSha256 1 3 'Focused reviewed profile')
        [void](Assert-Stage5PerformancePhaseTraceSemantics $candidateReceipt `
            $candidateProfile 'Focused reviewed trace')
    } $profile $receipt

    $mutations = @(
        { param($p, $r) $r.attemptTrace.binding.sourcePolicySha256 = ('AA' * 32) },
        { param($p, $r) $r.attemptTrace.limits.maximumRecords++ },
        { param($p, $r) $r.attemptTrace.residentAttemptCapacity++ },
        { param($p, $r) $p.window.completedFrameCount-- }
    )
    foreach ($mutation in $mutations) {
        $changedProfile = $profile | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $changedReceipt = $receipt | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        & $mutation $changedProfile $changedReceipt
        $rejected = $false
        try {
            & $module {
                param($candidateProfile, $candidateReceipt)
                [void](Assert-Stage5PerformancePhaseBaselineProfile `
                    $candidateProfile ('8B' * 32) 1 3 `
                    'Focused changed reviewed profile')
                [void](Assert-Stage5PerformancePhaseTraceSemantics `
                    $candidateReceipt $candidateProfile `
                    'Focused changed reviewed trace')
            } $changedProfile $changedReceipt
        }
        catch { $rejected = $true }
        Assert-True $rejected `
            'A rehashed profile/trace policy, limit, capacity, or window detachment was accepted.'
    }
}

function Assert-Stage5JsonReaderMutationRejected {
    param(
        [string]$Path,
        [string]$Name,
        [scriptblock]$Mutation,
        [scriptblock]$Reader
    )
    $originalBytes = [IO.File]::ReadAllBytes($Path)
    try {
        $document = Read-TestJson $Path
        & $Mutation $document
        Write-Json $Path $document
        $caught = $null
        try { & $Reader $Path (Get-Sha256 $Path) | Out-Null }
        catch { $caught = $_ }
        Assert-True ($null -ne $caught) `
            "JSON scalar mutation '$Name' was accepted by its reader."
    }
    finally {
        [IO.File]::WriteAllBytes($Path, $originalBytes)
    }
}

function New-Stage5JsonScalarTypingFixtures {
    param([string]$Root, [object]$ValidDocument)
    New-Item -ItemType Directory -Path $Root -Force | Out-Null

    $fixtureRoot = Join-Path $Root 'fixtures'
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $fixtureRows = @()
    foreach ($fixture in @($ValidDocument.fixtures)) {
        $fixturePath = Join-Path $fixtureRoot ("{0}.rep" -f $fixture.id)
        [IO.File]::WriteAllText($fixturePath, "scalar-typing-$($fixture.id)")
        $fixtureRows += [ordered]@{
            id = $fixture.id
            source = [IO.Path]::GetFileName($fixturePath)
            sha256 = Get-Sha256 $fixturePath
            seed = 7
            playerCount = 8
            peakUnitCount = $fixture.peakUnitCount
        }
    }
    $fixtureManifestPath = Join-Path $fixtureRoot 'fixtures.json'
    Write-Json $fixtureManifestPath ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-performance-scaling-fixtures'
        title = 'ZeroHour'
        executableSha256 = $ValidDocument.executableSha256
        fixtures = $fixtureRows
    })

    $dataRoot = Join-Path $Root 'qualification-runtime'
    New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null
    $dataRows = @()
    foreach ($relative in @('Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini', 'Data/Scripts/SkirmishScripts.scb',
            'INIZH.big', 'MapsZH.big', 'W3DZH.big')) {
        $dataPath = Join-Path $dataRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $dataPath) `
            -Force | Out-Null
        [IO.File]::WriteAllText($dataPath, "scalar-typing-data-$relative")
        $dataRows += [ordered]@{
            path = $relative
            sha256 = Get-Sha256 $dataPath
            length = [Int64](Get-Item -LiteralPath $dataPath).Length
        }
    }
    $dataLines = @($dataRows | ForEach-Object {
        '{0}|{1}|{2}' -f $_.path, $_.sha256, $_.length
    })
    $dataClosure = Get-Sha256Text (($dataLines -join "`n") + "`n")
    $qualificationDataPath = Join-Path $Root 'qualification-data.json'
    Write-Json $qualificationDataPath ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-performance-qualification-data'
        producer = 'genci-r2-trimmed-data-v1'
        sourceCommit = $ValidDocument.sourceCommit
        title = 'ZeroHour'
        archiveSource = [ordered]@{
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
        runtimeRoot = $dataRoot
        files = $dataRows
        closureSha256 = $dataClosure
    })

    $baselineRows = @()
    foreach ($fixture in @($fixtureRows)) {
        $baselineRows += [ordered]@{
            id = $fixture.id
            fixtureSha256 = $fixture.sha256
            playerCount = 8
            peakUnitCount = $fixture.peakUnitCount
            wallMilliseconds = @(101.0, 100.0, 100.0, 100.0)
        }
    }
    $baselinePath = Join-Path $Root 'stage3-baseline.json'
    Write-Json $baselinePath ([ordered]@{
        schemaVersion = 1
        stage = 'Stage3'
        architecture = 'x64'
        title = 'ZeroHour'
        executableSha256 = $ValidDocument.executableSha256
        fixtureManifestSha256 = $ValidDocument.fixtureManifestSha256
        configuration = 'parallel-1'
        physicalCoreCount = 16
        availableCpus = 16
        logicalProcessorCount = 16
        warmupRuns = 1
        fixtures = $baselineRows
    })
    return [pscustomobject]@{
        fixtureManifestPath = $fixtureManifestPath
        fixtureRows = $fixtureRows
        qualificationDataPath = $qualificationDataPath
        qualificationDataRows = $dataRows
        qualificationDataClosure = $dataClosure
        dataRoot = $dataRoot
        baselinePath = $baselinePath
        baselineRows = $baselineRows
    }
}

function Test-Stage5JsonScalarTypingReaders {
    param([string]$Root, [object]$ValidDocument)
    $fixtures = New-Stage5JsonScalarTypingFixtures $Root $ValidDocument
    $sourceCommit = [string]$ValidDocument.sourceCommit
    $title = [string]$ValidDocument.title
    $executablePath = [string]$ValidDocument.executablePath
    $executableHash = [string]$ValidDocument.executableSha256

    $artifactPath = [string]$ValidDocument.artifactSetManifestPath
    $artifactHash = [string]$ValidDocument.artifactSetSha256
    [void](Read-Stage5PerformanceArtifactSet $artifactPath $artifactHash `
        $sourceCommit $title $executablePath $executableHash)
    foreach ($values in @(
            @($sourceCommit),
            @($sourceCommit, 'array-tail')
        )) {
        Assert-Stage5JsonReaderMutationRejected $artifactPath `
            ('artifact-sourceCommit-array-' + $values.Count) `
            { param($d) $d.sourceCommit = $values } `
            { param($p, $h) Read-Stage5PerformanceArtifactSet $p $h `
                $sourceCommit $title $executablePath $executableHash }
    }
    Assert-Stage5JsonReaderMutationRejected $artifactPath `
        'artifact-architecture-array' `
        { param($d) $d.architecture = @('x64', 'array-tail') } `
        { param($p, $h) Read-Stage5PerformanceArtifactSet $p $h `
            $sourceCommit $title $executablePath $executableHash }
    Assert-Stage5JsonReaderMutationRejected $artifactPath `
        'artifact-product-array-element' `
        { param($d) $d.productSet[0] = @('Generals') } `
        { param($p, $h) Read-Stage5PerformanceArtifactSet $p $h `
            $sourceCommit $title $executablePath $executableHash }
    Assert-Stage5JsonReaderMutationRejected $artifactPath `
        'artifact-entry-path-array' `
        { param($d) $d.artifacts[0].path = @($d.artifacts[0].path) } `
        { param($p, $h) Read-Stage5PerformanceArtifactSet $p $h `
            $sourceCommit $title $executablePath $executableHash }

    $fixturePath = $fixtures.fixtureManifestPath
    $fixtureHash = Get-Sha256 $fixturePath
    $validatedFixtureManifest = Read-Stage5ScalingFixtureManifest $fixturePath $fixtureHash `
        $title $executableHash
    foreach ($field in @('title', 'executableSha256')) {
        Assert-Stage5JsonReaderMutationRejected $fixturePath `
            ('fixture-' + $field + '-array') `
            { param($d) $d.$field = @($d.$field) } `
            { param($p, $h) Read-Stage5ScalingFixtureManifest $p $h `
                $title $executableHash }
    }
    Assert-Stage5JsonReaderMutationRejected $fixturePath `
        'fixture-entry-id-array' `
        { param($d) $d.fixtures[0].id = @($d.fixtures[0].id) } `
        { param($p, $h) Read-Stage5ScalingFixtureManifest $p $h `
            $title $executableHash }
    Assert-Stage5JsonReaderMutationRejected $fixturePath `
        'fixture-entry-source-array' `
        { param($d) $d.fixtures[0].source = @($d.fixtures[0].source) } `
        { param($p, $h) Read-Stage5ScalingFixtureManifest $p $h `
            $title $executableHash }
    Assert-Stage5JsonReaderMutationRejected $fixturePath `
        'fixture-entry-hash-array' `
        { param($d) $d.fixtures[0].sha256 = @($d.fixtures[0].sha256) } `
        { param($p, $h) Read-Stage5ScalingFixtureManifest $p $h `
            $title $executableHash }

    $dataPath = $fixtures.qualificationDataPath
    $dataHash = Get-Sha256 $dataPath
    $dataReader = {
        param($p, $h)
        Read-Stage5PerformanceQualificationData $p $h `
            $fixtures.qualificationDataClosure $sourceCommit 'ZeroHour' `
            $fixtures.dataRoot $null -SkipInstalledFileValidation
    }
    [void](& $dataReader $dataPath $dataHash)
    foreach ($field in @('sourceCommit', 'title', 'closureSha256')) {
        Assert-Stage5JsonReaderMutationRejected $dataPath `
            ('qualification-' + $field + '-array') `
            { param($d) $d.$field = @($d.$field) } $dataReader
    }
    Assert-Stage5JsonReaderMutationRejected $dataPath `
        'qualification-archive-hash-array' `
        { param($d) $d.archiveSource.sha256 = @($d.archiveSource.sha256) } `
        $dataReader
    Assert-Stage5JsonReaderMutationRejected $dataPath `
        'qualification-file-path-array' `
        { param($d) $d.files[0].path = @($d.files[0].path) } $dataReader
    Assert-Stage5JsonReaderMutationRejected $dataPath `
        'qualification-file-hash-array' `
        { param($d) $d.files[0].sha256 = @($d.files[0].sha256) } $dataReader
    Assert-Stage5JsonReaderMutationRejected $dataPath `
        'qualification-file-length-fraction' `
        { param($d) $d.files[0].length = 1.5 } $dataReader
    Assert-Stage5JsonReaderMutationRejected $dataPath `
        'qualification-file-length-string' `
        { param($d) $d.files[0].length = '1' } $dataReader

    $baselinePath = $fixtures.baselinePath
    $baselineHash = Get-Sha256 $baselinePath
    $baselineReader = {
        param($p, $h)
        Read-Stage5ScalingBaseline $p $h $executableHash 'ZeroHour' `
            $ValidDocument.fixtureManifestSha256 $validatedFixtureManifest.fixtures `
            ('b' * 40)
    }
    [void](& $baselineReader $baselinePath $baselineHash)
    foreach ($field in @('stage', 'architecture', 'title', 'executableSha256',
            'fixtureManifestSha256', 'configuration')) {
        Assert-Stage5JsonReaderMutationRejected $baselinePath `
            ('baseline-' + $field + '-array') `
            { param($d) $d.$field = @($d.$field) } $baselineReader
    }
    Assert-Stage5JsonReaderMutationRejected $baselinePath `
        'baseline-fixture-id-array' `
        { param($d) $d.fixtures[0].id = @($d.fixtures[0].id) } $baselineReader
    Assert-Stage5JsonReaderMutationRejected $baselinePath `
        'baseline-fixture-hash-array' `
        { param($d) $d.fixtures[0].fixtureSha256 = @($d.fixtures[0].fixtureSha256) } `
        $baselineReader
    Assert-Stage5JsonReaderMutationRejected $baselinePath `
        'baseline-player-count-fraction' `
        { param($d) $d.fixtures[0].playerCount = 8.5 } $baselineReader
}

function Test-Stage5JsonScalarTypingNativeCandidate {
    param([string]$RunnerPath, [object]$ValidDocument)
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $RunnerPath, [ref]$tokens, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) `
        'Native candidate scalar-typing test requires a parseable validation runner.'
    $wanted = @('Assert-Condition', 'Get-Stage5FileSnapshot',
        'ConvertTo-OutputRelativePath', 'Assert-ContainedPathNoReparse',
        'ConvertTo-ProcessArgumentString',
        'Assert-Stage5NativeCommandLineMatchesPlan',
        'Get-NativePerformanceReceiptReference')
    $definitions = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $wanted -ccontains $_.Name
    } | Sort-Object {
        $wanted.IndexOf($_.Name)
    } | ForEach-Object { $_.Extent.Text })
    Assert-True ($definitions.Count -eq $wanted.Count) `
        'Native candidate scalar-typing test could not isolate all runner helpers.'

    & {
        param($DefinitionTexts, $Document)
        foreach ($definition in $DefinitionTexts) {
            . ([scriptblock]::Create($definition))
        }
        $run = @($Document.runs)[0]
        $nativePath = [string]$run.receiptPath
        $native = Read-TestJson $nativePath
        $arguments = [string[]]($run.expectedArgumentString -split ' ')
        $invoke = {
            param([object]$Receipt)
            Get-NativePerformanceReceiptReference `
                -OutputText ("SIMULATION_PERFORMANCE_RECEIPT status=written path={0}" -f $nativePath) `
                -OutputRoot $Document.taskRoot -WorkingDirectory $Document.taskRoot `
                -Role 'ai-results' -SourceCommit $Document.sourceCommit `
                -ArtifactSetSha256 $Document.artifactSetSha256 `
                -ExecutableSha256 $Document.executableSha256 `
                -RunNonce $run.runNonce -CohortNonce $Document.cohortNonce `
                -RuntimeClosure ([ordered]@{
                    dependencyManifestSha256 = $Document.runtimeClosure.dependencyManifestSha256
                    closureSha256 = $Document.runtimeClosure.closureSha256
                }) -ExpectedTitle $Document.title `
                -ProcessId ([int]$run.host.processId) `
                -ProcessCreationUtc $Receipt.provenance.processCreationUtc `
                -ExpectedExecutablePath $Document.executablePath `
                -ExpectedArguments $arguments `
                -ExpectedProducers @('game-executable-stage5-performance-report-v5') `
                -ExpectedCohortCreatedUtc $Document.cohortCreatedUtc
        }
        Assert-True ($null -ne (& $invoke $native)) `
            'The unmutated native receipt must be a positive candidate.'
        $originalBytes = [IO.File]::ReadAllBytes($nativePath)
        try {
            $mutations = @(
                [pscustomobject]@{ name='producer-one-array'; change={ param($d) $d.producer=@($d.producer) } },
                [pscustomobject]@{ name='role-one-array'; change={ param($d) $d.role=@($d.role) } },
                [pscustomobject]@{ name='title-one-array'; change={ param($d) $d.title=@($d.title) } },
                [pscustomobject]@{ name='runtime-hash-one-array'; change={ param($d) $d.runtimeClosure.closureSha256=@($d.runtimeClosure.closureSha256) } },
                [pscustomobject]@{ name='raw-log-name-one-array'; change={ param($d) $d.rawLogs[0].name=@($d.rawLogs[0].name) } },
                [pscustomobject]@{ name='raw-log-hash-one-array'; change={ param($d) $d.rawLogs[0].sha256=@($d.rawLogs[0].sha256) } },
                [pscustomobject]@{ name='raw-log-path-one-array'; change={ param($d) $d.rawLogs[0].path=@($d.rawLogs[0].path) } },
                [pscustomobject]@{ name='process-id-string'; change={ param($d) $d.provenance.processId='20001' } },
                [pscustomobject]@{ name='process-id-fraction'; change={ param($d) $d.provenance.processId=20001.5 } },
                [pscustomobject]@{ name='provenance-path-one-array'; change={ param($d) $d.provenance.executablePath=@($d.provenance.executablePath) } },
                [pscustomobject]@{ name='provenance-hash-one-array'; change={ param($d) $d.provenance.executableSha256=@($d.provenance.executableSha256) } },
                [pscustomobject]@{ name='exit-code-string'; change={ param($d) $d.provenance.exitCode='0' } },
                [pscustomobject]@{ name='exit-code-fraction'; change={ param($d) $d.provenance.exitCode=0.5 } },
                [pscustomobject]@{ name='second-identity-source-array'; change={ param($d) $d.sourceCommit=@($d.sourceCommit, 'array-tail') } },
                [pscustomobject]@{ name='second-identity-hash-array'; change={ param($d) $d.artifactSetSha256=@($d.artifactSetSha256) } }
            )
            foreach ($mutation in $mutations) {
                [IO.File]::WriteAllBytes($nativePath, $originalBytes)
                $changed = Read-TestJson $nativePath
                & $mutation.change $changed
                Write-Json $nativePath $changed
                $candidate = & $invoke (Read-TestJson $nativePath)
                Assert-True ($null -eq $candidate) `
                    "Native receipt candidate accepted '$($mutation.name)'."
            }
        }
        finally {
            [IO.File]::WriteAllBytes($nativePath, $originalBytes)
        }
    } $definitions $ValidDocument
}

$runner = Join-Path $PSScriptRoot 'Invoke-Stage5PerformanceScalingValidation.ps1'
$script:Stage5PrelaunchRunnerPath = $runner
$runnerSource = Get-Content -LiteralPath $runner -Raw
$evidenceModuleSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot `
    'DeterministicSimulationEvidence.psm1') -Raw
Test-Stage5ExecutionCohortInputContract $runner
Assert-True ($runnerSource -match '\[object\[\]\]\$PhaseBaselineProfiles = @\(\)' -and
    $runnerSource -match 'New-Stage5PerformanceRunPlan \$context \$TimeoutSeconds `\s*\r?\n\s*\$titleSession @\(\$PhaseBaselineProfiles\)' -and
    $runnerSource -notmatch 'New-Stage5PerformanceRunPlan \$context \$TimeoutSeconds \$titleSession @\(\)' -and
    $runnerSource -match '\[string\]\$QualificationMode = ''LocalCapacitySmoke''' -and
    $runnerSource -match 'Write-Stage5FinalAcceptanceFileAtomically') `
    'The public performance entrypoint must expose phase profiles and default to the bounded local-capacity lane.'
Assert-True ($runnerSource -match "'Stage5PerformanceScalingRawSamples\.json'" -and
    $runnerSource -match "'Stage5PerformanceScaling\.json'" -and
    $runnerSource -match "'Stage3PerformanceBaseline\.json'" -and
    $runnerSource -match "'Stage5PerformanceQualificationData\.json'" -and
    $runnerSource -match '\[string\]\$PerformanceDataManifestPath' -and
    $runnerSource -match '\[string\]\$ExpectedPerformanceDataManifestSha256' -and
    $runnerSource -match '\[string\]\$ExpectedPerformanceDataClosureSha256' -and
    $runnerSource -match 'Open-Stage5PerformanceReadOnlyLocks[\s\S]*?\$additionalImmutablePaths' -and
    $runnerSource -match 'installed-runtime-scaling-runner-v3' -and
    $runnerSource -match 'Read-Stage5PerformanceScalingEvidence' -and
    $runnerSource -match 'External16Core.*paired-serial-oracle-v1' -and
    $runnerSource -match 'dense-eight-player' -and
    $runnerSource -match 'phaseAccountingSamples' -and
    $runnerSource -match '''ReviewedFixtures''' -and
    $runnerSource -match
        'elseif \(\$file\.Extension -ceq ''\.rep''\) \{ ''Replay'' \}' -and
    $evidenceModuleSource -match
        'function Assert-Stage5PerformancePhaseBaselineProfile' -and
    $evidenceModuleSource -match
        'function Assert-Stage5PerformancePhaseTraceSemantics' -and
    $evidenceModuleSource -match
        '\[string\]\$ExpectedPhaseBaselineProfileSha256' -and
    $evidenceModuleSource -notmatch
        '\$inputBindings\.fixtures\[\[string\]\$native\.fixture\.id\]\s*=') `
    'External qualification must publish self-validated raw-v3/final scaling evidence from paired serial and dense one-worker phase observations.'
Assert-True ($runnerSource -notmatch
        '\[IO\.File\]::WriteAllText\(\$capture\.path' -and
    $runnerSource -notmatch '\.ReadToEndAsync\(\)' -and
    $runnerSource -match
        'Write-Stage5FinalAcceptanceFileAtomically\s+`\s*\r?\n\s*-Path \$capture\.path' -and
    $runnerSource -match
        'Start-Stage5BoundedOutputCapture\s+`\s*\r?\n\s*\$process\.StandardOutput\.BaseStream\s+\(\[Int64\]\(64MB\)\)' -and
    $runnerSource -match
        'Start-Stage5BoundedOutputCapture\s+`\s*\r?\n\s*\$process\.StandardError\.BaseStream\s+\(\[Int64\]\(64MB\)\)' -and
    $runnerSource -match
        'ConvertFrom-Stage5StrictUtf8OutputPair\s+`\s*\r?\n\s*\(\[byte\[\]\]\$stdoutSnapshot\.bytes\)\s+\(\[byte\[\]\]\$stderrSnapshot\.bytes\)' -and
    $runnerSource -notmatch
        '\$diagnosticText\s*=\s*\[IO\.File\]::ReadAllText\(\$stdoutPath\)') `
    'Performance stdout/stderr must be concurrently bounded, atomically snapshotted, and strictly decoded from those immutable bytes.'
if ($SourceContractPreflightOnly -or $ProfileClosurePreflightOnly) {
    Test-Stage5ProfileClosureContract
}
if ($ProfileClosurePreflightOnly) {
    Write-Output 'Stage 5 performance profile-closure preflight passed.'
    return
}
if ($SourceContractPreflightOnly) {
    Write-Output 'Stage 5 performance source-contract preflight passed.'
    return
}
$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
    else { 'H:\Stage5PerformanceScalingValidationScratch' }
$testRoot = Join-Path $scratchParent ('s5h-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N').Substring(0, 12))
$script:Stage5SelfTestCleanupBlocked = $false
$script:Stage5TestInputLocks = @{}
$beforeProductProcesses = @(Get-Process generalsv, generalszh -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Id })

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    $validManifest = New-ValidationFixture (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'valid'))
    $validDocument = Read-TestJson $validManifest
    foreach ($run in @($validDocument.runs)) {
        $receipt = Read-TestJson $run.receiptPath
        Assert-True (@($receipt.kernels | Where-Object {
                    [bool]$_.elapsedNanosecondsKnown -or [Int64]$_.elapsedNanoseconds -ne 0
                }).Count -eq 0) `
            'The valid external fixture must use production-shaped unknown aggregate kernel timings.'
    }
    if (-not $FinalizationPreflightOnly) {
        & $runner -SelfTestValidationManifestPath $validManifest | Out-Null
    }
    if ($ProductionTimingPreflightOnly) {
        Assert-Rejected 'production-timing-invalid-stage' {
            param($document)
            Update-Receipt $document.runs[0] {
                param($receipt)
                $receipt.kernelTiming.streams[0].stages[0].totalNanoseconds = 11
            }
        }
        Assert-Rejected 'production-timing-missing-stream' {
            param($document)
            Update-Receipt $document.runs[0] {
                param($receipt)
                $receipt.kernelTiming.streams = @($receipt.kernelTiming.streams | Select-Object -Skip 1)
            }
        }
        Assert-Rejected 'production-timing-incomplete-ledger' {
            param($document)
            Update-Receipt $document.runs[0] {
                param($receipt)
                $receipt.kernelTiming.complete = $false
            }
        }
        Write-Output 'Stage 5 production timing focused preflight passed.'
        return
    }
    # Load the runner's bounded path helpers so this regression exercises the
    # real path checks rather than only matching their source text.
    . $runner -SelfTestValidationManifestPath $validManifest | Out-Null
    if ($FinalizationPreflightOnly) {
        Test-Stage5CleanupFailureWithholdsAggregate `
            (Join-Path $testRoot 'prelaunch-finalization')
        Write-Output 'Stage 5 performance finalization preflight passed.'
        return
    }
    if ($JsonScalarTypingPreflightOnly) {
        Test-Stage5JsonScalarTypingReaders `
            (Join-Path $testRoot 'json-scalar-readers') $validDocument
        Test-Stage5JsonScalarTypingNativeCandidate `
            (Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1') `
            $validDocument
        Write-Output 'Stage 5 JSON scalar-typing focused preflight passed.'
        return
    }
    if (-not [string]::IsNullOrWhiteSpace($ExportAuthoritativeFixtureRoot)) {
        Assert-True (-not [string]::IsNullOrWhiteSpace(
                $ExportArtifactSetManifestPath) -and
            $ExportSourceCommit -cmatch '^[0-9a-f]{40}$' -and
            $ExportCohortNonce -cmatch
                '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
            $ExportCohortCreatedUtc -cmatch
                '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$') `
            'Authoritative fixture export requires exact artifact, commit, and cohort bindings.'
        $exportRoot = [IO.Path]::GetFullPath($ExportAuthoritativeFixtureRoot)
        Assert-True (-not (Test-Path -LiteralPath $exportRoot)) `
            'Authoritative fixture export root must be fresh.'
        $exportResult = Export-Stage5AuthoritativePerformanceFixture $exportRoot `
            $ExportArtifactSetManifestPath $ExportSourceCommit `
            $ExportCohortNonce $ExportCohortCreatedUtc
        Write-Output ($exportResult | ConvertTo-Json -Depth 8 -Compress)
        return
    }
    if ($OutputCapturePreflightOnly) {
        Test-Stage5BoundedOutputCaptureContract
        Test-Stage5NativeArtifactSelectionBoundary `
            (Join-Path $testRoot 'output-capture-tail')
        Write-Output 'Stage 5 bounded output-capture focused preflight passed.'
        return
    }
    if ($HeldInputPreflightOnly) {
        $heldInputFailures = New-Object 'Collections.Generic.List[string]'
        try {
            Test-Stage5HeldInputRegistrationArrayStability `
                (Join-Path $testRoot 'registration-array')
        }
        catch {
            $heldInputFailures.Add("registration array: $($_.Exception.Message)") | Out-Null
        }
        try {
            $phaseManifest = New-Stage5PhaseValidationFixture `
                (Join-Path $testRoot 'phase-capability')
            $phaseResult = Assert-Stage5PerformanceRunSet `
                (Read-TestJson $phaseManifest)
            Assert-True ($phaseResult.runs.Count -eq 48) `
                'Held-input phase preflight did not retain its complete run set.'
        }
        catch {
            $heldInputFailures.Add("phase capability: $($_.Exception.Message) [$($_.ScriptStackTrace)]") | Out-Null
        }
        Assert-True ($heldInputFailures.Count -eq 0) `
            ($heldInputFailures.ToArray() -join ' | ')
        Write-Output 'Stage 5 held-input focused preflight passed.'
        return
    }
    $safeTitleRoot = 'H:\Stage5PerformanceScalingValidationScratch\path-guard'
    Assert-True (Test-Stage5SafeTitleSessionPath "$safeTitleRoot\TitleSession" $safeTitleRoot) `
        'A normal H: title-session path must pass the bounded path guard.'
    Assert-True (Test-Stage5SafeTitleSessionPath "$safeTitleRoot\Command and Conquer Generals Data" `
            $safeTitleRoot -AllowWhitespace) `
        'A bounded H: profile path with an explicitly allowed space must pass.'
    Assert-True (-not (Test-Stage5SafeTitleSessionPath 'C:\outside-title-session')) `
        'A non-H: title-session path must fail closed.'
    $validationMutex = Acquire-Stage5ValidationMutex
    try {
        Assert-True ($null -ne $validationMutex -and $validationMutex.acquired) `
            'The installed-validation mutex must be acquired by the owning process.'
        Assert-Stage5NoInstalledTitleProcesses
    }
    finally {
        Release-Stage5ValidationMutex $validationMutex
    }
    $createdRegistrySegments = New-Object 'Collections.Generic.List[string]'
    $rolledBackRegistrySegments = New-Object 'Collections.Generic.List[string]'
    $fakeRegistryHandle = [pscustomobject]@{}
    $fakeRegistryHandle | Add-Member -MemberType ScriptMethod -Name Dispose -Value { }
    $registrySetupRejected = $false
    try {
        Invoke-Stage5RegistryTargetSetup 'Software\Stage5\Target' `
            $createdRegistrySegments `
            { param($path) return $null } `
            { param($path)
                if ($path -ceq 'Software\Stage5') {
                    throw 'synthetic registry CreateSubKey failure'
                }
                return $fakeRegistryHandle
            } `
            { param($path) return $fakeRegistryHandle } `
            { param($paths)
                foreach ($path in @($paths)) {
                    $rolledBackRegistrySegments.Add([string]$path) | Out-Null
                }
            } | Out-Null
    }
    catch {
        $registrySetupRejected = $_.Exception.Message -match
            'synthetic registry CreateSubKey failure'
    }
    Assert-True $registrySetupRejected `
        'Injected registry setup failure must remain the primary setup error.'
    Assert-True ($createdRegistrySegments.Count -eq 1 -and
        $createdRegistrySegments[0] -ceq 'Software' -and
        $rolledBackRegistrySegments.Count -eq 1 -and
        $rolledBackRegistrySegments[0] -ceq 'Software') `
        'Injected registry setup failure must rollback every newly-created segment.'
    $stopFailureProcess = [pscustomobject]@{ Id = 4242; HasExited = $false }
    $stopFailureProcess | Add-Member -MemberType ScriptMethod -Name Refresh -Value { }
    $stopFailureProcess | Add-Member -MemberType ScriptMethod -Name Kill -Value {
        throw 'synthetic owned-child Kill failure'
    }
    $stopFailureProcess | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {
        param([int]$milliseconds)
        return $false
    }
    $stopFailure = Invoke-Stage5OwnedProcessCleanup $stopFailureProcess $true $null 1
    Assert-True ($stopFailure.blocked -and -not $stopFailure.exitProof -and
        $stopFailure.errors -match 'synthetic owned-child Kill failure') `
        'Injected owned-child stop failure must become an explicit blocked cleanup state.'
    $recoveryJournalPath = Join-Path $testRoot 'synthetic-recovery-journal.json'
    $userSid = 'S-1-5-21-1-2-3-1000'
    $recoveryIdentity = [ordered]@{
        runNonce = '22222222-2222-4222-8222-222222222222'
        title = 'ZeroHour'; taskRoot = $testRoot
        journalPath = $recoveryJournalPath; userSid = $userSid
        mutexName = Get-Stage5RegistryRecoveryMutexName $userSid
        identityMode = 'acceptance-bound'
        runnerScriptSha256 = ('A' * 64)
        executableSha256 = ('B' * 64)
        sourceCommit = ('c' * 40); artifactSetSha256 = ('D' * 64)
    }
    $recoveryInstallKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
    $recoveryCreated = @(
        'Software', 'Software\Electronic Arts',
        'Software\Electronic Arts\EA Games', $recoveryInstallKey)
    $recoverySnapshot = New-Stage5RegistryRecoverySnapshot -Title ZeroHour `
        -View Registry64 -SubKey $recoveryInstallKey -Name InstallPath `
        -HadKey $false -HadValue $false `
        -ExpectedValue 'H:\Installed\ZeroHour\' `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -CreatedSubKeys $recoveryCreated
    $recoveryPlanned = @($recoveryCreated | ForEach-Object { "Registry64|$_" })
    $blockedProcessIdentity = [ordered]@{
        launchPending = $false; processId = 4242
        creationTimeUtc100ns = 638925120000000000
        executablePath = 'H:\Installed\generalszh.exe'
        executableSha256 = ('B' * 64)
    }
    $recoveryIdentity.snapshotPlanSha256 =
        Get-Stage5RegistryRecoverySnapshotPlanSha256 -Title ZeroHour `
            -PlannedMissingSubKeys $recoveryPlanned `
            -Snapshots @($recoverySnapshot)
    New-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
        -Identity $recoveryIdentity -PlannedMissingSubKeys $recoveryPlanned `
        -Snapshots @($recoverySnapshot) -ProcessIdentities @($blockedProcessIdentity) |
        Out-Null
    Update-Stage5RegistryRecoveryJournal -Path $recoveryJournalPath `
        -ExpectedIdentity $recoveryIdentity -State 'child-exit-unproven' `
        -Snapshots @($recoverySnapshot) -ChildExitProof $false `
        -NoActiveTitleProcesses $false -ProcessIdentities @($blockedProcessIdentity) |
        Out-Null
    $recoveryJournal = Read-TestJson $recoveryJournalPath
    Assert-True ($recoveryJournal.state -ceq 'child-exit-unproven' -and
        [int]$recoveryJournal.processIdentities[0].processId -eq 4242 -and
        -not [bool]$recoveryJournal.childExitProof -and
        -not [bool]$recoveryJournal.noActiveTitleProcesses -and
        @($recoveryJournal.snapshots).Count -eq 1 -and
        $recoveryJournal.snapshots[0].subKey -ceq $recoveryInstallKey -and
        @($recoveryJournal.snapshots[0].createdSubKeys)[0] -ceq 'Software') `
        'Blocked cleanup must retain an exact recoverable registry snapshot journal.'
    Test-Stage5RegistryJournalExistingFileUpdate $testRoot
    $validDocument = Read-TestJson $validManifest
    $literalReceipt = Read-TestJson $validDocument.runs[0].receiptPath
    Assert-True ($literalReceipt.cohortCreatedUtc -is [string] -and
        $literalReceipt.cohortCreatedUtc -ceq '2026-09-01T00:00:00.0000000Z' -and
        $literalReceipt.recordedUtc -is [string] -and
        $literalReceipt.recordedUtc -ceq '2026-09-01T00:00:01.0000000Z' -and
        [int]$literalReceipt.schemaVersion -eq 5 -and
        $literalReceipt.producer -ceq 'game-executable-stage5-performance-report-v5' -and
        $literalReceipt.simulationMode -ceq 'parallel' -and
        [bool]$literalReceipt.schedulerStarted -and
        $literalReceipt.fixture.kind -ceq 'replay' -and
        $literalReceipt.fixture.workloadQualification -ceq 'minimum-qualified' -and
        [bool]$literalReceipt.fixture.identityObserved -and
        $literalReceipt.fixture.contentPath -ceq $literalReceipt.fixture.replayPath -and
        [string]::IsNullOrEmpty([string]$literalReceipt.fixture.retainedReplayPath) -and
        [string]::IsNullOrEmpty([string]$literalReceipt.fixture.retainedReplaySha256) -and
        $literalReceipt.fixture.PSObject.Properties.Name -contains 'requestedPlayerCount' -and
        $literalReceipt.workload.sampling -ceq 'completed-simulation-frame-boundary-v1' -and
        [bool]$literalReceipt.rawEvidence.timingComplete -and
        $literalReceipt.measurementRole -ceq 'throughput' -and
        $literalReceipt.kernelTiming.mode -ceq 'owner-pipeline-observation' -and
        $literalReceipt.kernelTiming.serialReferenceKnown -eq $false -and
        $literalReceipt.kernelReference.mode -ceq 'throughput-binding' -and
        [int]$literalReceipt.kernelReference.streams[0].fieldSchema -eq 1 -and
        $literalReceipt.provenance.processCreationUtc -is [string] -and
        $literalReceipt.provenance.processCreationUtc -cmatch
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$') `
        'V5 receipts and PowerShell JSON readers must preserve literal UTC timestamp text.'
    Update-Receipt $validDocument.runs[0] {
        param($receipt)
        $receipt.recordedUtc = '2026-09-01T00:00:01.0000000Z'
    }
    $roundTripReceipt = Read-TestJson $validDocument.runs[0].receiptPath
    Assert-True ($roundTripReceipt.recordedUtc -is [string] -and
        $roundTripReceipt.recordedUtc -ceq '2026-09-01T00:00:01.0000000Z') `
        'Receipt mutation must retain the exact UTC timestamp representation.'
    $localManifest = New-ValidationFixture (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'local')) `
        'LocalCapacitySmoke'
    $localResult = & $runner -SelfTestValidationManifestPath $localManifest
    $localDocument = Read-TestJson $localManifest
    Assert-True (@($localDocument.runs).Count -eq 48) `
        'Local capacity smoke must schedule four fixtures across 1/2/4 workers.'
    Assert-True ((@($localDocument.runs | Where-Object {
        @('forced-one', 'physical-2', 'physical-4') -ccontains $_.lane
    }).Count) -eq 48) 'Local capacity smoke contains an unexpected lane.'
    Assert-True ($localResult -match '48 runs') `
        'Local capacity smoke self-test did not report its complete schedule.'
    $pairedManifest = New-PairedValidationFixture (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'paired'))
    & $runner -SelfTestValidationManifestPath $pairedManifest | Out-Null
    $pairedDocument = Read-TestJson $pairedManifest
    . $runner -SelfTestValidationManifestPath $pairedManifest | Out-Null
    $ordinalMaskDocument = Read-TestJson $localManifest
    $ordinalMaskRun = @($ordinalMaskDocument.runs | Where-Object {
        $_.lane -ceq 'physical-4'
    })[0]
    $ordinalMaskReceipt = ConvertFrom-Stage5JsonDictionary $ordinalMaskRun.receiptPath
    # Topology core masks use machine core ordinals; per-kernel masks use the
    # selected worker array's zero-based ordinals. They need equal capacity,
    # not numerically overlapping bits.
    $ordinalMaskReceipt.worker.selectedWorkerPhysicalCoreMask = [UInt64]0x55
    $ordinalMaskReceipt.schedulerMetrics.selectedWorkerPhysicalCoreMask = [UInt64]0x55
    $ordinalMaskAccepted = $true
    try { Assert-Stage5RealMulticoreKernelEvidence $ordinalMaskReceipt 'sparse topology ordinal fixture' }
    catch { $ordinalMaskAccepted = $false }
    Assert-True $ordinalMaskAccepted `
        'per-kernel worker-index masks remain valid when topology physical-core ordinals are sparse'
    $evidenceModuleForMask = Get-Module -Name DeterministicSimulationEvidence
    $diagnosticOrdinalMaskAccepted = $true
    try {
        & $evidenceModuleForMask {
            param($receipt)
            Assert-Stage5DiagnosticRealMulticoreKernelEvidence $receipt `
                'sparse topology ordinal fixture'
        } $ordinalMaskReceipt
    }
    catch { $diagnosticOrdinalMaskAccepted = $false }
    Assert-True $diagnosticOrdinalMaskAccepted `
        'final rereader uses worker ordinals rather than topology ordinals for kernel masks'
    $aggregateMaskReceipt = & $evidenceModuleForMask {
        param($json)
        ConvertFrom-Stage5JsonTextDictionary $json
    } ($ordinalMaskReceipt | ConvertTo-Json -Depth 30)
    foreach ($kernel in $aggregateMaskReceipt.kernels) {
        $kernel.physicalWorkerMask = 15
        $kernel.distinctPhysicalWorkers = 3
    }
    $aggregateMaskAccepted = $true
    $aggregateMaskError = ''
    try {
        Assert-Stage5RealMulticoreKernelEvidence $aggregateMaskReceipt `
            'aggregate worker-union fixture'
        & $evidenceModuleForMask {
            param($receipt)
            Assert-Stage5DiagnosticRealMulticoreKernelEvidence $receipt `
                'aggregate worker-union fixture'
        } $aggregateMaskReceipt
    }
    catch {
        $aggregateMaskAccepted = $false
        $aggregateMaskError = $_.Exception.Message
    }
    Assert-True $aggregateMaskAccepted `
        "performance receipts accept a worker union larger than the maximum per-batch distinct count: $aggregateMaskError"
    $missingAggregateWorker = & $evidenceModuleForMask {
        param($json)
        ConvertFrom-Stage5JsonTextDictionary $json
    } ($aggregateMaskReceipt | ConvertTo-Json -Depth 30)
    $missingAggregateWorker.kernels[0].physicalWorkerMask = 3
    $missingAggregateWorkerRejected = $false
    try {
        & $evidenceModuleForMask {
            param($receipt)
            Assert-Stage5DiagnosticRealMulticoreKernelEvidence $receipt `
                'missing aggregate worker fixture'
        } $missingAggregateWorker
    }
    catch { $missingAggregateWorkerRejected = $true }
    Assert-True $missingAggregateWorkerRejected `
        'performance receipt rereader rejects a union mask missing a per-batch physical worker'
    $pairedResult = Assert-Stage5PerformanceRunSet $pairedDocument
    Assert-True ($pairedResult.referencePolicy -ceq 'paired-serial-oracle-v1' -and
        @($pairedResult.runs).Count -eq 48 -and
        @($pairedResult.pairedOracleBindings).Count -eq 48 -and
        @($pairedResult.pairedOracleBindings | Where-Object {
            $null -eq $_.oracleRun -or $_.oracleRun.runId -ceq $_.throughputRunId
        }).Count -eq 0) `
        'Paired serial-oracle validation must retain only throughput runs in medians and bind one distinct oracle per run.'

    Assert-PairedRejected 'paired-missing-binding' {
        param($document)
        $document.pairedOracleBindings = @($document.pairedOracleBindings | Select-Object -Skip 1)
    }
    Assert-PairedRejected 'paired-duplicate-binding' {
        param($document)
        $document.pairedOracleBindings += $document.pairedOracleBindings[0]
    }
    Assert-PairedRejected 'paired-unknown-throughput-run' {
        param($document)
        $document.pairedOracleBindings[0].throughputRunId = 'missing-throughput-run'
    }
    Assert-PairedRejected 'paired-worker-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.worker.policy = 'fixed'
        }
    }
    Assert-PairedRejected 'paired-cpu-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.topology.selectedWorkerCpuSetIds[0] = 2
        }
    }
    Assert-PairedRejected 'paired-digest-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.kernelReference.streams[0].inputSha256 = ('44' * 32)
        }
    }
    Assert-PairedRejected 'paired-command-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        $binding.oracleRun.host.commandLine = '{0}  {1}' -f
            $binding.oracleRun.host.executablePath,
            $binding.oracleRun.expectedArgumentString
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.commandLine = '{0}  {1}' -f
                $receipt.executablePath,
                ($receipt.commandLine -replace '^[^ ]+ ', '')
        }
    }
    Assert-PairedRejected 'paired-oracle-role-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.measurementRole = 'throughput'
        }
    }
    Assert-PairedRejected 'paired-simulation-mode-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.simulationMode = 'serial'
        }
    }
    Assert-PairedRejected 'paired-scheduler-start-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.schedulerStarted = $false
        }
    }
    Assert-PairedRejected 'paired-fixture-kind-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.fixture.kind = 'fresh-ai-map'
        }
    }
    Assert-PairedRejected 'paired-fixture-qualification-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.fixture.workloadQualification = 'observed-only'
        }
    }
    Assert-PairedRejected 'paired-fixture-identity-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.fixture.identityObserved = $false
        }
    }
    Assert-PairedRejected 'paired-fixture-content-path-mismatch' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.fixture.contentPath = Join-Path $document.taskRoot 'other.rep'
        }
    }
    Assert-PairedRejected 'paired-retained-replay-metadata' {
        param($document)
        $binding = $document.pairedOracleBindings[0]
        Update-Receipt $binding.oracleRun {
            param($receipt)
            $receipt.fixture.retainedReplayPath = 'retained.rep'
            $receipt.fixture.retainedReplaySha256 = ('A' * 64)
        }
    }

    Assert-Rejected 'missing-lane' {
        param($document)
        $document.runs = @($document.runs | Where-Object { $_.lane -cne 'physical-16' })
    }
    Assert-Rejected 'pid-mismatch' {
        param($document)
        $document.runs[0].host.processId = [int]$document.runs[0].host.processId + 1
    }
    Assert-Rejected 'hash-mismatch' {
        param($document)
        $document.runs[0].host.executableSha256 = ('F' * 64)
    }
    Assert-Rejected 'command-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] { param($receipt) $receipt.commandLine += ' -forged' }
    }
    Assert-Rejected 'missing-phase-timing' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.phases[0].available = $false
            $receipt.phases[0].totalNanoseconds = 0
            $receipt.phases[0].maximumNanoseconds = 0
            $receipt.phases[0].sampleCount = 0
        }
    }
    Assert-Rejected 'unknown-external-serial' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.phases[0].serialNanoseconds = 0
            $receipt.phases[0].serialNanosecondsKnown = $false
        }
    }
    Assert-Rejected 'workload-frame-gap' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.workload.sampleCount = 99
        }
    }
    Assert-Rejected 'incomplete-timing-capture' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.rawEvidence.timingComplete = $false
        }
    }
    Assert-Rejected 'fractional-native-process-id' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.process.id = [double]$document.runs[0].host.processId + 0.4
        }
    }
    Assert-Rejected 'string-native-worker-pinned' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.worker.pinned = 'false'
        }
    }
    Assert-Rejected 'fractional-host-run-ordinal' {
        param($document)
        $document.runs[0].ordinal = 0.4
    }
    Assert-Rejected 'string-host-elapsed-time' {
        param($document)
        $document.runs[0].host.elapsedMilliseconds = '10.0'
    }
    Assert-Rejected 'string-host-topology-count' {
        param($document)
        $document.topology.physicalCoreCount = '16'
    }
    Assert-Rejected 'string-reviewed-player-count' {
        param($document)
        $document.fixtures[0].playerCount = '8'
    }
    Assert-Rejected 'external-affinity-failure' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.schedulerMetrics.affinityFailureCount = 1
        }
    }
    Assert-Rejected 'physical-lane-zero-worker-busy-time' {
        param($document)
        $run = @($document.runs | Where-Object { $_.lane -ceq 'physical-8' })[0]
        Update-Receipt $run { param($receipt)
            $receipt.schedulerMetrics.workerBusyNanoseconds = 0
        }
    }
    Assert-Rejected 'physical-lane-single-active-worker' {
        param($document)
        $run = @($document.runs | Where-Object { $_.lane -ceq 'physical-8' })[0]
        Update-Receipt $run { param($receipt)
            $receipt.schedulerMetrics.maximumActiveWorkers = 1
        }
    }
    Assert-Rejected 'physical-lane-uncommitted-kernel-stream' {
        param($document)
        $run = @($document.runs | Where-Object { $_.lane -ceq 'physical-8' })[0]
        Update-Receipt $run { param($receipt)
            $receipt.kernelTiming.streams[0].committedBatches = 0
            $receipt.kernelTiming.streams[0].admittedBatches = 0
            $receipt.kernelTiming.streams[0].abortedBatches = 0
        }
    }
    Assert-Rejected 'physical-lane-zero-kernel-worker-jobs' {
        param($document)
        $run = @($document.runs | Where-Object { $_.lane -ceq 'physical-8' })[0]
        Update-Receipt $run { param($receipt)
            $receipt.kernels[0].physicalWorkerJobs = 0
            $receipt.kernels[0].ownerHelpedJobs = $receipt.kernels[0].completedJobs
        }
    }
    Assert-Rejected 'physical-lane-kernel-worker-mask-mismatch' {
        param($document)
        $run = @($document.runs | Where-Object { $_.lane -ceq 'physical-8' })[0]
        Update-Receipt $run { param($receipt)
            $receipt.kernels[0].physicalWorkerMask = 1
        }
    }
    Assert-Rejected 'kernel-timing-stage-sum' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelTiming.streams[0].stages[0].totalNanoseconds = 11
        }
    }
    Assert-Rejected 'kernel-timing-duplicate-stream' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelTiming.streams += $receipt.kernelTiming.streams[0]
        }
    }
    Assert-Rejected 'kernel-timing-empty-external' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelTiming.streams = @()
            $receipt.kernelTiming.complete = $false
        }
    }
    Assert-Rejected 'v4-receipt-in-v5-run' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.schemaVersion = 4
            $receipt.producer = 'game-executable-stage5-performance-report-v4'
            $receipt.producerVersion = '4'
            $receipt.kernelTiming.mode = 'throughput-no-serial-oracle'
            $receipt.PSObject.Properties.Remove('measurementRole')
            $receipt.PSObject.Properties.Remove('kernelReference')
        }
    }
    Assert-Rejected 'throughput-with-serial-oracle-reference' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.mode = 'serial-oracle'
        }
    }
    Assert-Rejected 'serial-oracle-role-with-throughput-reference' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.measurementRole = 'serial-oracle'
        }
    }
    Assert-Rejected 'serial-oracle-not-aggregated' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.measurementRole = 'serial-oracle'
            $receipt.kernelReference.mode = 'serial-oracle'
            $receipt.kernelReference.streams[0].serialSampleCount = 1
            $receipt.kernelReference.streams[0].serialNanoseconds = 100
            $receipt.kernelReference.streams[0].maximumSerialNanoseconds = 100
        }
    }
    Assert-Rejected 'missing-native-simulation-mode' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.PSObject.Properties.Remove('simulationMode')
        }
    }
    Assert-Rejected 'missing-native-scheduler-started' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.PSObject.Properties.Remove('schedulerStarted')
        }
    }
    Assert-Rejected 'native-serial-simulation-mode' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.simulationMode = 'serial'
        }
    }
    Assert-Rejected 'native-scheduler-not-started' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.schedulerStarted = $false
        }
    }
    Assert-Rejected 'missing-native-fixture-kind' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.PSObject.Properties.Remove('kind')
        }
    }
    Assert-Rejected 'native-observed-only-workload' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.workloadQualification = 'observed-only'
        }
    }
    Assert-Rejected 'missing-native-fixture-identity' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.PSObject.Properties.Remove('identityObserved')
        }
    }
    Assert-Rejected 'native-fixture-identity-unobserved' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.identityObserved = $false
        }
    }
    Assert-Rejected 'missing-native-fixture-content-path' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.PSObject.Properties.Remove('contentPath')
        }
    }
    Assert-Rejected 'native-fixture-content-path-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.contentPath = Join-Path $document.taskRoot 'other.rep'
        }
    }
    Assert-Rejected 'native-fixture-retained-replay' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.fixture.retainedReplayPath = 'retained.rep'
            $receipt.fixture.retainedReplaySha256 = ('A' * 64)
        }
    }
    Assert-Rejected 'kernel-reference-arithmetic' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].abortedBatchCount = 1
        }
    }
    Assert-Rejected 'kernel-reference-timing-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].committedBatchCount = 0
        }
    }
    Assert-Rejected 'kernel-reference-serial-evidence' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].serialSampleCount = 1
        }
    }
    Assert-Rejected 'kernel-reference-missing-timing' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].name = 'status'
        }
    }
    Assert-Rejected 'kernel-reference-zero-field-schema' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].fieldSchema = 0
        }
    }
    Assert-Rejected 'kernel-reference-noncanonical-digest' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].inputSha256 = ('a' * 64)
        }
    }
    Assert-Rejected 'kernel-reference-zero-commit-orphan-operations' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].committedBatchCount = 0
            $receipt.kernelReference.streams[0].committedOperationCount = 1
            $receipt.kernelReference.streams[0].abortedBatchCount = 1
        }
    }
    Assert-Rejected 'kernel-reference-no-abort-orphan-operations' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernelReference.streams[0].committedBatchCount = 1
            $receipt.kernelReference.streams[0].abortedBatchCount = 0
            $receipt.kernelReference.streams[0].validatedOperationCount = 2
            $receipt.kernelReference.streams[0].committedOperationCount = 1
        }
    }
    Assert-Rejected 'legacy-pathfinding-kernel-name' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernels[5].name = 'pathfinding'
        }
    }
    Assert-Rejected 'exit-mismatch' {
        param($document)
        $document.runs[0].host.exitCode = 1
    }
    Assert-Rejected 'tampered-raw' {
        param($document)
        $receipt = Read-TestJson $document.runs[0].receiptPath
        [IO.File]::AppendAllText($receipt.rawEvidence.rawLogPath, 'tampered')
    }
    Assert-Rejected 'raw-final-crc-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $rawPath = [string]$receipt.rawEvidence.rawLogPath
            $raw = [IO.File]::ReadAllText($rawPath).Replace(
                'final_crc=12345678', 'final_crc=87654321')
            [IO.File]::WriteAllText($rawPath, $raw)
            $rawHash = Get-Sha256 $rawPath
            $receipt.rawEvidence.rawLogSha256 = $rawHash
            $receipt.rawLogs[0].sha256 = $rawHash
            $document.runs[0].host.rawLogSha256 = $rawHash
        }
    }
    Assert-Rejected 'tampered-timing' {
        param($document)
        $receipt = Read-TestJson $document.runs[0].receiptPath
        [IO.File]::AppendAllText($receipt.rawEvidence.timingPath, 'tampered')
    }
    Assert-Rejected 'reused-run-id' {
        param($document)
        $document.runs[1].runId = $document.runs[0].runId
        Update-Receipt $document.runs[1] {
            param($receipt)
            $receipt.runId = $document.runs[0].runId
        }
    }
    Assert-Rejected 'reused-receipt' {
        param($document)
        $document.runs[1].receiptPath = $document.runs[0].receiptPath
        $document.runs[1].receiptSha256 = $document.runs[0].receiptSha256
    }
    Assert-Rejected 'insufficient-topology' {
        param($document)
        $document.topology.physicalCoreCount = 15
        $document.topology.logicalProcessorCount = 15
    }
    Assert-Rejected 'independent-topology-mismatch' {
        param($document)
        $document.topology.cpuSets[0].coreIndex = 99
    }
    Assert-Rejected 'reused-run-nonce' {
        param($document)
        $document.runs[1].runNonce = $document.runs[0].runNonce
        Update-Receipt $document.runs[1] {
            param($receipt)
            $receipt.runNonce = $document.runs[0].runNonce
        }
    }
    Assert-Rejected 'cohort-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.cohortNonce = '00000000-0000-4000-8000-000000000099'
        }
    }
    Assert-Rejected 'runtime-closure-mismatch' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.runtimeClosure.closureSha256 = ('F' * 64)
        }
    }
    Assert-Rejected 'detached-raw-log-entry' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.rawLogs[0].sha256 = ('F' * 64)
        }
    }
    Assert-Rejected 'detached-provenance' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.provenance.receiptPath = Join-Path $document.taskRoot 'detached.json'
        }
    }
    Assert-Rejected 'tampered-artifact-closure' {
        param($document)
        $artifact = Read-TestJson $document.artifactSetManifestPath
        $artifact.runtimeClosure.closureSha256 = ('F' * 64)
        Write-Json $document.artifactSetManifestPath $artifact
        $document.artifactSetSha256 = Get-Sha256 $document.artifactSetManifestPath
        foreach ($run in @($document.runs)) {
            Update-Receipt $run {
                param($receipt)
                $receipt.artifactSetSha256 = $document.artifactSetSha256
            }
        }
    }
    Assert-Rejected 'tampered-launcher-role' {
        param($document)
        $artifact = Read-TestJson $document.artifactSetManifestPath
        $entry = @($artifact.artifacts | Where-Object {
            $_.role -ceq 'zerohour-launcher'
        })[0]
        $entry.path = 'artifacts/ZeroHour/runtime.dll'
        $entry.sha256 = Get-Sha256 (Join-Path (Split-Path -Parent $document.artifactSetManifestPath) $entry.path)
        Write-Json $document.artifactSetManifestPath $artifact
        $document.artifactSetSha256 = Get-Sha256 $document.artifactSetManifestPath
        foreach ($run in @($document.runs)) {
            Update-Receipt $run {
                param($receipt)
                $receipt.artifactSetSha256 = $document.artifactSetSha256
            }
        }
    }
    Assert-Rejected 'tampered-launcher-config-role' {
        param($document)
        $artifact = Read-TestJson $document.artifactSetManifestPath
        $entry = @($artifact.artifacts | Where-Object {
            $_.role -ceq 'zerohour-launcher-config'
        })[0]
        $entry.path = 'artifacts/ZeroHour/runtime.dll'
        $entry.sha256 = Get-Sha256 (Join-Path (Split-Path -Parent $document.artifactSetManifestPath) $entry.path)
        Write-Json $document.artifactSetManifestPath $artifact
        $document.artifactSetSha256 = Get-Sha256 $document.artifactSetManifestPath
        foreach ($run in @($document.runs)) {
            Update-Receipt $run {
                param($receipt)
                $receipt.artifactSetSha256 = $document.artifactSetSha256
            }
        }
    }

    $afterProductProcesses = @(Get-Process generalsv, generalszh -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Id })
    Assert-True ((@($beforeProductProcesses) -join ',') -ceq
        (@($afterProductProcesses) -join ',')) `
        'Host self-tests launched or replaced a product process.'
    Assert-True ($runnerSource -match 'if \(\$PSCmdlet\.ParameterSetName -ceq ''SelfTest''\)[\s\S]*?return[\s\S]*?Invoke-Stage5PerformanceRunPlan') `
        'Self-test parameter set must return before installed run-plan invocation.'
    Assert-True ($runnerSource -match 'Read-Stage5PerformanceArtifactSet' -and
        $runnerSource -match 'DateKind' -and
        $runnerSource -match 'Get-Stage5RuntimeClosureBinding' -and
        $runnerSource -match 'ArtifactSetManifestPath' -and
        $runnerSource -match 'RTS_PERFORMANCE_ROLE' -and
        $runnerSource -match 'RTS_PERFORMANCE_RUN_ID' -and
        $runnerSource -match 'RTS_PERFORMANCE_RUN_NONCE' -and
        $runnerSource -match 'RTS_PERFORMANCE_COHORT_NONCE' -and
        $runnerSource -match 'RTS_PERFORMANCE_COHORT_CREATED_UTC' -and
        $runnerSource -match 'RTS_PERFORMANCE_RECEIPT_DIR' -and
        $runnerSource -match 'RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256' -and
        $runnerSource -match 'RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256' -and
        $runnerSource -match 'RTS_PERFORMANCE_VERIFIER_BOUNDARY' -and
        $runnerSource -match 'RTS_PERFORMANCE_REFERENCE_MODE') `
        'Runner must set the executable performance receipt contract.'
    Assert-True ($runnerSource -match 'ReferencePolicy' -and
        $runnerSource -match 'throughputRunId=\$entry\.sourceEntryId; oracleRun=\$run' -and
        $runnerSource -match "'throughput-binding'" -and
        $runnerSource -match "'serial-oracle'" -and
        $runnerSource -match 'schemaVersion = 2' -and
        $runnerSource -match 'pairedOracleBindings') `
        'Runner must execute and persist paired serial-oracle runs without mixing their elapsed samples.'
    Assert-True ($runnerSource -match 'GetSystemCpuSetInformation' -and
        $runnerSource -match 'physicalCoreCount -ge 16' -and
        $runnerSource -match 'LocalCapacitySmoke' -and
        $runnerSource -match 'Stop-Stage5ProcessSafely' -and
        $runnerSource -match 'Get-Stage5LauncherContract' -and
        $runnerSource -match 'AllowHeadlessDirectExecution' -and
        $runnerSource -match 'otherLines' -and
        $runnerSource -match 'Assert-Stage5PerformanceLauncherBinding' -and
        $runnerSource -match 'Open-Stage5PerformanceReadOnlyLocks' -and
        $runnerSource -match 'Assert-Stage5PerformanceFixtureHash' -and
        $runnerSource -match 'New-Stage5TitleSessionContract' -and
        $runnerSource -match 'Set-Stage5RegistryValue' -and
        $runnerSource -match 'Stage5RegistryRecovery\.psm1' -and
        $runnerSource -match 'Get-Stage5RegistryRecoverySnapshotPlanSha256' -and
        $runnerSource -match 'New-Stage5RegistryRecoverySnapshot' -and
        $runnerSource -match 'Update-Stage5RegistryRecoveryJournal' -and
        $runnerSource -match 'Invoke-Stage5RegistryRecovery' -and
        $runnerSource -match 'snapshotPlanSha256' -and
        $runnerSource -match 'ProcessIdentities' -and
        $runnerSource -match 'launchPending' -and
        $runnerSource -match 'Add-Stage5RegistryRecoveryPendingProcess' -and
        $runnerSource -match 'Set-Stage5RegistryRecoveryObservedProcess' -and
        $runnerSource -notmatch 'Restore-Stage5RegistrySnapshots' -and
        $runnerSource -notmatch 'Write-Stage5RegistryRecoveryJournal' -and
        $runnerSource -match 'Registry32' -and
        $runnerSource -match 'Registry64' -and
        $runnerSource -match 'RTS_STAGE5_VALIDATION_PROFILE_ROOT' -and
        $runnerSource -match 'Stage5ValidationProfileCapability\.psm1' -and
        $runnerSource -match 'Assert-Stage5ProcessLocalProfileCapability' -and
        $runnerSource -match "profileStrategy = 'process-local-validation-profile-root'" -and
        $runnerSource -notmatch ([regex]::Escape(
            "name = 'Personal'; value = `$documentsRoot")) -and
        $runnerSource -notmatch "'UserDataLeafName'" -and
        $runnerSource -match 'Assert-Stage5ProfileReadOnly' -and
        $runnerSource -match 'profileHashBefore' -and
        $runnerSource -match 'profileHashAfter' -and
         $runnerSource -match 'Stage5FatalPattern' -and
         $runnerSource -match 'stdoutTask') `
         'Runner must fail closed on insufficient physical topology.'
    $profileCapabilityOffset = $runnerSource.IndexOf(
        'Assert-Stage5ProcessLocalProfileCapability $executableFull',
        [StringComparison]::Ordinal)
    $firstTaskRootOffset = $runnerSource.IndexOf(
        '$taskFull = [IO.Path]::GetFullPath($TaskRoot)',
        [StringComparison]::Ordinal)
    Assert-True ($profileCapabilityOffset -ge 0 -and
        $firstTaskRootOffset -gt $profileCapabilityOffset) `
        'Process-local profile capability must be checked before task/output setup.'
    Assert-True ([regex]::Matches($runnerSource,
            'Assert-Stage5ProcessLocalProfileCapability\s+\$executableFull').Count -ge 2) `
        'Both installed-run and native-fixture executable paths must require the process-local profile capability.'
    $taskRootCreateOffset = $runnerSource.IndexOf(
        'New-Item -ItemType Directory -Path $taskFull', [StringComparison]::Ordinal)
    $titleContractOffset = $runnerSource.IndexOf(
        '$titleSession = New-Stage5TitleSessionContract', $taskRootCreateOffset,
        [StringComparison]::Ordinal)
    $taskTryOffset = $runnerSource.IndexOf('try {', $taskRootCreateOffset,
        [StringComparison]::Ordinal)
    Assert-True ($taskRootCreateOffset -ge 0 -and $titleContractOffset -gt $taskTryOffset) `
        'Title-session construction must be inside the task-root cleanup scope.'
    Assert-True ($runnerSource -match '\$titleSession = \$null' -and
        $runnerSource -match '(?:if|elseif) \([^\r\n]*\$null -eq \$titleSession\)[\s\S]*?Remove-Item -LiteralPath \$taskFull') `
        'A title-session setup failure must remove its freshly created task root.'
    Assert-True ($runnerSource -match 'Stage5ValidationMutexName' -and
        $runnerSource -match 'Get-Stage5RegistryRecoveryMutexName' -and
        $runnerSource -match 'WindowsIdentity' -and
        $runnerSource -match 'Acquire-Stage5ValidationMutex' -and
        $runnerSource -match 'Release-Stage5ValidationMutex' -and
        $runnerSource -match 'Assert-Stage5NoInstalledTitleProcesses') `
        'Installed validation must serialize registry swaps and reject live title processes.'
    $mainMutexOffset = $runnerSource.IndexOf(
        '$validationMutex = Acquire-Stage5ValidationMutex', [StringComparison]::Ordinal)
    $mainPreflightOffset = $runnerSource.IndexOf(
        'Assert-Stage5NoInstalledTitleProcesses', $mainMutexOffset,
        [StringComparison]::Ordinal)
    $mainRegistrySwapOffset = $runnerSource.IndexOf(
        'Add-Stage5RegistryRecoveryMutation $registryRecovery $view', $mainPreflightOffset,
        [StringComparison]::Ordinal)
    Assert-True ($mainMutexOffset -ge 0 -and $mainPreflightOffset -gt $mainMutexOffset -and
        $mainRegistrySwapOffset -gt $mainPreflightOffset) `
        'Mutex acquisition and live-title preflight must precede every registry swap.'
    $pendingProcessOffset = $runnerSource.IndexOf(
        'Add-Stage5RegistryRecoveryPendingProcess `',
        [StringComparison]::Ordinal)
    $processStartOffset = $runnerSource.IndexOf(
        '$processStarted = $process.Start()', $pendingProcessOffset,
        [StringComparison]::Ordinal)
    Assert-True ($pendingProcessOffset -gt 0 -and
        $processStartOffset -gt $pendingProcessOffset) `
        'Every installed child launch must publish a pending process identity before Process.Start().'
    $registrySetupOffset = $runnerSource.IndexOf(
        'function Invoke-Stage5RegistryTargetSetup', [StringComparison]::Ordinal)
    Assert-True ($registrySetupOffset -ge 0 -and
        $runnerSource.IndexOf('CreatedSubKeys.Add', $registrySetupOffset,
            [StringComparison]::Ordinal) -lt
        $runnerSource.IndexOf('created.Dispose', $registrySetupOffset,
            [StringComparison]::Ordinal) -and
        $runnerSource.IndexOf('Rollback', $registrySetupOffset,
            [StringComparison]::Ordinal) -ge 0) `
        'Registry setup must journal created segments before disposal and rollback on failure.'
    Assert-True ($runnerSource -match '\$primaryError = \$null' -and
        $runnerSource -match 'catch \{\s*\$primaryError = \$_\s*\}' -and
        $runnerSource -match 'operation failed:.*cleanup also failed') `
        'Outer cleanup must preserve the primary setup/run error with any cleanup error.'
    Assert-True ($runnerSource -match '\$processStarted = \$false' -and
        $runnerSource -match 'Invoke-Stage5OwnedProcessCleanup' -and
        $runnerSource -match 'if \(-not \$ProcessStarted\)' -and
        $runnerSource -match 'started Stage 5 title process did not exit') `
        'Every successfully started title process must be stopped before evidence cleanup.'
    Assert-True ($runnerSource -match 'Invoke-Stage5OwnedProcessCleanup' -and
        $runnerSource -match 'childCleanupBlocked' -and
        $runnerSource -match 'registry/profile cleanup is deferred' -and
        $runnerSource -match 'Update-Stage5RegistryRecoveryState' -and
        $runnerSource -match "'child-exit-unproven'" -and
        $runnerSource -match 'Invoke-Stage5RegistryRecovery' -and
        $runnerSource -match 'game-executable-performance-receipt-v5' -and
        $runnerSource -match 'legacy-mutable-island' -and
        $runnerSource -match 'timingComplete' -and
        $runnerSource -match 'serialNanosecondsKnown' -and
        $runnerSource -match 'kernelTiming' -and
        $runnerSource -match 'owner-pipeline-observation' -and
        $runnerSource -match 'owner-stack-exclusive-v1' -and
        $runnerSource -match 'kernelReference' -and
        $runnerSource -match 'throughput-binding' -and
        $runnerSource -match 'serial-oracle') `
        'Unproven child exit must propagate to the outer cleanup policy and recovery journal.'
    Assert-True ($runnerSource -match 'if \(-not \$childCleanupBlocked -and \$registryRestored -and \$null -ne \$titleSession\)' -and
        $runnerSource -match 'if \(-not \$childCleanupBlocked -and \$registryRestored\)' -and
        $runnerSource -match 'mutex ownership ends with this validator' -and
        $runnerSource -match 'recovery journal to restore state before retrying' -and
        $runnerSource -notmatch 'mutex was retained because') `
        'Profile deletion and mutex release must remain blocked until child exit and registry restoration are proven.'
    $traceSnapshotOffset = $runnerSource.IndexOf(
        '$traceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot',
        [StringComparison]::Ordinal)
    $traceHashOnlyOffset = $runnerSource.IndexOf('-HashOnly', $traceSnapshotOffset,
        [StringComparison]::Ordinal)
    $traceHashAssertionOffset = $runnerSource.IndexOf(
        'Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256', $traceSnapshotOffset,
        [StringComparison]::Ordinal)
    Assert-True ($traceSnapshotOffset -ge 0 -and
        $traceHashOnlyOffset -gt $traceSnapshotOffset -and
        $traceHashAssertionOffset -gt $traceHashOnlyOffset) `
        'Binary phase traces must use the bounded hash-only snapshot and exact digest/length assertion.'
    $phaseFailures = New-Object 'Collections.Generic.List[string]'
    try { Test-Stage5HeldInputCapabilityRequired (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-held-input-capability')) }
    catch { $phaseFailures.Add("held input capability: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5HostFixturePathBudget }
    catch { $phaseFailures.Add("host fixture path budget: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5UnsafePathDiagnostic }
    catch { $phaseFailures.Add("unsafe path diagnostic: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PrelaunchPlanRejections (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-plan-rejections')) }
    catch { $phaseFailures.Add("prelaunch plan rejections: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5NativeRoleEnvironmentBinding (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-role-environment')) }
    catch { $phaseFailures.Add("prelaunch role environment: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5NativeArtifactSelectionBoundary (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-native-files')) }
    catch { $phaseFailures.Add("prelaunch native artifact selection: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5ReceiptSingleSnapshotBoundary (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-receipt-snapshot')) }
    catch { $phaseFailures.Add("prelaunch receipt snapshot: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5RawEvidenceSingleSnapshotBoundary (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-raw-snapshot')) }
    catch { $phaseFailures.Add("prelaunch raw snapshot: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5AttemptPublicationFaults (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-publication-faults')) }
    catch { $phaseFailures.Add("prelaunch publication faults: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5JournalReaderMutationClosure (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-journal-reader')) }
    catch { $phaseFailures.Add("prelaunch journal reader: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5CleanupFailureWithholdsAggregate (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-cleanup-publication')) }
    catch { $phaseFailures.Add("prelaunch cleanup publication: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5CreateNewEvidencePublication (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-create-new')) }
    catch { $phaseFailures.Add("create-new evidence: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PrelaunchIdentityPlan (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-identity-plan')) }
    catch { $phaseFailures.Add("prelaunch identity plan: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PrelaunchStartInfoBinding (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-start-info')) }
    catch { $phaseFailures.Add("prelaunch start-info binding: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5AttemptFailureClosure (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'prelaunch-attempt-failure')) }
    catch { $phaseFailures.Add("prelaunch attempt failure closure: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PhaseAccountingWireContract }
    catch { $phaseFailures.Add("phase accounting wire: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5V6FooterAndRepeatedControlContract }
    catch { $phaseFailures.Add("V6 footer/repeated control: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PhaseSelectedRunSetContract (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'phase-selected-cohort')) }
    catch { $phaseFailures.Add("phase selected cohort: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5PhaseHostBoundaryContracts (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'phase-host-boundaries')) }
    catch { $phaseFailures.Add("phase host boundaries: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5BoundedTraceSnapshotContract (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'phase-bounded-trace')) }
    catch { $phaseFailures.Add("bounded trace snapshot: $($_.Exception.Message)") | Out-Null }
    try { Test-Stage5SnapshotIdentityAndSparseBounds (Join-Path $testRoot (Get-Stage5HostFixtureDirectoryName 'snapshot-identity-bounds')) }
    catch { $phaseFailures.Add("snapshot identity/bounds: $($_.Exception.Message)") | Out-Null }
    Assert-True ($phaseFailures.Count -eq 0) ($phaseFailures.ToArray() -join ' | ')
    Write-Output 'Stage 5 performance scaling host validation self-tests passed.'
}
finally {
    foreach ($locks in @($script:Stage5TestInputLocks.Values)) {
        try { Dispose-Stage5PerformanceReadOnlyLocks $locks }
        catch { $script:Stage5SelfTestCleanupBlocked = $true }
    }
    $script:Stage5TestInputLocks = @{}
    if (Test-Path -LiteralPath $testRoot) {
        if ($script:Stage5SelfTestCleanupBlocked) {
            Write-Warning "Stage 5 self-test root retained because a junction could not be safely removed: $testRoot"
        }
        else {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
    }
}
