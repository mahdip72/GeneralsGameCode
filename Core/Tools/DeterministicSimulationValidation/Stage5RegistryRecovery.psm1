Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$evidenceModule = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
if (-not (Get-Module -Name DeterministicSimulationEvidence)) {
    Import-Module $evidenceModule -Force
}

$script:Stage5RegistryRecoverySchemaVersion = 1
$script:Stage5RegistryRecoveryKinds = @{
    String = [int][Microsoft.Win32.RegistryValueKind]::String
    ExpandString = [int][Microsoft.Win32.RegistryValueKind]::ExpandString
    Binary = [int][Microsoft.Win32.RegistryValueKind]::Binary
    DWord = [int][Microsoft.Win32.RegistryValueKind]::DWord
    MultiString = [int][Microsoft.Win32.RegistryValueKind]::MultiString
    QWord = [int][Microsoft.Win32.RegistryValueKind]::QWord
}

function Get-Stage5RecoveryProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    if ($null -eq $Object) { throw "$Context is missing." }
    if ($Object -is [Collections.IDictionary]) {
        if (-not $Object.Contains($Name)) { throw "$Context is missing '$Name'." }
        return ,$Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { throw "$Context is missing '$Name'." }
    return ,$property.Value
}

function Test-Stage5RecoveryProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $false }
    if ($Object -is [Collections.IDictionary]) { return $Object.Contains($Name) }
    return $null -ne $Object.PSObject.Properties[$Name]
}

function Assert-Stage5RecoveryAllowedProperties {
    param([object]$Object, [string[]]$Allowed, [string]$Context)
    $names = if ($Object -is [Collections.IDictionary]) {
        @($Object.Keys | ForEach-Object { [string]$_ })
    } else { @($Object.PSObject.Properties.Name) }
    foreach ($name in $names) {
        Assert-Stage5RecoveryCondition ($Allowed -contains $name) `
            "$Context contains unsupported field '$name'."
    }
}

function Assert-Stage5RecoveryCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Stage5RecoveryFullPath {
    param([string]$Path, [string]$Context)
    Assert-Stage5RecoveryCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is required."
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    Assert-Stage5RecoveryCondition ($full -notmatch '(^|[\/])\.\.?([\/]|$)') `
        "$Context path must not contain dot segments: $full"
    return $full
}

function Assert-Stage5RecoveryTaskRoot {
    param([string]$TaskRoot)
    $root = Get-Stage5RecoveryFullPath $TaskRoot 'Recovery task root'
    Assert-Stage5RecoveryCondition (
        [IO.Path]::GetPathRoot($root).Equals('H:\',
            [StringComparison]::OrdinalIgnoreCase)) `
        "Recovery task root must be below H:\: $root"
    Assert-Stage5RecoveryCondition ($root -ne 'H:') `
        'Recovery task root must not be the volume root.'
    Assert-Stage5RecoveryCondition (Test-Path -LiteralPath $root -PathType Container) `
        "Recovery task root does not exist: $root"
    $item = Get-Item -LiteralPath $root -Force
    Assert-Stage5RecoveryCondition (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Recovery task root is a reparse point: $root"
    return $root
}

function Assert-Stage5RecoveryContainedPath {
    param([string]$TaskRoot, [string]$Path, [string]$Context)
    $root = Assert-Stage5RecoveryTaskRoot $TaskRoot
    $full = Get-Stage5RecoveryFullPath $Path $Context
    Assert-Stage5RecoveryCondition ($full.StartsWith($root + '\',
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context escapes the recovery task root: $full"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        Assert-Stage5RecoveryCondition (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context is a reparse point: $full"
    }
    return $full
}

function Assert-Stage5RecoveryUuid {
    param([string]$Value, [string]$Context)
    Assert-Stage5RecoveryCondition ($Value -cmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$') `
        "$Context must be a lowercase UUIDv4."
}

function Get-Stage5RegistryRecoveryMutexName {
    param([Parameter(Mandatory = $true)][string]$UserSid)
    Assert-Stage5RecoveryCondition ($UserSid -cmatch
        '^S-\d-(?:\d+-){1,14}\d+$') `
        'UserSid must be an explicit Windows SID.'
    return "Global\GeneralsGameCode.Stage5PerformanceValidation-$UserSid"
}

function Enter-Stage5RegistryRecoveryMutex {
    param([Parameter(Mandatory = $true)][string]$UserSid)
    $name = Get-Stage5RegistryRecoveryMutexName $UserSid
    [bool]$createdNew = $false
    $mutex = New-Object Threading.Mutex($false, $name, [ref]$createdNew)
    $abandoned = $false
    try {
        try {
            if (-not $mutex.WaitOne(0)) {
                throw "Stage 5 registry recovery mutex is already held: $name"
            }
        }
        catch [Threading.AbandonedMutexException] {
            # Ownership is acquired, but an abandoned mutex is never treated as
            # permission to infer registry state. The caller must read and
            # explicitly authorize the journal recovery path.
            $abandoned = $true
        }
        return [pscustomobject]@{
            name = $name; mutex = $mutex; acquired = $true
            abandoned = $abandoned
        }
    }
    catch {
        $mutex.Dispose()
        throw
    }
}

function Exit-Stage5RegistryRecoveryMutex {
    param([AllowNull()][object]$Lock)
    if ($null -eq $Lock) { return }
    $errors = New-Object 'Collections.Generic.List[string]'
    if ([bool]$Lock.acquired) {
        try { $Lock.mutex.ReleaseMutex() }
        catch { $errors.Add("release: $($_.Exception.Message)") | Out-Null }
    }
    try { $Lock.mutex.Dispose() }
    catch { $errors.Add("dispose: $($_.Exception.Message)") | Out-Null }
    if ($errors.Count -gt 0) {
        throw "Stage 5 registry recovery mutex cleanup failed: $($errors -join ' | ')"
    }
}

function ConvertTo-Stage5RecoveryKind {
    param([object]$Kind, [string]$Context)
    Assert-Stage5RecoveryCondition ($null -ne $Kind) "$Context kind is required."
    [int]$value = -1
    if ($Kind -is [string]) {
        $parsed = [Microsoft.Win32.RegistryValueKind]::Unknown
        if ([Enum]::TryParse($Kind, $true, [ref]$parsed)) {
            $value = [int]$parsed
        }
        elseif (-not [int]::TryParse($Kind, [ref]$value)) {
            throw "$Context kind is unsupported: $Kind"
        }
    }
    else { $value = [int]$Kind }
    Assert-Stage5RecoveryCondition ($script:Stage5RegistryRecoveryKinds.Values -contains $value) `
        "$Context kind is unsupported: $value"
    return $value
}

function Get-Stage5RecoveryKindName {
    param([int]$Kind, [string]$Context)
    $value = ConvertTo-Stage5RecoveryKind $Kind $Context
    return [Enum]::GetName([Microsoft.Win32.RegistryValueKind], $value)
}

function ConvertTo-Stage5RecoveryEncodedValue {
    param([AllowNull()][object]$Value, [object]$Kind, [string]$Context)
    $kindValue = ConvertTo-Stage5RecoveryKind $Kind "$Context kind"
    $kindName = Get-Stage5RecoveryKindName $kindValue "$Context kind"
    $encoded = switch ($kindName) {
        'String' { [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'string'; value = [string]$Value } }
        'ExpandString' { [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'string'; value = [string]$Value } }
        'MultiString' {
            [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'string-array'; value = @($Value | ForEach-Object { [string]$_ }) }
        }
        'Binary' {
            [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'base64'; value = [Convert]::ToBase64String([byte[]]$Value) }
        }
        'DWord' {
            [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'uint32-decimal'; value = ([UInt32]$Value).ToString([Globalization.CultureInfo]::InvariantCulture) }
        }
        'QWord' {
            [ordered]@{ kind = $kindValue; type = $kindName; encoding = 'int64-decimal'; value = ([Int64]$Value).ToString([Globalization.CultureInfo]::InvariantCulture) }
        }
        default { throw "$Context value kind is unsupported: $kindName" }
    }
    Assert-Stage5RecoveryEncodedValue $encoded $Context | Out-Null
    return $encoded
}

function Get-Stage5RecoveryEncodedData {
    param([object]$Encoded, [string]$Context)
    # Preserve REG_MULTI_SZ cardinality: an empty or one-element array must
    # not become null or a scalar while crossing a PowerShell function boundary.
    [void](Get-Stage5RecoveryProperty $Encoded 'value' $Context)
    if ($Encoded -is [Collections.IDictionary]) { return ,$Encoded['value'] }
    return ,$Encoded.PSObject.Properties['value'].Value
}

function Assert-Stage5RecoveryEncodedValue {
    param([AllowNull()][object]$Encoded, [string]$Context)
    Assert-Stage5RecoveryCondition ($null -ne $Encoded) "$Context is required."
    Assert-Stage5RecoveryAllowedProperties $Encoded @('kind', 'type', 'encoding', 'value') $Context
    $kind = ConvertTo-Stage5RecoveryKind `
        (Get-Stage5RecoveryProperty $Encoded 'kind' $Context) "$Context kind"
    $type = [string](Get-Stage5RecoveryProperty $Encoded 'type' $Context)
    $expectedType = Get-Stage5RecoveryKindName $kind "$Context kind"
    Assert-Stage5RecoveryCondition ($type -ceq $expectedType) `
        "$Context type does not match kind: $type/$expectedType"
    $encoding = [string](Get-Stage5RecoveryProperty $Encoded 'encoding' $Context)
    $value = Get-Stage5RecoveryEncodedData $Encoded $Context
    switch ($type) {
        'String' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'string' -and $value -is [string]) `
                "$Context must be a REG_SZ string encoding."
        }
        'ExpandString' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'string' -and $value -is [string]) `
                "$Context must be a REG_EXPAND_SZ string encoding."
        }
        'MultiString' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'string-array' -and $value -is [Array]) `
                "$Context must be a REG_MULTI_SZ string-array encoding."
            foreach ($item in @($value)) {
                Assert-Stage5RecoveryCondition ($item -is [string]) `
                    "$Context contains a non-string REG_MULTI_SZ element."
            }
        }
        'Binary' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'base64' -and $value -is [string]) `
                "$Context must be a REG_BINARY base64 encoding."
            try { $bytes = [Convert]::FromBase64String([string]$value) }
            catch { throw "$Context contains invalid base64: $($_.Exception.Message)" }
            Assert-Stage5RecoveryCondition ([Convert]::ToBase64String($bytes) -ceq [string]$value) `
                "$Context base64 is not canonical."
        }
        'DWord' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'uint32-decimal' -and $value -is [string]) `
                "$Context must be a REG_DWORD decimal encoding."
            try { $parsed = [UInt32]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture) }
            catch { throw "$Context contains an invalid REG_DWORD: $($_.Exception.Message)" }
            Assert-Stage5RecoveryCondition ($parsed.ToString([Globalization.CultureInfo]::InvariantCulture) -ceq [string]$value) `
                "$Context REG_DWORD is not canonical decimal."
        }
        'QWord' {
            Assert-Stage5RecoveryCondition ($encoding -ceq 'int64-decimal' -and $value -is [string]) `
                "$Context must be a REG_QWORD decimal encoding."
            try { $parsed = [Int64]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture) }
            catch { throw "$Context contains an invalid REG_QWORD: $($_.Exception.Message)" }
            Assert-Stage5RecoveryCondition ($parsed.ToString([Globalization.CultureInfo]::InvariantCulture) -ceq [string]$value) `
                "$Context REG_QWORD is not canonical decimal."
        }
        default { throw "$Context contains unsupported value type '$type'." }
    }
    return $true
}

