[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [string]$ScratchRoot = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$resolvedScratchRoot = if (-not [string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot
}
elseif (-not [string]::IsNullOrWhiteSpace(
        $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else {
    throw 'Stage 5 combined host-runner tests require an explicit H: scratch root.'
}
$resolvedScratchRoot = [IO.Path]::GetFullPath($resolvedScratchRoot).TrimEnd('\')
if (-not $resolvedScratchRoot.StartsWith('H:\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage 5 combined host-runner test scratch must remain on H:.'
}
[IO.Directory]::CreateDirectory($resolvedScratchRoot) | Out-Null

function Assert-CombinedTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-CombinedThrows {
    param(
        [scriptblock]$Action,
        [string]$Pattern,
        [string]$Message
    )
    $caught = $null
    try { & $Action }
    catch { $caught = $_.Exception }
    Assert-CombinedTest ($null -ne $caught -and
        $caught.Message -match $Pattern) $Message
}

function Get-JsonProperty {
    param([object]$Object, [string]$Name, [string]$Context)
    Assert-CombinedTest ($null -ne $Object) "$Context is missing."
    $property = $Object.PSObject.Properties[$Name]
    Assert-CombinedTest ($null -ne $property) "$Context is missing property '$Name'."
    return $property.Value
}

function Get-OptionalJsonProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-Definition {
    param([object]$Definitions, [string]$Name)
    $property = $Definitions.PSObject.Properties[$Name]
    Assert-CombinedTest ($null -ne $property) "Schema definition '$Name' is missing."
    return $property.Value
}

function Get-RoleBranch {
    param([object]$Definition, [string]$Role)
    $allOf = @(Get-OptionalJsonProperty $Definition 'allOf')
    $branches = @($allOf | ForEach-Object {
        @((Get-OptionalJsonProperty $_ 'oneOf'))
    } | Where-Object { $null -ne $_ })
    $matches = @($branches | Where-Object {
        $properties = Get-OptionalJsonProperty $_ 'properties'
        $roleSchema = Get-OptionalJsonProperty $properties 'role'
        $roleConst = Get-OptionalJsonProperty $roleSchema 'const'
        [string]$roleConst -ceq $Role
    })
    Assert-CombinedTest ($matches.Count -eq 1) `
        "Schema must contain exactly one '$Role' host-runner branch."
    return $matches[0]
}

function Get-PathRefs {
    param([object]$Schema)
    return @((Get-OptionalJsonProperty $Schema 'oneOf') | ForEach-Object {
        [string](Get-OptionalJsonProperty $_ '$ref')
    })
}

function Assert-CombinedDirectJsonArrayAssignment {
    param(
        [object]$Ast,
        [string]$FunctionName,
        [string]$VariableName,
        [string]$PropertyName,
        [string]$Context
    )
    $functionDefinitions = @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $FunctionName
    }, $true))
    Assert-CombinedTest ($functionDefinitions.Count -eq 1) `
        "$Context must contain exactly one '$FunctionName' function."
    $assignments = @($functionDefinitions[0].Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            [string]$node.Left -ceq ('$' + $VariableName)
    }, $true))
    Assert-CombinedTest ($assignments.Count -eq 1) `
        "$Context must contain exactly one '$VariableName' assignment."
    $right = $assignments[0].Right
    $expression = if ($right -is [Management.Automation.Language.CommandExpressionAst]) {
        $right.Expression
    }
    else { $right }
    Assert-CombinedTest ($expression -is [Management.Automation.Language.PipelineAst]) `
        "$Context must assign the JSON array value directly without an array wrapper."
    $commands = @($expression.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Get-Stage5JsonValue'
    }, $true))
    Assert-CombinedTest ($commands.Count -eq 1) `
        "$Context must invoke Get-Stage5JsonValue exactly once."
    $propertyArguments = @($commands[0].CommandElements | Where-Object {
        $_ -is [Management.Automation.Language.StringConstantExpressionAst] -and
            $_.Value -ceq $PropertyName
    })
    Assert-CombinedTest ($propertyArguments.Count -eq 1) `
        "$Context must read the '$PropertyName' JSON property."
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$toolsRoot = Join-Path $root 'Core\Tools\DeterministicSimulationValidation'
$producerPath = Join-Path $toolsRoot 'New-Stage5CombinedHostRunnerReceipt.ps1'
$evidenceModulePath = Join-Path $toolsRoot 'DeterministicSimulationEvidence.psm1'
$schemaPath = Join-Path $toolsRoot 'Stage5ImmutableEvidenceReceipt.schema.json'
$workflowPath = Join-Path $root '.github\workflows\ci.yml'

foreach ($path in @($producerPath, $evidenceModulePath, $schemaPath, $workflowPath)) {
    Assert-CombinedTest (Test-Path -LiteralPath $path -PathType Leaf) `
        "Required Stage 5 combined contract file is missing: $path"
}

$tokens = $null
$errors = $null
$producerAst = [Management.Automation.Language.Parser]::ParseFile(
    $producerPath, [ref]$tokens, [ref]$errors)
Assert-CombinedTest ($errors.Count -eq 0) `
    "Combined producer must parse: $($errors -join '; ')"

$parameterNames = @($producerAst.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
$expectedParameters = @(
    'GeneralsReceiptPath', 'ZeroHourReceiptPath',
    'GeneralsReviewedFixtureReceiptPath',
    'GeneralsReviewedFixtureReceiptSha256',
    'ZeroHourReviewedFixtureReceiptPath',
    'ZeroHourReviewedFixtureReceiptSha256', 'OutputPath',
    'ExpectedSourceCommit', 'ExpectedArtifactSetSha256',
    'ExpectedGeneralsExecutableSha256', 'ExpectedZeroHourExecutableSha256',
    'ExpectedCohortNonce', 'ExpectedCohortCreatedUtc'
)
Assert-CombinedTest (($parameterNames -join '|') -ceq ($expectedParameters -join '|')) `
    "Combined producer parameter contract changed: $($parameterNames -join ', ')"

$stringLiterals = @($producerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst]
}, $true) | ForEach-Object { $_.Value })
foreach ($obsolete in @('renderer', 'renderThread', 'sourceReceipts', 'parallel')) {
    Assert-CombinedTest (-not ($stringLiterals -ccontains $obsolete)) `
        "Combined producer contains obsolete or unsafe projection field '$obsolete'."
}
foreach ($requiredLiteral in @(
        'serial', 'sourceCorpora', 'QualificationData.json',
        'validation-plan', 'validation-results', 'replay-results', 'ai-results')) {
    Assert-CombinedTest ($stringLiterals -ccontains $requiredLiteral) `
        "Combined producer is missing required contract literal '$requiredLiteral'."
}

$commands = @($producerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst]
}, $true))
foreach ($requiredCommand in @(
        'Read-Stage5FinalAcceptanceImmutableReceipt',
        'Get-Stage5FinalAcceptanceNativeRelocationBinding',
        'Read-Stage5SimulationQualificationDataEvidence',
        'Read-CombinedReviewedFixtureReceipt',
        'Get-CombinedFileSnapshot', 'Get-CombinedUtf8Sha256')) {
    Assert-CombinedTest (@($commands | Where-Object {
        $_.GetCommandName() -ceq $requiredCommand
    }).Count -ge 1) "Combined producer does not invoke '$requiredCommand'."
}

$moduleTokens = $null
$moduleErrors = $null
$moduleAst = [Management.Automation.Language.Parser]::ParseFile(
    $evidenceModulePath, [ref]$moduleTokens, [ref]$moduleErrors)
Assert-CombinedTest ($moduleErrors.Count -eq 0) `
    "Stage 5 evidence module must parse: $($moduleErrors -join '; ')"
foreach ($arrayCaller in @(
        [pscustomobject]@{
            FunctionName = 'Get-Stage5FinalAcceptanceNativeRelocationBinding'
            VariableName = 'children'; PropertyName = 'children'
        },
        [pscustomobject]@{
            FunctionName = 'Get-Stage5FinalAcceptanceNativeRelocationBinding'
            VariableName = 'rawLogs'; PropertyName = 'rawLogs'
        },
        [pscustomobject]@{
            FunctionName = 'Assert-Stage5CombinedNativeRawBindings'
            VariableName = 'rawLogs'; PropertyName = 'rawLogs'
        },
        [pscustomobject]@{
            FunctionName = 'Assert-Stage5CombinedHostSourceBindings'
            VariableName = 'sourceCorpora'; PropertyName = 'sourceCorpora'
        },
        [pscustomobject]@{
            FunctionName = 'Assert-Stage5CombinedHostSourceBindings'
            VariableName = 'fixtureEntries'; PropertyName = 'fixtures'
        },
        [pscustomobject]@{
            FunctionName = 'Assert-Stage5CombinedHostSourceBindings'
            VariableName = 'receiptBindings'; PropertyName = 'receipts'
        })) {
    Assert-CombinedDirectJsonArrayAssignment $moduleAst `
        $arrayCaller.FunctionName $arrayCaller.VariableName `
        $arrayCaller.PropertyName "Evidence module array caller $($arrayCaller.FunctionName)/$($arrayCaller.VariableName)"
}
Assert-CombinedDirectJsonArrayAssignment $producerAst `
    'Copy-CombinedReviewedFixtureClosure' 'fixtures' 'fixtures' `
    'Combined reviewed-fixture closure array caller'

Import-Module $evidenceModulePath -Force -PassThru | Out-Null
$evidenceModule = Get-Module -Name ([IO.Path]::GetFileNameWithoutExtension($evidenceModulePath))
Assert-CombinedTest ($null -ne $evidenceModule) `
    'Stage 5 evidence module must import before raw-log contract checks.'
$exportedRawLogCommands = @(Get-Command -Name Get-Stage5DevelopmentReadinessRawLog `
    -Module $evidenceModule.Name -ErrorAction SilentlyContinue)
Assert-CombinedTest ($exportedRawLogCommands.Count -eq 1) `
    'Get-Stage5DevelopmentReadinessRawLog must be exported by the evidence module.'

$validRawLog = [pscustomobject]@{
    name = 'validation-plan.json'
    path = 'validation-plan.json'
    sha256 = ('A' * 64)
    snapshot = [pscustomobject]@{
        path = 'validation-plan.json'
        sha256 = ('A' * 64)
        bytes = [byte[]](0x7B, 0x7D)
    }
}
$selectedRawLog = Get-Stage5DevelopmentReadinessRawLog `
    -RawLogs @($validRawLog) -LeafName 'validation-plan.json' `
    -Context 'valid raw-log record'
Assert-CombinedTest ($selectedRawLog -eq $validRawLog -and
    [string]$selectedRawLog.name -ceq 'validation-plan.json' -and
    $null -ne $selectedRawLog.snapshot.bytes -and
    [string]$selectedRawLog.sha256 -ceq ('A' * 64)) `
    'A valid retained raw-log record must be selected with its snapshot and hash.'

Assert-CombinedThrows {
    Get-Stage5DevelopmentReadinessRawLog -RawLogs @($validRawLog) `
        -LeafName 'missing.json' -Context 'missing raw-log record'
} 'exactly one retained raw log named' `
    'A missing retained raw-log record must be rejected.'

Assert-CombinedThrows {
    Get-Stage5DevelopmentReadinessRawLog -RawLogs @($validRawLog, $validRawLog) `
        -LeafName 'validation-plan.json' -Context 'duplicate raw-log record'
} 'exactly one retained raw log named' `
    'Duplicate retained raw-log records must be rejected.'

$malformedSnapshotRawLog = [pscustomobject]@{
    name = 'validation-plan.json'
    path = 'validation-plan.json'
    sha256 = ('A' * 64)
    snapshot = [pscustomobject]@{
        path = 'validation-plan.json'
        sha256 = ('A' * 64)
        bytes = $null
    }
}
Assert-CombinedThrows {
    Get-Stage5DevelopmentReadinessRawLog -RawLogs @($malformedSnapshotRawLog) `
        -LeafName 'validation-plan.json' -Context 'malformed snapshot raw-log record'
} 'snapshot is unavailable or malformed' `
    'A retained raw-log record with a malformed snapshot must be rejected.'

$malformedHashRawLog = [pscustomobject]@{
    name = 'validation-plan.json'
    path = 'validation-plan.json'
    sha256 = ('A' * 63)
    snapshot = [pscustomobject]@{
        path = 'validation-plan.json'
        sha256 = ('A' * 64)
        bytes = [byte[]](0x7B, 0x7D)
    }
}
Assert-CombinedThrows {
    Get-Stage5DevelopmentReadinessRawLog -RawLogs @($malformedHashRawLog) `
        -LeafName 'validation-plan.json' -Context 'malformed hash raw-log record'
} 'snapshot is unavailable or malformed' `
    'A retained raw-log record with a malformed hash must be rejected.'

foreach ($functionName in @('Assert-CombinedCondition',
        'Assert-CombinedRelativePath', 'Get-CombinedPathParts',
        'Get-CombinedRelativePathFromBase', 'Get-CombinedUtf8Sha256',
        'Copy-CombinedReviewedFixtureClosure')) {
    $functionDefinition = @($producerAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $functionName
    }, $true))
    Assert-CombinedTest ($functionDefinition.Count -eq 1) `
        "Combined fixture-count regression cannot load '$functionName'."
    Invoke-Expression $functionDefinition[0].Extent.Text
}
${script:combinedClosureCopiedDestinations} =
    New-Object 'Collections.Generic.List[string]'
function Copy-CombinedSourceFile {
    param(
        [string]$SourceBase,
        [string]$RelativePath,
        [string]$DestinationBase,
        [string]$DestinationRelativePath,
        [string]$ExpectedSha256,
        [string]$Context,
        [hashtable]$Copied,
        [object]$SourceSnapshot = $null,
        [string]$EvidenceKind = 'RawLog'
    )
    [void]${script:combinedClosureCopiedDestinations}.Add(
        $DestinationRelativePath.Replace('/', '\'))
    return $DestinationRelativePath.Replace('/', '\')
}
$fixtureEntries = New-Object 'Collections.Generic.List[object]'
for ($fixtureIndex = 1; $fixtureIndex -le 10; ++$fixtureIndex) {
    $fixtureId = 'fixture-{0:D2}' -f $fixtureIndex
    $fixtureEntries.Add([ordered]@{
        id = $fixtureId
        source = "replays\$fixtureId.rep"
        sha256 = ('F' * 63) + $fixtureIndex.ToString('X')
        stress = ($fixtureIndex -eq 1)
    }) | Out-Null
}
$fixtureManifestPath = Join-Path $resolvedScratchRoot `
    'stage5-combined-fixture-count-source\fixture-manifest.json'
$reviewed = [pscustomobject]@{
    path = Join-Path $resolvedScratchRoot `
        'stage5-combined-fixture-count-source\reviewed-receipt.json'
    sha256 = ('A' * 64)
    snapshot = [pscustomobject]@{
        path = Join-Path $resolvedScratchRoot `
            'stage5-combined-fixture-count-source\reviewed-receipt.json'
        sha256 = ('A' * 64)
        bytes = [byte[]](0x7B, 0x7D)
    }
    read = [pscustomobject]@{
        document = [ordered]@{
            protection = [ordered]@{
                kind = 'reviewed-fixture-attestation'
                path = 'protection.json'
                sha256 = ('B' * 64)
            }
        }
        provenance = [ordered]@{
            fixtureManifest = [ordered]@{
                path = 'fixture-manifest.json'
                sha256 = ('C' * 64)
            }
        }
        reviewedFixtureManifest = [ordered]@{
            fixtures = $fixtureEntries.ToArray()
        }
        reviewedFixtureManifestSnapshot = [pscustomobject]@{
            path = $fixtureManifestPath
            sha256 = ('C' * 64)
            bytes = [byte[]](0x7B, 0x7D)
        }
    }
}
$fixtureClosure = Copy-CombinedReviewedFixtureClosure `
    -Title 'Generals' -Reviewed $reviewed `
    -OutputDirectory (Join-Path $resolvedScratchRoot `
        'stage5-combined-fixture-count-output') `
    -Copied @{}
$closureCopies = @(${script:combinedClosureCopiedDestinations}.ToArray())
Assert-CombinedTest ([int]$fixtureClosure.fileCount -eq 13 -and
    $closureCopies.Count -eq 13 -and
    @($closureCopies | Sort-Object -Unique).Count -eq 13) `
    'Combined reviewed-fixture closure must retain exactly 13 unique files.'
Assert-CombinedTest (@($closureCopies | Where-Object {
        $_ -match '\\replays\\'
    }).Count -eq 10) `
    'Combined reviewed-fixture closure must copy exactly ten replay fixtures.'
Assert-CombinedTest ([string]$fixtureClosure.closureSha256 -cmatch '^[0-9A-F]{64}$') `
    'Combined reviewed-fixture closure must return a canonical closure hash.'
Remove-Item -Path Function:Copy-CombinedSourceFile -Force -ErrorAction SilentlyContinue
${script:combinedClosureCopiedDestinations} = $null

$typeFixtureRoot = Join-Path $resolvedScratchRoot `
    ('stage5-combined-relocation-type-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $typeFixtureRoot -Force | Out-Null
try {
    $rawPath = Join-Path $typeFixtureRoot 'native.raw.log'
    $timingPath = Join-Path $typeFixtureRoot 'native.timing.log'
    [IO.File]::WriteAllText($rawPath, 'combined relocation raw fixture')
    [IO.File]::WriteAllText($timingPath, 'combined relocation timing fixture')
    $nativePath = Join-Path $typeFixtureRoot 'native.json'
    $native = [ordered]@{
        rawLogs = @(
            [ordered]@{ name = 'raw-log'; path = 'native.raw.log';
                sha256 = Get-Stage5FileSha256 $rawPath },
            [ordered]@{ name = 'timing'; path = 'native.timing.log';
                sha256 = Get-Stage5FileSha256 $timingPath }
        )
        provenance = [ordered]@{ receiptPath = 'native.json' }
    }
    [IO.File]::WriteAllText($nativePath, ($native | ConvertTo-Json -Depth 12))
    $nativeHash = Get-Stage5FileSha256 $nativePath
    $child = [ordered]@{
        sequence = 1
        runNonce = '11111111-1111-4111-8111-111111111111'
        nativeReceipt = [ordered]@{
            path = 'native.json'
            sha256 = $nativeHash
            producer = 'game-executable-stage5-performance-report-v5'
            runNonce = '11111111-1111-4111-8111-111111111111'
            cohortNonce = '22222222-2222-4222-8222-222222222222'
        }
    }
    $hostReceiptPath = Join-Path $typeFixtureRoot 'host-receipt.json'
    $hostReceipt = [ordered]@{
        trustDomain = 'host-runner'
        provenance = [ordered]@{ children = @($child) }
    }
    [IO.File]::WriteAllText($hostReceiptPath,
        ($hostReceipt | ConvertTo-Json -Depth 12))

    $relocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
        -Path $hostReceiptPath -EvidenceDirectory $typeFixtureRoot
    $relocationChild = @($relocation.children)[0]
    Assert-CombinedTest ($relocationChild -is [Collections.IDictionary]) `
        'Native relocation must return each child as an IDictionary.'
    Assert-Stage5JsonShape $relocationChild `
        @('sequence', 'runNonce', 'evidenceDirectory',
            'nativeRawBindings', 'nativeReceiptSourcePath') `
        'positive relocation child type contract'

    $roundTripPath = Join-Path $typeFixtureRoot 'relocation-child.json'
    [IO.File]::WriteAllText($roundTripPath,
        ($relocationChild | ConvertTo-Json -Depth 12))
    $roundTripChild = ConvertFrom-Stage5JsonDictionary $roundTripPath
    Assert-CombinedTest ($roundTripChild -is [Collections.IDictionary]) `
        'Relocation child JSON round-trip must deserialize as an IDictionary.'
    Assert-Stage5JsonShape $roundTripChild `
        @('sequence', 'runNonce', 'evidenceDirectory',
            'nativeRawBindings', 'nativeReceiptSourcePath') `
        'positive relocation child JSON round-trip'

    $pscChild = [pscustomobject]@{
        sequence = 1
        runNonce = '11111111-1111-4111-8111-111111111111'
        evidenceDirectory = $typeFixtureRoot
        nativeRawBindings = @()
        nativeReceiptSourcePath = 'native.json'
    }
    Assert-CombinedThrows {
        Assert-Stage5JsonShape $pscChild `
            @('sequence', 'runNonce', 'evidenceDirectory',
                'nativeRawBindings', 'nativeReceiptSourcePath') `
            'negative PSCustomObject relocation child type contract'
    } 'must be a JSON object' `
        'PSCustomObject relocation children must remain rejected by strict shape readers.'
}
finally {
    if (Test-Path -LiteralPath $typeFixtureRoot) {
        Remove-Item -LiteralPath $typeFixtureRoot -Recurse -Force
    }
}

$combinedConditionDefinition = @($producerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-CombinedCondition'
}, $true))
Assert-CombinedTest ($combinedConditionDefinition.Count -eq 1) `
    'Combined producer must define one condition assertion function.'
Invoke-Expression $combinedConditionDefinition[0].Extent.Text

$qualificationBindingDefinition = @($producerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Get-CombinedQualificationDataBinding'
}, $true))
Assert-CombinedTest ($qualificationBindingDefinition.Count -eq 1) `
    'Combined producer must define one qualificationData binding function.'
Invoke-Expression $qualificationBindingDefinition[0].Extent.Text
$wrapperRoles = @('validation-plan', 'validation-results',
    'replay-results', 'ai-results')
foreach ($title in @('Generals', 'ZeroHour')) {
    $qualificationBinding = [ordered]@{
        path = 'QualificationData.json'
        title = $title
        manifestSha256 = ('A' * 64)
        closureSha256 = ('B' * 64)
        fileCount = 6
    }
    foreach ($role in $wrapperRoles) {
        # Read-Stage5FinalAcceptanceImmutableReceipt returns a PSCustomObject
        # envelope whose document/details/payload values remain strict JSON
        # dictionaries. Exercise that real boundary shape for every support
        # role and both title corpora.
        $document = [ordered]@{
            schemaVersion = 1
            evidenceKind = 'stage5-host-runner-receipt'
            status = 'passed'
            role = $role
            trustDomain = 'host-runner'
            producer = "installed-runtime-$role-v2"
            producerVersion = '2'
            sourceCommit = ('C' * 40)
            title = $title
            architecture = 'x64'
            artifactSetSha256 = ('D' * 64)
            recordedUtc = '2026-09-01T00:00:00Z'
            cohortNonce = '33333333-3333-4333-8333-333333333333'
            runtimeClosure = [ordered]@{
                dependencyManifestSha256 = ('E' * 64)
                closureSha256 = ('F' * 64)
            }
            details = [ordered]@{ qualificationData = $qualificationBinding }
        }
        $wrapper = [pscustomobject]@{
            role = $role
            trustDomain = 'host-runner'
            runNonce = '44444444-4444-4444-8444-444444444444'
            producer = "installed-runtime-$role-v2"
            producerVersion = '2'
            cohortNonce = '33333333-3333-4333-8333-333333333333'
            runtimeClosure = $document.runtimeClosure
            path = "sources\$title\$role-receipt.json"
            sha256 = ('1' * 64)
            snapshot = [pscustomobject]@{ bytes = [byte[]]@();
                path = "sources\$title\$role-receipt.json" }
            document = $document
            details = $document.details
            rawLogs = @()
            validatedChildren = @()
            provenance = [ordered]@{}
            protection = $null
            reviewedFixtureManifest = $null
            reviewedFixtureManifestSnapshot = $null
            qualificationData = $qualificationBinding
            qualificationDataEvidence = $null
            combinedSourceBindings = $null
            acceptanceFailure = $null
        }
        $actualBinding = Get-CombinedQualificationDataBinding `
            -Receipt $wrapper -Title $title `
            -Context "$title source corpus $role"
        Assert-CombinedTest ($actualBinding -is [Collections.IDictionary] -and
            [string]$actualBinding.path -ceq 'QualificationData.json' -and
            [string]$actualBinding.title -ceq $title -and
            [string]$actualBinding.manifestSha256 -ceq ('A' * 64) -and
            [string]$actualBinding.closureSha256 -ceq ('B' * 64) -and
            [int]$actualBinding.fileCount -eq 6) `
            "Immutable-read wrapper qualificationData binding failed for $title/$role."
    }
}
$pscDocumentWrapper = [pscustomobject]@{
    document = [pscustomobject]@{
        details = [ordered]@{ qualificationData = [ordered]@{
            path = 'QualificationData.json'; title = 'Generals'
            manifestSha256 = ('A' * 64); closureSha256 = ('B' * 64); fileCount = 6
        } }
    }
}
Assert-CombinedThrows {
    Get-CombinedQualificationDataBinding -Receipt $pscDocumentWrapper `
        -Title 'Generals' -Context 'negative PSCustomObject document wrapper'
} 'must be a JSON object' `
    'QualificationData binding must fail closed when the immutable wrapper document is not a dictionary.'

$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
$definitions = Get-JsonProperty $schema '$defs' 'Stage 5 immutable-receipt schema'
$requiredRootFields = @((Get-JsonProperty $schema 'required' 'Stage 5 schema root') | ForEach-Object { [string]$_ })
foreach ($field in @(
        'runtimeClosure', 'details', 'provenance', 'cohortNonce')) {
    Assert-CombinedTest ($requiredRootFields -ccontains $field) `
        "Schema root must require '$field'."
}

$nativeRawBinding = Get-Definition $definitions 'nativeRawBinding'
$nativeSourcePath = Get-JsonProperty (Get-JsonProperty $nativeRawBinding 'properties' 'nativeRawBinding') `
    'sourcePath' 'nativeRawBinding properties'
$nativeSourceRefs = @(Get-PathRefs $nativeSourcePath)
Assert-CombinedTest ($nativeSourceRefs.Count -eq 2 -and
    $nativeSourceRefs -ccontains '#/$defs/relativePath' -and
    $nativeSourceRefs -ccontains '#/$defs/absoluteWindowsPath') `
    'Native sourcePath must preserve the producer relative-or-absolute path contract.'

$combinedChild = Get-Definition $definitions 'combinedHostChild'
$combinedChildConstraint = @($combinedChild.allOf)[1]
$combinedChildRequired = @((Get-JsonProperty $combinedChildConstraint 'required' 'combinedHostChild') |
    ForEach-Object { [string]$_ })
foreach ($field in @('sourceSequence', 'qualificationData',
        'nativeReceiptSourcePath', 'nativeRawBindings')) {
    Assert-CombinedTest ($combinedChildRequired -ccontains $field) `
        "Combined child must require '$field'."
}
$combinedChildProperties = Get-JsonProperty $combinedChildConstraint 'properties' 'combinedHostChild'
$combinedRoleSchema = Get-JsonProperty $combinedChildProperties 'role' `
    'combinedHostChild properties'
$combinedRoleConst = Get-JsonProperty $combinedRoleSchema 'const' 'combined child role'
Assert-CombinedTest ([string]$combinedRoleConst -ceq 'combined-results') `
    'Combined child role must be combined-results.'
$combinedSequenceSchema = Get-JsonProperty $combinedChildProperties 'sourceSequence' `
    'combinedHostChild properties'
$combinedSequenceConst = Get-JsonProperty $combinedSequenceSchema 'const' `
    'combined child source sequence'
Assert-CombinedTest ([int]$combinedSequenceConst -eq 1) `
    'Combined child must project source sequence 1 only.'
$combinedNativeSourcePath = Get-JsonProperty $combinedChildProperties `
    'nativeReceiptSourcePath' 'combinedHostChild properties'
Assert-CombinedTest ((Get-PathRefs $combinedNativeSourcePath).Count -eq 2) `
    'Combined nativeReceiptSourcePath must accept the producer relative-or-absolute form.'

$validationChild = Get-Definition $definitions 'validationResultHostChild'
$validationChildConstraint = @($validationChild.allOf)[1]
$validationChildRequired = @((Get-JsonProperty $validationChildConstraint `
    'required' 'validationResultHostChild') | ForEach-Object { [string]$_ })
foreach ($field in @('sequence', 'arguments', 'qualificationData')) {
    Assert-CombinedTest ($validationChildRequired -ccontains $field) `
        "Validation-results child must require '$field'."
}

$validationProvenance = Get-Definition $definitions 'validationResultsHostProvenance'
$validationProvenanceProperties = Get-JsonProperty (@($validationProvenance.allOf)[1]) `
    'properties' 'validation-results provenance'
$validationChildren = Get-JsonProperty $validationProvenanceProperties `
    'children' 'validation-results provenance properties'
Assert-CombinedTest ([int]$validationChildren.minItems -eq 253 -and
    [int]$validationChildren.maxItems -eq 253) `
    'Validation-results provenance must contain exactly 253 children.'

$boundProvenance = Get-Definition $definitions 'boundHostProvenance'
$boundProvenanceProperties = Get-JsonProperty (@($boundProvenance.allOf)[1]) `
    'properties' 'bound provenance'
$boundChildren = Get-JsonProperty $boundProvenanceProperties `
    'children' 'bound provenance properties'
Assert-CombinedTest ([int]$boundChildren.minItems -eq 1 -and
    [int]$boundChildren.maxItems -eq 1) `
    'Bound support provenance must contain exactly one child.'

$planProvenance = Get-Definition $definitions 'planHostProvenance'
$planProvenanceProperties = Get-JsonProperty (@($planProvenance.allOf)[1]) `
    'properties' 'plan provenance'
$planChildren = Get-JsonProperty $planProvenanceProperties `
    'children' 'plan provenance properties'
Assert-CombinedTest ([int]$planChildren.maxItems -eq 0) `
    'Validation-plan provenance must not claim child processes.'

$combinedProvenance = Get-Definition $definitions 'combinedHostProvenance'
$combinedProvenanceProperties = Get-JsonProperty (@($combinedProvenance.allOf)[1]) `
    'properties' 'combined provenance'
$combinedChildren = Get-JsonProperty $combinedProvenanceProperties `
    'children' 'combined provenance properties'
Assert-CombinedTest ([int]$combinedChildren.minItems -eq 2 -and
    [int]$combinedChildren.maxItems -eq 2 -and
    @($combinedChildren.prefixItems).Count -eq 2) `
    'Combined provenance must contain exactly two title children.'
$combinedTitles = @($combinedChildren.prefixItems | ForEach-Object {
    $allOf = @($_.allOf)
    $properties = Get-JsonProperty $allOf[1] 'properties' 'combined title child'
    [string](Get-JsonProperty (Get-JsonProperty $properties 'title' 'combined title child') `
        'const' 'combined title')
})
Assert-CombinedTest (($combinedTitles -join '|') -ceq 'Generals|ZeroHour') `
    'Combined provenance title order must be Generals then ZeroHour.'

$qdata = Get-Definition $definitions 'simulationQualificationDataBinding'
$qdataRequired = @((Get-JsonProperty $qdata 'required' 'qualificationData binding') |
    ForEach-Object { [string]$_ })
Assert-CombinedTest (($qdataRequired -join '|') -ceq
    'path|title|manifestSha256|closureSha256|fileCount') `
    'Per-title qualificationData must expose exactly the five manifest binding fields.'
Assert-CombinedTest ([int](Get-JsonProperty (
    Get-JsonProperty (Get-JsonProperty $qdata 'properties' 'qualificationData binding') `
        'fileCount' 'qualificationData properties') 'const' 'qualificationData fileCount') -eq 6) `
    'QualificationData binding must report six files.'

$reviewedFixture = Get-JsonProperty (Get-JsonProperty (Get-Definition $definitions `
    'combinedSourceCorpus') 'properties' 'combined source corpus') `
    'reviewedFixture' 'combined source corpus properties'
$reviewedFixtureRequired = @((Get-JsonProperty $reviewedFixture 'required' `
    'reviewed fixture closure') | ForEach-Object { [string]$_ })
foreach ($field in @('receipt', 'protection', 'manifest', 'fileCount', 'closureSha256')) {
    Assert-CombinedTest ($reviewedFixtureRequired -ccontains $field) `
        "Reviewed fixture closure must require '$field'."
}
Assert-CombinedTest ([int](Get-JsonProperty (
    Get-JsonProperty (Get-JsonProperty $reviewedFixture 'properties' 'reviewed fixture closure') `
        'fileCount' 'reviewed fixture properties') 'const' 'reviewed fixture fileCount') -eq 13) `
    'Reviewed fixture closure must contain exactly 13 files.'

$sourceCorpus = Get-Definition $definitions 'combinedSourceCorpus'
$sourceCorpusProperties = Get-JsonProperty $sourceCorpus 'properties' 'combined source corpus'
Assert-CombinedTest ([int](Get-JsonProperty (
    Get-JsonProperty $sourceCorpusProperties 'sourceChildCount' 'source corpus properties') `
    'const' 'source corpus child count') -eq 253) `
    'Each source corpus must contain exactly 253 children.'
$sourceReceipts = Get-JsonProperty $sourceCorpusProperties 'receipts' 'source corpus properties'
$sourceReceiptDefinition = Get-Definition $definitions 'combinedSourceReceipt'
$sourceReceiptRequired = @((Get-JsonProperty $sourceReceiptDefinition 'required' `
    'source receipt bindings') | ForEach-Object { [string]$_ })
foreach ($field in @('role', 'path', 'sha256', 'runNonce', 'cohortNonce')) {
    Assert-CombinedTest ($sourceReceiptRequired -ccontains $field) `
        "Source receipt binding must require independent '$field'."
}
$receiptRoles = @($sourceReceipts.prefixItems | ForEach-Object {
    $allOf = @($_.allOf)
    $properties = Get-JsonProperty $allOf[1] 'properties' 'source receipt item'
    [string](Get-JsonProperty (Get-JsonProperty $properties 'role' 'source receipt item') `
        'const' 'source receipt role')
})
Assert-CombinedTest (($receiptRoles -join '|') -ceq
    'validation-plan|validation-results|replay-results|ai-results') `
    'Each source corpus must bind all four support roles in canonical order.'

$combinedDetails = Get-Definition $definitions 'combinedDetails'
$detailProperties = Get-JsonProperty $combinedDetails 'properties' 'combined details'
foreach ($pair in @(
        @('pipelineMode', 'serial'), @('simulationMode', 'serial'),
        @('requestedWorkers', '1'), @('workerPolicy', 'auto'),
        @('projectionSequence', 1),
        @('projectionSemantics', 'deterministic-lineage-pointer'),
        @('sourceChildCount', 253), @('bothTitlesPassed', $true))) {
    $value = Get-JsonProperty (Get-JsonProperty $detailProperties $pair[0] 'combined detail properties') `
        'const' "combined detail $($pair[0])"
    Assert-CombinedTest ([string]$value -ceq [string]$pair[1]) `
        "Combined details has incorrect '$($pair[0])' projection."
}
$sourceCorpora = Get-JsonProperty $detailProperties 'sourceCorpora' 'combined detail properties'
Assert-CombinedTest ([int]$sourceCorpora.minItems -eq 2 -and
    [int]$sourceCorpora.maxItems -eq 2 -and
    @($sourceCorpora.prefixItems).Count -eq 2) `
    'Combined details must carry both title source corpora.'
$sourceCorpusTitles = @($sourceCorpora.prefixItems | ForEach-Object {
    $allOf = @($_.allOf)
    $properties = Get-JsonProperty $allOf[1] 'properties' 'combined source corpus title'
    [string](Get-JsonProperty (Get-JsonProperty $properties 'title' `
        'combined source corpus title') 'const' 'source corpus title')
})
Assert-CombinedTest (($sourceCorpusTitles -join '|') -ceq 'Generals|ZeroHour') `
    'Combined sourceCorpora title order must be Generals then ZeroHour.'

$combinedBranch = Get-RoleBranch (Get-Definition $definitions 'hostRunnerReceipt') `
    'combined-results'
$combinedBranchProperties = Get-JsonProperty $combinedBranch 'properties' 'combined host branch'
Assert-CombinedTest ([string](Get-JsonProperty (
    Get-JsonProperty $combinedBranchProperties 'producer' 'combined host branch') `
    'const' 'combined producer') -ceq 'installed-runtime-combined-results-v2') `
    'Combined host receipt producer must be the installed-runtime v2 producer.'
Assert-CombinedTest ([string](Get-JsonProperty (
    Get-JsonProperty $combinedBranchProperties 'details' 'combined host branch') `
    '$ref' 'combined details reference') -ceq '#/$defs/combinedDetails') `
    'Combined host receipt must use combinedDetails.'

foreach ($obsolete in @('renderer', 'renderThread', 'sourceReceipts', 'parallel')) {
    Assert-CombinedTest (-not ($detailProperties.PSObject.Properties.Name -ccontains $obsolete)) `
        "Schema combinedDetails declares obsolete field '$obsolete'."
}

$workflow = Get-Content -LiteralPath $workflowPath -Raw
foreach ($requiredWorkflowText in @(
        'New-Stage5CombinedHostRunnerReceipt.ps1',
        '-GeneralsReviewedFixtureReceiptPath',
        '-GeneralsReviewedFixtureReceiptSha256',
        '-ZeroHourReviewedFixtureReceiptPath',
        '-ZeroHourReviewedFixtureReceiptSha256',
        '-ExpectedArtifactSetSha256', '-ExpectedGeneralsExecutableSha256',
        '-ExpectedZeroHourExecutableSha256', '-ExpectedCohortNonce',
        '-ExpectedCohortCreatedUtc',
        'New-Stage5DevelopmentReadinessBundle.ps1',
        'Invoke-Stage5FinalAcceptance.ps1',
        'Seal-Stage5DevelopmentReadinessBundle.ps1')) {
    Assert-CombinedTest $workflow.Contains($requiredWorkflowText) `
        "CI workflow is missing required Stage 5 integration '$requiredWorkflowText'."
}
Assert-CombinedTest (([regex]::Matches($workflow, '-ExternalQualificationExempt')).Count -ge 3) `
    'CI must pass -ExternalQualificationExempt to assembler, final acceptance, and seal.'

Write-Output 'Stage 5 combined producer/schema/CI focused tests passed.'
