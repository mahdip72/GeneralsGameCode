[CmdletBinding()]
param(
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Throws {
    param(
        [scriptblock]$Script,
        [string]$MessagePattern,
        [string]$Context
    )
    $thrown = $false
    try { & $Script }
    catch {
        $thrown = $true
        if (-not [string]::IsNullOrWhiteSpace($MessagePattern) -and
            $_.Exception.Message -notmatch $MessagePattern) {
            throw "$Context threw an unexpected error: $($_.Exception.Message)"
        }
    }
    if (-not $thrown) { throw "$Context unexpectedly accepted malformed numeric JSON." }
}

function Import-LockstepFixtureFunctions {
    param([string]$Path)
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) `
        "The lockstep fixture source must parse: $($parseErrors -join '; ')"
    $names = @(
        'Get-Sha256', 'Write-JsonDocument',
        'ConvertFrom-Stage5TestJsonTextDictionary',
        'Update-LockstepFixtureFnv',
        'Get-LockstepFixtureCommandDigest', 'Get-LockstepFixtureAIPlanningDigest',
        'Get-LockstepFixtureProjectionSha256', 'New-LockstepFixtureReceipt',
        'New-LockstepFixtureLauncherContract',
        'New-LockstepFixtureTitleSessionProfile',
        'New-LockstepFixturePeerEnvironment', 'New-LockstepFixtureNegativeProbe',
        'Get-LockstepFixtureTextSha256', 'New-LockstepQualificationDataFixture',
        'New-LockstepFixtureEvidence', 'Copy-LockstepFixtureCase',
        'Read-LockstepFixtureCaseDocument')
    $definitionsText = New-Object 'Collections.Generic.List[string]'
    foreach ($name in $names) {
        $definitions = @($ast.FindAll({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq $name
        }, $true))
        Assert-True ($definitions.Count -eq 1) `
            "The lockstep fixture source must contain one '$name' function."
        [void]$definitionsText.Add($definitions[0].Extent.Text)
    }
    return ($definitionsText -join "`n")
}

function Get-BadJsonValues {
    param([object]$GoodValue)
    return [ordered]@{
        string = [string]$GoodValue
        double = [double]$GoodValue
        fraction = ([double]$GoodValue) + 0.5
        array = @($GoodValue)
        bool = $true
    }
}

