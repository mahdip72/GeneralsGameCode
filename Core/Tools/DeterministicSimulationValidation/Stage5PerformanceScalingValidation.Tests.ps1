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

function Write-Json {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 20))
}

function Write-RunReceipt {
    param([string]$Root, [string]$Executable, [string]$FixtureId,
        [string]$FixturePath, [string]$Lane, [int]$Workers, [int]$Ordinal,
        [int]$ProcessId, [double]$Elapsed, [int]$UnitCount)
    $runId = "test-$FixtureId-$Lane-$Ordinal-$ProcessId"
    $runRoot = Join-Path $Root $runId
    New-Item -ItemType Directory -Path $runRoot | Out-Null
    $rawPath = Join-Path $runRoot 'raw.log'
    $timingPath = Join-Path $runRoot 'timing.csv'
    $receiptPath = Join-Path $runRoot 'receipt.json'
    $argumentString = "-headless -noFPSLimit -pipelineMode serial -simulationMode parallel -workerPolicy auto -validationExecutableSha256 $('A' * 64) -workerCount $Workers -replay $FixturePath"
    $commandLine = "$Executable $argumentString"
    $creation = [Int64](133000000000000000 + $ProcessId)
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
    $receipt = [ordered]@{
        schemaVersion = 1
        producer = 'game-executable-performance-receipt-v1'
        evidenceKind = 'stage5-performance-receipt'
        status = 'complete'
        title = 'ZeroHour'
        runId = $runId
        sourceCommit = ('a' * 40)
        artifactSetSha256 = ('B' * 64)
        executablePath = $Executable
        executableSha256 = ('A' * 64)
        commandLine = $commandLine
        process = [ordered]@{
            id = $ProcessId; creationTimeUtc100ns = $creation
            startTimeUtc100ns = $creation + 1; endTimeUtc100ns = $creation + 2
            identityAvailable = $true; exitCodeKnown = $true; exitCode = 0
            exitBoundary = 'ReplaySimulation::simulateReplaysInThisProcess:return'
        }
        fixture = [ordered]@{
            id = $FixtureId; contentSha256 = ('C' * 64); replayPath = $FixturePath
            seed = 7; seedKnown = $true; playerCount = 8
            playerCountKnown = $true; unitCount = $UnitCount; unitCountKnown = $true
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
        }
        schedulerMetrics = [ordered]@{}
        phases = @()
        kernels = @()
    }
    $raw = @(
        'producer=game-executable-performance-receipt-v1', 'game_owned=1',
        "run_id=$runId", "process_id=$ProcessId", "executable_sha256=$('A' * 64)",
        "fixture_id=$FixtureId", "fixture_sha256=$('C' * 64)", 'frame=100',
        'final_crc=12345678', 'close_boundary=game-owned-raw-diagnostic-closed-v1'
    ) -join [Environment]::NewLine
    [IO.File]::WriteAllText($rawPath, $raw)
    [IO.File]::WriteAllText($timingPath, "frame,logic_ns`r`n1,100`r`n")
    $receipt.rawEvidence.rawLogSha256 = Get-Sha256 $rawPath
    $receipt.rawEvidence.timingSha256 = Get-Sha256 $timingPath
    Write-Json $receiptPath $receipt
    return [ordered]@{
        fixtureId = $FixtureId; lane = $Lane; ordinal = $Ordinal
        warmup = ($Ordinal -eq 0); runId = $runId
        expectedArgumentString = $argumentString
        receiptPath = $receiptPath; receiptSha256 = Get-Sha256 $receiptPath
        host = [ordered]@{
            processId = $ProcessId; creationTimeUtc100ns = $creation
            executablePath = $Executable; executableSha256 = ('A' * 64)
            commandLine = $commandLine; argumentString = $argumentString
            exitCode = 0; elapsedMilliseconds = $Elapsed
            rawLogSha256 = Get-Sha256 $rawPath
            timingSha256 = Get-Sha256 $timingPath
        }
    }
}

