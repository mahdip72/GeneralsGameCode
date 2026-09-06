[CmdletBinding()]
param(
    [string]$SourceRoot = '',
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($SourceRoot)) { $SourceRoot = $PSScriptRoot }

function Assert-ReceiptTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

Assert-ReceiptTest ($PSVersionTable.PSVersion.Major -ge 7 -and
    $null -ne (Get-Command Test-Json -ErrorAction SilentlyContinue)) `
    'Stage5 local diagnostic receipt schema tests require PowerShell 7 Test-Json.'
$schemaPath = Join-Path $SourceRoot 'Stage5LocalLockstepDiagnostic.schema.json'
Assert-ReceiptTest (Test-Path -LiteralPath $schemaPath -PathType Leaf) `
    'Stage5 local diagnostic receipt schema is missing.'
$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
Assert-ReceiptTest ($schema.additionalProperties -eq $false -and
    [int]$schema.properties.schemaVersion.const -eq 1 -and
    [string]$schema.properties.evidenceKind.const -ceq
        'stage5-local-lockstep-diagnostic') `
    'Stage5 local diagnostic receipt schema must be strict and versioned.'

$shaA = 'A' * 64
$shaB = 'B' * 64
$shaC = 'C' * 64
$shaD = 'D' * 64
$profileRows = @(
    [ordered]@{ peer = 0; requestedWorkers = '2'; workerPolicy = 'all'; effectiveWorkers = 2 },
    [ordered]@{ peer = 1; requestedWorkers = 'auto'; workerPolicy = 'auto'; effectiveWorkers = 10 }
)
$workerRows = @(
    [ordered]@{
        title = 'Generals'; observedPeerRuns = 2
        requestedWorkerProfiles = $profileRows
        effectiveWorkerCounts = @(2, 10); effectiveWorkerTotal = 12
        finalCrcs = @(1, 1)
        comparableProjectionSha256 = $shaA
    },
    [ordered]@{
        title = 'ZeroHour'; observedPeerRuns = 2
        requestedWorkerProfiles = $profileRows
        effectiveWorkerCounts = @(2, 10); effectiveWorkerTotal = 12
        finalCrcs = @(1, 1)
        comparableProjectionSha256 = $shaA
    }
)
$peerProofs = @(
    [ordered]@{
        processId = 101; peer = 0; exitCode = 0; finalFrame = 4096; finalCRC = 1
        receiptSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC
        sourceCommit = 'a' * 40; executableSha256 = $shaD
        lockstepV2Receipt = $true; v1ReceiptAccepted = $false
    },
    [ordered]@{
        processId = 102; peer = 1; exitCode = 0; finalFrame = 4096; finalCRC = 1
        receiptSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC
        sourceCommit = 'a' * 40; executableSha256 = $shaD
        lockstepV2Receipt = $true; v1ReceiptAccepted = $false
    }
)
$sessionSamples = @(
    [ordered]@{
        title = 'Generals'; mapCrc = 739101722; peerCount = 2
        effectiveWorkerCounts = @(2, 10); mixedWorkerProof = $true
        comparableProjectionSha256 = $shaA; peers = $peerProofs
    },
    [ordered]@{
        title = 'ZeroHour'; mapCrc = 4042777579; peerCount = 2
        effectiveWorkerCounts = @(2, 10); mixedWorkerProof = $true
        comparableProjectionSha256 = $shaA; peers = $peerProofs
    }
)
$negativeSamples = @(
    [ordered]@{ title = 'Generals'; mode = 'negative-cross-epoch'; processId = 201; exitCode = 0; baselineAccepted = $true; mutatedAccepted = $false; sourceCommit = 'a' * 40; proofSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC },
    [ordered]@{ title = 'Generals'; mode = 'negative-content-mismatch'; processId = 202; exitCode = 0; baselineAccepted = $true; mutatedAccepted = $false; sourceCommit = 'a' * 40; proofSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC },
    [ordered]@{ title = 'ZeroHour'; mode = 'negative-cross-epoch'; processId = 203; exitCode = 0; baselineAccepted = $true; mutatedAccepted = $false; sourceCommit = 'a' * 40; proofSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC },
    [ordered]@{ title = 'ZeroHour'; mode = 'negative-content-mismatch'; processId = 204; exitCode = 0; baselineAccepted = $true; mutatedAccepted = $false; sourceCommit = 'a' * 40; proofSha256 = $shaA; stdoutSha256 = $shaB; stderrSha256 = $shaC }
)
$document = [ordered]@{
    schemaVersion = 1
    evidenceKind = 'stage5-local-lockstep-diagnostic'
    producer = 'installed-lockstep-v2-local-diagnostic-v1'
    status = 'passed'
    finalAcceptanceClaim = $false
    canonicalQualification = $false
    promotionGrant = $false
    externalQualificationSkipped = $true
    manualTestingDeferred = $true
    sourceCommit = 'a' * 40
    artifactSetSha256 = $shaA
    runtimeClosure = [ordered]@{
        dependencyManifestSha256 = $shaB; closureSha256 = $shaC
    }
    cohortNonce = '11111111-1111-4111-8111-111111111111'
    cohortCreatedUtc = '2026-09-05T12:00:00.0000000Z'
    recordedUtc = '2026-09-05T12:01:00.0000000Z'
    mapName = 'Maps\Twilight Flame\Twilight Flame.map'
    mapCrcs = [ordered]@{ Generals = 739101722; ZeroHour = 4042777579 }
    hostTopology = [ordered]@{
        source = 'GetSystemCpuSetInformation'; physicalCoreCount = 6
        logicalProcessorCount = 12
        cpuSets = @([ordered]@{
            id = 0; group = 0; logicalProcessorIndex = 0; coreIndex = 0
            parked = $false; allocated = $false; available = $true
        })
    }
    localData = [ordered]@{
        path = 'H:\Stage5LocalLockstepDiagnosticData.json'
        manifestSha256 = $shaA; closureSha256 = $shaB; fileCount = 12
        artifactSetSha256 = $shaA
    }
    runCounts = [ordered]@{
        expectedTitleSessions = 2; observedTitleSessions = 2
        expectedPeerRuns = 4; observedPeerRuns = 4
        expectedNegativeProbes = 4; observedNegativeProbes = 4
    }
    workerCapacity = [ordered]@{
        maximumTotalEffectiveWorkers = 12
        predictedEffectiveWorkerTotal = 12
        observedMaximumEffectiveWorkers = 12
        byTitle = $workerRows
    }
    requested = [ordered]@{
        peerCount = 2; seed = 23063; peerTimeoutSeconds = 300
        allowHeadlessDirectExecution = $true
    }
    # Native/session validators own the detailed peer and negative-proof shape;
    # this schema binds their two title and four mutation result collections.
    sessions = $sessionSamples
    negativeProbeResults = $negativeSamples
}

$positiveJson = $document | ConvertTo-Json -Depth 20
Assert-ReceiptTest ([bool]($positiveJson | Test-Json -SchemaFile $schemaPath `
    -ErrorAction Stop)) 'Valid local diagnostic receipt did not satisfy its schema.'