function Invoke-LockstepReader {
    param(
        [object]$Module,
        [string]$Path,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [string]$CohortNonce,
        [string]$CohortCreatedUtc,
        [Collections.IDictionary]$RuntimeClosure
    )
    & $Module {
        param($EvidencePath, $ExpectedCommit, $ExpectedArtifactHash,
            $ExpectedArtifacts, $ExpectedCohortNonce, $ExpectedCohortCreatedUtc,
            $ExpectedRuntimeClosure)
        Read-Stage5LockstepV2Evidence -Path $EvidencePath `
            -ExpectedSourceCommit $ExpectedCommit `
            -ExpectedArtifactSetSha256 $ExpectedArtifactHash `
            -ArtifactHashes $ExpectedArtifacts `
            -ExpectedCohortNonce $ExpectedCohortNonce `
            -ExpectedCohortCreatedUtc $ExpectedCohortCreatedUtc `
            -ExpectedRuntimeClosure $ExpectedRuntimeClosure | Out-Null
    } $Path $SourceCommit $ArtifactSetSha256 $ArtifactHashes $CohortNonce `
        $CohortCreatedUtc $RuntimeClosure
}

function Set-ReaderNumericMutation {
    param(
        [object]$Document,
        [string]$Family,
        [string]$Field,
        [object]$Value
    )
    if ($Family -ceq 'document') {
        $Document.$Field = $Value
    }
    elseif ($Family -ceq 'mapCrc') {
        $Document.mapCrcs.$Field = $Value
    }
    elseif ($Family -ceq 'session') {
        $Document.sessions[0].$Field = $Value
    }
    elseif ($Family -ceq 'sessionEffectiveWorkerCount') {
        $counts = @($Document.sessions[0].effectiveWorkerCounts)
        $counts[0] = $Value
        $Document.sessions[0].effectiveWorkerCounts = $counts
    }
    elseif ($Family -ceq 'sessionPort') {
        $ports = @($Document.sessions[0].ports)
        $ports[0] = $Value
        $Document.sessions[0].ports = $ports
    }
    elseif ($Family -ceq 'peer') {
        $Document.sessions[0].peers[0].$Field = $Value
    }
    else { throw "Unknown lockstep reader numeric family '$Family'." }
}

function Set-ReaderRawPeerMutation {
    param(
        [object]$Raw,
        [string]$Field,
        [object]$Value
    )
    $Raw.$Field = $Value
}

function Get-BadJsonLiteral {
    param([string]$Kind, [object]$GoodValue)
    switch ($Kind) {
        'string' { return ('"{0}"' -f [string]$GoodValue) }
        'double' { return ('{0}.0' -f [string]$GoodValue) }
        'fraction' { return ('{0}.5' -f [string]$GoodValue) }
        'array' { return ('[{0}]' -f [string]$GoodValue) }
        'bool' { return 'true' }
        default { throw "Unknown malformed JSON kind '$Kind'." }
    }
}

function Set-LockstepJsonPropertyLiteral {
    param(
        [string]$Path,
        [string]$Property,
        [string]$Literal,
        [string]$AfterMarker = ''
    )
    $text = Get-Content -LiteralPath $Path -Raw
    $offset = 0
    if (-not [string]::IsNullOrEmpty($AfterMarker)) {
        $offset = $text.IndexOf($AfterMarker, [StringComparison]::Ordinal)
        if ($offset -lt 0) { throw "JSON marker '$AfterMarker' was not found in $Path." }
        $offset += $AfterMarker.Length
    }
    $prefix = $text.Substring(0, $offset)
    $suffix = $text.Substring($offset)
    $pattern = '(?m)("' + [regex]::Escape($Property) + '"\s*:\s*)[^,\r\n]+'
    $replacement = '${1}' + $Literal
    if (-not [regex]::IsMatch($suffix, $pattern)) {
        throw "JSON property '$Property' was not found in $Path after '$AfterMarker'."
    }
    $updatedSuffix = [regex]::Replace($suffix, $pattern, $replacement, 1)
    [IO.File]::WriteAllText($Path, $prefix + $updatedSuffix,
        (New-Object Text.UTF8Encoding($false)))
}

function Set-LockstepJsonArrayElementLiteral {
    param(
        [string]$Path,
        [string]$Property,
        [string]$Literal,
        [string]$AfterMarker = ''
    )
    $text = Get-Content -LiteralPath $Path -Raw
    $offset = 0
    if (-not [string]::IsNullOrEmpty($AfterMarker)) {
        $offset = $text.IndexOf($AfterMarker, [StringComparison]::Ordinal)
        if ($offset -lt 0) { throw "JSON marker '$AfterMarker' was not found in $Path." }
        $offset += $AfterMarker.Length
    }
    $prefix = $text.Substring(0, $offset)
    $suffix = $text.Substring($offset)
    $pattern = '(?ms)("' + [regex]::Escape($Property) +
        '"\s*:\s*\[\s*)[^,\]\r\n]+'
    $replacement = '${1}' + $Literal
    if (-not [regex]::IsMatch($suffix, $pattern)) {
        throw "JSON array property '$Property' was not found in $Path after '$AfterMarker'."
    }
    $updatedSuffix = [regex]::Replace($suffix, $pattern, $replacement, 1)
    [IO.File]::WriteAllText($Path, $prefix + $updatedSuffix,
        (New-Object Text.UTF8Encoding($false)))
}

function Set-ReaderNumericJsonLiteral {
    param(
        [string]$Path,
        [string]$Family,
        [string]$Field,
        [string]$BadKind,
        [object]$GoodValue
    )
    $property = $Field
    $marker = ''
    if ($Family -ceq 'mapCrc') { $marker = '"mapCrcs":' }
    elseif ($Family -ceq 'session' -or
        $Family -ceq 'sessionEffectiveWorkerCount' -or
        $Family -ceq 'sessionPort') {
        $marker = '"sessions":'
    }
    elseif ($Family -ceq 'peer') { $marker = '"peers":' }
    if ($Family -ceq 'sessionEffectiveWorkerCount') {
        $property = 'effectiveWorkerCounts'
    }
    elseif ($Family -ceq 'sessionPort') { $property = 'ports' }
    $literal = Get-BadJsonLiteral $BadKind $GoodValue
    if ($Family -ceq 'sessionEffectiveWorkerCount') {
        Set-LockstepJsonArrayElementLiteral $Path $property $literal $marker
        return
    }
    elseif ($Family -ceq 'sessionPort') {
        Set-LockstepJsonArrayElementLiteral $Path $property $literal $marker
        return
    }
    Set-LockstepJsonPropertyLiteral $Path $property $literal $marker
}

$modulePath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
$sessionModulePath = Join-Path $PSScriptRoot 'Stage5InstalledLockstepV2Session.psm1'
$fixtureSourcePath = Join-Path $PSScriptRoot 'DeterministicSimulationValidation.Tests.ps1'
$validatorPath = Join-Path $PSScriptRoot 'Invoke-InstalledLockstepV2Validation.ps1'
Assert-True (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    'DeterministicSimulationEvidence.psm1 is missing.'
Assert-True (Test-Path -LiteralPath $sessionModulePath -PathType Leaf) `
    'Stage5InstalledLockstepV2Session.psm1 is missing.'
Assert-True (Test-Path -LiteralPath $fixtureSourcePath -PathType Leaf) `
    'The checked-in lockstep fixture source is missing.'
Assert-True (Test-Path -LiteralPath $validatorPath -PathType Leaf) `
    'Invoke-InstalledLockstepV2Validation.ps1 is missing.'

Import-Module $modulePath -Force
Import-Module $sessionModulePath -ErrorAction Stop
$module = Get-Module DeterministicSimulationEvidence
Invoke-Expression (Import-LockstepFixtureFunctions $fixtureSourcePath)

# The installed validator's envelope is a script-local function.  Import its
# function definitions without executing its production entrypoint so this
# focused test can exercise the same in-process validator boundary.
$validatorTokens = $null
$validatorParseErrors = $null
$validatorAst = [Management.Automation.Language.Parser]::ParseFile(
    $validatorPath, [ref]$validatorTokens, [ref]$validatorParseErrors)
Assert-True ($validatorParseErrors.Count -eq 0) `
    "The installed lockstep validator must parse: $($validatorParseErrors -join '; ')"
$validatorDefinitions = @($validatorAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true))
Assert-True ($validatorDefinitions.Count -gt 0) `
    'The installed lockstep validator must contain function definitions.'

$CommonStopFrame = 4096
$LockstepSchema = 2
$LockstepProtocolEpoch = 2
$LockstepAuthorityMask = 63
$LockstepNetworkPeerCount = 2
$LockstepNetworkRosterMask = 3
$LockstepSimulationRosterMask = 63
$LockstepAIRosterMask = 60
$LockstepAIPlayerCount = 4
$LockstepCheckpointCount = 129
$LockstepMode = 'installed-lockstep-v2-production'
$LockstepProducer = 'installed-lockstep-v2'
$LockstepTitleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
$LockstepEvidenceClosureLeaf = 'Stage5LockstepV2EvidenceClosure.json'
$LockstepQualificationDataEvidenceLeaf = 'QualificationData.json'
$LockstepGeneralsDataArchiveSha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
$LockstepZeroHourDataArchiveSha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
$LockstepMagic = 'RTS_LOCKSTEP_V2_RECEIPT'
$LockstepNegativeProbeMagic = 'RTS_LOCKSTEP_V2_NEGATIVE_PROBE'
$PostKillWaitMilliseconds = 5000
$script:LockstepHostSelfTestScratchRoot = $null
foreach ($definition in $validatorDefinitions) {
    Invoke-Expression $definition.Extent.Text
}

$script:TestCohortNonce = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
$script:TestCohortCreatedUtc = '2026-09-01T00:00:00.0000000Z'
$script:TestRuntimeClosure = [ordered]@{
    dependencyManifestSha256 = ('D' * 64)
    closureSha256 = ('E' * 64)
}
$sourceCommit = 'a' * 40
$artifactSetSha256 = '1' * 64
$artifactHashes = [ordered]@{
    'generals-executable' = 'A' * 64
    'zerohour-executable' = 'B' * 64
    'generals-launcher' = 'C' * 64
    'zerohour-launcher' = 'D' * 64
    'generals-launcher-config' = 'E' * 64
    'zerohour-launcher-config' = 'F' * 64
}

$scratchParent = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
}
else { [IO.Path]::GetFullPath($ScratchRoot) }
$testRoot = Join-Path $scratchParent `
    ('stage5-lockstep-numeric-{0}' -f [Guid]::NewGuid().ToString('N'))
Assert-True (-not (Test-Path -LiteralPath $testRoot)) `
    "Lockstep numeric scratch root already exists: $testRoot"
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$testSucceeded = $false

