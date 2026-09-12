[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ScratchRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Stage5ReplayCorpusExporter.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Throws {
    param([scriptblock]$Action, [string]$MessagePattern, [string]$Context)
    $thrown = $false
    $message = ''
    try {
        & $Action | Out-Null
    }
    catch {
        $thrown = $true
        $message = $_.Exception.Message
    }
    Assert-True $thrown "$Context did not reject the input."
    if (-not [string]::IsNullOrWhiteSpace($MessagePattern)) {
        Assert-True ($message -match $MessagePattern) `
            "$Context rejected with an unexpected message: $message"
    }
}

function Set-TestExporterCommitObserver {
    param([scriptblock]$Observer)
    $module = Get-Module -Name Stage5ReplayCorpusExporter |
        Select-Object -First 1
    Assert-True ($null -ne $module) `
        'Stage5ReplayCorpusExporter module is unavailable for commit-race testing.'
    & $module {
        param([scriptblock]$CommitObserver)
        $script:Stage5ExporterCommitObserver = $CommitObserver
    } $Observer
}

function Remove-TestOwnedJunction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedParent
    )
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $parentFull = [IO.Path]::GetFullPath($ExpectedParent).TrimEnd('\')
    Assert-True ([String]::Equals([IO.Path]::GetDirectoryName($full),
        $parentFull, [StringComparison]::OrdinalIgnoreCase)) `
        "Refusing to remove a test junction outside its exact expected parent: $full"
    $item = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    Assert-True ($item.PSIsContainer -and
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) `
        "Refusing to remove a test path that is not a directory reparse point: $full"
    [IO.Directory]::Delete($full, $false)
    Assert-True ($null -eq (Get-Item -LiteralPath $full -Force `
        -ErrorAction SilentlyContinue)) `
        "Test-owned junction still exists after non-recursive removal: $full"
}

function Assert-CurrentGeneralsReplayQualification {
    param([object]$Record, [string]$Context)
    $versionProperty = $Record.PSObject.Properties['replayQualificationVersion']
    $qualificationProperty = $Record.PSObject.Properties['replayQualification']
    $pathEpochProperty = $Record.PSObject.Properties['pathfindingReplayEpoch']
    $valid = $false
    if ($null -ne $versionProperty -and $null -ne $qualificationProperty -and
        $null -ne $pathEpochProperty) {
        $valid = ([int]$versionProperty.Value -eq 2 -and
            [string]$qualificationProperty.Value -ceq 'current-path-qualified' -and
            [int]$pathEpochProperty.Value -eq 1)
    }
    Assert-True $valid `
        "$Context does not carry the versioned current Generals path qualification."
}

function Set-TestRecordProperty {
    param([object]$Record, [string]$Name, [object]$Value)
    $property = $Record.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Record | Add-Member -MemberType NoteProperty -Name $Name -Value $Value |
            Out-Null
    }
    else {
        [void]($property.Value = $Value)
    }
}

function Assert-LocalCapacityRunnerPreservesReplayQualification {
    $runnerPath = Join-Path $PSScriptRoot 'Run-DeterministicSimulationValidation.ps1'
    $tokens = $null
    $parseErrors = $null
    $runnerAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $runnerPath, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) `
        'LocalCapacity runner parses for replay qualification integration testing.'
    $assertConditionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-Condition'
    }, $true)
    $corpusMatrixAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Assert-LocalCapacityCorpusMatrix'
    }, $true)
    Assert-True ($null -ne $assertConditionAst -and $null -ne $corpusMatrixAst) `
        'LocalCapacity runner exposes a testable preflight for the complete corpus AI matrix.'
    if ($null -ne $assertConditionAst -and $null -ne $corpusMatrixAst) {
        Invoke-Expression $assertConditionAst.Extent.Text
        Invoke-Expression $corpusMatrixAst.Extent.Text
        $validCorpusData = [pscustomobject]@{ ai = [pscustomobject]@{
            seeds = @(1729, 1730, 1731)
            scenarios = @('4v3', '4v2', 'hard-ai-2v6')
        } }
        Assert-LocalCapacityCorpusMatrix $validCorpusData
        Assert-Throws {
            Assert-LocalCapacityCorpusMatrix ([pscustomobject]@{ ai = [pscustomobject]@{
                seeds = @(1729, 1730, 1731)
                scenarios = @('4v3', '4v2')
            } })
        } 'hard-ai-2v6' `
            'LocalCapacity corpus preflight rejects a manifest without the hard-AI scenario'
        Assert-Throws {
            Assert-LocalCapacityCorpusMatrix ([pscustomobject]@{ ai = [pscustomobject]@{
                seeds = @(1729, 1730)
                scenarios = @('4v3', '4v2', 'hard-ai-2v6')
            } })
        } 'three distinct' `
            'LocalCapacity corpus preflight rejects fewer than three seeds'
    }
    $functionAst = $runnerAst.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq 'Export-LocalCapacityAiCorpus'
    }, $true)
    Assert-True ($null -ne $functionAst) `
        'LocalCapacity runner exposes the replay corpus export function.'
    if ($null -eq $functionAst) { return }

    $recordAst = $functionAst.Find({
        param($node)
        if ($node -isnot [System.Management.Automation.Language.HashtableAst]) {
            return $false
        }
        $keys = @($node.KeyValuePairs | ForEach-Object {
            $keyAst = $_.Item1
            if ($keyAst -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                [string]$keyAst.Value
            }
            else {
                [string]$keyAst.Extent.Text
            }
        })
        return ($keys -contains 'sequence' -and
            $keys -contains 'skirmishAiReplayEpoch' -and
            $keys -contains 'actualAi' -and $keys -contains 'actualTeams')
    }, $true)
    Assert-True ($null -ne $recordAst) `
        'LocalCapacity runner reconstructs a native replay record.'
    if ($null -eq $recordAst) { return }

    $metadataAst = $functionAst.Find({
        param($node)
        if ($node -isnot [System.Management.Automation.Language.HashtableAst]) {
            return $false
        }
        $actualAiPair = @($node.KeyValuePairs | Where-Object {
            $keyAst = $_.Item1
            $keyName = if ($keyAst -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                [string]$keyAst.Value
            }
            else {
                [string]$keyAst.Extent.Text
            }
            $keyName -ceq 'actualAi' -and
                $_.Item2.Extent.Text -match '\$aiEvidence\.fields\.actual_ai'
        })
        $actualTeamsPair = @($node.KeyValuePairs | Where-Object {
            $keyAst = $_.Item1
            $keyName = if ($keyAst -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                [string]$keyAst.Value
            }
            else {
                [string]$keyAst.Extent.Text
            }
            $keyName -ceq 'actualTeams' -and
                $_.Item2.Extent.Text -match '\$aiEvidence\.fields\.actual_teams'
        })
        return $actualAiPair.Count -eq 1 -and $actualTeamsPair.Count -eq 1
    }, $true)
    Assert-True ($null -ne $metadataAst) `
        'LocalCapacity runner passes actual AI/team fields from trusted aiEvidence into export metadata.'

    foreach ($field in @('pathfindingReplayEpoch',
            'replayQualificationVersion', 'replayQualification')) {
        $fieldPairs = @($recordAst.KeyValuePairs | Where-Object {
            $keyAst = $_.Item1
            $keyName = if ($keyAst -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
                [string]$keyAst.Value
            }
            else {
                [string]$keyAst.Extent.Text
            }
            $keyName -ceq $field
        })
        Assert-True ($fieldPairs.Count -eq 1) `
            "LocalCapacity runner record must preserve exactly one '$field' field."
    }

    $entry = [pscustomobject]@{
        sequence = 73
        configuration = 'parallel-3'
        repeat = 2
        scenario = '4v3'
        seed = 4242
    }
    $completion = [pscustomobject]@{
        runNonce = 'ABCD-000073-00000002'
        replayEpoch = 17
        replaySha256 = ('C' * 64)
    }
    $aiEvidence = [pscustomobject]@{
        fields = @{
            actual_ai = '7'
            actual_teams = '4v3'
        }
    }
    $artifact = [pscustomobject]@{
        origin = 'artifact-origin-sentinel'
        title = 'artifact-title-sentinel'
        category = 'artifact-category-sentinel'
        executableSha256 = ('D' * 64)
        sourceProfileRoot = 'artifact-profile-sentinel'
        sourcePath = 'artifact-source-sentinel'
        sourceSha256 = ('E' * 64)
        destinationPath = 'artifact-destination-sentinel'
        destinationSha256 = ('F' * 64)
        length = [Int64]987654
        actualAi = 7
        actualTeams = '4v3'
        containerMagic = 'artifact-container-magic'
        containerSchemaVersion = 19
        containerEngineEpoch = 23
        payloadMagic = 'artifact-payload-magic'
        skirmishAiReplayEpoch = 29
        pathfindingReplayEpoch = 31
        replayQualificationVersion = 37
        replayQualification = 'artifact-qualification-sentinel'
        exportedUtc = 'artifact-exported-utc-sentinel'
    }
    $entrySequence = $entry.sequence
    $entryRepeat = $entry.repeat
    $entrySeed = $entry.seed
    $evaluatedRecord = Invoke-Expression $recordAst.Extent.Text
    Assert-True ($null -ne $evaluatedRecord) `
        'LocalCapacity runner record AST evaluates with explicit entry, completion, and artifact inputs.'
    Assert-True ($evaluatedRecord.sequence -eq $entry.sequence -and
        $evaluatedRecord.configuration -ceq $entry.configuration -and
        $evaluatedRecord.repeat -eq $entry.repeat -and
        $evaluatedRecord.scenario -ceq $entry.scenario -and
        $evaluatedRecord.seed -eq $entry.seed -and
        $evaluatedRecord.runNonce -ceq $completion.runNonce -and
        $evaluatedRecord.replayEpoch -eq $completion.replayEpoch -and
        $evaluatedRecord.replaySha256 -ceq $completion.replaySha256 -and
        $evaluatedRecord.actualAi -eq 7 -and
        $evaluatedRecord.actualTeams -ceq '4v3' -and
        $evaluatedRecord.pathfindingReplayEpoch -eq $artifact.pathfindingReplayEpoch -and
        $evaluatedRecord.replayQualificationVersion -eq $artifact.replayQualificationVersion -and
        $evaluatedRecord.replayQualification -ceq $artifact.replayQualification) `
        'LocalCapacity runner record AST does not preserve entry, completion, and qualification values.'
}

function Get-TestSha256 {
    param([string]$Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            return (($sha.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            }) -join '').ToUpperInvariant()
        }
        finally { $stream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-TestSha256FromBytes {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '').ToUpperInvariant()
    }
    finally { $sha.Dispose() }
}

function Write-TestUtf16String {
    param([IO.BinaryWriter]$Writer, [string]$Value)
    foreach ($character in $Value.ToCharArray()) {
        $Writer.Write([UInt16][char]$character)
    }
    $Writer.Write([UInt16]0)
}

function New-TestReplayBytes {
    param([string]$AiMarker = ' [SkirmishAIEpoch=3]', [int]$Variant = 0)
    $payloadStream = New-Object IO.MemoryStream
    try {
        $payloadWriter = New-Object IO.BinaryWriter($payloadStream)
        try {
            $payloadWriter.Write([Text.Encoding]::ASCII.GetBytes('GENREP'))
            $payloadWriter.Write([UInt32]1)
            $payloadWriter.Write([UInt32]2)
            $payloadWriter.Write([UInt32]3)
            $payloadWriter.Write([byte]0)
            $payloadWriter.Write([byte]0)
            for ($index = 0; $index -lt 8; ++$index) {
                $payloadWriter.Write([byte]0)
            }
            $replayName = if ($Variant -gt 0) { "Last Replay $Variant" } else { 'Last Replay' }
            Write-TestUtf16String $payloadWriter $replayName
            for ($index = 0; $index -lt 8; ++$index) {
                $payloadWriter.Write([UInt16]0)
            }
            Write-TestUtf16String $payloadWriter 'Version 1'
            Write-TestUtf16String $payloadWriter ('Build' + $AiMarker)
            $payloadWriter.Flush()
            $payload = $payloadStream.ToArray()
        }
        finally { $payloadWriter.Dispose() }
    }
    finally { $payloadStream.Dispose() }

    $containerStream = New-Object IO.MemoryStream
    try {
        $containerWriter = New-Object IO.BinaryWriter($containerStream)
        try {
            $containerWriter.Write([Text.Encoding]::ASCII.GetBytes('RPL3'))
            $containerWriter.Write([UInt32]2)
            $containerWriter.Write([UInt32]1)
            $containerWriter.Write([UInt64]0)
            $containerWriter.Write([UInt64]0)
            $containerWriter.Write([UInt64]$payload.LongLength)
            $containerWriter.Write([UInt32]0)
            $containerWriter.Write($payload)
            $containerWriter.Flush()
            return $containerStream.ToArray()
        }
        finally { $containerWriter.Dispose() }
    }
    finally { $containerStream.Dispose() }
}

function New-TestMetadata {
    param([string]$RunNonce, [string]$Scenario = '4v2', [int]$Seed = 1729,
        [string]$Category = 'native-fresh-ai', [string]$Title = 'ZeroHour',
        [int]$ActualAi = 0, [string]$ActualTeams = '')
    $metadata = [ordered]@{
        title = $Title
        category = $Category
        scenario = $Scenario
        seed = $Seed
        runNonce = $RunNonce
        executableSha256 = ('A' * 64)
        origin = 'native-fresh-runtime'
    }
    if ($Category -ceq 'local-capacity-ai') {
        if ($ActualAi -eq 0) {
            $ActualAi = if ($Scenario -ceq '4v2') { 6 }
                elseif ($Scenario -ceq 'hard-ai-2v6') { 8 }
                else { 7 }
        }
        if ([string]::IsNullOrWhiteSpace($ActualTeams)) {
            $ActualTeams = if ($Scenario -ceq 'hard-ai-2v6') { '2v6' }
                else { $Scenario }
        }
        $metadata.actualAi = $ActualAi
        $metadata.actualTeams = $ActualTeams
    }
    return $metadata
}

function Copy-TestRecord {
    param([Parameter(Mandatory = $true)][object]$Record)
    $copy = [pscustomobject]@{}
    foreach ($property in $Record.PSObject.Properties) {
        $copy | Add-Member -MemberType NoteProperty -Name $property.Name `
            -Value $property.Value | Out-Null
    }
    return $copy
}