foreach ($mutation in @(
        [pscustomobject]@{ name = 'final acceptance claim'; apply = {
            param($value); $value.finalAcceptanceClaim = $true
        } },
        [pscustomobject]@{ name = 'unexpected field'; apply = {
            param($value); $value | Add-Member -NotePropertyName extraField -NotePropertyValue $true
        } },
        [pscustomobject]@{ name = 'observed peer count'; apply = {
            param($value); $value.runCounts.observedPeerRuns = 3
        } })) {
    $candidate = $positiveJson | ConvertFrom-Json
    $apply = $mutation.apply
    & $apply $candidate
    $candidateIsValid = $false
    try {
        $candidateIsValid = [bool](($candidate | ConvertTo-Json -Depth 20) |
            Test-Json -SchemaFile $schemaPath -ErrorAction Stop)
    }
    catch { $candidateIsValid = $false }
    Assert-ReceiptTest (-not $candidateIsValid) `
        "Schema accepted mutated $($mutation.name) local diagnostic receipt."
}

# Exercise the actual shipped writer against a complete small in-memory session
# result.  The native boundary is not launched here; all native proof fields are
# represented with the same types and identities that the shared session emits.
$sessionModulePath = Join-Path $SourceRoot 'Stage5InstalledLockstepV2Session.psm1'
$entrypointPath = Join-Path $SourceRoot 'Invoke-Stage5LocalLockstepDiagnostic.ps1'
Assert-ReceiptTest (Test-Path -LiteralPath $sessionModulePath -PathType Leaf) `
    'Shared installed-lockstep-v2 session module is missing.'
Assert-ReceiptTest (Test-Path -LiteralPath $entrypointPath -PathType Leaf) `
    'Local diagnostic entrypoint is missing.'
Import-Module $sessionModulePath -ErrorAction Stop -DisableNameChecking
$entrypointTokens = $null
$entrypointErrors = $null
$entrypointAst = [Management.Automation.Language.Parser]::ParseFile(
    $entrypointPath, [ref]$entrypointTokens, [ref]$entrypointErrors)
Assert-ReceiptTest ($entrypointErrors.Count -eq 0) `
    'Local diagnostic entrypoint does not parse for writer extraction.'
foreach ($name in @(
        'Assert-LocalDiagnosticCondition', 'Get-LocalDiagnosticProperty',
        'Assert-LocalDiagnosticSha256', 'Assert-LocalDiagnosticIdentity',
        'Write-Stage5LocalLockstepDiagnosticReceipt')) {
    $definition = @($entrypointAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $name
    }, $true))
    Assert-ReceiptTest ($definition.Count -eq 1) `
        "Local diagnostic writer function '$name' is missing."
    . ([scriptblock]::Create($definition[0].Extent.Text))
}

$writerScratch = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else { $ScratchRoot }
Assert-ReceiptTest (-not [string]::IsNullOrWhiteSpace($writerScratch)) `
    'Receipt writer tests require -ScratchRoot or RTS_STAGE5_VALIDATION_SCRATCH_ROOT.'
