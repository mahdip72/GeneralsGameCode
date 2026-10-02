[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$ExpectedArchiveSha256,
    [Parameter(Mandatory = $true)][string]$TaskRoot,
    [Parameter(Mandatory = $true)][string]$RuntimeRoot,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [string]$SevenZipPath = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$evidenceModulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
if (-not (Test-Path -LiteralPath $evidenceModulePath -PathType Leaf)) {
    throw "The Stage 5 evidence module is required beside this producer: $evidenceModulePath"
}
Import-Module $evidenceModulePath -Force

# This value is deliberately fixed in the reviewed producer.  The caller must
# supply the same value; accepting a caller-selected digest would turn the
# archive hash into an assertion about an unreviewed input.
$script:ReviewedArchiveSha256 =
    '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
$script:ReviewedArchiveObject =
    's3://github-ci/zerohour104_gamedata_trimmed.7z'
$script:ReviewedProducer = 'genci-r2-trimmed-data-v1'
$script:ReviewedEvidenceKind = 'stage5-performance-qualification-data'
$script:RequiredFiles = @(
    'INIZH.big',
    'MapsZH.big',
    'W3DZH.big',
    'Data/Scripts/MultiplayerScripts.scb',
    'Data/Scripts/Scripts.ini',
    'Data/Scripts/SkirmishScripts.scb'
)

function Assert-QualificationCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-QualificationFullPath {
    param([string]$Path, [string]$Context)
    Assert-QualificationCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context must not be empty."
    Assert-QualificationCondition ($Path -notmatch '[\x00-\x1F\x7F]') `
        "$Context contains a control character."
    try { return [IO.Path]::GetFullPath($Path) }
    catch { throw "$Context could not be canonicalized: $($_.Exception.Message)" }
}

function Get-QualificationNormalizedPath {
    param([string]$Path)
    $full = Get-QualificationFullPath $Path 'qualification path'
    if ($full.Length -gt 3) { return $full.TrimEnd([char[]]@('\', '/')) }
    return $full
}

function Assert-QualificationNoDotPath {
    param([string]$Path, [string]$Context)
    Assert-QualificationCondition ($Path -notmatch '(^|[\/])\.{1,2}([\/]|$)') `
        "$Context contains an explicit dot path segment."
    # A colon is valid only as the second character in a rooted drive path.
    $afterDrive = if ($Path -match '^[A-Za-z]:') { $Path.Substring(2) } else { $Path }
    Assert-QualificationCondition ($afterDrive -notmatch ':') `
        "$Context contains an alternate-data-stream or drive-relative separator."
}

function Assert-QualificationHPath {
    param([string]$Path, [string]$Context)
    Assert-QualificationNoDotPath $Path $Context
    $full = Get-QualificationNormalizedPath $Path
    Assert-QualificationCondition ($full -cmatch '^H:\\') `
        "$Context must be an explicit local H: path."
    Assert-QualificationCondition (-not $full.StartsWith('\\',
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must not be a UNC path."
    return $full
}

function Assert-QualificationNoReparsePath {
    param([string]$Path, [string]$Context, [switch]$RequireLeaf)
    $full = Get-QualificationFullPath $Path $Context
    $root = [IO.Path]::GetPathRoot($full)
    Assert-QualificationCondition ($root -is [string] -and
        -not [string]::IsNullOrWhiteSpace($root)) `
        "$Context does not have a volume root."
    $rootCurrent = $root
    foreach ($segment in @($full.Substring($root.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        $rootCurrent = Join-Path $rootCurrent $segment
        if (-not (Test-Path -LiteralPath $rootCurrent)) {
            break
        }
        $item = Get-Item -LiteralPath $rootCurrent -Force -ErrorAction Stop
        Assert-QualificationCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context path component '$segment' is a reparse point."
    }
    if ($RequireLeaf) {
        $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
        Assert-QualificationCondition ($item -is [IO.FileInfo] -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context is not a regular non-reparse file."
    }
}

function Assert-QualificationDirectory {
    param([string]$Path, [string]$Context)
    Assert-QualificationNoReparsePath $Path $Context
    $full = Get-QualificationFullPath $Path $Context
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-QualificationCondition ($item -is [IO.DirectoryInfo] -and
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context must be a regular non-reparse directory."
    return (Get-QualificationNormalizedPath $full)
}

function Assert-QualificationPathBelow {
    param([string]$BasePath, [string]$CandidatePath, [string]$Context)
    $base = Get-QualificationNormalizedPath $BasePath
    $candidate = Get-QualificationFullPath $CandidatePath $Context
    Assert-QualificationCondition ([String]::Equals(
            [IO.Path]::GetPathRoot($base), [IO.Path]::GetPathRoot($candidate),
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context is on a different volume."
    Assert-QualificationCondition ($candidate.StartsWith(
            $base + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context must be contained by the task root."
    return $candidate
}

function Resolve-QualificationSevenZip {
    param([string]$RequestedPath)
    $candidates = New-Object 'Collections.Generic.List[string]'
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        [void]$candidates.Add($RequestedPath)
    }
    else {
        foreach ($candidate in @(
                'C:\Program Files\7-Zip\7z.exe',
                'C:\Program Files (x86)\7-Zip\7z.exe')) {
            [void]$candidates.Add($candidate)
        }
        foreach ($commandName in @('7z.exe', '7za.exe')) {
            $command = Get-Command $commandName -CommandType Application `
                -ErrorAction SilentlyContinue
            foreach ($resolved in @($command)) {
                if ($null -ne $resolved -and $resolved.Source) {
                    [void]$candidates.Add([string]$resolved.Source)
                }
            }
        }
    }
    foreach ($candidate in $candidates) {
        try {
            $full = [IO.Path]::GetFullPath([string]$candidate)
            if ((Test-Path -LiteralPath $full -PathType Leaf) -and
                [IO.Path]::GetFileName($full) -match '^7za?\.exe$') {
                Assert-QualificationNoReparsePath $full '7-Zip executable' -RequireLeaf
                return $full
            }
        }
        catch {
            if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) { throw }
        }
    }
    throw 'A reviewed 7z.exe or 7za.exe executable could not be resolved.'
}

