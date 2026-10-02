[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Ast {
    param([string]$Path)
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$errors)
    Assert-Test ($errors.Count -eq 0) "'$Path' must parse: $($errors -join '; ')"
    return $ast
}

function Get-EmbeddedWorkflowAst {
    param([string]$Content, [string]$StepName)
    $stepMatch = [regex]::Match($Content,
        '(?ms)^      - name: ' + [regex]::Escape($StepName) +
        '\r?\n(?<Step>.*?)(?=^      - name: |\z)')
    Assert-Test $stepMatch.Success "Workflow step '$StepName' is missing."
    $step = $stepMatch.Groups['Step'].Value
    $lines = @($step -split '\r?\n')
    $runLineIndex = -1
    $runIndent = -1
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -match '^(?<Indent>\s*)run:\s*\|\s*$') {
            $runLineIndex = $index
            $runIndent = $Matches.Indent.Length
            break
        }
    }
    Assert-Test ($runLineIndex -ge 0) `
        "Workflow step '$StepName' is missing a literal run block."
    $bodyIndent = $runIndent + 2
    $bodyLines = New-Object 'Collections.Generic.List[string]'
    for ($index = $runLineIndex + 1; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) {
            $bodyLines.Add('') | Out-Null
            continue
        }
        $lineIndent = $line.Length - $line.TrimStart().Length
        Assert-Test ($lineIndent -ge $bodyIndent) `
            "Workflow step '$StepName' contains a run-body line outside its YAML literal."
        $bodyLines.Add($line.Substring($bodyIndent)) | Out-Null
    }
    $body = ($bodyLines.ToArray() -join "`n")
    $parseBody = [regex]::Replace($body, '(?s)\$\{\{.*?\}\}',
        '__STAGE5_GITHUB_EXPRESSION__')
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $parseBody, [ref]$tokens, [ref]$errors)
    Assert-Test ($errors.Count -eq 0) `
        "Workflow step '$StepName' run block must parse: $($errors -join '; ')"
    return [pscustomobject]@{ ast = $ast; body = $body }
}

function Test-Stage5SchemaVersionReference {
    param([Management.Automation.Language.Ast]$Node)

    # Do not classify a comparison from a fragile source-text shape.  The
    # evidence gates use direct properties, indexed properties, named local
    # variables (for example $attestationSchemaVersion), and inline
    # Get-Stage5JsonValue calls.  Their parsed ASTs all retain one of these
    # exact schemaVersion tokens, including chained members such as
    # $adapter.document.schemaVersion.
    $matches = @($Node.FindAll({
        param($candidate)
        if ($candidate -is [Management.Automation.Language.VariableExpressionAst]) {
            return $candidate.VariablePath.UserPath -match '(?i)schemaVersion$'
        }
        if ($candidate -is [Management.Automation.Language.MemberExpressionAst]) {
            return [string]$candidate.Member.Extent.Text -ieq 'schemaVersion'
        }
        if ($candidate -is [Management.Automation.Language.StringConstantExpressionAst]) {
            return [string]$candidate.Value -ieq 'schemaVersion'
        }
        return $false
    }, $true))
    return $matches.Count -gt 0
}

function Get-ContainingCondition {
    param(
        [object[]]$Conditions,
        [object[]]$Assignments,
        [Management.Automation.Language.Ast]$Node
    )
    $candidates = @($Conditions | Where-Object {
            $_.Extent.StartOffset -le $Node.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $Node.Extent.EndOffset
    })
    if ($candidates.Count -gt 0) {
        return $candidates | Sort-Object {
            $_.Extent.EndOffset - $_.Extent.StartOffset
        } | Select-Object -First 1
    }
    $assignmentCandidates = @($Assignments | Where-Object {
            $_.Extent.StartOffset -le $Node.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $Node.Extent.EndOffset
    })
    if ($assignmentCandidates.Count -gt 0) {
        return $assignmentCandidates | Sort-Object {
            $_.Extent.EndOffset - $_.Extent.StartOffset
        } | Select-Object -First 1
    }
    return $null
}

function Get-ContainingFunctionName {
    param(
        [object[]]$Functions,
        [Management.Automation.Language.Ast]$Node
    )
    $matches = @($Functions | Where-Object {
            $_.Extent.StartOffset -le $Node.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $Node.Extent.EndOffset
    })
    if ($matches.Count -eq 0) { return '<script>' }
    return [string](($matches | Sort-Object {
        $_.Extent.EndOffset - $_.Extent.StartOffset
    } | Select-Object -First 1).Name)
}

function Get-FunctionAst {
    param(
        [Management.Automation.Language.Ast]$Ast,
        [string]$Name,
        [string]$Path
    )
    $matches = @($Ast.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $candidate.Name -ceq $Name
    }, $true))
    Assert-Test ($matches.Count -eq 1) `
        "Expected one function '$Name' in '$Path', found $($matches.Count)."
    return $matches[0]
}

