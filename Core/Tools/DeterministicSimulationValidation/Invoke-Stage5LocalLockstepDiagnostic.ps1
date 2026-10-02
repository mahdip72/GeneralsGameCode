[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ValidationSourceRoot,
    [Parameter(Mandatory = $true)][string]$GeneralsExecutable,
    [Parameter(Mandatory = $true)][string]$ZeroHourExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
    [Parameter(Mandatory = $true)][string]$LocalDataManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceCommit,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$MapName,
    [Parameter(Mandatory = $true)][uint32]$GeneralsMapCrc,
    [Parameter(Mandatory = $true)][uint32]$ZeroHourMapCrc,
    [ValidateRange(2, 2)][int]$PeerCount = 2,
    [ValidateRange(1024, 65000)][int]$BasePort = 41000,
    [ValidateRange(1, 2147483646)][int]$Seed = 23063,
    [ValidateRange(30, 1800)][int]$PeerTimeoutSeconds = 300,
    [ValidateRange(1, 12)][int]$MaximumTotalEffectiveWorkers = 12,
    [Parameter(Mandatory = $true)][string]$ExecutionCohortNonce,
    [Parameter(Mandatory = $true)][string]$ExecutionCohortCreatedUtc,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$RuntimeClosureDependencyManifestSha256,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$RuntimeClosureSha256,
    [switch]$AllowHeadlessDirectExecution
)

# This is a non-canonical local diagnostic route.  It has its own data reader
# and receipt disposition; it never imports or evaluates the canonical runner,
# archive qualification reader, or final-acceptance envelope writer.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-LocalDiagnosticCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Stage5LocalLockstepExpectedEffectiveWorkerTotal {
    param([ValidateRange(0, 12)][int]$LogicalProcessorCount)
    $reserved = if ($LogicalProcessorCount -ge 8) { 2 } else { 1 }
    $automaticWorkers = if ($LogicalProcessorCount -eq 0) {
        1
    }
    elseif ($LogicalProcessorCount -gt $reserved) {
        $LogicalProcessorCount - $reserved
    }
    else { 1 }
    # The installed session always launches one explicit-two peer and one
    # automatic peer per title.  This is a pre-launch budget prediction only;
    # the receipt writer still records and checks each observed count.
    return [int](2 + $automaticWorkers)
}