$writerScratch = [IO.Path]::GetFullPath($writerScratch)
Assert-ReceiptTest (Test-Path -LiteralPath $writerScratch -PathType Container) `
    "Receipt writer scratch parent is not an existing directory: $writerScratch"
$writerRoot = Join-Path $writerScratch ('stage5-local-receipt-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($writerRoot) | Out-Null
$sourceCommit = 'a' * 40
$runtimeClosure = [ordered]@{
    dependencyManifestSha256 = $shaB
    closureSha256 = $shaC
}
$cohortNonce = '11111111-1111-4111-8111-111111111111'
$cohortCreatedUtc = '2026-09-05T12:00:00.0000000Z'
$mapName = 'Maps\Twilight Flame\Twilight Flame.map'
$mapCrcs = [ordered]@{ Generals = [uint32]739101722; ZeroHour = [uint32]4042777579 }
$hostTopology = [pscustomobject]@{
    source = 'GetSystemCpuSetInformation'; physicalCoreCount = 6
    logicalProcessorCount = 12
    cpuSets = @([pscustomobject]@{
        id = [uint32]0; group = [uint16]0; logicalProcessorIndex = [byte]0
        coreIndex = [byte]0; parked = $false; allocated = $false; available = $true
    })
}
$localDataBinding = [pscustomobject]@{
    path = Join-Path $writerRoot 'Stage5LocalLockstepDiagnosticData.json'
    manifestSha256 = $shaB
    closureSha256 = $shaD
    fileCount = 12
    artifactSetSha256 = $shaA
    runtimeClosure = $runtimeClosure
    document = [pscustomobject]@{ mapName = $mapName }
}

function New-SyntheticLocalDiagnosticSessionResult {
    $profiles = @(
        [pscustomobject]@{ requestedWorkers = '2'; workerPolicy = 'all' },
        [pscustomobject]@{ requestedWorkers = 'auto'; workerPolicy = 'auto' }
    )
    $makeSession = {
        param([string]$Title, [uint32]$MapCrc)
        $peers = @(
            [pscustomobject]@{
                processId = 101; peer = 0; exitCode = 0; finalFrame = 4096
                finalCRC = [uint32]1; receiptSha256 = $shaA
                stdoutSha256 = $shaB; stderrSha256 = $shaC
                sourceCommit = $sourceCommit; executableSha256 = $shaD
                lockstepV2Receipt = $true; v1ReceiptAccepted = $false
            },
            [pscustomobject]@{
                processId = 102; peer = 1; exitCode = 0; finalFrame = 4096
                finalCRC = [uint32]1; receiptSha256 = $shaA
                stdoutSha256 = $shaB; stderrSha256 = $shaC
                sourceCommit = $sourceCommit; executableSha256 = $shaD
                lockstepV2Receipt = $true; v1ReceiptAccepted = $false
            }
        )
        return [pscustomobject]@{
            title = $Title; mapCrc = $MapCrc; peerCount = 2
            workerProfiles = @($profiles)
            effectiveWorkerCounts = @([int]2, [int]10)
            mixedWorkerProof = $true; comparableProjectionSha256 = $shaA
            peers = $peers
        }
    }
    $negative = @()
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($mode in @('negative-cross-epoch', 'negative-content-mismatch')) {
            $negative += [pscustomobject]@{
                title = $title; mode = $mode; processId = 201; exitCode = 0
                baselineAccepted = $true; mutatedAccepted = $false
                sourceCommit = $sourceCommit; proofSha256 = $shaA
                stdoutSha256 = $shaB; stderrSha256 = $shaC
            }
        }
    }
    return [pscustomobject]@{
        sourceCommit = $sourceCommit
        artifactSet = [pscustomobject]@{ sha256 = $shaA }
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = $runtimeClosure.dependencyManifestSha256
            closureSha256 = $runtimeClosure.closureSha256
        }
        cohortNonce = $cohortNonce
        cohortCreatedUtc = $cohortCreatedUtc
        mapCrcs = $mapCrcs
        executables = [ordered]@{ Generals = $shaD; ZeroHour = $shaD }
        sessionResults = @(
            & $makeSession 'Generals' $mapCrcs.Generals
            & $makeSession 'ZeroHour' $mapCrcs.ZeroHour
        )
        negativeProbeResults = @($negative)
    }
}

function Assert-SyntheticWriterRejects {
    param([string]$Name, [scriptblock]$Mutate)
    $candidate = New-SyntheticLocalDiagnosticSessionResult
    & $Mutate $candidate
    $path = Join-Path $writerRoot ("rejected-{0}.json" -f $Name)
    $caught = $false
    try {
        Write-Stage5LocalLockstepDiagnosticReceipt `
            -Path $path -SessionResult $candidate `
            -LocalDataBinding $localDataBinding -HostTopology $hostTopology `
            -ExpectedSourceCommit $sourceCommit `
            -ExpectedRuntimeClosure $runtimeClosure `
            -ExpectedCohortNonce $cohortNonce `
            -ExpectedCohortCreatedUtc $cohortCreatedUtc `
            -ExpectedMapName $mapName -ExpectedMapCrcs $mapCrcs `
            -ExpectedEffectiveWorkerTotal 12 `
            -MaximumTotalEffectiveWorkers 12 -PeerCount 2 -Seed 23063 `
            -PeerTimeoutSeconds 300 -AllowHeadlessDirectExecution $true | Out-Null
    }
    catch { $caught = $true }
    Assert-ReceiptTest $caught "Receipt writer accepted mutated $Name proof."
}

$writerResult = Write-Stage5LocalLockstepDiagnosticReceipt `
    -Path (Join-Path $writerRoot 'Stage5LocalLockstepDiagnostic.json') `
    -SessionResult (New-SyntheticLocalDiagnosticSessionResult) `
    -LocalDataBinding $localDataBinding -HostTopology $hostTopology `
    -ExpectedSourceCommit $sourceCommit -ExpectedRuntimeClosure $runtimeClosure `
    -ExpectedCohortNonce $cohortNonce -ExpectedCohortCreatedUtc $cohortCreatedUtc `
    -ExpectedMapName $mapName -ExpectedMapCrcs $mapCrcs `
    -ExpectedEffectiveWorkerTotal 12 -MaximumTotalEffectiveWorkers 12 `
    -PeerCount 2 -Seed 23063 -PeerTimeoutSeconds 300 `
    -AllowHeadlessDirectExecution $true
