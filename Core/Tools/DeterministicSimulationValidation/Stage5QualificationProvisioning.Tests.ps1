[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Read-Stage5ScriptAst {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $Path), [ref]$tokens, [ref]$errors)
    Assert-True ($errors.Count -eq 0) `
        "$Path must parse without PowerShell errors: $($errors -join '; ')"
    return $ast
}

function Assert-Stage5ParameterContract {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedNames,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $parameters = @($Ast.ParamBlock.Parameters)
    $observedNames = @($parameters | ForEach-Object {
        $_.Name.VariablePath.UserPath
    })
    Assert-True (($observedNames -join '|') -ceq ($ExpectedNames -join '|')) `
        "$Description must expose only the reviewed parameters in the reviewed order. Observed: $($observedNames -join ', ')"

    foreach ($parameter in $parameters) {
        $parameterAttributes = @($parameter.Attributes | Where-Object {
            $_.TypeName.Name -ceq 'Parameter'
        })
        Assert-True ($parameterAttributes.Count -eq 1) `
            "$Description parameter $($parameter.Name.VariablePath.UserPath) must have one Parameter attribute."
        $mandatory = @($parameterAttributes[0].NamedArguments | Where-Object {
            $_.ArgumentName -ceq 'Mandatory' -and
            $_.Argument.Extent.Text -ceq '$true'
        })
        Assert-True ($mandatory.Count -eq 1) `
            "$Description parameter $($parameter.Name.VariablePath.UserPath) must be mandatory."
    }
}

function Get-Stage5StringValues {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast
    )

    return @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.StringConstantExpressionAst] -or
            $node -is [Management.Automation.Language.ExpandableStringExpressionAst]
    }, $true) | ForEach-Object { [string]$_.Value })
}

