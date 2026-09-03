[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [ValidateSet('Generals', 'ZeroHour')]
    [string]$Title,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$InstalledExecutablePath,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$ExpectedExecutableSha256,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$ExpectedSourceCommit,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$ExpectedArtifactSetSha256,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$ArtifactSetManifestPath,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [switch]$AllowHeadlessDirectExecution,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$FixtureManifestPath,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$ExpectedFixtureManifestSha256,

    [Parameter(ParameterSetName = 'Run')]
    [string]$Stage3BaselinePath,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedStage3BaselineSha256,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedStage3ExecutableSha256,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedStage3SourceCommit,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [string]$TaskRoot,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(3, 100)]
    [int]$MeasuredRuns = 3,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(1, 86400)]
    [int]$TimeoutSeconds = 7200,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('External16Core', 'LocalCapacitySmoke')]
    [string]$QualificationMode = 'External16Core',

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('throughput-only', 'paired-serial-oracle-v1')]
    [string]$ReferencePolicy = 'throughput-only',

    # This parameter set exists only for host-side contract tests. It consumes
    # already-created synthetic receipts and can never reach Process.Start().
    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)]
    [string]$SelfTestValidationManifestPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

$script:CanonicalFixtureIds = @(
    'one-thousand-units',
    'four-thousand-units',
    'eight-thousand-units',
    'dense-eight-player'
)
$script:CanonicalFixtureUnits = @(1000, 4000, 8000, 8000)
$script:ExternalLaneNames = @('forced-one', 'physical-8', 'physical-16')
$script:ExternalLaneWorkers = @(1, 8, 16)
$script:LocalLaneNames = @('forced-one', 'physical-2', 'physical-4')
$script:LocalLaneWorkers = @(1, 2, 4)
$script:WarmupRuns = 1
$script:VerifierBoundary = 'stage5-host-independent-correlation-v1'
$script:Stage5FatalPattern = '(?i)(CRC Mismatch|game thread ownership violation|assertion failed|fatal error|missing map|replay read error|SKIRMISH_AI_TEST_FAIL|SIMULATION_JOB_SYSTEM_FALLBACK|SIMULATION_SHADOW_(?:MISMATCH|FAIL)|SIMULATION_COLLISION_MISMATCH)'
$script:Stage5ValidationMutexName = $null
try {
    # Registry/profile redirection is user-scoped, so serialize cooperative
    # validators across the user's sessions without contending with another
    # Windows account.  Fail closed if the identity cannot be resolved.
    $validationUserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if ([string]::IsNullOrWhiteSpace([string]$validationUserSid)) {
        throw 'the current Windows user SID was unavailable'
    }
    $script:Stage5ValidationMutexName =
        "Global\GeneralsGameCode.Stage5PerformanceValidation-$validationUserSid"
}
catch {
    throw "Could not establish the Stage 5 installed-validation mutex identity: $($_.Exception.Message)"
}

function Get-Stage5LaneNames {
    param([string]$Mode)
    if ($Mode -ceq 'LocalCapacitySmoke') { return $script:LocalLaneNames }
    return $script:ExternalLaneNames
}

function Get-Stage5LaneWorkers {
    param([string]$Mode)
    if ($Mode -ceq 'LocalCapacitySmoke') { return $script:LocalLaneWorkers }
    return $script:ExternalLaneWorkers
}

function Assert-Stage5PerformanceCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Acquire-Stage5ValidationMutex {
    $mutex = $null
    try {
        [bool]$createdNew = $false
        $mutex = New-Object Threading.Mutex($false,
            $script:Stage5ValidationMutexName, [ref]$createdNew)
        if (-not $mutex.WaitOne(0)) {
            throw 'Another Stage 5 installed validation already owns the title-session registry contract.'
        }
        return [pscustomobject]@{ mutex = $mutex; acquired = $true }
    }
    catch {
        if ($null -ne $mutex) { $mutex.Dispose() }
        throw "Could not acquire the Stage 5 installed-validation mutex: $($_.Exception.Message)"
    }
}

function Release-Stage5ValidationMutex {
    param([object]$Lock)
    if ($null -eq $Lock) { return }
    $errors = New-Object 'Collections.Generic.List[string]'
    if ([bool]$Lock.acquired) {
        try { $Lock.mutex.ReleaseMutex() }
        catch { $errors.Add("release: $($_.Exception.Message)") | Out-Null }
    }
    try { $Lock.mutex.Dispose() }
    catch { $errors.Add("dispose: $($_.Exception.Message)") | Out-Null }
    if ($errors.Count -gt 0) {
        throw "Stage 5 installed-validation mutex cleanup failed: $($errors.ToArray() -join ' | ')"
    }
}

function Assert-Stage5NoInstalledTitleProcesses {
    $running = @(Get-Process -Name generalsv, generalszh -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        $details = @($running | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" })
        throw "Cannot swap the title-session registry contract while an installed title is running: $($details -join ', ')."
    }
}

function Get-Stage5PerformanceSha256 {
    param([string]$Path)
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $Path -PathType Leaf) `
        "File was not found: $Path"
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return (($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $algorithm.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Assert-Stage5PerformanceHash {
    param([string]$Value, [string]$Name)
    Assert-Stage5PerformanceCondition ($Value -cmatch '^[0-9A-F]{64}$') `
        "$Name must be an independently supplied uppercase SHA-256."
}

function Assert-Stage5PerformanceSourceCommit {
    param([string]$Value, [string]$Name)
    Assert-Stage5PerformanceCondition ($Value -cmatch '^[0-9a-f]{40}$') `
        "$Name must be an independently supplied lowercase 40-hex commit."
    $repository = $null
    try {
        $repository = (& git -C $PSScriptRoot rev-parse --show-toplevel 2>$null |
            Select-Object -First 1)
    }
    catch { }
    Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace([string]$repository)) `
        "Cannot resolve a Git repository to verify $Name."
    & git -C ([string]$repository).Trim() cat-file -e "${Value}^{commit}" 2>$null
    Assert-Stage5PerformanceCondition ($LASTEXITCODE -eq 0) `
        "$Name object is not present in the checked-out repository."
}

function Assert-Stage5PerformanceFileHash {
    param([string]$Path, [string]$Expected, [string]$Name)
    Assert-Stage5PerformanceHash $Expected $Name
    $actual = Get-Stage5PerformanceSha256 $Path
    Assert-Stage5PerformanceCondition ($actual -ceq $Expected) `
        "$Name mismatch. Expected $Expected, got $actual."
    return $actual
}

function Read-Stage5PerformanceJson {
    param([string]$Path, [string]$Context)
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $Path -PathType Leaf) `
        "$Context was not found: $Path"
    try {
        $json = Get-Content -LiteralPath $Path -Raw
        $convertFromJson = Get-Command ConvertFrom-Json
        if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
            # PowerShell 7 otherwise converts ISO strings into DateTime values,
            # losing the producer's exact UTC representation and breaking the
            # receipt's literal cohort/provenance binding.
            return $json | ConvertFrom-Json -DateKind String
        }
        return $json | ConvertFrom-Json
    }
    catch { throw "$Context is not valid JSON: $($_.Exception.Message)" }
}

function Read-Stage5PerformanceArtifactSet {
    param([string]$Path, [string]$ExpectedHash, [string]$ExpectedSourceCommit,
        [string]$ExpectedTitle, [string]$ExpectedExecutablePath,
        [string]$ExpectedExecutableHash)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5PerformanceFileHash $full $ExpectedHash `
        'Reviewed artifact-set manifest SHA-256' | Out-Null
    try { $document = ConvertFrom-Stage5JsonDictionary $full }
    catch { throw "Reviewed artifact-set manifest is not valid JSON: $($_.Exception.Message)" }
    Assert-Stage5JsonShape $document @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'artifacts', 'runtimeClosure') `
        'Reviewed artifact-set manifest'
    Assert-Stage5PerformanceCondition ([int]$document['schemaVersion'] -eq 1 -and
        [string]$document['sourceCommit'] -ceq $ExpectedSourceCommit -and
        [string]$document['architecture'] -ceq 'x64') `
        'Reviewed artifact-set identity does not match the requested native x64 source revision.'
    $productSet = $document['productSet']
    Assert-Stage5PerformanceCondition ($productSet -is [Array] -and
        $productSet.Count -eq 2 -and
        @('Generals', 'ZeroHour') -ccontains [string]$productSet[0] -and
        @('Generals', 'ZeroHour') -ccontains [string]$productSet[1] -and
        [string]$productSet[0] -cne [string]$productSet[1]) `
        'Reviewed artifact set must contain exactly Generals and ZeroHour.'

    # This is the independent closure read.  It rehashes the dependency
    # manifest, every declared DLL/asset, and every core artifact before a
    # game process can be started.
    $artifactDirectory = Split-Path -Parent $full
    $closureBinding = Get-Stage5RuntimeClosureBinding `
        -ArtifactSet $document -ArtifactDirectory $artifactDirectory `
        -ExpectedSourceCommit $ExpectedSourceCommit `
        -Context 'Reviewed artifact-set runtime closure'
    $runtimeClosure = [pscustomobject]@{
        dependencyManifestPath = [string]$closureBinding.dependencyManifestPath
        dependencyManifestSha256 = [string]$closureBinding.dependencyManifestSha256
        closureSha256 = [string]$closureBinding.closureSha256
        fileCount = [int]$closureBinding.fileCount
        # Keep only canonical paths for the long-lived read-only lock set.  Do
        # not retain module snapshots/bytes across the performance matrix.
        filePaths = @($closureBinding.files | ForEach-Object {
            [IO.Path]::GetFullPath([string]$_.fullPath)
        })
    }

    $requiredRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifacts = $document['artifacts']
    Assert-Stage5PerformanceCondition ($artifacts -is [Array] -and
        $artifacts.Count -eq $requiredRoles.Count) `
        'Reviewed artifact set must contain exactly six installed product artifacts.'
    $resolved = @{}
    foreach ($entry in $artifacts) {
        Assert-Stage5JsonShape $entry @('role', 'path', 'sha256') `
            'Reviewed artifact entry'
        $role = [string](Get-Stage5JsonValue $entry 'role' 'Reviewed artifact entry')
        Assert-Stage5PerformanceCondition ($requiredRoles -ccontains $role -and
            -not $resolved.ContainsKey($role)) `
            "Reviewed artifact role is missing, duplicated, or unsupported: $role"
        $relative = [string](Get-Stage5JsonValue $entry 'path' 'Reviewed artifact entry')
        $artifactPath = Resolve-Stage5PerformanceManifestFile $artifactDirectory `
            $relative "Reviewed artifact '$role'"
        $artifactHash = [string](Get-Stage5JsonValue $entry 'sha256' `
            'Reviewed artifact entry')
        Assert-Stage5PerformanceHash $artifactHash "Reviewed artifact '$role' SHA-256"
        Assert-Stage5PerformanceFileHash $artifactPath $artifactHash `
            "Reviewed artifact '$role' SHA-256" | Out-Null
        $resolved[$role] = [pscustomobject]@{
            path = $artifactPath; sha256 = $artifactHash.ToUpperInvariant()
        }
    }
    foreach ($role in $requiredRoles) {
        Assert-Stage5PerformanceCondition $resolved.ContainsKey($role) `
            "Reviewed artifact set is missing role '$role'."
    }
    $executableRole = if ($ExpectedTitle -ceq 'Generals') {
        'generals-executable'
    } else { 'zerohour-executable' }
    Assert-Stage5PerformanceCondition (
        [String]::Equals([IO.Path]::GetFullPath([string]$resolved[$executableRole].path),
            [IO.Path]::GetFullPath($ExpectedExecutablePath),
            [StringComparison]::OrdinalIgnoreCase) -and
        [string]$resolved[$executableRole].sha256 -ceq $ExpectedExecutableHash) `
        "Reviewed artifact set does not bind the requested $ExpectedTitle executable."
    return [pscustomobject]@{
        path = $full
        sha256 = $ExpectedHash
        runtimeClosure = $runtimeClosure
        artifacts = $resolved
    }
}

function Assert-Stage5PerformanceProperties {
    param([object]$Value, [string[]]$Names, [string]$Context,
        [switch]$AllowAdditional)
    Assert-Stage5PerformanceCondition ($null -ne $Value) "$Context is null."
    $actual = @($Value.PSObject.Properties | ForEach-Object { $_.Name })
    foreach ($name in $Names) {
        Assert-Stage5PerformanceCondition ($actual -ccontains $name) `
            "$Context is missing '$name'."
    }
    if (-not $AllowAdditional) {
        foreach ($name in $actual) {
            Assert-Stage5PerformanceCondition ($Names -ccontains $name) `
                "$Context contains unsupported property '$name'."
        }
    }
}

function Resolve-Stage5PerformanceManifestFile {
    param([string]$ManifestDirectory, [string]$RelativePath, [string]$Context)
    Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace($RelativePath)) `
        "$Context path is empty."
    Assert-Stage5PerformanceCondition (-not [IO.Path]::IsPathRooted($RelativePath)) `
        "$Context must be manifest-relative."
    $base = [IO.Path]::GetFullPath($ManifestDirectory)
    $candidate = [IO.Path]::GetFullPath((Join-Path $base $RelativePath))
    Assert-Stage5PerformanceCondition ($candidate.StartsWith(
        $base + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context escapes the manifest directory."
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $candidate -PathType Leaf) `
        "$Context was not found: $RelativePath"
    return $candidate
}

function Get-Stage5PerformanceMedian {
    param([double[]]$Values)
    Assert-Stage5PerformanceCondition ($Values.Count -gt 0) 'Median requires samples.'
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-Stage5PerformanceMaskBitCount {
    param([UInt64]$Value)
    $count = 0
    while ($Value -ne 0) {
        $count += [int]($Value -band 1)
        $Value = $Value -shr 1
    }
    return $count
}

function Test-Stage5PerformanceFinitePositive {
    param([object]$Value)
    try { $number = [double]$Value }
    catch { return $false }
    return -not [double]::IsNaN($number) -and
        -not [double]::IsInfinity($number) -and $number -gt 0.0
}

function Get-Stage5PeMachine {
    param([string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            Assert-Stage5PerformanceCondition ($reader.ReadUInt16() -eq 0x5A4D) `
                'Installed executable is not a PE image.'
            $stream.Position = 0x3C
            $peOffset = $reader.ReadUInt32()
            Assert-Stage5PerformanceCondition ($peOffset -ge 0x40 -and
                $peOffset -le ($stream.Length - 26)) `
                'Installed executable has an invalid PE header offset.'
            $stream.Position = $peOffset
            Assert-Stage5PerformanceCondition ($reader.ReadUInt32() -eq 0x00004550) `
                'Installed executable has no PE signature.'
            $machine = $reader.ReadUInt16()
            $stream.Position = $peOffset + 24
            $optionalMagic = $reader.ReadUInt16()
            Assert-Stage5PerformanceCondition ($optionalMagic -eq 0x20B) `
                'Installed executable is not a native PE32+ x64 image.'
            return $machine
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Read-Stage5ScalingFixtureManifest {
    param([string]$Path, [string]$ExpectedHash, [string]$ExpectedTitle,
        [string]$ExecutableHash)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5PerformanceFileHash $full $ExpectedHash `
        'Reviewed fixture manifest SHA-256' | Out-Null
    $document = Read-Stage5PerformanceJson $full 'Reviewed fixture manifest'
    Assert-Stage5PerformanceProperties $document @('schemaVersion', 'evidenceKind',
        'title', 'executableSha256', 'fixtures') 'Reviewed fixture manifest'
    Assert-Stage5PerformanceCondition ($document.schemaVersion -eq 1 -and
        $document.evidenceKind -ceq 'stage5-performance-scaling-fixtures' -and
        $document.title -ceq $ExpectedTitle -and
        $document.executableSha256 -ceq $ExecutableHash) `
        'Reviewed fixture manifest identity does not match the exact candidate.'
    $fixtures = @($document.fixtures)
    Assert-Stage5PerformanceCondition ($fixtures.Count -eq 4) `
        'Reviewed fixture manifest requires exactly four canonical fixtures.'
    $manifestDirectory = Split-Path -Parent $full
    $result = @()
    for ($index = 0; $index -lt 4; ++$index) {
        $fixture = $fixtures[$index]
        $context = "Reviewed fixture $index"
        Assert-Stage5PerformanceProperties $fixture @('id', 'source', 'sha256',
            'seed', 'playerCount', 'peakUnitCount') $context
        $expectedUnits = $script:CanonicalFixtureUnits[$index]
        [UInt32]$seedValue = 0
        $seedValid = [UInt32]::TryParse([string]$fixture.seed, [ref]$seedValue)
        $unitIdentityValid = if ($index -eq 3) {
            [int]$fixture.peakUnitCount -ge $expectedUnits
        } else { [int]$fixture.peakUnitCount -eq $expectedUnits }
        Assert-Stage5PerformanceCondition ($fixture.id -ceq
            $script:CanonicalFixtureIds[$index] -and
            $fixture.sha256 -cmatch '^[0-9A-F]{64}$' -and
            $seedValid -and
            [int]$fixture.playerCount -eq 8 -and $unitIdentityValid) `
            "$context does not contain canonical 1000/4000/8000/dense8 metadata."
        $fixturePath = Resolve-Stage5PerformanceManifestFile $manifestDirectory `
            ([string]$fixture.source) "$context source"
        Assert-Stage5PerformanceFileHash $fixturePath ([string]$fixture.sha256) `
            "$context SHA-256" | Out-Null
        $result += [pscustomobject]@{
            id = [string]$fixture.id
            path = $fixturePath
            sha256 = [string]$fixture.sha256
            seed = $seedValue
            playerCount = 8
            peakUnitCount = [int]$fixture.peakUnitCount
        }
    }
    return [pscustomobject]@{ path = $full; sha256 = $ExpectedHash; fixtures = $result }
}

function Read-Stage5ScalingBaseline {
    param([string]$Path, [string]$ExpectedHash, [string]$ExpectedExecutableHash,
        [string]$ExpectedTitle, [string]$FixtureManifestHash, [object[]]$Fixtures,
        [string]$ExpectedSourceCommit)
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5PerformanceFileHash $full $ExpectedHash `
        'Stage 3 baseline SHA-256' | Out-Null
    $document = Read-Stage5PerformanceJson $full 'Stage 3 performance baseline'
    Assert-Stage5PerformanceProperties $document @('schemaVersion', 'stage',
        'architecture', 'title', 'executableSha256', 'fixtureManifestSha256',
        'configuration', 'physicalCoreCount', 'availableCpus',
        'logicalProcessorCount', 'warmupRuns', 'fixtures') `
        'Stage 3 performance baseline'
    Assert-Stage5PerformanceCondition ($document.schemaVersion -eq 1 -and
        $document.stage -ceq 'Stage3' -and $document.architecture -ceq 'x64' -and
        $document.title -ceq $ExpectedTitle -and
        $document.executableSha256 -ceq $ExpectedExecutableHash -and
        $document.fixtureManifestSha256 -ceq $FixtureManifestHash -and
        $document.configuration -ceq 'parallel-1' -and
        [int]$document.availableCpus -ge [int]$document.physicalCoreCount -and
        [int]$document.warmupRuns -eq 1) `
        'Stage 3 baseline identity or exact hash binding is invalid.'
    Assert-Stage5PerformanceCondition ([int]$document.physicalCoreCount -ge 16 -and
        [int]$document.logicalProcessorCount -ge 16) `
        'Stage 3 baseline lacks the required 16-physical-core topology.'
    $baselineFixtures = @($document.fixtures)
    Assert-Stage5PerformanceCondition ($baselineFixtures.Count -eq 4) `
        'Stage 3 baseline requires all four canonical fixtures.'
    $result = @()
    for ($index = 0; $index -lt 4; ++$index) {
        $fixture = $baselineFixtures[$index]
        $context = "Stage 3 baseline fixture $index"
        Assert-Stage5PerformanceProperties $fixture @('id', 'fixtureSha256',
            'playerCount', 'peakUnitCount', 'wallMilliseconds') $context
        $samples = @($fixture.wallMilliseconds | ForEach-Object { [double]$_ })
        Assert-Stage5PerformanceCondition ($fixture.id -ceq $Fixtures[$index].id -and
            $fixture.fixtureSha256 -ceq $Fixtures[$index].sha256 -and
            [int]$fixture.playerCount -eq 8 -and
            [int]$fixture.peakUnitCount -eq $Fixtures[$index].peakUnitCount -and
            $samples.Count -ge 4 -and
            @($samples | Where-Object {
                -not (Test-Stage5PerformanceFinitePositive $_)
            }).Count -eq 0) `
            "$context metadata or warmup/measured samples are invalid."
        $result += [pscustomobject]@{
            id = [string]$fixture.id
            rawWallMilliseconds = $samples
            measuredMedianMilliseconds = Get-Stage5PerformanceMedian `
                @($samples | Select-Object -Skip 1)
        }
    }
    return [pscustomobject]@{
        path = $full
        sha256 = $ExpectedHash
        executableSha256 = $ExpectedExecutableHash
        stage3SourceCommit = $ExpectedSourceCommit
        physicalCoreCount = [int]$document.physicalCoreCount
        availableCpus = [int]$document.availableCpus
        logicalProcessorCount = [int]$document.logicalProcessorCount
        fixtures = $result
    }
}

