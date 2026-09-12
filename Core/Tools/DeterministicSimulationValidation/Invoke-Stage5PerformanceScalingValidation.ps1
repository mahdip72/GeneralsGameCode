[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [ValidateSet('Generals', 'ZeroHour')]
    [string]$Title,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$InstalledExecutablePath,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExpectedExecutableSha256,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExpectedSourceCommit,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExpectedArtifactSetSha256,

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ArtifactSetManifestPath,

    [string]$GeneralsInstallRoot = '',
    [string]$GeneralsQualificationDataManifestPath = '',
    [string]$GeneralsQualificationDataManifestSha256 = '',

    [Parameter(ParameterSetName = 'Run', Mandatory = $true)]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
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
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$TaskRoot,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateRange(3, 100)]
    [int]$MeasuredRuns = 3,

    [Parameter(ParameterSetName = 'Run')]
    [Parameter(ParameterSetName = 'FixtureProduction')]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic')]
    [ValidateRange(1, 86400)]
    [int]$TimeoutSeconds = 7200,

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('External16Core', 'LocalCapacitySmoke',
        'InstalledKernelExecution')]
    [string]$QualificationMode = 'LocalCapacitySmoke',

    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('throughput-only', 'paired-serial-oracle-v1')]
    [string]$ReferencePolicy = 'throughput-only',

    [Parameter(ParameterSetName = 'Run')]
    [object[]]$PhaseBaselineProfiles = @(),

    [Parameter(ParameterSetName = 'Run')]
    [string]$PhaseBaselineProfilePath,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedPhaseBaselineProfileSha256,

    [Parameter(ParameterSetName = 'Run')]
    [string]$PerformanceDataManifestPath,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedPerformanceDataManifestSha256,

    [Parameter(ParameterSetName = 'Run')]
    [string]$ExpectedPerformanceDataClosureSha256,

    [Parameter(ParameterSetName = 'Run')]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExecutionCohortNonce,

    [Parameter(ParameterSetName = 'Run')]
    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExecutionCohortCreatedUtc,

    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [switch]$ProduceNativeFixture,

    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [string]$ReviewedFixtureManifestPath,

    [Parameter(ParameterSetName = 'FixtureProduction', Mandatory = $true)]
    [string]$ExpectedReviewedFixtureManifestSha256,

    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [switch]$RunNativeFixtureDiagnostic,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$DiagnosticMapPath,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$ExpectedDiagnosticMapSha256,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [string]$DiagnosticMapKey,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic', Mandatory = $true)]
    [ValidateRange(1, 1000000)][int]$DiagnosticExpectedInitialUnits,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic')]
    [ValidateRange(1, 2147483647)][int]$DiagnosticSeed = 1729,
    [Parameter(ParameterSetName = 'NativeFixtureDiagnostic')]
    [ValidateRange(1, 108000)][int]$DiagnosticFrameBudget = 108000,

    # This parameter set exists only for host-side contract tests. It consumes
    # already-created synthetic receipts and can never reach Process.Start().
    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)]
    [string]$SelfTestValidationManifestPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1')
Import-Module (Join-Path $PSScriptRoot `
    'Stage5NativePerformanceFixtureProduction.psm1')
Import-Module (Join-Path $PSScriptRoot `
    'Stage5ValidationProfileCapability.psm1')
Import-Module (Join-Path $PSScriptRoot `
    'Stage5RegistryRecovery.psm1')
Import-Module (Join-Path $PSScriptRoot 'Stage5BaseGeneralsBinding.psm1')

$script:Stage5RunnerScriptSha256 = $null
try {
    $runnerBytes = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PSCommandPath))
    $runnerAlgorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $script:Stage5RunnerScriptSha256 = ([BitConverter]::ToString(
            $runnerAlgorithm.ComputeHash($runnerBytes)) -replace '-', '').ToUpperInvariant()
    }
    finally { $runnerAlgorithm.Dispose() }
}
catch {
    throw "Could not establish the Stage 5 runner script identity: $($_.Exception.Message)"
}

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
$script:InstalledKernelLaneNames = @('physical-4')
$script:InstalledKernelLaneWorkers = @(4)
$script:WarmupRuns = 1
$script:VerifierBoundary = 'stage5-host-independent-correlation-v1'
$script:Stage5FatalPattern = '(?i)(CRC Mismatch|game thread ownership violation|assertion failed|fatal error|missing map|replay read error|SKIRMISH_AI_TEST_FAIL|SIMULATION_JOB_SYSTEM_FALLBACK|SIMULATION_SHADOW_(?:MISMATCH|FAIL)|SIMULATION_COLLISION_MISMATCH)'
$script:Stage5ValidationMutexName = $null
$script:Stage5ActiveRegistryRecovery = $null
$script:Stage5CurrentProcessStarted = $false
$script:Stage5CurrentProcessIdentity = $null
try {
    # Registry/profile redirection is user-scoped, so serialize cooperative
    # validators across the user's sessions without contending with another
    # Windows account.  Fail closed if the identity cannot be resolved.
    $validationUserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if ([string]::IsNullOrWhiteSpace([string]$validationUserSid)) {
        throw 'the current Windows user SID was unavailable'
    }
    $script:Stage5ValidationMutexName =
        Get-Stage5RegistryRecoveryMutexName $validationUserSid
}
catch {
    throw "Could not establish the Stage 5 installed-validation mutex identity: $($_.Exception.Message)"
}

function Get-Stage5LaneNames {
    param([string]$Mode)
    if ($Mode -ceq 'LocalCapacitySmoke') { return $script:LocalLaneNames }
    if ($Mode -ceq 'InstalledKernelExecution') {
        return $script:InstalledKernelLaneNames
    }
    return $script:ExternalLaneNames
}

function Get-Stage5LaneWorkers {
    param([string]$Mode)
    if ($Mode -ceq 'LocalCapacitySmoke') { return $script:LocalLaneWorkers }
    if ($Mode -ceq 'InstalledKernelExecution') {
        return $script:InstalledKernelLaneWorkers
    }
    return $script:ExternalLaneWorkers
}

function Assert-Stage5PerformanceCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Resolve-Stage5PerformanceExecutionCohort {
    param(
        [string]$Mode,
        [string]$Nonce,
        [string]$CreatedUtc,
        [bool]$NonceSupplied,
        [bool]$CreatedUtcSupplied
    )
    Assert-Stage5PerformanceCondition (
        @('External16Core', 'LocalCapacitySmoke',
            'InstalledKernelExecution') -ccontains $Mode) `
        'Execution cohort qualification mode is invalid.'
    if ($Mode -ceq 'LocalCapacitySmoke') {
        Assert-Stage5PerformanceCondition (
            -not $NonceSupplied -and -not $CreatedUtcSupplied) `
            'LocalCapacitySmoke cannot accept an external execution cohort.'
        return [pscustomobject]@{
            nonce = [Guid]::NewGuid().ToString('D')
            createdUtc = [DateTime]::UtcNow.ToString(
                'o', [Globalization.CultureInfo]::InvariantCulture)
            externallySupplied = $false
        }
    }

    Assert-Stage5PerformanceCondition (
        $NonceSupplied -and $CreatedUtcSupplied) `
        "$Mode requires ExecutionCohortNonce and ExecutionCohortCreatedUtc."
    [Guid]$parsedNonce = [Guid]::Empty
    $nonceValid = -not [string]::IsNullOrWhiteSpace($Nonce) -and
        [Guid]::TryParseExact($Nonce, 'D', [ref]$parsedNonce) -and
        $parsedNonce.ToString('D') -ceq $Nonce -and
        $Nonce -cmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
    Assert-Stage5PerformanceCondition $nonceValid `
        'ExecutionCohortNonce must be a canonical lowercase version-4 UUID.'

    [DateTime]$parsedCreatedUtc = [DateTime]::MinValue
    $createdUtcValid = -not [string]::IsNullOrWhiteSpace($CreatedUtc) -and
        [DateTime]::TryParseExact($CreatedUtc, 'o',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind,
            [ref]$parsedCreatedUtc) -and
        $parsedCreatedUtc.Kind -eq [DateTimeKind]::Utc -and
        $parsedCreatedUtc.ToString('o',
            [Globalization.CultureInfo]::InvariantCulture) -ceq $CreatedUtc
    Assert-Stage5PerformanceCondition $createdUtcValid `
        'ExecutionCohortCreatedUtc must be a canonical UTC round-trip timestamp.'
    return [pscustomobject]@{
        nonce = $Nonce
        createdUtc = $CreatedUtc
        externallySupplied = $true
    }
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
        return [pscustomobject]@{
            name = $script:Stage5ValidationMutexName
            mutex = $mutex; acquired = $true
        }
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
    if (-not (Test-Stage5RegistryScopeInactive -ExecutablePaths @())) {
        throw 'Cannot swap the title-session registry contract while either installed title is active or its state is unreadable.'
    }
}

function Get-Stage5PerformanceBaseBinding {
    param([string]$Title, [string]$RuntimeRoot, [object]$ArtifactBinding,
        [string]$RequestedRoot = '', [bool]$RequireQualification = $false,
        [string]$QualificationManifestPath = '', [string]$QualificationManifestSha256 = '')
    if ($Title -ceq 'Generals') { return $null }
    $roleRoot = Split-Path -Parent $ArtifactBinding.artifacts['generals-executable'].path
    $baseRoot = if ([string]::IsNullOrWhiteSpace($RequestedRoot)) { $roleRoot } else { [IO.Path]::GetFullPath($RequestedRoot).TrimEnd('\') }
    Assert-Stage5PerformanceCondition ([string]::Equals($baseRoot, $roleRoot,
        [StringComparison]::OrdinalIgnoreCase)) 'Performance base root differs from the bound Generals artifact role.'
    $arguments = @{ RuntimeRoot=$RuntimeRoot; GeneralsInstallRoot=$baseRoot }
    if ($RequireQualification) {
        $artifactDocument = ConvertFrom-Stage5JsonDictionary $ArtifactBinding.path
        $arguments.AcceptanceSourceCommit = $artifactDocument.sourceCommit
        $arguments.AcceptanceArtifactSetPath = $ArtifactBinding.path
        $arguments.AcceptanceArtifactSetSha256 = $ArtifactBinding.sha256
        $arguments.AcceptanceRuntimeDependencyManifestSha256 = $ArtifactBinding.runtimeClosure.dependencyManifestSha256
        $arguments.AcceptanceRuntimeClosureSha256 = $ArtifactBinding.runtimeClosure.closureSha256
        $arguments.GeneralsQualificationDataManifestPath = $QualificationManifestPath
        $arguments.GeneralsQualificationDataManifestSha256 = $QualificationManifestSha256
    }
    return Get-Stage5BaseGeneralsBinding @arguments
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

function Get-Stage5PerformanceTextSha256 {
    param([string]$Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) |
            ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $algorithm.Dispose() }
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
    $sourceCommitValue = $document['sourceCommit']
    $architectureValue = $document['architecture']
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $document['schemaVersion']) -and
        $document['schemaVersion'] -eq 1 -and
        $sourceCommitValue -is [string] -and
        $architectureValue -is [string] -and
        $sourceCommitValue -ceq $ExpectedSourceCommit -and
        $architectureValue -ceq 'x64') `
        'Reviewed artifact-set identity does not match the requested native x64 source revision.'
    $productSet = $document['productSet']
    Assert-Stage5PerformanceCondition ($productSet -is [Array] -and
        $productSet.Count -eq 2 -and
        $productSet[0] -is [string] -and
        $productSet[1] -is [string] -and
        @('Generals', 'ZeroHour') -ccontains $productSet[0] -and
        @('Generals', 'ZeroHour') -ccontains $productSet[1] -and
        $productSet[0] -cne $productSet[1]) `
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
        $roleValue = Get-Stage5JsonValue $entry 'role' 'Reviewed artifact entry'
        $relativeValue = Get-Stage5JsonValue $entry 'path' 'Reviewed artifact entry'
        $artifactHashValue = Get-Stage5JsonValue $entry 'sha256' `
            'Reviewed artifact entry'
        Assert-Stage5PerformanceCondition ($roleValue -is [string] -and
            $relativeValue -is [string] -and
            $artifactHashValue -is [string]) `
            'Reviewed artifact entry scalar fields must retain their JSON string types.'
        $role = $roleValue
        Assert-Stage5PerformanceCondition ($requiredRoles -ccontains $role -and
            -not $resolved.ContainsKey($role)) `
            "Reviewed artifact role is missing, duplicated, or unsupported: $role"
        $relative = $relativeValue
        $artifactPath = Resolve-Stage5PerformanceManifestFile $artifactDirectory `
            $relative "Reviewed artifact '$role'"
        $artifactHash = $artifactHashValue
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

function Read-Stage5PerformanceQualificationData {
    param(
        [string]$Path,
        [string]$ExpectedManifestSha256,
        [string]$ExpectedClosureSha256,
        [string]$ExpectedSourceCommit,
        [string]$ExpectedTitle,
        [string]$ExpectedRuntimeRoot,
        [object]$ArtifactBinding,
        [object]$Snapshot = $null,
        [switch]$SkipInstalledFileValidation
    )
    $full = [IO.Path]::GetFullPath($Path)
    Assert-Stage5PerformanceHash $ExpectedManifestSha256 `
        'ExpectedPerformanceDataManifestSha256'
    Assert-Stage5PerformanceHash $ExpectedClosureSha256 `
        'ExpectedPerformanceDataClosureSha256'
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 5 performance qualification-data manifest'
    }
    Assert-Stage5PerformanceCondition ($Snapshot.PSObject.Properties.Name -ccontains
            'path' -and
        [IO.Path]::GetFullPath([string]$Snapshot.path) -ceq $full) `
        'Stage 5 performance qualification-data snapshot is bound to another path.'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot `
        $ExpectedManifestSha256 `
        'Stage 5 performance qualification-data manifest' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 5 performance qualification-data manifest' -AsPsObject
    Assert-Stage5PerformanceProperties $document @('schemaVersion',
        'evidenceKind', 'producer', 'sourceCommit', 'title', 'archiveSource',
        'runtimeRoot', 'files', 'closureSha256') `
        'Stage 5 performance qualification-data manifest'
    Assert-Stage5PerformanceProperties $document.archiveSource `
        @('object', 'sha256') 'Stage 5 performance qualification-data archive source'
    $runtimeRoot = [IO.Path]::GetFullPath($ExpectedRuntimeRoot).TrimEnd('\', '/')
    $evidenceKindValue = $document.evidenceKind
    $producerValue = $document.producer
    $sourceCommitValue = $document.sourceCommit
    $titleValue = $document.title
    $archiveObjectValue = $document.archiveSource.object
    $archiveHashValue = $document.archiveSource.sha256
    $runtimeRootValue = $document.runtimeRoot
    $closureSha256Value = $document.closureSha256
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $document.schemaVersion) -and
        $document.schemaVersion -eq 1 -and
        $evidenceKindValue -is [string] -and
        $producerValue -is [string] -and
        $sourceCommitValue -is [string] -and
        $titleValue -is [string] -and
        $archiveObjectValue -is [string] -and
        $archiveHashValue -is [string] -and
        $runtimeRootValue -is [string] -and
        $closureSha256Value -is [string] -and
        $evidenceKindValue -ceq 'stage5-performance-qualification-data' -and
        $producerValue -ceq 'genci-r2-trimmed-data-v1' -and
        $sourceCommitValue -ceq $ExpectedSourceCommit -and
        $sourceCommitValue -cmatch '^[0-9a-f]{40}$' -and
        $titleValue -ceq $ExpectedTitle -and $ExpectedTitle -ceq 'ZeroHour' -and
        $archiveObjectValue -ceq
            's3://github-ci/zerohour104_gamedata_trimmed.7z' -and
        $archiveHashValue -ceq
            '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21' -and
        [String]::Equals([IO.Path]::GetFullPath($runtimeRootValue).TrimEnd('\', '/'),
            $runtimeRoot, [StringComparison]::OrdinalIgnoreCase) -and
        $closureSha256Value -ceq $ExpectedClosureSha256) `
        'Stage 5 performance qualification-data identity or archive provenance is invalid.'

    $entries = @($document.files)
    Assert-Stage5PerformanceCondition ($document.files -is [Array] -and
        $entries.Count -ge 6) `
        'Stage 5 performance qualification-data manifest has incomplete file coverage.'
    $required = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($requiredPath in @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')) {
        [void]$required.Add($requiredPath)
    }
    $declared = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $filePaths = New-Object 'Collections.Generic.List[string]'
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $previousPath = $null
    foreach ($entry in $entries) {
        Assert-Stage5PerformanceProperties $entry @('path', 'sha256', 'length') `
            'Stage 5 performance qualification-data file'
        $relativeValue = $entry.path
        $hashValue = $entry.sha256
        $lengthValue = $entry.length
        $lengthValid = Test-Stage5JsonInteger $lengthValue
        if ($lengthValid) {
            $lengthValid = $lengthValue -gt 0 -and
                [decimal]$lengthValue -le [decimal][Int64]::MaxValue
        }
        Assert-Stage5PerformanceCondition ($relativeValue -is [string] -and
            $hashValue -is [string] -and $lengthValid) `
            'Stage 5 performance qualification-data file scalar fields must retain their JSON types.'
        $relative = $relativeValue
        $hash = $hashValue
        $isRootBig = $relative.IndexOf('/') -lt 0 -and
            $relative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $relative.StartsWith('Data/',
            [StringComparison]::OrdinalIgnoreCase)
        Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace($relative) -and
            $relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*$' -and
            $relative -cnotmatch '(^|/)\.\.?(/|$)' -and
            ($isRootBig -or $isDataFile) -and
            $hash -cmatch '^[0-9A-F]{64}$' -and
            ($null -eq $previousPath -or
                [StringComparer]::Ordinal.Compare($previousPath, $relative) -lt 0) -and
            $declared.Add($relative)) `
            "Stage 5 performance qualification-data path is unsafe, duplicated, or unsorted: $relative"
        $length = [Int64]$lengthValue
        [void]$required.Remove($relative)
        $canonicalLines.Add(('{0}|{1}|{2}' -f $relative, $hash, $length)) |
            Out-Null
        $previousPath = $relative
        if (-not $SkipInstalledFileValidation) {
            $candidate = [IO.Path]::GetFullPath((Join-Path $runtimeRoot $relative))
            Assert-Stage5PerformanceCondition ($candidate.StartsWith(
                    $runtimeRoot + [IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase) -and
                (Test-Path -LiteralPath $candidate -PathType Leaf)) `
                "Stage 5 performance qualification-data file is absent: $relative"
            Assert-Stage5FinalAcceptanceNoReparsePath $runtimeRoot $candidate `
                "Stage 5 performance qualification-data file '$relative'"
            $item = Get-Item -LiteralPath $candidate -Force
            Assert-Stage5PerformanceCondition ([Int64]$item.Length -eq $length -and
                (Get-Stage5PerformanceSha256 $candidate) -ceq $hash) `
                "Stage 5 performance qualification-data file changed: $relative"
            $filePaths.Add($candidate) | Out-Null
        }
    }
    Assert-Stage5PerformanceCondition ($required.Count -eq 0) `
        'Stage 5 performance qualification-data omits a required Zero Hour file.'
    $canonicalText = ($canonicalLines.ToArray() -join "`n") + "`n"
    $computedClosure = Get-Stage5PerformanceTextSha256 $canonicalText
    Assert-Stage5PerformanceCondition ($computedClosure -ceq
            $closureSha256Value -and
        $computedClosure -ceq $ExpectedClosureSha256) `
        'Stage 5 performance qualification-data closure SHA-256 is stale or substituted.'

    if (-not $SkipInstalledFileValidation) {
        Assert-Stage5PerformanceCondition ($null -ne $ArtifactBinding) `
            'Installed qualification-data validation requires the reviewed artifact binding.'
        $productFiles = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        foreach ($productPath in @($ArtifactBinding.runtimeClosure.filePaths) +
                @($ArtifactBinding.artifacts.Values | ForEach-Object { $_.path })) {
            $productFull = [IO.Path]::GetFullPath([string]$productPath)
            if ($productFull.StartsWith($runtimeRoot + '\',
                    [StringComparison]::OrdinalIgnoreCase)) {
                [void]$productFiles.Add($productFull)
            }
        }
        $actualExtras = @()
        foreach ($item in @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Force)) {
            Assert-Stage5FinalAcceptanceNoReparsePath $runtimeRoot $item.FullName `
                'Stage 5 performance installed runtime item'
            if (-not $item.PSIsContainer) {
                $itemFull = [IO.Path]::GetFullPath($item.FullName)
                if (-not $productFiles.Contains($itemFull)) { $actualExtras += $itemFull }
            }
        }
        Assert-Stage5PerformanceCondition ($actualExtras.Count -eq $filePaths.Count) `
            'Installed performance runtime has missing or undeclared qualification data.'
        foreach ($extra in $actualExtras) {
            Assert-Stage5PerformanceCondition ($filePaths.Contains($extra)) `
                "Installed performance runtime has undeclared qualification data: $extra"
        }
    }
    return [pscustomobject]@{
        path = $full
        snapshot = $Snapshot
        manifestSha256 = $ExpectedManifestSha256
        closureSha256 = $computedClosure
        runtimeRoot = $runtimeRoot
        fileCount = $entries.Count
        filePaths = @($filePaths.ToArray())
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
    if (-not (Test-Stage5JsonNumber $Value)) { return $false }
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
    $evidenceKindValue = $document.evidenceKind
    $titleValue = $document.title
    $executableHashValue = $document.executableSha256
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $document.schemaVersion) -and
        $document.schemaVersion -eq 1 -and
        $evidenceKindValue -is [string] -and
        $titleValue -is [string] -and
        $executableHashValue -is [string] -and
        $evidenceKindValue -ceq 'stage5-performance-scaling-fixtures' -and
        $titleValue -ceq $ExpectedTitle -and
        $executableHashValue -ceq $ExecutableHash) `
        'Reviewed fixture manifest identity does not match the exact candidate.'
    $fixtures = @($document.fixtures)
    Assert-Stage5PerformanceCondition ($document.fixtures -is [Array] -and
        $fixtures.Count -eq 4) `
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
        $idValue = $fixture.id
        $sourceValue = $fixture.source
        $hashValue = $fixture.sha256
        $seedRawValue = $fixture.seed
        $playerCountValue = $fixture.playerCount
        $peakUnitCountValue = $fixture.peakUnitCount
        $seedIsInteger = Test-Stage5JsonInteger $seedRawValue
        $seedValid = $false
        if ($seedIsInteger) {
            $seedValid = [UInt32]::TryParse([string]$seedRawValue,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$seedValue)
        }
        $countsAreExact = (Test-Stage5JsonInteger $playerCountValue) -and
            (Test-Stage5JsonInteger $peakUnitCountValue)
        $countsInIntRange = $false
        if ($countsAreExact) {
            $countsInIntRange = [decimal]$playerCountValue -ge [decimal][int]::MinValue -and
                [decimal]$playerCountValue -le [decimal][int]::MaxValue -and
                [decimal]$peakUnitCountValue -ge [decimal][int]::MinValue -and
                [decimal]$peakUnitCountValue -le [decimal][int]::MaxValue
        }
        Assert-Stage5PerformanceCondition ($idValue -is [string] -and
            $sourceValue -is [string] -and $hashValue -is [string] -and
            $seedIsInteger -and $seedValid -and $countsAreExact -and
            $countsInIntRange) `
            "$context scalar and integer fields must retain their exact JSON types."
        $unitIdentityValid = if ($index -eq 3) {
            $peakUnitCountValue -ge $expectedUnits
        } else { $peakUnitCountValue -eq $expectedUnits }
        Assert-Stage5PerformanceCondition ($idValue -ceq
            $script:CanonicalFixtureIds[$index] -and
            $hashValue -cmatch '^[0-9A-F]{64}$' -and
            $playerCountValue -eq 8 -and $unitIdentityValid) `
            "$context does not contain canonical 1000/4000/8000/dense8 metadata."
        $fixturePath = Resolve-Stage5PerformanceManifestFile $manifestDirectory `
            $sourceValue "$context source"
        Assert-Stage5PerformanceFileHash $fixturePath $hashValue `
            "$context SHA-256" | Out-Null
        [int]$playerCount = $playerCountValue
        [int]$peakUnitCount = $peakUnitCountValue
        $result += [pscustomobject]@{
            id = $idValue
            path = $fixturePath
            sha256 = $hashValue
            seed = $seedValue
            playerCount = $playerCount
            peakUnitCount = $peakUnitCount
        }
    }
    return [pscustomobject]@{ path = $full; sha256 = $ExpectedHash; fixtures = $result }
}

function Read-Stage5ScalingBaseline {
    param([string]$Path, [string]$ExpectedHash, [string]$ExpectedExecutableHash,
        [string]$ExpectedTitle, [string]$FixtureManifestHash, [object[]]$Fixtures,
        [string]$ExpectedSourceCommit, [object]$Snapshot = $null)
    $full = [IO.Path]::GetFullPath($Path)
    if ($null -eq $Snapshot) {
        $Snapshot = Get-Stage5FinalAcceptanceFileSnapshot $full `
            'Stage 3 performance baseline'
    }
    Assert-Stage5FinalAcceptanceSnapshotSha256 $Snapshot $ExpectedHash `
        'Stage 3 baseline SHA-256' | Out-Null
    $document = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $Snapshot `
        'Stage 3 performance baseline' -AsPsObject
    Assert-Stage5PerformanceProperties $document @('schemaVersion', 'stage',
        'architecture', 'title', 'executableSha256', 'fixtureManifestSha256',
        'configuration', 'physicalCoreCount', 'availableCpus',
        'logicalProcessorCount', 'warmupRuns', 'fixtures') `
        'Stage 3 performance baseline'
    $stageValue = $document.stage
    $architectureValue = $document.architecture
    $titleValue = $document.title
    $executableHashValue = $document.executableSha256
    $fixtureManifestHashValue = $document.fixtureManifestSha256
    $configurationValue = $document.configuration
    $physicalCoreCountValue = $document.physicalCoreCount
    $availableCpusValue = $document.availableCpus
    $logicalProcessorCountValue = $document.logicalProcessorCount
    $warmupRunsValue = $document.warmupRuns
    $topIntegersValid = (Test-Stage5JsonInteger $document.schemaVersion) -and
        (Test-Stage5JsonInteger $physicalCoreCountValue) -and
        (Test-Stage5JsonInteger $availableCpusValue) -and
        (Test-Stage5JsonInteger $logicalProcessorCountValue) -and
        (Test-Stage5JsonInteger $warmupRunsValue)
    $topIntegersInIntRange = $false
    if ($topIntegersValid) {
        $topIntegersInIntRange = [decimal]$physicalCoreCountValue -ge [decimal][int]::MinValue -and
            [decimal]$physicalCoreCountValue -le [decimal][int]::MaxValue -and
            [decimal]$availableCpusValue -ge [decimal][int]::MinValue -and
            [decimal]$availableCpusValue -le [decimal][int]::MaxValue -and
            [decimal]$logicalProcessorCountValue -ge [decimal][int]::MinValue -and
            [decimal]$logicalProcessorCountValue -le [decimal][int]::MaxValue -and
            [decimal]$warmupRunsValue -ge [decimal][int]::MinValue -and
            [decimal]$warmupRunsValue -le [decimal][int]::MaxValue
    }
    Assert-Stage5PerformanceCondition ($topIntegersValid -and
        $topIntegersInIntRange -and
        (Test-Stage5JsonInteger $document.schemaVersion) -and
        $stageValue -is [string] -and $architectureValue -is [string] -and
        $titleValue -is [string] -and $executableHashValue -is [string] -and
        $fixtureManifestHashValue -is [string] -and
        $configurationValue -is [string] -and
        $document.schemaVersion -eq 1 -and
        $stageValue -ceq 'Stage3' -and $architectureValue -ceq 'x64' -and
        $titleValue -ceq $ExpectedTitle -and
        $executableHashValue -ceq $ExpectedExecutableHash -and
        $fixtureManifestHashValue -ceq $FixtureManifestHash -and
        $configurationValue -ceq 'parallel-1' -and
        $availableCpusValue -ge $physicalCoreCountValue -and
        $warmupRunsValue -eq 1) `
        'Stage 3 baseline identity or exact hash binding is invalid.'
    Assert-Stage5PerformanceCondition ($physicalCoreCountValue -ge 16 -and
        $logicalProcessorCountValue -ge 16) `
        'Stage 3 baseline lacks the required 16-physical-core topology.'
    $baselineFixtures = @($document.fixtures)
    Assert-Stage5PerformanceCondition ($document.fixtures -is [Array] -and
        $baselineFixtures.Count -eq 4) `
        'Stage 3 baseline requires all four canonical fixtures.'
    $result = @()
    for ($index = 0; $index -lt 4; ++$index) {
        $fixture = $baselineFixtures[$index]
        $context = "Stage 3 baseline fixture $index"
        Assert-Stage5PerformanceProperties $fixture @('id', 'fixtureSha256',
            'playerCount', 'peakUnitCount', 'wallMilliseconds') $context
        $idValue = $fixture.id
        $fixtureHashValue = $fixture.fixtureSha256
        $playerCountValue = $fixture.playerCount
        $peakUnitCountValue = $fixture.peakUnitCount
        $wallValues = @($fixture.wallMilliseconds)
        $playerCountValid = Test-Stage5JsonInteger $playerCountValue
        $peakUnitCountValid = Test-Stage5JsonInteger $peakUnitCountValue
        $countsInIntRange = $false
        if ($playerCountValid -and $peakUnitCountValid) {
            $countsInIntRange = [decimal]$playerCountValue -ge [decimal][int]::MinValue -and
                [decimal]$playerCountValue -le [decimal][int]::MaxValue -and
                [decimal]$peakUnitCountValue -ge [decimal][int]::MinValue -and
                [decimal]$peakUnitCountValue -le [decimal][int]::MaxValue
        }
        $wallValuesValid = @($wallValues | Where-Object {
                -not (Test-Stage5JsonNumber $_)
            }).Count -eq 0
        Assert-Stage5PerformanceCondition ($fixture.wallMilliseconds -is [Array] -and
            $idValue -is [string] -and $fixtureHashValue -is [string] -and
            $playerCountValid -and $peakUnitCountValid -and
            $countsInIntRange -and $wallValuesValid) `
            "$context scalar and numeric fields must retain exact JSON types."
        [int]$playerCount = $playerCountValue
        [int]$peakUnitCount = $peakUnitCountValue
        $samples = @($wallValues | ForEach-Object { [double]$_ })
        Assert-Stage5PerformanceCondition ($idValue -ceq $Fixtures[$index].id -and
            $fixtureHashValue -ceq $Fixtures[$index].sha256 -and
            $playerCount -eq 8 -and
            $peakUnitCount -eq $Fixtures[$index].peakUnitCount -and
            $samples.Count -ge 4 -and
            @($samples | Where-Object {
                -not (Test-Stage5PerformanceFinitePositive $_)
            }).Count -eq 0) `
            "$context metadata or warmup/measured samples are invalid."
        $result += [pscustomobject]@{
            id = $idValue
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
        physicalCoreCount = [int]$physicalCoreCountValue
        availableCpus = [int]$availableCpusValue
        logicalProcessorCount = [int]$logicalProcessorCountValue
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
    Assert-Stage5FinalAcceptanceNoReparsePath $root $full $Context
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

function Get-Stage5HeldInputFinalPath {
    param([IO.FileStream]$Stream)
    return Assert-Stage5FinalAcceptanceFileHandlePath $Stream '' `
        'Held immutable input'
}

function Test-Stage5HeldInputStreams {
    param([object[]]$Streams, [string[]]$RequiredPaths)
    try {
        if ($Streams.Count -eq 0 -or $Streams.Count -ne $RequiredPaths.Count) { return $false }
        $required = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($path in $RequiredPaths) {
            if ([string]::IsNullOrWhiteSpace($path) -or
                -not $required.Add([IO.Path]::GetFullPath($path))) { return $false }
        }
        $resolved = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $volumes = @{}
        foreach ($stream in $Streams) {
            if ($stream -isnot [IO.FileStream] -or -not $stream.CanRead -or $stream.CanWrite -or
                $stream.SafeFileHandle.IsClosed -or $stream.SafeFileHandle.IsInvalid) { return $false }
            # Query the original registered handle. Name and timestamps are not identity.
            $path = Get-Stage5HeldInputFinalPath $stream
            if (-not $required.Contains($path) -or -not $resolved.Add($path)) { return $false }
            $root = [IO.Path]::GetPathRoot($path)
            if (-not $volumes.ContainsKey($root)) {
                $volume = New-Object IO.DriveInfo $root
                if ($volume.DriveType -ne [IO.DriveType]::Fixed -or $volume.DriveFormat -cne 'NTFS') { return $false }
                $volumes[$root] = $true
            }
            Assert-Stage5FinalAcceptanceNoReparsePath $root $path 'Held immutable input'
        }
        return $resolved.SetEquals($required)
    }
    catch { return $false }
}

function Read-Stage5PlannedPerformanceInputs {
    param([object]$Context, [object]$PlanBinding, [object]$Plan)
    $registrations = @()
    if ($null -ne (Get-Variable -Name Stage5HeldInputRegistrations -Scope Script -ErrorAction SilentlyContinue)) {
        $registrations = @($script:Stage5HeldInputRegistrations)
    }
    $verifiedMatches = New-Object 'Collections.Generic.List[object]'
    foreach ($registration in $registrations) {
        $verified = $registration.verified
        if ($null -ne $verified -and $verified.planPath -ceq $PlanBinding.path -and
            $verified.planSha256 -ceq $PlanBinding.sha256 -and
            $verified.artifactManifestPath -ceq $Context.artifactSetManifestPath -and
            (Test-Stage5HeldInputStreams $registration.streams $verified.requiredPaths)) {
            $verifiedMatches.Add($registration) | Out-Null
        }
    }
    Assert-Stage5PerformanceCondition ($verifiedMatches.Count -le 1) `
        'Frozen prelaunch inputs have ambiguous retained immutable capabilities.'
    if ($verifiedMatches.Count -eq 1) {
        # Return a fresh projection; callers never receive the retained cache object.
        return ($verifiedMatches[0].verified.projectionJson | ConvertFrom-Json)
    }
    # No registered, exact, live handle-path capability: independently verify all inputs.
    $artifact = Read-Stage5PerformanceArtifactSet $Context.artifactSetManifestPath $Plan.artifactSetSha256 `
        $Plan.sourceCommit $Plan.title $Plan.executablePath $Plan.executableSha256
    $installedFixtureProduction = $null
    $reviewed = if ($Plan.qualificationMode -ceq
            'InstalledKernelExecution') {
        Assert-Stage5PerformanceCondition (
            $Context.PSObject.Properties.Name -ccontains
                'fixtureProductionReceipt' -and
            $Context.fixtureProductionReceipt.path -ceq
                $Plan.fixtureManifestPath -and
            $Context.fixtureProductionReceipt.sha256 -ceq
                $Plan.fixtureManifestSha256) `
            'Frozen installed-kernel plan is detached from fixture-production evidence.'
        $installedFixtureProduction =
            Read-Stage5NativePerformanceFixtureProductionReceipt `
                -Path $Plan.fixtureManifestPath `
                -ExpectedSha256 $Plan.fixtureManifestSha256 `
                -ExpectedTitle $Plan.title `
                -ExpectedCohortNonce $Plan.cohortNonce `
                -ExpectedCohortCreatedUtc $Plan.cohortCreatedUtc `
                -ExpectedSourceCommit $Plan.sourceCommit `
                -ExpectedArtifactSetSha256 $Plan.artifactSetSha256 `
                -ExpectedExecutableSha256 $Plan.executableSha256 `
                -ExpectedDependencyManifestSha256 `
                    $Plan.runtimeClosure.dependencyManifestSha256 `
                -ExpectedRuntimeClosureSha256 `
                    $Plan.runtimeClosure.closureSha256
        [pscustomobject]@{
            path = $installedFixtureProduction.path
            fixtures = @($installedFixtureProduction.fixture)
        }
    }
    else {
        Read-Stage5ScalingFixtureManifest $Plan.fixtureManifestPath `
            $Plan.fixtureManifestSha256 $Plan.title $Plan.executableSha256
    }
    $projection = [pscustomobject]@{
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 = $artifact.runtimeClosure.dependencyManifestSha256
            closureSha256 = $artifact.runtimeClosure.closureSha256
        }
        fixtures = @($reviewed.fixtures)
    }
    $hasPerformanceData = $Context.PSObject.Properties.Name -ccontains
        'performanceData'
    if ($hasPerformanceData) {
        $data = Read-Stage5PerformanceQualificationData `
            $Context.performanceData.sourceManifestPath `
            $Context.performanceData.sha256 `
            $Context.performanceData.closureSha256 $Plan.sourceCommit `
            $Plan.title (Split-Path -Parent $Plan.executablePath) $artifact
        Assert-Stage5PerformanceCondition ($data.fileCount -eq
                $Context.performanceData.fileCount -and
            $Context.performanceData.path -ceq
                [string]$Plan.performanceData.path -and
            $Context.performanceData.sha256 -ceq
                [string]$Plan.performanceData.sha256 -and
            $Context.performanceData.closureSha256 -ceq
                [string]$Plan.performanceData.closureSha256) `
            'Frozen prelaunch performance qualification-data binding changed.'
        $projection | Add-Member NoteProperty performanceData ([pscustomobject]@{
            path = [string]$Context.performanceData.path
            sha256 = [string]$data.manifestSha256
            closureSha256 = [string]$data.closureSha256
            fileCount = [int]$data.fileCount
        })
    }
    $required = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $required.Add([IO.Path]::GetFullPath($artifact.path)) | Out-Null
    $required.Add([IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $artifact.path) $artifact.runtimeClosure.dependencyManifestPath))) | Out-Null
    foreach ($path in @($artifact.runtimeClosure.filePaths)) { $required.Add([IO.Path]::GetFullPath($path)) | Out-Null }
    foreach ($entry in @($artifact.artifacts.Values)) { $required.Add([IO.Path]::GetFullPath($entry.path)) | Out-Null }
    $required.Add([IO.Path]::GetFullPath($reviewed.path)) | Out-Null
    foreach ($entry in @($reviewed.fixtures)) { $required.Add([IO.Path]::GetFullPath($entry.path)) | Out-Null }
    if ($null -ne $installedFixtureProduction) {
        foreach ($path in @($installedFixtureProduction.filePaths)) {
            $required.Add([IO.Path]::GetFullPath([string]$path)) | Out-Null
        }
    }
    if ($hasPerformanceData) {
        foreach ($path in @($Context.performanceData.sourceManifestPath,
                $Context.performanceData.path) +
                @($Context.performanceData.filePaths)) {
            $required.Add([IO.Path]::GetFullPath([string]$path)) | Out-Null
        }
    }
    [string[]]$requiredPaths = @($required)
    $matchingRegistrations = New-Object 'Collections.Generic.List[object]'
    foreach ($registration in $registrations) {
        if (Test-Stage5HeldInputStreams $registration.streams $requiredPaths) {
            $matchingRegistrations.Add($registration) | Out-Null
        }
    }
    Assert-Stage5PerformanceCondition ($matchingRegistrations.Count -eq 1) `
        'Frozen prelaunch inputs require exactly one live held immutable capability.'
    # Open owns these precise stream references. Verification happened while they
    # were held, and native resolved paths close the acquisition alias boundary.
    $matchingRegistrations[0].verified = [pscustomobject]@{
        planPath = [string]$PlanBinding.path; planSha256 = [string]$PlanBinding.sha256
        artifactManifestPath = [string]$Context.artifactSetManifestPath
        requiredPaths = @($requiredPaths)
        projectionJson = (ConvertTo-Json $projection -Depth 20 -Compress)
    }
    return $projection
}

function Open-Stage5PerformanceReadOnlyLocks {
    param([object]$ArtifactBinding, [object[]]$Fixtures,
        [string]$FixtureManifestPath = '',
        [string[]]$AdditionalPaths = @())
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
    foreach ($path in $AdditionalPaths) {
        $candidatePaths.Add([string]$path) | Out-Null
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
        if ($null -eq (Get-Variable -Name Stage5HeldInputRegistrations -Scope Script -ErrorAction SilentlyContinue)) {
            $script:Stage5HeldInputRegistrations = @()
        }
        # Register only our complete successful open operation, never caller metadata.
        $registration = [pscustomobject]@{
            streams = $locks.ToArray(); verified = $null
        }
        # Script/control-flow output can unwrap a single-element array. Always
        # normalize retained registration state before adding a capability.
        $script:Stage5HeldInputRegistrations =
            @($script:Stage5HeldInputRegistrations) + @($registration)
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
    if ($null -ne (Get-Variable -Name Stage5HeldInputRegistrations -Scope Script -ErrorAction SilentlyContinue)) {
        $retained = @()
        foreach ($registration in @($script:Stage5HeldInputRegistrations)) {
            $revoked = $false
            foreach ($owned in $registration.streams) {
                foreach ($stream in @($Locks)) {
                    if ([object]::ReferenceEquals($owned,$stream)) { $revoked = $true; break }
                }
                if ($revoked) { break }
            }
            if (-not $revoked) { $retained += $registration }
        }
        # Revoke before the first Dispose, including partial disposal and later failures.
        $script:Stage5HeldInputRegistrations = @($retained)
    }
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
        [string]$TaskRootPath, [string]$GeneralsRuntimeRoot = '')
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
    if ($Title -ceq 'ZeroHour' -and -not [string]::IsNullOrWhiteSpace($GeneralsRuntimeRoot)) {
        $registryValues.Add([pscustomobject]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = [IO.Path]::GetFullPath($GeneralsRuntimeRoot).TrimEnd('\') + '\'
            purpose = 'base-generals-runtime-binding'
        }) | Out-Null
    }
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
    $profileText = [string]$ProfileRoot
    Assert-Stage5PerformanceCondition (
        Test-Stage5SafeTitleSessionPath $profileText -AllowWhitespace) `
        "Stage 5 title profile root is unsafe or too long: $profileText"
    $root = [IO.Path]::GetFullPath($profileText).TrimEnd('\', '/')
    Assert-Stage5PerformanceCondition (
        Test-Path -LiteralPath $root -PathType Container) `
        "Stage 5 title profile root disappeared: $root"
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
            # Track each newly-created segment before disposing the handle so
            # any setup failure can roll it back locally; the strict recovery
            # journal was published before this setup began.
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
        [object[]]$WrittenSnapshots = $null)
    $snapshotKey = "$View|$SubKey|$Name"
    Assert-Stage5PerformanceCondition ($SnapshotKeys.Contains($snapshotKey)) `
        "Registry mutation '$snapshotKey' was not preceded by a published recovery snapshot."
    $publishedSnapshot = @($Snapshots | Where-Object {
        [string]$_.view -ceq [string]$View -and
        [string]$_.subKey -ceq $SubKey -and [string]$_.name -ceq $Name
    })
    Assert-Stage5PerformanceCondition ($publishedSnapshot.Count -eq 1) `
        "Registry mutation '$snapshotKey' does not have exactly one recovery snapshot."
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    try {
        $target = $base.OpenSubKey($SubKey, $true)
        $targetExisted = $null -ne $target
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
            if ($null -ne $WrittenSnapshots) {
                $exists = @($target.GetValueNames()) -contains $Name
                $currentValue = if ($exists) { $target.GetValue($Name, $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } else { $null }
                $currentKind = if ($exists) { $target.GetValueKind($Name) } else { $null }
                Assert-Stage5RegistryRecoveryWriteState -Snapshot $publishedSnapshot[0] `
                    -CurrentKeyExists $targetExisted `
                    -CurrentValue ([pscustomobject]@{ exists=$exists; value=$currentValue; kind=$currentKind }) `
                    -WrittenSnapshots $WrittenSnapshots
            }
            $target.SetValue($Name, $Value,
                [Microsoft.Win32.RegistryValueKind]::String)
        }
        finally { $target.Dispose() }
    }
    finally { $base.Dispose() }
}

function ConvertTo-Stage5RecoveryRegistryView {
    param([Microsoft.Win32.RegistryView]$View)
    if ($View -eq [Microsoft.Win32.RegistryView]::Registry32) {
        return 'Registry32'
    }
    if ($View -eq [Microsoft.Win32.RegistryView]::Registry64) {
        return 'Registry64'
    }
    throw "Unsupported Stage 5 registry view: $View"
}

function ConvertFrom-Stage5RecoveryRegistryView {
    param([string]$View)
    if ($View -ceq 'Registry32') {
        return [Microsoft.Win32.RegistryView]::Registry32
    }
    if ($View -ceq 'Registry64') {
        return [Microsoft.Win32.RegistryView]::Registry64
    }
    throw "Unsupported Stage 5 recovery registry view: $View"
}

function New-Stage5RegistryRecoveryAdapter {
    return [ordered]@{
        GetValue = {
            param([string]$View, [string]$SubKey, [string]$Name)
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser,
                (ConvertFrom-Stage5RecoveryRegistryView $View))
            try {
                $key = $base.OpenSubKey($SubKey, $false)
                if ($null -eq $key) {
                    return [pscustomobject]@{ exists = $false; value = $null; kind = $null }
                }
                try {
                    if (-not (@($key.GetValueNames()) -contains $Name)) {
                        return [pscustomobject]@{ exists = $false; value = $null; kind = $null }
                    }
                    return [pscustomobject]@{
                        exists = $true
                        value = $key.GetValue($Name, $null,
                            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                        kind = $key.GetValueKind($Name)
                    }
                }
                finally { $key.Dispose() }
            }
            finally { $base.Dispose() }
        }
        GetKey = {
            param([string]$View, [string]$SubKey)
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser,
                (ConvertFrom-Stage5RecoveryRegistryView $View))
            try {
                $key = $base.OpenSubKey($SubKey, $false)
                if ($null -eq $key) {
                    return [pscustomobject]@{
                        exists = $false; valueNames = @(); subKeyNames = @()
                    }
                }
                try {
                    return [pscustomobject]@{
                        exists = $true
                        valueNames = @($key.GetValueNames())
                        subKeyNames = @($key.GetSubKeyNames())
                    }
                }
                finally { $key.Dispose() }
            }
            finally { $base.Dispose() }
        }
        SetValue = {
            param([string]$View, [string]$SubKey, [string]$Name,
                [object]$Value, [object]$Kind)
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser,
                (ConvertFrom-Stage5RecoveryRegistryView $View))
            try {
                $key = $base.OpenSubKey($SubKey, $true)
                if ($null -eq $key) {
                    throw "Recovery target key was not found: $View|$SubKey"
                }
                try {
                    $key.SetValue($Name, $Value,
                        [Microsoft.Win32.RegistryValueKind]([int]$Kind))
                }
                finally { $key.Dispose() }
            }
            finally { $base.Dispose() }
        }
        DeleteValue = {
            param([string]$View, [string]$SubKey, [string]$Name)
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser,
                (ConvertFrom-Stage5RecoveryRegistryView $View))
            try {
                $key = $base.OpenSubKey($SubKey, $true)
                if ($null -eq $key) {
                    throw "Recovery target key was not found: $View|$SubKey"
                }
                try { $key.DeleteValue($Name, $false) }
                finally { $key.Dispose() }
            }
            finally { $base.Dispose() }
        }
        DeleteKey = {
            param([string]$View, [string]$SubKey)
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
                [Microsoft.Win32.RegistryHive]::CurrentUser,
                (ConvertFrom-Stage5RecoveryRegistryView $View))
            try {
                $separator = $SubKey.LastIndexOf('\')
                if ($separator -lt 0) {
                    $base.DeleteSubKey($SubKey, $false)
                    return
                }
                $parentPath = $SubKey.Substring(0, $separator)
                $leaf = $SubKey.Substring($separator + 1)
                $parent = $base.OpenSubKey($parentPath, $true)
                if ($null -eq $parent) {
                    throw "Recovery parent key was not found: $View|$parentPath"
                }
                try { $parent.DeleteSubKey($leaf, $false) }
                finally { $parent.Dispose() }
            }
            finally { $base.Dispose() }
        }
    }
}

