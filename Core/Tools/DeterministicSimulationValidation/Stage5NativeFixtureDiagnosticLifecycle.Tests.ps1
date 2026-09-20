param([Parameter(Mandatory = $true)][string]$ScratchRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2.0
# CTest supplies a shared H: validation scratch parent. Keep every lifecycle
# case below a fresh invocation root so retained case output from an earlier
# run cannot collide with this run. Production still rejects an existing
# output sink; this test-only child is unique and never removes old evidence.
$scratchParent = [IO.Path]::GetFullPath($ScratchRoot).TrimEnd('\')
if (-not $scratchParent.StartsWith('H:\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage 5 native fixture lifecycle scratch must remain on H:.'
}
[IO.Directory]::CreateDirectory($scratchParent)|Out-Null
$scratchParentItem = Get-Item -LiteralPath $scratchParent -Force
if (($scratchParentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Stage 5 native fixture lifecycle scratch parent is a reparse point: $scratchParent"
}
$runRoot = Join-Path $scratchParent ('native-fixture-diagnostic-lifecycle-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot)|Out-Null
$runRootItem = Get-Item -LiteralPath $runRoot -Force
if (($runRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Stage 5 native fixture lifecycle run root is a reparse point: $runRoot"
}
$ScratchRoot = $runRoot
$env:TEMP=Join-Path $ScratchRoot 'Temp';$env:TMP=$env:TEMP
[IO.Directory]::CreateDirectory($env:TEMP)|Out-Null
Import-Module (Join-Path $PSScriptRoot 'Stage5NativePerformanceFixtureProduction.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'Stage5BaseGeneralsBinding.psm1') -Force
# Execute real host lifecycle functions with fake OS/process boundaries only.
foreach($file in @('Invoke-Stage5PerformanceScalingValidation.ps1','Stage5NativePerformanceFixtureProduction.Tests.ps1')) {
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $file),[ref]$tokens,[ref]$errors)
    foreach($fn in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
        . ([scriptblock]::Create($fn.Extent.Text))
    }
}
function Get-Stage5PeMachine { param($Path) return 0x8664 }
function Assert-Stage5ProcessLocalProfileCapability { param($Path,$Context) }
function Get-Stage5LauncherContract { param($Runtime,$Executable) return [pscustomobject]@{} }
function Read-Stage5PerformanceArtifactSet { return [pscustomobject]@{runtimeClosure=[pscustomobject]@{dependencyManifestSha256=('DD'*32);closureSha256=('EE'*32)}} }
function Assert-Stage5PerformanceLauncherBinding {}
function Get-Stage5PerformanceBaseBinding { return $null }
function Get-Stage5HostTopology { return [pscustomobject]@{physicalCoreCount=6;logicalProcessorCount=12} }
function Acquire-Stage5ValidationMutex { return [pscustomobject]@{testOnly=$true} }
function Release-Stage5ValidationMutex {}
function Assert-Stage5NoInstalledTitleProcesses {}
function New-Stage5RegistryRecoveryContext {
    param($Title,$TaskRootPath,$JournalPath,$ExecutionNonce,$SourceCommit,$ArtifactSetSha256,$ExecutablePath,$ExecutableSha256,$RegistryValues)
    [IO.File]::WriteAllText($JournalPath,'{"testOnly":true}')
    return [pscustomobject]@{path=$JournalPath;identity='test';adapter='fake';snapshots=(New-Object 'Collections.Generic.List[object]');snapshotKeys=@{};processIdentities=(New-Object 'Collections.Generic.List[object]')}
}
function Add-Stage5RegistryRecoveryMutation {}
function Add-Stage5RegistryRecoveryPendingProcess { param($Context) $Context.processIdentities.Add([pscustomobject]@{launchPending=$true}) }
function Set-Stage5RegistryRecoveryObservedProcess { param($Context,$Identity) foreach($p in $Context.processIdentities){$p.launchPending=$false} }
function Update-Stage5RegistryRecoveryState {}
function New-Stage5RegistryRecoveryAuthorization { return 'test-only' }
function Invoke-Stage5RegistryRecovery {}
function Test-Stage5StagedMapFileShare {
    param([string]$Path, [bool]$ExpectBlocked)
    Assert-True (-not [string]::IsNullOrWhiteSpace($Path) -and
        (Test-Path -LiteralPath $Path -PathType Leaf)) `
        'Staged map share probe received no existing map path.'
    $probeBytes = New-Object byte[] 1
    $probeBytes[0] = 0x7F
    $writeBlocked = $false
    try { [IO.File]::WriteAllBytes($Path, $probeBytes) }
    catch { $writeBlocked = $true }
    $deleteBlocked = $false
    try { [IO.File]::Delete($Path) }
    catch { $deleteBlocked = $true }
    if ($ExpectBlocked) {
        Assert-True ($writeBlocked -and $deleteBlocked -and
            (Test-Path -LiteralPath $Path -PathType Leaf)) `
            'Staged map write/delete was not blocked while the child ran.'
    }
    else {
        Assert-True ((-not $writeBlocked) -and (-not $deleteBlocked) -and
            (-not (Test-Path -LiteralPath $Path -PathType Leaf))) `
            'Staged map write/delete remained blocked after lock disposal.'
    }
}
function Open-Stage5PerformanceReadOnlyLocks {
    param($ArtifactBinding, $Fixtures, [string]$FixtureManifestPath = '',
        [string[]]$AdditionalPaths = @())
    $script:observedLockFixtureManifestPath = $FixtureManifestPath
    $script:observedLockAdditionalPaths = @($AdditionalPaths)
    $mapLeaf = [IO.Path]::GetFileName($script:fixture.mapPath)
    $stagedCandidates = @($AdditionalPaths | Where-Object {
        [string]::Equals([IO.Path]::GetFileName([string]$_), $mapLeaf,
            [StringComparison]::OrdinalIgnoreCase) -and
        ([string]$_ -like '*\TitleSession\*')
    })
    $script:stagedMapPath = if ($stagedCandidates.Count -eq 1) {
        [IO.Path]::GetFullPath([string]$stagedCandidates[0])
    } else { $null }
    $script:lockObservedStagedMap = ($stagedCandidates.Count -eq 1)
    if ($script:case -eq 'tamper-before-lock' -and $null -ne $script:stagedMapPath) {
        $tamperedBytes = [IO.File]::ReadAllBytes($script:stagedMapPath)
        $tamperedBytes[0] = [byte](($tamperedBytes[0] + 1) % 251)
        [IO.File]::WriteAllBytes($script:stagedMapPath, $tamperedBytes)
        $script:tamperApplied = $true
    }
    $paths = New-Object 'Collections.Generic.List[string]'
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($Fixtures | ForEach-Object { $_.path }) +
        @($FixtureManifestPath) + @($AdditionalPaths)) {
        if ([string]::IsNullOrWhiteSpace([string]$path)) { continue }
        $full = [IO.Path]::GetFullPath([string]$path)
        if ($seen.Add($full)) { $paths.Add($full) | Out-Null }
    }
    $streams = New-Object 'Collections.Generic.List[IO.FileStream]'
    try {
        foreach ($path in $paths) {
            $streams.Add([IO.File]::Open($path, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)) | Out-Null
        }
        return ,$streams.ToArray()
    }
    catch {
        foreach ($stream in $streams) { try { $stream.Dispose() } catch {} }
        throw
    }
}
function Dispose-Stage5PerformanceReadOnlyLocks {
    param([object[]]$Locks)
    foreach ($stream in @($Locks)) {
        if ($null -ne $stream) { $stream.Dispose() }
    }
    if ($null -ne $script:stagedMapPath) {
        Test-Stage5StagedMapFileShare $script:stagedMapPath $false
        $script:mapLockReleasedBeforeProfileCleanup = $true
    }
}
function New-Stage5NativeFixtureProcess {
    param($Info)
    $script:fakeInfo=$Info
    $profile=$Info.EnvironmentVariables['RTS_STAGE5_VALIDATION_PROFILE_ROOT']
    $replay=Join-Path $profile 'Replays/fixture.rep'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $replay))|Out-Null
    [IO.File]::WriteAllBytes($replay,(New-Object byte[] 16384))
    $text=New-TestNativeOutput $script:fixture $replay (Get-TestSha256 $replay) -ProcessId 900
    $text=$text.Replace('initial_units=8000','initial_units=1000').Replace('peak_units=12000','peak_units=1000').Replace('initial_units=1000 peak_units=1500','initial_units=125 peak_units=125')
    $script:fakeStdout=[Text.Encoding]::UTF8.GetBytes($text)
    $p=[pscustomobject]@{StartInfo=$Info;ExitCode=$(if($script:case -eq 'exit-failure'){1}else{0});StandardOutput=[pscustomobject]@{BaseStream='stdout'};StandardError=[pscustomobject]@{BaseStream='stderr'}}
    $p|Add-Member ScriptMethod Start {
        Assert-True $script:lockObservedStagedMap `
            'The staged map was not supplied to the read-only lock helper.'
        Test-Stage5StagedMapFileShare $script:stagedMapPath $true
        $script:childMapShareBlocked = $true
        $script:fakeProcessStarted = $true
        return $true
    }
    $p|Add-Member ScriptMethod WaitForExit {param($Milliseconds) return $true}
    $p|Add-Member ScriptMethod Dispose {}
    return $p
}
function Get-Stage5ProcessIdentity {
    return [pscustomobject]@{processId=900;creationTimeUtc100ns=133000000000000000;executablePath=$(if($script:case -eq 'foreign-child'){'H:\foreign.exe'}else{$script:exe});executableSha256=$script:fixture.executableSha256;commandLine=('"'+$script:exe+'" '+$script:fakeInfo.Arguments)}
}
function Start-Stage5BoundedOutputCapture {
    param($Stream,$Limit,$Context)
    if($script:case -eq 'capture-failure' -and $Stream -eq 'stderr'){throw 'test stderr capture failure'}
    $captureBytes = [byte[]]@()
    if ($Stream -eq 'stdout') { $captureBytes = $script:fakeStdout }
    $capture=[pscustomobject]@{IsFaulted=$false;Bytes=$captureBytes}
    $capture|Add-Member ScriptMethod Wait {param($Milliseconds)return $true}
    $capture|Add-Member ScriptMethod GetAwaiter {return $this}
    $capture|Add-Member ScriptMethod GetResult {return ,$this.Bytes}
    return $capture
}
function Invoke-Stage5OwnedProcessCleanup {
    return [pscustomobject]@{processId=900;exitProof=($script:case -ne 'foreign-child');blocked=($script:case -eq 'foreign-child');errors=@()}
}
function New-Stage5NativePerformanceFixtureProductionReceipt { throw 'DIAGNOSTIC REACHED DENSE PUBLISHER' }
$ProduceNativeFixture=$false;$RunNativeFixtureDiagnostic=$true;$AllowHeadlessDirectExecution=$true
$DiagnosticSeed=1729;$DiagnosticFrameBudget=3600;$DiagnosticExpectedInitialUnits=1000
$script:fixture=New-TestReviewedFixture (Join-Path $ScratchRoot 'input') 'ZeroHour'
$runtime=Join-Path $ScratchRoot 'installed'
[IO.Directory]::CreateDirectory($runtime)|Out-Null
$script:exe=Join-Path $runtime 'generalszh.exe'
[IO.File]::WriteAllBytes($script:exe,[Text.Encoding]::UTF8.GetBytes('test-only fake executable; never executed'))
$script:fixture.executableSha256=Get-TestSha256 $script:exe
$DiagnosticMapPath=$script:fixture.mapPath;$ExpectedDiagnosticMapSha256=$script:fixture.mapSha256
$DiagnosticMapKey='Maps\Stage5Dense\Stage5Dense.map'
$testRepositoryRoot = (& git -C $PSScriptRoot rev-parse --show-toplevel 2>$null |
    Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace([string]$testRepositoryRoot)) {
    throw 'The native-fixture lifecycle harness must run from a Git checkout.'
}
# The lifecycle harness imports runner functions from their AST.  Those
# dynamically defined functions do not retain the runner's $PSScriptRoot, so
# make their Git provenance lookup deterministic without weakening the
# production source-commit check.
Push-Location ([string]$testRepositoryRoot).Trim()
try {
    $sourceCommit = (& git -C $PSScriptRoot rev-parse HEAD).Trim()
    foreach($script:case in @('success','exit-failure','capture-failure','foreign-child','tamper-before-lock')) {
    $out=Join-Path $ScratchRoot $script:case
    $script:lockObservedStagedMap=$false;$script:stagedMapPath=$null
    $script:observedLockFixtureManifestPath=$null;$script:observedLockAdditionalPaths=@()
    $script:childMapShareBlocked=$false;$script:mapLockReleasedBeforeProfileCleanup=$false
    $script:fakeProcessStarted=$false;$script:tamperApplied=$false
    $caught=$null
    try {
        Invoke-Stage5NativePerformanceFixtureProduction -Diagnostic -FixtureTitle ZeroHour `
            -ExecutablePath $script:exe -ExecutableSha256 $script:fixture.executableSha256 `
            -SourceCommit $sourceCommit -ArtifactSetSha256 ('CC'*32) -ArtifactManifestPath 'test-only' `
            -CohortNonce '12345678-1234-4234-8234-123456789abc' `
            -CohortCreatedUtc '2026-09-12T12:00:00.0000000Z' -OutputRoot $out -Timeout 1 | Out-Null
    }catch{$caught=$_}
    $resultPath=Join-Path $out 'Stage5NativeFixtureDiagnostic.json'
    Assert-True (Test-Path $resultPath) "Missing diagnostic result for $script:case; error=$caught"
    $result=Get-Content $resultPath -Raw|ConvertFrom-Json
    Assert-True (-not $result.finalAcceptanceClaim -and -not $result.performanceScalingClaim -and -not $result.kernelQualificationClaim) 'Diagnostic made acceptance claims.'
    Assert-True (-not(Test-Path (Join-Path $out 'Stage5NativePerformanceFixture.json'))) 'Diagnostic published dense receipt.'
    Assert-True (Test-Path (Join-Path $out 'inputs/Stage5Dense.map')) 'Input map was lost.'
    $hasObservedExitEvidence = $result.PSObject.Properties.Name -contains 'exitCode' -and
        $result.PSObject.Properties.Name -contains 'identityBoundExitProven'
    if($script:case -eq 'tamper-before-lock') {
        Assert-True ($null -ne $caught -and $result.status -eq 'failed' -and
            $script:tamperApplied -and $script:lockObservedStagedMap -and
            (-not $script:fakeProcessStarted) -and
            ([string]$result.error -match 'staged map SHA-256')) `
            'A staged-map tamper after the pre-lock hash was allowed to launch the child.'
        continue
    }
    if($script:case -eq 'success') {
        Assert-True ($null -eq $caught -and $result.status -eq 'observed' -and $result.expectedPopulationObserved -and
            $hasObservedExitEvidence -and (Test-Stage5JsonInteger $result.exitCode) -and
            $result.exitCode -eq 0 -and $result.identityBoundExitProven -and
            $result.lifecycle.childExitProven) "Successful diagnostic failed: $caught"
        Assert-True (Test-Path (Join-Path $out 'replays/Stage5Performance.rep')) 'Replay was lost during cleanup.'
        Assert-True (-not(Test-Path (Join-Path $out 'TitleSession'))) 'Completed profile not cleaned.'
        Assert-True ($script:childMapShareBlocked -and
            $script:mapLockReleasedBeforeProfileCleanup) `
            'Staged map lock lifetime was not proven around child/profile cleanup.'
    }else{
        Assert-True ($null -ne $caught -and $result.status -eq 'failed' -and $null -eq $result.observation) "Failure became success: $script:case"
        if($script:case -eq 'exit-failure') {
            Assert-True ($hasObservedExitEvidence -and (Test-Stage5JsonInteger $result.exitCode) -and
                $result.exitCode -eq 1 -and $result.identityBoundExitProven -and
                -not $result.lifecycle.childExitProven) `
                'Known nonzero child exit was not retained separately from clean-exit proof.'
        }
        elseif($script:case -eq 'capture-failure') {
            Assert-True ($hasObservedExitEvidence -and $null -eq $result.exitCode -and
                $result.identityBoundExitProven -and -not $result.lifecycle.childExitProven) `
                'Capture failure must preserve an unknown exit code without inventing zero.'
        }
        elseif($script:case -eq 'foreign-child') {
            Assert-True ($hasObservedExitEvidence -and $null -eq $result.exitCode -and
                -not $result.identityBoundExitProven -and -not $result.lifecycle.childExitProven) `
                'Foreign child identity must preserve unknown/unproven exit evidence.'
        }
        if($script:case -eq 'foreign-child') {
            Assert-True ((Test-Path (Join-Path $out 'TitleSession')) -and (Test-Path (Join-Path $out 'Stage5FixtureRegistryRecovery.json'))) 'Unproven child lost profile/journal.'
        }else{
            Assert-True (Test-Path (Join-Path $out 'logs/stdout.log')) 'Failed child stdout was not retained.'
            Assert-True (Test-Path (Join-Path $out 'TitleSession')) 'Failed diagnostic lost its partial recording/profile.'
            Assert-True ($result.capturedOutput.stdoutAvailable -and
                ($result.capturedOutput.stderrAvailable -eq ($script:case -ne 'capture-failure'))) 'Capture availability was not recorded accurately.'
        }
    }
    }
    Write-Output 'PASS: real native-fixture host lifecycle with bounded fake child/registry adapters'
}
finally {
    Pop-Location
}