function Get-Stage5SystemCpuSets {
    if (-not ('Stage5PerformanceNative' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Stage5PerformanceNative {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetSystemCpuSetInformation(
        IntPtr information, uint bufferLength, out uint returnedLength,
        IntPtr process, uint flags);
}
'@
    }
    [UInt32]$required = 0
    [void][Stage5PerformanceNative]::GetSystemCpuSetInformation(
        [IntPtr]::Zero, 0, [ref]$required, [IntPtr]::Zero, 0)
    Assert-Stage5PerformanceCondition ($required -ge 32) `
        'GetSystemCpuSetInformation returned no topology.'
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal([int]$required)
    try {
        [UInt32]$written = 0
        Assert-Stage5PerformanceCondition (
            [Stage5PerformanceNative]::GetSystemCpuSetInformation(
                $buffer, $required, [ref]$written, [IntPtr]::Zero, 0)) `
            'GetSystemCpuSetInformation failed.'
        $rows = @()
        $offset = 0
        while ($offset -lt $written) {
            $entry = [IntPtr]::Add($buffer, $offset)
            $size = [Runtime.InteropServices.Marshal]::ReadInt32($entry, 0)
            $type = [Runtime.InteropServices.Marshal]::ReadInt32($entry, 4)
            Assert-Stage5PerformanceCondition ($size -ge 8 -and
                ($offset + $size) -le $written) `
                'GetSystemCpuSetInformation returned a malformed entry.'
            if ($type -eq 0 -and $size -ge 32) {
                $flags = [Runtime.InteropServices.Marshal]::ReadByte($entry, 19)
                $rows += [pscustomobject]@{
                    id = [UInt32][Runtime.InteropServices.Marshal]::ReadInt32($entry, 8)
                    group = [UInt16][Runtime.InteropServices.Marshal]::ReadInt16($entry, 12)
                    logicalProcessorIndex = [byte][Runtime.InteropServices.Marshal]::ReadByte($entry, 14)
                    coreIndex = [byte][Runtime.InteropServices.Marshal]::ReadByte($entry, 15)
                    efficiencyClass = [byte][Runtime.InteropServices.Marshal]::ReadByte($entry, 18)
                    parked = (($flags -band 1) -ne 0)
                    allocated = (($flags -band 2) -ne 0)
                    available = (($flags -band 3) -eq 0)
                }
            }
            $offset += $size
        }
        return $rows
    }
    finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }
}

function Get-Stage5HostTopology {
    param(
        [int]$MinimumPhysicalCores = 16,
        [int]$MaximumPhysicalCores = 0,
        [int]$MaximumLogicalProcessors = 0
    )
    $cpuSets = @(Get-Stage5SystemCpuSets)
    $available = @($cpuSets | Where-Object { $_.available })
    $physical = @{}
    foreach ($cpuSet in $available) {
        $physical["$($cpuSet.group):$($cpuSet.coreIndex)"] = $true
    }
    Assert-Stage5PerformanceCondition ($physical.Count -ge $MinimumPhysicalCores) `
        "Stage 5 performance qualification requires at least $MinimumPhysicalCores available physical cores; host exposes $($physical.Count)."
    if ($MaximumPhysicalCores -gt 0) {
        Assert-Stage5PerformanceCondition ($physical.Count -le $MaximumPhysicalCores) `
            "Local capacity smoke requires at most $MaximumPhysicalCores physical cores; host exposes $($physical.Count)."
    }
    if ($MaximumLogicalProcessors -gt 0) {
        Assert-Stage5PerformanceCondition ($available.Count -le $MaximumLogicalProcessors) `
            "Local capacity smoke requires at most $MaximumLogicalProcessors available logical processors; host exposes $($available.Count)."
    }
    return [pscustomobject]@{
        source = 'GetSystemCpuSetInformation'
        physicalCoreCount = $physical.Count
        logicalProcessorCount = $available.Count
        cpuSets = $available
    }
}

function ConvertTo-Stage5WindowsArgument {
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
        if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Get-Stage5PerformanceArguments {
    param([object]$Fixture, [int]$WorkerCount, [string]$ExecutableHash)
    $values = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
        '-simulationMode', 'parallel', '-workerPolicy', 'auto',
        '-validationExecutableSha256', $ExecutableHash, '-workerCount',
        [string]$WorkerCount, '-replay', $Fixture.path)
    return (@($values | ForEach-Object {
        ConvertTo-Stage5WindowsArgument ([string]$_)
    }) -join ' ')
}

function Get-Stage5ProcessCommandLine {
    param([int]$ProcessId)
    try {
        $record = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId"
        if ($null -ne $record -and -not [string]::IsNullOrWhiteSpace($record.CommandLine)) {
            return [string]$record.CommandLine
        }
    }
    catch { }
    try {
        $record = Get-WmiObject Win32_Process -Filter "ProcessId = $ProcessId"
        if ($null -ne $record -and -not [string]::IsNullOrWhiteSpace($record.CommandLine)) {
            return [string]$record.CommandLine
        }
    }
    catch { }
    throw "Host could not independently capture command line for PID $ProcessId."
}

function Get-Stage5LauncherContract {
    param([string]$RuntimeDirectory, [string]$Executable)
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    $launcherPath = Join-Path $runtimeFull 'launcher.exe'
    $configPath = Join-Path $runtimeFull 'launcher.lcf'
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $launcherPath -PathType Leaf) `
        "Installed runtime launcher was not found: $launcherPath"
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $configPath -PathType Leaf) `
        "Installed runtime launcher configuration was not found: $configPath"
    $configLines = @(Get-Content -LiteralPath $configPath)
    $runLines = @($configLines | Where-Object { $_ -match '^\s*RUN\s*=' })
    $otherLines = @($configLines | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        $_ -notmatch '^\s*RUN\s*='
    })
    Assert-Stage5PerformanceCondition ($runLines.Count -eq 1) `
        'launcher.lcf must contain exactly one RUN entry for performance validation.'
    Assert-Stage5PerformanceCondition ($otherLines.Count -eq 0) `
        'launcher.lcf contains an unsupported nonblank directive.'
    $match = [regex]::Match($runLines[0],
        '^\s*RUN\s*=\s*(?<directory>\S+)\s+(?<executable>"[^"]+"|\S+)(?<arguments>.*)$')
    Assert-Stage5PerformanceCondition $match.Success `
        'launcher.lcf RUN entry has an unsupported shape.'
    $directory = $match.Groups['directory'].Value
    Assert-Stage5PerformanceCondition ($directory -ceq '.') `
        "launcher.lcf RUN working directory must be '.', got '$directory'."
    $configuredExecutable = $match.Groups['executable'].Value.Trim('"')
    Assert-Stage5PerformanceCondition ($configuredExecutable -match '^[A-Za-z0-9._-]+\.exe$') `
        'launcher.lcf RUN target must be a leaf executable name.'
    $expectedExecutable = [IO.Path]::GetFileName([IO.Path]::GetFullPath($Executable))
    Assert-Stage5PerformanceCondition ($configuredExecutable -ceq $expectedExecutable) `
        "launcher.lcf target '$configuredExecutable' does not match '$expectedExecutable'."
    $argumentText = $match.Groups['arguments'].Value.Trim()
    $arguments = @()
    if (-not [string]::IsNullOrWhiteSpace($argumentText)) {
        $argumentMatches = [regex]::Matches($argumentText,
            '"(?<quoted>(?:[^"]|"")*)"|(?<bare>\S+)')
        $consumed = 0
        foreach ($argumentMatch in $argumentMatches) {
            Assert-Stage5PerformanceCondition (
                $argumentMatch.Index -eq $consumed -or
                $argumentText.Substring($consumed,
                    $argumentMatch.Index - $consumed) -match '^\s+$') `
                'launcher.lcf RUN arguments contain an unsupported token.'
            $arguments += if ($argumentMatch.Groups['quoted'].Success) {
                $argumentMatch.Groups['quoted'].Value.Replace('""', '"')
            } else { $argumentMatch.Groups['bare'].Value }
            $consumed = $argumentMatch.Index + $argumentMatch.Length
        }
        Assert-Stage5PerformanceCondition ($consumed -eq $argumentText.Length) `
            'launcher.lcf RUN arguments contain an unsupported trailing token.'
    }
    Assert-Stage5PerformanceCondition ($arguments.Count -eq 0 -or
        ($arguments.Count -eq 4 -and $arguments[0] -ceq '-simulationMode' -and
            $arguments[1] -ceq 'parallel' -and $arguments[2] -ceq '-workerPolicy' -and
            $arguments[3] -ceq 'auto')) `
        'launcher.lcf may only contribute the reviewed native Stage 5 defaults.'
    return [pscustomobject]@{
        launcherPath = [IO.Path]::GetFullPath($launcherPath)
        launcherSha256 = Get-Stage5PerformanceSha256 $launcherPath
        configPath = [IO.Path]::GetFullPath($configPath)
        configSha256 = Get-Stage5PerformanceSha256 $configPath
        directory = $directory
        executable = $configuredExecutable
        arguments = @($arguments)
        workingDirectory = $runtimeFull
        directException = 'launcher-main-does-not-propagate-child-exit-code'
    }
}

function Assert-Stage5PerformanceLauncherBinding {
    param([object]$ArtifactBinding, [object]$LauncherContract,
        [string]$Title)
    Assert-Stage5PerformanceCondition ($null -ne $ArtifactBinding -and
        $null -ne $LauncherContract) `
        "Stage 5 $Title launcher binding inputs are incomplete."
    $prefix = if ($Title -ceq 'Generals') { 'generals' } else { 'zerohour' }
    $launcherRole = $prefix + '-launcher'
    $configRole = $prefix + '-launcher-config'
    Assert-Stage5PerformanceCondition ($ArtifactBinding.artifacts.ContainsKey($launcherRole) -and
        $ArtifactBinding.artifacts.ContainsKey($configRole)) `
        "Reviewed artifact set is missing the $Title launcher roles."
    $launcher = $ArtifactBinding.artifacts[$launcherRole]
    $config = $ArtifactBinding.artifacts[$configRole]
    $launcherPath = [IO.Path]::GetFullPath([string]$LauncherContract.launcherPath)
    $configPath = [IO.Path]::GetFullPath([string]$LauncherContract.configPath)
    Assert-Stage5PerformanceCondition (
        [String]::Equals([IO.Path]::GetFullPath([string]$launcher.path),
            $launcherPath, [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals([IO.Path]::GetFullPath([string]$config.path),
            $configPath, [StringComparison]::OrdinalIgnoreCase) -and
        [string]$launcher.sha256 -ceq [string]$LauncherContract.launcherSha256 -and
        [string]$config.sha256 -ceq [string]$LauncherContract.configSha256) `
        "Reviewed artifact set does not bind the exact installed $Title launcher and configuration."
    Assert-Stage5PerformanceFileHash $launcherPath ([string]$launcher.sha256) `
        "Installed $Title launcher SHA-256" | Out-Null
    Assert-Stage5PerformanceFileHash $configPath ([string]$config.sha256) `
        "Installed $Title launcher configuration SHA-256" | Out-Null
}

function Get-Stage5ProcessIdentity {
    param([Diagnostics.Process]$Process)
    $Process.Refresh()
    $path = [IO.Path]::GetFullPath($Process.MainModule.FileName)
    $parentId = 0
    try {
        $record = Get-CimInstance Win32_Process -Filter "ProcessId = $($Process.Id)" `
            -ErrorAction Stop
        if ($null -ne $record) { $parentId = [int]$record.ParentProcessId }
    }
    catch { }
    $parentCreation = [Int64]0
    if ($parentId -gt 0) {
        try {
            $parent = Get-Process -Id $parentId -ErrorAction Stop
            $parentCreation = $parent.StartTime.ToUniversalTime().ToFileTimeUtc()
        }
        catch { }
    }
    return [pscustomobject]@{
        processId = [int]$Process.Id
        creationTimeUtc100ns = [Int64]$Process.StartTime.ToUniversalTime().ToFileTimeUtc()
        executablePath = $path
        executableSha256 = Get-Stage5PerformanceSha256 $path
        commandLine = Get-Stage5ProcessCommandLine $Process.Id
        parentProcessId = $parentId
        parentCreationTimeUtc100ns = $parentCreation
    }
}

function Stop-Stage5ProcessSafely {
    param([Diagnostics.Process]$Process, [object]$ExpectedIdentity,
        [int]$WaitMilliseconds = 30000)
    $Process.Refresh()
    if ($Process.HasExited) { return }
    $current = Get-Stage5ProcessIdentity $Process
    Assert-Stage5PerformanceCondition ($current.processId -eq $ExpectedIdentity.processId -and
        $current.creationTimeUtc100ns -eq $ExpectedIdentity.creationTimeUtc100ns -and
        $current.executableSha256 -ceq $ExpectedIdentity.executableSha256 -and
        [String]::Equals($current.executablePath, $ExpectedIdentity.executablePath,
            [StringComparison]::OrdinalIgnoreCase) -and
        $current.commandLine -ceq $ExpectedIdentity.commandLine -and
        $current.parentProcessId -eq $ExpectedIdentity.parentProcessId -and
        $current.parentCreationTimeUtc100ns -eq $ExpectedIdentity.parentCreationTimeUtc100ns) `
        'Refusing to terminate a process whose identity or parent changed.'
    $Process.Kill()
    Assert-Stage5PerformanceCondition ($Process.WaitForExit($WaitMilliseconds)) `
        'Owned Stage 5 title process did not exit within the bounded stop wait.'
    $Process.Refresh()
    Assert-Stage5PerformanceCondition $Process.HasExited `
        'Owned Stage 5 title process has no exit proof after the bounded stop wait.'
}

function Invoke-Stage5OwnedProcessCleanup {
    param([object]$Process, [bool]$ProcessStarted,
        [object]$ProcessIdentity, [int]$WaitMilliseconds = 30000)
    $errors = New-Object 'Collections.Generic.List[string]'
    $exitProof = -not $ProcessStarted
    $processId = 0
    if ($null -ne $Process) {
        try { $processId = [int]$Process.Id }
        catch { }
    }
    if (-not $ProcessStarted) {
        return [pscustomobject]@{
            processId = $processId; exitProof = $true; blocked = $false
            errors = @()
        }
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            $exitProof = $true
        }
        else {
            if ($null -ne $ProcessIdentity) {
                try {
                    Stop-Stage5ProcessSafely $Process $ProcessIdentity `
                        $WaitMilliseconds
                }
                catch {
                    $errors.Add("identity-safe stop: $($_.Exception.Message)") | Out-Null
                }
            }
            # The Process object owns the handle returned by Start().  After an
            # identity/timeout re-check failure, use only that handle and keep
            # it until the bounded stop attempt has completed.
            $Process.Refresh()
            if (-not $Process.HasExited) {
                $Process.Kill()
                Assert-Stage5PerformanceCondition (
                    $Process.WaitForExit($WaitMilliseconds)) `
                    'Started Stage 5 title process did not exit after the bounded handle-safe stop.'
                $Process.Refresh()
            }
            $exitProof = [bool]$Process.HasExited
        }
    }
    catch {
        $errors.Add("started-process cleanup: $($_.Exception.Message)") | Out-Null
        try {
            $Process.Refresh()
            $exitProof = [bool]$Process.HasExited
        }
        catch { $exitProof = $false }
    }
    return [pscustomobject]@{
        processId = $processId; exitProof = $exitProof
        blocked = -not $exitProof; errors = @($errors.ToArray())
    }
}

function Resolve-Stage5RunEvidenceFile {
    param([string]$TaskRootPath, [string]$Path, [string]$Context)
    Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is empty."
    $root = [IO.Path]::GetFullPath($TaskRootPath).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5PerformanceCondition ($full.StartsWith(
        $root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context is outside the fresh task root."
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $full -PathType Leaf) `
        "$Context was not found: $full"
    return $full
}

function Assert-Stage5PerformanceFixtureHash {
    param([object]$Fixture, [string]$Context = 'Stage 5 performance fixture')
    Assert-Stage5PerformanceCondition ($null -ne $Fixture -and
        -not [string]::IsNullOrWhiteSpace([string]$Fixture.path) -and
        [string]$Fixture.sha256 -cmatch '^[0-9A-F]{64}$') `
        "$Context binding is incomplete."
    Assert-Stage5PerformanceFileHash ([IO.Path]::GetFullPath([string]$Fixture.path)) `
        ([string]$Fixture.sha256) "$Context SHA-256" | Out-Null
}

function Open-Stage5PerformanceReadOnlyLocks {
    param([object]$ArtifactBinding, [object[]]$Fixtures,
        [string]$FixtureManifestPath = '')
    Assert-Stage5PerformanceCondition ($null -ne $ArtifactBinding -and
        $null -ne $ArtifactBinding.runtimeClosure) `
        'Stage 5 read-only runtime lock binding is incomplete.'
    $artifactManifestPath = [IO.Path]::GetFullPath([string]$ArtifactBinding.path)
    $artifactDirectory = Split-Path -Parent $artifactManifestPath
    $dependencyManifestPath = [IO.Path]::GetFullPath((Join-Path $artifactDirectory `
        ([string]$ArtifactBinding.runtimeClosure.dependencyManifestPath)))
    $candidatePaths = New-Object 'Collections.Generic.List[string]'
    $candidatePaths.Add($artifactManifestPath) | Out-Null
    $candidatePaths.Add($dependencyManifestPath) | Out-Null
    foreach ($path in @($ArtifactBinding.runtimeClosure.filePaths)) {
        $candidatePaths.Add([string]$path) | Out-Null
    }
    foreach ($artifact in @($ArtifactBinding.artifacts.Values)) {
        $candidatePaths.Add([string]$artifact.path) | Out-Null
    }
    foreach ($fixture in @($Fixtures)) {
        $candidatePaths.Add([string]$fixture.path) | Out-Null
    }
    if (-not [string]::IsNullOrWhiteSpace($FixtureManifestPath)) {
        $candidatePaths.Add([string]$FixtureManifestPath) | Out-Null
    }
    $paths = New-Object 'Collections.Generic.List[string]'
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $candidatePaths) {
        Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace([string]$path)) `
            'Stage 5 read-only lock set contains an empty path.'
        $full = [IO.Path]::GetFullPath([string]$path)
        Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $full -PathType Leaf) `
            "Stage 5 read-only lock target was not found: $full"
        if ($seen.Add($full)) { $paths.Add($full) | Out-Null }
    }
    $locks = New-Object 'Collections.Generic.List[IO.FileStream]'
    try {
        foreach ($path in $paths) {
            $locks.Add([IO.File]::Open($path, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)) | Out-Null
        }
        return $locks.ToArray()
    }
    catch {
        foreach ($stream in $locks) {
            try { $stream.Dispose() }
            catch { }
        }
        throw "Stage 5 read-only runtime/fixture lock setup failed: $($_.Exception.Message)"
    }
}

function Dispose-Stage5PerformanceReadOnlyLocks {
    param([object[]]$Locks)
    $errors = New-Object 'Collections.Generic.List[string]'
    foreach ($stream in @($Locks)) {
        if ($null -eq $stream) { continue }
        try { $stream.Dispose() }
        catch { $errors.Add($_.Exception.Message) | Out-Null }
    }
    if ($errors.Count -gt 0) {
        throw "Stage 5 read-only runtime/fixture lock cleanup failed: $($errors.ToArray() -join ' | ')"
    }
}

function Test-Stage5SafeTitleSessionPath {
    param([string]$Path, [string]$Boundary = '', [switch]$AllowWhitespace)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $isHPath = $full.Length -ge 3 -and $full.Substring(0, 1) -match '^[Hh]$' -and
        $full[1] -eq ':' -and ($full[2] -eq [char]92 -or $full[2] -eq [char]47)
    if (-not $isHPath -or $full.Length -lt 4 -or $full.Length -ge 248 -or
        $full.IndexOf('..', [StringComparison]::Ordinal) -ge 0 -or
        $full.IndexOf(';', [StringComparison]::Ordinal) -ge 0 -or
        $full.IndexOf('"', [StringComparison]::Ordinal) -ge 0 -or
        (-not $AllowWhitespace -and $full -match '\s')) { return $false }
    if (-not [string]::IsNullOrWhiteSpace($Boundary)) {
        $boundaryFull = [IO.Path]::GetFullPath($Boundary).TrimEnd('\', '/')
        if (-not ($full -ceq $boundaryFull -or $full.StartsWith(
                $boundaryFull + '\', [StringComparison]::OrdinalIgnoreCase))) {
            return $false
        }
    }
    $cursor = $full
    while ($true) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $false
        }
        $root = [IO.Path]::GetPathRoot($cursor).TrimEnd('\', '/')
        if ($cursor -ceq $root) { break }
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -ceq $cursor) { break }
        $cursor = [IO.Path]::GetFullPath($parent).TrimEnd('\', '/')
    }
    return $true
}

function New-Stage5TitleSessionContract {
    param([string]$Title, [string]$SessionRoot, [string]$RuntimeDirectory,
        [string]$TaskRootPath)
    Assert-Stage5PerformanceCondition ($Title -ceq 'Generals' -or
        $Title -ceq 'ZeroHour') `
        "Unsupported installed title for Stage 5 profile setup: $Title"
    $sessionFull = [IO.Path]::GetFullPath($SessionRoot)
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    Assert-Stage5PerformanceCondition (
        (Test-Stage5SafeTitleSessionPath $sessionFull $TaskRootPath) -and
        (Test-Stage5SafeTitleSessionPath $runtimeFull)) `
        'Stage 5 title/session paths must remain on task-owned H: and installed H: runtime roots.'
    $documentsRoot = Join-Path $sessionFull 'Documents'
    $profileLeaf = if ($Title -ceq 'Generals') {
        'Command and Conquer Generals Data'
    } else { 'GGC-LockstepV2-ZeroHour' }
    $profileRoot = Join-Path $documentsRoot $profileLeaf
    $peerRoot = Join-Path $sessionFull 'Peers'
    $tempRoot = Join-Path $sessionFull 'Temp'
    $tmpRoot = Join-Path $sessionFull 'Tmp'
    $cacheRoot = Join-Path $sessionFull 'Cache'
    $logRoot = Join-Path $sessionFull 'Logs'
    $dumpRoot = Join-Path $sessionFull 'Dumps'
    $localAppDataRoot = Join-Path $sessionFull 'LocalAppData'
    $appDataRoot = Join-Path $sessionFull 'AppData'
    $homeDrive = [IO.Path]::GetPathRoot($sessionFull).TrimEnd('\')
    $homePath = $sessionFull.Substring($homeDrive.Length)
    $environmentValues = [ordered]@{
        TEMP = $tempRoot
        TMP = $tmpRoot
        LOCALAPPDATA = $localAppDataRoot
        APPDATA = $appDataRoot
        USERPROFILE = $sessionFull
        HOMEDRIVE = $homeDrive
        HOMEPATH = $homePath
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = $cacheRoot
        RTS_STAGE5_VALIDATION_LOG_ROOT = $logRoot
        RTS_STAGE5_VALIDATION_DUMP_ROOT = $dumpRoot
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionFull
    }
    $registryValues = New-Object 'Collections.Generic.List[object]'
    $registryValues.Add([pscustomobject]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
    $registryValues.Add([pscustomobject]@{
        subKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders'
        name = 'Personal'; value = $documentsRoot; purpose = 'known-folder-documents'
    }) | Out-Null
    if ($Title -ceq 'Generals') {
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
    }
    else {
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        }) | Out-Null
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'UserDataLeafName'; value = $profileLeaf; purpose = 'title-profile-leaf'
        }) | Out-Null
    }
    return [pscustomobject]@{
        schemaVersion = 1; title = $Title; sessionRoot = $sessionFull
        runtimeDirectory = $runtimeFull; documentsRoot = $documentsRoot
        profileLeaf = $profileLeaf; profileRoot = $profileRoot; peerRoot = $peerRoot
        profileConcurrency = 'shared-title-profile-read-only'
        environmentValues = $environmentValues
        environmentVariableNames = @($environmentValues.Keys)
        registryViews = @('Registry32', 'Registry64')
        registryValues = $registryValues.ToArray()
    }
}

function Initialize-Stage5TitleSessionDirectories {
    param([object]$Contract)
    foreach ($directory in @(
        $Contract.sessionRoot, $Contract.documentsRoot, $Contract.profileRoot,
        $Contract.peerRoot, $Contract.environmentValues['TEMP'],
        $Contract.environmentValues['TMP'],
        $Contract.environmentValues['LOCALAPPDATA'],
        $Contract.environmentValues['APPDATA'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_CACHE_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_LOG_ROOT'],
        $Contract.environmentValues['RTS_STAGE5_VALIDATION_DUMP_ROOT'])) {
        Assert-Stage5PerformanceCondition (
            Test-Stage5SafeTitleSessionPath ([string]$directory) `
                $Contract.sessionRoot -AllowWhitespace) `
            "Stage 5 title-session directory is not a safe bounded H: path: $directory"
        [IO.Directory]::CreateDirectory([string]$directory) | Out-Null
    }
}

function Get-Stage5ProfileTreeHash {
    param([string]$ProfileRoot)
    Assert-Stage5PerformanceCondition (
        (Test-Stage5SafeTitleSessionPath $ProfileRoot -AllowWhitespace) -and
        (Test-Path -LiteralPath $ProfileRoot -PathType Container)) `
        "Stage 5 title profile root disappeared: $ProfileRoot"
    $root = [IO.Path]::GetFullPath($ProfileRoot).TrimEnd('\', '/')
    $lines = @((Get-ChildItem -LiteralPath $root -File -Force -Recurse) |
        ForEach-Object {
            $relative = $_.FullName.Substring($root.Length).TrimStart('\', '/')
            '{0}|{1}' -f $relative.Replace('\', '/'),
                (Get-Stage5PerformanceSha256 $_.FullName)
        })
    [Array]::Sort($lines, [StringComparer]::Ordinal)
    $text = if ($lines.Count -gt 0) { ($lines -join "`n") + "`n" } else { '' }
    return [pscustomobject]@{
        sha256 = if ($text.Length -eq 0) {
            $algorithm = [Security.Cryptography.SHA256]::Create()
            try { ([BitConverter]::ToString($algorithm.ComputeHash(
                [Text.Encoding]::UTF8.GetBytes(''))) -replace '-', '').ToUpperInvariant() }
            finally { $algorithm.Dispose() }
        } else {
            $bytes = [Text.Encoding]::UTF8.GetBytes($text)
            $algorithm = [Security.Cryptography.SHA256]::Create()
            try { ([BitConverter]::ToString($algorithm.ComputeHash($bytes)) -replace '-', '').ToUpperInvariant() }
            finally { $algorithm.Dispose() }
        }
        fileCount = $lines.Count
        files = @($lines)
    }
}

function Assert-Stage5ProfileReadOnly {
    param([string]$ProfileRoot)
    $tree = Get-Stage5ProfileTreeHash $ProfileRoot
    Assert-Stage5PerformanceCondition ($tree.fileCount -eq 0) `
        "Stage 5 shared title profile was written during qualification: $ProfileRoot"
    return $tree
}

function Invoke-Stage5RegistryTargetSetup {
    param([string]$SubKey, [Collections.Generic.List[string]]$CreatedSubKeys,
        [scriptblock]$OpenExisting, [scriptblock]$CreateSubKey,
        [scriptblock]$ReopenTarget, [scriptblock]$Rollback)
    $current = ''
    try {
        foreach ($segment in $SubKey.Split('\')) {
            $current = if ([string]::IsNullOrEmpty($current)) {
                $segment
            } else { $current + '\' + $segment }
            $existing = & $OpenExisting $current
            if ($null -ne $existing) { $existing.Dispose(); continue }
            $created = & $CreateSubKey $current
            if ($null -eq $created) {
                throw "Could not create registry key '$current'."
            }
            # Journal each newly-created segment before disposing the handle so
            # any later setup failure can roll it back.
            $CreatedSubKeys.Add($current) | Out-Null
            $created.Dispose()
        }
        $target = & $ReopenTarget $SubKey
        if ($null -eq $target) {
            throw "Could not reopen registry key '$SubKey'."
        }
        return $target
    }
    catch {
        $setupError = $_
        try { & $Rollback $CreatedSubKeys.ToArray() }
        catch {
            throw "Registry key setup failed for '$SubKey': $($setupError.Exception.Message); partial-key rollback also failed: $($_.Exception.Message)"
        }
        throw $setupError
    }
}

function Set-Stage5RegistryValue {
    param([Microsoft.Win32.RegistryView]$View, [string]$SubKey,
        [string]$Name, [string]$Value,
        [Collections.Generic.List[object]]$Snapshots,
        [Collections.IDictionary]$SnapshotKeys,
        [scriptblock]$SnapshotObserver = $null)
    $snapshotKey = "$View|$SubKey|$Name"
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    try {
        $target = $base.OpenSubKey($SubKey, $true)
        $createdSubKeys = New-Object 'Collections.Generic.List[string]'
        if ($null -eq $target) {
            $readOnlyTarget = $base.OpenSubKey($SubKey, $false)
            if ($null -ne $readOnlyTarget) {
                $readOnlyTarget.Dispose()
                throw "Registry key '$SubKey' is not writable for $View."
            }
            $target = Invoke-Stage5RegistryTargetSetup $SubKey $createdSubKeys `
                { param($path) $base.OpenSubKey($path, $false) } `
                { param($path) $base.CreateSubKey($path) } `
                { param($path) $base.OpenSubKey($path, $true) } `
                { param($paths) Remove-Stage5EmptyRegistryKeys $base $paths }
        }
        try {
            if (-not $SnapshotKeys.Contains($snapshotKey)) {
                $hadValue = @($target.GetValueNames()) -contains $Name
                $oldValue = $null; $oldKind = $null
                if ($hadValue) {
                    $oldValue = $target.GetValue($Name, $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $oldKind = $target.GetValueKind($Name)
                }
                $Snapshots.Add([pscustomobject]@{
                    view = $View; subKey = $SubKey; name = $Name
                    hadKey = $createdSubKeys.Count -eq 0; hadValue = $hadValue
                    oldValue = $oldValue; oldKind = $oldKind
                    createdSubKeys = $createdSubKeys.ToArray()
                }) | Out-Null
                $SnapshotKeys[$snapshotKey] = $true
                if ($null -ne $SnapshotObserver) {
                    & $SnapshotObserver $Snapshots.ToArray()
                }
            }
            $target.SetValue($Name, $Value,
                [Microsoft.Win32.RegistryValueKind]::String)
        }
        finally { $target.Dispose() }
    }
    finally { $base.Dispose() }
}

function Remove-Stage5EmptyRegistryKeys {
    param([Microsoft.Win32.RegistryKey]$Base, [object[]]$CreatedSubKeys)
    foreach ($path in @($CreatedSubKeys | Sort-Object Length -Descending -Unique)) {
        $key = $Base.OpenSubKey([string]$path, $false)
        if ($null -eq $key) { continue }
        try {
            if (@($key.GetValueNames()).Count -ne 0 -or
                @($key.GetSubKeyNames()).Count -ne 0) { continue }
        }
        finally { $key.Dispose() }
        $separator = ([string]$path).LastIndexOf('\')
        if ($separator -lt 0) {
            $Base.DeleteSubKey([string]$path, $false)
        }
        else {
            $parent = ([string]$path).Substring(0, $separator)
            $leaf = ([string]$path).Substring($separator + 1)
            $parentKey = $Base.OpenSubKey($parent, $true)
            if ($null -ne $parentKey) {
                try { $parentKey.DeleteSubKey($leaf, $false) }
                finally { $parentKey.Dispose() }
            }
        }
    }
}

function Restore-Stage5RegistrySnapshots {
    param([object[]]$Snapshots)
    $errors = New-Object 'Collections.Generic.List[string]'
    for ($index = $Snapshots.Count - 1; $index -ge 0; --$index) {
        $snapshot = $Snapshots[$index]
        $base = $null
        try {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser, $snapshot.view)
            $target = $base.OpenSubKey($snapshot.subKey, $true)
            if ($snapshot.hadKey -and $null -eq $target) {
                throw "Pre-existing registry key '$($snapshot.subKey)' disappeared before restoration."
            }
            if ($null -ne $target) {
                try {
                    if ($snapshot.hadValue) {
                        $target.SetValue($snapshot.name, $snapshot.oldValue,
                            $snapshot.oldKind)
                    }
                    elseif (@($target.GetValueNames()) -contains $snapshot.name) {
                        $target.DeleteValue($snapshot.name, $false)
                    }
                }
                finally { $target.Dispose() }
            }
            if (-not $snapshot.hadKey) {
                Remove-Stage5EmptyRegistryKeys $base $snapshot.createdSubKeys
            }
        }
        catch {
            $errors.Add("$($snapshot.view)/$($snapshot.subKey)/$($snapshot.name): $($_.Exception.Message)") | Out-Null
        }
        finally { if ($null -ne $base) { $base.Dispose() } }
    }
    if ($errors.Count -gt 0) {
        throw "Stage 5 registry restoration failed after every snapshot: $($errors.ToArray() -join ' | ')"
    }
}

function Remove-Stage5TitleSessionDirectories {
    param([object]$Contract, [string]$TaskRootPath)
    Assert-Stage5PerformanceCondition ($null -ne $Contract -and
        [IO.Path]::GetFileName([string]$Contract.sessionRoot) -ceq 'TitleSession' -and
        (Test-Stage5SafeTitleSessionPath ([string]$Contract.sessionRoot) $TaskRootPath)) `
        'Stage 5 title-session cleanup path is not bounded.'
    $root = Get-Item -LiteralPath ([string]$Contract.sessionRoot) -Force `
        -ErrorAction SilentlyContinue
    if ($null -eq $root) { return }
    if (($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Stage 5 title-session root is a reparse point: $($Contract.sessionRoot)"
    }
    Remove-Item -LiteralPath ([string]$Contract.sessionRoot) -Recurse -Force
    Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $Contract.sessionRoot)) `
        "Stage 5 title-session cleanup did not remove: $($Contract.sessionRoot)"
}

function Assert-Stage5RawDiagnostic {
    param([string]$Path, [object]$Receipt, [string]$Context)
    $fields = @{}
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ($line -match '^(?<name>[a-z0-9_]+)=(?<value>.*)$') {
            Assert-Stage5PerformanceCondition (-not $fields.ContainsKey($Matches.name)) `
                "$Context repeats raw field '$($Matches.name)'."
            $fields[$Matches.name] = $Matches.value
        }
    }
    foreach ($name in @('producer', 'game_owned', 'run_id', 'process_id',
            'process_creation_time_utc_100ns', 'executable_sha256',
            'command_line', 'fixture_id', 'fixture_sha256', 'frame',
            'final_crc', 'close_boundary')) {
        Assert-Stage5PerformanceCondition ($fields.ContainsKey($name)) `
                "$Context raw diagnostic is missing '$name'."
    }
    [UInt32]$rawFinalCrc = 0
    $rawFinalCrcValid = [string]$fields.final_crc -cmatch '^[0-9A-F]{8}$' -and
        [UInt32]::TryParse([string]$fields.final_crc,
            [Globalization.NumberStyles]::AllowHexSpecifier,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$rawFinalCrc)
    Assert-Stage5PerformanceCondition ($fields.producer -ceq
        'game-executable-performance-receipt-v5' -and
        $fields.game_owned -ceq '1' -and
        $fields.run_id -ceq [string]$Receipt.runId -and
        [int]$fields.process_id -eq [int]$Receipt.process.id -and
        [Int64]$fields.process_creation_time_utc_100ns -eq
            [Int64]$Receipt.process.creationTimeUtc100ns -and
        $fields.executable_sha256 -ceq [string]$Receipt.executableSha256 -and
        $fields.command_line -ceq [string]$Receipt.commandLine -and
        $fields.fixture_id -ceq [string]$Receipt.fixture.id -and
        $fields.fixture_sha256 -ceq [string]$Receipt.fixture.contentSha256 -and
        [UInt32]$fields.frame -eq [UInt32]$Receipt.frames.final -and
        $rawFinalCrcValid -and
        $rawFinalCrc -eq [UInt32]$Receipt.frames.finalCrc -and
        $fields.close_boundary -ceq 'game-owned-raw-diagnostic-closed-v1') `
        "$Context raw diagnostic identity does not match its executable receipt."
}

function Assert-Stage5ReceiptMetricContract {
    param([object]$Receipt, [object]$Context, [string]$Label,
        [string]$ExpectedMeasurementRole = 'throughput')
    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial',
        'path')
    $phases = @($Receipt.phases)
    $kernels = @($Receipt.kernels)
    Assert-Stage5PerformanceCondition ($phases.Count -eq $phaseNames.Count) `
        "$Label must contain exactly five phase metrics."
    Assert-Stage5PerformanceCondition ($kernels.Count -eq $kernelNames.Count) `
        "$Label must contain exactly six kernel metrics."
    Assert-Stage5PerformanceCondition ([Int64]$Receipt.frameSimulation.totalNanoseconds -gt 0 -and
        [Int64]$Receipt.frameSimulation.maximumNanoseconds -gt 0 -and
        [Int64]$Receipt.frameSimulation.maximumNanoseconds -le
            [Int64]$Receipt.frameSimulation.totalNanoseconds -and
        [Int64]$Receipt.frameSimulation.sampleCount -gt 0) `
        "$Label frame simulation timing is incomplete or inconsistent."
    [Int64]$phaseTotal = 0
    for ($index = 0; $index -lt $phaseNames.Count; ++$index) {
        $phase = $phases[$index]
        Assert-Stage5PerformanceProperties $phase @('name', 'available',
            'totalNanoseconds', 'maximumNanoseconds', 'sampleCount',
            'serialNanoseconds', 'serialNanosecondsKnown') `
            "$Label phase metric"
        Assert-Stage5PerformanceCondition ([string]$phase.name -ceq
            $phaseNames[$index]) "$Label phase order/name is not canonical."
        Assert-Stage5PerformanceCondition (
            $phase.available -or
            ([Int64]$phase.totalNanoseconds -eq 0 -and
                [Int64]$phase.maximumNanoseconds -eq 0 -and
                [Int64]$phase.sampleCount -eq 0)) `
            "$Label phase '$($phaseNames[$index])' is malformed."
        Assert-Stage5PerformanceCondition ([Int64]$phase.maximumNanoseconds -le
            [Int64]$phase.totalNanoseconds -and
            [Int64]$phase.sampleCount -le
                [Int64]$Receipt.frameSimulation.sampleCount -and
            [Int64]$phase.totalNanoseconds -le
                ([Int64]$Receipt.frameSimulation.totalNanoseconds - $phaseTotal)) `
            "$Label phase '$($phaseNames[$index])' exceeds frame timing coverage."
        if ([bool]$phase.available) {
            Assert-Stage5PerformanceCondition ([Int64]$phase.sampleCount -gt 0 -and
                [Int64]$phase.totalNanoseconds -gt 0 -and
                [Int64]$phase.maximumNanoseconds -gt 0) `
                "$Label available phase '$($phaseNames[$index])' lacks positive timing."
        }
        else {
            Assert-Stage5PerformanceCondition ([Int64]$phase.serialNanoseconds -eq 0 -and
                -not [bool]$phase.serialNanosecondsKnown) `
                "$Label unavailable phase '$($phaseNames[$index])' has serial evidence."
        }
        if ($Context.qualificationMode -ceq 'External16Core' -and
            -not [bool]$phase.serialNanosecondsKnown) {
            throw "$Label external qualification cannot use unknown serial timing for phase '$($phaseNames[$index])'."
        }
        if ([bool]$phase.serialNanosecondsKnown) {
            Assert-Stage5PerformanceCondition ([bool]$phase.available -and
                [Int64]$phase.serialNanoseconds -ge 0 -and
                [Int64]$phase.serialNanoseconds -le [Int64]$phase.totalNanoseconds) `
                "$Label phase '$($phaseNames[$index])' serial timing is inconsistent."
        }
        elseif ([Int64]$phase.serialNanoseconds -ne 0) {
            throw "$Label phase '$($phaseNames[$index])' has serial timing without a known flag."
        }
        $phaseTotal += [Int64]$phase.totalNanoseconds
        if (-not [bool]$phase.available -and
            $Context.qualificationMode -ceq 'External16Core') {
            throw "$Label external qualification cannot use unavailable phase '$($phaseNames[$index])'."
        }
    }
    $kernelTiming = $Receipt.kernelTiming
    Assert-Stage5PerformanceProperties $kernelTiming @('schemaVersion', 'mode',
        'attribution', 'enabled', 'frozen', 'complete', 'errors', 'generation',
        'serialReferenceKnown', 'streams') "$Label kernel timing"
    Assert-Stage5PerformanceCondition ($kernelTiming.schemaVersion -eq 1 -and
        $kernelTiming.mode -ceq 'owner-pipeline-observation' -and
        $kernelTiming.attribution -ceq 'owner-stack-exclusive-v1' -and
        [bool]$kernelTiming.enabled -and [bool]$kernelTiming.frozen -and
        [int]$kernelTiming.errors -eq 0 -and
        [Int64]$kernelTiming.generation -gt 0 -and
        -not [bool]$kernelTiming.serialReferenceKnown -and
        $kernelTiming.streams -is [Array]) `
        "$Label kernel timing identity or lifecycle is invalid."
    $kernelStreams = @($kernelTiming.streams)
    Assert-Stage5PerformanceCondition ($kernelStreams.Count -le 16 -and
        [bool]$kernelTiming.complete -eq
        ($kernelStreams.Count -ne 0)) `
        "$Label kernel timing stream capacity or completion is invalid."
    if ($Context.qualificationMode -ceq 'External16Core' -and
        $kernelStreams.Count -eq 0) {
        throw "$Label external qualification cannot use an incomplete kernel timing stream set."
    }
    $kernelTimingNames = @('physics', 'status', 'collision', 'ai-planning',
        'spatial', 'path')
    $kernelTimingStageNames = @('capture', 'schedule', 'wait', 'validate',
        'commit')
    $seenKernelStreams = @{}
    foreach ($stream in $kernelStreams) {
        Assert-Stage5PerformanceProperties $stream @('name', 'subtype',
            'attemptedBatches', 'admittedBatches', 'committedBatches',
            'abortedBatches', 'firstFrame', 'lastFrame',
            'activePipelineNanoseconds', 'inclusiveBatchNanoseconds',
            'maximumBatchNanoseconds', 'stages') "$Label kernel timing stream"
        $streamName = [string]$stream.name
        Assert-Stage5PerformanceCondition ($kernelTimingNames -ccontains $streamName) `
            "$Label kernel timing stream name is unsupported."
        $maximumSubtype = if ($streamName -ceq 'ai-planning' -or
            $streamName -ceq 'path') { 1 } else { 0 }
        [UInt64]$subtype = $stream.subtype
        Assert-Stage5PerformanceCondition ($subtype -le $maximumSubtype) `
            "$Label kernel timing stream subtype is unsupported."
        $streamKey = "$streamName`:$subtype"
        Assert-Stage5PerformanceCondition (-not $seenKernelStreams.ContainsKey($streamKey)) `
            "$Label kernel timing stream is duplicated."
        $seenKernelStreams[$streamKey] = $true
        [UInt64]$attempted = $stream.attemptedBatches
        [UInt64]$admitted = $stream.admittedBatches
        [UInt64]$committed = $stream.committedBatches
        [UInt64]$aborted = $stream.abortedBatches
        [UInt64]$firstFrame = $stream.firstFrame
        [UInt64]$lastFrame = $stream.lastFrame
        [UInt64]$active = $stream.activePipelineNanoseconds
        [UInt64]$inclusive = $stream.inclusiveBatchNanoseconds
        [UInt64]$maximum = $stream.maximumBatchNanoseconds
        Assert-Stage5PerformanceCondition ($attempted -gt 0 -and
            $admitted -le $attempted -and $committed -le $admitted -and
            $aborted -eq ($admitted - $committed) -and
            $firstFrame -le $lastFrame -and
            $firstFrame -ge [UInt64]$Receipt.frames.start -and
            $lastFrame -le [UInt64]$Receipt.frames.end -and
            $active -le $inclusive -and $maximum -le $inclusive) `
            "$Label kernel timing stream arithmetic or frame range is invalid."
        Assert-Stage5PerformanceCondition ($stream.stages -is [Array] -and
            @($stream.stages).Count -eq $kernelTimingStageNames.Count) `
            "$Label kernel timing stream stages are incomplete."
        [UInt64]$stageTotal = 0
        for ($stageIndex = 0; $stageIndex -lt $kernelTimingStageNames.Count; ++$stageIndex) {
            $stage = @($stream.stages)[$stageIndex]
            Assert-Stage5PerformanceProperties $stage @('name',
                'totalNanoseconds', 'sampleCount') "$Label kernel timing stage"
            Assert-Stage5PerformanceCondition ([string]$stage.name -ceq
                $kernelTimingStageNames[$stageIndex]) `
                "$Label kernel timing stage order/name is not canonical."
            [UInt64]$stageNanoseconds = $stage.totalNanoseconds
            [UInt64]$stageSamples = $stage.sampleCount
            Assert-Stage5PerformanceCondition (($stageNanoseconds -eq 0 -or
                $stageSamples -gt 0) -and $stageSamples -ge $committed) `
                "$Label kernel timing stage coverage is invalid."
            Assert-Stage5PerformanceCondition ($stageNanoseconds -le
                ([UInt64]::MaxValue - $stageTotal)) `
                "$Label kernel timing stage total overflows its bounded sum."
            $stageTotal += $stageNanoseconds
        }
        Assert-Stage5PerformanceCondition ($stageTotal -eq $active) `
            "$Label kernel timing stage totals do not equal active pipeline timing."
    }
    for ($index = 0; $index -lt $kernelNames.Count; ++$index) {
        Assert-Stage5PerformanceCondition ([string]$kernels[$index].name -ceq
            $kernelNames[$index]) "$Label kernel order/name is not canonical."
        if (-not [bool]$kernels[$index].available) {
            Assert-Stage5PerformanceCondition (
                [Int64]$kernels[$index].submittedJobs -eq 0 -and
                [Int64]$kernels[$index].completedJobs -eq 0 -and
                [Int64]$kernels[$index].physicalWorkerJobs -eq 0 -and
                [Int64]$kernels[$index].ownerHelpedJobs -eq 0 -and
                [UInt64]$kernels[$index].physicalWorkerMask -eq 0 -and
                [int]$kernels[$index].distinctPhysicalWorkers -eq 0 -and
                -not [bool]$kernels[$index].physicalWorkerMaskComplete -and
                [Int64]$kernels[$index].elapsedNanoseconds -eq 0 -and
                -not [bool]$kernels[$index].elapsedNanosecondsKnown) `
                "$Label unavailable kernel '$($kernelNames[$index])' contains evidence."
            if ($Context.qualificationMode -ceq 'External16Core') {
                throw "$Label external qualification cannot use unavailable kernel '$($kernelNames[$index])'."
            }
            continue
        }
        if ([bool]$kernels[$index].elapsedNanosecondsKnown) {
            Assert-Stage5PerformanceCondition ([Int64]$kernels[$index].elapsedNanoseconds -gt 0) `
                "$Label known kernel '$($kernelNames[$index])' lacks positive timing."
        }
        elseif ($Context.qualificationMode -ceq 'External16Core') {
            throw "$Label external qualification cannot use unknown kernel '$($kernelNames[$index])' timing."
        }
    }
    Assert-Stage5KernelReferenceContract $Receipt $Context $Label `
        $ExpectedMeasurementRole
}

function Get-Stage5UnsignedCounter {
    param([object]$Value, [string]$Context, [bool]$AllowZero = $true)
    $isNumeric = $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [uint16] -or $Value -is [int16] -or
        $Value -is [uint32] -or $Value -is [int32] -or
        $Value -is [uint64] -or $Value -is [int64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal]
    Assert-Stage5PerformanceCondition $isNumeric `
        "$Context must be a JSON number, not a string or object."
    $text = [Convert]::ToString($Value,
        [Globalization.CultureInfo]::InvariantCulture)
    Assert-Stage5PerformanceCondition ($text -cmatch '^(0|[1-9][0-9]*)$') `
        "$Context must be an unsigned integer."
    [UInt64]$parsed = 0
    Assert-Stage5PerformanceCondition ([UInt64]::TryParse($text,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) `
        "$Context is outside the unsigned 64-bit range."
    if (-not $AllowZero) {
        Assert-Stage5PerformanceCondition ($parsed -gt 0) `
            "$Context must be positive."
    }
    return $parsed
}

function Assert-Stage5KernelReferenceContract {
    param([object]$Receipt, [object]$Context, [string]$Label,
        [string]$ExpectedMeasurementRole = 'throughput')
    $reference = $Receipt.kernelReference
    Assert-Stage5PerformanceProperties $reference @('schemaVersion', 'mode',
        'frozen', 'complete', 'errors', 'generation', 'streams') `
        "$Label kernel reference"
    $schemaVersion = Get-Stage5UnsignedCounter $reference.schemaVersion `
        "$Label kernel reference schemaVersion" $false
    $errors = Get-Stage5UnsignedCounter $reference.errors `
        "$Label kernel reference errors"
    $generation = Get-Stage5UnsignedCounter $reference.generation `
        "$Label kernel reference generation" $false
    Assert-Stage5PerformanceCondition ($schemaVersion -eq 1 -and
        @('throughput-binding', 'serial-oracle') -ccontains [string]$reference.mode -and
        $reference.mode -is [string] -and
        $Receipt.measurementRole -is [string] -and
        $reference.frozen -is [bool] -and [bool]$reference.frozen -and
        $reference.complete -is [bool] -and $errors -eq 0 -and
        $generation -gt 0 -and $reference.streams -is [Array]) `
        "$Label kernel reference identity or lifecycle is invalid."
    $referenceStreams = @($reference.streams)
    Assert-Stage5PerformanceCondition ($referenceStreams.Count -le 16 -and
        [bool]$reference.complete -eq ($referenceStreams.Count -ne 0)) `
        "$Label kernel reference stream capacity or completion is invalid."
    if ($Context.qualificationMode -ceq 'External16Core' -and
        $referenceStreams.Count -eq 0) {
        throw "$Label external qualification cannot use an incomplete kernel reference stream set."
    }

    $timingByKey = @{}
    foreach ($timingStream in @($Receipt.kernelTiming.streams)) {
        $timingName = [string]$timingStream.name
        $timingSubtype = Get-Stage5UnsignedCounter $timingStream.subtype `
            "$Label kernel timing stream subtype" $true
        $timingKey = "$timingName`:$timingSubtype"
        Assert-Stage5PerformanceCondition (-not $timingByKey.ContainsKey($timingKey)) `
            "$Label kernel timing stream is duplicated."
        $timingByKey[$timingKey] = $timingStream
    }

    $referenceNames = @('physics', 'status', 'collision', 'ai-planning',
        'spatial', 'path')
    $seenReferenceStreams = @{}
    foreach ($stream in $referenceStreams) {
        Assert-Stage5PerformanceProperties $stream @('name', 'subtype',
            'fieldSchema', 'firstFrame', 'lastFrame', 'validatedBatchCount',
            'committedBatchCount', 'abortedBatchCount',
            'validatedOperationCount', 'committedOperationCount',
            'serialSampleCount', 'serialNanoseconds',
            'maximumSerialNanoseconds', 'inputSha256', 'outputSha256',
            'commitSha256') "$Label kernel reference stream"
        $name = [string]$stream.name
        Assert-Stage5PerformanceCondition ($referenceNames -ccontains $name) `
            "$Label kernel reference stream name is unsupported."
        $subtype = Get-Stage5UnsignedCounter $stream.subtype `
            "$Label kernel reference stream subtype" $true
        $maximumSubtype = if ($name -ceq 'ai-planning' -or
            $name -ceq 'path') { 1 } else { 0 }
        Assert-Stage5PerformanceCondition ($subtype -le $maximumSubtype) `
            "$Label kernel reference stream subtype is unsupported."
        $key = "$name`:$subtype"
        Assert-Stage5PerformanceCondition (-not $seenReferenceStreams.ContainsKey($key)) `
            "$Label kernel reference stream is duplicated."
        $seenReferenceStreams[$key] = $true
        $fieldSchema = Get-Stage5UnsignedCounter $stream.fieldSchema `
            "$Label kernel reference stream fieldSchema" $false
        $firstFrame = Get-Stage5UnsignedCounter $stream.firstFrame `
            "$Label kernel reference stream firstFrame" $true
        $lastFrame = Get-Stage5UnsignedCounter $stream.lastFrame `
            "$Label kernel reference stream lastFrame" $true
        $validatedBatches = Get-Stage5UnsignedCounter $stream.validatedBatchCount `
            "$Label kernel reference stream validatedBatchCount" $false
        $committedBatches = Get-Stage5UnsignedCounter $stream.committedBatchCount `
            "$Label kernel reference stream committedBatchCount" $true
        $abortedBatches = Get-Stage5UnsignedCounter $stream.abortedBatchCount `
            "$Label kernel reference stream abortedBatchCount" $true
        $validatedOperations = Get-Stage5UnsignedCounter $stream.validatedOperationCount `
            "$Label kernel reference stream validatedOperationCount" $false
        $committedOperations = Get-Stage5UnsignedCounter $stream.committedOperationCount `
            "$Label kernel reference stream committedOperationCount" $true
        $serialSamples = Get-Stage5UnsignedCounter $stream.serialSampleCount `
            "$Label kernel reference stream serialSampleCount" $true
        $serialNanoseconds = Get-Stage5UnsignedCounter $stream.serialNanoseconds `
            "$Label kernel reference stream serialNanoseconds" $true
        $maximumSerialNanoseconds = Get-Stage5UnsignedCounter `
            $stream.maximumSerialNanoseconds `
            "$Label kernel reference stream maximumSerialNanoseconds" $true
        Assert-Stage5PerformanceCondition ($firstFrame -le $lastFrame -and
            $firstFrame -ge [UInt64]$Receipt.frames.start -and
            $lastFrame -le [UInt64]$Receipt.frames.end -and
            $committedBatches -le $validatedBatches -and
            $abortedBatches -eq ($validatedBatches - $committedBatches) -and
            $validatedOperations -ge $validatedBatches -and
            $committedOperations -ge $committedBatches -and
            $committedOperations -le $validatedOperations -and
            ($validatedOperations - $committedOperations) -ge $abortedBatches -and
            ($committedBatches -ne 0 -or $committedOperations -eq 0) -and
            ($abortedBatches -ne 0 -or
                $validatedOperations -eq $committedOperations)) `
            "$Label kernel reference stream arithmetic or frame range is invalid."
        foreach ($hashName in @('inputSha256', 'outputSha256', 'commitSha256')) {
            Assert-Stage5PerformanceCondition ([string]$stream.$hashName -cmatch
                '^[0-9A-F]{64}$') `
                "$Label kernel reference stream $hashName is not a canonical uppercase SHA-256."
        }
        Assert-Stage5PerformanceCondition $timingByKey.ContainsKey($key) `
            "$Label kernel reference stream has no matching kernel timing stream."
        $timingStream = $timingByKey[$key]
        $timingAdmitted = Get-Stage5UnsignedCounter $timingStream.admittedBatches `
            "$Label matching kernel timing admittedBatches" $true
        $timingCommitted = Get-Stage5UnsignedCounter $timingStream.committedBatches `
            "$Label matching kernel timing committedBatches" $true
        Assert-Stage5PerformanceCondition ($committedBatches -eq $timingCommitted -and
            $validatedBatches -le $timingAdmitted) `
            "$Label kernel reference does not match the executable timing ledger."
        if ($reference.mode -ceq 'throughput-binding') {
            Assert-Stage5PerformanceCondition ($serialSamples -eq 0 -and
                $serialNanoseconds -eq 0 -and
                $maximumSerialNanoseconds -eq 0) `
                "$Label throughput kernel reference contains serial-oracle evidence."
        }
        else {
            Assert-Stage5PerformanceCondition ($serialSamples -eq $committedBatches -and
                $maximumSerialNanoseconds -le $serialNanoseconds -and
                (($serialNanoseconds -eq 0 -and $maximumSerialNanoseconds -eq 0) -or
                    $serialSamples -gt 0)) `
                "$Label serial-oracle kernel reference timing is inconsistent."
        }
    }
    Assert-Stage5PerformanceCondition (@('throughput', 'serial-oracle') -ccontains
        [string]$ExpectedMeasurementRole) `
        "$Label expected measurement role is unsupported."
    Assert-Stage5PerformanceCondition ($Receipt.measurementRole -is [string] -and
        $Receipt.measurementRole -ceq $ExpectedMeasurementRole -and
        (($ExpectedMeasurementRole -ceq 'throughput' -and
            $reference.mode -ceq 'throughput-binding') -or
         ($ExpectedMeasurementRole -ceq 'serial-oracle' -and
            $reference.mode -ceq 'serial-oracle'))) `
        "$Label measurement role and kernel reference mode do not match."
    Assert-Stage5PerformanceCondition ($Receipt.kernelTiming.mode -ceq
        'owner-pipeline-observation' -and
        -not [bool]$Receipt.kernelTiming.serialReferenceKnown) `
        "$Label kernel timing cannot claim an internal serial oracle."
}

function Assert-Stage5Receipt {
    param([object]$Run, [object]$Context, [Collections.IDictionary]$SeenRunIds,
        [Collections.IDictionary]$SeenRunNonces,
        [Collections.IDictionary]$SeenReceiptPaths,
        [Collections.IDictionary]$SeenReceiptHashes,
        [string]$ExpectedMeasurementRole = 'throughput')
    $label = "Run '$($Run.fixtureId)/$($Run.lane)/$($Run.ordinal)'"
    $receiptPath = Resolve-Stage5RunEvidenceFile $Context.taskRoot `
        ([string]$Run.receiptPath) "$label receipt"
    $receiptHash = Get-Stage5PerformanceSha256 $receiptPath
    Assert-Stage5PerformanceCondition ($receiptHash -ceq [string]$Run.receiptSha256) `
        "$label receipt SHA-256 mismatch."
    Assert-Stage5PerformanceCondition (-not $SeenReceiptPaths.ContainsKey(
        $receiptPath.ToLowerInvariant())) "$label reuses a receipt path."
    Assert-Stage5PerformanceCondition (-not $SeenReceiptHashes.ContainsKey($receiptHash)) `
        "$label reuses an executable receipt."
    $receipt = Read-Stage5PerformanceJson $receiptPath "$label receipt"
    Assert-Stage5PerformanceProperties $receipt @('schemaVersion', 'producer',
        'evidenceKind', 'status', 'role', 'producerVersion', 'title', 'runId',
        'runNonce', 'cohortNonce', 'cohortCreatedUtc', 'recordedUtc',
        'architecture', 'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'executablePath', 'executableSha256', 'commandLine', 'process',
        'fixture', 'workload', 'frameSimulation', 'frames', 'worker', 'topology',
        'rawEvidence', 'rawLogs',
        'provenance', 'schedulerMetrics', 'phases', 'kernels', 'kernelTiming',
        'measurementRole', 'kernelReference', 'simulationMode',
        'schedulerStarted') "$label receipt"
    Assert-Stage5ReceiptMetricContract $receipt $Context "$label receipt" `
        $ExpectedMeasurementRole
    [DateTimeOffset]$cohortCreated = [DateTimeOffset]::MinValue
    [DateTimeOffset]$recorded = [DateTimeOffset]::MinValue
    $isoUtcPattern = '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$'
    $uuidPattern = '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$'
    $cohortCreatedValid = [DateTimeOffset]::TryParse(
        [string]$receipt.cohortCreatedUtc, [ref]$cohortCreated)
    $recordedValid = [DateTimeOffset]::TryParse(
        [string]$receipt.recordedUtc, [ref]$recorded)
    Assert-Stage5PerformanceCondition ([string]$receipt.cohortCreatedUtc -cmatch
        $isoUtcPattern -and [string]$receipt.recordedUtc -cmatch $isoUtcPattern) `
        "$label receipt timestamps must be ISO-8601 UTC values ending in Z."
    Assert-Stage5PerformanceCondition ($receipt.schemaVersion -eq 5 -and
        $receipt.producer -ceq 'game-executable-stage5-performance-report-v5' -and
        $receipt.evidenceKind -ceq 'stage5-executable-originated-receipt' -and
        $receipt.status -ceq 'passed' -and $receipt.role -ceq 'performance-report' -and
        $receipt.producerVersion -ceq '5' -and $receipt.architecture -ceq 'x64' -and
        $receipt.title -ceq $Context.title -and
        $receipt.runId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and
        $receipt.runNonce -cmatch $uuidPattern -and
        $receipt.cohortNonce -cmatch $uuidPattern -and
        $receipt.sourceCommit -ceq $Context.sourceCommit -and
        $receipt.artifactSetSha256 -ceq $Context.artifactSetSha256 -and
        $receipt.runId -ceq [string]$Run.runId -and
        $receipt.runNonce -ceq [string]$Run.runNonce -and
        $receipt.cohortNonce -ceq [string]$Context.cohortNonce -and
        $receipt.cohortCreatedUtc -ceq [string]$Context.cohortCreatedUtc -and
        $cohortCreatedValid -and $recordedValid -and $recorded -ge $cohortCreated -and
        -not $SeenRunIds.ContainsKey([string]$receipt.runId) -and
        -not $SeenRunNonces.ContainsKey([string]$receipt.runNonce)) `
        "$label receipt identity is invalid, stale, or reused."
    Assert-Stage5PerformanceCondition ($receipt.simulationMode -is [string] -and
        $receipt.simulationMode -ceq 'parallel' -and
        $receipt.schedulerStarted -is [bool] -and
        [bool]$receipt.schedulerStarted) `
        "$label receipt does not prove a started parallel scheduler."
    Assert-Stage5PerformanceProperties $receipt.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') "$label runtime closure"
    Assert-Stage5PerformanceCondition ($receipt.runtimeClosure.dependencyManifestSha256 `
        -cmatch '^[0-9A-F]{64}$' -and
        $receipt.runtimeClosure.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
        $receipt.runtimeClosure.dependencyManifestSha256 -ceq
            $Context.runtimeClosure.dependencyManifestSha256 -and
        $receipt.runtimeClosure.closureSha256 -ceq
            $Context.runtimeClosure.closureSha256) `
        "$label runtime closure identity is invalid or substituted."
    $hostObservation = $Run.host
    Assert-Stage5PerformanceProperties $receipt.process @('id',
        'creationTimeUtc100ns', 'startTimeUtc100ns', 'endTimeUtc100ns',
        'identityAvailable', 'exitCodeKnown', 'exitCode', 'exitBoundary') `
        "$label process"
    Assert-Stage5PerformanceProperties $receipt.fixture @('id', 'kind',
        'workloadQualification', 'contentPath', 'identityObserved',
        'contentSha256', 'replayPath', 'retainedReplayPath',
        'retainedReplaySha256', 'seed', 'seedKnown', 'requestedPlayerCount',
        'requestedMinimumUnitCount') "$label fixture"
    Assert-Stage5PerformanceProperties $receipt.workload @('sampling',
        'sampleCount', 'firstFrame', 'lastFrame', 'playerCount', 'rosterStable',
        'contiguous', 'initialUnitCount', 'minimumUnitCount', 'peakUnitCount') `
        "$label workload"
    Assert-Stage5PerformanceProperties $receipt.frameSimulation @(
        'totalNanoseconds', 'maximumNanoseconds', 'sampleCount') `
        "$label frame simulation"
    Assert-Stage5PerformanceProperties $receipt.frames @('start', 'end', 'final',
        'finalCrcKnown', 'finalCrc') "$label frames"
    Assert-Stage5PerformanceProperties $receipt.worker @('requestedCount',
        'effectiveCount', 'policy', 'pinned', 'availableLogicalCpuCount',
        'reservedOwnerCpuCount', 'selectedWorkerCpuCount',
        'selectedWorkerPhysicalCoreCount', 'selectedWorkerPhysicalCoreMask',
        'selectedWorkerPhysicalCoreMaskComplete') "$label worker"
    Assert-Stage5PerformanceProperties $receipt.topology @('source', 'cpuSets',
        'ownerCpuSetIds', 'selectedWorkerCpuSetIds') "$label topology"
    Assert-Stage5PerformanceProperties $receipt.rawEvidence @('verifierBoundary',
        'rawLogPath', 'rawLogSha256', 'timingPath', 'timingSha256',
        'timingClosed', 'timingWriteSucceeded', 'timingTruncated',
        'timingComplete', 'timingSessionCount', 'timingFrameSamples',
        'timingFirstFrame', 'timingLastFrame') `
        "$label raw evidence"
    Assert-Stage5PerformanceProperties $receipt.provenance @('kind',
        'receiptPath', 'processId', 'processCreationUtc', 'executablePath',
        'executableSha256', 'commandLine', 'exitCode') "$label provenance"
    Assert-Stage5PerformanceProperties $receipt.schedulerMetrics @(
        'submittedJobCount', 'executedJobCount', 'stealCount', 'ownerHelpCount',
        'waitCount', 'workerWaitRejectionCount', 'failedJobCount',
        'cancelledJobCount', 'serialFallbackCount', 'totalQueueLatencyNanoseconds',
        'maximumQueueLatencyNanoseconds', 'workerBusyNanoseconds',
        'workerWaitNanoseconds', 'affinityFailureCount', 'injectionHighWater',
        'maximumActiveWorkers', 'availableLogicalCpuCount',
        'reservedOwnerCpuCount', 'selectedWorkerCpuCount',
        'selectedWorkerPhysicalCoreCount', 'selectedWorkerPhysicalCoreMask',
        'selectedWorkerPhysicalCoreMaskComplete') "$label scheduler metrics"
    Assert-Stage5PerformanceCondition ($receipt.frames.end -ge $receipt.frames.start -and
        $receipt.frames.final -eq $receipt.frames.end -and
        [bool]$receipt.frames.finalCrcKnown) "$label frame result is incomplete."
    Assert-Stage5PerformanceCondition ([int]$receipt.process.id -eq [int]$hostObservation.processId -and
        [Int64]$receipt.process.creationTimeUtc100ns -eq
            [Int64]$hostObservation.creationTimeUtc100ns -and
        [Int64]$receipt.process.startTimeUtc100ns -ge
            [Int64]$receipt.process.creationTimeUtc100ns -and
        [Int64]$receipt.process.endTimeUtc100ns -ge
            [Int64]$receipt.process.startTimeUtc100ns -and
        [bool]$receipt.process.identityAvailable -and
        [bool]$receipt.process.exitCodeKnown -and
        [int]$receipt.process.exitCode -eq [int]$hostObservation.exitCode -and
        [int]$hostObservation.exitCode -eq 0 -and
        $receipt.process.exitBoundary -ceq
            'ReplaySimulation::simulateReplaysInThisProcess:return') `
        "$label PID, creation-time, or exit identity does not match the host observation."
    Assert-Stage5PerformanceCondition ($receipt.executableSha256 -ceq
        [string]$hostObservation.executableSha256 -and
        $receipt.executableSha256 -ceq $Context.executableSha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.executablePath),
            [IO.Path]::GetFullPath([string]$hostObservation.executablePath),
            [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals([IO.Path]::GetFullPath([string]$hostObservation.executablePath),
            [IO.Path]::GetFullPath($Context.executablePath),
            [StringComparison]::OrdinalIgnoreCase)) `
        "$label executable path or SHA-256 does not match the host observation."
    Assert-Stage5PerformanceCondition ($receipt.commandLine -ceq
        [string]$hostObservation.commandLine -and
        [string]$hostObservation.argumentString -ceq [string]$Run.expectedArgumentString -and
        ([string]$hostObservation.commandLine).EndsWith(
            ' ' + [string]$Run.expectedArgumentString,
            [StringComparison]::Ordinal)) `
        "$label command line does not match the independently captured host command."
    $fixture = @($Context.fixtures | Where-Object { $_.id -ceq $Run.fixtureId })[0]
    Assert-Stage5PerformanceCondition ($receipt.fixture.kind -is [string] -and
        $receipt.fixture.kind -ceq 'replay' -and
        $receipt.fixture.workloadQualification -is [string] -and
        $receipt.fixture.workloadQualification -ceq 'minimum-qualified' -and
        $receipt.fixture.identityObserved -is [bool] -and
        [bool]$receipt.fixture.identityObserved -and
        $receipt.fixture.contentPath -is [string] -and
        -not [string]::IsNullOrWhiteSpace([string]$receipt.fixture.contentPath) -and
        $receipt.fixture.replayPath -is [string] -and
        -not [string]::IsNullOrWhiteSpace([string]$receipt.fixture.replayPath) -and
        $receipt.fixture.retainedReplayPath -is [string] -and
        [string]::IsNullOrEmpty([string]$receipt.fixture.retainedReplayPath) -and
        $receipt.fixture.retainedReplaySha256 -is [string] -and
        [string]::IsNullOrEmpty([string]$receipt.fixture.retainedReplaySha256) -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.fixture.contentPath),
            [IO.Path]::GetFullPath([string]$receipt.fixture.replayPath),
            [StringComparison]::OrdinalIgnoreCase) -and
        $receipt.fixture.id -ceq $fixture.id -and
        $receipt.fixture.contentSha256 -ceq $fixture.sha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.fixture.replayPath),
            [IO.Path]::GetFullPath($fixture.path),
            [StringComparison]::OrdinalIgnoreCase) -and
        [bool]$receipt.fixture.seedKnown -and
        [UInt32]$receipt.fixture.seed -eq [UInt32]$fixture.seed -and
        [int]$receipt.fixture.requestedPlayerCount -eq
            [int]$fixture.playerCount -and
        [int]$receipt.fixture.requestedMinimumUnitCount -eq
            [int]$fixture.peakUnitCount) `
        "$label fixture receipt does not match canonical reviewed metadata."
    Assert-Stage5PerformanceCondition (
        $receipt.workload.sampling -ceq
            'completed-simulation-frame-boundary-v1' -and
        [int]$receipt.workload.sampleCount -gt 0 -and
        [int]$receipt.workload.firstFrame -eq ([int]$receipt.frames.start + 1) -and
        [int]$receipt.workload.lastFrame -eq [int]$receipt.frames.end -and
        [int]$receipt.workload.lastFrame -ge [int]$receipt.workload.firstFrame -and
        [int]$receipt.workload.sampleCount -eq
            ([int]$receipt.workload.lastFrame - [int]$receipt.workload.firstFrame + 1) -and
        [int]$receipt.workload.playerCount -eq
            [int]$receipt.fixture.requestedPlayerCount -and
        [bool]$receipt.workload.rosterStable -and
        [bool]$receipt.workload.contiguous -and
        [int]$receipt.workload.initialUnitCount -ge
            [int]$receipt.fixture.requestedMinimumUnitCount -and
        [int]$receipt.workload.minimumUnitCount -le
            [int]$receipt.workload.initialUnitCount -and
        [int]$receipt.workload.peakUnitCount -ge
            [int]$receipt.workload.initialUnitCount) `
        "$label completed-frame workload is incomplete or below the requested minimum."
    Assert-Stage5PerformanceCondition (
        [Int64]$receipt.frameSimulation.totalNanoseconds -gt 0 -and
        [Int64]$receipt.frameSimulation.maximumNanoseconds -gt 0 -and
        [Int64]$receipt.frameSimulation.maximumNanoseconds -le
            [Int64]$receipt.frameSimulation.totalNanoseconds -and
        [Int64]$receipt.frameSimulation.sampleCount -ge
            [Int64]$receipt.workload.sampleCount) `
        "$label measured frame simulation timing is incomplete."
    $contextLaneNames = if ($Context.PSObject.Properties.Name -contains 'laneNames') {
        @($Context.laneNames)
    } else { @(Get-Stage5LaneNames ([string]$Context.qualificationMode)) }
    $contextLaneWorkers = if ($Context.PSObject.Properties.Name -contains 'laneWorkers') {
        @($Context.laneWorkers)
    } else { @(Get-Stage5LaneWorkers ([string]$Context.qualificationMode)) }
    $laneIndex = [Array]::IndexOf([object[]]$contextLaneNames, [string]$Run.lane)
    Assert-Stage5PerformanceCondition ($laneIndex -ge 0) `
        "$label contains an unsupported qualification lane."
    $workers = $contextLaneWorkers[$laneIndex]
    Assert-Stage5PerformanceCondition ([int]$receipt.worker.requestedCount -eq $workers -and
        [int]$receipt.worker.effectiveCount -eq $workers -and
        $receipt.worker.policy -ceq 'auto' -and [bool]$receipt.worker.pinned -and
        [int]$receipt.worker.selectedWorkerCpuCount -eq $workers -and
        [int]$receipt.worker.selectedWorkerPhysicalCoreCount -eq $workers -and
        [bool]$receipt.worker.selectedWorkerPhysicalCoreMaskComplete -and
        (Get-Stage5PerformanceMaskBitCount `
            ([UInt64]$receipt.worker.selectedWorkerPhysicalCoreMask)) -eq $workers) `
        "$label does not prove the exact physical worker lane."
    $cpuSetsById = @{}
    foreach ($cpuSet in @($receipt.topology.cpuSets)) {
        Assert-Stage5PerformanceCondition ($null -ne $cpuSet -and
            $cpuSet.PSObject.Properties.Name -contains 'id' -and
            -not $cpuSetsById.ContainsKey([string]$cpuSet.id)) `
            "$label topology receipt repeats or lacks a CPU-set id."
        $cpuSetsById[[string]$cpuSet.id] = $cpuSet
    }
    $independentCpuSetsById = @{}
    foreach ($cpuSet in @($Context.topology.cpuSets)) {
        Assert-Stage5PerformanceCondition ($null -ne $cpuSet -and
            $cpuSet.PSObject.Properties.Name -contains 'id' -and
            -not $independentCpuSetsById.ContainsKey([string]$cpuSet.id)) `
            "$label independent host topology repeats or lacks a CPU-set id."
        $independentCpuSetsById[[string]$cpuSet.id] = $cpuSet
    }
    $selectedIds = @($receipt.topology.selectedWorkerCpuSetIds)
    $selectedPhysical = @{}
    Assert-Stage5PerformanceCondition ($receipt.topology.source -ceq
        'GetSystemCpuSetInformation' -and $selectedIds.Count -eq $workers) `
        "$label topology receipt is incomplete."
    foreach ($id in $selectedIds) {
        Assert-Stage5PerformanceCondition ($cpuSetsById.ContainsKey([string]$id)) `
            "$label selected CPU set $id is absent."
        $cpuSet = $cpuSetsById[[string]$id]
        Assert-Stage5PerformanceCondition ($independentCpuSetsById.ContainsKey([string]$id)) `
            "$label selected CPU set $id is absent from independent host topology."
        $independentCpuSet = $independentCpuSetsById[[string]$id]
        Assert-Stage5PerformanceCondition (
            [UInt32]$cpuSet.id -eq [UInt32]$independentCpuSet.id -and
            [UInt16]$cpuSet.group -eq [UInt16]$independentCpuSet.group -and
            [byte]$cpuSet.coreIndex -eq [byte]$independentCpuSet.coreIndex -and
            [byte]$cpuSet.logicalProcessorIndex -eq
                [byte]$independentCpuSet.logicalProcessorIndex -and
            [byte]$cpuSet.efficiencyClass -eq [byte]$independentCpuSet.efficiencyClass -and
            [bool]$cpuSet.parked -eq [bool]$independentCpuSet.parked -and
            [bool]$cpuSet.allocatedToOtherProcess -eq [bool]$independentCpuSet.allocated -and
            [bool]$cpuSet.availableToProcess -eq [bool]$independentCpuSet.available) `
            "$label receipt CPU set $id does not match independent host topology."
        Assert-Stage5PerformanceCondition ([bool]$cpuSet.availableToProcess -and
            -not [bool]$cpuSet.parked -and
            -not [bool]$cpuSet.allocatedToOtherProcess) `
            "$label selected CPU set $id is not available."
        $physicalKey = "$($cpuSet.group):$($cpuSet.coreIndex)"
        Assert-Stage5PerformanceCondition (-not $selectedPhysical.ContainsKey($physicalKey)) `
            "$label selects sibling logical processors on one physical core."
        $selectedPhysical[$physicalKey] = $true
    }
    Assert-Stage5PerformanceCondition ($selectedPhysical.Count -eq $workers) `
        "$label distinct physical-core count is invalid."
    $rawPath = Resolve-Stage5RunEvidenceFile $Context.taskRoot `
        ([string]$receipt.rawEvidence.rawLogPath) "$label raw diagnostic"
    $timingPath = Resolve-Stage5RunEvidenceFile $Context.taskRoot `
        ([string]$receipt.rawEvidence.timingPath) "$label timing evidence"
    $rawHash = Get-Stage5PerformanceSha256 $rawPath
    $timingHash = Get-Stage5PerformanceSha256 $timingPath
    Assert-Stage5PerformanceCondition ($receipt.rawEvidence.verifierBoundary -ceq
        $script:VerifierBoundary -and
        $receipt.rawEvidence.rawLogSha256 -ceq $rawHash -and
        $receipt.rawEvidence.timingSha256 -ceq $timingHash -and
        [bool]$receipt.rawEvidence.timingClosed -and
        [bool]$receipt.rawEvidence.timingWriteSucceeded -and
        -not [bool]$receipt.rawEvidence.timingTruncated -and
        [bool]$receipt.rawEvidence.timingComplete -and
        [int]$receipt.rawEvidence.timingSessionCount -eq 1 -and
        [Int64]$receipt.rawEvidence.timingFrameSamples -ge
            [Int64]$receipt.workload.sampleCount -and
        [Int64]$receipt.rawEvidence.timingFirstFrame -le
            [Int64]$receipt.frames.start -and
        [Int64]$receipt.rawEvidence.timingLastFrame -ge
            [Int64]$receipt.frames.end -and
        [string]$hostObservation.rawLogSha256 -ceq $rawHash -and
        [string]$hostObservation.timingSha256 -ceq $timingHash) `
        "$label raw/timing closure, frame coverage, or SHA-256 does not match independent host rehashing."
    Assert-Stage5PerformanceCondition ($receipt.rawLogs -is [Array] -and
        @($receipt.rawLogs).Count -eq 2) `
        "$label rawLogs must contain exactly raw-log and timing entries."
    $rawLogEntries = @{}
    foreach ($rawLog in @($receipt.rawLogs)) {
        Assert-Stage5PerformanceProperties $rawLog @('name', 'path', 'sha256') `
            "$label raw log entry"
        $rawName = [string]$rawLog.name
        Assert-Stage5PerformanceCondition (($rawName -ceq 'raw-log' -or
            $rawName -ceq 'timing') -and -not $rawLogEntries.ContainsKey($rawName) -and
            $rawLog.sha256 -cmatch '^[0-9A-F]{64}$') `
            "$label raw log entry is duplicated, unsupported, or lacks a canonical SHA-256."
        $rawEntryPath = Resolve-Stage5RunEvidenceFile $Context.taskRoot `
            ([string]$rawLog.path) "$label raw log '$rawName'"
        $rawEntryHash = Get-Stage5PerformanceSha256 $rawEntryPath
        Assert-Stage5PerformanceCondition ($rawEntryHash -ceq [string]$rawLog.sha256) `
            "$label raw log '$rawName' SHA-256 does not match its executable observation."
        $rawLogEntries[$rawName] = [pscustomobject]@{
            path = $rawEntryPath; sha256 = $rawEntryHash
        }
    }
    Assert-Stage5PerformanceCondition ($rawLogEntries.ContainsKey('raw-log') -and
        $rawLogEntries.ContainsKey('timing') -and
        [String]::Equals($rawLogEntries['raw-log'].path, $rawPath,
            [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals($rawLogEntries['timing'].path, $timingPath,
            [StringComparison]::OrdinalIgnoreCase) -and
        [string]$receipt.rawEvidence.rawLogSha256 -ceq
            [string]$rawLogEntries['raw-log'].sha256 -and
        [string]$receipt.rawEvidence.timingSha256 -ceq
            [string]$rawLogEntries['timing'].sha256) `
        "$label rawEvidence is detached from its rawLogs observations."
    [DateTimeOffset]$provenanceCreation = [DateTimeOffset]::MinValue
    $provenanceCreationValid = [DateTimeOffset]::TryParse(
        [string]$receipt.provenance.processCreationUtc, [ref]$provenanceCreation)
    $hostCreation = [DateTimeOffset]::FromFileTime(
        [Int64]$hostObservation.creationTimeUtc100ns).ToUniversalTime()
    Assert-Stage5PerformanceCondition ($receipt.provenance.kind -ceq
        'native-executable-observation' -and
        $receipt.provenance.processId -eq [int]$hostObservation.processId -and
        $provenanceCreationValid -and
        [string]$receipt.provenance.processCreationUtc -cmatch $isoUtcPattern -and
        $provenanceCreation.UtcTicks -eq $hostCreation.UtcTicks -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.provenance.receiptPath),
            [IO.Path]::GetFullPath($receiptPath),
            [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.provenance.executablePath),
            [IO.Path]::GetFullPath($Context.executablePath),
            [StringComparison]::OrdinalIgnoreCase) -and
        [string]$receipt.provenance.executableSha256 -ceq
            [string]$Context.executableSha256 -and
        [string]$receipt.provenance.commandLine -ceq
            [string]$receipt.commandLine -and
        [int]$receipt.provenance.exitCode -eq 0) `
        "$label native provenance is stale, substituted, or detached."
    Assert-Stage5RawDiagnostic $rawPath $receipt $label
    Assert-Stage5PerformanceCondition ((Get-Item -LiteralPath $timingPath).Length -gt 0) `
        "$label timing evidence is empty."
    Assert-Stage5PerformanceCondition (Test-Stage5PerformanceFinitePositive `
        $hostObservation.elapsedMilliseconds) "$label host elapsed time is invalid."
    $SeenRunIds[[string]$receipt.runId] = $true
    $SeenRunNonces[[string]$receipt.runNonce] = $true
    $SeenReceiptPaths[$receiptPath.ToLowerInvariant()] = $true
    $SeenReceiptHashes[$receiptHash] = $true
    return [pscustomobject]@{
        fixtureId = [string]$Run.fixtureId
        lane = [string]$Run.lane
        ordinal = [int]$Run.ordinal
        warmup = [bool]$Run.warmup
        runId = [string]$Run.runId
        processId = [int]$hostObservation.processId
        processCreationTimeUtc100ns = [Int64]$hostObservation.creationTimeUtc100ns
        elapsedMilliseconds = [double]$hostObservation.elapsedMilliseconds
        receiptPath = $receiptPath
        receiptSha256 = $receiptHash
        rawLogPath = $rawPath
        rawLogSha256 = $rawHash
        timingPath = $timingPath
        timingSha256 = $timingHash
        receiptBinding = [pscustomobject]@{
            path = $receiptPath
            sha256 = $receiptHash
            runId = [string]$receipt.runId
            runNonce = [string]$receipt.runNonce
            cohortNonce = [string]$receipt.cohortNonce
            processId = [int]$receipt.process.id
            processCreationTimeUtc100ns = [Int64]$receipt.process.creationTimeUtc100ns
            executablePath = [string]$receipt.executablePath
            executableSha256 = [string]$receipt.executableSha256
            commandLine = [string]$receipt.commandLine
            rawLogPath = $rawPath
            rawLogSha256 = $rawHash
            timingPath = $timingPath
            timingSha256 = $timingHash
        }
        selectedWorkerCpuSetIds = @($selectedIds | ForEach-Object { [UInt32]$_ })
        selectedPhysicalCoreMask = ([UInt64]$receipt.worker.selectedWorkerPhysicalCoreMask).ToString('X16')
    }
}

function Assert-Stage5PairedOracleBinding {
    param([object]$Binding, [object]$ThroughputRun,
        [object]$ThroughputValidated, [object]$Context,
        [Collections.IDictionary]$SeenRunIds,
        [Collections.IDictionary]$SeenRunNonces,
        [Collections.IDictionary]$SeenReceiptPaths,
        [Collections.IDictionary]$SeenReceiptHashes)
    $label = "Paired oracle '$($ThroughputRun.fixtureId)/$($ThroughputRun.lane)/$($ThroughputRun.ordinal)'"
    Assert-Stage5PerformanceProperties $Binding @('throughputRunId', 'oracleRun') `
        "$label binding"
    Assert-Stage5PerformanceCondition ([string]$Binding.throughputRunId -ceq
        [string]$ThroughputRun.runId) `
        "$label does not bind the scheduled throughput run."
    $oracleRun = $Binding.oracleRun
    Assert-Stage5PerformanceProperties $oracleRun @('fixtureId', 'lane',
        'ordinal', 'warmup', 'runId', 'runNonce', 'expectedArgumentString',
        'receiptPath', 'receiptSha256', 'host') "$label oracle run"
    Assert-Stage5PerformanceCondition ($oracleRun.fixtureId -ceq
        $ThroughputRun.fixtureId -and $oracleRun.lane -ceq $ThroughputRun.lane -and
        [int]$oracleRun.ordinal -eq [int]$ThroughputRun.ordinal -and
        [bool]$oracleRun.warmup -eq [bool]$ThroughputRun.warmup -and
        [string]$oracleRun.runId -cne [string]$ThroughputRun.runId -and
        [string]$oracleRun.runNonce -cne [string]$ThroughputRun.runNonce) `
        "$label oracle run is not the distinct counterpart of the scheduled run."
    $oracleValidated = Assert-Stage5Receipt $oracleRun $Context $SeenRunIds `
        $SeenRunNonces $SeenReceiptPaths $SeenReceiptHashes 'serial-oracle'
    $throughputReceipt = Read-Stage5PerformanceJson `
        ([string]$ThroughputValidated.receiptPath) "$label throughput receipt"
    $oracleReceipt = Read-Stage5PerformanceJson `
        ([string]$oracleValidated.receiptPath) "$label oracle receipt"
    Assert-Stage5PerformanceCondition (
        $throughputReceipt.title -ceq $oracleReceipt.title -and
        $throughputReceipt.architecture -ceq $oracleReceipt.architecture -and
        $throughputReceipt.simulationMode -is [string] -and
        $oracleReceipt.simulationMode -is [string] -and
        $throughputReceipt.simulationMode -ceq $oracleReceipt.simulationMode -and
        $throughputReceipt.schedulerStarted -is [bool] -and
        $oracleReceipt.schedulerStarted -is [bool] -and
        [bool]$throughputReceipt.schedulerStarted -eq
            [bool]$oracleReceipt.schedulerStarted -and
        $throughputReceipt.sourceCommit -ceq $oracleReceipt.sourceCommit -and
        $throughputReceipt.artifactSetSha256 -ceq $oracleReceipt.artifactSetSha256 -and
        $throughputReceipt.executableSha256 -ceq $oracleReceipt.executableSha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$throughputReceipt.executablePath),
            [IO.Path]::GetFullPath([string]$oracleReceipt.executablePath),
            [StringComparison]::OrdinalIgnoreCase) -and
        $throughputReceipt.commandLine -ceq $oracleReceipt.commandLine -and
        $throughputReceipt.cohortNonce -ceq $oracleReceipt.cohortNonce -and
        $throughputReceipt.cohortCreatedUtc -ceq $oracleReceipt.cohortCreatedUtc -and
        $throughputReceipt.runtimeClosure.dependencyManifestSha256 -ceq
            $oracleReceipt.runtimeClosure.dependencyManifestSha256 -and
        $throughputReceipt.runtimeClosure.closureSha256 -ceq
            $oracleReceipt.runtimeClosure.closureSha256 -and
        $throughputReceipt.runId -cne $oracleReceipt.runId -and
        $throughputReceipt.runNonce -cne $oracleReceipt.runNonce -and
        [int]$throughputReceipt.process.id -ne
            [int]$oracleReceipt.process.id -and
        [Int64]$throughputReceipt.process.creationTimeUtc100ns -ne
            [Int64]$oracleReceipt.process.creationTimeUtc100ns -and
        -not [String]::Equals(
            [IO.Path]::GetFullPath([string]$throughputValidated.receiptPath),
            [IO.Path]::GetFullPath([string]$oracleValidated.receiptPath),
            [StringComparison]::OrdinalIgnoreCase)) `
        "$label common executable/provenance identity or distinct-run identity is invalid."

    $fixture = @($Context.fixtures | Where-Object {
        $_.id -ceq $ThroughputRun.fixtureId
    })[0]
    Assert-Stage5PerformanceFixtureHash $fixture "$label fixture"
    Assert-Stage5PerformanceCondition (
        $throughputReceipt.fixture.kind -is [string] -and
        $oracleReceipt.fixture.kind -is [string] -and
        $throughputReceipt.fixture.kind -ceq $oracleReceipt.fixture.kind -and
        $throughputReceipt.fixture.workloadQualification -is [string] -and
        $oracleReceipt.fixture.workloadQualification -is [string] -and
        $throughputReceipt.fixture.workloadQualification -ceq
            $oracleReceipt.fixture.workloadQualification -and
        $throughputReceipt.fixture.identityObserved -is [bool] -and
        $oracleReceipt.fixture.identityObserved -is [bool] -and
        [bool]$throughputReceipt.fixture.identityObserved -eq
            [bool]$oracleReceipt.fixture.identityObserved -and
        $throughputReceipt.fixture.contentPath -is [string] -and
        $oracleReceipt.fixture.contentPath -is [string] -and
        [String]::Equals(
            [IO.Path]::GetFullPath([string]$throughputReceipt.fixture.contentPath),
            [IO.Path]::GetFullPath([string]$oracleReceipt.fixture.contentPath),
            [StringComparison]::OrdinalIgnoreCase) -and
        $throughputReceipt.fixture.retainedReplayPath -is [string] -and
        $oracleReceipt.fixture.retainedReplayPath -is [string] -and
        $throughputReceipt.fixture.retainedReplayPath -ceq
            $oracleReceipt.fixture.retainedReplayPath -and
        $throughputReceipt.fixture.retainedReplaySha256 -is [string] -and
        $oracleReceipt.fixture.retainedReplaySha256 -is [string] -and
        $throughputReceipt.fixture.retainedReplaySha256 -ceq
            $oracleReceipt.fixture.retainedReplaySha256 -and
        $throughputReceipt.fixture.id -ceq $oracleReceipt.fixture.id -and
        $throughputReceipt.fixture.contentSha256 -ceq $oracleReceipt.fixture.contentSha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$throughputReceipt.fixture.replayPath),
            [IO.Path]::GetFullPath([string]$oracleReceipt.fixture.replayPath),
            [StringComparison]::OrdinalIgnoreCase) -and
        [UInt32]$throughputReceipt.fixture.seed -eq
            [UInt32]$oracleReceipt.fixture.seed -and
        [bool]$throughputReceipt.fixture.seedKnown -and
        [bool]$oracleReceipt.fixture.seedKnown -and
        [int]$throughputReceipt.fixture.requestedPlayerCount -eq
            [int]$oracleReceipt.fixture.requestedPlayerCount -and
        [int]$throughputReceipt.fixture.requestedMinimumUnitCount -eq
            [int]$oracleReceipt.fixture.requestedMinimumUnitCount) `
        "$label fixture provenance does not match exactly."
    foreach ($property in @('sampling', 'sampleCount', 'firstFrame',
            'lastFrame', 'playerCount', 'rosterStable', 'contiguous',
            'initialUnitCount', 'minimumUnitCount', 'peakUnitCount')) {
        Assert-Stage5PerformanceCondition (
            [string]$throughputReceipt.workload.$property -ceq
                [string]$oracleReceipt.workload.$property) `
            "$label workload '$property' differs between throughput and oracle."
    }
    foreach ($property in @('start', 'end', 'final', 'finalCrcKnown', 'finalCrc')) {
        Assert-Stage5PerformanceCondition (
            [string]$throughputReceipt.frames.$property -ceq
                [string]$oracleReceipt.frames.$property) `
            "$label frame result '$property' differs between throughput and oracle."
    }
    foreach ($property in @('requestedCount', 'effectiveCount', 'policy',
            'pinned', 'availableLogicalCpuCount', 'reservedOwnerCpuCount',
            'selectedWorkerCpuCount', 'selectedWorkerPhysicalCoreCount',
            'selectedWorkerPhysicalCoreMask',
            'selectedWorkerPhysicalCoreMaskComplete')) {
        Assert-Stage5PerformanceCondition (
            [string]$throughputReceipt.worker.$property -ceq
                [string]$oracleReceipt.worker.$property) `
            "$label worker policy '$property' differs between throughput and oracle."
    }
    Assert-Stage5PerformanceCondition (
        (ConvertTo-Json $throughputReceipt.topology -Depth 20 -Compress) -ceq
        (ConvertTo-Json $oracleReceipt.topology -Depth 20 -Compress)) `
        "$label CPU topology differs between throughput and oracle."

    $throughputReferenceStreams = @($throughputReceipt.kernelReference.streams)
    $oracleReferenceStreams = @($oracleReceipt.kernelReference.streams)
    Assert-Stage5PerformanceCondition ($throughputReferenceStreams.Count -gt 0 -and
        $throughputReferenceStreams.Count -eq $oracleReferenceStreams.Count) `
        "$label requires a nonempty, equally covered canonical reference stream set."
    $oracleReferenceByKey = @{}
    foreach ($stream in $oracleReferenceStreams) {
        $key = "$( [string]$stream.name ):$( [UInt64]$stream.subtype )"
        Assert-Stage5PerformanceCondition (-not $oracleReferenceByKey.ContainsKey($key)) `
            "$label oracle reference stream set is duplicated."
        $oracleReferenceByKey[$key] = $stream
    }
    foreach ($stream in $throughputReferenceStreams) {
        $key = "$( [string]$stream.name ):$( [UInt64]$stream.subtype )"
        Assert-Stage5PerformanceCondition $oracleReferenceByKey.ContainsKey($key) `
            "$label oracle reference stream '$key' is missing."
        $oracleStream = $oracleReferenceByKey[$key]
        foreach ($property in @('name', 'subtype', 'fieldSchema', 'firstFrame',
                'lastFrame', 'validatedBatchCount', 'committedBatchCount',
                'abortedBatchCount', 'validatedOperationCount',
                'committedOperationCount', 'inputSha256', 'outputSha256',
                'commitSha256')) {
            Assert-Stage5PerformanceCondition (
                [string]$stream.$property -ceq [string]$oracleStream.$property) `
                "$label reference stream '$key' field '$property' differs."
        }
    }
    $throughputTimingByKey = @{}
    $oracleTimingByKey = @{}
    foreach ($pair in @(
            [pscustomobject]@{ streams = @($throughputReceipt.kernelTiming.streams); map = $throughputTimingByKey; name = 'throughput' },
            [pscustomobject]@{ streams = @($oracleReceipt.kernelTiming.streams); map = $oracleTimingByKey; name = 'oracle' })) {
        foreach ($stream in $pair.streams) {
            $key = "$( [string]$stream.name ):$( [UInt64]$stream.subtype )"
            Assert-Stage5PerformanceCondition (-not $pair.map.ContainsKey($key)) `
                "$label $($pair.name) kernel timing stream set is duplicated."
            $pair.map[$key] = $stream
        }
    }
    Assert-Stage5PerformanceCondition ($throughputTimingByKey.Count -eq
        $oracleTimingByKey.Count) "$label kernel timing stream coverage differs."
    foreach ($key in $throughputTimingByKey.Keys) {
        Assert-Stage5PerformanceCondition $oracleTimingByKey.ContainsKey($key) `
            "$label oracle kernel timing stream '$key' is missing."
        $throughputTiming = $throughputTimingByKey[$key]
        $oracleTiming = $oracleTimingByKey[$key]
        foreach ($property in @('admittedBatches', 'committedBatches')) {
            Assert-Stage5PerformanceCondition (
                [string]$throughputTiming.$property -ceq
                    [string]$oracleTiming.$property) `
                "$label kernel timing '$key' field '$property' differs."
        }
    }
    return [pscustomobject]@{
        throughputRunId = [string]$ThroughputRun.runId
        oracleRun = $oracleValidated
    }
}

