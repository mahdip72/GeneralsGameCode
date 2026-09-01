[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RuntimeRoot,
    [Parameter(Mandatory = $true)][string]$FixtureManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [ValidateSet('Replay', 'AI', 'All')][string]$ValidationSet = 'All',
    [int]$ReplayMatrixRepeats = 2,
    [int]$StressRepeats = 3,
    [int]$ReplayTimeoutSeconds = 600,
    [int]$AiTimeoutSeconds = 1800,
    [UInt64]$MinimumFreeBytes = 10737418240,
    [string]$ExpectedExecutableSha256 = '',
    [string]$ProfileLeafName = '',
    [string]$GeneralsInstallRoot = '',
    [switch]$AllowNonStandardCorpus,
    [switch]$PlanOnly,
    [switch]$DisableFrameTiming,
    [switch]$DiagnosticNonAcceptance,
    [switch]$RequireX64,
    [switch]$EnforcePerformance,
    [string]$Stage3PerformanceBaselinePath = '',
    [string]$ExpectedStage3ExecutableSha256 = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-RequiredProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-Condition ($null -ne $Object -and $Object -is [Collections.IDictionary]) `
        "$Context must be a JSON object."
    $keys = @($Object.Keys | Where-Object { [string]$_ -ceq $Name })
    Assert-Condition ($keys.Count -eq 1) "$Context is missing required property '$Name'."
    $value = $Object[$keys[0]]
    if ($value -is [Array]) { return ,$value }
    return $value
}

function Assert-JsonObjectShape {
    param([object]$Object, [string[]]$Required, [string[]]$Allowed, [string]$Context)
    Assert-Condition ($null -ne $Object -and $Object -is [Collections.IDictionary]) `
        "$Context must be a JSON object."
    foreach ($requiredName in $Required) {
        Get-RequiredProperty $Object $requiredName $Context | Out-Null
    }
    foreach ($propertyName in $Object.Keys) {
        Assert-Condition ($Allowed -ccontains [string]$propertyName) `
            "$Context contains unsupported property '$propertyName'."
    }
}

function Assert-JsonString {
    param([object]$Value, [string]$Context)
    Assert-Condition ($Value -is [string]) "$Context must be a JSON string."
}

function Assert-JsonBoolean {
    param([object]$Value, [string]$Context)
    Assert-Condition ($Value -is [bool]) "$Context must be a JSON boolean."
}

function Assert-JsonArray {
    param([object]$Value, [string]$Context)
    Assert-Condition ($null -ne $Value -and $Value -is [Array]) "$Context must be a JSON array."
}

function Assert-JsonInteger {
    param([object]$Value, [string]$Context)
    $isInteger = $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
    Assert-Condition $isInteger "$Context must be a JSON integer."
}

function Test-Sha256Text {
    param([string]$Value)
    return $null -ne $Value -and $Value -match '^[0-9A-Fa-f]{64}$'
}

function Resolve-ManifestFile {
    param([string]$ManifestDirectory, [string]$RelativePath, [string]$Context)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) "$Context path is empty."
    Assert-Condition (-not [IO.Path]::IsPathRooted($RelativePath)) "$Context path must be manifest-relative."
    $manifestFull = [IO.Path]::GetFullPath($ManifestDirectory)
    $candidate = [IO.Path]::GetFullPath((Join-Path $manifestFull $RelativePath))
    Assert-Condition ($candidate.StartsWith($manifestFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) "$Context path escapes the manifest directory."
    Assert-Condition (Test-Path -LiteralPath $candidate -PathType Leaf) "$Context file was not found: $RelativePath"
    return $candidate
}

function Assert-FileHash {
    param([string]$Path, [string]$Expected, [string]$Context)
    Assert-Condition (Test-Sha256Text $Expected) "$Context SHA-256 must contain exactly 64 hexadecimal characters."
    $actual = Get-Sha256 $Path
    Assert-Condition ($actual -ceq $Expected.ToUpperInvariant()) "$Context SHA-256 mismatch. Expected $Expected, got $actual."
    return $actual
}

function Assert-FreeSpace {
    param([string]$Path, [UInt64]$RequiredBytes, [string]$Context)
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($root)) "$Context has no resolvable volume."
    $drive = New-Object IO.DriveInfo($root)
    Assert-Condition ([UInt64]$drive.AvailableFreeSpace -ge $RequiredBytes) `
        "$Context requires at least $RequiredBytes free bytes; $($drive.AvailableFreeSpace) are available on $root."
}

function Get-PhysicalCoreCount {
    try {
        $processors = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop)
        if ($processors.Count -eq 0) { return 0 }
        return [int](($processors | Measure-Object -Property NumberOfCores -Sum).Sum)
    }
    catch { return 0 }
}

function Assert-X64PeExecutable {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            Assert-Condition ($reader.ReadUInt16() -eq 0x5A4D) `
                'Performance validation requires a valid PE executable.'
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            Assert-Condition ($peOffset -gt 0 -and $peOffset -lt $stream.Length - 6) `
                'Performance validation requires a valid PE header.'
            $stream.Position = $peOffset
            Assert-Condition ($reader.ReadUInt32() -eq 0x00004550) `
                'Performance validation requires a valid PE signature.'
            Assert-Condition ($reader.ReadUInt16() -eq 0x8664) `
                'Performance validation requires the exact native x64 candidate.'
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-WorkerConfigurations {
    return @(
        [pscustomobject]@{ Id = 'serial-1'; Mode = 'serial'; WorkerCount = 1; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-1'; Mode = 'parallel'; WorkerCount = 1; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-2'; Mode = 'parallel'; WorkerCount = 2; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-4'; Mode = 'parallel'; WorkerCount = 4; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-8'; Mode = 'parallel'; WorkerCount = 8; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-16'; Mode = 'parallel'; WorkerCount = 16; HasWorkerCount = $true },
        [pscustomobject]@{ Id = 'parallel-auto'; Mode = 'parallel'; WorkerCount = 0; HasWorkerCount = $false }
    )
}

function Get-CollisionShadowConfiguration {
    return [pscustomobject]@{
        Id = 'shadow-16'; Mode = 'shadow'; WorkerCount = 16; HasWorkerCount = $true
    }
}

function New-CommonArguments {
    param([object]$Configuration, [string]$ExecutableHash)
    $arguments = @(
        '-headless', '-noFPSLimit',
        '-pipelineMode', 'serial',
        '-simulationMode', [string]$Configuration.Mode,
        '-workerPolicy', 'auto',
        '-validationExecutableSha256', $ExecutableHash
    )
    if ($Configuration.HasWorkerCount) {
        $arguments += @('-workerCount', [string]$Configuration.WorkerCount)
    }
    return $arguments
}

function ConvertTo-DisplayCommand {
    param([string]$Executable, [string[]]$Arguments)
    $display = @('"' + $Executable.Replace('"', '""') + '"')
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') {
            $display += '"' + $argument.Replace('"', '""') + '"'
        }
        else {
            $display += $argument
        }
    }
    return $display -join ' '
}

