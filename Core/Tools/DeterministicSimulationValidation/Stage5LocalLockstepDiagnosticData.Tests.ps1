[CmdletBinding()]
param(
    [string]$ScratchRoot = '',
    [switch]$RequireSchemaValidation
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-LocalTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-LocalThrows {
    param([scriptblock]$Action, [string]$Pattern, [string]$Description)
    $message = $null
    try { & $Action }
    catch { $message = $_.Exception.Message }
    Assert-LocalTest ($null -ne $message) "$Description must fail closed."
    Assert-LocalTest ($message -match $Pattern) `
        "$Description returned the wrong rejection: $message"
}

function ConvertFrom-LocalJson {
    param([Parameter(Mandatory = $true)][string]$Json)
    $converter = Get-Command ConvertFrom-Json -ErrorAction Stop
    if ($converter.Parameters.ContainsKey('DateKind')) {
        return $Json | ConvertFrom-Json -DateKind String
    }
    return $Json | ConvertFrom-Json
}

function Assert-LocalDocumentRejectedByModule {
    param(
        [object]$Module,
        [object]$Document,
        [string]$Pattern,
        [string]$Description
    )
    $expectedSourceCommit = 'a' * 40
    $expectedRuntimeClosure = [ordered]@{
        dependencyManifestSha256 = 'C' * 64
        closureSha256 = 'D' * 64
    }
    $expectedMapName = 'Maps\Twilight Flame\Twilight Flame.map'
    $expectedMapCrcs = [ordered]@{
        Generals = [uint32]739101722
        ZeroHour = [uint32]4042777579
    }
    $expectedArtifactSetSha256 = 'B' * 64
    # Exercise the production host-side validator directly; both exported
    # Write and Read paths rely on this same strict manifest boundary.
    Assert-LocalThrows {
        & $Module {
            param($Value, $SourceCommit, $RuntimeClosure, $MapName, $MapCrcs,
                $Files, $MapEntries, $ArtifactSetSha256)
            Assert-Stage5LocalManifestDocument $Value $SourceCommit $RuntimeClosure `
                $MapName $MapCrcs $Files $MapEntries $ArtifactSetSha256
        } $Document $expectedSourceCommit $expectedRuntimeClosure $expectedMapName `
            $expectedMapCrcs $Document.files $Document.mapEntries $expectedArtifactSetSha256
    } $Pattern $Description
}

function Get-LocalSyntheticDocument {
    $required = @(
        @('Generals', 'GeneralsRuntime/Data/Scripts/MultiplayerScripts.scb'),
        @('Generals', 'GeneralsRuntime/Data/Scripts/SkirmishScripts.scb'),
        @('Generals', 'GeneralsRuntime/English.big'),
        @('Generals', 'GeneralsRuntime/INI.big'),
        @('Generals', 'GeneralsRuntime/W3D.big'),
        @('Generals', 'GeneralsRuntime/maps.big'),
        @('ZeroHour', 'ZeroHourRuntime/Data/Scripts/MultiplayerScripts.scb'),
        @('ZeroHour', 'ZeroHourRuntime/Data/Scripts/Scripts.ini'),
        @('ZeroHour', 'ZeroHourRuntime/Data/Scripts/SkirmishScripts.scb'),
        @('ZeroHour', 'ZeroHourRuntime/INIZH.big'),
        @('ZeroHour', 'ZeroHourRuntime/MapsZH.big'),
        @('ZeroHour', 'ZeroHourRuntime/W3DZH.big')
    )
    $hashLetters = @('A', 'B', 'C', 'D', 'E', 'F', '1', '2', '3', '4', '5', '6')
    $files = New-Object 'Collections.Generic.List[object]'
    for ($index = 0; $index -lt $required.Count; ++$index) {
        $files.Add([ordered]@{
            title = $required[$index][0]
            path = $required[$index][1]
            sha256 = $hashLetters[$index] * 64
            byteLength = [uint64](100 + $index)
        })
    }
    $canonical = (@($files | ForEach-Object {
        '{0}|{1}|{2}|{3}' -f $_.title, $_.path, $_.sha256, $_.byteLength
    }) -join "`n") + "`n"
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $closure = (($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical)) |
            ForEach-Object { $_.ToString('X2') }) -join '')
    }
    finally { $algorithm.Dispose() }
    return [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-local-lockstep-diagnostic-data'
        producer = 'stage5-local-lockstep-diagnostic-data-v1'
        status = 'ready'
        finalAcceptanceClaim = $false
        canonicalQualification = $false
        promotionGrant = $false
        externalQualificationSkipped = $true
        manualTestingDeferred = $true
        sourceCommit = 'a' * 40
        artifactSetSha256 = 'B' * 64
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = 'C' * 64
            closureSha256 = 'D' * 64
        }
        cohortNonce = '11111111-1111-4111-8111-111111111111'
        cohortCreatedUtc = '2026-09-05T12:00:00.0000000Z'
        mapName = 'Maps\Twilight Flame\Twilight Flame.map'
        mapCrcs = [ordered]@{
            Generals = [uint32]739101722
            ZeroHour = [uint32]4042777579
        }
        mapEntries = @(
            [ordered]@{
                title = 'Generals'
                archivePath = 'GeneralsRuntime/maps.big'
                entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
                sha256 = 'A6811E2F16BAA0ED1E47839C71347CCB51F1F088BDE8EA696E1750687F65E115'
                byteLength = [uint64]402471
                mapCrc = [uint32]739101722
                crcAlgorithm = 'Common/crc.h rotate-left-one then byte-add modulo 2^32'
                encoding = 'EAR-refpack-raw'
            },
            [ordered]@{
                title = 'ZeroHour'
                archivePath = 'ZeroHourRuntime/MapsZH.big'
                entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
                sha256 = '3B8B47E1AF6E5D8D479F282EF6B6E542B5C2DBBD700A91FCF55AA7DA209B1100'
                byteLength = [uint64]400412
                mapCrc = [uint32]4042777579
                crcAlgorithm = 'Common/crc.h rotate-left-one then byte-add modulo 2^32'
                encoding = 'EAR-refpack-raw'
            }
        )
        files = @($files.ToArray())
        closureSha256 = $closure
    }
}

function Get-IndependentSha256Bytes {
    param([byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('X2')
        }) -join '')
    }
    finally { $algorithm.Dispose() }
}

function Get-IndependentRotateAddChecksum {
    param([byte[]]$Bytes, [uint64]$InitialValue = 0)
    Assert-LocalTest ($null -ne $Bytes -and $Bytes.Length -gt 0) `
        'Independent BIG map checksum requires nonempty bytes.'
    [uint64]$value = $InitialValue
    foreach ($byte in $Bytes) {
        $value = (($value -shl 1) + [uint64]$byte + ($value -shr 31)) -band [uint64]4294967295
    }
    return [uint32]$value
}