function Invoke-QualificationSevenZip {
    param([string]$Executable, [string[]]$Arguments, [string]$Context)
    $captured = @(& $Executable @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $diagnostic = (($captured | ForEach-Object { [string]$_ }) -join ' ')
        if ($diagnostic.Length -gt 2048) { $diagnostic = $diagnostic.Substring(0, 2048) }
        throw "$Context failed with 7-Zip exit code ${exitCode}: $diagnostic"
    }
    return $captured
}

function Get-QualificationArchiveEntries {
    param([string]$SevenZip, [string]$Archive)
    $lines = Invoke-QualificationSevenZip $SevenZip @(
        'l', '-slt', '-ba', '-sccUTF-8', $Archive
    ) 'Zero Hour qualification archive listing'
    $entries = New-Object 'Collections.Generic.List[object]'
    $current = $null
    foreach ($lineObject in $lines) {
        $line = [string]$lineObject
        if ($line -match '^Path = (.*)$') {
            if ($null -ne $current) { [void]$entries.Add($current) }
            $current = [ordered]@{ Path = $Matches[1] }
            continue
        }
        if ($null -eq $current) { continue }
        $separator = $line.IndexOf(' = ', [StringComparison]::Ordinal)
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 3)
            $current[$name] = $value
        }
    }
    if ($null -ne $current) { [void]$entries.Add($current) }
    Assert-QualificationCondition ($entries.Count -gt 0) `
        'Zero Hour qualification archive listing contains no entries.'
    return $entries.ToArray()
}

function ConvertTo-QualificationArchiveRelativePath {
    param([string]$Path, [string]$Context)
    Assert-QualificationCondition (-not [string]::IsNullOrWhiteSpace($Path)) `
        "$Context path is empty."
    Assert-QualificationCondition ($Path -notmatch '[\x00-\x1F\x7F]') `
        "$Context path contains a control character."
    $normalized = $Path.Replace('\', '/')
    Assert-QualificationCondition ($normalized -notmatch '^[A-Za-z]:') `
        "$Context path contains a drive prefix."
    Assert-QualificationCondition (-not $normalized.StartsWith('/')) `
        "$Context path is rooted."
    $segments = @($normalized -split '/')
    foreach ($segment in $segments) {
        Assert-QualificationCondition (-not [string]::IsNullOrWhiteSpace($segment) -and
            $segment -cne '.' -and $segment -cne '..') `
            "$Context path contains an empty or traversal segment."
    }
    Assert-QualificationCondition ($normalized -notmatch ':') `
        "$Context path contains an alternate-data-stream separator."
    return $normalized
}

function Test-QualificationDataPath {
    param([string]$Path)
    return (($Path.IndexOf('/') -lt 0 -and
            $Path.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)) -or
        $Path.StartsWith('Data/', [StringComparison]::OrdinalIgnoreCase))
}

function Get-QualificationArchiveFileEntries {
    param([object[]]$Entries)
    $seen = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $files = New-Object 'Collections.Generic.List[object]'
    foreach ($entry in $Entries) {
        Assert-QualificationCondition ($entry -is [Collections.IDictionary] -and
            $entry.Contains('Path')) 'Zero Hour qualification archive entry has no path.'
        $relative = ConvertTo-QualificationArchiveRelativePath `
            ([string]$entry['Path']) 'Zero Hour qualification archive entry'
        Assert-QualificationCondition ($seen.Add($relative)) `
            "Zero Hour qualification archive contains a duplicate path: $relative"
        foreach ($linkKey in @('Symbolic Link', 'Hard Link', 'Reparse Point', 'Link')) {
            if ($entry.Contains($linkKey) -and
                -not [string]::IsNullOrWhiteSpace([string]$entry[$linkKey]) -and
                [string]$entry[$linkKey] -cne '-') {
                throw "Zero Hour qualification archive contains a $linkKey entry: $relative"
            }
        }
        if ($entry.Contains('Encrypted') -and [string]$entry['Encrypted'] -ceq '+') {
            throw "Zero Hour qualification archive entry is encrypted: $relative"
        }
        $isDirectory = $entry.Contains('Attributes') -and
            ([string]$entry['Attributes']).IndexOf('D', [StringComparison]::OrdinalIgnoreCase) -ge 0
        if ($isDirectory) {
            Assert-QualificationCondition ($relative -ceq 'Data' -or
                $relative.StartsWith('Data/', [StringComparison]::OrdinalIgnoreCase)) `
                "Zero Hour qualification archive contains an undeclared directory: $relative"
            continue
        }
        Assert-QualificationCondition (Test-QualificationDataPath $relative) `
            "Zero Hour qualification archive contains an undeclared file: $relative"
        $files.Add([pscustomobject]@{ path = $relative }) | Out-Null
    }
    Assert-QualificationCondition ($files.Count -ge $script:RequiredFiles.Count) `
        'Zero Hour qualification archive does not contain enough reviewed data files.'
    return $files.ToArray()
}

