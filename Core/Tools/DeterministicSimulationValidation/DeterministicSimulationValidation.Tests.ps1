[CmdletBinding()]
param(
    [ValidateSet('All', 'Plan', 'Runtime', 'Acceptance')]
    [string]$ValidationPartition = 'All',
    [string]$FocusedAcceptanceCase = '',
    [switch]$PartitionSelectionPreflightOnly
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$focusedQualificationData = $FocusedAcceptanceCase -ceq 'QualificationDataReceipt'
$focusedDevelopmentReadinessExecutionEvidence =
    $FocusedAcceptanceCase -ceq 'DevelopmentReadinessExecutionEvidence'
$focusedAiDeterminismGrouping =
    $FocusedAcceptanceCase -ceq 'AiDeterminismGrouping'
$focusedLivePlanEntryIdentity =
    $FocusedAcceptanceCase -ceq 'LivePlanEntryIdentity'
$focusedResultTreeDictionary =
    $FocusedAcceptanceCase -ceq 'ResultTreeDictionary'
if (-not [string]::IsNullOrWhiteSpace($FocusedAcceptanceCase) -and
    -not ($focusedQualificationData -or
        $focusedDevelopmentReadinessExecutionEvidence -or
        $focusedAiDeterminismGrouping -or
        $focusedLivePlanEntryIdentity -or
        $focusedResultTreeDictionary)) {
    throw "Unknown focused acceptance case '$FocusedAcceptanceCase'."
}
if (($focusedQualificationData -or
    $focusedDevelopmentReadinessExecutionEvidence -or
        $focusedAiDeterminismGrouping -or
        $focusedLivePlanEntryIdentity -or
        $focusedResultTreeDictionary) -and
    $ValidationPartition -notin @('All', 'Acceptance')) {
    throw 'Focused acceptance cases require ValidationPartition Acceptance or All.'
}
$hasFocusedAcceptanceCase = $focusedQualificationData -or
    $focusedDevelopmentReadinessExecutionEvidence -or
    $focusedAiDeterminismGrouping -or
    $focusedLivePlanEntryIdentity -or
    $focusedResultTreeDictionary
$runPlan = -not $hasFocusedAcceptanceCase -and
    ($ValidationPartition -eq 'All' -or $ValidationPartition -eq 'Plan')
$runRuntime = -not $hasFocusedAcceptanceCase -and
    ($ValidationPartition -eq 'All' -or $ValidationPartition -eq 'Runtime')
$runAcceptance = $hasFocusedAcceptanceCase -or
    $ValidationPartition -eq 'All' -or $ValidationPartition -eq 'Acceptance'
# Keep the typed string parameter distinct from the boolean selector. PowerShell
# variable names are case-insensitive: assigning false to a lowercase spelling
# of FocusedAcceptanceCase converts it to the truthy string 'False'. These
# invariants run in every partition and catch accidental routing regressions.
$expectedFocusedSelection = $focusedQualificationData -or
    $focusedDevelopmentReadinessExecutionEvidence -or $focusedAiDeterminismGrouping -or
    $focusedLivePlanEntryIdentity -or $focusedResultTreeDictionary
if ($runPlan -ne (($ValidationPartition -in @('All', 'Plan')) -and
        -not $expectedFocusedSelection) -or
    $runRuntime -ne (($ValidationPartition -in @('All', 'Runtime')) -and
        -not $expectedFocusedSelection) -or
    $runAcceptance -ne (($ValidationPartition -in @('All', 'Acceptance')) -or
        $expectedFocusedSelection)) {
    throw 'Stage 5 test partition selection does not match its requested scope.'
}
if ($PartitionSelectionPreflightOnly) {
    Write-Output ("Stage 5 partition selection passed: {0}; Plan={1}; Runtime={2}; Acceptance={3}" -f
        $ValidationPartition, $runPlan, $runRuntime, $runAcceptance)
    return
}
$script:Failures = 0
$script:TestCohortNonce = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
$script:TestCohortCreatedUtc = '2026-09-01T00:00:00.0000000Z'
$script:TestRuntimeClosure = [ordered]@{
    dependencyManifestSha256 = ('D' * 64)
    closureSha256 = ('E' * 64)
}

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'Stage5ReplayCorpusExporter.psm1') -Force

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-Sha256Bytes {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Read-TestJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    $json = Get-Content -LiteralPath $Path -Raw
    $convertFromJson = Get-Command ConvertFrom-Json
    if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
        return $json | ConvertFrom-Json -DateKind String
    }
    return $json | ConvertFrom-Json
}

function ConvertFrom-Stage5TestJsonTextDictionary {
    param([Parameter(Mandatory = $true)][string]$Json)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $convertFromJson = Get-Command ConvertFrom-Json
        if ($convertFromJson.Parameters.ContainsKey('DateKind')) {
            return $Json | ConvertFrom-Json -AsHashtable -DateKind String
        }
        return $Json | ConvertFrom-Json -AsHashtable
    }
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = New-Object System.Web.Script.Serialization.JavaScriptSerializer
    $serializer.MaxJsonLength = 10485760
    return $serializer.DeserializeObject($Json)
}

function ConvertFrom-Stage5TestJsonDictionary {
    param([Parameter(Mandatory = $true)][string]$Path)
    return ConvertFrom-Stage5TestJsonTextDictionary `
        (Get-Content -LiteralPath $Path -Raw)
}

function Test-Stage5TestJsonObject {
    param([object]$Value)
    return $Value -is [Collections.IDictionary]
}

function Test-Stage5TestJsonArray {
    param([object]$Value)
    return $Value -is [Array]
}

function Test-Stage5TestJsonNumber {
    param([object]$Value)
    return $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [Int16] -or $Value -is [UInt16] -or
        $Value -is [Int32] -or $Value -is [UInt32] -or
        $Value -is [Int64] -or $Value -is [UInt64] -or
        $Value -is [single] -or $Value -is [double] -or
        $Value -is [decimal]
}

function Test-Stage5TestJsonInteger {
    param([object]$Value)
    if (-not (Test-Stage5TestJsonNumber $Value)) { return $false }
    try {
        $number = [decimal]$Value
        return [decimal]::Truncate($number) -eq $number
    }
    catch { return $false }
}

function Test-Stage5TestJsonProperty {
    param([Collections.IDictionary]$Object, [string]$Name)
    return @($Object.Keys | Where-Object { [string]$_ -ceq $Name }).Count -eq 1
}

function Get-Stage5TestJsonProperty {
    param([Collections.IDictionary]$Object, [string]$Name)
    $keys = @($Object.Keys | Where-Object { [string]$_ -ceq $Name })
    if ($keys.Count -ne 1) { throw "JSON object is missing exact property '$Name'." }
    $value = $Object[$keys[0]]
    if ($value -is [Array]) { return ,$value }
    return $value
}

function Test-Stage5TestJsonEquivalent {
    param([object]$Left, [object]$Right)
    if ($null -eq $Left -or $null -eq $Right) {
        return $null -eq $Left -and $null -eq $Right
    }
    if ((Test-Stage5TestJsonNumber $Left) -and
        (Test-Stage5TestJsonNumber $Right)) {
        try { return [decimal]$Left -eq [decimal]$Right }
        catch { return $false }
    }
    if ((Test-Stage5TestJsonObject $Left) -and
        (Test-Stage5TestJsonObject $Right)) {
        if ($Left.Count -ne $Right.Count) { return $false }
        foreach ($key in $Left.Keys) {
            if (-not (Test-Stage5TestJsonProperty $Right ([string]$key))) {
                return $false
            }
            if (-not (Test-Stage5TestJsonEquivalent $Left[$key] `
                    (Get-Stage5TestJsonProperty $Right ([string]$key)))) {
                return $false
            }
        }
        return $true
    }
    if ((Test-Stage5TestJsonArray $Left) -and
        (Test-Stage5TestJsonArray $Right)) {
        if ($Left.Count -ne $Right.Count) { return $false }
        for ($index = 0; $index -lt $Left.Count; ++$index) {
            if (-not (Test-Stage5TestJsonEquivalent $Left[$index] $Right[$index])) {
                return $false
            }
        }
        return $true
    }
    if ($Left -is [string] -and $Right -is [string]) {
        return $Left -ceq $Right
    }
    if ($Left -is [bool] -and $Right -is [bool]) {
        return $Left -eq $Right
    }
    return $false
}

function Resolve-Stage5TestJsonSchemaReference {
    param([Collections.IDictionary]$RootSchema, [string]$Reference)
    if (-not $Reference.StartsWith('#/', [StringComparison]::Ordinal)) {
        throw "Test JSON-schema validator supports only local references: $Reference"
    }
    $current = $RootSchema
    foreach ($encoded in $Reference.Substring(2).Split('/')) {
        $name = $encoded.Replace('~1', '/').Replace('~0', '~')
        if (-not (Test-Stage5TestJsonObject $current) -or
            -not (Test-Stage5TestJsonProperty $current $name)) {
            throw "Test JSON-schema reference is unresolved: $Reference"
        }
        $current = Get-Stage5TestJsonProperty $current $name
    }
    return $current
}

function Test-Stage5TestJsonSchemaNode {
    param(
        [object]$Instance,
        [object]$Schema,
        [Collections.IDictionary]$RootSchema
    )
    if ($Schema -is [bool]) { return [bool]$Schema }
    if (-not (Test-Stage5TestJsonObject $Schema)) { return $false }

    $supportedKeywords = @('$schema', '$id', 'title', 'description', '$defs',
        '$ref', 'type', 'const', 'enum', 'required', 'properties',
        'additionalProperties', 'prefixItems', 'items', 'minItems', 'maxItems', 'uniqueItems',
        'minLength', 'pattern', 'minimum', 'maximum', 'format', 'allOf',
        'anyOf', 'oneOf', 'not', 'if', 'then', 'else', 'contains',
        'minContains', 'maxContains')
    foreach ($keyword in $Schema.Keys) {
        if ($supportedKeywords -cnotcontains [string]$keyword) { return $false }
    }

    if (Test-Stage5TestJsonProperty $Schema '$ref') {
        $resolved = Resolve-Stage5TestJsonSchemaReference $RootSchema `
            ([string](Get-Stage5TestJsonProperty $Schema '$ref'))
        if (-not (Test-Stage5TestJsonSchemaNode $Instance $resolved $RootSchema)) {
            return $false
        }
    }

    if (Test-Stage5TestJsonProperty $Schema 'type') {
        $type = [string](Get-Stage5TestJsonProperty $Schema 'type')
        $typeMatches = switch ($type) {
            'object' { Test-Stage5TestJsonObject $Instance; break }
            'array' { Test-Stage5TestJsonArray $Instance; break }
            'string' { $Instance -is [string]; break }
            'integer' { Test-Stage5TestJsonInteger $Instance; break }
            'number' { Test-Stage5TestJsonNumber $Instance; break }
            'boolean' { $Instance -is [bool]; break }
            'null' { $null -eq $Instance; break }
            default { $false }
        }
        if (-not $typeMatches) { return $false }
    }
    if (Test-Stage5TestJsonProperty $Schema 'const') {
        if (-not (Test-Stage5TestJsonEquivalent $Instance `
                (Get-Stage5TestJsonProperty $Schema 'const'))) { return $false }
    }
    if (Test-Stage5TestJsonProperty $Schema 'enum') {
        $enumMatch = $false
        $enumValues = Get-Stage5TestJsonProperty $Schema 'enum'
        foreach ($candidate in $enumValues) {
            if (Test-Stage5TestJsonEquivalent $Instance $candidate) {
                $enumMatch = $true
                break
            }
        }
        if (-not $enumMatch) { return $false }
    }

    foreach ($combiner in @('allOf', 'anyOf', 'oneOf')) {
        if (-not (Test-Stage5TestJsonProperty $Schema $combiner)) { continue }
        $matches = 0
        $branches = Get-Stage5TestJsonProperty $Schema $combiner
        foreach ($branch in $branches) {
            if (Test-Stage5TestJsonSchemaNode $Instance $branch $RootSchema) {
                ++$matches
            }
        }
        if (($combiner -ceq 'allOf' -and $matches -ne $branches.Count) -or
            ($combiner -ceq 'anyOf' -and $matches -lt 1) -or
            ($combiner -ceq 'oneOf' -and $matches -ne 1)) { return $false }
    }
    if (Test-Stage5TestJsonProperty $Schema 'not') {
        if (Test-Stage5TestJsonSchemaNode $Instance `
                (Get-Stage5TestJsonProperty $Schema 'not') $RootSchema) {
            return $false
        }
    }
    if (Test-Stage5TestJsonProperty $Schema 'if') {
        $condition = Test-Stage5TestJsonSchemaNode $Instance `
            (Get-Stage5TestJsonProperty $Schema 'if') $RootSchema
        if ($condition -and (Test-Stage5TestJsonProperty $Schema 'then') -and
            -not (Test-Stage5TestJsonSchemaNode $Instance `
                (Get-Stage5TestJsonProperty $Schema 'then') $RootSchema)) {
            return $false
        }
        if (-not $condition -and (Test-Stage5TestJsonProperty $Schema 'else') -and
            -not (Test-Stage5TestJsonSchemaNode $Instance `
                (Get-Stage5TestJsonProperty $Schema 'else') $RootSchema)) {
            return $false
        }
    }

    if (Test-Stage5TestJsonObject $Instance) {
        if (Test-Stage5TestJsonProperty $Schema 'required') {
            $requiredNames = Get-Stage5TestJsonProperty $Schema 'required'
            foreach ($name in $requiredNames) {
                if (-not (Test-Stage5TestJsonProperty $Instance ([string]$name))) {
                    return $false
                }
            }
        }
        $propertySchemas = $null
        if (Test-Stage5TestJsonProperty $Schema 'properties') {
            $propertySchemas = Get-Stage5TestJsonProperty $Schema 'properties'
            if (-not (Test-Stage5TestJsonObject $propertySchemas)) { return $false }
            foreach ($name in $propertySchemas.Keys) {
                if ((Test-Stage5TestJsonProperty $Instance ([string]$name)) -and
                    -not (Test-Stage5TestJsonSchemaNode `
                        (Get-Stage5TestJsonProperty $Instance ([string]$name)) `
                        $propertySchemas[$name] $RootSchema)) {
                    return $false
                }
            }
        }
        if (Test-Stage5TestJsonProperty $Schema 'additionalProperties') {
            $additional = Get-Stage5TestJsonProperty $Schema 'additionalProperties'
            $knownNames = if ($null -eq $propertySchemas) { @() }
                else { @($propertySchemas.Keys | ForEach-Object { [string]$_ }) }
            foreach ($name in $Instance.Keys) {
                if ($knownNames -ccontains [string]$name) { continue }
                if ($additional -is [bool]) {
                    if (-not [bool]$additional) { return $false }
                }
                elseif (-not (Test-Stage5TestJsonSchemaNode $Instance[$name] `
                        $additional $RootSchema)) { return $false }
            }
        }
    }

    if (Test-Stage5TestJsonArray $Instance) {
        if ((Test-Stage5TestJsonProperty $Schema 'minItems') -and
            $Instance.Count -lt [int](Get-Stage5TestJsonProperty $Schema 'minItems')) {
            return $false
        }
        if ((Test-Stage5TestJsonProperty $Schema 'maxItems') -and
            $Instance.Count -gt [int](Get-Stage5TestJsonProperty $Schema 'maxItems')) {
            return $false
        }
        $prefixCount = 0
        if (Test-Stage5TestJsonProperty $Schema 'prefixItems') {
            $prefixSchemas = Get-Stage5TestJsonProperty $Schema 'prefixItems'
            if (-not (Test-Stage5TestJsonArray $prefixSchemas)) { return $false }
            $prefixCount = $prefixSchemas.Count
            $boundedPrefixCount = [Math]::Min($Instance.Count, $prefixCount)
            for ($index = 0; $index -lt $boundedPrefixCount; ++$index) {
                if (-not (Test-Stage5TestJsonSchemaNode $Instance[$index] `
                        $prefixSchemas[$index] $RootSchema)) { return $false }
            }
        }
        if (Test-Stage5TestJsonProperty $Schema 'items') {
            $itemSchema = Get-Stage5TestJsonProperty $Schema 'items'
            for ($index = $prefixCount; $index -lt $Instance.Count; ++$index) {
                if (-not (Test-Stage5TestJsonSchemaNode $Instance[$index] `
                        $itemSchema $RootSchema)) {
                    return $false
                }
            }
        }
        if ((Test-Stage5TestJsonProperty $Schema 'uniqueItems') -and
            [bool](Get-Stage5TestJsonProperty $Schema 'uniqueItems')) {
            for ($left = 0; $left -lt $Instance.Count; ++$left) {
                for ($right = $left + 1; $right -lt $Instance.Count; ++$right) {
                    if (Test-Stage5TestJsonEquivalent $Instance[$left] $Instance[$right]) {
                        return $false
                    }
                }
            }
        }
        if (Test-Stage5TestJsonProperty $Schema 'contains') {
            $containsCount = 0
            $containsSchema = Get-Stage5TestJsonProperty $Schema 'contains'
            foreach ($item in $Instance) {
                if (Test-Stage5TestJsonSchemaNode $item $containsSchema $RootSchema) {
                    ++$containsCount
                }
            }
            $minimumContains = if (Test-Stage5TestJsonProperty $Schema 'minContains') {
                [int](Get-Stage5TestJsonProperty $Schema 'minContains')
            } else { 1 }
            if ($containsCount -lt $minimumContains) { return $false }
            if ((Test-Stage5TestJsonProperty $Schema 'maxContains') -and
                $containsCount -gt [int](Get-Stage5TestJsonProperty $Schema 'maxContains')) {
                return $false
            }
        }
    }

    if ($Instance -is [string]) {
        if ((Test-Stage5TestJsonProperty $Schema 'minLength') -and
            $Instance.Length -lt [int](Get-Stage5TestJsonProperty $Schema 'minLength')) {
            return $false
        }
        if ((Test-Stage5TestJsonProperty $Schema 'pattern') -and
            -not [regex]::IsMatch($Instance,
                [string](Get-Stage5TestJsonProperty $Schema 'pattern'))) {
            return $false
        }
        if (Test-Stage5TestJsonProperty $Schema 'format') {
            $format = [string](Get-Stage5TestJsonProperty $Schema 'format')
            if ($format -ceq 'uuid') {
                [Guid]$guid = [Guid]::Empty
                if ($Instance -notmatch '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$' -or
                    -not [Guid]::TryParse($Instance, [ref]$guid)) { return $false }
            }
            elseif ($format -ceq 'date-time') {
                [DateTimeOffset]$timestamp = [DateTimeOffset]::MinValue
                if ($Instance -notmatch '(?:Z|[+-][0-9]{2}:[0-9]{2})$' -or
                    -not [DateTimeOffset]::TryParse($Instance,
                        [Globalization.CultureInfo]::InvariantCulture,
                        [Globalization.DateTimeStyles]::RoundtripKind,
                        [ref]$timestamp)) { return $false }
            }
            else { return $false }
        }
    }
    if (Test-Stage5TestJsonNumber $Instance) {
        if ((Test-Stage5TestJsonProperty $Schema 'minimum') -and
            [decimal]$Instance -lt [decimal](Get-Stage5TestJsonProperty $Schema 'minimum')) {
            return $false
        }
        if ((Test-Stage5TestJsonProperty $Schema 'maximum') -and
            [decimal]$Instance -gt [decimal](Get-Stage5TestJsonProperty $Schema 'maximum')) {
            return $false
        }
    }
    return $true
}

function Test-Stage5TestJsonSchema {
    param(
        [Parameter(Mandatory = $true)][string]$Json,
        [Parameter(Mandatory = $true)][string]$SchemaFile
    )
    try {
        $schema = ConvertFrom-Stage5TestJsonDictionary $SchemaFile
        $instance = ConvertFrom-Stage5TestJsonTextDictionary $Json
        $compatibleResult = Test-Stage5TestJsonSchemaNode $instance $schema $schema
    }
    catch { return $false }
    $nativeTestJson = Get-Command Test-Json -ErrorAction SilentlyContinue
    if ($null -ne $nativeTestJson) {
        $nativeResult = $false
        try {
            $nativeResult = [bool]($Json | Test-Json -SchemaFile $SchemaFile `
                -ErrorAction Stop)
        }
        catch { $nativeResult = $false }
        if ($compatibleResult -ne $nativeResult) {
            throw "Test-local JSON-schema compatibility result diverged from Test-Json for '$SchemaFile'."
        }
    }
    return $compatibleResult
}

function Get-Sha256Text {
    param([Parameter(Mandatory = $true)][string]$Text)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-TestRuntimeClosureSha256 {
    param([Parameter(Mandatory = $true)][object[]]$Files)
    $canonicalLines = @($Files | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f [string]$_['title'], [string]$_['kind'],
            ([string]$_['path']).Replace('\', '/'),
            ([string]$_['sha256']).ToUpperInvariant()
    })
    [Array]::Sort($canonicalLines, [StringComparer]::Ordinal)
    return Get-Sha256Text (($canonicalLines -join "`n") + "`n")
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Write-Error "FAIL: $Message" -ErrorAction Continue
        ++$script:Failures
    }
}

function Assert-Stage5FinalAcceptanceEvidenceSchemaContract {
    $schemaPath = Join-Path $PSScriptRoot 'FinalAcceptanceEvidence.schema.json'
    Assert-True (Test-Path -LiteralPath $schemaPath -PathType Leaf) `
        'the final-acceptance evidence schema is present'
    $validEnvelope = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'deterministic-runtime'
        status = 'passed'
        sourceCommit = 'a' * 40
        title = 'ZeroHour'
        architecture = 'x64'
        artifactSetSha256 = 'A' * 64
        recordedUtc = '2026-09-01T00:00:00.0000000Z'
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = 'D' * 64
            closureSha256 = 'E' * 64
        }
        attachments = @(
            [ordered]@{
                role = 'validation-plan'
                title = 'ZeroHour'
                path = 'validation-plan.json'
                sha256 = 'B' * 64
                trustDomain = 'host-runner'
            }
            [ordered]@{
                role = 'installed-kernel-execution'
                title = 'Both'
                path = 'installed-kernel-execution.json'
                sha256 = 'C' * 64
                trustDomain = 'installed-runtime'
            }
        )
        details = [ordered]@{}
    }
    $validJson = $validEnvelope | ConvertTo-Json -Depth 12
    Assert-True (Test-Stage5TestJsonSchema $validJson $schemaPath) `
        'the current producer-shaped final-acceptance envelope validates against its schema'

    $missingTitle = $validJson | ConvertFrom-Json
    [void]$missingTitle.attachments[0].PSObject.Properties.Remove('title')
    Assert-True (-not (Test-Stage5TestJsonSchema `
            ($missingTitle | ConvertTo-Json -Depth 12) $schemaPath)) `
        'the final-acceptance schema rejects an attachment missing its title binding'

    $unknownTitle = $validJson | ConvertFrom-Json
    $unknownTitle.attachments[0].title = 'UnknownTitle'
    Assert-True (-not (Test-Stage5TestJsonSchema `
            ($unknownTitle | ConvertTo-Json -Depth 12) $schemaPath)) `
        'the final-acceptance schema rejects an unknown attachment title'

    $unknownTrustDomain = $validJson | ConvertFrom-Json
    $unknownTrustDomain.attachments[1].trustDomain = 'unknown-trust-domain'
    Assert-True (-not (Test-Stage5TestJsonSchema `
            ($unknownTrustDomain | ConvertTo-Json -Depth 12) $schemaPath)) `
        'the final-acceptance schema rejects an unknown attachment trust domain'
}

function Assert-Stage5OutputRelativePathContract {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$TestScriptPath
    )
    $testSource = Get-Content -LiteralPath $TestScriptPath -Raw
    $incompatibleRelativePathCall = '[IO.Path]::Get' + 'RelativePath'
    Assert-True ($testSource -notmatch [regex]::Escape($incompatibleRelativePathCall)) `
        'registered validation tests use the Windows PowerShell-compatible relative-path helper'

    $fixtureRoot = Join-Path $Root 'relative-path-contract-root'
    $nestedRoot = Join-Path $fixtureRoot 'nested'
    $siblingRoot = Join-Path $Root 'relative-path-contract-root-sibling'
    New-Item -ItemType Directory -Path $nestedRoot, $siblingRoot -Force | Out-Null
    $insidePath = Join-Path $nestedRoot 'payload.txt'
    $siblingPath = Join-Path $siblingRoot 'payload.txt'
    [IO.File]::WriteAllText($insidePath, 'inside')
    [IO.File]::WriteAllText($siblingPath, 'sibling')

    $relative = ConvertTo-OutputRelativePath $insidePath $fixtureRoot `
        'relative path positive'
    Assert-True ($relative -ceq 'nested\payload.txt') `
        'relative path helper returns a safe nested relative path'

    $caseRoot = $fixtureRoot.ToUpperInvariant()
    $casePath = Join-Path $caseRoot 'nested\payload.txt'
    $caseRelative = ConvertTo-OutputRelativePath $casePath $fixtureRoot `
        'relative path case variant'
    Assert-True ($caseRelative -ceq 'nested\payload.txt') `
        'relative path helper accepts case variation on the same Windows path'

    Assert-Throws {
        ConvertTo-OutputRelativePath $siblingPath $fixtureRoot `
            'relative path sibling negative' | Out-Null
    } 'must remain below output root' `
        'relative path helper rejects a sibling path with a textual root prefix'

    Assert-Throws {
        ConvertTo-OutputRelativePath `
            (Join-Path $fixtureRoot '..\relative-path-contract-root-sibling\payload.txt') `
            $fixtureRoot 'relative path traversal negative' | Out-Null
    } 'must remain below output root' `
        'relative path helper rejects traversal that resolves outside the root'
}

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Stage5CombinedCompleteSequenceCoverage {
    # Regression for the combined producer's strict-mode missing-sequence
    # check.  A complete 1..253 set produces no pipeline objects; the check
    # must still observe an empty, countable collection rather than dereference
    # $null.
    Set-StrictMode -Version 2.0
    $seenSequences = New-Object 'Collections.Generic.HashSet[int]'
    foreach ($sequence in 1..253) {
        [void]$seenSequences.Add($sequence)
    }
    $missingSequences = @(
        1..253 | Where-Object { -not $seenSequences.Contains($_) }
    )
    Assert-True ($seenSequences.Count -eq 253 -and
        $missingSequences.Count -eq 0) `
        'combined producer complete sequence coverage remains countable under strict mode'
}

Assert-Stage5CombinedCompleteSequenceCoverage

function Assert-InstalledNet3ModuleBoundary {
    param([Parameter(Mandatory = $true)][string]$FixturePath)
    $runnerPath = Join-Path $PSScriptRoot 'Invoke-InstalledNet3LoopbackValidation.ps1'
    $runnerSource = Get-Content -LiteralPath $runnerPath -Raw
    Assert-True ($runnerSource -match '(?m)^Import-Module\s+\(Join-Path\s+\$PSScriptRoot') `
        'the installed NET3 runner imports the evidence module through its module boundary'
    Assert-True ($runnerSource -notmatch '(?m)^\.\s+\(Join-Path\s+\$PSScriptRoot') `
        'the installed NET3 runner does not dot-source the evidence module'
    Assert-True ($runnerSource -notmatch 'Get-Process\s+-Id|Stop-Process') `
        'the installed NET3 runner does not reacquire or terminate peers by PID'
    Assert-True ($runnerSource -match '\$Process\.Kill\(\)' -and
        $runnerSource -match '\$Process\.WaitForExit\(\$PostKillWaitMilliseconds\)' -and
        $runnerSource -notmatch '\$Process\.WaitForExit\(\s*\)') `
        'the installed NET3 runner performs bounded retained-handle cleanup'
    Assert-True ($runnerSource -match '\$cleanupFailures\s*=\s*New-Object' -and
        $runnerSource -match 'foreach\s*\(\$entry\s+in\s+\$processes\)' -and
        $runnerSource -match 'Installed NET3 peer cleanup failed') `
        'the installed NET3 runner attempts every peer cleanup and reports aggregate failures'
    Assert-True ($runnerSource -match '\$entry\.peerIndex' -and
        $runnerSource -notmatch '\$entry\.index') `
        'the installed NET3 runner reports cleanup failures with the retained peer index'
    foreach ($helper in @('ConvertFrom-Stage5JsonDictionary', 'Assert-Stage5JsonShape',
        'Get-Stage5JsonValue', 'Test-Stage5JsonInteger', 'Get-Stage5UInt64BitCount')) {
        $escaped = [regex]::Escape($helper)
        Assert-True ($runnerSource -match $escaped) `
            "the installed NET3 runner invokes its shared helper '$helper'"
        Assert-True ($null -ne (Get-Command $helper -ErrorAction SilentlyContinue)) `
            "the evidence module exports the installed NET3 helper '$helper'"
    }
    try {
        $fixture = [ordered]@{ schemaVersion = 1; marker = 'installed-net3-module-fixture' }
        Write-JsonDocument $FixturePath $fixture
        $document = ConvertFrom-Stage5JsonDictionary $FixturePath
        Assert-Stage5JsonShape $document @('schemaVersion', 'marker') `
            'installed NET3 module fixture'
        Assert-True ((Get-Stage5JsonValue $document 'marker' `
            'installed NET3 module fixture') -ceq 'installed-net3-module-fixture') `
            'the installed NET3 runner module fixture can invoke the shared JSON helpers'
        Assert-True (Test-Stage5JsonInteger (Get-Stage5JsonValue $document `
            'schemaVersion' 'installed NET3 module fixture')) `
            'the installed NET3 runner module fixture can invoke the shared scalar helpers'
        Assert-True ((Get-Stage5UInt64BitCount ([UInt64]7)) -eq 3) `
            'the installed NET3 runner module fixture can invoke the shared bit-count helper'
    }
    catch {
        Assert-True $false "the installed NET3 runner module fixture failed: $($_.Exception.Message)"
    }
}

function Assert-Throws {
    param([scriptblock]$Action, [string]$Pattern, [string]$Message)
    try {
        & $Action
        Assert-True $false "$Message (no exception)"
    }
    catch {
        Assert-True ($_.Exception.Message -match $Pattern) "$Message (got '$($_.Exception.Message)')"
    }
}

function Invoke-Stage5LivePlanEntryIdentityFocusedCase {
    # This focused contract reaches the same result projection and live-plan
    # resolver used by the 253-entry execution path without creating its corpus
    # or starting a child process.
    $runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
    $tokens = $null
    $parseErrors = $null
    $runnerAst = [Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) `
        'live-plan entry identity focused runner parses without errors'
    $projectionDefinition = @($runnerAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'New-Stage5ValidationResultProjection'
    }, $true))
    Assert-True ($projectionDefinition.Count -eq 1) `
        'live-plan entry identity focused result projection is present'
    Invoke-Expression $projectionDefinition[0].Extent.Text

    $planEntries = @(
        [pscustomobject]@{
            sequence = 1; entryId = 'ai-0001'; kind = 'ai'; stress = $true
            scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 1
            simulationMode = 'parallel'; requestedWorkers = '2'; workerPolicy = 'auto'
            determinismKey = '4v2-seed-1729'
            validationRole = 'live-determinism'; proofProfileId = 'live-invariants-v1'
        },
        [pscustomobject]@{
            sequence = 2; entryId = 'ai-0002'; kind = 'ai'; stress = $true
            scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
            simulationMode = 'parallel'; requestedWorkers = '2'; workerPolicy = 'auto'
            determinismKey = '4v2-seed-1729'
            validationRole = 'live-authority-stress'; proofProfileId =
                'live-all-slices-authority-v1'
        },
        [pscustomobject]@{
            sequence = 3; entryId = 'ai-0003'; kind = 'ai'; stress = $true
            scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
            simulationMode = 'shadow'; requestedWorkers = '16'; workerPolicy = 'auto'
            determinismKey = '4v2-seed-1729'
            validationRole = 'live-shadow-stress'; proofProfileId =
                'live-all-slices-shadow-v1'
        }
    )
    foreach ($plannedEntry in $planEntries) {
        # Complete AI metadata follows Add-PlanEntry's production call shape;
        # the V2 role identity above remains the original fixture plan identity.
        $plannedEntry | Add-Member -NotePropertyMembers @{
            caseId = $plannedEntry.determinismKey
            matrixRepeat = 0
            replayArgument = ''
            fixtureSha256 = ''
        }
    }
    $plan = [pscustomobject]@{
        schemaVersion = 2
        liveQualification = [pscustomobject]@{
            schemaVersion = 1; profileSetId = 'live-all-slices-v1'
            authorityEntries = @([pscustomobject]@{
                scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
            })
            shadowEntry = [pscustomobject]@{
                scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
            }
        }
        entries = $planEntries
    }
    $run = [pscustomobject]@{
        exitCode = 0; timedOut = $false; wallMilliseconds = [int64]1
        childProcess = [pscustomobject]@{
            stdoutSha256 = 'A' * 64; stderrSha256 = 'B' * 64
        }
    }
    $projection = New-Stage5ValidationResultProjection `
        -Entry $planEntries[0] -Run $run `
        -AiEvidence ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -ReplayMetrics $null -ReplayResult $null `
        -TimingEvidence ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -ExecutionProvenance ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -FrozenLiveEntry $planEntries[0] -RequireFrozenLiveIdentity $true `
        -Title 'ZeroHour'
    Assert-True ($projection.entryId -ceq 'ai-0001' -and
        $projection.validationRole -ceq 'live-determinism' -and
        $projection.proofProfileId -ceq 'live-invariants-v1') `
        'live-plan result projection did not retain the frozen V2 entry identity'
    $requirements = Resolve-Stage5LiveValidationRequirements `
        -Plan $plan -Entry $projection
    Assert-True ($requirements.entryId -ceq 'ai-0001' -and
        $requirements.validationRole -ceq 'live-determinism' -and
        $requirements.proofProfileId -ceq 'live-invariants-v1') `
        'live-plan result projection did not resolve through the production role contract'
    foreach ($identityField in @('entryId', 'kind', 'sequence', 'scenario',
            'seed', 'configuration', 'repeat', 'simulationMode',
            'requestedWorkers', 'workerPolicy', 'stress', 'validationRole',
            'proofProfileId')) {
        Assert-True ($projection.$identityField -ceq $planEntries[0].$identityField) `
            "projection retains original frozen identity field '$identityField'"
        $changedIdentity = $projection.PSObject.Copy()
        $originalValue = $changedIdentity.$identityField
        $changedIdentity.$identityField = if ($originalValue -is [bool]) {
            -not $originalValue
        } elseif ($originalValue -is [int]) {
            $originalValue + 1
        } else { [string]$originalValue + '-tampered' }
        Assert-Throws {
            Resolve-Stage5LiveValidationRequirements -Plan $plan `
                -Entry $changedIdentity | Out-Null
        } 'not a member|does not match|identity' `
            "resolver rejects changed frozen identity field '$identityField'"
        $missingIdentity = $projection.PSObject.Copy()
        [void]$missingIdentity.PSObject.Properties.Remove($identityField)
        Assert-Throws {
            Resolve-Stage5LiveValidationRequirements -Plan $plan `
                -Entry $missingIdentity | Out-Null
        } 'missing property' `
            "resolver rejects missing frozen identity field '$identityField'"
    }

    $legacyEntry = $planEntries[0].PSObject.Copy()
    $legacyEntry.kind = 'replay'
    $legacyEntry.replayArgument = 'fixture.rep'
    foreach ($field in @('entryId', 'validationRole', 'proofProfileId')) {
        [void]$legacyEntry.PSObject.Properties.Remove($field)
    }
    $legacyProjection = New-Stage5ValidationResultProjection `
        -Entry $legacyEntry -Run $run -AiEvidence $null `
        -ReplayMetrics ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -ReplayResult ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -TimingEvidence ([pscustomobject]@{ status = 'synthetic-focused' }) `
        -ExecutionProvenance $null -Title 'ZeroHour'
    Assert-True ($null -eq $legacyProjection.PSObject.Properties['entryId'] -and
        $null -eq $legacyProjection.PSObject.Properties['validationRole'] -and
        $null -eq $legacyProjection.PSObject.Properties['proofProfileId']) `
        'legacy replay result projection must retain the absence of V2 role fields'

    $missing = $projection | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    [void]$missing.PSObject.Properties.Remove('entryId')
    Assert-Throws {
        Resolve-Stage5LiveValidationRequirements -Plan $plan -Entry $missing | Out-Null
    } "missing property 'entryId'" `
        'live-plan resolver rejects result projections without their frozen entry ID'
    Write-Output 'Focused Stage 5 live-plan entry identity projection proof passed.'
}

function Invoke-Stage5AiDeterminismGroupingFocusedCase {
    # Acceptance results are retained as strict dictionaries.  Exercise the
    # same private grouping helper used by the production determinism reader so
    # Group-Object cannot collapse a complete 14-result case under an empty key.
    $determinismGroupingModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    Assert-True ($null -ne $determinismGroupingModule) `
        'AI determinism grouping regression has its evidence module loaded'
    $expectedDeterminismKeys = @(
        '4v3-seed-1729', '4v3-seed-1730', '4v3-seed-1731',
        '4v2-seed-1729', '4v2-seed-1730', '4v2-seed-1731'
    )
    $dictionaryAiResults = @(
        foreach ($determinismKey in $expectedDeterminismKeys) {
            foreach ($configuration in @('serial-1', 'parallel-1', 'parallel-2',
                    'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')) {
                foreach ($repeat in @(1, 2)) {
                    [ordered]@{
                        kind = 'ai'; determinismKey = $determinismKey
                        configuration = $configuration; repeat = $repeat
                        aiEvidence = [ordered]@{
                            finalDigest = 'A1B2C3D4'; endFrame = 42000; winnerTeam = 1
                        }
                    }
                }
            }
        }
    )
    $dictionaryGroups = & $determinismGroupingModule {
        param([object[]]$InputResults)
        Get-Stage5DeterminismGroups $InputResults `
            'focused dictionary AI determinism grouping'
    } $dictionaryAiResults
    $groupedResultCount = [int](($dictionaryGroups |
        Measure-Object -Property Count -Sum).Sum)
    Assert-True (@($dictionaryGroups).Count -eq 6 -and
        @($dictionaryGroups | Where-Object {
            $expectedDeterminismKeys -ccontains $_.Name -and $_.Count -eq 14
        }).Count -eq 6 -and $groupedResultCount -eq 84) `
        'dictionary AI grouping retains six named 14-result cases (84 results)'
    $missingDictionaryKey = [ordered]@{
        kind = 'ai'; configuration = 'serial-1'; repeat = 1
        aiEvidence = [ordered]@{
            finalDigest = 'A1B2C3D4'; endFrame = 42000; winnerTeam = 1
        }
    }
    Assert-Throws {
        & $determinismGroupingModule {
            param([object[]]$InputResults)
            Get-Stage5DeterminismGroups $InputResults `
                'focused dictionary AI determinism grouping'
        } @($missingDictionaryKey) | Out-Null
    } "missing property 'determinismKey'" `
        'dictionary AI grouping rejects a missing determinism key'
}

function Invoke-Stage5ResultTreeDictionaryFocusedCase {
    # This source-connected regression compares the producer's in-memory
    # PSObjects with the exact dictionary decoder used by acceptance readers.
    # It deliberately exercises numeric sequence ordering (10, 1, 2) for both
    # replay and AI trees without creating the canonical corpus.
    $runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
    $runnerTokens = $null
    $runnerErrors = $null
    $runnerAst = [Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$runnerTokens, [ref]$runnerErrors)
    Assert-True ($runnerErrors.Count -eq 0) `
        'result-tree dictionary focused runner parses without errors'
    $treeDefinition = @($runnerAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Get-Stage5ResultTreeSha256'
    }, $true))
    Assert-True ($treeDefinition.Count -eq 1) `
        'result-tree dictionary focused production tree helper is present'
    Invoke-Expression $treeDefinition[0].Extent.Text
    $evidenceModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    Assert-True ($null -ne $evidenceModule) `
        'result-tree dictionary focused evidence module is loaded'

    $results = @()
    foreach ($sequence in @(10, 1, 2)) {
        $results += [pscustomobject]@{
            sequence = [int]$sequence; kind = 'replay'
            determinismKey = "replay-$sequence"; matrixRepeat = 1; repeat = 1
            replayResult = [pscustomobject]@{
                finalFrame = [int](100 + $sequence)
                finalCRC = [uint32](1000 + $sequence)
            }
        }
        $results += [pscustomobject]@{
            sequence = [int]$sequence; kind = 'ai'; scenario = '4v2'
            seed = 1729; configuration = 'parallel-2'; repeat = 1
            aiEvidence = [pscustomobject]@{
                finalDigest = ('A' + $sequence.ToString())
            }
        }
    }
    $producerReplay = Get-Stage5ResultTreeSha256 $results 'replay'
    $producerAi = Get-Stage5ResultTreeSha256 $results 'ai'
    $expectedReplay = Get-Sha256Text ((@(
        '1|replay-1|1|1|101|1001'
        '2|replay-2|1|1|102|1002'
        '10|replay-10|1|1|110|1010'
    ) -join "`n") + "`n")
    $expectedAi = Get-Sha256Text ((@(
        '1|4v2|1729|parallel-2|1|A1'
        '2|4v2|1729|parallel-2|1|A2'
        '10|4v2|1729|parallel-2|1|A10'
    ) -join "`n") + "`n")
    Assert-True ($producerReplay -ceq $expectedReplay -and
        $producerAi -ceq $expectedAi) `
        'producer result-tree helper did not use numeric sequence ordering'

    $json = $results | ConvertTo-Json -Depth 12
    $snapshot = [pscustomobject]@{
        bytes = [Text.Encoding]::UTF8.GetBytes($json)
    }
    $dictionaryResults = & $evidenceModule {
        param($InputSnapshot)
        ConvertFrom-Stage5FinalAcceptanceJsonSnapshot `
            $InputSnapshot 'result-tree dictionary focused decoder'
    } $snapshot
    Assert-True (@($dictionaryResults).Count -eq 6 -and
        @($dictionaryResults | Where-Object {
            $_ -is [Collections.IDictionary]
        }).Count -eq 6) `
        'production acceptance decoder did not retain dictionary result objects'
    $readerReplay = & $evidenceModule {
        param($InputResults)
        Get-Stage5DevelopmentReadinessResultTreeSha256 $InputResults 'replay'
    } $dictionaryResults
    $readerAi = & $evidenceModule {
        param($InputResults)
        Get-Stage5DevelopmentReadinessResultTreeSha256 $InputResults 'ai'
    } $dictionaryResults
    Assert-True ($readerReplay -ceq $producerReplay -and
        $readerAi -ceq $producerAi -and
        $readerReplay -ceq $expectedReplay -and
        $readerAi -ceq $expectedAi) `
        'dictionary-decoded replay/AI trees detached from producer outcomes'

    $mutatedReplay = @($dictionaryResults | Where-Object {
        $_['kind'] -ceq 'replay' -and [int]$_['sequence'] -eq 2
    })[0]
    $mutatedReplay['replayResult']['finalCRC'] = [uint32]9999
    $mutatedReplayTree = & $evidenceModule {
        param($InputResults)
        Get-Stage5DevelopmentReadinessResultTreeSha256 $InputResults 'replay'
    } $dictionaryResults
    Assert-True ($mutatedReplayTree -cne $readerReplay) `
        'replay tree did not change after a retained final CRC mutation'

    $mutatedAi = @($dictionaryResults | Where-Object {
        $_['kind'] -ceq 'ai' -and [int]$_['sequence'] -eq 2
    })[0]
    $mutatedAi['aiEvidence']['finalDigest'] = 'F' * 64
    $mutatedAiTree = & $evidenceModule {
        param($InputResults)
        Get-Stage5DevelopmentReadinessResultTreeSha256 $InputResults 'ai'
    } $dictionaryResults
    Assert-True ($mutatedAiTree -cne $readerAi) `
        'AI tree did not change after a retained final digest mutation'
    Write-Output 'Focused Stage 5 dictionary result-tree proof passed.'
}

if ($focusedLivePlanEntryIdentity) {
    Invoke-Stage5LivePlanEntryIdentityFocusedCase
    return
}
if ($focusedResultTreeDictionary) {
    Invoke-Stage5ResultTreeDictionaryFocusedCase
    return
}
if ($focusedDevelopmentReadinessExecutionEvidence) {
    # This regression intentionally stops before scratch-root setup, artifact
    # generation, and the canonical 253-child corpus.  The default acceptance
    # path below remains the independent complete corpus and retains its outer
    # cardinality and provenance checks.
    $focusedStdoutBytes = [Text.Encoding]::UTF8.GetBytes(
        'focused development-readiness stdout')
    $focusedStderrBytes = [Text.Encoding]::UTF8.GetBytes(
        'focused development-readiness stderr')
    $focusedStdoutHash = Get-Sha256Bytes $focusedStdoutBytes
    $focusedStderrHash = Get-Sha256Bytes $focusedStderrBytes
    $focusedRawLogs = @(
        [ordered]@{
            name = '0001.stdout.log'
            path = 'streams\0001.stdout.log'
            sha256 = $focusedStdoutHash
            snapshot = [pscustomobject]@{ bytes = $focusedStdoutBytes }
        }
        [ordered]@{
            name = '0001.stderr.log'
            path = 'streams\0001.stderr.log'
            sha256 = $focusedStderrHash
            snapshot = [pscustomobject]@{ bytes = $focusedStderrBytes }
        }
    )
    $focusedEntry = [ordered]@{
        stdout = 'streams\0001.stdout.log'
        stderr = 'streams\0001.stderr.log'
    }
    $focusedResult = [ordered]@{
        stdoutSha256 = $focusedStdoutHash
        stderrSha256 = $focusedStderrHash
    }
    $evidenceModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    if ($null -eq $evidenceModule) {
        throw 'DeterministicSimulationEvidence module was not loaded for the focused stream regression.'
    }
    $invokeFocusedStreamReader = {
        param([object]$Entry, [object]$Result, [object[]]$RawLogs)
        & $evidenceModule {
            param($ModuleEntry, $ModuleResult, $ModuleRawLogs)
            Assert-Stage5DevelopmentReadinessExecutionStreamEvidence `
                -Entry $ModuleEntry -Result $ModuleResult `
                -ValidatedRawLogs $ModuleRawLogs `
                -Context 'Stage 5 development-readiness execution evidence focused result 1'
        } $Entry $Result $RawLogs
    }
    $focusedProof = & $invokeFocusedStreamReader `
        $focusedEntry $focusedResult $focusedRawLogs
    Assert-True ($focusedProof.stdout.sha256 -ceq $focusedStdoutHash -and
        $focusedProof.stderr.sha256 -ceq $focusedStderrHash) `
        'focused development-readiness execution evidence retained exact stream hashes'
    $badRawLogs = @(
        [ordered]@{
            name = 'streams\0001.stdout.log'
            path = 'streams\0001.stdout.log'
            sha256 = $focusedStdoutHash
            snapshot = [pscustomobject]@{ bytes = $focusedStdoutBytes }
        }
        [ordered]@{
            name = 'streams\0001.stderr.log'
            path = 'streams\0001.stderr.log'
            sha256 = $focusedStderrHash
            snapshot = [pscustomobject]@{ bytes = $focusedStderrBytes }
        }
    )
    Assert-Throws {
        & $invokeFocusedStreamReader $focusedEntry $focusedResult $badRawLogs |
            Out-Null
    } 'exactly one retained raw log named' `
        'focused development-readiness execution evidence rejects path-qualified raw-log names'
    Write-Output 'Focused Stage 5 development-readiness execution-evidence proof passed.'
    return
}
if ($focusedAiDeterminismGrouping) {
    Invoke-Stage5AiDeterminismGroupingFocusedCase
    if ($script:Failures -ne 0) {
        throw "$script:Failures AI determinism grouping focused test(s) failed."
    }
    Write-Output 'Focused Stage 5 AI determinism grouping proof passed.'
    return
}

function Assert-ValidationProcessTerminationContract {
    param([Parameter(Mandatory = $true)][string]$Content)
    if ($Content -notmatch '(?ms)function\s+Stop-ValidationProcessSafely') {
        throw 'Validation process termination helper is missing.'
    }
    if ($Content -match 'Get-Process\s+-Id') {
        throw 'Validation timeout handling reacquires a process by PID instead of retaining the launched handle.'
    }
    if ($Content -notmatch '\$Process\.Kill\(\)') {
        throw 'Validation timeout handling does not terminate through the retained Process handle.'
    }
    if ($Content -notmatch 'identity is unavailable; terminating through the original Process handle') {
        throw 'Validation timeout handling does not retain the original handle when CIM identity capture is unavailable.'
    }
    if ($Content -match '\$process\.WaitForExit\(\s*\)') {
        throw 'Validation timeout handling contains an unbounded WaitForExit call.'
    }
    if ($Content -match '\$process\.Kill\(\)\s*\}\s*catch\s*\{\s*\}') {
        throw 'Validation timeout handling swallows a process termination failure.'
    }
    if ($Content -notmatch '\$Process\.WaitForExit\(\$PostKillWaitMilliseconds\)') {
        throw 'Validation timeout handling has no bounded post-kill wait.'
    }
    if ($Content -notmatch '(?ms)finally\s*\{.*?\$process\.Dispose\(\).*?\}') {
        throw 'Validation process is not disposed from a finally path.'
    }
}

function Write-JsonDocument {
    param([string]$Path, [object]$Document)
    [IO.File]::WriteAllText($Path, ($Document | ConvertTo-Json -Depth 12))
}

function Write-ImmutableReceiptTestDocument {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$ExecutableSha256, [string]$RunNonce =
        '11111111-1111-4111-8111-111111111111')
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $rawLeaf = [IO.Path]::GetFileNameWithoutExtension($Path) + '.raw.log'
    $rawPath = Join-Path $directory $rawLeaf
    [IO.File]::WriteAllText($rawPath, 'unregistered executable receipt test log')
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-host-runner-receipt'
        status = 'passed'
        role = 'validation-plan'
        trustDomain = 'host-runner'
        producer = 'installed-runtime-validation-plan-v2'
        producerVersion = '2'
        runNonce = $RunNonce
        sourceCommit = $SourceCommit
        title = 'ZeroHour'
        architecture = 'x64'
        artifactSetSha256 = $ArtifactSetSha256
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        executableSha256 = $ExecutableSha256
        recordedUtc = '2026-09-01T00:00:00Z'
        rawLogs = @([ordered]@{
            name = 'stdout'; path = $rawLeaf; sha256 = Get-Sha256 $rawPath
        })
        provenance = [ordered]@{
            kind = 'host-runner-observation'
            runner = 'Run-DeterministicSimulationValidation.ps1'
            runnerVersion = '1'
            childProvenance = 'not-applicable'
            children = @()
        }
        details = [ordered]@{
            gateName = 'deterministic-runtime'; validationSet = 'All'; entryCount = 1
        }
    })
}

function Get-Stage5AcceptanceReceiptTestDetails {
    param([string]$Role)
    switch ($Role) {
        'validation-plan' {
            return [ordered]@{
                gateName = 'deterministic-runtime'; validationSet = 'All'; entryCount = 1
            }
        }
        'validation-results' {
            return [ordered]@{
                resultCount = 253; allExecutionsPassed = $true; resultsSha256 = 'A' * 64
            }
        }
        'replay-results' {
            return [ordered]@{
                uniqueReplayCount = 10; executionCount = 10; crcTreeSha256 = 'B' * 64
                allExecutionsPassed = $true
            }
        }
        'ai-results' {
            return [ordered]@{
                scenarioCount = 2; distinctSeedCount = 3; repeatCount = 2
                allGamesCompleted = $true; digestTreeSha256 = 'C' * 64
            }
        }
        'combined-results' {
            return [ordered]@{
                pipelineMode = 'parallel'; simulationMode = 'parallel'
                workerPolicy = 'auto'; renderer = 'd3d11'; renderThread = 'dedicated'
                bothTitlesPassed = $true
            }
        }
        'performance-report' { return [ordered]@{} }
        default { throw "No host receipt test details exist for role '$Role'." }
    }
}

function Write-Stage5HostReceiptTestDocument {
    param(
        [string]$Path, [string]$Role, [string]$Title, [string]$SourceCommit,
        [string]$ArtifactSetSha256, [Collections.IDictionary]$ArtifactHashes,
        [string]$RunNonce = ''
    )
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $leaf = [IO.Path]::GetFileNameWithoutExtension($Path)
    $stdoutLeaf = "$leaf.stdout.log"
    $stderrLeaf = "$leaf.stderr.log"
    $stdoutPath = Join-Path $directory $stdoutLeaf
    $stderrPath = Join-Path $directory $stderrLeaf
    [IO.File]::WriteAllText($stdoutPath, "host runner stdout for $Role")
    [IO.File]::WriteAllText($stderrPath, "host runner stderr for $Role")
    if ([string]::IsNullOrWhiteSpace($RunNonce)) {
        $RunNonce = [Guid]::NewGuid().ToString()
    }
    $runNonce = $RunNonce
    $qualificationData = $null
    if ($Role -cne 'validation-plan' -and $Role -cne 'combined-results') {
        # Keep host-runner result fixtures bound to the real retained
        # qualification-data reader. The reader returns a typed proof object;
        # final acceptance must not treat that proof as raw JSON.
        $qualificationData = New-Stage5SyntheticQualificationData `
            -Root $directory -Title $Title -SourceCommit $SourceCommit
    }
    $children = @()
    $rawLogs = @()
    if ($Role -ne 'validation-plan') {
        $childTitles = @(if ($Role -ceq 'validation-results') {
            1..253 | ForEach-Object { $Title }
        }
        elseif ($Title -ceq 'Both') { @('Generals', 'ZeroHour') }
        else { $Title })
        $processId = if ($Title -ceq 'Generals') { 32000 } else { 33000 }
        for ($childIndex = 0; $childIndex -lt $childTitles.Count; ++$childIndex) {
            $childTitle = [string]$childTitles[$childIndex]
            $sequence = $childIndex + 1
            $childRunNonce = [Guid]::NewGuid().ToString()
            $executableHash = if ($childTitle -ceq 'Generals') {
                [string]$ArtifactHashes['generals-executable']
            }
            else { [string]$ArtifactHashes['zerohour-executable'] }
            $childLeaf = if ($Role -ceq 'validation-results') {
                '{0}.{1:D4}' -f $childTitle.ToLowerInvariant(), $sequence
            }
            else { $childTitle.ToLowerInvariant() }
            $nativeLeaf = "$leaf.$childLeaf.native.json"
            $nativePath = Join-Path $directory $nativeLeaf
            $nativeRawLeaf = "$leaf.$childLeaf.native.raw.log"
            $nativeTimingLeaf = "$leaf.$childLeaf.native.timing.log"
            $nativeRawPath = Join-Path $directory $nativeRawLeaf
            $nativeTimingPath = Join-Path $directory $nativeTimingLeaf
            [IO.File]::WriteAllText($nativeRawPath,
                "native raw evidence for $Role/$childTitle/$sequence")
            [IO.File]::WriteAllText($nativeTimingPath,
                "native timing evidence for $Role/$childTitle/$sequence")
            $childArguments = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
                '-simulationMode', 'serial', '-workerPolicy', 'auto',
                '-workerCount', '1', '-validationExecutableSha256', $executableHash)
            $childCommandLine = "installed\$childTitle.exe " + ($childArguments -join ' ')
            $nativeDocument = [ordered]@{
                schemaVersion = 1
                evidenceKind = 'stage5-executable-originated-receipt'
                status = 'passed'
                producer = 'game-executable-stage5-performance-report-v2'
                producerVersion = '2'
                runNonce = $childRunNonce
                sourceCommit = $SourceCommit
                artifactSetSha256 = $ArtifactSetSha256
                executableSha256 = $executableHash
                cohortNonce = $script:TestCohortNonce
                runtimeClosure = $script:TestRuntimeClosure
                role = 'performance-report'
                title = $childTitle
                architecture = 'x64'
                cohortCreatedUtc = $script:TestCohortCreatedUtc
                recordedUtc = '2026-09-01T00:00:00Z'
                rawLogs = @(
                    [ordered]@{ name = 'raw-log'; path = $nativeRawLeaf; sha256 = Get-Sha256 $nativeRawPath }
                    [ordered]@{ name = 'timing'; path = $nativeTimingLeaf; sha256 = Get-Sha256 $nativeTimingPath }
                )
                provenance = [ordered]@{
                    kind = 'native-executable-observation'
                    receiptPath = $nativeLeaf
                    processId = $processId
                    processCreationUtc = '2026-09-01T00:00:00Z'
                    executablePath = "installed\$childTitle.exe"
                    executableSha256 = $executableHash
                    commandLine = $childCommandLine
                    exitCode = 0
                }
            }
            Add-Stage5NativeReceiptTestObservations $nativeDocument
            Write-JsonDocument $nativePath $nativeDocument
            $child = [ordered]@{
                role = $Role; title = $childTitle; runNonce = $childRunNonce
                processId = $processId; processCreationUtc = '2026-09-01T00:00:00Z'
                executablePath = "installed\$childTitle.exe"
                executableSha256 = $executableHash
                commandLine = $childCommandLine
                exitCode = 0
                nativeReceipt = [ordered]@{
                    path = $nativeLeaf; sha256 = Get-Sha256 $nativePath
                    producer = 'game-executable-stage5-performance-report-v5'
                    runNonce = $childRunNonce; cohortNonce = $script:TestCohortNonce
                }
            }
            if ($null -ne $qualificationData) {
                $child['qualificationData'] = [ordered]@{
                    path = 'QualificationData.json'
                    title = $childTitle
                    manifestSha256 = [string]$qualificationData.manifestSha256
                    closureSha256 = [string]$qualificationData.closureSha256
                    fileCount = 6
                }
            }
            if ($Role -ceq 'validation-results') {
                $childStdoutLeaf = "$leaf.$childLeaf.stdout.log"
                $childStderrLeaf = "$leaf.$childLeaf.stderr.log"
                $childStdoutPath = Join-Path $directory $childStdoutLeaf
                $childStderrPath = Join-Path $directory $childStderrLeaf
                [IO.File]::WriteAllText($childStdoutPath,
                    "host runner stdout for $Role child $sequence")
                [IO.File]::WriteAllText($childStderrPath,
                    "host runner stderr for $Role child $sequence")
                $child.Insert(0, 'sequence', $sequence)
                $child.Insert(8, 'arguments', [string[]]$childArguments)
                $child.stdout = [ordered]@{
                    path = $childStdoutLeaf; sha256 = Get-Sha256 $childStdoutPath
                }
                $child.stderr = [ordered]@{
                    path = $childStderrLeaf; sha256 = Get-Sha256 $childStderrPath
                }
                $rawLogs += @(
                    [ordered]@{ name = "child-$sequence-stdout"; path = $childStdoutLeaf
                        sha256 = Get-Sha256 $childStdoutPath }
                    [ordered]@{ name = "child-$sequence-stderr"; path = $childStderrLeaf
                        sha256 = Get-Sha256 $childStderrPath }
                )
            }
            else {
                $child.stdout = [ordered]@{ path = $stdoutLeaf; sha256 = Get-Sha256 $stdoutPath }
                $child.stderr = [ordered]@{ path = $stderrLeaf; sha256 = Get-Sha256 $stderrPath }
            }
            $children += $child
            ++$processId
        }
    }
    if ($Role -ceq 'validation-results') {
        $rawLogs = @([ordered]@{
            name = 'validation-results'; path = $stdoutLeaf; sha256 = Get-Sha256 $stdoutPath
        }, [ordered]@{
            name = 'qualification-data'; path = 'QualificationData.json'
            sha256 = [string]$qualificationData.manifestSha256
        }) + $rawLogs
    }
    else {
        $rawLogs = @(
            [ordered]@{ name = 'stdout'; path = $stdoutLeaf; sha256 = Get-Sha256 $stdoutPath }
            [ordered]@{ name = 'stderr'; path = $stderrLeaf; sha256 = Get-Sha256 $stderrPath }
            if ($null -ne $qualificationData) {
                [ordered]@{ name = 'qualification-data'; path = 'QualificationData.json'
                    sha256 = [string]$qualificationData.manifestSha256 }
            }
        )
    }
    $document = [ordered]@{
        schemaVersion = 1; evidenceKind = 'stage5-host-runner-receipt'; status = 'passed'
        role = $Role; trustDomain = 'host-runner'
        producer = "installed-runtime-$($Role)-v2"; producerVersion = '2'
        runNonce = $runNonce; sourceCommit = $SourceCommit; title = $Title
        architecture = 'x64'; artifactSetSha256 = $ArtifactSetSha256
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        executableSha256 = if ($Title -ceq 'Both') {
            [ordered]@{
                Generals = [string]$ArtifactHashes['generals-executable']
                ZeroHour = [string]$ArtifactHashes['zerohour-executable']
            }
        }
        elseif ($Title -ceq 'Generals') { [string]$ArtifactHashes['generals-executable'] }
        else { [string]$ArtifactHashes['zerohour-executable'] }
        recordedUtc = '2026-09-01T00:00:00Z'
        rawLogs = $rawLogs
        provenance = [ordered]@{
            kind = 'host-runner-observation'
            runner = 'Run-DeterministicSimulationValidation.ps1'
            runnerVersion = '1'
            childProvenance = if ($Role -eq 'validation-plan') { 'not-applicable' } else { 'bound' }
            children = $children
        }
        details = Get-Stage5AcceptanceReceiptTestDetails $Role
    }
    if ($Role -ceq 'validation-results') {
        $document.details.resultsSha256 = [string]$document.rawLogs[0].sha256
    }
    if ($null -ne $qualificationData) {
        $document.details.qualificationData = [ordered]@{
            path = 'QualificationData.json'
            title = $Title
            manifestSha256 = [string]$qualificationData.manifestSha256
            closureSha256 = [string]$qualificationData.closureSha256
            fileCount = 6
        }
    }
    if ($Role -ne 'validation-plan') {
        $childNonces = @($children | ForEach-Object { [string]$_['runNonce'] })
        Assert-True ($childNonces.Count -eq (@($childNonces | Sort-Object -Unique)).Count -and
            -not ($childNonces -contains $runNonce)) `
            'host receipt fixtures retain distinct wrapper and actual child process nonces'
        Assert-True (@($children | Where-Object {
            [string]$_['runNonce'] -cne [string]$_['nativeReceipt']['runNonce']
        }).Count -eq 0) `
            'host receipt fixtures bind every actual child nonce to its native receipt'
    }
    Write-JsonDocument $Path $document
}

function Write-Stage5ExecutableReceiptTestDocument {
    param(
        [string]$Path, [string]$Role, [string]$Title, [string]$SourceCommit,
        [string]$ArtifactSetSha256, [Collections.IDictionary]$ArtifactHashes
    )
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $leaf = [IO.Path]::GetFileNameWithoutExtension($Path)
    $stdoutLeaf = "$leaf.native.stdout.log"
    $stderrLeaf = "$leaf.native.stderr.log"
    $stdoutPath = Join-Path $directory $stdoutLeaf
    $stderrPath = Join-Path $directory $stderrLeaf
    [IO.File]::WriteAllText($stdoutPath, "native executable stdout for $Role")
    [IO.File]::WriteAllText($stderrPath, "native executable stderr for $Role")
    $runNonce = [Guid]::NewGuid().ToString()
    $executableHash = if ($Title -ceq 'Generals') {
        [string]$ArtifactHashes['generals-executable']
    }
    else { [string]$ArtifactHashes['zerohour-executable'] }
    $nativeLeaf = "$leaf.native.json"
    $nativePath = Join-Path $directory $nativeLeaf
    $nativeRawLeaf = "$leaf.native.raw.log"
    $nativeTimingLeaf = "$leaf.native.timing.log"
    $nativeRawPath = Join-Path $directory $nativeRawLeaf
    $nativeTimingPath = Join-Path $directory $nativeTimingLeaf
    [IO.File]::WriteAllText($nativeRawPath, "native raw evidence for $Role/$Title")
    [IO.File]::WriteAllText($nativeTimingPath, "native timing evidence for $Role/$Title")
    $nativeDocument = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-executable-originated-receipt'
        status = 'passed'
        producer = 'game-executable-stage5-performance-report-v2'
        producerVersion = '2'
        runNonce = $runNonce
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
        executableSha256 = $executableHash
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        role = 'performance-report'
        title = $Title
        architecture = 'x64'
        cohortCreatedUtc = $script:TestCohortCreatedUtc
        recordedUtc = '2026-09-01T00:00:00Z'
        rawLogs = @(
            [ordered]@{ name = 'raw-log'; path = $nativeRawLeaf; sha256 = Get-Sha256 $nativeRawPath }
            [ordered]@{ name = 'timing'; path = $nativeTimingLeaf; sha256 = Get-Sha256 $nativeTimingPath }
        )
        provenance = [ordered]@{
            kind = 'native-executable-observation'
            receiptPath = $nativeLeaf
            processId = 33000
            processCreationUtc = '2026-09-01T00:00:00Z'
            executablePath = "installed\$Title.exe"
            executableSha256 = $executableHash
            commandLine = "$Title.exe -headless -stage5-validation"
            exitCode = 0
        }
    }
    Add-Stage5NativeReceiptTestObservations $nativeDocument
    Write-JsonDocument $nativePath $nativeDocument
    $nativeHash = Get-Sha256 $nativePath
    $wrapperDetails = Get-Stage5AcceptanceReceiptTestDetails $Role
    if ($Role -ceq 'validation-results') {
        $wrapperDetails.resultsSha256 = Get-Sha256 $stdoutPath
    }
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-executable-originated-receipt'
        status = 'passed'
        role = $Role
        trustDomain = 'executable'
        producer = 'game-executable-stage5-performance-report-v5'
        producerVersion = '5'
        runNonce = $runNonce
        sourceCommit = $SourceCommit
        title = $Title
        architecture = 'x64'
        artifactSetSha256 = $ArtifactSetSha256
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        executableSha256 = $executableHash
        recordedUtc = '2026-09-01T00:00:00Z'
        rawLogs = @(
            [ordered]@{ name = 'stdout'; path = $stdoutLeaf; sha256 = Get-Sha256 $stdoutPath }
            [ordered]@{ name = 'stderr'; path = $stderrLeaf; sha256 = Get-Sha256 $stderrPath }
        )
        provenance = [ordered]@{
            kind = 'native-executable-observation'
            receiptPath = $nativeLeaf; receiptSha256 = $nativeHash
            processId = 33000; processCreationUtc = '2026-09-01T00:00:00Z'
            executablePath = "installed\$Title.exe"
            executableSha256 = $executableHash
            commandLine = "$Title.exe -headless -stage5-validation"
            exitCode = 0
        }
        details = $wrapperDetails
    })
}

function Add-Stage5NativeReceiptTestObservations {
    param([Collections.IDictionary]$Document)
    $Document.schemaVersion = 5
    $Document.producer = 'game-executable-stage5-performance-report-v5'
    $Document.producerVersion = '5'
    $Document.measurementRole = 'throughput'
    $Document.frames = @{start=0;end=1;final=1;finalCrcKnown=$true;finalCrc=123}
    $Document.fixture = @{id='native-provenance-fixture';requestedPlayerCount=8;requestedMinimumUnitCount=1000
        kind='replay';workloadQualification='minimum-qualified';identityObserved=$true
        contentPath='fixture.rep';contentSha256=('A'*64);replayPath='fixture.rep'
        retainedReplayPath='';retainedReplaySha256='';seed=1729;seedKnown=$true}
    $Document.simulationMode = 'parallel'; $Document.schedulerStarted = $true
    $Document.worker = @{requestedCount=1;effectiveCount=1;policy='auto';pinned=$true
        availableLogicalCpuCount=2;reservedOwnerCpuCount=1;selectedWorkerCpuCount=1
        selectedWorkerPhysicalCoreCount=1;selectedWorkerPhysicalCoreMask=2;selectedWorkerPhysicalCoreMaskComplete=$true}
    $Document.topology = @{source='GetSystemCpuSetInformation';ownerCpuSetIds=@(0);selectedWorkerCpuSetIds=@(1)
        cpuSets=@(@{id=0;efficiencyClass=0;group=0;coreIndex=0;logicalProcessorIndex=0
            parked=$false;allocatedToOtherProcess=$false;availableToProcess=$true},
            @{id=1;efficiencyClass=0;group=0;coreIndex=1;logicalProcessorIndex=1
            parked=$false;allocatedToOtherProcess=$false;availableToProcess=$true})}
    $Document.workload = @{sampling='completed-simulation-frame-boundary-v1';sampleCount=1
        firstFrame=1;lastFrame=1;playerCount=8;initialUnitCount=1000;minimumUnitCount=1000
        peakUnitCount=1000;rosterStable=$true;contiguous=$true}
    $Document.frameSimulation = @{totalNanoseconds=100;maximumNanoseconds=100;sampleCount=1}
    $Document.phases = @(@('owner-intake','legacy-mutable-island','spatial-work','owner-tail',
        'verification-publication') | ForEach-Object {
        @{name=$_;available=$true;totalNanoseconds=10;maximumNanoseconds=10;sampleCount=1
            serialNanoseconds=0;serialNanosecondsKnown=$false}
    })
    $Document.kernelTiming = @{schemaVersion=1;mode='owner-pipeline-observation'
        attribution='owner-stack-exclusive-v1';enabled=$true;frozen=$true;complete=$false
        errors=0;generation=1;serialReferenceKnown=$false;streams=@()}
    $Document.kernelReference = @{schemaVersion=1;mode='throughput-binding';frozen=$true
        complete=$false;errors=0;generation=1;streams=@()}
    $Document.rawEvidence = @{verifierBoundary='closed-native-files'
        rawLogPath=$Document.rawLogs[0].path;rawLogSha256=$Document.rawLogs[0].sha256
        timingPath=$Document.rawLogs[1].path;timingSha256=$Document.rawLogs[1].sha256
        timingClosed=$true;timingWriteSucceeded=$true;timingTruncated=$false;timingComplete=$true
        timingSessionCount=1;timingFrameSamples=2;timingFirstFrame=0;timingLastFrame=1}
}

function New-Stage5SyntheticUuid {
    param([Int64]$Index)
    Assert-True ($Index -ge 1 -and $Index -le 999999999999) `
        'synthetic UUID fixture index must fit the deterministic v4 suffix.'
    return ('00000000-0000-4000-8000-{0:D12}' -f $Index)
}

function New-Stage5SyntheticUtcTimestamp {
    param([int]$Sequence, [int]$TitleOffset = 0, [int]$ExtraTicks = 0)
    $base = [DateTimeOffset]::ParseExact(
        $script:TestCohortCreatedUtc, 'o',
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind)
    $ticks = ([Int64]$TitleOffset * 1000000) +
        ([Int64]$Sequence * 10) + [Int64]$ExtraTicks
    return $base.AddTicks($ticks).UtcDateTime.ToString(
        'yyyy-MM-ddTHH:mm:ss.fffffffZ',
        [Globalization.CultureInfo]::InvariantCulture)
}

function New-Stage5SyntheticQualificationData {
    param(
        [string]$Root,
        [ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [string]$SourceCommit
    )
    $requiredFiles = if ($Title -ceq 'Generals') {
        @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
    }
    else {
        @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    $entries = New-Object 'Collections.Generic.List[object]'
    foreach ($relative in $requiredFiles) {
        $full = Join-Path $Root ($relative.Replace('/', '\'))
        New-Item -ItemType Directory -Path (Split-Path -Parent $full) -Force |
            Out-Null
        [IO.File]::WriteAllText($full, "synthetic qualification data $Title/$relative")
        $entries.Add([ordered]@{
            path = $relative
            sha256 = Get-Sha256 $full
        }) | Out-Null
    }
    $entriesArray = $entries.ToArray()
    [Array]::Sort($entriesArray, [Collections.Generic.Comparer[object]]::Create({
        param($left, $right)
        return [StringComparer]::Ordinal.Compare(
            [string]$left.path, [string]$right.path)
    }))
    $closureLines = @($entriesArray | ForEach-Object {
        '{0}|{1}' -f [string]$_.path, [string]$_.sha256
    })
    $closureSha256 = Get-Sha256Text (($closureLines -join "`n") + "`n")
    $archiveSource = if ($Title -ceq 'Generals') {
        [ordered]@{
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        }
    }
    else {
        [ordered]@{
            object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
            sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
        }
    }
    $manifestPath = Join-Path $Root 'QualificationData.json'
    Write-JsonDocument $manifestPath ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-simulation-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        title = $Title
        archiveSource = $archiveSource
        files = $entriesArray
        closureSha256 = $closureSha256
    })
    return [ordered]@{
        path = 'QualificationData.json'
        title = $Title
        manifestSha256 = Get-Sha256 $manifestPath
        closureSha256 = $closureSha256
        fileCount = 6
        manifestPath = [IO.Path]::GetFullPath($manifestPath)
    }
}

function New-Stage5SyntheticCommandLine {
    param([string]$ExecutablePath, [string[]]$Arguments)
    $parts = @('"' + $ExecutablePath.Replace('"', '""') + '"')
    foreach ($argument in @($Arguments)) {
        if ([string]$argument -match '[\s"]') {
            $parts += '"' + ([string]$argument).Replace('"', '""') + '"'
        }
        else { $parts += [string]$argument }
    }
    return $parts -join ' '
}

function Get-Stage5SyntheticResultTreeSha256 {
    param(
        [object[]]$Results,
        [ValidateSet('replay', 'ai')][string]$Kind
    )
    $lines = New-Object 'Collections.Generic.List[string]'
    $orderedResults = @($Results | Where-Object {
                [string]$_.kind -ceq $Kind
            } | Sort-Object -Property @{
                Expression = { [Int64]$_.sequence }
                Ascending = $true
            })
    foreach ($result in $orderedResults) {
        if ($Kind -ceq 'replay') {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.determinismKey, $result.matrixRepeat,
                $result.repeat, $result.replayResult.finalFrame,
                $result.replayResult.finalCRC)) | Out-Null
        }
        else {
            $lines.Add(('{0}|{1}|{2}|{3}|{4}|{5}' -f
                $result.sequence, $result.scenario, $result.seed,
                $result.configuration, $result.repeat,
                $result.aiEvidence.finalDigest)) | Out-Null
        }
    }
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(
        (($lines.ToArray() -join "`n") + "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Get-Stage5SyntheticWorkerContract {
    param([string]$Configuration)
    switch ($Configuration) {
        'serial-1' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'serial'; requested = '1'; effective = 0
            } }
        'parallel-1' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = '1'; effective = 1
            } }
        'parallel-2' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = '2'; effective = 2
            } }
        'parallel-4' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = '4'; effective = 4
            } }
        'parallel-8' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = '8'; effective = 8
            } }
        'parallel-16' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = '16'; effective = 16
            } }
        'parallel-auto' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'parallel'; requested = 'auto'; effective = 4
            } }
        'shadow-16' { return [pscustomobject]@{
                configuration = $Configuration; mode = 'shadow'; requested = '16'; effective = 16
            } }
        default { throw "Unknown synthetic worker configuration '$Configuration'." }
    }
}

function New-Stage5SyntheticAiCompletionOutput {
    param(
        [object]$Entry,
        [string]$ExecutableHash
    )
    $worker = Get-Stage5SyntheticWorkerContract ([string]$Entry.configuration)
    $authoritative = $worker.mode -ceq 'parallel' -and $worker.effective -gt 1
    $shadow = $worker.mode -ceq 'shadow'
    $common = @{
        Seed = [int]$Entry.seed
        Mode = [string]$worker.mode
        RequestedWorkers = [string]$worker.requested
        EffectiveWorkers = [int]$worker.effective
        Digest = 'A1B2C3D4'
        EndFrame = 42000
        Winner = 1
        Submitted = if ($worker.mode -eq 'serial' -or $worker.requested -eq '1') { 0 } else { 20 }
        Executed = if ($worker.mode -eq 'serial' -or $worker.requested -eq '1') { 0 } else { 20 }
        Fallback = 0
        ExecutableHash = $ExecutableHash
        Scenario = [string]$Entry.scenario
        ActualAi = if ([string]$Entry.scenario -ceq '4v2') { 6 } else { 7 }
        ActualTeams = [string]$Entry.scenario
        RequestedSimulation = [string]$worker.mode
        EffectiveSimulation = [string]$worker.mode
        AuthoritativeCommits = if ($authoritative) { 5 } else { 0 }
        AiCommittedBatches = if ($authoritative) { 5 } else { 0 }
        AiParallelAuthoritativeCommits = if ($authoritative) { 5 } else { 0 }
        AiRequestedBatches = if ($authoritative) { 5 } else { 0 }
        AiSubmitted = if ($authoritative) { 20 } else { 0 }
        AiCompleted = if ($authoritative) { 20 } else { 0 }
        ShadowExecutions = if ($shadow) { 3 } else { 0 }
        CollisionShadowExecutions = if ($shadow) { 3 } else { 0 }
        CollisionShadowComparedCandidates = if ($shadow) { 6 } else { 0 }
        PhysicsShadowExecutions = if ($shadow) { 3 } else { 0 }
        StatusShadowExecutions = if ($shadow) { 3 } else { 0 }
    }
    return New-AiCompletionOutput @common
}

function New-Stage5SyntheticReplayOutput {
    param([object]$Entry)
    $worker = Get-Stage5SyntheticWorkerContract ([string]$Entry.configuration)
    $effectiveMode = if ($worker.mode -eq 'serial') { 'serial' } else { 'parallel' }
    $scheduler = if ($worker.mode -eq 'serial') { 0 } else { 1 }
    $submitted = if ($worker.mode -eq 'serial' -or $worker.requested -eq '1') { 0 } else { 20 }
    $metrics = New-ReplayMetricOutput `
        -Mode ([string]$worker.mode) `
        -EffectiveMode $effectiveMode `
        -Scheduler $scheduler `
        -Workers ([int]$worker.effective) `
        -Submitted $submitted `
        -Executed $submitted `
        -Fallback 0 -ReplayArgument ([string]$Entry.replayArgument)
    $result = New-ReplayResultOutput -FinalFrame 42000 -FinalCRC '01020304' `
        -ReplayArgument ([string]$Entry.replayArgument)
    return $metrics + "`n" + $result
}

function New-Stage5SyntheticAcceptanceCorpus {
    param(
        [string]$Root,
        [ValidateSet('Generals', 'ZeroHour')][string]$Title,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [string]$ExecutablePath
    )
    $sourceRoot = [IO.Path]::GetFullPath($Root)
    New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null
    $executableRole = if ($Title -ceq 'Generals') {
        'generals-executable'
    }
    else { 'zerohour-executable' }
    $executableHash = ([string]$ArtifactHashes[$executableRole]).ToUpperInvariant()
    $recordedRuntimeRoot = Join-Path $sourceRoot 'recorded-runtime'
    New-Item -ItemType Directory -Path $recordedRuntimeRoot -Force | Out-Null
    $recordedExecutablePath = Join-Path $recordedRuntimeRoot `
        ([IO.Path]::GetFileName($ExecutablePath))
    [IO.File]::Copy($ExecutablePath, $recordedExecutablePath)
    Assert-True ((Get-Sha256 $recordedExecutablePath) -ceq $executableHash) `
        "$Title synthetic recorded executable changed while it was copied"
    $qualificationDataInfo = New-Stage5SyntheticQualificationData `
        $sourceRoot $Title $SourceCommit
    $qualificationData = [ordered]@{
        path = [string]$qualificationDataInfo.path
        title = [string]$qualificationDataInfo.title
        manifestSha256 = [string]$qualificationDataInfo.manifestSha256
        closureSha256 = [string]$qualificationDataInfo.closureSha256
        fileCount = [int]$qualificationDataInfo.fileCount
    }
    $reviewedRoot = Join-Path $sourceRoot 'reviewed'
    $reviewed = Write-Stage5ReviewedFixtureReceiptTestDocument `
        (Join-Path $reviewedRoot 'receipt.json') $SourceCommit `
        $ArtifactSetSha256 $ArtifactHashes $Title
    $reviewedRoot = [IO.Path]::GetFullPath($reviewedRoot)

    $liveQualification = [ordered]@{
        schemaVersion = 1
        profileSetId = 'live-all-slices-v1'
        authorityEntries = @([ordered]@{
            scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
        })
        shadowEntry = [ordered]@{
            scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
        }
    }
    $workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
        'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
    $fixtures = @(0..9 | ForEach-Object {
        $id = "fixture-$_"
        $manifest = Read-TestJson $reviewed.manifestPath
        @($manifest.fixtures | Where-Object { [string]$_.id -ceq $id })[0]
    })
    $planEntries = New-Object 'Collections.Generic.List[object]'
    foreach ($configurationName in $workerConfigurations) {
        $worker = Get-Stage5SyntheticWorkerContract $configurationName
        $commonArguments = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
            '-simulationMode', [string]$worker.mode, '-workerPolicy', 'auto',
            '-validationExecutableSha256', $executableHash)
        if ($worker.requested -ne 'auto') {
            $commonArguments += @('-workerCount', [string]$worker.requested)
        }
        for ($matrixRepeat = 1; $matrixRepeat -le 2; ++$matrixRepeat) {
            foreach ($fixture in $fixtures) {
                $fixtureRepeats = if ([bool]$fixture.stress) { 3 } else { 1 }
                for ($repeat = 1; $repeat -le $fixtureRepeats; ++$repeat) {
                    $sequence = $planEntries.Count + 1
                    $arguments = @($commonArguments) + @(
                        '-replay', "Stage5Validation\$($fixture.id).rep")
                    $streamLeaf = '{0:D4}.stdout.log' -f $sequence
                    $stderrLeaf = '{0:D4}.stderr.log' -f $sequence
                    $entry = [pscustomobject]@{
                        sequence = $sequence; kind = 'replay'
                        caseId = "$($fixture.id)-p$matrixRepeat"
                        determinismKey = [string]$fixture.id
                        configuration = $configurationName
                        simulationMode = [string]$worker.mode
                        requestedWorkers = [string]$worker.requested
                        workerPolicy = 'auto'; repeat = $repeat
                        matrixRepeat = $matrixRepeat
                        replayArgument = [string]$arguments[$arguments.Count - 1]
                        fixtureSha256 = ([string]$fixture.sha256).ToUpperInvariant()
                        stress = [bool]$fixture.stress; seed = 0; scenario = ''
                        timeoutSeconds = 300; arguments = [string[]]$arguments
                        command = New-Stage5SyntheticCommandLine $recordedExecutablePath $arguments
                        stdout = Join-Path $sourceRoot "streams\$streamLeaf"
                        stderr = Join-Path $sourceRoot "streams\$stderrLeaf"
                        timingDirectory = Join-Path $sourceRoot "timing\$sequence"
                        runtimeLogDirectory = Join-Path $sourceRoot "runtime-logs\$sequence"
                    }
                    $planEntries.Add($entry) | Out-Null
                }
            }
        }
        foreach ($scenario in @('4v2', '4v3')) {
            foreach ($seed in @(1729, 1730, 1731)) {
                for ($repeat = 1; $repeat -le 2; ++$repeat) {
                    $sequence = $planEntries.Count + 1
                    $runnerFlag = if ($scenario -ceq '4v2') {
                        '-runSkirmishAITest4v2'
                    }
                    else { '-runSkirmishAITest' }
                    $arguments = @($commonArguments) + @($runnerFlag, [string]$seed)
                    $streamLeaf = '{0:D4}.stdout.log' -f $sequence
                    $stderrLeaf = '{0:D4}.stderr.log' -f $sequence
                    $authority = $scenario -ceq '4v2' -and
                        $seed -eq 1729 -and $configurationName -ceq 'parallel-2' -and
                        $repeat -eq 2
                    $entry = [pscustomobject]@{
                        sequence = $sequence; kind = 'ai'
                        caseId = "$scenario-seed-$seed"
                        determinismKey = "$scenario-seed-$seed"
                        configuration = $configurationName
                        simulationMode = [string]$worker.mode
                        requestedWorkers = [string]$worker.requested
                        workerPolicy = 'auto'; repeat = $repeat; matrixRepeat = 0
                        replayArgument = ''; seed = [int]$seed; scenario = $scenario
                        fixtureSha256 = ''; stress = ($scenario -ceq '4v2')
                        timeoutSeconds = 600; arguments = [string[]]$arguments
                        command = New-Stage5SyntheticCommandLine $recordedExecutablePath $arguments
                        stdout = Join-Path $sourceRoot "streams\$streamLeaf"
                        stderr = Join-Path $sourceRoot "streams\$stderrLeaf"
                        timingDirectory = Join-Path $sourceRoot "timing\$sequence"
                        runtimeLogDirectory = Join-Path $sourceRoot "runtime-logs\$sequence"
                        entryId = "ai-{0:D4}" -f $sequence
                        validationRole = if ($authority) {
                            'live-authority-stress'
                        } else { 'live-determinism' }
                        proofProfileId = if ($authority) {
                            'live-all-slices-authority-v1'
                        } else { 'live-invariants-v1' }
                    }
                    $planEntries.Add($entry) | Out-Null
                }
            }
        }
    }
    $shadowSequence = $planEntries.Count + 1
    $shadowWorker = Get-Stage5SyntheticWorkerContract 'shadow-16'
    $shadowArguments = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
        '-simulationMode', 'shadow', '-workerPolicy', 'auto',
        '-validationExecutableSha256', $executableHash,
        '-workerCount', '16', '-runSkirmishAITest4v2', '1729')
    $shadowEntry = [pscustomobject]@{
        sequence = $shadowSequence; kind = 'ai'
        caseId = '4v2-shadow-seed-1729'; determinismKey = '4v2-seed-1729'
        configuration = 'shadow-16'; simulationMode = 'shadow'; requestedWorkers = '16'
        workerPolicy = 'auto'; repeat = 1; matrixRepeat = 0; replayArgument = ''
        seed = 1729; scenario = '4v2'; fixtureSha256 = ''; stress = $true
        timeoutSeconds = 600; arguments = [string[]]$shadowArguments
        command = New-Stage5SyntheticCommandLine $recordedExecutablePath $shadowArguments
        stdout = Join-Path $sourceRoot ('streams\{0:D4}.stdout.log' -f $shadowSequence)
        stderr = Join-Path $sourceRoot ('streams\{0:D4}.stderr.log' -f $shadowSequence)
        timingDirectory = Join-Path $sourceRoot "timing\$shadowSequence"
        runtimeLogDirectory = Join-Path $sourceRoot "runtime-logs\$shadowSequence"
        entryId = "ai-{0:D4}" -f $shadowSequence
        validationRole = 'live-shadow-stress'; proofProfileId = 'live-all-slices-shadow-v1'
    }
    $planEntries.Add($shadowEntry) | Out-Null

    $planObject = [pscustomobject]@{
        schemaVersion = 2; gateName = 'deterministic-runtime';
        generatedUtc = $script:TestCohortCreatedUtc
        cohortNonce = $script:TestCohortNonce
        cohortCreatedUtc = $script:TestCohortCreatedUtc
        runtimeClosure = $script:TestRuntimeClosure
        qualificationData = $qualificationData
         runtimeRoot = [IO.Path]::GetFullPath($recordedRuntimeRoot)
         executable = [IO.Path]::GetFullPath($recordedExecutablePath)
        title = $Title; capacityMode = 'Canonical'; validationMode = 'Canonical'
        executableSha256 = $executableHash; executableSha256Source = 'synthetic-artifact'
        fixtureManifest = [IO.Path]::GetFullPath($reviewed.manifestPath)
        validationSet = 'All'; replayCorpusRequired = $true; replayFixtureCount = 10
        replayMatrixRepeats = 2; stressRepeats = 3; x64Required = $true
        performanceRequested = $true; performanceRequiredForDeterministicRuntimeGate = $true
        performanceMeasurementScope = 'aggregate-stage5-stress-replay-throughput'
        collisionSpecificReplayPerformanceClaim = $false; diagnosticNonAcceptance = $false
        diagnosticWorkerConfiguration = $null; directExecutionExceptionRequested = $false
        frameTimingRequired = $true; authoritativeWorkEvidenceRequired = $true
        deterministicRuntimeEligible = $true; finalAcceptanceEligible = $false
        acceptanceReceiptRequested = $true; acceptanceReceiptEligible = $true
        acceptanceSourceCommit = $SourceCommit
        acceptanceArtifactSetSha256 = $ArtifactSetSha256
        taskRoot = $sourceRoot; launcherContract = $null; physicalCoreCount = 16
        logicalProcessorCount = 16; entries = $planEntries.ToArray()
        liveQualification = $liveQualification
    }
    $planPath = Join-Path $sourceRoot 'validation-plan.json'
    Write-JsonDocument $planPath $planObject
    $planHash = Get-Sha256 $planPath

    $titleOffset = if ($Title -ceq 'Generals') { 100 } else { 200 }
    $children = New-Object 'Collections.Generic.List[object]'
    $results = New-Object 'Collections.Generic.List[object]'
    foreach ($entry in $planEntries) {
        $sequence = [int]$entry.sequence
        $childNonce = New-Stage5SyntheticUuid (($titleOffset * 10000) + $sequence)
        $processId = if ($Title -ceq 'Generals') { 40000 + $sequence } else { 50000 + $sequence }
        $processCreationUtc = New-Stage5SyntheticUtcTimestamp $sequence $titleOffset
        $streamDirectory = Split-Path -Parent $entry.stdout
        New-Item -ItemType Directory -Path $streamDirectory -Force | Out-Null
        $output = if ($entry.kind -ceq 'replay') {
            New-Stage5SyntheticReplayOutput $entry
        }
        else { New-Stage5SyntheticAiCompletionOutput $entry $executableHash }
        [IO.File]::WriteAllText($entry.stdout, $output + "`n",
            (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText($entry.stderr,
            "synthetic stderr for $Title sequence $sequence`n",
            (New-Object Text.UTF8Encoding($false)))
        $nativeDirectory = Join-Path $sourceRoot 'native'
        New-Item -ItemType Directory -Path $nativeDirectory -Force | Out-Null
        $nativeLeaf = "native\{0:D4}.json" -f $sequence
        $nativePath = Join-Path $sourceRoot $nativeLeaf
        $nativeRawLeaf = '{0:D4}.raw.log' -f $sequence
        $nativeTimingLeaf = '{0:D4}.timing.log' -f $sequence
        $nativeRawPath = Join-Path $nativeDirectory $nativeRawLeaf
        $nativeTimingPath = Join-Path $nativeDirectory $nativeTimingLeaf
        [IO.File]::WriteAllText($nativeRawPath,
            "synthetic native raw evidence for $Title sequence $sequence`n",
            (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText($nativeTimingPath,
            "synthetic native timing evidence for $Title sequence $sequence`n",
            (New-Object Text.UTF8Encoding($false)))
        $nativeDocument = [ordered]@{
            schemaVersion = 1
            evidenceKind = 'stage5-executable-originated-receipt'
            status = 'passed'
            producer = 'game-executable-stage5-performance-report-v5'
            producerVersion = '5'
            runNonce = $childNonce
            sourceCommit = $SourceCommit
            artifactSetSha256 = $ArtifactSetSha256
            executableSha256 = $executableHash
            cohortNonce = $script:TestCohortNonce
            runtimeClosure = $script:TestRuntimeClosure
            role = 'performance-report'; title = $Title; architecture = 'x64'
            cohortCreatedUtc = $script:TestCohortCreatedUtc
            recordedUtc = $processCreationUtc
            rawLogs = @(
                [ordered]@{ name = 'raw-log'; path = $nativeRawLeaf
                    sha256 = Get-Sha256 $nativeRawPath }
                [ordered]@{ name = 'timing'; path = $nativeTimingLeaf
                    sha256 = Get-Sha256 $nativeTimingPath }
            )
            provenance = [ordered]@{
                kind = 'native-executable-observation'; receiptPath = $nativeLeaf
                processId = $processId; processCreationUtc = $processCreationUtc
                executablePath = [IO.Path]::GetFullPath($recordedExecutablePath)
                executableSha256 = $executableHash
                commandLine = [string]$entry.command; exitCode = 0
            }
        }
        Add-Stage5NativeReceiptTestObservations $nativeDocument
        Write-JsonDocument $nativePath $nativeDocument
        $nativeHash = Get-Sha256 $nativePath
        $child = [ordered]@{
            sequence = $sequence; role = 'validation-results'; title = $Title
            runNonce = $childNonce; processId = $processId
            processCreationUtc = $processCreationUtc
            executablePath = [IO.Path]::GetFullPath($recordedExecutablePath)
            executableSha256 = $executableHash; commandLine = [string]$entry.command
            arguments = [string[]]$entry.arguments; exitCode = 0
            stdout = [ordered]@{
                path = ConvertTo-OutputRelativePath $entry.stdout $sourceRoot `
                    'Synthetic validation stdout'
                sha256 = Get-Sha256 $entry.stdout
            }
            stderr = [ordered]@{
                path = ConvertTo-OutputRelativePath $entry.stderr $sourceRoot `
                    'Synthetic validation stderr'
                sha256 = Get-Sha256 $entry.stderr
            }
            nativeReceipt = [ordered]@{
                path = $nativeLeaf; sha256 = $nativeHash
                producer = 'game-executable-stage5-performance-report-v5'
                runNonce = $childNonce; cohortNonce = $script:TestCohortNonce
            }
            qualificationData = $qualificationData
        }
        $children.Add($child) | Out-Null
        $replayMetrics = $null; $replayResult = $null; $aiEvidence = $null
        if ($entry.kind -ceq 'replay') {
            $replayMetrics = ConvertFrom-Stage5ReplayMetrics $output $entry
            $replayResult = ConvertFrom-Stage5ReplayResult $output $entry
        }
        else {
            $aiEvidence = ConvertFrom-Stage5AiCompletion $output $entry `
                $executableHash $true $planObject
        }
        $executionProvenance = [ordered]@{
            schemaVersion = 1; sequence = $sequence; role = 'validation-results'
            title = $Title; runNonce = $childNonce; processId = $processId
            processCreationUtc = $processCreationUtc
         executablePath = [IO.Path]::GetFullPath($recordedExecutablePath)
            executableSha256 = $executableHash; commandLine = [string]$entry.command
            arguments = [string[]]$entry.arguments; exitCode = 0
            stdout = $child.stdout; stderr = $child.stderr
            nativeReceipt = $child.nativeReceipt
            plan = [ordered]@{ path = 'validation-plan.json'; sha256 = $planHash }
            sourceCommit = $SourceCommit; artifactSetSha256 = $ArtifactSetSha256
            cohortNonce = $script:TestCohortNonce
            cohortCreatedUtc = $script:TestCohortCreatedUtc
            runtimeClosure = $script:TestRuntimeClosure
            qualificationData = $qualificationData
        }
        $resultDocument = [ordered]@{
            sequence = $sequence; title = $Title; kind = [string]$entry.kind
            caseId = [string]$entry.caseId; determinismKey = [string]$entry.determinismKey
            configuration = [string]$entry.configuration
            simulationMode = [string]$entry.simulationMode
            requestedWorkers = [string]$entry.requestedWorkers
            workerPolicy = 'auto'; repeat = [int]$entry.repeat
            matrixRepeat = [int]$entry.matrixRepeat
            replayArgument = [string]$entry.replayArgument; seed = [int]$entry.seed
            scenario = [string]$entry.scenario
            fixtureSha256 = [string]$entry.fixtureSha256
            stress = [bool]$entry.stress; exitCode = 0; timedOut = $false
            wallMilliseconds = 100
            stdoutSha256 = [string]$child.stdout.sha256
            stderrSha256 = [string]$child.stderr.sha256
            aiEvidence = $aiEvidence; replayMetrics = $replayMetrics
            replayResult = $replayResult
            timingEvidence = [ordered]@{ status = 'synthetic-closed'; maximumFrameEnd = 42000 }
            executionProvenance = $executionProvenance
        }
        if ($entry.kind -ceq 'ai') {
            # Preserve the exact identity already frozen into the V2 plan. Do
            # not synthesize a second result identity or add nullable V2 fields
            # to legacy replay entries.
            $resultDocument.entryId = [string]$entry.entryId
            $resultDocument.validationRole = [string]$entry.validationRole
            $resultDocument.proofProfileId = [string]$entry.proofProfileId
        }
        $results.Add([pscustomobject]$resultDocument) | Out-Null
    }
    $resultsPath = Join-Path $sourceRoot 'validation-results.json'
    Write-JsonDocument $resultsPath $results.ToArray()
    $resultsHash = Get-Sha256 $resultsPath
    $replayResults = @($results | Where-Object { $_.kind -ceq 'replay' })
    $aiResults = @($results | Where-Object { $_.kind -ceq 'ai' })
    $replayTree = Get-Stage5SyntheticResultTreeSha256 `
        $replayResults 'replay'
    $aiTree = Get-Stage5SyntheticResultTreeSha256 $aiResults 'ai'
    $rawResultBinding = [ordered]@{
        name = 'validation-results.json'; path = 'validation-results.json'
        sha256 = $resultsHash
    }
    $streamBindings = @($children | ForEach-Object {
        [ordered]@{ name = [IO.Path]::GetFileName([string]$_.stdout.path)
            path = [string]$_.stdout.path
            sha256 = [string]$_.stdout.sha256 }
        [ordered]@{ name = [IO.Path]::GetFileName([string]$_.stderr.path)
            path = [string]$_.stderr.path
            sha256 = [string]$_.stderr.sha256 }
    })
    $validationRawLogs = @($rawResultBinding) + @($streamBindings)
    Assert-True ($validationRawLogs.Count -eq 507) `
        "$Title synthetic validation corpus retains exactly 507 raw logs"
    $wrapperBase = if ($Title -ceq 'Generals') { 300 } else { 400 }
    $receiptPlans = @(
        [pscustomobject]@{ role = 'validation-plan'; path = (Join-Path $sourceRoot 'validation-plan-receipt.json'); nonce = New-Stage5SyntheticUuid (($wrapperBase * 10000) + 1); raw = @([ordered]@{ name = 'validation-plan.json'; path = 'validation-plan.json'; sha256 = $planHash }); details = [ordered]@{ gateName = 'deterministic-runtime'; validationSet = 'All'; entryCount = 253; qualificationData = $qualificationData } },
        [pscustomobject]@{ role = 'validation-results'; path = (Join-Path $sourceRoot 'validation-results-receipt.json'); nonce = New-Stage5SyntheticUuid (($wrapperBase * 10000) + 2); raw = $validationRawLogs; details = [ordered]@{ resultCount = 253; allExecutionsPassed = $true; resultsSha256 = $resultsHash; qualificationData = $qualificationData } },
        [pscustomobject]@{ role = 'replay-results'; path = (Join-Path $sourceRoot 'replay-results-receipt.json'); nonce = New-Stage5SyntheticUuid (($wrapperBase * 10000) + 3); raw = @($rawResultBinding); details = [ordered]@{ uniqueReplayCount = 10; executionCount = 168; crcTreeSha256 = $replayTree; allExecutionsPassed = $true; qualificationData = $qualificationData } },
        [pscustomobject]@{ role = 'ai-results'; path = (Join-Path $sourceRoot 'ai-results-receipt.json'); nonce = New-Stage5SyntheticUuid (($wrapperBase * 10000) + 4); raw = @($rawResultBinding); details = [ordered]@{ scenarioCount = 2; distinctSeedCount = 3; repeatCount = 2; allGamesCompleted = $true; digestTreeSha256 = $aiTree; qualificationData = $qualificationData } }
    )
    foreach ($receiptPlan in $receiptPlans) {
        $receiptChildren = @()
        $childProvenance = 'not-applicable'
        if ($receiptPlan.role -ceq 'validation-results') {
            $receiptChildren = $children.ToArray(); $childProvenance = 'bound'
        }
        elseif ($receiptPlan.role -in @('replay-results', 'ai-results')) {
            $sourceChild = $children[0]
            $receiptChildren = @([ordered]@{
                role = $receiptPlan.role; title = $Title
                runNonce = [string]$sourceChild.runNonce
                processId = [int]$sourceChild.processId
                processCreationUtc = [string]$sourceChild.processCreationUtc
                executablePath = [string]$sourceChild.executablePath
                executableSha256 = [string]$sourceChild.executableSha256
                commandLine = [string]$sourceChild.commandLine; exitCode = 0
                stdout = $sourceChild.stdout; stderr = $sourceChild.stderr
                nativeReceipt = $sourceChild.nativeReceipt
                qualificationData = $qualificationData
            }); $childProvenance = 'bound'
        }
        $receiptDocument = [ordered]@{
            schemaVersion = 1; evidenceKind = 'stage5-host-runner-receipt'; status = 'passed'
            role = [string]$receiptPlan.role; trustDomain = 'host-runner'
            producer = "installed-runtime-$($receiptPlan.role)-v2"; producerVersion = '2'
            runNonce = [string]$receiptPlan.nonce; sourceCommit = $SourceCommit
            title = $Title; architecture = 'x64'; artifactSetSha256 = $ArtifactSetSha256
            cohortNonce = $script:TestCohortNonce; runtimeClosure = $script:TestRuntimeClosure
            executableSha256 = $executableHash
            recordedUtc = $script:TestCohortCreatedUtc; rawLogs = $receiptPlan.raw
            provenance = [ordered]@{
                kind = 'host-runner-observation'; runner = 'Run-DeterministicSimulationValidation.ps1'
                runnerVersion = '1'; childProvenance = $childProvenance
                children = $receiptChildren
            }
            details = $receiptPlan.details
        }
        Write-JsonDocument $receiptPlan.path $receiptDocument
    }
    $resultsReceipt = $receiptPlans[1]
    return [pscustomobject]@{
        title = $Title; sourceRoot = $sourceRoot
         executablePath = [IO.Path]::GetFullPath($recordedExecutablePath)
        executableSha256 = $executableHash
        qualificationData = $qualificationData
        qualificationDataPath = [IO.Path]::GetFullPath((Join-Path $sourceRoot $qualificationData.path))
        reviewed = $reviewed; reviewedRoot = $reviewedRoot
        planPath = $planPath; planSha256 = $planHash
        resultsPath = $resultsPath; resultsSha256 = $resultsHash
        validationReceiptPath = [string]$resultsReceipt.path
        validationReceiptSha256 = Get-Sha256 ([string]$resultsReceipt.path)
        replayReceiptPath = [string]$receiptPlans[2].path
        aiReceiptPath = [string]$receiptPlans[3].path
        plan = $planObject; entries = $planEntries.ToArray()
        children = $children.ToArray(); results = $results.ToArray()
        replayTreeSha256 = $replayTree; aiTreeSha256 = $aiTree
        rawLogCount = $validationRawLogs.Count
        receiptPlans = $receiptPlans
    }
}

function Assert-NativeObservationProcessBinding {
    # Load only pure transport helpers. Never execute the runner's process,
    # profile, registry, or installed-runtime workflow in this regression.
    $parseTokens = $null; $parseErrors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'),
        [ref]$parseTokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) 'native observation runner parses without errors'
    $helpersPresent = $true
    foreach ($name in @('Get-NativeObservationBinding','Set-NativePerformanceObservationEnvironment')) {
        $definition = $tree.Find({param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true)
        Assert-True ($null -ne $definition) "native observation has a pure $name boundary"
        if ($null -eq $definition) { $helpersPresent = $false }
    }
    $processDefinition = $tree.Find({param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-ValidationProcess'
    }, $true)
    Assert-True (@($processDefinition.Body.ParamBlock.Parameters | Where-Object {
        $_.Name.VariablePath.UserPath -ceq 'NativeObservationBinding'
    }).Count -eq 1) 'installed process accepts an explicit observation binding without canonical acceptance flags'
    if (-not $helpersPresent) { return }
    foreach ($name in @('Get-RequiredProperty','Assert-JsonObjectShape','Assert-JsonString',
        'Test-Sha256Text','Assert-CanonicalUuid','Get-NativeObservationBinding',
        'Set-NativePerformanceFixtureEnvironment','Set-NativePerformanceObservationEnvironment')) {
        $definition = $tree.Find({param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true)
        Invoke-Expression $definition.Extent.Text
    }
    $binding = [ordered]@{sourceCommit=('a'*40);artifactSetSha256=('b'*64)
        runtimeClosure=[ordered]@{dependencyManifestSha256=('c'*64);closureSha256=('d'*64)}}
    $copies = @(Get-NativeObservationBinding $binding)
    Assert-True ($copies.Count -eq 1 -and $copies[0].artifactSetSha256 -ceq ('B'*64) -and
        $copies[0].runtimeClosure.closureSha256 -ceq ('D'*64) -and
        $binding.artifactSetSha256 -ceq ('b'*64)) `
        'observation validation returns exactly one normalized binding without changing caller evidence'
    $copies[0].runtimeClosure.closureSha256 = 'modified-copy'
    Assert-True ($binding.runtimeClosure.closureSha256 -ceq ('d'*64)) `
        'observation binding owns its nested closure copy'
    Assert-True ($null -eq (Get-NativeObservationBinding $null)) 'omitted observation binding remains disabled'
    foreach ($mutate in @(
        {param($value) $value.sourceCommit='A'*40},
        {param($value) $value.artifactSetSha256=' ' + ('B'*64)},
        {param($value) $value.runtimeClosure.Remove('closureSha256')},
        {param($value) $value.runtimeClosure.dependencyManifestSha256=123},
        {param($value) $value.finalAcceptanceEligible=$true},
        {param($value) $value.runtimeClosure.acceptanceReceiptRequested=$true}
    )) {
        $invalid = @{sourceCommit=('a'*40);artifactSetSha256=('b'*64)
            runtimeClosure=@{dependencyManifestSha256=('c'*64);closureSha256=('d'*64)}}
        & $mutate $invalid
        Assert-Throws { Get-NativeObservationBinding $invalid | Out-Null } 'observation|Observation' `
            'incomplete, malformed, or qualification-bearing observation bindings are rejected'
    }
    $observationEvidenceRoot = Join-Path $root 'observation-unit-test'
    $observationTimingDirectory = Join-Path $observationEvidenceRoot 'timing-7'
    $entry = @{kind='replay';caseId='observed-replay';fixtureSha256=('E'*64);seed=0
        sequence=7;timingDirectory=$observationTimingDirectory}
    $environment = @{'RTS_PERFORMANCE_RUN_ID'='stale';'RTS_PERFORMANCE_SEED'='999'
        'RTS_PERFORMANCE_REFERENCE_MODE'='serial-oracle';'RTS_PERFORMANCE_UNIT_COUNT'='8000'
        'RTS_PERFORMANCE_UNKNOWN_INHERITED'='stale';'unrelated'='preserved'}
    $cohort = 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'
    $firstNonce = '11111111-1111-4111-8111-111111111111'
    $secondNonce = '22222222-2222-4222-8222-222222222222'
    $arguments = @{Environment=$environment;Binding=$binding;Entry=$entry
        EvidenceRoot=$observationEvidenceRoot;RunNonce=$firstNonce
        CohortNonce=$cohort;CohortCreatedUtc='2026-09-01T00:00:00Z'}
    $directories = @(Set-NativePerformanceObservationEnvironment @arguments)
    $observationReceiptRoot = Join-Path $observationEvidenceRoot 'native-performance-receipts'
    $expectedReceiptDirectory = Join-Path $observationReceiptRoot $firstNonce
    Assert-True ($directories.Count -eq 1 -and $directories[0] -ceq
        $expectedReceiptDirectory -and
        $environment['RTS_PERFORMANCE_RECEIPT_DIR'] -ceq $directories[0] -and
        $environment['RTS_PERFORMANCE_RAW_LOG_PATH'] -ceq (Join-Path $directories[0] 'performance-raw.log') -and
        $environment['RTS_PERFORMANCE_TIMING_PATH'] -ceq $entry.timingDirectory -and
        $environment['RTS_PERFORMANCE_RUN_NONCE'] -ceq $firstNonce -and
        $environment['RTS_PERFORMANCE_RUN_ID'] -ceq "stage5-7-$firstNonce" -and
        $environment['RTS_PERFORMANCE_COHORT_NONCE'] -ceq $cohort -and
        $environment['RTS_PERFORMANCE_SOURCE_COMMIT'] -ceq ('a'*40) -and
        $environment['RTS_PERFORMANCE_ARTIFACT_SET_SHA256'] -ceq ('B'*64) -and
        $environment['RTS_PERFORMANCE_RUNTIME_CLOSURE_SHA256'] -ceq ('D'*64) -and
        $environment['RTS_PERFORMANCE_REFERENCE_MODE'] -ceq 'throughput-binding' -and
        $environment['RTS_PERFORMANCE_WORKLOAD_QUALIFICATION'] -ceq 'observed-only' -and
        -not $environment.ContainsKey('RTS_PERFORMANCE_SEED') -and
        -not $environment.ContainsKey('RTS_PERFORMANCE_UNIT_COUNT') -and
        -not $environment.ContainsKey('RTS_PERFORMANCE_UNKNOWN_INHERITED') -and
        $environment['unrelated'] -ceq 'preserved') `
        'local observation gets exact fresh child paths and verified binding without inherited oracle or workload claims'
    $arguments.RunNonce = $secondNonce
    $secondDirectory = Set-NativePerformanceObservationEnvironment @arguments
    Assert-True ($secondDirectory -cne $directories[0] -and
        $environment['RTS_PERFORMANCE_RUN_ID'] -ceq "stage5-7-$secondNonce" -and
        $environment['RTS_PERFORMANCE_RAW_LOG_PATH'] -notmatch $firstNonce) `
        'a second child cannot reuse the prior receipt or raw path'
    $beforeInvalid = $environment | ConvertTo-Json -Compress
    $arguments.Binding = @{sourceCommit='malformed'}
    Assert-Throws { Set-NativePerformanceObservationEnvironment @arguments | Out-Null } 'observation|Observation' `
        'malformed observation binding fails before changing child environment'
    Assert-True (($environment | ConvertTo-Json -Compress) -ceq $beforeInvalid) `
        'failed binding validation leaves the child environment untouched'
    $arguments.Binding = $null
    $disabled = Set-NativePerformanceObservationEnvironment @arguments
    Assert-True ($null -eq $disabled -and
        @($environment.Keys | Where-Object { $_ -like 'RTS_PERFORMANCE_*' }).Count -eq 0 -and
        -not $environment.ContainsKey('RTS_STAGE5_RUNTIME_CLOSURE_SHA256') -and
        -not $environment.ContainsKey('RTS_STAGE5_RUNTIME_MANIFEST_SHA256') -and
        $environment['unrelated'] -ceq 'preserved') `
        'disabled observation cannot accidentally inherit native receipt authorization'
    $processSource = $processDefinition.Extent.Text
    Assert-True ($processSource -notmatch '\$(?:acceptanceBindingsRequested|AcceptanceSourceCommit|AcceptanceArtifactSetSha256|hostRunnerRuntimeClosure)\b' -and
        $processSource -match 'Set-NativePerformanceObservationEnvironment' -and
        $processSource -match '-SourceCommit\s+\$nativeBinding.sourceCommit' -and
        $processSource -match '-ArtifactSetSha256\s+\$nativeBinding.artifactSetSha256' -and
        $processSource -match '-RuntimeClosure\s+\$nativeBinding.runtimeClosure') `
        'environment and receipt parser consume the same explicit observation binding, not acceptance globals'
    Assert-True ($tree.Extent.Text -match '-NativeObservationBinding\s+\$nativeObservationBinding' -and
        $tree.Extent.Text -match 'LocalCapacity cannot request canonical acceptance bindings or receipts\.') `
        'canonical caller passes its binding explicitly while LocalCapacity acceptance remains forbidden'
}

function Assert-CurrentNativeReceiptCatalog {
    param([string]$Directory, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes)
    Assert-NativeObservationProcessBinding
    # Execute only the real receipt parser and its pure file helpers. Never
    # invoke the runner's top-level registry/process workflow from a unit test.
    $parseTokens = $null; $parseErrors = $null
    $runnerTree = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'),
        [ref]$parseTokens, [ref]$parseErrors)
    $environmentHelper = $runnerTree.Find({param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Set-NativePerformanceFixtureEnvironment'
    }, $true)
    if ($null -eq $environmentHelper) {
        Assert-True $false 'generic runner has a tested observed-only fixture environment boundary'
    } else {
        Invoke-Expression $environmentHelper.Extent.Text
        $environment = @{'RTS_PERFORMANCE_PLAYER_COUNT'='8';'RTS_PERFORMANCE_UNIT_COUNT'='8000'
            'RTS_PERFORMANCE_SEED'='0';'unrelated'='preserved'}
        Set-NativePerformanceFixtureEnvironment $environment @{kind='replay';caseId='reviewed-replay';fixtureSha256=('A'*64);seed=0}
        Assert-True ($environment['RTS_PERFORMANCE_WORKLOAD_QUALIFICATION'] -ceq 'observed-only' -and
            $environment['RTS_PERFORMANCE_FIXTURE_KIND'] -ceq 'replay' -and
            $environment['RTS_PERFORMANCE_FIXTURE_SHA256'] -ceq ('A'*64) -and
            -not $environment.ContainsKey('RTS_PERFORMANCE_SEED') -and
            -not $environment.ContainsKey('RTS_PERFORMANCE_PLAYER_COUNT') -and
            -not $environment.ContainsKey('RTS_PERFORMANCE_UNIT_COUNT') -and $environment['unrelated'] -ceq 'preserved') `
            'generic replay cannot inherit workload minima or claim plan placeholder seed zero as observed'
        Set-NativePerformanceFixtureEnvironment $environment @{kind='ai';caseId='4v3-seed-1729';fixtureSha256='';seed=1729}
        Assert-True ($environment['RTS_PERFORMANCE_FIXTURE_KIND'] -ceq 'fresh-ai-map' -and
            $environment['RTS_PERFORMANCE_SEED'] -ceq '1729' -and
            -not $environment.ContainsKey('RTS_PERFORMANCE_FIXTURE_SHA256')) `
            'fresh AI carries its expected seed without fabricating the yet-unobserved map hash'
    }
    $parserCommand = ''
    foreach ($name in @('Get-Sha256Bytes','Get-Stage5FileSnapshot','ConvertTo-OutputRelativePath',
        'Assert-ContainedPathNoReparse')) {
        $definition = $runnerTree.Find({param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $true)
        Invoke-Expression $definition.Extent.Text
    }
    $definition = $runnerTree.Find({param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        @('Get-NativeV2ReceiptReference','Get-NativePerformanceReceiptReference') -ccontains $node.Name
    }, $true)
    Invoke-Expression $definition.Extent.Text
    $parserCommand = $definition.Name
    foreach ($domain in @('host-runner','executable')) {
        $path = Join-Path $Directory "current-native-$domain.json"
        if ($domain -ceq 'host-runner') {
            Write-Stage5HostReceiptTestDocument $path 'validation-results' 'ZeroHour' `
                $SourceCommit $ArtifactSetSha256 $ArtifactHashes
        } else {
            Write-Stage5ExecutableReceiptTestDocument $path 'validation-results' 'ZeroHour' `
                $SourceCommit $ArtifactSetSha256 $ArtifactHashes
        }
        $wrapper = ConvertFrom-Stage5JsonDictionary $path
        $nativeBinding = if ($domain -ceq 'host-runner') { $wrapper.provenance.children[0].nativeReceipt } else { $wrapper.provenance }
        $nativePath = if ($domain -ceq 'host-runner') {
            Join-Path $Directory $nativeBinding.path
        } else { Join-Path $Directory $nativeBinding.receiptPath }
        $native = ConvertFrom-Stage5JsonDictionary $nativePath
        $native.schemaVersion=1; $native.producer='game-executable-stage5-performance-report-v2'; $native.producerVersion='2'
        $publish = {
            Write-JsonDocument $nativePath $native
            if ($domain -ceq 'host-runner') {
                $nativeBinding.sha256=Get-Sha256 $nativePath; $nativeBinding.producer=$native.producer
            } else {
                $nativeBinding.receiptSha256=Get-Sha256 $nativePath
                $wrapper.producer=$native.producer; $wrapper.producerVersion=$native.producerVersion
            }
            Write-JsonDocument $path $wrapper
        }
        & $publish
        $readArguments = @{Path=$path;Kind='deterministic-runtime';Role='validation-results'
            EvidenceTitle='ZeroHour';ExpectedSourceCommit=$SourceCommit
            ExpectedArtifactSetSha256=$ArtifactSetSha256;ArtifactHashes=$ArtifactHashes}
        Assert-Throws { Read-Stage5FinalAcceptanceImmutableReceipt @readArguments } 'producer|version|V5|obsolete' `
            "$domain cannot promote an obsolete hash-bound native receipt"
        $parserArguments = @{OutputText="SIMULATION_PERFORMANCE_RECEIPT status=written path=$nativePath"
            OutputRoot=$Directory;WorkingDirectory=$Directory;Role='validation-results'
            SourceCommit=$SourceCommit;ArtifactSetSha256=$ArtifactSetSha256
            ExecutableSha256=$ArtifactHashes['zerohour-executable'];RunNonce=$native.runNonce
            CohortNonce=$native.cohortNonce;RuntimeClosure=$script:TestRuntimeClosure;ExpectedTitle='ZeroHour'
            ProcessId=$native.provenance.processId;ProcessCreationUtc=$native.provenance.processCreationUtc
            ExpectedExecutablePath=$native.provenance.executablePath}
        Assert-True ($null -eq (& $parserCommand @parserArguments)) `
            'runner child reader rejects obsolete native protocol despite correct raw hashes'
        Add-Stage5NativeReceiptTestObservations $native
        & $publish
        try {
            $proof = Read-Stage5FinalAcceptanceImmutableReceipt @readArguments
            Assert-True ($proof.trustDomain -ceq $domain) `
                'current V5 throughput provenance with no admitted streams remains valid non-scaling evidence'
        } catch { Assert-True $false "current V5 throughput provenance was rejected: $($_.Exception.Message)" }
        $nativeProcessId = [int]$native.provenance.processId
        $native.provenance.processId = [double]$nativeProcessId + 0.5
        & $publish
        Assert-Throws {
            Read-Stage5FinalAcceptanceImmutableReceipt @readArguments | Out-Null
        } 'processId must be an integer' `
            "$domain native provenance rejects a fractional processId"
        $native.provenance.processId = $nativeProcessId
        $native.provenance.exitCode = 0.5
        & $publish
        Assert-Throws {
            Read-Stage5FinalAcceptanceImmutableReceipt @readArguments | Out-Null
        } 'exitCode must be an integer' `
            "$domain native provenance rejects a fractional exitCode"
        $native.provenance.exitCode = 0
        & $publish
        $parsedReferences = @(& $parserCommand @parserArguments)
        Assert-True ($parsedReferences.Count -eq 1 -and $null -ne $parsedReferences[0] -and
            $parsedReferences[0].producer -ceq 'game-executable-stage5-performance-report-v5' -and
            $parsedReferences[0].path -ceq [IO.Path]::GetFileName($nativePath) -and
            $parsedReferences[0].sha256 -ceq (Get-Sha256 $nativePath)) `
            'runner child reader returns exactly one hash-bound V5 reference without guard-path output pollution'
        $marker = "SIMULATION_PERFORMANCE_RECEIPT status=written path=$nativePath"
        $lineEndingCases = [ordered]@{
            'no trailing newline' = $marker
            'LF' = $marker + "`n"
            'CRLF' = $marker + "`r`n"
            # Invoke-ValidationProcess concatenates the actual redirected
            # Windows stdout, a LF separator, and redirected stderr verbatim.
            'combined Windows process output' =
                "SKIRMISH_AI_TEST_COMPLETE end_frame=3`r`nSIMULATION_JOB_METRICS failures=0`r`n" +
                $marker + "`r`n" + "`n" + "diagnostic stderr line`r`n"
        }
        foreach ($case in $lineEndingCases.GetEnumerator()) {
            $lineArguments = $parserArguments.Clone()
            $lineArguments.OutputText = $case.Value
            $references = @(& $parserCommand @lineArguments)
            Assert-True ($references.Count -eq 1 -and $null -ne $references[0] -and
                $references[0].path -ceq [IO.Path]::GetFileName($nativePath) -and
                $references[0].sha256 -ceq (Get-Sha256 $nativePath) -and
                $references[0].runNonce -ceq $native.runNonce -and
                $references[0].cohortNonce -ceq $native.cohortNonce) `
                "$domain runner child reader verifies full V5 provenance with $($case.Key)"
        }
        foreach ($mismatch in @('RunNonce','CohortNonce','SourceCommit','ArtifactSetSha256',
            'ExecutableSha256','ProcessId','ProcessCreationUtc','ExpectedTitle','RuntimeClosure')) {
            $invalidArguments = $parserArguments.Clone()
            $invalidArguments.OutputText = $marker + "`r`n"
            switch ($mismatch) {
                'RunNonce' { $invalidArguments.RunNonce = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb' }
                'CohortNonce' { $invalidArguments.CohortNonce = 'cccccccc-cccc-4ccc-8ccc-cccccccccccc' }
                'SourceCommit' { $invalidArguments.SourceCommit = ('F' * 40) }
                'ArtifactSetSha256' { $invalidArguments.ArtifactSetSha256 = ('0' * 64) }
                'ExecutableSha256' { $invalidArguments.ExecutableSha256 = ('0' * 64) }
                'ProcessId' { $invalidArguments.ProcessId = $native.provenance.processId + 1 }
                'ProcessCreationUtc' { $invalidArguments.ProcessCreationUtc = '2026-09-01T00:00:01Z' }
                'ExpectedTitle' { $invalidArguments.ExpectedTitle = 'Generals' }
                'RuntimeClosure' {
                    $invalidArguments.RuntimeClosure = @{
                        dependencyManifestSha256 = ('0' * 64); closureSha256 = ('0' * 64)
                    }
                }
            }
            Assert-True ($null -eq (& $parserCommand @invalidArguments)) `
                "$domain CRLF marker does not bypass mismatched $mismatch provenance"
        }
        foreach ($mode in @('serial','parallel','shadow')) {
            Add-Stage5NativeReceiptTestObservations $native
            $native.fixture.workloadQualification='observed-only'
            $native.fixture.requestedPlayerCount=$null; $native.fixture.requestedMinimumUnitCount=$null
            $native.workload.playerCount=7; $native.workload.initialUnitCount=0; $native.workload.minimumUnitCount=0
            $native.simulationMode=$mode
            if ($mode -ceq 'serial') {
                $native.schedulerStarted=$false
                foreach ($field in @('effectiveCount','availableLogicalCpuCount','reservedOwnerCpuCount',
                    'selectedWorkerCpuCount','selectedWorkerPhysicalCoreCount','selectedWorkerPhysicalCoreMask')) { $native.worker[$field]=0 }
                $native.worker.pinned=$false; $native.worker.selectedWorkerPhysicalCoreMaskComplete=$false
                $native.topology=@{source='scheduler-not-started';cpuSets=@();ownerCpuSetIds=@();selectedWorkerCpuSetIds=@()}
            }
            & $publish
            try {
                $proof = Read-Stage5FinalAcceptanceImmutableReceipt @readArguments
                Assert-True ($proof.trustDomain -ceq $domain) "$domain accepts explicit observed-only $mode without fabricated workload or workers"
            } catch { Assert-True $false "$domain observed-only $mode rejected: $($_.Exception.Message)" }
            $parsedReferences=@(& $parserCommand @parserArguments)
            Assert-True ($parsedReferences.Count -eq 1 -and $null -ne $parsedReferences[0]) `
                "runner child reader retains observed-only $mode provenance"
        }
        foreach ($mutation in @('unobserved-fixture','unknown-qualification','missing-seed','map-as-replay','serial-workers')) {
            Add-Stage5NativeReceiptTestObservations $native
            switch ($mutation) {
                'unobserved-fixture' { $native.fixture.identityObserved=$false }
                'unknown-qualification' { $native.fixture.workloadQualification='assumed-eight' }
                'missing-seed' { $native.fixture.seedKnown=$false }
                'map-as-replay' { $native.fixture.kind='fresh-ai-map';$native.fixture.contentPath='Maps/Test/Test.map' }
                'serial-workers' { $native.simulationMode='serial';$native.schedulerStarted=$false }
            }
            & $publish
            Assert-Throws { Read-Stage5FinalAcceptanceImmutableReceipt @readArguments } 'fixture|seed|qualif|scheduler|worker|replay' `
                "$domain rejects $mutation without inventing observed metadata"
            Assert-True ($null -eq (& $parserCommand @parserArguments)) "runner child reader rejects $mutation"
            $crlfArguments = $parserArguments.Clone()
            $crlfArguments.OutputText = $marker + "`r`n"
            Assert-True ($null -eq (& $parserCommand @crlfArguments)) "CRLF runner child reader rejects $mutation"
        }
        foreach ($mutation in @('oracle','disabled','error','incomplete','unclosed')) {
            Add-Stage5NativeReceiptTestObservations $native
            switch ($mutation) {
                'oracle' { $native.measurementRole='serial-oracle';$native.kernelReference.mode='serial-oracle' }
                'disabled' { $native.kernelReference.mode='disabled' }
                'error' { $native.kernelReference.errors=64 }
                'incomplete' { $native.kernelReference.frozen=$false }
                'unclosed' { $native.rawEvidence.timingClosed=$false }
            }
            & $publish
            Assert-Throws { Read-Stage5FinalAcceptanceImmutableReceipt @readArguments } 'role|oracle|reference|timing|finalized|producer|version' `
                "$domain rejects $mutation native evidence without inventing coverage"
            Assert-True ($null -eq (& $parserCommand @parserArguments)) `
                "runner child reader rejects $mutation native evidence"
            $crlfArguments = $parserArguments.Clone()
            $crlfArguments.OutputText = $marker + "`r`n"
            Assert-True ($null -eq (& $parserCommand @crlfArguments)) `
                "CRLF runner child reader rejects $mutation native evidence"
        }
    }
}

function Write-Stage5ProtectedAttestationTestDocument {
    param(
        [string]$Path, [string]$Kind, [string]$Role, [string]$TrustDomain,
        [string]$SourceCommit, [string]$ArtifactSetSha256, [string]$Title
    )
    $attestationLeaf = [IO.Path]::GetFileNameWithoutExtension($Path) + '.attestation.json'
    $attestationPath = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Path))) $attestationLeaf
    $protectionKind = switch ($TrustDomain) {
        'reviewed-fixture' { 'external-reviewed-fixture-attestation' }
        'premium-review' { 'external-premium-review-attestation' }
        default { 'external-user-manual-approval-attestation' }
    }
    $subjectKey = '{0}|{1}|{2}|{3}|{4}' -f $SourceCommit,
        $ArtifactSetSha256.ToUpperInvariant(), $Kind, $Role, $Title
    Write-JsonDocument $attestationPath ([ordered]@{
        schemaVersion = 1; evidenceKind = 'stage5-external-attestation'
        trustDomain = $TrustDomain; role = $Role; sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256; subjectKey = $subjectKey
        authority = if ($TrustDomain -ceq 'manual-approval') {
            'user-approval-authority'
        }
        elseif ($TrustDomain -ceq 'premium-review') {
            'premium-review-authority'
        }
        else { 'fixture-review-authority' }
        issuedUtc = '2026-09-01T00:00:00Z'
    })
    return [ordered]@{
        kind = $protectionKind; path = $attestationLeaf
        sha256 = Get-Sha256 $attestationPath
    }
}

function Write-Stage5ReviewedFixtureReceiptTestDocument {
    param(
        [string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes,
        [ValidateSet('Generals', 'ZeroHour')][string]$Title = 'ZeroHour'
    )
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $fixtureDirectory = Join-Path $directory 'reviewed-fixtures'
    New-Item -ItemType Directory -Path $fixtureDirectory -Force | Out-Null
    $fixtureEntries = @()
    for ($index = 0; $index -lt 10; ++$index) {
        $fixtureLeaf = "fixture-$index.rep"
        $fixturePath = Join-Path $fixtureDirectory $fixtureLeaf
        [IO.File]::WriteAllText($fixturePath, "reviewed fixture $Title $index")
        $fixtureEntries += [ordered]@{
            id = "fixture-$index"; source = "reviewed-fixtures\\$fixtureLeaf"
            sha256 = Get-Sha256 $fixturePath; stress = ($index -eq 0)
        }
    }
    $manifestLeaf = 'reviewed-fixture-manifest.json'
    $manifestPath = Join-Path $directory $manifestLeaf
    $executableName = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    $executableRole = if ($Title -ceq 'Generals') {
        'generals-executable'
    }
    else { 'zerohour-executable' }
    $liveQualification = [ordered]@{
        schemaVersion = 1
        profileSetId = 'live-all-slices-v1'
        authorityEntries = @([ordered]@{
            scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
        })
        shadowEntry = [ordered]@{
            scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
        }
    }
    Write-JsonDocument $manifestPath ([ordered]@{
        schemaVersion = 2; title = $Title; executable = $executableName
        executableSha256 = [string]$ArtifactHashes[$executableRole]
        fixtures = $fixtureEntries
        ai = [ordered]@{
            seeds = @(1729, 1730, 1731); scenarios = @('4v3', '4v2'); repeats = 2
            liveQualification = $liveQualification
        }
    })
    $manifestHash = Get-Sha256 $manifestPath
    $protection = Write-Stage5ProtectedAttestationTestDocument $Path 'replay-determinism' `
        'replay-fixture-manifest' 'reviewed-fixture' $SourceCommit $ArtifactSetSha256 $Title
    $protectionPath = Join-Path $directory ([string]$protection.path)
    $protectionDocument = Read-TestJson $protectionPath
    $protectionDocument.issuedUtc = '2026-08-01T00:00:00.0000000Z'
    Write-JsonDocument $protectionPath $protectionDocument
    $protection.sha256 = Get-Sha256 $protectionPath
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1; evidenceKind = 'stage5-reviewed-fixture-receipt'; status = 'passed'
        role = 'replay-fixture-manifest'; trustDomain = 'reviewed-fixture'
        producer = 'reviewed-replay-fixture-manifest-v2'; producerVersion = '2'
        sourceCommit = $SourceCommit; title = $Title; architecture = 'x64'
        artifactSetSha256 = $ArtifactSetSha256
        # Reviewed replay fixtures are static protected inputs, not products of
        # the current randomized runner cohort.  Their source/artifact/runtime
        # bindings remain current even when their review cohort and timestamp
        # predate the execution envelope that consumes them.
        cohortNonce = 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb'
        runtimeClosure = $script:TestRuntimeClosure
        recordedUtc = '2026-08-01T00:00:00.0000000Z'
        provenance = [ordered]@{
            kind = 'reviewed-fixture'; reviewedBy = 'fixture-reviewer'
            reviewedUtc = '2026-08-01T00:00:00.0000000Z'
            fixtureManifest = [ordered]@{ path = $manifestLeaf; sha256 = $manifestHash }
        }
        protection = $protection
        details = [ordered]@{
            fixtureCount = 10; stressFixtureCount = 1; fixtureSetSha256 = $manifestHash
        }
    })
    return [ordered]@{
        receiptPath = [IO.Path]::GetFullPath($Path)
        receiptSha256 = Get-Sha256 $Path
        protectionPath = [IO.Path]::GetFullPath($protectionPath)
        protectionSha256 = [string]$protection.sha256
        manifestPath = [IO.Path]::GetFullPath($manifestPath)
        manifestSha256 = $manifestHash
        fixtureCount = 10
    }
}

function Write-Stage5ExternalReceiptTestDocument {
    param(
        [string]$Path, [string]$Kind, [string]$Role, [string]$TrustDomain,
        [string]$SourceCommit, [string]$ArtifactSetSha256
    )
    $title = 'Both'
    $details = if ($Role -eq 'premium-review-results') {
        [ordered]@{
            reviewedCommit = $SourceCommit; reviewRounds = 2; independentReviewers = 9
            openP0 = 0; openP1 = 0; openP2 = 0
        }
    }
    else {
        [ordered]@{
            approvalScope = 'final-stage5-installed-runtime'
            candidateHashVerified = $true; bothTitlesTested = $true; cleanExitPassed = $true
        }
    }
    $protection = Write-Stage5ProtectedAttestationTestDocument $Path $Kind $Role `
        $TrustDomain $SourceCommit $ArtifactSetSha256 $title
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1
        evidenceKind = if ($TrustDomain -ceq 'premium-review') {
            'stage5-premium-review-receipt'
        }
        else { 'stage5-manual-approval-receipt' }
        status = 'passed'; role = $Role; trustDomain = $TrustDomain
        producer = if ($TrustDomain -ceq 'premium-review') {
            'stage5-premium-review-receipt-v2'
        }
        else { 'installed-runtime-manual-acceptance-v2' }
        producerVersion = '2'; sourceCommit = $SourceCommit; title = $title
        architecture = 'x64'; artifactSetSha256 = $ArtifactSetSha256
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        recordedUtc = '2026-09-01T00:00:00Z'
        provenance = [ordered]@{
            kind = $TrustDomain; reviewedBy = if ($TrustDomain -ceq 'manual-approval') {
                'manual-tester'
            }
            else { 'premium-reviewer' }
            reviewedUtc = '2026-09-01T00:00:00Z'
        }
        protection = $protection; details = $details
    })
}

function Update-LockstepFixtureFnv {
    param([Numerics.BigInteger]$Hash, [UInt64]$Value, [int]$Bytes,
        [Numerics.BigInteger]$Prime, [Numerics.BigInteger]$Mask)
    $updated = $Hash
    for ($byteIndex = 0; $byteIndex -lt $Bytes; ++$byteIndex) {
        $updated = (($updated -bxor
            ([Numerics.BigInteger]($Value -band 255))) * $Prime) -band $Mask
        $Value = $Value -shr 8
    }
    return $updated
}

function Get-LockstepFixtureCommandDigest {
    param([Collections.IDictionary]$Pairs)
    Add-Type -AssemblyName System.Numerics
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $hash = Update-LockstepFixtureFnv $hash ([UInt64]$slot) 4 $prime $mask
        foreach ($field in @('command_count', 'first_command_frame',
            'last_command_frame')) {
            [UInt64]$value = [UInt64]::Parse([string]$Pairs["peer_${slot}_${field}"])
            $hash = Update-LockstepFixtureFnv $hash $value 4 $prime $mask
        }
        [UInt64]$lastId = [UInt64]::Parse([string]$Pairs["peer_${slot}_last_command_id"])
        $hash = Update-LockstepFixtureFnv $hash $lastId 2 $prime $mask
        [UInt64]$hasLast = if ($Pairs["peer_${slot}_has_last_command_id"] -ceq '1') { 1 } else { 0 }
        $hash = Update-LockstepFixtureFnv $hash $hasLast 4 $prime $mask
        foreach ($field in @('last_command_digest', 'command_digest')) {
            [UInt64]$value = [UInt64]::Parse([string]$Pairs["peer_${slot}_${field}"])
            $hash = Update-LockstepFixtureFnv $hash $value 8 $prime $mask
        }
    }
    return $hash.ToString()
}

function Get-LockstepFixtureAIPlanningDigest {
    param([Collections.IDictionary]$Pairs)
    Add-Type -AssemblyName System.Numerics
    [Numerics.BigInteger]$hash = [Numerics.BigInteger]::Parse('14695981039346656037')
    [Numerics.BigInteger]$prime = [Numerics.BigInteger]::Parse('1099511628211')
    [Numerics.BigInteger]$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
    $hash = Update-LockstepFixtureFnv $hash ([UInt64]$Pairs.simulation_roster_mask) 4 $prime $mask
    $hash = Update-LockstepFixtureFnv $hash ([UInt64]$Pairs.ai_roster_mask) 4 $prime $mask
    foreach ($field in @(
        'captured_snapshots', 'captured_candidates', 'requested_batches',
        'submitted_jobs', 'completed_jobs', 'serial_fallbacks',
        'shadow_matches', 'shadow_mismatches', 'validation_failures',
        'canonical_validation_invocations', 'committed_batches',
        'parallel_authoritative_commits', 'rejected_commits',
        'owner_helped_executions')) {
        $hash = Update-LockstepFixtureFnv $hash ([UInt64]$Pairs["ai_planning_$field"]) 8 $prime $mask
    }
    return $hash.ToString()
}

function Get-LockstepFixtureProjectionSha256 {
    param([Collections.IDictionary]$Pairs)
    $projection = [ordered]@{}
    foreach ($key in @('mode', 'schema', 'protocol_epoch', 'peer_count', 'roster_mask',
        'simulation_roster_mask', 'ai_roster_mask', 'build_compatibility_crc',
        'content_crc', 'map_crc', 'common_stop_frame',
        'proven_kernel_mask', 'packet_router_slot', 'origin_mode', 'session_nonce',
        'executable_sha256', 'source_revision', 'final_frame', 'frame_count',
        'contributed_peer_mask', 'checkpoint_count', 'validation_authority_mask',
        'executable_origin', 'worker_telemetry_executable_origin',
        'transport_path_used', 'handshake_validated', 'clean_shutdown',
        'ai_planning_captured_snapshots', 'ai_planning_captured_candidates',
        'ai_planning_requested_batches', 'ai_planning_submitted_jobs',
        'ai_planning_completed_jobs', 'ai_planning_serial_fallbacks',
        'ai_planning_shadow_matches', 'ai_planning_shadow_mismatches',
        'ai_planning_validation_failures',
        'ai_planning_canonical_validation_invocations',
        'ai_planning_committed_batches',
        'ai_planning_parallel_authoritative_commits',
        'ai_planning_rejected_commits',
        'ai_planning_physical_worker_executions',
        'ai_planning_owner_helped_executions',
        'ai_planning_observed_physical_worker_mask',
        'ai_planning_maximum_distinct_physical_workers',
        'ai_planning_maximum_concurrent_physical_workers',
        'ai_planning_digest')) {
        $projection[$key] = $Pairs[$key]
    }
    for ($slot = 0; $slot -lt 8; ++$slot) {
        foreach ($suffix in @('command_count', 'first_command_frame',
            'last_command_frame', 'last_command_id', 'has_last_command_id',
            'last_command_digest', 'command_digest')) {
            $projection["peer_${slot}_${suffix}"] = $Pairs["peer_${slot}_${suffix}"]
        }
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        foreach ($suffix in @('frame', 'crc', 'command_digest')) {
            $projection["checkpoint_${checkpoint}_${suffix}"] =
                $Pairs["checkpoint_${checkpoint}_${suffix}"]
        }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($projection | ConvertTo-Json -Compress -Depth 5))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function New-LockstepFixtureReceipt {
    param(
        [string]$Path,
        [int]$LocalSlot,
        [int]$WorkerCount,
        [string]$RunNonce,
        [string]$SessionNonce,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [uint32]$MapCrc,
        [int]$NetworkToken = 1
    )
    $pairs = [ordered]@{
        producer = 'installed-lockstep-v2'
        mode = 'installed-lockstep-v2-production'
        schema = '2'; protocol_epoch = '2'; local_slot = [string]$LocalSlot
        peer_count = '2'; roster_mask = '3'; simulation_roster_mask = '63'
        ai_roster_mask = '60'; build_compatibility_crc = '1'
        content_crc = '1'; map_crc = [string]$MapCrc; common_stop_frame = '4096'
        proven_kernel_mask = '63'; packet_router_slot = '0'; origin_mode = '2'
        run_nonce = $RunNonce; session_nonce = $SessionNonce
        executable_sha256 = $ExecutableSha256.ToUpperInvariant()
        source_revision = $SourceCommit
        network_session_token = [string]$NetworkToken
        final_frame = '4096'; frame_count = '4096'; contributed_peer_mask = '3'
        checkpoint_count = '129'; validation_authority_mask = '63'
        executable_origin = '1'; worker_telemetry_executable_origin = '1'
        transport_path_used = '1'; handshake_validated = '1'; clean_shutdown = '1'
        ai_planning_captured_snapshots = '4'
        ai_planning_captured_candidates = '8'
        ai_planning_requested_batches = '4'
        ai_planning_submitted_jobs = '4'
        ai_planning_completed_jobs = '4'
        ai_planning_serial_fallbacks = '0'
        ai_planning_shadow_matches = '4'
        ai_planning_shadow_mismatches = '0'
        ai_planning_validation_failures = '0'
        ai_planning_canonical_validation_invocations = '4'
        ai_planning_committed_batches = '4'
        ai_planning_parallel_authoritative_commits = '4'
        ai_planning_rejected_commits = '0'
        ai_planning_physical_worker_executions = '4'
        ai_planning_owner_helped_executions = '0'
        ai_planning_observed_physical_worker_mask = '3'
        ai_planning_maximum_distinct_physical_workers = '2'
        ai_planning_maximum_concurrent_physical_workers = '2'
        ai_planning_digest = '0'
    }
    $pairs['ai_planning_digest'] = Get-LockstepFixtureAIPlanningDigest $pairs
    for ($slot = 0; $slot -lt 8; ++$slot) {
        $isRoster = $slot -lt 2
        $pairs["peer_${slot}_command_count"] = if ($isRoster) { '1' } else { '0' }
        $pairs["peer_${slot}_first_command_frame"] = if ($isRoster) { '8' } else { '0' }
        $pairs["peer_${slot}_last_command_frame"] = if ($isRoster) { '4096' } else { '0' }
        $pairs["peer_${slot}_last_command_id"] = if ($isRoster) { [string]($slot + 1) } else { '0' }
        $pairs["peer_${slot}_has_last_command_id"] = if ($isRoster) { '1' } else { '0' }
        $pairs["peer_${slot}_last_command_digest"] = if ($isRoster) { [string](1001 + $slot) } else { '0' }
        $pairs["peer_${slot}_command_digest"] = if ($isRoster) { [string](2001 + $slot) } else { '0' }
    }
    for ($kernel = 0; $kernel -lt 6; ++$kernel) {
        $pairs["kernel_${kernel}_physical_worker_mask"] = [string]$(if ($WorkerCount -eq 2) { 3 } else { 15 })
        $pairs["kernel_${kernel}_physical_worker_jobs"] = '4096'
        $pairs["kernel_${kernel}_distinct_physical_workers"] = [string]$WorkerCount
        $pairs["kernel_${kernel}_peak_concurrent_physical_workers"] = [string]$WorkerCount
        $pairs["kernel_${kernel}_physical_worker_mask_complete"] = '1'
    }
    for ($checkpoint = 0; $checkpoint -lt 129; ++$checkpoint) {
        $frame = if ($checkpoint -eq 0) { 1 } else { $checkpoint * 32 }
        $pairs["checkpoint_${checkpoint}_frame"] = [string]$frame
        $pairs["checkpoint_${checkpoint}_crc"] = [string](100000 + $checkpoint)
        $pairs["checkpoint_${checkpoint}_command_digest"] = [string](300000 + $checkpoint)
    }
    $digest = Get-LockstepFixtureCommandDigest $pairs
    $pairs['checkpoint_128_command_digest'] = $digest
    $lines = New-Object 'Collections.Generic.List[string]'
    [void]$lines.Add('RTS_LOCKSTEP_V2_RECEIPT')
    foreach ($key in $pairs.Keys) { [void]$lines.Add("$key=$($pairs[$key])") }
    [void]$lines.Add('END')
    $text = ($lines -join "`n") + "`n"
    [IO.File]::WriteAllText($Path, $text, (New-Object Text.UTF8Encoding($false)))
    return [pscustomobject]@{
        path = $Path; pairs = $pairs; finalCRC = [UInt32]100128
        projectionSha256 = Get-LockstepFixtureProjectionSha256 $pairs
    }
}

function New-LockstepFixtureLauncherContract {
    param(
        [string]$Title,
        [string]$RuntimeDirectory,
        [string]$ExecutableSha256,
        [string]$LauncherSha256,
        [string]$ConfigSha256
    )
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    $executable = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    $executablePath = Join-Path $runtimeFull $executable
    [ordered]@{
        schemaVersion = 1; mode = 'headless-direct-exception'
        configPath = Join-Path $runtimeFull 'launcher.lcf'
        configSha256 = $ConfigSha256
        launcherPath = Join-Path $runtimeFull 'launcher.exe'
        launcherSha256 = $LauncherSha256
        directory = '.'; executable = $executable
        launcherTarget = $executablePath
        launcherArguments = @('-simulationMode', 'parallel', '-workerPolicy', 'auto')
        launcherWorkingDirectory = $runtimeFull
        directExecutable = $executablePath
        directWorkingDirectory = $runtimeFull
        directArguments = @('-simulationMode', 'parallel', '-workerPolicy', 'auto')
        childExitCodeObserved = $true
    }
}

function New-LockstepFixtureTitleSessionProfile {
    param([string]$Title, [string]$TitleRoot, [string]$RuntimeDirectory)
    $sessionRoot = Join-Path ([IO.Path]::GetFullPath($TitleRoot)) 'TitleSession'
    $runtimeFull = [IO.Path]::GetFullPath($RuntimeDirectory)
    $documentsRoot = Join-Path $sessionRoot 'Documents'
    $profileLeaf = if ($Title -ceq 'Generals') {
        'Command and Conquer Generals Data'
    } else { 'GGC-LockstepV2-ZeroHour' }
    $profileRoot = Join-Path $documentsRoot $profileLeaf
    $environmentValues = [ordered]@{
        TEMP = Join-Path $sessionRoot 'Temp'
        TMP = Join-Path $sessionRoot 'Tmp'
        LOCALAPPDATA = Join-Path $sessionRoot 'LocalAppData'
        APPDATA = Join-Path $sessionRoot 'AppData'
        USERPROFILE = $sessionRoot; HOMEDRIVE = 'H:'
        HOMEPATH = $sessionRoot.Substring(2)
        RTS_STAGE5_VALIDATION_PROFILE_ROOT = $profileRoot
        RTS_STAGE5_VALIDATION_CACHE_ROOT = Join-Path $sessionRoot 'Cache'
        RTS_STAGE5_VALIDATION_LOG_ROOT = Join-Path $sessionRoot 'Logs'
        RTS_STAGE5_VALIDATION_DUMP_ROOT = Join-Path $sessionRoot 'Dumps'
        RTS_STAGE5_VALIDATION_TITLE_SESSION_ROOT = $sessionRoot
    }
    $registryValues = @()
    if ($Title -ceq 'Generals') {
        $registryValues += ,([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Generals'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        })
    }
    else {
        $registryValues += ,([ordered]@{
            subKey = 'Software\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
            name = 'InstallPath'; value = $runtimeFull + '\'; purpose = 'installed-runtime-binding'
        })
    }
    [ordered]@{
        schemaVersion = 1; title = $Title; sessionRoot = $sessionRoot
        runtimeDirectory = $runtimeFull; documentsRoot = $documentsRoot
        profileLeaf = $profileLeaf; profileRoot = $profileRoot
        peerRoot = Join-Path $sessionRoot 'Peers'
        profileConcurrency = 'shared-title-profile-read-only'
        environmentValues = $environmentValues
        environmentVariableNames = @($environmentValues.Keys)
        registryViews = @('Registry32', 'Registry64')
        registryValues = @($registryValues)
    }
}

function New-LockstepFixturePeerEnvironment {
    param([Collections.IDictionary]$Profile, [int]$PeerIndex)
    $peerRoot = Join-Path $Profile['peerRoot'] "peer-$PeerIndex"
    $values = [ordered]@{}
    foreach ($name in $Profile['environmentVariableNames']) {
        $key = [string]$name
        $base = [string]$Profile['environmentValues'][$key]
        if ($key -ceq 'TEMP' -or $key -ceq 'TMP' -or
            $key -ceq 'LOCALAPPDATA' -or $key -ceq 'APPDATA' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_CACHE_ROOT' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_LOG_ROOT' -or
            $key -ceq 'RTS_STAGE5_VALIDATION_DUMP_ROOT') {
            $values[$key] = Join-Path $peerRoot ([IO.Path]::GetFileName($base))
        } else { $values[$key] = $base }
    }
    [ordered]@{
        peer = $PeerIndex; root = $peerRoot; values = $values
        variableNames = @($Profile['environmentVariableNames'])
    }
}

function New-LockstepFixtureNegativeProbe {
    param(
        [string]$Root,
        [string]$Title,
        [string]$ExecutableSha256,
        [string]$SourceCommit,
        [string]$Mode,
        [int]$ProcessId,
        [uint32]$MapCrc,
        [int]$Seed,
        [string]$RunNonce,
        [string]$SessionNonce
    )
    $negativeRoot = Join-Path (Join-Path $Root $Title) 'NegativeProbes'
    New-Item -ItemType Directory -Path $negativeRoot -Force | Out-Null
    $isCrossEpoch = $Mode -ceq 'negative-cross-epoch'
    $expectedError = if ($isCrossEpoch) { 'UnsupportedEngineEpoch' } else { 'ContentHashMismatch' }
    $mutation = if ($isCrossEpoch) { 'engine-epoch' } else { 'content-hash' }
    $proofLeaf = if ($isCrossEpoch) {
        'cross-epoch.proof'
    } else { 'content-mismatch.proof' }
    $proofPath = Join-Path $negativeRoot $proofLeaf
    $stdoutPath = Join-Path $negativeRoot ($Mode + '.stdout.log')
    $stderrPath = Join-Path $negativeRoot ($Mode + '.stderr.log')
    $baselineInputSha256 = if ($isCrossEpoch) { '1' * 64 } else { '3' * 64 }
    $inputSha256 = if ($isCrossEpoch) { '2' * 64 } else { '4' * 64 }
    $proofLines = @(
        'RTS_LOCKSTEP_V2_NEGATIVE_PROBE'
        "producer=installed-lockstep-v2"
        "mode=$Mode"
        'schema=2'
        'protocol_epoch=2'
        "run_nonce=$RunNonce"
        "session_nonce=$SessionNonce"
        "executable_sha256=$($ExecutableSha256.ToUpperInvariant())"
        "source_revision=$SourceCommit"
        "probe_build_compatibility_crc=$MapCrc"
        "probe_content_crc=$Seed"
        "mutation=$mutation"
        "baseline_input_sha256=$baselineInputSha256"
        "input_sha256=$inputSha256"
        'baseline_accepted=1'
        'mutated_accepted=0'
        "expected_error=$expectedError"
        "observed_error=$expectedError"
        "process_id=$ProcessId"
        'END'
    )
    [IO.File]::WriteAllText($proofPath, (($proofLines -join "`n") + "`n"),
        (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($stdoutPath,
        "LOCKSTEP_V2_NEGATIVE_PROBE_PASS mode=$Mode pid=$ProcessId rejection=$expectedError`n",
        (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($stderrPath, '', (New-Object Text.UTF8Encoding($false)))
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $toRelative = {
        param([string]$Candidate)
        ([IO.Path]::GetFullPath($Candidate).Substring($rootFull.Length)).Replace('\', '/')
    }
    $executableRelative = if ($Title -ceq 'Generals') {
        'GeneralsRuntime\generalsv.exe'
    } else { 'ZeroHourRuntime\generalszh.exe' }
    return [ordered]@{
        title = $Title; mode = $Mode; producer = 'installed-lockstep-v2'
        processId = $ProcessId
        processCreationUtc = '2026-09-01T00:00:00Z'
        executablePath = [IO.Path]::GetFullPath((Join-Path $Root $executableRelative))
        runNonce = $RunNonce; sessionNonce = $SessionNonce
        executableSha256 = $ExecutableSha256.ToUpperInvariant(); sourceCommit = $SourceCommit
        proofPath = & $toRelative $proofPath; proofSha256 = Get-Sha256 $proofPath
        stdoutPath = & $toRelative $stdoutPath; stdoutSha256 = Get-Sha256 $stdoutPath
        stderrPath = & $toRelative $stderrPath; stderrSha256 = Get-Sha256 $stderrPath
        inputSha256 = $inputSha256; baselineAccepted = $true; mutatedAccepted = $false
        mutation = $mutation; expectedError = $expectedError; observedError = $expectedError
        exitCode = 0
        commandLine = "generalsv.exe -installedLockstepV2Validation mode=$Mode"
        arguments = @('-installedLockstepV2Validation', "mode=$Mode")
        probeBuildCrc = $MapCrc; probeContentCrc = $Seed
    }
}

function Get-LockstepFixtureTextSha256 {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)) |
            ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function New-LockstepQualificationDataFixture {
    param(
        [string]$Root,
        [string]$SourceCommit,
        [string]$MapName,
        [Collections.IDictionary]$MapCrcs
    )
    $requiredByTitle = [ordered]@{
        Generals = @('English.big', 'INI.big', 'Maps.big', 'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb')
        ZeroHour = @('INIZH.big', 'MapsZH.big', 'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb', 'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb')
    }
    $entriesByIdentity = @{}
    $entryIndex = 0
    foreach ($title in $requiredByTitle.Keys) {
        $runtimeLeaf = if ($title -ceq 'Generals') {
            'GeneralsRuntime'
        }
        else { 'ZeroHourRuntime' }
        foreach ($relative in $requiredByTitle[$title]) {
            ++$entryIndex
            $path = "$runtimeLeaf/$relative"
            $identity = "$title|$path"
            $hashDigit = '{0:X}' -f $entryIndex
            $entriesByIdentity[$identity] = [ordered]@{
                title = $title
                path = $path
                sha256 = $hashDigit * 64
            }
        }
    }
    [string[]]$identities = @($entriesByIdentity.Keys)
    [Array]::Sort($identities, [StringComparer]::Ordinal)
    $entries = @($identities | ForEach-Object { $entriesByIdentity[$_] })
    $canonicalLines = @($entries | ForEach-Object {
        '{0}|{1}|{2}' -f $_.title, $_.path, $_.sha256
    })
    $closureSha256 = Get-LockstepFixtureTextSha256 `
        (($canonicalLines -join "`n") + "`n")
    $document = [ordered]@{
        schemaVersion = 2
        evidenceKind = 'lockstep-v2-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        mapName = $MapName
        mapCrcs = $MapCrcs
        archiveSources = @(
            [ordered]@{
                title = 'Generals'
                object = 's3://github-ci/generals108_gamedata_trimmed.7z'
                sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
            },
            [ordered]@{
                title = 'ZeroHour'
                object = 's3://github-ci/zerohour104_gamedata_trimmed.7z'
                sha256 = '6837FE1E3009A4C239406C39B1598216C0943EE8ED46BB10626767029AC05E21'
            }
        )
        files = $entries
        closureSha256 = $closureSha256
    }
    $path = Join-Path $Root 'QualificationData.json'
    Write-JsonDocument $path $document
    return [ordered]@{
        manifestSha256 = Get-Sha256 $path
        closureSha256 = $closureSha256
        fileCount = $entries.Count
    }
}

function New-LockstepFixtureEvidence {
    param(
        [string]$Root,
        [string]$SourceCommit,
        [string]$ArtifactSetSha256,
        [Collections.IDictionary]$ArtifactHashes
    )
    [IO.Directory]::CreateDirectory($Root) | Out-Null
    $mapName = 'Maps\Twilight Flame\Twilight Flame.map'
    $mapCrcs = [ordered]@{
        Generals = [UInt32]739101722
        ZeroHour = [UInt32]4042777579
    }
    $qualificationData = New-LockstepQualificationDataFixture $Root `
        $SourceCommit $mapName $mapCrcs
    $launcherContracts = [ordered]@{}
    $sessions = @()
    foreach ($titleIndex in 0..1) {
        $title = if ($titleIndex -eq 0) { 'Generals' } else { 'ZeroHour' }
        $mapCrc = [UInt32]$mapCrcs[$title]
        $titleRoot = Join-Path $Root $title
        $runtimeRoot = Join-Path $Root ("{0}Runtime" -f $title)
        [IO.Directory]::CreateDirectory($titleRoot) | Out-Null
        [IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
        $executableRole = if ($title -ceq 'Generals') {
            'generals-executable'
        } else { 'zerohour-executable' }
        $launcherRole = if ($title -ceq 'Generals') {
            'generals-launcher'
        } else { 'zerohour-launcher' }
        $configRole = if ($title -ceq 'Generals') {
            'generals-launcher-config'
        } else { 'zerohour-launcher-config' }
        $contract = New-LockstepFixtureLauncherContract $title $runtimeRoot `
            $ArtifactHashes[$executableRole] $ArtifactHashes[$launcherRole] `
            $ArtifactHashes[$configRole]
        $launcherContracts[$title] = $contract
        $profile = New-LockstepFixtureTitleSessionProfile $title $titleRoot $runtimeRoot
        $sessionNonce = if ($titleIndex -eq 0) {
            ('A' * 31) + '1'
        } else { ('B' * 31) + '2' }
        $ports = @([int](42000 + ($titleIndex * 2)), [int](42001 + ($titleIndex * 2)))
        $workerProfiles = @(
            [ordered]@{
                profile = 'explicit-two-workers'; requestedWorkers = '2'
                workerPolicy = 'all'; overrideArguments = @('-workerCount', '2', '-workerPolicy', 'all')
            },
            [ordered]@{
                profile = 'automatic-workers'; requestedWorkers = 'auto'
                workerPolicy = 'auto'; overrideArguments = @('-workerPolicy', 'auto')
            }
        )
        $effectiveWorkerCounts = @(2, 4)
        $registryEquivalence = [ordered]@{
            strategy = 'process-local-validation-profile-root'
            views = @($profile['registryViews'])
            values = @($profile['registryValues'])
            profileRoot = $profile['profileRoot']
        }
        $peers = @()
        $projection = $null
        for ($peerIndex = 0; $peerIndex -lt 2; ++$peerIndex) {
            $processId = 51000 + ($titleIndex * 10) + $peerIndex
            $runNonce = if ($titleIndex -eq 0) {
                if ($peerIndex -eq 0) { '1' * 32 } else { '2' * 32 }
            } else {
                if ($peerIndex -eq 0) { '3' * 32 } else { '4' * 32 }
            }
            $receiptLeaf = "lockstep-v2-$title-peer-$peerIndex.receipt"
            $receiptPath = Join-Path $titleRoot $receiptLeaf
            $receipt = New-LockstepFixtureReceipt $receiptPath $peerIndex `
                $effectiveWorkerCounts[$peerIndex] $runNonce $sessionNonce `
                $ArtifactHashes[$executableRole] $SourceCommit `
                $mapCrc (1 + ($titleIndex * 10) + $peerIndex)
            if ($null -eq $projection) { $projection = $receipt.projectionSha256 }
            elseif ($projection -cne $receipt.projectionSha256) {
                throw "Fixture receipt projection unexpectedly differed for $title peer $peerIndex."
            }
            $stdoutPath = Join-Path $titleRoot "peer-$peerIndex.stdout.log"
            $stderrPath = Join-Path $titleRoot "peer-$peerIndex.stderr.log"
            $stdoutActive = "LOCKSTEP_V2_VALIDATION_ACTIVE peer=$peerIndex frame_limit=4096"
            $stdoutPass = "LOCKSTEP_V2_VALIDATION_PASS peer=$peerIndex pid=$processId frame=4096 crc=00018720"
            [IO.File]::WriteAllText($stdoutPath, "$stdoutActive`n$stdoutPass`n",
                (New-Object Text.UTF8Encoding($false)))
            [IO.File]::WriteAllText($stderrPath, "lockstep-v2 peer $peerIndex clean exit`n",
                (New-Object Text.UTF8Encoding($false)))
            $override = $workerProfiles[$peerIndex]
            $arguments = @($contract['launcherArguments'] + $override['overrideArguments'] + @(
                '-installedLockstepV2Validation',
                "peer=$peerIndex;peers=2;ports=$($ports -join ',');run=$runNonce;session=$sessionNonce;exe=$($ArtifactHashes[$executableRole].ToUpperInvariant());source=$SourceCommit;map=$mapName;map_crc=$mapCrc;seed=23063;dir=$titleRoot;receipt=$receiptLeaf;mode=trusted-router;router=0;network_roster=3;simulation_roster=63;ai_roster=60"))
            $commandLine = '"{0}" {1}' -f $contract['directExecutable'], ($arguments -join ' ')
            $telemetryMask = if ($effectiveWorkerCounts[$peerIndex] -eq 2) { 3 } else { 15 }
            $telemetry = [ordered]@{
                requestedWorkers = $override['requestedWorkers']
                workerPolicy = $override['workerPolicy']
                effectiveWorkers = $effectiveWorkerCounts[$peerIndex]
                distinctPhysicalWorkers = @($effectiveWorkerCounts[$peerIndex]) * 6
                physicalWorkerMasks = @($telemetryMask) * 6
                executableOrigin = $true
            }
            $peer = [ordered]@{
                schemaVersion = 2; producer = 'installed-lockstep-v2'
                validationMode = 'installed-lockstep-v2-production'; title = $title
                processId = $processId; peer = $peerIndex; peerCount = 2
                networkRosterMask = 3; simulationRosterMask = 63
                aiRosterMask = 60; aiPlayerCount = 4
                port = $ports[$peerIndex]; runNonce = $runNonce; sessionNonce = $sessionNonce
                executableSha256 = $ArtifactHashes[$executableRole].ToUpperInvariant()
                sourceCommit = $SourceCommit; launcherEquivalence = $contract
                launcherPath = $contract['launcherPath']; launcherSha256 = $contract['launcherSha256']
                launcherConfigPath = $contract['configPath']; launcherConfigSha256 = $contract['configSha256']
                directExecutionOptIn = $true; workingDirectory = $contract['directWorkingDirectory']
                commandLine = $commandLine; arguments = $arguments
                launcherDefaultArguments = @($contract['launcherArguments'])
                directArguments = $arguments; workerOverride = $override
                stdoutProof = [ordered]@{
                    executableOrigin = $true; peer = $peerIndex; pid = $processId
                    frameLimit = 4096; activeMarker = $stdoutActive
                    passMarker = $stdoutPass; finalCrc = '00018720'
                }
                receiptWorkerTelemetry = $telemetry
                requestedWorkers = $override['requestedWorkers']; workerPolicy = $override['workerPolicy']
                effectiveWorkers = $effectiveWorkerCounts[$peerIndex]
                titleSessionProfile = $profile; registryEquivalence = $registryEquivalence
                environmentEquivalence = New-LockstepFixturePeerEnvironment $profile $peerIndex
                receiptPath = $receiptLeaf; receiptSha256 = Get-Sha256 $receiptPath
                stdoutSha256 = Get-Sha256 $stdoutPath; stderrSha256 = Get-Sha256 $stderrPath
                exitCode = 0; finalFrame = 4096; finalCRC = 100128
                comparableProjectionSha256 = $projection
                lockstepV2Receipt = $true; v1ReceiptAccepted = $false
            }
            $rawPath = Join-Path $titleRoot "peer-$peerIndex.raw.json"
            Write-JsonDocument $rawPath $peer
            $peers += ,$peer
        }
        $session = [ordered]@{
            title = $title; peerCount = 2; networkRosterMask = 3
            simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
            mapCrc = $mapCrc
            ports = $ports; sessionNonce = $sessionNonce
            launcherEquivalence = $contract; titleSessionProfile = $profile
            registryEquivalence = $registryEquivalence; workerProfiles = $workerProfiles
            effectiveWorkerCounts = $effectiveWorkerCounts; mixedWorkerProof = $true
            comparableProjectionSha256 = $projection; peers = $peers
            profileReadOnlyVerified = $true; profileFilesAfterRun = @()
        }
        $sessions += ,$session
    }
    $negativeProbes = [ordered]@{
        crossEpoch = @(
            (New-LockstepFixtureNegativeProbe $Root 'Generals' `
                $ArtifactHashes['generals-executable'] $SourceCommit `
                'negative-cross-epoch' 52000 $mapCrcs['Generals'] 23063 ('5' * 32) ('6' * 32)),
            (New-LockstepFixtureNegativeProbe $Root 'ZeroHour' `
                $ArtifactHashes['zerohour-executable'] $SourceCommit `
                'negative-cross-epoch' 52010 $mapCrcs['ZeroHour'] 23063 ('7' * 32) ('8' * 32))
        )
        contentMismatch = @(
            (New-LockstepFixtureNegativeProbe $Root 'Generals' `
                $ArtifactHashes['generals-executable'] $SourceCommit `
                'negative-content-mismatch' 52001 $mapCrcs['Generals'] 23063 ('9' * 32) ('A' * 32)),
            (New-LockstepFixtureNegativeProbe $Root 'ZeroHour' `
                $ArtifactHashes['zerohour-executable'] $SourceCommit `
                'negative-content-mismatch' 52011 $mapCrcs['ZeroHour'] 23063 ('B' * 32) ('C' * 32))
        )
    }
    $document = [ordered]@{
        schemaVersion = 2; evidenceKind = 'lockstep-v2-multiplayer'; status = 'passed'
        producer = 'installed-lockstep-v2'; validationMode = 'installed-lockstep-v2-production'
        architecture = 'x64'; sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256; recordedUtc = '2026-09-01T00:00:00Z'
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        qualificationData = $qualificationData
        allowHeadlessDirectExecution = $true; launcherEquivalence = $launcherContracts
        commonStopFrame = 4096; peerCount = 2; networkRosterMask = 3
        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
        mapName = $mapName
        mapCrcs = $mapCrcs; seed = 23063; v1Accepted = $false
        negativeProbes = $negativeProbes
        profileStrategy = 'process-local-validation-profile-root'
        registryViews = @('Registry32', 'Registry64')
        environmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA', 'USERPROFILE',
            'HOMEDRIVE', 'HOMEPATH', 'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
            'RTS_STAGE5_VALIDATION_CACHE_ROOT', 'RTS_STAGE5_VALIDATION_LOG_ROOT',
            'RTS_STAGE5_VALIDATION_DUMP_ROOT')
        profileConcurrency = 'shared-title-profile-read-only'
        titleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
        sessions = $sessions
    }
    $evidencePath = Join-Path $Root 'LockstepV2LoopbackEvidence.json'
    Write-JsonDocument $evidencePath $document
    return [pscustomobject]@{ path = $evidencePath; document = $document }
}

function Copy-LockstepFixtureCase {
    param([string]$SourceRoot, [string]$CaseRoot)
    Copy-Item -LiteralPath $SourceRoot -Destination $CaseRoot -Recurse -Force
    $sourceFull = [IO.Path]::GetFullPath($SourceRoot)
    $caseFull = [IO.Path]::GetFullPath($CaseRoot)
    $sourceJson = $sourceFull.Replace('\', '\\')
    $caseJson = $caseFull.Replace('\', '\\')
    $sourceHomePathJson = $sourceFull.Substring(2).Replace('\', '\\')
    $caseHomePathJson = $caseFull.Substring(2).Replace('\', '\\')
    foreach ($jsonPath in Get-ChildItem -LiteralPath $caseFull -Filter '*.json' -File -Recurse) {
        $json = [IO.File]::ReadAllText($jsonPath.FullName)
        if ($json.IndexOf($sourceJson) -ge 0 -or
            $json.IndexOf($sourceHomePathJson) -ge 0) {
            [IO.File]::WriteAllText($jsonPath.FullName,
                $json.Replace($sourceJson, $caseJson).Replace(
                    $sourceHomePathJson, $caseHomePathJson),
                (New-Object Text.UTF8Encoding($false)))
        }
    }
    return [IO.Path]::GetFullPath((Join-Path $CaseRoot 'LockstepV2LoopbackEvidence.json'))
}

function Read-LockstepFixtureCaseDocument {
    param([string]$EvidencePath)
    return Get-Content -LiteralPath $EvidencePath -Raw | ConvertFrom-Json
}

function Write-LockstepFixturePeerMutation {
    param(
        [string]$EvidencePath,
        [object]$Document,
        [int]$SessionIndex,
        [int]$PeerIndex,
        [object]$Raw
    )
    $title = if ($SessionIndex -eq 0) { 'Generals' } else { 'ZeroHour' }
    $rawPath = Join-Path (Split-Path -Parent $EvidencePath) "$title/peer-$PeerIndex.raw.json"
    Write-JsonDocument $rawPath $Raw
    Write-JsonDocument $EvidencePath $Document
}

function Set-LockstepFixtureReceiptCheckpointCrc {
    param([string]$ReceiptPath, [string]$Replacement)
    $text = [IO.File]::ReadAllText($ReceiptPath)
    $text = $text.Replace('checkpoint_0_crc=100000', "checkpoint_0_crc=$Replacement")
    [IO.File]::WriteAllText($ReceiptPath, $text, (New-Object Text.UTF8Encoding($false)))
    $pairs = [ordered]@{}
    foreach ($line in ($text -split "`n")) {
        $trimmed = $line.TrimEnd("`r")
        if ($trimmed -eq 'RTS_LOCKSTEP_V2_RECEIPT' -or $trimmed -eq 'END' -or
            [string]::IsNullOrEmpty($trimmed)) { continue }
        $equals = $trimmed.IndexOf('=')
        $pairs[$trimmed.Substring(0, $equals)] = $trimmed.Substring($equals + 1)
    }
    return Get-LockstepFixtureProjectionSha256 $pairs
}

function Write-Net3LoopbackTestManifest {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$GeneralsExecutableSha256, [string]$ZeroHourExecutableSha256)
    $topologies = @(
        [pscustomobject]@{ id = 'two-peer-1-v-16'; caseIndex = 0; workers = @('1', '16') },
        [pscustomobject]@{ id = 'two-peer-2-v-auto'; caseIndex = 1; workers = @('2', 'auto') },
        [pscustomobject]@{ id = 'two-peer-4-v-8'; caseIndex = 2; workers = @('4', '8') },
        [pscustomobject]@{ id = 'four-peer-mixed-workers'; caseIndex = 3; workers = @('1', '2', '8', 'auto') }
    )
    $kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
    $kernelBits = @(1, 2, 4, 8, 16, 32)
    $executableHashes = @{
        Generals = $GeneralsExecutableSha256
        ZeroHour = $ZeroHourExecutableSha256
    }
    $buildCrcs = @{ Generals = [UInt32]287454020; ZeroHour = [UInt32]1432778632 }
    $contentCrcs = @{ Generals = [UInt32]2864434397; ZeroHour = [UInt32]2578103244 }
    $receiptRoot = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $rawRoot = Join-Path $receiptRoot 'Net3Raw'
    if (-not (Test-Path -LiteralPath $rawRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $rawRoot | Out-Null
    }
    $processOrdinal = 1000
    $matches = @()
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($topology in $topologies) {
            foreach ($seed in @(23063, 49374)) {
                $rosterHash = if ($title -ceq 'Generals') { 'C' * 64 } else { 'D' * 64 }
                $peers = @()
                for ($peerIndex = 0; $peerIndex -lt $topology.workers.Count; ++$peerIndex) {
                    $requested = $topology.workers[$peerIndex]
                    $effective = if ($requested -ceq 'auto') { 8 } else { [int]$requested }
                    $kernels = @()
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $physical = if ($effective -eq 1) { 0 } else { 4 }
                        $kernels += [ordered]@{
                            name = $kernelNames[$kernelIndex]
                            bit = $kernelBits[$kernelIndex]
                            submitted = $physical
                            completed = $physical
                            physicalWorkerJobs = $physical
                            ownerHelpedJobs = 0
                            physicalWorkerMask = $(if ($effective -eq 1) { 0 } else { 3 })
                            distinctPhysicalWorkers = $(if ($effective -eq 1) { 0 } else { 2 })
							physicalWorkerMaskComplete = $true
                            peakConcurrentPhysicalWorkers = $(if ($effective -eq 1) { 0 } else { 2 })
                        }
                    }
                    $rawLeaf = "net3-$title-$($topology.id)-$seed-peer-$peerIndex.log"
                    $rawRelative = Join-Path 'Net3Raw' $rawLeaf
                    $rawPath = Join-Path $receiptRoot $rawRelative
                    $rawRecord = [ordered]@{
                        schemaVersion = 1
                        producer = 'installed-runtime-net3-peer-v1'
                        validationMode = 'scoped-net3-loopback-release-proof'
                        kernelFixture = 'actual-stage5-kernels-v1'
                        processId = $processOrdinal
                        title = $title
                        caseIndex = $topology.caseIndex
                        seed = $seed
                        ordinal = $peerIndex
                        peerCount = $topology.workers.Count
                        sourceCommit = $SourceCommit
                        executableSha256 = $executableHashes[$title]
                        artifactSetSha256 = $ArtifactSetSha256
                        buildCompatibilityCrc = $buildCrcs[$title]
                        contentCrc = $contentCrcs[$title]
                        requestedWorkers = $requested
                        effectiveWorkers = $effective
                        networkHelloReady = $true
                        rosterExact = $true
                        rosterSha256 = $rosterHash
                        policyMask = 63
                        finalFrame = 42000
                        finalCRC = 'A1B2C3D4'
                        cleanShutdown = $true
                        kernels = $kernels
                    }
                    Write-JsonDocument $rawPath $rawRecord
                    $peers += [ordered]@{
                        ordinal = $peerIndex
                        processId = $processOrdinal
                        observedExecutableSha256 = $executableHashes[$title]
                        observedArtifactSetSha256 = $ArtifactSetSha256
                        rawOutputPath = $rawRelative
                        rawOutputSha256 = Get-Sha256 $rawPath
                        requestedWorkers = $requested
                        effectiveWorkers = $effective
                        networkHelloReady = $true
                        rosterExact = $true
                        rosterSha256 = $rosterHash
                        policyMask = 63
                        finalFrame = 42000
                        finalCRC = 'A1B2C3D4'
                        exitCode = 0
                        cleanShutdown = $true
                        kernels = $kernels
                    }
                    ++$processOrdinal
                }
                $matches += [ordered]@{
                    recordId = "$title/$($topology.id)/$seed"
                    sourceCommit = $SourceCommit
                    title = $title
                    executableSha256 = $executableHashes[$title]
                    artifactSetSha256 = $ArtifactSetSha256
                    topologyId = $topology.id
                    seed = $seed
                    networkHelloReady = $true
                    rosterExact = $true
                    rosterSha256 = $rosterHash
                    policyMask = 63
                    peers = $peers
                }
            }
        }
    }
    Write-JsonDocument $Path ([ordered]@{
        schemaVersion = 1
        evidenceKind = 'installed-net3-loopback'
        status = 'passed'
        producer = 'installed-runtime-runner-v1'
        validationMode = 'scoped-net3-loopback-release-proof'
        installedRuntime = $true
        independentProcessHashing = $true
        sourceCommit = $SourceCommit
        artifactSetSha256 = $ArtifactSetSha256
        supportedKernelMask = 63
        policySchema = 1
        engineEpoch = 1
        determinismEpoch = 1
        buildCompatibilityCrc = [ordered]@{
            Generals = $buildCrcs.Generals
            ZeroHour = $buildCrcs.ZeroHour
        }
        contentCrc = [ordered]@{
            Generals = $contentCrcs.Generals
            ZeroHour = $contentCrcs.ZeroHour
        }
        executables = [ordered]@{
            Generals = $GeneralsExecutableSha256
            ZeroHour = $ZeroHourExecutableSha256
        }
        fixedSeeds = @(23063, 49374)
        matches = $matches
    })
}

function Write-PerformanceScalingTestManifest {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$ExecutableSha256, [string]$ArtifactSetManifestPath,
        [string]$Stage3BaselineOutputPath,
        [string]$PhaseBaselineProfileOutputPath,
        [ValidateSet('ZeroHour')][string]$Title = 'ZeroHour')
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $stem = [IO.Path]::GetFileNameWithoutExtension($Path)
    Assert-True ((Test-Path -LiteralPath $ArtifactSetManifestPath -PathType Leaf) -and
        (Get-Sha256 $ArtifactSetManifestPath) -ceq $ArtifactSetSha256) `
        'authoritative scaling fixture requires the exact reviewed artifact-set manifest'
    $artifactSet = Get-Content -LiteralPath $ArtifactSetManifestPath -Raw |
        ConvertFrom-Json
    $zeroHourExecutable = @($artifactSet.artifacts | Where-Object {
        $_.role -ceq 'zerohour-executable'
    })
    Assert-True ($zeroHourExecutable.Count -eq 1 -and
        $zeroHourExecutable[0].sha256 -ceq $ExecutableSha256) `
        'authoritative scaling fixture requires the reviewed Zero Hour executable'
    $closureLeaf = "$stem.authoritative"
    $closureRoot = Join-Path $directory $closureLeaf
    Assert-True (-not (Test-Path -LiteralPath $closureRoot)) `
        'authoritative scaling fixture closure root must be fresh'
    $exportOutput = @(& (Join-Path $PSScriptRoot `
            'Stage5PerformanceScalingValidation.Tests.ps1') `
        -ExportAuthoritativeFixtureRoot $closureRoot `
        -ExportArtifactSetManifestPath $ArtifactSetManifestPath `
        -ExportSourceCommit $SourceCommit `
        -ExportCohortNonce $script:TestCohortNonce `
        -ExportCohortCreatedUtc $script:TestCohortCreatedUtc)
    $exportLine = @($exportOutput | Where-Object {
        $_ -is [string] -and $_.TrimStart().StartsWith('{')
    } | Select-Object -Last 1)
    Assert-True ($exportLine.Count -eq 1) `
        'authoritative scaling fixture exporter did not return its exact binding'
    $export = $exportLine[0] | ConvertFrom-Json
    $internalFinalPath = Join-Path $closureRoot 'Stage5PerformanceScaling.json'
    Assert-True ([IO.Path]::GetFullPath([string]$export.finalPath) -ceq
        [IO.Path]::GetFullPath($internalFinalPath)) `
        'authoritative scaling fixture exporter returned another final envelope'
    $final = Read-TestJson $internalFinalPath
    $final.rawSampleManifest.path =
        "$closureLeaf/Stage5PerformanceScalingRawSamples.json"
    Write-JsonDocument $Path $final
    Copy-Item -LiteralPath (Join-Path $closureRoot 'Stage3PerformanceBaseline.json') `
        -Destination $Stage3BaselineOutputPath -Force
    Copy-Item -LiteralPath (Join-Path $closureRoot `
            'Stage5PerformancePhaseBaselineProfile.json') `
        -Destination $PhaseBaselineProfileOutputPath -Force
    return [pscustomobject]@{
        path = [IO.Path]::GetFullPath($Path)
        closureRoot = $closureRoot
        stage3BaselinePath = [IO.Path]::GetFullPath($Stage3BaselineOutputPath)
        stage3BaselineSha256 = Get-Sha256 $Stage3BaselineOutputPath
        phaseBaselineProfilePath =
            [IO.Path]::GetFullPath($PhaseBaselineProfileOutputPath)
        phaseBaselineProfileSha256 = Get-Sha256 $PhaseBaselineProfileOutputPath
        maximumOneWorkerRegressionRatio = [double](($final.fixtures |
            ForEach-Object { [double]$_.oneWorkerRegressionRatio } |
            Measure-Object -Maximum).Maximum)
        minimumEightWorkerSpeedup = [double](($final.fixtures |
            ForEach-Object { [double]$_.eightPhysicalCoreSpeedup } |
            Measure-Object -Minimum).Minimum)
        minimumEightToSixteenSpeedup = [double](($final.fixtures |
            ForEach-Object { [double]$_.eightToSixteenSpeedup } |
            Measure-Object -Minimum).Minimum)
    }
}

function Assert-PerformanceDiagnosticsConversion {
    param([string]$Path, [string]$SourceCommit, [string]$ArtifactSetSha256,
        [string]$ExecutableSha256)
    # This test catches a converter that relabels local evidence as acceptance,
    # includes warmups/oracle time in medians, or trusts changed receipt bytes.
    if ($null -eq (Get-Command ConvertTo-Stage5PerformanceDiagnostics -ErrorAction SilentlyContinue)) {
        Assert-True $false 'hash-bound local performance diagnostics converter is available'
        return
    }
    $directory = Split-Path -Parent $Path
    # A small, real byte-bound artifact/fixture bundle keeps this converter test
    # independent of the installed-runtime test corpus and large game assets.
    $dependencyFiles = @(); $artifactEntries = @(); $closureLines = @()
    foreach ($title in @('Generals', 'ZeroHour')) {
        foreach ($kind in @('executable', 'launcher', 'launcher-config', 'dll', 'asset')) {
            $leaf = "diagnostics-$title-$kind.bin"
            $filePath = Join-Path $directory $leaf
            Write-JsonDocument $filePath @{ title = $title; kind = $kind }
            $hash = Get-Sha256 $filePath
            $dependencyFiles += @{ title = $title; kind = $kind; path = $leaf; sha256 = $hash }
            $closureLines += "$title|$kind|$leaf|$hash"
            if ($kind -ceq 'executable') {
                $artifactEntries += @{ role = "$($title.ToLowerInvariant())-executable"; path = $leaf; sha256 = $hash }
                if ($title -ceq 'ZeroHour') { $ExecutableSha256 = $hash; $executablePath = $filePath }
            }
        }
    }
    [Array]::Sort($closureLines, [StringComparer]::Ordinal)
    $closureHash = Get-Sha256Text (($closureLines -join "`n") + "`n")
    $dependencyPath = Join-Path $directory 'diagnostics-dependencies.json'
    Write-JsonDocument $dependencyPath @{ schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'; files = $dependencyFiles }
    $dependencyHash = Get-Sha256 $dependencyPath
    $artifactPath = Join-Path $directory 'diagnostics-artifacts.json'
    Write-JsonDocument $artifactPath @{ schemaVersion = 1; sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'; artifacts = $artifactEntries
        runtimeClosure = @{ dependencyManifest = @{ path = 'diagnostics-dependencies.json'; sha256 = $dependencyHash }
            closureSha256 = $closureHash } }
    $ArtifactSetSha256 = Get-Sha256 $artifactPath
    $fixtureEntries = @()
    foreach ($fixture in @(@('one-thousand-units', 1000), @('four-thousand-units', 4000),
        @('eight-thousand-units', 8000), @('dense-eight-player', 8000))) {
        $fixturePath = Join-Path $directory "diagnostics-$($fixture[0]).rep"
        Write-JsonDocument $fixturePath @{ fixture = $fixture[0] }
        $fixtureEntries += @{ id = $fixture[0]; source = [IO.Path]::GetFileName($fixturePath)
            sha256 = Get-Sha256 $fixturePath; seed = 1729; playerCount = 8; peakUnitCount = $fixture[1] }
    }
    $fixtureManifestPath = Join-Path $directory 'diagnostics-fixtures.json'
    Write-JsonDocument $fixtureManifestPath @{ schemaVersion = 1
        evidenceKind = 'stage5-performance-scaling-fixtures'; title = 'ZeroHour'
        executableSha256 = $ExecutableSha256; fixtures = $fixtureEntries }
    $replayPath = Join-Path $directory $fixtureEntries[0].source
    $runs = @()
    $bindings = @()
    $times = @(999.0, 10.0, 30.0, 20.0)
    foreach ($index in 0..3) {
        $rawPath = Join-Path $directory "diagnostics-$index.log"
        $timingPath = Join-Path $directory "diagnostics-$index.csv"
        Write-JsonDocument $rawPath @{ run = $index }
        Write-JsonDocument $timingPath @{ frame = $index }
        $receiptPath = Join-Path $directory "diagnostics-$index.receipt.json"
        $phases = @()
        foreach ($name in @('owner-intake', 'legacy-mutable-island', 'spatial-work',
            'owner-tail', 'verification-publication')) {
            $phases += @{ name = $name; available = $true; totalNanoseconds = 10
                maximumNanoseconds = 10; sampleCount = 1; serialNanoseconds = 0
                serialNanosecondsKnown = $false }
        }
        $receipt = [ordered]@{
            schemaVersion = 5; producer = 'game-executable-stage5-performance-report-v5'
            producerVersion = '5'; evidenceKind = 'stage5-executable-originated-receipt'
            measurementRole = 'throughput'
            simulationMode = 'parallel'; schedulerStarted = $true
            status = 'passed'; title = 'ZeroHour'; sourceCommit = $SourceCommit
            artifactSetSha256 = $ArtifactSetSha256; executableSha256 = $ExecutableSha256
            executablePath = $executablePath; runId = "diagnostic-$index"
            architecture = 'x64'; cohortCreatedUtc = '2026-01-01T00:00:00Z'
            runtimeClosure = @{ dependencyManifestSha256 = $dependencyHash; closureSha256 = $closureHash }
            runNonce = "11111111-1111-4111-8111-11111111111$index"
            cohortNonce = '22222222-2222-4222-8222-222222222222'; commandLine = "test-run-$index"
            process = @{ id = 20000 + $index; creationTimeUtc100ns = 100 + $index
                identityAvailable = $true; exitCodeKnown = $true; exitCode = 0 }
            fixture = @{ id = 'one-thousand-units'; requestedPlayerCount = 8
                requestedMinimumUnitCount = 1000; contentSha256 = $fixtureEntries[0].sha256
                replayPath = $replayPath; seed = 1729; seedKnown = $true
                kind = 'replay'; workloadQualification = 'minimum-qualified'; contentPath = $replayPath
                identityObserved = $true; retainedReplayPath = ''; retainedReplaySha256 = '' }
            worker = @{ requestedCount = 1; effectiveCount = 1; policy = 'auto'; pinned = $true
                availableLogicalCpuCount = 2; reservedOwnerCpuCount = 1; selectedWorkerCpuCount = 1
                selectedWorkerPhysicalCoreCount = 1; selectedWorkerPhysicalCoreMask = 2
                selectedWorkerPhysicalCoreMaskComplete = $true }
            topology = @{ source = 'GetSystemCpuSetInformation'; ownerCpuSetIds = @(0); selectedWorkerCpuSetIds = @(1)
                cpuSets = @(@{ id = 0; efficiencyClass = 0; group = 0; coreIndex = 0; logicalProcessorIndex = 0
                    parked = $false; allocatedToOtherProcess = $false; availableToProcess = $true },
                    @{ id = 1; efficiencyClass = 0; group = 0; coreIndex = 1; logicalProcessorIndex = 1
                    parked = $false; allocatedToOtherProcess = $false; availableToProcess = $true }) }
            schedulerMetrics = @{ affinityFailureCount = 0 }
            frames = @{ start = 0; end = 1; final = 1; finalCrcKnown = $true; finalCrc = 123 }
            workload = @{ sampling = 'completed-simulation-frame-boundary-v1'; sampleCount = 1
                firstFrame = 1; lastFrame = 1; playerCount = 8; initialUnitCount = 1000
                minimumUnitCount = 1000; peakUnitCount = 1000 + $index
                rosterStable = $true; contiguous = $true }
            frameSimulation = @{ totalNanoseconds = 100; maximumNanoseconds = 100; sampleCount = 1 }
            phases = $phases
            kernelTiming = @{ schemaVersion = 1; mode = 'owner-pipeline-observation'
                attribution = 'owner-stack-exclusive-v1'; enabled = $true; frozen = $true
                complete = $true; errors = 0; generation = 1; serialReferenceKnown = $false
                streams = @(@{ name = 'physics'; subtype = 0; attemptedBatches = 1
                    admittedBatches = 1; committedBatches = 1; abortedBatches = 0
                    firstFrame = 1; lastFrame = 1; activePipelineNanoseconds = 5
                    inclusiveBatchNanoseconds = 10; maximumBatchNanoseconds = 10
                    stages = @(@{name='capture';totalNanoseconds=1;sampleCount=1},
                        @{name='schedule';totalNanoseconds=1;sampleCount=1},
                        @{name='wait';totalNanoseconds=1;sampleCount=1},
                        @{name='validate';totalNanoseconds=1;sampleCount=1},
                        @{name='commit';totalNanoseconds=1;sampleCount=1}) }) }
            kernelReference = @{ schemaVersion = 1; mode = 'throughput-binding'
                frozen = $true; complete = $true; errors = 0; generation = 1
                streams = @(@{ name = 'physics'; subtype = 0; fieldSchema = 1
                    firstFrame = 1; lastFrame = 1; validatedBatchCount = 1
                    committedBatchCount = 1; abortedBatchCount = 0
                    validatedOperationCount = 2; committedOperationCount = 2
                    serialSampleCount = 0; serialNanoseconds = 0; maximumSerialNanoseconds = 0
                    inputSha256 = ('A' * 64); outputSha256 = ('B' * 64); commitSha256 = ('C' * 64) }) }
            rawEvidence = @{ rawLogPath = $rawPath; rawLogSha256 = Get-Sha256 $rawPath
                timingPath = $timingPath; timingSha256 = Get-Sha256 $timingPath
                timingClosed = $true; timingWriteSucceeded = $true; timingTruncated = $false
                timingComplete = $true; timingSessionCount = 1; timingFrameSamples = 2
                timingFirstFrame = 0; timingLastFrame = 1 }
        }
        Write-JsonDocument $receiptPath $receipt
        $binding = [ordered]@{
            path = $receiptPath; sha256 = Get-Sha256 $receiptPath
            runId = $receipt.runId; runNonce = $receipt.runNonce; cohortNonce = $receipt.cohortNonce
            processId = $receipt.process.id; processCreationTimeUtc100ns = $receipt.process.creationTimeUtc100ns
            executablePath = $receipt.executablePath; executableSha256 = $ExecutableSha256
            commandLine = $receipt.commandLine; rawLogPath = $rawPath; rawLogSha256 = Get-Sha256 $rawPath
            timingPath = $timingPath; timingSha256 = Get-Sha256 $timingPath
        }
        $bindings += $binding
        $runs += [ordered]@{
            fixtureId = 'one-thousand-units'; lane = 'forced-one'; ordinal = $index
            warmup = $index -eq 0; elapsedMilliseconds = $times[$index]
            runId = $receipt.runId; processId = $receipt.process.id
            processCreationTimeUtc100ns = $receipt.process.creationTimeUtc100ns
            receiptPath = $receiptPath; receiptSha256 = $binding.sha256; receiptBinding = $binding
            rawLogPath = $rawPath; rawLogSha256 = $binding.rawLogSha256
            timingPath = $timingPath; timingSha256 = $binding.timingSha256
            selectedWorkerCpuSetIds = @(1); selectedPhysicalCoreMask = '0000000000000002'
        }
    }
    $hostAggregate = [ordered]@{
        schemaVersion = 2; evidenceKind = 'stage5-performance-scaling-local-capacity-smoke'
        producer = 'Invoke-Stage5PerformanceScalingValidation.ps1'; status = 'passed'
        qualificationMode = 'LocalCapacitySmoke'; measurementMode = 'headless-throughput'; installedRuntime = $true
        sourceCommit = $SourceCommit; artifactSetSha256 = $ArtifactSetSha256; title = 'ZeroHour'
        executable = @{ path = $executablePath; sha256 = $ExecutableSha256 }
        artifactSetManifest = @{ path = $artifactPath; sha256 = $ArtifactSetSha256 }
        runtimeClosure = @{ dependencyManifestPath = 'diagnostics-dependencies.json'
            dependencyManifestSha256 = $dependencyHash; closureSha256 = $closureHash; fileCount = 10 }
        fixtureManifest = @{ path = $fixtureManifestPath; sha256 = Get-Sha256 $fixtureManifestPath }
        referencePolicy = 'throughput-only'; pairedOracleBindings = @()
        nativeReceiptBindings = $bindings; runs = $runs
    }
    Write-JsonDocument $Path $hostAggregate
    $proof = ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour'
    Assert-True (@($proof).Count -eq 1 -and -not $proof.finalAcceptanceClaim -and $proof.status -ceq 'diagnostic' -and
        $proof.schemaVersion -eq 3 -and $proof.runs[1].measurementRole -ceq 'throughput' -and
        $proof.runs[1].kernelReference.streams[0].committedOperationCount -eq 2 -and
        $proof.lanes[0].medianElapsedMilliseconds -eq 20.0 -and $proof.lanes[0].measuredRuns -eq 3) `
        'local converter retains canonical batch/operation identity and excludes warmups from throughput medians'
    Assert-True ($proof.blockedBy -contains 'phase-serial-coverage-unknown' -and
        $proof.blockedBy -contains 'same-input-serial-reference-unavailable') `
        'local converter preserves both independent serial evidence prerequisites'
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' -RequireAcceptance } 'serial|acceptance' `
        'local converter cannot manufacture an external acceptance result'
    $originalReceipt = Get-Content -LiteralPath $runs[1].receiptPath -Raw
    $publishMutation = {
        param([object]$Value)
        Write-JsonDocument $runs[1].receiptPath $Value
        $bindings[1].sha256 = Get-Sha256 $runs[1].receiptPath
        $runs[1].receiptSha256 = $bindings[1].sha256
        Write-JsonDocument $Path $hostAggregate
    }
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.schemaVersion = 4; $changed.producer = 'game-executable-stage5-performance-report-v4'
    $changed.producerVersion = '4'
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'provenance|V5|schema version|unsupported' `
        'obsolete unbound receipt protocol is rejected even when its bytes are hash-bound'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.schedulerMetrics.affinityFailureCount = 1
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'affinity' `
        'a pinned physical lane with an affinity failure cannot enter local diagnostics'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.measurementRole = 'serial-oracle'; $changed.kernelReference.mode = 'serial-oracle'
    $changed.kernelReference.streams[0].serialSampleCount = 1
    $changed.kernelReference.streams[0].serialNanoseconds = 7
    $changed.kernelReference.streams[0].maximumSerialNanoseconds = 7
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'throughput|role|oracle' `
        'serial-oracle process elapsed cannot enter throughput medians'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.kernelReference.streams[0].serialNanoseconds = 7
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'serial|reference' `
        'a throughput label cannot hide executed serial cost'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.kernelReference.streams[0].committedBatchCount = 0
    $changed.kernelReference.streams[0].abortedBatchCount = 1
    $changed.kernelReference.streams[0].committedOperationCount = 0
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'reference|commit|timing' `
        'reference commit disposition must match timing batches, not operation count'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.kernelReference.streams[0].validatedOperationCount = 3
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'reference|operation' `
        'no aborted batch means no validated operation may disappear'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.kernelTiming.streams[0].committedBatches = 0
    $changed.kernelTiming.streams[0].abortedBatches = 1
    $changed.kernelReference.streams[0].committedBatchCount = 0
    $changed.kernelReference.streams[0].abortedBatchCount = 1
    $changed.kernelReference.streams[0].committedOperationCount = 1
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'reference|operation' `
        'zero committed batches cannot own committed operations'
    $changed = $originalReceipt | ConvertFrom-Json
    $changed.kernelReference.streams[0].inputSha256 = ''
    & $publishMutation $changed
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'digest|hash|reference' `
        'missing canonical input binding cannot be reported as valid diagnostics'
    & $publishMutation ($originalReceipt | ConvertFrom-Json)
    Write-JsonDocument $runs[1].receiptPath @{ changed = $true }
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'SHA-256|hash' `
        'local converter rechecks receipt hashes instead of trusting aggregate status'
    & $publishMutation ($originalReceipt | ConvertFrom-Json)
    $hostAggregate.schemaVersion = 1
    Write-JsonDocument $Path $hostAggregate
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'aggregate|version|protocol' `
        'an obsolete host aggregate cannot be silently reinterpreted as the paired protocol'
    $hostAggregate.schemaVersion = 2
    $hostAggregate.referencePolicy = 'paired-serial-oracle-v1'
    $serialTimes = @(7777, 10, 30, 20)
    $pairs = @()
    foreach ($index in 0..3) {
        $oracle = Get-Content -LiteralPath $runs[$index].receiptPath -Raw | ConvertFrom-Json
        $oracle.runId = "oracle-$index"
        $oracle.runNonce = "33333333-3333-4333-8333-33333333333$index"
        $oracle.process.id = 30000 + $index; $oracle.process.creationTimeUtc100ns = 200 + $index
        $oracle.measurementRole = 'serial-oracle'; $oracle.kernelReference.mode = 'serial-oracle'
        $oracle.kernelReference.streams[0].serialSampleCount = 1
        $oracle.kernelReference.streams[0].serialNanoseconds = $serialTimes[$index]
        $oracle.kernelReference.streams[0].maximumSerialNanoseconds = $serialTimes[$index]
        # Oracle pipeline/elapsed time is intentionally enormous and must not
        # contaminate the throughput denominator or its 20ms median.
        $oracle.kernelTiming.streams[0].activePipelineNanoseconds = 500000
        $oracle.kernelTiming.streams[0].inclusiveBatchNanoseconds = 500000
        $oracle.kernelTiming.streams[0].maximumBatchNanoseconds = 500000
        foreach ($stage in $oracle.kernelTiming.streams[0].stages) { $stage.totalNanoseconds = 100000 }
        $oracleRaw = Join-Path $directory "oracle-$index.log"
        $oracleTiming = Join-Path $directory "oracle-$index.csv"
        Write-JsonDocument $oracleRaw @{ oracle = $index }
        Write-JsonDocument $oracleTiming @{ oracleFrame = $index }
        $oracle.rawEvidence.rawLogPath = $oracleRaw; $oracle.rawEvidence.rawLogSha256 = Get-Sha256 $oracleRaw
        $oracle.rawEvidence.timingPath = $oracleTiming; $oracle.rawEvidence.timingSha256 = Get-Sha256 $oracleTiming
        $oraclePath = Join-Path $directory "oracle-$index.receipt.json"
        Write-JsonDocument $oraclePath $oracle
        $oracleRun = $runs[$index] | ConvertTo-Json -Depth 30 | ConvertFrom-Json
        $oracleRun.runId = $oracle.runId; $oracleRun.processId = $oracle.process.id
        $oracleRun.processCreationTimeUtc100ns = $oracle.process.creationTimeUtc100ns
        $oracleRun.elapsedMilliseconds = 999999
        $oracleRun.receiptPath = $oraclePath; $oracleRun.receiptSha256 = Get-Sha256 $oraclePath
        $oracleRun.rawLogPath = $oracleRaw; $oracleRun.rawLogSha256 = $oracle.rawEvidence.rawLogSha256
        $oracleRun.timingPath = $oracleTiming; $oracleRun.timingSha256 = $oracle.rawEvidence.timingSha256
        $oracleRun.receiptBinding.path = $oraclePath; $oracleRun.receiptBinding.sha256 = $oracleRun.receiptSha256
        $oracleRun.receiptBinding.runId = $oracle.runId; $oracleRun.receiptBinding.runNonce = $oracle.runNonce
        $oracleRun.receiptBinding.processId = $oracle.process.id
        $oracleRun.receiptBinding.processCreationTimeUtc100ns = $oracle.process.creationTimeUtc100ns
        $oracleRun.receiptBinding.rawLogPath = $oracleRaw; $oracleRun.receiptBinding.rawLogSha256 = $oracle.rawEvidence.rawLogSha256
        $oracleRun.receiptBinding.timingPath = $oracleTiming; $oracleRun.receiptBinding.timingSha256 = $oracle.rawEvidence.timingSha256
        $pairs += @{ throughputRunId = $runs[$index].runId; oracleRun = $oracleRun }
    }
    $hostAggregate.pairedOracleBindings = $pairs
    Write-JsonDocument $Path $hostAggregate
    $pairedProof = ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour'
    Assert-True (@($pairedProof).Count -eq 1 -and $pairedProof.runs.Count -eq 4 -and $pairedProof.pairedOracleBindings.Count -eq 4 -and
        $pairedProof.lanes[0].medianElapsedMilliseconds -eq 20.0 -and
        $pairedProof.kernelComparisons[0].measuredPairs -eq 3 -and
        $pairedProof.kernelComparisons[0].medianPipelineNanoseconds -eq 5 -and
        $pairedProof.kernelComparisons[0].medianSerialNanoseconds -eq 20 -and
        $pairedProof.kernelComparisons[0].netSpeedup -eq 4) `
        'matched independent oracle costs exclude warmups and all oracle pipeline/process elapsed'
    Assert-True (-not $pairedProof.finalAcceptanceClaim -and
        $pairedProof.blockedBy -contains 'phase-serial-coverage-unknown' -and
        $pairedProof.blockedBy -contains 'kernel-reference-coverage-incomplete' -and
        $pairedProof.blockedBy -notcontains 'same-input-serial-reference-unavailable') `
        'a matched partial kernel proof does not manufacture whole-frame or six-kernel acceptance'
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' -RequireAcceptance } 'serial|acceptance' `
        'paired serial kernels still cannot satisfy missing phase-serial coverage'
    $originalOracle = Get-Content -LiteralPath $pairs[1].oracleRun.receiptPath -Raw
    $publishOracle = {
        param([object]$Value)
        $run = $pairs[1].oracleRun
        Write-JsonDocument $run.receiptPath $Value
        $run.receiptSha256 = Get-Sha256 $run.receiptPath; $run.receiptBinding.sha256 = $run.receiptSha256
        Write-JsonDocument $Path $hostAggregate
    }
    foreach ($field in @('inputSha256', 'outputSha256', 'commitSha256')) {
        $changed = $originalOracle | ConvertFrom-Json
        $changed.kernelReference.streams[0].$field = 'D' * 64
        & $publishOracle $changed
        Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
            $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'pair|reference|match' `
            "paired oracle rejects independently rehashed unequal $field"
    }
    foreach ($mutation in @('seed', 'worker', 'topology', 'command', 'count')) {
        $changed = $originalOracle | ConvertFrom-Json
        switch ($mutation) {
            'seed' { $changed.fixture.seed = 1730 }
            'worker' { $changed.worker.policy = 'all' }
            'topology' { $changed.topology.cpuSets[0].efficiencyClass = 1 }
            'command' { $changed.commandLine += ' -different'; $pairs[1].oracleRun.receiptBinding.commandLine = $changed.commandLine }
            'count' { $changed.kernelReference.streams[0].validatedOperationCount = 3; $changed.kernelReference.streams[0].committedOperationCount = 3 }
        }
        & $publishOracle $changed
        Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
            $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'pair|fixture|reference|match' `
            "paired oracle rejects independently rehashed unequal $mutation"
        $pairs[1].oracleRun.receiptBinding.commandLine = ($originalOracle | ConvertFrom-Json).commandLine
    }
    & $publishOracle ($originalOracle | ConvertFrom-Json)
    $hostAggregate.pairedOracleBindings = @($pairs[0], $pairs[0], $pairs[2], $pairs[3])
    Write-JsonDocument $Path $hostAggregate
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'pair|duplicate|repeat' `
        'one oracle pair cannot be reused for another throughput run'
    $hostAggregate.pairedOracleBindings = @($pairs[0], $pairs[1], $pairs[2])
    Write-JsonDocument $Path $hostAggregate
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'pair|count|missing' `
        'every scheduled throughput run including warmup requires its own oracle'
    $hostAggregate.pairedOracleBindings = $pairs
    Write-JsonDocument $Path $hostAggregate
    Write-JsonDocument $replayPath @{ changedReplay = $true }
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'fixture|replay|SHA-256|hash' `
        'matching receipt strings cannot hide changed actual reviewed replay bytes'
    Write-JsonDocument $replayPath @{ fixture = 'one-thousand-units' }
    Write-JsonDocument $dependencyPath @{ changedDependencyManifest = $true }
    Assert-Throws { ConvertTo-Stage5PerformanceDiagnostics $Path (Get-Sha256 $Path) `
        $SourceCommit $ArtifactSetSha256 $ExecutableSha256 'ZeroHour' } 'dependency|closure|SHA-256|hash' `
        'matching receipt closure strings cannot hide changed dependency manifest bytes'
}

function Write-TestManifest {
    param([string]$Path, [string]$ExecutableHash, [string]$ReplayOneHash,
        [string]$ReplayTwoHash, [string]$FirstSource = 'fixtures\reference.rep',
        [int[]]$Seeds = @(1729, 1730, 1731),
        [string[]]$Scenarios = @('4v3', '4v2'), [int]$AiRepeats = 2)
    $manifest = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = $ExecutableHash
        fixtures = @(
            [ordered]@{
                id = 'reference'
                source = $FirstSource
                sha256 = $ReplayOneHash
                stress = $false
                maps = @()
            },
            [ordered]@{
                id = 'hard-ai-2v6'
                source = 'fixtures\hard-ai-2v6.rep'
                sha256 = $ReplayTwoHash
                stress = $true
                maps = @()
            }
        )
        ai = [ordered]@{
            seeds = $Seeds
            scenarios = $Scenarios
            repeats = $AiRepeats
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

function Write-StandardTestManifest {
    param([string]$Path, [string]$ExecutableHash, [string]$FixtureDirectory)
    $fixtureEntries = @()
    for ($index = 1; $index -le 10; ++$index) {
        $isStress = $index -eq 10
        $id = if ($isStress) { 'hard-ai-2v6' } else { 'reference-{0:D2}' -f $index }
        $leaf = "standard-$id.rep"
        $fixturePath = Join-Path $FixtureDirectory $leaf
        [IO.File]::WriteAllText($fixturePath, "standard replay fixture $index")
        $fixtureEntries += [ordered]@{
            id = $id
            source = "fixtures\$leaf"
            sha256 = Get-Sha256 $fixturePath
            stress = $isStress
            maps = @()
        }
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = $ExecutableHash
        fixtures = $fixtureEntries
        ai = [ordered]@{
            seeds = @(1729, 1730, 1731)
            scenarios = @('4v3', '4v2')
            repeats = 2
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

function Write-AiOnlyTestManifest {
    param([string]$Path, [string]$ExecutableHash,
        [int[]]$Seeds = @(1729, 1730, 1731),
        [string[]]$Scenarios = @('4v3', '4v2'), [int]$AiRepeats = 2)
    $manifest = [ordered]@{
        schemaVersion = 1
        title = 'ZeroHour'
        executable = 'generalszh.exe'
        executableSha256 = $ExecutableHash
        fixtures = @()
        ai = [ordered]@{
            seeds = $Seeds
            scenarios = $Scenarios
            repeats = $AiRepeats
        }
    }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 8))
}

function Write-MinimalRpl3TestFile {
    param([string]$Path)
    $payloadStream = New-Object IO.MemoryStream
    $payloadWriter = New-Object IO.BinaryWriter($payloadStream)
    try {
        $payloadWriter.Write([Text.Encoding]::ASCII.GetBytes('GENREP'))
        $payloadWriter.Write([UInt32]1)
        $payloadWriter.Write([UInt32]2)
        $payloadWriter.Write([UInt32]42000)
        $payloadWriter.Write([byte]0)
        $payloadWriter.Write([byte]0)
        for ($index = 0; $index -lt 8; ++$index) {
            $payloadWriter.Write([byte]0)
        }
        $payloadWriter.Write([Text.Encoding]::Unicode.GetBytes('Stage5 AI test'))
        $payloadWriter.Write([UInt16]0)
        for ($index = 0; $index -lt 8; ++$index) {
            $payloadWriter.Write([UInt16]0)
        }
        $payloadWriter.Write([Text.Encoding]::Unicode.GetBytes('GeneralsGameCode'))
        $payloadWriter.Write([UInt16]0)
        $payloadWriter.Write([Text.Encoding]::Unicode.GetBytes(
            'native [SkirmishAIEpoch=3]'))
        $payloadWriter.Write([UInt16]0)
        $payloadBytes = $payloadStream.ToArray()
    }
    finally {
        $payloadWriter.Dispose()
        $payloadStream.Dispose()
    }
    $fileStream = New-Object IO.MemoryStream
    $fileWriter = New-Object IO.BinaryWriter($fileStream)
    try {
        $fileWriter.Write([Text.Encoding]::ASCII.GetBytes('RPL3'))
        $fileWriter.Write([UInt32]2)
        $fileWriter.Write([UInt32]1)
        $fileWriter.Write([UInt64]0)
        $fileWriter.Write([UInt64]0)
        $fileWriter.Write([UInt64]$payloadBytes.Length)
        $fileWriter.Write([UInt32]0)
        $fileWriter.Write($payloadBytes)
        [IO.File]::WriteAllBytes($Path, $fileStream.ToArray())
    }
    finally {
        $fileWriter.Dispose()
        $fileStream.Dispose()
    }
}

function New-AiCompletionOutput {
    param([int]$Seed = 1729, [string]$Mode = 'parallel', [string]$RequestedWorkers = '2',
        [int]$EffectiveWorkers = 2, [string]$Digest = 'A1B2C3D4', [int]$EndFrame = 42000,
        [int]$Winner = 1, [int]$Submitted = 20, [int]$Executed = 20,
        [int]$Fallback = 0, [string]$ExecutableHash = ('A' * 64),
        [string]$Scenario = '4v3', [int]$ActualAi = 7, [string]$ActualTeams = '4v3',
        [int]$LoadedSeed = 0, [string]$RequestedPipeline = 'serial',
        [string]$EffectivePipeline = 'serial', [string]$RequestedSimulation = '',
        [string]$EffectiveSimulation = '', [int]$AuthoritativeCommits = 5,
        [int]$AiCommittedBatches = -1, [int]$AiParallelAuthoritativeCommits = -1,
        [int]$ShadowExecutions = 0, [int]$OwnerFallbacks = -1,
        [int]$AiRequestedBatches = -1,
        [int]$AiSubmitted = -1, [int]$AiCompleted = -1,
        [int]$CollisionAuthoritativeCommits = -1,
        [int]$CollisionShadowExecutions = 0, [int]$CollisionShadowMismatches = 0,
        [int]$CollisionShadowComparedCandidates = -1,
		[int]$CollisionOwnerFallbacks = 0, [int]$CollisionUnexpectedFallbacks = 0,
		[int]$CollisionStaleRejections = 0,
        [int]$CollisionCommittedCandidates = -1, [int]$CollisionPreparedPairs = -1,
        [int]$CollisionUniqueCandidates = -1, [int]$CollisionSubmitted = -1,
        [int]$CollisionCompleted = -1, [int]$CollisionIneligibleSlices = -1,
		[int]$CollisionPhysicalWorkerJobs = -1,
		[int]$CollisionOwnerHelpedJobs = -1,
		[Int64]$CollisionPhysicalWorkerMask = -1,
		[int]$CollisionDistinctPhysicalWorkers = -1,
		[int]$CollisionPhysicalWorkerMaskComplete = 1,
		[int]$PathWorkerExecuted = -1,
		[int]$PathAuthoritativeCommits = -1, [int]$PathOwnerHelped = 0,
		[int]$PathAuthoritativeMultiWorkerCommits = -1,
		[int]$PathUnsupportedAuthority = 0, [int]$PathShadowAuthority = 0,
		[int]$PathStaleAcceptance = 0, [int]$PathMalformedAcceptance = 0,
		[int]$PathShadowOnly = 0, [int]$PathTimeouts = 0, [int]$PathLateDrains = 0,
		[int]$PathValidationFailures = 0, [int]$PathPeakWorkers = -1,
		[int]$OrdinaryPathEligible = -1,
		[int]$OrdinaryPathSubmittedRequests = -1,
		[int]$OrdinaryPathSubmittedRanges = -1,
		[int]$OrdinaryPathWorkerExecutedRequests = -1,
		[int]$OrdinaryPathWorkerExecutedRangeJobs = -1,
		[int]$OrdinaryPathOwnerHelpedRangeJobs = 0,
		[int]$OrdinaryPathFailedRangeJobs = 0,
		[Int64]$OrdinaryPathPhysicalWorkerMask = -1,
		[int]$OrdinaryPathDistinctPhysicalWorkers = -1,
		[int]$OrdinaryPathPhysicalWorkerMaskComplete = 1,
		[int]$OrdinaryPathAuthoritativeCommits = -1,
		[int]$OrdinaryPathAuthoritativeMultiWorkerCommits = -1,
		[int]$OrdinaryPathStaleRejections = 0,
		[int]$OrdinaryPathValidationFailures = 0,
		[int]$OrdinaryPathSerialFallbacks = 0,
		[int]$OrdinaryPathShadowComparisons = -1,
		[int]$OrdinaryPathShadowMismatches = 0,
		[int]$OrdinaryPathTimeouts = 0, [int]$OrdinaryPathLateDrains = 0,
		[int]$OrdinaryPathPeakWorkers = -1,
		[int]$OrdinaryPathMaximumBatchRequests = -1,
		[int]$OrdinaryPathMaximumRangeCount = -1,
		[int]$OrdinaryPathMaximumGrainSize = -1,
		[int]$PhysicsAuthoritativeBatches = -1,
		[int]$PhysicsCommittedPrefixes = -1, [int]$PhysicsRanges = -1,
		[int]$PhysicsSubmitted = -1, [int]$PhysicsCompleted = -1,
		[int]$PhysicsPhysicalWorkerJobs = -1, [int]$PhysicsOwnerHelpedJobs = -1,
		[Int64]$PhysicsPhysicalWorkerMask = -1,
		[int]$PhysicsDistinctPhysicalWorkers = -1,
		[int]$PhysicsPhysicalWorkerMaskComplete = 1,
		[int]$PhysicsPeakConcurrentPhysicalWorkers = -1,
		[int]$PhysicsShadowExecutions = 0, [int]$PhysicsShadowMismatches = 0,
		[int]$PhysicsShadowPrefixes = -1, [int]$PhysicsShadowRanges = -1,
		[int]$PhysicsShadowSubmitted = -1, [int]$PhysicsShadowCompleted = -1,
		[int]$PhysicsOwnerFallbacks = 0, [int]$PhysicsUnexpectedFallbacks = 0,
		[int]$PhysicsStaleRejections = 0, [int]$PhysicsCircuitBreakerTrips = 0,
		[int]$StatusAuthoritativeBatches = -1, [int]$StatusCommittedCommands = -1,
		[int]$StatusSubmitted = -1, [int]$StatusCompleted = -1,
		[int]$StatusPhysicalWorkerJobs = -1, [int]$StatusOwnerHelpedJobs = -1,
		[Int64]$StatusPhysicalWorkerMask = -1,
		[int]$StatusDistinctPhysicalWorkers = -1,
		[int]$StatusPhysicalWorkerMaskComplete = 1,
		[int]$StatusPeakConcurrentPhysicalWorkers = -1,
		[int]$StatusShadowExecutions = -1, [int]$StatusShadowCommands = -1,
		[int]$StatusShadowMismatches = 0, [int]$StatusOwnerFallbacks = 0,
		[int]$StatusStaleRejections = 0,
		[int]$SpatialCapturedArenas = -1, [int]$SpatialCaptureFailures = 0,
		[int]$SpatialSuccessfulCollections = -1,
		[int]$SpatialSuccessfulCollectionQueries = -1,
		[int]$SpatialSuccessfulCollectionRanges = -1,
		[int]$SpatialMultiRangeCollections = -1,
		[int]$SpatialCollectionSubmitted = -1,
		[int]$SpatialCollectionCompleted = -1,
		[int]$SpatialCollectionPhysical = -1,
		[int]$SpatialCollectionOwnerHelped = 0,
		[Int64]$SpatialCollectionPhysicalWorkerMask = -1,
		[int]$SpatialMaximumCollectionQueries = -1,
		[int]$SpatialMaximumCollectionRanges = -1,
		[int]$SpatialMaximumCollectionDistinctPhysicalWorkers = -1,
		[int]$SpatialHealingEligible = 5,
		[int]$SpatialHealingAuthoritative = -1,
		[int]$SpatialHealingCandidates = -1, [int]$SpatialHealingShadow = -1,
		[int]$SpatialHealingShadowMismatches = 0,
		[int]$SpatialHealingSubmitted = -1, [int]$SpatialHealingCompleted = -1,
		[int]$SpatialHealingPhysical = -1, [int]$SpatialHealingOwnerHelped = 0,
		[int]$SpatialHealingExpectedFallbacks = -1,
		[int]$SpatialHealingUnexpectedFallbacks = 0,
		[int]$SpatialPdlEligible = 5,
		[int]$SpatialPdlAuthoritative = -1, [int]$SpatialPdlCandidates = -1,
		[int]$SpatialPdlShadow = -1, [int]$SpatialPdlShadowMismatches = 0,
		[int]$SpatialPdlSubmitted = -1, [int]$SpatialPdlCompleted = -1,
		[int]$SpatialPdlPhysical = -1, [int]$SpatialPdlOwnerHelped = 0,
		[int]$SpatialPdlExpectedFallbacks = -1,
		[int]$SpatialPdlUnexpectedFallbacks = 0,
		[switch]$OmitWorkEvidence)
    if ($LoadedSeed -eq 0) { $LoadedSeed = $Seed }
    if ([string]::IsNullOrEmpty($RequestedSimulation)) { $RequestedSimulation = $Mode }
    if ([string]::IsNullOrEmpty($EffectiveSimulation)) { $EffectiveSimulation = $Mode }
    if ($OwnerFallbacks -lt 0) { $OwnerFallbacks = $Fallback }
    if ($AiRequestedBatches -lt 0) { $AiRequestedBatches = [Math]::Max(5, $OwnerFallbacks) }
    if ($AiSubmitted -lt 0) { $AiSubmitted = $Submitted }
    if ($AiCompleted -lt 0) { $AiCompleted = $Executed }
    if ($AiCommittedBatches -lt 0) { $AiCommittedBatches = $AuthoritativeCommits }
    if ($AiParallelAuthoritativeCommits -lt 0) {
        $AiParallelAuthoritativeCommits = $AuthoritativeCommits
    }
    if ($CollisionAuthoritativeCommits -lt 0) {
        $CollisionAuthoritativeCommits = if ($Mode -ceq 'parallel' -and
            $RequestedWorkers -cne '1') { 3 } else { 0 }
    }
    if ($CollisionCommittedCandidates -lt 0) {
        $CollisionCommittedCandidates = if ($CollisionAuthoritativeCommits -gt 0) { 12 } else { 0 }
    }
    $collisionPreparedEligible = ($Mode -ceq 'parallel' -or $Mode -ceq 'shadow') -and
        $RequestedWorkers -cne '1'
    if ($CollisionPreparedPairs -lt 0) { $CollisionPreparedPairs = if ($collisionPreparedEligible) { 24 } else { 0 } }
    if ($CollisionUniqueCandidates -lt 0) { $CollisionUniqueCandidates = if ($collisionPreparedEligible) { 12 } else { 0 } }
    if ($CollisionSubmitted -lt 0) { $CollisionSubmitted = if ($collisionPreparedEligible) { 4 } else { 0 } }
    if ($CollisionCompleted -lt 0) { $CollisionCompleted = $CollisionSubmitted }
	if ($CollisionPhysicalWorkerJobs -lt 0) {
		$CollisionPhysicalWorkerJobs = if ($EffectiveWorkers -gt 1) {
			$CollisionCompleted
		} else { 0 }
	}
	if ($CollisionOwnerHelpedJobs -lt 0) {
		$CollisionOwnerHelpedJobs = $CollisionCompleted -
			$CollisionPhysicalWorkerJobs
	}
	if ($CollisionDistinctPhysicalWorkers -lt 0) {
		$CollisionDistinctPhysicalWorkers = if ($CollisionPhysicalWorkerJobs -gt 0) {
			[Math]::Min(2, $EffectiveWorkers)
		} else { 0 }
	}
	if ($CollisionPhysicalWorkerMask -lt 0) {
		$CollisionPhysicalWorkerMask = if ($CollisionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $CollisionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
    if ($CollisionIneligibleSlices -lt 0) { $CollisionIneligibleSlices = if ($Mode -ceq 'serial') { 0 } else { 2 } }
    if ($CollisionShadowComparedCandidates -lt 0) {
        $CollisionShadowComparedCandidates = if ($CollisionShadowExecutions -gt 0) { 6 } else { 0 }
    }
	$pathBatchEligible = $Mode -ceq 'parallel' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	if ($PathWorkerExecuted -lt 0) { $PathWorkerExecuted = if ($pathBatchEligible) { 4 } else { 0 } }
	if ($PathAuthoritativeCommits -lt 0) { $PathAuthoritativeCommits = if ($pathBatchEligible) { 2 } else { 0 } }
	if ($PathAuthoritativeMultiWorkerCommits -lt 0) {
		$PathAuthoritativeMultiWorkerCommits = if ($pathBatchEligible) {
			$PathAuthoritativeCommits
		} else { 0 }
	}
	$pathExecuted = $PathWorkerExecuted + $PathOwnerHelped
	$pathSubmitted = $pathExecuted
	$pathEligible = if ($pathSubmitted -gt 0) { $pathSubmitted + 1 } else { 0 }
	if ($PathPeakWorkers -lt 0) {
		$PathPeakWorkers = if ($PathWorkerExecuted -gt 1) {
			[Math]::Min(2, $EffectiveWorkers)
		} elseif ($PathWorkerExecuted -gt 0) { 1 } else { 0 }
	}
	$pathCallbackMin = if ($pathEligible -gt 0) { 2 } else { 0 }
	$pathCallbackMax = if ($pathEligible -gt 0) { 18 } else { 0 }
	$ordinaryParallelEligible = $Mode -ceq 'parallel' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	$ordinaryShadowEligible = $Mode -ceq 'shadow' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 0
	$ordinaryBatchEligible = $ordinaryParallelEligible -or $ordinaryShadowEligible
	if ($OrdinaryPathEligible -lt 0) {
		$OrdinaryPathEligible = if ($ordinaryBatchEligible) { 7 } else { 0 }
	}
	if ($OrdinaryPathSubmittedRequests -lt 0) {
		$OrdinaryPathSubmittedRequests = if ($ordinaryBatchEligible) { 6 } else { 0 }
	}
	if ($OrdinaryPathSubmittedRanges -lt 0) {
		$OrdinaryPathSubmittedRanges = if ($ordinaryBatchEligible) {
			[Math]::Min(4, [Math]::Min($EffectiveWorkers,
				$OrdinaryPathSubmittedRequests))
		} else { 0 }
	}
	if ($OrdinaryPathWorkerExecutedRequests -lt 0) {
		$OrdinaryPathWorkerExecutedRequests = $OrdinaryPathSubmittedRequests
	}
	if ($OrdinaryPathWorkerExecutedRangeJobs -lt 0) {
		$OrdinaryPathWorkerExecutedRangeJobs = $OrdinaryPathSubmittedRanges -
			$OrdinaryPathOwnerHelpedRangeJobs - $OrdinaryPathFailedRangeJobs
	}
	if ($OrdinaryPathDistinctPhysicalWorkers -lt 0) {
		$OrdinaryPathDistinctPhysicalWorkers = if (
			$OrdinaryPathWorkerExecutedRangeJobs -gt 0) {
			[Math]::Min(2, [Math]::Min($EffectiveWorkers,
				$OrdinaryPathWorkerExecutedRangeJobs))
		} else { 0 }
	}
	if ($OrdinaryPathPhysicalWorkerMask -lt 0) {
		$OrdinaryPathPhysicalWorkerMask = if (
			$OrdinaryPathDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $OrdinaryPathDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($OrdinaryPathAuthoritativeCommits -lt 0) {
		$OrdinaryPathAuthoritativeCommits = if ($ordinaryParallelEligible) { 3 } else { 0 }
	}
	if ($OrdinaryPathAuthoritativeMultiWorkerCommits -lt 0) {
		$OrdinaryPathAuthoritativeMultiWorkerCommits = if (
			$ordinaryParallelEligible -and
			$OrdinaryPathDistinctPhysicalWorkers -gt 1) {
			$OrdinaryPathAuthoritativeCommits
		} else { 0 }
	}
	if ($OrdinaryPathShadowComparisons -lt 0) {
		$OrdinaryPathShadowComparisons = if ($ordinaryShadowEligible) { 4 } else { 0 }
	}
	if ($OrdinaryPathPeakWorkers -lt 0) {
		$OrdinaryPathPeakWorkers = $OrdinaryPathDistinctPhysicalWorkers
	}
	if ($OrdinaryPathMaximumBatchRequests -lt 0) {
		$OrdinaryPathMaximumBatchRequests = $OrdinaryPathSubmittedRequests
	}
	if ($OrdinaryPathMaximumRangeCount -lt 0) {
		$OrdinaryPathMaximumRangeCount = $OrdinaryPathSubmittedRanges
	}
	if ($OrdinaryPathMaximumGrainSize -lt 0) {
		$OrdinaryPathMaximumGrainSize = if ($OrdinaryPathSubmittedRanges -gt 0) {
			[int][Math]::Ceiling($OrdinaryPathSubmittedRequests /
				[double]$OrdinaryPathSubmittedRanges)
		} else { 0 }
	}
	$physicsAuthoritativeEligible = $Mode -ceq 'parallel' -and $RequestedWorkers -cne '1'
	if ($PhysicsAuthoritativeBatches -lt 0) { $PhysicsAuthoritativeBatches = if ($physicsAuthoritativeEligible) { 3 } else { 0 } }
	if ($PhysicsCommittedPrefixes -lt 0) { $PhysicsCommittedPrefixes = if ($physicsAuthoritativeEligible) { 96 } else { 0 } }
	if ($PhysicsRanges -lt 0) { $PhysicsRanges = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsSubmitted -lt 0) { $PhysicsSubmitted = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsCompleted -lt 0) { $PhysicsCompleted = $PhysicsSubmitted }
	if ($PhysicsPhysicalWorkerJobs -lt 0) {
		$PhysicsPhysicalWorkerJobs = if ($physicsAuthoritativeEligible) { $PhysicsCompleted } else { 0 }
	}
	if ($PhysicsOwnerHelpedJobs -lt 0) {
		$PhysicsOwnerHelpedJobs = $PhysicsCompleted - $PhysicsPhysicalWorkerJobs
	}
	if ($PhysicsDistinctPhysicalWorkers -lt 0) {
		$PhysicsDistinctPhysicalWorkers = if ($PhysicsPhysicalWorkerJobs -gt 0) {
			[Math]::Min(2, $EffectiveWorkers)
		} else { 0 }
	}
	if ($PhysicsPhysicalWorkerMask -lt 0) {
		$PhysicsPhysicalWorkerMask = if ($PhysicsDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $PhysicsDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($PhysicsPeakConcurrentPhysicalWorkers -lt 0) {
		$PhysicsPeakConcurrentPhysicalWorkers = $PhysicsDistinctPhysicalWorkers
	}
	$statusLiveEligible = $Mode -ceq 'parallel' -and $RequestedWorkers -cne '1' -and
		$EffectiveWorkers -gt 1
	if ($StatusAuthoritativeBatches -lt 0) { $StatusAuthoritativeBatches = if ($statusLiveEligible) { 3 } else { 0 } }
	if ($StatusCommittedCommands -lt 0) { $StatusCommittedCommands = if ($StatusAuthoritativeBatches -gt 0) { 96 } else { 0 } }
	if ($StatusSubmitted -lt 0) { $StatusSubmitted = if ($statusLiveEligible) { 4 } else { 0 } }
	if ($StatusCompleted -lt 0) { $StatusCompleted = $StatusSubmitted }
	if ($StatusPhysicalWorkerJobs -lt 0) { $StatusPhysicalWorkerJobs = if ($statusLiveEligible) { $StatusCompleted } else { 0 } }
	if ($StatusOwnerHelpedJobs -lt 0) { $StatusOwnerHelpedJobs = $StatusCompleted - $StatusPhysicalWorkerJobs }
	if ($StatusDistinctPhysicalWorkers -lt 0) {
		$StatusDistinctPhysicalWorkers = if ($StatusPhysicalWorkerJobs -gt 0) { [Math]::Min(2, $EffectiveWorkers) } else { 0 }
	}
	if ($StatusPhysicalWorkerMask -lt 0) {
		$StatusPhysicalWorkerMask = if ($StatusDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $StatusDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($StatusPeakConcurrentPhysicalWorkers -lt 0) { $StatusPeakConcurrentPhysicalWorkers = $StatusDistinctPhysicalWorkers }
	if ($StatusShadowExecutions -lt 0) {
		$StatusShadowExecutions = if ($Mode -ceq 'shadow' -and $RequestedWorkers -cne '1') { 3 } else { 0 }
	}
	if ($StatusShadowCommands -lt 0) { $StatusShadowCommands = if ($StatusShadowExecutions -gt 0) { 96 } else { 0 } }
	$statusShadowMatches = $StatusShadowExecutions - $StatusShadowMismatches
	if ($PhysicsShadowPrefixes -lt 0) { $PhysicsShadowPrefixes = if ($PhysicsShadowExecutions -gt 0) { 96 } else { 0 } }
	if ($PhysicsShadowRanges -lt 0) { $PhysicsShadowRanges = if ($PhysicsShadowExecutions -gt 0) { 4 } else { 0 } }
	if ($PhysicsShadowSubmitted -lt 0) { $PhysicsShadowSubmitted = if ($PhysicsShadowExecutions -gt 0) { 4 } else { 0 } }
	if ($PhysicsShadowCompleted -lt 0) { $PhysicsShadowCompleted = $PhysicsShadowSubmitted }
	$physicsShadowMatches = $PhysicsShadowExecutions - $PhysicsShadowMismatches
	$physicsPreparedWork = $PhysicsSubmitted -gt 0 -or $PhysicsShadowSubmitted -gt 0
	$physicsAllocatedBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsCaptureNanoseconds = if ($physicsPreparedWork) { 100 } else { 0 }
	$physicsPrepareNanoseconds = if ($physicsPreparedWork) { 200 } else { 0 }
	$physicsWaitNanoseconds = if ($physicsPreparedWork) { 300 } else { 0 }
	$physicsCommitNanoseconds = if ($physicsPreparedWork) { 400 } else { 0 }
	$physicsStorageBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsStorageCapacityBytes = if ($physicsPreparedWork) { 8192 } else { 0 }
	$physicsStorageAllocations = if ($physicsPreparedWork) { 1 } else { 0 }
	$spatialParallelEligible = $Mode -ceq 'parallel' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	$spatialShadowEligible = $Mode -ceq 'shadow' -and
		$RequestedWorkers -cne '1' -and $EffectiveWorkers -gt 1
	$spatialCollectionEligible = $spatialParallelEligible -or $spatialShadowEligible
	if ($SpatialCapturedArenas -lt 0) {
		$SpatialCapturedArenas = if ($spatialCollectionEligible) { 4 } else { 0 }
	}
	if ($SpatialSuccessfulCollections -lt 0) { $SpatialSuccessfulCollections = if ($spatialCollectionEligible) { 4 } else { 0 } }
	if ($SpatialMaximumCollectionQueries -lt 0) { $SpatialMaximumCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 5 } else { 0 } }
	if ($SpatialMaximumCollectionRanges -lt 0) {
		$SpatialMaximumCollectionRanges = if ($SpatialSuccessfulCollections -gt 0) {
			[Math]::Min($EffectiveWorkers, $SpatialMaximumCollectionQueries)
		} else { 0 }
	}
	if ($SpatialSuccessfulCollectionQueries -lt 0) { $SpatialSuccessfulCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 20 } else { 0 } }
	if ($SpatialSuccessfulCollectionRanges -lt 0) {
		$SpatialSuccessfulCollectionRanges = $SpatialSuccessfulCollections *
			$SpatialMaximumCollectionRanges
	}
	if ($SpatialMultiRangeCollections -lt 0) { $SpatialMultiRangeCollections = $SpatialSuccessfulCollections }
	if ($SpatialCollectionSubmitted -lt 0) { $SpatialCollectionSubmitted = 2 * $SpatialSuccessfulCollectionRanges }
	if ($SpatialCollectionCompleted -lt 0) { $SpatialCollectionCompleted = $SpatialCollectionSubmitted }
	if ($SpatialCollectionPhysical -lt 0) { $SpatialCollectionPhysical = $SpatialCollectionCompleted }
	if ($SpatialMaximumCollectionDistinctPhysicalWorkers -lt 0) {
		$SpatialMaximumCollectionDistinctPhysicalWorkers = if ($SpatialSuccessfulCollections -gt 0) {
			if ($SpatialMaximumCollectionRanges -ge 4) { 2 } else { 1 }
		} else { 0 }
	}
	if ($SpatialCollectionPhysicalWorkerMask -lt 0) {
		$SpatialCollectionPhysicalWorkerMask = if ($SpatialMaximumCollectionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $SpatialMaximumCollectionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($SpatialHealingAuthoritative -lt 0) { $SpatialHealingAuthoritative = if ($spatialParallelEligible) { 3 } else { 0 } }
	if ($SpatialHealingCandidates -lt 0) { $SpatialHealingCandidates = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingShadow -lt 0) { $SpatialHealingShadow = if ($spatialShadowEligible) { 2 } else { 0 } }
	if ($SpatialPdlAuthoritative -lt 0) { $SpatialPdlAuthoritative = if ($spatialParallelEligible) { 2 } else { 0 } }
	if ($SpatialPdlCandidates -lt 0) { $SpatialPdlCandidates = if ($SpatialPdlAuthoritative -gt 0) { 5 } else { 0 } }
	if ($SpatialPdlShadow -lt 0) { $SpatialPdlShadow = if ($spatialShadowEligible) { 3 } else { 0 } }
	$spatialHasJobs = $SpatialHealingAuthoritative -gt 0 -or $SpatialHealingShadow -gt 0
	if ($SpatialHealingSubmitted -lt 0) { $SpatialHealingSubmitted = if ($spatialHasJobs) { 8 } else { 0 } }
	if ($SpatialHealingCompleted -lt 0) { $SpatialHealingCompleted = $SpatialHealingSubmitted }
	if ($SpatialHealingPhysical -lt 0) { $SpatialHealingPhysical = $SpatialHealingCompleted }
	$spatialPdlHasJobs = $SpatialPdlAuthoritative -gt 0 -or $SpatialPdlShadow -gt 0
	if ($SpatialPdlSubmitted -lt 0) { $SpatialPdlSubmitted = if ($spatialPdlHasJobs) { 8 } else { 0 } }
	if ($SpatialPdlCompleted -lt 0) { $SpatialPdlCompleted = $SpatialPdlSubmitted }
	if ($SpatialPdlPhysical -lt 0) { $SpatialPdlPhysical = $SpatialPdlCompleted }
	if ($SpatialHealingExpectedFallbacks -lt 0) {
		$SpatialHealingExpectedFallbacks = if ($Mode -ceq 'serial' -or $RequestedWorkers -ceq '1') { 2 } else { 0 }
	}
	if ($SpatialPdlExpectedFallbacks -lt 0) {
		$SpatialPdlExpectedFallbacks = if ($Mode -ceq 'serial' -or $RequestedWorkers -ceq '1') { 2 } else { 0 }
	}
	$spatialHealingMatches = $SpatialHealingShadow - $SpatialHealingShadowMismatches
	$spatialPdlMatches = $SpatialPdlShadow - $SpatialPdlShadowMismatches
    $workEvidence = if ($OmitWorkEvidence) { '' } else {
        " authoritative_commits=$AuthoritativeCommits shadow_executions=$ShadowExecutions owner_fallbacks=$OwnerFallbacks" +
        " ai_captured_snapshots=5 ai_captured_candidates=20 ai_requested_batches=$AiRequestedBatches" +
        " ai_submitted_jobs=$AiSubmitted ai_completed_jobs=$AiCompleted ai_serial_fallbacks=$OwnerFallbacks" +
        " ai_shadow_matches=$ShadowExecutions ai_shadow_mismatches=0 ai_validation_failures=0" +
        " ai_committed_batches=$AiCommittedBatches" +
        " ai_parallel_authoritative_commits=$AiParallelAuthoritativeCommits ai_rejected_commits=0" +
		" direct_eligible=$pathEligible direct_submitted=$pathSubmitted direct_executed=$pathExecuted" +
		" direct_worker_executed=$PathWorkerExecuted direct_owner_helped=$PathOwnerHelped" +
		" direct_authoritative_commits=$PathAuthoritativeCommits direct_stale_rejections=0" +
		" direct_authoritative_multiworker_commits=$PathAuthoritativeMultiWorkerCommits" +
		" direct_validation_failures=$PathValidationFailures direct_serial_fallbacks=0" +
		" direct_unsupported_authority=$PathUnsupportedAuthority direct_shadow_authority=$PathShadowAuthority" +
		" direct_stale_acceptance=$PathStaleAcceptance direct_malformed_acceptance=$PathMalformedAcceptance" +
		" direct_shadow_only=$PathShadowOnly direct_timeouts=$PathTimeouts" +
		" direct_late_drains=$PathLateDrains direct_peak_active_workers=$PathPeakWorkers" +
		" direct_callback_min=$pathCallbackMin direct_callback_max=$pathCallbackMax" +
		" ordinary_path_eligible=$OrdinaryPathEligible" +
		" ordinary_path_submitted_requests=$OrdinaryPathSubmittedRequests" +
		" ordinary_path_submitted_ranges=$OrdinaryPathSubmittedRanges" +
		" ordinary_path_worker_executed_requests=$OrdinaryPathWorkerExecutedRequests" +
		" ordinary_path_worker_executed_range_jobs=$OrdinaryPathWorkerExecutedRangeJobs" +
		" ordinary_path_owner_helped_range_jobs=$OrdinaryPathOwnerHelpedRangeJobs" +
		" ordinary_path_failed_range_jobs=$OrdinaryPathFailedRangeJobs" +
		" ordinary_path_physical_worker_mask=$OrdinaryPathPhysicalWorkerMask" +
		" ordinary_path_distinct_physical_workers=$OrdinaryPathDistinctPhysicalWorkers" +
		" ordinary_path_physical_worker_mask_complete=$OrdinaryPathPhysicalWorkerMaskComplete" +
		" ordinary_path_authoritative_commits=$OrdinaryPathAuthoritativeCommits" +
		" ordinary_path_authoritative_multiworker_commits=$OrdinaryPathAuthoritativeMultiWorkerCommits" +
		" ordinary_path_stale_rejections=$OrdinaryPathStaleRejections" +
		" ordinary_path_validation_failures=$OrdinaryPathValidationFailures" +
		" ordinary_path_serial_fallbacks=$OrdinaryPathSerialFallbacks" +
		" ordinary_path_shadow_comparisons=$OrdinaryPathShadowComparisons" +
		" ordinary_path_shadow_mismatches=$OrdinaryPathShadowMismatches" +
		" ordinary_path_timeouts=$OrdinaryPathTimeouts" +
		" ordinary_path_late_drains=$OrdinaryPathLateDrains" +
		" ordinary_path_peak_active_workers=$OrdinaryPathPeakWorkers" +
		" ordinary_path_max_batch_requests=$OrdinaryPathMaximumBatchRequests" +
		" ordinary_path_max_range_count=$OrdinaryPathMaximumRangeCount" +
		" ordinary_path_max_grain_size=$OrdinaryPathMaximumGrainSize" +
        " collision_authoritative_commits=$CollisionAuthoritativeCommits" +
        " collision_shadow_executions=$CollisionShadowExecutions" +
        " collision_shadow_compared_candidates=$CollisionShadowComparedCandidates" +
        " collision_shadow_mismatches=$CollisionShadowMismatches" +
        " collision_owner_fallbacks=$CollisionOwnerFallbacks" +
        " collision_unexpected_fallbacks=$CollisionUnexpectedFallbacks" +
		" collision_ineligible_slices=$CollisionIneligibleSlices collision_stale_rejections=$CollisionStaleRejections" +
        " collision_committed_candidates=$CollisionCommittedCandidates" +
        " collision_prepared_pairs=$CollisionPreparedPairs" +
        " collision_unique_candidates=$CollisionUniqueCandidates" +
        " collision_submitted_jobs=$CollisionSubmitted collision_completed_jobs=$CollisionCompleted" +
		" collision_physical_worker_jobs=$CollisionPhysicalWorkerJobs" +
		" collision_owner_helped_jobs=$CollisionOwnerHelpedJobs" +
		" collision_physical_worker_mask=$CollisionPhysicalWorkerMask" +
		" collision_distinct_physical_workers=$CollisionDistinctPhysicalWorkers" +
		" collision_physical_worker_mask_complete=$CollisionPhysicalWorkerMaskComplete" +
		" physics_authoritative_batches=$PhysicsAuthoritativeBatches" +
		" physics_committed_prefixes=$PhysicsCommittedPrefixes physics_ranges=$PhysicsRanges" +
		" physics_submitted_jobs=$PhysicsSubmitted physics_completed_jobs=$PhysicsCompleted" +
		" physics_physical_worker_jobs=$PhysicsPhysicalWorkerJobs physics_owner_helped_jobs=$PhysicsOwnerHelpedJobs" +
		" physics_physical_worker_mask=$PhysicsPhysicalWorkerMask physics_distinct_physical_workers=$PhysicsDistinctPhysicalWorkers" +
		" physics_physical_worker_mask_complete=$PhysicsPhysicalWorkerMaskComplete" +
		" physics_peak_concurrent_physical_workers=$PhysicsPeakConcurrentPhysicalWorkers" +
		" physics_allocated_bytes=$physicsAllocatedBytes physics_capture_ns=$physicsCaptureNanoseconds physics_prepare_ns=$physicsPrepareNanoseconds" +
		" physics_wait_ns=$physicsWaitNanoseconds physics_commit_ns=$physicsCommitNanoseconds physics_storage_bytes=$physicsStorageBytes" +
		" physics_storage_capacity_bytes=$physicsStorageCapacityBytes physics_storage_allocations=$physicsStorageAllocations" +
		" physics_shadow_executions=$PhysicsShadowExecutions" +
		" physics_shadow_prefixes=$PhysicsShadowPrefixes physics_shadow_ranges=$PhysicsShadowRanges" +
		" physics_shadow_submitted_jobs=$PhysicsShadowSubmitted" +
		" physics_shadow_completed_jobs=$PhysicsShadowCompleted" +
		" physics_shadow_matches=$physicsShadowMatches" +
		" physics_shadow_mismatches=$PhysicsShadowMismatches" +
		" physics_owner_fallbacks=$PhysicsOwnerFallbacks physics_ineligible_slices=2" +
		" physics_unexpected_fallbacks=$PhysicsUnexpectedFallbacks" +
		" physics_stale_rejections=$PhysicsStaleRejections" +
		" physics_circuit_breaker_trips=$PhysicsCircuitBreakerTrips" +
		" status_authoritative_batches=$StatusAuthoritativeBatches status_committed_commands=$StatusCommittedCommands" +
		" status_submitted_jobs=$StatusSubmitted status_completed_jobs=$StatusCompleted" +
		" status_physical_worker_jobs=$StatusPhysicalWorkerJobs status_owner_helped_jobs=$StatusOwnerHelpedJobs" +
		" status_physical_worker_mask=$StatusPhysicalWorkerMask status_distinct_physical_workers=$StatusDistinctPhysicalWorkers" +
		" status_physical_worker_mask_complete=$StatusPhysicalWorkerMaskComplete" +
		" status_peak_concurrent_physical_workers=$StatusPeakConcurrentPhysicalWorkers" +
		" status_shadow_executions=$StatusShadowExecutions status_shadow_commands=$StatusShadowCommands" +
		" status_shadow_matches=$statusShadowMatches status_shadow_mismatches=$StatusShadowMismatches" +
		" status_owner_fallbacks=$StatusOwnerFallbacks status_stale_rejections=$StatusStaleRejections" +
		" spatial_captured_arenas=$SpatialCapturedArenas spatial_capture_failures=$SpatialCaptureFailures" +
		" spatial_successful_collections=$SpatialSuccessfulCollections" +
		" spatial_successful_collection_queries=$SpatialSuccessfulCollectionQueries" +
		" spatial_successful_collection_ranges=$SpatialSuccessfulCollectionRanges" +
		" spatial_multi_range_collections=$SpatialMultiRangeCollections" +
		" spatial_collection_submitted_jobs=$SpatialCollectionSubmitted" +
		" spatial_collection_completed_jobs=$SpatialCollectionCompleted" +
		" spatial_collection_physical_worker_jobs=$SpatialCollectionPhysical" +
		" spatial_collection_owner_helped_jobs=$SpatialCollectionOwnerHelped" +
		" spatial_collection_physical_worker_mask=$SpatialCollectionPhysicalWorkerMask" +
		" spatial_maximum_collection_queries=$SpatialMaximumCollectionQueries" +
		" spatial_maximum_collection_ranges=$SpatialMaximumCollectionRanges" +
		" spatial_maximum_collection_distinct_physical_workers=$SpatialMaximumCollectionDistinctPhysicalWorkers" +
		" spatial_healing_eligible_queries=$SpatialHealingEligible" +
		" spatial_healing_authoritative_queries=$SpatialHealingAuthoritative" +
		" spatial_healing_authoritative_candidates=$SpatialHealingCandidates" +
		" spatial_healing_shadow_queries=$SpatialHealingShadow" +
		" spatial_healing_shadow_matches=$spatialHealingMatches" +
		" spatial_healing_shadow_mismatches=$SpatialHealingShadowMismatches" +
		" spatial_healing_submitted_jobs=$SpatialHealingSubmitted" +
		" spatial_healing_completed_jobs=$SpatialHealingCompleted" +
		" spatial_healing_physical_worker_jobs=$SpatialHealingPhysical" +
		" spatial_healing_owner_helped_jobs=$SpatialHealingOwnerHelped" +
		" spatial_healing_expected_fallbacks=$SpatialHealingExpectedFallbacks" +
		" spatial_healing_unexpected_fallbacks=$SpatialHealingUnexpectedFallbacks" +
		" spatial_healing_stale_rejections=0 spatial_healing_validation_failures=0" +
		" spatial_healing_circuit_breaker_trips=0" +
		" spatial_pdl_eligible_queries=$SpatialPdlEligible" +
		" spatial_pdl_authoritative_queries=$SpatialPdlAuthoritative" +
		" spatial_pdl_authoritative_candidates=$SpatialPdlCandidates" +
		" spatial_pdl_shadow_queries=$SpatialPdlShadow" +
		" spatial_pdl_shadow_matches=$spatialPdlMatches" +
		" spatial_pdl_shadow_mismatches=$SpatialPdlShadowMismatches" +
		" spatial_pdl_submitted_jobs=$SpatialPdlSubmitted" +
		" spatial_pdl_completed_jobs=$SpatialPdlCompleted" +
		" spatial_pdl_physical_worker_jobs=$SpatialPdlPhysical" +
		" spatial_pdl_owner_helped_jobs=$SpatialPdlOwnerHelped" +
		" spatial_pdl_expected_fallbacks=$SpatialPdlExpectedFallbacks" +
		" spatial_pdl_unexpected_fallbacks=$SpatialPdlUnexpectedFallbacks" +
		" spatial_pdl_stale_rejections=0 spatial_pdl_validation_failures=0" +
		" spatial_pdl_circuit_breaker_trips=0"
    }
    return "SKIRMISH_AI_TEST_COMPLETE seed=$Seed loaded_seed=$LoadedSeed scenario=$Scenario actual_ai=$ActualAi actual_teams=$ActualTeams winner_team=$Winner end_frame=$EndFrame " +
        "executable_sha256=$ExecutableHash simulation_mode=$Mode requested_pipeline=$RequestedPipeline effective_pipeline=$EffectivePipeline " +
        "requested_simulation=$RequestedSimulation effective_simulation=$EffectiveSimulation requested_workers=$RequestedWorkers " +
        "effective_workers=$EffectiveWorkers worker_policy=auto final_digest=$Digest wall_ms=100 " +
        "job_submitted=$Submitted job_executed=$Executed job_steals=0 job_owner_help=0 job_waits=0 " +
        "job_worker_wait_reject=0 job_failed=0 job_cancelled=0 job_fallback=$Fallback " +
        "job_queue_latency_ns=1 job_max_queue_latency_ns=1 job_sleeps=0 job_wakes=0 " +
        "job_affinity_failures=0$workEvidence job_queue_high_water=1 job_peak_active_workers=$EffectiveWorkers " +
        "available_cpus=16 reserved_owner_cpus=1 selected_worker_cpus=$EffectiveWorkers"
}

function New-Stage5LiveRoleTestPlan {
    # This is a role-resolution fixture, not a complete acceptance matrix.
    return [pscustomobject]@{
        schemaVersion = 2
        liveQualification = [pscustomobject]@{
            schemaVersion = 1
            profileSetId = 'live-all-slices-v1'
            authorityEntries = @([pscustomobject]@{
                scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
            })
            shadowEntry = [pscustomobject]@{
                scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
            }
        }
        entries = @(
            [pscustomobject]@{
                sequence = 11; entryId = 'ai-0011'; kind = 'ai'; stress = $true
                scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 1
                simulationMode = 'parallel'; requestedWorkers = '2'; workerPolicy = 'auto'
                determinismKey = '4v2-seed-1729'
                validationRole = 'live-determinism'; proofProfileId = 'live-invariants-v1'
            },
            [pscustomobject]@{
                sequence = 12; entryId = 'ai-0012'; kind = 'ai'; stress = $true
                scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 2
                simulationMode = 'parallel'; requestedWorkers = '2'; workerPolicy = 'auto'
                determinismKey = '4v2-seed-1729'
                validationRole = 'live-authority-stress'
                proofProfileId = 'live-all-slices-authority-v1'
            },
            [pscustomobject]@{
                sequence = 13; entryId = 'ai-0013'; kind = 'ai'; stress = $true
                scenario = '4v2'; seed = 1729; configuration = 'shadow-16'; repeat = 1
                simulationMode = 'shadow'; requestedWorkers = '16'; workerPolicy = 'auto'
                determinismKey = '4v2-seed-1729'
                validationRole = 'live-shadow-stress'; proofProfileId = 'live-all-slices-shadow-v1'
            }
        )
    }
}

function Assert-Stage5LivePlanPrelaunchBinding {
    # Execute real function bodies without the runner's top-level workflow.
    # No test executable is written: even an omitted guard cannot start a child.
    $parseTokens = $null; $parseErrors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'),
        [ref]$parseTokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) 'live plan prelaunch runner parses without errors'
    foreach ($definition in $tree.EndBlock.Statements) {
        if ($definition -is [System.Management.Automation.Language.FunctionDefinitionAst]) {
            Invoke-Expression $definition.Extent.Text
        }
    }

    $directory = Join-Path $root 'live-plan-prelaunch'
    New-Item -ItemType Directory -Path $directory | Out-Null
    $executable = Join-Path $directory ('never-created-' + [Guid]::NewGuid().ToString('N') + '.exe')
    Assert-True (-not (Test-Path -LiteralPath $executable)) 'prelaunch fixture executable cannot run'
    $plan = New-Stage5LiveRoleTestPlan
    foreach ($entry in $plan.entries) {
        $entryDirectory = Join-Path $directory $entry.entryId
        $entry | Add-Member -NotePropertyMembers @{
            arguments = @('-headless', '-noFPSLimit', '-pipelineMode', 'serial',
                '-simulationMode', $entry.simulationMode, '-workerPolicy', 'auto',
                '-workerCount', $entry.requestedWorkers, '-validationExecutableSha256', ('A' * 64))
            command = ''; timeoutSeconds = 1
            stdout = Join-Path $entryDirectory 'stdout.log'
            stderr = Join-Path $entryDirectory 'stderr.log'
            timingDirectory = Join-Path $entryDirectory 'timing'
            runtimeLogDirectory = Join-Path $entryDirectory 'runtime-logs'
        }
        $entry.command = ConvertTo-DisplayCommand $executable $entry.arguments
    }
    # Metadata only: this fixture never redirects Documents or creates this
    # path. Hosted unit-test scratch may be on a different volume.
    $documentsRoot = 'H:\Stage5SyntheticDocuments'
    $launcher = [pscustomobject]@{
        executable = [IO.Path]::GetFileName($executable); arguments = @()
        launcherPath = Join-Path $directory 'launcher.exe'
        configPath = Join-Path $directory 'launcher.lcf'
    }
    Assert-Throws {
        Assert-LauncherEquivalenceContract $launcher $executable $directory `
            $plan.entries 'FinalizedProfile' 'D:\Stage5SyntheticDocuments' | Out-Null
    } 'Validation Documents root must remain on H' `
        'installed Documents safety remains enforced for metadata outside H'
    $plan | Add-Member -NotePropertyMembers @{
        runtimeRoot = $directory; executable = $executable; executableSha256 = ('A' * 64)
        title = 'ZeroHour'; cohortNonce = $script:TestCohortNonce
        cohortCreatedUtc = $script:TestCohortCreatedUtc
        launcherContract = Assert-LauncherEquivalenceContract $launcher $executable `
            $directory $plan.entries 'FinalizedProfile' $documentsRoot
    }

    $freezePath = Join-Path $directory 'final-validation-plan.json'
    $frozen = $null
    $freezeError = ''
    try { $frozen = Write-Stage5FrozenValidationPlan -Plan $plan -Path $freezePath }
    catch { $freezeError = $_.Exception.Message }
    Assert-True ($null -ne $frozen) "finalized live plan freezes before child preparation (got '$freezeError')"
    if ($null -ne $frozen) {
        $writtenJson = [IO.File]::ReadAllText($freezePath)
        $writtenPlan = $writtenJson | ConvertFrom-Json
        Assert-True ($frozen.planPath -ceq $freezePath -and $frozen.planJson -ceq $writtenJson -and
            $frozen.planSha256 -ceq (Get-Sha256Text $writtenJson) -and
            $writtenPlan.launcherContract.profileLeafName -ceq 'FinalizedProfile' -and
            $writtenPlan.launcherContract.documentsRoot -ceq $documentsRoot) `
            'frozen plan owns the exact final launcher/profile bytes and their original hash'
        $originalHash = $frozen.planSha256
        Assert-Throws { $frozen.planJson = '{}' } 'read.only|set|property|key' `
            'a caller cannot replace immutable frozen plan text'
        Assert-Throws { $frozen.planSha256 = ('F' * 64) } 'read.only|set|property|key' `
            'a caller cannot replace the original frozen plan hash'
        Assert-True ($frozen.planJson -ceq $writtenJson -and $frozen.planSha256 -ceq $originalHash) `
            'failed frozen binding mutations preserve the original plan authority'
        Assert-Throws { Write-Stage5FrozenValidationPlan -Plan $plan -Path $freezePath | Out-Null } `
            'exist|create.new|overwrite' 'a frozen plan cannot be rewritten for a later child'
        Assert-True ([IO.File]::ReadAllText($freezePath) -ceq $writtenJson) `
            'a refused second freeze preserves the first plan bytes'
    }

    $existingPath = Join-Path $directory 'existing-plan.json'
    [IO.File]::WriteAllText($existingPath, 'previous cohort evidence')
    Assert-Throws { Write-Stage5FrozenValidationPlan -Plan $plan -Path $existingPath | Out-Null } `
        'exist|create.new|overwrite' 'freezing refuses an already occupied plan path'
    Assert-True ([IO.File]::ReadAllText($existingPath) -ceq 'previous cohort evidence') `
        'failed freezing never overwrites existing evidence'

    # Independent on-disk fixture lets resolution and the real invocation
    # boundary fail behaviorally even while the create-new seam is fail-closed.
    $bindingPath = Join-Path $directory 'bound-validation-plan.json'
    $bindingJson = $plan | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($bindingPath, $bindingJson, (New-Object Text.UTF8Encoding($false)))
    $bindingValues = New-Object 'Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
    $bindingValues.Add('planPath', $bindingPath)
    $bindingValues.Add('planJson', $bindingJson)
    $bindingValues.Add('planSha256', (Get-Sha256Text $bindingJson))
    $binding = New-Object 'Collections.ObjectModel.ReadOnlyDictionary[string,string]' -ArgumentList (,$bindingValues)
    $launchEntry = ($bindingJson | ConvertFrom-Json).entries[1]
    $plan.entries[1].validationRole = 'live-determinism'
    $plan.entries[1].proofProfileId = 'live-invariants-v1'
    $plan.entries[1].arguments[0] = '-mutated-after-freeze'
    $plan.liveQualification.authorityEntries[0].repeat = 1
    $plan.launcherContract.profileLeafName = 'MutatedProfile'
    $planDocument = $plan
    $executionCohortNonce = $script:TestCohortNonce
    $executionCohortCreatedUtc = $script:TestCohortCreatedUtc
    $resolved = $null
    $resolveError = ''
    try {
        $resolved = Resolve-Stage5FrozenLivePlanEntry -LivePlanBinding $binding -Entry $launchEntry `
            -Executable $executable -WorkingDirectory $directory
    }
    catch { $resolveError = $_.Exception.Message }
    Assert-True ($null -ne $resolved) `
        "frozen live entry resolves without ambient mutable-plan authority (got '$resolveError')"
    if ($null -ne $resolved) {
        Assert-True ($resolved.entryId -ceq 'ai-0012' -and
            $resolved.validationRole -ceq 'live-authority-stress' -and
            $resolved.proofProfileId -ceq 'live-all-slices-authority-v1' -and
            $resolved.arguments[0] -ceq '-headless') `
            'entry resolution retains the role and arguments selected before caller mutation'
    }
    if ($null -ne $frozen) {
        $stillFrozen = $frozen.planJson | ConvertFrom-Json
        Assert-True ($stillFrozen.liveQualification.authorityEntries[0].repeat -eq 2 -and
            $stillFrozen.launcherContract.profileLeafName -ceq 'FinalizedProfile' -and
            $stillFrozen.entries[1].arguments[0] -ceq '-headless') `
            'freezing owns nested selectors, launcher metadata, and arguments instead of caller aliases'
    }

    $invokeArguments = @{Executable=$executable;WorkingDirectory=$directory;Entry=$launchEntry
        CaptureTiming=$false;Environment=@{};EvidenceRoot=$directory;NativeObservationBinding=$null}
    Assert-Throws { Invoke-ValidationProcess @invokeArguments | Out-Null } `
        'frozen.*plan.*required|live.*plan.*binding.*required' `
        'V2 role-bearing entries require an explicit frozen binding before Process.Start'
    Assert-True (-not (Test-Path -LiteralPath (Split-Path -Parent $launchEntry.stdout))) `
        'missing V2 binding is rejected before creating child output directories'

    $strippedEntry = ($bindingJson | ConvertFrom-Json).entries[0]
    foreach ($name in @('entryId', 'validationRole', 'proofProfileId')) {
        $strippedEntry.PSObject.Properties.Remove($name)
    }
    $invokeArguments.Entry = $strippedEntry
    $invokeArguments.LivePlanBinding = $binding
    Assert-Throws { Invoke-ValidationProcess @invokeArguments | Out-Null } `
        'frozen (validation|live) plan|live plan binding|entryId|validationRole|proofProfileId' `
        'removing V2 entry metadata cannot downgrade an explicitly bound process to legacy'
    Assert-True (-not (Test-Path -LiteralPath (Split-Path -Parent $strippedEntry.stdout))) `
        'stripped V2 entry is rejected before creating child output directories'

    $tamperedPlan = $bindingJson | ConvertFrom-Json
    $tamperedPlan.entries[2].arguments[0] = '-changed-on-disk'
    [IO.File]::WriteAllText($bindingPath, ($tamperedPlan | ConvertTo-Json -Depth 12))
    $invokeArguments.Entry = ($bindingJson | ConvertFrom-Json).entries[2]
    Assert-Throws { Invoke-ValidationProcess @invokeArguments | Out-Null } `
        'frozen.*(hash|changed|bytes)|plan.*(hash|changed|bytes)' `
        'changed plan bytes reject the original binding rather than blessing a new hash'
    Assert-True (-not (Test-Path -LiteralPath (Split-Path -Parent $invokeArguments.Entry.stdout))) `
        'changed frozen plan is rejected before creating child output directories'
}

function Assert-Stage5LiveManifestPlanRouting {
    param([string]$Runtime, [string]$Manifest, [object]$LegacyPlan)
    Assert-True ($LegacyPlan.schemaVersion -eq 1 -and
        $null -eq $LegacyPlan.PSObject.Properties['liveQualification'] -and
        @($LegacyPlan.entries | Where-Object {
            $null -ne $_.PSObject.Properties['entryId'] -or
            $null -ne $_.PSObject.Properties['validationRole'] -or
            $null -ne $_.PSObject.Properties['proofProfileId']
        }).Count -eq 0) 'V1 public plans remain explicitly legacy without nullable V2 role properties'
    $manifestSchema = Join-Path $PSScriptRoot 'ReplayFixtureManifest.schema.json'
    Assert-True (Test-Stage5TestJsonSchema `
        (Get-Content -LiteralPath $Manifest -Raw) $manifestSchema) `
        'the replay-fixture schema accepts production-shaped V1 writer input'
    $v2 = [IO.File]::ReadAllText($Manifest) | ConvertFrom-Json
    $v2.schemaVersion = 2
    $v2.ai | Add-Member -NotePropertyName liveQualification -NotePropertyValue ([pscustomobject]@{
        schemaVersion = 1; profileSetId = 'live-all-slices-v1'
        authorityEntries = @(
            [pscustomobject]@{scenario='4v2';seed=1729;configuration='parallel-2';repeat=2},
            [pscustomobject]@{scenario='4v2';seed=1730;configuration='parallel-4';repeat=1})
        shadowEntry = [pscustomobject]@{scenario='4v2';seed=1729;configuration='shadow-16';repeat=1}
    })
    Assert-True (Test-Stage5TestJsonSchema `
        ($v2 | ConvertTo-Json -Depth 20) $manifestSchema) `
        'the replay-fixture schema accepts production-shaped V2 live qualification input'
    $schemaRepeatOverflow = $v2 | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $schemaRepeatOverflow.ai.liveQualification.authorityEntries[0].repeat = 11
    Assert-True (-not (Test-Stage5TestJsonSchema `
        ($schemaRepeatOverflow | ConvertTo-Json -Depth 20) $manifestSchema)) `
        'the replay-fixture schema rejects a V2 live selector repeat above ten'
    $schemaMatrixRepeatOverflow = $v2 | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $schemaMatrixRepeatOverflow.ai.repeats = 11
    Assert-True (-not (Test-Stage5TestJsonSchema `
        ($schemaMatrixRepeatOverflow | ConvertTo-Json -Depth 20) $manifestSchema)) `
        'the replay-fixture schema rejects a matrix repeat above ten'
    $schemaV1WithLive = $v2 | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $schemaV1WithLive.schemaVersion = 1
    Assert-True (-not (Test-Stage5TestJsonSchema `
        ($schemaV1WithLive | ConvertTo-Json -Depth 20) $manifestSchema)) `
        'the replay-fixture schema rejects V2 liveQualification on a V1 manifest'
    $schemaV2Extra = $v2 | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $schemaV2Extra.ai.liveQualification.shadowEntry | Add-Member `
        -NotePropertyName observedWorkers -NotePropertyValue 16
    Assert-True (-not (Test-Stage5TestJsonSchema `
        ($schemaV2Extra | ConvertTo-Json -Depth 20) $manifestSchema)) `
        'the replay-fixture schema rejects undeclared V2 selector fields'
    $manifestDirectory = Split-Path -Parent $Manifest
    $v2Path = Join-Path $manifestDirectory 'live-manifest-v2.json'
    [IO.File]::WriteAllText($v2Path, ($v2 | ConvertTo-Json -Depth 12))
    $output = Join-Path $root 'live-manifest-v2-plan'
    $errorText = ''
    try {
        & $scriptPath -RuntimeRoot $Runtime -FixtureManifestPath $v2Path -OutputRoot $output `
            -ValidationSet AI -MinimumFreeBytes 1 -PlanOnly | Out-Null
    }
    catch { $errorText = $_.Exception.Message }
    $planPath = Join-Path $output 'validation-plan.json'
    $generated = $errorText -ceq '' -and (Test-Path -LiteralPath $planPath)
    Assert-True $generated "public PlanOnly routes the explicit V2 live manifest (got '$errorText')"
    if ($generated) {
        $planned = [IO.File]::ReadAllText($planPath) | ConvertFrom-Json
        $authority = @($planned.entries | Where-Object { $_.validationRole -ceq 'live-authority-stress' })
        $shadow = @($planned.entries | Where-Object { $_.validationRole -ceq 'live-shadow-stress' })
        $ordinary = @($planned.entries | Where-Object { $_.validationRole -ceq 'live-determinism' })
        Assert-True ($planned.schemaVersion -eq 2 -and $planned.entries.Count -eq 85 -and
            $planned.liveQualification.profileSetId -ceq 'live-all-slices-v1' -and
            $planned.liveQualification.authorityEntries.Count -eq 2) `
            'V2 planning preserves the full 84-run regular matrix and one predeclared shadow'
        Assert-True (($authority.entryId -join '|') -ceq 'ai-0032|ai-0045' -and
            @($authority | Where-Object { $_.proofProfileId -ceq 'live-all-slices-authority-v1' }).Count -eq 2 -and
            $shadow.Count -eq 1 -and $shadow[0].entryId -ceq 'ai-0085' -and
            $shadow[0].proofProfileId -ceq 'live-all-slices-shadow-v1' -and
            $ordinary.Count -eq 82 -and
            @($ordinary | Where-Object { $_.proofProfileId -ceq 'live-invariants-v1' }).Count -eq 82) `
            'exact manifest tuples assign stable sequence IDs and closed profiles before any observed result'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $output 'runs')) -and
            -not (Test-Path -LiteralPath (Join-Path $output 'validation-results.json')) -and
            -not (Test-Path -LiteralPath (Join-Path $output 'validation-plan-receipt.json')) -and
            -not $planned.finalAcceptanceEligible) `
            'a V2 PlanOnly preview has no child attempt, result, or execution receipt'
    }
    foreach ($case in @(
        @{name='missing-qualification';pattern='liveQualification';edit={param($m) $m.ai.PSObject.Properties.Remove('liveQualification')}},
        @{name='incapable-authority';pattern='authority selector|incapable';edit={param($m) $m.ai.liveQualification.authorityEntries[0].configuration='parallel-1'}},
        @{name='wrong-shadow';pattern='shadow selector|shadow.*absent';edit={param($m) $m.ai.liveQualification.shadowEntry.configuration='shadow-8'}}
    )) {
        $invalid = [IO.File]::ReadAllText($v2Path) | ConvertFrom-Json
        & $case.edit $invalid
        $invalidPath = Join-Path $manifestDirectory ($case.name + '-live-manifest.json')
        [IO.File]::WriteAllText($invalidPath, ($invalid | ConvertTo-Json -Depth 12))
        $invalidOutput = Join-Path $root ($case.name + '-live-plan')
        Assert-Throws {
            & $scriptPath -RuntimeRoot $Runtime -FixtureManifestPath $invalidPath `
                -OutputRoot $invalidOutput -ValidationSet AI -MinimumFreeBytes 1 -PlanOnly | Out-Null
        } $case.pattern "public V2 planning rejects $($case.name) at its live contract boundary"
    }
    $v2.schemaVersion = 1
    $legacyTaggedPath = Join-Path $manifestDirectory 'legacy-tagged-live-manifest.json'
    [IO.File]::WriteAllText($legacyTaggedPath, ($v2 | ConvertTo-Json -Depth 12))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $Runtime -FixtureManifestPath $legacyTaggedPath `
            -OutputRoot (Join-Path $root 'legacy-tagged-live-plan') -ValidationSet AI `
            -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'unsupported property.*liveQualification|V1.*liveQualification' `
        'V1 cannot acquire V2 qualification by adding a nested live selector object'
}

function Assert-Stage5LivePlanCallerRouting {
    # Execute the actual top-level emission/finalization statements and child
    # call only. The profile/registry setup and the child loop are never run.
    $parseTokens = $null; $parseErrors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'),
        [ref]$parseTokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) 'live plan caller runner parses without errors'
    foreach ($definition in $tree.EndBlock.Statements) {
        if ($definition -is [System.Management.Automation.Language.FunctionDefinitionAst]) {
            Invoke-Expression $definition.Extent.Text
        }
    }
    $topLevel = @($tree.EndBlock.Statements)
    $initial = New-Object 'Collections.Generic.List[string]'
    $collecting = $false
    foreach ($statement in $topLevel) {
        if ($statement -is [System.Management.Automation.Language.AssignmentStatementAst] -and
            $statement.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
            $statement.Left.VariablePath.UserPath -ceq 'planPath') { $collecting = $true }
        if (-not $collecting) { continue }
        if ($statement -is [System.Management.Automation.Language.IfStatementAst] -and
            $null -ne $statement.Clauses[0].Item1.Find({param($node)
                $node -is [System.Management.Automation.Language.VariableExpressionAst] -and
                $node.VariablePath.UserPath -ceq 'EnforcePerformance'
            }, $true)) { break }
        $initial.Add($statement.Extent.Text) | Out-Null
    }
    $processLoop = $tree.Find({param($node)
        $node -is [System.Management.Automation.Language.ForEachStatementAst] -and
        $node.Variable.VariablePath.UserPath -ceq 'entry' -and
        $null -ne $node.Body.Find({param($child)
            $child -is [System.Management.Automation.Language.CommandAst] -and
            $child.GetCommandName() -ceq 'Invoke-ValidationProcess'
        }, $true)
    }, $true)
    Assert-True ($initial.Count -gt 0 -and $null -ne $processLoop) `
        'existing plan emission and process iteration remain executable caller boundaries'
    if ($initial.Count -eq 0 -or $null -eq $processLoop) { return }
    $final = New-Object 'Collections.Generic.List[string]'
    $collecting = $false
    foreach ($statement in $processLoop.Parent.Statements) {
        if ($statement.Extent.StartOffset -eq $processLoop.Extent.StartOffset) { break }
        if ($statement -is [System.Management.Automation.Language.AssignmentStatementAst] -and
            $statement.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
            $statement.Left.VariablePath.UserPath -ceq 'launcherEquivalence') { $collecting = $true }
        if ($collecting) { $final.Add($statement.Extent.Text) | Out-Null }
    }
    $processAssignment = @($processLoop.Body.Statements | Where-Object {
        $_ -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $null -ne $_.Right.Find({param($node)
            $node -is [System.Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Invoke-ValidationProcess'
        }, $true)
    })
    Assert-True ($final.Count -gt 0 -and $processAssignment.Count -eq 1) `
        'existing final launcher binding and actual child invocation can be isolated without setup'
    if ($final.Count -eq 0 -or $processAssignment.Count -ne 1) { return }
    $initialBlock = [scriptblock]::Create(($initial.ToArray() -join "`n"))
    $finalBlock = [scriptblock]::Create(($final.ToArray() -join "`n"))
    $processBlock = [scriptblock]::Create($processAssignment[0].Extent.Text)

    $outputFull = Join-Path $root 'live-plan-caller-routing'
    New-Item -ItemType Directory -Path $outputFull | Out-Null
    $runtimeFull = $outputFull
    $executableFull = Join-Path $runtimeFull ('never-created-' + [Guid]::NewGuid().ToString('N') + '.exe')
    $planDocument = New-Stage5LiveRoleTestPlan
    foreach ($entry in $planDocument.entries) {
        $configuration = [pscustomobject]@{Mode=$entry.simulationMode;HasWorkerCount=$true;WorkerCount=[int]$entry.requestedWorkers}
        $arguments = @(New-CommonArguments $configuration ('A' * 64)) + @('-runSkirmishAITest4v2', '1729')
        $entryDirectory = Join-Path $outputFull $entry.entryId
        $entry | Add-Member -NotePropertyMembers @{
            arguments=$arguments;command=(ConvertTo-DisplayCommand $executableFull $arguments);timeoutSeconds=1
            stdout=(Join-Path $entryDirectory 'stdout.log');stderr=(Join-Path $entryDirectory 'stderr.log')
            timingDirectory=(Join-Path $entryDirectory 'timing');runtimeLogDirectory=(Join-Path $entryDirectory 'runtime-logs')
        }
    }
    $plan = @($planDocument.entries)
    $launcherContract = [pscustomobject]@{
        executable=[IO.Path]::GetFileName($executableFull);arguments=@()
        launcherPath=(Join-Path $runtimeFull 'launcher.exe');configPath=(Join-Path $runtimeFull 'launcher.lcf')
    }
    $planDocument | Add-Member -NotePropertyMembers @{
        runtimeRoot=$runtimeFull;executable=$executableFull;executableSha256=('A' * 64);title='ZeroHour'
        cohortNonce=$script:TestCohortNonce;cohortCreatedUtc=$script:TestCohortCreatedUtc
        launcherContract=(Assert-LauncherEquivalenceContract $launcherContract $executableFull $runtimeFull $plan)
    }
    $manifestData = [pscustomobject]@{schemaVersion=2;ai=[pscustomobject]@{liveQualification=$planDocument.liveQualification}}
    $PlanOnly = $false; $localCapacityRequested = $false
    $deterministicRuntimeEligible = $false; $acceptanceBindingsRequested = $false
    $livePlanBinding = $null
    . $initialBlock
    Assert-True (-not (Test-Path -LiteralPath $planPath)) `
        'actual V2 execution defers plan publication until launcher/profile finalization'
    $ProfileLeafName = 'FinalizedCallerProfile'
    # The extracted caller consumes metadata, not a physical profile fixture.
    $documentsRoot = 'H:\Stage5SyntheticCallerDocuments'
    . $finalBlock
    Assert-True ($null -ne $livePlanBinding) `
        'top-level finalized V2 execution retains the original frozen binding for every child'
    $originalJson = [IO.File]::ReadAllText($planPath)
    $originalPlan = $originalJson | ConvertFrom-Json
    Assert-True ($originalPlan.launcherContract.profileLeafName -ceq 'FinalizedCallerProfile' -and
        $originalPlan.launcherContract.documentsRoot -ceq $documentsRoot) `
        'the top-level plan records the actual final profile rather than a draft launcher binding'
    Assert-Throws { . $finalBlock } 'exist|create.new|overwrite' `
        'a second top-level finalization cannot overwrite the frozen cohort plan'
    Assert-True ([IO.File]::ReadAllText($planPath) -ceq $originalJson) `
        'refused top-level finalization preserves original frozen bytes'

    # Independent immutable expected values expose an omitted caller argument
    # even before the finalization route starts returning its own binding.
    $values = New-Object 'Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
    $values.Add('planPath', $planPath); $values.Add('planJson', $originalJson)
    $values.Add('planSha256', (Get-Sha256Text $originalJson))
    $livePlanBinding = New-Object 'Collections.ObjectModel.ReadOnlyDictionary[string,string]' -ArgumentList (,$values)
    $entry = ($originalJson | ConvertFrom-Json).entries[1]
    $changed = $originalJson | ConvertFrom-Json
    $changed.entries[1].arguments[0] = '-changed-after-finalization'
    [IO.File]::WriteAllText($planPath, ($changed | ConvertTo-Json -Depth 12))
    $DisableFrameTiming = $true; $validationEnvironment = @{}; $nativeObservationBinding = $null
    Assert-Throws { . $processBlock } 'frozen.*(hash|changed|bytes)|plan.*(hash|changed|bytes)' `
        'the actual top-level child caller passes its original frozen binding instead of omitting it'
    Assert-True (-not (Test-Path -LiteralPath (Split-Path -Parent $entry.stdout)) -and
        -not (Test-Path -LiteralPath $executableFull)) `
        'changed final plan cannot prepare a child or launch the nonexistent fixture executable'
}

function Assert-Stage5SerialBaselineCorpusCaptureContract {
    $runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
    $runnerSource = Get-Content -LiteralPath $runnerPath -Raw
    Assert-True ($runnerSource -match '\[switch\]\$CaptureSerialBaselineCorpus') `
        'runner exposes the explicit serial-baseline corpus-capture switch'
    Assert-True ($runnerSource -match 'serial-baseline-ai') `
        'runner exposes an explicit serial-baseline capture provenance value'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus.*CorpusExportRoot|CorpusExportRoot.*CaptureSerialBaselineCorpus') `
        'serial-baseline capture remains coupled to durable corpus export'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus.*(?:ValidationSet|AI)|(?:ValidationSet|AI).*CaptureSerialBaselineCorpus') `
        'serial-baseline capture is restricted to the AI validation set'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus.*schemaVersion|schemaVersion.*CaptureSerialBaselineCorpus') `
        'serial-baseline capture rejects non-V1 manifests before execution'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus requires an empty replay-fixture list') `
        'serial-baseline capture rejects declared replay fixtures'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus requires the explicit -AllowHeadlessDirectExecution exception') `
        'serial-baseline capture requires the direct-execution exception'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus cannot request performance enforcement') `
        'serial-baseline capture rejects performance enforcement'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus cannot request canonical acceptance bindings') `
        'serial-baseline capture rejects canonical acceptance bindings'
    Assert-True ($runnerSource -match
        'CaptureSerialBaselineCorpus requires at least ten declared AI captures') `
        'serial-baseline capture rejects fewer than ten declared capture records'

    $parseTokens = $null; $parseErrors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$parseTokens, [ref]$parseErrors)
    Assert-True (@($parseErrors).Count -eq 0) `
        'runner parses before the serial-baseline plan is constructed'
    foreach ($definition in $tree.EndBlock.Statements) {
        if ($definition -is [System.Management.Automation.Language.FunctionDefinitionAst]) {
            Invoke-Expression $definition.Extent.Text
        }
    }

    $captureData = [pscustomobject]@{
        schemaVersion = 1
        executableSha256 = ('A' * 64)
        fixtures = @()
        ai = [pscustomobject]@{
            seeds = @(1729, 1730, 1731)
            scenarios = @('4v3', '4v2', 'hard-ai-2v6')
            repeats = 2
        }
    }
    Assert-SerialBaselineCorpusCaptureManifest $captureData
    $tooSmallCaptureData = $captureData | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $tooSmallCaptureData.ai.repeats = 1
    Assert-Throws {
        Assert-SerialBaselineCorpusCaptureManifest $tooSmallCaptureData
    } 'at least ten declared AI captures' `
        'serial-baseline capture rejects a manifest that cannot supply ten replay inputs'
    $selected = @(Get-ValidationWorkerConfigurations `
        -CapacityMode LocalCapacity -DiagnosticWorkerConfiguration '' `
        -CaptureSerialBaselineCorpus)
    Assert-True ($selected.Count -eq 1 -and $selected[0].Id -ceq 'serial-1' -and
        $selected[0].Mode -ceq 'serial' -and
        [int]$selected[0].WorkerCount -eq 1) `
        'serial-baseline capture selects only the existing serial-1 configuration'

    $plan = @(New-ValidationPlan -Data $captureData -Set 'AI' `
        -ReplayPasses 2 -StressRunCount 3 -ReplayTimeout 600 -AiTimeout 1800 `
        -Executable 'H:\Stage5SerialCapture\generalsv.exe' `
        -OutputDirectory 'H:\Stage5SerialCapture\output' `
        -CapacityMode LocalCapacity -DiagnosticWorkerConfiguration '' `
        -CaptureSerialBaselineCorpus)
    $expectedKeys = @(
        foreach ($scenario in $captureData.ai.scenarios) {
            foreach ($seed in $captureData.ai.seeds) {
                foreach ($repeat in 1..$captureData.ai.repeats) {
                    "$scenario|$seed|serial-1|$repeat"
                }
            }
        }
    )
    $actualKeys = @($plan | ForEach-Object {
        "$($_.scenario)|$($_.seed)|$($_.configuration)|$($_.repeat)"
    })
    Assert-True ($plan.Count -eq $expectedKeys.Count -and
        @($plan | Where-Object {
            $_.kind -cne 'ai' -or $_.configuration -cne 'serial-1' -or
            $_.simulationMode -cne 'serial' -or $_.requestedWorkers -cne '1' -or
            $null -ne $_.PSObject.Properties['validationRole']
        }).Count -eq 0 -and
        @($actualKeys | Sort-Object -Unique).Count -eq $expectedKeys.Count -and
        @($expectedKeys | Where-Object { $actualKeys -notcontains $_ }).Count -eq 0) `
        'serial-baseline capture preserves every declared scenario/seed/repeat without a shadow or parallel entry'

    Assert-Throws {
        Get-ValidationWorkerConfigurations -CapacityMode LocalCapacity `
            -DiagnosticWorkerConfiguration 'parallel-4' -CaptureSerialBaselineCorpus | Out-Null
    } 'serial.*baseline|DiagnosticWorkerConfiguration' `
        'serial-baseline capture rejects an explicit diagnostic worker selector'
}

function New-Stage5LiveRoleTestOutput {
    param([object]$Entry, [switch]$UnusedStatus,
        [string]$ExecutableHash = ('A' * 64))
    $arguments = @{
        Seed = $Entry.seed; Mode = $Entry.simulationMode
        RequestedWorkers = $Entry.requestedWorkers
        EffectiveWorkers = [int]$Entry.requestedWorkers
        Scenario = '4v2'; ActualAi = 6; ActualTeams = '4v2'
        ExecutableHash = $ExecutableHash
    }
    if ($Entry.simulationMode -ceq 'shadow') {
        $arguments.AuthoritativeCommits = 0
        $arguments.AiCommittedBatches = 5
        $arguments.ShadowExecutions = 5
        $arguments.CollisionShadowExecutions = 3
        $arguments.PhysicsShadowExecutions = 3
    }
    if ($UnusedStatus) {
        $arguments.StatusAuthoritativeBatches = 0
        $arguments.StatusCommittedCommands = 0
        $arguments.StatusSubmitted = 0
        $arguments.StatusCompleted = 0
        $arguments.StatusPhysicalWorkerJobs = 0
        $arguments.StatusOwnerHelpedJobs = 0
        $arguments.StatusPhysicalWorkerMask = 0
        $arguments.StatusDistinctPhysicalWorkers = 0
        $arguments.StatusPeakConcurrentPhysicalWorkers = 0
    }
    return New-AiCompletionOutput @arguments
}

function Assert-Stage5LiveRoleContract {
    $plan = New-Stage5LiveRoleTestPlan
    $ordinary = $plan.entries[0]
    $authority = $plan.entries[1]
    $executableHash = 'A' * 64

    # A result's observed positivity must not choose its own proof obligation.
    $resolver = Get-Command Resolve-Stage5LiveValidationRequirements -ErrorAction SilentlyContinue
    Assert-True ($null -ne $resolver) `
        'live roles resolve from a versioned predeclared plan rather than scenario inference'
    if ($null -ne $resolver) {
        foreach ($expected in @(
            @{ index = 0; role = 'live-determinism'; profile = 'live-invariants-v1' },
            @{ index = 1; role = 'live-authority-stress'; profile = 'live-all-slices-authority-v1' },
            @{ index = 2; role = 'live-shadow-stress'; profile = 'live-all-slices-shadow-v1' })) {
            try {
                $resolved = @(Resolve-Stage5LiveValidationRequirements -Plan $plan `
                    -Entry $plan.entries[$expected.index])
                Assert-True ($resolved.Count -eq 1 -and
                    $resolved[0].validationRole -ceq $expected.role -and
                    $resolved[0].proofProfileId -ceq $expected.profile -and
                    $resolved[0].requireCompleteSliceSchema) `
                    "the frozen selector resolves exactly one complete-schema $($expected.role) obligation"
            }
            catch { Assert-True $false "live role resolution failed: $($_.Exception.Message)" }
        }
    }

    # The batch guard freezes only scalar plan identity, never mutable plan
    # objects. Exercise its private map with both JSON-dictionary input and a
    # copied entry so every identity field remains checked after the plan is
    # mutated.
    $evidenceModule = @(Get-Module | Where-Object {
        $_.Name -ceq 'DeterministicSimulationEvidence'
    })[0]
    Assert-True ($null -ne $evidenceModule) `
        'live-plan batch requirements map has its evidence module loaded'
    $mapPlanJson = $plan | ConvertTo-Json -Depth 16
    $mapPlan = ConvertFrom-Stage5TestJsonTextDictionary $mapPlanJson
    $mapEntrySnapshot = ConvertFrom-Stage5TestJsonTextDictionary `
        ($mapPlan.entries[0] | ConvertTo-Json -Depth 16)
    $mapBundle = & $evidenceModule {
        param($InputPlan, $InputEntry)
        $map = New-Stage5LiveValidationRequirementsMap -Plan $InputPlan
        $stable = Get-Stage5LiveValidationRequirementsFromMap `
            -RequirementsMap $map -Entry $InputEntry
        $InputPlan.entries[0]['scenario'] = 'mutated-after-freeze'
        $stableAfterPlanMutation =
            Get-Stage5LiveValidationRequirementsFromMap `
                -RequirementsMap $map -Entry $InputEntry
        $recordReadOnly = $true
        try {
            $map[$InputEntry.entryId]['scenario'] = 'mutated-record'
            $recordReadOnly = $false
        }
        catch { }
        [pscustomobject]@{
            map = $map
            stable = $stable
            stableAfterPlanMutation = $stableAfterPlanMutation
            recordReadOnly = $recordReadOnly
        }
    } $mapPlan $mapEntrySnapshot
    Assert-True ($mapBundle.stable.entryId -ceq [string]$mapEntrySnapshot['entryId'] -and
        $mapBundle.stableAfterPlanMutation.entryId -ceq
            [string]$mapEntrySnapshot['entryId'] -and
        $mapBundle.recordReadOnly) `
        'batch requirements map freezes scalar identity independently of the mutable source plan'
    $coveragePlan = ConvertFrom-Stage5TestJsonTextDictionary $mapPlanJson
    $coverageResult = & $evidenceModule {
        param($InputPlan)
        $map = New-Stage5LiveValidationRequirementsMap -Plan $InputPlan
        [string[]]$requiredIds = @($map.Keys | ForEach-Object { [string]$_ })
        $seen = @{}
        foreach ($entryId in @($requiredIds | Select-Object -Skip 1)) {
            $seen[$entryId] = $true
        }
        $InputPlan.entries[0]['entryId'] = [string]$requiredIds[1]
        $rejected = $false
        try {
            Assert-Stage5PlannedLiveEntryCoverage `
                -RequiredEntryIds $requiredIds -Seen $seen `
                -Context 'batch frozen-entry coverage regression'
        }
        catch { $rejected = $true }
        [pscustomobject]@{ rejected = $rejected }
    } $coveragePlan
    Assert-True $coverageResult.rejected `
        'batch coverage rejects a missing original entry despite a mutated caller ID matching a seen entry'
    foreach ($typeMutation in @(
        @{ field = 'entryId'; value = @('ai-type-array') },
        @{ field = 'kind'; value = @('ai') },
        @{ field = 'sequence'; value = @([int]$mapEntrySnapshot['sequence']) },
        @{ field = 'scenario'; value = @('4v2') },
        @{ field = 'seed'; value = @([int]$mapEntrySnapshot['seed']) },
        @{ field = 'configuration'; value = [pscustomobject]@{ value = 'parallel-1' } },
        @{ field = 'repeat'; value = [decimal]$mapEntrySnapshot['repeat'] },
        @{ field = 'simulationMode'; value = @('parallel') },
        @{ field = 'requestedWorkers'; value = @('2') },
        @{ field = 'workerPolicy'; value = [pscustomobject]@{ value = 'auto' } },
        @{ field = 'stress'; value = 'true' },
        @{ field = 'validationRole'; value = @('live-determinism') },
        @{ field = 'proofProfileId'; value = @('live-invariants-v1') })) {
        $badTypePlan = ConvertFrom-Stage5TestJsonTextDictionary $mapPlanJson
        $badTypePlan.entries[0][$typeMutation.field] = $typeMutation.value
        Assert-Throws {
            & $evidenceModule {
                param($InputPlan)
                New-Stage5LiveValidationRequirementsMap -Plan $InputPlan | Out-Null
            } $badTypePlan
        } 'scalar|integer|boolean' `
            "batch map rejects non-scalar identity field '$($typeMutation.field)'"
    }
    $batchIdentityFields = @('entryId', 'kind', 'sequence', 'scenario', 'seed',
        'configuration', 'repeat', 'simulationMode', 'requestedWorkers',
        'workerPolicy', 'stress', 'validationRole', 'proofProfileId')
    foreach ($identityField in $batchIdentityFields) {
        $changedEntry = ConvertFrom-Stage5TestJsonTextDictionary `
            ($mapEntrySnapshot | ConvertTo-Json -Depth 16)
        $originalValue = $changedEntry[$identityField]
        $changedEntry[$identityField] = if ($originalValue -is [bool]) {
            -not $originalValue
        }
        elseif ($originalValue -is [int] -or $originalValue -is [int64]) {
            [int64]$originalValue + 1
        }
        else { [string]$originalValue + '-tampered' }
        Assert-Throws {
            & $evidenceModule {
                param($RequirementsMap, $Entry)
                Get-Stage5LiveValidationRequirementsFromMap `
                    -RequirementsMap $RequirementsMap -Entry $Entry | Out-Null
            } $mapBundle.map $changedEntry
        } 'identity|member|frozen' `
            "batch map rejects changed identity field '$identityField'"
        $missingEntry = ConvertFrom-Stage5TestJsonTextDictionary `
            ($mapEntrySnapshot | ConvertTo-Json -Depth 16)
        [void]$missingEntry.Remove($identityField)
        Assert-Throws {
            & $evidenceModule {
                param($RequirementsMap, $Entry)
                Get-Stage5LiveValidationRequirementsFromMap `
                    -RequirementsMap $RequirementsMap -Entry $Entry | Out-Null
            } $mapBundle.map $missingEntry
        } 'missing property' `
            "batch map rejects missing identity field '$identityField'"
    }
    $unknownEntry = ConvertFrom-Stage5TestJsonTextDictionary `
        ($mapEntrySnapshot | ConvertTo-Json -Depth 16)
    $unknownEntry['entryId'] = 'ai-unselected'
    Assert-Throws {
        & $evidenceModule {
            param($RequirementsMap, $Entry)
            Get-Stage5LiveValidationRequirementsFromMap `
                -RequirementsMap $RequirementsMap -Entry $Entry | Out-Null
        } $mapBundle.map $unknownEntry
    } 'member|matrix' 'batch map rejects an unselected entry identity'
    $duplicatePlan = ConvertFrom-Stage5TestJsonTextDictionary $mapPlanJson
    $duplicatePlan['entries'] = @($duplicatePlan.entries) +
        @($duplicatePlan.entries[0])
    Assert-Throws {
        & $evidenceModule {
            param($InputPlan)
            New-Stage5LiveValidationRequirementsMap -Plan $InputPlan | Out-Null
        } $duplicatePlan
    } 'duplicate AI entry identity' 'batch map rejects duplicate planned identities'
    $badRolePlan = ConvertFrom-Stage5TestJsonTextDictionary $mapPlanJson
    $badRolePlan.entries[1]['validationRole'] = 'live-selected-if-positive'
    Assert-Throws {
        & $evidenceModule {
            param($InputPlan)
            New-Stage5LiveValidationRequirementsMap -Plan $InputPlan | Out-Null
        } $badRolePlan
    } 'role|profile' 'batch map rejects an unregistered role/profile mutation'

    # These assertions exercise the real parser, independently of the resolver's existence.
    foreach ($entry in $plan.entries) {
        $output = New-Stage5LiveRoleTestOutput $entry
        try {
            $evidence = @(ConvertFrom-Stage5AiCompletion -Output $output -Entry $entry `
                -ExecutableHash $executableHash -ValidationPlan $plan)
            $expectedProof = if ($entry.validationRole -ceq 'live-determinism') {
                'not-required'
            } else { 'validated' }
            Assert-True ($evidence.Count -eq 1 -and
                $evidence[0].PSObject.Properties.Name -contains 'schemaStatus' -and
                $evidence[0].PSObject.Properties.Name -contains 'invariantStatus' -and
                $evidence[0].PSObject.Properties.Name -contains 'capabilityProofStatus' -and
                $evidence[0].PSObject.Properties.Name -contains 'validationRole' -and
                $evidence[0].PSObject.Properties.Name -contains 'proofProfileId' -and
                $evidence[0].schemaStatus -ceq 'complete' -and
                $evidence[0].invariantStatus -ceq 'validated' -and
                $evidence[0].capabilityProofStatus -ceq $expectedProof -and
                $evidence[0].validationRole -ceq $entry.validationRole -and
                $evidence[0].proofProfileId -ceq $entry.proofProfileId) `
                "complete $($entry.validationRole) evidence distinguishes validity from capability proof"
        }
        catch { Assert-True $false "complete $($entry.validationRole) evidence failed: $($_.Exception.Message)" }

        $invariantMutations = @(
            @{ field = 'job_failed'; value = '1'; pattern = 'failed' },
            @{ field = 'job_cancelled'; value = '1'; pattern = 'cancel' },
            @{ field = 'direct_stale_acceptance'; value = '1'; pattern = 'direct|stale' },
            @{ field = 'collision_physical_worker_mask'; value = '1'; pattern = 'collision.*(mask|identity)' },
            @{ field = 'collision_unexpected_fallbacks'; value = '1'; pattern = 'collision.*fallback' },
            @{ field = 'status_stale_rejections'; value = '1'; pattern = 'status.*stale' })
        if ($entry.simulationMode -ceq 'parallel') {
            # These zero guards currently share a conditional with positive proof.
            $invariantMutations += @(
                @{ field = 'collision_owner_fallbacks'; value = '1'; pattern = 'collision' },
                @{ field = 'collision_stale_rejections'; value = '1'; pattern = 'collision' },
                @{ field = 'spatial_healing_expected_fallbacks'; value = '1'; pattern = 'healing|spatial' })
        }
        foreach ($mutation in $invariantMutations) {
            $fieldPattern = '(?<=\s)' + [regex]::Escape($mutation.field) + '=\d+(?=\s|$)'
            $mutated = [regex]::Replace($output, $fieldPattern,
                ($mutation.field + '=' + $mutation.value))
            Assert-True ($mutated -cne $output) `
                "the $($mutation.field) negative mutates the complete producer fixture"
            Assert-Throws {
                ConvertFrom-Stage5AiCompletion -Output $mutated -Entry $entry `
                    -ExecutableHash $executableHash -ValidationPlan $plan | Out-Null
            } $mutation.pattern `
                "$($entry.validationRole) retains the $($mutation.field) invariant"
        }
    }

    $unusedStatusOutput = New-Stage5LiveRoleTestOutput $ordinary -UnusedStatus
    try {
        $unused = ConvertFrom-Stage5AiCompletion -Output $unusedStatusOutput -Entry $ordinary `
            -ExecutableHash $executableHash -ValidationPlan $plan
        Assert-True ($unused.schemaStatus -ceq 'complete' -and
            $unused.invariantStatus -ceq 'validated' -and
            $unused.capabilityProofStatus -ceq 'not-required' -and
            [UInt64]$unused.fields.status_authoritative_batches -eq 0 -and
            [UInt64]$unused.fields.status_submitted_jobs -eq 0 -and
            $unused.line -ceq $unusedStatusOutput) `
            'an ordinary complete 4v2 run retains a genuinely unused status slice without claiming capability'
    }
    catch { Assert-True $false "ordinary unused-status evidence failed: $($_.Exception.Message)" }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion -Output $unusedStatusOutput -Entry $authority `
            -ExecutableHash $executableHash -ValidationPlan $plan | Out-Null
    } 'status|capability' 'the identical unused status slice fails the designated full authority profile'

    foreach ($entry in @($ordinary, $authority)) {
        $missingWork = New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
            -ActualTeams '4v2' -OmitWorkEvidence
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion -Output $missingWork -Entry $entry `
                -ExecutableHash $executableHash -RequireAuthoritativeWorkEvidence $false `
                -ValidationPlan $plan | Out-Null
        } 'schema|work|evidence' "$($entry.validationRole) cannot waive the closed slice schema with the legacy presence flag"
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion -Output (New-Stage5LiveRoleTestOutput $authority) `
            -Entry $authority -ExecutableHash $executableHash | Out-Null
    } 'plan|role' 'a V2 role-labelled entry cannot silently fall back to V1 without its plan'

    # A malformed plan is rejected even when all of the observed counters are positive.
    foreach ($mutation in @(
        @{ name = 'a selector outside the matrix'; edit = { param($p) $p.liveQualification.authorityEntries[0].seed = 1730 } },
        @{ name = 'a duplicate selector'; edit = { param($p) $p.liveQualification.authorityEntries += $p.liveQualification.authorityEntries[0] } },
        @{ name = 'an empty authority selection'; edit = { param($p) $p.liveQualification.authorityEntries = @() } },
        @{ name = 'an unknown role'; edit = { param($p) $p.entries[1].validationRole = 'live-selected-if-positive' } },
        @{ name = 'an unknown profile'; edit = { param($p) $p.liveQualification.profileSetId = 'live-without-status' } },
        @{ name = 'a downgraded selected entry'; edit = {
            param($p)
            $p.entries[1].validationRole = 'live-determinism'
            $p.entries[1].proofProfileId = 'live-invariants-v1'
        } })) {
        $badPlan = New-Stage5LiveRoleTestPlan
        & $mutation.edit $badPlan
        if ($null -ne $resolver) {
            Assert-Throws {
                Resolve-Stage5LiveValidationRequirements -Plan $badPlan `
                    -Entry $badPlan.entries[1] | Out-Null
            } 'plan|selector|role|profile|authority' "the role resolver rejects $($mutation.name)"
        }
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion -Output (New-Stage5LiveRoleTestOutput $badPlan.entries[1]) `
                -Entry $badPlan.entries[1] -ExecutableHash $executableHash `
                -ValidationPlan $badPlan | Out-Null
        } 'plan|selector|role|profile|authority' "positive counters cannot rescue $($mutation.name)"
    }

    # Preserve explicit V1 semantics: no new role is inferred for old callers.
    $legacyEntry = [pscustomobject]@{
        sequence = 14; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v2'; stress = $true
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion $unusedStatusOutput $legacyEntry $executableHash | Out-Null
    } 'status' 'an unversioned legacy entry retains the original strict positive-status semantics'

    try {
        $results = @()
        foreach ($entry in $plan.entries) {
            $evidence = ConvertFrom-Stage5AiCompletion -Output (New-Stage5LiveRoleTestOutput $entry) `
                -Entry $entry -ExecutableHash $executableHash -ValidationPlan $plan
            $result = $entry | ConvertTo-Json -Depth 12 | ConvertFrom-Json
            $result | Add-Member -MemberType NoteProperty -Name aiEvidence -Value $evidence
            $results += $result
        }
        Assert-Stage5AuthoritativeWorkEvidence -Results $results -ValidationPlan $plan

        # Removing outer labels must not route the parser's nested V2 evidence
        # through the legacy favorable-result search without its original plan.
        $nestedRoleResults = $results | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        foreach ($nestedRoleResult in $nestedRoleResults) {
            $nestedRoleResult.PSObject.Properties.Remove('validationRole')
            $nestedRoleResult.PSObject.Properties.Remove('proofProfileId')
        }
        Assert-Throws {
            Assert-Stage5AuthoritativeWorkEvidence -Results $nestedRoleResults
        } 'V2|role|profile|plan' `
            'nested V2 role evidence cannot enter the no-plan legacy aggregate after outer labels are removed'

        # Real V1 parser output retains null metadata fields. Reject advertised
        # nested labels, not the mere presence of those backwards-compatible keys.
        $legacyResults = $nestedRoleResults | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        foreach ($legacyResult in $legacyResults) {
            $legacyResult.aiEvidence = ConvertFrom-Stage5AiCompletion `
                -Output $legacyResult.aiEvidence.line -Entry $legacyResult `
                -ExecutableHash $executableHash
            foreach ($field in @('validationRole', 'proofProfileId')) {
                Assert-True ($legacyResult.aiEvidence.PSObject.Properties.Name -contains $field -and
                    $null -eq $legacyResult.aiEvidence.$field) `
                    "the legacy parser retains a null nested $field without advertising a V2 obligation"
            }
        }
        Assert-Stage5AuthoritativeWorkEvidence -Results $legacyResults
        foreach ($metadata in @(
            @{ field = 'validationRole'; value = 'live-determinism' },
            @{ field = 'proofProfileId'; value = 'live-all-slices-authority-v1' })) {
            $partialRoleResults = $legacyResults | ConvertTo-Json -Depth 12 | ConvertFrom-Json
            $field = $metadata.field
            $partialRoleResults[0].aiEvidence.$field = $metadata.value
            Assert-Throws {
                Assert-Stage5AuthoritativeWorkEvidence -Results $partialRoleResults
            } 'V2|role|profile|plan' `
                "a non-null nested $field alone requires the original V2 plan"
        }

        # Each raw shadow outcome is independently valid and fully reparsable,
        # but differs from its regular case. Leave cached outcomes unchanged so
        # a comparison of cached aiEvidence properties cannot satisfy this test.
        foreach ($outcome in @(
            @{ field = 'final_digest'; value = 'DEADBEEF' },
            @{ field = 'end_frame'; value = '42001' },
            @{ field = 'winner_team'; value = '2' })) {
            $divergentShadowResults = $results | ConvertTo-Json -Depth 12 | ConvertFrom-Json
            $field = $outcome.field
            $originalLine = $divergentShadowResults[2].aiEvidence.line
            $divergentShadowResults[2].aiEvidence.line = [regex]::Replace($originalLine,
                ('(?<=\s)' + [regex]::Escape($field) + '=\S+(?=\s|$)'),
                ($field + '=' + $outcome.value))
            $divergentShadowResults[2].aiEvidence.fields.$field = $outcome.value
            Assert-True ($divergentShadowResults[2].aiEvidence.line -cne $originalLine -and
                $divergentShadowResults[2].aiEvidence.finalDigest -ceq 'A1B2C3D4' -and
                $divergentShadowResults[2].aiEvidence.endFrame -eq 42000 -and
                $divergentShadowResults[2].aiEvidence.winnerTeam -eq 1) `
                "the shadow $field negative changes raw evidence while preserving the misleading cached outcome"
            $reparsedShadow = ConvertFrom-Stage5AiCompletion `
                -Output $divergentShadowResults[2].aiEvidence.line -Entry $plan.entries[2] `
                -ExecutableHash $executableHash -ValidationPlan $plan
            Assert-True ($reparsedShadow.fields[$field] -ceq $outcome.value -and
                $reparsedShadow.capabilityProofStatus -ceq 'validated') `
                "the changed shadow $field remains individually valid before the outcome comparison"
            Assert-Throws {
                Assert-Stage5AuthoritativeWorkEvidence -Results $divergentShadowResults `
                    -ValidationPlan $plan
            } 'shadow.*(outcome|reference)|regular.*outcome' `
                "planned shadow $field must match the independently reparsed regular case outcome"
        }

        # An identical cached determinismKey is not a scenario/seed reference.
        # In the second case only a different scenario has the shadow's seed.
        foreach ($missingReference in @('seed', 'scenario')) {
            $referencePlan = New-Stage5LiveRoleTestPlan
            $referencePlan.entries[1].seed = 1730
            $referencePlan.liveQualification.authorityEntries[0].seed = 1730
            if ($missingReference -ceq 'seed') {
                $referencePlan.entries[0].seed = 1730
            }
            else {
                $referencePlan.entries[0].scenario = '4v3'
                $referencePlan.entries[0].stress = $false
            }
            $referenceResults = @()
            foreach ($referenceEntry in $referencePlan.entries) {
                $referenceOutput = if ($referenceEntry.scenario -ceq '4v3') {
                    New-AiCompletionOutput -Seed 1729 -Scenario '4v3' -ActualAi 7 -ActualTeams '4v3'
                }
                else { New-Stage5LiveRoleTestOutput $referenceEntry }
                $referenceEvidence = ConvertFrom-Stage5AiCompletion -Output $referenceOutput `
                    -Entry $referenceEntry -ExecutableHash $executableHash -ValidationPlan $referencePlan
                $referenceResult = $referenceEntry | ConvertTo-Json -Depth 12 | ConvertFrom-Json
                $referenceResult | Add-Member -MemberType NoteProperty -Name aiEvidence -Value $referenceEvidence
                $referenceResults += $referenceResult
            }
            Assert-True (@($referenceResults | Where-Object {
                $_.determinismKey -ceq '4v2-seed-1729'
            }).Count -eq 3) 'the missing-reference fixture retains a misleading shared cached determinism key'
            Assert-Throws {
                Assert-Stage5AuthoritativeWorkEvidence -Results $referenceResults -ValidationPlan $referencePlan
            } 'shadow.*reference|regular.*reference' `
                "planned shadow requires a regular reference for its exact $missingReference despite matching cached keys"
        }

        Assert-Throws {
            Assert-Stage5AuthoritativeWorkEvidence -Results @($results[0], $results[2]) `
                -ValidationPlan $plan
        } 'selected|required|missing|authority|entry' `
            'a positive unselected result cannot rescue an absent designated authority entry'

        $zeroSelected = $results | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        foreach ($field in @('status_authoritative_batches', 'status_committed_commands',
            'status_submitted_jobs', 'status_completed_jobs', 'status_physical_worker_jobs',
            'status_owner_helped_jobs', 'status_physical_worker_mask',
            'status_distinct_physical_workers', 'status_peak_concurrent_physical_workers')) {
            $zeroSelected[1].aiEvidence.fields.$field = '0'
        }
        $zeroSelected[1].aiEvidence.line = $unusedStatusOutput
        Assert-Throws {
            Assert-Stage5AuthoritativeWorkEvidence -Results $zeroSelected -ValidationPlan $plan
        } 'status|capability|authority' `
            'the selected same-result profile cannot borrow positive status from an unselected run'

        $allSelectedPlan = New-Stage5LiveRoleTestPlan
        $allSelectedPlan.liveQualification.authorityEntries += [pscustomobject]@{
            scenario = '4v2'; seed = 1729; configuration = 'parallel-2'; repeat = 1
        }
        $allSelectedPlan.entries[0].validationRole = 'live-authority-stress'
        $allSelectedPlan.entries[0].proofProfileId = 'live-all-slices-authority-v1'
        $zeroSelected[0].validationRole = 'live-authority-stress'
        $zeroSelected[0].proofProfileId = 'live-all-slices-authority-v1'
        $zeroSelected[0].aiEvidence = ConvertFrom-Stage5AiCompletion `
            -Output (New-Stage5LiveRoleTestOutput $allSelectedPlan.entries[0]) `
            -Entry $allSelectedPlan.entries[0] -ExecutableHash $executableHash `
            -ValidationPlan $allSelectedPlan
        Assert-Throws {
            Assert-Stage5AuthoritativeWorkEvidence -Results $zeroSelected `
                -ValidationPlan $allSelectedPlan
        } 'status|capability|authority' `
            'every preselected authority entry must pass rather than choosing one positive selected run'
    }
    catch { Assert-True $false "live-role aggregate fixture failed: $($_.Exception.Message)" }

    $localPlan = New-Stage5LiveRoleTestPlan
    $localPlan.liveQualification.shadowEntry.configuration = 'shadow-8'
    $localPlan.entries[2].configuration = 'shadow-8'
    $localPlan.entries[2].requestedWorkers = '8'
    $localShadow = $localPlan.entries[2]
    $localOutput = New-Stage5LiveRoleTestOutput $localShadow
    try {
        $localEvidence = ConvertFrom-Stage5AiCompletion -Output $localOutput -Entry $localShadow `
            -ExecutableHash $executableHash -ValidationPlan $localPlan
        Assert-True ($localEvidence.schemaStatus -ceq 'complete' -and
            $localEvidence.invariantStatus -ceq 'validated' -and
            $localEvidence.validationRole -ceq 'live-shadow-stress' -and
            $localEvidence.capabilityProofStatus -ceq 'validated' -and
            $localEvidence.fields.requested_workers -ceq '8' -and
            [UInt64]$localEvidence.fields.effective_workers -eq 8 -and
            $localEvidence.ordinaryPathShadowComparisons -gt 0 -and
            $localEvidence.spatialEvidence.successfulCollections -gt 0) `
            'the designated V2 local shadow-8 profile proves strict shadow work without relabelling its workers'
    }
    catch { Assert-True $false "complete local shadow-8 evidence failed: $($_.Exception.Message)" }

    $noLocalCollection = $localOutput
    foreach ($field in @('spatial_successful_collections', 'spatial_successful_collection_queries',
        'spatial_successful_collection_ranges', 'spatial_multi_range_collections',
        'spatial_collection_submitted_jobs', 'spatial_collection_completed_jobs',
        'spatial_collection_physical_worker_jobs', 'spatial_collection_owner_helped_jobs',
        'spatial_collection_physical_worker_mask', 'spatial_maximum_collection_queries',
        'spatial_maximum_collection_ranges', 'spatial_maximum_collection_distinct_physical_workers')) {
        $noLocalCollection = [regex]::Replace($noLocalCollection,
            ('(?<=\s)' + [regex]::Escape($field) + '=\d+(?=\s|$)'), ($field + '=0'))
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion -Output $noLocalCollection -Entry $localShadow `
            -ExecutableHash $executableHash -ValidationPlan $localPlan | Out-Null
    } 'positive.*immutable-spatial collection' `
        'local shadow-8 rejects zero multi-query collection proof even when consumer shadow counters are positive'

    $noLocalOrdinaryComparison = [regex]::Replace($localOutput,
        '(?<=\s)ordinary_path_shadow_comparisons=\d+(?=\s|$)', 'ordinary_path_shadow_comparisons=0')
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion -Output $noLocalOrdinaryComparison -Entry $localShadow `
            -ExecutableHash $executableHash -ValidationPlan $localPlan | Out-Null
    } 'shadow.*ordinary-path comparison' `
        'local shadow-8 requires its own positive physical-worker ordinary-path comparison'

    $localShadowWithFallback = [regex]::Replace($localOutput,
        '(?<=\s)job_fallback=\d+(?=\s|$)', 'job_fallback=1')
    $localShadowWithFallback = [regex]::Replace($localShadowWithFallback,
        '(?<=\s)owner_fallbacks=\d+(?=\s|$)', 'owner_fallbacks=1')
    $localShadowWithFallback = [regex]::Replace($localShadowWithFallback,
        '(?<=\s)ai_serial_fallbacks=\d+(?=\s|$)', 'ai_serial_fallbacks=1')
    try {
        ConvertFrom-Stage5AiCompletion -Output $localShadowWithFallback `
            -Entry $localShadow -ExecutableHash $executableHash `
            -ValidationPlan $localPlan | Out-Null
        Assert-True $true `
            'local shadow-8 accepts an owner-safe AI fallback accounted by the global scheduler'
    }
    catch {
        Assert-True $false `
            "local shadow-8 rejected accounted policy fallback: $($_.Exception.Message)"
    }

    foreach ($mutation in @(
        @{ field = 'effective_workers'; value = '7'; pattern = '8 effective workers|effective worker count' },
        @{ field = 'job_peak_active_workers'; value = '0'; pattern = 'shadow stress.*worker jobs' },
        @{ field = 'job_failed'; value = '1'; pattern = 'failed jobs' },
        @{ field = 'job_cancelled'; value = '1'; pattern = 'cancelled jobs' },
        @{ field = 'requested_workers'; value = '16'; pattern = 'requested worker count does not match' })) {
        $mutated = [regex]::Replace($localOutput,
            ('(?<=\s)' + [regex]::Escape($mutation.field) + '=\d+(?=\s|$)'),
            ($mutation.field + '=' + $mutation.value))
        Assert-True ($mutated -cne $localOutput) `
            "the local shadow-8 $($mutation.field) negative changes the producer fixture"
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion -Output $mutated -Entry $localShadow `
                -ExecutableHash $executableHash -ValidationPlan $localPlan | Out-Null
        } $mutation.pattern "local shadow-8 retains the strict $($mutation.field) scheduler contract"
    }
    $legacyLocalShadow = [pscustomobject]@{
        sequence = 15; scenario = '4v2'; seed = 1729; configuration = 'shadow-8'
        simulationMode = 'shadow'; requestedWorkers = '8'; stress = $true
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion $localOutput $legacyLocalShadow $executableHash | Out-Null
    } 'unsupported worker configuration' 'V1 shadow-8 does not silently acquire the new V2 role contract'
}

function Assert-Stage5HardAi2v6CompletionContract {
    $executableHash = 'A' * 64
    $plan = New-Stage5LiveRoleTestPlan | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $hardEntry = [pscustomobject]@{
        sequence = 14; entryId = 'ai-0014'; kind = 'ai'; stress = $true
        scenario = 'hard-ai-2v6'; seed = 1729; configuration = 'parallel-2'; repeat = 1
        simulationMode = 'parallel'; requestedWorkers = '2'; workerPolicy = 'auto'
        determinismKey = 'hard-ai-2v6-seed-1729'
        validationRole = 'live-determinism'; proofProfileId = 'live-invariants-v1'
    }
    $plan.entries += $hardEntry

    try {
        $requirements = Resolve-Stage5LiveValidationRequirements -Plan $plan -Entry $hardEntry
        Assert-True ($requirements.validationRole -ceq 'live-determinism' -and
            $requirements.proofProfileId -ceq 'live-invariants-v1' -and
            $requirements.requireCompleteSliceSchema) `
            'hard-ai-2v6 resolves to the ordinary live-determinism role without widening 4v2 qualification'
    }
    catch { Assert-True $false "hard-ai-2v6 ordinary role resolution failed: $($_.Exception.Message)" }

    $hardOutput = New-AiCompletionOutput -Seed 1729 -Scenario 'hard-ai-2v6' `
        -ActualAi 8 -ActualTeams '2v6' -Mode parallel -RequestedWorkers '2' `
        -EffectiveWorkers 2 -SpatialCapturedArenas 0 `
        -SpatialSuccessfulCollections 0 -SpatialSuccessfulCollectionQueries 0 `
        -SpatialSuccessfulCollectionRanges 0 -SpatialMultiRangeCollections 0 `
        -SpatialCollectionSubmitted 0 -SpatialCollectionCompleted 0 `
        -SpatialCollectionPhysical 0 -SpatialCollectionPhysicalWorkerMask 0 `
        -SpatialMaximumCollectionQueries 0 -SpatialMaximumCollectionRanges 0 `
        -SpatialMaximumCollectionDistinctPhysicalWorkers 0 `
        -SpatialHealingEligible 0 -SpatialHealingAuthoritative 0 `
        -SpatialHealingCandidates 0 -SpatialHealingSubmitted 0 `
        -SpatialHealingCompleted 0 -SpatialHealingPhysical 0 `
        -SpatialHealingExpectedFallbacks 0 -SpatialPdlEligible 0 `
        -SpatialPdlAuthoritative 0 -SpatialPdlCandidates 0 `
        -SpatialPdlSubmitted 0 -SpatialPdlCompleted 0 -SpatialPdlPhysical 0 `
        -SpatialPdlExpectedFallbacks 0
    try {
        $hardEvidence = ConvertFrom-Stage5AiCompletion -Output $hardOutput `
            -Entry $hardEntry -ExecutableHash $executableHash -ValidationPlan $plan
        Assert-True ($hardEvidence.fields.scenario -ceq 'hard-ai-2v6' -and
            [UInt64]$hardEvidence.fields.actual_ai -eq 8 -and
            $hardEvidence.fields.actual_teams -ceq '2v6' -and
            $hardEvidence.validationRole -ceq 'live-determinism' -and
            $hardEvidence.proofProfileId -ceq 'live-invariants-v1') `
            'truthful hard-ai-2v6 completion preserves native scenario, 8-AI count, 2v6 teams, and ordinary role'
    }
    catch { Assert-True $false "truthful hard-ai-2v6 completion was rejected: $($_.Exception.Message)" }

    $legacyEntry = [pscustomobject]@{
        sequence = 15; scenario = 'hard-ai-2v6'; seed = 1729; configuration = 'parallel-2'
        simulationMode = 'parallel'; requestedWorkers = '2'; stress = $true
    }
    try {
        $legacyEvidence = ConvertFrom-Stage5AiCompletion $hardOutput $legacyEntry $executableHash
        Assert-True ($legacyEvidence.fields.scenario -ceq 'hard-ai-2v6' -and
            [UInt64]$legacyEvidence.fields.actual_ai -eq 8 -and
            $legacyEvidence.fields.actual_teams -ceq '2v6' -and
            $null -eq $legacyEvidence.validationRole -and
            $null -eq $legacyEvidence.proofProfileId) `
            'legacy hard-ai-2v6 completion accepts the native 8-AI/2v6 shape without advertising a V2 role'
    }
    catch { Assert-True $false "legacy hard-ai-2v6 completion was rejected: $($_.Exception.Message)" }

    foreach ($mutation in @(
        @{ name = 'actual AI count'; replacement = 'actual_ai=7'; pattern = 'actual_ai' },
        @{ name = 'actual team shape'; replacement = 'actual_teams=hard-ai-2v6'; pattern = 'actual_teams' },
        @{ name = 'scenario identity'; replacement = 'scenario=4v2'; pattern = 'scenario' }
    )) {
        $mutated = switch ($mutation.name) {
            'actual AI count' { $hardOutput -replace 'actual_ai=8', $mutation.replacement }
            'actual team shape' { $hardOutput -replace 'actual_teams=2v6', $mutation.replacement }
            default { $hardOutput -replace 'scenario=hard-ai-2v6', $mutation.replacement }
        }
        Assert-True ($mutated -cne $hardOutput) `
            "hard-ai-2v6 $($mutation.name) negative mutates the producer fixture"
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion -Output $mutated -Entry $hardEntry `
                -ExecutableHash $executableHash -ValidationPlan $plan | Out-Null
        } $mutation.pattern "hard-ai-2v6 rejects a mismatched $($mutation.name)"
    }

    $hardAuthorityPlan = $plan | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $hardAuthorityPlan.entries[3].repeat = 2
    $hardAuthorityPlan.liveQualification.authorityEntries[0].scenario = 'hard-ai-2v6'
    Assert-Throws {
        Resolve-Stage5LiveValidationRequirements -Plan $hardAuthorityPlan `
            -Entry $hardAuthorityPlan.entries[3] | Out-Null
    } '4v2|authority|selector' `
        'hard-ai-2v6 cannot become an authority selector for the explicit 4v2 all-slices profile'

    $hardShadowPlan = $plan | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $hardShadowPlan.liveQualification.shadowEntry.scenario = 'hard-ai-2v6'
    Assert-Throws {
        Resolve-Stage5LiveValidationRequirements -Plan $hardShadowPlan `
            -Entry $hardShadowPlan.entries[3] | Out-Null
    } '4v2|shadow|selector' `
        'hard-ai-2v6 cannot become the shadow selector for the explicit 4v2 all-slices profile'

    $hardStressPlan = $plan | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $hardStressPlan.entries[3].stress = $false
    Assert-Throws {
        Resolve-Stage5LiveValidationRequirements -Plan $hardStressPlan `
            -Entry $hardStressPlan.entries[3] | Out-Null
    } 'stress|policy' 'hard-ai-2v6 remains a bounded stress-only ordinary entry'

    $legacy4v2Entry = [pscustomobject]@{
        sequence = 20; scenario = '4v2'; seed = 1729; configuration = 'parallel-2'
        simulationMode = 'parallel'; requestedWorkers = '2'; stress = $true
    }
    $legacy4v2Evidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2') `
        $legacy4v2Entry $executableHash
    $legacyShadowEntry = [pscustomobject]@{
        sequence = 21; scenario = '4v2'; seed = 1729; configuration = 'shadow-16'
        simulationMode = 'shadow'; requestedWorkers = '16'; stress = $true
    }
    $legacyShadowEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Mode 'shadow' -RequestedWorkers '16' `
            -EffectiveWorkers 16 -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -AuthoritativeCommits 0 -AiCommittedBatches 5 -ShadowExecutions 5 `
            -CollisionAuthoritativeCommits 0 -CollisionShadowExecutions 3 `
            -PhysicsShadowExecutions 3 -PathWorkerExecuted 0 `
            -PathAuthoritativeCommits 0) `
        $legacyShadowEntry $executableHash
    $legacy4v2Result = [pscustomobject]@{
        sequence = 20; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'parallel-2'
        aiEvidence = $legacy4v2Evidence
    }
    $legacyShadowResult = [pscustomobject]@{
        sequence = 21; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'shadow-16'
        aiEvidence = $legacyShadowEvidence
    }
    $hardLegacyResult = [pscustomobject]@{
        sequence = 22; kind = 'ai'; stress = $true; scenario = 'hard-ai-2v6'; configuration = 'parallel-2'
        aiEvidence = $null
    }
    try {
        Assert-Stage5AuthoritativeWorkEvidence @($legacy4v2Result, $legacyShadowResult)
        Assert-Stage5AuthoritativeWorkEvidence @($legacy4v2Result, $legacyShadowResult, $hardLegacyResult)
        Assert-True $true `
            'legacy all-slices qualification remains bound to 4v2 when a hard-ai-2v6 result is present'
    }
    catch { Assert-True $false "legacy 4v2 qualification was not isolated from hard-ai-2v6: $($_.Exception.Message)" }

    # A hard-ai-2v6 shadow may carry truthful shadow counters, but it is not
    # the installed 4v2 qualification selector.  Keep this negative fixture
    # deliberately complete for every shadow slice so a selector that omits
    # scenario identity cannot pass by accident.
    $hardOnlyShadowResult = [pscustomobject]@{
        sequence = 23; kind = 'ai'; stress = $true; scenario = 'hard-ai-2v6'
        configuration = 'shadow-16'; aiEvidence = $legacyShadowEvidence
    }
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @($legacy4v2Result, $hardOnlyShadowResult)
    } 'installed shadow|matching shadow|4v2' `
        'hard-ai-2v6 shadow evidence cannot satisfy the installed 4v2 shadow selector'
}

function New-AiResult {
    param([string]$Configuration, [int]$Repeat, [string]$Digest = 'A1B2C3D4',
        [int]$EndFrame = 42000, [int]$Winner = 1,
        [string]$DeterminismKey = '4v3-seed-1729')
    return [pscustomobject]@{
        kind = 'ai'; determinismKey = $DeterminismKey; configuration = $Configuration; repeat = $Repeat
        aiEvidence = [pscustomobject]@{ finalDigest = $Digest; endFrame = $EndFrame; winnerTeam = $Winner }
    }
}

function New-ReplayMetricOutput {
    param([string]$Mode = 'parallel', [string]$EffectiveMode = 'parallel', [int]$Scheduler = 1,
        [int]$Workers = 2, [int]$Submitted = 20, [int]$Executed = 20, [int]$Fallback = 0,
		[string]$ReplayArgument = 'Stage5Validation\reference.rep',
		[int]$CollisionShadowMismatches = 0, [int]$CollisionUnexpectedFallbacks = 0,
		[int]$CollisionShadowExecutions = 0,
		[int]$CollisionShadowComparedCandidates = 0,
		[int]$CollisionOwnerFallbacks = 0,
		[int]$CollisionStaleRejections = 0,
		[int]$PhysicsAuthoritativeBatches = -1, [int]$PhysicsCommittedPrefixes = -1,
		[int]$PhysicsRanges = -1, [int]$PhysicsSubmitted = -1,
		[int]$PhysicsCompleted = -1, [int]$PhysicsShadowExecutions = 0,
		[int]$PhysicsPhysicalWorkerJobs = -1, [int]$PhysicsOwnerHelpedJobs = -1,
		[Int64]$PhysicsPhysicalWorkerMask = -1,
		[int]$PhysicsDistinctPhysicalWorkers = -1,
		[int]$PhysicsPhysicalWorkerMaskComplete = 1,
		[int]$PhysicsPeakConcurrentPhysicalWorkers = -1,
		[int]$PhysicsShadowPrefixes = 0, [int]$PhysicsShadowRanges = 0,
		[int]$PhysicsShadowSubmitted = 0, [int]$PhysicsShadowCompleted = 0,
		[int]$PhysicsShadowMismatches = 0, [int]$PhysicsUnexpectedFallbacks = 0,
		[int]$PhysicsOwnerFallbacks = 0, [int]$PhysicsStaleRejections = 0,
		[int]$PhysicsCircuitBreakerTrips = 0,
		[int]$CollisionAuthoritativeCommits = -1,
		[int]$CollisionCommittedCandidates = -1,
		[int]$CollisionPreparedPairs = -1, [int]$CollisionUniqueCandidates = -1,
		[int]$CollisionSubmitted = -1, [int]$CollisionCompleted = -1,
		[int]$CollisionIneligibleSlices = -1,
		[int]$CollisionPhysicalWorkerJobs = -1,
		[int]$CollisionOwnerHelpedJobs = -1,
		[Int64]$CollisionPhysicalWorkerMask = -1,
		[int]$CollisionDistinctPhysicalWorkers = -1,
		[int]$CollisionPhysicalWorkerMaskComplete = 1,
		[int]$StatusAuthoritativeBatches = -1, [int]$StatusCommittedCommands = -1,
		[int]$StatusSubmitted = -1, [int]$StatusCompleted = -1,
		[int]$StatusPhysicalWorkerJobs = -1, [int]$StatusOwnerHelpedJobs = -1,
		[Int64]$StatusPhysicalWorkerMask = -1,
		[int]$StatusDistinctPhysicalWorkers = -1,
		[int]$StatusPhysicalWorkerMaskComplete = 1,
		[int]$StatusPeakConcurrentPhysicalWorkers = -1,
		[int]$SpatialCapturedArenas = -1, [int]$SpatialCaptureFailures = 0,
		[int]$SpatialSuccessfulCollections = -1,
		[int]$SpatialSuccessfulCollectionQueries = -1,
		[int]$SpatialSuccessfulCollectionRanges = -1,
		[int]$SpatialMultiRangeCollections = -1,
		[int]$SpatialCollectionSubmitted = -1,
		[int]$SpatialCollectionCompleted = -1,
		[int]$SpatialCollectionPhysical = -1,
		[int]$SpatialCollectionOwnerHelped = 0,
		[Int64]$SpatialCollectionPhysicalWorkerMask = -1,
		[int]$SpatialMaximumCollectionQueries = -1,
		[int]$SpatialMaximumCollectionRanges = -1,
		[int]$SpatialMaximumCollectionDistinctPhysicalWorkers = -1,
		[int]$SpatialHealingEligible = 5,
		[int]$SpatialHealingAuthoritative = -1,
		[int]$SpatialHealingCandidates = -1,
		[int]$SpatialHealingSubmitted = -1, [int]$SpatialHealingCompleted = -1,
		[int]$SpatialHealingPhysical = -1, [int]$SpatialHealingOwnerHelped = 0,
		[int]$SpatialHealingExpectedFallbacks = -1,
		[int]$SpatialHealingUnexpectedFallbacks = 0,
		[int]$SpatialPdlEligible = 5,
		[int]$SpatialPdlAuthoritative = -1, [int]$SpatialPdlCandidates = -1,
		[int]$SpatialPdlSubmitted = -1, [int]$SpatialPdlCompleted = -1,
		[int]$SpatialPdlPhysical = -1, [int]$SpatialPdlOwnerHelped = 0,
		[int]$SpatialPdlExpectedFallbacks = -1,
		[int]$SpatialPdlUnexpectedFallbacks = 0)
    $collisionPreparedEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
    if ($CollisionAuthoritativeCommits -lt 0) { $CollisionAuthoritativeCommits = if ($collisionPreparedEligible) { 3 } else { 0 } }
    if ($CollisionCommittedCandidates -lt 0) { $CollisionCommittedCandidates = if ($CollisionAuthoritativeCommits -gt 0) { 12 } else { 0 } }
    if ($CollisionPreparedPairs -lt 0) { $CollisionPreparedPairs = if ($collisionPreparedEligible) { 24 } else { 0 } }
    if ($CollisionUniqueCandidates -lt 0) { $CollisionUniqueCandidates = if ($collisionPreparedEligible) { 12 } else { 0 } }
    if ($CollisionSubmitted -lt 0) { $CollisionSubmitted = if ($collisionPreparedEligible) { 4 } else { 0 } }
    if ($CollisionCompleted -lt 0) { $CollisionCompleted = $CollisionSubmitted }
	if ($CollisionPhysicalWorkerJobs -lt 0) {
		$CollisionPhysicalWorkerJobs = if ($Workers -gt 1) {
			$CollisionCompleted
		} else { 0 }
	}
	if ($CollisionOwnerHelpedJobs -lt 0) {
		$CollisionOwnerHelpedJobs = $CollisionCompleted -
			$CollisionPhysicalWorkerJobs
	}
	if ($CollisionDistinctPhysicalWorkers -lt 0) {
		$CollisionDistinctPhysicalWorkers = if ($CollisionPhysicalWorkerJobs -gt 0) {
			[Math]::Min(2, $Workers)
		} else { 0 }
	}
	if ($CollisionPhysicalWorkerMask -lt 0) {
		$CollisionPhysicalWorkerMask = if ($CollisionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $CollisionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
    if ($CollisionIneligibleSlices -lt 0) { $CollisionIneligibleSlices = if ($Mode -ceq 'serial') { 0 } else { 2 } }
    $physicsAuthoritativeEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
    if ($PhysicsAuthoritativeBatches -lt 0) { $PhysicsAuthoritativeBatches = if ($physicsAuthoritativeEligible) { 3 } else { 0 } }
    if ($PhysicsCommittedPrefixes -lt 0) { $PhysicsCommittedPrefixes = if ($physicsAuthoritativeEligible) { 96 } else { 0 } }
    if ($PhysicsRanges -lt 0) { $PhysicsRanges = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsSubmitted -lt 0) { $PhysicsSubmitted = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($PhysicsCompleted -lt 0) { $PhysicsCompleted = $PhysicsSubmitted }
	if ($PhysicsPhysicalWorkerJobs -lt 0) { $PhysicsPhysicalWorkerJobs = if ($physicsAuthoritativeEligible) { $PhysicsCompleted } else { 0 } }
	if ($PhysicsOwnerHelpedJobs -lt 0) { $PhysicsOwnerHelpedJobs = $PhysicsCompleted - $PhysicsPhysicalWorkerJobs }
	if ($PhysicsDistinctPhysicalWorkers -lt 0) { $PhysicsDistinctPhysicalWorkers = if ($PhysicsPhysicalWorkerJobs -gt 0) { [Math]::Min(2, $Workers) } else { 0 } }
	if ($PhysicsPhysicalWorkerMask -lt 0) { $PhysicsPhysicalWorkerMask = if ($PhysicsDistinctPhysicalWorkers -gt 0) { [Int64](([UInt64]1 -shl $PhysicsDistinctPhysicalWorkers) - 1) } else { 0 } }
	if ($PhysicsPeakConcurrentPhysicalWorkers -lt 0) { $PhysicsPeakConcurrentPhysicalWorkers = $PhysicsDistinctPhysicalWorkers }
	if ($StatusAuthoritativeBatches -lt 0) { $StatusAuthoritativeBatches = if ($physicsAuthoritativeEligible) { 3 } else { 0 } }
	if ($StatusCommittedCommands -lt 0) { $StatusCommittedCommands = if ($StatusAuthoritativeBatches -gt 0) { 96 } else { 0 } }
	if ($StatusSubmitted -lt 0) { $StatusSubmitted = if ($physicsAuthoritativeEligible) { 4 } else { 0 } }
	if ($StatusCompleted -lt 0) { $StatusCompleted = $StatusSubmitted }
	if ($StatusPhysicalWorkerJobs -lt 0) { $StatusPhysicalWorkerJobs = if ($physicsAuthoritativeEligible) { $StatusCompleted } else { 0 } }
	if ($StatusOwnerHelpedJobs -lt 0) { $StatusOwnerHelpedJobs = $StatusCompleted - $StatusPhysicalWorkerJobs }
	if ($StatusDistinctPhysicalWorkers -lt 0) { $StatusDistinctPhysicalWorkers = if ($StatusPhysicalWorkerJobs -gt 0) { [Math]::Min(2, $Workers) } else { 0 } }
	if ($StatusPhysicalWorkerMask -lt 0) { $StatusPhysicalWorkerMask = if ($StatusDistinctPhysicalWorkers -gt 0) { [Int64](([UInt64]1 -shl $StatusDistinctPhysicalWorkers) - 1) } else { 0 } }
	if ($StatusPeakConcurrentPhysicalWorkers -lt 0) { $StatusPeakConcurrentPhysicalWorkers = $StatusDistinctPhysicalWorkers }
	$physicsPreparedWork = $PhysicsSubmitted -gt 0 -or $PhysicsShadowSubmitted -gt 0
	$physicsAllocatedBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsCaptureNanoseconds = if ($physicsPreparedWork) { 100 } else { 0 }
	$physicsPrepareNanoseconds = if ($physicsPreparedWork) { 200 } else { 0 }
	$physicsWaitNanoseconds = if ($physicsPreparedWork) { 300 } else { 0 }
	$physicsCommitNanoseconds = if ($physicsPreparedWork) { 400 } else { 0 }
	$physicsStorageBytes = if ($physicsPreparedWork) { 4096 } else { 0 }
	$physicsStorageCapacityBytes = if ($physicsPreparedWork) { 8192 } else { 0 }
	$physicsStorageAllocations = if ($physicsPreparedWork) { 1 } else { 0 }
	$spatialEligible = $Mode -ceq 'parallel' -and $Workers -gt 1
	$spatialCollectionEligible = ($Mode -ceq 'parallel' -or $Mode -ceq 'shadow') -and $Workers -gt 1
	if ($SpatialCapturedArenas -lt 0) {
		$SpatialCapturedArenas = if ($spatialCollectionEligible) { 4 } else { 0 }
	}
	if ($SpatialSuccessfulCollections -lt 0) { $SpatialSuccessfulCollections = if ($spatialCollectionEligible) { 4 } else { 0 } }
	if ($SpatialMaximumCollectionQueries -lt 0) { $SpatialMaximumCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 5 } else { 0 } }
	if ($SpatialMaximumCollectionRanges -lt 0) {
		$SpatialMaximumCollectionRanges = if ($SpatialSuccessfulCollections -gt 0) {
			[Math]::Min($Workers, $SpatialMaximumCollectionQueries)
		} else { 0 }
	}
	if ($SpatialSuccessfulCollectionQueries -lt 0) { $SpatialSuccessfulCollectionQueries = if ($SpatialSuccessfulCollections -gt 0) { 20 } else { 0 } }
	if ($SpatialSuccessfulCollectionRanges -lt 0) {
		$SpatialSuccessfulCollectionRanges = $SpatialSuccessfulCollections *
			$SpatialMaximumCollectionRanges
	}
	if ($SpatialMultiRangeCollections -lt 0) { $SpatialMultiRangeCollections = $SpatialSuccessfulCollections }
	if ($SpatialCollectionSubmitted -lt 0) { $SpatialCollectionSubmitted = 2 * $SpatialSuccessfulCollectionRanges }
	if ($SpatialCollectionCompleted -lt 0) { $SpatialCollectionCompleted = $SpatialCollectionSubmitted }
	if ($SpatialCollectionPhysical -lt 0) { $SpatialCollectionPhysical = $SpatialCollectionCompleted }
	if ($SpatialMaximumCollectionDistinctPhysicalWorkers -lt 0) {
		$SpatialMaximumCollectionDistinctPhysicalWorkers = if ($SpatialSuccessfulCollections -gt 0) {
			if ($SpatialMaximumCollectionRanges -ge 4) { 2 } else { 1 }
		} else { 0 }
	}
	if ($SpatialCollectionPhysicalWorkerMask -lt 0) {
		$SpatialCollectionPhysicalWorkerMask = if ($SpatialMaximumCollectionDistinctPhysicalWorkers -gt 0) {
			[Int64](([UInt64]1 -shl $SpatialMaximumCollectionDistinctPhysicalWorkers) - 1)
		} else { 0 }
	}
	if ($SpatialHealingAuthoritative -lt 0) { $SpatialHealingAuthoritative = if ($spatialEligible) { 3 } else { 0 } }
	if ($SpatialHealingCandidates -lt 0) { $SpatialHealingCandidates = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingSubmitted -lt 0) { $SpatialHealingSubmitted = if ($SpatialHealingAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialHealingCompleted -lt 0) { $SpatialHealingCompleted = $SpatialHealingSubmitted }
	if ($SpatialHealingPhysical -lt 0) { $SpatialHealingPhysical = $SpatialHealingCompleted }
	if ($SpatialPdlAuthoritative -lt 0) { $SpatialPdlAuthoritative = if ($spatialEligible) { 2 } else { 0 } }
	if ($SpatialPdlCandidates -lt 0) { $SpatialPdlCandidates = if ($SpatialPdlAuthoritative -gt 0) { 5 } else { 0 } }
	if ($SpatialPdlSubmitted -lt 0) { $SpatialPdlSubmitted = if ($SpatialPdlAuthoritative -gt 0) { 8 } else { 0 } }
	if ($SpatialPdlCompleted -lt 0) { $SpatialPdlCompleted = $SpatialPdlSubmitted }
	if ($SpatialPdlPhysical -lt 0) { $SpatialPdlPhysical = $SpatialPdlCompleted }
	if ($SpatialHealingExpectedFallbacks -lt 0) {
		$SpatialHealingExpectedFallbacks = if ($Mode -ceq 'serial' -or $Workers -le 1) { 2 } else { 0 }
	}
	if ($SpatialPdlExpectedFallbacks -lt 0) {
		$SpatialPdlExpectedFallbacks = if ($Mode -ceq 'serial' -or $Workers -le 1) { 2 } else { 0 }
	}
    return ('SIMULATION_JOB_METRICS replay="' + $ReplayArgument + '" ' +
        "requested_mode=$Mode effective_mode=$EffectiveMode requested_pipeline=serial effective_pipeline=serial " +
        "scheduler_started=$Scheduler workers=$Workers submitted=$Submitted executed=$Executed steals=0 owner_help=0 " +
        "waits=0 worker_wait_rejections=0 failures=0 cancelled=0 fallback=$Fallback queue_latency_ns=1 " +
        "max_queue_latency_ns=1 sleeps=0 wakes=0 affinity_failures=0 queue_high_water=1 peak_active_workers=$Workers " +
        "available_cpus=16 reserved_owner_cpus=1 selected_worker_cpus=$Workers") + "`n" +
        ("COLLISION_CANDIDATE_MANIFEST authoritative_commits=$CollisionAuthoritativeCommits shadow_executions=$CollisionShadowExecutions " +
        "shadow_compared_candidates=$CollisionShadowComparedCandidates " +
        "shadow_mismatches=$CollisionShadowMismatches owner_fallbacks=$CollisionOwnerFallbacks " +
		"unexpected_fallbacks=$CollisionUnexpectedFallbacks ineligible_slices=$CollisionIneligibleSlices stale_rejections=$CollisionStaleRejections " +
		"committed_candidates=$CollisionCommittedCandidates prepared_pairs=$CollisionPreparedPairs unique_candidates=$CollisionUniqueCandidates submitted_jobs=$CollisionSubmitted completed_jobs=$CollisionCompleted " +
		"physical_worker_jobs=$CollisionPhysicalWorkerJobs owner_helped_jobs=$CollisionOwnerHelpedJobs " +
		"physical_worker_mask=$CollisionPhysicalWorkerMask distinct_physical_workers=$CollisionDistinctPhysicalWorkers " +
		"physical_worker_mask_complete=$CollisionPhysicalWorkerMaskComplete") + "`n" +
		("PHYSICS_INTEGRATION_MANIFEST authoritative_batches=$PhysicsAuthoritativeBatches committed_prefixes=$PhysicsCommittedPrefixes ranges=$PhysicsRanges " +
		"submitted_jobs=$PhysicsSubmitted completed_jobs=$PhysicsCompleted physical_worker_jobs=$PhysicsPhysicalWorkerJobs owner_helped_jobs=$PhysicsOwnerHelpedJobs " +
		"physical_worker_mask=$PhysicsPhysicalWorkerMask distinct_physical_workers=$PhysicsDistinctPhysicalWorkers physical_worker_mask_complete=$PhysicsPhysicalWorkerMaskComplete peak_concurrent_physical_workers=$PhysicsPeakConcurrentPhysicalWorkers " +
		"allocated_bytes=$physicsAllocatedBytes capture_ns=$physicsCaptureNanoseconds prepare_ns=$physicsPrepareNanoseconds " +
		"wait_ns=$physicsWaitNanoseconds commit_ns=$physicsCommitNanoseconds storage_bytes=$physicsStorageBytes storage_capacity_bytes=$physicsStorageCapacityBytes " +
		"storage_allocations=$physicsStorageAllocations shadow_executions=$PhysicsShadowExecutions " +
		"shadow_prefixes=$PhysicsShadowPrefixes shadow_ranges=$PhysicsShadowRanges " +
		"shadow_submitted_jobs=$PhysicsShadowSubmitted shadow_completed_jobs=$PhysicsShadowCompleted " +
		"shadow_matches=$($PhysicsShadowExecutions - $PhysicsShadowMismatches) " +
		"shadow_mismatches=$PhysicsShadowMismatches owner_fallbacks=$PhysicsOwnerFallbacks ineligible_slices=2 " +
		"unexpected_fallbacks=$PhysicsUnexpectedFallbacks stale_rejections=$PhysicsStaleRejections circuit_breaker_trips=$PhysicsCircuitBreakerTrips") + "`n" +
		("OBJECT_STATUS_TIMER_MANIFEST authoritative_batches=$StatusAuthoritativeBatches committed_commands=$StatusCommittedCommands " +
		"submitted_jobs=$StatusSubmitted completed_jobs=$StatusCompleted physical_worker_jobs=$StatusPhysicalWorkerJobs owner_helped_jobs=$StatusOwnerHelpedJobs " +
		"physical_worker_mask=$StatusPhysicalWorkerMask distinct_physical_workers=$StatusDistinctPhysicalWorkers physical_worker_mask_complete=$StatusPhysicalWorkerMaskComplete peak_concurrent_physical_workers=$StatusPeakConcurrentPhysicalWorkers " +
		"shadow_executions=0 shadow_commands=0 shadow_matches=0 shadow_mismatches=0 owner_fallbacks=0 stale_rejections=0") + "`n" +
		("IMMUTABLE_SPATIAL_MANIFEST captured_arenas=$SpatialCapturedArenas capture_failures=$SpatialCaptureFailures " +
		"successful_collections=$SpatialSuccessfulCollections " +
		"successful_collection_queries=$SpatialSuccessfulCollectionQueries " +
		"successful_collection_ranges=$SpatialSuccessfulCollectionRanges " +
		"multi_range_collections=$SpatialMultiRangeCollections " +
		"collection_submitted_jobs=$SpatialCollectionSubmitted collection_completed_jobs=$SpatialCollectionCompleted " +
		"collection_physical_worker_jobs=$SpatialCollectionPhysical collection_owner_helped_jobs=$SpatialCollectionOwnerHelped " +
		"collection_physical_worker_mask=$SpatialCollectionPhysicalWorkerMask " +
		"maximum_collection_queries=$SpatialMaximumCollectionQueries maximum_collection_ranges=$SpatialMaximumCollectionRanges " +
		"maximum_collection_distinct_physical_workers=$SpatialMaximumCollectionDistinctPhysicalWorkers " +
		"healing_eligible_queries=$SpatialHealingEligible healing_authoritative_queries=$SpatialHealingAuthoritative " +
		"healing_authoritative_candidates=$SpatialHealingCandidates healing_shadow_queries=0 " +
		"healing_shadow_matches=0 healing_shadow_mismatches=0 " +
		"healing_submitted_jobs=$SpatialHealingSubmitted healing_completed_jobs=$SpatialHealingCompleted " +
		"healing_physical_worker_jobs=$SpatialHealingPhysical healing_owner_helped_jobs=$SpatialHealingOwnerHelped " +
		"healing_expected_fallbacks=$SpatialHealingExpectedFallbacks healing_unexpected_fallbacks=$SpatialHealingUnexpectedFallbacks " +
		"healing_stale_rejections=0 healing_validation_failures=0 healing_circuit_breaker_trips=0 " +
		"pdl_eligible_queries=$SpatialPdlEligible pdl_authoritative_queries=$SpatialPdlAuthoritative " +
		"pdl_authoritative_candidates=$SpatialPdlCandidates pdl_shadow_queries=0 " +
		"pdl_shadow_matches=0 pdl_shadow_mismatches=0 " +
		"pdl_submitted_jobs=$SpatialPdlSubmitted pdl_completed_jobs=$SpatialPdlCompleted " +
		"pdl_physical_worker_jobs=$SpatialPdlPhysical pdl_owner_helped_jobs=$SpatialPdlOwnerHelped " +
		"pdl_expected_fallbacks=$SpatialPdlExpectedFallbacks pdl_unexpected_fallbacks=$SpatialPdlUnexpectedFallbacks " +
		"pdl_stale_rejections=0 pdl_validation_failures=0 pdl_circuit_breaker_trips=0")
}

function New-ReplayResultOutput {
    param([int]$FinalFrame = 42000, [string]$FinalCRC = '01020304',
        [string]$ReplayArgument = 'Stage5Validation\reference.rep')
    return 'SIMULATION_REPLAY_RESULT replay="' + $ReplayArgument + '" ' +
        "final_frame=$FinalFrame final_crc=$FinalCRC"
}

function Write-TimingFixture {
    param([string]$Path, [switch]$HeaderOnly, [switch]$BadHeader, [switch]$MissingLogic,
        [switch]$Interactive, [switch]$CollisionPhases, [switch]$CollisionShadowPhase,
        [string]$WallMilliseconds = '100.000')
    $header = 'session,mode,frame_begin,frame_end,logic_frames,wall_ms,phase,samples,total_ms,avg_ms,p95_upper_ms,p99_upper_ms,max_ms,over_33ms,over_100ms'
    if ($BadHeader) { $header = $header.Replace('frame_end', 'last_frame') }
    $lines = New-Object 'Collections.Generic.List[string]'
    $lines.Add($header) | Out-Null
    if (-not $HeaderOnly) {
        $mode = if ($Interactive) { 'interactive' } else { 'headless' }
        $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,frame,42000,90.000,0.0021,0.0040,0.0080,0.0200,0,0") | Out-Null
        if (-not $MissingLogic) {
            $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,logic,42000,80.000,0.0019,0.0040,0.0080,0.0200,0,0") | Out-Null
        }
        if ($CollisionPhases) {
            foreach ($phase in @('collision_admission', 'simulation_snapshot',
                'simulation_parallel', 'simulation_wait', 'simulation_reduce',
                'collision_live_validation', 'simulation_commit')) {
                $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,$phase,1,1.000,1.000,1.000,1.000,1.000,0,0") | Out-Null
            }
        }
        if ($CollisionShadowPhase) {
            foreach ($phase in @('collision_existing_filter',
                'collision_commit_prepare', 'simulation_shadow_compare')) {
                $lines.Add("1,$mode,1,42000,41999,$WallMilliseconds,$phase,1,1.000,1.000,1.000,1.000,1.000,0,0") | Out-Null
            }
        }
    }
    [IO.File]::WriteAllLines($Path, $lines.ToArray())
}

function New-PerformanceResult {
    param([string]$Configuration, [int]$Sequence, [double]$WallMilliseconds, [int]$Workers,
        [int]$AvailableCpus = 16)
    return [pscustomobject]@{
        kind = 'replay'; stress = $true; configuration = $Configuration; sequence = $Sequence
        wallMilliseconds = $WallMilliseconds
        replayMetrics = [pscustomobject]@{
            workers = $Workers; availableCpus = $AvailableCpus; selectedWorkerCpus = $Workers
        }
    }
}

$scriptPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
$runnerParseTokens = $null
$runnerParseErrors = $null
$runnerTree = [Management.Automation.Language.Parser]::ParseFile(
    $scriptPath, [ref]$runnerParseTokens, [ref]$runnerParseErrors)
if (@($runnerParseErrors).Count -ne 0) {
    throw "Validation runner did not parse: $($runnerParseErrors -join '; ')"
}
$relativePathDefinitions = @($runnerTree.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'ConvertTo-OutputRelativePath'
}, $true))
if ($relativePathDefinitions.Count -ne 1) {
    throw 'Validation runner must define exactly one ConvertTo-OutputRelativePath helper.'
}
Invoke-Expression $relativePathDefinitions[0].Extent.Text
$configuredScratchRoot = [Environment]::GetEnvironmentVariable(
    'RTS_STAGE5_VALIDATION_SCRATCH_ROOT')
if ([string]::IsNullOrWhiteSpace($configuredScratchRoot)) {
    if ($env:GITHUB_ACTIONS -eq 'true' -and
        -not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
        $configuredScratchRoot = $env:RUNNER_TEMP
    }
    else {
        throw 'RTS_STAGE5_VALIDATION_SCRATCH_ROOT must identify an explicit task-owned test scratch root.'
    }
}
$temporaryBase = [IO.Path]::GetFullPath($configuredScratchRoot)
$root = Join-Path $temporaryBase ('GGC-Stage5Validation-Test-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    if ($runPlan) {
        Assert-InstalledNet3ModuleBoundary (Join-Path $root 'installed-net3-module-fixture.json')
        Assert-Stage5SerialBaselineCorpusCaptureContract
        Assert-Stage5LiveRoleContract
        Assert-Stage5HardAi2v6CompletionContract
        Assert-Stage5LivePlanPrelaunchBinding
        Assert-Stage5LivePlanCallerRouting
    }
    $runtime = Join-Path $root 'runtime'
    $fixtures = Join-Path $root 'fixtures'
    New-Item -ItemType Directory -Path $runtime | Out-Null
    New-Item -ItemType Directory -Path $fixtures | Out-Null
    # Non-executable fixture bytes declare the capability contract so negative
    # execution tests reach the later path guards they are intended to prove.
    # Marker-free preflight rejection is covered by the profile contract tests.
    [IO.File]::WriteAllText((Join-Path $runtime 'generalszh.exe'),
        'installed candidate fixture RTS_STAGE5_PROFILE_ROOT_CAPABILITY_V1 -validationExecutableSha256')
    [IO.File]::WriteAllText((Join-Path $runtime 'launcher.exe'), 'launcher fixture')
    [IO.File]::WriteAllText((Join-Path $runtime 'launcher.lcf'), 'RUN = . generalszh.exe')
    [IO.File]::WriteAllText((Join-Path $fixtures 'reference.rep'), 'reference replay fixture')
    [IO.File]::WriteAllText((Join-Path $fixtures 'hard-ai-2v6.rep'), 'stress replay fixture')
    $executableHash = Get-Sha256 (Join-Path $runtime 'generalszh.exe')
    $referenceHash = Get-Sha256 (Join-Path $fixtures 'reference.rep')
    $stressHash = Get-Sha256 (Join-Path $fixtures 'hard-ai-2v6.rep')
    $manifest = Join-Path $root 'manifest.json'
    Write-TestManifest $manifest $executableHash $referenceHash $stressHash
    $planOutput = Join-Path $root 'plan-output'
    if (-not $runPlan) {
        New-Item -ItemType Directory -Path $planOutput | Out-Null
    }

    if ($runPlan) {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $planOutput `
        -ValidationSet All -ReplayMatrixRepeats 2 -StressRepeats 3 -MinimumFreeBytes 1 `
        -AllowNonStandardCorpus -PlanOnly | Out-Null
    $plan = Get-Content -LiteralPath (Join-Path $planOutput 'validation-plan.json') -Raw | ConvertFrom-Json
    Assert-True (-not $plan.deterministicRuntimeEligible -and
        -not $plan.finalAcceptanceEligible) `
        'nonstandard corpus plans are ineligible for the deterministic-runtime and final gates'
    Assert-True ($plan.entries.Count -eq 141) `
        'two-pass replay, three-seed repeated AI matrix, and one shadow stress run have 141 planned runs'
    Assert-True (-not $plan.x64Required -and -not $plan.performanceRequested) `
        'a functional plan does not silently claim x64 or performance acceptance'
    Assert-True (@($plan.entries | Where-Object { $_.kind -ceq 'replay' }).Count -eq 56) `
        'replay matrix covers seven configurations, two passes, and three stress runs'
    Assert-True (@($plan.entries | Where-Object { $_.kind -ceq 'ai' }).Count -eq 85) `
        'AI matrix covers both scenarios, three seeds, repeats, seven regular configurations, and one shadow stress run'
    $shadowEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'shadow-16' })
    Assert-True ($shadowEntries.Count -eq 1 -and $shadowEntries[0].kind -ceq 'ai' -and
        $shadowEntries[0].stress -and $shadowEntries[0].scenario -ceq '4v2' -and
        $shadowEntries[0].simulationMode -ceq 'shadow' -and
        $shadowEntries[0].requestedWorkers -ceq '16') `
        'installed validation plan contains exactly one 16-worker 4v2 collision shadow stress execution'
    $autoEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'parallel-auto' })
    Assert-True ($autoEntries.Count -gt 0) 'automatic worker configuration is present'
    Assert-True (@($autoEntries | Where-Object { $_.arguments -contains '-workerCount' }).Count -eq 0) `
        'automatic worker configuration omits -workerCount'
    $explicitEntries = @($plan.entries | Where-Object { $_.configuration -ceq 'parallel-16' })
    Assert-True (@($explicitEntries | Where-Object { $_.arguments -contains '-workerCount' }).Count -eq $explicitEntries.Count) `
        'explicit worker configurations include -workerCount'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-validationExecutableSha256' }).Count -eq 0) `
        'every run supplies executable provenance'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-pipelineMode' }).Count -eq 0) `
        'every run holds Stage 4 pipeline mode constant'
    Assert-True (@($plan.entries | Where-Object {
        $index = [Array]::IndexOf([object[]]$_.arguments, '-pipelineMode')
        $index -lt 0 -or $_.arguments[$index + 1] -cne 'serial'
    }).Count -eq 0) 'every replay and AI run honestly requests the serial pipeline'
    Assert-True (@($plan.entries | Where-Object { $_.arguments -notcontains '-simulationMode' }).Count -eq 0) `
        'every run explicitly selects simulation mode'
    $shadowModeIndex = [Array]::IndexOf([object[]]$shadowEntries[0].arguments, '-simulationMode')
    Assert-True ($shadowModeIndex -ge 0 -and
        $shadowEntries[0].arguments[$shadowModeIndex + 1] -ceq 'shadow') `
        'shadow stress execution passes simulationMode=shadow to the installed runtime'
    Assert-Stage5LiveManifestPlanRouting $runtime $manifest $plan

    $localCapacityOutput = Join-Path $root 'local-capacity-plan-output'
    $localCapacityProcessors = @(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop)
    $localCapacityPhysical = [int](($localCapacityProcessors |
        Measure-Object -Property NumberOfCores -Sum).Sum)
    $localCapacityLogical = [int](($localCapacityProcessors |
        Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum)
    $localCapacityHostSupported = $localCapacityPhysical -ge 4 -and
        $localCapacityPhysical -le 6 -and $localCapacityLogical -ge $localCapacityPhysical -and
        $localCapacityLogical -le 12
    if ($localCapacityHostSupported) {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot $localCapacityOutput -ValidationSet All `
            -ReplayMatrixRepeats 2 -StressRepeats 3 -MinimumFreeBytes 1 `
            -AllowNonStandardCorpus -CapacityMode LocalCapacity -PlanOnly | Out-Null
        $localCapacityPlan = Get-Content -LiteralPath `
            (Join-Path $localCapacityOutput 'validation-plan.json') -Raw | ConvertFrom-Json
        $localCapacityReceipt = Get-Content -LiteralPath `
            (Join-Path $localCapacityOutput 'local-capacity-plan-receipt.json') -Raw | ConvertFrom-Json
        $localRegularConfigurationIds = @($localCapacityPlan.entries |
            Where-Object { $_.configuration -ne 'shadow-8' } |
            ForEach-Object { $_.configuration } | Sort-Object -Unique)
        Assert-True ($localCapacityPlan.capacityMode -ceq 'LocalCapacity' -and
            $localCapacityPlan.validationMode -ceq 'LocalCapacity' -and
            $localCapacityPlan.diagnosticNonAcceptance -and
            -not $localCapacityPlan.deterministicRuntimeEligible -and
            -not $localCapacityPlan.finalAcceptanceEligible -and
            -not $localCapacityPlan.acceptanceReceiptRequested) `
            'LocalCapacity plans are explicitly diagnostic and ineligible for canonical acceptance'
        Assert-True ($localRegularConfigurationIds.Count -eq 6 -and
            @('serial-1', 'parallel-1', 'parallel-2', 'parallel-4',
                'parallel-8', 'parallel-auto' | Where-Object {
                    $localRegularConfigurationIds -notcontains $_
                }).Count -eq 0 -and
            @($localCapacityPlan.entries | Where-Object {
                $_.configuration -eq 'parallel-16' -or
                $_.configuration -eq 'shadow-16'
            }).Count -eq 0) `
            'LocalCapacity selects only serial-1, parallel-1/2/4/8/auto and omits 16-worker configurations'
        Assert-True (@($localCapacityPlan.entries).Count -eq 121 -and
            @($localCapacityPlan.entries | Where-Object { $_.kind -eq 'replay' }).Count -eq 48 -and
            @($localCapacityPlan.entries | Where-Object { $_.kind -eq 'ai' }).Count -eq 73) `
            'LocalCapacity preserves the complete selected replay/AI cross-product and one shadow run'
        $localAutoEntries = @($localCapacityPlan.entries | Where-Object {
            $_.configuration -ceq 'parallel-auto'
        })
        $localShadowEntries = @($localCapacityPlan.entries | Where-Object {
            $_.configuration -ceq 'shadow-8'
        })
        Assert-True ($localAutoEntries.Count -gt 0 -and
            @($localAutoEntries | Where-Object { $_.arguments -contains '-workerCount' }).Count -eq 0 -and
            @($localCapacityPlan.entries | Where-Object {
                $_.requestedWorkers -match '^(?:16|shadow-16)$'
            }).Count -eq 0 -and
            $localShadowEntries.Count -eq 1 -and
            $localShadowEntries[0].requestedWorkers -ceq '8' -and
            $localShadowEntries[0].simulationMode -ceq 'shadow') `
            'LocalCapacity keeps auto unforced and bounds the collision shadow plan to eight workers'
        Assert-True ($localCapacityPlan.localCapacity.hostTopology.physicalCoreCount -eq
            $localCapacityPhysical -and
            $localCapacityPlan.localCapacity.hostTopology.logicalProcessorCount -eq
            $localCapacityLogical -and
            $localCapacityPlan.localCapacity.maximumPhysicalCoreCount -eq 6 -and
            $localCapacityPlan.localCapacity.maximumLogicalProcessorCount -eq 12 -and
            -not $localCapacityPlan.localCapacity.externalAcceptanceEligible -and
            -not $localCapacityPlan.localCapacity.canonicalFinalAcceptanceEligible) `
            'LocalCapacity records the bounded host topology and non-acceptance limits'
        Assert-True ($localCapacityReceipt.receiptKind -ceq
            'stage5-local-capacity-receipt' -and
            $localCapacityReceipt.validationMode -ceq 'LocalCapacity' -and
            $localCapacityReceipt.status -ceq 'planned-non-acceptance' -and
            -not $localCapacityReceipt.acceptanceEligible -and
            -not $localCapacityReceipt.finalAcceptanceEligible -and
            -not $localCapacityReceipt.externalAcceptanceEligible) `
            'LocalCapacity writes a visibly non-acceptance report instead of a canonical final envelope'
        Assert-True (-not (Test-Path -LiteralPath `
            (Join-Path $localCapacityOutput 'validation-plan-receipt.json')) -and
            -not (Test-Path -LiteralPath `
                (Join-Path $localCapacityOutput 'local-capacity-receipt.json'))) `
            'LocalCapacity plan-only output does not emit a canonical or completed receipt'
    }
    else {
        Assert-Throws {
            & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
                -OutputRoot $localCapacityOutput -ValidationSet All `
                -ReplayMatrixRepeats 2 -StressRepeats 3 -MinimumFreeBytes 1 `
                -AllowNonStandardCorpus -CapacityMode LocalCapacity -PlanOnly | Out-Null
        } 'LocalCapacity.*(?:physical|logical)' `
            'LocalCapacity fails closed when the host is outside the bounded local topology'
    }

    $localAiOnlyManifest = Join-Path $root 'local-ai-only-manifest.json'
    Write-AiOnlyTestManifest $localAiOnlyManifest $executableHash
    $localAiOnlyOutput = Join-Path $root 'local-ai-only-plan-output'
    if ($localCapacityHostSupported) {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $localAiOnlyManifest `
            -OutputRoot $localAiOnlyOutput -ValidationSet AI -MinimumFreeBytes 1 `
            -CapacityMode LocalCapacity -PlanOnly | Out-Null
        $localAiOnlyPlan = Get-Content -LiteralPath `
            (Join-Path $localAiOnlyOutput 'validation-plan.json') -Raw | ConvertFrom-Json
        Assert-True ($localAiOnlyPlan.validationSet -ceq 'AI' -and
            $localAiOnlyPlan.capacityMode -ceq 'LocalCapacity' -and
            -not $localAiOnlyPlan.replayCorpusRequired -and
            $localAiOnlyPlan.replayFixtureCount -eq 0 -and
            @($localAiOnlyPlan.entries | Where-Object { $_.kind -ceq 'replay' }).Count -eq 0 -and
            @($localAiOnlyPlan.entries | Where-Object { $_.kind -ceq 'ai' }).Count -eq 73 -and
            -not $localAiOnlyPlan.finalAcceptanceEligible) `
            'LocalCapacity AI-only corpus seeding does not require or execute old replay fixtures'
        $exportGuardTaskRoot = Join-Path $root 'export-guard-task'
        New-Item -ItemType Directory -Path $exportGuardTaskRoot -Force | Out-Null
        $exportGuardManifest = Join-Path $root 'export-guard-manifest.json'
        Write-AiOnlyTestManifest $exportGuardManifest $executableHash `
            -Scenarios @('4v3', '4v2', 'hard-ai-2v6')
        Assert-Throws {
            & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $exportGuardManifest `
                -OutputRoot (Join-Path $root 'export-guard-outside-output') `
                -TaskRoot $exportGuardTaskRoot -ValidationSet AI -MinimumFreeBytes 1 `
                -CapacityMode LocalCapacity -CorpusExportRoot 'fresh-native-corpus' `
                -AllowHeadlessDirectExecution | Out-Null
        } 'OutputRoot.*(?:task-owned|TaskRoot|containing)' `
            'CorpusExportRoot rejects execution when OutputRoot is outside TaskRoot before any game run'
    }
    else {
        Assert-Throws {
            & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $localAiOnlyManifest `
                -OutputRoot $localAiOnlyOutput -ValidationSet AI -MinimumFreeBytes 1 `
                -CapacityMode LocalCapacity -PlanOnly | Out-Null
        } 'LocalCapacity.*(?:physical|logical)' `
            'LocalCapacity AI-only corpus seeding still fails closed outside the bounded local topology'
    }
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'canonical-corpus-export-plan-output') `
            -ValidationSet AI -MinimumFreeBytes 1 -CorpusExportRoot `
            (Join-Path $root 'canonical-corpus-export') -PlanOnly | Out-Null
    } 'CorpusExportRoot.*LocalCapacity.*AI' `
        'CorpusExportRoot is restricted to an executing LocalCapacity AI-only lane'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'local-capacity-acceptance-output') `
            -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus `
            -CapacityMode LocalCapacity -PlanOnly `
            -AcceptanceSourceCommit ('a' * 40) `
            -AcceptanceArtifactSetSha256 ('B' * 64) `
            -AcceptanceRuntimeDependencyManifestSha256 ('C' * 64) `
            -AcceptanceRuntimeClosureSha256 ('D' * 64) | Out-Null
    } 'LocalCapacity.*acceptance' `
        'LocalCapacity cannot request canonical acceptance bindings'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'local-capacity-performance-output') `
            -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus `
            -CapacityMode LocalCapacity -PlanOnly -EnforcePerformance | Out-Null
    } 'LocalCapacity.*performance|Performance.*diagnostic' `
        'LocalCapacity cannot request canonical performance enforcement'

    $standardManifest = Join-Path $root 'standard-manifest.json'
    Write-StandardTestManifest $standardManifest $executableHash $fixtures
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $standardManifest `
            -OutputRoot (Join-Path $root 'one-pass-all-output') -ValidationSet All `
            -ReplayMatrixRepeats 1 -StressRepeats 3 -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly two complete replay matrix passes' `
        'the deterministic-runtime gate cannot reduce the replay matrix to one pass'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $standardManifest `
            -OutputRoot (Join-Path $root 'one-stress-all-output') -ValidationSet All `
            -ReplayMatrixRepeats 2 -StressRepeats 1 -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly three executions of the stress replay' `
        'the deterministic-runtime gate cannot reduce the stress replay execution count'
    $functionalOnlyOutput = Join-Path $root 'functional-only-plan-output'
    $functionalOnlyStdout = @(& $scriptPath -RuntimeRoot $runtime `
        -FixtureManifestPath $standardManifest -OutputRoot $functionalOnlyOutput `
        -ValidationSet All -MinimumFreeBytes 1 -PlanOnly) -join "`n"
    $functionalOnlyPlan = Get-Content -LiteralPath `
        (Join-Path $functionalOnlyOutput 'validation-plan.json') -Raw | ConvertFrom-Json
    Assert-True (-not $functionalOnlyPlan.deterministicRuntimeEligible -and
        -not $functionalOnlyPlan.finalAcceptanceEligible -and
        -not $functionalOnlyPlan.performanceRequested -and
        $functionalOnlyPlan.performanceRequiredForDeterministicRuntimeGate) `
        'an All matrix without enforced Stage 3 performance evidence is not a passing deterministic-runtime gate'
    Assert-True (@($functionalOnlyPlan.entries | Where-Object { $_.kind -ceq 'replay' }).Count -eq 168) `
        'the focused functional plan still proves the exact 24-execution replay matrix for all seven configurations'
    Assert-True ($functionalOnlyStdout -match 'focused/diagnostic deterministic-runtime' -and
        $functionalOnlyStdout -notmatch '\bpassed\b') `
        'an All plan without performance prints an explicit focused result and never a passed banner'

    $aiOnlyOutput = Join-Path $root 'ai-only-plan-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $aiOnlyOutput `
        -ValidationSet AI -MinimumFreeBytes 1 -PlanOnly | Out-Null
    $aiOnlyPlan = Get-Content -LiteralPath (Join-Path $aiOnlyOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True ($aiOnlyPlan.validationSet -ceq 'AI' -and
        -not $aiOnlyPlan.deterministicRuntimeEligible -and
        -not $aiOnlyPlan.finalAcceptanceEligible) `
        'AI-only plan with a two-fixture structural manifest remains a focused partial gate'

    $replayOnlyOutput = Join-Path $root 'replay-only-plan-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $replayOnlyOutput `
        -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    $replayOnlyPlan = Get-Content -LiteralPath (Join-Path $replayOnlyOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True ($replayOnlyPlan.validationSet -ceq 'Replay' -and
        -not $replayOnlyPlan.deterministicRuntimeEligible -and
        -not $replayOnlyPlan.finalAcceptanceEligible) `
        'Replay-only plan remains a focused partial gate'

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'require-x64-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly -RequireX64 | Out-Null
    } 'x64 validation requires a valid PE executable' `
        'RequireX64 rejects a non-PE replay candidate before producing acceptance evidence'

    $validationSource = Get-Content -LiteralPath $scriptPath -Raw
    $runnerTokens = $null
    $runnerParseErrors = $null
    $runnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $scriptPath, [ref]$runnerTokens, [ref]$runnerParseErrors)
    Assert-True (@($runnerParseErrors).Count -eq 0) `
        'the installed-runtime runner parses before structural contracts are inspected'
    $planAssignmentIndex = $validationSource.IndexOf(
        '$planPath = Join-Path $outputFull ''validation-plan.json''',
        [StringComparison]::Ordinal)
    $planOnlyIndex = $validationSource.IndexOf('if ($PlanOnly) {',
        $planAssignmentIndex, [StringComparison]::Ordinal)
    $planWriteMatches = [regex]::Matches($validationSource,
        'Write-Stage5TextFileAtomically\s+\$planPath')
    $launcherEquivalenceIndex = $validationSource.IndexOf(
        '$launcherEquivalence = Assert-LauncherEquivalenceContract',
        $planAssignmentIndex, [StringComparison]::Ordinal)
    Assert-True ($planAssignmentIndex -ge 0 -and $planOnlyIndex -gt $planAssignmentIndex -and
        $planWriteMatches.Count -eq 2 -and
        $planWriteMatches[0].Index -gt $planOnlyIndex -and
        $planWriteMatches[0].Index -lt $launcherEquivalenceIndex -and
        $planWriteMatches[1].Index -gt $launcherEquivalenceIndex -and
        $validationSource -notmatch
            'if\s*\(\$PlanOnly\s+-or\s+\$planDocument\.schemaVersion\s+-eq\s+1\)') `
        'plan-only owns the sole early plan write and schema-v1 execution writes once after launcher equivalence'

    $aiTimingBranches = @($runnerAst.FindAll({
        param($node)
        if ($node -isnot [System.Management.Automation.Language.IfStatementAst] -or
            $node.Clauses.Count -ne 1 -or $null -eq $node.ElseClause) {
            return $false
        }
        return $node.Clauses[0].Item1.Extent.Text -match
                '^\s*\$entry\.kind\s+-ceq\s+''ai''\s*$' -and
            $node.Clauses[0].Item2.Extent.Text -match
                'ConvertFrom-Stage5AiCompletion'
    }, $true))
    $aiTimingMatches = [regex]::Matches($validationSource,
        '\$timingEvidence\.maximumFrameEnd\s+-eq\s+\(\[UInt64\]\$aiEvidence\.endFrame\s+\+\s+1\)')
    $replayTimingMatches = [regex]::Matches($validationSource,
        '\$timingEvidence\.maximumFrameEnd\s+-eq\s+\$replayResult\.finalFrame')
    $aiTimingBody = if ($aiTimingBranches.Count -eq 1) {
        $aiTimingBranches[0].Clauses[0].Item2.Extent
    } else { $null }
    $replayTimingBody = if ($aiTimingBranches.Count -eq 1) {
        $aiTimingBranches[0].ElseClause.Extent
    } else { $null }
    Assert-True ($aiTimingBranches.Count -eq 1 -and
        $aiTimingMatches.Count -eq 1 -and
        $aiTimingMatches[0].Index -ge $aiTimingBody.StartOffset -and
        $aiTimingMatches[0].Index -lt $aiTimingBody.EndOffset -and
        $replayTimingMatches.Count -eq 1 -and
        $replayTimingMatches[0].Index -ge $replayTimingBody.StartOffset -and
        $replayTimingMatches[0].Index -lt $replayTimingBody.EndOffset -and
        $validationSource -notmatch
            '\$timingEvidence\.maximumFrameEnd\s+-eq\s+\$aiEvidence\.endFrame') `
        'AI timing uses the post-victory recorder frame while replay timing remains exact'
    Assert-ValidationProcessTerminationContract $validationSource
    Assert-True ($validationSource -match 'ValidateSet\(''Generals'', ''ZeroHour''\).*\$Title' -and
        $validationSource -match 'manifestData\.title' -and
        $validationSource -match 'expectedExecutablePrefix') `
        'the installed-runtime runner binds title-specific manifest and executable evidence'
    $localReceiptFunctionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Write-LocalCapacityReceipt'
    }, $true)
    Assert-True ($null -ne $localReceiptFunctionAst) `
        'LocalCapacity receipt writer is present for lifecycle testing'
    if ($null -ne $localReceiptFunctionAst) {
        Invoke-Expression $localReceiptFunctionAst.Extent.Text
        $lifecyclePlanPath = Join-Path $root 'local-capacity-lifecycle-plan.json'
        $lifecycleResultsPath = Join-Path $root 'local-capacity-lifecycle-results.json'
        Write-JsonDocument $lifecyclePlanPath ([ordered]@{ stage = 'plan' })
        Write-JsonDocument $lifecycleResultsPath ([ordered]@{ stage = 'results' })
        $lifecycleEntries = @(
            [pscustomobject]@{ kind = 'replay' }
            [pscustomobject]@{ kind = 'ai' }
        )
        $lifecycleResults = @(
            [pscustomobject]@{ kind = 'replay' }
            [pscustomobject]@{ kind = 'ai' }
        )
        $lifecycleConfigurations = @(
            [pscustomobject]@{ Id = 'serial-1' }
            [pscustomobject]@{ Id = 'parallel-1' }
        )
        $lifecycleShadow = [pscustomobject]@{ Id = 'shadow-8' }
        $lifecycleTopology = [pscustomobject]@{
            source = 'Win32_Processor'; physicalCoreCount = 6; logicalProcessorCount = 12
        }
        $lifecyclePlanReceiptPath = Join-Path $root 'local-capacity-lifecycle-plan-receipt.json'
        $lifecycleFinalReceiptPath = Join-Path $root 'local-capacity-lifecycle-receipt.json'
        Write-LocalCapacityReceipt -Path $lifecyclePlanReceiptPath `
            -Status 'planned-non-acceptance' -ValidationSet 'All' `
            -Entries $lifecycleEntries -Results @() `
            -Configurations $lifecycleConfigurations -ShadowConfiguration $lifecycleShadow `
            -Topology $lifecycleTopology -PlanPath $lifecyclePlanPath `
            -ResultsPath $lifecycleResultsPath | Out-Null
        Write-LocalCapacityReceipt -Path $lifecycleFinalReceiptPath `
            -Status 'passed-non-acceptance' -ValidationSet 'All' `
            -Entries $lifecycleEntries -Results $lifecycleResults `
            -Configurations $lifecycleConfigurations -ShadowConfiguration $lifecycleShadow `
            -Topology $lifecycleTopology -PlanPath $lifecyclePlanPath `
            -ResultsPath $lifecycleResultsPath | Out-Null
        $plannedLifecycleReceipt = Get-Content -LiteralPath $lifecyclePlanReceiptPath -Raw |
            ConvertFrom-Json
        $finalLifecycleReceipt = Get-Content -LiteralPath $lifecycleFinalReceiptPath -Raw |
            ConvertFrom-Json
        Assert-True ($plannedLifecycleReceipt.status -ceq 'planned-non-acceptance' -and
            -not $plannedLifecycleReceipt.replayDeterminismAsserted -and
            -not $plannedLifecycleReceipt.aiDeterminismAsserted -and
            $finalLifecycleReceipt.status -ceq 'passed-non-acceptance' -and
            $finalLifecycleReceipt.replayDeterminismAsserted -and
            $finalLifecycleReceipt.aiDeterminismAsserted -and
            -not $finalLifecycleReceipt.finalAcceptanceEligible -and
            $lifecyclePlanReceiptPath -ne $lifecycleFinalReceiptPath) `
            'LocalCapacity planned and completed receipts use distinct paths and report assertions only after completion'
    }
    $localCorpusExportFunctionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Export-LocalCapacityAiCorpus'
    }, $true)
    $pathWithinFunctionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Test-PathWithin'
    }, $true)
    $assertJsonIntegerFunctionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-JsonInteger'
    }, $true)
    $persistedIntegerFunctionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'ConvertTo-Stage5PersistedInteger'
    }, $true)
    Assert-True ($null -ne $localCorpusExportFunctionAst) `
        'LocalCapacity corpus export integration helper is present for mocked-run testing'
    Assert-True ($null -ne $pathWithinFunctionAst) `
        'LocalCapacity corpus export integration path guard is present for mocked-run testing'
    Assert-True ($null -ne $assertJsonIntegerFunctionAst -and
        $null -ne $persistedIntegerFunctionAst) `
        'LocalCapacity corpus export integer validators are present for mocked-run testing'
    if ($null -ne $pathWithinFunctionAst) {
        Invoke-Expression $pathWithinFunctionAst.Extent.Text
    }
    if ($null -ne $assertJsonIntegerFunctionAst -and
        $null -ne $persistedIntegerFunctionAst) {
        Invoke-Expression $assertJsonIntegerFunctionAst.Extent.Text
        Invoke-Expression $persistedIntegerFunctionAst.Extent.Text
    }
    if ($null -ne $localCorpusExportFunctionAst -and $null -ne $localReceiptFunctionAst -and
        $null -ne $pathWithinFunctionAst -and $null -ne $assertJsonIntegerFunctionAst -and
        $null -ne $persistedIntegerFunctionAst) {
        Invoke-Expression $localCorpusExportFunctionAst.Extent.Text
        $corpusTaskRoot = Join-Path $root 'mock-corpus-task'
        $corpusTaskRunRoot = Join-Path $corpusTaskRoot 'validation-run-mock'
        $corpusProfileRoot = Join-Path $corpusTaskRunRoot 'Documents\Profile'
        $corpusReplayRoot = Join-Path $corpusProfileRoot 'Replays\Stage5Validation'
        $corpusExportRoot = Join-Path $corpusTaskRoot 'fresh-native-corpus'
        New-Item -ItemType Directory -Path $corpusReplayRoot -Force | Out-Null
        $mockReplayPath = Join-Path $corpusReplayRoot 'SkirmishAI-4v3-1729-mock.rep'
        Write-MinimalRpl3TestFile $mockReplayPath
        $mockReplayHash = Get-Sha256 $mockReplayPath
        $mockRunNonce = '11111111-22222222-33333333'
        $mockCompletion = 'SKIRMISH_AI_TEST_COMPLETE seed=1729 scenario=4v3 ' +
            "run_nonce=$mockRunNonce replay_epoch=3 replay_sha256=$mockReplayHash " +
            ('replay_retained="' + $mockReplayPath + '"')
        $mockEntry = [pscustomobject]@{
            kind = 'ai'; sequence = 1; scenario = '4v3'; seed = 1729
            configuration = 'serial-1'; simulationMode = 'serial'
            requestedWorkers = '1'; repeat = 1
        }
        $mockChildRuns = @(
            [pscustomobject]@{
                entry = $mockEntry
                run = [pscustomobject]@{
                    exitCode = 0; timedOut = $false; stdout = $mockCompletion
                }
            }
        )
        $mockResults = @(
            [pscustomobject]@{
                kind = 'ai'; sequence = 1; scenario = '4v3'; seed = 1729
                aiEvidence = [pscustomobject]@{
                    finalDigest = 'A1B2C3D4'
                    fields = [ordered]@{ actual_ai = '7'; actual_teams = '4v3' }
                }
            }
        )
        $mockResultsPath = Join-Path $corpusTaskRoot 'validation-results.json'
        New-Item -ItemType Directory -Path $corpusTaskRoot -Force | Out-Null
        Write-JsonDocument $mockResultsPath ([ordered]@{ results = $mockResults })
        Assert-Throws {
            Export-LocalCapacityAiCorpus `
                -TaskRoot $corpusTaskRoot -TaskRunRoot $corpusTaskRunRoot `
                -ProfileRoot $corpusProfileRoot `
                -CorpusExportRoot (Join-Path $corpusTaskRunRoot 'bad-corpus') `
                -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
                -ChildRuns $mockChildRuns -Results $mockResults `
                -ValidationResultsPath $mockResultsPath | Out-Null
        } 'CorpusExportRoot.*durable.*sibling' `
            'LocalCapacity corpus export refuses a corpus root inside the ephemeral task-run root'
        $mockExport = Export-LocalCapacityAiCorpus `
            -TaskRoot $corpusTaskRoot -TaskRunRoot $corpusTaskRunRoot `
            -ProfileRoot $corpusProfileRoot -CorpusExportRoot $corpusExportRoot `
            -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
            -ChildRuns $mockChildRuns -Results $mockResults `
            -ValidationResultsPath $mockResultsPath
        $serialCaptureExportRoot = Join-Path $corpusTaskRoot 'serial-baseline-corpus'
        $serialCaptureExport = Export-LocalCapacityAiCorpus `
            -TaskRoot $corpusTaskRoot -TaskRunRoot $corpusTaskRunRoot `
            -ProfileRoot $corpusProfileRoot -CorpusExportRoot $serialCaptureExportRoot `
            -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
            -ChildRuns $mockChildRuns -Results $mockResults `
            -ValidationResultsPath $mockResultsPath `
            -CaptureMode 'serial-baseline-ai'
        Assert-True ($serialCaptureExport.status -ceq 'passed' -and
            $serialCaptureExport.captureMode -ceq 'serial-baseline-ai' -and
            @($serialCaptureExport.records | Where-Object {
                $_.configuration -cne 'serial-1'
            }).Count -eq 0) `
            'serial-baseline export records its mode and retains only serial-1 records'
        $serialCaptureUpper = Export-LocalCapacityAiCorpus `
            -TaskRoot $corpusTaskRoot -TaskRunRoot $corpusTaskRunRoot `
            -ProfileRoot $corpusProfileRoot `
            -CorpusExportRoot (Join-Path $corpusTaskRoot 'serial-baseline-upper') `
            -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
            -ChildRuns $mockChildRuns -Results $mockResults `
            -ValidationResultsPath $mockResultsPath `
            -CaptureMode 'SERIAL-BASELINE-AI'
        Assert-True ($serialCaptureUpper.captureMode -ceq 'serial-baseline-ai') `
            'serial-baseline export canonicalizes an uppercase capture mode before gating records'
        $parallelEntry = $mockEntry | ConvertTo-Json -Depth 8 | ConvertFrom-Json
        $parallelEntry.configuration = 'parallel-2'
        $parallelEntry.simulationMode = 'parallel'
        $parallelEntry.requestedWorkers = '2'
        Assert-Throws {
            Export-LocalCapacityAiCorpus `
                -TaskRoot $corpusTaskRoot -TaskRunRoot $corpusTaskRunRoot `
                -ProfileRoot $corpusProfileRoot `
                -CorpusExportRoot (Join-Path $corpusTaskRoot 'serial-baseline-parallel-rejected') `
                -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
                -ChildRuns @([pscustomobject]@{
                    entry = $parallelEntry; run = $mockChildRuns[0].run
                }) -Results $mockResults `
                -ValidationResultsPath $mockResultsPath `
                -CaptureMode 'serial-baseline-ai' | Out-Null
        } 'serial-1|serial-baseline' `
            'serial-baseline export rejects parallel child records before copying replay bytes'
        $serialCaptureReceiptPath = Join-Path $corpusTaskRoot `
            'serial-baseline-local-capacity-receipt.json'
        Write-LocalCapacityReceipt -Path $serialCaptureReceiptPath `
            -Status 'passed-non-acceptance' -ValidationSet 'AI' `
            -Entries @($mockEntry) -Results $mockResults `
            -Configurations @([pscustomobject]@{ Id = 'serial-1' }) `
            -ShadowConfiguration $null `
            -Topology ([pscustomobject]@{
                source = 'Win32_Processor'; physicalCoreCount = 6; logicalProcessorCount = 12
            }) -PlanPath $mockResultsPath -ResultsPath $mockResultsPath `
            -CorpusExportRoot $serialCaptureExportRoot `
            -CorpusExport $serialCaptureExport | Out-Null
        $serialCaptureReceipt = Get-Content -LiteralPath $serialCaptureReceiptPath -Raw |
            ConvertFrom-Json
        Assert-True ($serialCaptureReceipt.corpusExport.captureMode -ceq
            'serial-baseline-ai' -and
            $serialCaptureReceipt.corpusExportMode -ceq 'serial-baseline-ai' -and
            $serialCaptureReceipt.serialBaselineCorpusCapture -and
            -not $serialCaptureReceipt.finalAcceptanceEligible) `
            'serial-baseline receipt remains non-accepting and records its capture mode'
        $serialCaptureManifest = Write-Stage5FreshReplayCorpusManifest `
            -TaskRoot $corpusTaskRoot -CorpusExportRoot $serialCaptureExportRoot `
            -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
            -Records $serialCaptureExport.records -ValidationResultsPath $mockResultsPath `
            -ValidationReceiptPath $serialCaptureReceiptPath `
            -CaptureMode 'serial-baseline-ai'
        $serialCaptureManifestDocument = Get-Content -LiteralPath `
            $serialCaptureManifest.path -Raw | ConvertFrom-Json
        Assert-True ($serialCaptureManifestDocument.captureMode -ceq
            'serial-baseline-ai') `
            'serial-baseline corpus manifest records its distinct capture mode'
        $mockReceiptPath = Join-Path $corpusTaskRoot 'local-capacity-receipt.json'
        Write-LocalCapacityReceipt -Path $mockReceiptPath `
            -Status 'passed-non-acceptance' -ValidationSet 'AI' `
            -Entries @($mockEntry) -Results $mockResults `
            -Configurations @([pscustomobject]@{ Id = 'serial-1' }) `
            -ShadowConfiguration ([pscustomobject]@{ Id = 'shadow-8' }) `
            -Topology ([pscustomobject]@{
                source = 'Win32_Processor'; physicalCoreCount = 6; logicalProcessorCount = 12
            }) -PlanPath $mockResultsPath -ResultsPath $mockResultsPath `
            -CorpusExportRoot $corpusExportRoot -CorpusExport $mockExport | Out-Null
        $mockManifest = Write-Stage5FreshReplayCorpusManifest `
            -TaskRoot $corpusTaskRoot -CorpusExportRoot $corpusExportRoot `
            -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
            -Records $mockExport.records -ValidationResultsPath $mockResultsPath `
            -ValidationReceiptPath $mockReceiptPath
        Assert-True ($mockExport.status -ceq 'passed' -and
            $mockExport.recordCount -eq 1 -and
            (Test-Path -LiteralPath $mockExport.artifactIndex.path -PathType Leaf) -and
            (Test-Path -LiteralPath $mockManifest.path -PathType Leaf)) `
            'LocalCapacity exports one mocked completed AI recording, artifact index, and final corpus manifest'
        Assert-Throws {
            Write-Stage5FreshReplayArtifactIndex `
                -TaskRoot $corpusTaskRoot -CorpusExportRoot $corpusExportRoot `
                -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
                -Records $mockExport.records -ValidationResultsPath $mockResultsPath | Out-Null
        } 'already exists|refusing overwrite' `
            'artifact index refuses a second write'
        Assert-Throws {
            Write-Stage5FreshReplayCorpusManifest `
                -TaskRoot $corpusTaskRoot -CorpusExportRoot $corpusExportRoot `
                -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
                -Records $mockExport.records -ValidationResultsPath $mockResultsPath `
                -ValidationReceiptPath $mockReceiptPath | Out-Null
        } 'already exists|refusing overwrite' `
            'final corpus manifest refuses a second write'
        $mockManifestDocument = Get-Content -LiteralPath $mockManifest.path -Raw | ConvertFrom-Json
        $mockReceipt = Get-Content -LiteralPath $mockReceiptPath -Raw | ConvertFrom-Json
        $artifactIndexDocument = Get-Content -LiteralPath $mockExport.artifactIndex.path -Raw | ConvertFrom-Json
        $artifactIndexHash = Get-Sha256 $mockExport.artifactIndex.path
        $receiptHash = Get-Sha256 $mockReceiptPath
        Assert-True ($mockReceipt.corpusExport.artifactIndexPath -ceq
            $mockExport.artifactIndex.path -and
            $mockReceipt.corpusExport.artifactIndexSha256 -ceq $artifactIndexHash -and
            $mockManifestDocument.validationReceipt.path -ceq $mockReceiptPath -and
            $mockManifestDocument.validationReceipt.sha256 -ceq $receiptHash -and
            $artifactIndexDocument.kind -ceq 'stage5-native-replay-artifact-index') `
            'final corpus manifest binds the exact final receipt, which binds the immutable artifact index'
        $originalReceiptText = Get-Content -LiteralPath $mockReceiptPath -Raw
        [IO.File]::WriteAllText($mockReceiptPath, '{"tampered":true}')
        Assert-Throws {
            Convert-Stage5FreshReplayCorpusManifestToFixtures `
                -CorpusManifestPath $mockManifest.path `
                -FixtureManifestPath (Join-Path $corpusExportRoot 'tampered-receipt-fixtures.json') `
                -ProvenancePath (Join-Path $corpusExportRoot 'tampered-receipt-provenance.json') `
                -Executable 'generalszh.exe' | Out-Null
        } 'receipt.*SHA|binding' 'corpus conversion rejects a tampered final receipt'
        [IO.File]::WriteAllText($mockReceiptPath, $originalReceiptText)
        $originalIndexText = Get-Content -LiteralPath $mockExport.artifactIndex.path -Raw
        [IO.File]::WriteAllText($mockExport.artifactIndex.path, '{"tampered":true}')
        Assert-Throws {
            Convert-Stage5FreshReplayCorpusManifestToFixtures `
                -CorpusManifestPath $mockManifest.path `
                -FixtureManifestPath (Join-Path $corpusExportRoot 'tampered-index-fixtures.json') `
                -ProvenancePath (Join-Path $corpusExportRoot 'tampered-index-provenance.json') `
                -Executable 'generalszh.exe' | Out-Null
        } 'artifact index.*SHA|binding' 'corpus conversion rejects a tampered artifact index'
        [IO.File]::WriteAllText($mockExport.artifactIndex.path, $originalIndexText)
        $originalManifestText = Get-Content -LiteralPath $mockManifest.path -Raw
        $tamperedManifestDocument = $mockManifestDocument
        $tamperedManifestDocument.validationReceipt.sha256 = ('0' * 64)
        Write-JsonDocument $mockManifest.path $tamperedManifestDocument
        Assert-Throws {
            Convert-Stage5FreshReplayCorpusManifestToFixtures `
                -CorpusManifestPath $mockManifest.path `
                -FixtureManifestPath (Join-Path $corpusExportRoot 'tampered-manifest-fixtures.json') `
                -ProvenancePath (Join-Path $corpusExportRoot 'tampered-manifest-provenance.json') `
                -Executable 'generalszh.exe' | Out-Null
        } 'receipt.*SHA|binding' 'corpus conversion rejects a tampered final corpus manifest'
        [IO.File]::WriteAllText($mockManifest.path, $originalManifestText)
        Remove-Item -LiteralPath $corpusTaskRunRoot -Recurse -Force
        $mockManifestDocument = Get-Content -LiteralPath $mockManifest.path -Raw | ConvertFrom-Json
        $mockReceipt = Get-Content -LiteralPath $mockReceiptPath -Raw | ConvertFrom-Json
        $mockExportedPath = [string]$mockManifestDocument.records[0].destinationPath
        $mockExportedBytes = [IO.File]::ReadAllBytes($mockExportedPath)
        $mockExportedText = [Text.Encoding]::Unicode.GetString($mockExportedBytes)
        Assert-True ((Test-Path -LiteralPath $corpusExportRoot -PathType Container) -and
            $mockManifestDocument.kind -ceq 'stage5-native-replay-corpus' -and
            $mockManifestDocument.origin -ceq 'native-fresh-runtime' -and
            $mockManifestDocument.records.Count -eq 1 -and
            (Test-Path -LiteralPath $mockExportedPath -PathType Leaf) -and
            [Text.Encoding]::ASCII.GetString($mockExportedBytes[0..3]) -ceq 'RPL3' -and
            [BitConverter]::ToUInt32($mockExportedBytes, 4) -eq 2 -and
            [BitConverter]::ToUInt32($mockExportedBytes, 8) -eq 1 -and
            $mockExportedText -match '\[SkirmishAIEpoch=3\]' -and
            $mockReceipt.corpusExportRequested -and
            $mockReceipt.corpusExportRoot -ceq $corpusExportRoot -and
            $mockReceipt.corpusExport.corpusExportRoot -ceq $corpusExportRoot -and
            $mockReceipt.corpusExport.recordCount -eq 1 -and
            -not $mockReceipt.finalAcceptanceEligible -and
            -not $mockReceipt.externalAcceptanceEligible) `
            'LocalCapacity export preserves validated RPL3 schema/epochs and survives task-run cleanup without becoming acceptance evidence'
    }
    $unboundedTerminationFixture = $validationSource -replace `
        '\$Process\.WaitForExit\(\$PostKillWaitMilliseconds\)', '$Process.WaitForExit()'
    Assert-Throws {
        Assert-ValidationProcessTerminationContract $unboundedTerminationFixture
    } 'unbounded WaitForExit' `
        'validation timeout contract rejects an unbounded post-kill wait'

    $oneSeedManifest = Join-Path $root 'one-seed-all.json'
    Write-TestManifest $oneSeedManifest $executableHash $referenceHash $stressHash `
        -Seeds @(1729)
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $oneSeedManifest `
            -OutputRoot (Join-Path $root 'one-seed-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'at least three distinct' `
        'ValidationSet All rejects a reduced one-seed live-AI matrix'

    $oneScenarioManifest = Join-Path $root 'one-scenario-all.json'
    Write-TestManifest $oneScenarioManifest $executableHash $referenceHash $stressHash `
        -Scenarios @('4v2')
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $oneScenarioManifest `
            -OutputRoot (Join-Path $root 'one-scenario-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'both the 4v3 and 4v2' `
        'ValidationSet All rejects a matrix missing the mandatory 4v3 scenario'

    $duplicateSeedManifest = Join-Path $root 'duplicate-seed-all.json'
    Write-TestManifest $duplicateSeedManifest $executableHash $referenceHash $stressHash `
        -Seeds @(1729, 1729, 1730)
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $duplicateSeedManifest `
            -OutputRoot (Join-Path $root 'duplicate-seed-all-output') -ValidationSet All `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'seeds must be distinct' `
        'duplicate seed entries cannot disguise a three-seed acceptance matrix'

    }
    if ($runRuntime) {
    Assert-Stage5OutputRelativePathContract $root $PSCommandPath
    $badHashManifest = Join-Path $root 'bad-hash.json'
    Write-TestManifest $badHashManifest ('0' * 64) $referenceHash $stressHash
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $badHashManifest `
            -OutputRoot (Join-Path $root 'bad-hash-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'SHA-256 mismatch' 'candidate hash mismatch fails closed'

    $overrideOutput = Join-Path $root 'hash-override-output'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $badHashManifest `
        -OutputRoot $overrideOutput -ValidationSet Replay -MinimumFreeBytes 1 `
        -ExpectedExecutableSha256 $executableHash -AllowNonStandardCorpus -PlanOnly | Out-Null
    $overridePlan = Get-Content -LiteralPath (Join-Path $overrideOutput 'validation-plan.json') `
        -Raw | ConvertFrom-Json
    Assert-True ($overridePlan.executableSha256 -ceq $executableHash) `
        'controller hash override records the exact installed candidate hash'
    Assert-True ($overridePlan.executableSha256Source -ceq 'argument') `
        'validation plan identifies the controller hash source'

    $escapeFile = Join-Path (Split-Path -Parent $root) 'stage5-escape.rep'
    [IO.File]::WriteAllText($escapeFile, 'escape fixture')
    try {
        $escapeHash = Get-Sha256 $escapeFile
        $escapeManifest = Join-Path $root 'escape.json'
        Write-TestManifest $escapeManifest $executableHash $escapeHash $stressHash '..\stage5-escape.rep'
        Assert-Throws {
            & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $escapeManifest `
                -OutputRoot (Join-Path $root 'escape-output') -ValidationSet Replay `
                -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
        } 'escapes the manifest directory' 'fixture traversal fails closed'
    }
    finally {
        if (Test-Path -LiteralPath $escapeFile) { Remove-Item -LiteralPath $escapeFile -Force }
    }

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'standard-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -PlanOnly | Out-Null
    } 'exactly 10 fixtures' 'standard gate rejects an incomplete corpus'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'nonstandard-acceptance-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus | Out-Null
    } 'limited to PlanOnly or DiagnosticNonAcceptance' `
        'nonstandard replay corpus cannot execute as an accepting gate'

    $duplicateSourceManifest = Join-Path $root 'duplicate-source.json'
    $duplicateDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $duplicateDocument.fixtures[1].source = $duplicateDocument.fixtures[0].source
    $duplicateDocument.fixtures[1].sha256 = $duplicateDocument.fixtures[0].sha256
    [IO.File]::WriteAllText($duplicateSourceManifest, ($duplicateDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $duplicateSourceManifest `
            -OutputRoot (Join-Path $root 'duplicate-source-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'unique source SHA-256' 'different fixture ids cannot disguise duplicate replay content'

    $caseCollisionManifest = Join-Path $root 'case-collision.json'
    $caseCollisionDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $caseCollisionDocument.fixtures[1].id = 'REFERENCE'
    [IO.File]::WriteAllText($caseCollisionManifest,
        ($caseCollisionDocument | ConvertTo-Json -Depth 8))
    $caseCollisionOutput = Join-Path $root 'case-collision-output'
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $caseCollisionManifest `
            -OutputRoot $caseCollisionOutput -ValidationSet Replay -MinimumFreeBytes 1 `
            -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'collides case-insensitively' `
        'distinct replay hashes cannot hide case-colliding fixture IDs on Windows'
    Assert-True (-not (Test-Path -LiteralPath $caseCollisionOutput)) `
        'case-colliding replay IDs fail before evidence/profile creation begins'

    $stringStressManifest = Join-Path $root 'string-stress.json'
    $stringStressDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $stringStressDocument.fixtures[0].stress = 'false'
    [IO.File]::WriteAllText($stringStressManifest, ($stringStressDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $stringStressManifest `
            -OutputRoot (Join-Path $root 'string-stress-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'must be a JSON boolean' 'string stress values cannot be coerced into true'

    $extraPropertyManifest = Join-Path $root 'extra-property.json'
    $extraPropertyDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $extraPropertyDocument.fixtures[0] | Add-Member -NotePropertyName unexpected -NotePropertyValue 1
    [IO.File]::WriteAllText($extraPropertyManifest, ($extraPropertyDocument | ConvertTo-Json -Depth 8))
    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $extraPropertyManifest `
            -OutputRoot (Join-Path $root 'extra-property-output') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'contains unsupported property' 'manifest objects reject properties outside the exact schema'

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $planOutput `
            -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly | Out-Null
    } 'must not already exist' 'evidence directory reuse fails closed'

    $oneWorkerEntry = [pscustomobject]@{
        sequence = 1; configuration = 'parallel-1'; simulationMode = 'parallel'
        requestedWorkers = '1'; seed = 1729; scenario = '4v3'
    }
	$oneWorkerZeroPhysicsOutput = New-AiCompletionOutput `
		-RequestedWorkers '1' -EffectiveWorkers 1 `
            -Submitted 0 -Executed 0 -Fallback 7 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
			-PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0
	$oneWorkerEvidence = ConvertFrom-Stage5AiCompletion `
		$oneWorkerZeroPhysicsOutput $oneWorkerEntry ('A' * 64)
    Assert-True $oneWorkerEvidence.expectedOneWorkerFallback `
        'AI parser accepts the expected forced one-worker serial fallback'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			($oneWorkerZeroPhysicsOutput.Replace('physics_capture_ns=0',
				'physics_capture_ns=1')) $oneWorkerEntry ('A' * 64) | Out-Null
	} 'reports physics pre-scan, capture, or storage work' `
		'forced one-worker AI evidence rejects physics capture work before scheduler preflight'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -PathWorkerExecuted 2 -PathAuthoritativeCommits 1 `
                -PathPeakWorkers 1 -CollisionAuthoritativeCommits 0 `
                -CollisionCommittedCandidates 0 -PhysicsAuthoritativeBatches 0 `
                -PhysicsCommittedPrefixes 0 -PhysicsRanges 0 `
                -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'nonqualifying serial, one-worker, or non-parallel lane reports direct-path' `
        'one-worker AI evidence rejects stale direct-path batch work and authority'
    $oneWorkerCollisionFallbackEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
            -Submitted 0 -Executed 0 -Fallback 7 -CollisionOwnerFallbacks 3 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
        $oneWorkerEntry ('A' * 64)
    Assert-True ($oneWorkerCollisionFallbackEvidence.collisionOwnerFallbacks -eq 3) `
        'AI parser accepts owner-only collision fallback in the forced one-worker lane'
    $twoWorkerEntry = [pscustomobject]@{
        sequence = 2; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v3'
    }
    try {
        $fallbackEvidence = ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Fallback 4 -OwnerFallbacks 4) `
            $twoWorkerEntry ('A' * 64)
        Assert-True ($fallbackEvidence.ownerFallbacks -eq 4) `
            'multiworker AI evidence accepts accounted policy fallback alongside worker execution'
    }
    catch {
        Assert-True $false `
            "multiworker AI evidence rejected accounted policy fallback: $($_.Exception.Message)"
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Fallback 3 -OwnerFallbacks 4) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'global fallback count is smaller than its AI planning fallback count' `
        'AI evidence rejects fallback accounting that exceeds the global scheduler total'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Fallback 6 -OwnerFallbacks 6 `
                -AiRequestedBatches 5) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'AI planning fallback count exceeds its requested batch count' `
        'AI evidence rejects more serial fallbacks than planning requests'
    $scalarSpatialCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -SpatialCapturedArenas 0 `
            -SpatialSuccessfulCollections 0 `
            -SpatialSuccessfulCollectionQueries 0 `
            -SpatialSuccessfulCollectionRanges 0 `
            -SpatialMultiRangeCollections 0 -SpatialCollectionSubmitted 0 `
            -SpatialCollectionCompleted 0 -SpatialCollectionPhysical 0 `
            -SpatialMaximumCollectionQueries 0 -SpatialMaximumCollectionRanges 0 `
            -SpatialHealingEligible 1 -SpatialHealingExpectedFallbacks 1 `
            -SpatialHealingAuthoritative 0 -SpatialHealingCandidates 0 `
            -SpatialHealingSubmitted 0 -SpatialHealingCompleted 0 `
            -SpatialHealingPhysical 0 -SpatialPdlEligible 0 `
            -SpatialPdlExpectedFallbacks 0 -SpatialPdlAuthoritative 0 `
            -SpatialPdlCandidates 0 -SpatialPdlSubmitted 0 `
            -SpatialPdlCompleted 0 -SpatialPdlPhysical 0) `
        $twoWorkerEntry ('A' * 64)
    Assert-True ($scalarSpatialCompletion.spatialEvidence.capturedArenas -eq 0 -and
        $scalarSpatialCompletion.spatialEvidence.healing.eligibleQueries -eq 1 -and
        $scalarSpatialCompletion.spatialEvidence.healing.expectedFallbacks -eq 1 -and
        $scalarSpatialCompletion.spatialEvidence.healing.unexpectedFallbacks -eq 0 -and
        $scalarSpatialCompletion.spatialEvidence.healing.staleRejections -eq 0) `
        'singleton spatial preflight publishes one expected policy fallback with no arena capture or stale failure'
    $serialEntry = [pscustomobject]@{
        sequence = 60; configuration = 'serial-1'; simulationMode = 'serial'
        requestedWorkers = '1'; seed = 1729; scenario = '4v3'
    }
    $serialCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 0 `
            -Submitted 0 -Executed 0 -AuthoritativeCommits 0 -AiCommittedBatches 1 `
            -AiSubmitted 0 -AiCompleted 0) $serialEntry ('A' * 64)
    Assert-True ($serialCompletion.aiCommittedBatches -eq 1 -and
        $serialCompletion.aiParallelAuthoritativeCommits -eq 0 -and
        $serialCompletion.spatialEvidence.capturedArenas -eq 0) `
        'serial AI evidence preserves generic owner commits without claiming parallel or spatial authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialCapturedArenas 0) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'has no captured immutable-spatial arena' `
        'parallel AI evidence rejects an all-zero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialSuccessfulCollections 1 `
                -SpatialMultiRangeCollections 1 `
                -SpatialSuccessfulCollectionQueries 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'does not prove multi-query, multi-range two-pass worker execution' `
        'AI evidence rejects a nominal spatial collection that did not batch multiple queries'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -SpatialCollectionOwnerHelped 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collection reports owner help' `
        'AI spatial collection evidence requires physical workers only'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -SpatialCapturedArenas 1 -SpatialSuccessfulCollections 1 `
                -SpatialMultiRangeCollections 1 `
                -SpatialSuccessfulCollectionQueries 2 `
                -SpatialSuccessfulCollectionRanges 2 `
                -SpatialCollectionSubmitted 4 -SpatialCollectionCompleted 4 `
                -SpatialCollectionPhysical 4 -SpatialMaximumCollectionQueries 2 `
                -SpatialMaximumCollectionRanges 2) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'nonqualifying serial/one-worker lane reports immutable-spatial collection' `
        'one-worker AI evidence rejects stale spatial collection worker authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiCommittedBatches 1 `
                -AiSubmitted 0 -AiCompleted 0 -SpatialCapturedArenas 1) `
            $serialEntry ('A' * 64) | Out-Null
    } 'serial simulation reports captured immutable-spatial arenas' `
        'serial AI evidence rejects a nonzero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 0 `
                -Submitted 0 -Executed 0 -AuthoritativeCommits 1 -AiSubmitted 1 -AiCompleted 1) `
            $serialEntry ('A' * 64) | Out-Null
    } 'AI owner authority outside parallel simulation' `
        'serial AI evidence rejects stale authoritative AI commits from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -ShadowExecutions 1) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'AI shadow work outside shadow simulation' `
        'parallel AI evidence rejects stale AI shadow counters from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -AuthoritativeCommits 1 -AiCommittedBatches 1 `
                -AiParallelAuthoritativeCommits 0) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'does not match the mode-specific AI parallel-authority counter' `
        'generic AI owner commits cannot proxy the mode-specific parallel authority field'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '2' -EffectiveWorkers 0 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'effective worker count does not match' 'AI parser rejects fallback outside the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 0 -Submitted 0 -Executed 0 -Fallback 7) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'effective worker count does not match' `
        'parallel-1 AI completion requires its live one-worker scheduler'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '4' -EffectiveWorkers 1) `
            ([pscustomobject]@{
                sequence = 5; configuration = 'parallel-4'; simulationMode = 'parallel'
                requestedWorkers = '4'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'effective worker count does not match' `
        'parallel-4 AI completion cannot pass with one effective worker'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' -EffectiveWorkers 1 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0) `
            ([pscustomobject]@{
                sequence = 6; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'serial configuration reports active workers or jobs' `
        'serial AI completion cannot report an active worker or jobs'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v3' -ActualAi 7 -ActualTeams '4v3') `
            ([pscustomobject]@{
                sequence = 3; configuration = 'parallel-2'; simulationMode = 'parallel'
                requestedWorkers = '2'; seed = 1729; scenario = '4v2'
            }) ('A' * 64) | Out-Null
    } 'scenario does not match' '4v3 completion evidence cannot satisfy a planned 4v2 scenario'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v3') `
            ([pscustomobject]@{
                sequence = 4; configuration = 'parallel-2'; simulationMode = 'parallel'
                requestedWorkers = '2'; seed = 1729; scenario = '4v2'
            }) ('A' * 64) | Out-Null
    } 'actual_teams does not match' 'AI completion team shape must match the planned scenario'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Seed 1729 -LoadedSeed 1730) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'loaded_seed does not match' `
        'planned seed echo cannot conceal a different seed loaded into the live match'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedPipeline parallel -EffectivePipeline parallel) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'honestly request the serial pipeline' `
        'AI completion with a live parallel pipeline cannot satisfy the serial validation plan'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionShadowMismatches 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collision shadow mismatches' `
        'AI completion rejects collision adapter/legacy ordering mismatches'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionUnexpectedFallbacks 1) `
            $twoWorkerEntry ('A' * 64) | Out-Null
    } 'unexpected collision owner fallbacks' `
        'AI completion rejects unexpected collision fallback publication failures'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0 `
                -CollisionAuthoritativeCommits 1 -CollisionCommittedCandidates 1 `
                -CollisionPreparedPairs 1 -CollisionUniqueCandidates 1 `
                -CollisionSubmitted 1 -CollisionCompleted 1) `
            ([pscustomobject]@{
                sequence = 61; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'collision authority outside parallel' `
        'serial AI evidence rejects stale collision authority from another mode'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Mode serial -RequestedWorkers '1' `
                -EffectiveWorkers 0 -Submitted 0 -Executed 0 `
                -AuthoritativeCommits 0 -AiSubmitted 0 -AiCompleted 0 `
                -CollisionPreparedPairs 24 -CollisionUniqueCandidates 12 `
                -CollisionSubmitted 4 -CollisionCompleted 4) `
            ([pscustomobject]@{
                sequence = 63; configuration = 'serial-1'; simulationMode = 'serial'
                requestedWorkers = '1'; seed = 1729; scenario = '4v3'
            }) ('A' * 64) | Out-Null
    } 'serial simulation reports collision lane work' `
        'serial AI evidence rejects stale prepared collision work even with zero authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -RequestedWorkers '1' -EffectiveWorkers 1 `
                -Submitted 0 -Executed 0 -Fallback 7 `
                -CollisionPreparedPairs 4 -CollisionUniqueCandidates 2 `
                -CollisionSubmitted 1 -CollisionCompleted 1) `
            $oneWorkerEntry ('A' * 64) | Out-Null
    } 'one-worker ineligible simulation reports collision prepared' `
        'one-worker collision-ineligible AI evidence rejects stale prepared work'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -CollisionShadowExecutions 1 `
                -CollisionShadowComparedCandidates 1) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'collision shadow work outside shadow' `
        'parallel AI evidence rejects stale collision shadow counters from another mode'
    $shadowEntry = [pscustomobject]@{
        sequence = 7; configuration = 'shadow-16'; simulationMode = 'shadow'
        requestedWorkers = '16'; seed = 1729; scenario = '4v2'
    }
    $shadowCompletionArguments = @{
        Mode = 'shadow'; RequestedWorkers = '16'; EffectiveWorkers = 16
        Scenario = '4v2'; ActualAi = 6; ActualTeams = '4v2'
        AuthoritativeCommits = 0; AiCommittedBatches = 5; ShadowExecutions = 5
        CollisionAuthoritativeCommits = 0; CollisionShadowExecutions = 3
        CollisionCommittedCandidates = 0; CollisionPreparedPairs = 24
        CollisionUniqueCandidates = 12; CollisionSubmitted = 4; CollisionCompleted = 4
        PathWorkerExecuted = 0; PathAuthoritativeCommits = 0
        PhysicsShadowExecutions = 3
    }
    $shadowCompletion = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput @shadowCompletionArguments) $shadowEntry ('A' * 64)
    Assert-True ($shadowCompletion.collisionShadowExecutions -eq 3 -and
        $shadowCompletion.collisionSubmittedJobs -eq 4 -and
        $shadowCompletion.aiCommittedBatches -eq 5 -and
		$shadowCompletion.aiParallelAuthoritativeCommits -eq 0 -and
		$shadowCompletion.ordinaryPathShadowComparisons -gt 0 -and
		$shadowCompletion.ordinaryPathAuthoritativeCommits -eq 0) `
        'shadow evidence preserves generic AI commits and collision work without claiming parallel AI authority'
	Assert-Throws {
		$missingOrdinaryShadow = @{} + $shadowCompletionArguments
		$missingOrdinaryShadow.OrdinaryPathShadowComparisons = 0
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput @missingOrdinaryShadow) `
			$shadowEntry ('A' * 64) | Out-Null
	} 'shadow stress has no physical-worker ordinary-path comparison' `
		'installed shadow stress cannot omit ordinary-path comparison evidence'
    Assert-Throws {
        $shadowAuthority = @{} + $shadowCompletionArguments
        $shadowAuthority.AuthoritativeCommits = 1
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @shadowAuthority) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'AI owner authority outside parallel simulation' `
        'shadow AI evidence rejects stale authoritative AI commits from the parallel mode'
    Assert-Throws {
        $missingShadowComparison = @{} + $shadowCompletionArguments
        $missingShadowComparison.CollisionShadowExecutions = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @missingShadowComparison) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass without an executed collision comparison'
    Assert-Throws {
        $missingShadowJobs = @{} + $shadowCompletionArguments
        $missingShadowJobs.CollisionSubmitted = 0
        $missingShadowJobs.CollisionCompleted = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @missingShadowJobs) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass on global scheduler jobs without collision jobs'
    Assert-Throws {
        $vacuousShadowComparison = @{} + $shadowCompletionArguments
        $vacuousShadowComparison.CollisionShadowComparedCandidates = 0
        ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @vacuousShadowComparison) `
            $shadowEntry ('A' * 64) | Out-Null
    } 'successful legacy collision insertions' `
        'shadow stress cannot pass when every prepared pair was already present and no insertion was compared'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathUnsupportedAuthority 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_unsupported_authority' `
		'direct-path authority is forbidden in unsupported runtime policy'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathStaleAcceptance 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_stale_acceptance' `
		'stale direct-path output can never satisfy owner authority'
    Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 1 -PathAuthoritativeCommits 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'not backed by physical-worker execution' `
		'owner-help or global jobs cannot proxy direct-path physical-worker authority'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'more multi-worker direct-path commits than authoritative commits' `
		'multi-worker correlation can only be published by an actual authoritative path commit'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 1 `
				-PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 0 -PathPeakWorkers 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'authority from an impossible single-request batch' `
		'direct-path authority requires the runtime minimum of two submitted and worker-executed requests'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathWorkerExecuted 2 `
				-PathAuthoritativeCommits 1 `
				-PathAuthoritativeMultiWorkerCommits 1 -PathPeakWorkers 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'multi-worker direct-path authority without a multi-worker peak' `
		'multi-worker correlation requires an observed path-local peak above one'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathOwnerHelped 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'physical-worker-only' `
		'owner help is never accepted as bounded direct-path batch execution'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathPeakWorkers 3) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'impossible direct-path active-worker count' `
		'path-local peak cannot exceed the effective physical-worker count'
	ConvertFrom-Stage5AiCompletion `
		(New-AiCompletionOutput -PathLateDrains 1) $twoWorkerEntry ('A' * 64) | Out-Null
	Assert-True $true 'late direct-path drains remain diagnostic and do not enter executed or authority identities'
	ConvertFrom-Stage5AiCompletion `
		(New-AiCompletionOutput -PathLateDrains 30) $twoWorkerEntry ('A' * 64) | Out-Null
	Assert-True $true 'late direct-path drains may finish after the manifest and never decide acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathTimeouts 1) $twoWorkerEntry ('A' * 64) | Out-Null
	} 'synchronous direct-path watchdog timeouts' `
		'a terminal-frame direct-path timeout deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PathValidationFailures 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'direct_validation_failures' `
		'an eligible direct-path validation failure deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathOwnerHelpedRangeJobs 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'owner-helped ordinary-path range jobs' `
		'ordinary-path owner help cannot back installed worker authority'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathPhysicalWorkerMask 1 `
				-OrdinaryPathDistinctPhysicalWorkers 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'complete ordinary-path worker-union mask omits the maximum per-batch distinct count' `
		'ordinary-path evidence rejects an uncorrelated physical-worker count'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathPeakWorkers 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'ordinary-path multi-worker authority without a concurrent' `
		'ordinary-path multi-worker commits require observed concurrent workers'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathValidationFailures 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'ordinary_path_validation_failures' `
		'ordinary-path validation failure deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathTimeouts 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'ordinary_path_timeouts' `
		'ordinary-path owner timeout deterministically fails acceptance'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -OrdinaryPathShadowComparisons 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'ordinary-path shadow comparisons outside shadow' `
		'parallel evidence rejects ordinary shadow counters from another mode'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsShadowExecutions 1 -PhysicsShadowMismatches 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden physics evidence' 'AI completion rejects physics shadow divergence'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsUnexpectedFallbacks 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden physics evidence' 'AI completion rejects unexpected physics fallback'
	try {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsOwnerFallbacks 4 `
				-PhysicsStaleRejections 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
		Assert-True $true `
			'AI completion accepts owner-safe physics fallbacks including stale rejections'
	}
	catch {
		Assert-True $false `
			"AI completion rejected owner-safe physics fallback evidence: $($_.Exception.Message)"
	}
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsOwnerFallbacks 1 `
				-PhysicsStaleRejections 2) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'more stale physics rejections than owner fallbacks' `
		'AI completion rejects impossible physics fallback accounting'
	$shadowPhysicsFallbackArguments = @{} + $shadowCompletionArguments
	$shadowPhysicsFallbackArguments.PhysicsOwnerFallbacks = 2
	$shadowPhysicsFallbackArguments.PhysicsStaleRejections = 1
	$noneligibleAiPhysicsFallbackCases = @(
		[pscustomobject]@{
			name = 'serial'; entry = $serialEntry
			output = New-AiCompletionOutput -Mode serial `
				-RequestedWorkers '1' -EffectiveWorkers 0 -Submitted 0 `
				-Executed 0 -AuthoritativeCommits 0 -AiCommittedBatches 1 `
				-AiSubmitted 0 -AiCompleted 0 -PhysicsOwnerFallbacks 2 `
				-PhysicsStaleRejections 1
		},
		[pscustomobject]@{
			name = 'parallel-1'; entry = $oneWorkerEntry
			output = New-AiCompletionOutput -RequestedWorkers '1' `
				-EffectiveWorkers 1 -Submitted 0 -Executed 0 -Fallback 7 `
				-CollisionAuthoritativeCommits 0 `
				-CollisionCommittedCandidates 0 `
				-PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
				-PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
				-PhysicsOwnerFallbacks 2 -PhysicsStaleRejections 1
		},
		[pscustomobject]@{
			name = 'shadow'; entry = $shadowEntry
			output = New-AiCompletionOutput @shadowPhysicsFallbackArguments
		}
	)
	foreach ($fallbackCase in $noneligibleAiPhysicsFallbackCases) {
		Assert-Throws {
			ConvertFrom-Stage5AiCompletion $fallbackCase.output `
				$fallbackCase.entry ('A' * 64) | Out-Null
		} 'physics owner fallback evidence outside an eligible multiworker parallel lane' `
			"AI completion rejects cross-mode physics fallback evidence in $($fallbackCase.name)"
	}
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialHealingSubmitted 8 `
				-SpatialHealingCompleted 7 -SpatialHealingPhysical 7) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'healing immutable-spatial jobs are not balanced' `
		'AI completion rejects incomplete healing spatial jobs'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialHealingShadow 1 `
				-SpatialHealingShadowMismatches 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden healing immutable-spatial evidence' `
		'AI completion rejects healing spatial shadow divergence'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -SpatialPdlUnexpectedFallbacks 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'forbidden pdl immutable-spatial evidence' `
		'AI completion rejects unexpected PDL spatial fallback'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -PhysicsShadowExecutions 1) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'physics shadow work outside shadow' `
		'parallel AI evidence rejects stale physics shadow counters from another mode'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -CollisionSubmitted 4 -CollisionCompleted 3) `
			$twoWorkerEntry ('A' * 64) | Out-Null
	} 'collision submitted/completed job counts do not match' `
		'collision evidence rejects incomplete successful-job telemetry'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -OmitWorkEvidence) $twoWorkerEntry ('A' * 64) | Out-Null
    } 'missing required authoritative Stage 5 work evidence' `
        'acceptance parsing fails closed when slice-specific work evidence is absent'
    $focusedWithoutWork = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -OmitWorkEvidence) $twoWorkerEntry ('A' * 64) $false
    Assert-True ($focusedWithoutWork.authoritativeWorkStatus -ceq 'unavailable-non-acceptance') `
        'focused non-acceptance parsing records unavailable slice metrics without claiming acceptance'

    $validAiResults = @(
        (New-AiResult 'serial-1' 1), (New-AiResult 'serial-1' 2),
        (New-AiResult 'parallel-1' 1), (New-AiResult 'parallel-1' 2)
    )
    Assert-Stage5AiDeterminism $validAiResults @('serial-1', 'parallel-1') 2
    $validAiWithShadow = @($validAiResults) + @((New-AiResult 'shadow-16' 1))
    Assert-Stage5AiDeterminism $validAiWithShadow @('serial-1', 'parallel-1') 2 'shadow-16'
    Assert-Throws {
        Assert-Stage5AiDeterminism $validAiResults @('serial-1', 'parallel-1') 2 'shadow-16'
    } 'exactly one' 'AI matrix cannot pass without its installed collision shadow stress result'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'DEADBEEF'))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'final_digest differs' 'AI digest mismatches cannot pass across configurations'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'A1B2C3D4' 42001))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'end_frame differs' 'AI end-frame mismatches cannot pass across configurations'
    Assert-Throws {
        $bad = @($validAiResults[0..2]) + @((New-AiResult 'parallel-1' 2 'A1B2C3D4' 42000 2))
        Assert-Stage5AiDeterminism $bad @('serial-1', 'parallel-1') 2
    } 'winner_team differs' 'AI winner mismatches cannot pass across configurations'
    Assert-Throws {
        Assert-Stage5AiDeterminism @($validAiResults[0..2]) @('serial-1', 'parallel-1') 2
    } 'expected 4' 'AI matrix cannot pass with a missing worker/repeat result'
    Invoke-Stage5AiDeterminismGroupingFocusedCase
    $completeCrossProduct = @()
    foreach ($determinismKey in @('4v3-seed-1729', '4v2-seed-1729')) {
        foreach ($configuration in @('serial-1', 'parallel-1')) {
            foreach ($repeat in @(1, 2)) {
                $completeCrossProduct += New-AiResult $configuration $repeat `
                    'A1B2C3D4' 42000 1 $determinismKey
            }
        }
    }
    Assert-Stage5AiDeterminism $completeCrossProduct @('serial-1', 'parallel-1') 2 '' `
        @('4v3-seed-1729', '4v2-seed-1729')
    Assert-Throws {
        Assert-Stage5AiDeterminism @($completeCrossProduct | Select-Object -Skip 4) `
            @('serial-1', 'parallel-1') 2 '' @('4v3-seed-1729', '4v2-seed-1729')
    } 'complete 8-result' `
        'a fully missing scenario/seed case cannot evade per-case determinism checks'
    Assert-Throws {
        $duplicateCrossProduct = @($completeCrossProduct)
        $duplicateCrossProduct[0] = $duplicateCrossProduct[1]
        Assert-Stage5AiDeterminism $duplicateCrossProduct @('serial-1', 'parallel-1') 2 '' `
            @('4v3-seed-1729', '4v2-seed-1729')
    } 'duplicate scenario/seed/configuration/repeat' `
        'duplicate results cannot fill a missing cross-product position'

    $stressEntry = [pscustomobject]@{
        sequence = 20; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v2'
    }
    $authoritativeStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2') `
        $stressEntry ('A' * 64)
	Assert-True ($authoritativeStressEvidence.ordinaryPathAuthoritativeCommits -gt 0 -and
		$authoritativeStressEvidence.ordinaryPathWorkerExecutedRangeJobs -gt 1 -and
		$authoritativeStressEvidence.ordinaryPathDistinctPhysicalWorkers -gt 1) `
		'qualifying stress preserves independent ordinary-path physical-worker authority evidence'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-OrdinaryPathAuthoritativeCommits 0 `
				-OrdinaryPathAuthoritativeMultiWorkerCommits 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'no authoritative ordinary A\* batch' `
		'compact-direct worker authority cannot proxy missing ordinary A-star authority'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -CollisionPhysicalWorkerJobs 0 -CollisionOwnerHelpedJobs 4 `
                -CollisionPhysicalWorkerMask 0 `
                -CollisionDistinctPhysicalWorkers 0) `
            $stressEntry ('A' * 64) | Out-Null
    } 'at least two distinct physical collision workers' `
        'qualifying AI stress rejects collision jobs completed entirely by owner help'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -CollisionPhysicalWorkerJobs 4 -CollisionOwnerHelpedJobs 0 `
                -CollisionPhysicalWorkerMask 1 `
                -CollisionDistinctPhysicalWorkers 1) `
            $stressEntry ('A' * 64) | Out-Null
    } 'at least two distinct physical collision workers' `
        'qualifying AI stress rejects collision work confined to one physical worker'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -CollisionPhysicalWorkerJobs 4 -CollisionOwnerHelpedJobs 0 `
                -CollisionPhysicalWorkerMask 4 `
                -CollisionDistinctPhysicalWorkers 1) `
            $stressEntry ('A' * 64) | Out-Null
    } 'collision physical-worker mask exceeds the effective worker lane' `
        'AI collision evidence rejects a physical identity outside its configured lane'
	foreach ($lane in @(
		@{ Name = 'parallel-2'; Requested = '2'; Workers = 2 },
		@{ Name = 'parallel-4'; Requested = '4'; Workers = 4 },
		@{ Name = 'parallel-8'; Requested = '8'; Workers = 8 },
		@{ Name = 'parallel-16'; Requested = '16'; Workers = 16 },
		@{ Name = 'parallel-auto'; Requested = 'auto'; Workers = 16 }
	)) {
		$laneEntry = [pscustomobject]@{
			sequence = 500 + $lane.Workers
			configuration = $lane.Name
			simulationMode = 'parallel'
			requestedWorkers = $lane.Requested
			seed = 1729
			scenario = '4v2'
		}
		$baseParameters = @{
			Scenario = '4v2'; ActualAi = 6; ActualTeams = '4v2'
			RequestedWorkers = $lane.Requested; EffectiveWorkers = $lane.Workers
		}
		ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @baseParameters) `
			$laneEntry ('A' * 64) | Out-Null
		foreach ($badCollision in @(
			@{ CollisionAuthoritativeCommits = 0 },
			@{ CollisionCommittedCandidates = 0 },
			@{ CollisionOwnerFallbacks = 1 },
			@{ CollisionStaleRejections = 1 },
			@{ CollisionPhysicalWorkerMaskComplete = 0 }
		)) {
			$params = $baseParameters.Clone()
			foreach ($key in $badCollision.Keys) { $params[$key] = $badCollision[$key] }
			$expectedFailure = if ($badCollision.ContainsKey(
				'CollisionPhysicalWorkerMaskComplete')) {
				'collision physical-worker mask is incomplete inside the representable worker lane'
			} else {
				'qualifying parallel stress has no collision work'
			}
			Assert-Throws {
				ConvertFrom-Stage5AiCompletion (New-AiCompletionOutput @params) `
					$laneEntry ('A' * 64) | Out-Null
			} $expectedFailure `
				"$($lane.Name) rejects incomplete collision authority evidence"
		}
	}
	$aggregateWorkerEntry = [pscustomobject]@{
		sequence = 588; configuration = 'parallel-8'; simulationMode = 'parallel'
		requestedWorkers = '8'; seed = 1729; scenario = '4v2'; stress = $true
	}
	$aggregateWorkerParameters = @{
		Scenario = '4v2'; ActualAi = 6; ActualTeams = '4v2'
		RequestedWorkers = '8'; EffectiveWorkers = 8
		CollisionSubmitted = 16; CollisionCompleted = 16
		CollisionPhysicalWorkerJobs = 16; CollisionPhysicalWorkerMask = 255
		CollisionDistinctPhysicalWorkers = 7
		PhysicsSubmitted = 16; PhysicsCompleted = 16; PhysicsRanges = 8
		PhysicsPhysicalWorkerJobs = 16; PhysicsPhysicalWorkerMask = 255
		PhysicsDistinctPhysicalWorkers = 7; PhysicsPeakConcurrentPhysicalWorkers = 6
		StatusSubmitted = 16; StatusCompleted = 16
		StatusPhysicalWorkerJobs = 16; StatusPhysicalWorkerMask = 255
		StatusDistinctPhysicalWorkers = 7; StatusPeakConcurrentPhysicalWorkers = 6
		OrdinaryPathEligible = 24; OrdinaryPathSubmittedRequests = 20
		OrdinaryPathSubmittedRanges = 16
		OrdinaryPathWorkerExecutedRequests = 20
		OrdinaryPathWorkerExecutedRangeJobs = 16
		OrdinaryPathPhysicalWorkerMask = 255
		OrdinaryPathDistinctPhysicalWorkers = 7; OrdinaryPathPeakWorkers = 6
		OrdinaryPathMaximumBatchRequests = 8
		OrdinaryPathMaximumRangeCount = 8; OrdinaryPathMaximumGrainSize = 2
	}
	try {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput @aggregateWorkerParameters) `
			$aggregateWorkerEntry ('A' * 64) | Out-Null
		Assert-True $true `
			'AI evidence accepts epoch-wide worker unions larger than the maximum per-batch distinct count'
	}
	catch {
		Assert-True $false `
			"AI evidence rejected truthful aggregate worker identities: $($_.Exception.Message)"
	}
	$badAggregateWorkerParameters = $aggregateWorkerParameters.Clone()
	$badAggregateWorkerParameters.PhysicsPhysicalWorkerMask = 3
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput @badAggregateWorkerParameters) `
			$aggregateWorkerEntry ('A' * 64) | Out-Null
	} 'complete physics worker-union mask omits the maximum per-batch distinct count' `
		'AI evidence rejects a complete aggregate mask that omits a per-batch physical worker'
	$badOrdinaryAggregateParameters = $aggregateWorkerParameters.Clone()
	$badOrdinaryAggregateParameters.OrdinaryPathPhysicalWorkerMask = 3
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput @badOrdinaryAggregateParameters) `
			$aggregateWorkerEntry ('A' * 64) | Out-Null
	} 'complete ordinary-path worker-union mask omits the maximum per-batch distinct count' `
		'AI evidence rejects a complete ordinary-path union that omits a per-batch worker'
	$badAutoSpatialParameters = $aggregateWorkerParameters.Clone()
	$badAutoSpatialParameters.RequestedWorkers = 'auto'
	$badAutoSpatialParameters.SpatialCollectionPhysicalWorkerMask = 256
	$autoSpatialEntry = $aggregateWorkerEntry | Select-Object *
	$autoSpatialEntry.configuration = 'parallel-auto'
	$autoSpatialEntry.requestedWorkers = 'auto'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput @badAutoSpatialParameters) `
			$autoSpatialEntry ('A' * 64) | Out-Null
	} 'immutable-spatial physical-worker mask exceeds the effective worker lane' `
		'AI evidence rejects an immutable-spatial worker identity outside the actual lane'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PhysicsPhysicalWorkerJobs 3 -PhysicsOwnerHelpedJobs 1) `
			$stressEntry ('A' * 64) | Out-Null
	} 'qualifying parallel stress has no positive authoritative physics' `
		'qualifying AI stress rejects physics authority completed with owner help'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-StatusAuthoritativeBatches 3 -StatusCommittedCommands 96 `
				-StatusSubmitted 4 -StatusCompleted 4 -StatusPhysicalWorkerJobs 0 `
				-StatusOwnerHelpedJobs 4 -StatusPhysicalWorkerMask 0 `
				-StatusDistinctPhysicalWorkers 0 -StatusPeakConcurrentPhysicalWorkers 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'qualifying parallel stress has no physical live status authority' `
		'qualifying AI stress rejects advertised status authority without physical live execution'
    $spatialCollectionStressEntry = [pscustomobject]@{
        sequence = 24; configuration = 'parallel-2'; simulationMode = 'parallel'
        requestedWorkers = '2'; seed = 1729; scenario = '4v2'; stress = $true
    }
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -SpatialSuccessfulCollections 0 `
                -SpatialSuccessfulCollectionQueries 0 `
                -SpatialSuccessfulCollectionRanges 0 `
                -SpatialMultiRangeCollections 0 `
                -SpatialCollectionSubmitted 0 -SpatialCollectionCompleted 0 `
                -SpatialCollectionPhysical 0 `
                -SpatialMaximumCollectionQueries 0 `
                -SpatialMaximumCollectionRanges 0) `
            $spatialCollectionStressEntry ('A' * 64) | Out-Null
    } 'no positive multi-query, multi-range immutable-spatial collection evidence' `
        'qualifying AI stress cannot pass on single-query spatial worker submissions'
    foreach ($spatialLaneWorkers in @(2, 4, 8, 16)) {
        $spatialLaneEntry = [pscustomobject]@{
            sequence = 240 + $spatialLaneWorkers
            configuration = "parallel-$spatialLaneWorkers"
            simulationMode = 'parallel'
            requestedWorkers = [string]$spatialLaneWorkers
            seed = 1729
            scenario = '4v2'
            stress = $true
        }
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
                -ActualTeams '4v2' -RequestedWorkers ([string]$spatialLaneWorkers) `
                -EffectiveWorkers $spatialLaneWorkers) `
            $spatialLaneEntry ('A' * 64) | Out-Null
        $expectedSpatialRanges = [Math]::Min($spatialLaneWorkers, 5)
        $invalidSpatialRanges = if ($expectedSpatialRanges -eq 2) { 3 } else { 2 }
        Assert-Throws {
            ConvertFrom-Stage5AiCompletion `
                (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
                    -ActualTeams '4v2' `
                    -RequestedWorkers ([string]$spatialLaneWorkers) `
                    -EffectiveWorkers $spatialLaneWorkers `
                    -SpatialMaximumCollectionRanges $invalidSpatialRanges) `
                $spatialLaneEntry ('A' * 64) | Out-Null
        } 'maximum collection ranges do not match min' `
            "parallel-$spatialLaneWorkers spatial evidence must scale ranges with workers and queueable queries"
		if ($expectedSpatialRanges -ge 4) {
			Assert-Throws {
				ConvertFrom-Stage5AiCompletion `
					(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
						-ActualTeams '4v2' `
						-RequestedWorkers ([string]$spatialLaneWorkers) `
						-EffectiveWorkers $spatialLaneWorkers `
						-SpatialCollectionPhysicalWorkerMask 1 `
						-SpatialMaximumCollectionDistinctPhysicalWorkers 1) `
					$spatialLaneEntry ('A' * 64) | Out-Null
			} 'did not use more than one distinct physical worker' `
				"parallel-$spatialLaneWorkers spatial evidence rejects a multi-range wave executed by one physical worker"
		}
    }
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 `
				-ActualTeams '4v2' -RequestedWorkers '2' -EffectiveWorkers 2 `
				-SpatialCollectionPhysicalWorkerMask 4 `
				-SpatialMaximumCollectionDistinctPhysicalWorkers 1) `
			$spatialCollectionStressEntry ('A' * 64) | Out-Null
	} 'physical-worker mask exceeds the (?:explicit|effective) worker lane' `
		'physical spatial worker identities must remain inside the configured worker lane'
    Assert-Throws {
        ConvertFrom-Stage5AiCompletion `
            (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            $stressEntry ('A' * 64) | Out-Null
    } 'qualifying parallel stress has no positive authoritative physics' `
        'each qualifying parallel stress completion requires positive physics authority and jobs'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathPeakWorkers 1) $stressEntry ('A' * 64) | Out-Null
	} 'multi-worker direct-path authority without a multi-worker peak' `
		'per-record path authority rejects a sequential-worker peak before the qualifying stress aggregate gate'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathAuthoritativeMultiWorkerCommits 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'no multi-request direct-path batch backed by more than one physical path worker' `
		'qualifying path stress requires commit-backed per-batch multi-worker correlation'
    $shadowStressResult = [pscustomobject]@{
        sequence = 7; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'shadow-16'
        aiEvidence = $shadowCompletion
    }
    Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
        sequence = 20; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'parallel-2'
        aiEvidence = $authoritativeStressEvidence
    }, $shadowStressResult)
    Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'qualifying parallel stress has no collision work' `
		'positive AI work cannot substitute for collision authority in any qualifying execution'
	Assert-Throws {
		ConvertFrom-Stage5AiCompletion `
			(New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
				-PathWorkerExecuted 0 -PathAuthoritativeCommits 0) `
			$stressEntry ('A' * 64) | Out-Null
	} 'no multi-request direct-path batch backed by more than one physical path worker' `
		'AI and collision work cannot proxy direct-path worker authority'
    $noSpatialStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -SpatialHealingAuthoritative 0 -SpatialHealingCandidates 0 `
            -SpatialHealingSubmitted 0 -SpatialHealingCompleted 0 `
            -SpatialHealingPhysical 0 -SpatialPdlAuthoritative 0 `
            -SpatialPdlCandidates 0 -SpatialPdlSubmitted 0 `
            -SpatialPdlCompleted 0 -SpatialPdlPhysical 0) $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 25; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'parallel-2'
            aiEvidence = $noSpatialStressEvidence
        }, $shadowStressResult)
    } 'no authoritative immutable-spatial healing and point-defense-laser work' `
        'AI, collision, physics, and path work cannot proxy live spatial consumer authority'
    $noAiStressEvidence = ConvertFrom-Stage5AiCompletion `
        (New-AiCompletionOutput -Scenario '4v2' -ActualAi 6 -ActualTeams '4v2' `
            -AuthoritativeCommits 0 -AiCommittedBatches 5 `
            -AiSubmitted 0 -AiCompleted 0) `
        $stressEntry ('A' * 64)
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 21; kind = 'ai'; stress = $true; scenario = '4v2'; configuration = 'parallel-2'
            aiEvidence = $noAiStressEvidence
        }, $shadowStressResult)
    } 'global or shadow-only scheduler activity is insufficient' `
        'duplicate shadow/global jobs cannot satisfy authoritative Stage 5 simulation work'
    Assert-Throws {
        Assert-Stage5AuthoritativeWorkEvidence @([pscustomobject]@{
            sequence = 22; kind = 'ai'; stress = $false; scenario = '4v2'; configuration = 'parallel-2'
            aiEvidence = $authoritativeStressEvidence
        })
    } 'requires a parallel AI stress scenario' `
        'non-stress authoritative work cannot substitute for stress-scenario evidence'

    $replayEntry = [pscustomobject]@{
        sequence = 3; configuration = 'parallel-2'; simulationMode = 'parallel'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $true
    }
    $replayMetrics = ConvertFrom-Stage5ReplayMetrics (New-ReplayMetricOutput) $replayEntry
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialCapturedArenas 0) $replayEntry | Out-Null
    } 'has no captured immutable-spatial arena' `
        'parallel replay evidence rejects an all-zero immutable-spatial capture count'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialSuccessfulCollections 0 `
                -SpatialSuccessfulCollectionQueries 0 `
                -SpatialSuccessfulCollectionRanges 0 `
                -SpatialMultiRangeCollections 0 `
                -SpatialCollectionSubmitted 0 -SpatialCollectionCompleted 0 `
                -SpatialCollectionPhysical 0 `
                -SpatialMaximumCollectionQueries 0 `
                -SpatialMaximumCollectionRanges 0) $replayEntry | Out-Null
    } 'no positive multi-query, multi-range immutable-spatial collection evidence' `
        'qualifying replay stress cannot pass without a multi-query spatial collection'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialCollectionSubmitted 4 `
                -SpatialCollectionCompleted 3 -SpatialCollectionPhysical 3) `
            $replayEntry | Out-Null
    } 'collection jobs are not balanced physical-worker work' `
        'replay spatial collection evidence rejects incomplete job completion'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialSuccessfulCollectionRanges 1 `
                -SpatialCollectionSubmitted 2 -SpatialCollectionCompleted 2 `
                -SpatialCollectionPhysical 2 -SpatialMaximumCollectionRanges 1) `
            $replayEntry | Out-Null
    } 'does not prove multi-query, multi-range two-pass worker execution' `
        'replay spatial evidence rejects a one-range owner-wait submission'
    $serialReplayEntry = [pscustomobject]@{
        sequence = 66; configuration = 'serial-1'; simulationMode = 'serial'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $false
    }
    $serialReplayMetrics = ConvertFrom-Stage5ReplayMetrics `
        (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
            -Workers 0 -Submitted 0 -Executed 0 -Fallback 0) $serialReplayEntry
    Assert-True ($serialReplayMetrics.spatialEvidence.capturedArenas -eq 0) `
        'serial replay evidence accepts and requires zero immutable-spatial captures'
    Assert-True ($replayMetrics.workers -eq 2) 'replay metrics are parsed into structured evidence'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode 'corrupt') $replayEntry | Out-Null
    } 'effective_mode is not a supported' 'replay metrics reject an invalid effective-mode enum'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics 'replay completed without structured metrics' $replayEntry | Out-Null
    } 'exactly one SIMULATION_JOB_METRICS' 'replay cannot pass without structured job metrics'
    $completeReplayMetrics = New-ReplayMetricOutput
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (($completeReplayMetrics -split "`n" | Where-Object {
                $_ -notmatch '^PHYSICS_INTEGRATION_MANIFEST '
            }) -join "`n") $replayEntry | Out-Null
    } 'exactly one PHYSICS_INTEGRATION_MANIFEST' `
        'replay cannot pass without its per-replay physics delta manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            ($completeReplayMetrics + "`n" +
                (($completeReplayMetrics -split "`n" | Where-Object {
                    $_ -match '^PHYSICS_INTEGRATION_MANIFEST '
                }) -join "`n")) $replayEntry | Out-Null
    } 'exactly one PHYSICS_INTEGRATION_MANIFEST' `
        'replay cannot pass with duplicate physics lifecycle output'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (($completeReplayMetrics -split "`n" | Where-Object {
                $_ -notmatch '^IMMUTABLE_SPATIAL_MANIFEST '
            }) -join "`n") $replayEntry | Out-Null
    } 'exactly one IMMUTABLE_SPATIAL_MANIFEST' `
        'replay cannot pass without its per-replay immutable-spatial delta manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            ((New-ReplayMetricOutput).Replace(' requested_pipeline=serial', ' requested_pipeline=parallel')) `
            $replayEntry | Out-Null
    } 'honestly request' 'replay cannot conceal a parallel pipeline request'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionShadowMismatches 1) `
            $replayEntry | Out-Null
    } 'collision shadow mismatches' 'replay rejects collision shadow mismatch telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'unexpected collision owner fallbacks' `
        'replay rejects unexpected collision fallback telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionSubmitted 4 -CollisionCompleted 3) `
            $replayEntry | Out-Null
    } 'collision submitted/completed job counts do not match' `
        'replay collision evidence rejects incomplete successful-job telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionPhysicalWorkerJobs 0 `
                -CollisionOwnerHelpedJobs 4 -CollisionPhysicalWorkerMask 0 `
                -CollisionDistinctPhysicalWorkers 0) `
            $replayEntry | Out-Null
    } 'at least two distinct physical collision workers' `
        'qualifying replay stress rejects collision jobs completed entirely by owner help'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionPhysicalWorkerJobs 4 `
                -CollisionOwnerHelpedJobs 0 -CollisionPhysicalWorkerMask 1 `
                -CollisionDistinctPhysicalWorkers 1) `
            $replayEntry | Out-Null
    } 'at least two distinct physical collision workers' `
        'qualifying replay stress rejects collision work confined to one physical worker'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionPhysicalWorkerJobs 4 `
                -CollisionOwnerHelpedJobs 0 -CollisionPhysicalWorkerMask 4 `
                -CollisionDistinctPhysicalWorkers 1) `
            $replayEntry | Out-Null
    } 'collision physical-worker mask exceeds the effective worker lane' `
        'replay collision evidence rejects a physical identity outside its configured lane'
	foreach ($lane in @(
		@{ Name = 'parallel-2'; Workers = 2 },
		@{ Name = 'parallel-4'; Workers = 4 },
		@{ Name = 'parallel-8'; Workers = 8 },
		@{ Name = 'parallel-16'; Workers = 16 },
		@{ Name = 'parallel-auto'; Workers = 16 }
	)) {
		$laneEntry = [pscustomobject]@{
			sequence = 600 + $lane.Workers
			configuration = $lane.Name
			simulationMode = 'parallel'
			replayArgument = 'Stage5Validation\reference.rep'
			stress = $true
		}
		$baseParameters = @{ Workers = $lane.Workers }
		ConvertFrom-Stage5ReplayMetrics (New-ReplayMetricOutput @baseParameters) `
			$laneEntry | Out-Null
		foreach ($badCollision in @(
			@{ CollisionAuthoritativeCommits = 0 },
			@{ CollisionCommittedCandidates = 0 },
			@{ CollisionOwnerFallbacks = 1 },
			@{ CollisionStaleRejections = 1 },
			@{ CollisionPhysicalWorkerMaskComplete = 0 }
		)) {
			$params = $baseParameters.Clone()
			foreach ($key in $badCollision.Keys) { $params[$key] = $badCollision[$key] }
			$expectedFailure = if ($badCollision.ContainsKey(
				'CollisionPhysicalWorkerMaskComplete')) {
				'collision physical-worker mask is incomplete inside the representable worker lane'
			} else {
				'qualifying stress replay has no collision work'
			}
			Assert-Throws {
				ConvertFrom-Stage5ReplayMetrics (New-ReplayMetricOutput @params) `
					$laneEntry | Out-Null
			} $expectedFailure `
				"$($lane.Name) replay rejects incomplete collision authority evidence"
		}
	}
	$aggregateReplayEntry = [pscustomobject]@{
		sequence = 688; configuration = 'parallel-8'; simulationMode = 'parallel'
		replayArgument = 'Stage5Validation\reference.rep'; stress = $true
	}
	$aggregateReplayParameters = @{
		Workers = 8
		CollisionSubmitted = 16; CollisionCompleted = 16
		CollisionPhysicalWorkerJobs = 16; CollisionPhysicalWorkerMask = 255
		CollisionDistinctPhysicalWorkers = 7
		PhysicsSubmitted = 16; PhysicsCompleted = 16; PhysicsRanges = 8
		PhysicsPhysicalWorkerJobs = 16; PhysicsPhysicalWorkerMask = 255
		PhysicsDistinctPhysicalWorkers = 7; PhysicsPeakConcurrentPhysicalWorkers = 6
		StatusSubmitted = 16; StatusCompleted = 16
		StatusPhysicalWorkerJobs = 16; StatusPhysicalWorkerMask = 255
		StatusDistinctPhysicalWorkers = 7; StatusPeakConcurrentPhysicalWorkers = 6
	}
	try {
		ConvertFrom-Stage5ReplayMetrics `
			(New-ReplayMetricOutput @aggregateReplayParameters) `
			$aggregateReplayEntry | Out-Null
		Assert-True $true `
			'replay evidence accepts epoch-wide worker unions larger than the maximum per-batch distinct count'
	}
	catch {
		Assert-True $false `
			"replay evidence rejected truthful aggregate worker identities: $($_.Exception.Message)"
	}
	$badAggregateReplayParameters = $aggregateReplayParameters.Clone()
	$badAggregateReplayParameters.StatusPhysicalWorkerMask = 3
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			(New-ReplayMetricOutput @badAggregateReplayParameters) `
			$aggregateReplayEntry | Out-Null
	} 'complete status worker-union mask omits the maximum per-batch distinct count' `
		'replay evidence rejects a complete aggregate mask that omits a per-batch physical worker'
	$badReplaySpatialParameters = $aggregateReplayParameters.Clone()
	$badReplaySpatialParameters.SpatialCollectionPhysicalWorkerMask = 256
	$autoReplaySpatialEntry = $aggregateReplayEntry | Select-Object *
	$autoReplaySpatialEntry.configuration = 'parallel-auto'
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			(New-ReplayMetricOutput @badReplaySpatialParameters) `
			$autoReplaySpatialEntry | Out-Null
	} 'immutable-spatial physical-worker mask exceeds the effective worker lane' `
		'replay evidence rejects an immutable-spatial worker identity outside the actual lane'
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			(New-ReplayMetricOutput -PhysicsPhysicalWorkerJobs 3 `
				-PhysicsOwnerHelpedJobs 1) $replayEntry | Out-Null
	} 'qualifying stress replay has no positive authoritative physics' `
		'qualifying replay rejects physics authority completed with owner help'
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			(New-ReplayMetricOutput -StatusAuthoritativeBatches 3 `
				-StatusCommittedCommands 96 -StatusSubmitted 4 -StatusCompleted 4 `
				-StatusPhysicalWorkerJobs 0 -StatusOwnerHelpedJobs 4 `
				-StatusPhysicalWorkerMask 0 -StatusDistinctPhysicalWorkers 0 `
				-StatusPeakConcurrentPhysicalWorkers 0) $replayEntry | Out-Null
	} 'qualifying stress replay has no physical live status authority' `
		'qualifying replay rejects advertised status authority without physical live execution'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -CollisionShadowExecutions 1 `
                -CollisionShadowComparedCandidates 1) $replayEntry | Out-Null
    } 'collision shadow work outside shadow' `
        'parallel replay evidence rejects stale collision shadow counters from another mode'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
                -Workers 0 -Submitted 0 -Executed 0 `
                -CollisionAuthoritativeCommits 1 -CollisionCommittedCandidates 1 `
                -CollisionPreparedPairs 1 -CollisionUniqueCandidates 1 `
                -CollisionSubmitted 1 -CollisionCompleted 1 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0) `
            ([pscustomobject]@{
                sequence = 62; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'collision authority outside parallel' `
        'serial replay evidence rejects stale collision authority from another mode'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 0 `
                -Workers 0 -Submitted 0 -Executed 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0 `
                -CollisionPreparedPairs 24 -CollisionUniqueCandidates 12 `
                -CollisionSubmitted 4 -CollisionCompleted 4) `
            ([pscustomobject]@{
                sequence = 64; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'serial replay reports collision lane work' `
        'serial replay rejects stale prepared collision work even with zero authority'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsShadowExecutions 1 `
                -PhysicsShadowMismatches 1) `
            $replayEntry | Out-Null
    } 'forbidden physics evidence' 'replay rejects physics shadow mismatch telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'forbidden physics evidence' 'replay rejects unexpected physics fallback telemetry'
    try {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsOwnerFallbacks 4 `
                -PhysicsStaleRejections 2) $replayEntry | Out-Null
        Assert-True $true `
            'replay accepts owner-safe physics fallbacks including stale rejections'
    }
    catch {
        Assert-True $false `
            "replay rejected owner-safe physics fallback evidence: $($_.Exception.Message)"
    }
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsOwnerFallbacks 1 `
                -PhysicsStaleRejections 2) $replayEntry | Out-Null
    } 'more stale physics rejections than owner fallbacks' `
        'replay rejects impossible physics fallback accounting'
    $noneligibleReplayPhysicsFallbackCases = @(
        [pscustomobject]@{
            name = 'serial'
            entry = [pscustomobject]@{
                sequence = 65; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }
            output = New-ReplayMetricOutput -Mode serial -EffectiveMode serial `
                -Scheduler 0 -Workers 0 -Submitted 0 -Executed 0 `
                -PhysicsOwnerFallbacks 2 -PhysicsStaleRejections 1
        },
        [pscustomobject]@{
            name = 'parallel-1'
            entry = [pscustomobject]@{
                sequence = 66; configuration = 'parallel-1'; simulationMode = 'parallel'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }
            output = New-ReplayMetricOutput -Workers 1 `
                -PhysicsOwnerFallbacks 2 -PhysicsStaleRejections 1
        },
        [pscustomobject]@{
            name = 'shadow'
            entry = [pscustomobject]@{
                sequence = 67; configuration = 'shadow-2'; simulationMode = 'shadow'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }
            output = New-ReplayMetricOutput -Mode shadow -EffectiveMode shadow `
                -Workers 2 -PhysicsOwnerFallbacks 2 -PhysicsStaleRejections 1
        }
    )
    foreach ($fallbackCase in $noneligibleReplayPhysicsFallbackCases) {
        Assert-Throws {
            ConvertFrom-Stage5ReplayMetrics $fallbackCase.output `
                $fallbackCase.entry | Out-Null
        } 'physics owner fallback evidence outside an eligible multiworker parallel lane' `
            "replay rejects cross-mode physics fallback evidence in $($fallbackCase.name)"
    }
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialHealingSubmitted 8 `
                -SpatialHealingCompleted 7 -SpatialHealingPhysical 7) `
            $replayEntry | Out-Null
    } 'healing immutable-spatial jobs are not balanced' `
        'replay rejects incomplete healing spatial jobs'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -SpatialPdlUnexpectedFallbacks 1) `
            $replayEntry | Out-Null
    } 'forbidden pdl immutable-spatial evidence' `
        'replay rejects unexpected PDL spatial fallback telemetry'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsAuthoritativeBatches 0 `
                -PhysicsCommittedPrefixes 0 -PhysicsRanges 0 `
                -PhysicsSubmitted 0 -PhysicsCompleted 0) $replayEntry | Out-Null
    } 'qualifying stress replay has no positive authoritative physics' `
        'qualifying stress replay rejects an all-zero physics manifest'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -PhysicsShadowExecutions 1 `
                -PhysicsShadowPrefixes 24 -PhysicsShadowRanges 2 `
                -PhysicsShadowSubmitted 2 -PhysicsShadowCompleted 2) `
            $replayEntry | Out-Null
    } 'physics shadow work outside shadow' `
        'parallel replay evidence rejects stale physics shadow counters from another mode'
    $replayOneEntry = [pscustomobject]@{
        sequence = 4; configuration = 'parallel-1'; simulationMode = 'parallel'
        replayArgument = 'Stage5Validation\reference.rep'; stress = $true
    }
	$replayOneZeroPhysicsOutput = New-ReplayMetricOutput `
		-EffectiveMode parallel -Scheduler 1 -Workers 1 `
            -Submitted 0 -Executed 0 -Fallback 4 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
			-CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0
	$replayOneMetrics = ConvertFrom-Stage5ReplayMetrics `
		$replayOneZeroPhysicsOutput $replayOneEntry
    Assert-True $replayOneMetrics.expectedOneWorkerFallback `
        'replay parser accepts the expected forced one-worker kernel fallback with a live scheduler'
	Assert-Throws {
		ConvertFrom-Stage5ReplayMetrics `
			($replayOneZeroPhysicsOutput.Replace('capture_ns=0', 'capture_ns=1')) `
			$replayOneEntry | Out-Null
	} 'reports physics pre-scan, capture, or storage work' `
		'forced one-worker replay evidence rejects physics capture work before scheduler preflight'
    $replayOneCollisionFallbackMetrics = ConvertFrom-Stage5ReplayMetrics `
        (New-ReplayMetricOutput -EffectiveMode parallel -Scheduler 1 -Workers 1 `
            -Submitted 0 -Executed 0 -Fallback 4 -CollisionOwnerFallbacks 3 `
            -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
            -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
            -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
        $replayOneEntry
    Assert-True ($replayOneCollisionFallbackMetrics.collisionOwnerFallbacks -eq 3) `
        'replay parser accepts owner-only collision fallback in the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode serial -Scheduler 0 -Workers 0 `
                -Submitted 0 -Executed 0 -Fallback 4 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
            $replayOneEntry | Out-Null
    } 'explicit parallel scheduler unexpectedly fell back' `
        'forced one-worker replay cannot conceal a scheduler startup failure as kernel fallback'
    Assert-Throws {
        $nonStressReplayEntry = [pscustomobject]@{
            sequence = 65; configuration = 'parallel-2'; simulationMode = 'parallel'
            replayArgument = 'Stage5Validation\reference.rep'; stress = $false
        }
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -EffectiveMode serial -Scheduler 0 -Workers 0 -Submitted 0 -Executed 0 -Fallback 4) `
            $nonStressReplayEntry | Out-Null
    } 'unexpectedly fell back' 'replay parser rejects fallback outside the forced one-worker lane'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Workers 1) `
            ([pscustomobject]@{
                sequence = 5; configuration = 'parallel-4'; simulationMode = 'parallel'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'scheduler/worker count does not match' `
        'parallel-4 replay metrics cannot pass with one actual worker'
    Assert-Throws {
        ConvertFrom-Stage5ReplayMetrics `
            (New-ReplayMetricOutput -Mode serial -EffectiveMode serial -Scheduler 1 -Workers 1 `
                -PhysicsAuthoritativeBatches 0 -PhysicsCommittedPrefixes 0 `
                -PhysicsRanges 0 -PhysicsSubmitted 0 -PhysicsCompleted 0 `
                -CollisionAuthoritativeCommits 0 -CollisionCommittedCandidates 0) `
            ([pscustomobject]@{
                sequence = 6; configuration = 'serial-1'; simulationMode = 'serial'
                replayArgument = 'Stage5Validation\reference.rep'; stress = $false
            }) | Out-Null
    } 'serial configuration reports an active scheduler' `
        'serial replay metrics cannot report an active scheduler, worker, or jobs'

    $resultOne = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput) $replayEntry
    $resultTwo = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput) $replayEntry
    Assert-Stage5ReplayDeterminism @(
        [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
        [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultTwo }
    )
    Assert-Throws {
        $different = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalCRC 'DEADBEEF') $replayEntry
        Assert-Stage5ReplayDeterminism @(
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $different }
        )
    } 'final_crc differs' 'replay final CRC mismatches cannot pass across worker configurations'
    Assert-Throws {
        $different = ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalFrame 42001) $replayEntry
        Assert-Stage5ReplayDeterminism @(
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $resultOne },
            [pscustomobject]@{ kind = 'replay'; determinismKey = 'reference'; replayResult = $different }
        )
    } 'final_frame differs' 'replay final-frame mismatches cannot pass across worker configurations'
    Assert-Throws {
        ConvertFrom-Stage5ReplayResult 'replay completed with timing only' $replayEntry | Out-Null
    } 'exactly one SIMULATION_REPLAY_RESULT' 'timing-only replay evidence cannot pass as deterministic state'
    Assert-Throws {
        ConvertFrom-Stage5ReplayResult (New-ReplayResultOutput -FinalCRC 'not-a-crc') $replayEntry | Out-Null
    } 'final_crc' 'malformed authoritative replay CRC fails closed'

    $timingDirectory = Join-Path $root 'valid-timing'
    New-Item -ItemType Directory -Path $timingDirectory | Out-Null
    $timingFile = Join-Path $timingDirectory 'frame-timing-1-1.csv'
    Write-TimingFixture $timingFile
    $timingEvidence = Get-Stage5TimingEvidence $timingDirectory 'valid fixture'
    Assert-True ($timingEvidence.rows -eq 2 -and $timingEvidence.maximumFrameEnd -eq 42000) `
        'timing evidence validates required rows and final frame'
    Assert-True ($timingEvidence.sha256 -ceq (Get-Sha256 $timingFile)) `
        'timing evidence records the exact CSV hash'
    $originalCulture = [Threading.Thread]::CurrentThread.CurrentCulture
    try {
        [Threading.Thread]::CurrentThread.CurrentCulture =
            [Globalization.CultureInfo]::GetCultureInfo('de-DE')
        $cultureTimingEvidence = Get-Stage5TimingEvidence $timingDirectory `
            'invariant-culture timing fixture'
        $framePhase = @($cultureTimingEvidence.phaseSummaries | Where-Object {
            $_.phase -ceq 'frame'
        })[0]
        Assert-True ([Math]::Abs($framePhase.totalMilliseconds - 90.0) -lt 0.001) `
            'phase totals retain invariant decimal semantics under a non-English CurrentCulture'
    }
    finally {
        [Threading.Thread]::CurrentThread.CurrentCulture = $originalCulture
    }
    Assert-Throws {
        Assert-Stage5CollisionTimingEvidence $timingEvidence `
            ([pscustomobject]@{ collisionAuthoritativeCommits = 3; collisionShadowExecutions = 0 }) `
            'missing collision phases fixture'
    } 'missing timing phase' 'authoritative collision work requires all collision timing phases'
    $collisionTimingDirectory = Join-Path $root 'collision-timing'
    New-Item -ItemType Directory -Path $collisionTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $collisionTimingDirectory 'frame-timing-1-7.csv') `
        -CollisionPhases -CollisionShadowPhase
    $collisionTimingEvidence = Get-Stage5TimingEvidence $collisionTimingDirectory `
        'collision timing fixture'
    Assert-Stage5CollisionTimingEvidence $collisionTimingEvidence `
        ([pscustomobject]@{ collisionAuthoritativeCommits = 3; collisionShadowExecutions = 1 }) `
        'collision timing fixture'
    Assert-True ($collisionTimingEvidence.phaseSummaries.Count -ge 12) `
        'collision timing evidence retains separate admission/validation/filter/prepare/compare phase totals'
    Assert-Throws {
        Get-Stage5TimingEvidence (Join-Path $root 'missing-timing-directory') `
            'missing timing fixture' | Out-Null
    } 'timing directory is missing' 'missing timing evidence cannot pass the gate'

    $badHeaderDirectory = Join-Path $root 'bad-header-timing'
    New-Item -ItemType Directory -Path $badHeaderDirectory | Out-Null
    Write-TimingFixture (Join-Path $badHeaderDirectory 'frame-timing-1-2.csv') -BadHeader
    Assert-Throws {
        Get-Stage5TimingEvidence $badHeaderDirectory 'bad header fixture' | Out-Null
    } 'header is invalid' 'timing CSV with a changed header fails closed'
    $emptyTimingDirectory = Join-Path $root 'empty-timing'
    New-Item -ItemType Directory -Path $emptyTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $emptyTimingDirectory 'frame-timing-1-3.csv') -HeaderOnly
    Assert-Throws {
        Get-Stage5TimingEvidence $emptyTimingDirectory 'empty fixture' | Out-Null
    } 'contains no data rows' 'header-only timing CSV fails closed'
    $missingPhaseDirectory = Join-Path $root 'missing-phase-timing'
    New-Item -ItemType Directory -Path $missingPhaseDirectory | Out-Null
    Write-TimingFixture (Join-Path $missingPhaseDirectory 'frame-timing-1-4.csv') -MissingLogic
    Assert-Throws {
        Get-Stage5TimingEvidence $missingPhaseDirectory 'missing phase fixture' | Out-Null
    } 'frame and logic phases' 'timing CSV missing required phases fails closed'
    $interactiveTimingDirectory = Join-Path $root 'interactive-timing'
    New-Item -ItemType Directory -Path $interactiveTimingDirectory | Out-Null
    Write-TimingFixture (Join-Path $interactiveTimingDirectory 'frame-timing-1-5.csv') -Interactive
    Assert-Throws {
        Get-Stage5TimingEvidence $interactiveTimingDirectory 'interactive mode fixture' | Out-Null
    } 'must identify the headless validation mode' `
        'interactive timing rows cannot satisfy the headless validation gate'
    foreach ($nonFiniteTiming in @('NaN', '1e309')) {
        $nonFiniteTimingDirectory = Join-Path $root ("non-finite-timing-$nonFiniteTiming")
        New-Item -ItemType Directory -Path $nonFiniteTimingDirectory | Out-Null
        Write-TimingFixture (Join-Path $nonFiniteTimingDirectory 'frame-timing-1-6.csv') `
            -WallMilliseconds $nonFiniteTiming
        Assert-Throws {
            Get-Stage5TimingEvidence $nonFiniteTimingDirectory `
                "non-finite timing fixture $nonFiniteTiming" | Out-Null
        } 'invalid wall_ms' "timing CSV rejects non-finite decimal '$nonFiniteTiming'"
    }

    Assert-Throws {
        & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest `
            -OutputRoot (Join-Path $root 'timing-disabled-acceptance') -ValidationSet Replay `
            -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly -DisableFrameTiming | Out-Null
    } 'only with DiagnosticNonAcceptance' 'acceptance plan cannot disable frame timing'
    $diagnosticOutput = Join-Path $root 'timing-disabled-diagnostic'
    & $scriptPath -RuntimeRoot $runtime -FixtureManifestPath $manifest -OutputRoot $diagnosticOutput `
        -ValidationSet Replay -MinimumFreeBytes 1 -AllowNonStandardCorpus -PlanOnly `
        -DisableFrameTiming -DiagnosticNonAcceptance | Out-Null
    $diagnosticPlan = Get-Content -LiteralPath (Join-Path $diagnosticOutput 'validation-plan.json') -Raw |
        ConvertFrom-Json
    Assert-True (-not $diagnosticPlan.deterministicRuntimeEligible -and
        -not $diagnosticPlan.finalAcceptanceEligible -and -not $diagnosticPlan.frameTimingRequired) `
        'timing-disabled plan is explicitly marked focused diagnostic evidence'

    $stage3Baseline = [pscustomobject]@{
        executableSha256 = ('B' * 64); physicalCoreCount = 16; availableCpus = 16
        fileSha256 = ('C' * 64); wallMilliseconds = @(1000, 100, 100, 100)
        measuredMedianMilliseconds = 100.0; file = 'baseline-source.json'
        evidenceFile = 'stage3-performance-baseline.json'; expectedExecutableSha256 = ('B' * 64)
    }
    $performanceResults = New-Object 'Collections.Generic.List[object]'
    $sequence = 0
    foreach ($sample in @(10000, 100, 102, 98, 101)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-1' (++$sequence) $sample 1)) | Out-Null
    }
    foreach ($sample in @(1, 40, 41, 39, 40)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-8' (++$sequence) $sample 8)) | Out-Null
    }
    foreach ($sample in @(1, 30, 31, 29, 30)) {
        $performanceResults.Add((New-PerformanceResult 'parallel-16' (++$sequence) $sample 16)) | Out-Null
    }
    $performance = Measure-Stage5Performance $performanceResults.ToArray() 16 $stage3Baseline 1 3
    Assert-True ($performance.status -ceq 'passed') 'robust median performance fixture passes approved targets'
    Assert-True ($performance.measurementScope -ceq 'aggregate-stage5-stress-replay-throughput' -and
        -not $performance.collisionSpecificSpeedupClaim) `
        'performance report scopes 2x throughput to aggregate Stage 5 replay work, not collision speedup'
    Assert-True ($performance.currentOneWorker.medianWallMilliseconds -lt 200) `
        'performance median excludes the warm-up outlier'
    Assert-True (($performance.currentOneWorker.rawWallMilliseconds -join ',') -ceq `
        '10000,100,102,98,101') 'performance report retains current raw samples including warm-up'
    Assert-True ($performance.independentlyExpectedStage3ExecutableSha256 -ceq ('B' * 64)) `
        'performance report records independently supplied Stage 3 provenance'
    $zeroWarmup = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-1' })
    foreach ($sample in @(0, 100, 102, 98, 101)) {
        $zeroWarmup += New-PerformanceResult 'parallel-1' (++$sequence) $sample 1
    }
    Assert-Throws {
        Measure-Stage5Performance $zeroWarmup 16 $stage3Baseline 1 3 | Out-Null
    } 'non-positive wall time' 'performance warm-up samples must be valid positive evidence'
    $mixedTopology = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-8' })
    foreach ($sample in @(
        [pscustomobject]@{ wall = 1; cpus = 16 },
        [pscustomobject]@{ wall = 40; cpus = 16 },
        [pscustomobject]@{ wall = 41; cpus = 32 },
        [pscustomobject]@{ wall = 39; cpus = 16 },
        [pscustomobject]@{ wall = 40; cpus = 16 }
    )) {
        $mixedTopology += New-PerformanceResult 'parallel-8' (++$sequence) $sample.wall 8 $sample.cpus
    }
    Assert-Throws {
        Measure-Stage5Performance $mixedTopology 16 $stage3Baseline 1 3 | Out-Null
    } 'topology varies across measured runs' `
        'one 32-CPU sample among 16-CPU samples cannot be hidden by topology minima'

    $slowEight = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-8' })
    foreach ($sample in @(1, 60, 61, 59, 60)) {
        $slowEight += New-PerformanceResult 'parallel-8' (++$sequence) $sample 8
    }
    Assert-True ((Measure-Stage5Performance $slowEight 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'sub-2x eight-worker median throughput fails'

    $slowSixteen = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-16' })
    foreach ($sample in @(1, 45, 46, 44, 45)) {
        $slowSixteen += New-PerformanceResult 'parallel-16' (++$sequence) $sample 16
    }
    Assert-True ((Measure-Stage5Performance $slowSixteen 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'non-positive eight-to-sixteen scaling fails on a capable host'

    $regressedOne = @($performanceResults.ToArray() | Where-Object { $_.configuration -cne 'parallel-1' })
    foreach ($sample in @(1, 106, 107, 105, 106)) {
        $regressedOne += New-PerformanceResult 'parallel-1' (++$sequence) $sample 1
    }
    Assert-True ((Measure-Stage5Performance $regressedOne 16 $stage3Baseline 1 3).status -ceq 'failed') `
        'one-worker regression above five percent fails'
    Assert-True ((Measure-Stage5Performance $performanceResults.ToArray() 7 $stage3Baseline 1 3).status `
        -ceq 'unsupported-host-topology') 'smaller hosts report performance as unsupported'
    $mismatchedBaseline = [pscustomobject]@{
        executableSha256 = ('B' * 64); physicalCoreCount = 8; availableCpus = 8
        fileSha256 = ('C' * 64); wallMilliseconds = @(1000, 100, 100, 100)
        measuredMedianMilliseconds = 100.0; file = 'baseline-source.json'
        evidenceFile = 'stage3-performance-baseline.json'; expectedExecutableSha256 = ('B' * 64)
    }
    Assert-True ((Measure-Stage5Performance $performanceResults.ToArray() 16 $mismatchedBaseline 1 3).status `
        -ceq 'failed') 'Stage 3 baseline from a different machine topology cannot pass'
    Assert-Throws {
        Measure-Stage5Performance $performanceResults.ToArray() 16 $null 1 3 | Out-Null
    } 'baseline evidence is required' 'performance cannot pass without Stage 3 baseline evidence'
    Assert-Throws {
        Read-Stage5PerformanceBaseline '' $stressHash ('B' * 64) | Out-Null
    } 'BaselinePath is required' 'performance cannot start without an explicit Stage 3 baseline input'

    $invalidBaselinePath = Join-Path $root 'invalid-stage3-baseline.json'
    [IO.File]::WriteAllText($invalidBaselinePath, ([ordered]@{
        schemaVersion = 1; stage = 'Stage3'; architecture = 'x64'; executableSha256 = 'not-a-hash'
        fixtureSha256 = $stressHash; configuration = 'parallel-1'; physicalCoreCount = 16
        availableCpus = 16; warmupRuns = 1
        wallMilliseconds = @(1000, 100, 100, 100)
    } | ConvertTo-Json -Depth 4))
    Assert-Throws {
        Read-Stage5PerformanceBaseline $invalidBaselinePath $stressHash ('B' * 64) | Out-Null
    } 'exact executable SHA-256' 'Stage 3 baseline without exact candidate provenance fails closed'

    $validBaselinePath = Join-Path $root 'valid-stage3-baseline.json'
    [IO.File]::WriteAllText($validBaselinePath, ([ordered]@{
        schemaVersion = 1; stage = 'Stage3'; architecture = 'x64'; executableSha256 = ('B' * 64)
        fixtureSha256 = $stressHash; configuration = 'parallel-1'; physicalCoreCount = 16
        availableCpus = 16; warmupRuns = 1; wallMilliseconds = @(1000, 100, 100, 100)
    } | ConvertTo-Json -Depth 4))
    Assert-Throws {
        Read-Stage5PerformanceBaseline $validBaselinePath $stressHash ('C' * 64) | Out-Null
    } 'independently supplied expected hash' 'self-asserted Stage 3 executable provenance cannot pass'

    foreach ($nonFiniteJson in @('NaN', '1e309')) {
        $nonFiniteBaselinePath = Join-Path $root ("non-finite-stage3-$nonFiniteJson.json")
        $nonFiniteBaseline = @"
{"schemaVersion":1,"stage":"Stage3","architecture":"x64","executableSha256":"$('B' * 64)","fixtureSha256":"$stressHash","configuration":"parallel-1","physicalCoreCount":16,"availableCpus":16,"warmupRuns":1,"wallMilliseconds":[1000,$nonFiniteJson,100,100]}
"@
        [IO.File]::WriteAllText($nonFiniteBaselinePath, $nonFiniteBaseline)
        Assert-Throws {
            Read-Stage5PerformanceBaseline $nonFiniteBaselinePath $stressHash ('B' * 64) | Out-Null
        } 'finite JSON numbers|Invalid JSON primitive' `
            "Stage 3 baseline rejects non-finite JSON sample '$nonFiniteJson'"
    }

    $restored = New-Object 'Collections.Generic.List[int]'
    $restoreError = ''
    try {
        Invoke-Stage5RegistryRestore -Snapshots @(
            [pscustomobject]@{ id = 1 }, [pscustomobject]@{ id = 2 }, [pscustomobject]@{ id = 3 }
        ) -RestoreAction {
            param($snapshot)
            $restored.Add([int]$snapshot.id) | Out-Null
            if ($snapshot.id -eq 2 -or $snapshot.id -eq 3) {
                throw "injected restore failure $($snapshot.id)"
            }
        }
    }
    catch { $restoreError = $_.Exception.Message }
    Assert-True ($restoreError -match 'after attempting every snapshot' -and
        $restoreError -match 'injected restore failure 3' -and
        $restoreError -match 'injected restore failure 2') `
        'registry restore aggregates every injected failure'
    Assert-True (($restored.ToArray() -join ',') -ceq '3,2,1') `
        'registry restoration continues in reverse order after an injected failure'
    Assert-True (Test-Stage5RegistryLeafRemoval $false @() @()) `
        'absent-key snapshot removes its validation-created leaf after restoring the added value'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $true @() @())) `
        'pre-existing empty key is preserved during restoration'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $false @('concurrent-value') @())) `
        'validation-created key with concurrent value content is preserved'
    Assert-True (-not (Test-Stage5RegistryLeafRemoval $false @() @('concurrent-subkey'))) `
        'validation-created key with concurrent subkey content is preserved'
    $createdHierarchy = @(
        'Software\Electronic Arts',
        'Software\Electronic Arts\EA Games',
        'Software\Electronic Arts\EA Games\Validation Product'
    )
    $removedHierarchy = New-Object 'Collections.Generic.List[string]'
    Invoke-Stage5CreatedRegistryKeyCleanup $createdHierarchy -InspectAction {
        param($createdSubKey)
        return [pscustomobject]@{ valueNames = @(); subKeyNames = @() }
    } -RemoveAction {
        param($createdSubKey)
        $removedHierarchy.Add($createdSubKey) | Out-Null
    }
    Assert-True (($removedHierarchy.ToArray() -join '|') -ceq `
        (($createdHierarchy[2], $createdHierarchy[1], $createdHierarchy[0]) -join '|')) `
        'fixture starting with only HKCU Software removes the full validation-created hierarchy in reverse'
    $cleanupAttempts = New-Object 'Collections.Generic.List[string]'
    Assert-Throws {
        Invoke-Stage5CreatedRegistryKeyCleanup $createdHierarchy -InspectAction {
            return [pscustomobject]@{ valueNames = @(); subKeyNames = @() }
        } -RemoveAction {
            param($createdSubKey)
            $cleanupAttempts.Add($createdSubKey) | Out-Null
            if ($createdSubKey -ceq $createdHierarchy[2]) {
                throw 'injected leaf cleanup failure'
            }
        }
    } 'after attempting every created key.*injected leaf cleanup failure' `
        'created-key cleanup aggregates a removal failure after attempting later ancestors'
    Assert-True (($cleanupAttempts.ToArray() -join '|') -ceq `
        (($createdHierarchy[2], $createdHierarchy[1], $createdHierarchy[0]) -join '|')) `
        'created-key cleanup continues reverse attempts after one removal failure'

    $privacyState = [pscustomobject]@{
        value = 'private-original-value'
        registered = New-Object 'Collections.Generic.List[object]'
    }
    $setupPipelineOutput = @(Invoke-Stage5RegistrySetupTransaction @('Software') `
        -ActionContext $privacyState -EnsureSubKeyAction { return $false } `
        -CaptureValueAction {
        param($created, $state)
        return [pscustomobject]@{ oldValue = $state.value; createdSubKeys = @($created) }
    } -SetValueAction {
        param($snapshot, $state)
        $state.value = 'validation'
    } -RegisterSnapshotAction {
        param($snapshot, $state)
        $state.registered.Add($snapshot) | Out-Null
    } -RestoreValueAction { } -CleanupCreatedSubKeysAction { })
    Assert-True ($setupPipelineOutput.Count -eq 0 -and $privacyState.registered.Count -eq 1 -and
        $privacyState.registered[0].oldValue -ceq 'private-original-value') `
        'transactional registry setup registers its private snapshot without leaking it to pipeline output'

    $segmentFailureState = [pscustomobject]@{
        value = 'original'
        created = New-Object 'Collections.Generic.List[string]'
        removed = New-Object 'Collections.Generic.List[string]'
        registered = New-Object 'Collections.Generic.List[object]'
    }
    $segmentSetupError = ''
    try {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts',
            'Software\Electronic Arts\EA Games') -ActionContext $segmentFailureState `
            -EnsureSubKeyAction {
            param($subKey, $state)
            $state.created.Add($subKey) | Out-Null
            return $true
        } -CaptureValueAction {
            param($created, $state)
            return [pscustomobject]@{ hadValue = $true; oldValue = $state.value }
        } -SetValueAction {
            param($snapshot, $state)
            $state.value = 'validation'
        } -RegisterSnapshotAction {
            param($snapshot, $state)
            $state.registered.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $state)
            $state.value = $snapshot.oldValue
        } -CleanupCreatedSubKeysAction {
            param($created, $state)
            for ($index = $created.Count - 1; $index -ge 0; --$index) {
                $state.removed.Add($created[$index]) | Out-Null
                $state.created.Remove($created[$index]) | Out-Null
            }
        } -AfterSegmentAction {
            param($subKey, $createdCount)
            if ($createdCount -eq 2) { throw 'injected failure after two segments' }
        } | Out-Null
    }
    catch { $segmentSetupError = $_.Exception.Message }
    Assert-True ($segmentSetupError -match 'injected failure after two segments' -and
        $segmentSetupError -match 'rollback: completed') `
        'registry setup reports the injected segment failure and completed rollback'
    Assert-True ($segmentFailureState.created.Count -eq 0 -and
        ($segmentFailureState.removed.ToArray() -join '|') -ceq
        'Software\Electronic Arts\EA Games|Software\Electronic Arts') `
        'registry setup failure after two segments removes every created ancestor in reverse'

    $valueFailureState = [pscustomobject]@{
        value = 'original'
        created = New-Object 'Collections.Generic.List[string]'
        removed = New-Object 'Collections.Generic.List[string]'
        registered = New-Object 'Collections.Generic.List[object]'
        outerRestored = New-Object 'Collections.Generic.List[int]'
    }
    $valueFailureState.registered.Add([pscustomobject]@{ id = 99 }) | Out-Null
    $valueSetupError = ''
    try {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts',
            'Software\Electronic Arts\EA Games') -ActionContext $valueFailureState `
            -EnsureSubKeyAction {
            param($subKey, $state)
            $state.created.Add($subKey) | Out-Null
            return $true
        } -CaptureValueAction {
            param($created, $state)
            return [pscustomobject]@{ hadValue = $true; oldValue = $state.value }
        } -SetValueAction {
            param($snapshot, $state)
            $state.value = 'validation'
        } -RegisterSnapshotAction {
            param($snapshot, $state)
            $state.registered.Add($snapshot) | Out-Null
        } -RestoreValueAction {
            param($snapshot, $state)
            $state.value = $snapshot.oldValue
        } -CleanupCreatedSubKeysAction {
            param($created, $state)
            for ($index = $created.Count - 1; $index -ge 0; --$index) {
                $state.removed.Add($created[$index]) | Out-Null
                $state.created.Remove($created[$index]) | Out-Null
            }
        } -AfterValueWriteAction {
            throw 'injected failure after value write'
        } | Out-Null
    }
    catch { $valueSetupError = $_.Exception.Message }
    Assert-True ($valueSetupError -match 'injected failure after value write' -and
        $valueFailureState.value -ceq 'original' -and $valueFailureState.created.Count -eq 0) `
        'registry setup failure after value write restores the original value and created hierarchy'
    Invoke-Stage5RegistryRestore -Snapshots @($valueFailureState.registered.ToArray()) `
        -RestoreAction {
        param($snapshot)
        $valueFailureState.outerRestored.Add([int]$snapshot.id) | Out-Null
    }
    Assert-True (($valueFailureState.outerRestored.ToArray() -join ',') -ceq '99') `
        'a failed transactional setup leaves prior snapshots registered for the outer finally restore'

    Assert-Throws {
        Invoke-Stage5RegistrySetupTransaction @('Software\Electronic Arts') `
            -EnsureSubKeyAction { return $true } `
            -CaptureValueAction { return [pscustomobject]@{ hadValue = $false } } `
            -SetValueAction { } -RegisterSnapshotAction { } -RestoreValueAction { } `
            -CleanupCreatedSubKeysAction { throw 'injected rollback cleanup failure' } `
            -AfterSegmentAction { throw 'injected setup failure' } | Out-Null
    } 'setup: injected setup failure.*rollback: created-key cleanup: injected rollback cleanup failure' `
        'registry setup transaction aggregates setup and rollback errors'

    }
    if ($runAcceptance) {
    Assert-Stage5FinalAcceptanceEvidenceSchemaContract
    $deterministicRunnerSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot `
        'Run-DeterministicSimulationValidation.ps1') -Raw
    Assert-True ($deterministicRunnerSource -notmatch
            '\[IO\.File\]::WriteAllText' -and
        $deterministicRunnerSource -match
            'function Write-Stage5TextFileAtomically' -and
        $deterministicRunnerSource -match
            "'Validation results checkpoint' -ReplaceExisting") `
        'Accepted deterministic plan/results/performance/stdout/stderr artifacts must use the durable atomic publisher.'
    $atomicReplaceRoot = Join-Path $root 'atomic-replace-ps5'
    New-Item -ItemType Directory -Path $atomicReplaceRoot -Force | Out-Null
    $atomicReplacePath = Join-Path $atomicReplaceRoot 'checkpoint.json'
    [IO.File]::WriteAllText($atomicReplacePath, '{"generation":1}')
    $replacementBytes = [Text.UTF8Encoding]::new($false).GetBytes(
        '{"generation":2}')
    $replacementSnapshot = Write-Stage5FinalAcceptanceFileAtomically `
        -Path $atomicReplacePath -Bytes $replacementBytes `
        -Context 'PowerShell 5.1 atomic replacement regression' -ReplaceExisting
    Assert-True ([IO.File]::ReadAllText($atomicReplacePath) -ceq
            '{"generation":2}' -and
        $replacementSnapshot.sha256 -ceq (Get-Sha256 $atomicReplacePath)) `
        'replace-existing publication works through the native durable atomic path'
    # Final acceptance is deliberately separate from the deterministic-runtime
    # replay/AI matrix. Build one complete, independently hashed diagnostic v1
    # evidence set, then prove that v1 is rejected until lockstep-v2 exists,
    # while missing, stale, tampered, and combined-policy-invalid inputs fail
    # closed for their own reasons.
    $acceptanceRoot = Join-Path $root 'final-acceptance'
    $artifactFiles = Join-Path $acceptanceRoot 'artifact-files'
    $attachmentRoot = Join-Path $acceptanceRoot 'attachments'
    New-Item -ItemType Directory -Path $artifactFiles, $attachmentRoot -Force | Out-Null
    $sourceCommit = 'a' * 40
    $artifactRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifactEntries = @()
    $artifactTestHashes = @{}
    foreach ($role in $artifactRoles) {
        $artifactRelativePath = switch ($role) {
            'generals-executable' { 'lockstep-v2-positive\GeneralsRuntime\generalsv.exe' }
            'generals-launcher' { 'lockstep-v2-positive\GeneralsRuntime\launcher.exe' }
            'generals-launcher-config' { 'lockstep-v2-positive\GeneralsRuntime\launcher.lcf' }
            'zerohour-executable' { 'lockstep-v2-positive\ZeroHourRuntime\generalszh.exe' }
            'zerohour-launcher' { 'lockstep-v2-positive\ZeroHourRuntime\launcher.exe' }
            'zerohour-launcher-config' { 'lockstep-v2-positive\ZeroHourRuntime\launcher.lcf' }
        }
        $path = Join-Path $acceptanceRoot $artifactRelativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "artifact:$role")
        $artifactHash = Get-Sha256 $path
        $artifactEntries += [ordered]@{
            role = $role
            path = $artifactRelativePath
            sha256 = $artifactHash
        }
        $artifactTestHashes[$role] = $artifactHash
    }
    # The artifact set is not only the six launcher-facing files.  Bind a
    # complete installed-runtime closure (DLLs/assets plus its manifest) so a
    # same-hash executable with caller-selected sidecars cannot qualify.
    $runtimeClosureFiles = @()
    foreach ($closureEntry in @(
        [pscustomobject]@{ title = 'Generals'; kind = 'dll';
            path = 'lockstep-v2-positive\GeneralsRuntime\stage5-runtime.dll' },
        [pscustomobject]@{ title = 'Generals'; kind = 'asset';
            path = 'lockstep-v2-positive\GeneralsRuntime\stage5-runtime-assets.dat' },
        [pscustomobject]@{ title = 'ZeroHour'; kind = 'dll';
            path = 'lockstep-v2-positive\ZeroHourRuntime\stage5-runtime.dll' },
        [pscustomobject]@{ title = 'ZeroHour'; kind = 'asset';
            path = 'lockstep-v2-positive\ZeroHourRuntime\stage5-runtime-assets.dat' }
    )) {
        $closurePath = Join-Path $acceptanceRoot $closureEntry.path
        New-Item -ItemType Directory -Path (Split-Path -Parent $closurePath) -Force | Out-Null
        [IO.File]::WriteAllText($closurePath,
            "runtime-closure:$($closureEntry.title):$($closureEntry.kind)")
        $runtimeClosureFiles += [ordered]@{
            title = $closureEntry.title; kind = $closureEntry.kind
            path = $closureEntry.path; sha256 = Get-Sha256 $closurePath
        }
    }
    foreach ($artifactEntry in $artifactEntries) {
        $artifactTitle = if ([string]$artifactEntry.role -like 'generals-*') {
            'Generals'
        }
        else { 'ZeroHour' }
        $artifactKind = switch -Wildcard ([string]$artifactEntry.role) {
            '*-executable' { 'executable' }
            '*-launcher' { 'launcher' }
            '*-launcher-config' { 'launcher-config' }
        }
        $runtimeClosureFiles += [ordered]@{
            title = $artifactTitle; kind = $artifactKind
            path = [string]$artifactEntry.path
            sha256 = [string]$artifactEntry.sha256
        }
    }
    $runtimeClosureFiles = @($runtimeClosureFiles | Sort-Object title, kind, path)
    $runtimeClosureCanonicalLines = @($runtimeClosureFiles | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.kind,
            ([string]$_.path).Replace('\', '/'),
            ([string]$_.sha256).ToUpperInvariant()
    })
    [Array]::Sort($runtimeClosureCanonicalLines, [StringComparer]::Ordinal)
    $runtimeClosureHash = Get-Sha256Text (($runtimeClosureCanonicalLines -join "`n") + "`n")
    $runtimeClosureManifestPath = Join-Path $acceptanceRoot 'runtime-closure.json'
    Write-JsonDocument $runtimeClosureManifestPath ([ordered]@{
        schemaVersion = 1; sourceCommit = $sourceCommit
        productSet = @('Generals', 'ZeroHour'); architecture = 'x64'
        files = $runtimeClosureFiles
    })
    $script:TestRuntimeClosure = [ordered]@{
        dependencyManifestSha256 = Get-Sha256 $runtimeClosureManifestPath
        closureSha256 = $runtimeClosureHash
    }
    $artifactSetPath = Join-Path $acceptanceRoot 'artifact-set.json'
    Write-JsonDocument $artifactSetPath ([ordered]@{
        schemaVersion = 1
        sourceCommit = $sourceCommit
        productSet = @('Generals', 'ZeroHour')
        architecture = 'x64'
        runtimeClosure = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'runtime-closure.json'
                sha256 = $script:TestRuntimeClosure.dependencyManifestSha256
            }
            closureSha256 = $script:TestRuntimeClosure.closureSha256
        }
        artifacts = $artifactEntries
    })
    $artifactSetHash = Get-Sha256 $artifactSetPath
    $runtimeRoleBinding = Get-Stage5RuntimeClosureBinding `
        (ConvertFrom-Stage5TestJsonDictionary $artifactSetPath) `
        $acceptanceRoot $sourceCommit `
        'Runtime role semantic positive'
    Assert-True ($runtimeRoleBinding.fileCount -eq 10) `
        'runtime closure binds all six core artifact roles to the complete installed closure'

    $runtimeClosureOriginalBytes = [IO.File]::ReadAllBytes($runtimeClosureManifestPath)
    try {
        $titleMutation = ConvertFrom-Stage5TestJsonDictionary $runtimeClosureManifestPath
        $generalsExecutableFile = @($titleMutation['files'] | Where-Object {
            [string]$_['path'] -ceq
                'lockstep-v2-positive\GeneralsRuntime\generalsv.exe'
        })[0]
        $zeroHourExecutableFile = @($titleMutation['files'] | Where-Object {
            [string]$_['path'] -ceq
                'lockstep-v2-positive\ZeroHourRuntime\generalszh.exe'
        })[0]
        $generalsExecutableFile['title'] = 'ZeroHour'
        $zeroHourExecutableFile['title'] = 'Generals'
        Write-JsonDocument $runtimeClosureManifestPath $titleMutation
        $titleArtifactSet = ConvertFrom-Stage5TestJsonDictionary $artifactSetPath
        $titleArtifactSet['runtimeClosure']['dependencyManifest']['sha256'] =
            Get-Sha256 $runtimeClosureManifestPath
        $titleArtifactSet['runtimeClosure']['closureSha256'] =
            Get-TestRuntimeClosureSha256 $titleMutation['files']
        Assert-Throws {
            Get-Stage5RuntimeClosureBinding $titleArtifactSet $acceptanceRoot `
                $sourceCommit 'Runtime role title negative' | Out-Null
        } 'exact title and kind' `
            'runtime closure rejects core roles relabeled with the opposite title after a complete rehash'
    }
    finally {
        [IO.File]::WriteAllBytes($runtimeClosureManifestPath,
            $runtimeClosureOriginalBytes)
    }

    try {
        $kindMutation = ConvertFrom-Stage5TestJsonDictionary $runtimeClosureManifestPath
        $generalsExecutableFile = @($kindMutation['files'] | Where-Object {
            [string]$_['path'] -ceq
                'lockstep-v2-positive\GeneralsRuntime\generalsv.exe'
        })[0]
        $generalsLauncherFile = @($kindMutation['files'] | Where-Object {
            [string]$_['path'] -ceq
                'lockstep-v2-positive\GeneralsRuntime\launcher.exe'
        })[0]
        $generalsExecutableFile['kind'] = 'launcher'
        $generalsLauncherFile['kind'] = 'executable'
        Write-JsonDocument $runtimeClosureManifestPath $kindMutation
        $kindArtifactSet = ConvertFrom-Stage5TestJsonDictionary $artifactSetPath
        $kindArtifactSet['runtimeClosure']['dependencyManifest']['sha256'] =
            Get-Sha256 $runtimeClosureManifestPath
        $kindArtifactSet['runtimeClosure']['closureSha256'] =
            Get-TestRuntimeClosureSha256 $kindMutation['files']
        Assert-Throws {
            Get-Stage5RuntimeClosureBinding $kindArtifactSet $acceptanceRoot `
                $sourceCommit 'Runtime role kind negative' | Out-Null
        } 'exact title and kind' `
            'runtime closure rejects core roles relabeled with the wrong kind after a complete rehash'
    }
    finally {
        [IO.File]::WriteAllBytes($runtimeClosureManifestPath,
            $runtimeClosureOriginalBytes)
    }
    $artifactTestPaths = @{}
    foreach ($artifactEntry in $artifactEntries) {
        $artifactTestPaths[[string]$artifactEntry.role] =
            [IO.Path]::GetFullPath((Join-Path $acceptanceRoot ([string]$artifactEntry.path)))
    }
    if ($focusedQualificationData) {
        $focusedPath = Join-Path $attachmentRoot `
            'deterministic-runtime-validation-results.json'
        Write-Stage5HostReceiptTestDocument $focusedPath 'validation-results' `
            'ZeroHour' $sourceCommit $artifactSetHash $artifactTestHashes
        $focusedRelocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
            -Path $focusedPath -EvidenceDirectory $attachmentRoot
        $focusedDocument = ConvertFrom-Stage5TestJsonDictionary $focusedPath
        $focusedArguments = @{
            Path = $focusedPath
            Kind = 'deterministic-runtime'
            Role = 'validation-results'
            EvidenceTitle = 'ZeroHour'
            ExpectedSourceCommit = $sourceCommit
            ExpectedArtifactSetSha256 = $artifactSetHash
            ArtifactHashes = $artifactTestHashes
            ExpectedCohortNonce = $script:TestCohortNonce
            ExpectedCohortCreatedUtc = $script:TestCohortCreatedUtc
            ExpectedRuntimeClosure = $script:TestRuntimeClosure
            # Exercise the production expected-binding path: the immutable
            # receipt reader must compare the binding without replacing its
            # raw JSON dictionary with the typed comparison proof.
            ExpectedQualificationData = $focusedDocument['details']['qualificationData']
            NativeRelocationBindings = @($focusedRelocation.children)
        }
        $focusedRead = Read-Stage5FinalAcceptanceImmutableReceipt `
            @focusedArguments
        Assert-True ($focusedRead.qualificationData -is [Collections.IDictionary] -and
            $null -ne $focusedRead.qualificationDataEvidence -and
            [string]$focusedRead.qualificationDataEvidence.manifestSha256 -ceq
                [string]$focusedRead.qualificationData.manifestSha256 -and
            [string]$focusedRead.qualificationDataEvidence.closureSha256 -ceq
                [string]$focusedRead.qualificationData.closureSha256 -and
            [int]$focusedRead.qualificationDataEvidence.fileCount -eq 6) `
            'focused qualification-data acceptance proof retained a typed manifest binding'
        Write-Output 'Focused Stage 5 qualification-data acceptance proof passed.'
        return
    }
    # Combined host-runner acceptance consumes the same complete, title-scoped
    # 253-child corpus that deterministic-runtime validation authenticates.  A
    # synthetic corpus is deliberately rooted beside the acceptance fixtures;
    # it is test evidence only and never a production authority.
    $syntheticCorpusRoot = Join-Path $acceptanceRoot 'synthetic-corpus'
    $syntheticGenerals = New-Stage5SyntheticAcceptanceCorpus `
        (Join-Path $syntheticCorpusRoot 'Generals') 'Generals' $sourceCommit `
        $artifactSetHash $artifactTestHashes `
        $artifactTestPaths['generals-executable']
    $syntheticZeroHour = New-Stage5SyntheticAcceptanceCorpus `
        (Join-Path $syntheticCorpusRoot 'ZeroHour') 'ZeroHour' $sourceCommit `
        $artifactSetHash $artifactTestHashes `
        $artifactTestPaths['zerohour-executable']
    $requiredAttachmentRoles = [ordered]@{
        # Attachment identity is the composite role/title key.  Replay
        # qualification has one reviewed fixture receipt per title; retaining
        # only the role would make the two closures alias one another.
        'replay-determinism' = @('replay-results|ZeroHour',
            'replay-fixture-manifest|Generals',
            'replay-fixture-manifest|ZeroHour')
        'fresh-ai' = @('ai-results')
        'performance-scaling' = @('stage3-baseline', 'phase-baseline-profile',
            'performance-report')
        'mixed-worker-multiplayer' = @('multiplayer-results')
        'combined-stage4-stage5-installed-runtime' = @('combined-results')
        'premium-review' = @('premium-review-results')
        'manual-acceptance' = @('manual-checklist')
        'deterministic-runtime' = @('validation-plan', 'validation-results', 'performance-report')
    }
    $detailsByKind = [ordered]@{
        'replay-determinism' = [ordered]@{
            uniqueReplayCount = 10; executionCount = 168; matrixPasses = 2
            stressExecutionsPerConfiguration = 6
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            allExecutionsPassed = $true; deterministicAcrossWorkers = $true
        }
        'fresh-ai' = [ordered]@{
            scenarios = @('4v3', '4v2'); distinctSeeds = 3; repeats = 2
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            freshGames = $true; allGamesCompleted = $true
            deterministicAcrossWorkers = $true
        }
        'performance-scaling' = [ordered]@{
            physicalCoreCount = 16; oneWorkerRegressionRatio = 1.01
            eightWorkerSpeedup = 2.1; sixteenWorkerStatus = 'passed'
            eightToSixteenSpeedup = 1.1
        }
        'mixed-worker-multiplayer' = [ordered]@{
            nativeEvidenceKind = 'lockstep-v2-multiplayer'
            producer = 'installed-lockstep-v2'
            nativeEvidenceSha256 = 'A' * 64
            networkRosterMask = 3; simulationRosterMask = 63; aiRosterMask = 60
            aiPlayerCount = 4; title = 'Both'; sessionCount = 2; peerCount = 2
            commonStopFrame = 4096; allMatchesCompleted = $true
            stateTracesIdentical = $true
            crossEpochRejected = $true; contentMismatchRejected = $true
        }
        'combined-stage4-stage5-installed-runtime' = [ordered]@{
            installedRuntime = $true; pipelineMode = 'parallel'; simulationMode = 'parallel'
            workerPolicy = 'auto'; renderer = 'd3d11'; renderThread = 'dedicated'
            bothTitlesPassed = $true
        }
        'premium-review' = [ordered]@{
            reviewedCommit = $sourceCommit; reviewRounds = 2; independentReviewers = 9
            completeDiffReviewed = $true; fixesRetested = $true
            openP0 = 0; openP1 = 0; openP2 = 0
        }
        'manual-acceptance' = [ordered]@{
            approvalScope = 'final-stage5-installed-runtime'; approvedByUser = $true
            candidateHashVerified = $true; bothTitlesTested = $true
            graphicsPassed = $true; audioPassed = $true; inputPassed = $true
            saveLoadPassed = $true; largeMatchPassed = $true; cleanExitPassed = $true
        }
    }
    $evidenceDocuments = @{}
    $evidencePaths = @{}
    $evidenceHashes = @{}
    $performanceFixtureBinding = $null
    foreach ($kind in @($detailsByKind.Keys)) {
        $attachments = @()
        foreach ($attachmentSpec in $requiredAttachmentRoles[$kind]) {
            $bindingParts = ([string]$attachmentSpec) -split '\|', 2
            $role = [string]$bindingParts[0]
            $attachmentTitle = if ($bindingParts.Count -eq 2) {
                [string]$bindingParts[1]
            }
            elseif ($kind -in @('mixed-worker-multiplayer',
                    'combined-stage4-stage5-installed-runtime',
                    'premium-review', 'manual-acceptance')) {
                'Both'
            }
            else { 'ZeroHour' }
            $leaf = if ($role -ceq 'replay-fixture-manifest') {
                "$kind-$role-$attachmentTitle.json"
            }
            else { "$kind-$role.json" }
            $attachmentPath = Join-Path $attachmentRoot $leaf
            if ($kind -ceq 'mixed-worker-multiplayer' -and $role -ceq 'multiplayer-results') {
                Write-Net3LoopbackTestManifest $attachmentPath $sourceCommit $artifactSetHash `
                    $artifactTestHashes['generals-executable'] `
                    $artifactTestHashes['zerohour-executable']
                # Keep the v2 details fixture structurally complete even though
                # this attachment is intentionally diagnostic NET3 v1 and is
                # rejected before final acceptance can consume the binding.
                $detailsByKind[$kind].nativeEvidenceSha256 = Get-Sha256 $attachmentPath
            }
            elseif ($kind -ceq 'performance-scaling' -and $role -ceq 'performance-report') {
                $performanceFixtureBinding = Write-PerformanceScalingTestManifest `
                    -Path $attachmentPath `
                    -SourceCommit $sourceCommit `
                    -ArtifactSetSha256 $artifactSetHash `
                    -ExecutableSha256 $artifactTestHashes['zerohour-executable'] `
                    -ArtifactSetManifestPath $artifactSetPath `
                    -Stage3BaselineOutputPath (Join-Path $attachmentRoot `
                        "$kind-stage3-baseline.json") `
                    -PhaseBaselineProfileOutputPath (Join-Path $attachmentRoot `
                        "$kind-phase-baseline-profile.json")
                $detailsByKind[$kind].physicalCoreCount = 16
                $detailsByKind[$kind].oneWorkerRegressionRatio =
                    $performanceFixtureBinding.maximumOneWorkerRegressionRatio
                $detailsByKind[$kind].eightWorkerSpeedup =
                    $performanceFixtureBinding.minimumEightWorkerSpeedup
                $detailsByKind[$kind].eightToSixteenSpeedup =
                    $performanceFixtureBinding.minimumEightToSixteenSpeedup
                $detailsByKind[$kind].sixteenWorkerStatus = 'passed'
                foreach ($priorAttachment in @($attachments | Where-Object {
                            $_.role -in @('stage3-baseline',
                                'phase-baseline-profile')
                        })) {
                    $priorAttachment.sha256 = Get-Sha256 (Join-Path `
                        $acceptanceRoot ([string]$priorAttachment.path))
                }
            }
            elseif ($role -eq 'replay-fixture-manifest') {
                $reviewed = if ($attachmentTitle -ceq 'Generals') {
                    $syntheticGenerals.reviewed
                }
                elseif ($attachmentTitle -ceq 'ZeroHour') {
                    $syntheticZeroHour.reviewed
                }
                else {
                    throw "No synthetic reviewed fixture corpus exists for title '$attachmentTitle'."
                }
                # Keep the attachment bound to the exact reviewed receipt and
                # its protected manifest/attestation closure emitted by the
                # synthetic title corpus.  The path is intentionally rooted in
                # test evidence, never a production authority.
                $attachmentPath = [string]$reviewed.receiptPath
            }
            elseif ($role -eq 'premium-review-results') {
                Write-Stage5ExternalReceiptTestDocument $attachmentPath 'premium-review' `
                    $role 'premium-review' $sourceCommit $artifactSetHash
            }
            elseif ($role -eq 'manual-checklist') {
                Write-Stage5ExternalReceiptTestDocument $attachmentPath 'manual-acceptance' `
                    $role 'manual-approval' $sourceCommit $artifactSetHash
            }
            elseif ($role -in @('validation-plan', 'validation-results',
                'replay-results', 'ai-results', 'combined-results',
                'performance-report')) {
                if ($kind -eq 'combined-stage4-stage5-installed-runtime') {
                    $combinedSourceRoot = Join-Path $attachmentRoot 'combined-source-receipts'
                    if (-not (Test-Path -LiteralPath $combinedSourceRoot -PathType Container)) {
                        New-Item -ItemType Directory -Path $combinedSourceRoot -Force | Out-Null
                    }
                    $combinedGeneralsSource = [string]$syntheticGenerals.validationReceiptPath
                    $combinedZeroHourSource = [string]$syntheticZeroHour.validationReceiptPath
                    & (Join-Path $PSScriptRoot 'New-Stage5CombinedHostRunnerReceipt.ps1') `
                        -GeneralsReceiptPath $combinedGeneralsSource `
                        -ZeroHourReceiptPath $combinedZeroHourSource `
                        -GeneralsReviewedFixtureReceiptPath $syntheticGenerals.reviewed.receiptPath `
                        -GeneralsReviewedFixtureReceiptSha256 $syntheticGenerals.reviewed.receiptSha256 `
                        -ZeroHourReviewedFixtureReceiptPath $syntheticZeroHour.reviewed.receiptPath `
                        -ZeroHourReviewedFixtureReceiptSha256 $syntheticZeroHour.reviewed.receiptSha256 `
                        -OutputPath $attachmentPath `
                        -ExpectedSourceCommit $sourceCommit `
                        -ExpectedArtifactSetSha256 $artifactSetHash `
                        -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
                        -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] `
                        -ExpectedCohortNonce $script:TestCohortNonce `
                        -ExpectedCohortCreatedUtc $script:TestCohortCreatedUtc | Out-Null
                    $combinedWriterShape = Read-TestJson $attachmentPath
                    foreach ($sourcePath in @($combinedGeneralsSource,
                            $combinedZeroHourSource)) {
                        $sourceWriterShape = Read-TestJson $sourcePath
                        $sourceChild = @($sourceWriterShape.provenance.children)[0]
                        $combinedChild = @($combinedWriterShape.provenance.children |
                            Where-Object { $_.title -ceq $sourceWriterShape.title })[0]
                        Assert-True ($sourceChild.runNonce -cne
                                $sourceWriterShape.runNonce -and
                            $sourceChild.runNonce -ceq
                                $sourceChild.nativeReceipt.runNonce -and
                            $combinedChild.runNonce -ceq $sourceChild.runNonce -and
                            $combinedChild.nativeReceipt.runNonce -ceq
                                $sourceChild.runNonce) `
                            'Combined producer must preserve writer-shaped distinct wrapper and actual child/native nonces.'
                    }
                }
                else {
                    Write-Stage5HostReceiptTestDocument $attachmentPath $role 'ZeroHour' `
                        $sourceCommit $artifactSetHash $artifactTestHashes
                }
            }
            else {
                [IO.File]::WriteAllText($attachmentPath, "{`"evidence`":`"$kind/$role`"}")
            }
            $attachmentRelativePath = if ($role -ceq 'replay-fixture-manifest') {
                ConvertTo-OutputRelativePath $attachmentPath $acceptanceRoot `
                    'Synthetic acceptance attachment'
            }
            else { "attachments\$leaf" }
            $attachments += [ordered]@{
                role = $role
                title = $attachmentTitle
                path = $attachmentRelativePath
                sha256 = Get-Sha256 $attachmentPath
                trustDomain = switch ($role) {
                    'replay-fixture-manifest' { 'reviewed-fixture' }
                    'premium-review-results' { 'premium-review' }
                    'manual-checklist' { 'manual-approval' }
                    'stage3-baseline' { 'reviewed-fixture' }
                    'phase-baseline-profile' { 'reviewed-fixture' }
                    default { 'host-runner' }
                }
            }
        }
        $title = if ($kind -in @('replay-determinism',
            'mixed-worker-multiplayer',
            'combined-stage4-stage5-installed-runtime', 'premium-review',
            'manual-acceptance')) { 'Both' } else { 'ZeroHour' }
        $document = [ordered]@{
            schemaVersion = 1; evidenceKind = $kind; status = 'passed'
            sourceCommit = $sourceCommit; title = $title; architecture = 'x64'
            artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-01T00:00:00Z'
            cohortNonce = $script:TestCohortNonce
            runtimeClosure = $script:TestRuntimeClosure
            attachments = $attachments; details = $detailsByKind[$kind]
        }
        $path = Join-Path $acceptanceRoot "$kind.json"
        Write-JsonDocument $path $document
        $evidenceDocuments[$kind] = $document
        $evidencePaths[$kind] = $path
        $evidenceHashes[$kind] = Get-Sha256 $path
    }

    # The combined-results producer is an independent host-runner boundary. It
    # must consume two already-produced title receipts, not synthesize a Both
    # claim from caller-supplied details. Exercise the producer itself with
    # forged, single-title, stale, executable-hash, and nonce substitutions.
    $combinedProducerScript = Join-Path $PSScriptRoot `
        'New-Stage5CombinedHostRunnerReceipt.ps1'
    $combinedSourceRoot = Join-Path $attachmentRoot 'combined-source-receipts'
    # The combined producer consumes the canonical source receipts emitted by
    # the reusable synthetic corpus.  Keep these bindings pointed at the same
    # title-qualified documents used by the positive producer invocation above;
    # do not recreate a one-child or legacy-named source document here.
    $combinedGeneralsSource = [string]$syntheticGenerals.validationReceiptPath
    $combinedZeroHourSource = [string]$syntheticZeroHour.validationReceiptPath
    function Invoke-CombinedHostProducerTestCase {
        param(
            [string]$CaseRoot,
            [string]$ExpectedGeneralsHash = $artifactTestHashes['generals-executable'],
            [string]$ExpectedZeroHourHash = $artifactTestHashes['zerohour-executable']
        )
        $caseSourceRoot = Join-Path $CaseRoot 'combined-source-receipts'
        $caseGeneralsRoot = Join-Path $caseSourceRoot 'Generals'
        $caseZeroHourRoot = Join-Path $caseSourceRoot 'ZeroHour'
        & $combinedProducerScript `
            -GeneralsReceiptPath (Join-Path $caseGeneralsRoot `
                'validation-results-receipt.json') `
            -ZeroHourReceiptPath (Join-Path $caseZeroHourRoot `
                'validation-results-receipt.json') `
            -GeneralsReviewedFixtureReceiptPath (Join-Path $caseGeneralsRoot `
                'reviewed\receipt.json') `
            -GeneralsReviewedFixtureReceiptSha256 (Get-Sha256 (Join-Path `
                $caseGeneralsRoot 'reviewed\receipt.json')) `
            -ZeroHourReviewedFixtureReceiptPath (Join-Path $caseZeroHourRoot `
                'reviewed\receipt.json') `
            -ZeroHourReviewedFixtureReceiptSha256 (Get-Sha256 (Join-Path `
                $caseZeroHourRoot 'reviewed\receipt.json')) `
            -OutputPath (Join-Path $CaseRoot 'combined-results.json') `
            -ExpectedSourceCommit $sourceCommit `
            -ExpectedArtifactSetSha256 $artifactSetHash `
            -ExpectedGeneralsExecutableSha256 $ExpectedGeneralsHash `
            -ExpectedZeroHourExecutableSha256 $ExpectedZeroHourHash `
            -ExpectedCohortNonce $script:TestCohortNonce `
            -ExpectedCohortCreatedUtc $script:TestCohortCreatedUtc | Out-Null
    }
    function New-CombinedHostProducerTestCase {
        param([string]$Name)
        $caseRoot = Join-Path $acceptanceRoot "combined-producer-negative-$Name"
        $caseSourceRoot = Join-Path $caseRoot 'combined-source-receipts'
        New-Item -ItemType Directory -Path $caseSourceRoot -Force | Out-Null
        foreach ($template in @(
            [pscustomobject]@{
                title = 'Generals'
                sourceRoot = [IO.Path]::GetFullPath(
                    (Join-Path $syntheticCorpusRoot 'Generals'))
                corpus = $syntheticGenerals
            }
            [pscustomobject]@{
                title = 'ZeroHour'
                sourceRoot = [IO.Path]::GetFullPath(
                    (Join-Path $syntheticCorpusRoot 'ZeroHour'))
                corpus = $syntheticZeroHour
            }
        )) {
            if (-not (Test-Path -LiteralPath $template.sourceRoot -PathType Container)) {
                throw "Pristine $($template.title) synthetic corpus template is missing."
            }
            $templateItem = Get-Item -LiteralPath $template.sourceRoot -Force
            if (($templateItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Pristine $($template.title) synthetic corpus template is a reparse point."
            }
            if (-not [String]::Equals(
                    [IO.Path]::GetFullPath([string]$template.corpus.sourceRoot),
                    $template.sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
                @($template.corpus.children).Count -ne 253 -or
                [int]$template.corpus.rawLogCount -ne 507) {
                throw "Pristine $($template.title) synthetic corpus template identity or cardinality changed before copying."
            }
            if ((Get-Sha256 ([string]$template.corpus.validationReceiptPath)) -cne
                    [string]$template.corpus.validationReceiptSha256) {
                throw "Pristine $($template.title) synthetic corpus template receipt changed before copying."
            }
            $destinationRoot = Join-Path $caseSourceRoot $template.title
            if (Test-Path -LiteralPath $destinationRoot) {
                throw "Combined producer test destination is not fresh: $destinationRoot"
            }
            Copy-Item -LiteralPath $template.sourceRoot -Destination $destinationRoot `
                -Recurse -Force
            $destinationItem = Get-Item -LiteralPath $destinationRoot -Force
            if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Copied $($template.title) synthetic corpus destination became a reparse point."
            }
            $sourceFiles = @(Get-ChildItem -LiteralPath $template.sourceRoot `
                -Recurse -File -Force | ForEach-Object {
                $_.FullName.Substring($template.sourceRoot.Length + 1).Replace('/', '\')
            } | Sort-Object)
            $destinationFiles = @(Get-ChildItem -LiteralPath $destinationRoot `
                -Recurse -File -Force | ForEach-Object {
                $_.FullName.Substring($destinationRoot.Length + 1).Replace('/', '\')
            } | Sort-Object)
            if ($sourceFiles.Count -ne $destinationFiles.Count -or
                @((Compare-Object $sourceFiles $destinationFiles)).Count -ne 0) {
                throw "Copied $($template.title) synthetic corpus paths differ from the pristine template."
            }
            foreach ($relativePath in $sourceFiles) {
                $sourceFile = Get-Item -LiteralPath (Join-Path $template.sourceRoot $relativePath) -Force
                $destinationFile = Get-Item -LiteralPath (Join-Path $destinationRoot $relativePath) -Force
                if ($sourceFile.Length -ne $destinationFile.Length) {
                    throw "Copied $($template.title) synthetic corpus file length changed: $relativePath"
                }
            }
        }
        return $caseRoot
    }
    $combinedForgedRoot = New-CombinedHostProducerTestCase 'forged'
    $combinedForgedSource = Join-Path $combinedForgedRoot `
        'combined-source-receipts\Generals\validation-results-receipt.json'
    $combinedForgedDocument = Get-Content -LiteralPath $combinedForgedSource -Raw |
        ConvertFrom-Json
    $combinedForgedDocument.producer = 'installed-runtime-validation-plan-v2'
    Write-JsonDocument $combinedForgedSource $combinedForgedDocument
    Assert-Throws {
        Invoke-CombinedHostProducerTestCase $combinedForgedRoot
    } 'unregistered producer|allowlisted host-runner v2' `
        'combined host producer rejects a forged source producer identity'

    $combinedSingleTitleRoot = New-CombinedHostProducerTestCase 'single-title'
    $combinedSingleTitleSource = Join-Path $combinedSingleTitleRoot `
        'combined-source-receipts\ZeroHour\validation-results-receipt.json'
    $combinedSingleTitleDocument = Get-Content -LiteralPath $combinedSingleTitleSource -Raw |
        ConvertFrom-Json
    $combinedSingleTitleDocument.title = 'Generals'
    Write-JsonDocument $combinedSingleTitleSource $combinedSingleTitleDocument
    Assert-Throws {
        Invoke-CombinedHostProducerTestCase $combinedSingleTitleRoot
    } 'title scope is substituted|expected ''ZeroHour''' `
        'combined host producer rejects a source receipt that covers the wrong title'

    $combinedStaleRoot = New-CombinedHostProducerTestCase 'stale'
    $combinedStaleSource = Join-Path $combinedStaleRoot `
        'combined-source-receipts\Generals\validation-results-receipt.json'
    $combinedStaleDocument = Get-Content -LiteralPath $combinedStaleSource -Raw |
        ConvertFrom-Json
    $combinedStaleDocument.sourceCommit = 'B' * 40
    Write-JsonDocument $combinedStaleSource $combinedStaleDocument
    Assert-Throws {
        Invoke-CombinedHostProducerTestCase $combinedStaleRoot
    } 'stale or does not match the final acceptance commit' `
        'combined host producer rejects a source receipt from a stale commit'

    $combinedHashRoot = New-CombinedHostProducerTestCase 'executable-hash'
    Assert-Throws {
        Invoke-CombinedHostProducerTestCase $combinedHashRoot ('0' * 64)
    } 'executable SHA-256 binding' `
        'combined host producer rejects a source receipt with a substituted executable hash'

    $combinedNonceRoot = New-CombinedHostProducerTestCase 'duplicate-nonce'
    $combinedNonceGeneralsSource = Join-Path $combinedNonceRoot `
        'combined-source-receipts\Generals\validation-results-receipt.json'
    $combinedNonceZeroHourSource = Join-Path $combinedNonceRoot `
        'combined-source-receipts\ZeroHour\validation-results-receipt.json'
    $combinedNonceGeneralsDocument = Get-Content -LiteralPath $combinedNonceGeneralsSource -Raw |
        ConvertFrom-Json
    $combinedNonceZeroHourDocument = Get-Content -LiteralPath $combinedNonceZeroHourSource -Raw |
        ConvertFrom-Json
    $combinedNonceZeroHourDocument.runNonce = $combinedNonceGeneralsDocument.runNonce
    $combinedNonceZeroHourDocument.provenance.children[0].runNonce =
        $combinedNonceGeneralsDocument.runNonce
    $combinedNonceNativeReference =
        $combinedNonceZeroHourDocument.provenance.children[0].nativeReceipt
    $combinedNonceNativePath = Join-Path (Split-Path -Parent $combinedNonceZeroHourSource) `
        ([string]$combinedNonceNativeReference.path)
    $combinedNonceNativeDocument = Get-Content -LiteralPath $combinedNonceNativePath -Raw |
        ConvertFrom-Json
    $combinedNonceNativeDocument.runNonce = $combinedNonceGeneralsDocument.runNonce
    Write-JsonDocument $combinedNonceNativePath $combinedNonceNativeDocument
    $combinedNonceNativeReference.runNonce = $combinedNonceGeneralsDocument.runNonce
    $combinedNonceNativeReference.sha256 = Get-Sha256 $combinedNonceNativePath
    Write-JsonDocument $combinedNonceZeroHourSource $combinedNonceZeroHourDocument
    Assert-Throws {
        Invoke-CombinedHostProducerTestCase $combinedNonceRoot
    } 'replayed.*runNonce|distinct run nonces|wrapper nonce distinct' `
        'combined host producer rejects reused source run nonces'

    function Set-CombinedNativeRawPathFixture {
        param([string]$CaseRoot, [ValidateSet('absolute', 'upload-rebase', 'traversal', 'ads', 'drive-relative')][string]$Mode)
        foreach ($title in @('Generals', 'ZeroHour')) {
            $sourcePath = Join-Path $CaseRoot ("combined-source-receipts\$title\validation-results-receipt.json")
            $sourceDocument = Get-Content -LiteralPath $sourcePath -Raw | ConvertFrom-Json
            $nativeReference = $sourceDocument.provenance.children[0].nativeReceipt
            $nativePath = Join-Path (Split-Path -Parent $sourcePath) ([string]$nativeReference.path)
            $nativeDocument = Get-Content -LiteralPath $nativePath -Raw | ConvertFrom-Json
            $nativeReceiptAbsolute = [IO.Path]::GetFullPath($nativePath)
            if ($Mode -eq 'absolute' -or $Mode -eq 'upload-rebase') {
                $nativeDocument.provenance.receiptPath = if ($Mode -eq 'upload-rebase') {
                    "H:\uploaded-stage5\$title\$([IO.Path]::GetFileName($nativePath))"
                } else { $nativeReceiptAbsolute }
                foreach ($raw in @($nativeDocument.rawLogs)) {
                    $rawAbsolute = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $nativePath) ([string]$raw.path)))
                    $raw.path = if ($Mode -eq 'upload-rebase') {
                        "H:\uploaded-stage5\$title\$([IO.Path]::GetFileName($rawAbsolute))"
                    } else { $rawAbsolute }
                    if ([string]$raw.name -ceq 'raw-log') {
                        $nativeDocument.rawEvidence.rawLogPath = [string]$raw.path
                    }
                    else { $nativeDocument.rawEvidence.timingPath = [string]$raw.path }
                }
            }
            elseif ($Mode -eq 'traversal') {
                $nativeDocument.rawLogs[0].path = '..\escape.raw.log'
                $nativeDocument.rawEvidence.rawLogPath = '..\escape.raw.log'
            }
            elseif ($Mode -eq 'ads') {
                $nativeDocument.rawLogs[0].path = 'native.raw.log:secret'
                $nativeDocument.rawEvidence.rawLogPath = 'native.raw.log:secret'
            }
            else {
                $nativeDocument.rawLogs[0].path = 'C:relative.raw.log'
                $nativeDocument.rawEvidence.rawLogPath = 'C:relative.raw.log'
            }
            Write-JsonDocument $nativePath $nativeDocument
            $nativeReference.sha256 = Get-Sha256 $nativePath
            Write-JsonDocument $sourcePath $sourceDocument
        }
    }

    $combinedAbsoluteRoot = New-CombinedHostProducerTestCase 'native-absolute-paths'
    Set-CombinedNativeRawPathFixture $combinedAbsoluteRoot 'absolute'
    try {
        Invoke-CombinedHostProducerTestCase $combinedAbsoluteRoot
        $combinedAbsoluteDocument = Get-Content -LiteralPath (Join-Path $combinedAbsoluteRoot 'combined-results.json') -Raw |
            ConvertFrom-Json
        $combinedAbsoluteChildren = @($combinedAbsoluteDocument.provenance.children)
        Assert-True ($combinedAbsoluteChildren.Count -eq 2 -and
            @($combinedAbsoluteChildren | Where-Object {
                @($_.nativeRawBindings).Count -eq 2 -and
                @($_.nativeRawBindings | Where-Object {
                    [string]$_.sourcePath -match '^[A-Za-z]:[\\/]' -and
                    [string]$_.path -notmatch '^[A-Za-z]:|^[\\/]' -and
                    [string]$_.path -notmatch '(^|[\\/])\.\.([\\/]|$)'
                }).Count -eq 2 -and
                [string]$_.nativeReceiptSourcePath -match '^[A-Za-z]:[\\/]'
            }).Count -eq 2) `
            'combined host producer stages native absolute raw-log paths through explicit relative bindings without rewriting the hash-bound receipt'
    }
    catch {
        Assert-True $false "combined host producer accepts native absolute raw-log paths: $($_.Exception.Message)"
    }
    $combinedRebasedRoot = New-CombinedHostProducerTestCase 'native-upload-rebase'
    Set-CombinedNativeRawPathFixture $combinedRebasedRoot 'upload-rebase'
    try {
        Invoke-CombinedHostProducerTestCase $combinedRebasedRoot
        $rebasedDocument = Get-Content -LiteralPath `
            (Join-Path $combinedRebasedRoot 'combined-results.json') -Raw |
            ConvertFrom-Json
        Assert-True (@($rebasedDocument.provenance.children | Where-Object {
            [string]$_.nativeReceiptSourcePath -match '^H:\\uploaded-stage5\\' -and
            @($_.nativeRawBindings | Where-Object {
                [string]$_.sourcePath -match '^H:\\uploaded-stage5\\' -and
                [string]$_.path -notmatch '^[A-Za-z]:'
            }).Count -eq 2
        }).Count -eq 2) `
            'combined producer preserves original uploaded native provenance while validating downloaded immutable copies'
    }
    catch {
        Assert-True $false "combined host producer accepts an uploaded/rebased native receipt: $($_.Exception.Message)"
    }
    foreach ($pathMode in @(
        @{ mode = 'traversal'; pattern = 'parent traversal' },
        @{ mode = 'ads'; pattern = 'alternate data stream|ADS' },
        @{ mode = 'drive-relative'; pattern = 'drive-relative' }
    )) {
        $unsafeRoot = New-CombinedHostProducerTestCase "native-$($pathMode.mode)"
        Set-CombinedNativeRawPathFixture $unsafeRoot $pathMode.mode
        Assert-Throws {
            Invoke-CombinedHostProducerTestCase $unsafeRoot
        } $pathMode.pattern `
            "combined host producer rejects native $($pathMode.mode) raw-log paths"
    }

    $deterministicKind = 'deterministic-runtime'
    $deterministicAttachments = @()
    foreach ($role in $requiredAttachmentRoles[$deterministicKind]) {
        $leaf = "$deterministicKind-$role.json"
        $attachmentPath = Join-Path $attachmentRoot $leaf
        Write-Stage5HostReceiptTestDocument $attachmentPath $role 'ZeroHour' `
            $sourceCommit $artifactSetHash $artifactTestHashes
        $deterministicAttachments += [ordered]@{
            role = $role; title = 'ZeroHour'; path = "attachments\$leaf"
            sha256 = Get-Sha256 $attachmentPath
            trustDomain = 'host-runner'
        }
    }
    $deterministicDocument = [ordered]@{
        schemaVersion = 1; evidenceKind = $deterministicKind; status = 'passed'
        sourceCommit = $sourceCommit; title = 'ZeroHour'; architecture = 'x64'
        artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-01T00:00:00Z'
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        attachments = $deterministicAttachments
        details = [ordered]@{
            gateName = 'deterministic-runtime'; isolatedPipelineMode = 'serial'
            simulationModes = @('serial', 'parallel', 'shadow')
            workerConfigurations = @('serial-1', 'parallel-1', 'parallel-2',
                'parallel-4', 'parallel-8', 'parallel-16', 'parallel-auto')
            isolatedMatrixPassed = $true; finalAcceptanceClaim = $false
            replayEvidenceSha256 = $evidenceHashes['replay-determinism']
            freshAiEvidenceSha256 = $evidenceHashes['fresh-ai']
            performanceEvidenceSha256 = $evidenceHashes['performance-scaling']
        }
    }
    $deterministicPath = Join-Path $acceptanceRoot "$deterministicKind.json"
    Write-JsonDocument $deterministicPath $deterministicDocument
    $evidenceDocuments[$deterministicKind] = $deterministicDocument
    $evidencePaths[$deterministicKind] = $deterministicPath
    $evidenceHashes[$deterministicKind] = Get-Sha256 $deterministicPath

    # The local request is deliberately pre-manual only. Premium review and
    # user approval documents are exercised through their protected readers
    # above, but are never admitted to a local acceptance manifest.
    $acceptanceKinds = @('deterministic-runtime', 'replay-determinism', 'fresh-ai',
        'performance-scaling', 'mixed-worker-multiplayer',
        'combined-stage4-stage5-installed-runtime')
    $acceptanceRelativePaths = @{}
    function Write-AcceptanceRequest {
        param([string]$Path, [string[]]$Kinds)
        Write-JsonDocument $Path ([ordered]@{
            schemaVersion = 1; gateName = 'final-stage5-acceptance'
            sourceCommit = $sourceCommit
            cohortNonce = $script:TestCohortNonce
            cohortCreatedUtc = $script:TestCohortCreatedUtc
            artifactSet = [ordered]@{
                path = 'artifact-set.json'; sha256 = Get-Sha256 $artifactSetPath
            }
            evidence = @($Kinds | ForEach-Object {
                [ordered]@{
                    kind = $_
                    path = if ($acceptanceRelativePaths.ContainsKey($_)) {
                        $acceptanceRelativePaths[$_]
                    }
                    else { "$_.json" }
                    sha256 = Get-Sha256 $evidencePaths[$_]
                }
            })
        })
    }
    $acceptanceRequest = Join-Path $acceptanceRoot 'final-acceptance.json'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'diagnostic NET3 v1|lockstep-v2' `
        'a fully valid diagnostic NET3 v1 envelope cannot satisfy final acceptance'

    $outOfBandAcceptanceRequest = Join-Path $acceptanceRoot 'out-of-band-local-request.json'
    Write-AcceptanceRequest $outOfBandAcceptanceRequest `
        @($acceptanceKinds + 'manual-acceptance')
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $outOfBandAcceptanceRequest | Out-Null
    } 'not part of the local pre-manual contract|out-of-band' `
        'a local request cannot promote manual approval evidence into acceptance authority'

    # The installed v2 host is the only final-acceptance producer. Exercise its
    # canonical child boundary and fail-closed identity checks before adapting
    # a complete fixture into the compatibility role below.
    $lockstepRunnerPath = Join-Path $PSScriptRoot 'Invoke-InstalledLockstepV2Validation.ps1'
    $lockstepRunnerSource = Get-Content -LiteralPath $lockstepRunnerPath -Raw
    Assert-True ($lockstepRunnerSource -match '\$LockstepProducer\s*=\s*''installed-lockstep-v2''' -and
        $lockstepRunnerSource -match '\$LockstepMode\s*=\s*''installed-lockstep-v2-production''' -and
        $lockstepRunnerSource -match 'evidenceKind\s*=\s*''lockstep-v2-multiplayer''' -and
        $lockstepRunnerSource -match 'recordedUtc\s*=') `
        'the installed lockstep-v2 producer publishes the canonical final-acceptance identity'
    $lockstepProbe = [ordered]@{
        schemaVersion = 2; evidenceKind = 'lockstep-v2-multiplayer'; status = 'passed'
        producer = 'installed-lockstep-v2'; validationMode = 'installed-lockstep-v2-production'
        architecture = 'x64'; sourceCommit = $sourceCommit
        artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-01T00:00:00Z'
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        qualificationData = [ordered]@{
            manifestSha256 = '0' * 64
            closureSha256 = '1' * 64
            fileCount = 12
        }
        allowHeadlessDirectExecution = $true
        launcherEquivalence = [ordered]@{ Generals = [ordered]@{}; ZeroHour = [ordered]@{} }
        commonStopFrame = 4096; peerCount = 2; networkRosterMask = 3
        simulationRosterMask = 63; aiRosterMask = 60; aiPlayerCount = 4
        mapName = 'Maps\Twilight Flame\Twilight Flame.map'
        mapCrcs = [ordered]@{
            Generals = [UInt32]739101722
            ZeroHour = [UInt32]4042777579
        }
        seed = 23063; v1Accepted = $false
        profileStrategy = 'process-local-validation-profile-root'
        registryViews = @('Registry32', 'Registry64')
        environmentVariables = @('TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA', 'USERPROFILE',
            'HOMEDRIVE', 'HOMEPATH', 'RTS_STAGE5_VALIDATION_PROFILE_ROOT',
            'RTS_STAGE5_VALIDATION_CACHE_ROOT', 'RTS_STAGE5_VALIDATION_LOG_ROOT',
            'RTS_STAGE5_VALIDATION_DUMP_ROOT')
        profileConcurrency = 'shared-title-profile-read-only'
        titleSessionDisposition = 'removed-after-peer-exit-before-evidence-persist'
        sessions = @()
        negativeProbes = [ordered]@{ crossEpoch = @(); contentMismatch = @() }
    }
    $lockstepProbePath = Join-Path $acceptanceRoot 'lockstep-v2-probe.json'
    Write-JsonDocument $lockstepProbePath $lockstepProbe
    $lockstepReadArgs = @{
        Path = $lockstepProbePath; ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash; ArtifactHashes = $artifactTestHashes
    }
    $lockstepProbe.Remove('evidenceKind')
    Write-JsonDocument $lockstepProbePath $lockstepProbe
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @lockstepReadArgs | Out-Null
    } 'missing property ''evidenceKind''' `
        'lockstep-v2 evidence rejects an envelope without its explicit evidence kind'
    $lockstepProbe.evidenceKind = 'lockstep-v2-multiplayer'
    $lockstepProbe.producer = 'installed-runtime-lockstep-v2'
    $lockstepProbe.validationMode = 'production-lockstep-v2'
    Write-JsonDocument $lockstepProbePath $lockstepProbe
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @lockstepReadArgs | Out-Null
    } 'invalid schema/producer/mode boundary' `
        'lockstep-v2 evidence rejects a substituted producer and validation mode'
    $lockstepProbe.producer = 'installed-lockstep-v2'
    $lockstepProbe.validationMode = 'installed-lockstep-v2-production'
    $lockstepProbe.sourceCommit = 'b' * 40
    Write-JsonDocument $lockstepProbePath $lockstepProbe
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @lockstepReadArgs | Out-Null
    } 'stale or substituted' `
        'lockstep-v2 evidence rejects a stale source revision before session acceptance'
    $lockstepProbe.sourceCommit = $sourceCommit
    $lockstepProbe.artifactSetSha256 = 'B' * 64
    Write-JsonDocument $lockstepProbePath $lockstepProbe
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @lockstepReadArgs | Out-Null
    } 'does not match the independently hashed artifact set' `
        'lockstep-v2 evidence rejects a substituted artifact-set hash'

    $lockstepFixtureRoot = Join-Path $acceptanceRoot 'lockstep-v2-positive'
    $lockstepFixture = New-LockstepFixtureEvidence $lockstepFixtureRoot `
        $sourceCommit $artifactSetHash $artifactTestHashes
    $lockstepPositiveArgs = @{
        Path = $lockstepFixture.path; ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash; ArtifactHashes = $artifactTestHashes
        ArtifactPaths = $artifactTestPaths; ArtifactRootDirectory = $acceptanceRoot
    }
    try {
        $lockstepRead = Read-Stage5LockstepV2Evidence @lockstepPositiveArgs
        Assert-True ($lockstepRead.schemaVersion -eq 2 -and
            $lockstepRead.evidenceKind -ceq 'lockstep-v2-multiplayer' -and
            $lockstepRead.titleSessionDisposition -ceq
                'removed-after-peer-exit-before-evidence-persist' -and
            $lockstepRead.qualificationData.manifestSha256 -ceq
                $lockstepFixture.document.qualificationData.manifestSha256 -and
            $lockstepRead.qualificationData.closureSha256 -ceq
                $lockstepFixture.document.qualificationData.closureSha256 -and
            $lockstepRead.qualificationData.fileCount -eq 12 -and
            $lockstepRead.mapName -ceq 'Maps\Twilight Flame\Twilight Flame.map' -and
            [UInt64]$lockstepRead.mapCrcs['Generals'] -eq 739101722 -and
            [UInt64]$lockstepRead.mapCrcs['ZeroHour'] -eq 4042777579 -and
            [UInt64]$lockstepRead.mapCrcs['Generals'] -ne
                [UInt64]$lockstepRead.mapCrcs['ZeroHour'] -and
            $lockstepRead.sessions.Count -eq 2 -and
            [UInt64]$lockstepRead.sessions[0].mapCrc -eq 739101722 -and
            [UInt64]$lockstepRead.sessions[1].mapCrc -eq 4042777579 -and
            $lockstepRead.sessions[0].peers.Count -eq 2) `
            'a complete executable-shaped two-title/two-peer lockstep-v2 fixture is accepted'
    }
    catch {
        Assert-True $false "a complete lockstep-v2 fixture should be accepted: $($_.Exception.Message)"
    }
    $missingQualificationBindingRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-missing-qualification-binding'
    $missingQualificationBindingPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $missingQualificationBindingRoot
    $missingQualificationBinding = ConvertFrom-Stage5TestJsonDictionary `
        $missingQualificationBindingPath
    [void]$missingQualificationBinding.Remove('qualificationData')
    Write-JsonDocument $missingQualificationBindingPath $missingQualificationBinding
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $missingQualificationBindingPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } "missing property 'qualificationData'" `
        'lockstep-v2 requires an explicit retained qualification-data binding'

    $missingQualificationManifestRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-missing-qualification-manifest'
    $missingQualificationManifestPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $missingQualificationManifestRoot
    Remove-Item -LiteralPath (Join-Path $missingQualificationManifestRoot `
        'QualificationData.json') -Force
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $missingQualificationManifestPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'qualification-data manifest was not found|Cannot find path.*QualificationData.json' `
        'lockstep-v2 rejects a missing retained qualification-data manifest'

    $qualificationHashRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-qualification-hash'
    $qualificationHashPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $qualificationHashRoot
    $qualificationHashDocument = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationHashPath
    $qualificationHashDocument['qualificationData']['manifestSha256'] = '0' * 64
    Write-JsonDocument $qualificationHashPath $qualificationHashDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $qualificationHashPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'qualification-data manifest.*SHA-256 mismatch' `
        'lockstep-v2 rejects a retained qualification-data manifest detached from the native receipt'

    $qualificationArchiveRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-qualification-archive'
    $qualificationArchivePath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $qualificationArchiveRoot
    $qualificationArchiveManifestPath = Join-Path $qualificationArchiveRoot `
        'QualificationData.json'
    $qualificationArchiveManifest = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationArchiveManifestPath
    $qualificationArchiveManifest['archiveSources'][0]['sha256'] = '0' * 64
    Write-JsonDocument $qualificationArchiveManifestPath $qualificationArchiveManifest
    $qualificationArchiveDocument = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationArchivePath
    $qualificationArchiveDocument['qualificationData']['manifestSha256'] =
        Get-Sha256 $qualificationArchiveManifestPath
    Write-JsonDocument $qualificationArchivePath $qualificationArchiveDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $qualificationArchivePath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'archive source is unreviewed|archive provenance' `
        'lockstep-v2 rejects rehashed qualification data from an unreviewed archive'

    $qualificationMapRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-qualification-map'
    $qualificationMapPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $qualificationMapRoot
    $qualificationMapManifestPath = Join-Path $qualificationMapRoot `
        'QualificationData.json'
    $qualificationMapManifest = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationMapManifestPath
    $qualificationMapManifest['mapCrcs']['ZeroHour'] = [UInt32]739101722
    Write-JsonDocument $qualificationMapManifestPath $qualificationMapManifest
    $qualificationMapDocument = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationMapPath
    $qualificationMapDocument['qualificationData']['manifestSha256'] =
        Get-Sha256 $qualificationMapManifestPath
    Write-JsonDocument $qualificationMapPath $qualificationMapDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $qualificationMapPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'qualification-data identity.*map binding|map CRC' `
        'lockstep-v2 rejects a retained qualification-data manifest for another title map identity'

    $qualificationClosureRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-qualification-closure'
    $qualificationClosurePath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $qualificationClosureRoot
    $qualificationClosureManifestPath = Join-Path $qualificationClosureRoot `
        'QualificationData.json'
    $qualificationClosureManifest = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationClosureManifestPath
    $qualificationClosureManifest['files'][0]['sha256'] = 'F' * 64
    Write-JsonDocument $qualificationClosureManifestPath $qualificationClosureManifest
    $qualificationClosureDocument = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationClosurePath
    $qualificationClosureDocument['qualificationData']['manifestSha256'] =
        Get-Sha256 $qualificationClosureManifestPath
    Write-JsonDocument $qualificationClosurePath $qualificationClosureDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $qualificationClosurePath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'qualification-data file closure SHA-256 is stale or substituted' `
        'lockstep-v2 recomputes the retained qualification-data file closure'

    $qualificationCountRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-qualification-count'
    $qualificationCountPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $qualificationCountRoot
    $qualificationCountDocument = ConvertFrom-Stage5TestJsonDictionary `
        $qualificationCountPath
    $qualificationCountDocument['qualificationData']['fileCount'] = 13
    Write-JsonDocument $qualificationCountPath $qualificationCountDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $qualificationCountPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'qualification-data file count is stale or substituted' `
        'lockstep-v2 binds the exact retained qualification-data entry count'

    $mapCrcSubstitutionRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-map-crc-substitution'
    $mapCrcSubstitutionPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $mapCrcSubstitutionRoot
    $mapCrcSubstitution = Read-LockstepFixtureCaseDocument `
        $mapCrcSubstitutionPath
    $mapCrcSubstitution.mapCrcs.ZeroHour =
        $mapCrcSubstitution.mapCrcs.Generals
    Write-JsonDocument $mapCrcSubstitutionPath $mapCrcSubstitution
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $mapCrcSubstitutionPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'title-specific map CRC is substituted|qualification-data identity or map binding' `
        'lockstep-v2 rejects a Generals map CRC substituted into Zero Hour evidence'
    $titleDispositionMissingRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-title-disposition-missing'
    $titleDispositionMissingPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $titleDispositionMissingRoot
    $titleDispositionMissing = ConvertFrom-Stage5TestJsonDictionary `
        $titleDispositionMissingPath
    [void]$titleDispositionMissing.Remove('titleSessionDisposition')
    Write-JsonDocument $titleDispositionMissingPath $titleDispositionMissing
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $titleDispositionMissingPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } "missing property 'titleSessionDisposition'" `
        'lockstep-v2 requires the producer title-session cleanup disposition'
    $titleDispositionWrongRoot = Join-Path $acceptanceRoot `
        'lockstep-v2-negative-title-disposition-wrong'
    $titleDispositionWrongPath = Copy-LockstepFixtureCase $lockstepFixtureRoot `
        $titleDispositionWrongRoot
    $titleDispositionWrong = ConvertFrom-Stage5TestJsonDictionary `
        $titleDispositionWrongPath
    $titleDispositionWrong['titleSessionDisposition'] = 'retained-after-peer-exit'
    Write-JsonDocument $titleDispositionWrongPath $titleDispositionWrong
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $titleDispositionWrongPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'did not prove title-session cleanup before evidence persistence' `
        'lockstep-v2 rejects a substituted title-session cleanup disposition'
    $negativeProbeForgeRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-probe-forged'
    $negativeProbeForgePath = Copy-LockstepFixtureCase $lockstepFixtureRoot $negativeProbeForgeRoot
    $negativeProbeForgeDocument = Read-LockstepFixtureCaseDocument $negativeProbeForgePath
    $negativeProbeForgeDocument.negativeProbes.crossEpoch[0].mutatedAccepted = $true
    Write-JsonDocument $negativeProbeForgePath $negativeProbeForgeDocument
    $negativeProbeForgeArgs = @{} + $lockstepPositiveArgs
    $negativeProbeForgeArgs.Path = $negativeProbeForgePath
    $negativeProbeForgeArgs.Remove('ArtifactPaths')
    $negativeProbeForgeArgs.Remove('ArtifactRootDirectory')
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @negativeProbeForgeArgs | Out-Null
    } 'metadata is stale|substituted|detached' `
        'lockstep-v2 rejects forged negative-probe metadata even when the raw proof is unchanged'

    $negativeProbeStaleRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-probe-stale'
    $negativeProbeStalePath = Copy-LockstepFixtureCase $lockstepFixtureRoot $negativeProbeStaleRoot
    $negativeProbeStaleProof = Join-Path (Split-Path -Parent $negativeProbeStalePath) `
        'Generals/NegativeProbes/cross-epoch.proof'
    $negativeProbeStaleText = [IO.File]::ReadAllText($negativeProbeStaleProof).Replace(
        'observed_error=UnsupportedEngineEpoch', 'observed_error=ContentHashMismatch')
    [IO.File]::WriteAllText($negativeProbeStaleProof, $negativeProbeStaleText,
        (New-Object Text.UTF8Encoding($false)))
    $negativeProbeStaleArgs = @{} + $lockstepPositiveArgs
    $negativeProbeStaleArgs.Path = $negativeProbeStalePath
    $negativeProbeStaleArgs.Remove('ArtifactPaths')
    $negativeProbeStaleArgs.Remove('ArtifactRootDirectory')
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @negativeProbeStaleArgs | Out-Null
    } 'SHA-256 mismatch' `
        'lockstep-v2 rejects a negative-probe proof whose bytes are stale under the recorded hash'

    $negativeProbeTamperRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-probe-tamper'
    $negativeProbeTamperPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $negativeProbeTamperRoot
    $negativeProbeTamperProof = Join-Path (Split-Path -Parent $negativeProbeTamperPath) `
        'ZeroHour/NegativeProbes/content-mismatch.proof'
    $negativeProbeTamperText = [IO.File]::ReadAllText($negativeProbeTamperProof).Replace(
        'mutated_accepted=0', 'mutated_accepted=1')
    [IO.File]::WriteAllText($negativeProbeTamperProof, $negativeProbeTamperText,
        (New-Object Text.UTF8Encoding($false)))
    $negativeProbeTamperDocument = Read-LockstepFixtureCaseDocument $negativeProbeTamperPath
    $negativeProbeTamperDocument.negativeProbes.contentMismatch[1].proofSha256 =
        Get-Sha256 $negativeProbeTamperProof
    Write-JsonDocument $negativeProbeTamperPath $negativeProbeTamperDocument
    $negativeProbeTamperArgs = @{} + $lockstepPositiveArgs
    $negativeProbeTamperArgs.Path = $negativeProbeTamperPath
    $negativeProbeTamperArgs.Remove('ArtifactPaths')
    $negativeProbeTamperArgs.Remove('ArtifactRootDirectory')
    Assert-Throws {
        Read-Stage5LockstepV2Evidence @negativeProbeTamperArgs | Out-Null
    } 'raw proof' `
        'lockstep-v2 rejects tampered negative-probe raw rejection evidence after rehashing'
    $threePeerCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-three-peer'
    $threePeerPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $threePeerCaseRoot
    $threePeerDocument = Read-LockstepFixtureCaseDocument $threePeerPath
    $threePeerDocument.peerCount = 3
    Write-JsonDocument $threePeerPath $threePeerDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $threePeerPath $sourceCommit `
            $artifactSetHash $artifactTestHashes | Out-Null
    } 'bounded x64 4096-frame v2 contract' `
        'lockstep-v2 rejects a native envelope claiming three peers for the exact two-peer contract'

    # Adapt the installed lockstep-v2 child into the compatibility role consumed
    # by final acceptance.  The outer envelope is host-runner evidence; the
    # native child remains the only multiplayer qualification authority.
    # The final adapter must consume the canonical installed fixture paths from
    # the artifact set. A copied/staged runtime is intentionally exercised as a
    # later negative; accepting it here would allow same-hash sidecar swaps.
    $lockstepAdapterPath = $lockstepFixture.path
    $lockstepAdapterHash = Get-Sha256 $lockstepAdapterPath
    $lockstepAdapterDocument = [ordered]@{
        schemaVersion = 1; evidenceKind = 'mixed-worker-multiplayer'; status = 'passed'
        sourceCommit = $sourceCommit; title = 'Both'; architecture = 'x64'
        artifactSetSha256 = $artifactSetHash; recordedUtc = '2026-09-02T00:00:00Z'
        cohortNonce = $script:TestCohortNonce
        runtimeClosure = $script:TestRuntimeClosure
        attachments = @([ordered]@{
            role = 'multiplayer-results'; title = 'Both'
            path = 'lockstep-v2-positive\LockstepV2LoopbackEvidence.json'
            sha256 = $lockstepAdapterHash; trustDomain = 'host-runner'
        })
        details = [ordered]@{
            nativeEvidenceKind = 'lockstep-v2-multiplayer'
            producer = 'installed-lockstep-v2'; nativeEvidenceSha256 = $lockstepAdapterHash
            networkRosterMask = 3; simulationRosterMask = 63; aiRosterMask = 60
            aiPlayerCount = 4; title = 'Both'; sessionCount = 2; peerCount = 2
            commonStopFrame = 4096; allMatchesCompleted = $true
            stateTracesIdentical = $true; crossEpochRejected = $true
            contentMismatchRejected = $true
        }
    }
    $lockstepAdapterEvidencePath = Join-Path $acceptanceRoot `
        'mixed-worker-multiplayer.json'
    Write-JsonDocument $lockstepAdapterEvidencePath $lockstepAdapterDocument
    $evidencePaths['mixed-worker-multiplayer'] = $lockstepAdapterEvidencePath
    $acceptanceRelativePaths['mixed-worker-multiplayer'] = `
        'mixed-worker-multiplayer.json'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    try {
    $adapterAcceptance = Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
        -DevelopmentReadiness
        Assert-True ($adapterAcceptance.status -ceq 'ready-for-manual-approval' -and
            $adapterAcceptance.gateName -ceq 'stage5-development-readiness' -and
            -not [bool]$adapterAcceptance.finalAcceptanceClaim -and
            $adapterAcceptance.cohortNonce -match
                '^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}$' -and
            $adapterAcceptance.evidenceFreshness -ceq 'current-cohort' -and
            @($adapterAcceptance.evidence | Where-Object {
                $_.freshness -cne 'current-cohort'
            }).Count -eq 0) `
            'the installed lockstep-v2 child passes through the pre-manual readiness adapter'
    }
    catch {
        Assert-True $false "the installed lockstep-v2 adapter should satisfy final acceptance: $($_.Exception.Message)"
    }

    # Replay qualification is title-scoped.  Exercise the composite
    # role/title attachment key explicitly: collapsing the two reviewed
    # manifests to one key, or swapping a title onto the other receipt, must
    # fail closed even when every referenced file remains byte-valid.
    $replayEvidencePath = $evidencePaths['replay-determinism']
    $replayEvidenceOriginalText = [IO.File]::ReadAllText($replayEvidencePath)
    try {
        $duplicateReplayDocument = ConvertFrom-Stage5TestJsonDictionary `
            $replayEvidencePath
        $duplicateReplayDocument['attachments'][2]['title'] = 'Generals'
        Write-JsonDocument $replayEvidencePath $duplicateReplayDocument
        Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
        Assert-Throws {
            Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
                -DevelopmentReadiness | Out-Null
        } 'repeats or does not authorize attachment' `
            'replay acceptance rejects a duplicate composite role/title attachment key'

        [IO.File]::WriteAllText($replayEvidencePath, $replayEvidenceOriginalText)
        $swappedReplayDocument = ConvertFrom-Stage5TestJsonDictionary `
            $replayEvidencePath
        $generalsManifestPath = $swappedReplayDocument['attachments'][1]['path']
        $generalsManifestHash = $swappedReplayDocument['attachments'][1]['sha256']
        $zeroHourManifestPath = $swappedReplayDocument['attachments'][2]['path']
        $zeroHourManifestHash = $swappedReplayDocument['attachments'][2]['sha256']
        $swappedReplayDocument['attachments'][1]['path'] = $zeroHourManifestPath
        $swappedReplayDocument['attachments'][1]['sha256'] = $zeroHourManifestHash
        $swappedReplayDocument['attachments'][2]['path'] = $generalsManifestPath
        $swappedReplayDocument['attachments'][2]['sha256'] = $generalsManifestHash
        Write-JsonDocument $replayEvidencePath $swappedReplayDocument
        Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
        Assert-Throws {
            Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
                -DevelopmentReadiness | Out-Null
        } 'title scope|expected.*Generals|expected.*ZeroHour' `
            'replay acceptance rejects reviewed receipts swapped across title bindings'
    }
    finally {
        [IO.File]::WriteAllText($replayEvidencePath, $replayEvidenceOriginalText)
        Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    }
    $runtimeTitleDocument = $evidenceDocuments['deterministic-runtime']
    $runtimeTitleDocument.title = 'Generals'
    Write-JsonDocument $evidencePaths['deterministic-runtime'] $runtimeTitleDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
            -DevelopmentReadiness | Out-Null
    } "exact title scope 'ZeroHour'" `
        'final acceptance rejects a deterministic-runtime envelope relabeled as Generals'
    $runtimeTitleDocument.title = 'ZeroHour'
    Write-JsonDocument $evidencePaths['deterministic-runtime'] $runtimeTitleDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds

    $futureCohortRequest = ConvertFrom-Stage5TestJsonDictionary $acceptanceRequest
    $futureCohortRequest['cohortCreatedUtc'] = '2999-01-01T00:00:00.0000000Z'
    Write-JsonDocument $acceptanceRequest $futureCohortRequest
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
            -DevelopmentReadiness | Out-Null
    } 'canonical current UTC timestamp' `
        'final acceptance rejects a fully self-consistent manifest cohort rebased into the future'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds

    $copiedLockstepRoot = Join-Path $acceptanceRoot 'lockstep-v2-copied-runtime'
    $copiedLockstepPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $copiedLockstepRoot
    $copiedAdapterDocument = ConvertFrom-Json `
        ($lockstepAdapterDocument | ConvertTo-Json -Depth 16)
    $copiedAdapterDocument.attachments[0].path = `
        'lockstep-v2-copied-runtime\LockstepV2LoopbackEvidence.json'
    $copiedLockstepHash = Get-Sha256 $copiedLockstepPath
    $copiedAdapterDocument.attachments[0].sha256 = $copiedLockstepHash
    $copiedAdapterDocument.details.nativeEvidenceSha256 = $copiedLockstepHash
    $copiedAdapterEvidencePath = Join-Path $acceptanceRoot `
        'mixed-worker-multiplayer-copied.json'
    Write-JsonDocument $copiedAdapterEvidencePath $copiedAdapterDocument
    $evidencePaths['mixed-worker-multiplayer'] = $copiedAdapterEvidencePath
    $acceptanceRelativePaths['mixed-worker-multiplayer'] = `
        'mixed-worker-multiplayer-copied.json'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest `
            -DevelopmentReadiness | Out-Null
    } 'canonical artifact-set files|canonical executable' `
        'the lockstep adapter rejects a copied executable with staged sidecars'
    $evidencePaths['mixed-worker-multiplayer'] = $lockstepAdapterEvidencePath
    $acceptanceRelativePaths['mixed-worker-multiplayer'] = 'mixed-worker-multiplayer.json'
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    $adapterTrustDomain = $lockstepAdapterDocument.attachments[0].trustDomain
    $lockstepAdapterDocument.attachments[0].trustDomain = 'executable'
    Write-JsonDocument $lockstepAdapterEvidencePath $lockstepAdapterDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'wrong trust domain' `
        'the lockstep-v2 adapter rejects a multiplayer child outside the host-runner trust domain'
    $lockstepAdapterDocument.attachments[0].trustDomain = $adapterTrustDomain
    $lockstepAdapterDocument.attachments[0].sha256 = '0' * 64
    Write-JsonDocument $lockstepAdapterEvidencePath $lockstepAdapterDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'SHA-256 mismatch' `
        'the lockstep-v2 adapter rejects a substituted native-child hash'
    $lockstepAdapterDocument.attachments[0].sha256 = $lockstepAdapterHash
    Write-JsonDocument $lockstepAdapterEvidencePath $lockstepAdapterDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds

    $receiptBytesCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-receipt-bytes'
    $receiptBytesPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $receiptBytesCaseRoot
    $receiptBytesFile = Join-Path (Split-Path -Parent $receiptBytesPath) 'Generals/lockstep-v2-Generals-peer-0.receipt'
    $receiptBytes = [IO.File]::ReadAllText($receiptBytesFile).Replace('clean_shutdown=1', 'clean_shutdown=0')
    [IO.File]::WriteAllText($receiptBytesFile, $receiptBytes, (New-Object Text.UTF8Encoding($false)))
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $receiptBytesPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'receipt SHA-256 binding does not match' `
        'lockstep-v2 rejects receipt bytes that no longer match the producer hash'

    $receiptHashCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-receipt-hash'
    $receiptHashPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $receiptHashCaseRoot
    $receiptHashDocument = Read-LockstepFixtureCaseDocument $receiptHashPath
    $receiptHashDocument.sessions[0].peers[0].receiptSha256 = '0' * 64
    $receiptHashRaw = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $receiptHashPath) 'Generals/peer-0.raw.json') -Raw | ConvertFrom-Json
    $receiptHashRaw.receiptSha256 = '0' * 64
    Write-LockstepFixturePeerMutation $receiptHashPath $receiptHashDocument 0 0 $receiptHashRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $receiptHashPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'receipt SHA-256 binding does not match|raw receipt index field' `
        'lockstep-v2 rejects a substituted receipt hash'

    $stdoutMarkerCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-stdout-marker'
    $stdoutMarkerPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $stdoutMarkerCaseRoot
    $stdoutMarkerDocument = Read-LockstepFixtureCaseDocument $stdoutMarkerPath
    $stdoutMarkerRawPath = Join-Path (Split-Path -Parent $stdoutMarkerPath) 'Generals/peer-0.raw.json'
    $stdoutMarkerRaw = Get-Content -LiteralPath $stdoutMarkerRawPath -Raw | ConvertFrom-Json
    $stdoutMarkerFile = Join-Path (Split-Path -Parent $stdoutMarkerPath) 'Generals/peer-0.stdout.log'
    $stdoutMarkerText = [IO.File]::ReadAllText($stdoutMarkerFile).Replace('frame=4096 crc=00018720', 'frame=4095 crc=00018720')
    [IO.File]::WriteAllText($stdoutMarkerFile, $stdoutMarkerText, (New-Object Text.UTF8Encoding($false)))
    $stdoutMarkerHash = Get-Sha256 $stdoutMarkerFile
    $stdoutMarkerDocument.sessions[0].peers[0].stdoutSha256 = $stdoutMarkerHash
    $stdoutMarkerRaw.stdoutSha256 = $stdoutMarkerHash
    Write-LockstepFixturePeerMutation $stdoutMarkerPath $stdoutMarkerDocument 0 0 $stdoutMarkerRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $stdoutMarkerPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'stdout proof is stale|stdout is not an exclusive' `
        'lockstep-v2 rejects a stdout marker detached from the 4096-frame contract'

    $stdoutHashCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-stdout-hash'
    $stdoutHashPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $stdoutHashCaseRoot
    $stdoutHashDocument = Read-LockstepFixtureCaseDocument $stdoutHashPath
    $stdoutHashRawPath = Join-Path (Split-Path -Parent $stdoutHashPath) 'ZeroHour/peer-1.raw.json'
    $stdoutHashRaw = Get-Content -LiteralPath $stdoutHashRawPath -Raw | ConvertFrom-Json
    $stdoutHashDocument.sessions[1].peers[1].stdoutSha256 = 'F' * 64
    $stdoutHashRaw.stdoutSha256 = 'F' * 64
    Write-LockstepFixturePeerMutation $stdoutHashPath $stdoutHashDocument 1 1 $stdoutHashRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $stdoutHashPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'stdout SHA-256 binding does not match' `
        'lockstep-v2 rejects a substituted stdout hash'

    $pidCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-pid'
    $pidPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $pidCaseRoot
    $pidDocument = Read-LockstepFixtureCaseDocument $pidPath
    $pidDocument.sessions[0].peers[0].processId = 51999
    $pidRawPath = Join-Path (Split-Path -Parent $pidPath) 'Generals/peer-0.raw.json'
    $pidRaw = Get-Content -LiteralPath $pidRawPath -Raw | ConvertFrom-Json
    $pidRaw.processId = 51999
    Write-LockstepFixturePeerMutation $pidPath $pidDocument 0 0 $pidRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $pidPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'stdout proof is stale|stdout proof' `
        'lockstep-v2 rejects a pass marker with a substituted process identity'

    $nonceCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-run-nonce'
    $noncePath = Copy-LockstepFixtureCase $lockstepFixtureRoot $nonceCaseRoot
    $nonceDocument = Read-LockstepFixtureCaseDocument $noncePath
    $nonceDocument.sessions[1].peers[0].runNonce = 'F' * 32
    $nonceRawPath = Join-Path (Split-Path -Parent $noncePath) 'ZeroHour/peer-0.raw.json'
    $nonceRaw = Get-Content -LiteralPath $nonceRawPath -Raw | ConvertFrom-Json
    $nonceRaw.runNonce = 'F' * 32
    Write-LockstepFixturePeerMutation $noncePath $nonceDocument 1 0 $nonceRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $noncePath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'configuration does not bind|run identity|receipt.*run' `
        'lockstep-v2 rejects a replayed or substituted run nonce'

    $workerOverrideCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-worker-override'
    $workerOverridePath = Copy-LockstepFixtureCase $lockstepFixtureRoot $workerOverrideCaseRoot
    $workerOverrideDocument = Read-LockstepFixtureCaseDocument $workerOverridePath
    $workerOverrideDocument.sessions[0].peers[0].workerOverride.profile = 'automatic-workers'
    $workerOverrideRawPath = Join-Path (Split-Path -Parent $workerOverridePath) 'Generals/peer-0.raw.json'
    $workerOverrideRaw = Get-Content -LiteralPath $workerOverrideRawPath -Raw | ConvertFrom-Json
    $workerOverrideRaw.workerOverride.profile = 'automatic-workers'
    Write-LockstepFixturePeerMutation $workerOverridePath $workerOverrideDocument 0 0 $workerOverrideRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $workerOverridePath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'worker override|process arguments' `
        'lockstep-v2 rejects a substituted executable worker override'

    $workerTelemetryCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-worker-telemetry'
    $workerTelemetryPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $workerTelemetryCaseRoot
    $workerTelemetryDocument = Read-LockstepFixtureCaseDocument $workerTelemetryPath
    $workerTelemetryDocument.sessions[0].peers[0].receiptWorkerTelemetry.effectiveWorkers = 3
    $workerTelemetryDocument.sessions[0].peers[0].effectiveWorkers = 3
    $workerTelemetryRawPath = Join-Path (Split-Path -Parent $workerTelemetryPath) 'Generals/peer-0.raw.json'
    $workerTelemetryRaw = Get-Content -LiteralPath $workerTelemetryRawPath -Raw | ConvertFrom-Json
    $workerTelemetryRaw.receiptWorkerTelemetry.effectiveWorkers = 3
    $workerTelemetryRaw.effectiveWorkers = 3
    Write-LockstepFixturePeerMutation $workerTelemetryPath $workerTelemetryDocument 0 0 $workerTelemetryRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $workerTelemetryPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'worker profile|worker telemetry|effective worker' `
        'lockstep-v2 rejects telemetry that no longer matches the executable worker profile'

    $projectionCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-projection'
    $projectionPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $projectionCaseRoot
    $projectionReceiptPath = Join-Path (Split-Path -Parent $projectionPath) 'ZeroHour/lockstep-v2-ZeroHour-peer-1.receipt'
    $projection = Set-LockstepFixtureReceiptCheckpointCrc $projectionReceiptPath '100999'
    $projectionDocument = Read-LockstepFixtureCaseDocument $projectionPath
    $projectionDocument.sessions[1].peers[1].receiptSha256 = Get-Sha256 $projectionReceiptPath
    $projectionDocument.sessions[1].peers[1].comparableProjectionSha256 = $projection
    $projectionRawPath = Join-Path (Split-Path -Parent $projectionPath) 'ZeroHour/peer-1.raw.json'
    $projectionRaw = Get-Content -LiteralPath $projectionRawPath -Raw | ConvertFrom-Json
    $projectionRaw.receiptSha256 = $projectionDocument.sessions[1].peers[1].receiptSha256
    $projectionRaw.comparableProjectionSha256 = $projection
    Write-LockstepFixturePeerMutation $projectionPath $projectionDocument 1 1 $projectionRaw
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $projectionPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'peers disagree on CRC, checkpoint, or command digests' `
        'lockstep-v2 rejects a substituted cross-peer checkpoint projection'

    $timestampCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-timestamp'
    $timestampPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $timestampCaseRoot
    $timestampDocument = Read-LockstepFixtureCaseDocument $timestampPath
    $timestampDocument.recordedUtc = 'not-a-timestamp'
    Write-JsonDocument $timestampPath $timestampDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $timestampPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'not a valid timestamp' 'lockstep-v2 rejects a stale or malformed aggregate timestamp'

    $sourceCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-source'
    $sourcePath = Copy-LockstepFixtureCase $lockstepFixtureRoot $sourceCaseRoot
    $sourceDocument = Read-LockstepFixtureCaseDocument $sourcePath
    $sourceDocument.sourceCommit = 'B' * 40
    Write-JsonDocument $sourcePath $sourceDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $sourcePath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'stale or substituted' 'lockstep-v2 rejects a stale aggregate source revision'

    $artifactCaseRoot = Join-Path $acceptanceRoot 'lockstep-v2-negative-artifact'
    $artifactPath = Copy-LockstepFixtureCase $lockstepFixtureRoot $artifactCaseRoot
    $artifactDocument = Read-LockstepFixtureCaseDocument $artifactPath
    $artifactDocument.artifactSetSha256 = 'C' * 64
    Write-JsonDocument $artifactPath $artifactDocument
    Assert-Throws {
        Read-Stage5LockstepV2Evidence $artifactPath $sourceCommit $artifactSetHash $artifactTestHashes | Out-Null
    } 'does not match the independently hashed artifact set' 'lockstep-v2 rejects a substituted artifact-set binding'

    $immutableReceiptSchema = Join-Path $PSScriptRoot 'Stage5ImmutableEvidenceReceipt.schema.json'
    Assert-True (Test-Path -LiteralPath $immutableReceiptSchema -PathType Leaf) `
        'the generic immutable receipt schema is retained beside the host validator'
    $immutableSchemaDocument = Get-Content -LiteralPath $immutableReceiptSchema -Raw |
        ConvertFrom-Json
    $combinedChildRequirement = @($immutableSchemaDocument.'$defs'.combinedHostChild.allOf |
        Where-Object { $_.PSObject.Properties.Name -contains 'required' })[0]
    $combinedHostBranch = @($immutableSchemaDocument.'$defs'.hostRunnerReceipt.allOf[1].oneOf |
        Where-Object { $_.properties.role.const -ceq 'combined-results' })[0]
    Assert-True ($null -ne $combinedChildRequirement -and
        @($combinedChildRequirement.required) -contains 'nativeReceiptSourcePath' -and
        @($combinedChildRequirement.required) -contains 'nativeRawBindings' -and
        $combinedHostBranch.properties.provenance.'$ref' -ceq
            '#/$defs/combinedHostProvenance') `
        'combined immutable receipt schema requires the runtime native source and raw bindings for every child'
    $immutableReceiptPath = Join-Path $attachmentRoot 'immutable-receipt-validation-plan.json'
    Write-ImmutableReceiptTestDocument $immutableReceiptPath $sourceCommit `
        $artifactSetHash $artifactTestHashes['zerohour-executable']
    Assert-True (Test-Stage5TestJsonSchema `
        (Get-Content -LiteralPath $immutableReceiptPath -Raw) `
        $immutableReceiptSchema) `
        'the immutable schema accepts the production-shaped validation-plan writer output'
    Assert-True (Test-Stage5TestJsonSchema `
        (Get-Content -LiteralPath $combinedGeneralsSource -Raw) `
        $immutableReceiptSchema) `
        'the immutable schema accepts a production-shaped bound-child host receipt'
    $resultsBindingPath = Join-Path $attachmentRoot `
        'immutable-validation-results-hash-binding.json'
    Write-Stage5HostReceiptTestDocument $resultsBindingPath `
        'validation-results' 'ZeroHour' $sourceCommit $artifactSetHash `
        $artifactTestHashes
    $resultsBindingRelocation = Get-Stage5FinalAcceptanceNativeRelocationBinding `
        -Path $resultsBindingPath -EvidenceDirectory $attachmentRoot
    $resultsBindingArgs = @{
        Path = $resultsBindingPath; Kind = 'deterministic-runtime'
        Role = 'validation-results'; EvidenceTitle = 'ZeroHour'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
        ExpectedCohortNonce = $script:TestCohortNonce
        ExpectedCohortCreatedUtc = $script:TestCohortCreatedUtc
        ExpectedRuntimeClosure = $script:TestRuntimeClosure
        NativeRelocationBindings = @($resultsBindingRelocation.children)
    }
    $resultsBindingRead = Read-Stage5FinalAcceptanceImmutableReceipt @resultsBindingArgs
    Assert-True ($null -ne $resultsBindingRead.qualificationDataEvidence -and
        [string]$resultsBindingRead.qualificationDataEvidence.manifestSha256 -ceq
            [string]$resultsBindingRead.qualificationData.manifestSha256 -and
        [string]$resultsBindingRead.qualificationDataEvidence.closureSha256 -ceq
            [string]$resultsBindingRead.qualificationData.closureSha256 -and
        [int]$resultsBindingRead.qualificationDataEvidence.fileCount -eq 6) `
        'validation-results host receipt retains typed qualification-data proof through final acceptance'
    $resultsBindingDocument = Read-TestJson $resultsBindingPath
    $resultsBindingDocument.details.resultsSha256 = '0' * 64
    Write-JsonDocument $resultsBindingPath $resultsBindingDocument
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @resultsBindingArgs | Out-Null
    } 'bind exactly one retained validation-results log' `
        'validation-results details cannot detach their aggregate hash from the retained raw-log snapshot'

    # Artifact upload/download rebases the evidence root while immutable native
    # receipts retain their producer-observed absolute raw paths.  Acceptance
    # must derive a unique byte-bound staged mapping without rewriting either
    # the host wrapper or native receipt.
    $evidenceModule = Get-Module DeterministicSimulationEvidence
    function New-Stage5RelocationTestCase {
        param([string]$Name)
        $caseRoot = Join-Path $acceptanceRoot "native-relocation-$Name"
        New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
        $receiptPath = Join-Path $caseRoot 'validation-results-receipt.json'
        Write-Stage5HostReceiptTestDocument $receiptPath 'validation-results' `
            'ZeroHour' $sourceCommit $artifactSetHash $artifactTestHashes
        $wrapper = Read-TestJson $receiptPath
        $nativeReference = $wrapper.provenance.children[0].nativeReceipt
        $nativePath = Join-Path $caseRoot ([string]$nativeReference.path)
        $native = Read-TestJson $nativePath
        $stagedRawDirectory = Join-Path $caseRoot 'uploaded-native'
        New-Item -ItemType Directory -Path $stagedRawDirectory -Force | Out-Null
        foreach ($raw in @($native.rawLogs)) {
            $source = Join-Path $caseRoot ([string]$raw.path)
            $leaf = [IO.Path]::GetFileName($source)
            $destination = Join-Path $stagedRawDirectory $leaf
            Move-Item -LiteralPath $source -Destination $destination
            $raw.path = "H:\Stage5SimulationValidationTask\Evidence\uploaded-native\$leaf"
            if ([string]$raw.name -ceq 'raw-log') {
                $native.rawEvidence.rawLogPath = [string]$raw.path
            }
            else {
                $native.rawEvidence.timingPath = [string]$raw.path
            }
        }
        $native.provenance.receiptPath =
            "H:\Stage5SimulationValidationTask\Evidence\$([IO.Path]::GetFileName($nativePath))"
        Write-JsonDocument $nativePath $native
        $nativeReference.sha256 = Get-Sha256 $nativePath
        Write-JsonDocument $receiptPath $wrapper
        return [pscustomobject]@{
            root = $caseRoot; receiptPath = $receiptPath
            nativePath = $nativePath
        }
    }
    function Get-Stage5RelocationTestBinding {
        param([object]$Case)
        return & $evidenceModule {
            param($receiptPath, $evidenceDirectory)
            Get-Stage5FinalAcceptanceNativeRelocationBinding `
                -Path $receiptPath -EvidenceDirectory $evidenceDirectory
        } $Case.receiptPath $Case.root
    }
    function Update-Stage5RelocationTestNative {
        param([object]$Case, [object]$Native)
        Write-JsonDocument $Case.nativePath $Native
        $wrapper = Read-TestJson $Case.receiptPath
        $wrapper.provenance.children[0].nativeReceipt.sha256 =
            Get-Sha256 $Case.nativePath
        Write-JsonDocument $Case.receiptPath $wrapper
    }

    $relocationPositive = New-Stage5RelocationTestCase 'positive'
    try {
        $relocationBinding = Get-Stage5RelocationTestBinding $relocationPositive
        Assert-True (@($relocationBinding.nativeRawBindings).Count -eq 2 -and
            @($relocationBinding.nativeRawBindings | Where-Object {
                [string]$_.sourcePath -match '^H:\\Stage5SimulationValidationTask\\Evidence\\' -and
                [string]$_.path -match '^uploaded-native[\\/]' -and
                [string]$_.path -notmatch '^[A-Za-z]:|^[\\/]'
            }).Count -eq 2 -and
            [string]$relocationBinding.nativeReceiptSourcePath -match
                '^H:\\Stage5SimulationValidationTask\\Evidence\\') `
            'acceptance derives exact relative bindings for immutable uploaded native paths'
        $relocationReadArgs = @{
            Path = $relocationPositive.receiptPath; Kind = 'deterministic-runtime'
            Role = 'validation-results'; EvidenceTitle = 'ZeroHour'
            ExpectedSourceCommit = $sourceCommit
            ExpectedArtifactSetSha256 = $artifactSetHash
            ArtifactHashes = $artifactTestHashes
            ExpectedCohortNonce = $script:TestCohortNonce
            ExpectedCohortCreatedUtc = $script:TestCohortCreatedUtc
            ExpectedRuntimeClosure = $script:TestRuntimeClosure
            ExpectedEvidenceDirectory = $relocationBinding.evidenceDirectory
            NativeRawBindings = $relocationBinding.nativeRawBindings
            NativeReceiptSourcePath = $relocationBinding.nativeReceiptSourcePath
        }
        Read-Stage5FinalAcceptanceImmutableReceipt @relocationReadArgs | Out-Null
    }
    catch {
        Assert-True $false "uploaded immutable native evidence should relocate safely: $($_.Exception.Message)"
    }

    $relocationAlias = New-Stage5RelocationTestCase 'alias'
    $aliasNative = Read-TestJson $relocationAlias.nativePath
    $aliasNative.rawLogs[1].path = [string]$aliasNative.rawLogs[0].path
    $aliasNative.rawLogs[1].sha256 = [string]$aliasNative.rawLogs[0].sha256
    $aliasNative.rawEvidence.timingPath = [string]$aliasNative.rawLogs[0].path
    $aliasNative.rawEvidence.timingSha256 = [string]$aliasNative.rawLogs[0].sha256
    Update-Stage5RelocationTestNative $relocationAlias $aliasNative
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationAlias | Out-Null
    } 'aliases another staged raw log|alias' `
        'native relocation rejects two receipt observations mapped to one staged file'

    $relocationMissing = New-Stage5RelocationTestCase 'missing'
    $missingNative = Read-TestJson $relocationMissing.nativePath
    $missingLeaf = [IO.Path]::GetFileName([string]$missingNative.rawLogs[1].path)
    Remove-Item -LiteralPath (Join-Path $relocationMissing.root `
        "uploaded-native\$missingLeaf")
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationMissing | Out-Null
    } 'no staged candidate|missing' `
        'native relocation rejects a missing uploaded raw file'

    $relocationAmbiguous = New-Stage5RelocationTestCase 'ambiguous'
    $ambiguousNative = Read-TestJson $relocationAmbiguous.nativePath
    $ambiguousLeaf = [IO.Path]::GetFileName([string]$ambiguousNative.rawLogs[0].path)
    $ambiguousDuplicateDirectory = Join-Path $relocationAmbiguous.root `
        'duplicate\uploaded-native'
    New-Item -ItemType Directory -Path $ambiguousDuplicateDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $relocationAmbiguous.root `
        "uploaded-native\$ambiguousLeaf") -Destination `
        (Join-Path $ambiguousDuplicateDirectory $ambiguousLeaf)
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationAmbiguous | Out-Null
    } 'ambiguous|multiple staged candidates' `
        'native relocation rejects equally specific duplicate uploaded files'

    $relocationWrongHash = New-Stage5RelocationTestCase 'wrong-hash'
    $wrongHashNative = Read-TestJson $relocationWrongHash.nativePath
    $wrongHashLeaf = [IO.Path]::GetFileName([string]$wrongHashNative.rawLogs[0].path)
    [IO.File]::AppendAllText((Join-Path $relocationWrongHash.root `
        "uploaded-native\$wrongHashLeaf"), 'tampered')
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationWrongHash | Out-Null
    } 'SHA-256|hash' `
        'native relocation rejects a staged file with the wrong bytes'

    $relocationStaleSource = New-Stage5RelocationTestCase 'stale-source'
    $staleSourceNative = Read-TestJson $relocationStaleSource.nativePath
    $staleSourceLeaf = [IO.Path]::GetFileName([string]$staleSourceNative.rawLogs[0].path)
    $staleSourceNative.rawLogs[0].path =
        "H:\Stage5SimulationValidationTask\Evidence\stale-parent\$staleSourceLeaf"
    $staleSourceNative.rawEvidence.rawLogPath =
        [string]$staleSourceNative.rawLogs[0].path
    Update-Stage5RelocationTestNative $relocationStaleSource $staleSourceNative
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationStaleSource | Out-Null
    } 'no staged candidate|source path|stale' `
        'native relocation rejects a stale source path that matches only by leaf name'

    $relocationStaleReceipt = New-Stage5RelocationTestCase 'stale-receipt-source'
    $staleReceiptNative = Read-TestJson $relocationStaleReceipt.nativePath
    $staleReceiptNative.provenance.receiptPath =
        'H:\Stage5SimulationValidationTask\Evidence\different-native-receipt.json'
    Update-Stage5RelocationTestNative $relocationStaleReceipt $staleReceiptNative
    Assert-Throws {
        Get-Stage5RelocationTestBinding $relocationStaleReceipt | Out-Null
    } 'source receiptPath|does not end with|stale' `
        'native relocation rejects a stale producer receipt path'

    $schemaExecutablePath = Join-Path $attachmentRoot 'schema-executable-receipt.json'
    Write-Stage5ExecutableReceiptTestDocument $schemaExecutablePath `
        'performance-report' 'ZeroHour' $sourceCommit $artifactSetHash `
        $artifactTestHashes
    Assert-True (Test-Stage5TestJsonSchema `
        (Get-Content -LiteralPath $schemaExecutablePath -Raw) `
        $immutableReceiptSchema) `
        'the immutable schema accepts the current schema-1 executable-domain envelope around a V5 native receipt'
    $schemaV5Envelope = Get-Content -LiteralPath $schemaExecutablePath -Raw |
        ConvertFrom-Json
    $schemaV5Envelope.schemaVersion = 5
    Assert-True (-not (Test-Stage5TestJsonSchema `
        ($schemaV5Envelope | ConvertTo-Json -Depth 100) `
        $immutableReceiptSchema)) `
        'the immutable envelope schema must reject an impossible schema-5 outer receipt'
    $immutableReceipt = Get-Content -LiteralPath $immutableReceiptPath -Raw | ConvertFrom-Json
    $receiptContractArgs = @{
        Path = $immutableReceiptPath; Kind = 'deterministic-runtime'
        Role = 'validation-plan'; EvidenceTitle = 'ZeroHour'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
    }
    $receiptRead = Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs
    Assert-True ($null -eq $receiptRead.acceptanceFailure -and
        $receiptRead.trustDomain -ceq 'host-runner' -and
        $receiptRead.producer -ceq 'installed-runtime-validation-plan-v2') `
        'a host-runner receipt passes only after its exact producer and trust domain are validated'
    $immutableReceipt.sourceCommit = 'b' * 40
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'stale or does not match' `
        'a stale receipt from another source commit is rejected'
    $immutableReceipt.sourceCommit = $sourceCommit
    $immutableReceipt.role = 'validation-results'
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'role is substituted' `
        'a receipt substituted from another role is rejected'
    $immutableReceipt.role = 'validation-plan'
    $immutableReceipt.executableSha256 = '0' * 64
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'executable SHA-256 binding' `
        'a receipt bound to the wrong executable hash is rejected'
    $immutableReceipt.executableSha256 = $artifactTestHashes['zerohour-executable']
    $immutableReceipt.rawLogs[0].sha256 = '0' * 64
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'raw log.*SHA-256 mismatch' `
        'a receipt with a substituted raw log hash is rejected'
    $immutableReceipt.rawLogs[0].sha256 = Get-Sha256 (Join-Path $attachmentRoot `
        'immutable-receipt-validation-plan.raw.log')
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    $replayedNonces = @{}
    $replayArgs = @{} + $receiptContractArgs
    $replayArgs['SeenRunNonces'] = $replayedNonces
    Read-Stage5FinalAcceptanceImmutableReceipt @replayArgs | Out-Null
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @replayArgs | Out-Null
    } 'replayed.*runNonce' `
        'a replayed receipt nonce is rejected even when its bytes are unchanged'

    $immutableReceipt.trustDomain = 'executable'
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'trustDomain is substituted' `
        'a receipt cannot cross from the host-runner domain into the executable domain'
    $immutableReceipt.trustDomain = 'host-runner'
    $immutableReceipt.provenance.childProvenance = 'bound'
    $immutableReceipt.provenance.children = @()
    Write-JsonDocument $immutableReceiptPath $immutableReceipt
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @receiptContractArgs | Out-Null
    } 'claims child provenance' `
        'a host-runner plan cannot claim child provenance that it did not bind'
    $immutableReceipt.provenance.childProvenance = 'not-applicable'
    $immutableReceipt.provenance.children = @()
    Write-JsonDocument $immutableReceiptPath $immutableReceipt

    Assert-CurrentNativeReceiptCatalog $attachmentRoot $sourceCommit $artifactSetHash $artifactTestHashes

    $executableReceiptPath = Join-Path $attachmentRoot `
        'executable-validation-results.json'
    Write-Stage5ExecutableReceiptTestDocument $executableReceiptPath `
        'validation-results' 'ZeroHour' $sourceCommit $artifactSetHash $artifactTestHashes
    $executableArgs = @{
        Path = $executableReceiptPath; Kind = 'deterministic-runtime'
        Role = 'validation-results'; EvidenceTitle = 'ZeroHour'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
    }
    $executableRead = Read-Stage5FinalAcceptanceImmutableReceipt @executableArgs
    Assert-True ($executableRead.trustDomain -ceq 'executable' -and
        $executableRead.producer -ceq 'game-executable-stage5-performance-report-v5' -and
        $executableRead.provenance.kind -ceq 'native-executable-observation') `
        'an executable-originated receipt is accepted only with its role-bound native receipt'
    $nativeReceiptPath = Join-Path $attachmentRoot 'executable-validation-results.native.json'
    $nativeReceiptText = [IO.File]::ReadAllText($nativeReceiptPath)
    [IO.File]::AppendAllText($nativeReceiptPath, "`n tampered")
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @executableArgs | Out-Null
    } 'native provenance receipt.*SHA-256 mismatch' `
        'an executable receipt rejects a tampered native receipt'
    [IO.File]::WriteAllText($nativeReceiptPath, $nativeReceiptText)

    # Reuse the title-specific synthetic reviewed closure for the protected
    # reader regression below.  Its receipt, manifest, and attestation remain
    # co-located under the test-only corpus root.
    $reviewedReceiptPath = [string]$syntheticZeroHour.reviewed.receiptPath
    $reviewedArgs = @{
        Path = $reviewedReceiptPath; Kind = 'replay-determinism'
        Role = 'replay-fixture-manifest'; EvidenceTitle = 'ZeroHour'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
    }
    $reviewedRead = Read-Stage5FinalAcceptanceImmutableReceipt @reviewedArgs
    Assert-True ($reviewedRead.trustDomain -ceq 'reviewed-fixture' -and
        $reviewedRead.protection.kind -ceq 'external-reviewed-fixture-attestation') `
        'reviewed fixture receipts use the external reviewed-fixture trust domain'
    $reviewedManifestPath = [string]$syntheticZeroHour.reviewed.manifestPath
    $reviewedManifestText = [IO.File]::ReadAllText($reviewedManifestPath)
    $reviewedReceiptText = [IO.File]::ReadAllText($reviewedReceiptPath)
    [IO.File]::AppendAllText($reviewedManifestPath, "`n tampered")
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @reviewedArgs | Out-Null
    } 'fixture manifest.*SHA-256 mismatch' `
        'reviewed fixture receipts reject a changed nested fixture manifest'
    [IO.File]::WriteAllText($reviewedManifestPath, $reviewedManifestText)
    $reviewedDocument = Get-Content -LiteralPath $reviewedReceiptPath -Raw | ConvertFrom-Json
    $reviewedDocument.trustDomain = 'host-runner'
    Write-JsonDocument $reviewedReceiptPath $reviewedDocument
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @reviewedArgs | Out-Null
    } 'trustDomain is substituted' `
        'a reviewed fixture receipt cannot be reclassified as host-runner evidence'
    [IO.File]::WriteAllText($reviewedReceiptPath, $reviewedReceiptText)

    $premiumReceiptPath = Join-Path $attachmentRoot `
        'premium-review-premium-review-results.json'
    $premiumArgs = @{
        Path = $premiumReceiptPath; Kind = 'premium-review'
        Role = 'premium-review-results'; EvidenceTitle = 'Both'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
    }
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @premiumArgs | Out-Null
    } 'genuinely independent premium-review authority|writable local JSON' `
        'local premium review JSON cannot mint a final-acceptance authority'
    $premiumReceiptText = [IO.File]::ReadAllText($premiumReceiptPath)
    $premiumDocument = Get-Content -LiteralPath $premiumReceiptPath -Raw | ConvertFrom-Json
    $premiumDocument.protection.kind = 'external-user-manual-approval-attestation'
    Write-JsonDocument $premiumReceiptPath $premiumDocument
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @premiumArgs | Out-Null
    } 'protection kind is not external/protected|genuinely independent premium-review authority' `
        'premium review rejects a manual-approval protection marker'
    [IO.File]::WriteAllText($premiumReceiptPath, $premiumReceiptText)

    $manualReceiptPath = Join-Path $attachmentRoot `
        'manual-acceptance-manual-checklist.json'
    $manualArgs = @{
        Path = $manualReceiptPath; Kind = 'manual-acceptance'
        Role = 'manual-checklist'; EvidenceTitle = 'Both'
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ArtifactHashes = $artifactTestHashes
    }
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @manualArgs | Out-Null
    } 'genuinely independent manual-approval authority|writable local JSON' `
        'local manual approval JSON cannot mint a final-acceptance authority'
    $manualReceiptText = [IO.File]::ReadAllText($manualReceiptPath)
    $manualDocument = Get-Content -LiteralPath $manualReceiptPath -Raw | ConvertFrom-Json
    $manualDocument.provenance.kind = 'host-runner'
    Write-JsonDocument $manualReceiptPath $manualDocument
    Assert-Throws {
        Read-Stage5FinalAcceptanceImmutableReceipt @manualArgs | Out-Null
    } 'authorized protected record|genuinely independent manual-approval authority' `
        'manual acceptance rejects a caller-forged host-runner provenance marker'
    [IO.File]::WriteAllText($manualReceiptPath, $manualReceiptText)

    $acceptanceOutput = Join-Path $acceptanceRoot 'final-acceptance-report.json'
    & (Join-Path $PSScriptRoot 'Invoke-Stage5FinalAcceptance.ps1') `
        -AcceptanceManifestPath $acceptanceRequest -OutputPath $acceptanceOutput `
        -DevelopmentReadiness | Out-Null
    $acceptanceReport = Get-Content -LiteralPath $acceptanceOutput -Raw | ConvertFrom-Json
    Assert-True ($acceptanceReport.status -ceq 'ready-for-manual-approval' -and
        $acceptanceReport.gateName -ceq 'stage5-development-readiness' -and
        -not [bool]$acceptanceReport.finalAcceptanceClaim -and
        [bool]$acceptanceReport.premiumReviewRequired -and
        [bool]$acceptanceReport.manualApprovalRequired) `
        'development readiness writes a non-final report after installed lockstep-v2 evidence is attached'

    $missingManualRequest = Join-Path $acceptanceRoot 'missing-manual.json'
    Write-AcceptanceRequest $missingManualRequest @($acceptanceKinds | Where-Object {
        $_ -cne 'manual-acceptance'
    })
    $missingManualReport = Invoke-Stage5FinalAcceptanceAggregation $missingManualRequest
    Assert-True ($missingManualReport.status -ceq 'ready-for-manual-approval' -and
        -not [bool]$missingManualReport.finalAcceptanceClaim -and
        [bool]$missingManualReport.manualApprovalRequired) `
        'pre-manual development readiness remains non-final when manual evidence is absent'

    $combinedDocument = $evidenceDocuments['combined-stage4-stage5-installed-runtime']
    $combinedDocument.details.pipelineMode = 'serial'
    Write-JsonDocument $evidencePaths['combined-stage4-stage5-installed-runtime'] $combinedDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'requires pipelineMode=parallel' `
        'final acceptance rejects a serial Stage 4 pipeline masquerading as the combined policy lane'
    $combinedDocument.details.pipelineMode = 'parallel'
    Write-JsonDocument $evidencePaths['combined-stage4-stage5-installed-runtime'] $combinedDocument

    $staleArtifactDocument = $evidenceDocuments['deterministic-runtime']
    $staleArtifactDocument.artifactSetSha256 = 'B' * 64
    Write-JsonDocument $evidencePaths['deterministic-runtime'] $staleArtifactDocument
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'does not identify the same passed x64 commit and artifact set' `
        'final acceptance rejects evidence from a different artifact set'
    $staleArtifactDocument.artifactSetSha256 = $artifactSetHash
    Write-JsonDocument $evidencePaths['deterministic-runtime'] $staleArtifactDocument

    $net3Manifest = Join-Path $attachmentRoot 'mixed-worker-multiplayer-multiplayer-results.json'
    $net3Proof = Read-Stage5Net3LoopbackEvidence $net3Manifest $sourceCommit $artifactSetHash `
        $artifactTestHashes['generals-executable'] $artifactTestHashes['zerohour-executable']
    Assert-True ($net3Proof.provenKernelMask -eq 0x3F -and
        $net3Proof.matchCount -eq 16 -and $net3Proof.peerRecordCount -eq 40) `
        'canonical NET3 evidence proves six kernels across exactly 16 matches and 40 peer records'
    $proofDirectory = Join-Path $acceptanceRoot 'external-proof'
    New-Item -ItemType Directory -Path $proofDirectory | Out-Null
    $proofEvidence = Join-Path $proofDirectory 'Net3LoopbackEvidence.json'
    $proofArtifactSet = Join-Path $proofDirectory 'Stage5ArtifactSet.json'
    Copy-Item -LiteralPath $net3Manifest -Destination $proofEvidence
    Copy-Item -LiteralPath $artifactSetPath -Destination $proofArtifactSet
    $proofManifest = Get-Content -LiteralPath $proofEvidence -Raw | ConvertFrom-Json
    $proofRawLines = @('RTS_MULTIPLAYER_SIMULATION_RAW_EVIDENCE_V1')
    $proofRawOrdinal = 0
    foreach ($proofMatch in $proofManifest.matches) {
        foreach ($proofPeer in $proofMatch.peers) {
            $sourceRaw = Join-Path (Split-Path -Parent $net3Manifest) $proofPeer.rawOutputPath
            $destinationRaw = Join-Path $proofDirectory $proofPeer.rawOutputPath
            $destinationParent = Split-Path -Parent $destinationRaw
            if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
                New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
            }
            Copy-Item -LiteralPath $sourceRaw -Destination $destinationRaw
            $proofRawLines += ('{0:D2}|{1}|{2}' -f $proofRawOrdinal,
                $proofPeer.rawOutputPath, (Get-Sha256 $destinationRaw))
            ++$proofRawOrdinal
        }
    }
    $proofRawLines += 'END'
    $proofRawIndex = Join-Path $proofDirectory 'MultiplayerSimulationRawEvidence.index'
    [IO.File]::WriteAllText($proofRawIndex, (($proofRawLines -join "`n") + "`n"))
    $proofFile = Join-Path $proofDirectory 'MultiplayerSimulationRuntimeProof.txt'
    & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
        -EvidenceManifestPath $proofEvidence -RawEvidenceIndexPath $proofRawIndex `
        -ArtifactSetManifestPath $proofArtifactSet -OutputPath $proofFile `
        -ExpectedSourceCommit $sourceCommit -ExpectedArtifactSetSha256 $artifactSetHash `
        -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
        -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] `
        -Title Generals -ExpectedBuildCompatibilityCrc 287454020 `
        -ExpectedContentCrc 2864434397 | Out-Null
    $proofContent = Get-Content -LiteralPath $proofFile -Raw
    Assert-True ($proofContent -match '^RTS_MULTIPLAYER_SIMULATION_RUNTIME_PROOF_V1' -and
        $proofContent -match 'proven_kernel_mask=63' -and
        $proofContent -match 'producer=installed-runtime-runner-v1' -and
        $proofContent -match 'validation_mode=scoped-net3-loopback-release-proof' -and
        $proofContent -match 'build_compatibility_crc=287454020' -and
        $proofContent.Contains($artifactTestHashes['generals-executable']) -and
        $proofContent.Contains($sourceCommit) -and
        $proofContent.Contains((Get-Sha256 $net3Manifest))) `
        'external proof binds unchanged executable, build, content, schema, runner, and raw evidence'

    $misorderedProofDirectory = Join-Path $acceptanceRoot 'misordered-proof'
    New-Item -ItemType Directory -Path $misorderedProofDirectory | Out-Null
    Copy-Item -Path (Join-Path $proofDirectory '*') -Destination $misorderedProofDirectory `
        -Recurse
    Remove-Item -LiteralPath (Join-Path $misorderedProofDirectory `
        'MultiplayerSimulationRuntimeProof.txt')
    $misorderedRawIndex = Join-Path $misorderedProofDirectory `
        'MultiplayerSimulationRawEvidence.index'
    $misorderedLines = @(Get-Content -LiteralPath $misorderedRawIndex)
    $firstEntry = $misorderedLines[1]
    $secondEntry = $misorderedLines[2]
    $misorderedLines[1] = '00' + $secondEntry.Substring(2)
    $misorderedLines[2] = '01' + $firstEntry.Substring(2)
    [IO.File]::WriteAllText($misorderedRawIndex,
        (($misorderedLines -join "`n") + "`n"))
    $misorderedProofFile = Join-Path $misorderedProofDirectory `
        'MultiplayerSimulationRuntimeProof.txt'
    Assert-Throws {
        & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
            -EvidenceManifestPath (Join-Path $misorderedProofDirectory `
                'Net3LoopbackEvidence.json') -RawEvidenceIndexPath $misorderedRawIndex `
            -ArtifactSetManifestPath (Join-Path $misorderedProofDirectory `
                'Stage5ArtifactSet.json') -OutputPath $misorderedProofFile `
            -ExpectedSourceCommit $sourceCommit -ExpectedArtifactSetSha256 $artifactSetHash `
            -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
            -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] `
            -Title Generals -ExpectedBuildCompatibilityCrc 287454020 `
            -ExpectedContentCrc 2864434397 | Out-Null
    } 'does not match the canonical evidence peer order' `
        'external proof rejects a valid raw record set reordered away from canonical peer provenance'
    Assert-True (-not (Test-Path -LiteralPath $misorderedProofFile)) `
        'misordered raw evidence leaves the external proof absent'

    $net3FixtureRoot = Split-Path -Parent $net3Manifest
    $missingNet3Path = Join-Path $net3FixtureRoot 'net3-missing-match.json'
    $missingNet3 = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $missingNet3.matches = @($missingNet3.matches | Select-Object -First 15)
    Write-JsonDocument $missingNet3Path $missingNet3
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $missingNet3Path $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'exactly 16 match records' 'NET3 evidence rejects a missing match record'

    $duplicateNet3Path = Join-Path $net3FixtureRoot 'net3-duplicate-match.json'
    $duplicateNet3 = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $duplicateNet3.matches[15] = $duplicateNet3.matches[0]
    Write-JsonDocument $duplicateNet3Path $duplicateNet3
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $duplicateNet3Path $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'not canonical' 'NET3 evidence rejects a duplicate match disguised as the final record'

    $missingPeerPath = Join-Path $net3FixtureRoot 'net3-missing-peer.json'
    $missingPeer = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $missingPeer.matches[0].peers = @($missingPeer.matches[0].peers | Select-Object -First 1)
    Write-JsonDocument $missingPeerPath $missingPeer
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $missingPeerPath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'exact topology peer roster' 'NET3 evidence rejects a missing nested peer record'

    $wrongProvenancePath = Join-Path $net3FixtureRoot 'net3-wrong-provenance.json'
    $wrongProvenance = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $wrongProvenance.sourceCommit = 'b' * 40
    Write-JsonDocument $wrongProvenancePath $wrongProvenance
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $wrongProvenancePath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'source commit does not match independent provenance' `
        'NET3 evidence rejects self-consistent but independently wrong provenance'

    $tamperedKernelPath = Join-Path $net3FixtureRoot 'net3-tampered-kernel.json'
    $tamperedKernel = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
    $tamperedKernel.matches[0].peers[1].kernels[0].physicalWorkerJobs = 0
    Write-JsonDocument $tamperedKernelPath $tamperedKernel
    Assert-Throws {
        Read-Stage5Net3LoopbackEvidence $tamperedKernelPath $sourceCommit $artifactSetHash `
            $artifactTestHashes['generals-executable'] `
            $artifactTestHashes['zerohour-executable'] | Out-Null
    } 'raw counters do not match' 'NET3 evidence rejects tampered physical-worker proof'
	$synchronizedMaskPath = Join-Path $net3FixtureRoot 'net3-synchronized-mask-mismatch.json'
	$synchronizedMask = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
	$synchronizedPeer = $synchronizedMask.matches[0].peers[1]
	$synchronizedPeer.kernels[0].physicalWorkerMask = 7
	$sourceSynchronizedRawPath = Join-Path $net3FixtureRoot $synchronizedPeer.rawOutputPath
	$synchronizedRaw = Get-Content -LiteralPath $sourceSynchronizedRawPath -Raw | ConvertFrom-Json
	$synchronizedRaw.kernels[0].physicalWorkerMask = 7
	$synchronizedRawRelative = 'Net3Raw\net3-synchronized-mask-mismatch.log'
	$synchronizedRawPath = Join-Path $net3FixtureRoot $synchronizedRawRelative
	Write-JsonDocument $synchronizedRawPath $synchronizedRaw
	$synchronizedPeer.rawOutputPath = $synchronizedRawRelative
	$synchronizedPeer.rawOutputSha256 = Get-Sha256 $synchronizedRawPath
	Write-JsonDocument $synchronizedMaskPath $synchronizedMask
	Assert-Throws {
		Read-Stage5Net3LoopbackEvidence $synchronizedMaskPath $sourceCommit `
			$artifactSetHash $artifactTestHashes['generals-executable'] `
			$artifactTestHashes['zerohour-executable'] | Out-Null
	} 'complete physical-worker mask/count is inconsistent' `
		'NET3 evidence rejects a synchronized single-batch mask/distinct mismatch'
	$incompleteStatusPath = Join-Path $net3FixtureRoot 'net3-incomplete-status-mask.json'
	$incompleteStatus = Get-Content -LiteralPath $net3Manifest -Raw | ConvertFrom-Json
	$incompletePeer = $incompleteStatus.matches[0].peers[1]
	$incompletePeer.kernels[1].physicalWorkerMaskComplete = $false
	$sourceStatusRawPath = Join-Path $net3FixtureRoot $incompletePeer.rawOutputPath
	$incompleteStatusRaw = Get-Content -LiteralPath $sourceStatusRawPath -Raw | ConvertFrom-Json
	$incompleteStatusRaw.kernels[1].physicalWorkerMaskComplete = $false
	$incompleteStatusRawRelative = 'Net3Raw\net3-incomplete-status-mask.log'
	$incompleteStatusRawPath = Join-Path $net3FixtureRoot $incompleteStatusRawRelative
	Write-JsonDocument $incompleteStatusRawPath $incompleteStatusRaw
	$incompletePeer.rawOutputPath = $incompleteStatusRawRelative
	$incompletePeer.rawOutputSha256 = Get-Sha256 $incompleteStatusRawPath
	Write-JsonDocument $incompleteStatusPath $incompleteStatus
	Assert-Throws {
		Read-Stage5Net3LoopbackEvidence $incompleteStatusPath `
			$sourceCommit $artifactSetHash $artifactTestHashes['generals-executable'] `
			$artifactTestHashes['zerohour-executable'] | Out-Null
	} 'physical-worker mask is incomplete inside the representable worker lane' `
		'NET3 evidence rejects an incomplete 64-bit mask when every configured worker is representable'
    $invalidProofDirectory = Join-Path $acceptanceRoot 'invalid-proof'
    New-Item -ItemType Directory -Path $invalidProofDirectory | Out-Null
    Copy-Item -Path (Join-Path $proofDirectory '*') -Destination $invalidProofDirectory `
        -Recurse
    Remove-Item -LiteralPath (Join-Path $invalidProofDirectory `
        'MultiplayerSimulationRuntimeProof.txt')
    Copy-Item -LiteralPath $tamperedKernelPath -Destination (Join-Path `
        $invalidProofDirectory 'Net3LoopbackEvidence.json') -Force
    $invalidProofFile = Join-Path $invalidProofDirectory `
        'MultiplayerSimulationRuntimeProof.txt'
    $invalidRawIndex = Join-Path $invalidProofDirectory `
        'MultiplayerSimulationRawEvidence.index'
    $invalidArtifactSet = Join-Path $invalidProofDirectory 'Stage5ArtifactSet.json'
    Assert-Throws {
        & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
            -EvidenceManifestPath (Join-Path $invalidProofDirectory `
                'Net3LoopbackEvidence.json') -RawEvidenceIndexPath $invalidRawIndex `
            -ArtifactSetManifestPath $invalidArtifactSet -OutputPath $invalidProofFile `
            -ExpectedSourceCommit $sourceCommit -ExpectedArtifactSetSha256 $artifactSetHash `
            -ExpectedGeneralsExecutableSha256 $artifactTestHashes['generals-executable'] `
            -ExpectedZeroHourExecutableSha256 $artifactTestHashes['zerohour-executable'] `
            -Title Generals -ExpectedBuildCompatibilityCrc 287454020 `
            -ExpectedContentCrc 2864434397 | Out-Null
    } 'raw counters do not match' `
        'invalid evidence cannot generate a nonzero multiplayer release proof'
    Assert-True (-not (Test-Path -LiteralPath $invalidProofFile)) `
        'invalid evidence leaves the external proof absent so the runtime mask remains zero'

    $scalingManifest = Join-Path $attachmentRoot 'performance-scaling-performance-report.json'
    $scalingBaselineHash = Get-Sha256 (Join-Path $attachmentRoot `
        'performance-scaling-stage3-baseline.json')
    Assert-True ($null -ne $performanceFixtureBinding) `
        'canonical acceptance evidence retains its authoritative performance-fixture binding'
    $scalingReaderArguments = @{
        ExpectedSourceCommit = $sourceCommit
        ExpectedArtifactSetSha256 = $artifactSetHash
        ExpectedExecutableSha256 = $artifactTestHashes['zerohour-executable']
        ExpectedStage3BaselineSha256 = $scalingBaselineHash
        ExpectedTitle = 'ZeroHour'
        ExpectedCohortNonce = $script:TestCohortNonce
        ExpectedCohortCreatedUtc = $script:TestCohortCreatedUtc
        ExpectedRuntimeClosure = $script:TestRuntimeClosure
        ExpectedPhaseBaselineProfileSha256 =
            $performanceFixtureBinding.phaseBaselineProfileSha256
    }
    $scalingProof = Read-Stage5PerformanceScalingEvidence `
        -Path $scalingManifest @scalingReaderArguments
    Assert-True ($scalingProof.physicalCoreCount -eq 16 -and
        $scalingProof.fixtureCount -eq 4 -and $scalingProof.kernelCount -eq 6 -and
        [Math]::Abs([double]$scalingProof.maximumOneWorkerRegressionRatio -
            [double]$performanceFixtureBinding.maximumOneWorkerRegressionRatio) -le 1.0e-12 -and
        [Math]::Abs([double]$scalingProof.minimumEightWorkerSpeedup -
            [double]$performanceFixtureBinding.minimumEightWorkerSpeedup) -le 1.0e-12 -and
        [Math]::Abs([double]$scalingProof.minimumEightToSixteenSpeedup -
            [double]$performanceFixtureBinding.minimumEightToSixteenSpeedup) -le 1.0e-12) `
        'canonical scaling evidence proves physical topology, realistic fixtures, and six kernels'

    Assert-PerformanceDiagnosticsConversion (Join-Path $attachmentRoot 'scaling-diagnostics-input.json') `
        $sourceCommit $artifactSetHash $artifactTestHashes['zerohour-executable']

    $versionOneScalingPath = Join-Path $attachmentRoot 'scaling-version-one.json'
    $versionOneScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $versionOneScaling.schemaVersion = 1
    Write-JsonDocument $versionOneScalingPath $versionOneScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $versionOneScalingPath `
            @scalingReaderArguments | Out-Null
    } 'provenance is invalid|schema' `
        'authoritative scaling evidence rejects the superseded schema-v1 envelope'

    $missingScalingPath = Join-Path $attachmentRoot 'scaling-missing-fixture.json'
    $missingScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $missingScaling.fixtures = @($missingScaling.fixtures | Select-Object -First 3)
    Write-JsonDocument $missingScalingPath $missingScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $missingScalingPath `
            @scalingReaderArguments | Out-Null
    } 'exact 1k, 4k, 8k, and dense eight-player fixtures' `
        'scaling evidence rejects a missing realistic fixture'

    $tamperedScalingPath = Join-Path $attachmentRoot 'scaling-tampered-kernel.json'
    $tamperedScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $tamperedScaling.kernelTimings[0].totalParallelMilliseconds = 5.0
    Write-JsonDocument $tamperedScalingPath $tamperedScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $tamperedScalingPath `
            @scalingReaderArguments | Out-Null
    } 'does not match raw installed runs' `
        'scaling evidence rejects tampered aggregate kernel timing'

    $forgedScalingPath = Join-Path $attachmentRoot 'scaling-forged-summary.json'
    $forgedScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $forgedScaling.fixtures[0].stage5OneWorkerMilliseconds = 1010.0
    $forgedScaling.fixtures[0].oneWorkerRegressionRatio = 1.01
    $forgedScaling.fixtures[0].eightPhysicalCoreSpeedup = 2.02
    Write-JsonDocument $forgedScalingPath $forgedScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $forgedScalingPath `
            @scalingReaderArguments | Out-Null
    } 'does not match raw per-repeat medians' `
        'internally consistent forged summary cannot replace installed raw samples'

    $forgedCommandPath = Join-Path $attachmentRoot 'scaling-forged-command.json'
    $forgedCommand = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $canonicalRawPath = Join-Path $attachmentRoot $forgedCommand.rawSampleManifest.path
    $canonicalClosureRoot = Split-Path -Parent $canonicalRawPath
    $forgedClosureLeaf = 'scaling-forged-command.authoritative'
    $forgedClosureRoot = Join-Path $attachmentRoot $forgedClosureLeaf
    New-Item -ItemType Directory -Path $forgedClosureRoot | Out-Null
    Copy-Item -Path (Join-Path $canonicalClosureRoot '*') `
        -Destination $forgedClosureRoot -Recurse
    $forgedRawPath = Join-Path $forgedClosureRoot `
        'Stage5PerformanceScalingRawSamples.json'
    $forgedRaw = Get-Content -LiteralPath $canonicalRawPath -Raw | ConvertFrom-Json
    $forgedRaw.fixtureSamples[0].commandLine += ' -repeat 0'
    Write-JsonDocument $forgedRawPath $forgedRaw
    $forgedCommand.rawSampleManifest.path =
        "$forgedClosureLeaf/Stage5PerformanceScalingRawSamples.json"
    $forgedCommand.rawSampleManifest.sha256 = Get-Sha256 $forgedRawPath
    Write-JsonDocument $forgedCommandPath $forgedCommand
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $forgedCommandPath `
            @scalingReaderArguments | Out-Null
    } 'not an exact installed per-process timing receipt|command|relocat|closure' `
        'raw scaling samples reject a forged unsupported executable command'

    $logicalOnlyScalingPath = Join-Path $attachmentRoot 'scaling-logical-only.json'
    $logicalOnlyScaling = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $logicalOnlyScaling.selectedLanes[1].selectedDistinctPhysicalCores = 4
    Write-JsonDocument $logicalOnlyScalingPath $logicalOnlyScaling
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $logicalOnlyScalingPath `
            @scalingReaderArguments | Out-Null
    } 'exact selected logical and distinct physical-core count' `
        'scaling evidence rejects eight logical workers backed by only four physical cores'

    $tamperedAmdahlPath = Join-Path $attachmentRoot 'scaling-tampered-amdahl.json'
    $tamperedAmdahl = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $tamperedAmdahl.amdahl.serialFraction = 0.1
    Write-JsonDocument $tamperedAmdahlPath $tamperedAmdahl
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $tamperedAmdahlPath `
            @scalingReaderArguments | Out-Null
    } 'Amdahl evidence does not prove' `
        'scaling evidence rejects a self-asserted Amdahl fraction that differs from phase timing'

    $wrongBaselineArguments = $scalingReaderArguments.Clone()
    $wrongBaselineArguments.ExpectedStage3BaselineSha256 = 'F' * 64
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $scalingManifest `
            @wrongBaselineArguments | Out-Null
    } 'provenance is invalid|Stage 3' `
        'scaling evidence rejects a Stage 3 regression baseline with a different independent hash'

    $staleCohortPath = Join-Path $attachmentRoot 'scaling-stale-cohort.json'
    $staleCohort = Get-Content -LiteralPath $scalingManifest -Raw | ConvertFrom-Json
    $staleCohort.cohortNonce = '87654321-4321-4abc-8def-123456789abc'
    Write-JsonDocument $staleCohortPath $staleCohort
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $staleCohortPath `
            @scalingReaderArguments | Out-Null
    } 'provenance is invalid|cohort' `
        'scaling evidence rejects a prior execution cohort under a fresh acceptance envelope'

    $wrongRuntimeArguments = $scalingReaderArguments.Clone()
    $wrongRuntimeArguments.ExpectedRuntimeClosure = [ordered]@{
        dependencyManifestSha256 = $script:TestRuntimeClosure.dependencyManifestSha256
        closureSha256 = 'F' * 64
    }
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $scalingManifest `
            @wrongRuntimeArguments | Out-Null
    } 'runtime closure|substituted' `
        'scaling evidence rejects an independently expected runtime closure from another candidate'

    $wrongProfileArguments = $scalingReaderArguments.Clone()
    $wrongProfileArguments.ExpectedPhaseBaselineProfileSha256 = 'F' * 64
    Assert-Throws {
        Read-Stage5PerformanceScalingEvidence -Path $scalingManifest `
            @wrongProfileArguments | Out-Null
    } 'phase-baseline|profile|reviewed hash' `
        'scaling evidence rejects a phase baseline profile detached from independent review'

    $evidenceModule = Get-Module -Name DeterministicSimulationEvidence
    Assert-True ($null -ne $evidenceModule) `
        'the final-acceptance snapshot tests can access the evidence module boundary'
    $snapshotPath = Join-Path $attachmentRoot 'snapshot-mutation.json'
    Write-JsonDocument $snapshotPath ([ordered]@{ marker = 'original'; value = 17 })
    $snapshot = & $evidenceModule {
        param($path)
        Get-Stage5FinalAcceptanceFileSnapshot $path 'snapshot mutation test'
    } $snapshotPath
    [IO.File]::WriteAllText($snapshotPath, '{"marker":"mutated","value":99}')
    $snapshotDocument = & $evidenceModule {
        param($fileSnapshot)
        ConvertFrom-Stage5FinalAcceptanceJsonSnapshot $fileSnapshot 'snapshot mutation test'
    } $snapshot
    Assert-True ((Get-Stage5JsonValue $snapshotDocument 'marker' 'snapshot mutation test') -ceq 'original' -and
        (Get-Stage5JsonValue $snapshotDocument 'value' 'snapshot mutation test') -eq 17) `
        'final-acceptance JSON parsing remains bound to the bytes captured before a later file mutation'

    $identityStream = [IO.File]::Open($snapshotPath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $resolvedIdentity = Assert-Stage5FinalAcceptanceFileHandlePath `
            $identityStream $snapshotPath 'snapshot handle identity positive'
        Assert-True ([String]::Equals($resolvedIdentity,
                [IO.Path]::GetFullPath($snapshotPath),
                [StringComparison]::OrdinalIgnoreCase)) `
            'final-acceptance handle identity resolves the exact opened local file'
        Assert-Throws {
            Assert-Stage5FinalAcceptanceFileHandlePath $identityStream `
                (Join-Path $attachmentRoot 'different.json') `
                'snapshot handle identity negative' | Out-Null
        } 'different path|opened handle|identity' `
            'final-acceptance handle identity rejects a lexical path detached from the opened file'
    }
    finally { $identityStream.Dispose() }

    $reparseRoot = Join-Path $root 'reparse-negative'
    $reparseBase = Join-Path $reparseRoot 'manifest'
    $reparseTarget = Join-Path $reparseRoot 'outside'
    New-Item -ItemType Directory -Path $reparseBase, $reparseTarget -Force | Out-Null
    $reparseTargetFile = Join-Path $reparseTarget 'outside.json'
    Write-JsonDocument $reparseTargetFile ([ordered]@{ marker = 'outside' })
    $reparseLink = Join-Path $reparseBase 'linked'
    $reparseRelative = 'linked\outside.json'
    $reparseCreated = $false
    try {
        New-Item -ItemType Junction -Path $reparseLink -Target $reparseTarget `
            -ErrorAction Stop | Out-Null
        $reparseCreated = $true
    }
    catch {
        $reparseLink = Join-Path $reparseBase 'linked.json'
        $reparseRelative = 'linked.json'
        try {
            New-Item -ItemType SymbolicLink -Path $reparseLink -Target $reparseTargetFile `
                -ErrorAction Stop | Out-Null
            $reparseCreated = $true
        }
        catch {
            Write-Warning 'Skipping reparse-path negative: this host does not permit junction or symbolic-link creation.'
        }
    }
    if ($reparseCreated) {
        $reparseError = $null
        try {
            & $evidenceModule {
                param($baseDirectory, $relativePath)
                Resolve-Stage5FinalAcceptanceFile $baseDirectory $relativePath `
                    'reparse path negative'
            } $reparseBase $reparseRelative | Out-Null
        }
        catch { $reparseError = $_.Exception.Message }
        Assert-True ($reparseError -match 'reparse point') `
            'final-acceptance evidence rejects a junction or symbolic-link path escape'
        try {
            $reparseLinkItem = Get-Item -LiteralPath $reparseLink -Force `
                -ErrorAction SilentlyContinue
            if ($null -ne $reparseLinkItem) {
                if (($reparseLinkItem.Attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                    [IO.Directory]::Delete($reparseLink)
                }
                else {
                    [IO.File]::Delete($reparseLink)
                }
            }
        }
        catch {
            # Windows PowerShell 5.1 can throw while Remove-Item tears down a
            # junction; the outer fixture cleanup remains the final fallback.
        }
    }

    $hostAttachment = Join-Path $attachmentRoot 'deterministic-runtime-validation-plan.json'
    [IO.File]::AppendAllText($hostAttachment, 'tampered')
    Write-AcceptanceRequest $acceptanceRequest $acceptanceKinds
    Assert-Throws {
        Invoke-Stage5FinalAcceptanceAggregation $acceptanceRequest | Out-Null
    } 'attachment.*SHA-256 mismatch' `
        'final acceptance independently rehashes and rejects a tampered attachment'
    }
}
finally {
    $rootFull = [IO.Path]::GetFullPath($root)
    if ($rootFull.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($rootFull) -like 'GGC-Stage5Validation-Test-*') {
        Remove-Item -LiteralPath $rootFull -Recurse -Force
    }
}

if ($script:Failures -ne 0) {
    throw "$script:Failures deterministic simulation validation test(s) failed."
}
Write-Output 'Deterministic simulation validation script tests passed.'