function ConvertFrom-Stage5RecoveryEncodedValue {
    param([AllowNull()][object]$Encoded, [string]$Context)
    if ($null -eq $Encoded) { return $null }
    Assert-Stage5RecoveryEncodedValue $Encoded $Context | Out-Null
    $type = [string](Get-Stage5RecoveryProperty $Encoded 'type' $Context)
    $value = Get-Stage5RecoveryEncodedData $Encoded $Context
    switch ($type) {
        'String' { return [string]$value }
        'ExpandString' { return [string]$value }
        'MultiString' { return ,([string[]]@($value | ForEach-Object { [string]$_ })) }
        'Binary' { return ,([Convert]::FromBase64String([string]$value)) }
        'DWord' { return [UInt32]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture) }
        'QWord' { return [Int64]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture) }
        default { throw "$Context contains unsupported value type '$type'." }
    }
}

function Test-Stage5RecoveryEncodedValueEqual {
    param([AllowNull()][object]$Left, [AllowNull()][object]$Right, [string]$Context)
    if ($null -eq $Left -or $null -eq $Right) { return $null -eq $Left -and $null -eq $Right }
    if ([int](Get-Stage5RecoveryProperty $Left 'kind' $Context) -ne
        [int](Get-Stage5RecoveryProperty $Right 'kind' $Context)) { return $false }
    if ([string](Get-Stage5RecoveryProperty $Left 'type' $Context) -cne
        [string](Get-Stage5RecoveryProperty $Right 'type' $Context)) { return $false }
    if ([string](Get-Stage5RecoveryProperty $Left 'encoding' $Context) -cne
        [string](Get-Stage5RecoveryProperty $Right 'encoding' $Context)) { return $false }
    $leftValue = Get-Stage5RecoveryEncodedData $Left $Context
    $rightValue = Get-Stage5RecoveryEncodedData $Right $Context
    if ($leftValue -is [Array] -or $rightValue -is [Array]) {
        $leftArray = @($leftValue); $rightArray = @($rightValue)
        if ($leftArray.Count -eq 0 -and $rightArray.Count -eq 0) { return $true }
        if ($leftArray.Count -eq 0 -or $rightArray.Count -eq 0) { return $false }
        return $leftArray.Count -eq $rightArray.Count -and
            (0..($leftArray.Count - 1) | ForEach-Object {
                [string]$leftArray[$_] -ceq [string]$rightArray[$_]
            } | Where-Object { -not $_ }).Count -eq 0
    }
    return [string]$leftValue -ceq [string]$rightValue
}

function Test-Stage5RecoverySnapshotAlreadyRestored {
    param([AllowNull()][object]$Current, [object]$Snapshot)
    if ([bool]$Snapshot.hadValue) {
        if ($null -eq $Current -or -not [bool]$Current.exists) { return $false }
        $currentEncoded = ConvertTo-Stage5RecoveryEncodedValue $Current.value `
            $Current.kind 'Current already-restored value'
        return Test-Stage5RecoveryEncodedValueEqual $currentEncoded `
            $Snapshot.oldValue 'Current already-restored value'
    }
    return $null -eq $Current -or -not [bool]$Current.exists
}

function Get-Stage5RecoveryInstallPathKey {
    param([ValidateSet('Generals', 'ZeroHour')][string]$Title)
    if ($Title -ceq 'Generals') {
        return 'Software\Electronic Arts\EA Games\Generals'
    }
    return 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
}