function Get-QualificationLockedArchiveSha256 {
    param([IO.FileStream]$Stream)
    $Stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (([BitConverter]::ToString($sha.ComputeHash($Stream)) -replace '-', '')).ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-QualificationFileHashAndLength {
    param([string]$Path, [string]$Context)
    Assert-QualificationNoReparsePath $Path $Context -RequireLeaf
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
        [IO.FileOptions]::SequentialScan)
    try {
        $length = [Int64]$stream.Length
        Assert-QualificationCondition ($length -gt 0) "$Context is empty."
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $buffer = New-Object byte[] 1048576
            while ($true) {
                $read = $stream.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) { break }
                [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
            }
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            $hash = (([BitConverter]::ToString($sha.Hash) -replace '-', '')).ToUpperInvariant()
        }
        finally { $sha.Dispose() }
        Assert-QualificationCondition ($stream.Position -eq $length) `
            "$Context did not reach its immutable end."
        return [pscustomobject]@{ path = [IO.Path]::GetFullPath($Path); sha256 = $hash; length = $length }
    }
    finally { $stream.Dispose() }
}

function Copy-QualificationDataFile {
    param([string]$SourcePath, [string]$DestinationPath, [string]$Context)
    Assert-QualificationNoReparsePath $SourcePath $Context -RequireLeaf
    $source = [IO.FileStream]::new($SourcePath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
        [IO.FileOptions]::SequentialScan)
    $destination = $null
    try {
        Assert-QualificationCondition ($source.Length -gt 0) "$Context source is empty."
        $destination = [IO.FileStream]::new($DestinationPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None, 1048576,
            [IO.FileOptions]::SequentialScan)
        $sourceLength = [Int64]$source.Length
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $buffer = New-Object byte[] 1048576
            [Int64]$readLength = 0
            while ($true) {
                $read = $source.Read($buffer, 0, $buffer.Length)
                if ($read -le 0) { break }
                $destination.Write($buffer, 0, $read)
                [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
                $readLength += [Int64]$read
            }
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            $sourceHash = (([BitConverter]::ToString($sha.Hash) -replace '-', '')).ToUpperInvariant()
            Assert-QualificationCondition ($readLength -eq $sourceLength -and
                $source.Position -eq $sourceLength) `
                "$Context source changed during the immutable copy."
        }
        finally { $sha.Dispose() }
        $destination.Flush($true)
        return [pscustomobject]@{ sha256 = $sourceHash; length = $sourceLength }
    }
    finally {
        if ($null -ne $destination) { $destination.Dispose() }
        $source.Dispose()
    }
}

