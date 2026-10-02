[CmdletBinding()]
param(
    [string]$SourceRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($SourceRoot)) { $SourceRoot = $PSScriptRoot }

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Read-ParsedScript {
    param([string]$Path, [string]$Description)
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) `
        "$Description is missing: $Path"
    $tokens = $null
    $errors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$errors)
    Assert-True ($errors.Count -eq 0) `
        "$Description does not parse: $($errors -join '; ')"
    return $tree
}

$entrypointPath = Join-Path $SourceRoot 'Invoke-Stage5LocalLockstepDiagnostic.ps1'
$schemaPath = Join-Path $SourceRoot 'Stage5LocalLockstepDiagnostic.schema.json'
$entrypointAst = Read-ParsedScript $entrypointPath `
    'Invoke-Stage5LocalLockstepDiagnostic.ps1'
$entrypointSource = Get-Content -LiteralPath $entrypointPath -Raw
Assert-True (Test-Path -LiteralPath $schemaPath -PathType Leaf) `
    'local diagnostic receipt schema is missing'
$schemaSource = Get-Content -LiteralPath $schemaPath -Raw
Assert-True ($schemaSource -match '"additionalProperties"\s*:\s*false' -and
    $schemaSource -match 'stage5-local-lockstep-diagnostic' -and
    $schemaSource -match 'finalAcceptanceClaim') `
    'local diagnostic receipt schema must be strict and non-final'

foreach ($name in @(
    'ValidationSourceRoot', 'GeneralsExecutable', 'ZeroHourExecutable',
    'ArtifactSetManifestPath', 'LocalDataManifestPath', 'SourceCommit',
    'OutputDirectory', 'MapName', 'GeneralsMapCrc', 'ZeroHourMapCrc',
    'MaximumTotalEffectiveWorkers', 'AllowHeadlessDirectExecution')) {
    $parameter = @($entrypointAst.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -ceq $name
    })
    Assert-True ($parameter.Count -eq 1) `
        "local diagnostic entrypoint must expose explicit parameter '$name'"
}

Assert-True ($entrypointSource -match
    'Stage5InstalledLockstepV2Session\.psm1' -and
    $entrypointSource -match 'Stage5LocalLockstepDiagnosticData\.psm1' -and
    $entrypointSource -match 'Stage5LocalLockstepDiagnostic\.json' -and
    $entrypointSource -match 'Read-Stage5LocalLockstepDiagnosticData' -and
    $entrypointSource -match 'Invoke-Stage5InstalledLockstepV2SessionSet' -and
    $entrypointSource -match 'Write-Stage5LocalLockstepDiagnosticReceipt') `
    'local diagnostic entrypoint owns explicit shared/data module calls'
Assert-True ($entrypointSource -match 'Get-Stage5InstalledLockstepV2HostTopology' -and
    $entrypointSource -match 'MaximumTotalEffectiveWorkers' -and
    $entrypointSource -match 'predictedEffectiveWorkerTotal' -and
    $entrypointSource -match 'effectiveWorkerTotal') `
    'local diagnostic checks host and observed worker capacity before final publication'
Assert-True ($entrypointSource -match 'finalAcceptanceClaim' -and
    $entrypointSource -match 'externalQualificationSkipped' -and
    $entrypointSource -match 'manualTestingDeferred') `
    'local diagnostic retains explicit non-final/deferred dispositions'

# Exercise the cheap pre-launch budget contract without invoking native peers.
# On the supported twelve-logical-processor host, the installed pair predicts
# two explicit workers plus ten automatic workers; a budget of eleven must fail
# before the data reader or shared session delegate can run.
$budgetDefinitions = @($entrypointAst.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -in @(
            'Get-Stage5LocalLockstepExpectedEffectiveWorkerTotal',
            'Assert-Stage5LocalLockstepWorkerBudget')
}, $true))
Assert-True ($budgetDefinitions.Count -eq 2) `
    'local diagnostic pre-launch worker budget helpers are missing.'
foreach ($definition in $budgetDefinitions) {
    Invoke-Expression $definition.Extent.Text
}
Assert-True ((Get-Stage5LocalLockstepExpectedEffectiveWorkerTotal 12) -eq 12) `
    'twelve-logical-processor worker prediction must be explicit-two plus auto-ten.'
$budgetRejected = $false
try { Assert-Stage5LocalLockstepWorkerBudget 12 11 | Out-Null }
catch { $budgetRejected = $true }
Assert-True $budgetRejected `
    'an eleven-worker budget must fail before local diagnostic execution.'
Assert-True ((Assert-Stage5LocalLockstepWorkerBudget 12 12) -eq 12) `
    'the default twelve-worker budget must admit the supported host prediction.'
$budgetOffset = $entrypointSource.IndexOf(
    'Assert-Stage5LocalLockstepWorkerBudget', [StringComparison]::Ordinal)
$dataOffset = $entrypointSource.IndexOf(
    'Read-Stage5LocalLockstepDiagnosticData', [StringComparison]::Ordinal)
$sessionOffset = $entrypointSource.IndexOf(
    'Invoke-Stage5InstalledLockstepV2SessionSet', [StringComparison]::Ordinal)
Assert-True ($budgetOffset -ge 0 -and $dataOffset -gt $budgetOffset -and
    $sessionOffset -gt $budgetOffset) `
    'local diagnostic worker budget must run before data/session execution.'

# The local entrypoint must never reach the canonical archive/envelope route or
# import the top-level runner as an implicit dependency.
Assert-True ($entrypointSource -notmatch
    'Invoke-InstalledLockstepV2Validation\.ps1' -and
    $entrypointSource -notmatch 'Read-AndValidateQualificationData' -and
    $entrypointSource -notmatch 'New-LockstepV2FinalAcceptanceEnvelope' -and
    $entrypointSource -notmatch 'AST|Parser.*ParseFile') `
    'local diagnostic entrypoint does not import, AST-evaluate, or bypass through the canonical runner'

Write-Output 'Stage 5 local lockstep diagnostic contract tests passed.'
