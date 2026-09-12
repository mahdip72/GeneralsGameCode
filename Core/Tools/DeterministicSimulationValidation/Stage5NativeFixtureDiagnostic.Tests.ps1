param([Parameter(Mandatory = $true)][string]$ScratchRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
[IO.Directory]::CreateDirectory($ScratchRoot) | Out-Null
$env:TEMP = Join-Path $ScratchRoot 'Temp'
$env:TMP = $env:TEMP
[IO.Directory]::CreateDirectory($env:TEMP) | Out-Null
Import-Module (Join-Path $PSScriptRoot 'Stage5NativePerformanceFixtureProduction.psm1') -Force
# Reuse only existing fixture builders/assertions, never execute the prior test body.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'Stage5NativePerformanceFixtureProduction.Tests.ps1'),
    [ref]$tokens, [ref]$errors)
foreach ($name in @('Assert-True', 'Assert-ThrowsLike', 'Get-TestSha256',
        'Write-TestJson', 'New-TestReviewedFixture', 'New-TestNativeOutput')) {
    $fn = $ast.Find({ param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -ceq $name }, $true)
    . ([scriptblock]::Create($fn.Extent.Text))
}
Assert-True ($null -ne (Get-Command Read-Stage5NativeFixtureDiagnosticInput -ErrorAction SilentlyContinue)) `
    'A distinct nonaccepting native map diagnostic reader is required.'
$fixture = New-TestReviewedFixture (Join-Path $ScratchRoot 'fixture') 'ZeroHour'
$argsForMap = @{
    Path = $fixture.mapPath; ExpectedSha256 = $fixture.mapSha256
    Title = 'ZeroHour'; ExecutableSha256 = $fixture.executableSha256
    MapKey = 'Maps\Stage5Dense\Stage5Dense.map'; Seed = 1729; FrameBudget = 3600
    ExpectedInitialUnitCount = 1000
}
$inputBinding = Read-Stage5NativeFixtureDiagnosticInput @argsForMap
Assert-True ($inputBinding.evidenceKind -ceq 'stage5-native-fixture-diagnostic-input' -and
    -not $inputBinding.finalAcceptanceClaim -and -not $inputBinding.performanceScalingClaim -and
    -not $inputBinding.kernelQualificationClaim) 'Diagnostic binding must never claim admission.'
$profile = Join-Path $ScratchRoot 'profile'
[IO.Directory]::CreateDirectory($profile) | Out-Null
$replay = Join-Path $profile 'recording.rep'
[IO.File]::WriteAllBytes($replay, (New-Object byte[] 16384))
$output = New-TestNativeOutput $fixture $replay (Get-TestSha256 $replay) -ProcessId 4660
$output = $output.Replace('initial_units=8000', 'initial_units=1000').Replace(
    'peak_units=12000', 'peak_units=1000').Replace(
    'initial_units=1000 peak_units=1500', 'initial_units=125 peak_units=125')
$observation = ConvertFrom-Stage5NativeFixtureObservation -Text $output `
    -MapBinding $inputBinding -ExpectedProcessId 4660 -ProfileRoot $profile
Assert-True ($observation.initialUnitCount -eq 1000 -and $observation.peakUnitCount -eq 1000 -and
    $observation.diagnosticOnly) 'Actual1k observation must parse without dense qualification.'
# The unchanged reviewed input still rejects these same1k counts.
$reviewed = Read-Stage5ReviewedNativeKernelFixture -Path $fixture.manifestPath `
    -ExpectedSha256 (Get-TestSha256 $fixture.manifestPath) -ExpectedTitle 'ZeroHour' `
    -ExpectedSourceCommit ('a' * 40) -ExpectedArtifactSetSha256 ('CC' * 32) `
    -ExpectedExecutableSha256 $fixture.executableSha256 `
    -ExpectedDependencyManifestSha256 ('DD' * 32) -ExpectedRuntimeClosureSha256 ('EE' * 32)
