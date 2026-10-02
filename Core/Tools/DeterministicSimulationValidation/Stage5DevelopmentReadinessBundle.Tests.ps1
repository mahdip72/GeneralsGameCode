[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$ScratchRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-FunctionDefinition {
    param([object]$Ast, [string]$Name)
    $matches = @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $Name
    }, $true))
    Assert-Test ($matches.Count -eq 1) `
        "Assembler must define exactly one '$Name' function."
    return $matches[0]
}

function Get-CommandParameters {
    param([Management.Automation.Language.CommandAst]$Command)
    return @($Command.CommandElements | Where-Object {
        $_ -is [Management.Automation.Language.CommandParameterAst]
    } | ForEach-Object { $_.ParameterName })
}

function Get-ConstantNamedArgument {
    param(
        [Management.Automation.Language.CommandAst]$Command,
        [string]$Name
    )
    for ($index = 0; $index -lt $Command.CommandElements.Count - 1; ++$index) {
        $element = $Command.CommandElements[$index]
        if ($element -is [Management.Automation.Language.CommandParameterAst] -and
            $element.ParameterName -ceq $Name) {
            return $Command.CommandElements[$index + 1].SafeGetValue()
        }
    }
    return $null
}

function Get-LiteralHashtableValue {
    param(
        [Management.Automation.Language.HashtableAst]$Hashtable,
        [string]$Name
    )
    foreach ($pair in $Hashtable.KeyValuePairs) {
        if ([string]$pair.Item1.SafeGetValue() -ceq $Name) {
            return $pair.Item2.SafeGetValue()
        }
    }
    return $null
}

function Get-HashtableValueAst {
    param(
        [Management.Automation.Language.HashtableAst]$Hashtable,
        [string]$Name
    )
    foreach ($pair in $Hashtable.KeyValuePairs) {
        if ([string]$pair.Item1.SafeGetValue() -ceq $Name) {
            return $pair.Item2
        }
    }
    return $null
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$assemblerPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\New-Stage5DevelopmentReadinessBundle.ps1'
Assert-Test (Test-Path -LiteralPath $assemblerPath -PathType Leaf) `
    'The Stage 5 development-readiness assembler is missing.'

$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $assemblerPath, [ref]$tokens, [ref]$errors)
Assert-Test ($errors.Count -eq 0) `
    "Development-readiness assembler must parse: $($errors -join '; ')"

$parameterNames = @($ast.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
$expectedParameters = @(
    'AcceptanceTemplateRoot', 'AcceptanceTemplatePath', 'GeneralsEvidenceRoot',
    'ZeroHourEvidenceRoot', 'CombinedEvidenceRoot', 'LockstepEvidenceRoot',
    'PerformanceEvidenceRoot', 'InstalledKernelEvidencePath',
    'ExternalQualificationExempt', 'OutputRoot', 'ExpectedSourceCommit',
    'ExpectedCohortNonce', 'ExpectedCohortCreatedUtc',
    'ExpectedPhaseBaselineProfileSha256'
)
Assert-Test (($parameterNames -join '|') -ceq ($expectedParameters -join '|')) `
    "Assembler parameter contract changed: $($parameterNames -join ', ')"

$assemblyRecordedAssignments = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ceq 'script:AssemblyRecordedUtc'
}, $true))
Assert-Test ($assemblyRecordedAssignments.Count -eq 1) `
    'Assembler must define one canonical UTC envelope timestamp.'
$assemblyRecordedText = [string]$assemblyRecordedAssignments[0].Right.Extent.Text
Assert-Test ($assemblyRecordedText -match
    "\[DateTime\]::UtcNow\.ToString\('o',\s*\[Globalization\.CultureInfo\]::InvariantCulture\)") `
    'Envelope recordedUtc must use the canonical UTC Z representation.'

