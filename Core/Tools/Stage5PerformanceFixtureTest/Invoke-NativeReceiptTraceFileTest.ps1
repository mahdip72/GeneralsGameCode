# Runs only one existing native test utility and preserves each unique run child.
[CmdletBinding()]
param(
    [string]$UtilityPath = '',
    [string]$Group = 'basic'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-HeldFileTestInput {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-ConcreteLocalTestPath {
    param([string]$Value, [string]$Description)
    Assert-HeldFileTestInput (-not [string]::IsNullOrWhiteSpace($Value)) "$Description is required."
    # CMake commonly supplies slash-separated absolute paths. This wrapper's
    # trusted path normalization is not the native receipt's qualification API.
    $candidate = $Value.Replace('/', '\').TrimEnd('\')
    Assert-HeldFileTestInput ($candidate -cmatch '^[A-Za-z]:\\' -and
        $candidate.Length -gt 3 -and $candidate.Length -lt 260 -and
        $candidate -cnotmatch '[^\x20-\x7e]' -and $candidate.Substring(2) -notmatch '[:*?"<>|]') `
        "$Description must be a bounded ASCII local absolute non-root DOS path."
    foreach ($component in $candidate.Substring(3).Split('\')) {
        Assert-HeldFileTestInput ($component.Length -gt 0 -and $component -cne '.' -and
            $component -cne '..' -and -not $component.EndsWith(' ') -and -not $component.EndsWith('.')) `
            "$Description contains a non-concrete component."
    }
    $full = [IO.Path]::GetFullPath($candidate)
    Assert-HeldFileTestInput ([string]::Equals($full, $candidate, [StringComparison]::OrdinalIgnoreCase)) `
        "$Description must not rely on ambient path resolution."
    return $full
}

function Assert-ExistingTestPathNoReparse {
    param([string]$Path, [bool]$Directory)
    $cursor = [IO.Path]::GetPathRoot($Path)
    $components = @($Path.Substring($cursor.Length).Split('\'))
    for ($index = -1; $index -lt $components.Count; ++$index) {
        if ($index -ge 0) { $cursor = Join-Path $cursor $components[$index] }
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
        $expectDirectory = ($index -lt $components.Count - 1) -or $Directory
        Assert-HeldFileTestInput ($item.PSProvider.Name -ceq 'FileSystem' -and
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
            [bool]$item.PSIsContainer -eq $expectDirectory) `
            "Existing path component is reparse, missing or has the wrong type: $cursor"
    }
}

$nativeTestProcess = $null
$nativeTestStarted = $false
$nativeTestExit = 2
$nativeTestRunRoot = $null
$nativeTestStdout = $null
$nativeTestStderr = $null

try {
    Assert-HeldFileTestInput ($env:OS -ceq 'Windows_NT') 'Held-file tests require Windows.'
    Assert-HeldFileTestInput (@('basic', 'aliases') -ccontains $Group) 'Group must be exactly basic or aliases.'
    $nativeTestExecutable = Get-ConcreteLocalTestPath $UtilityPath 'UtilityPath'
    $allowedUtilities = @('g_skirmish_ai_runner_contract_tests.exe', 'z_runtime_regression_tests.exe')
    Assert-HeldFileTestInput ($allowedUtilities -ccontains [IO.Path]::GetFileName($nativeTestExecutable)) `
        'Only the two existing native regression utility executables are allowed; never a game/launcher or arbitrary command.'
    Assert-ExistingTestPathNoReparse $nativeTestExecutable $false

    # Read the explicit parent once. No TEMP, cwd, build-directory or RUNNER_TEMP
    # fallback, no parent creation and no second mutable environment selection.
    $nativeTestScratchSelection = [Environment]::GetEnvironmentVariable('RTS_STAGE5_VALIDATION_SCRATCH_ROOT', 'Process')
    $nativeTestScratchParent = Get-ConcreteLocalTestPath $nativeTestScratchSelection 'RTS_STAGE5_VALIDATION_SCRATCH_ROOT'
    Assert-ExistingTestPathNoReparse $nativeTestScratchParent $true
    $nativeTestVolumeRoot = [IO.Path]::GetPathRoot($nativeTestScratchParent)
    $nativeTestVolume = New-Object IO.DriveInfo -ArgumentList $nativeTestVolumeRoot
    Assert-HeldFileTestInput ($nativeTestVolume.IsReady -and
        $nativeTestVolume.DriveType -eq [IO.DriveType]::Fixed -and
        $nativeTestVolume.DriveFormat -ceq 'NTFS') 'Scratch parent must be on an existing local fixed NTFS volume.'

    $nativeTestRunRoot = Join-Path $nativeTestScratchParent ('hf-' + [Guid]::NewGuid().ToString('N'))
    Assert-HeldFileTestInput ($nativeTestRunRoot.Length + 90 -lt 260 -and
        [string]::Equals([IO.Path]::GetDirectoryName($nativeTestRunRoot), $nativeTestScratchParent,
            [StringComparison]::OrdinalIgnoreCase)) 'Fresh child must fit the native fixture path budget and remain a direct child.'
    # No Force/reuse/retry. Even an improbable GUID collision fails closed.
    New-Item -ItemType Directory -Path $nativeTestRunRoot -ErrorAction Stop | Out-Null
    Assert-ExistingTestPathNoReparse $nativeTestRunRoot $true
    $nativeTestStdout = Join-Path $nativeTestRunRoot 'stdout.log'
    $nativeTestStderr = Join-Path $nativeTestRunRoot 'stderr.log'
    $nativeTestSelector = if ($Group -ceq 'basic') { '--performance-receipt-files' } else { '--performance-receipt-file-aliases' }
    $nativeTestArguments = $nativeTestSelector + ' "' + $nativeTestRunRoot + '"'
    Write-Output "HELD-FILE TEST group=$Group retainedRoot=$nativeTestRunRoot utility=$nativeTestExecutable"

    # This fresh wrapper process owns only these temporary process-environment
    # changes. Child scratch stays explicit; prior values are restored exactly.
    $nativeTestPriorTemp = [Environment]::GetEnvironmentVariable('TEMP', 'Process')
    $nativeTestPriorTmp = [Environment]::GetEnvironmentVariable('TMP', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('TEMP', $nativeTestRunRoot, 'Process')
        [Environment]::SetEnvironmentVariable('TMP', $nativeTestRunRoot, 'Process')
        $nativeTestProcess = Start-Process -FilePath $nativeTestExecutable -ArgumentList $nativeTestArguments `
            -WorkingDirectory $nativeTestRunRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $nativeTestStdout -RedirectStandardError $nativeTestStderr
        $nativeTestStarted = $true
        # Keep and use this original process object/handle, never a PID lookup.
        $null = $nativeTestProcess.Handle
    }
    finally {
        [Environment]::SetEnvironmentVariable('TEMP', $nativeTestPriorTemp, 'Process')
        [Environment]::SetEnvironmentVariable('TMP', $nativeTestPriorTmp, 'Process')
    }

    # Leave margin for wrapper startup, logs and the bounded post-kill wait inside
    # the parent's 30-second CTest. This is a utility timeout, not a game gate.
    if (-not $nativeTestProcess.WaitForExit(25000)) {
        [Console]::Error.WriteLine('FAIL: held-file utility exceeded its 25-second execution budget.')
        $nativeTestExit = 1
    }
    else {
        # In particular, never convert native prerequisite 2 into skip/success.
        $nativeTestObservedExit = $nativeTestProcess.ExitCode
        Assert-HeldFileTestInput ($null -ne $nativeTestObservedExit -and $nativeTestObservedExit -is [int]) `
            'The original utility exited but its exact exit code was not observed.'
        $nativeTestExit = $nativeTestObservedExit
        Write-Output "HELD-FILE TEST nativeExit=$nativeTestExit"
    }
}
catch {
    [Console]::Error.WriteLine('PREREQUISITE: ' + $_.Exception.Message)
    $nativeTestExit = 2
}
finally {
    if ($null -ne $nativeTestProcess) {
        try {
            if ($nativeTestStarted -and -not $nativeTestProcess.WaitForExit(0)) {
                try { $nativeTestProcess.Kill() }
                catch {
                    # A process that just exited needs no kill. Any other failure
                    # is actionable; never target a new process by its old PID.
                    if (-not $nativeTestProcess.WaitForExit(0)) { throw }
                }
                if (-not $nativeTestProcess.WaitForExit(2000)) {
                    throw "Original utility did not exit after bounded termination; PID=$($nativeTestProcess.Id). Do not retry this test."
                }
            }
        }
        catch {
            [Console]::Error.WriteLine('PREREQUISITE: utility cleanup not confirmed: ' + $_.Exception.Message)
            $nativeTestExit = 2
        }
        finally {
            try { $nativeTestProcess.Dispose() }
            catch { [Console]::Error.WriteLine('PREREQUISITE: original process handle disposal failed: ' + $_.Exception.Message); $nativeTestExit = 2 }
        }
    }
    foreach ($log in @($nativeTestStdout, $nativeTestStderr)) {
        if ($null -ne $log -and [IO.File]::Exists($log)) {
            # Full files remain in the unique child; keep CTest output bounded.
            Write-Output "HELD-FILE TEST log=$log"
            try { Get-Content -LiteralPath $log -Tail 160 -ErrorAction Stop | Write-Output }
            catch { [Console]::Error.WriteLine('PREREQUISITE: retained log unreadable: ' + $_.Exception.Message); $nativeTestExit = 2 }
        }
    }
    if ($null -ne $nativeTestRunRoot) { Write-Output "HELD-FILE TEST evidence preserved: $nativeTestRunRoot" }
}
exit $nativeTestExit
