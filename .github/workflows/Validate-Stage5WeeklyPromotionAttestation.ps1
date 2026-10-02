[CmdletBinding(DefaultParameterSetName = 'Validate')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Validate')]
    [string]$BundleRoot,
    [Parameter(Mandatory = $true, ParameterSetName = 'Validate')]
    [string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true, ParameterSetName = 'Validate')]
    [string]$ExpectedAttestationSha256,
    [Parameter(Mandatory = $true, ParameterSetName = 'Validate')]
    [string]$ExpectedQualificationRunId,
    [Parameter(Mandatory = $true, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$canonicalPromotionRoot = [IO.Path]::GetFullPath(
    'H:\Stage5WeeklyPromotionQualification')
$evidenceModule = Join-Path $repositoryRoot `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1'
Import-Module $evidenceModule -Force

$qualificationDataEvidenceLeaf = 'QualificationData.json'
$generalsQualificationArchiveSha256 =
    '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
$zeroHourQualificationArchiveSha256 =
    '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'

function Assert-PromotionCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-PromotionJsonShape {
    param(
        [object]$Value,
        [string[]]$ExpectedNames,
        [string]$Context
    )
    Assert-PromotionCondition ($Value -is [Collections.IDictionary]) `
        "$Context must be a JSON object."
    $actualNames = @($Value.Keys | ForEach-Object { [string]$_ })
    Assert-PromotionCondition ($actualNames.Count -eq $ExpectedNames.Count) `
        "$Context has an unexpected field count."
    foreach ($name in $ExpectedNames) {
        # Windows PowerShell's JavaScriptSerializer fallback returns
        # Dictionary[string,object]. Its public Contains overload expects a
        # KeyValuePair, unlike Hashtable.Contains(key). Compare the already
        # materialized names so both engines enforce the same case-sensitive
        # shape contract.
        Assert-PromotionCondition ($actualNames -ccontains $name) `
            "$Context is missing field '$name'."
    }
}

function Test-PromotionJsonInteger {
    param([object]$Value)
    return $Value -is [sbyte] -or $Value -is [byte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}

function Assert-PromotionMapCrcs {
    param([object]$Value, [string]$Context)

    Assert-PromotionJsonShape $Value @('Generals', 'ZeroHour') $Context
    $result = [ordered]@{}
    foreach ($title in @('Generals', 'ZeroHour')) {
        Assert-PromotionCondition ((Test-PromotionJsonInteger $Value[$title]) -and
            [UInt64]$Value[$title] -gt 0 -and
            [UInt64]$Value[$title] -le [UInt64][UInt32]::MaxValue) `
            "$Context '$title' must be a nonzero UInt32 JSON integer."
        $result[$title] = [UInt32]$Value[$title]
    }
    return $result
}

function Get-PromotionSha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Get-PromotionSha256FromText {
    param([string]$Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '')
    }
    finally { $algorithm.Dispose() }
}

function Assert-PromotionQualificationDataManifest {
    param(
        [string]$Path,
        [string]$SourceCommit,
        [string]$MapName,
        [object]$MapCrcs
    )
    $document = ConvertFrom-Stage5JsonDictionary $Path
    Assert-PromotionJsonShape $document @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'productSet', 'mapName', 'mapCrcs',
        'archiveSources', 'files', 'closureSha256') `
        'Installed lockstep-v2 qualification data manifest'
    Assert-PromotionCondition ($document['productSet'] -is [Array]) `
        'Installed lockstep-v2 qualification data productSet must be a JSON array.'
    $productSet = @($document['productSet'])
    $expectedMapCrcs = Assert-PromotionMapCrcs $MapCrcs `
        'Installed lockstep-v2 evidence map CRCs'
    $manifestMapCrcs = Assert-PromotionMapCrcs $document['mapCrcs'] `
        'Installed lockstep-v2 qualification data map CRCs'
    Assert-PromotionCondition ((Test-PromotionJsonInteger `
            $document['schemaVersion']) -and
        $document['schemaVersion'] -eq 2 -and
        $document['evidenceKind'] -is [string] -and
        $document['evidenceKind'] -ceq
            'lockstep-v2-qualification-data' -and
        $document['producer'] -is [string] -and
        $document['producer'] -ceq 'genci-r2-trimmed-data' -and
        $document['sourceCommit'] -is [string] -and
        $document['sourceCommit'] -ceq $SourceCommit -and
        $productSet.Count -eq 2 -and
        $productSet[0] -is [string] -and
        $productSet[0] -ceq 'Generals' -and
        $productSet[1] -is [string] -and
        $productSet[1] -ceq 'ZeroHour' -and
        $document['mapName'] -is [string] -and
        $document['mapName'] -ceq $MapName -and
        $manifestMapCrcs['Generals'] -eq $expectedMapCrcs['Generals'] -and
        $manifestMapCrcs['ZeroHour'] -eq $expectedMapCrcs['ZeroHour'] -and
        $document['closureSha256'] -is [string] -and
        $document['closureSha256'] -cmatch '^[0-9A-F]{64}$') `
        'Installed lockstep-v2 qualification data identity or map binding is stale or substituted.'

    $expectedArchives = @(
        [pscustomobject]@{
            title = 'Generals'
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = $generalsQualificationArchiveSha256
        },
        [pscustomobject]@{
            title = 'ZeroHour'
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = $zeroHourQualificationArchiveSha256
        })
    $archiveSources = @($document['archiveSources'])
    Assert-PromotionCondition ($archiveSources.Count -eq
        $expectedArchives.Count) `
        'Installed lockstep-v2 qualification data must bind exactly two reviewed archives.'
    for ($archiveIndex = 0; $archiveIndex -lt $archiveSources.Count;
        ++$archiveIndex) {
        $archive = $archiveSources[$archiveIndex]
        $expectedArchive = $expectedArchives[$archiveIndex]
        Assert-PromotionJsonShape $archive @('title', 'object', 'sha256') `
            'Installed lockstep-v2 qualification data archive source'
        Assert-PromotionCondition (
            $archive['title'] -is [string] -and
            $archive['title'] -ceq $expectedArchive.title -and
            $archive['object'] -is [string] -and
            $archive['object'] -ceq $expectedArchive.object -and
            $archive['sha256'] -is [string] -and
            $archive['sha256'] -ceq $expectedArchive.sha256) `
            'Installed lockstep-v2 qualification data archive is unreviewed, reordered, or substituted.'
    }

    $requiredByTitle = [ordered]@{
        Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
        ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    Assert-PromotionCondition ($document['files'] -is [Array]) `
        'Installed lockstep-v2 qualification data files must be a JSON array.'
    $entries = @($document['files'])
    Assert-PromotionCondition ($entries.Count -ge 12) `
        'Installed lockstep-v2 qualification data omits its required staged files.'
    $seenPaths = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $seenTitlePaths = @{
        Generals = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
        ZeroHour = New-Object 'Collections.Generic.HashSet[string]' `
            ([StringComparer]::OrdinalIgnoreCase)
    }
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    $previousIdentity = $null
    foreach ($entry in $entries) {
        Assert-PromotionJsonShape $entry @('title', 'path', 'sha256') `
            'Installed lockstep-v2 qualification data file'
        $title = $entry['title']
        $relative = $entry['path']
        $declaredHash = $entry['sha256']
        Assert-PromotionCondition ($title -is [string] -and
            $relative -is [string] -and $declaredHash -is [string]) `
            'Installed lockstep-v2 qualification data file fields must be JSON strings.'
        $runtimePrefix = if ($title -ceq 'Generals') {
            'GeneralsRuntime/'
        }
        elseif ($title -ceq 'ZeroHour') { 'ZeroHourRuntime/' }
        else { '' }
        $titleRelative = if ($relative.StartsWith($runtimePrefix,
                [StringComparison]::Ordinal)) {
            $relative.Substring($runtimePrefix.Length)
        }
        else { '' }
        $isRootBig = $titleRelative.IndexOf('/') -lt 0 -and
            $titleRelative.EndsWith('.big',
                [StringComparison]::OrdinalIgnoreCase)
        $isDataFile = $titleRelative.StartsWith('Data/',
            [StringComparison]::OrdinalIgnoreCase)
        $identity = "$title|$relative"
        Assert-PromotionCondition (
            -not [string]::IsNullOrWhiteSpace($runtimePrefix) -and
            -not [string]::IsNullOrWhiteSpace($titleRelative) -and
            $relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*$' -and
            $relative -notmatch '(^|/)\.\.?(/|$)' -and
            ($isRootBig -or $isDataFile) -and
            $declaredHash -cmatch '^[0-9A-F]{64}$' -and
            ($null -eq $previousIdentity -or
                [StringComparer]::Ordinal.Compare($previousIdentity,
                    $identity) -lt 0) -and
            $seenPaths.Add($relative) -and
            $seenTitlePaths[$title].Add($titleRelative)) `
            "Installed lockstep-v2 qualification data file is unsafe, duplicated, or unsorted: $relative"
        $canonicalLines.Add(('{0}|{1}|{2}' -f $title, $relative,
            $declaredHash)) | Out-Null
        $previousIdentity = $identity
    }
    foreach ($title in $requiredByTitle.Keys) {
        foreach ($requiredPath in $requiredByTitle[$title]) {
            Assert-PromotionCondition ($seenTitlePaths[$title].Contains(
                    $requiredPath)) `
                "Installed lockstep-v2 qualification data for $title omits $requiredPath."
        }
    }
    $computedClosureSha256 = Get-PromotionSha256FromText `
        (($canonicalLines.ToArray() -join "`n") + "`n")
    Assert-PromotionCondition ($document['closureSha256'] -is [string] -and
        $computedClosureSha256 -ceq $document['closureSha256']) `
        'Installed lockstep-v2 qualification data closure is stale or substituted.'
    return [pscustomobject]@{
        path = [IO.Path]::GetFullPath($Path)
        manifestSha256 = Get-PromotionSha256 $Path
        closureSha256 = $computedClosureSha256
        fileCount = $entries.Count
    }
}

function Resolve-PromotionFile {
    param([string]$Root, [string]$RelativePath, [string]$Context)
    Assert-PromotionCondition (-not [string]::IsNullOrWhiteSpace($RelativePath) -and
        -not [IO.Path]::IsPathRooted($RelativePath) -and
        $RelativePath -notmatch '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))') `
        "$Context is not a safe bundle-relative path."
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    Assert-PromotionCondition ($candidate.StartsWith($prefix,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $candidate -PathType Leaf)) `
        "$Context was not found inside the qualification bundle."
    $item = Get-Item -LiteralPath $candidate -Force
    Assert-PromotionCondition (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Context is a reparse point."
    return $candidate
}

function Assert-PromotionX64Pe {
    param([string]$Path, [string]$Context)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        try {
            Assert-PromotionCondition ($stream.Length -ge 136 -and
                $reader.ReadUInt16() -eq 0x5A4D) `
                "$Context is not a PE image."
            $stream.Position = 0x3c
            $offset = $reader.ReadInt32()
            Assert-PromotionCondition ($offset -ge 0x40 -and
                $offset + 6 -le $stream.Length) `
                "$Context has an invalid PE header offset."
            $stream.Position = $offset
            Assert-PromotionCondition ($reader.ReadUInt32() -eq 0x00004550 -and
                $reader.ReadUInt16() -eq 0x8664) `
                "$Context is not a native x64 PE image."
        }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Assert-PromotionRuntimeClosure {
    param(
        [string]$Root,
        [Collections.IDictionary]$ArtifactSet,
        [string]$SourceCommit
    )
    $binding = Get-Stage5RuntimeClosureBinding `
        -ArtifactSet $ArtifactSet `
        -ArtifactDirectory $Root `
        -ExpectedSourceCommit $SourceCommit `
        -Context 'Weekly release promoted artifact set'

    $runtimeRoots = [ordered]@{
        Generals = [IO.Path]::GetFullPath((Join-Path $Root 'GeneralsRuntime'))
        ZeroHour = [IO.Path]::GetFullPath((Join-Path $Root 'ZeroHourRuntime'))
    }
    foreach ($runtimeRoot in $runtimeRoots.Values) {
        Assert-PromotionCondition (Test-Path -LiteralPath $runtimeRoot -PathType Container) `
            "Qualified runtime directory was not found: $runtimeRoot"
        $directoryItem = Get-Item -LiteralPath $runtimeRoot -Force
        Assert-PromotionCondition (($directoryItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Qualified runtime directory is a reparse point: $runtimeRoot"
    }

    $declaredFiles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($binding.files)) {
        Assert-PromotionCondition ($file.fullPath -is [string]) `
            'Runtime closure file fullPath must be a JSON string.'
        $full = [IO.Path]::GetFullPath($file.fullPath)
        $contained = $false
        foreach ($runtimeRoot in $runtimeRoots.Values) {
            $prefix = $runtimeRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
            if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $contained = $true
                break
            }
        }
        Assert-PromotionCondition $contained `
            "Runtime closure contains a file outside the published title roots: $full"
        Assert-PromotionCondition ($declaredFiles.Add($full)) `
            "Runtime closure repeats a published file: $full"
    }

    $actualItems = @($runtimeRoots.Values | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Recurse -Force
    })
    foreach ($item in $actualItems) {
        Assert-PromotionCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Published runtime tree contains a reparse point: $($item.FullName)"
    }
    $actualFiles = @($actualItems | Where-Object { -not $_.PSIsContainer })
    Assert-PromotionCondition ($actualFiles.Count -eq $declaredFiles.Count) `
        'Published runtime trees contain missing or undeclared files.'
    foreach ($file in $actualFiles) {
        Assert-PromotionCondition (($file.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0 -and
            $declaredFiles.Contains([IO.Path]::GetFullPath($file.FullName))) `
            "Published runtime file is undeclared or redirected: $($file.FullName)"
    }
    return [pscustomobject]@{ binding = $binding; runtimeRoots = $runtimeRoots }
}

