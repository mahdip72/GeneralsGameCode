[CmdletBinding()]
param(
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $observed = $null
    try {
        & $Action
    }
    catch {
        $observed = $_.Exception.Message
    }
    Assert-True ($null -ne $observed) "$Description must fail closed."
    Assert-True ($observed -match $Pattern) `
        "$Description returned the wrong rejection: $observed"
}

function Get-Sha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return (($algorithm.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $algorithm.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-TextSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return (($algorithm.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Write-JsonDocument {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $json = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json,
        (New-Object Text.UTF8Encoding($false)))
}

function New-QualificationDataFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$SourceCommit,

        [Parameter(Mandatory = $true)]
        [string]$MapName,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$MapCrcs
    )

    $requiredByTitle = [ordered]@{
        Generals = @(
            'English.big',
            'INI.big',
            'Maps.big',
            'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb'
        )
        ZeroHour = @(
            'INIZH.big',
            'MapsZH.big',
            'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb'
        )
    }
    $entriesByIdentity = @{}
    $entryIndex = 0
    foreach ($title in $requiredByTitle.Keys) {
        $runtimeLeaf = if ($title -ceq 'Generals') {
            'GeneralsRuntime'
        }
        else { 'ZeroHourRuntime' }
        foreach ($relative in $requiredByTitle[$title]) {
            ++$entryIndex
            $path = "$runtimeLeaf/$relative"
            $identity = "$title|$path"
            $hashDigit = '{0:X}' -f $entryIndex
            $entriesByIdentity[$identity] = [ordered]@{
                title = $title
                path = $path
                sha256 = $hashDigit * 64
            }
        }
    }
    [string[]]$identities = @($entriesByIdentity.Keys)
    [Array]::Sort($identities, [StringComparer]::Ordinal)
    $entries = @($identities | ForEach-Object { $entriesByIdentity[$_] })
    $canonicalText = (@($entries | ForEach-Object {
        '{0}|{1}|{2}' -f $_.title, $_.path, $_.sha256
    }) -join "`n") + "`n"
    $closureSha256 = Get-TextSha256 -Text $canonicalText
    $document = [ordered]@{
        schemaVersion = 2
        evidenceKind = 'lockstep-v2-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        mapName = $MapName
        mapCrcs = [ordered]@{
            Generals = [UInt32]$MapCrcs['Generals']
            ZeroHour = [UInt32]$MapCrcs['ZeroHour']
        }
        archiveSources = @(
            [ordered]@{
                title = 'Generals'
                object = 's3://github-ci/generals108_gamedata_trimmed.7z'
                sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
            },
            [ordered]@{
                title = 'ZeroHour'
                object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
                sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
            }
        )
        files = $entries
        closureSha256 = $closureSha256
    }
    $path = Join-Path $Root 'QualificationData.json'
    Write-JsonDocument -Path $path -Value $document
    return [pscustomobject]@{
        path = $path
        document = $document
        binding = [ordered]@{
            manifestSha256 = Get-Sha256 -Path $path
            closureSha256 = $closureSha256
            fileCount = $entries.Count
        }
    }
}

function Set-FixtureDocument {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Fixture,

        [switch]$RefreshManifestBinding
    )

    Write-JsonDocument -Path $Fixture.path -Value $Fixture.document
    if ($RefreshManifestBinding) {
        $Fixture.binding.manifestSha256 = Get-Sha256 -Path $Fixture.path
    }
}

$modulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
$validatorPath = Join-Path $PSScriptRoot `
    'Invoke-InstalledLockstepV2Validation.ps1'
$tokens = $null
$parseErrors = $null
$moduleAst = [Management.Automation.Language.Parser]::ParseFile(
    $modulePath, [ref]$tokens, [ref]$parseErrors)
Assert-True ($parseErrors.Count -eq 0) `
    "DeterministicSimulationEvidence.psm1 must parse: $($parseErrors -join '; ')"
$tokens = $null
$parseErrors = $null
$validatorAst = [Management.Automation.Language.Parser]::ParseFile(
    $validatorPath, [ref]$tokens, [ref]$parseErrors)
Assert-True ($parseErrors.Count -eq 0) `
    "Invoke-InstalledLockstepV2Validation.ps1 must parse: $($parseErrors -join '; ')"

$reader = @($moduleAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Read-Stage5LockstepV2Evidence'
}, $true))
$qualificationReader = @($moduleAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Read-Stage5LockstepQualificationDataEvidence'
}, $true))
Assert-True ($reader.Count -eq 1 -and $qualificationReader.Count -eq 1) `
    'The checked-in lockstep evidence reader must have one qualification-data reader.'
$qualificationCalls = @($reader[0].FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Read-Stage5LockstepQualificationDataEvidence'
}, $true))
Assert-True ($qualificationCalls.Count -eq 1) `
    'The public lockstep reader must actively validate qualification data exactly once.'
