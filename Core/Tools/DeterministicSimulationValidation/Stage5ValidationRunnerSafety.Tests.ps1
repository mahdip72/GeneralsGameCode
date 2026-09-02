param(
    [string]$ScratchRoot = ''
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
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
        [string]$ReplayHash)
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
            seeds = @(1729)
            scenarios = @('4v3')
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
    [IO.File]::WriteAllText($executable, 'installed executable fixture')
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
            'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH', 'RTS_FRAME_TIMING_DIR')) {
        Assert-True (@($contract.environmentVariables) -contains $environmentName) `
            "launcher equivalence must account for child environment variable $environmentName"
    }
    Assert-True ($contract.profileStrategy -ceq 'known-folder-registry-redirect' -and
        @($contract.profileRegistryValues).Count -eq 2) `
        'launcher equivalence must account for the title-independent Documents redirect'
    Assert-True ([bool]$contract.childExitCodeObserved) `
        'the direct exception must record that the child exit code is observed'

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
    Assert-True ($runnerSource -match 'finally\s*\{[\s\S]*Invoke-Stage5RegistryRestore[\s\S]*Remove-TaskOwnedDirectory') `
        'runner finally must restore registry state before removing task scratch'

    Write-Output 'Stage 5 validation runner safety and launcher-equivalence tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