function Get-PromotionArtifactBindings {
    param(
        [string]$Root,
        [Collections.IDictionary]$ArtifactSet,
        [Collections.IDictionary]$RuntimeRoots
    )
    $roleSpecifications = [ordered]@{
        'generals-executable' = @('Generals', '^generalsv(?:-[A-Za-z0-9._-]+)?\.exe$')
        'generals-launcher' = @('Generals', '^launcher\.exe$')
        'generals-launcher-config' = @('Generals', '^launcher\.lcf$')
        'zerohour-executable' = @('ZeroHour', '^generalszh(?:-[A-Za-z0-9._-]+)?\.exe$')
        'zerohour-launcher' = @('ZeroHour', '^launcher\.exe$')
        'zerohour-launcher-config' = @('ZeroHour', '^launcher\.lcf$')
    }
    $entries = @($ArtifactSet['artifacts'])
    Assert-PromotionCondition ($entries.Count -eq $roleSpecifications.Count) `
        'Weekly release artifact set must contain exactly six launcher-facing roles.'
    $resolved = @{}
    foreach ($entry in $entries) {
        Assert-PromotionJsonShape $entry @('role', 'path', 'sha256') `
            'Weekly release artifact entry'
        $role = $entry['role']
        $pathValue = $entry['path']
        $hashValue = $entry['sha256']
        Assert-PromotionCondition ($role -is [string] -and
            $pathValue -is [string] -and $hashValue -is [string]) `
            'Weekly release artifact fields must be JSON strings.'
        Assert-PromotionCondition ($roleSpecifications.Contains($role) -and
            -not $resolved.ContainsKey($role)) `
            "Weekly release artifact role is missing, duplicated, or unsupported: $role"
        $path = Resolve-PromotionFile $Root $pathValue `
            "Weekly release artifact '$role'"
        $expectedTitle = [string]$roleSpecifications[$role][0]
        $titleRoot = [IO.Path]::GetFullPath([string]$RuntimeRoots[$expectedTitle])
        $titlePrefix = $titleRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        Assert-PromotionCondition ($path.StartsWith($titlePrefix,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($path) -cmatch [string]$roleSpecifications[$role][1]) `
            "Weekly release artifact '$role' is outside its published title root or has the wrong name."
        $expectedHash = $hashValue
        Assert-PromotionCondition ($expectedHash -cmatch '^[0-9A-Fa-f]{64}$' -and
            (Get-PromotionSha256 $path) -ceq $expectedHash.ToUpperInvariant()) `
            "Weekly release artifact '$role' does not match its manifest SHA-256."
        $resolved[$role] = [pscustomobject]@{
            path = $path
            sha256 = $expectedHash.ToUpperInvariant()
        }
    }
    foreach ($role in $roleSpecifications.Keys) {
        Assert-PromotionCondition ($resolved.ContainsKey($role)) `
            "Weekly release artifact role is missing: $role"
    }
    Assert-PromotionX64Pe $resolved['generals-executable'].path `
        'Qualified Generals executable'
    Assert-PromotionX64Pe $resolved['zerohour-executable'].path `
        'Qualified Zero Hour executable'
    return $resolved
}

