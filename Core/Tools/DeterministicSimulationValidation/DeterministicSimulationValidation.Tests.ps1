[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:Failures = 0

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

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

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Write-Error "FAIL: $Message" -ErrorAction Continue
        ++$script:Failures
    }
}

function Assert-Throws {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    try {
        & $Action
        Assert-True $false "$Message (no exception)"
    }
    catch {
        Assert-True ($_.Exception.Message -match $Pattern) "$Message (got '$($_.Exception.Message)')"
    }
}

function Write-JsonDocument {
    param([string]$Path, [object]$Document)
    [IO.File]::WriteAllText($Path, ($Document | ConvertTo-Json -Depth 12))
}

function Write-Net3LoopbackTestManifest {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$GeneralsExecutableSha256, [string]$ZeroHourExecutableSha256)
    $topologies = @(
        [pscustomobject]@{ id = 'two-peer-1-v-16'; workers = @('1', '16') },
        [pscustomobject]@{ id = 'two-peer-2-v-auto'; workers = @('2', 'auto') },
        [pscustomobject]@{ id = 'two-peer-4-v-8'; workers = @('4', '8') },
        [pscustomobject]@{ id = 'four-peer-mixed-workers'; workers = @('1', '2', '8', 'auto') }
    )
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelBits = @(1, 2, 4, 8, 16, 32)
    $executableHashes = @{
        Generals = $GeneralsExecutableSha256
        ZeroHour = $ZeroHourExecutableSha256
    }
    $matches = @()
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($topology in $topologies) {
            foreach ($seed in @(23063, 49374)) {
                $rosterHash = if ($title -ceq 'Generals') { 'C' * 64 } else { 'D' * 64 }
                $peers = @()
                for ($peerIndex = 0; $peerIndex -lt $topology.workers.Count; ++$peerIndex) {
                    $requested = $topology.workers[$peerIndex]
                    $effective = if ($requested -ceq 'auto') { 8 } else { [int]$requested }
                    $kernels = @()
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $physical = if ($effective -eq 1) { 0 } else { 4 }
                        $kernels += [ordered]@{
                            name = $kernelNames[$kernelIndex]
                            bit = $kernelBits[$kernelIndex]
                            submitted = $physical
                            completed = $physical
                            physicalWorkerJobs = $physical
                            distinctPhysicalWorkers = $(if ($effective -eq 1) { 0 } else { 2 })
                        }
                    }
                    $peers += [ordered]@{
                        ordinal = $peerIndex
                        requestedWorkers = $requested
                        effectiveWorkers = $effective
                        networkHelloReady = $true
                        rosterExact = $true
                        rosterSha256 = $rosterHash
                        policyMask = 63
                        finalFrame = 42000
                        finalCRC = 'A1B2C3D4'
                        exitCode = 0
                        cleanShutdown = $true
                        kernels = $kernels
                    }
                }
                $matches += [ordered]@{
                    recordId = "$title/$($topology.id)/$seed"
                    sourceCommit = $SourceCommit
                    title = $title
                    executableSha256 = $executableHashes[$title]
                    artifactSetSha256 = $ArtifactSetSha256
                    topologyId = $topology.id
                    seed = $seed
                    networkHelloReady = $true
                    rosterExact = $true
                    rosterSha256 = $rosterHash
                    policyMask = 63
                    peers = $peers
                }
            }
        }
    }
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'installed-net3-loopback'
        status = 'passed'
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
        supportedKernelMask = 63
        executables = [ordered]@{
            Generals = $GeneralsExecutableSha256
            ZeroHour = $ZeroHourExecutableSha256
        }
        fixedSeeds = @(23063, 49374)
        matches = $matches
    })
}

function Write-PerformanceScalingTestManifest {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$ExecutableSha256, [string]$Stage3BaselineSha256)
    $phaseNames = @('owner-intake', 'world-queries', 'pathfinding', 'object-computation',
        'spatial-work', 'deterministic-commit', 'verification-publication')
    $phaseElapsed = @(10.0, 20.0, 20.0, 20.0, 15.0, 10.0, 5.0)
    $phaseSerial = @(8.0, 2.0, 2.0, 2.0, 1.0, 5.0, 2.0)
    $phases = @()
    for ($index = 0; $index -lt $phaseNames.Count; ++$index) {
        $phases += [ordered]@{
            name = $phaseNames[$index]
            elapsedMilliseconds = $phaseElapsed[$index]
            serialMilliseconds = $phaseSerial[$index]
        }
    }
    $kernels = @()
    foreach ($name in @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')) {
        $kernels += [ordered]@{
            name = $name; admittedSlices = 32
            captureMilliseconds = 1.0; scheduleMilliseconds = 1.0
            waitMilliseconds = 2.0; validateMilliseconds = 1.0; commitMilliseconds = 1.0
            totalParallelMilliseconds = 6.0
            exactSerialOperationMilliseconds = 12.0; netSpeedup = 2.0
        }
    }
    $fixtures = @()
    $fixtureNames = @('one-thousand-units', 'four-thousand-units',
        'eight-thousand-units', 'dense-eight-player')
    $unitCounts = @(1000, 4000, 8000, 12000)
    foreach ($index in 0..3) {
        $fixtures += [ordered]@{
            name = $fixtureNames[$index]; playerCount = 8; peakUnitCount = $unitCounts[$index]
            repeats = 5; stage3OneWorkerMilliseconds = 1000.0
            stage5OneWorkerMilliseconds = 1020.0; eightPhysicalCoreMilliseconds = 500.0
            sixteenPhysicalCoreMilliseconds = 450.0; oneWorkerRegressionRatio = 1.02
            eightPhysicalCoreSpeedup = 2.04; eightToSixteenSpeedup = 1.1111111111
        }
    }
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1; evidenceKind = 'stage5-performance-scaling'; status = 'passed'
        sourceCommit = $SourceCommit; artifactSetSha256 = $ArtifactSetSha256
        title = 'ZeroHour'; executableSha256 = $ExecutableSha256
        stage3BaselineSha256 = $Stage3BaselineSha256
        measurementMode = 'headless-throughput'; installedRuntime = $true
        topology = [ordered]@{
            source = 'GetSystemCpuSetInformation'; topologySha256 = ('E' * 64)
            physicalCoreCount = 16; logicalProcessorCount = 32
        }
        selectedLanes = @(
            [ordered]@{ name = 'forced-one'; requestedWorkers = 1; selectedLogicalProcessors = 1
                selectedDistinctPhysicalCores = 1; selectedPhysicalCoreMask = '0000000000000001' },
            [ordered]@{ name = 'physical-8'; requestedWorkers = 8; selectedLogicalProcessors = 8
                selectedDistinctPhysicalCores = 8; selectedPhysicalCoreMask = '00000000000000FF' },
            [ordered]@{ name = 'physical-16'; requestedWorkers = 16; selectedLogicalProcessors = 16
                selectedDistinctPhysicalCores = 16; selectedPhysicalCoreMask = '000000000000FFFF' }
        )
        oneWorkerPhases = $phases
        amdahl = [ordered]@{
            totalOneWorkerMilliseconds = 100.0; totalSerialMilliseconds = 22.0
            serialFraction = 0.22; maximumSpeedup = 4.5454545455; reachesTwoX = $true
        }
        kernelTimings = $kernels
        fixtures = $fixtures
    })
}

