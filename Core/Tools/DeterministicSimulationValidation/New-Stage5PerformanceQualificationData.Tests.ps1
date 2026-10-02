param(
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Rejects {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )
    $errorText = ''
    try { & $Action }
    catch { $errorText = $_.Exception.Message }
    Assert-True ($errorText -match $Pattern) `
        "$Message (got '$errorText')"
}

function Get-Sha256 {
    param([string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '')).ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Invoke-SevenZip {
    param([string]$SevenZipPath, [string[]]$Arguments)
    $captured = @(& $SevenZipPath @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip fixture command failed with exit code ${LASTEXITCODE}: $($captured -join ' ')"
    }
}

function New-QualificationSource {
    param([string]$Root, [switch]$OmitRequiredFile)
    $dataScripts = Join-Path $Root 'Data\Scripts'
    New-Item -ItemType Directory -Path $dataScripts -Force | Out-Null
    $files = [ordered]@{
        'INIZH.big' = 'synthetic ini'
        'MapsZH.big' = 'synthetic maps'
        'W3DZH.big' = 'synthetic w3d'
        'Art.big' = 'synthetic art'
        'Data\Scripts\MultiplayerScripts.scb' = 'synthetic multiplayer'
        'Data\Scripts\Scripts.ini' = 'synthetic scripts ini'
        'Data\Scripts\SkirmishScripts.scb' = 'synthetic skirmish'
        'Data\INI\GameData.ini' = 'synthetic game data'
    }
    foreach ($relative in $files.Keys) {
        if ($OmitRequiredFile -and $relative -ceq 'MapsZH.big') { continue }
        $path = Join-Path $Root $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, [string]$files[$relative])
    }
}

function New-ProducerCopy {
    param([string]$ProducerPath, [string]$ModulePath, [string]$Destination,
        [string]$ArchiveHash)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $copyPath = Join-Path $Destination 'New-Stage5PerformanceQualificationData.ps1'
    $copyText = (Get-Content -LiteralPath $ProducerPath -Raw).Replace(
        '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
        $ArchiveHash)
    [IO.File]::WriteAllText($copyPath, $copyText)
    Copy-Item -LiteralPath $ModulePath -Destination (
        Join-Path $Destination 'DeterministicSimulationEvidence.psm1')
    return $copyPath
}

function Invoke-Producer {
    param([string]$ProducerPath, [string]$ArchivePath, [string]$ExpectedArchiveHash,
        [string]$TaskRoot, [string]$RuntimeRoot, [string]$OutputPath,
        [string]$SevenZipPath)
    & $ProducerPath -ArchivePath $ArchivePath `
        -ExpectedArchiveSha256 $ExpectedArchiveHash `
        -TaskRoot $TaskRoot -RuntimeRoot $RuntimeRoot -OutputPath $OutputPath `
        -ExpectedSourceCommit ('a' * 40) -SevenZipPath $SevenZipPath | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'qualification-data producer returned a nonzero exit code'
}

$producerPath = Join-Path $PSScriptRoot 'New-Stage5PerformanceQualificationData.ps1'
$modulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
Assert-True (Test-Path -LiteralPath $producerPath -PathType Leaf) `
    'qualification-data producer script is missing'
Assert-True (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    'validation evidence module is missing'

$sevenZipCommand = Get-Command '7z.exe' -ErrorAction SilentlyContinue
$sevenZipPath = if ($null -ne $sevenZipCommand) { $sevenZipCommand.Source } else {
    'C:\Program Files\7-Zip\7z.exe'
}
Assert-True (Test-Path -LiteralPath $sevenZipPath -PathType Leaf) `
    'focused producer tests require the reviewed 7z.exe fixture tool'

