[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FixtureExecutable,
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:Failures = 0
$script:PriorObservation = $null

function Assert-LifecycleTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-LifecycleTestCase {
    param([string]$Name, [scriptblock]$Action)
    try {
        & $Action
        Write-Output "PASS: $Name"
    }
    catch {
        ++$script:Failures
        Write-Error "FAIL: $Name -- $($_.Exception.Message)" -ErrorAction Continue
    }
}

# Load the actual function bodies, including any private handle reader added to
# the runner. Never execute its top-level runtime/profile/registry workflow.
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
$runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$parseTokens = $null; $parseErrors = $null
$runnerTree = [Management.Automation.Language.Parser]::ParseFile(
    $runnerPath, [ref]$parseTokens, [ref]$parseErrors)
Assert-LifecycleTest ($parseErrors.Count -eq 0) 'PREREQUISITE: the runner must parse.'
foreach ($definition in @($runnerTree.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst]
})) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
$invokeCommand = Get-Command Invoke-ValidationProcess -CommandType Function
if (-not $invokeCommand.Parameters.ContainsKey('LifecycleObservation')) {
    throw 'PREREQUISITE: add the approved optional LifecycleObservation parameter scaffold before running behavioral RED; an absent parameter is not a failing lifecycle assertion.'
}

$fixtureSource = [IO.Path]::GetFullPath($FixtureExecutable)
Assert-LifecycleTest (Test-Path -LiteralPath $fixtureSource -PathType Leaf) `
    'PREREQUISITE: provide the native ValidationProcessLifecycleFixture executable built by MSVC.'
$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
else { throw 'PREREQUISITE: lifecycle tests require an explicit scratch root.' }
$testToken = [Guid]::NewGuid().ToString('N')
$testRoot = Join-Path $scratchParent ('stage5-process-lifecycle-' + $testToken)
$fixtureRuntime = Join-Path $testRoot 'fixture-runtime'
New-Item -ItemType Directory -Path $fixtureRuntime | Out-Null
$fixturePath = Join-Path $fixtureRuntime ('stage5-lifecycle-' + $testToken + '.exe')
Copy-Item -LiteralPath $fixtureSource -Destination $fixturePath
Assert-LifecycleTest ((Get-Sha256 $fixturePath) -ceq (Get-Sha256 $fixtureSource)) `
    'PREREQUISITE: the isolated fixture copy must match the supplied executable.'

# These are transport inputs, not native-receipt acceptance claims. The fixture
# independently prints the actual child nonce and original native FILETIME.
$executionCohortNonce = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
$executionCohortCreatedUtc = [DateTime]::UtcNow.ToString('o')
$manifestData = [pscustomobject]@{
    title = 'ZeroHour'
    executableSha256 = Get-Sha256 $fixturePath
}
$nativeBinding = [ordered]@{
    sourceCommit = ('a' * 40)
    artifactSetSha256 = ('B' * 64)
    runtimeClosure = [ordered]@{
        dependencyManifestSha256 = ('C' * 64)
        closureSha256 = ('D' * 64)
    }
}

function New-LifecycleFixtureEntry {
    param([string]$Name, [int]$SleepMilliseconds = 3000,
        [int]$ExitCode = 0, [int]$TimeoutSeconds = 15)
    $output = Join-Path $testRoot $Name
    return [pscustomobject]@{
        sequence = 1; kind = 'ai'; caseId = $Name; seed = 1729
        arguments = @([string]$SleepMilliseconds, [string]$ExitCode)
        command = 'lifecycle fixture ' + $Name
        timeoutSeconds = $TimeoutSeconds
        stdout = Join-Path $output 'stdout.log'
        stderr = Join-Path $output 'stderr.log'
        timingDirectory = Join-Path $output 'timing'
        runtimeLogDirectory = Join-Path $output 'runtime-logs'
    }
}

