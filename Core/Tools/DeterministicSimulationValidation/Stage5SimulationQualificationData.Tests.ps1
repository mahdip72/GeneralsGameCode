[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScratchRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    $caught = $null
    try { & $Action } catch { $caught = $_ }
    Assert-Test ($null -ne $caught -and
        $caught.Exception.Message -match $Pattern) `
        "$Message (got '$($caught.Exception.Message)')"
}

function Get-TestSha256 {
    param([string]$Path)
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

function Get-TestTextSha256 {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) |
            ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Write-TestText {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text,
        (New-Object Text.UTF8Encoding($false)))
}

function Write-TestJson {
    param([string]$Path, [object]$Value)
    Write-TestText $Path ($Value | ConvertTo-Json -Depth 20)
}

function New-SimulationQualificationFixture {
    param(
        [string]$Root,
        [string]$SourceCommit,
        [ValidateSet('Generals', 'ZeroHour')][string]$Title = 'ZeroHour'
    )
    $runtimeRoot = Join-Path $Root 'runtime'
    [IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
    $relativePaths = if ($Title -ceq 'Generals') {
        @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
    }
    else {
        @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)
    $entries = @()
    $index = 0
    foreach ($relative in $relativePaths) {
        ++$index
        $path = Join-Path $runtimeRoot $relative
        Write-TestText $path "qualification-$Title-$index"
        $entries += [ordered]@{
            path = $relative.Replace('\', '/')
            sha256 = Get-TestSha256 $path
        }
    }
    $canonical = (@($entries | ForEach-Object {
        '{0}|{1}' -f $_.path, $_.sha256
    }) -join "`n") + "`n"
    $closureSha256 = Get-TestTextSha256 $canonical
    $archive = if ($Title -ceq 'Generals') {
        [ordered]@{
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        }
    }
    else {
        [ordered]@{
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
    }
    $document = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-simulation-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        title = $Title
        archiveSource = $archive
        files = $entries
        closureSha256 = $closureSha256
    }
    $manifestPath = Join-Path $Root 'QualificationData.json'
    Write-TestJson $manifestPath $document
    return [pscustomobject]@{
        runtimeRoot = $runtimeRoot
        manifestPath = $manifestPath
        document = $document
        binding = [ordered]@{
            path = 'QualificationData.json'
            title = $Title
            manifestSha256 = Get-TestSha256 $manifestPath
            closureSha256 = $closureSha256
            fileCount = $entries.Count
        }
    }
}

function Set-FixtureDocument {
    param([object]$Fixture)
    Write-TestJson $Fixture.manifestPath $Fixture.document
    $Fixture.binding.manifestSha256 = Get-TestSha256 $Fixture.manifestPath
}

$scratch = [IO.Path]::GetFullPath($ScratchRoot)
Assert-Test ($scratch.StartsWith('H:\',
    [StringComparison]::OrdinalIgnoreCase)) `
    'Simulation qualification-data tests require task-owned H: scratch.'
$testRoot = Join-Path $scratch ('stage5-simulation-data-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))
Assert-Test (-not (Test-Path -LiteralPath $testRoot)) `
    "Test scratch already exists: $testRoot"
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

$producerPath = Join-Path $PSScriptRoot `
    'Install-Stage5SimulationQualificationData.ps1'
$runnerPath = Join-Path $PSScriptRoot `
    'Run-DeterministicSimulationValidation.ps1'
$modulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
$workflowPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot `
    '..\..\..\.github\workflows\check-replays.yml'))