function Assert-Stage5LocalLockstepWorkerBudget {
    param(
        [ValidateRange(0, 12)][int]$LogicalProcessorCount,
        [ValidateRange(1, 12)][int]$MaximumTotalEffectiveWorkers
    )
    $expected = Get-Stage5LocalLockstepExpectedEffectiveWorkerTotal `
        $LogicalProcessorCount
    if ($MaximumTotalEffectiveWorkers -lt $expected) {
        throw "Local diagnostic worker budget $MaximumTotalEffectiveWorkers is below the predicted two-peer total $expected for $LogicalProcessorCount available logical processors."
    }
    return $expected
}

function Get-LocalDiagnosticProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-LocalDiagnosticCondition ($null -ne $Object) "$Context is null."
    if ($Object -is [Collections.IDictionary]) {
        Assert-LocalDiagnosticCondition ($Object.Contains($Name)) `
            "$Context is missing '$Name'."
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    Assert-LocalDiagnosticCondition ($null -ne $property) `
        "$Context is missing '$Name'."
    return $property.Value
}

function Assert-LocalDiagnosticSha256 {
    param([string]$Value, [string]$Context)
    Assert-LocalDiagnosticCondition ($Value -is [string] -and
        $Value -cmatch '^[0-9A-F]{64}$') "$Context is not an uppercase SHA-256."
}

function Assert-LocalDiagnosticIdentity {
    param(
        [object]$SessionResult,
        [object]$LocalDataBinding,
        [object]$HostTopology,
        [string]$ExpectedSourceCommit,
        [object]$ExpectedRuntimeClosure,
        [string]$ExpectedCohortNonce,
        [string]$ExpectedCohortCreatedUtc,
        [string]$ExpectedMapName,
        [object]$ExpectedMapCrcs,
        [int]$MaximumWorkers
    )
    $source = [string](Get-LocalDiagnosticProperty $SessionResult `
        'sourceCommit' 'Shared lockstep session result')
    Assert-LocalDiagnosticCondition ($source -ceq $ExpectedSourceCommit) `
        'Local diagnostic session source identity changed.'
    $artifactBinding = Get-LocalDiagnosticProperty $SessionResult `
        'artifactSet' 'Shared lockstep session result'
    $artifactSha = [string]$artifactBinding.sha256
    Assert-LocalDiagnosticSha256 $artifactSha 'Shared artifact-set identity'
    Assert-LocalDiagnosticCondition ($artifactSha -ceq
        [string](Get-LocalDiagnosticProperty $LocalDataBinding `
            'artifactSetSha256' 'Local data binding')) `
        'Local diagnostic data and session artifact identities differ.'

    $sessionClosure = Get-LocalDiagnosticProperty $SessionResult `
        'runtimeClosure' 'Shared lockstep session result'
    $expectedDependency = ([string](Get-LocalDiagnosticProperty `
        $ExpectedRuntimeClosure 'dependencyManifestSha256' 'Expected runtime closure')).ToUpperInvariant()
    $expectedClosure = ([string](Get-LocalDiagnosticProperty `
        $ExpectedRuntimeClosure 'closureSha256' 'Expected runtime closure')).ToUpperInvariant()
    Assert-LocalDiagnosticCondition (
        [string]$sessionClosure.dependencyManifestSha256 -ceq $expectedDependency -and
        [string]$sessionClosure.closureSha256 -ceq $expectedClosure
    ) 'Local diagnostic session runtime closure changed.'
    Assert-LocalDiagnosticCondition (
        [string](Get-LocalDiagnosticProperty $LocalDataBinding `
            'runtimeClosure' 'Local data binding').closureSha256 -ceq $expectedClosure
    ) 'Local diagnostic data runtime closure changed.'

    Assert-LocalDiagnosticCondition (
        [string](Get-LocalDiagnosticProperty $SessionResult 'cohortNonce' `
            'Shared lockstep session result') -ceq $ExpectedCohortNonce -and
        [string](Get-LocalDiagnosticProperty $SessionResult 'cohortCreatedUtc' `
            'Shared lockstep session result') -ceq $ExpectedCohortCreatedUtc
    ) 'Local diagnostic session execution cohort changed.'
    $mapCrcs = Get-LocalDiagnosticProperty $SessionResult 'mapCrcs' `
        'Shared lockstep session result'
    Assert-LocalDiagnosticCondition (
        [uint32]$mapCrcs.Generals -eq [uint32]$ExpectedMapCrcs.Generals -and
        [uint32]$mapCrcs.ZeroHour -eq [uint32]$ExpectedMapCrcs.ZeroHour
    ) 'Local diagnostic session map CRC binding changed.'
    $localDataDocument = Get-LocalDiagnosticProperty $LocalDataBinding `
        'document' 'Local data binding'
    Assert-LocalDiagnosticCondition ([string]$localDataDocument.mapName -ceq $ExpectedMapName) `
        'Local diagnostic data map identity changed.'
    Assert-LocalDiagnosticCondition (
        [string](Get-LocalDiagnosticProperty $SessionResult 'sourceCommit' `
            'Shared lockstep session result') -cmatch '^[0-9a-f]{40}$'
    ) 'Local diagnostic source commit is not lowercase canonical hex.'

    $physical = [int](Get-LocalDiagnosticProperty $HostTopology `
        'physicalCoreCount' 'Host topology')
    $logical = [int](Get-LocalDiagnosticProperty $HostTopology `
        'logicalProcessorCount' 'Host topology')
    Assert-LocalDiagnosticCondition ($physical -ge 1 -and $physical -le 6 -and
        $logical -ge 1 -and $logical -le 12) `
        'Local diagnostic host topology is outside the six-physical/twelve-logical bound.'
    Assert-LocalDiagnosticCondition ($MaximumWorkers -ge 1 -and $MaximumWorkers -le 12) `
        'Local diagnostic worker bound exceeds the supported twelve-worker cap.'
}

function Write-Stage5LocalLockstepDiagnosticReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$SessionResult,
        [Parameter(Mandatory = $true)][object]$LocalDataBinding,
        [Parameter(Mandatory = $true)][object]$HostTopology,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][object]$ExpectedRuntimeClosure,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$ExpectedMapName,
        [Parameter(Mandatory = $true)][object]$ExpectedMapCrcs,
        [ValidateRange(1, 12)][int]$ExpectedEffectiveWorkerTotal,
        [ValidateRange(1, 12)][int]$MaximumTotalEffectiveWorkers = 12,
        [ValidateRange(2, 2)][int]$PeerCount = 2,
        [ValidateRange(1, 2147483646)][int]$Seed = 23063,
        [ValidateRange(30, 1800)][int]$PeerTimeoutSeconds = 300,
        [bool]$AllowHeadlessDirectExecution = $true
    )
    Assert-LocalDiagnosticIdentity $SessionResult $LocalDataBinding $HostTopology `
        $ExpectedSourceCommit $ExpectedRuntimeClosure $ExpectedCohortNonce `
        $ExpectedCohortCreatedUtc $ExpectedMapName $ExpectedMapCrcs `
        $MaximumTotalEffectiveWorkers
    Assert-LocalDiagnosticCondition ($ExpectedEffectiveWorkerTotal -ge 1 -and
        $ExpectedEffectiveWorkerTotal -le 12 -and
        $MaximumTotalEffectiveWorkers -ge $ExpectedEffectiveWorkerTotal) `
        'Local diagnostic expected worker total is outside the bounded requested budget.'
    Assert-LocalDiagnosticCondition ($AllowHeadlessDirectExecution -and $PeerCount -eq 2) `
        'Local diagnostic requested execution settings are not the bounded two-peer contract.'
    $sessions = @((Get-LocalDiagnosticProperty $SessionResult `
        'sessionResults' 'Shared lockstep session result'))
    $negative = @((Get-LocalDiagnosticProperty $SessionResult `
        'negativeProbeResults' 'Shared lockstep session result'))
    Assert-LocalDiagnosticCondition ($sessions.Count -eq 2) `
        'Local diagnostic must retain exactly two title sessions.'
    Assert-LocalDiagnosticCondition ($negative.Count -eq 4) `
        'Local diagnostic must retain exactly four negative probes.'
    $expectedTitles = @('Generals', 'ZeroHour')
    $sessionSummaries = New-Object 'Collections.Generic.List[object]'
    $observedPeerRuns = 0
    $observedEffectiveWorkers = 0L
    foreach ($index in 0..1) {
        $session = $sessions[$index]
        $title = [string](Get-LocalDiagnosticProperty $session 'title' `
            "Local diagnostic session $index")
        Assert-LocalDiagnosticCondition ($title -ceq $expectedTitles[$index]) `
            "Local diagnostic title order or identity changed at index $index."
        $mapCrc = if ($title -ceq 'Generals') {
            [uint32]$ExpectedMapCrcs.Generals
        }
        else { [uint32]$ExpectedMapCrcs.ZeroHour }
        Assert-LocalDiagnosticCondition ($session.mapCrc -is [uint32] -and
            [uint32]$session.mapCrc -eq $mapCrc) `
            "Local diagnostic $title map CRC changed."
        Assert-LocalDiagnosticCondition ($session.peerCount -is [int] -and
            [int]$session.peerCount -eq 2 -and
            @($session.peers).Count -eq 2) `
            "Local diagnostic $title did not retain exactly two peer runs."
        $observedPeerRuns += @($session.peers).Count
        Assert-LocalDiagnosticSha256 ([string]$session.comparableProjectionSha256) `
            "$title comparable receipt projection"
        Assert-LocalDiagnosticCondition ([bool]$session.mixedWorkerProof) `
            "$title did not retain the mixed-worker proof."

        $profiles = @($session.workerProfiles)
        $effective = @($session.effectiveWorkerCounts)
        Assert-LocalDiagnosticCondition ($profiles.Count -eq 2 -and
            $effective.Count -eq 2) "$title worker counts are incomplete."
        $effectiveTotal = 0L
        $expectedAutomaticWorkers = $ExpectedEffectiveWorkerTotal - 2
        Assert-LocalDiagnosticCondition ($expectedAutomaticWorkers -ge 1) `
            "$title expected automatic worker count is not positive."
        $profileSummary = New-Object 'Collections.Generic.List[object]'
        for ($peer = 0; $peer -lt 2; ++$peer) {
            $profile = $profiles[$peer]
            $requested = [string](Get-LocalDiagnosticProperty $profile `
                'requestedWorkers' "$title peer $peer worker profile")
            $policy = [string](Get-LocalDiagnosticProperty $profile `
                'workerPolicy' "$title peer $peer worker profile")
            $workerValue = $effective[$peer]
            Assert-LocalDiagnosticCondition ($workerValue -is [int] -and
                [int]$workerValue -ge 1) `
                "$title peer $peer effective worker count is not a positive integer."
            if ($peer -eq 0) {
                Assert-LocalDiagnosticCondition ($requested -ceq '2' -and
                    $policy -ceq 'all' -and [int]$workerValue -eq 2) `
                    "$title explicit worker profile binding changed."
            }
            else {
                Assert-LocalDiagnosticCondition ($requested -ceq 'auto' -and
                    $policy -ceq 'auto' -and [int]$workerValue -eq
                        $expectedAutomaticWorkers) `
                    "$title automatic worker profile binding changed."
            }
            $effectiveTotal += [int]$workerValue
            $profileSummary.Add([ordered]@{
                peer = $peer
                requestedWorkers = $requested
                workerPolicy = $policy
                effectiveWorkers = [int]$workerValue
            }) | Out-Null
        }
        Assert-LocalDiagnosticCondition ($effectiveTotal -le
            [int64]$MaximumTotalEffectiveWorkers) `
            "$title observed $effectiveTotal effective workers; bound is $MaximumTotalEffectiveWorkers."
        $observedEffectiveWorkers = [Math]::Max($observedEffectiveWorkers, $effectiveTotal)
        $expectedFinalCrc = $null
        foreach ($peer in @($session.peers)) {
            Assert-LocalDiagnosticCondition ($peer.processId -is [int] -and
                $peer.exitCode -is [int] -and $peer.finalFrame -is [int] -and
                [int]$peer.processId -gt 0 -and [int]$peer.exitCode -eq 0 -and
                [int]$peer.finalFrame -eq 4096 -and
                $peer.finalCRC -is [uint32] -and
                [bool]$peer.lockstepV2Receipt -and
                -not [bool]$peer.v1ReceiptAccepted) `
                "$title peer $($peer.peer) did not retain a successful native v2 exit proof."
            if ($null -eq $expectedFinalCrc) {
                $expectedFinalCrc = [uint32]$peer.finalCRC
            }
            else {
                Assert-LocalDiagnosticCondition ([uint32]$peer.finalCRC -eq
                    $expectedFinalCrc) `
                    "$title peers disagreed on the final replay CRC."
            }
            Assert-LocalDiagnosticSha256 ([string]$peer.receiptSha256) `
                "$title peer receipt"
            Assert-LocalDiagnosticSha256 ([string]$peer.stdoutSha256) `
                "$title peer stdout"
            Assert-LocalDiagnosticSha256 ([string]$peer.stderrSha256) `
                "$title peer stderr"
            Assert-LocalDiagnosticCondition ([string]$peer.sourceCommit -ceq $ExpectedSourceCommit -and
                [string]$peer.executableSha256 -ceq
                    [string](Get-LocalDiagnosticProperty $SessionResult `
                        'executables' 'Shared executable identities')[$title]) `
                "$title peer source or executable binding changed."
        }
        $sessionSummaries.Add([ordered]@{
            title = $title
            observedPeerRuns = @($session.peers).Count
            requestedWorkerProfiles = @($profileSummary.ToArray())
            effectiveWorkerCounts = @($effective | ForEach-Object { [int]$_ })
            effectiveWorkerTotal = [int64]$effectiveTotal
            finalCrcs = @($session.peers | ForEach-Object { [uint32]$_.finalCRC })
            comparableProjectionSha256 = [string]$session.comparableProjectionSha256
        }) | Out-Null
    }
    Assert-LocalDiagnosticCondition ($observedPeerRuns -eq 4) `
        "Local diagnostic observed $observedPeerRuns peer runs; expected 4."

    $negativeKeys = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::Ordinal)
    foreach ($probe in $negative) {
        $title = [string]$probe.title
        $mode = [string]$probe.mode
        Assert-LocalDiagnosticCondition ($expectedTitles -ccontains $title -and
            @('negative-cross-epoch', 'negative-content-mismatch') -ccontains $mode) `
            'Local diagnostic retained an unsupported negative-probe identity.'
        Assert-LocalDiagnosticCondition ($negativeKeys.Add("$title|$mode")) `
            "Local diagnostic retained a duplicate negative probe: $title/$mode."
        Assert-LocalDiagnosticCondition ($probe.processId -is [int] -and
            $probe.exitCode -is [int] -and
            $probe.baselineAccepted -is [bool] -and $probe.mutatedAccepted -is [bool] -and
            [int]$probe.processId -gt 0 -and [bool]$probe.baselineAccepted -and
            -not [bool]$probe.mutatedAccepted -and [int]$probe.exitCode -eq 0 -and
            [string]$probe.sourceCommit -ceq $ExpectedSourceCommit) `
            "Local diagnostic negative probe $title/$mode did not retain the rejection proof."
        Assert-LocalDiagnosticSha256 ([string]$probe.proofSha256) `
            "$title/$mode negative proof"
        Assert-LocalDiagnosticSha256 ([string]$probe.stdoutSha256) `
            "$title/$mode negative stdout"
        Assert-LocalDiagnosticSha256 ([string]$probe.stderrSha256) `
            "$title/$mode negative stderr"
    }
    Assert-LocalDiagnosticCondition ($negativeKeys.Count -eq 4) `
        'Local diagnostic did not retain one negative probe for each title and mutation.'

    $cohort = [string](Get-LocalDiagnosticProperty $SessionResult 'cohortNonce' `
        'Shared lockstep session result')
    $cohortUtc = [string](Get-LocalDiagnosticProperty $SessionResult `
        'cohortCreatedUtc' 'Shared lockstep session result')
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
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = [string]$SessionResult.artifactSet.sha256
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = ([string]$ExpectedRuntimeClosure.dependencyManifestSha256).ToUpperInvariant()
            closureSha256 = ([string]$ExpectedRuntimeClosure.closureSha256).ToUpperInvariant()
        }
        cohortNonce = $cohort
        cohortCreatedUtc = $cohortUtc
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        mapName = $ExpectedMapName
        mapCrcs = [ordered]@{
            Generals = [uint32]$ExpectedMapCrcs.Generals
            ZeroHour = [uint32]$ExpectedMapCrcs.ZeroHour
        }
        hostTopology = [ordered]@{
            source = [string]$HostTopology.source
            physicalCoreCount = [int]$HostTopology.physicalCoreCount
            logicalProcessorCount = [int]$HostTopology.logicalProcessorCount
            cpuSets = @($HostTopology.cpuSets)
        }
        localData = [ordered]@{
            path = [string]$LocalDataBinding.path
            manifestSha256 = [string]$LocalDataBinding.manifestSha256
            closureSha256 = [string]$LocalDataBinding.closureSha256
            fileCount = [int]$LocalDataBinding.fileCount
            artifactSetSha256 = [string]$LocalDataBinding.artifactSetSha256
        }
        runCounts = [ordered]@{
            expectedTitleSessions = 2
            observedTitleSessions = $sessions.Count
            expectedPeerRuns = 4
            observedPeerRuns = $observedPeerRuns
            expectedNegativeProbes = 4
            observedNegativeProbes = $negative.Count
        }
        workerCapacity = [ordered]@{
            maximumTotalEffectiveWorkers = $MaximumTotalEffectiveWorkers
            predictedEffectiveWorkerTotal = $ExpectedEffectiveWorkerTotal
            observedMaximumEffectiveWorkers = [int64]$observedEffectiveWorkers
            byTitle = @($sessionSummaries.ToArray())
        }
        requested = [ordered]@{
            peerCount = $PeerCount
            seed = $Seed
            peerTimeoutSeconds = $PeerTimeoutSeconds
            allowHeadlessDirectExecution = [bool]$AllowHeadlessDirectExecution
        }
        sessions = $sessions
        negativeProbeResults = $negative
    }
    $full = [IO.Path]::GetFullPath($Path)
    $parent = Split-Path -Parent $full
    Assert-LocalDiagnosticCondition (Test-SafeHDirectory $parent) `
        'Local diagnostic receipt must be written under an H: output root.'
    Assert-LockstepNoReparse $parent 'local diagnostic receipt parent'
    Assert-LocalDiagnosticCondition (-not (Test-Path -LiteralPath $full)) `
        "Local diagnostic receipt must be fresh: $full"
    Write-AtomicText $full ($document | ConvertTo-Json -Depth 20)
    Assert-LockstepNoReparse $full 'local diagnostic receipt' $parent
    $writtenSha = Get-UpperSha256 $full
    Assert-LocalDiagnosticSha256 $writtenSha 'Written local diagnostic receipt'
    return [pscustomobject]@{
        path = $full
        sha256 = $writtenSha
        finalAcceptanceClaim = [bool]$document.finalAcceptanceClaim
        externalQualificationSkipped = [bool]$document.externalQualificationSkipped
        manualTestingDeferred = [bool]$document.manualTestingDeferred
        document = $document
    }
}

$sessionModulePath = Join-Path $ValidationSourceRoot 'Stage5InstalledLockstepV2Session.psm1'
$localDataModulePath = Join-Path $ValidationSourceRoot 'Stage5LocalLockstepDiagnosticData.psm1'
foreach ($modulePath in @($sessionModulePath, $localDataModulePath)) {
    Assert-LocalDiagnosticCondition (Test-Path -LiteralPath $modulePath -PathType Leaf) `
        "Required local diagnostic module is missing: $modulePath"
}
Import-Module $sessionModulePath -ErrorAction Stop
Import-Module $localDataModulePath -ErrorAction Stop
# Clear any caller-process self-test exception before validating production
# local-diagnostic paths; the session boundary repeats this guard at invocation.
Set-Stage5LockstepHostSelfTestScratchRoot $null