try {
    $fixtureRoot = Join-Path $testRoot 'positive'
    $fixture = New-LockstepFixtureEvidence $fixtureRoot $sourceCommit `
        $artifactSetSha256 $artifactHashes
    Invoke-LockstepReader $module $fixture.path $sourceCommit $artifactSetSha256 `
        $artifactHashes $script:TestCohortNonce $script:TestCohortCreatedUtc `
        $script:TestRuntimeClosure

    $readerCases = @(
        [pscustomobject]@{ family = 'document'; field = 'commonStopFrame'; good = 4096 },
        [pscustomobject]@{ family = 'document'; field = 'peerCount'; good = 2 },
        [pscustomobject]@{ family = 'document'; field = 'networkRosterMask'; good = 3 },
        [pscustomobject]@{ family = 'document'; field = 'simulationRosterMask'; good = 63 },
        [pscustomobject]@{ family = 'document'; field = 'aiRosterMask'; good = 60 },
        [pscustomobject]@{ family = 'document'; field = 'aiPlayerCount'; good = 4 },
        [pscustomobject]@{ family = 'document'; field = 'seed'; good = 23063 },
        [pscustomobject]@{ family = 'mapCrc'; field = 'Generals'; good = 739101722 },
        [pscustomobject]@{ family = 'session'; field = 'peerCount'; good = 2 },
        [pscustomobject]@{ family = 'session'; field = 'networkRosterMask'; good = 3 },
        [pscustomobject]@{ family = 'session'; field = 'simulationRosterMask'; good = 63 },
        [pscustomobject]@{ family = 'session'; field = 'aiRosterMask'; good = 60 },
        [pscustomobject]@{ family = 'session'; field = 'aiPlayerCount'; good = 4 },
        [pscustomobject]@{ family = 'session'; field = 'mapCrc'; good = 739101722 },
        [pscustomobject]@{ family = 'sessionEffectiveWorkerCount'; field = 'effectiveWorkerCounts[0]'; good = 2 },
        [pscustomobject]@{ family = 'sessionPort'; field = 'ports[0]'; good = 42000 },
        [pscustomobject]@{ family = 'peer'; field = 'schemaVersion'; good = 2 },
        [pscustomobject]@{ family = 'peer'; field = 'processId'; good = 51000 },
        [pscustomobject]@{ family = 'peer'; field = 'peer'; good = 0 },
        [pscustomobject]@{ family = 'peer'; field = 'peerCount'; good = 2 },
        [pscustomobject]@{ family = 'peer'; field = 'networkRosterMask'; good = 3 },
        [pscustomobject]@{ family = 'peer'; field = 'simulationRosterMask'; good = 63 },
        [pscustomobject]@{ family = 'peer'; field = 'aiRosterMask'; good = 60 },
        [pscustomobject]@{ family = 'peer'; field = 'aiPlayerCount'; good = 4 },
        [pscustomobject]@{ family = 'peer'; field = 'port'; good = 42000 },
        [pscustomobject]@{ family = 'peer'; field = 'exitCode'; good = 0 },
        [pscustomobject]@{ family = 'peer'; field = 'finalFrame'; good = 4096 },
        [pscustomobject]@{ family = 'peer'; field = 'finalCRC'; good = 100128 },
        [pscustomobject]@{ family = 'peer'; field = 'effectiveWorkers'; good = 2 }
    )
    $badKinds = @('string', 'double', 'fraction', 'array', 'bool')
    $caseIndex = 0
    foreach ($case in $readerCases) {
        foreach ($badKind in $badKinds) {
            ++$caseIndex
            $caseRoot = Join-Path $testRoot ('reader-{0:D3}-{1}-{2}' -f `
                $caseIndex, $case.family, $case.field.Replace('[', '-').Replace(']', ''))
            $casePath = Copy-LockstepFixtureCase $fixtureRoot $caseRoot
            $document = Read-LockstepFixtureCaseDocument $casePath
            $badValue = (Get-BadJsonValues $case.good)[$badKind]
            Set-ReaderNumericMutation $document $case.family $case.field $badValue
            if ($case.family -ceq 'peer') {
                $title = 'Generals'
                $rawPath = Join-Path (Split-Path -Parent $casePath) "$title/peer-0.raw.json"
                $raw = Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
                Set-ReaderRawPeerMutation $raw $case.field $badValue
                Write-JsonDocument $rawPath $raw
                Set-LockstepJsonPropertyLiteral $rawPath $case.field `
                    (Get-BadJsonLiteral $badKind $case.good)
            }
            Write-JsonDocument $casePath $document
            # ConvertTo-Json already preserves an array as an array.  Scalar
            # forms are rewritten in-place as the first array element so the
            # resulting document remains valid JSON and reaches the reader's
            # type guard.
            if ($badKind -cne 'array') {
                Set-ReaderNumericJsonLiteral $casePath $case.family $case.field `
                    $badKind $case.good
            }
            Assert-Throws {
                Invoke-LockstepReader $module $casePath $sourceCommit $artifactSetSha256 `
                    $artifactHashes $script:TestCohortNonce $script:TestCohortCreatedUtc `
                    $script:TestRuntimeClosure
            } 'integer|roster|worker|port|schema|process|CRC|contract|identity|stale|substituted|invalid' `
                "reader $($case.family)/$($case.field) $badKind"
        }
    }

    $validatorBase = Read-LockstepFixtureCaseDocument $fixture.path
    $baselineEnvelope = New-LockstepV2FinalAcceptanceEnvelope `
        -NativeEvidencePath $fixture.path `
        -SourceCommit $sourceCommit `
        -ArtifactSetSha256 $artifactSetSha256 `
        -RecordedUtc '2026-09-01T00:00:00Z' `
        -MapName 'Maps\Twilight Flame\Twilight Flame.map' `
        -MapCrcs ([ordered]@{ Generals = 739101722; ZeroHour = 4042777579 }) `
        -Seed 23063 -PeerCount 2 -Sessions @($validatorBase.sessions) `
        -CohortNonce $script:TestCohortNonce `
        -CohortCreatedUtc $script:TestCohortCreatedUtc `
        -RuntimeClosure $script:TestRuntimeClosure
    Assert-True ($null -ne $baselineEnvelope -and
        $null -ne $baselineEnvelope.document -and
        $baselineEnvelope.document.details.sessionCount -eq 2 -and
        $baselineEnvelope.document.details.peerCount -eq 2) `
        'The installed lockstep validator must accept the unmodified positive fixture.'
    $validatorNegativeProbeCases = @(
        [pscustomobject]@{ field = 'processId'; good = 52000 },
        [pscustomobject]@{ field = 'exitCode'; good = 0 },
        [pscustomobject]@{ field = 'probeBuildCrc'; good = 739101722 },
        [pscustomobject]@{ field = 'probeContentCrc'; good = 23063 }
    )
    $validatorNegativeProbeIndex = 0
    foreach ($case in $validatorNegativeProbeCases) {
        foreach ($badKind in $badKinds) {
            ++$validatorNegativeProbeIndex
            $caseRoot = Join-Path $testRoot ('validator-negative-probe-{0:D3}-{1}-{2}' -f `
                $validatorNegativeProbeIndex, $case.field, $badKind)
            $casePath = Copy-LockstepFixtureCase $fixtureRoot $caseRoot
            Set-LockstepJsonPropertyLiteral $casePath $case.field `
                (Get-BadJsonLiteral $badKind $case.good) '"negativeProbes":'
            Assert-Throws {
                New-LockstepV2FinalAcceptanceEnvelope `
                    -NativeEvidencePath $casePath `
                    -SourceCommit $sourceCommit `
                    -ArtifactSetSha256 $artifactSetSha256 `
                    -RecordedUtc '2026-09-01T00:00:00Z' `
                    -MapName 'Maps\Twilight Flame\Twilight Flame.map' `
                    -MapCrcs ([ordered]@{ Generals = 739101722; ZeroHour = 4042777579 }) `
                    -Seed 23063 -PeerCount 2 -Sessions @($validatorBase.sessions) `
                    -CohortNonce $script:TestCohortNonce `
                    -CohortCreatedUtc $script:TestCohortCreatedUtc `
                    -RuntimeClosure $script:TestRuntimeClosure | Out-Null
            } 'integer|range' `
                "installed validator negative probe $($case.field) $badKind"
        }
    }
    $validatorCases = @(
        [pscustomobject]@{ family = 'session'; field = 'mapCrc'; good = 739101722 },
        [pscustomobject]@{ family = 'session'; field = 'peerCount'; good = 2 },
        [pscustomobject]@{ family = 'session'; field = 'networkRosterMask'; good = 3 },
        [pscustomobject]@{ family = 'session'; field = 'simulationRosterMask'; good = 63 },
        [pscustomobject]@{ family = 'session'; field = 'aiRosterMask'; good = 60 },
        [pscustomobject]@{ family = 'session'; field = 'aiPlayerCount'; good = 4 },
        [pscustomobject]@{ family = 'sessionEffectiveWorkerCount'; field = 'effectiveWorkerCounts[0]'; good = 2 },
        [pscustomobject]@{ family = 'peer'; field = 'peerCount'; good = 2 },
        [pscustomobject]@{ family = 'peer'; field = 'networkRosterMask'; good = 3 },
        [pscustomobject]@{ family = 'peer'; field = 'simulationRosterMask'; good = 63 },
        [pscustomobject]@{ family = 'peer'; field = 'aiRosterMask'; good = 60 },
        [pscustomobject]@{ family = 'peer'; field = 'aiPlayerCount'; good = 4 }
    )
    $validatorIndex = 0
    foreach ($case in $validatorCases) {
        foreach ($badKind in $badKinds) {
            ++$validatorIndex
            $sessions = ConvertFrom-Stage5TestJsonTextDictionary `
                ((ConvertTo-Json $validatorBase.sessions -Depth 20))
            $badValue = (Get-BadJsonValues $case.good)[$badKind]
            if ($case.family -ceq 'session') {
                $sessions[0].$($case.field) = $badValue
            }
            elseif ($case.family -ceq 'sessionEffectiveWorkerCount') {
                $counts = @($sessions[0].effectiveWorkerCounts)
                $counts[0] = $badValue
                $sessions[0].effectiveWorkerCounts = $counts
            }
            elseif ($case.family -ceq 'peer') {
                $sessions = ConvertFrom-Stage5TestJsonTextDictionary `
                    ((ConvertTo-Json $validatorBase.sessions -Depth 20))
                $sessions[0].peers[0].$($case.field) = $badValue
            }
            else { throw ('Unknown installed validator numeric family ' + $case.family + '.') }
            Assert-Throws {
                New-LockstepV2FinalAcceptanceEnvelope `
                    -NativeEvidencePath $fixture.path `
                    -SourceCommit $sourceCommit `
                    -ArtifactSetSha256 $artifactSetSha256 `
                    -RecordedUtc '2026-09-01T00:00:00Z' `
                    -MapName 'Maps\Twilight Flame\Twilight Flame.map' `
                    -MapCrcs ([ordered]@{ Generals = 739101722; ZeroHour = 4042777579 }) `
                    -Seed 23063 -PeerCount 2 -Sessions $sessions `
                    -CohortNonce $script:TestCohortNonce `
                    -CohortCreatedUtc $script:TestCohortCreatedUtc `
                    -RuntimeClosure $script:TestRuntimeClosure | Out-Null
            } 'integer|roster|worker|session|peer|CRC|incomplete|substituted|invalid' `
                "installed validator $($case.family)/$($case.field) $badKind"
        }
    }
    $testSucceeded = $true
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        if (-not $testSucceeded) {
            Write-Warning "Retaining failed lockstep numeric test child for diagnostics: $testRoot"
        }
        else {
            $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
            $resolvedParent = [IO.Path]::GetFullPath($scratchParent).TrimEnd('\', '/')
            $actualParent = [IO.Path]::GetDirectoryName($resolvedRoot).TrimEnd('\', '/')
            $testItem = Get-Item -LiteralPath $resolvedRoot -Force
            Assert-True ($actualParent -ceq $resolvedParent -and
                [IO.Path]::GetFileName($resolvedRoot) -like 'stage5-lockstep-numeric-*' -and
                $testItem.PSIsContainer -and
                (($testItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) `
                "Refusing to clean an unexpected lockstep numeric test child: $resolvedRoot"
            [IO.Directory]::Delete($resolvedRoot, $true)
        }
    }
}

Write-Output 'Stage 5 lockstep numeric type tests passed.'
