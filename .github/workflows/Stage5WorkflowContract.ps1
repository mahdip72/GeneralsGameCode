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

function Assert-Stage5CanonicalDownloadArtifactPins {
    param([string]$Content, [int]$ExpectedCount, [string]$Context)

    $canonicalReference =
        'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3'
    $references = [regex]::Matches($Content,
        '(?m)^\s*uses:\s*actions/download-artifact@(?<Ref>[^\s#]+)')
    Assert-Stage5WorkflowCondition ($references.Count -eq $ExpectedCount) `
        "$Context must contain exactly $ExpectedCount download-artifact references."
    foreach ($reference in $references) {
        $value = $reference.Groups['Ref'].Value
        Assert-Stage5WorkflowCondition ($value -cmatch '^[0-9a-f]{40}$') `
            "$Context contains a download-artifact reference that is not an exact 40-hex commit: $value"
        Assert-Stage5WorkflowCondition `
            (('actions/download-artifact@' + $value) -ceq $canonicalReference) `
            "$Context does not use the canonical download-artifact v8.0.0 pin."
    }
}

function Assert-Stage5CanonicalUploadArtifactPins {
    param([string]$Content, [int]$ExpectedCount, [string]$Context)

    $canonicalReference =
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f'
    $references = [regex]::Matches($Content,
        '(?m)^\s*uses:\s*actions/upload-artifact@(?<Ref>[^\s#]+)')
    Assert-Stage5WorkflowCondition ($references.Count -eq $ExpectedCount) `
        "$Context must contain exactly $ExpectedCount upload-artifact references."
    foreach ($reference in $references) {
        $value = $reference.Groups['Ref'].Value
        Assert-Stage5WorkflowCondition ($value -cmatch '^[0-9a-f]{40}$') `
            "$Context contains an upload-artifact reference that is not an exact 40-hex commit: $value"
        Assert-Stage5WorkflowCondition `
            (('actions/upload-artifact@' + $value) -ceq $canonicalReference) `
            "$Context does not use the canonical upload-artifact v7.0.0 pin."
    }
}

function Assert-Stage5CurrentRunArtifactDownload {
    param(
        [string]$Step,
        [string]$ExpectedName,
        [string]$ExpectedPath,
        [string]$Context
    )

    Assert-Stage5CanonicalDownloadArtifactPins $Step 1 $Context
    Assert-Stage5ExactActionStep $Step `
        'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3' `
        ([ordered]@{ name = $ExpectedName; path = $ExpectedPath }) $Context
    Assert-Stage5WorkflowFailClosedStep $Step `
        "$Context unconditional current-run download"
    Assert-Stage5WorkflowContains $Step '(?m)^ {8}with:\s*$' `
        "$Context artifact input map"
    $withBlock = Get-Stage5IndentedBlock $Step 'with:' 8
    $withKeys = @(Get-Stage5CanonicalYamlMappingEntries `
            $withBlock 10 "$Context artifact inputs" |
        ForEach-Object { $_.Key })
    Assert-Stage5WorkflowCondition ($withKeys.Count -eq 2 -and
        $withKeys[0] -ceq 'name' -and $withKeys[1] -ceq 'path') `
        "$Context must use only current-run artifact name and path inputs."
    Assert-Stage5WorkflowContains $Step `
        ('(?m)^ {10}name:\s*' + [regex]::Escape($ExpectedName) + '\s*$') `
        "$Context exact artifact name"
    Assert-Stage5WorkflowContains $Step `
        ('(?m)^ {10}path:\s*' + [regex]::Escape($ExpectedPath) + '\s*$') `
        "$Context exact artifact path"
}

function Assert-Stage5ProductArtifactIsolation {
    param([string]$Content, [string]$Context)

    $contractStep = Get-Stage5IndentedBlock $Content `
        '- name: Run installed native contract tests' 6
    Assert-Stage5WorkflowLiteral $contractStep `
        'installed\Validation' `
        "$Context isolated validation install"
    Assert-Stage5WorkflowLiteral $contractStep `
        'Push-Location $installedRuntime' `
        "$Context product-runtime working directory"

    $collectStep = Get-Stage5IndentedBlock $Content `
        '- name: Collect ${{ inputs.game }} ${{ inputs.preset }}${{ inputs.tools && ''+t'' || '''' }}${{ inputs.extras && ''+e'' || '''' }} Artifact' 6
    Assert-Stage5WorkflowLiteral $collectStep `
        '$installedRuntime = Join-Path $buildDir "installed\${{ inputs.game == ''Generals'' && ''Generals'' || ''ZeroHour'' }}"' `
        "$Context title-only product artifact source"
    Assert-Stage5WorkflowNotContains $collectStep `
        'installed\\Validation|ContractExecutable|runtime_regression_tests|skirmish_ai_runner_contract_tests' `
        "$Context test-only artifact payload"
}

function Assert-Stage5ValidationVolumeHelper {
    param([string]$Content, [string]$Context)

    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseInput(
        $Content, [ref]$tokens, [ref]$parseErrors)
    Assert-Stage5WorkflowCondition (@($parseErrors).Count -eq 0) `
        "$Context PowerShell helper does not parse."
    foreach ($requiredLiteral in @(
        "[ValidateSet('Provision', 'Cleanup')]",
        'Stage5ValidationH-',
        'maximum=32768 type=expandable',
        'attach vdisk',
        'detach vdisk',
        'Get-DiskImage -ImagePath $Paths.BackingFile',
        'Get-Partition -DriveLetter H',
        '$partition.DiskNumber',
        'Get-Volume -Partition $partition',
        '[IO.DriveType]::Fixed',
        '[string[]]$environmentLines',
        '[string[]]$diskpartCommands',
        '[string[]]$detachCommands',
        'AppendAllLines',
        'WriteAllLines',
        'ReadAllLines',
        'Write-Stage5ValidationCommandFile',
        'Stage5ValidationVolumeSelfTest-',
        'RTS_STAGE5_VALIDATION_VHD_OWNED=true',
        'RTS_STAGE5_VALIDATION_VHD_PATH=',
        'RTS_STAGE5_VALIDATION_VHD_TOKEN=',
        'RTS_STAGE5_VALIDATION_VHD_MARKER=',
        '[IO.Path]::Combine',
        'Stage5CiScratch',
        '.stage5-vhd-owner',
        'Remove-Item -LiteralPath $Paths.ScratchRoot -Recurse -Force',
        'remainingImage.Attached')) {
        Assert-Stage5WorkflowLiteral $Content $requiredLiteral $Context
    }
    Assert-Stage5WorkflowNotContains $Content 'subst\.exe\s+H:' `
        "$Context must not mutate subst mappings."
    Assert-Stage5WorkflowNotContains $Content `
        ([regex]::Escape("Join-Path 'H:\Stage5CiScratch'")) `
        "$Context must not require a provider drive while planning future H:."
    $cleanupStart = $Content.IndexOf(
        'function Invoke-Stage5ValidationVolumeCleanup',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupStart -ge 0) `
        "$Context cleanup function is missing."
    $cleanupContent = $Content.Substring($cleanupStart)
    $cleanupImageQueryIndex = $cleanupContent.IndexOf(
        '$diskImage = Get-DiskImage -ImagePath $Paths.BackingFile',
        [StringComparison]::Ordinal)
    $cleanupPartitionQueryIndex = $cleanupContent.IndexOf(
        '$partition = Get-Partition -DriveLetter H',
        [StringComparison]::Ordinal)
    $cleanupScratchRemovalIndex = $cleanupContent.IndexOf(
        'Remove-Item -LiteralPath $Paths.ScratchRoot -Recurse -Force',
        [StringComparison]::Ordinal)
    $cleanupDetachIndex = $cleanupContent.IndexOf(
        '& diskpart.exe /s $Paths.DetachScript', [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupImageQueryIndex -ge 0 -and
        $cleanupPartitionQueryIndex -ge 0 -and $cleanupScratchRemovalIndex -ge 0 -and
        $cleanupDetachIndex -ge 0 -and
        $cleanupImageQueryIndex -lt $cleanupScratchRemovalIndex -and
        $cleanupPartitionQueryIndex -lt $cleanupScratchRemovalIndex -and
        $cleanupScratchRemovalIndex -lt $cleanupDetachIndex) `
        "$Context must validate image/partition identity before scratch removal and detach."
}

function Assert-Stage5ValidationVolumeInvocation {
    param(
        [string]$Step,
        [ValidateSet('Provision', 'Cleanup')]
        [string]$Mode,
        [string]$TokenPattern,
        [string]$Context
    )

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    $matchingCalls = @($ast.FindAll({
        param($node)
        if ($node -isnot [Management.Automation.Language.CommandAst] -or
            $node.InvocationOperator -ne
                [Management.Automation.Language.TokenKind]::Ampersand -or
            $node.CommandElements.Count -eq 0 -or
            $node.Parent -isnot [Management.Automation.Language.PipelineAst] -or
            $node.Parent.PipelineElements.Count -ne 1 -or
            $node.Parent.Parent -isnot
                [Management.Automation.Language.NamedBlockAst] -or
            $node.Parent.Parent.Parent -ne $ast) {
            return $false
        }
        $command = $node.CommandElements[0]
        return ($command.PSObject.Properties.Name -contains 'Value' -and
            [string]$command.Value -ceq
            '$env:GITHUB_WORKSPACE/.github/workflows/Stage5ValidationVolume.ps1')
    }, $true))
    Assert-Stage5WorkflowCondition ($matchingCalls.Count -eq 1) `
        "$Context must execute exactly one direct validation-volume helper call."
    $elements = @($matchingCalls[0].CommandElements)
    $arguments = [ordered]@{}
    $consumedArgumentIndex = -1
    for ($index = 1; $index -lt $elements.Count; ++$index) {
        if ($index -eq $consumedArgumentIndex) { continue }
        $element = $elements[$index]
        Assert-Stage5WorkflowCondition `
            ($element -is [Management.Automation.Language.CommandParameterAst]) `
            "$Context helper call contains an unbound positional argument."
        $name = [string]$element.ParameterName
        Assert-Stage5WorkflowCondition (-not $arguments.Contains($name)) `
            "$Context helper call repeats -$name."
        $argument = $null
        if ($null -ne $element.Argument) {
            $argument = [string]$element.Argument.Extent.Text
        }
        elseif ($index + 1 -lt $elements.Count -and
            $elements[$index + 1] -isnot
                [Management.Automation.Language.CommandParameterAst]) {
            $argument = [string]$elements[$index + 1].Extent.Text
            $consumedArgumentIndex = $index + 1
        }
        $arguments[$name] = $argument
    }
    Assert-Stage5WorkflowCondition ($arguments.Count -eq 2 -and
        $arguments.Contains('Mode') -and $arguments.Contains('Token')) `
        "$Context helper call must bind exactly -Mode and -Token."
    Assert-Stage5WorkflowCondition ([string]$arguments['Mode'] -ceq $Mode) `
        "$Context helper call mode is not exactly '$Mode'."
    Assert-Stage5WorkflowCondition ([string]$arguments['Token'] -match $TokenPattern) `
        "$Context helper call token is not lane-unique and run-bound."
}

function Assert-Stage5ValidationVolumeBinding {
    param(
        [string]$Content,
        [string]$ProvisionStepName,
        [string]$CleanupStepName,
        [string]$ProvisionIf,
        [string]$CleanupIf,
        [string]$TokenPattern,
        [string[]]$UploadStepNames,
        [AllowNull()][Collections.IDictionary]$ProvisionEnvironment = $null,
        [string]$Context
    )

    $provisionStep = Get-Stage5IndentedBlock $Content `
        ('- name: ' + $ProvisionStepName) 6
    $cleanupStep = Get-Stage5IndentedBlock $Content `
        ('- name: ' + $CleanupStepName) 6
    Assert-Stage5ExactPowerShellStepMetadata $provisionStep `
        $ProvisionEnvironment $ProvisionIf `
        "$Context exact volume-provisioning step"
    Assert-Stage5ExactPowerShellStepMetadata $cleanupStep $null $CleanupIf `
        "$Context exact volume-cleanup step"
    Assert-Stage5ValidationVolumeInvocation $provisionStep 'Provision' `
        $TokenPattern "$Context volume-provisioning helper binding"
    Assert-Stage5ValidationVolumeInvocation $cleanupStep 'Cleanup' `
        $TokenPattern "$Context volume-cleanup helper binding"
    Assert-Stage5WorkflowNotContains $Content 'subst\.exe\s+H:' `
        "$Context live subst H: mutation"
    $cleanupIndex = $Content.IndexOf(
        ('- name: ' + $CleanupStepName), [StringComparison]::Ordinal)
    $lastUploadIndex = -1
    foreach ($uploadStepName in $UploadStepNames) {
        $uploadIndex = $Content.IndexOf(
            ('- name: ' + $uploadStepName), [StringComparison]::Ordinal)
        Assert-Stage5WorkflowCondition ($uploadIndex -ge 0) `
            "$Context required upload step is missing: $uploadStepName"
        if ($uploadIndex -gt $lastUploadIndex) { $lastUploadIndex = $uploadIndex }
    }
    Assert-Stage5WorkflowCondition ($cleanupIndex -gt $lastUploadIndex) `
        "$Context must clean the volume only after all lane artifacts upload."
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
    $matchingStarts = New-Object 'Collections.Generic.List[int]'
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index].TrimEnd() -ceq $prefix) {
            $matchingStarts.Add($index) | Out-Null
        }
    }
    Assert-Stage5WorkflowCondition ($matchingStarts.Count -eq 1) `
        "Workflow block '$Marker' must occur exactly once at indent $Indent (found $($matchingStarts.Count))."
    $start = $matchingStarts[0]
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

function ConvertTo-Stage5SelfTestLf {
    param([string]$Content)
    return [regex]::Replace($Content, "`r`n|`r", "`n")
}

function Get-Stage5WorkflowEffectiveContent {
    param([string]$Content)

    return (@($Content -split '\r?\n' | Where-Object {
        -not $_.TrimStart().StartsWith('#')
    }) -join "`n")
}

function Get-Stage5CanonicalYamlMappingEntries {
    param(
        [string]$Content,
        [int]$Indent,
        [string]$Context
    )

    $entries = New-Object 'Collections.Generic.List[object]'
    foreach ($line in @($Content -split '\r?\n')) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $lineIndent = $line.Length - $line.TrimStart().Length
        if ($lineIndent -ne $Indent) { continue }
        $text = $line.Substring($Indent)
        if ($text.TrimStart().StartsWith('#')) { continue }
        $match = [regex]::Match($text,
            '^(?<Key>[A-Za-z_][A-Za-z0-9_-]*):(?<Value>.*)$')
        Assert-Stage5WorkflowCondition $match.Success `
            "$Context contains a quoted, explicit, merged, or otherwise non-canonical YAML mapping key."
        $entries.Add([pscustomobject]@{
                Key = [string]$match.Groups['Key'].Value
                Value = [string]$match.Groups['Value'].Value.Trim()
            }) | Out-Null
    }
    return $entries.ToArray()
}

function Assert-Stage5NoJobRunDefaults {
    param([string]$Job, [string]$Context)

    $entries = @(Get-Stage5CanonicalYamlMappingEntries $Job 4 $Context)
    Assert-Stage5WorkflowCondition (-not (@($entries | Where-Object {
                    [string]$_.Key -ceq 'defaults'
                }).Count -gt 0)) "$Context contains job-level run defaults."
}

function Assert-Stage5ExactJobSchema {
    param(
        [string]$Job,
        [Collections.IDictionary]$ExpectedProperties,
        [string]$Context
    )

    $entries = @(Get-Stage5CanonicalYamlMappingEntries $Job 4 $Context)
    Assert-Stage5WorkflowCondition ($entries.Count -eq
        $ExpectedProperties.Count) "$Context top-level property count is not exact."
    $expectedKeys = @($ExpectedProperties.Keys | ForEach-Object { [string]$_ })
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $entry = $entries[$index]
        Assert-Stage5WorkflowCondition ([string]$entry.Key -ceq
            [string]$expectedKeys[$index]) `
            "$Context top-level property $index is not exactly '$($expectedKeys[$index])'."
        Assert-Stage5WorkflowCondition ([string]$entry.Value -ceq
            [string]$ExpectedProperties[$expectedKeys[$index]]) `
            "$Context top-level property '$($expectedKeys[$index])' has a substituted value."
    }
    foreach ($forbidden in @('continue-on-error', 'strategy', 'container',
            'services', 'defaults')) {
        Assert-Stage5WorkflowCondition (-not ($expectedKeys -ccontains $forbidden)) `
            "$Context expected schema must not authorize job-level $forbidden."
    }
}

function Get-Stage5WorkflowPowerShellRunBody {
    param([string]$Step, [string]$Context)

    $lines = @($Step -split '\r?\n')
    $runLineIndex = -1
    $runIndent = -1
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -match '^(?<Indent>\s*)run:\s*\|\s*$') {
            $runLineIndex = $index
            $runIndent = $Matches.Indent.Length
            break
        }
    }
    Assert-Stage5WorkflowCondition ($runLineIndex -ge 0) `
        "$Context does not contain a PowerShell literal run block."

    $bodyIndent = $runIndent + 2
    $bodyLines = New-Object 'Collections.Generic.List[string]'
    for ($index = $runLineIndex + 1; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) {
            $bodyLines.Add('') | Out-Null
            continue
        }
        $lineIndent = $line.Length - $line.TrimStart().Length
        Assert-Stage5WorkflowCondition ($lineIndent -ge $bodyIndent) `
            "$Context contains a run-body line outside the YAML literal block."
        $bodyLines.Add($line.Substring($bodyIndent)) | Out-Null
    }

    return ($bodyLines.ToArray() -join "`n")
}

function Get-Stage5WorkflowPowerShellRunAst {
    param([string]$Step, [string]$Context)

    $tokens = $null
    $parseErrors = $null
    $body = Get-Stage5WorkflowPowerShellRunBody $Step $Context
    $parseBody = [regex]::Replace(
        $body,
        '(?s)\$\{\{.*?\}\}', '__STAGE5_GITHUB_EXPRESSION__')
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $parseBody, [ref]$tokens, [ref]$parseErrors)
    Assert-Stage5WorkflowCondition (@($parseErrors).Count -eq 0) `
        "$Context PowerShell run block does not parse."
    return $ast
}

function Assert-Stage5ExactTopLevelPowerShellStatements {
    param(
        [string]$Step,
        [string[]]$ExpectedStatements,
        [string]$Context
    )

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    Assert-Stage5WorkflowCondition ($null -eq $ast.BeginBlock -and
        $null -eq $ast.ProcessBlock -and $null -ne $ast.EndBlock) `
        "$Context must contain only an end block."
    $statements = @($ast.EndBlock.Statements)
    Assert-Stage5WorkflowCondition ($statements.Count -eq
        $ExpectedStatements.Count) `
        "$Context has an unexpected executable statement count."
    for ($index = 0; $index -lt $ExpectedStatements.Count; ++$index) {
        Assert-Stage5WorkflowCondition `
            ([string]$statements[$index].Extent.Text -ceq
                [string]$ExpectedStatements[$index]) `
            "$Context statement $index is not exact."
    }
}

function Assert-Stage5ClosedPowerShellRunBlock {
    param(
        [string]$Step,
        [string]$ExpectedSha256,
        [int]$ExpectedStatementCount,
        [string]$Context
    )

    Assert-Stage5WorkflowCondition ($ExpectedSha256 -cmatch '^[0-9A-F]{64}$') `
        "$Context expected run-block hash is malformed."
    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    Assert-Stage5WorkflowCondition ($null -eq $ast.BeginBlock -and
        $null -eq $ast.ProcessBlock -and $null -ne $ast.EndBlock) `
        "$Context must contain only an end block."
    Assert-Stage5WorkflowCondition `
        (@($ast.EndBlock.Statements).Count -eq $ExpectedStatementCount) `
        "$Context has an unexpected executable statement count."

    $body = Get-Stage5WorkflowPowerShellRunBody $Step $Context
    $canonicalProgram = ([string]$body -replace "`r`n", "`n").TrimEnd()
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($canonicalProgram)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $actualSha256 = ([BitConverter]::ToString(
            $hasher.ComputeHash($bytes))).Replace('-', '')
    }
    finally { $hasher.Dispose() }
    Assert-Stage5WorkflowCondition ($actualSha256 -ceq $ExpectedSha256) `
        "$Context executable program is not exact."
}

function Assert-Stage5ExactJobStepSequence {
    param(
        [string]$Job,
        [string[]]$ExpectedNames,
        [string[]]$ExpectedKinds,
        [string]$Context
    )

    Assert-Stage5WorkflowCondition ($ExpectedNames.Count -eq
        $ExpectedKinds.Count) "$Context expected step contract is malformed."
    Assert-Stage5WorkflowNotContains $Job '(?m)^ {6}- (?!name:)' `
        "$Context anonymous step"
    $stepNames = @([regex]::Matches($Job,
            '(?m)^ {6}- name:[ \t]*(?<Name>[^\r\n]+)\r?$') |
        ForEach-Object { $_.Groups['Name'].Value })
    Assert-Stage5WorkflowCondition ($stepNames.Count -eq
        $ExpectedNames.Count) "$Context has an unexpected step count."
    for ($index = 0; $index -lt $ExpectedNames.Count; ++$index) {
        Assert-Stage5WorkflowCondition `
            ([string]$stepNames[$index] -ceq [string]$ExpectedNames[$index]) `
            "$Context step $index is not exactly '$($ExpectedNames[$index])'."
        Assert-Stage5WorkflowCondition `
            (@('run', 'uses') -ccontains [string]$ExpectedKinds[$index]) `
            "$Context expected kind for step $index is invalid."
        $step = Get-Stage5IndentedBlock $Job `
            ('- name: ' + $ExpectedNames[$index]) 6
        $directKinds = @([regex]::Matches($step,
                '(?m)^ {8}(?<Kind>run|uses):') |
            ForEach-Object { $_.Groups['Kind'].Value })
        Assert-Stage5WorkflowCondition ($directKinds.Count -eq 1 -and
            [string]$directKinds[0] -ceq [string]$ExpectedKinds[$index]) `
            "$Context step '$($ExpectedNames[$index])' must have exactly one direct $($ExpectedKinds[$index]) action."
    }
}

function Assert-Stage5ExactCheckoutStep {
    param(
        [string]$Step,
        [bool]$RequireFullHistory,
        [string]$Context
    )

    Assert-Stage5WorkflowFailClosedStep $Step "$Context unconditional checkout"
    Assert-Stage5WorkflowContains $Step `
        '(?m)^ {8}uses:\s*actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd(?:\s+#.*)?$' `
        "$Context exact checkout v6.0.2 commit"
    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries $Step 8 $Context |
        ForEach-Object { $_.Key })
    $expectedKeys = @(if ($RequireFullHistory) { 'uses'; 'with' }
        else { 'uses' })
    Assert-Stage5WorkflowCondition ($directKeys.Count -eq
        $expectedKeys.Count) "$Context has unexpected direct step properties."
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        Assert-Stage5WorkflowCondition `
            ([string]$directKeys[$index] -ceq [string]$expectedKeys[$index]) `
            "$Context direct step properties are not exact."
    }
    if ($RequireFullHistory) {
        $withBlock = Get-Stage5IndentedBlock $Step 'with:' 8
        $withKeys = @(Get-Stage5CanonicalYamlMappingEntries `
                $withBlock 10 "$Context checkout inputs" |
            ForEach-Object { $_.Key })
        Assert-Stage5WorkflowCondition ($withKeys.Count -eq 1 -and
            [string]$withKeys[0] -ceq 'fetch-depth') `
            "$Context checkout inputs are not exact."
        Assert-Stage5WorkflowContains $Step `
            '(?m)^ {10}fetch-depth:\s*0\s*$' `
            "$Context must check out the complete current commit history."
    }
}

function Assert-Stage5ExactActionStep {
    param(
        [string]$Step,
        [string]$ExpectedAction,
        [Collections.IDictionary]$ExpectedInputs,
        [string]$Context
    )

    Assert-Stage5WorkflowFailClosedStep $Step "$Context unconditional action"
    Assert-Stage5NoStepEnvironment $Step "$Context inherited action identity"
    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries $Step 8 $Context |
        ForEach-Object { $_.Key })
    Assert-Stage5WorkflowCondition ($directKeys.Count -eq 2 -and
        [string]$directKeys[0] -ceq 'uses' -and
        [string]$directKeys[1] -ceq 'with') `
        "$Context direct action metadata is not exact."
    $usesMatch = [regex]::Match($Step,
        '(?m)^ {8}uses:\s*(?<Action>[^\s#]+)(?:\s+#.*)?$')
    Assert-Stage5WorkflowCondition ($usesMatch.Success -and
        [string]$usesMatch.Groups['Action'].Value -ceq $ExpectedAction) `
        "$Context action reference is not exact."

    $withBlock = Get-Stage5IndentedBlock $Step 'with:' 8
    $entries = @(Get-Stage5CanonicalYamlMappingEntries `
            $withBlock 10 "$Context action inputs")
    Assert-Stage5WorkflowCondition ($entries.Count -eq
        $ExpectedInputs.Count) "$Context action input count is not exact."
    $expectedInputKeys = @($ExpectedInputs.Keys | ForEach-Object { [string]$_ })
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $key = [string]$entries[$index].Key
        Assert-Stage5WorkflowCondition ($key -ceq
            [string]$expectedInputKeys[$index]) `
            "$Context action input order is not exact."
        Assert-Stage5WorkflowCondition ([string]$entries[$index].Value -ceq
            [string]$ExpectedInputs[$key]) `
            "$Context action input $key is not exact."
    }
}

function Assert-Stage5ExactConditionalActionStep {
    param(
        [string]$Step,
        [string]$ExpectedAction,
        [Collections.IDictionary]$ExpectedInputs,
        [string]$ExpectedIf = '',
        [string]$Context
    )

    Assert-Stage5NoStepEnvironment $Step "$Context inherited action identity"
    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries $Step 8 $Context |
        ForEach-Object { $_.Key })
    $expectedKeys = New-Object 'Collections.Generic.List[string]'
    if (-not [string]::IsNullOrEmpty($ExpectedIf)) {
        $expectedKeys.Add('if') | Out-Null
    }
    $expectedKeys.Add('uses') | Out-Null
    $expectedKeys.Add('with') | Out-Null
    Assert-Stage5WorkflowCondition ($directKeys.Count -eq $expectedKeys.Count) `
        "$Context direct action metadata is not exact."
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        Assert-Stage5WorkflowCondition ([string]$directKeys[$index] -ceq
            [string]$expectedKeys[$index]) `
            "$Context direct action metadata order is not exact."
    }
    if ([string]::IsNullOrEmpty($ExpectedIf)) {
        Assert-Stage5WorkflowFailClosedStep $Step "$Context unconditional action"
    }
    else {
        Assert-Stage5WorkflowContains $Step `
            ('(?m)^ {8}if:\s*' + [regex]::Escape($ExpectedIf) + '\s*$') `
            "$Context exact action condition"
    }
    $usesMatch = [regex]::Match($Step,
        '(?m)^ {8}uses:\s*(?<Action>[^\s#]+)(?:\s+#.*)?$')
    Assert-Stage5WorkflowCondition ($usesMatch.Success -and
        [string]$usesMatch.Groups['Action'].Value -ceq $ExpectedAction) `
        "$Context action reference is not exact."

    $withBlock = Get-Stage5IndentedBlock $Step 'with:' 8
    $entries = @(Get-Stage5CanonicalYamlMappingEntries `
            $withBlock 10 "$Context action inputs")
    Assert-Stage5WorkflowCondition ($entries.Count -eq $ExpectedInputs.Count) `
        "$Context action input count is not exact."
    $expectedInputKeys = @($ExpectedInputs.Keys | ForEach-Object { [string]$_ })
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $key = [string]$entries[$index].Key
        Assert-Stage5WorkflowCondition ($key -ceq
            [string]$expectedInputKeys[$index]) `
            "$Context action input order is not exact."
        Assert-Stage5WorkflowCondition ([string]$entries[$index].Value -ceq
            [string]$ExpectedInputs[$key]) `
            "$Context action input $key is not exact."
    }
}

