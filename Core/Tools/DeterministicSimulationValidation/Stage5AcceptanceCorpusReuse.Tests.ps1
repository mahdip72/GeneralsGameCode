[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ScratchRoot)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-CorpusReuseTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
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
    $copyFunctionText = $copyFunctions[0].Extent.Text
    Assert-CorpusReuseTest ($copyFunctionText -match
        '(?s)@\(\$template\.corpus\.children\)\.Count\s*-ne\s*253' -and
        $copyFunctionText -match '\[int\]\$template\.corpus\.rawLogCount\s*-ne\s*507') `
        'The corpus creation helper must retain its 253-child / 507-raw-log assertions.'

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

    Write-Output 'Stage 5 Acceptance corpus reuse self-test passed: one full corpus, nine producer cases, and byte-identical mutation restores.'
}
finally {
    $runItem = Get-Item -LiteralPath $runRoot -Force
    if (($runItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to remove Acceptance corpus reuse reparse point: $runRoot"
    }
    Remove-Item -LiteralPath $runRoot -Recurse -Force
}
