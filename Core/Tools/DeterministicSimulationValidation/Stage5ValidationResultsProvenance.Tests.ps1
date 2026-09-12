[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ScratchRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    $caught = $null
    try { & $Action } catch { $caught = $_ }
    Assert-Test ($null -ne $caught -and $caught.Exception.Message -match $Pattern) `
        "$Message (got '$($caught.Exception.Message)')"
}

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
$runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$tokens = $null; $parseErrors = $null
$tree = [Management.Automation.Language.Parser]::ParseFile(
    $runnerPath, [ref]$tokens, [ref]$parseErrors)
Assert-Test (@($parseErrors).Count -eq 0) 'runner must parse before provenance tests'
foreach ($definition in @($tree.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst]
})) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
Assert-Test ($null -ne (Get-Command New-Stage5ResultExecutionProvenance `
    -CommandType Function -ErrorAction SilentlyContinue)) `
    'runner must expose the per-result execution-provenance producer'
Assert-Test ($null -ne (Get-Command Assert-Stage5NativeCommandLineMatchesPlan `
    -CommandType Function -ErrorAction SilentlyContinue)) `
    'runner must expose the native command-line/plan comparator'
$nativeReceiptDefinition = $tree.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Get-NativePerformanceReceiptReference'
}, $true)
$processDefinition = $tree.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Invoke-ValidationProcess'
}, $true)
Assert-Test ($null -ne $nativeReceiptDefinition -and
    $nativeReceiptDefinition.Extent.Text -match
        'Assert-Stage5NativeCommandLineMatchesPlan' -and
    $nativeReceiptDefinition.Extent.Text -match '\$ExpectedArguments' -and
    $null -ne $processDefinition -and $processDefinition.Extent.Text -match
        '(?s)-ExpectedArguments\s+\(\[string\[\]\]@\(\$Entry\.arguments\)\)') `
    'actual native receipt selection must consume exact arguments from the launched plan entry'