function Assert-Stage5PerformanceRunSet {
    param([object]$Document)
    $manifestProperties = @('schemaVersion', 'title',
        'qualificationMode', 'stage3SourceCommit',
        'sourceCommit', 'artifactSetSha256', 'artifactSetManifestPath',
        'runtimeClosure', 'cohortNonce', 'cohortCreatedUtc', 'executablePath',
        'executableSha256',
        'fixtureManifestSha256', 'stage3BaselineSha256', 'taskRoot', 'warmupRuns',
        'measuredRuns', 'fixtures', 'stage3Fixtures', 'topology', 'runs')
    $hasReferencePolicy = ($Document.PSObject.Properties.Name -contains
        'referencePolicy')
    $hasOracleBindings = ($Document.PSObject.Properties.Name -contains
        'pairedOracleBindings')
    Assert-Stage5PerformanceCondition ($hasReferencePolicy -eq $hasOracleBindings) `
        'Stage 5 host validation manifest must bind referencePolicy and pairedOracleBindings together.'
    if ($hasReferencePolicy) {
        $manifestProperties += @('referencePolicy', 'pairedOracleBindings')
    }
    $hasFixtureManifestPath = ($Document.PSObject.Properties.Name -contains
        'fixtureManifestPath')
    if ($hasFixtureManifestPath) {
        $manifestProperties += 'fixtureManifestPath'
    }
    Assert-Stage5PerformanceProperties $Document $manifestProperties `
        'Stage 5 host validation manifest'
    $mode = [string]$Document.qualificationMode
    $referencePolicy = if ($hasReferencePolicy) {
        [string]$Document.referencePolicy
    } else { 'throughput-only' }
    Assert-Stage5PerformanceCondition (@('throughput-only',
        'paired-serial-oracle-v1') -ccontains $referencePolicy) `
        'Stage 5 host validation reference policy is invalid.'
    $pairedOracleBindings = @()
    if ($hasOracleBindings) {
        $pairedOracleBindings = @($Document.pairedOracleBindings)
    }
    Assert-Stage5PerformanceCondition ($referencePolicy -ceq
        'paired-serial-oracle-v1' -or $pairedOracleBindings.Count -eq 0) `
        'Throughput-only validation cannot carry serial-oracle bindings.'
    $artifactBinding = Read-Stage5PerformanceArtifactSet `
        ([string]$Document.artifactSetManifestPath) `
        ([string]$Document.artifactSetSha256) ([string]$Document.sourceCommit) `
        ([string]$Document.title) ([string]$Document.executablePath) `
        ([string]$Document.executableSha256)
    Assert-Stage5PerformanceCondition ($artifactBinding.sha256 -ceq
        [string]$Document.artifactSetSha256 -and
        $artifactBinding.runtimeClosure.dependencyManifestSha256 -ceq
            [string]$Document.runtimeClosure.dependencyManifestSha256 -and
        $artifactBinding.runtimeClosure.closureSha256 -ceq
            [string]$Document.runtimeClosure.closureSha256) `
        'Stage 5 host validation artifact/runtime closure is detached from its independently rehashed manifest.'
    if ($hasFixtureManifestPath) {
        Assert-Stage5PerformanceFileHash ([string]$Document.fixtureManifestPath) `
            ([string]$Document.fixtureManifestSha256) `
            'Stage 5 host validation fixture manifest SHA-256' | Out-Null
    }
    Assert-Stage5PerformanceCondition (@('External16Core', 'LocalCapacitySmoke') -ccontains $mode) `
        'Stage 5 host validation manifest qualification mode is invalid.'
    $laneNames = @(Get-Stage5LaneNames $mode)
    $laneWorkers = @(Get-Stage5LaneWorkers $mode)
    Assert-Stage5PerformanceCondition ($Document.schemaVersion -eq 1 -and
        @('Generals', 'ZeroHour') -ccontains [string]$Document.title -and
        $Document.sourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $Document.artifactSetSha256 -cmatch '^[0-9A-F]{64}$' -and
        -not [string]::IsNullOrWhiteSpace([string]$Document.artifactSetManifestPath) -and
        $Document.cohortNonce -cmatch
            '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
        $Document.cohortCreatedUtc -match
            '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$' -and
        $Document.runtimeClosure.dependencyManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $Document.runtimeClosure.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
        $Document.executableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $Document.fixtureManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        ($mode -ceq 'LocalCapacitySmoke' -or
            $Document.stage3SourceCommit -cmatch '^[0-9a-f]{40}$') -and
        ($mode -ceq 'LocalCapacitySmoke' -or
            $Document.stage3BaselineSha256 -cmatch '^[0-9A-F]{64}$') -and
        [int]$Document.warmupRuns -eq 1 -and [int]$Document.measuredRuns -ge 3) `
        'Stage 5 host validation manifest identity is invalid.'
    Assert-Stage5PerformanceProperties $Document.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Stage 5 host validation runtime closure'
    Assert-Stage5PerformanceCondition ($Document.topology.source -ceq
        'GetSystemCpuSetInformation' -and
        (($mode -ceq 'External16Core' -and
            [int]$Document.topology.physicalCoreCount -ge 16 -and
            [int]$Document.topology.logicalProcessorCount -ge 16) -or
         ($mode -ceq 'LocalCapacitySmoke' -and
            [int]$Document.topology.physicalCoreCount -ge 4 -and
            [int]$Document.topology.physicalCoreCount -le 6 -and
            [int]$Document.topology.logicalProcessorCount -le 12))) `
        'Stage 5 performance validation topology does not match its qualification mode.'
    $fixtures = @($Document.fixtures)
    $stage3 = @($Document.stage3Fixtures)
    Assert-Stage5PerformanceCondition ($fixtures.Count -eq 4 -and
        (($mode -ceq 'External16Core' -and $stage3.Count -eq 4) -or
         ($mode -ceq 'LocalCapacitySmoke' -and $stage3.Count -eq 0))) `
        'Stage 5 validation manifest has invalid current/Stage 3 fixture coverage.'
    for ($index = 0; $index -lt 4; ++$index) {
        Assert-Stage5PerformanceCondition ($fixtures[$index].id -ceq
            $script:CanonicalFixtureIds[$index] -and
            [int]$fixtures[$index].playerCount -eq 8 -and
            [int]$fixtures[$index].peakUnitCount -ge
                $script:CanonicalFixtureUnits[$index] -and
            ($mode -ceq 'LocalCapacitySmoke' -or
                ($stage3[$index].id -ceq $fixtures[$index].id -and
                    (Test-Stage5PerformanceFinitePositive `
                        $stage3[$index].measuredMedianMilliseconds)))) `
            "Stage 5 validation manifest fixture $index is not canonical."
    }
    $expectedPerLane = 1 + [int]$Document.measuredRuns
    $expectedTotal = 4 * $laneNames.Count * $expectedPerLane
    $runs = @($Document.runs)
    Assert-Stage5PerformanceCondition ($runs.Count -eq $expectedTotal) `
        "Stage 5 run schedule requires exactly $expectedTotal runs."
    $seenRunIds = @{}
    $seenRunNonces = @{}
    $seenReceiptPaths = @{}
    $seenReceiptHashes = @{}
    $validated = @()
    $validatedByRunId = @{}
    foreach ($fixture in $fixtures) {
        Assert-Stage5PerformanceFixtureHash $fixture `
            "Fixture '$($fixture.id)' before host receipt validation"
        foreach ($lane in $laneNames) {
            $scheduled = @($runs | Where-Object {
                $_.fixtureId -ceq $fixture.id -and $_.lane -ceq $lane
            } | Sort-Object ordinal)
            Assert-Stage5PerformanceCondition ($scheduled.Count -eq $expectedPerLane) `
                "Fixture '$($fixture.id)' is missing lane '$lane'."
            for ($ordinal = 0; $ordinal -lt $expectedPerLane; ++$ordinal) {
                Assert-Stage5PerformanceCondition ([int]$scheduled[$ordinal].ordinal -eq
                    $ordinal -and [bool]$scheduled[$ordinal].warmup -eq ($ordinal -eq 0)) `
                    "Fixture '$($fixture.id)' lane '$lane' has an invalid warmup/measured schedule."
                $validatedRun = Assert-Stage5Receipt $scheduled[$ordinal] $Document `
                    $seenRunIds $seenRunNonces $seenReceiptPaths $seenReceiptHashes
                Assert-Stage5PerformanceCondition (-not $validatedByRunId.ContainsKey(
                    [string]$validatedRun.runId)) `
                    "Fixture '$($fixture.id)' lane '$lane' reuses a throughput run id."
                $validatedByRunId[[string]$validatedRun.runId] = $validatedRun
                $validated += $validatedRun
            }
        }
    }
    $validatedPairs = @()
    if ($referencePolicy -ceq 'paired-serial-oracle-v1') {
        Assert-Stage5PerformanceCondition ($pairedOracleBindings.Count -eq
            $validated.Count) `
            'Paired serial-oracle validation requires exactly one binding for every throughput run, including warmups.'
        $seenThroughputBindings = @{}
        foreach ($binding in $pairedOracleBindings) {
            Assert-Stage5PerformanceCondition (-not $seenThroughputBindings.ContainsKey(
                [string]$binding.throughputRunId)) `
                'Paired serial-oracle validation repeats a throughput run binding.'
            $throughputRunId = [string]$binding.throughputRunId
            Assert-Stage5PerformanceCondition $validatedByRunId.ContainsKey(
                $throughputRunId) `
                "Paired serial-oracle validation references unknown throughput run '$throughputRunId'."
            $seenThroughputBindings[$throughputRunId] = $true
            $throughputRun = @($runs | Where-Object {
                [string]$_.runId -ceq $throughputRunId
            })[0]
            $validatedPairs += Assert-Stage5PairedOracleBinding $binding `
                $throughputRun $validatedByRunId[$throughputRunId] $Document `
                $seenRunIds $seenRunNonces $seenReceiptPaths $seenReceiptHashes
        }
        Assert-Stage5PerformanceCondition ($seenThroughputBindings.Count -eq
            $validated.Count) `
            'Paired serial-oracle validation has incomplete throughput coverage.'
    }
    $fixtureResults = @()
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $fixture = $fixtures[$fixtureIndex]
        $laneMedians = @{}
        foreach ($lane in $laneNames) {
            $values = @($validated | Where-Object {
                $_.fixtureId -ceq $fixture.id -and $_.lane -ceq $lane -and
                    -not $_.warmup
            } | ForEach-Object { [double]$_.elapsedMilliseconds })
            $laneMedians[$lane] = Get-Stage5PerformanceMedian $values
        }
        if ($mode -ceq 'External16Core') {
            $one = [double]$laneMedians['forced-one']
            $eight = [double]$laneMedians['physical-8']
            $sixteen = [double]$laneMedians['physical-16']
            $baseline = [double]$stage3[$fixtureIndex].measuredMedianMilliseconds
            $regression = $one / $baseline
            $speedup8 = $one / $eight
            $scale16 = $eight / $sixteen
            Assert-Stage5PerformanceCondition ($regression -le 1.05) `
                "Fixture '$($fixture.id)' forced-one regression ratio $regression exceeds 1.05."
            Assert-Stage5PerformanceCondition ($speedup8 -ge 2.0) `
                "Fixture '$($fixture.id)' physical-8 speedup $speedup8 is below 2.0x."
            Assert-Stage5PerformanceCondition ($scale16 -gt 1.0) `
                "Fixture '$($fixture.id)' physical-16 does not scale positively from physical-8."
            $fixtureResults += [pscustomobject]@{
                id = [string]$fixture.id
                playerCount = 8
                peakUnitCount = [int]$fixture.peakUnitCount
                measuredRuns = [int]$Document.measuredRuns
                stage3ForcedOneMedianMilliseconds = $baseline
                stage5ForcedOneMedianMilliseconds = $one
                physical8MedianMilliseconds = $eight
                physical16MedianMilliseconds = $sixteen
                forcedOneRegressionRatio = $regression
                physical8Speedup = $speedup8
                physical8To16Speedup = $scale16
            }
        }
        else {
            $fixtureResults += [pscustomobject]@{
                id = [string]$fixture.id
                playerCount = 8
                peakUnitCount = [int]$fixture.peakUnitCount
                measuredRuns = [int]$Document.measuredRuns
                laneMedians = [pscustomobject]$laneMedians
                qualificationClass = 'local-capacity-smoke'
            }
        }
    }
    return [pscustomobject]@{
        qualificationMode = $mode
        referencePolicy = $referencePolicy
        runs = $validated
        pairedOracleBindings = $validatedPairs
        fixtures = $fixtureResults
    }
}

