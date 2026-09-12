[CmdletBinding()]
param([string]$ScratchRoot = '')

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-RecoveryTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-RecoveryThrows {
    param([scriptblock]$Action, [string]$Message)
    $caught = $null
    try { & $Action } catch { $caught = $_ }
    $script:LastRecoveryRejection = if ($null -eq $caught) { '' } else { $caught.Exception.Message }
    Assert-RecoveryTest ($null -ne $caught) $Message
}

function Copy-RecoveryIdentity {
    param([Collections.IDictionary]$Source)
    $copy = [ordered]@{}
    foreach ($key in $Source.Keys) { $copy[[string]$key] = $Source[$key] }
    return $copy
}

$scratch = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace(
        $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
else { throw 'Registry-recovery tests require an explicit H: scratch root.' }
Assert-RecoveryTest ($scratch.StartsWith('H:\',
    [StringComparison]::OrdinalIgnoreCase)) 'Test scratch must remain on H:.'
$root = Join-Path $scratch ('stage5-registry-recovery-' +
    [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null

try {
    Import-Module (Join-Path $PSScriptRoot 'Stage5RegistryRecovery.psm1') -Force
    $userSid = 'S-1-5-21-1-2-3-1000'
    $identity = [ordered]@{
        runNonce = '11111111-1111-4111-8111-111111111111'
        title = 'Generals'
        taskRoot = $root
        journalPath = Join-Path $root 'Stage5RegistryRecovery.json'
        userSid = $userSid
        mutexName = "Global\GeneralsGameCode.Stage5PerformanceValidation-$userSid"
        identityMode = 'acceptance-bound'
        runnerScriptSha256 = ('C' * 64)
        executableSha256 = ('A' * 64)
        sourceCommit = ('a' * 40)
        artifactSetSha256 = ('B' * 64)
    }
    $processIdentity = [ordered]@{
        childLaunched = $false; processId = 0; creationTimeUtc100ns = 0
        executablePath = 'H:\Installed\generalsv.exe'
        executableSha256 = ('A' * 64)
    }
    $mutexLock = [pscustomobject]@{
        acquired = $true; name = $identity.mutexName
    }

    $state = @{}
    function Get-FakeKey {
        param([string]$View, [string]$SubKey)
        $keyId = "$View|$SubKey"
        if (-not $state.ContainsKey($keyId)) {
            return [pscustomobject]@{ exists = $false; valueNames = @(); subKeyNames = @() }
        }
        $prefix = $keyId + '\'
        $children = @($state.Keys | Where-Object {
            $_.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object {
            $_.Substring($prefix.Length).Split('\')[0]
        } | Sort-Object -Unique)
        return [pscustomobject]@{
            exists = $true; valueNames = @($state[$keyId].values.Keys)
            subKeyNames = $children
        }
    }
    $adapter = [ordered]@{
        GetValue = {
            param($View, $SubKey, $Name)
            $keyId = "$View|$SubKey"
            if (-not $state.ContainsKey($keyId) -or
                -not $state[$keyId].values.ContainsKey($Name)) {
                return [pscustomobject]@{ exists = $false; value = $null; kind = $null }
            }
            $entry = $state[$keyId].values[$Name]
            return [pscustomobject]@{ exists = $true; value = $entry.value; kind = $entry.kind }
        }
        GetKey = { param($View, $SubKey) Get-FakeKey $View $SubKey }
        SetValue = {
            param($View, $SubKey, $Name, $Value, $Kind)
            $keyId = "$View|$SubKey"
            if (-not $state.ContainsKey($keyId)) { $state[$keyId] = @{ values = @{} } }
            $state[$keyId].values[$Name] = @{ value = $Value; kind = [int]$Kind }
        }
        DeleteValue = {
            param($View, $SubKey, $Name)
            $keyId = "$View|$SubKey"
            if ($state.ContainsKey($keyId)) { $state[$keyId].values.Remove($Name) }
        }
        DeleteKey = { param($View, $SubKey) $state.Remove("$View|$SubKey") }
    }
    $baseFixtureRegistryAdapter = $adapter
    $kind = [ordered]@{
        String = [Microsoft.Win32.RegistryValueKind]::String
        ExpandString = [Microsoft.Win32.RegistryValueKind]::ExpandString
        MultiString = [Microsoft.Win32.RegistryValueKind]::MultiString
        Binary = [Microsoft.Win32.RegistryValueKind]::Binary
        DWord = [Microsoft.Win32.RegistryValueKind]::DWord
        QWord = [Microsoft.Win32.RegistryValueKind]::QWord
    }

    $installKey = 'Software\Electronic Arts\EA Games\Generals'
    $state["Registry32|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $state["Registry64|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $typed = @(
        @{ old = 'before'; kind = $kind.String },
        @{ old = '%OLD%'; kind = $kind.ExpandString },
        @{ old = [string[]]@(); kind = $kind.MultiString },
        @{ old = [string[]]@('old-one'); kind = $kind.MultiString },
        @{ old = @('old-a', 'old-b'); kind = $kind.MultiString },
        @{ old = [byte[]]@(); kind = $kind.Binary },
        @{ old = [byte[]]@(255); kind = $kind.Binary },
        @{ old = [byte[]](1, 2, 255); kind = $kind.Binary },
        @{ old = [uint32]7; kind = $kind.DWord },
        @{ old = [int64]10; kind = $kind.QWord }
    )
    foreach ($entry in $typed) {
        $snapshot = New-Stage5RegistryRecoverySnapshot -Title Generals `
            -View Registry32 -SubKey $installKey -Name InstallPath `
            -HadKey $true -HadValue $true -OldValue $entry.old -OldKind $entry.kind `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String
        Assert-RecoveryTest ([int]$snapshot.oldKind -eq [int]$entry.kind) `
            'Raw registry kind was not retained.'
        Assert-RecoveryTest ([string]$snapshot.oldValue.type -ceq $entry.kind.ToString()) `
            'Raw registry value type was not retained.'
        $decoded = & (Get-Module Stage5RegistryRecovery) {
            param($encoded)
            ConvertFrom-Stage5RecoveryEncodedValue $encoded 'Typed recovery regression'
        } $snapshot.oldValue
        if ($entry.kind -eq $kind.MultiString -or $entry.kind -eq $kind.Binary) {
            $expectedType = if ($entry.kind -eq $kind.MultiString) { [string[]] } else { [byte[]] }
            Assert-RecoveryTest ($decoded.GetType() -eq $expectedType -and
                $decoded.Length -eq $entry.old.Length) `
                'Raw registry array type or cardinality changed across decoding.'
        }
    }

    $snapshots = @((New-Stage5RegistryRecoverySnapshot -Title Generals `
        -View Registry32 -SubKey $installKey -Name InstallPath `
        -HadKey $true -HadValue $true -OldValue 'before' -OldKind $kind.String `
        -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String))
    $identity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots $snapshots
    $journal = New-Stage5RegistryRecoveryJournal -Path $identity.journalPath `
        -Identity $identity -PlannedMissingSubKeys @() -Snapshots $snapshots `
        -ProcessIdentity $processIdentity
    Assert-RecoveryTest ($journal.state -ceq 'planned') 'Journal did not publish planned state.'
    $appendedSnapshot = New-Stage5RegistryRecoverySnapshot -Title Generals `
        -View Registry64 -SubKey $installKey -Name InstallPath `
        -HadKey $true -HadValue $true -OldValue 'before64' -OldKind $kind.String `
        -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String
    $appendedSnapshots = @($snapshots + $appendedSnapshot)
    Update-Stage5RegistryRecoveryJournal -Path $identity.journalPath `
        -ExpectedIdentity $identity -State active -PlannedMissingSubKeys @() `
        -Snapshots $appendedSnapshots `
        -ChildExitProof $true -NoActiveTitleProcesses $true `
        -ProcessIdentity $processIdentity | Out-Null
    $snapshots = $appendedSnapshots
    $authorization = [ordered]@{
        childExitProven = $true; noActiveTitleProcesses = $true
        processIdentities = @()
    }
    $restored = Invoke-Stage5RegistryRecovery -Path $identity.journalPath `
        -ExpectedIdentity $identity -Authorization $authorization `
        -MutexLock $mutexLock -Adapter $adapter
    $value = & $adapter['GetValue'] 'Registry32' $installKey 'InstallPath'
    Assert-RecoveryTest ($restored.state -ceq 'restored' -and
        [string]$value.value -ceq 'before' -and
        [int]$value.kind -eq [int]$kind.String) 'InstallPath did not restore exactly.'
    $value64 = & $adapter['GetValue'] 'Registry64' $installKey 'InstallPath'
    Assert-RecoveryTest ([string]$value64.value -ceq 'before64' -and
        [int]$value64.kind -eq [int]$kind.String) 'Appended InstallPath did not restore exactly.'

    # The runner's recovery authorization contains one or more retained
    # process identities.  ConvertTo-Stage5RecoveryProcessIdentities returns
    # an array of ordered dictionaries; keep one and two identity cases here
    # so a single-item pipeline unroll cannot turn authProcesses[0] into the
    # first Boolean value (launchPending) instead of an identity object.
    $authorizationProcessIdentity = [ordered]@{
        launchPending = $false; processId = 58460
        creationTimeUtc100ns = [Int64]134331680251836257
        executablePath = 'H:\Installed\generalsv.exe'
        executableSha256 = ('A' * 64); exitProven = $true
    }
    $oneRoot = Join-Path $root 'authorization-one-process'
    New-Item -ItemType Directory -Path $oneRoot -Force | Out-Null
    $oneIdentity = Copy-RecoveryIdentity $identity
    $oneIdentity.taskRoot = $oneRoot
    $oneIdentity.journalPath = Join-Path $oneRoot 'Stage5RegistryRecovery.json'
    $state.Clear()
    $state["Registry32|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\OneBefore'; kind = [int]$kind.String
    } } }
    $oneSnapshot = New-Stage5RegistryRecoverySnapshot -Title Generals `
        -View Registry32 -SubKey $installKey -Name InstallPath -HadKey $true `
        -HadValue $true -OldValue 'H:\Task\OneBefore' -OldKind $kind.String `
        -ExpectedValue 'H:\Task\OneRuntime' -ExpectedKind $kind.String
    $oneIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots @($oneSnapshot)
    New-Stage5RegistryRecoveryJournal -Path $oneIdentity.journalPath `
        -Identity $oneIdentity -PlannedMissingSubKeys @() `
        -Snapshots @($oneSnapshot) -ProcessIdentities @($authorizationProcessIdentity) | Out-Null
    $oneAuthorization = [ordered]@{
        childExitProven = $true; noActiveTitleProcesses = $true
        processIdentities = @([ordered]@{
            launchPending = $false; processId = 58460
            creationTimeUtc100ns = [Int64]134331680251836257
            executablePath = 'H:\Installed\generalsv.exe'
            executableSha256 = ('A' * 64); exitProven = $true
        })
    }
    $oneRestored = Invoke-Stage5RegistryRecovery -Path $oneIdentity.journalPath `
        -ExpectedIdentity $oneIdentity -Authorization $oneAuthorization `
        -MutexLock $mutexLock -Adapter $adapter
    $oneValue = & $adapter['GetValue'] 'Registry32' $installKey 'InstallPath'
    Assert-RecoveryTest ($oneRestored.state -ceq 'restored' -and
        [string]$oneValue.value -ceq 'H:\Task\OneBefore' -and
        [int]$oneValue.kind -eq [int]$kind.String) `
        'One-process recovery authorization did not preserve the complete identity schema.'

    $twoRoot = Join-Path $root 'authorization-two-processes'
    New-Item -ItemType Directory -Path $twoRoot -Force | Out-Null
    $twoIdentity = Copy-RecoveryIdentity $identity
    $twoIdentity.taskRoot = $twoRoot
    $twoIdentity.journalPath = Join-Path $twoRoot 'Stage5RegistryRecovery.json'
    $secondAuthorizationProcessIdentity = [ordered]@{
        launchPending = $false; processId = 58461
        creationTimeUtc100ns = [Int64]134331680251836258
        executablePath = 'H:\Installed\generalsv.exe'
        executableSha256 = ('A' * 64); exitProven = $true
    }
    $state.Clear()
    foreach ($view in @('Registry32', 'Registry64')) {
        $state["$view|$installKey"] = @{ values = @{ InstallPath = @{
            value = "H:\Task\TwoBefore-$view"; kind = [int]$kind.String
        } } }
    }
    $twoSnapshots = @(
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry32 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'H:\Task\TwoBefore-Registry32' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\TwoRuntime' -ExpectedKind $kind.String),
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry64 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'H:\Task\TwoBefore-Registry64' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\TwoRuntime' -ExpectedKind $kind.String)
    )
    $twoIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots $twoSnapshots
    New-Stage5RegistryRecoveryJournal -Path $twoIdentity.journalPath `
        -Identity $twoIdentity -PlannedMissingSubKeys @() -Snapshots $twoSnapshots `
        -ProcessIdentities @($authorizationProcessIdentity, $secondAuthorizationProcessIdentity) | Out-Null
    $twoAuthorization = [ordered]@{
        childExitProven = $true; noActiveTitleProcesses = $true
        processIdentities = @(
            [ordered]@{
                launchPending = $false; processId = 58460
                creationTimeUtc100ns = [Int64]134331680251836257
                executablePath = 'H:\Installed\generalsv.exe'
                executableSha256 = ('A' * 64); exitProven = $true
            },
            [ordered]@{
                launchPending = $false; processId = 58461
                creationTimeUtc100ns = [Int64]134331680251836258
                executablePath = 'H:\Installed\generalsv.exe'
                executableSha256 = ('A' * 64); exitProven = $true
            }
        )
    }
    $twoRestored = Invoke-Stage5RegistryRecovery -Path $twoIdentity.journalPath `
        -ExpectedIdentity $twoIdentity -Authorization $twoAuthorization `
        -MutexLock $mutexLock -Adapter $adapter
    $twoValue32 = & $adapter['GetValue'] 'Registry32' $installKey 'InstallPath'
    $twoValue64 = & $adapter['GetValue'] 'Registry64' $installKey 'InstallPath'
    Assert-RecoveryTest ($twoRestored.state -ceq 'restored' -and
        [string]$twoValue32.value -ceq 'H:\Task\TwoBefore-Registry32' -and
        [string]$twoValue64.value -ceq 'H:\Task\TwoBefore-Registry64') `
        'Two-process recovery authorization did not preserve both complete identities.'

    $diagnosticRoot = Join-Path $root 'diagnostic'
    New-Item -ItemType Directory -Path $diagnosticRoot -Force | Out-Null
    $diagnosticIdentity = Copy-RecoveryIdentity $identity
    $diagnosticIdentity.identityMode = 'diagnostic'
    $diagnosticIdentity.taskRoot = $diagnosticRoot
    $diagnosticIdentity.journalPath = Join-Path $diagnosticRoot 'Stage5RegistryRecovery.json'
    $diagnosticIdentity.Remove('sourceCommit')
    $diagnosticIdentity.Remove('artifactSetSha256')
    $diagnosticJournal = New-Stage5RegistryRecoveryJournal `
        -Path $diagnosticIdentity.journalPath -Identity $diagnosticIdentity `
        -PlannedMissingSubKeys @() -Snapshots $snapshots `
        -ProcessIdentity $processIdentity
    Assert-RecoveryTest ($diagnosticJournal.identity.identityMode -ceq 'diagnostic' -and
        -not $diagnosticJournal.identity.Contains('sourceCommit') -and
        -not $diagnosticJournal.identity.Contains('artifactSetSha256')) `
        'Diagnostic journal carried acceptance-only source/artifact claims.'
    Read-Stage5RegistryRecoveryJournal -Path $diagnosticIdentity.journalPath `
        -ExpectedIdentity $diagnosticIdentity | Out-Null
    $badDiagnosticRoot = Join-Path $root 'diagnostic-with-claim'
    New-Item -ItemType Directory -Path $badDiagnosticRoot -Force | Out-Null
    $badDiagnosticIdentity = Copy-RecoveryIdentity $diagnosticIdentity
    $badDiagnosticIdentity.taskRoot = $badDiagnosticRoot
    $badDiagnosticIdentity.journalPath = Join-Path $badDiagnosticRoot 'Stage5RegistryRecovery.json'
    $badDiagnosticIdentity.sourceCommit = ('a' * 40)
    Assert-RecoveryThrows {
        New-Stage5RegistryRecoveryJournal -Path $badDiagnosticIdentity.journalPath `
            -Identity $badDiagnosticIdentity -PlannedMissingSubKeys @() `
            -Snapshots $snapshots -ProcessIdentity $processIdentity | Out-Null
    } 'Diagnostic recovery accepted an acceptance-only source claim.'

    $emptyPlanRoot = Join-Path $root 'empty-plan-with-foreign-key'
    New-Item -ItemType Directory -Path $emptyPlanRoot -Force | Out-Null
    $emptyPlanIdentity = Copy-RecoveryIdentity $identity
    $emptyPlanIdentity.taskRoot = $emptyPlanRoot
    $emptyPlanIdentity.journalPath = Join-Path $emptyPlanRoot 'Stage5RegistryRecovery.json'
    $emptyPlanIdentity.snapshotPlanSha256 = ('D' * 64)
    Assert-RecoveryThrows {
        New-Stage5RegistryRecoveryJournal -Path $emptyPlanIdentity.journalPath `
            -Identity $emptyPlanIdentity -PlannedMissingSubKeys @('Registry32|Software') `
            -Snapshots @() -ProcessIdentity $processIdentity | Out-Null
    } 'Empty snapshot plan accepted a foreign-created planned key.'

    $invalidProcessRoot = Join-Path $root 'invalid-zero-process-identity'
    New-Item -ItemType Directory -Path $invalidProcessRoot -Force | Out-Null
    $invalidProcessIdentity = Copy-RecoveryIdentity $identity
    $invalidProcessIdentity.taskRoot = $invalidProcessRoot
    $invalidProcessIdentity.journalPath = Join-Path $invalidProcessRoot 'Stage5RegistryRecovery.json'
    Assert-RecoveryThrows {
        New-Stage5RegistryRecoveryJournal -Path $invalidProcessIdentity.journalPath `
            -Identity $invalidProcessIdentity -PlannedMissingSubKeys @() `
            -Snapshots $snapshots -ProcessIdentities @([ordered]@{
                launchPending = $false; processId = 0; creationTimeUtc100ns = 0
                executablePath = $processIdentity.executablePath
                executableSha256 = $processIdentity.executableSha256
            }) | Out-Null
    } 'Recovery accepted a non-pending zero-PID process identity.'

    $pendingRoot = Join-Path $root 'launch-pending'
    New-Item -ItemType Directory -Path $pendingRoot -Force | Out-Null
    $pendingIdentity = Copy-RecoveryIdentity $identity
    $pendingIdentity.taskRoot = $pendingRoot
    $pendingIdentity.journalPath = Join-Path $pendingRoot 'Stage5RegistryRecovery.json'
    $pendingProcess = [ordered]@{
        launchPending = $true; processId = 0; creationTimeUtc100ns = 0
        executablePath = $processIdentity.executablePath
        executableSha256 = $processIdentity.executableSha256
    }
    New-Stage5RegistryRecoveryJournal -Path $pendingIdentity.journalPath `
        -Identity $pendingIdentity -PlannedMissingSubKeys @() `
        -Snapshots $snapshots -ProcessIdentities @($pendingProcess) | Out-Null
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $pendingIdentity.journalPath `
            -ExpectedIdentity $pendingIdentity `
            -Authorization ([ordered]@{
                childExitProven = $false; noActiveTitleProcesses = $false
                processIdentities = @([ordered]@{
                    launchPending = $true; processId = 0; creationTimeUtc100ns = 0
                    executablePath = $processIdentity.executablePath
                    executableSha256 = $processIdentity.executableSha256
                    exitProven = $false
                })
            }) -MutexLock $mutexLock -Adapter $adapter | Out-Null
    } 'Recovery accepted unresolved launch-pending process identity.'

    $tamperExpectedRoot = Join-Path $root 'tamper-expected'
    New-Item -ItemType Directory -Path $tamperExpectedRoot -Force | Out-Null
    $tamperExpectedIdentity = Copy-RecoveryIdentity $identity
    $tamperExpectedIdentity.taskRoot = $tamperExpectedRoot
    $tamperExpectedIdentity.journalPath = Join-Path $tamperExpectedRoot 'Stage5RegistryRecovery.json'
    New-Stage5RegistryRecoveryJournal -Path $tamperExpectedIdentity.journalPath `
        -Identity $tamperExpectedIdentity -PlannedMissingSubKeys @() `
        -Snapshots $snapshots -ProcessIdentity $processIdentity | Out-Null
    $tamperedExpected = Get-Content -LiteralPath $tamperExpectedIdentity.journalPath -Raw | ConvertFrom-Json
    $tamperedExpected.snapshots[0].expectedValue.value = 'H:\Tampered'
    $tamperedExpected | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $tamperExpectedIdentity.journalPath
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $tamperExpectedIdentity.journalPath `
            -ExpectedIdentity $tamperExpectedIdentity -Authorization $authorization `
            -MutexLock $mutexLock -Adapter $adapter | Out-Null
    } 'Recovery accepted on-disk expectedValue tampering.'

    $tamperOldRoot = Join-Path $root 'tamper-old'
    New-Item -ItemType Directory -Path $tamperOldRoot -Force | Out-Null
    $tamperOldIdentity = Copy-RecoveryIdentity $identity
    $tamperOldIdentity.taskRoot = $tamperOldRoot
    $tamperOldIdentity.journalPath = Join-Path $tamperOldRoot 'Stage5RegistryRecovery.json'
    New-Stage5RegistryRecoveryJournal -Path $tamperOldIdentity.journalPath `
        -Identity $tamperOldIdentity -PlannedMissingSubKeys @() `
        -Snapshots $snapshots -ProcessIdentity $processIdentity | Out-Null
    $tamperedOld = Get-Content -LiteralPath $tamperOldIdentity.journalPath -Raw | ConvertFrom-Json
    $tamperedOld.snapshots[0].oldValue.value = 'tampered-old'
    $tamperedOld | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $tamperOldIdentity.journalPath
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $tamperOldIdentity.journalPath `
            -ExpectedIdentity $tamperOldIdentity -Authorization $authorization `
            -MutexLock $mutexLock -Adapter $adapter | Out-Null
    } 'Recovery accepted on-disk oldValue tampering.'

    # HKCU title InstallPath may be one physical unlisted descendant exposed
    # through both registry views. The reverse restore must write that physical
    # value once, then accept the other view's exact old-state readback without
    # weakening the ownership check.
    $sharedRoot = Join-Path $root 'shared-alias'
    New-Item -ItemType Directory -Path $sharedRoot -Force | Out-Null
    $sharedIdentity = Copy-RecoveryIdentity $identity
    $sharedIdentity.taskRoot = $sharedRoot
    $sharedIdentity.journalPath = Join-Path $sharedRoot 'Stage5RegistryRecovery.json'
    $sharedEntry = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $state["Registry32|$installKey"] = $sharedEntry
    $state["Registry64|$installKey"] = $sharedEntry
    $sharedSnapshots = @(
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry32 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before-shared' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String),
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry64 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before-shared' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String)
    )
    $sharedIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots $sharedSnapshots
    New-Stage5RegistryRecoveryJournal -Path $sharedIdentity.journalPath `
        -Identity $sharedIdentity -PlannedMissingSubKeys @() `
        -Snapshots $sharedSnapshots -ProcessIdentity $processIdentity | Out-Null
    $sharedRestored = Invoke-Stage5RegistryRecovery -Path $sharedIdentity.journalPath `
        -ExpectedIdentity $sharedIdentity -Authorization $authorization `
        -MutexLock ([pscustomobject]@{ acquired = $true; name = $sharedIdentity.mutexName }) `
        -Adapter $adapter
    foreach ($view in @('Registry32', 'Registry64')) {
        $sharedValue = & $adapter['GetValue'] $view $installKey 'InstallPath'
        Assert-RecoveryTest ([string]$sharedValue.value -ceq 'before-shared' -and
            [int]$sharedValue.kind -eq [int]$kind.String) `
            "Shared registry alias did not restore through $view."
    }
    Assert-RecoveryTest ($sharedRestored.state -ceq 'restored') `
        'Shared registry alias recovery did not complete.'

    $forgedRoot = Join-Path $root 'forged'
    New-Item -ItemType Directory -Path $forgedRoot -Force | Out-Null
    $forgedIdentity = Copy-RecoveryIdentity $identity
    $forgedIdentity.taskRoot = $forgedRoot
    $forgedIdentity.journalPath = Join-Path $forgedRoot 'Stage5RegistryRecovery.json'
    $forged = Get-Content -LiteralPath $identity.journalPath -Raw | ConvertFrom-Json
    $forged.identity.taskRoot = $forgedRoot
    $forged.identity.journalPath = $forgedIdentity.journalPath
    $forged.snapshots[0].subKey = 'Software\Foreign\InstallPath'
    $forged | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $forgedIdentity.journalPath
    Assert-RecoveryThrows {
        Read-Stage5RegistryRecoveryJournal -Path $forgedIdentity.journalPath `
            -ExpectedIdentity $forgedIdentity | Out-Null
    } 'Recovery accepted a journal redirected to an arbitrary registry key.'

    $createdRoot = Join-Path $root 'created'
    # This scenario creates the complete ancestor chain; earlier scenarios'
    # Generals sibling is unrelated state and must not enter this fresh fixture.
    $state.Clear()
    New-Item -ItemType Directory -Path $createdRoot -Force | Out-Null
    $createdIdentity = Copy-RecoveryIdentity $identity
    $createdIdentity.title = 'ZeroHour'
    $createdIdentity.taskRoot = $createdRoot
    $createdIdentity.journalPath = Join-Path $createdRoot 'Stage5RegistryRecovery.json'
    $createdKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
    $createdAncestors = @('Software', 'Software\Electronic Arts',
        'Software\Electronic Arts\EA Games', $createdKey)
    foreach ($path in $createdAncestors) {
        $state["Registry32|$path"] = @{ values = @{} }
    }
    $state["Registry32|$createdKey"].values['InstallPath'] = @{
        value = 'H:\Task\ZeroHour'; kind = [int]$kind.String
    }
    $createdSnapshot = New-Stage5RegistryRecoverySnapshot -Title ZeroHour `
        -View Registry32 -SubKey $createdKey -Name InstallPath -HadKey $false `
        -HadValue $false -ExpectedValue 'H:\Task\ZeroHour' `
        -ExpectedKind $kind.String -CreatedSubKeys $createdAncestors
    $createdPlanned = @($createdAncestors | ForEach-Object { "Registry32|$_" })
    $createdIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ZeroHour -PlannedMissingSubKeys $createdPlanned `
        -Snapshots @($createdSnapshot)
    $state["Registry64|$createdKey"] = @{ values = @{} }
    New-Stage5RegistryRecoveryJournal -Path $createdIdentity.journalPath `
        -Identity $createdIdentity -PlannedMissingSubKeys $createdPlanned `
        -Snapshots @($createdSnapshot) -ProcessIdentity $processIdentity | Out-Null
    Invoke-Stage5RegistryRecovery -Path $createdIdentity.journalPath `
        -ExpectedIdentity $createdIdentity -Authorization $authorization `
        -MutexLock ([pscustomobject]@{ acquired = $true; name = $createdIdentity.mutexName }) `
        -Adapter $adapter | Out-Null
    foreach ($path in $createdAncestors) {
        Assert-RecoveryTest (-not $state.ContainsKey("Registry32|$path")) `
            "Created key remained after restore: $path"
    }
    Assert-RecoveryTest ($state.ContainsKey("Registry64|$createdKey")) `
        'Recovery touched an asymmetric pre-existing key in the other registry view.'

    $dualRoot = Join-Path $root 'created-both-views'
    New-Item -ItemType Directory -Path $dualRoot -Force | Out-Null
    $dualIdentity = Copy-RecoveryIdentity $createdIdentity
    $dualIdentity.taskRoot = $dualRoot
    $dualIdentity.journalPath = Join-Path $dualRoot 'Stage5RegistryRecovery.json'
    $state.Clear()
    $dualSnapshots = @()
    $dualPlanned = @()
    foreach ($view in @('Registry32', 'Registry64')) {
        foreach ($path in $createdAncestors) { $state["$view|$path"] = @{ values = @{} } }
        $state["$view|$createdKey"].values['InstallPath'] = @{
            value = 'H:\Task\ZeroHour'; kind = [int]$kind.String
        }
        $dualSnapshots += New-Stage5RegistryRecoverySnapshot -Title ZeroHour `
            -View $view -SubKey $createdKey -Name InstallPath -HadKey $false `
            -HadValue $false -ExpectedValue 'H:\Task\ZeroHour' `
            -ExpectedKind $kind.String -CreatedSubKeys $createdAncestors
        $dualPlanned += @($createdAncestors | ForEach-Object { "$view|$_" })
    }
    $dualIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ZeroHour -PlannedMissingSubKeys $dualPlanned -Snapshots $dualSnapshots
    New-Stage5RegistryRecoveryJournal -Path $dualIdentity.journalPath `
        -Identity $dualIdentity -PlannedMissingSubKeys $dualPlanned `
        -Snapshots $dualSnapshots -ProcessIdentity $processIdentity | Out-Null
    Invoke-Stage5RegistryRecovery -Path $dualIdentity.journalPath `
        -ExpectedIdentity $dualIdentity -Authorization $authorization `
        -MutexLock $mutexLock -Adapter $adapter | Out-Null
    Assert-RecoveryTest ($state.Count -eq 0) `
        'Equal-depth created keys in both registry views were not all removed.'

    $intervenedRoot = Join-Path $root 'intervened'
    New-Item -ItemType Directory -Path $intervenedRoot -Force | Out-Null
    $intervenedIdentity = Copy-RecoveryIdentity $identity
    $intervenedIdentity.taskRoot = $intervenedRoot
    $intervenedIdentity.journalPath = Join-Path $intervenedRoot 'Stage5RegistryRecovery.json'
    $state["Registry32|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $state["Registry64|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $intervenedSnapshots = @(
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry32 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before32' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String),
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry64 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before64' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String)
    )
    $intervenedIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots $intervenedSnapshots
    New-Stage5RegistryRecoveryJournal -Path $intervenedIdentity.journalPath `
        -Identity $intervenedIdentity -PlannedMissingSubKeys @() `
        -Snapshots $intervenedSnapshots -ProcessIdentity $processIdentity | Out-Null
    $writeState = [pscustomobject]@{ count = 0 }
    $interveningAdapter = [ordered]@{}
    foreach ($name in @('GetValue', 'GetKey', 'DeleteValue', 'DeleteKey')) {
        $interveningAdapter[$name] = $adapter[$name]
    }
    $interveningAdapter['SetValue'] = {
        param($View, $SubKey, $Name, $Value, $Kind)
        & $baseFixtureRegistryAdapter['SetValue'] $View $SubKey $Name $Value $Kind
        ++$writeState.count
        if ($writeState.count -eq 1) {
            $state["Registry32|$installKey"].values['InstallPath'] = @{
                value = 'H:\Intervening\Owner'; kind = [int][Microsoft.Win32.RegistryValueKind]::String
            }
        }
    }
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $intervenedIdentity.journalPath `
            -ExpectedIdentity $intervenedIdentity -Authorization $authorization `
            -MutexLock $mutexLock -Adapter $interveningAdapter | Out-Null
    } 'Recovery failed to detect an intervening ownership change.'
    Assert-RecoveryTest ([string]$state["Registry32|$installKey"].values['InstallPath'].value -ceq
        'H:\Intervening\Owner') ("Recovery overwrote an intervening registry owner. Writes=$($writeState.count); rejection=$script:LastRecoveryRejection")

    $interruptedRoot = Join-Path $root 'interrupted-restore'
    New-Item -ItemType Directory -Path $interruptedRoot -Force | Out-Null
    $interruptedIdentity = Copy-RecoveryIdentity $identity
    $interruptedIdentity.taskRoot = $interruptedRoot
    $interruptedIdentity.journalPath = Join-Path $interruptedRoot 'Stage5RegistryRecovery.json'
    $state["Registry32|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $state["Registry64|$installKey"] = @{ values = @{ InstallPath = @{
        value = 'H:\Task\Generals'; kind = [int]$kind.String
    } } }
    $interruptedSnapshots = @(
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry32 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before-interrupted32' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String),
        (New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry64 `
            -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
            -OldValue 'before-interrupted64' -OldKind $kind.String `
            -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String)
    )
    $interruptedIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots $interruptedSnapshots
    New-Stage5RegistryRecoveryJournal -Path $interruptedIdentity.journalPath `
        -Identity $interruptedIdentity -PlannedMissingSubKeys @() `
        -Snapshots $interruptedSnapshots -ProcessIdentity $processIdentity | Out-Null
    $interruptWriteState = [pscustomobject]@{ count = 0 }
    $interruptingAdapter = [ordered]@{}
    foreach ($name in @('GetValue', 'GetKey', 'DeleteValue', 'DeleteKey')) {
        $interruptingAdapter[$name] = $adapter[$name]
    }
    $interruptingAdapter['SetValue'] = {
        param($View, $SubKey, $Name, $Value, $Kind)
        & $baseFixtureRegistryAdapter['SetValue'] $View $SubKey $Name $Value $Kind
        ++$interruptWriteState.count
        if ($interruptWriteState.count -eq 1) {
            throw 'injected restore interruption'
        }
    }
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $interruptedIdentity.journalPath `
            -ExpectedIdentity $interruptedIdentity -Authorization $authorization `
            -MutexLock $mutexLock -Adapter $interruptingAdapter | Out-Null
    } 'Interrupted restore did not retain a recoverable journal.'
    $resumed = Invoke-Stage5RegistryRecovery -Path $interruptedIdentity.journalPath `
        -ExpectedIdentity $interruptedIdentity -Authorization $authorization `
        -MutexLock $mutexLock -Adapter $adapter
    Assert-RecoveryTest ($resumed.state -ceq 'restored' -and
        [string]$state["Registry32|$installKey"].values['InstallPath'].value -ceq 'before-interrupted32' -and
        [string]$state["Registry64|$installKey"].values['InstallPath'].value -ceq 'before-interrupted64') `
        'Recovery did not resume from an already-restored prefix.'

    $mismatchRoot = Join-Path $root 'mismatch'
    New-Item -ItemType Directory -Path $mismatchRoot -Force | Out-Null
    $mismatchIdentity = Copy-RecoveryIdentity $identity
    $mismatchIdentity.taskRoot = $mismatchRoot
    $mismatchIdentity.journalPath = Join-Path $mismatchRoot 'Stage5RegistryRecovery.json'
    $state["Registry32|$installKey"].values['InstallPath'] = @{
        value = 'H:\Foreign\Runtime'; kind = [int]$kind.String
    }
    $mismatchSnapshot = New-Stage5RegistryRecoverySnapshot -Title Generals `
        -View Registry32 -SubKey $installKey -Name InstallPath -HadKey $true `
        -HadValue $true -OldValue 'before' -OldKind $kind.String `
        -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String
    $mismatchIdentity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title Generals -PlannedMissingSubKeys @() -Snapshots @($mismatchSnapshot)
    New-Stage5RegistryRecoveryJournal -Path $mismatchIdentity.journalPath `
        -Identity $mismatchIdentity -PlannedMissingSubKeys @() `
        -Snapshots @($mismatchSnapshot) -ProcessIdentity $processIdentity | Out-Null
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $mismatchIdentity.journalPath `
            -ExpectedIdentity $mismatchIdentity -Authorization $authorization `
            -MutexLock $mutexLock -Adapter $adapter | Out-Null
    } 'Recovery accepted a changed task-owned value.'
    Assert-RecoveryTest ((Get-Content -LiteralPath $mismatchIdentity.journalPath -Raw |
        ConvertFrom-Json).state -ceq 'registry-restoration-failed') `
        'Failed recovery did not retain a failure journal.'
    Assert-RecoveryThrows {
        Invoke-Stage5RegistryRecovery -Path $mismatchIdentity.journalPath `
            -ExpectedIdentity $mismatchIdentity `
            -Authorization ([ordered]@{ childExitProven = $false; noActiveTitleProcesses = $true; processIdentity = $processIdentity }) `
            -MutexLock $mutexLock `
            -Adapter $adapter | Out-Null
    } 'Recovery accepted missing child-exit proof.'
    # A Zero Hour child reads both title bindings. Plan both views before any
    # write, and recover the complete scope without granting arbitrary keys.
    $paired = Copy-RecoveryIdentity $identity
    $paired.title = 'ZeroHour'
    $paired.registryScope = 'ZeroHourWithGeneralsBase'
    $paired.journalPath = Join-Path $root 'PairedRegistryRecovery.json'
    $pairedSnapshots = @(
        foreach ($pairedTitle in @('Generals', 'ZeroHour')) {
            $pairedKey = if ($pairedTitle -ceq 'Generals') { $installKey } else {
                'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            }
            foreach ($pairedView in @('Registry32', 'Registry64')) {
                New-Stage5RegistryRecoverySnapshot -Title $pairedTitle -View $pairedView `
                    -SubKey $pairedKey -Name InstallPath -HadKey $true -HadValue $true `
                    -OldValue "old-$pairedTitle-$pairedView" -OldKind $kind.String `
                    -ExpectedValue "H:\Task\$pairedTitle" -ExpectedKind $kind.String
            }
        }
    )
    $paired.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ZeroHour -RegistryScope ZeroHourWithGeneralsBase `
        -Snapshots $pairedSnapshots -PlannedMissingSubKeys @()
    $pairedDocument = New-Stage5RegistryRecoveryJournal -Path $paired.journalPath `
        -Identity $paired -Snapshots $pairedSnapshots -PlannedMissingSubKeys @() `
        -ProcessIdentities @()
    Assert-RecoveryTest ($pairedDocument.schemaVersion -eq 2 -and
        $pairedDocument.snapshots.Count -eq 4) 'Paired journal must bind exactly four snapshots.'
    Assert-RecoveryThrows {
        Get-Stage5RegistryRecoverySnapshotPlanSha256 -Title ZeroHour `
            -RegistryScope ZeroHourWithGeneralsBase -Snapshots @($pairedSnapshots[0])
    } 'Paired scope accepted an incomplete pre-write snapshot plan.'
    Assert-RecoveryThrows {
        Get-Stage5RegistryRecoverySnapshotPlanSha256 -Title ZeroHour -Snapshots $pairedSnapshots
    } 'Legacy single-title scope accepted a paired plan.'
    $wrongScope = Copy-RecoveryIdentity $paired
    $wrongScope.Remove('registryScope')
    Assert-RecoveryThrows {
        Read-Stage5RegistryRecoveryJournal $paired.journalPath $wrongScope
    } 'Paired journal was readable without its explicit scope binding.'
    $state.Clear()
    foreach ($snapshot in $pairedSnapshots) {
        $state["$($snapshot.view)|$($snapshot.subKey)"] = @{ values = @{
            InstallPath = @{ value = "H:\Task\$(if ($snapshot.subKey -ceq $installKey) {'Generals'} else {'ZeroHour'})";
                kind = [int]$kind.String }
        } }
    }
    Invoke-Stage5RegistryRecovery -Path $paired.journalPath -ExpectedIdentity $paired `
        -Authorization ([ordered]@{ childExitProven = $true; noActiveTitleProcesses = $true; processIdentities = @() }) `
        -MutexLock $mutexLock -Adapter $baseFixtureRegistryAdapter | Out-Null
    foreach ($snapshot in $pairedSnapshots) {
        $restoredPair = & $baseFixtureRegistryAdapter.GetValue $snapshot.view $snapshot.subKey InstallPath
        Assert-RecoveryTest ($restoredPair.value -ceq $snapshot.oldValue.value) `
            'Paired recovery did not restore an exact original title/view value.'
    }
    $plannedWrite = $pairedSnapshots[0]
    $originalState = & $baseFixtureRegistryAdapter.GetValue $plannedWrite.view $plannedWrite.subKey InstallPath
    Assert-Stage5RegistryRecoveryWriteState -Snapshot $plannedWrite -CurrentKeyExists $true `
        -CurrentValue $originalState -WrittenSnapshots @()
    Assert-RecoveryThrows {
        Assert-Stage5RegistryRecoveryWriteState -Snapshot $plannedWrite -CurrentKeyExists $true `
            -CurrentValue ([pscustomobject]@{ exists=$true; value='foreign-after-plan'; kind=$kind.String }) `
            -WrittenSnapshots @()
    } 'Setup accepted a concurrent change after the journal plan was frozen.'
    Assert-RecoveryThrows {
        Assert-Stage5RegistryRecoveryWriteState -Snapshot $plannedWrite -CurrentKeyExists $true `
            -CurrentValue ([pscustomobject]@{ exists=$true; value='H:\Task\Generals'; kind=$kind.String }) `
            -WrittenSnapshots @()
    } 'An expected value without an owned prior write was treated as an alias.'
    $aliasWrite = New-Stage5RegistryRecoverySnapshot -Title Generals -View Registry64 `
        -SubKey $installKey -Name InstallPath -HadKey $true -HadValue $true `
        -OldValue $plannedWrite.oldValue.value -OldKind $kind.String `
        -ExpectedValue 'H:\Task\Generals' -ExpectedKind $kind.String
    Assert-Stage5RegistryRecoveryWriteState -Snapshot $aliasWrite -CurrentKeyExists $true `
        -CurrentValue ([pscustomobject]@{exists=$true;value='H:\Task\Generals';kind=$kind.String}) `
        -WrittenSnapshots @($plannedWrite)
    Assert-RecoveryThrows {
        Assert-Stage5RegistryRecoveryWriteState -Snapshot $aliasWrite -CurrentKeyExists $true `
            -CurrentValue ([pscustomobject]@{exists=$true;value='H:\Task\Generals';kind=$kind.ExpandString}) `
            -WrittenSnapshots @($plannedWrite)
    } 'Aliased setup accepted a concurrent registry kind change.'
    $partial = Copy-RecoveryIdentity $paired
    $partial.journalPath = Join-Path $root 'PartialPair.json'
    New-Stage5RegistryRecoveryJournal -Path $partial.journalPath -Identity $partial `
        -Snapshots $pairedSnapshots -PlannedMissingSubKeys @() -ProcessIdentities @() | Out-Null
    & $baseFixtureRegistryAdapter.SetValue $plannedWrite.view $plannedWrite.subKey InstallPath `
        'H:\Task\Generals' $kind.String
    Invoke-Stage5RegistryRecovery -Path $partial.journalPath -ExpectedIdentity $partial `
        -Authorization ([ordered]@{childExitProven=$true;noActiveTitleProcesses=$true;processIdentities=@()}) `
        -MutexLock $mutexLock -Adapter $baseFixtureRegistryAdapter | Out-Null
    foreach ($snapshot in $pairedSnapshots) {
        Assert-RecoveryTest ((& $baseFixtureRegistryAdapter.GetValue $snapshot.view $snapshot.subKey InstallPath).value -ceq
            $snapshot.oldValue.value) 'Crash before later paired writes did not preserve original values.'
    }
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Output 'Stage 5 registry recovery synthetic tests passed.'
