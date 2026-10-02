[CmdletBinding()]
param(
    [string]$SourceRoot = '',
    [switch]$LifecyclePreflightOnly,
    [string]$ScratchRoot = ''
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

$modulePath = Join-Path $SourceRoot 'Stage5InstalledLockstepV2Session.psm1'
$runnerPath = Join-Path $SourceRoot 'Invoke-InstalledLockstepV2Validation.ps1'
$moduleSource = if (Test-Path -LiteralPath $modulePath -PathType Leaf) {
    Get-Content -LiteralPath $modulePath -Raw
} else { '' }
$runnerSource = Get-Content -LiteralPath $runnerPath -Raw
$moduleAst = Read-ParsedScript $modulePath 'Stage5InstalledLockstepV2Session.psm1'
$runnerAst = Read-ParsedScript $runnerPath 'Invoke-InstalledLockstepV2Validation.ps1'

$expectedModuleFunctions = @(
    'Get-Stage5InstalledLockstepV2HostTopology',
    'Read-AndValidateArtifactSet',
    'Invoke-Stage5InstalledLockstepV2SessionSet'
)
foreach ($name in $expectedModuleFunctions) {
    $definitions = @($moduleAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $name
    }, $true))
    Assert-True ($definitions.Count -eq 1) `
        "shared session module must define exactly one $name boundary"
}

# The extracted module must be a shared session boundary, never a second
# acceptance implementation or a mode that disables checks inside the session.
Assert-True ($moduleSource -notmatch 'Read-AndValidateQualificationData' -and
    $moduleSource -notmatch 'New-LockstepV2FinalAcceptanceEnvelope' -and
    $moduleSource -notmatch 'DiagnosticMode') `
    'shared session module does not contain canonical archive/envelope code or a diagnostic bypass switch'
Assert-True ($moduleSource -match 'DeterministicSimulationEvidence\.psm1' -and
    $moduleSource -match 'Stage5RegistryRecovery\.psm1' -and
    $moduleSource -match 'Import-Module[^\r\n]+-ErrorAction Stop') `
    'shared session module owns explicit evidence and registry-recovery dependencies'
Assert-True ($moduleSource -match
    'Set-Stage5LockstepHostSelfTestScratchRoot\s+\$null') `
    'shared production session clears any inherited self-test path exception'