Import-Module (Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1') -Force

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$arrayValueContracts = @(
    [pscustomobject]@{ field = 'rawLogs'; expectedCalls = 2; expectedCount = 3 }
    [pscustomobject]@{ field = 'attachments'; expectedCalls = 1; expectedCount = 2 }
    [pscustomobject]@{ field = 'fixtures'; expectedCalls = 2; expectedCount = 2 }
    [pscustomobject]@{ field = 'artifacts'; expectedCalls = 1; expectedCount = 3 }
    [pscustomobject]@{ field = 'entries'; expectedCalls = 1; expectedCount = 4 }
    [pscustomobject]@{ field = 'arguments'; expectedCalls = 1; expectedCount = 5 }
    [pscustomobject]@{ field = 'seeds'; expectedCalls = 1; expectedCount = 3 }
    [pscustomobject]@{ field = 'scenarios'; expectedCalls = 1; expectedCount = 2 }
)
foreach ($contract in $arrayValueContracts) {
    $field = [string]$contract.field
    $calls = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Get-Stage5JsonValue' -and
            @($node.CommandElements | Where-Object {
                $_ -is [Management.Automation.Language.StringConstantExpressionAst] -and
                    $_.Value -ceq $field
            }).Count -eq 1
    }, $true))
    Assert-Test ($calls.Count -eq [int]$contract.expectedCalls) `
        "Assembler array contract '$field' changed its helper-call count."
    foreach ($call in $calls) {
        $ancestor = $call.Parent
        $wrapped = $false
        while ($null -ne $ancestor) {
            if ($ancestor -is [Management.Automation.Language.ArrayExpressionAst]) {
                $wrapped = $true
                break
            }
            $ancestor = $ancestor.Parent
        }
        Assert-Test (-not $wrapped) `
            "Assembler array contract '$field' wraps Get-Stage5JsonValue in an array expression."
    }
    $representative = [ordered]@{}
    $values = 1..([int]$contract.expectedCount) | ForEach-Object {
        "${field}-$_"
    }
    $representative[$field] = @($values)
    $observed = Get-Stage5JsonValue $representative $field `
        "Representative '$field' array contract"
    Assert-Test (@($observed).Count -eq [int]$contract.expectedCount) `
        "Representative '$field' array contract changed cardinality."
}

$resultLoopAsts = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.ForEachStatementAst] -and
        [string]$node.Extent.Text -like 'foreach ($result in $resultEntries)*'
}, $true))
Assert-Test ($resultLoopAsts.Count -eq 1) `
    'Assembler must validate each persisted validation result exactly once.'
$resultLoopText = [string]$resultLoopAsts[0].Extent.Text

$planLoopAsts = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.ForEachStatementAst] -and
        [string]$node.Extent.Text -like 'foreach ($planEntry in $planEntries)*' -and
        [string]$node.Extent.Text -match 'executionIdentityFields'
}, $true))
Assert-Test ($planLoopAsts.Count -eq 1) `
    'Assembler must validate each persisted validation-plan entry exactly once.'
$planLoopText = [string]$planLoopAsts[0].Extent.Text

$resultLoopScript = [scriptblock]::Create(
    "param([object[]]`$resultEntries)`n" +
    "`$planEntries = @([ordered]@{ placeholder = 1 })`n" +
    "`$resultsBySequence = @{}`n" +
    $resultLoopText)
$planLoopScript = [scriptblock]::Create(
    "param([object[]]`$planEntries)`n" +
    "`$resultsBySequence = @{ 1 = [ordered]@{} }`n" +
    "`$executionIdentityFields = @()`n" +
    $planLoopText)

function Assert-AssemblerRejects {
    param([scriptblock]$Action, [string]$Message)
    $caught = $null
    try { & $Action }
    catch { $caught = $_ }
    Assert-Test ($null -ne $caught) `
        "$Message (malformed persisted value was accepted)."
    Assert-Test ($caught.Exception.Message -match 'integer|Boolean') `
        "$Message (wrong rejection: $($caught.Exception.Message))."
}

