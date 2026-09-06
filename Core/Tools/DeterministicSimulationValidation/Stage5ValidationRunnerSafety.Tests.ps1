param(
    [string]$ScratchRoot = ''
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-RunnerRejects {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )
    $rejected = $false
    $errorText = ''
    try {
        & $Action
    }
    catch {
        $errorText = $_.Exception.Message
        $rejected = $errorText -match $Pattern
    }
    Assert-True $rejected "$Message (got '$errorText')"
}

function Get-Sha256 {
    param([string]$Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $stream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Write-SafetyManifest {
    param([string]$Path, [string]$Title, [string]$Executable, [string]$ExecutableHash,
        [string]$ReplayHash, [int[]]$Seeds = @(1729),
        [string[]]$Scenarios = @('4v3'))
    $manifest = [ordered]@{
        schemaVersion = 1
        title = $Title
        executable = $Executable
        executableSha256 = $ExecutableHash
        fixtures = @(
            [ordered]@{
                id = 'reference'
                source = 'fixtures\reference.rep'
                sha256 = $ReplayHash
                stress = $false
                maps = @()
            }
        )
        ai = [ordered]@{
            seeds = $Seeds
            scenarios = $Scenarios
            repeats = 1
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

$runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$runnerSource = Get-Content -LiteralPath $runnerPath -Raw
$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
else {
    throw 'Stage5ValidationRunnerSafety.Tests.ps1 requires an explicit scratch root.'
}
$testRoot = Join-Path $scratchParent ('stage5-runner-safety-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    $runtime = Join-Path $testRoot 'runtime'
    $fixtures = Join-Path $testRoot 'fixtures'
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    New-Item -ItemType Directory -Path $fixtures -Force | Out-Null
    $executable = Join-Path $runtime 'generalszh.exe'
    $launcher = Join-Path $runtime 'launcher.exe'
    $launcherConfig = Join-Path $runtime 'launcher.lcf'
    $replay = Join-Path $fixtures 'reference.rep'
    # This synthetic file is never launched. Advertise the current contract so
    # later setup-guard fixtures reach the specific boundary they exercise.
    [IO.File]::WriteAllText($executable,
        'installed executable fixture RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1 -validationExecutableSha256')
    [IO.File]::WriteAllText($launcher, 'launcher fixture; this test never starts it')
    [IO.File]::WriteAllText($launcherConfig,
        'RUN = . generalszh.exe -simulationMode parallel -workerPolicy auto')
    [IO.File]::WriteAllText($replay, 'replay fixture')
    $manifest = Join-Path $testRoot 'manifest.json'
    Write-SafetyManifest $manifest 'ZeroHour' 'generalszh.exe' (Get-Sha256 $executable) (Get-Sha256 $replay)

    $output = Join-Path $testRoot 'plan-output'
    & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $output `
        -ValidationSet Replay -AllowNonStandardCorpus -PlanOnly -MinimumFreeBytes 1 | Out-Null
    $plan = Get-Content -LiteralPath (Join-Path $output 'validation-plan.json') -Raw |
        ConvertFrom-Json
    $contract = $plan.launcherContract
    $expectedExecutable = [IO.Path]::GetFullPath($executable)
    $expectedRuntime = [IO.Path]::GetFullPath($runtime)
    Assert-True ($contract.mode -ceq 'headless-direct-exception') `
        'the plan must record the reviewed headless direct-execution exception'
    Assert-True ([String]::Equals($contract.launcherTarget, $expectedExecutable,
        [StringComparison]::OrdinalIgnoreCase)) `
        'launcher contract target must match the manifest executable'
    Assert-True ([String]::Equals($contract.directExecutable, $expectedExecutable,
        [StringComparison]::OrdinalIgnoreCase)) `
        'direct contract target must match the launcher target'
    Assert-True ([String]::Equals($contract.launcherWorkingDirectory, $expectedRuntime,
        [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals($contract.directWorkingDirectory, $expectedRuntime,
            [StringComparison]::OrdinalIgnoreCase)) `
        'launcher and direct contracts must use the installed runtime directory'
    Assert-True ((@($contract.launcherArguments) -join '|') -ceq
        '-simulationMode|parallel|-workerPolicy|auto') `
        'launcher defaults must be preserved in the equivalence contract'
    foreach ($environmentName in @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA',
            'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH',
            'RTS_STAGE5_VALIDATION_PROFILE_ROOT', 'RTS_FRAME_TIMING_DIR')) {
        Assert-True (@($contract.environmentVariables) -contains $environmentName) `
            "launcher equivalence must account for child environment variable $environmentName"
    }
    Assert-True ($contract.profileStrategy -ceq 'process-local-validation-profile-root' -and
        $contract.profileEnvironmentVariable -ceq 'RTS_STAGE5_VALIDATION_PROFILE_ROOT' -and
        @($contract.profileRegistryValues).Count -eq 0) `
        'launcher equivalence must account for the process-local complete profile root'
    Assert-True ([bool]$contract.childExitCodeObserved) `
        'the direct exception must record that the child exit code is observed'

    # The native hard-AI producer has a distinct completion scenario and must
    # remain a distinct plan entry.  This uses only the runner's ordinary
    # plan/manifest path; it does not claim that these fixture files are live
    # product evidence or launch an installed runtime.
    $hardAiManifest = Join-Path $testRoot 'hard-ai-2v6-manifest.json'
    Write-SafetyManifest $hardAiManifest 'ZeroHour' 'generalszh.exe' `
        (Get-Sha256 $executable) (Get-Sha256 $replay) `
        -Scenarios @('4v3', '4v2', 'hard-ai-2v6')
    $hardAiOutput = Join-Path $testRoot 'hard-ai-2v6-plan-output'
    & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
        -OutputRoot $hardAiOutput -ValidationSet AI -AllowNonStandardCorpus `
        -PlanOnly -MinimumFreeBytes 1 | Out-Null
    $hardAiPlan = Get-Content -LiteralPath (Join-Path $hardAiOutput 'validation-plan.json') `
        -Raw | ConvertFrom-Json
    $hardAiEntries = @($hardAiPlan.entries | Where-Object {
        $_.kind -ceq 'ai' -and $_.scenario -ceq 'hard-ai-2v6'
    })
    $fourV2Entries = @($hardAiPlan.entries | Where-Object {
        $_.kind -ceq 'ai' -and $_.scenario -ceq '4v2' -and
            $_.configuration -ne 'shadow-16'
    })
    $fourV3Entries = @($hardAiPlan.entries | Where-Object {
        $_.kind -ceq 'ai' -and $_.scenario -ceq '4v3'
    })
    Assert-True ($hardAiEntries.Count -gt 0 -and
        $hardAiEntries.Count -eq $fourV2Entries.Count -and
        $hardAiEntries.Count -eq $fourV3Entries.Count) `
        'AI plan must retain one complete hard-ai-2v6 cross-product beside 4v2 and 4v3.'
    Assert-True (@($hardAiEntries | Where-Object {
        $_.stress -and @($_.arguments) -contains '-runSkirmishAITestHardAI2v6' -and
            @($_.arguments) -notcontains '-runSkirmishAITest4v2' -and
            @($_.arguments) -notcontains '-runSkirmishAITest'
    }).Count -eq $hardAiEntries.Count) `
        'hard-ai-2v6 entries must be marked stress and dispatch the distinct native flag.'
    Assert-True (@($fourV2Entries | Where-Object {
        @($_.arguments) -contains '-runSkirmishAITest4v2' -and
            @($_.arguments) -notcontains '-runSkirmishAITestHardAI2v6'
    }).Count -eq $fourV2Entries.Count) `
        '4v2 entries must retain their existing native dispatch flag.'
    Assert-True (@($fourV3Entries | Where-Object {
        @($_.arguments) -contains '-runSkirmishAITest' -and
            @($_.arguments) -notcontains '-runSkirmishAITest4v2' -and
            @($_.arguments) -notcontains '-runSkirmishAITestHardAI2v6'
    }).Count -eq $fourV3Entries.Count) `
        '4v3 entries must retain the existing ordinary native dispatch flag.'

    # A diagnostic selector must change the real plan-builder output, rather
    # than merely changing a report field. Removing its plan-builder wiring or
    # restoring the automatic shadow entry must fail this test.
    $tokens = $null
    $parseErrors = $null
    $runnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$tokens, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) `
        'runner must parse before its diagnostic worker-scope behavior is tested'
    $requiredFunctions = @(
        'Assert-Condition',
        'Get-WorkerConfigurations',
        'Get-DiagnosticWorkerConfigurations',
        'Get-CollisionShadowConfiguration',
        'New-CommonArguments',
        'ConvertTo-DisplayCommand',
        'Add-PlanEntry',
        'New-ValidationPlan'
    )
    foreach ($functionName in $requiredFunctions) {
        $definition = $runnerAst.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq $functionName
        }, $true)
        Assert-True ($null -ne $definition) `
            "runner must define the real '$functionName' diagnostic planning dependency"
        Invoke-Expression $definition.Extent.Text
    }
    $diagnosticPlanData = [pscustomobject]@{
        schemaVersion = 1
        executableSha256 = ('A' * 64)
        fixtures = @()
        ai = [pscustomobject]@{
            seeds = @(1729)
            scenarios = @('4v3', '4v2', 'hard-ai-2v6')
            repeats = 1
        }
    }
    $diagnosticPlan = @(New-ValidationPlan $diagnosticPlanData 'AI' 2 3 600 1800 `
        $executable (Join-Path $testRoot 'diagnostic-plan-only-in-memory') `
        'LocalCapacity' 'parallel-2')
    Assert-True ($diagnosticPlan.Count -eq 3 -and
        @($diagnosticPlan | Where-Object {
            $_.configuration -cne 'parallel-2' -or $_.simulationMode -cne 'parallel' -or
                $_.requestedWorkers -cne '2'
        }).Count -eq 0) `
        'diagnostic worker selection must retain every AI case only for parallel-2'
    Assert-True (@($diagnosticPlan | Where-Object {
        $_.configuration -match '^shadow-'
    }).Count -eq 0) `
        'diagnostic worker selection must not append a collision-shadow execution'

    # These calls exercise the public runner boundary. Each forbidden context
    # must fail before output setup or any installed executable can start.
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-canonical') `
            -ValidationSet AI -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-2 -PlanOnly -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration.*LocalCapacity' `
        'canonical mode must reject the diagnostic worker selector'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-not-explicit') `
            -ValidationSet AI -CapacityMode LocalCapacity `
            -DiagnosticWorkerConfiguration parallel-2 -PlanOnly -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration.*DiagnosticNonAcceptance' `
        'LocalCapacity alone must not implicitly authorize worker filtering'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-plan-only') `
            -ValidationSet AI -CapacityMode LocalCapacity -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-2 -PlanOnly -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration.*PlanOnly|execution-only' `
        'the execution-only diagnostic worker selector must reject PlanOnly'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-all') `
            -ValidationSet All -CapacityMode LocalCapacity -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-2 -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration.*AI-only' `
        'the diagnostic worker selector must not reduce replay or combined gates'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-export') `
            -TaskRoot $testRoot -CorpusExportRoot 'diagnostic-filter-corpus' `
            -ValidationSet AI -CapacityMode LocalCapacity -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-2 -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration.*CorpusExportRoot|screening.*export' `
        'a filtered diagnostic screen must not become replay-corpus evidence'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-acceptance') `
            -ValidationSet AI -CapacityMode LocalCapacity -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-2 -MinimumFreeBytes 1 `
            -AcceptanceSourceCommit ('a' * 40) `
            -AcceptanceArtifactSetSha256 ('B' * 64) `
            -AcceptanceRuntimeDependencyManifestSha256 ('C' * 64) `
            -AcceptanceRuntimeClosureSha256 ('D' * 64) | Out-Null
    } 'LocalCapacity.*acceptance|DiagnosticWorkerConfiguration.*acceptance' `
        'a filtered diagnostic screen must reject canonical acceptance bindings'
    Assert-RunnerRejects {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $hardAiManifest `
            -OutputRoot (Join-Path $testRoot 'diagnostic-filter-invalid-value') `
            -ValidationSet AI -CapacityMode LocalCapacity -DiagnosticNonAcceptance `
            -DiagnosticWorkerConfiguration parallel-16 -PlanOnly -MinimumFreeBytes 1 |
            Out-Null
    } 'DiagnosticWorkerConfiguration|ValidateSet|validation set' `
        'the parameter binder must reject configurations outside LocalCapacity'

    # Independently parse the fixture LCF and compare its executable/arguments
    # with the contract rather than trusting only the runner's parser.
    $lcf = (Get-Content -LiteralPath $launcherConfig -Raw).Trim()
    $lcfMatch = [regex]::Match($lcf,
        '^RUN = (?<directory>\.) (?<executable>\S+)(?<arguments>.*)$')
    Assert-True ($lcfMatch.Success -and $lcfMatch.Groups['directory'].Value -ceq '.') `
        'fixture launcher LCF must independently parse with a dot working directory'
    $lcfArguments = @($lcfMatch.Groups['arguments'].Value.Trim() -split '\s+' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    Assert-True ((@($lcfArguments) -join '|') -ceq (@($contract.launcherArguments) -join '|')) `
        'runner launcher arguments must equal the independently parsed LCF arguments'

    $badConfig = 'RUN = . generalsv.exe -simulationMode parallel -workerPolicy auto'
    [IO.File]::WriteAllText($launcherConfig, $badConfig)
    $badOutput = Join-Path $testRoot 'bad-plan-output'
    $badRejected = $false
    try {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot $badOutput -ValidationSet Replay -AllowNonStandardCorpus `
            -PlanOnly -MinimumFreeBytes 1 | Out-Null
    }
    catch {
        $badRejected = $_.Exception.Message -match 'does not match'
    }
    Assert-True $badRejected 'a launcher target mismatch must fail before any installed execution'

    [IO.File]::WriteAllText($launcherConfig,
        'RUN = . generalszh.exe -simulationMode parallel -workerPolicy auto')
    $badTaskOutput = Join-Path $testRoot 'bad-task-output'
    $badTaskRejected = $false
    $badTaskMessage = ''
    try {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot $badTaskOutput -ValidationSet Replay -AllowNonStandardCorpus `
            -DiagnosticNonAcceptance -TaskRoot 'C:\Stage5ValidationSafetyNegative' `
            -AllowHeadlessDirectExecution `
            -MinimumFreeBytes 1 | Out-Null
    }
    catch {
        $badTaskMessage = $_.Exception.Message
        $badTaskRejected = $badTaskMessage -match 'explicit task-owned H:'
    }
    Assert-True $badTaskRejected "installed execution must reject a non-H task root before setup: $badTaskMessage"

    Assert-True ($runnerSource -match '\$Title -ceq ''Generals''') `
        'runner must branch explicitly for the Generals title'
    Assert-True ($runnerSource -match 'Command and Conquer Generals Data') `
        'runner must stage the INI-selected Generals profile leaf'
    $personalWriteToken = [regex]::Escape("'Personal' `$documentsRoot")
    Assert-True ($runnerSource -match 'finally\s*\{[\s\S]*Invoke-Stage5RegistryRecovery[\s\S]*if \(\$registryRecoveryRestored\)[\s\S]*Remove-TaskOwnedDirectory' -and
        -not [regex]::IsMatch($runnerSource, $personalWriteToken)) `
        'runner must restore title registry state and leave global Documents untouched'

    Write-Output 'Stage 5 validation runner safety and launcher-equivalence tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
