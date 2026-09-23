[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path $PSScriptRoot 'DeterministicSimulationValidation.Tests.ps1'
$sourceText = [IO.File]::ReadAllText($sourcePath)
$tokens = $null
$parseErrors = $null
$sourceAst = [Management.Automation.Language.Parser]::ParseInput(
    $sourceText, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count -ne 0) {
    throw "The validation entrypoint does not parse: $($parseErrors[0].Message)"
}

# The synthetic final-acceptance fixture deliberately records installed-kernel
# execution as externally exempt. Every positive or downstream-negative use of
# that fixture must therefore opt in, while one explicit omission remains to
# prove the production guard fails closed.
$aggregationCommands = @($sourceAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Invoke-Stage5FinalAcceptanceAggregation'
}, $true))
$unexemptedAggregationCommands = @($aggregationCommands | Where-Object {
    @($_.CommandElements | Where-Object {
        $_ -is [Management.Automation.Language.CommandParameterAst] -and
            $_.ParameterName -ceq 'ExternalQualificationExempt'
    }).Count -eq 0
})
if ($unexemptedAggregationCommands.Count -ne 1 -or
    $unexemptedAggregationCommands[0].Extent.Text -cnotmatch '-DevelopmentReadiness') {
    throw 'The skipped installed-kernel fixture must have exactly one explicit fail-closed omission case.'
}
if ($sourceText -cnotmatch
    'a skipped installed-kernel qualification fails closed without an explicit exemption') {
    throw 'The explicit installed-kernel exemption omission is missing its guard assertion.'
}
$closureResetCommands = @($sourceAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Reset-Stage5TestFinalAcceptanceValidatedClosure'
}, $true))
if ($closureResetCommands.Count -ne 2) {
    throw 'Independent immutable-reader mutation suites must reset abandoned aggregation collectors.'
}
if ($sourceText -cnotmatch
    '(?s)Invoke-Stage5FinalAcceptance\.ps1.{0,300}-ExternalQualificationExempt') {
    throw 'The final-acceptance script test must authorize its synthetic installed-kernel exemption.'
}
$catalogFunction = $sourceAst.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-CurrentNativeReceiptCatalog'
}, $true)
if ($null -eq $catalogFunction) {
    throw 'The current native receipt catalog regression helper is missing.'
}
$catalogSource = $catalogFunction.Extent.Text
foreach ($binding in @('ExpectedCohortNonce', 'ExpectedCohortCreatedUtc',
        'ExpectedRuntimeClosure')) {
    $pattern = '(?s)\$readArguments\s*=\s*@\{.*?' +
        [regex]::Escape($binding) + '\s*='
    if ($catalogSource -cnotmatch $pattern) {
        throw "The current native receipt reader test omits $binding."
    }
}
foreach ($binding in @('ExpectedArguments', 'ExpectedCohortCreatedUtc')) {
    $pattern = '(?s)\$parserArguments\s*=\s*@\{.*?' +
        [regex]::Escape($binding) + '\s*='
    if ($catalogSource -cnotmatch $pattern) {
        throw "The native receipt parser test omits $binding."
    }
}
foreach ($pattern in @(
    '\$syntheticZeroHourReceipts\s*=\s*\[ordered\]@\{',
    '\$syntheticZeroHourReceipts\.Contains\(\$role\)',
    'reusedValidationDocument\.provenance\.children\)\.Count\s*-eq\s*253',
    'reusedValidationDocument\.rawLogs\)\.Count\s*-eq\s*507',
    'reusedValidationDocument\.details\.resultCount\s*-eq\s*253'
)) {
    if ($sourceText -cnotmatch $pattern) {
        throw 'The complete synthetic ZeroHour authority reuse contract is missing.'
    }
}
$installedKernelGuardBoundary = $sourceText.IndexOf(
    'a skipped installed-kernel qualification fails closed without an explicit exemption',
    [StringComparison]::Ordinal)
$outOfBandBoundary = $sourceText.IndexOf('$outOfBandAcceptanceRequest =',
    [StringComparison]::Ordinal)