function New-Stage5ProcessStartInfo {
    param([string]$Executable, [string]$Arguments, [string]$WorkingDirectory,
        [Collections.IDictionary]$Environment)
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($name in $Environment.Keys) {
        $info.EnvironmentVariables[[string]$name] = [string]$Environment[$name]
    }
    foreach ($name in @('RTS_PERFORMANCE_RAW_LOG_SHA256',
            'RTS_PERFORMANCE_TIMING_SHA256')) {
        if ($info.EnvironmentVariables.ContainsKey($name)) {
            $info.EnvironmentVariables.Remove($name)
        }
    }
    return $info
}

function Invoke-Stage5InstalledPerformanceRun {
    param([object]$Context, [object]$Fixture, [string]$Lane,
        [int]$Ordinal, [int]$WorkerCount, [int]$Timeout,
        [object]$TitleSessionContract,
        [ValidateSet('throughput-binding', 'serial-oracle')]
        [string]$ReferenceMode = 'throughput-binding')
    Assert-Stage5PerformanceCondition ($null -ne $TitleSessionContract -and
        $TitleSessionContract.title -ceq $Context.title) `
        "Stage 5 $($Context.title) performance run has no title-session profile contract."
    if ($null -eq $Context.PSObject.Properties['processCleanup']) {
        $Context | Add-Member -MemberType NoteProperty -Name processCleanup `
            -Value ([pscustomobject]@{})
    }
    $Context.processCleanup = [pscustomobject]@{
        processId = 0; exitProof = $true; blocked = $false; errors = @()
    }
    $runNonce = [Guid]::NewGuid().ToString()
    $runId = ('s5perf-{0}-{1}-{2}-{3}' -f $Fixture.id, $Lane, $Ordinal,
        $runNonce)
    $runRoot = Join-Path $Context.taskRoot $runId
    $receiptDirectory = Join-Path $runRoot 'receipt'
    $timingDirectory = Join-Path $runRoot 'timing'
    $tempDirectory = Join-Path $runRoot 'temp'
    foreach ($directory in @($runRoot, $receiptDirectory, $timingDirectory,
            $tempDirectory)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
    }
    $rawPath = Join-Path $runRoot 'game-owned-raw.log'
    $stdoutPath = Join-Path $runRoot 'host-stdout.log'
    $stderrPath = Join-Path $runRoot 'host-stderr.log'
    $arguments = Get-Stage5PerformanceArguments $Fixture $WorkerCount `
        $Context.executableSha256
    $environment = @{
        RTS_PERFORMANCE_ROLE = 'performance-report'
        # Throughput and serial-oracle processes are launched separately.  Their
        # receipt roles and reference ledgers are compared after both runs;
        # oracle elapsed time is never admitted to throughput medians.
        RTS_PERFORMANCE_REFERENCE_MODE = $ReferenceMode
        RTS_PERFORMANCE_RUN_ID = $runId
        RTS_PERFORMANCE_RUN_NONCE = $runNonce
        RTS_PERFORMANCE_COHORT_NONCE = $Context.cohortNonce
        RTS_PERFORMANCE_COHORT_CREATED_UTC = $Context.cohortCreatedUtc
        RTS_PERFORMANCE_RECEIPT_DIR = $receiptDirectory
        RTS_PERFORMANCE_SOURCE_COMMIT = $Context.sourceCommit
        RTS_PERFORMANCE_ARTIFACT_SET_SHA256 = $Context.artifactSetSha256
        RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256 = $Context.runtimeClosure.dependencyManifestSha256
        RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256 = $Context.runtimeClosure.closureSha256
        RTS_PERFORMANCE_FIXTURE_ID = $Fixture.id
        RTS_PERFORMANCE_FIXTURE_SHA256 = $Fixture.sha256
        RTS_PERFORMANCE_FIXTURE_KIND = 'replay'
        RTS_PERFORMANCE_WORKLOAD_QUALIFICATION = 'minimum-qualified'
        RTS_PERFORMANCE_RAW_LOG_PATH = $rawPath
        RTS_PERFORMANCE_TIMING_PATH = $timingDirectory
        RTS_PERFORMANCE_VERIFIER_BOUNDARY = $script:VerifierBoundary
        RTS_PERFORMANCE_SEED = [string]$Fixture.seed
        RTS_PERFORMANCE_PLAYER_COUNT = '8'
        RTS_PERFORMANCE_UNIT_COUNT = [string]$Fixture.peakUnitCount
        RTS_STAGE5_RUN_NONCE = $runNonce
        RTS_STAGE5_COHORT_NONCE = $Context.cohortNonce
        RTS_STAGE5_COHORT_CREATED_UTC = $Context.cohortCreatedUtc
        RTS_STAGE5_RUNTIME_MANIFEST_SHA256 = $Context.runtimeClosure.dependencyManifestSha256
        RTS_STAGE5_RUNTIME_CLOSURE_SHA256 = $Context.runtimeClosure.closureSha256
        RTS_FRAME_TIMING_DIR = $timingDirectory
        TEMP = $tempDirectory
        TMP = $tempDirectory
    }
    foreach ($name in $TitleSessionContract.environmentValues.Keys) {
        $environment[[string]$name] = [string]$TitleSessionContract.environmentValues[$name]
    }
    $info = New-Stage5ProcessStartInfo $Context.executablePath $arguments `
        (Split-Path -Parent $Context.executablePath) $environment
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $processStarted = $false
    $processIdentity = $null
    $stdoutTask = $null
    $stderrTask = $null
    $exitCode = $null
    $runError = $null
    $captureErrors = New-Object 'Collections.Generic.List[string]'
    # The self-test parameter set returns before this function is reachable.
    try {
        $processStarted = $process.Start()
        Assert-Stage5PerformanceCondition $processStarted `
            "Failed to start installed performance run '$runId'."
        $processIdentity = Get-Stage5ProcessIdentity $process
        $hostProcessId = $processIdentity.processId
        $hostCreationTime = $processIdentity.creationTimeUtc100ns
        $hostExecutablePath = $processIdentity.executablePath
        $hostExecutableHash = $processIdentity.executableSha256
        $hostCommandLine = $processIdentity.commandLine
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $exited = $process.WaitForExit($Timeout * 1000)
        if (-not $exited) {
            Stop-Stage5ProcessSafely $process $processIdentity
            throw "Installed performance run '$runId' exceeded $Timeout seconds."
        }
        $process.WaitForExit()
        $stopwatch.Stop()
        $exitCode = $process.ExitCode
    }
    catch { $runError = $_ }
    finally {
        $stopwatch.Stop()
        $ownedCleanup = Invoke-Stage5OwnedProcessCleanup $process $processStarted `
            $processIdentity 30000
        if ($null -ne $Context.processCleanup) {
            $Context.processCleanup.processId = $ownedCleanup.processId
            $Context.processCleanup.exitProof = $ownedCleanup.exitProof
            $Context.processCleanup.blocked = $ownedCleanup.blocked
            $Context.processCleanup.errors = @($ownedCleanup.errors)
        }
        foreach ($cleanupError in @($ownedCleanup.errors)) {
            $captureErrors.Add([string]$cleanupError) | Out-Null
        }
        foreach ($capture in @(
                [pscustomobject]@{ task = $stdoutTask; path = $stdoutPath; name = 'stdout' },
                [pscustomobject]@{ task = $stderrTask; path = $stderrPath; name = 'stderr' })) {
            try {
                if ($null -eq $capture.task) {
                    [IO.File]::WriteAllText($capture.path, '')
                }
                else {
                    if (-not $capture.task.Wait(30000)) {
                        throw "Stage 5 $($capture.name) did not complete within the bounded post-process wait."
                    }
                    [IO.File]::WriteAllText($capture.path, $capture.task.Result)
                }
            }
            catch {
                $captureErrors.Add("$($capture.name): $($_.Exception.Message)") | Out-Null
            }
        }
        try { $process.Dispose() }
        catch { $captureErrors.Add("process dispose: $($_.Exception.Message)") | Out-Null }
    }
    if ($null -ne $runError) {
        if ($captureErrors.Count -gt 0) {
            throw "Installed performance run '$runId' failed: $($runError.Exception.Message); output capture also failed: $($captureErrors.ToArray() -join ' | ')"
        }
        throw $runError
    }
    if ($captureErrors.Count -gt 0) {
        throw "Installed performance run '$runId' output capture failed: $($captureErrors.ToArray() -join ' | ')"
    }
    if ($null -ne $Context.processCleanup -and
        [bool]$Context.processCleanup.blocked) {
        throw "Installed performance run '$runId' cannot prove owned child exit for PID $($Context.processCleanup.processId); cleanup is blocked."
    }
    Assert-Stage5PerformanceCondition ($exitCode -eq 0) `
        "Installed performance run '$runId' exited with code $exitCode."
    $diagnosticText = [IO.File]::ReadAllText($stdoutPath) + "`n" +
        [IO.File]::ReadAllText($stderrPath)
    Assert-Stage5PerformanceCondition ($diagnosticText -notmatch $script:Stage5FatalPattern) `
        "Installed performance run '$runId' reported a fatal diagnostic."
    $receiptFiles = @(Get-ChildItem -LiteralPath $receiptDirectory -File -Filter `
        'performance-receipt-*.json')
    Assert-Stage5PerformanceCondition ($receiptFiles.Count -eq 1) `
        "Installed performance run '$runId' did not emit exactly one receipt."
    return [pscustomobject]@{
        fixtureId = $Fixture.id
        lane = $Lane
        ordinal = $Ordinal
        warmup = ($Ordinal -eq 0)
        runId = $runId
        runNonce = $runNonce
        expectedArgumentString = $arguments
        receiptPath = $receiptFiles[0].FullName
        receiptSha256 = Get-Stage5PerformanceSha256 $receiptFiles[0].FullName
        host = [pscustomobject]@{
            processId = $hostProcessId
            creationTimeUtc100ns = $hostCreationTime
            executablePath = $hostExecutablePath
            executableSha256 = $hostExecutableHash
            commandLine = $hostCommandLine
            parentProcessId = [int]$processIdentity.parentProcessId
            parentCreationTimeUtc100ns = [Int64]$processIdentity.parentCreationTimeUtc100ns
            argumentString = $arguments
            exitCode = $exitCode
            elapsedMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
            rawLogSha256 = Get-Stage5PerformanceSha256 $rawPath
            timingSha256 = Get-Stage5PerformanceSha256 `
                (@(Get-ChildItem -LiteralPath $timingDirectory -File -Filter `
                    'frame-timing-*.csv')[0].FullName)
        }
    }
}

function Write-Stage5JsonAtomically {
    param([string]$Path, [object]$Value)
    $temporary = "$Path.tmp-$PID-$([Guid]::NewGuid().ToString('N'))"
    [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 20))
    try {
        if ([IO.File]::Exists($Path)) {
            [IO.File]::Replace($temporary, $Path, $null)
        }
        else {
            [IO.File]::Move($temporary, $Path)
        }
    }
    catch {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        throw
    }
}

function Write-Stage5RegistryRecoveryJournal {
    param([string]$Path, [string]$Title, [string]$TaskRootPath,
        [object[]]$Snapshots, [object]$ProcessCleanup,
        [string]$State = 'pending')
    $processId = 0
    $exitProof = $true
    $blocked = $false
    if ($null -ne $ProcessCleanup) {
        $processId = [int]$ProcessCleanup.processId
        $exitProof = [bool]$ProcessCleanup.exitProof
        $blocked = [bool]$ProcessCleanup.blocked
    }
    $journalSnapshots = @($Snapshots | ForEach-Object {
        [ordered]@{
            view = [string]$_.view
            subKey = [string]$_.subKey
            name = [string]$_.name
            hadKey = [bool]$_.hadKey
            hadValue = [bool]$_.hadValue
            oldValue = $_.oldValue
            oldKind = if ($null -eq $_.oldKind) { $null } else { [int]$_.oldKind }
            createdSubKeys = @($_.createdSubKeys)
        }
    })
    $journal = [ordered]@{
        schemaVersion = 1
        state = $State
        title = $Title
        taskRoot = $TaskRootPath
        processId = $processId
        childExitProof = $exitProof
        cleanupBlocked = $blocked
        recoveryInstructions = 'Restore every snapshot under both recorded registry views before removing this task root or title-session profile.'
        snapshots = $journalSnapshots
    }
    Write-Stage5JsonAtomically $Path $journal
}

if ($PSCmdlet.ParameterSetName -ceq 'SelfTest') {
    $selfTestPath = [IO.Path]::GetFullPath($SelfTestValidationManifestPath)
    $selfTestDocument = Read-Stage5PerformanceJson $selfTestPath `
        'Stage 5 host self-test validation manifest'
    $selfTestArtifactBinding = Read-Stage5PerformanceArtifactSet `
        ([string]$selfTestDocument.artifactSetManifestPath) `
        ([string]$selfTestDocument.artifactSetSha256) `
        ([string]$selfTestDocument.sourceCommit) `
        ([string]$selfTestDocument.title) `
        ([string]$selfTestDocument.executablePath) `
        ([string]$selfTestDocument.executableSha256)
    $selfTestLauncherContract = Get-Stage5LauncherContract `
        (Split-Path -Parent ([string]$selfTestDocument.executablePath)) `
        ([string]$selfTestDocument.executablePath)
    Assert-Stage5PerformanceLauncherBinding $selfTestArtifactBinding `
        $selfTestLauncherContract ([string]$selfTestDocument.title)
    $selfTestLocks = Open-Stage5PerformanceReadOnlyLocks `
        $selfTestArtifactBinding @($selfTestDocument.fixtures)
    try { }
    finally { Dispose-Stage5PerformanceReadOnlyLocks $selfTestLocks }
    Assert-Stage5PerformanceCondition (
        [String]::Equals([IO.Path]::GetFullPath($selfTestArtifactBinding.path),
            [IO.Path]::GetFullPath([string]$selfTestDocument.artifactSetManifestPath),
            [StringComparison]::OrdinalIgnoreCase) -and
        $selfTestArtifactBinding.sha256 -ceq [string]$selfTestDocument.artifactSetSha256 -and
        $selfTestArtifactBinding.runtimeClosure.dependencyManifestSha256 -ceq
            [string]$selfTestDocument.runtimeClosure.dependencyManifestSha256 -and
        $selfTestArtifactBinding.runtimeClosure.closureSha256 -ceq
            [string]$selfTestDocument.runtimeClosure.closureSha256) `
        'Stage 5 self-test runtime closure is detached from its independently loaded artifact set.'
    $validatedSelfTest = Assert-Stage5PerformanceRunSet $selfTestDocument
    Write-Output ("Stage 5 performance host self-test validation passed: {0} runs." -f
        @($validatedSelfTest.runs).Count)
    return
}

Assert-Stage5PerformanceSourceCommit $ExpectedSourceCommit 'ExpectedSourceCommit'
Assert-Stage5PerformanceCondition ([bool]$AllowHeadlessDirectExecution) `
    'Installed performance validation requires the reviewed -AllowHeadlessDirectExecution exception.'
foreach ($binding in @(
        @('ExpectedExecutableSha256', $ExpectedExecutableSha256),
        @('ExpectedArtifactSetSha256', $ExpectedArtifactSetSha256),
        @('ExpectedFixtureManifestSha256', $ExpectedFixtureManifestSha256))) {
    Assert-Stage5PerformanceHash ([string]$binding[1]) ([string]$binding[0])
}
$isExternalQualification = $QualificationMode -ceq 'External16Core'
if ($isExternalQualification) {
    Assert-Stage5PerformanceSourceCommit $ExpectedStage3SourceCommit `
        'ExpectedStage3SourceCommit'
    foreach ($binding in @(
            @('ExpectedStage3BaselineSha256', $ExpectedStage3BaselineSha256),
            @('ExpectedStage3ExecutableSha256', $ExpectedStage3ExecutableSha256))) {
        Assert-Stage5PerformanceHash ([string]$binding[1]) ([string]$binding[0])
    }
}
$executableFull = [IO.Path]::GetFullPath($InstalledExecutablePath)
Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $executableFull -PathType Leaf) `
    "Installed executable was not found: $executableFull"
$expectedLeaf = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
Assert-Stage5PerformanceCondition ([IO.Path]::GetFileName($executableFull) -ceq $expectedLeaf) `
    "Installed executable leaf must be '$expectedLeaf'."
Assert-Stage5PerformanceCondition ((Get-Stage5PeMachine $executableFull) -eq 0x8664) `
    'Installed executable machine is not AMD64 (0x8664).'
Assert-Stage5PerformanceFileHash $executableFull $ExpectedExecutableSha256 `
    'Installed executable SHA-256' | Out-Null
$runtimeFull = Split-Path -Parent $executableFull
$launcherContract = Get-Stage5LauncherContract $runtimeFull $executableFull
$artifactBinding = Read-Stage5PerformanceArtifactSet $ArtifactSetManifestPath `
    $ExpectedArtifactSetSha256 $ExpectedSourceCommit $Title $executableFull `
    $ExpectedExecutableSha256
Assert-Stage5PerformanceLauncherBinding $artifactBinding $launcherContract $Title
$taskFull = [IO.Path]::GetFullPath($TaskRoot).TrimEnd('\', '/')
Assert-Stage5PerformanceCondition ($taskFull.StartsWith('H:\',
    [StringComparison]::OrdinalIgnoreCase)) `
    'TaskRoot must be an explicit fresh task-owned H: path.'
Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $taskFull)) `
    'TaskRoot must not already exist; qualification requires a fresh root.'

$fixtureManifest = Read-Stage5ScalingFixtureManifest $FixtureManifestPath `
    $ExpectedFixtureManifestSha256 $Title $ExpectedExecutableSha256
$baseline = $null
if ($isExternalQualification) {
    $baseline = Read-Stage5ScalingBaseline $Stage3BaselinePath `
        $ExpectedStage3BaselineSha256 $ExpectedStage3ExecutableSha256 $Title `
        $ExpectedFixtureManifestSha256 $fixtureManifest.fixtures `
        $ExpectedStage3SourceCommit
}
$hostTopology = if ($isExternalQualification) {
    Get-Stage5HostTopology -MinimumPhysicalCores 16
} else {
    Get-Stage5HostTopology -MinimumPhysicalCores 4 -MaximumPhysicalCores 6 `
        -MaximumLogicalProcessors 12
}
if ($isExternalQualification) {
    Assert-Stage5PerformanceCondition ($baseline.physicalCoreCount -eq
        $hostTopology.physicalCoreCount -and $baseline.logicalProcessorCount -eq
        $hostTopology.logicalProcessorCount) `
        'Stage 3 baseline physical/logical topology does not match the qualification host.'
}
New-Item -ItemType Directory -Path $taskFull | Out-Null

    $context = [pscustomobject]@{
    schemaVersion = 1
    title = $Title
    qualificationMode = $QualificationMode
    sourceCommit = $ExpectedSourceCommit
    stage3SourceCommit = if ($isExternalQualification) {
        $ExpectedStage3SourceCommit
    } else { '' }
    artifactSetSha256 = $ExpectedArtifactSetSha256
    artifactSetManifestPath = $artifactBinding.path
    runtimeClosure = [pscustomobject]@{
        dependencyManifestSha256 = $artifactBinding.runtimeClosure.dependencyManifestSha256
        closureSha256 = $artifactBinding.runtimeClosure.closureSha256
    }
    cohortNonce = [Guid]::NewGuid().ToString()
    cohortCreatedUtc = [DateTime]::UtcNow.ToString('o')
    executablePath = $executableFull
    executableSha256 = $ExpectedExecutableSha256
        fixtureManifestSha256 = $ExpectedFixtureManifestSha256
        fixtureManifestPath = $fixtureManifest.path
        referencePolicy = $ReferencePolicy
        pairedOracleBindings = @()
        stage3BaselineSha256 = if ($isExternalQualification) {
        $ExpectedStage3BaselineSha256
    } else { '' }
    taskRoot = $taskFull
    warmupRuns = 1
    measuredRuns = $MeasuredRuns
    fixtures = $fixtureManifest.fixtures
    stage3Fixtures = if ($isExternalQualification) { $baseline.fixtures } else { @() }
    topology = $hostTopology
    processCleanup = [pscustomobject]@{
        processId = 0; exitProof = $true; blocked = $false; errors = @()
    }
    runs = @()
}

$laneNames = @(Get-Stage5LaneNames $QualificationMode)
$laneWorkers = @(Get-Stage5LaneWorkers $QualificationMode)
$titleSessionRoot = Join-Path $taskFull 'TitleSession'
$titleSession = $null
$registrySnapshots = New-Object 'Collections.Generic.List[object]'
$registrySnapshotKeys = @{}
$readOnlyLocks = $null
$profileBefore = $null
$profileAfter = $null
$aggregatePath = $null
$registryRecoveryPath = Join-Path $taskFull 'Stage5RegistryRecovery.json'
$primaryError = $null
$validationMutex = $null
$registryJournalObserver = {
    param($snapshots)
    Write-Stage5RegistryRecoveryJournal $registryRecoveryPath $Title $taskFull `
        $snapshots $context.processCleanup 'pending'
}
try {
    $titleSession = New-Stage5TitleSessionContract $Title $titleSessionRoot `
        $runtimeFull $taskFull
    $validationMutex = Acquire-Stage5ValidationMutex
    Assert-Stage5NoInstalledTitleProcesses
    Initialize-Stage5TitleSessionDirectories $titleSession
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
        [Microsoft.Win32.RegistryView]::Registry64)) {
        foreach ($registryValue in $titleSession.registryValues) {
            Set-Stage5RegistryValue $view $registryValue.subKey `
                $registryValue.name $registryValue.value $registrySnapshots `
                $registrySnapshotKeys $registryJournalObserver
        }
    }
    $profileBefore = Get-Stage5ProfileTreeHash $titleSession.profileRoot
    $readOnlyLocks = Open-Stage5PerformanceReadOnlyLocks `
        $artifactBinding $fixtureManifest.fixtures $fixtureManifest.path
    $scheduledRuns = New-Object 'Collections.Generic.List[object]'
    $scheduledOracleBindings = New-Object 'Collections.Generic.List[object]'
    foreach ($fixture in $fixtureManifest.fixtures) {
        for ($laneIndex = 0; $laneIndex -lt $laneNames.Count; ++$laneIndex) {
            for ($ordinal = 0; $ordinal -lt (1 + $MeasuredRuns); ++$ordinal) {
                Assert-Stage5PerformanceFixtureHash $fixture `
                    "Fixture '$($fixture.id)' before run"
                try {
                    $run = Invoke-Stage5InstalledPerformanceRun $context $fixture `
                        $laneNames[$laneIndex] $ordinal `
                        $laneWorkers[$laneIndex] $TimeoutSeconds $titleSession `
                        'throughput-binding'
                }
                finally {
                    Assert-Stage5PerformanceFixtureHash $fixture `
                        "Fixture '$($fixture.id)' after run"
                }
                $scheduledRuns.Add($run) | Out-Null
                if ($ReferencePolicy -ceq 'paired-serial-oracle-v1') {
                    Assert-Stage5PerformanceFixtureHash $fixture `
                        "Fixture '$($fixture.id)' before paired oracle run"
                    try {
                        $oracleRun = Invoke-Stage5InstalledPerformanceRun $context $fixture `
                            $laneNames[$laneIndex] $ordinal `
                            $laneWorkers[$laneIndex] $TimeoutSeconds $titleSession `
                            'serial-oracle'
                    }
                    finally {
                        Assert-Stage5PerformanceFixtureHash $fixture `
                            "Fixture '$($fixture.id)' after paired oracle run"
                    }
                    $scheduledOracleBindings.Add([pscustomobject]@{
                        throughputRunId = [string]$run.runId
                        oracleRun = $oracleRun
                    }) | Out-Null
                }
            }
        }
    }
    $context.runs = $scheduledRuns.ToArray()
    $context.pairedOracleBindings = $scheduledOracleBindings.ToArray()
    $validated = Assert-Stage5PerformanceRunSet $context
    $profileAfter = Assert-Stage5ProfileReadOnly $titleSession.profileRoot

    # This is the only aggregate write. Any missing/tampered receipt,
    # topology, command, exit, raw file, timing file, or threshold failure
    # leaves it absent.
    $stage3Summary = if ($isExternalQualification) {
        [ordered]@{
            path = $baseline.path
            sha256 = $ExpectedStage3BaselineSha256
            sourceCommit = $ExpectedStage3SourceCommit
            executableSha256 = $ExpectedStage3ExecutableSha256
        }
    } else { $null }
    $aggregate = [ordered]@{
        schemaVersion = 2
        evidenceKind = if ($isExternalQualification) {
            'stage5-performance-scaling-host-qualification'
        } else { 'stage5-performance-scaling-local-capacity-smoke' }
        producer = 'Invoke-Stage5PerformanceScalingValidation.ps1'
        status = 'passed'
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        qualificationMode = $QualificationMode
        qualificationClass = if ($isExternalQualification) {
            'external-16-core-qualification'
        } else { 'local-capacity-smoke' }
        measurementMode = 'headless-throughput'
        referencePolicy = $ReferencePolicy
        installedRuntime = $true
        sourceCommit = $ExpectedSourceCommit
        artifactSetSha256 = $ExpectedArtifactSetSha256
        artifactSetManifest = [ordered]@{
            path = $artifactBinding.path; sha256 = $artifactBinding.sha256
        }
        runtimeClosure = [ordered]@{
            dependencyManifestPath = $artifactBinding.runtimeClosure.dependencyManifestPath
            dependencyManifestSha256 = $artifactBinding.runtimeClosure.dependencyManifestSha256
            closureSha256 = $artifactBinding.runtimeClosure.closureSha256
            fileCount = $artifactBinding.runtimeClosure.fileCount
        }
        title = $Title
        executable = [ordered]@{ path = $executableFull; sha256 = $ExpectedExecutableSha256 }
        fixtureManifest = [ordered]@{
            path = $fixtureManifest.path; sha256 = $ExpectedFixtureManifestSha256
        }
        stage3Baseline = $stage3Summary
        launcher = $launcherContract
        profileStrategy = 'known-folder-registry-redirect'
        registryViews = @('Registry32', 'Registry64')
        environmentVariables = @($titleSession.environmentVariableNames)
        profileConcurrency = 'shared-title-profile-read-only'
        validationConcurrency = 'cooperative-global-user-sid-mutex-and-live-title-process-preflight'
        titleSessionProfile = [ordered]@{
            schemaVersion = 1; title = $Title
            sessionRoot = $titleSession.sessionRoot
            documentsRoot = $titleSession.documentsRoot
            profileLeaf = $titleSession.profileLeaf
            profileRoot = $titleSession.profileRoot
            profileHashBefore = $profileBefore.sha256
            profileHashAfter = $profileAfter.sha256
            profileFilesAfter = @($profileAfter.files)
            profileReadOnlyVerified = $true
            registryViews = @($titleSession.registryViews)
        }
        schedule = [ordered]@{ warmupRuns = 1; measuredRuns = $MeasuredRuns }
        topology = $hostTopology
        thresholds = if ($isExternalQualification) {
            [ordered]@{
                maximumForcedOneRegressionRatio = 1.05
                minimumPhysical8Speedup = 2.0
                minimumPhysical8To16SpeedupExclusive = 1.0
            }
        } else { $null }
        nativeReceiptBindings = @($validated.runs | ForEach-Object {
            $_.receiptBinding
        })
        pairedOracleBindings = @($validated.pairedOracleBindings)
        fixtures = $validated.fixtures
        runs = $validated.runs
    }
    $aggregateName = if ($isExternalQualification) {
        'Stage5PerformanceScalingQualification.json'
    } else { 'Stage5PerformanceLocalCapacitySmoke.json' }
    $aggregatePath = Join-Path $taskFull $aggregateName
    Write-Stage5JsonAtomically $aggregatePath $aggregate
}
catch {
    $primaryError = $_
}
finally {
    $cleanupErrors = New-Object 'Collections.Generic.List[string]'
    $childCleanupBlocked = $false
    $childProcessId = 0
    if ($null -ne $context.processCleanup -and
        [bool]$context.processCleanup.blocked) {
        $childCleanupBlocked = $true
        $childProcessId = [int]$context.processCleanup.processId
        $cleanupErrors.Add("Owned Stage 5 title child PID $childProcessId has no exit proof; registry/profile cleanup is deferred. Recovery journal: $registryRecoveryPath") | Out-Null
    }
    if ($null -ne $readOnlyLocks) {
        try { Dispose-Stage5PerformanceReadOnlyLocks $readOnlyLocks }
        catch { $cleanupErrors.Add("read-only lock cleanup: $($_.Exception.Message)") | Out-Null }
    }
    $registryRestored = $false
    if ($childCleanupBlocked) {
        try {
            # Keep a bounded recovery record even if the child failed before
            # its first registry mutation; it still identifies the live PID
            # and prevents a deferred cleanup from being mistaken for success.
            Write-Stage5RegistryRecoveryJournal $registryRecoveryPath $Title $taskFull `
                $registrySnapshots.ToArray() $context.processCleanup `
                'child-exit-unproven'
        }
        catch { $cleanupErrors.Add("recovery journal write: $($_.Exception.Message)") | Out-Null }
    }
    else {
        try {
            Restore-Stage5RegistrySnapshots @($registrySnapshots.ToArray())
            $registryRestored = $true
        }
        catch {
            $cleanupErrors.Add("registry restoration: $($_.Exception.Message)") | Out-Null
            if ($registrySnapshots.Count -gt 0) {
                try {
                    Write-Stage5RegistryRecoveryJournal $registryRecoveryPath $Title $taskFull `
                        $registrySnapshots.ToArray() $context.processCleanup `
                        'registry-restoration-failed'
                }
                catch { $cleanupErrors.Add("recovery journal write: $($_.Exception.Message)") | Out-Null }
            }
        }
    }
    if (-not $childCleanupBlocked -and $registryRestored -and $null -ne $titleSession) {
        try { Remove-Stage5TitleSessionDirectories $titleSession $taskFull }
        catch { $cleanupErrors.Add("title-session cleanup: $($_.Exception.Message)") | Out-Null }
    }
    elseif (-not $childCleanupBlocked -and $registryRestored -and $null -eq $titleSession) {
        # The title-session contract is the first operation after creating the
        # fresh task root.  If it cannot be constructed, remove only that exact
        # root, after rejecting a replacement/reparse point.
        if (Test-Path -LiteralPath $taskFull) {
            try {
                $taskRootItem = Get-Item -LiteralPath $taskFull -Force
                Assert-Stage5PerformanceCondition ($taskRootItem.PSIsContainer -and
                    ($taskRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
                    'Fresh Stage 5 task root was replaced before setup-failure cleanup.'
                Remove-Item -LiteralPath $taskFull -Recurse -Force
                Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $taskFull)) `
                    'Fresh Stage 5 task root remained after setup-failure cleanup.'
            }
            catch { $cleanupErrors.Add("fresh task-root cleanup: $($_.Exception.Message)") | Out-Null }
        }
    }
    if ($registryRestored -and -not $childCleanupBlocked -and
        (Test-Path -LiteralPath $registryRecoveryPath)) {
        try {
            Remove-Item -LiteralPath $registryRecoveryPath -Force
            Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $registryRecoveryPath)) `
                'Stage 5 registry recovery journal remained after successful restoration.'
        }
        catch { $cleanupErrors.Add("recovery journal cleanup: $($_.Exception.Message)") | Out-Null }
    }
    if (-not $childCleanupBlocked -and $registryRestored) {
        try { Release-Stage5ValidationMutex $validationMutex }
        catch { $cleanupErrors.Add("validation mutex cleanup: $($_.Exception.Message)") | Out-Null }
    }
    else {
        $cleanupErrors.Add('Stage 5 validation mutex ownership ends with this validator; child exit or registry restoration is unproven. Use the recovery journal to restore state before retrying.') | Out-Null
    }
    if ($null -ne $primaryError) {
        if ($cleanupErrors.Count -gt 0) {
            throw "Stage 5 operation failed: $($primaryError.Exception.Message); final cleanup also failed: $($cleanupErrors.ToArray() -join ' | ')"
        }
        throw $primaryError
    }
    if ($cleanupErrors.Count -gt 0) {
        throw "Stage 5 final cleanup failed: $($cleanupErrors.ToArray() -join ' | ')"
    }
}
Write-Output $aggregatePath
