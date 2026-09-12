$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$script:Stage5NativeFixtureBaseCommands = @(
    'Assert-Stage5FinalAcceptanceNoReparsePath',
    'Assert-Stage5FinalAcceptancePathContained',
    'Assert-Stage5FinalAcceptanceSnapshotSha256',
    'Get-Stage5FinalAcceptanceFileSnapshot',
    'Test-Stage5JsonInteger')
$script:Stage5NativeFixtureEvidenceModulePath = Join-Path $PSScriptRoot `
    'DeterministicSimulationEvidence.psm1'
# Keep an owned dependency scope for every native-reader instance. Ambient
# command visibility is not stable when this module is nested under another
# evidence module and that caller later reloads the base module with -Force.
# A private local import keeps the reader's snapshot helpers alive across that
# caller-side reload without changing the caller's module/session scope.
$script:Stage5NativeFixtureEvidenceModule = Import-Module `
    $script:Stage5NativeFixtureEvidenceModulePath -Scope Local `
    -PassThru -ErrorAction Stop
$missingBaseCommands = @($script:Stage5NativeFixtureBaseCommands |
    Where-Object { $null -eq (Get-Command -Name $_ -CommandType Function `
        -ErrorAction SilentlyContinue) })
if ($missingBaseCommands.Count -gt 0) {
    throw ('Native fixture production is missing required evidence commands: ' +
        ($missingBaseCommands -join ', ') + '.')
}

$script:Stage5NativeFixtureFatalPattern =
    '(?i)(CRC Mismatch|game thread ownership violation|assertion failed|fatal error|missing map|replay read error|SKIRMISH_AI_TEST_FAIL|STAGE5_PERFORMANCE_FIXTURE_FAIL|SIMULATION_JOB_SYSTEM_FALLBACK|SIMULATION_SHADOW_(?:MISMATCH|FAIL)|SIMULATION_COLLISION_MISMATCH)'
$script:Stage5NativeFixtureMinimumMapBytes = [Int64]16384
$script:Stage5NativeFixtureMaximumMapBytes = [Int64](64MB)
$script:Stage5NativeFixtureKernels = @(
    'physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')

function Assert-Stage5NativeFixtureCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Stage5NativeFixtureRawString {
    param([object]$Value, [string]$Context)
    Assert-Stage5NativeFixtureCondition ($Value -is [string]) `
        "$Context must be a JSON string."
    return [string]$Value
}

function Get-Stage5NativeFixtureRawInteger {
    param([object]$Value, [string]$Context)
    Assert-Stage5NativeFixtureCondition (Test-Stage5JsonInteger $Value) `
        "$Context must be a JSON integer."
    return $Value
}

function Get-Stage5NativeFixtureRawBoolean {
    param([object]$Value, [string]$Context)
    Assert-Stage5NativeFixtureCondition ($Value -is [bool]) `
        "$Context must be a JSON boolean."
    return [bool]$Value
}

function Get-Stage5NativeFixtureRawArray {
    param([object]$Value, [string]$Context)
    Assert-Stage5NativeFixtureCondition ($null -ne $Value -and
        $Value -is [Array]) "$Context must be a JSON array."
    return $Value
}

function Assert-Stage5NativeFixtureExactProperties {
    param([object]$Value, [string[]]$Names, [string]$Context)
    Assert-Stage5NativeFixtureCondition ($null -ne $Value -and
        $Value -isnot [Array]) "$Context must be one object."
    $actual = @($Value.PSObject.Properties.Name)
    $missing = @($Names | Where-Object { $actual -cnotcontains $_ })
    $extra = @($actual | Where-Object { $Names -cnotcontains $_ })
    Assert-Stage5NativeFixtureCondition ($missing.Count -eq 0 -and
        $extra.Count -eq 0) ("$Context has a non-canonical shape. Missing: " +
        "[$($missing -join ', ')]; unexpected: [$($extra -join ', ')].")
}

function Assert-Stage5NativeFixtureSha256 {
    param([object]$Value, [string]$Context)
    $raw = Get-Stage5NativeFixtureRawString $Value $Context
    Assert-Stage5NativeFixtureCondition ($raw -cmatch '^[0-9A-F]{64}$') `
        "$Context must be an independently supplied uppercase SHA-256."
}

function Assert-Stage5NativeFixtureSourceCommit {
    param([object]$Value, [string]$Context)
    $raw = Get-Stage5NativeFixtureRawString $Value $Context
    Assert-Stage5NativeFixtureCondition ($raw -cmatch '^[0-9a-f]{40}$') `
        "$Context must be a lowercase 40-hex source commit."
}

function Assert-Stage5NativeFixtureCanonicalUtc {
    param([object]$Value, [string]$Context)
    $raw = Get-Stage5NativeFixtureRawString $Value $Context
    [DateTime]$parsed = [DateTime]::MinValue
    $valid = $raw -cmatch
        '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{7}Z$' -and
        [DateTime]::TryParseExact($raw, 'o',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind, [ref]$parsed) -and
        $parsed.Kind -eq [DateTimeKind]::Utc -and
        $parsed.ToString('o',
            [Globalization.CultureInfo]::InvariantCulture) -ceq $raw
    Assert-Stage5NativeFixtureCondition $valid `
        "$Context must be a canonical UTC round-trip timestamp."
    return $parsed
}

