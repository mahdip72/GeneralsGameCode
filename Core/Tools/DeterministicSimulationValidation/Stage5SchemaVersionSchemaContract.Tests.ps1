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

function Get-JsonProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) {
        return $null
    }
    return $Object.PSObject.Properties[$Name].Value
}

function Test-StrictJsonInteger {
    param([object]$Value)
    if ($null -eq $Value -or $Value -is [bool] -or
        $Value -is [System.Array] -or $Value -is [string]) {
        return $false
    }
    return $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64]
}

function Test-SchemaVersionAllowedValue {
    param(
        [Parameter(Mandatory = $true)][object]$SchemaNode,
        [object]$Value
    )

    $type = Get-JsonProperty $SchemaNode 'type'
    if ([string]$type -ceq 'integer' -and -not (Test-StrictJsonInteger $Value)) {
        return $false
    }

    $const = Get-JsonProperty $SchemaNode 'const'
    if ($null -ne $const) {
        return [decimal]$Value -eq [decimal]$const
    }

    $enum = Get-JsonProperty $SchemaNode 'enum'
    if ($null -ne $enum) {
        foreach ($candidate in @($enum)) {
            if ([decimal]$Value -eq [decimal]$candidate) {
                return $true
            }
        }
        return $false
    }

    return $false
}

function Find-SchemaVersionNodes {
    param(
        [AllowNull()][object]$Node,
        [Parameter(Mandatory = $true)][string]$FileName,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Pointer
    )

    $found = @()
    if ($null -eq $Node) { return $found }

    if (($Node -is [System.Collections.IEnumerable]) -and
        ($Node -isnot [string]) -and ($Node -isnot [pscustomobject])) {
        $index = 0
        foreach ($item in $Node) {
            $found += @(Find-SchemaVersionNodes $item $FileName "$Pointer/$index")
            $index++
        }
        return $found
    }

    if ($Node -is [pscustomobject]) {
        foreach ($property in $Node.PSObject.Properties) {
            $childPointer = "$Pointer/$($property.Name)"
            if ($property.Name -ceq 'schemaVersion') {
                $found += [pscustomobject]@{
                    FileName = $FileName
                    Pointer = $childPointer
                    SchemaNode = $property.Value
                }
            }
            $found += @(Find-SchemaVersionNodes $property.Value $FileName $childPointer)
        }
    }
    return $found
}

function Find-StringConstraintNodes {
    param(
        [AllowNull()][object]$Node,
        [Parameter(Mandatory = $true)][string]$FileName,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Pointer,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$ParentName
    )

    $found = @()
    if ($null -eq $Node) { return $found }

    if (($Node -is [System.Collections.IEnumerable]) -and
        ($Node -isnot [string]) -and ($Node -isnot [pscustomobject])) {
        $index = 0
        foreach ($item in $Node) {
            $found += @(Find-StringConstraintNodes $item $FileName `
                "$Pointer/$index" $ParentName)
            $index++
        }
        return $found
    }

    if ($Node -is [pscustomobject]) {
        $const = Get-JsonProperty $Node 'const'
        if ($const -is [string]) {
            $found += [pscustomobject]@{
                FileName = $FileName
                Pointer = $Pointer
                ParentName = $ParentName
                Kind = 'const'
                AllowedValues = @([string]$const)
                SchemaNode = $Node
            }
        }

        $enum = Get-JsonProperty $Node 'enum'
        if (($enum -is [System.Collections.IEnumerable]) -and
            ($enum -isnot [string])) {
            $values = @($enum)
            if ($values.Count -gt 0 -and
                @($values | Where-Object { $_ -isnot [string] }).Count -eq 0) {
                $found += [pscustomobject]@{
                    FileName = $FileName
                    Pointer = $Pointer
                    ParentName = $ParentName
                    Kind = 'enum'
                    AllowedValues = @($values | ForEach-Object { [string]$_ })
                    SchemaNode = $Node
                }
            }
        }

        foreach ($property in $Node.PSObject.Properties) {
            $childPointer = "$Pointer/$($property.Name)"
            $found += @(Find-StringConstraintNodes $property.Value $FileName `
                $childPointer $property.Name)
        }
    }
    return $found
}