function Get-Stage5RecoveryInstallPathAncestors {
    param([string]$InstallPathKey)
    $result = New-Object 'Collections.Generic.List[string]'
    $current = ''
    foreach ($part in @($InstallPathKey.Split('\'))) {
        $current = if ([string]::IsNullOrEmpty($current)) { $part } else { $current + '\' + $part }
        $result.Add($current) | Out-Null
    }
    return $result.ToArray()
}

function Assert-Stage5RecoveryAllowedSnapshot {
    param([object]$Snapshot, [string]$Title, [string[]]$PlannedMissingSubKeys)
    $allowedKey = Get-Stage5RecoveryInstallPathKey $Title
    $view = [string](Get-Stage5RecoveryProperty $Snapshot 'view' 'Recovery snapshot')
    Assert-Stage5RecoveryCondition (@('Registry32', 'Registry64') -contains $view) `
        "Recovery snapshot registry view is unsupported: $view"
    $subKey = [string](Get-Stage5RecoveryProperty $Snapshot 'subKey' 'Recovery snapshot')
    $name = [string](Get-Stage5RecoveryProperty $Snapshot 'name' 'Recovery snapshot')
    Assert-Stage5RecoveryCondition ($subKey -ceq $allowedKey -and $name -ceq 'InstallPath') `
        "Recovery journal may bind only $Title InstallPath: $subKey/$name"
    $hadKey = [bool](Get-Stage5RecoveryProperty $Snapshot 'hadKey' 'Recovery snapshot')
    $hadValue = [bool](Get-Stage5RecoveryProperty $Snapshot 'hadValue' 'Recovery snapshot')
    Assert-Stage5RecoveryCondition ($hadKey -or -not $hadValue) `
        'A missing registry key cannot claim an existing value.'
    $oldValue = Get-Stage5RecoveryProperty $Snapshot 'oldValue' 'Recovery snapshot'
    $oldKind = Get-Stage5RecoveryProperty $Snapshot 'oldKind' 'Recovery snapshot'
    if ($hadValue) {
        Assert-Stage5RecoveryEncodedValue $oldValue 'Recovery snapshot oldValue' | Out-Null
        Assert-Stage5RecoveryCondition ($null -ne $oldKind -and
            [int]$oldKind -eq [int](Get-Stage5RecoveryProperty $oldValue 'kind' 'Recovery snapshot oldValue')) `
            'Recovery snapshot oldKind does not match oldValue.kind.'
    }
    else {
        Assert-Stage5RecoveryCondition ($null -eq $oldValue -and $null -eq $oldKind) `
            'A missing registry value must not carry an old value or kind.'
    }
    $expectedValue = Get-Stage5RecoveryProperty $Snapshot 'expectedValue' 'Recovery snapshot'
    Assert-Stage5RecoveryEncodedValue $expectedValue 'Recovery snapshot expectedValue' | Out-Null
    $expectedKind = ConvertTo-Stage5RecoveryKind `
        (Get-Stage5RecoveryProperty $Snapshot 'expectedKind' 'Recovery snapshot') `
        'Recovery expected InstallPath kind'
    Assert-Stage5RecoveryCondition ($expectedKind -eq
        [int](Get-Stage5RecoveryProperty $expectedValue 'kind' 'Recovery snapshot expectedValue')) `
        'Recovery snapshot expectedKind does not match expectedValue.kind.'
    $createdValue = Get-Stage5RecoveryProperty $Snapshot 'createdSubKeys' 'Recovery snapshot'
    Assert-Stage5RecoveryCondition ($createdValue -is [Array]) `
        'Recovery snapshot createdSubKeys must be an array.'
    $created = @($createdValue |
        ForEach-Object { [string]$_ })
    if ($hadKey) {
        Assert-Stage5RecoveryCondition ($created.Count -eq 0) `
            'A pre-existing title InstallPath key cannot also have createdSubKeys.'
    }
    else {
        Assert-Stage5RecoveryCondition ($created.Count -gt 0) `
            'A missing title InstallPath key must record its createdSubKeys.'
    }
    $ancestors = @(Get-Stage5RecoveryInstallPathAncestors $allowedKey)
    foreach ($path in $created) {
        Assert-Stage5RecoveryCondition ($ancestors -contains $path) `
            "Recovery createdSubKey escapes the allowed title InstallPath ancestry: $path"
        Assert-Stage5RecoveryCondition (@($PlannedMissingSubKeys) -contains
            ("$view|$path")) `
            "Recovery createdSubKey was not in the pre-side-effect plan: $path"
    }
    Assert-Stage5RecoveryCondition ($expectedKind -eq
        [int][Microsoft.Win32.RegistryValueKind]::String) `
        'Recovery expected InstallPath writes must use REG_SZ.'
}

function Assert-Stage5RecoveryPlanBindings {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [AllowEmptyCollection()][string[]]$PlannedMissingSubKeys = @(),
        [AllowEmptyCollection()][object[]]$Snapshots = @()
    )
    $planned = @($PlannedMissingSubKeys | ForEach-Object { [string]$_ })
    $allowed = @(Get-Stage5RecoveryInstallPathAncestors (
        Get-Stage5RecoveryInstallPathKey $Title))
    foreach ($plannedIdentity in $planned) {
        $separator = $plannedIdentity.IndexOf('|')
        Assert-Stage5RecoveryCondition ($separator -gt 0) `
            "Recovery planned subkey identity is not view-bound: $plannedIdentity"
        $view = $plannedIdentity.Substring(0, $separator)
        $path = $plannedIdentity.Substring($separator + 1)
        Assert-Stage5RecoveryCondition (@('Registry32', 'Registry64') -contains $view -and
            $allowed -contains $path) `
            "Recovery planned subkey escapes the allowed title InstallPath ancestry: $plannedIdentity"
    }
    $snapshotKeys = New-Object 'Collections.Generic.List[string]'
    $seenSnapshotKeys = @{}
    foreach ($snapshot in @($Snapshots)) {
        Assert-Stage5RecoveryAllowedSnapshot $snapshot $Title $planned
        $snapshotKey = "$($snapshot.view)|$($snapshot.subKey)|$($snapshot.name)"
        Assert-Stage5RecoveryCondition (-not $seenSnapshotKeys.ContainsKey($snapshotKey)) `
            "Recovery plan contains duplicate snapshot identity: $snapshotKey"
        $seenSnapshotKeys[$snapshotKey] = $true
        foreach ($created in @((Get-Stage5RecoveryProperty $snapshot `
                'createdSubKeys' 'Recovery snapshot'))) {
            $snapshotKeys.Add("$($snapshot.view)|$created") | Out-Null
        }
    }
    $plannedSet = @($planned | Sort-Object -Unique)
    $snapshotSet = @($snapshotKeys.ToArray() | Sort-Object -Unique)
    Assert-Stage5RecoveryCondition ($plannedSet.Count -eq $snapshotSet.Count -and
        (@($plannedSet | Where-Object { $snapshotSet -notcontains $_ }).Count -eq 0) -and
        (@($snapshotSet | Where-Object { $plannedSet -notcontains $_ }).Count -eq 0)) `
        'Recovery plannedMissingSubKeys must equal the view-bound union of snapshot.createdSubKeys.'
}

function Assert-Stage5RecoveryDocumentBindings {
    param([object]$Document)
    Assert-Stage5RecoveryPlanBindings -Title ([string]$Document.identity.title) `
        -PlannedMissingSubKeys @($Document.plannedMissingSubKeys | ForEach-Object { [string]$_ }) `
        -Snapshots @($Document.snapshots)
}

function Get-Stage5RecoveryCanonicalEncodedValue {
    param([AllowNull()][object]$Encoded, [string]$Context)
    if ($null -eq $Encoded) { return $null }
    Assert-Stage5RecoveryEncodedValue $Encoded $Context | Out-Null
    $kind = ConvertTo-Stage5RecoveryKind `
        (Get-Stage5RecoveryProperty $Encoded 'kind' $Context) "$Context kind"
    $type = Get-Stage5RecoveryKindName $kind "$Context kind"
    $value = Get-Stage5RecoveryEncodedData $Encoded $Context
    switch ($type) {
        'MultiString' { $value = @($value | ForEach-Object { [string]$_ }) }
        'Binary' { $value = [Convert]::ToBase64String([Convert]::FromBase64String([string]$value)) }
        'DWord' { $value = ([UInt32]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture)).ToString([Globalization.CultureInfo]::InvariantCulture) }
        'QWord' { $value = ([Int64]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture)).ToString([Globalization.CultureInfo]::InvariantCulture) }
        default { $value = [string]$value }
    }
    return [ordered]@{
        kind = $kind
        type = $type
        encoding = [string](Get-Stage5RecoveryProperty $Encoded 'encoding' $Context)
        value = $value
    }
}

