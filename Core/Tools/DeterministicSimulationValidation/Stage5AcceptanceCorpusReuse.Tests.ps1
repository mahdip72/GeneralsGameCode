[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ScratchRoot)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-CorpusReuseTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object {
                $_.ToString('X2')
            }) -join '')
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

$scratchFull = [IO.Path]::GetFullPath($ScratchRoot)
if (-not [String]::Equals([IO.Path]::GetPathRoot($scratchFull), 'H:\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Acceptance corpus reuse scratch must remain on H:; got $scratchFull"
}
[IO.Directory]::CreateDirectory($scratchFull) | Out-Null
$scratchItem = Get-Item -LiteralPath $scratchFull -Force
if (($scratchItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Acceptance corpus reuse scratch is a reparse point: $scratchFull"
}
$runRoot = Join-Path $scratchFull ("run-{0}" -f [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null

try {
    $sourcePath = Join-Path $PSScriptRoot 'DeterministicSimulationValidation.Tests.ps1'
    $sourceText = [IO.File]::ReadAllText($sourcePath)
    $tokens = $null
    $parseErrors = $null
    $sourceAst = [Management.Automation.Language.Parser]::ParseInput(
        $sourceText, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -ne 0) {
        throw "The Acceptance entrypoint does not parse: $($parseErrors[0].Message)"
    }

    $reparseCleanupDefinitions = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Remove-Stage5AcceptanceReparseFixtureLink'
    }, $true))
    Assert-CorpusReuseTest ($reparseCleanupDefinitions.Count -eq 1) `
        'Acceptance must define one exact-path reparse fixture cleanup helper.'
    Invoke-Expression $reparseCleanupDefinitions[0].Extent.Text
    $scratchTreeGuardDefinitions = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5AcceptanceScratchTreeContainsNoReparsePoints'
    }, $true))
    Assert-CorpusReuseTest ($scratchTreeGuardDefinitions.Count -eq 1) `
        'Acceptance scratch cleanup must define one fail-closed reparse preflight.'
    Invoke-Expression $scratchTreeGuardDefinitions[0].Extent.Text

    $reparseRootAssignments = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -ceq '$reparseRoot'
    }, $true))
    $acceptanceSchemaCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Assert-Stage5FinalAcceptanceEvidenceSchemaContract'
    }, $true))
    $reparseJunctionCreates = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'New-Item' -and
            $node.Extent.Text -match '-ItemType\s+Junction'
    }, $true))
    $sharedProducerCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'New-CombinedHostProducerTestCase' -and
            $node.Extent.Text -match "'shared'"
    }, $true))
    Assert-CorpusReuseTest ($reparseRootAssignments.Count -eq 1 -and
        $acceptanceSchemaCalls.Count -eq 1 -and
        $reparseJunctionCreates.Count -eq 1 -and $sharedProducerCalls.Count -eq 1 -and
        $reparseRootAssignments[0].Extent.StartOffset -lt
            $acceptanceSchemaCalls[0].Extent.StartOffset -and
        $reparseJunctionCreates[0].Extent.StartOffset -lt
            $acceptanceSchemaCalls[0].Extent.StartOffset -and
        $reparseJunctionCreates[0].Extent.StartOffset -lt
            $sharedProducerCalls[0].Extent.StartOffset) `
        'The reparse-negative fixture must run before the long Acceptance and shared-producer work.'
    $reparseFixtureTryBlocks = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.TryStatementAst] -and
            $null -ne $node.Finally -and
            $node.Body.Extent.Text -match '-ItemType\s+Junction' -and
            @($node.Finally.FindAll({
                param($candidate)
                $candidate -is [Management.Automation.Language.CommandAst] -and
                    $candidate.GetCommandName() -ceq 'Remove-Stage5AcceptanceReparseFixtureLink'
            }, $true)).Count -eq 2
    }, $true))
    $reparseCleanupCalls = if ($reparseFixtureTryBlocks.Count -eq 1) {
        @($reparseFixtureTryBlocks[0].Finally.FindAll({
            param($node)
            $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -ceq 'Remove-Stage5AcceptanceReparseFixtureLink'
        }, $true))
    }
    else { @() }
    Assert-CorpusReuseTest ($reparseFixtureTryBlocks.Count -eq 1 -and
        $reparseCleanupCalls.Count -eq 2) `
        'The reparse-negative fixture must unlink both known link candidates in its finally block.'

    $reparseCleanupProbeRoot = Join-Path $runRoot 'reparse-cleanup-probe'
    $reparseProbeManifest = Join-Path $reparseCleanupProbeRoot 'manifest'
    $reparseProbeTarget = Join-Path $reparseCleanupProbeRoot 'outside'
    [IO.Directory]::CreateDirectory($reparseProbeManifest) | Out-Null
    [IO.Directory]::CreateDirectory($reparseProbeTarget) | Out-Null
    $reparseProbeTargetFile = Join-Path $reparseProbeTarget 'sentinel.txt'
    [IO.File]::WriteAllText($reparseProbeTargetFile, 'outside target remains intact')
    $reparseProbeLink = Join-Path $reparseProbeManifest 'linked'
    New-Item -ItemType Junction -Path $reparseProbeLink `
        -Target $reparseProbeTarget -ErrorAction Stop | Out-Null
    $reparseProbeLinkItem = Get-Item -LiteralPath $reparseProbeLink -Force
    Assert-CorpusReuseTest (($reparseProbeLinkItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) `
        'The cleanup regression probe must create a real directory reparse point.'
    $outsideProbeFile = Join-Path $runRoot 'outside-reparse-probe.txt'
    [IO.File]::WriteAllText($outsideProbeFile, 'must not be removed')
    $outsidePathRejected = $false
    try {
        Remove-Stage5AcceptanceReparseFixtureLink -FixtureRoot $reparseCleanupProbeRoot `
            -LinkPath $outsideProbeFile
    }
    catch { $outsidePathRejected = $_.Exception.Message -match 'outside its owned root' }
    Assert-CorpusReuseTest ($outsidePathRejected -and [IO.File]::Exists($outsideProbeFile)) `
        'Exact-link cleanup must reject and preserve a path outside its fixture root.'
    $ordinaryDirectory = Join-Path $reparseProbeManifest 'ordinary-directory'
    [IO.Directory]::CreateDirectory($ordinaryDirectory) | Out-Null
    $ordinaryPathRejected = $false
    try {
        Remove-Stage5AcceptanceReparseFixtureLink -FixtureRoot $reparseCleanupProbeRoot `
            -LinkPath $ordinaryDirectory
    }
    catch { $ordinaryPathRejected = $_.Exception.Message -match 'non-reparse path' }
    Assert-CorpusReuseTest ($ordinaryPathRejected -and
        [IO.Directory]::Exists($ordinaryDirectory)) `
        'Exact-link cleanup must reject and preserve a normal directory.'
    $scratchTreeRejected = $false
    try {
        Assert-Stage5AcceptanceScratchTreeContainsNoReparsePoints `
            -RootPath $reparseCleanupProbeRoot
    }
    catch { $scratchTreeRejected = $_.Exception.Message -match 'contains a reparse point' }
    Assert-CorpusReuseTest ($scratchTreeRejected -and
        [IO.File]::ReadAllText($reparseProbeTargetFile) -ceq
            'outside target remains intact') `
        'Scratch-tree cleanup preflight must refuse a junction without traversing or changing its target.'
    Remove-Stage5AcceptanceReparseFixtureLink -FixtureRoot $reparseCleanupProbeRoot `
        -LinkPath $reparseProbeLink
    Assert-CorpusReuseTest (-not (Test-Path -LiteralPath $reparseProbeLink) -and
        [IO.File]::ReadAllText($reparseProbeTargetFile) -ceq
            'outside target remains intact') `
        'Nonrecursive unlink must remove only the junction object and preserve target contents.'
    Assert-Stage5AcceptanceScratchTreeContainsNoReparsePoints `
        -RootPath $reparseCleanupProbeRoot

    # Acceptance runs this receipt through the production validator only after
    # constructing and hashing its full diagnostic corpus. Exercise this one
    # cheap contract before that expensive path so stale synthetic evidence
    # cannot consume the outer CTest timeout before reporting its real defect.
    $detailsAssignments = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -ceq '$detailsByKind'
    }, $true))
    Assert-CorpusReuseTest ($detailsAssignments.Count -eq 1) `
        'The Acceptance entrypoint must have one identifiable details fixture.'
    $sourceCommit = 'a' * 40
    $detailsByKind = Invoke-Expression $detailsAssignments[0].Right.Extent.Text
    $installedRuntimeDetails = $detailsByKind[
        'combined-stage4-stage5-installed-runtime']
    Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') `
        -Force
    $evidenceModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    try {
        & $evidenceModule {
            param($Details)
            Assert-Stage5FinalAcceptanceDetails `
                'combined-stage4-stage5-installed-runtime' $Details ('a' * 40) @{}
        } $installedRuntimeDetails
    }
    catch {
        throw "The installed-runtime Acceptance fixture violates the production details contract: $($_.Exception.Message)"
    }

    $replayBindingDefinitions = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Set-Stage5ReplayEvidenceHashBinding'
    }, $true))
    Assert-CorpusReuseTest ($replayBindingDefinitions.Count -eq 1) `
        'The Acceptance entrypoint must define one replay-evidence hash rebinding helper.'
    Invoke-Expression $replayBindingDefinitions[0].Extent.Text
    $replayBindingCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Set-Stage5ReplayEvidenceHashBinding'
    }, $true))
    Assert-CorpusReuseTest ($replayBindingCalls.Count -eq 3) `
        'Duplicate-title, restored-source, and swapped-title acceptance probes must each refresh the cross-evidence hash.'

    $replayBindingProbePath = Join-Path $runRoot 'replay-binding-probe.json'
    [IO.File]::WriteAllText($replayBindingProbePath, '{"marker":"mutated replay"}')
    $actualReplayHash = Get-Sha256 $replayBindingProbePath
    $runtimeDetailsProbe = [ordered]@{
        gateName = 'deterministic-runtime'; isolatedPipelineMode = 'serial'
        simulationModes = @('serial', 'parallel', 'shadow')
        workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
            'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
        isolatedMatrixPassed = $true; finalAcceptanceClaim = $false
        replayEvidenceSha256 = 'A' * 64
        freshAiEvidenceSha256 = 'B' * 64
        performanceEvidenceSha256 = 'C' * 64
        installedKernelExecution = [ordered]@{
            status = 'skipped'; claim = $false
            reason = 'external-qualification-exempt-and-reviewed-native-fixture-unavailable'
            sha256 = $null
        }
    }
    $runtimeHashesProbe = @{
        'replay-determinism' = $actualReplayHash
        'fresh-ai' = 'B' * 64
        'performance-scaling' = 'C' * 64
    }
    $staleReplayBindingError = ''
    try {
        & $evidenceModule {
            param($Details, $EvidenceHashes)
            Assert-Stage5FinalAcceptanceDetails 'deterministic-runtime' `
                $Details ('a' * 40) $EvidenceHashes
        } $runtimeDetailsProbe $runtimeHashesProbe
    }
    catch { $staleReplayBindingError = $_.Exception.Message }
    Assert-CorpusReuseTest ($staleReplayBindingError -match
        'replayEvidenceSha256 does not bind the independently hashed replay-determinism evidence') `
        'The focused replay mutation probe must expose the stale cross-evidence binding before title-specific validation.'

    $runtimeEvidenceProbe = [ordered]@{ details = $runtimeDetailsProbe }
    Set-Stage5ReplayEvidenceHashBinding $replayBindingProbePath $runtimeEvidenceProbe
    Assert-CorpusReuseTest ($runtimeEvidenceProbe.details.replayEvidenceSha256 -ceq
        $actualReplayHash) `
        'Refreshing the deterministic-runtime binding must hash the current replay evidence bytes.'
    try {
        & $evidenceModule {
            param($Details, $EvidenceHashes)
            Assert-Stage5FinalAcceptanceDetails 'deterministic-runtime' `
                $Details ('a' * 40) $EvidenceHashes
        } $runtimeEvidenceProbe.details $runtimeHashesProbe
    }
    catch {
        throw "The rebound runtime evidence does not satisfy its production contract: $($_.Exception.Message)"
    }

    $helperNames = @(
        'Add-Stage5AcceptanceFileSnapshot',
        'Restore-Stage5AcceptanceFileSnapshot',
        'Invoke-Stage5AcceptanceMutationCase'
    )
    $helperDefinitions = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $helperNames -ccontains $node.Name
    }, $true))
    Assert-CorpusReuseTest ($helperDefinitions.Count -eq $helperNames.Count) `
        'The exact-byte acceptance mutation helpers are missing or duplicated.'
    foreach ($definition in $helperDefinitions) {
        Invoke-Expression $definition.Extent.Text
    }

    $copyCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'New-CombinedHostProducerTestCase'
    }, $true))
    $copyFunctions = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'New-CombinedHostProducerTestCase'
    }, $true))
    $producerCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Invoke-CombinedHostProducerTestCase'
    }, $true))
    $mutationCalls = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Invoke-Stage5AcceptanceMutationCase'
    }, $true))
    Assert-CorpusReuseTest ($copyCalls.Count -eq 1) `
        'Acceptance must create one relocated corpus for all combined producer cases.'
    Assert-CorpusReuseTest ($copyFunctions.Count -eq 1) `
        'The corpus creation helper must be uniquely identifiable for scoped assertions.'
    $corpusCardinalityGuards = @($copyFunctions[0].FindAll({
        param($node)
        if ($node -isnot [Management.Automation.Language.IfStatementAst]) {
            return $false
        }
        $cardinalityClauses = @($node.Clauses | Where-Object {
            $clause = $_
            $inequalityChecks = @($clause.Item1.FindAll({
                param($candidate)
                $candidate -is [Management.Automation.Language.BinaryExpressionAst] -and
                    $candidate.Operator -eq [Management.Automation.Language.TokenKind]::Ine
            }, $true))
            $childCountCheck = @($inequalityChecks | Where-Object {
                $_.Left.Extent.Text -match '^\s*@\(\$template\.corpus\.children\)\.Count\s*$' -and
                    $_.Right.Extent.Text.Trim() -ceq '253'
            }).Count -eq 1
            $rawLogCountCheck = @($inequalityChecks | Where-Object {
                $_.Left.Extent.Text -match '^\s*\[int\]\$template\.corpus\.rawLogCount\s*$' -and
                    $_.Right.Extent.Text.Trim() -ceq '507'
            }).Count -eq 1
            $cardinalityFailure = @($clause.Item2.FindAll({
                param($candidate)
                $candidate -is [Management.Automation.Language.ThrowStatementAst] -and
                    $candidate.Extent.Text -match 'cardinality changed before copying'
            }, $true)).Count -eq 1
            $childCountCheck -and $rawLogCountCheck -and $cardinalityFailure
        })
        return ($cardinalityClauses.Count -eq 1)
    }, $true))
    Assert-CorpusReuseTest ($corpusCardinalityGuards.Count -eq 1) `
        'The corpus creation helper must actively guard the 253-child / 507-raw-log cardinality in an if condition.'

    $pathModeLoops = @($sourceAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.ForEachStatementAst] -and
            $node.Variable.Extent.Text -ceq '$pathMode'
    }, $true))
    Assert-CorpusReuseTest ($pathModeLoops.Count -eq 1) `
        'The three unsafe native-path negatives must remain in one path-mode loop.'
    $pathModeEntries = @($pathModeLoops[0].Condition.FindAll({
        param($node)
        $node -is [Management.Automation.Language.HashtableAst]
    }, $true))
    $pathModeNames = @($pathModeEntries | ForEach-Object {
        $modeMatch = [regex]::Match($_.Extent.Text,
            "(?i)\bmode\s*=\s*'([^']+)'\s*;")
        if (-not $modeMatch.Success) {
            throw "Path-mode loop entry has no literal mode: $($_.Extent.Text)"
        }
        $modeMatch.Groups[1].Value
    })
    $expectedPathModeNames = @('traversal', 'ads', 'drive-relative')
    Assert-CorpusReuseTest ($pathModeNames.Count -eq $expectedPathModeNames.Count -and
        @($pathModeNames | Select-Object -Unique).Count -eq $expectedPathModeNames.Count -and
        @((Compare-Object $expectedPathModeNames $pathModeNames)).Count -eq 0) `
        'The path-mode loop must retain traversal, ADS, and drive-relative cases.'

    $loopedProducerCalls = @($pathModeLoops[0].Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Invoke-CombinedHostProducerTestCase'
    }, $true))
    $directProducerCallCount = $producerCalls.Count - $loopedProducerCalls.Count
    Assert-CorpusReuseTest ($loopedProducerCalls.Count -eq 1 -and
        $directProducerCallCount -eq 6 -and
        ($directProducerCallCount + $pathModeNames.Count) -eq 9) `
        'The producer call sites must still execute all nine full-corpus cases.'
    foreach ($call in $producerCalls) {
        Assert-CorpusReuseTest ($call.CommandElements.Count -ge 2 -and
            $call.CommandElements[1].Extent.Text -ceq '$combinedProducerRoot') `
            'Every combined producer case must use the shared complete corpus.'
    }
    Assert-CorpusReuseTest ($mutationCalls.Count -eq 6) `
        'Every file-mutating case group must run inside the restore-on-exit helper.'
    Assert-CorpusReuseTest ($sourceText -match
        '(?s)combined-producer-output-\{0\}.*?combined-results\.json') `
        'Each producer invocation must retain a distinct output directory.'

    $cmakePath = Join-Path $PSScriptRoot 'CMakeLists.txt'
    $cmakeText = [IO.File]::ReadAllText($cmakePath)
    Assert-CorpusReuseTest ($cmakeText -match
        '(?s)core_stage5_acceptance_corpus_reuse_tests.*?Stage5AcceptanceCorpusReuse\.Tests\.ps1.*?TIMEOUT 60') `
        'The focused corpus reuse self-test must be registered with a bounded CTest timeout.'

    $fixtureRoot = Join-Path $runRoot 'restore-contract'
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    $firstPath = Join-Path $fixtureRoot 'first.json'
    $secondPath = Join-Path $fixtureRoot 'second.json'
    $untouchedPath = Join-Path $fixtureRoot 'untouched.json'
    $encoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($firstPath, '{"generation":1}', $encoding)
    [IO.File]::WriteAllText($secondPath, '{"generation":2}', $encoding)
    [IO.File]::WriteAllText($untouchedPath, '{"generation":3}', $encoding)
    $firstOriginal = [Convert]::ToBase64String([IO.File]::ReadAllBytes($firstPath))
    $secondOriginal = [Convert]::ToBase64String([IO.File]::ReadAllBytes($secondPath))

    $caught = $false
    try {
        Invoke-Stage5AcceptanceMutationCase {
            param($snapshot)
            Add-Stage5AcceptanceFileSnapshot $snapshot $firstPath
            Add-Stage5AcceptanceFileSnapshot $snapshot $secondPath
            Add-Stage5AcceptanceFileSnapshot $snapshot $firstPath
            Assert-CorpusReuseTest ($snapshot.Count -eq 2) `
                'Repeated snapshot registration must preserve one original byte array per file.'
            [IO.File]::WriteAllText($firstPath, '{"generation":"mutated-1"}', $encoding)
            [IO.File]::WriteAllText($secondPath, '{"generation":"mutated-2"}', $encoding)
            [IO.File]::WriteAllText($untouchedPath, '{"generation":"outside-snapshot"}', $encoding)
            throw 'expected snapshot restoration probe'
        }
    }
    catch {
        $caught = $_.Exception.Message -ceq 'expected snapshot restoration probe'
    }
    Assert-CorpusReuseTest $caught 'The mutation helper did not propagate the expected case failure.'
    Assert-CorpusReuseTest ([Convert]::ToBase64String([IO.File]::ReadAllBytes($firstPath)) -ceq
        $firstOriginal -and
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($secondPath)) -ceq $secondOriginal) `
        'A failed case must restore every selected source document byte-for-byte.'
    Assert-CorpusReuseTest ((Get-Content -LiteralPath $untouchedPath -Raw) -ceq
        '{"generation":"outside-snapshot"}') `
        'Restoration must leave files outside the case snapshot untouched.'

    Invoke-Stage5AcceptanceMutationCase {
        param($snapshot)
        Add-Stage5AcceptanceFileSnapshot $snapshot $firstPath
        [IO.File]::WriteAllText($firstPath, '{"generation":"successful-case"}', $encoding)
    }
    Assert-CorpusReuseTest ([Convert]::ToBase64String([IO.File]::ReadAllBytes($firstPath)) -ceq
        $firstOriginal) 'A successful case must also restore its source document byte-for-byte.'

    Write-Output 'Stage 5 Acceptance preflight self-test passed: early reparse fixture cleanup, installed-runtime/replay bindings, one full corpus, nine producer cases, and byte-identical mutation restores.'
}
finally {
    if ($null -ne (Get-Command Assert-Stage5AcceptanceScratchTreeContainsNoReparsePoints `
            -ErrorAction SilentlyContinue)) {
        Assert-Stage5AcceptanceScratchTreeContainsNoReparsePoints -RootPath $runRoot
    }
    else {
        $runItem = Get-Item -LiteralPath $runRoot -Force
        if (($runItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to remove Acceptance corpus reuse reparse point: $runRoot"
        }
    }
    Remove-Item -LiteralPath $runRoot -Recurse -Force
}