try {
    Assert-Test (Test-Path -LiteralPath $producerPath -PathType Leaf) `
        'The checked-in simulation qualification-data producer is missing.'
    $tokens = $null
    $errors = $null
    $producerAst = [Management.Automation.Language.Parser]::ParseFile(
        $producerPath, [ref]$tokens, [ref]$errors)
    Assert-Test ($errors.Count -eq 0) `
        "Simulation qualification-data producer must parse: $($errors -join '; ')"
    $producerParameters = @($producerAst.ParamBlock.Parameters |
        ForEach-Object { $_.Name.VariablePath.UserPath })
    Assert-Test (($producerParameters -join '|') -ceq
        'RuntimeRoot|TaskRoot|SourceCommit|Title|AwsEndpointUrl|OutputEnvironmentFile') `
        "Producer parameter contract changed: $($producerParameters -join ', ')"

    $workflowText = [IO.File]::ReadAllText($workflowPath)
    Assert-Test ($workflowText -notmatch 'actions/cache@' -and
        $workflowText -notmatch 'gamedata-permanent-cache-v4' -and
        $workflowText -notmatch 'cache-gamedata|cache-hit') `
        'Stage 5 replay qualification must never consume mutable game-data cache state.'
    Assert-Test (@([regex]::Matches($workflowText,
        'Install-Stage5SimulationQualificationData\.ps1')).Count -eq 1) `
        'check-replays must invoke the checked-in qualification-data producer exactly once.'
    foreach ($runnerParameter in @(
        'QualificationDataManifestPath',
        'QualificationDataManifestSha256',
        'QualificationDataClosureSha256',
        'QualificationDataFileCount')) {
        Assert-Test ($workflowText.Contains("'-$runnerParameter'")) `
            "check-replays does not pass -$runnerParameter to the Stage 5 runner."
    }

    $fixedValues = @(
        's3://github-ci/generals108_gamedata_trimmed.7z',
        '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372',
        's3://github-ci/zerohour104_gamedata_trimmed.7z',
        '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
        'STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH',
        'STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256',
        'STAGE5_SIMULATION_QUALIFICATION_DATA_CLOSURE_SHA256',
        'STAGE5_SIMULATION_QUALIFICATION_DATA_FILE_COUNT'
    )
    $producerText = [IO.File]::ReadAllText($producerPath)
    foreach ($value in $fixedValues) {
        Assert-Test ($producerText.Contains($value)) `
            "Producer omits reviewed literal '$value'."
    }
    Assert-Test ($producerText -match
        '\[IO\.FileShare\]::Read' -and
        $producerText -match '\$finalArchiveSha256') `
        'Producer must hold and rehash the reviewed archive.'

    $producerFunctions = @($producerAst.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst]
    })
    foreach ($definition in $producerFunctions) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $poisonedRuntime = Join-Path $testRoot 'poisoned-cache-runtime'
    [IO.Directory]::CreateDirectory($poisonedRuntime) | Out-Null
    Write-TestText (Join-Path $poisonedRuntime 'INIZH.big') `
        'unreviewed cache-hit payload'
    Assert-Rejected {
        Assert-Stage5FreshSimulationRuntimeData -RuntimeRoot $poisonedRuntime
    } 'not fresh|existing.*qualification data' `
        'A cache-hit payload must fail before download or extraction.'

    Import-Module $modulePath -Force
    Assert-Test ($null -ne (Get-Command `
        Read-Stage5SimulationQualificationDataEvidence `
        -ErrorAction SilentlyContinue)) `
        'The module must export its simulation qualification-data reader.'

    $sourceCommit = 'a' * 40
    $fixtureRoot = Join-Path $testRoot 'complete'
    [IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
    $fixture = New-SimulationQualificationFixture $fixtureRoot $sourceCommit
    $proof = Read-Stage5SimulationQualificationDataEvidence `
        -Path $fixture.manifestPath -Binding $fixture.binding `
        -ExpectedSourceCommit $sourceCommit -ExpectedTitle 'ZeroHour'
    Assert-Test ($proof.manifestSha256 -ceq
            $fixture.binding.manifestSha256 -and
        $proof.closureSha256 -ceq $fixture.binding.closureSha256 -and
        $proof.fileCount -eq 6 -and $proof.title -ceq 'ZeroHour') `
        'The complete per-title qualification-data proof was not retained.'

    $generalsRoot = Join-Path $testRoot 'complete-generals'
    [IO.Directory]::CreateDirectory($generalsRoot) | Out-Null
    $generals = New-SimulationQualificationFixture $generalsRoot `
        $sourceCommit 'Generals'
    $generalsProof = Read-Stage5SimulationQualificationDataEvidence `
        -Path $generals.manifestPath -Binding $generals.binding `
        -ExpectedSourceCommit $sourceCommit -ExpectedTitle 'Generals'
    Assert-Test ($generalsProof.title -ceq 'Generals' -and
        $generalsProof.fileCount -eq 6 -and
        $generalsProof.archiveSource.object -ceq
            's3://github-ci/generals108_gamedata_trimmed.7z') `
        'The Generals qualification-data proof does not retain its fixed archive identity.'

    $fixture.document.archiveSource.sha256 = '0' * 64
    Set-FixtureDocument $fixture
    Assert-Rejected {
        Read-Stage5SimulationQualificationDataEvidence `
            -Path $fixture.manifestPath -Binding $fixture.binding `
            -ExpectedSourceCommit $sourceCommit -ExpectedTitle 'ZeroHour'
    } 'archive source.*unreviewed|archive.*substituted' `
        'A poisoned cached archive identity must be rejected after rehashing.'

    $staleRoot = Join-Path $testRoot 'stale'
    [IO.Directory]::CreateDirectory($staleRoot) | Out-Null
    $stale = New-SimulationQualificationFixture $staleRoot ('b' * 40)
    Assert-Rejected {
        Read-Stage5SimulationQualificationDataEvidence `
            -Path $stale.manifestPath -Binding $stale.binding `
            -ExpectedSourceCommit $sourceCommit -ExpectedTitle 'ZeroHour'
    } 'sourceCommit|identity.*stale|stale.*substituted' `
        'Qualification data from another source revision must be rejected.'

    $tokens = $null
    $errors = $null
    $runnerAst = [Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$tokens, [ref]$errors)
    Assert-Test ($errors.Count -eq 0) `
        "Simulation runner must parse: $($errors -join '; ')"
    foreach ($definition in @($runnerAst.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst]
    })) {
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    foreach ($command in @(
        'Open-Stage5SimulationQualificationRuntimeClosure',
        'Assert-Stage5SimulationQualificationRuntimeMembership',
        'Confirm-Stage5SimulationQualificationRuntimeClosure',
        'Close-Stage5SimulationQualificationRuntimeClosure')) {
        Assert-Test ($null -ne (Get-Command $command -ErrorAction SilentlyContinue)) `
            "Runner is missing qualification-data guard '$command'."
    }

    $runtimeFixtureRoot = Join-Path $testRoot 'runtime-guard'
    [IO.Directory]::CreateDirectory($runtimeFixtureRoot) | Out-Null
    $runtimeFixture = New-SimulationQualificationFixture `
        $runtimeFixtureRoot $sourceCommit
    $runtimeProof = Read-Stage5SimulationQualificationDataEvidence `
        -Path $runtimeFixture.manifestPath -Binding $runtimeFixture.binding `
        -ExpectedSourceCommit $sourceCommit -ExpectedTitle 'ZeroHour'
    $guard = Open-Stage5SimulationQualificationRuntimeClosure `
        -RuntimeRoot $runtimeFixture.runtimeRoot -Evidence $runtimeProof
    try {
        Assert-Stage5SimulationQualificationRuntimeMembership `
            -RuntimeRoot $runtimeFixture.runtimeRoot -Evidence $runtimeProof
        Assert-Rejected {
            $lockedPath = Join-Path $runtimeFixture.runtimeRoot `
                ([string]$runtimeProof.files[0].path)
            $writer = [IO.File]::Open($lockedPath, [IO.FileMode]::Open,
                [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
            $writer.Dispose()
        } 'used by another process|cannot access|being used' `
            'Declared qualification data must remain write-locked across the matrix.'

        $poisonPath = Join-Path $runtimeFixture.runtimeRoot `
            'Data/InjectedFromCache.ini'
        Write-TestText $poisonPath 'poisoned membership'
        Assert-Rejected {
            Assert-Stage5SimulationQualificationRuntimeMembership `
                -RuntimeRoot $runtimeFixture.runtimeRoot -Evidence $runtimeProof
        } 'membership.*changed|undeclared' `
            'An added cache member must fail the per-child membership check.'
        Remove-Item -LiteralPath $poisonPath -Force
        Confirm-Stage5SimulationQualificationRuntimeClosure `
            -RuntimeRoot $runtimeFixture.runtimeRoot -Evidence $runtimeProof `
            -Guard $guard
    }
    finally {
        Close-Stage5SimulationQualificationRuntimeClosure -Guard $guard
    }

    $changedPath = Join-Path $runtimeFixture.runtimeRoot `
        ([string]$runtimeProof.files[0].path)
    Write-TestText $changedPath 'changed after the producer released its files'
    Assert-Rejected {
        $changedGuard = Open-Stage5SimulationQualificationRuntimeClosure `
            -RuntimeRoot $runtimeFixture.runtimeRoot -Evidence $runtimeProof
        Close-Stage5SimulationQualificationRuntimeClosure -Guard $changedGuard
    } 'hash.*changed|SHA-256.*mismatch' `
        'Changed staged data must fail the runner preflight.'

    Write-Output 'Stage 5 simulation qualification-data tests passed.'
}
finally {
    Remove-Module DeterministicSimulationEvidence -Force `
        -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
        $expectedPrefix = $scratch.TrimEnd('\') + '\stage5-simulation-data-'
        Assert-Test ($resolvedTestRoot.StartsWith($expectedPrefix,
            [StringComparison]::OrdinalIgnoreCase)) `
            "Refusing to remove unexpected test path: $resolvedTestRoot"
        [IO.Directory]::Delete($resolvedTestRoot, $true)
    }
}
