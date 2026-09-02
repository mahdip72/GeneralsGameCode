[CmdletBinding()]
param(
    [string]$SourceRoot = '',
    [switch]$SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Stage5WorkflowCondition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Stage5WorkflowContains {
    param([string]$Content, [string]$Pattern, [string]$Context)
    Assert-Stage5WorkflowCondition ($Content -match $Pattern) `
        "$Context is missing required contract '$Pattern'."
}

function Assert-Stage5WorkflowNotContains {
    param([string]$Content, [string]$Pattern, [string]$Context)
    Assert-Stage5WorkflowCondition ($Content -notmatch $Pattern) `
        "$Context contains forbidden contract '$Pattern'."
}

function Assert-Stage5WorkflowLiteral {
    param([string]$Content, [string]$Text, [string]$Context)
    Assert-Stage5WorkflowCondition $Content.Contains($Text) `
        "$Context is missing required text '$Text'."
}

function Get-Stage5WorkflowFile {
    param([string]$Root, [string]$RelativePath)
    $path = Join-Path $Root $RelativePath
    Assert-Stage5WorkflowCondition (Test-Path -LiteralPath $path -PathType Leaf) `
        "Required workflow file is missing: $RelativePath"
    return Get-Content -LiteralPath $path -Raw
}

function Get-Stage5IndentedBlock {
    param([string]$Content, [string]$Marker, [int]$Indent)

    $lines = @($Content -split '\r?\n')
    $prefix = ((' ' * $Indent) -join '') + $Marker
    $start = -1
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index].TrimEnd() -ceq $prefix) {
            $start = $index
            break
        }
    }
    Assert-Stage5WorkflowCondition ($start -ge 0) `
        "Could not locate indented workflow block '$Marker'."

    $blockLines = New-Object 'Collections.Generic.List[string]'
    [void]$blockLines.Add($lines[$start])
    for ($index = $start + 1; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) {
            [void]$blockLines.Add($line)
            continue
        }
        $lineIndent = $line.Length - $line.TrimStart().Length
        if ($lineIndent -le $Indent) {
            break
        }
        [void]$blockLines.Add($line)
    }
    return ($blockLines -join "`n")
}