function Assert-Stage5ExactLiteralActionInputLines {
    param(
        [string]$Step,
        [string]$InputName,
        [string[]]$ExpectedLines,
        [string]$Context
    )

    $lines = @($Step -split '\r?\n')
    $marker = ' ' * 10 + $InputName + ': |'
    $start = [Array]::IndexOf($lines, $marker)
    Assert-Stage5WorkflowCondition ($start -ge 0) `
        "$Context exact literal action input is missing."
    $actualLines = New-Object 'Collections.Generic.List[string]'
    for ($index = $start + 1; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $indent = $line.Length - $line.TrimStart().Length
        if ($indent -le 10) { break }
        Assert-Stage5WorkflowCondition ($indent -eq 12) `
            "$Context literal action input contains unexpected nesting."
        $actualLines.Add($line.Substring(12)) | Out-Null
    }
    Assert-Stage5WorkflowCondition ($actualLines.Count -eq
        $ExpectedLines.Count) "$Context literal action input line count is not exact."
    for ($index = 0; $index -lt $ExpectedLines.Count; ++$index) {
        Assert-Stage5WorkflowCondition `
            ([string]$actualLines[$index] -ceq [string]$ExpectedLines[$index]) `
            "$Context literal action input line $index is not exact."
    }
}

function Assert-Stage5NoStepEnvironment {
    param([string]$Step, [string]$Context)

    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries `
            $Step 8 "$Context step metadata" | ForEach-Object { $_.Key })
    Assert-Stage5WorkflowCondition (-not ($directKeys -ccontains 'env')) `
        "$Context contains a step-local environment override."
}

function Assert-Stage5ExactStepEnvironment {
    param(
        [string]$Step,
        [Collections.IDictionary]$ExpectedEnvironment,
        [string]$Context
    )

    Assert-Stage5WorkflowContains $Step '(?m)^ {8}env:\s*$' `
        "$Context environment map"
    $environment = Get-Stage5IndentedBlock $Step 'env:' 8
    $entries = @(Get-Stage5CanonicalYamlMappingEntries `
            $environment 10 "$Context environment entries")
    Assert-Stage5WorkflowCondition ($entries.Count -eq
        $ExpectedEnvironment.Count) "$Context environment key count is not exact."
    $expectedEnvironmentKeys = @($ExpectedEnvironment.Keys |
        ForEach-Object { [string]$_ })
    $actual = [ordered]@{}
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $key = [string]$entries[$index].Key
        Assert-Stage5WorkflowCondition ($key -ceq
            [string]$expectedEnvironmentKeys[$index]) `
            "$Context environment key order is not exact."
        Assert-Stage5WorkflowCondition (-not $actual.Contains($key)) `
            "$Context repeats environment key $key."
        $actual[$key] = [string]$entries[$index].Value
        Assert-Stage5WorkflowCondition ([string]$entries[$index].Value -ceq
            [string]$ExpectedEnvironment[$key]) `
            "$Context environment value for $key is not exact."
    }
    foreach ($key in @($actual.Keys)) {
        if ([string]$key -cmatch '^STAGE5_') {
            Assert-Stage5WorkflowCondition `
                ($ExpectedEnvironment.Contains([string]$key)) `
                "$Context contains a protected Stage 5 step override."
        }
    }
}

function Assert-Stage5ExactPowerShellStepMetadata {
    param(
        [string]$Step,
        [AllowNull()][Collections.IDictionary]$ExpectedEnvironment,
        [string]$ExpectedIf = '',
        [string]$Context
    )

    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries $Step 8 $Context |
        ForEach-Object { $_.Key })
    $expectedKeys = New-Object 'Collections.Generic.List[string]'
    if (-not [string]::IsNullOrEmpty($ExpectedIf)) {
        $expectedKeys.Add('if') | Out-Null
    }
    $expectedKeys.Add('shell') | Out-Null
    if ($null -ne $ExpectedEnvironment) {
        $expectedKeys.Add('env') | Out-Null
    }
    $expectedKeys.Add('run') | Out-Null
    Assert-Stage5WorkflowCondition ($directKeys.Count -eq
        $expectedKeys.Count) "$Context direct step metadata is not exact."
    for ($index = 0; $index -lt $expectedKeys.Count; ++$index) {
        Assert-Stage5WorkflowCondition `
            ([string]$directKeys[$index] -ceq [string]$expectedKeys[$index]) `
            "$Context direct step metadata order is not exact."
    }
    Assert-Stage5WorkflowContains $Step '(?m)^ {8}shell:\s*pwsh\s*$' `
        "$Context exact PowerShell shell"
    Assert-Stage5WorkflowContains $Step '(?m)^ {8}run:\s*\|\s*$' `
        "$Context literal run program"
    if ([string]::IsNullOrEmpty($ExpectedIf)) {
        Assert-Stage5WorkflowFailClosedStep $Step "$Context unconditional step"
    }
    else {
        Assert-Stage5WorkflowContains $Step `
            ('(?m)^ {8}if:\s*' + [regex]::Escape($ExpectedIf) + '\s*$') `
            "$Context exact step condition"
        Assert-Stage5WorkflowNotContains $Step '(?m)^ {8}continue-on-error:' `
            "$Context continue-on-error bypass"
    }
    if ($null -eq $ExpectedEnvironment) {
        Assert-Stage5NoStepEnvironment $Step "$Context inherited environment"
    }
    else {
        Assert-Stage5ExactStepEnvironment $Step $ExpectedEnvironment `
            "$Context exact step environment"
    }
}

function Assert-Stage5ExactJobEnvironment {
    param(
        [string]$Job,
        [Collections.IDictionary]$ExpectedEnvironment,
        [string]$Context
    )

    $environment = Get-Stage5IndentedBlock $Job 'env:' 4
    $entries = @(Get-Stage5CanonicalYamlMappingEntries `
            $environment 6 "$Context environment entries")
    Assert-Stage5WorkflowCondition ($entries.Count -eq
        $ExpectedEnvironment.Count) "$Context environment key count is not exact."
    $expectedEnvironmentKeys = @($ExpectedEnvironment.Keys |
        ForEach-Object { [string]$_ })
    $actual = [ordered]@{}
    for ($index = 0; $index -lt $entries.Count; ++$index) {
        $key = [string]$entries[$index].Key
        Assert-Stage5WorkflowCondition ($key -ceq
            [string]$expectedEnvironmentKeys[$index]) `
            "$Context environment key order is not exact."
        Assert-Stage5WorkflowCondition (-not $actual.Contains($key)) `
            "$Context repeats environment key $key."
        $actual[$key] = [string]$entries[$index].Value
        Assert-Stage5WorkflowCondition ([string]$entries[$index].Value -ceq
            [string]$ExpectedEnvironment[$key]) `
            "$Context environment value for $key is not exact."
    }
}

function Assert-Stage5ExactFoldedJobCondition {
    param(
        [string]$Condition,
        [string]$ExpectedExpression,
        [string]$Context
    )

    $normalized = (@($Condition -split '\r?\n' |
        ForEach-Object { $_.Trim() }) -join ' ')
    $expected = 'if: >- ${{ ' + $ExpectedExpression + ' }}'
    Assert-Stage5WorkflowCondition ($normalized -ceq $expected) `
        "$Context must use the exact fail-closed condition."
}

function Assert-Stage5WorkflowExecutablePowerShellCall {
    param(
        [string]$Step,
        [string]$ExpectedCommand,
        [string]$Context,
        [Collections.IDictionary]$RequiredArguments = @{}
    )

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    $matchingCalls = @($ast.FindAll({
        param($node)
        if ($node -isnot [Management.Automation.Language.CommandAst] -or
            $node.InvocationOperator -ne
                [Management.Automation.Language.TokenKind]::Ampersand -or
            $node.CommandElements.Count -eq 0 -or
            $node.Parent -isnot [Management.Automation.Language.PipelineAst] -or
            $node.Parent.PipelineElements.Count -ne 1 -or
            $node.Parent.Parent -isnot
                [Management.Automation.Language.NamedBlockAst] -or
            $node.Parent.Parent.Parent -ne $ast) {
            return $false
        }
        $command = $node.CommandElements[0]
        return ($command.PSObject.Properties.Name -contains 'Value' -and
            [string]$command.Value -ceq $ExpectedCommand)
    }, $true))
    Assert-Stage5WorkflowCondition ($matchingCalls.Count -eq 1) `
        "$Context must execute exactly one direct top-level call to $ExpectedCommand."
    $actualArguments = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::Ordinal)
    $elements = @($matchingCalls[0].CommandElements)
    $consumedArgumentIndex = -1
    for ($index = 1; $index -lt $elements.Count; ++$index) {
        if ($index -eq $consumedArgumentIndex) { continue }
        $element = $elements[$index]
        if ($element -isnot
            [Management.Automation.Language.CommandParameterAst]) {
            throw "$Context executable call contains an unbound positional argument."
        }
        $name = [string]$element.ParameterName
        Assert-Stage5WorkflowCondition (-not $actualArguments.ContainsKey($name)) `
            "$Context executable call repeats -$name."
        $argument = $null
        if ($null -ne $element.Argument) {
            $argument = [string]$element.Argument.Extent.Text
        }
        elseif ($index + 1 -lt $elements.Count -and
            $elements[$index + 1] -isnot
                [Management.Automation.Language.CommandParameterAst]) {
            $argument = [string]$elements[$index + 1].Extent.Text
            $consumedArgumentIndex = $index + 1
        }
        $actualArguments.Add($name, $argument)
    }
    Assert-Stage5WorkflowCondition ($actualArguments.Count -eq
        $RequiredArguments.Count) `
        "$Context executable call has an unexpected parameter set."
    foreach ($requiredParameter in @($RequiredArguments.Keys)) {
        Assert-Stage5WorkflowCondition `
            ($actualArguments.ContainsKey([string]$requiredParameter)) `
            "$Context executable call is missing -$requiredParameter."
        $expectedArgument = $RequiredArguments[$requiredParameter]
        $actualArgument = $actualArguments[[string]$requiredParameter]
        Assert-Stage5WorkflowCondition `
            (($null -eq $expectedArgument -and $null -eq $actualArgument) -or
             ($null -ne $expectedArgument -and $null -ne $actualArgument -and
              [string]$actualArgument -ceq [string]$expectedArgument)) `
            "$Context executable -$requiredParameter argument is not exactly '$expectedArgument'."
    }
}

function Assert-Stage5RepositoryRelativeFileGuard {
    param(
        [string]$Step,
        [string]$Context,
        [Collections.IDictionary]$RequiredResolutions
    )

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    $definitions = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Resolve-Stage5RepositoryFile' -and
        $node.Parent -is [Management.Automation.Language.NamedBlockAst] -and
        $node.Parent.Parent -eq $ast
    }, $true))
    Assert-Stage5WorkflowCondition ($definitions.Count -eq 1) `
        "$Context must define one direct top-level repository-file resolver."

    $functionAst = $definitions[0]
    $resolverBytes = [Text.UTF8Encoding]::new($false).GetBytes(
        ([string]$functionAst.Extent.Text -replace "`r`n", "`n"))
    $resolverHasher = [Security.Cryptography.SHA256]::Create()
    try {
        $resolverSha256 = ([BitConverter]::ToString(
            $resolverHasher.ComputeHash($resolverBytes))).Replace('-', '')
    }
    finally { $resolverHasher.Dispose() }
    Assert-Stage5WorkflowCondition ($resolverSha256 -ceq
        'C530B4F33427EB73E012A0D4DF45D9B0E5DC2FEC498A92DBF38A1C1900A212E5') `
        "$Context repository-file resolver implementation is not exact."
    $conditionTexts = New-Object 'Collections.Generic.List[string]'
    foreach ($ifStatement in @($functionAst.Body.FindAll({
                param($node)
                $node -is [Management.Automation.Language.IfStatementAst]
            }, $true))) {
        foreach ($clause in @($ifStatement.Clauses)) {
            $conditionTexts.Add([string]$clause.Item1.Extent.Text) | Out-Null
        }
    }
    $activeConditions = $conditionTexts.ToArray() -join "`n"
    foreach ($requiredCondition in @(
        '[string]::IsNullOrWhiteSpace($RelativePath)',
        '[IO.Path]::IsPathRooted($RelativePath)',
        '$RelativePath -match '':''',
        '$RelativePath -match ''[\x00-\x1F\x7F]''',
        '$RelativePath -match ''(^|[\\/])\.{1,2}([\\/]|$)''',
        '-not $rootItem.PSIsContainer',
        '($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0',
        '-not $candidate.StartsWith(',
        '$rootFull + [IO.Path]::DirectorySeparatorChar',
        '[StringComparison]::OrdinalIgnoreCase',
        '-not (Test-Path -LiteralPath $candidate -PathType Leaf)',
        '($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0')) {
        Assert-Stage5WorkflowLiteral $activeConditions $requiredCondition `
            "$Context active repository containment guard"
    }
    $throws = @($functionAst.Body.FindAll({
        param($node)
        $node -is [Management.Automation.Language.ThrowStatementAst]
    }, $true))
    Assert-Stage5WorkflowCondition ($throws.Count -ge 4) `
        "$Context repository-file resolver must fail closed at every boundary."

    $resolutionCalls = @($ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Resolve-Stage5RepositoryFile' -and
        $node.Parent -is [Management.Automation.Language.PipelineAst] -and
        $node.Parent.PipelineElements.Count -eq 1 -and
        $node.Parent.Parent -is
            [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Parent.Parent.Parent -is
            [Management.Automation.Language.NamedBlockAst] -and
        $node.Parent.Parent.Parent.Parent -eq $ast
    }, $true))
    Assert-Stage5WorkflowCondition ($resolutionCalls.Count -eq
        $RequiredResolutions.Count) `
        "$Context repository-file resolver has an unexpected direct-use set."
    foreach ($leftHandSide in @($RequiredResolutions.Keys)) {
        $matching = @($resolutionCalls | Where-Object {
            [string]$_.Parent.Parent.Left.Extent.Text -ceq [string]$leftHandSide
        })
        Assert-Stage5WorkflowCondition ($matching.Count -eq 1) `
            "$Context must directly resolve $leftHandSide exactly once."
        $actualArguments = @($matching[0].CommandElements | Select-Object -Skip 1 |
            ForEach-Object { [string]$_.Extent.Text })
        $expectedArguments = @($RequiredResolutions[$leftHandSide])
        Assert-Stage5WorkflowCondition ($actualArguments.Count -eq
            $expectedArguments.Count) `
            "$Context $leftHandSide resolution has an unexpected argument count."
        for ($index = 0; $index -lt $expectedArguments.Count; ++$index) {
            Assert-Stage5WorkflowCondition ($actualArguments[$index] -ceq
                [string]$expectedArguments[$index]) `
                "$Context $leftHandSide resolution argument $index is not exact."
        }
    }
}

function Assert-Stage5JsonIntegerSchemaVersionGuards {
    param(
        [string]$Step,
        [int]$ExpectedCount,
        [string]$Context
    )

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    $comparisons = @($ast.FindAll({
            param($node)
            $node -is [Management.Automation.Language.BinaryExpressionAst] -and
                $node.Operator -in @('Ieq', 'Ine') -and
                $node.Extent.Text -match '(?i)(?:schemaVersion|SchemaVersion)'
        }, $true))
    Assert-Stage5WorkflowCondition ($comparisons.Count -eq $ExpectedCount) `
        "$Context must contain exactly $ExpectedCount schemaVersion equality checks."

    foreach ($comparison in $comparisons) {
        $ifCandidates = @($ast.FindAll({
                param($candidate)
                if ($candidate -isnot
                    [Management.Automation.Language.IfStatementAst]) {
                    return $false
                }
                foreach ($clause in @($candidate.Clauses)) {
                    $conditionAst = $clause.Item1
                    if ($conditionAst.Extent.StartOffset -le
                        $comparison.Extent.StartOffset -and
                        $conditionAst.Extent.EndOffset -ge
                        $comparison.Extent.EndOffset) {
                        return $true
                    }
                }
                return $false
            }, $true) | Sort-Object {
                $_.Extent.EndOffset - $_.Extent.StartOffset
            })
        Assert-Stage5WorkflowCondition ($ifCandidates.Count -gt 0) `
            "$Context schemaVersion comparison at line $($comparison.Extent.StartLineNumber) is not inside an if-condition."
        $conditionAst = $null
        foreach ($clause in @($ifCandidates[0].Clauses)) {
            $candidateCondition = $clause.Item1
            if ($candidateCondition.Extent.StartOffset -le
                $comparison.Extent.StartOffset -and
                $candidateCondition.Extent.EndOffset -ge
                $comparison.Extent.EndOffset) {
                $conditionAst = $candidateCondition
                break
            }
        }
        Assert-Stage5WorkflowCondition ($null -ne $conditionAst) `
            "$Context schemaVersion comparison at line $($comparison.Extent.StartLineNumber) has no bounded condition expression."
        $conditionText = [string]$conditionAst.Extent.Text
        $guardMatch = [regex]::Match($conditionText,
            '(?i)Test-Stage5JsonInteger')
        Assert-Stage5WorkflowCondition $guardMatch.Success `
            "$Context schemaVersion comparison at line $($comparison.Extent.StartLineNumber) lacks a strict JSON-integer guard."
        $comparisonIndex = $conditionText.IndexOf(
            [string]$comparison.Extent.Text, [StringComparison]::Ordinal)
        Assert-Stage5WorkflowCondition ($comparisonIndex -gt $guardMatch.Index) `
            "$Context schemaVersion comparison at line $($comparison.Extent.StartLineNumber) occurs before its strict JSON-integer guard."
        Assert-Stage5WorkflowCondition `
            ($comparison.Left.Extent.Text -notmatch '^\s*\[[^]]+\]' -and
             $comparison.Right.Extent.Text -notmatch '^\s*\[[^]]+\]') `
            "$Context schemaVersion comparison at line $($comparison.Extent.StartLineNumber) casts before equality."
    }
}

function Assert-Stage5CheckReplaysCardinalityAndArrayContracts {
    param([string]$Step, [string]$Context)

    $ast = Get-Stage5WorkflowPowerShellRunAst $Step $Context
    foreach ($arrayKey in @('artifacts', 'evidence', 'attachments', 'fixtures')) {
        $keyPattern = "(?i)'" + [regex]::Escape($arrayKey) + "'"
        $calls = @($ast.FindAll({
                param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                    $node.GetCommandName() -ceq 'Get-Stage5JsonValue' -and
                    $node.Extent.Text -match $keyPattern
            }, $true))
        Assert-Stage5WorkflowCondition ($calls.Count -eq 1) `
            "$Context must read '$arrayKey' exactly once through Get-Stage5JsonValue."
        $ancestor = $calls[0].Parent
        $nestedArray = $false
        while ($null -ne $ancestor) {
            if ($ancestor -is
                [Management.Automation.Language.ArrayExpressionAst]) {
                $nestedArray = $true
                break
            }
            $ancestor = $ancestor.Parent
        }
        Assert-Stage5WorkflowCondition (-not $nestedArray) `
            "$Context '$arrayKey' input must not be wrapped in an outer array expression."
    }
    $body = Get-Stage5WorkflowPowerShellRunBody $Step $Context
    foreach ($cardinalityGuard in @(
            '$artifactHashes.Count -ne $requiredArtifactRoles.Count',
            '$evidenceEntries.Count -ne $requiredEvidenceKinds.Count',
            '$seenReplayAttachmentBindings.Count -ne',
            '$fixtures.Count -ne 10')) {
        Assert-Stage5WorkflowLiteral $body $cardinalityGuard `
            "$Context durable cardinality guard"
    }
}

function Assert-Stage5WorkflowContractInvocation {
    param([string]$Content, [string]$Context)

    $job = Get-Stage5IndentedBlock $Content 'stage5-workflow-contract:' 2
    Assert-Stage5ExactJobSchema $job ([ordered]@{
            name = 'Stage 5 Workflow Contract'
            needs = 'detect-changes'
            'if' = '${{ github.event_name == ''workflow_dispatch'' || needs.detect-changes.outputs.stage5 == ''true'' }}'
            'runs-on' = 'windows-2022'
            'timeout-minutes' = '5'
            steps = ''
        }) "$Context exact job schema"
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Validate Stage 5 workflow contract',
        'Validate Stage 5 workflow contract self-test',
        'Validate weekly release promotion attestation self-test') @(
        'uses', 'run', 'run', 'run') "$Context exact closed step sequence"
    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactCheckoutStep $checkoutStep $false `
        "$Context exact current-commit checkout"

    $mainStep = Get-Stage5IndentedBlock $job `
        '- name: Validate Stage 5 workflow contract' 6
    Assert-Stage5ExactPowerShellStepMetadata $mainStep $null '' `
        "$Context exact main contract invocation step"
    Assert-Stage5WorkflowExecutablePowerShellCall $mainStep `
        '$env:GITHUB_WORKSPACE/.github/workflows/Stage5WorkflowContract.ps1' `
        "$Context exact main contract invocation" `
        ([ordered]@{ SourceRoot = '"$env:GITHUB_WORKSPACE"' })
    Assert-Stage5ClosedPowerShellRunBlock $mainStep `
        '9777050A0125EE51CC4FB52D4088F6F6D93143233FB38B15C10CB5EF6F45EE7A' 1 `
        "$Context sealed main contract invocation"

    $selfTestStep = Get-Stage5IndentedBlock $job `
        '- name: Validate Stage 5 workflow contract self-test' 6
    Assert-Stage5ExactPowerShellStepMetadata $selfTestStep $null '' `
        "$Context exact contract self-test invocation step"
    Assert-Stage5WorkflowExecutablePowerShellCall $selfTestStep `
        '$env:GITHUB_WORKSPACE/.github/workflows/Stage5WorkflowContract.ps1' `
        "$Context exact contract self-test invocation" `
        ([ordered]@{ SelfTest = $null })
    Assert-Stage5ClosedPowerShellRunBlock $selfTestStep `
        'A17BA6BAAE07134AC9E3236A56CB57D2CB820FC64E82FB7F1216BB229842D898' 1 `
        "$Context sealed contract self-test invocation"

    $promotionSelfTest = Get-Stage5IndentedBlock $job `
        '- name: Validate weekly release promotion attestation self-test' 6
    Assert-Stage5ExactPowerShellStepMetadata $promotionSelfTest $null '' `
        "$Context exact promotion self-test invocation step"
    Assert-Stage5WorkflowExecutablePowerShellCall $promotionSelfTest `
        '$env:GITHUB_WORKSPACE/.github/workflows/Validate-Stage5WeeklyPromotionAttestation.ps1' `
        "$Context exact promotion self-test invocation" `
        ([ordered]@{ SelfTest = $null })
    Assert-Stage5ClosedPowerShellRunBlock $promotionSelfTest `
        '88808488C25D245DE0CAD614A1C0B8AD891519689E0D0A040E3E4FA1CB275A91' 1 `
        "$Context sealed promotion self-test invocation"
}