Assert-LocalDiagnosticCondition ($AllowHeadlessDirectExecution.IsPresent) `
    'Local lockstep diagnostic requires the explicit headless-direct opt-in.'
Assert-LocalDiagnosticCondition ($SourceCommit -cmatch '^[0-9a-f]{40}$') `
    'Local lockstep diagnostic SourceCommit must be lowercase 40-hex.'
Assert-LocalDiagnosticCondition ($PeerCount -eq 2) `
    'Local lockstep diagnostic requires exactly two network peers.'
Assert-LocalDiagnosticCondition ($GeneralsMapCrc -ne 0 -and $ZeroHourMapCrc -ne 0) `
    'Local lockstep diagnostic requires nonzero per-title map CRCs.'
Assert-LocalDiagnosticCondition (Test-SafeHDirectory $OutputDirectory) `
    'Local lockstep diagnostic output must be on H:.'
Assert-LocalDiagnosticCondition (-not (Test-Path -LiteralPath $OutputDirectory)) `
    "Local lockstep diagnostic output must be fresh: $OutputDirectory"

$expectedRuntimeClosure = [ordered]@{
    dependencyManifestSha256 = $RuntimeClosureDependencyManifestSha256.ToUpperInvariant()
    closureSha256 = $RuntimeClosureSha256.ToUpperInvariant()
}
$expectedMapCrcs = [ordered]@{
    Generals = [uint32]$GeneralsMapCrc
    ZeroHour = [uint32]$ZeroHourMapCrc
}