function Assert-WeeklyPromotionAllowNonCanonicalRootFence {
    param(
        [string]$Content,
        [string]$Context
    )
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $Content, [ref]$tokens, [ref]$errors)
    Assert-Test (@($errors).Count -eq 0) `
        "$Context validator source must parse: $($errors -join '; ')"
    $functions = @($ast.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.FunctionDefinitionAst]
    }, $true))
    $promotionFunction = @($functions | Where-Object {
        $_.Name -ceq 'Invoke-PromotionValidation'
    })
    Assert-Test ($promotionFunction.Count -eq 1) `
        "$Context must contain one Invoke-PromotionValidation function."
    $allowParameter = @($promotionFunction[0].Body.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -ceq 'AllowNonCanonicalSelfTestRoot'
    })
    Assert-Test ($allowParameter.Count -eq 1) `
        "$Context must keep the noncanonical-root switch private to validation."

    $calls = @($ast.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.CommandAst] -and
            $candidate.GetCommandName() -ceq 'Invoke-PromotionValidation'
    }, $true))
    Assert-Test ($calls.Count -ge 2) `
        "$Context must inventory both normal validation and self-test calls."
    $normalCalls = @($calls | Where-Object {
        $call = $_
        $owners = @($functions | Where-Object {
            $_.Extent.StartOffset -le $call.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $call.Extent.EndOffset
        } | Sort-Object {
            $_.Extent.EndOffset - $_.Extent.StartOffset
        })
        $owners.Count -eq 0
    })
    Assert-Test ($normalCalls.Count -eq 1) `
        "$Context must contain exactly one top-level normal validation call."
    Assert-Test ($normalCalls[0].Extent.Text -notmatch
            '(?i)-AllowNonCanonicalSelfTestRoot(?:\s|$)') `
        "$Context normal validation must not pass -AllowNonCanonicalSelfTestRoot."

    $selfTestCalls = @($calls | Where-Object {
        $call = $_
        $owners = @($functions | Where-Object {
            $_.Extent.StartOffset -le $call.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $call.Extent.EndOffset
        } | Sort-Object {
            $_.Extent.EndOffset - $_.Extent.StartOffset
        })
        $owners.Count -gt 0 -and $owners[0].Name -ceq 'Invoke-PromotionSelfTest'
    })
    Assert-Test ($selfTestCalls.Count -gt 0) `
        "$Context must retain a bounded self-test validation call."
    foreach ($call in $selfTestCalls) {
        Assert-Test ($call.Extent.Text -match
                '(?i)-AllowNonCanonicalSelfTestRoot(?:\s|$)') `
            "$Context self-test validation must explicitly use the noncanonical-root switch."
    }
    $unexpectedSwitchCalls = @($calls | Where-Object {
        $call = $_
        $owners = @($functions | Where-Object {
            $_.Extent.StartOffset -le $call.Extent.StartOffset -and
            $_.Extent.EndOffset -ge $call.Extent.EndOffset
        } | Sort-Object {
            $_.Extent.EndOffset - $_.Extent.StartOffset
        })
        $ownerName = if ($owners.Count -eq 0) { '<script>' } else {
            [string]$owners[0].Name
        }
        $ownerName -cne 'Invoke-PromotionSelfTest' -and
            $call.Extent.Text -match
            '(?i)-AllowNonCanonicalSelfTestRoot(?:\s|$)'
    })
    Assert-Test ($unexpectedSwitchCalls.Count -eq 0) `
        "$Context found a noncanonical-root switch outside SelfTest."
}

function Test-Stage5PromotionNumericReference {
    param(
        [Management.Automation.Language.Ast]$Node,
        [string]$Field
    )
    return $Node.Extent.Text -match
        ("(?i)(?:\['" + [regex]::Escape($Field) + "'\]|\." +
            [regex]::Escape($Field) + '\b)')
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$modulePath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1'
$assemblerPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\New-Stage5DevelopmentReadinessBundle.ps1'
$net3Path = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Invoke-InstalledNet3LoopbackValidation.ps1'
$lockstepPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Invoke-InstalledLockstepV2Validation.ps1'
$lockstepSessionPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Stage5InstalledLockstepV2Session.psm1'
$scalingPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Invoke-Stage5PerformanceScalingValidation.ps1'
$performanceTestPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Stage5PerformanceScalingValidation.Tests.ps1'
$weeklyPromotionPath = Join-Path $root `
    '.github\workflows\Validate-Stage5WeeklyPromotionAttestation.ps1'