function Assert-Stage5CheckReplaysContract {
    param([string]$Content, [string]$Context)

    $job = Get-Stage5IndentedBlock $Content 'build:' 2
    Assert-Stage5ExactJobSchema $job ([ordered]@{
            name = '${{ inputs.preset }}'
            'runs-on' = 'windows-2022'
            'timeout-minutes' = '240'
            steps = ''
        }) "$Context exact reusable job schema"
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Enforce native Stage 5 replay contract',
        'Resolve paired artifact runtime directories',
        'Download Game Artifact',
        'Download paired Generals Artifact',
        'Download paired Zero Hour Artifact',
        'Provision immutable Stage 5 simulation qualification data',
        'Provision paired Generals qualification data',
        'Provision paired Zero Hour qualification data',
        'Audit Replay Worker Mode Propagation',
        'Run Stage 5 Installed-Runtime Replay Matrix',
        'Normalize Stage 5 Evidence Paths for Upload',
        'Upload Debug Log',
        'Upload Stage 5 Validation Evidence',
        'Clean Stage 5 validation volume') @(
        'uses', 'run', 'run', 'uses', 'uses', 'uses', 'run', 'run', 'run', 'run', 'run', 'run', 'uses', 'uses', 'run') `
        "$Context exact closed step sequence"

    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactConditionalActionStep $checkoutStep `
        'actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd' `
        ([ordered]@{ submodules = 'true' }) '' `
        "$Context exact checkout action"

    $enforceStep = Get-Stage5IndentedBlock $job `
        '- name: Enforce native Stage 5 replay contract' 6
    Assert-Stage5ExactPowerShellStepMetadata $enforceStep ([ordered]@{
            STAGE5_GAME = '${{ inputs.game }}'
            STAGE5_PRESET = '${{ inputs.preset }}'
            STAGE5_ENABLED = '${{ inputs.stage5 }}'
            STAGE5_REQUIRE_X64 = '${{ inputs.stage5_require_x64 }}'
        }) '' "$Context exact native-contract step"
    Assert-Stage5ClosedPowerShellRunBlock $enforceStep `
        '612400DEBD5C92A55EB1FB2F1CA66D54D57D41EA447FC4CFD501B04A356122EB' 1 `
        "$Context sealed native-contract program"

    $layoutStep = Get-Stage5IndentedBlock $job '- name: Resolve paired artifact runtime directories' 6
    Assert-Stage5ExactPowerShellStepMetadata $layoutStep ([ordered]@{
        STAGE5_GAME = '${{ inputs.game }}'
        STAGE5_ACCEPTANCE_MANIFEST = '${{ inputs.stage5_acceptance_manifest }}'
    }) '' "$Context paired runtime layout step"
    Assert-Stage5ClosedPowerShellRunBlock $layoutStep `
        'AC1B95BAAF174C522FF799C329DA62AAE0BFB87CACAF171F140BF708F003A109' 7 `
        "$Context sealed paired runtime layout program"
    $pairedDownloadStep = Get-Stage5IndentedBlock $job '- name: Download paired Generals Artifact' 6
    Assert-Stage5ExactConditionalActionStep $pairedDownloadStep `
        'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3' `
        ([ordered]@{ name = 'Generals-x64-generals-vcpkg-product+e'; path = '${{ env.STAGE5_GENERALS_RUNTIME_ROOT }}' }) `
        '${{ inputs.game == ''GeneralsMD'' }}' "$Context same-run paired Generals download"

    $downloadStep = Get-Stage5IndentedBlock $job `
        '- name: Download Game Artifact' 6
    $pairedZeroHourDownload = Get-Stage5IndentedBlock $job '- name: Download paired Zero Hour Artifact' 6
    Assert-Stage5ExactConditionalActionStep $pairedZeroHourDownload `
        'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3' `
        ([ordered]@{ name = 'GeneralsMD-x64-zerohour-vcpkg-product+e'; path = '${{ env.STAGE5_ZEROHOUR_RUNTIME_ROOT }}' }) `
        '${{ inputs.game == ''Generals'' }}' "$Context same-run paired Zero Hour download"
    Assert-Stage5ExactConditionalActionStep $downloadStep `
        'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3' `
        ([ordered]@{
            name = '${{ inputs.game }}-${{ inputs.preset }}'
            path = '${{ env.STAGE5_RUNTIME_ROOT }}'
        }) '' "$Context exact current-run product download"

    $provisionStep = Get-Stage5IndentedBlock $job `
        '- name: Provision immutable Stage 5 simulation qualification data' 6
    Assert-Stage5ExactPowerShellStepMetadata $provisionStep ([ordered]@{
            AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
            AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
            AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
            STAGE5_GAME = '${{ inputs.game }}'
        }) '' "$Context exact qualification-data provision step"
    Assert-Stage5WorkflowExecutablePowerShellCall $provisionStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5SimulationQualificationData.ps1' `
        "$Context exact qualification-data producer call" ([ordered]@{
            RuntimeRoot = '$runtimeRoot'
            TaskRoot = '$taskRoot'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            Title = '$title'
            AwsEndpointUrl = '$env:AWS_ENDPOINT_URL'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ValidationVolumeInvocation $provisionStep 'Provision' `
        '(?s)__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__' `
        "$Context validation-volume provision helper"
    Assert-Stage5ClosedPowerShellRunBlock $provisionStep `
        'BE1CE3D25F986DA77E47036C897CD00BF0D6867EDC457EDCC1DB4942B3FA7C08' 9 `
        "$Context sealed qualification-data provision program"

    $pairedProvision = Get-Stage5IndentedBlock $job '- name: Provision paired Generals qualification data' 6
    Assert-Stage5ExactPowerShellStepMetadata $pairedProvision ([ordered]@{
        AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
        AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
        AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    }) '${{ inputs.game == ''GeneralsMD'' }}' "$Context paired data provision step"
    Assert-Stage5WorkflowExecutablePowerShellCall $pairedProvision `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5SimulationQualificationData.ps1' `
        "$Context paired data producer call" ([ordered]@{
            RuntimeRoot = '$env:STAGE5_GENERALS_RUNTIME_ROOT'
            TaskRoot = '$taskRoot'
            ProvisioningRole = 'GeneralsBase'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            Title = "'Generals'"
            AwsEndpointUrl = '$env:AWS_ENDPOINT_URL'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ClosedPowerShellRunBlock $pairedProvision `
        '2D9EE962D9FF53FE544482DEDED88F98505FE556517A10DE535DBFEC70F15506' 4 `
        "$Context sealed paired data provision program"

    $pairedZeroHourProvision = Get-Stage5IndentedBlock $job '- name: Provision paired Zero Hour qualification data' 6
    Assert-Stage5ExactPowerShellStepMetadata $pairedZeroHourProvision ([ordered]@{
        AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
        AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
        AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    }) '${{ inputs.game == ''Generals'' }}' "$Context paired Zero Hour data step"
    Assert-Stage5WorkflowExecutablePowerShellCall $pairedZeroHourProvision `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5SimulationQualificationData.ps1' `
        "$Context paired Zero Hour data producer" ([ordered]@{
            RuntimeRoot = '$env:STAGE5_ZEROHOUR_RUNTIME_ROOT'
            TaskRoot = "'H:\Stage5SimulationValidationTask'"
            ProvisioningRole = 'ZeroHourBase'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            Title = "'ZeroHour'"
            AwsEndpointUrl = '$env:AWS_ENDPOINT_URL'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ClosedPowerShellRunBlock $pairedZeroHourProvision `
        '499203CFD3A08C33C88515E9435383AFDD98192ACE53F3BF07FD26A55066CA85' 3 `
        "$Context sealed paired Zero Hour data program"

    $auditStep = Get-Stage5IndentedBlock $job `
        '- name: Audit Replay Worker Mode Propagation' 6
    Assert-Stage5ExactPowerShellStepMetadata $auditStep $null '' `
        "$Context exact replay-mode audit step"
    Assert-Stage5WorkflowExecutablePowerShellCall $auditStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Audit-ReplayModePropagation.ps1' `
        "$Context exact replay-mode audit call" `
        ([ordered]@{ SourceRoot = '$env:GITHUB_WORKSPACE' })
    Assert-Stage5ClosedPowerShellRunBlock $auditStep `
        '22F3ED1CAF36A1B9DB1315C97027A15D2A2F79A7E97BA75BFF181C0DDEDF41EE' 2 `
        "$Context sealed replay-mode audit program"

    $matrixStep = Get-Stage5IndentedBlock $job `
        '- name: Run Stage 5 Installed-Runtime Replay Matrix' 6
    Assert-Stage5ExactPowerShellStepMetadata $matrixStep ([ordered]@{
            STAGE5_GAME = '${{ inputs.game }}'
            STAGE5_PRESET = '${{ inputs.preset }}'
            STAGE5_REQUIRE_X64 = '${{ inputs.stage5_require_x64 }}'
            STAGE5_REPLAY_MATRIX_REPEATS = '${{ inputs.stage5_replay_matrix_repeats }}'
            STAGE5_STRESS_REPEATS = '${{ inputs.stage5_stress_repeats }}'
            STAGE5_FIXTURE_MANIFEST = '${{ inputs.stage5_fixture_manifest }}'
            STAGE5_PERFORMANCE_BASELINE = '${{ inputs.stage5_performance_baseline }}'
            STAGE5_EXPECTED_STAGE3_EXECUTABLE_SHA256 = '${{ inputs.stage5_expected_stage3_executable_sha256 }}'
            STAGE5_ACCEPTANCE_MANIFEST = '${{ inputs.stage5_acceptance_manifest }}'
            STAGE5_EXECUTION_COHORT_NONCE = '${{ inputs.stage5_execution_cohort_nonce }}'
            STAGE5_EXECUTION_COHORT_CREATED_UTC = '${{ inputs.stage5_execution_cohort_created_utc }}'
        }) '${{ inputs.stage5 }}' "$Context exact replay-matrix step"
    Assert-Stage5RepositoryRelativeFileGuard $matrixStep `
        "$Context exact repository input resolver" ([ordered]@{
            '$requestedManifestPath' = @('$workspace',
                '$env:STAGE5_FIXTURE_MANIFEST', "'Stage 5 fixture manifest'")
            '$baselinePath' = @('$workspace',
                '$env:STAGE5_PERFORMANCE_BASELINE', "'Stage 5 performance baseline'")
            '$acceptancePath' = @('$workspace',
                '$env:STAGE5_ACCEPTANCE_MANIFEST', "'Stage 5 final acceptance manifest'")
            '$artifactSetPath' = @('$workspace', '$artifactSetRelative',
                "'Stage 5 artifact set'")
            '$replayEvidencePath' = @('$workspace', '$replayEvidenceRelative',
                "'Stage 5 replay-determinism evidence'")
            '$validatedManifestPath' = @('$workspace',
                '$validatedManifestRelative',
                "'Stage 5 protected reviewed manifest snapshot'")
        })
    Assert-Stage5WorkflowExecutablePowerShellCall $matrixStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Run-DeterministicSimulationValidation.ps1' `
        "$Context exact installed-runtime validation call" ([ordered]@{
            RuntimeRoot = '$runtimeRoot'
            FixtureManifestPath = '$validatedManifestPath'
            OutputRoot = '$outputRoot'
            TaskRoot = '$taskRoot'
            AllowHeadlessDirectExecution = $null
            RequireX64 = $null
            ValidationSet = "'All'"
            ReplayMatrixRepeats = '$env:STAGE5_REPLAY_MATRIX_REPEATS'
            StressRepeats = '$env:STAGE5_STRESS_REPEATS'
            ExpectedExecutableSha256 = '$candidateHash'
            Title = '$expectedTitle'
            GeneralsInstallRoot = '$generalsRuntimeRoot'
            AcceptanceArtifactSetPath = '$artifactSetPath'
            GeneralsQualificationDataManifestPath = '$env:STAGE5_GENERALS_QUALIFICATION_DATA_MANIFEST_PATH'
            GeneralsQualificationDataManifestSha256 = '$env:STAGE5_GENERALS_QUALIFICATION_DATA_MANIFEST_SHA256'
            MinimumFreeBytes = '2147483648'
            EnforcePerformance = $null
            Stage3PerformanceBaselinePath = '$baselinePath'
            ExpectedStage3ExecutableSha256 = '$expectedStage3Hash'
            AcceptanceSourceCommit = '$acceptanceSourceCommit'
            AcceptanceArtifactSetSha256 = '$artifactSetHash'
            ExecutionCohortNonce = '$executionCohortNonce'
            ExecutionCohortCreatedUtc = '$executionCohortCreatedUtc'
            AcceptanceRuntimeDependencyManifestSha256 = '$expectedRuntimeClosure.dependencyManifestSha256'
            AcceptanceRuntimeClosureSha256 = '$expectedRuntimeClosure.closureSha256'
            QualificationDataManifestPath = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH'
            QualificationDataManifestSha256 = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256'
            QualificationDataClosureSha256 = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_CLOSURE_SHA256'
            QualificationDataFileCount = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_FILE_COUNT'
        })
    Assert-Stage5JsonIntegerSchemaVersionGuards $matrixStep 2 `
        "$Context strict replay-matrix schemaVersion gates"
    Assert-Stage5CheckReplaysCardinalityAndArrayContracts $matrixStep `
        "$Context replay-matrix JSON array cardinality"
    Assert-Stage5ClosedPowerShellRunBlock $matrixStep `
        '682211EE77ACC6497072FBA9EF067446A1F13591A61356F0FA3F2A9AC816D4FD' 107 `
        "$Context sealed installed-runtime validation program"

    $normalizerStep = Get-Stage5IndentedBlock $job `
        '- name: Normalize Stage 5 Evidence Paths for Upload' 6
    Assert-Stage5ExactPowerShellStepMetadata $normalizerStep ([ordered]@{
            STAGE5_UPLOAD_ROOT = '${{ runner.temp }}\Stage5SimulationValidation-upload'
        }) '${{ always() && inputs.stage5 }}' `
        "$Context exact evidence normalization step"
    Assert-Stage5WorkflowExecutablePowerShellCall $normalizerStep `
        '$env:GITHUB_WORKSPACE/.github/workflows/Normalize-Stage5EvidenceForUpload.ps1' `
        "$Context exact evidence normalization call" ([ordered]@{
            InputRoot = "'H:\Stage5SimulationValidationTask\Evidence'"
            OutputRoot = '$env:STAGE5_UPLOAD_ROOT'
        })
    Assert-Stage5ClosedPowerShellRunBlock $normalizerStep `
        '6986DFD5D7B3A2DBA637DD6001E6F06F30895D36FECB483B822FED2D4A08B37F' 2 `
        "$Context sealed evidence normalization program"

    $debugUpload = Get-Stage5IndentedBlock $job '- name: Upload Debug Log' 6
    Assert-Stage5ExactConditionalActionStep $debugUpload `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Replay-Debug-Log-${{ inputs.preset }}'
            path = '${{ env.STAGE5_RUNTIME_ROOT }}/DebugLogFile*.txt'
            'retention-days' = '30'
            'if-no-files-found' = 'ignore'
        }) 'always()' "$Context exact debug-log upload"

    $evidenceUpload = Get-Stage5IndentedBlock $job `
        '- name: Upload Stage 5 Validation Evidence' 6
    Assert-Stage5ExactConditionalActionStep $evidenceUpload `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Stage5-Simulation-Validation-${{ inputs.game }}-${{ inputs.preset }}'
            path = '${{ runner.temp }}\Stage5SimulationValidation-upload'
            'retention-days' = '30'
            'if-no-files-found' = 'error'
        }) '${{ always() && inputs.stage5 }}' `
        "$Context exact validation-evidence upload"
    $cleanupStep = Get-Stage5IndentedBlock $job `
        '- name: Clean Stage 5 validation volume' 6
    Assert-Stage5ValidationVolumeInvocation $cleanupStep 'Cleanup' `
        '(?s)__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__' `
        "$Context validation-volume cleanup helper"
    $cleanupIndex = $job.IndexOf(
        '- name: Clean Stage 5 validation volume', [StringComparison]::Ordinal)
    $evidenceUploadIndex = $job.IndexOf(
        '- name: Upload Stage 5 Validation Evidence', [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupIndex -gt $evidenceUploadIndex) `
        "$Context must clean the validation volume after evidence upload."
}

function Invoke-Stage5CheckReplaysContractSelfTest {
    $path = Join-Path $PSScriptRoot 'check-replays.yml'
    Assert-Stage5WorkflowCondition (Test-Path -LiteralPath $path -PathType Leaf) `
        'check-replays workflow contract self-test fixture is missing.'
    $fixture = (Get-Content -LiteralPath $path -Raw) -replace "`r`n|`r", "`n"
    Assert-Stage5CanonicalDownloadArtifactPins $fixture 3 `
        'self-test reusable Stage 5 replay artifact download assembly'
    Assert-Stage5CheckReplaysContract $fixture `
        'self-test reusable Stage 5 replay workflow fixture'

    $swap = $fixture.Replace(
        '      - name: Checkout Code',
        '      - name: __STAGE5_SWAP__').Replace(
        '      - name: Download Game Artifact',
        '      - name: Checkout Code').Replace(
        '      - name: __STAGE5_SWAP__',
        '      - name: Download Game Artifact')
    $mutations = [ordered]@{
        comment = $fixture.Replace(
            '          $workspace = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE).TrimEnd(''\'', ''/'')',
            "          # hidden workflow mutation`n          `$workspace = [IO.Path]::GetFullPath(`$env:GITHUB_WORKSPACE).TrimEnd('\', '/')")
        'quoted-key' = $fixture.Replace(
            '    runs-on: windows-2022', "    'runs-on': windows-2022")
        action = $fixture.Replace(
            'actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3',
            'actions/download-artifact@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
        'validation-volume-token' = $fixture.Replace(
            '-Token "${{ github.run_id }}-${{ github.run_attempt }}-${{ inputs.game }}-${{ inputs.preset }}"',
            '-Token "${{ github.run_id }}-${{ github.run_attempt }}-${{ inputs.game }}"')
        args = $fixture.Replace("-ValidationSet 'All'", "-ValidationSet 'Replay'")
        swap = $swap
        duplicate = $fixture.Replace(
            '    timeout-minutes: 240',
            "    timeout-minutes: 240`n    timeout-minutes: 240")
        'duplicate-job' = $fixture.Replace(
            '  build:', "  build:`n  build:")
        'action-input-order' = $fixture.Replace(
            ('          name: ${{ inputs.game }}-${{ inputs.preset }}' +
                "`n" +
                '          path: ${{ env.STAGE5_RUNTIME_ROOT }}'),
            ('          path: ${{ env.STAGE5_RUNTIME_ROOT }}' + "`n" +
                '          name: ${{ inputs.game }}-${{ inputs.preset }}'))
        tamper = $fixture.Replace(
            "-Role 'replay-fixture-manifest'", "-Role 'replay-results'")
        'missing-attachment-title' = $fixture.Replace(
            "@('role', 'title', 'path', 'sha256',", "@('role', 'path', 'sha256',")
        'swapped-attachment-title' = $fixture.Replace(
            '$receiptTitle -cne $attachmentTitle',
            '$receiptTitle -ceq $attachmentTitle')
        'wrong-outer-title' = $fixture.Replace(
            "'Stage 5 replay-determinism evidence') -cne 'Both'",
            "'Stage 5 replay-determinism evidence') -cne `$expectedTitle")
        'rooted-resolution' = $fixture.Replace(
            '$artifactSetPath = Resolve-Stage5RepositoryFile $workspace',
            '$artifactSetPath = Resolve-Stage5RepositoryFile $artifactSetPath')
        'reparse-guard' = $fixture.Replace(
            '[IO.FileAttributes]::ReparsePoint', '[IO.FileAttributes]::Hidden')
    }
    foreach ($name in @($mutations.Keys)) {
        $invalid = [string]$mutations[$name]
        Assert-Stage5WorkflowCondition ($invalid -cne $fixture) `
            "check-replays workflow contract self-test mutation '$name' did not apply."
        $caught = $false
        try {
            Assert-Stage5CheckReplaysContract $invalid `
                "self-test invalid reusable Stage 5 replay workflow '$name'"
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            "check-replays workflow contract self-test accepted '$name' mutation."
    }
}

function Assert-Stage5WorkflowRunBlockIndentation {
    param([string]$Block, [int]$StepIndent, [int]$RunIndent, [string]$Context)

    $lines = @($Block -split '\r?\n')
    $runPrefix = ((' ' * $RunIndent) -join '') + 'run: |'
    $runIndex = -1
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index].TrimEnd() -ceq $runPrefix) {
            $runIndex = $index
            break
        }
    }
    Assert-Stage5WorkflowCondition ($runIndex -ge 0) `
        "$Context is missing its run block marker."

    $bodyLineCount = 0
    for ($index = $runIndex + 1; $index -lt $lines.Count; ++$index) {
        $line = $lines[$index]
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $lineIndent = $line.Length - $line.TrimStart().Length
        if ($lineIndent -le $StepIndent) {
            break
        }
        if ($line.TrimStart().StartsWith('#')) {
            continue
        }
        Assert-Stage5WorkflowCondition ($lineIndent -gt $RunIndent) `
            "$Context contains a non-comment run-body line with indentation $lineIndent; expected greater than $RunIndent."
        ++$bodyLineCount
    }
    Assert-Stage5WorkflowCondition ($bodyLineCount -gt 0) `
        "$Context does not contain any non-comment run-body lines."
}

function Assert-Stage5ExecutionCohortProducer {
    param([string]$Content, [string]$Context)

    $job = Get-Stage5IndentedBlock $Content 'stage5-execution-cohort:' 2
    Assert-Stage5WorkflowLiteral $job 'needs: detect-changes' `
        "$Context change-detector dependency"
    $condition = Get-Stage5IndentedBlock $job 'if: >-' 4
    Assert-Stage5ExactFoldedJobCondition $condition `
        ("github.event_name == 'workflow_dispatch' && " +
            '(inputs.stage5_lockstep_v2_qualification == true || ' +
            'inputs.stage5_external_performance_qualification == true || ' +
            "(inputs.stage5_acceptance_manifest != '' && " +
            "(inputs.stage5_fixture_manifest != '' || " +
            "inputs.stage5_generals_fixture_manifest != '')))" ) `
        "$Context manual external qualification trigger"
    $outputs = Get-Stage5IndentedBlock $job 'outputs:' 4
    $normalizedOutputs = (@($outputs -split '\r?\n' |
        ForEach-Object { $_.Trim() }) -join ' ')
    Assert-Stage5WorkflowCondition ($normalizedOutputs -ceq
        'outputs: nonce: ${{ steps.mint.outputs.nonce }} created_utc: ${{ steps.mint.outputs.created_utc }}') `
        "$Context must expose only the exact fresh mint outputs."
    $mintStep = Get-Stage5IndentedBlock $job `
        '- name: Mint fresh execution cohort' 6
    Assert-Stage5WorkflowFailClosedStep $mintStep `
        "$Context unconditional cohort mint"
    Assert-Stage5WorkflowContains $mintStep '(?m)^ {8}id:\s*mint\s*$' `
        "$Context exact cohort producer id"
    Assert-Stage5WorkflowContains $mintStep '(?m)^ {8}shell:\s*pwsh\s*$' `
        "$Context exact cohort producer shell"
    Assert-Stage5WorkflowCondition `
        ([regex]::Matches($job, '(?m)^ {8}id:\s*mint\s*$').Count -eq 1) `
        "$Context must define one mint step id."
    Assert-Stage5WorkflowRunBlockIndentation $mintStep 6 8 `
        "$Context executable cohort mint"
    $effectiveMintStep = Get-Stage5WorkflowEffectiveContent $mintStep
    Assert-Stage5ExactTopLevelPowerShellStatements $mintStep @(
        "`$nonce = [Guid]::NewGuid().ToString('D').ToLowerInvariant()",
        "`$created = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)",
        '"nonce=$nonce" >> $env:GITHUB_OUTPUT',
        '"created_utc=$created" >> $env:GITHUB_OUTPUT') `
        "$Context exact fresh cohort mint"
    foreach ($mintBinding in @(
        "[Guid]::NewGuid().ToString('D').ToLowerInvariant()",
        "[DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)",
        '"nonce=$nonce" >> $env:GITHUB_OUTPUT',
        '"created_utc=$created" >> $env:GITHUB_OUTPUT')) {
        Assert-Stage5WorkflowLiteral $effectiveMintStep $mintBinding `
            "$Context canonical shared identity producer"
    }
    Assert-Stage5WorkflowNotContains $effectiveMintStep `
        '\[DateTimeOffset\]::UtcNow|ToString\(\)' `
        "$Context noncanonical UUID or UTC producer"
}

function Assert-Stage5CombinedHostRunnerProducer {
    param([string]$Content, [string]$Context)

    $job = Get-Stage5IndentedBlock $Content 'stage5-combined-host-runner:' 2
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Provision isolated Stage 5 volume',
        'Download Generals Stage 5 evidence',
        'Download Zero Hour Stage 5 evidence',
        'Produce combined host-runner receipt',
        'Upload combined Stage 5 evidence',
        'Clean Stage 5 combined host-runner validation volume') @(
        'uses', 'run', 'uses', 'uses', 'run', 'uses', 'run') `
        "$Context exact closed step sequence"
    Assert-Stage5NoJobRunDefaults $job "$Context job metadata"
    Assert-Stage5ExactJobEnvironment $job ([ordered]@{
            STAGE5_ACCEPTANCE_MANIFEST = '${{ inputs.stage5_acceptance_manifest }}'
            STAGE5_COHORT_NONCE = '${{ needs[''stage5-execution-cohort''].outputs.nonce }}'
            STAGE5_COHORT_CREATED_UTC = '${{ needs[''stage5-execution-cohort''].outputs.created_utc }}'
        }) "$Context exact job identity"
    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactCheckoutStep $checkoutStep $false `
        "$Context trusted current-commit checkout"
    $condition = Get-Stage5IndentedBlock $job 'if: >-' 4
    Assert-Stage5ExactFoldedJobCondition $condition `
        ("always() && github.event_name == 'workflow_dispatch' && " +
            "inputs.stage5_fixture_manifest != '' && " +
            "inputs.stage5_generals_fixture_manifest != '' && " +
            "inputs.stage5_acceptance_manifest != '' && " +
            "needs['stage5-execution-cohort'].result == 'success' && " +
            "needs['stage5-replaycheck-generalsmd-x64'].result == 'success' && " +
            "needs['stage5-replaycheck-generals-x64'].result == 'success'") `
        "$Context complete current-run dependency gate"
    foreach ($downloadSpec in @(
        [pscustomobject]@{
            step = '- name: Download Generals Stage 5 evidence'
            name = 'Stage5-Simulation-Validation-Generals-x64-generals-vcpkg-product+e'
            path = '${{ runner.temp }}\Stage5CombinedSources\Generals'
        },
        [pscustomobject]@{
            step = '- name: Download Zero Hour Stage 5 evidence'
            name = 'Stage5-Simulation-Validation-GeneralsMD-x64-zerohour-vcpkg-product+e'
            path = '${{ runner.temp }}\Stage5CombinedSources\ZeroHour'
        })) {
        $downloadStep = Get-Stage5IndentedBlock $job $downloadSpec.step 6
        Assert-Stage5CurrentRunArtifactDownload $downloadStep `
            $downloadSpec.name $downloadSpec.path `
            "$Context exact current-run evidence input"
    }
    $provisionStep = Get-Stage5IndentedBlock $job `
        '- name: Provision isolated Stage 5 volume' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $provisionStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact isolated-volume step metadata"
    Assert-Stage5WorkflowFailClosedStep $provisionStep `
        "$Context unconditional isolated-volume provisioning"
    Assert-Stage5ValidationVolumeInvocation $provisionStep 'Provision' `
        '(?s)combined-host-runner.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume provision helper"
    Assert-Stage5ClosedPowerShellRunBlock $provisionStep `
        '67DE779213B539E2CAAAFC2BBFC1C8F9E06539CF78AAC56BA71143B3C1BA1901' 3 `
        "$Context exact isolated-volume provisioning program"
    $producerStep = Get-Stage5IndentedBlock $job `
        '- name: Produce combined host-runner receipt' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $producerStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact combined-producer step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $producerStep 6 8 `
        "$Context executable combined producer"
    Assert-Stage5RepositoryRelativeFileGuard $producerStep `
        "$Context acceptance authority containment" `
        ([ordered]@{
            '$acceptancePath' = @('$workspace',
                '$env:STAGE5_ACCEPTANCE_MANIFEST',
                "'Stage 5 acceptance manifest'")
            '$artifactPath' = @('(Split-Path -Parent $acceptancePath)',
                '([string]$acceptance.artifactSet.path)',
                "'Stage 5 acceptance artifact-set manifest'")
            '$replayEnvelopePath' = @('(Split-Path -Parent $acceptancePath)',
                '([string]$replayEntries[0].path)',
                "'Stage 5 replay-determinism envelope'")
            '$generalsReviewedFixtureReceipt' = @(
                '(Split-Path -Parent $replayEnvelopePath)',
                '([string]$generalsReviewedAttachments[0].path)',
                "'Generals protected reviewed-fixture receipt'")
            '$zeroHourReviewedFixtureReceipt' = @(
                '(Split-Path -Parent $replayEnvelopePath)',
                '([string]$zeroHourReviewedAttachments[0].path)',
                "'Zero Hour protected reviewed-fixture receipt'")
        })
    Assert-Stage5WorkflowExecutablePowerShellCall $producerStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5CombinedHostRunnerReceipt.ps1' `
        "$Context exact combined receipt producer" `
        ([ordered]@{
            GeneralsReceiptPath = '$generalsReceipts[0].FullName'
            ZeroHourReceiptPath = '$zeroHourReceipts[0].FullName'
            GeneralsReviewedFixtureReceiptPath = '$generalsReviewedFixtureReceipt'
            GeneralsReviewedFixtureReceiptSha256 = '$generalsReviewedFixtureReceiptSha256'
            ZeroHourReviewedFixtureReceiptPath = '$zeroHourReviewedFixtureReceipt'
            ZeroHourReviewedFixtureReceiptSha256 = '$zeroHourReviewedFixtureReceiptSha256'
            OutputPath = '$combinedOutput'
            ExpectedSourceCommit = '$sourceCommit'
            ExpectedArtifactSetSha256 = '$artifactHash'
            ExpectedGeneralsExecutableSha256 = "([string]`$artifactByRole['generals-executable'])"
            ExpectedZeroHourExecutableSha256 = "([string]`$artifactByRole['zerohour-executable'])"
            ExpectedCohortNonce = '$env:STAGE5_COHORT_NONCE'
            ExpectedCohortCreatedUtc = '$env:STAGE5_COHORT_CREATED_UTC'
        })
    Assert-Stage5WorkflowFailClosedStep $producerStep `
        "$Context unconditional combined receipt producer"
    Assert-Stage5ClosedPowerShellRunBlock $producerStep `
        '94C59D91A4FDCF6E789DD4DF8391C209A550B79DB24BCEA6E863334EA65CB115' 35 `
        "$Context exact closed combined receipt program"
    $uploadStep = Get-Stage5IndentedBlock $job `
        '- name: Upload combined Stage 5 evidence' 6
    Assert-Stage5ExactActionStep $uploadStep `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Stage5-Simulation-Validation-combined'
            path = 'H:\Stage5CombinedSimulationTask\Evidence'
            'retention-days' = '30'
            'if-no-files-found' = 'error'
        }) "$Context exact combined evidence upload"
    $cleanupStep = Get-Stage5IndentedBlock $job `
        '- name: Clean Stage 5 combined host-runner validation volume' 6
    Assert-Stage5ValidationVolumeInvocation $cleanupStep 'Cleanup' `
        '(?s)combined-host-runner.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume cleanup helper"
    $cleanupIndex = $job.IndexOf(
        '- name: Clean Stage 5 combined host-runner validation volume',
        [StringComparison]::Ordinal)
    $uploadIndex = $job.IndexOf(
        '- name: Upload combined Stage 5 evidence', [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupIndex -gt $uploadIndex) `
        "$Context must clean the validation volume after evidence upload."
}