# Preserve the authoritative full runtime-closure validator before consulting
# the local twelve-file data contract.  The local data module is intentionally
# not treated as proof of the installed DLL/asset closure.
$artifactSet = Read-AndValidateArtifactSet $ArtifactSetManifestPath $SourceCommit
$artifactRuntimeClosure = Assert-LockstepRuntimeClosure $artifactSet.runtimeClosure `
    'Artifact-set runtime closure'
Assert-LocalDiagnosticCondition (
    $artifactRuntimeClosure.dependencyManifestSha256 -ceq
        $expectedRuntimeClosure.dependencyManifestSha256 -and
    $artifactRuntimeClosure.closureSha256 -ceq $expectedRuntimeClosure.closureSha256
) 'Requested runtime closure does not match the validated artifact-set closure.'

$hostTopology = Get-Stage5InstalledLockstepV2HostTopology `
    -MinimumPhysicalCores 1 -MaximumPhysicalCores 6 -MaximumLogicalProcessors 12
$predictedEffectiveWorkerTotal = Assert-Stage5LocalLockstepWorkerBudget `
    $hostTopology.logicalProcessorCount $MaximumTotalEffectiveWorkers

$localData = Read-Stage5LocalLockstepDiagnosticData `
    -ManifestPath $LocalDataManifestPath `
    -ArtifactSetManifestPath $ArtifactSetManifestPath `
    -GeneralsExecutable $GeneralsExecutable `
    -ZeroHourExecutable $ZeroHourExecutable `
    -ExpectedSourceCommit $SourceCommit `
    -ExpectedRuntimeClosure $expectedRuntimeClosure `
    -ExpectedMapName $MapName `
    -ExpectedMapCrcs $expectedMapCrcs `
    -ExpectedCohortNonce $ExecutionCohortNonce `
    -ExpectedCohortCreatedUtc $ExecutionCohortCreatedUtc

$sessionResult = Invoke-Stage5InstalledLockstepV2SessionSet `
    -GeneralsExecutable $GeneralsExecutable `
    -ZeroHourExecutable $ZeroHourExecutable `
    -ArtifactSetManifestPath $ArtifactSetManifestPath `
    -SourceCommit $SourceCommit `
    -OutputDirectory $OutputDirectory `
    -MapName $MapName `
    -MapCrcs $expectedMapCrcs `
    -PeerCount $PeerCount `
    -BasePort $BasePort `
    -Seed $Seed `
    -PeerTimeoutSeconds $PeerTimeoutSeconds `
    -ExecutionCohortNonce $ExecutionCohortNonce `
    -ExecutionCohortCreatedUtc $ExecutionCohortCreatedUtc `
    -ExpectedRuntimeClosure $expectedRuntimeClosure `
    -ValidationDataBinding $localData `
    -AllowHeadlessDirectExecution ([bool]$AllowHeadlessDirectExecution) `
    -RunnerScriptPath $PSCommandPath