$redundantDiagnosticAggregations = @($aggregationCommands | Where-Object {
    $_.Extent.StartOffset -gt $installedKernelGuardBoundary -and
        $_.Extent.StartOffset -lt $outOfBandBoundary
})
if ($installedKernelGuardBoundary -lt 0 -or $outOfBandBoundary -lt 0 -or
    $redundantDiagnosticAggregations.Count -ne 0) {
    throw 'The diagnostic NET3 regression must not invoke redundant full final aggregation.'
}
if ($sourceText -cnotmatch
    '(?s)Read-Stage5LockstepV2Evidence.{0,1000}canonical diagnostic NET3 v1 fixture') {
    throw 'The diagnostic NET3 regression must exercise the strict lockstep-v2 reader directly.'
}
# Execute the actual test entrypoint's early routing preflight, not a copied
# selector implementation. It returns before module imports or fixture I/O.
$entrypoint = [scriptblock]::Create($sourceText)
$expected = @{
    All = 'Plan=True; Runtime=True; Acceptance=True'
    Plan = 'Plan=True; Runtime=False; Acceptance=False'
    Runtime = 'Plan=False; Runtime=True; Acceptance=False'
    Acceptance = 'Plan=False; Runtime=False; Acceptance=True'
}
foreach ($partition in @('All', 'Plan', 'Runtime', 'Acceptance')) {
    foreach ($explicitEmpty in @($false, $true)) {
        $arguments = @{ ValidationPartition = $partition; PartitionSelectionPreflightOnly = $true }
        if ($explicitEmpty) { $arguments.FocusedAcceptanceCase = '' }
        $output = @(& $entrypoint @arguments)
        if ($output.Count -ne 1 -or
            $output[0] -cne "Stage 5 partition selection passed: $partition; $($expected[$partition])") {
            throw "Wrong routing for $partition (explicit empty selection: $explicitEmpty)."
        }
    }
}
foreach ($focusedCase in @('QualificationDataReceipt',
        'DevelopmentReadinessExecutionEvidence', 'AiDeterminismGrouping',
        'LivePlanEntryIdentity', 'ResultTreeDictionary',
        'FinalAcceptanceOutputPublication')) {
    foreach ($partition in @('All', 'Acceptance')) {
        $output = @(& $entrypoint -ValidationPartition $partition `
            -FocusedAcceptanceCase $focusedCase -PartitionSelectionPreflightOnly)
        if ($output.Count -ne 1 -or $output[0] -cne
            "Stage 5 partition selection passed: $partition; Plan=False; Runtime=False; Acceptance=True") {
            throw "Wrong focused routing for $partition / $focusedCase."
        }
    }
    foreach ($partition in @('Plan', 'Runtime')) {
        $caught = $null
        try { & $entrypoint -ValidationPartition $partition `
            -FocusedAcceptanceCase $focusedCase -PartitionSelectionPreflightOnly | Out-Null }
        catch { $caught = $_.Exception.Message }
        if ($null -eq $caught -or $caught -notlike 'Focused acceptance cases require*') {
            throw "Unsupported focused routing was not rejected: $partition / $focusedCase."
        }
    }
}
$caught = $null
try { & $entrypoint -FocusedAcceptanceCase 'unknown-case' -PartitionSelectionPreflightOnly | Out-Null }
catch { $caught = $_.Exception.Message }
if ($null -eq $caught -or $caught -notlike 'Unknown focused acceptance case*') {
    throw 'Unknown focused selection was not rejected.'
}
# Demonstrate the historical case-insensitive typed-parameter collision is
# rejected by the entrypoint invariant rather than silently running Acceptance.
$historicalText = $sourceText.Replace('$hasFocusedAcceptanceCase', '$focusedAcceptanceCase')
$historicalEntrypoint = [scriptblock]::Create($historicalText)
$caught = $null
try { & $historicalEntrypoint -ValidationPartition Plan -PartitionSelectionPreflightOnly | Out-Null }
catch { $caught = $_.Exception.Message }
if ($caught -cne 'Stage 5 test partition selection does not match its requested scope.') {
    throw 'Historical typed-string selector collision was not detected.'
}
Write-Output 'Stage 5 validation partition routing regression passed.'