function Read-LifecycleFixtureIdentity {
    param([string]$Text)
    $matches = [regex]::Matches($Text,
        '(?m)^STAGE5_LIFECYCLE_FIXTURE schema=1 pid=(?<pid>[0-9]+) creationTime100ns=(?<creation>[0-9]+) sleepMilliseconds=(?<sleep>[0-9]+) exitCode=(?<exit>[0-9]+) runNonce=(?<nonce>[0-9a-f-]{36})\r?$')
    Assert-LifecycleTest ($matches.Count -eq 1) `
        'the actual native child must report exactly one independently captured identity'
    $match = $matches[0]
    return [pscustomobject]@{
        processId = [UInt32]::Parse($match.Groups['pid'].Value, [Globalization.CultureInfo]::InvariantCulture)
        creationTime100ns = [UInt64]::Parse($match.Groups['creation'].Value, [Globalization.CultureInfo]::InvariantCulture)
        runNonce = $match.Groups['nonce'].Value
        requestedExitCode = [int]$match.Groups['exit'].Value
    }
}

function Assert-LifecycleObservationShape {
    param([AllowNull()][object]$Observation)
    Assert-LifecycleTest ($Observation -is [Collections.ObjectModel.ReadOnlyDictionary[string,object]]) `
        'the runner must publish an immutable primitive observation, including after an exception'
    $fields = @('schemaVersion','authority','runNonce','launchAttempted','started',
        'identityStatus','processId','processCreationTime100ns','executablePath',
        'waitForExitSucceeded','exitCodeKnown','exitCode','timedOut',
        'terminationAttempted','cleanupState','bodyCompleted')
    Assert-LifecycleTest ((@($Observation.Keys | Sort-Object) -join '|') -ceq
        (@($fields | Sort-Object) -join '|')) 'the lifecycle snapshot must have exactly the closed schema fields'
    Assert-LifecycleTest ($Observation['schemaVersion'] -is [int] -and
        $Observation['schemaVersion'] -eq 1 -and
        $Observation['authority'] -ceq 'stage5-original-process-handle-v1') `
        'the direct observation must identify its schema and original-handle authority'
    foreach ($field in @('launchAttempted','started','waitForExitSucceeded',
        'exitCodeKnown','timedOut','terminationAttempted','bodyCompleted')) {
        Assert-LifecycleTest ($Observation[$field] -is [bool]) "$field must remain a Boolean"
    }
    foreach ($field in $fields) {
        $value = $Observation[$field]
        Assert-LifecycleTest ($null -eq $value -or $value -is [string] -or
            $value -is [bool] -or $value -is [int] -or $value -is [UInt32] -or
            $value -is [UInt64]) "the observation cannot expose a live handle or mutable value in $field"
    }
}

function Assert-LifecycleExitedIdentity {
    param([object]$Observation, [object]$NativeIdentity, [int]$ExitCode)
    Assert-LifecycleObservationShape $Observation
    Assert-LifecycleTest ($Observation['launchAttempted'] -and $Observation['started'] -and
        $Observation['identityStatus'] -ceq 'verified') 'a started child must have verified original-handle identity'
    Assert-LifecycleTest ($Observation['processId'] -is [UInt32] -and
        $Observation['processId'] -eq $NativeIdentity.processId -and
        $Observation['processId'] -gt 0) 'observed PID must equal the real native child PID'
    Assert-LifecycleTest ($Observation['processCreationTime100ns'] -is [UInt64] -and
        $Observation['processCreationTime100ns'] -eq $NativeIdentity.creationTime100ns -and
        $Observation['processCreationTime100ns'] -gt 0) `
        'creation FILETIME must exactly equal native GetProcessTimes, without timestamp rounding'
    Assert-LifecycleTest ($Observation['runNonce'] -ceq $NativeIdentity.runNonce) `
        'lifecycle observation must retain the existing child nonce rather than generate another identity'
    Assert-LifecycleTest ([String]::Equals($Observation['executablePath'], $fixturePath,
        [StringComparison]::OrdinalIgnoreCase)) 'observed image path must equal the isolated real fixture'
    Assert-LifecycleTest ($Observation['waitForExitSucceeded'] -and
        $Observation['exitCodeKnown'] -and $Observation['exitCode'] -is [int] -and
        $Observation['exitCode'] -eq $ExitCode) 'original WaitForExit and ExitCode evidence must survive finalization'
}

function Invoke-LifecycleReceiptFault {
    param([object]$Entry, [AllowNull()][ref]$Observation,
        [switch]$OmitObservation)
    # The real getter consumes captured stdout after the bounded process wait.
    # Ordinary malformed candidates return null, so inject only its unexpected
    # exception. Start, waits, identity capture, cleanup, and out-value are real.
    function Get-NativePerformanceReceiptReference {
        param([string]$OutputText, [string]$OutputRoot, [string]$WorkingDirectory,
            [string]$Role, [string]$SourceCommit, [string]$ArtifactSetSha256,
            [string]$ExecutableSha256, [string]$RunNonce, [string]$CohortNonce,
            [Collections.IDictionary]$RuntimeClosure, [string]$ExpectedTitle,
            [int]$ProcessId, [string]$ProcessCreationUtc,
            [string]$ExpectedExecutablePath, [string[]]$ExpectedProducers,
            [string]$ExpectedCohortCreatedUtc)
        throw 'lifecycle-fixture-post-wait-receipt-fault'
    }
    $arguments = @{
        Executable = $fixturePath; WorkingDirectory = $fixtureRuntime
        Entry = $Entry; CaptureTiming = $true; Environment = @{}
        EvidenceRoot = $testRoot; NativeObservationBinding = $nativeBinding
    }
    if (-not $OmitObservation) { $arguments.LifecycleObservation = $Observation }
    Invoke-ValidationProcess @arguments
}

# Break caught: a return-only capture, CIM-rounded creation time, mutable backing
# dictionary, or a leaked Process cannot satisfy this actual child comparison.
Invoke-LifecycleTestCase 'normal exit retains exact immutable original-handle evidence' {
    $entry = New-LifecycleFixtureEntry 'normal'
    $observation = $null
    $run = Invoke-ValidationProcess -Executable $fixturePath -WorkingDirectory $fixtureRuntime `
        -Entry $entry -CaptureTiming $false -Environment @{} -LifecycleObservation ([ref]$observation)
    $script:PriorObservation = $observation
    Assert-LifecycleTest ($run.exitCode -eq 0 -and -not $run.timedOut -and
        $run.stderr -match 'STAGE5_LIFECYCLE_FIXTURE stderr=ready') 'the existing successful run result must be preserved'
    $identity = Read-LifecycleFixtureIdentity $run.stdout
    Assert-LifecycleExitedIdentity $observation $identity 0
    Assert-LifecycleTest ($observation['bodyCompleted'] -and -not $observation['timedOut'] -and
        -not $observation['terminationAttempted'] -and $observation['cleanupState'] -ceq 'not-needed') `
        'natural exit must not invent termination, timeout, or failed-body state'
    $mutationRejected = $false
    try { $observation['exitCode'] = 99 } catch { $mutationRejected = $true }
    Assert-LifecycleTest ($mutationRejected -and $observation['exitCode'] -eq 0) `
        'the published original exit code must reject caller mutation'
    $additionRejected = $false
    try { $observation.Add('callerProof', $true) } catch { $additionRejected = $true }
    Assert-LifecycleTest $additionRejected 'the published observation cannot gain caller-attested fields'
}