function Assert-Stage5LockstepV2QualificationProducer {
    param([string]$Content, [string]$Context)

    $enableInput = Get-Stage5IndentedBlock $Content `
        'stage5_lockstep_v2_qualification:' 6
    Assert-Stage5WorkflowContains $enableInput '(?m)^\s*type:\s*boolean\s*$' `
        "$Context explicit qualification input"
    Assert-Stage5WorkflowContains $enableInput '(?m)^\s*default:\s*false\s*$' `
        "$Context default-disabled qualification input"
    foreach ($inputName in @(
        'stage5_lockstep_v2_map_name:',
        'stage5_lockstep_v2_generals_map_crc:',
        'stage5_lockstep_v2_zerohour_map_crc:')) {
        Assert-Stage5WorkflowLiteral $Content $inputName `
            "$Context reviewed qualification input"
    }

    $job = Get-Stage5IndentedBlock $Content `
        'stage5-lockstep-v2-qualification:' 2
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Provision canonical Stage 5 qualification root',
        'Download exact Generals x64 product',
        'Download exact Zero Hour x64 product',
        'Produce exact Stage 5 runtime manifests',
        'Provision verified trimmed qualification data',
        'Run installed lockstep-v2 qualification',
        'Upload installed lockstep-v2 qualification',
        'Clean Stage 5 lockstep-v2 validation volume') @(
        'uses', 'run', 'uses', 'uses', 'run', 'run', 'run', 'uses', 'run') `
        "$Context exact closed step sequence"
    Assert-Stage5NoJobRunDefaults $job "$Context job metadata"
    Assert-Stage5ExactJobEnvironment $job ([ordered]@{
            STAGE5_QUALIFICATION_ROOT = "'H:\Stage5WeeklyPromotionQualification'"
            STAGE5_LOCKSTEP_V2_MAP_NAME = '${{ inputs.stage5_lockstep_v2_map_name }}'
            STAGE5_LOCKSTEP_V2_GENERALS_MAP_CRC = '${{ inputs.stage5_lockstep_v2_generals_map_crc }}'
            STAGE5_LOCKSTEP_V2_ZEROHOUR_MAP_CRC = '${{ inputs.stage5_lockstep_v2_zerohour_map_crc }}'
            STAGE5_COHORT_NONCE = '${{ needs[''stage5-execution-cohort''].outputs.nonce }}'
            STAGE5_COHORT_CREATED_UTC = '${{ needs[''stage5-execution-cohort''].outputs.created_utc }}'
        }) "$Context exact job identity"
    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactCheckoutStep $checkoutStep $false `
        "$Context trusted current-commit checkout"
    Assert-Stage5WorkflowLiteral $job `
        'needs: [build-generals-x64, build-generalsmd-x64, stage5-execution-cohort]' `
        "$Context exact x64 product and shared-cohort dependencies"
    $condition = Get-Stage5IndentedBlock $job 'if: >-' 4
    Assert-Stage5ExactFoldedJobCondition $condition `
        ("github.event_name == 'workflow_dispatch' && " +
            'inputs.stage5_lockstep_v2_qualification == true') `
        "$Context manual explicit opt-in gate"
    Assert-Stage5WorkflowLiteral $job `
        "STAGE5_QUALIFICATION_ROOT: 'H:\Stage5WeeklyPromotionQualification'" `
        "$Context canonical qualification root"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_COHORT_NONCE: ${{ needs[''stage5-execution-cohort''].outputs.nonce }}' `
        "$Context shared cohort nonce"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_COHORT_CREATED_UTC: ${{ needs[''stage5-execution-cohort''].outputs.created_utc }}' `
        "$Context shared cohort timestamp"
    Assert-Stage5CanonicalDownloadArtifactPins $job 2 `
        "$Context exact product downloads"
    foreach ($downloadSpec in @(
        [pscustomobject]@{
            step = '- name: Download exact Generals x64 product'
            name = 'Generals-x64-generals-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\GeneralsRuntime'
        },
        [pscustomobject]@{
            step = '- name: Download exact Zero Hour x64 product'
            name = 'GeneralsMD-x64-zerohour-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime'
        })) {
        $downloadStep = Get-Stage5IndentedBlock $job $downloadSpec.step 6
        Assert-Stage5CurrentRunArtifactDownload $downloadStep `
            $downloadSpec.name $downloadSpec.path `
            "$Context exact product artifact binding"
    }

    $provisionStep = Get-Stage5IndentedBlock $job `
        '- name: Provision canonical Stage 5 qualification root' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $provisionStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact canonical-root step metadata"
    Assert-Stage5WorkflowFailClosedStep $provisionStep `
        "$Context unconditional canonical-root provisioning"
    Assert-Stage5NoStepEnvironment $provisionStep `
        "$Context inherited canonical-root identity"
    Assert-Stage5ValidationVolumeInvocation $provisionStep 'Provision' `
        '(?s)lockstep-v2.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume provision helper"
    Assert-Stage5ClosedPowerShellRunBlock $provisionStep `
        '4F9DBE7162E2C8B829FBD34115590CC44875A27B71A564EE6B761110D19000CC' 5 `
        "$Context exact canonical-root provisioning program"

    $manifestStep = Get-Stage5IndentedBlock $job `
        '- name: Produce exact Stage 5 runtime manifests' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $manifestStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact runtime-manifest step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $manifestStep 6 8 `
        "$Context runtime-manifest producer"
    Assert-Stage5WorkflowFailClosedStep $manifestStep `
        "$Context unconditional runtime-manifest producer"
    Assert-Stage5NoStepEnvironment $manifestStep `
        "$Context inherited runtime-manifest identity"
    Assert-Stage5WorkflowExecutablePowerShellCall $manifestStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5RuntimeManifests.ps1' `
        "$Context exact trusted runtime-manifest producer" `
        ([ordered]@{
            QualificationRoot = '$env:STAGE5_QUALIFICATION_ROOT'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ClosedPowerShellRunBlock $manifestStep `
        'A25E2DCBC08FBC9C781D4453A44578468FCDA551405AAE3ED353C1D64C9216D6' 3 `
        "$Context exact closed runtime-manifest program"

    $qualificationDataStep = Get-Stage5IndentedBlock $job `
        '- name: Provision verified trimmed qualification data' 6
    $qualificationDataEnvironment = [ordered]@{
        AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
        AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
        AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    }
    Assert-Stage5ExactPowerShellStepMetadata -Step $qualificationDataStep `
        -ExpectedEnvironment $qualificationDataEnvironment -Context `
        "$Context exact qualification-data step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $qualificationDataStep 6 8 `
        "$Context qualification-data producer"
    Assert-Stage5WorkflowFailClosedStep $qualificationDataStep `
        "$Context unconditional qualification-data producer"
    Assert-Stage5ExactStepEnvironment $qualificationDataStep `
        $qualificationDataEnvironment "$Context exact qualification-data credentials"
    Assert-Stage5WorkflowExecutablePowerShellCall $qualificationDataStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5QualificationData.ps1' `
        "$Context exact trusted qualification-data producer" `
        ([ordered]@{
            QualificationRoot = '$env:STAGE5_QUALIFICATION_ROOT'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            MapName = '$env:STAGE5_LOCKSTEP_V2_MAP_NAME'
            GeneralsMapCrc = '$generalsMapCrc'
            ZeroHourMapCrc = '$zeroHourMapCrc'
            AwsEndpointUrl = '$env:AWS_ENDPOINT_URL'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ClosedPowerShellRunBlock $qualificationDataStep `
        '42BCA4E400F5B76959B2889A33E8AC1676DF25811FB87C646B31C845BB0ED24F' 6 `
        "$Context exact closed qualification-data program"

    $qualificationStep = Get-Stage5IndentedBlock $job `
        '- name: Run installed lockstep-v2 qualification' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $qualificationStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact installed-qualifier step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $qualificationStep 6 8 `
        "$Context installed lockstep-v2 invocation"
    Assert-Stage5WorkflowFailClosedStep $qualificationStep `
        "$Context unconditional installed lockstep-v2 qualifier"
    Assert-Stage5NoStepEnvironment $qualificationStep `
        "$Context inherited installed qualifier identity"
    $effectiveQualificationStep = Get-Stage5WorkflowEffectiveContent `
        $qualificationStep
    Assert-Stage5WorkflowExecutablePowerShellCall $qualificationStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-InstalledLockstepV2Validation.ps1' `
        "$Context scoped executable installed qualifier" `
        ([ordered]@{
            GeneralsExecutable = '$env:STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE'
            ZeroHourExecutable = '$env:STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE'
            ArtifactSetManifestPath = '"$env:STAGE5_QUALIFICATION_ROOT/Stage5ArtifactSet.json"'
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            OutputDirectory = '"$env:STAGE5_QUALIFICATION_ROOT/Evidence"'
            MapName = '$env:STAGE5_LOCKSTEP_V2_MAP_NAME'
            GeneralsMapCrc = '$generalsMapCrc'
            ZeroHourMapCrc = '$zeroHourMapCrc'
            PeerCount = '2'
            Seed = '23063'
            ExecutionCohortNonce = '$env:STAGE5_COHORT_NONCE'
            ExecutionCohortCreatedUtc = '$env:STAGE5_COHORT_CREATED_UTC'
            RuntimeClosureDependencyManifestSha256 = '$env:STAGE5_LOCKSTEP_V2_RUNTIME_MANIFEST_SHA256'
            RuntimeClosureSha256 = '$env:STAGE5_LOCKSTEP_V2_RUNTIME_CLOSURE_SHA256'
            QualificationDataManifestPath = '"$env:STAGE5_QUALIFICATION_ROOT/Stage5QualificationData.json"'
            QualificationDataManifestSha256 = '$env:STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256'
            QualificationDataClosureSha256 = '$env:STAGE5_LOCKSTEP_V2_DATA_CLOSURE_SHA256'
            AllowHeadlessDirectExecution = $null
        })
    foreach ($qualificationBinding in @(
        '& "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-InstalledLockstepV2Validation.ps1"',
        '-GeneralsExecutable $env:STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE',
        '-ZeroHourExecutable $env:STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE',
        '-ArtifactSetManifestPath "$env:STAGE5_QUALIFICATION_ROOT/Stage5ArtifactSet.json"',
        '-SourceCommit $env:GITHUB_SHA.ToLowerInvariant()',
        '-OutputDirectory "$env:STAGE5_QUALIFICATION_ROOT/Evidence"',
        '-MapName $env:STAGE5_LOCKSTEP_V2_MAP_NAME',
        '-GeneralsMapCrc $generalsMapCrc',
        '-ZeroHourMapCrc $zeroHourMapCrc',
        '-PeerCount 2',
        '-Seed 23063',
        '-ExecutionCohortNonce $env:STAGE5_COHORT_NONCE',
        '-ExecutionCohortCreatedUtc $env:STAGE5_COHORT_CREATED_UTC',
        '-RuntimeClosureDependencyManifestSha256 $env:STAGE5_LOCKSTEP_V2_RUNTIME_MANIFEST_SHA256',
        '-RuntimeClosureSha256 $env:STAGE5_LOCKSTEP_V2_RUNTIME_CLOSURE_SHA256',
        '-QualificationDataManifestPath "$env:STAGE5_QUALIFICATION_ROOT/Stage5QualificationData.json"',
        '-QualificationDataManifestSha256 $env:STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256',
        '-QualificationDataClosureSha256 $env:STAGE5_LOCKSTEP_V2_DATA_CLOSURE_SHA256',
        '-AllowHeadlessDirectExecution')) {
        Assert-Stage5WorkflowLiteral $effectiveQualificationStep `
            $qualificationBinding "$Context executable installed qualifier"
    }
    Assert-Stage5ClosedPowerShellRunBlock $qualificationStep `
        '4ECFB923D3329B77819F6EF381942F2F35D3079F786D3027FF9D6D245D7EAE86' 7 `
        "$Context exact closed installed qualification program"

    $uploadStep = Get-Stage5IndentedBlock $job `
        '- name: Upload installed lockstep-v2 qualification' 6
    Assert-Stage5ExactActionStep $uploadStep `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Stage5-LockstepV2-Qualification'
            path = '|'
            'retention-days' = '30'
            'if-no-files-found' = 'error'
        }) "$Context exact lockstep qualification upload"
    Assert-Stage5WorkflowFailClosedStep $uploadStep `
        "$Context unconditional qualification upload"
    Assert-Stage5NoStepEnvironment $uploadStep `
        "$Context inherited qualification upload identity"
    $effectiveUploadStep = Get-Stage5WorkflowEffectiveContent $uploadStep
    Assert-Stage5CanonicalUploadArtifactPins $effectiveUploadStep 1 `
        "$Context qualification upload"
    Assert-Stage5WorkflowLiteral $effectiveUploadStep `
        'name: Stage5-LockstepV2-Qualification' `
        "$Context exact qualification artifact name"
    $requiredUploadPaths = @(
        'H:\Stage5WeeklyPromotionQualification\Stage5ArtifactSet.json',
        'H:\Stage5WeeklyPromotionQualification\Stage5RuntimeDependencies.json',
        'H:\Stage5WeeklyPromotionQualification\Stage5QualificationData.json',
        'H:\Stage5WeeklyPromotionQualification\Evidence')
    Assert-Stage5ExactLiteralActionInputLines $uploadStep 'path' $requiredUploadPaths `
        "$Context exact lockstep upload payload"
    $uploadPathEntries = [regex]::Matches($effectiveUploadStep,
        '(?m)^\s{12}(?<Path>H:\\Stage5WeeklyPromotionQualification\\[^\r\n]+)\s*$')
    Assert-Stage5WorkflowCondition ($uploadPathEntries.Count -eq
        $requiredUploadPaths.Count) `
        "$Context qualification artifact must contain exactly the three manifests and Evidence subtree."
    foreach ($uploadedPath in $requiredUploadPaths) {
        Assert-Stage5WorkflowLiteral $effectiveUploadStep $uploadedPath `
            "$Context complete qualification upload"
    }
    Assert-Stage5WorkflowLiteral $effectiveUploadStep `
        'if-no-files-found: error' "$Context fail-closed qualification upload"
    Assert-Stage5WorkflowLiteral $effectiveUploadStep `
        'retention-days: 30' "$Context qualification evidence retention"
    Assert-Stage5WorkflowNotContains $effectiveUploadStep `
        'H:\\Stage5WeeklyPromotionQualification\\(?:GeneralsRuntime|ZeroHourRuntime|QualificationData)(?:\\|\s|$)' `
        "$Context attestation-only qualification upload"
    $cleanupStep = Get-Stage5IndentedBlock $job `
        '- name: Clean Stage 5 lockstep-v2 validation volume' 6
    Assert-Stage5ValidationVolumeInvocation $cleanupStep 'Cleanup' `
        '(?s)lockstep-v2.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume cleanup helper"

    $manifestIndex = $job.IndexOf(
        '- name: Produce exact Stage 5 runtime manifests',
        [StringComparison]::Ordinal)
    $qualificationIndex = $job.IndexOf(
        '- name: Run installed lockstep-v2 qualification',
        [StringComparison]::Ordinal)
    $qualificationDataIndex = $job.IndexOf(
        '- name: Provision verified trimmed qualification data',
        [StringComparison]::Ordinal)
    $uploadIndex = $job.IndexOf(
        '- name: Upload installed lockstep-v2 qualification',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($manifestIndex -ge 0 -and
        $qualificationDataIndex -gt $manifestIndex -and
        $qualificationIndex -gt $qualificationDataIndex -and
        $uploadIndex -gt $qualificationIndex) `
        "$Context must produce runtime/data manifests, qualify, and only then upload evidence."
    $cleanupIndex = $job.IndexOf(
        '- name: Clean Stage 5 lockstep-v2 validation volume',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupIndex -gt $uploadIndex) `
        "$Context must clean the validation volume after evidence upload."
}