function Get-QualificationExtractedFiles {
    param([string]$StagingRoot, [object[]]$ArchiveFiles)
    $allowed = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $ArchiveFiles) { $allowed[$entry.path] = $entry }
    $staging = Get-QualificationNormalizedPath $StagingRoot
    Assert-QualificationNoReparsePath $staging 'qualification staging root'
    $actual = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in @(Get-ChildItem -LiteralPath $staging -Recurse -Force -ErrorAction Stop)) {
        Assert-QualificationCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Extracted qualification data contains a reparse point: $($item.FullName)"
        if ($item -isnot [IO.FileInfo]) { continue }
        $full = [IO.Path]::GetFullPath($item.FullName)
        Assert-QualificationPathBelow $staging $full 'extracted qualification file' | Out-Null
        $relative = $full.Substring($staging.Length).TrimStart([char[]]@('\', '/')).Replace('\', '/')
        $relative = ConvertTo-QualificationArchiveRelativePath $relative 'extracted qualification file'
        Assert-QualificationCondition (Test-QualificationDataPath $relative) `
            "Extracted qualification data contains an undeclared file: $relative"
        Assert-QualificationCondition ($allowed.ContainsKey($relative)) `
            "Extracted qualification data contains a file absent from the archive listing: $relative"
        Assert-QualificationCondition (-not $actual.ContainsKey($relative)) `
            "Extracted qualification data contains a duplicate path: $relative"
        $actual[$relative] = [pscustomobject]@{ path = $relative; fullPath = $full }
    }
    Assert-QualificationCondition ($actual.Count -eq $allowed.Count) `
        'Extracted qualification data does not match the safe archive file listing.'
    return @($actual.Values)
}

function Get-QualificationClosureSha256 {
    param([object[]]$Files)
    $lines = @($Files | ForEach-Object {
        '{0}|{1}|{2}' -f $_.path, ([string]$_.sha256).ToUpperInvariant(), [Int64]$_.length
    })
    [Array]::Sort($lines, [StringComparer]::Ordinal)
    $text = ($lines -join "`n") + "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (([BitConverter]::ToString($sha.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '')).ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Write-QualificationManifest {
    param([string]$Path, [object]$Manifest, [string]$TaskRoot)
    Assert-QualificationPathBelow $TaskRoot $Path 'qualification-data OutputPath' | Out-Null
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    Assert-QualificationDirectory $parent 'qualification-data output directory' | Out-Null
    Assert-QualificationCondition (-not (Test-Path -LiteralPath $Path)) `
        "qualification-data OutputPath already exists: $Path"
    $json = ($Manifest | ConvertTo-Json -Depth 8) + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    $stream = $null
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
    try {
        Assert-QualificationNoReparsePath $temporary 'qualification-data temporary manifest' -RequireLeaf
        [IO.File]::Move($temporary, [IO.Path]::GetFullPath($Path))
    }
    catch {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
        throw
    }
    Assert-QualificationNoReparsePath $Path 'qualification-data manifest' -RequireLeaf
    return (Get-QualificationFileHashAndLength $Path 'qualification-data manifest')
}

$archiveFull = Assert-QualificationHPath $ArchivePath 'ArchivePath'
$taskFull = Assert-QualificationHPath $TaskRoot 'TaskRoot'
$runtimeFull = Assert-QualificationHPath $RuntimeRoot 'RuntimeRoot'
$outputFull = Assert-QualificationHPath $OutputPath 'OutputPath'
Assert-QualificationCondition ($ExpectedArchiveSha256 -ceq $script:ReviewedArchiveSha256) `
    'ExpectedArchiveSha256 must exactly equal the fixed reviewed Zero Hour archive SHA-256.'
Assert-QualificationCondition ($ExpectedSourceCommit -cmatch '^[0-9a-f]{40}$' -and
    $ExpectedSourceCommit -ceq $ExpectedSourceCommit.ToLowerInvariant()) `
    'ExpectedSourceCommit must be a canonical lowercase 40-character commit.'
Assert-QualificationDirectory $taskFull 'TaskRoot' | Out-Null
Assert-QualificationDirectory $runtimeFull 'RuntimeRoot' | Out-Null
Assert-QualificationPathBelow $taskFull $outputFull 'OutputPath' | Out-Null
Assert-QualificationCondition (-not (Test-Path -LiteralPath $outputFull)) `
    "OutputPath already exists: $outputFull"
Assert-QualificationDirectory (Split-Path -Parent $outputFull) `
    'OutputPath parent' | Out-Null
Assert-QualificationNoReparsePath $archiveFull 'ArchivePath' -RequireLeaf
Assert-QualificationCondition ([IO.Path]::GetFullPath($archiveFull) -cne
        [IO.Path]::GetFullPath($outputFull)) `
    'ArchivePath and OutputPath must be different files.'
$sevenZipFull = Resolve-QualificationSevenZip $SevenZipPath

$archiveLock = $null
$stagingRoot = Join-Path $taskFull ('qualification-data-staging-' +
    [Guid]::NewGuid().ToString('N'))
$createdDestinationFiles = New-Object 'Collections.Generic.List[string]'
$createdDestinationDirectories = New-Object 'Collections.Generic.List[string]'
$outputCreated = $false
try {
    # FileShare.Read keeps the exact archive bytes immutable for this complete
    # operation.  The lock remains live through archive validation, extraction,
    # source/destination hashing, manifest creation, and final revalidation.
    $archiveLock = [IO.FileStream]::new($archiveFull, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
        [IO.FileOptions]::SequentialScan)
    if (Get-Command Assert-Stage5FinalAcceptanceFileHandlePath `
        -ErrorAction SilentlyContinue) {
        Assert-Stage5FinalAcceptanceFileHandlePath $archiveLock $archiveFull `
            'Zero Hour qualification archive lock' | Out-Null
    }
    $initialArchiveHash = Get-QualificationLockedArchiveSha256 $archiveLock
    Assert-QualificationCondition ($initialArchiveHash -ceq $script:ReviewedArchiveSha256) `
        'The downloaded Zero Hour qualification archive does not match the fixed reviewed SHA-256.'
    $archiveEntries = Get-QualificationArchiveEntries $sevenZipFull $archiveFull
    $archiveFiles = Get-QualificationArchiveFileEntries $archiveEntries
    Invoke-QualificationSevenZip $sevenZipFull @(
        't', '-sccUTF-8', '-y', $archiveFull
    ) 'Zero Hour qualification archive integrity test' | Out-Null

    Assert-QualificationCondition (-not (Test-Path -LiteralPath $stagingRoot)) `
        'Qualification staging directory was not fresh.'
    New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
    Assert-QualificationDirectory $stagingRoot 'qualification staging root' | Out-Null
    Invoke-QualificationSevenZip $sevenZipFull @(
        'x', '-y', '-aoa', '-sccUTF-8', $archiveFull, "-o$stagingRoot"
    ) 'Zero Hour qualification archive extraction' | Out-Null
    $extractedFiles = Get-QualificationExtractedFiles $stagingRoot $archiveFiles
    $sourceRecords = New-Object 'Collections.Generic.List[object]'
    $sourceByPath = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($source in $extractedFiles) {
        $record = Get-QualificationFileHashAndLength $source.fullPath `
            "qualification source '$($source.path)'"
        $sourceRecord = [pscustomobject]@{
            path = [string]$source.path
            fullPath = [string]$source.fullPath
            sha256 = [string]$record.sha256
            length = [Int64]$record.length
        }
        $sourceByPath.Add($sourceRecord.path, $sourceRecord)
        $sourceRecords.Add($sourceRecord) | Out-Null
    }
    foreach ($required in $script:RequiredFiles) {
        Assert-QualificationCondition ($sourceByPath.ContainsKey($required)) `
            "Zero Hour qualification archive omits required file: $required"
    }

    # Only absent data paths are admitted into the existing product runtime.
    # Existing executable/DLL/launcher closure files remain untouched.
    foreach ($sourceRecord in $sourceRecords) {
        $destination = [IO.Path]::GetFullPath((Join-Path $runtimeFull $sourceRecord.path))
        Assert-QualificationPathBelow $runtimeFull $destination `
            "qualification destination '$($sourceRecord.path)'" | Out-Null
        $destinationParent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $destinationParent)) {
            Assert-QualificationNoReparsePath (Split-Path -Parent $destinationParent) `
                'qualification destination parent' | Out-Null
            New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
            $createdDestinationDirectories.Add([IO.Path]::GetFullPath($destinationParent)) | Out-Null
        }
        Assert-QualificationDirectory $destinationParent `
            'qualification destination parent' | Out-Null
        Assert-QualificationCondition (-not (Test-Path -LiteralPath $destination)) `
            "qualification-data destination already exists: $($sourceRecord.path)"
    }
    foreach ($sourceRecord in $sourceRecords) {
        $destination = [IO.Path]::GetFullPath((Join-Path $runtimeFull $sourceRecord.path))
        $copyResult = Copy-QualificationDataFile $sourceRecord.fullPath $destination `
            "qualification destination '$($sourceRecord.path)'"
        Assert-QualificationNoReparsePath $destination `
            "qualification destination '$($sourceRecord.path)'" -RequireLeaf
        Assert-QualificationCondition ($copyResult.sha256 -ceq $sourceRecord.sha256 -and
            $copyResult.length -eq $sourceRecord.length) `
            "qualification destination '$($sourceRecord.path)' failed source binding."
        $createdDestinationFiles.Add($destination) | Out-Null
        $sourceRecord | Add-Member NoteProperty destinationPath $destination
    }

    [string[]]$sortedSourcePaths = @($sourceRecords | ForEach-Object {
        [string]$_.path
    })
    [Array]::Sort($sortedSourcePaths, [StringComparer]::Ordinal)
    $manifestFiles = @($sortedSourcePaths | ForEach-Object {
        $sourceRecord = $sourceByPath[$_]
        [ordered]@{ path = $sourceRecord.path; sha256 = $sourceRecord.sha256; length = [Int64]$sourceRecord.length }
    })
    $closureSha256 = Get-QualificationClosureSha256 $manifestFiles
    $manifest = [ordered]@{
        schemaVersion = 1
        evidenceKind = $script:ReviewedEvidenceKind
        producer = $script:ReviewedProducer
        sourceCommit = $ExpectedSourceCommit
        title = 'ZeroHour'
        archiveSource = [ordered]@{
            object = $script:ReviewedArchiveObject
            sha256 = $script:ReviewedArchiveSha256
        }
        runtimeRoot = $runtimeFull
        files = $manifestFiles
        closureSha256 = $closureSha256
    }
    $manifestBinding = Write-QualificationManifest $outputFull $manifest $taskFull
    $outputCreated = $true

    # Re-read every source and destination after publication while the archive
    # lock is still held.  This closes both the extraction and copy boundaries.
    foreach ($sourceRecord in $sourceRecords) {
        $sourceAfter = Get-QualificationFileHashAndLength $sourceRecord.fullPath `
            "qualification source '$($sourceRecord.path)' final check"
        $destinationAfter = Get-QualificationFileHashAndLength $sourceRecord.destinationPath `
            "qualification destination '$($sourceRecord.path)' final check"
        Assert-QualificationCondition ($sourceAfter.sha256 -ceq $sourceRecord.sha256 -and
            $sourceAfter.length -eq $sourceRecord.length -and
            $destinationAfter.sha256 -ceq $sourceRecord.sha256 -and
            $destinationAfter.length -eq $sourceRecord.length) `
            "qualification source or destination changed during publication: $($sourceRecord.path)"
    }
    $finalArchiveHash = Get-QualificationLockedArchiveSha256 $archiveLock
    Assert-QualificationCondition ($finalArchiveHash -ceq $script:ReviewedArchiveSha256) `
        'The reviewed Zero Hour qualification archive changed during production.'
    $finalManifest = Get-QualificationFileHashAndLength $outputFull `
        'qualification-data manifest final check'
    Assert-QualificationCondition ($finalManifest.sha256 -ceq $manifestBinding.sha256 -and
        $finalManifest.length -eq $manifestBinding.length) `
        'qualification-data manifest changed during publication.'
    if (Test-Path -LiteralPath $stagingRoot) {
        Assert-QualificationNoReparsePath $stagingRoot `
            'qualification staging root cleanup'
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
    Write-Output ("Created Stage 5 performance qualification data: {0}" -f $outputFull)
}
catch {
    foreach ($destination in @($createdDestinationFiles)) {
        if (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($directory in @($createdDestinationDirectories | Sort-Object Length -Descending)) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Force -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($outputCreated -and (Test-Path -LiteralPath $outputFull)) {
        Remove-Item -LiteralPath $outputFull -Force -ErrorAction SilentlyContinue
    }
    throw
}
finally {
    if ($null -ne $archiveLock) { $archiveLock.Dispose() }
}
