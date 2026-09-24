[CmdletBinding()]
param(
    [string]$ScratchRoot = '',
    [string]$ModulePath = '',
    [switch]$LegacyLookup
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-RawIndexTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-RawIndexThrows {
    param(
        [scriptblock]$Action,
        [string]$Pattern,
        [string]$Message
    )
    $caught = $null
    try {
        & $Action
    }
    catch {
        $caught = $_.Exception
    }
    $errorText = if ($null -ne $caught) { $caught.Message } else { 'no exception' }
    Assert-RawIndexTest ($null -ne $caught -and $errorText -match $Pattern) "$Message (got '$errorText')"
}

function Write-RawIndexText {
    param([string]$Path, [string]$Text)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Get-RawIndexSha256 {
    param([string]$Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash([IO.File]::ReadAllBytes($Path)) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Write-RawIndexJson {
    param([string]$Path, [object]$Value)
    Write-RawIndexText $Path ($Value | ConvertTo-Json -Depth 20)
}

function New-RawIndexInput {
    $hash = ('A' * 64)
    $bytes = [Text.Encoding]::UTF8.GetBytes('raw-index-snapshot')
    $snapshot = [pscustomobject]@{ bytes = $bytes; sha256 = $hash }
    $records = New-Object 'Collections.Generic.List[object]'
    $lookupNames = New-Object 'Collections.Generic.List[string]'
    for ($sequence = 1; $sequence -le 253; ++$sequence) {
        foreach ($stream in @('stdout', 'stderr')) {
            $name = 'child-{0:D3}.{1}.log' -f $sequence, $stream
            $records.Add([pscustomobject]@{
                name = $name
                sha256 = $hash
                snapshot = $snapshot
                lookupOrigin = 'raw'
            }) | Out-Null
            $lookupNames.Add($name) | Out-Null
        }
    }
    $records.Add([pscustomobject]@{
        name = 'validation-plan.json'
        sha256 = $hash
        snapshot = $snapshot
        lookupOrigin = 'raw'
    }) | Out-Null
    return [pscustomobject]@{
        hash = $hash
        records = @($records.ToArray())
        lookupNames = @($lookupNames.ToArray())
    }
}

function Invoke-RawIndexLookup {
    param(
        [object]$EvidenceModule,
        [object[]]$Records,
        [Collections.IDictionary]$Index,
        [string]$Name,
        [bool]$UseIndex
    )
    if ($UseIndex) {
        return [object](Get-Stage5DevelopmentReadinessRawLog -RawLogs $Records -LeafName $Name -Context 'raw-index lookup' -RawLogIndex $Index)
    }
    return [object](Get-Stage5DevelopmentReadinessRawLog -RawLogs $Records -LeafName $Name -Context 'raw-index lookup')
}

function Invoke-RawIndexStream {
    param(
        [object]$EvidenceModule,
        [object]$InputData,
        [Collections.IDictionary]$Index,
        [bool]$UseIndex
    )
    $watch = [Diagnostics.Stopwatch]::StartNew()
    for ($sequence = 1; $sequence -le 253; ++$sequence) {
        $stdout = 'child-{0:D3}.stdout.log' -f $sequence
        $stderr = 'child-{0:D3}.stderr.log' -f $sequence
        $entry = [ordered]@{ stdout = $stdout; stderr = $stderr }
        $result = [ordered]@{
            stdoutSha256 = $InputData.hash
            stderrSha256 = $InputData.hash
        }
        if ($UseIndex) {
            & $EvidenceModule { param($entryValue, $resultValue, $records, $indexValue) Assert-Stage5DevelopmentReadinessExecutionStreamEvidence -Entry $entryValue -Result $resultValue -ValidatedRawLogs $records -Context 'raw-index stream' -RawLogIndex $indexValue | Out-Null } $entry $result $InputData.records $Index
        }
        else {
            & $EvidenceModule { param($entryValue, $resultValue, $records) Assert-Stage5DevelopmentReadinessExecutionStreamEvidence -Entry $entryValue -Result $resultValue -ValidatedRawLogs $records -Context 'raw-index stream' | Out-Null } $entry $result $InputData.records
        }
    }
    $watch.Stop()
    return $watch.Elapsed.TotalMilliseconds
}

function New-ReaderMutationFixture {
    param([string]$Root)
    $sourceCommit = ('a' * 40)
    $artifactHash = ('B' * 64)
    $dependencyHash = ('C' * 64)
    $closureHash = ('D' * 64)
    $cohortNonce = '11111111-1111-4111-8111-111111111111'
    $runNonce = '22222222-2222-4222-8222-222222222222'
    $sortedQualFiles = @(
        [ordered]@{ path = 'Data/Scripts/MultiplayerScripts.scb'; sha256 = ('5' * 64) },
        [ordered]@{ path = 'Data/Scripts/SkirmishScripts.scb'; sha256 = ('6' * 64) },
        [ordered]@{ path = 'English.big'; sha256 = ('1' * 64) },
        [ordered]@{ path = 'INI.big'; sha256 = ('2' * 64) },
        [ordered]@{ path = 'Maps.big'; sha256 = ('3' * 64) },
        [ordered]@{ path = 'W3D.big'; sha256 = ('4' * 64) }
    )
    $qualLines = @($sortedQualFiles | ForEach-Object { '{0}|{1}' -f $_.path, $_.sha256 })
    $qualText = ($qualLines -join [char]10) + [char]10
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $qualClosure = (($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($qualText)) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally {
        $sha.Dispose()
    }
    $qualification = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-simulation-qualification-data'
        producer = 'genci-r2-trimmed-data'
        sourceCommit = $sourceCommit
        title = 'Generals'
        archiveSource = [ordered]@{
            object = 's3://github-ci/generals108_gamedata_trimmed.7z'
            sha256 = '37A351AA430199D1F05DEB9E404857DCE7B461A6AC272C5D4A0B5652CDB06372'
        }
        files = $sortedQualFiles
        closureSha256 = $qualClosure
    }
    $qualificationPath = Join-Path $Root 'QualificationData.json'
    Write-RawIndexJson $qualificationPath $qualification
    $qualificationHash = Get-RawIndexSha256 $qualificationPath
    $rawPath = Join-Path $Root 'raw\validation-plan.raw.log'
    Write-RawIndexText $rawPath 'raw-index-reader-mutation'
    $rawHash = Get-RawIndexSha256 $rawPath
    $receipt = [ordered]@{
        schemaVersion = 1
        evidenceKind = 'stage5-host-runner-receipt'
        status = 'passed'
        role = 'validation-plan'
        trustDomain = 'host-runner'
        producer = 'installed-runtime-validation-plan-v2'
        producerVersion = '2'
        sourceCommit = $sourceCommit
        title = 'Generals'
        architecture = 'x64'
        artifactSetSha256 = $artifactHash
        recordedUtc = '2026-09-12T00:00:00.0000000Z'
        cohortNonce = $cohortNonce
        runtimeClosure = [ordered]@{
            dependencyManifestSha256 = $dependencyHash
            closureSha256 = $closureHash
        }
        details = [ordered]@{
            gateName = 'deterministic-runtime'
            validationSet = 'All'
            entryCount = 253
            qualificationData = [ordered]@{
                path = 'QualificationData.json'
                title = 'Generals'
                manifestSha256 = $qualificationHash
                closureSha256 = $qualClosure
                fileCount = 6
            }
        }
        runNonce = $runNonce
        executableSha256 = $artifactHash
        rawLogs = @([ordered]@{
            name = 'validation-plan.json'
            path = 'raw\validation-plan.raw.log'
            sha256 = $rawHash
        })
        provenance = [ordered]@{
            kind = 'host-runner-observation'
            runner = 'Run-DeterministicSimulationValidation.ps1'
            runnerVersion = '1'
            childProvenance = 'not-applicable'
            children = @()
        }
    }
    $receiptPath = Join-Path $Root 'validation-plan-receipt.json'
    Write-RawIndexJson $receiptPath $receipt
    return [pscustomobject]@{
        root = $Root
        receiptPath = $receiptPath
        sourceCommit = $sourceCommit
        artifactHash = $artifactHash
        cohortNonce = $cohortNonce
        runtimeClosure = $receipt.runtimeClosure
        expectedReceiptHash = Get-RawIndexSha256 $receiptPath
    }
}

function Invoke-ReaderMutationCheck {
    param([object]$Fixture)
    $artifactHashes = @{
        'generals-executable' = $Fixture.artifactHash
        'zerohour-executable' = $Fixture.artifactHash
    }
    $readArguments = @{
        Path = $Fixture.receiptPath
        Kind = 'deterministic-runtime'
        Role = 'validation-plan'
        EvidenceTitle = 'Generals'
        ExpectedSourceCommit = $Fixture.sourceCommit
        ExpectedArtifactSetSha256 = $Fixture.artifactHash
        ArtifactHashes = $artifactHashes
        ExpectedEvidenceSha256 = $Fixture.expectedReceiptHash
        ExpectedCohortNonce = $Fixture.cohortNonce
        ExpectedCohortCreatedUtc = '2026-09-12T00:00:00.0000000Z'
        ExpectedRuntimeClosure = $Fixture.runtimeClosure
    }
    $first = Read-Stage5FinalAcceptanceImmutableReceipt @readArguments
    Assert-RawIndexTest ($first.trustDomain -ceq 'host-runner') 'existing reader did not accept the unchanged receipt'
    $rawPath = Join-Path $Fixture.root 'raw\validation-plan.raw.log'
    Write-RawIndexText $rawPath 'mutated-after-reader'
    Assert-RawIndexThrows { Read-Stage5FinalAcceptanceImmutableReceipt @readArguments | Out-Null } 'SHA-256|sha256' 'existing reader accepted mutated raw bytes'
    return $true
}

$configuredScratchRoot = if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $env:RTS_STAGE5_VALIDATION_SCRATCH_ROOT
}
else {
    $ScratchRoot
}
Assert-RawIndexTest (-not [string]::IsNullOrWhiteSpace($configuredScratchRoot)) 'raw-index tests require -ScratchRoot or RTS_STAGE5_VALIDATION_SCRATCH_ROOT'
$resolvedScratchRoot = [IO.Path]::GetFullPath($configuredScratchRoot).TrimEnd('\')
Assert-RawIndexTest $resolvedScratchRoot.StartsWith('H:\', [StringComparison]::OrdinalIgnoreCase) 'raw-index tests require an explicit H: scratch root'
[IO.Directory]::CreateDirectory($resolvedScratchRoot) | Out-Null
$runRoot = Join-Path $resolvedScratchRoot ('raw-index-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$logPath = Join-Path $runRoot 'raw-index.log'
$resultPath = Join-Path $runRoot 'raw-index.json'
$modulePath = if ([string]::IsNullOrWhiteSpace($ModulePath)) {
    Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1'
}
else {
    [IO.Path]::GetFullPath($ModulePath)
}
Assert-RawIndexTest (Test-Path -LiteralPath $modulePath -PathType Leaf) "evidence module is missing: $modulePath"
Import-Module $modulePath -Force
$evidenceModule = Get-Module -Name 'DeterministicSimulationEvidence' |
    Where-Object { $_.Path -ceq ([IO.Path]::GetFullPath($modulePath)) } |
    Select-Object -First 1
Assert-RawIndexTest ($null -ne $evidenceModule) 'evidence module did not import'

try {
    $inputData = New-RawIndexInput
    $useIndex = -not $LegacyLookup
    $index = if ($useIndex) {
        & $evidenceModule {
            param($records)
            New-Stage5DevelopmentReadinessRawLogIndex $records
        } $inputData.records
    }
    else {
        $null
    }
    if ($useIndex) {
        $indexedMarker = [pscustomobject]@{
            name = 'child-001.stdout.log'
            sha256 = $inputData.hash
            snapshot = $inputData.records[0].snapshot
            lookupOrigin = 'indexed'
        }
        $index['child-001.stdout.log'] = @($indexedMarker)
    }
    foreach ($name in @('child-001.stdout.log', 'child-253.stderr.log', 'validation-plan.json')) {
        $record = Invoke-RawIndexLookup $evidenceModule $inputData.records $index $name $useIndex
        Assert-RawIndexTest ([string]$record.name -ceq $name) "lookup returned the wrong record for '$name'"
        $expectedOrigin = if ($useIndex -and $name -ceq 'child-001.stdout.log') { 'indexed' } else { 'raw' }
        Assert-RawIndexTest ([string]$record.lookupOrigin -ceq $expectedOrigin) "lookup origin changed for '$name'"
    }

    $duplicateRecord = [pscustomobject]@{
        name = 'child-001.stdout.log'
        sha256 = $inputData.hash
        snapshot = $inputData.records[0].snapshot
    }
    $duplicateRecords = @($inputData.records + $duplicateRecord)
    $duplicateIndex = if ($useIndex) {
        & $evidenceModule {
            param($records)
            New-Stage5DevelopmentReadinessRawLogIndex $records
        } $duplicateRecords
    }
    else {
        $null
    }
    Assert-RawIndexThrows { Invoke-RawIndexLookup $evidenceModule $duplicateRecords $duplicateIndex 'child-001.stdout.log' $useIndex | Out-Null } 'exactly one retained raw log' 'duplicate raw names must be rejected'
    Assert-RawIndexThrows { Invoke-RawIndexLookup $evidenceModule $inputData.records $index 'missing.raw.log' $useIndex | Out-Null } 'exactly one retained raw log' 'missing raw names must be rejected'
    Assert-RawIndexThrows { Invoke-RawIndexLookup $evidenceModule $inputData.records $index 'CHILD-001.STDOUT.LOG' $useIndex | Out-Null } 'exactly one retained raw log' 'case-folded raw names must not match case-sensitive names'

    $lookupMilliseconds = Invoke-RawIndexStream $evidenceModule $inputData $index $useIndex
    $readerFixture = New-ReaderMutationFixture (Join-Path $runRoot 'reader')
    $mutationRejected = Invoke-ReaderMutationCheck $readerFixture
    $summary = [ordered]@{
        status = 'passed'
        variant = if ($useIndex) { 'indexed' } else { 'legacy' }
        sourceModule = [IO.Path]::GetFullPath($modulePath)
        runRoot = $runRoot
        rawRecordCount = $inputData.records.Count
        rawReferenceCount = $inputData.lookupNames.Count
        streamWorkloadCount = 253
        lookupMilliseconds = $lookupMilliseconds
        duplicateRejected = $true
        missingRejected = $true
        caseSensitiveRejected = $true
        readerMutationRejected = $mutationRejected
        allLookupsReturnedExpectedRecords = $true
    }
    Write-RawIndexJson $resultPath $summary
    Write-RawIndexText $logPath ($summary | ConvertTo-Json -Depth 12)
    Write-Output ($summary | ConvertTo-Json -Depth 12)
}
catch {
    $failure = [ordered]@{
        status = 'failed'
        variant = if ($LegacyLookup) { 'legacy' } else { 'indexed' }
        runRoot = $runRoot
        error = $_.Exception.Message
    }
    Write-RawIndexText $logPath ($failure | ConvertTo-Json -Depth 12)
    throw
}