if ($SelfTest) {
    $fixture = @"
stage5: true
workflow_dispatch
ValidationSet All
Invoke-Stage5FinalAcceptance.ps1
H:\Stage5SimulationValidationTask
-TaskRoot
-AllowHeadlessDirectExecution
"@
    Assert-Stage5WorkflowContains $fixture 'stage5:\s*true' 'self-test positive fixture'
    Assert-Stage5WorkflowContains $fixture 'ValidationSet\s+All' 'self-test positive fixture'
    Assert-Stage5WorkflowLiteral $fixture 'H:\Stage5SimulationValidationTask' `
        'self-test positive fixture'
    Assert-Stage5WorkflowLiteral $fixture '-TaskRoot' 'self-test positive fixture'
    Assert-Stage5WorkflowLiteral $fixture '-AllowHeadlessDirectExecution' `
        'self-test positive fixture'
    Assert-Stage5WorkflowLiteral $fixture 'workflow_dispatch' `
        'self-test positive fixture'
    Assert-Stage5WorkflowNotContains $fixture 'ReplayFixtureManifest\.example\.json' `
        'self-test positive fixture'
    $caught = $false
    try {
        Assert-Stage5WorkflowContains $fixture 'missing-contract' 'self-test negative fixture'
    }
    catch {
        $caught = $true
    }
    Assert-Stage5WorkflowCondition $caught 'workflow contract self-test did not reject a missing contract.'
    Write-Output 'Stage 5 workflow contract self-test passed.'
    return
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$root = [IO.Path]::GetFullPath($SourceRoot)
$ci = Get-Stage5WorkflowFile $root '.github/workflows/ci.yml'
$check = Get-Stage5WorkflowFile $root '.github/workflows/check-replays.yml'
$normalizer = Join-Path $root '.github/workflows/Normalize-Stage5EvidenceForUpload.ps1'
Assert-Stage5WorkflowCondition (Test-Path -LiteralPath $normalizer -PathType Leaf) `
    'Stage 5 evidence normalizer is missing.'
$normalizerContent = Get-Content -LiteralPath $normalizer -Raw

Assert-Stage5WorkflowContains $ci `
    'stage5:\s*\$\{\{\s*steps\.filter\.outputs\.stage5\s*\}\}' `
    'CI change detector output'
$stage5Filter = Get-Stage5IndentedBlock $ci 'stage5:' 12
Assert-Stage5WorkflowLiteral $stage5Filter ".github/workflows/**" 'CI Stage 5 path filter'
Assert-Stage5WorkflowLiteral $stage5Filter 'Core/**' 'CI Stage 5 path filter'
Assert-Stage5WorkflowLiteral $stage5Filter 'Generals/**' 'CI Stage 5 path filter'

$workflowContractJob = Get-Stage5IndentedBlock $ci 'stage5-workflow-contract:' 2
Assert-Stage5WorkflowLiteral $workflowContractJob 'needs: detect-changes' `
    'CI workflow contract job'
Assert-Stage5WorkflowContains $workflowContractJob `
    'if:\s+\$\{\{\s*github\.event_name\s*==\s*.workflow_dispatch.\s*\|\|\s*needs\.detect-changes\.outputs\.stage5\s*==\s*.true.\s*\}\}' `
    'CI workflow contract gate condition'
Assert-Stage5WorkflowContains $ci 'stage5_acceptance_manifest:' `
    'CI final acceptance input'
$generalsX64Build = Get-Stage5IndentedBlock $ci 'build-generals-x64:' 2
Assert-Stage5WorkflowContains $generalsX64Build 'outputs\.stage5\s*==\s*.true.' `
    'Generals x64 build Stage 5 prerequisite'
$generalsMdX64Build = Get-Stage5IndentedBlock $ci 'build-generalsmd-x64:' 2
Assert-Stage5WorkflowContains $generalsMdX64Build 'outputs\.stage5\s*==\s*.true.' `
    'GeneralsMD x64 build Stage 5 prerequisite'
$zeroHourStage5Job = Get-Stage5IndentedBlock $ci 'stage5-replaycheck-generalsmd-x64:' 2
Assert-Stage5WorkflowLiteral $zeroHourStage5Job 'needs: [detect-changes, build-generalsmd-x64]' `
    'Zero Hour Stage 5 build dependency'
Assert-Stage5WorkflowLiteral $zeroHourStage5Job "github.event_name == 'workflow_dispatch'" `
    'Zero Hour Stage 5 manual-dispatch gate'
foreach ($requiredInput in @(
        'inputs.stage5_fixture_manifest',
        'inputs.stage5_performance_baseline',
        'inputs.stage5_expected_stage3_executable_sha256',
        'inputs.stage5_acceptance_manifest')) {
    Assert-Stage5WorkflowLiteral $zeroHourStage5Job "$requiredInput != ''" `
        'Zero Hour Stage 5 evidence-input gate'
}
Assert-Stage5WorkflowNotContains $zeroHourStage5Job 'needs\.detect-changes\.outputs\.stage5' `
    'Zero Hour Stage 5 manual-only qualification job'
$generalsStage5Job = Get-Stage5IndentedBlock $ci 'stage5-replaycheck-generals-x64:' 2
Assert-Stage5WorkflowLiteral $generalsStage5Job 'needs: [detect-changes, build-generals-x64]' `
    'Generals Stage 5 build dependency'
Assert-Stage5WorkflowLiteral $generalsStage5Job "github.event_name == 'workflow_dispatch'" `
    'Generals Stage 5 manual-dispatch gate'
foreach ($requiredInput in @(
        'inputs.stage5_generals_fixture_manifest',
        'inputs.stage5_generals_performance_baseline',
        'inputs.stage5_generals_expected_stage3_executable_sha256',
        'inputs.stage5_acceptance_manifest')) {
    Assert-Stage5WorkflowLiteral $generalsStage5Job "$requiredInput != ''" `
        'Generals Stage 5 evidence-input gate'
}
Assert-Stage5WorkflowNotContains $generalsStage5Job 'needs\.detect-changes\.outputs\.stage5' `
    'Generals Stage 5 manual-only qualification job'
Assert-Stage5WorkflowContains $ci 'vc6-zerohour-oracle' `
    'legacy VC6 replay oracle'