function Get-BigEndianUInt32Bytes {
    param([uint32]$Value)
    return [byte[]]@(
        (($Value -shr 24) -band 0xff),
        (($Value -shr 16) -band 0xff),
        (($Value -shr 8) -band 0xff),
        ($Value -band 0xff)
    )
}

function Write-SyntheticBigArchive {
    param([string]$Path, [object[]]$Entries)
    Assert-LocalTest ($Entries.Count -gt 0) 'Synthetic BIG archive needs at least one entry.'
    $directoryBytes = 16
    foreach ($entry in $Entries) {
        $nameBytes = [Text.Encoding]::ASCII.GetBytes([string]$entry.name)
        $directoryBytes += 8 + $nameBytes.Length + 1
    }
    $bytes = New-Object 'Collections.Generic.List[byte]'
    foreach ($byte in [Text.Encoding]::ASCII.GetBytes('BIGF')) { [void]$bytes.Add($byte) }
    foreach ($byte in [byte[]]@(0, 0, 0, 0)) { [void]$bytes.Add($byte) }
    foreach ($byte in (Get-BigEndianUInt32Bytes ([uint32]$Entries.Count))) { [void]$bytes.Add($byte) }
    foreach ($byte in [byte[]]@(0, 0, 0, 0)) { [void]$bytes.Add($byte) }
    [uint32]$offset = $directoryBytes
    foreach ($entry in $Entries) {
        [byte[]]$entryBytes = $entry.bytes
        foreach ($byte in (Get-BigEndianUInt32Bytes $offset)) { [void]$bytes.Add($byte) }
        foreach ($byte in (Get-BigEndianUInt32Bytes ([uint32]$entryBytes.Length))) { [void]$bytes.Add($byte) }
        foreach ($byte in [Text.Encoding]::ASCII.GetBytes([string]$entry.name)) { [void]$bytes.Add($byte) }
        [void]$bytes.Add([byte]0)
        $offset += [uint32]$entryBytes.Length
    }
    foreach ($entry in $Entries) {
        foreach ($byte in [byte[]]$entry.bytes) { [void]$bytes.Add($byte) }
    }
    [IO.File]::WriteAllBytes($Path, $bytes.ToArray())
}

function Write-SyntheticBigInvalidSpan {
    param([string]$Path, [string]$Name)
    $bytes = New-Object 'Collections.Generic.List[byte]'
    foreach ($byte in [Text.Encoding]::ASCII.GetBytes('BIGF')) { [void]$bytes.Add($byte) }
    foreach ($byte in [byte[]]@(0, 0, 0, 0)) { [void]$bytes.Add($byte) }
    foreach ($byte in (Get-BigEndianUInt32Bytes ([uint32]1))) { [void]$bytes.Add($byte) }
    foreach ($byte in [byte[]]@(0, 0, 0, 0)) { [void]$bytes.Add($byte) }
    foreach ($byte in (Get-BigEndianUInt32Bytes ([uint32]16))) { [void]$bytes.Add($byte) }
    foreach ($byte in (Get-BigEndianUInt32Bytes ([uint32]7))) { [void]$bytes.Add($byte) }
    foreach ($byte in [Text.Encoding]::ASCII.GetBytes($Name)) { [void]$bytes.Add($byte) }
    [void]$bytes.Add([byte]0)
    [IO.File]::WriteAllBytes($Path, $bytes.ToArray())
}