# Break caught: post-wait errors must not erase the original child exit proof or
# get swallowed merely because the cleanup-safe observation is available.
Invoke-LifecycleTestCase 'post-wait receipt error preserves exit evidence and the original exception' {
    $entry = New-LifecycleFixtureEntry 'receipt-error' -ExitCode 17
    $observation = $null; $caught = $null; $run = $null
    try { $run = Invoke-LifecycleReceiptFault $entry -Observation ([ref]$observation) }
    catch { $caught = $_ }
    Assert-LifecycleTest ($null -ne $caught -and
        $caught.Exception.Message -ceq 'lifecycle-fixture-post-wait-receipt-fault' -and
        $null -eq $run) 'the real post-wait receipt fault must still propagate without a fabricated run'
    $identity = Read-LifecycleFixtureIdentity (Get-Content -LiteralPath $entry.stdout -Raw)
    Assert-LifecycleExitedIdentity $observation $identity 17
    Assert-LifecycleTest (-not $observation['bodyCompleted'] -and -not $observation['timedOut'] -and
        -not $observation['terminationAttempted'] -and $observation['cleanupState'] -ceq 'not-needed') `
        'a receipt exception after natural exit is cleanup-safe but not a completed run body'
}

# Break caught: killed timeouts cannot become natural successes, and HasExited
# alone cannot replace a verified retained-handle wait/exit-code observation.
Invoke-LifecycleTestCase 'timeout cleanup retains verified exit without clearing timeout' {
    $entry = New-LifecycleFixtureEntry 'timeout' -SleepMilliseconds 12000 -ExitCode 23 -TimeoutSeconds 1
    $observation = $null
    $run = Invoke-ValidationProcess -Executable $fixturePath -WorkingDirectory $fixtureRuntime `
        -Entry $entry -CaptureTiming $false -Environment @{} -LifecycleObservation ([ref]$observation)
    Assert-LifecycleTest $run.timedOut 'the slow real child must exceed the primary timeout'
    $identity = Read-LifecycleFixtureIdentity $run.stdout
    Assert-LifecycleExitedIdentity $observation $identity $run.exitCode
    Assert-LifecycleTest ($observation['timedOut'] -and $observation['terminationAttempted'] -and
        $observation['cleanupState'] -ceq 'completed' -and $observation['bodyCompleted']) `
        'bounded timeout cleanup must retain timeout and termination separately from body completion'
}

# Break caught: preflight exceptions before the old try/finally cannot leave a
# previous invocation's observation in the holder or be counted as Start calls.
Invoke-LifecycleTestCase 'V2 preflight rejection clears stale proof and records no attempted Start' {
    $entry = New-LifecycleFixtureEntry 'preflight'
    $entry | Add-Member -NotePropertyName entryId -NotePropertyValue 'ai-0001'
    $prior = if ($null -ne $script:PriorObservation) { $script:PriorObservation } else { 'stale-holder' }
    $observation = $prior; $caught = $null
    try {
        Invoke-ValidationProcess -Executable $fixturePath -WorkingDirectory $fixtureRuntime `
            -Entry $entry -CaptureTiming $false -Environment @{} -LifecycleObservation ([ref]$observation) | Out-Null
    }
    catch { $caught = $_ }
    Assert-LifecycleTest ($null -ne $caught -and $caught.Exception.Message -match 'frozen live plan binding') `
        'advertised V2 metadata without a frozen binding must still fail before process preparation'
    Assert-LifecycleTest (-not (Test-Path -LiteralPath (Split-Path -Parent $entry.stdout))) `
        'the V2 preflight failure must precede output-directory creation'
    Assert-LifecycleObservationShape $observation
    Assert-LifecycleTest (-not [object]::ReferenceEquals($prior, $observation) -and
        -not $observation['launchAttempted'] -and -not $observation['started'] -and
        $observation['identityStatus'] -ceq 'not-started' -and
        $null -eq $observation['runNonce'] -and $null -eq $observation['processId'] -and
        $null -eq $observation['processCreationTime100ns'] -and $null -eq $observation['executablePath'] -and
        -not $observation['waitForExitSucceeded'] -and -not $observation['exitCodeKnown'] -and
        $null -eq $observation['exitCode'] -and -not $observation['timedOut'] -and
        -not $observation['terminationAttempted'] -and -not $observation['bodyCompleted'] -and
        $observation['cleanupState'] -ceq 'not-needed') 'no-attempt observation must be fresh, explicit, and free of stale child facts'
}

