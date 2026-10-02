Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1')

function Resolve-Stage5BaseRegularPath {
    param([string]$Path, [ValidateSet('Leaf','Container')][string]$PathType)
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'An explicit base runtime path is required.' }
    $absolute = [IO.Path]::GetFullPath($Path)
    $full = $absolute.TrimEnd('\','/')
    if ($Path -match '^[A-Za-z]:$' -or [string]::Equals($full,
        [IO.Path]::GetPathRoot($absolute).TrimEnd('\','/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A volume or share root is not an installation/source directory.'
    }
    if (-not (Test-Path -LiteralPath $full -PathType $PathType)) { throw "Base runtime $PathType is missing: $full" }
    $current = Get-Item -LiteralPath $full -Force
    while ($null -ne $current) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Base runtime path contains a reparse point: $full" }
        $current = if ($current -is [IO.DirectoryInfo]) { $current.Parent } else { $current.Directory }
    }
    return $full
}

function Get-Stage5BaseGeneralsBinding {
    param(
        [string]$RuntimeRoot, [string]$GeneralsInstallRoot,
        [string]$AcceptanceSourceCommit = '', [string]$AcceptanceArtifactSetPath = '',
        [string]$AcceptanceArtifactSetSha256 = '',
        [string]$AcceptanceRuntimeDependencyManifestSha256 = '',
        [string]$AcceptanceRuntimeClosureSha256 = '',
        [string]$GeneralsQualificationDataManifestPath = '',
        [string]$GeneralsQualificationDataManifestSha256 = ''
    )
    $base = Resolve-Stage5BaseRegularPath $GeneralsInstallRoot Container
    $runtime = Resolve-Stage5BaseRegularPath $RuntimeRoot Container
    if ([string]::Equals($base, $runtime, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Zero Hour requires a distinct, explicit paired Generals runtime.'
    }
    $files = @(
        foreach ($name in @('English.big','INI.big','Maps.big','W3D.big')) {
            $file = Resolve-Stage5BaseRegularPath (Join-Path $base $name) Leaf
            [ordered]@{ path = $name; sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
        }
    )
    $binding = [ordered]@{ identityMode = 'diagnostic'; runtimeRoot = $base; files = $files }
    $acceptanceValues = @($AcceptanceSourceCommit, $AcceptanceArtifactSetPath,
        $AcceptanceArtifactSetSha256, $AcceptanceRuntimeDependencyManifestSha256,
        $AcceptanceRuntimeClosureSha256, $GeneralsQualificationDataManifestPath,
        $GeneralsQualificationDataManifestSha256)
    if (@($acceptanceValues | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) { return $binding }
    if (@($acceptanceValues | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) { throw 'Paired acceptance binding is incomplete.' }
    $artifactPath = Resolve-Stage5BaseRegularPath $AcceptanceArtifactSetPath Leaf
    $artifact = Read-Stage5BaseJsonSnapshot $artifactPath 'Paired artifact set' $AcceptanceArtifactSetSha256
    if (-not (Test-Stage5JsonInteger $artifact.schemaVersion) -or $artifact.schemaVersion -ne 1 -or
        $artifact.sourceCommit -cne $AcceptanceSourceCommit -or $artifact.architecture -cne 'x64' -or
        @($artifact.productSet).Count -ne 2 -or @($artifact.productSet) -cnotcontains 'Generals' -or
        @($artifact.productSet) -cnotcontains 'ZeroHour') { throw 'Paired artifact-set identity mismatch.' }
    $closure = Get-Stage5RuntimeClosureBinding -ArtifactSet $artifact `
        -ArtifactDirectory (Split-Path -Parent $artifactPath) -ExpectedSourceCommit $AcceptanceSourceCommit
    if ($closure.dependencyManifestSha256 -cne $AcceptanceRuntimeDependencyManifestSha256.ToUpperInvariant() -or
        $closure.closureSha256 -cne $AcceptanceRuntimeClosureSha256.ToUpperInvariant()) { throw 'Paired runtime closure differs from acceptance.' }
    foreach ($title in @('Generals','ZeroHour')) {
        $executable = @($closure.files | Where-Object { $_.title -ceq $title -and $_.kind -ceq 'executable' })
        $expectedRoot = if ($title -ceq 'Generals') { $base } else { $runtime }
        if ($executable.Count -ne 1 -or -not [string]::Equals(
            (Split-Path -Parent $executable[0].fullPath), $expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Supplied $title root is not the acceptance artifact-role directory."
        }
    }
    $qualificationPath = Resolve-Stage5BaseRegularPath $GeneralsQualificationDataManifestPath Leaf
    $qualification = Read-Stage5BaseJsonSnapshot $qualificationPath 'Base qualification manifest' $GeneralsQualificationDataManifestSha256
    $qualificationBinding = [ordered]@{
        path = 'QualificationData.json'; title = 'Generals'
        manifestSha256 = $GeneralsQualificationDataManifestSha256.ToUpperInvariant()
        closureSha256 = $qualification.closureSha256; fileCount = @($qualification.files).Count
    }
    $evidence = Read-Stage5SimulationQualificationDataEvidence -Path $qualificationPath `
        -Binding $qualificationBinding -ExpectedSourceCommit $AcceptanceSourceCommit -ExpectedTitle Generals
    foreach ($entry in $evidence.files) {
        $file = Resolve-Stage5BaseContainedFile $base $entry.path
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -cne $entry.sha256) { throw 'Paired base qualification file hash mismatch.' }
    }
    $binding.identityMode = 'acceptance-bound'
    $binding.sourceCommit = $AcceptanceSourceCommit
    $binding.artifactSetSha256 = $AcceptanceArtifactSetSha256.ToUpperInvariant()
    $binding.runtimeClosure = [ordered]@{ dependencyManifestSha256 = $closure.dependencyManifestSha256; closureSha256 = $closure.closureSha256 }
    $binding.qualificationData = $qualificationBinding
    $binding.files = @($evidence.files)
    Assert-Stage5BaseGeneralsBindingCurrent $binding
    return $binding
}

function Test-Stage5RegistryScopeInactive {
    param([string[]]$ExecutablePaths, [scriptblock]$ProcessProvider = {
        param($names)
        $activityErrors = @()
        $processes = @(Get-Process -Name $names -ErrorAction SilentlyContinue -ErrorVariable activityErrors)
        foreach ($activityError in $activityErrors) {
            if ($activityError.CategoryInfo.Category -ne [Management.Automation.ErrorCategory]::ObjectNotFound) {
                throw $activityError
            }
        }
        $processes
    })
    $names = @(@('generals','generalsv','generalszh','game') + @($ExecutablePaths | ForEach-Object {
        [IO.Path]::GetFileNameWithoutExtension($_)
    }) | Sort-Object -Unique)
    try {
        # Registry keys are global to each title, not to this candidate path.
        # Any matching title process (including a foreign install) is active.
        return @(& $ProcessProvider $names).Count -eq 0
    }
    catch { return $false }
}

function Assert-Stage5BaseGeneralsBindingCurrent {
    param([object]$Binding)
    $root = Resolve-Stage5BaseRegularPath $Binding.runtimeRoot Container
    foreach ($entry in $Binding.files) {
        $path = Resolve-Stage5BaseContainedFile $root $entry.path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $entry.sha256) { throw 'Base Generals file changed after binding.' }
    }
    if ($Binding.identityMode -ceq 'acceptance-bound') {
        $actual = @(@(Get-ChildItem -LiteralPath $root -File -Force | Where-Object Extension -ieq '.big') +
            @(if (Test-Path -LiteralPath (Join-Path $root 'Data')) {
                Get-ChildItem -LiteralPath (Join-Path $root 'Data') -Recurse -File -Force
            }) | ForEach-Object {
                [void](Resolve-Stage5BaseRegularPath $_.FullName Leaf)
                $_.FullName.Substring($root.Length + 1).Replace('\','/')
            } | Sort-Object)
        $expected = @($Binding.files | ForEach-Object { $_.path } | Sort-Object)
        if (($actual -join "`n") -cne ($expected -join "`n")) { throw 'Base Generals qualification membership contains missing or undeclared data.' }
    }
}

function Get-Stage5ReplayRuntimeLayout {
    param([string]$SourceRoot, [string]$AcceptanceManifestPath,
        [string]$SourceCommit, [ValidateSet('Generals','ZeroHour')][string]$Title)
    $source = Resolve-Stage5BaseRegularPath $SourceRoot Container
    if ($SourceCommit -cnotmatch '^[0-9a-f]{40}$') { throw 'Replay layout requires a canonical source commit.' }
    $manifestPath = Resolve-Stage5BaseRegularPath $AcceptanceManifestPath Leaf
    if (-not $manifestPath.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Acceptance manifest escapes the checkout.' }
    $manifest = Read-Stage5BaseJsonSnapshot $manifestPath 'Replay acceptance manifest'
    if ($manifest.sourceCommit -cne $SourceCommit) { throw 'Replay layout source identity is stale.' }
    $artifactPath = Resolve-Stage5BaseContainedFile (Split-Path -Parent $manifestPath) $manifest.artifactSet.path
    $artifact = Read-Stage5BaseJsonSnapshot $artifactPath 'Replay layout artifact set' $manifest.artifactSet.sha256
    if ($artifact.sourceCommit -cne $SourceCommit) { throw 'Replay artifact source identity is stale.' }
    $roots = @{}
    foreach ($role in @('generals-executable','zerohour-executable')) {
        $entries = @($artifact.artifacts | Where-Object { $_.role -ceq $role })
        if ($entries.Count -ne 1) { throw "Replay layout requires exactly one $role." }
        $relative = [string]$entries[0].path
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)|:|[\x00-\x1F\x7F]') { throw 'Unsafe replay artifact path.' }
        $root = Split-Path -Parent ([IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $artifactPath) $relative)))
        if (-not $root.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Replay runtime directory escapes the checkout.' }
        $ancestor = $root
        while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor = Split-Path -Parent $ancestor }
        [void](Resolve-Stage5BaseRegularPath $ancestor Container)
        if (Test-Path -LiteralPath $root) {
            if (@(Get-ChildItem -LiteralPath $root -Force).Count -ne 0) { throw 'Replay artifact destination is not empty.' }
        }
        $roots[$role] = $root
    }
    if ([string]::Equals($roots['generals-executable'], $roots['zerohour-executable'], [StringComparison]::OrdinalIgnoreCase)) { throw 'Paired runtime directories must be distinct.' }
    return [ordered]@{
        runtimeRoot = if ($Title -ceq 'Generals') { $roots['generals-executable'] } else { $roots['zerohour-executable'] }
        generalsRuntimeRoot = $roots['generals-executable']
        zeroHourRuntimeRoot = $roots['zerohour-executable']
    }
}

function Resolve-Stage5BaseContainedFile {
    param([string]$Root, [string]$Relative)
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '(^|[\\/])\.\.([\\/]|$)|:|[\x00-\x1F\x7F]') { throw 'Unsafe base binding relative path.' }
    $rootFull = Resolve-Stage5BaseRegularPath $Root Container
    $full = [IO.Path]::GetFullPath((Join-Path $rootFull $Relative))
    if (-not $full.StartsWith($rootFull + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Base binding file escapes its root.' }
    return Resolve-Stage5BaseRegularPath $full Leaf
}

function Read-Stage5BaseJsonSnapshot {
    param([string]$Path, [string]$Context, [string]$ExpectedSha256 = '')
    $snapshot = Get-Stage5FinalAcceptanceFileSnapshot $Path $Context
    if ($PSBoundParameters.ContainsKey('ExpectedSha256')) {
        Assert-Stage5FinalAcceptanceSnapshotSha256 $snapshot $ExpectedSha256 $Context | Out-Null
    }
    return ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $snapshot $Context
}

Export-ModuleMember -Function Get-Stage5BaseGeneralsBinding, Test-Stage5RegistryScopeInactive, Get-Stage5ReplayRuntimeLayout, Assert-Stage5BaseGeneralsBindingCurrent