function Complete-TestCorpusRecords {
    param([Parameter(Mandatory = $true)][object[]]$Records)
    for ($index = 0; $index -lt $Records.Count; ++$index) {
        $record = $Records[$index]
        if ($null -eq $record.PSObject.Properties['sequence']) {
            $record | Add-Member NoteProperty sequence ([int]($index + 1))
        }
        if ($null -eq $record.PSObject.Properties['configuration']) {
            $record | Add-Member NoteProperty configuration 'parallel-2'
        }
        if ($null -eq $record.PSObject.Properties['repeat']) {
            $record | Add-Member NoteProperty repeat ([int]1)
        }
        if ($null -eq $record.PSObject.Properties['replayEpoch']) {
            $record | Add-Member NoteProperty replayEpoch `
                ([int]$record.skirmishAiReplayEpoch)
        }
        if ($null -eq $record.PSObject.Properties['replaySha256']) {
            $record | Add-Member NoteProperty replaySha256 `
                ([string]$record.destinationSha256)
        }
    }
}

function Write-TestValidationResults {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object[]]$Records
    )
    Complete-TestCorpusRecords $Records
    $results = @($Records | ForEach-Object {
        $result = [ordered]@{
            sequence = $_.sequence
            title = $_.title
            kind = 'ai'
            configuration = $_.configuration
            repeat = $_.repeat
            seed = $_.seed
            scenario = $_.scenario
            exitCode = 0
            timedOut = $false
            aiEvidence = [ordered]@{
                fields = [ordered]@{
                    actual_ai = [string]$_.actualAi
                    actual_teams = [string]$_.actualTeams
                    run_nonce = [string]$_.runNonce
                    replay_epoch = [string]$_.replayEpoch
                    replay_sha256 = [string]$_.replaySha256
                    replay_retained = [string]$_.sourcePath
                }
            }
        }
        if ($null -ne $_.PSObject.Properties['maps'] -and $_.maps.Count -gt 0) {
            $result.aiEvidence.fields.map = $_.maps[0].profileRelativePath
            $result.aiEvidence.fields.map_sha256 = $_.maps[0].sha256
            $result.aiEvidence.fields.map_crc = $_.maps[0].crc
            $result.aiEvidence.fields.map_size = [string]$_.maps[0].byteCount
        }
        $result
    })
    [IO.File]::WriteAllText($Path, ($results | ConvertTo-Json -Depth 12))
}

function New-TestCorpusBundle {
    param(
        [Parameter(Mandatory = $true)][string]$TaskRoot,
        [Parameter(Mandatory = $true)][string]$CorpusRoot,
        [Parameter(Mandatory = $true)][object[]]$Records,
        [string]$Title = 'ZeroHour'
    )
    $resultsPath = Join-Path $TaskRoot 'validation-results.json'
    Write-TestValidationResults $resultsPath $Records
    $artifactIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $TaskRoot -CorpusExportRoot $CorpusRoot `
        -Title $Title -ExecutableSha256 ('A' * 64) `
        -Records $Records -ValidationResultsPath $resultsPath
    $receiptPath = Join-Path $TaskRoot 'local-capacity-receipt.json'
    $receipt = [ordered]@{
        schemaVersion = 1
        receiptKind = 'stage5-local-capacity-receipt'
        status = 'passed-non-acceptance'
        notAnAcceptanceEnvelope = $true
        finalAcceptanceEligible = $false
        validationMode = 'LocalCapacity'
        capacityMode = 'LocalCapacity'
        corpusExportRequested = $true
        corpusExportRoot = $CorpusRoot
        resultsSha256 = Get-TestSha256 $resultsPath
        corpusExport = [ordered]@{
            status = 'passed'
            corpusExportRoot = $CorpusRoot
            artifactIndexPath = $artifactIndex.path
            artifactIndexSha256 = $artifactIndex.sha256
            recordCount = $Records.Count
            records = $Records
        }
    }
    [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 16))
    $manifest = Write-Stage5FreshReplayCorpusManifest `
        -TaskRoot $TaskRoot -CorpusExportRoot $CorpusRoot `
        -Title $Title -ExecutableSha256 ('A' * 64) -Records $Records `
        -ValidationResultsPath $resultsPath -ValidationReceiptPath $receiptPath
    return [pscustomobject]@{
        taskRoot = $TaskRoot
        corpusRoot = $CorpusRoot
        resultsPath = $resultsPath
        receiptPath = $receiptPath
        artifactIndex = $artifactIndex
        manifest = $manifest
    }
}

Assert-LocalCapacityRunnerPreservesReplayQualification

$exporterModule = Get-Module -Name Stage5ReplayCorpusExporter |
    Select-Object -First 1
$completeAiMatrixRecords = @(
    foreach ($scenario in @('4v3', '4v2', 'hard-ai-2v6')) {
        foreach ($seed in @(1729, 1730, 1731)) {
            [pscustomobject]@{ scenario = $scenario; seed = [int]$seed }
        }
    }
)
& $exporterModule {
    param([object[]]$Records)
    Assert-Stage5ExporterFullAiSeedMatrix $Records | Out-Null
} $completeAiMatrixRecords
Assert-Throws {
    & $exporterModule {
        param([object[]]$Records)
        Assert-Stage5ExporterFullAiSeedMatrix $Records | Out-Null
    } @(
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1729 },
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1730 },
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1731 },
        [pscustomobject]@{ scenario = '4v2'; seed = [int]1729 },
        [pscustomobject]@{ scenario = '4v2'; seed = [int]1730 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1729 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1730 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1731 }
    )
} 'at least three distinct seeds.*4v2' `
    'corpus conversion rejects a required scenario with fewer than three seeds'
Assert-Throws {
    & $exporterModule {
        param([object[]]$Records)
        Assert-Stage5ExporterFullAiSeedMatrix $Records | Out-Null
    } @(
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1729 },
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1730 },
        [pscustomobject]@{ scenario = '4v3'; seed = [int]1731 },
        [pscustomobject]@{ scenario = '4v2'; seed = [int]1729 },
        [pscustomobject]@{ scenario = '4v2'; seed = [int]1730 },
        [pscustomobject]@{ scenario = '4v2'; seed = [int]1732 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1729 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1730 },
        [pscustomobject]@{ scenario = 'hard-ai-2v6'; seed = [int]1731 }
    )
} 'full scenario-by-seed cross-product' `
    'corpus conversion rejects different per-scenario seed sets'

$exporterSource = Get-Content (Join-Path $PSScriptRoot `
    'Stage5ReplayCorpusExporter.psm1') -Raw
Assert-True ($exporterSource -match
    '\[UInt32\]3221291008, \[UInt32\]4, \[IntPtr\]::Zero') `
    'commit-file native access/share constants must be represented without signed UInt32 overflow.'
Assert-True ($exporterSource -notmatch
    'DangerousGetHandle\(\).{0,80}\[(?:U?Int32)\]') `
    'native file identity must not truncate SafeFileHandle values to 32 bits.'

$nativeDrivePath = & $exporterModule {
    ConvertTo-Stage5ExporterNativePath 'H:\corpus\replay.rep' `
        'drive-path conversion test'
}
$nativeUncPath = & $exporterModule {
    ConvertTo-Stage5ExporterNativePath '\\server\share\corpus\replay.rep' `
        'UNC-path conversion test'
}
$nativePrefixedUncPath = & $exporterModule {
    ConvertTo-Stage5ExporterNativePath `
        '\\?\UNC\server\share\corpus\replay.rep' `
        'prefixed UNC-path conversion test'
}
Assert-True ($nativeDrivePath -ceq '\\?\H:\corpus\replay.rep' -and
    $nativeUncPath -ceq '\\?\UNC\server\share\corpus\replay.rep' -and
    $nativePrefixedUncPath -ceq '\\?\UNC\server\share\corpus\replay.rep') `
    'native extended-length conversion did not preserve drive and UNC path semantics.'

$scratchFull = [IO.Path]::GetFullPath($ScratchRoot).TrimEnd('\')
$scratchRootName = [IO.Path]::GetPathRoot($scratchFull)
Assert-True (-not [string]::IsNullOrWhiteSpace($scratchRootName) -and
    -not [String]::Equals($scratchFull, $scratchRootName,
        [StringComparison]::OrdinalIgnoreCase)) `
    'ScratchRoot must be a non-root filesystem directory.'
New-Item -ItemType Directory -Path $scratchFull -Force | Out-Null
$testRoot = Join-Path $scratchFull ('stage5-replay-export-{0}-{1}' -f
    $PID, [Guid]::NewGuid().ToString('N'))