$legacyBlock = Get-Stage5IndentedBlock $ci 'replaycheck-generalsmd:' 2
Assert-Stage5WorkflowCondition (-not [string]::IsNullOrWhiteSpace($legacyBlock)) `
    'Legacy VC6 replay job could not be isolated for the separation check.'
Assert-Stage5WorkflowNotContains $legacyBlock 'stage5:\s*true' `
    'Legacy VC6 replay job'

Assert-Stage5WorkflowContains $check 'stage5_acceptance_manifest:' `
    'reusable Stage 5 final acceptance input'
Assert-Stage5WorkflowContains $check `
    'Stage 5 requires the reviewed ten-replay corpus' `
    'reviewed ten-replay corpus preflight'
Assert-Stage5WorkflowContains $check `
    'example/sample manifests are not accepted' `
    'placeholder manifest rejection'
Assert-Stage5WorkflowContains $check `
    'Stage 5 final acceptance requires a reviewed manifest' `
    'placeholder final acceptance rejection'
Assert-Stage5WorkflowContains $check `
    "-ValidationSet',\s*'All'" `
    'full qualification ValidationSet'
Assert-Stage5WorkflowContains $check 'Invoke-Stage5FinalAcceptance\.ps1' `
    'full qualification final acceptance invocation'
Assert-Stage5WorkflowContains $check `
    'Stage 5 full qualification requires stage5_performance_baseline' `
    'full qualification performance inputs'
Assert-Stage5WorkflowContains $check `
    'Normalize-Stage5EvidenceForUpload\.ps1' `
    'evidence upload normalizer invocation'
Assert-Stage5WorkflowContains $check `
    'path:\s+\$\{\{\s*runner\.temp\s*\}\}\\Stage5SimulationValidation-upload' `
    'normalized evidence upload path'
Assert-Stage5WorkflowNotContains $check 'ReplayFixtureManifest\.example\.json' `
    'reusable Stage 5 workflow'

$allIndex = $check.IndexOf("-ValidationSet', 'All'", [StringComparison]::Ordinal)
$acceptanceIndex = $check.IndexOf('Invoke-Stage5FinalAcceptance.ps1', [StringComparison]::Ordinal)
Assert-Stage5WorkflowCondition ($allIndex -ge 0 -and $acceptanceIndex -gt $allIndex) `
    'Final acceptance must run after the complete ValidationSet All runner.'

$stage5Block = Get-Stage5IndentedBlock $check '- name: Run Stage 5 Installed-Runtime Replay Matrix' 6
Assert-Stage5WorkflowCondition (-not [string]::IsNullOrWhiteSpace($stage5Block)) `
    'Stage 5 runner block could not be isolated.'
Assert-Stage5WorkflowNotContains $stage5Block "-ValidationSet', 'Replay'" `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowNotContains $stage5Block '-AllowNonStandardCorpus' `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowNotContains $stage5Block '-DiagnosticNonAcceptance' `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowLiteral $stage5Block "`$taskRoot = 'H:\Stage5SimulationValidationTask'" `
    'Stage 5 H-resident task root'
Assert-Stage5WorkflowLiteral $stage5Block "'-TaskRoot', `$taskRoot" `
    'Stage 5 explicit task-root argument'
Assert-Stage5WorkflowLiteral $stage5Block "'-AllowHeadlessDirectExecution'" `
    'Stage 5 explicit hosted-runner execution exception'

$normalizerBlock = Get-Stage5IndentedBlock $check '- name: Normalize Stage 5 Evidence Paths for Upload' 6
Assert-Stage5WorkflowLiteral $normalizerBlock `
    '-InputRoot "H:\Stage5SimulationValidationTask\Evidence"' `
    'Stage 5 evidence normalizer H-resident input'
Assert-Stage5WorkflowContains $normalizerContent 'byte preserving' `
    'Stage 5 immutable evidence upload normalizer'
Assert-Stage5WorkflowNotContains $normalizerContent 'ConvertFrom-Json|WriteAllText' `
    'Stage 5 immutable evidence upload normalizer'

Write-Output 'Stage 5 workflow contract passed.'
