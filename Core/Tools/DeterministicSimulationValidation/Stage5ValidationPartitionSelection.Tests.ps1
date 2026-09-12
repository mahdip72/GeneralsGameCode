[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path $PSScriptRoot 'DeterministicSimulationValidation.Tests.ps1'
$sourceText = [IO.File]::ReadAllText($sourcePath)
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
        'LivePlanEntryIdentity', 'ResultTreeDictionary')) {
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
