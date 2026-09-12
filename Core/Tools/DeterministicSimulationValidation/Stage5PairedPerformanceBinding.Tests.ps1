[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ScratchRoot)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'Invoke-Stage5PerformanceScalingValidation.ps1'), [ref]$null, [ref]$null)
$names = @('Assert-Stage5PerformanceCondition','Test-Stage5SafeTitleSessionPath',
    'New-Stage5TitleSessionContract','New-Stage5RegistryRecoveryContext',
    'Get-Stage5RegistryInstallPathAncestors','Get-Stage5RegistryRecoveryMissingSubKeys',
    'New-Stage5RegistryRecoveryMutationSnapshot','ConvertTo-Stage5RecoveryRegistryView',
    'Add-Stage5RegistryRecoveryMutation','Assert-Stage5RegistryRecoverySnapshotCurrent',
    'Update-Stage5RegistryRecoveryState')
foreach ($function in $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true) | Where-Object { $names -contains $_.Name }) { . ([scriptblock]::Create($function.Extent.Text)) }
Import-Module (Join-Path $PSScriptRoot 'Stage5RegistryRecovery.psm1') -Force
$root = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('performance-pair-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$base = Join-Path $root 'Generals'
$runtime = Join-Path $root 'ZeroHour'
$session = New-Stage5TitleSessionContract ZeroHour (Join-Path $root 'TitleSession') $runtime $root -GeneralsRuntimeRoot $base
Check ($session.registryValues.Count -eq 2 -and $session.registryValues[0].value -ceq ($base + '\')) 'Performance ZH session does not bind its paired Generals root.'
$validationUserSid = 'S-1-5-21-1-2-3-1000'
$script:Stage5ValidationMutexName = Get-Stage5RegistryRecoveryMutexName $validationUserSid
$script:Stage5RunnerScriptSha256 = 'A' * 64
$state = @{}
foreach ($view in @('Registry32','Registry64')) {
    foreach ($value in $session.registryValues) { $state["$view|$($value.subKey)"] = 'original' }
}
function New-Stage5RegistryRecoveryAdapter {
    return @{
        GetKey = { param($view,$key) [pscustomobject]@{ exists=$true; valueNames=@('InstallPath'); subKeyNames=@() } }
        GetValue = { param($view,$key,$name) [pscustomobject]@{ exists=$true; value=$state["$view|$key"]; kind=[Microsoft.Win32.RegistryValueKind]::String } }
    }
}
$writes = New-Object 'Collections.Generic.List[string]'
function Set-Stage5RegistryValue {
    param($View,$SubKey,$Name,$Value,$Snapshots,$SnapshotKeys,$WrittenSnapshots)
    Check ($Snapshots.Count -eq 4) 'A paired performance registry write occurred before all four snapshots were planned.'
    $state["$View|$SubKey"] = $Value
    $writes.Add("$View|$SubKey") | Out-Null
}
$context = New-Stage5RegistryRecoveryContext -Title ZeroHour -TaskRootPath $root `
    -JournalPath (Join-Path $root 'Stage5RegistryRecovery.json') `
    -ExecutionNonce '11111111-1111-4111-8111-111111111111' -SourceCommit ('a' * 40) `
    -ArtifactSetSha256 ('B' * 64) -ExecutablePath (Join-Path $runtime 'generalszh.exe') `
    -ExecutableSha256 ('C' * 64) -RegistryValues $session.registryValues
Check ($context.snapshots.Count -eq 4) 'Performance paired recovery did not plan all views before mutation.'
$firstValue = $session.registryValues[0]
$firstKey = "Registry32|$($firstValue.subKey)"
$state[$firstKey] = 'foreign-after-plan'
$rejected = $false
try { Add-Stage5RegistryRecoveryMutation $context ([Microsoft.Win32.RegistryView]::Registry32) $firstValue }
catch { $rejected = $_.Exception.Message -match 'ownership changed after planning' }
Check ($rejected -and $writes.Count -eq 0 -and $state[$firstKey] -ceq 'foreign-after-plan') 'Performance setup overwrote a concurrent change after planning.'
$state[$firstKey] = 'original'
foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,[Microsoft.Win32.RegistryView]::Registry64)) {
    foreach ($value in $session.registryValues) { Add-Stage5RegistryRecoveryMutation $context $view $value }
}
Check ($writes.Count -eq 4) 'Performance paired setup omitted a title/view write.'
Write-Output 'Stage 5 paired performance binding fake-adapter tests passed.'