$diagnosticReceiptPath = Join-Path $OutputDirectory 'Stage5LocalLockstepDiagnostic.json'
$diagnostic = Write-Stage5LocalLockstepDiagnosticReceipt `
    -Path $diagnosticReceiptPath `
    -SessionResult $sessionResult `
    -LocalDataBinding $localData `
    -HostTopology $hostTopology `
    -ExpectedSourceCommit $SourceCommit `
    -ExpectedRuntimeClosure $expectedRuntimeClosure `
    -ExpectedCohortNonce $ExecutionCohortNonce `
    -ExpectedCohortCreatedUtc $ExecutionCohortCreatedUtc `
    -ExpectedMapName $MapName `
    -ExpectedMapCrcs $expectedMapCrcs `
    -ExpectedEffectiveWorkerTotal $predictedEffectiveWorkerTotal `
    -MaximumTotalEffectiveWorkers $MaximumTotalEffectiveWorkers `
    -PeerCount $PeerCount -Seed $Seed -PeerTimeoutSeconds $PeerTimeoutSeconds `
    -AllowHeadlessDirectExecution ([bool]$AllowHeadlessDirectExecution)
Assert-LocalDiagnosticCondition (-not [bool]$diagnostic.finalAcceptanceClaim -and
    [bool]$diagnostic.externalQualificationSkipped -and
    [bool]$diagnostic.manualTestingDeferred) `
    'Local diagnostic receipt did not retain the explicit non-final/deferred disposition.'
Write-Output ("STAGE5_LOCAL_LOCKSTEP_DIAGNOSTIC_PASS receipt={0} sha256={1} " +
    "finalAcceptanceClaim=false externalQualificationSkipped=true manualTestingDeferred=true" -f
    $diagnostic.path, $diagnostic.sha256)
