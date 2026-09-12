param([Parameter(Mandatory = $true)][string]$ScratchRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2.0
[IO.Directory]::CreateDirectory($ScratchRoot)|Out-Null
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
function Open-Stage5PerformanceReadOnlyLocks { return $null }
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
    $p|Add-Member ScriptMethod Start {return $true}
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
$sourceCommit = (& git -C $PSScriptRoot rev-parse HEAD).Trim()
foreach($script:case in @('success','exit-failure','capture-failure','foreign-child')) {
    $out=Join-Path $ScratchRoot $script:case
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
    if($script:case -eq 'success') {
        Assert-True ($null -eq $caught -and $result.status -eq 'observed' -and $result.expectedPopulationObserved) "Successful diagnostic failed: $caught"
        Assert-True (Test-Path (Join-Path $out 'replays/Stage5Performance.rep')) 'Replay was lost during cleanup.'
        Assert-True (-not(Test-Path (Join-Path $out 'TitleSession'))) 'Completed profile not cleaned.'
    }else{
        Assert-True ($null -ne $caught -and $result.status -eq 'failed' -and $null -eq $result.observation) "Failure became success: $script:case"
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