$invalidResultCases = @(
    [pscustomobject]@{
        name = 'fractional persisted result sequence'
        value = [ordered]@{ sequence = [double]1.4; exitCode = [int]0; timedOut = $false }
    }
    [pscustomobject]@{
        name = 'fractional persisted result exitCode'
        value = [ordered]@{ sequence = [int]1; exitCode = [double]0.4; timedOut = $false }
    }
    [pscustomobject]@{
        name = 'numeric persisted result timedOut'
        value = [ordered]@{ sequence = [int]1; exitCode = [int]0; timedOut = [int]0 }
    }
)
foreach ($case in $invalidResultCases) {
    Assert-AssemblerRejects {
        & $resultLoopScript -resultEntries @($case.value)
    } $case.name
}

Assert-AssemblerRejects {
    & $planLoopScript -planEntries @([ordered]@{ sequence = [double]1.4 })
} 'fractional persisted plan sequence'

$stringLiterals = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst]
}, $true) | ForEach-Object { $_.Value })
foreach ($obsolete in @('sourceReceipts', 'renderer', 'renderThread')) {
    Assert-Test (-not ($stringLiterals -ccontains $obsolete)) `
        "Assembler still consumes obsolete combined field '$obsolete'."
}
Assert-Test ($stringLiterals -ccontains 'sourceCorpora') `
    'Assembler does not consume the current two-title sourceCorpora contract.'

$receiptAssignment = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ceq 'receiptSpecs'
}, $true))
Assert-Test ($receiptAssignment.Count -eq 1) `
    'Assembler must define one exact source receipt specification set.'
$receiptSpecs = @($receiptAssignment[0].Right.FindAll({
    param($node)
    $node -is [Management.Automation.Language.HashtableAst]
}, $true) | ForEach-Object {
    '{0}|{1}|{2}' -f
        (Get-LiteralHashtableValue $_ 'title'),
        (Get-LiteralHashtableValue $_ 'role'),
        (Get-LiteralHashtableValue $_ 'leaf')
} | Sort-Object -CaseSensitive)
$expectedReceiptSpecs = @(
    'Both|combined-results|combined-results-receipt.json',
    'Generals|ai-results|ai-results-receipt.json',
    'Generals|replay-results|replay-results-receipt.json',
    'Generals|validation-plan|validation-plan-receipt.json',
    'Generals|validation-results|validation-results-receipt.json',
    'ZeroHour|ai-results|ai-results-receipt.json',
    'ZeroHour|replay-results|replay-results-receipt.json',
    'ZeroHour|validation-plan|validation-plan-receipt.json',
    'ZeroHour|validation-results|validation-results-receipt.json'
)
Assert-Test (($receiptSpecs -join "`n") -ceq ($expectedReceiptSpecs -join "`n")) `
    "Assembler source receipt corpus is incomplete or title-swapped: $($receiptSpecs -join ', ')"

$templateFunction = Get-FunctionDefinition $ast 'Get-TemplateAttachment'
$templateParameters = @($templateFunction.Body.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
Assert-Test ($templateParameters -ccontains 'Title') `
    'Reviewed template attachment selection is not title-qualified.'
$titleComparisons = @($templateFunction.FindAll({
    param($node)
    $node -is [Management.Automation.Language.BinaryExpressionAst] -and
        $node.Operator -eq [Management.Automation.Language.TokenKind]::Ceq -and
        [string]$node.Left.Extent.Text -ceq '[string]$_.title' -and
        $node.Right -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Right.VariablePath.UserPath -ceq 'Title'
}, $true))
Assert-Test ($titleComparisons.Count -ge 1) `
    'Reviewed template attachment lookup does not compare attachment.title exactly.'

$attachmentFunction = Get-FunctionDefinition $ast 'New-Attachment'
$attachmentParameters = @($attachmentFunction.Body.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
Assert-Test ($attachmentParameters -ccontains 'Title') `
    'Acceptance attachments do not require an explicit title.'
$attachmentTables = @($attachmentFunction.FindAll({
    param($node)
    $node -is [Management.Automation.Language.HashtableAst]
}, $true))
$attachmentTitleValue = Get-HashtableValueAst $attachmentTables[0] 'title'
Assert-Test ($attachmentTables.Count -eq 1 -and
    $null -ne $attachmentTitleValue -and
    [string]$attachmentTitleValue.Extent.Text -ceq '$Title') `
    'Acceptance attachment output omits its exact title binding.'

$commands = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst]
}, $true))
$fixtureAttachmentCalls = @($commands | Where-Object {
    $_.GetCommandName() -ceq 'New-Attachment' -and
        [string](Get-ConstantNamedArgument $_ 'Role') -ceq
            'replay-fixture-manifest'
})
$fixtureAttachmentTitles = @($fixtureAttachmentCalls | ForEach-Object {
    [string](Get-ConstantNamedArgument $_ 'Title')
} | Sort-Object -CaseSensitive)
Assert-Test (($fixtureAttachmentTitles -join '|') -ceq 'Generals|ZeroHour') `
    'Replay determinism must attach one protected reviewed fixture authority per title.'

$executionCalls = @($commands | Where-Object {
    $_.GetCommandName() -ceq 'Assert-Stage5DevelopmentReadinessExecutionEvidence'
})
Assert-Test ($executionCalls.Count -ge 1) `
    'Assembler does not invoke the strict development-readiness execution validator.'