$writerJson = Get-Content -LiteralPath $writerResult.path -Raw
Assert-ReceiptTest ([bool]($writerJson | Test-Json -SchemaFile $schemaPath `
    -ErrorAction Stop)) 'Actual local diagnostic writer output failed its schema.'
Assert-ReceiptTest (-not [bool]$writerResult.finalAcceptanceClaim -and
    [bool]$writerResult.externalQualificationSkipped -and
    [bool]$writerResult.manualTestingDeferred) `
    'Actual local diagnostic writer output lost its non-final disposition.'

Assert-SyntheticWriterRejects 'source' {
    param($value); $value.sourceCommit = 'b' * 40
}
Assert-SyntheticWriterRejects 'cohort' {
    param($value); $value.cohortNonce = '22222222-2222-4222-8222-222222222222'
}
Assert-SyntheticWriterRejects 'runtime-closure' {
    param($value); $value.runtimeClosure.closureSha256 = $shaD
}
Assert-SyntheticWriterRejects 'native-exit' {
    param($value); $value.sessionResults[0].peers[0].exitCode = 1
}
Assert-SyntheticWriterRejects 'replay-crc' {
    param($value); $value.sessionResults[0].peers[1].finalCRC = [uint32]2
}
Assert-SyntheticWriterRejects 'effective-workers' {
    param($value); $value.sessionResults[0].effectiveWorkerCounts[1] = [int]9
}

if (Test-Path -LiteralPath $writerRoot) {
    [IO.Directory]::Delete($writerRoot, $true)
}

Write-Output 'Stage5 local lockstep diagnostic receipt schema tests passed.'