function Assert-Stage5ExternalPerformanceQualificationProducer {
    param([string]$Content, [string]$Context)

    $enableInput = Get-Stage5IndentedBlock $Content `
        'stage5_external_performance_qualification:' 6
    Assert-Stage5WorkflowContains $enableInput '(?m)^\s*type:\s*boolean\s*$' `
        "$Context explicit external qualification input"
    Assert-Stage5WorkflowContains $enableInput '(?m)^\s*default:\s*false\s*$' `
        "$Context default-disabled external qualification input"
    foreach ($inputName in @(
        'stage5_performance_scaling_fixture_manifest:',
        'stage5_expected_performance_scaling_fixture_manifest_sha256:',
        'stage5_performance_phase_baseline_profile:',
        'stage5_expected_performance_phase_baseline_profile_sha256:',
        'stage5_performance_baseline:',
        'stage5_expected_stage3_performance_baseline_sha256:',
        'stage5_expected_stage3_executable_sha256:',
        'stage5_expected_stage3_source_commit:')) {
        Assert-Stage5WorkflowLiteral $Content $inputName `
            "$Context authoritative external performance input"
    }

    $job = Get-Stage5IndentedBlock $Content `
        'stage5-performance-scaling-qualification:' 2
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Require fresh external qualification roots',
        'Download exact lockstep-v2 artifact closure',
        'Download exact Generals x64 product',
        'Download exact Zero Hour x64 product',
        'Provision verified Zero Hour performance data',
        'Provision verified Generals base data',
        'Run external Stage 5 performance qualification',
        'Upload external Stage 5 performance qualification',
        'Clean dedicated external qualification roots') @(
        'uses', 'run', 'uses', 'uses', 'uses', 'run', 'run', 'run', 'uses', 'run') `
        "$Context exact closed step sequence"
    Assert-Stage5NoJobRunDefaults $job "$Context job metadata"
    Assert-Stage5ExactJobEnvironment $job ([ordered]@{
            STAGE5_QUALIFICATION_ROOT = "'H:\Stage5WeeklyPromotionQualification'"
            STAGE5_PERFORMANCE_TASK_ROOT = 'H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}'
            STAGE5_PERFORMANCE_DATA_TASK_ROOT = 'H:\Stage5ExternalPerformanceData-${{ github.run_id }}-${{ github.run_attempt }}'
            STAGE5_PERFORMANCE_FIXTURE_MANIFEST = '${{ inputs.stage5_performance_scaling_fixture_manifest }}'
            STAGE5_EXPECTED_PERFORMANCE_FIXTURE_MANIFEST_SHA256 = '${{ inputs.stage5_expected_performance_scaling_fixture_manifest_sha256 }}'
            STAGE5_PERFORMANCE_PHASE_BASELINE_PROFILE = '${{ inputs.stage5_performance_phase_baseline_profile }}'
            STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256 = '${{ inputs.stage5_expected_performance_phase_baseline_profile_sha256 }}'
            STAGE5_STAGE3_BASELINE = '${{ inputs.stage5_performance_baseline }}'
            STAGE5_EXPECTED_STAGE3_BASELINE_SHA256 = '${{ inputs.stage5_expected_stage3_performance_baseline_sha256 }}'
            STAGE5_EXPECTED_STAGE3_EXECUTABLE_SHA256 = '${{ inputs.stage5_expected_stage3_executable_sha256 }}'
            STAGE5_EXPECTED_STAGE3_SOURCE_COMMIT = '${{ inputs.stage5_expected_stage3_source_commit }}'
            STAGE5_COHORT_NONCE = '${{ needs[''stage5-execution-cohort''].outputs.nonce }}'
            STAGE5_COHORT_CREATED_UTC = '${{ needs[''stage5-execution-cohort''].outputs.created_utc }}'
        }) "$Context exact job identity and reviewed inputs"
    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactCheckoutStep $checkoutStep $true `
        "$Context trusted current-commit checkout"
    Assert-Stage5WorkflowLiteral $job `
        'needs: [build-generals-x64, build-generalsmd-x64, stage5-execution-cohort, stage5-lockstep-v2-qualification]' `
        "$Context exact products, cohort, and artifact-set dependency"
    $condition = Get-Stage5IndentedBlock $job 'if: >-' 4
    Assert-Stage5ExactFoldedJobCondition $condition `
        ("github.event_name == 'workflow_dispatch' && " +
            'inputs.stage5_lockstep_v2_qualification == true && ' +
            'inputs.stage5_external_performance_qualification == true && ' +
            "inputs.stage5_performance_scaling_fixture_manifest != '' && " +
            "inputs.stage5_expected_performance_scaling_fixture_manifest_sha256 != '' && " +
            "inputs.stage5_performance_phase_baseline_profile != '' && " +
            "inputs.stage5_expected_performance_phase_baseline_profile_sha256 != '' && " +
            "inputs.stage5_performance_baseline != '' && " +
            "inputs.stage5_expected_stage3_performance_baseline_sha256 != '' && " +
            "inputs.stage5_expected_stage3_executable_sha256 != '' && " +
            "inputs.stage5_expected_stage3_source_commit != ''") `
        "$Context explicit opt-in and complete-input gate"
    Assert-Stage5WorkflowLiteral $job `
        'runs-on: [self-hosted, windows, x64, stage5-16-physical-core]' `
        "$Context dedicated external 16-physical-core runner"
    Assert-Stage5WorkflowLiteral $job `
        'group: stage5-external-16-core-performance-qualification' `
        "$Context serialized canonical runtime root"
    Assert-Stage5WorkflowLiteral $job `
        'cancel-in-progress: false' "$Context non-cancelling qualification"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_PERFORMANCE_TASK_ROOT: H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}' `
        "$Context run-owned external performance root"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_PERFORMANCE_DATA_TASK_ROOT: H:\Stage5ExternalPerformanceData-${{ github.run_id }}-${{ github.run_attempt }}' `
        "$Context run-owned external data root"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_COHORT_NONCE: ${{ needs[''stage5-execution-cohort''].outputs.nonce }}' `
        "$Context shared cohort nonce"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_COHORT_CREATED_UTC: ${{ needs[''stage5-execution-cohort''].outputs.created_utc }}' `
        "$Context shared cohort timestamp"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_PERFORMANCE_PHASE_BASELINE_PROFILE: ${{ inputs.stage5_performance_phase_baseline_profile }}' `
        "$Context reviewed serial-oracle profile path"
    Assert-Stage5WorkflowLiteral $job `
        'STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256: ${{ inputs.stage5_expected_performance_phase_baseline_profile_sha256 }}' `
        "$Context independent serial-oracle profile hash"

    $provisionStep = Get-Stage5IndentedBlock $job `
        '- name: Require fresh external qualification roots' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $provisionStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact external-root step metadata"
    $effectiveProvisionStep = Get-Stage5WorkflowEffectiveContent $provisionStep
    foreach ($provisionBinding in @(
        'Test-Path -LiteralPath $env:STAGE5_QUALIFICATION_ROOT',
        'Test-Path -LiteralPath $env:STAGE5_PERFORMANCE_TASK_ROOT',
        'Test-Path -LiteralPath $env:STAGE5_PERFORMANCE_DATA_TASK_ROOT',
        '[IO.Directory]::CreateDirectory($env:STAGE5_QUALIFICATION_ROOT)',
        'STAGE5_EXTERNAL_CLEANUP_OWNER=')) {
        Assert-Stage5WorkflowLiteral $effectiveProvisionStep $provisionBinding `
            "$Context fresh owned-root acquisition"
    }
    Assert-Stage5ClosedPowerShellRunBlock $provisionStep `
        'E1F0BCE91C3B368C9F0B2F9689DC3B2A87E0A666AA1D0611A0EDC430AF358392' 7 `
        "$Context exact closed external-root acquisition program"

    Assert-Stage5CanonicalDownloadArtifactPins $job 3 `
        "$Context exact artifact downloads"
    foreach ($downloadSpec in @(
        [pscustomobject]@{
            step = '- name: Download exact lockstep-v2 artifact closure'
            name = 'Stage5-LockstepV2-Qualification'
            path = 'H:\Stage5WeeklyPromotionQualification'
        },
        [pscustomobject]@{
            step = '- name: Download exact Generals x64 product'
            name = 'Generals-x64-generals-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\GeneralsRuntime'
        },
        [pscustomobject]@{
            step = '- name: Download exact Zero Hour x64 product'
            name = 'GeneralsMD-x64-zerohour-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime'
        })) {
        $downloadStep = Get-Stage5IndentedBlock $job $downloadSpec.step 6
        Assert-Stage5CurrentRunArtifactDownload $downloadStep `
            $downloadSpec.name $downloadSpec.path `
            "$Context exact canonical artifact restoration"
    }

    $dataStep = Get-Stage5IndentedBlock $job `
        '- name: Provision verified Zero Hour performance data' 6
    $performanceDataEnvironment = [ordered]@{
        AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
        AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
        AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    }
    Assert-Stage5ExactPowerShellStepMetadata -Step $dataStep `
        -ExpectedEnvironment $performanceDataEnvironment -Context `
        "$Context exact performance-data step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $dataStep 6 8 `
        "$Context verified performance data"
    Assert-Stage5WorkflowExecutablePowerShellCall $dataStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5PerformanceQualificationData.ps1' `
        "$Context exact trusted performance-data producer" `
        ([ordered]@{
            ArchivePath = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\zerohour104_gamedata_trimmed.7z"'
            ExpectedArchiveSha256 = '"6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21"'
            TaskRoot = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT"'
            RuntimeRoot = '"$env:STAGE5_QUALIFICATION_ROOT\ZeroHourRuntime"'
            OutputPath = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\Stage5PerformanceQualificationData.json"'
            ExpectedSourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            SevenZipPath = '"C:\Program Files\7-Zip\7z.exe"'
        })
    Assert-Stage5ClosedPowerShellRunBlock $dataStep `
        '4B33624ED5AC55568853DCCA478C2C0A3C07562536921BE9D2C60D6199AAAC9A' 16 `
        "$Context exact closed performance-data program"

    $baseDataStep = Get-Stage5IndentedBlock $job '- name: Provision verified Generals base data' 6
    Assert-Stage5ExactPowerShellStepMetadata $baseDataStep ([ordered]@{
        AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
        AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
        AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    }) '' "$Context exact base Generals data step"
    Assert-Stage5WorkflowExecutablePowerShellCall $baseDataStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5SimulationQualificationData.ps1' `
        "$Context exact base Generals data producer" ([ordered]@{
            RuntimeRoot = '(Join-Path $env:STAGE5_QUALIFICATION_ROOT ''GeneralsRuntime'')'
            TaskRoot = "'H:\Stage5SimulationValidationTask'"
            SourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            Title = "'Generals'"
            AwsEndpointUrl = '$env:AWS_ENDPOINT_URL'
            OutputEnvironmentFile = '$env:GITHUB_ENV'
        })
    Assert-Stage5ClosedPowerShellRunBlock $baseDataStep `
        '69A63389C595DF5A6115D937413DD3122FA7564C594C6292547E149100E9E04C' 4 `
        "$Context closed base Generals data program"

    $runStep = Get-Stage5IndentedBlock $job `
        '- name: Run external Stage 5 performance qualification' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $runStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact external-qualifier step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $runStep 6 8 `
        "$Context executable external performance invocation"
    $effectiveRunStep = Get-Stage5WorkflowEffectiveContent $runStep
    Assert-Stage5RepositoryRelativeFileGuard $runStep `
        "$Context reviewed performance input containment" `
        ([ordered]@{
            '$fixtureManifest' = @('$workspace',
                '$env:STAGE5_PERFORMANCE_FIXTURE_MANIFEST',
                "'Stage 5 performance-scaling fixture manifest'")
            '$stage3Baseline' = @('$workspace',
                '$env:STAGE5_STAGE3_BASELINE',
                "'Stage 3 performance baseline'")
            '$phaseBaselineProfilePath' = @('$workspace',
                '$env:STAGE5_PERFORMANCE_PHASE_BASELINE_PROFILE',
                "'Stage 5 performance phase-baseline profile'")
        })
    Assert-Stage5WorkflowExecutablePowerShellCall $runStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5PerformanceScalingValidation.ps1' `
        "$Context scoped executable external performance invocation" `
        ([ordered]@{
            Title = 'ZeroHour'
            InstalledExecutablePath = '$executablePath'
            ExpectedExecutableSha256 = '$executableSha256'
            ExpectedSourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            ExpectedArtifactSetSha256 = '$artifactSetSha256'
            ArtifactSetManifestPath = '$artifactSetPath'
            GeneralsQualificationDataManifestPath = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_PATH'
            GeneralsQualificationDataManifestSha256 = '$env:STAGE5_SIMULATION_QUALIFICATION_DATA_MANIFEST_SHA256'
            AllowHeadlessDirectExecution = $null
            FixtureManifestPath = '$fixtureManifest'
            ExpectedFixtureManifestSha256 = '$env:STAGE5_EXPECTED_PERFORMANCE_FIXTURE_MANIFEST_SHA256'
            Stage3BaselinePath = '$stage3Baseline'
            ExpectedStage3BaselineSha256 = '$env:STAGE5_EXPECTED_STAGE3_BASELINE_SHA256'
            ExpectedStage3ExecutableSha256 = '$env:STAGE5_EXPECTED_STAGE3_EXECUTABLE_SHA256'
            ExpectedStage3SourceCommit = '$env:STAGE5_EXPECTED_STAGE3_SOURCE_COMMIT'
            TaskRoot = '$env:STAGE5_PERFORMANCE_TASK_ROOT'
            MeasuredRuns = '3'
            QualificationMode = 'External16Core'
            ReferencePolicy = 'paired-serial-oracle-v1'
            PhaseBaselineProfiles = '@($phaseBaselineProfile)'
            PhaseBaselineProfilePath = '$phaseBaselineProfilePath'
            ExpectedPhaseBaselineProfileSha256 = '$env:STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256'
            PerformanceDataManifestPath = '$env:STAGE5_PERFORMANCE_DATA_MANIFEST_PATH'
            ExpectedPerformanceDataManifestSha256 = '$env:STAGE5_PERFORMANCE_DATA_MANIFEST_SHA256'
            ExpectedPerformanceDataClosureSha256 = '$env:STAGE5_PERFORMANCE_DATA_CLOSURE_SHA256'
            ExecutionCohortNonce = '$env:STAGE5_COHORT_NONCE'
            ExecutionCohortCreatedUtc = '$env:STAGE5_COHORT_CREATED_UTC'
        })
    foreach ($binding in @(
        '-Title ZeroHour',
        '-ArtifactSetManifestPath $artifactSetPath',
        '-ExpectedArtifactSetSha256 $artifactSetSha256',
        '-FixtureManifestPath $fixtureManifest',
        '-ExpectedFixtureManifestSha256 $env:STAGE5_EXPECTED_PERFORMANCE_FIXTURE_MANIFEST_SHA256',
        '-Stage3BaselinePath $stage3Baseline',
        '-ExpectedStage3BaselineSha256 $env:STAGE5_EXPECTED_STAGE3_BASELINE_SHA256',
        '-ExpectedStage3ExecutableSha256 $env:STAGE5_EXPECTED_STAGE3_EXECUTABLE_SHA256',
        '-ExpectedStage3SourceCommit $env:STAGE5_EXPECTED_STAGE3_SOURCE_COMMIT',
        '-QualificationMode External16Core',
        '-ReferencePolicy paired-serial-oracle-v1',
        '-PhaseBaselineProfiles @($phaseBaselineProfile)',
        '-PhaseBaselineProfilePath $phaseBaselineProfilePath',
        '-ExpectedPhaseBaselineProfileSha256 $env:STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256',
        '-PerformanceDataManifestPath $env:STAGE5_PERFORMANCE_DATA_MANIFEST_PATH',
        '-ExpectedPerformanceDataManifestSha256 $env:STAGE5_PERFORMANCE_DATA_MANIFEST_SHA256',
        '-ExpectedPerformanceDataClosureSha256 $env:STAGE5_PERFORMANCE_DATA_CLOSURE_SHA256',
        '-ExecutionCohortNonce $env:STAGE5_COHORT_NONCE',
        '-ExecutionCohortCreatedUtc $env:STAGE5_COHORT_CREATED_UTC',
        'Stage5PerformanceQualificationData.json',
        'Stage5PerformanceScalingQualification.json',
        'Stage5PerformanceScalingRawSamples.json',
        'Stage5PerformanceScaling.json')) {
        Assert-Stage5WorkflowLiteral $effectiveRunStep $binding `
            "$Context authoritative external performance invocation"
    }
    Assert-Stage5WorkflowNotContains $effectiveRunStep 'LocalCapacitySmoke' `
        "$Context acceptance-ineligible local smoke"
    foreach ($profileBinding in @(
        '$phaseBaselineProfilePath = Resolve-Stage5RepositoryFile',
        'STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256',
        'ConvertFrom-Json -NoEnumerate',
        '$phaseBaselineProfile -is [Array]',
        'must be one top-level JSON object')) {
        Assert-Stage5WorkflowLiteral $effectiveRunStep $profileBinding `
            "$Context reviewed serial-oracle profile binding"
    }
    Assert-Stage5ClosedPowerShellRunBlock $runStep `
        'AECFA42240538FA82ECAE4C6349564B21F8DDE6211FFF11518ACFAFBC8C82EC9' 26 `
        "$Context exact closed external performance program"

    $uploadStep = Get-Stage5IndentedBlock $job `
        '- name: Upload external Stage 5 performance qualification' 6
    Assert-Stage5ExactActionStep $uploadStep `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Stage5-Performance-Scaling-Qualification'
            path = 'H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}'
            'retention-days' = '30'
            'if-no-files-found' = 'error'
        }) "$Context exact performance qualification upload"
    Assert-Stage5WorkflowFailClosedStep $uploadStep `
        "$Context unconditional performance upload"
    Assert-Stage5NoStepEnvironment $uploadStep `
        "$Context inherited performance upload identity"
    Assert-Stage5CanonicalUploadArtifactPins $uploadStep 1 `
        "$Context authoritative performance upload"
    Assert-Stage5WorkflowLiteral $uploadStep `
        'name: Stage5-Performance-Scaling-Qualification' `
        "$Context exact performance artifact name"
    Assert-Stage5WorkflowLiteral $uploadStep 'if-no-files-found: error' `
        "$Context fail-closed performance upload"
    $cleanupStep = Get-Stage5IndentedBlock $job `
        '- name: Clean dedicated external qualification roots' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $cleanupStep `
        -ExpectedEnvironment ([ordered]@{ STAGE5_BASE_CONSUMERS_STATUS = '${{ job.status }}' }) -ExpectedIf '${{ always() }}' -Context `
        "$Context exact guarded-cleanup step metadata"
    Assert-Stage5WorkflowLiteral $cleanupStep 'if: ${{ always() }}' `
        "$Context unconditional owned-root cleanup"
    $effectiveCleanupStep = Get-Stage5WorkflowEffectiveContent $cleanupStep
    foreach ($cleanupBinding in @(
        'STAGE5_EXTERNAL_CLEANUP_OWNER -cne $expectedOwner',
        'No roots were acquired by this job',
        'Get-Process -Name generalsv, generalszh',
        "`$expectedRoot = 'H:\Stage5WeeklyPromotionQualification'",
        'H:\Stage5ExternalPerformanceQualification-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT',
        'H:\Stage5ExternalPerformanceData-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT',
        'not owned by this run attempt',
        '^H:\\Stage5ExternalPerformance(?:Qualification|Data)-[0-9]+-[0-9]+$',
        'ReparsePoint',
        'Remove-Item -LiteralPath $ownedRoot -Recurse -Force')) {
        Assert-Stage5WorkflowLiteral $effectiveCleanupStep $cleanupBinding `
            "$Context guarded owned-root cleanup"
    }
    Assert-Stage5ClosedPowerShellRunBlock $cleanupStep `
        '3BF5589EB86AA67F01D34C41810B83C17E01B2B7773BBFD4820144E337701C0C' 13 `
        "$Context exact closed owned-root cleanup program"

    $provisionIndex = $job.IndexOf(
        '- name: Require fresh external qualification roots',
        [StringComparison]::Ordinal)
    $dataIndex = $job.IndexOf(
        '- name: Provision verified Zero Hour performance data',
        [StringComparison]::Ordinal)
    $runIndex = $job.IndexOf(
        '- name: Run external Stage 5 performance qualification',
        [StringComparison]::Ordinal)
    $uploadIndex = $job.IndexOf(
        '- name: Upload external Stage 5 performance qualification',
        [StringComparison]::Ordinal)
    $cleanupIndex = $job.IndexOf(
        '- name: Clean dedicated external qualification roots',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($provisionIndex -ge 0 -and
        $dataIndex -gt $provisionIndex -and
        $runIndex -gt $dataIndex -and $uploadIndex -gt $runIndex -and
        $cleanupIndex -gt $uploadIndex) `
        "$Context must provision verified data, qualify, upload, and then clean task-owned roots."
}

function Assert-Stage5DevelopmentReadinessCondition {
    param([string]$Condition, [string]$Context)

    $normalized = (@($Condition -split '\r?\n' |
        ForEach-Object { $_.Trim() }) -join ' ')
    $expected = "if: >- `${{ always() && github.event_name == 'workflow_dispatch' && " +
        'inputs.stage5_lockstep_v2_qualification == true && ' +
        'inputs.stage5_external_performance_qualification == true && ' +
        "inputs.stage5_acceptance_manifest != '' && " +
        "needs['stage5-execution-cohort'].result == 'success' && " +
        "needs['stage5-replaycheck-generalsmd-x64'].result == 'success' && " +
        "needs['stage5-replaycheck-generals-x64'].result == 'success' && " +
        "needs['stage5-combined-host-runner'].result == 'success' && " +
        "needs['stage5-lockstep-v2-qualification'].result == 'success' && " +
        "needs['stage5-performance-scaling-qualification'].result == 'success' }}"
    Assert-Stage5WorkflowCondition ($normalized -ceq $expected) `
        "$Context must use the exact fail-closed readiness condition."
}

function Assert-Stage5WorkflowFailClosedStep {
    param([string]$Step, [string]$Context)

    $directKeys = @(Get-Stage5CanonicalYamlMappingEntries `
            $Step 8 "$Context step metadata" | ForEach-Object { $_.Key })
    Assert-Stage5WorkflowCondition `
        (-not ($directKeys -ccontains 'if') -and
         -not ($directKeys -ccontains 'continue-on-error')) `
        "$Context contains a condition or continue-on-error bypass."
}

function Assert-Stage5SealedReadinessUpload {
    param([string]$Step, [string]$Context)

    Assert-Stage5WorkflowFailClosedStep $Step `
        "$Context unconditional fail-closed upload"
    Assert-Stage5ExactActionStep $Step `
        'actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f' `
        ([ordered]@{
            name = 'Stage5-Development-Readiness'
            path = 'H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip'
            'retention-days' = '30'
            'if-no-files-found' = 'error'
        }) "$Context exact sealed upload action"
    Assert-Stage5WorkflowLiteral $Step `
        'name: Stage5-Development-Readiness' "$Context exact artifact name"
    Assert-Stage5WorkflowContains $Step `
        '(?m)^\s{10}path:\s*H:\\Stage5WeeklyPromotionQualification\\Stage5DevelopmentReadinessBundle\.zip\s*$' `
        "$Context exact sealed payload"
    Assert-Stage5WorkflowNotContains $Step `
        '(?m)^\s*path:\s*\||^\s*path:\s*H:\\Stage5WeeklyPromotionQualification\s*$|GeneralsRuntime|ZeroHourRuntime' `
        "$Context broad or mutable payload"
    Assert-Stage5WorkflowLiteral $Step 'if-no-files-found: error' `
        "$Context fail-closed missing-file policy"
}

function Assert-Stage5ReadinessTerminalStepOrder {
    param([string]$Job, [string]$Context)

    Assert-Stage5WorkflowNotContains $Job '(?m)^ {6}- (?!name:)' `
        "$Context anonymous step"
    $stepNames = @([regex]::Matches($Job,
            '(?m)^ {6}- name:\s*(?<Name>[^\r\n]+)$') |
        ForEach-Object { $_.Groups['Name'].Value })
    $assemblyOrdinal = [Array]::IndexOf($stepNames,
        'Assemble trusted Stage 5 development-readiness bundle')
    $acceptanceOrdinal = [Array]::IndexOf($stepNames,
        'Run Stage 5 final pre-manual acceptance')
    $sealOrdinal = [Array]::IndexOf($stepNames,
        'Seal Stage 5 development-readiness bundle')
    $uploadOrdinal = [Array]::IndexOf($stepNames,
        'Upload Stage 5 development-readiness evidence')
    Assert-Stage5WorkflowCondition ($assemblyOrdinal -ge 0 -and
        $acceptanceOrdinal -eq $assemblyOrdinal + 1 -and
        $sealOrdinal -eq $acceptanceOrdinal + 1 -and
        $uploadOrdinal -eq $sealOrdinal + 1) `
        "$Context must adjacently assemble, validate, seal, and publish."
}

function Assert-Stage5DevelopmentReadinessProducer {
    param([string]$Content, [string]$Context)

    $job = Get-Stage5IndentedBlock $Content 'stage5-development-readiness:' 2
    Assert-Stage5ExactJobStepSequence $job @(
        'Checkout Code',
        'Provision isolated Stage 5 readiness volume',
        'Download exact lockstep-v2 closure',
        'Download exact Generals x64 product for closure validation',
        'Download exact Zero Hour x64 product for closure validation',
        'Download Generals Stage 5 evidence',
        'Download Zero Hour Stage 5 evidence',
        'Download combined Stage 5 evidence',
        'Download external Stage 5 performance qualification',
        'Assemble trusted Stage 5 development-readiness bundle',
        'Run Stage 5 final pre-manual acceptance',
        'Seal Stage 5 development-readiness bundle',
        'Upload Stage 5 development-readiness evidence',
        'Clean Stage 5 development-readiness validation volume') @(
        'uses', 'run', 'uses', 'uses', 'uses', 'uses', 'uses', 'uses', 'uses',
        'run', 'run', 'run', 'uses', 'run') "$Context exact closed step sequence"
    Assert-Stage5NoJobRunDefaults $job "$Context job metadata"
    Assert-Stage5ExactJobEnvironment $job ([ordered]@{
            STAGE5_ACCEPTANCE_TEMPLATE = '${{ inputs.stage5_acceptance_manifest }}'
            STAGE5_COHORT_NONCE = '${{ needs[''stage5-execution-cohort''].outputs.nonce }}'
            STAGE5_COHORT_CREATED_UTC = '${{ needs[''stage5-execution-cohort''].outputs.created_utc }}'
            STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256 = '${{ inputs.stage5_expected_performance_phase_baseline_profile_sha256 }}'
            STAGE5_READINESS_ROOT = "'H:\Stage5WeeklyPromotionQualification'"
            STAGE5_PERFORMANCE_ROOT = 'H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}'
            STAGE5_SOURCE_ROOT = 'H:\Stage5DevelopmentReadinessSources-${{ github.run_id }}-${{ github.run_attempt }}'
        }) "$Context exact job identity and staging roots"
    $checkoutStep = Get-Stage5IndentedBlock $job '- name: Checkout Code' 6
    Assert-Stage5ExactCheckoutStep $checkoutStep $true `
        "$Context trusted full-history current-commit checkout"
    Assert-Stage5WorkflowLiteral $job `
        'needs: [stage5-execution-cohort, stage5-replaycheck-generalsmd-x64, stage5-replaycheck-generals-x64, stage5-combined-host-runner, stage5-lockstep-v2-qualification, stage5-performance-scaling-qualification]' `
        "$Context complete producer dependency set"
    $condition = Get-Stage5IndentedBlock $job 'if: >-' 4
    Assert-Stage5DevelopmentReadinessCondition $condition `
        "$Context fail-closed prerequisite gate"
    foreach ($binding in @(
        'STAGE5_COHORT_NONCE: ${{ needs[''stage5-execution-cohort''].outputs.nonce }}',
        'STAGE5_COHORT_CREATED_UTC: ${{ needs[''stage5-execution-cohort''].outputs.created_utc }}',
        'STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256: ${{ inputs.stage5_expected_performance_phase_baseline_profile_sha256 }}',
        "STAGE5_READINESS_ROOT: 'H:\Stage5WeeklyPromotionQualification'",
        'STAGE5_PERFORMANCE_ROOT: H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}',
        'STAGE5_SOURCE_ROOT: H:\Stage5DevelopmentReadinessSources-${{ github.run_id }}-${{ github.run_attempt }}')) {
        Assert-Stage5WorkflowLiteral $job $binding `
            "$Context exact shared identity or staging root"
    }
    Assert-Stage5CanonicalDownloadArtifactPins $job 7 `
        "$Context complete readiness downloads"
    $downloadSpecs = @(
        [pscustomobject]@{ step = '- name: Download exact lockstep-v2 closure'
            artifact = 'Stage5-LockstepV2-Qualification'
            path = 'H:\Stage5WeeklyPromotionQualification' },
        [pscustomobject]@{ step = '- name: Download exact Generals x64 product for closure validation'
            artifact = 'Generals-x64-generals-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\GeneralsRuntime' },
        [pscustomobject]@{ step = '- name: Download exact Zero Hour x64 product for closure validation'
            artifact = 'GeneralsMD-x64-zerohour-vcpkg-product+e'
            path = 'H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime' },
        [pscustomobject]@{ step = '- name: Download Generals Stage 5 evidence'
            artifact = 'Stage5-Simulation-Validation-Generals-x64-generals-vcpkg-product+e'
            path = 'H:\Stage5DevelopmentReadinessSources-${{ github.run_id }}-${{ github.run_attempt }}\Generals' },
        [pscustomobject]@{ step = '- name: Download Zero Hour Stage 5 evidence'
            artifact = 'Stage5-Simulation-Validation-GeneralsMD-x64-zerohour-vcpkg-product+e'
            path = 'H:\Stage5DevelopmentReadinessSources-${{ github.run_id }}-${{ github.run_attempt }}\ZeroHour' },
        [pscustomobject]@{ step = '- name: Download combined Stage 5 evidence'
            artifact = 'Stage5-Simulation-Validation-combined'
            path = 'H:\Stage5DevelopmentReadinessSources-${{ github.run_id }}-${{ github.run_attempt }}\Combined' },
        [pscustomobject]@{ step = '- name: Download external Stage 5 performance qualification'
            artifact = 'Stage5-Performance-Scaling-Qualification'
            path = 'H:\Stage5ExternalPerformanceQualification-${{ github.run_id }}-${{ github.run_attempt }}' }
    )
    foreach ($downloadSpec in $downloadSpecs) {
        $downloadStep = Get-Stage5IndentedBlock $job $downloadSpec.step 6
        Assert-Stage5CurrentRunArtifactDownload $downloadStep `
            $downloadSpec.artifact $downloadSpec.path `
            "$Context exact current-run evidence input"
    }

    $provisionStep = Get-Stage5IndentedBlock $job `
        '- name: Provision isolated Stage 5 readiness volume' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $provisionStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact readiness-volume step metadata"
    Assert-Stage5WorkflowFailClosedStep $provisionStep `
        "$Context unconditional isolated-volume provisioning"
    Assert-Stage5ValidationVolumeInvocation $provisionStep 'Provision' `
        '(?s)development-readiness.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume provision helper"
    Assert-Stage5ClosedPowerShellRunBlock $provisionStep `
        '6656222449DC5A3DA686DBC6CC65C1AE2FE829B9A94303D55F4B4FE12FE3E384' 6 `
        "$Context exact isolated-volume provisioning program"

    $assemblyStep = Get-Stage5IndentedBlock $job `
        '- name: Assemble trusted Stage 5 development-readiness bundle' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $assemblyStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact trusted-assembly step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $assemblyStep 6 8 `
        "$Context executable trusted assembly"
    Assert-Stage5WorkflowFailClosedStep $assemblyStep `
        "$Context unconditional fail-closed assembly step"
    Assert-Stage5WorkflowExecutablePowerShellCall $assemblyStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5DevelopmentReadinessBundle.ps1' `
        "$Context scoped executable trusted assembly" `
        ([ordered]@{
            AcceptanceTemplateRoot = '$env:GITHUB_WORKSPACE'
            AcceptanceTemplatePath = '$env:STAGE5_ACCEPTANCE_TEMPLATE'
            GeneralsEvidenceRoot = '"$env:STAGE5_SOURCE_ROOT/Generals"'
            ZeroHourEvidenceRoot = '"$env:STAGE5_SOURCE_ROOT/ZeroHour"'
            CombinedEvidenceRoot = '"$env:STAGE5_SOURCE_ROOT/Combined"'
            LockstepEvidenceRoot = '$env:STAGE5_READINESS_ROOT'
            PerformanceEvidenceRoot = '$env:STAGE5_PERFORMANCE_ROOT'
            ExternalQualificationExempt = $null
            OutputRoot = '$env:STAGE5_READINESS_ROOT'
            ExpectedSourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            ExpectedPhaseBaselineProfileSha256 = '$env:STAGE5_EXPECTED_PERFORMANCE_PHASE_BASELINE_PROFILE_SHA256'
            ExpectedCohortNonce = '$env:STAGE5_COHORT_NONCE'
            ExpectedCohortCreatedUtc = '$env:STAGE5_COHORT_CREATED_UTC'
        })
    Assert-Stage5ClosedPowerShellRunBlock $assemblyStep `
        '9898BA9390D3B03579596B221602181D25E1269A603BB4C0C24B70CFB00BDB5A' 3 `
        "$Context exact closed trusted assembly program"

    $acceptanceStep = Get-Stage5IndentedBlock $job `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $acceptanceStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact final-acceptance step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $acceptanceStep 6 8 `
        "$Context executable final acceptance"
    Assert-Stage5WorkflowFailClosedStep $acceptanceStep `
        "$Context unconditional fail-closed final acceptance step"
    $effectiveAcceptanceStep = Get-Stage5WorkflowEffectiveContent $acceptanceStep
    Assert-Stage5WorkflowExecutablePowerShellCall $acceptanceStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
        "$Context scoped executable final acceptance" `
        ([ordered]@{
            AcceptanceManifestPath = '$manifest'
            OutputPath = '$output'
            ReadinessMode = 'development-readiness'
            DevelopmentReadiness = $null
            ExternalQualificationExempt = $null
        })
    Assert-Stage5ClosedPowerShellRunBlock $acceptanceStep `
        '1E6DF69B90B225C5D9D2811BC3AB361097D583D4371A80F811605C8FD3614BB4' 8 `
        "$Context exact closed final acceptance program"
    foreach ($binding in @(
        '-AcceptanceManifestPath $manifest',
        '-OutputPath $output',
        '-ReadinessMode development-readiness',
        '-DevelopmentReadiness',
        '-ExternalQualificationExempt',
        "gateName -cne 'stage5-development-readiness'",
        "status -cne 'ready-for-manual-approval'",
        '$report.premiumReviewRequired -isnot [bool]',
        '$report.manualApprovalRequired -isnot [bool]',
        '$report.finalAcceptanceClaim -isnot [bool]',
        '[bool]$report.finalAcceptanceClaim')) {
        Assert-Stage5WorkflowLiteral $effectiveAcceptanceStep $binding `
            "$Context final pre-manual acceptance contract"
    }
    Assert-Stage5WorkflowLiteral $effectiveAcceptanceStep `
        'Import-Module "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/DeterministicSimulationEvidence.psm1"' `
        "$Context shared JSON integer predicate import"
    Assert-Stage5JsonIntegerSchemaVersionGuards $acceptanceStep 1 `
        "$Context strict final pre-manual schemaVersion gate"
    Assert-Stage5WorkflowNotContains $effectiveAcceptanceStep `
        '(?i)\[[^]]+\]\$report\.schemaVersion' `
        "$Context pre-manual schemaVersion cast"

    $sealStep = Get-Stage5IndentedBlock $job `
        '- name: Seal Stage 5 development-readiness bundle' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $sealStep `
        -ExpectedEnvironment $null -Context `
        "$Context exact readiness-seal step metadata"
    Assert-Stage5WorkflowRunBlockIndentation $sealStep 6 8 `
        "$Context executable readiness sealer"
    Assert-Stage5WorkflowFailClosedStep $sealStep `
        "$Context unconditional fail-closed seal step"
    Assert-Stage5WorkflowExecutablePowerShellCall $sealStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Seal-Stage5DevelopmentReadinessBundle.ps1' `
        "$Context exact readiness sealer" `
        ([ordered]@{
            AcceptanceManifestPath = '"$env:STAGE5_READINESS_ROOT/FinalAcceptanceManifest.json"'
            ReadinessReportPath = '"$env:STAGE5_READINESS_ROOT/Stage5DevelopmentReadiness.json"'
            OutputPath = '"$env:STAGE5_READINESS_ROOT/Stage5DevelopmentReadinessBundle.zip"'
            ExpectedSourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
            ExpectedCohortNonce = '$env:STAGE5_COHORT_NONCE'
            ExpectedCohortCreatedUtc = '$env:STAGE5_COHORT_CREATED_UTC'
            ExternalQualificationExempt = $null
        })
    Assert-Stage5ClosedPowerShellRunBlock $sealStep `
        '730FEF0A0686136A18EC1C72D8862DB03614EE183BAB5EE7907866467FF77019' 3 `
        "$Context exact closed readiness sealer program"

    $uploadStep = Get-Stage5IndentedBlock $job `
        '- name: Upload Stage 5 development-readiness evidence' 6
    Assert-Stage5NoStepEnvironment $uploadStep `
        "$Context inherited readiness upload identity"
    Assert-Stage5CanonicalUploadArtifactPins $uploadStep 1 `
        "$Context final readiness upload"
    Assert-Stage5SealedReadinessUpload $uploadStep `
        "$Context final readiness upload"
    $cleanupStep = Get-Stage5IndentedBlock $job `
        '- name: Clean Stage 5 development-readiness validation volume' 6
    Assert-Stage5ValidationVolumeInvocation $cleanupStep 'Cleanup' `
        '(?s)development-readiness.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
        "$Context validation-volume cleanup helper"

    $assemblyIndex = $job.IndexOf(
        '- name: Assemble trusted Stage 5 development-readiness bundle',
        [StringComparison]::Ordinal)
    $acceptanceIndex = $job.IndexOf(
        '- name: Run Stage 5 final pre-manual acceptance',
        [StringComparison]::Ordinal)
    $sealIndex = $job.IndexOf(
        '- name: Seal Stage 5 development-readiness bundle',
        [StringComparison]::Ordinal)
    $uploadIndex = $job.IndexOf(
        '- name: Upload Stage 5 development-readiness evidence',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($assemblyIndex -ge 0 -and
        $acceptanceIndex -gt $assemblyIndex -and $sealIndex -gt $acceptanceIndex -and
        $uploadIndex -gt $sealIndex) `
        "$Context must adjacently assemble, validate, seal, and only then publish readiness."
    $cleanupIndex = $job.IndexOf(
        '- name: Clean Stage 5 development-readiness validation volume',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($cleanupIndex -gt $uploadIndex) `
        "$Context must clean the validation volume after evidence upload."
    Assert-Stage5ReadinessTerminalStepOrder $job `
        "$Context terminal readiness steps"
}