function Add-PlanEntry {
    param(
        [Collections.Generic.List[object]]$Plan,
        [string]$Kind,
        [string]$CaseId,
        [object]$Configuration,
        [int]$Repeat,
        [int]$TimeoutSeconds,
        [string[]]$Arguments,
        [string]$Executable,
        [string]$OutputDirectory,
        [string]$DeterminismKey,
        [string]$ReplayArgument,
        [string]$FixtureSha256,
        [bool]$Stress,
        [int]$MatrixRepeat,
        [int]$Seed,
        [string]$Scenario
    )
    $sequence = $Plan.Count + 1
    $safeId = "$($Configuration.Id)-$CaseId-r$Repeat"
    $prefix = '{0:D4}-{1}' -f $sequence, $safeId
    $Plan.Add([pscustomobject]@{
        sequence = $sequence
        kind = $Kind
        caseId = $CaseId
        determinismKey = $DeterminismKey
        configuration = $Configuration.Id
        simulationMode = $Configuration.Mode
        requestedWorkers = $(if ($Configuration.HasWorkerCount) { [string]$Configuration.WorkerCount } else { 'auto' })
        workerPolicy = 'auto'
        repeat = $Repeat
        matrixRepeat = $MatrixRepeat
        replayArgument = $ReplayArgument
        fixtureSha256 = $FixtureSha256
        stress = $Stress
        seed = $Seed
        scenario = $Scenario
        timeoutSeconds = $TimeoutSeconds
        arguments = $Arguments
        command = ConvertTo-DisplayCommand $Executable $Arguments
        stdout = Join-Path $OutputDirectory "runs\$prefix.stdout.log"
        stderr = Join-Path $OutputDirectory "runs\$prefix.stderr.log"
        timingDirectory = Join-Path $OutputDirectory "timing\$prefix"
        runtimeLogDirectory = Join-Path $OutputDirectory "runtime-logs\$prefix"
    }) | Out-Null
}