function Get-Stage5RecoveryCanonicalSnapshotPlan {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [AllowEmptyCollection()][string[]]$PlannedMissingSubKeys = @(),
        [AllowEmptyCollection()][object[]]$Snapshots = @()
    )
    Assert-Stage5RecoveryPlanBindings -Title $Title `
        -PlannedMissingSubKeys $PlannedMissingSubKeys -Snapshots $Snapshots
    $canonicalSnapshots = @(
        @($Snapshots) | ForEach-Object {
                [ordered]@{
                    view = [string]$_.view
                    subKey = [string]$_.subKey
                    name = [string]$_.name
                    hadKey = [bool]$_.hadKey
                    hadValue = [bool]$_.hadValue
                    oldKind = if ($null -eq $_.oldKind) { $null } else { [int]$_.oldKind }
                    oldValue = Get-Stage5RecoveryCanonicalEncodedValue $_.oldValue 'Recovery plan oldValue'
                    expectedKind = [int]$_.expectedKind
                    expectedValue = Get-Stage5RecoveryCanonicalEncodedValue $_.expectedValue 'Recovery plan expectedValue'
                    createdSubKeys = @($_.createdSubKeys | ForEach-Object { [string]$_ } | Sort-Object -Unique)
                }
            }
    )
    return [ordered]@{
        title = $Title
        plannedMissingSubKeys = @($PlannedMissingSubKeys | ForEach-Object { [string]$_ } | Sort-Object -Unique)
        snapshots = $canonicalSnapshots
    }
}

function Get-Stage5RegistryRecoverySnapshotPlanSha256 {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [AllowEmptyCollection()][string[]]$PlannedMissingSubKeys = @(),
        [AllowEmptyCollection()][object[]]$Snapshots = @()
    )
    $canonicalPlan = Get-Stage5RecoveryCanonicalSnapshotPlan -Title $Title `
        -PlannedMissingSubKeys $PlannedMissingSubKeys -Snapshots $Snapshots
    $json = $canonicalPlan | ConvertTo-Json -Depth 32 -Compress
    $sha = New-Object Security.Cryptography.SHA256Managed
    try {
        return ([BitConverter]::ToString($sha.ComputeHash(
            (New-Object Text.UTF8Encoding($false)).GetBytes($json)))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}

function Assert-Stage5RecoverySnapshotPlanAppend {
    param(
        [Parameter(Mandatory = $true)][object]$ExistingPlan,
        [Parameter(Mandatory = $true)][object]$ProposedPlan
    )
    $existingPlanned = @($ExistingPlan.plannedMissingSubKeys | ForEach-Object { [string]$_ })
    $proposedPlanned = @($ProposedPlan.plannedMissingSubKeys | ForEach-Object { [string]$_ })
    Assert-Stage5RecoveryCondition (@($existingPlanned | Where-Object {
        $proposedPlanned -notcontains $_ }).Count -eq 0) `
        'Recovery update cannot remove an already-published planned subkey.'
    $existingSnapshots = @($ExistingPlan.snapshots)
    $proposedSnapshots = @($ProposedPlan.snapshots)
    Assert-Stage5RecoveryCondition ($proposedSnapshots.Count -ge $existingSnapshots.Count) `
        'Recovery update cannot remove an already-published snapshot.'
    for ($index = 0; $index -lt $existingSnapshots.Count; ++$index) {
        $oldJson = $existingSnapshots[$index] | ConvertTo-Json -Depth 32 -Compress
        $newJson = $proposedSnapshots[$index] | ConvertTo-Json -Depth 32 -Compress
        Assert-Stage5RecoveryCondition ($oldJson -ceq $newJson) `
            'Recovery update may only append snapshots after the unchanged existing prefix.'
    }
}

function Assert-Stage5RecoveryProcessIdentities {
    param([AllowEmptyCollection()][object[]]$ProcessIdentities = @())
    foreach ($entry in @($ProcessIdentities)) {
        Assert-Stage5RecoveryAllowedProperties $entry @(
            'launchPending', 'processId', 'creationTimeUtc100ns',
            'executablePath', 'executableSha256') 'Recovery process identity'
        $pending = Get-Stage5RecoveryProperty $entry 'launchPending' `
            'Recovery process identity'
        Assert-Stage5RecoveryCondition ($pending -is [bool]) `
            'Recovery process identity launchPending must be an actual boolean.'
        $processId = [Int64](Get-Stage5RecoveryProperty $entry 'processId' `
            'Recovery process identity')
        $creation = [Int64](Get-Stage5RecoveryProperty $entry `
            'creationTimeUtc100ns' 'Recovery process identity')
        Assert-Stage5RecoveryCondition ($processId -ge 0 -and $creation -ge 0) `
            'Recovery process identity numeric fields are invalid.'
        $path = [string](Get-Stage5RecoveryProperty $entry 'executablePath' `
            'Recovery process identity')
        $hash = [string](Get-Stage5RecoveryProperty $entry 'executableSha256' `
            'Recovery process identity')
        Assert-Stage5RecoveryCondition (-not [string]::IsNullOrWhiteSpace($path) -and
            $hash -cmatch '^[0-9A-Fa-f]{64}$') `
            'Recovery process identity requires executable path and SHA256.'
        if ($pending) {
            Assert-Stage5RecoveryCondition ($processId -eq 0 -and $creation -eq 0) `
                'A launch-pending identity cannot claim a PID or creation FILETIME.'
        }
        else {
            Assert-Stage5RecoveryCondition ($processId -gt 0 -and $creation -gt 0) `
                'A non-pending process identity requires a positive PID and creation FILETIME.'
        }
    }
}

function ConvertTo-Stage5RecoveryProcessIdentities {
    param(
        [AllowEmptyCollection()][object[]]$ProcessIdentities = @(),
        [AllowNull()][object]$ProcessIdentity,
        [bool]$UseProcessIdentities
    )
    $items = if ($UseProcessIdentities) { @($ProcessIdentities) } else { @($ProcessIdentity) }
    $result = New-Object 'Collections.Generic.List[object]'
    foreach ($item in $items) {
        Assert-Stage5RecoveryCondition ($null -ne $item) `
            'Recovery process identity entries cannot be null.'
        $hasPending = Test-Stage5RecoveryProperty $item 'launchPending'
        $hasLegacyLaunched = Test-Stage5RecoveryProperty $item 'childLaunched'
        if (-not $hasPending -and -not $hasLegacyLaunched) {
            throw 'Recovery process identity requires launchPending.'
        }
        $pending = if ($hasPending) {
            $value = Get-Stage5RecoveryProperty $item 'launchPending' `
                'Recovery process identity'
            Assert-Stage5RecoveryCondition ($value -is [bool]) `
                'Recovery process identity launchPending must be an actual boolean.'
            [bool]$value
        }
        else { $false }
        $legacyLaunched = if ($hasLegacyLaunched) {
            $value = Get-Stage5RecoveryProperty $item 'childLaunched' `
                'Recovery process identity'
            Assert-Stage5RecoveryCondition ($value -is [bool]) `
                'Recovery process identity childLaunched must be an actual boolean.'
            [bool]$value
        }
        else { $false }
        Assert-Stage5RecoveryCondition (-not ($pending -and $legacyLaunched)) `
            'A process identity cannot be both launch-pending and child-launched.'
        $processId = if (Test-Stage5RecoveryProperty $item 'processId') {
            [Int64](Get-Stage5RecoveryProperty $item 'processId' 'Recovery process identity')
        } else { 0 }
        $creation = if (Test-Stage5RecoveryProperty $item 'creationTimeUtc100ns') {
            [Int64](Get-Stage5RecoveryProperty $item 'creationTimeUtc100ns' 'Recovery process identity')
        } else { 0 }
        $path = if (Test-Stage5RecoveryProperty $item 'executablePath') {
            [string](Get-Stage5RecoveryProperty $item 'executablePath' 'Recovery process identity')
        } else { '' }
        $hash = if (Test-Stage5RecoveryProperty $item 'executableSha256') {
            [string](Get-Stage5RecoveryProperty $item 'executableSha256' 'Recovery process identity')
        } else { '' }
        if ($legacyLaunched) {
            Assert-Stage5RecoveryCondition ($processId -gt 0 -and $creation -gt 0) `
                'A launched process identity requires PID and creation FILETIME.'
        }
        if ($hasLegacyLaunched -and -not $legacyLaunched -and -not $pending) {
            # The compatibility shape used by older single-process callers
            # explicitly records that no child was launched. In the journal
            # that is represented by an empty processIdentities array, never
            # by an ambiguous zero-PID tuple.
            continue
        }
        $result.Add([ordered]@{
            launchPending = $pending
            processId = $processId
            creationTimeUtc100ns = $creation
            executablePath = $path
            executableSha256 = $hash.ToUpperInvariant()
        }) | Out-Null
    }
    $array = $result.ToArray()
    Assert-Stage5RecoveryProcessIdentities $array
    return $array
}