try {
    $taskRoot = Join-Path $testRoot 'task'
    $taskRunRoot = Join-Path $taskRoot 'validation-run'
    $profileRoot = Join-Path $taskRunRoot 'Documents\Profile Name'
    $sourceDirectory = Join-Path $profileRoot 'Replays'
    $corpusRoot = Join-Path $taskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $sourceDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $corpusRoot -Force | Out-Null

    $sourcePath = Join-Path $sourceDirectory 'SkirmishAI-4v2-1729-00AA-000006.rep'
    [IO.File]::WriteAllBytes($sourcePath, (New-TestReplayBytes))
    $sourceSha256 = Get-TestSha256 $sourcePath
    $runNonce = '00AA-000006-00000001'
    $completion = 'SKIRMISH_AI_TEST_COMPLETE seed=1729 scenario=4v2 ' +
        'run_nonce=' + $runNonce + ' replay_epoch=3 replay_sha256=' +
        $sourceSha256 + ' replay_retained="' + $sourcePath + '"'
    $retention = Get-Stage5ReplayCompletionFields `
        -Output $completion -ExpectedSeed 1729 -ExpectedScenario '4v2' `
        -ExpectedTitle 'ZeroHour'
    Assert-True ($retention.replayRetained -ceq $sourcePath) `
        'completion parser did not retain a quoted source path.'
    Assert-True ($retention.replaySha256 -ceq $sourceSha256) `
        'completion parser did not retain the replay SHA-256.'

    $record = Export-Stage5FreshReplayArtifact `
        -SourcePath $retention.replayRetained `
        -ExpectedSha256 $retention.replaySha256 `
        -TaskRoot $taskRoot `
        -TaskRunRoot $taskRunRoot `
        -ProfileRoot $profileRoot `
        -CorpusExportRoot $corpusRoot `
        -Metadata (New-TestMetadata $retention.runNonce $retention.scenario $retention.seed)
    Assert-True (Test-Path -LiteralPath $record.destinationPath -PathType Leaf) `
        'valid replay was not exported.'
    Assert-True ($record.sourceSha256 -ceq $record.destinationSha256 -and
        $record.sourceSha256 -ceq $sourceSha256) `
        'source and destination hashes do not match.'
    $mapPath = Join-Path $taskRoot 'reviewed.map'
    [IO.File]::WriteAllBytes($mapPath, (New-Object byte[] 16384))
    $mapHash = (Get-FileHash $mapPath -Algorithm SHA256).Hash
    $reviewedMap = Read-Stage5ReviewedAiMap -ManifestDirectory $taskRoot -Map ([ordered]@{
        source='reviewed.map';mapKey='Maps\AiProof\AiProof.map';sha256=$mapHash;byteCount=16384;crc='00000000'
    })
    [IO.File]::WriteAllText($mapPath, 'changed after immutable map snapshot')
    $mappedRecord = Export-Stage5FreshReplayArtifact -SourcePath $retention.replayRetained `
        -ExpectedSha256 $retention.replaySha256 -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
        -ProfileRoot $profileRoot -CorpusExportRoot $corpusRoot -ReviewedMap $reviewedMap `
        -Metadata (New-TestMetadata '00AB-000006-00000001' $retention.scenario $retention.seed 'local-capacity-ai')
    Assert-True ($mappedRecord.maps.Count -eq 1 -and
        $mappedRecord.maps[0].profileRelativePath -ceq 'Maps\AiProof\AiProof.map' -and
        (Get-FileHash (Join-Path $corpusRoot $mappedRecord.maps[0].source) -Algorithm SHA256).Hash -ceq $mapHash) `
        'Exported replay lost its immutable reviewed map bytes or portable key.'
    $mapResultsPath = Join-Path $taskRoot 'map-results.json'
    Write-TestValidationResults $mapResultsPath @($mappedRecord)
    $mapResults = Get-Content -LiteralPath $mapResultsPath -Raw | ConvertFrom-Json
    $missingMapRecord = Copy-TestRecord $mappedRecord
    $missingMapRecord.PSObject.Properties.Remove('maps')
    Assert-Throws {
        & $exporterModule { param($record,$results) Assert-Stage5ExporterValidationResults @($record) $results } `
            $missingMapRecord $mapResults
    } 'map' 'Stripping a reviewed map dependency must invalidate corpus provenance'
    Assert-True ($record.containerMagic -ceq 'RPL3' -and
        $record.containerSchemaVersion -eq 2 -and
        $record.containerEngineEpoch -eq 1 -and
        $record.payloadMagic -ceq 'GENREP' -and
        $record.skirmishAiReplayEpoch -eq 3) `
        'valid replay container metadata was not recorded.'
    $temporaryFiles = @(Get-ChildItem -LiteralPath $corpusRoot -Recurse -Force -File |
        Where-Object { $_.Name -like '*.tmp-*' })
    Assert-True ($temporaryFiles.Count -eq 0) `
        'successful export left an exporter temporary file behind.'

    # The installed runner can produce task-owned destination paths beyond the
    # legacy MAX_PATH boundary. Exercise the real held-source, ancestor-chain,
    # temporary-create, atomic-rename, and final-identity path end to end.
    $longTaskRoot = Join-Path $testRoot 'long-path-task'
    $longSegmentIndex = 0
    while ($longTaskRoot.Length -lt 205) {
        $longSegment = 'segment-{0:D2}-{1}' -f $longSegmentIndex, ('x' * 40)
        $longTaskRoot = Join-Path $longTaskRoot $longSegment
        ++$longSegmentIndex
    }
    $longTaskRunRoot = Join-Path $longTaskRoot 'validation-run'
    $longProfileRoot = Join-Path $longTaskRunRoot 'Documents\Profile Name'
    $longSourceDirectory = Join-Path $longProfileRoot 'Replays'
    $longCorpusRoot = Join-Path $longTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $longSourceDirectory, $longCorpusRoot `
        -Force | Out-Null
    $longSourcePath = Join-Path $longSourceDirectory `
        'SkirmishAI-4v2-1729-00AA-000006-long-path.rep'
    [IO.File]::WriteAllBytes($longSourcePath, (New-TestReplayBytes))
    $longSourceSha256 = Get-TestSha256 $longSourcePath
    $longRunNonce = '00AA-000006-00000011'
    $longExpectedDestination = Join-Path `
        (Join-Path (Join-Path $longCorpusRoot 'ZeroHour') 'native-fresh-ai') `
        "native-fresh-ai-scenario-4v2-seed-1729-$longRunNonce.rep"
    Assert-True ($longSourcePath.Length -gt 260 -and
        $longExpectedDestination.Length -gt 260) `
        'long-path regression fixture did not cross the legacy MAX_PATH boundary.'
    $longRecord = Export-Stage5FreshReplayArtifact `
        -SourcePath $longSourcePath -ExpectedSha256 $longSourceSha256 `
        -TaskRoot $longTaskRoot -TaskRunRoot $longTaskRunRoot `
        -ProfileRoot $longProfileRoot -CorpusExportRoot $longCorpusRoot `
        -Metadata (New-TestMetadata $longRunNonce)
    Assert-True ([String]::Equals($longRecord.destinationPath,
        $longExpectedDestination, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $longRecord.destinationPath -PathType Leaf) -and
        $longRecord.destinationSha256 -ceq $longSourceSha256) `
        'extended-length replay export did not preserve destination identity and bytes.'
    $longTemporaryFiles = @(Get-ChildItem -LiteralPath $longCorpusRoot -Recurse `
        -Force -File | Where-Object { $_.Name -like '*.tmp-*' })
    Assert-True ($longTemporaryFiles.Count -eq 0) `
        'extended-length replay export left an exporter temporary file behind.'

    # Replay markers and completion epochs are title-specific and must remain
    # consistent through the reader, completion parser, and record validation.
    $generalsTaskRoot = Join-Path $testRoot 'generals-task'
    $generalsTaskRunRoot = Join-Path $generalsTaskRoot 'validation-run'
    $generalsProfileRoot = Join-Path $generalsTaskRunRoot 'Documents\Profile Name'
    $generalsSourceDirectory = Join-Path $generalsProfileRoot 'Replays'
    $generalsCorpusRoot = Join-Path $generalsTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $generalsSourceDirectory, $generalsCorpusRoot -Force | Out-Null
    $generalsSourcePath = Join-Path $generalsSourceDirectory 'SkirmishAI-4v2-1729-AE-000001.rep'
    [IO.File]::WriteAllBytes($generalsSourcePath,
        (New-TestReplayBytes -AiMarker ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]'))
    $generalsSourceSha256 = Get-TestSha256 $generalsSourcePath
    $generalsRunNonce = '00AE-000001-00000001'
    $generalsCompletion = 'SKIRMISH_AI_TEST_COMPLETE seed=1729 scenario=4v2 ' +
        'run_nonce=' + $generalsRunNonce + ' replay_epoch=1 replay_sha256=' +
        $generalsSourceSha256 + ' replay_retained="' + $generalsSourcePath + '"'
    $generalsRetention = Get-Stage5ReplayCompletionFields `
        -Output $generalsCompletion -ExpectedSeed 1729 -ExpectedScenario '4v2' `
        -ExpectedTitle 'Generals'
    Assert-True ($generalsRetention.replayEpoch -eq 1 -and
        $generalsRetention.replayRetained -ceq $generalsSourcePath) `
        'Generals completion parsing did not preserve the title-specific epoch-1 contract.'
    $generalsRecord = Export-Stage5FreshReplayArtifact `
        -SourcePath $generalsSourcePath -ExpectedSha256 $generalsSourceSha256 `
        -TaskRoot $generalsTaskRoot -TaskRunRoot $generalsTaskRunRoot `
        -ProfileRoot $generalsProfileRoot -CorpusExportRoot $generalsCorpusRoot `
        -Metadata (New-TestMetadata $generalsRunNonce '4v2' 1729 `
            'native-fresh-ai' 'Generals')
    Assert-True ($generalsRecord.title -ceq 'Generals' -and
        $generalsRecord.skirmishAiReplayEpoch -eq 1) `
        'Generals export did not retain the title-specific epoch-1 replay marker.'
    Assert-CurrentGeneralsReplayQualification $generalsRecord `
        'Generals export'
    $generalsResultsPath = Join-Path $generalsTaskRoot 'validation-results.json'
    [IO.File]::WriteAllText($generalsResultsPath, '{"status":"passed"}')
    $generalsIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $generalsTaskRoot -CorpusExportRoot $generalsCorpusRoot `
        -Title 'Generals' -ExecutableSha256 ('A' * 64) `
        -Records @($generalsRecord) -ValidationResultsPath $generalsResultsPath
    Assert-True (Test-Path -LiteralPath $generalsIndex.path -PathType Leaf) `
        'Generals artifact index did not accept a title-matched epoch-1 record.'
    $generalsManifest = Write-Stage5FreshReplayCorpusManifest `
        -TaskRoot $generalsTaskRoot -CorpusExportRoot $generalsCorpusRoot `
        -Title 'Generals' -ExecutableSha256 ('A' * 64) `
        -Records @($generalsRecord) -ValidationResultsPath $generalsResultsPath
    Assert-True (Test-Path -LiteralPath $generalsManifest.path -PathType Leaf) `
        'Generals corpus manifest did not accept a title-matched epoch-1 record.'
    $generalsManifestDocument = Get-Content -LiteralPath $generalsManifest.path -Raw |
        ConvertFrom-Json
    Assert-CurrentGeneralsReplayQualification $generalsManifestDocument.records[0] `
        'Generals corpus manifest record'

    # A legacy Generals replay carrying only the AI epoch remains readable for
    # diagnostics, but it is not current-path-qualified fresh-corpus input.
    $legacyGeneralsSourcePath = Join-Path $generalsSourceDirectory 'legacy-ai-only.rep'
    [IO.File]::WriteAllBytes($legacyGeneralsSourcePath,
        (New-TestReplayBytes -AiMarker ' [GeneralsAIPlanningEpoch=1]'))
    $legacyGeneralsSourceSha256 = Get-TestSha256 $legacyGeneralsSourcePath
    $legacyGeneralsCompletion = 'SKIRMISH_AI_TEST_COMPLETE seed=1729 scenario=4v2 ' +
        'run_nonce=00AE-000001-00000008 replay_epoch=1 replay_sha256=' +
        $legacyGeneralsSourceSha256 + ' replay_retained="' +
        $legacyGeneralsSourcePath + '"'
    $legacyGeneralsRetention = Get-Stage5ReplayCompletionFields `
        -Output $legacyGeneralsCompletion -ExpectedSeed 1729 `
        -ExpectedScenario '4v2' -ExpectedTitle 'Generals' `
        -Context 'legacy Generals diagnostic completion'
    Assert-True ($legacyGeneralsRetention.replayEpoch -eq 1 -and
        $legacyGeneralsRetention.replayRetained -ceq $legacyGeneralsSourcePath) `
        'legacy Generals AI-only completion was not preserved for diagnostics.'
    $exporterModule = Get-Module -Name Stage5ReplayCorpusExporter |
        Select-Object -First 1
    $legacyGeneralsHeader = & $exporterModule {
        param([string]$ReplayPath)
        Read-Stage5ReplayContainer -Path $ReplayPath -ExpectedTitle 'Generals'
    } $legacyGeneralsSourcePath
    Assert-True ($legacyGeneralsHeader.skirmishAiReplayEpoch -eq 1 -and
        $legacyGeneralsHeader.versionTime.EndsWith(
            '[GeneralsAIPlanningEpoch=1]', [StringComparison]::Ordinal)) `
        'generic replay reading did not preserve the legacy Generals AI-only classification.'
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $legacyGeneralsSourcePath `
            -ExpectedSha256 $legacyGeneralsSourceSha256 `
            -TaskRoot $generalsTaskRoot -TaskRunRoot $generalsTaskRunRoot `
            -ProfileRoot $generalsProfileRoot `
            -CorpusExportRoot (Join-Path $generalsTaskRoot 'legacy-ai-only-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000008' '4v2' 1729 `
                'native-fresh-ai' 'Generals')
    } 'pathfinding|current.*path|marker|replay' `
        'fresh Generals export rejects a legacy AI-only replay'

    $invalidGeneralsMarkers = @(
        [pscustomobject]@{
            name = 'path-only'
            marker = ' [GeneralsPathfindingEpoch=1]'
            nonce = '00AE-000001-00000009'
        }
        [pscustomobject]@{
            name = 'future-path-epoch'
            marker = ' [GeneralsPathfindingEpoch=2] [GeneralsAIPlanningEpoch=1]'
            nonce = '00AE-000001-0000000A'
        }
        [pscustomobject]@{
            name = 'future-ai-epoch'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=2]'
            nonce = '00AE-000001-0000000B'
        }
        [pscustomobject]@{
            name = 'reversed-pair'
            marker = ' [GeneralsAIPlanningEpoch=1] [GeneralsPathfindingEpoch=1]'
            nonce = '00AE-000001-0000000C'
        }
        [pscustomobject]@{
            name = 'duplicate-path'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]'
            nonce = '00AE-000001-0000000D'
        }
        [pscustomobject]@{
            name = 'duplicate-ai'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] [GeneralsAIPlanningEpoch=1]'
            nonce = '00AE-000001-0000000E'
        }
        [pscustomobject]@{
            name = 'malformed-path-prefix'
            marker = ' [GeneralsPathfindingEpoch] [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]'
            nonce = '00AE-000001-0000000F'
        }
        [pscustomobject]@{
            name = 'malformed-ai-prefix'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch] [GeneralsAIPlanningEpoch=1]'
            nonce = '00AE-000001-00000010'
        }
        [pscustomobject]@{
            name = 'extra-suffix'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] trailing'
            nonce = '00AE-000001-00000011'
        }
        [pscustomobject]@{
            name = 'mixed-title-marker'
            marker = ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1] [SkirmishAIEpoch=3]'
            nonce = '00AE-000001-00000012'
        }
    )
    foreach ($invalidMarker in $invalidGeneralsMarkers) {
        $invalidPath = Join-Path $generalsSourceDirectory `
            ('invalid-generals-{0}.rep' -f $invalidMarker.name)
        [IO.File]::WriteAllBytes($invalidPath,
            (New-TestReplayBytes -AiMarker $invalidMarker.marker))
        Assert-Throws {
            Export-Stage5FreshReplayArtifact `
                -SourcePath $invalidPath -ExpectedSha256 (Get-TestSha256 $invalidPath) `
                -TaskRoot $generalsTaskRoot -TaskRunRoot $generalsTaskRunRoot `
                -ProfileRoot $generalsProfileRoot `
                -CorpusExportRoot (Join-Path $generalsTaskRoot `
                    ('invalid-generals-{0}-corpus' -f $invalidMarker.name)) `
                -Metadata (New-TestMetadata $invalidMarker.nonce '4v2' 1729 `
                    'native-fresh-ai' 'Generals')
        } 'pathfinding|current.*path|marker|epoch|title|replay' `
            ('fresh Generals export rejects {0} marker' -f $invalidMarker.name)
    }

    $zeroHourGeneralsPairPath = Join-Path $generalsSourceDirectory `
        'zerohour-with-generals-path-pair.rep'
    [IO.File]::WriteAllBytes($zeroHourGeneralsPairPath,
        (New-TestReplayBytes -AiMarker ' [GeneralsPathfindingEpoch=1] [GeneralsAIPlanningEpoch=1]'))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $zeroHourGeneralsPairPath `
            -ExpectedSha256 (Get-TestSha256 $zeroHourGeneralsPairPath) `
            -TaskRoot $generalsTaskRoot -TaskRunRoot $generalsTaskRunRoot `
            -ProfileRoot $generalsProfileRoot `
            -CorpusExportRoot (Join-Path $generalsTaskRoot 'zerohour-generals-pair-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000013' '4v2' 1729 `
                'native-fresh-ai' 'ZeroHour')
    } 'pathfinding|current.*path|marker|epoch|title|replay' `
        'Zero Hour export rejects a Generals pathfinding marker pair'

    # Qualification metadata must not promote old bytes: even a forged record
    # claiming version 2/current path qualification is re-read from disk.
    $forgedMetadataCorpusRoot = Join-Path $generalsTaskRoot 'forged-legacy-corpus'
    $forgedMetadataDirectory = Join-Path $forgedMetadataCorpusRoot 'Generals\native-fresh-ai'
    New-Item -ItemType Directory -Path $forgedMetadataDirectory -Force | Out-Null
    $forgedDestinationPath = Join-Path $forgedMetadataDirectory 'legacy-ai-only.rep'
    [IO.File]::WriteAllBytes($forgedDestinationPath,
        [IO.File]::ReadAllBytes($legacyGeneralsSourcePath))
    $forgedGeneralsRecord = [pscustomobject]@{}
    foreach ($property in $generalsRecord.PSObject.Properties) {
        $forgedGeneralsRecord | Add-Member -MemberType NoteProperty `
            -Name $property.Name -Value $property.Value
    }
    Set-TestRecordProperty $forgedGeneralsRecord 'title' 'Generals'
    Set-TestRecordProperty $forgedGeneralsRecord 'sourcePath' $legacyGeneralsSourcePath
    Set-TestRecordProperty $forgedGeneralsRecord 'sourceSha256' $legacyGeneralsSourceSha256
    Set-TestRecordProperty $forgedGeneralsRecord 'destinationPath' $forgedDestinationPath
    Set-TestRecordProperty $forgedGeneralsRecord 'destinationSha256' $legacyGeneralsSourceSha256
    Set-TestRecordProperty $forgedGeneralsRecord 'length' ([Int64](Get-Item -LiteralPath $legacyGeneralsSourcePath).Length)
    Set-TestRecordProperty $forgedGeneralsRecord 'replayQualificationVersion' 2
    Set-TestRecordProperty $forgedGeneralsRecord 'replayQualification' 'current-path-qualified'
    Set-TestRecordProperty $forgedGeneralsRecord 'pathfindingReplayEpoch' 1
    Assert-Throws {
        Write-Stage5FreshReplayArtifactIndex `
            -TaskRoot $generalsTaskRoot -CorpusExportRoot $forgedMetadataCorpusRoot `
            -Title 'Generals' -ExecutableSha256 ('A' * 64) `
            -Records @($forgedGeneralsRecord) -ValidationResultsPath $generalsResultsPath
    } 'pathfinding|current.*path|marker|replay' `
        'artifact index rejects forged current-path metadata over legacy bytes'
    Assert-Throws {
        Write-Stage5FreshReplayCorpusManifest `
            -TaskRoot $generalsTaskRoot -CorpusExportRoot $forgedMetadataCorpusRoot `
            -Title 'Generals' -ExecutableSha256 ('A' * 64) `
            -Records @($forgedGeneralsRecord) -ValidationResultsPath $generalsResultsPath
    } 'pathfinding|current.*path|marker|replay' `
        'corpus manifest rejects forged current-path metadata over legacy bytes'

    # A record without the versioned qualification fields is historical V1
    # proof and cannot be silently promoted into a fresh corpus.
    $v1GeneralsDestinationPath = Join-Path $forgedMetadataDirectory 'unversioned-pair.rep'
    [IO.File]::WriteAllBytes($v1GeneralsDestinationPath,
        [IO.File]::ReadAllBytes($generalsSourcePath))
    $v1GeneralsRecord = [pscustomobject]@{}
    foreach ($property in $generalsRecord.PSObject.Properties) {
        $v1GeneralsRecord | Add-Member -MemberType NoteProperty `
            -Name $property.Name -Value $property.Value
    }
    Set-TestRecordProperty $v1GeneralsRecord 'sourcePath' $generalsSourcePath
    Set-TestRecordProperty $v1GeneralsRecord 'sourceSha256' $generalsSourceSha256
    Set-TestRecordProperty $v1GeneralsRecord 'destinationPath' $v1GeneralsDestinationPath
    Set-TestRecordProperty $v1GeneralsRecord 'destinationSha256' $generalsSourceSha256
    Set-TestRecordProperty $v1GeneralsRecord 'length' ([Int64](Get-Item -LiteralPath $generalsSourcePath).Length)
    foreach ($propertyName in @('replayQualificationVersion',
            'replayQualification', 'pathfindingReplayEpoch')) {
        $property = $v1GeneralsRecord.PSObject.Properties[$propertyName]
        if ($null -ne $property) {
            $v1GeneralsRecord.PSObject.Properties.Remove($propertyName)
        }
    }
    Assert-Throws {
        Write-Stage5FreshReplayArtifactIndex `
            -TaskRoot $generalsTaskRoot -CorpusExportRoot $forgedMetadataCorpusRoot `
            -Title 'Generals' -ExecutableSha256 ('A' * 64) `
            -Records @($v1GeneralsRecord) -ValidationResultsPath $generalsResultsPath
    } 'qualification|version|current|path|record' `
        'artifact index rejects unversioned Generals qualification proof'

    # Cross-title and mixed-marker negatives must remain rejected even when a
    # marker from another title would satisfy the current hard-coded parser.
    $generalsEpoch3Completion = $generalsCompletion.Replace(
        'replay_epoch=1', 'replay_epoch=3')
    Assert-Throws {
        Get-Stage5ReplayCompletionFields `
            -Output $generalsEpoch3Completion -ExpectedSeed 1729 `
            -ExpectedScenario '4v2' -ExpectedTitle 'Generals'
    } 'epoch|title' `
        'Generals completion rejects a Zero Hour epoch'
    $zeroHourEpoch1Completion = $completion.Replace('replay_epoch=3', 'replay_epoch=1')
    Assert-Throws {
        Get-Stage5ReplayCompletionFields `
            -Output $zeroHourEpoch1Completion -ExpectedSeed 1729 `
            -ExpectedScenario '4v2' -ExpectedTitle 'ZeroHour'
    } 'epoch|title' `
        'Zero Hour completion rejects a Generals epoch'

    $generalsWrongMarkerPath = Join-Path $sourceDirectory 'generals-with-zerohour-marker.rep'
    [IO.File]::WriteAllBytes($generalsWrongMarkerPath, (New-TestReplayBytes))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $generalsWrongMarkerPath `
            -ExpectedSha256 (Get-TestSha256 $generalsWrongMarkerPath) `
            -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'wrong-generals-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000002' '4v2' 1729 `
                'native-fresh-ai' 'Generals')
    } 'epoch|title|marker|replay' `
        'Generals export rejects a Zero Hour marker'

    $mixedMarkerPath = Join-Path $sourceDirectory 'mixed-title-markers.rep'
    [IO.File]::WriteAllBytes($mixedMarkerPath,
        (New-TestReplayBytes -AiMarker ' [GeneralsAIPlanningEpoch=1] [SkirmishAIEpoch=3]'))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $mixedMarkerPath `
            -ExpectedSha256 (Get-TestSha256 $mixedMarkerPath) `
            -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'mixed-marker-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000003' '4v2' 1729 `
                'native-fresh-ai' 'ZeroHour')
    } 'epoch|title|marker|mixed|replay' `
        'export rejects mixed title replay markers'

    $reverseMixedMarkerPath = Join-Path $sourceDirectory 'reverse-mixed-title-markers.rep'
    [IO.File]::WriteAllBytes($reverseMixedMarkerPath,
        (New-TestReplayBytes -AiMarker ' [SkirmishAIEpoch=3] [GeneralsAIPlanningEpoch=1]'))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $reverseMixedMarkerPath `
            -ExpectedSha256 (Get-TestSha256 $reverseMixedMarkerPath) `
            -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'reverse-mixed-marker-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000005' '4v2' 1729 `
                'native-fresh-ai' 'Generals')
    } 'epoch|title|marker|mixed|replay' `
        'export rejects reverse-order mixed title replay markers'

    $malformedDuplicateMarkerPath = Join-Path $sourceDirectory 'malformed-duplicate-marker.rep'
    [IO.File]::WriteAllBytes($malformedDuplicateMarkerPath,
        (New-TestReplayBytes -AiMarker ' [SkirmishAI] [SkirmishAIEpoch=3]'))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $malformedDuplicateMarkerPath `
            -ExpectedSha256 (Get-TestSha256 $malformedDuplicateMarkerPath) `
            -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'malformed-duplicate-marker-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000006')
    } 'epoch|title|marker|mixed|replay' `
        'export rejects malformed duplicate title marker prefixes'

    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $sourcePath -ExpectedSha256 $sourceSha256 `
            -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'invalid-title-corpus') `
            -Metadata (New-TestMetadata '00AE-000001-00000007' '4v2' 1729 `
                'native-fresh-ai' 'UnsupportedTitle')
    } 'Generals|ZeroHour|title' `
        'export rejects an unsupported replay title'

    $crossTitleRecordCorpusRoot = Join-Path $taskRoot 'cross-title-record-corpus'
    New-Item -ItemType Directory -Path $crossTitleRecordCorpusRoot -Force | Out-Null
    $zeroHourRecordForCrossTitle = Export-Stage5FreshReplayArtifact `
        -SourcePath $sourcePath -ExpectedSha256 $sourceSha256 `
        -TaskRoot $taskRoot -TaskRunRoot $taskRunRoot `
        -ProfileRoot $profileRoot -CorpusExportRoot $crossTitleRecordCorpusRoot `
        -Metadata (New-TestMetadata '00AE-000001-00000004')
    $forgedGeneralsRecord = [pscustomobject]@{}
    foreach ($property in $zeroHourRecordForCrossTitle.PSObject.Properties) {
        $forgedGeneralsRecord | Add-Member -MemberType NoteProperty `
            -Name $property.Name -Value $property.Value
    }
    $forgedGeneralsRecord.title = 'Generals'
    $crossTitleResultsPath = Join-Path $taskRoot 'cross-title-results.json'
    [IO.File]::WriteAllText($crossTitleResultsPath, '{"status":"passed"}')
    Assert-Throws {
        Write-Stage5FreshReplayArtifactIndex `
            -TaskRoot $taskRoot -CorpusExportRoot $crossTitleRecordCorpusRoot `
            -Title 'Generals' -ExecutableSha256 ('A' * 64) `
            -Records @($forgedGeneralsRecord) `
            -ValidationResultsPath $crossTitleResultsPath
    } 'epoch|title|marker|replay' `
        'record validation rejects a Zero Hour artifact relabeled as Generals'

    $validationResults = Join-Path $taskRoot 'validation-results.json'
    $validationReceipt = Join-Path $taskRoot 'validation-results-receipt.json'
    [IO.File]::WriteAllText($validationResults, '{"status":"passed"}')
    [IO.File]::WriteAllText($validationReceipt, '{"role":"validation-results"}')
    $manifest = Write-Stage5FreshReplayCorpusManifest `
        -TaskRoot $taskRoot `
        -CorpusExportRoot $corpusRoot `
        -Title 'ZeroHour' `
        -ExecutableSha256 ('A' * 64) `
        -Records @($record) `
        -ValidationResultsPath $validationResults `
        -ValidationReceiptPath $validationReceipt
    Assert-True (Test-Path -LiteralPath $manifest.path -PathType Leaf) `
        'corpus manifest was not created.'
    $manifestDocument = Get-Content -LiteralPath $manifest.path -Raw | ConvertFrom-Json
    Assert-True ($manifestDocument.origin -ceq 'native-fresh-runtime' -and
        $manifestDocument.title -ceq 'ZeroHour' -and
        $manifestDocument.records.Count -eq 1 -and
        $manifestDocument.records[0].category -ceq 'native-fresh-ai' -and
        $manifestDocument.records[0].scenario -ceq '4v2' -and
        [int]$manifestDocument.records[0].seed -eq 1729 -and
        $manifestDocument.records[0].sourcePath -ceq $sourcePath) `
        'corpus manifest did not preserve native-fresh metadata and paths.'
    Assert-True ($manifestDocument.validationReceipt.sha256 -match '^[0-9A-F]{64}$') `
        'corpus manifest did not bind the validation receipt hash.'

    $conversionTaskRoot = Join-Path $testRoot 'conversion-task'
    $conversionTaskRunRoot = Join-Path $conversionTaskRoot 'validation-run'
    $conversionProfileRoot = Join-Path $conversionTaskRunRoot 'Documents\Profile Name'
    $conversionSourceDirectory = Join-Path $conversionProfileRoot 'Replays'
    $conversionCorpusRoot = Join-Path $conversionTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $conversionSourceDirectory, $conversionCorpusRoot -Force | Out-Null
    $conversionRecords = New-Object 'Collections.Generic.List[object]'
    for ($index = 1; $index -le 15; ++$index) {
        $scenario = @('4v2', '4v3', 'hard-ai-2v6')[($index - 1) % 3]
        $seed = 1701 + [int][Math]::Floor(($index - 1) / 3)
        $nonce = '00AA-000006-{0:D8}' -f $index
        $source = Join-Path $conversionSourceDirectory "record-$index.rep"
        [IO.File]::WriteAllBytes($source, (New-TestReplayBytes -Variant $index))
        $sourceHash = Get-TestSha256 $source
        $conversionRecords.Add((Export-Stage5FreshReplayArtifact `
            -SourcePath $source -ExpectedSha256 $sourceHash `
            -TaskRoot $conversionTaskRoot -TaskRunRoot $conversionTaskRunRoot `
            -ProfileRoot $conversionProfileRoot -CorpusExportRoot $conversionCorpusRoot `
            -Metadata (New-TestMetadata $nonce $scenario $seed 'local-capacity-ai'))) | Out-Null
    }
    Complete-TestCorpusRecords $conversionRecords.ToArray()
    $conversionMap = & $exporterModule { param($map,$root) Export-Stage5ReviewedAiReplayMap $map $root } $reviewedMap $conversionCorpusRoot
    foreach ($record in @($conversionRecords.ToArray() | Where-Object scenario -CEQ '4v2')) {
        $record | Add-Member -NotePropertyName maps -NotePropertyValue @($conversionMap)
    }
    foreach ($record in $conversionRecords.ToArray()) {
        $record.configuration = 'serial-1'
    }
    $hard2v6Record = @($conversionRecords.ToArray() | Where-Object {
        $_.scenario -ceq 'hard-ai-2v6'
    } | Sort-Object destinationSha256 | Select-Object -First 1)[0]
    Assert-True ($null -ne $hard2v6Record) `
        'conversion fixture setup produced its native hard-ai-2v6 scenario record.'
    $hard2v6Completion = 'SKIRMISH_AI_TEST_COMPLETE seed=' +
        $hard2v6Record.seed + ' scenario=hard-ai-2v6 run_nonce=' +
        $hard2v6Record.runNonce + ' replay_epoch=3 replay_sha256=' +
        $hard2v6Record.sourceSha256 + ' replay_retained="' +
        $hard2v6Record.sourcePath + '"'
    $hard2v6Parsed = Get-Stage5ReplayCompletionFields `
        -Output $hard2v6Completion -ExpectedSeed ([int]$hard2v6Record.seed) `
        -ExpectedScenario 'hard-ai-2v6' -ExpectedTitle 'ZeroHour'
    Assert-True ($hard2v6Parsed.scenario -ceq 'hard-ai-2v6' -and
        $hard2v6Parsed.runNonce -ceq $hard2v6Record.runNonce -and
        $hard2v6Parsed.replaySha256 -ceq $hard2v6Record.sourceSha256 -and
        $hard2v6Parsed.replayRetained -ceq $hard2v6Record.sourcePath) `
        'completion parsing must preserve the true hard-ai-2v6 scenario and retained source binding'
    $conversionResults = Join-Path $conversionTaskRoot 'validation-results.json'
    Write-TestValidationResults $conversionResults $conversionRecords.ToArray()
    $wrongMapResults = Get-Content -LiteralPath $conversionResults -Raw | ConvertFrom-Json
    foreach ($result in @($wrongMapResults | Where-Object scenario -CEQ '4v2')) {
        $result.aiEvidence.fields.map_sha256 = 'F' * 64
    }
    Assert-Throws {
        & $exporterModule { param($records,$results) Assert-Stage5ExporterValidationResults $records $results } `
            $conversionRecords.ToArray() $wrongMapResults
    } 'map' 'Replay map must match its actual completion provenance'
    $artifactIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $conversionTaskRoot -CorpusExportRoot $conversionCorpusRoot `
        -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
        -Records $conversionRecords.ToArray() -ValidationResultsPath $conversionResults `
        -CaptureMode 'serial-baseline-ai'
    $conversionReceiptPath = Join-Path $conversionTaskRoot 'local-capacity-receipt.json'
    $conversionReceipt = [ordered]@{
        schemaVersion = 1
        receiptKind = 'stage5-local-capacity-receipt'
        status = 'passed-non-acceptance'
        notAnAcceptanceEnvelope = $true
        finalAcceptanceEligible = $false
        validationMode = 'LocalCapacity'
        capacityMode = 'LocalCapacity'
        corpusExportRequested = $true
        corpusExportRoot = $conversionCorpusRoot
        resultsSha256 = Get-TestSha256 $conversionResults
        corpusExport = [ordered]@{
            status = 'passed'
            captureMode = 'serial-baseline-ai'
            corpusExportRoot = $conversionCorpusRoot
            artifactIndexPath = $artifactIndex.path
            artifactIndexSha256 = $artifactIndex.sha256
            recordCount = $conversionRecords.Count
            records = $conversionRecords.ToArray()
        }
    }
    [IO.File]::WriteAllText($conversionReceiptPath,
        ($conversionReceipt | ConvertTo-Json -Depth 16))
    $conversionManifest = Write-Stage5FreshReplayCorpusManifest `
        -TaskRoot $conversionTaskRoot -CorpusExportRoot $conversionCorpusRoot `
        -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
        -Records $conversionRecords.ToArray() -ValidationResultsPath $conversionResults `
        -ValidationReceiptPath $conversionReceiptPath `
        -CaptureMode 'serial-baseline-ai'
    $originalConversionManifestText = Get-Content -LiteralPath $conversionManifest.path -Raw
    $parallelCaptureManifest = $originalConversionManifestText | ConvertFrom-Json
    $parallelCaptureManifest.records[0].configuration = 'parallel-2'
    [IO.File]::WriteAllText($conversionManifest.path,
        ($parallelCaptureManifest | ConvertTo-Json -Depth 16))
    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $conversionManifest.path `
            -FixtureManifestPath (Join-Path $conversionCorpusRoot 'parallel-capture-fixtures.json') `
            -ProvenancePath (Join-Path $conversionCorpusRoot 'parallel-capture-provenance.json') `
            -Executable 'generalszh.exe' | Out-Null
    } 'serial-1|serial-baseline|non-serial' `
        'serial-baseline corpus read rejects a parallel record relabeled as capture input'
    [IO.File]::WriteAllText($conversionManifest.path, $originalConversionManifestText)
    $fixtureManifestPath = Join-Path $conversionCorpusRoot 'native-fixture-manifest.json'
    $provenancePath = Join-Path $conversionCorpusRoot 'native-fixture-provenance.json'
    $conversion = Convert-Stage5FreshReplayCorpusManifestToFixtures `
        -CorpusManifestPath $conversionManifest.path `
        -FixtureManifestPath $fixtureManifestPath `
        -ProvenancePath $provenancePath -Executable 'generalszh.exe'
    $fixtureDocument = Get-Content -LiteralPath $fixtureManifestPath -Raw | ConvertFrom-Json
    $provenanceDocument = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
    $stressFixtures = @($fixtureDocument.fixtures | Where-Object { $_.stress })
    $selectedShas = @($fixtureDocument.fixtures | ForEach-Object { $_.sha256 })
    $richRecords = @($provenanceDocument.fixtures)
    $mappedFixtures = @($fixtureDocument.fixtures | Where-Object { $_.maps.Count -gt 0 })
    Assert-True ($mappedFixtures.Count -gt 0 -and
        @($mappedFixtures | Where-Object { $_.maps[0].sha256 -cne $mapHash -or $_.maps[0].profileRelativePath -cne 'Maps\AiProof\AiProof.map' }).Count -eq 0) `
        'Replay fixture conversion lost the exact portable map closure.'
    Assert-True ($conversion.fixtureCount -eq 10 -and
        $fixtureDocument.fixtures.Count -eq 10 -and
        $fixtureDocument.ai.seeds.Count -eq 5 -and
        (@($fixtureDocument.ai.scenarios | Sort-Object) -join ',') -ceq
            '4v2,4v3,hard-ai-2v6' -and
        $stressFixtures.Count -eq 1 -and
        $richRecords.Count -eq 10 -and
        (@($selectedShas | Sort-Object -Unique).Count -eq 10) -and
        $stressFixtures[0].id -ceq 'native-stress-hard-ai-2v6' -and
        $richRecords[0].category -ceq 'local-capacity-ai' -and
        $provenanceDocument.captureMode -ceq 'serial-baseline-ai' -and
        (@($richRecords | Where-Object { $_.category -cne 'local-capacity-ai' }).Count -eq 0) -and
        (@($richRecords | Where-Object {
            $_.scenario -notin @('4v2', '4v3', 'hard-ai-2v6')
        }).Count -eq 0) -and
        (@($richRecords | Where-Object { [int]$_.seed -le 0 }).Count -eq 0) -and
        (@($richRecords | Where-Object { $_.origin -cne 'native-fresh-runtime' }).Count -eq 0) -and
        (@($richRecords | Where-Object {
            $_.stress -and $_.scenario -ceq 'hard-ai-2v6'
        }).Count -eq 1) -and
        (@($richRecords | Where-Object {
            $_.stress -and $_.scenario -ceq '4v2'
        }).Count -eq 0) -and
        (@($richRecords | Where-Object {
            $_.stress -and $_.scenario -ceq 'hard-ai-2v6' -and
                $_.sourceSha256 -ceq $hard2v6Record.sourceSha256 -and
                $_.destinationSha256 -ceq $hard2v6Record.destinationSha256
        }).Count -eq 1) -and
        (@($richRecords | Where-Object {
            $_.stress -and $_.actualAi -eq 8 -and $_.actualTeams -ceq '2v6'
        }).Count -eq 1) -and
        $provenanceDocument.corpusManifest.sha256 -ceq (Get-TestSha256 $conversionManifest.path) -and
        $provenanceDocument.validationReceipt.sha256 -ceq (Get-TestSha256 $conversionReceiptPath) -and
        $provenanceDocument.artifactIndex.sha256 -ceq (Get-TestSha256 $artifactIndex.path) -and
        $provenanceDocument.validationResults.sha256 -ceq (Get-TestSha256 $conversionResults)) `
        'corpus conversion emits exactly ten unique native fixtures, one hard-ai-2v6 stress fixture, rich provenance, and transitive bindings'

    # The validation-results join must bind the replay completion itself, not
    # only its topology.  These fields are emitted by the production completion
    # writer and retained verbatim in aiEvidence.fields.
    $exporterModule = Get-Module -Name Stage5ReplayCorpusExporter |
        Select-Object -First 1
    $validationResultsBaseline = Get-Content -LiteralPath $conversionResults `
        -Raw | ConvertFrom-Json
    foreach ($fieldName in @('run_nonce', 'replay_epoch', 'replay_sha256',
            'replay_retained')) {
        $missingCompletionField = $validationResultsBaseline |
            ConvertTo-Json -Depth 24 | ConvertFrom-Json
        $missingCompletionField[0].aiEvidence.fields.PSObject.Properties.Remove(
            $fieldName)
        Assert-Throws {
            & $exporterModule {
                param([object[]]$Records, [object]$ResultsDocument)
                Assert-Stage5ExporterValidationResults $Records $ResultsDocument
            } $conversionRecords.ToArray() $missingCompletionField
        } "missing required property '$fieldName'" `
            "validation-results rejects stripped AI completion field $fieldName"
    }

    $completionMismatchCases = @(
        [pscustomobject]@{
            name = 'run nonce'
            edit = { param($fields) $fields.run_nonce = '00AF-FFFF-00000001' }
        }
        [pscustomobject]@{
            name = 'replay epoch'
            edit = { param($fields) $fields.replay_epoch = '2' }
        }
        [pscustomobject]@{
            name = 'replay hash'
            edit = { param($fields) $fields.replay_sha256 = ('F' * 64) }
        }
        [pscustomobject]@{
            name = 'retained replay path'
            edit = { param($fields) $fields.replay_retained = [string]$conversionRecords[1].sourcePath }
        }
    )
    foreach ($mismatch in $completionMismatchCases) {
        $mismatchedCompletion = $validationResultsBaseline |
            ConvertTo-Json -Depth 24 | ConvertFrom-Json
        $editCompletion = $mismatch.edit
        & $editCompletion $mismatchedCompletion[0].aiEvidence.fields
        Assert-Throws {
            & $exporterModule {
                param([object[]]$Records, [object]$ResultsDocument)
                Assert-Stage5ExporterValidationResults $Records $ResultsDocument
            } $conversionRecords.ToArray() $mismatchedCompletion
        } 'live-AI|replay-hash provenance' `
            "validation-results rejects mismatched AI completion $($mismatch.name)"
    }

    $detachedCompletion = $validationResultsBaseline |
        ConvertTo-Json -Depth 24 | ConvertFrom-Json
    $detachedRecord = $conversionRecords[1]
    $detachedCompletion[0].aiEvidence.fields.run_nonce =
        [string]$detachedRecord.runNonce
    $detachedCompletion[0].aiEvidence.fields.replay_epoch =
        [string]$detachedRecord.replayEpoch
    $detachedCompletion[0].aiEvidence.fields.replay_sha256 =
        [string]$detachedRecord.replaySha256
    $detachedCompletion[0].aiEvidence.fields.replay_retained =
        [string]$detachedRecord.sourcePath
    Assert-Throws {
        & $exporterModule {
            param([object[]]$Records, [object]$ResultsDocument)
            Assert-Stage5ExporterValidationResults $Records $ResultsDocument
        } $conversionRecords.ToArray() $detachedCompletion
    } 'live-AI|replay-hash provenance' `
        'validation-results rejects a completion detached from another replay run'

    # The production receipt writer always emits both root bindings.  Removing
    # the nested binding must fail even when the manifest hash is coherently
    # updated, so readers cannot silently weaken the writer's provenance shape.
    $receiptBeforeNestedRootMutation = [IO.File]::ReadAllBytes($conversionReceiptPath)
    $manifestBeforeNestedRootMutation = [IO.File]::ReadAllBytes($conversionManifest.path)
    try {
        $receiptWithoutNestedRoot = Get-Content -LiteralPath `
            $conversionReceiptPath -Raw | ConvertFrom-Json
        $receiptWithoutNestedRoot.corpusExport.PSObject.Properties.Remove(
            'corpusExportRoot')
        [IO.File]::WriteAllText($conversionReceiptPath,
            ($receiptWithoutNestedRoot | ConvertTo-Json -Depth 24))
        $manifestWithMutatedReceiptBinding = Get-Content -LiteralPath `
            $conversionManifest.path -Raw | ConvertFrom-Json
        $manifestWithMutatedReceiptBinding.validationReceipt.sha256 =
            Get-TestSha256 $conversionReceiptPath
        [IO.File]::WriteAllText($conversionManifest.path,
            ($manifestWithMutatedReceiptBinding | ConvertTo-Json -Depth 24))
        Assert-Throws {
            & $exporterModule {
                param([string]$ManifestPath)
                Read-Stage5FreshReplayCorpusBundle $ManifestPath
            } $conversionManifest.path
        } "missing required property 'corpusExportRoot'" `
            'corpus reader rejects a stripped nested receipt corpusExportRoot'
    }
    finally {
        [IO.File]::WriteAllBytes($conversionReceiptPath,
            $receiptBeforeNestedRootMutation)
        [IO.File]::WriteAllBytes($conversionManifest.path,
            $manifestBeforeNestedRootMutation)
    }

    # Record length is part of the immutable source/destination provenance,
    # not an optional annotation. Exercise the private validator directly so
    # each malformed value is rejected before any manifest can be published.
    $exporterModule = Get-Module -Name Stage5ReplayCorpusExporter |
        Select-Object -First 1
    $lengthBaselineRecord = $conversionRecords[0]
    $missingLengthRecord = Copy-TestRecord $lengthBaselineRecord
    $missingLengthRecord.PSObject.Properties.Remove('length')
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $missingLengthRecord $conversionTaskRoot $conversionCorpusRoot
    } 'length|Length' 'record validation rejects a missing length'

    $nonIntegerLengthRecord = Copy-TestRecord $lengthBaselineRecord
    Set-TestRecordProperty $nonIntegerLengthRecord 'length' 'not-an-integer'
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $nonIntegerLengthRecord $conversionTaskRoot $conversionCorpusRoot
    } 'length|Length' 'record validation rejects a non-integer length'

    $negativeLengthRecord = Copy-TestRecord $lengthBaselineRecord
    Set-TestRecordProperty $negativeLengthRecord 'length' ([Int64]-1)
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $negativeLengthRecord $conversionTaskRoot $conversionCorpusRoot
    } 'length|Length' 'record validation rejects a negative length'

    $wrongDestinationLengthRecord = Copy-TestRecord $lengthBaselineRecord
    Set-TestRecordProperty $wrongDestinationLengthRecord 'length' `
        ([Int64]$lengthBaselineRecord.length + 1)
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $wrongDestinationLengthRecord $conversionTaskRoot $conversionCorpusRoot
    } 'length|Length' 'record validation rejects a destination length mismatch'

    $sourceLengthMismatchPath = Join-Path $conversionSourceDirectory `
        'source-length-mismatch.rep'
    $sourceBytes = [IO.File]::ReadAllBytes([string]$lengthBaselineRecord.sourcePath)
    $sourceMismatchBytes = New-Object byte[] ($sourceBytes.Length + 1)
    [Array]::Copy($sourceBytes, $sourceMismatchBytes, $sourceBytes.Length)
    $sourceMismatchBytes[$sourceBytes.Length] = [byte]127
    [IO.File]::WriteAllBytes($sourceLengthMismatchPath, $sourceMismatchBytes)
    $wrongSourceLengthRecord = Copy-TestRecord $lengthBaselineRecord
    Set-TestRecordProperty $wrongSourceLengthRecord 'sourcePath' $sourceLengthMismatchPath
    Set-TestRecordProperty $wrongSourceLengthRecord 'sourceSha256' `
        (Get-TestSha256 $sourceLengthMismatchPath)
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $wrongSourceLengthRecord $conversionTaskRoot $conversionCorpusRoot
    } 'length|Length|SHA|source/destination' `
        'record validation rejects a present-source provenance mismatch'

    $recordContractBaseline = Copy-TestRecord $lengthBaselineRecord
    Set-TestRecordProperty $recordContractBaseline 'sequence' ([int]1)
    Set-TestRecordProperty $recordContractBaseline 'repeat' ([int]1)
    $relabeledOrdinaryRecord = Copy-TestRecord $recordContractBaseline
    Set-TestRecordProperty $relabeledOrdinaryRecord 'scenario' 'hard-ai-2v6'
    Assert-Throws {
        & $exporterModule {
            param($candidate, $candidateTaskRoot, $candidateCorpusRoot)
            Assert-Stage5ExporterRecord $candidate $candidateTaskRoot `
                $candidateCorpusRoot 'ZeroHour' ('A' * 64)
        } $relabeledOrdinaryRecord $conversionTaskRoot $conversionCorpusRoot
    } 'actual|team|scenario|2v6' `
        'record validation rejects an ordinary replay relabeled as hard-ai-2v6'

    $persistedIntegerBaseline = ($recordContractBaseline | ConvertTo-Json -Depth 16 |
        ConvertFrom-Json)
    $fractionalPersistedFields = @(
        [pscustomobject]@{ name = 'seed'; value = [double]1729.5 }
        [pscustomobject]@{ name = 'sequence'; value = [double]1.5 }
        [pscustomobject]@{ name = 'repeat'; value = [double]1.5 }
        [pscustomobject]@{ name = 'actualAi'; value = [double]6.0 }
        [pscustomobject]@{ name = 'length'; value = [double]$recordContractBaseline.length }
        [pscustomobject]@{ name = 'containerSchemaVersion'; value = [double]2.0 }
        [pscustomobject]@{ name = 'containerEngineEpoch'; value = [double]1.0 }
        [pscustomobject]@{ name = 'skirmishAiReplayEpoch'; value = [double]3.0 }
        [pscustomobject]@{ name = 'replayEpoch'; value = [double]3.0 }
        [pscustomobject]@{ name = 'replayQualificationVersion'; value = [double]2.0 }
    )
    foreach ($mutation in $fractionalPersistedFields) {
        $candidate = Copy-TestRecord $persistedIntegerBaseline
        Set-TestRecordProperty $candidate $mutation.name $mutation.value
        Assert-Throws {
            & $exporterModule {
                param($candidateRecord, $candidateTaskRoot, $candidateCorpusRoot)
                Assert-Stage5ExporterRecord $candidateRecord $candidateTaskRoot `
                    $candidateCorpusRoot 'ZeroHour' ('A' * 64)
            } $candidate $conversionTaskRoot $conversionCorpusRoot
        } 'integer|range|fraction|seed|sequence|repeat|length|epoch|schema|actual|qualification' `
            "record validation rejects fractional persisted $($mutation.name)"
    }
    $outOfRangePersistedFields = @(
        [pscustomobject]@{ name = 'seed'; value = [int64]0 }
        [pscustomobject]@{ name = 'seed'; value = [int64]2147483648 }
        [pscustomobject]@{ name = 'sequence'; value = [int64]0 }
        [pscustomobject]@{ name = 'sequence'; value = [int64]2147483648 }
        [pscustomobject]@{ name = 'repeat'; value = [int64]0 }
        [pscustomobject]@{ name = 'repeat'; value = [int64]11 }
    )
    foreach ($mutation in $outOfRangePersistedFields) {
        $candidate = Copy-TestRecord $persistedIntegerBaseline
        Set-TestRecordProperty $candidate $mutation.name $mutation.value
        Assert-Throws {
            & $exporterModule {
                param($candidateRecord, $candidateTaskRoot, $candidateCorpusRoot)
                Assert-Stage5ExporterRecord $candidateRecord $candidateTaskRoot `
                    $candidateCorpusRoot 'ZeroHour' ('A' * 64)
            } $candidate $conversionTaskRoot $conversionCorpusRoot
        } 'integer|range|positive|between|seed|sequence|repeat' `
            "record validation rejects out-of-range persisted $($mutation.name)"
    }

    # Two hard-AI source hashes must not let the second hard record leak into
    # the nine ordinary fixture slots. Choose the two lexically-smallest
    # source hashes as hard records so the old first-nine selection exposes
    # that leak deterministically.
    $multiHardTaskRoot = Join-Path $testRoot 'multi-hard-conversion-task'
    $multiHardTaskRunRoot = Join-Path $multiHardTaskRoot 'validation-run'
    $multiHardProfileRoot = Join-Path $multiHardTaskRunRoot 'Documents\Profile Name'
    $multiHardSourceDirectory = Join-Path $multiHardProfileRoot 'Replays'
    $multiHardCorpusRoot = Join-Path $multiHardTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $multiHardSourceDirectory, $multiHardCorpusRoot -Force | Out-Null
    $multiHardCandidates = New-Object 'Collections.Generic.List[object]'
    for ($index = 0; $index -lt 15; ++$index) {
        $source = Join-Path $multiHardSourceDirectory `
            ('candidate-{0}.rep' -f $index)
        [IO.File]::WriteAllBytes($source,
            [IO.File]::ReadAllBytes([string]$conversionRecords[$index].destinationPath))
        $multiHardCandidates.Add([pscustomobject]@{
            source = $source
            hash = Get-TestSha256 $source
            seed = 2700 + $index
        }) | Out-Null
    }
    $hardHashes = @($multiHardCandidates | Sort-Object hash | Select-Object -First 5 |
        ForEach-Object { [string]$_.hash })
    Assert-True ($hardHashes.Count -eq 5) `
        'multi-hard fixture setup selected five distinct hard-AI source hashes.'
    $multiHardRecords = New-Object 'Collections.Generic.List[object]'
    $normalOrdinal = 0
    $hardOrdinal = 0
    foreach ($candidate in $multiHardCandidates) {
        $scenario = if ($hardHashes -contains [string]$candidate.hash) {
            'hard-ai-2v6'
        }
        elseif ($normalOrdinal -lt 5) {
            '4v2'
        }
        else {
            '4v3'
        }
        $seed = if ($scenario -ceq 'hard-ai-2v6') {
            2701 + $hardOrdinal++
        }
        else {
            2701 + ($normalOrdinal++ % 5)
        }
        $multiHardRecords.Add((Export-Stage5FreshReplayArtifact `
            -SourcePath $candidate.source -ExpectedSha256 $candidate.hash `
            -TaskRoot $multiHardTaskRoot -TaskRunRoot $multiHardTaskRunRoot `
            -ProfileRoot $multiHardProfileRoot -CorpusExportRoot $multiHardCorpusRoot `
            -Metadata (New-TestMetadata `
                ('00AF-000003-{0:D8}' -f $multiHardRecords.Count) $scenario `
                $seed 'local-capacity-ai'))) | Out-Null
    }
    $multiHardBundle = New-TestCorpusBundle `
        -TaskRoot $multiHardTaskRoot -CorpusRoot $multiHardCorpusRoot `
        -Records $multiHardRecords.ToArray()
    $multiHardFixturePath = Join-Path $multiHardCorpusRoot 'multi-hard-fixtures.json'
    $multiHardProvenancePath = Join-Path $multiHardCorpusRoot 'multi-hard-provenance.json'
    $multiHardConversion = Convert-Stage5FreshReplayCorpusManifestToFixtures `
        -CorpusManifestPath $multiHardBundle.manifest.path `
        -FixtureManifestPath $multiHardFixturePath `
        -ProvenancePath $multiHardProvenancePath -Executable 'generalszh.exe'
    $multiHardFixtureDocument = Get-Content -LiteralPath $multiHardFixturePath -Raw |
        ConvertFrom-Json
    $multiHardProvenanceDocument = Get-Content -LiteralPath $multiHardProvenancePath -Raw |
        ConvertFrom-Json
    $multiHardSelected = @($multiHardProvenanceDocument.fixtures)
    Assert-True ($multiHardConversion.fixtureCount -eq 10 -and
        $multiHardFixtureDocument.fixtures.Count -eq 10 -and
        (@($multiHardSelected | Where-Object {
            $_.stress -and $_.scenario -ceq 'hard-ai-2v6'
        }).Count -eq 1) -and
        (@($multiHardSelected | Where-Object {
            -not $_.stress -and $_.scenario -in @('4v2', '4v3')
        }).Count -eq 9) -and
        (@($multiHardSelected | Where-Object {
            $_.scenario -ceq 'hard-ai-2v6'
        }).Count -eq 1) -and
        (@($multiHardFixtureDocument.fixtures | ForEach-Object {
            $_.sha256
        } | Sort-Object -Unique).Count -eq 10)) `
        'fixture conversion excludes extra hard-ai-2v6 hashes from the nine ordinary slots'

    # With four hard hashes and only eight true ordinary hashes, conversion must
    # not satisfy the ordinary count by treating the second hard replay as
    # normal.
    $fewNormalTaskRoot = Join-Path $testRoot 'few-normal-conversion-task'
    $fewNormalTaskRunRoot = Join-Path $fewNormalTaskRoot 'validation-run'
    $fewNormalProfileRoot = Join-Path $fewNormalTaskRunRoot 'Documents\Profile Name'
    $fewNormalSourceDirectory = Join-Path $fewNormalProfileRoot 'Replays'
    $fewNormalCorpusRoot = Join-Path $fewNormalTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $fewNormalSourceDirectory, $fewNormalCorpusRoot -Force | Out-Null
    $fewNormalRecords = New-Object 'Collections.Generic.List[object]'
    $fewNormalSourceRecords = @(
        @($multiHardRecords.ToArray() | Where-Object {
            $_.scenario -ceq 'hard-ai-2v6'
        } | Select-Object -First 4)
        @($multiHardRecords.ToArray() | Where-Object {
            $_.scenario -ceq '4v2' -and $_.seed -le 2704
        })
        @($multiHardRecords.ToArray() | Where-Object {
            $_.scenario -ceq '4v3' -and $_.seed -le 2704
        })
    )
    foreach ($original in $fewNormalSourceRecords) {
        $source = Join-Path $fewNormalSourceDirectory `
            ('candidate-{0}.rep' -f $fewNormalRecords.Count)
        [IO.File]::WriteAllBytes($source,
            [IO.File]::ReadAllBytes([string]$original.destinationPath))
        $fewNormalRecords.Add((Export-Stage5FreshReplayArtifact `
            -SourcePath $source -ExpectedSha256 (Get-TestSha256 $source) `
            -TaskRoot $fewNormalTaskRoot -TaskRunRoot $fewNormalTaskRunRoot `
            -ProfileRoot $fewNormalProfileRoot -CorpusExportRoot $fewNormalCorpusRoot `
            -Metadata (New-TestMetadata `
                ('00AF-000004-{0:D8}' -f $fewNormalRecords.Count) `
                ([string]$original.scenario) ([int]$original.seed) `
                'local-capacity-ai'))) | Out-Null
    }
    $fewNormalBundle = New-TestCorpusBundle `
        -TaskRoot $fewNormalTaskRoot -CorpusRoot $fewNormalCorpusRoot `
        -Records $fewNormalRecords.ToArray()
    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $fewNormalBundle.manifest.path `
            -FixtureManifestPath (Join-Path $fewNormalCorpusRoot 'few-normal-fixtures.json') `
            -ProvenancePath (Join-Path $fewNormalCorpusRoot 'few-normal-provenance.json') `
            -Executable 'generalszh.exe'
    } 'nine non-stress unique replay' `
        'fixture conversion rejects fewer than nine true ordinary hashes'

    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $conversionManifest.path `
            -FixtureManifestPath (Join-Path $conversionCorpusRoot 'bad-title-fixtures.json') `
            -ProvenancePath (Join-Path $conversionCorpusRoot 'bad-title-provenance.json') `
            -Executable 'generalsv.exe'
    } 'does not belong to corpus title' 'fixture conversion enforces title-specific executable naming'
    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $conversionManifest.path `
            -FixtureManifestPath $fixtureManifestPath `
            -ProvenancePath $provenancePath -Executable 'generalszh.exe'
    } 'already exists|refusing overwrite' 'fixture conversion refuses output overwrite'
    $tamperedReceipt = Get-Content -LiteralPath $conversionReceiptPath -Raw
    [IO.File]::WriteAllText($conversionReceiptPath, '{"tampered":true}')
    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $conversionManifest.path `
            -FixtureManifestPath (Join-Path $conversionCorpusRoot 'tampered-fixtures.json') `
            -ProvenancePath (Join-Path $conversionCorpusRoot 'tampered-provenance.json') `
            -Executable 'generalszh.exe'
    } 'receipt.*SHA|binding' 'fixture conversion rejects a tampered final receipt'
    [IO.File]::WriteAllText($conversionReceiptPath, $tamperedReceipt)
    Remove-Item -LiteralPath $conversionTaskRunRoot -Recurse -Force
    $postCleanupFixtureManifestPath = Join-Path $conversionCorpusRoot 'post-cleanup-fixtures.json'
    $postCleanupProvenancePath = Join-Path $conversionCorpusRoot 'post-cleanup-provenance.json'
    $postCleanupConversion = Convert-Stage5FreshReplayCorpusManifestToFixtures `
        -CorpusManifestPath $conversionManifest.path `
        -FixtureManifestPath $postCleanupFixtureManifestPath `
        -ProvenancePath $postCleanupProvenancePath -Executable 'generalszh.exe'
    $postCleanupFixtureDocument = Get-Content -LiteralPath $postCleanupFixtureManifestPath -Raw |
        ConvertFrom-Json
    Assert-True ($postCleanupConversion.fixtureCount -eq 10 -and
        $postCleanupFixtureDocument.fixtures.Count -eq 10) `
        'fixture conversion remains usable after the ephemeral validation run is removed'

    # A hard-AI source cannot become the standard stress fixture merely by
    # relabeling its metadata as the existing 4v2 live-AI scenario.  Re-export
    # the same real fixture bytes under that relabel and require conversion to
    # fail closed for the missing truthful hard-ai-2v6 record.
    $relabeledTaskRoot = Join-Path $testRoot 'relabeled-4v2-conversion-task'
    $relabeledTaskRunRoot = Join-Path $relabeledTaskRoot 'validation-run'
    $relabeledProfileRoot = Join-Path $relabeledTaskRunRoot 'Documents\Profile Name'
    $relabeledSourceDirectory = Join-Path $relabeledProfileRoot 'Replays'
    $relabeledCorpusRoot = Join-Path $relabeledTaskRoot 'fresh-native-corpus'
    New-Item -ItemType Directory -Path $relabeledSourceDirectory, $relabeledCorpusRoot -Force | Out-Null
    $relabeledRecords = New-Object 'Collections.Generic.List[object]'
    $relabeledSourceRecords = @($conversionRecords.ToArray() | Select-Object -First 10)
    for ($index = 0; $index -lt $relabeledSourceRecords.Count; ++$index) {
        $original = $relabeledSourceRecords[$index]
        $scenario = if ($original.scenario -ceq 'hard-ai-2v6') { '4v2' } else {
            [string]$original.scenario
        }
        $source = Join-Path $relabeledSourceDirectory "record-$($index + 1).rep"
        [IO.File]::WriteAllBytes($source,
            [IO.File]::ReadAllBytes([string]$original.destinationPath))
        $sourceHash = Get-TestSha256 $source
        $relabeledRecords.Add((Export-Stage5FreshReplayArtifact `
            -SourcePath $source -ExpectedSha256 $sourceHash `
            -TaskRoot $relabeledTaskRoot -TaskRunRoot $relabeledTaskRunRoot `
            -ProfileRoot $relabeledProfileRoot -CorpusExportRoot $relabeledCorpusRoot `
            -Metadata (New-TestMetadata `
                ('00AF-000002-{0:D8}' -f ($index + 1)) $scenario `
                ([int]$original.seed) 'local-capacity-ai'))) | Out-Null
    }
    Complete-TestCorpusRecords $relabeledRecords.ToArray()
    $relabeledValidationRecords = New-Object 'Collections.Generic.List[object]'
    for ($index = 0; $index -lt $relabeledRecords.Count; ++$index) {
        $validationRecord = Copy-TestRecord $relabeledRecords[$index]
        $original = $relabeledSourceRecords[$index]
        Set-TestRecordProperty $validationRecord 'scenario' ([string]$original.scenario)
        Set-TestRecordProperty $validationRecord 'actualAi' ([int]$original.actualAi)
        Set-TestRecordProperty $validationRecord 'actualTeams' ([string]$original.actualTeams)
        $relabeledValidationRecords.Add($validationRecord) | Out-Null
    }
    $relabeledResults = Join-Path $relabeledTaskRoot 'validation-results.json'
    Write-TestValidationResults $relabeledResults $relabeledValidationRecords.ToArray()
    $relabeledIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $relabeledTaskRoot -CorpusExportRoot $relabeledCorpusRoot `
        -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
        -Records $relabeledRecords.ToArray() -ValidationResultsPath $relabeledResults
    $relabeledReceiptPath = Join-Path $relabeledTaskRoot 'relabeled-4v2-receipt.json'
    $relabeledReceipt = [ordered]@{
        schemaVersion = 1
        receiptKind = 'stage5-local-capacity-receipt'
        status = 'passed-non-acceptance'
        notAnAcceptanceEnvelope = $true
        finalAcceptanceEligible = $false
        validationMode = 'LocalCapacity'
        capacityMode = 'LocalCapacity'
        corpusExportRequested = $true
        corpusExportRoot = $relabeledCorpusRoot
        resultsSha256 = Get-TestSha256 $relabeledResults
        corpusExport = [ordered]@{
            status = 'passed'
            corpusExportRoot = $relabeledCorpusRoot
            artifactIndexPath = $relabeledIndex.path
            artifactIndexSha256 = $relabeledIndex.sha256
            recordCount = $relabeledRecords.Count
            records = $relabeledRecords.ToArray()
        }
    }
    [IO.File]::WriteAllText($relabeledReceiptPath,
        ($relabeledReceipt | ConvertTo-Json -Depth 16))
    $relabeledManifest = Write-Stage5FreshReplayCorpusManifest `
        -TaskRoot $relabeledTaskRoot -CorpusExportRoot $relabeledCorpusRoot `
        -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
        -Records $relabeledRecords.ToArray() -ValidationResultsPath $relabeledResults `
        -ValidationReceiptPath $relabeledReceiptPath
    Assert-Throws {
        Convert-Stage5FreshReplayCorpusManifestToFixtures `
            -CorpusManifestPath $relabeledManifest.path `
            -FixtureManifestPath (Join-Path $relabeledCorpusRoot 'relabeled-fixtures.json') `
            -ProvenancePath (Join-Path $relabeledCorpusRoot 'relabeled-provenance.json') `
            -Executable 'generalszh.exe'
    } 'validation-results|scenario|provenance|2v6|stress' `
        'fixture conversion rejects corpus metadata that disagrees with its bound live-AI result'

    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $retention.replayRetained `
            -ExpectedSha256 $retention.replaySha256 `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot $corpusRoot `
            -Metadata (New-TestMetadata $retention.runNonce $retention.scenario $retention.seed)
    } 'refusing overwrite' 'existing destination'

    $filesystemRoot = [IO.Path]::GetPathRoot($taskRoot)
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $retention.replayRetained `
            -ExpectedSha256 $retention.replaySha256 `
            -TaskRoot $filesystemRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'root-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000005')
    } 'below a filesystem root' 'filesystem-root task rejection'

    $outsideSource = Join-Path $taskRunRoot 'outside.rep'
    [IO.File]::WriteAllBytes($outsideSource, (New-TestReplayBytes))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $outsideSource `
            -ExpectedSha256 (Get-TestSha256 $outsideSource) `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'outside-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000002')
    } 'escapes|below its containing directory' 'source profile containment'

    $legacySource = Join-Path $sourceDirectory 'legacy.rep'
    $legacyBytes = New-Object byte[] 64
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('GENREP legacy'), $legacyBytes, 13)
    [IO.File]::WriteAllBytes($legacySource, $legacyBytes)
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $legacySource `
            -ExpectedSha256 (Get-TestSha256 $legacySource) `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'legacy-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000003')
    } 'does not begin with RPL3' 'legacy replay rejection'

    $epoch2Source = Join-Path $sourceDirectory 'epoch2.rep'
    [IO.File]::WriteAllBytes($epoch2Source, (New-TestReplayBytes ' [SkirmishAIEpoch=2]'))
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $epoch2Source `
            -ExpectedSha256 (Get-TestSha256 $epoch2Source) `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'epoch2-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000004')
    } 'epoch-3 marker' 'epoch-2 replay rejection'

    $reparseProfile = Join-Path $taskRunRoot 'ReparseProfile'
    $reparseCreated = $false
    try {
        New-Item -ItemType SymbolicLink -Path $reparseProfile -Target $profileRoot -ErrorAction Stop | Out-Null
        $reparseCreated = $true
    }
    catch {
        # Symbolic-link creation can be disabled by the host policy.  The
        # production path checker is still covered by its ordinary path tests.
    }
    if ($reparseCreated -and (Test-Path -LiteralPath $reparseProfile)) {
        Assert-Throws {
            Export-Stage5FreshReplayArtifact `
                -SourcePath (Join-Path $reparseProfile 'Replays\SkirmishAI.rep') `
                -ExpectedSha256 $retention.replaySha256 `
                -TaskRoot $taskRoot `
                -TaskRunRoot $taskRunRoot `
                -ProfileRoot $reparseProfile `
                -CorpusExportRoot (Join-Path $taskRoot 'reparse-corpus') `
                -Metadata (New-TestMetadata '00AA-000006-00000006')
        } 'reparse point' 'reparse profile rejection'
    }

    # Every file-consuming path uses one identity-bound open: its canonical
    # handle path, file ID, single-link count, fixed extent, EOF, and ancestor
    # directory handles stay valid through hashing/parsing/copying.
    $identitySnapshot = & $exporterModule {
        param([string]$ReplayPath)
        Get-Stage5ExporterReplaySnapshot $ReplayPath 'ZeroHour' `
            'identity-bound replay snapshot'
    } $sourcePath
    Assert-True ($identitySnapshot.linkCount -eq 1 -and
        -not [string]::IsNullOrWhiteSpace([string]$identitySnapshot.fileId) -and
        [String]::Equals([string]$identitySnapshot.canonicalPath,
            [IO.Path]::GetFullPath($sourcePath),
            [StringComparison]::OrdinalIgnoreCase)) `
        'replay snapshot does not expose its opened-handle canonical identity.'

    $oversizedReplay = Join-Path $sourceDirectory 'oversized-sparse.rep'
    $oversizedReplayStream = [IO.File]::Open($oversizedReplay,
        [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $oversizedReplayStream.SetLength([Int64](256 * 1024 * 1024) + 1) }
    finally { $oversizedReplayStream.Dispose() }
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $oversizedReplay `
            -ExpectedSha256 ('0' * 64) `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'oversized-replay-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000007')
    } '268435456|256 MiB|allowed.*byte range' `
        'oversized sparse replay rejection before hashing or copying'

    $oversizedJson = Join-Path $taskRoot 'oversized-sparse.json'
    $oversizedJsonStream = [IO.File]::Open($oversizedJson,
        [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $oversizedJsonStream.SetLength([Int64](64 * 1024 * 1024) + 1) }
    finally { $oversizedJsonStream.Dispose() }
    Assert-Throws {
        & $exporterModule {
            param([string]$JsonPath)
            Get-Stage5ExporterJsonSnapshot $JsonPath 'oversized JSON snapshot'
        } $oversizedJson
    } '67108864|64 MiB|allowed.*byte range' `
        'oversized sparse JSON rejection before allocation or parsing'

    $hardLinkReplay = Join-Path $sourceDirectory 'hard-linked-source.rep'
    New-Item -ItemType HardLink -Path $hardLinkReplay -Target $sourcePath `
        -ErrorAction Stop | Out-Null
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath $hardLinkReplay `
            -ExpectedSha256 $sourceSha256 `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $profileRoot `
            -CorpusExportRoot (Join-Path $taskRoot 'hardlink-replay-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000008')
    } 'hard links|exactly one' 'hard-linked replay source rejection'
    Remove-Item -LiteralPath $hardLinkReplay -Force

    $jsonIdentityPath = Join-Path $taskRoot 'identity-bound.json'
    [IO.File]::WriteAllText($jsonIdentityPath, '{"status":"passed"}')
    $hardLinkJson = Join-Path $taskRoot 'hard-linked.json'
    New-Item -ItemType HardLink -Path $hardLinkJson -Target $jsonIdentityPath `
        -ErrorAction Stop | Out-Null
    Assert-Throws {
        & $exporterModule {
            param([string]$JsonPath)
            Get-Stage5ExporterJsonSnapshot $JsonPath 'hard-linked JSON snapshot'
        } $hardLinkJson
    } 'hard links|exactly one' 'hard-linked JSON source rejection'
    Remove-Item -LiteralPath $hardLinkJson -Force

    $junctionProfile = Join-Path $taskRunRoot 'JunctionProfile'
    New-Item -ItemType Junction -Path $junctionProfile -Target $profileRoot `
        -ErrorAction Stop | Out-Null
    Assert-Throws {
        Export-Stage5FreshReplayArtifact `
            -SourcePath (Join-Path $junctionProfile 'Replays\SkirmishAI-4v2-1729-00AA-000006.rep') `
            -ExpectedSha256 $sourceSha256 `
            -TaskRoot $taskRoot `
            -TaskRunRoot $taskRunRoot `
            -ProfileRoot $junctionProfile `
            -CorpusExportRoot (Join-Path $taskRoot 'junction-corpus') `
            -Metadata (New-TestMetadata '00AA-000006-00000009')
    } 'reparse point|redirected' 'junction ancestor rejection'
    Remove-TestOwnedJunction $junctionProfile $taskRunRoot

    $junctionJsonTarget = Join-Path $taskRoot 'junction-json-target'
    [void](New-Item -ItemType Directory -Path $junctionJsonTarget)
    [IO.File]::WriteAllText((Join-Path $junctionJsonTarget 'payload.json'),
        '{"status":"passed"}')
    $junctionJson = Join-Path $taskRoot 'JunctionJson'
    New-Item -ItemType Junction -Path $junctionJson -Target $junctionJsonTarget `
        -ErrorAction Stop | Out-Null
    Assert-Throws {
        & $exporterModule {
            param([string]$JsonPath)
            Get-Stage5ExporterJsonSnapshot $JsonPath `
                'junction-ancestor JSON snapshot'
        } (Join-Path $junctionJson 'payload.json')
    } 'reparse point|redirected' 'opened-handle junction ancestor rejection'
    Remove-TestOwnedJunction $junctionJson $taskRoot

    # A same-byte replacement at the temporary replay name must not be promoted.
    # Failure cleanup follows the held file object and never deletes the injected
    # replacement merely because it occupies the expected pathname.
    $replayRaceCorpusRoot = Join-Path $taskRoot 'replay-identity-race-corpus'
    $replayRaceBytes = [IO.File]::ReadAllBytes($sourcePath)
    $replayRaceState = [ordered]@{
        injectedPath = $null
        ownedMovedPath = $null
    }
    $replayRaceObserver = {
        param([string]$Stage, [string]$Kind, [string]$TemporaryPath,
            [string]$DestinationPath)
        if ($Stage -ceq 'before-commit' -and $Kind -ceq 'replay') {
            $replayRaceState.ownedMovedPath = "$TemporaryPath.owned-moved"
            $replayRaceState.injectedPath = $TemporaryPath
            [IO.File]::Move($TemporaryPath, $replayRaceState.ownedMovedPath)
            [IO.File]::WriteAllBytes($TemporaryPath, $replayRaceBytes)
        }
    }.GetNewClosure()
    try {
        Set-TestExporterCommitObserver $replayRaceObserver
        Assert-Throws {
            Export-Stage5FreshReplayArtifact `
                -SourcePath $sourcePath `
                -ExpectedSha256 $sourceSha256 `
                -TaskRoot $taskRoot `
                -TaskRunRoot $taskRunRoot `
                -ProfileRoot $profileRoot `
                -CorpusExportRoot $replayRaceCorpusRoot `
                -Metadata (New-TestMetadata '00AA-000006-00000010')
        } 'changed identity.*canonical path' `
            'same-byte replay temporary identity substitution'
    }
    finally { Set-TestExporterCommitObserver $null }
    Assert-True (-not (Test-Path -LiteralPath $replayRaceState.ownedMovedPath)) `
        'replay failure cleanup did not delete the exact held temporary object.'
    Assert-True (Test-Path -LiteralPath $replayRaceState.injectedPath -PathType Leaf) `
        'replay failure cleanup deleted an unowned same-byte injected path.'
    [IO.File]::Delete($replayRaceState.injectedPath)

    # The same identity rule applies after an atomic JSON rename.  If the
    # committed object is moved away and an identical file is injected at the
    # destination, validation rejects it and handle-owned cleanup deletes only
    # the moved original.
    $jsonRacePath = Join-Path $taskRoot 'json-identity-race.json'
    $jsonRaceDocument = [ordered]@{
        schemaVersion = 1
        kind = 'stage5-exporter-identity-race-test'
    }
    $jsonRaceBytes = [Text.Encoding]::UTF8.GetBytes(
        ($jsonRaceDocument | ConvertTo-Json -Depth 24))
    $jsonRaceState = [ordered]@{ ownedMovedPath = $null }
    $jsonRaceObserver = {
        param([string]$Stage, [string]$Kind, [string]$TemporaryPath,
            [string]$DestinationPath)
        if ($Stage -ceq 'after-commit' -and $Kind -ceq 'json') {
            $jsonRaceState.ownedMovedPath = "$DestinationPath.owned-moved"
            [IO.File]::Move($DestinationPath, $jsonRaceState.ownedMovedPath)
            [IO.File]::WriteAllBytes($DestinationPath, $jsonRaceBytes)
        }
    }.GetNewClosure()
    try {
        Set-TestExporterCommitObserver $jsonRaceObserver
        Assert-Throws {
            & $exporterModule {
                param([string]$JsonPath, [object]$Document,
                    [string]$BaseDirectory)
                Write-Stage5ExporterJsonAtomically $JsonPath $Document `
                    $BaseDirectory 'JSON identity-race output'
            } $jsonRacePath $jsonRaceDocument $taskRoot
        } 'commit changed file identity.*canonical destination' `
            'same-byte JSON post-commit identity substitution'
    }
    finally { Set-TestExporterCommitObserver $null }
    Assert-True (-not (Test-Path -LiteralPath $jsonRaceState.ownedMovedPath)) `
        'JSON post-commit cleanup did not delete the exact held object.'
    Assert-True (Test-Path -LiteralPath $jsonRacePath -PathType Leaf) `
        'JSON post-commit cleanup deleted the unowned injected destination.'
    Assert-True ((Get-TestSha256 $jsonRacePath) -ceq
        (Get-TestSha256FromBytes $jsonRaceBytes)) `
        'JSON same-byte substitution fixture does not match the admitted bytes.'
    [IO.File]::Delete($jsonRacePath)

    Assert-Throws {
        Write-Stage5FreshReplayCorpusManifest `
            -TaskRoot $taskRoot `
            -CorpusExportRoot $corpusRoot `
            -Title 'ZeroHour' `
            -ExecutableSha256 ('A' * 64) `
            -Records @($record) `
            -ValidationResultsPath $validationResults
    } 'already exists|refusing overwrite' 'existing manifest'

    Write-Output 'Stage 5 replay corpus exporter focused tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