$readinessSealerPath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Seal-Stage5DevelopmentReadinessBundle.ps1'
$ciPath = Join-Path $root '.github\workflows\ci.yml'
$checkReplaysPath = Join-Path $root '.github\workflows\check-replays.yml'
Assert-Test (Test-Path -LiteralPath $modulePath -PathType Leaf) `
    'Deterministic simulation evidence module is missing.'
Assert-Test (Test-Path -LiteralPath $assemblerPath -PathType Leaf) `
    'Stage 5 development-readiness assembler is missing.'
Assert-Test (Test-Path -LiteralPath $net3Path -PathType Leaf) `
    'Installed NET3 loopback validator is missing.'
Assert-Test (Test-Path -LiteralPath $lockstepPath -PathType Leaf) `
    'Installed lockstep-v2 validator is missing.'
Assert-Test (Test-Path -LiteralPath $lockstepSessionPath -PathType Leaf) `
    'Installed lockstep-v2 shared session module is missing.'
Assert-Test (Test-Path -LiteralPath $scalingPath -PathType Leaf) `
    'Stage 5 performance scaling validator is missing.'
Assert-Test (Test-Path -LiteralPath $performanceTestPath -PathType Leaf) `
    'Stage 5 performance focused test is missing.'
Assert-Test (Test-Path -LiteralPath $weeklyPromotionPath -PathType Leaf) `
    'Weekly promotion attestation validator is missing.'
Assert-Test (Test-Path -LiteralPath $readinessSealerPath -PathType Leaf) `
    'Development-readiness bundle sealer is missing.'
Assert-Test (Test-Path -LiteralPath $ciPath -PathType Leaf) `
    'CI workflow is missing.'
Assert-Test (Test-Path -LiteralPath $checkReplaysPath -PathType Leaf) `
    'Replay-check workflow is missing.'

Import-Module $modulePath -Force
$weeklyPromotionAst = Get-Ast $weeklyPromotionPath
$weeklyPromotionContent = Get-Content -LiteralPath $weeklyPromotionPath -Raw
Assert-WeeklyPromotionAllowNonCanonicalRootFence $weeklyPromotionContent `
    'weekly promotion noncanonical-root fence'
$mutatedWeeklyPromotionContent = [regex]::Replace(
    $weeklyPromotionContent,
    '(?m)(Invoke-PromotionValidation\s+\$BundleRoot\s+\$ExpectedSourceCommit\s+`\r?\n\s+\$ExpectedAttestationSha256\s+\$ExpectedQualificationRunId)',
    '$1 -AllowNonCanonicalSelfTestRoot')
Assert-Test ($mutatedWeeklyPromotionContent -cne $weeklyPromotionContent) `
    'weekly promotion noncanonical-root mutation did not change the normal call.'
$caught = $false
try {
    Assert-WeeklyPromotionAllowNonCanonicalRootFence `
        $mutatedWeeklyPromotionContent `
        'mutated weekly promotion noncanonical-root fence'
}
catch { $caught = $true }
Assert-Test $caught `
    'weekly promotion contract accepted a normal Validate call using -AllowNonCanonicalSelfTestRoot.'
$ciContent = Get-Content -LiteralPath $ciPath -Raw
$ciEmbedded = Get-EmbeddedWorkflowAst $ciContent `
    'Run Stage 5 final pre-manual acceptance'
$checkReplaysContent = Get-Content -LiteralPath $checkReplaysPath -Raw
$matrixEmbedded = Get-EmbeddedWorkflowAst $checkReplaysContent `
    'Run Stage 5 Installed-Runtime Replay Matrix'

