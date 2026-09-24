[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$QualificationRoot,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$SourceCommit,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$MapName,

    [Parameter(Mandatory = $true)]
    [uint32]$GeneralsMapCrc,

    [Parameter(Mandatory = $true)]
    [uint32]$ZeroHourMapCrc,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$AwsEndpointUrl,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputEnvironmentFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Stage5FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        $Path.IndexOfAny([char[]]@([char]0, [char]10, [char]13)) -ge 0) {
        throw 'Stage 5 provisioning paths must be nonempty single-line paths.'
    }

    $full = [IO.Path]::GetFullPath($Path)
    $pathRoot = [IO.Path]::GetPathRoot($full)
    if ([string]::Equals($full, $pathRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $full
    }
    return $full.TrimEnd('\', '/')
}

function Assert-Stage5ExistingRegularPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Leaf', 'Container')]
        [string]$PathType,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $full = Get-Stage5FullPath -Path $Path
    if (-not (Test-Path -LiteralPath $full -PathType $PathType)) {
        throw "$Description is not an existing $($PathType.ToLowerInvariant()): $full"
    }

    $cursor = $full
    while ($null -ne $cursor) {
        $item = Get-Item -LiteralPath $cursor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Description traverses a reparse point: $cursor"
        }
        $parent = [IO.Directory]::GetParent($cursor)
        if ($null -eq $parent) {
            break
        }
        $parentFull = Get-Stage5FullPath -Path $parent.FullName
        if ([string]::Equals($parentFull, $cursor,
                [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $cursor = $parentFull
    }

    return $full
}

function Assert-Stage5PathUnderRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $rootFull = Get-Stage5FullPath -Path $Root
    $pathFull = Get-Stage5FullPath -Path $Path
    if (-not $pathFull.StartsWith($rootFull + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description escapes its trusted Stage 5 root: $pathFull"
    }
    return $pathFull
}

function Get-Stage5StreamSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [IO.Stream]$Stream
    )

    if (-not $Stream.CanRead -or -not $Stream.CanSeek) {
        throw 'Stage 5 hashing requires a readable, seekable stream.'
    }
    $Stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Stream)) -replace '-', '')
    }
    finally {
        $sha.Dispose()
    }
}

function Get-Stage5TextSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Text
    )

    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '')
    }
    finally {
        $sha.Dispose()
    }
}