function Get-Stage5RegistryInstallPathAncestors {
    param([string]$SubKey)
    $result = New-Object 'Collections.Generic.List[string]'
    $current = ''
    foreach ($segment in $SubKey.Split('\')) {
        $current = if ([string]::IsNullOrEmpty($current)) {
            $segment
        } else { $current + '\' + $segment }
        $result.Add($current) | Out-Null
    }
    return $result.ToArray()
}

function Get-Stage5RegistryRecoveryMissingSubKeys {
    param([object]$Adapter, [string]$View, [string]$SubKey)
    $planned = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @(Get-Stage5RegistryInstallPathAncestors $SubKey)) {
        $key = & $Adapter['GetKey'] $View $path
        if ($null -ne $key -and [bool]$key.exists) { continue }
        [void]$planned.Add("$View|$path")
    }
    return @($planned | Sort-Object)
}

function New-Stage5RegistryRecoveryMutationSnapshot {
    param(
        [object]$Recovery,
        [Microsoft.Win32.RegistryView]$View,
        [object]$RegistryValue
    )
    $viewName = ConvertTo-Stage5RecoveryRegistryView $View
    $subKey = [string]$RegistryValue.subKey
    $name = [string]$RegistryValue.name
    $key = & $Recovery.adapter['GetKey'] $viewName $subKey
    $value = & $Recovery.adapter['GetValue'] $viewName $subKey $name
    $hadKey = $null -ne $key -and [bool]$key.exists
    $hadValue = $null -ne $value -and [bool]$value.exists
    $createdSubKeys = @(if ($hadKey) {
        @()
    } else {
        @(Get-Stage5RegistryInstallPathAncestors $subKey | Where-Object {
            @($Recovery.plannedMissingSubKeys) -contains "$viewName|$_"
        })
    })
    Assert-Stage5PerformanceCondition ($hadKey -or $createdSubKeys.Count -gt 0) `
        "Registry recovery plan no longer covers missing key '$viewName|$subKey'."
    $snapshotTitle = if ($subKey -ceq 'Software\Electronic Arts\EA Games\Generals') { 'Generals' } else { $Recovery.identity.title }
    return (New-Stage5RegistryRecoverySnapshot -Title $snapshotTitle `
        -View $viewName -SubKey $subKey -Name $name `
        -HadKey $hadKey -HadValue $hadValue `
        -OldValue $(if ($hadValue) { $value.value } else { $null }) `
        -OldKind $(if ($hadValue) { $value.kind } else { $null }) `
        -ExpectedValue ([string]$RegistryValue.value) `
        -ExpectedKind ([Microsoft.Win32.RegistryValueKind]::String) `
        -CreatedSubKeys $createdSubKeys)
}

function ConvertFrom-Stage5RegistryRecoveryEncodedValue {
    param([object]$Encoded)
    if ($null -eq $Encoded) { return $null }
    switch ([string]$Encoded.type) {
        'String' { return [string]$Encoded.value }
        'ExpandString' { return [string]$Encoded.value }
        'MultiString' { return [string[]]@($Encoded.value | ForEach-Object { [string]$_ }) }
        'Binary' { return [Convert]::FromBase64String([string]$Encoded.value) }
        'DWord' { return [UInt32]::Parse([string]$Encoded.value, [Globalization.CultureInfo]::InvariantCulture) }
        'QWord' { return [Int64]::Parse([string]$Encoded.value, [Globalization.CultureInfo]::InvariantCulture) }
    }
    throw "Unsupported encoded registry recovery kind: $($Encoded.type)"
}

function Test-Stage5RegistryRecoveryRawValueEqual {
    param([object]$Left, [object]$Right, [object]$Kind)
    if ($Left -is [byte[]] -or $Right -is [byte[]]) {
        $leftBytes = [byte[]]$Left; $rightBytes = [byte[]]$Right
        if ($leftBytes.Length -ne $rightBytes.Length) { return $false }
        for ($index = 0; $index -lt $leftBytes.Length; ++$index) {
            if ($leftBytes[$index] -ne $rightBytes[$index]) { return $false }
        }
        return $true
    }
    if ($Left -is [Array] -or $Right -is [Array]) {
        $leftArray = @($Left); $rightArray = @($Right)
        if ($leftArray.Count -ne $rightArray.Count) { return $false }
        for ($index = 0; $index -lt $leftArray.Count; ++$index) {
            if ([string]$leftArray[$index] -cne [string]$rightArray[$index]) {
                return $false
            }
        }
        return $true
    }
    if ([int]$Kind -eq [int][Microsoft.Win32.RegistryValueKind]::DWord) {
        return [UInt32]$Left -eq [UInt32]$Right
    }
    if ([int]$Kind -eq [int][Microsoft.Win32.RegistryValueKind]::QWord) {
        return [Int64]$Left -eq [Int64]$Right
    }
    return [string]$Left -ceq [string]$Right
}

function Assert-Stage5RegistryRecoverySnapshotCurrent {
    param([object]$Recovery, [object]$Snapshot)
    $key = & $Recovery.adapter['GetKey'] $Snapshot.view $Snapshot.subKey
    $currentKeyExists = $null -ne $key -and [bool]$key.exists
    Assert-Stage5PerformanceCondition ($currentKeyExists -eq [bool]$Snapshot.hadKey) `
        "Registry target key changed after its recovery snapshot: $($Snapshot.view)|$($Snapshot.subKey)"
    if (-not [bool]$Snapshot.hadKey) { return }
    $current = & $Recovery.adapter['GetValue'] $Snapshot.view $Snapshot.subKey $Snapshot.name
    if ([bool]$Snapshot.hadValue) {
        $oldValue = ConvertFrom-Stage5RegistryRecoveryEncodedValue $Snapshot.oldValue
        Assert-Stage5PerformanceCondition ($null -ne $current -and
            [bool]$current.exists -and
            [int]$current.kind -eq [int]$Snapshot.oldKind -and
            (Test-Stage5RegistryRecoveryRawValueEqual $current.value $oldValue $Snapshot.oldKind)) `
            "Registry target value changed after its recovery snapshot: $($Snapshot.view)|$($Snapshot.subKey)|$($Snapshot.name)"
    }
    else {
        Assert-Stage5PerformanceCondition ($null -eq $current -or
            -not [bool]$current.exists) `
            "Registry target value appeared after its recovery snapshot: $($Snapshot.view)|$($Snapshot.subKey)|$($Snapshot.name)"
    }
}

function Update-Stage5RegistryRecoveryState {
    param(
        [object]$Recovery,
        [ValidateSet('planned', 'active', 'child-running',
            'child-exit-unproven', 'registry-restoration-failed', 'restored')]
        [string]$State,
        [bool]$ChildExitProof,
        [bool]$NoActiveTitleProcesses,
        [string]$FailureMessage = ''
    )
    $parameters = @{
        Path = [string]$Recovery.path
        ExpectedIdentity = $Recovery.identity
        State = $State
        PlannedMissingSubKeys = @($Recovery.plannedMissingSubKeys)
        Snapshots = @($Recovery.snapshots.ToArray())
        ChildExitProof = $ChildExitProof
        NoActiveTitleProcesses = $NoActiveTitleProcesses
        ProcessIdentities = @($Recovery.processIdentities.ToArray())
    }
    if (-not [string]::IsNullOrWhiteSpace($FailureMessage)) {
        $parameters.FailureMessage = $FailureMessage
    }
    Update-Stage5RegistryRecoveryJournal @parameters | Out-Null
}

function Add-Stage5RegistryRecoveryMutation {
    param(
        [object]$Recovery,
        [Microsoft.Win32.RegistryView]$View,
        [object]$RegistryValue
    )
    $viewName = ConvertTo-Stage5RecoveryRegistryView $View
    if ($Recovery.identity.Contains('registryScope')) {
        $snapshot = @($Recovery.snapshots | Where-Object {
            $_.view -ceq $viewName -and $_.subKey -ceq $RegistryValue.subKey -and $_.name -ceq $RegistryValue.name
        })
        Assert-Stage5PerformanceCondition ($snapshot.Count -eq 1) 'Paired mutation was not in the frozen registry plan.'
        $key = & $Recovery.adapter.GetKey $viewName $RegistryValue.subKey
        $value = & $Recovery.adapter.GetValue $viewName $RegistryValue.subKey $RegistryValue.name
        Assert-Stage5RegistryRecoveryWriteState -Snapshot $snapshot[0] `
            -CurrentKeyExists ($null -ne $key -and [bool]$key.exists) -CurrentValue $value `
            -WrittenSnapshots @($Recovery.writtenSnapshots.ToArray())
        Set-Stage5RegistryValue $View $RegistryValue.subKey $RegistryValue.name `
            $RegistryValue.value $Recovery.snapshots $Recovery.snapshotKeys `
            -WrittenSnapshots @($Recovery.writtenSnapshots.ToArray())
        $Recovery.writtenSnapshots.Add($snapshot[0]) | Out-Null
        Update-Stage5RegistryRecoveryState $Recovery active $true $true
        return
    }
    foreach ($planned in @(Get-Stage5RegistryRecoveryMissingSubKeys `
            $Recovery.adapter $viewName ([string]$RegistryValue.subKey))) {
        if (@($Recovery.plannedMissingSubKeys) -notcontains $planned) {
            $Recovery.plannedMissingSubKeys.Add($planned) | Out-Null
        }
    }
    $snapshot = New-Stage5RegistryRecoveryMutationSnapshot $Recovery $View `
        $RegistryValue
    Assert-Stage5RegistryRecoverySnapshotCurrent $Recovery $snapshot
    $snapshotKey = "$(ConvertTo-Stage5RecoveryRegistryView $View)|$($RegistryValue.subKey)|$($RegistryValue.name)"
    Assert-Stage5PerformanceCondition (-not $Recovery.snapshotKeys.Contains($snapshotKey)) `
        "Duplicate Stage 5 registry recovery snapshot: $snapshotKey"
    $Recovery.snapshots.Add($snapshot) | Out-Null
    $Recovery.snapshotKeys[$snapshotKey] = $true
    # Publish the exact raw snapshot before EnsureSubKey/CreateSubKey/SetValue.
    Update-Stage5RegistryRecoveryState $Recovery 'active' $false $true
    Set-Stage5RegistryValue $View $RegistryValue.subKey $RegistryValue.name `
        $RegistryValue.value $Recovery.snapshots $Recovery.snapshotKeys
}

function New-Stage5RegistryRecoveryContext {
    param(
        [string]$Title,
        [string]$TaskRootPath,
        [string]$JournalPath,
        [string]$ExecutionNonce,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$ExecutablePath,
        [string]$ExecutableSha256,
        [object[]]$RegistryValues = @()
    )
    $adapter = New-Stage5RegistryRecoveryAdapter
    $identity = [ordered]@{
        runNonce = $ExecutionNonce
        title = $Title
        taskRoot = $TaskRootPath
        journalPath = $JournalPath
        userSid = $validationUserSid
        mutexName = $script:Stage5ValidationMutexName
        identityMode = 'acceptance-bound'
        runnerScriptSha256 = $script:Stage5RunnerScriptSha256
        executableSha256 = $ExecutableSha256
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
    }
    $recovery = [pscustomobject]@{
        path = $JournalPath
        identity = $identity
        adapter = $adapter
        plannedMissingSubKeys = New-Object 'Collections.Generic.List[string]'
        snapshots = New-Object 'Collections.Generic.List[object]'
        snapshotKeys = @{}
        processIdentities = New-Object 'Collections.Generic.List[object]'
        currentProcessIdentityIndex = -1
        writtenSnapshots = New-Object 'Collections.Generic.List[object]'
    }
    $scope = 'SingleTitle'
    if ($Title -ceq 'ZeroHour' -and @($RegistryValues).Count -gt 0) {
        Assert-Stage5PerformanceCondition (@($RegistryValues).Count -eq 2) 'Paired performance registry scope must bind two titles.'
        $scope = 'ZeroHourWithGeneralsBase'
        $identity.registryScope = $scope
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
            foreach ($value in $RegistryValues) {
                foreach ($missing in @(Get-Stage5RegistryRecoveryMissingSubKeys $adapter ([string]$view) $value.subKey)) {
                    if (-not $recovery.plannedMissingSubKeys.Contains($missing)) { $recovery.plannedMissingSubKeys.Add($missing) | Out-Null }
                }
                $snapshot = New-Stage5RegistryRecoveryMutationSnapshot $recovery $view $value
                $recovery.snapshots.Add($snapshot) | Out-Null
                $recovery.snapshotKeys["$view|$($value.subKey)|$($value.name)"] = $true
            }
        }
    }
    $identity.snapshotPlanSha256 = Get-Stage5RegistryRecoverySnapshotPlanSha256 -Title $Title `
        -RegistryScope $scope -PlannedMissingSubKeys @($recovery.plannedMissingSubKeys.ToArray()) `
        -Snapshots @($recovery.snapshots.ToArray())
    New-Stage5RegistryRecoveryJournal -Path $JournalPath -Identity $identity `
        -PlannedMissingSubKeys @($recovery.plannedMissingSubKeys.ToArray()) `
        -Snapshots @($recovery.snapshots.ToArray()) -ProcessIdentities @() | Out-Null
    return $recovery
}

function Add-Stage5RegistryRecoveryPendingProcess {
    param([object]$Recovery, [string]$ExecutablePath, [string]$ExecutableSha256)
    $pending = [ordered]@{
        launchPending = $true
        processId = 0
        creationTimeUtc100ns = 0
        executablePath = $ExecutablePath
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
    }
    $Recovery.processIdentities.Add($pending) | Out-Null
    $Recovery.currentProcessIdentityIndex =
        $Recovery.processIdentities.Count - 1
    # A pending launch is deliberately not allowed to claim either exit or
    # no-active-title proof. This journal update precedes Process.Start().
    Update-Stage5RegistryRecoveryState $Recovery 'active' $false $false
}

function Set-Stage5RegistryRecoveryObservedProcess {
    param([object]$Recovery, [object]$ProcessIdentity)
    Assert-Stage5PerformanceCondition (
        [int]$Recovery.currentProcessIdentityIndex -ge 0 -and
        [int]$Recovery.currentProcessIdentityIndex -lt
            $Recovery.processIdentities.Count) `
        'Observed Stage 5 child identity has no pending journal entry.'
    $observed = [ordered]@{
        launchPending = $false
        processId = [int]$ProcessIdentity.processId
        creationTimeUtc100ns = [Int64]$ProcessIdentity.creationTimeUtc100ns
        executablePath = [string]$ProcessIdentity.executablePath
        executableSha256 = ([string]$ProcessIdentity.executableSha256).ToUpperInvariant()
    }
    $Recovery.processIdentities[$Recovery.currentProcessIdentityIndex] = $observed
}

function New-Stage5RegistryRecoveryAuthorization {
    param([object]$Recovery)
    Assert-Stage5PerformanceCondition (
        @($Recovery.processIdentities.ToArray() | Where-Object {
            [bool]$_.launchPending
        }).Count -eq 0) `
        'Cannot authorize registry recovery while a Stage 5 child launch is pending.'
    $processIdentities = @($Recovery.processIdentities.ToArray() |
        ForEach-Object {
            [ordered]@{
                launchPending = [bool]$_.launchPending
                processId = [int]$_.processId
                creationTimeUtc100ns = [Int64]$_.creationTimeUtc100ns
                executablePath = [string]$_.executablePath
                executableSha256 = [string]$_.executableSha256
                exitProven = $true
            }
        })
    return [ordered]@{
        childExitProven = $true
        noActiveTitleProcesses = $true
        processIdentities = $processIdentities
    }
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
    param([object]$Snapshot, [object]$Receipt, [string]$Context)
    Assert-Stage5PerformanceCondition ($null -ne $Snapshot -and
        $Snapshot.PSObject.Properties.Name -contains 'bytes' -and
        $null -ne $Snapshot.bytes) `
        "$Context raw diagnostic snapshot is incomplete."
    try {
        $encoding = New-Object Text.UTF8Encoding($false, $true)
        $text = $encoding.GetString([byte[]]$Snapshot.bytes)
    }
    catch { throw "$Context raw diagnostic is not strict UTF-8: $($_.Exception.Message)" }
    $fields = @{}
    foreach ($line in @($text -split '\r?\n')) {
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
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        ($Receipt.schemaVersion -eq 5 -or $Receipt.schemaVersion -eq 6)) `
        "$Context receipt schemaVersion is not an exact supported integer."
    $rawProducer = if ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        $Receipt.schemaVersion -eq 6) {
        'game-executable-performance-receipt-v6'
    } else { 'game-executable-performance-receipt-v5' }
    Assert-Stage5PerformanceCondition ($fields.producer -ceq $rawProducer -and
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
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        ($Receipt.schemaVersion -eq 5 -or $Receipt.schemaVersion -eq 6)) `
        "$Label receipt schemaVersion is not an exact supported integer."
    $isV6 = (Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        $Receipt.schemaVersion -eq 6
    $isBaseline = $ExpectedMeasurementRole -ceq 'phase-serial-baseline'
    if ($isBaseline) {
        Assert-Stage5PhaseAccountingContract $Receipt $Label | Out-Null
    }
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
    Assert-Stage5PerformanceUnsignedFields $Receipt.frameSimulation @(
        'totalNanoseconds', 'maximumNanoseconds', 'sampleCount') `
        "$Label frame simulation"
    foreach ($phase in $phases) {
        $phaseCounters = @('totalNanoseconds', 'maximumNanoseconds',
            'sampleCount', 'serialNanoseconds')
        $phaseBooleans = @('available', 'serialNanosecondsKnown')
        if ($isV6) {
            $phaseCounters += 'pureNanoseconds'
            $phaseBooleans += 'pureNanosecondsKnown'
        }
        Assert-Stage5PerformanceUnsignedFields $phase $phaseCounters "$Label phase metric"
        Assert-Stage5PerformanceBooleanFields $phase $phaseBooleans "$Label phase metric"
    }
    foreach ($kernel in $kernels) {
        Assert-Stage5PerformanceUnsignedFields $kernel @('submittedJobs',
            'completedJobs', 'physicalWorkerJobs', 'ownerHelpedJobs',
            'physicalWorkerMask', 'distinctPhysicalWorkers',
            'elapsedNanoseconds') "$Label kernel metric"
        Assert-Stage5PerformanceBooleanFields $kernel @('available',
            'physicalWorkerMaskComplete', 'elapsedNanosecondsKnown') `
            "$Label kernel metric"
    }
    Assert-Stage5PerformanceCondition ([Int64]$Receipt.frameSimulation.totalNanoseconds -gt 0 -and
        [Int64]$Receipt.frameSimulation.maximumNanoseconds -gt 0 -and
        [Int64]$Receipt.frameSimulation.maximumNanoseconds -le
            [Int64]$Receipt.frameSimulation.totalNanoseconds -and
        [Int64]$Receipt.frameSimulation.sampleCount -gt 0) `
        "$Label frame simulation timing is incomplete or inconsistent."
    [Int64]$phaseTotal = 0
    for ($index = 0; -not $isBaseline -and $index -lt $phaseNames.Count; ++$index) {
        $phase = $phases[$index]
        $phaseFields = @('name', 'available',
            'totalNanoseconds', 'maximumNanoseconds', 'sampleCount',
            'serialNanoseconds', 'serialNanosecondsKnown')
        if ($isV6) { $phaseFields += @('pureNanoseconds', 'pureNanosecondsKnown') }
        Assert-Stage5PerformanceProperties $phase $phaseFields "$Label phase metric"
        if ($isV6) {
            Assert-Stage5PerformanceCondition ($phase.serialNanosecondsKnown -is [bool] -and
                -not $phase.serialNanosecondsKnown -and $phase.serialNanoseconds -eq 0 -and
                $phase.pureNanosecondsKnown -is [bool] -and -not $phase.pureNanosecondsKnown -and
                $phase.pureNanoseconds -eq 0) "$Label ordinary V6 phases must remain unknown serial/pure."
        }
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
        if (-not $isV6 -and $Context.qualificationMode -ceq 'External16Core' -and
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
    $timingMode = if ($isBaseline) { 'owner-inline-baseline-observation' } else { 'owner-pipeline-observation' }
    $timingAttribution = if ($isBaseline) { 'owner-inline-baseline-exclusive-v1' } else { 'owner-stack-exclusive-v1' }
    Assert-Stage5PerformanceUnsignedFields $kernelTiming @('schemaVersion',
        'errors', 'generation') "$Label kernel timing"
    Assert-Stage5PerformanceBooleanFields $kernelTiming @('enabled', 'frozen',
        'complete', 'serialReferenceKnown') "$Label kernel timing"
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $kernelTiming.schemaVersion) -and
        $kernelTiming.schemaVersion -eq 1 -and
        $kernelTiming.mode -ceq $timingMode -and
        $kernelTiming.attribution -ceq $timingAttribution -and
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
        [UInt64]$subtype = Get-Stage5UnsignedCounter $stream.subtype `
            "$Label kernel timing stream subtype"
        Assert-Stage5PerformanceCondition ($subtype -le $maximumSubtype) `
            "$Label kernel timing stream subtype is unsupported."
        $streamKey = "$streamName`:$subtype"
        Assert-Stage5PerformanceCondition (-not $seenKernelStreams.ContainsKey($streamKey)) `
            "$Label kernel timing stream is duplicated."
        $seenKernelStreams[$streamKey] = $true
        [UInt64]$attempted = Get-Stage5UnsignedCounter $stream.attemptedBatches `
            "$Label kernel timing attemptedBatches"
        [UInt64]$admitted = Get-Stage5UnsignedCounter $stream.admittedBatches `
            "$Label kernel timing admittedBatches"
        [UInt64]$committed = Get-Stage5UnsignedCounter $stream.committedBatches `
            "$Label kernel timing committedBatches"
        [UInt64]$aborted = Get-Stage5UnsignedCounter $stream.abortedBatches `
            "$Label kernel timing abortedBatches"
        [UInt64]$firstFrame = Get-Stage5UnsignedCounter $stream.firstFrame `
            "$Label kernel timing firstFrame"
        [UInt64]$lastFrame = Get-Stage5UnsignedCounter $stream.lastFrame `
            "$Label kernel timing lastFrame"
        [UInt64]$active = Get-Stage5UnsignedCounter $stream.activePipelineNanoseconds `
            "$Label kernel timing activePipelineNanoseconds"
        [UInt64]$inclusive = Get-Stage5UnsignedCounter $stream.inclusiveBatchNanoseconds `
            "$Label kernel timing inclusiveBatchNanoseconds"
        [UInt64]$maximum = Get-Stage5UnsignedCounter $stream.maximumBatchNanoseconds `
            "$Label kernel timing maximumBatchNanoseconds"
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
            [UInt64]$stageNanoseconds = Get-Stage5UnsignedCounter `
                $stage.totalNanoseconds "$Label kernel timing stage totalNanoseconds"
            [UInt64]$stageSamples = Get-Stage5UnsignedCounter `
                $stage.sampleCount "$Label kernel timing stage sampleCount"
            $waitIsAbsent = $isBaseline -and $stageIndex -eq 2
            Assert-Stage5PerformanceCondition (($waitIsAbsent -and $stageNanoseconds -eq 0 -and $stageSamples -eq 0) -or
                (-not $waitIsAbsent -and ($stageNanoseconds -eq 0 -or
                    $stageSamples -gt 0) -and $stageSamples -ge $committed)) `
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
        if ($isBaseline) {
            foreach ($field in @('submittedJobs','completedJobs','physicalWorkerJobs',
                    'ownerHelpedJobs','physicalWorkerMask','distinctPhysicalWorkers','elapsedNanoseconds')) {
                $value = Get-Stage5UnsignedCounter $kernels[$index].$field "$Label baseline kernel $field"
                Assert-Stage5PerformanceCondition ($value -eq 0) "$Label baseline fabricates physical kernel execution."
            }
            Assert-Stage5PerformanceCondition ($kernels[$index].elapsedNanosecondsKnown -is [bool] -and
                -not $kernels[$index].elapsedNanosecondsKnown) "$Label baseline kernel timing has an unsupported origin."
            continue
        }
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
    }
    Assert-Stage5KernelReferenceContract $Receipt $Context $Label `
        $ExpectedMeasurementRole
}

