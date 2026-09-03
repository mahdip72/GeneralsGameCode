param([string]$ScratchRoot = '')

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
    $recordedUtc = '2026-09-01T00:00:01.0000000Z'
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
    if ($SerialKnown) {
        $kernelStreams += [ordered]@{
            name = 'physics'; subtype = 0
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
        $kernelReferenceStreams += [ordered]@{
            name = 'physics'; subtype = 0; fieldSchema = 1
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
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial',
        'path')
    $kernels = @()
    for ($kernelIndex = 0; $kernelIndex -lt $kernelNames.Count; ++$kernelIndex) {
        $kernels += [ordered]@{
            name = $kernelNames[$kernelIndex]; available = $true
            submittedJobs = 8; completedJobs = 8; physicalWorkerJobs = 8
            ownerHelpedJobs = 0; physicalWorkerMask = 255
            distinctPhysicalWorkers = 8; physicalWorkerMaskComplete = $true
            elapsedNanoseconds = 2000 + $kernelIndex
            elapsedNanosecondsKnown = $true
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
            exitCode = 0; elapsedMilliseconds = $Elapsed
            rawLogSha256 = Get-Sha256 $rawPath
            timingSha256 = Get-Sha256 $timingPath
        }
    }
}

function New-ValidationFixture {
    param([string]$Root, [string]$Mode = 'External16Core')
    New-Item -ItemType Directory -Path $Root | Out-Null
    $sourceCommit = ('a' * 40)
    $artifactBinding = New-ArtifactSetFixture $Root $sourceCommit
    $executable = $artifactBinding.executablePath
    $fixtureIds = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $unitCounts = @(1000, 4000, 8000, 12000)
    $fixtures = @()
    $stage3 = @()
    $runs = New-Object 'Collections.Generic.List[object]'
    $processId = 20000
    $cohortNonce = '00000000-0000-4000-8000-000000000001'
    $cohortCreatedUtc = '2026-09-01T00:00:00.0000000Z'
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
    param([string]$Root)
    $manifest = New-ValidationFixture $Root 'External16Core'
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

function Update-Receipt {
    param([object]$Run, [scriptblock]$Mutation)
    $receipt = Read-TestJson $Run.receiptPath
    & $Mutation $receipt
    Write-Json $Run.receiptPath $receipt
    $Run.receiptSha256 = Get-Sha256 $Run.receiptPath
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Mutation)
    $caseRoot = Join-Path $testRoot $Name
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
    $caseRoot = Join-Path $testRoot $Name
    $manifest = New-PairedValidationFixture $caseRoot
    $document = Read-TestJson $manifest
    & $Mutation $document
    Write-Json $manifest $document
    $rejected = $false
    try { & $runner -SelfTestValidationManifestPath $manifest | Out-Null }
    catch { $rejected = $true }
    Assert-True $rejected "Negative paired self-test '$Name' was not rejected."
}

function Test-Stage5RegistryJournalExistingFileUpdate {
    param([string]$Root)
    $journalPath = Join-Path $Root 'synthetic-recovery-journal-update.json'
    $snapshots = @(
        [pscustomobject]@{
            view = [Microsoft.Win32.RegistryView]::Registry32
            subKey = 'Software\Stage5\Validation'; name = 'Personal'
            hadKey = $true; hadValue = $true
            oldValue = '%STAGE5_TEST_DOCUMENTS%\Documents'
            oldKind = [Microsoft.Win32.RegistryValueKind]::ExpandString
            createdSubKeys = @()
        },
        [pscustomobject]@{
            view = [Microsoft.Win32.RegistryView]::Registry64
            subKey = 'Software\Stage5\Validation'; name = 'InstallPath'
            hadKey = $true; hadValue = $false
            oldValue = $null; oldKind = $null; createdSubKeys = @()
        }
    )
    Write-Stage5RegistryRecoveryJournal $journalPath 'ZeroHour' $Root $snapshots `
        ([pscustomobject]@{ processId = 4242; exitProof = $false; blocked = $true }) 'pending'
    $pendingJournal = Read-TestJson $journalPath
    Assert-True ($pendingJournal.state -ceq 'pending' -and
        @($pendingJournal.snapshots).Count -eq 2 -and
        $pendingJournal.snapshots[0].oldValue -ceq '%STAGE5_TEST_DOCUMENTS%\Documents' -and
        [int]$pendingJournal.snapshots[0].oldKind -eq 2 -and
        -not [bool]$pendingJournal.snapshots[1].hadValue) `
        'Initial recovery journal must preserve raw expansion, type, and absence.'
    $pendingHash = Get-Sha256 $journalPath
    try {
        # This second write must exercise replacement of an existing file;
        # a fresh-file-only test misses the PowerShell null-string binding bug.
        Write-Stage5RegistryRecoveryJournal $journalPath 'ZeroHour' $Root $snapshots `
            ([pscustomobject]@{ processId = 4242; exitProof = $true; blocked = $false }) 'restored'
    }
    catch {
        Assert-True ((Get-Sha256 $journalPath) -ceq $pendingHash) `
            'Failed journal replacement must leave the pending recovery data intact.'
        throw
    }
    $restoredJournal = Read-TestJson $journalPath
    Assert-True ($restoredJournal.state -ceq 'restored' -and
        [bool]$restoredJournal.childExitProof -and -not [bool]$restoredJournal.cleanupBlocked -and
        [int]$restoredJournal.processId -eq 4242 -and
        @($restoredJournal.snapshots).Count -eq 2 -and
        $restoredJournal.snapshots[0].oldValue -ceq '%STAGE5_TEST_DOCUMENTS%\Documents' -and
        [int]$restoredJournal.snapshots[0].oldKind -eq 2 -and
        -not [bool]$restoredJournal.snapshots[1].hadValue -and
        $null -eq $restoredJournal.snapshots[1].oldValue -and
        $null -eq $restoredJournal.snapshots[1].oldKind) `
        'Existing recovery journal must atomically publish restored state without changing originals.'
    Assert-True (@(Get-ChildItem -LiteralPath $Root -File `
            -Filter 'synthetic-recovery-journal-update.json.tmp-*').Count -eq 0) `
        'Successful journal replacement must not leave a temporary recovery file.'
}

$runner = Join-Path $PSScriptRoot 'Invoke-Stage5PerformanceScalingValidation.ps1'
$runnerSource = Get-Content -LiteralPath $runner -Raw
$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
    else { 'H:\Stage5PerformanceScalingValidationScratch' }
$testRoot = Join-Path $scratchParent ('stage5-performance-host-self-test-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))
$beforeProductProcesses = @(Get-Process generalsv, generalszh -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Id })

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    $validManifest = New-ValidationFixture (Join-Path $testRoot 'valid')
    & $runner -SelfTestValidationManifestPath $validManifest | Out-Null
    # Load the runner's bounded path helpers so this regression exercises the
    # real path checks rather than only matching their source text.
    . $runner -SelfTestValidationManifestPath $validManifest | Out-Null
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
    Write-Stage5RegistryRecoveryJournal $recoveryJournalPath 'ZeroHour' $testRoot `
        @([pscustomobject]@{
            view = [Microsoft.Win32.RegistryView]::Registry64
            subKey = 'Software\Stage5\Validation'
            name = 'InstallPath'; hadKey = $false; hadValue = $false
            oldValue = $null; oldKind = $null; createdSubKeys = @('Software')
        }) `
        ([pscustomobject]@{ processId = 4242; exitProof = $false; blocked = $true }) `
        'child-exit-unproven'
    $recoveryJournal = Read-TestJson $recoveryJournalPath
    Assert-True ($recoveryJournal.state -ceq 'child-exit-unproven' -and
        [int]$recoveryJournal.processId -eq 4242 -and
        [bool]$recoveryJournal.cleanupBlocked -and
        @($recoveryJournal.snapshots).Count -eq 1 -and
        $recoveryJournal.snapshots[0].subKey -ceq 'Software\Stage5\Validation' -and
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
    $localManifest = New-ValidationFixture (Join-Path $testRoot 'local') `
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

    $pairedManifest = New-PairedValidationFixture (Join-Path $testRoot 'paired')
    & $runner -SelfTestValidationManifestPath $pairedManifest | Out-Null
    $pairedDocument = Read-TestJson $pairedManifest
    . $runner -SelfTestValidationManifestPath $pairedManifest | Out-Null
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
    Assert-Rejected 'unknown-kernel-timing' {
        param($document)
        Update-Receipt $document.runs[0] {
            param($receipt)
            $receipt.kernels[0].elapsedNanosecondsKnown = $false
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
    Assert-True ($runnerSource -match 'if \(\$PSCmdlet\.ParameterSetName -ceq ''SelfTest''\)[\s\S]*?return[\s\S]*?Invoke-Stage5InstalledPerformanceRun') `
        'Self-test parameter set must return before installed process invocation.'
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
        $runnerSource -match 'scheduledOracleBindings' -and
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
        $runnerSource -match 'Restore-Stage5RegistrySnapshots' -and
        $runnerSource -match 'Registry32' -and
        $runnerSource -match 'Registry64' -and
        $runnerSource -match 'RTS_STAGE5_VALIDATION_PROFILE_ROOT' -and
        $runnerSource -match 'Assert-Stage5ProfileReadOnly' -and
        $runnerSource -match 'profileHashBefore' -and
        $runnerSource -match 'profileHashAfter' -and
        $runnerSource -match 'Stage5FatalPattern' -and
        $runnerSource -match 'stdoutTask') `
        'Runner must fail closed on insufficient physical topology.'
    $taskRootCreateOffset = $runnerSource.IndexOf(
        'New-Item -ItemType Directory -Path $taskFull', [StringComparison]::Ordinal)
    $titleContractOffset = $runnerSource.IndexOf(
        '$titleSession = New-Stage5TitleSessionContract', [StringComparison]::Ordinal)
    $taskTryOffset = $runnerSource.IndexOf('try {', $taskRootCreateOffset,
        [StringComparison]::Ordinal)
    Assert-True ($taskRootCreateOffset -ge 0 -and $titleContractOffset -gt $taskTryOffset) `
        'Title-session construction must be inside the task-root cleanup scope.'
    Assert-True ($runnerSource -match '\$titleSession = \$null' -and
        $runnerSource -match '(?:if|elseif) \([^\r\n]*\$null -eq \$titleSession\)[\s\S]*?Remove-Item -LiteralPath \$taskFull') `
        'A title-session setup failure must remove its freshly created task root.'
    Assert-True ($runnerSource -match 'Stage5ValidationMutexName' -and
        $runnerSource -match 'Global\\GeneralsGameCode\.Stage5PerformanceValidation' -and
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
        'Set-Stage5RegistryValue $view', $mainPreflightOffset,
        [StringComparison]::Ordinal)
    Assert-True ($mainMutexOffset -ge 0 -and $mainPreflightOffset -gt $mainMutexOffset -and
        $mainRegistrySwapOffset -gt $mainPreflightOffset) `
        'Mutex acquisition and live-title preflight must precede every registry swap.'
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
        $runnerSource -match 'Write-Stage5RegistryRecoveryJournal' -and
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
    Write-Output 'Stage 5 performance scaling host validation self-tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