function Get-ManifestData {
    param([string]$Path, [string]$Set, [bool]$AllowNonStandard,
        [string]$ExecutableHashOverride)
    $manifestFile = [IO.Path]::GetFullPath($Path)
    Assert-Condition (Test-Path -LiteralPath $manifestFile -PathType Leaf) "Fixture manifest was not found: $manifestFile"
    $manifestDirectory = Split-Path -Parent $manifestFile
    $manifestJson = Get-Content -LiteralPath $manifestFile -Raw
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $manifest = $manifestJson | ConvertFrom-Json -AsHashtable
    }
    else {
        Add-Type -AssemblyName System.Web.Extensions
        $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $serializer.MaxJsonLength = 10485760
        $manifest = $serializer.DeserializeObject($manifestJson)
    }
    Assert-JsonObjectShape $manifest @('schemaVersion', 'title', 'executable',
        'executableSha256', 'fixtures', 'ai') @('schemaVersion', 'title', 'executable',
        'executableSha256', 'fixtures', 'ai') 'Fixture manifest'
    $schemaVersion = Get-RequiredProperty $manifest 'schemaVersion' 'Fixture manifest'
    Assert-JsonInteger $schemaVersion 'Fixture manifest schemaVersion'
    Assert-Condition ($schemaVersion -eq 1) `
        'Fixture manifest schemaVersion must be 1.'
    $title = Get-RequiredProperty $manifest 'title' 'Fixture manifest'
    Assert-JsonString $title 'Fixture manifest title'
    Assert-Condition ($title -ceq 'ZeroHour') `
        'The Stage 5 installed-runtime matrix currently supports title ZeroHour only.'
    $executableName = Get-RequiredProperty $manifest 'executable' 'Fixture manifest'
    Assert-JsonString $executableName 'Fixture manifest executable'
    Assert-Condition ($executableName -match '^[A-Za-z0-9._-]+\.exe$') `
        'Fixture manifest executable must be a leaf .exe name.'
    $executableHash = Get-RequiredProperty $manifest 'executableSha256' 'Fixture manifest'
    Assert-JsonString $executableHash 'Fixture manifest executableSha256'
    Assert-Condition (Test-Sha256Text $executableHash) 'Fixture manifest executableSha256 is invalid.'
    $executableHashSource = 'manifest'
    if (-not [string]::IsNullOrWhiteSpace($ExecutableHashOverride)) {
        Assert-Condition (Test-Sha256Text $ExecutableHashOverride) `
            'ExpectedExecutableSha256 must contain exactly 64 hexadecimal characters.'
        $executableHash = $ExecutableHashOverride
        $executableHashSource = 'argument'
    }

    $fixtures = New-Object 'Collections.Generic.List[object]'
    $fixtureIds = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $replayArguments = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $fixtureItemsValue = Get-RequiredProperty $manifest 'fixtures' 'Fixture manifest'
    Assert-JsonArray $fixtureItemsValue 'Fixture manifest fixtures'
    $fixtureItems = @($fixtureItemsValue)
    Assert-Condition ($fixtureItems.Count -gt 0 -and $fixtureItems.Count -le 100) `
        'Fixture manifest fixtures must contain between 1 and 100 entries.'
    foreach ($fixture in $fixtureItems) {
            Assert-JsonObjectShape $fixture @('id', 'source', 'sha256', 'stress') `
                @('id', 'source', 'sha256', 'stress', 'maps') 'Replay fixture'
            $id = Get-RequiredProperty $fixture 'id' 'Replay fixture'
            Assert-JsonString $id 'Replay fixture id'
            Assert-Condition ($id -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') "Replay fixture id '$id' is invalid."
            Assert-Condition ($fixtureIds.Add($id)) `
                "Replay fixture id '$id' collides case-insensitively with another fixture id."
            $replayArgument = "Stage5Validation\$id.rep"
            Assert-Condition ($replayArguments.Add($replayArgument)) `
                "Replay argument '$replayArgument' collides case-insensitively with another staged replay path."
            $sourceRelative = Get-RequiredProperty $fixture 'source' "Replay fixture '$id'"
            Assert-JsonString $sourceRelative "Replay fixture '$id' source"
            $sourceFull = Resolve-ManifestFile $manifestDirectory $sourceRelative "Replay fixture '$id'"
            $sha256 = Get-RequiredProperty $fixture 'sha256' "Replay fixture '$id'"
            Assert-JsonString $sha256 "Replay fixture '$id' sha256"
            Assert-FileHash $sourceFull $sha256 "Replay fixture '$id'" | Out-Null
            $stress = Get-RequiredProperty $fixture 'stress' "Replay fixture '$id'"
            Assert-JsonBoolean $stress "Replay fixture '$id' stress"
            $maps = New-Object 'Collections.Generic.List[object]'
            $mapsKeys = @($fixture.Keys | Where-Object { [string]$_ -ceq 'maps' })
            if ($mapsKeys.Count -eq 1) {
                $mapValues = $fixture[$mapsKeys[0]]
                Assert-JsonArray $mapValues "Replay fixture '$id' maps"
                foreach ($map in @($mapValues)) {
                    Assert-JsonObjectShape $map @('source', 'profileRelativePath', 'sha256') `
                        @('source', 'profileRelativePath', 'sha256') "Replay fixture '$id' map"
                    $mapSourceRelative = Get-RequiredProperty $map 'source' "Replay fixture '$id' map"
                    Assert-JsonString $mapSourceRelative "Replay fixture '$id' map source"
                    $mapSourceFull = Resolve-ManifestFile $manifestDirectory $mapSourceRelative "Replay fixture '$id' map"
                    $mapRelative = Get-RequiredProperty $map 'profileRelativePath' "Replay fixture '$id' map"
                    Assert-JsonString $mapRelative "Replay fixture '$id' map profileRelativePath"
                    Assert-Condition (-not [IO.Path]::IsPathRooted($mapRelative)) `
                        "Replay fixture '$id' map profileRelativePath must be relative."
                    $normalizedMap = $mapRelative.Replace('/', '\')
                    Assert-Condition ($normalizedMap -match '^Maps\\' -and $normalizedMap -notmatch '(^|\\)\.\.(\\|$)') `
                        "Replay fixture '$id' map profileRelativePath must stay below Maps."
                    $mapHash = Get-RequiredProperty $map 'sha256' "Replay fixture '$id' map"
                    Assert-JsonString $mapHash "Replay fixture '$id' map sha256"
                    Assert-FileHash $mapSourceFull $mapHash "Replay fixture '$id' map" | Out-Null
                    $maps.Add([pscustomobject]@{ source = $mapSourceFull; relative = $normalizedMap; sha256 = $mapHash.ToUpperInvariant() }) | Out-Null
                }
            }
            $fixtures.Add([pscustomobject]@{
                id = $id
                source = $sourceFull
                sha256 = $sha256.ToUpperInvariant()
                stress = $stress
                replayArgument = $replayArgument
                maps = $maps.ToArray()
            }) | Out-Null
    }
    if ($Set -ne 'AI') {
        if (-not $AllowNonStandard) {
            Assert-Condition ($fixtures.Count -eq 10) 'The standard replay corpus must contain exactly 10 fixtures.'
            Assert-Condition (@($fixtures | Where-Object { $_.stress }).Count -eq 1) `
                'The standard replay corpus must mark exactly one stress fixture.'
        }
        else {
            Assert-Condition ($fixtures.Count -gt 0) 'Replay validation requires at least one fixture.'
        }
    }
    $uniqueSourceHashes = @($fixtures | ForEach-Object { $_.sha256 } | Select-Object -Unique)
    Assert-Condition ($uniqueSourceHashes.Count -eq $fixtures.Count) `
        'Replay validation requires a unique source SHA-256 for every fixture.'

    $aiObject = Get-RequiredProperty $manifest 'ai' 'Fixture manifest'
    Assert-JsonObjectShape $aiObject @('seeds', 'scenarios', 'repeats') `
        @('seeds', 'scenarios', 'repeats') 'AI manifest'
    $seedValues = Get-RequiredProperty $aiObject 'seeds' 'AI manifest'
    Assert-JsonArray $seedValues 'AI manifest seeds'
    $seeds = @($seedValues)
        Assert-Condition ($seeds.Count -gt 0) 'AI manifest seeds must not be empty.'
        Assert-Condition ($seeds.Count -le 32) 'AI manifest accepts at most 32 seeds.'
        foreach ($seed in $seeds) {
            Assert-JsonInteger $seed 'AI manifest seed'
            Assert-Condition ($seed -gt 0) 'AI manifest seeds must be positive integers.'
        }
        Assert-Condition (@($seeds | Sort-Object -Unique).Count -eq $seeds.Count) `
            'AI manifest seeds must be distinct.'
    $scenarioValues = Get-RequiredProperty $aiObject 'scenarios' 'AI manifest'
    Assert-JsonArray $scenarioValues 'AI manifest scenarios'
    $scenarios = @($scenarioValues)
        Assert-Condition ($scenarios.Count -gt 0) 'AI manifest scenarios must not be empty.'
        foreach ($scenario in $scenarios) {
            Assert-JsonString $scenario 'AI manifest scenario'
            Assert-Condition ($scenario -ceq '4v3' -or $scenario -ceq '4v2') `
                "AI manifest scenario '$scenario' is invalid."
        }
        Assert-Condition (@($scenarios | Sort-Object -Unique).Count -eq $scenarios.Count) `
            'AI manifest scenarios must be distinct.'
    $aiRepeats = Get-RequiredProperty $aiObject 'repeats' 'AI manifest'
    Assert-JsonInteger $aiRepeats 'AI manifest repeats'
        Assert-Condition ($aiRepeats -gt 0 -and $aiRepeats -le 10) `
            'AI manifest repeats must be between 1 and 10.'
    if ($Set -ceq 'All') {
        Assert-Condition ($scenarios -ccontains '4v3' -and $scenarios -ccontains '4v2') `
            'The deterministic-runtime gate requires both the 4v3 and 4v2 live-AI scenarios.'
        Assert-Condition ($seeds.Count -ge 3) `
            'The deterministic-runtime gate requires at least three distinct live-AI seeds.'
    }
    $ai = [pscustomobject]@{ seeds = $seeds; scenarios = $scenarios; repeats = [int]$aiRepeats }

    return [pscustomobject]@{
        file = $manifestFile
        directory = $manifestDirectory
        executable = $executableName
        executableSha256 = $executableHash.ToUpperInvariant()
        executableSha256Source = $executableHashSource
        fixtures = $fixtures.ToArray()
        ai = $ai
    }
}