function Write-Stage5NewUtf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Text
    )

    $full = Get-Stage5FullPath -Path $Path
    if (Test-Path -LiteralPath $full) {
        throw "Stage 5 manifest output already exists: $full"
    }
    $parent = Assert-Stage5ExistingRegularPath -Path (Split-Path -Parent $full) `
        -PathType Container -Description 'Stage 5 manifest parent'
    [void](Assert-Stage5PathUnderRoot -Root $script:QualificationRootFull `
        -Path $full -Description 'Stage 5 manifest output')

    $temporary = Join-Path $parent ('.stage5-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $stream = $null
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        Move-Item -LiteralPath $temporary -Destination $full
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Open-Stage5OutputEnvironmentFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
        throw 'GITHUB_ENV is not available to the Stage 5 producer.'
    }
    $requested = Assert-Stage5ExistingRegularPath -Path $Path -PathType Leaf `
        -Description 'Stage 5 output environment file'
    $githubEnvironment = Assert-Stage5ExistingRegularPath -Path $env:GITHUB_ENV `
        -PathType Leaf -Description 'GITHUB_ENV'
    if (-not [string]::Equals($requested, $githubEnvironment,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputEnvironmentFile must resolve to the exact existing GITHUB_ENV file.'
    }
    return [IO.File]::Open($requested, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
}

function Write-Stage5EnvironmentBindings {
    param(
        [Parameter(Mandatory = $true)]
        [IO.FileStream]$Stream,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Bindings
    )

    $prefix = ''
    if ($Stream.Length -gt 0) {
        $Stream.Position = $Stream.Length - 1
        $lastByte = $Stream.ReadByte()
        if ($lastByte -ne 10 -and $lastByte -ne 13) {
            $prefix = "`n"
        }
    }
    $builder = [Text.StringBuilder]::new($prefix)
    foreach ($key in $Bindings.Keys) {
        $name = [string]$key
        $value = [string]$Bindings[$key]
        if ($name -cnotmatch '^STAGE5_LOCKSTEP_V2_[A-Z0-9_]+$' -or
            $value.IndexOfAny([char[]]@([char]0, [char]10, [char]13)) -ge 0) {
            throw 'Stage 5 output environment bindings must be single-line lockstep-v2 bindings.'
        }
        [void]$builder.Append($name).Append('=').Append($value).Append("`n")
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($builder.ToString())
    $Stream.Position = $Stream.Length
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush($true)
}

function Copy-Stage5LockedFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $sourceFull = Assert-Stage5ExistingRegularPath -Path $Source -PathType Leaf `
        -Description 'Extracted qualification-data source'
    $destinationFull = Assert-Stage5PathUnderRoot -Root $script:QualificationRootFull `
        -Path $Destination -Description 'Qualification-data destination'
    if (Test-Path -LiteralPath $destinationFull) {
        throw "Qualification data would replace an immutable product-runtime path: $destinationFull"
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destinationFull)) | Out-Null
    [void](Assert-Stage5ExistingRegularPath -Path (Split-Path -Parent $destinationFull) `
        -PathType Container -Description 'Qualification-data destination parent')

    $sourceStream = $null
    $destinationStream = $null
    try {
        $sourceStream = [IO.File]::Open($sourceFull, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $sourceSha256 = Get-Stage5StreamSha256 -Stream $sourceStream
        $sourceStream.Position = 0
        $destinationStream = [IO.File]::Open($destinationFull,
            [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $sourceStream.CopyTo($destinationStream)
        $destinationStream.Flush($true)
        $destinationStream.Dispose()
        $destinationStream = $null
        if ((Get-Stage5StreamSha256 -Stream $sourceStream) -cne $sourceSha256) {
            throw "Qualification-data source changed while it was copied: $sourceFull"
        }
    }
    finally {
        if ($null -ne $destinationStream) {
            $destinationStream.Dispose()
        }
        if ($null -ne $sourceStream) {
            $sourceStream.Dispose()
        }
    }

    $destinationStream = [IO.File]::Open($destinationFull, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $destinationSha256 = Get-Stage5StreamSha256 -Stream $destinationStream
    }
    finally {
        $destinationStream.Dispose()
    }
    if ($destinationSha256 -cne $sourceSha256) {
        throw "Qualification data copy hash mismatch: $destinationFull"
    }
    return $destinationSha256
}

$script:QualificationRootFull = Get-Stage5FullPath -Path $QualificationRoot
if ($script:QualificationRootFull -cne 'H:\Stage5WeeklyPromotionQualification') {
    throw 'Stage 5 lockstep-v2 qualification data root is not canonical.'
}
$script:QualificationRootFull = Assert-Stage5ExistingRegularPath `
    -Path $script:QualificationRootFull -PathType Container `
    -Description 'Stage 5 qualification root'

if ([string]::IsNullOrWhiteSpace($MapName) -or
    $MapName -notmatch '\.map$' -or
    $MapName -match '(^[\\/]|:|\.\.|[;"]|[\x00-\x1F\x7F]|[^\S ])' -or
    $GeneralsMapCrc -eq 0 -or $ZeroHourMapCrc -eq 0) {
    throw 'Reviewed map name and both title-specific CRCs are required before qualification data is staged.'
}

$endpoint = $null
if (-not [Uri]::TryCreate($AwsEndpointUrl, [UriKind]::Absolute, [ref]$endpoint) -or
    $endpoint.Scheme -cne 'https' -or -not [string]::IsNullOrEmpty($endpoint.UserInfo) -or
    -not [string]::IsNullOrEmpty($endpoint.Query) -or
    -not [string]::IsNullOrEmpty($endpoint.Fragment)) {
    throw 'AwsEndpointUrl must be an absolute HTTPS endpoint without credentials, query, or fragment.'
}
foreach ($secretName in @('AWS_ACCESS_KEY_ID', 'AWS_SECRET_ACCESS_KEY')) {
    if ([string]::IsNullOrWhiteSpace(
            [Environment]::GetEnvironmentVariable($secretName))) {
        throw "Manual lockstep-v2 qualification requires secret $secretName."
    }
}

$awsCommands = @(Get-Command aws -CommandType Application -ErrorAction Stop)
$sevenZipCommands = @(Get-Command 7z -CommandType Application -ErrorAction Stop)
if ($awsCommands.Count -lt 1 -or $sevenZipCommands.Count -lt 1) {
    throw 'Stage 5 qualification data requires the AWS CLI and 7-Zip executables.'
}
$awsExecutable = [string]$awsCommands[0].Source
$sevenZipExecutable = [string]$sevenZipCommands[0].Source

$runtimeRoots = [ordered]@{
    Generals = Assert-Stage5ExistingRegularPath `
        -Path (Join-Path $script:QualificationRootFull 'GeneralsRuntime') `
        -PathType Container -Description 'Downloaded Generals runtime'
    ZeroHour = Assert-Stage5ExistingRegularPath `
        -Path (Join-Path $script:QualificationRootFull 'ZeroHourRuntime') `
        -PathType Container -Description 'Downloaded Zero Hour runtime'
}
foreach ($runtimeRoot in $runtimeRoots.Values) {
    [void](Assert-Stage5PathUnderRoot -Root $script:QualificationRootFull `
        -Path $runtimeRoot -Description 'Downloaded product runtime')
    foreach ($item in @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Force)) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Downloaded product runtime contains a reparse point: $($item.FullName)"
        }
    }
}