Assert-ThrowsLike { ConvertFrom-Stage5NativePerformanceFixtureOutput -Text $output `
    -ReviewedFixture $reviewed -ExpectedProcessId 4660 -ProfileRoot $profile } 'minimum|workload|dense' `
    'Reviewed dense production must continue rejecting actual1k output.'
Assert-ThrowsLike { Assert-Stage5NativeFixtureProductionCompletion $observation } 'diagnostic' `
    'A diagnostic observation must not be eligible for the dense publisher.'
$diagnosticRepresentations = @(
    [ordered]@{ diagnosticOnly=$true },
    @{ diagnosticOnly=$true },
    ([pscustomobject]@{ diagnosticOnly=$true }),
    ('{"diagnosticOnly":true}' | ConvertFrom-Json),
    ('{"diagnosticOnly":true}' | ConvertFrom-Json -AsHashtable)
)
foreach ($representation in $diagnosticRepresentations) {
    Assert-ThrowsLike { Assert-Stage5NativeFixtureProductionCompletion $representation } 'diagnostic' `
        'Dictionary/JSON diagnostic marker bypassed the production guard.'
}
$publisherArguments = @{}
foreach ($parameter in (Get-Command New-Stage5NativePerformanceFixtureProductionReceipt).ParameterSets[0].Parameters) {
    if (-not $parameter.IsMandatory) { continue }
    $publisherArguments[$parameter.Name] = if ($parameter.ParameterType -eq [Int64]) { [Int64]900 } `
        elseif ($parameter.ParameterType -eq [string]) { 'test-only-not-published' } else { [pscustomobject]@{} }
}
$publisherArguments.Title = 'ZeroHour'
foreach ($representation in $diagnosticRepresentations) {
    $publisherArguments.Completion = $representation
    Assert-ThrowsLike { New-Stage5NativePerformanceFixtureProductionReceipt @publisherArguments } 'diagnostic' `
        'Actual production publisher did not reject the diagnostic before projection/publication.'
}
foreach ($badFlag in @('false', 0, 1, $null)) {
    foreach ($representation in @([ordered]@{diagnosticOnly=$badFlag}, [pscustomobject]@{diagnosticOnly=$badFlag})) {
        Assert-ThrowsLike { Assert-Stage5NativeFixtureProductionCompletion $representation } 'boolean|type' `
            'A nonboolean diagnostic marker was coerced into admission.'
    }
}
# Marker-free historical completion objects and genuine booleanfalse remain
# compatible with this guard; the publisher still validates their full closure.
Assert-Stage5NativeFixtureProductionCompletion ([pscustomobject]@{endFrame=1200})
Assert-Stage5NativeFixtureProductionCompletion ([ordered]@{endFrame=1200})
Assert-Stage5NativeFixtureProductionCompletion ([pscustomobject]@{diagnosticOnly=$false})
Assert-Stage5NativeFixtureProductionCompletion ([ordered]@{diagnosticOnly=$false})
$bad = @{} + $argsForMap; $bad.ExpectedSha256 = 'FF' * 32
Assert-ThrowsLike { Read-Stage5NativeFixtureDiagnosticInput @bad } 'hash|SHA' 'Tampered map accepted.'
$bad = @{} + $argsForMap; $bad.MapKey = '..\foreign.map'
Assert-ThrowsLike { Read-Stage5NativeFixtureDiagnosticInput @bad } 'map|path' 'Escaping map sink accepted.'
Assert-ThrowsLike { ConvertFrom-Stage5NativeFixtureObservation -Text ($output.Replace(
    $fixture.executableSha256, ('FF' * 32))) -MapBinding $inputBinding `
    -ExpectedProcessId 4660 -ProfileRoot $profile } 'identity|executable|binding' 'Foreign executable output accepted.'
Assert-ThrowsLike { ConvertFrom-Stage5NativeFixtureObservation -Text ($output -split "`r`n" | Select-Object -First 10 | Out-String) `
    -MapBinding $inputBinding -ExpectedProcessId 4660 -ProfileRoot $profile } 'cardinality|completion' 'Partial child output accepted.'
Write-Output 'PASS: native fixture diagnostic input/observation/admission isolation'
$runner = Join-Path $PSScriptRoot 'Invoke-Stage5PerformanceScalingValidation.ps1'
$command = Get-Command $runner
Assert-True ($command.Parameters.ContainsKey('RunNativeFixtureDiagnostic')) 'Public diagnostic entrypoint is missing.'
$diagnosticSet = @($command.ParameterSets | Where-Object Name -CEQ 'NativeFixtureDiagnostic')
Assert-True ($diagnosticSet.Count -eq 1) 'Diagnostic parameter set must be distinct.'
$names = @($diagnosticSet[0].Parameters.Name)
Assert-True ($names -contains 'DiagnosticMapPath' -and $names -contains 'ExpectedDiagnosticMapSha256' -and
    $names -notcontains 'ProduceNativeFixture' -and $names -notcontains 'ReviewedFixtureManifestPath') `
    'Diagnostic entrypoint must not consume a reviewed dense manifest.'
$captureRoot = Join-Path $ScratchRoot 'capture'
[IO.Directory]::CreateDirectory((Join-Path $captureRoot 'logs')) | Out-Null
$captured = Write-Stage5NativeFixtureCapturedOutput -TaskRoot $captureRoot `
    -StdoutBytes ([Text.Encoding]::UTF8.GetBytes('bounded child diagnostic')) -StderrBytes $null
Assert-True ($captured.stdoutAvailable -and -not $captured.stderrAvailable -and
    $null -eq $captured.rawLogSnapshot -and
    [IO.File]::ReadAllText((Join-Path $captureRoot 'logs/stdout.log')) -ceq 'bounded child diagnostic' -and
    -not (Test-Path (Join-Path $captureRoot 'logs/stderr.log'))) 'Partial capture was lost or relabelled complete.'
$blockedLifecycle = [pscustomobject]@{ childExitProven=$false; registryRestored=$false; profileRemoved=$false }
$failed = New-Stage5NativeFixtureDiagnosticResult -Status failed -ErrorText 'child exit unproven' `
    -Lifecycle $blockedLifecycle -ExpectedInitialUnitCount 1000
Assert-True ($failed.status -ceq 'failed' -and $null -eq $failed.observation -and
    -not $failed.finalAcceptanceClaim -and -not $failed.kernelQualificationClaim -and
    -not $failed.performanceScalingClaim -and -not $failed.lifecycle.profileRemoved) 'Failure became acceptance or lost cleanup evidence.'
Assert-ThrowsLike { New-Stage5NativeFixtureDiagnosticResult -Status observed -Completion $observation `
    -Lifecycle $blockedLifecycle -ExpectedInitialUnitCount 1000 } 'lifecycle|identity' 'Unproven child published completion.'
$badSink = Join-Path $ScratchRoot 'bad-sink'
[IO.File]::WriteAllText($badSink, 'not a directory')
Assert-ThrowsLike { Write-Stage5NativeFixtureCapturedOutput -TaskRoot $badSink `
    -StdoutBytes ([byte[]]@(1)) -StderrBytes ([byte[]]@(2)) } 'sink|directory|container' 'Bad output sink accepted.'
Write-Output 'PASS: diagnostic parameter set and bounded failure evidence'