function New-ValidationFixture {
    param([string]$Root)
    New-Item -ItemType Directory -Path $Root | Out-Null
    $executable = Join-Path $Root 'generalszh.exe'
    [IO.File]::WriteAllText($executable, 'self-test only; never executed')
    $fixtureIds = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $unitCounts = @(1000, 4000, 8000, 12000)
    $fixtures = @()
    $stage3 = @()
    $runs = New-Object 'Collections.Generic.List[object]'
    $processId = 20000
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $fixturePath = Join-Path $Root "$($fixtureIds[$fixtureIndex]).rep"
        [IO.File]::WriteAllText($fixturePath, "fixture-$fixtureIndex")
        $fixtures += [ordered]@{
            id = $fixtureIds[$fixtureIndex]; path = $fixturePath
            sha256 = ('C' * 64); seed = 7; playerCount = 8
            peakUnitCount = $unitCounts[$fixtureIndex]
        }
        $stage3 += [ordered]@{
            id = $fixtureIds[$fixtureIndex]; measuredMedianMilliseconds = 100.0
        }
        foreach ($lane in @(
                [pscustomobject]@{ name = 'forced-one'; workers = 1; elapsed = 100.0 },
                [pscustomobject]@{ name = 'physical-8'; workers = 8; elapsed = 40.0 },
                [pscustomobject]@{ name = 'physical-16'; workers = 16; elapsed = 30.0 })) {
            for ($ordinal = 0; $ordinal -lt 4; ++$ordinal) {
                ++$processId
                $runs.Add((Write-RunReceipt $Root $executable $fixtureIds[$fixtureIndex] `
                    $fixturePath $lane.name $lane.workers $ordinal $processId `
                    $lane.elapsed $unitCounts[$fixtureIndex])) | Out-Null
            }
        }
    }
    $document = [ordered]@{
        schemaVersion = 1; title = 'ZeroHour'; sourceCommit = ('a' * 40)
        artifactSetSha256 = ('B' * 64); executablePath = $executable
        executableSha256 = ('A' * 64); fixtureManifestSha256 = ('D' * 64)
        stage3BaselineSha256 = ('E' * 64); taskRoot = $Root
        warmupRuns = 1; measuredRuns = 3; fixtures = $fixtures
        stage3Fixtures = $stage3
        topology = [ordered]@{
            source = 'GetSystemCpuSetInformation'; physicalCoreCount = 16
            logicalProcessorCount = 16; cpuSets = @()
        }
        runs = $runs.ToArray()
    }
    $manifest = Join-Path $Root 'validation.json'
    Write-Json $manifest $document
    return $manifest
}

function Update-Receipt {
    param([object]$Run, [scriptblock]$Mutation)
    $receipt = Get-Content -LiteralPath $Run.receiptPath -Raw | ConvertFrom-Json
    & $Mutation $receipt
    Write-Json $Run.receiptPath $receipt
    $Run.receiptSha256 = Get-Sha256 $Run.receiptPath
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Mutation)
    $caseRoot = Join-Path $testRoot $Name
    $manifest = New-ValidationFixture $caseRoot
    $document = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    & $Mutation $document
    Write-Json $manifest $document
    $rejected = $false
    try { & $runner -SelfTestValidationManifestPath $manifest | Out-Null }
    catch { $rejected = $true }
    Assert-True $rejected "Negative self-test '$Name' was not rejected."
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
    Assert-Rejected 'exit-mismatch' {
        param($document)
        $document.runs[0].host.exitCode = 1
    }
    Assert-Rejected 'tampered-raw' {
        param($document)
        $receipt = Get-Content -LiteralPath $document.runs[0].receiptPath -Raw |
            ConvertFrom-Json
        [IO.File]::AppendAllText($receipt.rawEvidence.rawLogPath, 'tampered')
    }
    Assert-Rejected 'tampered-timing' {
        param($document)
        $receipt = Get-Content -LiteralPath $document.runs[0].receiptPath -Raw |
            ConvertFrom-Json
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

    $afterProductProcesses = @(Get-Process generalsv, generalszh -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Id })
    Assert-True ((@($beforeProductProcesses) -join ',') -ceq
        (@($afterProductProcesses) -join ',')) `
        'Host self-tests launched or replaced a product process.'
    Assert-True ($runnerSource -match 'if \(\$PSCmdlet\.ParameterSetName -ceq ''SelfTest''\)[\s\S]*?return[\s\S]*?Invoke-Stage5InstalledPerformanceRun') `
        'Self-test parameter set must return before installed process invocation.'
    Assert-True ($runnerSource -match 'RTS_PERFORMANCE_RUN_ID' -and
        $runnerSource -match 'RTS_PERFORMANCE_RECEIPT_DIR' -and
        $runnerSource -match 'RTS_PERFORMANCE_VERIFIER_BOUNDARY') `
        'Runner must set the executable performance receipt contract.'
    Assert-True ($runnerSource -match 'GetSystemCpuSetInformation' -and
        $runnerSource -match 'physicalCoreCount -ge 16') `
        'Runner must fail closed on insufficient physical topology.'
    Write-Output 'Stage 5 performance scaling host validation self-tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