$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath($ScratchRoot)
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    [IO.Path]::GetFullPath($env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)
}
else {
    throw 'New-Stage5PerformanceQualificationData.Tests.ps1 requires an explicit H: scratch root.'
}
Assert-True ($scratchParent.StartsWith('H:\', [StringComparison]::OrdinalIgnoreCase)) `
    'producer test scratch must remain on H:'
$testRoot = Join-Path $scratchParent ('stage5-qualification-data-' +
    [Guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

    $producerSource = Get-Content -LiteralPath $producerPath -Raw
    foreach ($requiredText in @(
        '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
        's3://github-ci/zerohour104_gamedata_trimmed.7z',
        'genci-r2-trimmed-data-v1',
        'stage5-performance-qualification-data',
        'FileShare]::Read',
        'Symbolic Link',
        'Data/'
    )) {
        Assert-True ($producerSource.Contains($requiredText)) `
            "producer source omitted required trust boundary '$requiredText'"
    }
    Assert-Rejects {
        & $producerPath -ArchivePath (Join-Path $testRoot 'missing.7z') `
            -ExpectedArchiveSha256 ('0' * 64) `
            -TaskRoot $testRoot -RuntimeRoot (Join-Path $testRoot 'runtime') `
            -OutputPath (Join-Path $testRoot 'Stage5PerformanceQualificationData.json') `
            -ExpectedSourceCommit ('a' * 40) -SevenZipPath $sevenZipPath
    } 'fixed.*archive|reviewed.*archive|ExpectedArchiveSha256' `
        'producer must reject an override of the fixed reviewed archive SHA-256'
    Write-Output 'PASS: fixed provenance and archive-lock source contract'

    $source = Join-Path $testRoot 'source'
    New-QualificationSource $source
    $archive = Join-Path $testRoot 'zerohour104_gamedata_trimmed.7z'
    Invoke-SevenZip $sevenZipPath @('a', '-t7z', '-mx=1', $archive,
        (Join-Path $source 'INIZH.big'), (Join-Path $source 'MapsZH.big'),
        (Join-Path $source 'W3DZH.big'), (Join-Path $source 'Art.big'),
        (Join-Path $source 'Data'))
    $archiveHash = Get-Sha256 $archive
    $taskRoot = Join-Path $testRoot 'task'
    $runtimeRoot = Join-Path $testRoot 'installed\ZeroHourRuntime'
    New-Item -ItemType Directory -Path $taskRoot, $runtimeRoot -Force | Out-Null
    foreach ($productFile in @('generalszh.exe', 'launcher.exe', 'launcher.lcf', 'runtime.dll')) {
        [IO.File]::WriteAllText((Join-Path $runtimeRoot $productFile), "product $productFile")
    }
    $producerCopy = New-ProducerCopy $producerPath $modulePath `
        (Join-Path $testRoot 'producer-copy') $archiveHash
    $outputPath = Join-Path $taskRoot 'Stage5PerformanceQualificationData.json'
    Invoke-Producer $producerCopy $archive $archiveHash $taskRoot $runtimeRoot `
        $outputPath $sevenZipPath
    $document = Get-Content -LiteralPath $outputPath -Raw | ConvertFrom-Json
    Assert-True ($document.schemaVersion -eq 1 -and
        $document.evidenceKind -ceq 'stage5-performance-qualification-data' -and
        $document.producer -ceq 'genci-r2-trimmed-data-v1' -and
        $document.sourceCommit -ceq ('a' * 40) -and
        $document.title -ceq 'ZeroHour' -and
        $document.archiveSource.object -ceq 's3://github-ci/zerohour104_gamedata_trimmed.7z' -and
        $document.archiveSource.sha256 -ceq $archiveHash -and
        [IO.Path]::GetFullPath($document.runtimeRoot) -ceq
            [IO.Path]::GetFullPath($runtimeRoot)) `
        'successful producer output omitted exact identity/provenance binding'
    $entries = @($document.files)
    Assert-True ($entries.Count -eq 8) `
        'successful producer output must collect every root .big and Data/** file'
    $previous = $null
    foreach ($entry in $entries) {
        Assert-True ($entry.path -notmatch '\\' -and
            ($null -eq $previous -or [StringComparer]::Ordinal.Compare(
                [string]$previous, [string]$entry.path) -lt 0) -and
            $entry.sha256 -cmatch '^[0-9A-F]{64}$' -and
            [int64]$entry.length -gt 0) `
            'producer output paths/hashes/lengths must be canonical and ordinal-sorted'
        $destination = Join-Path $runtimeRoot ([string]$entry.path)
        Assert-True ((Get-Sha256 $destination) -ceq $entry.sha256 -and
            (Get-Item -LiteralPath $destination).Length -eq [int64]$entry.length) `
            "producer destination did not self-validate: $($entry.path)"
        $previous = $entry.path
    }
    Assert-True ($document.files.path -contains 'INIZH.big' -and
        $document.files.path -contains 'MapsZH.big' -and
        $document.files.path -contains 'W3DZH.big' -and
        $document.files.path -contains 'Data/Scripts/MultiplayerScripts.scb' -and
        $document.files.path -contains 'Data/Scripts/Scripts.ini' -and
        $document.files.path -contains 'Data/Scripts/SkirmishScripts.scb') `
        'producer output omitted one of the six mandatory Zero Hour paths'
    $canonicalText = (($entries | ForEach-Object {
        '{0}|{1}|{2}' -f $_.path, $_.sha256, [int64]$_.length
    }) -join "`n") + "`n"
    Assert-True ($document.closureSha256 -cmatch '^[0-9A-F]{64}$') `
        'producer closure must be a canonical uppercase SHA-256'
    # Recompute the closure independently without relying on producer helpers.
    $closureBytes = [Text.Encoding]::UTF8.GetBytes($canonicalText)
    $closureSha = [Security.Cryptography.SHA256]::Create()
    try {
        $expectedClosure = ([BitConverter]::ToString($closureSha.ComputeHash($closureBytes)) -replace '-', '').ToUpperInvariant()
    }
    finally { $closureSha.Dispose() }
    Assert-True ($document.closureSha256 -ceq $expectedClosure) `
        'producer closure does not match UTF-8 LF-final path|sha256|length lines'
    # The archive must no longer be held after the producer returns.
    $archiveProbe = [IO.File]::Open($archive, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $archiveProbe.Dispose()
    Assert-True (@(Get-ChildItem -LiteralPath $taskRoot -Force).Count -eq 1 -and
        (Get-Item -LiteralPath $outputPath).Name -ceq
            'Stage5PerformanceQualificationData.json') `
        'producer must not retain an extracted staging directory in the task root'
    Write-Output 'PASS: synthetic archive extraction, destination copy, and closure'

    $preexistingRuntime = Join-Path $testRoot 'preexisting-runtime'
    New-Item -ItemType Directory -Path $preexistingRuntime -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $preexistingRuntime 'INIZH.big'), 'preexisting')
    Assert-Rejects {
        Invoke-Producer $producerCopy $archive $archiveHash $taskRoot `
            $preexistingRuntime (Join-Path $taskRoot 'preexisting-output.json') $sevenZipPath
    } 'pre-existing|already exists|qualification-data' `
        'producer must reject a preexisting qualification-data destination'
    Assert-Rejects {
        Invoke-Producer $producerCopy $archive $archiveHash $taskRoot `
            $runtimeRoot (Join-Path $testRoot 'outside-output.json') $sevenZipPath
    } 'under.*task|contained|OutputPath' `
        'producer must reject an output path outside its task root'
    Write-Output 'PASS: fresh destination and task-root containment guards'

    $missingSource = Join-Path $testRoot 'missing-source'
    New-QualificationSource $missingSource -OmitRequiredFile
    $missingArchive = Join-Path $testRoot 'missing-required.7z'
    Invoke-SevenZip $sevenZipPath @('a', '-t7z', '-mx=1', $missingArchive,
        (Join-Path $missingSource 'INIZH.big'), (Join-Path $missingSource 'W3DZH.big'),
        (Join-Path $missingSource 'Art.big'), (Join-Path $missingSource 'Data'))
    $missingHash = Get-Sha256 $missingArchive
    $missingProducer = New-ProducerCopy $producerPath $modulePath `
        (Join-Path $testRoot 'missing-producer-copy') $missingHash
    Assert-Rejects {
        Invoke-Producer $missingProducer $missingArchive $missingHash `
            $taskRoot $runtimeRoot (Join-Path $taskRoot 'missing-output.json') $sevenZipPath
    } 'required|MapsZH\.big|mandatory' `
        'producer must reject an archive missing a mandatory Zero Hour file'
    Write-Output 'PASS: required-path completeness guard'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