$bindingAssignments = @($validatorAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$evidence'
}, $true) | Where-Object {
    $_.Right.Extent.Text -match '(?m)^\s*qualificationData\s*=\s*\[ordered\]@\{'
})
Assert-True ($bindingAssignments.Count -eq 1) `
    'The installed validator must publish one active qualification-data binding.'
foreach ($field in @('manifestSha256', 'closureSha256', 'fileCount')) {
    Assert-True ($bindingAssignments[0].Right.Extent.Text -match
        (('(?m)^\s*{0}\s*=\s*\$qualificationData\.{0}\s*$' -f $field))) `
        "The installed validator qualification-data binding omits $field."
}

$scratchParent = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
}
else { [IO.Path]::GetFullPath($ScratchRoot) }
$testRoot = Join-Path $scratchParent `
    ("stage5-lockstep-qualification-{0}" -f [Guid]::NewGuid().ToString('N'))
Assert-True (-not (Test-Path -LiteralPath $testRoot)) `
    "Lockstep qualification-data scratch root already exists: $testRoot"
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$testSucceeded = $false

$module = $null
$sourceCommit = 'a' * 40
$mapName = 'Maps\Twilight Flame\Twilight Flame.map'
$mapCrcs = [ordered]@{
    Generals = [UInt32]739101722
    ZeroHour = [UInt32]4042777579
}
try {
    $module = Import-Module $modulePath -Force -PassThru
    $invokeReader = {
        param([object]$Fixture)
        & $module {
            param($Path, $Binding, $ExpectedCommit, $ExpectedMap,
                $ExpectedCrcs)
            Read-Stage5LockstepQualificationDataEvidence -Path $Path `
                -Binding $Binding -ExpectedSourceCommit $ExpectedCommit `
                -ExpectedMapName $ExpectedMap -ExpectedMapCrcs $ExpectedCrcs
        } $Fixture.path $Fixture.binding $sourceCommit $mapName $mapCrcs
    }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $proof = & $invokeReader $fixture
    Assert-True ($proof.manifestSha256 -ceq $fixture.binding.manifestSha256 -and
        $proof.closureSha256 -ceq $fixture.binding.closureSha256 -and
        [Int64]$proof.fileCount -eq [Int64]$fixture.binding.fileCount) `
        'Qualification-data reader did not return the exact retained proof.'

    $fixture.binding.manifestSha256 = '0' * 64
    Assert-Throws -Description 'Manifest SHA substitution' `
        -Pattern 'SHA-256' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.sourceCommit = 'b' * 40
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'Source-commit substitution' `
        -Pattern 'identity or map binding' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.mapCrcs.Generals = [UInt32]1
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'Map CRC substitution' `
        -Pattern 'identity or map binding' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.archiveSources[0].sha256 = '0' * 64
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'Reviewed archive substitution' `
        -Pattern 'archive source is unreviewed' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.files[0].sha256 = '0' * 64
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'File-closure substitution' `
        -Pattern 'file closure SHA-256' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.files[0].path =
        $fixture.document.files[0].path + "`nGeneralsRuntime/Forged.big"
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'Control-character path substitution' `
        -Pattern 'unsafe, duplicated, or unsorted' `
        -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    $fixture.document.files[0].path = $fixture.document.files[0].path + ' '
    Set-FixtureDocument -Fixture $fixture -RefreshManifestBinding
    Assert-Throws -Description 'Trailing-space path substitution' `
        -Pattern 'unsafe, duplicated, or unsorted' `
        -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    ++$fixture.binding.fileCount
    Assert-Throws -Description 'File-count substitution' `
        -Pattern 'file count is stale' -Action { & $invokeReader $fixture }

    $fixture = New-QualificationDataFixture -Root $testRoot `
        -SourceCommit $sourceCommit -MapName $mapName -MapCrcs $mapCrcs
    Remove-Item -LiteralPath $fixture.path -Force
    Assert-Throws -Description 'Missing retained qualification-data manifest' `
        -Pattern 'was not found' -Action { & $invokeReader $fixture }
    $testSucceeded = $true
}
finally {
    if ($null -ne $module) {
        Remove-Module $module -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $testRoot) {
        if (-not $testSucceeded) {
            Write-Warning "Retaining failed lockstep qualification test child for diagnostics: $testRoot"
        }
        else {
            $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
            $resolvedParent = [IO.Path]::GetFullPath($scratchParent).TrimEnd('\', '/')
            $actualParent = [IO.Path]::GetDirectoryName($resolvedRoot).TrimEnd('\', '/')
            $testItem = Get-Item -LiteralPath $resolvedRoot -Force
            Assert-True ($actualParent -ceq $resolvedParent -and
                [IO.Path]::GetFileName($resolvedRoot) -like 'stage5-lockstep-qualification-*' -and
                $testItem.PSIsContainer -and
                (($testItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) `
                "Refusing to clean an unexpected lockstep qualification test child: $resolvedRoot"
            [IO.Directory]::Delete($resolvedRoot, $true)
        }
    }
}

Write-Output 'Stage 5 lockstep qualification-data binding tests passed.'
