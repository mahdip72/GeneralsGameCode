[CmdletBinding()]
param([string]$SourceRoot = '', [string]$ScratchRoot = '')
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($SourceRoot)) { $SourceRoot = $PSScriptRoot }
if ([string]::IsNullOrWhiteSpace($ScratchRoot)) { $ScratchRoot = $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT }
if ([string]::IsNullOrWhiteSpace($ScratchRoot) -or
    [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($ScratchRoot)) -ine 'H:\') {
    throw 'Suspended process tests require a task-owned H: ScratchRoot.'
}
$root = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('stage5-owned-child-' + $PID + '-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$tools = Join-Path $root 'installed-tools'
[IO.Directory]::CreateDirectory($tools) | Out-Null
foreach ($leaf in @('Stage5InstalledLockstepV2Session.psm1',
        'DeterministicSimulationEvidence.psm1', 'Stage5RegistryRecovery.psm1',
        'Stage5ValidationProfileCapability.psm1')) {
    Copy-Item -LiteralPath (Join-Path $SourceRoot $leaf) `
        -Destination (Join-Path $tools $leaf)
}
$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$child = $null
try {
    $env:TEMP = $root
    $env:TMP = $root
    $module = Import-Module (Join-Path $tools 'Stage5InstalledLockstepV2Session.psm1') -PassThru
    $executable = (Get-Command pwsh -CommandType Application | Select-Object -First 1).Source
    $hash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    & $module { Initialize-LockstepSuspendedProcessNative }
    $environmentName = 'RTS_STAGE5_ENV_' + [Guid]::NewGuid().ToString('N')
    try {
        foreach ($initial in @($null, '', 'original-value')) {
            if ($null -eq $initial) {
                [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue($environmentName, [NullString]::Value)
            } else {
                [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue($environmentName, $initial)
            }
            $snapshot = & $module {
                param($Name) Set-LockstepProcessEnvironment ([ordered]@{ $Name = 'changed' })
            } $environmentName
            & $module { param($Snapshot) Restore-LockstepProcessEnvironment $Snapshot } $snapshot
            $actual = [Stage5ValidationNative.SuspendedChild]::ReadEnvironmentValue($environmentName)
            if (($null -eq $actual) -ne ($null -eq $initial) -or $actual -cne $initial) {
                throw 'Missing/empty/present environment restoration differs.'
            }
            $failed = $false
            try {
                & $module {
                    param($Name)
                    Set-LockstepProcessEnvironment ([ordered]@{ $Name = 'partial-change'; 'invalid=name' = 'fail' }) | Out-Null
                } $environmentName
            } catch { $failed = $true }
            $actual = [Stage5ValidationNative.SuspendedChild]::ReadEnvironmentValue($environmentName)
            if (-not $failed -or ($null -eq $actual) -ne ($null -eq $initial) -or $actual -cne $initial) {
                throw 'Mid-setup environment failure did not roll back its prefix exactly.'
            }
        }
    }
    finally { [Stage5ValidationNative.SuspendedChild]::WriteEnvironmentValue($environmentName, [NullString]::Value) }
    $originalMarker = [Environment]::GetEnvironmentVariable('RTS_SUSPENDED_TEST', 'Process')
    $environmentSnapshot = & $module {
        Set-LockstepProcessEnvironment @{ RTS_SUSPENDED_TEST = 'INTEGRATED_OK' }
    }
    try {
        $child = & $module {
            param($Exe, $Directory, $Hash)
            New-LockstepSuspendedChild $Exe `
                '-NoProfile -NonInteractive -Command "[Console]::WriteLine($env:RTS_SUSPENDED_TEST); exit 0"' `
                $Directory (Join-Path $Directory 'child.stdout.log') `
                (Join-Path $Directory 'child.stderr.log') $Hash
        } $executable $root $hash
    }
    finally { & $module { param($Snapshot) Restore-LockstepProcessEnvironment $Snapshot } $environmentSnapshot }
    if ([Environment]::GetEnvironmentVariable('RTS_SUSPENDED_TEST', 'Process') -cne $originalMarker) {
        throw 'Parent scoped environment was not restored.'
    }
    $lifecycle = & $module { param($Process) New-LockstepProcessLifecycleRecord $Process } $child
    $identity = & $module { param($Process, $Hash) New-LockstepRecoveryStartedIdentity $Process $Hash } $child $hash
    $lifecycle.identity = $identity
    if ($child.HasResumed -or $child.HasExited) { throw 'Child did not remain behind the launch barrier.' }
    $identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'start.json') -Encoding UTF8
    $lifecycle.startProofPublished = $true
    $child.Resume()
    if (-not $child.WaitForExit(5000)) { throw 'Owned immediate-exit child did not exit.' }
    $afterExit = & $module { param($Process, $Hash) New-LockstepRecoveryStartedIdentity $Process $Hash } $child $hash
    if ($afterExit.processId -ne $identity.processId -or
        $afterExit.creationTimeUtc100ns -ne $identity.creationTimeUtc100ns -or
        $afterExit.executablePath -cne $identity.executablePath) {
        throw 'Cached identity changed after confirmed exit.'
    }
    $exit = & $module { param($Process) Stop-TaskPeer $Process } $child
    $child = $null
    $events = New-Object 'Collections.Generic.List[object]'
    $observer = { param($Event) $events.Add($Event) | Out-Null }
    $published = & $module {
        param($Record, $Observer, $Exit)
        Publish-LockstepProcessExitProof $Record $Observer $Exit
    } $lifecycle $observer $exit
    if ($exit -ne 0 -or -not $published -or $events.Count -ne 1 -or
        -not $lifecycle.identity.exitProven -or
        (Get-Content -LiteralPath (Join-Path $root 'child.stdout.log') -Raw).Trim() -cne 'INTEGRATED_OK') {
        throw 'Actual module cleanup/publication or environment fidelity failed.'
    }
    if (@(Get-ChildItem -LiteralPath $tools -Filter '*.cs').Count -ne 0) {
        throw 'Installed-like test unexpectedly used a standalone C# dependency.'
    }
    # Exercise both actual catch/adoption statements with one real still-
    # suspended carrier. Only the factory call is replaced by an injected
    # failure; the adoption code and retained owner helper remain unchanged.
    $child = & $module {
        param($Exe, $Directory, $Hash)
        New-LockstepSuspendedChild $Exe '-NoProfile -Command "exit 0"' $Directory `
            (Join-Path $Directory 'adopt.stdout.log') (Join-Path $Directory 'adopt.stderr.log') $Hash
    } $executable $root $hash
    $sourceTokens = $null
    $sourceErrors = $null
    $tree = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $tools 'Stage5InstalledLockstepV2Session.psm1'),
        [ref]$sourceTokens, [ref]$sourceErrors)
    foreach ($functionName in @('Invoke-LockstepSession', 'Invoke-LockstepNegativeProbe')) {
        $definition = $tree.Find({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq $functionName
        }, $true)
        $adoption = @($definition.FindAll({ param($node)
            $node -is [Management.Automation.Language.TryStatementAst] -and
                $node.CatchClauses.Count -gt 0 -and
                $node.Body.Extent.Text -match 'New-LockstepSuspendedChild' -and
                $node.CatchClauses[0].Extent.Text -match 'Get-LockstepRetainedFailedChild'
        }, $true))
        if ($adoption.Count -ne 1) { throw 'Expected exactly one factory adoption boundary per launch site.' }
        & $module {
            param($Statement, $Owned, $FunctionName)
            $factoryFailure = New-Object Stage5ValidationNative.SuspendedLaunchException(
                (New-Object InvalidOperationException('synthetic capture failure')),
                (New-Object InvalidOperationException('synthetic cleanup failure')),
                $Owned, [uint32]$Owned.Id)
            function Invoke-TestFactoryFailure { throw $factoryFailure }
            $Executable = 'unused'; $argumentString = ''; $workingDirectory = 'unused'
            $stdout = ''; $stderr = ''; $stdoutPath = ''; $stderrPath = ''; $ExecutableSha256 = ''
            $process = $null; $processes = @()
            $lifecycleRecords = New-Object 'Collections.Generic.List[object]'
            $lifecycle = New-LockstepProcessLifecycleRecord $null
            $caught = $false
            try { . ([scriptblock]::Create($Statement.Replace('New-LockstepSuspendedChild', 'Invoke-TestFactoryFailure'))) }
            catch { $caught = $true }
            if (-not $caught -or -not [Object]::ReferenceEquals($process, $Owned)) {
                throw 'Factory failure lost its retained native owner.'
            }
            if ($FunctionName -ceq 'Invoke-LockstepSession') {
                if ($lifecycleRecords.Count -ne 1 -or
                    -not [Object]::ReferenceEquals($lifecycleRecords[0].process, $Owned)) {
                    throw 'Normal launch did not register the retained lifecycle.'
                }
            }
            elseif (-not [Object]::ReferenceEquals($lifecycle.process, $Owned)) {
                throw 'Negative probe did not register the retained lifecycle.'
            }
        } $adoption[0].Extent.Text $child $functionName
    }
    if ($child.HasResumed) { throw 'Adoption fixture unexpectedly resumed its child.' }
    $adoptionExit = & $module { param($Owned) Stop-TaskPeer $Owned } $child
    $child = $null
    if ($adoptionExit -ne 1) { throw 'Retained suspended owner was not terminated through the original handle.' }
    [pscustomobject]@{ host = $PSVersionTable.PSVersion.ToString(); pid = $identity.processId
        creationTimeUtc100ns = $identity.creationTimeUtc100ns; cachedAfterExit = $true
        originalHandleExitCode = $exit; exitPublications = $events.Count
        retainedOwnerLaunchSites = 2; environmentInitialStates = 3
        standaloneCsFiles = 0; root = $root } | ConvertTo-Json -Compress
    'INTEGRATED_SUSPENDED_MODULE_PASS'
}
finally {
    if ($null -ne $child) {
        try {
            $child.Kill()
            if (-not $child.WaitForExit(5000)) { throw 'Owned integrated child cleanup timed out.' }
        }
        finally { $child.Dispose() }
    }
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
}
