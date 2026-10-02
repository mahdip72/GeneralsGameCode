param([string]$ScratchRoot = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-TestSha256 {
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

function Write-TestJson {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 30),
        (New-Object Text.UTF8Encoding($false)))
}

function Copy-TestValue {
    param([object]$Value)
    $json = $Value | ConvertTo-Json -Depth 30
    $command = Get-Command ConvertFrom-Json
    if ($command.Parameters.ContainsKey('DateKind')) {
        return $json | ConvertFrom-Json -DateKind String
    }
    return $json | ConvertFrom-Json
}

function Assert-ThrowsLike {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    $caught = $null
    try { & $Action }
    catch { $caught = $_ }
    Assert-True ($null -ne $caught -and
        $caught.Exception.Message -match $Pattern) $Message
}

function Assert-ReviewedFixtureMutationRejected {
    param(
        [Parameter(Mandatory = $true)][object]$Fixture,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Edit
    )
    $changed = Copy-TestValue $Fixture.document
    & $Edit $changed
    $path = Join-Path $Root ($Name + '.json')
    Write-TestJson $path $changed
    $hash = Get-TestSha256 $path
    Assert-ThrowsLike {
        Read-Stage5ReviewedNativeKernelFixture -Path $path `
            -ExpectedSha256 $hash -ExpectedTitle $Fixture.title `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedArtifactSetSha256 ('CC' * 32) `
            -ExpectedExecutableSha256 $Fixture.executableSha256 `
            -ExpectedDependencyManifestSha256 ('DD' * 32) `
            -ExpectedRuntimeClosureSha256 ('EE' * 32) | Out-Null
    } '(?i)review|manifest|fixture|map|timestamp|sha|scalar|integer|string' `
        "The reviewed fixture reader must reject $Name."
}

function Assert-ReceiptPlanMutationRejected {
    param(
        [Parameter(Mandatory = $true)][object]$Receipt,
        [Parameter(Mandatory = $true)][object]$Plan,
        [Parameter(Mandatory = $true)][object]$Start,
        [Parameter(Mandatory = $true)][string]$ReceiptPath,
        [Parameter(Mandatory = $true)][string]$PlanPath,
        [Parameter(Mandatory = $true)][string]$StartPath,
        [Parameter(Mandatory = $true)][string]$Name,
        [scriptblock]$EditPlan = $null,
        [scriptblock]$EditStart = $null
    )
    $changedPlan = Copy-TestValue $Plan
    $changedStart = Copy-TestValue $Start
    $changedReceipt = Copy-TestValue $Receipt
    try {
        if ($null -ne $EditPlan) { & $EditPlan $changedPlan }
        Write-TestJson $PlanPath $changedPlan
        $changedPlanSha256 = Get-TestSha256 $PlanPath
        $changedStart.plan.sha256 = $changedPlanSha256
        if ($null -ne $EditStart) { & $EditStart $changedStart }
        Write-TestJson $StartPath $changedStart
        $changedStartSha256 = Get-TestSha256 $StartPath
        $changedReceipt.prelaunchPlan.sha256 = $changedPlanSha256
        $changedReceipt.attemptStart.sha256 = $changedStartSha256
        Write-TestJson $ReceiptPath $changedReceipt
        $changedReceiptSha256 = Get-TestSha256 $ReceiptPath
        Assert-ThrowsLike {
            Read-Stage5NativePerformanceFixtureProductionReceipt `
                -Path $ReceiptPath -ExpectedSha256 $changedReceiptSha256 `
                -ExpectedTitle ([string]$Receipt.title) `
                -ExpectedCohortNonce ([string]$Receipt.cohortNonce) `
                -ExpectedCohortCreatedUtc ([string]$Receipt.cohortCreatedUtc) `
                -ExpectedSourceCommit ([string]$Receipt.sourceCommit) `
                -ExpectedArtifactSetSha256 ([string]$Receipt.artifactSetSha256) `
                -ExpectedExecutableSha256 ([string]$Receipt.executableSha256) `
                -ExpectedDependencyManifestSha256 `
                    ([string]$Receipt.runtimeClosure.dependencyManifestSha256) `
                -ExpectedRuntimeClosureSha256 `
                    ([string]$Receipt.runtimeClosure.closureSha256) | Out-Null
        } '(?i)plan|start|receipt|physical|native|json|scalar|integer|string' `
            "The production receipt reader must reject $Name."
    }
    finally {
        Write-TestJson $PlanPath $Plan
        Write-TestJson $StartPath $Start
        Write-TestJson $ReceiptPath $Receipt
    }
}

function New-TestReviewedFixture {
    param([string]$Root, [string]$Title = 'Generals')
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $mapPath = Join-Path $Root 'Stage5Dense.map'
    $bytes = New-Object byte[] 16384
    for ($index = 0; $index -lt $bytes.Length; ++$index) {
        $bytes[$index] = [byte](($index * 31 + 17) % 251)
    }
    [IO.File]::WriteAllBytes($mapPath, $bytes)
    $mapSha256 = Get-TestSha256 $mapPath
    $executableSha256 = if ($Title -ceq 'Generals') {
        'AA' * 32
    } else { 'BB' * 32 }
    $epoch = if ($Title -ceq 'Generals') { 1 } else { 3 }
    $marker = if ($Title -ceq 'Generals') {
        ' [GeneralsAIPlanningEpoch=1]'
    } else { ' [SkirmishAIEpoch=3]' }
    $document = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-reviewed-native-kernel-fixture'
        reviewStatus = 'approved'
        reviewScope = 'native-dense-eight-player-kernel-execution-v1'
        reviewedUtc = '2026-09-04T10:00:00.0000000Z'
        reviewedBy = 'stage5-premium-review'
        title = $Title
        sourceCommit = 'a' * 40
        artifactSetSha256 = 'CC' * 32
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = 'DD' * 32
            closureSha256 = 'EE' * 32
        }
        executableSha256 = $executableSha256
        fixture = [ordered]@{
            id = 'dense-eight-player'
            kind = 'native-map'
            source = 'Stage5Dense.map'
            profileRelativePath = 'Maps\Stage5Dense\Stage5Dense.map'
            mapKey = 'Maps\Stage5Dense\Stage5Dense.map'
            sha256 = $mapSha256
            byteCount = 16384
            seed = 1729
            frameBudget = 3600
            expectedPlayerCount = 8
            minimumInitialUnitCount = 8000
            minimumPeakUnitCount = 8000
        }
        requirements = [ordered]@{
            nativeMapLoad = $true
            exactEightPlayerRoster = $true
            naturalVictory = $true
            naturallyClosedReplay = $true
            requiredKernelFamilies = @(
                'physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
        }
    }
    $manifestPath = Join-Path $Root 'ReviewedFixture.json'
    Write-TestJson $manifestPath $document
    return [pscustomobject]@{
        root = $Root
        title = $Title
        epoch = $epoch
        marker = $marker
        mapPath = $mapPath
        mapSha256 = $mapSha256
        executableSha256 = $executableSha256
        manifestPath = $manifestPath
        manifestSha256 = Get-TestSha256 $manifestPath
        document = $document
    }
}

function New-TestNativeOutput {
    param([object]$Fixture, [string]$ReplayPath, [string]$ReplaySha256,
        [int]$ProcessId = 900)
    $nativeNonce = '{0:X8}-{1:X8}-{2:X8}' -f $ProcessId, 0x12345678, 1729
    $title = $Fixture.title
    $map = 'Maps\Stage5Dense\Stage5Dense.map'
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_START category=native-performance-fixture title={0} map="{1}" seed=1729 frame_budget=3600 map_crc=1234ABCD map_size=16384 map_sha256={2} executable_sha256={3} run_nonce={4}' -f
        $title, $map, $Fixture.mapSha256, $Fixture.executableSha256,
        $nativeNonce)) | Out-Null
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $controller = if ($slot -eq 0) { 'human' } else { 'brutal-ai' }
        $team = if ($slot -lt 4) { 0 } else { 1 }
        $owner = if ($slot -eq 0) { 'teamplayer0' } else {
            'teamSkirmishAmerica{0}' -f $slot
        }
        $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_ROSTER slot={0} controller={1} faction=FactionAmerica faction_index=1 start={0} color={0} team={2} observer=0 map_owner={3} owner_verified=1' -f
            $slot, $controller, $team, $owner)) | Out-Null
    }
    $lines.Add('STAGE5_PERFORMANCE_FIXTURE_OBSERVED first_frame=1 actual_players=8 initial_units=8000 initial_unrostered_units=0') | Out-Null
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_PLAYER_UNITS slot={0} initial_units=1000 peak_units=1500' -f $slot)) | Out-Null
    }
    $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_COMPLETE category=native-performance-fixture title={0} map="{1}" seed=1729 frame_budget=3600 map_crc=1234ABCD map_size=16384 map_sha256={2} actual_players=8 initial_units=8000 peak_units=12000 initial_unrostered_units=0 peak_unrostered_units=0 observed_first_frame=1 observed_last_frame=1201 observed_frame_samples=1201 winner_team=0 end_frame=1200 final_crc=12345678 replay_epoch={3} ai_epoch_marker="{4}" replay_frame_count=1201 executable_sha256={5} run_nonce={6} replay_sha256={7} retained_replay="{8}"' -f
        $title, $map, $Fixture.mapSha256, $Fixture.epoch, $Fixture.marker,
        $Fixture.executableSha256, $nativeNonce, $ReplaySha256, $ReplayPath)) | Out-Null
    return $lines.ToArray() -join "`r`n"
}