function Get-Stage5RecoveryAuthorizationProcessIdentities {
    param([object]$Authorization)
    if (Test-Stage5RecoveryProperty $Authorization 'processIdentities') {
        $entries = Get-Stage5RecoveryProperty $Authorization 'processIdentities' `
            'Recovery authorization'
        foreach ($entry in $entries) { $entry }
        return
    }
    if (Test-Stage5RecoveryProperty $Authorization 'processIdentity') {
        $legacy = Get-Stage5RecoveryProperty $Authorization 'processIdentity' `
            'Recovery authorization'
        if ((Test-Stage5RecoveryProperty $legacy 'childLaunched') -and
            $legacy.childLaunched -is [bool] -and -not $legacy.childLaunched -and
            -not (Test-Stage5RecoveryProperty $legacy 'launchPending')) {
            return @()
        }
        return @($legacy)
    }
    return @()
}

function New-Stage5RegistryRecoverySnapshot {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [Parameter(Mandatory = $true)][ValidateSet('Registry32', 'Registry64')][string]$View,
        [Parameter(Mandatory = $true)][string]$SubKey,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$HadKey,
        [Parameter(Mandatory = $true)][bool]$HadValue,
        [AllowNull()][object]$OldValue,
        [AllowNull()][object]$OldKind,
        [Parameter(Mandatory = $true)][object]$ExpectedValue,
        [Parameter(Mandatory = $true)][object]$ExpectedKind,
        [string[]]$CreatedSubKeys = @()
    )
    $context = "$View/$SubKey/$Name"
    Assert-Stage5RecoveryCondition (-not [string]::IsNullOrWhiteSpace($SubKey) -and
        -not [string]::IsNullOrWhiteSpace($Name)) "$context snapshot has an empty registry identity."
    $oldEncoded = if ($HadValue) {
        ConvertTo-Stage5RecoveryEncodedValue $OldValue $OldKind "$context old value"
    } else { $null }
    $expectedEncoded = ConvertTo-Stage5RecoveryEncodedValue $ExpectedValue $ExpectedKind `
        "$context expected value"
    $snapshot = [ordered]@{
        view = $View; subKey = $SubKey; name = $Name
        hadKey = $HadKey; hadValue = $HadValue
        oldKind = if ($null -eq $oldEncoded) { $null } else { $oldEncoded.kind }
        oldValue = $oldEncoded
        expectedKind = $expectedEncoded.kind
        expectedValue = $expectedEncoded
        createdSubKeys = @($CreatedSubKeys | ForEach-Object { [string]$_ })
    }
    Assert-Stage5RecoveryAllowedSnapshot $snapshot $Title @(
        $CreatedSubKeys | ForEach-Object { "$View|$_" })
    return $snapshot
}

function Assert-Stage5RecoveryIdentity {
    param([object]$Identity, [string]$TaskRoot, [string]$JournalPath)
    $root = Assert-Stage5RecoveryTaskRoot $TaskRoot
    $journal = Assert-Stage5RecoveryContainedPath $root $JournalPath 'Recovery journal'
    foreach ($name in @('runNonce', 'title', 'taskRoot', 'journalPath',
            'userSid', 'mutexName', 'identityMode', 'runnerScriptSha256',
            'executableSha256', 'snapshotPlanSha256')) {
        [void](Get-Stage5RecoveryProperty $Identity $name 'Recovery identity')
    }
    Assert-Stage5RecoveryUuid ([string](Get-Stage5RecoveryProperty $Identity 'runNonce' 'Recovery identity')) 'Recovery identity runNonce'
    $title = [string](Get-Stage5RecoveryProperty $Identity 'title' 'Recovery identity')
    Assert-Stage5RecoveryCondition (@('Generals', 'ZeroHour') -ccontains $title) `
        "Recovery identity title is unsupported: $title"
    Assert-Stage5RecoveryCondition ([IO.Path]::GetFullPath([string](Get-Stage5RecoveryProperty $Identity 'taskRoot' 'Recovery identity')).TrimEnd('\') -ceq $root) `
        'Recovery identity taskRoot does not match the contained task root.'
    Assert-Stage5RecoveryCondition ([IO.Path]::GetFullPath([string](Get-Stage5RecoveryProperty $Identity 'journalPath' 'Recovery identity')).TrimEnd('\') -ceq $journal) `
        'Recovery identity journalPath does not match the contained journal.'
    $userSid = [string](Get-Stage5RecoveryProperty $Identity 'userSid' 'Recovery identity')
    Assert-Stage5RecoveryCondition ($userSid -cmatch '^S-\d-(?:\d+-){1,14}\d+$') `
        'Recovery identity userSid is invalid.'
    Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $Identity 'mutexName' 'Recovery identity') -ceq
        (Get-Stage5RegistryRecoveryMutexName $userSid)) `
        'Recovery identity mutexName is not the SID-scoped Stage 5 mutex.'
    $identityMode = [string](Get-Stage5RecoveryProperty $Identity 'identityMode' 'Recovery identity')
    Assert-Stage5RecoveryCondition (@('diagnostic', 'acceptance-bound') -contains $identityMode) `
        "Recovery identityMode is unsupported: $identityMode"
    Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $Identity 'runnerScriptSha256' 'Recovery identity') -cmatch '^[0-9A-F]{64}$') `
        'Recovery identity runnerScriptSha256 is invalid.'
    Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $Identity 'executableSha256' 'Recovery identity') -cmatch '^[0-9A-F]{64}$') `
        'Recovery identity executableSha256 is invalid.'
    Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $Identity 'snapshotPlanSha256' 'Recovery identity') -cmatch '^[0-9A-F]{64}$') `
        'Recovery identity snapshotPlanSha256 is invalid.'
    $hasSource = Test-Stage5RecoveryProperty $Identity 'sourceCommit'
    $hasArtifact = Test-Stage5RecoveryProperty $Identity 'artifactSetSha256'
    if ($identityMode -ceq 'acceptance-bound') {
        Assert-Stage5RecoveryCondition ($hasSource -and $hasArtifact -and
            [string]$Identity.sourceCommit -cmatch '^[0-9a-f]{40}$' -and
            [string]$Identity.artifactSetSha256 -cmatch '^[0-9A-F]{64}$') `
            'Acceptance-bound recovery identity requires genuine sourceCommit and artifactSetSha256 bindings.'
    }
    else {
        Assert-Stage5RecoveryCondition (-not $hasSource -and -not $hasArtifact) `
            'Diagnostic recovery identity must omit sourceCommit and artifactSetSha256 claims.'
    }
    return $root
}

function Write-Stage5RegistryRecoveryDocument {
    param([string]$Path, [object]$Document, [switch]$ReplaceExisting)
    $json = $Document | ConvertTo-Json -Depth 32
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json)
    Write-Stage5FinalAcceptanceFileAtomically -Path $Path -Bytes $bytes `
        -Context 'Stage 5 registry recovery journal' -EvidenceKind JsonReceipt `
        -ReplaceExisting:$ReplaceExisting | Out-Null
}

