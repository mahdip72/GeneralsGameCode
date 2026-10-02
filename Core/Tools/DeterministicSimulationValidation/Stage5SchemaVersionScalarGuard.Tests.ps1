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
    Assert-Test (@($errors).Count -eq 0) "'$Path' must parse: $($errors -join '; ')"
    return $ast
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

function Get-ContainingCondition {
    param(
        [Management.Automation.Language.Ast]$FunctionAst,
        [Management.Automation.Language.Ast]$Node
    )
    $candidates = @($FunctionAst.FindAll({
        param($candidate)
        if ($candidate -is [Management.Automation.Language.IfStatementAst]) {
            return $true
        }
        if ($candidate -is [Management.Automation.Language.CommandAst]) {
            return @(
                'Assert-Stage5InstalledKernelCondition',
                'Assert-Stage5NativeFixtureCondition'
            ) -ccontains $candidate.GetCommandName()
        }
        if ($candidate -is [Management.Automation.Language.AssignmentStatementAst]) {
            return $true
        }
        return $false
    }, $true) | Where-Object {
        $_.Extent.StartOffset -le $Node.Extent.StartOffset -and
        $_.Extent.EndOffset -ge $Node.Extent.EndOffset
    })
    if ($candidates.Count -eq 0) { return $null }
    return $candidates | Sort-Object {
        $_.Extent.EndOffset - $_.Extent.StartOffset
    } | Select-Object -First 1
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$evidenceModulePath = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\DeterministicSimulationEvidence.psm1'
Assert-Test (Test-Path -LiteralPath $evidenceModulePath -PathType Leaf) `
    'Shared deterministic simulation evidence module is missing.'
Import-Module $evidenceModulePath -Force

# The shared helper is the runtime scalar contract.  A JSON integer may be any
# integral CLR type, but strings, arrays, floating-point values, and booleans
# must never reach an exact schemaVersion comparison.
Assert-Test (Test-Stage5JsonInteger ([int64]1)) `
    'An integral CLR scalar must be accepted as a JSON integer.'
foreach ($invalid in @(
        [string]'1',
        [object[]]@(1),
        [double]1.0,
        [double]1.5,
        [decimal]1,
        [bool]$true
    )) {
    Assert-Test (-not (Test-Stage5JsonInteger $invalid)) `
        "Non-integer JSON scalar '$($invalid.GetType().FullName)' was accepted."
}

$specs = @(
    [pscustomobject]@{
        Name = 'installed-kernel native receipt'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Stage5InstalledKernelExecutionEvidence.psm1'
        Function = 'ConvertTo-Stage5InstalledKernelRunProjection'
        Occurrence = 1
        Operator = 'Ieq'
        Expected = '6'
    },
    [pscustomobject]@{
        Name = 'installed-kernel host aggregate'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Stage5InstalledKernelExecutionEvidence.psm1'
        Function = 'Read-Stage5InstalledKernelSourceHost'
        Occurrence = 1
        Operator = 'Ieq'
        Expected = '2'
    },
    [pscustomobject]@{
        Name = 'reviewed native fixture manifest'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Stage5NativePerformanceFixtureProduction.psm1'
        Function = 'Read-Stage5ReviewedNativeKernelFixture'
        Occurrence = 1
        Operator = 'Ieq'
        Expected = '1'
    },
    [pscustomobject]@{
        Name = 'native fixture prelaunch plan'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Stage5NativePerformanceFixtureProduction.psm1'
        Function = 'Read-Stage5NativePerformanceFixtureProductionReceipt'
        Occurrence = 1
        Operator = 'Ieq'
        Expected = '1'
    },
    [pscustomobject]@{
        Name = 'native fixture attempt start'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Stage5NativePerformanceFixtureProduction.psm1'
        Function = 'Read-Stage5NativePerformanceFixtureProductionReceipt'
        Occurrence = 2
        Operator = 'Ieq'
        Expected = '1'
    },
    [pscustomobject]@{
        Name = 'runner native receipt candidate'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Run-DeterministicSimulationValidation.ps1'
        Function = 'Get-NativePerformanceReceiptReference'
        Occurrence = 1
        Operator = 'Ine'
        Expected = '5'
    },
    [pscustomobject]@{
        Name = 'runner native receipt binding'
        RelativePath = 'Core\Tools\DeterministicSimulationValidation\Run-DeterministicSimulationValidation.ps1'
        Function = 'Get-NativePerformanceReceiptReference'
        Occurrence = 2
        Operator = 'Ine'
        Expected = '5'
    }
)

$asts = @{}
foreach ($spec in $specs) {
    $path = Join-Path $root $spec.RelativePath
    if (-not $asts.ContainsKey($path)) {
        Assert-Test (Test-Path -LiteralPath $path -PathType Leaf) `
            "$($spec.Name) source is missing: $path"
        $asts[$path] = Get-Ast $path
    }
    $functionAst = Get-FunctionAst $asts[$path] $spec.Function $path
    $comparisons = @($functionAst.FindAll({
        param($candidate)
        $candidate -is [Management.Automation.Language.BinaryExpressionAst] -and
            $candidate.Operator -in @('Ieq', 'Ine') -and
            $candidate.Extent.Text -match '(?i)schemaVersion'
    }, $true) | Sort-Object Extent.StartOffset)
    Assert-Test ($comparisons.Count -ge $spec.Occurrence) `
        "$($spec.Name) is missing schemaVersion equality occurrence $($spec.Occurrence)."
    $comparison = $comparisons[$spec.Occurrence - 1]
    Assert-Test ($comparison.Operator.ToString() -ceq $spec.Operator) `
        "$($spec.Name) uses '$($comparison.Operator)' instead of '$($spec.Operator)'."
    Assert-Test ($comparison.Extent.Text -match "(?i)-$($spec.Operator.Substring(1).ToLowerInvariant())\s+$($spec.Expected)\s*$") `
        "$($spec.Name) does not compare schemaVersion to the expected value $($spec.Expected)."
    Assert-Test ($comparison.Left.Extent.Text -notmatch '^\s*\[[^]]+\]') `
        "$($spec.Name) casts schemaVersion before the exact comparison."
    Assert-Test ($comparison.Right.Extent.Text -notmatch '^\s*\[[^]]+\]') `
        "$($spec.Name) casts the comparison value."

    $condition = Get-ContainingCondition $functionAst $comparison
    Assert-Test ($null -ne $condition) `
        "$($spec.Name) schemaVersion comparison has no bounded condition."
    $conditionText = $condition.Extent.Text
    $guardMatch = [regex]::Match($conditionText,
        '(?i)Test-Stage5JsonInteger[^\r\n]*schemaVersion')
    $comparisonIndex = $conditionText.IndexOf(
        $comparison.Extent.Text, [StringComparison]::Ordinal)
    Assert-Test ($guardMatch.Success -and $guardMatch.Index -lt $comparisonIndex) `
        "$($spec.Name) must guard schemaVersion with Test-Stage5JsonInteger before equality."
}

Write-Output "Stage5SchemaVersionScalarGuard: PASS ($($specs.Count) owned comparisons guarded)"