function Find-StringIdentityPathHashNodes {
    param(
        [AllowNull()][object]$Node,
        [Parameter(Mandatory = $true)][string]$FileName,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Pointer,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$ParentName
    )

    $found = @()
    if ($null -eq $Node) { return $found }

    if (($Node -is [System.Collections.IEnumerable]) -and
        ($Node -isnot [string]) -and ($Node -isnot [pscustomobject])) {
        $index = 0
        foreach ($item in $Node) {
            $found += @(Find-StringIdentityPathHashNodes $item $FileName `
                "$Pointer/$index" $ParentName)
            $index++
        }
        return $found
    }

    if ($Node -is [pscustomobject]) {
        $type = Get-JsonProperty $Node 'type'
        $ref = Get-JsonProperty $Node '$ref'
        $const = Get-JsonProperty $Node 'const'
        $enum = Get-JsonProperty $Node 'enum'
        $pattern = Get-JsonProperty $Node 'pattern'
        $format = Get-JsonProperty $Node 'format'
        $minLength = Get-JsonProperty $Node 'minLength'
        $maxLength = Get-JsonProperty $Node 'maxLength'
        $hasStringConstraint = $null -ne $pattern -or
            $null -ne $format -or $null -ne $minLength -or
            $null -ne $maxLength
        if ($ParentName -match '(?i)(?:path|sha256|hash|commit|nonce|identity)$' -and
            $hasStringConstraint -and $null -eq $ref -and
            $null -eq $const -and $null -eq $enum) {
            $found += [pscustomobject]@{
                FileName = $FileName
                Pointer = $Pointer
                ParentName = $ParentName
                Type = $type
                SchemaNode = $Node
            }
        }

        foreach ($property in $Node.PSObject.Properties) {
            $childPointer = "$Pointer/$($property.Name)"
            $found += @(Find-StringIdentityPathHashNodes $property.Value $FileName `
                $childPointer $property.Name)
        }
    }
    return $found
}

function Test-StringConstraintAllowedValue {
    param(
        [Parameter(Mandatory = $true)][object]$SchemaNode,
        [object]$Value
    )

    $type = Get-JsonProperty $SchemaNode 'type'
    if ([string]$type -cne 'string' -or $Value -isnot [string]) {
        return $false
    }

    $const = Get-JsonProperty $SchemaNode 'const'
    if ($const -is [string]) {
        return $Value -ceq $const
    }

    $enum = Get-JsonProperty $SchemaNode 'enum'
    if (($enum -is [System.Collections.IEnumerable]) -and
        ($enum -isnot [string])) {
        foreach ($candidate in @($enum)) {
            if ($candidate -is [string] -and $Value -ceq $candidate) {
                return $true
            }
        }
    }
    return $false
}

function Assert-StringConstraintNodes {
    param(
        [Parameter(Mandatory = $true)][object[]]$Nodes,
        [Parameter(Mandatory = $true)][string]$Context
    )

    Assert-Test ($Nodes.Count -gt 0) `
        "$Context must contain at least one string const or all-string enum node."
    foreach ($entry in $Nodes) {
        $node = $entry.SchemaNode
        $type = Get-JsonProperty $node 'type'
        Assert-Test ([string]$type -ceq 'string') `
            "String $($entry.Kind) at $($entry.FileName):$($entry.Pointer) must declare type string."

        foreach ($allowed in $entry.AllowedValues) {
            Assert-Test (Test-StringConstraintAllowedValue $node $allowed) `
                "String $($entry.Kind) at $($entry.FileName):$($entry.Pointer) rejected valid value '$allowed'."
            foreach ($invalid in @(
                    [int64]1,
                    [double]1.0,
                    [bool]$true,
                    [object[]]@(,$allowed)
                )) {
                Assert-Test (-not (Test-StringConstraintAllowedValue $node $invalid)) `
                    "String $($entry.Kind) at $($entry.FileName):$($entry.Pointer) accepted non-string value '$invalid'."
            }
        }
    }
}