function Assert-Stage5NativeFixtureUuidV4 {
    param([object]$Value, [string]$Context)
    $raw = Get-Stage5NativeFixtureRawString $Value $Context
    [Guid]$parsed = [Guid]::Empty
    $valid = $raw -cmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
        [Guid]::TryParseExact($raw, 'D', [ref]$parsed) -and
        $parsed.ToString('D') -ceq $raw
    Assert-Stage5NativeFixtureCondition $valid `
        "$Context must be a canonical lowercase version-4 UUID."
}

function ConvertFrom-Stage5NativeFixtureJsonBytes {
    param([byte[]]$Bytes, [string]$Context)
    try {
        $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($Bytes)
        $command = Get-Command ConvertFrom-Json
        if ($command.Parameters.ContainsKey('DateKind')) {
            return $text | ConvertFrom-Json -DateKind String
        }
        return $text | ConvertFrom-Json
    }
    catch { throw "$Context is not strict UTF-8 JSON: $($_.Exception.Message)" }
}

function Test-Stage5NativeFixtureSafeLeafMapSource {
    param([object]$Value)
    return $Value -is [string] -and
        -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -cmatch '^[A-Za-z0-9_-][A-Za-z0-9_. -]{0,127}\.map$' -and
        $Value -cnotmatch '(^|[ .])\.{1,2}([ .]|$)' -and
        -not [IO.Path]::IsPathRooted($Value) -and
        $Value.IndexOfAny([char[]]@('\', '/')) -lt 0
}

function Test-Stage5NativeFixtureSafeMapKey {
    param([object]$Value)
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value) -or
        $Value -cnotmatch '^Maps\\[A-Za-z0-9_-][A-Za-z0-9_. -]{0,63}\\[A-Za-z0-9_-][A-Za-z0-9_. -]{0,127}\.map$' -or
        $Value -match '(^|\\)[ .]|[ .](\\|$)' -or
        $Value -match '(^|\\)\.\.?($|\\)') { return $false }
    foreach ($component in @($Value -split '\\')) {
        $base = ($component -split '\.')[0]
        if ($base -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') {
            return $false
        }
    }
    return $true
}

function Read-Stage5ReviewedNativeKernelFixture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')]
        [string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedExecutableSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedDependencyManifestSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedRuntimeClosureSha256
    )
    Assert-Stage5NativeFixtureSha256 $ExpectedSha256 `
        'Expected reviewed fixture manifest SHA-256'
    Assert-Stage5NativeFixtureSha256 $ExpectedArtifactSetSha256 `
        'Expected artifact-set SHA-256'
    Assert-Stage5NativeFixtureSha256 $ExpectedExecutableSha256 `
        'Expected executable SHA-256'
    Assert-Stage5NativeFixtureSha256 $ExpectedDependencyManifestSha256 `
        'Expected dependency-manifest SHA-256'
    Assert-Stage5NativeFixtureSha256 $ExpectedRuntimeClosureSha256 `
        'Expected runtime-closure SHA-256'
    Assert-Stage5NativeFixtureSourceCommit $ExpectedSourceCommit `
        'Expected source commit'

    $full = [IO.Path]::GetFullPath($Path)
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        'Reviewed native kernel fixture manifest' -EvidenceKind JsonReceipt
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $ExpectedSha256 `
        'Reviewed native kernel fixture manifest' | Out-Null
    $document = ConvertFrom-Stage5NativeFixtureJsonBytes `
        ([byte[]]$snapshot.bytes) 'Reviewed native kernel fixture manifest'
    $rootNames = @('schemaVersion', 'evidenceKind', 'reviewStatus',
        'reviewScope', 'reviewedUtc', 'reviewedBy', 'title', 'sourceCommit',
        'artifactSetSha256', 'runtimeClosure', 'executableSha256', 'fixture',
        'requirements')
    Assert-Stage5NativeFixtureExactProperties $document $rootNames `
        'Reviewed native kernel fixture manifest'
    $schemaVersion = Get-Stage5NativeFixtureRawInteger $document.schemaVersion `
        'Reviewed native fixture schemaVersion'
    $evidenceKind = Get-Stage5NativeFixtureRawString $document.evidenceKind `
        'Reviewed native fixture evidenceKind'
    $reviewStatus = Get-Stage5NativeFixtureRawString $document.reviewStatus `
        'Reviewed native fixture reviewStatus'
    $reviewScope = Get-Stage5NativeFixtureRawString $document.reviewScope `
        'Reviewed native fixture reviewScope'
    $reviewedUtc = Get-Stage5NativeFixtureRawString $document.reviewedUtc `
        'Reviewed fixture review timestamp'
    $reviewedBy = Get-Stage5NativeFixtureRawString $document.reviewedBy `
        'Reviewed native fixture reviewer'
    $title = Get-Stage5NativeFixtureRawString $document.title `
        'Reviewed native fixture title'
    $sourceCommit = Get-Stage5NativeFixtureRawString $document.sourceCommit `
        'Reviewed native fixture source commit'
    $artifactSetSha256 = Get-Stage5NativeFixtureRawString `
        $document.artifactSetSha256 'Reviewed native fixture artifact-set SHA-256'
    $executableSha256 = Get-Stage5NativeFixtureRawString `
        $document.executableSha256 'Reviewed native fixture executable SHA-256'
    Assert-Stage5NativeFixtureCondition (
        (Test-Stage5JsonInteger $document.schemaVersion) -and
        $schemaVersion -eq 1 -and
        $evidenceKind -ceq 'stage5-reviewed-native-kernel-fixture' -and
        $reviewStatus -ceq 'approved' -and
        $reviewScope -ceq
            'native-dense-eight-player-kernel-execution-v1' -and
        $reviewedBy -cmatch '^[A-Za-z0-9][A-Za-z0-9._@ -]{2,127}$' -and
        $title -ceq $ExpectedTitle -and
        $sourceCommit -ceq $ExpectedSourceCommit -and
        $artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $executableSha256 -ceq $ExpectedExecutableSha256) `
        'Reviewed native fixture identity, approval, or candidate binding is invalid.'
    [void](Assert-Stage5NativeFixtureCanonicalUtc `
        $reviewedUtc 'Reviewed fixture review timestamp')
    Assert-Stage5NativeFixtureExactProperties $document.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Reviewed native fixture runtime closure'
    $dependencyManifestSha256 = Get-Stage5NativeFixtureRawString `
        $document.runtimeClosure.dependencyManifestSha256 `
        'Reviewed native fixture dependency-manifest SHA-256'
    $runtimeClosureSha256 = Get-Stage5NativeFixtureRawString `
        $document.runtimeClosure.closureSha256 `
        'Reviewed native fixture runtime-closure SHA-256'
    Assert-Stage5NativeFixtureCondition (
        $dependencyManifestSha256 -ceq
            $ExpectedDependencyManifestSha256 -and
        $runtimeClosureSha256 -ceq
            $ExpectedRuntimeClosureSha256) `
        'Reviewed native fixture runtime closure does not match the exact candidate.'

    $fixtureNames = @('id', 'kind', 'source', 'profileRelativePath',
        'mapKey', 'sha256', 'byteCount', 'seed', 'frameBudget',
        'expectedPlayerCount', 'minimumInitialUnitCount',
        'minimumPeakUnitCount')
    Assert-Stage5NativeFixtureExactProperties $document.fixture $fixtureNames `
        'Reviewed native fixture map'
    $fixture = $document.fixture
    $fixtureId = Get-Stage5NativeFixtureRawString $fixture.id `
        'Reviewed native fixture map id'
    $fixtureKind = Get-Stage5NativeFixtureRawString $fixture.kind `
        'Reviewed native fixture map kind'
    $fixtureSource = Get-Stage5NativeFixtureRawString $fixture.source `
        'Reviewed native fixture map source'
    $fixtureProfileRelativePath = Get-Stage5NativeFixtureRawString `
        $fixture.profileRelativePath 'Reviewed native fixture profile-relative path'
    $fixtureMapKey = Get-Stage5NativeFixtureRawString $fixture.mapKey `
        'Reviewed native fixture map key'
    $fixtureSha256 = Get-Stage5NativeFixtureRawString $fixture.sha256 `
        'Reviewed native map SHA-256'
    $fixtureByteCount = Get-Stage5NativeFixtureRawInteger $fixture.byteCount `
        'Reviewed native fixture map byte count'
    $fixtureSeed = Get-Stage5NativeFixtureRawInteger $fixture.seed `
        'Reviewed native fixture seed'
    $fixtureFrameBudget = Get-Stage5NativeFixtureRawInteger $fixture.frameBudget `
        'Reviewed native fixture frame budget'
    $fixtureExpectedPlayerCount = Get-Stage5NativeFixtureRawInteger `
        $fixture.expectedPlayerCount 'Reviewed native fixture expected player count'
    $fixtureMinimumInitialUnitCount = Get-Stage5NativeFixtureRawInteger `
        $fixture.minimumInitialUnitCount 'Reviewed native fixture minimum initial unit count'
    $fixtureMinimumPeakUnitCount = Get-Stage5NativeFixtureRawInteger `
        $fixture.minimumPeakUnitCount 'Reviewed native fixture minimum peak unit count'
    Assert-Stage5NativeFixtureSha256 $fixtureSha256 `
        'Reviewed native map SHA-256'
    Assert-Stage5NativeFixtureCondition (
        $fixtureId -ceq 'dense-eight-player' -and
        $fixtureKind -ceq 'native-map' -and
        (Test-Stage5NativeFixtureSafeLeafMapSource $fixtureSource) -and
        (Test-Stage5NativeFixtureSafeMapKey $fixtureProfileRelativePath) -and
        $fixtureMapKey -ceq $fixtureProfileRelativePath -and
        [Int64]$fixtureByteCount -ge $script:Stage5NativeFixtureMinimumMapBytes -and
        [Int64]$fixtureByteCount -le $script:Stage5NativeFixtureMaximumMapBytes -and
        [Int64]$fixtureSeed -ge 1 -and [Int64]$fixtureSeed -le 2147483647 -and
        [Int64]$fixtureFrameBudget -ge 1 -and
        [Int64]$fixtureFrameBudget -le 108000 -and
        [Int64]$fixtureExpectedPlayerCount -eq 8 -and
        [Int64]$fixtureMinimumInitialUnitCount -ge 8000 -and
        [Int64]$fixtureMinimumPeakUnitCount -ge 8000) `
        'Reviewed native fixture map is unsafe, non-dense, or outside the native recorder contract.'

    Assert-Stage5NativeFixtureExactProperties $document.requirements `
        @('nativeMapLoad', 'exactEightPlayerRoster', 'naturalVictory',
            'naturallyClosedReplay', 'requiredKernelFamilies') `
        'Reviewed native fixture requirements'
    $nativeMapLoad = Get-Stage5NativeFixtureRawBoolean `
        $document.requirements.nativeMapLoad `
        'Reviewed native fixture nativeMapLoad requirement'
    $exactEightPlayerRoster = Get-Stage5NativeFixtureRawBoolean `
        $document.requirements.exactEightPlayerRoster `
        'Reviewed native fixture exactEightPlayerRoster requirement'
    $naturalVictory = Get-Stage5NativeFixtureRawBoolean `
        $document.requirements.naturalVictory `
        'Reviewed native fixture naturalVictory requirement'
    $naturallyClosedReplay = Get-Stage5NativeFixtureRawBoolean `
        $document.requirements.naturallyClosedReplay `
        'Reviewed native fixture naturallyClosedReplay requirement'
    Get-Stage5NativeFixtureRawArray `
        $document.requirements.requiredKernelFamilies `
        'Reviewed native fixture required kernel families' | Out-Null
    $families = @($document.requirements.requiredKernelFamilies)
    Assert-Stage5NativeFixtureCondition (
        $nativeMapLoad -and $exactEightPlayerRoster -and
        $naturalVictory -and $naturallyClosedReplay -and
        $families.Count -eq $script:Stage5NativeFixtureKernels.Count) `
        'Reviewed native fixture requirements are incomplete.'
    for ($index = 0; $index -lt $families.Count; ++$index) {
        $family = Get-Stage5NativeFixtureRawString $families[$index] `
            "Reviewed native fixture kernel family $index"
        Assert-Stage5NativeFixtureCondition (
            $family -ceq
                $script:Stage5NativeFixtureKernels[$index]) `
            'Reviewed native fixture kernel families are missing, duplicated, or reordered.'
    }

    $manifestDirectory = Split-Path -Parent $full
    $mapPath = [IO.Path]::GetFullPath((Join-Path $manifestDirectory `
        $fixtureSource))
    Assert-Stage5FinalAcceptancePathContained $manifestDirectory $mapPath `
        'Reviewed native map source'
    $mapSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $mapPath `
        'Reviewed native map source' -EvidenceKind RuntimeBinary
    Assert-Stage5FinalAcceptanceSnapshotSha256 $mapSnapshot `
        $fixtureSha256 'Reviewed native map source' | Out-Null
    Assert-Stage5NativeFixtureCondition (
        [Int64]$mapSnapshot.length -eq [Int64]$fixtureByteCount -and
        [Int64]$mapSnapshot.length -ge $script:Stage5NativeFixtureMinimumMapBytes) `
        'Reviewed native map is a tiny placeholder or its byte-count binding changed.'

    return [pscustomobject]@{
        path = $full
        sha256 = $ExpectedSha256
        snapshot = $snapshot
        title = $ExpectedTitle
        reviewedUtc = $reviewedUtc
        reviewedBy = $reviewedBy
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        executableSha256 = $ExpectedExecutableSha256
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = $ExpectedDependencyManifestSha256
            closureSha256 = $ExpectedRuntimeClosureSha256
        }
        fixture = [pscustomobject]@{
            id = 'dense-eight-player'
            sourcePath = $mapPath
            sourceSnapshot = $mapSnapshot
            profileRelativePath = $fixtureProfileRelativePath
            mapKey = $fixtureMapKey
            sha256 = $fixtureSha256
            byteCount = [Int64]$fixtureByteCount
            seed = [int]$fixtureSeed
            frameBudget = [int]$fixtureFrameBudget
            expectedPlayerCount = 8
            minimumInitialUnitCount = [Int64]$fixtureMinimumInitialUnitCount
            minimumPeakUnitCount = [Int64]$fixtureMinimumPeakUnitCount
        }
    }
}

function ConvertTo-Stage5NativeFixtureWindowsArgument {
    param([string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { ++$slashes; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes)); $slashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) {
        [void]$builder.Append(('\' * ($slashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Get-Stage5NativePerformanceFixtureArgumentString {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$MapKey,
        [Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)]
        [int]$Seed,
        [Parameter(Mandatory = $true)][ValidateRange(1, 108000)]
        [int]$FrameBudget,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256
    )
    Assert-Stage5NativeFixtureCondition (
        Test-Stage5NativeFixtureSafeMapKey $MapKey) `
        'Native performance fixture map key is unsafe.'
    Assert-Stage5NativeFixtureSha256 $ExecutableSha256 `
        'Native performance fixture executable SHA-256'
    $values = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
        '-simulationMode', 'parallel', '-workerPolicy', 'auto',
        '-validationExecutableSha256', $ExecutableSha256, '-workerCount', '4',
        '-runStage5PerformanceFixture', $MapKey, [string]$Seed,
        [string]$FrameBudget)
    return (@($values | ForEach-Object {
        ConvertTo-Stage5NativeFixtureWindowsArgument ([string]$_)
    }) -join ' ')
}