$modulePath = Join-Path $PSScriptRoot 'Stage5NativePerformanceFixtureProduction.psm1'
Assert-True (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    'The native performance fixture production module must exist.'
Import-Module $modulePath -Force

$ownsRoot = [string]::IsNullOrWhiteSpace($ScratchRoot)
$testRoot = if ($ownsRoot) {
    Join-Path ([IO.Path]::GetTempPath()) ('Stage5NativeFixture-' +
        [Guid]::NewGuid().ToString('N'))
} else { [IO.Path]::GetFullPath($ScratchRoot) }
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

try {
    $fixture = New-TestReviewedFixture (Join-Path $testRoot 'valid')
    $reviewed = Read-Stage5ReviewedNativeKernelFixture `
        -Path $fixture.manifestPath `
        -ExpectedSha256 $fixture.manifestSha256 `
        -ExpectedTitle $fixture.title `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 ('CC' * 32) `
        -ExpectedExecutableSha256 $fixture.executableSha256 `
        -ExpectedDependencyManifestSha256 ('DD' * 32) `
        -ExpectedRuntimeClosureSha256 ('EE' * 32)
    Assert-True ($reviewed.fixture.sha256 -ceq $fixture.mapSha256 -and
        $reviewed.fixture.byteCount -eq 16384) `
        'The reviewed native map must be reopened and bound to its exact bytes.'

    foreach ($case in @(
            [pscustomobject]@{ name = 'a one-element title array'; edit = {
                param($d) $d.title = @('Generals') } },
            [pscustomobject]@{ name = 'a multi-element title array'; edit = {
                param($d) $d.title = @('Generals', 'extra') } },
            [pscustomobject]@{ name = 'a one-element reviewed timestamp array'; edit = {
                param($d) $d.reviewedUtc = @('2026-09-04T10:00:00.0000000Z') } },
            [pscustomobject]@{ name = 'a one-element map-source array'; edit = {
                param($d) $d.fixture.source = @('Stage5Dense.map') } },
            [pscustomobject]@{ name = 'a one-element map-hash array'; edit = {
                param($d) $d.fixture.sha256 = @($d.fixture.sha256) } },
            [pscustomobject]@{ name = 'a boolean title scalar'; edit = {
                param($d) $d.title = $true } },
            [pscustomobject]@{ name = 'a string byte count scalar'; edit = {
                param($d) $d.fixture.byteCount = '16384' } },
            [pscustomobject]@{ name = 'a scalar kernel-family value'; edit = {
                param($d) $d.requirements.requiredKernelFamilies = 'physics' } },
            [pscustomobject]@{ name = 'a fractional byte count'; edit = {
                param($d) $d.fixture.byteCount = 16384.5 } },
            [pscustomobject]@{ name = 'a fractional seed'; edit = {
                param($d) $d.fixture.seed = 1729.5 } }
        )) {
        Assert-ReviewedFixtureMutationRejected $fixture $testRoot `
            ('reviewed-' + ($case.name -replace '[^A-Za-z0-9]+', '-').Trim('-')) `
            $case.edit
    }

    $argumentString = Get-Stage5NativePerformanceFixtureArgumentString `
        -MapKey $reviewed.fixture.mapKey -Seed $reviewed.fixture.seed `
        -FrameBudget $reviewed.fixture.frameBudget `
        -ExecutableSha256 $fixture.executableSha256
    Assert-True ($argumentString -ceq ('-headless -noFPSLimit -pipelineMode serial ' +
        '-simulationMode parallel -workerPolicy auto -validationExecutableSha256 ' +
        $fixture.executableSha256 + ' -workerCount 4 -runStage5PerformanceFixture ' +
        'Maps\Stage5Dense\Stage5Dense.map 1729 3600')) `
        'Fixture production must use the exact non-scaling physical-4 command line.'

    $bundleRoot = Join-Path $testRoot 'bundle'
    [IO.Directory]::CreateDirectory($bundleRoot) | Out-Null
    $profileRoot = Join-Path $bundleRoot `
        'TitleSession\Documents\Command and Conquer Generals Data'
    [IO.Directory]::CreateDirectory($profileRoot) | Out-Null
    $replayPath = Join-Path $profileRoot 'Stage5Performance-1729-00000384-12345678-000006C1.rep'
    [IO.File]::WriteAllBytes($replayPath, [Text.Encoding]::UTF8.GetBytes(
        'synthetic bytes used only by the host contract test'))
    $replaySha256 = Get-TestSha256 $replayPath
    $output = New-TestNativeOutput $fixture $replayPath $replaySha256
    $completion = ConvertFrom-Stage5NativePerformanceFixtureOutput `
        -Text $output -ReviewedFixture $reviewed -ExpectedProcessId 900 `
        -ProfileRoot $profileRoot
    Assert-True ($completion.nativeRunNonce -ceq
        '00000384-12345678-000006C1' -and
        $completion.replaySha256 -ceq $replaySha256 -and
        $completion.observedFrameSamples -eq 1201) `
        'The native log reader must close process, map, roster, workload, and replay identity.'

    $manifestSchema = Join-Path $PSScriptRoot `
        'Stage5ReviewedNativeKernelFixture.schema.json'
    $receiptSchema = Join-Path $PSScriptRoot `
        'Stage5NativePerformanceFixtureProduction.schema.json'
    Assert-True (Test-Path -LiteralPath $manifestSchema -PathType Leaf) `
        'The reviewed native fixture schema must exist.'
    Assert-True (Test-Path -LiteralPath $receiptSchema -PathType Leaf) `
        'The native fixture production receipt schema must exist.'
    Assert-True ((Get-Content -LiteralPath $fixture.manifestPath -Raw) |
        Test-Json -SchemaFile $manifestSchema -ErrorAction SilentlyContinue) `
        'The valid reviewed-map manifest must satisfy its closed schema.'

    foreach ($relativeDirectory in @('inputs', 'logs', 'replays', 'prelaunch')) {
        [IO.Directory]::CreateDirectory((Join-Path $bundleRoot $relativeDirectory)) |
            Out-Null
    }
    Copy-Item -LiteralPath $fixture.manifestPath `
        -Destination (Join-Path $bundleRoot 'inputs/ReviewedFixture.json')
    Copy-Item -LiteralPath $fixture.mapPath `
        -Destination (Join-Path $bundleRoot 'inputs/Stage5Dense.map')
    [IO.File]::WriteAllText((Join-Path $bundleRoot 'logs/fixture-output.log'),
        $output, (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $replayPath `
        -Destination (Join-Path $bundleRoot 'replays/Stage5Performance.rep')
    $rawLogSha256 = Get-TestSha256 `
        (Join-Path $bundleRoot 'logs/fixture-output.log')
    $planPath = Join-Path $bundleRoot 'prelaunch/fixture-plan.json'
    $plan = [ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-plan'
        recordedUtc = '2026-09-04T11:05:00.0000000Z'
        hostRunNonce = '20000000-0000-4000-8000-000000000001'
        cohortNonce = '10000000-0000-4000-8000-000000000001'
        cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
        title = 'Generals'
        sourceCommit = 'a' * 40
        artifactSetSha256 = 'CC' * 32
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = 'DD' * 32
            closureSha256 = 'EE' * 32
        }
        executablePath = (Join-Path $testRoot 'generalsv.exe')
        executableSha256 = 'AA' * 32
        reviewedFixtureManifestPath = $fixture.manifestPath
        reviewedFixtureManifestSha256 = $fixture.manifestSha256
        mapSourcePath = $fixture.mapPath
        mapSha256 = $fixture.mapSha256
        mapDestinationPath = (Join-Path $profileRoot `
            'Maps\Stage5Dense\Stage5Dense.map')
        argumentString = $argumentString
        taskRoot = $bundleRoot
        timeoutSeconds = 7200
        workerCount = 4
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
    }
    Write-TestJson $planPath $plan
    $planSha256 = Get-TestSha256 $planPath
    $startPath = Join-Path $bundleRoot 'prelaunch/fixture-start.json'
    Write-TestJson $startPath ([ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-start'
        recordedUtc = '2026-09-04T11:10:00.0000000Z'
        plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
        hostRunNonce = '20000000-0000-4000-8000-000000000001'
    })
    $startSha256 = Get-TestSha256 $startPath

    # Exercise historical marker-free production completion compatibility
    # through the actual publisher and its complete receipt validation.
    $legacyCompletion = $completion.PSObject.Copy()
    $legacyCompletion.PSObject.Properties.Remove('diagnosticOnly')
    $receipt = New-Stage5NativePerformanceFixtureProductionReceipt `
        -Title $fixture.title -RecordedUtc '2026-09-04T11:30:00.0000000Z' `
        -CohortNonce '10000000-0000-4000-8000-000000000001' `
        -CohortCreatedUtc '2026-09-04T11:00:00.0000000Z' `
        -SourceCommit ('a' * 40) -ArtifactSetSha256 ('CC' * 32) `
        -DependencyManifestSha256 ('DD' * 32) `
        -RuntimeClosureSha256 ('EE' * 32) `
        -ReviewedFixtureManifestPath 'inputs/ReviewedFixture.json' `
        -ReviewedFixtureManifestSha256 $fixture.manifestSha256 `
        -ExecutableSha256 $fixture.executableSha256 `
        -HostRunNonce '20000000-0000-4000-8000-000000000001' `
        -ProcessId 900 -ProcessCreationTimeUtc100ns 90000 `
        -CommandLine ('generalsv.exe ' + $argumentString) `
        -ArgumentString $argumentString -NativeProfileRoot $profileRoot `
        -TaskRoot $bundleRoot `
        -Completion $legacyCompletion `
        -RawLogPath 'logs/fixture-output.log' -RawLogSha256 $rawLogSha256 `
        -RetainedReplayPath 'replays/Stage5Performance.rep' `
        -PrelaunchPlanPath 'prelaunch/fixture-plan.json' `
        -PrelaunchPlanSha256 $planSha256 `
        -AttemptStartPath 'prelaunch/fixture-start.json' `
        -AttemptStartSha256 $startSha256 `
        -Lifecycle ([pscustomobject]@{
            mutexAcquired = $true
            noInstalledTitleProcessesAtPreflight = $true
            childExitProven = $true
            registryRestored = $true
            profileRemoved = $true
            recoveryJournalAbsent = $true
        })
    Assert-True (($receipt | ConvertTo-Json -Depth 30) |
        Test-Json -SchemaFile $receiptSchema -ErrorAction SilentlyContinue) `
        'The closed fixture-production receipt must satisfy its schema.'
    Assert-Stage5NativePerformanceFixtureProductionReceipt $receipt |
        Out-Null
    foreach ($edit in @(
        { param($d) $d.completion.peakUnitCount = 12001 },
        { param($d) $d.completion.playerUnits[0].peakUnitCount = 12001 },
        { param($d) $d.completion.observedLastFrame = 1200; $d.completion.observedFrameSamples = 1200 },
        { param($d) $d.completion.observedFrameSamples = 1200 }
    )) {
        $badClosure = Copy-TestValue $receipt
        & $edit $badClosure
        Assert-ThrowsLike { Assert-Stage5NativePerformanceFixtureProductionReceipt $badClosure | Out-Null } `
            'workload|closure|frame' 'Receipt semantic validation accepted inconsistent global peaks or completed-frame coverage.'
    }
    $receiptPath = Join-Path $bundleRoot 'Stage5NativePerformanceFixture.json'
    Write-TestJson $receiptPath $receipt
    $receiptSha256 = Get-TestSha256 $receiptPath
    $reopened = Read-Stage5NativePerformanceFixtureProductionReceipt `
        -Path $receiptPath -ExpectedSha256 $receiptSha256 `
        -ExpectedTitle Generals `
        -ExpectedCohortNonce '10000000-0000-4000-8000-000000000001' `
        -ExpectedCohortCreatedUtc '2026-09-04T11:00:00.0000000Z' `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 ('CC' * 32) `
        -ExpectedExecutableSha256 ('AA' * 32) `
        -ExpectedDependencyManifestSha256 ('DD' * 32) `
        -ExpectedRuntimeClosureSha256 ('EE' * 32)
    Assert-True ($reopened.fixture.id -ceq 'dense-eight-player' -and
        $reopened.fixture.sha256 -ceq $replaySha256 -and
        $reopened.prelaunchPlan.sha256 -ceq $planSha256 -and
        $reopened.attemptStart.sha256 -ceq $startSha256 -and
        $reopened.fixture.path -ceq
            (Join-Path $bundleRoot 'replays/Stage5Performance.rep')) `
        'The production receipt reader must reopen the full reviewed-map/raw-log/replay closure.'

    Assert-ReceiptPlanMutationRejected $receipt $plan `
        ([pscustomobject]@{
            schemaVersion = 1
            event = 'native-fixture-production-start'
            recordedUtc = '2026-09-04T11:10:00.0000000Z'
            plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
            hostRunNonce = '20000000-0000-4000-8000-000000000001'
        }) `
        $receiptPath $planPath $startPath 'an array-valued plan event' `
        { param($d) $d.event = @('native-fixture-production-plan') }

    Assert-ReceiptPlanMutationRejected $receipt $plan `
        ([pscustomobject]@{
            schemaVersion = 1
            event = 'native-fixture-production-start'
            recordedUtc = '2026-09-04T11:10:00.0000000Z'
            plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
            hostRunNonce = '20000000-0000-4000-8000-000000000001'
        }) `
        $receiptPath $planPath $startPath 'an array-valued start timestamp' `
        $null { param($d) $d.recordedUtc = @('2026-09-04T11:10:00.0000000Z') }

    Assert-ReceiptPlanMutationRejected $receipt $plan `
        ([pscustomobject]@{
            schemaVersion = 1
            event = 'native-fixture-production-start'
            recordedUtc = '2026-09-04T11:10:00.0000000Z'
            plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
            hostRunNonce = '20000000-0000-4000-8000-000000000001'
        }) `
        $receiptPath $planPath $startPath 'an array-valued plan path' `
        { param($d) $d.mapDestinationPath = @($d.mapDestinationPath) }

    $relocatedRoot = Join-Path $testRoot 'relocated-bundle'
    Copy-Item -LiteralPath $bundleRoot -Destination $relocatedRoot -Recurse
    $relocatedReceiptPath = Join-Path $relocatedRoot `
        'Stage5NativePerformanceFixture.json'
    $relocated = Read-Stage5NativePerformanceFixtureProductionReceipt `
        -Path $relocatedReceiptPath -ExpectedSha256 $receiptSha256 `
        -ExpectedTitle Generals `
        -ExpectedCohortNonce '10000000-0000-4000-8000-000000000001' `
        -ExpectedCohortCreatedUtc '2026-09-04T11:00:00.0000000Z' `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 ('CC' * 32) `
        -ExpectedExecutableSha256 ('AA' * 32) `
        -ExpectedDependencyManifestSha256 ('DD' * 32) `
        -ExpectedRuntimeClosureSha256 ('EE' * 32)
    Assert-True ($relocated.fixture.path -ceq
        (Join-Path $relocatedRoot 'replays/Stage5Performance.rep')) `
        'Immutable fixture-production evidence must remain verifiable after exact relocation.'

    $badReceipts = @(
        [pscustomobject]@{ name = 'a scaling claim'; edit = {
            param($d) $d.performanceScalingClaim = $true } },
        [pscustomobject]@{ name = 'a missing cleanup proof'; edit = {
            param($d) $d.lifecycle.registryRestored = $false } },
        [pscustomobject]@{ name = 'a UUID substituted for the native nonce'; edit = {
            param($d) $d.completion.nativeRunNonce =
                '30000000-0000-4000-8000-000000000001' } },
        [pscustomobject]@{ name = 'a Windows alternate-data-stream path'; edit = {
            param($d) $d.rawLog.path = 'logs/fixture-output.log:forged' } })
    foreach ($case in $badReceipts) {
        $changed = Copy-TestValue $receipt
        & $case.edit $changed
        Assert-True (-not (($changed | ConvertTo-Json -Depth 30) |
            Test-Json -SchemaFile $receiptSchema -ErrorAction SilentlyContinue)) `
            "The receipt schema must reject $($case.name)."
    }
    $detachedReplay = Copy-TestValue $receipt
    $detachedReplay.retainedReplay.sha256 = '99' * 32
    Assert-ThrowsLike {
        Assert-Stage5NativePerformanceFixtureProductionReceipt `
            $detachedReplay | Out-Null
    } '(?i)replay|projection|detached' `
        'Semantic validation must reject a mismatched replay binding.'

    $changedPlan = Copy-TestValue $plan
    $changedPlan.workerCount = 3
    Write-TestJson $planPath $changedPlan
    $changedPlanSha256 = Get-TestSha256 $planPath
    $changedStart = [ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-start'
        recordedUtc = '2026-09-04T11:10:00.0000000Z'
        plan = [ordered]@{ path = $planPath; sha256 = $changedPlanSha256 }
        hostRunNonce = '20000000-0000-4000-8000-000000000001'
    }
    Write-TestJson $startPath $changedStart
    $changedStartSha256 = Get-TestSha256 $startPath
    $detachedPlanReceipt = Copy-TestValue $receipt
    $detachedPlanReceipt.prelaunchPlan.sha256 = $changedPlanSha256
    $detachedPlanReceipt.attemptStart.sha256 = $changedStartSha256
    Write-TestJson $receiptPath $detachedPlanReceipt
    $detachedPlanReceiptSha256 = Get-TestSha256 $receiptPath
    Assert-ThrowsLike {
        Read-Stage5NativePerformanceFixtureProductionReceipt `
            -Path $receiptPath -ExpectedSha256 $detachedPlanReceiptSha256 `
            -ExpectedTitle Generals `
            -ExpectedCohortNonce '10000000-0000-4000-8000-000000000001' `
            -ExpectedCohortCreatedUtc '2026-09-04T11:00:00.0000000Z' `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedArtifactSetSha256 ('CC' * 32) `
            -ExpectedExecutableSha256 ('AA' * 32) `
            -ExpectedDependencyManifestSha256 ('DD' * 32) `
            -ExpectedRuntimeClosureSha256 ('EE' * 32) | Out-Null
    } '(?i)physical-4|worker|prelaunch|plan' `
        'The reader must reject a rehashed prelaunch plan that changes physical-4 authority.'
    Write-TestJson $planPath $plan
    Write-TestJson $startPath ([ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-start'
        recordedUtc = '2026-09-04T11:10:00.0000000Z'
        plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
        hostRunNonce = '20000000-0000-4000-8000-000000000001'
    })
    Write-TestJson $receiptPath $receipt

    $tooSmall = New-TestReviewedFixture (Join-Path $testRoot 'tiny')
    [IO.File]::WriteAllBytes($tooSmall.mapPath, (New-Object byte[] 128))
    $tooSmall.document.fixture.sha256 = Get-TestSha256 $tooSmall.mapPath
    $tooSmall.document.fixture.byteCount = 128
    Write-TestJson $tooSmall.manifestPath $tooSmall.document
    $tooSmallHash = Get-TestSha256 $tooSmall.manifestPath
    Assert-ThrowsLike {
        Read-Stage5ReviewedNativeKernelFixture -Path $tooSmall.manifestPath `
            -ExpectedSha256 $tooSmallHash -ExpectedTitle Generals `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedArtifactSetSha256 ('CC' * 32) `
            -ExpectedExecutableSha256 ('AA' * 32) `
            -ExpectedDependencyManifestSha256 ('DD' * 32) `
            -ExpectedRuntimeClosureSha256 ('EE' * 32) | Out-Null
    } '(?i)placeholder|16384|byte|non-dense|recorder contract' `
        'A tiny placeholder must fail before any title process can launch.'

    $invalidOutputs = @(
        [pscustomobject]@{ name = 'a native failure line'; text =
            ($output + "`r`nSTAGE5_PERFORMANCE_FIXTURE_FAIL reason=test") },
        [pscustomobject]@{ name = 'a duplicate completion'; text =
            ($output + "`r`n" + (@($output -split '\r?\n') |
                Where-Object { $_ -like 'STAGE5_PERFORMANCE_FIXTURE_COMPLETE*' })) },
        [pscustomobject]@{ name = 'a relabelled map hash'; text =
            $output.Replace($fixture.mapSha256, ('99' * 32)) },
        [pscustomobject]@{ name = 'a non-dense workload'; text =
            $output.Replace('initial_units=8000', 'initial_units=7999') },
        [pscustomobject]@{ name = 'a detached replay'; text =
            $output.Replace($replayPath, (Join-Path $testRoot 'outside.rep')) })
    foreach ($case in $invalidOutputs) {
        Assert-ThrowsLike {
            ConvertFrom-Stage5NativePerformanceFixtureOutput `
                -Text $case.text -ReviewedFixture $reviewed `
                -ExpectedProcessId 900 -ProfileRoot $profileRoot | Out-Null
        } '(?i)fixture|map|workload|replay|completion|fail' `
            "The native parser must reject $($case.name)."
    }

    Write-Output 'Stage 5 native performance fixture production contract tests passed.'
}
finally {
    if ($ownsRoot -and (Test-Path -LiteralPath $testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
