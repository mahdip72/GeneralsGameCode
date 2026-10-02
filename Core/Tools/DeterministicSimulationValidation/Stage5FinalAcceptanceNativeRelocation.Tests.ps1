[CmdletBinding()]
param(
    [string]$ScratchRoot = '',
    [string]$ModulePath = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-RelocationTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-RelocationThrows {
    param(
        [scriptblock]$Action,
        [string]$Pattern,
        [string]$Message
    )
    $caught = $null
    try {
        & $Action
    }
    catch {
        $caught = $_.Exception
    }
    $errorText = if ($null -ne $caught) { $caught.Message } else { 'no exception' }
    Assert-RelocationTest ($null -ne $caught -and $errorText -match $Pattern) "$Message (got '$errorText')"
}

function Write-RelocationText {
    param([string]$Path, [string]$Text)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Get-RelocationSha256 {
    param([string]$Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Set-Stage5ReplayEvidenceHashBinding {
    param(
        [Parameter(Mandatory = $true)][string]$ReplayEvidencePath,
        [Parameter(Mandatory = $true)][object]$RuntimeEvidenceDocument
    )
    $RuntimeEvidenceDocument.details.replayEvidenceSha256 =
        (Get-FileHash -LiteralPath $ReplayEvidencePath -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Write-RelocationJson {
    param([string]$Path, [object]$Value)
    Write-RelocationText $Path ($Value | ConvertTo-Json -Depth 20)
}

function New-RelocationFixture {
    param(
        [ValidateSet('absolute', 'relative', 'ambiguous', 'no-match', 'alias')]
        [string]$Mode,
        [string]$Root
    )

    $nativeRoot = Join-Path $Root 'native'
    $stagedRoot = Join-Path $Root 'staged'
    [IO.Directory]::CreateDirectory($nativeRoot) | Out-Null
    [IO.Directory]::CreateDirectory($stagedRoot) | Out-Null
    $specs = @(
        [pscustomobject]@{ sequence = 1; runNonce = '00000000-0000-4000-8000-000000000001'; sourceStem = 'runA'; candidateStem = 'runA' },
        [pscustomobject]@{ sequence = 2; runNonce = '00000000-0000-4000-8000-000000000002'; sourceStem = 'case\logs'; candidateStem = 'Case\Logs' }
    )
    $cohortNonce = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
    $children = New-Object 'Collections.Generic.List[object]'
    foreach ($spec in $specs) {
        $nativeRelative = 'native\{0:D4}.json' -f $spec.sequence
        $nativePath = Join-Path $Root $nativeRelative
        $rawDefinitions = New-Object 'Collections.Generic.List[object]'
        foreach ($kind in @(
            [pscustomobject]@{ name = 'raw-log'; leaf = 'raw.log'; text = 'raw' },
            [pscustomobject]@{ name = 'timing'; leaf = 'timing.log'; text = 'timing' }
        )) {
            $sourceStem = $spec.sourceStem
            $candidateStem = $spec.candidateStem
            if ($Mode -ceq 'alias' -and $spec.sequence -eq 2 -and $kind.name -ceq 'raw-log') {
                $sourceStem = 'runA'
                $candidateStem = 'runA'
            }
            if ($Mode -ceq 'no-match' -and $spec.sequence -eq 1 -and $kind.name -ceq 'raw-log') {
                $sourceStem = 'missing\never'
            }
            $candidateRelative = if ($Mode -ceq 'relative') {
                'native\{0}\{1}' -f $candidateStem, $kind.leaf
            }
            else {
                'staged\{0}\{1}' -f $candidateStem, $kind.leaf
            }
            $candidatePath = Join-Path $Root $candidateRelative
            if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
                Write-RelocationText $candidatePath "$($kind.text)-$($spec.sequence)"
            }
            $hash = Get-RelocationSha256 $candidatePath
            $sourcePath = if ($Mode -ceq 'relative') {
                '{0}\{1}' -f $candidateStem, $kind.leaf
            }
            else {
                'H:\historical\{0}\{1}' -f $sourceStem, $kind.leaf
            }
            $rawDefinitions.Add([ordered]@{ name = $kind.name; path = $sourcePath; sha256 = $hash }) | Out-Null
        }
        $native = [ordered]@{
            rawLogs = @($rawDefinitions.ToArray())
            provenance = [ordered]@{ receiptPath = $nativeRelative }
        }
        Write-RelocationJson $nativePath $native
        $nativeHash = Get-RelocationSha256 $nativePath
        $children.Add([ordered]@{
            sequence = $spec.sequence
            runNonce = $spec.runNonce
            nativeReceipt = [ordered]@{
                path = $nativeRelative
                sha256 = $nativeHash
                producer = 'game-executable-stage5-performance-report-v5'
                runNonce = $spec.runNonce
                cohortNonce = $cohortNonce
            }
        }) | Out-Null
    }
    if ($Mode -ceq 'absolute' -or $Mode -ceq 'ambiguous' -or $Mode -ceq 'no-match' -or $Mode -ceq 'alias') {
        foreach ($decoy in @('staged\logs\raw.log', 'staged\logs\timing.log', 'staged\other\raw.log', 'staged\other\timing.log')) {
            $decoyPath = Join-Path $Root $decoy
            if (-not (Test-Path -LiteralPath $decoyPath -PathType Leaf)) {
                Write-RelocationText $decoyPath 'decoy'
            }
        }
    }
    if ($Mode -ceq 'ambiguous') {
        Write-RelocationText (Join-Path $stagedRoot 'duplicate\runA\raw.log') 'ambiguous'
    }
    $hostReceiptPath = Join-Path $Root 'validation-results-receipt.json'
    Write-RelocationJson $hostReceiptPath ([ordered]@{
        trustDomain = 'host-runner'
        provenance = [ordered]@{ children = @($children.ToArray()) }
    })
    return [pscustomobject]@{ root = $Root; receiptPath = $hostReceiptPath; nativeRoot = $nativeRoot }
}

function Invoke-RelocationBinding {
    param([object]$Fixture)
    return Get-Stage5FinalAcceptanceNativeRelocationBinding -Path $Fixture.receiptPath -EvidenceDirectory $Fixture.root
}

function Get-ChildBinding {
    param([object]$Result, [int]$Sequence)
    $matches = @($Result.children | Where-Object { [int]$_.sequence -eq $Sequence })
    Assert-RelocationTest ($matches.Count -eq 1) "expected one relocation child for sequence $Sequence"
    return $matches[0]
}

function Get-NamedRawBinding {
    param([object]$Child, [string]$Name)
    $matches = @($Child.nativeRawBindings | Where-Object { [string]$_.name -ceq $Name })
    Assert-RelocationTest ($matches.Count -eq 1) "expected one '$Name' binding for sequence $($Child.sequence)"
    return $matches[0]
}

function Enable-CandidateRelativeCounter {
    param([object]$EvidenceModule, [Collections.IDictionary]$Counter)
    $original = & $EvidenceModule { (Get-Command Get-Stage5FinalAcceptanceRelativePath -CommandType Function).ScriptBlock }
    & $EvidenceModule {
        param($originalFunction, $counter)
        Set-Item Function:\script:Stage5OriginalRelativePath -Value $originalFunction
        $script:Stage5CandidateRelativeCounter = $counter
        $wrapper = {
            param([string]$BaseDirectory, [string]$Path, [string]$Context)
            if ($Context -match ' candidate$') {
                $script:Stage5CandidateRelativeCounter['count'] = [int]$script:Stage5CandidateRelativeCounter['count'] + 1
            }
            & Stage5OriginalRelativePath $BaseDirectory $Path $Context
        }
        Set-Item Function:\script:Get-Stage5FinalAcceptanceRelativePath -Value $wrapper
    } $original $Counter
    return $original
}

function Disable-CandidateRelativeCounter {
    param([object]$EvidenceModule, [object]$Original)
    & $EvidenceModule {
        param($originalFunction)
        Set-Item Function:\script:Get-Stage5FinalAcceptanceRelativePath -Value $originalFunction
        Remove-Item Function:\script:Stage5OriginalRelativePath -ErrorAction SilentlyContinue
        Remove-Variable Stage5CandidateRelativeCounter -Scope Script -ErrorAction SilentlyContinue
    } $Original
}

$configuredScratchRoot = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else {
    $ScratchRoot
}
Assert-RelocationTest (-not [string]::IsNullOrWhiteSpace($configuredScratchRoot)) 'relocation tests require -ScratchRoot or RTS_STAGE5_VALIDATION_SCRATCH_ROOT'
$resolvedScratchRoot = [IO.Path]::GetFullPath($configuredScratchRoot).TrimEnd('\')
Assert-RelocationTest $resolvedScratchRoot.StartsWith('H:\', [StringComparison]::OrdinalIgnoreCase) 'relocation tests require an explicit H: scratch root'
[IO.Directory]::CreateDirectory($resolvedScratchRoot) | Out-Null
$runRoot = Join-Path $resolvedScratchRoot ('actual-binding-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$logPath = Join-Path $runRoot 'actual-binding-regression.log'
$summaryPath = Join-Path $runRoot 'actual-binding-regression.json'
$modulePath = if ([string]::IsNullOrWhiteSpace($ModulePath)) {
    Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
}
else {
    [IO.Path]::GetFullPath($ModulePath)
}
Assert-RelocationTest (Test-Path -LiteralPath $modulePath -PathType Leaf) "the source evidence module is missing: $modulePath"
Import-Module $modulePath -Force
$evidenceModule = Get-Module -Name 'DeterministicSimulationEvidence' | Where-Object { $_.Path -ceq ([IO.Path]::GetFullPath($modulePath)) } | Select-Object -First 1
Assert-RelocationTest ($null -ne $evidenceModule) 'the source evidence module did not import'

function Invoke-Stage5FinalAcceptanceBoundedGuardTests {
    param([string]$RunRoot, [object]$EvidenceModule)

    # These production guards are also exercised through the full Acceptance
    # reader. Keep focused mutations here so each negative does not repeat a
    # 253-child evidence-root traversal.
$attachmentTrustDomains = @{
    'replay-fixture-manifest' = 'reviewed-fixture'
    'multiplayer-results' = 'host-runner'
}
$expectedReplayBindings = @(
    'replay-fixture-manifest|Generals',
    'replay-fixture-manifest|ZeroHour'
)
$seenReplayBindings = New-Object 'Collections.Generic.List[string]'

# Keep replay-envelope rebinding probes here with the other bounded production
# guard cases. Re-running final acceptance for each mutation would traverse the
# full 253-child corpus repeatedly without exercising a different reader guard.
function Assert-CurrentReplayRuntimeBinding {
    param([string]$ReplayEvidencePath, [object]$RuntimeEvidenceDocument,
        [object]$EvidenceModule)
    $evidenceHashes = @{
        'replay-determinism' = Get-RelocationSha256 $ReplayEvidencePath
        'fresh-ai' = 'B' * 64
        'performance-scaling' = 'C' * 64
    }
    & $EvidenceModule {
        param($details, $hashes)
        Assert-Stage5FinalAcceptanceDetails 'deterministic-runtime' `
            $details ('a' * 40) $hashes | Out-Null
    } $RuntimeEvidenceDocument.details $evidenceHashes
}
function Assert-StaleReplayRuntimeBindingRejected {
    param([string]$ReplayEvidencePath, [object]$RuntimeEvidenceDocument,
        [object]$EvidenceModule, [string]$CaseName)
    $evidenceHashes = @{
        'replay-determinism' = Get-RelocationSha256 $ReplayEvidencePath
        'fresh-ai' = 'B' * 64
        'performance-scaling' = 'C' * 64
    }
    Assert-RelocationThrows {
        & $EvidenceModule {
            param($details, $hashes)
            Assert-Stage5FinalAcceptanceDetails 'deterministic-runtime' `
                $details ('a' * 40) $hashes | Out-Null
        } $RuntimeEvidenceDocument.details $evidenceHashes
    } 'replayEvidenceSha256 does not bind the independently hashed replay-determinism evidence' `
        "$CaseName must reject a stale cross-evidence hash before its title guard."
}

$replayHashFixtureRoot = Join-Path $RunRoot 'replay-hash-binding'
$generalsReplayManifestPath = Join-Path $replayHashFixtureRoot 'Generals.json'
$zeroHourReplayManifestPath = Join-Path $replayHashFixtureRoot 'ZeroHour.json'
Write-RelocationJson $generalsReplayManifestPath ([ordered]@{ title = 'Generals' })
Write-RelocationJson $zeroHourReplayManifestPath ([ordered]@{ title = 'ZeroHour' })
$replayHashFixturePath = Join-Path $replayHashFixtureRoot 'replay-determinism.json'
$replayHashFixtureDocument = [ordered]@{
    attachments = @(
        [ordered]@{
            role = 'replay-fixture-manifest'; title = 'Generals'
            path = $generalsReplayManifestPath
            sha256 = Get-RelocationSha256 $generalsReplayManifestPath
            trustDomain = 'reviewed-fixture'
        }
        [ordered]@{
            role = 'replay-fixture-manifest'; title = 'ZeroHour'
            path = $zeroHourReplayManifestPath
            sha256 = Get-RelocationSha256 $zeroHourReplayManifestPath
            trustDomain = 'reviewed-fixture'
        }
    )
}
Write-RelocationJson $replayHashFixturePath $replayHashFixtureDocument
$originalReplayHashFixtureBytes = [IO.File]::ReadAllBytes($replayHashFixturePath)
$runtimeEvidenceDocument = [ordered]@{
    details = [ordered]@{
        gateName = 'deterministic-runtime'; isolatedPipelineMode = 'serial'
        simulationModes = @('serial', 'parallel', 'shadow')
        workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
            'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
        isolatedMatrixPassed = $true; finalAcceptanceClaim = $false
        replayEvidenceSha256 = Get-RelocationSha256 $replayHashFixturePath
        freshAiEvidenceSha256 = 'B' * 64
        performanceEvidenceSha256 = 'C' * 64
        installedKernelExecution = [ordered]@{
            status = 'skipped'; claim = $false
            reason = 'external-qualification-exempt-and-reviewed-native-fixture-unavailable'
            sha256 = $null
        }
    }
}

$duplicateReplayDocument = Get-Content -LiteralPath $replayHashFixturePath -Raw |
    ConvertFrom-Json
$duplicateReplayDocument.attachments[1].title = 'Generals'
Write-RelocationJson $replayHashFixturePath $duplicateReplayDocument
Assert-RelocationTest (@($duplicateReplayDocument.attachments | Where-Object {
    $_.sha256 -cne (Get-RelocationSha256 $_.path)
}).Count -eq 0) `
    'duplicate-title probe must keep both replay fixture files byte-valid'
Assert-StaleReplayRuntimeBindingRejected $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule 'duplicate-title probe'
$duplicateTitleReplayPath = $replayHashFixturePath
Set-Stage5ReplayEvidenceHashBinding $duplicateTitleReplayPath $runtimeEvidenceDocument
Assert-CurrentReplayRuntimeBinding $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule
$duplicateSeenBindings = New-Object 'Collections.Generic.List[string]'
Assert-RelocationThrows {
    & $EvidenceModule {
        param($attachments, $trustDomains, $expectedBindings, $seenBindings)
        foreach ($attachment in @($attachments)) {
            $binding = Assert-Stage5FinalAcceptanceAttachmentBinding `
                -Role $attachment.role -Title $attachment.title `
                -TrustDomain $attachment.trustDomain `
                -AttachmentTrustDomains $trustDomains `
                -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
                -Context 'bounded duplicate-title replay binding test'
            $seenBindings.Add($binding) | Out-Null
        }
    } $duplicateReplayDocument.attachments $attachmentTrustDomains `
        $expectedReplayBindings $duplicateSeenBindings
} 'repeats or does not authorize attachment' `
    'duplicate-title probe must reach the production duplicate-binding guard after rebinding'

[IO.File]::WriteAllBytes($replayHashFixturePath,
    [byte[]]$originalReplayHashFixtureBytes)
Assert-RelocationTest ([Convert]::ToBase64String(
    [IO.File]::ReadAllBytes($replayHashFixturePath)) -ceq
        [Convert]::ToBase64String($originalReplayHashFixtureBytes)) `
    'restored-source probe must restore the original replay evidence bytes exactly'
$restoredSourceReplayPath = $replayHashFixturePath
Assert-StaleReplayRuntimeBindingRejected $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule 'restored-source probe'
Set-Stage5ReplayEvidenceHashBinding $restoredSourceReplayPath $runtimeEvidenceDocument
Assert-RelocationTest ($runtimeEvidenceDocument.details.replayEvidenceSha256 -ceq
    (Get-RelocationSha256 $replayHashFixturePath)) `
    'restored-source probe must rebind to the byte-identical original replay evidence'
Assert-CurrentReplayRuntimeBinding $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule

$swappedReplayDocument = Get-Content -LiteralPath $replayHashFixturePath -Raw |
    ConvertFrom-Json
$swappedReplayDocument.attachments[1].path =
    $swappedReplayDocument.attachments[0].path
$swappedReplayDocument.attachments[1].sha256 =
    $swappedReplayDocument.attachments[0].sha256
Write-RelocationJson $replayHashFixturePath $swappedReplayDocument
Assert-StaleReplayRuntimeBindingRejected $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule 'swapped-title probe'
$swappedTitleReplayPath = $replayHashFixturePath
Set-Stage5ReplayEvidenceHashBinding $swappedTitleReplayPath $runtimeEvidenceDocument
Assert-CurrentReplayRuntimeBinding $replayHashFixturePath `
    $runtimeEvidenceDocument $EvidenceModule
$swappedReceipt = Get-Content -LiteralPath `
    $swappedReplayDocument.attachments[1].path -Raw | ConvertFrom-Json
Assert-RelocationTest ($swappedReplayDocument.attachments[1].sha256 -ceq
    (Get-RelocationSha256 $swappedReplayDocument.attachments[1].path)) `
    'swapped-title probe must keep the substituted receipt byte-valid'
Assert-RelocationThrows {
    & $EvidenceModule {
        param($expectedTitle, $receiptTitle)
        Assert-Stage5FinalAcceptanceReceiptTitleScope `
            $expectedTitle $receiptTitle 'bounded swapped replay manifest test'
    } $swappedReplayDocument.attachments[1].title $swappedReceipt.title
} 'title scope is substituted' `
    'swapped-title probe must reach the production receipt-title guard after rebinding'

$bindingResults = & $evidenceModule {
    param($trustDomains, $expectedBindings, $seenBindings)
    $first = Assert-Stage5FinalAcceptanceAttachmentBinding `
        -Role 'replay-fixture-manifest' -Title 'Generals' `
        -TrustDomain 'reviewed-fixture' -AttachmentTrustDomains $trustDomains `
        -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
        -Context 'bounded replay-fixture test'
    $seenBindings.Add($first) | Out-Null
    $second = Assert-Stage5FinalAcceptanceAttachmentBinding `
        -Role 'replay-fixture-manifest' -Title 'ZeroHour' `
        -TrustDomain 'reviewed-fixture' -AttachmentTrustDomains $trustDomains `
        -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
        -Context 'bounded replay-fixture test'
    $seenBindings.Add($second) | Out-Null
    @($first, $second)
} $attachmentTrustDomains $expectedReplayBindings $seenReplayBindings
Assert-RelocationTest (@($bindingResults).Count -eq 2 -and
    @($seenReplayBindings.ToArray()).Count -eq 2) `
    'the production attachment guard accepts distinct title-scoped bindings'
Assert-RelocationThrows {
    & $evidenceModule {
        param($trustDomains, $expectedBindings, $seenBindings)
        Assert-Stage5FinalAcceptanceAttachmentBinding `
            -Role 'replay-fixture-manifest' -Title 'Generals' `
            -TrustDomain 'reviewed-fixture' -AttachmentTrustDomains $trustDomains `
            -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
            -Context 'bounded replay-fixture test'
    } $attachmentTrustDomains $expectedReplayBindings $seenReplayBindings | Out-Null
} 'repeats or does not authorize attachment' `
    'the production attachment guard rejects a duplicate role/title key'
Assert-RelocationThrows {
    & $evidenceModule {
        param($trustDomains, $expectedBindings)
        Assert-Stage5FinalAcceptanceAttachmentBinding `
            -Role 'replay-fixture-manifest' -Title 'Generals' `
            -TrustDomain 'host-runner' -AttachmentTrustDomains $trustDomains `
            -ExpectedBindings $expectedBindings `
            -SeenBindings (New-Object 'Collections.Generic.List[string]') `
            -Context 'bounded reviewed-fixture trust-domain test'
    } $attachmentTrustDomains $expectedReplayBindings | Out-Null
} 'wrong trust domain' `
    'the production attachment guard rejects a replay fixture mislabeled as host-runner evidence'
Assert-RelocationThrows {
    & $evidenceModule {
        param($trustDomains)
        Assert-Stage5FinalAcceptanceAttachmentBinding `
            -Role 'multiplayer-results' -Title 'Both' -TrustDomain 'executable' `
            -AttachmentTrustDomains $trustDomains `
            -ExpectedBindings @('multiplayer-results|Both') `
            -SeenBindings (New-Object 'Collections.Generic.List[string]') `
            -Context 'bounded trust-domain test'
    } $attachmentTrustDomains | Out-Null
} 'wrong trust domain' `
    'the production attachment guard rejects an unauthorized trust domain'

& $evidenceModule {
    Assert-Stage5FinalAcceptanceReceiptTitleScope `
        'Generals' 'Generals' 'bounded receipt-title test'
} | Out-Null
Assert-RelocationThrows {
    & $evidenceModule {
        Assert-Stage5FinalAcceptanceReceiptTitleScope `
            'Generals' 'ZeroHour' 'bounded receipt-title test'
    } | Out-Null
} 'title scope is substituted' `
    'the production immutable-receipt guard rejects a cross-title receipt'
& $evidenceModule {
    Assert-Stage5FinalAcceptanceEvidenceTitleScope `
        'Both' 'Both' 'bounded evidence-envelope title test'
} | Out-Null
Assert-RelocationThrows {
    & $evidenceModule {
        Assert-Stage5FinalAcceptanceEvidenceTitleScope `
            'Generals' 'Both' 'bounded evidence-envelope title test'
    } | Out-Null
} 'must have exact title scope' `
    'the production evidence-envelope guard rejects an incorrectly scoped title'

& $evidenceModule {
    Assert-Stage5FinalAcceptanceEvidenceIdentity `
        1 'deterministic-runtime' 'passed' ('a' * 40) 'x64' ('B' * 64) `
        'deterministic-runtime' ('a' * 40) ('B' * 64)
} | Out-Null
Assert-RelocationThrows {
    & $evidenceModule {
        Assert-Stage5FinalAcceptanceEvidenceIdentity `
            1 'deterministic-runtime' 'passed' ('a' * 40) 'x64' ('C' * 64) `
            'deterministic-runtime' ('a' * 40) ('B' * 64)
    } | Out-Null
} 'does not identify the same passed x64 commit and artifact set' `
    'the production evidence identity guard rejects a stale artifact-set binding'

$lockstepEvidencePath = Join-Path $runRoot 'lockstep-evidence-hash-guard.json'
Write-RelocationText $lockstepEvidencePath '{}'
$wrongExpectedLockstepHashArgs = @{
    Path = $lockstepEvidencePath
    ExpectedSourceCommit = 'a' * 40
    ExpectedArtifactSetSha256 = 'B' * 64
    ArtifactHashes = @{
        'generals-executable' = 'C' * 64
        'zerohour-executable' = 'D' * 64
    }
    ExpectedEvidenceSha256 = '0' * 64
}
Assert-RelocationThrows {
    Read-Stage5LockstepV2Evidence @wrongExpectedLockstepHashArgs | Out-Null
} 'Lockstep-v2 multiplayer evidence SHA-256 mismatch' `
    'the lockstep-v2 reader rejects JSON evidence detached from its independent ExpectedEvidenceSha256'

$launcherRoot = Join-Path $runRoot 'launcher-canonical'
$copiedLauncherRoot = Join-Path $runRoot 'launcher-copied'
[IO.Directory]::CreateDirectory($launcherRoot) | Out-Null
[IO.Directory]::CreateDirectory($copiedLauncherRoot) | Out-Null
$canonicalExecutable = Join-Path $launcherRoot 'generals.exe'
$canonicalLauncher = Join-Path $launcherRoot 'launcher.exe'
$canonicalConfig = Join-Path $launcherRoot 'launcher.lcf'
Write-RelocationText $canonicalExecutable 'canonical executable'
Write-RelocationText $canonicalLauncher 'canonical launcher'
Write-RelocationText $canonicalConfig 'canonical configuration'
$launcherHashes = @{
    'generals-executable' = Get-RelocationSha256 $canonicalExecutable
    'generals-launcher' = Get-RelocationSha256 $canonicalLauncher
    'generals-launcher-config' = Get-RelocationSha256 $canonicalConfig
}
$launcherPaths = @{
    'generals-executable' = [IO.Path]::GetFullPath($canonicalExecutable)
    'generals-launcher' = [IO.Path]::GetFullPath($canonicalLauncher)
    'generals-launcher-config' = [IO.Path]::GetFullPath($canonicalConfig)
}
$launcherArguments = @('-simulationMode', 'parallel', '-workerPolicy', 'auto')
$launcherContract = [ordered]@{
    schemaVersion = 1; mode = 'headless-direct-exception'
    configPath = [IO.Path]::GetFullPath($canonicalConfig)
    configSha256 = $launcherHashes['generals-launcher-config']
    launcherPath = [IO.Path]::GetFullPath($canonicalLauncher)
    launcherSha256 = $launcherHashes['generals-launcher']
    directory = '.'; executable = 'generals.exe'
    launcherTarget = [IO.Path]::GetFullPath($canonicalExecutable)
    launcherArguments = $launcherArguments
    launcherWorkingDirectory = [IO.Path]::GetFullPath($launcherRoot)
    directExecutable = [IO.Path]::GetFullPath($canonicalExecutable)
    directWorkingDirectory = [IO.Path]::GetFullPath($launcherRoot)
    directArguments = $launcherArguments; childExitCodeObserved = $true
}
& $evidenceModule {
    param($contract, $hashes, $paths, $root)
    Assert-Stage5LockstepLauncherContract $contract 'Generals' $hashes `
        'bounded launcher path-binding test' $paths $root
} $launcherContract $launcherHashes $launcherPaths $launcherRoot | Out-Null
foreach ($leaf in @('generals.exe', 'launcher.exe', 'launcher.lcf')) {
    Copy-Item -LiteralPath (Join-Path $launcherRoot $leaf) `
        -Destination (Join-Path $copiedLauncherRoot $leaf)
}
$copiedLauncherContract = [ordered]@{}
foreach ($field in $launcherContract.Keys) {
    $copiedLauncherContract[$field] = $launcherContract[$field]
}
$copiedLauncherContract.configPath = [IO.Path]::GetFullPath(
    (Join-Path $copiedLauncherRoot 'launcher.lcf'))
$copiedLauncherContract.launcherPath = [IO.Path]::GetFullPath(
    (Join-Path $copiedLauncherRoot 'launcher.exe'))
$copiedLauncherContract.launcherTarget = [IO.Path]::GetFullPath(
    (Join-Path $copiedLauncherRoot 'generals.exe'))
$copiedLauncherContract.launcherWorkingDirectory = [IO.Path]::GetFullPath(
    $copiedLauncherRoot)
$copiedLauncherContract.directExecutable = [IO.Path]::GetFullPath(
    (Join-Path $copiedLauncherRoot 'generals.exe'))
$copiedLauncherContract.directWorkingDirectory = [IO.Path]::GetFullPath(
    $copiedLauncherRoot)
Assert-RelocationThrows {
    & $evidenceModule {
        param($contract, $hashes, $paths, $root)
        Assert-Stage5LockstepLauncherContract $contract 'Generals' $hashes `
            'bounded launcher path-binding test' $paths $root
    } $copiedLauncherContract $launcherHashes $launcherPaths $launcherRoot | Out-Null
} 'launch paths are not bound to the canonical artifact-set files' `
    'the production launcher guard rejects a byte-identical copied runtime'
}

function Invoke-CandidateIndexLookup {
    param(
        [object]$Module,
        [object[]]$Records,
        [string[]]$SourceSegments
    )
    return & $Module {
        param($candidateRecords, $sourcePathSegments)
        $index = New-Stage5FinalAcceptanceCandidateSuffixIndex $candidateRecords
        @(Find-Stage5FinalAcceptanceCandidateMatches `
            $index $sourcePathSegments)
    } $Records $SourceSegments
}

$candidateRecords = @(
    [pscustomobject]@{
        path = 'staged\short\runA\raw.log'
        segments = @('staged', 'short', 'runA', 'raw.log')
    },
    [pscustomobject]@{
        path = 'staged\runA\raw.log'
        segments = @('staged', 'runA', 'raw.log')
    },
    [pscustomobject]@{
        path = 'staged\Case\Logs\raw.log'
        segments = @('staged', 'Case', 'Logs', 'raw.log')
    },
    [pscustomobject]@{
        path = 'staged\decoy\raw.log'
        segments = @('staged', 'decoy', 'raw.log')
    }
)
$longestMatches = @(Invoke-CandidateIndexLookup $evidenceModule `
    $candidateRecords @('historical', 'short', 'runA', 'raw.log'))
Assert-RelocationTest ($longestMatches.Count -eq 1 -and
    [int]$longestMatches[0].suffixLength -eq 3 -and
    [string]$longestMatches[0].path -ceq 'staged\short\runA\raw.log') `
    'candidate suffix index did not preserve the longest unique suffix winner'
$caseMatches = @(Invoke-CandidateIndexLookup $evidenceModule `
    $candidateRecords @('historical', 'case', 'logs', 'raw.log'))
Assert-RelocationTest ($caseMatches.Count -eq 1 -and
    [string]$caseMatches[0].path -ceq 'staged\Case\Logs\raw.log') `
    'candidate suffix index did not preserve case-insensitive matching'
$ambiguousRecords = @(
    [pscustomobject]@{
        path = 'staged\one\shared\raw.log'
        segments = @('staged', 'one', 'shared', 'raw.log')
    },
    [pscustomobject]@{
        path = 'staged\two\shared\raw.log'
        segments = @('staged', 'two', 'shared', 'raw.log')
    }
)
$ambiguousMatches = @(Invoke-CandidateIndexLookup $evidenceModule `
    $ambiguousRecords @('historical', 'shared', 'raw.log'))
Assert-RelocationTest ($ambiguousMatches.Count -eq 2 -and
    @($ambiguousMatches | Where-Object { $_.suffixLength -eq 2 }).Count -eq 2) `
    'candidate suffix index did not preserve a longest-suffix ambiguity'
$noMatch = @(Invoke-CandidateIndexLookup $evidenceModule `
    $candidateRecords @('historical', 'missing', 'raw.log'))
Assert-RelocationTest ($noMatch.Count -eq 0) `
    'candidate suffix index returned a candidate for an unmatched suffix'

$summary = $null
try {
    Invoke-Stage5FinalAcceptanceBoundedGuardTests $runRoot $evidenceModule

    $absoluteFixture = New-RelocationFixture -Mode absolute -Root (Join-Path $runRoot 'absolute')
    $absoluteFileCount = @(Get-ChildItem -LiteralPath $absoluteFixture.root -Recurse -File -Force).Count
    $counter = @{ count = 0 }
    $originalRelative = Enable-CandidateRelativeCounter $evidenceModule $counter
    try {
        $absoluteResult = Invoke-RelocationBinding $absoluteFixture
    }
    finally {
        Disable-CandidateRelativeCounter $evidenceModule $originalRelative
    }
    Assert-RelocationTest (@($absoluteResult.children).Count -eq 2) 'absolute binding did not return both native children'
    $child1 = Get-ChildBinding $absoluteResult 1
    $child2 = Get-ChildBinding $absoluteResult 2
    $child1Raw = Get-NamedRawBinding $child1 'raw-log'
    $child2Raw = Get-NamedRawBinding $child2 'raw-log'
    Assert-RelocationTest ([string]$child1Raw.path -ceq 'staged\runA\raw.log') "unique absolute winner changed: '$($child1Raw.path)'"
    Assert-RelocationTest ([string]$child2Raw.path -ceq 'staged\Case\Logs\raw.log') "case-insensitive absolute winner changed: '$($child2Raw.path)'"
    Assert-RelocationTest ([int]$counter.count -eq $absoluteFileCount) "candidate relative path was evaluated $($counter.count) times; expected one per bounded file ($absoluteFileCount)"

    $mutatedPath = Join-Path $absoluteFixture.root 'staged\runA\raw.log'
    Write-RelocationText $mutatedPath 'changed-after-first-binding'
    Assert-RelocationThrows { Invoke-RelocationBinding $absoluteFixture | Out-Null } 'SHA-256|sha256' 'a changed raw file must be re-read and fail its bound hash'

    $relativeFixture = New-RelocationFixture -Mode relative -Root (Join-Path $runRoot 'relative')
    $relativeResult = Invoke-RelocationBinding $relativeFixture
    $relativeChild1 = Get-ChildBinding $relativeResult 1
    $relativeRaw1 = Get-NamedRawBinding $relativeChild1 'raw-log'
    Assert-RelocationTest ([string]$relativeRaw1.path -ceq 'native\runA\raw.log') "relative branch changed: '$($relativeRaw1.path)'"

    $ambiguousFixture = New-RelocationFixture -Mode ambiguous -Root (Join-Path $runRoot 'ambiguous')
    Assert-RelocationThrows { Invoke-RelocationBinding $ambiguousFixture | Out-Null } 'multiple staged candidates.*ambiguous' 'longest-suffix ties must remain ambiguous'

    $noMatchFixture = New-RelocationFixture -Mode no-match -Root (Join-Path $runRoot 'no-match')
    Assert-RelocationThrows { Invoke-RelocationBinding $noMatchFixture | Out-Null } 'no staged candidate matching' 'absolute paths without a qualifying suffix must be rejected'

    $aliasFixture = New-RelocationFixture -Mode alias -Root (Join-Path $runRoot 'alias')
    Assert-RelocationThrows { Invoke-RelocationBinding $aliasFixture | Out-Null } 'aliases another staged native raw log' 'two raw names resolving to one staged file must remain an alias error'

    $summary = [ordered]@{
        status = 'passed'
        sourceModule = [IO.Path]::GetFullPath($modulePath)
        runRoot = $runRoot
        absoluteFileCount = $absoluteFileCount
        candidateRelativeCallCount = [int]$counter.count
        explicitWinners = [ordered]@{
            unique = [string]$child1Raw.path
            caseInsensitive = [string]$child2Raw.path
        }
        relativeWinner = [string]$relativeRaw1.path
        checks = @(
            'production acceptance role/title, trust, and duplicate-binding guards',
            'production evidence identity and outer/immutable receipt title guards',
            'production lockstep reader ExpectedEvidenceSha256 rejection',
            'production launcher canonical-path rejection for a copied runtime',
            'actual exported binding function invoked with real receipt/native/raw files',
            'all selected raw hashes validated, then mutation was re-read and rejected',
            'unique longest suffix winner',
            'case-insensitive segment winner',
            'relative branch',
            'ambiguous tie rejection',
            'no-match rejection',
            'alias rejection'
        )
    }
    Write-RelocationJson $summaryPath $summary
    Write-RelocationText $logPath ($summary | ConvertTo-Json -Depth 12)
    Write-Output ($summary | ConvertTo-Json -Depth 12)
}
catch {
    $failure = [ordered]@{ status = 'failed'; runRoot = $runRoot; error = $_.Exception.Message }
    Write-RelocationText $logPath ($failure | ConvertTo-Json -Depth 12)
    throw
}