function New-Stage5RegistryRecoveryJournal {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Identity,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$PlannedMissingSubKeys,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Snapshots,
        [AllowEmptyCollection()][object[]]$ProcessIdentities = @(),
        [AllowNull()][object]$ProcessIdentity
    )
    $useProcessIdentities = $PSBoundParameters.ContainsKey('ProcessIdentities')
    $useLegacyProcessIdentity = $PSBoundParameters.ContainsKey('ProcessIdentity')
    Assert-Stage5RecoveryCondition ($useProcessIdentities -xor $useLegacyProcessIdentity) `
        'Recovery journal requires exactly one of ProcessIdentities or legacy ProcessIdentity.'
    $root = Assert-Stage5RecoveryIdentity $Identity ([string]$Identity.taskRoot) $Path
    $journal = [IO.Path]::GetFullPath($Path)
    Assert-Stage5RecoveryCondition (-not (Test-Path -LiteralPath $journal)) `
        "Recovery journal already exists: $journal"
    foreach ($subKey in $PlannedMissingSubKeys) {
        Assert-Stage5RecoveryCondition (-not [string]::IsNullOrWhiteSpace($subKey)) `
            'Planned missing registry subkeys cannot be empty.'
    }
    $snapshotArray = @($Snapshots)
    foreach ($snapshot in $snapshotArray) {
        $snapshotView = [string](Get-Stage5RecoveryProperty $snapshot 'view' 'Recovery snapshot')
        $snapshotCreatedSubKeys = Get-Stage5RecoveryProperty $snapshot 'createdSubKeys' 'Recovery snapshot'
        foreach ($created in $snapshotCreatedSubKeys) {
            Assert-Stage5RecoveryCondition (@($PlannedMissingSubKeys) -contains
                ("$snapshotView|$created")) `
                "Recovery snapshot created subkey was not planned: $created"
            }
    }
    $computedSnapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ([string]$Identity.title) `
        -PlannedMissingSubKeys $PlannedMissingSubKeys -Snapshots $snapshotArray
    Assert-Stage5RecoveryCondition (
        [string]$Identity.snapshotPlanSha256 -ceq $computedSnapshotPlanSha256) `
        'Recovery identity snapshotPlanSha256 does not bind the canonical immutable snapshot plan.'
    $processIdentities = @(ConvertTo-Stage5RecoveryProcessIdentities `
        -ProcessIdentities $ProcessIdentities -ProcessIdentity $ProcessIdentity `
        -UseProcessIdentities $useProcessIdentities)
    $hasPendingProcess = @($processIdentities | Where-Object { $_.launchPending }).Count -gt 0
    $hasObservedProcess = @($processIdentities | Where-Object { $_.processId -gt 0 }).Count -gt 0
    $journalIdentity = [ordered]@{
        runNonce = [string]$Identity.runNonce
        title = [string]$Identity.title
        taskRoot = $root
        journalPath = $journal
        userSid = [string]$Identity.userSid
        mutexName = [string]$Identity.mutexName
        identityMode = [string]$Identity.identityMode
        runnerScriptSha256 = [string]$Identity.runnerScriptSha256
        executableSha256 = [string]$Identity.executableSha256
        snapshotPlanSha256 = [string]$Identity.snapshotPlanSha256
    }
    if ([string]$Identity.identityMode -ceq 'acceptance-bound') {
        $journalIdentity.sourceCommit = [string](Get-Stage5RecoveryProperty $Identity `
            'sourceCommit' 'Recovery identity')
        $journalIdentity.artifactSetSha256 = [string](Get-Stage5RecoveryProperty $Identity `
            'artifactSetSha256' 'Recovery identity')
    }
    $document = [ordered]@{
        schemaVersion = $script:Stage5RegistryRecoverySchemaVersion
        evidenceKind = 'stage5-registry-recovery'
        state = 'planned'
        identity = $journalIdentity
        processIdentities = $processIdentities
        childExitProof = -not ($hasPendingProcess -or $hasObservedProcess)
        noActiveTitleProcesses = -not ($hasPendingProcess -or $hasObservedProcess)
        plannedMissingSubKeys = @($PlannedMissingSubKeys | Sort-Object -Unique)
        snapshots = $snapshotArray
        recordedUtc = [DateTime]::UtcNow.ToString('o')
    }
    Assert-Stage5RecoveryDocumentBindings $document
    Write-Stage5RegistryRecoveryDocument $journal $document
    return $document
}

function Read-Stage5RegistryRecoveryJournal {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedIdentity
    )
    $root = Assert-Stage5RecoveryIdentity $ExpectedIdentity ([string]$ExpectedIdentity.taskRoot) $Path
    $journal = Assert-Stage5RecoveryContainedPath $root $Path 'Recovery journal'
    Assert-Stage5RecoveryCondition (Test-Path -LiteralPath $journal -PathType Leaf) `
        "Recovery journal is missing: $journal"
    $document = Get-Content -LiteralPath $journal -Raw | ConvertFrom-Json
    Assert-Stage5RecoveryCondition ([int]$document.schemaVersion -eq $script:Stage5RegistryRecoverySchemaVersion -and
        [string]$document.evidenceKind -ceq 'stage5-registry-recovery') `
        'Recovery journal schema or evidence kind is unsupported.'
    Assert-Stage5RecoveryAllowedProperties $document @(
        'schemaVersion', 'evidenceKind', 'state', 'identity',
        'processIdentities', 'childExitProof', 'noActiveTitleProcesses',
        'plannedMissingSubKeys', 'snapshots', 'recordedUtc', 'failure') `
        'Recovery journal'
    Assert-Stage5RecoveryCondition (@('planned', 'active', 'child-running',
        'child-exit-unproven', 'registry-restoration-failed', 'restored') -contains
        [string]$document.state) 'Recovery journal state is unsupported.'
    Assert-Stage5RecoveryCondition ($document.childExitProof -is [bool] -and
        $document.noActiveTitleProcesses -is [bool] -and
        $document.plannedMissingSubKeys -is [Array] -and
        $document.snapshots -is [Array] -and
        $document.processIdentities -is [Array]) 'Recovery journal proof or array fields are malformed.'
    $pendingProcessCount = @($document.processIdentities | Where-Object {
        $_.launchPending -is [bool] -and $_.launchPending }).Count
    Assert-Stage5RecoveryCondition (-not ($pendingProcessCount -gt 0 -and
        ($document.childExitProof -or $document.noActiveTitleProcesses))) `
        'A launch-pending journal cannot claim child exit or no-active proof.'
    $identity = $document.identity
    Assert-Stage5RecoveryAllowedProperties $identity @(
        'runNonce', 'title', 'taskRoot', 'journalPath', 'userSid',
        'mutexName', 'identityMode', 'runnerScriptSha256',
        'executableSha256', 'snapshotPlanSha256', 'sourceCommit', 'artifactSetSha256') `
        'Recovery journal identity'
    Assert-Stage5RecoveryProcessIdentities @($document.processIdentities)
    foreach ($snapshot in @($document.snapshots)) {
        Assert-Stage5RecoveryAllowedProperties $snapshot @(
            'view', 'subKey', 'name', 'hadKey', 'hadValue', 'oldKind',
            'oldValue', 'expectedKind', 'expectedValue', 'createdSubKeys') `
            'Recovery journal snapshot'
        foreach ($field in @('oldValue', 'expectedValue')) {
            if ($null -ne $snapshot.$field) {
                Assert-Stage5RecoveryAllowedProperties $snapshot.$field `
                    @('kind', 'type', 'encoding', 'value') `
                    "Recovery journal snapshot $field"
            }
        }
    }
    foreach ($name in @('runNonce', 'title', 'taskRoot', 'journalPath',
            'userSid', 'mutexName', 'identityMode', 'runnerScriptSha256',
            'executableSha256', 'snapshotPlanSha256')) {
        [void](Get-Stage5RecoveryProperty $identity $name 'Recovery journal identity')
    }
    Assert-Stage5RecoveryIdentity $identity ([string]$identity.taskRoot) `
        ([string]$identity.journalPath) | Out-Null
    foreach ($name in @('runNonce', 'title', 'taskRoot', 'journalPath',
            'userSid', 'mutexName', 'identityMode', 'runnerScriptSha256',
            'executableSha256', 'snapshotPlanSha256')) {
        Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $identity $name 'Recovery journal identity') -ceq
            [string](Get-Stage5RecoveryProperty $ExpectedIdentity $name 'Expected recovery identity')) `
            "Recovery journal identity field '$name' differs from the explicit expected identity."
    }
    foreach ($name in @('sourceCommit', 'artifactSetSha256')) {
        $journalHas = Test-Stage5RecoveryProperty $identity $name
        $expectedHas = Test-Stage5RecoveryProperty $ExpectedIdentity $name
        $journalValue = if ($journalHas) {
            [string](Get-Stage5RecoveryProperty $identity $name 'Recovery journal identity')
        } else { '' }
        $expectedValue = if ($expectedHas) {
            [string](Get-Stage5RecoveryProperty $ExpectedIdentity $name 'Expected recovery identity')
        } else { '' }
        Assert-Stage5RecoveryCondition ($journalValue -ceq $expectedValue) `
            "Recovery journal identity field '$name' differs from the explicit expected identity."
    }
    Assert-Stage5RecoveryDocumentBindings $document
    $computedSnapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ([string]$identity.title) `
        -PlannedMissingSubKeys @($document.plannedMissingSubKeys) `
        -Snapshots @($document.snapshots)
    Assert-Stage5RecoveryCondition (
        [string]$identity.snapshotPlanSha256 -ceq $computedSnapshotPlanSha256 -and
        [string]$ExpectedIdentity.snapshotPlanSha256 -ceq $computedSnapshotPlanSha256) `
        'Recovery journal snapshot plan digest does not match the trusted caller-bound plan.'
    return $document
}

