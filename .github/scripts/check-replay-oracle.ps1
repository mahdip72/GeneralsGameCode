param(
    [Parameter(Mandatory = $true)][string]$ExecutablePath,
    [Parameter(Mandatory = $true)][string]$UserdataPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Keep this list explicit: a missing or changed corpus must not silently reduce coverage.
$expectedReplays = @(
    '!Golden Replay #1.rep',
    '00-03-45_2v6_PC03_ss_HardAI_HardAI_HardAI_HardAI_HardAI_HardAI.rep',
    '00-31-22_2v2_Derky_DESKTOPJ_HardAI_HardAI.rep',
    '00-41-30_2v2_Nic_BOMD2MAS_HardAI_HardAI.rep',
    '05-01-50_2v2_amoor123_beshr_HardAI_HardAI.rep',
    '11-25-57_2v2_Kana_HardAI_Erbolat_Hulk.rep',
    '12-11-35_2v2_babai_ILnur_HardAI_HardAI.rep',
    '15-07-24_2v2v2_Emkill_haker_HardAI_HardAI_HardAI_HardAI.rep',
    '18-13-02_3v3_Supremac_Loonen_JB_HardAI_HardAI_HardAI.rep',
    '366648.rep'
)
$stressReplay = '00-03-45_2v6_PC03_ss_HardAI_HardAI_HardAI_HardAI_HardAI_HardAI.rep'
$timeoutSeconds = 10 * 60
$failurePattern = '(?i)(REPLAY_FAIL|CRC[_ ]MISMATCH|DESYNC|ASSERT(?:ION)?(?:\s+FAILED|:)|DEBUG_CRASH|ACCESS VIOLATION|OWNERSHIP(?:\s+FAILURE)?|CANNOT OPEN REPLAY|MISSING MAP|MAP NOT FOUND|FAILED TO LOAD MAP)'

$exe = (Resolve-Path -LiteralPath $ExecutablePath).Path
$workingDirectory = Split-Path -Parent $exe
$fixtureRoot = (Resolve-Path -LiteralPath $UserdataPath).Path
$replaySource = Join-Path $fixtureRoot 'Replays'
$mapSource = Join-Path $fixtureRoot 'Maps'
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$documents = [Environment]::GetFolderPath('MyDocuments')
if (-not $documents -or -not (Test-Path -LiteralPath $replaySource -PathType Container) -or
    -not (Test-Path -LiteralPath $mapSource -PathType Container)) {
    throw 'The hosted replay profile or corpus is unavailable.'
}

$actualReplays = @(Get-ChildItem -LiteralPath $replaySource -File -Filter '*.rep' | ForEach-Object Name)
if ($expectedReplays.Count -ne 10 -or ($expectedReplays | Select-Object -Unique).Count -ne 10 -or
    $actualReplays.Count -ne 10 -or @(Compare-Object $expectedReplays $actualReplays).Count -ne 0) {
    throw "Replay corpus differs from the ten-fixture manifest. Found: $($actualReplays -join ', ')"
}
if (@(Get-ChildItem -LiteralPath $mapSource -Force).Count -eq 0) {
    throw 'The replay map corpus is empty.'
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$registryPath = 'SOFTWARE\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour'
$registryBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
$registryKey = $registryBase.CreateSubKey($registryPath)
if (-not $registryKey) { throw 'Could not open the hosted Zero Hour user-data registry key.' }
$hadLeaf = $registryKey.GetValueNames() -contains 'UserDataLeafName'
if ($hadLeaf) {
    $originalLeaf = $registryKey.GetValue('UserDataLeafName')
    $originalLeafKind = $registryKey.GetValueKind('UserDataLeafName')
}

$runs = [Collections.Generic.List[object]]::new()
$stressManifests = [Collections.Generic.List[string[]]]::new()
$runNumber = 0
try {
    foreach ($replay in $expectedReplays) {
        $repetitions = if ($replay -ceq $stressReplay) { 3 } else { 1 }
        for ($repeat = 1; $repeat -le $repetitions; $repeat++) {
            $runNumber++
            $runId = 'run-{0:D2}' -f $runNumber
            $leaf = "CodexReplayOracle\$runId"
            $profile = Join-Path $documents $leaf
            $profileReplays = Join-Path $profile 'Replays'
            $profileMaps = Join-Path $profile 'Maps'
            $runDirectory = Join-Path $outputRoot $runId
            $crcDirectory = Join-Path $runDirectory 'crc'
            New-Item -ItemType Directory -Path $profileReplays, $profileMaps, $crcDirectory -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $replaySource $replay) -Destination $profileReplays
            Copy-Item -Path (Join-Path $mapSource '*') -Destination $profileMaps -Recurse -Force
            $registryKey.SetValue('UserDataLeafName', $leaf, [Microsoft.Win32.RegistryValueKind]::String)

            # Debug logs use a fixed name in the executable directory; retain each run's copy.
            Get-ChildItem -LiteralPath $workingDirectory -File -Filter 'DebugLogFile*.txt' |
                Remove-Item -Force
            $stdout = Join-Path $runDirectory 'stdout.log'
            $stderr = Join-Path $runDirectory 'stderr.log'
            $arguments = '-headless'
            if ($replay -ceq $stressReplay) {
                $arguments += " -SaveDebugCRCPerFrame `"$crcDirectory`""
            }
            $arguments += " -replay `"$replay`""
            Write-Host "Replay $runNumber/12 : $replay (attempt $repeat/$repetitions)"
            $started = Get-Date
            $process = Start-Process -FilePath $exe -WorkingDirectory $workingDirectory `
                -ArgumentList $arguments -RedirectStandardOutput $stdout `
                -RedirectStandardError $stderr -PassThru
            $exited = $process.WaitForExit($timeoutSeconds * 1000)
            if (-not $exited) {
                Stop-Process -Id $process.Id -Force
                $process.WaitForExit()
            }
            $process.Refresh()
            $elapsedSeconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 2)
            $debugLogs = @(Get-ChildItem -LiteralPath $workingDirectory -File -Filter 'DebugLogFile*.txt')
            foreach ($log in $debugLogs) {
                Copy-Item -LiteralPath $log.FullName -Destination $runDirectory -Force
            }
            $logFiles = @($stdout, $stderr) + @($debugLogs | ForEach-Object { Join-Path $runDirectory $_.Name })
            $stdoutText = Get-Content -LiteralPath $stdout -Raw
            $validStart = $stdoutText -and $stdoutText.Contains('Simulating Replay "' + $replay + '"')
            $validEnd = $stdoutText -and $stdoutText.Contains('Elapsed Time:')
            $failureLog = Select-String -LiteralPath $logFiles -Pattern $failurePattern -List |
                Select-Object -First 1
            $failure = if (-not $exited) { 'timeout' }
                elseif ($process.ExitCode -ne 0) { "exit $($process.ExitCode)" }
                elseif (-not $validStart) { 'missing replay-start marker' }
                elseif (-not $validEnd) { 'missing replay-completion marker' }
                elseif ($failureLog) { "failure log marker: $($failureLog.Matches[0].Value)" }
                else { $null }

            $crcCount = 0
            $manifestHash = $null
            if ($replay -ceq $stressReplay) {
                $crcFiles = @(Get-ChildItem -LiteralPath $crcDirectory -File -Filter 'DebugFrame_*.txt' |
                    Sort-Object Name)
                $crcCount = $crcFiles.Count
                if ($crcCount -eq 0) { $failure = 'stress replay produced no per-frame CRC files' }
                $manifest = @($crcFiles | ForEach-Object {
                    '{0} {1}' -f $_.Name, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
                })
                $manifestPath = Join-Path $runDirectory 'crc-manifest.txt'
                [IO.File]::WriteAllLines($manifestPath, [string[]]$manifest)
                $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
                $stressManifests.Add([string[]]$manifest)
            }

            $runs.Add([pscustomobject]@{
                run = $runId
                fixture = $replay
                attempt = $repeat
                exitCode = $process.ExitCode
                durationSeconds = $elapsedSeconds
                crcFileCount = $crcCount
                crcManifestSha256 = $manifestHash
                failure = $failure
            })
            $runs | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputRoot 'results.json')
            if ($failure) { throw "Replay $runId failed: $failure" }
        }
    }

    if ($runNumber -ne 12 -or $stressManifests.Count -ne 3) {
        throw 'Replay coverage did not reach ten unique fixtures and twelve processes.'
    }
    $reference = $stressManifests[0]
    for ($i = 1; $i -lt $stressManifests.Count; $i++) {
        $candidate = $stressManifests[$i]
        if ($reference.Count -ne $candidate.Count) {
            throw 'Repeated stress replays produced different CRC file sets.'
        }
        for ($j = 0; $j -lt $reference.Count; $j++) {
            if ($reference[$j] -cne $candidate[$j]) {
                throw 'Repeated stress replays produced different CRC file bytes.'
            }
        }
    }
    Write-Host 'PASS: ten unique replay fixtures, twelve successful processes, three byte-identical stress CRC manifests.'
}
finally {
    if ($hadLeaf) {
        $registryKey.SetValue('UserDataLeafName', $originalLeaf, $originalLeafKind)
    }
    else {
        $registryKey.DeleteValue('UserDataLeafName', $false)
    }
    $registryKey.Dispose()
    $registryBase.Dispose()
}
