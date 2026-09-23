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
    'replay-fixture-manifest' = 'host-runner'
    'multiplayer-results' = 'host-runner'
}
$expectedReplayBindings = @(
    'replay-fixture-manifest|Generals',
    'replay-fixture-manifest|ZeroHour'
)
$seenReplayBindings = New-Object 'Collections.Generic.List[string]'
$bindingResults = & $evidenceModule {
    param($trustDomains, $expectedBindings, $seenBindings)
    $first = Assert-Stage5FinalAcceptanceAttachmentBinding `
        -Role 'replay-fixture-manifest' -Title 'Generals' `
        -TrustDomain 'host-runner' -AttachmentTrustDomains $trustDomains `
        -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
        -Context 'bounded replay-fixture test'
    $seenBindings.Add($first) | Out-Null
    $second = Assert-Stage5FinalAcceptanceAttachmentBinding `
        -Role 'replay-fixture-manifest' -Title 'ZeroHour' `
        -TrustDomain 'host-runner' -AttachmentTrustDomains $trustDomains `
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
            -TrustDomain 'host-runner' -AttachmentTrustDomains $trustDomains `
            -ExpectedBindings $expectedBindings -SeenBindings $seenBindings `
            -Context 'bounded replay-fixture test'
    } $attachmentTrustDomains $expectedReplayBindings $seenReplayBindings | Out-Null
} 'repeats or does not authorize attachment' `
    'the production attachment guard rejects a duplicate role/title key'
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

$snapshotPath = Join-Path $runRoot 'snapshot-hash-guard.txt'
Write-RelocationText $snapshotPath 'snapshot guard bytes'
$snapshot = Get-Stage5FinalAcceptanceFileSnapshot `
    -Path $snapshotPath -Context 'bounded snapshot-hash test'
Assert-RelocationThrows {
    Assert-Stage5FinalAcceptanceSnapshotSha256 `
        $snapshot ('0' * 64) 'bounded snapshot-hash test' | Out-Null
} 'SHA-256 mismatch' `
    'the production snapshot guard rejects a substituted attachment digest'

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
            'production immutable snapshot digest rejection',
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
