[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ScratchRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'Stage5ReplayCorpusExporter.psm1') -Force

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
            $keys -contains 'skirmishAiReplayEpoch')
    }, $true)
    Assert-True ($null -ne $recordAst) `
        'LocalCapacity runner reconstructs a native replay record.'
    if ($null -eq $recordAst) { return }

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
        [string]$Category = 'native-fresh-ai', [string]$Title = 'ZeroHour')
    return [ordered]@{
        title = $Title
        category = $Category
        scenario = $Scenario
        seed = $Seed
        runNonce = $RunNonce
        executableSha256 = ('A' * 64)
        origin = 'native-fresh-runtime'
    }
}

Assert-LocalCapacityRunnerPreservesReplayQualification

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
    for ($index = 1; $index -le 11; ++$index) {
        $scenario = if ($index -eq 1) { '4v2' } else { '4v3' }
        $seed = 1700 + $index
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
    $conversionResults = Join-Path $conversionTaskRoot 'validation-results.json'
    [IO.File]::WriteAllText($conversionResults, '{"status":"passed"}')
    $artifactIndex = Write-Stage5FreshReplayArtifactIndex `
        -TaskRoot $conversionTaskRoot -CorpusExportRoot $conversionCorpusRoot `
        -Title 'ZeroHour' -ExecutableSha256 ('A' * 64) `
        -Records $conversionRecords.ToArray() -ValidationResultsPath $conversionResults
    $conversionReceiptPath = Join-Path $conversionTaskRoot 'local-capacity-receipt.json'
    $conversionReceipt = [ordered]@{
        schemaVersion = 1
        receiptKind = 'stage5-local-capacity-receipt'
        status = 'passed-non-acceptance'
        notAnAcceptanceEnvelope = $true
        finalAcceptanceEligible = $false
        corpusExport = [ordered]@{
            status = 'passed'
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
        -ValidationReceiptPath $conversionReceiptPath
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
    Assert-True ($conversion.fixtureCount -eq 10 -and
        $fixtureDocument.fixtures.Count -eq 10 -and
        $stressFixtures.Count -eq 1 -and
        $richRecords.Count -eq 10 -and
        (@($selectedShas | Sort-Object -Unique).Count -eq 10) -and
        $richRecords[0].category -ceq 'local-capacity-ai' -and
        (@($richRecords | Where-Object { $_.category -cne 'local-capacity-ai' }).Count -eq 0) -and
        (@($richRecords | Where-Object { $_.scenario -notin @('4v2', '4v3') }).Count -eq 0) -and
        (@($richRecords | Where-Object { [int]$_.seed -le 0 }).Count -eq 0) -and
        (@($richRecords | Where-Object { $_.origin -cne 'native-fresh-runtime' }).Count -eq 0) -and
        (@($richRecords | Where-Object { $_.stress -and $_.scenario -ceq '4v2' }).Count -eq 1) -and
        $provenanceDocument.corpusManifest.sha256 -ceq (Get-TestSha256 $conversionManifest.path) -and
        $provenanceDocument.validationReceipt.sha256 -ceq (Get-TestSha256 $conversionReceiptPath) -and
        $provenanceDocument.artifactIndex.sha256 -ceq (Get-TestSha256 $artifactIndex.path) -and
        $provenanceDocument.validationResults.sha256 -ceq (Get-TestSha256 $conversionResults)) `
        'corpus conversion emits exactly ten unique native fixtures, one 4v2 stress fixture, rich provenance, and transitive bindings'
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