function Assert-StringIdentityPathHashNodes {
    param(
        [Parameter(Mandatory = $true)][object[]]$Nodes,
        [Parameter(Mandatory = $true)][string]$Context
    )

    foreach ($entry in $Nodes) {
        $type = Get-JsonProperty $entry.SchemaNode 'type'
        Assert-Test ([string]$type -ceq 'string') `
            "String identity/path/hash property at $($entry.FileName):$($entry.Pointer) must declare type string."
    }
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$validationRoot = Join-Path $root 'Core\Tools\DeterministicSimulationValidation'
Assert-Test (Test-Path -LiteralPath $validationRoot -PathType Container) `
    "Deterministic simulation validation directory is missing: $validationRoot"

$schemaFiles = @(Get-ChildItem -LiteralPath $validationRoot -Filter '*.schema.json' -File |
    Sort-Object Name)
Assert-Test ($schemaFiles.Count -gt 0) 'No deterministic simulation JSON schemas were found.'

$allNodes = @()
$nodesByFile = @{}
$allStringNodes = @()
$allStringIdentityPathHashNodes = @()
foreach ($schemaFile in $schemaFiles) {
    try {
        $schema = Get-Content -LiteralPath $schemaFile.FullName -Raw | ConvertFrom-Json
    }
    catch {
        throw "Schema '$($schemaFile.Name)' is not valid JSON: $($_.Exception.Message)"
    }

    $nodes = @(Find-SchemaVersionNodes $schema $schemaFile.Name '')
    Assert-Test ($nodes.Count -gt 0) `
        "Schema '$($schemaFile.Name)' has no schemaVersion definition."
    $nodesByFile[$schemaFile.Name] = $nodes
    $allNodes += $nodes

    $stringNodes = @(Find-StringConstraintNodes $schema $schemaFile.Name '' '')
    Assert-StringConstraintNodes $stringNodes $schemaFile.Name
    $allStringNodes += $stringNodes

    $stringIdentityPathHashNodes = @(Find-StringIdentityPathHashNodes `
        $schema $schemaFile.Name '' '')
    Assert-StringIdentityPathHashNodes $stringIdentityPathHashNodes $schemaFile.Name
    $allStringIdentityPathHashNodes += $stringIdentityPathHashNodes
}

# Removing type from any constrained string node must be rejected.  Mutate
# detached in-memory copies so the test never changes the repository schemas.
foreach ($entry in $allStringNodes) {
    $mutatedNode = $entry.SchemaNode | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    [void]$mutatedNode.PSObject.Properties.Remove('type')
    $mutationRejected = $false
    try {
        Assert-StringConstraintNodes @([pscustomobject]@{
                FileName = $entry.FileName
                Pointer = $entry.Pointer
                ParentName = $entry.ParentName
                Kind = $entry.Kind
                AllowedValues = $entry.AllowedValues
                SchemaNode = $mutatedNode
            }) 'mutated string constraint'
    }
    catch {
        $mutationRejected = $true
    }
    Assert-Test $mutationRejected `
        "String $($entry.Kind) mutation at $($entry.FileName):$($entry.Pointer) was accepted without type string."
}

foreach ($entry in $allStringIdentityPathHashNodes) {
    $mutatedNode = $entry.SchemaNode | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    [void]$mutatedNode.PSObject.Properties.Remove('type')
    $mutationRejected = $false
    try {
        Assert-StringIdentityPathHashNodes @([pscustomobject]@{
                FileName = $entry.FileName
                Pointer = $entry.Pointer
                ParentName = $entry.ParentName
                Type = $null
                SchemaNode = $mutatedNode
            }) 'mutated identity/path/hash property'
    }
    catch {
        $mutationRejected = $true
    }
    Assert-Test $mutationRejected `
        "String identity/path/hash mutation at $($entry.FileName):$($entry.Pointer) was accepted without type string."
}

# Keep the named families in the audit boundary. Recursive discovery above is
# authoritative for omissions; these checks make the required root/nested
# representatives explicit and prevent an over-narrow fixture from passing.
$familyRequirements = @(
    [pscustomobject]@{ Name = 'PerformanceReceipt'; Minimum = 3; Nested = $true },
    [pscustomobject]@{ Name = 'Stage5ImmutableEvidenceReceipt'; Minimum = 3; Nested = $true },
    [pscustomobject]@{ Name = 'ReplayFixtureManifest'; Minimum = 4; Nested = $true },
    [pscustomobject]@{ Name = 'FinalAcceptanceArtifactSet'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'FinalAcceptanceEvidence'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'FinalAcceptanceManifest'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'Net3LoopbackEvidence'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'PerformanceScalingDiagnostics'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'PerformanceScalingEvidence'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'PerformanceScalingRawSamples'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'PerformanceScalingTopologyReceipt'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'Stage5InstalledKernelExecution'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'Stage5NativePerformanceFixtureProduction'; Minimum = 1; Nested = $false },
    [pscustomobject]@{ Name = 'Stage5ReviewedNativeKernelFixture'; Minimum = 1; Nested = $false }
)
foreach ($family in $familyRequirements) {
    $fileName = "$($family.Name).schema.json"
    Assert-Test $nodesByFile.ContainsKey($fileName) `
        "Required schema family '$fileName' is missing from the inventory."
    $familyNodes = @($nodesByFile[$fileName])
    Assert-Test ($familyNodes.Count -ge $family.Minimum) `
        "Schema family '$fileName' has $($familyNodes.Count) schemaVersion nodes; expected at least $($family.Minimum)."
    if ($family.Nested) {
        Assert-Test (@($familyNodes | Where-Object { $_.Pointer -ne '/properties/schemaVersion' }).Count -gt 0) `
            "Schema family '$fileName' has no nested schemaVersion node."
    }
}

foreach ($entry in $allNodes) {
    $node = $entry.SchemaNode
    $type = Get-JsonProperty $node 'type'
    Assert-Test ([string]$type -ceq 'integer') `
        "schemaVersion at $($entry.FileName):$($entry.Pointer) must declare type integer."

    $allowed = @()
    $const = Get-JsonProperty $node 'const'
    if ($null -ne $const) {
        $allowed = @($const)
    }
    else {
        $enum = Get-JsonProperty $node 'enum'
        Assert-Test ($null -ne $enum) `
            "schemaVersion at $($entry.FileName):$($entry.Pointer) must declare const or enum."
        $allowed = @($enum)
    }
    Assert-Test ($allowed.Count -gt 0) `
        "schemaVersion at $($entry.FileName):$($entry.Pointer) must allow at least one value."
    foreach ($expected in $allowed) {
        Assert-Test (Test-StrictJsonInteger $expected) `
            "schemaVersion constraint at $($entry.FileName):$($entry.Pointer) is not an integer."
        $valid = [int64]$expected
        Assert-Test (Test-SchemaVersionAllowedValue $node $valid) `
            "schemaVersion at $($entry.FileName):$($entry.Pointer) rejected valid integer $valid."

        $invalidCases = @(
            [pscustomobject]@{ Name = 'string'; Value = [string]$valid },
            [pscustomobject]@{ Name = 'array'; Value = [object[]]@(,$valid) },
            [pscustomobject]@{ Name = 'double'; Value = [double]$valid },
            [pscustomobject]@{ Name = 'fraction'; Value = [double]$valid + 0.5 },
            [pscustomobject]@{ Name = 'boolean'; Value = [bool]$true }
        )
        foreach ($invalid in $invalidCases) {
            Assert-Test (-not (Test-SchemaVersionAllowedValue $node $invalid.Value)) `
                "schemaVersion at $($entry.FileName):$($entry.Pointer) accepted $($invalid.Name) value for $valid."
        }
    }
}

$inventory = $allNodes | ForEach-Object {
    "{0}:{1}" -f $_.FileName, $_.Pointer
}
Write-Output "Stage5SchemaVersionSchemaContract: PASS ($($schemaFiles.Count) schemas; $($allNodes.Count) schemaVersion nodes; $($allStringNodes.Count) explicit string const/enum nodes; $($allStringNodes.Count) const/enum mutations rejected; $($allStringIdentityPathHashNodes.Count) explicit identity/path/hash properties; $($allStringIdentityPathHashNodes.Count) identity/path/hash mutations rejected)"
$inventory | ForEach-Object { Write-Output "  $_" }