function Assert-Stage5RequiredStringValues {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast,

        [Parameter(Mandatory = $true)]
        [string[]]$Values,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $observed = @(Get-Stage5StringValues -Ast $Ast)
    foreach ($value in $Values) {
        Assert-True ($observed -ccontains $value) `
            "$Description is missing reviewed active string literal: $value"
    }
}

function Get-Stage5Assignments {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast,

        [Parameter(Mandatory = $true)]
        [string]$LeftExtent
    )

    return @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -ceq $LeftExtent
    }, $true))
}

function Get-Stage5Commands {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return @($Ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq $Name
    }, $true))
}

$runtimeScript = Join-Path $PSScriptRoot 'New-Stage5RuntimeManifests.ps1'
$dataScript = Join-Path $PSScriptRoot 'Install-Stage5QualificationData.ps1'
$runtimeAst = Read-Stage5ScriptAst -Path $runtimeScript
$dataAst = Read-Stage5ScriptAst -Path $dataScript

Assert-Stage5ParameterContract -Ast $runtimeAst -Description 'Runtime manifest producer' `
    -ExpectedNames @('QualificationRoot', 'SourceCommit', 'OutputEnvironmentFile')
Assert-Stage5ParameterContract -Ast $dataAst -Description 'Qualification-data producer' `
    -ExpectedNames @(
        'QualificationRoot',
        'SourceCommit',
        'MapName',
        'GeneralsMapCrc',
        'ZeroHourMapCrc',
        'AwsEndpointUrl',
        'OutputEnvironmentFile'
    )

Assert-Stage5RequiredStringValues -Ast $runtimeAst -Description 'Runtime manifest producer' `
    -Values @(
        'H:\Stage5WeeklyPromotionQualification',
        'GeneralsRuntime',
        'ZeroHourRuntime',
        'Stage5RuntimeDependencies.json',
        'Stage5ArtifactSet.json',
        'generals',
        'zerohour',
        'executable',
        'launcher',
        'launcher-config',
        'STAGE5_LOCKSTEP_V2_GENERALS_EXECUTABLE',
        'STAGE5_LOCKSTEP_V2_ZEROHOUR_EXECUTABLE',
        'STAGE5_LOCKSTEP_V2_RUNTIME_MANIFEST_SHA256',
        'STAGE5_LOCKSTEP_V2_RUNTIME_CLOSURE_SHA256',
        'OutputEnvironmentFile must resolve to the exact existing GITHUB_ENV file.'
    )

Assert-Stage5RequiredStringValues -Ast $dataAst -Description 'Qualification-data producer' `
    -Values @(
        'H:\Stage5WeeklyPromotionQualification',
        'Stage5QualificationData.json',
        'lockstep-v2-qualification-data',
        'genci-r2-trimmed-data',
        's3://github-ci/generals108_gamedata_trimmed.7z',
        '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372',
        's3://github-ci/zerohour104_gamedata_trimmed.7z',
        '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21',
        'English.big',
        'INI.big',
        'Maps.big',
        'W3D.big',
        'INIZH.big',
        'MapsZH.big',
        'W3DZH.big',
        'Data/Scripts/MultiplayerScripts.scb',
        'Data/Scripts/Scripts.ini',
        'Data/Scripts/SkirmishScripts.scb',
        'STAGE5_LOCKSTEP_V2_DATA_MANIFEST_SHA256',
        'STAGE5_LOCKSTEP_V2_DATA_CLOSURE_SHA256',
        'OutputEnvironmentFile must resolve to the exact existing GITHUB_ENV file.'
    )

$forbiddenCommands = @('Invoke-Expression', 'iex')
foreach ($scriptAst in @($runtimeAst, $dataAst)) {
    foreach ($forbiddenCommand in $forbiddenCommands) {
        Assert-True (@(Get-Stage5Commands -Ast $scriptAst `
            -Name $forbiddenCommand).Count -eq 0) `
            "Stage 5 provisioning scripts must not invoke $forbiddenCommand."
    }
}

$runtimeOpenAssignments = @(Get-Stage5Assignments -Ast $runtimeAst `
    -LeftExtent '$stream' | Where-Object {
        $_.Right.Extent.Text -match '\[IO\.FileShare\]::Read' -and
        $_.Right.Extent.Text -match '\[IO\.FileAccess\]::Read'
    })
Assert-True ($runtimeOpenAssignments.Count -ge 1) `
    'Runtime files must be held read-only with FileShare.Read while manifests are produced.'
$runtimeFinalHash = @(Get-Stage5Assignments -Ast $runtimeAst `
    -LeftExtent '$observedSha256' | Where-Object {
        $_.Right.Extent.Text -match '\$runtimeLock\.stream'
    })
Assert-True ($runtimeFinalHash.Count -eq 1) `
    'Runtime files must be rehashed from their held streams before environment publication.'

$archiveOpen = @(Get-Stage5Assignments -Ast $dataAst -LeftExtent '$archiveStream' |
    Where-Object {
        $_.Right.Extent.Text -match '\[IO\.FileMode\]::Open' -and
        $_.Right.Extent.Text -match '\[IO\.FileAccess\]::Read' -and
        $_.Right.Extent.Text -match '\[IO\.FileShare\]::Read'
    })
Assert-True ($archiveOpen.Count -eq 1) `
    'Reviewed archives must be opened read-only with FileShare.Read.'

$initialArchiveHash = @(Get-Stage5Assignments -Ast $dataAst `
    -LeftExtent '$archiveSha256' | Where-Object {
        $_.Right.Extent.Text -match '\$archiveStream'
    })
$finalArchiveHash = @(Get-Stage5Assignments -Ast $dataAst `
    -LeftExtent '$finalArchiveSha256' | Where-Object {
        $_.Right.Extent.Text -match '\$archiveRecord\.stream'
    })