function New-ValidationPlan {
    param([object]$Data, [string]$Set, [int]$ReplayPasses, [int]$StressRunCount,
        [int]$ReplayTimeout, [int]$AiTimeout, [string]$Executable, [string]$OutputDirectory)
    $plan = New-Object 'Collections.Generic.List[object]'
    foreach ($configuration in Get-WorkerConfigurations) {
        $common = @(New-CommonArguments $configuration $Data.executableSha256)
        if ($Set -ne 'AI') {
            for ($matrixRepeat = 1; $matrixRepeat -le $ReplayPasses; ++$matrixRepeat) {
                foreach ($fixture in $Data.fixtures) {
                    $fixtureRepeats = if ($fixture.stress) { $StressRunCount } else { 1 }
                    for ($repeat = 1; $repeat -le $fixtureRepeats; ++$repeat) {
                        $arguments = @($common) + @('-replay', $fixture.replayArgument)
                        Add-PlanEntry $plan 'replay' "$($fixture.id)-p$matrixRepeat" $configuration $repeat `
                            $ReplayTimeout $arguments $Executable $OutputDirectory $fixture.id `
                            $fixture.replayArgument $fixture.sha256 $fixture.stress $matrixRepeat 0 ''
                    }
                }
            }
        }
        if ($Set -ne 'Replay') {
            foreach ($scenario in $Data.ai.scenarios) {
                foreach ($seed in $Data.ai.seeds) {
                    for ($repeat = 1; $repeat -le $Data.ai.repeats; ++$repeat) {
                        $runnerFlag = if ($scenario -ceq '4v2') { '-runSkirmishAITest4v2' } else { '-runSkirmishAITest' }
                        $arguments = @($common) + @($runnerFlag, [string]$seed)
                        Add-PlanEntry $plan 'ai' "$scenario-seed-$seed" $configuration $repeat `
                            $AiTimeout $arguments $Executable $OutputDirectory "$scenario-seed-$seed" `
                            '' '' ($scenario -ceq '4v2') 0 $seed $scenario
                    }
                }
            }
        }
    }
    if ($Set -ne 'Replay' -and $Data.ai.scenarios -ccontains '4v2') {
        # One bounded installed-runtime shadow stress execution proves the
        # post-legacy collision oracle without multiplying the full matrix.
        $shadowConfiguration = Get-CollisionShadowConfiguration
        $shadowSeed = [int]$Data.ai.seeds[0]
        $shadowCommon = @(New-CommonArguments $shadowConfiguration $Data.executableSha256)
        $shadowArguments = @($shadowCommon) + @('-runSkirmishAITest4v2', [string]$shadowSeed)
        Add-PlanEntry $plan 'ai' "4v2-shadow-seed-$shadowSeed" $shadowConfiguration 1 `
            $AiTimeout $shadowArguments $Executable $OutputDirectory "4v2-seed-$shadowSeed" `
            '' '' $true 0 $shadowSeed '4v2'
    }
    return $plan.ToArray()
}