foreach ($call in $executionCalls) {
    $names = @(Get-CommandParameters $call)
    foreach ($required in @('ExpectedQualificationData',
            'ExpectedCurrentExecutablePath', 'RequireCurrentArtifactRelocation',
            'ExpectedCohortCreatedUtc', 'ExpectedTitle')) {
        Assert-Test ($names -ccontains $required) `
            "Execution validator call omits -$required."
    }
}

$installedReaderCalls = @($commands | Where-Object {
    $_.GetCommandName() -ceq 'Read-Stage5InstalledKernelExecutionEvidence'
})
Assert-Test ($installedReaderCalls.Count -eq 1) `
    'Present installed-kernel evidence must be independently strict-read once.'
foreach ($required in @('Path', 'ExpectedSha256', 'ExpectedSourceCommit',
        'ExpectedArtifactSetSha256', 'ExpectedCohortNonce',
        'ExpectedCohortCreatedUtc', 'ExpectedDependencyManifestSha256',
        'ExpectedRuntimeClosureSha256', 'GeneralsExecutableSha256',
        'ZeroHourExecutableSha256')) {
    Assert-Test ((Get-CommandParameters $installedReaderCalls[0]) -ccontains $required) `
        "Installed-kernel strict reader call omits -$required."
}

$xorNodes = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.BinaryExpressionAst] -and
        $node.Operator -eq [Management.Automation.Language.TokenKind]::Xor
}, $true))
Assert-Test ($xorNodes.Count -ge 1) `
    'Installed-kernel evidence path and external exemption are not mutually exclusive.'
Assert-Test ($stringLiterals -ccontains
    'external-qualification-exempt-and-reviewed-native-fixture-unavailable') `
    'External qualification exemption does not emit the exact reviewed reason.'

$evidenceAssignment = @($ast.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ceq 'evidenceRecords'
}, $true))
Assert-Test ($evidenceAssignment.Count -eq 1) `
    'Assembler must define one final evidence record map.'
$evidenceKinds = @($evidenceAssignment[0].Right.FindAll({
    param($node)
    $node -is [Management.Automation.Language.HashtableAst]
}, $true)[0].KeyValuePairs | ForEach-Object {
    [string]$_.Item1.SafeGetValue()
})
$expectedEvidenceKinds = @('deterministic-runtime', 'replay-determinism',
    'fresh-ai', 'performance-scaling', 'mixed-worker-multiplayer',
    'combined-stage4-stage5-installed-runtime')
Assert-Test (($evidenceKinds -join '|') -ceq ($expectedEvidenceKinds -join '|')) `
    "Assembler must preserve exactly six top-level evidence kinds: $($evidenceKinds -join ', ')"
$evidenceModulePath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1'
$evidenceModuleTokens = $null
$evidenceModuleErrors = $null
$evidenceModuleAst = [Management.Automation.Language.Parser]::ParseFile(
    $evidenceModulePath, [ref]$evidenceModuleTokens, [ref]$evidenceModuleErrors)
Assert-Test ($evidenceModuleErrors.Count -eq 0) `
    "Final-acceptance evidence module must parse: $($evidenceModuleErrors -join '; ')"
$aggregationFunction = Get-FunctionDefinition $evidenceModuleAst `
    'Invoke-Stage5FinalAcceptanceAggregation'
$attachmentBindingAssignments = @($aggregationFunction.FindAll({
    param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ceq 'attachmentBindings'
}, $true))
Assert-Test ($attachmentBindingAssignments.Count -eq 1 -and
    [string]$attachmentBindingAssignments[0].Right.Extent.Text -match
        "'deterministic-runtime'\s*=\s*@\('validation-plan\|ZeroHour',\s*'validation-results\|ZeroHour'\)") `
    'Final-acceptance reader must require exactly the plan and results attachments for deterministic-runtime.'

$runtimeEnvelopeFunction = Get-FunctionDefinition $ast `
    'New-Stage5DeterministicRuntimeEnvelope'
$runtimeEnvelopeText = [string]$runtimeEnvelopeFunction.Extent.Text
Assert-Test ($runtimeEnvelopeText -notmatch 'performance-report' -and
    $runtimeEnvelopeText -match 'PerformanceScalingEvidenceSha256') `
    'Deterministic-runtime envelope must not require a host-runner performance report and must only reference the separately validated scaling envelope.'
Assert-Test ($runtimeEnvelopeText -match "Write-EvidenceEnvelope 'deterministic-runtime'") `
    'Deterministic-runtime envelope helper no longer writes the runtime evidence envelope.'
$performanceScalingEnvelopeCalls = @($commands | Where-Object {
    $_.GetCommandName() -ceq 'Write-EvidenceEnvelope' -and
        $_.CommandElements.Count -gt 1 -and
        $_.CommandElements[1] -is [Management.Automation.Language.StringConstantExpressionAst] -and
        $_.CommandElements[1].Value -ceq 'performance-scaling'
})
Assert-Test ($performanceScalingEnvelopeCalls.Count -eq 1 -and
    [string]$performanceScalingEnvelopeCalls[0].Extent.Text -match
        '\$performancePath' -and
    [string]$performanceScalingEnvelopeCalls[0].Extent.Text -match
        "-Role 'performance-report'" -and
    [string]$performanceScalingEnvelopeCalls[0].Extent.Text -match
        "-Role 'stage3-baseline'" -and
    [string]$performanceScalingEnvelopeCalls[0].Extent.Text -match
        "-Role 'phase-baseline-profile'") `
    'Separate performance-scaling envelope must remain bound to the external scaling report.'

# Exercise the production attachment and deterministic-runtime envelope writers
# with current-cohort receipt fixtures. This is intentionally a bounded bundle
# component test rather than a synthetic claim that the complete 253-run
# readiness corpus has been assembled.
$scratchFull = [IO.Path]::GetFullPath($ScratchRoot)
$fixtureRoot = Join-Path $scratchFull `
    ('readiness-runtime-no-performance-' + [Guid]::NewGuid().ToString('N'))
$readinessRoot = Join-Path $fixtureRoot 'readiness'
$sourceRootFixture = Join-Path $readinessRoot 'Sources\ZeroHour'
try {
    [IO.Directory]::CreateDirectory($sourceRootFixture) | Out-Null
    foreach ($functionName in @('Get-ContainedRelativePath', 'New-Attachment',
            'Write-EvidenceEnvelope', 'New-Stage5DeterministicRuntimeEnvelope')) {
        $functionAst = Get-FunctionDefinition $ast $functionName
        Invoke-Expression ([string]$functionAst.Extent.Text)
    }

    $script:ReadinessRoot = $readinessRoot
    $script:ArtifactSetSha256 = 'B' * 64
    $script:AssemblyRecordedUtc = [DateTime]::UtcNow.ToString('o',
        [Globalization.CultureInfo]::InvariantCulture)
    $script:RuntimeClosure = [ordered]@{
        dependencyManifestSha256 = 'C' * 64
        closureSha256 = 'D' * 64
    }
    $script:InstalledKernelExecutionStatus = [ordered]@{
        status = 'skipped'
        claim = $false
        reason = 'external-qualification-exempt-and-reviewed-native-fixture-unavailable'
        sha256 = $null
    }
    $ExpectedSourceCommit = '1' * 40
    $ExpectedCohortNonce = '11111111-1111-4111-8111-111111111111'

    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    $runtimeClosure = [ordered]@{
        dependencyManifestSha256 = $script:RuntimeClosure.dependencyManifestSha256
        closureSha256 = $script:RuntimeClosure.closureSha256
    }
    $fixtureReceiptCommon = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-host-runner-receipt'
        status = 'passed'
        trustDomain = 'host-runner'
        producerVersion = '2'
        runNonce = '22222222-2222-4222-8222-222222222222'
        sourceCommit = $ExpectedSourceCommit
        title = 'ZeroHour'
        architecture = 'x64'
        artifactSetSha256 = $script:ArtifactSetSha256
        cohortNonce = $ExpectedCohortNonce
        runtimeClosure = $runtimeClosure
        executableSha256 = 'A' * 64
        recordedUtc = $script:AssemblyRecordedUtc
        rawLogs = @()
        provenance = [ordered]@{}
        details = [ordered]@{}
    }
    $planPath = Join-Path $sourceRootFixture 'validation-plan-receipt.json'
    $planDocument = [ordered]@{}
    foreach ($field in $fixtureReceiptCommon.Keys) {
        $planDocument[$field] = $fixtureReceiptCommon[$field]
    }
    $planDocument.role = 'validation-plan'
    $planDocument.producer = 'installed-runtime-validation-plan-v2'
    $planDocument.runNonce = '33333333-3333-4333-8333-333333333333'
    [IO.File]::WriteAllText($planPath, ($planDocument | ConvertTo-Json -Depth 16),
        $utf8NoBom)
    $resultsPath = Join-Path $sourceRootFixture 'validation-results-receipt.json'
    $resultsDocument = [ordered]@{}
    foreach ($field in $fixtureReceiptCommon.Keys) {
        $resultsDocument[$field] = $fixtureReceiptCommon[$field]
    }
    $resultsDocument.role = 'validation-results'
    $resultsDocument.producer = 'installed-runtime-validation-results-v2'
    $resultsDocument.runNonce = '44444444-4444-4444-8444-444444444444'
    [IO.File]::WriteAllText($resultsPath,
        ($resultsDocument | ConvertTo-Json -Depth 16), $utf8NoBom)

    $missingPerformanceReceipt = Join-Path $sourceRootFixture `
        'performance-report-receipt.json'
    Assert-Test (-not (Test-Path -LiteralPath $missingPerformanceReceipt)) `
        'No-performance fixture unexpectedly contains a host-runner performance receipt.'
    foreach ($boundReceipt in @(
            [pscustomobject]@{ path = $planPath; role = 'validation-plan' },
            [pscustomobject]@{ path = $resultsPath; role = 'validation-results' })) {
        $document = ConvertFrom-Stage5JsonDictionary $boundReceipt.path
        Assert-Test ($document.role -ceq $boundReceipt.role -and
            $document.sourceCommit -ceq $ExpectedSourceCommit -and
            $document.title -ceq 'ZeroHour' -and $document.architecture -ceq 'x64' -and
            $document.artifactSetSha256 -ceq $script:ArtifactSetSha256 -and
            $document.cohortNonce -ceq $ExpectedCohortNonce -and
            $document.runtimeClosure.closureSha256 -ceq
                $script:RuntimeClosure.closureSha256) `
            "No-performance '$($boundReceipt.role)' fixture is not source/artifact/cohort/runtime bound."
    }
    $runtimeEnvelope = New-Stage5DeterministicRuntimeEnvelope `
        -ValidationPlanReceiptPath $planPath `
        -ValidationResultsReceiptPath $resultsPath `
        -InstalledKernelAttachmentPath '' `
        -RequiredWorkers @('serial-1', 'parallel-1', 'parallel-2', 'parallel-4',
            'parallel-8', 'parallel-16', 'parallel-auto') `
        -ReplayEvidenceSha256 ('E' * 64) `
        -FreshAiEvidenceSha256 ('F' * 64) `
        -PerformanceScalingEvidenceSha256 ('9' * 64)
    $runtimeDocument = ConvertFrom-Stage5JsonDictionary $runtimeEnvelope.path
    Assert-Test ($runtimeDocument.evidenceKind -ceq 'deterministic-runtime' -and
        $runtimeDocument.sourceCommit -ceq $ExpectedSourceCommit -and
        $runtimeDocument.artifactSetSha256 -ceq $script:ArtifactSetSha256 -and
        $runtimeDocument.cohortNonce -ceq $ExpectedCohortNonce -and
        $runtimeDocument.runtimeClosure.closureSha256 -ceq
            $script:RuntimeClosure.closureSha256) `
        'Runtime envelope lost source, artifact, cohort, or closure binding.'
    Assert-Test (@($runtimeDocument.attachments).Count -eq 2 -and
        (@($runtimeDocument.attachments | ForEach-Object { $_.role }) -join '|') -ceq
            'validation-plan|validation-results') `
        'No-performance runtime envelope must bind exactly the plan and result receipts.'
    foreach ($expectedFixture in @(
            [pscustomobject]@{ role = 'validation-plan'; path = $planPath },
            [pscustomobject]@{ role = 'validation-results'; path = $resultsPath })) {
        $attachment = @($runtimeDocument.attachments | Where-Object {
            $_.role -ceq $expectedFixture.role -and $_.title -ceq 'ZeroHour'
        })
        $relative = Get-ContainedRelativePath $readinessRoot $expectedFixture.path `
            "No-performance fixture '$($expectedFixture.role)'"
        Assert-Test ($attachment.Count -eq 1 -and
            $attachment[0].path -ceq $relative -and
            $attachment[0].sha256 -ceq
                (Get-Stage5FileSha256 $expectedFixture.path) -and
            $attachment[0].trustDomain -ceq 'host-runner') `
            "Runtime envelope '$($expectedFixture.role)' binding is detached from its fixture bytes."
    }
    Assert-Test (-not [bool]$runtimeDocument.details.finalAcceptanceClaim -and
        $runtimeDocument.details.replayEvidenceSha256 -ceq ('E' * 64) -and
        $runtimeDocument.details.freshAiEvidenceSha256 -ceq ('F' * 64) -and
        $runtimeDocument.details.performanceEvidenceSha256 -ceq ('9' * 64)) `
        'Runtime envelope overclaims final acceptance or lost its replay, AI, or separate scaling-envelope binding.'
    $evidenceModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    & $evidenceModule {
        param($details, $evidenceHashes)
        Assert-Stage5FinalAcceptanceDetails 'deterministic-runtime' $details `
            ('1' * 40) $evidenceHashes
    } $runtimeDocument.details @{
        'replay-determinism' = 'E' * 64
        'fresh-ai' = 'F' * 64
        'performance-scaling' = '9' * 64
    }
    Assert-Test (-not (Test-Path -LiteralPath $missingPerformanceReceipt)) `
        'Runtime envelope assembly created an unsupported performance receipt.'
}
finally {
    if (Test-Path -LiteralPath $fixtureRoot -PathType Container) {
        $fixtureFull = [IO.Path]::GetFullPath($fixtureRoot).TrimEnd([char[]]@(
            [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
        $scratchPrefix = $scratchFull.TrimEnd([char[]]@(
            [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)) +
            [IO.Path]::DirectorySeparatorChar
        Assert-Test ($fixtureFull.StartsWith($scratchPrefix,
            [StringComparison]::OrdinalIgnoreCase)) `
            'No-performance fixture cleanup target escaped its designated scratch root.'
        Remove-Item -LiteralPath $fixtureFull -Recurse -Force
    }
}

Write-Output 'Stage 5 development-readiness bundle assembler tests passed.'