function Update-Stage5RegistryRecoveryJournal {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedIdentity,
        [Parameter(Mandatory = $true)][ValidateSet('planned', 'active', 'child-running', 'child-exit-unproven', 'registry-restoration-failed', 'restored')][string]$State,
        [AllowEmptyCollection()][string[]]$PlannedMissingSubKeys,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Snapshots,
        [Parameter(Mandatory = $true)][bool]$ChildExitProof,
        [Parameter(Mandatory = $true)][bool]$NoActiveTitleProcesses,
        [AllowEmptyCollection()][object[]]$ProcessIdentities = @(),
        [AllowNull()][object]$ProcessIdentity,
        [string]$FailureMessage = ''
    )
    $useProcessIdentities = $PSBoundParameters.ContainsKey('ProcessIdentities')
    $useLegacyProcessIdentity = $PSBoundParameters.ContainsKey('ProcessIdentity')
    Assert-Stage5RecoveryCondition ($useProcessIdentities -xor $useLegacyProcessIdentity) `
        'Recovery journal update requires exactly one of ProcessIdentities or legacy ProcessIdentity.'
    $document = Read-Stage5RegistryRecoveryJournal $Path $ExpectedIdentity
    $newPlannedMissingSubKeys = @()
    if ($PSBoundParameters.ContainsKey('PlannedMissingSubKeys')) {
        $newPlannedMissingSubKeys = @($PlannedMissingSubKeys)
    }
    else { $newPlannedMissingSubKeys = @($document.plannedMissingSubKeys) }
    $existingPlan = Get-Stage5RecoveryCanonicalSnapshotPlan `
        -Title ([string]$document.identity.title) `
        -PlannedMissingSubKeys @($document.plannedMissingSubKeys) `
        -Snapshots @($document.snapshots)
    $proposedPlan = Get-Stage5RecoveryCanonicalSnapshotPlan `
        -Title ([string]$document.identity.title) `
        -PlannedMissingSubKeys $newPlannedMissingSubKeys -Snapshots $Snapshots
    Assert-Stage5RecoverySnapshotPlanAppend -ExistingPlan $existingPlan `
        -ProposedPlan $proposedPlan
    $computedSnapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 `
        -Title ([string]$document.identity.title) `
        -PlannedMissingSubKeys $newPlannedMissingSubKeys -Snapshots $Snapshots
    $processIdentities = @(ConvertTo-Stage5RecoveryProcessIdentities `
        -ProcessIdentities $ProcessIdentities -ProcessIdentity $ProcessIdentity `
        -UseProcessIdentities $useProcessIdentities)
    $hasPendingProcess = @($processIdentities | Where-Object { $_.launchPending }).Count -gt 0
    Assert-Stage5RecoveryCondition (-not ($hasPendingProcess -and
        ($ChildExitProof -or $NoActiveTitleProcesses))) `
        'Launch-pending process identities cannot claim exit or no-active proof.'
    $document.state = $State
    $document.plannedMissingSubKeys = @($newPlannedMissingSubKeys | Sort-Object -Unique)
    $document.snapshots = @($Snapshots)
    $document.identity.snapshotPlanSha256 = $computedSnapshotPlanSha256
    $document.childExitProof = $ChildExitProof
    $document.noActiveTitleProcesses = $NoActiveTitleProcesses
    $document.processIdentities = $processIdentities
    if (-not [string]::IsNullOrWhiteSpace($FailureMessage)) {
        $document | Add-Member -NotePropertyName failure -NotePropertyValue $FailureMessage -Force
    }
    $document.recordedUtc = [DateTime]::UtcNow.ToString('o')
    Assert-Stage5RecoveryDocumentBindings $document
    Write-Stage5RegistryRecoveryDocument $Path $document -ReplaceExisting
    $ExpectedIdentity.snapshotPlanSha256 = $computedSnapshotPlanSha256
    return $document
}

