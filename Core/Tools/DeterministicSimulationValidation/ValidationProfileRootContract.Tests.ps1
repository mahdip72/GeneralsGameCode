param(
    [string]$ScratchRoot = ''
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$lockstepPath = Join-Path $PSScriptRoot 'Stage5InstalledLockstepV2Session.psm1'
$scalingPath = Join-Path $PSScriptRoot 'Invoke-Stage5PerformanceScalingValidation.ps1'
$capabilityModulePath = Join-Path $PSScriptRoot 'Stage5ValidationProfileCapability.psm1'
$runnerSource = Get-Content -LiteralPath $runnerPath -Raw
$lockstepSource = Get-Content -LiteralPath $lockstepPath -Raw
$scalingSource = Get-Content -LiteralPath $scalingPath -Raw
$capabilityModuleSource = Get-Content -LiteralPath $capabilityModulePath -Raw
$generalsGlobalDataPath = Join-Path $PSScriptRoot '..\..\..\Generals\Code\GameEngine\Source\Common\GlobalData.cpp'
$zeroHourGlobalDataPath = Join-Path $PSScriptRoot '..\..\..\GeneralsMD\Code\GameEngine\Source\Common\GlobalData.cpp'
$profileRootHeaderPath = Join-Path $PSScriptRoot '..\..\..\Core\Libraries\Include\Lib\ValidationProfileRoot.h'
$generalsGlobalData = Get-Content -LiteralPath $generalsGlobalDataPath -Raw
$zeroHourGlobalData = Get-Content -LiteralPath $zeroHourGlobalDataPath -Raw
$profileRootHeader = Get-Content -LiteralPath $profileRootHeaderPath -Raw

Assert-True ($runnerSource -match 'RTS_STAGE5_VALIDATION_PROFILE_ROOT') `
    'runner must publish the process-local profile-root environment variable'
Assert-True ($runnerSource -match "profileStrategy = 'process-local-validation-profile-root'") `
    'runner launcher contract must identify the process-local profile strategy'
Assert-True ($runnerSource -match 'profileRoot =') `
    'runner launcher contract must bind the complete profile root'
Assert-True ($runnerSource -match 'profileRegistryValues = @\(\)') `
    'runner launcher contract must prove no Documents registry values are required'
$personalWriteToken = [regex]::Escape("'Personal' `$documentsRoot")
Assert-True (-not [regex]::IsMatch($runnerSource, $personalWriteToken)) `
    'runner must not mutate either global Documents Personal registry value'
Assert-True ($runnerSource -notmatch "Set-PreservedRegistryValue[\s\S]{0,260}'UserDataLeafName'") `
    'runner must not mutate the Zero Hour profile leaf registry value'
Assert-True ($runnerSource -match "environmentVariables = @\([\s\S]*'RTS_STAGE5_VALIDATION_PROFILE_ROOT'") `
    'runner contract must include the process-local profile-root environment variable'
Assert-True ($runnerSource -match 'Stage5ValidationProfileCapability\.psm1' -and
    $runnerSource -match 'Assert-Stage5ProcessLocalProfileCapability' -and
    $runnerSource -match 'Stage5RegistryRecovery\.psm1' -and
    $runnerSource -match 'New-Stage5RegistryRecoveryJournal' -and
    $runnerSource -match 'Invoke-Stage5RegistryRecovery') `
    'runner must consume shared profile capability and registry recovery modules'
Assert-True ($lockstepSource -match 'Stage5ValidationProfileCapability\.psm1' -and
    $lockstepSource -match 'Assert-Stage5ProcessLocalProfileCapability' -and
    $lockstepSource -match 'Stage5RegistryRecovery\.psm1' -and
    $lockstepSource -match 'New-Stage5RegistryRecoveryJournal' -and
    $lockstepSource -match 'Invoke-Stage5RegistryRecovery' -and
    $lockstepSource -match "profileStrategy = 'process-local-validation-profile-root'") `
    'lockstep runner must consume the shared capability module and profile strategy'
Assert-True ($lockstepSource -notmatch 'Personal' -and
    $lockstepSource -notmatch 'UserDataLeafName') `
    'lockstep runner must not mutate global Documents or profile-leaf registry values'
Assert-True ($scalingSource -match 'Stage5ValidationProfileCapability\.psm1' -and
    $scalingSource -match 'Assert-Stage5ProcessLocalProfileCapability') `
    'scaling runner must consume the shared capability module'
Assert-True ($scalingSource -notmatch "Set-Stage5RegistryValue[\s\S]{0,260}'Personal'" -and
    $scalingSource -notmatch "Set-Stage5RegistryValue[\s\S]{0,260}'UserDataLeafName'") `
    'scaling runner must not mutate global Documents or profile-leaf registry values'
$scalingTokens = $null
$scalingParseErrors = $null
$scalingAst = [Management.Automation.Language.Parser]::ParseFile(
    $scalingPath, [ref]$scalingTokens, [ref]$scalingParseErrors)
Assert-True (@($scalingParseErrors).Count -eq 0) `
    'scaling runner must parse before capability-order inspection'
$scalingFunctions = @($scalingAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true))
$scalingCommands = @($scalingAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst]
}, $true))
$scalingCapabilityCommands = @($scalingCommands | Where-Object {
    $_.GetCommandName() -ceq 'Assert-Stage5ProcessLocalProfileCapability'
})
$scalingMutationCommands = @($scalingCommands | Where-Object {
    $_.GetCommandName() -ceq 'Set-Stage5RegistryValue'
})
Assert-True ($scalingCapabilityCommands.Count -gt 0 -and
    $scalingMutationCommands.Count -gt 0) `
    'scaling runner must expose both capability and installed-session mutation commands'
function Get-ScalingCommandScopeKey {
    param([object]$CommandAst)
    $scope = @($scalingFunctions | Where-Object {
        $_.Extent.StartOffset -lt $CommandAst.Extent.StartOffset -and
        $_.Extent.EndOffset -gt $CommandAst.Extent.EndOffset
    } | Sort-Object {
        $_.Extent.EndOffset - $_.Extent.StartOffset
    } | Select-Object -First 1)
    if ($scope.Count -eq 0) { return '__main__' }
    return [string]$scope[0].Name
}
$capabilityByScope = @{}
foreach ($command in $scalingCapabilityCommands) {
    $key = Get-ScalingCommandScopeKey $command
    if (-not $capabilityByScope.ContainsKey($key)) { $capabilityByScope[$key] = @() }
    $capabilityByScope[$key] += $command
}
foreach ($mutation in $scalingMutationCommands) {
    $key = Get-ScalingCommandScopeKey $mutation
    $capabilityCommands = @($capabilityByScope[$key])
    Assert-True ($capabilityCommands.Count -gt 0 -and
        (@($capabilityCommands | Where-Object {
            $_.Extent.StartOffset -lt $mutation.Extent.StartOffset
        }).Count -gt 0)) `
        "scaling capability must precede $($mutation.GetCommandName()) in scope '$key'"
}
Assert-True ($capabilityModuleSource -match 'Test-Stage5AsciiMarkerInFile' -and
    $capabilityModuleSource -match 'Assert-Stage5ProcessLocalProfileCapability' -and
    $capabilityModuleSource -match 'StringComparison\]::Ordinal') `
    'shared capability module must expose the bounded streaming marker scan'
$capabilityOffset = $runnerSource.IndexOf(
    'Assert-Stage5ProcessLocalProfileCapability $executableFull', [StringComparison]::Ordinal)
$outputOffset = $runnerSource.IndexOf(
    '$outputFull = [IO.Path]::GetFullPath($OutputRoot)', [StringComparison]::Ordinal)
Assert-True ($capabilityOffset -ge 0 -and $outputOffset -gt $capabilityOffset) `
    'profile capability must be checked before output/task/profile setup'

foreach ($source in @($generalsGlobalData, $zeroHourGlobalData)) {
    Assert-True ($source -match 'ReadProcessLocalProfileRoot') `
        'both title GlobalData implementations must consume the shared process-local override'
    Assert-True ($source -match 'PROCESS_LOCAL_PROFILE_ROOT_INVALID[\s\S]*?refusing live Documents fallback[\s\S]*?exit\(1\)') `
        'an invalid present override must fail closed before known-folder fallback'
}
Assert-True ($profileRootHeader -match 'PROCESS_LOCAL_PROFILE_ROOT_NOT_SET' -and
    $profileRootHeader -match 'PROCESS_LOCAL_PROFILE_ROOT_INVALID' -and
    $profileRootHeader -match 'RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1' -and
    $profileRootHeader -match 'FILE_ATTRIBUTE_REPARSE_POINT' -and
    $profileRootHeader -match 'segmentLength == 2') `
    'shared profile-root helper must distinguish absent/invalid values and reject unsafe paths'

function Get-TestFunctionDefinitions {
    param([string]$Path, [string[]]$Names)
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) "could not parse $Path"
    $definitions = @{}
    foreach ($name in $Names) {
        $definition = $ast.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq $name
        }, $true)
        Assert-True ($null -ne $definition) "$Path is missing function '$name'"
        $definitions[$name] = $definition.Extent.Text
    }
    return $definitions
}