function Write-TestManifest {
    param([string]$Path, [string]$ExecutableHash, [string]$ReplayOneHash,
        [string]$ReplayTwoHash, [string]$FirstSource = 'fixtures\reference.rep',
        [int[]]$Seeds = @(1729, 1730, 1731),
        [string[]]$Scenarios = @('4v3', '4v2'), [int]$AiRepeats = 2)
    $manifest = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = $ExecutableHash
        fixtures = @(
            [ordered]@{
                id = 'reference'
                source = $FirstSource
                sha256 = $ReplayOneHash
                stress = $false
                maps = @()
            },
            [ordered]@{
                id = 'hard-ai-2v6'
                source = 'fixtures\hard-ai-2v6.rep'
                sha256 = $ReplayTwoHash
                stress = $true
                maps = @()
            }
        )
        ai = [ordered]@{
            seeds = $Seeds
            scenarios = $Scenarios
            repeats = $AiRepeats
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

function Write-StandardTestManifest {
    param([string]$Path, [string]$ExecutableHash, [string]$FixtureDirectory)
    $fixtureEntries = @()
    for ($index = 1; $index -le 10; ++$index) {
        $isStress = $index -eq 10
        $id = if ($isStress) { 'hard-ai-2v6' } else { 'reference-{0:D2}' -f $index }
        $leaf = "standard-$id.rep"
        $fixturePath = Join-Path $FixtureDirectory $leaf
        [IO.File]::WriteAllText($fixturePath, "standard replay fixture $index")
        $fixtureEntries += [ordered]@{
            id = $id
            source = "fixtures\$leaf"
            sha256 = Get-Sha256 $fixturePath
            stress = $isStress
            maps = @()
        }
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = $ExecutableHash
        fixtures = $fixtureEntries
        ai = [ordered]@{
            seeds = @(1729, 1730, 1731)
            scenarios = @('4v3', '4v2')
            repeats = 2
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

function New-AiCompletionOutput {
    param([int]$Seed = 1729, [string]$Mode = 'parallel', [string]$RequestedWorkers = '2',
        [int]$EffectiveWorkers = 2, [string]$Digest = 'A1B2C3D4', [int]$EndFrame = 42000,
        [int]$Winner = 1, [int]$Submitted = 20, [int]$Executed = 20,
        [int]$Fallback = 0, [string]$ExecutableHash = ('A' * 64),
        [string]$Scenario = '4v3', [int]$ActualAi = 7, [string]$ActualTeams = '4v3',
        [int]$LoadedSeed = 0, [string]$RequestedPipeline = 'serial',
        [string]$EffectivePipeline = 'serial', [string]$RequestedSimulation = '',
        [string]$EffectiveSimulation = '', [int]$AuthoritativeCommits = 5,
        [int]$AiCommittedBatches = -1, [int]$AiParallelAuthoritativeCommits = -1,
        [int]$ShadowExecutions = 0, [int]$OwnerFallbacks = -1,
        [int]$AiSubmitted = -1, [int]$AiCompleted = -1,
        [int]$CollisionAuthoritativeCommits = -1,
        [int]$CollisionShadowExecutions = 0, [int]$CollisionShadowMismatches = 0,
        [int]$CollisionShadowComparedCandidates = -1,
        [int]$CollisionOwnerFallbacks = 0, [int]$CollisionUnexpectedFallbacks = 0,
        [int]$CollisionCommittedCandidates = -1, [int]$CollisionPreparedPairs = -1,
        [int]$CollisionUniqueCandidates = -1, [int]$CollisionSubmitted = -1,
        [int]$CollisionCompleted = -1, [int]$CollisionIneligibleSlices = -1,
		[int]$PathWorkerExecuted = -1,
		[int]$PathAuthoritativeCommits = -1, [int]$PathOwnerHelped = 0,
		[int]$PathAuthoritativeMultiWorkerCommits = -1,
		[int]$PathUnsupportedAuthority = 0, [int]$PathShadowAuthority = 0,
		[int]$PathStaleAcceptance = 0, [int]$PathMalformedAcceptance = 0,
		[int]$PathShadowOnly = 0, [int]$PathTimeouts = 0, [int]$PathLateDrains = 0,
		[int]$PathValidationFailures = 0, [int]$PathPeakWorkers = -1,
		[int]$PhysicsAuthoritativeBatches = -1,
		[int]$PhysicsCommittedPrefixes = -1, [int]$PhysicsRanges = -1,
		[int]$PhysicsSubmitted = -1, [int]$PhysicsCompleted = -1,
		[int]$PhysicsShadowExecutions = 0, [int]$PhysicsShadowMismatches = 0,
		[int]$PhysicsShadowPrefixes = -1, [int]$PhysicsShadowRanges = -1,
		[int]$PhysicsShadowSubmitted = -1, [int]$PhysicsShadowCompleted = -1,
		[int]$PhysicsOwnerFallbacks = 0, [int]$PhysicsUnexpectedFallbacks = 0,
		[int]$PhysicsStaleRejections = 0, [int]$PhysicsCircuitBreakerTrips = 0,
		[int]$SpatialCapturedArenas = -1, [int]$SpatialCaptureFailures = 0,
		[int]$SpatialSuccessfulCollections = -1,
		[int]$SpatialSuccessfulCollectionQueries = -1,
		[int]$SpatialSuccessfulCollectionRanges = -1,
		[int]$SpatialMultiRangeCollections = -1,
		[int]$SpatialCollectionSubmitted = -1,
		[int]$SpatialCollectionCompleted = -1,
		[int]$SpatialCollectionPhysical = -1,
		[int]$SpatialCollectionOwnerHelped = 0,
		[Int64]$SpatialCollectionPhysicalWorkerMask = -1,
		[int]$SpatialMaximumCollectionQueries = -1,
		[int]$SpatialMaximumCollectionRanges = -1,
		[int]$SpatialMaximumCollectionDistinctPhysicalWorkers = -1,
		[int]$SpatialHealingEligible = 5,
		[int]$SpatialHealingAuthoritative = -1,
		[int]$SpatialHealingCandidates = -1, [int]$SpatialHealingShadow = -1,
		[int]$SpatialHealingShadowMismatches = 0,
		[int]$SpatialHealingSubmitted = -1, [int]$SpatialHealingCompleted = -1,
		[int]$SpatialHealingPhysical = -1, [int]$SpatialHealingOwnerHelped = 0,
		[int]$SpatialHealingExpectedFallbacks = -1,
		[int]$SpatialHealingUnexpectedFallbacks = 0,
		[int]$SpatialPdlEligible = 5,
		[int]$SpatialPdlAuthoritative = -1, [int]$SpatialPdlCandidates = -1,
		[int]$SpatialPdlShadow = -1, [int]$SpatialPdlShadowMismatches = 0,
		[int]$SpatialPdlSubmitted = -1, [int]$SpatialPdlCompleted = -1,
		[int]$SpatialPdlPhysical = -1, [int]$SpatialPdlOwnerHelped = 0,
		[int]$SpatialPdlExpectedFallbacks = -1,
		[int]$SpatialPdlUnexpectedFallbacks = 0,
		[switch]$OmitWorkEvidence)
    if ($LoadedSeed -eq 0) { $LoadedSeed = $Seed }
    if ([string]::IsNullOrEmpty($RequestedSimulation)) { $RequestedSimulation = $Mode }
    if ([string]::IsNullOrEmpty($EffectiveSimulation)) { $EffectiveSimulation = $Mode }
    if ($OwnerFallbacks -lt 0) { $OwnerFallbacks = $Fallback }
    if ($AiSubmitted -lt 0) { $AiSubmitted = $Submitted }
    if ($AiCompleted -lt 0) { $AiCompleted = $Executed }
    if ($AiCommittedBatches -lt 0) { $AiCommittedBatches = $AuthoritativeCommits }
    if ($AiParallelAuthoritativeCommits -lt 0) {
        $AiParallelAuthoritativeCommits = $AuthoritativeCommits
    }
    if ($CollisionAuthoritativeCommits -lt 0) {
        $CollisionAuthoritativeCommits = if ($Mode -ceq 'parallel' -and
            $RequestedWorkers -cne '1') { 3 } else { 0 }
    }
    if ($CollisionCommittedCandidates -lt 0) {
        $CollisionCommittedCandidates = if ($CollisionAuthoritativeCommits -gt 0) { 12 } else { 0 }
    }
    $collisionPreparedEligible = ($Mode -ceq 'parallel' -or $Mode -ceq 'shadow') -and
        $RequestedWorkers -cne '1'
    if ($CollisionPreparedPairs -lt 0) { $CollisionPreparedPairs = if ($collisionPreparedEligible) { 24 } else { 0 } }
    if ($CollisionUniqueCandidates -lt 0) { $CollisionUniqueCandidates = if ($collisionPreparedEligible) { 12 } else { 0 } }
    if ($CollisionSubmitted -lt 0) { $CollisionSubmitted = if ($collisionPreparedEligible) { 4 } else { 0 } }
    if ($CollisionCompleted -lt 0) { $CollisionCompleted = $CollisionSubmitted }
    if ($CollisionIneligibleSlices -lt 0) { $CollisionIneligibleSlices = if ($Mode -ceq 'serial') { 0 } else { 2 } }
    if ($CollisionShadowComparedCandidates -lt 0) {
        $CollisionShadowComparedCandidates = if ($CollisionShadowExecutions -gt 0) { 6 } else { 0 }
    }
	$pathBatchEligible = $Mode -ceq 'parallel' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	if ($PathWorkerExecuted -lt 0) { $PathWorkerExecuted = if ($pathBatchEligible) { 4 } else { 0 } }
	if ($PathAuthoritativeCommits -lt 0) { $PathAuthoritativeCommits = if ($pathBatchEligible) { 2 } else { 0 } }
	if ($PathAuthoritativeMultiWorkerCommits -lt 0) {
		$PathAuthoritativeMultiWorkerCommits = if ($pathBatchEligible) {
			$PathAuthoritativeCommits
		} else { 0 }
	}
	$pathExecuted = $PathWorkerExecuted + $PathOwnerHelped
	$pathSubmitted = $pathExecuted
	$pathEligible = if ($pathSubmitted -gt 0) { $pathSubmitted + 1 } else { 0 }
	if ($PathPeakWorkers -lt 0) {
		$PathPeakWorkers = if ($PathWorkerExecuted -gt 1) {
			[Math]::Min(2, $EffectiveWorkers)
		} elseif ($PathWorkerExecuted -gt 0) { 1 } else { 0 }
	}
	$pathCallbackMin = if ($pathEligible -gt 0) { 2 } else { 0 }
	$pathCallbackMax = if ($pathEligible -gt 0) { 18 } else { 0 }
	$physicsAuthoritativeEligible = $Mode -ceq 'parallel' -and $RequestedWorkers -cne '1'
	if ($PhysicsAuthoritativeBatches -lt 0) { $PhysicsAuthoritativeBatches = if ($physicsAuthoritativeEligible) { 3 } else { 0 } }
	if ($PhysicsCommittedPrefixes -lt 0) { $PhysicsCommittedPrefixes = if ($physicsAuthoritativeEligible) { 96 } else { 0 } }
	if ($PhysicsRanges -lt 0) { $PhysicsRanges = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsSubmitted -lt 0) { $PhysicsSubmitted = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsCompleted -lt 0) { $PhysicsCompleted = $PhysicsSubmitted }
	if ($PhysicsShadowPrefixes -lt 0) { $PhysicsShadowPrefixes = if ($PhysicsShadowExecutions -gt 0) { 96 } else { 0 } }
	if ($PhysicsShadowRanges -lt 0) { $PhysicsShadowRanges = if ($PhysicsShadowExecutions -gt 0) { 4 } else { 0 } }
	if ($PhysicsShadowSubmitted -lt 0) { $PhysicsShadowSubmitted = if ($PhysicsShadowExecutions -gt 0) { 4 } else { 0 } }
	if ($PhysicsShadowCompleted -lt 0) { $PhysicsShadowCompleted = $PhysicsShadowSubmitted }
	$physicsShadowMatches = $PhysicsShadowExecutions - $PhysicsShadowMismatches
	$physicsPreparedWork = $PhysicsSubmitted -gt 0 -or $PhysicsShadowSubmitted -gt 0
	$physicsAllocatedBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsCaptureNanoseconds = if ($physicsPreparedWork) { 100 } else { 0 }
	$physicsPrepareNanoseconds = if ($physicsPreparedWork) { 200 } else { 0 }
	$physicsWaitNanoseconds = if ($physicsPreparedWork) { 300 } else { 0 }
	$physicsCommitNanoseconds = if ($physicsPreparedWork) { 400 } else { 0 }
	$physicsStorageBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsStorageCapacityBytes = if ($physicsPreparedWork) { 8192 } else { 0 }
	$physicsStorageAllocations = if ($physicsPreparedWork) { 1 } else { 0 }
	$spatialParallelEligible = $Mode -ceq 'parallel' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	$spatialShadowEligible = $Mode -ceq 'shadow' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	$spatialCollectionEligible = $spatialParallelEligible -or $spatialShadowEligible
	if ($SpatialCapturedArenas -lt 0) {
		$SpatialCapturedArenas = if ($spatialCollectionEligible) { 4 } else { 0 }
	}
	if ($SpatialSuccessfulCollections -lt 0) { $SpatialSuccessfulCollections = if ($spatialCollectionEligible) { 4 } else { 0 } }
	if ($SpatialMaximumCollectionQueries -lt 0) { $SpatialMaximumCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 5 } else { 0 } }
	if ($SpatialMaximumCollectionRanges -lt 0) {
		$SpatialMaximumCollectionRanges = if ($SpatialSuccessfulCollections -gt 0) {
			[Math]::Min($EffectiveWorkers, $SpatialMaximumCollectionQueries)
		} else { 0 }
	}
	if ($SpatialSuccessfulCollectionQueries -lt 0) { $SpatialSuccessfulCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 20 } else { 0 } }
	if ($SpatialSuccessfulCollectionRanges -lt 0) {
		$SpatialSuccessfulCollectionRanges = $SpatialSuccessfulCollections *
			$SpatialMaximumCollectionRanges
	}
	if ($SpatialMultiRangeCollections -lt 0) { $SpatialMultiRangeCollections = $SpatialSuccessfulCollections }
	if ($SpatialCollectionSubmitted -lt 0) { $SpatialCollectionSubmitted = 2 * $SpatialSuccessfulCollectionRanges }
	if ($SpatialCollectionCompleted -lt 0) { $SpatialCollectionCompleted = $SpatialCollectionSubmitted }
	if ($SpatialCollectionPhysical -lt 0) { $SpatialCollectionPhysical = $SpatialCollectionCompleted }
	if ($SpatialMaximumCollectionDistinctPhysicalWorkers -lt 0) {
		$SpatialMaximumCollectionDistinctPhysicalWorkers = if ($SpatialSuccessfulCollections -gt 0) {
			if ($SpatialMaximumCollectionRanges -ge 4) { 2 } else { 1 }
		} else { 0 }
	}
	if ($SpatialCollectionPhysicalWorkerMask -lt 0) {
		$SpatialCollectionPhysicalWorkerMask = if ($SpatialMaximumCollectionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $SpatialMaximumCollectionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($SpatialHealingAuthoritative -lt 0) { $SpatialHealingAuthoritative = if ($spatialParallelEligible) { 3 } else { 0 } }
	if ($SpatialHealingCandidates -lt 0) { $SpatialHealingCandidates = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingShadow -lt 0) { $SpatialHealingShadow = if ($spatialShadowEligible) { 2 } else { 0 } }
	if ($SpatialPdlAuthoritative -lt 0) { $SpatialPdlAuthoritative = if ($spatialParallelEligible) { 2 } else { 0 } }
	if ($SpatialPdlCandidates -lt 0) { $SpatialPdlCandidates = if ($SpatialPdlAuthoritative -gt 0) { 5 } else { 0 } }
	if ($SpatialPdlShadow -lt 0) { $SpatialPdlShadow = if ($spatialShadowEligible) { 3 } else { 0 } }
	$spatialHasJobs = $SpatialHealingAuthoritative -gt 0 -or $SpatialHealingShadow -gt 0
	if ($SpatialHealingSubmitted -lt 0) { $SpatialHealingSubmitted = if ($spatialHasJobs) { 8 } else { 0 } }
	if ($SpatialHealingCompleted -lt 0) { $SpatialHealingCompleted = $SpatialHealingSubmitted }
	if ($SpatialHealingPhysical -lt 0) { $SpatialHealingPhysical = $SpatialHealingCompleted }
	$spatialPdlHasJobs = $SpatialPdlAuthoritative -gt 0 -or $SpatialPdlShadow -gt 0
	if ($SpatialPdlSubmitted -lt 0) { $SpatialPdlSubmitted = if ($spatialPdlHasJobs) { 8 } else { 0 } }
	if ($SpatialPdlCompleted -lt 0) { $SpatialPdlCompleted = $SpatialPdlSubmitted }
	if ($SpatialPdlPhysical -lt 0) { $SpatialPdlPhysical = $SpatialPdlCompleted }
	if ($SpatialHealingExpectedFallbacks -lt 0) {
		$SpatialHealingExpectedFallbacks = if ($Mode -ceq 'serial' -or $RequestedWorkers -ceq '1') { 2 } else { 0 }
	}
	if ($SpatialPdlExpectedFallbacks -lt 0) {
		$SpatialPdlExpectedFallbacks = if ($Mode -ceq 'serial' -or $RequestedWorkers -ceq '1') { 2 } else { 0 }
	}
	$spatialHealingMatches = $SpatialHealingShadow - $SpatialHealingShadowMismatches
	$spatialPdlMatches = $SpatialPdlShadow - $SpatialPdlShadowMismatches
    $workEvidence = if ($OmitWorkEvidence) { '' } else {
        " authoritative_commits=$AuthoritativeCommits shadow_executions=$ShadowExecutions owner_fallbacks=$OwnerFallbacks" +
        " ai_captured_snapshots=5 ai_captured_candidates=20 ai_requested_batches=5" +
        " ai_submitted_jobs=$AiSubmitted ai_completed_jobs=$AiCompleted ai_serial_fallbacks=$OwnerFallbacks" +
        " ai_shadow_matches=$ShadowExecutions ai_shadow_mismatches=0 ai_validation_failures=0" +
        " ai_committed_batches=$AiCommittedBatches" +
        " ai_parallel_authoritative_commits=$AiParallelAuthoritativeCommits ai_rejected_commits=0" +
		" direct_eligible=$pathEligible direct_submitted=$pathSubmitted direct_executed=$pathExecuted" +
		" direct_worker_executed=$PathWorkerExecuted direct_owner_helped=$PathOwnerHelped" +
		" direct_authoritative_commits=$PathAuthoritativeCommits direct_stale_rejections=0" +
		" direct_authoritative_multiworker_commits=$PathAuthoritativeMultiWorkerCommits" +
		" direct_validation_failures=$PathValidationFailures direct_serial_fallbacks=0" +
		" direct_unsupported_authority=$PathUnsupportedAuthority direct_shadow_authority=$PathShadowAuthority" +
		" direct_stale_acceptance=$PathStaleAcceptance direct_malformed_acceptance=$PathMalformedAcceptance" +
		" direct_shadow_only=$PathShadowOnly direct_timeouts=$PathTimeouts" +
		" direct_late_drains=$PathLateDrains direct_peak_active_workers=$PathPeakWorkers" +
		" direct_callback_min=$pathCallbackMin direct_callback_max=$pathCallbackMax" +
        " collision_authoritative_commits=$CollisionAuthoritativeCommits" +
        " collision_shadow_executions=$CollisionShadowExecutions" +
        " collision_shadow_compared_candidates=$CollisionShadowComparedCandidates" +
        " collision_shadow_mismatches=$CollisionShadowMismatches" +
        " collision_owner_fallbacks=$CollisionOwnerFallbacks" +
        " collision_unexpected_fallbacks=$CollisionUnexpectedFallbacks" +
        " collision_ineligible_slices=$CollisionIneligibleSlices collision_stale_rejections=0" +
        " collision_committed_candidates=$CollisionCommittedCandidates" +
        " collision_prepared_pairs=$CollisionPreparedPairs" +
        " collision_unique_candidates=$CollisionUniqueCandidates" +
        " collision_submitted_jobs=$CollisionSubmitted collision_completed_jobs=$CollisionCompleted" +
		" physics_authoritative_batches=$PhysicsAuthoritativeBatches" +
		" physics_committed_prefixes=$PhysicsCommittedPrefixes physics_ranges=$PhysicsRanges" +
		" physics_submitted_jobs=$PhysicsSubmitted physics_completed_jobs=$PhysicsCompleted" +
		" physics_allocated_bytes=$physicsAllocatedBytes physics_capture_ns=$physicsCaptureNanoseconds physics_prepare_ns=$physicsPrepareNanoseconds" +
		" physics_wait_ns=$physicsWaitNanoseconds physics_commit_ns=$physicsCommitNanoseconds physics_storage_bytes=$physicsStorageBytes" +
		" physics_storage_capacity_bytes=$physicsStorageCapacityBytes physics_storage_allocations=$physicsStorageAllocations" +
		" physics_shadow_executions=$PhysicsShadowExecutions" +
		" physics_shadow_prefixes=$PhysicsShadowPrefixes physics_shadow_ranges=$PhysicsShadowRanges" +
		" physics_shadow_submitted_jobs=$PhysicsShadowSubmitted" +
		" physics_shadow_completed_jobs=$PhysicsShadowCompleted" +
		" physics_shadow_matches=$physicsShadowMatches" +
		" physics_shadow_mismatches=$PhysicsShadowMismatches" +
		" physics_owner_fallbacks=$PhysicsOwnerFallbacks physics_ineligible_slices=2" +
		" physics_unexpected_fallbacks=$PhysicsUnexpectedFallbacks" +
		" physics_stale_rejections=$PhysicsStaleRejections" +
		" physics_circuit_breaker_trips=$PhysicsCircuitBreakerTrips" +
		" spatial_captured_arenas=$SpatialCapturedArenas spatial_capture_failures=$SpatialCaptureFailures" +
		" spatial_successful_collections=$SpatialSuccessfulCollections" +
		" spatial_successful_collection_queries=$SpatialSuccessfulCollectionQueries" +
		" spatial_successful_collection_ranges=$SpatialSuccessfulCollectionRanges" +
		" spatial_multi_range_collections=$SpatialMultiRangeCollections" +
		" spatial_collection_submitted_jobs=$SpatialCollectionSubmitted" +
		" spatial_collection_completed_jobs=$SpatialCollectionCompleted" +
		" spatial_collection_physical_worker_jobs=$SpatialCollectionPhysical" +
		" spatial_collection_owner_helped_jobs=$SpatialCollectionOwnerHelped" +
		" spatial_collection_physical_worker_mask=$SpatialCollectionPhysicalWorkerMask" +
		" spatial_maximum_collection_queries=$SpatialMaximumCollectionQueries" +
		" spatial_maximum_collection_ranges=$SpatialMaximumCollectionRanges" +
		" spatial_maximum_collection_distinct_physical_workers=$SpatialMaximumCollectionDistinctPhysicalWorkers" +
		" spatial_healing_eligible_queries=$SpatialHealingEligible" +
		" spatial_healing_authoritative_queries=$SpatialHealingAuthoritative" +
		" spatial_healing_authoritative_candidates=$SpatialHealingCandidates" +
		" spatial_healing_shadow_queries=$SpatialHealingShadow" +
		" spatial_healing_shadow_matches=$spatialHealingMatches" +
		" spatial_healing_shadow_mismatches=$SpatialHealingShadowMismatches" +
		" spatial_healing_submitted_jobs=$SpatialHealingSubmitted" +
		" spatial_healing_completed_jobs=$SpatialHealingCompleted" +
		" spatial_healing_physical_worker_jobs=$SpatialHealingPhysical" +
		" spatial_healing_owner_helped_jobs=$SpatialHealingOwnerHelped" +
		" spatial_healing_expected_fallbacks=$SpatialHealingExpectedFallbacks" +
		" spatial_healing_unexpected_fallbacks=$SpatialHealingUnexpectedFallbacks" +
		" spatial_healing_stale_rejections=0 spatial_healing_validation_failures=0" +
		" spatial_healing_circuit_breaker_trips=0" +
		" spatial_pdl_eligible_queries=$SpatialPdlEligible" +
		" spatial_pdl_authoritative_queries=$SpatialPdlAuthoritative" +
		" spatial_pdl_authoritative_candidates=$SpatialPdlCandidates" +
		" spatial_pdl_shadow_queries=$SpatialPdlShadow" +
		" spatial_pdl_shadow_matches=$spatialPdlMatches" +
		" spatial_pdl_shadow_mismatches=$SpatialPdlShadowMismatches" +
		" spatial_pdl_submitted_jobs=$SpatialPdlSubmitted" +
		" spatial_pdl_completed_jobs=$SpatialPdlCompleted" +
		" spatial_pdl_physical_worker_jobs=$SpatialPdlPhysical" +
		" spatial_pdl_owner_helped_jobs=$SpatialPdlOwnerHelped" +
		" spatial_pdl_expected_fallbacks=$SpatialPdlExpectedFallbacks" +
		" spatial_pdl_unexpected_fallbacks=$SpatialPdlUnexpectedFallbacks" +
		" spatial_pdl_stale_rejections=0 spatial_pdl_validation_failures=0" +
		" spatial_pdl_circuit_breaker_trips=0"
    }
    return "SKIRMISH_AI_TEST_COMPLETE seed=$Seed loaded_seed=$LoadedSeed scenario=$Scenario actual_ai=$ActualAi actual_teams=$ActualTeams winner_team=$Winner end_frame=$EndFrame " +
        "executable_sha256=$ExecutableHash simulation_mode=$Mode requested_pipeline=$RequestedPipeline effective_pipeline=$EffectivePipeline " +
        "requested_simulation=$RequestedSimulation effective_simulation=$EffectiveSimulation requested_workers=$RequestedWorkers " +
        "effective_workers=$EffectiveWorkers worker_policy=auto final_digest=$Digest wall_ms=100 " +
        "job_submitted=$Submitted job_executed=$Executed job_steals=0 job_owner_help=0 job_waits=0 " +
        "job_worker_wait_reject=0 job_failed=0 job_cancelled=0 job_fallback=$Fallback " +
        "job_queue_latency_ns=1 job_max_queue_latency_ns=1 job_sleeps=0 job_wakes=0 " +
        "job_affinity_failures=0$workEvidence job_queue_high_water=1 job_peak_active_workers=$EffectiveWorkers " +
        "available_cpus=16 reserved_owner_cpus=1 selected_worker_cpus=$EffectiveWorkers"
}

function New-AiResult {
    param([string]$Configuration, [int]$Repeat, [string]$Digest = 'A1B2C3D4',
        [int]$EndFrame = 42000, [int]$Winner = 1,
        [string]$DeterminismKey = '4v3-seed-1729')
    return [pscustomobject]@{
        kind = 'ai'; determinismKey = $DeterminismKey; configuration = $Configuration; repeat = $Repeat
        aiEvidence = [pscustomobject]@{ finalDigest = $Digest; endFrame = $EndFrame; winnerTeam = $Winner }
    }
}

function New-ReplayMetricOutput {
    param([string]$Mode = 'parallel', [string]$EffectiveMode = 'parallel', [int]$Scheduler = 1,
        [int]$Workers = 2, [int]$Submitted = 20, [int]$Executed = 20, [int]$Fallback = 0,
		[int]$CollisionShadowMismatches = 0, [int]$CollisionUnexpectedFallbacks = 0,
		[int]$CollisionShadowExecutions = 0,
		[int]$CollisionShadowComparedCandidates = 0,
		[int]$CollisionOwnerFallbacks = 0,
		[int]$PhysicsAuthoritativeBatches = -1, [int]$PhysicsCommittedPrefixes = -1,
		[int]$PhysicsRanges = -1, [int]$PhysicsSubmitted = -1,
		[int]$PhysicsCompleted = -1, [int]$PhysicsShadowExecutions = 0,
		[int]$PhysicsShadowPrefixes = 0, [int]$PhysicsShadowRanges = 0,
		[int]$PhysicsShadowSubmitted = 0, [int]$PhysicsShadowCompleted = 0,
		[int]$PhysicsShadowMismatches = 0, [int]$PhysicsUnexpectedFallbacks = 0,
		[int]$CollisionAuthoritativeCommits = -1,
		[int]$CollisionCommittedCandidates = -1,
		[int]$CollisionPreparedPairs = -1, [int]$CollisionUniqueCandidates = -1,
		[int]$CollisionSubmitted = -1, [int]$CollisionCompleted = -1,
		[int]$CollisionIneligibleSlices = -1,
		[int]$SpatialCapturedArenas = -1, [int]$SpatialCaptureFailures = 0,
		[int]$SpatialSuccessfulCollections = -1,
		[int]$SpatialSuccessfulCollectionQueries = -1,
		[int]$SpatialSuccessfulCollectionRanges = -1,
		[int]$SpatialMultiRangeCollections = -1,
		[int]$SpatialCollectionSubmitted = -1,
		[int]$SpatialCollectionCompleted = -1,
		[int]$SpatialCollectionPhysical = -1,
		[int]$SpatialCollectionOwnerHelped = 0,
		[Int64]$SpatialCollectionPhysicalWorkerMask = -1,
		[int]$SpatialMaximumCollectionQueries = -1,
		[int]$SpatialMaximumCollectionRanges = -1,
		[int]$SpatialMaximumCollectionDistinctPhysicalWorkers = -1,
		[int]$SpatialHealingEligible = 5,
		[int]$SpatialHealingAuthoritative = -1,
		[int]$SpatialHealingCandidates = -1,
		[int]$SpatialHealingSubmitted = -1, [int]$SpatialHealingCompleted = -1,
		[int]$SpatialHealingPhysical = -1, [int]$SpatialHealingOwnerHelped = 0,
		[int]$SpatialHealingExpectedFallbacks = -1,
		[int]$SpatialHealingUnexpectedFallbacks = 0,
		[int]$SpatialPdlEligible = 5,
		[int]$SpatialPdlAuthoritative = -1, [int]$SpatialPdlCandidates = -1,
		[int]$SpatialPdlSubmitted = -1, [int]$SpatialPdlCompleted = -1,
		[int]$SpatialPdlPhysical = -1, [int]$SpatialPdlOwnerHelped = 0,
		[int]$SpatialPdlExpectedFallbacks = -1,
		[int]$SpatialPdlUnexpectedFallbacks = 0)
    $collisionPreparedEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
    if ($CollisionAuthoritativeCommits -lt 0) { $CollisionAuthoritativeCommits = if ($collisionPreparedEligible) { 3 } else { 0 } }
    if ($CollisionCommittedCandidates -lt 0) { $CollisionCommittedCandidates = if ($CollisionAuthoritativeCommits -gt 0) { 12 } else { 0 } }
    if ($CollisionPreparedPairs -lt 0) { $CollisionPreparedPairs = if ($collisionPreparedEligible) { 24 } else { 0 } }
    if ($CollisionUniqueCandidates -lt 0) { $CollisionUniqueCandidates = if ($collisionPreparedEligible) { 12 } else { 0 } }
    if ($CollisionSubmitted -lt 0) { $CollisionSubmitted = if ($collisionPreparedEligible) { 4 } else { 0 } }
    if ($CollisionCompleted -lt 0) { $CollisionCompleted = $CollisionSubmitted }
    if ($CollisionIneligibleSlices -lt 0) { $CollisionIneligibleSlices = if ($Mode -ceq 'serial') { 0 } else { 2 } }
    $physicsAuthoritativeEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
    if ($PhysicsAuthoritativeBatches -lt 0) { $PhysicsAuthoritativeBatches = if ($physicsAuthoritativeEligible) { 3 } else { 0 } }
    if ($PhysicsCommittedPrefixes -lt 0) { $PhysicsCommittedPrefixes = if ($physicsAuthoritativeEligible) { 96 } else { 0 } }
    if ($PhysicsRanges -lt 0) { $PhysicsRanges = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsSubmitted -lt 0) { $PhysicsSubmitted = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsCompleted -lt 0) { $PhysicsCompleted = $PhysicsSubmitted }
	$physicsPreparedWork = $PhysicsSubmitted -gt 0 -or $PhysicsShadowSubmitted -gt 0
	$physicsAllocatedBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsCaptureNanoseconds = if ($physicsPreparedWork) { 100 } else { 0 }
	$physicsPrepareNanoseconds = if ($physicsPreparedWork) { 200 } else { 0 }
	$physicsWaitNanoseconds = if ($physicsPreparedWork) { 300 } else { 0 }
	$physicsCommitNanoseconds = if ($physicsPreparedWork) { 400 } else { 0 }
	$physicsStorageBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsStorageCapacityBytes = if ($physicsPreparedWork) { 8192 } else { 0 }
	$physicsStorageAllocations = if ($physicsPreparedWork) { 1 } else { 0 }
	$spatialEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
	$spatialCollectionEligible = ($Mode -ceq 'parallel' -or $Mode -ceq 'shadow') -and $Workers -gt 1
	if ($SpatialCapturedArenas -lt 0) {
		$SpatialCapturedArenas = if ($spatialCollectionEligible) { 4 } else { 0 }
	}
	if ($SpatialSuccessfulCollections -lt 0) { $SpatialSuccessfulCollections = if ($spatialCollectionEligible) { 4 } else { 0 } }
	if ($SpatialMaximumCollectionQueries -lt 0) { $SpatialMaximumCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 5 } else { 0 } }
	if ($SpatialMaximumCollectionRanges -lt 0) {
		$SpatialMaximumCollectionRanges = if ($SpatialSuccessfulCollections -gt 0) {
			[Math]::Min($Workers, $SpatialMaximumCollectionQueries)
		} else { 0 }
	}
	if ($SpatialSuccessfulCollectionQueries -lt 0) { $SpatialSuccessfulCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 20 } else { 0 } }
	if ($SpatialSuccessfulCollectionRanges -lt 0) {
		$SpatialSuccessfulCollectionRanges = $SpatialSuccessfulCollections *
			$SpatialMaximumCollectionRanges
	}
	if ($SpatialMultiRangeCollections -lt 0) { $SpatialMultiRangeCollections = $SpatialSuccessfulCollections }
	if ($SpatialCollectionSubmitted -lt 0) { $SpatialCollectionSubmitted = 2 * $SpatialSuccessfulCollectionRanges }
	if ($SpatialCollectionCompleted -lt 0) { $SpatialCollectionCompleted = $SpatialCollectionSubmitted }
	if ($SpatialCollectionPhysical -lt 0) { $SpatialCollectionPhysical = $SpatialCollectionCompleted }
	if ($SpatialMaximumCollectionDistinctPhysicalWorkers -lt 0) {
		$SpatialMaximumCollectionDistinctPhysicalWorkers = if ($SpatialSuccessfulCollections -gt 0) {
			if ($SpatialMaximumCollectionRanges -ge 4) { 2 } else { 1 }
		} else { 0 }
	}
	if ($SpatialCollectionPhysicalWorkerMask -lt 0) {
		$SpatialCollectionPhysicalWorkerMask = if ($SpatialMaximumCollectionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $SpatialMaximumCollectionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($SpatialHealingAuthoritative -lt 0) { $SpatialHealingAuthoritative = if ($spatialEligible) { 3 } else { 0 } }
	if ($SpatialHealingCandidates -lt 0) { $SpatialHealingCandidates = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingSubmitted -lt 0) { $SpatialHealingSubmitted = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingCompleted -lt 0) { $SpatialHealingCompleted = $SpatialHealingSubmitted }
	if ($SpatialHealingPhysical -lt 0) { $SpatialHealingPhysical = $SpatialHealingCompleted }
	if ($SpatialPdlAuthoritative -lt 0) { $SpatialPdlAuthoritative = if ($spatialEligible) { 2 } else { 0 } }
	if ($SpatialPdlCandidates -lt 0) { $SpatialPdlCandidates = if ($SpatialPdlAuthoritative -gt 0) { 5 } else { 0 } }
	if ($SpatialPdlSubmitted -lt 0) { $SpatialPdlSubmitted = if ($SpatialPdlAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialPdlCompleted -lt 0) { $SpatialPdlCompleted = $SpatialPdlSubmitted }
	if ($SpatialPdlPhysical -lt 0) { $SpatialPdlPhysical = $SpatialPdlCompleted }
	if ($SpatialHealingExpectedFallbacks -lt 0) {
		$SpatialHealingExpectedFallbacks = if ($Mode -ceq 'serial' -or $Workers -le 1) { 2 } else { 0 }
	}
	if ($SpatialPdlExpectedFallbacks -lt 0) {
		$SpatialPdlExpectedFallbacks = if ($Mode -ceq 'serial' -or $Workers -le 1) { 2 } else { 0 }
	}
    return ('SIMULATION_JOB_METRICS replay="Stage5Validation\reference.rep" ' +
        "requested_mode=$Mode effective_mode=$EffectiveMode requested_pipeline=serial effective_pipeline=serial " +
        "scheduler_started=$Scheduler workers=$Workers submitted=$Submitted executed=$Executed steals=0 owner_help=0 " +
        "waits=0 worker_wait_rejections=0 failures=0 cancelled=0 fallback=$Fallback queue_latency_ns=1 " +
        "max_queue_latency_ns=1 sleeps=0 wakes=0 affinity_failures=0 queue_high_water=1 peak_active_workers=$Workers " +
        "available_cpus=16 reserved_owner_cpus=1 selected_worker_cpus=$Workers") + "`n" +
        ("COLLISION_CANDIDATE_MANIFEST authoritative_commits=$CollisionAuthoritativeCommits shadow_executions=$CollisionShadowExecutions " +
        "shadow_compared_candidates=$CollisionShadowComparedCandidates " +
        "shadow_mismatches=$CollisionShadowMismatches owner_fallbacks=$CollisionOwnerFallbacks " +
        "unexpected_fallbacks=$CollisionUnexpectedFallbacks ineligible_slices=$CollisionIneligibleSlices stale_rejections=0 " +
		"committed_candidates=$CollisionCommittedCandidates prepared_pairs=$CollisionPreparedPairs unique_candidates=$CollisionUniqueCandidates submitted_jobs=$CollisionSubmitted completed_jobs=$CollisionCompleted") + "`n" +
		("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=$PhysicsAuthoritativeBatches committed_prefixes=$PhysicsCommittedPrefixes ranges=$PhysicsRanges " +
		"submitted_jobs=$PhysicsSubmitted completed_jobs=$PhysicsCompleted allocated_bytes=$physicsAllocatedBytes capture_ns=$physicsCaptureNanoseconds prepare_ns=$physicsPrepareNanoseconds " +
		"wait_ns=$physicsWaitNanoseconds commit_ns=$physicsCommitNanoseconds storage_bytes=$physicsStorageBytes storage_capacity_bytes=$physicsStorageCapacityBytes " +
		"storage_allocations=$physicsStorageAllocations shadow_executions=$PhysicsShadowExecutions " +
		"shadow_prefixes=$PhysicsShadowPrefixes shadow_ranges=$PhysicsShadowRanges " +
		"shadow_submitted_jobs=$PhysicsShadowSubmitted shadow_completed_jobs=$PhysicsShadowCompleted " +
		"shadow_matches=$($PhysicsShadowExecutions - $PhysicsShadowMismatches) " +
		"shadow_mismatches=$PhysicsShadowMismatches owner_fallbacks=0 ineligible_slices=2 " +
		"unexpected_fallbacks=$PhysicsUnexpectedFallbacks stale_rejections=0 circuit_breaker_trips=0") + "`n" +
		("IMMUTABLE_SPATIAL_MANIFEST captured_arenas=$SpatialCapturedArenas capture_failures=$SpatialCaptureFailures " +
		"successful_collections=$SpatialSuccessfulCollections " +
		"successful_collection_queries=$SpatialSuccessfulCollectionQueries " +
		"successful_collection_ranges=$SpatialSuccessfulCollectionRanges " +
		"multi_range_collections=$SpatialMultiRangeCollections " +
		"collection_submitted_jobs=$SpatialCollectionSubmitted collection_completed_jobs=$SpatialCollectionCompleted " +
		"collection_physical_worker_jobs=$SpatialCollectionPhysical collection_owner_helped_jobs=$SpatialCollectionOwnerHelped " +
		"collection_physical_worker_mask=$SpatialCollectionPhysicalWorkerMask " +
		"maximum_collection_queries=$SpatialMaximumCollectionQueries maximum_collection_ranges=$SpatialMaximumCollectionRanges " +
		"maximum_collection_distinct_physical_workers=$SpatialMaximumCollectionDistinctPhysicalWorkers " +
		"healing_eligible_queries=$SpatialHealingEligible healing_authoritative_queries=$SpatialHealingAuthoritative " +
		"healing_authoritative_candidates=$SpatialHealingCandidates healing_shadow_queries=0 " +
		"healing_shadow_matches=0 healing_shadow_mismatches=0 " +
		"healing_submitted_jobs=$SpatialHealingSubmitted healing_completed_jobs=$SpatialHealingCompleted " +
		"healing_physical_worker_jobs=$SpatialHealingPhysical healing_owner_helped_jobs=$SpatialHealingOwnerHelped " +
		"healing_expected_fallbacks=$SpatialHealingExpectedFallbacks healing_unexpected_fallbacks=$SpatialHealingUnexpectedFallbacks " +
		"healing_stale_rejections=0 healing_validation_failures=0 healing_circuit_breaker_trips=0 " +
		"pdl_eligible_queries=$SpatialPdlEligible pdl_authoritative_queries=$SpatialPdlAuthoritative " +
		"pdl_authoritative_candidates=$SpatialPdlCandidates pdl_shadow_queries=0 " +
		"pdl_shadow_matches=0 pdl_shadow_mismatches=0 " +
		"pdl_submitted_jobs=$SpatialPdlSubmitted pdl_completed_jobs=$SpatialPdlCompleted " +
		"pdl_physical_worker_jobs=$SpatialPdlPhysical pdl_owner_helped_jobs=$SpatialPdlOwnerHelped " +
		"pdl_expected_fallbacks=$SpatialPdlExpectedFallbacks pdl_unexpected_fallbacks=$SpatialPdlUnexpectedFallbacks " +
		"pdl_stale_rejections=0 pdl_validation_failures=0 pdl_circuit_breaker_trips=0")
}

function New-ReplayResultOutput {
    param([int]$FinalFrame = 42000, [string]$FinalCRC = '01020304')
    return 'SIMULATION_REPLAY_RESULT replay="Stage5Validation\reference.rep" ' +
        "final_frame=$FinalFrame final_crc=$FinalCRC"
}

function Write-TimingFixture {
    param([string]$Path, [switch]$HeaderOnly, [switch]$BadHeader, [switch]$MissingLogic,
        [switch]$Interactive, [switch]$CollisionPhases, [switch]$CollisionShadowPhase,
        [string]$WallMilliseconds = '100.000')
    $header = 'session,mode,frame_begin,frame_end,logic_frames,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms'
    if ($BadHeader) { $header = $header.Replace('frame_end', 'last_frame') }
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add($header) | Out-Null
    if (-not $HeaderOnly) {
        $mode = if ($Interactive) { 'interactive' } else { 'headless' }
        $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,frame,42000,90.000,0.0021,0.0040,0.0080,0.0200,0,0") | Out-Null
        if (-not $MissingLogic) {
            $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,logic,42000,80.000,0.0019,0.0040,0.0080,0.0200,0,0") | Out-Null
        }
        if ($CollisionPhases) {
            foreach ($phase in @('collision_admission', 'simulation_snapshot',
                'simulation_parallel', 'simulation_wait', 'simulation_reduce',
                'collision_live_validation', 'simulation_commit')) {
                $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,$phase,1,1.000,1.000,1.000,1.000,1.000,0,0") | Out-Null
            }
        }
        if ($CollisionShadowPhase) {
            foreach ($phase in @('collision_existing_filter',
                'collision_commit_prepare', 'simulation_shadow_compare')) {
                $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,$phase,1,1.000,1.000,1.000,1.000,1.000,0,0") | Out-Null
            }
        }
    }
    [IO.File]::WriteAllLines($Path, $lines.ToArray())
}

function New-PerformanceResult {
    param([string]$Configuration, [int]$Sequence, [double]$WallMilliseconds, [int]$Workers,
        [int]$AvailableCpus = 16)
    return [pscustomobject]@{
        kind = 'replay'; stress = $true; configuration = $Configuration; sequence = $Sequence
        wallMilliseconds = $WallMilliseconds
        replayMetrics = [pscustomobject]@{
            workers = $Workers; availableCpus = $AvailableCpus; selectedWorkerCpus = $Workers
        }
    }
}

$scriptPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $temporaryBase ('GGC-Stage5Validation-Test-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $runtime = Join-Path $root 'runtime'
    $fixtures = Join-Path $root 'fixtures'
    New-Item -ItemType Directory -Path $runtime | Out-Null
    New-Item -ItemType Directory -Path $fixtures | Out-Null
    [IO.File]::WriteAllText((Join-Path $runtime 'generalszh.exe'), 'installed candidate fixture')
    [IO.File]::WriteAllText((Join-Path $runtime 'launcher.exe'), 'launcher fixture')
    [IO.File]::WriteAllText((Join-Path $runtime 'launcher.lcf'), 'RUN = . generalszh.exe')
    [IO.File]::WriteAllText((Join-Path $fixtures 'reference.rep'), 'reference replay fixture')
    [IO.File]::WriteAllText((Join-Path $fixtures 'hard-ai-2v6.rep'), 'stress replay fixture')
    $executableHash = Get-Sha256 (Join-Path $runtime 'generalszh.exe')
    $referenceHash = Get-Sha256 (Join-Path $fixtures 'reference.rep')
    $stressHash = Get-Sha256 (Join-Path $fixtures 'hard-ai-2v6.rep')
    $manifest = Join-Path $root 'manifest.json'
    Write-TestManifest $manifest $executableHash $referenceHash $stressHash

    $planOutput = Join-Path $root 'plan-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $planOutput `
        -ValidationSet All -ReplayMatrixRepeats 2 -StressRepeats 3 -MinimumFreeBytes 1 `
        -AllowNonStandardCorpus -PlanOnly | Out-Null
    $plan = Get-Content -LiteralPath (Join-Path $planOutput 'validation-plan.json') -Raw | ConvertFrom-Json
    Assert-True (-not $plan.deterministicRuntimeEligible -and
        -not $plan.finalAcceptanceEligible) `
        'nonstandard corpus plans are ineligible for the deterministic-runtime and final gates'
    Assert-True ($plan.entries.Count -eq 141) `
        'two-pass replay, three-seed repeated AI matrix, and one shadow stress run have 141 planned runs'
    Assert-True (@($plan.entries | Where-Object { $_.kind -ceq 'replay' }).Count -eq 56) `
        'replay matrix covers seven configurations, two passes, and three stress runs'
    Assert-True (@($plan.entries | Where-Object { $_.kind -ceq 'ai' }).Count -eq 85) `
        'AI matrix covers both scenarios, three seeds, repeats, seven regular configurations, and one shadow stress run'
    $shadowEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'shadow-16' })
    Assert-True ($shadowEntries.Count -eq 1 -and $shadowEntries[0].kind -ceq 'ai' -and
        $shadowEntries[0].stress -and $shadowEntries[0].scenario -ceq '4v2' -and
        $shadowEntries[0].simulationMode -ceq 'shadow' -and
        $shadowEntries[0].requestedWorkers -ceq '16') `
        'installed validation plan contains exactly one 16-worker 4v2 collision shadow stress execution'
    $autoEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'parallel-auto' })
    Assert-True ($autoEntries.Count -gt 0) 'automatic worker configuration is present'
    Assert-True (@($autoEntries | Where-Object { $_.arguments -contains '-workerCount' }).Count -eq 0) `
        'automatic worker configuration omits -workerCount'
    $explicitEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'parallel-16' })
    Assert-True (@($explicitEntries | Where-Object { $_.arguments -contains '-workerCount' }).Count -eq $explicitEntries.Count) `
        'explicit worker configurations include -workerCount'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-validationExecutableSha256' }).Count -eq 0) `
        'every run supplies executable provenance'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-pipelineMode' }).Count -eq 0) `
        'every run holds Stage 4 pipeline mode constant'
    Assert-True (@($plan.entries | Where-Object {
        $index = [Array]::IndexOf([object[]]$_.arguments, '-pipelineMode')
        $index -lt 0 -or $_.arguments[$index + 1] -cne 'serial'
    }).Count -eq 0) 'every replay and AI run honestly requests the serial pipeline'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-simulationMode' }).Count -eq 0) `
        'every run explicitly selects simulation mode'
    $shadowModeIndex = [Array]::IndexOf([object[]]$shadowEntries[0].arguments, '-simulationMode')
    Assert-True ($shadowModeIndex -ge 0 -and
        $shadowEntries[0].arguments[$shadowModeIndex + 1] -ceq 'shadow') `
        'shadow stress execution passes simulationMode=shadow to the installed runtime'

    $standardManifest = Join-Path $root 'standard-manifest.json'
    Write-StandardTestManifest $standardManifest $executableHash $fixtures
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $standardManifest `
            -OutputRoot (Join-Path $root 'one-pass-all-output') -ValidationSet All `
            -ReplayMatrixRepeats 1 -StressRepeats 3 -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly two complete replay matrix passes' `
        'the deterministic-runtime gate cannot reduce the replay matrix to one pass'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $standardManifest `
            -OutputRoot (Join-Path $root 'one-stress-all-output') -ValidationSet All `
            -ReplayMatrixRepeats 2 -StressRepeats 1 -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly three executions of the stress replay' `
        'the deterministic-runtime gate cannot reduce the stress replay execution count'
    $functionalOnlyOutput = Join-Path $root 'functional-only-plan-output'
    $functionalOnlyStdout = @(& $scriptPath -RuntimeRoot $runtime `
        -FixtureManifestPath $standardManifest -OutputRoot $functionalOnlyOutput `
        -ValidationSet All -MinimumFreeBytes 1 -PlanOnly) -join "`n"
    $functionalOnlyPlan = Get-Content -LiteralPath `
        (Join-Path $functionalOnlyOutput 'validation-plan.json') -Raw | ConvertFrom-Json
    Assert-True (-not $functionalOnlyPlan.deterministicRuntimeEligible -and
        -not $functionalOnlyPlan.finalAcceptanceEligible -and
        -not $functionalOnlyPlan.performanceRequested -and
        $functionalOnlyPlan.performanceRequiredForDeterministicRuntimeGate) `
        'an All matrix without enforced Stage 3 performance evidence is not a passing deterministic-runtime gate'
    Assert-True (@($functionalOnlyPlan.entries | Where-Object { $_.kind -ceq 'replay' }).Count -eq 168) `
        'the focused functional plan still proves the exact 24-execution replay matrix for all seven configurations'
    Assert-True ($functionalOnlyStdout -match 'focused/diagnostic deterministic-runtime' -and
        $functionalOnlyStdout -notmatch '\bpassed\b') `
        'an All plan without performance prints an explicit focused result and never a passed banner'

    $aiOnlyOutput = Join-Path $root 'ai-only-plan-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $aiOnlyOutput `
        -ValidationSet AI -MinimumFreeBytes 1 -PlanOnly | Out-Null
    $aiOnlyPlan = Get-Content -LiteralPath (Join-Path $aiOnlyOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True ($aiOnlyPlan.validationSet -ceq 'AI' -and
        -not $aiOnlyPlan.deterministicRuntimeEligible -and
        -not $aiOnlyPlan.finalAcceptanceEligible) `
        'AI-only plan with a two-fixture structural manifest remains a focused partial gate'

    $replayOnlyOutput = Join-Path $root 'replay-only-plan-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $replayOnlyOutput `
        -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    $replayOnlyPlan = Get-Content -LiteralPath (Join-Path $replayOnlyOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True ($replayOnlyPlan.validationSet -ceq 'Replay' -and
        -not $replayOnlyPlan.deterministicRuntimeEligible -and
        -not $replayOnlyPlan.finalAcceptanceEligible) `
        'Replay-only plan remains a focused partial gate'

    $oneSeedManifest = Join-Path $root 'one-seed-all.json'
    Write-TestManifest $oneSeedManifest $executableHash $referenceHash $stressHash `
        -Seeds @(1729)
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $oneSeedManifest `
            -OutputRoot (Join-Path $root 'one-seed-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'at least three distinct' `
        'ValidationSet All rejects a reduced one-seed live-AI matrix'

    $oneScenarioManifest = Join-Path $root 'one-scenario-all.json'
    Write-TestManifest $oneScenarioManifest $executableHash $referenceHash $stressHash `
        -Scenarios @('4v2')
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $oneScenarioManifest `
            -OutputRoot (Join-Path $root 'one-scenario-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'both the 4v3 and 4v2' `
        'ValidationSet All rejects a matrix missing the mandatory 4v3 scenario'

    $duplicateSeedManifest = Join-Path $root 'duplicate-seed-all.json'
    Write-TestManifest $duplicateSeedManifest $executableHash $referenceHash $stressHash `
        -Seeds @(1729, 1729, 1730)
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $duplicateSeedManifest `
            -OutputRoot (Join-Path $root 'duplicate-seed-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'seeds must be distinct' `
        'duplicate seed entries cannot disguise a three-seed acceptance matrix'

    $badHashManifest = Join-Path $root 'bad-hash.json'
    Write-TestManifest $badHashManifest ('0' * 64) $referenceHash $stressHash
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $badHashManifest `
            -OutputRoot (Join-Path $root 'bad-hash-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'SHA-256 mismatch' 'candidate hash mismatch fails closed'

    $overrideOutput = Join-Path $root 'hash-override-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $badHashManifest `
        -OutputRoot $overrideOutput -ValidationSet Replay -MinimumFreeBytes 1 `
        -ExpectedExecutableSha256 $executableHash -AllowNonStandardCorpus -PlanOnly | Out-Null
    $overridePlan = Get-Content -LiteralPath (Join-Path $overrideOutput 'validation-plan.json') `
        -Raw | ConvertFrom-Json
    Assert-True ($overridePlan.executableSha256 -ceq $executableHash) `
        'controller hash override records the exact installed candidate hash'
    Assert-True ($overridePlan.executableSha256Source -ceq 'argument') `
        'validation plan identifies the controller hash source'

    $escapeFile = Join-Path (Split-Path -Parent $root) 'stage5-escape.rep'
    [IO.File]::WriteAllText($escapeFile, 'escape fixture')
    try {
        $escapeHash = Get-Sha256 $escapeFile
        $escapeManifest = Join-Path $root 'escape.json'
        Write-TestManifest $escapeManifest $executableHash $escapeHash $stressHash '..\stage5-escape.rep'
        Assert-Throws {
            & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $escapeManifest `
                -OutputRoot (Join-Path $root 'escape-output') -ValidationSet Replay `
                -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
        } 'escapes the manifest directory' 'fixture traversal fails closed'
    }
    finally {
        if (Test-Path -LiteralPath $escapeFile) { Remove-Item -LiteralPath $escapeFile -Force }
    }

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'standard-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly 10 fixtures' 'standard gate rejects an incomplete corpus'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'nonstandard-acceptance-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus | Out-Null
    } 'limited to PlanOnly or DiagnosticNonAcceptance' `
        'nonstandard replay corpus cannot execute as an accepting gate'

    $duplicateSourceManifest = Join-Path $root 'duplicate-source.json'
    $duplicateDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $duplicateDocument.fixtures[1].source = $duplicateDocument.fixtures[0].source
    $duplicateDocument.fixtures[1].sha256 = $duplicateDocument.fixtures[0].sha256
    [IO.File]::WriteAllText($duplicateSourceManifest, ($duplicateDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $duplicateSourceManifest `
            -OutputRoot (Join-Path $root 'duplicate-source-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'unique source SHA-256' 'different fixture ids cannot disguise duplicate replay content'

    $caseCollisionManifest = Join-Path $root 'case-collision.json'
    $caseCollisionDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $caseCollisionDocument.fixtures[1].id = 'REFERENCE'
    [IO.File]::WriteAllText($caseCollisionManifest,
        ($caseCollisionDocument | ConvertTo-Json -Depth 8))
    $caseCollisionOutput = Join-Path $root 'case-collision-output'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $caseCollisionManifest `
            -OutputRoot $caseCollisionOutput -ValidationSet Replay -MinimumFreeBytes 1 `
            -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'collides case-insensitively' `
        'distinct replay hashes cannot hide case-colliding fixture IDs on Windows'
    Assert-True (-not (Test-Path -LiteralPath $caseCollisionOutput)) `
        'case-colliding replay IDs fail before evidence/profile creation begins'

    $stringStressManifest = Join-Path $root 'string-stress.json'
    $stringStressDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $stringStressDocument.fixtures[0].stress = 'false'
    [IO.File]::WriteAllText($stringStressManifest, ($stringStressDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $stringStressManifest `
            -OutputRoot (Join-Path $root 'string-stress-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'must be a JSON boolean' 'string stress values cannot be coerced into true'

    $extraPropertyManifest = Join-Path $root 'extra-property.json'
    $extraPropertyDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $extraPropertyDocument.fixtures[0] | Add-Member -NotePropertyName unexpected -NotePropertyValue 1
    [IO.File]::WriteAllText($extraPropertyManifest, ($extraPropertyDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $extraPropertyManifest `
            -OutputRoot (Join-Path $root 'extra-property-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'contains unsupported property' 'manifest objects reject properties outside the exact schema'

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $planOutput `
            -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'must not already exist' 'evidence directory reuse fails closed'

    $oneWorkerEntry = [pscustomobject]@{
        sequence = 1; configuration = 'parallel-1'; simulationMode = 'parallel'
        requestedWorkers = '1'; seed = 1729; scenario = '4v3'
    }
	$oneWorkerZeroPhysicsOutput = New-AiCompletionOutput `
		-RequestedWorkers '1' -EffectiveWorkers 1 `
            -Submitted 0 -Executed 0 -Fallback 7 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
			-PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0
	$oneWorkerEvidence = ConvertFrom-Stage5AiCompletion `
		$oneWorkerZeroPhysicsOutput $oneWorkerEntry ('A' * 64)
    Assert-True $oneWorkerEvidence.expectedOneWorkerFallback `
        'AI parser accepts the expected forced one-worker serial fallback'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			($oneWorkerZeroPhysicsOutput.Replace('physics_capture_ns=0',
				'physics_capture_ns=1')) $oneWorkerEntry ('A' * 64) | Out-Null
	} 'reports physics pre-scan, capture, or storage work' `
		'forced one-worker AI evidence rejects physics capture work before scheduler preflight'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -PathWorkerExecuted 2 -PathAuthoritativeCommits 1 `
                -PathPeakWorkers 1 -CollisionAuthoritativeCommits 0 `
                -CollisionCommittedCandidates 0 -PhysicsAuthoritativeBatches 0 `
                -PhysicsCommittedPrefixes 0 -PhysicsRanges 0 `
                -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'nonqualifying serial, one-worker, or non-parallel lane reports direct-path' `
        'one-worker AI evidence rejects stale direct-path batch work and authority'
    $oneWorkerCollisionFallbackEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
            -Submitted 0 -Executed 0 -Fallback 7 -CollisionOwnerFallbacks 3 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
        $oneWorkerEntry ('A' * 64)
    Assert-True ($oneWorkerCollisionFallbackEvidence.collisionOwnerFallbacks -eq 3) `
        'AI parser accepts owner-only collision fallback in the forced one-worker lane'
    $twoWorkerEntry = [pscustomobject]@{
        sequence = 2; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v3'
    }
    $scalarSpatialCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -SpatialCapturedArenas 0 `
            -SpatialSuccessfulCollections 0 `
            -SpatialSuccessfulCollectionQueries 0 `
            -SpatialSuccessfulCollectionRanges 0 `
            -SpatialMultiRangeCollections 0 -SpatialCollectionSubmitted 0 `
            -SpatialCollectionCompleted 0 -SpatialCollectionPhysical 0 `
            -SpatialMaximumCollectionQueries 0 -SpatialMaximumCollectionRanges 0 `
            -SpatialHealingEligible 1 -SpatialHealingExpectedFallbacks 1 `
            -SpatialHealingAuthoritative 0 -SpatialHealingCandidates 0 `
            -SpatialHealingSubmitted 0 -SpatialHealingCompleted 0 `
            -SpatialHealingPhysical 0 -SpatialPdlEligible 0 `
            -SpatialPdlExpectedFallbacks 0 -SpatialPdlAuthoritative 0 `
            -SpatialPdlCandidates 0 -SpatialPdlSubmitted 0 `
            -SpatialPdlCompleted 0 -SpatialPdlPhysical 0) `
        $twoWorkerEntry ('A' * 64)
    Assert-True ($scalarSpatialCompletion.spatialEvidence.capturedArenas -eq 0 -and
        $scalarSpatialCompletion.spatialEvidence.healing.eligibleQueries -eq 1 -and
        $scalarSpatialCompletion.spatialEvidence.healing.expectedFallbacks -eq 1 -and
        $scalarSpatialCompletion.spatialEvidence.healing.unexpectedFallbacks -eq 0 -and
        $scalarSpatialCompletion.spatialEvidence.healing.staleRejections -eq 0) `
        'singleton spatial preflight publishes one expected policy fallback with no arena capture or stale failure'
    $serialEntry = [pscustomobject]@{
        sequence = 60; configuration = 'serial-1'; simulationMode = 'serial'
        requestedWorkers = '1'; seed = 1729; scenario = '4v3'
    }
    $serialCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 0 `
            -Submitted 0 -Executed 0 -AuthoritativeCommits 0 -AiCommittedBatches 1 `
            -AiSubmitted 0 -AiCompleted 0) $serialEntry ('A' * 64)
    Assert-True ($serialCompletion.aiCommittedBatches -eq 1 -and
        $serialCompletion.aiParallelAuthoritativeCommits -eq 0 -and
        $serialCompletion.spatialEvidence.capturedArenas -eq 0) `
        'serial AI evidence preserves generic owner commits without claiming parallel or spatial authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialCapturedArenas 0) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'has no captured immutable-spatial arena' `
        'parallel AI evidence rejects an all-zero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialSuccessfulCollections 1 `
                -SpatialMultiRangeCollections 1 `
                -SpatialSuccessfulCollectionQueries 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'does not prove multi-query, multi-range two-pass worker execution' `
        'AI evidence rejects a nominal spatial collection that did not batch multiple queries'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialCollectionOwnerHelped 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collection reports owner help' `
        'AI spatial collection evidence requires physical workers only'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -SpatialCapturedArenas 1 -SpatialSuccessfulCollections 1 `
                -SpatialMultiRangeCollections 1 `
                -SpatialSuccessfulCollectionQueries 2 `
                -SpatialSuccessfulCollectionRanges 2 `
                -SpatialCollectionSubmitted 4 -SpatialCollectionCompleted 4 `
                -SpatialCollectionPhysical 4 -SpatialMaximumCollectionQueries 2 `
                -SpatialMaximumCollectionRanges 2) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'nonqualifying serial/one-worker lane reports immutable-spatial collection' `
        'one-worker AI evidence rejects stale spatial collection worker authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiCommittedBatches 1 `
                -AiSubmitted 0 -AiCompleted 0 -SpatialCapturedArenas 1) `
            $serialEntry ('A' * 64) | Out-Null
    } 'serial simulation reports captured immutable-spatial arenas' `
        'serial AI evidence rejects a nonzero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 0 `
                -Submitted 0 -Executed 0 -AuthoritativeCommits 1 -AiSubmitted 1 -AiCompleted 1) `
            $serialEntry ('A' * 64) | Out-Null
    } 'AI owner authority outside parallel simulation' `
        'serial AI evidence rejects stale authoritative AI commits from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -ShadowExecutions 1) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'AI shadow work outside shadow simulation' `
        'parallel AI evidence rejects stale AI shadow counters from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -AuthoritativeCommits 1 -AiCommittedBatches 1 `
                -AiParallelAuthoritativeCommits 0) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'does not match the mode-specific AI parallel-authority counter' `
        'generic AI owner commits cannot proxy the mode-specific parallel authority field'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '2' -EffectiveWorkers 0 -Submitted 0 -Executed 0 -Fallback 7) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'effective worker count does not match' 'AI parser rejects fallback outside the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 0 -Submitted 0 -Executed 0 -Fallback 7) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'effective worker count does not match' `
        'parallel-1 AI completion requires its live one-worker scheduler'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '4' -EffectiveWorkers 1) `
            ([pscustomobject]@{
                sequence = 5; configuration = 'parallel-4'; simulationMode = 'parallel'
                requestedWorkers = '4'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'effective worker count does not match' `
        'parallel-4 AI completion cannot pass with one effective worker'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 1 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0) `
            ([pscustomobject]@{
                sequence = 6; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'serial configuration reports active workers or jobs' `
        'serial AI completion cannot report an active worker or jobs'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v3' -ActualAi 7 -ActualTeams '4v3') `
            ([pscustomobject]@{
                sequence = 3; configuration = 'parallel-2'; simulationMode = 'parallel'
                requestedWorkers = '2'; seed = 1729; scenario = '4v2'
            }) ('A' * 64) | Out-Null
    } 'scenario does not match' '4v3 completion evidence cannot satisfy a planned 4v2 scenario'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v3') `
            ([pscustomobject]@{
                sequence = 4; configuration = 'parallel-2'; simulationMode = 'parallel'
                requestedWorkers = '2'; seed = 1729; scenario = '4v2'
            }) ('A' * 64) | Out-Null
    } 'actual_teams does not match' 'AI completion team shape must match the planned scenario'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Seed 1729 -LoadedSeed 1730) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'loaded_seed does not match' `
        'planned seed echo cannot conceal a different seed loaded into the live match'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedPipeline parallel -EffectivePipeline parallel) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'honestly request the serial pipeline' `
        'AI completion with a live parallel pipeline cannot satisfy the serial validation plan'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionShadowMismatches 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collision shadow mismatches' `
        'AI completion rejects collision adapter/legacy ordering mismatches'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionUnexpectedFallbacks 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'unexpected collision owner fallbacks' `
        'AI completion rejects unexpected collision fallback publication failures'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0 `
                -CollisionAuthoritativeCommits 1 -CollisionCommittedCandidates 1 `
                -CollisionPreparedPairs 1 -CollisionUniqueCandidates 1 `
                -CollisionSubmitted 1 -CollisionCompleted 1) `
            ([pscustomobject]@{
                sequence = 61; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'collision authority outside parallel' `
        'serial AI evidence rejects stale collision authority from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0 `
                -CollisionPreparedPairs 24 -CollisionUniqueCandidates 12 `
                -CollisionSubmitted 4 -CollisionCompleted 4) `
            ([pscustomobject]@{
                sequence = 63; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'serial simulation reports collision lane work' `
        'serial AI evidence rejects stale prepared collision work even with zero authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -CollisionPreparedPairs 4 -CollisionUniqueCandidates 2 `
                -CollisionSubmitted 1 -CollisionCompleted 1) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'one-worker ineligible simulation reports collision prepared' `
        'one-worker collision-ineligible AI evidence rejects stale prepared work'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionShadowExecutions 1 `
                -CollisionShadowComparedCandidates 1) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collision shadow work outside shadow' `
        'parallel AI evidence rejects stale collision shadow counters from another mode'
    $shadowEntry = [pscustomobject]@{
        sequence = 7; configuration = 'shadow-16'; simulationMode = 'shadow'
        requestedWorkers = '16'; seed = 1729; scenario = '4v2'
    }
    $shadowCompletionArguments = @{
        Mode = 'shadow'; RequestedWorkers = '16'; EffectiveWorkers = 16
        Scenario = '4v2'; ActualAi = 6; ActualTeams = '4v2'
        AuthoritativeCommits = 0; AiCommittedBatches = 5; ShadowExecutions = 5
        CollisionAuthoritativeCommits = 0; CollisionShadowExecutions = 3
        CollisionCommittedCandidates = 0; CollisionPreparedPairs = 24
        CollisionUniqueCandidates = 12; CollisionSubmitted = 4; CollisionCompleted = 4
        PathWorkerExecuted = 0; PathAuthoritativeCommits = 0
        PhysicsShadowExecutions = 3
    }
    $shadowCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput @shadowCompletionArguments) $shadowEntry ('A' * 64)
    Assert-True ($shadowCompletion.collisionShadowExecutions -eq 3 -and
        $shadowCompletion.collisionSubmittedJobs -eq 4 -and
        $shadowCompletion.aiCommittedBatches -eq 5 -and
        $shadowCompletion.aiParallelAuthoritativeCommits -eq 0) `
        'shadow evidence preserves generic AI commits and collision work without claiming parallel AI authority'
    Assert-Throws {
        $shadowAuthority = @{} + $shadowCompletionArguments
        $shadowAuthority.AuthoritativeCommits = 1
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @shadowAuthority) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'AI owner authority outside parallel simulation' `
        'shadow AI evidence rejects stale authoritative AI commits from the parallel mode'
    Assert-Throws {
        $missingShadowComparison = @{} + $shadowCompletionArguments
        $missingShadowComparison.CollisionShadowExecutions = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @missingShadowComparison) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass without an executed collision comparison'
    Assert-Throws {
        $missingShadowJobs = @{} + $shadowCompletionArguments
        $missingShadowJobs.CollisionSubmitted = 0
        $missingShadowJobs.CollisionCompleted = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @missingShadowJobs) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass on global scheduler jobs without collision jobs'
    Assert-Throws {
        $vacuousShadowComparison = @{} + $shadowCompletionArguments
        $vacuousShadowComparison.CollisionShadowComparedCandidates = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @vacuousShadowComparison) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass when every prepared pair was already present and no insertion was compared'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathUnsupportedAuthority 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_unsupported_authority' `
		'direct-path authority is forbidden in unsupported runtime policy'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathStaleAcceptance 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_stale_acceptance' `
		'stale direct-path output can never satisfy owner authority'
    Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 1 -PathAuthoritativeCommits 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'not backed by physical-worker execution' `
		'owner-help or global jobs cannot proxy direct-path physical-worker authority'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'more multi-worker direct-path commits than authoritative commits' `
		'multi-worker correlation can only be published by an actual authoritative path commit'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 1 `
				-PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 0 -PathPeakWorkers 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'authority from an impossible single-request batch' `
		'direct-path authority requires the runtime minimum of two submitted and worker-executed requests'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 2 `
				-PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 1 -PathPeakWorkers 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'multi-worker direct-path authority without a multi-worker peak' `
		'multi-worker correlation requires an observed path-local peak above one'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathOwnerHelped 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'physical-worker-only' `
		'owner help is never accepted as bounded direct-path batch execution'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathPeakWorkers 3) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'impossible direct-path active-worker count' `
		'path-local peak cannot exceed the effective physical-worker count'
	ConvertFrom-Stage5AiCompletion `
		(New-AiCompletionOutput -PathLateDrains 1) $twoWorkerEntry ('A' * 64) | Out-Null
	Assert-True $true 'late direct-path drains remain diagnostic and do not enter executed or authority identities'
	ConvertFrom-Stage5AiCompletion `
		(New-AiCompletionOutput -PathLateDrains 30) $twoWorkerEntry ('A' * 64) | Out-Null
	Assert-True $true 'late direct-path drains may finish after the manifest and never decide acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathTimeouts 1) $twoWorkerEntry ('A' * 64) | Out-Null
	} 'synchronous direct-path watchdog timeouts' `
		'a terminal-frame direct-path timeout deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathValidationFailures 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_validation_failures' `
		'an eligible direct-path validation failure deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsShadowExecutions 1 -PhysicsShadowMismatches 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden physics evidence' 'AI completion rejects physics shadow divergence'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsUnexpectedFallbacks 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden physics evidence' 'AI completion rejects unexpected physics fallback'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialHealingSubmitted 8 `
				-SpatialHealingCompleted 7 -SpatialHealingPhysical 7) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'healing immutable-spatial jobs are not balanced' `
		'AI completion rejects incomplete healing spatial jobs'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialHealingShadow 1 `
				-SpatialHealingShadowMismatches 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden healing immutable-spatial evidence' `
		'AI completion rejects healing spatial shadow divergence'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialPdlUnexpectedFallbacks 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden pdl immutable-spatial evidence' `
		'AI completion rejects unexpected PDL spatial fallback'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsShadowExecutions 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'physics shadow work outside shadow' `
		'parallel AI evidence rejects stale physics shadow counters from another mode'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -CollisionSubmitted 4 -CollisionCompleted 3) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'collision submitted/completed job counts do not match' `
		'collision evidence rejects incomplete successful-job telemetry'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -OmitWorkEvidence) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'missing required authoritative Stage 5 work evidence' `
        'acceptance parsing fails closed when slice-specific work evidence is absent'
    $focusedWithoutWork = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -OmitWorkEvidence) $twoWorkerEntry ('A' * 64) $false
    Assert-True ($focusedWithoutWork.authoritativeWorkStatus -ceq 'unavailable-non-acceptance') `
        'focused non-acceptance parsing records unavailable slice metrics without claiming acceptance'

    $validAiResults = @(
        (New-AiResult 'serial-1' 1), (New-AiResult 'serial-1' 2),
        (New-AiResult 'parallel-1' 1), (New-AiResult 'parallel-1' 2)
    )
    Assert-Stage5AiDeterminism $validAiResults @('serial-1', 'parallel-1') 2
    $validAiWithShadow = @($validAiResults) + @((New-AiResult 'shadow-16' 1))
    Assert-Stage5AiDeterminism $validAiWithShadow @('serial-1', 'parallel-1') 2 'shadow-16'
    Assert-Throws {
        Assert-Stage5AiDeterminism $validAiResults @('serial-1', 'parallel-1') 2 'shadow-16'
    } 'exactly one' 'AI matrix cannot pass without its installed collision shadow stress result'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'DEADBEEF'))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'final_digest differs' 'AI digest mismatches cannot pass across configurations'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'A1B2C3D4' 42001))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'end_frame differs' 'AI end-frame mismatches cannot pass across configurations'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'A1B2C3D4' 42000 2))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'winner_team differs' 'AI winner mismatches cannot pass across configurations'
    Assert-Throws {
        Assert-Stage5AiDeterminism @($validAiResults[0..2]) @('serial-1', 'parallel-1') 2
    } 'expected 4' 'AI matrix cannot pass with a missing worker/repeat result'
    $completeCrossProduct = @()
    foreach ($determinismKey in @('4v3-seed-1729', '4v2-seed-1729')) {
        foreach ($configuration in @('serial-1', 'parallel-1')) {
            foreach ($repeat in @(1, 2)) {
                $completeCrossProduct += New-AiResult $configuration $repeat `
                    'A1B2C3D4' 42000 1 $determinismKey
            }
        }
    }
    Assert-Stage5AiDeterminism $completeCrossProduct @('serial-1', 'parallel-1') 2 '' `
        @('4v3-seed-1729', '4v2-seed-1729')
    Assert-Throws {
        Assert-Stage5AiDeterminism @($completeCrossProduct | Select-Object -Skip 4) `
            @('serial-1', 'parallel-1') 2 '' @('4v3-seed-1729', '4v2-seed-1729')
    } 'complete 8-result' `
        'a fully missing scenario/seed case cannot evade per-case determinism checks'
    Assert-Throws {
        $duplicateCrossProduct = @($completeCrossProduct)
        $duplicateCrossProduct[0] = $duplicateCrossProduct[1]
        Assert-Stage5AiDeterminism $duplicateCrossProduct @('serial-1', 'parallel-1') 2 '' `
            @('4v3-seed-1729', '4v2-seed-1729')
    } 'duplicate scenario/seed/configuration/repeat' `
        'duplicate results cannot fill a missing cross-product position'

    $stressEntry = [pscustomobject]@{
        sequence = 20; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v2'
    }
    $authoritativeStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2') `
        $stressEntry ('A' * 64)
    $spatialCollectionStressEntry = [pscustomobject]@{
        sequence = 24; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v2'; stress = $true
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -SpatialSuccessfulCollections 0 `
                -SpatialSuccessfulCollectionQueries 0 `
                -SpatialSuccessfulCollectionRanges 0 `
                -SpatialMultiRangeCollections 0 `
                -SpatialCollectionSubmitted 0 -SpatialCollectionCompleted 0 `
                -SpatialCollectionPhysical 0 `
                -SpatialMaximumCollectionQueries 0 `
                -SpatialMaximumCollectionRanges 0) `
            $spatialCollectionStressEntry ('A' * 64) | Out-Null
    } 'no positive multi-query, multi-range immutable-spatial collection evidence' `
        'qualifying AI stress cannot pass on single-query spatial worker submissions'
    foreach ($spatialLaneWorkers in @(2, 4, 8, 16)) {
        $spatialLaneEntry = [pscustomobject]@{
            sequence = 240 + $spatialLaneWorkers
            configuration = "parallel-$spatialLaneWorkers"
            simulationMode = 'parallel'
            requestedWorkers = [string]$spatialLaneWorkers
            seed = 1729
            scenario = '4v2'
            stress = $true
        }
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
                -ActualTeams '4v2' -RequestedWorkers ([string]$spatialLaneWorkers) `
                -EffectiveWorkers $spatialLaneWorkers) `
            $spatialLaneEntry ('A' * 64) | Out-Null
        $expectedSpatialRanges = [Math]::Min($spatialLaneWorkers, 5)
        $invalidSpatialRanges = if ($expectedSpatialRanges -eq 2) { 3 } else { 2 }
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion `
                (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
                    -ActualTeams '4v2' `
                    -RequestedWorkers ([string]$spatialLaneWorkers) `
                    -EffectiveWorkers $spatialLaneWorkers `
                    -SpatialMaximumCollectionRanges $invalidSpatialRanges) `
                $spatialLaneEntry ('A' * 64) | Out-Null
        } 'maximum collection ranges do not match min' `
            "parallel-$spatialLaneWorkers spatial evidence must scale ranges with workers and queueable queries"
		if ($expectedSpatialRanges -ge 4) {
			Assert-Throws {
				ConvertFrom-Stage5AiCompletion `
					(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
						-ActualTeams '4v2' `
						-RequestedWorkers ([string]$spatialLaneWorkers) `
						-EffectiveWorkers $spatialLaneWorkers `
						-SpatialCollectionPhysicalWorkerMask 1 `
						-SpatialMaximumCollectionDistinctPhysicalWorkers 1) `
					$spatialLaneEntry ('A' * 64) | Out-Null
			} 'did not use more than one distinct physical worker' `
				"parallel-$spatialLaneWorkers spatial evidence rejects a multi-range wave executed by one physical worker"
		}
    }
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
				-ActualTeams '4v2' -RequestedWorkers '2' -EffectiveWorkers 2 `
				-SpatialCollectionPhysicalWorkerMask 4 `
				-SpatialMaximumCollectionDistinctPhysicalWorkers 1) `
			$spatialCollectionStressEntry ('A' * 64) | Out-Null
	} 'physical-worker mask exceeds the explicit worker lane' `
		'physical spatial worker identities must remain inside the configured worker lane'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            $stressEntry ('A' * 64) | Out-Null
    } 'qualifying parallel stress has no positive authoritative physics' `
        'each qualifying parallel stress completion requires positive physics authority and jobs'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathPeakWorkers 1) $stressEntry ('A' * 64) | Out-Null
	} 'multi-worker direct-path authority without a multi-worker peak' `
		'per-record path authority rejects a sequential-worker peak before the qualifying stress aggregate gate'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathAuthoritativeMultiWorkerCommits 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'no multi-request direct-path batch backed by more than one physical path worker' `
		'qualifying path stress requires commit-backed per-batch multi-worker correlation'
    $shadowStressResult = [pscustomobject]@{
        sequence = 7; kind = 'ai'; stress = $true; configuration = 'shadow-16'
        aiEvidence = $shadowCompletion
    }
    Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
        sequence = 20; kind = 'ai'; stress = $true; configuration = 'parallel-2'
        aiEvidence = $authoritativeStressEvidence
    }, $shadowStressResult)
    $splitAiAuthority = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
			-CollisionAuthoritativeCommits 0) $stressEntry ('A' * 64)
    $splitCollisionAuthority = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
			-AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0) $stressEntry ('A' * 64)
    $splitPathAuthority = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0 `
            -CollisionAuthoritativeCommits 0) $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @(
            [pscustomobject]@{
                sequence = 201; kind = 'ai'; stress = $true; configuration = 'parallel-2'
                aiEvidence = $splitAiAuthority
            },
            [pscustomobject]@{
                sequence = 202; kind = 'ai'; stress = $true; configuration = 'parallel-2'
                aiEvidence = $splitCollisionAuthority
            },
            [pscustomobject]@{
                sequence = 203; kind = 'ai'; stress = $true; configuration = 'parallel-2'
                aiEvidence = $splitPathAuthority
            },
            $shadowStressResult)
    } 'same qualifying parallel 4v2 stress execution' `
        'authority split across otherwise valid stress executions cannot satisfy overall acceptance'
    $aiOnlyStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
            -CollisionPreparedPairs 0 -CollisionUniqueCandidates 0 `
            -CollisionSubmitted 0 -CollisionCompleted 0) $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 23; kind = 'ai'; stress = $true; configuration = 'parallel-2'
            aiEvidence = $aiOnlyStressEvidence
        }, $shadowStressResult)
    } 'AI counters cannot proxy collision work' `
        'positive AI work cannot substitute for authoritative collision work'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathWorkerExecuted 0 -PathAuthoritativeCommits 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'no multi-request direct-path batch backed by more than one physical path worker' `
		'AI and collision work cannot proxy direct-path worker authority'
    $noSpatialStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -SpatialHealingAuthoritative 0 -SpatialHealingCandidates 0 `
            -SpatialHealingSubmitted 0 -SpatialHealingCompleted 0 `
            -SpatialHealingPhysical 0 -SpatialPdlAuthoritative 0 `
            -SpatialPdlCandidates 0 -SpatialPdlSubmitted 0 `
            -SpatialPdlCompleted 0 -SpatialPdlPhysical 0) $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 25; kind = 'ai'; stress = $true; configuration = 'parallel-2'
            aiEvidence = $noSpatialStressEvidence
        }, $shadowStressResult)
    } 'no authoritative immutable-spatial healing and point-defense-laser work' `
        'AI, collision, physics, and path work cannot proxy live spatial consumer authority'
    $noAiStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -AuthoritativeCommits 0 -AiCommittedBatches 5 `
            -AiSubmitted 0 -AiCompleted 0) `
        $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 21; kind = 'ai'; stress = $true; configuration = 'parallel-2'
            aiEvidence = $noAiStressEvidence
        }, $shadowStressResult)
    } 'global or shadow-only scheduler activity is insufficient' `
        'duplicate shadow/global jobs cannot satisfy authoritative Stage 5 simulation work'
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 22; kind = 'ai'; stress = $false; configuration = 'parallel-2'
            aiEvidence = $authoritativeStressEvidence
        })
    } 'requires a parallel AI stress scenario' `
        'non-stress authoritative work cannot substitute for stress-scenario evidence'

    $replayEntry = [pscustomobject]@{
        sequence = 3; configuration = 'parallel-2'; simulationMode = 'parallel'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $true
    }
    $replayMetrics = ConvertFrom-Stage5ReplayMetrics (New-ReplayMetricOutput) $replayEntry
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialCapturedArenas 0) $replayEntry | Out-Null
    } 'has no captured immutable-spatial arena' `
        'parallel replay evidence rejects an all-zero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialSuccessfulCollections 0 `
                -SpatialSuccessfulCollectionQueries 0 `
                -SpatialSuccessfulCollectionRanges 0 `
                -SpatialMultiRangeCollections 0 `
                -SpatialCollectionSubmitted 0 -SpatialCollectionCompleted 0 `
                -SpatialCollectionPhysical 0 `
                -SpatialMaximumCollectionQueries 0 `
                -SpatialMaximumCollectionRanges 0) $replayEntry | Out-Null
    } 'no positive multi-query, multi-range immutable-spatial collection evidence' `
        'qualifying replay stress cannot pass without a multi-query spatial collection'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialCollectionSubmitted 4 `
                -SpatialCollectionCompleted 3 -SpatialCollectionPhysical 3) `
            $replayEntry | Out-Null
    } 'collection jobs are not balanced physical-worker work' `
        'replay spatial collection evidence rejects incomplete job completion'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialSuccessfulCollectionRanges 1 `
                -SpatialCollectionSubmitted 2 -SpatialCollectionCompleted 2 `
                -SpatialCollectionPhysical 2 -SpatialMaximumCollectionRanges 1) `
            $replayEntry | Out-Null
    } 'does not prove multi-query, multi-range two-pass worker execution' `
        'replay spatial evidence rejects a one-range owner-wait submission'
    $serialReplayEntry = [pscustomobject]@{
        sequence = 66; configuration = 'serial-1'; simulationMode = 'serial'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $false
    }
    $serialReplayMetrics = ConvertFrom-Stage5ReplayMetrics `
        (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
            -Workers 0 -Submitted 0 -Executed 0 -Fallback 0) $serialReplayEntry
    Assert-True ($serialReplayMetrics.spatialEvidence.capturedArenas -eq 0) `
        'serial replay evidence accepts and requires zero immutable-spatial captures'
    Assert-True ($replayMetrics.workers -eq 2) 'replay metrics are parsed into structured evidence'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode 'corrupt') $replayEntry | Out-Null
    } 'effective_mode is not a supported' 'replay metrics reject an invalid effective-mode enum'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics 'replay completed without structured metrics' $replayEntry | Out-Null
    } 'exactly one SIMULATION_JOB_METRICS' 'replay cannot pass without structured job metrics'
    $completeReplayMetrics = New-ReplayMetricOutput
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (($completeReplayMetrics -split "`n" | Where-Object {
                $_ -notmatch '^PHYSICS_INTEGRATION_MANIFEST '
            }) -join "`n") $replayEntry | Out-Null
    } 'exactly one PHYSICS_INTEGRATION_MANIFEST' `
        'replay cannot pass without its per-replay physics delta manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            ($completeReplayMetrics + "`n" +
                (($completeReplayMetrics -split "`n" | Where-Object {
                    $_ -match '^PHYSICS_INTEGRATION_MANIFEST '
                }) -join "`n")) $replayEntry | Out-Null
    } 'exactly one PHYSICS_INTEGRATION_MANIFEST' `
        'replay cannot pass with duplicate physics lifecycle output'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (($completeReplayMetrics -split "`n" | Where-Object {
                $_ -notmatch '^IMMUTABLE_SPATIAL_MANIFEST '
            }) -join "`n") $replayEntry | Out-Null
    } 'exactly one IMMUTABLE_SPATIAL_MANIFEST' `
        'replay cannot pass without its per-replay immutable-spatial delta manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            ((New-ReplayMetricOutput).Replace(' requested_pipeline=serial', ' requested_pipeline=parallel')) `
            $replayEntry | Out-Null
    } 'honestly request' 'replay cannot conceal a parallel pipeline request'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionShadowMismatches 1) `
            $replayEntry | Out-Null
    } 'collision shadow mismatches' 'replay rejects collision shadow mismatch telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'unexpected collision owner fallbacks' `
        'replay rejects unexpected collision fallback telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionSubmitted 4 -CollisionCompleted 3) `
            $replayEntry | Out-Null
    } 'collision submitted/completed job counts do not match' `
        'replay collision evidence rejects incomplete successful-job telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionShadowExecutions 1 `
                -CollisionShadowComparedCandidates 1) $replayEntry | Out-Null
    } 'collision shadow work outside shadow' `
        'parallel replay evidence rejects stale collision shadow counters from another mode'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
                -Workers 0 -Submitted 0 -Executed 0 `
                -CollisionAuthoritativeCommits 1 -CollisionCommittedCandidates 1 `
                -CollisionPreparedPairs 1 -CollisionUniqueCandidates 1 `
                -CollisionSubmitted 1 -CollisionCompleted 1 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            ([pscustomobject]@{
                sequence = 62; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'collision authority outside parallel' `
        'serial replay evidence rejects stale collision authority from another mode'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
                -Workers 0 -Submitted 0 -Executed 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
                -CollisionPreparedPairs 24 -CollisionUniqueCandidates 12 `
                -CollisionSubmitted 4 -CollisionCompleted 4) `
            ([pscustomobject]@{
                sequence = 64; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'serial replay reports collision lane work' `
        'serial replay rejects stale prepared collision work even with zero authority'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsShadowExecutions 1 `
                -PhysicsShadowMismatches 1) `
            $replayEntry | Out-Null
    } 'forbidden physics evidence' 'replay rejects physics shadow mismatch telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'forbidden physics evidence' 'replay rejects unexpected physics fallback telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialHealingSubmitted 8 `
                -SpatialHealingCompleted 7 -SpatialHealingPhysical 7) `
            $replayEntry | Out-Null
    } 'healing immutable-spatial jobs are not balanced' `
        'replay rejects incomplete healing spatial jobs'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialPdlUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'forbidden pdl immutable-spatial evidence' `
        'replay rejects unexpected PDL spatial fallback telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsAuthoritativeBatches 0 `
                -PhysicsCommittedPrefixes 0 -PhysicsRanges 0 `
                -PhysicsSubmitted 0 -PhysicsCompleted 0) $replayEntry | Out-Null
    } 'qualifying stress replay has no positive authoritative physics' `
        'qualifying stress replay rejects an all-zero physics manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsShadowExecutions 1 `
                -PhysicsShadowPrefixes 24 -PhysicsShadowRanges 2 `
                -PhysicsShadowSubmitted 2 -PhysicsShadowCompleted 2) `
            $replayEntry | Out-Null
    } 'physics shadow work outside shadow' `
        'parallel replay evidence rejects stale physics shadow counters from another mode'
    $replayOneEntry = [pscustomobject]@{
        sequence = 4; configuration = 'parallel-1'; simulationMode = 'parallel'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $true
    }
	$replayOneZeroPhysicsOutput = New-ReplayMetricOutput `
		-EffectiveMode parallel -Scheduler 1 -Workers 1 `
            -Submitted 0 -Executed 0 -Fallback 4 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
			-CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0
	$replayOneMetrics = ConvertFrom-Stage5ReplayMetrics `
		$replayOneZeroPhysicsOutput $replayOneEntry
    Assert-True $replayOneMetrics.expectedOneWorkerFallback `
        'replay parser accepts the expected forced one-worker kernel fallback with a live scheduler'
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			($replayOneZeroPhysicsOutput.Replace('capture_ns=0', 'capture_ns=1')) `
			$replayOneEntry | Out-Null
	} 'reports physics pre-scan, capture, or storage work' `
		'forced one-worker replay evidence rejects physics capture work before scheduler preflight'
    $replayOneCollisionFallbackMetrics = ConvertFrom-Stage5ReplayMetrics `
        (New-ReplayMetricOutput -EffectiveMode parallel -Scheduler 1 -Workers 1 `
            -Submitted 0 -Executed 0 -Fallback 4 -CollisionOwnerFallbacks 3 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
        $replayOneEntry
    Assert-True ($replayOneCollisionFallbackMetrics.collisionOwnerFallbacks -eq 3) `
        'replay parser accepts owner-only collision fallback in the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode serial -Scheduler 0 -Workers 0 `
                -Submitted 0 -Executed 0 -Fallback 4 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
            $replayOneEntry | Out-Null
    } 'explicit parallel scheduler unexpectedly fell back' `
        'forced one-worker replay cannot conceal a scheduler startup failure as kernel fallback'
    Assert-Throws {
        $nonStressReplayEntry = [pscustomobject]@{
            sequence = 65; configuration = 'parallel-2'; simulationMode = 'parallel'
            replayArgument = 'Stage5Validation\reference.rep'; stress = $false
        }
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode serial -Scheduler 0 -Workers 0 -Submitted 0 -Executed 0 -Fallback 4) `
            $nonStressReplayEntry | Out-Null
    } 'unexpectedly fell back' 'replay parser rejects fallback outside the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Workers 1) `
            ([pscustomobject]@{
                sequence = 5; configuration = 'parallel-4'; simulationMode = 'parallel'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'scheduler/worker count does not match' `
        'parallel-4 replay metrics cannot pass with one actual worker'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 1 -Workers 1 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
            ([pscustomobject]@{
                sequence = 6; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'serial configuration reports an active scheduler' `
        'serial replay metrics cannot report an active scheduler, worker, or jobs'

    $resultOne = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput) $replayEntry
    $resultTwo = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput) $replayEntry
    Assert-Stage5ReplayDeterminism @(
        [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
        [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultTwo }
    )
    Assert-Throws {
        $different = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalCRC 'DEADBEEF') $replayEntry
        Assert-Stage5ReplayDeterminism @(
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $different }
        )
    } 'final_crc differs' 'replay final CRC mismatches cannot pass across worker configurations'
    Assert-Throws {
        $different = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalFrame 42001) $replayEntry
        Assert-Stage5ReplayDeterminism @(
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $different }
        )
    } 'final_frame differs' 'replay final-frame mismatches cannot pass across worker configurations'
    Assert-Throws {
        ConvertFrom-Stage5ReplayResult 'replay completed with timing only' $replayEntry | Out-Null
    } 'exactly one SIMULATION_REPLAY_RESULT' 'timing-only replay evidence cannot pass as deterministic state'
    Assert-Throws {
        ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalCRC 'not-a-crc') $replayEntry | Out-Null
    } 'final_crc' 'malformed authoritative replay CRC fails closed'

    $timingDirectory = Join-Path $root 'valid-timing'
    New-Item -ItemType Directory -Path $timingDirectory | Out-Null
    $timingFile = Join-Path $timingDirectory 'frame-timing-1-1.csv'
    Write-TimingFixture $timingFile
    $timingEvidence = Get-Stage5TimingEvidence $timingDirectory 'valid fixture'
    Assert-True ($timingEvidence.rows -eq 2 -and $timingEvidence.maximumFrameEnd -eq 42000) `
        'timing evidence validates required rows and final frame'
    Assert-True ($timingEvidence.sha256 -ceq (Get-Sha256 $timingFile)) `
        'timing evidence records the exact CSV hash'
    $originalCulture = [Threading.Thread]::CurrentThread.CurrentCulture
    try {
        [Threading.Thread]::CurrentThread.CurrentCulture =
            [Globalization.CultureInfo]::GetCultureInfo('de-DE')
        $cultureTimingEvidence = Get-Stage5TimingEvidence $timingDirectory `
            'invariant-culture timing fixture'
        $framePhase = @($cultureTimingEvidence.phaseSummaries | Where-Object {
            $_.phase -ceq 'frame'
        })[0]
        Assert-True ([Math]::Abs($framePhase.totalMilliseconds - 90.0) -lt 0.001) `
            'phase totals retain invariant decimal semantics under a non-English CurrentCulture'
    }
    finally {
        [Threading.Thread]::CurrentThread.CurrentCulture = $originalCulture
    }
    Assert-Throws {
        Assert-Stage5CollisionTimingEvidence $timingEvidence `
            ([pscustomobject]@{ collisionAuthoritativeCommits = 3; collisionShadowExecutions = 0 }) `
            'missing collision phases fixture'
    } 'missing timing phase' 'authoritative collision work requires all collision timing phases'
    $collisionTimingDirectory = Join-Path $root 'collision-timing'
    New-Item -ItemType Directory -Path $collisionTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $collisionTimingDirectory 'frame-timing-1-7.csv') `
        -CollisionPhases -CollisionShadowPhase
    $collisionTimingEvidence = Get-Stage5TimingEvidence $collisionTimingDirectory `
        'collision timing fixture'
    Assert-Stage5CollisionTimingEvidence $collisionTimingEvidence `
        ([pscustomobject]@{ collisionAuthoritativeCommits = 3; collisionShadowExecutions = 1 }) `
        'collision timing fixture'
    Assert-True ($collisionTimingEvidence.phaseSummaries.Count -ge 12) `
        'collision timing evidence retains separate admission/validation/filter/prepare/compare phase totals'
    Assert-Throws {
        Get-Stage5TimingEvidence (Join-Path $root 'missing-timing-directory') `
            'missing timing fixture' | Out-Null
    } 'timing directory is missing' 'missing timing evidence cannot pass the gate'

    $badHeaderDirectory = Join-Path $root 'bad-header-timing'
    New-Item -ItemType Directory -Path $badHeaderDirectory | Out-Null
    Write-TimingFixture (Join-Path $badHeaderDirectory 'frame-timing-1-2.csv') -BadHeader
    Assert-Throws {
        Get-Stage5TimingEvidence $badHeaderDirectory 'bad header fixture' | Out-Null
    } 'header is invalid' 'timing CSV with a changed header fails closed'
    $emptyTimingDirectory = Join-Path $root 'empty-timing'
    New-Item -ItemType Directory -Path $emptyTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $emptyTimingDirectory 'frame-timing-1-3.csv') -HeaderOnly
    Assert-Throws {
        Get-Stage5TimingEvidence $emptyTimingDirectory 'empty fixture' | Out-Null
    } 'contains no data rows' 'header-only timing CSV fails closed'
    $missingPhaseDirectory = Join-Path $root 'missing-phase-timing'
    New-Item -ItemType Directory -Path $missingPhaseDirectory | Out-Null
    Write-TimingFixture (Join-Path $missingPhaseDirectory 'frame-timing-1-4.csv') -MissingLogic
    Assert-Throws {
        Get-Stage5TimingEvidence $missingPhaseDirectory 'missing phase fixture' | Out-Null
    } 'frame and logic phases' 'timing CSV missing required phases fails closed'
    $interactiveTimingDirectory = Join-Path $root 'interactive-timing'
    New-Item -ItemType Directory -Path $interactiveTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $interactiveTimingDirectory 'frame-timing-1-5.csv') -Interactive
    Assert-Throws {
        Get-Stage5TimingEvidence $interactiveTimingDirectory 'interactive mode fixture' | Out-Null
    } 'must identify the headless validation mode' `
        'interactive timing rows cannot satisfy the headless validation gate'
    foreach ($nonFiniteTiming in @('NaN', '1e309')) {
        $nonFiniteTimingDirectory = Join-Path $root ("non-finite-timing-$nonFiniteTiming")
        New-Item -ItemType Directory -Path $nonFiniteTimingDirectory | Out-Null
        Write-TimingFixture (Join-Path $nonFiniteTimingDirectory 'frame-timing-1-6.csv') `
            -WallMilliseconds $nonFiniteTiming
        Assert-Throws {
            Get-Stage5TimingEvidence $nonFiniteTimingDirectory `
                "non-finite timing fixture $nonFiniteTiming" | Out-Null
        } 'invalid wall_ms' "timing CSV rejects non-finite decimal '$nonFiniteTiming'"
    }

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'timing-disabled-acceptance') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly -DisableFrameTiming | Out-Null
    } 'only with DiagnosticNonAcceptance' 'acceptance plan cannot disable frame timing'
    $diagnosticOutput = Join-Path $root 'timing-disabled-diagnostic'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $diagnosticOutput `
        -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly `
        -DisableFrameTiming -DiagnosticNonAcceptance | Out-Null
    $diagnosticPlan = Get-Content -LiteralPath (Join-Path $diagnosticOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True (-not $diagnosticPlan.deterministicRuntimeEligible -and
        -not $diagnosticPlan.finalAcceptanceEligible -and -not $diagnosticPlan.frameTimingRequired) `
        'timing-disabled plan is explicitly marked focused diagnostic evidence'

    $stage3Baseline = [pscustomobject]@{
        executableSha256 = ('B' * 64); physicalCoreCount = 16; availableCpus = 16
        fileSha256 = ('C' * 64); wallMilliseconds = @(1000, 100, 100, 100)
        measuredMedianMilliseconds = 100.0; file = 'baseline-source.json'
        evidenceFile = 'stage3-performance-baseline.json'; expectedExecutableSha256 = ('B' * 64)
    }
    $performanceResults = New-Object 'Collections.Generic.List[object]'
    $sequence = 0
    foreach ($sample in @(10000, 100, 102, 98, 101)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-1' (++$sequence) $sample 1)) | Out-Null
    }
    foreach ($sample in @(1, 40, 41, 39, 40)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-8' (++$sequence) $sample 8)) | Out-Null
    }
    foreach ($sample in @(1, 30, 31, 29, 30)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-16' (++$sequence) $sample 16)) | Out-Null
    }
    $performance = Measure-Stage5Performance $performanceResults.ToArray() 16 $stage3Baseline 1 3
    Assert-True ($performance.status -ceq 'passed') 'robust median performance fixture passes approved targets'
    Assert-True ($performance.measurementScope -ceq 'aggregate-stage5-stress-replay-throughput' -and
        -not $performance.collisionSpecificSpeedupClaim) `
        'performance report scopes 2x throughput to aggregate Stage 5 replay work, not collision speedup'
    Assert-True ($performance.currentOneWorker.medianWallMilliseconds -lt 200) `
        'performance median excludes the warm-up outlier'
    Assert-True (($performance.currentOneWorker.rawWallMilliseconds -join ',') -ceq `
        '10000,100,102,98,101') 'performance report retains current raw samples including warm-up'
    Assert-True ($performance.independentlyExpectedStage3ExecutableSha256 -ceq ('B' * 64)) `
        'performance report records independently supplied Stage 3 provenance'
    $zeroWarmup = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-1' })
    foreach ($sample in @(0, 100, 102, 98, 101)) {
        $zeroWarmup += New-PerformanceResult 'parallel-1' (++$sequence) $sample 1
    }
    Assert-Throws {
        Measure-Stage5Performance $zeroWarmup 16 $stage3Baseline 1 3 | Out-Null
    } 'non-positive wall time' 'performance warm-up samples must be valid positive evidence'
    $mixedTopology = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-8' })
    foreach ($sample in @(
        [pscustomobject]@{ wall = 1; cpus = 16 },
        [pscustomobject]@{ wall = 40; cpus = 16 },
        [pscustomobject]@{ wall = 41; cpus = 32 },
        [pscustomobject]@{ wall = 39; cpus = 16 },
        [pscustomobject]@{ wall = 40; cpus = 16 }
    )) {
        $mixedTopology += New-PerformanceResult 'parallel-8' (++$sequence) $sample.wall 8 $sample.cpus
    }
    Assert-Throws {
        Measure-Stage5Performance $mixedTopology 16 $stage3Baseline 1 3 | Out-Null
    } 'topology varies across measured runs' `
        'one 32-CPU sample among 16-CPU samples cannot be hidden by topology minima'

    $slowEight = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-8' })
    foreach ($sample in @(1, 60, 61, 59, 60)) {
        $slowEight += New-PerformanceResult 'parallel-8' (++$sequence) $sample 8
    }
    Assert-True ((Measure-Stage5Performance $slowEight 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'sub-2x eight-worker median throughput fails'

    $slowSixteen = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-16' })
    foreach ($sample in @(1, 45, 46, 44, 45)) {
        $slowSixteen += New-PerformanceResult 'parallel-16' (++$sequence) $sample 16
    }
    Assert-True ((Measure-Stage5Performance $slowSixteen 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'non-positive eight-to-sixteen scaling fails on a capable host'

    $regressedOne = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-1' })
    foreach ($sample in @(1, 106, 107, 105, 106)) {
        $regressedOne += New-PerformanceResult 'parallel-1' (++$sequence) $sample 1
    }
    Assert-True ((Measure-Stage5Performance $regressedOne 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'one-worker regression above five percent fails'
    Assert-True ((Measure-Stage5Performance $performanceResults.ToArray() 7 $stage3Baseline 1 3).status `
        -ceq 'unsupported-host-topology') 'smaller hosts report performance as unsupported'
    $mismatchedBaseline = [pscustomobject]@{
        executableSha256 = ('B' * 64); physicalCoreCount = 8; availableCpus = 8
        fileSha256 = ('C' * 64); wallMilliseconds = @(1000, 100, 100, 100)
        measuredMedianMilliseconds = 100.0; file = 'baseline-source.json'
        evidenceFile = 'stage3-performance-baseline.json'; expectedExecutableSha256 = ('B' * 64)
    }
    Assert-True ((Measure-Stage5Performance $performanceResults.ToArray() 16 $mismatchedBaseline 1 3).status `
        -ceq 'failed') 'Stage 3 baseline from a different machine topology cannot pass'
    Assert-Throws {
        Measure-Stage5Performance $performanceResults.ToArray() 16 $null 1 3 | Out-Null
    } 'baseline evidence is required' 'performance cannot pass without Stage 3 baseline evidence'
    Assert-Throws {
        Read-Stage5PerformanceBaseline '' $stressHash ('B' * 64) | Out-Null
    } 'BaselinePath is required' 'performance cannot start without an explicit Stage 3 baseline input'

    $invalidBaselinePath = Join-Path $root 'invalid-stage3-baseline.json'
    [IO.File]::WriteAllText($invalidBaselinePath, ([ordered]@{
        schemaVersion = 1; stage = 'Stage3'; architecture = 'x64'; executableSha256 = 'not-a-hash'
        fixtureSha256 = $stressHash; configuration = 'parallel-1'; physicalCoreCount = 16
        availableCpus = 16; warmupRuns = 1
        wallMilliseconds = @(1000, 100, 100, 100)
    } | ConvertTo-Json -Depth 4))
    Assert-Throws {
        Read-Stage5PerformanceBaseline $invalidBaselinePath $stressHash ('B' * 64) | Out-Null
    } 'exact executable SHA-256' 'Stage 3 baseline without exact candidate provenance fails closed'

    $validBaselinePath = Join-Path $root 'valid-stage3-baseline.json'
    [IO.File]::WriteAllText($validBaselinePath, ([ordered]@{
        schemaVersion = 1; stage = 'Stage3'; architecture = 'x64'; executableSha256 = ('B' * 64)
        fixtureSha256 = $stressHash; configuration = 'parallel-1'; physicalCoreCount = 16
        availableCpus = 16; warmupRuns = 1; wallMilliseconds = @(1000, 100, 100, 100)
    } | ConvertTo-Json -Depth 4))
    Assert-Throws {
        Read-Stage5PerformanceBaseline $validBaselinePath $stressHash ('C' * 64) | Out-Null
    } 'independently supplied expected hash' 'self-asserted Stage 3 executable provenance cannot pass'

    foreach ($nonFiniteJson in @('NaN', '1e309')) {
        $nonFiniteBaselinePath = Join-Path $root ("non-finite-stage3-$nonFiniteJson.json")
        $nonFiniteBaseline = @"
{"schemaVersion":1,"stage":"Stage3","architecture":"x64","executableSha256":"$('B' * 64)","fixtureSha256":"$stressHash","configuration":"parallel-1","physicalCoreCount":16,"availableCpus":16,"warmupRuns":1,"wallMilliseconds":[1000,$nonFiniteJson,100,100]}
"@
        [IO.File]::WriteAllText($nonFiniteBaselinePath, $nonFiniteBaseline)
        Assert-Throws {
            Read-Stage5PerformanceBaseline $nonFiniteBaselinePath $stressHash ('B' * 64) | Out-Null
        } 'finite JSON numbers|Invalid JSON primitive' `
            "Stage 3 baseline rejects non-finite JSON sample '$nonFiniteJson'"
    }

    $restored = New-Object 'Collections.Generic.List[int]'
    $restoreError = ''
    try {
        Invoke-Stage5RegistryRestore -Snapshots @(
            [pscustomobject]@{ id = 1 }, [pscustomobject]@{ id = 2 }, [pscustomobject]@{ id = 3 }
        ) -RestoreAction {
            param($snapshot)
            $restored.Add([int]$snapshot.id) | Out-Null
            if ($snapshot.id -eq 2 -or $snapshot.id -eq 3) {
                throw "injected restore failure $($snapshot.id)"
            }
        }
    }
    catch { $restoreError = $_.Exception.Message }
    Assert-True ($restoreError -match 'after attempting every snapshot' -and
        $restoreError -match 'injected restore failure 3' -and
        $restoreError -match 'injected restore failure 2') `
        'registry restore aggregates every injected failure'
    Assert-True (($restored.ToArray() -join ',') -ceq '3,2,1') `
        'registry restoration continues in reverse order after an injected failure'
    Assert-True (Test-Stage5RegistryLeafRemoval $false @() @()) `
        'absent-key snapshot removes its validation-created leaf after restoring the added value'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $true @() @())) `
        'pre-existing empty key is preserved during restoration'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $false @('concurrent-value') @())) `
        'validation-created key with concurrent value content is preserved'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $false @() @('concurrent-subkey'))) `
        'validation-created key with concurrent subkey content is preserved'
    $createdHierarchy = @(
        'Software\Electronic Arts',
        'Software\Electronic Arts\EA Games',
        'Software\Electronic Arts\EA Games\Validation Product'
    )
    $removedHierarchy = New-Object 'Collections.Generic.List[string]'
    Invoke-Stage5CreatedRegistryKeyCleanup $createdHierarchy -InspectAction {
        param($createdSubKey)
        return [pscustomobject]@{ valueNames = @(); subKeyNames = @() }
    } -RemoveAction {
        param($createdSubKey)
        $removedHierarchy.Add($createdSubKey) | Out-Null
    }
    Assert-True (($removedHierarchy.ToArray() -join '|') -ceq `
        (($createdHierarchy[2], $createdHierarchy[1], $createdHierarchy[0]) -join '|')) `
        'fixture starting with only HKCU Software removes the full validation-created hierarchy in reverse'
    $cleanupAttempts = New-Object 'Collections.Generic.List[string]'
    Assert-Throws {
        Invoke-Stage5CreatedRegistryKeyCleanup $createdHierarchy -InspectAction {
            return [pscustomobject]@{ valueNames = @(); subKeyNames = @() }
        } -RemoveAction {
            param($createdSubKey)
            $cleanupAttempts.Add($createdSubKey) | Out-Null
            if ($createdSubKey -ceq $createdHierarchy[2]) {
                throw 'injected leaf cleanup failure'
            }
        }
    } 'after attempting every created key.*injected leaf cleanup failure' `
        'created-key cleanup aggregates a removal failure after attempting later ancestors'
    Assert-True (($cleanupAttempts.ToArray() -join '|') -ceq `
        (($createdHierarchy[2], $createdHierarchy[1], $createdHierarchy[0]) -join '|')) `
        'created-key cleanup continues reverse attempts after one removal failure'

    $privacyState = [pscustomobject]@{
        value = 'private-original-value'
        registered = New-Object 'Collections.Generic.List[object]'
    }
    $setupPipelineOutput = @(Invoke-Stage5RegistrySetupTransaction @('Software') `
        -ActionContext $privacyState -EnsureSubKeyAction { return $false } `
        -CaptureValueAction {
        param($created, $state)
        return [pscustomobject]@{ oldValue = $state.value; createdSubKeys = @($created) }
    } -SetValueAction {
        param($snapshot, $state)
        $state.value = 'validation'
    } -RegisterSnapshotAction {
        param($snapshot, $state)
        $state.registered.Add($snapshot) | Out-Null
    } -RestoreValueAction { } -CleanupCreatedSubKeysAction { })
    Assert-True ($setupPipelineOutput.Count -eq 0 -and $privacyState.registered.Count -eq 1 -and
        $privacyState.registered[0].oldValue -ceq 'private-original-value') `
        'transactional registry setup registers its private snapshot without leaking it to pipeline output'

    $segmentFailureState = [pscustomobject]@{
        value = 'original'
        created = New-Object 'Collections.Generic.List[string]'
        removed = New-Object 'Collections.Generic.List[string]'
        registered = New-Object 'Collections.Generic.List[object]'
    }
    $segmentSetupError = ''
    try {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts',
            'Software\Electronic Arts\EA Games') -ActionContext $segmentFailureState `
            -EnsureSubKeyAction {
            param($subKey, $state)
            $state.created.Add($subKey) | Out-Null
            return $true
        } -CaptureValueAction {
            param($created, $state)
            return [pscustomobject]@{ hadValue = $true; oldValue = $state.value }
        } -SetValueAction {
            param($snapshot, $state)
            $state.value = 'validation'
        } -RegisterSnapshotAction {
            param($snapshot, $state)
            $state.registered.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $state)
            $state.value = $snapshot.oldValue
        } -CleanupCreatedSubKeysAction {
            param($created, $state)
            for ($index = $created.Count - 1; $index -ge 0; --$index) {
                $state.removed.Add($created[$index]) | Out-Null
                $state.created.Remove($created[$index]) | Out-Null
            }
        } -AfterSegmentAction {
            param($subKey, $createdCount)
            if ($createdCount -eq 2) { throw 'injected failure after two segments' }
        } | Out-Null
    }
    catch { $segmentSetupError = $_.Exception.Message }
    Assert-True ($segmentSetupError -match 'injected failure after two segments' -and
        $segmentSetupError -match 'rollback: completed') `
        'registry setup reports the injected segment failure and completed rollback'
    Assert-True ($segmentFailureState.created.Count -eq 0 -and
        ($segmentFailureState.removed.ToArray() -join '|') -ceq
        'Software\Electronic Arts\EA Games|Software\Electronic Arts') `
        'registry setup failure after two segments removes every created ancestor in reverse'

    $valueFailureState = [pscustomobject]@{
        value = 'original'
        created = New-Object 'Collections.Generic.List[string]'
        removed = New-Object 'Collections.Generic.List[string]'
        registered = New-Object 'Collections.Generic.List[object]'
        outerRestored = New-Object 'Collections.Generic.List[int]'
    }
    $valueFailureState.registered.Add([pscustomobject]@{ id = 99 }) | Out-Null
    $valueSetupError = ''
    try {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts',
            'Software\Electronic Arts\EA Games') -ActionContext $valueFailureState `
            -EnsureSubKeyAction {
            param($subKey, $state)
            $state.created.Add($subKey) | Out-Null
            return $true
        } -CaptureValueAction {
            param($created, $state)
            return [pscustomobject]@{ hadValue = $true; oldValue = $state.value }
        } -SetValueAction {
            param($snapshot, $state)
            $state.value = 'validation'
        } -RegisterSnapshotAction {
            param($snapshot, $state)
            $state.registered.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $state)
            $state.value = $snapshot.oldValue
        } -CleanupCreatedSubKeysAction {
            param($created, $state)
            for ($index = $created.Count - 1; $index -ge 0; --$index) {
                $state.removed.Add($created[$index]) | Out-Null
                $state.created.Remove($created[$index]) | Out-Null
            }
        } -AfterValueWriteAction {
            throw 'injected failure after value write'
        } | Out-Null
    }
    catch { $valueSetupError = $_.Exception.Message }
    Assert-True ($valueSetupError -match 'injected failure after value write' -and
        $valueFailureState.value -ceq 'original' -and $valueFailureState.created.Count -eq 0) `
        'registry setup failure after value write restores the original value and created hierarchy'
    Invoke-Stage5RegistryRestore -Snapshots @($valueFailureState.registered.ToArray()) `
        -RestoreAction {
        param($snapshot)
        $valueFailureState.outerRestored.Add([int]$snapshot.id) | Out-Null
    }
    Assert-True (($valueFailureState.outerRestored.ToArray() -join ',') -ceq '99') `
        'a failed transactional setup leaves prior snapshots registered for the outer finally restore'

    Assert-Throws {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts') `
            -EnsureSubKeyAction { return $true } `
            -CaptureValueAction { return [pscustomobject]@{ hadValue = $false } } `
            -SetValueAction { } -RegisterSnapshotAction { } -RestoreValueAction { } `
            -CleanupCreatedSubKeysAction { throw 'injected rollback cleanup failure' } `
            -AfterSegmentAction { throw 'injected setup failure' } | Out-Null
    } 'setup: injected setup failure.*rollback: created-key cleanup: injected rollback cleanup failure' `
        'registry setup transaction aggregates setup and rollback errors'

    # Final acceptance is deliberately separate from the deterministic-runtime
    # replay/AI matrix. Build one complete, independently hashed evidence set,
    # then prove that missing, stale, tampered, and combined-policy-invalid
    # inputs all fail closed.
    $acceptanceRoot = Join-Path $root 'final-acceptance'
    $artifactFiles = Join-Path $acceptanceRoot 'artifact-files'
    $attachmentRoot = Join-Path $acceptanceRoot 'attachments'
    New-Item -ItemType Directory -Path $artifactFiles, $attachmentRoot -Force | Out-Null
    $sourceCommit = 'a' * 40
    $artifactRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifactEntries = @()
    $artifactTestHashes = @{}
    foreach ($role in $artifactRoles) {
        $leaf = "$role.bin"
        $path = Join-Path $artifactFiles $leaf
        [IO.File]::WriteAllText($path, "artifact:$role")
        $artifactHash = Get-Sha256 $path
        $artifactEntries += [ordered]@{
            role = $role
            path = "artifact-files\$leaf"
            sha256 = $artifactHash
        }
        $artifactTestHashes[$role] = $artifactHash
    }
    $artifactSetPath = Join-Path $acceptanceRoot 'artifact-set.json'
    Write-JsonDocument $artifactSetPath ([ordered]@{
        schemaVersion = 1
        sourceCommit = $sourceCommit
        productSet = @('Generals', 'ZeroHour')
        architecture = 'x64'
        artifacts = $artifactEntries
    })
    $artifactSetHash = Get-Sha256 $artifactSetPath
    $requiredAttachmentRoles = [ordered]@{
        'replay-determinism' = @('replay-results', 'replay-fixture-manifest')
        'fresh-ai' = @('ai-results')
        'performance-scaling' = @('stage3-baseline', 'performance-report')
        'mixed-worker-multiplayer' = @('multiplayer-results')
        'combined-stage4-stage5-installed-runtime' = @('combined-results')
        'premium-review' = @('premium-review-results')
        'manual-acceptance' = @('manual-checklist')
        'deterministic-runtime' = @('validation-plan', 'validation-results', 'performance-report')
    }
    $detailsByKind = [ordered]@{
        'replay-determinism' = [ordered]@{
            uniqueReplayCount = 10; executionCount = 168; matrixPasses = 2
            stressExecutionsPerConfiguration = 6
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            allExecutionsPassed = $true; deterministicAcrossWorkers = $true
        }
        'fresh-ai' = [ordered]@{
            scenarios = @('4v3', '4v2'); distinctSeeds = 3; repeats = 2
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            freshGames = $true; allGamesCompleted = $true
            deterministicAcrossWorkers = $true
        }
        'performance-scaling' = [ordered]@{
            physicalCoreCount = 16; oneWorkerRegressionRatio = 1.01
            eightWorkerSpeedup = 2.1; sixteenWorkerStatus = 'passed'
            eightToSixteenSpeedup = 1.1
        }
        'mixed-worker-multiplayer' = [ordered]@{
            workerCounts = @('1', '2', '4', '8', 'auto')
            matchRecords = 16; peerRecords = 40; fixedSeeds = @(23063, 49374)
            topologies = @('two-peer-1-v-16', 'two-peer-2-v-auto',
                'two-peer-4-v-8', 'four-peer-mixed-workers')
            provenKernelMask = 63
            allMatchesCompleted = $true; stateTracesIdentical = $true
            crossEpochRejected = $true; contentMismatchRejected = $true
        }
        'combined-stage4-stage5-installed-runtime' = [ordered]@{
            installedRuntime = $true; pipelineMode = 'parallel'; simulationMode = 'parallel'
            workerPolicy = 'auto'; renderer = 'd3d11'; renderThread = 'dedicated'
            bothTitlesPassed = $true; stage4AndStage5Concurrent = $true
            visualParityPassed = $true; deviceRecoveryPassed = $true
            gameplaySoakPassed = $true
        }
        'premium-review' = [ordered]@{
            reviewedCommit = $sourceCommit; reviewRounds = 2; independentReviewers = 9
            completeDiffReviewed = $true; fixesRetested = $true
            openP0 = 0; openP1 = 0; openP2 = 0
        }
        'manual-acceptance' = [ordered]@{
            approvalScope = 'final-stage5-installed-runtime'; approvedByUser = $true
            candidateHashVerified = $true; bothTitlesTested = $true
            graphicsPassed = $true; audioPassed = $true; inputPassed = $true
            saveLoadPassed = $true; largeMatchPassed = $true; cleanExitPassed = $true
        }
    }
    $evidenceDocuments = @{}
    $evidencePaths = @{}
    $evidenceHashes = @{}
    foreach ($kind in @($detailsByKind.Keys)) {
        $attachments = @()
        foreach ($role in $requiredAttachmentRoles[$kind]) {
            $leaf = "$kind-$role.json"
            $attachmentPath = Join-Path $attachmentRoot $leaf
            if ($kind -ceq 'mixed-worker-multiplayer' -and $role -ceq 'multiplayer-results') {
                Write-Net3LoopbackTestManifest $attachmentPath $sourceCommit $artifactSetHash `
                    $artifactTestHashes['generals-executable'] `
                    $artifactTestHashes['zerohour-executable']
            }
            elseif ($kind -ceq 'performance-scaling' -and $role -ceq 'performance-report') {
                Write-PerformanceScalingTestManifest $attachmentPath $sourceCommit $artifactSetHash `
                    $artifactTestHashes['zerohour-executable'] `
                    (Get-Sha256 (Join-Path $attachmentRoot "$kind-stage3-baseline.json"))
            }
            else {
                [IO.File]::WriteAllText($attachmentPath, "{`"evidence`":`"$kind/$role`"}")
            }
            $attachments += [ordered]@{
                role = $role
                path = "attachments\$leaf"
                sha256 = Get-Sha256 $attachmentPath
            }
        }
        $title = if ($kind -in @('combined-stage4-stage5-installed-runtime',
            'premium-review', 'manual-acceptance')) { 'Both' } else { 'ZeroHour' }
        $document = [ordered]@{
            schemaVersion = 1; evidenceKind = $kind; status = 'passed'
            sourceCommit = $sourceCommit; title = $title; architecture = 'x64'
            artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-01T00:00:00Z'
            attachments = $attachments; details = $detailsByKind[$kind]
        }
        $path = Join-Path $acceptanceRoot "$kind.json"
        Write-JsonDocument $path $document
        $evidenceDocuments[$kind] = $document
        $evidencePaths[$kind] = $path
        $evidenceHashes[$kind] = Get-Sha256 $path
    }
    $deterministicKind = 'deterministic-runtime'
    $deterministicAttachments = @()
    foreach ($role in $requiredAttachmentRoles[$deterministicKind]) {
        $leaf = "$deterministicKind-$role.json"
        $attachmentPath = Join-Path $attachmentRoot $leaf
        [IO.File]::WriteAllText($attachmentPath, "{`"evidence`":`"$deterministicKind/$role`"}")
        $deterministicAttachments += [ordered]@{
            role = $role; path = "attachments\$leaf"; sha256 = Get-Sha256 $attachmentPath
        }
    }
    $deterministicDocument = [ordered]@{
        schemaVersion = 1; evidenceKind = $deterministicKind; status = 'passed'
        sourceCommit = $sourceCommit; title = 'ZeroHour'; architecture = 'x64'
        artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-01T00:00:00Z'
        attachments = $deterministicAttachments
        details = [ordered]@{
            gateName = 'deterministic-runtime'; isolatedPipelineMode = 'serial'
            simulationModes = @('serial', 'parallel', 'shadow')
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            isolatedMatrixPassed = $true; finalAcceptanceClaim = $false
            replayEvidenceSha256 = $evidenceHashes['replay-determinism']
            freshAiEvidenceSha256 = $evidenceHashes['fresh-ai']
            performanceEvidenceSha256 = $evidenceHashes['performance-scaling']
        }
    }
    $deterministicPath = Join-Path $acceptanceRoot "$deterministicKind.json"
    Write-JsonDocument $deterministicPath $deterministicDocument
    $evidenceDocuments[$deterministicKind] = $deterministicDocument
    $evidencePaths[$deterministicKind] = $deterministicPath
    $evidenceHashes[$deterministicKind] = Get-Sha256 $deterministicPath

    $acceptanceKinds = @('deterministic-runtime', 'replay-determinism', 'fresh-ai',
        'performance-scaling', 'mixed-worker-multiplayer',
        'combined-stage4-stage5-installed-runtime', 'premium-review', 'manual-acceptance')
    function Write-AcceptanceRequest {
        param([string]$Path, [string[]]$Kinds)
        Write-JsonDocument $Path ([ordered]@{
            schemaVersion = 1; gateName = 'final-stage5-acceptance'
            sourceCommit = $sourceCommit
            artifactSet = [ordered]@{
                path = 'artifact-set.json'; sha256 = Get-Sha256 $artifactSetPath
            }
            evidence = @($Kinds | ForEach-Object {
                [ordered]@{
                    kind = $_; path = "$_.json"; sha256 = Get-Sha256 $evidencePaths[$_]
                }
            })
        })
    }
    $acceptanceRequest = Join-Path $acceptanceRoot 'final-acceptance.json'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    $acceptanceReport = Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest
    Assert-True ($acceptanceReport.status -ceq 'passed' -and
        $acceptanceReport.gateName -ceq 'final-stage5-acceptance' -and
        $acceptanceReport.evidence.Count -eq 8) `
        'complete independently hashed final acceptance evidence passes aggregation'

    $acceptanceOutput = Join-Path $acceptanceRoot 'final-acceptance-report.json'
    & (Join-Path $PSScriptRoot 'Invoke-Stage5FinalAcceptance.ps1') `
        -AcceptanceManifestPath $acceptanceRequest -OutputPath $acceptanceOutput | Out-Null
    $writtenReport = Get-Content -LiteralPath $acceptanceOutput -Raw | ConvertFrom-Json
    Assert-True ($writtenReport.status -ceq 'passed' -and
        $writtenReport.sourceCommit -ceq $sourceCommit) `
        'final acceptance command writes only a validated passed report'
    Assert-Throws {
        & (Join-Path $PSScriptRoot 'Invoke-Stage5FinalAcceptance.ps1') `
            -AcceptanceManifestPath $acceptanceRequest -OutputPath $acceptanceOutput | Out-Null
    } 'refusing to overwrite evidence' 'final acceptance report refuses evidence overwrite'

    $missingManualRequest = Join-Path $acceptanceRoot 'missing-manual.json'
    Write-AcceptanceRequest $missingManualRequest @($acceptanceKinds | Where-Object {
        $_ -cne 'manual-acceptance'
    })
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $missingManualRequest | Out-Null
    } 'missing.*manual-acceptance|must contain each required value' `
        'final acceptance fails closed before user manual evidence exists'

    $combinedDocument = $evidenceDocuments['combined-stage4-stage5-installed-runtime']
    $combinedDocument.details.pipelineMode = 'serial'
    Write-JsonDocument $evidencePaths['combined-stage4-stage5-installed-runtime'] $combinedDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'requires pipelineMode=parallel' `
        'final acceptance rejects a serial Stage 4 pipeline masquerading as the combined policy lane'
    $combinedDocument.details.pipelineMode = 'parallel'
    Write-JsonDocument $evidencePaths['combined-stage4-stage5-installed-runtime'] $combinedDocument

    $manualDocument = $evidenceDocuments['manual-acceptance']
    $manualDocument.artifactSetSha256 = 'B' * 64
    Write-JsonDocument $evidencePaths['manual-acceptance'] $manualDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'does not identify the same passed x64 commit and artifact set' `
        'final acceptance rejects evidence from a different artifact set'
    $manualDocument.artifactSetSha256 = $artifactSetHash
    Write-JsonDocument $evidencePaths['manual-acceptance'] $manualDocument

    $net3Manifest = Join-Path $attachmentRoot 'mixed-worker-multiplayer-multiplayer-results.json'
    $net3Proof = Read-Stage5Net3LoopbackEvidence $net3Manifest $sourceCommit $artifactSetHash `
        $artifactTestHashes['generals-executable'] $artifactTestHashes['zerohour-executable']
    Assert-True ($net3Proof.provenKernelMask -eq 0x3F -and
        $net3Proof.matchCount -eq 16 -and $net3Proof.peerRecordCount -eq 40) `
        'canonical NET3 evidence proves six kernels across exactly 16 matches and 40 peer records'
    $proofDirectory = Join-Path $acceptanceRoot 'generated-proof'
    $proofHeader = Join-Path $proofDirectory 'MultiplayerSimulationReleaseProof.generated.h'
    & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
        -EvidenceManifestPath $net3Manifest -OutputPath $proofHeader `
        -ExpectedSourceCommit $sourceCommit -ExpectedArtifactSetSha256 $artifactSetHash `
        -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
        -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] | Out-Null
    $proofContent = Get-Content -LiteralPath $proofHeader -Raw
    Assert-True ($proofContent -match 'RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_SCHEMA 1' -and
        $proofContent -match 'RTS_MULTIPLAYER_SIMULATION_RELEASE_PROOF_PROVEN_KERNEL_MASK 0x0000003Fu' -and
        $proofContent.Contains($sourceCommit) -and
        $proofContent.Contains((Get-Sha256 $net3Manifest))) `
        'release-proof header binds schema, source, manifest digest, and the validated kernel mask'

    $missingNet3Path = Join-Path $acceptanceRoot 'net3-missing-match.json'
    $missingNet3 = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $missingNet3.matches = @($missingNet3.matches | Select-Object -First 15)
    Write-JsonDocument $missingNet3Path $missingNet3
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $missingNet3Path $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'exactly 16 match records' 'NET3 evidence rejects a missing match record'

    $duplicateNet3Path = Join-Path $acceptanceRoot 'net3-duplicate-match.json'
    $duplicateNet3 = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $duplicateNet3.matches[15] = $duplicateNet3.matches[0]
    Write-JsonDocument $duplicateNet3Path $duplicateNet3
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $duplicateNet3Path $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'not canonical' 'NET3 evidence rejects a duplicate match disguised as the final record'

    $missingPeerPath = Join-Path $acceptanceRoot 'net3-missing-peer.json'
    $missingPeer = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $missingPeer.matches[0].peers = @($missingPeer.matches[0].peers | Select-Object -First 1)
    Write-JsonDocument $missingPeerPath $missingPeer
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $missingPeerPath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'exact topology peer roster' 'NET3 evidence rejects a missing nested peer record'

    $wrongProvenancePath = Join-Path $acceptanceRoot 'net3-wrong-provenance.json'
    $wrongProvenance = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $wrongProvenance.sourceCommit = 'b' * 40
    Write-JsonDocument $wrongProvenancePath $wrongProvenance
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $wrongProvenancePath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'source commit does not match independent provenance' `
        'NET3 evidence rejects self-consistent but independently wrong provenance'

    $tamperedKernelPath = Join-Path $acceptanceRoot 'net3-tampered-kernel.json'
    $tamperedKernel = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $tamperedKernel.matches[0].peers[1].kernels[0].physicalWorkerJobs = 0
    Write-JsonDocument $tamperedKernelPath $tamperedKernel
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $tamperedKernelPath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'does not prove positive work' 'NET3 evidence rejects tampered physical-worker proof'
    $invalidProofHeader = Join-Path (Join-Path $acceptanceRoot 'invalid-proof') `
        'MultiplayerSimulationReleaseProof.generated.h'
    Assert-Throws {
        & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
            -EvidenceManifestPath $tamperedKernelPath -OutputPath $invalidProofHeader `
            -ExpectedSourceCommit $sourceCommit -ExpectedArtifactSetSha256 $artifactSetHash `
            -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
            -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'does not prove positive work' `
        'invalid evidence cannot generate a nonzero multiplayer release proof'
    Assert-True (-not (Test-Path -LiteralPath $invalidProofHeader)) `
        'invalid evidence leaves the release-proof header absent so the runtime mask remains zero'

    $scalingManifest = Join-Path $attachmentRoot 'performance-scaling-performance-report.json'
    $scalingBaselineHash = Get-Sha256 (Join-Path $attachmentRoot `
        'performance-scaling-stage3-baseline.json')
    $scalingProof = Read-Stage5PerformanceScalingEvidence $scalingManifest $sourceCommit `
        $artifactSetHash $artifactTestHashes['zerohour-executable'] $scalingBaselineHash
    Assert-True ($scalingProof.physicalCoreCount -eq 16 -and
        $scalingProof.fixtureCount -eq 4 -and $scalingProof.kernelCount -eq 6) `
        'canonical scaling evidence proves physical topology, realistic fixtures, and six kernels'

    $missingScalingPath = Join-Path $acceptanceRoot 'scaling-missing-fixture.json'
    $missingScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $missingScaling.fixtures = @($missingScaling.fixtures | Select-Object -First 3)
    Write-JsonDocument $missingScalingPath $missingScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence $missingScalingPath $sourceCommit `
            $artifactSetHash $artifactTestHashes['zerohour-executable'] `
            $scalingBaselineHash | Out-Null
    } 'exact 1k, 4k, 8k, and dense eight-player fixtures' `
        'scaling evidence rejects a missing realistic fixture'

    $tamperedScalingPath = Join-Path $acceptanceRoot 'scaling-tampered-kernel.json'
    $tamperedScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $tamperedScaling.kernelTimings[0].totalParallelMilliseconds = 5.0
    Write-JsonDocument $tamperedScalingPath $tamperedScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence $tamperedScalingPath $sourceCommit `
            $artifactSetHash $artifactTestHashes['zerohour-executable'] `
            $scalingBaselineHash | Out-Null
    } 'does not prove positive net speedup' `
        'scaling evidence rejects tampered aggregate kernel timing'

    $logicalOnlyScalingPath = Join-Path $acceptanceRoot 'scaling-logical-only.json'
    $logicalOnlyScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $logicalOnlyScaling.selectedLanes[1].selectedDistinctPhysicalCores = 4
    Write-JsonDocument $logicalOnlyScalingPath $logicalOnlyScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence $logicalOnlyScalingPath $sourceCommit `
            $artifactSetHash $artifactTestHashes['zerohour-executable'] `
            $scalingBaselineHash | Out-Null
    } 'exact selected logical and distinct physical-core count' `
        'scaling evidence rejects eight logical workers backed by only four physical cores'

    $tamperedAmdahlPath = Join-Path $acceptanceRoot 'scaling-tampered-amdahl.json'
    $tamperedAmdahl = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $tamperedAmdahl.amdahl.serialFraction = 0.1
    Write-JsonDocument $tamperedAmdahlPath $tamperedAmdahl
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence $tamperedAmdahlPath $sourceCommit `
            $artifactSetHash $artifactTestHashes['zerohour-executable'] `
            $scalingBaselineHash | Out-Null
    } 'Amdahl evidence does not prove' `
        'scaling evidence rejects a self-asserted Amdahl fraction that differs from phase timing'

    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence $scalingManifest $sourceCommit `
            $artifactSetHash $artifactTestHashes['zerohour-executable'] ('F' * 64) | Out-Null
    } 'provenance is invalid' `
        'scaling evidence rejects a Stage 3 regression baseline with a different independent hash'

    $manualAttachment = Join-Path $attachmentRoot 'manual-acceptance-manual-checklist.json'
    [IO.File]::AppendAllText($manualAttachment, 'tampered')
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'attachment.*SHA-256 mismatch' `
        'final acceptance independently rehashes and rejects a tampered attachment'
}
finally {
    $rootFull = [IO.Path]::GetFullPath($root)
    if ($rootFull.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($rootFull) -like 'GGC-Stage5Validation-Test-*') {
        Remove-Item -LiteralPath $rootFull -Recurse -Force
    }
}

if ($script:Failures -ne 0) {
    throw "$script:Failures deterministic simulation validation test(s) failed."
}
Write-Output 'Deterministic simulation validation script tests passed.'