function ConvertFrom-Stage5NativeFixtureFields {
    param([string]$Line, [string]$Prefix, [string[]]$Names,
        [string]$Context)
    Assert-Stage5NativeFixtureCondition ($Line.StartsWith(
        $Prefix, [StringComparison]::Ordinal)) "$Context has an invalid prefix."
    $remaining = $Line.Substring($Prefix.Length).TrimStart()
    $fields = [ordered]@{}
    while ($remaining.Length -gt 0) {
        $match = [regex]::Match($remaining,
            '^(?<name>[a-z][a-z0-9_]*)=(?:"(?<quoted>[^"\r\n]*)"|(?<bare>\S+))')
        Assert-Stage5NativeFixtureCondition $match.Success `
            "$Context contains malformed key/value text."
        $name = $match.Groups['name'].Value
        Assert-Stage5NativeFixtureCondition (-not $fields.Contains($name)) `
            "$Context repeats '$name'."
        $fields[$name] = if ($match.Groups['quoted'].Success) {
            $match.Groups['quoted'].Value
        } else { $match.Groups['bare'].Value }
        $remaining = $remaining.Substring($match.Length)
        if ($remaining.Length -gt 0) {
            Assert-Stage5NativeFixtureCondition ($remaining[0] -match '\s') `
                "$Context fields are not whitespace separated."
            $remaining = $remaining.TrimStart()
        }
    }
    $actual = @($fields.Keys)
    Assert-Stage5NativeFixtureCondition (
        @($Names | Where-Object { $actual -cnotcontains $_ }).Count -eq 0 -and
        @($actual | Where-Object { $Names -cnotcontains $_ }).Count -eq 0) `
        "$Context has missing or unexpected fields."
    return $fields
}

function Get-Stage5NativeFixtureUInt32 {
    param([object]$Value, [string]$Context)
    [UInt32]$parsed = 0
    Assert-Stage5NativeFixtureCondition (
        [string]$Value -cmatch '^(?:0|[1-9][0-9]*)$' -and
        [UInt32]::TryParse([string]$Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) `
        "$Context is not a canonical unsigned 32-bit integer."
    return $parsed
}

function Get-Stage5NativeFixtureUInt64 {
    param([object]$Value, [string]$Context)
    [UInt64]$parsed = 0
    Assert-Stage5NativeFixtureCondition (
        [string]$Value -cmatch '^(?:0|[1-9][0-9]*)$' -and
        [UInt64]::TryParse([string]$Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) `
        "$Context is not a canonical unsigned 64-bit integer."
    return $parsed
}

function Get-Stage5NativeFixtureHexUInt32 {
    param([object]$Value, [string]$Context)
    [UInt32]$parsed = 0
    Assert-Stage5NativeFixtureCondition (
        [string]$Value -cmatch '^[0-9A-F]{8}$' -and
        [UInt32]::TryParse([string]$Value,
            [Globalization.NumberStyles]::AllowHexSpecifier,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) `
        "$Context is not canonical uppercase eight-hex."
    return $parsed
}

function Read-Stage5NativeFixtureDiagnosticInput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][string]$MapKey,
        [Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)][int]$Seed,
        [Parameter(Mandatory = $true)][ValidateRange(1, 108000)][int]$FrameBudget,
        [Parameter(Mandatory = $true)][ValidateRange(1, 1000000)][int]$ExpectedInitialUnitCount
    )
    Assert-Stage5NativeFixtureSha256 $ExpectedSha256 'Diagnostic map SHA256'
    Assert-Stage5NativeFixtureSha256 $ExecutableSha256 'Diagnostic executable SHA256'
    Assert-Stage5NativeFixtureCondition (Test-Stage5NativeFixtureSafeMapKey $MapKey) 'Diagnostic map path is unsafe.'
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5NativeFixtureCondition ([IO.Path]::GetExtension($full) -ceq '.map') 'Diagnostic input must be a map.'
    Assert-Stage5FinalAcceptanceNoReparsePath ([IO.Path]::GetPathRoot($full)) $full 'Diagnostic map'
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full 'Diagnostic map' -EvidenceKind RawLog
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $ExpectedSha256 'Diagnostic map SHA256' | Out-Null
    Assert-Stage5NativeFixtureCondition ($snapshot.length -ge 16384 -and $snapshot.length -le 67108864) 'Diagnostic map size is outside bounds.'
    return [pscustomobject][ordered]@{
        evidenceKind = 'stage5-native-fixture-diagnostic-input'
        finalAcceptanceClaim = $false; performanceScalingClaim = $false; kernelQualificationClaim = $false
        title = $Title; executableSha256 = $ExecutableSha256
        fixture = [pscustomobject][ordered]@{
            sourcePath = $full; sourceSnapshot = $snapshot
            mapKey = $MapKey; profileRelativePath = $MapKey
            sha256 = $ExpectedSha256; byteCount = [Int64]$snapshot.length
            seed = $Seed; frameBudget = $FrameBudget; expectedInitialUnitCount = $ExpectedInitialUnitCount
        }
    }
}

# This parser verifies observations and natural recorder closure only. It does
# not admit a map/replay to any reviewed density or kernel qualification gate.
function ConvertFrom-Stage5NativeFixtureObservation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][object]$MapBinding,
        [Parameter(Mandatory = $true)][ValidateRange(1, 4294967295)]
        [Int64]$ExpectedProcessId,
        [Parameter(Mandatory = $true)][string]$ProfileRoot,
        [object]$RetainedReplaySnapshot = $null
    )
    Assert-Stage5NativeFixtureCondition ($Text.Length -gt 0 -and
        $Text.Length -le 64MB) `
        'Native performance fixture output is empty or exceeds 64 MiB.'
    Assert-Stage5NativeFixtureCondition ($Text -notmatch
        $script:Stage5NativeFixtureFatalPattern) `
        'Native performance fixture output contains a failure or fatal diagnostic.'
    $lines = @($Text -split '\r?\n' | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    })
    $startLines = @($lines | Where-Object {
        $_.StartsWith('STAGE5_PERFORMANCE_FIXTURE_START ',
            [StringComparison]::Ordinal)
    })
    $rosterLines = @($lines | Where-Object {
        $_.StartsWith('STAGE5_PERFORMANCE_FIXTURE_ROSTER ',
            [StringComparison]::Ordinal)
    })
    $observedLines = @($lines | Where-Object {
        $_.StartsWith('STAGE5_PERFORMANCE_FIXTURE_OBSERVED ',
            [StringComparison]::Ordinal)
    })
    $unitLines = @($lines | Where-Object {
        $_.StartsWith('STAGE5_PERFORMANCE_FIXTURE_PLAYER_UNITS ',
            [StringComparison]::Ordinal)
    })
    $completeLines = @($lines | Where-Object {
        $_.StartsWith('STAGE5_PERFORMANCE_FIXTURE_COMPLETE ',
            [StringComparison]::Ordinal)
    })
    Assert-Stage5NativeFixtureCondition ($startLines.Count -eq 1 -and
        $rosterLines.Count -eq 8 -and $observedLines.Count -eq 1 -and
        $unitLines.Count -eq 8 -and $completeLines.Count -eq 1) `
        'Native performance fixture output lacks its exact start/roster/workload/completion cardinality.'

    $start = ConvertFrom-Stage5NativeFixtureFields $startLines[0] `
        'STAGE5_PERFORMANCE_FIXTURE_START' @('category', 'title', 'map',
            'seed', 'frame_budget', 'map_crc', 'map_size', 'map_sha256',
            'executable_sha256', 'run_nonce') 'Native fixture start'
    $complete = ConvertFrom-Stage5NativeFixtureFields $completeLines[0] `
        'STAGE5_PERFORMANCE_FIXTURE_COMPLETE' @('category', 'title', 'map',
            'seed', 'frame_budget', 'map_crc', 'map_size', 'map_sha256',
            'actual_players', 'initial_units', 'peak_units',
            'initial_unrostered_units', 'peak_unrostered_units',
            'observed_first_frame', 'observed_last_frame',
            'observed_frame_samples', 'winner_team', 'end_frame', 'final_crc',
            'replay_epoch', 'ai_epoch_marker', 'replay_frame_count',
            'executable_sha256', 'run_nonce', 'replay_sha256',
            'retained_replay') 'Native fixture completion'

    foreach ($name in @('category', 'title', 'map', 'seed', 'frame_budget',
            'map_crc', 'map_size', 'map_sha256', 'executable_sha256',
            'run_nonce')) {
        Assert-Stage5NativeFixtureCondition (
            [string]$complete[$name] -ceq [string]$start[$name]) `
            "Native fixture start/completion field '$name' changed."
    }
    Assert-Stage5NativeFixtureCondition (
        $start.category -ceq 'native-performance-fixture' -and
        $start.title -ceq $MapBinding.title -and
        $start.map -ceq $MapBinding.fixture.mapKey -and
        $start.map_sha256 -ceq $MapBinding.fixture.sha256 -and
        $start.executable_sha256 -ceq $MapBinding.executableSha256 -and
        $start.run_nonce -cmatch '^[0-9A-F]{8}-[0-9A-F]{8}-[0-9A-F]{8}$') `
        'Native fixture start identity is detached from the reviewed candidate or native nonce format.'
    $seed = Get-Stage5NativeFixtureUInt32 $start.seed 'Native fixture seed'
    $frameBudget = Get-Stage5NativeFixtureUInt32 $start.frame_budget `
        'Native fixture frame budget'
    $mapByteCount = Get-Stage5NativeFixtureUInt32 $start.map_size `
        'Native fixture map size'
    $mapCrc = [string]$start.map_crc
    [void](Get-Stage5NativeFixtureHexUInt32 $mapCrc 'Native fixture map CRC')
    Assert-Stage5NativeFixtureCondition (
        $seed -eq [UInt32]$MapBinding.fixture.seed -and
        $frameBudget -eq [UInt32]$MapBinding.fixture.frameBudget -and
        $mapByteCount -eq [UInt32]$MapBinding.fixture.byteCount) `
        'Native fixture map/seed/frame identity differs from the reviewed manifest.'

    $nonceParts = @([string]$start.run_nonce -split '-')
    $noncePid = Get-Stage5NativeFixtureHexUInt32 $nonceParts[0] `
        'Native fixture nonce process ID'
    $nonceSeed = Get-Stage5NativeFixtureHexUInt32 $nonceParts[2] `
        'Native fixture nonce seed'
    Assert-Stage5NativeFixtureCondition (
        [UInt64]$noncePid -eq [UInt64]$ExpectedProcessId -and
        $nonceSeed -eq $seed) `
        'Native fixture nonce is detached from the launched process or requested seed.'

    $roster = New-Object 'Collections.Generic.List[object]'
    $americaIndex = $null
    for ($index = 0; $index -lt 8; ++$index) {
        $fields = ConvertFrom-Stage5NativeFixtureFields $rosterLines[$index] `
            'STAGE5_PERFORMANCE_FIXTURE_ROSTER' @('slot', 'controller',
                'faction', 'faction_index', 'start', 'color', 'team',
                'observer', 'map_owner', 'owner_verified') `
            "Native fixture roster row $index"
        $slot = Get-Stage5NativeFixtureUInt32 $fields.slot `
            "Native fixture roster row $index slot"
        $factionIndex = Get-Stage5NativeFixtureUInt32 $fields.faction_index `
            "Native fixture roster row $index faction index"
        if ($null -eq $americaIndex) { $americaIndex = $factionIndex }
        $expectedController = if ($index -eq 0) { 'human' } else { 'brutal-ai' }
        $expectedTeam = if ($index -lt 4) { '0' } else { '1' }
        $expectedOwner = if ($index -eq 0) { 'teamplayer0' } else {
            'teamSkirmishAmerica{0}' -f $index
        }
        Assert-Stage5NativeFixtureCondition (
            $slot -eq [UInt32]$index -and
            $fields.controller -ceq $expectedController -and
            $fields.faction -ceq 'FactionAmerica' -and
            $factionIndex -eq $americaIndex -and
            $fields.start -ceq [string]$index -and
            $fields.color -ceq [string]$index -and
            $fields.team -ceq $expectedTeam -and
            $fields.observer -ceq '0' -and
            $fields.map_owner -ceq $expectedOwner -and
            $fields.owner_verified -ceq '1') `
            "Native fixture roster row $index is not the exact reviewed 4v4 plan."
        $roster.Add([pscustomobject]@{
            slot = $index; controller = $expectedController
            factionIndex = [int]$factionIndex; team = [int]$expectedTeam
        }) | Out-Null
    }

    $observed = ConvertFrom-Stage5NativeFixtureFields $observedLines[0] `
        'STAGE5_PERFORMANCE_FIXTURE_OBSERVED' @('first_frame',
            'actual_players', 'initial_units', 'initial_unrostered_units') `
        'Native fixture first observation'
    $firstFrame = Get-Stage5NativeFixtureUInt32 $observed.first_frame `
        'Native fixture first observed frame'
    $players = Get-Stage5NativeFixtureUInt32 $observed.actual_players `
        'Native fixture observed player count'
    $initialUnits = Get-Stage5NativeFixtureUInt32 $observed.initial_units `
        'Native fixture initial unit count'
    $initialUnrostered = Get-Stage5NativeFixtureUInt32 `
        $observed.initial_unrostered_units `
        'Native fixture initial unrostered unit count'
    Assert-Stage5NativeFixtureCondition ($firstFrame -ge 1 -and
        $players -eq 8 -and
        $initialUnrostered -eq 0) `
        'Native fixture first observation is not an exact eight-player workload.'

    $playerUnits = New-Object 'Collections.Generic.List[object]'
    [UInt64]$initialSum = 0
    [UInt64]$peakSum = 0
    for ($index = 0; $index -lt 8; ++$index) {
        $fields = ConvertFrom-Stage5NativeFixtureFields $unitLines[$index] `
            'STAGE5_PERFORMANCE_FIXTURE_PLAYER_UNITS' @('slot',
                'initial_units', 'peak_units') "Native fixture unit row $index"
        $slot = Get-Stage5NativeFixtureUInt32 $fields.slot `
            "Native fixture unit row $index slot"
        $initial = Get-Stage5NativeFixtureUInt32 $fields.initial_units `
            "Native fixture unit row $index initial count"
        $peak = Get-Stage5NativeFixtureUInt32 $fields.peak_units `
            "Native fixture unit row $index peak count"
        Assert-Stage5NativeFixtureCondition ($slot -eq [UInt32]$index -and
            $initial -gt 0 -and $peak -ge $initial) `
            "Native fixture unit row $index is missing, empty, or regressed."
        $initialSum += $initial
        $peakSum += $peak
        $playerUnits.Add([pscustomobject][ordered]@{
            slot = $index
            initialUnitCount = [Int64]$initial
            peakUnitCount = [Int64]$peak
        }) | Out-Null
    }

    $actualPlayers = Get-Stage5NativeFixtureUInt32 $complete.actual_players `
        'Native fixture completed player count'
    $completedInitial = Get-Stage5NativeFixtureUInt32 $complete.initial_units `
        'Native fixture completed initial unit count'
    $peakUnits = Get-Stage5NativeFixtureUInt32 $complete.peak_units `
        'Native fixture completed peak unit count'
    $completedInitialUnrostered = Get-Stage5NativeFixtureUInt32 `
        $complete.initial_unrostered_units `
        'Native fixture completed initial unrostered count'
    $peakUnrostered = Get-Stage5NativeFixtureUInt32 `
        $complete.peak_unrostered_units `
        'Native fixture peak unrostered count'
    $observedFirst = Get-Stage5NativeFixtureUInt32 $complete.observed_first_frame `
        'Native fixture completed first frame'
    $observedLast = Get-Stage5NativeFixtureUInt32 $complete.observed_last_frame `
        'Native fixture completed last frame'
    $observedSamples = Get-Stage5NativeFixtureUInt64 `
        $complete.observed_frame_samples `
        'Native fixture observed frame sample count'
    $winner = Get-Stage5NativeFixtureUInt32 $complete.winner_team `
        'Native fixture winner team'
    $endFrame = Get-Stage5NativeFixtureUInt32 $complete.end_frame `
        'Native fixture end frame'
    $finalCrc = Get-Stage5NativeFixtureHexUInt32 $complete.final_crc `
        'Native fixture final CRC'
    $replayEpoch = Get-Stage5NativeFixtureUInt32 $complete.replay_epoch `
        'Native fixture replay epoch'
    $replayFrames = Get-Stage5NativeFixtureUInt32 $complete.replay_frame_count `
        'Native fixture replay frame count'
    $expectedEpoch = if ($MapBinding.title -ceq 'Generals') { 1 } else { 3 }
    $expectedMarker = if ($MapBinding.title -ceq 'Generals') {
        ' [GeneralsAIPlanningEpoch=1]'
    } else { ' [SkirmishAIEpoch=3]' }
    Assert-Stage5NativeFixtureCondition (
        $actualPlayers -eq 8 -and $completedInitial -eq $initialUnits -and
        [UInt64]$completedInitial -eq $initialSum -and
        [UInt64]$peakUnits -eq $peakSum -and
        $completedInitialUnrostered -eq 0 -and $peakUnrostered -eq 0 -and
        $observedFirst -eq $firstFrame -and $observedLast -eq $endFrame -and
        $observedSamples -eq ([UInt64]$observedLast - [UInt64]$observedFirst + 1) -and
        ($winner -eq 0 -or $winner -eq 1) -and
        $endFrame -gt 0 -and $endFrame -le $frameBudget -and
        $replayFrames -eq ($endFrame + 1) -and
        $replayEpoch -eq $expectedEpoch -and
        $complete.ai_epoch_marker -ceq $expectedMarker) `
        'Native fixture workload, natural-victory, contiguous-frame, or replay-epoch closure is invalid.'

    Assert-Stage5NativeFixtureSha256 ([string]$complete.replay_sha256) `
        'Native fixture retained replay SHA-256'
    $profileFull = [IO.Path]::GetFullPath($ProfileRoot).TrimEnd('\', '/')
    $replayFull = [IO.Path]::GetFullPath([string]$complete.retained_replay)
    Assert-Stage5NativeFixtureCondition ($replayFull.StartsWith(
        $profileFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Native fixture retained replay is outside the isolated title profile.'
    $replaySnapshot = if ($null -eq $RetainedReplaySnapshot) {
        Get-Stage5FinalAcceptanceFileSnapshot $replayFull `
            'Native fixture retained replay' -EvidenceKind Replay
    }
    else {
        Assert-Stage5NativeFixtureCondition (
            $RetainedReplaySnapshot.PSObject.Properties.Name -ccontains 'sha256' -and
            $RetainedReplaySnapshot.PSObject.Properties.Name -ccontains 'length' -and
            [Int64]$RetainedReplaySnapshot.length -gt 0) `
            'Native fixture relocated replay snapshot is incomplete.'
        $RetainedReplaySnapshot
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $replaySnapshot `
        ([string]$complete.replay_sha256) 'Native fixture retained replay' |
        Out-Null

    return [pscustomobject][ordered]@{
        diagnosticOnly = $true
        nativeRunNonce = [string]$start.run_nonce
        mapKey = [string]$start.map
        mapSha256 = [string]$start.map_sha256
        mapCrc32 = $mapCrc
        mapByteCount = [Int64]$mapByteCount
        seed = [int]$seed
        frameBudget = [int]$frameBudget
        endFrame = [int]$endFrame
        winnerTeam = [int]$winner
        finalCrc = [UInt32]$finalCrc
        replayEpoch = [int]$replayEpoch
        aiEpochMarker = [string]$complete.ai_epoch_marker
        observedFirstFrame = [int]$observedFirst
        observedLastFrame = [int]$observedLast
        observedFrameSamples = [Int64]$observedSamples
        observedPlayerCount = 8
        initialUnitCount = [Int64]$completedInitial
        peakUnitCount = [Int64]$peakUnits
        playerUnits = $playerUnits.ToArray()
        replaySha256 = [string]$complete.replay_sha256
        replayFrameCount = [Int64]$replayFrames
        nativeRetainedReplayPath = $replayFull
        retainedReplayPath = $replayFull
        retainedReplaySnapshot = $replaySnapshot
    }
}

function ConvertFrom-Stage5NativePerformanceFixtureOutput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][object]$ReviewedFixture,
        [Parameter(Mandatory = $true)][ValidateRange(1, 4294967295)][Int64]$ExpectedProcessId,
        [Parameter(Mandatory = $true)][string]$ProfileRoot,
        [object]$RetainedReplaySnapshot = $null
    )
    $completion = ConvertFrom-Stage5NativeFixtureObservation -Text $Text -MapBinding $ReviewedFixture `
        -ExpectedProcessId $ExpectedProcessId -ProfileRoot $ProfileRoot -RetainedReplaySnapshot $RetainedReplaySnapshot
    Assert-Stage5NativeFixtureCondition (
        $completion.initialUnitCount -ge [Int64]$ReviewedFixture.fixture.minimumInitialUnitCount -and
        $completion.peakUnitCount -ge [Int64]$ReviewedFixture.fixture.minimumPeakUnitCount) `
        'Native fixture workload is below its reviewed dense minimum.'
    $completion.diagnosticOnly = $false
    return $completion
}

function Assert-Stage5NativeFixtureProductionCompletion {
    param([Parameter(Mandatory = $true)][object]$Completion)
    $isDictionary = $Completion -is [Collections.IDictionary]
    $names = if ($isDictionary) { @($Completion.Keys) } else { @($Completion.PSObject.Properties.Name) }
    $markers = @($names | Where-Object { $_ -is [string] -and $_ -ieq 'diagnosticOnly' })
    # Historical production completions predate this marker. They still must
    # pass the publisher's complete existing projection/receipt validation.
    if ($markers.Count -eq 0) { return }
    Assert-Stage5NativeFixtureCondition ($markers.Count -eq 1) 'Duplicate diagnostic marker is ambiguous.'
    $value = if ($isDictionary) { $Completion[$markers[0]] } else { $Completion.PSObject.Properties[$markers[0]].Value }
    Assert-Stage5NativeFixtureCondition ($value -is [bool]) 'Diagnostic marker must retain its boolean type.'
    Assert-Stage5NativeFixtureCondition (-not $value) 'A diagnostic observation cannot publish a native production receipt.'
}

function Write-Stage5NativeFixtureCapturedOutput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [AllowNull()][AllowEmptyCollection()][byte[]]$StdoutBytes,
        [AllowNull()][AllowEmptyCollection()][byte[]]$StderrBytes
    )
    $root = [IO.Path]::GetFullPath($TaskRoot)
    $logs = Join-Path $root 'logs'
    Assert-Stage5NativeFixtureCondition ((Test-Path -LiteralPath $root -PathType Container) -and
        (Test-Path -LiteralPath $logs -PathType Container)) 'Native fixture output sink must be an existing directory.'
    Assert-Stage5FinalAcceptanceNoReparsePath ([IO.Path]::GetPathRoot($root)) $logs 'Native fixture log sink'
    foreach ($entry in @(@('stdout.log', $StdoutBytes), @('stderr.log', $StderrBytes))) {
        if ($null -ne $entry[1]) {
            Assert-Stage5NativeFixtureCondition ($entry[1].Length -le 67108864) 'Native fixture captured output exceeds bound.'
            Write-Stage5FinalAcceptanceFileAtomically -Path (Join-Path $logs $entry[0]) `
                -Bytes ([byte[]]$entry[1]) -Context 'Native fixture captured output' -EvidenceKind RawLog | Out-Null
        }
    }
    $raw = $null
    if ($null -ne $StdoutBytes -and $null -ne $StderrBytes) {
        $bytes = New-Object byte[] ($StdoutBytes.Length + 1 + $StderrBytes.Length)
        [Array]::Copy($StdoutBytes, 0, $bytes, 0, $StdoutBytes.Length)
        $bytes[$StdoutBytes.Length] = 10
        [Array]::Copy($StderrBytes, 0, $bytes, $StdoutBytes.Length + 1, $StderrBytes.Length)
        $path = Join-Path $logs 'fixture-output.log'
        Write-Stage5FinalAcceptanceFileAtomically -Path $path -Bytes $bytes `
            -Context 'Native fixture combined raw log' -EvidenceKind RawLog | Out-Null
        $raw = Get-Stage5FinalAcceptanceFileSnapshot $path 'Native fixture combined raw log' -EvidenceKind RawLog
    }
    return [pscustomobject]@{ stdoutAvailable=($null -ne $StdoutBytes); stderrAvailable=($null -ne $StderrBytes); rawLogSnapshot=$raw }
}

function New-Stage5NativeFixtureDiagnosticResult {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidateSet('observed', 'failed')][string]$Status,
        [object]$Completion = $null, [object]$ProcessIdentity = $null,
        [Parameter(Mandatory = $true)][object]$Lifecycle,
        [Parameter(Mandatory = $true)][int]$ExpectedInitialUnitCount,
        [string]$ErrorText = '', [string[]]$CleanupErrors = @()
    )
    $observation = $null; $matches = $false
    if ($Status -ceq 'observed') {
        foreach ($name in @('mutexAcquired', 'noInstalledTitleProcessesAtPreflight', 'childExitProven',
                'registryRestored', 'profileRemoved', 'recoveryJournalAbsent')) {
            Assert-Stage5NativeFixtureCondition ($Lifecycle.PSObject.Properties.Name -ccontains $name -and
                [bool]$Lifecycle.$name) "Diagnostic completion lacks lifecycle proof '$name'."
        }
        Assert-Stage5NativeFixtureCondition ($null -ne $ProcessIdentity -and
            $ProcessIdentity.processId -gt 0 -and $null -ne $Completion -and $Completion.diagnosticOnly) `
            'Diagnostic completion lacks identity-bound observation.'
        $observation = Get-Stage5NativeFixtureCompletionProjection $Completion
        $matches = $Completion.initialUnitCount -eq $ExpectedInitialUnitCount -and $Completion.peakUnitCount -eq $ExpectedInitialUnitCount
    }
    return [ordered]@{
        schemaVersion=1; evidenceKind='stage5-native-fixture-diagnostic-observation'; status=$Status
        recordedUtc=[DateTime]::UtcNow.ToString('o')
        finalAcceptanceClaim=$false; performanceScalingClaim=$false; kernelQualificationClaim=$false
        expectedInitialUnitCount=$ExpectedInitialUnitCount; expectedPopulationObserved=$matches
        observation=$observation; processIdentity=$ProcessIdentity; lifecycle=$Lifecycle
        error=$ErrorText; cleanupErrors=@($CleanupErrors)
    }
}

function Get-Stage5NativeFixtureCompletionProjection {
    param([object]$Completion)
    $names = @('nativeRunNonce', 'mapKey', 'mapSha256', 'mapCrc32',
        'mapByteCount', 'seed', 'frameBudget', 'endFrame', 'winnerTeam',
        'finalCrc', 'replayEpoch', 'aiEpochMarker', 'observedFirstFrame',
        'observedLastFrame', 'observedFrameSamples', 'observedPlayerCount',
        'initialUnitCount', 'peakUnitCount', 'playerUnits', 'replaySha256',
        'replayFrameCount', 'nativeRetainedReplayPath')
    $result = [ordered]@{}
    foreach ($name in $names) {
        Assert-Stage5NativeFixtureCondition (
            $Completion.PSObject.Properties.Name -ccontains $name) `
            "Native fixture completion is missing '$name'."
        $result[$name] = $Completion.$name
    }
    return [pscustomobject]$result
}

function New-Stage5NativePerformanceFixtureProductionReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')]
        [string]$Title,
        [Parameter(Mandatory = $true)][string]$RecordedUtc,
        [Parameter(Mandatory = $true)][string]$CohortNonce,
        [Parameter(Mandatory = $true)][string]$CohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][string]$ArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$DependencyManifestSha256,
        [Parameter(Mandatory = $true)][string]$RuntimeClosureSha256,
        [Parameter(Mandatory = $true)][string]$ReviewedFixtureManifestPath,
        [Parameter(Mandatory = $true)][string]$ReviewedFixtureManifestSha256,
        [Parameter(Mandatory = $true)][string]$PrelaunchPlanPath,
        [Parameter(Mandatory = $true)][string]$PrelaunchPlanSha256,
        [Parameter(Mandatory = $true)][string]$AttemptStartPath,
        [Parameter(Mandatory = $true)][string]$AttemptStartSha256,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][string]$HostRunNonce,
        [Parameter(Mandatory = $true)][ValidateRange(1, 4294967295)]
        [Int64]$ProcessId,
        [Parameter(Mandatory = $true)][ValidateRange(1, [Int64]::MaxValue)]
        [Int64]$ProcessCreationTimeUtc100ns,
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$ArgumentString,
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$NativeProfileRoot,
        [Parameter(Mandatory = $true)][object]$Completion,
        [Parameter(Mandatory = $true)][string]$RawLogPath,
        [Parameter(Mandatory = $true)][string]$RawLogSha256,
        [Parameter(Mandatory = $true)][string]$RetainedReplayPath,
        [Parameter(Mandatory = $true)][object]$Lifecycle
    )
    Assert-Stage5NativeFixtureProductionCompletion $Completion
    $projected = Get-Stage5NativeFixtureCompletionProjection $Completion
    $receipt = [pscustomobject][ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-native-performance-fixture-production'
        producer = 'installed-runtime-performance-fixture-producer-v1'
        status = 'passed'
        recordedUtc = $RecordedUtc
        cohortNonce = $CohortNonce
        cohortCreatedUtc = $CohortCreatedUtc
        qualificationMode = 'InstalledKernelExecution'
        acceptanceScope = 'fixture-production-only'
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
        title = $Title
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
        runtimeClosure = [pscustomobject][ordered]@{
            dependencyManifestSha256 = $DependencyManifestSha256
            closureSha256 = $RuntimeClosureSha256
        }
        reviewedFixtureManifest = [pscustomobject][ordered]@{
            path = $ReviewedFixtureManifestPath
            sha256 = $ReviewedFixtureManifestSha256
        }
        prelaunchPlan = [pscustomobject][ordered]@{
            path = $PrelaunchPlanPath
            sha256 = $PrelaunchPlanSha256
        }
        attemptStart = [pscustomobject][ordered]@{
            path = $AttemptStartPath
            sha256 = $AttemptStartSha256
        }
        executableSha256 = $ExecutableSha256
        hostRunNonce = $HostRunNonce
        process = [pscustomobject][ordered]@{
            id = $ProcessId
            creationTimeUtc100ns = $ProcessCreationTimeUtc100ns
            exitCode = 0
        }
        commandLine = $CommandLine
        argumentString = $ArgumentString
        taskRoot = $TaskRoot
        nativeProfileRoot = $NativeProfileRoot
        map = [pscustomobject][ordered]@{
            key = $projected.mapKey
            sha256 = $projected.mapSha256
            crc32 = $projected.mapCrc32
            byteCount = $projected.mapByteCount
        }
        rawLog = [pscustomobject][ordered]@{
            path = $RawLogPath
            sha256 = $RawLogSha256
        }
        retainedReplay = [pscustomobject][ordered]@{
            path = $RetainedReplayPath
            sha256 = $projected.replaySha256
            frameCount = $projected.replayFrameCount
        }
        completion = $projected
        lifecycle = $Lifecycle
    }
    Assert-Stage5NativePerformanceFixtureProductionReceipt $receipt |
        Out-Null
    return $receipt
}

function Assert-Stage5NativeFixturePlanRawTypes {
    param([object]$Plan)
    $names = @('schemaVersion', 'event', 'recordedUtc', 'hostRunNonce',
        'cohortNonce', 'cohortCreatedUtc', 'title', 'sourceCommit',
        'artifactSetSha256', 'runtimeClosure', 'executablePath',
        'executableSha256', 'reviewedFixtureManifestPath',
        'reviewedFixtureManifestSha256', 'mapSourcePath', 'mapSha256',
        'mapDestinationPath', 'argumentString', 'taskRoot', 'timeoutSeconds',
        'workerCount', 'finalAcceptanceClaim', 'performanceScalingClaim')
    Assert-Stage5NativeFixtureExactProperties $Plan $names `
        'Retained native fixture prelaunch plan'
    Assert-Stage5NativeFixtureExactProperties $Plan.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Retained native fixture prelaunch runtime closure'
    Get-Stage5NativeFixtureRawInteger $Plan.schemaVersion `
        'Native fixture plan schemaVersion' | Out-Null
    foreach ($name in @('event', 'recordedUtc', 'hostRunNonce', 'cohortNonce',
            'cohortCreatedUtc', 'title', 'sourceCommit', 'artifactSetSha256',
            'executablePath', 'executableSha256',
            'reviewedFixtureManifestPath', 'reviewedFixtureManifestSha256',
            'mapSourcePath', 'mapSha256', 'mapDestinationPath',
            'argumentString', 'taskRoot')) {
        Get-Stage5NativeFixtureRawString $Plan.$name `
            "Native fixture plan $name" | Out-Null
    }
    foreach ($name in @('timeoutSeconds', 'workerCount')) {
        Get-Stage5NativeFixtureRawInteger $Plan.$name `
            "Native fixture plan $name" | Out-Null
    }
    foreach ($name in @('finalAcceptanceClaim', 'performanceScalingClaim')) {
        Get-Stage5NativeFixtureRawBoolean $Plan.$name `
            "Native fixture plan $name" | Out-Null
    }
    Get-Stage5NativeFixtureRawString $Plan.runtimeClosure.dependencyManifestSha256 `
        'Native fixture plan dependency-manifest SHA-256' | Out-Null
    Get-Stage5NativeFixtureRawString $Plan.runtimeClosure.closureSha256 `
        'Native fixture plan runtime-closure SHA-256' | Out-Null
}

function Assert-Stage5NativeFixtureAttemptStartRawTypes {
    param([object]$Start)
    Assert-Stage5NativeFixtureExactProperties $Start `
        @('schemaVersion', 'event', 'recordedUtc', 'plan', 'hostRunNonce') `
        'Retained native fixture attempt start'
    Assert-Stage5NativeFixtureExactProperties $Start.plan `
        @('path', 'sha256') 'Retained native fixture attempt-start plan binding'
    Get-Stage5NativeFixtureRawInteger $Start.schemaVersion `
        'Native fixture attempt-start schemaVersion' | Out-Null
    foreach ($name in @('event', 'recordedUtc', 'hostRunNonce')) {
        Get-Stage5NativeFixtureRawString $Start.$name `
            "Native fixture attempt-start $name" | Out-Null
    }
    Get-Stage5NativeFixtureRawString $Start.plan.path `
        'Native fixture attempt-start plan path' | Out-Null
    Get-Stage5NativeFixtureRawString $Start.plan.sha256 `
        'Native fixture attempt-start plan SHA-256' | Out-Null
}

function Assert-Stage5NativeFixtureProductionReceiptRawTypes {
    param([object]$Receipt)
    $rootNames = @('schemaVersion', 'evidenceKind', 'producer', 'status',
        'recordedUtc', 'cohortNonce', 'cohortCreatedUtc', 'qualificationMode',
        'acceptanceScope', 'finalAcceptanceClaim', 'performanceScalingClaim',
        'title', 'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'reviewedFixtureManifest', 'prelaunchPlan', 'attemptStart',
        'executableSha256', 'hostRunNonce', 'process', 'commandLine',
        'argumentString', 'taskRoot', 'nativeProfileRoot', 'map', 'rawLog',
        'retainedReplay', 'completion', 'lifecycle')
    Assert-Stage5NativeFixtureExactProperties $Receipt $rootNames `
        'Native fixture production receipt'

    foreach ($name in @('runtimeClosure', 'reviewedFixtureManifest',
            'prelaunchPlan', 'attemptStart', 'process', 'map', 'rawLog',
            'retainedReplay', 'completion', 'lifecycle')) {
        Assert-Stage5NativeFixtureCondition ($Receipt.$name -isnot [Array] -and
            $null -ne $Receipt.$name) `
            "Native fixture production receipt $name must be one object."
    }
    Assert-Stage5NativeFixtureExactProperties $Receipt.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Fixture receipt runtime closure'
    Assert-Stage5NativeFixtureExactProperties $Receipt.reviewedFixtureManifest `
        @('path', 'sha256') 'Fixture receipt reviewed-manifest binding'
    Assert-Stage5NativeFixtureExactProperties $Receipt.prelaunchPlan `
        @('path', 'sha256') 'Fixture receipt prelaunch-plan binding'
    Assert-Stage5NativeFixtureExactProperties $Receipt.attemptStart `
        @('path', 'sha256') 'Fixture receipt attempt-start binding'
    Assert-Stage5NativeFixtureExactProperties $Receipt.process `
        @('id', 'creationTimeUtc100ns', 'exitCode') `
        'Fixture receipt process'
    Assert-Stage5NativeFixtureExactProperties $Receipt.map `
        @('key', 'sha256', 'crc32', 'byteCount') 'Fixture receipt map'
    Assert-Stage5NativeFixtureExactProperties $Receipt.rawLog `
        @('path', 'sha256') 'Fixture receipt raw log binding'
    Assert-Stage5NativeFixtureExactProperties $Receipt.retainedReplay `
        @('path', 'sha256', 'frameCount') 'Fixture receipt replay binding'
    Assert-Stage5NativeFixtureExactProperties $Receipt.completion `
        @('nativeRunNonce', 'mapKey', 'mapSha256', 'mapCrc32', 'mapByteCount',
            'seed', 'frameBudget', 'endFrame', 'winnerTeam', 'finalCrc',
            'replayEpoch', 'aiEpochMarker', 'observedFirstFrame',
            'observedLastFrame', 'observedFrameSamples', 'observedPlayerCount',
            'initialUnitCount', 'peakUnitCount', 'playerUnits', 'replaySha256',
            'replayFrameCount', 'nativeRetainedReplayPath') `
        'Fixture receipt completion'
    Assert-Stage5NativeFixtureExactProperties $Receipt.lifecycle `
        @('mutexAcquired', 'noInstalledTitleProcessesAtPreflight',
            'childExitProven', 'registryRestored', 'profileRemoved',
            'recoveryJournalAbsent') 'Fixture receipt lifecycle'

    Get-Stage5NativeFixtureRawInteger $Receipt.schemaVersion `
        'Fixture receipt schemaVersion' | Out-Null
    foreach ($name in @('evidenceKind', 'producer', 'status', 'recordedUtc',
            'cohortNonce', 'cohortCreatedUtc', 'qualificationMode',
            'acceptanceScope', 'title', 'sourceCommit', 'artifactSetSha256',
            'executableSha256', 'hostRunNonce', 'commandLine', 'argumentString',
            'taskRoot', 'nativeProfileRoot')) {
        Get-Stage5NativeFixtureRawString $Receipt.$name `
            "Fixture receipt $name" | Out-Null
    }
    foreach ($name in @('finalAcceptanceClaim', 'performanceScalingClaim')) {
        Get-Stage5NativeFixtureRawBoolean $Receipt.$name `
            "Fixture receipt $name" | Out-Null
    }
    foreach ($name in @('dependencyManifestSha256', 'closureSha256')) {
        Get-Stage5NativeFixtureRawString $Receipt.runtimeClosure.$name `
            "Fixture receipt runtime closure $name" | Out-Null
    }
    foreach ($binding in @(
            @($Receipt.reviewedFixtureManifest.path, 'Fixture reviewed-manifest path'),
            @($Receipt.reviewedFixtureManifest.sha256, 'Fixture reviewed-manifest SHA-256'),
            @($Receipt.prelaunchPlan.path, 'Fixture prelaunch-plan path'),
            @($Receipt.prelaunchPlan.sha256, 'Fixture prelaunch-plan SHA-256'),
            @($Receipt.attemptStart.path, 'Fixture attempt-start path'),
            @($Receipt.attemptStart.sha256, 'Fixture attempt-start SHA-256'),
            @($Receipt.rawLog.path, 'Fixture raw-log path'),
            @($Receipt.rawLog.sha256, 'Fixture raw-log SHA-256'),
            @($Receipt.retainedReplay.path, 'Fixture replay path'),
            @($Receipt.retainedReplay.sha256, 'Fixture replay SHA-256'))) {
        Get-Stage5NativeFixtureRawString $binding[0] $binding[1] | Out-Null
    }
    foreach ($name in @('id', 'creationTimeUtc100ns', 'exitCode')) {
        Get-Stage5NativeFixtureRawInteger $Receipt.process.$name `
            "Fixture receipt process $name" | Out-Null
    }
    foreach ($name in @('key', 'sha256', 'crc32')) {
        Get-Stage5NativeFixtureRawString $Receipt.map.$name `
            "Fixture receipt map $name" | Out-Null
    }
    Get-Stage5NativeFixtureRawInteger $Receipt.map.byteCount `
        'Fixture receipt map byteCount' | Out-Null
    foreach ($name in @('frameCount')) {
        Get-Stage5NativeFixtureRawInteger $Receipt.retainedReplay.$name `
            "Fixture receipt replay $name" | Out-Null
    }

    foreach ($name in @('nativeRunNonce', 'mapKey', 'mapSha256', 'mapCrc32',
            'aiEpochMarker', 'replaySha256', 'nativeRetainedReplayPath')) {
        Get-Stage5NativeFixtureRawString $Receipt.completion.$name `
            "Fixture receipt completion $name" | Out-Null
    }
    foreach ($name in @('mapByteCount', 'seed', 'frameBudget', 'endFrame',
            'winnerTeam', 'finalCrc', 'replayEpoch', 'observedFirstFrame',
            'observedLastFrame', 'observedFrameSamples', 'observedPlayerCount',
            'initialUnitCount', 'peakUnitCount', 'replayFrameCount')) {
        Get-Stage5NativeFixtureRawInteger $Receipt.completion.$name `
            "Fixture receipt completion $name" | Out-Null
    }
    Get-Stage5NativeFixtureRawArray $Receipt.completion.playerUnits `
        'Fixture receipt completion playerUnits' | Out-Null
    $playerUnits = @($Receipt.completion.playerUnits)
    for ($index = 0; $index -lt $playerUnits.Count; ++$index) {
        Assert-Stage5NativeFixtureExactProperties $playerUnits[$index] `
            @('slot', 'initialUnitCount', 'peakUnitCount') `
            "Fixture receipt completion playerUnits row $index"
        foreach ($name in @('slot', 'initialUnitCount', 'peakUnitCount')) {
            Get-Stage5NativeFixtureRawInteger $playerUnits[$index].$name `
                "Fixture receipt completion playerUnits row $index $name" |
                Out-Null
        }
    }
    foreach ($name in @('mutexAcquired', 'noInstalledTitleProcessesAtPreflight',
            'childExitProven', 'registryRestored', 'profileRemoved',
            'recoveryJournalAbsent')) {
        Get-Stage5NativeFixtureRawBoolean $Receipt.lifecycle.$name `
            "Fixture receipt lifecycle $name" | Out-Null
    }
}

function Assert-Stage5NativePerformanceFixtureProductionReceipt {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Receipt)
    Assert-Stage5NativeFixtureProductionReceiptRawTypes $Receipt
    $schemaPath = Join-Path $PSScriptRoot `
        'Stage5NativePerformanceFixtureProduction.schema.json'
    Assert-Stage5NativeFixtureCondition (Test-Path -LiteralPath $schemaPath `
        -PathType Leaf) 'Native fixture production receipt schema is absent.'
    $json = $Receipt | ConvertTo-Json -Depth 30
    Assert-Stage5NativeFixtureCondition ($json | Test-Json `
        -SchemaFile $schemaPath -ErrorAction SilentlyContinue) `
        'Native fixture production receipt violates its closed schema.'
    [void](Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$Receipt.recordedUtc) 'Fixture receipt recordedUtc')
    $cohortCreated = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$Receipt.cohortCreatedUtc) 'Fixture receipt cohortCreatedUtc'
    $recorded = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$Receipt.recordedUtc) 'Fixture receipt recordedUtc'
    Assert-Stage5NativeFixtureCondition ($recorded -ge $cohortCreated) `
        'Fixture receipt predates its execution cohort.'
    Assert-Stage5NativeFixtureUuidV4 ([string]$Receipt.cohortNonce) `
        'Fixture receipt cohort nonce'
    Assert-Stage5NativeFixtureUuidV4 ([string]$Receipt.hostRunNonce) `
        'Fixture receipt host run nonce'
    foreach ($binding in @(
            @([string]$Receipt.artifactSetSha256, 'Fixture artifact-set SHA-256'),
            @([string]$Receipt.runtimeClosure.dependencyManifestSha256,
                'Fixture dependency-manifest SHA-256'),
            @([string]$Receipt.runtimeClosure.closureSha256,
                'Fixture runtime-closure SHA-256'),
            @([string]$Receipt.reviewedFixtureManifest.sha256,
                'Fixture reviewed-manifest SHA-256'),
            @([string]$Receipt.prelaunchPlan.sha256,
                'Fixture prelaunch-plan SHA-256'),
            @([string]$Receipt.attemptStart.sha256,
                'Fixture attempt-start SHA-256'),
            @([string]$Receipt.executableSha256,
                'Fixture executable SHA-256'),
            @([string]$Receipt.rawLog.sha256, 'Fixture raw-log SHA-256'),
            @([string]$Receipt.retainedReplay.sha256,
                'Fixture replay SHA-256'))) {
        Assert-Stage5NativeFixtureSha256 $binding[0] $binding[1]
    }
    Assert-Stage5NativeFixtureSourceCommit ([string]$Receipt.sourceCommit) `
        'Fixture receipt source commit'
    $completion = $Receipt.completion
    $profileRoot = [IO.Path]::GetFullPath(
        [string]$Receipt.nativeProfileRoot).TrimEnd('\', '/')
    $taskRoot = [IO.Path]::GetFullPath(
        [string]$Receipt.taskRoot).TrimEnd('\', '/')
    $nativeReplayPath = [IO.Path]::GetFullPath(
        [string]$completion.nativeRetainedReplayPath)
    Assert-Stage5NativeFixtureCondition (
        $Receipt.map.key -ceq $completion.mapKey -and
        $Receipt.map.sha256 -ceq $completion.mapSha256 -and
        $Receipt.map.crc32 -ceq $completion.mapCrc32 -and
        [Int64]$Receipt.map.byteCount -eq [Int64]$completion.mapByteCount -and
        $Receipt.retainedReplay.sha256 -ceq $completion.replaySha256 -and
        [Int64]$Receipt.retainedReplay.frameCount -eq
            [Int64]$completion.replayFrameCount -and
        $profileRoot.StartsWith(
            $taskRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -and
        $nativeReplayPath.StartsWith(
            $profileRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Fixture receipt map or replay projection is detached from native completion.'
    $expectedArguments = Get-Stage5NativePerformanceFixtureArgumentString `
        -MapKey ([string]$completion.mapKey) -Seed ([int]$completion.seed) `
        -FrameBudget ([int]$completion.frameBudget) `
        -ExecutableSha256 ([string]$Receipt.executableSha256)
    Assert-Stage5NativeFixtureCondition (
        $Receipt.argumentString -ceq $expectedArguments -and
        $Receipt.commandLine.EndsWith(' ' + $expectedArguments,
            [StringComparison]::Ordinal)) `
        'Fixture receipt command line is detached from the exact physical-4 native plan.'
    $nonceParts = @([string]$completion.nativeRunNonce -split '-')
    $noncePid = Get-Stage5NativeFixtureHexUInt32 $nonceParts[0] `
        'Fixture receipt native nonce process ID'
    $nonceSeed = Get-Stage5NativeFixtureHexUInt32 $nonceParts[2] `
        'Fixture receipt native nonce seed'
    $expectedEpoch = if ($Receipt.title -ceq 'Generals') { 1 } else { 3 }
    $expectedMarker = if ($Receipt.title -ceq 'Generals') {
        ' [GeneralsAIPlanningEpoch=1]'
    } else { ' [SkirmishAIEpoch=3]' }
    Assert-Stage5NativeFixtureCondition (
        [UInt64]$noncePid -eq [UInt64]$Receipt.process.id -and
        $nonceSeed -eq [UInt32]$completion.seed -and
        [int]$completion.replayEpoch -eq $expectedEpoch -and
        $completion.aiEpochMarker -ceq $expectedMarker -and
        [Int64]$completion.initialUnitCount -ge 8000 -and
        [Int64]$completion.peakUnitCount -ge 8000 -and
        [Int64]$completion.observedFrameSamples -eq
            ([Int64]$completion.observedLastFrame -
                [Int64]$completion.observedFirstFrame + 1) -and
        [Int64]$completion.replayFrameCount -eq
            ([Int64]$completion.endFrame + 1)) `
        'Fixture receipt native process, workload, title epoch, or replay closure is invalid.'
    foreach ($name in @('mutexAcquired',
            'noInstalledTitleProcessesAtPreflight', 'childExitProven',
            'registryRestored', 'profileRemoved', 'recoveryJournalAbsent')) {
        Assert-Stage5NativeFixtureCondition (
            $Receipt.lifecycle.$name -is [bool] -and
            [bool]$Receipt.lifecycle.$name) `
            "Fixture receipt lacks lifecycle proof '$name'."
    }
    return $Receipt
}

function Resolve-Stage5NativeFixtureReceiptFile {
    param([string]$Root, [object]$RelativePath, [string]$Context)
    $relative = Get-Stage5NativeFixtureRawString $RelativePath `
        "$Context path"
    Assert-Stage5NativeFixtureCondition (
        -not [string]::IsNullOrWhiteSpace($relative) -and
        -not [IO.Path]::IsPathRooted($relative) -and
        $relative -notmatch ':' -and
        $relative -notmatch '(^|[\\/])\.\.?(?:[\\/]|$)') `
        "$Context must be a safe receipt-relative path."
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath((Join-Path $base $relative))
    Assert-Stage5FinalAcceptancePathContained $base $full $Context
    Assert-Stage5NativeFixtureCondition (Test-Path -LiteralPath $full `
        -PathType Leaf) "$Context was not found: $relative"
    Assert-Stage5FinalAcceptanceNoReparsePath $base $full $Context
    return $full
}

function Read-Stage5NativePerformanceFixtureProductionReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][ValidateSet('Generals', 'ZeroHour')]
        [string]$ExpectedTitle,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortNonce,
        [Parameter(Mandatory = $true)][string]$ExpectedCohortCreatedUtc,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifactSetSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedExecutableSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedDependencyManifestSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedRuntimeClosureSha256
    )
    Assert-Stage5NativeFixtureSha256 $ExpectedSha256 `
        'Expected native fixture production receipt SHA-256'
    Assert-Stage5NativeFixtureUuidV4 $ExpectedCohortNonce `
        'Expected native fixture production cohort nonce'
    [void](Assert-Stage5NativeFixtureCanonicalUtc $ExpectedCohortCreatedUtc `
        'Expected native fixture production cohort timestamp')
    $full = [IO.Path]::GetFullPath($Path)
    $root = Split-Path -Parent $full
    $receiptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
        'Native fixture production receipt' -EvidenceKind JsonReceipt
    Assert-Stage5FinalAcceptanceSnapshotSha256 $receiptSnapshot $ExpectedSha256 `
        'Native fixture production receipt' | Out-Null
    $receipt = ConvertFrom-Stage5NativeFixtureJsonBytes `
        ([byte[]]$receiptSnapshot.bytes) 'Native fixture production receipt'
    Assert-Stage5NativePerformanceFixtureProductionReceipt $receipt | Out-Null
    Assert-Stage5NativeFixtureCondition (
        $receipt.title -ceq $ExpectedTitle -and
        $receipt.cohortNonce -ceq $ExpectedCohortNonce -and
        $receipt.cohortCreatedUtc -ceq $ExpectedCohortCreatedUtc -and
        $receipt.sourceCommit -ceq $ExpectedSourceCommit -and
        $receipt.artifactSetSha256 -ceq $ExpectedArtifactSetSha256 -and
        $receipt.executableSha256 -ceq $ExpectedExecutableSha256 -and
        $receipt.runtimeClosure.dependencyManifestSha256 -ceq
            $ExpectedDependencyManifestSha256 -and
        $receipt.runtimeClosure.closureSha256 -ceq
            $ExpectedRuntimeClosureSha256) `
        'Native fixture production receipt is detached from the exact execution cohort or candidate closure.'

    $reviewedPath = Resolve-Stage5NativeFixtureReceiptFile $root `
        ([string]$receipt.reviewedFixtureManifest.path) `
        'Retained reviewed native fixture manifest'
    $reviewed = Read-Stage5ReviewedNativeKernelFixture -Path $reviewedPath `
        -ExpectedSha256 ([string]$receipt.reviewedFixtureManifest.sha256) `
        -ExpectedTitle $ExpectedTitle `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ExpectedExecutableSha256 $ExpectedExecutableSha256 `
        -ExpectedDependencyManifestSha256 $ExpectedDependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $ExpectedRuntimeClosureSha256
    $planPath = Resolve-Stage5NativeFixtureReceiptFile $root `
        ([string]$receipt.prelaunchPlan.path) `
        'Retained native fixture prelaunch plan'
    $planSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $planPath `
        'Retained native fixture prelaunch plan' -EvidenceKind JsonReceipt
    Assert-Stage5FinalAcceptanceSnapshotSha256 $planSnapshot `
        ([string]$receipt.prelaunchPlan.sha256) `
        'Retained native fixture prelaunch plan' | Out-Null
    $plan = ConvertFrom-Stage5NativeFixtureJsonBytes `
        ([byte[]]$planSnapshot.bytes) 'Retained native fixture prelaunch plan'
    Assert-Stage5NativeFixturePlanRawTypes $plan
    $planRecorded = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$plan.recordedUtc) 'Native fixture plan recordedUtc'
    $cohortCreated = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$receipt.cohortCreatedUtc) 'Native fixture cohortCreatedUtc'
    $receiptRecorded = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$receipt.recordedUtc) 'Native fixture receipt recordedUtc'
    $expectedMapDestination = [IO.Path]::GetFullPath((Join-Path `
        ([string]$receipt.nativeProfileRoot) `
        ([string]$receipt.completion.mapKey)))
    Assert-Stage5NativeFixtureCondition (
        (Test-Stage5JsonInteger $plan.schemaVersion) -and
        $plan.schemaVersion -eq 1 -and
        $plan.event -ceq 'native-fixture-production-plan' -and
        $planRecorded -ge $cohortCreated -and
        $planRecorded -le $receiptRecorded -and
        $plan.hostRunNonce -ceq $receipt.hostRunNonce -and
        $plan.cohortNonce -ceq $receipt.cohortNonce -and
        $plan.cohortCreatedUtc -ceq $receipt.cohortCreatedUtc -and
        $plan.title -ceq $receipt.title -and
        $plan.sourceCommit -ceq $receipt.sourceCommit -and
        $plan.artifactSetSha256 -ceq $receipt.artifactSetSha256 -and
        $plan.runtimeClosure.dependencyManifestSha256 -ceq
            $receipt.runtimeClosure.dependencyManifestSha256 -and
        $plan.runtimeClosure.closureSha256 -ceq
            $receipt.runtimeClosure.closureSha256 -and
        $plan.executableSha256 -ceq $receipt.executableSha256 -and
        $plan.reviewedFixtureManifestSha256 -ceq
            $receipt.reviewedFixtureManifest.sha256 -and
        $plan.mapSha256 -ceq $receipt.map.sha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$plan.mapDestinationPath),
            $expectedMapDestination, [StringComparison]::OrdinalIgnoreCase) -and
        $plan.argumentString -ceq $receipt.argumentString -and
        [String]::Equals([IO.Path]::GetFullPath([string]$plan.taskRoot),
            [IO.Path]::GetFullPath([string]$receipt.taskRoot).TrimEnd('\', '/'),
            [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Stage5JsonInteger $plan.timeoutSeconds) -and
        [int]$plan.timeoutSeconds -gt 0 -and
        (Test-Stage5JsonInteger $plan.workerCount) -and
        [int]$plan.workerCount -eq 4 -and
        $plan.finalAcceptanceClaim -is [bool] -and
        -not [bool]$plan.finalAcceptanceClaim -and
        $plan.performanceScalingClaim -is [bool] -and
        -not [bool]$plan.performanceScalingClaim) `
        'Native fixture prelaunch plan is detached from physical-4 kernel execution authority.'
    $startPath = Resolve-Stage5NativeFixtureReceiptFile $root `
        ([string]$receipt.attemptStart.path) `
        'Retained native fixture attempt start'
    $startSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $startPath `
        'Retained native fixture attempt start' -EvidenceKind JsonReceipt
    Assert-Stage5FinalAcceptanceSnapshotSha256 $startSnapshot `
        ([string]$receipt.attemptStart.sha256) `
        'Retained native fixture attempt start' | Out-Null
    $start = ConvertFrom-Stage5NativeFixtureJsonBytes `
        ([byte[]]$startSnapshot.bytes) 'Retained native fixture attempt start'
    Assert-Stage5NativeFixtureAttemptStartRawTypes $start
    $startRecorded = Assert-Stage5NativeFixtureCanonicalUtc `
        ([string]$start.recordedUtc) 'Native fixture attempt-start recordedUtc'
    Assert-Stage5NativeFixtureCondition (
        (Test-Stage5JsonInteger $start.schemaVersion) -and
        $start.schemaVersion -eq 1 -and
        $start.event -ceq 'native-fixture-production-start' -and
        $startRecorded -ge $planRecorded -and
        $startRecorded -le $receiptRecorded -and
        $start.hostRunNonce -ceq $receipt.hostRunNonce -and
        [String]::Equals([IO.Path]::GetFullPath([string]$start.plan.path),
            [IO.Path]::GetFullPath((Join-Path ([string]$receipt.taskRoot) `
                ([string]$receipt.prelaunchPlan.path))),
            [StringComparison]::OrdinalIgnoreCase) -and
        $start.plan.sha256 -ceq $receipt.prelaunchPlan.sha256) `
        'Native fixture attempt start is detached from its immutable prelaunch plan.'
    $rawPath = Resolve-Stage5NativeFixtureReceiptFile $root `
        ([string]$receipt.rawLog.path) 'Retained native fixture raw log'
    $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
        'Retained native fixture raw log' -EvidenceKind RawLog
    Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot `
        ([string]$receipt.rawLog.sha256) 'Retained native fixture raw log' |
        Out-Null
    $replayPath = Resolve-Stage5NativeFixtureReceiptFile $root `
        ([string]$receipt.retainedReplay.path) `
        'Retained native performance fixture replay'
    $replaySnapshot = Get-Stage5FinalAcceptanceFileSnapshot $replayPath `
        'Retained native performance fixture replay' -EvidenceKind Replay
    Assert-Stage5FinalAcceptanceSnapshotSha256 $replaySnapshot `
        ([string]$receipt.retainedReplay.sha256) `
        'Retained native performance fixture replay' | Out-Null
    $rawText = try {
        (New-Object Text.UTF8Encoding($false, $true)).GetString(
            [byte[]]$rawSnapshot.bytes)
    }
    catch {
        throw "Retained native fixture raw log is not strict UTF-8: $($_.Exception.Message)"
    }
    $reparsed = ConvertFrom-Stage5NativePerformanceFixtureOutput `
        -Text $rawText -ReviewedFixture $reviewed `
        -ExpectedProcessId ([Int64]$receipt.process.id) `
        -ProfileRoot ([string]$receipt.nativeProfileRoot) `
        -RetainedReplaySnapshot $replaySnapshot
    $projection = Get-Stage5NativeFixtureCompletionProjection $reparsed
    Assert-Stage5NativeFixtureCondition (
        (ConvertTo-Json $projection -Depth 20 -Compress) -ceq
            (ConvertTo-Json $receipt.completion -Depth 20 -Compress)) `
        'Native fixture receipt completion differs from the independently reparsed raw log.'

    return [pscustomobject]@{
        path = $full
        sha256 = $ExpectedSha256
        receipt = $receipt
        receiptSnapshot = $receiptSnapshot
        reviewedFixture = $reviewed
        prelaunchPlan = [pscustomobject]@{
            path = $planPath; sha256 = [string]$planSnapshot.sha256
        }
        attemptStart = [pscustomobject]@{
            path = $startPath; sha256 = [string]$startSnapshot.sha256
        }
        rawLog = [pscustomobject]@{
            path = $rawPath; sha256 = [string]$rawSnapshot.sha256
        }
        retainedReplay = [pscustomobject]@{
            path = $replayPath; sha256 = [string]$replaySnapshot.sha256
            frameCount = [Int64]$receipt.retainedReplay.frameCount
        }
        fixture = [pscustomobject]@{
            id = 'dense-eight-player'
            path = $replayPath
            sha256 = [string]$replaySnapshot.sha256
            seed = [int]$receipt.completion.seed
            playerCount = 8
            peakUnitCount = [Int64]$receipt.completion.peakUnitCount
        }
        filePaths = @($full, $reviewedPath,
            [string]$reviewed.fixture.sourcePath, $planPath, $startPath,
            $rawPath, $replayPath)
    }
}

Export-ModuleMember -Function Read-Stage5ReviewedNativeKernelFixture, `
    Read-Stage5NativeFixtureDiagnosticInput, ConvertFrom-Stage5NativeFixtureObservation, `
    Assert-Stage5NativeFixtureProductionCompletion, `
    Write-Stage5NativeFixtureCapturedOutput, New-Stage5NativeFixtureDiagnosticResult, `
    Get-Stage5NativePerformanceFixtureArgumentString, `
    ConvertFrom-Stage5NativePerformanceFixtureOutput, `
    New-Stage5NativePerformanceFixtureProductionReceipt, `
    Assert-Stage5NativePerformanceFixtureProductionReceipt, `
    Read-Stage5NativePerformanceFixtureProductionReceipt
