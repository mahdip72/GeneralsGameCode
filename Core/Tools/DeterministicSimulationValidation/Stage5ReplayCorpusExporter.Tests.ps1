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
        [string]$Category = 'native-fresh-ai')
    return [ordered]@{
        title = 'ZeroHour'
        category = $Category
        scenario = $Scenario
        seed = $Seed
        runNonce = $RunNonce
        executableSha256 = ('A' * 64)
        origin = 'native-fresh-runtime'
    }
}

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
    $retention = Get-Stage5ReplayCompletionFields $completion 1729 '4v2'
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
