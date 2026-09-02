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

    # This parameter set exists only for host-side contract tests. It consumes
    # already-created synthetic receipts and can never reach Process.Start().
    [Parameter(ParameterSetName = 'SelfTest', Mandatory = $true)]
    [string]$SelfTestValidationManifestPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

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
    try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
    catch { throw "$Context is not valid JSON: $($_.Exception.Message)" }
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
    $runLines = @(Get-Content -LiteralPath $configPath |
        Where-Object { $_ -match '^\s*RUN\s*=' })
    Assert-Stage5PerformanceCondition ($runLines.Count -eq 1) `
        'launcher.lcf must contain exactly one RUN entry for performance validation.'
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
    param([Diagnostics.Process]$Process, [object]$ExpectedIdentity)
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
    $Process.WaitForExit()
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
    Assert-Stage5PerformanceCondition ($fields.producer -ceq
        'game-executable-performance-receipt-v1' -and
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
        $fields.close_boundary -ceq 'game-owned-raw-diagnostic-closed-v1') `
        "$Context raw diagnostic identity does not match its executable receipt."
}

function Assert-Stage5ReceiptMetricContract {
    param([object]$Receipt, [object]$Context, [string]$Label)
    $phaseNames = @('owner-intake', 'world-queries', 'pathfinding',
        'object-computation', 'spatial-work', 'deterministic-commit',
        'verification-publication')
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial',
        'path')
    $phases = @($Receipt.phases)
    $kernels = @($Receipt.kernels)
    Assert-Stage5PerformanceCondition ($phases.Count -eq $phaseNames.Count) `
        "$Label must contain exactly seven phase metrics."
    Assert-Stage5PerformanceCondition ($kernels.Count -eq $kernelNames.Count) `
        "$Label must contain exactly six kernel metrics."
    for ($index = 0; $index -lt $phaseNames.Count; ++$index) {
        Assert-Stage5PerformanceCondition ([string]$phases[$index].name -ceq
            $phaseNames[$index]) "$Label phase order/name is not canonical."
        Assert-Stage5PerformanceCondition (
            $phases[$index].available -or
            ([Int64]$phases[$index].totalNanoseconds -eq 0 -and
                [Int64]$phases[$index].maximumNanoseconds -eq 0 -and
                [Int64]$phases[$index].sampleCount -eq 0)) `
            "$Label phase '$($phaseNames[$index])' is malformed."
        if ([bool]$phases[$index].available) {
            Assert-Stage5PerformanceCondition ([Int64]$phases[$index].sampleCount -gt 0 -and
                [Int64]$phases[$index].totalNanoseconds -gt 0 -and
                [Int64]$phases[$index].maximumNanoseconds -gt 0) `
                "$Label available phase '$($phaseNames[$index])' lacks positive timing."
        }
        elseif ($Context.qualificationMode -ceq 'External16Core') {
            throw "$Label external qualification cannot use unavailable phase '$($phaseNames[$index])'."
        }
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
}