function Get-Stage5UnsignedCounter {
    param([object]$Value, [string]$Context, [bool]$AllowZero = $true)
    Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $Value) `
        "$Context must be an exact JSON integer."
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

function Assert-Stage5RealMulticoreKernelEvidence {
    param([object]$Receipt, [string]$Label)
    $canonicalNames = @('physics', 'status', 'collision', 'ai-planning',
        'spatial', 'path')
    $streams = @($Receipt.kernelTiming.streams)
    $streamFamilies = @($streams | ForEach-Object { [string]$_.name } |
        Sort-Object -Unique)
    Assert-Stage5PerformanceCondition ($streams.Count -ge 6 -and
        $streams.Count -le 8 -and
        @($streams | Where-Object { [UInt64]$_.committedBatches -le 0 }).Count -eq 0 -and
        $streamFamilies.Count -eq $canonicalNames.Count -and
        @($canonicalNames | Where-Object { $streamFamilies -cnotcontains $_ }).Count -eq 0) `
        "$Label does not contain 6-8 committed unique timing streams covering all six kernel families."

    [UInt64]$effectiveWorkers = $Receipt.worker.effectiveCount
    [UInt64]$workerOrdinalMask = if ($effectiveWorkers -ge 64) {
        [UInt64]::MaxValue
    } else { ([UInt64]1 -shl [int]$effectiveWorkers) - 1 }
    foreach ($kernel in @($Receipt.kernels)) {
        [UInt64]$submitted = $kernel.submittedJobs
        [UInt64]$completed = $kernel.completedJobs
        [UInt64]$physical = $kernel.physicalWorkerJobs
        [UInt64]$ownerHelped = $kernel.ownerHelpedJobs
        [UInt64]$mask = $kernel.physicalWorkerMask
        [UInt64]$distinct = $kernel.distinctPhysicalWorkers
        [UInt64]$maskBitCount = Get-Stage5UInt64BitCount $mask
        Assert-Stage5PerformanceCondition ([bool]$kernel.available -and
            $submitted -gt 0 -and $completed -eq $submitted -and
            $physical -gt 0 -and
            [decimal]$physical + [decimal]$ownerHelped -eq [decimal]$completed -and
            $kernel.physicalWorkerMaskComplete -is [bool] -and
            [bool]$kernel.physicalWorkerMaskComplete -and $mask -ne 0 -and
            # Kernel masks are indexed by worker ordinal, not by the topology
            # core ordinals encoded by selectedWorkerPhysicalCoreMask.
            ($mask -band $workerOrdinalMask) -eq $mask -and
            $distinct -gt 0 -and $distinct -le $maskBitCount -and
            $maskBitCount -le $physical -and $distinct -le $physical -and
            $distinct -le $effectiveWorkers) `
            "$Label kernel '$($kernel.name)' lacks reconciled physical-worker counters/mask evidence."
    }
}

function Assert-Stage5PerformanceUnsignedFields {
    param([object]$Value, [string[]]$Names, [string]$Context)
    foreach ($name in $Names) {
        Get-Stage5UnsignedCounter $Value.$name "$Context $name" | Out-Null
    }
}

function Assert-Stage5PerformanceBooleanFields {
    param([object]$Value, [string[]]$Names, [string]$Context)
    foreach ($name in $Names) {
        Assert-Stage5PerformanceCondition ($Value.$name -is [bool]) `
            "$Context $name must be a JSON boolean."
    }
}

function Assert-Stage5KernelReferenceContract {
    param([object]$Receipt, [object]$Context, [string]$Label,
        [string]$ExpectedMeasurementRole = 'throughput')
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        ($Receipt.schemaVersion -eq 5 -or $Receipt.schemaVersion -eq 6)) `
        "$Label receipt schemaVersion is not an exact supported integer."
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
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $schemaVersion) -and
        $schemaVersion -eq 1 -and
        (@('throughput-binding', 'serial-oracle') -ccontains [string]$reference.mode -or
            ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
                $Receipt.schemaVersion -eq 6 -and
                $reference.mode -ceq 'phase-baseline-binding')) -and
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
        Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $timingStream.firstFrame) -and
            (Test-Stage5JsonInteger $timingStream.lastFrame)) `
            "$Label matching kernel timing frame range is not an exact integer."
        $timingFirstFrame = Get-Stage5UnsignedCounter $timingStream.firstFrame `
            "$Label matching kernel timing firstFrame" $true
        $timingLastFrame = Get-Stage5UnsignedCounter $timingStream.lastFrame `
            "$Label matching kernel timing lastFrame" $true
        Assert-Stage5PerformanceCondition ($committedBatches -eq $timingCommitted -and
            $validatedBatches -le $timingAdmitted -and
            $firstFrame -ge $timingFirstFrame -and
            $lastFrame -le $timingLastFrame) `
            "$Label kernel reference does not match the executable timing ledger."
        if ($reference.mode -ceq 'throughput-binding' -or $reference.mode -ceq 'phase-baseline-binding') {
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
    Assert-Stage5PerformanceCondition (@('throughput', 'serial-oracle', 'phase-serial-baseline') -ccontains
        [string]$ExpectedMeasurementRole) `
        "$Label expected measurement role is unsupported."
    Assert-Stage5PerformanceCondition ($Receipt.measurementRole -is [string] -and
        $Receipt.measurementRole -ceq $ExpectedMeasurementRole -and
        (($ExpectedMeasurementRole -ceq 'throughput' -and
            $reference.mode -ceq 'throughput-binding') -or
         ($ExpectedMeasurementRole -ceq 'serial-oracle' -and
            $reference.mode -ceq 'serial-oracle') -or
         ($ExpectedMeasurementRole -ceq 'phase-serial-baseline' -and
             (Test-Stage5JsonInteger $Receipt.schemaVersion) -and
             $Receipt.schemaVersion -eq 6 -and
             $reference.mode -ceq 'phase-baseline-binding'))) `
        "$Label measurement role and kernel reference mode do not match."
    $expectedTimingMode = if ($ExpectedMeasurementRole -ceq 'phase-serial-baseline') {
        'owner-inline-baseline-observation'
    } else { 'owner-pipeline-observation' }
    Assert-Stage5PerformanceCondition ($Receipt.kernelTiming.mode -ceq $expectedTimingMode -and
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
    $receiptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $receiptPath "$label receipt"
    $receiptHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $receiptSnapshot `
        $Run.receiptSha256 "$label receipt"
    Assert-Stage5PerformanceCondition (-not $SeenReceiptPaths.ContainsKey(
        $receiptPath.ToLowerInvariant())) "$label reuses a receipt path."
    Assert-Stage5PerformanceCondition (-not $SeenReceiptHashes.ContainsKey($receiptHash)) `
        "$label reuses an executable receipt."
    $receipt = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $receiptSnapshot `
        "$label receipt" -AsPsObject
    $receiptSchemaVersion = Get-Stage5UnsignedCounter $receipt.schemaVersion `
        "$label receipt schemaVersion" $false
    Assert-Stage5PerformanceCondition (@([UInt64]5, [UInt64]6) -contains
        $receiptSchemaVersion) "$label receipt schemaVersion is unsupported."
    $receiptFields = @('schemaVersion', 'producer',
        'evidenceKind', 'status', 'role', 'producerVersion', 'title', 'runId',
        'runNonce', 'cohortNonce', 'cohortCreatedUtc', 'recordedUtc',
        'architecture', 'sourceCommit', 'artifactSetSha256', 'runtimeClosure',
        'executablePath', 'executableSha256', 'commandLine', 'process',
        'fixture', 'workload', 'frameSimulation', 'frames', 'worker', 'topology',
        'rawEvidence', 'rawLogs',
        'provenance', 'schedulerMetrics', 'phases', 'kernels', 'kernelTiming',
        'measurementRole', 'kernelReference', 'simulationMode',
        'schedulerStarted')
    $isV6 = (Test-Stage5JsonInteger $receipt.schemaVersion) -and
        $receipt.schemaVersion -eq 6
    if ($isV6) { $receiptFields += @('phaseAccounting','attemptTrace') }
    Assert-Stage5PerformanceProperties $receipt $receiptFields "$label receipt"
    $nativeVersion = if ($isV6) { '6' } else { '5' }
    if ($isV6 -and $ExpectedMeasurementRole -cne 'phase-serial-baseline') {
        Assert-Stage5PerformanceCondition ($null -eq $receipt.phaseAccounting) `
            "$label ordinary/oracle role cannot advertise phase accounting."
    }
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
    $expectedNativeSchemaVersion = if ($isV6) { 6 } else { 5 }
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $receipt.schemaVersion) -and
        $receipt.schemaVersion -eq $expectedNativeSchemaVersion -and
        $receipt.producer -ceq "game-executable-stage5-performance-report-v$nativeVersion" -and
        $receipt.evidenceKind -ceq 'stage5-executable-originated-receipt' -and
        $receipt.status -ceq 'passed' -and $receipt.role -ceq 'performance-report' -and
        $receipt.producerVersion -ceq $nativeVersion -and $receipt.architecture -ceq 'x64' -and
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
    Assert-Stage5PerformanceUnsignedFields $Run @('ordinal') "$label schedule"
    Assert-Stage5PerformanceBooleanFields $Run @('warmup') "$label schedule"
    Assert-Stage5PerformanceUnsignedFields $hostObservation @('processId',
        'creationTimeUtc100ns', 'exitCode') "$label host observation"
    Assert-Stage5PerformanceCondition (Test-Stage5JsonNumber `
        $hostObservation.elapsedMilliseconds) `
        "$label host elapsedMilliseconds must be a finite JSON number."
    Assert-Stage5PerformanceUnsignedFields $receipt.process @('id',
        'creationTimeUtc100ns', 'startTimeUtc100ns', 'endTimeUtc100ns',
        'exitCode') "$label process"
    Assert-Stage5PerformanceBooleanFields $receipt.process @(
        'identityAvailable', 'exitCodeKnown') "$label process"
    Assert-Stage5PerformanceUnsignedFields $receipt.fixture @('seed',
        'requestedPlayerCount', 'requestedMinimumUnitCount') "$label fixture"
    Assert-Stage5PerformanceBooleanFields $receipt.fixture @('identityObserved',
        'seedKnown') "$label fixture"
    Assert-Stage5PerformanceUnsignedFields $receipt.workload @('sampleCount',
        'firstFrame', 'lastFrame', 'playerCount', 'initialUnitCount',
        'minimumUnitCount', 'peakUnitCount') "$label workload"
    Assert-Stage5PerformanceBooleanFields $receipt.workload @('rosterStable',
        'contiguous') "$label workload"
    Assert-Stage5PerformanceUnsignedFields $receipt.frames @('start', 'end',
        'final', 'finalCrc') "$label frames"
    Assert-Stage5PerformanceBooleanFields $receipt.frames @('finalCrcKnown') `
        "$label frames"
    Assert-Stage5PerformanceUnsignedFields $receipt.worker @('requestedCount',
        'effectiveCount', 'availableLogicalCpuCount', 'reservedOwnerCpuCount',
        'selectedWorkerCpuCount', 'selectedWorkerPhysicalCoreCount',
        'selectedWorkerPhysicalCoreMask') "$label worker"
    Assert-Stage5PerformanceBooleanFields $receipt.worker @('pinned',
        'selectedWorkerPhysicalCoreMaskComplete') "$label worker"
    Assert-Stage5PerformanceUnsignedFields $receipt.rawEvidence @(
        'timingSessionCount', 'timingFrameSamples', 'timingFirstFrame',
        'timingLastFrame') "$label raw evidence"
    Assert-Stage5PerformanceBooleanFields $receipt.rawEvidence @('timingClosed',
        'timingWriteSucceeded', 'timingTruncated', 'timingComplete') `
        "$label raw evidence"
    Assert-Stage5PerformanceUnsignedFields $receipt.provenance @('processId',
        'exitCode') "$label provenance"
    $schedulerCounterFields = @('submittedJobCount', 'executedJobCount',
        'stealCount', 'ownerHelpCount', 'waitCount',
        'workerWaitRejectionCount', 'failedJobCount', 'cancelledJobCount',
        'serialFallbackCount', 'totalQueueLatencyNanoseconds',
        'maximumQueueLatencyNanoseconds', 'workerBusyNanoseconds',
        'workerWaitNanoseconds', 'affinityFailureCount', 'injectionHighWater',
        'maximumActiveWorkers', 'availableLogicalCpuCount',
        'reservedOwnerCpuCount', 'selectedWorkerCpuCount',
        'selectedWorkerPhysicalCoreCount', 'selectedWorkerPhysicalCoreMask')
    Assert-Stage5PerformanceUnsignedFields $receipt.schedulerMetrics `
        $schedulerCounterFields "$label scheduler metrics"
    Assert-Stage5PerformanceBooleanFields $receipt.schedulerMetrics @(
        'selectedWorkerPhysicalCoreMaskComplete') "$label scheduler metrics"
    Assert-Stage5PerformanceCondition (
        [UInt64]$receipt.schedulerMetrics.affinityFailureCount -eq 0) `
        "$label pinned physical lane recorded an affinity failure."
    if ([string]$Run.lane -cne 'forced-one' -and
        $ExpectedMeasurementRole -cne 'phase-serial-baseline') {
        $scheduler = $receipt.schedulerMetrics
        Assert-Stage5RealMulticoreKernelEvidence $receipt $label
        Assert-Stage5PerformanceCondition ([bool]$receipt.worker.pinned -and
            [UInt64]$receipt.worker.effectiveCount -gt 1 -and
            [UInt64]$receipt.worker.selectedWorkerCpuCount -gt 1 -and
            [UInt64]$receipt.worker.selectedWorkerPhysicalCoreCount -gt 1 -and
            [UInt64]$receipt.worker.selectedWorkerPhysicalCoreMask -ne 0 -and
            (Get-Stage5UInt64BitCount ([UInt64]$receipt.worker.selectedWorkerPhysicalCoreMask)) -eq
                [UInt64]$receipt.worker.selectedWorkerPhysicalCoreCount -and
            [UInt64]$scheduler.submittedJobCount -gt 0 -and
            [UInt64]$scheduler.executedJobCount -eq [UInt64]$scheduler.submittedJobCount -and
            [UInt64]$scheduler.workerBusyNanoseconds -gt 0 -and
            [UInt64]$scheduler.maximumActiveWorkers -gt 1 -and
            [UInt64]$scheduler.maximumActiveWorkers -le [UInt64]$receipt.worker.effectiveCount -and
            [UInt64]$scheduler.selectedWorkerPhysicalCoreMask -eq
                [UInt64]$receipt.worker.selectedWorkerPhysicalCoreMask -and
            [UInt64]$scheduler.failedJobCount -eq 0 -and
            [UInt64]$scheduler.cancelledJobCount -eq 0 -and
            [UInt64]$scheduler.serialFallbackCount -eq 0 -and
            [UInt64]$scheduler.workerWaitRejectionCount -eq 0 -and
            $receipt.kernelTiming.streams -is [Array]) `
            ("$label does not prove that all six kernel families used physical workers and the run demonstrated scheduler-wide multicore concurrency " +
            "(workers=$($receipt.worker.effectiveCount), selected=$($receipt.worker.selectedWorkerCpuCount), " +
            "physical=$($receipt.worker.selectedWorkerPhysicalCoreCount), mask=$($receipt.worker.selectedWorkerPhysicalCoreMask), " +
            "submitted=$($scheduler.submittedJobCount), executed=$($scheduler.executedJobCount), " +
            "busy=$($scheduler.workerBusyNanoseconds), active=$($scheduler.maximumActiveWorkers), " +
            "schedulerMask=$($scheduler.selectedWorkerPhysicalCoreMask), streams=$(@($receipt.kernelTiming.streams).Count)).")
    }
    foreach ($cpuSet in @($receipt.topology.cpuSets)) {
        Assert-Stage5PerformanceUnsignedFields $cpuSet @('id', 'group',
            'coreIndex', 'logicalProcessorIndex', 'efficiencyClass') `
            "$label receipt CPU set"
        Assert-Stage5PerformanceBooleanFields $cpuSet @('parked',
            'allocatedToOtherProcess', 'availableToProcess') `
            "$label receipt CPU set"
    }
    foreach ($id in @($receipt.topology.ownerCpuSetIds) +
        @($receipt.topology.selectedWorkerCpuSetIds)) {
        Get-Stage5UnsignedCounter $id "$label topology CPU-set id" | Out-Null
    }
    foreach ($cpuSet in @($Context.topology.cpuSets)) {
        Assert-Stage5PerformanceUnsignedFields $cpuSet @('id', 'group',
            'coreIndex', 'logicalProcessorIndex', 'efficiencyClass') `
            "$label independent host CPU set"
        Assert-Stage5PerformanceBooleanFields $cpuSet @('parked', 'allocated',
            'available') "$label independent host CPU set"
    }
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
    $rawSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $rawPath `
        "$label raw diagnostic"
    $timingSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $timingPath `
        "$label timing evidence"
    $rawHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $rawSnapshot `
        ([string]$receipt.rawEvidence.rawLogSha256) "$label raw diagnostic"
    $timingHash = Assert-Stage5FinalAcceptanceSnapshotSha256 $timingSnapshot `
        ([string]$receipt.rawEvidence.timingSha256) "$label timing evidence"
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
        $rawEntryHash = if ([String]::Equals($rawEntryPath, $rawPath,
                [StringComparison]::OrdinalIgnoreCase)) { $rawHash }
            elseif ([String]::Equals($rawEntryPath, $timingPath,
                [StringComparison]::OrdinalIgnoreCase)) { $timingHash }
            else { throw "$label raw log '$rawName' is detached from the snapshotted evidence paths." }
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
    Assert-Stage5RawDiagnostic $rawSnapshot $receipt $label
    Assert-Stage5PerformanceCondition ([Int64]$timingSnapshot.length -gt 0) `
        "$label timing evidence is empty."
    Assert-Stage5PerformanceCondition (Test-Stage5PerformanceFinitePositive `
        $hostObservation.elapsedMilliseconds) "$label host elapsed time is invalid."
    if ($isV6) { Assert-Stage5PhaseTraceContract $receipt $Context $label }
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
        expectedArgumentString = [string]$Run.expectedArgumentString
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

function Read-Stage5PhaseBoundJson {
    param([object]$Binding, [string]$Root, [string]$Label)
    Assert-Stage5PerformanceProperties $Binding @('path','sha256') $Label
    Assert-Stage5PerformanceCondition ($Binding.path -is [string] -and
        $Binding.sha256 -is [string]) "$Label path and SHA-256 must be scalar strings."
    Assert-Stage5PerformanceHash $Binding.sha256 "$Label SHA-256"
    $path = Resolve-Stage5RunEvidenceFile $Root ([string]$Binding.path) $Label
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path $Label
    Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $Binding.sha256 `
        "$Label SHA-256" | Out-Null
    return ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot $Label -AsPsObject
}

function Get-Stage5PhaseRunIdentitySha256 {
    param([string]$RunId, [string]$Nonce, [object]$ProcessId, [object]$Creation)
    Assert-Stage5PerformanceCondition ($RunId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and
        -not $RunId.Contains('..') -and
        $Nonce -cmatch '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
        (Test-Stage5JsonInteger $ProcessId) -and (Test-Stage5JsonInteger $Creation) -and
        $ProcessId -gt 0 -and $ProcessId -le [UInt32]::MaxValue -and $Creation -gt 0) `
        'Phase trace source has an invalid exact native process identity.'
    $memory = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($memory)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RTS-KERNEL-FIELDS-v1'))
        $writer.Write([UInt32]0x5003)
        [UInt32]$tag = 1
        foreach ($value in @($RunId, $Nonce)) {
            $bytes = [Text.Encoding]::ASCII.GetBytes($value)
            $writer.Write([byte]6); $writer.Write($tag); $writer.Write([UInt32]$bytes.Length)
            foreach ($item in $bytes) {
                $writer.Write([byte]1); $writer.Write($tag); $writer.Write([UInt32]$item)
            }
            ++$tag
        }
        $writer.Write([byte]1); $writer.Write([UInt32]3); $writer.Write([UInt32]$ProcessId)
        $writer.Write([byte]3); $writer.Write([UInt32]4); $writer.Write([UInt64]$Creation)
        $writer.Flush()
        $hash = [Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($hash.ComputeHash($memory.ToArray()))).Replace('-', '') }
        finally { $hash.Dispose() }
    }
    finally { $writer.Dispose(); $memory.Dispose() }
}

function Assert-Stage5PhaseTraceContract {
    param([object]$Receipt, [object]$Context, [string]$Label)
    $trace = $Receipt.attemptTrace
    if ($null -eq $trace) {
        Assert-Stage5PerformanceCondition ($Receipt.measurementRole -cne 'phase-serial-baseline') `
            "$Label baseline cannot strip its consumed source trace."
        return
    }
    Assert-Stage5PerformanceCondition ($Context.PSObject.Properties.Name -ccontains 'phaseBaselinePolicy' -and
        $Context.phaseBaselinePolicy -ceq 'paired-source-admissions-v1') "$Label trace lacks a declared phase cohort."
    $counters = @('residentAttemptCapacity','residentRangeCapacity','residentAttemptCount',
        'residentAttemptHighWater','residentRangeCount','residentRangeHighWater','recordCount',
        'logicalEventCount','coalescedSpanCount','coalescedAttemptCount','attemptCount',
        'admittedAttemptCount','notAdmittedAttemptCount','abortedAfterAdmissionAttemptCount',
        'reapCount','capturedAttemptCount','capturedOperationCount','dispatchCount','rangeCount','releasedRangeCount',
        'windowBoundaryCount','completedWindowCount','controlWindowCount')
    Assert-Stage5PerformanceProperties $trace (@('schemaVersion','encoding','fieldSchema','mode',
        'frozen','complete','errors','observationIngressSealed','executionClosureSealed',
        'file','binding','limits','sourceBinding') + $counters) "$Label trace"
    foreach ($field in @('schemaVersion','fieldSchema','errors') + $counters) {
        Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $trace.$field) "$Label trace $field is not an exact integer."
        Get-Stage5UnsignedCounter $trace.$field "$Label trace $field" | Out-Null
    }
    foreach ($field in @('frozen','complete','observationIngressSealed','executionClosureSealed')) {
        Assert-Stage5PerformanceCondition ($trace.$field -is [bool] -and $trace.$field) "$Label trace $field is not proven."
    }
    $expectedMode = if ($Receipt.measurementRole -ceq 'phase-serial-baseline') { 'consume' } else { 'record' }
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Receipt.schemaVersion) -and
        (Test-Stage5JsonInteger $trace.schemaVersion) -and
        $Receipt.schemaVersion -eq 6 -and
        @('throughput','phase-serial-baseline') -ccontains $Receipt.measurementRole -and
        $trace.schemaVersion -eq 1 -and $trace.encoding -ceq 'typed-canonical-le-v1' -and
        $trace.fieldSchema -eq 20481 -and $trace.errors -eq 0 -and $trace.mode -ceq $expectedMode) `
        "$Label trace version or role is invalid."
    Assert-Stage5PerformanceProperties $trace.file @('path','sha256','byteCount') "$Label trace file"
    Assert-Stage5PerformanceProperties $trace.binding @('nativeRunIdentitySha256','executableSha256',
        'fixtureSha256','sourcePolicySha256') "$Label trace identity"
    foreach ($name in @('nativeRunIdentitySha256','executableSha256','fixtureSha256','sourcePolicySha256')) {
        Assert-Stage5PerformanceHash $trace.binding.$name "$Label trace $name"
    }
    $limitNames = @('maximumBytes','maximumRecords','maximumLogicalEvents','maximumAttempts','maximumRanges')
    Assert-Stage5PerformanceProperties $trace.limits $limitNames "$Label trace limits"
    foreach ($name in $limitNames) {
        Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $trace.limits.$name) "$Label trace limit must be exact."
        Get-Stage5UnsignedCounter $trace.limits.$name "$Label trace $name" $false | Out-Null
    }
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $trace.file.byteCount) -and
        $trace.file.byteCount -gt 0 -and $trace.file.byteCount -le $trace.limits.maximumBytes -and
        $trace.recordCount -gt 0 -and $trace.recordCount -le $trace.limits.maximumRecords -and
        $trace.logicalEventCount -ge $trace.recordCount -and $trace.logicalEventCount -le $trace.limits.maximumLogicalEvents -and
        ($trace.coalescedSpanCount -ne 0 -or $trace.logicalEventCount -eq $trace.recordCount) -and
        $trace.coalescedSpanCount -le $trace.recordCount -and
        $trace.coalescedSpanCount -le $trace.coalescedAttemptCount -and
        $trace.coalescedAttemptCount -le $trace.attemptCount -and
        $trace.attemptCount -le $trace.limits.maximumAttempts -and
        [decimal]$trace.attemptCount -eq ([decimal]$trace.admittedAttemptCount + [decimal]$trace.notAdmittedAttemptCount) -and
        $trace.abortedAfterAdmissionAttemptCount -le $trace.admittedAttemptCount -and
        $trace.reapCount -eq $trace.attemptCount -and $trace.capturedAttemptCount -le $trace.attemptCount -and
        $trace.rangeCount -le $trace.limits.maximumRanges -and $trace.releasedRangeCount -eq $trace.rangeCount -and
        $trace.residentAttemptCapacity -gt 0 -and $trace.residentRangeCapacity -gt 0 -and
        $trace.residentAttemptCount -eq 0 -and $trace.residentRangeCount -eq 0 -and
        $trace.residentAttemptHighWater -le $trace.residentAttemptCapacity -and
        $trace.residentRangeHighWater -le $trace.residentRangeCapacity) "$Label trace counts do not close within frozen limits."
    Assert-Stage5PerformanceCondition (
        $trace.windowBoundaryCount -gt 0 -and
        $trace.windowBoundaryCount -le $trace.recordCount -and
        $trace.completedWindowCount -eq $Receipt.workload.sampleCount -and
        ([decimal]$trace.completedWindowCount + [decimal]$trace.controlWindowCount) -le
            [decimal]$trace.windowBoundaryCount) `
        "$Label trace window footer coverage is inconsistent."
    if ($expectedMode -ceq 'consume') {
        Assert-Stage5PerformanceCondition (
            $trace.controlWindowCount -eq
                $Receipt.phaseAccounting.controlAccounting.windowCount) `
            "$Label trace control-window count differs from phase accounting."
    }
    Assert-Stage5PerformanceCondition ($trace.file.path -is [string] -and
        $trace.file.sha256 -is [string]) "$Label trace path and SHA-256 must be scalar strings."
    $tracePath = Resolve-Stage5RunEvidenceFile $Context.taskRoot $trace.file.path "$Label trace bytes"
    $traceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $tracePath `
        "$Label trace bytes" -HashOnly -EvidenceKind Trace
    Assert-Stage5FinalAcceptanceHashOnlySnapshotSha256 `
        -Snapshot $traceSnapshot -Expected $trace.file.sha256 `
        -ExpectedLength ([Int64]$trace.file.byteCount) `
        -Context "$Label trace bytes" | Out-Null
    $identity = $Receipt
    if ($expectedMode -ceq 'consume') {
        $source = $trace.sourceBinding
        Assert-Stage5PerformanceProperties $source @('receipt','runId','runNonce','processId','processCreationTimeUtc100ns') "$Label source binding"
        Get-Stage5PhaseRunIdentitySha256 $source.runId $source.runNonce $source.processId `
            $source.processCreationTimeUtc100ns | Out-Null
        $identity = Read-Stage5PhaseBoundJson $source.receipt $Context.taskRoot "$Label selected source receipt"
        Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $identity.schemaVersion) -and
            $identity.schemaVersion -eq 6 -and $identity.measurementRole -ceq 'throughput' -and
            $identity.runId -ceq $source.runId -and $identity.runNonce -ceq $source.runNonce -and
            $identity.process.id -eq $source.processId -and
            $identity.process.creationTimeUtc100ns -eq $source.processCreationTimeUtc100ns) "$Label source binding does not identify its raw throughput receipt."
    }
    else { Assert-Stage5PerformanceCondition ($null -eq $trace.sourceBinding) "$Label record cannot claim a source receipt." }
    $identityHash = Get-Stage5PhaseRunIdentitySha256 $identity.runId $identity.runNonce `
        $identity.process.id $identity.process.creationTimeUtc100ns
    Assert-Stage5PerformanceCondition ($trace.binding.nativeRunIdentitySha256 -ceq $identityHash -and
        $trace.binding.executableSha256 -ceq $Receipt.executableSha256 -and
        $trace.binding.fixtureSha256 -ceq $Receipt.fixture.contentSha256) "$Label trace native identity differs from its receipt."
}

