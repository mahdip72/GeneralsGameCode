param(
    [string]$ScratchRoot = '',
    [switch]$ModuleScopePreflightOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# Windows PowerShell 5.1 has no native Test-Json. The production paths under
# test perform their own closed-shape and scalar validation; this compatibility
# shim only lets the focused fixture reach those checks on both supported hosts.
if ($null -eq (Get-Command Test-Json -ErrorAction SilentlyContinue)) {
    function Test-Json {
        [CmdletBinding()]
        param([Parameter(ValueFromPipeline = $true)][object]$InputObject,
            [string]$SchemaFile)
        process { return $true }
    }
}

function Assert-InstalledTestCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-InstalledTestSha256Bytes {
    param([byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $algorithm.Dispose() }
}

function New-InstalledTestTextSnapshot {
    param([string]$Text, [string]$Path = 'H:\synthetic-evidence')
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($Text)
    return [pscustomobject]@{
        path = $Path
        bytes = $bytes
        length = [Int64]$bytes.Length
        sha256 = Get-InstalledTestSha256Bytes $bytes
        identity = [pscustomobject]@{
            volumeSerialNumber = [UInt64]1
            fileIdLow = [UInt64]1
            fileIdHigh = [UInt64]0
        }
    }
}

function Set-InstalledTestSnapshotJsonPropertyLiteral {
    param([object]$Snapshot, [string]$Property, [string]$Literal)
    $json = [Text.Encoding]::UTF8.GetString([byte[]]$Snapshot.bytes)
    $pattern = '("' + [regex]::Escape($Property) +
        '"\s*:\s*)(-?(?:\d+(?:\.\d*)?(?:[eE][+-]?\d+)?))'
    $match = [regex]::Match($json, $pattern)
    Assert-InstalledTestCondition $match.Success `
        "Could not locate numeric JSON property '$Property'."
    $value = $match.Groups[2]
    $json = $json.Substring(0, $value.Index) + $Literal +
        $json.Substring($value.Index + $value.Length)
    $Snapshot.bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json)
    $Snapshot.length = [Int64]$Snapshot.bytes.Length
    $Snapshot.sha256 = Get-InstalledTestSha256Bytes $Snapshot.bytes
}

function Set-InstalledTestFileJsonPropertyLiteral {
    param([string]$Path, [string]$Property, [string]$Literal)
    $json = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($Path))
    $pattern = '("' + [regex]::Escape($Property) +
        '"\s*:\s*)(-?(?:\d+(?:\.\d*)?(?:[eE][+-]?\d+)?))'
    $match = [regex]::Match($json, $pattern)
    Assert-InstalledTestCondition $match.Success `
        "Could not locate numeric JSON property '$Property'."
    $value = $match.Groups[2]
    $json = $json.Substring(0, $value.Index) + $Literal +
        $json.Substring($value.Index + $value.Length)
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Copy-InstalledTestValue {
    param([object]$Value)
    $json = $Value | ConvertTo-Json -Depth 40
    $command = Get-Command ConvertFrom-Json
    if ($command.Parameters.ContainsKey('DateKind')) {
        return $json | ConvertFrom-Json -DateKind String
    }
    return $json | ConvertFrom-Json
}

function Invoke-InstalledTestNativeReaderMissingPathProbe {
    param(
        [Parameter(Mandatory = $true)][object]$KernelModule,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $missingPath = Join-Path 'H:\' `
        ('stage5-module-scope-missing-' + [Guid]::NewGuid().ToString('N') + '.json')
    Assert-InstalledTestCondition (-not (Test-Path -LiteralPath $missingPath)) `
        "$Context unexpectedly found its missing-path fixture: $missingPath"
    $caught = $null
    try {
        & $KernelModule {
            param([string]$Path)
            Read-Stage5NativePerformanceFixtureProductionReceipt `
                -Path $Path -ExpectedSha256 ('A' * 64) `
                -ExpectedTitle Generals `
                -ExpectedCohortNonce '10000000-0000-4000-8000-000000000001' `
                -ExpectedCohortCreatedUtc '2026-09-04T11:30:00.0000000Z' `
                -ExpectedSourceCommit ('a' * 40) `
                -ExpectedArtifactSetSha256 ('B' * 64) `
                -ExpectedExecutableSha256 ('C' * 64) `
                -ExpectedDependencyManifestSha256 ('D' * 64) `
                -ExpectedRuntimeClosureSha256 ('E' * 64)
        } $missingPath | Out-Null
    }
    catch { $caught = $_ }
    $message = if ($null -eq $caught) { '' } else {
        [string]$caught.Exception.Message
    }
    Assert-InstalledTestCondition ($null -ne $caught -and
        $message -match '(?i)cannot find path|does not exist|not found' -and
        $message -notmatch '(?i)not recognized|Get-Stage5FinalAcceptanceFileSnapshot') `
        "$Context did not reach the native reader's real missing-path validation; observed: $message"
}

function Invoke-InstalledTestPrivateGroupingProbe {
    param(
        [Parameter(Mandatory = $true)][object]$EvidenceModule,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $results = @(
        [ordered]@{ kind = 'ai'; determinismKey = 'module-scope-case'; configuration = 'serial-1'; repeat = 1 }
        [ordered]@{ kind = 'ai'; determinismKey = 'module-scope-case'; configuration = 'parallel-1'; repeat = 1 }
    )
    $groups = & $EvidenceModule {
        param([object[]]$InputResults, [string]$GroupingContext)
        Get-Stage5DeterminismGroups $InputResults $GroupingContext
    } $results "$Context grouping"
    Assert-InstalledTestCondition (@($groups).Count -eq 1 -and
        $groups[0].Name -ceq 'module-scope-case' -and
        $groups[0].Count -eq 2 -and @($groups[0].Group).Count -eq 2) `
        "$Context private determinism grouping did not retain its named result group."
    $missingKey = [ordered]@{ kind = 'ai'; configuration = 'serial-1'; repeat = 1 }
    $caught = $null
    try {
        & $EvidenceModule {
            param([object[]]$InputResults, [string]$GroupingContext)
            Get-Stage5DeterminismGroups $InputResults $GroupingContext | Out-Null
        } @($missingKey) "$Context grouping"
    }
    catch { $caught = $_ }
    Assert-InstalledTestCondition ($null -ne $caught -and
        [string]$caught.Exception.Message -match "missing property 'determinismKey'") `
        "$Context private determinism grouping accepted a missing key."
}

function Get-InstalledTestScalarMutations {
    param([object]$GoodValue)
    $stringValue = if ($GoodValue -is [string]) { 'wrong-kind' } else {
        [string]$GoodValue
    }
    $doubleValue = if ($GoodValue -is [bool] -or $GoodValue -is [string]) {
        [double]1.0
    } else { [double]$GoodValue }
    $fractionValue = if ($GoodValue -is [bool] -or $GoodValue -is [string]) {
        [double]1.5
    } else { [double]$GoodValue + 0.5 }
    return @(
        [pscustomobject]@{ name = 'one-element-array'; value = @($GoodValue) }
        [pscustomobject]@{ name = 'two-element-array'; value = @($GoodValue,
                $GoodValue) }
        [pscustomobject]@{ name = 'string'; value = $stringValue }
        [pscustomobject]@{ name = 'double-integer'; value = $doubleValue
            literal = '1.0' }
        [pscustomobject]@{ name = 'double-fraction'; value = $fractionValue
            literal = '1.5' }
        [pscustomobject]@{ name = 'boolean'; value = if ($GoodValue -is [bool]) {
                -not [bool]$GoodValue
            } else { $true } }
        [pscustomobject]@{ name = 'object'; value = [pscustomobject]@{ value = $GoodValue } }
    )
}

function Invoke-InstalledTestSourceHostRead {
    param(
        [object]$Module,
        [string]$Path,
        [string]$ExpectedSha256,
        [string]$ExpectedTitle,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [string]$ExpectedDependencyManifestSha256,
        [string]$ExpectedRuntimeClosureSha256,
        [string]$ExpectedExecutableSha256
    )
    & $Module {
        param($Arguments)
        Read-Stage5InstalledKernelSourceHost @Arguments | Out-Null
    } ([ordered]@{
        Path = $Path
        ExpectedSha256 = $ExpectedSha256
        ExpectedTitle = $ExpectedTitle
        ExpectedSourceCommit = $ExpectedSourceCommit
        ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
        ExpectedCohortNonce = $ExpectedCohortNonce
        ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
        ExpectedDependencyManifestSha256 = $ExpectedDependencyManifestSha256
        ExpectedRuntimeClosureSha256 = $ExpectedRuntimeClosureSha256
        ExpectedExecutableSha256 = $ExpectedExecutableSha256
    })
}

function Invoke-InstalledTestIndependentReader {
    param(
        [object]$Module,
        [string]$Path,
        [string]$ExpectedSha256,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedArtifactSetSha256,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [string]$ExpectedDependencyManifestSha256,
        [string]$ExpectedRuntimeClosureSha256,
        [string]$GeneralsExecutableSha256,
        [string]$ZeroHourExecutableSha256
    )
    & $Module {
        param($Arguments)
        # The PS7 schema cmdlet is deliberately shadowed for this matrix so
        # that the reader's own scalar guards, rather than Test-Json, reject
        # each malformed value. Windows PowerShell 5.1 has no Test-Json.
        function Test-Json {
            param([Parameter(ValueFromPipeline = $true)][object]$InputObject,
                [string]$SchemaFile)
            process { return $true }
        }
        Read-Stage5InstalledKernelExecutionEvidence @Arguments | Out-Null
    } ([ordered]@{
        Path = $Path
        ExpectedSha256 = $ExpectedSha256
        ExpectedSourceCommit = $ExpectedSourceCommit
        ExpectedArtifactSetSha256 = $ExpectedArtifactSetSha256
        ExpectedCohortNonce = $ExpectedCohortNonce
        ExpectedCohortCreatedUtc = $ExpectedCohortCreatedUtc
        ExpectedDependencyManifestSha256 = $ExpectedDependencyManifestSha256
        ExpectedRuntimeClosureSha256 = $ExpectedRuntimeClosureSha256
        GeneralsExecutableSha256 = $GeneralsExecutableSha256
        ZeroHourExecutableSha256 = $ZeroHourExecutableSha256
    })
}

function New-InstalledTestProjectionInputs {
    param([object]$Receipt, [object]$Run, [object]$Artifact,
        [string]$ReplaySha256)
    $raw = New-InstalledTestTextSnapshot `
        'synthetic game-owned raw evidence; never runtime authority'
    $timing = New-InstalledTestTextSnapshot "frame,logic_ns`r`n1,100`r`n"
    $Receipt.rawEvidence.rawLogSha256 = $raw.sha256
    $Receipt.rawEvidence.timingSha256 = $timing.sha256
    $Receipt.rawLogs[0].sha256 = $raw.sha256
    $Receipt.rawLogs[1].sha256 = $timing.sha256
    $receiptSnapshot = New-InstalledTestTextSnapshot `
        ($Receipt | ConvertTo-Json -Depth 40)
    $hostBinding = Copy-InstalledTestValue $Run.host
    $hostBinding.rawLogSha256 = [string]$raw.sha256
    $hostBinding.timingSha256 = [string]$timing.sha256
    $runBinding = [pscustomobject]@{
        fixtureId = 'dense-eight-player'
        lane = 'physical-4'
        ordinal = 1
        warmup = $false
        runId = [string]$Receipt.runId
        runNonce = [string]$Receipt.runNonce
        expectedArgumentString = [string]$Run.expectedArgumentString
        receiptPath = 'H:\synthetic\receipt.json'
        receiptSha256 = [string]$receiptSnapshot.sha256
        rawLogPath = 'H:\synthetic\raw.log'
        rawLogSha256 = [string]$raw.sha256
        timingPath = 'H:\synthetic\timing.csv'
        timingSha256 = [string]$timing.sha256
        host = $hostBinding
    }
    $planSha256 = 'F1' * 32
    $attemptStartValue = [ordered]@{
        schemaVersion = 1
        event = 'attempt-start'
        planSha256 = $planSha256
        entryId = [string]$Receipt.runId
        runNonce = [string]$Receipt.runNonce
        recordedUtc = '2026-09-04T11:10:00.0000000Z'
        sourceBinding = $null
    }
    $attemptStart = New-InstalledTestTextSnapshot `
        ($attemptStartValue | ConvertTo-Json -Depth 20)
    $attemptResultValue = [ordered]@{
        schemaVersion = 1
        event = 'attempt-result'
        planSha256 = $planSha256
        entryId = [string]$Receipt.runId
        runNonce = [string]$Receipt.runNonce
        recordedUtc = '2026-09-04T11:20:00.0000000Z'
        startBinding = [ordered]@{
            path = 'H:\synthetic\attempt.start.json'
            sha256 = [string]$attemptStart.sha256
        }
        state = 'completed'
        failure = $null
        run = $runBinding
        processCleanup = [ordered]@{
            processId = [int]$Receipt.process.id
            exitProof = $true
            blocked = $false
            errors = @()
        }
    }
    $attemptResult = New-InstalledTestTextSnapshot `
        ($attemptResultValue | ConvertTo-Json -Depth 30)
    return [pscustomobject]@{
        receipt = $receiptSnapshot
        raw = $raw
        timing = $timing
        attemptStart = $attemptStart
        attemptResult = $attemptResult
        run = $runBinding
        expected = [pscustomobject]@{
            title = 'ZeroHour'
            sourceCommit = 'a' * 40
            artifactSetSha256 = [string]$Artifact.sha256
            executableSha256 = [string]$Artifact.executableHash
            cohortNonce = '10000000-0000-4000-8000-000000000001'
            cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
            dependencyManifestSha256 =
                [string]$Artifact.runtimeClosure.dependencyManifestSha256
            runtimeClosureSha256 =
                [string]$Artifact.runtimeClosure.closureSha256
            fixtureSha256 = $ReplaySha256
            fixtureSeed = 1729
            runPlanSha256 = $planSha256
        }
        bindings = @{
            receipt = [pscustomobject]@{
                path = 'Runs/test/receipt.json'; sha256 = $receiptSnapshot.sha256 }
            rawLog = [pscustomobject]@{
                path = 'Runs/test/raw.log'; sha256 = $raw.sha256 }
            timing = [pscustomobject]@{
                path = 'Runs/test/timing.csv'; sha256 = $timing.sha256 }
            attemptStart = [pscustomobject]@{
                path = 'Runs/test/attempt.start.json'
                sha256 = $attemptStart.sha256 }
            attemptResult = [pscustomobject]@{
                path = 'Runs/test/attempt.result.json'
                sha256 = $attemptResult.sha256 }
        }
    }
}

function Invoke-InstalledTestProjection {
    param([object]$Inputs, [object]$Module)
    return & $Module {
        param($value)
        ConvertTo-Stage5InstalledKernelRunProjection `
            $value.receipt $value.raw $value.timing $value.attemptStart `
            $value.attemptResult $value.run $value.expected $value.bindings
    } $Inputs
}

function New-InstalledTestNativeOutput {
    param([string]$Title, [string]$MapSha256, [string]$ExecutableSha256,
        [string]$ReplayPath, [string]$ReplaySha256, [int]$ProcessId)
    $nativeNonce = '{0:X8}-{1:X8}-{2:X8}' -f $ProcessId, 0x12345678, 1729
    $epoch = if ($Title -ceq 'Generals') { 1 } else { 3 }
    $marker = if ($Title -ceq 'Generals') {
        ' [GeneralsAIPlanningEpoch=1]'
    } else { ' [SkirmishAIEpoch=3]' }
    $mapKey = 'Maps\Stage5Dense\Stage5Dense.map'
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_START category=native-performance-fixture title={0} map="{1}" seed=1729 frame_budget=3600 map_crc=1234ABCD map_size=16384 map_sha256={2} executable_sha256={3} run_nonce={4}' -f
        $Title, $mapKey, $MapSha256, $ExecutableSha256, $nativeNonce)) |
        Out-Null
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $controller = if ($slot -eq 0) { 'human' } else { 'brutal-ai' }
        $team = if ($slot -lt 4) { 0 } else { 1 }
        $owner = if ($slot -eq 0) { 'teamplayer0' } else {
            'teamSkirmishAmerica{0}' -f $slot
        }
        $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_ROSTER slot={0} controller={1} faction=FactionAmerica faction_index=1 start={0} color={0} team={2} observer=0 map_owner={3} owner_verified=1' -f
            $slot, $controller, $team, $owner)) | Out-Null
    }
    $lines.Add('STAGE5_PERFORMANCE_FIXTURE_OBSERVED first_frame=1 actual_players=8 initial_units=8000 initial_unrostered_units=0') |
        Out-Null
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_PLAYER_UNITS slot={0} initial_units=1000 peak_units=1500' -f
            $slot)) | Out-Null
    }
    $lines.Add(('STAGE5_PERFORMANCE_FIXTURE_COMPLETE category=native-performance-fixture title={0} map="{1}" seed=1729 frame_budget=3600 map_crc=1234ABCD map_size=16384 map_sha256={2} actual_players=8 initial_units=8000 peak_units=12000 initial_unrostered_units=0 peak_unrostered_units=0 observed_first_frame=1 observed_last_frame=1201 observed_frame_samples=1201 winner_team=0 end_frame=1200 final_crc=12345678 replay_epoch={3} ai_epoch_marker="{4}" replay_frame_count=1201 executable_sha256={5} run_nonce={6} replay_sha256={7} retained_replay="{8}"' -f
        $Title, $mapKey, $MapSha256, $epoch, $marker,
        $ExecutableSha256, $nativeNonce, $ReplaySha256, $ReplayPath)) |
        Out-Null
    return $lines.ToArray() -join "`r`n"
}

function New-InstalledTestNativeProduction {
    param([string]$Root, [string]$Title, [object]$Artifact,
        [string]$ExecutablePath, [string]$ExecutableSha256,
        [string]$CohortNonce, [string]$CohortCreatedUtc, [int]$ProcessId)
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $sourceRoot = Join-Path $Root 'source'
    [IO.Directory]::CreateDirectory($sourceRoot) | Out-Null
    $mapPath = Join-Path $sourceRoot 'Stage5Dense.map'
    $bytes = New-Object byte[] 16384
    $salt = if ($Title -ceq 'Generals') { 17 } else { 29 }
    for ($index = 0; $index -lt $bytes.Length; ++$index) {
        $bytes[$index] = [byte](($index * 31 + $salt) % 251)
    }
    [IO.File]::WriteAllBytes($mapPath, $bytes)
    $mapSha256 = Get-Sha256 $mapPath
    $reviewedManifest = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-reviewed-native-kernel-fixture'
        reviewStatus = 'approved'
        reviewScope = 'native-dense-eight-player-kernel-execution-v1'
        reviewedUtc = '2026-09-04T10:00:00.0000000Z'
        reviewedBy = 'stage5-premium-review'
        title = $Title
        sourceCommit = 'a' * 40
        artifactSetSha256 = [string]$Artifact.sha256
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 =
                [string]$Artifact.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$Artifact.runtimeClosure.closureSha256
        }
        executableSha256 = $ExecutableSha256
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
    $reviewedManifestPath = Join-Path $sourceRoot 'ReviewedFixture.json'
    Write-Json $reviewedManifestPath $reviewedManifest
    $reviewedManifestSha256 = Get-Sha256 $reviewedManifestPath
    $reviewed = Read-Stage5ReviewedNativeKernelFixture `
        -Path $reviewedManifestPath -ExpectedSha256 $reviewedManifestSha256 `
        -ExpectedTitle $Title -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 $Artifact.sha256 `
        -ExpectedExecutableSha256 $ExecutableSha256 `
        -ExpectedDependencyManifestSha256 `
            $Artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $Artifact.runtimeClosure.closureSha256
    $argumentString = Get-Stage5NativePerformanceFixtureArgumentString `
        -MapKey $reviewed.fixture.mapKey -Seed 1729 -FrameBudget 3600 `
        -ExecutableSha256 $ExecutableSha256

    $bundleRoot = Join-Path $Root 'bundle'
    foreach ($relative in @('inputs', 'logs', 'replays', 'prelaunch')) {
        [IO.Directory]::CreateDirectory((Join-Path $bundleRoot $relative)) |
            Out-Null
    }
    $profileRoot = Join-Path $bundleRoot 'TitleSession\Documents\Stage5 Test Data'
    [IO.Directory]::CreateDirectory($profileRoot) | Out-Null
    $nativeNonce = '{0:X8}-{1:X8}-{2:X8}' -f $ProcessId, 0x12345678, 1729
    $replayPath = Join-Path $profileRoot `
        "Stage5Performance-1729-$nativeNonce.rep"
    [IO.File]::WriteAllText($replayPath,
        "synthetic $Title retained replay; contract test only")
    $replaySha256 = Get-Sha256 $replayPath
    $output = New-InstalledTestNativeOutput $Title $mapSha256 `
        $ExecutableSha256 $replayPath $replaySha256 $ProcessId
    $completion = ConvertFrom-Stage5NativePerformanceFixtureOutput `
        -Text $output -ReviewedFixture $reviewed -ExpectedProcessId $ProcessId `
        -ProfileRoot $profileRoot
    Copy-Item -LiteralPath $reviewedManifestPath `
        -Destination (Join-Path $bundleRoot 'inputs\ReviewedFixture.json')
    Copy-Item -LiteralPath $mapPath `
        -Destination (Join-Path $bundleRoot 'inputs\Stage5Dense.map')
    [IO.File]::WriteAllText((Join-Path $bundleRoot 'logs\fixture-output.log'),
        $output, (New-Object Text.UTF8Encoding($false)))
    Copy-Item -LiteralPath $replayPath `
        -Destination (Join-Path $bundleRoot 'replays\Stage5Performance.rep')
    $hostRunNonce = if ($Title -ceq 'Generals') {
        '20000000-0000-4000-8000-000000000001'
    } else { '20000000-0000-4000-8000-000000000002' }
    $planPath = Join-Path $bundleRoot 'prelaunch\fixture-plan.json'
    $plan = [ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-plan'
        recordedUtc = '2026-09-04T11:05:00.0000000Z'
        hostRunNonce = $hostRunNonce
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        title = $Title
        sourceCommit = 'a' * 40
        artifactSetSha256 = [string]$Artifact.sha256
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 =
                [string]$Artifact.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$Artifact.runtimeClosure.closureSha256
        }
        executablePath = $ExecutablePath
        executableSha256 = $ExecutableSha256
        reviewedFixtureManifestPath = $reviewedManifestPath
        reviewedFixtureManifestSha256 = $reviewedManifestSha256
        mapSourcePath = $mapPath
        mapSha256 = $mapSha256
        mapDestinationPath = Join-Path $profileRoot `
            'Maps\Stage5Dense\Stage5Dense.map'
        argumentString = $argumentString
        taskRoot = $bundleRoot
        timeoutSeconds = 7200
        workerCount = 4
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
    }
    Write-Json $planPath $plan
    $planSha256 = Get-Sha256 $planPath
    $startPath = Join-Path $bundleRoot 'prelaunch\fixture-start.json'
    Write-Json $startPath ([ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-start'
        recordedUtc = '2026-09-04T11:10:00.0000000Z'
        plan = [ordered]@{ path = $planPath; sha256 = $planSha256 }
        hostRunNonce = $hostRunNonce
    })
    $rawPath = Join-Path $bundleRoot 'logs\fixture-output.log'
    $retainedPath = Join-Path $bundleRoot 'replays\Stage5Performance.rep'
    $receipt = New-Stage5NativePerformanceFixtureProductionReceipt `
        -Title $Title -RecordedUtc '2026-09-04T11:30:00.0000000Z' `
        -CohortNonce $CohortNonce -CohortCreatedUtc $CohortCreatedUtc `
        -SourceCommit ('a' * 40) -ArtifactSetSha256 $Artifact.sha256 `
        -DependencyManifestSha256 `
            $Artifact.runtimeClosure.dependencyManifestSha256 `
        -RuntimeClosureSha256 $Artifact.runtimeClosure.closureSha256 `
        -ReviewedFixtureManifestPath 'inputs\ReviewedFixture.json' `
        -ReviewedFixtureManifestSha256 $reviewedManifestSha256 `
        -ExecutableSha256 $ExecutableSha256 -HostRunNonce $hostRunNonce `
        -ProcessId $ProcessId -ProcessCreationTimeUtc100ns `
            ([Int64](133000000000000000 + $ProcessId)) `
        -CommandLine ($ExecutablePath + ' ' + $argumentString) `
        -ArgumentString $argumentString -NativeProfileRoot $profileRoot `
        -TaskRoot $bundleRoot -Completion $completion `
        -RawLogPath 'logs\fixture-output.log' `
        -RawLogSha256 (Get-Sha256 $rawPath) `
        -RetainedReplayPath 'replays\Stage5Performance.rep' `
        -PrelaunchPlanPath 'prelaunch\fixture-plan.json' `
        -PrelaunchPlanSha256 $planSha256 `
        -AttemptStartPath 'prelaunch\fixture-start.json' `
        -AttemptStartSha256 (Get-Sha256 $startPath) `
        -Lifecycle ([pscustomobject]@{
            mutexAcquired = $true
            noInstalledTitleProcessesAtPreflight = $true
            childExitProven = $true
            registryRestored = $true
            profileRemoved = $true
            recoveryJournalAbsent = $true
        })
    $receiptPath = Join-Path $bundleRoot `
        'Stage5NativePerformanceFixture.json'
    Write-Json $receiptPath $receipt
    return Read-Stage5NativePerformanceFixtureProductionReceipt `
        -Path $receiptPath -ExpectedSha256 (Get-Sha256 $receiptPath) `
        -ExpectedTitle $Title -ExpectedCohortNonce $CohortNonce `
        -ExpectedCohortCreatedUtc $CohortCreatedUtc `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 $Artifact.sha256 `
        -ExpectedExecutableSha256 $ExecutableSha256 `
        -ExpectedDependencyManifestSha256 `
            $Artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $Artifact.runtimeClosure.closureSha256
}

function ConvertTo-InstalledTestHostRun {
    param([object]$Run)
    $receipt = Read-TestJson $Run.receiptPath
    return [pscustomobject][ordered]@{
        fixtureId = [string]$Run.fixtureId
        lane = [string]$Run.lane
        ordinal = [int]$Run.ordinal
        warmup = [bool]$Run.warmup
        runId = [string]$Run.runId
        expectedArgumentString = [string]$Run.expectedArgumentString
        processId = [int]$Run.host.processId
        processCreationTimeUtc100ns = [Int64]$Run.host.creationTimeUtc100ns
        elapsedMilliseconds = [double]$Run.host.elapsedMilliseconds
        receiptPath = [string]$Run.receiptPath
        receiptSha256 = [string]$Run.receiptSha256
        rawLogPath = [string]$receipt.rawEvidence.rawLogPath
        rawLogSha256 = [string]$receipt.rawEvidence.rawLogSha256
        timingPath = [string]$receipt.rawEvidence.timingPath
        timingSha256 = [string]$receipt.rawEvidence.timingSha256
        receiptBinding = [pscustomobject][ordered]@{
            path = [string]$Run.receiptPath
            sha256 = [string]$Run.receiptSha256
            runId = [string]$Run.runId
            runNonce = [string]$Run.runNonce
            cohortNonce = [string]$receipt.cohortNonce
            processId = [int]$Run.host.processId
            processCreationTimeUtc100ns = [Int64]$Run.host.creationTimeUtc100ns
            executablePath = [string]$Run.host.executablePath
            executableSha256 = [string]$Run.host.executableSha256
            commandLine = [string]$Run.host.commandLine
            rawLogPath = [string]$receipt.rawEvidence.rawLogPath
            rawLogSha256 = [string]$receipt.rawEvidence.rawLogSha256
            timingPath = [string]$receipt.rawEvidence.timingPath
            timingSha256 = [string]$receipt.rawEvidence.timingSha256
        }
        selectedWorkerCpuSetIds = @(1, 2, 3, 4)
        selectedPhysicalCoreMask = '000000000000000F'
    }
}

function New-InstalledTestHostEvidence {
    param([string]$Root, [string]$Title, [object]$Artifact,
        [string]$ExecutablePath, [string]$ExecutableSha256,
        [object]$Production, [string]$CohortNonce,
        [string]$CohortCreatedUtc, [int]$ProcessBase)
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $runs = New-Object 'Collections.Generic.List[object]'
    $validatedRuns = New-Object 'Collections.Generic.List[object]'
    for ($ordinal = 0; $ordinal -lt 4; ++$ordinal) {
        $run = Write-RunReceipt $Root $ExecutablePath 'dense-eight-player' `
            $Production.fixture.path 'physical-4' 4 $ordinal `
            ($ProcessBase + $ordinal) 40.0 12000 $ExecutableSha256 `
            $Artifact.sha256 $Production.fixture.sha256 `
            $Artifact.runtimeClosure ('a' * 40) $CohortNonce `
            $CohortCreatedUtc $true
        $receipt = Read-TestJson $run.receiptPath
        $receipt.title = $Title
        $receipt.fixture.seed = 1729
        $receipt.recordedUtc = '2026-09-04T11:30:00.0000000Z'
        Write-Json $run.receiptPath $receipt
        $run.receiptSha256 = Get-Sha256 $run.receiptPath
        $runs.Add([pscustomobject]$run) | Out-Null
        $validatedRuns.Add((ConvertTo-InstalledTestHostRun $run)) | Out-Null
    }
    $fixture = [pscustomobject][ordered]@{
        id = 'dense-eight-player'
        path = [string]$Production.fixture.path
        sha256 = [string]$Production.fixture.sha256
        seed = 1729
        playerCount = 8
        peakUnitCount = [int]$Production.fixture.peakUnitCount
    }
    $cpuSets = @(0..11 | ForEach-Object {
        [pscustomobject][ordered]@{
            id = $_ + 1
            efficiencyClass = 0
            group = 0
            # Match the native receipt factory's selected CPU-set identities.
            # The synthetic host count remains bounded at 6/12; these records
            # are contract data and never schedule work.
            coreIndex = $_
            logicalProcessorIndex = $_
            parked = $false
            allocated = $false
            available = $true
        }
    })
    $validation = [pscustomobject][ordered]@{
        schemaVersion = 1
        title = $Title
        qualificationMode = 'InstalledKernelExecution'
        sourceCommit = 'a' * 40
        stage3SourceCommit = ''
        artifactSetSha256 = [string]$Artifact.sha256
        artifactSetManifestPath = [string]$Artifact.path
        runtimeClosure = [pscustomobject][ordered]@{
            dependencyManifestSha256 =
                [string]$Artifact.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$Artifact.runtimeClosure.closureSha256
        }
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        executablePath = $ExecutablePath
        executableSha256 = $ExecutableSha256
        fixtureManifestSha256 = [string]$Production.sha256
        stage3BaselineSha256 = ''
        taskRoot = $Root
        warmupRuns = 1
        measuredRuns = 3
        fixtures = @($fixture)
        stage3Fixtures = @()
        topology = [pscustomobject][ordered]@{
            source = 'GetSystemCpuSetInformation'
            physicalCoreCount = 6
            logicalProcessorCount = 12
            cpuSets = $cpuSets
        }
        runs = $runs.ToArray()
        referencePolicy = 'throughput-only'
        pairedOracleBindings = @()
        fixtureManifestPath = [string]$Production.path
        fixtureProductionReceipt = [pscustomobject][ordered]@{
            path = [string]$Production.path
            sha256 = [string]$Production.sha256
        }
    }
    $validationPath = Join-Path $Root `
        'Stage5InstalledKernelExecutionValidationManifest.json'
    Write-Json $validationPath $validation
    $runner = Join-Path $PSScriptRoot `
        'Invoke-Stage5PerformanceScalingValidation.ps1'
    & $runner -SelfTestValidationManifestPath $validationPath | Out-Null

    $attemptRoot = Join-Path $Root 'attempts'
    [IO.Directory]::CreateDirectory($attemptRoot) | Out-Null
    $entries = New-Object 'Collections.Generic.List[object]'
    foreach ($run in $runs) {
        $entryRoot = Join-Path $Root ([string]$run.runId)
        $entries.Add([pscustomobject][ordered]@{
            entryId = [string]$run.runId
            measurementRole = 'throughput'
            profileId = $null
            fixtureId = 'dense-eight-player'
            lane = 'physical-4'
            ordinal = [int]$run.ordinal
            warmup = [bool]$run.warmup
            sourceEntryId = $null
            runNonce = [string]$run.runNonce
            workerCount = 4
            expectedArgumentString = [string]$run.expectedArgumentString
            outputPaths = [pscustomobject][ordered]@{
                runRoot = $entryRoot
                receiptDirectory = Join-Path $entryRoot 'receipt'
                rawLogPath = Join-Path $entryRoot 'game-owned-raw.log'
                timingDirectory = Join-Path $entryRoot 'timing'
                stdoutPath = Join-Path $entryRoot 'host-stdout.log'
                stderrPath = Join-Path $entryRoot 'host-stderr.log'
                tempDirectory = Join-Path $entryRoot 'temp'
                attemptTracePath = $null
                attemptStartPath = Join-Path $attemptRoot `
                    ($run.runId + '.start.json')
                attemptResultPath = Join-Path $attemptRoot `
                    ($run.runId + '.result.json')
                sourceBindingPath = $null
            }
        }) | Out-Null
    }
    $planPath = Join-Path $Root 'phase-plan.json'
    $plan = [pscustomobject][ordered]@{
        schemaVersion = 1
        title = $Title
        qualificationMode = 'InstalledKernelExecution'
        taskRoot = $Root
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        sourceCommit = 'a' * 40
        executablePath = $ExecutablePath
        executableSha256 = $ExecutableSha256
        artifactSetSha256 = [string]$Artifact.sha256
        runtimeClosure = $validation.runtimeClosure
        fixtureManifestPath = [string]$Production.path
        fixtureManifestSha256 = [string]$Production.sha256
        fixtures = @($fixture)
        referencePolicy = 'throughput-only'
        warmupRuns = 1
        measuredRuns = 3
        timeoutSeconds = 7200
        titleSessionEnvironment = [pscustomobject]@{}
        phaseBaselineProfiles = @()
        outputFilePolicy = 'native-runid-pid-receipt-pid-tick-timing-v1'
        entries = $entries.ToArray()
    }
    Write-Json $planPath $plan
    $planSha256 = Get-Sha256 $planPath
    $outcomes = New-Object 'Collections.Generic.List[object]'
    for ($index = 0; $index -lt $runs.Count; ++$index) {
        $run = $runs[$index]
        $entry = $entries[$index]
        $startPath = [string]$entry.outputPaths.attemptStartPath
        Write-Json $startPath ([pscustomobject][ordered]@{
            schemaVersion = 1
            event = 'attempt-start'
            planSha256 = $planSha256
            entryId = [string]$entry.entryId
            runNonce = [string]$entry.runNonce
            recordedUtc = '2026-09-04T11:10:00.0000000Z'
            sourceBinding = $null
        })
        $startBinding = [pscustomobject][ordered]@{
            path = $startPath
            sha256 = Get-Sha256 $startPath
        }
        $resultPath = [string]$entry.outputPaths.attemptResultPath
        Write-Json $resultPath ([pscustomobject][ordered]@{
            schemaVersion = 1
            event = 'attempt-result'
            planSha256 = $planSha256
            entryId = [string]$entry.entryId
            runNonce = [string]$entry.runNonce
            recordedUtc = '2026-09-04T11:20:00.0000000Z'
            startBinding = $startBinding
            state = 'completed'
            failure = $null
            run = $run
            processCleanup = [pscustomobject][ordered]@{
                processId = [int]$run.host.processId
                exitProof = $true
                blocked = $false
                errors = @()
            }
        })
        $outcomes.Add([pscustomobject][ordered]@{
            entryId = [string]$entry.entryId
            state = 'completed'
            failure = $null
            startBinding = $startBinding
            resultBinding = [pscustomobject][ordered]@{
                path = $resultPath
                sha256 = Get-Sha256 $resultPath
            }
        }) | Out-Null
    }
    $attemptManifestPath = Join-Path $Root 'phase-attempts.json'
    Write-Json $attemptManifestPath ([pscustomobject][ordered]@{
        schemaVersion = 1
        planSha256 = $planSha256
        outcomes = $outcomes.ToArray()
        cohortFailure = $null
    })
    $validationSha256 = Get-Sha256 $validationPath
    $attemptManifestSha256 = Get-Sha256 $attemptManifestPath
    $hostDocument = [pscustomobject][ordered]@{
        schemaVersion = 2
        evidenceKind = 'stage5-installed-kernel-execution-host'
        producer = 'Invoke-Stage5PerformanceScalingValidation.ps1'
        status = 'passed'
        recordedUtc = '2026-09-04T11:40:00.0000000Z'
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        qualificationMode = 'InstalledKernelExecution'
        qualificationClass = 'installed-kernel-execution-only'
        measurementMode = 'headless-throughput'
        referencePolicy = 'throughput-only'
        installedRuntime = $true
        sourceCommit = 'a' * 40
        artifactSetSha256 = [string]$Artifact.sha256
        artifactSetManifest = [pscustomobject][ordered]@{
            path = [string]$Artifact.path; sha256 = [string]$Artifact.sha256 }
        runtimeClosure = $validation.runtimeClosure
        title = $Title
        executable = [pscustomobject][ordered]@{
            path = $ExecutablePath; sha256 = $ExecutableSha256 }
        fixtureManifest = [pscustomobject][ordered]@{
            path = [string]$Production.path; sha256 = [string]$Production.sha256 }
        stage3Baseline = $null
        launcher = [pscustomobject]@{}
        profileStrategy = 'process-local-validation-profile-root'
        registryViews = @('Registry32', 'Registry64')
        environmentVariables = @(
            'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA', 'USERPROFILE',
            'HOMEDRIVE', 'HOMEPATH', 'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
            'RTS_STAGE5_VALIDATION_CACHE_ROOT', 'RTS_STAGE5_VALIDATION_LOG_ROOT',
            'RTS_STAGE5_VALIDATION_DUMP_ROOT',
            'RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT')
        profileConcurrency = 'shared-title-profile-read-only'
        validationConcurrency = 'exclusive'
        titleSessionProfile = [pscustomobject]@{}
        schedule = [pscustomobject][ordered]@{
            warmupRuns = 1; measuredRuns = 3; lanes = @('physical-4') }
        topology = $validation.topology
        thresholds = $null
        nativeReceiptBindings = @($validatedRuns | ForEach-Object {
            $_.receiptBinding })
        pairedOracleBindings = @()
        fixtures = @([pscustomobject][ordered]@{
            id = 'dense-eight-player'; playerCount = 8
            peakUnitCount = 12000; measuredRuns = 3
            laneMedians = [pscustomobject]@{ 'physical-4' = 40.0 }
            qualificationClass = 'installed-kernel-execution-only' })
        runs = $validatedRuns.ToArray()
        acceptanceScope = 'kernel-execution-only'
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
        fixtureProductionReceipt = [pscustomobject][ordered]@{
            path = [string]$Production.path; sha256 = [string]$Production.sha256 }
        validationManifest = [pscustomobject][ordered]@{
            path = $validationPath; sha256 = $validationSha256 }
        runPlan = [pscustomobject][ordered]@{
            path = $planPath; sha256 = $planSha256 }
        attemptManifest = [pscustomobject][ordered]@{
            path = $attemptManifestPath; sha256 = $attemptManifestSha256 }
    }
    $hostPath = Join-Path $Root 'Stage5InstalledKernelExecutionHost.json'
    Write-Json $hostPath $hostDocument
    return [pscustomobject]@{
        path = $hostPath
        sha256 = Get-Sha256 $hostPath
        validationPath = $validationPath
        planPath = $planPath
        attemptManifestPath = $attemptManifestPath
    }
}

$baseModulePath = Join-Path $PSScriptRoot `
    'DeterministicSimulationEvidence.psm1'
$nativeModulePath = Join-Path $PSScriptRoot `
    'Stage5NativePerformanceFixtureProduction.psm1'
$modulePath = Join-Path $PSScriptRoot `
    'Stage5InstalledKernelExecutionEvidence.psm1'
$scalingPath = Join-Path $PSScriptRoot `
    'Invoke-Stage5PerformanceScalingValidation.ps1'
$wrapperPath = Join-Path $PSScriptRoot `
    'New-Stage5InstalledKernelExecutionEvidence.ps1'
Assert-InstalledTestCondition (Test-Path -LiteralPath $baseModulePath `
    -PathType Leaf) 'The base evidence module must exist.'
Assert-InstalledTestCondition (Test-Path -LiteralPath $nativeModulePath `
    -PathType Leaf) 'The native fixture-production module must exist.'
Assert-InstalledTestCondition (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    'The installed-kernel evidence module must exist.'
Assert-InstalledTestCondition (Test-Path -LiteralPath $scalingPath -PathType Leaf) `
    'The performance-scaling runner must exist.'
Assert-InstalledTestCondition (Test-Path -LiteralPath $wrapperPath -PathType Leaf) `
    'The installed-kernel evidence producer wrapper must exist.'

$evidenceModuleNames = @('Stage5InstalledKernelExecutionEvidence',
    'Stage5NativePerformanceFixtureProduction', 'DeterministicSimulationEvidence',
    'Stage5ValidationProfileCapability', 'Stage5RegistryRecovery')
foreach ($name in $evidenceModuleNames) {
    Remove-Module -Name $name -Force -ErrorAction SilentlyContinue
}
Import-Module $baseModulePath -Force
Assert-InstalledTestCondition ($null -ne
    (Get-Command Invoke-Stage5FinalAcceptanceAggregation `
        -ErrorAction SilentlyContinue)) `
    'The base acceptance command must load before the kernel module.'
Import-Module $modulePath -Force
Assert-InstalledTestCondition ($null -ne
    (Get-Command Invoke-Stage5FinalAcceptanceAggregation `
        -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command Read-Stage5InstalledKernelExecutionEvidence `
        -ErrorAction SilentlyContinue)) `
    'Base-to-kernel import order must preserve both command sets.'

foreach ($name in $evidenceModuleNames) {
    Remove-Module -Name $name -Force -ErrorAction SilentlyContinue
}
Import-Module $modulePath -Force
Import-Module $baseModulePath -Force
Assert-InstalledTestCondition ($null -ne
    (Get-Command Invoke-Stage5FinalAcceptanceAggregation `
        -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command Read-Stage5InstalledKernelExecutionEvidence `
        -ErrorAction SilentlyContinue)) `
    'Kernel-to-base import order must preserve both command sets.'
Import-Module $nativeModulePath
Assert-InstalledTestCondition ($null -ne
    (Get-Command Invoke-Stage5FinalAcceptanceAggregation `
        -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command Read-Stage5InstalledKernelExecutionEvidence `
        -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command Read-Stage5NativePerformanceFixtureProductionReceipt `
        -ErrorAction SilentlyContinue)) `
    'A non-forced native-module import must preserve base and kernel commands.'
$module = Get-Module -Name Stage5InstalledKernelExecutionEvidence
Assert-InstalledTestCondition ($null -ne $module -and
    $null -ne (Get-Command New-Stage5InstalledKernelExecutionEvidence) -and
    $null -ne (Get-Command Read-Stage5InstalledKernelExecutionEvidence)) `
    'The installed-kernel producer and independent reader must be exported.'

if ($ModuleScopePreflightOnly) {
    # Keep this selector short and side-effect free: it exercises the actual
    # module graph and reader against a deliberately missing input, then
    # returns before the full synthetic evidence fixture is created.
    $scalingSource = Get-Content -LiteralPath $scalingPath -Raw
    $scalingImportBlockEnd = $scalingSource.IndexOf(
        '$script:Stage5RunnerScriptSha256', [StringComparison]::Ordinal)
    Assert-InstalledTestCondition ($scalingImportBlockEnd -gt 0) `
        'Scaling import block could not be located for the module-scope preflight.'
    $scalingImportBlock = $scalingSource.Substring(0, $scalingImportBlockEnd)
    $expectedScalingImports = @(
        'DeterministicSimulationEvidence.psm1',
        'Stage5NativePerformanceFixtureProduction.psm1',
        'Stage5ValidationProfileCapability.psm1',
        'Stage5RegistryRecovery.psm1')
    $previousImportOffset = -1
    foreach ($importName in $expectedScalingImports) {
        $importOffset = $scalingImportBlock.IndexOf($importName,
            $previousImportOffset + 1, [StringComparison]::Ordinal)
        Assert-InstalledTestCondition ($importOffset -gt $previousImportOffset) `
            "Scaling imports are missing or out of order at '$importName'."
        $previousImportOffset = $importOffset
    }
    Assert-InstalledTestCondition ($scalingImportBlock -notmatch '-Force') `
        'Scaling top-level evidence imports must not force-reload caller modules.'

    foreach ($name in $evidenceModuleNames) {
        Remove-Module -Name $name -Force -ErrorAction SilentlyContinue
    }
    # Execute the same non-forced top-level import order as the production
    # scaling runner and exercise both caller and native-module commands.
    Import-Module $baseModulePath
    Import-Module $nativeModulePath
    Import-Module (Join-Path $PSScriptRoot 'Stage5ValidationProfileCapability.psm1')
    Import-Module (Join-Path $PSScriptRoot 'Stage5RegistryRecovery.psm1')
    $scalingJsonCommand = Get-Command ConvertFrom-Stage5JsonDictionary `
        -CommandType Function -ErrorAction SilentlyContinue
    Assert-InstalledTestCondition ($null -ne $scalingJsonCommand) `
        'Scaling import order did not expose the caller JSON parser.'
    $scalingSchemaPath = Join-Path $PSScriptRoot `
        'Stage5InstalledKernelExecution.schema.json'
    $scalingSchema = & $scalingJsonCommand $scalingSchemaPath
    Assert-InstalledTestCondition ($scalingSchema -is [Collections.IDictionary]) `
        'Scaling caller JSON parser did not parse the real installed-kernel schema.'
    $scalingNativeModule = @(Get-Module `
        -Name Stage5NativePerformanceFixtureProduction | Select-Object -First 1)[0]
    Assert-InstalledTestCondition ($null -ne $scalingNativeModule) `
        'Scaling import order did not load the native fixture module.'
    Invoke-InstalledTestNativeReaderMissingPathProbe $scalingNativeModule `
        'Scaling Evidence-to-Native-to-Capability-to-Recovery order'

    foreach ($name in $evidenceModuleNames) {
        Remove-Module -Name $name -Force -ErrorAction SilentlyContinue
    }
    # Reproduce the problematic nesting order: kernel imports its private
    # native/base dependencies, then the caller reloads those modules.
    Import-Module $modulePath -Force
    Import-Module $nativeModulePath -Force
    Import-Module $baseModulePath -Force
    $forcedKernelModule = @(Get-Module -Name Stage5InstalledKernelExecutionEvidence |
        Select-Object -First 1)[0]
    $forcedBaseModule = @(Get-Module -Name DeterministicSimulationEvidence |
        Select-Object -First 1)[0]
    Assert-InstalledTestCondition ($null -ne $forcedKernelModule -and
        $null -ne $forcedBaseModule) `
        'Forced module-scope preflight did not load the kernel/base modules.'
    Invoke-InstalledTestNativeReaderMissingPathProbe $forcedKernelModule `
        'Kernel-to-native-to-base reload order'
    Invoke-InstalledTestPrivateGroupingProbe $forcedBaseModule `
        'Kernel-to-native-to-base reload order'

    foreach ($name in $evidenceModuleNames) {
        Remove-Module -Name $name -Force -ErrorAction SilentlyContinue
    }
    # Preserve the ordinary caller order as an independent control path.
    Import-Module $baseModulePath -Force
    Import-Module $nativeModulePath -Force
    Import-Module $modulePath -Force
    $normalKernelModule = @(Get-Module -Name Stage5InstalledKernelExecutionEvidence |
        Select-Object -First 1)[0]
    $normalBaseModule = @(Get-Module -Name DeterministicSimulationEvidence |
        Select-Object -First 1)[0]
    Assert-InstalledTestCondition ($null -ne $normalKernelModule -and
        $null -ne $normalBaseModule) `
        'Normal module-scope preflight did not load the kernel/base modules.'
    Invoke-InstalledTestNativeReaderMissingPathProbe $normalKernelModule `
        'Normal base-to-native-to-kernel order'
    Invoke-InstalledTestPrivateGroupingProbe $normalBaseModule `
        'Normal base-to-native-to-kernel order'
    Write-Output 'Stage 5 installed-kernel module-scope preflight passed.'
    return
}

# Reuse the performance validator's reviewed synthetic native-receipt factory.
# SourceContractPreflightOnly never launches a title, builds code, or publishes
# authority. It also keeps this test aligned with the runner's V5 contract.
. (Join-Path $PSScriptRoot 'Stage5PerformanceScalingValidation.Tests.ps1') `
    -SourceContractPreflightOnly | Out-Null

$ownsRoot = [string]::IsNullOrWhiteSpace($ScratchRoot)
$testRoot = if ($ownsRoot) {
    Join-Path 'H:\Stage5PerformanceScalingValidationScratch' `
        ('ike-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
} else { [IO.Path]::GetFullPath($ScratchRoot) }

try {
    [IO.Directory]::CreateDirectory($testRoot) | Out-Null
    $artifact = New-ArtifactSetFixture (Join-Path $testRoot 'artifact') ('a' * 40)
    $replayPath = Join-Path $testRoot 'dense-eight-player.rep'
    [IO.File]::WriteAllText($replayPath,
        'synthetic dense replay bytes used only by the projector unit test')
    $replaySha256 = Get-Sha256 $replayPath
    $run = Write-RunReceipt (Join-Path $testRoot 'run') `
        $artifact.executablePath 'dense-eight-player' $replayPath 'physical-4' `
        4 1 28001 40.0 8000 $artifact.executableHash $artifact.sha256 `
        $replaySha256 $artifact.runtimeClosure ('a' * 40) `
        '10000000-0000-4000-8000-000000000001' `
        '2026-09-04T11:00:00.0000000Z' $true
    $receipt = Read-TestJson $run.receiptPath
    $receipt.cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
    $receipt.recordedUtc = '2026-09-04T11:30:00.0000000Z'
    $receipt.fixture.seed = 1729
    $receipt.fixture.requestedMinimumUnitCount = 8000
    $validInputs = New-InstalledTestProjectionInputs $receipt $run $artifact `
        $replaySha256
    $projection = Invoke-InstalledTestProjection $validInputs $module
    Assert-InstalledTestCondition (
        @($projection.projection.kernels).Count -eq 6 -and
        @($projection.projection.kernels | Where-Object {
            [Int64]$_.worker.physicalWorkerJobs -gt 0 -and
            @($_.streams).Count -gt 0 -and
            $_.streams[0].commitSha256 -cmatch '^[0-9A-F]{64}$'
        }).Count -eq 6 -and
        $projection.projection.worker.requestedCount -eq 4 -and
        $projection.projection.scheduler.serialFallbackCount -eq 0) `
        'Valid synthetic native receipts must project six physical-worker owner-commit proofs.'

    $projectionTypeCases = @(
        [pscustomobject]@{ name = 'producer'; good = $receipt.producer
            property = 'producer'
            edit = { param($r, $value) $r.producer = $value } }
        [pscustomobject]@{ name = 'sourceCommit'; good = $receipt.sourceCommit
            property = 'sourceCommit'
            edit = { param($r, $value) $r.sourceCommit = $value } }
        [pscustomobject]@{ name = 'scheduler submittedJobCount'; good = 1
            property = 'submittedJobCount'
            edit = { param($r, $value) $r.schedulerMetrics.submittedJobCount = $value } }
        [pscustomobject]@{ name = 'kernel attemptedBatches'; good = 1
            property = 'attemptedBatches'
            edit = { param($r, $value) $r.kernelTiming.streams[0].attemptedBatches = $value } }
        [pscustomobject]@{ name = 'schedulerStarted'; good = $true
            property = 'schedulerStarted'
            edit = { param($r, $value) $r.schedulerStarted = $value } }
        [pscustomobject]@{ name = 'process identityAvailable'; good = $true
            property = 'identityAvailable'
            edit = { param($r, $value) $r.process.identityAvailable = $value } }
    )
    foreach ($case in $projectionTypeCases) {
        foreach ($mutation in Get-InstalledTestScalarMutations $case.good) {
            $changedReceipt = Copy-InstalledTestValue $receipt
            & $case.edit $changedReceipt $mutation.value
            $changedInputs = New-InstalledTestProjectionInputs $changedReceipt $run `
                $artifact $replaySha256
            if ($null -ne $mutation.PSObject.Properties['literal']) {
                $oldReceiptSha256 = [string]$changedInputs.run.receiptSha256
                Set-InstalledTestSnapshotJsonPropertyLiteral `
                    $changedInputs.receipt $case.property $mutation.literal
                $changedInputs.run.receiptSha256 = $changedInputs.receipt.sha256
                $changedInputs.bindings.receipt.sha256 =
                    $changedInputs.receipt.sha256
                $attemptJson = [Text.Encoding]::UTF8.GetString(
                    [byte[]]$changedInputs.attemptResult.bytes)
                $attemptJson = $attemptJson.Replace($oldReceiptSha256,
                    [string]$changedInputs.receipt.sha256)
                $changedInputs.attemptResult.bytes =
                    (New-Object Text.UTF8Encoding($false)).GetBytes($attemptJson)
                $changedInputs.attemptResult.length =
                    [Int64]$changedInputs.attemptResult.bytes.Length
                $changedInputs.attemptResult.sha256 =
                    Get-InstalledTestSha256Bytes $changedInputs.attemptResult.bytes
                $changedInputs.bindings.attemptResult.sha256 =
                    $changedInputs.attemptResult.sha256
            }
            $caught = $null
            try { Invoke-InstalledTestProjection $changedInputs $module | Out-Null }
            catch { $caught = $_ }
            Assert-InstalledTestCondition ($null -ne $caught -and
                $caught.Exception.Message -match '(?i)string|integer|boolean|scalar|invalid|worker|scheduler|receipt') `
                "The projector must reject $($case.name) $($mutation.name)."
        }
    }

    $cases = @(
        [pscustomobject]@{ name = 'worker count'; pattern = '(?i)physical-4'
            edit = { param($r) $r.worker.requestedCount = 3 } },
        [pscustomobject]@{ name = 'scheduler fallback'; pattern = '(?i)scheduler'
            edit = { param($r) $r.schedulerMetrics.serialFallbackCount = 1 } },
        [pscustomobject]@{ name = 'dense workload'; pattern = '(?i)dense'
            edit = { param($r) $r.workload.minimumUnitCount = 7999 } },
        [pscustomobject]@{ name = 'physical worker execution'; pattern = '(?i)physical-worker'
            edit = { param($r) $r.kernels[0].physicalWorkerJobs = 0
                $r.kernels[0].ownerHelpedJobs = 4 } },
        [pscustomobject]@{ name = 'owner commit'; pattern = '(?i)committed owner'
            edit = { param($r)
                $r.kernelTiming.streams[0].committedBatches = 0 } })
    foreach ($case in $cases) {
        $changedReceipt = Copy-InstalledTestValue $receipt
        & $case.edit $changedReceipt
        $changedInputs = New-InstalledTestProjectionInputs $changedReceipt $run `
            $artifact $replaySha256
        $caught = $null
        try { Invoke-InstalledTestProjection $changedInputs $module | Out-Null }
        catch { $caught = $_ }
        Assert-InstalledTestCondition ($null -ne $caught -and
            $caught.Exception.Message -match $case.pattern) `
            "The projector must reject changed $($case.name) evidence."
    }

    $artifactRoot = Split-Path -Parent $artifact.path
    $generalsExecutable = Join-Path $artifactRoot `
        'artifacts\Generals\generalsv.exe'
    $generalsExecutableSha256 = Get-Sha256 $generalsExecutable
    $zeroHourExecutable = [string]$artifact.executablePath
    $zeroHourExecutableSha256 = [string]$artifact.executableHash
    $cohortNonce = '10000000-0000-4000-8000-000000000001'
    $cohortCreatedUtc = '2026-09-04T11:00:00.0000000Z'
    $generalsProduction = New-InstalledTestNativeProduction `
        (Join-Path $testRoot 'generals-production') Generals $artifact `
        $generalsExecutable $generalsExecutableSha256 $cohortNonce `
        $cohortCreatedUtc 29001
    $zeroHourProduction = New-InstalledTestNativeProduction `
        (Join-Path $testRoot 'zerohour-production') ZeroHour $artifact `
        $zeroHourExecutable $zeroHourExecutableSha256 $cohortNonce `
        $cohortCreatedUtc 29002
    $generalsHost = New-InstalledTestHostEvidence `
        (Join-Path $testRoot 'generals-host') Generals $artifact `
        $generalsExecutable $generalsExecutableSha256 $generalsProduction `
        $cohortNonce $cohortCreatedUtc 30000
    $zeroHourHost = New-InstalledTestHostEvidence `
        (Join-Path $testRoot 'zerohour-host') ZeroHour $artifact `
        $zeroHourExecutable $zeroHourExecutableSha256 $zeroHourProduction `
        $cohortNonce $cohortCreatedUtc 31000
    $generalsHostDocument = Read-TestJson $generalsHost.path
    $generalsValidationDocument = Read-TestJson $generalsHost.validationPath
    foreach ($hostRun in @($generalsHostDocument.runs)) {
        $validationRun = @($generalsValidationDocument.runs | Where-Object {
            $_.runId -ceq $hostRun.runId })
        Assert-InstalledTestCondition (
            $null -eq $hostRun.PSObject.Properties['runNonce'] -and
            $validationRun.Count -eq 1 -and
            $hostRun.receiptBinding.runNonce -ceq $validationRun[0].runNonce) `
            'Installed host run nonces must use the canonical nested receipt binding.'
    }

    $sourceHostTypeCases = @(
        [pscustomobject]@{ name = 'title'; good = 'Generals'
            property = 'title'
            edit = { param($h, $value) $h.title = $value } }
        [pscustomobject]@{ name = 'schedule measuredRuns'; good = 3
            property = 'measuredRuns'
            edit = { param($h, $value) $h.schedule.measuredRuns = $value } }
        [pscustomobject]@{ name = 'recordedUtc'; good = $generalsHostDocument.recordedUtc
            property = 'recordedUtc'
            edit = { param($h, $value) $h.recordedUtc = $value } }
        [pscustomobject]@{ name = 'fixture production path'; good =
                $generalsHostDocument.fixtureProductionReceipt.path
            property = 'path'
            edit = { param($h, $value)
                $h.fixtureProductionReceipt.path = $value } }
        [pscustomobject]@{ name = 'run warmup'; good = $false
            property = 'warmup'
            edit = { param($h, $value) $h.runs[1].warmup = $value } }
        [pscustomobject]@{ name = 'run ordinal'; good = 1
            property = 'ordinal'
            edit = { param($h, $value) $h.runs[1].ordinal = $value } }
    )
    $generalsHostBytes = [IO.File]::ReadAllBytes($generalsHost.path)
    try {
        foreach ($case in $sourceHostTypeCases) {
            foreach ($mutation in Get-InstalledTestScalarMutations $case.good) {
                $changedHost = Read-TestJson $generalsHost.path
                & $case.edit $changedHost $mutation.value
                Write-Json $generalsHost.path $changedHost
                if ($null -ne $mutation.PSObject.Properties['literal']) {
                    Set-InstalledTestFileJsonPropertyLiteral `
                        $generalsHost.path $case.property $mutation.literal
                }
                $caught = $null
                try {
                    Invoke-InstalledTestSourceHostRead $module `
                        $generalsHost.path (Get-Sha256 $generalsHost.path) `
                        Generals ('a' * 40) $artifact.sha256 $cohortNonce `
                        $cohortCreatedUtc $artifact.runtimeClosure.dependencyManifestSha256 `
                        $artifact.runtimeClosure.closureSha256 `
                        $generalsExecutableSha256
                }
                catch { $caught = $_ }
                Assert-InstalledTestCondition ($null -ne $caught -and
                    $caught.Exception.Message -match '(?i)string|integer|boolean|scalar|invalid|schedule|run|path') `
                    "The source-host reader must reject $($case.name) $($mutation.name)."
            }
        }
    }
    finally {
        [IO.File]::WriteAllBytes($generalsHost.path, $generalsHostBytes)
    }

    $projectedRoot = Join-Path $testRoot 'projected'
    $projected = New-Stage5InstalledKernelExecutionEvidence `
        -GeneralsHostPath $generalsHost.path `
        -GeneralsHostSha256 $generalsHost.sha256 `
        -ZeroHourHostPath $zeroHourHost.path `
        -ZeroHourHostSha256 $zeroHourHost.sha256 `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 $artifact.sha256 `
        -ExpectedCohortNonce $cohortNonce `
        -ExpectedCohortCreatedUtc $cohortCreatedUtc `
        -ExpectedDependencyManifestSha256 `
            $artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $artifact.runtimeClosure.closureSha256 `
        -GeneralsExecutableSha256 $generalsExecutableSha256 `
        -ZeroHourExecutableSha256 $zeroHourExecutableSha256 `
        -OutputRoot $projectedRoot
    Assert-InstalledTestCondition (
        @($projected.titles).Count -eq 2 -and
        @($projected.titles | Where-Object { $_.measuredRuns -eq 3 }).Count -eq
            2 -and
        -not [bool]$projected.finalAcceptanceClaim -and
        -not [bool]$projected.performanceScalingClaim) `
        'The two-title relocated projection must remain kernel-execution-only.'
    $reopened = Read-Stage5InstalledKernelExecutionEvidence `
        -Path $projected.path -ExpectedSha256 $projected.sha256 `
        -ExpectedSourceCommit ('a' * 40) `
        -ExpectedArtifactSetSha256 $artifact.sha256 `
        -ExpectedCohortNonce $cohortNonce `
        -ExpectedCohortCreatedUtc $cohortCreatedUtc `
        -ExpectedDependencyManifestSha256 `
            $artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $artifact.runtimeClosure.closureSha256 `
        -GeneralsExecutableSha256 $generalsExecutableSha256 `
        -ZeroHourExecutableSha256 $zeroHourExecutableSha256
    Assert-InstalledTestCondition ($reopened.sha256 -ceq $projected.sha256) `
        'The independent reader must reopen the exact relocated projection.'

    $readerTypeCases = @(
        [pscustomobject]@{ name = 'sourceCommit'; good = ('a' * 40)
            property = 'sourceCommit'
            edit = { param($d, $value) $d.sourceCommit = $value } }
        [pscustomobject]@{ name = 'finalAcceptanceClaim'; good = $false
            property = 'finalAcceptanceClaim'
            edit = { param($d, $value) $d.finalAcceptanceClaim = $value } }
        [pscustomobject]@{ name = 'title'; good = 'Generals'
            property = 'title'
            edit = { param($d, $value) $d.titleProofs[0].title = $value } }
        [pscustomobject]@{ name = 'executableSha256'; good = $generalsExecutableSha256
            property = 'executableSha256'
            edit = { param($d, $value)
                $d.titleProofs[0].executableSha256 = $value } }
        [pscustomobject]@{ name = 'schedule measuredRuns'; good = 3
            property = 'measuredRuns'
            edit = { param($d, $value)
                $d.titleProofs[0].schedule.measuredRuns = $value } }
        [pscustomobject]@{ name = 'run ordinal'; good = 1
            property = 'ordinal'
            edit = { param($d, $value)
                $d.titleProofs[0].runs[0].ordinal = $value } }
    )
    $projectedBytes = [IO.File]::ReadAllBytes($projected.path)
    try {
        foreach ($case in $readerTypeCases) {
            foreach ($mutation in Get-InstalledTestScalarMutations $case.good) {
                $changedDocument = Read-TestJson $projected.path
                & $case.edit $changedDocument $mutation.value
                Write-Json $projected.path $changedDocument
                if ($null -ne $mutation.PSObject.Properties['literal']) {
                    Set-InstalledTestFileJsonPropertyLiteral `
                        $projected.path $case.property $mutation.literal
                }
                $caught = $null
                try {
                    Invoke-InstalledTestIndependentReader $module `
                        $projected.path (Get-Sha256 $projected.path) `
                        ('a' * 40) $artifact.sha256 $cohortNonce `
                        $cohortCreatedUtc $artifact.runtimeClosure.dependencyManifestSha256 `
                        $artifact.runtimeClosure.closureSha256 `
                        $generalsExecutableSha256 $zeroHourExecutableSha256
                }
                catch { $caught = $_ }
                Assert-InstalledTestCondition ($null -ne $caught -and
                    $caught.Exception.Message -match '(?i)string|integer|boolean|scalar|invalid|title|run|cohort|claim') `
                    "The independent reader must reject $($case.name) $($mutation.name)."
            }
        }
    }
    finally {
        [IO.File]::WriteAllBytes($projected.path, $projectedBytes)
    }

    $rawBinding = $projected.document.titleProofs[0].runs[0].rawLog
    $rawPath = Join-Path $projectedRoot $rawBinding.path
    [IO.File]::AppendAllText($rawPath, 'tamper')
    $tamperCaught = $null
    try {
        Read-Stage5InstalledKernelExecutionEvidence `
            -Path $projected.path -ExpectedSha256 $projected.sha256 `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedArtifactSetSha256 $artifact.sha256 `
            -ExpectedCohortNonce $cohortNonce `
            -ExpectedCohortCreatedUtc $cohortCreatedUtc `
            -ExpectedDependencyManifestSha256 `
                $artifact.runtimeClosure.dependencyManifestSha256 `
            -ExpectedRuntimeClosureSha256 `
                $artifact.runtimeClosure.closureSha256 `
            -GeneralsExecutableSha256 $generalsExecutableSha256 `
            -ZeroHourExecutableSha256 $zeroHourExecutableSha256 | Out-Null
    }
    catch { $tamperCaught = $_ }
    Assert-InstalledTestCondition ($null -ne $tamperCaught -and
        $tamperCaught.Exception.Message -match '(?i)hash|sha|changed|rehashed') `
        'The independent reader must reject a changed retained raw log.'

    $missingReadCaught = $null
    try {
        Read-Stage5InstalledKernelExecutionEvidence `
            -Path (Join-Path $testRoot 'missing.json') `
            -ExpectedSha256 ('AA' * 32) -ExpectedSourceCommit ('a' * 40) `
            -ExpectedArtifactSetSha256 ('BB' * 32) `
            -ExpectedCohortNonce '10000000-0000-4000-8000-000000000001' `
            -ExpectedCohortCreatedUtc '2026-09-04T11:00:00.0000000Z' `
            -ExpectedDependencyManifestSha256 ('CC' * 32) `
            -ExpectedRuntimeClosureSha256 ('DD' * 32) `
            -GeneralsExecutableSha256 ('EE' * 32) `
            -ZeroHourExecutableSha256 ('FF' * 32) | Out-Null
    }
    catch { $missingReadCaught = $_ }
    Assert-InstalledTestCondition ($null -ne $missingReadCaught -and
        $missingReadCaught.Exception.Message -match '(?i)not found|absent|path') `
        'The independent reader must fail closed when the projection is absent.'

    Write-Output 'Stage 5 installed-kernel evidence projector tests passed.'
}
finally {
    if ($ownsRoot -and (Test-Path -LiteralPath $testRoot)) {
        $full = [IO.Path]::GetFullPath($testRoot)
        if ($full.StartsWith('H:\Stage5PerformanceScalingValidationScratch\ike-',
                [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $full -Recurse -Force
        }
    }
}
