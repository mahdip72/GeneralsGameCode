[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GeneralsExecutable,
    [Parameter(Mandatory = $true)][string]$ZeroHourExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactSetManifestPath,
    [Parameter(Mandatory = $true)][string]$SourceCommit,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(60, 1800)][int]$PeerTimeoutSeconds = 300
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'DeterministicSimulationEvidence.psm1') -Force

$PostKillWaitMilliseconds = 5000

function Get-UpperSha256 {
    param([string]$Path)
    return Get-Stage5FileSha256 $Path
}

function Resolve-BoundedArtifactPath {
    param([string]$Manifest, [string]$RelativePath)
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath)) {
        throw "Artifact path must be a nonempty manifest-relative path: $RelativePath"
    }
    $root = [IO.Path]::GetFullPath((Split-Path -Parent $Manifest))
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    $prefix = $root.TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Artifact path escapes its manifest root: $RelativePath"
    }
    return $candidate
}

function Get-RelativeEvidencePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Evidence path escapes its runner root: $pathFull"
    }
    return $pathFull.Substring($rootFull.Length)
}

function Read-AndValidateArtifactSet {
    param([string]$Manifest, [string]$ExpectedSourceCommit)
    $full = [IO.Path]::GetFullPath($Manifest)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Artifact-set manifest was not found: $full"
    }
    $document = ConvertFrom-Stage5JsonDictionary $full
    Assert-Stage5JsonShape $document @('schemaVersion', 'sourceCommit',
        'productSet', 'architecture', 'artifacts') 'Artifact set manifest'
    if ((Get-Stage5JsonValue $document 'schemaVersion' 'Artifact set manifest') -ne 1 -or
        (Get-Stage5JsonValue $document 'sourceCommit' 'Artifact set manifest') -cne
            $ExpectedSourceCommit -or
        (Get-Stage5JsonValue $document 'architecture' 'Artifact set manifest') -cne 'x64') {
        throw 'Artifact-set identity does not match the requested native x64 source revision.'
    }
    $productSet = Get-Stage5JsonValue $document 'productSet' 'Artifact set manifest'
    if ($productSet -isnot [Array] -or $productSet.Count -ne 2 -or
        -not ($productSet -ccontains 'Generals') -or
        -not ($productSet -ccontains 'ZeroHour')) {
        throw 'Artifact set must contain exactly Generals and ZeroHour.'
    }
    $requiredRoles = @('generals-executable', 'generals-launcher',
        'generals-launcher-config', 'zerohour-executable', 'zerohour-launcher',
        'zerohour-launcher-config')
    $artifacts = Get-Stage5JsonValue $document 'artifacts' 'Artifact set manifest'
    if ($artifacts -isnot [Array] -or $artifacts.Count -ne $requiredRoles.Count) {
        throw 'Artifact set must contain exactly six installed product artifacts.'
    }
    $resolved = @{}
    foreach ($entry in $artifacts) {
        Assert-Stage5JsonShape $entry @('role', 'path', 'sha256') 'Artifact entry'
        $role = [string](Get-Stage5JsonValue $entry 'role' 'Artifact entry')
        if (-not ($requiredRoles -ccontains $role) -or $resolved.ContainsKey($role)) {
            throw "Artifact role is missing, duplicated, or unsupported: $role"
        }
        $path = Resolve-BoundedArtifactPath $full `
            ([string](Get-Stage5JsonValue $entry 'path' 'Artifact entry'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Installed artifact is missing: $path"
        }
        $expected = ([string](Get-Stage5JsonValue $entry 'sha256' 'Artifact entry')).ToUpperInvariant()
        $actual = Get-UpperSha256 $path
        if ($expected -notmatch '^[0-9A-F]{64}$' -or $actual -cne $expected) {
            throw "Installed artifact hash mismatch for role $role."
        }
        $resolved[$role] = [pscustomobject]@{ path = $path; sha256 = $actual }
    }
    foreach ($role in $requiredRoles) {
        if (-not $resolved.ContainsKey($role)) { throw "Missing artifact role: $role" }
    }
    return [pscustomobject]@{
        path = $full
        sha256 = Get-UpperSha256 $full
        artifacts = $resolved
    }
}

function Wait-ForLeaf {
    param([string]$Path, [Diagnostics.Process]$Process, [datetime]$Deadline)
    while ([datetime]::UtcNow -lt $Deadline) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) { return }
        if ($Process.HasExited) {
            throw "Peer process $($Process.Id) exited before publishing $Path (exit $($Process.ExitCode))."
        }
        Start-Sleep -Milliseconds 20
    }
    throw "Timed out waiting for peer process $($Process.Id) to publish $Path."
}

function Write-AtomicText {
    param([string]$Path, [string]$Text)
    $temporary = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
    [IO.File]::WriteAllText($temporary, $Text, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::Move($temporary, $Path)
}

function Stop-TaskPeer {
    param([Diagnostics.Process]$Process)
    if ($null -eq $Process) { return }
    try {
        $shouldTerminate = -not $Process.HasExited
        if ($shouldTerminate) {
            try {
                # Keep the original Process object. Reacquiring by PID here
                # could terminate an unrelated process after PID reuse.
                $Process.Kill()
            }
            catch [InvalidOperationException] {
                # The peer may have exited between HasExited and Kill. If it
                # is still alive, preserve the termination failure.
                if (-not $Process.HasExited) { throw }
            }
        }
        if (-not $Process.WaitForExit($PostKillWaitMilliseconds)) {
            throw "Peer process $($Process.Id) remained alive after bounded cleanup."
        }
    }
    finally {
        $Process.Dispose()
    }
}

if ($SourceCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'SourceCommit must be the exact independently supplied lowercase 40-hex revision.'
}
$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputFull) {
    throw "Installed NET3 evidence output must be a fresh task-owned directory: $outputFull"
}
if ($outputFull.Contains(';')) {
    throw 'The scoped validation output path cannot contain semicolons.'
}
[IO.Directory]::CreateDirectory($outputFull) | Out-Null
$rawRoot = Join-Path $outputFull 'Net3Raw'
[IO.Directory]::CreateDirectory($rawRoot) | Out-Null

$artifactSet = Read-AndValidateArtifactSet $ArtifactSetManifestPath $SourceCommit
$generalsFull = [IO.Path]::GetFullPath($GeneralsExecutable)
$zeroHourFull = [IO.Path]::GetFullPath($ZeroHourExecutable)
if ($generalsFull -cne $artifactSet.artifacts['generals-executable'].path -or
    $zeroHourFull -cne $artifactSet.artifacts['zerohour-executable'].path) {
    throw 'Requested executables must be the exact independently hashed installed artifact-set executables.'
}
$executables = [ordered]@{
    Generals = $artifactSet.artifacts['generals-executable'].sha256
    ZeroHour = $artifactSet.artifacts['zerohour-executable'].sha256
}
$titleExecutables = @{ Generals = $generalsFull; ZeroHour = $zeroHourFull }
$topologies = @(
    [pscustomobject]@{ id = 'two-peer-1-v-16'; workers = @('1', '16') },
    [pscustomobject]@{ id = 'two-peer-2-v-auto'; workers = @('2', 'auto') },
    [pscustomobject]@{ id = 'two-peer-4-v-8'; workers = @('4', '8') },
    [pscustomobject]@{ id = 'four-peer-mixed-workers'; workers = @('1', '2', '8', 'auto') }
)
$kernelNames = @('physics', 'status', 'collision', 'ai-planning', 'spatial', 'path')
$kernelBits = @(1, 2, 4, 8, 16, 32)
$matches = @()
$rawIndexEntries = @()
$buildCrcs = @{}
$contentCrcs = @{}

foreach ($title in @('Generals', 'ZeroHour')) {
    $executable = $titleExecutables[$title]
    foreach ($topologyIndex in 0..($topologies.Count - 1)) {
        $topology = $topologies[$topologyIndex]
        foreach ($seed in @(23063, 49374)) {
            $recordId = "$title/$($topology.id)/$seed"
            $sessionLeaf = ($recordId -replace '/', '-')
            $sessionDirectory = Join-Path $rawRoot $sessionLeaf
            [IO.Directory]::CreateDirectory($sessionDirectory) | Out-Null
            $sessionBytes = New-Object byte[] 32
            $sessionGenerator = [Security.Cryptography.RandomNumberGenerator]::Create()
            try {
                $sessionGenerator.GetBytes($sessionBytes)
            }
            finally {
                $sessionGenerator.Dispose()
            }
            $sessionToken = ([BitConverter]::ToString($sessionBytes) -replace '-', '')
            $processes = @()
            try {
                for ($peerIndex = 0; $peerIndex -lt $topology.workers.Count; ++$peerIndex) {
                    $worker = $topology.workers[$peerIndex]
                    $configuration = 'case={0};seed={1};peer={2};peers={3};workers={4};session={5};dir={6};source={7};exe={8};artifact={9}' -f `
                        $topologyIndex, $seed, $peerIndex, $topology.workers.Count,
                        $worker, $sessionToken, $sessionDirectory, $SourceCommit,
                        $executables[$title], $artifactSet.sha256
                    $stdout = Join-Path $sessionDirectory "stdout-$peerIndex.log"
                    $stderr = Join-Path $sessionDirectory "stderr-$peerIndex.log"
                    $arguments = '-installedNet3Validation "{0}"' -f $configuration
                    $process = Start-Process -FilePath $executable -ArgumentList $arguments `
                        -WorkingDirectory (Split-Path -Parent $executable) -PassThru `
                        -WindowStyle Hidden -RedirectStandardOutput $stdout `
                        -RedirectStandardError $stderr
                    $processes += [pscustomobject]@{
                        process = $process; peerIndex = $peerIndex; worker = $worker
                        stdout = $stdout; stderr = $stderr
                    }
                }

                $deadline = [datetime]::UtcNow.AddSeconds($PeerTimeoutSeconds)
                foreach ($entry in $processes) {
                    $process = $entry.process
                    $pidPath = Join-Path $sessionDirectory "pid-$($entry.peerIndex).txt"
                    Wait-ForLeaf $pidPath $process $deadline
                    # Refresh the retained handle instead of looking the PID
                    # up again; a reused PID must never proxy peer identity.
                    $process.Refresh()
                    if ($process.HasExited) {
                        throw "Peer process $($process.Id) exited before identity observation."
                    }
                    $observedPath = [IO.Path]::GetFullPath($process.Path)
                    if ($observedPath -cne $executable) {
                        throw "Peer PID $($process.Id) is not the requested installed executable."
                    }
                    $observedExecutableHash = Get-UpperSha256 $observedPath
                    if ($observedExecutableHash -cne $executables[$title]) {
                        throw "Peer PID $($process.Id) executable changed after process creation."
                    }
                    # Revalidate every artifact for every live peer observation. This
                    # prevents one title/process record from proxying another artifact set.
                    $peerArtifactSet = Read-AndValidateArtifactSet `
                        $ArtifactSetManifestPath $SourceCommit
                    if ($peerArtifactSet.sha256 -cne $artifactSet.sha256) {
                        throw "Artifact set changed while observing peer PID $($process.Id)."
                    }
                    $pidText = Get-Content -LiteralPath $pidPath -Raw
                    $expectedPidText = "PID=$($process.Id)`nEXE=$observedExecutableHash`nARTIFACT=$($artifactSet.sha256)`n"
                    if (($pidText -replace "`r`n", "`n") -cne $expectedPidText) {
                        throw "Peer PID $($process.Id) did not bind its own process/artifact identity."
                    }
                    $observationPath = Join-Path $sessionDirectory `
                        "observed-$($entry.peerIndex).txt"
                    Write-AtomicText $observationPath $expectedPidText
                }

                foreach ($entry in $processes) {
                    $remaining = [Math]::Max(1, [int]($deadline - [datetime]::UtcNow).TotalMilliseconds)
                    if (-not $entry.process.WaitForExit($remaining)) {
                        throw "Peer PID $($entry.process.Id) exceeded the bounded validation timeout."
                    }
                    if ($entry.process.ExitCode -ne 0) {
                        $errorText = if (Test-Path -LiteralPath $entry.stderr) {
                            Get-Content -LiteralPath $entry.stderr -Raw
                        } else { '' }
                        throw "Peer PID $($entry.process.Id) failed with exit $($entry.process.ExitCode): $errorText"
                    }
                }

                $peerRecords = @()
                $referenceFrame = $null
                $referenceCrc = $null
                $referenceRoster = $null
                foreach ($entry in $processes) {
                    $rawPath = Join-Path $sessionDirectory "peer-$($entry.peerIndex).json"
                    if (-not (Test-Path -LiteralPath $rawPath -PathType Leaf)) {
                        throw "Peer PID $($entry.process.Id) did not publish raw validation output."
                    }
                    $raw = ConvertFrom-Stage5JsonDictionary $rawPath
                    $requiredRaw = @('schemaVersion', 'producer', 'validationMode',
                        'kernelFixture', 'processId', 'title', 'caseIndex', 'seed', 'ordinal',
                        'peerCount', 'sourceCommit', 'executableSha256',
                        'artifactSetSha256', 'buildCompatibilityCrc', 'contentCrc',
                        'requestedWorkers', 'effectiveWorkers', 'networkHelloReady',
                        'rosterExact', 'rosterSha256', 'policyMask', 'finalFrame',
                        'finalCRC', 'cleanShutdown', 'kernels')
                    Assert-Stage5JsonShape $raw $requiredRaw "Raw NET3 peer $recordId/$($entry.peerIndex)"
                    if ($raw.schemaVersion -ne 1 -or
                        $raw.producer -cne 'installed-runtime-net3-peer-v1' -or
                        $raw.validationMode -cne 'scoped-net3-loopback-release-proof' -or
                        $raw.kernelFixture -cne 'actual-stage5-kernels-v1' -or
                        [int]$raw.processId -ne $entry.process.Id -or
                        $raw.title -cne $title -or [int]$raw.caseIndex -ne $topologyIndex -or
                        [int]$raw.seed -ne $seed -or [int]$raw.ordinal -ne $entry.peerIndex -or
                        [int]$raw.peerCount -ne $topology.workers.Count -or
                        $raw.sourceCommit -cne $SourceCommit -or
                        $raw.executableSha256 -cne $executables[$title] -or
                        $raw.artifactSetSha256 -cne $artifactSet.sha256 -or
                        $raw.requestedWorkers -cne $entry.worker -or
                        $raw.networkHelloReady -ne $true -or $raw.rosterExact -ne $true -or
                        [int]$raw.policyMask -ne 63 -or $raw.cleanShutdown -ne $true) {
                        throw "Raw NET3 peer identity/policy mismatch for $recordId/$($entry.peerIndex)."
                    }
                    if ($raw.kernels -isnot [Array] -or $raw.kernels.Count -ne 6) {
                        throw "Raw NET3 peer did not publish exactly six kernel records for $recordId/$($entry.peerIndex)."
                    }
                    for ($kernelIndex = 0; $kernelIndex -lt 6; ++$kernelIndex) {
                        $kernel = $raw.kernels[$kernelIndex]
                        $kernelContext = "Raw NET3 peer $recordId/$($entry.peerIndex) kernel $kernelIndex"
                        Assert-Stage5JsonShape $kernel @('name', 'bit', 'submitted',
                            'completed', 'physicalWorkerJobs', 'ownerHelpedJobs',
							'physicalWorkerMask', 'distinctPhysicalWorkers',
							'physicalWorkerMaskComplete', 'peakConcurrentPhysicalWorkers') $kernelContext
                        $submitted = Get-Stage5JsonValue $kernel 'submitted' $kernelContext
                        $completed = Get-Stage5JsonValue $kernel 'completed' $kernelContext
                        $physical = Get-Stage5JsonValue $kernel 'physicalWorkerJobs' $kernelContext
                        $ownerHelped = Get-Stage5JsonValue $kernel 'ownerHelpedJobs' $kernelContext
                        $physicalMask = Get-Stage5JsonValue $kernel 'physicalWorkerMask' $kernelContext
						$distinct = Get-Stage5JsonValue $kernel 'distinctPhysicalWorkers' $kernelContext
						$maskComplete = Get-Stage5JsonValue $kernel 'physicalWorkerMaskComplete' $kernelContext
                        $peak = Get-Stage5JsonValue $kernel 'peakConcurrentPhysicalWorkers' $kernelContext
                        foreach ($counter in @($submitted, $completed, $physical, $ownerHelped,
                            $physicalMask, $distinct, $peak)) {
                            if (-not (Test-Stage5JsonInteger $counter) -or [Int64]$counter -lt 0) {
                                throw "$kernelContext contains a non-integer or negative counter."
                            }
                        }
                        if ((Get-Stage5JsonValue $kernel 'name' $kernelContext) -cne
                                $kernelNames[$kernelIndex] -or
                            (Get-Stage5JsonValue $kernel 'bit' $kernelContext) -ne
                                $kernelBits[$kernelIndex] -or
                            [UInt64]$submitted -ne [UInt64]$completed -or
							[UInt64]$physical + [UInt64]$ownerHelped -ne [UInt64]$completed -or
							$maskComplete -isnot [bool] -or [UInt64]$distinct -lt
								(Get-Stage5UInt64BitCount ([UInt64]$physicalMask)) -or
							($maskComplete -and [UInt64]$distinct -ne
								(Get-Stage5UInt64BitCount ([UInt64]$physicalMask)))) {
                            throw "$kernelContext identity or execution accounting is invalid."
                        }
                        if ([int]$raw.effectiveWorkers -eq 1) {
                            if ([UInt64]$submitted -ne 0 -or [UInt64]$physical -ne 0 -or
                                [UInt64]$ownerHelped -ne 0 -or [UInt64]$physicalMask -ne 0 -or
                                [UInt64]$distinct -ne 0 -or [UInt64]$peak -ne 0) {
                                throw "$kernelContext forced-one counters must all be zero."
                            }
                        }
						elseif ([UInt64]$submitted -eq 0 -or [UInt64]$physical -eq 0 -or
							[UInt64]$physical -ne [UInt64]$completed -or
							[UInt64]$ownerHelped -ne 0 -or [UInt64]$distinct -le 1 -or
                            [UInt64]$distinct -gt [UInt64]$raw.effectiveWorkers -or
                            [UInt64]$peak -le 1 -or
                            [UInt64]$peak -gt [UInt64]$raw.effectiveWorkers) {
                            throw "$kernelContext does not prove concurrent physical execution."
                        }
                    }
                    if (-not $buildCrcs.ContainsKey($title)) {
                        $buildCrcs[$title] = [UInt32]$raw.buildCompatibilityCrc
                        $contentCrcs[$title] = [UInt32]$raw.contentCrc
                    }
                    elseif ([UInt32]$raw.buildCompatibilityCrc -ne $buildCrcs[$title] -or
                        [UInt32]$raw.contentCrc -ne $contentCrcs[$title]) {
                        throw "Installed build/content CRC changed across $title peers."
                    }
                    if ($null -eq $referenceFrame) {
                        $referenceFrame = [UInt64]$raw.finalFrame
                        $referenceCrc = [string]$raw.finalCRC
                        $referenceRoster = [string]$raw.rosterSha256
                    }
                    elseif ([UInt64]$raw.finalFrame -ne $referenceFrame -or
                        [string]$raw.finalCRC -cne $referenceCrc -or
                        [string]$raw.rosterSha256 -cne $referenceRoster) {
                        throw "NET3 peer trace/roster mismatch for $recordId."
                    }
                    $rawRelative = Get-RelativeEvidencePath $outputFull $rawPath
                    $rawHash = Get-UpperSha256 $rawPath
                    $rawIndexEntries += [pscustomobject]@{ path = $rawRelative; sha256 = $rawHash }
                    $peerRecords += [ordered]@{
                        ordinal = $entry.peerIndex
                        processId = $entry.process.Id
                        observedExecutableSha256 = $executables[$title]
                        observedArtifactSetSha256 = $artifactSet.sha256
                        rawOutputPath = $rawRelative
                        rawOutputSha256 = $rawHash
                        requestedWorkers = $entry.worker
                        effectiveWorkers = [int]$raw.effectiveWorkers
                        networkHelloReady = $true
                        rosterExact = $true
                        rosterSha256 = [string]$raw.rosterSha256
                        policyMask = 63
                        finalFrame = [UInt64]$raw.finalFrame
                        finalCRC = [string]$raw.finalCRC
                        exitCode = 0
                        cleanShutdown = $true
                        kernels = @($raw.kernels)
                    }
                }
                $matches += [ordered]@{
                    recordId = $recordId
                    sourceCommit = $SourceCommit
                    title = $title
                    executableSha256 = $executables[$title]
                    artifactSetSha256 = $artifactSet.sha256
                    topologyId = $topology.id
                    seed = $seed
                    networkHelloReady = $true
                    rosterExact = $true
                    rosterSha256 = $referenceRoster
                    policyMask = 63
                    peers = $peerRecords
                }
            }
            finally {
				$cleanupFailures = New-Object 'Collections.Generic.List[string]'
				foreach ($entry in $processes) {
					try {
						Stop-TaskPeer $entry.process
					}
					catch {
						$cleanupFailures.Add("peer $($entry.peerIndex): $($_.Exception.Message)") | Out-Null
					}
				}
				if ($cleanupFailures.Count -ne 0) {
					throw "Installed NET3 peer cleanup failed: $($cleanupFailures -join '; ')"
				}
            }
        }
    }
}

if ($matches.Count -ne 16 -or $rawIndexEntries.Count -ne 40 -or
    $buildCrcs.Count -ne 2 -or $contentCrcs.Count -ne 2) {
    throw 'Runner did not produce the canonical 16-match/40-peer matrix.'
}
$evidencePath = Join-Path $outputFull 'Net3LoopbackEvidence.json'
$evidence = [ordered]@{
    schemaVersion = 1
    evidenceKind = 'installed-net3-loopback'
    status = 'passed'
    producer = 'installed-runtime-runner-v1'
    validationMode = 'scoped-net3-loopback-release-proof'
    installedRuntime = $true
    independentProcessHashing = $true
    sourceCommit = $SourceCommit
    artifactSetSha256 = $artifactSet.sha256
    supportedKernelMask = 63
    policySchema = 1
    engineEpoch = 1
    determinismEpoch = 1
    buildCompatibilityCrc = [ordered]@{
        Generals = $buildCrcs.Generals; ZeroHour = $buildCrcs.ZeroHour
    }
    contentCrc = [ordered]@{
        Generals = $contentCrcs.Generals; ZeroHour = $contentCrcs.ZeroHour
    }
    executables = $executables
    fixedSeeds = @(23063, 49374)
    matches = $matches
}
[IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 20),
    (New-Object Text.UTF8Encoding($false)))

$rawIndexPath = Join-Path $outputFull 'MultiplayerSimulationRawEvidence.index'
$indexLines = @('RTS_MULTIPLAYER_SIMULATION_RAW_EVIDENCE_V1')
for ($index = 0; $index -lt $rawIndexEntries.Count; ++$index) {
    $indexLines += ('{0:D2}|{1}|{2}' -f $index,
        $rawIndexEntries[$index].path, $rawIndexEntries[$index].sha256)
}
$indexLines += 'END'
[IO.File]::WriteAllText($rawIndexPath, (($indexLines -join "`n") + "`n"),
    (New-Object Text.UTF8Encoding($false)))

# Re-run the strict acceptance parser against independent arguments before any
# proof is emitted. The runner does not infer authority from its own JSON.
Read-Stage5Net3LoopbackEvidence -Path $evidencePath `
    -ExpectedSourceCommit $SourceCommit `
    -ExpectedArtifactSetSha256 $artifactSet.sha256 `
    -ExpectedGeneralsExecutableSha256 $executables.Generals `
    -ExpectedZeroHourExecutableSha256 $executables.ZeroHour `
    -ExpectedGeneralsBuildCompatibilityCrc $buildCrcs.Generals `
    -ExpectedZeroHourBuildCompatibilityCrc $buildCrcs.ZeroHour `
    -ExpectedGeneralsContentCrc $contentCrcs.Generals `
    -ExpectedZeroHourContentCrc $contentCrcs.ZeroHour | Out-Null

$artifactBundlePath = Join-Path $outputFull 'Stage5ArtifactSet.json'
Copy-Item -LiteralPath $artifactSet.path -Destination $artifactBundlePath
$proofBundles = Join-Path $outputFull 'ProofBundles'
foreach ($title in @('Generals', 'ZeroHour')) {
    $bundle = Join-Path $proofBundles $title
    [IO.Directory]::CreateDirectory($bundle) | Out-Null
    Copy-Item -LiteralPath $evidencePath -Destination (Join-Path $bundle `
        'Net3LoopbackEvidence.json')
    Copy-Item -LiteralPath $rawIndexPath -Destination (Join-Path $bundle `
        'MultiplayerSimulationRawEvidence.index')
    Copy-Item -LiteralPath $artifactBundlePath -Destination (Join-Path $bundle `
        'Stage5ArtifactSet.json')
    Copy-Item -LiteralPath $rawRoot -Destination (Join-Path $bundle 'Net3Raw') `
        -Recurse
    $proofPath = Join-Path $bundle 'MultiplayerSimulationRuntimeProof.txt'
    $buildCrc = if ($title -ceq 'Generals') { $buildCrcs.Generals } `
        else { $buildCrcs.ZeroHour }
    $contentCrc = if ($title -ceq 'Generals') { $contentCrcs.Generals } `
        else { $contentCrcs.ZeroHour }
    & (Join-Path $PSScriptRoot 'New-MultiplayerSimulationReleaseProof.ps1') `
        -EvidenceManifestPath (Join-Path $bundle 'Net3LoopbackEvidence.json') `
        -RawEvidenceIndexPath (Join-Path $bundle 'MultiplayerSimulationRawEvidence.index') `
        -ArtifactSetManifestPath (Join-Path $bundle 'Stage5ArtifactSet.json') `
        -OutputPath $proofPath -ExpectedSourceCommit $SourceCommit `
        -ExpectedArtifactSetSha256 $artifactSet.sha256 `
        -ExpectedGeneralsExecutableSha256 $executables.Generals `
        -ExpectedZeroHourExecutableSha256 $executables.ZeroHour `
        -Title $title -ExpectedBuildCompatibilityCrc $buildCrc `
        -ExpectedContentCrc $contentCrc | Out-Null
}

Write-Output "Installed NET3 runner passed exact matrix: matches=16 peers=40 evidence=$evidencePath proofBundles=$proofBundles"