function Assert-Stage5PhaseCohort {
    param([object]$Document, [Collections.IDictionary]$SeenRunIds,
        [Collections.IDictionary]$SeenRunNonces, [Collections.IDictionary]$SeenReceiptPaths,
        [Collections.IDictionary]$SeenReceiptHashes)
    Assert-Stage5PerformanceCondition ($Document.phaseBaselinePolicy -ceq 'paired-source-admissions-v1' -and
        $Document.phaseBaselineProfiles -is [Array] -and $Document.phaseBaselineProfiles.Count -gt 0 -and
        $Document.pairedPhaseBaselineBindings -is [Array]) 'Phase cohort policy/profiles are incomplete.'
    $plan = Read-Stage5PhaseBoundJson $Document.phaseBaselinePlan $Document.taskRoot 'Frozen phase plan'
    $attempts = Read-Stage5PhaseBoundJson $Document.phaseBaselineAttemptManifest $Document.taskRoot 'Phase attempt manifest'
    $titleSession = New-Stage5TitleSessionContract $Document.title (Join-Path $Document.taskRoot 'TitleSession') `
        (Split-Path -Parent $Document.executablePath) $Document.taskRoot
    Resolve-Stage5PlannedPerformanceLaunch $Document $Document.phaseBaselinePlan $plan.entries[0].entryId $titleSession | Out-Null
    Assert-Stage5PerformanceProperties $attempts @('schemaVersion','planSha256','outcomes','cohortFailure') 'Phase attempt manifest'
    Assert-Stage5PerformanceCondition ($Document.phaseBaselineAttemptManifest.path -ceq (Join-Path $Document.taskRoot 'phase-attempts.json') -and
        $null -eq $attempts.cohortFailure) 'A failed or displaced final attempt manifest cannot qualify.'
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $plan.schemaVersion) -and
        (Test-Stage5JsonInteger $attempts.schemaVersion) -and
        $plan.schemaVersion -eq 1 -and $attempts.schemaVersion -eq 1 -and
        $plan.cohortNonce -ceq $Document.cohortNonce -and $plan.executableSha256 -ceq $Document.executableSha256 -and
        $plan.sourceCommit -ceq $Document.sourceCommit -and $attempts.planSha256 -ceq $Document.phaseBaselinePlan.sha256 -and
        $plan.entries -is [Array] -and $attempts.outcomes -is [Array] -and
        (ConvertTo-Json @($plan.phaseBaselineProfiles) -Depth 20 -Compress) -ceq
            (ConvertTo-Json @($Document.phaseBaselineProfiles) -Depth 20 -Compress)) 'Phase cohort differs from its independently reopened original plan.'
    $profiles = @{}; $selected = @{}
    foreach ($profile in $plan.phaseBaselineProfiles) {
        Assert-Stage5PerformanceProperties $profile @('profileId','fixtureId','sourceLane','sourcePolicySha256',
            'limits','residentAttemptCapacity','residentRangeCapacity','fixtureSha256','window','warmupRuns','measuredRuns') 'Phase profile'
        Assert-Stage5PerformanceProperties $profile.window @('firstCompletedFrame','lastCompletedFrame',
            'completedFrameCount','controlWindowCount') 'Phase profile window'
        Assert-Stage5PerformanceProperties $profile.limits @('maximumBytes','maximumRecords',
            'maximumLogicalEvents','maximumAttempts','maximumRanges') 'Phase profile limits'
        foreach ($field in @('warmupRuns','measuredRuns','residentAttemptCapacity','residentRangeCapacity')) {
            Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $profile.$field) "Phase profile $field is not exact."
            Get-Stage5UnsignedCounter $profile.$field "Phase profile $field" $false | Out-Null
        }
        foreach ($field in $profile.limits.PSObject.Properties.Name) {
            Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $profile.limits.$field) "Phase profile limit $field is not exact."
            Get-Stage5UnsignedCounter $profile.limits.$field "Phase profile limit $field" $false | Out-Null
        }
        foreach ($field in $profile.window.PSObject.Properties.Name) {
            Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $profile.window.$field) "Phase profile window $field is not exact."
            Get-Stage5UnsignedCounter $profile.window.$field "Phase profile window $field" | Out-Null
        }
        Assert-Stage5PerformanceCondition ($profile.fixtureId -is [string] -and
            $profile.sourceLane -is [string]) 'Phase profile identity fields must be scalar strings.'
        Assert-Stage5PerformanceHash $profile.sourcePolicySha256 'Phase profile source policy'
        Assert-Stage5PerformanceHash $profile.fixtureSha256 'Phase profile fixture'
        Assert-Stage5PerformanceCondition ($profile.profileId -is [string] -and $profile.profileId.Length -gt 0 -and
            -not $profiles.ContainsKey($profile.profileId) -and $profile.warmupRuns -eq $Document.warmupRuns -and
            $profile.measuredRuns -eq $Document.measuredRuns -and $profile.window.firstCompletedFrame -gt 0 -and
            $profile.window.lastCompletedFrame -le [UInt32]::MaxValue -and
            [decimal]$profile.window.lastCompletedFrame - [decimal]$profile.window.firstCompletedFrame + 1 -eq
                [decimal]$profile.window.completedFrameCount) 'Phase profile is duplicated or changes the declared repeats/window.'
        $profiles[$profile.profileId] = $profile
        $sources = @($Document.runs | Where-Object { $_.fixtureId -ceq $profile.fixtureId -and $_.lane -ceq $profile.sourceLane })
        Assert-Stage5PerformanceCondition ($sources.Count -eq 1 + $Document.measuredRuns) 'Phase profile does not name an entire scheduled source lane.'
        foreach ($source in $sources) {
            Assert-Stage5PerformanceCondition (-not $selected.ContainsKey($source.runId)) 'A phase source repeat cannot be selected twice.'
            $selected[$source.runId] = $profile
        }
    }
    Assert-Stage5PerformanceCondition ($Document.pairedPhaseBaselineBindings.Count -eq $selected.Count) 'Every selected source, including warmup, requires one baseline.'
    $actualEntries = @(); $validated = @(); $paired = @{}
    foreach ($run in $Document.runs) {
        $actualEntries += @{run=$run; role='throughput'; source=$null; profile=$null}
        $native = Read-Stage5PhaseBoundJson ([pscustomobject]@{path=$run.receiptPath;sha256=$run.receiptSha256}) $Document.taskRoot 'Phase cohort throughput'
        $isSelectedSource = $selected.ContainsKey($run.runId)
        Assert-Stage5PerformanceCondition (
            (Test-Stage5JsonInteger $native.schemaVersion) -and
            (($isSelectedSource -and $native.schemaVersion -eq 6 -and
                    $null -ne $native.attemptTrace) -or
             (-not $isSelectedSource -and $native.schemaVersion -eq 5 -and
                    $null -eq $native.PSObject.Properties['attemptTrace']))) `
            'A phase source was added, stripped, upgraded, or downgraded outside the frozen plan.'
    }
    if ($Document.PSObject.Properties.Name -ccontains 'pairedOracleBindings') {
        foreach ($pair in $Document.pairedOracleBindings) {
            $actualEntries += @{run=$pair.oracleRun; role='serial-oracle'; source=$pair.throughputRunId; profile=$null}
        }
    }
    foreach ($pair in $Document.pairedPhaseBaselineBindings) {
        Assert-Stage5PerformanceProperties $pair @('profileId','throughputRunId','baselineRun') 'Phase baseline pair'
        Assert-Stage5PerformanceCondition ($pair.profileId -is [string] -and
            $pair.throughputRunId -is [string] -and
            $selected.ContainsKey([string]$pair.throughputRunId) -and
            -not $paired.ContainsKey([string]$pair.throughputRunId) -and
            $pair.profileId -ceq $selected[$pair.throughputRunId].profileId) 'Phase pair changes or repeats its planned source.'
        $profile = $selected[$pair.throughputRunId]
        $sourceRun = @($Document.runs | Where-Object { $_.runId -ceq $pair.throughputRunId })[0]
        $baselineRun = $pair.baselineRun
        Assert-Stage5PerformanceProperties $baselineRun @('fixtureId','lane','ordinal','warmup',
            'runId','runNonce','expectedArgumentString','receiptPath','receiptSha256','host') 'Phase baseline run'
        foreach ($field in @('fixtureId','lane','ordinal','warmup')) {
            Assert-Stage5PerformanceCondition ($baselineRun.$field -ceq $sourceRun.$field) "Phase pair changes source $field."
        }
        $baselineValidated = Assert-Stage5Receipt $baselineRun $Document $SeenRunIds $SeenRunNonces `
            $SeenReceiptPaths $SeenReceiptHashes 'phase-serial-baseline'
        $source = Read-Stage5PhaseBoundJson ([pscustomobject]@{path=$sourceRun.receiptPath;sha256=$sourceRun.receiptSha256}) $Document.taskRoot 'Selected throughput source'
        $baseline = Read-Stage5PhaseBoundJson ([pscustomobject]@{path=$baselineRun.receiptPath;sha256=$baselineRun.receiptSha256}) $Document.taskRoot 'Selected baseline'
        foreach ($field in @('title','architecture','sourceCommit','artifactSetSha256','runtimeClosure','executablePath',
                'executableSha256','commandLine','cohortNonce','cohortCreatedUtc','simulationMode','schedulerStarted',
                'fixture','workload','frames','worker','topology')) {
            Assert-Stage5PerformanceCondition ((ConvertTo-Json $source.$field -Depth 20 -Compress) -ceq
                (ConvertTo-Json $baseline.$field -Depth 20 -Compress)) "Phase pair differs in $field."
        }
        Assert-Stage5PerformanceCondition ($source.process.id -ne $baseline.process.id -and
            $source.process.creationTimeUtc100ns -ne $baseline.process.creationTimeUtc100ns -and
            (ConvertTo-Json $source.kernelReference.streams -Depth 20 -Compress) -ceq
                (ConvertTo-Json $baseline.kernelReference.streams -Depth 20 -Compress)) 'Phase baseline reused process identity or changed canonical source results.'
        Assert-Stage5PerformanceCondition ($source.kernelTiming.streams.Count -eq $baseline.kernelTiming.streams.Count) 'Phase baseline changed timing stream coverage.'
        for ($index = 0; $index -lt $source.kernelTiming.streams.Count; ++$index) {
            foreach ($field in @('name','subtype','attemptedBatches','admittedBatches','committedBatches',
                    'abortedBatches','firstFrame','lastFrame')) {
                Assert-Stage5PerformanceCondition ($source.kernelTiming.streams[$index].$field -ceq
                    $baseline.kernelTiming.streams[$index].$field) "Phase baseline changed timing stream $field."
            }
        }
        $trace = $baseline.attemptTrace
        Assert-Stage5PerformanceCondition ($trace.sourceBinding.receipt.path -ceq $sourceRun.receiptPath -and
            $trace.sourceBinding.receipt.sha256 -ceq $sourceRun.receiptSha256 -and
            $trace.binding.sourcePolicySha256 -ceq $profile.sourcePolicySha256 -and
            $source.fixture.contentSha256 -ceq $profile.fixtureSha256 -and
            $baseline.phaseAccounting.completedFrameCount -eq $profile.window.completedFrameCount -and
            $baseline.phaseAccounting.firstCompletedFrame -eq $profile.window.firstCompletedFrame -and
            $baseline.phaseAccounting.lastCompletedFrame -eq $profile.window.lastCompletedFrame -and
            $baseline.phaseAccounting.controlAccounting.windowCount -eq $profile.window.controlWindowCount) 'Phase source receipt/profile/window substitution was detected.'
        foreach ($field in $source.attemptTrace.PSObject.Properties.Name) {
            if ($field -ceq 'mode' -or $field -ceq 'sourceBinding') { continue }
            Assert-Stage5PerformanceCondition ((ConvertTo-Json $source.attemptTrace.$field -Depth 20 -Compress) -ceq
                (ConvertTo-Json $trace.$field -Depth 20 -Compress)) "Consumed trace changed source $field."
        }
        foreach ($field in @('limits','residentAttemptCapacity','residentRangeCapacity')) {
            Assert-Stage5PerformanceCondition ((ConvertTo-Json $trace.$field -Depth 20 -Compress) -ceq
                (ConvertTo-Json $profile.$field -Depth 20 -Compress)) "Consumed trace changed planned $field."
        }
        $actualEntries += @{run=$baselineRun; role='phase-serial-baseline'; source=$sourceRun.runId; profile=$profile.profileId}
        $paired[$pair.throughputRunId] = $true
        $validated += [pscustomobject]@{profileId=$profile.profileId;throughputRunId=$sourceRun.runId;baselineRun=$baselineValidated}
    }
    Assert-Stage5PerformanceCondition ($plan.entries.Count -eq $actualEntries.Count -and
        $attempts.outcomes.Count -eq $actualEntries.Count) 'Phase plan/attempt journal dropped or added a declared role.'
    $planById = @{}; $outcomesById = @{}; $resultsById = @{}; $actualById = @{}
    foreach ($actual in $actualEntries) { $actualById[$actual.run.runId] = $actual }
    foreach ($entry in $plan.entries) {
        Assert-Stage5PerformanceProperties $entry @('entryId','measurementRole','profileId','fixtureId','lane','ordinal','warmup','sourceEntryId',
            'runNonce','workerCount','expectedArgumentString','outputPaths') 'Planned phase entry'
        Assert-Stage5PerformanceCondition ($entry.entryId -is [string] -and
            $entry.measurementRole -is [string] -and
            ($null -eq $entry.profileId -or $entry.profileId -is [string]) -and
            $entry.fixtureId -is [string] -and $entry.lane -is [string] -and
            ($null -eq $entry.sourceEntryId -or $entry.sourceEntryId -is [string]) -and
            -not $planById.ContainsKey($entry.entryId) -and
            (Test-Stage5JsonInteger $entry.ordinal) -and $entry.warmup -is [bool]) 'Planned phase entry is duplicated or malformed.'
        $planById[$entry.entryId] = $entry
    }
    foreach ($outcome in $attempts.outcomes) {
        Assert-Stage5PerformanceProperties $outcome @('entryId','state','failure','startBinding','resultBinding') 'Phase attempt outcome'
        Assert-Stage5PerformanceCondition ($outcome.entryId -is [string] -and
            $outcome.state -is [string] -and
            -not $outcomesById.ContainsKey($outcome.entryId) -and $planById.ContainsKey($outcome.entryId) -and
            $outcome.state -ceq 'completed' -and $null -eq $outcome.failure) 'A failed, incomplete, duplicated or unresolved attempt cannot qualify.'
        $outcomesById[$outcome.entryId] = $outcome
        $entry = $planById[$outcome.entryId]
        Assert-Stage5PerformanceCondition ($null -ne $outcome.startBinding -and $null -ne $outcome.resultBinding -and
            $outcome.startBinding.path -ceq $entry.outputPaths.attemptStartPath -and
            $outcome.resultBinding.path -ceq $entry.outputPaths.attemptResultPath) 'A completed outcome lacks its original planned record paths.'
        $start = Read-Stage5PhaseBoundJson $outcome.startBinding $Document.taskRoot 'Original attempt start'
        $result = Read-Stage5PhaseBoundJson $outcome.resultBinding $Document.taskRoot 'Original attempt result'
        Assert-Stage5PerformanceProperties $start @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc','sourceBinding') 'Original attempt start'
        Assert-Stage5PerformanceProperties $result @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc',
            'startBinding','state','failure','run','processCleanup') 'Original attempt result'
        foreach ($record in @($start,$result)) {
            Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $record.schemaVersion) -and $record.schemaVersion -eq 1 -and
                $record.entryId -is [string] -and $record.entryId -ceq $entry.entryId -and
                $record.runNonce -is [string] -and $record.runNonce -ceq $entry.runNonce -and
                $record.planSha256 -is [string] -and $record.planSha256 -ceq $Document.phaseBaselinePlan.sha256 -and
                $record.recordedUtc -is [string] -and $record.recordedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$') `
                'An attempt record changed its planned identity or time representation.'
        }
        Assert-Stage5PerformanceCondition ($start.event -is [string] -and $start.event -ceq 'attempt-start' -and
            $result.event -is [string] -and $result.event -ceq 'attempt-result' -and
            $result.state -is [string] -and $result.state -ceq 'completed' -and $null -eq $result.failure -and $null -ne $result.startBinding -and
            $result.startBinding.path -is [string] -and $result.startBinding.sha256 -is [string] -and
            $result.startBinding.path -ceq $outcome.startBinding.path -and $result.startBinding.sha256 -ceq $outcome.startBinding.sha256 -and
            [DateTimeOffset]::Parse($start.recordedUtc) -le [DateTimeOffset]::Parse($result.recordedUtc)) `
            'An attempt result is unresolved or detached from its original start.'
        Assert-Stage5PerformanceProperties $result.startBinding @('path','sha256') 'Result original start binding'
        Assert-Stage5PerformanceProperties $result.processCleanup @('processId','exitProof','blocked','errors') 'Attempt child cleanup'
        Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $result.processCleanup.processId) -and
            $result.processCleanup.processId -gt 0 -and $result.processCleanup.processId -eq $result.run.host.processId -and
            $result.processCleanup.exitProof -is [bool] -and $result.processCleanup.exitProof -and
            $result.processCleanup.blocked -is [bool] -and -not $result.processCleanup.blocked -and
            $result.processCleanup.errors -is [Array] -and $result.processCleanup.errors.Count -eq 0) `
            'A completed attempt lacks exact original child cleanup proof.'
        if ($entry.measurementRole -ceq 'phase-serial-baseline') {
            Assert-Stage5PerformanceCondition ($null -ne $start.sourceBinding -and
                $start.sourceBinding.path -ceq $entry.outputPaths.sourceBindingPath) 'Baseline start lacks its original source binding.'
            $source = Read-Stage5PhaseBoundJson $start.sourceBinding $Document.taskRoot 'Original baseline source audit'
            Assert-Stage5PerformanceProperties $source @('receipt','runId','runNonce','processId','processCreationTimeUtc100ns') 'Original baseline source audit'
            $sourceRun = $actualById[$entry.sourceEntryId].run
            Assert-Stage5PerformanceCondition ($source.runId -is [string] -and $source.runNonce -is [string] -and
                $source.runId -ceq $sourceRun.runId -and $source.runNonce -ceq $sourceRun.runNonce -and
                $source.processId -eq $sourceRun.host.processId -and $source.processCreationTimeUtc100ns -eq $sourceRun.host.creationTimeUtc100ns -and
                $source.receipt.path -ceq $sourceRun.receiptPath -and $source.receipt.sha256 -ceq $sourceRun.receiptSha256) `
                'Baseline start substituted its selected original source identity.'
            Get-Stage5PhaseRunIdentitySha256 $source.runId $source.runNonce $source.processId $source.processCreationTimeUtc100ns | Out-Null
            Read-Stage5PhaseBoundJson $source.receipt $Document.taskRoot 'Original baseline source receipt' | Out-Null
        }
        else { Assert-Stage5PerformanceCondition ($null -eq $start.sourceBinding) 'An ordinary start carries an unplanned baseline source.' }
        $resultsById[$entry.entryId] = $result
    }
    foreach ($actual in $actualEntries) {
        $run = $actual.run
        Assert-Stage5PerformanceCondition ($planById.ContainsKey($run.runId) -and $outcomesById.ContainsKey($run.runId)) 'A run lacks its original planned entry or attempt outcome.'
        $entry = $planById[$run.runId]
        $result = $resultsById[$run.runId]
        Assert-Stage5PerformanceCondition ($entry.runNonce -ceq $run.runNonce -and
            $entry.expectedArgumentString -ceq $run.expectedArgumentString -and
            (ConvertTo-Json $result.run -Depth 20 -Compress) -ceq (ConvertTo-Json $run -Depth 20 -Compress)) `
            'The independently reopened result does not bind the exact observed run.'
        $native = Read-Stage5PhaseBoundJson ([pscustomobject]@{path=$run.receiptPath;sha256=$run.receiptSha256}) $Document.taskRoot 'Planned native artifact paths'
        $receiptFiles = @(Get-ChildItem -LiteralPath $entry.outputPaths.receiptDirectory -File -Filter '*.json')
        $timingFiles = @(Get-ChildItem -LiteralPath $entry.outputPaths.timingDirectory -File -Filter '*.csv')
        $expectedReceipt = Join-Path $entry.outputPaths.receiptDirectory ('performance-receipt-{0}-{1}.json' -f $entry.entryId, $run.host.processId)
        Assert-Stage5PerformanceCondition ($receiptFiles.Count -eq 1 -and $timingFiles.Count -eq 1 -and
            $run.receiptPath -ceq $expectedReceipt -and $receiptFiles[0].FullName -ceq $expectedReceipt -and
            $native.rawEvidence.rawLogPath -ceq $entry.outputPaths.rawLogPath -and
            $native.rawEvidence.timingPath -ceq $timingFiles[0].FullName -and
            $timingFiles[0].Name -cmatch ('^frame-timing-' + [Regex]::Escape([string]$run.host.processId) + '-(?:0|[1-9][0-9]*)\.csv$')) `
            'A native artifact escaped its immutable role output/naming policy.'
        if ($null -ne $entry.outputPaths.attemptTracePath) {
            Assert-Stage5PerformanceCondition ($native.attemptTrace.file.path -ceq $entry.outputPaths.attemptTracePath) `
                'A selected source trace escaped its frozen receipt bundle.'
        }
        Assert-Stage5PerformanceCondition ($entry.measurementRole -ceq $actual.role -and
            $entry.sourceEntryId -ceq $actual.source -and $entry.profileId -ceq $actual.profile) 'A planned role or source binding was relabelled.'
        foreach ($field in @('fixtureId','lane','ordinal','warmup')) {
            Assert-Stage5PerformanceCondition ($entry.$field -ceq $run.$field) "A planned run changed $field."
        }
    }
    return $validated
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
    $phaseProperties = @('phaseBaselinePolicy','phaseBaselineProfiles','phaseBaselinePlan',
        'phaseBaselineAttemptManifest','pairedPhaseBaselineBindings')
    $phasePropertyCount = @($phaseProperties | Where-Object {
        $Document.PSObject.Properties.Name -ccontains $_
    }).Count
    Assert-Stage5PerformanceCondition ($phasePropertyCount -eq 0 -or
        $phasePropertyCount -eq $phaseProperties.Count) 'Stage 5 phase cohort metadata cannot be partially stripped.'
    $hasPhasePolicy = ($phasePropertyCount -gt 0)
    if ($hasPhasePolicy) { $manifestProperties += $phaseProperties }
    $hasFixtureManifestPath = ($Document.PSObject.Properties.Name -contains
        'fixtureManifestPath')
    if ($hasFixtureManifestPath) {
        $manifestProperties += 'fixtureManifestPath'
    }
    $hasPerformanceData = ($Document.PSObject.Properties.Name -contains
        'performanceData')
    if ($hasPerformanceData) { $manifestProperties += 'performanceData' }
    $hasFixtureProduction = ($Document.PSObject.Properties.Name -contains
        'fixtureProductionReceipt')
    if ($hasFixtureProduction) {
        $manifestProperties += 'fixtureProductionReceipt'
    }
    Assert-Stage5PerformanceProperties $Document $manifestProperties `
        'Stage 5 host validation manifest'
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Document.schemaVersion) -and
        (Test-Stage5JsonInteger $Document.warmupRuns) -and
        (Test-Stage5JsonInteger $Document.measuredRuns)) `
        'Stage 5 host validation schema/warmup/measured counts must be exact JSON integers.'
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
    if ($hasPerformanceData) {
        Assert-Stage5PerformanceProperties $Document.performanceData @(
            'sourceManifestPath', 'path', 'sha256', 'closureSha256',
            'runtimeRoot', 'fileCount', 'filePaths') `
            'Stage 5 host validation performance qualification data'
        Assert-Stage5PerformanceCondition ($mode -ceq 'External16Core' -and
            $Document.performanceData.sha256 -cmatch '^[0-9A-F]{64}$' -and
            $Document.performanceData.closureSha256 -cmatch '^[0-9A-F]{64}$' -and
            (Test-Stage5JsonInteger $Document.performanceData.fileCount) -and
            [int]$Document.performanceData.fileCount -ge 6 -and
            $Document.performanceData.filePaths -is [Array] -and
            $Document.performanceData.filePaths.Count -eq
                [int]$Document.performanceData.fileCount) `
            'Stage 5 host validation performance qualification-data binding is invalid.'
    }
    Assert-Stage5PerformanceCondition (@('External16Core', 'LocalCapacitySmoke',
        'InstalledKernelExecution') -ccontains $mode) `
        'Stage 5 host validation manifest qualification mode is invalid.'
    Assert-Stage5PerformanceCondition ($hasFixtureProduction -eq
        ($mode -ceq 'InstalledKernelExecution')) `
        'Only InstalledKernelExecution may carry, and must carry, native fixture-production evidence.'
    $laneNames = @(Get-Stage5LaneNames $mode)
    $laneWorkers = @(Get-Stage5LaneWorkers $mode)
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $Document.schemaVersion) -and
        $Document.schemaVersion -eq 1 -and
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
        ($mode -cne 'External16Core' -or
            $Document.stage3SourceCommit -cmatch '^[0-9a-f]{40}$') -and
        ($mode -cne 'External16Core' -or
            $Document.stage3BaselineSha256 -cmatch '^[0-9A-F]{64}$') -and
        [int]$Document.warmupRuns -eq 1 -and [int]$Document.measuredRuns -ge 3) `
        'Stage 5 host validation manifest identity is invalid.'
    Assert-Stage5PerformanceProperties $Document.runtimeClosure `
        @('dependencyManifestSha256', 'closureSha256') `
        'Stage 5 host validation runtime closure'
    Assert-Stage5PerformanceUnsignedFields $Document.topology @(
        'physicalCoreCount', 'logicalProcessorCount') `
        'Stage 5 host validation topology'
    Assert-Stage5PerformanceCondition ($Document.topology.source -ceq
        'GetSystemCpuSetInformation' -and
        (($mode -ceq 'External16Core' -and
            [int]$Document.topology.physicalCoreCount -ge 16 -and
            [int]$Document.topology.logicalProcessorCount -ge 16) -or
         (($mode -ceq 'LocalCapacitySmoke' -or
             $mode -ceq 'InstalledKernelExecution') -and
            [int]$Document.topology.physicalCoreCount -ge 4 -and
            [int]$Document.topology.physicalCoreCount -le 6 -and
            [int]$Document.topology.logicalProcessorCount -le 12))) `
        'Stage 5 performance validation topology does not match its qualification mode.'
    $fixtures = @($Document.fixtures)
    $stage3 = @($Document.stage3Fixtures)
    $expectedFixtureCount = if ($mode -ceq 'InstalledKernelExecution') {
        1
    } else { 4 }
    Assert-Stage5PerformanceCondition ($fixtures.Count -eq
        $expectedFixtureCount -and
        (($mode -ceq 'External16Core' -and $stage3.Count -eq 4) -or
         ($mode -cne 'External16Core' -and $stage3.Count -eq 0))) `
        'Stage 5 validation manifest has invalid current/Stage 3 fixture coverage.'
    for ($index = 0; $index -lt $fixtures.Count; ++$index) {
        Assert-Stage5PerformanceUnsignedFields $fixtures[$index] @('seed',
            'playerCount', 'peakUnitCount') `
            "Stage 5 validation manifest fixture $index"
        $expectedFixtureId = if ($mode -ceq 'InstalledKernelExecution') {
            'dense-eight-player'
        } else { $script:CanonicalFixtureIds[$index] }
        $expectedFixtureUnits = if ($mode -ceq 'InstalledKernelExecution') {
            8000
        } else { $script:CanonicalFixtureUnits[$index] }
        Assert-Stage5PerformanceCondition ($fixtures[$index].id -ceq
            $expectedFixtureId -and
            [int]$fixtures[$index].playerCount -eq 8 -and
            [int]$fixtures[$index].peakUnitCount -ge
                $expectedFixtureUnits -and
            ($mode -cne 'External16Core' -or
                ($stage3[$index].id -ceq $fixtures[$index].id -and
                    (Test-Stage5PerformanceFinitePositive `
                        $stage3[$index].measuredMedianMilliseconds)))) `
            "Stage 5 validation manifest fixture $index is not canonical."
    }
    if ($hasFixtureProduction) {
        Assert-Stage5PerformanceProperties $Document.fixtureProductionReceipt `
            @('path', 'sha256') 'Native fixture-production binding'
        $production = Read-Stage5NativePerformanceFixtureProductionReceipt `
            -Path ([string]$Document.fixtureProductionReceipt.path) `
            -ExpectedSha256 ([string]$Document.fixtureProductionReceipt.sha256) `
            -ExpectedTitle ([string]$Document.title) `
            -ExpectedCohortNonce ([string]$Document.cohortNonce) `
            -ExpectedCohortCreatedUtc ([string]$Document.cohortCreatedUtc) `
            -ExpectedSourceCommit ([string]$Document.sourceCommit) `
            -ExpectedArtifactSetSha256 ([string]$Document.artifactSetSha256) `
            -ExpectedExecutableSha256 ([string]$Document.executableSha256) `
            -ExpectedDependencyManifestSha256 `
                ([string]$Document.runtimeClosure.dependencyManifestSha256) `
            -ExpectedRuntimeClosureSha256 `
                ([string]$Document.runtimeClosure.closureSha256)
        Assert-Stage5PerformanceCondition (
            $production.path -ceq [string]$Document.fixtureManifestPath -and
            $production.sha256 -ceq [string]$Document.fixtureManifestSha256 -and
            $production.fixture.id -ceq [string]$fixtures[0].id -and
            $production.fixture.path -ceq [string]$fixtures[0].path -and
            $production.fixture.sha256 -ceq [string]$fixtures[0].sha256 -and
            [int]$production.fixture.seed -eq [int]$fixtures[0].seed -and
            [int]$production.fixture.playerCount -eq
                [int]$fixtures[0].playerCount -and
            [Int64]$production.fixture.peakUnitCount -eq
                [Int64]$fixtures[0].peakUnitCount) `
            'Installed kernel fixture differs from its native production receipt.'
    }
    $expectedPerLane = 1 + [int]$Document.measuredRuns
    $expectedTotal = $fixtures.Count * $laneNames.Count * $expectedPerLane
    $runs = @($Document.runs)
    Assert-Stage5PerformanceCondition ($runs.Count -eq $expectedTotal) `
        "Stage 5 run schedule requires exactly $expectedTotal runs."
    $seenRunIds = @{}
    $seenRunNonces = @{}
    $seenReceiptPaths = @{}
    $seenReceiptHashes = @{}
    $validated = @()
    $validatedByRunId = @{}
    foreach ($run in $runs) {
        Assert-Stage5PerformanceUnsignedFields $run @('ordinal') `
            'Stage 5 scheduled run'
        Assert-Stage5PerformanceBooleanFields $run @('warmup') `
            'Stage 5 scheduled run'
    }
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
    $validatedPhasePairs = @()
    if ($hasPhasePolicy) {
        $validatedPhasePairs = @(Assert-Stage5PhaseCohort $Document $seenRunIds $seenRunNonces `
            $seenReceiptPaths $seenReceiptHashes)
    }
    $fixtureResults = @()
    for ($fixtureIndex = 0; $fixtureIndex -lt $fixtures.Count; ++$fixtureIndex) {
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
                qualificationClass = if ($mode -ceq
                    'InstalledKernelExecution') {
                    'installed-kernel-execution-only'
                } else { 'local-capacity-smoke' }
            }
        }
    }
    $result = [pscustomobject]@{
        qualificationMode = $mode
        referencePolicy = $referencePolicy
        runs = $validated
        pairedOracleBindings = $validatedPairs
        fixtures = $fixtureResults
    }
    if ($hasPhasePolicy) {
        $result | Add-Member NoteProperty phaseBaselinePolicy $Document.phaseBaselinePolicy
        $result | Add-Member NoteProperty pairedPhaseBaselineBindings $validatedPhasePairs
    }
    return $result
}

function New-Stage5PerformanceRunPlan {
    param([object]$Context, [int]$Timeout,
        [object]$TitleSessionContract, [object[]]$PhaseProfiles)
    if ($null -eq $PhaseProfiles) { $PhaseProfiles = @() }
    Assert-Stage5PerformanceCondition ($Timeout -gt 0 -and $Context.warmupRuns -eq 1 -and
        (Test-Stage5JsonInteger $Context.measuredRuns) -and $Context.measuredRuns -gt 0) `
        'Prelaunch timeout/repeat inputs are invalid.'
    $lanes = @(Get-Stage5LaneNames $Context.qualificationMode)
    $workers = @(Get-Stage5LaneWorkers $Context.qualificationMode)
    $profilesByLane = @{}
    foreach ($profile in @($PhaseProfiles)) {
        $key = [string]$profile.fixtureId + '|' + [string]$profile.sourceLane
        Assert-Stage5PerformanceCondition (-not $profilesByLane.ContainsKey($key)) `
            'A phase source lane cannot be selected twice.'
        $profilesByLane[$key] = $profile
    }
    $entries = New-Object 'Collections.Generic.List[object]'
    foreach ($fixture in $Context.fixtures) {
        for ($laneIndex = 0; $laneIndex -lt $lanes.Count; ++$laneIndex) {
            $lane = $lanes[$laneIndex]
            $key = [string]$fixture.id + '|' + $lane
            $selected = $profilesByLane.ContainsKey($key)
            for ($ordinal = 0; $ordinal -lt 1 + $Context.measuredRuns; ++$ordinal) {
                $roles = @('throughput')
                if ($Context.referencePolicy -ceq 'paired-serial-oracle-v1') { $roles += 'serial-oracle' }
                if ($selected) { $roles += 'phase-serial-baseline' }
                $sourceId = $null
                foreach ($role in $roles) {
                    $nonce = [Guid]::NewGuid().ToString()
                    $id = 's5perf-{0}-{1}-{2}-{3}' -f $fixture.id, $lane, $ordinal, $nonce
                    $runRoot = Join-Path $Context.taskRoot $id
                    $receiptDirectory = Join-Path $runRoot 'receipt'
                    $entry = [pscustomobject][ordered]@{
                        entryId=$id; measurementRole=$role
                        profileId=if ($role -ceq 'phase-serial-baseline') { $profilesByLane[$key].profileId } else { $null }
                        fixtureId=$fixture.id; lane=$lane; ordinal=$ordinal; warmup=($ordinal -eq 0)
                        sourceEntryId=if ($role -ceq 'throughput') { $null } else { $sourceId }
                        runNonce=$nonce; workerCount=$workers[$laneIndex]
                        expectedArgumentString=(Get-Stage5PerformanceArguments $fixture $workers[$laneIndex] $Context.executableSha256)
                        outputPaths=[pscustomobject][ordered]@{
                            runRoot=$runRoot; receiptDirectory=$receiptDirectory
                            rawLogPath=(Join-Path $runRoot 'game-owned-raw.log')
                            timingDirectory=(Join-Path $runRoot 'timing')
                            stdoutPath=(Join-Path $runRoot 'host-stdout.log'); stderrPath=(Join-Path $runRoot 'host-stderr.log')
                            tempDirectory=(Join-Path $runRoot 'temp')
                            attemptTracePath=if ($role -ceq 'throughput' -and $selected) { Join-Path $receiptDirectory 'attempt-trace.bin' } else { $null }
                            attemptStartPath=(Join-Path $Context.taskRoot ('attempts/' + $id + '.start.json'))
                            attemptResultPath=(Join-Path $Context.taskRoot ('attempts/' + $id + '.result.json'))
                            sourceBindingPath=if ($role -ceq 'phase-serial-baseline') { Join-Path $Context.taskRoot ('bindings/' + $id + '.source.json') } else { $null }
                        }
                    }
                    $entries.Add($entry) | Out-Null
                    if ($role -ceq 'throughput') { $sourceId = $id }
                }
            }
        }
    }
    $plan = [pscustomobject][ordered]@{
        schemaVersion=1; title=$Context.title; qualificationMode=$Context.qualificationMode; taskRoot=$Context.taskRoot
        cohortNonce=$Context.cohortNonce; cohortCreatedUtc=$Context.cohortCreatedUtc; sourceCommit=$Context.sourceCommit
        executablePath=$Context.executablePath; executableSha256=$Context.executableSha256
        artifactSetSha256=$Context.artifactSetSha256; runtimeClosure=$Context.runtimeClosure
        fixtureManifestPath=$Context.fixtureManifestPath; fixtureManifestSha256=$Context.fixtureManifestSha256
        fixtures=$Context.fixtures; referencePolicy=$Context.referencePolicy
        warmupRuns=$Context.warmupRuns; measuredRuns=$Context.measuredRuns; timeoutSeconds=$Timeout
        titleSessionEnvironment=$TitleSessionContract.environmentValues; phaseBaselineProfiles=@($PhaseProfiles)
        outputFilePolicy='native-runid-pid-receipt-pid-tick-timing-v1'; entries=$entries.ToArray()
    }
    if ($Context.PSObject.Properties.Name -ccontains 'performanceData') {
        $plan | Add-Member NoteProperty performanceData ([pscustomobject][ordered]@{
            path = [string]$Context.performanceData.path
            sha256 = [string]$Context.performanceData.sha256
            closureSha256 = [string]$Context.performanceData.closureSha256
            fileCount = [int]$Context.performanceData.fileCount
        })
    }
    $path = Join-Path $Context.taskRoot 'phase-plan.json'
    Write-Stage5JsonAtomically $path $plan -CreateNew
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $path 'Original prelaunch plan'
    $binding = [pscustomobject]@{ path=$path; sha256=$snapshot.sha256 }
    # Reopen the published capability and validate every declared role before a
    # caller can receive it. No role output directory exists at this boundary.
    Resolve-Stage5PlannedPerformanceLaunch $Context $binding $entries[0].entryId $TitleSessionContract | Out-Null
    return $binding
}

function Resolve-Stage5PlannedPerformanceLaunch {
    param([object]$Context, [object]$PlanBinding, [string]$EntryId,
        [object]$TitleSessionContract)
    $plan = Read-Stage5PhaseBoundJson $PlanBinding $Context.taskRoot 'Frozen prelaunch plan'
    $planProperties = @('schemaVersion','title','qualificationMode','taskRoot',
        'cohortNonce','cohortCreatedUtc','sourceCommit','executablePath','executableSha256','artifactSetSha256',
        'runtimeClosure','fixtureManifestPath','fixtureManifestSha256','fixtures','referencePolicy','warmupRuns',
        'measuredRuns','timeoutSeconds','titleSessionEnvironment','phaseBaselineProfiles','outputFilePolicy','entries')
    $hasPerformanceData = $plan.PSObject.Properties.Name -ccontains
        'performanceData'
    Assert-Stage5PerformanceCondition ($hasPerformanceData -eq
        ($Context.PSObject.Properties.Name -ccontains 'performanceData')) `
        'Frozen prelaunch performance qualification-data presence changed.'
    if ($hasPerformanceData) { $planProperties += 'performanceData' }
    Assert-Stage5PerformanceProperties $plan $planProperties 'Frozen prelaunch plan'
    Assert-Stage5PerformanceCondition ($PlanBinding.path -ceq (Join-Path $Context.taskRoot 'phase-plan.json') -and
        (Test-Stage5JsonInteger $plan.schemaVersion) -and $plan.schemaVersion -eq 1 -and
        $plan.outputFilePolicy -is [string] -and $plan.outputFilePolicy -ceq 'native-runid-pid-receipt-pid-tick-timing-v1' -and
        $plan.entries -is [Array] -and $plan.entries.Count -gt 0 -and $plan.fixtures -is [Array] -and
        $plan.phaseBaselineProfiles -is [Array]) 'Frozen prelaunch plan shape/path is invalid.'
    foreach ($field in @('title','qualificationMode','taskRoot','cohortNonce','cohortCreatedUtc','sourceCommit',
            'executablePath','executableSha256','artifactSetSha256','fixtureManifestPath','fixtureManifestSha256','referencePolicy')) {
        Assert-Stage5PerformanceCondition ($plan.$field -is [string] -and $Context.$field -is [string] -and
            $plan.$field -ceq $Context.$field) "Frozen prelaunch plan changed $field."
    }
    Assert-Stage5PerformanceCondition (@('Generals','ZeroHour') -ccontains $plan.title -and
        @('LocalCapacitySmoke','External16Core',
            'InstalledKernelExecution') -ccontains $plan.qualificationMode -and
        @('throughput-only','paired-serial-oracle-v1') -ccontains $plan.referencePolicy -and
        $plan.sourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $plan.cohortNonce -cmatch '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
        $plan.cohortCreatedUtc -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$' -and
        (Test-Stage5SafeTitleSessionPath $plan.taskRoot)) 'Frozen prelaunch identity is invalid.'
    foreach ($field in @('warmupRuns','measuredRuns','timeoutSeconds')) {
        Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $plan.$field) -and
            $plan.$field -gt 0 -and $plan.$field -le [int]::MaxValue) "Frozen prelaunch $field is invalid."
    }
    Assert-Stage5PerformanceCondition ($plan.warmupRuns -eq 1 -and $plan.warmupRuns -eq $Context.warmupRuns -and
        $plan.measuredRuns -eq $Context.measuredRuns) 'Frozen prelaunch repeats changed.'
    Assert-Stage5PerformanceProperties $plan.runtimeClosure @('dependencyManifestSha256','closureSha256') 'Frozen runtime closure'
    foreach ($field in @('dependencyManifestSha256','closureSha256')) {
        Assert-Stage5PerformanceHash $plan.runtimeClosure.$field "Frozen runtime $field"
        Assert-Stage5PerformanceCondition ($plan.runtimeClosure.$field -ceq $Context.runtimeClosure.$field) `
            "Frozen prelaunch runtime $field changed."
    }
    $inputs = Read-Stage5PlannedPerformanceInputs $Context $PlanBinding $plan
    Assert-Stage5PerformanceCondition ($inputs.runtimeClosure.dependencyManifestSha256 -ceq $plan.runtimeClosure.dependencyManifestSha256 -and
        $inputs.runtimeClosure.closureSha256 -ceq $plan.runtimeClosure.closureSha256) 'Frozen runtime closure no longer matches its artifact.'
    if ($hasPerformanceData) {
        Assert-Stage5PerformanceProperties $plan.performanceData @('path',
            'sha256', 'closureSha256', 'fileCount') `
            'Frozen performance qualification-data binding'
        Assert-Stage5PerformanceCondition ($null -ne $inputs.performanceData -and
            (ConvertTo-Json $inputs.performanceData -Compress -Depth 5) -ceq
                (ConvertTo-Json $plan.performanceData -Compress -Depth 5)) `
            'Frozen performance qualification-data no longer matches its held immutable inputs.'
    }
    Assert-Stage5PerformanceCondition ((ConvertTo-Json @($plan.fixtures) -Depth 20 -Compress) -ceq
        (ConvertTo-Json @($inputs.fixtures) -Depth 20 -Compress) -and
        (ConvertTo-Json @($plan.fixtures) -Depth 20 -Compress) -ceq
        (ConvertTo-Json @($Context.fixtures) -Depth 20 -Compress)) 'Frozen fixture schedule changed.'
    $canonicalSession = New-Stage5TitleSessionContract $plan.title (Join-Path $plan.taskRoot 'TitleSession') `
        (Split-Path -Parent $plan.executablePath) $plan.taskRoot
    Assert-Stage5PerformanceCondition ($null -ne $TitleSessionContract -and $TitleSessionContract.title -ceq $plan.title -and
        $TitleSessionContract.sessionRoot -ceq $canonicalSession.sessionRoot) 'Frozen title-session identity changed.'
    $environmentNames = @($canonicalSession.environmentValues.Keys)
    Assert-Stage5PerformanceProperties $plan.titleSessionEnvironment $environmentNames 'Frozen title environment'
    Assert-Stage5PerformanceCondition ($TitleSessionContract.environmentValues.Count -eq $environmentNames.Count) `
        'Title-session environment has unplanned fields.'
    foreach ($name in $environmentNames) {
        Assert-Stage5PerformanceCondition ($plan.titleSessionEnvironment.$name -is [string] -and
            $plan.titleSessionEnvironment.$name -ceq $canonicalSession.environmentValues[$name] -and
            $TitleSessionContract.environmentValues[$name] -ceq $canonicalSession.environmentValues[$name]) `
            "Frozen title environment changed $name."
    }
    $lanes = @(Get-Stage5LaneNames $plan.qualificationMode)
    $workers = @(Get-Stage5LaneWorkers $plan.qualificationMode)
    $profilesByLane = @{}; $profileIds = @{}
    foreach ($profile in $plan.phaseBaselineProfiles) {
        Assert-Stage5PerformanceProperties $profile @('profileId','fixtureId','sourceLane','sourcePolicySha256',
            'limits','residentAttemptCapacity','residentRangeCapacity','fixtureSha256','window','warmupRuns','measuredRuns') 'Frozen phase profile'
        Assert-Stage5PerformanceProperties $profile.window @('firstCompletedFrame','lastCompletedFrame','completedFrameCount','controlWindowCount') 'Frozen phase window'
        Assert-Stage5PerformanceProperties $profile.limits @('maximumBytes','maximumRecords','maximumLogicalEvents','maximumAttempts','maximumRanges') 'Frozen phase limits'
        foreach ($field in @('profileId','fixtureId','sourceLane')) {
            Assert-Stage5PerformanceCondition ($profile.$field -is [string] -and $profile.$field.Length -gt 0) `
                "Frozen phase profile $field is invalid."
        }
        foreach ($field in @('sourcePolicySha256','fixtureSha256')) { Assert-Stage5PerformanceHash $profile.$field "Frozen phase $field" }
        foreach ($field in @('warmupRuns','measuredRuns','residentAttemptCapacity','residentRangeCapacity')) {
            Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $profile.$field) "Frozen phase $field is not an exact integer."
            Get-Stage5UnsignedCounter $profile.$field "Frozen phase $field" $false | Out-Null
        }
        foreach ($group in @('window','limits')) {
            foreach ($field in $profile.$group.PSObject.Properties.Name) {
                Assert-Stage5PerformanceCondition (Test-Stage5JsonInteger $profile.$group.$field) "Frozen phase $group/$field is not exact."
                Get-Stage5UnsignedCounter $profile.$group.$field "Frozen phase $group/$field" $false | Out-Null
            }
        }
        $fixtures = @($plan.fixtures | Where-Object { $_.id -ceq $profile.fixtureId })
        $key = $profile.fixtureId + '|' + $profile.sourceLane
        Assert-Stage5PerformanceCondition (-not $profileIds.ContainsKey($profile.profileId) -and
            -not $profilesByLane.ContainsKey($key) -and $fixtures.Count -eq 1 -and
            $lanes -ccontains $profile.sourceLane -and $profile.fixtureSha256 -ceq $fixtures[0].sha256 -and
            $profile.warmupRuns -eq $plan.warmupRuns -and $profile.measuredRuns -eq $plan.measuredRuns -and
            $profile.window.lastCompletedFrame -le [UInt32]::MaxValue -and
            [decimal]$profile.window.lastCompletedFrame - [decimal]$profile.window.firstCompletedFrame + 1 -eq
                [decimal]$profile.window.completedFrameCount) 'Frozen phase source selection/repeats/window are invalid.'
        $profilesByLane[$key] = $profile; $profileIds[$profile.profileId] = $true
    }
    if ($Context.PSObject.Properties.Name -ccontains 'phaseBaselineProfiles') {
        Assert-Stage5PerformanceCondition ((ConvertTo-Json @($plan.phaseBaselineProfiles) -Depth 20 -Compress) -ceq
            (ConvertTo-Json @($Context.phaseBaselineProfiles) -Depth 20 -Compress)) 'Frozen phase profiles changed.'
    }
    $byId = @{}; $nonces = @{}; $entryIndex = 0; $resolvedFixture = $null
    foreach ($fixture in $plan.fixtures) {
        for ($laneIndex = 0; $laneIndex -lt $lanes.Count; ++$laneIndex) {
            $lane = $lanes[$laneIndex]; $key = $fixture.id + '|' + $lane
            $selected = $profilesByLane.ContainsKey($key)
            for ($ordinal = 0; $ordinal -lt 1 + $plan.measuredRuns; ++$ordinal) {
                $roles = @('throughput')
                if ($plan.referencePolicy -ceq 'paired-serial-oracle-v1') { $roles += 'serial-oracle' }
                if ($selected) { $roles += 'phase-serial-baseline' }
                $sourceId = $null
                foreach ($role in $roles) {
                    Assert-Stage5PerformanceCondition ($entryIndex -lt $plan.entries.Count) 'Frozen schedule dropped a role.'
                    $entry = $plan.entries[$entryIndex]; ++$entryIndex
                    Assert-Stage5PerformanceProperties $entry @('entryId','measurementRole','profileId','fixtureId','lane',
                        'ordinal','warmup','sourceEntryId','runNonce','workerCount','expectedArgumentString','outputPaths') 'Frozen role'
                    Assert-Stage5PerformanceCondition ($entry.entryId -is [string] -and
                        $entry.entryId -cmatch '^[A-Za-z0-9_.-]{1,256}$' -and -not $entry.entryId.Contains('..') -and
                        $entry.runNonce -is [string] -and
                        $entry.runNonce -cmatch '^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$' -and
                        -not $byId.ContainsKey($entry.entryId) -and -not $nonces.ContainsKey($entry.runNonce) -and
                        $entry.measurementRole -is [string] -and $entry.measurementRole -ceq $role -and
                        $entry.fixtureId -is [string] -and $entry.fixtureId -ceq $fixture.id -and
                        $entry.lane -is [string] -and $entry.lane -ceq $lane -and
                        (Test-Stage5JsonInteger $entry.ordinal) -and $entry.ordinal -eq $ordinal -and
                        $entry.warmup -is [bool] -and $entry.warmup -eq ($ordinal -eq 0) -and
                        (Test-Stage5JsonInteger $entry.workerCount) -and $entry.workerCount -eq $workers[$laneIndex] -and
                        $entry.expectedArgumentString -is [string] -and $entry.expectedArgumentString -ceq
                            (Get-Stage5PerformanceArguments $fixture $workers[$laneIndex] $plan.executableSha256)) 'Frozen role identity/order/command changed.'
                    $expectedSource = if ($role -ceq 'throughput') { $null } else { $sourceId }
                    $expectedProfile = if ($role -ceq 'phase-serial-baseline') { $profilesByLane[$key].profileId } else { $null }
                    Assert-Stage5PerformanceCondition (($null -eq $entry.sourceEntryId -or $entry.sourceEntryId -is [string]) -and
                        ($null -eq $entry.profileId -or $entry.profileId -is [string]) -and
                        $entry.sourceEntryId -ceq $expectedSource -and $entry.profileId -ceq $expectedProfile) 'Frozen role dependency changed.'
                    $runRoot = Join-Path $plan.taskRoot $entry.entryId
                    $receiptDirectory = Join-Path $runRoot 'receipt'
                    $paths = [ordered]@{
                        runRoot=$runRoot; receiptDirectory=$receiptDirectory; rawLogPath=(Join-Path $runRoot 'game-owned-raw.log')
                        timingDirectory=(Join-Path $runRoot 'timing'); stdoutPath=(Join-Path $runRoot 'host-stdout.log')
                        stderrPath=(Join-Path $runRoot 'host-stderr.log'); tempDirectory=(Join-Path $runRoot 'temp')
                        attemptTracePath=if ($role -ceq 'throughput' -and $selected) { Join-Path $receiptDirectory 'attempt-trace.bin' } else { $null }
                        attemptStartPath=(Join-Path $plan.taskRoot ('attempts/' + $entry.entryId + '.start.json'))
                        attemptResultPath=(Join-Path $plan.taskRoot ('attempts/' + $entry.entryId + '.result.json'))
                        sourceBindingPath=if ($role -ceq 'phase-serial-baseline') { Join-Path $plan.taskRoot ('bindings/' + $entry.entryId + '.source.json') } else { $null }
                    }
                    Assert-Stage5PerformanceProperties $entry.outputPaths @($paths.Keys) 'Frozen output paths'
                    foreach ($name in $paths.Keys) {
                        Assert-Stage5PerformanceCondition (($null -eq $entry.outputPaths.$name -or $entry.outputPaths.$name -is [string]) -and
                            $entry.outputPaths.$name -ceq $paths[$name]) "Frozen role output $name changed."
                        if ($null -ne $paths[$name]) {
                            Assert-Stage5FinalAcceptancePathContained $plan.taskRoot $paths[$name] 'Frozen role output'
                            $ancestor = $paths[$name]
                            while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor = Split-Path -Parent $ancestor }
                            if ($ancestor -cne $plan.taskRoot) {
                                Assert-Stage5FinalAcceptanceNoReparsePath $plan.taskRoot $ancestor 'Frozen role existing output ancestor'
                            }
                        }
                    }
                    $byId[$entry.entryId] = $entry; $nonces[$entry.runNonce] = $true
                    if ($entry.entryId -ceq $EntryId) { $resolvedFixture = $fixture }
                    if ($role -ceq 'throughput') { $sourceId = $entry.entryId }
                }
            }
        }
    }
    Assert-Stage5PerformanceCondition ($entryIndex -eq $plan.entries.Count -and $byId.ContainsKey($EntryId)) `
        'Frozen schedule added a role or does not contain the requested entry.'
    $entry = $byId[$EntryId]
    $environment = @{}
    foreach ($name in $environmentNames) { $environment[$name] = [string]$plan.titleSessionEnvironment.$name }
    $roleEnvironment = @{
        RTS_PERFORMANCE_ROLE='performance-report'
        RTS_PERFORMANCE_REFERENCE_MODE=if ($entry.measurementRole -ceq 'phase-serial-baseline') { 'phase-baseline-binding' } elseif ($entry.measurementRole -ceq 'serial-oracle') { 'serial-oracle' } else { 'throughput-binding' }
        RTS_PERFORMANCE_RUN_ID=$entry.entryId; RTS_PERFORMANCE_RUN_NONCE=$entry.runNonce
        RTS_PERFORMANCE_COHORT_NONCE=$plan.cohortNonce; RTS_PERFORMANCE_COHORT_CREATED_UTC=$plan.cohortCreatedUtc
        RTS_PERFORMANCE_RECEIPT_DIR=$entry.outputPaths.receiptDirectory; RTS_PERFORMANCE_SOURCE_COMMIT=$plan.sourceCommit
        RTS_PERFORMANCE_ARTIFACT_SET_SHA256=$plan.artifactSetSha256
        RTS_PERFORMANCE_RUNTIME_MANIFEST_SHA256=$plan.runtimeClosure.dependencyManifestSha256
        RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256=$plan.runtimeClosure.closureSha256
        RTS_PERFORMANCE_FIXTURE_ID=$resolvedFixture.id; RTS_PERFORMANCE_FIXTURE_SHA256=$resolvedFixture.sha256
        RTS_PERFORMANCE_FIXTURE_KIND='replay'; RTS_PERFORMANCE_WORKLOAD_QUALIFICATION='minimum-qualified'
        RTS_PERFORMANCE_RAW_LOG_PATH=$entry.outputPaths.rawLogPath; RTS_PERFORMANCE_TIMING_PATH=$entry.outputPaths.timingDirectory
        RTS_PERFORMANCE_VERIFIER_BOUNDARY=$script:VerifierBoundary; RTS_PERFORMANCE_SEED=[string]$resolvedFixture.seed
        RTS_PERFORMANCE_PLAYER_COUNT=[string]$resolvedFixture.playerCount; RTS_PERFORMANCE_UNIT_COUNT=[string]$resolvedFixture.peakUnitCount
        RTS_STAGE5_RUN_NONCE=$entry.runNonce; RTS_STAGE5_COHORT_NONCE=$plan.cohortNonce; RTS_STAGE5_COHORT_CREATED_UTC=$plan.cohortCreatedUtc
        RTS_STAGE5_RUNTIME_MANIFEST_SHA256=$plan.runtimeClosure.dependencyManifestSha256
        RTS_STAGE5_RUNTIME_CLOSURE_SHA256=$plan.runtimeClosure.closureSha256
        RTS_FRAME_TIMING_DIR=$entry.outputPaths.timingDirectory; TEMP=$entry.outputPaths.tempDirectory; TMP=$entry.outputPaths.tempDirectory
    }
    if ($null -ne $entry.outputPaths.attemptTracePath) {
        $roleEnvironment.RTS_PERFORMANCE_ATTEMPT_TRACE_PATH = $entry.outputPaths.attemptTracePath
    }
    if ($entry.measurementRole -ceq 'phase-serial-baseline') {
        $sourceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.outputPaths.sourceBindingPath 'Planned source audit binding'
        $source = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $sourceSnapshot 'Planned source audit binding' -AsPsObject
        Assert-Stage5PerformanceProperties $source @('receipt','runId','runNonce','processId','processCreationTimeUtc100ns') 'Planned source audit identity'
        $sourceEntry = $byId[$entry.sourceEntryId]
        Assert-Stage5PerformanceCondition ($source.runId -is [string] -and $source.runNonce -is [string] -and
            $source.runId -ceq $sourceEntry.entryId -and $source.runNonce -ceq $sourceEntry.runNonce) `
            'Planned baseline source was substituted.'
        Get-Stage5PhaseRunIdentitySha256 $source.runId $source.runNonce $source.processId $source.processCreationTimeUtc100ns | Out-Null
        $resultSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $sourceEntry.outputPaths.attemptResultPath 'Planned source completed result'
        $sourceResult = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $resultSnapshot 'Planned source completed result' -AsPsObject
        Assert-Stage5PerformanceProperties $sourceResult @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc',
            'startBinding','state','failure','run','processCleanup') 'Planned source completed result'
        Assert-Stage5PerformanceCondition ($sourceResult.event -ceq 'attempt-result' -and $sourceResult.state -ceq 'completed' -and
            $null -eq $sourceResult.failure -and $sourceResult.planSha256 -ceq $PlanBinding.sha256 -and
            $sourceResult.entryId -ceq $sourceEntry.entryId -and $sourceResult.runNonce -ceq $sourceEntry.runNonce -and
            $sourceResult.run.receiptPath -ceq $source.receipt.path -and $sourceResult.run.receiptSha256 -ceq $source.receipt.sha256 -and
            $sourceResult.run.host.processId -eq $source.processId -and
            $sourceResult.run.host.creationTimeUtc100ns -eq $source.processCreationTimeUtc100ns -and
            $sourceResult.startBinding.path -ceq $sourceEntry.outputPaths.attemptStartPath -and
            $sourceResult.processCleanup.exitProof -and -not $sourceResult.processCleanup.blocked -and
            @($sourceResult.processCleanup.errors).Count -eq 0) 'Baseline source audit is not bound to its completed original attempt.'
        Read-Stage5PhaseBoundJson $sourceResult.startBinding $plan.taskRoot 'Planned source original start' | Out-Null
        $native = Read-Stage5PhaseBoundJson $source.receipt $plan.taskRoot 'Planned baseline native source'
        Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $native.schemaVersion) -and
            $source.receipt.path -ceq (Join-Path $sourceEntry.outputPaths.receiptDirectory `
            ('performance-receipt-{0}-{1}.json' -f $source.runId, $source.processId)) -and
            $native.schemaVersion -eq 6 -and $native.measurementRole -ceq 'throughput' -and
            $native.runId -ceq $sourceEntry.entryId -and $native.runNonce -ceq $sourceEntry.runNonce -and
            $native.process.id -eq $source.processId -and $native.process.creationTimeUtc100ns -eq $source.processCreationTimeUtc100ns -and
            $native.attemptTrace.file.path -ceq $sourceEntry.outputPaths.attemptTracePath) 'Planned baseline source receipt/bundle identity changed.'
        Assert-Stage5PhaseTraceContract $native $Context 'Planned baseline source trace'
        $roleEnvironment.RTS_PERFORMANCE_SOURCE_RECEIPT_PATH = $source.receipt.path
        $roleEnvironment.RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256 = $source.receipt.sha256
    }
    foreach ($name in $roleEnvironment.Keys) { $environment[$name] = [string]$roleEnvironment[$name] }
    $info = New-Stage5ProcessStartInfo $plan.executablePath $entry.expectedArgumentString `
        (Split-Path -Parent $plan.executablePath) $environment
    return [pscustomobject]@{ entry=$entry; fixture=$resolvedFixture; startInfo=$info }
}

function Invoke-Stage5PerformanceRunPlan {
    param([object]$Context, [object]$PlanBinding,
        [object]$TitleSessionContract)
    $plan = Read-Stage5PhaseBoundJson $PlanBinding $Context.taskRoot 'Execution original plan'
    Resolve-Stage5PlannedPerformanceLaunch $Context $PlanBinding $plan.entries[0].entryId $TitleSessionContract | Out-Null
    foreach ($leaf in @('attempts','bindings')) {
        $directory = Join-Path $Context.taskRoot $leaf
        New-Item -ItemType Directory -Path $directory | Out-Null
        Assert-Stage5FinalAcceptanceNoReparsePath $Context.taskRoot $directory 'Prelaunch journal directory'
    }
    $runs = New-Object 'Collections.Generic.List[object]'
    $oracles = New-Object 'Collections.Generic.List[object]'
    $baselines = New-Object 'Collections.Generic.List[object]'
    $outcomes = New-Object 'Collections.Generic.List[object]'
    $completedById = @{}
    $seenIds = @{}; $seenNonces = @{}; $seenPaths = @{}; $seenHashes = @{}
    $failure = $null
    foreach ($entry in $plan.entries) {
        $startBinding = $null; $sourceBinding = $null; $run = $null; $cleanup = $null
        $entryFailure = $null; $state = 'not-attempted'
        if ($null -ne $failure) {
            $entryFailure = [pscustomobject]@{ stage='not-attempted'; message=('Blocked by earlier cohort failure: ' + $failure.message) }
        }
        else {
            $stage = 'prelaunch'
            try {
                if ($entry.measurementRole -ceq 'phase-serial-baseline') {
                    Assert-Stage5PerformanceCondition ($completedById.ContainsKey($entry.sourceEntryId)) `
                        'The planned phase source did not complete before its baseline.'
                    $sourceRun = $completedById[$entry.sourceEntryId]
                    $native = Read-Stage5PhaseBoundJson ([pscustomobject]@{
                        path=$sourceRun.receiptPath; sha256=$sourceRun.receiptSha256
                    }) $Context.taskRoot 'Completed planned phase source'
                    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $native.schemaVersion) -and
                        $native.schemaVersion -eq 6 -and $native.measurementRole -ceq 'throughput' -and
                        $native.runId -ceq $entry.sourceEntryId -and $native.runNonce -ceq $sourceRun.runNonce) `
                        'A baseline cannot bind an unplanned or downgraded source.'
                    Assert-Stage5PhaseTraceContract $native $Context 'Completed planned phase source trace'
                    $source = [pscustomobject][ordered]@{
                        receipt=[pscustomobject]@{ path=$sourceRun.receiptPath; sha256=$sourceRun.receiptSha256 }
                        runId=$native.runId; runNonce=$native.runNonce; processId=$native.process.id
                        processCreationTimeUtc100ns=$native.process.creationTimeUtc100ns
                    }
                    Write-Stage5JsonAtomically $entry.outputPaths.sourceBindingPath $source -CreateNew
                    $sourceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.outputPaths.sourceBindingPath 'Published source audit binding'
                    $sourceBinding = [pscustomobject]@{ path=$entry.outputPaths.sourceBindingPath; sha256=$sourceSnapshot.sha256 }
                }
                Resolve-Stage5PlannedPerformanceLaunch $Context $PlanBinding $entry.entryId $TitleSessionContract | Out-Null
                $stage = 'attempt-start'
                $start = [pscustomobject][ordered]@{
                    schemaVersion=1; event='attempt-start'; planSha256=$PlanBinding.sha256
                    entryId=$entry.entryId; runNonce=$entry.runNonce; recordedUtc=[DateTime]::UtcNow.ToString('o')
                    sourceBinding=$sourceBinding
                }
                Write-Stage5JsonAtomically $entry.outputPaths.attemptStartPath $start -CreateNew
                $startSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.outputPaths.attemptStartPath 'Published attempt start'
                $startBinding = [pscustomobject]@{ path=$entry.outputPaths.attemptStartPath; sha256=$startSnapshot.sha256 }
                $published = Read-Stage5PhaseBoundJson $startBinding $Context.taskRoot 'Durable attempt start'
                Assert-Stage5PerformanceCondition ((ConvertTo-Json $published -Depth 20 -Compress) -ceq
                    (ConvertTo-Json $start -Depth 20 -Compress)) 'Published attempt start differs from its original declaration.'
                $Context.processCleanup = [pscustomobject]@{ processId=0; exitProof=$true; blocked=$false; errors=@() }
                $stage = 'execution'
                $run = Invoke-Stage5InstalledPerformanceRun $Context $PlanBinding $entry.entryId $TitleSessionContract
                $stage = 'receipt-validation'
                Assert-Stage5PerformanceCondition ($run.runId -ceq $entry.entryId -and $run.runNonce -ceq $entry.runNonce -and
                    $run.fixtureId -ceq $entry.fixtureId -and $run.lane -ceq $entry.lane -and $run.ordinal -eq $entry.ordinal -and
                    $run.warmup -eq $entry.warmup -and $run.expectedArgumentString -ceq $entry.expectedArgumentString) `
                    'Installed attempt returned an identity outside its original planned role.'
                Assert-Stage5Receipt $run $Context $seenIds $seenNonces $seenPaths $seenHashes $entry.measurementRole | Out-Null
                Assert-Stage5PerformanceCondition ($Context.processCleanup.exitProof -is [bool] -and $Context.processCleanup.exitProof -and
                    $Context.processCleanup.blocked -is [bool] -and -not $Context.processCleanup.blocked -and
                    @($Context.processCleanup.errors).Count -eq 0 -and $Context.processCleanup.processId -eq $run.host.processId) `
                    'Completed attempt lacks its original owned-child cleanup proof.'
                $state = 'completed'
                $completedById[$entry.entryId] = $run
                if ($entry.measurementRole -ceq 'throughput') { $runs.Add($run) | Out-Null }
                elseif ($entry.measurementRole -ceq 'serial-oracle') {
                    $oracles.Add([pscustomobject]@{ throughputRunId=$entry.sourceEntryId; oracleRun=$run }) | Out-Null
                }
                else {
                    $baselines.Add([pscustomobject]@{ profileId=$entry.profileId; throughputRunId=$entry.sourceEntryId; baselineRun=$run }) | Out-Null
                }
            }
            catch {
                $state = 'failed'
                $entryFailure = [pscustomobject]@{ stage=$stage; message=("Entry '$($entry.entryId)': " + $_.Exception.Message) }
                $failure = $entryFailure
            }
            if ($null -ne $startBinding) { $cleanup = $Context.processCleanup | Select-Object -Property * }
        }
        $result = [pscustomobject][ordered]@{
            schemaVersion=1; event='attempt-result'; planSha256=$PlanBinding.sha256
            entryId=$entry.entryId; runNonce=$entry.runNonce; recordedUtc=[DateTime]::UtcNow.ToString('o')
            startBinding=$startBinding; state=$state; failure=$entryFailure; run=$run; processCleanup=$cleanup
        }
        $resultBinding = $null
        try {
            Write-Stage5JsonAtomically $entry.outputPaths.attemptResultPath $result -CreateNew
            $resultSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.outputPaths.attemptResultPath 'Published attempt result'
            $resultBinding = [pscustomobject]@{ path=$entry.outputPaths.attemptResultPath; sha256=$resultSnapshot.sha256 }
            $published = Read-Stage5PhaseBoundJson $resultBinding $Context.taskRoot 'Durable attempt result'
            Assert-Stage5PerformanceCondition ((ConvertTo-Json $published -Depth 20 -Compress) -ceq
                (ConvertTo-Json $result -Depth 20 -Compress)) 'Published attempt result differs from its observed outcome.'
        }
        catch {
            $journalMessage = "Entry '$($entry.entryId)' result publication: $($_.Exception.Message)"
            if ($null -eq $failure) { $failure = [pscustomobject]@{ stage='attempt-result'; message=$journalMessage } }
            else { $failure = [pscustomobject]@{ stage=$failure.stage; message=($failure.message + '; ' + $journalMessage) } }
        }
        $outcomes.Add([pscustomobject]@{
            entryId=$entry.entryId; state=$state; failure=$entryFailure; startBinding=$startBinding; resultBinding=$resultBinding
        }) | Out-Null
    }
    return [pscustomobject]@{
        runs=$runs.ToArray(); pairedOracleBindings=$oracles.ToArray(); pairedPhaseBaselineBindings=$baselines.ToArray()
        outcomes=$outcomes.ToArray(); failure=$failure
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
    foreach ($name in @('RTS_PERFORMANCE_ATTEMPT_TRACE_PATH',
            'RTS_PERFORMANCE_SOURCE_RECEIPT_PATH', 'RTS_PERFORMANCE_SOURCE_RECEIPT_SHA256')) {
        if ($info.EnvironmentVariables.ContainsKey($name)) { $info.EnvironmentVariables.Remove($name) }
    }
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

function Initialize-Stage5BoundedOutputCapture {
    if ($null -ne ('Stage5BoundedOutputCaptureReader' -as [type])) { return }
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading.Tasks;

public static class Stage5BoundedOutputCaptureReader
{
    public static async Task<byte[]> CaptureAsync(
        Stream source, long maximumBytes, string context)
    {
        if (source == null) throw new ArgumentNullException("source");
        if (!source.CanRead) throw new ArgumentException("The capture stream is not readable.", "source");
        if (maximumBytes < 0) throw new ArgumentOutOfRangeException("maximumBytes");

        // Make both redirected-stream calls return before either reader can
        // consume synchronously, so stdout and stderr are always drained in
        // parallel even when one pipe already contains buffered output.
        await Task.Yield();
        byte[] buffer = new byte[65536];
        using (MemoryStream captured = new MemoryStream())
        {
            while (true)
            {
                int count = await source.ReadAsync(buffer, 0, buffer.Length)
                    .ConfigureAwait(false);
                if (count == 0) break;
                if (captured.Length > maximumBytes - count)
                {
                    throw new InvalidDataException(
                        context + " exceeds its " + maximumBytes + "-byte capture bound.");
                }
                captured.Write(buffer, 0, count);
            }
            return captured.ToArray();
        }
    }
}
'@
}

function Start-Stage5BoundedOutputCapture {
    param([IO.Stream]$Stream, [Int64]$MaximumBytes, [string]$Context)
    Assert-Stage5PerformanceCondition ($null -ne $Stream -and $Stream.CanRead -and
        $MaximumBytes -ge 0 -and -not [string]::IsNullOrWhiteSpace($Context)) `
        'Stage 5 output capture requires a readable stream, a non-negative bound, and a context.'
    Initialize-Stage5BoundedOutputCapture
    return [Stage5BoundedOutputCaptureReader]::CaptureAsync(
        $Stream, $MaximumBytes, $Context)
}