function Assert-Stage5RecoveryAdapter {
    param([Collections.IDictionary]$Adapter)
    foreach ($name in @('GetValue', 'GetKey', 'SetValue', 'DeleteValue', 'DeleteKey')) {
        Assert-Stage5RecoveryCondition ($Adapter.Contains($name) -and
            $Adapter[$name] -is [scriptblock]) `
            "Registry recovery adapter is missing scriptblock '$name'."
    }
}

function Invoke-Stage5RegistryRecovery {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$ExpectedIdentity,
        [Parameter(Mandatory = $true)][object]$Authorization,
        [Parameter(Mandatory = $true)][object]$MutexLock,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Adapter
    )
    $document = Read-Stage5RegistryRecoveryJournal $Path $ExpectedIdentity
    Assert-Stage5RecoveryAdapter $Adapter
    $mutexAcquired = Get-Stage5RecoveryProperty $MutexLock 'acquired' 'Recovery mutex'
    Assert-Stage5RecoveryCondition ($null -ne $MutexLock -and
        $mutexAcquired -is [bool] -and $mutexAcquired -and
        [string](Get-Stage5RecoveryProperty $MutexLock 'name' 'Recovery mutex') -ceq
            [string]$document.identity.mutexName) `
        'Registry recovery requires the explicit SID-scoped mutex ownership context.'
    $childExitProof = Get-Stage5RecoveryProperty $Authorization `
        'childExitProven' 'Recovery authorization'
    $noActiveTitleProcesses = Get-Stage5RecoveryProperty $Authorization `
        'noActiveTitleProcesses' 'Recovery authorization'
    Assert-Stage5RecoveryCondition ($childExitProof -is [bool] -and $childExitProof -and
        $noActiveTitleProcesses -is [bool] -and $noActiveTitleProcesses) `
        'Registry recovery requires explicit proven child exit and no-active-title authorization.'
    $journalProcesses = @($document.processIdentities)
    $authProcessInputs = @(Get-Stage5RecoveryAuthorizationProcessIdentities $Authorization)
    Assert-Stage5RecoveryCondition ($authProcessInputs.Count -eq $journalProcesses.Count) `
        'Recovery authorization must account for every journaled process identity.'
    $authProcesses = ConvertTo-Stage5RecoveryProcessIdentities `
        -ProcessIdentities $authProcessInputs -UseProcessIdentities $true
    for ($processIndex = 0; $processIndex -lt $journalProcesses.Count; ++$processIndex) {
        $journalProcess = $journalProcesses[$processIndex]
        $authProcess = $authProcesses[$processIndex]
        foreach ($name in @('launchPending', 'processId', 'creationTimeUtc100ns',
                'executablePath', 'executableSha256')) {
            Assert-Stage5RecoveryCondition ([string](Get-Stage5RecoveryProperty $authProcess $name 'Recovery authorization process') -ceq
                [string](Get-Stage5RecoveryProperty $journalProcess $name 'Recovery journal process')) `
                "Recovery process identity field '$name' differs from the journal."
        }
        $exitProven = Get-Stage5RecoveryProperty $authProcessInputs[$processIndex] `
            'exitProven' 'Recovery authorization process'
        Assert-Stage5RecoveryCondition ($exitProven -is [bool] -and $exitProven) `
            'Recovery authorization must prove exit for every journaled process identity.'
    }
    try {
    $snapshots = @($document.snapshots)
    # Preflight every current task-owned value and created-key boundary before
    # the first restore side effect. A changed value or unrelated child keeps
    # the journal in place and fails closed.
    foreach ($snapshot in $snapshots) {
        $current = & $Adapter['GetValue'] $snapshot.view $snapshot.subKey $snapshot.name
        if (Test-Stage5RecoverySnapshotAlreadyRestored $current $snapshot) {
            $preflightReadback = & $Adapter['GetValue'] $snapshot.view `
                $snapshot.subKey $snapshot.name
            Assert-Stage5RecoveryCondition (
                Test-Stage5RecoverySnapshotAlreadyRestored $preflightReadback $snapshot) `
                "Recovery already-restored value changed during preflight: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
            continue
        }
        Assert-Stage5RecoveryCondition ($null -ne $current -and [bool]$current.exists) `
            "Recovery ownership value is missing: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
        $currentEncoded = ConvertTo-Stage5RecoveryEncodedValue $current.value $current.kind 'Current recovery value'
        Assert-Stage5RecoveryCondition (Test-Stage5RecoveryEncodedValueEqual $currentEncoded $snapshot.expectedValue 'Recovery ownership value') `
            "Recovery ownership value changed: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
    }
    $createdPaths = @($document.plannedMissingSubKeys | Sort-Object -Unique |
        Sort-Object { $_.Length } -Descending)
    foreach ($createdIdentity in $createdPaths) {
        $separator = $createdIdentity.IndexOf('|')
        $view = $createdIdentity.Substring(0, $separator)
        $createdPath = $createdIdentity.Substring($separator + 1)
        $expectedValues = @($snapshots | Where-Object {
            $_.view -ceq $view -and $_.subKey -ceq $createdPath
        } | ForEach-Object { $_.name })
        $expectedChildren = @($document.plannedMissingSubKeys | Where-Object {
            $_.StartsWith($view + '|' + $createdPath + '\',
                [StringComparison]::OrdinalIgnoreCase)
        } | ForEach-Object {
            $_.Substring(($view + '|' + $createdPath + '\').Length).Split('\')[0]
        } | Sort-Object -Unique)
        $key = & $Adapter['GetKey'] $view $createdPath
        if ($null -eq $key -or -not [bool]$key.exists) {
            # A prior restore may already have removed this owned, planned
            # empty key. Its absence is the verified desired state; continue
            # while retaining failure for any unrelated present content.
            continue
        }
        foreach ($valueName in @($key.valueNames)) {
            Assert-Stage5RecoveryCondition ($expectedValues -contains [string]$valueName) `
                "Recovery refuses to delete key with unrelated value '$valueName': $createdIdentity"
        }
        foreach ($childName in @($key.subKeyNames)) {
            Assert-Stage5RecoveryCondition ($expectedChildren -contains [string]$childName) `
                "Recovery refuses to delete key with unrelated child '$childName': $createdIdentity"
        }
    }
        for ($index = $snapshots.Count - 1; $index -ge 0; --$index) {
            $snapshot = $snapshots[$index]
            $currentBeforeWrite = & $Adapter['GetValue'] $snapshot.view `
                $snapshot.subKey $snapshot.name
            if (Test-Stage5RecoverySnapshotAlreadyRestored $currentBeforeWrite $snapshot) {
                # Registry32/Registry64 title InstallPath is an unlisted HKCU
                # descendant on supported Windows versions and can therefore
                # be one physical value. A prior reverse-order restore through
                # the other view already achieved this snapshot's exact old
                # state; do not treat that alias readback as an ownership loss
                # or write it a second time.
                $aliasReadback = & $Adapter['GetValue'] $snapshot.view `
                    $snapshot.subKey $snapshot.name
                Assert-Stage5RecoveryCondition (
                    Test-Stage5RecoverySnapshotAlreadyRestored $aliasReadback $snapshot) `
                    "Registry alias readback did not match the original value: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
                continue
            }
            Assert-Stage5RecoveryCondition ($null -ne $currentBeforeWrite -and
                [bool]$currentBeforeWrite.exists) `
                "Recovery ownership value disappeared before restore: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
            $currentBeforeWriteEncoded = ConvertTo-Stage5RecoveryEncodedValue `
                $currentBeforeWrite.value $currentBeforeWrite.kind `
                'Current recovery value before restore'
            Assert-Stage5RecoveryCondition (Test-Stage5RecoveryEncodedValueEqual `
                $currentBeforeWriteEncoded $snapshot.expectedValue `
                'Current recovery value before restore') `
                "Recovery ownership value changed before restore: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
            if ([bool]$snapshot.hadValue) {
                $oldValue = ConvertFrom-Stage5RecoveryEncodedValue $snapshot.oldValue 'Recovery old value'
                & $Adapter['SetValue'] $snapshot.view $snapshot.subKey $snapshot.name $oldValue ([int]$snapshot.oldKind)
            }
            else {
                & $Adapter['DeleteValue'] $snapshot.view $snapshot.subKey $snapshot.name
            }
            $after = & $Adapter['GetValue'] $snapshot.view $snapshot.subKey $snapshot.name
            if ([bool]$snapshot.hadValue) {
                $afterEncoded = ConvertTo-Stage5RecoveryEncodedValue $after.value $after.kind 'Restored value'
                Assert-Stage5RecoveryCondition ([bool]$after.exists -and
                    (Test-Stage5RecoveryEncodedValueEqual $afterEncoded $snapshot.oldValue 'Restored value')) `
                    "Registry readback did not match the original value: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
            }
            else {
                Assert-Stage5RecoveryCondition (-not [bool]$after.exists) `
                    "Registry value remained after restoration: $($snapshot.view)/$($snapshot.subKey)/$($snapshot.name)"
            }
        }
        foreach ($createdIdentity in $createdPaths) {
            $separator = $createdIdentity.IndexOf('|')
            $view = $createdIdentity.Substring(0, $separator)
            $createdPath = $createdIdentity.Substring($separator + 1)
            $key = & $Adapter['GetKey'] $view $createdPath
            if ($null -eq $key -or -not [bool]$key.exists) { continue }
            Assert-Stage5RecoveryCondition (@($key.valueNames).Count -eq 0 -and
                @($key.subKeyNames).Count -eq 0) `
                "Recovery refuses to delete non-empty created key: $createdIdentity"
            & $Adapter['DeleteKey'] $view $createdPath
            $afterKey = & $Adapter['GetKey'] $view $createdPath
            Assert-Stage5RecoveryCondition ($null -eq $afterKey -or -not [bool]$afterKey.exists) `
                "Created key remained after restoration: $createdIdentity"
        }
        $document.state = 'restored'
        $document.recordedUtc = [DateTime]::UtcNow.ToString('o')
        Write-Stage5RegistryRecoveryDocument $Path $document -ReplaceExisting
        return $document
    }
    catch {
        try {
            $document.state = 'registry-restoration-failed'
            $document | Add-Member -NotePropertyName failure -NotePropertyValue $_.Exception.Message -Force
            $document.recordedUtc = [DateTime]::UtcNow.ToString('o')
            Write-Stage5RegistryRecoveryDocument $Path $document -ReplaceExisting
        }
        catch { }
        throw
    }
}

Export-ModuleMember -Function Get-Stage5RegistryRecoveryMutexName, `
    Enter-Stage5RegistryRecoveryMutex, Exit-Stage5RegistryRecoveryMutex, `
    Get-Stage5RegistryRecoverySnapshotPlanSha256, `
    New-Stage5RegistryRecoverySnapshot, New-Stage5RegistryRecoveryJournal, `
    Read-Stage5RegistryRecoveryJournal, Update-Stage5RegistryRecoveryJournal, `
    Invoke-Stage5RegistryRecovery