function Assert-Stage5WeeklyPromotionGate {
    param([string]$Content, [string]$Context)

    Assert-Stage5WorkflowContains $Content 'stage5_qualified_run_id:' `
        "$Context qualification run input"
    Assert-Stage5WorkflowContains $Content `
        'stage5_lockstep_v2_attestation_sha256:' `
        "$Context independent attestation hash input"
    Assert-Stage5WorkflowLiteral $Content `
        'Stage5LockstepV2EvidenceClosure.json' `
        "$Context exact evidence-closure attestation input"
    Assert-Stage5WorkflowLiteral $Content `
        'vars.STAGE5_LOCKSTEP_V2_QUALIFIED_RUN_ID' `
        "$Context scheduled qualification run binding"
    Assert-Stage5WorkflowLiteral $Content `
        'vars.STAGE5_LOCKSTEP_V2_ATTESTATION_SHA256' `
        "$Context scheduled attestation hash binding"

    $gateJob = Get-Stage5IndentedBlock $Content `
        'validate-promotion-attestation:' 2
    Assert-Stage5WorkflowLiteral $gateJob `
        'needs: [build-generals, build-generalsmd, get-date]' `
        "$Context promotion gate build dependencies"
    Assert-Stage5WorkflowLiteral $gateJob `
        "needs.build-generals.result == 'success'" `
        "$Context Generals build prerequisite"
    Assert-Stage5WorkflowLiteral $gateJob `
        "needs.build-generalsmd.result == 'success'" `
        "$Context Zero Hour build prerequisite"
    Assert-Stage5WorkflowLiteral $gateJob `
        'repos/$env:GITHUB_REPOSITORY/actions/runs/$env:QUALIFIED_RUN_ID' `
        "$Context source-run provenance query"
    foreach ($provenanceBinding in @(
        "run.head_sha -cne `$env:GITHUB_SHA",
        "run.conclusion -cne 'success'",
        "run.event -cne 'workflow_dispatch'",
        "run.path -cne '.github/workflows/ci.yml'",
        'run.head_repository.full_name -cne $env:GITHUB_REPOSITORY')) {
        Assert-Stage5WorkflowLiteral $gateJob $provenanceBinding `
            "$Context source-run provenance gate"
    }
    Assert-Stage5CanonicalDownloadArtifactPins $gateJob 3 `
        "$Context qualified artifact downloads"
    foreach ($artifactName in @(
        'Generals-x64-generals-vcpkg-product+e',
        'GeneralsMD-x64-zerohour-vcpkg-product+e',
        'Stage5-LockstepV2-Qualification')) {
        Assert-Stage5WorkflowLiteral $gateJob "name: $artifactName" `
            "$Context qualified artifact source"
    }
    Assert-Stage5WorkflowContains $gateJob 'run-id:\s*\$\{\{' `
        "$Context cross-run artifact binding"
    Assert-Stage5WorkflowContains $gateJob 'github-token:\s*\$\{\{' `
        "$Context same-repository artifact authorization"
    Assert-Stage5WorkflowLiteral $gateJob 'path: promotion-attestation' `
        "$Context isolated attestation download"
    Assert-Stage5WorkflowLiteral $gateJob `
        "PROMOTION_BUNDLE_ROOT: 'H:\Stage5WeeklyPromotionQualification'" `
        "$Context canonical relocatable qualification root"
    foreach ($qualifiedSubtree in @(
        'path: H:\Stage5WeeklyPromotionQualification\GeneralsRuntime',
        'path: H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime')) {
        Assert-Stage5WorkflowLiteral $gateJob $qualifiedSubtree `
            "$Context exact qualified runtime staging"
    }
    Assert-Stage5WorkflowLiteral $gateJob `
        "foreach (`$forbiddenPayload in @('GeneralsRuntime', 'ZeroHourRuntime'," `
        "$Context runtime/data replacement rejection"
    Assert-Stage5WorkflowLiteral $gateJob "'QualificationData'))" `
        "$Context proprietary qualification-data rejection"
    Assert-Stage5WorkflowLiteral $gateJob `
        'Validate-Stage5WeeklyPromotionAttestation.ps1' `
        "$Context exact artifact/attestation validator"
    Assert-Stage5WorkflowLiteral $gateJob `
        '-ExpectedAttestationSha256 $env:EXPECTED_ATTESTATION_SHA256' `
        "$Context independent attestation hash argument"
    Assert-Stage5WorkflowLiteral $gateJob `
        '-ExpectedQualificationRunId $env:QUALIFIED_RUN_ID' `
        "$Context qualification run argument"
    Assert-Stage5WorkflowLiteral $gateJob `
        '-BundleRoot $env:PROMOTION_BUNDLE_ROOT' `
        "$Context canonical qualification-root validation"
    foreach ($validatedArtifact in @(
        'Weekly-Qualified-Generals-x64',
        'Weekly-Qualified-GeneralsMD-x64')) {
        Assert-Stage5WorkflowLiteral $gateJob "name: $validatedArtifact" `
            "$Context validated artifact handoff"
    }

    $releaseJob = Get-Stage5IndentedBlock $Content 'create-release:' 2
    Assert-Stage5WorkflowLiteral $releaseJob `
        'needs: [validate-promotion-attestation, get-date]' `
        "$Context publication dependency"
    Assert-Stage5WorkflowLiteral $releaseJob `
        "needs.validate-promotion-attestation.result == 'success'" `
        "$Context publication success gate"
    Assert-Stage5CanonicalDownloadArtifactPins $releaseJob 2 `
        "$Context validated publication artifact downloads"
    foreach ($validatedArtifact in @(
        'Weekly-Qualified-Generals-x64',
        'Weekly-Qualified-GeneralsMD-x64')) {
        Assert-Stage5WorkflowLiteral $releaseJob "name: $validatedArtifact" `
            "$Context validated publication input"
    }
    Assert-Stage5WorkflowNotContains $releaseJob `
        'name:\s*(?:Generals|GeneralsMD)-x64-(?:generals|zerohour)-vcpkg-product\+e' `
        "$Context unvalidated publication input"
    Assert-Stage5WorkflowNotContains $releaseJob 'zip\s+-[^\r\n]*j' `
        "$Context flattened runtime archive"
    Assert-Stage5WorkflowLiteral $releaseJob `
        '(cd generals-x64-artifacts && zip -r ../generals-x64-weekly-' `
        "$Context Generals runtime tree archive"
    Assert-Stage5WorkflowLiteral $releaseJob `
        '(cd generalsmd-x64-artifacts && zip -r ../generalszh-x64-weekly-' `
        "$Context Zero Hour runtime tree archive"
    Assert-Stage5WorkflowLiteral $releaseJob 'softprops/action-gh-release@' `
        "$Context release publisher"

    $gateIndex = $Content.IndexOf('validate-promotion-attestation:',
        [StringComparison]::Ordinal)
    $releaseIndex = $Content.IndexOf('create-release:',
        [StringComparison]::Ordinal)
    Assert-Stage5WorkflowCondition ($gateIndex -ge 0 -and
        $releaseIndex -gt $gateIndex) `
        "$Context must validate promotion before defining publication."
}

if ($SelfTest) {
    $validationVolumeHelperPath = Join-Path $PSScriptRoot `
        'Stage5ValidationVolume.ps1'
    Assert-Stage5WorkflowCondition `
        (Test-Path -LiteralPath $validationVolumeHelperPath -PathType Leaf) `
        'Stage 5 validation-volume helper self-test file is missing.'
    $validationVolumeHelper = ConvertTo-Stage5SelfTestLf (Get-Content `
        -LiteralPath $validationVolumeHelperPath -Raw)
    Assert-Stage5ValidationVolumeHelper $validationVolumeHelper `
        'self-test Stage 5 validation-volume helper'
    & pwsh -NoProfile -File $validationVolumeHelperPath `
        -Mode Cleanup -Token 'path-plan-selftest' -SelfTest
    Assert-Stage5WorkflowCondition ($LASTEXITCODE -eq 0) `
        'Stage 5 validation-volume path planning self-test failed.'
    $validationVolumeHelperCrlf = [regex]::Replace(
        $validationVolumeHelper, "`r`n|`r|`n", "`r`n")
    Assert-Stage5ValidationVolumeHelper $validationVolumeHelperCrlf `
        'self-test CRLF Stage 5 validation-volume helper'
    $cleanupQuery = ConvertTo-Stage5SelfTestLf ([string]::Join("`n", @(
        '    $diskImage = Get-DiskImage -ImagePath $Paths.BackingFile `',
        '        -ErrorAction SilentlyContinue')))
    $cleanupEarlyRemoval = ConvertTo-Stage5SelfTestLf ([string]::Join("`n", @(
        '    Remove-Item -LiteralPath $Paths.ScratchRoot -Recurse -Force',
        '    $diskImage = Get-DiskImage -ImagePath $Paths.BackingFile `',
        '        -ErrorAction SilentlyContinue')))
    foreach ($invalidValidationVolumeHelper in @(
        $validationVolumeHelper.Replace(
            'maximum=32768 type=expandable',
            'maximum=32768 type=fixed'),
        $validationVolumeHelper.Replace('detach vdisk', 'attach vdisk'),
        $validationVolumeHelper.Replace(
            'Get-Partition -DriveLetter H',
            'Get-Partition -DriveLetter G'),
        $validationVolumeHelper.Replace(
            $cleanupQuery, $cleanupEarlyRemoval))) {
        Assert-Stage5WorkflowCondition `
            ($invalidValidationVolumeHelper -cne $validationVolumeHelper) `
            'Stage 5 validation-volume helper self-test mutation did not apply.'
        $caught = $false
        try {
            Assert-Stage5ValidationVolumeHelper $invalidValidationVolumeHelper `
                'self-test invalid Stage 5 validation-volume helper'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted an unsafe or reordered validation-volume helper.'
    }

    $reviewedMapNameGuard = '(^[\\/]|:|\.\.|[;"]|[\x00-\x1F\x7F]|[^\S ])'
    Assert-Stage5WorkflowCondition `
        ('Maps\Twilight Flame\Twilight Flame.map' -notmatch $reviewedMapNameGuard) `
        'workflow contract map guard rejected the canonical spaced 2v4 map.'
    foreach ($invalidMapName in @(
        "Maps\Twilight`tFlame\Twilight Flame.map",
        "Maps\Twilight`nFlame\Twilight Flame.map",
        'Maps\Twilight Flame\Twilight Flame.map;extra',
        '..\Maps\Twilight Flame\Twilight Flame.map')) {
        Assert-Stage5WorkflowCondition ($invalidMapName -match $reviewedMapNameGuard) `
            'workflow contract map guard accepted unsafe spacing or path punctuation.'
    }

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
    $downloadReference =
        '      uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3'
    $downloadFixture = [string]::Join("`n", @(
        $downloadReference, $downloadReference, $downloadReference))
    Assert-Stage5CanonicalDownloadArtifactPins $downloadFixture 3 `
        'self-test three-source canonical action fixture'
    foreach ($invalidDownloadFixture in @(
        [string]::Join("`n", @($downloadReference, $downloadReference)),
        [string]::Join("`n", @(
            $downloadReference, $downloadReference, $downloadReference,
            $downloadReference)))) {
        $caught = $false
        try {
            Assert-Stage5CanonicalDownloadArtifactPins $invalidDownloadFixture 3 `
                'self-test missing-or-extra canonical action fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a missing or extra artifact download.'
    }
    $caught = $false
    try {
        Assert-Stage5WorkflowContains $fixture 'missing-contract' 'self-test negative fixture'
    }
    catch {
        $caught = $true
    }
    Assert-Stage5WorkflowCondition $caught 'workflow contract self-test did not reject a missing contract.'
    $caught = $false
    try {
        Assert-Stage5CanonicalDownloadArtifactPins `
            'uses: actions/download-artifact@70fc10c6e5e1ceaa63e36b76f2b72d43f7d47b13c3' `
            1 'self-test malformed action fixture'
    }
    catch {
        $caught = $true
    }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test did not reject a malformed action commit pin.'
    $artifactIsolationFixture = @'
jobs:
  build:
    steps:
      - name: Run installed native contract tests
        run: |
          $titleDirectory = 'Generals'
          $installedRuntime = Join-Path "build\preset\installed" $titleDirectory
          $validationRoot = Join-Path "build\preset\installed\Validation" $titleDirectory
          $test = Join-Path $validationRoot $testName
          Push-Location $installedRuntime
      - name: Collect ${{ inputs.game }} ${{ inputs.preset }}${{ inputs.tools && '+t' || '' }}${{ inputs.extras && '+e' || '' }} Artifact
        run: |
          $installedRuntime = Join-Path $buildDir "installed\${{ inputs.game == 'Generals' && 'Generals' || 'ZeroHour' }}"
          Copy-Item -Path (Join-Path $installedRuntime '*') -Destination $artifactsDir -Recurse -Force
'@
    Assert-Stage5ProductArtifactIsolation $artifactIsolationFixture `
        'self-test product artifact isolation fixture'
    $caught = $false
    try {
        Assert-Stage5ProductArtifactIsolation ($artifactIsolationFixture.Replace(
            'installed\Validation', 'installed\Generals')) `
            'self-test aliased validation fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a contract utility inside the product artifact root.'

    $qualificationProducerFixture = @'
on:
  workflow_dispatch:
    inputs:
      stage5_lockstep_v2_qualification:
        type: boolean
        default: false
      stage5_lockstep_v2_map_name:
        type: string
        default: ""
      stage5_lockstep_v2_generals_map_crc:
        type: string
        default: ""
      stage5_lockstep_v2_zerohour_map_crc:
        type: string
        default: ""
jobs:
  stage5-lockstep-v2-qualification:
    needs: [build-generals-x64, build-generalsmd-x64, stage5-execution-cohort]
    if: >-
      ${{ github.event_name == 'workflow_dispatch' &&
          inputs.stage5_lockstep_v2_qualification == true }}
    env:
      STAGE5_QUALIFICATION_ROOT: 'H:\Stage5WeeklyPromotionQualification'
      STAGE5_LOCKSTEP_V2_MAP_NAME: ${{ inputs.stage5_lockstep_v2_map_name }}
      STAGE5_LOCKSTEP_V2_GENERALS_MAP_CRC: ${{ inputs.stage5_lockstep_v2_generals_map_crc }}
      STAGE5_LOCKSTEP_V2_ZEROHOUR_MAP_CRC: ${{ inputs.stage5_lockstep_v2_zerohour_map_crc }}
      STAGE5_COHORT_NONCE: ${{ needs['stage5-execution-cohort'].outputs.nonce }}
      STAGE5_COHORT_CREATED_UTC: ${{ needs['stage5-execution-cohort'].outputs.created_utc }}
    steps:
      - name: Checkout Code
        uses: actions/checkout@de0fac2e4500dabe0009e67214ff5f5447ce83dd
      - name: Provision canonical Stage 5 qualification root
        shell: pwsh
        run: |
          $ErrorActionPreference = 'Stop'
          & "$env:GITHUB_WORKSPACE/.github/workflows/Stage5ValidationVolume.ps1" `
            -Mode Provision `
            -Token ('{0}-{1}-lockstep-v2' -f $env:GITHUB_RUN_ID,
              $env:GITHUB_RUN_ATTEMPT)
          if (-not $?) { throw 'Stage 5 lockstep-v2 validation volume provisioning failed.' }
          if (Test-Path -LiteralPath $env:STAGE5_QUALIFICATION_ROOT) {
            throw "Stage 5 lockstep-v2 qualification root is not fresh: $env:STAGE5_QUALIFICATION_ROOT"
          }
          [IO.Directory]::CreateDirectory($env:STAGE5_QUALIFICATION_ROOT) | Out-Null
      - name: Download exact Generals x64 product
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Generals-x64-generals-vcpkg-product+e
          path: H:\Stage5WeeklyPromotionQualification\GeneralsRuntime
      - name: Download exact Zero Hour x64 product
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: GeneralsMD-x64-zerohour-vcpkg-product+e
          path: H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime
      - name: Produce exact Stage 5 runtime manifests
        shell: pwsh
        run: |
          $ErrorActionPreference = 'Stop'
          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5RuntimeManifests.ps1" `
            -QualificationRoot $env:STAGE5_QUALIFICATION_ROOT `
            -SourceCommit $env:GITHUB_SHA.ToLowerInvariant() `
            -OutputEnvironmentFile $env:GITHUB_ENV
          if (-not $?) { throw 'Stage 5 runtime-manifest production failed.' }
      - name: Provision verified trimmed qualification data
        shell: pwsh
        env:
          AWS_ACCESS_KEY_ID: ${{ secrets.R2_ACCESS_KEY_ID }}
          AWS_SECRET_ACCESS_KEY: ${{ secrets.R2_SECRET_ACCESS_KEY }}
          AWS_ENDPOINT_URL: ${{ secrets.R2_ENDPOINT_URL }}
        run: |
          $ErrorActionPreference = 'Stop'
          [uint32]$generalsMapCrc = 0
          [uint32]$zeroHourMapCrc = 0
          if (-not [uint32]::TryParse($env:STAGE5_LOCKSTEP_V2_GENERALS_MAP_CRC,
              [Globalization.NumberStyles]::None,
              [Globalization.CultureInfo]::InvariantCulture, [ref]$generalsMapCrc) -or
              -not [uint32]::TryParse($env:STAGE5_LOCKSTEP_V2_ZEROHOUR_MAP_CRC,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$zeroHourMapCrc) -or
              $generalsMapCrc -eq 0 -or $zeroHourMapCrc -eq 0) {
            throw 'Both reviewed map CRCs must be nonzero decimal UInt32 values.'
          }
          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Install-Stage5QualificationData.ps1" `
            -QualificationRoot $env:STAGE5_QUALIFICATION_ROOT `
            -SourceCommit $env:GITHUB_SHA.ToLowerInvariant() `
            -MapName $env:STAGE5_LOCKSTEP_V2_MAP_NAME `
            -GeneralsMapCrc $generalsMapCrc `
            -ZeroHourMapCrc $zeroHourMapCrc `
            -AwsEndpointUrl $env:AWS_ENDPOINT_URL `
            -OutputEnvironmentFile $env:GITHUB_ENV
          if (-not $?) { throw 'Stage 5 qualification-data provisioning failed.' }
      - name: Run installed lockstep-v2 qualification
        shell: pwsh
        run: |
          $ErrorActionPreference = 'Stop'
          if ([string]::IsNullOrWhiteSpace($env:STAGE5_LOCKSTEP_V2_MAP_NAME)) {
            throw 'stage5_lockstep_v2_map_name is required when qualification is enabled.'
          }
          [uint32]$generalsMapCrc = 0
          [uint32]$zeroHourMapCrc = 0
          if (-not [uint32]::TryParse($env:STAGE5_LOCKSTEP_V2_GENERALS_MAP_CRC,
              [Globalization.NumberStyles]::None,
              [Globalization.CultureInfo]::InvariantCulture, [ref]$generalsMapCrc) -or
              -not [uint32]::TryParse($env:STAGE5_LOCKSTEP_V2_ZEROHOUR_MAP_CRC,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$zeroHourMapCrc) -or
              $generalsMapCrc -eq 0 -or $zeroHourMapCrc -eq 0) {
            throw 'Both title-specific lockstep-v2 map CRCs must be nonzero decimal UInt32 values when qualification is enabled.'
          }
          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-InstalledLockstepV2Validation.ps1" `
            -GeneralsExecutable $env:STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE `
            -ZeroHourExecutable $env:STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE `
            -ArtifactSetManifestPath "$env:STAGE5_QUALIFICATION_ROOT/Stage5ArtifactSet.json" `
            -SourceCommit $env:GITHUB_SHA.ToLowerInvariant() `
            -OutputDirectory "$env:STAGE5_QUALIFICATION_ROOT/Evidence" `
            -MapName $env:STAGE5_LOCKSTEP_V2_MAP_NAME `
            -GeneralsMapCrc $generalsMapCrc `
            -ZeroHourMapCrc $zeroHourMapCrc `
            -PeerCount 2 `
            -Seed 23063 `
            -ExecutionCohortNonce $env:STAGE5_COHORT_NONCE `
            -ExecutionCohortCreatedUtc $env:STAGE5_COHORT_CREATED_UTC `
            -RuntimeClosureDependencyManifestSha256 $env:STAGE5_LOCKSTEP_V2_RUNTIME_MANIFEST_SHA256 `
            -RuntimeClosureSha256 $env:STAGE5_LOCKSTEP_V2_RUNTIME_CLOSURE_SHA256 `
            -QualificationDataManifestPath "$env:STAGE5_QUALIFICATION_ROOT/Stage5QualificationData.json" `
            -QualificationDataManifestSha256 $env:STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256 `
            -QualificationDataClosureSha256 $env:STAGE5_LOCKSTEP_V2_DATA_CLOSURE_SHA256 `
            -AllowHeadlessDirectExecution
          if (-not $?) { throw 'Stage 5 installed lockstep-v2 qualification failed.' }
      - name: Upload installed lockstep-v2 qualification
        uses: actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f
        with:
          name: Stage5-LockstepV2-Qualification
          path: |
            H:\Stage5WeeklyPromotionQualification\Stage5ArtifactSet.json
            H:\Stage5WeeklyPromotionQualification\Stage5RuntimeDependencies.json
            H:\Stage5WeeklyPromotionQualification\Stage5QualificationData.json
            H:\Stage5WeeklyPromotionQualification\Evidence
          retention-days: 30
          if-no-files-found: error
      - name: Clean Stage 5 lockstep-v2 validation volume
        if: ${{ always() }}
        shell: pwsh
        run: |
          & "$env:GITHUB_WORKSPACE/.github/workflows/Stage5ValidationVolume.ps1" `
            -Mode Cleanup `
            -Token ('{0}-{1}-lockstep-v2' -f $env:GITHUB_RUN_ID,
              $env:GITHUB_RUN_ATTEMPT)
          if (-not $?) { throw 'Stage 5 lockstep-v2 validation volume cleanup failed.' }
'@
    $qualificationProducerFixture = ConvertTo-Stage5SelfTestLf $qualificationProducerFixture
    Assert-Stage5LockstepV2QualificationProducer `
        $qualificationProducerFixture 'self-test qualification producer fixture'
    foreach ($negativeProducerFixture in @(
        $qualificationProducerFixture.Replace(
            '          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-InstalledLockstepV2Validation.ps1" `',
            '          # & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-InstalledLockstepV2Validation.ps1" `'),
        $qualificationProducerFixture.Replace(
            '          name: Stage5-LockstepV2-Qualification',
            '          name: Stage5-Incomplete-Qualification'),
        $qualificationProducerFixture.Replace(
            '            H:\Stage5WeeklyPromotionQualification\Evidence',
            '            H:\Stage5WeeklyPromotionQualification\GeneralsRuntime'),
        $qualificationProducerFixture.Replace(
            '            H:\Stage5WeeklyPromotionQualification\Evidence',
            '            H:\Stage5WeeklyPromotionQualification\QualificationData'),
        $qualificationProducerFixture.Replace(
            '            H:\Stage5WeeklyPromotionQualification\Evidence',
            "            H:\Stage5WeeklyPromotionQualification\Evidence`n            C:\stale\extra.json"),
        $qualificationProducerFixture.Replace(
            '            -QualificationDataManifestSha256 $env:STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256 `',
            '            # -QualificationDataManifestSha256 $env:STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256 `'),
        $qualificationProducerFixture.Replace(
            '      - name: Provision verified trimmed qualification data',
            '      - name: Provision unverified qualification data'),
        $qualificationProducerFixture.Replace(
            '          AWS_ENDPOINT_URL: ${{ secrets.R2_ENDPOINT_URL }}',
            "          AWS_ENDPOINT_URL: `${{ secrets.R2_ENDPOINT_URL }}`n          STAGE5_COHORT_NONCE: stale-cohort"),
        $qualificationProducerFixture.Replace(
            '            -ZeroHourMapCrc $zeroHourMapCrc `',
            '            -ZeroHourMapCrc $generalsMapCrc `'),
        $qualificationProducerFixture.Replace(
            '            -ExecutionCohortNonce $env:STAGE5_COHORT_NONCE `',
            '            # -ExecutionCohortNonce $env:STAGE5_COHORT_NONCE `'),
        $qualificationProducerFixture.Replace(
            '        default: false', '        default: true'))) {
        $caught = $false
        try {
            Assert-Stage5LockstepV2QualificationProducer `
                $negativeProducerFixture 'self-test invalid qualification producer fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted an incomplete, automatic, or non-attestation qualification producer.'
    }
    $weeklyFixture = @'
stage5_qualified_run_id:
stage5_lockstep_v2_attestation_sha256:
description: Stage5LockstepV2EvidenceClosure.json
vars.STAGE5_LOCKSTEP_V2_QUALIFIED_RUN_ID
vars.STAGE5_LOCKSTEP_V2_ATTESTATION_SHA256
jobs:
  validate-promotion-attestation:
    needs: [build-generals, build-generalsmd, get-date]
    if: needs.build-generals.result == 'success' && needs.build-generalsmd.result == 'success'
    env:
      PROMOTION_BUNDLE_ROOT: 'H:\Stage5WeeklyPromotionQualification'
    steps:
      - run: gh api "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:QUALIFIED_RUN_ID"
        test: run.head_sha -cne $env:GITHUB_SHA
        conclusion: run.conclusion -cne 'success'
        event: run.event -cne 'workflow_dispatch'
        path: run.path -cne '.github/workflows/ci.yml'
        repository: run.head_repository.full_name -cne $env:GITHUB_REPOSITORY
      - name: Download Generals
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Generals-x64-generals-vcpkg-product+e
          path: H:\Stage5WeeklyPromotionQualification\GeneralsRuntime
          run-id: ${{ inputs.run }}
          github-token: ${{ secrets.GITHUB_TOKEN }}
      - name: Download Zero Hour
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: GeneralsMD-x64-zerohour-vcpkg-product+e
          path: H:\Stage5WeeklyPromotionQualification\ZeroHourRuntime
          run-id: ${{ inputs.run }}
          github-token: ${{ secrets.GITHUB_TOKEN }}
      - name: Download qualification
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Stage5-LockstepV2-Qualification
          path: promotion-attestation
          run-id: ${{ inputs.run }}
          github-token: ${{ secrets.GITHUB_TOKEN }}
      - run: foreach ($forbiddenPayload in @('GeneralsRuntime', 'ZeroHourRuntime',
          'QualificationData')) { throw }
      - run: Validate-Stage5WeeklyPromotionAttestation.ps1 -BundleRoot $env:PROMOTION_BUNDLE_ROOT -ExpectedAttestationSha256 $env:EXPECTED_ATTESTATION_SHA256 -ExpectedQualificationRunId $env:QUALIFIED_RUN_ID
      - name: Weekly-Qualified-Generals-x64
      - name: Weekly-Qualified-GeneralsMD-x64
  create-release:
    needs: [validate-promotion-attestation, get-date]
    if: needs.validate-promotion-attestation.result == 'success'
    steps:
      - name: Download qualified Generals
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Weekly-Qualified-Generals-x64
      - name: Download qualified Zero Hour
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Weekly-Qualified-GeneralsMD-x64
      - run: (cd generals-x64-artifacts && zip -r ../generals-x64-weekly-date.zip .)
      - run: (cd generalsmd-x64-artifacts && zip -r ../generalszh-x64-weekly-date.zip .)
      - uses: softprops/action-gh-release@commit
'@
    Assert-Stage5WeeklyPromotionGate $weeklyFixture `
        'self-test weekly promotion fixture'
    $caught = $false
    try {
        Assert-Stage5WeeklyPromotionGate ($weeklyFixture.Replace(
            'needs: [validate-promotion-attestation, get-date]',
            'needs: [build-generals, build-generalsmd, get-date]')) `
            'self-test bypassed weekly promotion fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a publication path that bypasses promotion validation.'
    $caught = $false
    try {
        Assert-Stage5WeeklyPromotionGate ($weeklyFixture.Replace(
            "'QualificationData')) { throw }",
            "'UnreviewedData')) { throw }")) `
            'self-test weekly proprietary-data payload fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted staging without proprietary qualification-data rejection.'
    $executableAcceptanceFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Run Stage 5 final pre-manual acceptance
        shell: pwsh
        run: |
          # Invoke-Stage5FinalAcceptance.ps1 appears in documentation only.
          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" `
            -AcceptanceManifestPath $manifest
'@
    $executableAcceptanceFixture = ConvertTo-Stage5SelfTestLf $executableAcceptanceFixture
    $executableAcceptanceStep = Get-Stage5IndentedBlock `
        $executableAcceptanceFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    Assert-Stage5WorkflowRunBlockIndentation $executableAcceptanceStep 6 8 `
        'self-test scoped executable final acceptance fixture'
    Assert-Stage5WorkflowExecutablePowerShellCall $executableAcceptanceStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
        'self-test scoped executable final acceptance fixture' `
        ([ordered]@{ AcceptanceManifestPath = '$manifest' })
    $commentOnlyAcceptanceFixture = $executableAcceptanceFixture.Replace(
        '          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" `',
        '          # & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" `')
    $commentOnlyAcceptanceStep = Get-Stage5IndentedBlock `
        $commentOnlyAcceptanceFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $commentOnlyAcceptanceStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
            'self-test comment-only final acceptance fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a comment-only final acceptance invocation.'
    $blockCommentOnlyAcceptanceFixture = $executableAcceptanceFixture.Replace(
        '          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" `',
        '          <# & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" ` #>')
    $blockCommentOnlyAcceptanceStep = Get-Stage5IndentedBlock `
        $blockCommentOnlyAcceptanceFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $blockCommentOnlyAcceptanceStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
            'self-test block-comment-only final acceptance fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a block-comment-only final acceptance invocation.'
    $commentOnlyParameterFixture = $executableAcceptanceFixture.Replace(
        '            -AcceptanceManifestPath $manifest',
        '            # -AcceptanceManifestPath $manifest')
    $commentOnlyParameterStep = Get-Stage5IndentedBlock `
        $commentOnlyParameterFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $commentOnlyParameterStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
            'self-test comment-only final acceptance parameter fixture' `
            ([ordered]@{ AcceptanceManifestPath = '$manifest' })
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a required parameter that existed only in a comment.'
    $wrongValueAcceptanceFixture = $executableAcceptanceFixture.Replace(
        '            -AcceptanceManifestPath $manifest',
        "            -AcceptanceManifestPath `$wrongManifest`n          <# expected -AcceptanceManifestPath `$manifest #>")
    $wrongValueAcceptanceStep = Get-Stage5IndentedBlock `
        $wrongValueAcceptanceFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $wrongValueAcceptanceStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
            'self-test wrong-value final acceptance fixture' `
            ([ordered]@{ AcceptanceManifestPath = '$manifest' })
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a wrong active argument hidden by a block-comment literal.'
    $deadCodeAcceptanceFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Run Stage 5 final pre-manual acceptance
        shell: pwsh
        run: |
          if ($false) {
            & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1" `
              -AcceptanceManifestPath $manifest
          }
'@
    $deadCodeAcceptanceStep = Get-Stage5IndentedBlock `
        $deadCodeAcceptanceFixture `
        '- name: Run Stage 5 final pre-manual acceptance' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $deadCodeAcceptanceStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/Invoke-Stage5FinalAcceptance.ps1' `
            'self-test dead-code final acceptance fixture' `
            ([ordered]@{ AcceptanceManifestPath = '$manifest' })
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a final acceptance call inside dead code.'
    $performanceDataCallFixture = @'
jobs:
  stage5-performance-scaling-qualification:
    steps:
      - name: Provision verified Zero Hour performance data
        shell: pwsh
        run: |
          & "$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5PerformanceQualificationData.ps1" `
            -ArchivePath "$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\zerohour104_gamedata_trimmed.7z" `
            -ExpectedArchiveSha256 "6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21" `
            -TaskRoot "$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT" `
            -RuntimeRoot "$env:STAGE5_QUALIFICATION_ROOT\ZeroHourRuntime" `
            -OutputPath "$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\Stage5PerformanceQualificationData.json" `
            -ExpectedSourceCommit $env:GITHUB_SHA.ToLowerInvariant() `
            -SevenZipPath "C:\Program Files\7-Zip\7z.exe"
'@
    $performanceDataCallStep = Get-Stage5IndentedBlock `
        $performanceDataCallFixture `
        '- name: Provision verified Zero Hour performance data' 6
    $performanceDataArguments = [ordered]@{
        ArchivePath = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\zerohour104_gamedata_trimmed.7z"'
        ExpectedArchiveSha256 = '"6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21"'
        TaskRoot = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT"'
        RuntimeRoot = '"$env:STAGE5_QUALIFICATION_ROOT\ZeroHourRuntime"'
        OutputPath = '"$env:STAGE5_PERFORMANCE_DATA_TASK_ROOT\Stage5PerformanceQualificationData.json"'
        ExpectedSourceCommit = '$env:GITHUB_SHA.ToLowerInvariant()'
        SevenZipPath = '"C:\Program Files\7-Zip\7z.exe"'
    }
    Assert-Stage5WorkflowExecutablePowerShellCall $performanceDataCallStep `
        '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5PerformanceQualificationData.ps1' `
        'self-test exact performance-data call fixture' `
        $performanceDataArguments
    $wrongPerformanceDataHashStep = Get-Stage5IndentedBlock `
        ($performanceDataCallFixture.Replace(
            '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
            'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA')) `
        '- name: Provision verified Zero Hour performance data' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $wrongPerformanceDataHashStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5PerformanceQualificationData.ps1' `
            'self-test wrong performance-data archive authority fixture' `
            $performanceDataArguments
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a caller-selected performance-data archive hash.'
    $extraPositionalPerformanceDataStep = Get-Stage5IndentedBlock `
        ($performanceDataCallFixture.Replace(
            '            -SevenZipPath "C:\Program Files\7-Zip\7z.exe"',
            '            -SevenZipPath "C:\Program Files\7-Zip\7z.exe" unexpected')) `
        '- name: Provision verified Zero Hour performance data' 6
    $caught = $false
    try {
        Assert-Stage5WorkflowExecutablePowerShellCall `
            $extraPositionalPerformanceDataStep `
            '$env:GITHUB_WORKSPACE/Core/Tools/DeterministicSimulationValidation/New-Stage5PerformanceQualificationData.ps1' `
            'self-test extra positional performance-data argument fixture' `
            $performanceDataArguments
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted an unbound positional executable argument.'
    $repositoryGuardFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Assemble trusted Stage 5 development-readiness bundle
        shell: pwsh
        run: |
          function Resolve-Stage5RepositoryFile {
            param([string]$Root, [string]$RelativePath, [string]$Context)
            $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
            if ([string]::IsNullOrWhiteSpace($RelativePath) -or
                [IO.Path]::IsPathRooted($RelativePath) -or
                $RelativePath -match ':' -or
                $RelativePath -match '[\x00-\x1F\x7F]' -or
                $RelativePath -match '(^|[\\/])\.{1,2}([\\/]|$)') {
              throw "$Context must be a canonical repository-relative path."
            }
            $rootItem = Get-Item -LiteralPath $rootFull -Force -ErrorAction Stop
            if (-not $rootItem.PSIsContainer -or
                ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
              throw "$Context trusted root is not a regular directory."
            }
            $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath))
            if (-not $candidate.StartsWith(
                  $rootFull + [IO.Path]::DirectorySeparatorChar,
                  [StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
              throw "$Context must resolve to a file below its trusted root."
            }
            $current = $rootFull
            foreach ($segment in @($candidate.Substring($rootFull.Length + 1) -split '[\\/]')) {
              $current = Join-Path $current $segment
              $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
              if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Context contains a reparse-point path component."
              }
            }
            return $candidate
          }
          $templateRoot = [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
          $templatePath = Resolve-Stage5RepositoryFile $templateRoot `
            $env:STAGE5_ACCEPTANCE_TEMPLATE 'Stage 5 acceptance template'
'@
    $repositoryGuardStep = Get-Stage5IndentedBlock $repositoryGuardFixture `
        '- name: Assemble trusted Stage 5 development-readiness bundle' 6
    $requiredRepositoryResolution = [ordered]@{
        '$templatePath' = @('$templateRoot',
            '$env:STAGE5_ACCEPTANCE_TEMPLATE',
            "'Stage 5 acceptance template'")
    }
    Assert-Stage5RepositoryRelativeFileGuard $repositoryGuardStep `
        'self-test repository input guard fixture' $requiredRepositoryResolution
    foreach ($invalidRepositoryGuardFixture in @(
        $repositoryGuardFixture.Replace(
            '[IO.Path]::IsPathRooted($RelativePath)',
            '[IO.Path]::IsPathFullyQualified($RelativePath)'),
        $repositoryGuardFixture.Replace(
            "`$RelativePath -match '(^|[\\/])\.{1,2}([\\/]|`$)'",
            "`$RelativePath -match '(^|[\\/])([\\/]|`$)'"),
        $repositoryGuardFixture.Replace(
            '[IO.FileAttributes]::ReparsePoint',
            '[IO.FileAttributes]::Hidden'),
        $repositoryGuardFixture.Replace(
            'Resolve-Stage5RepositoryFile $templateRoot',
            "Resolve-Stage5RepositoryFile 'C:\\'"))) {
        $invalidRepositoryGuardStep = Get-Stage5IndentedBlock `
            $invalidRepositoryGuardFixture `
            '- name: Assemble trusted Stage 5 development-readiness bundle' 6
        $caught = $false
        try {
            Assert-Stage5RepositoryRelativeFileGuard $invalidRepositoryGuardStep `
                'self-test invalid repository input guard fixture' `
                $requiredRepositoryResolution
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a rooted, dot-segment, reparse, or wrongly rooted repository input guard.'
    }
    $cohortMintFixture = @'
jobs:
  stage5-execution-cohort:
    steps:
      - name: Mint fresh execution cohort
        shell: pwsh
        run: |
          $nonce = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
          $created = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
          "nonce=$nonce" >> $env:GITHUB_OUTPUT
          "created_utc=$created" >> $env:GITHUB_OUTPUT
'@
    $cohortMintFixture = ConvertTo-Stage5SelfTestLf $cohortMintFixture
    $cohortMintStep = Get-Stage5IndentedBlock $cohortMintFixture `
        '- name: Mint fresh execution cohort' 6
    $cohortStatements = @(
        "`$nonce = [Guid]::NewGuid().ToString('D').ToLowerInvariant()",
        "`$created = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)",
        '"nonce=$nonce" >> $env:GITHUB_OUTPUT',
        '"created_utc=$created" >> $env:GITHUB_OUTPUT')
    Assert-Stage5ExactTopLevelPowerShellStatements $cohortMintStep `
        $cohortStatements 'self-test exact cohort mint fixture'
    $staleCohortMintStep = Get-Stage5IndentedBlock `
        ($cohortMintFixture.Replace(
            '          "nonce=$nonce" >> $env:GITHUB_OUTPUT',
            "          `$nonce = `$env:REUSED_COHORT_NONCE`n          `"nonce=`$nonce`" >> `$env:GITHUB_OUTPUT")) `
        '- name: Mint fresh execution cohort' 6
    $caught = $false
    try {
        Assert-Stage5ExactTopLevelPowerShellStatements $staleCohortMintStep `
            $cohortStatements 'self-test overwritten cohort mint fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted an overwritten fresh cohort output.'

    $closedProgramFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Trusted producer
        shell: pwsh
        run: |
          $ErrorActionPreference = 'Stop'
          & 'trusted.ps1' -OutputPath $env:OUTPUT
          if (-not $?) { throw 'failed' }
'@
    $closedProgramFixture = ConvertTo-Stage5SelfTestLf $closedProgramFixture
    $closedProgramStep = Get-Stage5IndentedBlock $closedProgramFixture `
        '- name: Trusted producer' 6
    Assert-Stage5ExactPowerShellStepMetadata -Step $closedProgramStep `
        -ExpectedEnvironment $null -Context `
        'self-test exact trusted-producer metadata fixture'
    Assert-Stage5ClosedPowerShellRunBlock $closedProgramStep `
        'A56CDEF6A77F554D13110AB978400DBAFFC5EE15CCCA811C083EFC37E8BEDCB9' 3 `
        'self-test exact closed producer fixture'
    $mutatedClosedProgramStep = Get-Stage5IndentedBlock `
        ($closedProgramFixture.Replace(
            "          if (-not `$?) { throw 'failed' }",
            "          if (-not `$?) { throw 'failed' }`n          [IO.File]::WriteAllText(`$env:OUTPUT, 'replacement')")) `
        '- name: Trusted producer' 6
    $caught = $false
    try {
        Assert-Stage5ClosedPowerShellRunBlock $mutatedClosedProgramStep `
            'A56CDEF6A77F554D13110AB978400DBAFFC5EE15CCCA811C083EFC37E8BEDCB9' 3 `
            'self-test post-call mutation fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a post-call artifact replacement.'
    $githubExpressionProgramFixture = @'
jobs:
  stage5-combined-host-runner:
    steps:
      - name: Bind current-run evidence
        shell: pwsh
        run: |
          $inputPath = "${{ runner.temp }}\evidence"
'@
    $githubExpressionProgramStep = Get-Stage5IndentedBlock `
        $githubExpressionProgramFixture '- name: Bind current-run evidence' 6
    Assert-Stage5ClosedPowerShellRunBlock $githubExpressionProgramStep `
        '2E5AA0B81827E1A27DC0F55AF3307E67058C12B295A5E187231900149A797A61' 1 `
        'self-test exact GitHub-expression program fixture'
    $staleExpressionProgramStep = Get-Stage5IndentedBlock `
        ($githubExpressionProgramFixture.Replace(
            '${{ runner.temp }}', '${{ github.workspace }}')) `
        '- name: Bind current-run evidence' 6
    $caught = $false
    try {
        Assert-Stage5ClosedPowerShellRunBlock $staleExpressionProgramStep `
            '2E5AA0B81827E1A27DC0F55AF3307E67058C12B295A5E187231900149A797A61' 1 `
            'self-test replaced GitHub-expression program fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a changed current-run GitHub expression.'
    foreach ($invalidMetadataStep in @(
        $closedProgramStep.Replace('        shell: pwsh', '        shell: bash'),
        $closedProgramStep.Replace('        shell: pwsh',
            "        shell: pwsh`n        working-directory: C:\stale"),
        $closedProgramStep.Replace('        shell: pwsh',
            "        shell: pwsh`n        env:`n          STAGE5_COHORT_NONCE: stale-cohort"),
        $closedProgramStep.Replace('        shell: pwsh',
            "        shell: pwsh`n        'env':`n          STAGE5_COHORT_NONCE: stale-cohort"),
        $closedProgramStep.Replace('        shell: pwsh',
            "        shell: pwsh`n        'continue-on-error': true"))) {
        $caught = $false
        try {
            Assert-Stage5ExactPowerShellStepMetadata -Step $invalidMetadataStep `
                -ExpectedEnvironment $null -Context `
                'self-test unsafe trusted-producer metadata fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a wrong shell, working directory, quoted key, or protected step environment override.'
    }

    $closedStepSequenceFixture = @'
  stage5-development-readiness:
    steps:
      - name: Download evidence
        uses: action/download@0000000000000000000000000000000000000000
      - name: Assemble evidence
        run: Write-Output assemble
      - name: Validate evidence
        run: Write-Output validate
      - name: Upload evidence
        uses: action/upload@0000000000000000000000000000000000000000
'@
    $closedStepSequenceFixture = ConvertTo-Stage5SelfTestLf $closedStepSequenceFixture
    Assert-Stage5ExactJobStepSequence $closedStepSequenceFixture @(
        'Download evidence', 'Assemble evidence', 'Validate evidence',
        'Upload evidence') @('uses', 'run', 'run', 'uses') `
        'self-test exact closed job sequence fixture'
    $closedStepSequenceCrlfFixture = [regex]::Replace(
        $closedStepSequenceFixture, "`r`n|`r|`n", "`r`n")
    Assert-Stage5ExactJobStepSequence $closedStepSequenceCrlfFixture @(
        'Download evidence', 'Assemble evidence', 'Validate evidence',
        'Upload evidence') @('uses', 'run', 'run', 'uses') `
        'self-test CRLF closed job sequence fixture'
    foreach ($invalidClosedSequenceFixture in @(
        $closedStepSequenceFixture.Replace(
            '      - name: Assemble evidence',
            "      - name: Mutate downloaded evidence`n        run: Write-Output mutate`n      - name: Assemble evidence"),
        $closedStepSequenceFixture.Replace(
            '      - name: Assemble evidence',
            "      - run: Write-Output mutate`n      - name: Assemble evidence"))) {
        $caught = $false
        try {
            Assert-Stage5ExactJobStepSequence $invalidClosedSequenceFixture @(
                'Download evidence', 'Assemble evidence', 'Validate evidence',
                'Upload evidence') @('uses', 'run', 'run', 'uses') `
                'self-test inserted pre-consumer step fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted an inserted pre-consumer step.'
    }

    $jobEnvironmentFixture = @'
  stage5-development-readiness:
    env:
      STAGE5_COHORT_NONCE: current-cohort
      STAGE5_READINESS_ROOT: 'H:\Stage5WeeklyPromotionQualification'
    steps:
      - name: Validate
        run: Write-Output validate
'@
    $jobEnvironmentFixture = ConvertTo-Stage5SelfTestLf $jobEnvironmentFixture
    $expectedJobEnvironment = [ordered]@{
        STAGE5_COHORT_NONCE = 'current-cohort'
        STAGE5_READINESS_ROOT = "'H:\Stage5WeeklyPromotionQualification'"
    }
    Assert-Stage5ExactJobEnvironment $jobEnvironmentFixture `
        $expectedJobEnvironment 'self-test exact job environment fixture'
    $duplicateJobEnvironmentFixture = $jobEnvironmentFixture.Replace(
        '      STAGE5_COHORT_NONCE: current-cohort',
        "      STAGE5_COHORT_NONCE: current-cohort`n      STAGE5_COHORT_NONCE: stale-cohort")
    $caught = $false
    try {
        Assert-Stage5ExactJobEnvironment $duplicateJobEnvironmentFixture `
            $expectedJobEnvironment 'self-test duplicate job environment fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a duplicate stale job environment binding.'
    $swappedJobEnvironmentFixture = $jobEnvironmentFixture.Replace(
        "      STAGE5_COHORT_NONCE: current-cohort`n      STAGE5_READINESS_ROOT: 'H:\Stage5WeeklyPromotionQualification'",
        "      STAGE5_READINESS_ROOT: 'H:\Stage5WeeklyPromotionQualification'`n      STAGE5_COHORT_NONCE: current-cohort")
    $caught = $false
    try {
        Assert-Stage5ExactJobEnvironment $swappedJobEnvironmentFixture `
            $expectedJobEnvironment 'self-test reordered job environment fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted reordered job environment bindings.'
    foreach ($jobDefaultsFixture in @(
        $jobEnvironmentFixture.Replace(
            '    env:', "    defaults:`n      run:`n        working-directory: C:\stale`n    env:"),
        $jobEnvironmentFixture.Replace(
            '    env:', "    'defaults':`n      run:`n        working-directory: C:\stale`n    env:"))) {
        $caught = $false
        try {
            Assert-Stage5NoJobRunDefaults $jobDefaultsFixture `
                'self-test job run-default fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted plain or quoted job-level run working-directory defaults.'
    }

    $currentRunDownloadFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Download exact evidence
        uses: actions/download-artifact@70fc10c6e5e1ce46ad2ea6f2b72d43f7d47b13c3
        with:
          name: Stage5-Exact-Evidence
          path: H:\Stage5ExactEvidence
'@
    $currentRunDownloadFixture = ConvertTo-Stage5SelfTestLf $currentRunDownloadFixture
    $currentRunDownloadStep = Get-Stage5IndentedBlock `
        $currentRunDownloadFixture '- name: Download exact evidence' 6
    Assert-Stage5CurrentRunArtifactDownload $currentRunDownloadStep `
        'Stage5-Exact-Evidence' 'H:\Stage5ExactEvidence' `
        'self-test current-run artifact fixture'
    foreach ($priorRunDownloadFixture in @(
        $currentRunDownloadFixture.Replace(
            '          path: H:\Stage5ExactEvidence',
            "          path: H:\Stage5ExactEvidence`n          run-id: 12345`n          github-token: `${{ secrets.GITHUB_TOKEN }}"),
        $currentRunDownloadFixture.Replace(
            '          path: H:\Stage5ExactEvidence',
            "          path: H:\Stage5ExactEvidence`n          'run-id': 12345"))) {
        $priorRunDownloadStep = Get-Stage5IndentedBlock `
            $priorRunDownloadFixture '- name: Download exact evidence' 6
        $caught = $false
        try {
            Assert-Stage5CurrentRunArtifactDownload $priorRunDownloadStep `
                'Stage5-Exact-Evidence' 'H:\Stage5ExactEvidence' `
                'self-test stale-run artifact fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a plain or quoted artifact download from another run.'
    }

    $exactConditionFixture = @'
    if: >-
      ${{ github.event_name == 'workflow_dispatch' &&
          inputs.stage5_external_performance_qualification == true }}
'@
    Assert-Stage5ExactFoldedJobCondition $exactConditionFixture `
        "github.event_name == 'workflow_dispatch' && inputs.stage5_external_performance_qualification == true" `
        'self-test exact job condition fixture'
    $caught = $false
    try {
        Assert-Stage5ExactFoldedJobCondition `
            ($exactConditionFixture.Replace(
                'inputs.stage5_external_performance_qualification == true',
                '!(false && inputs.stage5_external_performance_qualification == true)')) `
            "github.event_name == 'workflow_dispatch' && inputs.stage5_external_performance_qualification == true" `
            'self-test negated job condition fixture'
    }
    catch { $caught = $true }
    Assert-Stage5WorkflowCondition $caught `
        'workflow contract self-test accepted a semantically bypassed job condition.'

    foreach ($unsafeStepProperty in @(
        'if: false',
        'continue-on-error: true',
        "'if': false",
        "'continue-on-error': true")) {
        $unsafeAcceptanceStep = $executableAcceptanceStep.Replace(
            '        shell: pwsh', "        shell: pwsh`n        $unsafeStepProperty")
        $caught = $false
        try {
            Assert-Stage5WorkflowFailClosedStep $unsafeAcceptanceStep `
                'self-test conditional acceptance step fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a conditional or continue-on-error readiness step.'
    }

    $sealedUploadFixture = @'
jobs:
  stage5-development-readiness:
    steps:
      - name: Upload Stage 5 development-readiness evidence
        uses: actions/upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f
        with:
          name: Stage5-Development-Readiness
          path: H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip
          retention-days: 30
          if-no-files-found: error
'@
    $sealedUploadFixture = ConvertTo-Stage5SelfTestLf $sealedUploadFixture
    $sealedUploadStep = Get-Stage5IndentedBlock $sealedUploadFixture `
        '- name: Upload Stage 5 development-readiness evidence' 6
    Assert-Stage5SealedReadinessUpload $sealedUploadStep `
        'self-test sealed readiness upload fixture'
    foreach ($invalidUploadFixture in @(
        $sealedUploadFixture.Replace(
            '          path: H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip',
            '          path: H:\Stage5WeeklyPromotionQualification'),
        $sealedUploadFixture.Replace(
            '          path: H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip',
            "          path: |`n            H:\Stage5WeeklyPromotionQualification"),
        $sealedUploadFixture.Replace(
            '          path: H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip',
            "          path: H:\Stage5WeeklyPromotionQualification\Stage5DevelopmentReadinessBundle.zip`n          path: H:\Stale\Stage5DevelopmentReadinessBundle.zip"))) {
        $invalidUploadStep = Get-Stage5IndentedBlock $invalidUploadFixture `
            '- name: Upload Stage 5 development-readiness evidence' 6
        $caught = $false
        try {
            Assert-Stage5SealedReadinessUpload $invalidUploadStep `
                'self-test broad readiness upload fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted a broad mutable readiness upload.'
    }

    $terminalOrderFixture = @'
  stage5-development-readiness:
    steps:
      - name: Download evidence
      - name: Assemble trusted Stage 5 development-readiness bundle
      - name: Run Stage 5 final pre-manual acceptance
      - name: Seal Stage 5 development-readiness bundle
      - name: Upload Stage 5 development-readiness evidence
'@
    $terminalOrderFixture = ConvertTo-Stage5SelfTestLf $terminalOrderFixture
    Assert-Stage5ReadinessTerminalStepOrder $terminalOrderFixture `
        'self-test readiness terminal order fixture'
    foreach ($invalidTerminalOrderFixture in @(
        $terminalOrderFixture.Replace(
            '      - name: Seal Stage 5 development-readiness bundle',
            "      - name: Mutate accepted evidence`n      - name: Seal Stage 5 development-readiness bundle"),
        $terminalOrderFixture.Replace(
            '      - name: Seal Stage 5 development-readiness bundle',
            "      - run: Write-Output bypass`n      - name: Seal Stage 5 development-readiness bundle"))) {
        $caught = $false
        try {
            Assert-Stage5ReadinessTerminalStepOrder $invalidTerminalOrderFixture `
                'self-test intervening readiness step fixture'
        }
        catch { $caught = $true }
        Assert-Stage5WorkflowCondition $caught `
            'workflow contract self-test accepted an intervening readiness step.'
    }

    Invoke-Stage5CheckReplaysContractSelfTest
    Write-Output 'Stage 5 workflow contract self-test passed.'
    return
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$root = [IO.Path]::GetFullPath($SourceRoot)
$ci = Get-Stage5WorkflowFile $root '.github/workflows/ci.yml'
$check = Get-Stage5WorkflowFile $root '.github/workflows/check-replays.yml'
$buildToolchain = Get-Stage5WorkflowFile $root `
    '.github/workflows/build-toolchain.yml'
$weekly = Get-Stage5WorkflowFile $root '.github/workflows/weekly-release.yml'
$validationVolumeHelper = Get-Stage5WorkflowFile $root `
    '.github/workflows/Stage5ValidationVolume.ps1'
Assert-Stage5ValidationVolumeHelper $validationVolumeHelper `
    'Stage 5 shared validation-volume helper'
$weeklyPromotionValidator = Join-Path $root `
    '.github/workflows/Validate-Stage5WeeklyPromotionAttestation.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $weeklyPromotionValidator -PathType Leaf) `
    'Stage 5 weekly promotion attestation validator is missing.'
$weeklyPromotionValidatorContent = Get-Content -LiteralPath `
    $weeklyPromotionValidator -Raw
$lockstepV2Qualifier = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Invoke-InstalledLockstepV2Validation.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $lockstepV2Qualifier -PathType Leaf) `
    'Installed lockstep-v2 qualifier is missing.'
$lockstepV2QualifierContent = Get-Content -LiteralPath $lockstepV2Qualifier -Raw
$runtimeManifestProducer = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\New-Stage5RuntimeManifests.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $runtimeManifestProducer -PathType Leaf) `
    'Trusted Stage 5 runtime-manifest producer is missing.'
$qualificationDataProducer = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Install-Stage5QualificationData.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $qualificationDataProducer -PathType Leaf) `
    'Trusted Stage 5 qualification-data producer is missing.'
$performanceDataProducer = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\New-Stage5PerformanceQualificationData.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $performanceDataProducer -PathType Leaf) `
    'Trusted Stage 5 performance-data producer is missing.'
$normalizer = Join-Path $root '.github/workflows/Normalize-Stage5EvidenceForUpload.ps1'
Assert-Stage5WorkflowCondition (Test-Path -LiteralPath $normalizer -PathType Leaf) `
    'Stage 5 evidence normalizer is missing.'
$normalizerContent = Get-Content -LiteralPath $normalizer -Raw
$readinessAssembler = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\New-Stage5DevelopmentReadinessBundle.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $readinessAssembler -PathType Leaf) `
    'Stage 5 development-readiness bundle assembler is missing.'
$readinessSealer = Join-Path $root `
    'Core\Tools\DeterministicSimulationValidation\Seal-Stage5DevelopmentReadinessBundle.ps1'
Assert-Stage5WorkflowCondition `
    (Test-Path -LiteralPath $readinessSealer -PathType Leaf) `
    'Stage 5 development-readiness bundle sealer is missing.'

Assert-Stage5WorkflowContains $ci `
    'stage5:\s*\$\{\{\s*steps\.filter\.outputs\.stage5\s*\}\}' `
    'CI change detector output'
$stage5Filter = Get-Stage5IndentedBlock $ci 'stage5:' 12
Assert-Stage5WorkflowLiteral $stage5Filter ".github/workflows/**" 'CI Stage 5 path filter'
Assert-Stage5WorkflowLiteral $stage5Filter 'Core/**' 'CI Stage 5 path filter'
Assert-Stage5WorkflowLiteral $stage5Filter 'Generals/**' 'CI Stage 5 path filter'

$workflowContractJob = Get-Stage5IndentedBlock $ci 'stage5-workflow-contract:' 2
Assert-Stage5WorkflowContractInvocation $ci `
    'CI Stage 5 workflow-contract invocation'
Assert-Stage5WorkflowLiteral $workflowContractJob 'needs: detect-changes' `
    'CI workflow contract job'
Assert-Stage5WorkflowContains $workflowContractJob `
    'if:\s+\$\{\{\s*github\.event_name\s*==\s*.workflow_dispatch.\s*\|\|\s*needs\.detect-changes\.outputs\.stage5\s*==\s*.true.\s*\}\}' `
    'CI workflow contract gate condition'
Assert-Stage5WorkflowContains $ci 'stage5_acceptance_manifest:' `
    'CI final acceptance input'
Assert-Stage5WorkflowContains $ci 'stage5-execution-cohort:' `
    'CI fresh execution cohort producer'
$executionCohortJob = Get-Stage5IndentedBlock $ci 'stage5-execution-cohort:' 2
Assert-Stage5ExactJobSchema $executionCohortJob ([ordered]@{
        name = 'Stage 5 Fresh Execution Cohort'
        needs = 'detect-changes'
        'if' = '>-'
        'runs-on' = 'windows-2022'
        'timeout-minutes' = '5'
        outputs = ''
        steps = ''
    }) 'CI exact Stage 5 execution-cohort job schema'
$exactLockstepJob = Get-Stage5IndentedBlock $ci 'stage5-lockstep-v2-qualification:' 2
Assert-Stage5ExactJobSchema $exactLockstepJob ([ordered]@{
        name = 'Stage 5 Installed Lockstep-v2 Release Qualification'
        needs = '[build-generals-x64, build-generalsmd-x64, stage5-execution-cohort]'
        'if' = '>-'
        'runs-on' = 'windows-2022'
        'timeout-minutes' = '90'
        env = ''
        steps = ''
    }) 'CI exact Stage 5 lockstep-v2 job schema'
$exactPerformanceJob = Get-Stage5IndentedBlock $ci `
    'stage5-performance-scaling-qualification:' 2
Assert-Stage5ExactJobSchema $exactPerformanceJob ([ordered]@{
        name = 'Stage 5 External 16-Core Performance Qualification'
        needs = '[build-generals-x64, build-generalsmd-x64, stage5-execution-cohort, stage5-lockstep-v2-qualification]'
        'if' = '>-'
        'runs-on' = '[self-hosted, windows, x64, stage5-16-physical-core]'
        'timeout-minutes' = '360'
        concurrency = ''
        env = ''
        steps = ''
    }) 'CI exact Stage 5 performance job schema'
$exactCombinedJob = Get-Stage5IndentedBlock $ci 'stage5-combined-host-runner:' 2
Assert-Stage5ExactJobSchema $exactCombinedJob ([ordered]@{
        name = 'Stage 5 Combined Host-Runner Receipt'
        needs = '[stage5-execution-cohort, stage5-replaycheck-generalsmd-x64, stage5-replaycheck-generals-x64]'
        'if' = '>-'
        'runs-on' = 'windows-2022'
        'timeout-minutes' = '15'
        env = ''
        steps = ''
    }) 'CI exact Stage 5 combined job schema'
$exactReadinessJob = Get-Stage5IndentedBlock $ci 'stage5-development-readiness:' 2
Assert-Stage5ExactJobSchema $exactReadinessJob ([ordered]@{
        name = 'Stage 5 Development Readiness'
        needs = '[stage5-execution-cohort, stage5-replaycheck-generalsmd-x64, stage5-replaycheck-generals-x64, stage5-combined-host-runner, stage5-lockstep-v2-qualification, stage5-performance-scaling-qualification]'
        'if' = '>-'
        'runs-on' = 'windows-2022'
        'timeout-minutes' = '30'
        env = ''
        steps = ''
    }) 'CI exact Stage 5 development-readiness job schema'
Assert-Stage5ExecutionCohortProducer $ci `
    'CI shared Stage 5 execution cohort producer'
Assert-Stage5CombinedHostRunnerProducer $ci `
    'CI combined host-runner producer'
Assert-Stage5LockstepV2QualificationProducer $ci `
    'CI installed lockstep-v2 qualification producer'
Assert-Stage5ExternalPerformanceQualificationProducer $ci `
    'CI external performance qualification producer'
Assert-Stage5DevelopmentReadinessProducer $ci `
    'CI Stage 5 development-readiness producer'
Assert-Stage5ValidationVolumeBinding $exactLockstepJob `
    'Provision canonical Stage 5 qualification root' `
    'Clean Stage 5 lockstep-v2 validation volume' '' '${{ always() }}' `
    '(?s)lockstep-v2.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
    @('Upload installed lockstep-v2 qualification') $null `
    'CI Stage 5 lockstep-v2 task-owned validation volume'
Assert-Stage5ValidationVolumeBinding $exactCombinedJob `
    'Provision isolated Stage 5 volume' `
    'Clean Stage 5 combined host-runner validation volume' '' '${{ always() }}' `
    '(?s)combined-host-runner.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
    @('Upload combined Stage 5 evidence') $null `
    'CI Stage 5 combined task-owned validation volume'
Assert-Stage5ValidationVolumeBinding $exactReadinessJob `
    'Provision isolated Stage 5 readiness volume' `
    'Clean Stage 5 development-readiness validation volume' '' '${{ always() }}' `
    '(?s)development-readiness.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
    @('Upload Stage 5 development-readiness evidence') $null `
    'CI Stage 5 readiness task-owned validation volume'
Assert-Stage5ProductArtifactIsolation $buildToolchain `
    'reusable native product build workflow'
Assert-Stage5ValidationVolumeBinding $buildToolchain `
    'Provision Stage 5 validation scratch' `
    'Clean Stage 5 validation scratch volume' '${{ inputs.extras }}' `
    '${{ always() && inputs.extras }}' `
    '__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__' `
    @('Upload ${{ inputs.game }} ${{ inputs.preset }}${{ inputs.tools && ''+t'' || '''' }}${{ inputs.extras && ''+e'' || '''' }} Artifact') `
    $null 'reusable native product build workflow'
Assert-Stage5WorkflowLiteral $workflowContractJob `
    'Validate-Stage5WeeklyPromotionAttestation.ps1' `
    'CI weekly promotion validator self-test'
$combinedStage5Job = Get-Stage5IndentedBlock $ci 'stage5-combined-host-runner:' 2
Assert-Stage5CanonicalDownloadArtifactPins $combinedStage5Job 2 `
    'CI combined host-runner artifact downloads'
$generalsX64Build = Get-Stage5IndentedBlock $ci 'build-generals-x64:' 2
Assert-Stage5WorkflowContains $generalsX64Build 'outputs\.stage5\s*==\s*.true.' `
    'Generals x64 build Stage 5 prerequisite'
$generalsMdX64Build = Get-Stage5IndentedBlock $ci 'build-generalsmd-x64:' 2
Assert-Stage5WorkflowContains $generalsMdX64Build 'outputs\.stage5\s*==\s*.true.' `
    'GeneralsMD x64 build Stage 5 prerequisite'
$zeroHourStage5Job = Get-Stage5IndentedBlock $ci 'stage5-replaycheck-generalsmd-x64:' 2
Assert-Stage5WorkflowLiteral $zeroHourStage5Job 'needs: [detect-changes, build-generalsmd-x64, stage5-execution-cohort]' `
    'Zero Hour Stage 5 build dependency'
$zeroHourCondition = Get-Stage5IndentedBlock $zeroHourStage5Job 'if: >-' 4
Assert-Stage5ExactFoldedJobCondition $zeroHourCondition `
    ("github.event_name == 'workflow_dispatch' && " +
        "inputs.stage5_fixture_manifest != '' && " +
        "inputs.stage5_performance_baseline != '' && " +
        "inputs.stage5_expected_stage3_executable_sha256 != '' && " +
        "inputs.stage5_acceptance_manifest != ''") `
    'Zero Hour Stage 5 exact evidence-input gate'
Assert-Stage5WorkflowLiteral $zeroHourStage5Job `
    'uses: ./.github/workflows/check-replays.yml' `
    'Zero Hour Stage 5 local reusable workflow'
foreach ($cohortBinding in @(
    'stage5_execution_cohort_nonce: ${{ needs[''stage5-execution-cohort''].outputs.nonce }}',
    'stage5_execution_cohort_created_utc: ${{ needs[''stage5-execution-cohort''].outputs.created_utc }}')) {
    Assert-Stage5WorkflowCondition `
        ([regex]::Matches($zeroHourStage5Job,
            [regex]::Escape($cohortBinding)).Count -eq 1) `
        'Zero Hour Stage 5 exact fresh cohort binding'
}
$generalsStage5Job = Get-Stage5IndentedBlock $ci 'stage5-replaycheck-generals-x64:' 2
Assert-Stage5WorkflowLiteral $generalsStage5Job 'needs: [detect-changes, build-generals-x64, build-generalsmd-x64, stage5-execution-cohort]' `
    'Generals Stage 5 build dependency'
$generalsCondition = Get-Stage5IndentedBlock $generalsStage5Job 'if: >-' 4
Assert-Stage5ExactFoldedJobCondition $generalsCondition `
    ("github.event_name == 'workflow_dispatch' && " +
        "inputs.stage5_generals_fixture_manifest != '' && " +
        "inputs.stage5_generals_performance_baseline != '' && " +
        "inputs.stage5_generals_expected_stage3_executable_sha256 != '' && " +
        "inputs.stage5_acceptance_manifest != ''") `
    'Generals Stage 5 exact evidence-input gate'
Assert-Stage5WorkflowLiteral $generalsStage5Job `
    'uses: ./.github/workflows/check-replays.yml' `
    'Generals Stage 5 local reusable workflow'
foreach ($cohortBinding in @(
    'stage5_execution_cohort_nonce: ${{ needs[''stage5-execution-cohort''].outputs.nonce }}',
    'stage5_execution_cohort_created_utc: ${{ needs[''stage5-execution-cohort''].outputs.created_utc }}')) {
    Assert-Stage5WorkflowCondition `
        ([regex]::Matches($generalsStage5Job,
            [regex]::Escape($cohortBinding)).Count -eq 1) `
        'Generals Stage 5 exact fresh cohort binding'
}
$cohortJob = Get-Stage5IndentedBlock $ci 'stage5-execution-cohort:' 2
Assert-Stage5WorkflowLiteral $cohortJob `
    "[Guid]::NewGuid().ToString('D').ToLowerInvariant()" `
    'Stage 5 canonical lowercase cohort UUID producer'
Assert-Stage5WorkflowLiteral $cohortJob `
    "[DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)" `
    'Stage 5 canonical UTC cohort timestamp producer'
foreach ($retiredLane in @(
    'vc6-generals-oracle',
    'vc6-zerohour-oracle',
    'build-generalsmd-vc6:',
    'replaycheck-generalsmd:'
)) {
    Assert-Stage5WorkflowNotContains $ci ([regex]::Escape($retiredLane)) `
        "retired 32-bit product lane $retiredLane"
}

Assert-Stage5CheckReplaysContract $check `
    'reusable Stage 5 installed-runtime replay workflow'
$checkJob = Get-Stage5IndentedBlock $check 'build:' 2
$checkVolumeProvisionEnvironment = [ordered]@{
    AWS_ACCESS_KEY_ID = '${{ secrets.R2_ACCESS_KEY_ID }}'
    AWS_SECRET_ACCESS_KEY = '${{ secrets.R2_SECRET_ACCESS_KEY }}'
    AWS_ENDPOINT_URL = '${{ secrets.R2_ENDPOINT_URL }}'
    STAGE5_GAME = '${{ inputs.game }}'
}
Assert-Stage5ValidationVolumeBinding $checkJob `
    'Provision immutable Stage 5 simulation qualification data' `
    'Clean Stage 5 validation volume' '' '${{ always() && inputs.stage5 }}' `
    '(?s)__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__-__STAGE5_GITHUB_EXPRESSION__' `
    @('Upload Debug Log', 'Upload Stage 5 Validation Evidence') `
    $checkVolumeProvisionEnvironment 'reusable Stage 5 replay validation volume'
Assert-Stage5WorkflowContains $check 'stage5_acceptance_manifest:' `
    'reusable Stage 5 final acceptance input'
$stage5Input = Get-Stage5IndentedBlock $check 'stage5:' 6
Assert-Stage5WorkflowContains $stage5Input 'required:\s*true' `
    'reusable Stage 5 required input'
Assert-Stage5WorkflowNotContains $stage5Input 'default:' `
    'reusable Stage 5 required input'
Assert-Stage5WorkflowContains $check '(?m)^\s*timeout-minutes:\s*240\s*$' `
    'reusable Stage 5 timeout'
foreach ($retiredReplayMarker in @(
    '!inputs.stage5',
    'Run Replay Compatibility Tests',
    'legacy replay executable',
    'build/generalszh.exe',
    'inputs.stage5 && 240 || 15',
    'inputs.userdata'
)) {
    Assert-Stage5WorkflowNotContains $check ([regex]::Escape($retiredReplayMarker)) `
        "retired legacy replay branch marker $retiredReplayMarker"
}
Assert-Stage5CanonicalDownloadArtifactPins $check 3 `
    'reusable Stage 5 installed-runtime artifact download'
Assert-Stage5WorkflowContains $check 'stage5_execution_cohort_nonce:' `
    'reusable Stage 5 fresh execution cohort nonce input'
Assert-Stage5WorkflowContains $check 'stage5_execution_cohort_created_utc:' `
    'reusable Stage 5 cohort creation timestamp input'
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
    "(?m)^\s*-ValidationSet\s+'All'" `
    'full qualification ValidationSet'
Assert-Stage5WorkflowContains $check `
    'STAGE5_PERFORMANCE_BASELINE' `
    'full qualification performance inputs'
Assert-Stage5WorkflowContains $check `
    'Normalize-Stage5EvidenceForUpload\.ps1' `
    'evidence upload normalizer invocation'
Assert-Stage5WorkflowContains $check 'ExecutionCohortNonce' `
    'Stage 5 runner cohort nonce binding'
Assert-Stage5WorkflowContains $check 'ExecutionCohortCreatedUtc' `
    'Stage 5 runner cohort timestamp binding'
Assert-Stage5WorkflowContains $check 'AcceptanceRuntimeDependencyManifestSha256' `
    'Stage 5 runner runtime dependency-manifest binding'
Assert-Stage5WorkflowContains $check 'AcceptanceRuntimeClosureSha256' `
    'Stage 5 runner runtime-closure binding'
Assert-Stage5WorkflowContains $check `
    'path:\s+\$\{\{\s*runner\.temp\s*\}\}\\Stage5SimulationValidation-upload' `
    'normalized evidence upload path'
Assert-Stage5WorkflowNotContains $check 'ReplayFixtureManifest\.example\.json' `
    'reusable Stage 5 workflow'

$stage5Block = Get-Stage5IndentedBlock $check '- name: Run Stage 5 Installed-Runtime Replay Matrix' 6
Assert-Stage5WorkflowCondition (-not [string]::IsNullOrWhiteSpace($stage5Block)) `
    'Stage 5 runner block could not be isolated.'
Assert-Stage5WorkflowRunBlockIndentation $stage5Block 6 8 `
    'Stage 5 installed-runtime replay step'
Assert-Stage5WorkflowNotContains $stage5Block "-ValidationSet 'Replay'" `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowNotContains $stage5Block '-AllowNonStandardCorpus' `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowNotContains $stage5Block '-DiagnosticNonAcceptance' `
    'Stage 5 full qualification runner'
Assert-Stage5WorkflowLiteral $stage5Block "`$taskRoot = 'H:\Stage5SimulationValidationTask'" `
    'Stage 5 H-resident task root'
Assert-Stage5WorkflowLiteral $stage5Block '-TaskRoot $taskRoot' `
    'Stage 5 explicit task-root argument'
Assert-Stage5WorkflowLiteral $stage5Block '-AllowHeadlessDirectExecution' `
    'Stage 5 explicit hosted-runner execution exception'

$normalizerBlock = Get-Stage5IndentedBlock $check '- name: Normalize Stage 5 Evidence Paths for Upload' 6
Assert-Stage5WorkflowLiteral $normalizerBlock `
    '-InputRoot ''H:\Stage5SimulationValidationTask\Evidence''' `
    'Stage 5 evidence normalizer H-resident input'
Assert-Stage5WorkflowContains $normalizerContent 'byte preserving' `
    'Stage 5 immutable evidence upload normalizer'
Assert-Stage5WorkflowNotContains $normalizerContent 'ConvertFrom-Json|WriteAllText' `
    'Stage 5 immutable evidence upload normalizer'

Assert-Stage5CanonicalDownloadArtifactPins $weekly 5 `
    'weekly release artifact downloads'
Assert-Stage5WeeklyPromotionGate $weekly 'weekly release'
$weeklyPromotionJob = Get-Stage5IndentedBlock $weekly `
    'validate-promotion-attestation:' 2
Assert-Stage5ValidationVolumeBinding $weeklyPromotionJob `
    'Provision canonical qualification root' `
    'Clean Stage 5 weekly promotion validation volume' '' '${{ always() }}' `
    '(?s)weekly-promotion.*GITHUB_RUN_ID.*GITHUB_RUN_ATTEMPT' `
    @('Upload validated Generals x64 artifact',
      'Upload validated GeneralsMD x64 artifact') $null 'weekly release task-owned validation volume'
foreach ($fullEvidenceBinding in @(
    "'H:\Stage5WeeklyPromotionQualification'",
    "'Stage5QualificationData.json'",
    "'Evidence\LockstepV2LoopbackEvidence.json'",
    "'Evidence\mixed-worker-multiplayer.json'",
    "'Evidence\Stage5LockstepV2EvidenceClosure.json'",
    "'QualificationData.json'",
    'Assert-PromotionQualificationDataManifest',
    '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372',
    '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
    'Assert-PromotionEvidenceTree',
    "'lockstep-v2-evidence-closure'",
    "'canonicalEvidenceRoot'",
    'Read-Stage5LockstepV2Evidence',
    '-ExpectedSourceCommit $evidenceValidationContext.sourceCommit',
    '-ExpectedArtifactSetSha256 $evidenceValidationContext.artifactSetSha256',
    '-ArtifactHashes $evidenceValidationContext.artifactHashes',
    '-ArtifactPaths $evidenceValidationContext.artifactPaths',
    '-ExpectedEvidenceSha256 $evidenceValidationContext.evidenceSha256',
    '-ExpectedCohortNonce $evidenceValidationContext.cohortNonce',
    '-ExpectedRuntimeClosure $evidenceValidationContext.runtimeClosure')) {
    Assert-Stage5WorkflowLiteral $weeklyPromotionValidatorContent `
        $fullEvidenceBinding 'weekly release full lockstep-v2 evidence validation'
}
foreach ($producerClosureBinding in @(
    "`$LockstepEvidenceClosureLeaf = 'Stage5LockstepV2EvidenceClosure.json'",
    "`$LockstepQualificationDataEvidenceLeaf = 'QualificationData.json'",
    'function Read-AndValidateQualificationData',
    '-ExpectedManifestSha256 $QualificationDataManifestSha256',
    '-ExpectedClosureSha256 $QualificationDataClosureSha256',
    '[IO.File]::Copy($qualificationData.path, $qualificationDataEvidencePath)',
    'function New-LockstepV2EvidenceClosure',
    "evidenceKind = 'lockstep-v2-evidence-closure'",
    'canonicalEvidenceRoot = $rootFull',
    'fileCount = $entries.Count',
    'closureSha256 = $closureSha256',
    '$evidenceClosure = New-LockstepV2EvidenceClosure')) {
    Assert-Stage5WorkflowLiteral $lockstepV2QualifierContent `
        $producerClosureBinding 'installed lockstep-v2 evidence-closure producer'
}
$outerAcceptanceWrite = $lockstepV2QualifierContent.LastIndexOf(
    'Write-AtomicText $finalAcceptancePath', [StringComparison]::Ordinal)
$closureProduction = $lockstepV2QualifierContent.LastIndexOf(
    '$evidenceClosure = New-LockstepV2EvidenceClosure',
    [StringComparison]::Ordinal)
$qualificationDataEvidenceCopy = $lockstepV2QualifierContent.LastIndexOf(
    '[IO.File]::Copy($qualificationData.path, $qualificationDataEvidencePath)',
    [StringComparison]::Ordinal)
Assert-Stage5WorkflowCondition ($outerAcceptanceWrite -ge 0 -and
    $qualificationDataEvidenceCopy -gt $outerAcceptanceWrite -and
    $closureProduction -gt $qualificationDataEvidenceCopy -and
    $closureProduction -gt $outerAcceptanceWrite) `
    'Installed lockstep-v2 evidence closure must be produced after the outer acceptance envelope and qualification-data copy.'

Write-Output 'Stage 5 workflow contract passed.'