# Windows PowerShell 5.1 parses generic method type arguments as an array
# expression.  Keep the bounded-output byte-equality regression on the
# overload-inferred form and parse the focused test under the current host so
# this compatibility boundary stays durable.
$performanceTestContent = Get-Content -LiteralPath $performanceTestPath -Raw
Get-Ast $performanceTestPath | Out-Null
Assert-Test ($performanceTestContent -match
        '\[Linq\.Enumerable\]::SequenceEqual\(\$captured,\s*\$exactBytes\)' -and
        $performanceTestContent -notmatch
        '\[Linq\.Enumerable\]::SequenceEqual\[[^]]+\]') `
    'Bounded-output byte equality must use a Windows PowerShell-compatible non-generic invocation.'

# The helper itself is the runtime contract: only integral CLR values that can
# have come from a JSON integer are accepted.  These cases catch the coercions
# that made the old raw -eq gates unsafe.
$validInteger = [int64]1
Assert-Test (Test-Stage5JsonInteger $validInteger) `
    'A JSON integer must be recognized as an integer.'
foreach ($invalid in @(
        [string]'1',
        [object[]]@(1),
        [double]1.0,
        [double]1.5,
        [decimal]1,
        [bool]$true
    )) {
    Assert-Test (-not (Test-Stage5JsonInteger $invalid)) `
        "Non-integer schemaVersion type '$($invalid.GetType().FullName)' was accepted."
}

# The weekly promotion validator also consumes numeric evidence fields.  Each
# field is an external JSON scalar and must be proven integral before an exact
# comparison or a cast can coerce it.  Keep these families explicit so a new
# gate cannot silently regress to PowerShell's string/fraction/bool coercions.
$weeklyPromotionNumericFamilies = @(
    [pscustomobject]@{
        Name = 'weekly lockstep evidence envelope'
        Function = 'Assert-PromotionEvidence'
        Fields = @('commonStopFrame', 'peerCount', 'networkRosterMask',
            'simulationRosterMask', 'aiRosterMask', 'aiPlayerCount')
    },
    [pscustomobject]@{
        Name = 'weekly title session'
        Function = 'Assert-PromotionEvidence'
        Fields = @('peerCount', 'networkRosterMask', 'simulationRosterMask',
            'aiRosterMask', 'aiPlayerCount')
    },
    [pscustomobject]@{
        Name = 'weekly lockstep peer envelope'
        Function = 'Assert-PromotionEvidence'
        Fields = @('peerCount', 'networkRosterMask', 'simulationRosterMask',
            'aiRosterMask', 'aiPlayerCount', 'exitCode', 'finalFrame', 'finalCRC')
    },
    [pscustomobject]@{
        Name = 'weekly negative probe'
        Function = 'Assert-PromotionEvidence'
        Fields = @('exitCode')
    },
    [pscustomobject]@{
        Name = 'weekly promotion attestation details'
        Function = 'Invoke-PromotionValidation'
        Fields = @('networkRosterMask', 'simulationRosterMask',
            'aiRosterMask', 'aiPlayerCount', 'sessionCount', 'peerCount',
            'commonStopFrame')
    },
    [pscustomobject]@{
        Name = 'weekly validated evidence'
        Function = 'Invoke-PromotionValidation'
        Fields = @('commonStopFrame', 'peerCount')
    }
)
$weeklyPromotionConditions = @($weeklyPromotionAst.FindAll({
    param($candidate)
    ($candidate -is [Management.Automation.Language.CommandAst] -and
        $candidate.GetCommandName() -ceq 'Assert-PromotionCondition') -or
    $candidate -is [Management.Automation.Language.IfStatementAst]
}, $true))
$weeklyPromotionAssignments = @($weeklyPromotionAst.FindAll({
    param($candidate)
    $candidate -is [Management.Automation.Language.AssignmentStatementAst]
}, $true))
$weeklyPromotionFunctions = @($weeklyPromotionAst.FindAll({
    param($candidate)
    $candidate -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true))
$weeklyInvalidScalarCases = @(
    [pscustomobject]@{ Name = 'string'; Value = [string]'1' },
    [pscustomobject]@{ Name = 'array'; Value = [object[]]@(1) },
    [pscustomobject]@{ Name = 'double'; Value = [double]1.0 },
    [pscustomobject]@{ Name = 'fraction'; Value = [double]1.5 },
    [pscustomobject]@{ Name = 'decimal'; Value = [decimal]1 },
    [pscustomobject]@{ Name = 'boolean'; Value = [bool]$true }
)
foreach ($family in $weeklyPromotionNumericFamilies) {
    $functionAst = Get-FunctionAst $weeklyPromotionAst $family.Function $weeklyPromotionPath
    foreach ($field in $family.Fields) {
        $fieldNodes = @($functionAst.FindAll({
            param($candidate)
            $candidate -is [Management.Automation.Language.BinaryExpressionAst] -and
                $candidate.Operator -in @('Ieq', 'Ine', 'Igt', 'Ige', 'Ilt', 'Ile') -and
                (Test-Stage5PromotionNumericReference $candidate $field)
        }, $true))
        Assert-Test ($fieldNodes.Count -gt 0) `
            "$($family.Name) field '$field' has no exact numeric comparison in $($family.Function)."
        foreach ($node in $fieldNodes) {
            $condition = Get-ContainingCondition $weeklyPromotionConditions `
                $weeklyPromotionAssignments $node
            Assert-Test ($null -ne $condition) `
                "$($family.Name) field '$field' comparison at line $($node.Extent.StartLineNumber) has no bounded condition."
            $conditionText = $condition.Extent.Text
            $guardPattern = '(?is)Test-PromotionJsonInteger\s+[^\r\n]*' +
                ("(?:\['" + [regex]::Escape($field) + "'\]|\." +
                [regex]::Escape($field) + '\b)')
            $guardMatch = [regex]::Match($conditionText, $guardPattern)
            $comparisonIndex = $conditionText.IndexOf(
                $node.Extent.Text, [StringComparison]::Ordinal)
            Assert-Test ($guardMatch.Success -and $comparisonIndex -gt $guardMatch.Index) `
                "$($family.Name) field '$field' comparison at line $($node.Extent.StartLineNumber) lacks a pre-comparison Test-PromotionJsonInteger guard."
            Assert-Test ($node.Left.Extent.Text -notmatch '^\s*\[[^]]+\]' -and
                $node.Right.Extent.Text -notmatch '^\s*\[[^]]+\]') `
                "$($family.Name) field '$field' casts before its exact comparison at line $($node.Extent.StartLineNumber)."
        }
        foreach ($invalid in $weeklyInvalidScalarCases) {
            Assert-Test (-not (Test-Stage5JsonInteger $invalid.Value)) `
                "$($family.Name) field '$field' accepted $($invalid.Name) as an integer scalar."
        }
    }
}

# Every exact schemaVersion equality is a release-gate identity check.  Keep
# this inventory derived from the parsed AST so a newly added contract cannot
# silently reintroduce a coercive comparison.  The named families below make
# the intended coverage explicit for reviewers.
$asts = @(
    [pscustomobject]@{ path = $modulePath; ast = Get-Ast $modulePath },
    [pscustomobject]@{ path = $assemblerPath; ast = Get-Ast $assemblerPath },
    [pscustomobject]@{ path = $net3Path; ast = Get-Ast $net3Path },
    [pscustomobject]@{ path = $lockstepPath; ast = Get-Ast $lockstepPath },
    [pscustomobject]@{ path = $lockstepSessionPath; ast = Get-Ast $lockstepSessionPath },
    [pscustomobject]@{ path = $scalingPath; ast = Get-Ast $scalingPath },
    [pscustomobject]@{ path = $weeklyPromotionPath; ast = $weeklyPromotionAst },
    [pscustomobject]@{ path = $readinessSealerPath; ast = Get-Ast $readinessSealerPath },
    [pscustomobject]@{ path = $ciPath; ast = $ciEmbedded.ast },
    [pscustomobject]@{ path = $checkReplaysPath; ast = $matrixEmbedded.ast }
)
$familySpecs = @(
    [pscustomobject]@{ name = 'external attestation'; path = $modulePath; function = 'Read-Stage5FinalAcceptanceProtectedAttestation' },
    [pscustomobject]@{ name = 'reviewed fixture manifest'; path = $modulePath; function = 'Read-Stage5FinalAcceptanceImmutableReceipt' },
    [pscustomobject]@{ name = 'Stage 5 scaling topology receipt'; path = $modulePath; function = 'Read-Stage5PerformanceScalingTopologyReceipt' },
    [pscustomobject]@{ name = 'Stage 3 protected scaling baseline'; path = $modulePath; function = 'Read-Stage5PerformanceScalingStage3Baseline' },
    [pscustomobject]@{ name = 'Stage 5 performance qualification-data manifest'; path = $modulePath; function = 'Read-Stage5PerformanceQualificationDataEvidence' },
    [pscustomobject]@{ name = 'Stage 5 scaling raw-sample manifest'; path = $modulePath; function = 'Read-Stage5PerformanceScalingRawSamples' },
    [pscustomobject]@{ name = 'Stage 5 scaling evidence'; path = $modulePath; function = 'Read-Stage5PerformanceScalingEvidence' },
    [pscustomobject]@{ name = 'Stage 5 simulation qualification-data manifest'; path = $modulePath; function = 'Read-Stage5SimulationQualificationDataEvidence' },
    [pscustomobject]@{ name = 'Artifact set manifest'; path = $modulePath; function = 'Invoke-Stage5FinalAcceptanceAggregation' },
    [pscustomobject]@{ name = 'Evidence docs'; path = $modulePath; function = 'Invoke-Stage5FinalAcceptanceAggregation' },
    [pscustomobject]@{ name = 'NET3 artifact-set manifest'; path = $net3Path; function = 'Read-AndValidateArtifactSet' },
    [pscustomobject]@{ name = 'NET3 raw peer'; path = $net3Path; function = '<script>' },
    [pscustomobject]@{ name = 'lockstep qualification-data manifest'; path = $lockstepPath; function = 'Read-AndValidateQualificationData' },
    [pscustomobject]@{ name = 'lockstep artifact-set manifest'; path = $lockstepSessionPath; function = 'Read-AndValidateArtifactSet' },
    [pscustomobject]@{ name = 'lockstep adapter envelope'; path = $lockstepPath; function = 'Invoke-SelfTest' },
    [pscustomobject]@{ name = 'lockstep closure envelope'; path = $lockstepPath; function = 'Invoke-SelfTest' },
    [pscustomobject]@{ name = 'development-readiness assembler'; path = $assemblerPath; function = 'Assert-CurrentReceipt' },
    [pscustomobject]@{ name = 'scaling artifact-set manifest'; path = $scalingPath; function = 'Read-Stage5PerformanceArtifactSet' },
    [pscustomobject]@{ name = 'scaling qualification-data manifest'; path = $scalingPath; function = 'Read-Stage5PerformanceQualificationData' },
    [pscustomobject]@{ name = 'scaling fixture manifest'; path = $scalingPath; function = 'Read-Stage5ScalingFixtureManifest' },
    [pscustomobject]@{ name = 'scaling Stage 3 baseline'; path = $scalingPath; function = 'Read-Stage5ScalingBaseline' },
    [pscustomobject]@{ name = 'scaling raw diagnostic receipt'; path = $scalingPath; function = 'Assert-Stage5RawDiagnostic' },
    [pscustomobject]@{ name = 'scaling receipt metric contract'; path = $scalingPath; function = 'Assert-Stage5ReceiptMetricContract' },
    [pscustomobject]@{ name = 'scaling kernel reference'; path = $scalingPath; function = 'Assert-Stage5KernelReferenceContract' },
    [pscustomobject]@{ name = 'scaling receipt'; path = $scalingPath; function = 'Assert-Stage5Receipt' },
    [pscustomobject]@{ name = 'scaling phase trace'; path = $scalingPath; function = 'Assert-Stage5PhaseTraceContract' },
    [pscustomobject]@{ name = 'scaling phase cohort'; path = $scalingPath; function = 'Assert-Stage5PhaseCohort' },
    [pscustomobject]@{ name = 'scaling run-set document'; path = $scalingPath; function = 'Assert-Stage5PerformanceRunSet' },
    [pscustomobject]@{ name = 'scaling planned launch plan'; path = $scalingPath; function = 'Resolve-Stage5PlannedPerformanceLaunch' },
    [pscustomobject]@{ name = 'scaling planned native receipt'; path = $scalingPath; function = 'Resolve-Stage5PlannedPerformanceLaunch' },
    [pscustomobject]@{ name = 'scaling run-plan native receipt'; path = $scalingPath; function = 'Invoke-Stage5PerformanceRunPlan' },
    [pscustomobject]@{ name = 'scaling installed attempt start'; path = $scalingPath; function = 'Invoke-Stage5InstalledPerformanceRun' },
    [pscustomobject]@{ name = 'weekly qualification-data manifest'; path = $weeklyPromotionPath; function = 'Assert-PromotionQualificationDataManifest' },
    [pscustomobject]@{ name = 'weekly lockstep evidence envelope'; path = $weeklyPromotionPath; function = 'Assert-PromotionEvidence' },
    [pscustomobject]@{ name = 'weekly lockstep peer envelope'; path = $weeklyPromotionPath; function = 'Assert-PromotionEvidence' },
    [pscustomobject]@{ name = 'weekly evidence closure'; path = $weeklyPromotionPath; function = 'Assert-PromotionEvidenceTree' },
    [pscustomobject]@{ name = 'weekly artifact-set manifest'; path = $weeklyPromotionPath; function = 'Invoke-PromotionValidation' },
    [pscustomobject]@{ name = 'weekly promotion attestation'; path = $weeklyPromotionPath; function = 'Invoke-PromotionValidation' },
    [pscustomobject]@{ name = 'weekly validated evidence'; path = $weeklyPromotionPath; function = 'Invoke-PromotionValidation' },
    [pscustomobject]@{ name = 'development-readiness seal manifest'; path = $readinessSealerPath; function = '<script>' }
)
$namedFamilies = @($familySpecs | ForEach-Object { $_.name })
$familyHits = @{}
$comparisonCount = 0
foreach ($entry in $asts) {
    $conditions = @($entry.ast.FindAll({
        param($candidate)
        ($candidate -is [Management.Automation.Language.CommandAst] -and
            @('Assert-Stage5Condition', 'Assert-Condition',
                'Assert-Stage5PerformanceCondition', 'Assert-PromotionCondition') -ccontains
                $candidate.GetCommandName()) -or
        $candidate -is [Management.Automation.Language.IfStatementAst]
    }, $true))
    $assignments = @($entry.ast.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.AssignmentStatementAst]
    }, $true))
    $functions = @($entry.ast.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.FunctionDefinitionAst]
    }, $true))
    $nodes = @($entry.ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.BinaryExpressionAst] -and
            $node.Operator -in @('Ieq', 'Ine') -and
            (Test-Stage5SchemaVersionReference $node)
    }, $true))
    foreach ($node in $nodes) {
        ++$comparisonCount
        $condition = Get-ContainingCondition $conditions $assignments $node
        Assert-Test ($null -ne $condition) `
            "schemaVersion comparison at $($entry.path):$($node.Extent.StartLineNumber) has no bounded condition."
        Assert-Test ($condition.Extent.Text -match '(?i)(?:Test-Stage5JsonInteger|Test-PromotionJsonInteger|Get-Stage5UnsignedCounter)') `
            "schemaVersion comparison at $($entry.path):$($node.Extent.StartLineNumber) in $(Get-ContainingFunctionName $functions $node) lacks an exact scalar-integer guard before equality."
        $guardMatch = [regex]::Match($condition.Extent.Text,
            '(?i)(?:Test-Stage5JsonInteger|Test-PromotionJsonInteger|Get-Stage5UnsignedCounter)')
        $comparisonIndex = $condition.Extent.Text.IndexOf(
            $node.Extent.Text, [StringComparison]::Ordinal)
        Assert-Test ($guardMatch.Success -and $comparisonIndex -gt $guardMatch.Index) `
            "schemaVersion comparison at $($entry.path):$($node.Extent.StartLineNumber) evaluates equality before its scalar-integer guard."
        Assert-Test ($node.Left.Extent.Text -notmatch '^\s*\[[^]]+\]' -and
            $node.Right.Extent.Text -notmatch '^\s*\[[^]]+\]') `
            "schemaVersion comparison at $($entry.path):$($node.Extent.StartLineNumber) casts before equality."
        $functionName = Get-ContainingFunctionName $functions $node
        foreach ($family in $familySpecs) {
            if ($entry.path -ceq $family.path -and
                $functionName -ceq $family.function) {
                $familyHits[$family.name] = $true
            }
        }
    }
}
Assert-Test ($comparisonCount -gt 0) `
    'No schemaVersion equality checks were found in the release-gate sources.'
foreach ($family in $namedFamilies) {
    Assert-Test ($familyHits.ContainsKey($family)) `
        "Schema-version contract family '$family' is not covered by the focused inventory."
}

# The CI readiness check is a PowerShell literal embedded in YAML, so keep a
# focused contract for its single schemaVersion comparison as well.
$ciStepMatch = [regex]::Match($ciContent,
    '(?ms)^      - name: Run Stage 5 final pre-manual acceptance\r?\n(?<Step>.*?)(?=^      - name: |\z)')
Assert-Test $ciStepMatch.Success `
    'CI final pre-manual acceptance step is missing.'
$ciStep = $ciStepMatch.Groups['Step'].Value
Assert-Test ($ciStep -match
        '(?i)Import-Module\s+"\$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/DeterministicSimulationEvidence\.psm1"') `
    'CI final pre-manual acceptance must import the shared JSON integer predicate.'
Assert-Test ($ciStep -match
        '(?i)Test-Stage5JsonInteger\s+\$report\.schemaVersion') `
    'CI final pre-manual acceptance lacks a scalar-integer schemaVersion guard.'
Assert-Test ($ciStep -notmatch '(?i)\[[^]]+\]\$report\.schemaVersion') `
    'CI final pre-manual acceptance casts schemaVersion before comparison.'
Assert-Test ($ciStep -match
        '(?is)Test-Stage5JsonInteger\s+\$report\.schemaVersion.*?\$report\.schemaVersion\s+-ne\s+1') `
    'CI final pre-manual acceptance does not guard schemaVersion before exact equality.'

# The replay matrix has two external schemaVersion gates and four array-valued
# JSON fields.  Get-Stage5JsonValue deliberately returns arrays with unary-comma
# so an additional @() wrapper would create a nested array and break valid data.
$matrixBody = [string]$matrixEmbedded.body
$matrixSchemaSites = @(
    'acceptanceSchemaVersion',
    'replayEvidenceSchemaVersion'
)
foreach ($schemaSite in $matrixSchemaSites) {
    Assert-Test ($matrixBody -match
            ('(?is)Test-Stage5JsonInteger\s+\$' +
                [regex]::Escape($schemaSite) + '.*?\$' +
                [regex]::Escape($schemaSite) + '\s+-ne\s+1')) `
        "Replay matrix schemaVersion site '$schemaSite' lacks a pre-comparison integer guard."
}
$matrixArrayKeys = @('artifacts', 'evidence', 'attachments', 'fixtures')
foreach ($arrayKey in $matrixArrayKeys) {
    $keyPattern = "(?i)'" + [regex]::Escape($arrayKey) + "'"
    $calls = @($matrixEmbedded.ast.FindAll({
            param($node)
            $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -ceq 'Get-Stage5JsonValue' -and
                $node.Extent.Text -match $keyPattern
        }, $true))
    Assert-Test ($calls.Count -eq 1) `
        "Replay matrix must read '$arrayKey' exactly once through Get-Stage5JsonValue."
    $ancestor = $calls[0].Parent
    $nestedArray = $false
    while ($null -ne $ancestor) {
        if ($ancestor -is [Management.Automation.Language.ArrayExpressionAst]) {
            $nestedArray = $true
            break
        }
        $ancestor = $ancestor.Parent
    }
    Assert-Test (-not $nestedArray) `
        "Replay matrix '$arrayKey' input is wrapped in an outer array expression."
}
foreach ($cardinalityGuard in @(
        '$artifactHashes.Count -ne $requiredArtifactRoles.Count',
        '$evidenceEntries.Count -ne $requiredEvidenceKinds.Count',
        '$seenReplayAttachmentBindings.Count -ne',
        '$fixtures.Count -ne 10')) {
    Assert-Test ($matrixBody.Contains($cardinalityGuard)) `
        "Replay matrix is missing durable cardinality guard '$cardinalityGuard'."
}

Write-Output "Stage5SchemaVersionTypeContract: PASS ($comparisonCount guarded comparisons; invalid JSON scalar cases rejected)"