Assert-True ($initialArchiveHash.Count -eq 1 -and $finalArchiveHash.Count -eq 1) `
    'Reviewed archives must be hashed from the held stream before and after provisioning.'

$sevenZipCalls = @($dataAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.CommandElements.Count -gt 0 -and
        $node.CommandElements[0].Extent.Text -ceq '$sevenZipExecutable'
}, $true))
$sevenZipListCalls = @($sevenZipCalls | Where-Object {
    $_.CommandElements.Count -gt 1 -and $_.CommandElements[1].Extent.Text -ceq 'l'
})
$sevenZipExtractCalls = @($sevenZipCalls | Where-Object {
    $_.CommandElements.Count -gt 1 -and $_.CommandElements[1].Extent.Text -ceq 'x'
})
$manifestWrites = @(Get-Stage5Commands -Ast $dataAst -Name 'Write-Stage5NewUtf8File' |
    Where-Object { $_.Extent.Text -match '\$dataManifestPath' })
$environmentWrites = @(Get-Stage5Commands -Ast $dataAst `
    -Name 'Write-Stage5EnvironmentBindings')
Assert-True ($sevenZipListCalls.Count -eq 1 -and $sevenZipExtractCalls.Count -eq 1 -and
    $manifestWrites.Count -eq 1 -and $environmentWrites.Count -eq 1) `
    'Qualification-data production must have one reviewed archive inspection, extraction, manifest publication, and environment publication site.'
Assert-True ($archiveOpen[0].Extent.StartOffset -lt $sevenZipListCalls[0].Extent.StartOffset -and
    $sevenZipListCalls[0].Extent.StartOffset -lt $sevenZipExtractCalls[0].Extent.StartOffset -and
    $sevenZipExtractCalls[0].Extent.StartOffset -lt $manifestWrites[0].Extent.StartOffset -and
    $manifestWrites[0].Extent.StartOffset -lt $finalArchiveHash[0].Extent.StartOffset -and
    $finalArchiveHash[0].Extent.StartOffset -lt $environmentWrites[0].Extent.StartOffset) `
    'Archive locking, extraction, manifest production, final rehash, and environment publication must remain ordered fail-closed.'

$dataParameterNames = @($dataAst.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
Assert-True (-not ($dataParameterNames -match 'Hash|Object|Archive')) `
    'Reviewed R2 object identities and hashes must not be caller-controlled parameters.'

$dataSource = Get-Content -LiteralPath $dataScript -Raw
Assert-True ($dataSource -notmatch '\$env:EXPECTED_HASH_' -and
    $dataSource -notmatch '\$env:AWS_ENDPOINT_URL') `
    'Qualification-data provenance must use checked-in hashes and the explicit endpoint argument.'
Assert-True ($dataSource -notmatch 'Remove-Item[^\r\n]*-Recurse') `
    'Qualification-data provisioning must not perform recursive deletion.'

$testCommit = '0000000000000000000000000000000000000000'
try {
    & $runtimeScript -QualificationRoot 'H:\NotStage5' `
        -SourceCommit $testCommit -OutputEnvironmentFile 'H:\missing.env'
    throw 'Runtime manifest producer accepted a noncanonical qualification root.'
}
catch {
    Assert-True ($_.Exception.Message -match 'root is not canonical') `
        "Runtime manifest producer returned the wrong root rejection: $($_.Exception.Message)"
}
try {
    & $dataScript -QualificationRoot 'H:\NotStage5' -SourceCommit $testCommit `
        -MapName 'Maps/Test/Test.map' -GeneralsMapCrc 1 -ZeroHourMapCrc 2 `
        -AwsEndpointUrl 'https://example.invalid' `
        -OutputEnvironmentFile 'H:\missing.env'
    throw 'Qualification-data producer accepted a noncanonical qualification root.'
}
catch {
    Assert-True ($_.Exception.Message -match 'root is not canonical') `
        "Qualification-data producer returned the wrong root rejection: $($_.Exception.Message)"
}

Write-Output 'Stage 5 qualification provisioning static tests passed.'
