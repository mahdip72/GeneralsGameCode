Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Stage5ExporterCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-Stage5ExporterFullPath {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is required."
    try {
        return [IO.Path]::GetFullPath($Path)
    }
    catch {
        throw "$Context path is invalid: $($_.Exception.Message)"
    }
}

function Assert-Stage5ExporterRegularDirectory {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $full -PathType Container) `
        "$Context directory was not found: $full"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context directory is a reparse point: $full"
    return $full.TrimEnd('\')
}

function Assert-Stage5ExporterExplicitTaskRoot {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    $root = [IO.Path]::GetPathRoot($full)
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($root)) `
        "$Context has no resolvable filesystem root: $full"
    Assert-Stage5ExporterCondition (-not [String]::Equals($full, $root,
        [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must name a directory below a filesystem root: $full"
    return Assert-Stage5ExporterRegularDirectory $full $Context
}

function Assert-Stage5ExporterContainedPathNoReparse {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath,
        [Parameter(Mandatory = $true)][string]$Context,
        [switch]$AllowBase
    )
    $base = Get-Stage5ExporterFullPath $BaseDirectory "$Context base"
    $candidate = Get-Stage5ExporterFullPath $CandidatePath $Context
    $base = $base.TrimEnd('\')
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Stage5ExporterCondition ($baseRoot -is [string] -and
        $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."

    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $minimumParts = if ($AllowBase) { $baseParts.Count } else { $baseParts.Count + 1 }
    Assert-Stage5ExporterCondition ($candidateParts.Count -ge $minimumParts) `
        "$Context path escapes or is not below its containing directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Stage5ExporterCondition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its containing directory."
    }

    $rootCurrent = $baseRoot
    foreach ($segment in @($candidate.Substring($candidateRoot.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $rootCurrent = Join-Path $rootCurrent $segment
        if (-not (Test-Path -LiteralPath $rootCurrent)) { break }
        $rootItem = Get-Item -LiteralPath $rootCurrent -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context ancestor path component '$segment' is a reparse point."
    }
    $baseItem = Get-Item -LiteralPath $base -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context containing directory is a reparse point."
    $current = $base
    for ($index = $baseParts.Count; $index -lt $candidateParts.Count; ++$index) {
        $current = Join-Path $current $candidateParts[$index]
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$($candidateParts[$index])' is a reparse point."
    }
    if (-not $AllowBase) {
        Assert-Stage5ExporterCondition (-not [String]::Equals($candidate, $base,
            [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path must not be the containing directory itself."
    }
    return $candidate
}

function Assert-Stage5ExporterTextContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$CandidatePath,
        [Parameter(Mandatory = $true)][string]$Context
    )
    # Source provenance may outlive the ephemeral source tree.  Keep validating
    # its textual containment without treating the missing path as an export
    # success; the durable destination and its hash remain authoritative.
    $base = (Get-Stage5ExporterFullPath $BaseDirectory "$Context base").TrimEnd('\')
    $candidate = Get-Stage5ExporterFullPath $CandidatePath $Context
    $baseRoot = [IO.Path]::GetPathRoot($base)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    Assert-Stage5ExporterCondition ($baseRoot -is [string] -and
        $candidateRoot -is [string] -and
        $baseRoot.Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) `
        "$Context path is on a different volume or share."

    $baseParts = @($base.Substring($baseRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $candidateParts = @($candidate.Substring($candidateRoot.Length) -split '[\\/]' |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    Assert-Stage5ExporterCondition ($candidateParts.Count -ge ($baseParts.Count + 1)) `
        "$Context path escapes or is not below its containing directory."
    for ($index = 0; $index -lt $baseParts.Count; ++$index) {
        Assert-Stage5ExporterCondition ($candidateParts[$index].Equals(
            $baseParts[$index], [StringComparison]::OrdinalIgnoreCase)) `
            "$Context path escapes its containing directory."
    }
    return $candidate
}

function Ensure-Stage5ExporterDirectory {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    if (-not (Test-Path -LiteralPath $full)) {
        New-Item -ItemType Directory -Path $full -Force | Out-Null
    }
    return Assert-Stage5ExporterRegularDirectory $full $Context
}

function Assert-Stage5ExporterPathAbsent {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Context)
    $full = Get-Stage5ExporterFullPath $Path $Context
    $item = Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
    Assert-Stage5ExporterCondition ($null -eq $item) `
        "$Context already exists; refusing overwrite: $full"
}

function Get-Stage5ExporterSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-Stage5ExporterUtf16String {
    param(
        [Parameter(Mandatory = $true)][IO.BinaryReader]$Reader,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $builder = New-Object Text.StringBuilder
    while ($Reader.BaseStream.Position + 2 -le $Reader.BaseStream.Length) {
        $character = $Reader.ReadUInt16()
        if ($character -eq 0) {
            return $builder.ToString()
        }
        Assert-Stage5ExporterCondition ($builder.Length -lt 65536) `
            "$Context wide string is unreasonably long."
        [void]$builder.Append([char]$character)
    }
    throw "$Context wide string is unterminated."
}

function Read-Stage5ReplayContainer {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-Stage5ExporterFullPath $Path 'Replay file'
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Assert-Stage5ExporterCondition ($stream.Length -ge 46) `
            "Replay file is shorter than the RPL3/GENREP prefix: $full"
        $reader = New-Object IO.BinaryReader($stream)
        try {
            $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            Assert-Stage5ExporterCondition ($magic -ceq 'RPL3') `
                "Replay file does not begin with RPL3: $full"
            $schemaVersion = $reader.ReadUInt32()
            $engineEpoch = $reader.ReadUInt32()
            [void]$reader.ReadUInt64()
            [void]$reader.ReadUInt64()
            $payloadByteCount = $reader.ReadUInt64()
            [void]$reader.ReadUInt32()
            Assert-Stage5ExporterCondition ($schemaVersion -eq 2) `
                "Replay file has unsupported RPL3 schema ${schemaVersion}: $full"
            Assert-Stage5ExporterCondition ($engineEpoch -eq 1) `
                "Replay file has unsupported runtime engine epoch ${engineEpoch}: $full"
            Assert-Stage5ExporterCondition ($payloadByteCount -ne [UInt64]::MaxValue -and
                $payloadByteCount -eq [UInt64]($stream.Length - 40)) `
                "Replay file has an incomplete or inconsistent RPL3 payload length: $full"

            $stream.Position = 40
            $genrep = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(6))
            Assert-Stage5ExporterCondition ($genrep -ceq 'GENREP') `
                "Replay file does not contain GENREP at the native payload start: $full"

            # Skip the native fixed fields written before replay/version strings:
            # start time, end time, frame count, two flags, and MAX_SLOTS flags.
            [void]$reader.ReadUInt32()
            [void]$reader.ReadUInt32()
            [void]$reader.ReadUInt32()
            [void]$reader.ReadByte()
            [void]$reader.ReadByte()
            for ($index = 0; $index -lt 8; ++$index) {
                [void]$reader.ReadByte()
            }
            [void](Get-Stage5ExporterUtf16String $reader 'Replay name')
            for ($index = 0; $index -lt 8; ++$index) {
                [void]$reader.ReadUInt16()
            }
            [void](Get-Stage5ExporterUtf16String $reader 'Replay version')
            $versionTime = Get-Stage5ExporterUtf16String $reader 'Replay version-time'
            $aiMarker = ' [SkirmishAIEpoch=3]'
            $markerMatches = [regex]::Matches($versionTime, '\[SkirmishAI[^\]]*\]')
            Assert-Stage5ExporterCondition ($markerMatches.Count -eq 1 -and
                $versionTime.EndsWith($aiMarker, [StringComparison]::Ordinal)) `
                "Replay file does not carry exactly one current Skirmish-AI epoch-3 marker: $full"
            return [pscustomobject]@{
                magic = $magic
                schemaVersion = [int]$schemaVersion
                engineEpoch = [int]$engineEpoch
                payloadByteCount = [UInt64]$payloadByteCount
                genrep = $genrep
                skirmishAiReplayEpoch = 3
                versionTime = $versionTime
            }
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-Stage5ReplayCompletionFields {
    param(
        [Parameter(Mandatory = $true)][string]$Output,
        [Parameter(Mandatory = $true)][int]$ExpectedSeed,
        [Parameter(Mandatory = $true)][string]$ExpectedScenario,
        [string]$Context = 'Fresh replay completion'
    )
    $lines = @($Output -split "`r?`n" | Where-Object {
        $_.StartsWith('SKIRMISH_AI_TEST_COMPLETE ', [StringComparison]::Ordinal)
    })
    Assert-Stage5ExporterCondition ($lines.Count -eq 1) `
        "$Context requires exactly one SKIRMISH_AI_TEST_COMPLETE line."
    $line = $lines[0]
    $fields = @{}
    $matches = [regex]::Matches($line.Substring('SKIRMISH_AI_TEST_COMPLETE '.Length),
        '(?<name>[A-Za-z_][A-Za-z0-9_]*)=(?:"(?<quoted>[^"]*)"|(?<plain>[^\s]+))')
    foreach ($match in $matches) {
        $name = $match.Groups['name'].Value
        Assert-Stage5ExporterCondition (-not $fields.ContainsKey($name)) `
            "$Context repeats field '$name'."
        $fields[$name] = if ($match.Groups['quoted'].Success) {
            $match.Groups['quoted'].Value
        }
        else {
            $match.Groups['plain'].Value
        }
    }
    foreach ($required in @('seed', 'scenario', 'run_nonce', 'replay_epoch',
        'replay_sha256', 'replay_retained')) {
        Assert-Stage5ExporterCondition ($fields.ContainsKey($required)) `
            "$Context is missing field '$required'."
    }
    [int]$seed = 0
    Assert-Stage5ExporterCondition ([int]::TryParse([string]$fields['seed'], [ref]$seed) -and
        $seed -eq $ExpectedSeed) "$Context seed does not match the plan."
    Assert-Stage5ExporterCondition ([string]$fields['scenario'] -ceq $ExpectedScenario) `
        "$Context scenario does not match the plan."
    [int]$replayEpoch = 0
    Assert-Stage5ExporterCondition ([int]::TryParse([string]$fields['replay_epoch'], [ref]$replayEpoch) -and
        $replayEpoch -eq 3) "$Context replay epoch is not 3."
    $replaySha256 = [string]$fields['replay_sha256']
    Assert-Stage5ExporterCondition ($replaySha256 -match '^[0-9A-Fa-f]{64}$') `
        "$Context replay SHA-256 is invalid."
    $runNonce = [string]$fields['run_nonce']
    Assert-Stage5ExporterCondition ($runNonce -match '^[0-9A-Fa-f-]{1,64}$') `
        "$Context run nonce is invalid."
    $replayRetained = [string]$fields['replay_retained']
    Assert-Stage5ExporterCondition (-not [string]::IsNullOrWhiteSpace($replayRetained) -and
        $replayRetained -cne 'unavailable') "$Context has no retained replay path."
    return [pscustomobject]@{
        seed = $seed
        scenario = [string]$fields['scenario']
        runNonce = $runNonce
        replayEpoch = $replayEpoch
        replaySha256 = $replaySha256.ToUpperInvariant()
        replayRetained = $replayRetained
    }
}

function Get-Stage5ExporterMetadataValue {
    param([Collections.IDictionary]$Metadata, [string]$Name)
    Assert-Stage5ExporterCondition ($Metadata.Contains($Name)) `
        "Fresh replay metadata is missing '$Name'."
    return [string]$Metadata[$Name]
}

function Assert-Stage5ExporterMetadata {
    param([Collections.IDictionary]$Metadata)
    Assert-Stage5ExporterCondition ($null -ne $Metadata) `
        'Fresh replay metadata is required.'
    foreach ($name in @('title', 'category', 'scenario', 'seed', 'runNonce',
        'executableSha256', 'origin')) {
        [void](Get-Stage5ExporterMetadataValue $Metadata $name)
    }
    $title = Get-Stage5ExporterMetadataValue $Metadata 'title'
    Assert-Stage5ExporterCondition ($title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Fresh replay metadata title contains unsupported path characters.'
    $category = Get-Stage5ExporterMetadataValue $Metadata 'category'
    Assert-Stage5ExporterCondition ($category -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') `
        'Fresh replay metadata category contains unsupported path characters.'
    $scenario = Get-Stage5ExporterMetadataValue $Metadata 'scenario'
    Assert-Stage5ExporterCondition ($scenario -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') `
        'Fresh replay metadata scenario contains unsupported path characters.'
    [int]$seed = 0
    Assert-Stage5ExporterCondition ([int]::TryParse(
        (Get-Stage5ExporterMetadataValue $Metadata 'seed'), [ref]$seed) -and $seed -gt 0) `
        'Fresh replay metadata seed must be a positive integer.'
    $runNonce = Get-Stage5ExporterMetadataValue $Metadata 'runNonce'
    Assert-Stage5ExporterCondition ($runNonce -match '^[0-9A-Fa-f-]{1,64}$') `
        'Fresh replay metadata runNonce is invalid.'
    $executableSha256 = Get-Stage5ExporterMetadataValue $Metadata 'executableSha256'
    Assert-Stage5ExporterCondition ($executableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Fresh replay metadata executableSha256 is invalid.'
    $origin = Get-Stage5ExporterMetadataValue $Metadata 'origin'
    Assert-Stage5ExporterCondition ($origin -ceq 'native-fresh-runtime') `
        "Fresh replay metadata origin must be 'native-fresh-runtime'."
}

function ConvertTo-Stage5ExporterPathComponent {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value.Replace(' ', '_')
}

function Copy-Stage5ReplayWithStableSource {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$TemporaryPath
    )
    $source = [IO.File]::Open($SourcePath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        $sourceLength = [Int64]$source.Length
        $temporary = [IO.File]::Open($TemporaryPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $buffer = New-Object byte[] 1048576
                $hashBuffer = New-Object byte[] 1048576
                while ($true) {
                    $read = $source.Read($buffer, 0, $buffer.Length)
                    if ($read -eq 0) { break }
                    $temporary.Write($buffer, 0, $read)
                    [void]$sha.TransformBlock($buffer, 0, $read, $hashBuffer, 0)
                }
                [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
                $sourceSha256 = (($sha.Hash | ForEach-Object {
                    $_.ToString('x2')
                }) -join '').ToUpperInvariant()
            }
            finally { $sha.Dispose() }
            $temporary.Flush($true)
        }
        finally { $temporary.Dispose() }
        Assert-Stage5ExporterCondition ($source.Position -eq $sourceLength) `
            "Replay source changed while it was copied: $SourcePath"
        $after = Get-Item -LiteralPath $SourcePath -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition ([Int64]$after.Length -eq $sourceLength -and
            ($after.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Replay source changed or became a reparse point while it was copied: $SourcePath"
        return [pscustomobject]@{
            sha256 = $sourceSha256
            length = $sourceLength
        }
    }
    finally { $source.Dispose() }
}

function Export-Stage5FreshReplayArtifact {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$TaskRunRoot,
        [Parameter(Mandatory = $true)][string]$ProfileRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Metadata
    )
    Assert-Stage5ExporterMetadata $Metadata
    Assert-Stage5ExporterCondition ($ExpectedSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Expected replay SHA-256 is invalid.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $taskRunRootFull = Assert-Stage5ExporterRegularDirectory $TaskRunRoot 'Task run root'
    $profileRootFull = Assert-Stage5ExporterRegularDirectory $ProfileRoot 'Profile root'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $taskRunRootFull `
        'Task run root' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $taskRunRootFull $profileRootFull `
        'Profile root' | Out-Null
    $sourceFull = Assert-Stage5ExporterContainedPathNoReparse $profileRootFull $SourcePath `
        'Retained replay source'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $sourceFull -PathType Leaf) `
        "Retained replay source was not found: $sourceFull"
    $sourceItem = Get-Item -LiteralPath $sourceFull -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Retained replay source is a reparse point: $sourceFull"

    $corpusRootFull = Get-Stage5ExporterFullPath $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    Assert-Stage5ExporterCondition (-not [String]::Equals($corpusRootFull, $taskRootFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'CorpusExportRoot must be below TaskRoot.'
    Ensure-Stage5ExporterDirectory $corpusRootFull 'CorpusExportRoot' | Out-Null
    $titleComponent = ConvertTo-Stage5ExporterPathComponent `
        (Get-Stage5ExporterMetadataValue $Metadata 'title')
    $categoryComponent = Get-Stage5ExporterMetadataValue $Metadata 'category'
    $scenarioComponent = Get-Stage5ExporterMetadataValue $Metadata 'scenario'
    [int]$seed = 0
    [void][int]::TryParse((Get-Stage5ExporterMetadataValue $Metadata 'seed'), [ref]$seed)
    $runNonce = Get-Stage5ExporterMetadataValue $Metadata 'runNonce'
    $destinationDirectory = Join-Path (Join-Path $corpusRootFull $titleComponent) $categoryComponent
    Ensure-Stage5ExporterDirectory $destinationDirectory 'Replay export directory' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $destinationDirectory `
        'Replay export directory' | Out-Null
    $destinationName = '{0}-scenario-{1}-seed-{2}-{3}.rep' -f `
        $categoryComponent, $scenarioComponent, $seed, $runNonce
    $destinationFull = [IO.Path]::GetFullPath((Join-Path $destinationDirectory $destinationName))
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $destinationFull `
        'Replay export destination' | Out-Null
    Assert-Stage5ExporterPathAbsent $destinationFull 'Replay export destination'

    $temporaryFull = '{0}.tmp-{1}' -f $destinationFull, [Guid]::NewGuid().ToString('N')
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $temporaryFull `
        'Replay export temporary file' | Out-Null
    $temporaryOwned = $true
    try {
        $copy = Copy-Stage5ReplayWithStableSource $sourceFull $temporaryFull
        Assert-Stage5ExporterCondition ($copy.sha256 -ceq $ExpectedSha256.ToUpperInvariant()) `
            "Retained replay source SHA-256 mismatch. Expected $ExpectedSha256, got $($copy.sha256)."
        $header = Read-Stage5ReplayContainer $temporaryFull
        Assert-Stage5ExporterCondition ((Get-Stage5ExporterSha256 $temporaryFull) -ceq $copy.sha256) `
            'Temporary replay copy SHA-256 differs from the stable source snapshot.'
        [IO.File]::Move($temporaryFull, $destinationFull)
        $temporaryOwned = $false
        Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $destinationFull `
            'Replay export destination' | Out-Null
        $destinationItem = Get-Item -LiteralPath $destinationFull -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Replay export destination is a reparse point: $destinationFull"
        $destinationSha256 = Get-Stage5ExporterSha256 $destinationFull
        Assert-Stage5ExporterCondition ($destinationSha256 -ceq $copy.sha256) `
            'Replay export destination SHA-256 differs from the source snapshot.'
        $destinationHeader = Read-Stage5ReplayContainer $destinationFull
        Assert-Stage5ExporterCondition ($destinationHeader.magic -ceq $header.magic -and
            $destinationHeader.schemaVersion -eq $header.schemaVersion -and
            $destinationHeader.engineEpoch -eq $header.engineEpoch -and
            $destinationHeader.skirmishAiReplayEpoch -eq $header.skirmishAiReplayEpoch) `
            'Replay export destination header differs from the validated source snapshot.'
        return [pscustomobject]@{
            origin = Get-Stage5ExporterMetadataValue $Metadata 'origin'
            title = Get-Stage5ExporterMetadataValue $Metadata 'title'
            category = $categoryComponent
            scenario = $scenarioComponent
            seed = $seed
            runNonce = $runNonce
            executableSha256 = (Get-Stage5ExporterMetadataValue $Metadata 'executableSha256').ToUpperInvariant()
            sourceProfileRoot = $profileRootFull
            sourcePath = $sourceFull
            destinationPath = $destinationFull
            sourceSha256 = $copy.sha256
            destinationSha256 = $destinationSha256
            length = $copy.length
            containerMagic = $destinationHeader.magic
            containerSchemaVersion = $destinationHeader.schemaVersion
            containerEngineEpoch = $destinationHeader.engineEpoch
            payloadMagic = $destinationHeader.genrep
            skirmishAiReplayEpoch = $destinationHeader.skirmishAiReplayEpoch
            exportedUtc = ([DateTime]::UtcNow).ToString('o')
        }
    }
    finally {
        if ($temporaryOwned -and (Test-Path -LiteralPath $temporaryFull)) {
            Remove-Item -LiteralPath $temporaryFull -Force -ErrorAction SilentlyContinue
        }
    }
}

function Assert-Stage5ExporterRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [switch]$AllowMissingSource
    )
    foreach ($name in @('origin', 'title', 'category', 'scenario', 'seed',
        'runNonce', 'executableSha256', 'sourceProfileRoot', 'sourcePath',
        'destinationPath', 'sourceSha256', 'destinationSha256',
        'containerMagic', 'containerSchemaVersion', 'containerEngineEpoch',
        'payloadMagic', 'skirmishAiReplayEpoch')) {
        Assert-Stage5ExporterCondition ($null -ne $Record.PSObject.Properties[$name]) `
            "Corpus manifest record is missing '$name'."
    }
    Assert-Stage5ExporterCondition ([string]$Record.origin -ceq 'native-fresh-runtime') `
        'Corpus manifest record origin is not native-fresh-runtime.'
    Assert-Stage5ExporterCondition ([string]$Record.title -ceq $Title) `
        'Corpus manifest record title does not match the manifest title.'
    Assert-Stage5ExporterCondition ([string]$Record.executableSha256 -ceq $ExecutableSha256.ToUpperInvariant()) `
        'Corpus manifest record executable SHA-256 does not match the manifest executable.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusRoot 'CorpusExportRoot'
    $profileRootCandidate = [IO.Path]::GetFullPath([string]$Record.sourceProfileRoot)
    $profileRootExists = Test-Path -LiteralPath $profileRootCandidate -PathType Container
    if ($AllowMissingSource -and -not $profileRootExists) {
        $profileRootFull = Assert-Stage5ExporterTextContainedPath $taskRootFull `
            $profileRootCandidate 'Corpus record source profile root'
    }
    else {
        $profileRootFull = Assert-Stage5ExporterRegularDirectory $profileRootCandidate `
            'Corpus record source profile root'
        Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $profileRootFull `
            'Corpus record source profile root' | Out-Null
    }
    $sourceCandidate = [IO.Path]::GetFullPath([string]$Record.sourcePath)
    $sourceExists = Test-Path -LiteralPath $sourceCandidate -PathType Leaf
    if ($AllowMissingSource -and -not $sourceExists) {
        $sourceFull = Assert-Stage5ExporterTextContainedPath $profileRootFull `
            $sourceCandidate 'Corpus record source path'
    }
    else {
        $sourceFull = Assert-Stage5ExporterContainedPathNoReparse $profileRootFull `
            $sourceCandidate 'Corpus record source path'
    }
    $destinationFull = Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull `
        ([string]$Record.destinationPath) 'Corpus record destination path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $destinationFull -PathType Leaf) `
        "Corpus record destination file was not found: $destinationFull"
    $destinationItem = Get-Item -LiteralPath $destinationFull -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        'Corpus manifest record references a reparse-point artifact.'
    Assert-Stage5ExporterCondition ([string]$Record.sourceSha256 -match '^[0-9A-Fa-f]{64}$' -and
        [string]$Record.destinationSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest record contains an invalid artifact SHA-256.'
    $destinationSha256 = Get-Stage5ExporterSha256 $destinationFull
    Assert-Stage5ExporterCondition ($destinationSha256 -ceq
        ([string]$Record.destinationSha256).ToUpperInvariant()) `
        'Corpus manifest record artifact SHA-256 does not match its files.'
    if ($sourceExists) {
        $sourceItem = Get-Item -LiteralPath $sourceFull -Force -ErrorAction Stop
        Assert-Stage5ExporterCondition (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            'Corpus manifest record references a reparse-point artifact.'
        Assert-Stage5ExporterCondition ((Get-Stage5ExporterSha256 $sourceFull) -ceq
            ([string]$Record.sourceSha256).ToUpperInvariant()) `
            'Corpus manifest record source SHA-256 does not match its file.'
    }
    else {
        Assert-Stage5ExporterCondition ($AllowMissingSource -and
            [string]$Record.sourceSha256 -ceq ([string]$Record.destinationSha256)) `
            'Corpus manifest record source is unavailable and cannot be provenance-matched.'
    }
    Assert-Stage5ExporterCondition ([string]$Record.containerMagic -ceq 'RPL3' -and
        [int]$Record.containerSchemaVersion -eq 2 -and
        [int]$Record.containerEngineEpoch -eq 1 -and
        [string]$Record.payloadMagic -ceq 'GENREP' -and
        [int]$Record.skirmishAiReplayEpoch -eq 3) `
        'Corpus manifest record has an invalid native replay container contract.'
    $header = Read-Stage5ReplayContainer $destinationFull
    Assert-Stage5ExporterCondition ($header.magic -ceq 'RPL3' -and
        $header.schemaVersion -eq 2 -and $header.engineEpoch -eq 1 -and
        $header.genrep -ceq 'GENREP' -and $header.skirmishAiReplayEpoch -eq 3) `
        'Corpus manifest record destination failed native replay revalidation.'
}

function Get-Stage5ExporterJsonProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )
    Assert-Stage5ExporterCondition ($null -ne $Object) `
        "$Context must be a JSON object."
    $property = $Object.PSObject.Properties[$Name]
    Assert-Stage5ExporterCondition ($null -ne $property -and $null -ne $property.Value) `
        "$Context is missing required property '$Name'."
    return $property.Value
}

function Get-Stage5ExporterJsonDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $full = Get-Stage5ExporterFullPath $Path $Context
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $full -PathType Leaf) `
        "$Context file was not found: $full"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-Stage5ExporterCondition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context file is a reparse point: $full"
    try {
        return ([IO.File]::ReadAllText($full) | ConvertFrom-Json)
    }
    catch {
        throw "$Context is not valid JSON: $($_.Exception.Message)"
    }
}

function Get-Stage5ExporterBoundFile {
    param(
        [Parameter(Mandatory = $true)][object]$Binding,
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $path = [string](Get-Stage5ExporterJsonProperty $Binding 'path' $Context)
    $expectedSha256 = [string](Get-Stage5ExporterJsonProperty $Binding 'sha256' $Context)
    Assert-Stage5ExporterCondition ($expectedSha256 -match '^[0-9A-Fa-f]{64}$') `
        "$Context SHA-256 is invalid."
    $full = Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $path $Context
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $full -PathType Leaf) `
        "$Context file was not found: $full"
    $actualSha256 = Get-Stage5ExporterSha256 $full
    Assert-Stage5ExporterCondition ($actualSha256 -ceq $expectedSha256.ToUpperInvariant()) `
        "$Context SHA-256 mismatch. Expected $expectedSha256, got $actualSha256."
    return [pscustomobject]@{ path = $full; sha256 = $actualSha256 }
}

function Get-Stage5ExporterRecordIdentity {
    param([Parameter(Mandatory = $true)][object]$Record)
    $names = @('origin', 'title', 'category', 'scenario', 'seed', 'runNonce',
        'executableSha256', 'sourceProfileRoot', 'sourcePath', 'sourceSha256',
        'destinationPath', 'destinationSha256', 'length', 'containerMagic',
        'containerSchemaVersion', 'containerEngineEpoch', 'payloadMagic',
        'skirmishAiReplayEpoch', 'sequence', 'configuration', 'repeat',
        'replayEpoch', 'replaySha256', 'exportedUtc')
    $values = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $names) {
        $property = $Record.PSObject.Properties[$name]
        $value = ''
        if ($null -ne $property) { $value = [string]$property.Value }
        $values.Add($value) | Out-Null
    }
    return [string]::Join('|', $values.ToArray())
}

function Assert-Stage5ExporterRecordSetsEqual {
    param(
        [Parameter(Mandatory = $true)][object[]]$Left,
        [Parameter(Mandatory = $true)][object[]]$Right,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $leftKeys = @($Left | ForEach-Object { Get-Stage5ExporterRecordIdentity $_ } |
        Sort-Object)
    $rightKeys = @($Right | ForEach-Object { Get-Stage5ExporterRecordIdentity $_ } |
        Sort-Object)
    Assert-Stage5ExporterCondition ($leftKeys.Count -eq $rightKeys.Count) `
        "$Context record counts differ."
    for ($index = 0; $index -lt $leftKeys.Count; ++$index) {
        Assert-Stage5ExporterCondition ($leftKeys[$index] -ceq $rightKeys[$index]) `
            "$Context records differ."
    }
}

function Write-Stage5ExporterJsonAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Document,
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $full = Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $Path $Context
    Assert-Stage5ExporterPathAbsent $full $Context
    $temporaryFull = '{0}.tmp-{1}' -f $full, [Guid]::NewGuid().ToString('N')
    Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $temporaryFull `
        "$Context temporary path" | Out-Null
    $temporaryOwned = $true
    try {
        $json = $Document | ConvertTo-Json -Depth 24
        $stream = [IO.File]::Open($temporaryFull, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes($json)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally { $stream.Dispose() }
        [IO.File]::Move($temporaryFull, $full)
        $temporaryOwned = $false
        Assert-Stage5ExporterContainedPathNoReparse $BaseDirectory $full $Context | Out-Null
        return [pscustomobject]@{
            path = $full
            sha256 = Get-Stage5ExporterSha256 $full
        }
    }
    finally {
        if ($temporaryOwned -and (Test-Path -LiteralPath $temporaryFull)) {
            Remove-Item -LiteralPath $temporaryFull -Force -ErrorAction SilentlyContinue
        }
    }
}

function Read-Stage5FreshReplayCorpusBundle {
    param([Parameter(Mandatory = $true)][string]$CorpusManifestPath)
    $manifestFull = Get-Stage5ExporterFullPath $CorpusManifestPath 'Corpus manifest'
    $manifest = Get-Stage5ExporterJsonDocument $manifestFull 'Corpus manifest'
    Assert-Stage5ExporterCondition ([int](Get-Stage5ExporterJsonProperty $manifest `
        'schemaVersion' 'Corpus manifest') -eq 1) `
        'Corpus manifest schemaVersion must be 1.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $manifest `
        'kind' 'Corpus manifest') -ceq 'stage5-native-replay-corpus') `
        'Corpus manifest kind is not stage5-native-replay-corpus.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $manifest `
        'origin' 'Corpus manifest') -ceq 'native-fresh-runtime') `
        'Corpus manifest origin is not native-fresh-runtime.'
    $title = [string](Get-Stage5ExporterJsonProperty $manifest 'title' 'Corpus manifest')
    $executableSha256 = [string](Get-Stage5ExporterJsonProperty $manifest `
        'executableSha256' 'Corpus manifest')
    Assert-Stage5ExporterCondition ($executableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest executable SHA-256 is invalid.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot `
        ([string](Get-Stage5ExporterJsonProperty $manifest 'taskRoot' 'Corpus manifest')) `
        'Corpus manifest taskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory `
        ([string](Get-Stage5ExporterJsonProperty $manifest 'corpusExportRoot' 'Corpus manifest')) `
        'Corpus manifest corpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'Corpus manifest corpusExportRoot' | Out-Null
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $manifestFull `
        'Corpus manifest path' | Out-Null
    $resultsBinding = Get-Stage5ExporterBoundFile `
        (Get-Stage5ExporterJsonProperty $manifest 'validationResults' 'Corpus manifest') `
        $taskRootFull 'Corpus manifest validation results'
    $receiptBinding = Get-Stage5ExporterBoundFile `
        (Get-Stage5ExporterJsonProperty $manifest 'validationReceipt' 'Corpus manifest') `
        $taskRootFull 'Corpus manifest validation receipt'
    $receipt = Get-Stage5ExporterJsonDocument $receiptBinding.path 'Validation receipt'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'receiptKind' 'Validation receipt') -ceq 'stage5-local-capacity-receipt') `
        'Corpus manifest validation receipt has an unexpected kind.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receipt `
        'status' 'Validation receipt') -ceq 'passed-non-acceptance') `
        'Corpus manifest validation receipt is not a passed non-acceptance receipt.'
    Assert-Stage5ExporterCondition ((Get-Stage5ExporterJsonProperty $receipt `
        'notAnAcceptanceEnvelope' 'Validation receipt') -is [bool] -and
        [bool](Get-Stage5ExporterJsonProperty $receipt 'notAnAcceptanceEnvelope' `
            'Validation receipt')) `
        'Corpus manifest validation receipt is not marked non-acceptance.'
    Assert-Stage5ExporterCondition ((Get-Stage5ExporterJsonProperty $receipt `
        'finalAcceptanceEligible' 'Validation receipt') -is [bool] -and
        -not [bool](Get-Stage5ExporterJsonProperty $receipt 'finalAcceptanceEligible' `
            'Validation receipt')) `
        'Corpus manifest validation receipt unexpectedly claims final acceptance.'
    $receiptExport = Get-Stage5ExporterJsonProperty $receipt 'corpusExport' `
        'Validation receipt'
    $artifactIndexBinding = Get-Stage5ExporterBoundFile `
        ([pscustomobject]@{
            path = Get-Stage5ExporterJsonProperty $receiptExport 'artifactIndexPath' `
                'Validation receipt corpusExport'
            sha256 = Get-Stage5ExporterJsonProperty $receiptExport 'artifactIndexSha256' `
                'Validation receipt corpusExport'
        }) $taskRootFull 'Validation receipt artifact index'
    $artifactIndex = Get-Stage5ExporterJsonDocument $artifactIndexBinding.path `
        'Artifact index'
    Assert-Stage5ExporterCondition ([int](Get-Stage5ExporterJsonProperty $artifactIndex `
        'schemaVersion' 'Artifact index') -eq 1) `
        'Artifact index schemaVersion must be 1.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'kind' 'Artifact index') -ceq 'stage5-native-replay-artifact-index') `
        'Artifact index kind is invalid.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'origin' 'Artifact index') -ceq 'native-fresh-runtime') `
        'Artifact index origin is invalid.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'title' 'Artifact index') -ceq $title) `
        'Artifact index title does not match the corpus manifest.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $artifactIndex `
        'executableSha256' 'Artifact index') -ceq $executableSha256.ToUpperInvariant()) `
        'Artifact index executable SHA-256 does not match the corpus manifest.'
    $indexResultsBinding = Get-Stage5ExporterBoundFile `
        (Get-Stage5ExporterJsonProperty $artifactIndex 'validationResults' 'Artifact index') `
        $taskRootFull 'Artifact index validation results'
    Assert-Stage5ExporterCondition ($indexResultsBinding.sha256 -ceq $resultsBinding.sha256 -and
        [String]::Equals($indexResultsBinding.path, $resultsBinding.path,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index validation-results binding differs from the corpus manifest.'
    $expectedManifestFull = [IO.Path]::GetFullPath((Join-Path $corpusRootFull `
        'native-replay-corpus-manifest.json'))
    Assert-Stage5ExporterCondition ([String]::Equals($manifestFull, $expectedManifestFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Corpus manifest must use the fixed native-replay-corpus-manifest.json filename.'
    Assert-Stage5ExporterCondition ([string](Get-Stage5ExporterJsonProperty $receiptExport `
        'status' 'Validation receipt corpusExport') -ceq 'passed') `
        'Validation receipt corpusExport status is not passed.'
    $receiptCorpusRoot = Get-Stage5ExporterFullPath `
        ([string](Get-Stage5ExporterJsonProperty $receiptExport 'corpusExportRoot' `
            'Validation receipt corpusExport')) 'Validation receipt corpusExportRoot'
    Assert-Stage5ExporterCondition ([String]::Equals($receiptCorpusRoot, $corpusRootFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Validation receipt corpusExportRoot differs from the corpus manifest.'
    Assert-Stage5ExporterCondition ([String]::Equals(
        [string](Get-Stage5ExporterJsonProperty $artifactIndex 'taskRoot' 'Artifact index'),
        $taskRootFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index taskRoot differs from the corpus manifest.'
    Assert-Stage5ExporterCondition ([String]::Equals(
        [string](Get-Stage5ExporterJsonProperty $artifactIndex 'corpusExportRoot' 'Artifact index'),
        $corpusRootFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index corpusExportRoot differs from the corpus manifest.'
    $expectedArtifactIndexFull = [IO.Path]::GetFullPath((Join-Path $corpusRootFull `
        'native-replay-artifact-index.json'))
    Assert-Stage5ExporterCondition ([String]::Equals($artifactIndexBinding.path,
        $expectedArtifactIndexFull, [StringComparison]::OrdinalIgnoreCase)) `
        'Artifact index must use the fixed native-replay-artifact-index.json filename.'
    $records = @((Get-Stage5ExporterJsonProperty $manifest 'records' 'Corpus manifest'))
    $indexRecords = @((Get-Stage5ExporterJsonProperty $artifactIndex 'records' 'Artifact index'))
    $receiptRecords = @((Get-Stage5ExporterJsonProperty $receiptExport 'records' `
        'Validation receipt corpusExport'))
    $receiptRecordCount = [int](Get-Stage5ExporterJsonProperty $receiptExport 'recordCount' `
        'Validation receipt corpusExport')
    $indexRecordCount = [int](Get-Stage5ExporterJsonProperty $artifactIndex 'recordCount' `
        'Artifact index')
    Assert-Stage5ExporterCondition ($records.Count -gt 0 -and
        $records.Count -eq $indexRecords.Count -and
        $records.Count -eq $receiptRecords.Count -and
        $records.Count -eq $indexRecordCount -and
        $records.Count -eq $receiptRecordCount) `
        'Corpus manifest, validation receipt, and artifact index record counts are invalid.'
    foreach ($record in $records) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    foreach ($record in $indexRecords) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    foreach ($record in $receiptRecords) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $title $executableSha256 `
            -AllowMissingSource
    }
    Assert-Stage5ExporterRecordSetsEqual $records $indexRecords `
        'Corpus manifest and artifact index'
    Assert-Stage5ExporterRecordSetsEqual $records $receiptRecords `
        'Corpus manifest and validation receipt'
    return [pscustomobject]@{
        manifestPath = $manifestFull
        manifestSha256 = Get-Stage5ExporterSha256 $manifestFull
        manifest = $manifest
        taskRoot = $taskRootFull
        corpusRoot = $corpusRootFull
        title = $title
        executableSha256 = $executableSha256.ToUpperInvariant()
        results = $resultsBinding
        receipt = $receiptBinding
        artifactIndex = $artifactIndexBinding
        records = $records
    }
}

function Write-Stage5FreshReplayArtifactIndex {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$ValidationResultsPath
    )
    Assert-Stage5ExporterCondition ($Title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Artifact index title is invalid.'
    Assert-Stage5ExporterCondition ($ExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Artifact index executable SHA-256 is invalid.'
    Assert-Stage5ExporterCondition ($null -ne $Records -and $Records.Count -gt 0) `
        'Artifact index requires at least one exported replay record.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    $resultsFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull `
        $ValidationResultsPath 'Validation results path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $resultsFull -PathType Leaf) `
        "Validation results file was not found: $resultsFull"
    foreach ($record in @($Records)) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $Title $ExecutableSha256
    }
    $indexFull = Join-Path $corpusRootFull 'native-replay-artifact-index.json'
    $document = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-artifact-index'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        taskRoot = $taskRootFull
        corpusExportRoot = $corpusRootFull
        title = $Title
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        validationResults = [ordered]@{
            path = $resultsFull
            sha256 = Get-Stage5ExporterSha256 $resultsFull
        }
        recordCount = $Records.Count
        records = @($Records)
    }
    $written = Write-Stage5ExporterJsonAtomically $indexFull $document `
        $corpusRootFull 'Artifact index'
    return [pscustomobject]@{
        path = $written.path
        sha256 = $written.sha256
        recordCount = $Records.Count
    }
}

function Write-Stage5FreshReplayCorpusManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusExportRoot,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$ExecutableSha256,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$ValidationResultsPath,
        [string]$ValidationReceiptPath = ''
    )
    Assert-Stage5ExporterCondition ($Title -match '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,79}$') `
        'Corpus manifest title is invalid.'
    Assert-Stage5ExporterCondition ($ExecutableSha256 -match '^[0-9A-Fa-f]{64}$') `
        'Corpus manifest executable SHA-256 is invalid.'
    Assert-Stage5ExporterCondition ($null -ne $Records -and $Records.Count -gt 0) `
        'Corpus manifest requires at least one exported replay record.'
    $taskRootFull = Assert-Stage5ExporterExplicitTaskRoot $TaskRoot 'TaskRoot'
    $corpusRootFull = Assert-Stage5ExporterRegularDirectory $CorpusExportRoot 'CorpusExportRoot'
    Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $corpusRootFull `
        'CorpusExportRoot' | Out-Null
    $resultsFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $ValidationResultsPath `
        'Validation results path'
    Assert-Stage5ExporterCondition (Test-Path -LiteralPath $resultsFull -PathType Leaf) `
        "Validation results file was not found: $resultsFull"
    $manifestFull = Join-Path $corpusRootFull 'native-replay-corpus-manifest.json'
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $manifestFull `
        'Corpus manifest path' | Out-Null
    Assert-Stage5ExporterPathAbsent $manifestFull 'Corpus manifest'
    foreach ($record in @($Records)) {
        Assert-Stage5ExporterRecord $record $taskRootFull $corpusRootFull $Title $ExecutableSha256
    }
    $receiptBinding = $null
    if (-not [string]::IsNullOrWhiteSpace($ValidationReceiptPath)) {
        $receiptFull = Assert-Stage5ExporterContainedPathNoReparse $taskRootFull $ValidationReceiptPath `
            'Validation receipt path'
        Assert-Stage5ExporterCondition (Test-Path -LiteralPath $receiptFull -PathType Leaf) `
            "Validation receipt file was not found: $receiptFull"
        $receiptBinding = [ordered]@{
            path = $receiptFull
            sha256 = Get-Stage5ExporterSha256 $receiptFull
        }
    }
    $document = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-corpus'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        taskRoot = $taskRootFull
        corpusExportRoot = $corpusRootFull
        title = $Title
        executableSha256 = $ExecutableSha256.ToUpperInvariant()
        validationResults = [ordered]@{
            path = $resultsFull
            sha256 = Get-Stage5ExporterSha256 $resultsFull
        }
        validationReceipt = $receiptBinding
        records = @($Records)
    }
    $temporaryFull = '{0}.tmp-{1}' -f $manifestFull, [Guid]::NewGuid().ToString('N')
    Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $temporaryFull `
        'Corpus manifest temporary path' | Out-Null
    $temporaryOwned = $true
    try {
        $json = $document | ConvertTo-Json -Depth 16
        $stream = [IO.File]::Open($temporaryFull, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes($json)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally { $stream.Dispose() }
        [IO.File]::Move($temporaryFull, $manifestFull)
        $temporaryOwned = $false
        Assert-Stage5ExporterContainedPathNoReparse $corpusRootFull $manifestFull `
            'Corpus manifest path' | Out-Null
        return [pscustomobject]@{
            path = $manifestFull
            sha256 = Get-Stage5ExporterSha256 $manifestFull
            recordCount = $Records.Count
        }
    }
    finally {
        if ($temporaryOwned -and (Test-Path -LiteralPath $temporaryFull)) {
            Remove-Item -LiteralPath $temporaryFull -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-Stage5ExporterSelectionKey {
    param([Parameter(Mandatory = $true)][object]$Record)
    $sequence = 0
    $sequenceProperty = $Record.PSObject.Properties['sequence']
    if ($null -ne $sequenceProperty) { $sequence = [int]$sequenceProperty.Value }
    $repeat = 0
    $repeatProperty = $Record.PSObject.Properties['repeat']
    if ($null -ne $repeatProperty) { $repeat = [int]$repeatProperty.Value }
    $configuration = ''
    $configurationProperty = $Record.PSObject.Properties['configuration']
    if ($null -ne $configurationProperty) { $configuration = [string]$configurationProperty.Value }
    return ('{0:D10}|{1}|{2:D10}|{3:D10}|{4}|{5}|{6}|{7}' -f `
        $sequence, [string]$Record.scenario, [int]$Record.seed, $repeat,
        $configuration, [string]$Record.runNonce,
        [string]$Record.sourceSha256, [string]$Record.destinationSha256)
}

function Convert-Stage5FreshReplayCorpusManifestToFixtures {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CorpusManifestPath,
        [Parameter(Mandatory = $true)][string]$FixtureManifestPath,
        [Parameter(Mandatory = $true)][string]$ProvenancePath,
        [Parameter(Mandatory = $true)][string]$Executable
    )
    Assert-Stage5ExporterCondition ($Executable -match '^[A-Za-z0-9._-]+\.exe$') `
        'Native fixture executable must be a leaf .exe name.'
    $bundle = Read-Stage5FreshReplayCorpusBundle $CorpusManifestPath
    Assert-Stage5ExporterCondition ($bundle.title -ceq 'Generals' -or
        $bundle.title -ceq 'ZeroHour') `
        'Native fixture conversion requires a Generals or ZeroHour corpus title.'
    $expectedExecutablePrefix = if ($bundle.title -ceq 'Generals') {
        'generalsv'
    }
    else {
        'generalszh'
    }
    Assert-Stage5ExporterCondition ($Executable -match ('^' +
        [regex]::Escape($expectedExecutablePrefix) +
        '(?:-[A-Za-z0-9._-]+)?\.exe$')) `
        "Native fixture executable '$Executable' does not belong to corpus title '$($bundle.title)'."
    $fixtureFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
        $FixtureManifestPath 'Native fixture manifest path'
    $provenanceFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
        $ProvenancePath 'Native fixture provenance path'
    $fixtureParent = [IO.Path]::GetDirectoryName($fixtureFull).TrimEnd('\')
    $provenanceParent = [IO.Path]::GetDirectoryName($provenanceFull).TrimEnd('\')
    Assert-Stage5ExporterCondition ([String]::Equals($fixtureParent, $bundle.corpusRoot,
        [StringComparison]::OrdinalIgnoreCase) -and
        [String]::Equals($provenanceParent, $bundle.corpusRoot,
            [StringComparison]::OrdinalIgnoreCase)) `
        'Native fixture manifest and provenance must be direct children of CorpusExportRoot.'
    Assert-Stage5ExporterCondition (-not [String]::Equals($fixtureFull, $provenanceFull,
        [StringComparison]::OrdinalIgnoreCase)) `
        'Native fixture manifest and provenance paths must differ.'
    Assert-Stage5ExporterPathAbsent $fixtureFull 'Native fixture manifest'
    Assert-Stage5ExporterPathAbsent $provenanceFull 'Native fixture provenance'

    $records = @($bundle.records)
    $groups = @($records | Group-Object -Property {
        ([string]$_.destinationSha256).ToUpperInvariant()
    } | Sort-Object Name)
    Assert-Stage5ExporterCondition ($groups.Count -ge 10) `
        'Native fixture conversion requires at least 10 unique replay SHA-256 values.'
    $stressGroups = @($groups | Where-Object {
        @($_.Group | Where-Object { [string]$_.scenario -ceq '4v2' }).Count -gt 0
    })
    Assert-Stage5ExporterCondition ($stressGroups.Count -gt 0) `
        'Native fixture conversion requires a 4v2 replay for the single stress fixture.'
    $stressGroup = @($stressGroups | Sort-Object Name | Select-Object -First 1)[0]
    $stressRecord = @($stressGroup.Group | Where-Object {
        [string]$_.scenario -ceq '4v2'
    } | Sort-Object @{ Expression = { Get-Stage5ExporterSelectionKey $_ } } |
        Select-Object -First 1)[0]
    $normalRecords = @($groups | Where-Object { $_.Name -cne $stressGroup.Name } |
        ForEach-Object {
            @($_.Group | Sort-Object @{
                Expression = { Get-Stage5ExporterSelectionKey $_ }
            } | Select-Object -First 1)
        } | Sort-Object Name)
    Assert-Stage5ExporterCondition ($normalRecords.Count -ge 9) `
        'Native fixture conversion requires nine non-stress unique replay SHA-256 values.'

    $selected = New-Object 'Collections.Generic.List[object]'
    foreach ($record in @($normalRecords | Select-Object -First 9)) {
        $selected.Add([pscustomobject]@{ record = $record; stress = $false }) | Out-Null
    }
    $selected.Add([pscustomobject]@{ record = $stressRecord; stress = $true }) | Out-Null
    Assert-Stage5ExporterCondition ($selected.Count -eq 10) `
        'Native fixture conversion did not select exactly 10 records.'
    Assert-Stage5ExporterCondition (@($selected | Where-Object { $_.stress }).Count -eq 1) `
        'Native fixture conversion did not select exactly one stress record.'

    $fixtureEntries = New-Object 'Collections.Generic.List[object]'
    $richFixtures = New-Object 'Collections.Generic.List[object]'
    $normalIndex = 1
    foreach ($selectedRecord in $selected.ToArray()) {
        $record = $selectedRecord.record
        Assert-Stage5ExporterCondition ([string]$record.category -ceq 'local-capacity-ai') `
            'Native fixture conversion accepts only local-capacity-ai records.'
        Assert-Stage5ExporterCondition ([string]$record.origin -ceq 'native-fresh-runtime') `
            'Native fixture conversion accepts only native-fresh-runtime records.'
        Assert-Stage5ExporterCondition ([string]$record.scenario -ceq '4v2' -or
            [string]$record.scenario -ceq '4v3') `
            'Native fixture conversion found an unsupported AI scenario.'
        Assert-Stage5ExporterCondition ([int]$record.seed -gt 0) `
            'Native fixture conversion found a non-positive AI seed.'
        Assert-Stage5ExporterCondition ([string]$record.sourceSha256 -ceq
            [string]$record.destinationSha256) `
            'Native fixture conversion found a source/destination SHA-256 mismatch.'
        $destinationFull = Assert-Stage5ExporterContainedPathNoReparse $bundle.corpusRoot `
            ([string]$record.destinationPath) 'Native fixture destination path'
        $relative = $destinationFull.Substring($bundle.corpusRoot.Length)
        while ($relative.StartsWith('\') -or $relative.StartsWith('/')) {
            $relative = $relative.Substring(1)
        }
        Assert-Stage5ExporterCondition (-not [IO.Path]::IsPathRooted($relative) -and
            $relative -notmatch '(^|[\\/])\.\.([\\/]|$)') `
            'Native fixture source path must remain relative to CorpusExportRoot.'
        $relative = $relative.Replace('/', '\')
        $id = if ($selectedRecord.stress) {
            'native-stress-4v2'
        }
        else {
            'native-{0:D2}' -f $normalIndex++
        }
        $fixtureEntries.Add([ordered]@{
            id = $id
            source = $relative
            sha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            stress = [bool]$selectedRecord.stress
            maps = @()
        }) | Out-Null
        $rich = [ordered]@{
            id = $id
            source = $relative
            sha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            stress = [bool]$selectedRecord.stress
            category = [string]$record.category
            scenario = [string]$record.scenario
            seed = [int]$record.seed
            runNonce = [string]$record.runNonce
            origin = [string]$record.origin
            title = [string]$record.title
            executableSha256 = ([string]$record.executableSha256).ToUpperInvariant()
            sourceProfileRoot = [string]$record.sourceProfileRoot
            sourcePath = [string]$record.sourcePath
            sourceSha256 = ([string]$record.sourceSha256).ToUpperInvariant()
            destinationPath = $destinationFull
            destinationSha256 = ([string]$record.destinationSha256).ToUpperInvariant()
            length = [Int64]$record.length
            containerMagic = [string]$record.containerMagic
            containerSchemaVersion = [int]$record.containerSchemaVersion
            containerEngineEpoch = [int]$record.containerEngineEpoch
            payloadMagic = [string]$record.payloadMagic
            skirmishAiReplayEpoch = [int]$record.skirmishAiReplayEpoch
            sequence = if ($null -ne $record.PSObject.Properties['sequence']) {
                [int]$record.sequence
            } else { $null }
            configuration = if ($null -ne $record.PSObject.Properties['configuration']) {
                [string]$record.configuration
            } else { $null }
            repeat = if ($null -ne $record.PSObject.Properties['repeat']) {
                [int]$record.repeat
            } else { $null }
            replayEpoch = if ($null -ne $record.PSObject.Properties['replayEpoch']) {
                [int]$record.replayEpoch
            } else { $null }
            replaySha256 = if ($null -ne $record.PSObject.Properties['replaySha256']) {
                ([string]$record.replaySha256).ToUpperInvariant()
            } else { $null }
            exportedUtc = if ($null -ne $record.PSObject.Properties['exportedUtc']) {
                [string]$record.exportedUtc
            } else { $null }
        }
        $richFixtures.Add($rich) | Out-Null
    }
    $fixtureDocument = [ordered]@{
        schemaVersion = 1
        title = [string]$bundle.title
        executable = $Executable
        executableSha256 = [string]$bundle.executableSha256
        fixtures = $fixtureEntries.ToArray()
        ai = [ordered]@{
            seeds = @($selected | ForEach-Object { [int]$_.record.seed } |
                Sort-Object -Unique)
            scenarios = @($selected | ForEach-Object { [string]$_.record.scenario } |
                Sort-Object -Unique)
            repeats = 1
        }
    }
    $fixtureWritten = Write-Stage5ExporterJsonAtomically $fixtureFull $fixtureDocument `
        $bundle.corpusRoot 'Native fixture manifest'
    $provenanceDocument = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-native-replay-fixture-provenance'
        producer = 'Stage5ReplayCorpusExporter'
        producerVersion = '1'
        origin = 'native-fresh-runtime'
        generatedUtc = ([DateTime]::UtcNow).ToString('o')
        title = [string]$bundle.title
        executable = $Executable
        executableSha256 = [string]$bundle.executableSha256
        corpusManifest = [ordered]@{
            path = $bundle.manifestPath
            sha256 = $bundle.manifestSha256
        }
        artifactIndex = [ordered]@{
            path = $bundle.artifactIndex.path
            sha256 = $bundle.artifactIndex.sha256
        }
        validationReceipt = [ordered]@{
            path = $bundle.receipt.path
            sha256 = $bundle.receipt.sha256
        }
        validationResults = [ordered]@{
            path = $bundle.results.path
            sha256 = $bundle.results.sha256
        }
        fixtureManifest = [ordered]@{
            path = $fixtureWritten.path
            sha256 = $fixtureWritten.sha256
        }
        fixtureCount = $fixtureEntries.Count
        stressFixtureCount = @($selected | Where-Object { $_.stress }).Count
        fixtures = $richFixtures.ToArray()
    }
    $provenanceWritten = Write-Stage5ExporterJsonAtomically $provenanceFull `
        $provenanceDocument $bundle.corpusRoot 'Native fixture provenance'
    return [pscustomobject]@{
        status = 'passed'
        fixtureManifest = $fixtureWritten
        provenance = $provenanceWritten
        fixtureCount = $fixtureEntries.Count
        stressFixtureCount = @($selected | Where-Object { $_.stress }).Count
        selectedSha256 = @($fixtureEntries.ToArray() | ForEach-Object { $_.sha256 })
    }
}

Export-ModuleMember -Function Get-Stage5ReplayCompletionFields, `
    Export-Stage5FreshReplayArtifact, Write-Stage5FreshReplayArtifactIndex, `
    Write-Stage5FreshReplayCorpusManifest, Convert-Stage5FreshReplayCorpusManifestToFixtures
