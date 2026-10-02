[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$scratchParent = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot
}
elseif (-not [string]::IsNullOrWhiteSpace(
        $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else {
    throw 'Stage 5 JSON cardinality tests require an explicit H: scratch root.'
}
$scratchParent = [IO.Path]::GetFullPath($scratchParent).TrimEnd('\')
if (-not $scratchParent.StartsWith('H:\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage 5 JSON cardinality test scratch must remain on H:.'
}
[IO.Directory]::CreateDirectory($scratchParent) | Out-Null

function Assert-Cardinality {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-ArrayAssignment {
    param(
        [Management.Automation.Language.Ast]$Ast,
        [string]$Name,
        [string]$Field,
        [string]$ContextFragment
    )
    $matches = @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $node.Left.VariablePath.UserPath -ceq $Name -and
            $node.Right.Extent.Text -match 'Get-Stage5JsonValue' -and
            $node.Right.Extent.Text -match ("'" + [regex]::Escape($Field) + "'") -and
            $node.Right.Extent.Text -match [regex]::Escape($ContextFragment)
    }, $true))
    Assert-Cardinality ($matches.Count -eq 1) `
        "Expected one direct array assignment for '$Name' ($Field/$ContextFragment); found $($matches.Count)."
    return $matches[0]
}

function Assert-DirectJsonArrayAssignment {
    param(
        [Management.Automation.Language.Ast]$Ast,
        [string]$Name,
        [string]$Field,
        [string]$ContextFragment
    )
    $assignment = Get-ArrayAssignment $Ast $Name $Field $ContextFragment
    Assert-Cardinality ($assignment.Right -is [Management.Automation.Language.PipelineAst]) `
        "Array-valued '$Name' assignment must remain a direct JSON-value pipeline."
    $elements = @($assignment.Right.PipelineElements)
    Assert-Cardinality ($elements.Count -eq 1 -and
        $elements[0] -is [Management.Automation.Language.CommandAst] -and
        $elements[0].GetCommandName() -ceq 'Get-Stage5JsonValue') `
        "Array-valued '$Name' assignment must not wrap Get-Stage5JsonValue in an outer array."
}

function Get-CardinalitySha256Text {
    param([Parameter(Mandatory = $true)][string]$Text)
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-CardinalitySha256File {
    param([Parameter(Mandatory = $true)][string]$Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return (($sha.ComputeHash($stream) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Write-CardinalityJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Document
    )
    $json = $Document | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json,
        (New-Object Text.UTF8Encoding($false)))
}

function Assert-CardinalityThrows {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Message
    )
    $threw = $false
    try { & $Action }
    catch { $threw = $true }
    Assert-Cardinality $threw $Message
}

function Copy-CardinalityOrderedObject {
    param([Parameter(Mandatory = $true)][Collections.IDictionary]$Object)
    $copy = [ordered]@{}
    foreach ($key in $Object.Keys) { $copy[$key] = $Object[$key] }
    return $copy
}

function Invoke-CardinalityProtectedAttestation {
    param(
        [Parameter(Mandatory = $true)][Management.Automation.PSModuleInfo]$Module,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][Collections.IDictionary]$Document,
        [Parameter(Mandatory = $true)][string]$SourceCommit,
        [Parameter(Mandatory = $true)][string]$ArtifactSetSha256
    )
    & $Module {
        param($path, $document, $sourceCommit, $artifactSetSha256)
        Read-Stage5FinalAcceptanceProtectedAttestation `
            -Path $path -Document $document -Kind 'replay-determinism' `
            -Role 'replay-fixture-manifest' -EvidenceTitle 'ZeroHour' `
            -TrustDomain 'reviewed-fixture' `
            -ExpectedSourceCommit $sourceCommit `
            -ExpectedArtifactSetSha256 $artifactSetSha256 | Out-Null
    } $Path $Document $SourceCommit $ArtifactSetSha256
}

function New-CardinalityRuntimeClosureFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$SourceCommit
    )
    $files = New-Object 'Collections.Generic.List[object]'
    $artifactEntries = New-Object 'Collections.Generic.List[object]'
    $definitions = @(
        @('Generals', 'executable', 'generalsv.exe', 'generals-executable'),
        @('Generals', 'launcher', 'launcher.exe', 'generals-launcher'),
        @('Generals', 'launcher-config', 'launcher.lcf', 'generals-launcher-config'),
        @('Generals', 'dll', 'generals.dll', $null),
        @('Generals', 'asset', 'generals.big', $null),
        @('ZeroHour', 'executable', 'generalszh.exe', 'zerohour-executable'),
        @('ZeroHour', 'launcher', 'launcher.exe', 'zerohour-launcher'),
        @('ZeroHour', 'launcher-config', 'launcher.lcf', 'zerohour-launcher-config'),
        @('ZeroHour', 'dll', 'generalszh.dll', $null),
        @('ZeroHour', 'asset', 'generalszh.big', $null)
    )
    foreach ($title in @('Generals', 'ZeroHour')) {
        New-Item -ItemType Directory -Path (Join-Path $Root $title) -Force | Out-Null
    }
    foreach ($definition in $definitions) {
        $title = [string]$definition[0]
        $kind = [string]$definition[1]
        $leaf = [string]$definition[2]
        $relative = "$title\$leaf"
        $path = Join-Path $Root ($relative -replace '\\', [IO.Path]::DirectorySeparatorChar)
        [IO.File]::WriteAllText($path, "stage5 closure fixture $relative",
            (New-Object Text.UTF8Encoding($false)))
        $hash = Get-CardinalitySha256File $path
        $entry = [ordered]@{
            title = $title; kind = $kind; path = $relative; sha256 = $hash
        }
        $files.Add($entry) | Out-Null
        if ($null -ne $definition[3]) {
            $artifactEntries.Add([ordered]@{
                role = [string]$definition[3]; path = $relative; sha256 = $hash
            }) | Out-Null
        }
    }
    $canonicalLines = @($files | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.kind,
            ([string]$_.path).Replace('\', '/'),
            ([string]$_.sha256).ToUpperInvariant()
    })
    [Array]::Sort($canonicalLines, [StringComparer]::Ordinal)
    $manifest = [ordered]@{
        schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
        files = $files.ToArray()
    }
    $manifestPath = Join-Path $Root 'dependency-manifest.json'
    Write-CardinalityJson $manifestPath $manifest
    $manifestHash = Get-CardinalitySha256File $manifestPath
    $closureHash = Get-CardinalitySha256Text (($canonicalLines -join "`n") + "`n")
    $artifactSet = [ordered]@{
        schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
        artifacts = $artifactEntries.ToArray()
        runtimeClosure = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'dependency-manifest.json'; sha256 = $manifestHash
            }
            closureSha256 = $closureHash
        }
    }
    return [pscustomobject]@{
        artifactSet = $artifactSet; manifest = $manifest
        manifestPath = $manifestPath
    }
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$modulePath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1'
Assert-Cardinality (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    "Evidence module is missing: $modulePath"

$tokens = $null
$errors = $null
$moduleAst = [Management.Automation.Language.Parser]::ParseFile(
    $modulePath, [ref]$tokens, [ref]$errors)
Assert-Cardinality ($errors.Count -eq 0) `
    "Evidence module must parse: $($errors -join '; ')"

foreach ($spec in @(
        [ordered]@{ name = 'fixtureEntries'; field = 'fixtures'; context = 'reviewed manifest' },
        [ordered]@{ name = 'actual'; field = 'arguments'; context = '$Context' },
        [ordered]@{ name = 'planEntries'; field = 'entries'; context = '$context plan' },
        [ordered]@{ name = 'reviewedFixtures'; field = 'fixtures'; context = '$context reviewed corpus' },
        [ordered]@{ name = 'aiSeeds'; field = 'seeds'; context = '$context reviewed AI' },
        [ordered]@{ name = 'aiScenarios'; field = 'scenarios'; context = '$context reviewed AI' }
    )) {
    Assert-DirectJsonArrayAssignment $moduleAst $spec.name $spec.field $spec.context
}

Import-Module $modulePath -Force
Assert-Cardinality ($null -ne (Get-Command Get-Stage5JsonValue -ErrorAction SilentlyContinue)) `
    'Get-Stage5JsonValue was not exported by the evidence module.'

$planEntries = [object[]](1..253 | ForEach-Object {
    [ordered]@{ sequence = $_; kind = 'replay' }
})
$plan = [ordered]@{ entries = $planEntries }
$boundPlanEntries = Get-Stage5JsonValue $plan 'entries' '253-entry plan regression'
Assert-Cardinality ($boundPlanEntries -is [Array] -and $boundPlanEntries.Count -eq 253) `
    "253-entry plan array was rebound with count $($boundPlanEntries.Count)."

$arguments = [object[]]@('-headless', '-pipelineMode', 'serial', '-simulationMode',
    'parallel', '-workerPolicy', 'auto', '-workerCount', '4')
$argumentEntry = [ordered]@{ arguments = $arguments }
$boundArguments = Get-Stage5JsonValue $argumentEntry 'arguments' `
    'representative arguments regression'
Assert-Cardinality ($boundArguments -is [Array] -and $boundArguments.Count -eq 9) `
    "Representative arguments array was rebound with count $($boundArguments.Count)."

$fixtures = [object[]](1..10 | ForEach-Object {
    [ordered]@{ id = "fixture-$($_)"; sha256 = ('A' * 64) }
})
$fixtureManifest = [ordered]@{ fixtures = $fixtures }
$boundFixtures = Get-Stage5JsonValue $fixtureManifest 'fixtures' `
    'representative fixture regression'
Assert-Cardinality ($boundFixtures -is [Array] -and $boundFixtures.Count -eq 10) `
    "Representative fixture array was rebound with count $($boundFixtures.Count)."

$ai = [ordered]@{
    seeds = [object[]]@(1729, 2718, 3141)
    scenarios = [object[]]@('4v2', '4v3')
}
$boundSeeds = Get-Stage5JsonValue $ai 'seeds' 'representative AI seeds regression'
$boundScenarios = Get-Stage5JsonValue $ai 'scenarios' 'representative AI scenarios regression'
Assert-Cardinality ($boundSeeds -is [Array] -and $boundSeeds.Count -eq 3) `
    "Representative AI seed array was rebound with count $($boundSeeds.Count)."
Assert-Cardinality ($boundScenarios -is [Array] -and $boundScenarios.Count -eq 2) `
    "Representative AI scenario array was rebound with count $($boundScenarios.Count)."

$module = Get-Module -Name DeterministicSimulationEvidence
Assert-Cardinality ($null -ne $module) 'Evidence module was not imported.'

# These are intentionally JSON-shaped arrays, including single-element arrays.
# The latter catches scalar coercion such as [string]$value, while the former
# catches PowerShell collection comparison returning a matching element.
$scratchRoot = Join-Path $scratchParent `
    ('stage5-json-array-cardinality-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratchRoot -Force | Out-Null
try {
    $sourceCommit = '0123456789abcdef0123456789abcdef01234567'
    $artifactSetSha256 = 'A' * 64
    $issuedUtc = '2026-09-04T00:00:00.0000000Z'
    $recordedUtc = '2026-09-04T00:00:01.0000000Z'
    $attestation = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-external-attestation'
        trustDomain = 'reviewed-fixture'
        role = 'replay-fixture-manifest'
        sourceCommit = $sourceCommit
        artifactSetSha256 = $artifactSetSha256
        subjectKey = "$sourceCommit|$artifactSetSha256|replay-determinism|replay-fixture-manifest|ZeroHour"
        authority = 'fixture-review-authority'
        issuedUtc = $issuedUtc
    }
    $attestationPath = Join-Path $scratchRoot 'attestation.json'
    Write-CardinalityJson $attestationPath $attestation
    $document = [ordered]@{
        protection = [ordered]@{
            kind = 'external-reviewed-fixture-attestation'
            path = 'attestation.json'
            sha256 = Get-CardinalitySha256File $attestationPath
        }
        provenance = [ordered]@{ reviewedUtc = $issuedUtc }
        recordedUtc = $recordedUtc
    }
    $wrapperPath = Join-Path $scratchRoot 'wrapper.json'
    foreach ($field in @('evidenceKind', 'trustDomain', 'role', 'sourceCommit',
            'artifactSetSha256', 'subjectKey', 'authority')) {
        $mutatedAttestation = Copy-CardinalityOrderedObject $attestation
        $mutatedAttestation[$field] = [object[]]@($attestation[$field], 'extra')
        Write-CardinalityJson $attestationPath $mutatedAttestation
        $mutatedDocument = Copy-CardinalityOrderedObject $document
        $mutatedProtection = Copy-CardinalityOrderedObject $document.protection
        $mutatedProtection.sha256 = Get-CardinalitySha256File $attestationPath
        $mutatedDocument.protection = $mutatedProtection
        Assert-CardinalityThrows {
            Invoke-CardinalityProtectedAttestation $module $wrapperPath `
                $mutatedDocument $sourceCommit $artifactSetSha256
        } "Protected attestation rejects array-valued '$field'."
    }

    $closureFixture = New-CardinalityRuntimeClosureFixture $scratchRoot $sourceCommit
    $closureRead = Get-Stage5RuntimeClosureBinding $closureFixture.artifactSet `
        $scratchRoot $sourceCommit 'JSON array cardinality runtime closure'
    Assert-Cardinality ($closureRead.fileCount -eq 10) `
        'Runtime closure positive fixture did not validate all ten files.'
    foreach ($field in @('sourceCommit', 'architecture')) {
        $mutatedManifest = Copy-CardinalityOrderedObject $closureFixture.manifest
        $mutatedManifest[$field] = [object[]]@($closureFixture.manifest[$field])
        Write-CardinalityJson $closureFixture.manifestPath $mutatedManifest
        $mutatedArtifactSet = Copy-CardinalityOrderedObject $closureFixture.artifactSet
        $mutatedRuntimeClosure = Copy-CardinalityOrderedObject `
            $closureFixture.artifactSet.runtimeClosure
        $mutatedManifestReference = Copy-CardinalityOrderedObject `
            $closureFixture.artifactSet.runtimeClosure.dependencyManifest
        $mutatedManifestReference.sha256 = `
            Get-CardinalitySha256File $closureFixture.manifestPath
        $mutatedRuntimeClosure.dependencyManifest = $mutatedManifestReference
        $mutatedArtifactSet.runtimeClosure = $mutatedRuntimeClosure
        Assert-CardinalityThrows {
            Get-Stage5RuntimeClosureBinding $mutatedArtifactSet $scratchRoot `
                $sourceCommit 'JSON array cardinality runtime closure mutation' | Out-Null
        } "Runtime closure rejects a single-element array-valued '$field'."
        Write-CardinalityJson $closureFixture.manifestPath $closureFixture.manifest
    }

    $expectedClosure = [ordered]@{
        dependencyManifestSha256 = $closureRead.dependencyManifestSha256
        closureSha256 = $closureRead.closureSha256
    }
    foreach ($field in @('dependencyManifestSha256', 'closureSha256')) {
        $mutatedExpected = Copy-CardinalityOrderedObject $expectedClosure
        $mutatedExpected[$field] = [object[]]@($expectedClosure[$field], 'extra')
        Assert-CardinalityThrows {
            & $module {
                param($value, $expected)
                Assert-Stage5RuntimeClosureBinding $value $expected `
                    'JSON array cardinality runtime closure expectation' | Out-Null
            } $closureRead $mutatedExpected
        } "Runtime closure rejects an array-valued expected '$field'."
    }

    $readinessHash = 'B' * 64
    $readinessEntry = [ordered]@{
        configuration = 'serial-1'
        simulationMode = 'serial'
        kind = 'replay'
        replayArgument = 'Stage5Validation\fixture.rep'
        arguments = @(
            '-headless', '-noFPSLimit', '-pipelineMode', 'serial',
            '-simulationMode', 'serial', '-workerPolicy', 'auto',
            '-validationExecutableSha256', $readinessHash,
            '-workerCount', '1', '-replay', 'Stage5Validation\fixture.rep')
    }
    & $module {
        param($entry, $hash)
        Assert-Stage5DevelopmentReadinessArguments $entry $hash `
            'JSON array cardinality readiness arguments positive'
    } $readinessEntry $readinessHash
    foreach ($field in @('configuration', 'simulationMode', 'kind', 'replayArgument')) {
        $mutatedEntry = Copy-CardinalityOrderedObject $readinessEntry
        $mutatedEntry[$field] = [object[]]@($readinessEntry[$field], 'extra')
        Assert-CardinalityThrows {
            & $module {
                param($entry, $hash)
                Assert-Stage5DevelopmentReadinessArguments $entry $hash `
                    'JSON array cardinality readiness arguments mutation'
            } $mutatedEntry $readinessHash
        } "Readiness arguments rejects an array-valued '$field'."
    }
    $mutatedArgumentsEntry = Copy-CardinalityOrderedObject $readinessEntry
    $mutatedArguments = @($readinessEntry.arguments)
    $mutatedArguments[0] = [object[]]@('-headless', 'extra')
    $mutatedArgumentsEntry.arguments = $mutatedArguments
    Assert-CardinalityThrows {
        & $module {
            param($entry, $hash)
            Assert-Stage5DevelopmentReadinessArguments $entry $hash `
                'JSON array cardinality readiness argument-token mutation'
        } $mutatedArgumentsEntry $readinessHash
    } 'Readiness arguments rejects an array-valued argument token.'

    # Keep the native-child and final-aggregation guard inventory durable even
    # though their complete fixtures are exercised by the bounded acceptance
    # partition.  This test must fail if a later edit removes the scalar guard
    # from any identity comparison at these high-risk boundaries.
    $nativeChildAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Read-Stage5FinalAcceptanceImmutableReceipt'
    }, $true))[0]
    $aggregationAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Invoke-Stage5FinalAcceptanceAggregation'
    }, $true))[0]
    $evidenceIdentityAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5FinalAcceptanceEvidenceIdentity'
    }, $true))[0]
    $nativeChildText = $nativeChildAst.Extent.Text
    $aggregationText = $aggregationAst.Extent.Text
    $evidenceIdentityText = $evidenceIdentityAst.Extent.Text
    foreach ($guard in @(
            '\$nativeEvidenceKind\s+-is\s+\[string\]',
            '\$nativeStatus\s+-is\s+\[string\]',
            '\$nativeSourceCommit\s+-is\s+\[string\]',
            '\$nativeArtifactSetSha256\s+-is\s+\[string\]')) {
        Assert-Cardinality ($nativeChildText -match $guard) `
            "Native child receipt scalar guard is missing: $guard"
    }
    foreach ($guard in @(
            '\$artifactSetSourceCommit\s+-is\s+\[string\]',
            '\$artifactSetArchitecture\s+-is\s+\[string\]')) {
        Assert-Cardinality ($aggregationText -match $guard) `
            "Final aggregation scalar guard is missing: $guard"
    }
    foreach ($guard in @(
            '\$EvidenceKind\s+-is\s+\[string\]',
            '\$Status\s+-is\s+\[string\]',
            '\$SourceCommit\s+-is\s+\[string\]',
            '\$Architecture\s+-is\s+\[string\]',
            '\$ArtifactSetSha256\s+-is\s+\[string\]')) {
        Assert-Cardinality ($evidenceIdentityText -match $guard) `
            "Final evidence identity scalar guard is missing: $guard"
    }
    $evidenceIdentityCalls = @($aggregationAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Assert-Stage5FinalAcceptanceEvidenceIdentity'
    }, $true))
    Assert-Cardinality ($evidenceIdentityCalls.Count -eq 1) `
        'Final aggregation must use one shared evidence identity guard.'
    $evidenceIdentityCommandElements = @(
        $evidenceIdentityCalls[0].CommandElements | ForEach-Object {
            $_.Extent.Text
        })
    $evidenceIdentityBindings = @(
        '-SchemaVersion', '$evidenceSchemaVersion',
        '-EvidenceKind', '$evidenceKind',
        '-Status', '$evidenceStatus',
        '-SourceCommit', '$evidenceSourceCommit',
        '-Architecture', '$evidenceArchitecture',
        '-ArtifactSetSha256', '$evidenceArtifactSetSha256',
        '-ExpectedKind', '$kind',
        '-ExpectedSourceCommit', '$sourceCommit',
        '-ExpectedArtifactSetSha256', '$artifactSetHash')
    for ($bindingIndex = 0; $bindingIndex -lt $evidenceIdentityBindings.Count;
            $bindingIndex += 2) {
        $parameterName = $evidenceIdentityBindings[$bindingIndex]
        $expectedValue = $evidenceIdentityBindings[$bindingIndex + 1]
        $parameterIndex = [Array]::IndexOf($evidenceIdentityCommandElements, $parameterName)
        Assert-Cardinality ($parameterIndex -ge 0 -and
            [Array]::LastIndexOf($evidenceIdentityCommandElements, $parameterName) -eq $parameterIndex -and
            $parameterIndex + 1 -lt $evidenceIdentityCommandElements.Count -and
            $evidenceIdentityCommandElements[$parameterIndex + 1] -ceq $expectedValue) `
            "Final aggregation must bind $parameterName to $expectedValue through the evidence identity guard."
    }
    $runtimeClosureAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5RuntimeClosureBinding'
    }, $true))[0]
    $qualificationReaderText = $nativeChildText
    $readinessArgumentsAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5DevelopmentReadinessArguments'
    }, $true))[0]
    $readinessEvidenceAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5DevelopmentReadinessExecutionEvidence'
    }, $true))[0]
    $launcherContractAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5LockstepLauncherContract'
    }, $true))[0]
    $peerLaunchAst = @($moduleAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Stage5LockstepPeerLaunchBinding'
    }, $true))[0]
    foreach ($spec in @(
            [ordered]@{ text = $runtimeClosureAst.Extent.Text; guard = '\$expectedManifestHash\s+-is\s+\[string\]' },
            [ordered]@{ text = $runtimeClosureAst.Extent.Text; guard = '\$expectedClosureHash\s+-is\s+\[string\]' },
            [ordered]@{ text = $qualificationReaderText; guard = '\$qualificationDataPath\s+-is\s+\[string\]' },
            [ordered]@{ text = $qualificationReaderText; guard = '\$externalKind\s+-is\s+\[string\]' },
            [ordered]@{ text = $qualificationReaderText; guard = '\$externalReviewedUtc\s+-is\s+\[string\]' },
            [ordered]@{ text = $qualificationReaderText; guard = '\$sourceCorpusTitle0\s+-is\s+\[string\]' },
            [ordered]@{ text = $readinessArgumentsAst.Extent.Text; guard = '\$configuration\s+-is\s+\[string\]' },
            [ordered]@{ text = $readinessArgumentsAst.Extent.Text; guard = '\$actual\s+-is\s+\[Array\]' },
            [ordered]@{ text = $readinessEvidenceAst.Extent.Text; guard = '\$executableSha256\s+-is\s+\[string\]' },
            [ordered]@{ text = $readinessEvidenceAst.Extent.Text; guard = '\$planExecutablePath\s+-is\s+\[string\]' },
            [ordered]@{ text = $readinessEvidenceAst.Extent.Text; guard = '\$executionRole\s+-is\s+\[string\]' },
            [ordered]@{ text = $launcherContractAst.Extent.Text; guard = '\$contractExecutable\s+-is\s+\[string\]' },
            [ordered]@{ text = $peerLaunchAst.Extent.Text; guard = '\$peerExecutableSha256\s+-is\s+\[string\]' },
            [ordered]@{ text = $peerLaunchAst.Extent.Text; guard = '\$peerCommandLine\s+-is\s+\[string\]' })) {
        Assert-Cardinality ($spec.text -match $spec.guard) `
            "Required scalar guard is missing: $($spec.guard)"
    }
}
finally {
    if (Test-Path -LiteralPath $scratchRoot) {
        Remove-Item -LiteralPath $scratchRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Output 'Stage 5 JSON array cardinality regression passed (array shape, protected attestation identity, runtime closure identity, native-child and aggregation scalar guard coverage).'