$runnerFunctions = @($runnerAst.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true) | ForEach-Object { $_.Name })
foreach ($movedName in @(
    'Invoke-LockstepSession', 'Invoke-LockstepNegativeProbe',
    'Get-LockstepWorkerProfiles', 'New-LockstepTitleSessionContract',
    'Read-AndValidateArtifactSet')) {
    Assert-True ($runnerFunctions -notcontains $movedName) `
        "canonical runner must consume the shared module instead of redefining $movedName"
}
Assert-True ($runnerSource -match 'Stage5InstalledLockstepV2Session\.psm1' -and
    $runnerSource -match 'Invoke-Stage5InstalledLockstepV2SessionSet') `
    'canonical runner imports and invokes the shared session boundary'
Assert-True ($runnerSource -match 'Read-AndValidateQualificationData' -and
    $runnerSource -match 'New-LockstepV2FinalAcceptanceEnvelope' -and
    $runnerSource -match 'Invoke-SelfTest') `
    'canonical archive, envelope, and self-test responsibilities remain in the canonical runner'

if ($LifecyclePreflightOnly) {
    $sessionModule = Import-Module $modulePath -Force -PassThru
    $mapCommand = Get-Command Assert-LockstepMapCrcs -CommandType Function `
        -ErrorAction SilentlyContinue
    $pathCommand = Get-Command Resolve-BoundedArtifactPath -CommandType Function `
        -ErrorAction SilentlyContinue
    Assert-True ($null -ne $mapCommand -and $null -ne $pathCommand) `
        'canonical callers can import both shared pure session helpers'
    $map = & $mapCommand ([ordered]@{ Generals = 1; ZeroHour = 2 }) `
        'focused shared-session map CRC fixture'
    $resolved = & $pathCommand $modulePath `
        'Stage5InstalledLockstepV2Session.psm1'
    Assert-True ($map.Generals -eq 1 -and $map.ZeroHour -eq 2 -and
        [IO.Path]::GetFullPath($resolved) -ceq [IO.Path]::GetFullPath($modulePath)) `
        'canonical callers did not receive the real shared pure-helper results'

    $sessionDefinition = @($moduleAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Invoke-LockstepSession'
    }, $true))[0]
    $negativeDefinition = @($moduleAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Invoke-LockstepNegativeProbe'
    }, $true))[0]
    foreach ($definition in @($sessionDefinition, $negativeDefinition)) {
        $body = $definition.Extent.Text
        $waitOffset = $body.IndexOf('Wait-ForLeaf', [StringComparison]::Ordinal)
        Assert-True ($waitOffset -ge 0 -and
            $body.IndexOf('.Refresh()', $waitOffset, [StringComparison]::Ordinal) -lt 0 -and
            $body.IndexOf('$record.process.Path', $waitOffset,
                [StringComparison]::Ordinal) -lt 0 -and
            $body.IndexOf('$process.Path', $waitOffset,
                [StringComparison]::Ordinal) -lt 0 -and
            $body -match 'Publish-LockstepProcessExitProof' -and
            $body -match 'startProofPublished') `
            "$($definition.Name) must use the captured start identity and publish retained-handle exit proof."
    }
    Assert-True ($moduleSource -match '\$allPorts\s*=\s*@\(\)' -and
        $moduleSource -match 'Stop-TaskPeer\s+\$lifecycle\.process' -and
        $moduleSource -match 'Publish-LockstepProcessExitProof[\s\S]*-SuppressErrors') `
        'shared session lifecycle cleanup must cover all started peers with bounded retained-handle proof'

    $identity = [pscustomobject]@{
        launchPending = $false
        processId = 321
        creationTimeUtc100ns = [Int64]133000000000000321
        executablePath = [IO.Path]::GetFullPath($modulePath)
        executableSha256 = ('A' * 64)
        exitProven = $false
    }
    $lifecycle = [pscustomobject]@{
        process = $null
        identity = $identity
        startProofPublished = $true
        exitProofPublished = $false
    }
    $observations = New-Object 'Collections.Generic.List[object]'
    $observer = { param($observation) $observations.Add($observation) | Out-Null }
    $published = & $sessionModule {
        param($Lifecycle, $Observer)
        Publish-LockstepProcessExitProof $Lifecycle $Observer 0
    } $lifecycle $observer
    Assert-True ($published -and $lifecycle.exitProofPublished -and
        $lifecycle.identity.exitProven -and $observations.Count -eq 1 -and
        $observations[0].processIdentity.processId -eq 321 -and
        $observations[0].exited) `
        'successful retained-handle lifecycle publication must record one exit proof'

    $failedIdentity = [pscustomobject]@{
        launchPending = $false
        processId = 654
        creationTimeUtc100ns = [Int64]133000000000000654
        executablePath = [IO.Path]::GetFullPath($modulePath)
        executableSha256 = ('B' * 64)
        exitProven = $false
    }
    $failedLifecycle = [pscustomobject]@{
        process = $null
        identity = $failedIdentity
        startProofPublished = $true
        exitProofPublished = $false
    }
    $failedObserver = { throw 'synthetic exit-observer publication failure' }
    $caught = $null
    try {
        & $sessionModule {
            param($Lifecycle, $Observer)
            Publish-LockstepProcessExitProof $Lifecycle $Observer 1
        } $failedLifecycle $failedObserver | Out-Null
    }
    catch { $caught = $_ }
    Assert-True ($null -ne $caught -and
        -not $failedLifecycle.exitProofPublished -and
        -not $failedLifecycle.identity.exitProven) `
        'failed exit-observer publication must remain unproven and fail closed'
    $suppressed = & $sessionModule {
        param($Lifecycle, $Observer)
        Publish-LockstepProcessExitProof $Lifecycle $Observer 1 -SuppressErrors
    } $failedLifecycle $failedObserver
    Assert-True (-not $suppressed -and
        -not $failedLifecycle.exitProofPublished -and
        -not $failedLifecycle.identity.exitProven) `
        'suppressed cleanup observer failure must not become exit authority'

    & (Join-Path $PSScriptRoot 'Stage5SuspendedProcess.Tests.ps1') `
        -SourceRoot $SourceRoot -ScratchRoot $ScratchRoot
    Write-Output 'Stage 5 shared installed-lockstep-v2 lifecycle preflight passed.'
    return
}

Write-Output 'Stage 5 shared installed-lockstep-v2 session contract tests passed.'
