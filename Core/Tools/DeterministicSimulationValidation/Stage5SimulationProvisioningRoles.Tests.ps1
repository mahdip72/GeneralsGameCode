[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$ScratchRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2.0
function Check([bool]$value,[string]$message) { if (-not $value) { throw $message } }
function Reject([scriptblock]$action,[string]$message) { $caught=$false; try { & $action | Out-Null } catch { $caught=$true }; Check $caught $message }
$root=Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('provisioning-roles-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$runtimeRoot=Join-Path $root 'Runtime'
[IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
$sink=Join-Path $root 'github.env'
[IO.File]::WriteAllText($sink,'EXISTING=value')
$baseEnvironmentFile=Join-Path $root 'other.env'
[IO.File]::WriteAllText($baseEnvironmentFile,'UNRELATED=value')
$originalEnvironment=@{}
foreach($name in @('GITHUB_ENV','GITHUB_WORKSPACE','GITHUB_SHA','STAGE5_GENERALS_RUNTIME_ROOT','STAGE5_ZEROHOUR_RUNTIME_ROOT','STAGE5_QUALIFICATION_ROOT','STAGE5_PERFORMANCE_DATA_TASK_ROOT','AWS_ENDPOINT_URL','STAGE5_BASE_CONSUMERS_STATUS','STAGE5_BASE_SIMULATION_CLEANUP_OWNER','STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH','STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256')) {
    $originalEnvironment[$name]=[Environment]::GetEnvironmentVariable($name)
}
$producerPath=Join-Path $PSScriptRoot 'Install-Stage5SimulationQualificationData.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($producerPath,[ref]$null,[ref]$null)
foreach($function in $ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst]}) { . ([scriptblock]::Create($function.Extent.Text)) }
$preflight=@(); $collect=$false
foreach($statement in $ast.EndBlock.Statements) {
    if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and $statement.Left.Extent.Text -ceq '$runtimeFull') { $collect=$true }
    if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and $statement.Left.Extent.Text -ceq '$awsExecutable') { break }
    if($collect) { $preflight += $statement.Extent.Text }
}
Check ($preflight.Count -gt 0) 'Producer preflight was not found.'
$script:producerPreflight=[scriptblock]::Create($preflight -join "`n")
$script:canonicalExists=$false
$script:childExists=$false
# Virtualize only the reserved task namespace. No real canonical directories
# are created or removed; runtime freshness and the environment sink use files.
function Test-Path {
    param([string]$LiteralPath,[string]$PathType)
    if($LiteralPath.TrimEnd('\') -ceq 'H:\Stage5SimulationValidationTask') { return $script:canonicalExists }
    if($LiteralPath.StartsWith('H:\Stage5SimulationValidationTask\')) { return $script:childExists }
    $args=@{LiteralPath=$LiteralPath}; if($PathType){$args.PathType=$PathType}
    Microsoft.PowerShell.Management\Test-Path @args
}
function Get-Item {
    param([string]$LiteralPath,[switch]$Force,[string]$ErrorAction)
    if($LiteralPath.StartsWith('H:\Stage5SimulationValidationTask')) { return Microsoft.PowerShell.Management\Get-Item -LiteralPath $root }
    Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath -Force:$Force
}
function Invoke-ProducerPreflight {
    param($RuntimeRoot,$TaskRoot,$SourceCommit,$Title,$AwsEndpointUrl,$OutputEnvironmentFile,$ProvisioningRole='Primary')
    . $script:producerPreflight
    $stream=Open-Stage5SimulationEnvironmentFile $OutputEnvironmentFile
    try {
        Write-Stage5SimulationEnvironmentBindings $stream ([ordered]@{
            STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH=(Join-Path $taskFull 'QualificationData.json')
        }) -ProvisioningRole $ProvisioningRole
    } finally { $stream.Dispose() }
    return $taskFull
}
try {
    $env:GITHUB_ENV=$sink; $env:GITHUB_WORKSPACE=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
    $env:GITHUB_SHA='a'*40; $env:STAGE5_GENERALS_RUNTIME_ROOT=$runtimeRoot; $env:STAGE5_ZEROHOUR_RUNTIME_ROOT=$runtimeRoot
    $env:STAGE5_QUALIFICATION_ROOT=$root; $env:STAGE5_PERFORMANCE_DATA_TASK_ROOT=$root; $env:AWS_ENDPOINT_URL='https://example.invalid'
    [IO.Directory]::CreateDirectory((Join-Path $root 'GeneralsRuntime')) | Out-Null
    $taskRoot='H:\Stage5SimulationValidationTask'; $title='ZeroHour'
    $calls=@()
    $cleanupStatement=$null
    foreach($workflow in @('check-replays.yml','ci.yml')) {
        $text=[IO.File]::ReadAllText((Join-Path $env:GITHUB_WORKSPACE ".github/workflows/$workflow"))
        foreach($block in [regex]::Matches($text,'(?ms)^        run: \|\r?\n(?<body>(?:          .*\r?\n|\r?\n)+)')) {
            $body=$block.Groups['body'].Value -replace '(?m)^          ',''
            $bodyAst=[Management.Automation.Language.Parser]::ParseInput($body,[ref]$null,[ref]$null)
            foreach($conditional in $bodyAst.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst]},$true)) {
                if($conditional.Extent.Text.StartsWith('if ($env:STAGE5_BASE_CONSUMERS_STATUS')) { $cleanupStatement=$conditional.Extent.Text }
            }
            foreach($command in $bodyAst.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst]},$true)) {
                if($command.CommandElements[0].Extent.Text -notmatch 'Install-Stage5SimulationQualificationData\.ps1') {continue}
                $invocation='Invoke-ProducerPreflight '+(($command.CommandElements | Select-Object -Skip 1 | ForEach-Object {$_.Extent.Text}) -join ' ')
                $script:canonicalExists=$invocation -match 'ProvisioningRole'
                $actual=& ([scriptblock]::Create($invocation))
                $calls += [string]$actual
            }
        }
    }
    Check ($calls.Count -eq 4) 'All four checked-in producer callers must execute the actual preflight and sink.'
    Check (@($calls | Where-Object {$_ -ceq 'H:\Stage5SimulationValidationTask\BaseGenerals'}).Count -eq 1) 'Generals companion did not select its fixed child.'
    Check (@($calls | Where-Object {$_ -ceq 'H:\Stage5SimulationValidationTask\BaseZeroHour'}).Count -eq 1) 'Zero Hour companion did not select its fixed child.'
    $output=[IO.File]::ReadAllText($sink)
    Check ($output.StartsWith("EXISTING=value`n") -and $output.Contains('STAGE5_GENERALS_QUALIFICATION_DATA_MANIFEST_PATH=') -and $output.Contains('STAGE5_ZEROHOUR_QUALIFICATION_DATA_MANIFEST_PATH=')) 'Role-derived publication failed to preserve the exact sink.'
    Reject { Open-Stage5SimulationEnvironmentFile $baseEnvironmentFile } 'Producer accepted an arbitrary existing output sink.'
    Reject { Get-Stage5SimulationProvisioningLayout $taskRoot ZeroHour GeneralsBase } 'Mismatched role/title was accepted.'
    Reject { Get-Stage5SimulationProvisioningLayout ($taskRoot+'\arbitrary') Generals Primary } 'Arbitrary task root was accepted.'
    $script:canonicalExists=$true; $script:childExists=$true
    Reject { Invoke-ProducerPreflight $runtimeRoot $taskRoot ('a'*40) Generals 'https://example.invalid' $sink GeneralsBase } 'A pre-existing companion target was reused.'
    $script:canonicalExists=$false; $script:childExists=$false
    Reject { Invoke-ProducerPreflight $runtimeRoot $taskRoot ('a'*40) Generals 'https://example.invalid' $sink GeneralsBase } 'Companion provisioning adopted a missing parent.'

    # Execute the actual late-cleanup gate with only its reserved filesystem
    # root relocated to owned scratch. The consumer/owner/hash checks are intact.
    Check ($null -ne $cleanupStatement) 'The external manifest lifetime gate was not found.'
    $script:lifetimeRoot=Join-Path $root 'lifetime'
    [IO.Directory]::CreateDirectory($script:lifetimeRoot) | Out-Null
    $lifetimeManifest=Join-Path $script:lifetimeRoot 'QualificationData.json'
    [IO.File]::WriteAllText($lifetimeManifest,'retained qualified data')
    $cleanupBody=[scriptblock]::Create($cleanupStatement.Replace(
        "`$simulationRoot = 'H:\Stage5SimulationValidationTask'", '$simulationRoot = $script:lifetimeRoot'))
    $expectedOwner='123:1'; $env:STAGE5_BASE_SIMULATION_CLEANUP_OWNER=$expectedOwner
    $env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH=$lifetimeManifest
    $env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256=(Get-FileHash -LiteralPath $lifetimeManifest).Hash
    foreach($status in @('in_progress','failure','cancelled')) {
        $env:STAGE5_BASE_CONSUMERS_STATUS=$status; & $cleanupBody
        Check (Test-Path -LiteralPath $lifetimeManifest) 'Cleanup removed the manifest before successful consumers finished.'
    }
    $env:STAGE5_BASE_CONSUMERS_STATUS='success'; $env:STAGE5_BASE_SIMULATION_CLEANUP_OWNER='foreign'
    & $cleanupBody
    Check (Test-Path -LiteralPath $lifetimeManifest) 'Cleanup removed an unowned manifest.'
    $env:STAGE5_BASE_SIMULATION_CLEANUP_OWNER=$expectedOwner
    [IO.File]::WriteAllText($lifetimeManifest,'changed')
    Reject { & $cleanupBody } 'Cleanup accepted a changed manifest hash.'
    Check (Test-Path -LiteralPath $lifetimeManifest) 'Rejected cleanup removed a changed manifest.'
    [IO.File]::WriteAllText($lifetimeManifest,'retained qualified data')
    & $cleanupBody
    Check (-not (Test-Path -LiteralPath $script:lifetimeRoot)) 'Successful owned cleanup did not remove its sole manifest and empty directory.'
    Write-Output 'Source-connected simulation provisioning role and sink tests passed.'
} finally {
    foreach($name in $originalEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name,$originalEnvironment[$name]) }
}