$scratch = [IO.Path]::GetFullPath($ScratchRoot)
Assert-Test ($scratch.StartsWith('H:\', [StringComparison]::OrdinalIgnoreCase)) `
    'focused provenance tests require task-owned H: scratch'
$testRoot = Join-Path $scratch ('stage5-results-provenance-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))
$sourceCommit = ('a' * 40) -join ''
$artifactSet = ('B' * 64) -join ''
$executableHash = ('C' * 64) -join ''
$cohortNonce = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
$cohortCreatedUtc = '2026-09-04T12:00:00.0000000Z'
$runtimeClosure = [ordered]@{
    dependencyManifestSha256 = ('D' * 64) -join ''
    closureSha256 = ('E' * 64) -join ''
}
$qualificationData = [ordered]@{
    path = 'QualificationData.json'
    title = 'ZeroHour'
    manifestSha256 = ('F' * 64) -join ''
    closureSha256 = ('1' * 64) -join ''
    fileCount = 6
}

function Write-TestText {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function New-TestChildRun {
    param([int]$Sequence)
    $prefix = '{0:D4}-fixture' -f $Sequence
    $stdout = Join-Path $testRoot "runs\$prefix.stdout.log"
    $stderr = Join-Path $testRoot "runs\$prefix.stderr.log"
    $native = Join-Path $testRoot "native\$prefix.receipt.json"
    Write-TestText $stdout "stdout-$Sequence`n"
    Write-TestText $stderr "stderr-$Sequence`n"
    Write-TestText $native ('{"sequence":' + $Sequence + '}')
    $nonce = '00000000-0000-4000-8000-{0:D12}' -f $Sequence
    $arguments = @('-headless', '-simulationMode', 'parallel', '-workerCount', [string]$Sequence)
    $command = '"H:\Installed\generalszh.exe" ' + ($arguments -join ' ')
    $entry = [pscustomobject]@{
        sequence = $Sequence; kind = 'ai'; command = $command; arguments = $arguments
        stdout = $stdout; stderr = $stderr
    }
    $child = [pscustomobject]@{
        processId = 41000 + $Sequence
        runNonce = $nonce
        processCreationUtc = ('2026-09-04T12:00:{0:D2}.0000000Z' -f $Sequence)
        executablePath = 'H:\Installed\generalszh.exe'
        commandLine = $command
        stdoutSha256 = Get-Sha256 $stdout
        stderrSha256 = Get-Sha256 $stderr
        nativeReceipt = [pscustomobject]@{
            path = ConvertTo-OutputRelativePath $native $testRoot 'test native receipt'
            sha256 = Get-Sha256 $native
            producer = 'game-executable-stage5-performance-report-v5'
            runNonce = $nonce
            cohortNonce = $cohortNonce
        }
    }
    return [pscustomobject]@{
        entry = $entry
        run = [pscustomobject]@{ exitCode = 0; timedOut = $false; childProcess = $child }
        childProcess = $child
    }
}

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $planPath = Join-Path $testRoot 'validation-plan.json'
    Write-TestText $planPath '{"schemaVersion":2}'
    $planHash = Get-Sha256 $planPath
    Assert-Test (Test-Sha256Text $planHash) "test plan hash is invalid: '$planHash'"
    $runs = @(New-TestChildRun 1; New-TestChildRun 2; New-TestChildRun 3)
    $nativeCommand = 'H:\Installed\generalszh.exe ' +
        ($runs[0].entry.arguments -join ' ')
    Assert-Stage5NativeCommandLineMatchesPlan $nativeCommand `
        $runs[0].childProcess.executablePath $runs[0].entry.arguments `
        'matching native command' | Out-Null
    Assert-Rejected {
        Assert-Stage5NativeCommandLineMatchesPlan `
            ($nativeCommand -replace '-workerCount 1', '-workerCount 8') `
            $runs[0].childProcess.executablePath $runs[0].entry.arguments `
            'easier native command' | Out-Null
    } 'command line.*plan|arguments.*plan' `
        'native execution with substituted worker flags must not back the canonical plan'
    $results = New-Object 'Collections.Generic.List[object]'
    foreach ($run in $runs) {
        $provenance = New-Stage5ResultExecutionProvenance `
            -ChildRun $run -OutputRoot $testRoot -Title 'ZeroHour' `
            -SourceCommit $sourceCommit -ArtifactSetSha256 $artifactSet `
            -ExecutableSha256 $executableHash -CohortNonce $cohortNonce `
            -CohortCreatedUtc $cohortCreatedUtc -RuntimeClosure $runtimeClosure `
            -QualificationData $qualificationData `
            -PlanPath $planPath -PlanSha256 $planHash
        $results.Add([pscustomobject]@{
            sequence = $run.entry.sequence
            kind = 'ai'
            executionProvenance = $provenance
        }) | Out-Null
    }
    $resultsPath = Join-Path $testRoot 'validation-results.json'
    Write-TestText $resultsPath ($results.ToArray() | ConvertTo-Json -Depth 10)

    $receiptPath = Join-Path $testRoot 'validation-results-receipt.json'
    Write-Stage5HostRunnerReceipt -Role 'validation-results' `
        -OutputRoot $testRoot -ReceiptPath $receiptPath -Title 'ZeroHour' `
        -SourceCommit $sourceCommit -ArtifactSetSha256 $artifactSet `
        -ExecutableSha256 $executableHash `
        -RawLogPaths (@($resultsPath) + @($runs | ForEach-Object {
            $_.entry.stdout; $_.entry.stderr
        })) -ChildRuns @($runs[2], $runs[0], $runs[1]) `
        -Results $results.ToArray() -Details ([ordered]@{
            resultCount = 3; allExecutionsPassed = $true
            resultsSha256 = Get-Sha256 $resultsPath
        }) -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
        -RuntimeClosure $runtimeClosure -QualificationData $qualificationData `
        -PlanPath $planPath `
        -PlanSha256 $planHash | Out-Null
    $receiptText = Get-Content -LiteralPath $receiptPath -Raw
    $receipt = if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) {
        $receiptText | ConvertFrom-Json -DateKind String
    } else { $receiptText | ConvertFrom-Json }
    $children = @($receipt.provenance.children)
    Assert-Test ($children.Count -eq 3 -and
        (@($children.sequence) -join '|') -ceq '1|2|3') `
        'validation-results receipt must bind every execution in ascending sequence order'
    foreach ($index in 0..2) {
        $child = $children[$index]
        $resultProvenance = $results[$index].executionProvenance
        Assert-Test ($child.sequence -eq $results[$index].sequence) `
            "receipt child sequence mismatch at $($index + 1)"
        Assert-Test ($child.runNonce -ceq $resultProvenance.runNonce) `
            "result runNonce mismatch at $($index + 1)"
        Assert-Test ($child.processId -eq $resultProvenance.processId -and
            $child.processCreationUtc -ceq $resultProvenance.processCreationUtc) `
            "result process identity mismatch at $($index + 1)"
        Assert-Test ($child.commandLine -ceq $runs[$index].entry.command) `
            "receipt command mismatch at $($index + 1)"
        Assert-Test ((@($child.arguments) -join "`0") -ceq
            (@($runs[$index].entry.arguments) -join "`0")) `
            "receipt arguments mismatch at $($index + 1)"
        Assert-Test ($child.stdout.sha256 -ceq $resultProvenance.stdout.sha256 -and
            $child.stderr.sha256 -ceq $resultProvenance.stderr.sha256) `
            "result stream hash mismatch at $($index + 1)"
        Assert-Test ($child.nativeReceipt.sha256 -ceq
            $resultProvenance.nativeReceipt.sha256) `
            "result native receipt mismatch at $($index + 1)"
        Assert-Test (($child.qualificationData | ConvertTo-Json -Compress) -ceq
            ($qualificationData | ConvertTo-Json -Compress) -and
            ($resultProvenance.qualificationData | ConvertTo-Json -Compress) -ceq
            ($qualificationData | ConvertTo-Json -Compress)) `
            "result and child qualification-data binding mismatch at $($index + 1)"
        Assert-Test ($resultProvenance.plan.sha256 -ceq $planHash -and
            $resultProvenance.sourceCommit -ceq $sourceCommit -and
            $resultProvenance.artifactSetSha256 -ceq $artifactSet -and
            $resultProvenance.cohortNonce -ceq $cohortNonce -and
            $resultProvenance.runtimeClosure.closureSha256 -ceq
                $runtimeClosure.closureSha256) `
            "result provenance must retain plan/source/artifact/cohort/runtime closure for sequence $($index + 1)"
    }
    Assert-Test (($receipt.details.qualificationData | ConvertTo-Json -Compress) -ceq
        ($qualificationData | ConvertTo-Json -Compress)) `
        'validation-results receipt details must retain the exact qualification-data binding'

    $originalCommand = $results[0].executionProvenance.commandLine
    $results[0].executionProvenance.commandLine = $originalCommand + ' -easier'
    try {
        Assert-Rejected {
            Write-Stage5HostRunnerReceipt -Role 'validation-results' `
                -OutputRoot $testRoot -ReceiptPath (Join-Path $testRoot 'mismatched-result.json') `
                -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -RawLogPaths @($resultsPath) -ChildRuns $runs `
                -Results $results.ToArray() -Details ([ordered]@{}) `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'provenance differs.*child.*plan.*runtime' `
            'a result row cannot claim a command different from its child and plan'
    }
    finally { $results[0].executionProvenance.commandLine = $originalCommand }

    $originalQualificationClosure =
        $results[0].executionProvenance.qualificationData.closureSha256
    $results[0].executionProvenance.qualificationData.closureSha256 = ('2' * 64)
    try {
        Assert-Rejected {
            Write-Stage5HostRunnerReceipt -Role 'validation-results' `
                -OutputRoot $testRoot `
                -ReceiptPath (Join-Path $testRoot 'mismatched-qualification-data.json') `
                -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -RawLogPaths @($resultsPath) -ChildRuns $runs `
                -Results $results.ToArray() -Details ([ordered]@{}) `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'provenance differs.*child.*plan.*runtime' `
            'a result row cannot substitute another qualification-data closure'
    }
    finally {
        $results[0].executionProvenance.qualificationData.closureSha256 =
            $originalQualificationClosure
    }

    $originalCreation = $runs[0].childProcess.processCreationUtc
    $runs[0].childProcess.processCreationUtc = '2026-09-04T11:59:59.0000000Z'
    try {
        Assert-Rejected {
            Write-Stage5HostRunnerReceipt -Role 'validation-results' `
                -OutputRoot $testRoot -ReceiptPath (Join-Path $testRoot 'stale-process.json') `
                -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -RawLogPaths @($resultsPath) -ChildRuns $runs `
                -Results $results.ToArray() -Details ([ordered]@{}) `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'process creation.*current-cohort' `
            'a pre-cohort child process identity must not enter validation results'
    }
    finally { $runs[0].childProcess.processCreationUtc = $originalCreation }

    $originalProducer = $runs[0].childProcess.nativeReceipt.producer
    $runs[0].childProcess.nativeReceipt.producer = 'host-asserted-substitute'
    try {
        Assert-Rejected {
            New-Stage5ResultExecutionProvenance -ChildRun $runs[0] `
                -OutputRoot $testRoot -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'native executable receipt' `
            'a host-asserted substitute must not replace the native executable producer'
    }
    finally { $runs[0].childProcess.nativeReceipt.producer = $originalProducer }

    $originalNativeHash = $runs[0].childProcess.nativeReceipt.sha256
    $runs[0].childProcess.nativeReceipt.sha256 = ('F' * 64)
    try {
        Assert-Rejected {
            New-Stage5ResultExecutionProvenance -ChildRun $runs[0] `
                -OutputRoot $testRoot -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'native receipt changed' `
            'a changed native receipt hash must fail before result serialization'
    }
    finally { $runs[0].childProcess.nativeReceipt.sha256 = $originalNativeHash }

    $originalPid = $runs[1].childProcess.processId
    $originalSecondCreation = $runs[1].childProcess.processCreationUtc
    $runs[1].childProcess.processId = $runs[0].childProcess.processId
    $runs[1].childProcess.processCreationUtc = $runs[0].childProcess.processCreationUtc
    try {
        Assert-Rejected {
            Write-Stage5HostRunnerReceipt -Role 'validation-results' `
                -OutputRoot $testRoot -ReceiptPath (Join-Path $testRoot 'duplicate-process.json') `
                -Title 'ZeroHour' -SourceCommit $sourceCommit `
                -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
                -RawLogPaths @($resultsPath) -ChildRuns $runs `
                -Results $results.ToArray() -Details ([ordered]@{}) `
                -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
                -RuntimeClosure $runtimeClosure `
                -QualificationData $qualificationData -PlanPath $planPath `
                -PlanSha256 $planHash | Out-Null
        } 'reuses a child process identity' `
            'one OS process identity cannot satisfy two validation results'
    }
    finally {
        $runs[1].childProcess.processId = $originalPid
        $runs[1].childProcess.processCreationUtc = $originalSecondCreation
    }

    $duplicateRuns = @($runs[0], $runs[1])
    $duplicateRuns[1].childProcess.runNonce = $duplicateRuns[0].childProcess.runNonce
    $duplicateRuns[1].childProcess.nativeReceipt.runNonce = $duplicateRuns[0].childProcess.runNonce
    Assert-Rejected {
        Write-Stage5HostRunnerReceipt -Role 'validation-results' `
            -OutputRoot $testRoot -ReceiptPath (Join-Path $testRoot 'duplicate.json') `
            -Title 'ZeroHour' -SourceCommit $sourceCommit `
            -ArtifactSetSha256 $artifactSet -ExecutableSha256 $executableHash `
            -RawLogPaths @($resultsPath) -ChildRuns $duplicateRuns `
            -Results @($results[0], $results[1]) -Details ([ordered]@{}) `
            -CohortNonce $cohortNonce -CohortCreatedUtc $cohortCreatedUtc `
            -RuntimeClosure $runtimeClosure `
            -QualificationData $qualificationData -PlanPath $planPath `
            -PlanSha256 $planHash | Out-Null
    } 'reuses.*runNonce|unique.*runNonce' `
        'validation-results receipt must reject cross-result run nonce reuse'

    Write-Output 'Stage 5 validation-results execution provenance tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
