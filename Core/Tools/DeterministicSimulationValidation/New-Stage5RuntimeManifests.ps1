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
        throw "$Description escapes the Stage 5 qualification root: $pathFull"
    }
    return $pathFull
}

function Get-Stage5RelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootFull = Get-Stage5FullPath -Path $Root
    $pathFull = Assert-Stage5PathUnderRoot -Root $rootFull -Path $Path `
        -Description 'Runtime artifact'
    return $pathFull.Substring($rootFull.Length).TrimStart('\', '/').Replace('\', '/')
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

$script:QualificationRootFull = Get-Stage5FullPath -Path $QualificationRoot
if ($script:QualificationRootFull -cne 'H:\Stage5WeeklyPromotionQualification') {
    throw 'Stage 5 lockstep-v2 qualification root is not canonical.'
}
$script:QualificationRootFull = Assert-Stage5ExistingRegularPath `
    -Path $script:QualificationRootFull -PathType Container `
    -Description 'Stage 5 qualification root'

$environmentStream = $null
$runtimeLocks = New-Object 'Collections.Generic.List[object]'
try {
    $environmentStream = Open-Stage5OutputEnvironmentFile -Path $OutputEnvironmentFile

    $titleSpecifications = @(
        [pscustomobject]@{
            title = 'Generals'
            rolePrefix = 'generals'
            root = Join-Path $script:QualificationRootFull 'GeneralsRuntime'
            executablePattern = '^generalsv(?:-[A-Za-z0-9._-]+)?\.exe$'
            executableBinding = 'STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE'
        },
        [pscustomobject]@{
            title = 'ZeroHour'
            rolePrefix = 'zerohour'
            root = Join-Path $script:QualificationRootFull 'ZeroHourRuntime'
            executablePattern = '^generalszh(?:-[A-Za-z0-9._-]+)?\.exe$'
            executableBinding = 'STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE'
        }
    )

    $artifactEntries = New-Object 'Collections.Generic.List[object]'
    $runtimeFiles = New-Object 'Collections.Generic.List[object]'
    $roleByPath = @{}
    $hashByPath = @{}
    $executableBindings = [ordered]@{}
    $initialRuntimePaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)

    foreach ($titleSpecification in $titleSpecifications) {
        $runtimeRoot = Assert-Stage5PathUnderRoot -Root $script:QualificationRootFull `
            -Path $titleSpecification.root -Description 'Downloaded runtime'
        $runtimeRoot = Assert-Stage5ExistingRegularPath -Path $runtimeRoot `
            -PathType Container -Description "Downloaded $($titleSpecification.title) runtime"
        $runtimeItems = @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Force)
        foreach ($runtimeItem in $runtimeItems) {
            $itemFull = Assert-Stage5PathUnderRoot -Root $runtimeRoot `
                -Path $runtimeItem.FullName -Description 'Downloaded runtime item'
            if (($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Downloaded runtime contains a reparse point: $itemFull"
            }
        }

        $files = @($runtimeItems | Where-Object { -not $_.PSIsContainer })
        if ($files.Count -eq 0) {
            throw "Downloaded $($titleSpecification.title) runtime is empty."
        }
        foreach ($file in $files) {
            $fullPath = Assert-Stage5ExistingRegularPath -Path $file.FullName `
                -PathType Leaf -Description 'Downloaded runtime file'
            [void](Assert-Stage5PathUnderRoot -Root $runtimeRoot -Path $fullPath `
                -Description 'Downloaded runtime file')
            if (-not $initialRuntimePaths.Add($fullPath)) {
                throw "Downloaded runtimes repeat a file identity: $fullPath"
            }
            $stream = [IO.File]::Open($fullPath, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::Read)
            try {
                $sha256 = Get-Stage5StreamSha256 -Stream $stream
                $runtimeLocks.Add([pscustomobject]@{
                    path = $fullPath
                    sha256 = $sha256
                    stream = $stream
                }) | Out-Null
                $stream = $null
                $hashByPath[$fullPath.ToLowerInvariant()] = $sha256
            }
            finally {
                if ($null -ne $stream) {
                    $stream.Dispose()
                }
            }
        }

        $executable = @($files | Where-Object {
            $_.Name -cmatch [string]$titleSpecification.executablePattern
        })
        $launcher = @($files | Where-Object { $_.Name -ceq 'launcher.exe' })
        $launcherConfig = @($files | Where-Object { $_.Name -ceq 'launcher.lcf' })
        if ($executable.Count -ne 1 -or $launcher.Count -ne 1 -or
            $launcherConfig.Count -ne 1) {
            throw "Downloaded $($titleSpecification.title) runtime does not contain exactly one executable, launcher, and launcher config."
        }

        foreach ($coreArtifact in @(
            [pscustomobject]@{ suffix = 'executable'; file = $executable[0]; kind = 'executable' },
            [pscustomobject]@{ suffix = 'launcher'; file = $launcher[0]; kind = 'launcher' },
            [pscustomobject]@{ suffix = 'launcher-config'; file = $launcherConfig[0]; kind = 'launcher-config' }
        )) {
            $fullPath = Get-Stage5FullPath -Path $coreArtifact.file.FullName
            $relativePath = Get-Stage5RelativePath -Root $script:QualificationRootFull `
                -Path $fullPath
            $sha256 = [string]$hashByPath[$fullPath.ToLowerInvariant()]
            $role = "$($titleSpecification.rolePrefix)-$($coreArtifact.suffix)"
            $artifactEntries.Add([ordered]@{
                role = $role
                path = $relativePath
                sha256 = $sha256
            }) | Out-Null
            $roleByPath[$fullPath.ToLowerInvariant()] = [string]$coreArtifact.kind
            if ($coreArtifact.suffix -ceq 'executable') {
                $executableBindings[[string]$titleSpecification.executableBinding] = $fullPath
            }
        }

        foreach ($file in $files) {
            $fullPath = Get-Stage5FullPath -Path $file.FullName
            $relativePath = Get-Stage5RelativePath -Root $script:QualificationRootFull `
                -Path $fullPath
            $pathKey = $fullPath.ToLowerInvariant()
            $kind = if ($roleByPath.ContainsKey($pathKey)) {
                [string]$roleByPath[$pathKey]
            }
            elseif ($file.Extension -ieq '.dll') {
                'dll'
            }
            else {
                'asset'
            }
            $runtimeFiles.Add([ordered]@{
                title = [string]$titleSpecification.title
                kind = $kind
                path = $relativePath
                sha256 = [string]$hashByPath[$pathKey]
            }) | Out-Null
        }
    }

    if ($executableBindings.Count -ne 2) {
        throw 'Stage 5 runtime manifest production did not discover both title executables.'
    }

    $runtimeFileArray = @($runtimeFiles.ToArray() | Sort-Object title, kind, path)
    $canonicalLines = @($runtimeFileArray | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.kind, $_.path, $_.sha256
    })
    [Array]::Sort($canonicalLines, [StringComparer]::Ordinal)
    $canonicalText = ($canonicalLines -join "`n") + "`n"
    $closureSha256 = Get-Stage5TextSha256 -Text $canonicalText

    $runtimeClosureManifestPath = Join-Path $script:QualificationRootFull `
        'Stage5RuntimeDependencies.json'
    $artifactSetPath = Join-Path $script:QualificationRootFull 'Stage5ArtifactSet.json'
    $runtimeClosureManifest = [ordered]@{
        schemaVersion = 1
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        architecture = 'x64'
        files = $runtimeFileArray
    }
    $runtimeClosureManifestJson = $runtimeClosureManifest | ConvertTo-Json -Depth 8
    $runtimeClosureManifestSha256 = Get-Stage5TextSha256 `
        -Text $runtimeClosureManifestJson

    $artifactSet = [ordered]@{
        schemaVersion = 1
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        architecture = 'x64'
        artifacts = @($artifactEntries.ToArray())
        runtimeClosure = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'Stage5RuntimeDependencies.json'
                sha256 = $runtimeClosureManifestSha256
            }
            closureSha256 = $closureSha256
        }
    }
    $artifactSetJson = $artifactSet | ConvertTo-Json -Depth 8

    Write-Stage5NewUtf8File -Path $runtimeClosureManifestPath `
        -Text $runtimeClosureManifestJson
    Write-Stage5NewUtf8File -Path $artifactSetPath -Text $artifactSetJson

    $observedRuntimePaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($titleSpecification in $titleSpecifications) {
        $runtimeRoot = Get-Stage5FullPath -Path $titleSpecification.root
        foreach ($item in @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Downloaded runtime changed to include a reparse point: $($item.FullName)"
            }
            if (-not $item.PSIsContainer) {
                [void]$observedRuntimePaths.Add((Get-Stage5FullPath -Path $item.FullName))
            }
        }
    }
    if ($observedRuntimePaths.Count -ne $initialRuntimePaths.Count) {
        throw 'Downloaded runtime file membership changed while manifests were produced.'
    }
    foreach ($initialPath in $initialRuntimePaths) {
        if (-not $observedRuntimePaths.Contains($initialPath)) {
            throw "Downloaded runtime file membership changed: $initialPath"
        }
    }
    foreach ($runtimeLock in $runtimeLocks) {
        $observedSha256 = Get-Stage5StreamSha256 -Stream $runtimeLock.stream
        if ($observedSha256 -cne [string]$runtimeLock.sha256) {
            throw "Downloaded runtime file changed while manifests were produced: $($runtimeLock.path)"
        }
    }

    $bindings = [ordered]@{
        STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE = [string]$executableBindings['STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE']
        STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE = [string]$executableBindings['STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE']
        STAGE5_LOCKSTEP_V2_RUNTIME_MANIFEST_SHA256 = $runtimeClosureManifestSha256
        STAGE5_LOCKSTEP_V2_RUNTIME_CLOSURE_SHA256 = $closureSha256
    }
    Write-Stage5EnvironmentBindings -Stream $environmentStream -Bindings $bindings
}
finally {
    foreach ($runtimeLock in $runtimeLocks) {
        if ($null -ne $runtimeLock.stream) {
            $runtimeLock.stream.Dispose()
        }
    }
    if ($null -ne $environmentStream) {
        $environmentStream.Dispose()
    }
}