$dataRoot = Assert-Stage5PathUnderRoot -Root $script:QualificationRootFull `
    -Path (Join-Path $script:QualificationRootFull 'QualificationData') `
    -Description 'Qualification data root'
if (Test-Path -LiteralPath $dataRoot) {
    throw "Qualification data root is not fresh: $dataRoot"
}
[IO.Directory]::CreateDirectory($dataRoot) | Out-Null
$dataRoot = Assert-Stage5ExistingRegularPath -Path $dataRoot -PathType Container `
    -Description 'Qualification data root'

# These content-addressed objects are the only reviewed data inputs accepted by
# this producer. They are intentionally not command-line parameters.
$specifications = @(
    [pscustomobject]@{
        title = 'Generals'
        object = 's3://github-ci/generals108_gamedata_trimmed.7z'
        archive = 'generals108_gamedata_trimmed.7z'
        expectedSha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        runtimeLeaf = 'GeneralsRuntime'
        required = @(
            'English.big',
            'INI.big',
            'Maps.big',
            'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb'
        )
    },
    [pscustomobject]@{
        title = 'ZeroHour'
        object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
        archive = 'zerohour104_gamedata_trimmed.7z'
        expectedSha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        runtimeLeaf = 'ZeroHourRuntime'
        required = @(
            'INIZH.big',
            'MapsZH.big',
            'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb'
        )
    }
)

$environmentStream = $null
$manifestStream = $null
$archiveRecords = New-Object 'Collections.Generic.List[object]'
$archiveEntries = New-Object 'Collections.Generic.List[object]'
$dataEntries = New-Object 'Collections.Generic.List[object]'
$completed = $false
try {
    $environmentStream = Open-Stage5OutputEnvironmentFile -Path $OutputEnvironmentFile

    # Download and lock both reviewed archives before any archive is extracted.
    # FileShare.Read permits 7-Zip to read but prevents writers and deletion until
    # the final manifest has been produced and each archive has been rehashed.
    foreach ($specification in $specifications) {
        $archivePath = Assert-Stage5PathUnderRoot -Root $dataRoot `
            -Path (Join-Path $dataRoot $specification.archive) `
            -Description 'Reviewed qualification archive'
        if (Test-Path -LiteralPath $archivePath) {
            throw "Reviewed qualification archive destination is not fresh: $archivePath"
        }
        & $awsExecutable s3 cp $specification.object $archivePath `
            --endpoint-url $endpoint.AbsoluteUri
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
            throw "Could not download reviewed $($specification.title) qualification data."
        }
        $archivePath = Assert-Stage5ExistingRegularPath -Path $archivePath `
            -PathType Leaf -Description 'Reviewed qualification archive'
        $archiveStream = [IO.File]::Open($archivePath, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            $archiveSha256 = Get-Stage5StreamSha256 -Stream $archiveStream
            if ($archiveSha256 -cne [string]$specification.expectedSha256) {
                throw "Reviewed $($specification.title) qualification archive hash mismatch."
            }
            $archiveRecords.Add([pscustomobject]@{
                specification = $specification
                path = $archivePath
                sha256 = $archiveSha256
                stream = $archiveStream
            }) | Out-Null
            $archiveStream = $null
            $archiveEntries.Add([ordered]@{
                title = $specification.title
                object = $specification.object
                sha256 = $archiveSha256
            }) | Out-Null
        }
        finally {
            if ($null -ne $archiveStream) {
                $archiveStream.Dispose()
            }
        }
    }

    foreach ($archiveRecord in $archiveRecords) {
        $specification = $archiveRecord.specification
        $extractRoot = Assert-Stage5PathUnderRoot -Root $dataRoot `
            -Path (Join-Path $dataRoot ('Extracted-' + $specification.title)) `
            -Description 'Qualification archive extraction root'
        if (Test-Path -LiteralPath $extractRoot) {
            throw "Qualification archive extraction root is not fresh: $extractRoot"
        }
        [IO.Directory]::CreateDirectory($extractRoot) | Out-Null
        $extractRoot = Assert-Stage5ExistingRegularPath -Path $extractRoot `
            -PathType Container -Description 'Qualification archive extraction root'

        $listingLines = @(& $sevenZipExecutable l -slt -ba $archiveRecord.path)
        if ($LASTEXITCODE -ne 0) {
            throw "Could not inspect reviewed $($specification.title) qualification data."
        }
        if (@($listingLines | Where-Object {
                $_ -match '^(Symbolic Link|Hard Link|Alternate Stream)\s*=' -or
                $_ -match '^Attributes\s*=\s*[lL]'
            }).Count -ne 0) {
            throw "Reviewed $($specification.title) qualification archive contains link or alternate-stream metadata."
        }
        $listedPaths = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        foreach ($listingLine in $listingLines) {
            if ($listingLine -notmatch '^Path\s*=\s*(?<path>.*)$') {
                continue
            }
            $archiveRelative = [string]$Matches.path
            $segments = @($archiveRelative -split '[\\/]')
            if ([string]::IsNullOrWhiteSpace($archiveRelative) -or
                [IO.Path]::IsPathRooted($archiveRelative) -or
                $archiveRelative -match '[:\x00-\x1F\x7F]' -or
                @($segments | Where-Object {
                    [string]::IsNullOrEmpty($_) -or $_ -ceq '.' -or $_ -ceq '..' -or
                        $_.EndsWith('.') -or $_.EndsWith(' ')
                }).Count -ne 0) {
                throw "Reviewed $($specification.title) qualification archive contains an unsafe path: $archiveRelative"
            }
            $normalizedArchivePath = $archiveRelative.Replace('\', '/')
            if (-not $listedPaths.Add($normalizedArchivePath)) {
                throw "Reviewed $($specification.title) qualification archive repeats a path: $archiveRelative"
            }
            [void](Assert-Stage5PathUnderRoot -Root $extractRoot `
                -Path (Join-Path $extractRoot $archiveRelative) `
                -Description 'Reviewed qualification archive entry')
        }
        if ($listedPaths.Count -eq 0) {
            throw "Reviewed $($specification.title) qualification archive has no inspectable entries."
        }

        & $sevenZipExecutable x $archiveRecord.path "-o$extractRoot" -y
        if ($LASTEXITCODE -ne 0) {
            throw "Could not extract reviewed $($specification.title) qualification data."
        }

        $sourceItems = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -Force)
        foreach ($sourceItem in $sourceItems) {
            $sourceFull = Assert-Stage5PathUnderRoot -Root $extractRoot `
                -Path $sourceItem.FullName -Description 'Extracted qualification-data item'
            if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Qualification data contains a reparse point: $sourceFull"
            }
        }

        $selected = New-Object 'Collections.Generic.List[object]'
        foreach ($sourceFile in @($sourceItems | Where-Object { -not $_.PSIsContainer })) {
            $sourceFull = Assert-Stage5ExistingRegularPath -Path $sourceFile.FullName `
                -PathType Leaf -Description 'Extracted qualification-data file'
            [void](Assert-Stage5PathUnderRoot -Root $extractRoot -Path $sourceFull `
                -Description 'Extracted qualification-data file')
            $relative = $sourceFull.Substring($extractRoot.Length).TrimStart('\', '/')
            $relative = $relative.Replace('\', '/')
            $isRootBig = $relative.IndexOf('/') -lt 0 -and
                $sourceFile.Extension -ieq '.big'
            $isDataFile = $relative.StartsWith('Data/',
                [StringComparison]::OrdinalIgnoreCase)
            if ($isRootBig -or $isDataFile) {
                $selected.Add([pscustomobject]@{
                    source = $sourceFull
                    relative = $relative
                }) | Out-Null
            }
        }

        $selectedPaths = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        foreach ($selectedFile in $selected) {
            if (-not $selectedPaths.Add([string]$selectedFile.relative)) {
                throw "Reviewed $($specification.title) qualification data repeats a path: $($selectedFile.relative)"
            }
        }
        foreach ($requiredPath in $specification.required) {
            if (-not $selectedPaths.Contains($requiredPath)) {
                throw "Reviewed $($specification.title) qualification data omits $requiredPath."
            }
        }

        $runtimeRoot = [string]$runtimeRoots[[string]$specification.title]
        foreach ($selectedFile in $selected) {
            $destination = Assert-Stage5PathUnderRoot -Root $runtimeRoot `
                -Path (Join-Path $runtimeRoot ([string]$selectedFile.relative)) `
                -Description 'Qualification-data runtime destination'
            $destinationHash = Copy-Stage5LockedFile -Source $selectedFile.source `
                -Destination $destination
            $dataEntries.Add([ordered]@{
                title = $specification.title
                path = "$($specification.runtimeLeaf)/$($selectedFile.relative)"
                sha256 = $destinationHash
            }) | Out-Null
        }
    }

    $entriesByIdentity = @{}
    foreach ($entry in $dataEntries.ToArray()) {
        $identity = '{0}|{1}' -f $entry.title, $entry.path
        if ($entriesByIdentity.ContainsKey($identity)) {
            throw "Qualification data repeats staged identity: $identity"
        }
        $entriesByIdentity[$identity] = $entry
    }
    [string[]]$entryIdentities = @($entriesByIdentity.Keys)
    [Array]::Sort($entryIdentities, [StringComparer]::Ordinal)
    $dataEntryArray = @($entryIdentities | ForEach-Object {
        $entriesByIdentity[$_]
    })
    $canonicalLines = @($dataEntryArray | ForEach-Object {
        '{0}|{1}|{2}' -f $_.title, $_.path, $_.sha256
    })
    [Array]::Sort($canonicalLines, [StringComparer]::Ordinal)
    $canonicalText = ($canonicalLines -join "`n") + "`n"
    $closureSha256 = Get-Stage5TextSha256 -Text $canonicalText

    $dataManifestPath = Join-Path $script:QualificationRootFull `
        'Stage5QualificationData.json'
    $dataManifest = [ordered]@{
        schemaVersion = 2
        evidenceKind = 'lockstep-v2-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        mapName = $MapName
        mapCrcs = [ordered]@{
            Generals = $GeneralsMapCrc
            ZeroHour = $ZeroHourMapCrc
        }
        archiveSources = @($archiveEntries.ToArray())
        files = $dataEntryArray
        closureSha256 = $closureSha256
    }
    $dataManifestJson = $dataManifest | ConvertTo-Json -Depth 8
    Write-Stage5NewUtf8File -Path $dataManifestPath -Text $dataManifestJson
    $manifestStream = [IO.File]::Open($dataManifestPath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $dataManifestSha256 = Get-Stage5StreamSha256 -Stream $manifestStream
    if ($dataManifestSha256 -cne (Get-Stage5TextSha256 -Text $dataManifestJson)) {
        throw 'Stage 5 qualification-data manifest bytes do not match the reviewed document.'
    }

    # The archive handles have remained open with FileShare.Read from the first
    # accepted hash through extraction, runtime staging, and manifest creation.
    foreach ($archiveRecord in $archiveRecords) {
        $finalArchiveSha256 = Get-Stage5StreamSha256 -Stream $archiveRecord.stream
        if ($finalArchiveSha256 -cne [string]$archiveRecord.sha256) {
            throw "Reviewed $($archiveRecord.specification.title) qualification archive changed during provisioning."
        }
    }

    Write-Stage5EnvironmentBindings -Stream $environmentStream -Bindings ([ordered]@{
        STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256 = $dataManifestSha256
        STAGE5_LOCKSTEP_V2_DATA_CLOSURE_SHA256 = $closureSha256
    })
    $completed = $true
}
finally {
    if ($null -ne $manifestStream) {
        $manifestStream.Dispose()
    }
    foreach ($archiveRecord in $archiveRecords) {
        if ($null -ne $archiveRecord.stream) {
            $archiveRecord.stream.Dispose()
        }
    }
    if ($null -ne $environmentStream) {
        $environmentStream.Dispose()
    }
}

if ($completed) {
    foreach ($archiveRecord in $archiveRecords) {
        Remove-Item -LiteralPath $archiveRecord.path -Force
    }
}