function Assert-Stage5Receipt {
    param([object]$Run, [object]$Context, [Collections.IDictionary]$SeenRunIds,
        [Collections.IDictionary]$SeenReceiptPaths,
        [Collections.IDictionary]$SeenReceiptHashes)
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
        'evidenceKind', 'status', 'title', 'runId', 'sourceCommit',
        'artifactSetSha256', 'executablePath', 'executableSha256', 'commandLine',
        'process', 'fixture', 'frames', 'worker', 'topology', 'rawEvidence',
        'schedulerMetrics', 'phases', 'kernels') "$label receipt"
    Assert-Stage5ReceiptMetricContract $receipt $Context "$label receipt"
    Assert-Stage5PerformanceCondition ($receipt.schemaVersion -eq 1 -and
        $receipt.producer -ceq 'game-executable-performance-receipt-v1' -and
        $receipt.evidenceKind -ceq 'stage5-performance-receipt' -and
        $receipt.status -ceq 'complete' -and $receipt.title -ceq $Context.title -and
        $receipt.sourceCommit -ceq $Context.sourceCommit -and
        $receipt.artifactSetSha256 -ceq $Context.artifactSetSha256 -and
        $receipt.runId -ceq [string]$Run.runId -and
        -not $SeenRunIds.ContainsKey([string]$receipt.runId)) `
        "$label receipt identity is invalid or its run ID was reused."
    $hostObservation = $Run.host
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
    Assert-Stage5PerformanceCondition ($receipt.fixture.id -ceq $fixture.id -and
        $receipt.fixture.contentSha256 -ceq $fixture.sha256 -and
        [String]::Equals([IO.Path]::GetFullPath([string]$receipt.fixture.replayPath),
            [IO.Path]::GetFullPath($fixture.path),
            [StringComparison]::OrdinalIgnoreCase) -and
        [bool]$receipt.fixture.seedKnown -and
        [UInt32]$receipt.fixture.seed -eq [UInt32]$fixture.seed -and
        [bool]$receipt.fixture.playerCountKnown -and
        [int]$receipt.fixture.playerCount -eq 8 -and
        [bool]$receipt.fixture.unitCountKnown -and
        [int]$receipt.fixture.unitCount -eq $fixture.peakUnitCount) `
        "$label fixture receipt does not match canonical reviewed metadata."
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
        $cpuSetsById[[string]$cpuSet.id] = $cpuSet
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
        [string]$hostObservation.rawLogSha256 -ceq $rawHash -and
        [string]$hostObservation.timingSha256 -ceq $timingHash) `
        "$label raw or timing SHA-256 does not match independent host rehashing."
    Assert-Stage5RawDiagnostic $rawPath $receipt $label
    Assert-Stage5PerformanceCondition ((Get-Item -LiteralPath $timingPath).Length -gt 0) `
        "$label timing evidence is empty."
    Assert-Stage5PerformanceCondition (Test-Stage5PerformanceFinitePositive `
        $hostObservation.elapsedMilliseconds) "$label host elapsed time is invalid."
    $SeenRunIds[[string]$receipt.runId] = $true
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

function Assert-Stage5PerformanceRunSet {
    param([object]$Document)
    Assert-Stage5PerformanceProperties $Document @('schemaVersion', 'title',
        'qualificationMode', 'stage3SourceCommit',
        'sourceCommit', 'artifactSetSha256', 'executablePath', 'executableSha256',
        'fixtureManifestSha256', 'stage3BaselineSha256', 'taskRoot', 'warmupRuns',
        'measuredRuns', 'fixtures', 'stage3Fixtures', 'topology', 'runs') `
        'Stage 5 host validation manifest'
    $mode = [string]$Document.qualificationMode
    Assert-Stage5PerformanceCondition (@('External16Core', 'LocalCapacitySmoke') -ccontains $mode) `
        'Stage 5 host validation manifest qualification mode is invalid.'
    $laneNames = @(Get-Stage5LaneNames $mode)
    $laneWorkers = @(Get-Stage5LaneWorkers $mode)
    Assert-Stage5PerformanceCondition ($Document.schemaVersion -eq 1 -and
        @('Generals', 'ZeroHour') -ccontains [string]$Document.title -and
        $Document.sourceCommit -cmatch '^[0-9a-f]{40}$' -and
        $Document.artifactSetSha256 -cmatch '^[0-9A-F]{64}$' -and
        $Document.executableSha256 -cmatch '^[0-9A-F]{64}$' -and
        $Document.fixtureManifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        ($mode -ceq 'LocalCapacitySmoke' -or
            $Document.stage3SourceCommit -cmatch '^[0-9a-f]{40}$') -and
        ($mode -ceq 'LocalCapacitySmoke' -or
            $Document.stage3BaselineSha256 -cmatch '^[0-9A-F]{64}$') -and
        [int]$Document.warmupRuns -eq 1 -and [int]$Document.measuredRuns -ge 3) `
        'Stage 5 host validation manifest identity is invalid.'
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
    $seenReceiptPaths = @{}
    $seenReceiptHashes = @{}
    $validated = @()
    foreach ($fixture in $fixtures) {
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
                $validated += Assert-Stage5Receipt $scheduled[$ordinal] $Document `
                    $seenRunIds $seenReceiptPaths $seenReceiptHashes
            }
        }
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
        runs = $validated
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
        [int]$Ordinal, [int]$WorkerCount, [int]$Timeout)
    $runId = ('s5perf-{0}-{1}-{2}-{3}' -f $Fixture.id, $Lane, $Ordinal,
        [Guid]::NewGuid().ToString('N'))
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
        RTS_PERFORMANCE_RUN_ID = $runId
        RTS_PERFORMANCE_RECEIPT_DIR = $receiptDirectory
        RTS_PERFORMANCE_SOURCE_COMMIT = $Context.sourceCommit
        RTS_PERFORMANCE_ARTIFACT_SET_SHA256 = $Context.artifactSetSha256
        RTS_PERFORMANCE_FIXTURE_ID = $Fixture.id
        RTS_PERFORMANCE_FIXTURE_SHA256 = $Fixture.sha256
        RTS_PERFORMANCE_RAW_LOG_PATH = $rawPath
        RTS_PERFORMANCE_TIMING_PATH = $timingDirectory
        RTS_PERFORMANCE_VERIFIER_BOUNDARY = $script:VerifierBoundary
        RTS_PERFORMANCE_SEED = [string]$Fixture.seed
        RTS_PERFORMANCE_PLAYER_COUNT = '8'
        RTS_PERFORMANCE_UNIT_COUNT = [string]$Fixture.peakUnitCount
        RTS_FRAME_TIMING_DIR = $timingDirectory
        TEMP = $tempDirectory
        TMP = $tempDirectory
    }
    $info = New-Stage5ProcessStartInfo $Context.executablePath $arguments `
        (Split-Path -Parent $Context.executablePath) $environment
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    # The self-test parameter set returns before this function is reachable.
    Assert-Stage5PerformanceCondition ($process.Start()) `
        "Failed to start installed performance run '$runId'."
    try {
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
        [IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
        [IO.File]::WriteAllText($stderrPath, $stderrTask.Result)
        $exitCode = $process.ExitCode
    }
    finally { $process.Dispose() }
    Assert-Stage5PerformanceCondition ($exitCode -eq 0) `
        "Installed performance run '$runId' exited with code $exitCode."
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
    try { [IO.File]::Move($temporary, $Path) }
    catch {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        throw
    }
}

if ($PSCmdlet.ParameterSetName -ceq 'SelfTest') {
    $selfTestPath = [IO.Path]::GetFullPath($SelfTestValidationManifestPath)
    $selfTestDocument = Read-Stage5PerformanceJson $selfTestPath `
        'Stage 5 host self-test validation manifest'
    $validatedSelfTest = Assert-Stage5PerformanceRunSet $selfTestDocument
    Write-Output ("Stage 5 performance host self-test validation passed: {0} runs." -f
        @($validatedSelfTest.runs).Count)
    return
}

Assert-Stage5PerformanceSourceCommit $ExpectedSourceCommit 'ExpectedSourceCommit'
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
    laneNames = @(Get-Stage5LaneNames $QualificationMode)
    laneWorkers = @(Get-Stage5LaneWorkers $QualificationMode)
    sourceCommit = $ExpectedSourceCommit
    stage3SourceCommit = if ($isExternalQualification) {
        $ExpectedStage3SourceCommit
    } else { '' }
    artifactSetSha256 = $ExpectedArtifactSetSha256
    executablePath = $executableFull
    executableSha256 = $ExpectedExecutableSha256
    fixtureManifestSha256 = $ExpectedFixtureManifestSha256
    stage3BaselineSha256 = if ($isExternalQualification) {
        $ExpectedStage3BaselineSha256
    } else { '' }
    taskRoot = $taskFull
    warmupRuns = 1
    measuredRuns = $MeasuredRuns
    fixtures = $fixtureManifest.fixtures
    stage3Fixtures = if ($isExternalQualification) { $baseline.fixtures } else { @() }
    topology = $hostTopology
    launcherContract = $launcherContract
    runs = @()
}

$scheduledRuns = New-Object 'Collections.Generic.List[object]'
foreach ($fixture in $fixtureManifest.fixtures) {
    for ($laneIndex = 0; $laneIndex -lt $context.laneNames.Count; ++$laneIndex) {
        for ($ordinal = 0; $ordinal -lt (1 + $MeasuredRuns); ++$ordinal) {
            $scheduledRuns.Add((Invoke-Stage5InstalledPerformanceRun $context $fixture `
                $context.laneNames[$laneIndex] $ordinal `
                $context.laneWorkers[$laneIndex] $TimeoutSeconds)) | Out-Null
        }
    }
}
$context.runs = $scheduledRuns.ToArray()
$validated = Assert-Stage5PerformanceRunSet $context

# This is the only aggregate write. Any missing/tampered receipt, topology,
# command, exit, raw file, timing file, or threshold failure leaves it absent.
$stage3Summary = if ($isExternalQualification) {
    [ordered]@{
        path = $baseline.path
        sha256 = $ExpectedStage3BaselineSha256
        sourceCommit = $ExpectedStage3SourceCommit
        executableSha256 = $ExpectedStage3ExecutableSha256
    }
} else { $null }
$aggregate = [ordered]@{
    schemaVersion = 1
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
    installedRuntime = $true
    sourceCommit = $ExpectedSourceCommit
    artifactSetSha256 = $ExpectedArtifactSetSha256
    title = $Title
    executable = [ordered]@{ path = $executableFull; sha256 = $ExpectedExecutableSha256 }
    fixtureManifest = [ordered]@{
        path = $fixtureManifest.path; sha256 = $ExpectedFixtureManifestSha256
    }
    stage3Baseline = $stage3Summary
    launcher = $launcherContract
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
    fixtures = $validated.fixtures
    runs = $validated.runs
}
$aggregateName = if ($isExternalQualification) {
    'Stage5PerformanceScalingQualification.json'
} else { 'Stage5PerformanceLocalCapacitySmoke.json' }
$aggregatePath = Join-Path $taskFull $aggregateName
Write-Stage5JsonAtomically $aggregatePath $aggregate
Write-Output $aggregatePath