function Install-TestFunctionDefinitions {
    param([hashtable]$Definitions, [string[]]$Order)
    foreach ($name in $Order) {
        . ([scriptblock]::Create($Definitions[$name]))
    }
}

$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
else {
    throw 'ValidationProfileRootContract.Tests.ps1 requires an explicit scratch root.'
}
$testRoot = Join-Path $scratchParent ('validation-profile-root-contract-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    Import-Module $capabilityModulePath -Force
    $markerFixture = Join-Path $testRoot 'capability-marker-fixture.bin'
    $capabilityMarker = 'RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1'
    $parserMarker = '-validationExecutableSha256'
    # The capability starts in one 64 KiB read and ends in the next.
    [IO.File]::WriteAllBytes($markerFixture, [Text.Encoding]::ASCII.GetBytes(
        ('X' * 65530) + $capabilityMarker + [char]0 + $parserMarker))
    Assert-True (Test-Stage5AsciiMarkerInFile $markerFixture $capabilityMarker) `
        'capability scanning must retain a marker across the read boundary'
    Assert-True (-not (Test-Stage5AsciiMarkerInFile $markerFixture `
        $capabilityMarker.ToLowerInvariant())) `
        'capability marker matching must remain ordinal and case-sensitive'
    Assert-Stage5ProcessLocalProfileCapability $markerFixture | Out-Null
    [IO.File]::WriteAllBytes($markerFixture, [Text.Encoding]::ASCII.GetBytes(
        ('X' * 65530) + $capabilityMarker.Substring(0, $capabilityMarker.Length - 1)))
    Assert-True (-not (Test-Stage5AsciiMarkerInFile $markerFixture $capabilityMarker)) `
        'an incomplete marker at end of file must not advertise capability'

    $scalingDefinitions = Get-TestFunctionDefinitions $scalingPath @(
        'Assert-Stage5PerformanceCondition', 'Test-Stage5SafeTitleSessionPath',
        'New-Stage5TitleSessionContract')
    . Install-TestFunctionDefinitions $scalingDefinitions @(
        'Assert-Stage5PerformanceCondition', 'Test-Stage5SafeTitleSessionPath',
        'New-Stage5TitleSessionContract')
    $contractTaskRoot = 'H:\Stage5ProfileContract\TaskRoot'
    $contractRuntime = 'H:\Stage5ProfileContract\Runtime'
    foreach ($title in @('Generals', 'ZeroHour')) {
        $scalingContract = New-Stage5TitleSessionContract $title `
            (Join-Path $contractTaskRoot 'ScalingTitleSession') `
            $contractRuntime $contractTaskRoot
        $expectedProfileLeaf = if ($title -ceq 'Generals') {
            'Command and Conquer Generals Data'
        } else { 'GGC-LockstepV2-ZeroHour' }
        Assert-True ($scalingContract.environmentValues.RTS_STAGE5_VALIDATION_PROFILE_ROOT -ceq
            $scalingContract.profileRoot -and
            [IO.Path]::GetFileName($scalingContract.profileRoot) -ceq $expectedProfileLeaf -and
            @($scalingContract.registryValues).Count -eq 1 -and
            @($scalingContract.registryValues |
                Where-Object { $_.name -cne 'InstallPath' }).Count -eq 0) `
            "scaling title-session contract is not process-local and InstallPath-only for $title"
    }

    $lockstepDefinitions = Get-TestFunctionDefinitions $lockstepPath @(
        'Test-SafeHDirectory', 'New-LockstepTitleSessionContract')
    . Install-TestFunctionDefinitions $lockstepDefinitions @(
        'Test-SafeHDirectory', 'New-LockstepTitleSessionContract')
    foreach ($title in @('Generals', 'ZeroHour')) {
        $lockstepContract = New-LockstepTitleSessionContract $title `
            (Join-Path $contractTaskRoot 'LockstepTitleSession') $contractRuntime
        $expectedProfileLeaf = if ($title -ceq 'Generals') {
            'Command and Conquer Generals Data'
        } else { 'GGC-LockstepV2-ZeroHour' }
        Assert-True ($lockstepContract.environmentValues.RTS_STAGE5_VALIDATION_PROFILE_ROOT -ceq
            $lockstepContract.profileRoot -and
            [IO.Path]::GetFileName($lockstepContract.profileRoot) -ceq $expectedProfileLeaf -and
            @($lockstepContract.registryValues).Count -eq 1 -and
            @($lockstepContract.registryValues |
                Where-Object { $_.name -cne 'InstallPath' }).Count -eq 0) `
            "lockstep title-session contract is not process-local and InstallPath-only for $title"
    }

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
    $manifestObject = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        fixtures = @([ordered]@{
            id = 'reference'; source = 'fixtures\reference.rep'
            sha256 = (Get-FileHash -LiteralPath $replay -Algorithm SHA256).Hash
            stress = $false; maps = @()
        })
        ai = [ordered]@{ seeds = @(1729); scenarios = @('4v3'); repeats = 1 }
    }
    $manifestObject | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest
    $rejectedOutput = Join-Path $testRoot 'unsupported-execution-output'
    $rejectionMessage = ''
    try {
        & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot $rejectedOutput -ValidationSet Replay -AllowNonStandardCorpus `
            -TaskRoot $testRoot -AllowHeadlessDirectExecution -DiagnosticNonAcceptance -MinimumFreeBytes 1 |
            Out-Null
    }
    catch { $rejectionMessage = $_.Exception.Message }
    Assert-True ($rejectionMessage -match 'capability marker|unsupported validation binary') `
        "marker-free execution must fail before launching or creating validation output (actual: $rejectionMessage)"
    Assert-True (-not (Test-Path -LiteralPath $rejectedOutput)) `
        'unsupported marker-free execution must not create output evidence'
    $output = Join-Path $testRoot 'plan-output'
    & $runnerPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
        -OutputRoot $output -ValidationSet Replay -AllowNonStandardCorpus `
        -PlanOnly -MinimumFreeBytes 1 | Out-Null
    $plan = Get-Content -LiteralPath (Join-Path $output 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True ($plan.launcherContract.profileStrategy -ceq
        'process-local-validation-profile-root') `
        'plan contract did not retain the process-local profile strategy'
    Assert-True (@($plan.launcherContract.environmentVariables) -contains
        'RTS_STAGE5_VALIDATION_PROFILE_ROOT') `
        'plan contract did not retain the process-local profile-root environment boundary'

    Write-Output 'Stage 5 process-local validation profile-root contract passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