function Assert-PromotionEvidence {
    param(
        [Collections.IDictionary]$Evidence,
        [string]$EvidenceSha256,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [object]$RuntimeClosure,
        [Collections.IDictionary]$Artifacts
    )
    $evidenceNames = @('schemaVersion', 'evidenceKind', 'status', 'producer',
        'validationMode', 'architecture', 'sourceCommit', 'artifactSetSha256',
        'recordedUtc', 'cohortNonce', 'runtimeClosure',
        'allowHeadlessDirectExecution', 'launcherEquivalence',
        'commonStopFrame', 'peerCount', 'networkRosterMask',
        'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount', 'mapName',
        'mapCrcs', 'seed', 'negativeProbes', 'v1Accepted', 'profileStrategy',
        'registryViews', 'environmentVariables', 'profileConcurrency',
        'titleSessionDisposition', 'sessions')
    Assert-PromotionJsonShape $Evidence $evidenceNames `
        'Installed lockstep-v2 evidence'
    [void](Assert-PromotionMapCrcs $Evidence['mapCrcs'] `
        'Installed lockstep-v2 evidence map CRCs')
    Assert-PromotionCondition ((Test-PromotionJsonInteger $Evidence['schemaVersion']) -and
        $Evidence['schemaVersion'] -eq 2 -and
        $Evidence['evidenceKind'] -is [string] -and
        $Evidence['evidenceKind'] -ceq 'lockstep-v2-multiplayer' -and
        $Evidence['status'] -is [string] -and
        $Evidence['status'] -ceq 'passed' -and
        $Evidence['producer'] -is [string] -and
        $Evidence['producer'] -ceq 'installed-lockstep-v2' -and
        $Evidence['validationMode'] -is [string] -and
        $Evidence['validationMode'] -ceq 'installed-lockstep-v2-production' -and
        $Evidence['architecture'] -is [string] -and
        $Evidence['architecture'] -ceq 'x64' -and
        $Evidence['sourceCommit'] -is [string] -and
        $Evidence['sourceCommit'] -ceq $SourceCommit -and
        $Evidence['artifactSetSha256'] -is [string] -and
        $Evidence['artifactSetSha256'] -ceq $ArtifactSetSha256 -and
        (Test-PromotionJsonInteger $Evidence['commonStopFrame']) -and
        $Evidence['commonStopFrame'] -eq 4096 -and
        (Test-PromotionJsonInteger $Evidence['peerCount']) -and
        $Evidence['peerCount'] -eq 2 -and
        (Test-PromotionJsonInteger $Evidence['networkRosterMask']) -and
        $Evidence['networkRosterMask'] -eq 3 -and
        (Test-PromotionJsonInteger $Evidence['simulationRosterMask']) -and
        $Evidence['simulationRosterMask'] -eq 63 -and
        (Test-PromotionJsonInteger $Evidence['aiRosterMask']) -and
        $Evidence['aiRosterMask'] -eq 60 -and
        (Test-PromotionJsonInteger $Evidence['aiPlayerCount']) -and
        $Evidence['aiPlayerCount'] -eq 4 -and
        $Evidence['v1Accepted'] -is [bool] -and
        -not [bool]$Evidence['v1Accepted'] -and
        $Evidence['allowHeadlessDirectExecution'] -is [bool] -and
        [bool]$Evidence['allowHeadlessDirectExecution'] -and
        $Evidence['cohortNonce'] -is [string] -and
        $Evidence['mapName'] -is [string] -and
        $Evidence['titleSessionDisposition'] -is [string] -and
        $Evidence['titleSessionDisposition'] -ceq
            'removed-after-peer-exit-before-evidence-persist') `
        'Installed lockstep-v2 evidence does not satisfy the promoted 2v4 x64 boundary.'
    Assert-PromotionJsonShape $Evidence['runtimeClosure'] `
        @('dependencyManifestSha256', 'closureSha256') `
        'Installed lockstep-v2 runtime closure'
    Assert-PromotionCondition (
        $Evidence['runtimeClosure']['dependencyManifestSha256'] -is [string] -and
        $Evidence['runtimeClosure']['dependencyManifestSha256'] -ceq
            $RuntimeClosure.dependencyManifestSha256 -and
        $Evidence['runtimeClosure']['closureSha256'] -is [string] -and
        $Evidence['runtimeClosure']['closureSha256'] -ceq
            $RuntimeClosure.closureSha256) `
        'Installed lockstep-v2 evidence is detached from the published runtime closure.'

    $launcherRoles = [ordered]@{
        Generals = @('generals-executable', 'generals-launcher', 'generals-launcher-config')
        ZeroHour = @('zerohour-executable', 'zerohour-launcher', 'zerohour-launcher-config')
    }
    Assert-PromotionJsonShape $Evidence['launcherEquivalence'] @('Generals', 'ZeroHour') `
        'Installed lockstep-v2 launcher aggregate'
    foreach ($title in $launcherRoles.Keys) {
        $contract = $Evidence['launcherEquivalence'][$title]
        Assert-PromotionJsonShape $contract @('configSha256', 'launcherSha256') `
            "Installed lockstep-v2 $title launcher contract"
        Assert-PromotionCondition ($contract['configSha256'] -is [string] -and
                $contract['configSha256'] -ceq
                    $Artifacts[$launcherRoles[$title][2]].sha256 -and
            $contract['launcherSha256'] -is [string] -and
            $contract['launcherSha256'] -ceq
                $Artifacts[$launcherRoles[$title][1]].sha256) `
            "Installed lockstep-v2 $title launcher evidence is detached from the published artifact."
    }

    $sessions = @($Evidence['sessions'])
    Assert-PromotionCondition ($sessions.Count -eq 2) `
        'Installed lockstep-v2 evidence must contain exactly two title sessions.'
    $expectedTitles = @('Generals', 'ZeroHour')
    for ($sessionIndex = 0; $sessionIndex -lt $sessions.Count; ++$sessionIndex) {
        $session = $sessions[$sessionIndex]
        $title = $expectedTitles[$sessionIndex]
        Assert-PromotionCondition ($session['title'] -is [string] -and
            $session['title'] -ceq $title -and
            (Test-PromotionJsonInteger $session['peerCount']) -and
            $session['peerCount'] -eq 2 -and
            (Test-PromotionJsonInteger $session['networkRosterMask']) -and
            $session['networkRosterMask'] -eq 3 -and
            (Test-PromotionJsonInteger $session['simulationRosterMask']) -and
            $session['simulationRosterMask'] -eq 63 -and
            (Test-PromotionJsonInteger $session['aiRosterMask']) -and
            $session['aiRosterMask'] -eq 60 -and
            (Test-PromotionJsonInteger $session['aiPlayerCount']) -and
            $session['aiPlayerCount'] -eq 4 -and
            $session['mixedWorkerProof'] -is [bool] -and
            [bool]$session['mixedWorkerProof'] -and
            $session['profileReadOnlyVerified'] -is [bool] -and
            [bool]$session['profileReadOnlyVerified']) `
            "Installed lockstep-v2 $title session is incomplete or substituted."
        $effectiveWorkerValues = @($session['effectiveWorkerCounts'])
        Assert-PromotionCondition (
            @($effectiveWorkerValues | Where-Object {
                -not (Test-PromotionJsonInteger $_)
            }).Count -eq 0) `
            "Installed lockstep-v2 $title session has non-integer effective worker counts."
        $effectiveWorkers = @($effectiveWorkerValues | ForEach-Object {
            [int]$_
        })
        Assert-PromotionCondition ($effectiveWorkers.Count -eq 2 -and
            @($effectiveWorkers | Where-Object { $_ -lt 2 }).Count -eq 0 -and
            @($effectiveWorkers | Select-Object -Unique).Count -eq 2) `
            "Installed lockstep-v2 $title session did not preserve mixed multicore workers."
        $peers = @($session['peers'])
        Assert-PromotionCondition ($peers.Count -eq 2) `
            "Installed lockstep-v2 $title session does not contain both peers."
        $expectedRole = [string]$launcherRoles[$title][0]
        Assert-PromotionCondition (Test-PromotionJsonInteger $peers[0]['finalCRC']) `
            "Installed lockstep-v2 $title peer reference CRC must be an integer."
        $referenceCrc = $peers[0]['finalCRC']
        foreach ($peer in $peers) {
            Assert-PromotionCondition ((Test-PromotionJsonInteger $peer['schemaVersion']) -and
                $peer['schemaVersion'] -eq 2 -and
                $peer['producer'] -is [string] -and
                $peer['producer'] -ceq 'installed-lockstep-v2' -and
                $peer['validationMode'] -is [string] -and
                $peer['validationMode'] -ceq 'installed-lockstep-v2-production' -and
                $peer['title'] -is [string] -and
                $peer['title'] -ceq $title -and
                $peer['sourceCommit'] -is [string] -and
                $peer['sourceCommit'] -ceq $SourceCommit -and
                $peer['executableSha256'] -is [string] -and
                $peer['executableSha256'] -ceq $Artifacts[$expectedRole].sha256 -and
                (Test-PromotionJsonInteger $peer['peerCount']) -and
                $peer['peerCount'] -eq 2 -and
                (Test-PromotionJsonInteger $peer['networkRosterMask']) -and
                $peer['networkRosterMask'] -eq 3 -and
                (Test-PromotionJsonInteger $peer['simulationRosterMask']) -and
                $peer['simulationRosterMask'] -eq 63 -and
                (Test-PromotionJsonInteger $peer['aiRosterMask']) -and
                $peer['aiRosterMask'] -eq 60 -and
                (Test-PromotionJsonInteger $peer['aiPlayerCount']) -and
                $peer['aiPlayerCount'] -eq 4 -and
                (Test-PromotionJsonInteger $peer['exitCode']) -and
                $peer['exitCode'] -eq 0 -and
                (Test-PromotionJsonInteger $peer['finalFrame']) -and
                $peer['finalFrame'] -eq 4096 -and
                (Test-PromotionJsonInteger $peer['finalCRC']) -and
                $peer['finalCRC'] -gt 0 -and
                $peer['finalCRC'] -eq $referenceCrc -and
                $peer['lockstepV2Receipt'] -is [bool] -and
                [bool]$peer['lockstepV2Receipt'] -and
                $peer['v1ReceiptAccepted'] -is [bool] -and
                -not [bool]$peer['v1ReceiptAccepted'] -and
                $peer['receiptPath'] -is [string]) `
                "Installed lockstep-v2 $title peer is not a clean artifact-bound v2 result."
        }
    }
    Assert-PromotionJsonShape $Evidence['negativeProbes'] @('crossEpoch', 'contentMismatch') `
        'Installed lockstep-v2 negative probes'
    foreach ($probe in @(
        @('crossEpoch', 'negative-cross-epoch'),
        @('contentMismatch', 'negative-content-mismatch'))) {
        $entries = @($Evidence['negativeProbes'][$probe[0]])
        Assert-PromotionCondition ($entries.Count -eq 2) `
            "Installed lockstep-v2 $($probe[0]) evidence must contain both titles."
        foreach ($title in @('Generals', 'ZeroHour')) {
            $matching = @($entries | Where-Object {
                $_['title'] -is [string] -and
                $_['title'] -ceq $title -and
                $_['mode'] -is [string] -and
                $_['mode'] -ceq $probe[1] -and
                $_['baselineAccepted'] -is [bool] -and
                [bool]($_['baselineAccepted']) -and
                $_['mutatedAccepted'] -is [bool] -and
                -not [bool]($_['mutatedAccepted']) -and
                (Test-PromotionJsonInteger $_['exitCode']) -and
                $_['exitCode'] -eq 0
            })
            Assert-PromotionCondition ($matching.Count -eq 1) `
                "Installed lockstep-v2 $title/$($probe[1]) rejection is missing."
        }
    }
    return $EvidenceSha256
}

function Assert-PromotionEvidenceTree {
    param(
        [string]$EvidenceRoot,
        [Collections.IDictionary]$Evidence,
        [Collections.IDictionary]$Closure,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [string]$CohortNonce
    )
    $rootFull = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\', '/')
    Assert-PromotionCondition (Test-Path -LiteralPath $rootFull -PathType Container) `
        "Installed lockstep-v2 evidence root was not found: $rootFull"
    $rootItem = Get-Item -LiteralPath $rootFull -Force
    Assert-PromotionCondition (($rootItem.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Installed lockstep-v2 evidence root is a reparse point: $rootFull"

    $closureLeaf = 'Stage5LockstepV2EvidenceClosure.json'
    Assert-PromotionJsonShape $Closure @('schemaVersion', 'evidenceKind',
        'producer', 'sourceCommit', 'artifactSetSha256', 'cohortNonce',
        'canonicalEvidenceRoot', 'fileCount', 'files', 'closureSha256') `
        'Installed lockstep-v2 evidence closure'
    Assert-PromotionCondition (Test-PromotionJsonInteger $Closure['fileCount']) `
        'Installed lockstep-v2 evidence closure fileCount must be an integer.'
    $closureFileCount = [Int64]$Closure['fileCount']
    Assert-PromotionCondition ((Test-PromotionJsonInteger $Closure['schemaVersion']) -and
        $Closure['schemaVersion'] -eq 1 -and
        $Closure['evidenceKind'] -is [string] -and
        $Closure['evidenceKind'] -ceq 'lockstep-v2-evidence-closure' -and
        $Closure['producer'] -is [string] -and
        $Closure['producer'] -ceq 'installed-lockstep-v2' -and
        $Closure['sourceCommit'] -is [string] -and
        $Closure['sourceCommit'] -ceq $SourceCommit -and
        $Closure['artifactSetSha256'] -is [string] -and
        $Closure['artifactSetSha256'] -ceq $ArtifactSetSha256 -and
        $Closure['cohortNonce'] -is [string] -and
        $Closure['cohortNonce'] -ceq $CohortNonce -and
        $Closure['canonicalEvidenceRoot'] -is [string] -and
        $Closure['canonicalEvidenceRoot'] -ceq $rootFull -and
        $Closure['closureSha256'] -is [string] -and
        $Closure['closureSha256'] -cmatch '^[0-9A-F]{64}$') `
        'Installed lockstep-v2 evidence closure identity or canonical root is stale or substituted.'

    $declaredFiles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $canonicalLines = New-Object 'Collections.Generic.List[string]'
    Assert-PromotionCondition ($Closure['files'] -is [Array]) `
        'Installed lockstep-v2 evidence closure files must be a JSON array.'
    $declaredEntries = @($Closure['files'])
    Assert-PromotionCondition ($closureFileCount -eq $declaredEntries.Count -and
        $declaredEntries.Count -ge 2) `
        'Installed lockstep-v2 evidence closure fileCount is stale or empty.'
    $previousRelative = $null
    foreach ($entry in $declaredEntries) {
        Assert-PromotionJsonShape $entry @('path', 'sha256') `
            'Installed lockstep-v2 evidence closure entry'
        $relative = $entry['path']
        $declaredHash = $entry['sha256']
        Assert-PromotionCondition ($relative -is [string] -and
            $declaredHash -is [string]) `
            'Installed lockstep-v2 evidence closure fields must be JSON strings.'
        Assert-PromotionCondition ($relative -cmatch '^[^\\/:]+(?:/[^\\/:]+)*$' -and
            $relative -notmatch '(^|/)\.\.?(/|$)' -and
            $relative -cne $closureLeaf -and
            ($null -eq $previousRelative -or
                [StringComparer]::Ordinal.Compare($previousRelative, $relative) -lt 0)) `
            "Installed lockstep-v2 evidence closure path is unsafe, duplicated, or not ordinally sorted: $relative"
        Assert-PromotionCondition ($declaredHash -cmatch '^[0-9A-F]{64}$') `
            "Installed lockstep-v2 evidence closure hash is not canonical for '$relative'."
        $full = Resolve-PromotionFile $rootFull $relative `
            "Installed lockstep-v2 evidence closure member '$relative'"
        Assert-Stage5FinalAcceptanceNoReparsePath $rootFull $full `
            "Installed lockstep-v2 evidence closure member '$relative'"
        Assert-PromotionCondition ($declaredFiles.Add($full)) `
            "Installed lockstep-v2 evidence closure aliases member '$relative'."
        Assert-PromotionCondition ((Get-PromotionSha256 $full) -ceq $declaredHash) `
            "Installed lockstep-v2 evidence closure hash does not match '$relative'."
        $canonicalLines.Add(('{0}|{1}' -f $relative, $declaredHash)) | Out-Null
        $previousRelative = $relative
    }
    $computedClosureSha256 = Get-PromotionSha256FromText `
        (($canonicalLines.ToArray() -join "`n") + "`n")
    Assert-PromotionCondition ($Closure['closureSha256'] -is [string] -and
        $computedClosureSha256 -ceq $Closure['closureSha256']) `
        'Installed lockstep-v2 evidence closure aggregate SHA-256 is stale or substituted.'

    $expectedFiles = New-Object 'Collections.Generic.HashSet[string]' `
        ([StringComparer]::OrdinalIgnoreCase)
    $addExpectedFile = {
        param([string]$RelativePath, [string]$Context)
        $full = Resolve-PromotionFile $rootFull $RelativePath $Context
        Assert-Stage5FinalAcceptanceNoReparsePath $rootFull $full $Context
        Assert-PromotionCondition ($expectedFiles.Add($full)) `
            "$Context aliases another declared evidence file."
    }
    & $addExpectedFile 'LockstepV2LoopbackEvidence.json' `
        'Installed lockstep-v2 aggregate evidence'
    & $addExpectedFile 'mixed-worker-multiplayer.json' `
        'Installed lockstep-v2 promotion attestation'
    & $addExpectedFile $qualificationDataEvidenceLeaf `
        'Installed lockstep-v2 qualification data closure'
    foreach ($session in @($Evidence['sessions'])) {
        $title = $session['title']
        Assert-PromotionCondition ($title -is [string]) `
            'Installed lockstep-v2 evidence tree session title must be a JSON string.'
        Assert-PromotionCondition (@('Generals', 'ZeroHour') -ccontains $title) `
            'Installed lockstep-v2 evidence tree contains an unsupported title.'
        $peers = @($session['peers'])
        for ($peerIndex = 0; $peerIndex -lt $peers.Count; ++$peerIndex) {
            $receiptLeaf = $peers[$peerIndex]['receiptPath']
            Assert-PromotionCondition ($receiptLeaf -is [string]) `
                "Installed lockstep-v2 $title peer $peerIndex receipt path must be a JSON string."
            Assert-PromotionCondition ($receiptLeaf -cmatch
                "^lockstep-v2-$title-peer-$peerIndex\.receipt$") `
                "Installed lockstep-v2 $title peer $peerIndex has a substituted receipt path."
            foreach ($leaf in @(
                "peer-$peerIndex.raw.json",
                $receiptLeaf,
                "peer-$peerIndex.stdout.log",
                "peer-$peerIndex.stderr.log")) {
                & $addExpectedFile "$title/$leaf" `
                    "Installed lockstep-v2 $title peer $peerIndex evidence '$leaf'"
            }
        }
    }
    foreach ($collection in @('crossEpoch', 'contentMismatch')) {
        foreach ($probe in @($Evidence['negativeProbes'][$collection])) {
            foreach ($pathField in @('proofPath', 'stdoutPath', 'stderrPath')) {
                Assert-PromotionCondition ($probe[$pathField] -is [string]) `
                    "Installed lockstep-v2 $collection negative probe $pathField must be a JSON string."
                & $addExpectedFile $probe[$pathField] `
                    "Installed lockstep-v2 $collection negative probe $pathField"
            }
        }
    }

    Assert-PromotionCondition ($expectedFiles.Count -eq $declaredFiles.Count) `
        'Installed lockstep-v2 evidence closure does not declare the exact aggregate-derived file set.'
    foreach ($expectedFile in $expectedFiles) {
        Assert-PromotionCondition ($declaredFiles.Contains($expectedFile)) `
            "Installed lockstep-v2 evidence closure omits an aggregate-derived file: $expectedFile"
    }

    $allItems = @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force)
    foreach ($item in $allItems) {
        Assert-PromotionCondition (($item.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -eq 0) `
            "Installed lockstep-v2 evidence tree contains a reparse point: $($item.FullName)"
    }
    $closurePath = [IO.Path]::GetFullPath((Join-Path $rootFull $closureLeaf))
    $actualFiles = @($allItems | Where-Object {
        -not $_.PSIsContainer -and
        [IO.Path]::GetFullPath($_.FullName) -cne $closurePath
    })
    Assert-PromotionCondition ($actualFiles.Count -eq $declaredFiles.Count) `
        'Installed lockstep-v2 evidence tree contains missing or undeclared files.'
    foreach ($file in $actualFiles) {
        Assert-PromotionCondition ($declaredFiles.Contains(
                [IO.Path]::GetFullPath($file.FullName))) `
            "Installed lockstep-v2 evidence tree contains an undeclared file: $($file.FullName)"
    }
    return $computedClosureSha256
}

function Invoke-PromotionValidation {
    param(
        [string]$Root,
        [string]$SourceCommit,
        [string]$AttestationSha256,
        [string]$QualificationRunId,
        [scriptblock]$EvidenceValidationAction = $null,
        [switch]$AllowNonCanonicalSelfTestRoot
    )
    if ($AllowNonCanonicalSelfTestRoot) {
        Assert-PromotionCondition $SelfTest.IsPresent `
            'Noncanonical promotion roots are permitted only during SelfTest.'
    }
    Assert-PromotionCondition ($SourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Expected source commit must be an independently supplied lowercase 40-hex value.'
    Assert-PromotionCondition ($AttestationSha256 -cmatch '^[0-9A-Fa-f]{64}$') `
        'Expected promotion-attestation SHA-256 is malformed.'
    Assert-PromotionCondition ($QualificationRunId -cmatch '^[1-9][0-9]*$') `
        'Expected qualification run id must be a positive decimal value.'
    $rootFull = [IO.Path]::GetFullPath($Root)
    Assert-PromotionCondition ($AllowNonCanonicalSelfTestRoot -or
        $rootFull.TrimEnd('\', '/') -ceq $canonicalPromotionRoot.TrimEnd('\', '/')) `
        "Promotion bundle must be staged at the canonical qualification root: $canonicalPromotionRoot"
    Assert-PromotionCondition (Test-Path -LiteralPath $rootFull -PathType Container) `
        "Promotion bundle root was not found: $rootFull"

    $artifactSetPath = Resolve-PromotionFile $rootFull 'Stage5ArtifactSet.json' `
        'Stage 5 artifact-set manifest'
    $qualificationDataManifestPath = Resolve-PromotionFile $rootFull `
        'Stage5QualificationData.json' `
        'Stage 5 qualification data manifest'
    $evidencePath = Resolve-PromotionFile $rootFull `
        'Evidence\LockstepV2LoopbackEvidence.json' `
        'Installed lockstep-v2 evidence'
    $attestationPath = Resolve-PromotionFile $rootFull `
        'Evidence\mixed-worker-multiplayer.json' `
        'Installed lockstep-v2 promotion attestation'
    $evidenceClosurePath = Resolve-PromotionFile $rootFull `
        'Evidence\Stage5LockstepV2EvidenceClosure.json' `
        'Installed lockstep-v2 evidence-closure attestation'
    Assert-PromotionCondition ((Get-PromotionSha256 $evidenceClosurePath) -ceq
        $AttestationSha256.ToUpperInvariant()) `
        'Installed lockstep-v2 evidence-closure attestation does not match the independently reviewed SHA-256.'

    $artifactSet = ConvertFrom-Stage5JsonDictionary $artifactSetPath
    Assert-PromotionJsonShape $artifactSet @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'runtimeClosure', 'artifacts') `
        'Stage 5 artifact-set manifest'
    Assert-PromotionCondition ($artifactSet['productSet'] -is [Array]) `
        'Stage 5 artifact-set productSet must be a JSON array.'
    $productSet = @($artifactSet['productSet'])
    Assert-PromotionCondition ((Test-PromotionJsonInteger $artifactSet['schemaVersion']) -and
        $artifactSet['schemaVersion'] -eq 1 -and
        $artifactSet['sourceCommit'] -is [string] -and
        $artifactSet['sourceCommit'] -ceq $SourceCommit -and
        $artifactSet['architecture'] -is [string] -and
        $artifactSet['architecture'] -ceq 'x64' -and
        $productSet.Count -eq 2 -and
        $productSet[0] -is [string] -and
        $productSet[0] -ceq 'Generals' -and
        $productSet[1] -is [string] -and
        $productSet[1] -ceq 'ZeroHour') `
        'Stage 5 artifact-set identity is stale, reordered, or not native x64.'
    $artifactSetSha256 = Get-PromotionSha256 $artifactSetPath
    $closure = Assert-PromotionRuntimeClosure $rootFull $artifactSet $SourceCommit
    $artifacts = Get-PromotionArtifactBindings $rootFull $artifactSet `
        $closure.runtimeRoots

    $evidenceSha256 = Get-PromotionSha256 $evidencePath
    $evidence = ConvertFrom-Stage5JsonDictionary $evidencePath
    [void](Assert-PromotionEvidence $evidence $evidenceSha256 $SourceCommit `
        $artifactSetSha256 $closure.binding $artifacts)
    Assert-PromotionCondition ($evidence['mapName'] -is [string] -and
        $evidence['cohortNonce'] -is [string]) `
        'Installed lockstep-v2 evidence mapName and cohortNonce must be JSON strings.'
    $qualificationData = Assert-PromotionQualificationDataManifest `
        -Path $qualificationDataManifestPath `
        -SourceCommit $SourceCommit `
        -MapName $evidence['mapName'] `
        -MapCrcs $evidence['mapCrcs']
    $qualificationDataEvidencePath = Resolve-PromotionFile $rootFull `
        ("Evidence\" + $qualificationDataEvidenceLeaf) `
        'Installed lockstep-v2 qualification data evidence copy'
    Assert-PromotionCondition ((Get-PromotionSha256 `
            $qualificationDataEvidencePath) -ceq
        $qualificationData.manifestSha256) `
        'Installed lockstep-v2 qualification data evidence copy is detached from its root manifest.'
    $evidenceClosure = ConvertFrom-Stage5JsonDictionary $evidenceClosurePath
    $evidenceTreeSha256 = Assert-PromotionEvidenceTree `
        (Join-Path $rootFull 'Evidence') $evidence $evidenceClosure `
        $SourceCommit $artifactSetSha256 $evidence['cohortNonce']

    $attestation = ConvertFrom-Stage5JsonDictionary $attestationPath
    Assert-PromotionJsonShape $attestation @('schemaVersion', 'evidenceKind',
        'status', 'sourceCommit', 'title', 'architecture', 'artifactSetSha256',
        'recordedUtc', 'cohortNonce', 'runtimeClosure', 'attachments', 'details') `
        'Installed lockstep-v2 promotion attestation'
    Assert-PromotionCondition ((Test-PromotionJsonInteger $attestation['schemaVersion']) -and
        $attestation['schemaVersion'] -eq 1 -and
        $attestation['evidenceKind'] -is [string] -and
        $attestation['evidenceKind'] -ceq 'mixed-worker-multiplayer' -and
        $attestation['status'] -is [string] -and
        $attestation['status'] -ceq 'passed' -and
        $attestation['sourceCommit'] -is [string] -and
        $attestation['sourceCommit'] -ceq $SourceCommit -and
        $attestation['title'] -is [string] -and
        $attestation['title'] -ceq 'Both' -and
        $attestation['architecture'] -is [string] -and
        $attestation['architecture'] -ceq 'x64' -and
        $attestation['artifactSetSha256'] -is [string] -and
        $attestation['artifactSetSha256'] -ceq $artifactSetSha256 -and
        $attestation['cohortNonce'] -is [string] -and
        $attestation['cohortNonce'] -ceq $evidence['cohortNonce']) `
        'Installed lockstep-v2 promotion attestation identity is stale or substituted.'
    Assert-PromotionJsonShape $attestation['runtimeClosure'] `
        @('dependencyManifestSha256', 'closureSha256') `
        'Installed lockstep-v2 attestation runtime closure'
    Assert-PromotionCondition (
        $attestation['runtimeClosure']['dependencyManifestSha256'] -is [string] -and
        $attestation['runtimeClosure']['dependencyManifestSha256'] -ceq
            $closure.binding.dependencyManifestSha256 -and
        $attestation['runtimeClosure']['closureSha256'] -is [string] -and
        $attestation['runtimeClosure']['closureSha256'] -ceq
            $closure.binding.closureSha256) `
        'Installed lockstep-v2 attestation is detached from the published runtime closure.'
    $attachments = @($attestation['attachments'])
    Assert-PromotionCondition ($attachments.Count -eq 1 -and
        $attachments[0]['role'] -is [string] -and
        $attachments[0]['role'] -ceq 'multiplayer-results' -and
        $attachments[0]['path'] -is [string] -and
        $attachments[0]['path'] -ceq 'LockstepV2LoopbackEvidence.json' -and
        $attachments[0]['sha256'] -is [string] -and
        $attachments[0]['sha256'] -ceq $evidenceSha256 -and
        $attachments[0]['trustDomain'] -is [string] -and
        $attachments[0]['trustDomain'] -ceq 'host-runner') `
        'Installed lockstep-v2 attestation does not hash-bind its native evidence.'
    Assert-PromotionJsonShape $attestation['details'] @('nativeEvidenceKind',
        'producer', 'nativeEvidenceSha256', 'networkRosterMask',
        'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount', 'title',
        'sessionCount', 'peerCount', 'commonStopFrame', 'allMatchesCompleted',
        'stateTracesIdentical', 'crossEpochRejected', 'contentMismatchRejected') `
        'Installed lockstep-v2 attestation details'
    $details = $attestation['details']
    Assert-PromotionCondition ($details['nativeEvidenceKind'] -is [string] -and
            $details['nativeEvidenceKind'] -ceq
            'lockstep-v2-multiplayer' -and
        $details['producer'] -is [string] -and
        $details['producer'] -ceq 'installed-lockstep-v2' -and
        $details['nativeEvidenceSha256'] -is [string] -and
        $details['nativeEvidenceSha256'] -ceq $evidenceSha256 -and
        (Test-PromotionJsonInteger $details['networkRosterMask']) -and
        $details['networkRosterMask'] -eq 3 -and
        (Test-PromotionJsonInteger $details['simulationRosterMask']) -and
        $details['simulationRosterMask'] -eq 63 -and
        (Test-PromotionJsonInteger $details['aiRosterMask']) -and
        $details['aiRosterMask'] -eq 60 -and
        (Test-PromotionJsonInteger $details['aiPlayerCount']) -and
        $details['aiPlayerCount'] -eq 4 -and
        $details['title'] -is [string] -and
        $details['title'] -ceq 'Both' -and
        (Test-PromotionJsonInteger $details['sessionCount']) -and
        $details['sessionCount'] -eq 2 -and
        (Test-PromotionJsonInteger $details['peerCount']) -and
        $details['peerCount'] -eq 2 -and
        (Test-PromotionJsonInteger $details['commonStopFrame']) -and
        $details['commonStopFrame'] -eq 4096 -and
        $details['allMatchesCompleted'] -is [bool] -and
        [bool]$details['allMatchesCompleted'] -and
        $details['stateTracesIdentical'] -is [bool] -and
        [bool]$details['stateTracesIdentical'] -and
        $details['crossEpochRejected'] -is [bool] -and
        [bool]$details['crossEpochRejected'] -and
        $details['contentMismatchRejected'] -is [bool] -and
        [bool]$details['contentMismatchRejected']) `
        'Installed lockstep-v2 attestation does not prove the complete promoted boundary.'

    $artifactHashes = @{}
    $artifactPaths = @{}
    foreach ($role in $artifacts.Keys) {
        $artifactHashes[$role] = [string]$artifacts[$role].sha256
        $artifactPaths[$role] = [string]$artifacts[$role].path
    }
    $evidenceValidationContext = [pscustomobject]@{
        path = $evidencePath
        sourceCommit = $SourceCommit
        artifactSetSha256 = $artifactSetSha256
        artifactHashes = $artifactHashes
        artifactPaths = $artifactPaths
        artifactRootDirectory = $rootFull
        evidenceSha256 = $evidenceSha256
        cohortNonce = $attestation['cohortNonce']
        runtimeClosure = $closure.binding
    }
    $validatedEvidence = if ($null -eq $EvidenceValidationAction) {
        Read-Stage5LockstepV2Evidence `
            -Path $evidenceValidationContext.path `
            -ExpectedSourceCommit $evidenceValidationContext.sourceCommit `
            -ExpectedArtifactSetSha256 $evidenceValidationContext.artifactSetSha256 `
            -ArtifactHashes $evidenceValidationContext.artifactHashes `
            -ArtifactPaths $evidenceValidationContext.artifactPaths `
            -ArtifactRootDirectory $evidenceValidationContext.artifactRootDirectory `
            -ExpectedEvidenceSha256 $evidenceValidationContext.evidenceSha256 `
            -ExpectedCohortNonce $evidenceValidationContext.cohortNonce `
            -ExpectedRuntimeClosure $evidenceValidationContext.runtimeClosure
    }
    else {
        & $EvidenceValidationAction $evidenceValidationContext
    }
    Assert-PromotionCondition ($null -ne $validatedEvidence -and
        (Test-PromotionJsonInteger $validatedEvidence.schemaVersion) -and
        $validatedEvidence.schemaVersion -eq 2 -and
        $validatedEvidence.evidenceKind -is [string] -and
        $validatedEvidence.evidenceKind -ceq 'lockstep-v2-multiplayer' -and
        $validatedEvidence.producer -is [string] -and
        $validatedEvidence.producer -ceq 'installed-lockstep-v2' -and
        $validatedEvidence.sourceCommit -is [string] -and
        $validatedEvidence.sourceCommit -ceq $SourceCommit -and
        $validatedEvidence.artifactSetSha256 -is [string] -and
        $validatedEvidence.artifactSetSha256 -ceq $artifactSetSha256 -and
        $validatedEvidence.cohortNonce -is [string] -and
        $validatedEvidence.cohortNonce -ceq $attestation['cohortNonce'] -and
        (Test-PromotionJsonInteger $validatedEvidence.commonStopFrame) -and
        $validatedEvidence.commonStopFrame -eq 4096 -and
        (Test-PromotionJsonInteger $validatedEvidence.peerCount) -and
        $validatedEvidence.peerCount -eq 2 -and
        $validatedEvidence.sessions -is [Array] -and
        @($validatedEvidence.sessions).Count -eq 2 -and
        $validatedEvidence.crossEpochRejected -is [bool] -and
        [bool]$validatedEvidence.crossEpochRejected -and
        $validatedEvidence.contentMismatchRejected -is [bool] -and
        [bool]$validatedEvidence.contentMismatchRejected) `
        'Installed lockstep-v2 raw evidence validation did not prove the complete promoted boundary.'

    Write-Output ("STAGE5_WEEKLY_PROMOTION_ATTESTATION_PASS run={0} source={1} artifactSet={2} qualificationData={3} qualificationDataClosure={4} evidenceClosureAttestation={5} evidenceTree={6}" -f `
        $QualificationRunId, $SourceCommit, $artifactSetSha256,
        $qualificationData.manifestSha256, $qualificationData.closureSha256,
        $AttestationSha256.ToUpperInvariant(), $evidenceTreeSha256)
}

function Write-PromotionJson {
    param([string]$Path, [object]$Value)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 20),
        (New-Object Text.UTF8Encoding($false)))
}

function Write-PromotionX64Fixture {
    param([string]$Path)
    $bytes = New-Object byte[] 256
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int32]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    $bytes[0x84] = 0x64
    $bytes[0x85] = 0x86
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function Assert-PromotionSelfTestRejects {
    param(
        [scriptblock]$Action,
        [string]$Message
    )
    $rejected = $false
    try { & $Action }
    catch { $rejected = $true }
    Assert-PromotionCondition $rejected $Message
}

function Invoke-PromotionSelfTest {
    $genericDictionary = New-Object `
        'Collections.Generic.Dictionary[string,object]'
    $genericDictionary.Add('schemaVersion', [int]1)
    Assert-PromotionJsonShape $genericDictionary @('schemaVersion') `
        'Windows PowerShell generic-dictionary JSON compatibility fixture'

    $root = Join-Path ([IO.Path]::GetTempPath()) `
        ('GGC-WeeklyPromotion-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($root) | Out-Null
    try {
        $sourceCommit = 'a' * 40
        $evidenceRoot = Join-Path $root 'Evidence'
        [IO.Directory]::CreateDirectory($evidenceRoot) | Out-Null
        $closureFiles = @()
        $artifactEntries = @()
        foreach ($title in @('Generals', 'ZeroHour')) {
            $directoryName = if ($title -ceq 'Generals') {
                'GeneralsRuntime'
            }
            else { 'ZeroHourRuntime' }
            $directory = Join-Path $root $directoryName
            [IO.Directory]::CreateDirectory($directory) | Out-Null
            $executableName = if ($title -ceq 'Generals') {
                'generalsv.exe'
            }
            else { 'generalszh.exe' }
            Write-PromotionX64Fixture (Join-Path $directory $executableName)
            [IO.File]::WriteAllText((Join-Path $directory 'launcher.exe'),
                "$title launcher", [Text.Encoding]::UTF8)
            [IO.File]::WriteAllText((Join-Path $directory 'launcher.lcf'),
                "$executableName`n-simulationMode parallel -workerPolicy auto`n",
                [Text.Encoding]::UTF8)
            [IO.File]::WriteAllText((Join-Path $directory 'runtime.dll'),
                "$title dll", [Text.Encoding]::UTF8)
            [IO.File]::WriteAllText((Join-Path $directory 'runtime.big'),
                "$title asset", [Text.Encoding]::UTF8)
            foreach ($fileSpec in @(
                @($executableName, 'executable'),
                @('launcher.exe', 'launcher'),
                @('launcher.lcf', 'launcher-config'),
                @('runtime.dll', 'dll'),
                @('runtime.big', 'asset'))) {
                $relative = "$directoryName/$($fileSpec[0])"
                $closureFiles += [ordered]@{
                    title = $title
                    kind = $fileSpec[1]
                    path = $relative
                    sha256 = Get-PromotionSha256 (Join-Path $root $relative)
                }
            }
            $prefix = if ($title -ceq 'Generals') { 'generals' } else { 'zerohour' }
            $titleEvidenceRoot = Join-Path $evidenceRoot $title
            [IO.Directory]::CreateDirectory($titleEvidenceRoot) | Out-Null
            foreach ($roleSpec in @(
                @('executable', $executableName),
                @('launcher', 'launcher.exe'),
                @('launcher-config', 'launcher.lcf'))) {
                $relative = "$directoryName/$($roleSpec[1])"
                $artifactEntries += [ordered]@{
                    role = "$prefix-$($roleSpec[0])"
                    path = $relative
                    sha256 = Get-PromotionSha256 (Join-Path $root $relative)
                }
            }
        }
        $lines = @($closureFiles | ForEach-Object {
            '{0}|{1}|{2}|{3}' -f $_.title, $_.kind,
                ([string]$_.path).Replace('\', '/'), $_.sha256
        })
        [Array]::Sort($lines, [StringComparer]::Ordinal)
        $closureSha256 = Get-PromotionSha256FromText (($lines -join "`n") + "`n")
        $closureManifestPath = Join-Path $root 'runtime-closure.json'
        Write-PromotionJson $closureManifestPath ([ordered]@{
            schemaVersion = 1; sourceCommit = $sourceCommit
            productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
            files = $closureFiles
        })
        $closureBinding = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'runtime-closure.json'
                sha256 = Get-PromotionSha256 $closureManifestPath
            }
            closureSha256 = $closureSha256
        }
        $artifactSetPath = Join-Path $root 'Stage5ArtifactSet.json'
        Write-PromotionJson $artifactSetPath ([ordered]@{
            schemaVersion = 1; sourceCommit = $sourceCommit
            productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
            runtimeClosure = $closureBinding; artifacts = $artifactEntries
        })
        $artifactSetSha256 = Get-PromotionSha256 $artifactSetPath
        $runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $closureBinding.dependencyManifest.sha256
            closureSha256 = $closureSha256
        }
        $artifactHashes = @{}
        foreach ($entry in $artifactEntries) { $artifactHashes[$entry.role] = $entry.sha256 }
        $mapName = 'Maps\Twilight Flame\Twilight Flame.map'
        $mapCrcs = [ordered]@{
            Generals = [UInt32]739101722
            ZeroHour = [UInt32]4042777579
        }
        $sessions = @()
        foreach ($title in @('Generals', 'ZeroHour')) {
            $prefix = if ($title -ceq 'Generals') { 'generals' } else { 'zerohour' }
            $peers = @()
            foreach ($peerIndex in 0..1) {
                $receiptLeaf = "lockstep-v2-$title-peer-$peerIndex.receipt"
                foreach ($peerFile in @(
                    "peer-$peerIndex.raw.json",
                    $receiptLeaf,
                    "peer-$peerIndex.stdout.log",
                    "peer-$peerIndex.stderr.log")) {
                    [IO.File]::WriteAllText(
                        (Join-Path (Join-Path $evidenceRoot $title) $peerFile),
                        "$title peer $peerIndex $peerFile", [Text.Encoding]::UTF8)
                }
                $peers += [ordered]@{
                    schemaVersion = 2; producer = 'installed-lockstep-v2'
                    validationMode = 'installed-lockstep-v2-production'; title = $title
                    sourceCommit = $sourceCommit
                    executableSha256 = $artifactHashes["$prefix-executable"]
                    peerCount = 2; networkRosterMask = 3; simulationRosterMask = 63
                    aiRosterMask = 60; aiPlayerCount = 4; exitCode = 0
                    finalFrame = 4096; finalCRC = 1; lockstepV2Receipt = $true
                    v1ReceiptAccepted = $false; receiptPath = $receiptLeaf
                }
            }
            $sessions += [ordered]@{
                title = $title; peerCount = 2; networkRosterMask = 3
                simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                mapCrc = $mapCrcs[$title]
                mixedWorkerProof = $true; profileReadOnlyVerified = $true
                effectiveWorkerCounts = @(2, 4); peers = $peers
            }
        }
        $negativeProbes = [ordered]@{}
        foreach ($probe in @(
            @('crossEpoch', 'negative-cross-epoch'),
            @('contentMismatch', 'negative-content-mismatch'))) {
            $negativeProbes[$probe[0]] = @('Generals', 'ZeroHour') | ForEach-Object {
                $title = $_
                $leafPrefix = if ($probe[0] -ceq 'crossEpoch') {
                    'cross-epoch'
                }
                else { 'content-mismatch' }
                $negativeRoot = Join-Path (Join-Path $evidenceRoot $title) `
                    'NegativeProbes'
                [IO.Directory]::CreateDirectory($negativeRoot) | Out-Null
                $proofRelative = "$title/NegativeProbes/$leafPrefix.proof"
                $stdoutRelative = "$title/NegativeProbes/$leafPrefix.stdout.log"
                $stderrRelative = "$title/NegativeProbes/$leafPrefix.stderr.log"
                foreach ($relative in @($proofRelative, $stdoutRelative, $stderrRelative)) {
                    [IO.File]::WriteAllText((Join-Path $evidenceRoot $relative),
                        "$title $($probe[1]) $relative", [Text.Encoding]::UTF8)
                }
                [ordered]@{
                    title = $title; mode = $probe[1]; baselineAccepted = $true
                    mutatedAccepted = $false; exitCode = 0
                    proofPath = $proofRelative; stdoutPath = $stdoutRelative
                    stderrPath = $stderrRelative
                }
            }
        }
        $launcherEquivalence = [ordered]@{}
        foreach ($title in @('Generals', 'ZeroHour')) {
            $prefix = if ($title -ceq 'Generals') { 'generals' } else { 'zerohour' }
            $launcherEquivalence[$title] = [ordered]@{
                launcherSha256 = $artifactHashes["$prefix-launcher"]
                configSha256 = $artifactHashes["$prefix-launcher-config"]
            }
        }
        $evidencePath = Join-Path $evidenceRoot 'LockstepV2LoopbackEvidence.json'
        $evidence = [ordered]@{
            schemaVersion = 2; evidenceKind = 'lockstep-v2-multiplayer'; status = 'passed'
            producer = 'installed-lockstep-v2'; validationMode = 'installed-lockstep-v2-production'
            architecture = 'x64'; sourceCommit = $sourceCommit
            artifactSetSha256 = $artifactSetSha256; recordedUtc = [DateTime]::UtcNow.ToString('o')
            cohortNonce = '01234567-89ab-4cde-8fab-0123456789ab'
            runtimeClosure = $runtimeClosure; allowHeadlessDirectExecution = $true
            launcherEquivalence = $launcherEquivalence; commonStopFrame = 4096
            peerCount = 2; networkRosterMask = 3; simulationRosterMask = 63
            aiRosterMask = 60; aiPlayerCount = 4; mapName = $mapName
            mapCrcs = $mapCrcs; seed = 23063; negativeProbes = $negativeProbes
            v1Accepted = $false; profileStrategy = 'known-folder-registry-redirect'
            registryViews = @('Registry32', 'Registry64'); environmentVariables = @()
            profileConcurrency = 'shared-title-profile-read-only'
            titleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
            sessions = $sessions
        }
        Write-PromotionJson $evidencePath $evidence
        $evidenceSha256 = Get-PromotionSha256 $evidencePath
        $attestationPath = Join-Path $evidenceRoot 'mixed-worker-multiplayer.json'
        Write-PromotionJson $attestationPath ([ordered]@{
            schemaVersion = 1; evidenceKind = 'mixed-worker-multiplayer'; status = 'passed'
            sourceCommit = $sourceCommit; title = 'Both'; architecture = 'x64'
            artifactSetSha256 = $artifactSetSha256; recordedUtc = [DateTime]::UtcNow.ToString('o')
            cohortNonce = $evidence.cohortNonce; runtimeClosure = $runtimeClosure
            attachments = @([ordered]@{
                role = 'multiplayer-results'; path = 'LockstepV2LoopbackEvidence.json'
                sha256 = $evidenceSha256; trustDomain = 'host-runner'
            })
            details = [ordered]@{
                nativeEvidenceKind = 'lockstep-v2-multiplayer'; producer = 'installed-lockstep-v2'
                nativeEvidenceSha256 = $evidenceSha256; networkRosterMask = 3
                simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
                title = 'Both'; sessionCount = 2; peerCount = 2; commonStopFrame = 4096
                allMatchesCompleted = $true; stateTracesIdentical = $true
                crossEpochRejected = $true; contentMismatchRejected = $true
            }
        })
        $qualificationDataFiles = [ordered]@{
            Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
                'Data/Scripts/MultiplayerScripts.scb',
                'Data/Scripts/SkirmishScripts.scb')
            ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
                'Data/Scripts/MultiplayerScripts.scb',
                'Data/Scripts/Scripts.ini',
                'Data/Scripts/SkirmishScripts.scb')
        }
        $qualificationDataEntriesByIdentity = @{}
        foreach ($title in $qualificationDataFiles.Keys) {
            $runtimeLeaf = if ($title -ceq 'Generals') {
                'GeneralsRuntime'
            }
            else { 'ZeroHourRuntime' }
            foreach ($relativeDataPath in $qualificationDataFiles[$title]) {
                $relative = "$runtimeLeaf/$relativeDataPath"
                $identity = "$title|$relative"
                $qualificationDataEntriesByIdentity[$identity] = [ordered]@{
                    title = $title
                    path = $relative
                    sha256 = Get-PromotionSha256FromText `
                        "reviewed fixture $identity"
                }
            }
        }
        [string[]]$qualificationDataIdentities = @(
            $qualificationDataEntriesByIdentity.Keys)
        [Array]::Sort($qualificationDataIdentities, [StringComparer]::Ordinal)
        $qualificationDataEntries = @($qualificationDataIdentities |
            ForEach-Object { $qualificationDataEntriesByIdentity[$_] })
        $qualificationDataLines = @($qualificationDataEntries |
            ForEach-Object {
                '{0}|{1}|{2}' -f $_.title, $_.path, $_.sha256
            })
        $qualificationDataManifestPath = Join-Path $root `
            'Stage5QualificationData.json'
        Write-PromotionJson $qualificationDataManifestPath ([ordered]@{
            schemaVersion = 2
            evidenceKind = 'lockstep-v2-qualification-data'
            producer = 'genci-r2-trimmed-data'
            sourceCommit = $sourceCommit
            productSet = @('Generals', 'ZeroHour')
            mapName = $evidence.mapName
            mapCrcs = $evidence.mapCrcs
            archiveSources = @(
                [ordered]@{
                    title = 'Generals'
                    object = 's3://github-ci/generals108_gamedata_trimmed.7z'
                    sha256 = $generalsQualificationArchiveSha256
                },
                [ordered]@{
                    title = 'ZeroHour'
                    object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
                    sha256 = $zeroHourQualificationArchiveSha256
                })
            files = $qualificationDataEntries
            closureSha256 = Get-PromotionSha256FromText `
                (($qualificationDataLines -join "`n") + "`n")
        })
        [IO.File]::Copy($qualificationDataManifestPath,
            (Join-Path $evidenceRoot $qualificationDataEvidenceLeaf))
        $qualificationDataOriginal = [IO.File]::ReadAllBytes(
            $qualificationDataManifestPath)
        $substitutedQualificationData = ConvertFrom-Stage5JsonDictionary `
            $qualificationDataManifestPath
        $substitutedQualificationData['mapCrcs']['ZeroHour'] =
            $substitutedQualificationData['mapCrcs']['Generals']
        Write-PromotionJson $qualificationDataManifestPath `
            $substitutedQualificationData
        $rejected = $false
        try {
            [void](Assert-PromotionQualificationDataManifest `
                -Path $qualificationDataManifestPath `
                -SourceCommit $sourceCommit `
                -MapName $mapName `
                -MapCrcs $mapCrcs)
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted a substituted title-specific map CRC.'
        [IO.File]::WriteAllBytes($qualificationDataManifestPath,
            $qualificationDataOriginal)
        $evidenceRootFull = [IO.Path]::GetFullPath($evidenceRoot).TrimEnd('\', '/')
        $closureFilesByPath = @{}
        foreach ($file in @(Get-ChildItem -LiteralPath $evidenceRoot -File `
                -Recurse -Force)) {
            $relative = [IO.Path]::GetFullPath($file.FullName).Substring(
                $evidenceRootFull.Length)
            $relative = $relative.TrimStart('\', '/').Replace('\', '/')
            $closureFilesByPath[$relative] = Get-PromotionSha256 $file.FullName
        }
        [string[]]$closurePaths = @($closureFilesByPath.Keys | ForEach-Object {
            [string]$_
        })
        [Array]::Sort($closurePaths, [StringComparer]::Ordinal)
        $closureEntries = @($closurePaths | ForEach-Object {
            [ordered]@{ path = $_; sha256 = [string]$closureFilesByPath[$_] }
        })
        $closureLines = @($closureEntries | ForEach-Object {
            '{0}|{1}' -f $_.path, $_.sha256
        })
        $evidenceClosurePath = Join-Path $evidenceRoot `
            'Stage5LockstepV2EvidenceClosure.json'
        Write-PromotionJson $evidenceClosurePath ([ordered]@{
            schemaVersion = 1; evidenceKind = 'lockstep-v2-evidence-closure'
            producer = 'installed-lockstep-v2'; sourceCommit = $sourceCommit
            artifactSetSha256 = $artifactSetSha256
            cohortNonce = $evidence.cohortNonce
            canonicalEvidenceRoot = $evidenceRootFull
            fileCount = $closureEntries.Count; files = $closureEntries
            closureSha256 = Get-PromotionSha256FromText `
                (($closureLines -join "`n") + "`n")
        })
        $evidenceClosure = ConvertFrom-Stage5JsonDictionary $evidenceClosurePath
        $attestationSha256 = Get-PromotionSha256 $evidenceClosurePath

        # Raw JSON values must remain scalars all the way through the weekly
        # promotion boundary.  These mutations intentionally exercise both
        # collection coercion shapes and non-string/non-integer scalar kinds.
        $qualificationDataScalarMutations = @(
            [pscustomobject]@{
                Name = 'extra-element evidence-kind array'
                Field = 'evidenceKind'
                Value = [object[]]@('lockstep-v2-qualification-data', 'extra')
            },
            [pscustomobject]@{
                Name = 'single-element evidence-kind array'
                Field = 'evidenceKind'
                Value = [object[]]@('lockstep-v2-qualification-data')
            },
            [pscustomobject]@{
                Name = 'wrong evidence-kind scalar'
                Field = 'evidenceKind'
                Value = [int]1
            },
            [pscustomobject]@{
                Name = 'fractional schema-version scalar'
                Field = 'schemaVersion'
                Value = [double]2.5
            }
        )
        $qualificationDataMutationOriginal = [IO.File]::ReadAllBytes(
            $qualificationDataManifestPath)
        foreach ($mutation in $qualificationDataScalarMutations) {
            $mutatedQualificationData = ConvertFrom-Stage5JsonDictionary `
                $qualificationDataManifestPath
            $mutatedQualificationData[$mutation.Field] = $mutation.Value
            Write-PromotionJson $qualificationDataManifestPath `
                $mutatedQualificationData
            try {
                Assert-PromotionSelfTestRejects {
                    [void](Assert-PromotionQualificationDataManifest `
                        -Path $qualificationDataManifestPath `
                        -SourceCommit $sourceCommit `
                        -MapName $mapName `
                        -MapCrcs $mapCrcs)
                } "Weekly promotion self-test accepted $($mutation.Name)."
            }
            finally {
                [IO.File]::WriteAllBytes($qualificationDataManifestPath,
                    $qualificationDataMutationOriginal)
            }
        }

        $originalEvidenceKind = $evidence['evidenceKind']
        $evidenceKindMutations = @(
            [pscustomobject]@{
                Name = 'extra-element evidence-kind array'
                Value = [object[]]@('lockstep-v2-multiplayer', 'extra')
            },
            [pscustomobject]@{
                Name = 'single-element evidence-kind array'
                Value = [object[]]@('lockstep-v2-multiplayer')
            },
            [pscustomobject]@{ Name = 'wrong evidence-kind scalar'; Value = [int]1 }
        )
        foreach ($mutation in $evidenceKindMutations) {
            $evidence['evidenceKind'] = $mutation.Value
            try {
                Assert-PromotionSelfTestRejects {
                    [void](Assert-PromotionEvidence $evidence $evidenceSha256 `
                        $sourceCommit $artifactSetSha256 $runtimeClosure $artifacts)
                } "Weekly promotion self-test accepted $($mutation.Name)."
            }
            finally { $evidence['evidenceKind'] = $originalEvidenceKind }
        }

        $originalClosurePath = $evidenceClosure['files'][0]['path']
        $closurePathMutations = @(
            [pscustomobject]@{
                Name = 'extra-element evidence-closure path array'
                Value = [object[]]@($originalClosurePath, 'extra')
            },
            [pscustomobject]@{
                Name = 'single-element evidence-closure path array'
                Value = [object[]]@($originalClosurePath)
            },
            [pscustomobject]@{ Name = 'wrong evidence-closure path scalar'; Value = [int]1 }
        )
        foreach ($mutation in $closurePathMutations) {
            $evidenceClosure['files'][0]['path'] = $mutation.Value
            try {
                Assert-PromotionSelfTestRejects {
                    [void](Assert-PromotionEvidenceTree `
                        (Join-Path $root 'Evidence') $evidence $evidenceClosure `
                        $sourceCommit $artifactSetSha256 $evidence.cohortNonce)
                } "Weekly promotion self-test accepted $($mutation.Name)."
            }
            finally { $evidenceClosure['files'][0]['path'] = $originalClosurePath }
        }

        $evidenceValidationProbe = @{ invoked = $false }
        $evidenceValidationAction = {
            param([object]$Context)
            Assert-PromotionCondition ([string]$Context.path -ceq $evidencePath -and
                [string]$Context.sourceCommit -ceq $sourceCommit -and
                [string]$Context.artifactSetSha256 -ceq $artifactSetSha256 -and
                [string]$Context.evidenceSha256 -ceq $evidenceSha256 -and
                [string]$Context.cohortNonce -ceq [string]$evidence.cohortNonce -and
                [string]$Context.runtimeClosure.dependencyManifestSha256 -ceq
                    [string]$runtimeClosure.dependencyManifestSha256 -and
                [string]$Context.runtimeClosure.closureSha256 -ceq
                    [string]$runtimeClosure.closureSha256 -and
                $Context.artifactHashes.Count -eq 6 -and
                $Context.artifactPaths.Count -eq 6) `
                'Weekly promotion self-test did not bind the full evidence reader to the exact artifact set.'
            $evidenceValidationProbe.invoked = $true
            return [pscustomobject]@{
                schemaVersion = 2
                evidenceKind = 'lockstep-v2-multiplayer'
                producer = 'installed-lockstep-v2'
                sourceCommit = $sourceCommit
                artifactSetSha256 = $artifactSetSha256
                cohortNonce = [string]$evidence.cohortNonce
                commonStopFrame = 4096
                peerCount = 2
                mapName = $mapName
                mapCrcs = $mapCrcs
                sessions = @([pscustomobject]@{}, [pscustomobject]@{})
                crossEpochRejected = $true
                contentMismatchRejected = $true
            }
        }
        Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
            $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        Assert-PromotionCondition ([bool]$evidenceValidationProbe.invoked) `
            'Weekly promotion self-test did not invoke the full raw-evidence validation boundary.'

        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit ('F' * 64) '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted an independently mismatched evidence-closure hash.'

        $unexpectedEvidence = Join-Path $evidenceRoot 'undeclared.tmp'
        [IO.File]::WriteAllText($unexpectedEvidence, 'undeclared', [Text.Encoding]::UTF8)
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted an undeclared evidence-tree file.'
        Remove-Item -LiteralPath $unexpectedEvidence -Force

        $rawEvidencePath = Join-Path $evidenceRoot 'Generals\peer-0.raw.json'
        $originalRawEvidenceBytes = [IO.File]::ReadAllBytes($rawEvidencePath)
        [IO.File]::AppendAllText($rawEvidencePath, 'tampered')
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted a changed declared raw-evidence member.'
        [IO.File]::WriteAllBytes($rawEvidencePath, $originalRawEvidenceBytes)

        $missingEvidencePath = Join-Path $evidenceRoot 'ZeroHour\peer-1.stderr.log'
        $missingEvidenceBytes = [IO.File]::ReadAllBytes($missingEvidencePath)
        Remove-Item -LiteralPath $missingEvidencePath -Force
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted a missing declared evidence-tree member.'
        [IO.File]::WriteAllBytes($missingEvidencePath, $missingEvidenceBytes)

        $originalEvidenceBytes = [IO.File]::ReadAllBytes($evidencePath)
        [IO.File]::AppendAllText($evidencePath, 'tampered')
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted native evidence detached from its attestation.'
        [IO.File]::WriteAllBytes($evidencePath, $originalEvidenceBytes)

        $originalQualificationDataBytes = [IO.File]::ReadAllBytes(
            $qualificationDataManifestPath)
        [IO.File]::AppendAllText($qualificationDataManifestPath, 'tampered')
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted qualification data detached from its evidence closure.'
        [IO.File]::WriteAllBytes($qualificationDataManifestPath,
            $originalQualificationDataBytes)

        $runtimeDll = Join-Path $root 'GeneralsRuntime\runtime.dll'
        [IO.File]::AppendAllText($runtimeDll, 'tampered')
        $rejected = $false
        try {
            Invoke-PromotionValidation $root $sourceCommit $attestationSha256 '1' `
                $evidenceValidationAction -AllowNonCanonicalSelfTestRoot | Out-Null
        }
        catch { $rejected = $true }
        Assert-PromotionCondition $rejected `
            'Weekly promotion self-test accepted a changed published runtime file.'
        Write-Output 'Stage 5 weekly promotion attestation self-test passed.'
    }
    finally {
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
}

if ($SelfTest) {
    Invoke-PromotionSelfTest
    return
}

Invoke-PromotionValidation $BundleRoot $ExpectedSourceCommit `
    $ExpectedAttestationSha256 $ExpectedQualificationRunId
