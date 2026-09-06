[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RuntimeRoot,
    [Parameter(Mandatory = $true)][string]$TaskRoot,
    [Parameter(Mandatory = $true)][string]$SourceCommit,
    [Parameter(Mandatory = $true)]
    [ValidateSet('Generals', 'ZeroHour')][string]$Title,
    [Parameter(Mandatory = $true)][string]$AwsEndpointUrl,
    [Parameter(Mandatory = $true)][string]$OutputEnvironmentFile
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Stage5SimulationCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Stage5SimulationFullPath {
    param([string]$Path, [string]$Context)
    Assert-Stage5SimulationCondition (
        -not [string]::IsNullOrWhiteSpace($Path) -and
        $Path -notmatch '[\x00-\x1F\x7F]') `
        "$Context must be a nonempty single-line path."
    try { return [IO.Path]::GetFullPath($Path) }
    catch { throw "$Context could not be canonicalized: $($_.Exception.Message)" }
}

function Get-Stage5SimulationNormalizedPath {
    param([string]$Path, [string]$Context)
    $full = Get-Stage5SimulationFullPath $Path $Context
    $root = [IO.Path]::GetPathRoot($full)
    if ($full.Length -gt $root.Length) {
        return $full.TrimEnd([char[]]@('\', '/'))
    }
    return $full
}

function Assert-Stage5SimulationNoReparsePath {
    param(
        [string]$Path,
        [string]$Context,
        [ValidateSet('Any', 'Leaf', 'Container')][string]$PathType = 'Any'
    )
    $full = Get-Stage5SimulationNormalizedPath $Path $Context
    $root = [IO.Path]::GetPathRoot($full)
    Assert-Stage5SimulationCondition (-not [string]::IsNullOrWhiteSpace($root)) `
        "$Context has no volume root."
    $current = $root
    foreach ($segment in @($full.Substring($root.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($segment)) { continue }
        Assert-Stage5SimulationCondition ($segment -cnotmatch '[\x00-\x1F\x7F]' -and
            $segment -cne '.' -and $segment -cne '..' -and
            -not $segment.EndsWith('.') -and -not $segment.EndsWith(' ')) `
            "$Context contains an unsafe path segment."
        $current = Join-Path $current $segment
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        Assert-Stage5SimulationCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context traverses a reparse point: $current"
    }
    if ($PathType -cne 'Any') {
        Assert-Stage5SimulationCondition (Test-Path -LiteralPath $full `
                -PathType $PathType) `
            "$Context is not an existing $($PathType.ToLowerInvariant()): $full"
        $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
        $correctType = if ($PathType -ceq 'Leaf') {
            $item -is [IO.FileInfo]
        }
        else { $item -is [IO.DirectoryInfo] }
        Assert-Stage5SimulationCondition ($correctType -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "$Context is not a regular non-reparse $($PathType.ToLowerInvariant())."
    }
    return $full
}

function Assert-Stage5SimulationPathBelow {
    param([string]$Root, [string]$Path, [string]$Context)
    $rootFull = Get-Stage5SimulationNormalizedPath $Root "$Context root"
    $pathFull = Get-Stage5SimulationFullPath $Path $Context
    Assert-Stage5SimulationCondition ([string]::Equals(
            [IO.Path]::GetPathRoot($rootFull), [IO.Path]::GetPathRoot($pathFull),
            [StringComparison]::OrdinalIgnoreCase) -and
        $pathFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) `
        "$Context escapes its trusted root."
    return $pathFull
}

function Assert-Stage5SimulationSafeRelativePath {
    param([string]$Path, [string]$Context)
    $segments = @($Path -split '[\\/]')
    Assert-Stage5SimulationCondition (
        -not [string]::IsNullOrWhiteSpace($Path) -and
        -not [IO.Path]::IsPathRooted($Path) -and
        $Path -notmatch '[:\x00-\x1F\x7F]' -and
        @($segments | Where-Object {
            [string]::IsNullOrEmpty($_) -or $_ -ceq '.' -or $_ -ceq '..' -or
                $_.EndsWith('.') -or $_.EndsWith(' ')
        }).Count -eq 0) `
        "$Context contains an unsafe archive path: $Path"
    return $Path.Replace('\', '/')
}

function Get-Stage5SimulationStreamSha256 {
    param([IO.Stream]$Stream)
    Assert-Stage5SimulationCondition ($Stream.CanRead -and $Stream.CanSeek) `
        'Stage 5 simulation qualification hashing requires a readable seekable stream.'
    $Stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (([BitConverter]::ToString($sha.ComputeHash($Stream)) -replace '-',
            '')).ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Stage5SimulationFileSha256 {
    param([string]$Path, [string]$Context)
    $full = Assert-Stage5SimulationNoReparsePath $Path $Context -PathType Leaf
    $stream = [IO.File]::Open($full, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { return Get-Stage5SimulationStreamSha256 $stream }
    finally { $stream.Dispose() }
}

function Get-Stage5SimulationTextSha256 {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (([BitConverter]::ToString($sha.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($Text))) -replace '-',
            '')).ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Assert-Stage5FreshSimulationRuntimeData {
    param([string]$RuntimeRoot)
    $runtimeFull = Assert-Stage5SimulationNoReparsePath $RuntimeRoot `
        'Stage 5 simulation runtime root' -PathType Container
    $existingRootBigs = @(Get-ChildItem -LiteralPath $runtimeFull -File -Force `
        -ErrorAction Stop | Where-Object { $_.Extension -ieq '.big' })
    $dataPath = Join-Path $runtimeFull 'Data'
    Assert-Stage5SimulationCondition ($existingRootBigs.Count -eq 0 -and
        -not (Test-Path -LiteralPath $dataPath)) `
        'Stage 5 simulation runtime is not fresh; existing qualification data would trust mutable cache state.'
    return $runtimeFull
}

function Open-Stage5SimulationEnvironmentFile {
    param([string]$Path)
    Assert-Stage5SimulationCondition (-not [string]::IsNullOrWhiteSpace(
            $env:GITHUB_ENV)) `
        'GITHUB_ENV is unavailable to the Stage 5 simulation producer.'
    $requested = Assert-Stage5SimulationNoReparsePath $Path `
        'Stage 5 simulation output environment file' -PathType Leaf
    $expected = Assert-Stage5SimulationNoReparsePath $env:GITHUB_ENV `
        'GITHUB_ENV' -PathType Leaf
    Assert-Stage5SimulationCondition ([string]::Equals($requested, $expected,
            [StringComparison]::OrdinalIgnoreCase)) `
        'OutputEnvironmentFile must resolve to the exact existing GITHUB_ENV file.'
    return [IO.File]::Open($requested, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
}

function Write-Stage5SimulationEnvironmentBindings {
    param([IO.FileStream]$Stream, [Collections.IDictionary]$Bindings)
    $prefix = ''
    if ($Stream.Length -gt 0) {
        $Stream.Position = $Stream.Length - 1
        $last = $Stream.ReadByte()
        if ($last -ne 10 -and $last -ne 13) { $prefix = "`n" }
    }
    $builder = [Text.StringBuilder]::new($prefix)
    foreach ($key in $Bindings.Keys) {
        $name = [string]$key
        $value = [string]$Bindings[$key]
        Assert-Stage5SimulationCondition ($name -cmatch
                '^STAGE5_SIMULATION_QUALIFICATION_DATA_[A-Z0-9_]+$' -and
            $value -notmatch '[\x00-\x1F\x7F]') `
            'Stage 5 simulation environment bindings must be canonical single-line values.'
        [void]$builder.Append($name).Append('=').Append($value).Append("`n")
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($builder.ToString())
    $Stream.Position = $Stream.Length
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush($true)
}

function Resolve-Stage5SimulationApplication {
    param([string[]]$Names, [string]$Context)
    foreach ($name in $Names) {
        foreach ($command in @(Get-Command $name -CommandType Application `
                -ErrorAction SilentlyContinue)) {
            if ($null -ne $command -and
                -not [string]::IsNullOrWhiteSpace([string]$command.Source)) {
                return Assert-Stage5SimulationNoReparsePath $command.Source `
                    $Context -PathType Leaf
            }
        }
    }
    throw "$Context could not be resolved."
}

function Copy-Stage5SimulationQualificationFile {
    param([string]$Source, [string]$Destination, [string]$RuntimeRoot)
    $sourceFull = Assert-Stage5SimulationNoReparsePath $Source `
        'Extracted Stage 5 simulation qualification file' -PathType Leaf
    $destinationFull = Assert-Stage5SimulationPathBelow $RuntimeRoot $Destination `
        'Stage 5 simulation qualification destination'
    Assert-Stage5SimulationCondition (-not (Test-Path -LiteralPath $destinationFull)) `
        "Stage 5 simulation qualification destination already exists: $destinationFull"
    $parent = Split-Path -Parent $destinationFull
    if (-not (Test-Path -LiteralPath $parent)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [void](Assert-Stage5SimulationNoReparsePath $parent `
        'Stage 5 simulation qualification destination parent' -PathType Container)

    $sourceStream = $null
    $destinationStream = $null
    try {
        $sourceStream = [IO.File]::Open($sourceFull, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        Assert-Stage5SimulationCondition ($sourceStream.Length -gt 0) `
            "Stage 5 simulation qualification source is empty: $sourceFull"
        $sourceSha256 = Get-Stage5SimulationStreamSha256 $sourceStream
        $sourceStream.Position = 0
        $destinationStream = [IO.File]::Open($destinationFull,
            [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        $sourceStream.CopyTo($destinationStream)
        $destinationStream.Flush($true)
        $destinationStream.Dispose()
        $destinationStream = $null
        Assert-Stage5SimulationCondition (
            (Get-Stage5SimulationStreamSha256 $sourceStream) -ceq $sourceSha256) `
            "Stage 5 simulation qualification source changed while copied: $sourceFull"
    }
    finally {
        if ($null -ne $destinationStream) { $destinationStream.Dispose() }
        if ($null -ne $sourceStream) { $sourceStream.Dispose() }
    }
    $destinationSha256 = Get-Stage5SimulationFileSha256 $destinationFull `
        'Staged Stage 5 simulation qualification file'
    Assert-Stage5SimulationCondition ($destinationSha256 -ceq $sourceSha256) `
        "Stage 5 simulation qualification copy SHA-256 mismatch: $destinationFull"
    return $destinationSha256
}

function Write-Stage5SimulationManifest {
    param([string]$Path, [object]$Document, [string]$TaskRoot)
    $full = Assert-Stage5SimulationPathBelow $TaskRoot $Path `
        'Stage 5 simulation qualification manifest'
    Assert-Stage5SimulationCondition (-not (Test-Path -LiteralPath $full)) `
        "Stage 5 simulation qualification manifest already exists: $full"
    $parent = Assert-Stage5SimulationNoReparsePath (Split-Path -Parent $full) `
        'Stage 5 simulation qualification manifest parent' -PathType Container
    $temporary = Join-Path $parent ('.qualification-' +
        [Guid]::NewGuid().ToString('N') + '.tmp')
    $stream = $null
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes(
            ($Document | ConvertTo-Json -Depth 8))
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        [IO.File]::Move($temporary, $full)
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    [void](Assert-Stage5SimulationNoReparsePath $full `
        'Stage 5 simulation qualification manifest' -PathType Leaf)
    return $full
}

function Remove-Stage5SimulationStagingTree {
    param([string]$Path, [string]$TaskRoot)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $full = Assert-Stage5SimulationPathBelow $TaskRoot $Path `
        'Stage 5 simulation qualification staging cleanup'
    [void](Assert-Stage5SimulationNoReparsePath $full `
        'Stage 5 simulation qualification staging cleanup' -PathType Container)
    foreach ($item in @(Get-ChildItem -LiteralPath $full -Recurse -Force `
            -ErrorAction Stop)) {
        Assert-Stage5SimulationCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Refusing to recursively remove a qualification staging tree containing a reparse point: $($item.FullName)"
        [void](Assert-Stage5SimulationPathBelow $full $item.FullName `
            'Stage 5 simulation qualification staging cleanup item')
    }
    Remove-Item -LiteralPath $full -Recurse -Force
    Assert-Stage5SimulationCondition (-not (Test-Path -LiteralPath $full)) `
        'Stage 5 simulation qualification staging bytes were not removed.'
}

Assert-Stage5SimulationCondition ($SourceCommit -cmatch '^[0-9a-f]{40}$') `
    'SourceCommit must be a canonical lowercase 40-character commit.'
$endpoint = $null
Assert-Stage5SimulationCondition ([Uri]::TryCreate($AwsEndpointUrl,
        [UriKind]::Absolute, [ref]$endpoint) -and
    $endpoint.Scheme -ceq 'https' -and
    [string]::IsNullOrEmpty($endpoint.UserInfo) -and
    [string]::IsNullOrEmpty($endpoint.Query) -and
    [string]::IsNullOrEmpty($endpoint.Fragment)) `
    'AwsEndpointUrl must be an absolute HTTPS endpoint without credentials, query, or fragment.'
foreach ($secretName in @('AWS_ACCESS_KEY_ID', 'AWS_SECRET_ACCESS_KEY')) {
    Assert-Stage5SimulationCondition (-not [string]::IsNullOrWhiteSpace(
            [Environment]::GetEnvironmentVariable($secretName))) `
        "Stage 5 simulation qualification requires secret $secretName."
}

$runtimeFull = Assert-Stage5FreshSimulationRuntimeData $RuntimeRoot
$taskFull = Get-Stage5SimulationNormalizedPath $TaskRoot `
    'Stage 5 simulation task root'
Assert-Stage5SimulationCondition ($taskFull -ceq
        'H:\Stage5SimulationValidationTask') `
    'TaskRoot must be the canonical H:\Stage5SimulationValidationTask path.'
Assert-Stage5SimulationCondition (-not (Test-Path -LiteralPath $taskFull)) `
    'Stage 5 simulation task root must be fresh.'
$awsExecutable = Resolve-Stage5SimulationApplication @('aws.exe', 'aws') `
    'AWS CLI executable'
$sevenZipExecutable = Resolve-Stage5SimulationApplication @('7z.exe', '7za.exe',
    '7z', '7za') '7-Zip executable'

$specification = if ($Title -ceq 'Generals') {
    [pscustomobject]@{
        object = 's3://github-ci/generals108_gamedata_trimmed.7z'
        archive = 'generals108_gamedata_trimmed.7z'
        expectedSha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        required = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
    }
}
else {
    [pscustomobject]@{
        object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
        archive = 'zerohour104_gamedata_trimmed.7z'
        expectedSha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        required = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
}

$stagingRoot = Assert-Stage5SimulationPathBelow $taskFull `
    (Join-Path $taskFull 'QualificationDataStaging') `
    'Stage 5 simulation qualification staging root'
$archivePath = Assert-Stage5SimulationPathBelow $stagingRoot `
    (Join-Path $stagingRoot $specification.archive) `
    'Reviewed Stage 5 simulation archive'
$extractRoot = Assert-Stage5SimulationPathBelow $stagingRoot `
    (Join-Path $stagingRoot 'Extracted') `
    'Stage 5 simulation extraction root'
$manifestPath = Assert-Stage5SimulationPathBelow $taskFull `
    (Join-Path $taskFull 'QualificationData.json') `
    'Stage 5 simulation qualification manifest'
$createdRuntimeFiles = New-Object 'Collections.Generic.List[string]'
$archiveLock = $null
$manifestLock = $null
$environmentStream = $null
$completed = $false
try {
    [IO.Directory]::CreateDirectory($taskFull) | Out-Null
    $taskFull = Assert-Stage5SimulationNoReparsePath $taskFull `
        'Stage 5 simulation task root' -PathType Container
    $environmentStream = Open-Stage5SimulationEnvironmentFile `
        $OutputEnvironmentFile
    [IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    [IO.Directory]::CreateDirectory($extractRoot) | Out-Null
    [void](Assert-Stage5SimulationNoReparsePath $stagingRoot `
        'Stage 5 simulation qualification staging root' -PathType Container)
    [void](Assert-Stage5SimulationNoReparsePath $extractRoot `
        'Stage 5 simulation extraction root' -PathType Container)

    & $awsExecutable s3 cp $specification.object $archivePath `
        --endpoint-url $endpoint.AbsoluteUri
    Assert-Stage5SimulationCondition ($LASTEXITCODE -eq 0 -and
        (Test-Path -LiteralPath $archivePath -PathType Leaf)) `
        "Could not download reviewed $Title simulation qualification data."
    $archivePath = Assert-Stage5SimulationNoReparsePath $archivePath `
        'Reviewed Stage 5 simulation archive' -PathType Leaf

    # FileShare.Read permits 7-Zip to consume these exact bytes while denying
    # writers and replacement. The handle stays live through extraction,
    # runtime staging, manifest publication, and the final archive rehash.
    $archiveLock = [IO.FileStream]::new($archivePath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read, 1048576,
        [IO.FileOptions]::SequentialScan)
    $initialArchiveSha256 = Get-Stage5SimulationStreamSha256 $archiveLock
    Assert-Stage5SimulationCondition ($initialArchiveSha256 -ceq
        [string]$specification.expectedSha256) `
        "Reviewed $Title simulation qualification archive SHA-256 mismatch."

    $listingLines = @(& $sevenZipExecutable l -slt -ba $archivePath)
    Assert-Stage5SimulationCondition ($LASTEXITCODE -eq 0 -and
        $listingLines.Count -gt 0) `
        "Could not inspect reviewed $Title simulation qualification data."
    Assert-Stage5SimulationCondition (@($listingLines | Where-Object {
            $_ -match '^(Symbolic Link|Hard Link|Alternate Stream)\s*=' -or
            $_ -match '^Attributes\s*=.*[lL]' -or
            $_ -match '^Encrypted\s*=\s*\+'
        }).Count -eq 0) `
        "Reviewed $Title simulation qualification archive contains link, alternate-stream, or encrypted metadata."
    $listedPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $listingLines) {
        if ($line -notmatch '^Path\s*=\s*(?<path>.*)$') { continue }
        $normalized = Assert-Stage5SimulationSafeRelativePath `
            ([string]$Matches.path) "Reviewed $Title simulation archive"
        Assert-Stage5SimulationCondition ($listedPaths.Add($normalized)) `
            "Reviewed $Title simulation qualification archive repeats path '$normalized'."
        [void](Assert-Stage5SimulationPathBelow $extractRoot `
            (Join-Path $extractRoot $normalized) `
            'Reviewed Stage 5 simulation archive entry')
    }
    Assert-Stage5SimulationCondition ($listedPaths.Count -gt 0) `
        "Reviewed $Title simulation qualification archive has no entries."
    & $sevenZipExecutable t -sccUTF-8 -y $archivePath
    Assert-Stage5SimulationCondition ($LASTEXITCODE -eq 0) `
        "Reviewed $Title simulation qualification archive failed its integrity test."
    & $sevenZipExecutable x -sccUTF-8 -y -aoa $archivePath "-o$extractRoot"
    Assert-Stage5SimulationCondition ($LASTEXITCODE -eq 0) `
        "Could not extract reviewed $Title simulation qualification data."

    $selected = New-Object 'Collections.Generic.List[object]'
    foreach ($item in @(Get-ChildItem -LiteralPath $extractRoot -Recurse -Force `
            -ErrorAction Stop)) {
        Assert-Stage5SimulationCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Extracted $Title simulation qualification data contains a reparse point."
        if ($item -isnot [IO.FileInfo]) { continue }
        $sourceFull = Assert-Stage5SimulationNoReparsePath $item.FullName `
            'Extracted Stage 5 simulation qualification file' -PathType Leaf
        [void](Assert-Stage5SimulationPathBelow $extractRoot $sourceFull `
            'Extracted Stage 5 simulation qualification file')
        $relative = $sourceFull.Substring($extractRoot.Length).TrimStart(
            [char[]]@('\', '/')).Replace('\', '/')
        $relative = Assert-Stage5SimulationSafeRelativePath $relative `
            'Extracted Stage 5 simulation qualification file'
        $isRootBig = $relative.IndexOf('/') -lt 0 -and
            $relative.EndsWith('.big', [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $relative.StartsWith('Data/',
            [StringComparison]::OrdinalIgnoreCase)
        if ($isRootBig -or $isDataFile) {
            $selected.Add([pscustomobject]@{
                relative = $relative
                source = $sourceFull
            }) | Out-Null
        }
    }
    $selectedByPath = New-Object `
        'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $selected) {
        Assert-Stage5SimulationCondition (-not $selectedByPath.ContainsKey(
                [string]$file.relative)) `
            "Reviewed $Title simulation qualification data repeats '$($file.relative)'."
        $selectedByPath.Add([string]$file.relative, $file)
    }
    foreach ($required in $specification.required) {
        Assert-Stage5SimulationCondition ($selectedByPath.ContainsKey($required)) `
            "Reviewed $Title simulation qualification data omits required file '$required'."
    }
    Assert-Stage5SimulationCondition ($selectedByPath.Count -eq
        $specification.required.Count) `
        "Reviewed $Title simulation qualification data has an unexpected BIG/Data membership."

    [string[]]$relativePaths = @($selectedByPath.Keys)
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)
    $manifestFiles = New-Object 'Collections.Generic.List[object]'
    foreach ($relative in $relativePaths) {
        $destination = Assert-Stage5SimulationPathBelow $runtimeFull `
            (Join-Path $runtimeFull $relative) `
            'Stage 5 simulation qualification runtime destination'
        $sha256 = Copy-Stage5SimulationQualificationFile `
            $selectedByPath[$relative].source $destination $runtimeFull
        $createdRuntimeFiles.Add($destination) | Out-Null
        $manifestFiles.Add([ordered]@{
            path = $relative
            sha256 = $sha256
        }) | Out-Null
    }
    $canonicalText = (@($manifestFiles.ToArray() | ForEach-Object {
        '{0}|{1}' -f $_.path, $_.sha256
    }) -join "`n") + "`n"
    $closureSha256 = Get-Stage5SimulationTextSha256 $canonicalText
    $manifest = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-simulation-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        title = $Title
        archiveSource = [ordered]@{
            object = $specification.object
            sha256 = $specification.expectedSha256
        }
        files = $manifestFiles.ToArray()
        closureSha256 = $closureSha256
    }
    $manifestPath = Write-Stage5SimulationManifest $manifestPath $manifest $taskFull
    $manifestLock = [IO.File]::Open($manifestPath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $manifestSha256 = Get-Stage5SimulationStreamSha256 $manifestLock

    foreach ($entry in $manifestFiles) {
        $destinationPath = Join-Path $runtimeFull ([string]$entry.path)
        Assert-Stage5SimulationCondition (
            (Get-Stage5SimulationFileSha256 $destinationPath `
                "Staged $Title simulation qualification file") -ceq
                [string]$entry.sha256) `
            "Staged $Title simulation qualification data changed before publication."
    }
    $finalArchiveSha256 = Get-Stage5SimulationStreamSha256 $archiveLock
    Assert-Stage5SimulationCondition ($finalArchiveSha256 -ceq
        $initialArchiveSha256) `
        "Reviewed $Title simulation qualification archive changed during provisioning."
    Assert-Stage5SimulationCondition (
        (Get-Stage5SimulationStreamSha256 $manifestLock) -ceq $manifestSha256) `
        'Stage 5 simulation qualification manifest changed during publication.'

    $manifestLock.Dispose()
    $manifestLock = $null
    $archiveLock.Dispose()
    $archiveLock = $null
    Remove-Stage5SimulationStagingTree $stagingRoot $taskFull

    Write-Stage5SimulationEnvironmentBindings $environmentStream ([ordered]@{
        STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH = $manifestPath
        STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256 = $manifestSha256
        STAGE5_SIMULATION_QUALIFICATION_DATA_CLOSURE_SHA256 = $closureSha256
        STAGE5_SIMULATION_QUALIFICATION_DATA_FILE_COUNT = $manifestFiles.Count
    })
    $completed = $true
}
finally {
    if ($null -ne $manifestLock) { $manifestLock.Dispose() }
    if ($null -ne $archiveLock) { $archiveLock.Dispose() }
    if ($null -ne $environmentStream) { $environmentStream.Dispose() }
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Stage5SimulationStagingTree $stagingRoot $taskFull
    }
    if (-not $completed) {
        foreach ($createdFile in @($createdRuntimeFiles)) {
            if (Test-Path -LiteralPath $createdFile) {
                Remove-Item -LiteralPath $createdFile -Force `
                    -ErrorAction SilentlyContinue
            }
        }
        if (Test-Path -LiteralPath $manifestPath) {
            Remove-Item -LiteralPath $manifestPath -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

Write-Output ("Provisioned immutable Stage 5 simulation qualification data for {0}." -f $Title)