function ConvertFrom-Stage5StrictUtf8Bytes {
    param([byte[]]$Bytes, [string]$Context)
    Assert-Stage5PerformanceCondition ($null -ne $Bytes -and
        -not [string]::IsNullOrWhiteSpace($Context)) `
        'Stage 5 UTF-8 decoding requires original bytes and a context.'
    try {
        return (New-Object Text.UTF8Encoding($false, $true)).GetString($Bytes)
    }
    catch {
        throw "$Context is not strict UTF-8: $($_.Exception.Message)"
    }
}

function ConvertFrom-Stage5StrictUtf8OutputPair {
    param([byte[]]$StdoutBytes, [byte[]]$StderrBytes)
    return (ConvertFrom-Stage5StrictUtf8Bytes $StdoutBytes 'Stage 5 stdout') +
        "`n" +
        (ConvertFrom-Stage5StrictUtf8Bytes $StderrBytes 'Stage 5 stderr')
}

function Invoke-Stage5InstalledPerformanceRun {
    param([object]$Context, [object]$PlanBinding, [string]$EntryId,
        [object]$TitleSessionContract)
    $launch = Resolve-Stage5PlannedPerformanceLaunch $Context $PlanBinding $EntryId $TitleSessionContract
    $entry = $launch.entry
    $plan = Read-Stage5PhaseBoundJson $PlanBinding $Context.taskRoot 'Installed launch original plan'
    $startSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $entry.outputPaths.attemptStartPath 'Installed launch durable start'
    $start = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $startSnapshot 'Installed launch durable start' -AsPsObject
    Assert-Stage5PerformanceProperties $start @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc','sourceBinding') 'Installed launch start'
    Assert-Stage5PerformanceCondition ((Test-Stage5JsonInteger $start.schemaVersion) -and
        $start.schemaVersion -eq 1 -and $start.event -ceq 'attempt-start' -and
        $start.planSha256 -ceq $PlanBinding.sha256 -and $start.entryId -ceq $entry.entryId -and
        $start.runNonce -ceq $entry.runNonce) 'Installed launch lacks its immutable original attempt start.'
    if ($entry.measurementRole -ceq 'phase-serial-baseline') {
        Assert-Stage5PerformanceCondition ($null -ne $start.sourceBinding -and
            $start.sourceBinding.path -ceq $entry.outputPaths.sourceBindingPath) 'Installed baseline start lacks its planned source audit binding.'
        Read-Stage5PhaseBoundJson $start.sourceBinding $Context.taskRoot 'Installed baseline durable source' | Out-Null
    }
    else { Assert-Stage5PerformanceCondition ($null -eq $start.sourceBinding) 'An ordinary role cannot claim a baseline source audit binding.' }
    if ($null -eq $Context.PSObject.Properties['processCleanup']) {
        $Context | Add-Member NoteProperty processCleanup ([pscustomobject]@{})
    }
    $Context.processCleanup = [pscustomobject]@{
        processId=0; exitProof=$true; blocked=$false; errors=@()
    }
    $script:Stage5CurrentProcessStarted = $false
    $script:Stage5CurrentProcessIdentity = $null
    $Fixture = $launch.fixture; $Lane = $entry.lane; $Ordinal = [int]$entry.ordinal
    $WorkerCount = [int]$entry.workerCount; $Timeout = [int]$plan.timeoutSeconds
    $runNonce = $entry.runNonce; $runId = $entry.entryId
    $runRoot = $entry.outputPaths.runRoot; $receiptDirectory = $entry.outputPaths.receiptDirectory
    $timingDirectory = $entry.outputPaths.timingDirectory; $tempDirectory = $entry.outputPaths.tempDirectory
    foreach ($directory in @($runRoot, $receiptDirectory, $timingDirectory, $tempDirectory)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
        Assert-Stage5FinalAcceptanceNoReparsePath $Context.taskRoot $directory 'Installed role output directory'
    }
    $rawPath = $entry.outputPaths.rawLogPath; $stdoutPath = $entry.outputPaths.stdoutPath; $stderrPath = $entry.outputPaths.stderrPath
    $arguments = $entry.expectedArgumentString
    $info = $launch.startInfo
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $processStarted = $false
    $processIdentity = $null
    $stdoutTask = $null
    $stderrTask = $null
    $exitCode = $null
    $runError = $null
    $stdoutSnapshot = $null
    $stderrSnapshot = $null
    $captureErrors = New-Object 'Collections.Generic.List[string]'
    # The self-test parameter set returns before this function is reachable.
    try {
        if ($null -ne $script:Stage5ActiveRegistryRecovery) {
            Add-Stage5RegistryRecoveryPendingProcess `
                $script:Stage5ActiveRegistryRecovery $Context.executablePath `
                $Context.executableSha256
        }
        $processStarted = $process.Start()
        if ($processStarted) {
            $Context.processCleanup.exitProof = $false
            $script:Stage5CurrentProcessStarted = $true
        }
        Assert-Stage5PerformanceCondition $processStarted `
            "Failed to start installed performance run '$runId'."
        $processIdentity = Get-Stage5ProcessIdentity $process
        $hostProcessId = $processIdentity.processId
        $hostCreationTime = $processIdentity.creationTimeUtc100ns
        $hostExecutablePath = $processIdentity.executablePath
        $hostExecutableHash = $processIdentity.executableSha256
        $hostCommandLine = $processIdentity.commandLine
        $script:Stage5CurrentProcessIdentity = $processIdentity
        if ($null -ne $script:Stage5ActiveRegistryRecovery) {
            Set-Stage5RegistryRecoveryObservedProcess `
                $script:Stage5ActiveRegistryRecovery $processIdentity
            Update-Stage5RegistryRecoveryState `
                $script:Stage5ActiveRegistryRecovery 'child-running' `
                $false $false
        }
        $stdoutTask = Start-Stage5BoundedOutputCapture `
            $process.StandardOutput.BaseStream ([Int64](64MB)) 'Stage 5 stdout'
        $stderrTask = Start-Stage5BoundedOutputCapture `
            $process.StandardError.BaseStream ([Int64](64MB)) 'Stage 5 stderr'
        $timeoutMilliseconds = [Int64]$Timeout * 1000
        $waitStopwatch = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            foreach ($capture in @(
                    [pscustomobject]@{ task = $stdoutTask; name = 'stdout' },
                    [pscustomobject]@{ task = $stderrTask; name = 'stderr' })) {
                if ($capture.task.IsFaulted) {
                    throw "Stage 5 $($capture.name) capture failed: $($capture.task.Exception.GetBaseException().Message)"
                }
            }
            $remainingMilliseconds = $timeoutMilliseconds - $waitStopwatch.ElapsedMilliseconds
            if ($remainingMilliseconds -le 0) {
                if ($process.WaitForExit(0)) { break }
                Stop-Stage5ProcessSafely $process $processIdentity
                throw "Installed performance run '$runId' exceeded $Timeout seconds."
            }
            $waitMilliseconds = [int][Math]::Min([Int64]250, $remainingMilliseconds)
            if ($process.WaitForExit($waitMilliseconds)) { break }
        }
        $waitStopwatch.Stop()
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
        if ($null -ne $script:Stage5ActiveRegistryRecovery -and
            $script:Stage5CurrentProcessStarted -and
            $null -ne $script:Stage5CurrentProcessIdentity) {
            Set-Stage5RegistryRecoveryObservedProcess `
                $script:Stage5ActiveRegistryRecovery `
                $script:Stage5CurrentProcessIdentity
            if (-not [bool]$Context.processCleanup.blocked) {
                try {
                    Assert-Stage5NoInstalledTitleProcesses
                    Update-Stage5RegistryRecoveryState `
                        $script:Stage5ActiveRegistryRecovery 'active' $true $true
                }
                catch {
                    $captureErrors.Add("registry recovery proof: $($_.Exception.Message)") |
                        Out-Null
                }
            }
        }
        foreach ($cleanupError in @($ownedCleanup.errors)) {
            $captureErrors.Add([string]$cleanupError) | Out-Null
        }
        foreach ($capture in @(
                [pscustomobject]@{ task = $stdoutTask; path = $stdoutPath; name = 'stdout' },
                [pscustomobject]@{ task = $stderrTask; path = $stderrPath; name = 'stderr' })) {
            try {
                $captureBytes = [byte[]]@()
                if ($null -ne $capture.task) {
                    if (-not $capture.task.Wait(30000)) {
                        throw "Stage 5 $($capture.name) did not complete within the bounded post-process wait."
                    }
                    $captureBytes = [byte[]]$capture.task.GetAwaiter().GetResult()
                }
                $captureSnapshot = Write-Stage5FinalAcceptanceFileAtomically `
                    -Path $capture.path -Bytes $captureBytes `
                    -Context "Stage 5 $($capture.name) capture" `
                    -EvidenceKind RawLog
                if ($capture.name -ceq 'stdout') {
                    $stdoutSnapshot = $captureSnapshot
                }
                else {
                    $stderrSnapshot = $captureSnapshot
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
    Assert-Stage5PerformanceCondition ($null -ne $stdoutSnapshot -and
        $null -ne $stderrSnapshot -and $null -ne $stdoutSnapshot.bytes -and
        $null -ne $stderrSnapshot.bytes) `
        "Installed performance run '$runId' lacks immutable host output snapshots."
    $diagnosticText = ConvertFrom-Stage5StrictUtf8OutputPair `
        ([byte[]]$stdoutSnapshot.bytes) ([byte[]]$stderrSnapshot.bytes)
    Assert-Stage5PerformanceCondition ($diagnosticText -notmatch $script:Stage5FatalPattern) `
        "Installed performance run '$runId' reported a fatal diagnostic."
    $receiptFiles = @(Get-ChildItem -LiteralPath $receiptDirectory -File -Filter '*.json')
    Assert-Stage5PerformanceCondition ($receiptFiles.Count -eq 1) `
        "Installed performance run '$runId' did not emit exactly one receipt."
    $expectedReceiptName = 'performance-receipt-{0}-{1}.json' -f $runId, $hostProcessId
    Assert-Stage5PerformanceCondition ($receiptFiles[0].Name -ceq $expectedReceiptName) `
        "Installed performance run '$runId' receipt filename is detached from its native run/process identity."
    $timingFiles = @(Get-ChildItem -LiteralPath $timingDirectory -File -Filter '*.csv')
    Assert-Stage5PerformanceCondition ($timingFiles.Count -eq 1) `
        "Installed performance run '$runId' did not emit exactly one timing file."
    $timingNameMatches = $timingFiles[0].Name -cmatch (
        '^frame-timing-' + [Regex]::Escape([string]$hostProcessId) + '-(?<tick>0|[1-9][0-9]*)\.csv$')
    [UInt32]$timingTick = 0
    Assert-Stage5PerformanceCondition ($timingNameMatches -and
        [UInt32]::TryParse([string]$Matches.tick, [ref]$timingTick)) `
        "Installed performance run '$runId' timing filename is detached from its native process/tick identity."
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
            timingSha256 = Get-Stage5PerformanceSha256 $timingFiles[0].FullName
        }
    }
}

function Write-Stage5JsonAtomically {
    param([string]$Path, [object]$Value, [switch]$CreateNew)
    $json = $Value | ConvertTo-Json -Depth 20
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json)
    Write-Stage5FinalAcceptanceFileAtomically `
        -Path $Path -Bytes $bytes -Context 'Stage 5 performance JSON publication' `
        -EvidenceKind JsonReceipt -ReplaceExisting:(-not $CreateNew) | Out-Null
}

function Get-Stage5PerformanceRelocationManifest {
    param([string]$TaskRootPath)
    $root = [IO.Path]::GetFullPath($TaskRootPath).TrimEnd('\', '/')
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File |
            Where-Object { $_.Length -gt 0 } | Sort-Object FullName)) {
        $full = [IO.Path]::GetFullPath($file.FullName)
        Assert-Stage5FinalAcceptancePathContained $root $full `
            'Relocation-manifest evidence file'
        Assert-Stage5FinalAcceptanceNoReparsePath $root $full `
            'Relocation-manifest evidence file'
        $relative = $full.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
        $kind = if ($file.Extension -ceq '.json') { 'JsonReceipt' }
            elseif ($file.Extension -ceq '.bin') { 'Trace' }
            elseif ($file.Extension -ceq '.rep') { 'Replay' }
            else { 'RawLog' }
        $entries += [ordered]@{
            recordedPath = $full
            path = $relative
            sha256 = Get-Stage5PerformanceSha256 $full
            length = [Int64]$file.Length
            evidenceKind = $kind
        }
    }
    Assert-Stage5PerformanceCondition ($entries.Count -gt 0) `
        'Authoritative performance relocation manifest is empty.'
    return $entries
}

function New-Stage5AuthoritativePerformanceEvidence {
    param(
        [object]$Context,
        [object]$Aggregate,
        [string]$HostAggregatePath,
        [string]$HostAggregateSha256,
        [object]$Baseline,
        [object]$FixtureManifest,
        [string]$PhaseProfilePath,
        [string]$PhaseProfileSha256,
        [object]$PerformanceData
    )
    Assert-Stage5PerformanceCondition ($Context.qualificationMode -ceq
            'External16Core' -and
        $Context.referencePolicy -ceq 'paired-serial-oracle-v1') `
        'Only External16Core paired qualification can publish authoritative scaling evidence.'
    Assert-Stage5PerformanceCondition ($null -ne $PerformanceData -and
        $Aggregate.performanceData.path -ceq $PerformanceData.path -and
        $Aggregate.performanceData.sha256 -ceq $PerformanceData.manifestSha256 -and
        $Aggregate.performanceData.closureSha256 -ceq
            $PerformanceData.closureSha256 -and
        $Aggregate.performanceData.fileCount -eq $PerformanceData.fileCount) `
        'Authoritative performance qualification-data binding is incomplete.'
    $retainedPerformanceData = Read-Stage5PerformanceQualificationData `
        $PerformanceData.path $PerformanceData.manifestSha256 `
        $PerformanceData.closureSha256 $Context.sourceCommit $Context.title `
        $PerformanceData.runtimeRoot $null -SkipInstalledFileValidation
    Assert-Stage5PerformanceCondition ($retainedPerformanceData.fileCount -eq
        $PerformanceData.fileCount) `
        'Retained performance qualification-data file coverage changed.'
    $diagnostics = ConvertTo-Stage5PerformanceDiagnostics `
        $HostAggregatePath $HostAggregateSha256 $Context.sourceCommit `
        $Context.artifactSetSha256 $Context.executableSha256 `
        -ExpectedTitle $Context.title
    Assert-Stage5PerformanceCondition ($diagnostics.qualificationMode -ceq
            'External16Core' -and
        $diagnostics.referencePolicy -ceq 'paired-serial-oracle-v1') `
        'Validated host diagnostics cannot be promoted to authoritative external evidence.'

    $fixtureNames = @($script:CanonicalFixtureIds)
    $laneNames = @($script:ExternalLaneNames)
    $laneWorkers = @($script:ExternalLaneWorkers)
    $phaseNames = @('owner-intake', 'legacy-mutable-island', 'spatial-work',
        'owner-tail', 'verification-publication')
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning',
        'spatial', 'path')
    $stageNames = @('capture', 'schedule', 'wait', 'validate', 'commit')

    $runsByKey = @{}
    foreach ($run in @($diagnostics.runs)) {
        $key = "$($run.fixtureId)|$($run.lane)|$($run.ordinal)"
        Assert-Stage5PerformanceCondition (-not $runsByKey.ContainsKey($key)) `
            "Authoritative throughput schedule repeats '$key'."
        $runsByKey[$key] = $run
    }
    Assert-Stage5PerformanceCondition ($runsByKey.Count -eq
        12 * ($Context.measuredRuns + $Context.warmupRuns)) `
        'Authoritative throughput diagnostics do not cover the exact external matrix.'

    $oracleBySource = @{}
    foreach ($pair in @($diagnostics.pairedOracleBindings)) {
        Assert-Stage5PerformanceCondition (-not $oracleBySource.ContainsKey(
                [string]$pair.throughputRunId)) `
            'Authoritative paired serial-oracle schedule is duplicated.'
        $oracleBySource[[string]$pair.throughputRunId] = $pair.oracleRun
    }
    Assert-Stage5PerformanceCondition ($oracleBySource.Count -eq $runsByKey.Count) `
        'Authoritative serial-oracle schedule does not pair every throughput run.'

    $phaseBySource = @{}
    foreach ($pair in @($diagnostics.pairedPhaseBaselineBindings)) {
        Assert-Stage5PerformanceCondition ($pair.profileId -ceq
                [string]$Context.phaseBaselineProfiles[0].profileId -and
            -not $phaseBySource.ContainsKey([string]$pair.throughputRunId)) `
            'Authoritative phase-baseline schedule is duplicated or uses another profile.'
        $phaseBySource[[string]$pair.throughputRunId] = $pair.baselineRun
    }
    Assert-Stage5PerformanceCondition ($phaseBySource.Count -eq
        $Context.measuredRuns + $Context.warmupRuns) `
        'Authoritative phase-baseline schedule does not cover the selected dense one-worker lane.'

    $orderedCpuSets = @($Context.topology.cpuSets | Sort-Object group,
        logicalProcessorIndex, id)
    $physicalIndices = @{}
    $logicalIndexByCpuSetId = @{}
    $logicalProcessors = @()
    foreach ($cpuSet in $orderedCpuSets) {
        $physicalKey = "$($cpuSet.group):$($cpuSet.coreIndex)"
        if (-not $physicalIndices.ContainsKey($physicalKey)) {
            $physicalIndices[$physicalKey] = $physicalIndices.Count
        }
        $logicalIndex = $logicalProcessors.Count
        Assert-Stage5PerformanceCondition (-not $logicalIndexByCpuSetId.ContainsKey(
                [UInt32]$cpuSet.id)) `
            'Authoritative topology contains a duplicate CPU-set identifier.'
        $logicalIndexByCpuSetId[[UInt32]$cpuSet.id] = $logicalIndex
        $logicalProcessors += [ordered]@{
            logicalProcessorIndex = $logicalIndex
            physicalCoreIndex = [int]$physicalIndices[$physicalKey]
        }
    }
    Assert-Stage5PerformanceCondition ($physicalIndices.Count -eq
            $Context.topology.physicalCoreCount -and
        $logicalProcessors.Count -eq $Context.topology.logicalProcessorCount -and
        $physicalIndices.Count -ge 16 -and $physicalIndices.Count -le 64) `
        'Authoritative topology cannot be represented by the exact 64-bit physical-core mask contract.'

    $topologyLanes = @()
    $finalLanes = @()
    for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
        $laneName = $laneNames[$laneIndex]
        $laneRuns = @($diagnostics.runs | Where-Object {
            $_.lane -ceq $laneName
        })
        $referenceIds = @($laneRuns[0].selectedWorkerCpuSetIds)
        foreach ($run in $laneRuns) {
            Assert-Stage5PerformanceCondition (
                ($run.selectedWorkerCpuSetIds | ConvertTo-Json -Compress) -ceq
                ($referenceIds | ConvertTo-Json -Compress)) `
                "Authoritative '$laneName' CPU-set selection changed between runs."
        }
        $selectedLogical = @()
        [UInt64]$mask = 0
        foreach ($cpuSetId in $referenceIds) {
            Assert-Stage5PerformanceCondition ($logicalIndexByCpuSetId.ContainsKey(
                    [UInt32]$cpuSetId)) `
                "Authoritative '$laneName' selects a CPU set outside the host snapshot."
            $logicalIndex = [int]$logicalIndexByCpuSetId[[UInt32]$cpuSetId]
            $selectedLogical += $logicalIndex
            $physicalIndex = [int]$logicalProcessors[$logicalIndex].physicalCoreIndex
            $mask = $mask -bor ([UInt64]1 -shl $physicalIndex)
        }
        Assert-Stage5PerformanceCondition ($selectedLogical.Count -eq
                $laneWorkers[$laneIndex] -and
            (Get-Stage5PerformanceMaskBitCount $mask) -eq $laneWorkers[$laneIndex]) `
            "Authoritative '$laneName' does not select distinct physical cores."
        $topologyLanes += [ordered]@{
            name = $laneName
            requestedWorkers = $laneWorkers[$laneIndex]
            selectedLogicalProcessorIndices = $selectedLogical
        }
        $finalLanes += [ordered]@{
            name = $laneName
            requestedWorkers = $laneWorkers[$laneIndex]
            selectedLogicalProcessors = $selectedLogical.Count
            selectedDistinctPhysicalCores = Get-Stage5PerformanceMaskBitCount $mask
            selectedPhysicalCoreMask = $mask.ToString('X16')
        }
    }

    $topologySourceRun = $runsByKey['one-thousand-units|forced-one|1']
    Assert-Stage5PerformanceCondition ($null -ne $topologySourceRun -and
        -not $topologySourceRun.warmup) `
        'Authoritative topology has no first measured one-worker source run.'
    $topologyPath = Join-Path $Context.taskRoot `
        'Stage5PerformanceScalingTopologyReceipt.json'
    $topologyDocument = [ordered]@{
        schemaVersion = 2
        producer = 'installed-runtime-scaling-runner-v2'
        source = 'GetSystemCpuSetInformation'
        sourceCommit = $Context.sourceCommit
        executableSha256 = $Context.executableSha256
        runId = [string]$topologySourceRun.runId
        processId = [Int64]$topologySourceRun.processId
        processCreationTimeUtc100ns = [Int64]$topologySourceRun.processCreationTimeUtc100ns
        argumentString = [string]$topologySourceRun.argumentString
        commandLine = [string]$topologySourceRun.commandLine
        logicalProcessors = $logicalProcessors
        selectedLanes = $topologyLanes
    }
    Write-Stage5JsonAtomically $topologyPath $topologyDocument -CreateNew
    $topologySha256 = Get-Stage5PerformanceSha256 $topologyPath

    $stage3Samples = @()
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $baselineFixture = $Baseline.fixtures[$fixtureIndex]
        $fixture = $FixtureManifest.fixtures[$fixtureIndex]
        for ($sampleIndex = $script:WarmupRuns;
                $sampleIndex -lt $baselineFixture.rawWallMilliseconds.Count;
                ++$sampleIndex) {
            $stage3Samples += [ordered]@{
                fixture = $fixture.id
                fixtureSha256 = $fixture.sha256
                playerCount = 8
                peakUnitCount = $fixture.peakUnitCount
                lane = 'stage3-forced-one'
                repeat = $sampleIndex - $script:WarmupRuns
                baselineSampleIndex = $sampleIndex
                executableSha256 = $Baseline.executableSha256
                elapsedMilliseconds = [double]$baselineFixture.rawWallMilliseconds[$sampleIndex]
            }
        }
    }

    $fixtureSamples = @()
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        for ($laneIndex = 0; $laneIndex -lt 3; ++$laneIndex) {
            for ($repeat = 0; $repeat -lt $Context.measuredRuns; ++$repeat) {
                $run = $runsByKey["$($fixtureNames[$fixtureIndex])|$($laneNames[$laneIndex])|$($repeat + 1)"]
                Assert-Stage5PerformanceCondition ($null -ne $run -and
                    -not $run.warmup) 'Authoritative measured throughput row is missing.'
                $fixtureSamples += [ordered]@{
                    fixture = $fixtureNames[$fixtureIndex]
                    fixtureSha256 = [string]$run.fixtureSha256
                    playerCount = [int]$run.workload.playerCount
                    peakUnitCount = [int]$run.workload.peakUnitCount
                    requestedMinimumUnitCount = [int]$run.requestedMinimumUnitCount
                    initialUnitCount = [int]$run.workload.initialUnitCount
                    lane = $laneNames[$laneIndex]
                    repeat = $repeat
                    runId = [string]$run.runId
                    processId = [Int64]$run.processId
                    processCreationTimeUtc100ns = [Int64]$run.processCreationTimeUtc100ns
                    executableSha256 = $Context.executableSha256
                    argumentString = [string]$run.argumentString
                    commandLine = [string]$run.commandLine
                    elapsedMilliseconds = [double]$run.elapsedMilliseconds
                }
            }
        }
    }

    $phaseSamples = @()
    $phaseAccountingSamples = @()
    $phaseAggregates = @()
    $phaseElapsedValues = @{}
    $phaseSerialValues = @{}
    foreach ($phaseName in $phaseNames) {
        $phaseElapsedValues[$phaseName] = @()
        $phaseSerialValues[$phaseName] = @()
    }
    [double]$worstSerialFraction = -1.0
    [double]$amdahlOne = 0.0
    [double]$amdahlSerial = 0.0
    foreach ($scope in @('world', 'control')) {
        for ($phaseIndex = 0; $phaseIndex -lt $phaseNames.Count; ++$phaseIndex) {
            for ($repeat = 0; $repeat -lt $Context.measuredRuns; ++$repeat) {
                $source = $runsByKey["dense-eight-player|forced-one|$($repeat + 1)"]
                $baselineRun = $phaseBySource[[string]$source.runId]
                Assert-Stage5PerformanceCondition ($null -ne $baselineRun) `
                    'Authoritative measured phase-baseline pair is missing.'
                $phase = if ($scope -ceq 'world') {
                    $baselineRun.phases[$phaseIndex]
                } else { $baselineRun.phaseAccounting.controlAccounting.phases[$phaseIndex] }
                $elapsed = [double]$phase.totalNanoseconds / 1000000.0
                $serial = [double]$phase.serialNanoseconds / 1000000.0
                $phaseSamples += [ordered]@{
                    scope = $scope; phase = $phaseNames[$phaseIndex]; repeat = $repeat
                    sourceRunId = [string]$source.runId
                    sourceProcessId = [Int64]$source.processId
                    sourceProcessCreationTimeUtc100ns = [Int64]$source.processCreationTimeUtc100ns
                    sourceArgumentString = [string]$source.argumentString
                    sourceCommandLine = [string]$source.commandLine
                    baselineRunId = [string]$baselineRun.runId
                    baselineProcessId = [Int64]$baselineRun.processId
                    baselineProcessCreationTimeUtc100ns = [Int64]$baselineRun.processCreationTimeUtc100ns
                    baselineArgumentString = [string]$baselineRun.argumentString
                    baselineCommandLine = [string]$baselineRun.commandLine
                    elapsedMilliseconds = $elapsed
                    serialMilliseconds = $serial
                    serialMillisecondsKnown = $true
                }
                if ($scope -ceq 'world') {
                    $phaseElapsedValues[$phaseNames[$phaseIndex]] += $elapsed
                    $phaseSerialValues[$phaseNames[$phaseIndex]] += $serial
                }
            }
        }
    }
    for ($repeat = 0; $repeat -lt $Context.measuredRuns; ++$repeat) {
        $source = $runsByKey["dense-eight-player|forced-one|$($repeat + 1)"]
        $baselineRun = $phaseBySource[[string]$source.runId]
        $accounting = $baselineRun.phaseAccounting
        [double]$world = [double]$accounting.frameNanoseconds / 1000000.0
        [double]$control = [double]$accounting.controlAccounting.totalNanoseconds / 1000000.0
        [double]$completion = [double]$accounting.completionSerialNanoseconds / 1000000.0
        [double]$worldUnscoped = [double]$accounting.unscopedSerialNanoseconds / 1000000.0
        [double]$controlUnscoped = [double]$accounting.controlAccounting.unscopedSerialNanoseconds / 1000000.0
        [double]$serial = $worldUnscoped + $controlUnscoped + $completion
        foreach ($phase in @($baselineRun.phases) +
                @($accounting.controlAccounting.phases)) {
            $serial += [double]$phase.serialNanoseconds / 1000000.0
        }
        [double]$total = $world + $control + $completion
        [double]$fraction = $serial / $total
        if ($fraction -gt $worstSerialFraction) {
            $worstSerialFraction = $fraction; $amdahlOne = $total
            $amdahlSerial = $serial
        }
        $phaseAccountingSamples += [ordered]@{
            repeat = $repeat
            sourceRunId = [string]$source.runId
            sourceProcessId = [Int64]$source.processId
            sourceProcessCreationTimeUtc100ns = [Int64]$source.processCreationTimeUtc100ns
            sourceArgumentString = [string]$source.argumentString
            sourceCommandLine = [string]$source.commandLine
            baselineRunId = [string]$baselineRun.runId
            baselineProcessId = [Int64]$baselineRun.processId
            baselineProcessCreationTimeUtc100ns = [Int64]$baselineRun.processCreationTimeUtc100ns
            baselineArgumentString = [string]$baselineRun.argumentString
            baselineCommandLine = [string]$baselineRun.commandLine
            accountingOrigin = 'kernel-performance-ledger-whole-frame-v1'
            worldMilliseconds = $world; controlMilliseconds = $control
            completionSerialMilliseconds = $completion
            worldUnscopedSerialMilliseconds = $worldUnscoped
            controlUnscopedSerialMilliseconds = $controlUnscoped
            totalOneWorkerMilliseconds = $total
            totalSerialMilliseconds = $serial
        }
    }
    Assert-Stage5PerformanceCondition ($worstSerialFraction -gt 0.0 -and
        $worstSerialFraction -le 0.5) `
        'Authoritative whole-frame phase accounting does not prove a finite 2x Amdahl ceiling.'
    foreach ($phaseName in $phaseNames) {
        $phaseAggregates += [ordered]@{
            name = $phaseName
            elapsedMilliseconds = Get-Stage5PerformanceMedian $phaseElapsedValues[$phaseName]
            serialMilliseconds = Get-Stage5PerformanceMedian $phaseSerialValues[$phaseName]
            serialMillisecondsKnown = $true
        }
    }

    $kernelSamples = @()
    $kernelPartValues = @{}
    $kernelPipelineValues = @{}
    $kernelSerialValues = @{}
    $kernelAdmittedValues = @{}
    foreach ($kernelName in $kernelNames) {
        $kernelPartValues[$kernelName] = @{}
        foreach ($stageName in $stageNames) {
            $kernelPartValues[$kernelName][$stageName] = @()
        }
        $kernelPipelineValues[$kernelName] = @()
        $kernelSerialValues[$kernelName] = @()
        $kernelAdmittedValues[$kernelName] = @()
        for ($repeat = 0; $repeat -lt $Context.measuredRuns; ++$repeat) {
            $source = $runsByKey["dense-eight-player|physical-8|$($repeat + 1)"]
            $oracle = $oracleBySource[[string]$source.runId]
            Assert-Stage5PerformanceCondition ($null -ne $oracle) `
                'Authoritative dense physical-8 serial-oracle pair is missing.'
            $timingStreams = @($source.kernelTiming.streams | Where-Object {
                $_.name -ceq $kernelName
            })
            $oracleStreams = @($oracle.kernelReference.streams | Where-Object {
                $_.name -ceq $kernelName
            })
            Assert-Stage5PerformanceCondition ($timingStreams.Count -gt 0 -and
                $oracleStreams.Count -eq $timingStreams.Count) `
                "Authoritative '$kernelName' subtype coverage is incomplete."
            $parts = @{}; foreach ($stageName in $stageNames) {
                $parts[$stageName] = [decimal]0
            }
            [UInt64]$admitted = 0; [decimal]$serial = 0
            foreach ($stream in $timingStreams) {
                $admitted += [UInt64]$stream.admittedBatches
                foreach ($stage in $stream.stages) {
                    $parts[[string]$stage.name] +=
                        [decimal]$stage.totalNanoseconds / 1000000
                }
            }
            foreach ($stream in $oracleStreams) {
                $serial += [decimal]$stream.serialNanoseconds / 1000000
            }
            [double]$pipeline = 0.0
            foreach ($stageName in $stageNames) {
                $value = [double]$parts[$stageName]
                $kernelPartValues[$kernelName][$stageName] += $value
                $pipeline += $value
            }
            Assert-Stage5PerformanceCondition ($admitted -gt 0 -and
                $pipeline -gt 0.0 -and $serial -gt 0) `
                "Authoritative '$kernelName' pair has no positive timing coverage."
            $kernelPipelineValues[$kernelName] += $pipeline
            $kernelSerialValues[$kernelName] += [double]$serial
            $kernelAdmittedValues[$kernelName] += [double]$admitted
            $kernelSamples += [ordered]@{
                kernel = $kernelName; repeat = $repeat
                runId = [string]$source.runId; processId = [Int64]$source.processId
                processCreationTimeUtc100ns = [Int64]$source.processCreationTimeUtc100ns
                argumentString = [string]$source.argumentString
                commandLine = [string]$source.commandLine
                oracleRunId = [string]$oracle.runId
                oracleProcessId = [Int64]$oracle.processId
                oracleProcessCreationTimeUtc100ns = [Int64]$oracle.processCreationTimeUtc100ns
                oracleArgumentString = [string]$oracle.argumentString
                oracleCommandLine = [string]$oracle.commandLine
                admittedSlices = [Int64]$admitted
                captureMilliseconds = [double]$parts.capture
                scheduleMilliseconds = [double]$parts.schedule
                waitMilliseconds = [double]$parts.wait
                validateMilliseconds = [double]$parts.validate
                commitMilliseconds = [double]$parts.commit
                exactSerialOperationMilliseconds = [double]$serial
                exactSerialOperationMillisecondsKnown = $true
                timingAttribution = 'owner-stack-exclusive-v1'
            }
        }
    }

    $kernelAggregates = @()
    foreach ($kernelName in $kernelNames) {
        $pipelineMedian = Get-Stage5PerformanceMedian `
            $kernelPipelineValues[$kernelName]
        $serialMedian = Get-Stage5PerformanceMedian $kernelSerialValues[$kernelName]
        $netSpeedup = $serialMedian / $pipelineMedian
        Assert-Stage5PerformanceCondition ($netSpeedup -gt 1.0) `
            "Authoritative '$kernelName' median paired oracle does not prove positive net speedup."
        $kernelAggregates += [ordered]@{
            name = $kernelName
            admittedSlices = [int](Get-Stage5PerformanceMedian `
                $kernelAdmittedValues[$kernelName])
            captureMilliseconds = Get-Stage5PerformanceMedian `
                $kernelPartValues[$kernelName].capture
            scheduleMilliseconds = Get-Stage5PerformanceMedian `
                $kernelPartValues[$kernelName].schedule
            waitMilliseconds = Get-Stage5PerformanceMedian `
                $kernelPartValues[$kernelName].wait
            validateMilliseconds = Get-Stage5PerformanceMedian `
                $kernelPartValues[$kernelName].validate
            commitMilliseconds = Get-Stage5PerformanceMedian `
                $kernelPartValues[$kernelName].commit
            totalParallelMilliseconds = $pipelineMedian
            exactSerialOperationMilliseconds = $serialMedian
            exactSerialOperationMillisecondsKnown = $true
            timingAttribution = 'owner-stack-exclusive-v1'
            netSpeedup = $netSpeedup
        }
    }

    $relocationManifest = @(Get-Stage5PerformanceRelocationManifest `
        $Context.taskRoot)
    $rawPath = Join-Path $Context.taskRoot `
        'Stage5PerformanceScalingRawSamples.json'
    $rawDocument = [ordered]@{
        schemaVersion = 3
        evidenceKind = 'stage5-performance-scaling-raw-samples'
        producer = 'installed-runtime-scaling-runner-v3'
        recordedUtc = [string]$Aggregate.recordedUtc
        cohortNonce = [string]$Context.cohortNonce
        cohortCreatedUtc = [string]$Context.cohortCreatedUtc
        qualificationMode = 'External16Core'
        referencePolicy = 'paired-serial-oracle-v1'
        sourceCommit = [string]$Context.sourceCommit
        artifactSetSha256 = [string]$Context.artifactSetSha256
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = [string]$Context.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$Context.runtimeClosure.closureSha256
        }
        title = [string]$Context.title
        executableSha256 = [string]$Context.executableSha256
        stage3SourceCommit = [string]$Context.stage3SourceCommit
        stage3ExecutableSha256 = [string]$Baseline.executableSha256
        stage3BaselineSha256 = [string]$Baseline.sha256
        measurementMode = 'headless-throughput'; installedRuntime = $true
        recordedTaskRoot = [string]$Context.taskRoot
        relocationManifest = $relocationManifest
        hostQualification = [ordered]@{
            path = [IO.Path]::GetFileName($HostAggregatePath)
            sha256 = $HostAggregateSha256
        }
        phaseBaselineProfile = [ordered]@{
            path = [IO.Path]::GetFileName($PhaseProfilePath)
            sha256 = $PhaseProfileSha256
        }
        performanceData = [ordered]@{
            path = [IO.Path]::GetFileName([string]$PerformanceData.path)
            sha256 = [string]$PerformanceData.manifestSha256
            closureSha256 = [string]$PerformanceData.closureSha256
            fileCount = [int]$PerformanceData.fileCount
        }
        stage3Baseline = [ordered]@{
            path = [IO.Path]::GetFileName([string]$Baseline.path)
            sha256 = [string]$Baseline.sha256
        }
        topologyReceipt = [ordered]@{
            path = [IO.Path]::GetFileName($topologyPath); sha256 = $topologySha256
        }
        stage3Samples = $stage3Samples
        fixtureSamples = $fixtureSamples
        phaseSamples = $phaseSamples
        phaseAccountingSamples = $phaseAccountingSamples
        kernelSamples = $kernelSamples
    }
    Write-Stage5JsonAtomically $rawPath $rawDocument -CreateNew
    $rawSha256 = Get-Stage5PerformanceSha256 $rawPath

    $fixtureAggregates = @()
    for ($fixtureIndex = 0; $fixtureIndex -lt 4; ++$fixtureIndex) {
        $fixtureName = $fixtureNames[$fixtureIndex]
        $stage3 = Get-Stage5PerformanceMedian @($stage3Samples | Where-Object {
            $_.fixture -ceq $fixtureName
        } | ForEach-Object { [double]$_.elapsedMilliseconds })
        $medians = @{}
        foreach ($laneName in $laneNames) {
            $medians[$laneName] = Get-Stage5PerformanceMedian @(
                $fixtureSamples | Where-Object {
                    $_.fixture -ceq $fixtureName -and $_.lane -ceq $laneName
                } | ForEach-Object { [double]$_.elapsedMilliseconds })
        }
        $fixtureRows = @($fixtureSamples | Where-Object {
            $_.fixture -ceq $fixtureName
        })
        $regression = $medians['forced-one'] / $stage3
        $speedup8 = $medians['forced-one'] / $medians['physical-8']
        $scale16 = $medians['physical-8'] / $medians['physical-16']
        Assert-Stage5PerformanceCondition ($regression -le 1.05 -and
            $speedup8 -ge 2.0 -and $scale16 -gt 1.0) `
            "Authoritative fixture '$fixtureName' misses a Stage 5 scaling threshold."
        $fixtureAggregates += [ordered]@{
            name = $fixtureName; playerCount = 8
            peakUnitCount = [int](($fixtureRows | ForEach-Object {
                [int]$_.peakUnitCount
            } | Measure-Object -Maximum).Maximum)
            requestedMinimumUnitCount = [int]$fixtureRows[0].requestedMinimumUnitCount
            minimumInitialUnitCount = [int](($fixtureRows | ForEach-Object {
                [int]$_.initialUnitCount
            } | Measure-Object -Minimum).Minimum)
            repeats = [int]$Context.measuredRuns
            stage3OneWorkerMilliseconds = $stage3
            stage5OneWorkerMilliseconds = $medians['forced-one']
            eightPhysicalCoreMilliseconds = $medians['physical-8']
            sixteenPhysicalCoreMilliseconds = $medians['physical-16']
            oneWorkerRegressionRatio = $regression
            eightPhysicalCoreSpeedup = $speedup8
            eightToSixteenSpeedup = $scale16
        }
    }

    $finalPath = Join-Path $Context.taskRoot 'Stage5PerformanceScaling.json'
    $finalDocument = [ordered]@{
        schemaVersion = 2; evidenceKind = 'stage5-performance-scaling'
        status = 'passed'; recordedUtc = [string]$Aggregate.recordedUtc
        cohortNonce = [string]$Context.cohortNonce
        cohortCreatedUtc = [string]$Context.cohortCreatedUtc
        qualificationMode = 'External16Core'
        referencePolicy = 'paired-serial-oracle-v1'
        sourceCommit = [string]$Context.sourceCommit
        artifactSetSha256 = [string]$Context.artifactSetSha256
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = [string]$Context.runtimeClosure.dependencyManifestSha256
            closureSha256 = [string]$Context.runtimeClosure.closureSha256
        }
        title = [string]$Context.title
        executableSha256 = [string]$Context.executableSha256
        stage3SourceCommit = [string]$Context.stage3SourceCommit
        stage3ExecutableSha256 = [string]$Baseline.executableSha256
        stage3BaselineSha256 = [string]$Baseline.sha256
        measurementMode = 'headless-throughput'; installedRuntime = $true
        hostQualification = $rawDocument.hostQualification
        phaseBaselineProfile = $rawDocument.phaseBaselineProfile
        performanceData = $rawDocument.performanceData
        stage3Baseline = $rawDocument.stage3Baseline
        rawSampleManifest = [ordered]@{
            path = [IO.Path]::GetFileName($rawPath); sha256 = $rawSha256
        }
        topology = [ordered]@{
            source = 'GetSystemCpuSetInformation'; topologySha256 = $topologySha256
            physicalCoreCount = [int]$physicalIndices.Count
            logicalProcessorCount = [int]$logicalProcessors.Count
        }
        selectedLanes = $finalLanes
        oneWorkerPhases = $phaseAggregates
        amdahl = [ordered]@{
            totalOneWorkerMilliseconds = $amdahlOne
            totalSerialMilliseconds = $amdahlSerial
            serialFraction = $worstSerialFraction
            maximumSpeedup = 1.0 / $worstSerialFraction
            reachesTwoX = $true
        }
        kernelTimings = $kernelAggregates
        fixtures = $fixtureAggregates
    }
    Write-Stage5JsonAtomically $finalPath $finalDocument -CreateNew
    $finalSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $finalPath `
        'Authoritative Stage 5 performance evidence'
    $proof = Read-Stage5PerformanceScalingEvidence $finalPath `
        $Context.sourceCommit $Context.artifactSetSha256 `
        $Context.executableSha256 $Baseline.sha256 -ExpectedTitle $Context.title `
        -Snapshot $finalSnapshot -ExpectedSnapshotSha256 $finalSnapshot.sha256 `
        -ExpectedCohortNonce $Context.cohortNonce `
        -ExpectedCohortCreatedUtc $Context.cohortCreatedUtc `
        -ExpectedRuntimeClosure $Context.runtimeClosure `
        -ExpectedPhaseBaselineProfileSha256 $PhaseProfileSha256
    Assert-Stage5PerformanceCondition ($proof.cohortNonce -ceq
            $Context.cohortNonce -and
        $proof.recordedUtc -ceq [string]$Aggregate.recordedUtc -and
        $proof.hostQualificationSha256 -ceq $HostAggregateSha256) `
        'Authoritative Stage 5 performance evidence failed its final provenance self-check.'
    return [pscustomobject]@{
        rawPath = $rawPath; finalPath = $finalPath; proof = $proof
    }
}

function New-Stage5NativeFixtureProcess {
    param([Diagnostics.ProcessStartInfo]$StartInfo)
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $StartInfo
    return $process
}

function Invoke-Stage5NativePerformanceFixtureProduction {
    param(
        [string]$FixtureTitle,
        [string]$ExecutablePath,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$ArtifactManifestPath,
        [string]$ReviewedManifestPath,
        [string]$ReviewedManifestSha256,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [string]$OutputRoot,
        [int]$Timeout,
        [string]$GeneralsRuntimeRoot = '',
        [switch]$Diagnostic
    )
    Assert-Stage5PerformanceCondition (([bool]$ProduceNativeFixture -xor [bool]$Diagnostic) -and
        [bool]$AllowHeadlessDirectExecution) `
        'Native fixture production requires both explicit production and reviewed headless direct-execution consent.'
    Assert-Stage5PerformanceSourceCommit $SourceCommit 'ExpectedSourceCommit'
    $requiredHashes = @(@($ExecutableSha256, 'ExpectedExecutableSha256'),
        @($ArtifactSetSha256, 'ExpectedArtifactSetSha256'))
    if (-not $Diagnostic) { $requiredHashes += ,@($ReviewedManifestSha256, 'ExpectedReviewedFixtureManifestSha256') }
    foreach ($binding in $requiredHashes) {
        Assert-Stage5PerformanceHash ([string]$binding[0]) ([string]$binding[1])
    }
    $executionCohort = Resolve-Stage5PerformanceExecutionCohort `
        'InstalledKernelExecution' $CohortNonce $CohortCreatedUtc $true $true
    $executableFull = [IO.Path]::GetFullPath($ExecutablePath)
    Assert-Stage5PerformanceCondition (Test-Path -LiteralPath $executableFull `
        -PathType Leaf) "Installed executable was not found: $executableFull"
    $expectedLeaf = if ($FixtureTitle -ceq 'Generals') {
        'generalsv.exe'
    } else { 'generalszh.exe' }
    Assert-Stage5PerformanceCondition (
        [IO.Path]::GetFileName($executableFull) -ceq $expectedLeaf) `
        "Installed executable leaf must be '$expectedLeaf'."
    Assert-Stage5PerformanceCondition (
        (Get-Stage5PeMachine $executableFull) -eq 0x8664) `
        'Installed executable machine is not AMD64 (0x8664).'
    Assert-Stage5PerformanceFileHash $executableFull $ExecutableSha256 `
        'Installed executable SHA-256' | Out-Null
    Assert-Stage5ProcessLocalProfileCapability $executableFull `
        -Context 'Native fixture production executable' | Out-Null
    $runtimeFull = Split-Path -Parent $executableFull
    $launcher = Get-Stage5LauncherContract $runtimeFull $executableFull
    $artifact = Read-Stage5PerformanceArtifactSet $ArtifactManifestPath `
        $ArtifactSetSha256 $SourceCommit $FixtureTitle $executableFull `
        $ExecutableSha256
    Assert-Stage5PerformanceLauncherBinding $artifact $launcher $FixtureTitle
    $baseBinding = Get-Stage5PerformanceBaseBinding $FixtureTitle $runtimeFull $artifact $GeneralsRuntimeRoot
    $baseImmutablePaths = @()
    if ($null -ne $baseBinding) {
        $baseImmutablePaths = @($baseBinding.files | ForEach-Object { Join-Path $baseBinding.runtimeRoot $_.path })
    }
    if ($Diagnostic) {
        $fixtureInput = Read-Stage5NativeFixtureDiagnosticInput -Path $DiagnosticMapPath `
            -ExpectedSha256 $ExpectedDiagnosticMapSha256 -Title $FixtureTitle `
            -ExecutableSha256 $ExecutableSha256 -MapKey $DiagnosticMapKey -Seed $DiagnosticSeed `
            -FrameBudget $DiagnosticFrameBudget -ExpectedInitialUnitCount $DiagnosticExpectedInitialUnits
    }
    else {
    $fixtureInput = Read-Stage5ReviewedNativeKernelFixture `
        -Path $ReviewedManifestPath `
        -ExpectedSha256 $ReviewedManifestSha256 `
        -ExpectedTitle $FixtureTitle `
        -ExpectedSourceCommit $SourceCommit `
        -ExpectedArtifactSetSha256 $ArtifactSetSha256 `
        -ExpectedExecutableSha256 $ExecutableSha256 `
        -ExpectedDependencyManifestSha256 `
            $artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $artifact.runtimeClosure.closureSha256
    }
    $hostTopology = Get-Stage5HostTopology -MinimumPhysicalCores 4 `
        -MaximumPhysicalCores 6 -MaximumLogicalProcessors 12

    $taskFull = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\', '/')
    Assert-Stage5PerformanceCondition ($taskFull.StartsWith('H:\',
        [StringComparison]::OrdinalIgnoreCase)) `
        'TaskRoot must be an explicit fresh task-owned H: path.'
    Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $taskFull)) `
        'TaskRoot must not already exist; fixture production requires a fresh root.'
    New-Item -ItemType Directory -Path $taskFull | Out-Null
    Assert-Stage5FinalAcceptanceNoReparsePath `
        ([IO.Path]::GetPathRoot($taskFull)) $taskFull `
        'Native fixture task root'

    $inputRoot = Join-Path $taskFull 'inputs'
    $logRoot = Join-Path $taskFull 'logs'
    $replayRoot = Join-Path $taskFull 'replays'
    $prelaunchRoot = Join-Path $taskFull 'prelaunch'
    foreach ($directory in @($inputRoot, $logRoot, $replayRoot,
            $prelaunchRoot)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
        Assert-Stage5FinalAcceptanceNoReparsePath $taskFull $directory `
            'Native fixture output directory'
    }
    $bindingLeaf = if ($Diagnostic) { 'DiagnosticMapBinding.json' } else { 'ReviewedFixture.json' }
    $retainedManifestPath = Join-Path $inputRoot $bindingLeaf
    $retainedMapPath = Join-Path $inputRoot `
        ([IO.Path]::GetFileName([string]$fixtureInput.fixture.sourcePath))
    if ($Diagnostic) {
        $document = [ordered]@{
            schemaVersion = 1; evidenceKind = 'stage5-native-fixture-diagnostic-input'
            title = $FixtureTitle; sourceCommit = $SourceCommit; executableSha256 = $ExecutableSha256
            artifactSetSha256 = $ArtifactSetSha256; runtimeClosure = $artifact.runtimeClosure
            mapSource = [IO.Path]::GetFileName($retainedMapPath); mapKey = $fixtureInput.fixture.mapKey
            mapSha256 = $fixtureInput.fixture.sha256; mapByteCount = $fixtureInput.fixture.byteCount
            seed = $fixtureInput.fixture.seed; frameBudget = $fixtureInput.fixture.frameBudget
            expectedInitialUnitCount = $DiagnosticExpectedInitialUnits
            finalAcceptanceClaim = $false; performanceScalingClaim = $false; kernelQualificationClaim = $false
        }
        Write-Stage5JsonAtomically $retainedManifestPath $document -CreateNew
        $ReviewedManifestSha256 = Get-Stage5PerformanceSha256 $retainedManifestPath
        $fixtureInput | Add-Member -NotePropertyName path -NotePropertyValue $retainedManifestPath
    }
    else {
    Write-Stage5FinalAcceptanceFileAtomically -Path $retainedManifestPath `
        -Bytes ([byte[]]$fixtureInput.snapshot.bytes) `
        -Context 'Retained reviewed native fixture manifest' `
        -EvidenceKind JsonReceipt | Out-Null
    }
    Write-Stage5FinalAcceptanceFileAtomically -Path $retainedMapPath `
        -Bytes ([byte[]]$fixtureInput.fixture.sourceSnapshot.bytes) `
        -Context 'Retained reviewed native map' -EvidenceKind RawLog | Out-Null
    Assert-Stage5PerformanceFileHash $retainedManifestPath `
        $ReviewedManifestSha256 'Retained reviewed fixture manifest SHA-256' |
        Out-Null
    Assert-Stage5PerformanceFileHash $retainedMapPath `
        $fixtureInput.fixture.sha256 'Retained reviewed native map SHA-256' |
        Out-Null

    $titleSession = $null
    $validationMutex = $null
    $registrySnapshots = New-Object 'Collections.Generic.List[object]'
    $registrySnapshotKeys = @{}
    $readOnlyLocks = $null
    $processCleanup = [pscustomobject]@{
        processId = 0; exitProof = $true; blocked = $false; errors = @()
    }
    $registryRecovery = $null
    $registryRecoveryPath = Join-Path $taskFull `
        'Stage5FixtureRegistryRecovery.json'
    $rawLogPath = Join-Path $logRoot 'fixture-output.log'
    $stdoutPath = Join-Path $logRoot 'stdout.log'
    $stderrPath = Join-Path $logRoot 'stderr.log'
    $retainedReplayPath = Join-Path $replayRoot 'Stage5Performance.rep'
    $receiptLeaf = if ($Diagnostic) { 'Stage5NativeFixtureDiagnostic.json' } else { 'Stage5NativePerformanceFixture.json' }
    $receiptPath = Join-Path $taskFull $receiptLeaf
    $hostRunNonce = [Guid]::NewGuid().ToString('D')
    $argumentString = Get-Stage5NativePerformanceFixtureArgumentString `
        -MapKey $fixtureInput.fixture.mapKey -Seed $fixtureInput.fixture.seed `
        -FrameBudget $fixtureInput.fixture.frameBudget `
        -ExecutableSha256 $ExecutableSha256
    $titleSessionRoot = Join-Path $taskFull 'TitleSession'
    $profileRelativeRoot = if ($FixtureTitle -ceq 'Generals') {
            'Documents\Command and Conquer Generals Data'
        } else { 'Documents\GGC-LockstepV2-ZeroHour' }
    $plannedProfileRoot = Join-Path $titleSessionRoot $profileRelativeRoot
    $plannedMapPath = Join-Path $plannedProfileRoot `
        ([string]$fixtureInput.fixture.profileRelativePath)
    $planPath = Join-Path $prelaunchRoot 'fixture-plan.json'
    $startPath = Join-Path $prelaunchRoot 'fixture-start.json'
    $plan = [pscustomobject][ordered]@{
        schemaVersion = 1
        event = 'native-fixture-production-plan'
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        hostRunNonce = $hostRunNonce
        cohortNonce = $executionCohort.nonce
        cohortCreatedUtc = $executionCohort.createdUtc
        title = $FixtureTitle
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
        runtimeClosure = [pscustomobject]@{
            dependencyManifestSha256 =
                $artifact.runtimeClosure.dependencyManifestSha256
            closureSha256 = $artifact.runtimeClosure.closureSha256
        }
        executablePath = $executableFull
        executableSha256 = $ExecutableSha256
        reviewedFixtureManifestPath = $fixtureInput.path
        reviewedFixtureManifestSha256 = $ReviewedManifestSha256
        mapSourcePath = $fixtureInput.fixture.sourcePath
        mapSha256 = $fixtureInput.fixture.sha256
        mapDestinationPath = $plannedMapPath
        argumentString = $argumentString
        taskRoot = $taskFull
        timeoutSeconds = $Timeout
        workerCount = 4
        finalAcceptanceClaim = $false
        performanceScalingClaim = $false
    }
    if ($Diagnostic) {
        $plan.event = 'native-fixture-diagnostic-plan'
        $plan.PSObject.Properties.Remove('reviewedFixtureManifestPath')
        $plan.PSObject.Properties.Remove('reviewedFixtureManifestSha256')
        $plan | Add-Member -NotePropertyName diagnosticMapBindingPath -NotePropertyValue $retainedManifestPath
        $plan | Add-Member -NotePropertyName diagnosticMapBindingSha256 -NotePropertyValue $ReviewedManifestSha256
        $plan | Add-Member -NotePropertyName kernelQualificationClaim -NotePropertyValue $false
    }
    Write-Stage5JsonAtomically $planPath $plan -CreateNew
    if ($null -ne $baseBinding) {
        $baseBindingPath = Join-Path $taskFull 'BaseGeneralsBinding.json'
        Write-Stage5JsonAtomically $baseBindingPath $baseBinding -CreateNew
        $baseImmutablePaths += $baseBindingPath
    }
    $planSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $planPath `
        'Native fixture immutable prelaunch plan'

    $primaryError = $null
    $completion = $null
    $capturedOutput = $null
    $processStarted = $false
    $processIdentity = $null
    $rawLogSnapshot = $null
    $lifecycle = [ordered]@{
        mutexAcquired = $false
        noInstalledTitleProcessesAtPreflight = $false
        childExitProven = $false
        registryRestored = $false
        profileRemoved = $false
        recoveryJournalAbsent = $false
    }
    $script:Stage5ActiveRegistryRecovery = $null
    try {
        $titleSession = New-Stage5TitleSessionContract $FixtureTitle `
            $titleSessionRoot $runtimeFull $taskFull `
            -GeneralsRuntimeRoot $(if ($null -ne $baseBinding) { $baseBinding.runtimeRoot } else { '' })
        Assert-Stage5PerformanceCondition (
            $titleSession.profileRoot -ceq $plannedProfileRoot) `
            'Native fixture title profile differs from the immutable plan.'
        $validationMutex = Acquire-Stage5ValidationMutex
        $lifecycle.mutexAcquired = $true
        Assert-Stage5NoInstalledTitleProcesses
        $lifecycle.noInstalledTitleProcessesAtPreflight = $true
        Initialize-Stage5TitleSessionDirectories $titleSession
        $registryRecovery = New-Stage5RegistryRecoveryContext `
            -Title $FixtureTitle -TaskRootPath $taskFull `
            -JournalPath $registryRecoveryPath `
            -ExecutionNonce $executionCohort.nonce `
            -SourceCommit $SourceCommit `
            -ArtifactSetSha256 $ArtifactSetSha256 `
            -ExecutablePath $executableFull `
            -ExecutableSha256 $ExecutableSha256 -RegistryValues $titleSession.registryValues
        $registrySnapshots = $registryRecovery.snapshots
        $registrySnapshotKeys = $registryRecovery.snapshotKeys
        $script:Stage5ActiveRegistryRecovery = $registryRecovery
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
                [Microsoft.Win32.RegistryView]::Registry64)) {
            foreach ($registryValue in $titleSession.registryValues) {
                Add-Stage5RegistryRecoveryMutation $registryRecovery $view `
                    $registryValue
            }
        }
        $mapDestinationDirectory = Split-Path -Parent $plannedMapPath
        New-Item -ItemType Directory -Path $mapDestinationDirectory | Out-Null
        Assert-Stage5FinalAcceptanceNoReparsePath $titleSession.profileRoot `
            $mapDestinationDirectory 'Native fixture map directory'
        Write-Stage5FinalAcceptanceFileAtomically -Path $plannedMapPath `
            -Bytes ([byte[]]$fixtureInput.fixture.sourceSnapshot.bytes) `
            -Context 'Native fixture staged reviewed map' `
            -EvidenceKind RawLog | Out-Null
        Assert-Stage5PerformanceFileHash $plannedMapPath `
            $fixtureInput.fixture.sha256 'Native fixture staged map SHA-256' |
            Out-Null
        $readOnlyLocks = Open-Stage5PerformanceReadOnlyLocks $artifact `
            @([pscustomobject]@{
                path = $fixtureInput.fixture.sourcePath
                sha256 = $fixtureInput.fixture.sha256
            }) $fixtureInput.path (@($planPath) + $baseImmutablePaths)
        if ($null -ne $baseBinding) { Assert-Stage5BaseGeneralsBindingCurrent $baseBinding }
        $start = [pscustomobject][ordered]@{
            schemaVersion = 1
            event = $(if ($Diagnostic) { 'native-fixture-diagnostic-start' } else { 'native-fixture-production-start' })
            recordedUtc = [DateTime]::UtcNow.ToString('o')
            plan = [pscustomobject]@{
                path = $planPath; sha256 = $planSnapshot.sha256
            }
            hostRunNonce = $hostRunNonce
        }
        Write-Stage5JsonAtomically $startPath $start -CreateNew
        $startSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $startPath `
            'Native fixture immutable attempt start'

        $environment = @{}
        foreach ($name in $titleSession.environmentValues.Keys) {
            $environment[$name] = [string]$titleSession.environmentValues[$name]
        }
        $info = New-Stage5ProcessStartInfo $executableFull $argumentString `
            $runtimeFull $environment
        foreach ($name in @($info.EnvironmentVariables.Keys)) {
            if ([string]$name -cmatch '^RTS_PERFORMANCE_' -or
                [string]$name -ceq 'RTS_FRAME_TIMING_DIR' -or
                [string]$name -cmatch '^RTS_STAGE5_(?:RUN_NONCE|COHORT_|RUNTIME_)') {
                $info.EnvironmentVariables.Remove([string]$name)
            }
        }
        $process = New-Stage5NativeFixtureProcess $info
        $processStarted = $false
        $stdoutTask = $null
        $stderrTask = $null
        $runError = $null
        $captureErrors = New-Object 'Collections.Generic.List[string]'
        $stdoutBytes = $null
        $stderrBytes = $null
        $exitCode = $null
        try {
            if ($null -ne $registryRecovery) {
                Add-Stage5RegistryRecoveryPendingProcess $registryRecovery `
                    $executableFull $ExecutableSha256
            }
            $processStarted = $process.Start()
            if ($processStarted) {
                $processCleanup.exitProof = $false
            }
            Assert-Stage5PerformanceCondition $processStarted `
                'Native performance fixture process did not start.'
            $processIdentity = Get-Stage5ProcessIdentity $process
            Assert-Stage5PerformanceCondition (
                [String]::Equals($processIdentity.executablePath,
                    $executableFull, [StringComparison]::OrdinalIgnoreCase) -and
                $processIdentity.executableSha256 -ceq $ExecutableSha256 -and
                $processIdentity.commandLine.EndsWith(
                    ' ' + $argumentString, [StringComparison]::Ordinal)) `
                'Native fixture child identity or command line differs from the immutable plan.'
            if ($null -ne $registryRecovery) {
                Set-Stage5RegistryRecoveryObservedProcess `
                    $registryRecovery $processIdentity
                Update-Stage5RegistryRecoveryState $registryRecovery `
                    'child-running' $false $false
            }
            $stdoutTask = Start-Stage5BoundedOutputCapture `
                $process.StandardOutput.BaseStream ([Int64](64MB)) `
                'Native fixture stdout'
            $stderrTask = Start-Stage5BoundedOutputCapture `
                $process.StandardError.BaseStream ([Int64](64MB)) `
                'Native fixture stderr'
            $timeoutMilliseconds = [Int64]$Timeout * 1000
            $wait = [Diagnostics.Stopwatch]::StartNew()
            while ($true) {
                foreach ($capture in @($stdoutTask, $stderrTask)) {
                    if ($capture.IsFaulted) {
                        throw "Native fixture output capture failed: $($capture.Exception.GetBaseException().Message)"
                    }
                }
                $remaining = $timeoutMilliseconds - $wait.ElapsedMilliseconds
                if ($remaining -le 0) {
                    if ($process.WaitForExit(0)) { break }
                    Stop-Stage5ProcessSafely $process $processIdentity
                    throw "Native fixture process exceeded $Timeout seconds."
                }
                if ($process.WaitForExit([int][Math]::Min([Int64]250,
                            $remaining))) { break }
            }
            $process.WaitForExit()
            $exitCode = [int]$process.ExitCode
        }
        catch { $runError = $_ }
        finally {
            $ownedCleanup = Invoke-Stage5OwnedProcessCleanup $process `
                $processStarted $processIdentity 30000
            $processCleanup.processId = $ownedCleanup.processId
            $processCleanup.exitProof = $ownedCleanup.exitProof
            $processCleanup.blocked = $ownedCleanup.blocked
            $processCleanup.errors = @($ownedCleanup.errors)
            if ($null -ne $registryRecovery -and
                $processStarted -and
                $null -ne $processIdentity) {
                Set-Stage5RegistryRecoveryObservedProcess `
                    $registryRecovery $processIdentity
                if (-not [bool]$processCleanup.blocked) {
                    try {
                        Assert-Stage5NoInstalledTitleProcesses
                        Update-Stage5RegistryRecoveryState $registryRecovery `
                            'active' $true $true
                    }
                    catch {
                        $captureErrors.Add("registry recovery proof: $($_.Exception.Message)") |
                            Out-Null
                    }
                }
            }
            foreach ($capture in @(
                    [pscustomobject]@{ task=$stdoutTask; name='stdout' },
                    [pscustomobject]@{ task=$stderrTask; name='stderr' })) {
                try {
                    if ($null -eq $capture.task -or
                        -not $capture.task.Wait(30000)) {
                        throw "$($capture.name) capture did not close."
                    }
                    $bytes = [byte[]]$capture.task.GetAwaiter().GetResult()
                    if ($capture.name -ceq 'stdout') { $stdoutBytes = $bytes }
                    else { $stderrBytes = $bytes }
                }
                catch {
                    $captureErrors.Add("$($capture.name): $($_.Exception.Message)") |
                        Out-Null
                }
            }
            try { $process.Dispose() }
            catch {
                $captureErrors.Add("process dispose: $($_.Exception.Message)") |
                    Out-Null
            }
        }
        # Retain complete captures before reporting child/capture failures.
        # Missing streams remain unavailable, never relabelled empty/complete.
        $capturedOutput = Write-Stage5NativeFixtureCapturedOutput -TaskRoot $taskFull `
            -StdoutBytes $stdoutBytes -StderrBytes $stderrBytes
        if ($null -ne $runError) { throw $runError }
        Assert-Stage5PerformanceCondition ($captureErrors.Count -eq 0) `
            "Native fixture output capture failed: $($captureErrors.ToArray() -join ' | ')"
        Assert-Stage5PerformanceCondition (-not $processCleanup.blocked -and
            $processCleanup.exitProof -and $exitCode -eq 0) `
            'Native fixture child did not exit cleanly with identity-bound proof.'
        $lifecycle.childExitProven = $true
        $rawLogSnapshot = $capturedOutput.rawLogSnapshot
        $diagnosticText = ConvertFrom-Stage5StrictUtf8OutputPair `
            $stdoutBytes $stderrBytes
        if ($Diagnostic) {
            $completion = ConvertFrom-Stage5NativeFixtureObservation -Text $diagnosticText `
                -MapBinding $fixtureInput -ExpectedProcessId $processIdentity.processId `
                -ProfileRoot $titleSession.profileRoot
        }
        else {
            $completion = ConvertFrom-Stage5NativePerformanceFixtureOutput `
                -Text $diagnosticText -ReviewedFixture $fixtureInput `
                -ExpectedProcessId $processIdentity.processId -ProfileRoot $titleSession.profileRoot
        }
        Write-Stage5FinalAcceptanceFileAtomically -Path $retainedReplayPath `
            -Bytes ([byte[]]$completion.retainedReplaySnapshot.bytes) `
            -Context 'Native fixture replay retention' -EvidenceKind Replay |
            Out-Null
        Assert-Stage5PerformanceFileHash $retainedReplayPath `
            $completion.replaySha256 'Retained native fixture replay SHA-256' |
            Out-Null
    }
    catch { $primaryError = $_ }
    finally {
        $cleanupErrors = New-Object 'Collections.Generic.List[string]'
        if ($processStarted -and $null -eq $processIdentity) {
            $processCleanup.blocked = $true
            $cleanupErrors.Add('Native fixture child identity was not captured; registry/profile cleanup is deferred.') | Out-Null
        }
        if ($processStarted -and
            -not [bool]$processCleanup.exitProof) {
            $processCleanup.blocked = $true
            $cleanupErrors.Add('Native fixture child exit proof was not recorded; registry/profile cleanup is deferred.') | Out-Null
        }
        if ($null -ne $registryRecovery -and
            @($registryRecovery.processIdentities.ToArray() | Where-Object {
                [bool]$_.launchPending
            }).Count -gt 0) {
            $processCleanup.blocked = $true
            $cleanupErrors.Add('A Native fixture child launch remained pending in the recovery journal; registry/profile cleanup is deferred.') | Out-Null
        }
        if ($null -ne $readOnlyLocks) {
            try { Dispose-Stage5PerformanceReadOnlyLocks $readOnlyLocks }
            catch {
                $cleanupErrors.Add("read-only lock cleanup: $($_.Exception.Message)") |
                    Out-Null
            }
        }
        $recoveryJournalExists = Test-Path -LiteralPath $registryRecoveryPath -PathType Leaf
        if (-not $recoveryJournalExists -and $null -eq $registryRecovery) {
            $lifecycle.registryRestored = $true
        }
        elseif (-not $recoveryJournalExists) {
            $cleanupErrors.Add('Stage 5 registry recovery journal disappeared before restoration; registry restoration is deferred.') | Out-Null
        }
        elseif ($null -eq $registryRecovery) {
            $cleanupErrors.Add('Recovery journal exists without its in-memory recovery context; registry restoration is deferred.') | Out-Null
        }
        elseif ($processCleanup.blocked) {
            try {
                Update-Stage5RegistryRecoveryState $registryRecovery `
                    'child-exit-unproven' $false $false
            }
            catch {
                $cleanupErrors.Add("recovery journal update: $($_.Exception.Message)") |
                    Out-Null
            }
        }
        else {
            $recoveryAttempted = $false
            try {
                Assert-Stage5NoInstalledTitleProcesses
                Update-Stage5RegistryRecoveryState $registryRecovery `
                    'active' $true $true
                $recoveryAuthorization = New-Stage5RegistryRecoveryAuthorization `
                    $registryRecovery
                $recoveryAttempted = $true
                Invoke-Stage5RegistryRecovery -Path $registryRecovery.path `
                    -ExpectedIdentity $registryRecovery.identity `
                    -Authorization $recoveryAuthorization `
                    -MutexLock $validationMutex `
                    -Adapter $registryRecovery.adapter | Out-Null
                $lifecycle.registryRestored = $true
            }
            catch {
                $cleanupErrors.Add("registry restoration: $($_.Exception.Message)") |
                    Out-Null
                if (-not $recoveryAttempted) {
                    try {
                        Update-Stage5RegistryRecoveryState $registryRecovery `
                            'child-exit-unproven' $false $false
                    }
                    catch {
                        $cleanupErrors.Add("recovery journal update: $($_.Exception.Message)") |
                            Out-Null
                    }
                }
            }
        }
        if ($lifecycle.registryRestored) {
            if ($lifecycle.registryRestored -and $null -ne $titleSession -and
                -not ($Diagnostic -and $null -ne $primaryError)) {
                try {
                    Remove-Stage5TitleSessionDirectories $titleSession $taskFull
                    $lifecycle.profileRemoved = -not (Test-Path -LiteralPath `
                        $titleSession.sessionRoot)
                }
                catch {
                    $cleanupErrors.Add("title profile cleanup: $($_.Exception.Message)") |
                        Out-Null
                }
            }
            if ($lifecycle.registryRestored -and
                (Test-Path -LiteralPath $registryRecoveryPath)) {
                try { Remove-Item -LiteralPath $registryRecoveryPath -Force }
                catch {
                    $cleanupErrors.Add("recovery journal cleanup: $($_.Exception.Message)") |
                        Out-Null
                }
            }
            $lifecycle.recoveryJournalAbsent =
                -not (Test-Path -LiteralPath $registryRecoveryPath)
            try { Release-Stage5ValidationMutex $validationMutex }
            catch {
                $cleanupErrors.Add("validation mutex cleanup: $($_.Exception.Message)") |
                    Out-Null
            }
        }
        if ($Diagnostic -and ($null -ne $primaryError -or $cleanupErrors.Count -gt 0)) {
            $failureText = if ($null -ne $primaryError) { $primaryError.Exception.Message } else { 'cleanup failure' }
            $failure = New-Stage5NativeFixtureDiagnosticResult -Status failed -ErrorText $failureText `
                -Lifecycle ([pscustomobject]$lifecycle) -ProcessIdentity $processIdentity `
                -ExpectedInitialUnitCount $DiagnosticExpectedInitialUnits -CleanupErrors @($cleanupErrors.ToArray())
            $failure.inputBinding = [ordered]@{ path = $retainedManifestPath; sha256 = $ReviewedManifestSha256 }
            $failure.prelaunchPlan = [ordered]@{ path = $planPath; sha256 = $planSnapshot.sha256 }
            $failure.capturedOutput = [ordered]@{
                stdoutAvailable = ($null -ne $capturedOutput -and $capturedOutput.stdoutAvailable)
                stderrAvailable = ($null -ne $capturedOutput -and $capturedOutput.stderrAvailable)
                combinedLogAvailable = ($null -ne $capturedOutput -and $null -ne $capturedOutput.rawLogSnapshot)
            }
            $failure.retainedProfileRoot = if ($null -ne $titleSession -and
                (Test-Path -LiteralPath $titleSession.sessionRoot)) { $titleSession.sessionRoot } else { $null }
            Write-Stage5JsonAtomically $receiptPath $failure -CreateNew
        }
        if ($null -ne $primaryError) {
            if ($cleanupErrors.Count -gt 0) {
                throw "Native fixture production failed: $($primaryError.Exception.Message); cleanup also failed: $($cleanupErrors.ToArray() -join ' | ')"
            }
            throw $primaryError
        }
        if ($cleanupErrors.Count -gt 0) {
            throw "Native fixture cleanup failed: $($cleanupErrors.ToArray() -join ' | ')"
        }
    }
    foreach ($name in $lifecycle.Keys) {
        Assert-Stage5PerformanceCondition ([bool]$lifecycle[$name]) `
            "Native fixture final publication lacks lifecycle proof '$name'."
    }
    if ($Diagnostic) {
        $result = New-Stage5NativeFixtureDiagnosticResult -Status observed -Completion $completion `
            -Lifecycle ([pscustomobject]$lifecycle) -ProcessIdentity $processIdentity `
            -ExpectedInitialUnitCount $DiagnosticExpectedInitialUnits
        $result.inputBinding = [ordered]@{ path = $retainedManifestPath; sha256 = $ReviewedManifestSha256 }
        $result.prelaunchPlan = [ordered]@{ path = $planPath; sha256 = $planSnapshot.sha256 }
        $result.rawLog = [ordered]@{ path = $rawLogPath; sha256 = $rawLogSnapshot.sha256 }
        $result.replay = [ordered]@{ path = $retainedReplayPath; sha256 = $completion.replaySha256 }
        Write-Stage5JsonAtomically $receiptPath $result -CreateNew
        Write-Output $receiptPath
        return
    }
    $receipt = New-Stage5NativePerformanceFixtureProductionReceipt `
        -Title $FixtureTitle `
        -RecordedUtc ([DateTime]::UtcNow.ToString('o')) `
        -CohortNonce $executionCohort.nonce `
        -CohortCreatedUtc $executionCohort.createdUtc `
        -SourceCommit $SourceCommit -ArtifactSetSha256 $ArtifactSetSha256 `
        -DependencyManifestSha256 `
            $artifact.runtimeClosure.dependencyManifestSha256 `
        -RuntimeClosureSha256 $artifact.runtimeClosure.closureSha256 `
        -ReviewedFixtureManifestPath 'inputs/ReviewedFixture.json' `
        -ReviewedFixtureManifestSha256 $ReviewedManifestSha256 `
        -PrelaunchPlanPath 'prelaunch/fixture-plan.json' `
        -PrelaunchPlanSha256 $planSnapshot.sha256 `
        -AttemptStartPath 'prelaunch/fixture-start.json' `
        -AttemptStartSha256 $startSnapshot.sha256 `
        -ExecutableSha256 $ExecutableSha256 -HostRunNonce $hostRunNonce `
        -ProcessId $processIdentity.processId `
        -ProcessCreationTimeUtc100ns $processIdentity.creationTimeUtc100ns `
        -CommandLine $processIdentity.commandLine `
        -ArgumentString $argumentString `
        -TaskRoot $taskFull -NativeProfileRoot $titleSession.profileRoot `
        -Completion $completion `
        -RawLogPath 'logs/fixture-output.log' `
        -RawLogSha256 $rawLogSnapshot.sha256 `
        -RetainedReplayPath 'replays/Stage5Performance.rep' `
        -Lifecycle ([pscustomobject]$lifecycle)
    Write-Stage5JsonAtomically $receiptPath $receipt -CreateNew
    $receiptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $receiptPath `
        'Native fixture production final receipt'
    $reopened = Read-Stage5NativePerformanceFixtureProductionReceipt `
        -Path $receiptPath -ExpectedSha256 $receiptSnapshot.sha256 `
        -ExpectedTitle $FixtureTitle `
        -ExpectedCohortNonce $executionCohort.nonce `
        -ExpectedCohortCreatedUtc $executionCohort.createdUtc `
        -ExpectedSourceCommit $SourceCommit `
        -ExpectedArtifactSetSha256 $ArtifactSetSha256 `
        -ExpectedExecutableSha256 $ExecutableSha256 `
        -ExpectedDependencyManifestSha256 `
            $artifact.runtimeClosure.dependencyManifestSha256 `
        -ExpectedRuntimeClosureSha256 $artifact.runtimeClosure.closureSha256
    Assert-Stage5PerformanceCondition ($reopened.fixture.sha256 -ceq
        $completion.replaySha256) `
        'Native fixture production failed its independent final receipt closure.'
    Write-Output $receiptPath
}

if ($PSCmdlet.ParameterSetName -ceq 'NativeFixtureDiagnostic') {
    Assert-Stage5PerformanceCondition ([bool]$RunNativeFixtureDiagnostic) 'Native fixture diagnostic requires explicit opt-in.'
    Invoke-Stage5NativePerformanceFixtureProduction -Diagnostic `
        -FixtureTitle $Title -ExecutablePath $InstalledExecutablePath `
        -ExecutableSha256 $ExpectedExecutableSha256 -GeneralsRuntimeRoot $GeneralsInstallRoot `
        -SourceCommit $ExpectedSourceCommit -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ArtifactManifestPath $ArtifactSetManifestPath -CohortNonce $ExecutionCohortNonce `
        -CohortCreatedUtc $ExecutionCohortCreatedUtc -OutputRoot $TaskRoot -Timeout $TimeoutSeconds
    return
}

if ($PSCmdlet.ParameterSetName -ceq 'FixtureProduction') {
    Invoke-Stage5NativePerformanceFixtureProduction `
        -FixtureTitle $Title `
        -ExecutablePath $InstalledExecutablePath `
        -ExecutableSha256 $ExpectedExecutableSha256 `
        -GeneralsRuntimeRoot $GeneralsInstallRoot `
        -SourceCommit $ExpectedSourceCommit `
        -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ArtifactManifestPath $ArtifactSetManifestPath `
        -ReviewedManifestPath $ReviewedFixtureManifestPath `
        -ReviewedManifestSha256 $ExpectedReviewedFixtureManifestSha256 `
        -CohortNonce $ExecutionCohortNonce `
        -CohortCreatedUtc $ExecutionCohortCreatedUtc `
        -OutputRoot $TaskRoot -Timeout $TimeoutSeconds
    return
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

$executionCohort = Resolve-Stage5PerformanceExecutionCohort `
    $QualificationMode $ExecutionCohortNonce $ExecutionCohortCreatedUtc `
    $PSBoundParameters.ContainsKey('ExecutionCohortNonce') `
    $PSBoundParameters.ContainsKey('ExecutionCohortCreatedUtc')
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
$isInstalledKernelExecution = $QualificationMode -ceq
    'InstalledKernelExecution'
if ($isExternalQualification) {
    Assert-Stage5PerformanceSourceCommit $ExpectedStage3SourceCommit `
        'ExpectedStage3SourceCommit'
    foreach ($binding in @(
            @('ExpectedStage3BaselineSha256', $ExpectedStage3BaselineSha256),
            @('ExpectedStage3ExecutableSha256', $ExpectedStage3ExecutableSha256),
            @('ExpectedPhaseBaselineProfileSha256',
                $ExpectedPhaseBaselineProfileSha256),
            @('ExpectedPerformanceDataManifestSha256',
                $ExpectedPerformanceDataManifestSha256),
            @('ExpectedPerformanceDataClosureSha256',
                $ExpectedPerformanceDataClosureSha256))) {
        Assert-Stage5PerformanceHash ([string]$binding[1]) ([string]$binding[0])
    }
    Assert-Stage5PerformanceCondition ($ReferencePolicy -ceq
        'paired-serial-oracle-v1') `
        'External16Core authoritative evidence requires paired-serial-oracle-v1.'
    Assert-Stage5PerformanceCondition (@($PhaseBaselineProfiles).Count -eq 1) `
        'External16Core authoritative evidence requires one reviewed phase-baseline profile.'
    Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace(
        $PhaseBaselineProfilePath)) `
        'External16Core authoritative evidence requires PhaseBaselineProfilePath.'
    Assert-Stage5PerformanceCondition (-not [string]::IsNullOrWhiteSpace(
        $PerformanceDataManifestPath)) `
        'External16Core authoritative evidence requires PerformanceDataManifestPath.'
}
else {
    Assert-Stage5PerformanceCondition (-not
        ($PSBoundParameters.ContainsKey('PerformanceDataManifestPath') -or
         $PSBoundParameters.ContainsKey('ExpectedPerformanceDataManifestSha256') -or
         $PSBoundParameters.ContainsKey('ExpectedPerformanceDataClosureSha256'))) `
        "$QualificationMode cannot accept authoritative performance qualification data."
}
if ($isInstalledKernelExecution) {
    Assert-Stage5PerformanceCondition ($ReferencePolicy -ceq
        'throughput-only') `
        'InstalledKernelExecution is kernel-execution-only and cannot claim a serial oracle or scaling reference.'
    Assert-Stage5PerformanceCondition (@($PhaseBaselineProfiles).Count -eq 0 -and
        -not $PSBoundParameters.ContainsKey('PhaseBaselineProfilePath') -and
        -not $PSBoundParameters.ContainsKey('ExpectedPhaseBaselineProfileSha256') -and
        -not $PSBoundParameters.ContainsKey('Stage3BaselinePath') -and
        -not $PSBoundParameters.ContainsKey('ExpectedStage3BaselineSha256') -and
        -not $PSBoundParameters.ContainsKey('ExpectedStage3ExecutableSha256') -and
        -not $PSBoundParameters.ContainsKey('ExpectedStage3SourceCommit')) `
        'InstalledKernelExecution cannot accept Stage 3, phase-baseline, or scaling inputs.'
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
Assert-Stage5ProcessLocalProfileCapability $executableFull `
    -Context 'Installed performance executable' | Out-Null
$runtimeFull = Split-Path -Parent $executableFull
$launcherContract = Get-Stage5LauncherContract $runtimeFull $executableFull
$artifactBinding = Read-Stage5PerformanceArtifactSet $ArtifactSetManifestPath `
    $ExpectedArtifactSetSha256 $ExpectedSourceCommit $Title $executableFull `
    $ExpectedExecutableSha256
Assert-Stage5PerformanceLauncherBinding $artifactBinding $launcherContract $Title
$baseBinding = Get-Stage5PerformanceBaseBinding $Title $runtimeFull $artifactBinding $GeneralsInstallRoot `
    -RequireQualification $isExternalQualification `
    -QualificationManifestPath $GeneralsQualificationDataManifestPath `
    -QualificationManifestSha256 $GeneralsQualificationDataManifestSha256
$taskFull = [IO.Path]::GetFullPath($TaskRoot).TrimEnd('\', '/')
Assert-Stage5PerformanceCondition ($taskFull.StartsWith('H:\',
    [StringComparison]::OrdinalIgnoreCase)) `
    'TaskRoot must be an explicit fresh task-owned H: path.'
Assert-Stage5PerformanceCondition (-not (Test-Path -LiteralPath $taskFull)) `
    'TaskRoot must not already exist; qualification requires a fresh root.'

$performanceData = $null
if ($isExternalQualification) {
    $performanceDataSourcePath = [IO.Path]::GetFullPath(
        $PerformanceDataManifestPath)
    $performanceDataSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
        $performanceDataSourcePath `
        'Stage 5 performance qualification-data input'
    $performanceData = Read-Stage5PerformanceQualificationData `
        $performanceDataSourcePath $ExpectedPerformanceDataManifestSha256 `
        $ExpectedPerformanceDataClosureSha256 $ExpectedSourceCommit $Title `
        $runtimeFull $artifactBinding $performanceDataSnapshot
}
$fixtureProduction = $null
$fixtureManifest = if ($isInstalledKernelExecution) {
    $fixtureProduction =
        Read-Stage5NativePerformanceFixtureProductionReceipt `
            -Path $FixtureManifestPath `
            -ExpectedSha256 $ExpectedFixtureManifestSha256 `
            -ExpectedTitle $Title `
            -ExpectedCohortNonce $executionCohort.nonce `
            -ExpectedCohortCreatedUtc $executionCohort.createdUtc `
            -ExpectedSourceCommit $ExpectedSourceCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
            -ExpectedExecutableSha256 $ExpectedExecutableSha256 `
            -ExpectedDependencyManifestSha256 `
                $artifactBinding.runtimeClosure.dependencyManifestSha256 `
            -ExpectedRuntimeClosureSha256 `
                $artifactBinding.runtimeClosure.closureSha256
    [pscustomobject]@{
        path = $fixtureProduction.path
        sha256 = $fixtureProduction.sha256
        fixtures = @($fixtureProduction.fixture)
    }
}
else {
    Read-Stage5ScalingFixtureManifest $FixtureManifestPath `
        $ExpectedFixtureManifestSha256 $Title $ExpectedExecutableSha256
}
$fixtureManifestSourceSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
    $fixtureManifest.path 'Reviewed fixture manifest retention input'
Assert-Stage5FinalAcceptanceSnapshotSha256 $fixtureManifestSourceSnapshot `
    $ExpectedFixtureManifestSha256 'Reviewed fixture manifest retention input' |
    Out-Null
$fixtureManifestRetentionDocument =
    ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
        $fixtureManifestSourceSnapshot 'Reviewed fixture manifest retention input' `
        -AsPsObject
$baseline = $null
$baselineSnapshot = $null
$phaseProfileSnapshot = $null
$phaseBaselineProfile = $null
if ($isExternalQualification) {
    $baselineSourcePath = [IO.Path]::GetFullPath($Stage3BaselinePath)
    $baselineSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
        $baselineSourcePath 'Stage 3 protected scaling baseline input'
    $baseline = Read-Stage5ScalingBaseline $Stage3BaselinePath `
        $ExpectedStage3BaselineSha256 $ExpectedStage3ExecutableSha256 $Title `
        $ExpectedFixtureManifestSha256 $fixtureManifest.fixtures `
        $ExpectedStage3SourceCommit $baselineSnapshot

    $phaseProfileSourcePath = [IO.Path]::GetFullPath($PhaseBaselineProfilePath)
    $phaseProfileSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
        $phaseProfileSourcePath 'Reviewed Stage 5 phase-baseline profile input'
    Assert-Stage5FinalAcceptanceSnapshotSha256 $phaseProfileSnapshot `
        $ExpectedPhaseBaselineProfileSha256 `
        'Reviewed Stage 5 phase-baseline profile SHA-256' | Out-Null
    $phaseBaselineProfile = ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
        $phaseProfileSnapshot 'Reviewed Stage 5 phase-baseline profile' -AsPsObject
    Assert-Stage5PerformanceCondition ($null -ne $phaseBaselineProfile -and
        $phaseBaselineProfile -isnot [Array] -and
        ($phaseBaselineProfile | ConvertTo-Json -Compress -Depth 20) -ceq
            ($PhaseBaselineProfiles[0] | ConvertTo-Json -Compress -Depth 20)) `
        'PhaseBaselineProfiles differs from the independently hashed reviewed profile file.'
    Assert-Stage5PerformanceProperties $phaseBaselineProfile @('profileId',
        'fixtureId', 'sourceLane', 'sourcePolicySha256', 'limits',
        'residentAttemptCapacity', 'residentRangeCapacity', 'fixtureSha256',
        'window', 'warmupRuns', 'measuredRuns') `
        'Reviewed Stage 5 phase-baseline profile'
    Assert-Stage5PerformanceCondition ($phaseBaselineProfile.fixtureId -ceq
            'dense-eight-player' -and
        $phaseBaselineProfile.sourceLane -ceq 'forced-one' -and
        $phaseBaselineProfile.fixtureSha256 -ceq
            $fixtureManifest.fixtures[3].sha256 -and
        (Test-Stage5JsonInteger $phaseBaselineProfile.warmupRuns) -and
        [int]$phaseBaselineProfile.warmupRuns -eq $script:WarmupRuns -and
        (Test-Stage5JsonInteger $phaseBaselineProfile.measuredRuns) -and
        [int]$phaseBaselineProfile.measuredRuns -eq $MeasuredRuns) `
        'Reviewed phase-baseline profile must select dense-eight-player/forced-one with the exact schedule.'
    $PhaseBaselineProfiles = @($phaseBaselineProfile)
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
if ($null -ne $baseBinding) {
    if ($baseBinding.identityMode -ceq 'acceptance-bound') {
        $baseEvidenceRoot = Join-Path $taskFull 'BaseGenerals'
        New-Item -ItemType Directory -Path $baseEvidenceRoot | Out-Null
        $baseManifestCopy = Join-Path $baseEvidenceRoot 'QualificationData.json'
        Copy-Item -LiteralPath $GeneralsQualificationDataManifestPath -Destination $baseManifestCopy
        Assert-Stage5PerformanceFileHash $baseManifestCopy $GeneralsQualificationDataManifestSha256 `
            'Retained base Generals qualification data' | Out-Null
        $baseBinding.retainedQualificationData = [ordered]@{
            path='BaseGenerals/QualificationData.json';sha256=$GeneralsQualificationDataManifestSha256.ToUpperInvariant()
        }
    }
    Write-Stage5JsonAtomically (Join-Path $taskFull 'BaseGeneralsBinding.json') $baseBinding -CreateNew
}
$stagedFixtureManifestPath = $fixtureManifest.path
if ($isExternalQualification) {
    $stagedFixtureDirectory = Join-Path $taskFull 'ReviewedFixtures'
    New-Item -ItemType Directory -Path $stagedFixtureDirectory | Out-Null
    Assert-Stage5FinalAcceptanceNoReparsePath $taskFull $stagedFixtureDirectory `
        'Retained reviewed fixture directory'
    $stagedFixtureManifestPath = Join-Path $stagedFixtureDirectory `
        ([IO.Path]::GetFileName($fixtureManifest.path))
    Write-Stage5FinalAcceptanceFileAtomically -Path $stagedFixtureManifestPath `
        -Bytes $fixtureManifestSourceSnapshot.bytes `
        -Context 'Reviewed fixture manifest retention' `
        -EvidenceKind JsonReceipt | Out-Null
    Assert-Stage5PerformanceFileHash $stagedFixtureManifestPath `
        $ExpectedFixtureManifestSha256 'Retained reviewed fixture manifest SHA-256' |
        Out-Null
    $stagedFixturePaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    [void]$stagedFixturePaths.Add([IO.Path]::GetFullPath(
        $stagedFixtureManifestPath))
    for ($fixtureIndex = 0; $fixtureIndex -lt
            $fixtureManifest.fixtures.Count; ++$fixtureIndex) {
        $fixture = $fixtureManifest.fixtures[$fixtureIndex]
        $retainedFixture = $fixtureManifestRetentionDocument.fixtures[$fixtureIndex]
        Assert-Stage5PerformanceCondition ($retainedFixture.id -ceq $fixture.id -and
            $retainedFixture.sha256 -ceq $fixture.sha256) `
            "Reviewed fixture '$($fixture.id)' retention identity changed."
        $relativeSource = [string]$retainedFixture.source
        Assert-Stage5PerformanceCondition (-not
            [string]::IsNullOrWhiteSpace($relativeSource) -and
            -not [IO.Path]::IsPathRooted($relativeSource) -and
            $relativeSource -notmatch ':' -and
            $relativeSource -notmatch '(^|[\\/])\.\.?([\\/]|$)') `
            "Reviewed fixture '$($fixture.id)' source is not a canonical relative path."
        $stagedFixturePath = [IO.Path]::GetFullPath((Join-Path `
            $stagedFixtureDirectory $relativeSource))
        Assert-Stage5PerformanceCondition ($stagedFixturePath.StartsWith(
                $stagedFixtureDirectory + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase) -and
            $stagedFixturePaths.Add($stagedFixturePath)) `
            "Reviewed fixture '$($fixture.id)' retention path escapes or aliases."
        $stagedFixtureParent = Split-Path -Parent $stagedFixturePath
        if (-not (Test-Path -LiteralPath $stagedFixtureParent -PathType Container)) {
            New-Item -ItemType Directory -Path $stagedFixtureParent -Force |
                Out-Null
        }
        Assert-Stage5FinalAcceptanceNoReparsePath $stagedFixtureDirectory `
            $stagedFixtureParent "Reviewed fixture '$($fixture.id)' retention directory"
        $fixtureSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $fixture.path `
            "Reviewed fixture '$($fixture.id)' retention input" `
            -EvidenceKind Replay
        Assert-Stage5FinalAcceptanceSnapshotSha256 $fixtureSnapshot `
            $fixture.sha256 "Reviewed fixture '$($fixture.id)' retention input" |
            Out-Null
        Write-Stage5FinalAcceptanceFileAtomically -Path $stagedFixturePath `
            -Bytes $fixtureSnapshot.bytes `
            -Context "Reviewed fixture '$($fixture.id)' retention" `
            -EvidenceKind Replay | Out-Null
        Assert-Stage5PerformanceFileHash $stagedFixturePath $fixture.sha256 `
            "Retained reviewed fixture '$($fixture.id)' SHA-256" | Out-Null
        $fixture.path = $stagedFixturePath
    }
    $fixtureManifest.path = $stagedFixtureManifestPath
}
$stagedBaselinePath = $null
$stagedPhaseProfilePath = $null
$stagedPerformanceDataPath = $null
if ($isExternalQualification) {
    $stagedBaselinePath = Join-Path $taskFull 'Stage3PerformanceBaseline.json'
    $stagedPhaseProfilePath = Join-Path $taskFull `
        'Stage5PerformancePhaseBaselineProfile.json'
    $stagedPerformanceDataPath = Join-Path $taskFull `
        'Stage5PerformanceQualificationData.json'
    try {
        Write-Stage5FinalAcceptanceFileAtomically -Path $stagedBaselinePath `
            -Bytes $baselineSnapshot.bytes `
            -Context 'Stage 3 protected scaling baseline retention' `
            -EvidenceKind JsonReceipt | Out-Null
        Write-Stage5FinalAcceptanceFileAtomically -Path $stagedPhaseProfilePath `
            -Bytes $phaseProfileSnapshot.bytes `
            -Context 'Stage 5 reviewed phase-baseline profile retention' `
            -EvidenceKind JsonReceipt | Out-Null
        Write-Stage5FinalAcceptanceFileAtomically -Path $stagedPerformanceDataPath `
            -Bytes $performanceData.snapshot.bytes `
            -Context 'Stage 5 performance qualification-data retention' `
            -EvidenceKind JsonReceipt | Out-Null
        Assert-Stage5PerformanceFileHash $stagedBaselinePath `
            $ExpectedStage3BaselineSha256 `
            'Retained Stage 3 baseline SHA-256' | Out-Null
        Assert-Stage5PerformanceFileHash $stagedPhaseProfilePath `
            $ExpectedPhaseBaselineProfileSha256 `
            'Retained phase-baseline profile SHA-256' | Out-Null
        Assert-Stage5PerformanceFileHash $stagedPerformanceDataPath `
            $ExpectedPerformanceDataManifestSha256 `
            'Retained performance qualification-data SHA-256' | Out-Null
        $baseline.path = $stagedBaselinePath
    }
    catch {
        $taskRootItem = Get-Item -LiteralPath $taskFull -Force
        if ($taskRootItem.PSIsContainer -and
            ($taskRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            Remove-Item -LiteralPath $taskFull -Recurse -Force
        }
        throw
    }
}

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
    cohortNonce = $executionCohort.nonce
    cohortCreatedUtc = $executionCohort.createdUtc
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
if ($isExternalQualification) {
    $context | Add-Member NoteProperty performanceData ([pscustomobject]@{
        sourceManifestPath = [string]$performanceData.path
        path = [string]$stagedPerformanceDataPath
        sha256 = [string]$performanceData.manifestSha256
        closureSha256 = [string]$performanceData.closureSha256
        runtimeRoot = [string]$performanceData.runtimeRoot
        fileCount = [int]$performanceData.fileCount
        filePaths = @($performanceData.filePaths)
    })
}
elseif ($isInstalledKernelExecution) {
    $context | Add-Member NoteProperty fixtureProductionReceipt `
        ([pscustomobject]@{
            path = [string]$fixtureProduction.path
            sha256 = [string]$fixtureProduction.sha256
        })
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
$runPlanBinding = $null
$runPlanExecution = $null
$phaseAttemptManifestBinding = $null
$validationManifestBinding = $null
$validationMutex = $null
$registryRecovery = $null
$script:Stage5ActiveRegistryRecovery = $null
try {
    $titleSession = New-Stage5TitleSessionContract $Title $titleSessionRoot `
        $runtimeFull $taskFull `
        -GeneralsRuntimeRoot $(if ($null -ne $baseBinding) { $baseBinding.runtimeRoot } else { '' })
    $validationMutex = Acquire-Stage5ValidationMutex
    Assert-Stage5NoInstalledTitleProcesses
    Initialize-Stage5TitleSessionDirectories $titleSession
    $registryRecovery = New-Stage5RegistryRecoveryContext `
        -Title $Title -TaskRootPath $taskFull `
        -JournalPath $registryRecoveryPath `
        -ExecutionNonce $executionCohort.nonce `
        -SourceCommit $ExpectedSourceCommit `
        -ArtifactSetSha256 $ExpectedArtifactSetSha256 `
        -ExecutablePath $executableFull `
        -ExecutableSha256 $ExpectedExecutableSha256 -RegistryValues $titleSession.registryValues
    $registrySnapshots = $registryRecovery.snapshots
    $registrySnapshotKeys = $registryRecovery.snapshotKeys
    $script:Stage5ActiveRegistryRecovery = $registryRecovery
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32,
        [Microsoft.Win32.RegistryView]::Registry64)) {
        foreach ($registryValue in $titleSession.registryValues) {
            Add-Stage5RegistryRecoveryMutation $registryRecovery $view `
                $registryValue
        }
    }
    $profileBefore = Get-Stage5ProfileTreeHash $titleSession.profileRoot
    $additionalImmutablePaths = if ($isExternalQualification) {
        @($context.performanceData.sourceManifestPath,
            $context.performanceData.path) + @($context.performanceData.filePaths)
    }
    elseif ($isInstalledKernelExecution) {
        @($fixtureProduction.filePaths)
    }
    else { @() }
    if ($null -ne $baseBinding) {
        $additionalImmutablePaths += @($baseBinding.files | ForEach-Object { Join-Path $baseBinding.runtimeRoot $_.path })
        $additionalImmutablePaths += (Join-Path $taskFull 'BaseGeneralsBinding.json')
        if ($baseBinding.identityMode -ceq 'acceptance-bound') { $additionalImmutablePaths += $baseManifestCopy }
    }
    $readOnlyLocks = Open-Stage5PerformanceReadOnlyLocks `
        $artifactBinding $fixtureManifest.fixtures $fixtureManifest.path `
        $additionalImmutablePaths
    if ($null -ne $baseBinding) { Assert-Stage5BaseGeneralsBindingCurrent $baseBinding }
    $runPlanBinding = New-Stage5PerformanceRunPlan $context $TimeoutSeconds `
        $titleSession @($PhaseBaselineProfiles)
    $runPlanExecution = Invoke-Stage5PerformanceRunPlan $context $runPlanBinding $titleSession
    $context.runs = $runPlanExecution.runs
    $context.pairedOracleBindings = $runPlanExecution.pairedOracleBindings
    if ($null -ne $runPlanExecution.failure) { throw $runPlanExecution.failure.message }
    if (@($PhaseBaselineProfiles).Count -gt 0) {
        $phaseAttemptManifestPath = Join-Path $taskFull 'phase-attempts.json'
        Write-Stage5JsonAtomically $phaseAttemptManifestPath ([pscustomobject][ordered]@{
            schemaVersion=1; planSha256=$runPlanBinding.sha256
            outcomes=@($runPlanExecution.outcomes); cohortFailure=$null
        }) -CreateNew
        $phaseAttemptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot `
            $phaseAttemptManifestPath 'Completed phase attempt manifest'
        $phaseAttemptManifestBinding = [pscustomobject]@{
            path=$phaseAttemptManifestPath; sha256=$phaseAttemptSnapshot.sha256
        }
        $context | Add-Member NoteProperty phaseBaselinePolicy `
            'paired-source-admissions-v1'
        $context | Add-Member NoteProperty phaseBaselineProfiles `
            @($PhaseBaselineProfiles)
        $context | Add-Member NoteProperty phaseBaselinePlan $runPlanBinding
        $context | Add-Member NoteProperty phaseBaselineAttemptManifest `
            $phaseAttemptManifestBinding
        $context | Add-Member NoteProperty pairedPhaseBaselineBindings `
            @($runPlanExecution.pairedPhaseBaselineBindings)
    }
    # Cleanup state belongs to the live host context, not the closed evidence
    # document. Copy all other fields so unknown evidence remains rejectable.
    $validated = Assert-Stage5PerformanceRunSet (
        $context | Select-Object -Property * -ExcludeProperty processCleanup)
    if ($isExternalQualification) {
        $finalPerformanceData = Read-Stage5PerformanceQualificationData `
            $performanceData.path $ExpectedPerformanceDataManifestSha256 `
            $ExpectedPerformanceDataClosureSha256 $ExpectedSourceCommit $Title `
            $runtimeFull $artifactBinding $performanceData.snapshot
        Assert-Stage5PerformanceCondition ($finalPerformanceData.manifestSha256 -ceq
                $performanceData.manifestSha256 -and
            $finalPerformanceData.closureSha256 -ceq $performanceData.closureSha256 -and
            $finalPerformanceData.fileCount -eq $performanceData.fileCount -and
            (Get-Stage5PerformanceSha256 $stagedPerformanceDataPath) -ceq
                $performanceData.manifestSha256) `
            'Performance qualification data changed during installed execution.'
    }
    elseif ($isInstalledKernelExecution) {
        $finalFixtureProduction =
            Read-Stage5NativePerformanceFixtureProductionReceipt `
                -Path $fixtureProduction.path `
                -ExpectedSha256 $fixtureProduction.sha256 `
                -ExpectedTitle $Title `
                -ExpectedCohortNonce $executionCohort.nonce `
                -ExpectedCohortCreatedUtc $executionCohort.createdUtc `
                -ExpectedSourceCommit $ExpectedSourceCommit `
                -ExpectedArtifactSetSha256 $ExpectedArtifactSetSha256 `
                -ExpectedExecutableSha256 $ExpectedExecutableSha256 `
                -ExpectedDependencyManifestSha256 `
                    $artifactBinding.runtimeClosure.dependencyManifestSha256 `
                -ExpectedRuntimeClosureSha256 `
                    $artifactBinding.runtimeClosure.closureSha256
        Assert-Stage5PerformanceCondition (
            $finalFixtureProduction.path -ceq $fixtureProduction.path -and
            $finalFixtureProduction.sha256 -ceq $fixtureProduction.sha256 -and
            $finalFixtureProduction.fixture.sha256 -ceq
                $fixtureProduction.fixture.sha256) `
            'Native fixture-production closure changed during installed kernel execution.'
        $validationManifestPath = Join-Path $taskFull `
            'Stage5InstalledKernelExecutionValidationManifest.json'
        Write-Stage5JsonAtomically $validationManifestPath `
            ($context | Select-Object -Property * `
                -ExcludeProperty processCleanup) -CreateNew
        $validationManifestSnapshot =
            Get-Stage5FinalAcceptanceFileSnapshot $validationManifestPath `
                'Installed-kernel host validation manifest'
        $validationManifestBinding = [pscustomobject]@{
            path = $validationManifestPath
            sha256 = $validationManifestSnapshot.sha256
        }
    }
    $profileAfter = Assert-Stage5ProfileReadOnly $titleSession.profileRoot

    # Prepare the aggregate in memory. Publication follows the finally block,
    # which must prove cleanup and preserve any primary operation failure.
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
        }
        elseif ($isInstalledKernelExecution) {
            'stage5-installed-kernel-execution-host'
        }
        else { 'stage5-performance-scaling-local-capacity-smoke' }
        producer = 'Invoke-Stage5PerformanceScalingValidation.ps1'
        status = 'passed'
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        cohortNonce = $executionCohort.nonce
        cohortCreatedUtc = $executionCohort.createdUtc
        qualificationMode = $QualificationMode
        qualificationClass = if ($isExternalQualification) {
            'external-16-core-qualification'
        }
        elseif ($isInstalledKernelExecution) {
            'installed-kernel-execution-only'
        }
        else { 'local-capacity-smoke' }
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
        profileStrategy = 'process-local-validation-profile-root'
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
    if ($isInstalledKernelExecution) {
        $aggregate.acceptanceScope = 'kernel-execution-only'
        $aggregate.finalAcceptanceClaim = $false
        $aggregate.performanceScalingClaim = $false
        $aggregate.fixtureProductionReceipt = [ordered]@{
            path = [string]$fixtureProduction.path
            sha256 = [string]$fixtureProduction.sha256
        }
        $aggregate.validationManifest = [ordered]@{
            path = [string]$validationManifestBinding.path
            sha256 = [string]$validationManifestBinding.sha256
        }
    }
    if (@($PhaseBaselineProfiles).Count -gt 0) {
        $aggregate.phaseBaselinePolicy = $validated.phaseBaselinePolicy
        $aggregate.phaseBaselineProfiles = @($PhaseBaselineProfiles)
        $aggregate.phaseBaselineProfile = [ordered]@{
            path = $stagedPhaseProfilePath
            sha256 = $ExpectedPhaseBaselineProfileSha256
        }
        $aggregate.performanceData = [ordered]@{
            path = $stagedPerformanceDataPath
            sha256 = $ExpectedPerformanceDataManifestSha256
            closureSha256 = $ExpectedPerformanceDataClosureSha256
            fileCount = [int]$performanceData.fileCount
        }
        $aggregate.phaseBaselinePlan = $runPlanBinding
        $aggregate.phaseBaselineAttemptManifest = $phaseAttemptManifestBinding
        $aggregate.pairedPhaseBaselineBindings =
            @($validated.pairedPhaseBaselineBindings)
    }
    $aggregateName = if ($isExternalQualification) {
        'Stage5PerformanceScalingQualification.json'
    }
    elseif ($isInstalledKernelExecution) {
        'Stage5InstalledKernelExecutionHost.json'
    }
    else { 'Stage5PerformanceLocalCapacitySmoke.json' }
    $aggregatePath = Join-Path $taskFull $aggregateName
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
    if ($null -ne $context.processCleanup -and
        $script:Stage5CurrentProcessStarted -and
        -not [bool]$context.processCleanup.exitProof) {
        $childCleanupBlocked = $true
        $cleanupErrors.Add('Owned Stage 5 title child exit proof was not recorded; registry/profile cleanup is deferred.') | Out-Null
    }
    if ($null -ne $context.processCleanup -and
        $script:Stage5CurrentProcessStarted -and
        $null -eq $script:Stage5CurrentProcessIdentity) {
        $childCleanupBlocked = $true
        $cleanupErrors.Add('Owned Stage 5 title child identity was not captured; registry/profile cleanup is deferred.') | Out-Null
    }
    if ($null -ne $registryRecovery -and
        @($registryRecovery.processIdentities.ToArray() | Where-Object {
            [bool]$_.launchPending
        }).Count -gt 0) {
        $childCleanupBlocked = $true
        $cleanupErrors.Add('A Stage 5 child launch remained pending in the recovery journal; registry/profile cleanup is deferred.') | Out-Null
    }
    if ($null -ne $readOnlyLocks) {
        try { Dispose-Stage5PerformanceReadOnlyLocks $readOnlyLocks }
        catch { $cleanupErrors.Add("read-only lock cleanup: $($_.Exception.Message)") | Out-Null }
    }
    $registryRestored = $false
    $recoveryJournalExists = Test-Path -LiteralPath $registryRecoveryPath -PathType Leaf
    if (-not $recoveryJournalExists -and $null -eq $registryRecovery) {
        # No registry journal means no registry side effect reached the strict
        # recovery boundary; there is nothing to restore.
        $registryRestored = $true
    }
    elseif (-not $recoveryJournalExists) {
        $cleanupErrors.Add('Stage 5 registry recovery journal disappeared before restoration; registry restoration is deferred.') | Out-Null
    }
    elseif ($null -eq $registryRecovery) {
        $cleanupErrors.Add('Recovery journal exists without its in-memory recovery context; registry restoration is deferred.') | Out-Null
    }
    elseif ($childCleanupBlocked) {
        try {
            Update-Stage5RegistryRecoveryState $registryRecovery `
                'child-exit-unproven' $false $false
        }
        catch { $cleanupErrors.Add("recovery journal update: $($_.Exception.Message)") | Out-Null }
    }
    else {
        $recoveryAttempted = $false
        try {
            Assert-Stage5NoInstalledTitleProcesses
            Update-Stage5RegistryRecoveryState $registryRecovery `
                'active' $true $true
            $recoveryAuthorization = New-Stage5RegistryRecoveryAuthorization `
                $registryRecovery
            $recoveryAttempted = $true
            Invoke-Stage5RegistryRecovery -Path $registryRecovery.path `
                -ExpectedIdentity $registryRecovery.identity `
                -Authorization $recoveryAuthorization `
                -MutexLock $validationMutex `
                -Adapter $registryRecovery.adapter | Out-Null
            $registryRestored = $true
        }
        catch {
            $cleanupErrors.Add("registry restoration: $($_.Exception.Message)") | Out-Null
            if (-not $recoveryAttempted) {
                try {
                    Update-Stage5RegistryRecoveryState $registryRecovery `
                        'child-exit-unproven' $false $false
                }
                catch { $cleanupErrors.Add("recovery journal update: $($_.Exception.Message)") | Out-Null }
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
    if ($null -ne $runPlanBinding) {
        try {
            $cohortFailure = $null
            if ($null -ne $primaryError -or $cleanupErrors.Count -gt 0) {
                $messages = @()
                if ($null -ne $primaryError) { $messages += $primaryError.Exception.Message }
                $messages += @($cleanupErrors.ToArray())
                $cohortFailure = [pscustomobject]@{
                    stage=if ($null -ne $primaryError) { 'execution' } else { 'cleanup' }
                    message=($messages -join '; ')
                }
            }
            $outcomes = if ($null -eq $runPlanExecution) { @() } else { @($runPlanExecution.outcomes) }
            $attemptManifestPath = Join-Path $taskFull 'phase-attempts.json'
            if ($null -eq $phaseAttemptManifestBinding) {
                Write-Stage5JsonAtomically $attemptManifestPath ([pscustomobject][ordered]@{
                    schemaVersion=1; planSha256=$runPlanBinding.sha256; outcomes=$outcomes; cohortFailure=$cohortFailure
                }) -CreateNew
            }
            # Reopen the final file and every original recorded capability. This
            # internal closure does not add phase fields to the public aggregate.
            $attemptSnapshot = Get-Stage5FinalAcceptanceFileSnapshot $attemptManifestPath 'Final attempt manifest'
            $attemptManifestBinding = [pscustomobject]@{path=$attemptManifestPath;sha256=$attemptSnapshot.sha256}
            $attemptManifest = Read-Stage5PhaseBoundJson $attemptManifestBinding $taskFull 'Final attempt closure'
            if ($null -eq $cohortFailure) {
                $plan = Read-Stage5PhaseBoundJson $runPlanBinding $taskFull 'Final original plan'
                Assert-Stage5PerformanceCondition ($null -ne $runPlanExecution -and $null -eq $runPlanExecution.failure -and
                    $attemptManifest.outcomes.Count -eq $plan.entries.Count) 'Final attempt closure is incomplete.'
                for ($index=0; $index -lt $plan.entries.Count; ++$index) {
                    $entry = $plan.entries[$index]; $outcome = $attemptManifest.outcomes[$index]
                    Assert-Stage5PerformanceProperties $outcome @('entryId','state','failure','startBinding','resultBinding') 'Final attempt outcome'
                    Assert-Stage5PerformanceCondition ($outcome.entryId -ceq $entry.entryId -and $outcome.state -ceq 'completed' -and
                        $null -eq $outcome.failure -and $outcome.startBinding.path -ceq $entry.outputPaths.attemptStartPath -and
                        $outcome.resultBinding.path -ceq $entry.outputPaths.attemptResultPath -and
                        (ConvertTo-Json $outcome -Depth 20 -Compress) -ceq (ConvertTo-Json $runPlanExecution.outcomes[$index] -Depth 20 -Compress)) `
                        'Final attempt outcome does not retain its original complete execution.'
                    $start = Read-Stage5PhaseBoundJson $outcome.startBinding $taskFull 'Final original attempt start'
                    $result = Read-Stage5PhaseBoundJson $outcome.resultBinding $taskFull 'Final original attempt result'
                    Assert-Stage5PerformanceProperties $start @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc','sourceBinding') 'Final original start'
                    Assert-Stage5PerformanceProperties $result @('schemaVersion','event','planSha256','entryId','runNonce','recordedUtc','startBinding','state','failure','run','processCleanup') 'Final original result'
                    Assert-Stage5PerformanceCondition ($start.entryId -ceq $entry.entryId -and $start.runNonce -ceq $entry.runNonce -and
                        $start.planSha256 -ceq $runPlanBinding.sha256 -and $result.entryId -ceq $entry.entryId -and $result.runNonce -ceq $entry.runNonce -and
                        $result.planSha256 -ceq $runPlanBinding.sha256 -and $result.state -ceq 'completed' -and $null -eq $result.failure -and
                        $result.startBinding.path -ceq $outcome.startBinding.path -and $result.startBinding.sha256 -ceq $outcome.startBinding.sha256 -and
                        $result.processCleanup.exitProof -and -not $result.processCleanup.blocked -and @($result.processCleanup.errors).Count -eq 0) `
                        'Final original attempt records do not close.'
                }
            }
        }
        catch { $cleanupErrors.Add("attempt journal closure: $($_.Exception.Message)") | Out-Null }
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
if ($isInstalledKernelExecution) {
    Assert-Stage5PerformanceCondition ($null -ne $runPlanBinding -and
        $null -ne $attemptManifestBinding -and
        $null -ne $validationManifestBinding) `
        'Installed-kernel publication lacks its validated execution journals.'
    $aggregate.runPlan = [ordered]@{
        path = [string]$runPlanBinding.path
        sha256 = [string]$runPlanBinding.sha256
    }
    $aggregate.attemptManifest = [ordered]@{
        path = [string]$attemptManifestBinding.path
        sha256 = [string]$attemptManifestBinding.sha256
    }
}
Write-Stage5JsonAtomically $aggregatePath $aggregate -CreateNew
$aggregateSha256 = Get-Stage5PerformanceSha256 $aggregatePath
Write-Output $aggregatePath
if ($isExternalQualification) {
    $authoritativeEvidence = New-Stage5AuthoritativePerformanceEvidence `
        $context $aggregate $aggregatePath $aggregateSha256 $baseline `
        $fixtureManifest $stagedPhaseProfilePath `
        $ExpectedPhaseBaselineProfileSha256 ([pscustomobject]@{
            path = $stagedPerformanceDataPath
            manifestSha256 = $ExpectedPerformanceDataManifestSha256
            closureSha256 = $ExpectedPerformanceDataClosureSha256
            runtimeRoot = $performanceData.runtimeRoot
            fileCount = [int]$performanceData.fileCount
        })
    Write-Output $authoritativeEvidence.rawPath
    Write-Output $authoritativeEvidence.finalPath
}