function Write-LocalArtifactFixture {
    param(
        [string]$Root,
        [string]$SourceCommit,
        [Collections.IDictionary]$RuntimeClosure,
        [byte[]]$DependencyBytes
    )
    $artifactRoot = Join-Path $Root 'artifacts'
    foreach ($directory in @('Generals', 'ZeroHour')) {
        [IO.Directory]::CreateDirectory((Join-Path $artifactRoot $directory)) | Out-Null
    }
    $dependencyPath = Join-Path $artifactRoot 'RuntimeClosure.json'
    Assert-LocalTest ($null -ne $DependencyBytes -and $DependencyBytes.Length -gt 0) `
        'Synthetic runtime dependency fixture must contain bytes.'
    [IO.File]::WriteAllBytes($dependencyPath, $DependencyBytes)
    $artifactSpecs = @(
        @('generals-executable', 'Generals/generalsv.exe'),
        @('generals-launcher', 'Generals/launcher.exe'),
        @('generals-launcher-config', 'Generals/launcher.lcf'),
        @('zerohour-executable', 'ZeroHour/generalszh.exe'),
        @('zerohour-launcher', 'ZeroHour/launcher.exe'),
        @('zerohour-launcher-config', 'ZeroHour/launcher.lcf')
    )
    $artifacts = New-Object 'Collections.Generic.List[object]'
    foreach ($spec in $artifactSpecs) {
        $path = Join-Path $artifactRoot ($spec[1] -replace '/', '\')
        $bytes = [byte[]]@([byte]($artifacts.Count + 10), 2, 3)
        [IO.File]::WriteAllBytes($path, $bytes)
        $artifacts.Add([ordered]@{
            role = $spec[0]
            path = $spec[1]
            sha256 = Get-IndependentSha256Bytes $bytes
        })
    }
    $document = [ordered]@{
        schemaVersion = 1
        sourceCommit = $SourceCommit
        productSet = @('Generals', 'ZeroHour')
        architecture = 'x64'
        artifacts = @($artifacts.ToArray())
        runtimeClosure = [ordered]@{
            dependencyManifest = [ordered]@{
                path = 'RuntimeClosure.json'
                sha256 = Get-IndependentSha256Bytes $DependencyBytes
            }
            closureSha256 = [string]$RuntimeClosure.closureSha256
        }
    }
    $manifestPath = Join-Path $artifactRoot 'Stage5ArtifactSet.json'
    [IO.File]::WriteAllText($manifestPath,
        ($document | ConvertTo-Json -Depth 10),
        (New-Object Text.UTF8Encoding($false)))
    return [pscustomobject]@{
        root = $artifactRoot
        path = $manifestPath
        generalsExecutable = Join-Path $artifactRoot 'Generals/generalsv.exe'
        zeroHourExecutable = Join-Path $artifactRoot 'ZeroHour/generalszh.exe'
        document = $document
    }
}

function Write-LocalRuntimeFixture {
    param(
        [string]$Root,
        [string]$Title,
        [string]$MapArchiveName,
        [string]$MapEntryName,
        [byte[]]$MapBytes
    )
    $runtimeRoot = Join-Path $Root ($Title + 'Runtime')
    [IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
    $required = if ($Title -ceq 'Generals') {
        @(
            'English.big',
            'INI.big',
            'maps.big',
            'W3D.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/SkirmishScripts.scb'
        )
    }
    else {
        @(
            'INIZH.big',
            'MapsZH.big',
            'W3DZH.big',
            'Data/Scripts/MultiplayerScripts.scb',
            'Data/Scripts/Scripts.ini',
            'Data/Scripts/SkirmishScripts.scb'
        )
    }
    for ($index = 0; $index -lt $required.Count; ++$index) {
        $relative = $required[$index]
        $path = Join-Path $runtimeRoot ($relative -replace '/', '\')
        [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
        if ($relative -ceq $MapArchiveName) {
            Write-SyntheticBigArchive -Path $path -Entries @(
                [pscustomobject]@{ name = $MapEntryName; bytes = $MapBytes },
                [pscustomobject]@{ name = 'Data/fixture.dat'; bytes = [byte[]]@(9, 8, 7) }
            )
        }
        else {
            [IO.File]::WriteAllBytes($path, [byte[]]@([byte](32 + $index),
                [byte]$Title.Length, [byte]127))
        }
    }
    $executableName = if ($Title -ceq 'Generals') { 'generalsv.exe' } else { 'generalszh.exe' }
    $executable = Join-Path $runtimeRoot $executableName
    $executableBytes = if ($Title -ceq 'Generals') {
        [byte[]]@(10, 2, 3)
    }
    else { [byte[]]@(13, 2, 3) }
    [IO.File]::WriteAllBytes($executable, $executableBytes)
    return [pscustomobject]@{
        root = $runtimeRoot
        executable = $executable
        mapArchive = Join-Path $runtimeRoot $MapArchiveName
        mapBytes = $MapBytes
    }
}

function Get-LocalExpectedMapEntriesSignature {
    param([object]$Entries)
    return [string](ConvertTo-Json -InputObject $Entries -Depth 10 -Compress)
}

$modulePath = Join-Path $PSScriptRoot 'Stage5LocalLockstepDiagnosticData.psm1'
$schemaPath = Join-Path $PSScriptRoot 'Stage5LocalLockstepDiagnosticData.schema.json'
Assert-LocalTest (Test-Path -LiteralPath $modulePath -PathType Leaf) 'Local data module is missing.'
Assert-LocalTest (Test-Path -LiteralPath $schemaPath -PathType Leaf) 'Local data schema is missing.'

$tokens = $null
$errors = $null
$moduleAst = [Management.Automation.Language.Parser]::ParseFile(
    $modulePath, [ref]$tokens, [ref]$errors)
Assert-LocalTest ($errors.Count -eq 0) "Local data module does not parse: $errors"
$exports = @($moduleAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Export-ModuleMember'
}, $true))
Assert-LocalTest ($exports.Count -eq 1) 'Local data module must have one export declaration.'
$exportText = $exports[0].Extent.Text
foreach ($name in @(
        'Read-Stage5LocalLockstepDiagnosticData',
        'New-Stage5LocalLockstepDiagnosticDataManifest',
        'Write-Stage5LocalLockstepDiagnosticDataManifest')) {
    Assert-LocalTest ($exportText.Contains($name)) "Local data module does not export $name."
}
$schemaText = [IO.File]::ReadAllText($schemaPath)
$schema = ConvertFrom-LocalJson $schemaText
Assert-LocalTest ($schema.additionalProperties -eq $false -and
    [int]$schema.properties.schemaVersion.const -eq 1) `
    'Local data schema must be strict and versioned.'
$dateProbe = ConvertFrom-LocalJson '{"cohortCreatedUtc":"2026-09-05T12:00:00.0000000Z"}'
Assert-LocalTest ($dateProbe.cohortCreatedUtc -is [string]) `
    'Date-preserving JSON reader must retain UTC timestamps as strings.'
Assert-LocalTest (-not $schemaText.Contains('archiveSources') -and
    -not $schemaText.Contains('reviewed-archive')) `
    'Local data schema must not claim reviewed archive provenance.'
if ($RequireSchemaValidation) {
    Assert-LocalTest ($PSVersionTable.PSVersion.Major -ge 7 -and
        $null -ne (Get-Command Test-Json -ErrorAction SilentlyContinue)) `
        'RequireSchemaValidation requires the native PowerShell 7+ Test-Json cmdlet.'
}

$canonicalReaderPath = Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
if (Test-Path -LiteralPath $canonicalReaderPath -PathType Leaf) {
    $canonicalReader = [IO.File]::ReadAllText($canonicalReaderPath)
    Assert-LocalTest ($canonicalReader -notmatch
        'stage5-local-lockstep-diagnostic-data') `
        'Canonical evidence reader must not recognize the local diagnostic-data kind.'
}

$module = $null
$originalExpectedMapEntries = $null
$originalExpectedMapEntriesSignature = $null
$expectedMapEntriesOverridden = $false
$module = Import-Module $modulePath -Force -PassThru
$originalExpectedMapEntries = & $module { return ,$script:ExpectedMapEntries }
$originalExpectedMapEntriesSignature =
    Get-LocalExpectedMapEntriesSignature $originalExpectedMapEntries
$scratchParent = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else { $ScratchRoot }
$scratchParent = [string]$scratchParent
Assert-LocalTest (-not [string]::IsNullOrWhiteSpace($scratchParent)) `
    'Synthetic tests require -ScratchRoot or RTS_STAGE5_VALIDATION_SCRATCH_ROOT on H:.'
$scratchParent = [IO.Path]::GetFullPath($scratchParent)
Assert-LocalTest (Test-Path -LiteralPath $scratchParent -PathType Container) `
    "Synthetic test scratch parent is not an existing directory: $scratchParent"
$scratchParentItem = Get-Item -LiteralPath $scratchParent -Force
Assert-LocalTest (($scratchParentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
    "Synthetic test scratch parent is a reparse point: $scratchParent"
$testRoot = Join-Path $scratchParent ('stage5-local-lockstep-data-' + [Guid]::NewGuid().ToString('N'))
Assert-LocalTest (-not (Test-Path -LiteralPath $testRoot)) 'Synthetic test root already exists.'
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$succeeded = $false
try {
    $mapName = 'Maps\Twilight Flame\Twilight Flame.map'
    $mapBytes = [byte[]]@(0x45, 0x41, 0x52, 0x00, 1, 2, 3)
    Assert-LocalTest ((Get-IndependentRotateAddChecksum ([byte[]]@(1, 2, 3))) -eq 11) `
        'Independent rotate/add checksum known-value check failed.'
    Assert-LocalTest ((Get-IndependentRotateAddChecksum ([byte[]]@(1)) 2147483648) -eq 2) `
        'Independent rotate/add checksum high-bit known-value check failed.'
    $mapExpected = [ordered]@{
        archivePath = 'GeneralsRuntime/maps.big'
        entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
        sha256 = Get-IndependentSha256Bytes $mapBytes
        byteLength = [uint64]$mapBytes.Length
        mapCrc = Get-IndependentRotateAddChecksum $mapBytes
    }
    $archivePath = Join-Path $testRoot 'Maps.big'
    Write-SyntheticBigArchive -Path $archivePath -Entries @(
        [pscustomobject]@{ name = $mapName; bytes = $mapBytes },
        [pscustomobject]@{ name = 'Data/other.dat'; bytes = [byte[]]@(9, 8, 7) }
    )
    $parsedMap = & $module {
        param($Path, $Expected, $Name)
        Get-Stage5LocalBigEntryBinding $Path 'Generals' $Name $Expected
    } $archivePath $mapExpected $mapName
    Assert-LocalTest ([string]$parsedMap.sha256 -ceq $mapExpected.sha256 -and
        [uint64]$parsedMap.byteLength -eq [uint64]$mapBytes.Length -and
        [uint32]$parsedMap.mapCrc -eq [uint32]$mapExpected.mapCrc -and
        [string]$parsedMap.crcAlgorithm -ceq
            'Common/crc.h rotate-left-one then byte-add modulo 2^32') `
        'Synthetic BIGF map entry was not parsed with actual bytes and game checksum semantics.'

    $truncatedPath = Join-Path $testRoot 'truncated.big'
    [IO.File]::WriteAllBytes($truncatedPath, [byte[]]@(66, 73, 71, 70, 0, 0, 0, 0,
        0, 0, 0, 1, 0, 0, 0, 0))
    Assert-LocalThrows {
        & $module {
            param($Path, $Expected, $Name)
            Get-Stage5LocalBigEntryBinding $Path 'Generals' $Name $Expected
        } $truncatedPath $mapExpected $mapName
    } 'truncated|table' 'Truncated BIGF directory'

    $duplicatePath = Join-Path $testRoot 'duplicate.big'
    Write-SyntheticBigArchive -Path $duplicatePath -Entries @(
        [pscustomobject]@{ name = $mapName; bytes = $mapBytes },
        [pscustomobject]@{ name = $mapName; bytes = $mapBytes }
    )
    Assert-LocalThrows {
        & $module {
            param($Path, $Expected, $Name)
            Get-Stage5LocalBigEntryBinding $Path 'Generals' $Name $Expected
        } $duplicatePath $mapExpected $mapName
    } 'duplicate' 'Duplicate BIGF map entry'

    $spanPath = Join-Path $testRoot 'invalid-span.big'
    Write-SyntheticBigInvalidSpan -Path $spanPath -Name $mapName
    Assert-LocalThrows {
        & $module {
            param($Path, $Expected, $Name)
            Get-Stage5LocalBigEntryBinding $Path 'Generals' $Name $Expected
        } $spanPath $mapExpected $mapName
    } 'span|bounded' 'Out-of-table BIGF map span'

    $changedStream = [IO.File]::Open($archivePath, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    try {
        $mapOffset = 16 + (8 + ([Text.Encoding]::ASCII.GetByteCount($mapName)) + 1) +
            (8 + ([Text.Encoding]::ASCII.GetByteCount('Data/other.dat')) + 1)
        # Preserve EAR header validity so the byte-binding rejection is tested.
        $changedStream.Position = $mapOffset + 4
        $changedStream.WriteByte([byte]0xfe)
        $changedStream.Flush()
    }
    finally { $changedStream.Dispose() }
    Assert-LocalThrows {
        & $module {
            param($Path, $Expected, $Name)
            Get-Stage5LocalBigEntryBinding $Path 'Generals' $Name $Expected
        } $archivePath $mapExpected $mapName
    } 'map bytes|binding' 'Changed BIG map entry'

    $dependencyBytes = [byte[]]@(4, 5, 6)
    $runtimeClosure = [ordered]@{
        dependencyManifestSha256 = Get-IndependentSha256Bytes $dependencyBytes
        closureSha256 = 'D' * 64
    }
    $artifact = Write-LocalArtifactFixture -Root $testRoot `
        -SourceCommit ('a' * 40) -RuntimeClosure $runtimeClosure `
        -DependencyBytes $dependencyBytes
    $artifactBinding = & $module {
        param($ManifestPath, $Generals, $ZeroHour, $Commit, $Closure)
        Get-Stage5LocalArtifactBinding $ManifestPath $Generals $ZeroHour $Commit $Closure
    } $artifact.path $artifact.generalsExecutable $artifact.zeroHourExecutable `
        ('a' * 40) $runtimeClosure
    Assert-LocalTest ([string]$artifactBinding.sha256 -cmatch '^[0-9A-F]{64}$') `
        'Synthetic artifact-set binding did not hash the manifest.'
    [IO.File]::WriteAllBytes($artifact.generalsExecutable, [byte[]]@(0xff, 2, 3))
    Assert-LocalThrows {
        & $module {
            param($ManifestPath, $Generals, $ZeroHour, $Commit, $Closure)
            Get-Stage5LocalArtifactBinding $ManifestPath $Generals $ZeroHour $Commit $Closure
        } $artifact.path $artifact.generalsExecutable $artifact.zeroHourExecutable `
            ('a' * 40) $runtimeClosure
    } 'bytes do not match|artifact' 'Changed artifact executable'
    [IO.File]::WriteAllBytes($artifact.generalsExecutable, [byte[]]@(10, 2, 3))
    [IO.File]::WriteAllBytes((Join-Path $artifact.root 'RuntimeClosure.json'),
        [byte[]]@(0xff, 5, 6))
    Assert-LocalThrows {
        & $module {
            param($ManifestPath, $Generals, $ZeroHour, $Commit, $Closure)
            Get-Stage5LocalArtifactBinding $ManifestPath $Generals $ZeroHour $Commit $Closure
        } $artifact.path $artifact.generalsExecutable $artifact.zeroHourExecutable `
            ('a' * 40) $runtimeClosure
    } 'dependency manifest bytes' 'Changed runtime dependency manifest'
    [IO.File]::WriteAllBytes((Join-Path $artifact.root 'RuntimeClosure.json'), $dependencyBytes)
    Assert-LocalThrows {
        & $module {
            param($ManifestPath, $Generals, $ZeroHour, $Commit, $Closure)
            Get-Stage5LocalArtifactBinding $ManifestPath $Generals $ZeroHour $Commit $Closure
        } $artifact.path $artifact.generalsExecutable $artifact.zeroHourExecutable `
            ('b' * 40) $runtimeClosure
    } 'sourceCommit' 'Changed artifact source commit binding'

    $artifactDocument = ConvertFrom-LocalJson ([IO.File]::ReadAllText($artifact.path))
    $artifactSchemaMutations = @(
        [pscustomobject]@{ Name = 'schema-version-string'; Value = '1' },
        [pscustomobject]@{ Name = 'schema-version-bool'; Value = $true },
        [pscustomobject]@{ Name = 'schema-version-fractional'; Value = 1.5 }
    )
    foreach ($artifactSchemaMutation in $artifactSchemaMutations) {
        $artifactMutated = ConvertFrom-LocalJson ($artifactDocument | ConvertTo-Json -Depth 20)
        $artifactMutated.schemaVersion = $artifactSchemaMutation.Value
        $artifactMutationPath = Join-Path $artifact.root `
            ('Stage5ArtifactSet.' + $artifactSchemaMutation.Name + '.json')
        [IO.File]::WriteAllText($artifactMutationPath,
            ($artifactMutated | ConvertTo-Json -Depth 20),
            (New-Object Text.UTF8Encoding($false)))
        Assert-LocalThrows {
            & $module {
                param($ManifestPath, $Generals, $ZeroHour, $Commit, $Closure)
                Get-Stage5LocalArtifactBinding $ManifestPath $Generals $ZeroHour $Commit $Closure
            } $artifactMutationPath $artifact.generalsExecutable $artifact.zeroHourExecutable `
                ('a' * 40) $runtimeClosure
        } 'JSON integer|schemaVersion' `
            "Artifact reader rejects $($artifactSchemaMutation.Name)."
    }

    $roundtripGeneralsMapBytes = [byte[]]@(0x45, 0x41, 0x52, 0x00, 1, 2, 3)
    $roundtripZeroHourMapBytes = [byte[]]@(0x45, 0x41, 0x52, 0x00, 4, 5, 6)
    $roundtripExpectedMapEntries = [ordered]@{
        Generals = [ordered]@{
            archivePath = 'GeneralsRuntime/maps.big'
            entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
            sha256 = Get-IndependentSha256Bytes $roundtripGeneralsMapBytes
            byteLength = [uint64]$roundtripGeneralsMapBytes.Length
            mapCrc = Get-IndependentRotateAddChecksum $roundtripGeneralsMapBytes
        }
        ZeroHour = [ordered]@{
            archivePath = 'ZeroHourRuntime/MapsZH.big'
            entryPath = 'Maps/Twilight Flame/Twilight Flame.map'
            sha256 = Get-IndependentSha256Bytes $roundtripZeroHourMapBytes
            byteLength = [uint64]$roundtripZeroHourMapBytes.Length
            mapCrc = Get-IndependentRotateAddChecksum $roundtripZeroHourMapBytes
        }
    }
    $roundtripMapCrcs = [ordered]@{
        Generals = [uint32]$roundtripExpectedMapEntries.Generals.mapCrc
        ZeroHour = [uint32]$roundtripExpectedMapEntries.ZeroHour.mapCrc
    }
    $documentCohortNonce = '11111111-1111-4111-8111-111111111111'
    $documentCohortCreatedUtc = '2026-09-05T12:00:00.0000000Z'
    & $module {
        param($Entries)
        $script:ExpectedMapEntries = $Entries
    } $roundtripExpectedMapEntries
    $expectedMapEntriesOverridden = $true
    try {
        $roundtripGenerals = Write-LocalRuntimeFixture -Root $testRoot `
            -Title 'Generals' -MapArchiveName 'maps.big' `
            -MapEntryName $mapName -MapBytes $roundtripGeneralsMapBytes
        $roundtripZeroHour = Write-LocalRuntimeFixture -Root $testRoot `
            -Title 'ZeroHour' -MapArchiveName 'MapsZH.big' `
            -MapEntryName $mapName -MapBytes $roundtripZeroHourMapBytes
        $newRoundtrip = New-Stage5LocalLockstepDiagnosticDataManifest `
            -GeneralsExecutable $roundtripGenerals.executable `
            -ZeroHourExecutable $roundtripZeroHour.executable `
            -ArtifactSetManifestPath $artifact.path `
            -SourceCommit ('a' * 40) `
            -RuntimeClosure $runtimeClosure `
            -CohortNonce $documentCohortNonce `
            -CohortCreatedUtc $documentCohortCreatedUtc `
            -MapName $mapName `
            -MapCrcs $roundtripMapCrcs
        Assert-LocalTest ($newRoundtrip.fileCount -eq 12 -and
            $newRoundtrip.document.finalAcceptanceClaim -eq $false -and
            $newRoundtrip.document.canonicalQualification -eq $false -and
            $newRoundtrip.document.promotionGrant -eq $false) `
            'Synthetic New manifest was not explicitly diagnostic-only.'
        $roundtripManifestPath = Join-Path $testRoot 'synthetic-roundtrip.json'
        $writtenRoundtrip = Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path $roundtripManifestPath -Manifest $newRoundtrip
        $readRoundtrip = Read-Stage5LocalLockstepDiagnosticData `
            -ManifestPath $writtenRoundtrip.path `
            -ArtifactSetManifestPath $artifact.path `
            -GeneralsExecutable $roundtripGenerals.executable `
            -ZeroHourExecutable $roundtripZeroHour.executable `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedRuntimeClosure $runtimeClosure `
            -ExpectedMapName $mapName `
            -ExpectedMapCrcs $roundtripMapCrcs `
            -ExpectedCohortNonce $documentCohortNonce `
            -ExpectedCohortCreatedUtc $documentCohortCreatedUtc
        Assert-LocalTest ($readRoundtrip.fileCount -eq 12 -and
            $readRoundtrip.document.cohortNonce -ceq $documentCohortNonce -and
            [uint32]$readRoundtrip.document.mapCrcs.Generals -eq
                [uint32]$roundtripMapCrcs.Generals -and
            [uint32]$readRoundtrip.document.mapCrcs.ZeroHour -eq
                [uint32]$roundtripMapCrcs.ZeroHour) `
            'Synthetic New -> Write -> Read round-trip did not preserve bindings.'

        $roundtripFilePath = Join-Path $roundtripGenerals.root 'English.big'
        $roundtripOriginalFileBytes = [IO.File]::ReadAllBytes($roundtripFilePath)
        try {
            [IO.File]::WriteAllBytes($roundtripFilePath, [byte[]]@(0xee, 0xdd, 0xcc))
            Assert-LocalThrows {
                Read-Stage5LocalLockstepDiagnosticData `
                    -ManifestPath $writtenRoundtrip.path `
                    -ArtifactSetManifestPath $artifact.path `
                    -GeneralsExecutable $roundtripGenerals.executable `
                    -ZeroHourExecutable $roundtripZeroHour.executable `
                    -ExpectedSourceCommit ('a' * 40) `
                    -ExpectedRuntimeClosure $runtimeClosure `
                    -ExpectedMapName $mapName `
                    -ExpectedMapCrcs $roundtripMapCrcs `
                    -ExpectedCohortNonce $documentCohortNonce `
                    -ExpectedCohortCreatedUtc $documentCohortCreatedUtc
            } 'runtime file|retained binding|differs' `
                'Reader rejects changed synthetic runtime file.'
        }
        finally {
            [IO.File]::WriteAllBytes($roundtripFilePath, $roundtripOriginalFileBytes)
        }

        $tamperedRoundtrip = ConvertFrom-LocalJson `
            ([IO.File]::ReadAllText($writtenRoundtrip.path))
        $tamperedRoundtrip.cohortNonce = '22222222-2222-4222-8222-222222222222'
        $tamperedRoundtripPath = Join-Path $testRoot 'synthetic-roundtrip-tampered-cohort.json'
        [IO.File]::WriteAllText($tamperedRoundtripPath,
            ($tamperedRoundtrip | ConvertTo-Json -Depth 20),
            (New-Object Text.UTF8Encoding($false)))
        Assert-LocalThrows {
            Read-Stage5LocalLockstepDiagnosticData `
                -ManifestPath $tamperedRoundtripPath `
                -ArtifactSetManifestPath $artifact.path `
                -GeneralsExecutable $roundtripGenerals.executable `
                -ZeroHourExecutable $roundtripZeroHour.executable `
                -ExpectedSourceCommit ('a' * 40) `
                -ExpectedRuntimeClosure $runtimeClosure `
                -ExpectedMapName $mapName `
                -ExpectedMapCrcs $roundtripMapCrcs `
                -ExpectedCohortNonce $documentCohortNonce `
                -ExpectedCohortCreatedUtc $documentCohortCreatedUtc
        } 'cohort binding|stale|substituted' `
            'Reader rejects changed synthetic cohort nonce.'
    }
    finally {
        & $module {
            param($Entries)
            $script:ExpectedMapEntries = $Entries
        } $originalExpectedMapEntries
        $restoredExpectedMapEntries = & $module { return ,$script:ExpectedMapEntries }
        Assert-LocalTest (
            (Get-LocalExpectedMapEntriesSignature $restoredExpectedMapEntries) -ceq
                $originalExpectedMapEntriesSignature) `
            'Production expected map-entry table was not restored after synthetic round-trip.'
        $expectedMapEntriesOverridden = $false
    }

    $document = Get-LocalSyntheticDocument
    $manifest = Write-Stage5LocalLockstepDiagnosticDataManifest `
        -Path (Join-Path $testRoot 'Stage5LocalLockstepDiagnosticData.json') `
        -Manifest ([pscustomobject]@{ document = $document })
    Assert-LocalTest ($manifest.fileCount -eq 12 -and
        $manifest.manifestSha256 -cmatch '^[0-9A-F]{64}$' -and
        $manifest.closureSha256 -ceq $document.closureSha256) `
        'Synthetic valid local data manifest did not return its binding.'
    if ($RequireSchemaValidation) {
        $positiveJson = [IO.File]::ReadAllText($manifest.path)
        Assert-LocalTest ([bool]($positiveJson | Test-Json -SchemaFile $schemaPath -ErrorAction Stop)) `
            'Synthetic positive local data JSON did not satisfy its strict schema.'

        $schemaMutations = @(
            [pscustomobject]@{ Name = 'finalAcceptanceClaim'; Mutate = {
                param($Value)
                $Value.finalAcceptanceClaim = $true
            } },
            [pscustomobject]@{ Name = 'promotionGrant'; Mutate = {
                param($Value)
                $Value.promotionGrant = $true
            } },
            [pscustomobject]@{ Name = 'archiveSources'; Mutate = {
                param($Value)
                $Value | Add-Member -NotePropertyName archiveSources -NotePropertyValue @()
            } }
        )
        foreach ($schemaMutation in $schemaMutations) {
            $candidate = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
            $mutator = $schemaMutation.Mutate
            & $mutator $candidate
            $candidateJson = $candidate | ConvertTo-Json -Depth 20
            try {
                $candidateIsValid = [bool]($candidateJson | Test-Json `
                    -SchemaFile $schemaPath -ErrorAction Stop)
            }
            catch { $candidateIsValid = $false }
            Assert-LocalTest (-not $candidateIsValid) `
                "Strict schema accepted mutated $($schemaMutation.Name) local data JSON."
        }
    }

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.schemaVersion = '1'
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'JSON integer|schemaVersion' 'Reader rejects string schemaVersion.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'schema-version-string.json') -Manifest $mutated
    } 'JSON integer|schemaVersion' 'Writer rejects string schemaVersion.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.schemaVersion = $true
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'JSON integer|schemaVersion' 'Reader rejects boolean schemaVersion.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'schema-version-bool.json') -Manifest $mutated
    } 'JSON integer|schemaVersion' 'Writer rejects boolean schemaVersion.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapCrcs.ZeroHour = 1.5
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'JSON integer|map CRC' 'Reader rejects fractional map CRC.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-crc-fractional.json') -Manifest $mutated
    } 'JSON integer|map CRC' 'Writer rejects fractional map CRC.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.files[0].byteLength = '101'
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'JSON integer|byteLength' 'Reader rejects string runtime byteLength.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'file-byte-length-string.json') -Manifest $mutated
    } 'JSON integer|byteLength' 'Writer rejects string runtime byteLength.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapName = $true
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'JSON string|mapName' 'Reader rejects boolean mapName.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-name-bool.json') -Manifest $mutated
    } 'JSON string|mapName' 'Writer rejects boolean mapName.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.PSObject.Properties.Remove('schemaVersion')
    $mutated | Add-Member -NotePropertyName SchemaVersion -NotePropertyValue 1
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'missing|unexpected|property' 'Reader rejects wrong-case top-level property.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'wrong-case-schema-version.json') -Manifest $mutated
    } 'missing|unexpected|property' 'Writer rejects wrong-case top-level property.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapCrcs.PSObject.Properties.Remove('Generals')
    $mutated.mapCrcs | Add-Member -NotePropertyName generals -NotePropertyValue 739101722
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'Expected map CRCs|missing|Generals' 'Reader rejects wrong-case map CRC key.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'wrong-case-map-crc.json') -Manifest $mutated
    } 'Expected map CRCs|missing|Generals' 'Writer rejects wrong-case map CRC key.'

    $manifestPath = $manifest.path
    Assert-LocalThrows {
        Read-Stage5LocalLockstepDiagnosticData `
            -ManifestPath $manifestPath `
            -ArtifactSetManifestPath (Join-Path $testRoot 'missing-artifact-set.json') `
            -GeneralsExecutable (Join-Path $testRoot 'missing-generals.exe') `
            -ZeroHourExecutable (Join-Path $testRoot 'missing-zerohour.exe') `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedRuntimeClosure $document.runtimeClosure `
            -ExpectedMapName $document.mapName `
            -ExpectedMapCrcs $document.mapCrcs `
            -ExpectedCohortNonce 'not-a-uuid' `
            -ExpectedCohortCreatedUtc $document.cohortCreatedUtc
    } 'Expected cohort nonce' 'Malformed expected cohort nonce'
    Assert-LocalThrows {
        Read-Stage5LocalLockstepDiagnosticData `
            -ManifestPath $manifestPath `
            -ArtifactSetManifestPath (Join-Path $testRoot 'missing-artifact-set.json') `
            -GeneralsExecutable (Join-Path $testRoot 'missing-generals.exe') `
            -ZeroHourExecutable (Join-Path $testRoot 'missing-zerohour.exe') `
            -ExpectedSourceCommit ('a' * 40) `
            -ExpectedRuntimeClosure $document.runtimeClosure `
            -ExpectedMapName $document.mapName `
            -ExpectedMapCrcs $document.mapCrcs `
            -ExpectedCohortNonce $document.cohortNonce `
            -ExpectedCohortCreatedUtc 'not-a-utc'
    } 'Expected cohort created UTC' 'Malformed expected cohort timestamp'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.finalAcceptanceClaim = $true
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'acceptance-claim.json') -Manifest $mutated
    } 'non-final' 'Final-acceptance mutation'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.files[0].path = $mutated.files[1].path
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'duplicate-path.json') -Manifest $mutated
    } 'duplicate|canonical|omits required' 'Duplicate runtime path mutation'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.files[0].path = 'GeneralsRuntime/../forged.big'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'unsafe-path.json') -Manifest $mutated
    } 'unsafe|relative|canonical|omits required' 'Unsafe runtime path mutation'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapCrcs.ZeroHour = 1
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-crc.json') -Manifest $mutated
    } 'map CRC|binding' 'Map CRC mutation'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapEntries = @($mutated.mapEntries[1], $mutated.mapEntries[0])
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'map-entry|binding|title' 'Reader rejects swapped BIG map-entry title order.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-entry-swapped.json') -Manifest $mutated
    } 'map-entry|binding|title' 'Writer rejects swapped BIG map-entry title order.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapEntries[1].title = 'Generals'
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'map-entry|binding|title' 'Reader rejects duplicate BIG map-entry title.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-entry-duplicate-title.json') -Manifest $mutated
    } 'map-entry|binding|title' 'Writer rejects duplicate BIG map-entry title.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated.mapEntries[0].archivePath = 'ZeroHourRuntime/MapsZH.big'
    Assert-LocalDocumentRejectedByModule $module $mutated `
        'map-entry|binding|title' 'Reader rejects wrong BIG map-entry metadata.'
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'map-entry-wrong-metadata.json') -Manifest $mutated
    } 'map-entry|binding|title' 'Writer rejects wrong BIG map-entry metadata.'

    $mutated = ConvertFrom-LocalJson ($document | ConvertTo-Json -Depth 20)
    $mutated | Add-Member -NotePropertyName archiveSources -NotePropertyValue @()
    Assert-LocalThrows {
        Write-Stage5LocalLockstepDiagnosticDataManifest `
            -Path (Join-Path $testRoot 'archive-claim.json') -Manifest $mutated
    } 'unexpected|property' 'Archive provenance mutation'

    $succeeded = $true
}
finally {
    if ($null -ne $module -and $expectedMapEntriesOverridden -and
        $null -ne $originalExpectedMapEntries) {
        & $module {
            param($Entries)
            $script:ExpectedMapEntries = $Entries
        } $originalExpectedMapEntries
        $restoredExpectedMapEntries = & $module { return ,$script:ExpectedMapEntries }
        Assert-LocalTest (
            (Get-LocalExpectedMapEntriesSignature $restoredExpectedMapEntries) -ceq
                $originalExpectedMapEntriesSignature) `
            'Production expected map-entry table was not restored during test cleanup.'
        $expectedMapEntriesOverridden = $false
    }
    if ($null -ne $module) {
        Remove-Module $module -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $testRoot) {
        if (-not $succeeded) {
            Write-Warning "Retaining failed local lockstep data test root: $testRoot"
        }
        else {
            [IO.Directory]::Delete($testRoot, $true)
        }
    }
}

Write-Output 'Stage 5 local lockstep diagnostic-data synthetic tests passed.'