# Break caught: an attempted Start that throws is ambiguous, not a no-child
# certificate inferred from a null process lookup or a missing returned run.
Invoke-LifecycleTestCase 'failed actual Start remains unconfirmed rather than no-attempt' {
    $entry = New-LifecycleFixtureEntry 'failed-start'
    $missingExecutable = Join-Path $fixtureRuntime ('absent-' + $testToken + '.exe')
    Assert-LifecycleTest (-not (Test-Path -LiteralPath $missingExecutable)) 'the failed-Start executable must not exist'
    $observation = $null; $caught = $null
    try {
        Invoke-ValidationProcess -Executable $missingExecutable -WorkingDirectory $fixtureRuntime `
            -Entry $entry -CaptureTiming $false -Environment @{} -LifecycleObservation ([ref]$observation) | Out-Null
    }
    catch { $caught = $_ }
    $nativeStartError = if ($null -ne $caught) { $caught.Exception } else { $null }
    while ($null -ne $nativeStartError -and $nativeStartError -isnot [ComponentModel.Win32Exception]) {
        $nativeStartError = $nativeStartError.InnerException
    }
    Assert-LifecycleTest ($nativeStartError -is [ComponentModel.Win32Exception] -and
        $nativeStartError.NativeErrorCode -eq 2 -and
        (Test-Path -LiteralPath (Split-Path -Parent $entry.stdout) -PathType Container)) `
        'the missing executable must fail at the real Start after output preparation'
    Assert-LifecycleObservationShape $observation
    Assert-LifecycleTest ($observation['launchAttempted'] -and -not $observation['started'] -and
        $observation['identityStatus'] -ceq 'unavailable' -and
        $observation['runNonce'] -cmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' -and
        $null -eq $observation['processId'] -and $null -eq $observation['processCreationTime100ns'] -and
        $null -eq $observation['executablePath'] -and -not $observation['waitForExitSucceeded'] -and
        -not $observation['exitCodeKnown'] -and $null -eq $observation['exitCode'] -and
        -not $observation['timedOut'] -and -not $observation['terminationAttempted'] -and
        -not $observation['bodyCompleted'] -and $observation['cleanupState'] -ceq 'unconfirmed') `
        'failed attempted Start cannot acquire no-attempt or known-exit authority'
}

# Break caught: the optional seam must not alter the existing return object or
# require callers to request lifecycle evidence merely to observe process exit.
Invoke-LifecycleTestCase 'omitted observation preserves the existing nonzero-exit return contract' {
    $entry = New-LifecycleFixtureEntry 'omitted-success' -ExitCode 17
    $run = Invoke-ValidationProcess -Executable $fixturePath -WorkingDirectory $fixtureRuntime `
        -Entry $entry -CaptureTiming $false -Environment @{}
    Assert-LifecycleTest ($run.exitCode -eq 17 -and -not $run.timedOut) `
        'omitting observation must still return the actual nonzero child exit code'
    $identity = Read-LifecycleFixtureIdentity $run.stdout
    Assert-LifecycleTest ($identity.requestedExitCode -eq 17 -and $null -ne $run.childProcess -and
        $run.childProcess.processId -eq $identity.processId -and
        $run.childProcess.runNonce -ceq $identity.runNonce) 'the unchanged run must retain its existing child provenance'
    Assert-LifecycleTest ((@($run.PSObject.Properties.Name | Sort-Object) -join '|') -ceq
        (@('timedOut','exitCode','wallMilliseconds','stdout','stderr','runtimeLogText','childProcess' |
            Sort-Object) -join '|')) 'lifecycle observation must not widen the existing returned run shape'
}

Invoke-LifecycleTestCase 'omitted observation preserves the existing post-wait exception contract' {
    $entry = New-LifecycleFixtureEntry 'omitted-receipt-error'
    $caught = $null; $run = $null
    try { $run = Invoke-LifecycleReceiptFault $entry -OmitObservation }
    catch { $caught = $_ }
    Assert-LifecycleTest ($null -ne $caught -and
        $caught.Exception.Message -ceq 'lifecycle-fixture-post-wait-receipt-fault' -and
        $null -eq $run) 'omitting observation must not suppress or replace the existing receipt exception'
    Read-LifecycleFixtureIdentity (Get-Content -LiteralPath $entry.stdout -Raw) | Out-Null
}

# Keep this invocation's isolated executable and logs for parent verification;
# this test never deletes artifacts, reacquires a child by PID, or kills a name.
Write-Output "Lifecycle fixture artifacts: $testRoot"
if ($script:Failures -gt 0) { throw "$($script:Failures) lifecycle regression case(s) failed." }
Write-Output 'All source-connected validation-process lifecycle tests passed.'