function Set-PreservedRegistryValue {
    param([Microsoft.Win32.RegistryView]$View, [string]$SubKey, [string]$Name,
        [string]$Value, [Collections.Generic.List[object]]$Snapshots)
    Assert-Condition ($null -ne $Snapshots) `
        'Registry snapshot destination is required before setup can mutate state.'
    $subKeys = New-Object 'Collections.Generic.List[string]'
    $currentSubKey = ''
    foreach ($segment in @($SubKey.Split('\'))) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($segment)) `
            "Registry path '$SubKey' contains an empty segment."
        $currentSubKey = if ([string]::IsNullOrEmpty($currentSubKey)) {
            $segment
        }
        else {
            $currentSubKey + '\' + $segment
        }
        $subKeys.Add($currentSubKey) | Out-Null
    }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    try {
        Invoke-Stage5RegistrySetupTransaction $subKeys.ToArray() -ActionContext $base `
            -EnsureSubKeyAction {
            param($candidateSubKey, $registryBase)
            $existingKey = $registryBase.OpenSubKey($candidateSubKey)
            if ($null -ne $existingKey) {
                $existingKey.Dispose()
                return $false
            }
            $createdKey = $null
            $createdKey = $registryBase.CreateSubKey($candidateSubKey)
            try {
                Assert-Condition ($null -ne $createdKey) `
                    "Failed to create registry key '$candidateSubKey'."
            }
            finally {
                if ($null -ne $createdKey) { $createdKey.Dispose() }
            }
            return $true
        } -CaptureValueAction {
            param($createdSubKeys, $registryBase)
            $key = $registryBase.OpenSubKey($SubKey, $true)
            Assert-Condition ($null -ne $key) "Failed to open registry key '$SubKey' for validation."
            try {
                $names = @($key.GetValueNames())
                $hadValue = $names -contains $Name
                $oldValue = $null
                $oldKind = $null
                if ($hadValue) {
                    $oldValue = $key.GetValue($Name, $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $oldKind = $key.GetValueKind($Name)
                }
                return [pscustomobject]@{
                    view = $View; subKey = $SubKey; name = $Name
                    hadKey = -not ($createdSubKeys -ccontains $SubKey)
                    hadValue = $hadValue; oldValue = $oldValue; oldKind = $oldKind
                    createdSubKeys = @($createdSubKeys)
                }
            }
            finally { $key.Dispose() }
        } -SetValueAction {
            param($snapshot, $registryBase)
            $key = $registryBase.OpenSubKey($snapshot.subKey, $true)
            Assert-Condition ($null -ne $key) `
                "Failed to reopen registry key '$($snapshot.subKey)' for validation."
            try {
                $key.SetValue($snapshot.name, $Value,
                    [Microsoft.Win32.RegistryValueKind]::String)
            }
            finally { $key.Dispose() }
        } -RegisterSnapshotAction {
            param($snapshot)
            $Snapshots.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $registryBase)
            $key = $registryBase.OpenSubKey($snapshot.subKey, $true)
            if ($null -eq $key) {
                Assert-Condition (-not $snapshot.hadKey) `
                    "Pre-existing registry key '$($snapshot.subKey)' disappeared during setup rollback."
                return
            }
            try {
                if ($snapshot.hadValue) {
                    $key.SetValue($snapshot.name, $snapshot.oldValue, $snapshot.oldKind)
                }
                else {
                    $key.DeleteValue($snapshot.name, $false)
                }
            }
            finally { $key.Dispose() }
        } -CleanupCreatedSubKeysAction {
            param($createdSubKeys, $registryBase)
            Invoke-Stage5CreatedRegistryKeyCleanup @($createdSubKeys) -ActionContext $registryBase `
                -InspectAction {
                param($createdSubKey, $cleanupBase)
                $createdKey = $cleanupBase.OpenSubKey($createdSubKey)
                if ($null -eq $createdKey) { return $null }
                try {
                    return [pscustomobject]@{
                        valueNames = @($createdKey.GetValueNames())
                        subKeyNames = @($createdKey.GetSubKeyNames())
                    }
                }
                finally { $createdKey.Dispose() }
            } -RemoveAction {
                param($createdSubKey, $cleanupBase)
                $cleanupBase.DeleteSubKey($createdSubKey, $false)
            }
        } | Out-Null
    }
    finally {
        $base.Dispose()
    }
}

function Restore-RegistryValue {
    param([object]$Snapshot)
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $Snapshot.view)
    try {
        $restoreErrors = New-Object 'Collections.Generic.List[string]'
        try {
            $key = $base.OpenSubKey($Snapshot.subKey, $true)
            if ($null -eq $key) {
                Assert-Condition (-not $Snapshot.hadKey) `
                    "Pre-existing registry key '$($Snapshot.subKey)' disappeared before restoration."
            }
            else {
                try {
                    if ($Snapshot.hadValue) {
                        $key.SetValue($Snapshot.name, $Snapshot.oldValue, $Snapshot.oldKind)
                    }
                    else {
                        $key.DeleteValue($Snapshot.name, $false)
                    }
                }
                finally { $key.Dispose() }
            }
        }
        catch { $restoreErrors.Add("value restore: $($_.Exception.Message)") | Out-Null }
        try {
            Invoke-Stage5CreatedRegistryKeyCleanup @($Snapshot.createdSubKeys) -ActionContext $base `
                -InspectAction {
                param($createdSubKey, $registryBase)
                $createdKey = $registryBase.OpenSubKey($createdSubKey)
                if ($null -eq $createdKey) { return $null }
                try {
                    return [pscustomobject]@{
                        valueNames = @($createdKey.GetValueNames())
                        subKeyNames = @($createdKey.GetSubKeyNames())
                    }
                }
                finally { $createdKey.Dispose() }
            } -RemoveAction {
                param($createdSubKey, $registryBase)
                $registryBase.DeleteSubKey($createdSubKey, $false)
            }
        }
        catch { $restoreErrors.Add("created-key cleanup: $($_.Exception.Message)") | Out-Null }
        if ($restoreErrors.Count -gt 0) {
            throw "Registry snapshot restore failed after attempting value and key cleanup: $($restoreErrors -join ' | ')"
        }
    }
    finally {
        $base.Dispose()
    }
}

function Invoke-ValidationProcess {
    param([string]$Executable, [string]$WorkingDirectory, [object]$Entry, [bool]$CaptureTiming)
    $processName = [IO.Path]::GetFileNameWithoutExtension($Executable)
    Assert-Condition (@(Get-Process -Name $processName -ErrorAction SilentlyContinue).Count -eq 0) `
        "A $processName process is already running. Validation will not overlap game processes."
    $stdoutDirectory = Split-Path -Parent $Entry.stdout
    if (-not (Test-Path -LiteralPath $stdoutDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $stdoutDirectory | Out-Null
    }
    if ($CaptureTiming) {
        New-Item -ItemType Directory -Path $Entry.timingDirectory -Force | Out-Null
    }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.Arguments = (($Entry.arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ }
    }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($CaptureTiming) {
        $startInfo.EnvironmentVariables['RTS_FRAME_TIMING_DIR'] = $Entry.timingDirectory
    }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    $startedAt = [DateTime]::UtcNow
    Assert-Condition ($process.Start()) "Failed to start installed runtime process."
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $exited = $process.WaitForExit($Entry.timeoutSeconds * 1000)
    if (-not $exited) {
        try { $process.Kill() } catch { }
        $process.WaitForExit()
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    [IO.File]::WriteAllText($Entry.stdout, $stdout)
    [IO.File]::WriteAllText($Entry.stderr, $stderr)
    $runtimeLogText = New-Object 'Collections.Generic.List[string]'
    foreach ($runtimeLog in @(Get-ChildItem -LiteralPath $WorkingDirectory -Filter '*DebugLogFile*.txt' -File |
        Where-Object { $_.LastWriteTimeUtc -ge $startedAt.AddSeconds(-2) })) {
        if (-not (Test-Path -LiteralPath $Entry.runtimeLogDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $Entry.runtimeLogDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $runtimeLog.FullName -Destination `
            (Join-Path $Entry.runtimeLogDirectory $runtimeLog.Name)
        $runtimeLogText.Add((Get-Content -LiteralPath $runtimeLog.FullName -Raw)) | Out-Null
    }
    return [pscustomobject]@{
        timedOut = -not $exited
        exitCode = $(if ($exited) { $process.ExitCode } else { -1 })
        wallMilliseconds = [int64]([DateTime]::UtcNow - $startedAt).TotalMilliseconds
        stdout = $stdout
        stderr = $stderr
        runtimeLogText = $runtimeLogText.ToArray() -join "`n"
    }
}

Assert-Condition ($ReplayMatrixRepeats -gt 0 -and $ReplayMatrixRepeats -le 10) `
    'ReplayMatrixRepeats must be between 1 and 10.'
Assert-Condition ($StressRepeats -gt 0 -and $StressRepeats -le 10) `
    'StressRepeats must be between 1 and 10.'
$deterministicRuntimeContractRequested = $ValidationSet -ceq 'All' -and
    -not [bool]$DiagnosticNonAcceptance -and -not [bool]$AllowNonStandardCorpus
Assert-Condition (-not $deterministicRuntimeContractRequested -or $ReplayMatrixRepeats -eq 2) `
    'The deterministic-runtime gate requires exactly two complete replay matrix passes.'
Assert-Condition (-not $deterministicRuntimeContractRequested -or $StressRepeats -eq 3) `
    'The deterministic-runtime gate requires exactly three executions of the stress replay per configuration and matrix pass.'
Assert-Condition ($ReplayTimeoutSeconds -gt 0 -and $ReplayTimeoutSeconds -le 86400 -and
    $AiTimeoutSeconds -gt 0 -and $AiTimeoutSeconds -le 86400) `
    'Timeouts must be between 1 and 86400 seconds.'
Assert-Condition (-not $EnforcePerformance -or $ValidationSet -ne 'AI') `
    'Performance validation requires the replay matrix.'
Assert-Condition (-not $AllowNonStandardCorpus -or $PlanOnly -or $DiagnosticNonAcceptance) `
    'AllowNonStandardCorpus is limited to PlanOnly or DiagnosticNonAcceptance; accepting execution requires exactly 10 unique replay sources.'
Assert-Condition (-not $DisableFrameTiming -or $DiagnosticNonAcceptance) `
    'DisableFrameTiming is allowed only with DiagnosticNonAcceptance; timing is mandatory for an accepting gate.'
Assert-Condition (-not $EnforcePerformance -or (-not $DisableFrameTiming -and -not $DiagnosticNonAcceptance)) `
    'Performance validation cannot run in non-acceptance diagnostic mode or without frame timing.'
Assert-Condition (-not $EnforcePerformance -or
    ($ReplayMatrixRepeats * $StressRepeats) -ge 4) `
    'Performance validation requires one warm-up and at least three measured stress runs per configuration.'

$runtimeFull = [IO.Path]::GetFullPath($RuntimeRoot)
$manifestData = Get-ManifestData $FixtureManifestPath $ValidationSet `
    ([bool]$AllowNonStandardCorpus) $ExpectedExecutableSha256
$executableFull = Join-Path $runtimeFull $manifestData.executable
Assert-Condition (Test-Path -LiteralPath $runtimeFull -PathType Container) "Installed runtime root was not found: $runtimeFull"
Assert-Condition (Test-Path -LiteralPath (Join-Path $runtimeFull 'launcher.exe') -PathType Leaf) `
    'Installed runtime root is missing launcher.exe.'
Assert-Condition (Test-Path -LiteralPath (Join-Path $runtimeFull 'launcher.lcf') -PathType Leaf) `
    'Installed runtime root is missing launcher.lcf.'
Assert-Condition (Test-Path -LiteralPath $executableFull -PathType Leaf) `
    "Installed runtime executable was not found: $executableFull"
Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable' | Out-Null

$physicalCoreCount = 0
$stage3PerformanceBaseline = $null
$stage3PerformanceBaselineEvidencePath = $null
if ($RequireX64 -or $EnforcePerformance) {
    Assert-X64PeExecutable $executableFull
}
if ($EnforcePerformance) {
    $stressFixtures = @($manifestData.fixtures | Where-Object { $_.stress })
    Assert-Condition ($stressFixtures.Count -eq 1) `
        'Performance validation requires exactly one stress replay fixture.'
    $stage3PerformanceBaseline = Read-Stage5PerformanceBaseline `
        $Stage3PerformanceBaselinePath $stressFixtures[0].sha256 $ExpectedStage3ExecutableSha256
    $physicalCoreCount = Get-PhysicalCoreCount
}

$outputFull = [IO.Path]::GetFullPath($OutputRoot)
Assert-Condition (-not (Test-Path -LiteralPath $outputFull)) `
    'OutputRoot must not already exist; every validation run owns a fresh evidence directory.'
Assert-FreeSpace (Split-Path -Parent $outputFull) $MinimumFreeBytes 'Validation output volume'
New-Item -ItemType Directory -Path $outputFull | Out-Null
if ($null -ne $stage3PerformanceBaseline) {
    $stage3PerformanceBaselineEvidencePath = Join-Path $outputFull 'stage3-performance-baseline.json'
    Copy-Item -LiteralPath $stage3PerformanceBaseline.file -Destination $stage3PerformanceBaselineEvidencePath
    Assert-FileHash $stage3PerformanceBaselineEvidencePath $stage3PerformanceBaseline.fileSha256 `
        'Copied Stage 3 performance baseline evidence' | Out-Null
    $stage3PerformanceBaseline.evidenceFile = $stage3PerformanceBaselineEvidencePath
}

$plan = @(New-ValidationPlan $manifestData $ValidationSet $ReplayMatrixRepeats $StressRepeats `
    $ReplayTimeoutSeconds $AiTimeoutSeconds $executableFull $outputFull)
Assert-Condition ($plan.Count -gt 0) 'The fixture manifest produced an empty validation plan.'
Assert-Condition ($plan.Count -le 10000) 'The fixture manifest produced more than 10000 validation entries.'
if ($deterministicRuntimeContractRequested) {
    foreach ($configuration in @(Get-WorkerConfigurations | ForEach-Object { $_.Id })) {
        $configurationReplayPlan = @($plan | Where-Object {
            $_.kind -ceq 'replay' -and $_.configuration -ceq $configuration
        })
        Assert-Condition ($configurationReplayPlan.Count -eq 24) `
            "The deterministic-runtime gate requires exactly 24 replay executions for configuration '$configuration' across two complete passes."
        foreach ($matrixPass in @(1, 2)) {
            $passReplayPlan = @($configurationReplayPlan | Where-Object {
                $_.matrixRepeat -eq $matrixPass
            })
            Assert-Condition ($passReplayPlan.Count -eq 12 -and
                @($passReplayPlan | Where-Object { $_.stress }).Count -eq 3 -and
                @($passReplayPlan | Where-Object { -not $_.stress }).Count -eq 9 -and
                @($passReplayPlan.determinismKey | Sort-Object -Unique).Count -eq 10) `
                "The deterministic-runtime gate requires one execution of each of nine non-stress replays and three executions of the one stress replay for configuration '$configuration' in matrix pass $matrixPass."
        }
    }
}
if ($ValidationSet -ne 'Replay') {
    $workerConfigurationCount = @(Get-WorkerConfigurations).Count
    $regularAiPlan = @($plan | Where-Object {
        $_.kind -ceq 'ai' -and $_.configuration -cne 'shadow-16'
    })
    $expectedRegularAiCount = $workerConfigurationCount * $manifestData.ai.scenarios.Count *
        $manifestData.ai.seeds.Count * $manifestData.ai.repeats
    Assert-Condition ($regularAiPlan.Count -eq $expectedRegularAiCount) `
        "AI validation plan does not contain the complete $expectedRegularAiCount-result scenario/seed/configuration/repeat cross-product."
    $regularAiKeys = @($regularAiPlan | ForEach-Object {
        "$($_.scenario)|$($_.seed)|$($_.configuration)|$($_.repeat)"
    })
    Assert-Condition (@($regularAiKeys | Sort-Object -Unique).Count -eq $regularAiKeys.Count) `
        'AI validation plan contains a duplicate scenario/seed/configuration/repeat entry.'
}
$deterministicRuntimeEligible = $deterministicRuntimeContractRequested -and [bool]$EnforcePerformance
$planDocument = [pscustomobject]@{
    schemaVersion = 1
    gateName = 'deterministic-runtime'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    runtimeRoot = $runtimeFull
    executable = $executableFull
    executableSha256 = $manifestData.executableSha256
    executableSha256Source = $manifestData.executableSha256Source
    fixtureManifest = $manifestData.file
    validationSet = $ValidationSet
    replayMatrixRepeats = $ReplayMatrixRepeats
    stressRepeats = $StressRepeats
    x64Required = [bool]($RequireX64 -or $EnforcePerformance)
    performanceRequested = [bool]$EnforcePerformance
    performanceRequiredForDeterministicRuntimeGate = $true
    performanceMeasurementScope = 'aggregate-stage5-stress-replay-throughput'
    collisionSpecificReplayPerformanceClaim = $false
    diagnosticNonAcceptance = [bool]$DiagnosticNonAcceptance
    frameTimingRequired = -not [bool]$DisableFrameTiming
    authoritativeWorkEvidenceRequired = $deterministicRuntimeEligible
    deterministicRuntimeEligible = $deterministicRuntimeEligible
    finalAcceptanceEligible = $false
    physicalCoreCount = $physicalCoreCount
    stage3PerformanceBaseline = $(if ($null -ne $stage3PerformanceBaseline) {
        [pscustomobject]@{
            file = $stage3PerformanceBaseline.file
            evidenceFile = $stage3PerformanceBaselineEvidencePath
            fileSha256 = $stage3PerformanceBaseline.fileSha256
            executableSha256 = $stage3PerformanceBaseline.executableSha256
            expectedExecutableSha256 = $ExpectedStage3ExecutableSha256.ToUpperInvariant()
            fixtureSha256 = $stage3PerformanceBaseline.fixtureSha256
            physicalCoreCount = $stage3PerformanceBaseline.physicalCoreCount
            availableCpus = $stage3PerformanceBaseline.availableCpus
            warmupRuns = $stage3PerformanceBaseline.warmupRuns
            rawWallMilliseconds = $stage3PerformanceBaseline.wallMilliseconds
            measuredMedianMilliseconds = $stage3PerformanceBaseline.measuredMedianMilliseconds
        }
    } else { $null })
    entries = $plan
}
$planPath = Join-Path $outputFull 'validation-plan.json'
[IO.File]::WriteAllText($planPath, ($planDocument | ConvertTo-Json -Depth 8))
if ($EnforcePerformance -and $physicalCoreCount -lt 8) {
    $unsupportedPerformance = [pscustomobject]@{
        schemaVersion = 1
        status = 'unsupported-host-topology'
        measurementScope = 'aggregate-stage5-stress-replay-throughput'
        collisionSpecificSpeedupClaim = $false
        physicalCoreCount = $physicalCoreCount
        failures = @('Performance enforcement requires topology evidence for at least eight physical cores.')
    }
    [IO.File]::WriteAllText((Join-Path $outputFull 'performance-report.json'),
        ($unsupportedPerformance | ConvertTo-Json -Depth 4))
    throw 'Stage 5 performance validation is unsupported: fewer than eight physical cores were detected.'
}
if ($PlanOnly) {
    if (-not $deterministicRuntimeEligible) {
        Write-Output "Stage 5 focused/diagnostic deterministic-runtime plan completed: $($plan.Count) synchronous installed-runtime executions."
    }
    else {
        Write-Output "Stage 5 deterministic-runtime plan passed: $($plan.Count) synchronous installed-runtime executions."
    }
    return
}

if ([string]::IsNullOrWhiteSpace($ProfileLeafName)) {
    $ProfileLeafName = 'GGC-Stage5-Validation-{0}-{1}' -f ([DateTime]::UtcNow.ToString('yyyyMMddHHmmss')), $PID
}
Assert-Condition ($ProfileLeafName -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') `
    'ProfileLeafName must be a simple unique Documents leaf name.'
$documents = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
Assert-Condition (-not [string]::IsNullOrWhiteSpace($documents)) 'The Documents directory could not be resolved.'
$profileFull = [IO.Path]::GetFullPath((Join-Path $documents $ProfileLeafName))
Assert-Condition ($profileFull.StartsWith([IO.Path]::GetFullPath($documents) + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)) 'Profile path escapes Documents.'
Assert-Condition (-not (Test-Path -LiteralPath $profileFull)) 'ProfileLeafName already exists; use a unique profile.'
Assert-FreeSpace $documents $MinimumFreeBytes 'Validation profile volume'
New-Item -ItemType Directory -Path (Join-Path $profileFull 'Replays\Stage5Validation') -Force | Out-Null

foreach ($fixture in $manifestData.fixtures) {
    $replayDestination = Join-Path $profileFull "Replays\$($fixture.replayArgument)"
    Copy-Item -LiteralPath $fixture.source -Destination $replayDestination
    Assert-FileHash $replayDestination $fixture.sha256 `
        "Staged replay '$($fixture.replayArgument)'" | Out-Null
    foreach ($map in $fixture.maps) {
        $mapDestination = Join-Path $profileFull $map.relative
        $mapDirectory = Split-Path -Parent $mapDestination
        if (-not (Test-Path -LiteralPath $mapDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $mapDirectory -Force | Out-Null
        }
        if (Test-Path -LiteralPath $mapDestination) {
            Assert-FileHash $mapDestination $map.sha256 "Shared staged map '$($map.relative)'" | Out-Null
        }
        else {
            Copy-Item -LiteralPath $map.source -Destination $mapDestination
        }
        Assert-FileHash $mapDestination $map.sha256 `
            "Staged map '$($map.relative)'" | Out-Null
    }
}

# Rehash every staged destination as one complete corpus immediately before any
# registry mutation or installed-runtime execution. This catches overwrite,
# case-alias, and copy corruption independently of the source-file preflight.
foreach ($fixture in $manifestData.fixtures) {
    Assert-FileHash (Join-Path $profileFull "Replays\$($fixture.replayArgument)") `
        $fixture.sha256 "Execution-ready replay '$($fixture.replayArgument)'" | Out-Null
    foreach ($map in $fixture.maps) {
        Assert-FileHash (Join-Path $profileFull $map.relative) $map.sha256 `
            "Execution-ready map '$($map.relative)'" | Out-Null
    }
}

if ([string]::IsNullOrWhiteSpace($GeneralsInstallRoot)) {
    $GeneralsInstallRoot = $runtimeFull
}
$generalsInstallFull = [IO.Path]::GetFullPath($GeneralsInstallRoot)
Assert-Condition (Test-Path -LiteralPath $generalsInstallFull -PathType Container) `
    "GeneralsInstallRoot was not found: $generalsInstallFull"

$registrySnapshots = New-Object 'Collections.Generic.List[object]'
$results = New-Object 'Collections.Generic.List[object]'
$resultsPath = Join-Path $outputFull 'validation-results.json'
$fatalPattern = '(?i)(CRC Mismatch|game thread ownership violation|assertion failed|fatal error|missing map|replay read error|SKIRMISH_AI_TEST_FAIL|SIMULATION_JOB_SYSTEM_FALLBACK|SIMULATION_SHADOW_(?:MISMATCH|FAIL)|SIMULATION_COLLISION_MISMATCH)'
try {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        Set-PreservedRegistryValue $view `
            'Software\Electronic Arts\EA Games\Generals' 'InstallPath' `
            ($generalsInstallFull + '\') $registrySnapshots
        Set-PreservedRegistryValue $view `
            'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour' `
            'InstallPath' ($runtimeFull + '\') $registrySnapshots
        Set-PreservedRegistryValue $view `
            'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour' `
            'UserDataLeafName' $ProfileLeafName $registrySnapshots
    }

    foreach ($entry in $plan) {
        Assert-FreeSpace $outputFull $MinimumFreeBytes 'Validation evidence volume'
        Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable before run' | Out-Null
        $run = Invoke-ValidationProcess $executableFull $runtimeFull $entry (-not $DisableFrameTiming)
        $combined = $run.stdout + "`n" + $run.stderr + "`n" + $run.runtimeLogText
        Assert-Condition (-not $run.timedOut) "Validation entry $($entry.sequence) timed out."
        Assert-Condition ($run.exitCode -eq 0) "Validation entry $($entry.sequence) exited with code $($run.exitCode)."
        Assert-Condition ($combined -notmatch $fatalPattern) `
            "Validation entry $($entry.sequence) reported a fatal runtime marker."
        $aiEvidence = $null
        $replayMetrics = $null
        $replayResult = $null
        $timingEvidence = if ($DisableFrameTiming) {
            [pscustomobject]@{ status = 'disabled-non-acceptance-diagnostic' }
        }
        else {
            Get-Stage5TimingEvidence $entry.timingDirectory "Validation entry $($entry.sequence)"
        }
        if ($entry.kind -ceq 'ai') {
            $aiEvidence = ConvertFrom-Stage5AiCompletion $run.stdout $entry `
                $manifestData.executableSha256 $deterministicRuntimeEligible
            if (-not $DisableFrameTiming) {
                Assert-Condition ($timingEvidence.maximumFrameEnd -eq $aiEvidence.endFrame) `
                    "AI validation entry $($entry.sequence) timing final frame does not match the completion manifest."
                Assert-Stage5CollisionTimingEvidence $timingEvidence $aiEvidence `
                    "AI validation entry $($entry.sequence)"
            }
        }
        else {
            $replayMetrics = ConvertFrom-Stage5ReplayMetrics $run.stdout $entry
            $replayResult = ConvertFrom-Stage5ReplayResult $run.stdout $entry
            if (-not $DisableFrameTiming) {
                Assert-Condition ($timingEvidence.maximumFrameEnd -eq $replayResult.finalFrame) `
                    "Replay validation entry $($entry.sequence) timing final frame does not match SIMULATION_REPLAY_RESULT."
                Assert-Stage5CollisionTimingEvidence $timingEvidence $replayMetrics `
                    "Replay validation entry $($entry.sequence)"
            }
        }
        $results.Add([pscustomobject]@{
            sequence = $entry.sequence
            kind = $entry.kind
            caseId = $entry.caseId
            determinismKey = $entry.determinismKey
            configuration = $entry.configuration
            simulationMode = $entry.simulationMode
            repeat = $entry.repeat
            matrixRepeat = $entry.matrixRepeat
            fixtureSha256 = $entry.fixtureSha256
            stress = $entry.stress
            exitCode = $run.exitCode
            timedOut = $run.timedOut
            wallMilliseconds = $run.wallMilliseconds
            stdoutSha256 = Get-Sha256 $entry.stdout
            stderrSha256 = Get-Sha256 $entry.stderr
            aiEvidence = $aiEvidence
            replayMetrics = $replayMetrics
            replayResult = $replayResult
            timingEvidence = $timingEvidence
        }) | Out-Null
        [IO.File]::WriteAllText($resultsPath, ($results.ToArray() | ConvertTo-Json -Depth 5))
    }
    $workerConfigurationIds = @(Get-WorkerConfigurations | ForEach-Object { $_.Id })
    if ($ValidationSet -ne 'Replay') {
        $expectedAiDeterminismKeys = @(
            foreach ($scenario in $manifestData.ai.scenarios) {
                foreach ($seed in $manifestData.ai.seeds) {
                    "$scenario-seed-$seed"
                }
            }
        )
        Assert-Stage5AiDeterminism $results.ToArray() $workerConfigurationIds `
            $manifestData.ai.repeats 'shadow-16' $expectedAiDeterminismKeys
    }
    if ($deterministicRuntimeEligible) {
        Assert-Stage5AuthoritativeWorkEvidence $results.ToArray()
    }
    if ($ValidationSet -ne 'AI') {
        Assert-Stage5ReplayDeterminism $results.ToArray()
    }
    if ($EnforcePerformance) {
        $performanceReport = Measure-Stage5Performance $results.ToArray() $physicalCoreCount `
            $stage3PerformanceBaseline 1 3
        $performanceReportPath = Join-Path $outputFull 'performance-report.json'
        [IO.File]::WriteAllText($performanceReportPath, ($performanceReport | ConvertTo-Json -Depth 8))
        Assert-Condition ($performanceReport.status -ceq 'passed') `
            "Stage 5 performance validation status is '$($performanceReport.status)': $($performanceReport.failures -join ' ')"
    }
    Assert-FileHash $executableFull $manifestData.executableSha256 'Installed runtime executable after matrix' | Out-Null
}
finally {
    Invoke-Stage5RegistryRestore -Snapshots @($registrySnapshots.ToArray()) -RestoreAction {
        param($snapshot)
        Restore-RegistryValue $snapshot
    }
}

if (-not $deterministicRuntimeEligible) {
    Write-Output "Stage 5 focused/diagnostic deterministic-runtime validation completed: $($results.Count) synchronous installed-runtime executions."
}
else {
    Write-Output "Stage 5 deterministic-runtime gate passed: $($results.Count) synchronous installed-runtime executions. Final acceptance remains a separate evidence aggregation."
}
